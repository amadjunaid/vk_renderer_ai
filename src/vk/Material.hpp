#pragma once

#include <string>

/// How a mesh is shaded.
///
/// For now a material is only the shader pair the mesh is drawn with: the
/// descriptor sets, textures and the pipeline state that go with them will move
/// in here as the renderer grows, at which point GraphicsPipeline reads them
/// from the material instead of being handed the shaders directly.
class Material {
public:
    Material(std::string name, std::string vertexShaderPath, std::string fragmentShaderPath);

    const std::string& name() const { return name_; }

    /// Paths to the compiled SPIR-V. The caller supplies them - Renderer passes
    /// absolute paths, so they do not depend on the working directory.
    const std::string& vertexShader() const { return vertexShaderPath_; }
    const std::string& fragmentShader() const { return fragmentShaderPath_; }

private:
    std::string name_;
    std::string vertexShaderPath_;
    std::string fragmentShaderPath_;
};
