package mindustry.desktop;

import arc.*;
import arc.files.*;
import arc.util.*;
import mindustry.*;
import mindustry.game.*;
import mindustry.game.EventType.*;
import mindustry.io.*;
import mindustry.core.World;
import mindustry.maps.*;

/**
 * Developer harness for the isometric-3D renderer. Not part of normal gameplay.
 *
 * <p>Enable with {@code -Dmindustry.iso.preview=true}. When the client finishes loading, it
 * loads the stock "domain" map, starts a sandbox-style game and turns on
 * {@link Vars#iso3d} — giving a deterministic scene to screenshot while iterating on the 3D
 * renderer.</p>
 */
public class IsoPreview{
    private IsoPreview(){
    }

    public static void install(){
        Events.on(ClientLoadEvent.class, event -> Core.app.post(IsoPreview::start));
    }

    private static void start(){
        try{
            Fi file = Core.files.internal("maps/default/mudFlats.msav");
            Map map = MapIO.createMap(file, false);
            Rules rules = new Rules();
            rules.waves = false;

            Vars.control.playMap(map, rules);
            ensurePlayerUnit();
            // -Dmindustry.iso.2d=true renders the same map with the stock 2D renderer, for comparison.
            // -Dmindustry.iso.useSetting=true keeps whatever the persisted setting says,
            // which verifies that the Settings toggle round-trips through settings.bin.
            if(!Boolean.getBoolean("mindustry.iso.useSetting")){
                Vars.iso3d = !Boolean.getBoolean("mindustry.iso.2d");
            }

            // -Dmindustry.iso.stereo=true renders a side-by-side eye pair for the VR path.
            Vars.renderer.iso.stereo = Boolean.getBoolean("mindustry.iso.stereo");
            Vars.renderer.iso.stats = Boolean.getBoolean("mindustry.iso.stats");

            Log.info("Iso preview: playing '@' (@x@), iso3d=@", map.name(), map.width, map.height, Vars.iso3d);

            arc.util.Timer.schedule(IsoPreview::verifyInput, 10f);
        }catch(Throwable t){
            Log.err("Iso preview failed to start", t);
        }
    }

    /** Stock maps started outside the campaign give the player no unit; input stays locked. */
    private static void ensurePlayerUnit(){
        if(Vars.player == null || Vars.player.unit() != null) return;

        var team = Vars.player.team();
        var core = team.core();
        float sx = core != null ? core.x : Vars.world.width() * 4f;
        float sy = core != null ? core.y : Vars.world.height() * 4f;
        var unit = mindustry.content.UnitTypes.dagger.spawn(team, sx, sy);
        Vars.player.unit(unit);
        Log.info("Iso preview: spawned @ for player at @,@", unit.type, sx, sy);
    }

    /** Numeric check that the iso camera maps the screen centre back onto the camera target. */
    private static void verifyInput(){
        if(!Vars.iso3d){
            Log.info("Iso input check: skipped (iso3d off, 2D baseline run)");
            return;
        }

        int w = Core.graphics.getWidth(), h = Core.graphics.getHeight();
        if(w <= 0 || h <= 0) return;

        arc.math.geom.Vec2 centre = Vars.renderer.iso.screenToWorld(w / 2f, h / 2f).cpy();
        arc.math.geom.Vec2 mouse = Core.input.mouseWorld(w / 2f, h / 2f).cpy();
        float dx = centre.x - Core.camera.position.x, dy = centre.y - Core.camera.position.y;
        float dist = (float)Math.sqrt(dx * dx + dy * dy);

        Log.info("Iso input check: screenCentre->world @,@ (camera @,@) dist=@ mouseWorld @,@",
                 centre.x, centre.y, Core.camera.position.x, Core.camera.position.y, dist, mouse.x, mouse.y);
        Log.info("Iso input check: canvas @x@; iso3d=@; camReady=@", w, h, Vars.iso3d, Vars.renderer.iso.ready());
        // End the landing cutscene so captures are at normal gameplay zoom.
        Vars.renderer.landTime = 0f;

        ensurePlayerUnit();

        // Deterministic structure near the player so lighting/shadows and wall extrusion are
        // visible in captures (the stock spawn area is mostly open ground).
        if(Vars.player.unit() != null){
            int px = World.toTile(Vars.player.unit().x), py = World.toTile(Vars.player.unit().y);
            for(int ox = -3; ox <= 3; ox++){
                for(int oy = -3; oy <= 3; oy++){
                    boolean edge = Math.abs(ox) == 3 || Math.abs(oy) == 3;
                    boolean gate = (ox == 0 && Math.abs(oy) == 3);
                    if(edge && !gate && Vars.world.tile(px + ox, py + oy) != null){
                        Vars.world.tile(px + ox, py + oy).setBlock(mindustry.content.Blocks.titaniumWall);
                    }
                }
            }
            Log.info("Iso preview: built test wall structure at @,@", px, py);
        }

        Vars.control.input.block = mindustry.content.Blocks.conveyor;
        Vars.control.input.rotation = 1;
        Log.info("Iso input check: selected block=@ breaking=@", Vars.control.input.block, Vars.control.input.isBreaking());
        Log.info("Iso input check: playerDead=@ unit=@ paused=@ locked=@", Vars.player.dead(), Vars.player.unit(),
                 Vars.state.isPaused(), Vars.control.input.locked());

        // The tutorial/placement flow clears the selection; for the harness, keep it selected
        // so the 3D ghost can be captured.
        arc.util.Timer.schedule(() -> {
            if(Vars.control.input != null) Vars.control.input.block = mindustry.content.Blocks.conveyor;
        }, 0f, 2f);

        // Round-trip: world -> screen -> world must be identity.
        float wx = centre.x + 40f, wy = centre.y - 25f;
        arc.math.geom.Vec2 screen = Vars.renderer.iso.worldToScreen(wx, wy).cpy();
        arc.math.geom.Vec2 back = Vars.renderer.iso.screenToWorld(screen.x, screen.y).cpy();
        float rd = (float)Math.hypot(back.x - wx, back.y - wy);
        Log.info("Iso input check: roundtrip error=@ (screen @,@)", rd, screen.x, screen.y);

        // Axis sanity: moving the mouse right must move along the camera's ground-right vector
        // r = (-sin(yaw), cos(yaw)). A dot product near 1 means the mapping is not mirrored.
        arc.math.geom.Vec2 rightPt = Vars.renderer.iso.screenToWorld(w / 2f + 100f, h / 2f).cpy();
        float ddx = rightPt.x - centre.x, ddy = rightPt.y - centre.y;
        float len = (float)Math.hypot(ddx, ddy);
        float yawRad = Vars.renderer.iso.yaw * arc.math.Mathf.degRad;
        float ex = -arc.math.Mathf.sin(yawRad), ey = arc.math.Mathf.cos(yawRad);
        float dot = (ddx / len) * ex + (ddy / len) * ey;
        Log.info("Iso input check: right-axis dot=@ (delta @,@ len @)", dot, ddx, ddy, len);
        // Scale-independent consistency check: 100px right must equal 100 * cameraWidth / screenWidth
        // world units along the ground-right axis.
        float expected = 100f * Vars.renderer.iso.camera.width / w;
        Log.info("Iso input check: mouse=@,@ mouseWorld=@,@", Core.input.mouseX(), Core.input.mouseY(),
                 Core.input.mouseWorld().x, Core.input.mouseWorld().y);
        Log.info("Iso input check: displayScale=@ expectedLen=@ measuredLen=@ ratio=@", Vars.renderer.getDisplayScale(),
                 expected, len, len / expected);
    }
}
