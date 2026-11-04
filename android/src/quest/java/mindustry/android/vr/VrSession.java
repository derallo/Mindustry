package mindustry.android.vr;

import android.annotation.SuppressLint;
import arc.backend.android.*;
import android.app.Activity;
import android.opengl.EGL14;
import android.opengl.EGLContext;
import android.opengl.EGLDisplay;
import android.os.SystemClock;
import android.view.InputDevice;
import android.view.MotionEvent;
import arc.Core;
import arc.util.Log;

/**
 * Java half of the OpenXR bridge.
 *
 * Rendering stays entirely in libGDX: the game draws normally, this class copies the finished
 * frame into a texture the native XR thread presents on a head-tracked virtual screen. Hovering
 * the controller ray over that screen becomes a synthetic touch at the matching pixel, so every
 * existing touch control in the game keeps working.
 */
public final class VrSession{
    private static volatile boolean loadFailed, active, failed, sizeWarned, timeoutLogged, solidDebug, lookAround;
    private static volatile long startedAt;
    private static final long INIT_TIMEOUT_MS = 10_000;
    /**
     * The activity owns the GL surface and outlives the VR session; Mindustry exits when it is
     * destroyed, and start() replaces this reference on every launch.
     */
    @SuppressLint("StaticFieldLeak")
    private static volatile Activity activity;
    private static int screenW, screenH;
    private static boolean pointerDown;
    private static long downTime;
    private static volatile float pendingZoom;
    private static volatile float moveAxisX, moveAxisY;
    private static volatile boolean viewToggleRequested;
    private static int lastX, lastY;

    private VrSession(){
    }

    private static boolean load(){
        if(loadFailed) return false;
        try{
            System.loadLibrary("mindustryvr");
            return true;
        }catch(Throwable t){
            loadFailed = true;
            Log.err("Could not load libmindustryvr", t);
            return false;
        }
    }

    /** Probe that also performs the JNI library load; safe to call before start(). */
    public static String runtimeVersion(){
        if(!load()) return "";
        try{
            return nativeRuntimeVersion();
        }catch(Throwable t){
            return "";
        }
    }

    /** Called on the game's GL thread once the surface exists. */
    public static boolean start(Activity act){
        if(active || failed) return active;
        if(!load()) return false;

        EGLDisplay display = EGL14.eglGetCurrentDisplay();
        EGLContext context = EGL14.eglGetCurrentContext();
        if(display == null || context == null || display.getNativeHandle() == 0 || context.getNativeHandle() == 0){
            failed = true;
            Log.err("VR: no current EGL context");
            return false;
        }

        activity = act;
        nativeSetSolid(solidDebug);
        nativeSetLookAround(lookAround);
        screenW = Core.graphics.getWidth();
        screenH = Core.graphics.getHeight();
        sizeWarned = false;
        timeoutLogged = false;

        try{
            active = nativeStart(act, display.getNativeHandle(), context.getNativeHandle(), screenW, screenH);
            startedAt = System.currentTimeMillis();
        }catch(Throwable t){
            Log.err("VR: native start failed", t);
            active = false;
        }

        if(!active){
            failed = true;
            Log.err("VR: unable to start OpenXR session: @", lastErrorSafe());
        }else{
            Log.info("VR: OpenXR session starting (@x@), status: @", screenW, screenH, status());

            // A tiny surface means Horizon OS ignored the fixed resolution; the quad would then
            // show a heavily upscaled image. Worth a loud, actionable log line.
            if(screenW < 640 || screenH < 360){
                Log.warn("VR: capture surface is only @x@; the virtual screen may look very blurry", screenW, screenH);
            }
        }
        return active;
    }

    /** Called every frame on the GL thread, after the game has drawn. */
    public static void capture(){
        if(!active) return;

        // A surface resize would make the captured region wrong; restart with the new size.
        int w = Core.graphics.getWidth(), h = Core.graphics.getHeight();
        if(w > 0 && h > 0 && (w != screenW || h != screenH)){
            if(!sizeWarned){
                sizeWarned = true;
                Log.info("VR: surface resized @x@ -> @x@, restarting session", screenW, screenH, w, h);
            }
            restart(activity);
            return;
        }

        if(isActive()) nativeCapture(screenW, screenH);
    }

    /** True while an OpenXR session is begun (frames flowing); false while paused/lost. */
    public static boolean sessionRunning(){
        try{
            return nativeSessionRunning();
        }catch(Throwable t){
            return false;
        }
    }

    /** Debug: inject a tap through the same path the controller uses. */
    public static void debugTap(float u, float v){
        Activity act = activity;
        if(act == null) return;

        act.runOnUiThread(() -> {
            dispatch(act, u, v, true);
            try{
                Thread.sleep(90);
            }catch(InterruptedException ignored){
            }
            dispatch(act, u, v, false);
        });
    }

    /** Recenter the virtual screen on the XR thread's next frame. */
    public static void requestRecenter(){
        try{
            nativeRequestRecenter();
        }catch(Throwable ignored){
        }
    }

    /** Debug: present a solid colour instead of the game (isolates the XR layer pipeline). */
    public static void setSolid(boolean solid){
        solidDebug = solid;
    }

    /** Flat-VR look-around (head-driven overscan crop + fade). Applied at session start. */
    public static void setLookAround(boolean enabled){
        lookAround = enabled;
    }

    /**
     * In-game fullscreen presentation. When on, the native side wraps the capture around a
     * fixed panorama cylinder and submits a per-eye projection layer; menus stay on the quad.
     * Safe to call every frame (the native side only acts on the transition).
     */
    public static void setWorldView(boolean inGame){
        if(!active) return;
        try{
            nativeSetWorldView(inGame);
        }catch(Throwable ignored){
        }
    }

    /** True when the last submitted frame used the fullscreen projection layer. */
    public static boolean worldViewActive(){
        if(!active) return false;
        try{
            return nativeWorldViewActive();
        }catch(Throwable t){
            return false;
        }
    }

    /** True when the native Alpha ship mesh is loaded (safe to hide the 2D sprite). */
    public static boolean shipReady(){
        if(!active) return false;
        try{
            return nativeShipReady();
        }catch(Throwable t){
            return false;
        }
    }

    /** Publishes the player unit's capture UV, heading and camera zoom for the 3D ship. */
    public static void setPlayerPose(float u, float v, float rotation, float zoom, boolean visible){
        if(!active) return;
        try{
            nativeSetPlayerPose(u, v, rotation, zoom, visible);
        }catch(Throwable ignored){
        }
    }

    /** Live native status string, for logcat triage when no headset UI is visible. */
    public static String status(){
        try{
            return nativeStatus();
        }catch(Throwable t){
            return "unavailable";
        }
    }

    public static void stop(){
        if(!active) return;
        active = false;
        moveAxisX = moveAxisY = 0f;
        viewToggleRequested = false;
        try{
            nativeStop();
        }catch(Throwable t){
            Log.err("VR: native stop failed", t);
        }
    }

    /**
     * Restarts a dead session (headset took the app out of focus, session was lost, ...).
     * Safe to call every frame; the caller rate-limits it.
     */
    public static void restart(Activity act){
        if(failed) return;
        Log.info("VR: restarting session (status: @)", status());
        stop();
        active = false;
        start(act);
    }

    /** True while the native session is still coming up (avoids a false "failed" message). */
    public static boolean isInitializing(){
        boolean initializing = active && System.currentTimeMillis() - startedAt < INIT_TIMEOUT_MS;

        if(active && !initializing && !timeoutLogged){
            timeoutLogged = true;
            if(sessionRunning()){
                Log.info("VR: session running after @ms; status: @", INIT_TIMEOUT_MS, status());
            }else{
                Log.err("VR: session not active after @ms; status: @", INIT_TIMEOUT_MS, status());
            }
        }

        return initializing;
    }

    /** True only while the native OpenXR session is actually running. */
    public static boolean isActive(){
        if(!active) return false;
        try{
            return nativeIsActive();
        }catch(Throwable t){
            return false;
        }
    }

    /** Thumbstick-driven camera zoom, consumed once per frame by the game thread. */
    public static float takeZoom(){
        float z = pendingZoom;
        pendingZoom = 0f;
        return z;
    }

    /** In-game presentation: 0 = panorama projection, 1 = flat quad screen. */
    public static void setViewMode(int mode){
        try{
            nativeSetViewMode(mode);
        }catch(Throwable ignored){
        }
    }

    /** Globe curvature 0..4 (0 small/curved, 4 giant/flat). */
    public static void setGlobeStrength(int strength){
        try{
            nativeSetGlobeStrength(strength);
        }catch(Throwable ignored){
        }
    }

    /** Called from the XR thread when the left Y button is pressed. */
    static void onVrViewToggle(){
        viewToggleRequested = true;
    }

    /** Consumed by the game thread once per frame. */
    public static boolean takeViewToggle(){
        boolean value = viewToggleRequested;
        viewToggleRequested = false;
        return value;
    }

    /** Left-thumbstick movement axis, latest value from the XR thread (no reset). */
    public static float moveX(){
        return moveAxisX;
    }

    public static float moveY(){
        return moveAxisY;
    }

    private static String lastErrorSafe(){
        try{
            return nativeLastError();
        }catch(Throwable t){
            return "unknown";
        }
    }

    // ---- callbacks from the native XR thread ----

    static void onVrEvent(String event){
        Log.info("VR: @", event);
        if(event.endsWith("failed")){
            active = false;
        }
    }

    private static int inputLogCount;

    /** Left thumbstick from the XR thread. Just two volatile floats; no allocation. */
    static void onVrMove(float x, float y){
        moveAxisX = x;
        moveAxisY = y;
    }

    static void onVrInput(float u, float v, boolean down, float stickX, float stickY){
        if(inputLogCount < 3 || (down && inputLogCount % 30 == 0)){
            Log.info("VR input cb: u=@ v=@ down=@ activity=@ (#@)", u, v, down, activity != null, inputLogCount);
        }
        inputLogCount++;

        // Thumbstick is consumed on the game thread; accumulate away from the UI thread.
        if(Math.abs(stickY) > 0.15f){
            pendingZoom += stickY * 0.06f;
        }

        Activity act = activity;
        if(act == null) return;

        act.runOnUiThread(() -> dispatch(act, u, v, down));
    }

    private static boolean loggedViewSize;

    private static void dispatch(Activity act, float u, float v, boolean down){
        if(u < 0f){
            if(pointerDown) release(act, lastX, lastY);
            return;
        }

        // OpenXR/texture space has v=0 at the bottom; MotionEvent/screen space has y=0 at the top.
        int x = Math.round(u * screenW);
        int y = Math.round((1f - v) * screenH);
        lastX = x;
        lastY = y;

        if(!loggedViewSize){
            loggedViewSize = true;
            android.view.View decor = act.getWindow().getDecorView();
            Log.info("VR: capture surface @x@, decor @x@, down=@ at @,@", screenW, screenH,
                     decor.getWidth(), decor.getHeight(), down, x, y);
        }

        if(down){
            if(!pointerDown){
                pointerDown = true;
                downTime = SystemClock.uptimeMillis();
                send(act, MotionEvent.ACTION_DOWN, x, y);
            }else{
                send(act, MotionEvent.ACTION_MOVE, x, y);
            }
        }else if(pointerDown){
            release(act, x, y);
        }
    }

    private static void release(Activity act, int x, int y){
        pointerDown = false;
        send(act, MotionEvent.ACTION_UP, x, y);
    }

    private static void send(Activity act, int action, int x, int y){
        long now = SystemClock.uptimeMillis();
        MotionEvent event = MotionEvent.obtain(downTime, now, action, x, y, 0);
        event.setSource(InputDevice.SOURCE_TOUCHSCREEN);
        try{
            // An immersive VR activity has no touchable window (the headset delivers input via
            // OpenXR), so inject straight into Arc's input queue instead of the view hierarchy.
            if(Core.input instanceof AndroidInput input){
                input.handleTouch(event, input);
            }else{
                act.dispatchTouchEvent(event);
            }
        }catch(Throwable t){
            Log.err("VR: touch dispatch failed", t);
        }finally{
            event.recycle();
        }
    }

    // ---- native ----

    private static native boolean nativeStart(Activity activity, long eglDisplay, long eglContext, int texWidth, int texHeight);

    private static native void nativeCapture(int fbWidth, int fbHeight);

    private static native void nativeStop();

    private static native boolean nativeIsActive();

    private static native String nativeLastError();

    private static native String nativeStatus();

    private static native boolean nativeSessionRunning();

    private static native void nativeSetSolid(boolean solid);

    private static native void nativeSetLookAround(boolean enabled);

    private static native void nativeSetWorldView(boolean inGame);

    private static native void nativeSetViewMode(int mode);

    private static native void nativeSetGlobeStrength(int strength);

    private static native boolean nativeWorldViewActive();

    private static native boolean nativeShipReady();

    private static native void nativeSetPlayerPose(float u, float v, float rotation, float zoom, boolean visible);

    private static native void nativeRequestRecenter();

    public static native String nativeRuntimeVersion();
}
