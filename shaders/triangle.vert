#version 450

// Positions and colors come from the mesh's vertex buffer (see Vertex in
// src/vk/Mesh.hpp); the shader only applies the rotation to them.

// Supplied every frame by the renderer through vkCmdPushConstants.
layout(push_constant) uniform PushConstants {
    float angle;  // rotation around the origin, in radians
} pc;

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec3 inColor;

layout(location = 0) out vec3 fragColor;

void main() {
    // pc.angle is a runtime value, so these cannot be const: GLSL requires a
    // constant expression to initialize a const declaration.
    float c = cos(pc.angle);
    float s = sin(pc.angle);

    // glslang fills mat2 from columns, so mat2(c, s, -s, c) is the matrix
    // [[c, -s], [s, c]] - a counter-clockwise rotation.
    mat2 rotation = mat2(c, s, -s, c);

    gl_Position = vec4(rotation * inPosition, 0.0, 1.0);
    fragColor = inColor;
}
