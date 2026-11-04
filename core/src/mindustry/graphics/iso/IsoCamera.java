package mindustry.graphics.iso;

import arc.graphics.*;
import arc.math.geom.*;

import static mindustry.Vars.*;

/**
 * Camera used by Mindustry's 2D pipeline, extended so screen↔world conversions go through the
 * isometric ground-plane projection while {@link Vars#iso3d} is active.
 *
 * <p>This is deliberately the single choke point: every input, UI and overlay call site uses
 * {@code Core.input.mouseWorld()} / {@code Core.camera.project()}, so making the camera aware of
 * the 3D view makes the whole game interactive in 3D without touching dozens of call sites.</p>
 */
public class IsoCamera extends Camera{

    private boolean active(){
        return iso3d && state != null && state.isGame() && renderer != null && renderer.iso.ready();
    }

    @Override
    public Vec2 unproject(Vec2 coords){
        if(active()) return renderer.iso.screenToWorld(coords.x, coords.y);
        return super.unproject(coords);
    }

    @Override
    public Vec2 unproject(float screenX, float screenY){
        if(active()) return renderer.iso.screenToWorld(screenX, screenY);
        return super.unproject(screenX, screenY);
    }

    @Override
    public Vec2 project(Vec2 coords){
        if(active()) return renderer.iso.worldToScreen(coords.x, coords.y);
        return super.project(coords);
    }

    @Override
    public Vec2 project(float worldX, float worldY){
        if(active()) return renderer.iso.worldToScreen(worldX, worldY);
        return super.project(worldX, worldY);
    }
}
