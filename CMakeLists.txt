cmake_minimum_required(VERSION 3.20)

project(deepseek_vk_renderer
    VERSION 0.1.0
    DESCRIPTION "Minimal Vulkan renderer that draws a triangle"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Keep the executable and the compiled SPIR-V in predictable places for both
# single-config (Ninja, Makefiles) and multi-config (Visual Studio) generators.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
foreach(config IN ITEMS DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_${config} ${CMAKE_BINARY_DIR}/bin)
endforeach()

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
    set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS Debug Release RelWithDebInfo MinSizeRel)
endif()

# Lets the C/C++ extension resolve includes from the real compile flags, so the
# editor config does not need hand-written platform-specific include paths.
# Honoured by the Ninja and Makefile generators (what Linux and macOS use);
# silently ignored by the Visual Studio generator.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------

include(FetchContent)

set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(GLFW_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(glfw
    GIT_REPOSITORY https://github.com/glfw/glfw.git
    GIT_TAG 3.4
    GIT_SHALLOW TRUE)
FetchContent_MakeAvailable(glfw)

find_package(Vulkan QUIET)
if(NOT Vulkan_FOUND)
    if(APPLE)
        # The loader/headers fallback below cannot work here: macOS has no
        # Vulkan driver of its own, only MoltenVK layered on Metal.
        message(FATAL_ERROR
            "Vulkan was not found. macOS has no native Vulkan implementation, so MoltenVK "
            "is required. Install the LunarG macOS SDK (https://vulkan.lunarg.com) and "
            "re-run CMake, or point CMake at an existing MoltenVK with "
            "-DVulkan_INCLUDE_DIR=... -DVulkan_LIBRARY=...")
    endif()

    # No Vulkan SDK installed: use the Khronos headers and link the loader that
    # ships with the GPU driver. Fine for plain (non-validation) builds.
    message(STATUS "Vulkan SDK not found - falling back to Vulkan-Headers + system loader")
    FetchContent_Declare(vulkan_headers
        GIT_REPOSITORY https://github.com/KhronosGroup/Vulkan-Headers.git
        GIT_TAG vulkan-sdk-1.4.363.0
        GIT_SHALLOW TRUE)
    FetchContent_MakeAvailable(vulkan_headers)

    add_library(Vulkan::Vulkan INTERFACE IMPORTED)
    target_link_libraries(Vulkan::Vulkan INTERFACE Vulkan::Headers)
    if(WIN32)
        target_link_libraries(Vulkan::Vulkan INTERFACE vulkan-1)
    else()
        target_link_libraries(Vulkan::Vulkan INTERFACE vulkan)
    endif()
endif()

# ---------------------------------------------------------------------------
# Shaders: GLSL -> SPIR-V at build time via glslc (ships with the Vulkan SDK)
# ---------------------------------------------------------------------------

# glslc is preferred, but some Linux distributions only package glslangValidator.
find_program(GLSL_COMPILER
    NAMES glslc glslangValidator
    HINTS ENV VULKAN_SDK
    PATH_SUFFIXES Bin bin)

if(WIN32 AND NOT GLSL_COMPILER)
    # The Windows SDK installer does not always leave the compiler on PATH for
    # shells opened earlier, so fall back to the default install location.
    file(GLOB _vulkan_sdk_glslc "C:/VulkanSDK/*/Bin/glslc.exe")
    if(_vulkan_sdk_glslc)
        list(SORT _vulkan_sdk_glslc)
        list(POP_BACK _vulkan_sdk_glslc GLSL_COMPILER)
    endif()
endif()

if(NOT GLSL_COMPILER)
    message(FATAL_ERROR
        "No shader compiler was found. Install the Vulkan SDK "
        "(https://vulkan.lunarg.com), or 'glslang-tools' / 'shaderc' on Linux, or "
        "'brew install shaderc' on macOS. Alternatively re-run CMake with "
        "-DGLSL_COMPILER=/path/to/glslc.")
endif()

# glslangValidator only emits Vulkan SPIR-V with -V; glslc does so by default.
get_filename_component(GLSL_COMPILER_NAME "${GLSL_COMPILER}" NAME)
if(GLSL_COMPILER_NAME MATCHES "^glslangValidator")
    set(GLSL_COMPILER_ARGS -V)
else()
    set(GLSL_COMPILER_ARGS "")
endif()
message(STATUS "shader compiler: ${GLSL_COMPILER} ${GLSL_COMPILER_ARGS}")

set(SHADER_OUTPUT_DIR ${CMAKE_BINARY_DIR}/shaders)
set(SHADER_SOURCES
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.vert
    ${CMAKE_CURRENT_SOURCE_DIR}/shaders/triangle.frag)

set(SHADER_SPIRV "")
foreach(shader ${SHADER_SOURCES})
    get_filename_component(shader_name ${shader} NAME)
    set(spirv_file ${SHADER_OUTPUT_DIR}/${shader_name}.spv)

    add_custom_command(
        OUTPUT ${spirv_file}
        COMMAND ${CMAKE_COMMAND} -E make_directory ${SHADER_OUTPUT_DIR}
        COMMAND ${GLSL_COMPILER} ${GLSL_COMPILER_ARGS} ${shader} -o ${spirv_file}
        DEPENDS ${shader}
        COMMENT "Compiling ${shader_name} -> SPIR-V"
        VERBATIM)

    list(APPEND SHADER_SPIRV ${spirv_file})
endforeach()

add_custom_target(vk_shaders DEPENDS ${SHADER_SPIRV})

# ---------------------------------------------------------------------------
# Renderer
# ---------------------------------------------------------------------------

add_executable(vk_triangle
    src/main.cpp
    src/core/Window.cpp
    src/vk/VulkanContext.cpp
    src/vk/Swapchain.cpp
    src/vk/GraphicsPipeline.cpp
    src/vk/Renderer.cpp)

target_include_directories(vk_triangle PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# The renderer loads its SPIR-V from this directory, so the working directory
# does not matter when launching the executable.
target_compile_definitions(vk_triangle PRIVATE SHADER_DIR="${SHADER_OUTPUT_DIR}")

if(WIN32)
    target_compile_definitions(vk_triangle PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

target_link_libraries(vk_triangle PRIVATE glfw Vulkan::Vulkan)
add_dependencies(vk_triangle vk_shaders)

if(MSVC)
    target_compile_options(vk_triangle PRIVATE /W4 /permissive- /utf-8)
    # Visual Studio only: makes F5 start the binary from the build tree.
    set_target_properties(vk_triangle PROPERTIES
        VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
else()
    target_compile_options(vk_triangle PRIVATE -Wall -Wextra -Wpedantic)
endif()
