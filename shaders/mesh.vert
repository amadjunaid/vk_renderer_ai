#version 450

// Transforms a mesh's vertices into clip space and hands the world-space
// position and normal to the fragment shader.
//
// The per-frame matrices and the light arrive in one uniform buffer (see
// SceneUniforms in src/vk/SceneUniformBuffer.hpp); the per-material constants
// are pushed by the fragment stage only, so this shader does not declare a push
// constant block at all. The vertex attributes are the ones declared by Vertex
// in src/vk/Mesh.hpp.

layout(set = 0, binding = 0) uniform SceneUniforms {
    mat4 model;
    mat4 viewProjection;
    vec4 cameraPosition;
    vec4 lightDirection;
    vec4 lightColor;
} scene;

layout(location = 0) in vec3 inPosition;
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inTexCoord;

layout(location = 0) out vec3 worldPosition;
layout(location = 1) out vec3 worldNormal;
layout(location = 2) out vec2 texCoord;

void main() {
    const vec4 position = scene.model * vec4(inPosition, 1.0);
    worldPosition = position.xyz;

    // The normal needs the inverse transpose of the model matrix, otherwise a
    // scale would shear it away from the surface. The model transform this
    // renderer builds is a rotation times a uniform scale, so the inverse
    // transpose is the rotation part - but going through the transpose of the
    // inverse costs almost nothing and stays correct if that ever changes.
    worldNormal = transpose(inverse(mat3(scene.model))) * inNormal;

    // Passed through for the texture sampling a later material will add.
    texCoord = inTexCoord;

    gl_Position = scene.viewProjection * position;
}
