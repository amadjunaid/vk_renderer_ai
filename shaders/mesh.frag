#version 450

// A basic physically based metallic/roughness shader, fed by the values an
// imported glTF material carries.
//
// One directional light plus a cheap ambient fill: enough to show how metallic
// and roughness change the look of a surface, without an image based lighting
// pipeline behind it. The three functions below are the standard Cook-Torrance
// pieces - GGX for the microfacets, Schlick for the Fresnel term and Smith for
// the shadowing - written out rather than pulled from a library so the whole
// BRDF is visible in one place.

// Filled in by the renderer with vkCmdPushConstants before every draw. Must
// stay byte-compatible with MaterialPushConstants in vk/GraphicsPipeline.hpp:
// std430 puts each vec4 on a 16-byte boundary, so the emissive factor is
// padded to four floats on both sides even though only rgb is used.
layout(push_constant) uniform MaterialPushConstants {
    vec4 baseColorFactor;
    vec4 emissiveFactor;
    float metallicFactor;
    float roughnessFactor;
} material;

layout(set = 0, binding = 0) uniform SceneUniforms {
    mat4 model;
    mat4 viewProjection;
    vec4 cameraPosition;
    vec4 lightDirection;
    vec4 lightColor;
} scene;

layout(location = 0) in vec3 worldPosition;
layout(location = 1) in vec3 worldNormal;
layout(location = 2) in vec2 texCoord;

layout(location = 0) out vec4 outColor;

const float kPi = 3.14159265359;

// Fraction of the light a fully lit, black surface would still receive. The
// directional light leaves every surface facing away from it black, which makes
// a mesh hard to read; this stands in for the indirect light an environment map
// would provide. Not physically based, and deliberately small.
const float kAmbientStrength = 0.12;

// Microfacet distribution: how much of the surface is oriented along the
// halfway vector, which is what makes a rough surface spread its highlight.
float distributionGGX(float nDotH, float roughness) {
    const float a = roughness * roughness;
    const float a2 = a * a;
    const float denominator = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(kPi * denominator * denominator, 1e-6);
}

// Smith's shadowing term, using the remapped roughness that fits a direct light.
float geometrySchlickGGX(float nDotX, float roughness) {
    const float k = roughness * roughness * 0.5;
    return nDotX / max(nDotX * (1.0 - k) + k, 1e-6);
}

float geometrySmith(float nDotV, float nDotL, float roughness) {
    return geometrySchlickGGX(nDotV, roughness) * geometrySchlickGGX(nDotL, roughness);
}

// Fresnel: surfaces reflect more at grazing angles. `f0` is the reflectance at
// normal incidence.
vec3 fresnelSchlick(float cosTheta, vec3 f0) {
    return f0 + (1.0 - f0) * pow(clamp(1.0 - cosTheta, 0.0, 1.0), 5.0);
}

void main() {
    const vec3 baseColor = material.baseColorFactor.rgb;
    const float metallic = clamp(material.metallicFactor, 0.0, 1.0);
    // A perfectly smooth surface turns the GGX denominator into a divide by
    // zero, so roughness is kept away from zero.
    const float roughness = clamp(material.roughnessFactor, 0.04, 1.0);

    const vec3 normal = normalize(worldNormal);
    const vec3 view = normalize(scene.cameraPosition.xyz - worldPosition);
    const vec3 light = normalize(scene.lightDirection.xyz);
    const vec3 halfway = normalize(view + light);

    const float nDotL = max(dot(normal, light), 0.0);
    const float nDotV = max(dot(normal, view), 1e-4);
    const float nDotH = max(dot(normal, halfway), 0.0);
    const float hDotV = max(dot(halfway, view), 0.0);

    // Dielectrics reflect about 4% of the light head-on; a metal reflects its
    // base color instead and has no diffuse lobe left.
    const vec3 f0 = mix(vec3(0.04), baseColor, metallic);

    const vec3 fresnel = fresnelSchlick(hDotV, f0);
    const vec3 specular = (distributionGGX(nDotH, roughness) *
                           geometrySmith(nDotV, nDotL, roughness) * fresnel) /
                          max(4.0 * nDotV * nDotL, 1e-4);

    // Energy conservation: whatever is not reflected specularly is available to
    // the diffuse lobe, and metals have nothing left.
    const vec3 diffuse = (vec3(1.0) - fresnel) * (1.0 - metallic) * baseColor / kPi;

    const vec3 direct = (diffuse + specular) * scene.lightColor.rgb * nDotL;

    // A hemisphere-ish fill: upward-facing surfaces catch more of the sky. Z is
    // the world's up axis, so that is the component to weigh.
    const float skyAmount = mix(0.4, 1.0, 0.5 + 0.5 * normal.z);
    const vec3 ambient = baseColor * kAmbientStrength * skyAmount * scene.lightColor.rgb;

    // The swapchain is an sRGB format, so the hardware converts this linear
    // value to sRGB on write - no manual gamma here.
    outColor = vec4(direct + ambient + material.emissiveFactor.rgb, material.baseColorFactor.a);
}
