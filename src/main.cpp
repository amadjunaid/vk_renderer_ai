#include "core/Window.hpp"
#include "vk/Renderer.hpp"
#include "vk/VulkanContext.hpp"

#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>

// CMake bakes in the absolute path of the compiled SPIR-V and of the data
// directory that holds the model files; these fallbacks only apply to builds
// that bypass CMake entirely.
#ifndef SHADER_DIR
#define SHADER_DIR "shaders"
#endif
#ifndef DATA_DIR
#define DATA_DIR "data"
#endif

namespace {

constexpr std::uint32_t kWindowWidth = 1280;
constexpr std::uint32_t kWindowHeight = 720;

}  // namespace

int main() {
    try {
        Window window(kWindowWidth, kWindowHeight, "Vulkan Triangle");

        // Requested unconditionally: VulkanContext falls back to a warning if
        // the VK_LAYER_KHRONOS_validation layer is not installed.
        constexpr bool kEnableValidation = true;

        VulkanContext context(window, kEnableValidation);

        // The ImGui menu bar, toolbar and model window are on by default. Pass
        // false as the last argument to render the bare mesh instead.
        Renderer renderer(context, window, SHADER_DIR, DATA_DIR);
        renderer.run();
    } catch (const std::exception& exception) {
        std::cerr << "Fatal: " << exception.what() << '\n';
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}
