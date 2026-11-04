// Host-side tests for the VR pointer mapping (no Android/OpenXR dependencies).
// Build + run:  g++ -std=c++17 -I<path to android/src/main/cpp> vr_raycast_test.cpp -o /tmp/t && /tmp/t
#include "vr_math.h"
#include "vr_render_math.h"

#include <cmath>
#include <cstdio>
#include <initializer_list>

using namespace vr;

static int failures = 0;

static void check(bool ok, const char *what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    } else {
        std::printf("ok:   %s\n", what);
    }
}

static bool approx(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) < eps; }

// Quaternion rotating `angle` radians about the Y axis (yaw).
static Quat yaw(float angle) {
    return Quat{0.f, std::sin(angle / 2.f), 0.f, std::cos(angle / 2.f)};
}

// Quaternion rotating `angle` radians about the X axis (pitch).
static Quat pitch(float angle) {
    return Quat{std::sin(angle / 2.f), 0.f, 0.f, std::cos(angle / 2.f)};
}

int main() {
    const ScreenGeometry screen{2.4f, 1.35f, -2.1f};
    float u = -1.f, v = -1.f;

    // Straight ahead from the origin: dead center of the screen.
    Pose center{Quat{0.f, 0.f, 0.f, 1.f}, Vec3{0.f, 0.f, 0.f}};
    check(rayToScreen(center, screen, u, v), "centre ray hits");
    check(approx(u, 0.5f) && approx(v, 0.5f), "centre ray maps to (0.5, 0.5)");

    // Aiming up by atan(0.675/2.1) should hit the top edge (v = 1).
    const float halfHeightAngle = std::atan((screen.height / 2.f) / std::fabs(screen.z));
    Quat up = pitch(halfHeightAngle);
    Pose top{up, Vec3{0.f, 0.f, 0.f}};
    check(rayToScreen(top, screen, u, v), "top-edge ray hits");
    check(approx(u, 0.5f, 1e-3f) && approx(v, 1.f, 1e-3f), "top-edge ray maps to v=1");

    // Aim to the right: u must grow.
    Pose right{Quat{yaw(-0.4f).x, yaw(-0.4f).y, 0.f, yaw(-0.4f).w}, Vec3{0.f, 0.f, 0.f}};
    check(rayToScreen(right, screen, u, v), "right ray hits");
    check(u > 0.5f && u < 1.f, "right ray maps to u > 0.5");
    check(approx(v, 0.5f, 1e-3f), "right ray keeps v centred");

    // Aiming behind the player must miss.
    Pose behind{Quat{0.f, 1.f, 0.f, 0.f}, Vec3{0.f, 0.f, 0.f}}; // 180 deg yaw
    check(!rayToScreen(behind, screen, u, v), "backwards ray misses");

    // Ray offset far to the left must miss the screen.
    Pose left{Quat{0.f, 0.f, 0.f, 1.f}, Vec3{-10.f, 0.f, 0.f}};
    check(!rayToScreen(left, screen, u, v), "far-offset ray misses");

    // A ray starting behind the screen and facing further away must miss.
    Pose away{Quat{0.f, 0.f, 0.f, 1.f}, Vec3{0.f, 0.f, screen.z - 1.f}};
    check(!rayToScreen(away, screen, u, v), "ray behind the screen misses");

    // Rotated/moved screen (the recenter case): screen at origin facing +X, ray from -X.
    ScreenPose rotated{Quat{0.f, std::sin(M_PI / 4.f), 0.f, std::cos(M_PI / 4.f)}, Vec3{0.f, 0.f, 0.f}, 2.4f, 1.35f};
    Pose fromLeft{Quat{0.f, -std::sin(M_PI / 4.f), 0.f, std::cos(M_PI / 4.f)}, Vec3{-2.f, 0.f, 0.f}};
    check(rayToScreen(fromLeft, rotated, u, v), "rotated screen: centre ray hits");
    check(approx(u, 0.5f) && approx(v, 0.5f), "rotated screen: centre maps to (0.5, 0.5)");

    Pose raised = fromLeft;
    raised.position.y = 0.5f;
    check(rayToScreen(raised, rotated, u, v), "rotated screen: raised ray hits");
    check(v > 0.5f, "rotated screen: raised ray maps upward");

    // Recenter placement: screen in front of the head yaw, facing back at the user.
    auto facesUser = [](const ScreenPose &s, const Vec3 &head) {
        const Vec3 normal = rotate(s.orientation, Vec3{0.f, 0.f, 1.f});
        const Vec3 toHead{head.x - s.position.x, head.y - s.position.y, head.z - s.position.z};
        return dot(normal, toHead) > 0.f;
    };
    auto distanceTo = [](const ScreenPose &s, const Vec3 &head) {
        const float dx = s.position.x - head.x, dy = s.position.y - head.y, dz = s.position.z - head.z;
        return std::sqrt(dx * dx + dy * dy + dz * dz);
    };

    const Vec3 eye{0.f, 1.6f, 0.f};
    for (float deg : {0.f, 90.f, 180.f, 270.f, 37.f}) {
        const float yaw = deg * (float)M_PI / 180.f;
        ScreenPose s = recenteredScreen(yaw, eye, 2.1f, 2.4f, 1.35f);
        check(approx(distanceTo(s, eye), 2.1f), "recenter: screen stays 2.1 m from the head");
        check(approx(s.position.y, eye.y), "recenter: screen at eye height");
        check(facesUser(s, eye), "recenter: screen faces the user");
    }

    // Look-around crop: centred at rest, pans with the head, clamps at the overscan edge.
    const float pan = 35.f * (float)M_PI / 180.f;
    LookCrop c0 = lookCrop(0.f, 0.f, 0.78f, pan);
    check(approx(c0.x + c0.w * 0.5f, 0.5f) && approx(c0.y + c0.h * 0.5f, 0.5f), "crop: centred at rest");
    LookCrop cR = lookCrop(pan, 0.f, 0.78f, pan);
    check(approx(cR.x + cR.w, 1.f), "crop: full right pan reaches texture edge");
    LookCrop cL = lookCrop(-pan * 3.f, 0.f, 0.78f, pan);
    check(approx(cL.x, 0.f), "crop: over-pan clamps at texture edge");
    LookCrop cU = lookCrop(0.f, pan, 0.78f, pan);
    check(approx(cU.y + cU.h, 1.f), "crop: looking up reaches top edge");

    // Radial fade: untouched centre, darker corners, never negative.
    check(approx(radialFade(0.f, 0.45f, 0.95f, 0.6f), 1.f), "fade: centre untouched");
    check(radialFade(1.f, 0.45f, 0.95f, 0.6f) < 0.5f, "fade: corners darken");
    check(radialFade(2.f, 0.45f, 0.95f, 0.6f) >= 0.f, "fade: clamps non-negative");

    // Globe sphere projection: centre ray -> texture centre, off-centre ray -> edge,
    // and the inverse direction is unit length.
    {
        const GlobeView globe{Vec3{0.f, 1.6f, 0.f}, 0.f, 2.f, 0.82f,
                              (float)M_PI / 2.f, (float)M_PI * 0.3f};
        const Pose centre{Quat{0.f, 0.f, 0.f, 1.f}, Vec3{0.f, 1.6f, 0.f}};
        float gu = 0.f, gv = 0.f;
        check(globeProject(centre, globe, gu, gv) && approx(gu, 0.5f, 2e-3f) &&
              approx(gv, 0.5f, 2e-3f), "globe: centre maps to centre");
        const float yaw30 = 30.f * (float)M_PI / 180.f;
        const Pose right{Quat{0.f, -std::sin(yaw30 * 0.5f), 0.f, std::cos(yaw30 * 0.5f)},
                         Vec3{0.f, 1.6f, 0.f}};
        check(globeProject(right, globe, gu, gv) && gu > 0.5f, "globe: right ray maps right");
        const Vec3 dir = globeDirection(globe, 0.7f, 0.6f);
        check(std::fabs(dot(dir, dir) - 1.f) < 1e-4f, "globe: direction unit length");
    }

    // 9x map panorama: the capture wrapped around a 300x180 degree cylinder (a 3x3 world
    // capture; the centre 100x60 is the normal view).
    const float spanH = 300.f * (float)M_PI / 180.f;
    const float spanV = 180.f * (float)M_PI / 180.f;
    const PanoView pano{Vec3{0.f, 1.6f, 0.f}, 0.f, 2.f, spanH, spanV};

    // The warp is identity in panorama mode (reserved for a future dome mode).
    check(approx(convexWarp(0.f), 0.f) && approx(convexWarp(1.f), 1.f) &&
          approx(convexWarp(-1.f), -1.f), "warp: endpoints fixed");

    const Pose panoEye{Quat{0.f, 0.f, 0.f, 1.f}, Vec3{0.f, 1.6f, 0.f}};
    check(rayToPano(panoEye, pano, u, v), "pano: centre ray hits");
    check(approx(u, 0.5f) && approx(v, 0.5f), "pano: centre maps to (0.5, 0.5)");

    // Right/up in yaw/pitch space map to u/v growing in the expected direction.
    const float yawRad = 20.f * (float)M_PI / 180.f;
    Pose panoRightRay{yaw(-yawRad), Vec3{0.f, 1.6f, 0.f}};
    check(rayToPano(panoRightRay, pano, u, v), "pano: right ray hits");
    check(u > 0.5f && u < 1.f, "pano: right ray maps to u > 0.5");
    check(approx(v, 0.5f, 1e-3f), "pano: right ray keeps v centred");

    Pose panoUpRay{pitch(0.4f), Vec3{0.f, 1.6f, 0.f}};
    check(rayToPano(panoUpRay, pano, u, v), "pano: up ray hits");
    check(v > 0.5f, "pano: up ray maps to v > 0.5");

    // Outside the capture's angular span (300 wide, so 160 deg still misses).
    Pose farRight{yaw(-160.f * (float)M_PI / 180.f), Vec3{0.f, 1.6f, 0.f}};
    check(!rayToPano(farRight, pano, u, v), "pano: beyond span misses");

    // Ray pointing away from the map misses even though it leaves the cylinder.
    Pose awayPano{Quat{0.f, 0.f, 0.f, 1.f}, Vec3{0.f, 1.6f, 0.f}};
    Pose behindPano{Quat{0.f, 1.f, 0.f, 0.f}, Vec3{0.f, 1.6f, 0.f}};
    check(!rayToPano(behindPano, pano, u, v), "pano: backwards ray misses");
    (void)awayPano;

    // Eye offset (head lean): a ray parallel to forward lands off-centre, giving parallax.
    Pose leanRay{Quat{0.f, 0.f, 0.f, 1.f}, Vec3{0.2f, 1.6f, 0.f}};
    float leanU = -1.f, leanV = -1.f;
    check(rayToPano(leanRay, pano, leanU, leanV), "pano: leaning eye ray hits");
    check(leanU > 0.5f, "pano: leaning right samples right of the centre seam (parallax)");

    // UV -> direction -> UV round-trips, including off-centre map content.
    for(float tu : {0.5f, 0.3f, 0.72f}) {
        for(float tv : {0.5f, 0.38f, 0.66f}) {
            Vec3 dir = panoDirection(pano, tu, tv);
            const Vec3 hit{pano.center.x + dir.x * pano.radius,
                           pano.center.y + dir.y * pano.radius,
                           pano.center.z + dir.z * pano.radius};
            float ru = -1.f, rv = -1.f;
            check(panoProject(pano, hit, ru, rv), "pano: project direction hit");
            check(approx(ru, tu, 1e-4f) && approx(rv, tv, 1e-4f), "pano: UV round-trip");
        }
    }

    // Rotated reference frame: recentering to 90 deg yaw keeps the centre centred.
    const PanoView turned{Vec3{3.f, 1.6f, -2.f}, (float)M_PI / 2.f, 2.f, spanH, spanV};
    Pose turnedCentre{Quat{0.f, std::sin((float)M_PI / 4.f), 0.f, std::cos((float)M_PI / 4.f)},
                      Vec3{3.f, 1.6f, -2.f}};
    check(rayToPano(turnedCentre, turned, u, v), "pano: rotated reference centre hits");
    check(approx(u, 0.5f) && approx(v, 0.5f), "pano: rotated reference centre maps to centre");

    // ---- per-eye camera math -----------------------------------------------------------------
    // Identity / translation.
    Mat4 ident = mat4Identity();
    Vec3 tp = mat4TransformPoint(ident, Vec3{1.f, 2.f, 3.f});
    check(approx(tp.x, 1.f) && approx(tp.y, 2.f) && approx(tp.z, 3.f), "mat4: identity keeps a point");

    Mat4 trans = mat4Translation(2.f, -1.f, 0.5f);
    tp = mat4TransformPoint(trans, Vec3{1.f, 1.f, 1.f});
    check(approx(tp.x, 3.f) && approx(tp.y, 0.f) && approx(tp.z, 1.5f), "mat4: translation applied");

    Mat4 scale = mat4Scale(2.f);
    tp = mat4TransformPoint(scale, Vec3{1.f, -2.f, 0.5f});
    check(approx(tp.x, 2.f) && approx(tp.y, -4.f) && approx(tp.z, 1.f), "mat4: scale applied");

    // Rotation sanity: yaw(-90 deg) turns -Z into +X (matching panoForward).
    Mat4 ry = mat4RotationY(-(float)M_PI / 2.f);
    tp = mat4TransformPoint(ry, Vec3{0.f, 0.f, -1.f});
    check(approx(tp.x, 1.f, 1e-5f) && approx(tp.y, 0.f) && approx(tp.z, 0.f), "mat4: yaw(-90) maps -Z to +X");

    Mat4 qy = mat4Quat(yaw(-(float)M_PI / 2.f));
    tp = mat4TransformPoint(qy, Vec3{0.f, 0.f, -1.f});
    check(approx(tp.x, 1.f, 1e-5f) && approx(tp.z, 0.f, 1e-5f), "mat4: quat yaw matches matrix yaw");

    // View matrix: eye at (0,0,-2) looking forward; a point 1 m in front is at z=-1.
    Mat4 view = mat4View(Quat{0.f, 0.f, 0.f, 1.f}, Vec3{0.f, 0.f, -2.f});
    tp = mat4TransformPoint(view, Vec3{0.f, 0.f, -3.f});
    check(approx(tp.x, 0.f) && approx(tp.y, 0.f) && approx(tp.z, -1.f), "mat4: view puts the eye at the origin");

    // Projection: symmetric FOV maps the centre to NDC (0,0) and the frustum edge to 1.
    Mat4 proj = mat4ProjectionFov(-1.f, 1.f, -1.f, 1.f, 0.05f, 100.f);
    Vec3 ndc = mat4Project(proj, Vec3{0.f, 0.f, -1.f});
    check(approx(ndc.x, 0.f) && approx(ndc.y, 0.f), "mat4: projection centre is NDC 0,0");
    ndc = mat4Project(proj, Vec3{1.f, 0.f, -1.f});
    check(approx(ndc.x, 1.f, 1e-5f) && approx(ndc.y, 0.f), "mat4: projection right edge is x=1");
    ndc = mat4Project(proj, Vec3{0.f, 1.f, -1.f});
    check(approx(ndc.y, 1.f, 1e-5f), "mat4: projection top edge is y=1");

    // Asymmetric projection: the straight-ahead ray lands at the off-axis principal point,
    // and each edge still reaches its own tangent. (Runtime FOVs are never symmetric.)
    Mat4 aproj = mat4ProjectionFov(-0.8f, 1.2f, -0.6f, 1.1f, 0.05f, 100.f);
    ndc = mat4Project(aproj, Vec3{0.f, 0.f, -1.f});
    check(approx(ndc.x, -0.2f, 1e-5f) && approx(ndc.y, -0.5f / 1.7f, 1e-5f),
          "mat4: asymmetric principal point is off-axis");
    ndc = mat4Project(aproj, Vec3{-0.8f, 0.f, -1.f});
    check(approx(ndc.x, -1.f, 1e-5f), "mat4: asymmetric left edge is x=-1");
    ndc = mat4Project(aproj, Vec3{0.f, 1.1f, -1.f});
    check(approx(ndc.y, 1.f, 1e-5f), "mat4: asymmetric top edge is y=1");

    // Scale/rotation/translation order: T * Ry * S moves the model origin to T.
    Mat4 model = mat4Mul(mat4Mul(mat4Translation(4.f, 5.f, 6.f), mat4RotationY(0.7f)), mat4Scale(0.05f));
    tp = mat4TransformPoint(model, Vec3{0.f, 0.f, 0.f});
    check(approx(tp.x, 4.f) && approx(tp.y, 5.f) && approx(tp.z, 6.f), "mat4: model origin lands at translation");

    // Aim math must agree with the visual mapping: a UV found by projection maps back.
    const Vec3 targetDir = panoDirection(pano, 0.68f, 0.56f);
    const Vec3 target{pano.center.x + targetDir.x * pano.radius,
                      pano.center.y + targetDir.y * pano.radius,
                      pano.center.z + targetDir.z * pano.radius};
    const Vec3 aimDir{target.x - pano.center.x, target.y - pano.center.y, target.z - pano.center.z};
    const float len = std::sqrt(dot(aimDir, aimDir));
    Quat aimQ = yaw(-std::atan2(aimDir.x, -aimDir.z));
    // pitch toward the target
    Quat pQ = pitch(std::asin(aimDir.y / len));
    aimQ = Quat{aimQ.x, aimQ.y, aimQ.z, aimQ.w};
    // compose pitch then yaw
    Pose aimed{Quat{
            aimQ.w * pQ.x + aimQ.x * pQ.w + aimQ.y * pQ.z - aimQ.z * pQ.y,
            aimQ.w * pQ.y - aimQ.x * pQ.z + aimQ.y * pQ.w + aimQ.z * pQ.x,
            aimQ.w * pQ.z + aimQ.x * pQ.y - aimQ.y * pQ.x + aimQ.z * pQ.w,
            aimQ.w * pQ.w - aimQ.x * pQ.x - aimQ.y * pQ.y - aimQ.z * pQ.z}, pano.center};
    float au = -1.f, av = -1.f;
    check(rayToPano(aimed, pano, au, av), "pano: aimed ray hits");
    check(approx(au, 0.68f, 2e-3f) && approx(av, 0.56f, 2e-3f), "pano: aimed ray maps to target UV");

    // ---- planet globe (orbit view): aspect-correct spans, wide disk, round-trips --------
    {
        // Default strength (Large): R=6.5 m, front=1.5 m -> D=8.0 m, capture 5:3.
        float halfLon = 0.f, halfLat = 0.f;
        globeAngles(6.5f, 8.0f, 0.6f, halfLon, halfLat);
        check(approx(halfLon, std::asin(6.5f / 8.0f), 1e-6f), "globeAngles: lon spans the disk");
        check(approx(halfLat, halfLon * 0.6f, 1e-6f), "globeAngles: lat keeps capture aspect");
        check(halfLon > 0.85f && halfLon < 1.05f, "globeAngles: disk is wide (~106 deg), not a small ball");

        const GlobeView planet{Vec3{0.f, 1.6f, 0.f}, 0.f, 8.0f, 6.5f / 8.0f, halfLon, halfLat};
        auto poseAlong = [](const Vec3 &dir, const Vec3 &origin) {
            const float yawA = std::atan2(-dir.x, -dir.z);
            const float pitchA = std::asin(clampf(dir.y, -1.f, 1.f));
            const Quat yq = yaw(yawA), pq = pitch(pitchA);
            // orientation = yaw * pitch (pitch applied first)
            return Pose{Quat{yq.w * pq.x + yq.x * pq.w + yq.y * pq.z - yq.z * pq.y,
                             yq.w * pq.y - yq.x * pq.z + yq.y * pq.w + yq.z * pq.x,
                             yq.w * pq.z + yq.x * pq.y - yq.y * pq.x + yq.z * pq.w,
                             yq.w * pq.w - yq.x * pq.x - yq.y * pq.y - yq.z * pq.z},
                        origin};
        };

        // Centre / right / up rays land at centre / right / up of the capture.
        float pu = -1.f, pv = -1.f;
        check(globeProject(poseAlong(Vec3{0.f, 0.f, -1.f}, planet.center), planet, pu, pv) &&
              approx(pu, 0.5f, 2e-3f) && approx(pv, 0.5f, 2e-3f), "planet: centre ray -> capture centre");
        check(globeProject(poseAlong(normalized(Vec3{0.45f, 0.f, -1.f}), planet.center), planet, pu, pv) &&
              pu > 0.55f && approx(pv, 0.5f, 3e-3f), "planet: right ray -> right of centre");
        check(globeProject(poseAlong(normalized(Vec3{0.f, 0.45f, -1.f}), planet.center), planet, pu, pv) &&
              pv > 0.55f && approx(pu, 0.5f, 3e-3f), "planet: up ray -> above centre");

        // UV -> direction -> UV round-trips (ship anchor + aim must agree with the shader).
        for (float tu : {0.5f, 0.25f, 0.78f}) {
            for (float tv : {0.5f, 0.3f, 0.72f}) {
                const Vec3 dir = globeDirection(planet, tu, tv);
                float ru = -1.f, rv = -1.f;
                check(globeProject(poseAlong(dir, planet.center), planet, ru, rv),
                      "planet: round-trip ray hits");
                check(approx(ru, tu, 2e-3f) && approx(rv, tv, 2e-3f), "planet: UV round-trip");
            }
        }

        // Head offset (lean) shifts the sample: real parallax, canvas itself stays pinned.
        check(globeProject(poseAlong(Vec3{0.f, 0.f, -1.f}, Vec3{0.2f, 1.6f, 0.f}), planet, pu, pv) &&
              pu > 0.5f, "planet: offset eye samples off-centre (parallax)");

        // Aim assist: centre still maps to centre; a beyond-limb ray stays clickable.
        check(globeProjectAssist(poseAlong(Vec3{0.f, 0.f, -1.f}, planet.center), planet, pu, pv) &&
              approx(pu, 0.5f, 2e-3f) && approx(pv, 0.5f, 2e-3f), "planet assist: centre -> centre");
        const float yaw80 = 80.f * (float)M_PI / 180.f;
        check(globeProjectAssist(poseAlong(Vec3{-std::sin(-yaw80), 0.f, -std::cos(-yaw80)}, planet.center),
                                 planet, pu, pv) && pu > 0.9f,
              "planet assist: beyond-limb ray clamps near the edge");
    }

    std::printf(failures == 0 ? "\nALL TESTS PASSED\n" : "\n%d TEST(S) FAILED\n", failures);
    return failures == 0 ? 0 : 1;
}
