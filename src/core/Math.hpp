#pragma once

#include <cmath>

/// The handful of vector and matrix types the renderer needs.
///
/// There is no matrix library in this build: one mesh transform, one orbit
/// camera and one projection chain do not justify a dependency, and keeping the
/// math here makes the conventions explicit - which matters, because a silently
/// wrong convention produces a mesh that is mirrored, upside down, or clipped
/// away by the near plane.
///
/// The conventions, all chosen to match what the shaders expect:
///  * Right-handed world space with **+Z up and -X forward**. This is the
///    renderer's own convention; glTF is +Y up with the front of an asset
///    facing +Z, so imported files are rotated into it on the way in (see
///    worldConventionTransform in io/MeshLoader.cpp).
///  * Column vectors, so a transform is applied as `M * v`.
///  * Column-major storage: element (row r, column c) lives at `m[c * 4 + r]`.
///    That is exactly GLSL's `mat4` layout, so a matrix can be copied into a
///    uniform buffer without being transposed.
namespace math {

struct Vec3 {
    float x = 0.0f;
    float y = 0.0f;
    float z = 0.0f;
};

inline Vec3 operator+(const Vec3& a, const Vec3& b) {
    return Vec3{a.x + b.x, a.y + b.y, a.z + b.z};
}

inline Vec3 operator-(const Vec3& a, const Vec3& b) {
    return Vec3{a.x - b.x, a.y - b.y, a.z - b.z};
}

inline Vec3 operator*(const Vec3& v, float scale) {
    return Vec3{v.x * scale, v.y * scale, v.z * scale};
}

inline float dot(const Vec3& a, const Vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

inline Vec3 cross(const Vec3& a, const Vec3& b) {
    return Vec3{a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x};
}

inline float length(const Vec3& v) {
    return std::sqrt(dot(v, v));
}

/// A zero-length input comes back as the zero vector rather than a NaN, so
/// callers do not have to guard every normal before using it.
inline Vec3 normalize(const Vec3& v) {
    const float len = length(v);
    return len > 0.0f ? v * (1.0f / len) : Vec3{};
}

/// A 4x4 matrix in column-major order. The default value is the identity.
struct Mat4 {
    float m[16] = {1.0f, 0.0f, 0.0f, 0.0f,
                   0.0f, 1.0f, 0.0f, 0.0f,
                   0.0f, 0.0f, 1.0f, 0.0f,
                   0.0f, 0.0f, 0.0f, 1.0f};
};

/// `a * b`, i.e. the transform that applies `b` first and then `a`.
inline Mat4 multiply(const Mat4& a, const Mat4& b) {
    Mat4 result{};
    for (int column = 0; column < 4; ++column) {
        for (int row = 0; row < 4; ++row) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) {
                sum += a.m[k * 4 + row] * b.m[column * 4 + k];
            }
            result.m[column * 4 + row] = sum;
        }
    }
    return result;
}

inline Mat4 translation(const Vec3& offset) {
    Mat4 result;
    result.m[12] = offset.x;
    result.m[13] = offset.y;
    result.m[14] = offset.z;
    return result;
}

/// A uniform scale on all three axes. The mesh normal matrix is derived from
/// the inverse transpose, so uniform scaling never skews the normals.
inline Mat4 scaling(float scale) {
    Mat4 result;
    result.m[0] = scale;
    result.m[5] = scale;
    result.m[10] = scale;
    return result;
}

/// Right-handed perspective projection writing Vulkan clip space.
///
/// Two differences from the OpenGL version of this matrix matter:
///  * Vulkan's depth range is [0, 1] instead of [-1, 1], which changes the two
///    depth entries of the third column.
///  * Vulkan's framebuffer origin is the top-left corner, so the Y axis is
///    flipped (m[5] is negative) to keep the scene upright. The side effect is
///    that a counter-clockwise triangle in world space ends up clockwise in
///    clip space, which the pipeline's winding setting has to account for.
inline Mat4 perspective(float fovYRadians, float aspect, float nearPlane, float farPlane) {
    const float focal = 1.0f / std::tan(fovYRadians * 0.5f);

    Mat4 result{};
    result.m[0] = focal / aspect;
    result.m[5] = -focal;
    result.m[10] = farPlane / (nearPlane - farPlane);
    result.m[11] = -1.0f;
    result.m[14] = (farPlane * nearPlane) / (nearPlane - farPlane);
    // Everything else stays zero: this is not the identity matrix.
    result.m[15] = 0.0f;
    return result;
}

/// Builds the world-to-view matrix for a camera at `eye` looking at `center`.
/// `up` only has to be roughly "up": it is orthogonalised here.
inline Mat4 lookAt(const Vec3& eye, const Vec3& center, const Vec3& up) {
    const Vec3 forward = normalize(center - eye);
    const Vec3 right = normalize(cross(forward, up));
    const Vec3 trueUp = cross(right, forward);

    Mat4 result;
    result.m[0] = right.x;
    result.m[4] = right.y;
    result.m[8] = right.z;
    result.m[1] = trueUp.x;
    result.m[5] = trueUp.y;
    result.m[9] = trueUp.z;
    result.m[2] = -forward.x;
    result.m[6] = -forward.y;
    result.m[10] = -forward.z;
    result.m[12] = -dot(right, eye);
    result.m[13] = -dot(trueUp, eye);
    result.m[14] = dot(forward, eye);
    return result;
}

}  // namespace math
