// Mindustry VR — OpenXR bridge implementation.
//
// Rendering model:
//   * libGDX keeps rendering the game exactly as it does on a phone, into the window's
//     default framebuffer (so the flat build still works and is debuggable).
//   * A game-thread "capture" call copies the finished frame into a shared GL texture.
//   * A dedicated XR thread owns a *shared* EGL context (same share group as libGDX's),
//     presents that texture through an OpenXR quad layer (a head-tracked virtual screen),
//     and turns controller aim into screen coordinates for the Java input layer.
//
// Shared-context EGL is what lets the two GL contexts see the same texture without a
// GPU copy through the CPU.

#define LOG_TAG "MindustryVR"

#include <android/asset_manager.h>
#include <android/asset_manager_jni.h>
#include <android/log.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#include "vr_session.h"
#include "vr_math.h"
#include "vr_render_math.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace vr {
namespace {

struct State {
    JavaVM *vm = nullptr;
    jobject activity = nullptr;
    jclass sessionClass = nullptr;
    jmethodID onInputMethod = nullptr;
    jmethodID onEventMethod = nullptr;
    jmethodID onMoveMethod = nullptr;
    jmethodID onViewToggleMethod = nullptr;

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLContext gameContext = EGL_NO_CONTEXT;
    EGLContext xrContext = EGL_NO_CONTEXT;
    EGLSurface pbuffer = EGL_NO_SURFACE;
    EGLConfig xrConfig = nullptr;

    // Two capture textures: the game thread writes one while the XR thread may still be
    // reading the other, which avoids tearing between frames.
    GLuint captureTex[2] = {0, 0};
    GLuint captureFbo[2] = {0, 0};
    int captureWrite = 0;
    std::atomic<int> capturePublished{-1};
    int texW = 0;
    int texH = 0;

    std::atomic<bool> running{false};
    std::atomic<bool> frameReady{false};
    std::atomic<bool> active{false};
    std::thread thread;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId systemId = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace localSpace = XR_NULL_HANDLE;
    XrSpace viewSpace = XR_NULL_HANDLE;
    XrSwapchain swapchain = XR_NULL_HANDLE;
    int64_t swapFormat = 0;
    uint32_t swapW = 0;
    uint32_t swapH = 0;
    std::vector<XrSwapchainImageOpenGLESKHR> swapImages;
    std::vector<GLuint> swapFbos;

    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction aimAction = XR_NULL_HANDLE;
    XrAction triggerAction = XR_NULL_HANDLE;
    XrPath handPath = XR_NULL_PATH;
    XrPath leftHandPath = XR_NULL_PATH;
    XrAction stickAction = XR_NULL_HANDLE;
    XrAction moveStickAction = XR_NULL_HANDLE;
    XrAction gripAction = XR_NULL_HANDLE;
    XrAction viewToggleAction = XR_NULL_HANDLE;
    bool viewToggleDown = false;
    XrAction hapticAction = XR_NULL_HANDLE;
    XrSpace aimSpace = XR_NULL_HANDLE;

    XrSessionState sessionState = XR_SESSION_STATE_UNKNOWN;
    // Written on the XR thread, read from Java; atomic for visibility.
    std::atomic<bool> sessionRunning{false};
    bool exitRequested = false;

    // Controller cursor, in capture-texture pixels (top-left origin). -1 = hidden.
    std::atomic<int> cursorX{-1};
    std::atomic<int> cursorY{-1};

    // Virtual screen geometry, in metres.
    float quadWidth = 4.4f;  // full-view: ~103 deg wide at 1.75 m, world-pinned
    float quadHeight = 1.35f;
    float quadZ = -1.75f;
    /** Pose of the virtual screen in LOCAL space; updated by the recenter action. */
    XrPosef quadPose{{0.f, 0.f, 0.f, 1.f}, {0.f, 0.f, -1.75f}};
    bool gripDown = false;
    bool triggerDown = false;
    int endFrameLog = 0;
    uint64_t submittedFrames = 0;
    uint64_t capturedFrames = 0;
    bool pendingRecenter = false;
    bool loggedCaptureSample = false;
    bool solidDebug = false;
    std::atomic<bool> recenterRequested{false};
    // Flat-VR look-around (docs/vr/flat-vr-lookaround-goal.md)
    std::atomic<bool> lookAround{false};
    bool lookRefValid = false;
    float refYaw = 0.f, refPitch = 0.f;
    float smoothRight = 0.f, smoothUp = 0.f;
    LookCrop crop{0.f, 0.f, 1.f, 1.f};
    bool cropValid = false;
    LookCrop backCrop{0.f, 0.f, 1.f, 1.f};
    int lookLog = 0;

    // ---- Fullscreen panoramic world view (in-game only) --------------------------------------
    //
    // The capture is wrapped around a fixed vertical cylinder in LOCAL space. The game
    // camera's centre is the image centre, so the centre stays centre; turning the head
    // reveals the map beyond the old screen, and the cylinder radius gives real per-eye
    // parallax. Two single-eye projection swapchains are used (the documented array
    // fallback) because they are the safest path on Adreno.
    std::atomic<bool> worldView{false};         // Java: Vars.state.isGame()
    std::atomic<int> viewMode{0};               // 0 = panorama (default), 1 = flat 4.4m, 2 = flat full 5.2m
    std::atomic<bool> projectionActive{false};  // last submitted frame used a projection layer
    std::atomic<bool> shipReady{false};
    std::atomic<bool> panoRecenterRequested{false};
    bool panoRefValid = false;
    Vec3 panoCenter{0.f, 1.6f, 0.f};
    float panoYaw = 0.f;
    float panoRadius = 2.0f;
    // Panorama: the capture wrapped around the fixed cylinder (the view the user preferred
    // before the dome experiment). Flat mode is a separate quad presentation.
    float panoSpanH = 170.f * 0.01745329252f;
    float panoSpanV = 102.f * 0.01745329252f;

    // Player unit anchor, published by Java each frame (capture UV + heading).
    std::atomic<float> playerU{0.5f}, playerV{0.5f}, playerRot{0.f}, playerZoom{1.f};
    std::atomic<bool> playerVisible{false};

    bool projReady = false;
    bool projFailed = false;
    uint32_t eyeW[2] = {0, 0};
    uint32_t eyeH[2] = {0, 0};
    XrSwapchain projSwapchain[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::vector<XrSwapchainImageOpenGLESKHR> projImages[2];
    std::vector<GLuint> projFbos[2];
    GLuint projDepth[2] = {0, 0};
    XrView projViews[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    XrCompositionLayerProjectionView projLayerViews[2]{};
    XrCompositionLayerProjection projLayer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    int lastCapture = 0;
    int projLog = 0;

    GLuint panoProg = 0;
    GLint panoTex = -1, panoTan = -1, panoEyeQuat = -1, panoEyePos = -1, panoRefCenter = -1;
    GLint panoRefRight = -1, panoRefFwd = -1, panoRadiusLoc = -1, panoSpan = -1, panoFade = -1;
    GLuint fullscreenVao = 0;

    GLuint shipProg = 0;
    GLint shipMvp = -1;
    GLuint shipVao = 0, shipVbo = 0, shipEbo = 0;
    int shipIndexCount = 0;
    float shipScale = 0.12f;
    AAssetManager *assets = nullptr;
    jobject assetsRef = nullptr;
    bool loggedWorldView = false;

    GLuint fadeProg = 0;
    bool fadeFailed = false;
    GLuint fillFbo[2] = {0, 0};
    bool loggedBlitError = false;
    bool loggedFbSample = false;
    bool loggedTriggerActive = false;
    bool loggedCopyError = false;
    bool useReadback = false;
    std::vector<unsigned char> readbackPixels;
};

State g;

/** Local copy of the "last error" string, safe to read from any thread. */
char errorBuf[256] = "not initialized";

/** Human-readable session status, refreshed on every state change (for logcat triage). */
char statusBuf[224] = "uninitialized";

void refreshStatus(){
    if(g.session == XR_NULL_HANDLE){
        snprintf(statusBuf, sizeof(statusBuf), "no-session active=%d running=%d err=%s",
                 (int)g.active.load(), (int)g.running.load(), errorBuf);
    }else{
        snprintf(statusBuf, sizeof(statusBuf), "active=%d running=%d state=%d",
                 (int)g.active.load(), (int)g.running.load(), (int)g.sessionState);
    }
}

void setError(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vsnprintf(errorBuf, sizeof(errorBuf), fmt, args);
    va_end(args);
    LOGE("%s", errorBuf);
}

#define XRCHECK(expr)                                                                    \
    do {                                                                                 \
        XrResult _r = (expr);                                                            \
        if (XR_FAILED(_r)) {                                                             \
            setError("%s failed: XrResult %d", #expr, (int)_r);                          \
            return false;                                                                \
        }                                                                                \
    } while (0)

bool checkExtension(const char *name) {
    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, 0, &count, nullptr))) return false;
    std::vector<XrExtensionProperties> props(count, {XR_TYPE_EXTENSION_PROPERTIES});
    if (XR_FAILED(xrEnumerateInstanceExtensionProperties(nullptr, count, &count, props.data()))) return false;
    for (auto &p : props) {
        if (strcmp(p.extensionName, name) == 0) return true;
    }
    return false;
}

/** Finds the EGLConfig the game's context was created with (needed for a compatible session). */
EGLConfig findGameConfig() {
    EGLint id = 0;
    if (!eglQueryContext(g.display, g.gameContext, EGL_CONFIG_ID, &id)) return nullptr;

    EGLint count = 0;
    if (!eglGetConfigs(g.display, nullptr, 0, &count) || count == 0) return nullptr;
    std::vector<EGLConfig> configs((size_t)count);
    if (!eglGetConfigs(g.display, configs.data(), count, &count)) return nullptr;

    for (int i = 0; i < count; i++) {
        EGLint candidate = 0;
        if (eglGetConfigAttrib(g.display, configs[(size_t)i], EGL_CONFIG_ID, &candidate) && candidate == id) {
            return configs[(size_t)i];
        }
    }
    return nullptr;
}

bool createSharedContext() {
    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        setError("eglBindAPI(ES) failed");
        return false;
    }

    EGLint cfgAttrs[] = {
            EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
            EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
            EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
            EGL_NONE
    };
    EGLint numConfigs = 0;
    EGLConfig pbufferConfig = nullptr;
    eglChooseConfig(g.display, cfgAttrs, &pbufferConfig, 1, &numConfigs);

    EGLint ctxAttrs[] = {EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE};

    auto tryCreate = [&](EGLConfig cfg, bool share) -> bool {
        if (cfg == nullptr) return false;
        EGLContext ctx = eglCreateContext(g.display, cfg, share ? g.gameContext : EGL_NO_CONTEXT, ctxAttrs);
        if (ctx == EGL_NO_CONTEXT) return false;

        EGLint pbAttrs[] = {EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE};
        EGLSurface pb = eglCreatePbufferSurface(g.display, cfg, pbAttrs);
        if (pb == EGL_NO_SURFACE) {
            eglDestroyContext(g.display, ctx);
            return false;
        }
        if (!eglMakeCurrent(g.display, pb, pb, ctx)) {
            eglDestroySurface(g.display, pb);
            eglDestroyContext(g.display, ctx);
            return false;
        }
        g.xrConfig = cfg;
        g.xrContext = ctx;
        g.pbuffer = pb;
        return true;
    };

    // Preferred: pbuffer config, sharing textures with libGDX. Sharing across different
    // configs is legal, but if a driver disagrees, fall back to the game's own config.
    if (tryCreate(pbufferConfig, true)) {
        LOGI("XR EGL context created (shared, pbuffer config)");
        return true;
    }
    LOGE("shared context (pbuffer config) failed: 0x%x", eglGetError());

    EGLConfig gameConfig = findGameConfig();
    if (tryCreate(gameConfig, true)) {
        LOGI("XR EGL context created (shared, game config)");
        return true;
    }
    LOGE("shared context (game config) failed: 0x%x", eglGetError());

    // Last resort: unshared context. The XR session still works, but the game texture
    // will not be visible (black screen) — log it loudly instead of silently failing.
    if (tryCreate(pbufferConfig, false)) {
        LOGE("Falling back to UNSHARED EGL context; game texture will not be visible");
        return true;
    }

    setError("could not create any EGL context for OpenXR (0x%x)", eglGetError());
    return false;
}

bool createSwapchain() {
    uint32_t formatCount = 0;
    XRCHECK(xrEnumerateSwapchainFormats(g.session, 0, &formatCount, nullptr));
    std::vector<int64_t> formats(formatCount);
    XRCHECK(xrEnumerateSwapchainFormats(g.session, formatCount, &formatCount, formats.data()));

    int64_t chosen = 0;
    for (int64_t f : formats) {
        if (f == GL_RGBA8) {
            chosen = f;
            break;
        }
    }
    if (chosen == 0 && !formats.empty()) chosen = formats[0];
    g.swapFormat = chosen;

    XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
    info.format = chosen;
    info.sampleCount = 1;
    info.width = (uint32_t)g.texW;
    info.height = (uint32_t)g.texH;
    info.faceCount = 1;
    info.arraySize = 1;
    info.mipCount = 1;
    XRCHECK(xrCreateSwapchain(g.session, &info, &g.swapchain));

    uint32_t imageCount = 0;
    XRCHECK(xrEnumerateSwapchainImages(g.swapchain, 0, &imageCount, nullptr));
    g.swapImages.assign(imageCount, XrSwapchainImageOpenGLESKHR{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    XRCHECK(xrEnumerateSwapchainImages(g.swapchain, imageCount, &imageCount,
                                      reinterpret_cast<XrSwapchainImageBaseHeader *>(g.swapImages.data())));

    g.swapW = (uint32_t)g.texW;
    g.swapH = (uint32_t)g.texH;
    g.swapFbos.resize(imageCount, 0);

    for (uint32_t i = 0; i < imageCount; i++) {
        GLuint fbo = 0;
        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.swapImages[i].image, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            setError("swapchain framebuffer incomplete");
            return false;
        }
        glViewport(0, 0, (GLsizei)g.swapW, (GLsizei)g.swapH);
        glClearColor(0.f, 0.f, 0.f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        g.swapFbos[i] = fbo;
    }

    // Framebuffers that expose libGDX's capture textures to this context for blitting.
    for (int i = 0; i < 2; i++) {
        glGenFramebuffers(1, &g.captureFbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, g.captureFbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.captureTex[i], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            setError("capture framebuffer %d incomplete (shared context failed?)", i);
            return false;
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

bool createActions() {
    XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
    strncpy(setInfo.actionSetName, "gameplay", XR_MAX_ACTION_SET_NAME_SIZE - 1);
    strncpy(setInfo.localizedActionSetName, "Gameplay", XR_MAX_LOCALIZED_ACTION_SET_NAME_SIZE - 1);
    XRCHECK(xrCreateActionSet(g.instance, &setInfo, &g.actionSet));

    XrPath handPath = XR_NULL_PATH;
    XRCHECK(xrStringToPath(g.instance, "/user/hand/right", &handPath));
    g.handPath = handPath;
    XrPath leftPath = XR_NULL_PATH;
    XRCHECK(xrStringToPath(g.instance, "/user/hand/left", &leftPath));
    g.leftHandPath = leftPath;

    XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
    actionInfo.countSubactionPaths = 1;
    actionInfo.subactionPaths = &handPath;

    strncpy(actionInfo.actionName, "aim", XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(actionInfo.localizedActionName, "Aim", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
    XRCHECK(xrCreateAction(g.actionSet, &actionInfo, &g.aimAction));

    strncpy(actionInfo.actionName, "trigger", XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(actionInfo.localizedActionName, "Trigger", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    XRCHECK(xrCreateAction(g.actionSet, &actionInfo, &g.triggerAction));

    strncpy(actionInfo.actionName, "thumbstick", XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(actionInfo.localizedActionName, "Thumbstick", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    actionInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    XRCHECK(xrCreateAction(g.actionSet, &actionInfo, &g.stickAction));

    strncpy(actionInfo.actionName, "haptic", XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(actionInfo.localizedActionName, "Haptic", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    actionInfo.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
    XRCHECK(xrCreateAction(g.actionSet, &actionInfo, &g.hapticAction));

    strncpy(actionInfo.actionName, "grip", XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(actionInfo.localizedActionName, "Grip", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
    XRCHECK(xrCreateAction(g.actionSet, &actionInfo, &g.gripAction));

    // Left thumbstick: movement (the right stick stays camera zoom).
    actionInfo.countSubactionPaths = 1;
    actionInfo.subactionPaths = &leftPath;
    strncpy(actionInfo.actionName, "move", XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(actionInfo.localizedActionName, "Move", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    actionInfo.actionType = XR_ACTION_TYPE_VECTOR2F_INPUT;
    XRCHECK(xrCreateAction(g.actionSet, &actionInfo, &g.moveStickAction));

    strncpy(actionInfo.actionName, "viewtoggle", XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(actionInfo.localizedActionName, "View Toggle", XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    actionInfo.actionType = XR_ACTION_TYPE_BOOLEAN_INPUT;
    XRCHECK(xrCreateAction(g.actionSet, &actionInfo, &g.viewToggleAction));

    XrActionSpaceCreateInfo spaceInfo{XR_TYPE_ACTION_SPACE_CREATE_INFO};
    spaceInfo.action = g.aimAction;
    spaceInfo.subactionPath = handPath;
    spaceInfo.poseInActionSpace.orientation.w = 1.f;
    XRCHECK(xrCreateActionSpace(g.session, &spaceInfo, &g.aimSpace));

    XrPath profile = XR_NULL_PATH;
    XRCHECK(xrStringToPath(g.instance, "/interaction_profiles/oculus/touch_controller", &profile));

    auto path = [](const char *s) {
        XrPath p = XR_NULL_PATH;
        xrStringToPath(g.instance, s, &p);
        return p;
    };

    XrActionSuggestedBinding bindings[] = {
            {g.aimAction, path("/user/hand/right/input/aim/pose")},
            {g.triggerAction, path("/user/hand/right/input/trigger/value")},
            {g.stickAction, path("/user/hand/right/input/thumbstick")},
            {g.gripAction, path("/user/hand/right/input/squeeze/value")},
            {g.hapticAction, path("/user/hand/right/output/haptic")},
            {g.moveStickAction, path("/user/hand/left/input/thumbstick")},
    };

    XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    suggested.interactionProfile = profile;
    suggested.countSuggestedBindings = sizeof(bindings) / sizeof(bindings[0]);
    suggested.suggestedBindings = bindings;
    // Not fatal: a runtime may support the session without this particular profile.
    if (XR_FAILED(xrSuggestInteractionProfileBindings(g.instance, &suggested))) {
        LOGW("Oculus Touch bindings were rejected; controller input will be unavailable");
    }

    // Optional: left Y cycles the VR view. Suggested separately so a runtime that rejects
    // this one path does not invalidate the aim/trigger/stick bindings above.
    XrActionSuggestedBinding viewBinding[] = {
            {g.viewToggleAction, path("/user/hand/left/input/y/click")},
    };
    XrInteractionProfileSuggestedBinding viewSuggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    viewSuggested.interactionProfile = profile;
    viewSuggested.countSuggestedBindings = 1;
    viewSuggested.suggestedBindings = viewBinding;
    if (XR_FAILED(xrSuggestInteractionProfileBindings(g.instance, &viewSuggested))) {
        LOGW("left Y view-toggle binding rejected; use Settings > Graphics > VR View");
    }

    XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    attach.countActionSets = 1;
    attach.actionSets = &g.actionSet;
    XRCHECK(xrAttachSessionActionSets(g.session, &attach));
    return true;
}

/**
 * Android loaders require xrInitializeLoaderKHR (XR_KHR_loader_init_android) with the VM and
 * a Context before extensions can be enumerated. Without it every runtime extension looks
 * missing and xrCreateInstance fails.
 */
bool initializeLoader() {
    PFN_xrInitializeLoaderKHR initLoader = nullptr;
    XrResult lookup = xrGetInstanceProcAddr(
            XR_NULL_HANDLE, "xrInitializeLoaderKHR",
            reinterpret_cast<PFN_xrVoidFunction *>(&initLoader));

    if (XR_FAILED(lookup) || initLoader == nullptr) {
        setError("xrInitializeLoaderKHR unavailable (loader too old?)");
        return false;
    }

    XrLoaderInitInfoAndroidKHR info{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
    info.applicationVM = g.vm;
    info.applicationContext = g.activity;

    XrResult result = initLoader(reinterpret_cast<const XrLoaderInitInfoBaseHeaderKHR *>(&info));
    if (XR_FAILED(result)) {
        setError("xrInitializeLoaderKHR failed: XrResult %d", (int)result);
        return false;
    }

    LOGI("OpenXR loader initialized (android)");
    return true;
}

bool createProjectionSwapchain();
bool loadShip();

bool createSession() {
    if (!initializeLoader()) return false;

    XrInstanceCreateInfoAndroidKHR androidInfo{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    androidInfo.applicationVM = g.vm;
    androidInfo.applicationActivity = g.activity;

    XrInstanceCreateInfo info{XR_TYPE_INSTANCE_CREATE_INFO};
    info.next = &androidInfo;
    strncpy(info.applicationInfo.applicationName, "Mindustry VR", XR_MAX_APPLICATION_NAME_SIZE - 1);
    info.applicationInfo.applicationVersion = 1;
    strncpy(info.applicationInfo.engineName, "Mindustry/libGDX", XR_MAX_ENGINE_NAME_SIZE - 1);
    info.applicationInfo.engineVersion = 1;
    info.applicationInfo.apiVersion = XR_CURRENT_API_VERSION;

    const char *extensions[] = {
            XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME,
            XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME,
    };

    if (!checkExtension(extensions[0])) {
        setError("OpenXR runtime is missing %s", extensions[0]);
        return false;
    }
    if (!checkExtension(extensions[1])) {
        setError("OpenXR runtime is missing %s", extensions[1]);
        return false;
    }

    info.enabledExtensionCount = 2;
    info.enabledExtensionNames = extensions;

    XRCHECK(xrCreateInstance(&info, &g.instance));
    LOGI("OpenXR instance created");

    XrSystemGetInfo systemInfo{XR_TYPE_SYSTEM_GET_INFO};
    systemInfo.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XRCHECK(xrGetSystem(g.instance, &systemInfo, &g.systemId));

    // Android runtimes require the graphics-requirements query before xrCreateSession;
    // skipping it fails with XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING (-50).
    PFN_xrGetOpenGLESGraphicsRequirementsKHR getRequirements = nullptr;
    xrGetInstanceProcAddr(g.instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                          reinterpret_cast<PFN_xrVoidFunction *>(&getRequirements));
    if (getRequirements == nullptr) {
        setError("runtime does not expose xrGetOpenGLESGraphicsRequirementsKHR");
        return false;
    }

    XrGraphicsRequirementsOpenGLESKHR requirements{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    XrResult requirementsResult = getRequirements(g.instance, g.systemId, &requirements);
    if (XR_FAILED(requirementsResult)) {
        setError("xrGetOpenGLESGraphicsRequirementsKHR failed: XrResult %d", (int)requirementsResult);
        return false;
    }
    LOGI("OpenGLES requirements: min %u.%u, max %u.%u",
         XR_VERSION_MAJOR(requirements.minApiVersionSupported), XR_VERSION_MINOR(requirements.minApiVersionSupported),
         XR_VERSION_MAJOR(requirements.maxApiVersionSupported), XR_VERSION_MINOR(requirements.maxApiVersionSupported));

    XrGraphicsBindingOpenGLESAndroidKHR binding{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    binding.display = g.display;
    binding.config = g.xrConfig;
    binding.context = g.xrContext;

    XrSessionCreateInfo sessionInfo{XR_TYPE_SESSION_CREATE_INFO};
    sessionInfo.next = &binding;
    sessionInfo.systemId = g.systemId;
    XRCHECK(xrCreateSession(g.instance, &sessionInfo, &g.session));
    LOGI("OpenXR session created");

    XrReferenceSpaceCreateInfo spaceInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    spaceInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    spaceInfo.poseInReferenceSpace.orientation.w = 1.f;
    XRCHECK(xrCreateReferenceSpace(g.session, &spaceInfo, &g.localSpace));

    XrReferenceSpaceCreateInfo viewInfo{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    viewInfo.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    viewInfo.poseInReferenceSpace.orientation.w = 1.f;
    XRCHECK(xrCreateReferenceSpace(g.session, &viewInfo, &g.viewSpace));

    if (!createSwapchain()) return false;
    if (!createActions()) return false;

    g.quadWidth = 4.4f;
    g.quadHeight = g.quadWidth * (float)g.texH / (float)g.texW;

    // Projection is the in-game presentation, but it is strictly optional: if any part of
    // the stereo path fails, the session keeps running with the quad screen.
    if (!createProjectionSwapchain()) {
        LOGW("fullscreen projection unavailable; continuing with the quad screen");
    }
    if (!loadShip()) {
        LOGW("3D player ship unavailable; keeping the sprite");
    }

    return true;
}

void notifyEvent(JNIEnv *env, const char *message) {
    if (env == nullptr || g.sessionClass == nullptr || g.onEventMethod == nullptr) return;
    jstring str = env->NewStringUTF(message);
    env->CallStaticVoidMethod(g.sessionClass, g.onEventMethod, str);
    env->DeleteLocalRef(str);
}

void notifyInput(JNIEnv *env, float u, float v, bool down, float stickX, float stickY) {
    static int notifyCalls = 0;
    if (notifyCalls < 3 || (notifyCalls % 600) == 0) {
        LOGI("notifyInput #%d u=%.2f v=%.2f down=%d env=%d cls=%d mid=%d", notifyCalls, u, v,
             (int)down, env != nullptr, g.sessionClass != nullptr, g.onInputMethod != nullptr);
    }
    notifyCalls++;

    if (env == nullptr || g.sessionClass == nullptr || g.onInputMethod == nullptr) return;
    env->CallStaticVoidMethod(g.sessionClass, g.onInputMethod, (jfloat)u, (jfloat)v, (jboolean)down,
                              (jfloat)stickX, (jfloat)stickY);
}

void notifyMove(JNIEnv *env, float x, float y) {
    if (env == nullptr || g.sessionClass == nullptr || g.onMoveMethod == nullptr) return;
    env->CallStaticVoidMethod(g.sessionClass, g.onMoveMethod, (jfloat)x, (jfloat)y);
}

void notifyViewToggle(JNIEnv *env) {
    if (env == nullptr || g.sessionClass == nullptr || g.onViewToggleMethod == nullptr) return;
    env->CallStaticVoidMethod(g.sessionClass, g.onViewToggleMethod);
}

void pollEvents(JNIEnv *env) {
    for (;;) {
        XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
        XrResult result = xrPollEvent(g.instance, &buffer);
        if (result == XR_EVENT_UNAVAILABLE) break;
        if (XR_FAILED(result)) {
            LOGW("xrPollEvent failed (%d)", (int)result);
            break;
        }

        if (buffer.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto *event = reinterpret_cast<XrEventDataSessionStateChanged *>(&buffer);
            g.sessionState = event->state;
            LOGI("session state -> %d", (int)event->state);
            refreshStatus();

            switch (event->state) {
                case XR_SESSION_STATE_VISIBLE:
                case XR_SESSION_STATE_FOCUSED:
                    // The user is (re)entering the app; put the screen where they are looking.
                    g.pendingRecenter = true;
                    break;
                case XR_SESSION_STATE_READY: {
                    XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
                    begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (XR_SUCCEEDED(xrBeginSession(g.session, &begin))) {
                        g.sessionRunning = true;
                        notifyEvent(env, "running");
                    } else {
                        setError("xrBeginSession failed");
                    }
                    break;
                }
                case XR_SESSION_STATE_STOPPING:
                    if (g.sessionRunning) {
                        xrEndSession(g.session);
                        g.sessionRunning = false;
                    }
                    break;
                case XR_SESSION_STATE_EXITING:
                case XR_SESSION_STATE_LOSS_PENDING:
                    g.exitRequested = true;
                    break;
                default:
                    break;
            }
        } else if (buffer.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            g.exitRequested = true;
        }
    }
}

GLuint compileFadeProgram() {
    const char *vs = "#version 300 es\n"
        "out vec2 vUv;\n"
        "void main(){ vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));"
        " vUv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); }\n";
    const char *fs = "#version 300 es\n"
        "precision mediump float;\n"
        "uniform sampler2D uTex; uniform vec4 uCrop; uniform float uAspect;\n"
        "in vec2 vUv; out vec4 o;\n"
        "void main(){ vec4 c = texture(uTex, uCrop.xy + vUv * uCrop.zw);\n"
        " vec2 p = (vUv - 0.5) * vec2(uAspect, 1.0);\n"
        " float d = length(p) * 1.6;\n"
        " float s = smoothstep(0.45, 0.95, d);\n"
        " float lum = dot(c.rgb, vec3(0.299, 0.587, 0.114));\n"
        " c.rgb = mix(vec3(lum), c.rgb, 1.0 - 0.25 * s) * (1.0 - 0.45 * s);\n"
        " o = c; }\n";
    auto compile = [](GLenum type, const char *src) -> GLuint {
        GLuint s = glCreateShader(type);
        glShaderSource(s, 1, &src, nullptr);
        glCompileShader(s);
        GLint ok = 0;
        glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { glDeleteShader(s); return 0; }
        return s;
    };
    GLuint v = compile(GL_VERTEX_SHADER, vs), f = compile(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) { if (v) glDeleteShader(v); if (f) glDeleteShader(f); return 0; }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v); glAttachShader(prog, f);
    glLinkProgram(prog);
    glDeleteShader(v); glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) { glDeleteProgram(prog); return 0; }
    return prog;
}

void blitCapture(uint32_t imageIndex, int buffer);

/** Crop + radial-fade copy used by look-around; falls back to the plain blit. */
void blitFade(uint32_t imageIndex, int buffer) {
    if (g.fadeProg == 0 && !g.fadeFailed) {
        g.fadeProg = compileFadeProgram();
        if (g.fadeProg == 0) {
            g.fadeFailed = true;
            LOGW("look-around fade shader failed; using plain blit");
        } else {
            LOGI("look-around fade shader ready");
        }
    }
    if (g.fadeProg == 0) { blitCapture(imageIndex, buffer); return; }
    glBindFramebuffer(GL_FRAMEBUFFER, g.swapFbos[imageIndex]);
    glViewport(0, 0, (GLsizei)g.swapW, (GLsizei)g.swapH);
    glUseProgram(g.fadeProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g.captureTex[buffer]);
    glUniform1i(glGetUniformLocation(g.fadeProg, "uTex"), 0);
    const LookCrop c = g.cropValid ? g.crop : LookCrop{0.f, 0.f, 1.f, 1.f};
    glUniform4f(glGetUniformLocation(g.fadeProg, "uCrop"), c.x, c.y, c.w, c.h);
    glUniform1f(glGetUniformLocation(g.fadeProg, "uAspect"),
                g.swapH > 0 ? (float)g.swapW / (float)g.swapH : 1.78f);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

void blitCapture(uint32_t imageIndex, int buffer) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, g.captureFbo[buffer]);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, g.swapFbos[imageIndex]);
    glBlitFramebuffer(0, 0, g.texW, g.texH, 0, 0, (GLint)g.swapW, (GLint)g.swapH,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// =============================================================================================
// Fullscreen panoramic world view + stereo 3D ship
// =============================================================================================

GLuint compileStage(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        GLsizei len = 0;
        glGetShaderInfoLog(s, sizeof(log) - 1, &len, log);
        log[len > 0 ? len : 0] = '\0';
        LOGE("shader compile failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint linkProgram(const char *vs, const char *fs, const char *const *attribs, int attribCount) {
    GLuint v = compileStage(GL_VERTEX_SHADER, vs);
    GLuint f = compileStage(GL_FRAGMENT_SHADER, fs);
    if (!v || !f) {
        if (v) glDeleteShader(v);
        if (f) glDeleteShader(f);
        return 0;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, v);
    glAttachShader(prog, f);
    for (int i = 0; i < attribCount; i++) {
        glBindAttribLocation(prog, (GLuint)i, attribs[i]);
    }
    glLinkProgram(prog);
    glDeleteShader(v);
    glDeleteShader(f);
    GLint ok = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        GLsizei len = 0;
        glGetProgramInfoLog(prog, sizeof(log) - 1, &len, log);
        log[len > 0 ? len : 0] = '\0';
        LOGE("program link failed: %s", log);
        glDeleteProgram(prog);
        return 0;
    }
    return prog;
}

bool ensurePanoProgram() {
    if (g.panoProg != 0) return true;
    const char *vs = "#version 300 es\n"
        "out vec2 vNdc;\n"
        "void main(){ vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));"
        " vNdc = p * 2.0 - 1.0; gl_Position = vec4(vNdc, 0.0, 1.0); }\n";
    const char *fs = "#version 300 es\n"
        "precision highp float;\n"
        "in vec2 vNdc; out vec4 o;\n"
        "uniform sampler2D uTex;\n"
        "uniform vec4 uTan;        // tanLeft, tanRight, tanDown, tanUp\n"
        "uniform vec4 uEyeQuat;    // eye orientation in LOCAL (x,y,z,w)\n"
        "uniform vec3 uEyePos;     // eye position in LOCAL\n"
        "uniform vec3 uRefCenter;  // pano cylinder centre\n"
        "uniform vec3 uRefRight;   // pano reference right (+X)\n"
        "uniform vec3 uRefFwd;     // pano reference forward (-Z)\n"
        "uniform float uRadius;\n"
        "uniform vec2 uSpan;       // capture angular span (rad)\n"
        "uniform vec4 uFade;       // edge inner/outer, vignette inner/outer\n"
        "vec3 qrot(vec4 q, vec3 v){\n"
        "  return v + 2.0 * cross(q.xyz, cross(q.xyz, v) + q.w * v);\n"
        "}\n"
        "void main(){\n"
        "  float tanX = mix(uTan.x, uTan.y, (vNdc.x + 1.0) * 0.5);\n"
        "  float tanY = mix(uTan.z, uTan.w, (vNdc.y + 1.0) * 0.5);\n"
        "  vec3 dir = normalize(qrot(uEyeQuat, vec3(tanX, tanY, -1.0)));\n"
        "  vec3 rel = uEyePos - uRefCenter;\n"
        "  float a = dot(dir.xz, dir.xz);\n"
        "  float b = 2.0 * dot(rel.xz, dir.xz);\n"
        "  float c = dot(rel.xz, rel.xz) - uRadius * uRadius;\n"
        "  float disc = b * b - 4.0 * a * c;\n"
        "  vec3 col = vec3(0.0);\n"
        "  if(a > 1e-7 && disc >= 0.0){\n"
        "    float t = (-b + sqrt(disc)) / (2.0 * a);\n"
        "    if(t > 0.0){\n"
        "      vec3 hit = rel + dir * t;\n"
        "      float x = dot(hit, uRefRight);\n"
        "      float z = dot(hit, uRefFwd);\n"
        "      float yaw = atan(x, z);\n"
        "      float pitch = atan(hit.y, length(hit.xz));\n"
        "      vec2 uv = vec2(0.5 + yaw / uSpan.x, 0.5 + pitch / uSpan.y);\n"
        "      float edge = min(min(uv.x, 1.0 - uv.x), min(uv.y, 1.0 - uv.y));\n"
        "      float edgeFade = smoothstep(uFade.x, uFade.y, edge);\n"
        "      vec3 tex = texture(uTex, clamp(uv, vec2(0.001), vec2(0.999))).rgb;\n"
        "      float lum = dot(tex, vec3(0.299, 0.587, 0.114));\n"
        "      tex = mix(vec3(lum), tex, 1.0 - 0.28 * (1.0 - edgeFade));\n"
        "      col = tex * edgeFade;\n"
        "    }\n"
        "  }\n"
        "  float ang = atan(length(vec2(tanX, tanY)));\n"
        "  float vig = 1.0 - 0.35 * smoothstep(uFade.z, uFade.w, ang);\n"
        "  o = vec4(col * vig, 1.0);\n"
        "}\n";
    g.panoProg = linkProgram(vs, fs, nullptr, 0);
    if (g.panoProg == 0) {
        LOGE("panorama shader failed to build");
        return false;
    }
    g.panoTex = glGetUniformLocation(g.panoProg, "uTex");
    g.panoTan = glGetUniformLocation(g.panoProg, "uTan");
    g.panoEyeQuat = glGetUniformLocation(g.panoProg, "uEyeQuat");
    g.panoEyePos = glGetUniformLocation(g.panoProg, "uEyePos");
    g.panoRefCenter = glGetUniformLocation(g.panoProg, "uRefCenter");
    g.panoRefRight = glGetUniformLocation(g.panoProg, "uRefRight");
    g.panoRefFwd = glGetUniformLocation(g.panoProg, "uRefFwd");
    g.panoRadiusLoc = glGetUniformLocation(g.panoProg, "uRadius");
    g.panoSpan = glGetUniformLocation(g.panoProg, "uSpan");
    g.panoFade = glGetUniformLocation(g.panoProg, "uFade");

    glGenVertexArrays(1, &g.fullscreenVao);
    LOGI("panorama shader ready");
    return true;
}

bool ensureShipProgram() {
    if (g.shipProg != 0) return true;
    const char *vs = "#version 300 es\n"
        "in vec3 a_position;\n"
        "in vec4 a_color;\n"
        "uniform mat4 uMvp;\n"
        "out vec4 vColor;\n"
        "void main(){ vColor = a_color; gl_Position = uMvp * vec4(a_position, 1.0); }\n";
    const char *fs = "#version 300 es\n"
        "precision mediump float;\n"
        "in vec4 vColor; out vec4 o;\n"
        "void main(){ o = vColor; }\n";
    const char *attrs[] = {"a_position", "a_color"};
    g.shipProg = linkProgram(vs, fs, attrs, 2);
    if (g.shipProg == 0) {
        LOGE("ship shader failed to build");
        return false;
    }
    g.shipMvp = glGetUniformLocation(g.shipProg, "uMvp");
    LOGI("ship shader ready");
    return true;
}

/** Reads vr/alpha_ship.vrmesh from the APK and uploads it to a VAO/VBO/EBO. */
bool loadShip() {
    if (g.shipReady.load()) return true;
    if (g.assets == nullptr) return false;
    AAsset *asset = AAssetManager_open(g.assets, "vr/alpha_ship.vrmesh", AASSET_MODE_BUFFER);
    if (asset == nullptr) {
        LOGE("ship asset vr/alpha_ship.vrmesh not found");
        return false;
    }
    const off_t size = AAsset_getLength(asset);
    std::vector<unsigned char> data((size_t)size);
    int read = 0;
    while (read < (int)size) {
        const int chunk = AAsset_read(asset, data.data() + read, (size_t)((int)size - read));
        if (chunk <= 0) break;
        read += chunk;
    }
    AAsset_close(asset);
    if (read != (int)size || size < 44) {
        LOGE("ship asset read failed (%d of %d)", read, (int)size);
        return false;
    }

    uint32_t magic = 0, version = 0, nv = 0, ni = 0, flags = 0;
    memcpy(&magic, data.data(), 4);
    memcpy(&version, data.data() + 4, 4);
    memcpy(&nv, data.data() + 8, 4);
    memcpy(&ni, data.data() + 12, 4);
    memcpy(&flags, data.data() + 16, 4);
    if (magic != 0x4D445631u || version != 1 || nv == 0 || ni == 0 || nv > 65535) {
        LOGE("ship asset header invalid (magic 0x%x v%u nv%u ni%u)", magic, version, nv, ni);
        return false;
    }
    const size_t vertexFloats = (size_t)nv * 7;
    const size_t expected = 44 + vertexFloats * sizeof(float) + (size_t)ni * sizeof(uint16_t);
    if (data.size() < expected) {
        LOGE("ship asset truncated (%zu < %zu)", data.size(), expected);
        return false;
    }

    std::vector<float> verts(vertexFloats);
    memcpy(verts.data(), data.data() + 44, vertexFloats * sizeof(float));
    std::vector<uint16_t> indices(ni);
    memcpy(indices.data(), data.data() + 44 + vertexFloats * sizeof(float), ni * sizeof(uint16_t));

    glGenVertexArrays(1, &g.shipVao);
    glBindVertexArray(g.shipVao);
    glGenBuffers(1, &g.shipVbo);
    glBindBuffer(GL_ARRAY_BUFFER, g.shipVbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(verts.size() * sizeof(float)), verts.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 7 * sizeof(float), (const void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, 7 * sizeof(float),
                          (const void *)(3 * sizeof(float)));
    glGenBuffers(1, &g.shipEbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.shipEbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, (GLsizeiptr)(indices.size() * sizeof(uint16_t)), indices.data(),
                 GL_STATIC_DRAW);
    glBindVertexArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    if (glGetError() != GL_NO_ERROR) {
        LOGE("ship mesh upload failed (0x%x)", glGetError());
        return false;
    }

    g.shipIndexCount = (int)ni;
    // The overlay needs a depth buffer to sort its 15k triangles. If the projection eye FBOs
    // could not attach depth, keep the 2D sprite instead of drawing a broken model.
    const bool depthOk = g.projDepth[0] != 0 && g.projDepth[1] != 0;
    g.shipReady.store(depthOk);
    if (depthOk) {
        LOGI("ship mesh ready: %u verts, %u indices (%u tris)", nv, ni, ni / 3);
    } else {
        LOGW("ship mesh loaded, but eye depth is unavailable; keeping the 2D sprite");
    }
    return true;
}

/** Creates the two per-eye projection swapchains + FBOs, after xrCreateSession. */
bool createProjectionSwapchain() {
    if (g.projReady) return true;
    if (g.projFailed) return false;

    uint32_t count = 0;
    if (XR_FAILED(xrEnumerateViewConfigurationViews(g.instance, g.systemId,
                                                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                    0, &count, nullptr)) || count < 2) {
        LOGE("no stereo view configuration (%u views)", count);
        g.projFailed = true;
        return false;
    }
    XrViewConfigurationView configs[2] = {
            {XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    if (XR_FAILED(xrEnumerateViewConfigurationViews(g.instance, g.systemId,
                                                    XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                                    2, &count, configs)) || count < 2) {
        LOGE("xrEnumerateViewConfigurationViews failed");
        g.projFailed = true;
        return false;
    }

    uint32_t formatCount = 0;
    if (XR_FAILED(xrEnumerateSwapchainFormats(g.session, 0, &formatCount, nullptr)) ||
        formatCount == 0) {
        LOGE("no swapchain formats for projection");
        g.projFailed = true;
        return false;
    }
    std::vector<int64_t> formats(formatCount);
    xrEnumerateSwapchainFormats(g.session, formatCount, &formatCount, formats.data());
    int64_t chosen = 0;
    for (int64_t f : formats) {
        if (f == GL_RGBA8) { chosen = f; break; }
    }
    if (chosen == 0) chosen = formats[0];

    // Full recommended eye resolution: the central screen is now sharp and the panorama is
    // no longer upscaled, so 0.8 was visibly soft. Drop to 0.85 if the device drops frames.
    const float renderScale = 0.9f;

    for (int eye = 0; eye < 2; eye++) {
        const uint32_t w = (uint32_t)std::max(1.f, configs[eye].recommendedImageRectWidth * renderScale);
        const uint32_t h = (uint32_t)std::max(1.f, configs[eye].recommendedImageRectHeight * renderScale);
        g.eyeW[eye] = w;
        g.eyeH[eye] = h;

        XrSwapchainCreateInfo info{XR_TYPE_SWAPCHAIN_CREATE_INFO};
        info.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT;
        info.format = chosen;
        info.sampleCount = 1;
        info.width = w;
        info.height = h;
        info.faceCount = 1;
        info.arraySize = 1;
        info.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(g.session, &info, &g.projSwapchain[eye]))) {
            setError("xrCreateSwapchain(projection eye %d) failed", eye);
            g.projFailed = true;
            return false;
        }

        uint32_t imageCount = 0;
        if (XR_FAILED(xrEnumerateSwapchainImages(g.projSwapchain[eye], 0, &imageCount, nullptr)) ||
            imageCount == 0) {
            LOGE("projection eye %d has no images", eye);
            g.projFailed = true;
            return false;
        }
        g.projImages[eye].assign(
                imageCount, XrSwapchainImageOpenGLESKHR{XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
        if (XR_FAILED(xrEnumerateSwapchainImages(
                g.projSwapchain[eye], imageCount, &imageCount,
                reinterpret_cast<XrSwapchainImageBaseHeader *>(g.projImages[eye].data())))) {
            LOGE("xrEnumerateSwapchainImages(projection eye %d) failed", eye);
            g.projFailed = true;
            return false;
        }

        // One shared depth renderbuffer per eye. It is attached to every image FBO; because
        // only one image per eye is rendered at a time that is sufficient (the compositor
        // never reads application depth here).
        glGenRenderbuffers(1, &g.projDepth[eye]);
        glBindRenderbuffer(GL_RENDERBUFFER, g.projDepth[eye]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, (GLsizei)w, (GLsizei)h);
        if (glGetError() != GL_NO_ERROR) {
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT16, (GLsizei)w, (GLsizei)h);
        }
        glBindRenderbuffer(GL_RENDERBUFFER, 0);

        g.projFbos[eye].resize(imageCount, 0);
        for (uint32_t i = 0; i < imageCount; i++) {
            GLuint fbo = 0;
            glGenFramebuffers(1, &fbo);
            glBindFramebuffer(GL_FRAMEBUFFER, fbo);
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D,
                                   g.projImages[eye][i].image, 0);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER,
                                      g.projDepth[eye]);
            if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                // Retry colour-only: the panorama still works; the ship pass loses depth.
                glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, 0);
                if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
                    LOGE("projection FBO incomplete (eye %d image %u)", eye, i);
                    glDeleteFramebuffers(1, &fbo);
                    glBindFramebuffer(GL_FRAMEBUFFER, 0);
                    g.projFailed = true;
                    return false;
                }
                if (g.projDepth[eye] != 0) {
                    glDeleteRenderbuffers(1, &g.projDepth[eye]);
                    g.projDepth[eye] = 0;
                }
            }
            glViewport(0, 0, (GLsizei)w, (GLsizei)h);
            glClearColor(0.f, 0.f, 0.f, 1.f);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            g.projFbos[eye][i] = fbo;
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        LOGI("projection eye %d: %ux%u, %u images, format 0x%llx", eye, w, h, imageCount,
             (unsigned long long)chosen);
    }

    if (!ensurePanoProgram()) {
        g.projFailed = true;
        return false;
    }
    if (!ensureShipProgram()) {
        g.projFailed = true;
        return false;
    }

    g.projReady = true;
    LOGI("fullscreen projection ready (eyes %ux%u and %ux%u)", g.eyeW[0], g.eyeH[0], g.eyeW[1],
         g.eyeH[1]);
    return true;
}

/** Draws the cylindrical game panorama for one eye into the currently bound eye FBO. */
void drawPano(int eye, const XrView &view, int captureBuffer) {
    const XrFovf &fov = view.fov;
    const XrPosef &pose = view.pose;
    glUseProgram(g.panoProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, g.captureTex[captureBuffer]);
    glUniform1i(g.panoTex, 0);
    glUniform4f(g.panoTan, std::tan(fov.angleLeft), std::tan(fov.angleRight),
                std::tan(fov.angleDown), std::tan(fov.angleUp));
    glUniform4f(g.panoEyeQuat, pose.orientation.x, pose.orientation.y, pose.orientation.z,
                pose.orientation.w);
    glUniform3f(g.panoEyePos, pose.position.x, pose.position.y, pose.position.z);
    glUniform3f(g.panoRefCenter, g.panoCenter.x, g.panoCenter.y, g.panoCenter.z);
    glUniform3f(g.panoRefRight, std::cos(g.panoYaw), 0.f, -std::sin(g.panoYaw));
    glUniform3f(g.panoRefFwd, -std::sin(g.panoYaw), 0.f, -std::cos(g.panoYaw));
    glUniform1f(g.panoRadiusLoc, g.panoRadius);
    glUniform2f(g.panoSpan, g.panoSpanH, g.panoSpanV);
    glUniform4f(g.panoFade, 0.004f, 0.060f, 0.62f, 1.02f);
    glBindVertexArray(g.fullscreenVao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindVertexArray(0);
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
    (void)eye;
}

/** Draws the player's Alpha ship in true stereo, anchored to the map UV from Java. */
void drawShip(int eye, const XrView &view) {
    if (!g.shipReady.load() || g.shipIndexCount <= 0) return;
    if (!g.playerVisible.load()) return;

    const float pu = clampf(g.playerU.load(), 0.f, 1.f);
    const float pv = clampf(g.playerV.load(), 0.f, 1.f);
    const PanoView pano{g.panoCenter, g.panoYaw, g.panoRadius, g.panoSpanH, g.panoSpanV};
    const Vec3 dir = panoDirection(pano, pu, pv);

    // Put the ship in front of the cylinder so it gains real stereo disparity against the map.
    const float dist = g.panoRadius * 0.82f;

    // 3/4 hero view of the Alpha. The nose starts facing the viewer (the pose users liked)
    // and yaws with the stick/travel heading, so movement visibly turns the 3D model.
    const float bob = 0.f;
    Vec3 pos{g.panoCenter.x + dir.x * dist,
             g.panoCenter.y + dir.y * dist + bob - 0.02f,
             g.panoCenter.z + dir.z * dist};

    const float faceYaw = std::atan2(dir.x, dir.z);
    const float rotRad = g.playerRot.load() * 0.01745329252f;
    const Mat4 model = mat4Mul(
            mat4Mul(
                    mat4Mul(
                            mat4Mul(mat4Translation(pos.x, pos.y, pos.z),
                                    mat4RotationY(faceYaw + rotRad)),
                            mat4RotationX(-0.65f)),
                    mat4RotationZ(std::sin(rotRad) * 0.05f)),
            mat4Scale(g.shipScale * clampf(g.playerZoom.load(), 0.4f, 4.f)));

    const Quat eyeQ{view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z,
                    view.pose.orientation.w};
    const Vec3 eyeP{view.pose.position.x, view.pose.position.y, view.pose.position.z};
    const Mat4 viewMat = mat4View(eyeQ, eyeP);
    const Mat4 proj = mat4ProjectionFov(std::tan(view.fov.angleLeft), std::tan(view.fov.angleRight),
                                        std::tan(view.fov.angleDown), std::tan(view.fov.angleUp),
                                        0.05f, 50.f);
    const Mat4 mvp = mat4Mul(mat4Mul(proj, viewMat), model);

    glUseProgram(g.shipProg);
    glUniformMatrix4fv(g.shipMvp, 1, GL_FALSE, mvp.m);
    glBindVertexArray(g.shipVao);
    glDrawElements(GL_TRIANGLES, g.shipIndexCount, GL_UNSIGNED_SHORT, nullptr);
    glBindVertexArray(0);
    glUseProgram(0);
    (void)eye;
}

/** Renders both eyes and fills g.projLayer. False means the caller must fall back to the quad. */
bool renderProjection(const XrFrameState &frameState) {
    if (!g.projReady || !g.panoRefValid || !ensurePanoProgram()) return false;

    XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
    locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    locate.displayTime = frameState.predictedDisplayTime;
    locate.space = g.localSpace;

    XrViewState state{XR_TYPE_VIEW_STATE};
    uint32_t viewCount = 0;
    if (XR_FAILED(xrLocateViews(g.session, &locate, &state, 2, &viewCount, g.projViews)) ||
        viewCount < 2) {
        if ((g.projLog++ % 120) == 0) LOGW("xrLocateViews failed; falling back to quad");
        return false;
    }
    const XrViewStateFlags needed =
            XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT;
    if ((state.viewStateFlags & needed) != needed) {
        if ((g.projLog++ % 120) == 0) LOGW("views not valid (flags 0x%llx); quad fallback",
                                           (unsigned long long)state.viewStateFlags);
        return false;
    }

    uint32_t imageIndex[2] = {0, 0};
    bool acquired[2] = {false, false};
    for (int eye = 0; eye < 2; eye++) {
        XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(g.projSwapchain[eye], &acquire, &imageIndex[eye]))) {
            LOGE("xrAcquireSwapchainImage(projection eye %d) failed", eye);
            for (int e2 = 0; e2 < 2; e2++) {
                if (acquired[e2]) {
                    XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    xrReleaseSwapchainImage(g.projSwapchain[e2], &rel);
                }
            }
            g.projectionActive.store(false);
            return false;
        }
        acquired[eye] = true;
        XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wait.timeout = XR_INFINITE_DURATION;
        if (XR_FAILED(xrWaitSwapchainImage(g.projSwapchain[eye], &wait))) {
            LOGE("xrWaitSwapchainImage(projection eye %d) failed", eye);
            for (int e2 = 0; e2 < 2; e2++) {
                if (acquired[e2]) {
                    XrSwapchainImageReleaseInfo rel{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
                    xrReleaseSwapchainImage(g.projSwapchain[e2], &rel);
                }
            }
            g.projectionActive.store(false);
            return false;
        }
    }

    // Consume the newest capture only after both eye images are secured, so a failure here
    // can still fall back to the quad with a fresh frame.
    if (g.frameReady.exchange(false)) {
        const int published = g.capturePublished.exchange(-1);
        if (published >= 0) g.lastCapture = published;
    }
    const int buffer = g.lastCapture;

    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glClearColor(0.f, 0.f, 0.f, 1.f);

    for (int eye = 0; eye < 2; eye++) {
        glBindFramebuffer(GL_FRAMEBUFFER, g.projFbos[eye][imageIndex[eye]]);
        glViewport(0, 0, (GLsizei)g.eyeW[eye], (GLsizei)g.eyeH[eye]);
        glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
        drawPano(eye, g.projViews[eye], buffer);
        if (g.projDepth[eye] != 0 && g.shipReady.load() && g.playerVisible.load()) {
            glEnable(GL_DEPTH_TEST);
            glDepthMask(GL_TRUE);
            glClear(GL_DEPTH_BUFFER_BIT);
            drawShip(eye, g.projViews[eye]);
            glDisable(GL_DEPTH_TEST);
            glDepthMask(GL_FALSE);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);

    for (int eye = 0; eye < 2; eye++) {
        XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        if (XR_FAILED(xrReleaseSwapchainImage(g.projSwapchain[eye], &release))) {
            LOGW("xrReleaseSwapchainImage(projection eye %d) failed", eye);
        }
    }

    for (int eye = 0; eye < 2; eye++) {
        g.projLayerViews[eye] = XrCompositionLayerProjectionView{XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        g.projLayerViews[eye].pose = g.projViews[eye].pose;
        g.projLayerViews[eye].fov = g.projViews[eye].fov;
        g.projLayerViews[eye].subImage.swapchain = g.projSwapchain[eye];
        g.projLayerViews[eye].subImage.imageRect.offset = {0, 0};
        g.projLayerViews[eye].subImage.imageRect.extent = {(int32_t)g.eyeW[eye],
                                                          (int32_t)g.eyeH[eye]};
        g.projLayerViews[eye].subImage.imageArrayIndex = 0;
    }
    g.projLayer.space = g.localSpace;
    g.projLayer.layerFlags = 0;
    g.projLayer.viewCount = 2;
    g.projLayer.views = g.projLayerViews;
    g.projectionActive.store(true);

    if (g.projLog++ % 600 == 0) {
        LOGI("mode=PROJ eyes %ux%u %ux%u ship=%d player=%.2f,%.2f ipd=%.3f m", g.eyeW[0],
             g.eyeH[0], g.eyeW[1], g.eyeH[1], (int)g.shipReady.load(), g.playerU.load(),
             g.playerV.load(),
             std::sqrt(std::pow(g.projViews[1].pose.position.x - g.projViews[0].pose.position.x, 2) +
                       std::pow(g.projViews[1].pose.position.y - g.projViews[0].pose.position.y, 2) +
                       std::pow(g.projViews[1].pose.position.z - g.projViews[0].pose.position.z, 2)));
    }
    return true;
}

/** Converts a controller aim pose into normalized screen coordinates on the virtual screen. */
bool rayToScreen(const XrPosef &aim, float &outU, float &outV) {
    const Pose pose{
            Quat{aim.orientation.x, aim.orientation.y, aim.orientation.z, aim.orientation.w},
            Vec3{aim.position.x, aim.position.y, aim.position.z},
    };

    // The screen can be moved/rotated by the recenter action, so use its live pose.
    const ScreenPose screen{
            Quat{g.quadPose.orientation.x, g.quadPose.orientation.y, g.quadPose.orientation.z,
                 g.quadPose.orientation.w},
            Vec3{g.quadPose.position.x, g.quadPose.position.y, g.quadPose.position.z},
            g.quadWidth, g.quadHeight,
    };
    return vr::rayToScreen(pose, screen, outU, outV);
}

/** Moves the virtual screen 2.1 m in front of the current head yaw. */
void recenter(JNIEnv *env, XrTime time){
    if(g.viewSpace == XR_NULL_HANDLE || g.localSpace == XR_NULL_HANDLE) return;

    XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
    if(XR_FAILED(xrLocateSpace(g.viewSpace, g.localSpace, time, &head))) return;
    if(!(head.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return;
    if(!(head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return;

    const XrQuaternionf q = head.pose.orientation;
    const float yaw = atan2f(2.f * (q.w * q.y + q.x * q.z), 1.f - 2.f * (q.y * q.y + q.x * q.x));

    // Place the screen along the head's forward vector and rotate it to face the user.
    const ScreenPose pose = vr::recenteredScreen(
            yaw, Vec3{head.pose.position.x, head.pose.position.y, head.pose.position.z},
            -g.quadZ, g.quadWidth, g.quadHeight);

    g.quadPose.orientation = {pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w};
    g.quadPose.position = {pose.position.x, pose.position.y, pose.position.z};

    const Vec3 fwd = rotate(Quat{q.x, q.y, q.z, q.w}, Vec3{0.f, 0.f, -1.f});
    g.refYaw = yaw;
    g.refPitch = std::asin(clampf(fwd.y, -1.f, 1.f));
    g.smoothRight = 0.f;
    g.smoothUp = 0.f;
    g.lookRefValid = true;

    // The panorama shares the recenter event: its cylinder is centred on the user and its
    // forward seam follows the same yaw. A head lean then produces correct parallax, and
    // looking forward always puts the game camera centre dead ahead.
    g.panoCenter = Vec3{head.pose.position.x, head.pose.position.y, head.pose.position.z};
    g.panoYaw = yaw;
    g.panoRefValid = true;

    LOGI("recentered screen: yaw %.2f pos (%.2f, %.2f, %.2f); pano ref set", yaw,
         g.quadPose.position.x, g.quadPose.position.y, g.quadPose.position.z);
    notifyEvent(env, "recentered");
}

void renderFrame(JNIEnv *env, const XrFrameState &frameState) {
    // Flat screen sizes: mode 1 is the compact centred panel, mode 2 fills more of the view.
    const int vm = g.viewMode.load();
    if (vm == 2) {
        g.quadWidth = 5.2f;
        g.quadHeight = 3.12f;
    } else if (vm == 1) {
        g.quadWidth = 4.4f;
        g.quadHeight = 2.64f;
    }
    // Auto-place the screen in front of the user whenever the session gains focus; this covers
    // launching the app while the headset is off the head, or any local-space orientation drift.
    // Entering an in-game world also asks for a fresh panorama centre.
    const bool panoNeedsRef = g.worldView.load() && !g.panoRefValid;
    if (g.pendingRecenter || g.recenterRequested.exchange(false) ||
        g.panoRecenterRequested.exchange(false) || panoNeedsRef) {
        g.pendingRecenter = false;
        recenter(env, frameState.predictedDisplayTime);
    }

    XrActiveActionSet activeActionSet{g.actionSet, XR_NULL_PATH};
    XrActionsSyncInfo syncInfo{XR_TYPE_ACTIONS_SYNC_INFO};
    syncInfo.countActiveActionSets = 1;
    syncInfo.activeActionSets = &activeActionSet;
    xrSyncActions(g.session, &syncInfo);

    bool down = false;
    float stickX = 0.f, stickY = 0.f;
    float u = -1.f, v = -1.f;

    XrActionStateGetInfo getInfo{XR_TYPE_ACTION_STATE_GET_INFO};
    getInfo.action = g.aimAction;
    // The actions are bound to the right-hand subaction, so query with that path; passing
    // XR_NULL_PATH leaves hand-bound float/vector actions inactive on some runtimes.
    getInfo.subactionPath = g.handPath;

    const bool panoMode = g.viewMode.load() == 0 && g.worldView.load() && g.projReady && g.panoRefValid;

    XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
    if (XR_SUCCEEDED(xrLocateSpace(g.aimSpace, g.localSpace, frameState.predictedDisplayTime, &location)) &&
        (location.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
        (location.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        if (panoMode) {
            const PanoView pano{g.panoCenter, g.panoYaw, g.panoRadius, g.panoSpanH, g.panoSpanV};
            const Pose aim{Quat{location.pose.orientation.x, location.pose.orientation.y,
                                location.pose.orientation.z, location.pose.orientation.w},
                           Vec3{location.pose.position.x, location.pose.position.y,
                                location.pose.position.z}};
            rayToPano(aim, pano, u, v);
        } else {
            rayToScreen(location.pose, u, v);
        }
    }

    XrActionStateFloat trigger{XR_TYPE_ACTION_STATE_FLOAT};
    getInfo.action = g.triggerAction;
    if (XR_SUCCEEDED(xrGetActionStateFloat(g.session, &getInfo, &trigger)) && trigger.isActive) {
        down = trigger.currentState > 0.5f;
        if (!g.loggedTriggerActive) {
            g.loggedTriggerActive = true;
            LOGI("trigger active (state %.2f)", trigger.currentState);
        }
    }
    if (down != g.triggerDown) {
        LOGI("trigger %s (active=%d state=%.2f)", down ? "down" : "up", (int)trigger.isActive,
             trigger.currentState);
    }

    XrActionStateVector2f stick{XR_TYPE_ACTION_STATE_VECTOR2F};
    getInfo.action = g.stickAction;
    if (XR_SUCCEEDED(xrGetActionStateVector2f(g.session, &getInfo, &stick)) && stick.isActive) {
        stickX = stick.currentState.x;
        stickY = stick.currentState.y;
    }

    // Short haptic tick on the trigger edge, so taps have physical feedback.
    if(down && !g.triggerDown && g.hapticAction != XR_NULL_HANDLE){
        XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
        vibration.duration = XR_MIN_HAPTIC_DURATION;
        vibration.frequency = XR_FREQUENCY_UNSPECIFIED;
        vibration.amplitude = 0.35f;

        XrHapticActionInfo hapticInfo{XR_TYPE_HAPTIC_ACTION_INFO};
        hapticInfo.action = g.hapticAction;
        hapticInfo.subactionPath = g.handPath;
        xrApplyHapticFeedback(g.session, &hapticInfo,
                              reinterpret_cast<const XrHapticBaseHeader *>(&vibration));
    }
    g.triggerDown = down;

    // Grip (squeeze) recenters the virtual screen in front of the user.
    XrActionStateFloat grip{XR_TYPE_ACTION_STATE_FLOAT};
    getInfo.action = g.gripAction;
    bool gripNow = false;
    if(XR_SUCCEEDED(xrGetActionStateFloat(g.session, &getInfo, &grip)) && grip.isActive){
        gripNow = grip.currentState > 0.5f;
    }
    if(gripNow && !g.gripDown){
        recenter(env, frameState.predictedDisplayTime);
    }
    g.gripDown = gripNow;

    // Left thumbstick -> movement. The right stick above is camera zoom.
    XrActionStateVector2f moveStick{XR_TYPE_ACTION_STATE_VECTOR2F};
    getInfo.action = g.moveStickAction;
    getInfo.subactionPath = g.leftHandPath;
    float moveX = 0.f, moveY = 0.f;
    if (XR_SUCCEEDED(xrGetActionStateVector2f(g.session, &getInfo, &moveStick)) && moveStick.isActive) {
        moveX = moveStick.currentState.x;
        moveY = moveStick.currentState.y;
    }
    notifyMove(env, moveX, moveY);

    XrActionStateBoolean viewState{XR_TYPE_ACTION_STATE_BOOLEAN};
    getInfo.action = g.viewToggleAction;
    getInfo.subactionPath = g.leftHandPath;
    if (XR_SUCCEEDED(xrGetActionStateBoolean(g.session, &getInfo, &viewState)) && viewState.isActive) {
        const bool now = viewState.currentState == XR_TRUE;
        if (now && !g.viewToggleDown) notifyViewToggle(env);
        g.viewToggleDown = now;
    }
    // Restore the right-hand path for any later action queries.
    getInfo.subactionPath = g.handPath;

    // Flat-VR look-around (quad menus / fallback only): pan the overscan crop with the head.
    // The panorama is already world-fixed and must not double-apply a crop.
    if (!panoMode && g.lookAround.load() && g.viewSpace != XR_NULL_HANDLE) {
        XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
        if (XR_SUCCEEDED(xrLocateSpace(g.viewSpace, g.localSpace, frameState.predictedDisplayTime, &head)) &&
            (head.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
            const XrQuaternionf q = head.pose.orientation;
            const float yaw = atan2f(2.f * (q.w * q.y + q.x * q.z), 1.f - 2.f * (q.y * q.y + q.x * q.x));
            const Vec3 fwd = rotate(Quat{q.x, q.y, q.z, q.w}, Vec3{0.f, 0.f, -1.f});
            const float pitch = std::asin(clampf(fwd.y, -1.f, 1.f));
            if (!g.lookRefValid) { g.refYaw = yaw; g.refPitch = pitch; g.lookRefValid = true; }
            g.smoothRight += ((g.refYaw - yaw) - g.smoothRight) * 0.12f;
            g.smoothUp += ((pitch - g.refPitch) - g.smoothUp) * 0.12f;
            g.crop = lookCrop(g.smoothRight, g.smoothUp, 0.86f, 0.70f);
            g.backCrop = lookCrop(g.smoothRight * 1.5f, g.smoothUp * 1.5f, 0.92f, 0.61f);
            g.cropValid = true;
            if ((g.lookLog++ % 600) == 0) {
                LOGI("lookaround crop %.2f,%.2f look %.2f,%.2f", g.crop.x, g.crop.y,
                     g.smoothRight, g.smoothUp);
            }
        }
        if (u >= 0.f && v >= 0.f && g.cropValid) {
            u = g.crop.x + u * g.crop.w;
            v = g.crop.y + v * g.crop.h;
        }
    }

    notifyInput(env, u, v, down, stickX, stickY);

    if (u >= 0.f && v >= 0.f) {
        g.cursorX.store((int)(u * (float)g.texW));
        g.cursorY.store((int)((1.f - v) * (float)g.texH));
    } else {
        g.cursorX.store(-1);
    }

}

/** Copies the newest capture into the quad swapchain. True when an image was written. */
bool submitQuad() {
    if (!g.frameReady.exchange(false)) return false;

    int buffer = g.capturePublished.exchange(-1);
    if (buffer >= 0) {
        g.lastCapture = buffer;
    } else {
        buffer = g.lastCapture;
    }

    uint32_t imageIndex = 0;
    XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(g.swapchain, &acquire, &imageIndex))) {
        LOGW("xrAcquireSwapchainImage(quad) failed");
        return false;
    }

    bool wrote = false;
    XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout = XR_INFINITE_DURATION;
    if (XR_SUCCEEDED(xrWaitSwapchainImage(g.swapchain, &wait))) {
        if (!g.loggedCaptureSample && g.capturedFrames > 300) {
            g.loggedCaptureSample = true;
            glBindFramebuffer(GL_READ_FRAMEBUFFER, g.captureFbo[buffer]);
            unsigned char px[16] = {0};
            glReadPixels(g.texW / 2, g.texH / 2, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, px);
            LOGI("capture centre RGBA: %d,%d,%d,%d", px[0], px[1], px[2], px[3]);
        }
        if (g.lookAround.load()) blitFade(imageIndex, buffer); else blitCapture(imageIndex, buffer);
        if (!g.loggedBlitError) {
            GLenum blitError = glGetError();
            if (blitError != GL_NO_ERROR) {
                g.loggedBlitError = true;
                LOGW("GL error after blit: 0x%x", blitError);
            }
        }
        wrote = true;
    } else {
        LOGW("xrWaitSwapchainImage(quad) failed");
    }

    // Release even on wait failure: the image was acquired and must go back.
    XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g.swapchain, &release);
    return wrote;
}

/** Destroys XR objects and the GL resources created on the XR thread. */
void cleanupXr(){
    if (g.sessionRunning && g.session != XR_NULL_HANDLE) {
        xrEndSession(g.session);
        g.sessionRunning = false;
    }
    if (g.aimSpace != XR_NULL_HANDLE) xrDestroySpace(g.aimSpace);
    if (g.viewSpace != XR_NULL_HANDLE) xrDestroySpace(g.viewSpace);
    if (g.localSpace != XR_NULL_HANDLE) xrDestroySpace(g.localSpace);
    if (g.actionSet != XR_NULL_HANDLE) xrDestroyActionSet(g.actionSet);

    if (g.xrContext != EGL_NO_CONTEXT) {
        if (g.fadeProg) { glDeleteProgram(g.fadeProg); g.fadeProg = 0; }
        if (g.panoProg) { glDeleteProgram(g.panoProg); g.panoProg = 0; }
        if (g.shipProg) { glDeleteProgram(g.shipProg); g.shipProg = 0; }
        if (g.fullscreenVao) { glDeleteVertexArrays(1, &g.fullscreenVao); g.fullscreenVao = 0; }
        if (g.shipVao) { glDeleteVertexArrays(1, &g.shipVao); g.shipVao = 0; }
        if (g.shipVbo) { glDeleteBuffers(1, &g.shipVbo); g.shipVbo = 0; }
        if (g.shipEbo) { glDeleteBuffers(1, &g.shipEbo); g.shipEbo = 0; }
        for (GLuint fbo : g.swapFbos) {
            if (fbo) glDeleteFramebuffers(1, &fbo);
        }
        for (int i = 0; i < 2; i++) {
            if (g.captureFbo[i]) glDeleteFramebuffers(1, &g.captureFbo[i]);
            for (GLuint fbo : g.projFbos[i]) {
                if (fbo) glDeleteFramebuffers(1, &fbo);
            }
            if (g.projDepth[i]) {
                glDeleteRenderbuffers(1, &g.projDepth[i]);
                g.projDepth[i] = 0;
            }
        }
    }

    if (g.swapchain != XR_NULL_HANDLE) xrDestroySwapchain(g.swapchain);
    for (int i = 0; i < 2; i++) {
        if (g.projSwapchain[i] != XR_NULL_HANDLE) xrDestroySwapchain(g.projSwapchain[i]);
    }
    if (g.session != XR_NULL_HANDLE) xrDestroySession(g.session);
    if (g.instance != XR_NULL_HANDLE) xrDestroyInstance(g.instance);

    g.aimSpace = XR_NULL_HANDLE;
    g.viewSpace = XR_NULL_HANDLE;
    g.localSpace = XR_NULL_HANDLE;
    g.actionSet = XR_NULL_HANDLE;
    g.swapchain = XR_NULL_HANDLE;
    g.session = XR_NULL_HANDLE;
    g.instance = XR_NULL_HANDLE;
    g.swapFbos.clear();
    g.swapImages.clear();
    g.projFbos[0].clear();
    g.projFbos[1].clear();
    g.projImages[0].clear();
    g.projImages[1].clear();
    for (int i = 0; i < 2; i++) {
        g.captureFbo[i] = 0;
        g.projSwapchain[i] = XR_NULL_HANDLE;
    }
    g.projReady = false;
    g.projFailed = false;
    g.shipReady.store(false);
    g.projectionActive.store(false);
    g.shipIndexCount = 0;
    g.panoRefValid = false;
}

/** Releases the XR thread's EGL context/surface and detaches EGL from this thread. */
void cleanupEgl(){
    if (g.display != EGL_NO_DISPLAY) {
        eglMakeCurrent(g.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (g.pbuffer != EGL_NO_SURFACE) eglDestroySurface(g.display, g.pbuffer);
        if (g.xrContext != EGL_NO_CONTEXT) eglDestroyContext(g.display, g.xrContext);
        eglReleaseThread();
    }
    g.pbuffer = EGL_NO_SURFACE;
    g.xrContext = EGL_NO_CONTEXT;
    g.xrConfig = nullptr;
}

void threadMain() {
    JNIEnv *env = nullptr;
    bool attached = false;
    if (g.vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_EDETACHED) {
        if (g.vm->AttachCurrentThread(&env, nullptr) == JNI_OK) attached = true;
    }

    bool ok = createSharedContext();
    if (!ok) {
        notifyEvent(env, "egl-failed");
    } else if (!createSession()) {
        notifyEvent(env, "openxr-failed");
        ok = false;
    }

    std::vector<const XrCompositionLayerBaseHeader *> layers;

    if (ok) {
        g.active = true;
        refreshStatus();
        notifyEvent(env, "ready");

    while (g.running && !g.exitRequested) {
        pollEvents(env);

        if (!g.sessionRunning) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        XrFrameState frameState{XR_TYPE_FRAME_STATE};
        XrFrameWaitInfo waitInfo{XR_TYPE_FRAME_WAIT_INFO};
        if (XR_FAILED(xrWaitFrame(g.session, &waitInfo, &frameState))) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        XrFrameBeginInfo beginInfo{XR_TYPE_FRAME_BEGIN_INFO};
        if (XR_FAILED(xrBeginFrame(g.session, &beginInfo))) continue;

        layers.clear();
        XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};

        if (frameState.shouldRender) {
            renderFrame(env, frameState);

            const bool projection = g.viewMode.load() == 0 && g.worldView.load() &&
                                    !g.solidDebug && renderProjection(frameState);
            if (projection) {
                layers.push_back(
                        reinterpret_cast<const XrCompositionLayerBaseHeader *>(&g.projLayer));
            } else if (submitQuad()) {
                g.projectionActive.store(false);

                quad.space = g.localSpace;
                quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
                quad.subImage.swapchain = g.swapchain;
                quad.subImage.imageRect.offset = {0, 0};
                quad.subImage.imageRect.extent = {(int32_t)g.swapW, (int32_t)g.swapH};
                quad.subImage.imageArrayIndex = 0;
                quad.pose = g.quadPose;
                quad.size.width = g.quadWidth;
                quad.size.height = g.quadHeight;
                quad.layerFlags = 0;

                layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader *>(&quad));
            } else {
                g.projectionActive.store(false);
            }
        }

        XrFrameEndInfo endInfo{XR_TYPE_FRAME_END_INFO};
        endInfo.displayTime = frameState.predictedDisplayTime;
        endInfo.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        endInfo.layerCount = (uint32_t)layers.size();
        endInfo.layers = layers.empty() ? nullptr : layers.data();

        XrResult endResult = xrEndFrame(g.session, &endInfo);
        if (XR_SUCCEEDED(endResult)) {
            g.submittedFrames++;
            if (g.submittedFrames == 1 || (g.submittedFrames % 600) == 0) {
                LOGI("frame %llu submitted (mode=%s cursor %d,%d, eyes %ux%u ship=%d)",
                     (unsigned long long)g.submittedFrames,
                     g.projectionActive.load() ? "PROJ" : "QUAD", g.cursorX.load(),
                     g.cursorY.load(), g.eyeW[0], g.eyeH[0], (int)g.shipReady.load());
            }
        } else if ((g.endFrameLog++ % 120) == 0) {
            LOGW("xrEndFrame failed: %d", (int)endResult);
        }
    }
    }

    LOGI("OpenXR thread exiting");
    g.active = false;
    g.running = false;
    refreshStatus();

    cleanupXr();
    cleanupEgl();

    if (attached) g.vm->DetachCurrentThread();
}

} // namespace

bool isActive() { return g.active.load(); }

void setSolidDebug(bool solid) { g.solidDebug = solid; }

void setLookAround(bool enabled) { g.lookAround.store(enabled); }

void setViewMode(int mode) {
    g.viewMode.store(mode == 2 ? 2 : (mode == 1 ? 1 : 0));
    LOGI("view mode -> %s", mode == 2 ? "FLAT FULL (quad 5.2m)" : (mode == 1 ? "FLAT (quad 4.4m)" : "PANO (projection)"));
}

void setWorldView(bool enabled) {
    const bool previous = g.worldView.exchange(enabled);
    if (enabled && !previous) {
        g.panoRecenterRequested.store(true);
        LOGI("world view ON: fullscreen panorama requested");
    } else if (!enabled && previous) {
        g.projectionActive.store(false);
        LOGI("world view OFF: returning to the quad screen");
    }
}

bool worldViewActive() { return g.projectionActive.load(); }

bool shipReady() { return g.shipReady.load(); }

void setPlayerPose(float u, float v, float rotation, float zoom, bool visible) {
    g.playerU.store(u);
    g.playerV.store(v);
    g.playerRot.store(rotation);
    g.playerZoom.store(zoom);
    g.playerVisible.store(visible);
}

void requestRecenter() { g.recenterRequested.store(true); }

bool sessionRunning() { return g.sessionRunning.load(); }

const char *statusString() { return statusBuf; }

const char *lastError() { return errorBuf; }

bool start(JavaVM *vm, JNIEnv *env, jobject activity, jlong eglDisplay, jlong eglContext,
           jint texWidth, jint texHeight) {
    if (g.running.load()) return g.active.load();

    if (vm == nullptr || activity == nullptr || texWidth <= 0 || texHeight <= 0) {
        setError("invalid parameters for VR start");
        return false;
    }

    g.vm = vm;
    g.display = (EGLDisplay)eglDisplay;
    g.gameContext = (EGLContext)eglContext;
    g.texW = texWidth;
    g.texH = texHeight;

    if (g.activity != nullptr) env->DeleteGlobalRef(g.activity);
    g.activity = env->NewGlobalRef(activity);

    // Cache the Java callback surface so the XR thread can talk back without FindClass.
    jclass cls = env->FindClass("mindustry/android/vr/VrSession");
    if (cls == nullptr) {
        setError("VrSession class not found");
        return false;
    }
    if (g.sessionClass != nullptr) env->DeleteGlobalRef(g.sessionClass);
    g.sessionClass = static_cast<jclass>(env->NewGlobalRef(cls));
    env->DeleteLocalRef(cls);

    g.onInputMethod = env->GetStaticMethodID(g.sessionClass, "onVrInput", "(FFZFF)V");
    g.onEventMethod = env->GetStaticMethodID(g.sessionClass, "onVrEvent", "(Ljava/lang/String;)V");
    g.onMoveMethod = env->GetStaticMethodID(g.sessionClass, "onVrMove", "(FF)V");
    g.onViewToggleMethod = env->GetStaticMethodID(g.sessionClass, "onVrViewToggle", "()V");
    if (g.onInputMethod == nullptr || g.onEventMethod == nullptr || g.onMoveMethod == nullptr) {
        setError("VrSession callback methods missing");
        return false;
    }

    // Capture textures live in libGDX's context; the XR context shares them.
    for (int i = 0; i < 2; i++) {
        glGenTextures(1, &g.captureTex[i]);
        if (g.captureTex[i] == 0) {
            setError("could not create capture texture %d", i);
            return false;
        }
        glBindTexture(GL_TEXTURE_2D, g.captureTex[i]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, g.texW, g.texH, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

        // Clear to opaque black so frames submitted before the first capture are not garbage.
        std::vector<unsigned char> zero((size_t)g.texW * g.texH * 4, 0);
        for (int y = 0; y < g.texH; y++) {
            for (int x = 0; x < g.texW; x++) {
                zero[((size_t)y * g.texW + x) * 4 + 3] = 255;
            }
        }
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, g.texW, g.texH, GL_RGBA, GL_UNSIGNED_BYTE, zero.data());
        glBindTexture(GL_TEXTURE_2D, 0);

        if (glGetError() != GL_NO_ERROR) {
            setError("capture texture %d creation failed", i);
            return false;
        }
    }

    // Game-context framebuffers used by the solid-colour debug mode.
    for (int i = 0; i < 2; i++) {
        glGenFramebuffers(1, &g.fillFbo[i]);
        glBindFramebuffer(GL_FRAMEBUFFER, g.fillFbo[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, g.captureTex[i], 0);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    g.captureWrite = 0;
    g.capturePublished.store(-1);
    g.frameReady.store(false);

    // Keep a global ref to the AssetManager so the XR thread can read packaged meshes.
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getAssets = env->GetMethodID(activityClass, "getAssets",
                                           "()Landroid/content/res/AssetManager;");
    if (getAssets != nullptr) {
        jobject assets = env->CallObjectMethod(activity, getAssets);
        if (assets != nullptr) {
            if (g.assetsRef != nullptr) env->DeleteGlobalRef(g.assetsRef);
            g.assetsRef = env->NewGlobalRef(assets);
            g.assets = AAssetManager_fromJava(env, assets);
            env->DeleteLocalRef(assets);
        }
    }
    env->DeleteLocalRef(activityClass);

    if (g.thread.joinable()) g.thread.join();

    g.running = true;
    g.thread = std::thread(threadMain);

    LOGI("VR thread started (%dx%d)", g.texW, g.texH);
    return true;
}

void capture(jint fbWidth, jint fbHeight) {
    if (!g.active.load() || g.captureTex[0] == 0) return;

    // Controller cursor: a small crosshair drawn straight into the back buffer before the
    // frame is copied. Cheap (scissor + clear) and needs no extra quad layer or shader.
    int cx = g.cursorX.load();
    int cy = g.cursorY.load();
    if (cx >= 0 && cy >= 0 && fbWidth > 0 && fbHeight > 0) {
        int px = cx * fbWidth / (g.texW > 0 ? g.texW : 1);
        int py = cy * fbHeight / (g.texH > 0 ? g.texH : 1);
        int glY = fbHeight - py; // switch to GL's bottom-left origin

        GLfloat clearColor[4];
        glGetFloatv(GL_COLOR_CLEAR_VALUE, clearColor);

        glEnable(GL_SCISSOR_TEST);
        glClearColor(1.f, 0.42f, 0.15f, 1.f);
        glScissor(px - 9, glY - 3, 18, 6);
        glClear(GL_COLOR_BUFFER_BIT);
        glScissor(px - 3, glY - 9, 6, 18);
        glClear(GL_COLOR_BUFFER_BIT);
        glClearColor(clearColor[0], clearColor[1], clearColor[2], clearColor[3]);
        glDisable(GL_SCISSOR_TEST);
    }

    int w = fbWidth < g.texW ? fbWidth : g.texW;
    int h = fbHeight < g.texH ? fbHeight : g.texH;

    int index = g.captureWrite;
    g.captureWrite ^= 1;

    if (g.solidDebug) {
        // Diagnostic: ignore the game and present a bright colour, isolating the layer pipeline
        // from the framebuffer-copy path.
        glBindFramebuffer(GL_FRAMEBUFFER, g.fillFbo[index]);
        glClearColor(0.10f, 0.55f, 1.00f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glFinish();
        g.capturePublished.store(index);
        g.frameReady.store(true);
        return;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    if (!g.loggedFbSample && g.capturedFrames > 300) {
        g.loggedFbSample = true;
        unsigned char px[16] = {0};
        glReadPixels(w / 2, h / 2, 2, 2, GL_RGBA, GL_UNSIGNED_BYTE, px);
        LOGI("framebuffer centre RGBA: %d,%d,%d,%d (err 0x%x)", px[0], px[1], px[2], px[3],
             glGetError());
    }

    // Some Adreno drivers reject glCopyTexSubImage2D from the window surface (GL_INVALID_OPERATION),
    // so fall back to a readback upload, which always works.
    bool copied = false;
    if (!g.useReadback) {
        while (glGetError() != GL_NO_ERROR) {
        }
        glBindTexture(GL_TEXTURE_2D, g.captureTex[index]);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);
        glBindTexture(GL_TEXTURE_2D, 0);
        copied = glGetError() == GL_NO_ERROR;

        if (!copied) {
            g.useReadback = true;
            LOGW("glCopyTexSubImage2D rejected; switching to readback capture");
        }
    }

    if (!copied) {
        g.readbackPixels.resize((size_t)w * h * 4);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, g.readbackPixels.data());
        glBindTexture(GL_TEXTURE_2D, g.captureTex[index]);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE,
                        g.readbackPixels.data());
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    // The XR context samples this texture on another thread; finish guarantees the copy
    // is complete before it is published. (Fences across contexts are driver-dependent.)
    glFinish();
    g.capturePublished.store(index);
    g.frameReady.store(true);

    g.capturedFrames++;
    if (g.capturedFrames == 1 || (g.capturedFrames % 600) == 0) {
        LOGI("capture %llu (%dx%d of %dx%d)", (unsigned long long)g.capturedFrames, w, h, g.texW, g.texH);
    }
}

void stop() {
    if (!g.running.exchange(false)) return;
    LOGI("VR stop requested");
    if (g.thread.joinable()) g.thread.join();

    // The XR thread is done with the AssetManager; drop the extra global ref.
    if (g.assetsRef != nullptr && g.vm != nullptr) {
        JNIEnv *env = nullptr;
        if (g.vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK && env != nullptr) {
            env->DeleteGlobalRef(g.assetsRef);
        }
        g.assetsRef = nullptr;
        g.assets = nullptr;
    }
    for (int i = 0; i < 2; i++) {
        // Deleting is only safe with the game's context current; stop() is called from there.
        if (g.fillFbo[i] != 0) {
            glDeleteFramebuffers(1, &g.fillFbo[i]);
            g.fillFbo[i] = 0;
        }
        if (g.captureTex[i] != 0) {
            glDeleteTextures(1, &g.captureTex[i]);
            g.captureTex[i] = 0;
        }
    }
    g.frameReady.store(false);
    g.capturePublished.store(-1);
}

} // namespace vr
