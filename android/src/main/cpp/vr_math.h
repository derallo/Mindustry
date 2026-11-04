#pragma once

// Pure math for turning a controller aim pose into a point on the virtual screen.
// Kept free of OpenXR/Android types so it can be unit-tested on the host.

#include <cmath>

namespace vr {

struct Vec3 {
    float x, y, z;
};

struct Quat {
    float x, y, z, w;
};

struct Pose {
    Quat orientation;
    Vec3 position;
};

struct ScreenGeometry {
    float width;
    float height;
    float z;
};

/** A screen that can be rotated and moved (the recenter feature does both). */
struct ScreenPose {
    Quat orientation;
    Vec3 position;
    float width;
    float height;
};

/** Rotates `v` by unit quaternion `q`. */
inline Vec3 rotate(const Quat &q, const Vec3 &v) {
    const float tx = 2.f * (q.y * v.z - q.z * v.y);
    const float ty = 2.f * (q.z * v.x - q.x * v.z);
    const float tz = 2.f * (q.x * v.y - q.y * v.x);

    return Vec3{
            v.x + q.w * tx + (q.y * tz - q.z * ty),
            v.y + q.w * ty + (q.z * tx - q.x * tz),
            v.z + q.w * tz + (q.x * ty - q.y * tx),
    };
}

inline float dot(const Vec3 &a, const Vec3 &b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 cross(const Vec3 &a, const Vec3 &b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline Vec3 normalized(const Vec3 &v) {
    const float len = std::sqrt(dot(v, v));
    return len > 1e-6f ? Vec3{v.x / len, v.y / len, v.z / len} : Vec3{0.f, 1.f, 0.f};
}

/**
 * Intersects the aim ray with an arbitrarily posed screen and returns normalized texture
 * coordinates in [0,1]. Returns false when the ray misses.
 */
inline bool rayToScreen(const Pose &aim, const ScreenPose &screen, float &outU, float &outV) {
    const Vec3 dir = rotate(aim.orientation, Vec3{0.f, 0.f, -1.f});
    const Vec3 normal = rotate(screen.orientation, Vec3{0.f, 0.f, 1.f});

    const float denom = dot(dir, normal);
    if (std::fabs(denom) < 1e-5f) return false;

    const Vec3 toScreen{screen.position.x - aim.position.x,
                        screen.position.y - aim.position.y,
                        screen.position.z - aim.position.z};

    const float t = dot(toScreen, normal) / denom;
    if (t <= 0.f) return false;

    const Vec3 hit{aim.position.x + dir.x * t,
                   aim.position.y + dir.y * t,
                   aim.position.z + dir.z * t};

    const Vec3 local{hit.x - screen.position.x, hit.y - screen.position.y, hit.z - screen.position.z};
    const Vec3 right = rotate(screen.orientation, Vec3{1.f, 0.f, 0.f});
    const Vec3 up = rotate(screen.orientation, Vec3{0.f, 1.f, 0.f});

    const float u = dot(local, right) / screen.width + 0.5f;
    const float v = dot(local, up) / screen.height + 0.5f;

    if (u < 0.f || u > 1.f || v < 0.f || v > 1.f) return false;

    outU = u;
    outV = v;
    return true;
}

/**
 * Screen placement used by the grip recenter action: `distance` metres along the head's
 * forward vector (yaw only, so the screen stays vertical), rotated to face back at the user.
 */
inline ScreenPose recenteredScreen(float headYaw, const Vec3 &head, float distance, float width, float height){
    const float fx = -std::sin(headYaw), fz = -std::cos(headYaw);

    ScreenPose out;
    out.orientation = Quat{0.f, std::sin(headYaw * 0.5f), 0.f, std::cos(headYaw * 0.5f)};
    out.position = Vec3{head.x + fx * distance, head.y, head.z + fz * distance};
    out.width = width;
    out.height = height;
    return out;
}

/** Visible window into the overscan capture texture, normalized [0,1] (bottom-left origin). */
struct LookCrop {
    float x, y, w, h;
};

inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/**
 * Head-driven crop for flat-VR look-around. `lookRight`/`lookUp` are head-turn offsets in
 * radians (positive = looking right/up) from the recenter reference; the window reaches the
 * overscan edge at `fullPanRad`. `visibleFrac` is the fraction of the texture shown.
 */
inline LookCrop lookCrop(float lookRight, float lookUp, float visibleFrac, float fullPanRad) {
    const float maxOff = (1.f - visibleFrac) * 0.5f;
    const float nx = clampf(lookRight / fullPanRad, -1.f, 1.f);
    const float ny = clampf(lookUp / fullPanRad, -1.f, 1.f);
    return LookCrop{0.5f + nx * maxOff - visibleFrac * 0.5f,
                    0.5f + ny * maxOff - visibleFrac * 0.5f, visibleFrac, visibleFrac};
}

/** Radial fade multiplier for the depth illusion: 1 at centre, darker toward the corners. */
inline float radialFade(float distNorm, float inner, float outer, float strength) {
    const float t = clampf((distNorm - inner) / (outer - inner), 0.f, 1.f);
    const float s = t * t * (3.f - 2.f * t); // smoothstep
    return 1.f - strength * s;
}

// ---------------------------------------------------------------------------------------------
// Fullscreen panoramic world view
//
// The old virtual screen was a quad: a rectangle floating in front of the user. The fullscreen
// path instead treats the overscan game capture as a cylindrical panorama wrapped around a
// fixed point in LOCAL space. The game camera's centre maps to the centre of the panorama, so
// the centre of the game stays dead ahead; turning the head reveals the map that was beyond
// the edge of the old screen. The cylinder radius gives real per-eye parallax and lets a head
// lean produce motion parallax. Everything here is pure math and host-tested.
// ---------------------------------------------------------------------------------------------

/**
 * Fixed cylinder / dome that the game capture is wrapped around. `yaw` is head yaw at
 * recenter. `spanH`/`spanV` are the **central screen's** full angular span (the game capture,
 * including the HUD); the halo beyond it is visual-only and not interactive.
 */
struct PanoView {
    Vec3 center;        // cylinder axis at eye height, LOCAL space
    float yaw;          // reference forward yaw, radians (0 = -Z)
    float radius;       // cylinder radius, metres
    float spanH;        // central screen horizontal angular span, radians
    float spanV;        // central screen vertical angular span, radians
};

/**
 * Mild spherical/barrel warp used to make the central screen read as a convex dome rather
 * than a flat rectangle. `f(0)=0`, `f(1)=1`; the centre is magnified and the edges compressed.
 * The same function must be used by the input mapping and the GLSL shader.
 */
constexpr float convexWarpK = 0.f; // linear panorama; kept for a possible dome mode
constexpr float halfPi = 1.57079632679f;

inline float convexWarp(float s) {
    return (1.f - convexWarpK) * s + convexWarpK * std::sin(s * halfPi);
}

/** Inverse of convexWarp (Newton, monotonic, 6 iterations is plenty). */
inline float convexUnwarp(float x) {
    float s = x;
    for (int i = 0; i < 6; i++) {
        const float f = convexWarp(s) - x;
        const float df = (1.f - convexWarpK) + convexWarpK * halfPi * std::cos(s * halfPi);
        if (std::fabs(df) < 1e-6f) break;
        s -= f / df;
    }
    return clampf(s, -1.f, 1.f);
}

/** Unit vectors of the panorama's reference frame (yaw only, +Y up). */
inline Vec3 panoForward(const PanoView &pano) {
    return Vec3{-std::sin(pano.yaw), 0.f, -std::cos(pano.yaw)};
}

inline Vec3 panoRight(const PanoView &pano) {
    return Vec3{std::cos(pano.yaw), 0.f, -std::sin(pano.yaw)};
}

/**
 * Intersects a ray with the panorama cylinder. Returns the far intersection (the one on the
 * side the user is looking at), or false when the ray never reaches the cylinder.
 */
inline bool intersectPano(const Pose &aim, const PanoView &pano, Vec3 &outHit) {
    const Vec3 dir = rotate(aim.orientation, Vec3{0.f, 0.f, -1.f});
    const float ox = aim.position.x - pano.center.x;
    const float oz = aim.position.z - pano.center.z;
    const float a = dir.x * dir.x + dir.z * dir.z;
    if (a < 1e-7f) return false;

    const float b = 2.f * (ox * dir.x + oz * dir.z);
    const float c = ox * ox + oz * oz - pano.radius * pano.radius;
    const float disc = b * b - 4.f * a * c;
    if (disc < 0.f) return false;

    const float t = (-b + std::sqrt(disc)) / (2.f * a);
    if (t <= 0.f) return false;

    outHit = Vec3{aim.position.x + dir.x * t,
                  aim.position.y + dir.y * t,
                  aim.position.z + dir.z * t};
    return true;
}

/**
 * Projects a panorama hit point (or any LOCAL point) into capture UV. `u` grows right, `v`
 * grows up (bottom-left origin, matching the capture texture). Returns false outside the
 * capture's angular span.
 */
inline bool panoProject(const PanoView &pano, const Vec3 &point, float &outU, float &outV) {
    const Vec3 delta{point.x - pano.center.x, point.y - pano.center.y, point.z - pano.center.z};
    const Vec3 fwd = panoForward(pano), right = panoRight(pano);

    const float x = dot(delta, right);
    const float z = dot(delta, fwd);
    if (z <= 0.f) return false;

    const float yawRel = std::atan2(x, z);
    const float pitch = std::atan2(delta.y, std::sqrt(x * x + z * z));
    const float sx = yawRel / (pano.spanH * 0.5f);
    const float sy = pitch / (pano.spanV * 0.5f);
    if (std::fabs(sx) > 1.f || std::fabs(sy) > 1.f) return false;

    outU = 0.5f + 0.5f * convexWarp(sx);
    outV = 0.5f + 0.5f * convexWarp(sy);
    return true;
}

/** Aim ray -> capture UV through the panorama cylinder. False when the ray misses the map. */
inline bool rayToPano(const Pose &aim, const PanoView &pano, float &outU, float &outV) {
    Vec3 hit;
    if (!intersectPano(aim, pano, hit)) return false;
    return panoProject(pano, hit, outU, outV);
}

/** Capture UV -> unit direction in LOCAL space (used to anchor 3D content to the map). */
// ---------------------------------------------------------------------------------------------
// Globe / sphere view. The capture is mapped onto the visible front hemisphere of a sphere that
// sits in front of the scene. The centre of the sphere faces the eye (flat at the centre); toward
// the limb the surface turns away, so the image compresses and vanishes like a globe. `k` is the
// sphere radius as a fraction of its distance (bigger k = a larger, flatter globe).
// ---------------------------------------------------------------------------------------------
struct GlobeView {
    Vec3 center;
    float yaw;
    float distance;
    float k;
    float halfLon;   // longitude span / 2 (radians)
    float halfLat;   // latitude span / 2 (radians)
};

inline Vec3 globeFwd(float yaw) { return Vec3{-std::sin(yaw), 0.f, -std::cos(yaw)}; }
inline Vec3 globeRight(float yaw) { return Vec3{std::cos(yaw), 0.f, -std::sin(yaw)}; }

inline bool globeProject(const Pose &eye, const GlobeView &globe, float &u, float &v) {
    const Vec3 fwd = globeFwd(globe.yaw);
    const Vec3 right = globeRight(globe.yaw);
    const Vec3 up{0.f, 1.f, 0.f};
    const Vec3 sphere{globe.center.x + fwd.x * globe.distance,
                      globe.center.y + fwd.y * globe.distance,
                      globe.center.z + fwd.z * globe.distance};
    const float radius = globe.distance * globe.k;
    const Vec3 dir = rotate(eye.orientation, Vec3{0.f, 0.f, -1.f});
    const Vec3 oc{eye.position.x - sphere.x, eye.position.y - sphere.y, eye.position.z - sphere.z};
    const float b = dot(oc, dir);
    const float c = dot(oc, oc) - radius * radius;
    const float disc = b * b - c;
    if (disc < 0.f) return false;
    const float t = -b - std::sqrt(disc);
    if (t <= 0.f) return false;
    const Vec3 point{eye.position.x + dir.x * t, eye.position.y + dir.y * t,
                     eye.position.z + dir.z * t};
    const Vec3 q{point.x - sphere.x, point.y - sphere.y, point.z - sphere.z};
    // The visible hemisphere is the one facing the eye: its outward normal points back
    // along -fwd, so the front surface has q·fwd < 0. Longitude is measured from -fwd.
    if (dot(q, fwd) >= 0.f) return false;
    const float lon = std::atan2(dot(q, right), -dot(q, fwd));
    const float lat = std::asin(clampf(dot(q, up) / radius, -1.f, 1.f));
    u = 0.5f + lon / (2.f * globe.halfLon);
    v = 0.5f + lat / (2.f * globe.halfLat);
    return u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f;
}

/** Aim-assist variant: if the ray misses the sphere, intersect the flat plane at the
 *  globe's front instead, so the controller cursor still maps onto the visible globe. */
inline bool globeProjectAssist(const Pose &eye, const GlobeView &globe, float &u, float &v) {
    if (globeProject(eye, globe, u, v)) return true;
    const Vec3 fwd = globeFwd(globe.yaw);
    const Vec3 right = globeRight(globe.yaw);
    const Vec3 up{0.f, 1.f, 0.f};
    const Vec3 front{globe.center.x + fwd.x * (globe.distance - globe.distance * globe.k),
                     globe.center.y + fwd.y * (globe.distance - globe.distance * globe.k),
                     globe.center.z + fwd.z * (globe.distance - globe.distance * globe.k)};
    const Vec3 dir = rotate(eye.orientation, Vec3{0.f, 0.f, -1.f});
    const float denom = dot(dir, fwd);
    if (std::fabs(denom) < 1e-4f) return false;
    const float t = dot(Vec3{front.x - eye.position.x, front.y - eye.position.y, front.z - eye.position.z}, fwd) / denom;
    if (t <= 0.f) return false;
    const Vec3 hit{eye.position.x + dir.x * t, eye.position.y + dir.y * t, eye.position.z + dir.z * t};
    const Vec3 local{hit.x - front.x, hit.y - front.y, hit.z - front.z};
    const float planeHalfW = (globe.distance * globe.k) * std::tan(globe.halfLon);
    const float planeHalfH = (globe.distance * globe.k) * std::tan(globe.halfLat);
    const float px = dot(local, right) / planeHalfW * 0.5f;
    const float py = dot(local, up) / planeHalfH * 0.5f;
    u = 0.5f + px;
    v = 0.5f + py;
    return u >= 0.f && u <= 1.f && v >= 0.f && v <= 1.f;
}

inline Vec3 globeDirection(const GlobeView &globe, float u, float v) {
    const Vec3 fwd = globeFwd(globe.yaw);
    const Vec3 right = globeRight(globe.yaw);
    const Vec3 up{0.f, 1.f, 0.f};
    const Vec3 sphere{globe.center.x + fwd.x * globe.distance,
                      globe.center.y + fwd.y * globe.distance,
                      globe.center.z + fwd.z * globe.distance};
    const float radius = globe.distance * globe.k;
    const float lon = (u - 0.5f) * 2.f * globe.halfLon;
    const float lat = (v - 0.5f) * 2.f * globe.halfLat;
    const float cl = std::cos(lat);
    const Vec3 front{-fwd.x, -fwd.y, -fwd.z}; // sphere front (toward the eye)
    const Vec3 q{(front.x * cl * std::cos(lon) + right.x * cl * std::sin(lon) + up.x * std::sin(lat)) * radius,
                 (front.y * cl * std::cos(lon) + right.y * cl * std::sin(lon) + up.y * std::sin(lat)) * radius,
                 (front.z * cl * std::cos(lon) + right.z * cl * std::sin(lon) + up.z * std::sin(lat)) * radius};
    const Vec3 point{sphere.x + q.x, sphere.y + q.y, sphere.z + q.z};
    return normalized(Vec3{point.x - globe.center.x, point.y - globe.center.y, point.z - globe.center.z});
}

inline Vec3 panoDirection(const PanoView &pano, float u, float v) {
    const float yawRel = convexUnwarp((u - 0.5f) * 2.f) * pano.spanH * 0.5f;
    const float pitch = convexUnwarp((v - 0.5f) * 2.f) * pano.spanV * 0.5f;
    const Vec3 fwd = panoForward(pano), right = panoRight(pano);
    const float cp = std::cos(pitch);
    return Vec3{
            fwd.x * cp * std::cos(yawRel) + right.x * std::sin(yawRel) * cp,
            std::sin(pitch),
            fwd.z * cp * std::cos(yawRel) + right.z * std::sin(yawRel) * cp,
    };
}

/**
 * Axis-aligned convenience overload (identity orientation, plane at `screen.z`), kept for
 * tests and for the common non-recentered case.
 *
 * `u` grows to the right, `v` grows upward in the texture's coordinate space, so the
 * Java layer flips v when producing top-left-origin screen pixels.
 */
inline bool rayToScreen(const Pose &aim, const ScreenGeometry &screen, float &outU, float &outV) {
    const ScreenPose pose{Quat{0.f, 0.f, 0.f, 1.f}, Vec3{0.f, 0.f, screen.z}, screen.width, screen.height};
    return rayToScreen(aim, pose, outU, outV);
}

} // namespace vr
