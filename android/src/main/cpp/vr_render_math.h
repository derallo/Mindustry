#pragma once

// Small, dependency-free 4x4 matrix helpers for the fullscreen stereo path.
// Column-major, matching OpenGL/OpenXR (`m[col * 4 + row]`). Kept in this
// header so the host tests can verify the camera math without any GL context.

#include "vr_math.h"

namespace vr {

struct Mat4 {
    float m[16];
};

inline Mat4 mat4Identity() {
    Mat4 r{};
    r.m[0] = r.m[5] = r.m[10] = r.m[15] = 1.f;
    return r;
}

inline Mat4 mat4Mul(const Mat4 &a, const Mat4 &b) {
    Mat4 r{};
    for (int c = 0; c < 4; c++) {
        for (int row = 0; row < 4; row++) {
            float sum = 0.f;
            for (int k = 0; k < 4; k++) {
                sum += a.m[k * 4 + row] * b.m[c * 4 + k];
            }
            r.m[c * 4 + row] = sum;
        }
    }
    return r;
}

inline Mat4 mat4Translation(float x, float y, float z) {
    Mat4 r = mat4Identity();
    r.m[12] = x;
    r.m[13] = y;
    r.m[14] = z;
    return r;
}

inline Mat4 mat4Scale(float s) {
    Mat4 r = mat4Identity();
    r.m[0] = r.m[5] = r.m[10] = s;
    return r;
}

inline Mat4 mat4RotationX(float rad) {
    Mat4 r = mat4Identity();
    const float c = std::cos(rad), s = std::sin(rad);
    r.m[5] = c;
    r.m[6] = s;
    r.m[9] = -s;
    r.m[10] = c;
    return r;
}

inline Mat4 mat4RotationY(float rad) {
    Mat4 r = mat4Identity();
    const float c = std::cos(rad), s = std::sin(rad);
    r.m[0] = c;
    r.m[2] = -s;
    r.m[8] = s;
    r.m[10] = c;
    return r;
}

inline Mat4 mat4RotationZ(float rad) {
    Mat4 r = mat4Identity();
    const float c = std::cos(rad), s = std::sin(rad);
    r.m[0] = c;
    r.m[1] = s;
    r.m[4] = -s;
    r.m[5] = c;
    return r;
}

/** Rotation matrix for a unit quaternion (same convention as vr_math.h rotate()). */
inline Mat4 mat4Quat(const Quat &q) {
    const float x = q.x, y = q.y, z = q.z, w = q.w;
    Mat4 r = mat4Identity();
    r.m[0] = 1.f - 2.f * (y * y + z * z);
    r.m[1] = 2.f * (x * y + z * w);
    r.m[2] = 2.f * (x * z - y * w);
    r.m[4] = 2.f * (x * y - z * w);
    r.m[5] = 1.f - 2.f * (x * x + z * z);
    r.m[6] = 2.f * (y * z + x * w);
    r.m[8] = 2.f * (x * z + y * w);
    r.m[9] = 2.f * (y * z - x * w);
    r.m[10] = 1.f - 2.f * (x * x + y * y);
    return r;
}

/** Inverse of a rigid transform (world-from-eye -> eye-from-world). */
inline Mat4 mat4View(const Quat &orientation, const Vec3 &position) {
    const Mat4 r = mat4Quat(orientation);
    Mat4 v = mat4Identity();
    // Transpose the rotation.
    v.m[0] = r.m[0]; v.m[1] = r.m[4]; v.m[2] = r.m[8];
    v.m[4] = r.m[1]; v.m[5] = r.m[5]; v.m[6] = r.m[9];
    v.m[8] = r.m[2]; v.m[9] = r.m[6]; v.m[10] = r.m[10];
    v.m[12] = -(v.m[0] * position.x + v.m[4] * position.y + v.m[8] * position.z);
    v.m[13] = -(v.m[1] * position.x + v.m[5] * position.y + v.m[9] * position.z);
    v.m[14] = -(v.m[2] * position.x + v.m[6] * position.y + v.m[10] * position.z);
    return v;
}

/** OpenGL projection from the runtime's asymmetric FOV tangents, near/far positive. */
inline Mat4 mat4ProjectionFov(float tanLeft, float tanRight, float tanDown, float tanUp,
                              float near, float far) {
    Mat4 p{};
    const float rl = tanRight - tanLeft;
    const float ud = tanUp - tanDown;
    p.m[0] = 2.f / rl;
    p.m[5] = 2.f / ud;
    p.m[8] = (tanRight + tanLeft) / rl;
    p.m[9] = (tanUp + tanDown) / ud;
    p.m[10] = -(far + near) / (far - near);
    p.m[11] = -1.f;
    p.m[14] = -(2.f * far * near) / (far - near);
    return p;
}

inline Vec3 mat4TransformPoint(const Mat4 &a, const Vec3 &v) {
    return Vec3{
            a.m[0] * v.x + a.m[4] * v.y + a.m[8] * v.z + a.m[12],
            a.m[1] * v.x + a.m[5] * v.y + a.m[9] * v.z + a.m[13],
            a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z + a.m[14],
    };
}

/** Returns the xyz components after the perspective divide (for test assertions). */
inline Vec3 mat4Project(const Mat4 &a, const Vec3 &v) {
    const float x = a.m[0] * v.x + a.m[4] * v.y + a.m[8] * v.z + a.m[12];
    const float y = a.m[1] * v.x + a.m[5] * v.y + a.m[9] * v.z + a.m[13];
    const float z = a.m[2] * v.x + a.m[6] * v.y + a.m[10] * v.z + a.m[14];
    const float w = a.m[3] * v.x + a.m[7] * v.y + a.m[11] * v.z + a.m[15];
    const float iw = std::fabs(w) > 1e-8f ? 1.f / w : 0.f;
    return Vec3{x * iw, y * iw, z * iw};
}

} // namespace vr
