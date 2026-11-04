package mindustry.graphics.iso;

import arc.*;
import arc.graphics.*;
import arc.graphics.g2d.Draw;
import arc.graphics.g2d.Fill;
import arc.graphics.g2d.TextureRegion;
import arc.graphics.g3d.*;
import arc.math.*;
import arc.math.geom.*;
import arc.util.*;
import mindustry.content.Blocks;
import mindustry.entities.units.BuildPlan;
import mindustry.input.InputHandler;
import mindustry.core.World;
import mindustry.gen.*;
import mindustry.world.*;
import mindustry.world.blocks.environment.*;

import static mindustry.Vars.*;

/**
 * Experimental isometric-3D world renderer.
 *
 * <p>M1 scope: draw the loaded map as real 3D geometry — floor tiles as quads on the ground
 * plane, walls/buildings as extruded boxes — with an orthographic isometric camera. The
 * simulation is untouched; this only replaces the world presentation pass.</p>
 *
 * <p>Coordinates: Mindustry's 2D {@code (x, y)} maps to 3D {@code (x, z)}; height is {@code y}.
 * One tile is {@link #tileWorldSize} world units, matching the 2D renderer.</p>
 */
public class IsoRenderer implements Disposable{
    public static final float tileWorldSize = 8f;

    /** Camera yaw around the vertical axis, degrees. */
    public float yaw = 45f;
    /** Camera pitch below the horizontal, degrees. */
    public float pitch = 35f;
    /** How far back the orthographic camera sits; affects near/far only. */
    public float viewDistance = 1200f;
    /** Extruded height of a 1x1 wall, in world units. */
    public float wallHeight = 4.2f;
    /** Multiplies the 2D camera's visible area; >1 shows more of the world. */
    public float zoomOut = 2.2f;
    /** Log per-frame draw stats (quads, draw time) every couple of seconds. */
    public boolean stats = false;

    /** Render two eyes side by side (prerequisite for a stereo OpenXR projection layer). */
    public boolean stereo = false;
    /** Half-distance between the two eye cameras, in world units. */
    public float eyeSeparation = 6f;

    /** Distance shading (fractions of the draw radius) — soft depth cue toward the horizon. */
    public float fogStart = 0.5f, fogEnd = 1.1f, fogMax = 0.4f;

    public final Camera3D camera = new Camera3D();
    private final Vec3 direction = new Vec3();
    private final Color topColor = Color.white.cpy();
    private final Color shadeTop = new Color(), shadeSideX = new Color(), shadeSideZ = new Color(), shadeLiquid = new Color();
    private final Color ghostValid = new Color(1f, 1f, 1f, 0.7f);
    private final Color ghostInvalid = new Color(1f, 0.7f, 0.65f, 0.7f);
    // Directional sun: top faces are brightest, each side gets its own shade.
    private final Color topLit = new Color(1f, 0.99f, 0.95f, 1f);
    private final Color sideSouth = new Color(0.70f, 0.70f, 0.74f, 1f);
    private final Color sideEast = new Color(0.56f, 0.56f, 0.62f, 1f);
    private final Color sideNorth = new Color(0.50f, 0.50f, 0.56f, 1f);
    private final Color sideWest = new Color(0.78f, 0.78f, 0.82f, 1f);
    private final Color blockShadow = new Color(0f, 0f, 0f, 0.20f);
    private final Color unitShadow = new Color(0f, 0f, 0f, 0.28f);
    private final Color rangeColor = new Color(1f, 0.85f, 0.55f, 0.30f);
    private final Color arrowColor = new Color(1f, 1f, 1f, 0.95f);
    private final Color arrowOutline = new Color(0f, 0f, 0f, 0.7f);

    private static final int maxVertices = 60000;

    private VertexBatch3D batch;
    private Texture whiteTexture;
    private TextureRegion white;
    private Texture lastTexture;
    private boolean depthActive;
    private boolean ready;
    private int frameQuads, statFrames;
    private long drawStart;

    public void draw(){
        drawStart = stats ? Time.nanos() : 0;
        frameQuads = 0;

        if(batch == null){
            // Own 1x1 opaque white texture: the atlas "white" entry is a single texel and is not
            // reliable for large solid overlays (UV insets collapse it).
            Pixmap pixmap = new Pixmap(1, 1);
            pixmap.set(0, 0, Color.white);
            whiteTexture = new Texture(pixmap);
            white = new TextureRegion(whiteTexture);
            batch = new VertexBatch3D(maxVertices, false, true, 1);
            camera.perspective = false;
            camera.near = 1f;
            camera.far = viewDistance * 3f;
        }

        int width = Core.graphics.getWidth(), height = Core.graphics.getHeight();

        if(stereo){
            // Side-by-side stereo: two eye offsets of the same scene, each in half the frame.
            // This is the image pair an OpenXR projection layer will consume per eye.
            int halfWidth = Math.max(1, width / 2);

            Gl.viewport(0, 0, halfWidth, height);
            renderEye(-1f, halfWidth, height);

            Gl.viewport(halfWidth, 0, width - halfWidth, height);
            renderEye(1f, width - halfWidth, height);

            Gl.viewport(0, 0, width, height);
            restoreDepth();
            return;
        }

        Gl.viewport(0, 0, width, height);
        renderEye(0f, width, height);

        // Screen-space overlays are mono-only for now: in stereo they would land between the eyes.
        drawReticle();

        restoreDepth();
        logStats();
    }

    private void logStats(){
        if(!stats) return;
        if(++statFrames % 120 != 0) return;

        float ms = (Time.nanos() - drawStart) / 1_000_000f;
        Log.info("iso stats: quads=@ draw=@ms fps=@", frameQuads, String.format("%.2f", ms),
                 Core.graphics.getFramesPerSecond());
    }

    private void restoreDepth(){
        if(depthActive){
            Gl.disable(Gl.depthTest);
            Gl.depthMask(false);
            depthActive = false;
        }
    }

    /**
     * Renders the world for one eye into the current viewport.
     * @param eye -1 = left, 0 = mono, +1 = right
     */
    private void renderEye(float eye, int viewportWidth, int viewportHeight){
        float scale = Math.max(renderer.getDisplayScale(), 0.001f);

        camera.width = viewportWidth / scale * zoomOut;
        camera.height = viewportHeight / scale * zoomOut;

        // Look at the same point the 2D camera is centred on.
        float cx = Core.camera.position.x, cz = Core.camera.position.y;
        float yr = yaw * Mathf.degRad, pr = pitch * Mathf.degRad;
        direction.set(Mathf.cos(yr) * Mathf.cos(pr), -Mathf.sin(pr), Mathf.sin(yr) * Mathf.cos(pr)).nor();

        // Stereo offset along the camera's ground-right vector.
        float rx = -Mathf.sin(yr), rz = Mathf.cos(yr);
        float offset = eye * eyeSeparation * 0.5f;

        camera.position.set(cx + rx * offset - direction.x * viewDistance, -direction.y * viewDistance,
                            cz + rz * offset - direction.z * viewDistance);
        camera.direction.set(direction);
        camera.up.set(0f, 1f, 0f);
        camera.update();

        batch.proj(camera.combined);
        ready = true;

        // Opaque geometry plus blended ore overlays and contact shadows.
        Gl.enable(Gl.blend);
        Gl.blendFunc(Gl.srcAlpha, Gl.oneMinusSrcAlpha);

        // Depth testing is what keeps the extruded geometry from drawing in the wrong order.
        Gl.depthMask(true);
        Gl.enable(Gl.depthTest);
        Gl.clear(Gl.depthBufferBit);
        depthActive = true;

        lastTexture = null;
        drawTiles(cx, cz, Math.max(camera.width, camera.height));
        drawUnits();

        // Build-plan ghosts: overlay so they cannot be hidden inside walls.
        Gl.disable(Gl.depthTest);
        drawPlans();
        flush();
        Gl.enable(Gl.depthTest);
    }

    private void drawTiles(float cx, float cz, float viewSize){
        int centerX = World.toTile(cx), centerY = World.toTile(cz);
        float radius = viewSize * 0.95f;
        int range = (int)(radius / tileWorldSize) + 3;
        float maxDist2 = radius * radius;

        for(int dx = -range; dx <= range; dx++){
            for(int dy = -range; dy <= range; dy++){
                int tx = centerX + dx, ty = centerY + dy;
                float wx = (tx + 0.5f) * tileWorldSize, wz = (ty + 0.5f) * tileWorldSize;
                float ddx = wx - cx, ddz = wz - cz;
                if(ddx * ddx + ddz * ddz > maxDist2) continue;

                Tile tile = world.tile(tx, ty);
                if(tile == null) continue;

                // Frustum test against a sphere covering the tile and its extrusion.
                if(!camera.frustum.containsSphere(wx, wallHeight * 0.5f, wz, tileWorldSize * 0.85f + wallHeight)) continue;

                float dist = Mathf.sqrt(ddx * ddx + ddz * ddz);
                float shade = 1f - Mathf.clamp((dist - radius * fogStart) / (radius * (fogEnd - fogStart))) * fogMax;

                drawFloor(tile, shade);
                drawBlock(tile, shade);
            }
        }
    }

    private void drawFloor(Tile tile, float shade){
        Floor floor = tile.floor();
        if(floor == null) return;

        float x = tile.worldx(), z = tile.worldy();
        float half = tileWorldSize / 2f;

        // Liquid floors are shader-driven masks in the 2D renderer; as a flat quad their
        // texture is effectively black, so use the liquid colour instead.
        if(floor.isLiquid){
            Color liquid = floor.liquidDrop != null ? floor.liquidDrop.color : Color.valueOf("3d5a80");
            shadeLiquid.set(liquid).mul(shade);
            quad(white, x - half, z - half, x + half, z + half, 0f, shadeLiquid);
            return;
        }

        // Pick the same per-tile variant the 2D renderer uses, otherwise large floors look
        // like one texture repeated in a grid.
        TextureRegion region = floor.region;
        TextureRegion[] variants = floor.variantRegions();
        if(variants != null && variants.length > 1){
            region = variants[floor.variant(tile.x, tile.y)];
        }

        if(region == null || region.texture == null) return;
        shadeTop.set(topColor).mul(shade);
        quad(region, x - half, z - half, x + half, z + half, 0f, shadeTop);

        // Ore overlays sit on the floor; without them, resource patches are invisible in 3D.
        Block ore = tile.overlay();
        if(ore != null && ore != Blocks.air && ore.region != null && ore.region.texture != null){
            quad(ore.region, x - half, z - half, x + half, z + half, 0.06f, shadeTop);
        }
    }

    private void drawBlock(Tile tile, float shade){
        Block block = tile.block();
        if(block == Blocks.air || block.region == null || block.region.texture == null) return;

        float x = tile.worldx(), z = tile.worldy();
        float half = block.size * tileWorldSize / 2f;

        // Buildings (anything with an entity) get a low plinth: their sprite is already drawn
        // top-down and stretching it over a tall box reads as noise. Terrain walls get a real
        // extrusion because that is what gives the world its height.
        boolean building = tile.build != null;
        float top = building ? 1.0f + block.size * 0.7f : block.size * wallHeight;

        shadeTop.set(topLit).mul(shade);
        shadeSideX.set(sideEast).mul(shade);
        shadeSideZ.set(sideSouth).mul(shade);

        // Soft contact shadow on the ground, offset slightly toward the sun's opposite side.
        Gl.enable(Gl.blend);
        Gl.blendFunc(Gl.srcAlpha, Gl.oneMinusSrcAlpha);
        quad(white, x - half - 0.8f, z - half - 1.4f, x + half + 1.4f, z + half + 0.8f, 0.04f, blockShadow);

        // Top face.
        quad(block.region, x - half, z - half, x + half, z + half, top, shadeTop);

        // Four side faces. A flat white region tinted by the vertex colour avoids stretching
        // the sprite's top-down art across a vertical face. Faces touching another block are
        // skipped: they are never visible and cost a quad each.
        if(!blocked(tile.x, tile.y + 1)){
            sideQuad(white, x - half, z + half, x + half, z + half, top, shadeSideZ); // south
        }
        if(!blocked(tile.x + 1, tile.y)){
            sideQuad(white, x + half, z + half, x + half, z - half, top, shadeSideX); // east
        }
        shadeSideX.set(sideNorth).mul(shade);
        if(!blocked(tile.x, tile.y - 1)){
            sideQuad(white, x + half, z - half, x - half, z - half, top, shadeSideX); // north
        }
        shadeSideX.set(sideWest).mul(shade);
        if(!blocked(tile.x - 1, tile.y)){
            sideQuad(white, x - half, z - half, x - half, z + half, top, shadeSideX); // west
        }
    }

    /**
     * Build-plan feedback: ghost blocks for queued placements and for the tile under the cursor,
     * tinted green when the placement is valid and red when it is not. Without this, building in
     * 3D is blind because the stock 2D preview lives in the skipped world pass.
     */
    private void drawPlans(){
        if(state.isMenu() || player == null || control.input == null) return;

        InputHandler in = control.input;

        // Alpha blending for the ghosts; the 3D pass otherwise draws opaque geometry.
        Gl.enable(Gl.blend);
        Gl.blendFunc(Gl.srcAlpha, Gl.oneMinusSrcAlpha);

        drawBuildRange();

        for(BuildPlan plan : in.selectPlans) drawPlanGhost(plan);
        for(BuildPlan plan : in.linePlans) drawPlanGhost(plan);

        // Cursor ghost: the tile highlight/border itself is drawn in the 2D overlay pass.
        Tile tile = world.tileWorld(Core.input.mouseWorld().x, Core.input.mouseWorld().y);
        if(tile != null && !in.isBreaking() && in.block != null){
            boolean valid = Build.validPlace(in.block, player.team(), tile.x, tile.y, in.rotation);
            drawGhost(in.block, tile.x, tile.y, valid);
            drawRotationArrow(tile.x, tile.y, in.block, in.rotation);
        }
    }

    /**
     * Build-range ring around the player, shown only while a block is selected or breaking.
     * The 2D HUD draws this; in 3D it has to be laid on the ground in world space.
     */
    private void drawBuildRange(){
        if(player == null || player.unit() == null || player.unit().type == null) return;

        InputHandler in = control.input;
        if(in == null || (in.block == null && !in.isBreaking())) return;

        float range = player.unit().type.buildRange;
        if(range <= 0f) return;

        int segments = 72;
        float step = Mathf.PI2 / segments;
        float lastX = player.unit().x + range, lastZ = player.unit().y;

        for(int i = 1; i <= segments; i++){
            float a = i * step;
            float x = player.unit().x + Mathf.cos(a) * range;
            float z = player.unit().y + Mathf.sin(a) * range;
            segment(lastX, lastZ, x, z, 2.2f, 0.12f, rangeColor);
            lastX = x;
            lastZ = z;
        }
    }

    /** Arrow on the ghost tile showing block facing (rotation 0..3 = +X, +Y, -X, -Y). */
    private void drawRotationArrow(int tx, int ty, Block block, int rotation){
        float size = block.size * tileWorldSize;
        float cx = tx * tileWorldSize + size / 2f;
        float cz = ty * tileWorldSize + size / 2f;

        float dx = rotation == 0 ? 1f : rotation == 2 ? -1f : 0f;
        float dz = rotation == 1 ? 1f : rotation == 3 ? -1f : 0f;
        if(dx == 0f && dz == 0f) return;

        float len = size * 0.55f;
        float tipX = cx + dx * len, tipZ = cz + dz * len;
        float ang = Mathf.atan2(dz, dx);

        float[][] strokes = new float[3][4];
        strokes[0] = new float[]{cx, cz, tipX, tipZ};
        for(int side = -1, i = 1; side <= 1; side += 2, i++){
            float back = ang + side * 0.7f;
            strokes[i] = new float[]{tipX, tipZ, tipX - Mathf.cos(back) * len * 0.5f,
                                     tipZ - Mathf.sin(back) * len * 0.5f};
        }

        // Outline first, then the bright stroke, so the arrow reads on any terrain.
        for(float[] st : strokes){
            segment(st[0], st[1], st[2], st[3], 4.2f, 0.18f, arrowOutline);
        }
        for(float[] st : strokes){
            segment(st[0], st[1], st[2], st[3], 2.0f, 0.20f, arrowColor);
        }
    }

    /** Thin quad between two ground points, using the 1x1 white texture. */
    private void segment(float x1, float z1, float x2, float z2, float width, float y, Color color){
        float dx = x2 - x1, dz = z2 - z1;
        float len = Mathf.sqrt(dx * dx + dz * dz);
        if(len < 1e-4f) return;

        float nx = -dz / len * width * 0.5f;
        float nz = dx / len * width * 0.5f;

        ensureCapacity(6);
        bind(white.texture);

        vertex(x1 - nx, y, z1 - nz, 0f, 0f, color);
        vertex(x2 - nx, y, z2 - nz, 0f, 0f, color);
        vertex(x2 + nx, y, z2 + nz, 0f, 0f, color);

        vertex(x1 - nx, y, z1 - nz, 0f, 0f, color);
        vertex(x2 + nx, y, z2 + nz, 0f, 0f, color);
        vertex(x1 + nx, y, z1 + nz, 0f, 0f, color);
    }

    private void drawPlanGhost(BuildPlan plan){
        if(plan.block == null) return;
        boolean valid = Build.validPlace(plan.block, player.team(), plan.x, plan.y, plan.rotation);
        drawGhost(plan.block, plan.x, plan.y, valid);
    }

    private void drawGhost(Block block, int tx, int ty, boolean valid){
        float size = block.size * tileWorldSize;
        float x0 = tx * tileWorldSize, z0 = ty * tileWorldSize;

        // The block's own icon, so you can see what is being placed. The tile highlight and
        // border are drawn in the 2D overlay pass (exact projected footprint).
        TextureRegion region = block.fullIcon != null ? block.fullIcon : block.region;
        if(region != null && region.texture != null){
            quad(region, x0, z0, x0 + size, z0 + size, 1.1f, valid ? ghostValid : ghostInvalid);
        }
    }

    /**
     * 2D reticle at the projected cursor position. Drawn after the 3D pass with a screen-space
     * orthographic projection, so it cannot be occluded by world geometry.
     */
    private final Vec2[] corners = {new Vec2(), new Vec2(), new Vec2(), new Vec2()};
    private final Vec2[] inner = {new Vec2(), new Vec2(), new Vec2(), new Vec2()};

    /** Projects a tile footprint to screen space and draws a filled, outlined highlight. */
    private void drawTileHighlight(int tx, int ty, int size, boolean valid){
        float x0 = tx * tileWorldSize, z0 = ty * tileWorldSize;
        float x1 = x0 + size * tileWorldSize, z1 = z0 + size * tileWorldSize;

        worldToScreen(x0, z0, corners[0]);
        worldToScreen(x1, z0, corners[1]);
        worldToScreen(x1, z1, corners[2]);
        worldToScreen(x0, z1, corners[3]);

        float cx = (corners[0].x + corners[2].x) / 2f, cy = (corners[0].y + corners[2].y) / 2f;
        for(int i = 0; i < 4; i++){
            inner[i].set(cx + (corners[i].x - cx) * 0.66f, cy + (corners[i].y - cy) * 0.66f);
        }

        Draw.flush();
        Draw.color(valid ? 0.45f : 1f, valid ? 1f : 0.35f, valid ? 0.55f : 0.3f, 0.45f);
        Fill.quad(corners[0].x, corners[0].y, corners[1].x, corners[1].y,
                  corners[2].x, corners[2].y, corners[3].x, corners[3].y);

        Draw.color(valid ? 0.45f : 1f, valid ? 1f : 0.35f, valid ? 0.55f : 0.3f, 0.95f);
        for(int i = 0; i < 4; i++){
            int j = (i + 1) % 4;
            Fill.quad(corners[i].x, corners[i].y, corners[j].x, corners[j].y,
                      inner[j].x, inner[j].y, inner[i].x, inner[i].y);
        }
        Draw.flush();
    }

    private void drawReticle(){
        if(state.isMenu() || !ready) return;

        float w = Core.graphics.getWidth(), h = Core.graphics.getHeight();

        // The reticle marks the cursor itself; the world position that input resolves to is
        // verified separately (desktop/.../IsoPreview). Using screen coordinates here avoids a
        // second projection round-trip and always matches what the player is pointing at.
        // Draw uses a bottom-left origin, which is exactly what Core.input reports.
        float sx = Core.input.mouseX(), sy = Core.input.mouseY();

        Draw.flush();
        Draw.proj(Tmp.m1.setOrtho(0f, 0f, w, h));

        // Plan footprints: exact projected iso tiles, drawn with the reliable 2D batch.
        if(!state.isMenu() && player != null && control.input != null){
            InputHandler in = control.input;
            for(BuildPlan plan : in.selectPlans){
                if(plan.block != null){
                    drawTileHighlight(plan.x, plan.y, plan.block.size,
                                      Build.validPlace(plan.block, player.team(), plan.x, plan.y, plan.rotation));
                }
            }

            Tile tile = world.tileWorld(Core.input.mouseWorld().x, Core.input.mouseWorld().y);
            if(tile != null){
                if(in.isBreaking()){
                    drawTileHighlight(tile.x, tile.y, 1, false);
                }else if(in.block != null){
                    boolean valid = Build.validPlace(in.block, player.team(), tile.x, tile.y, in.rotation);
                    drawTileHighlight(tile.x, tile.y, in.block.size, valid);
                }
            }
        }

        // Cyan reads clearly against every Mindustry terrain/block palette; the dark rings keep
        // the reticle visible over bright buildings as well.
        Color accent = Color.valueOf("5fe8ff");
        Draw.color(0f, 0f, 0f, 0.65f);
        Draw.rect(Core.atlas.white(), sx, sy, 14f, 14f);
        Draw.color(accent);
        Draw.rect(Core.atlas.white(), sx, sy, 8f, 8f);
        Draw.color();
        Draw.flush();
    }

    /** Screen-aligned billboards for units; the only dynamic entities drawn in M1. */
    private void drawUnits(){
        if(state.isMenu()) return;

        Vec3 right = new Vec3(camera.direction).crs(camera.up).nor();
        Vec3 up = new Vec3(camera.up);

        for(Unit unit : Groups.unit){
            TextureRegion region = unit.type.region;
            if(region == null || region.texture == null) continue;

            // Ground shadow first, so units read as standing on the terrain.
            float shadowSize = Math.max(unit.hitSize, 4f) * 0.95f;
            quad(white, unit.x - shadowSize, unit.y - shadowSize, unit.x + shadowSize, unit.y + shadowSize,
                 0.05f, unitShadow);

            float w = Math.max(unit.hitSize, 4f) * 1.9f;
            float h = w * region.height / (float)region.width;
            float x = unit.x, z = unit.y;

            Vec3 base = new Vec3(x, 0f, z);
            Vec3 cx = new Vec3(right).scl(w / 2f);
            Vec3 cy = new Vec3(up).scl(h);
            Color tint = unit.team.color;

            ensureCapacity(6);
            bind(region.texture);

            Vec3 bl = new Vec3(base).sub(cx);
            Vec3 br = new Vec3(base).add(cx);
            Vec3 tl = new Vec3(bl).add(cy);
            Vec3 tr = new Vec3(br).add(cy);

            float iu = insetU(region), iv = insetV(region);
            float u0 = region.u + iu, u1 = region.u2 - iu, v0 = region.v + iv, v1 = region.v2 - iv;

            vertex(bl, u0, v1, tint);
            vertex(br, u1, v1, tint);
            vertex(tr, u1, v0, tint);

            vertex(bl, u0, v1, tint);
            vertex(tr, u1, v0, tint);
            vertex(tl, u0, v0, tint);
        }
    }

    /** True when a tile holds any non-air block, used to cull hidden wall faces. */
    private boolean blocked(int x, int y){
        Tile tile = world.tile(x, y);
        return tile != null && tile.block() != Blocks.air;
    }

    /** Horizontal quad at a fixed height. */
    private void quad(TextureRegion region, float x0, float z0, float x1, float z1, float y, Color color){
        frameQuads++;
        ensureCapacity(6);
        bind(region.texture);

        // NOTE: Arc's VertexBatch3D applies colour/texcoord to the *next* vertex only, so both
        // attributes must be set before every single vertex().
        float iu = insetU(region), iv = insetV(region);
        // Single-texel regions (e.g. atlas "white") would collapse to a zero/negative UV span.
        if(region.u2 - region.u <= iu * 2f) iu = 0f;
        if(region.v2 - region.v <= iv * 2f) iv = 0f;
        float u0 = region.u + iu, u1 = region.u2 - iu, v0 = region.v + iv, v1 = region.v2 - iv;

        vertex(x0, y, z0, u0, v1, color);
        vertex(x1, y, z0, u1, v1, color);
        vertex(x1, y, z1, u1, v0, color);

        vertex(x0, y, z0, u0, v1, color);
        vertex(x1, y, z1, u1, v0, color);
        vertex(x0, y, z1, u0, v0, color);
    }

    /** Vertical quad from y=0 to y=height, between two ground points. */
    private void sideQuad(TextureRegion region, float x0, float z0, float x1, float z1, float height, Color color){
        frameQuads++;
        ensureCapacity(6);
        bind(region.texture);

        float iu = insetU(region), iv = insetV(region);
        if(region.u2 - region.u <= iu * 2f) iu = 0f;
        if(region.v2 - region.v <= iv * 2f) iv = 0f;
        float u0 = region.u + iu, u1 = region.u2 - iu, v0 = region.v + iv, v1 = region.v2 - iv;

        vertex(x0, 0f, z0, u0, v1, color);
        vertex(x1, 0f, z1, u1, v1, color);
        vertex(x1, height, z1, u1, v0, color);

        vertex(x0, 0f, z0, u0, v1, color);
        vertex(x1, height, z1, u1, v0, color);
        vertex(x0, height, z0, u0, v0, color);
    }

    /** Half-texel inset so bilinear filtering cannot bleed in neighbouring atlas sprites. */
    private static float insetU(TextureRegion region){
        return 0.5f / region.texture.width;
    }

    private static float insetV(TextureRegion region){
        return 0.5f / region.texture.height;
    }

    private void vertex(float x, float y, float z, float u, float v, Color color){
        batch.color(color);
        batch.texCoord(u, v);
        batch.vertex(x, y, z);
    }

    private void vertex(Vec3 v, float u, float vCoords, Color color){
        vertex(v.x, v.y, v.z, u, vCoords, color);
    }

    /** Prevents the fixed-size batch from overflowing on dense maps. */
    private void ensureCapacity(int vertices){
        if(batch.getNumVertices() + vertices > maxVertices - 8){
            batch.flush(Gl.triangles);
            lastTexture = null;
        }
    }

    private void bind(Texture texture){
        if(texture == lastTexture) return;
        batch.flush(Gl.triangles);
        if(texture != null) texture.bind();
        lastTexture = texture;
    }

    private void flush(){
        batch.flush(Gl.triangles);
        lastTexture = null;
    }

    /** True once the camera has been positioned at least once this session. */
    public boolean ready(){
        return ready;
    }

    private final Vec2 worldOut = new Vec2();

    /**
     * Screen (top-left origin, pixels) → world ground position for the isometric camera.
     * Exact inverse of {@link #worldToScreen(float, float)}; used by {@link IsoCamera} so every
     * existing {@code Core.input.mouseWorld()} call site works in 3D mode.
     */
    public Vec2 screenToWorld(float screenX, float screenY){
        if(!ready) return worldOut.set(Core.camera.position);

        float yr = yaw * Mathf.degRad, pr = pitch * Mathf.degRad;
        float cy = Mathf.cos(yr), sy = Mathf.sin(yr), cp = Mathf.cos(pr), sp = Mathf.sin(pr);

        // Camera basis, matching the orthographic camera built in draw().
        float dx = cy * cp, dy = -sp, dz = sy * cp;
        float rx = -sy, rz = cy;
        float ux = cy * sp, uy = cp, uz = sy * sp;

        int w = Core.graphics.getWidth(), h = Core.graphics.getHeight();
        // Arc (SDL and Android) reports input in bottom-left origin coordinates, so screenY is
        // already measured upwards and must NOT be flipped here.
        float ndcX = 2f * screenX / w - 1f;
        float ndcY = 2f * screenY / h - 1f;

        float a = ndcX * (camera.width / 2f);
        float b = ndcY * (camera.height / 2f);

        float px = camera.position.x + a * rx + b * ux;
        float py = camera.position.y + b * uy;
        float pz = camera.position.z + a * rz + b * uz;

        float t = -py / dy;
        return worldOut.set(px + dx * t, pz + dz * t);
    }

    /** World ground position → screen pixels (bottom-left origin), written into {@code out}. */
    public void worldToScreen(float worldX, float worldY, Vec2 out){
        if(!ready){
            out.set(Float.NaN, Float.NaN);
            return;
        }

        float yr = yaw * Mathf.degRad, pr = pitch * Mathf.degRad;
        float cy = Mathf.cos(yr), sy = Mathf.sin(yr), cp = Mathf.cos(pr), sp = Mathf.sin(pr);

        float rx = -sy, rz = cy;
        float ux = cy * sp, uy = cp, uz = sy * sp;

        float ox = worldX - camera.position.x;
        float oy = -camera.position.y;
        float oz = worldY - camera.position.z;

        float a = ox * rx + oz * rz;
        float b = ox * ux + oy * uy + oz * uz;

        int w = Core.graphics.getWidth(), h = Core.graphics.getHeight();
        out.set((a / (camera.width / 2f) + 1f) * 0.5f * w, (b / (camera.height / 2f) + 1f) * 0.5f * h);
    }

    /** World ground position → screen pixels (bottom-left origin). */
    public Vec2 worldToScreen(float worldX, float worldY){
        if(!ready) return worldOut.set(Float.NaN, Float.NaN);

        float yr = yaw * Mathf.degRad, pr = pitch * Mathf.degRad;
        float cy = Mathf.cos(yr), sy = Mathf.sin(yr), cp = Mathf.cos(pr), sp = Mathf.sin(pr);

        float rx = -sy, rz = cy;
        float ux = cy * sp, uy = cp, uz = sy * sp;

        float ox = worldX - camera.position.x;
        float oy = -camera.position.y;
        float oz = worldY - camera.position.z;

        float a = ox * rx + oz * rz;
        float b = ox * ux + oy * uy + oz * uz;

        int w = Core.graphics.getWidth(), h = Core.graphics.getHeight();
        float ndcX = a / (camera.width / 2f);
        float ndcY = b / (camera.height / 2f);

        // Bottom-left origin, matching Core.input.mouseX()/mouseY().
        return worldOut.set((ndcX + 1f) * 0.5f * w, (ndcY + 1f) * 0.5f * h);
    }

    @Override
    public void dispose(){
        if(whiteTexture != null){
            whiteTexture.dispose();
            whiteTexture = null;
        }
        if(batch != null){
            batch.dispose();
            batch = null;
        }
    }
}
