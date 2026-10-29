#pragma once

#include "core/Math.hpp"

/// The viewport camera, navigated the way Blender's viewport is.
///
/// The camera always looks at `target` from a point on a sphere around it, so
/// no amount of dragging can lose the model: orbiting changes the two angles,
/// panning moves the pivot, and zooming changes the radius. Everything is kept
/// in the renderer's world convention, +Z up and -X forward, so pitch is an
/// elevation above the horizon and yaw swings around +Z.
///
/// The class owns the state and nothing else, which is what lets the debug UI
/// edit the very same numbers the renderer reads when it builds the view
/// matrix - there is no second copy of the camera to keep in step.
class OrbitCamera {
public:
    /// The framing a freshly loaded mesh starts with. Yaw 135 degrees puts the
    /// eye on the -X side, which is the side an imported mesh faces.
    static constexpr float kDefaultYaw = 2.3561945f;
    static constexpr float kDefaultPitch = 0.35f;
    static constexpr float kDefaultDistance = 3.0f;

    /// Pitch stops just short of a right angle: looking straight down would
    /// make the view direction parallel to the up vector, and lookAt would
    /// collapse. The distance limits keep the pivot outside the near plane and
    /// inside the far plane.
    static constexpr float kMinPitch = -1.5533f;   // -89 degrees
    static constexpr float kMaxPitch = 1.5533f;    // 89 degrees
    static constexpr float kMinDistance = 0.1f;
    static constexpr float kMaxDistance = 50.0f;

    float yaw = kDefaultYaw;
    float pitch = kDefaultPitch;
    float distance = kDefaultDistance;

    /// The point the camera looks at and orbits around, in world space.
    math::Vec3 target;

    /// Restores the defaults, including a pivot back at the origin.
    void reset();

    /// Swings the eye around the pivot. The deltas are mouse movement in
    /// pixels, so the gesture feels the same at any window size.
    void orbit(float deltaXPixels, float deltaYPixels);

    /// Slides the pivot in the view plane, so the model follows the mouse.
    /// worldPerPixel (see below) at the pivot's depth converts the movement
    /// into world units.
    void pan(float deltaXPixels, float deltaYPixels, float worldPerPixel);

    /// Multiplies the orbit distance; a positive step moves closer.
    void dolly(float steps);

    math::Vec3 eye() const;
    math::Mat4 viewMatrix() const;

    /// The camera's right and up axes in world space, both unit length.
    math::Vec3 right() const;
    math::Vec3 up() const;

    /// How many world units one pixel covers at the pivot's depth for a
    /// vertical field of view of fovYRadians. Needs the viewport height in
    /// pixels; a zero height (a minimized window) falls back to 1.
    float worldPerPixel(float fovYRadians, float viewportHeightPixels) const;
};
