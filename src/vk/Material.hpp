#pragma once

#include <string>

/// The shading parameters of a physically based material, mirroring the subset
/// of glTF's `pbrMetallicRoughness` model that this renderer implements.
///
/// These are the values the fragment shader consumes and the ones an imported
/// file carries, so a loader fills this straight from the file. The factors are
/// linear (glTF defines them in linear space, and an FBX material is converted
/// into the same model) and the swapchain is an sRGB format, so the hardware
/// does the display encoding - nothing here is gamma corrected by hand.
struct PbrMaterial {
    std::string name = "material";

    /// Linear RGB albedo for dielectrics, and the tint of the specular
    /// reflection for metals, plus the alpha channel (unused so far: the
    /// renderer does not blend).
    float baseColorFactor[4] = {1.0f, 1.0f, 1.0f, 1.0f};

    /// 0 = dielectric, 1 = metal. Controls how much of the base color becomes
    /// the specular reflection instead of the diffuse one.
    float metallicFactor = 1.0f;

    /// Perceptual roughness in [0, 1], where 0 is a mirror. The shader clamps it
    /// away from zero so a highlight never becomes a singularity.
    float roughnessFactor = 1.0f;

    /// Light the surface emits regardless of the scene lighting.
    float emissiveFactor[3] = {0.0f, 0.0f, 0.0f};

    /// glTF's `doubleSided`. Kept for display: the pipeline draws both faces of
    /// every mesh, which is what a double-sided material wants and is merely
    /// wasteful for a single-sided one.
    bool doubleSided = false;
};

/// How a mesh is shaded.
///
/// A material is the shader pair the mesh is drawn with, plus the parameters
/// those shaders read. The pipeline state and the textures (samplers,
/// descriptor sets, alpha modes) will move in here as the renderer grows, at
/// which point GraphicsPipeline will read them from the material instead of
/// being handed just the shader paths.
///
/// The parameters are mutable rather than fixed at construction because the
/// debug UI edits them live - which is also why `pbr()` has a non-const
/// overload: whoever owns the material hands that reference to the UI, and the
/// renderer reads the same object back every frame.
class Material {
public:
    Material(std::string name, std::string vertexShaderPath, std::string fragmentShaderPath);

    const std::string& name() const { return name_; }

    /// Paths to the compiled SPIR-V. The caller supplies them - Renderer passes
    /// absolute paths, so they do not depend on the working directory.
    const std::string& vertexShader() const { return vertexShaderPath_; }
    const std::string& fragmentShader() const { return fragmentShaderPath_; }

    /// Replaces every shading parameter at once, which is what importing a mesh
    /// does: the values arrive with the file.
    void setPbr(const PbrMaterial& pbr) { pbr_ = pbr; }

    const PbrMaterial& pbr() const { return pbr_; }
    PbrMaterial& pbr() { return pbr_; }

private:
    std::string name_;
    std::string vertexShaderPath_;
    std::string fragmentShaderPath_;

    PbrMaterial pbr_;
};
