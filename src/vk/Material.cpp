#include "vk/Material.hpp"

#include <utility>

Material::Material(std::string name, std::string vertexShaderPath, std::string fragmentShaderPath)
    : name_(std::move(name)),
      vertexShaderPath_(std::move(vertexShaderPath)),
      fragmentShaderPath_(std::move(fragmentShaderPath)) {}
