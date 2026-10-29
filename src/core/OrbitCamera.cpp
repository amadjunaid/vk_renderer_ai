#include "core/OrbitCamera.hpp"

#include <algorithm>
#include <cmath>

namespace {

constexpr float kTwoPi = 6.28318530718f;
constexpr float kPi = 3.14159265359f;

/// Radians of rotation per pixel of drag. About a third of a degree, which
/// covers a half turn with a comfortable drag across the window.
constexpr float kOrbitRadiansPerPixel = 0.01f;

/// One mouse-wheel notch multiplies the distance by exp(-this).
constexpr float kZoomPerStep = 0.15f;

const math::Vec3 kWorldUp{0.0f, 0.0f, 1.0f};

/// Wraps an angle into [-pi, pi) so yaw stays readable in the UI and does not
/// lose precision over a long session.
float wrapAngle(float radians) {
    radians = std::fmod(radians + kPi, kTwoPi);
    if (radians < 0.0f) {
        radians += kTwoPi;
    }
    return radians - kPi;
}

}  // namespace

void OrbitCamera::reset() {
    yaw = kDefaultYaw;
    pitch = kDefaultPitch;
    distance = kDefaultDistance;
    target = math::Vec3{};
}

void OrbitCamera::orbit(float deltaXPixels, float deltaYPixels) {
    // The model follows the mouse: dragging right swings the eye to its own
    // left, and dragging down lifts the eye so the top of the model comes into
    // view - the same mapping Blender's middle-mouse orbit uses.
    yaw = wrapAngle(yaw - deltaXPixels * kOrbitRadiansPerPixel);
    pitch = std::clamp(pitch + deltaYPixels * kOrbitRadiansPerPixel, kMinPitch, kMaxPitch);
}

void OrbitCamera::pan(float deltaXPixels, float deltaYPixels, float worldPerPixel) {
    // Moving the pivot opposite to the drag makes the model travel with the
    // mouse instead of against it.
    const math::Vec3 offset =
        right() * (-deltaXPixels * worldPerPixel) + up() * (deltaYPixels * worldPerPixel);
    target = target + offset;
}

void OrbitCamera::dolly(float steps) {
    // Exponential rather than linear so a notch covers the same fraction of
    // the distance whether the camera is close or far away.
    const float scaled = distance * std::exp(-steps * kZoomPerStep);
    distance = std::clamp(scaled, kMinDistance, kMaxDistance);
}

math::Vec3 OrbitCamera::eye() const {
    // Clamped here rather than stored clamped, because the UI edits the fields
    // directly and a frame must never be built from an invalid camera.
    const float elevation = std::clamp(pitch, kMinPitch, kMaxPitch);
    const float radius = std::clamp(distance, kMinDistance, kMaxDistance);
    const float horizontal = std::cos(elevation) * radius;

    return target + math::Vec3{std::cos(yaw) * horizontal, std::sin(yaw) * horizontal,
                               std::sin(elevation) * radius};
}

math::Vec3 OrbitCamera::right() const {
    const math::Vec3 forward = math::normalize(target - eye());
    return math::normalize(math::cross(forward, kWorldUp));
}

math::Vec3 OrbitCamera::up() const {
    const math::Vec3 forward = math::normalize(target - eye());
    return math::cross(right(), forward);
}

math::Mat4 OrbitCamera::viewMatrix() const {
    // Z is up, so that is the reference the camera keeps level.
    return math::lookAt(eye(), target, kWorldUp);
}

float OrbitCamera::worldPerPixel(float fovYRadians, float viewportHeightPixels) const {
    if (viewportHeightPixels <= 0.0f) {
        return 0.0f;
    }
    const float visibleHeight = 2.0f * std::clamp(distance, kMinDistance, kMaxDistance) *
                                std::tan(fovYRadians * 0.5f);
    return visibleHeight / viewportHeightPixels;
}
