#version 450

// The triangle has no vertex buffer yet: positions and colors are generated
// from gl_VertexIndex so the pipeline needs no vertex input state.

// Supplied every frame by the renderer through vkCmdPushConstants.
layout(push_constant) uniform PushConstants {
    float angle;  // rotation around the origin, in radians
} pc;

layout(location = 0) out vec3 fragColor;

const vec2 kPositions[3] = vec2[](
    vec2( 0.0, -0.5),
    vec2( 0.5,  0.5),
    vec2(-0.5,  0.5)
);

const vec3 kColors[3] = vec3[](
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, 1.0)
);

void main() {
    // pc.angle is a runtime value, so these cannot be const: GLSL requires a
    // constant expression to initialize a const declaration.
    float c = cos(pc.angle);
    float s = sin(pc.angle);

    // glslang fills mat2 from columns, so mat2(c, s, -s, c) is the matrix
    // [[c, -s], [s, c]] - a counter-clockwise rotation.
    mat2 rotation = mat2(c, s, -s, c);

    vec2 position = rotation * kPositions[gl_VertexIndex];

    gl_Position = vec4(position, 0.0, 1.0);
    fragColor = kColors[gl_VertexIndex];
}
