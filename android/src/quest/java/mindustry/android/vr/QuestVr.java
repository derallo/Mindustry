package mindustry.android.vr;

import android.app.Activity;
import arc.ApplicationListener;
import arc.Core;
import arc.Events;
import arc.files.Fi;
import arc.graphics.Texture;
import arc.graphics.Texture.TextureFilter;
import arc.math.Mathf;
import arc.math.geom.Vec2;
import arc.scene.Element;
import arc.scene.Group;
import arc.scene.ui.Button;
import arc.util.Log;
import arc.util.Tmp;
import mindustry.Vars;
import mindustry.android.AndroidLauncher;
import mindustry.content.UnitTypes;
import mindustry.game.EventType.ClientLoadEvent;
import mindustry.game.Rules;
import mindustry.gen.Unit;
import mindustry.io.MapIO;
import mindustry.maps.Map;

/**
 * Quest bootstrap. Loaded reflectively from the shared launcher so the stock flavor never
 * compiles or touches any VR code.
 */
public final class QuestVr{
    private static boolean initialised;

    private QuestVr(){
    }

    public static boolean isQuestDevice(){
        return AndroidLauncher.isQuestDevice();
    }

    public static synchronized void init(Activity activity){
        if(initialised) return;
        initialised = true;

        if(!isQuestDevice()){
            Log.info("VR: not a Meta Quest device, staying in flat mode");
            return;
        }

        Vars.quest = true;

        // Panorama-only baseline: the flat quad regressed controller input, flickered, and
        // removed the 3D ship. Keep it selectable only by the developer intent extra.
        if(!Core.settings.getBool("vrpanoonly", false)){
            Core.settings.put("vrview", 0);
            Core.settings.put("vrpanoonly", true);
        }

        // Dev/test: `--ei vrview 1` launches straight into the flat screen.
        if(activity.getIntent() != null && activity.getIntent().hasExtra("vrview")){
            Core.settings.put("vrview", activity.getIntent().getIntExtra("vrview", 0));
            Log.info("VR: intent view mode @", Core.settings.getInt("vrview", 0));
        }
        // Dev/test: `--ei vrglobe 0..4` launches at a fixed curvature for screenshots.
        if(activity.getIntent() != null && activity.getIntent().hasExtra("vrglobe")){
            Core.settings.put("vrglobe", activity.getIntent().getIntExtra("vrglobe", 2));
            Log.info("VR: intent globe strength @", Core.settings.getInt("vrglobe", 2));
        }

        // 9x map: render a 3x3 world area. The camera is 3x wider, so the pano's centre
        // third is the normal-scale view and the surrounding thirds hold actual extra map.
        if(Vars.renderer != null){
            Vars.renderer.minZoomInGame = 0.25f;
            Vars.renderer.setScale(Vars.renderer.getScale() / 3f);
            Vars.renderer.clampScale();
        }

        // The user explicitly asked to keep the isometric graphics mode off. The in-game VR
        // presentation is the fullscreen panorama/projection path instead; the iso renderer
        // stays a desktop experiment and must never replace the capture on Quest.
        Vars.iso3d = false;
        Core.settings.put("iso3d", false);
        Vars.vrWorldView = false;
        Vars.vrShipModel = false;
        Log.info("VR: isometric 3D forced off; fullscreen world view owns in-game presentation");

        // Legacy quad look-around stays available for the menu/fallback screen.
        VrSession.setLookAround(Core.settings.getBool("vrlookaround", false));
        Log.info("VR: quad look-around: @", Core.settings.getBool("vrlookaround", false));
        VrSession.setViewMode(Core.settings.getInt("vrview", 0));
        VrSession.setGlobeStrength(Core.settings.getInt("vrglobe", 3));
        Log.info("VR: in-game globe strength @", Core.settings.getInt("vrglobe", 3));

        String runtime = VrSession.runtimeVersion();
        Log.info("VR: Quest detected, OpenXR runtime probe: '@'", runtime);

        Core.app.addListener(new VrFrameListener(activity));
        Log.info("VR: frame listener registered");

        // Dev/test hook: launch with `--ez sandbox true` to jump straight into a map with an
        // Alpha player, so the projection/ship/movement can be exercised without a controller.
        if(activity.getIntent() != null && activity.getIntent().getBooleanExtra("sandbox", false)){
            Events.on(ClientLoadEvent.class, e -> Core.app.post(QuestVr::startSandbox));
            Log.info("VR: sandbox intent registered");
        }
        // Dev/test: `--ez settings true` opens the settings dialog so the flat-menu path
        // (dialog -> flat quad, controller aim -> screen) can be exercised.
        if(activity.getIntent() != null && activity.getIntent().getBooleanExtra("settings", false)){
            Events.on(ClientLoadEvent.class, e -> Core.app.post(() -> {
                try{
                    if(Vars.ui != null) Vars.ui.settings.show();
                }catch(Throwable t){
                    Log.err("VR: settings intent failed", t);
                }
            }));
            Log.info("VR: settings intent registered");
        }
    }

    /** Dev/test harness: load Mud Flats and give the player an Alpha. */
    private static void startSandbox(){
        try{
            Fi file = Core.files.internal("maps/default/mudFlats.msav");
            Map map = MapIO.createMap(file, false);
            Rules rules = new Rules();
            rules.waves = false;

            Vars.control.playMap(map, rules);
            Log.info("VR: sandbox map '@' loading", map.name());

            arc.util.Timer.schedule(() -> {
                try{
                    if(Vars.player == null || Vars.player.unit() != null) return;
                    var team = Vars.player.team();
                    var core = team.core();
                    float sx = core != null ? core.x : Vars.world.width() * 4f;
                    float sy = core != null ? core.y : Vars.world.height() * 4f;
                    Unit unit = UnitTypes.alpha.spawn(team, sx, sy);
                    Vars.player.unit(unit);
                    Log.info("VR: sandbox spawned @ at @,@", unit.type, sx, sy);
                }catch(Throwable t){
                    Log.err("VR: sandbox unit spawn failed", t);
                }
            }, 1f);
        }catch(Throwable t){
            Log.err("VR: sandbox start failed", t);
        }
    }

    /** Adds a per-frame hook that publishes the finished frame to the headset. */
    private static final class VrFrameListener implements ApplicationListener{
        private final Activity activity;
        private boolean attempted, announced, paused;
        private long lastRetry, lastRunningAt = System.currentTimeMillis();
        private long lastStatsLog;
        private int retries;
        private float startedAt;
        private boolean didSelfTap, didButtonTap;
        private boolean lastInGame, lastMoveActive, linearApplied;
        private int lastViewMode = -1, lastGlobeStrength = -1;

        VrFrameListener(Activity activity){
            this.activity = activity;
        }

        @Override
        public void update(){
            if(!attempted){
                attempted = true;
                if(activity.getIntent() != null && activity.getIntent().getBooleanExtra("solid", false)){
            VrSession.setSolid(true);
        }
                if(activity.getIntent() != null && activity.getIntent().getBooleanExtra("lookaround", false)){
            VrSession.setLookAround(true);
        }

        VrSession.start(activity);
            }

            // World uses the globe; dialogs/menus stay on the flat quad so their UI is
            // readable and the controller aim still maps to a normal screen.
            boolean menuOpen = Core.scene != null && Core.scene.hasDialog();
            int desiredView = menuOpen ? 2 : 0;
            if(lastViewMode != desiredView){
                lastViewMode = desiredView;
                VrSession.setViewMode(desiredView);
                VrSession.requestRecenter();
            }

            // Left Y cycles the globe curvature: small/curved -> giant/flat.
            if(VrSession.takeViewToggle()){
                int next = (Core.settings.getInt("vrglobe", 3) + 1) % 5;
                Core.settings.put("vrglobe", next);
                lastGlobeStrength = next;
                VrSession.setGlobeStrength(next);
                String label = new String[]{"Small globe", "Gentle globe", "Medium globe", "Large globe", "Giant globe"}[next];
                Log.info("VR: globe strength -> @", label);
                if(Vars.ui != null && Core.scene != null){
                    Vars.ui.announce("VR globe: " + label);
                }
            }

            int configuredGlobe = Core.settings.getInt("vrglobe", 3);
            if(configuredGlobe != lastGlobeStrength){
                lastGlobeStrength = configuredGlobe;
                VrSession.setGlobeStrength(configuredGlobe);
            }

            // Apply linear filtering once the atlas is available (Quest only).
            if(!linearApplied && Core.atlas != null){
                linearApplied = true;
                if(Core.settings.getBool("linear", false)){
                    for(Texture tex : Core.atlas.getTextures()){
                        tex.setFilter(TextureFilter.linear, TextureFilter.linear);
                    }
                    Log.info("VR: linear atlas filtering applied");
                }
            }

            // Switch the presentation to the fullscreen panorama as soon as gameplay starts,
            // and publish the player unit so the native stereo ship follows the map.
            publishWorldView();
            publishMovement();

            long now = System.currentTimeMillis();
            if(VrSession.isActive() && VrSession.sessionRunning()){
                lastRunningAt = now;

                // Periodic health line: confirms the game is rendering while the XR session runs.
                if(Core.input.justTouched()){
                Log.info("VR touch received: mouse @,@", Core.input.mouseX(), Core.input.mouseY());
            }

            long millis = System.currentTimeMillis();
                if(millis - lastStatsLog > 5000L){
                    lastStatsLog = millis;
                    Log.info("VR: session running, game fps @, status: @",
                             Core.graphics.getFramesPerSecond(), VrSession.status());
                }
            }

            // Bring the session back after a pause/home-button trip or a lost runtime session,
            // at most a few times. While paused, the game loop does not run, so no restarts.
            if(!paused && attempted && !VrSession.isInitializing() && retries < 3 && now - lastRetry > 5000){
                boolean lost = !VrSession.isActive();
                boolean stalled = VrSession.isActive() && now - lastRunningAt > 10_000;

                if(lost || stalled){
                    lastRetry = now;
                    lastRunningAt = now;
                    retries++;
                    Log.info("VR: session @, restart attempt @ (status: @)",
                             lost ? "inactive" : "not running", retries, VrSession.status());
                    VrSession.restart(activity);
                }
            }

            if(VrSession.isActive() && activity.getIntent() != null
               && activity.getIntent().getBooleanExtra("buttontap", false) && !didButtonTap
               && Core.scene != null && arc.util.Time.time - startedAt > 10f){
                didButtonTap = true;
                debugTapFirstButton();
            }

            if(VrSession.isActive() && activity.getIntent() != null
               && activity.getIntent().getBooleanExtra("selftap", false) && !didSelfTap
               && arc.util.Time.time - startedAt > 8f){
                didSelfTap = true;
                Log.info("VR: injecting self-test tap at screen centre");
                VrSession.debugTap(0.5f, 0.5f);
            }

            if(VrSession.isActive()){
                float zoom = VrSession.takeZoom();
                if(zoom != 0f && Vars.renderer != null){
                    Vars.renderer.scaleCamera(zoom);
                }
                VrSession.capture();

                // Core.scene is still null during early boot; Label construction requires it.
                if(!announced && Vars.ui != null && Vars.state != null && Core.scene != null){
                    announced = true;
                    Vars.ui.announce("VR: virtual screen active");
                }
            }else if(!announced && attempted && !VrSession.isInitializing() && Vars.ui != null
                     && Vars.state != null && Core.scene != null){
                announced = true;
                Vars.ui.announce("VR unavailable - OpenXR did not start (flat mode)");
                Log.err("VR: OpenXR session could not be created; running flat");
            }
        }

        /**
         * Left thumbstick -> unit movement. The camera is kept on the player so the panorama
         * centre is always the player/ship; the game world scrolls under the fixed view.
         */
        private void publishMovement(){
            boolean inGame = Vars.state != null && Vars.state.isGame();
            float mx = VrSession.moveX(), my = VrSession.moveY();

            Unit unit = null;
            if(inGame && Vars.player != null && !Vars.player.dead()){
                unit = Vars.player.unit();
            }

            boolean canMove = unit != null && !Vars.state.isPaused()
                    && (Core.scene == null || !Core.scene.hasDialog())
                    && Vars.control.input != null && !Vars.control.input.locked()
                    && Vars.renderer != null && Vars.renderer.landTime <= 0f;

            if(canMove){
                float mag = (float)Math.hypot(mx, my);
                // Larger deadzone than the stock input: Touch stick drift was making the
                // ship creep and the map jitter between frames.
                if(mag > 0.30f){
                    Vars.vrMoveX = mx / mag;
                    Vars.vrMoveY = my / mag;
                    Vars.vrMoveActive = true;
                    // Face the stick direction (0 = up/north, 90 = right/east) so both the
                    // 2D sprite and the native 3D ship visibly turn.
                    unit.rotation = Mathf.atan2(mx, my) * Mathf.radDeg;
                }else{
                    Vars.vrMoveActive = false;
                }

                // The game normally lets the camera lead the unit (mobile pan). In VR the
                // fixed screen should stay centred on the player, so pin the camera to it.
                Core.camera.position.set(unit.x, unit.y);
            }else{
                Vars.vrMoveActive = false;
            }

            if(Vars.vrMoveActive != lastMoveActive){
                lastMoveActive = Vars.vrMoveActive;
                Log.info("VR: left-stick movement @ (axis @,@)", Vars.vrMoveActive ? "on" : "off",
                         mx, my);
            }
        }

        /**
         * Publishes whether gameplay is active and where the player's unit is on the capture.
         * The native bridge uses this to swap the quad screen for the fullscreen projection
         * layer and to anchor the 3D Alpha ship at the player's map position.
         */
        private void publishWorldView(){
            boolean inGame = Vars.state != null && Vars.state.isGame();
            VrSession.setWorldView(inGame);
            Vars.vrWorldView = inGame;

            boolean visible = false;
            float u = 0.5f, v = 0.5f, rot = 0f;

            Unit unit = null;
            if(inGame && Vars.player != null && !Vars.player.dead()){
                unit = Vars.player.unit();
            }

            // The ship model exists for the campaign's Alpha core unit. Other player units
            // (e.g. a ground mech) keep their stock sprite.
            if(unit != null && unit.isAdded() && !unit.dead && unit.type == UnitTypes.alpha){
                Vec2 screen = Core.camera.project(Tmp.v1.set(unit.x, unit.y));
                int w = Core.graphics.getWidth(), h = Core.graphics.getHeight();
                if(w > 0 && h > 0){
                    u = Mathf.clamp(screen.x / w, 0f, 1f);
                    v = Mathf.clamp(screen.y / h, 0f, 1f);
                }
                // Face the stick/travel direction; the native model turns in 3D with it.
                float mx = VrSession.moveX(), my = VrSession.moveY();
                if(Math.hypot(mx, my) > 0.30f){
                    rot = Mathf.atan2(mx, my) * Mathf.radDeg;
                }else{
                    rot = unit.vel.len2() > 0.04f ? unit.vel.angle() : unit.rotation;
                }
                visible = true;
            }

            float zoom = Vars.renderer != null ? Vars.renderer.getDisplayScale() : 1f;
            VrSession.setPlayerPose(u, v, rot, zoom, visible);

            boolean model = visible && VrSession.worldViewActive() && VrSession.shipReady();
            if(model != Vars.vrShipModel){
                Log.info("VR: 3D player ship @ (worldview @, shipReady @)",
                         model ? "on" : "off", VrSession.worldViewActive(), VrSession.shipReady());
            }
            Vars.vrShipModel = model;

            if(inGame != lastInGame){
                lastInGame = inGame;
                Log.info("VR: in-game fullscreen view @", inGame ? "on" : "off");
                if(inGame && Vars.ui != null && Core.scene != null){
                    Vars.ui.announce("Flat full   Y: flat/panorama   Left stick: move   Right stick: zoom");
                }
            }
        }

        /** Debug: find a real button, log where it is, and tap its centre. */
        private void debugTapFirstButton(){
            if(Core.scene == null || Core.scene.root == null) return;

            Button button = findFirstButton(Core.scene.root);
            if(button == null){
                Log.info("VR: no visible button found in the scene");
                return;
            }

            Vec2 stage = button.localToStageCoordinates(new Vec2(button.getWidth() / 2f, button.getHeight() / 2f));
            Vec2 screen = Core.scene.stageToScreenCoordinates(stage.cpy());

            Log.info("VR: button @ size @x@ at stage @,@ -> screen @,@ (viewport @x@)",
                     button.getClass().getSimpleName(), button.getWidth(), button.getHeight(),
                     stage.x, stage.y, screen.x, screen.y,
                     Core.graphics.getWidth(), Core.graphics.getHeight());
            Log.info("VR: hit test at that point returns @", Core.scene.hit(stage.x, stage.y, true));
            Log.info("VR: button label '@', dialogs before @", labelOf(button), countDialogs());

            VrSession.debugTap(screen.x / Core.graphics.getWidth(),
                               1f - screen.y / Core.graphics.getHeight());

            arc.util.Timer.schedule(() -> Log.info("VR: dialogs after tap @", countDialogs()), 1.5f);
        }

        private String labelOf(Element element){
            if(element instanceof arc.scene.ui.Label l) return l.getText().toString();
            if(element instanceof Group g){
                for(Element child : g.getChildren()){
                    String text = labelOf(child);
                    if(text != null) return text;
                }
            }
            return null;
        }

        private int countDialogs(){
            if(Core.scene == null || Core.scene.root == null) return -1;
            return countDialogs(Core.scene.root);
        }

        private int countDialogs(Group group){
            int count = 0;
            for(Element element : group.getChildren()){
                if(element instanceof arc.scene.ui.Dialog) count++;
                if(element instanceof Group g) count += countDialogs(g);
            }
            return count;
        }

        private Button findFirstButton(Group group){
            for(Element element : group.getChildren()){
                if(element instanceof Button b && b.visible && b.getWidth() > 0 && b.getHeight() > 0){
                    return b;
                }
                if(element instanceof Group g){
                    Button found = findFirstButton(g);
                    if(found != null) return found;
                }
            }
            return null;
        }

        @Override
        public void pause(){
            paused = true;
        }

        @Override
        public void resume(){
            paused = false;
            lastRunningAt = System.currentTimeMillis();
            // Re-place the screen in front of wherever the user is looking now.
            VrSession.requestRecenter();
        }

        @Override
        public void dispose(){
            VrSession.stop();
        }
    }
}
