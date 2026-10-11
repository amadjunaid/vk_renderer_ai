# deepseek_vk_renderer

A minimal Vulkan renderer in C++20 that opens a window and draws a single triangle.
The goal of this first milestone is a clean, correct render loop with no abstractions
that will have to be torn out later.

![status](https://img.shields.io/badge/status-triangle-blue)

## What it does today

- Creates a Vulkan instance with the `VK_LAYER_KHRONOS_validation` debug messenger
  (falls back to a warning if the layer is not installed).
- Picks a physical device that has a graphics queue, a present queue and `VK_KHR_swapchain`,
  preferring a discrete GPU.
- Builds a swapchain, image views and framebuffers, and recreates them on window resize
  or minimize/restore.
- Compiles GLSL to SPIR-V at build time and creates a graphics pipeline from it.
- Renders with 2 frames in flight, using per-frame fences, per-image present semaphores
  and per-image command buffers.
- Triangles are generated in the vertex shader, so no vertex buffer or descriptor sets
  are involved yet. The rotation is a push constant, and the vertex shader turns that
  angle into a 2D rotation matrix.

## Requirements

| Dependency | Notes |
| --- | --- |
| CMake | 3.20 or newer |
| C++ compiler | MSVC 2022 (Visual Studio Build Tools are enough), GCC 11+, or Clang 14+ |
| [Vulkan SDK](https://vulkan.lunarg.com) | Provides the loader, validation layers and `glslc` |
| Git | Used by CMake to fetch GLFW |
| [C/C++ extension for VS Code](https://marketplace.visualstudio.com/items?itemName=ms-vscode.cpptools) | Only needed to debug or to get IntelliSense; provides the `cppvsdbg` debug type |

On Windows, one way to get everything:

```powershell
winget install --id Kitware.CMake -e
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install --id KhronosGroup.VulkanSDK -e
```

GLFW 3.4 is downloaded automatically by CMake, so it does not need to be installed.
If the Vulkan SDK is missing, CMake falls back to the Khronos headers plus the system
`vulkan-1` loader; that path builds fine but cannot enable validation.

## Build and run

The renderer looks for its SPIR-V next to the build directory, so it can be launched
from anywhere.

### Windows / MSVC

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
.\build\bin\vk_triangle.exe
```

### Debugging (VS Code, Windows)

`.vscode/launch.json` ships a `Debug (MSVC)` configuration: pressing F5 builds the
Debug configuration and launches it under `cppvsdbg`, with the renderer's console
output in the integrated terminal. `cppvsdbg` comes from the C/C++ extension
(`ms-vscode.cpptools`), which `.vscode/extensions.json` recommends; without it F5
fails with *"The debug type 'cppvsdbg' is not supported"*.

### Ninja or Linux

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
./build/bin/vk_triangle
```

If CMake cannot find `glslc`, pass it explicitly:

```powershell
cmake -S . -B build -DGLSL_COMPILER="C:/VulkanSDK/1.4.363.0/Bin/glslc.exe"
```

### Verifying the output

`tools/capture_window.ps1` screenshots the running window and samples a few known
pixel positions, which is handy after changing the triangle or the clear color:

```powershell
powershell -ExecutionPolicy Bypass -File tools/capture_window.ps1 -Out build/verify.png
```

## Project layout

```
CMakeLists.txt              build: fetches GLFW, finds Vulkan + glslc, compiles shaders
shaders/
  triangle.vert             positions/colors from gl_VertexIndex, rotated by a push constant
  triangle.frag             passes the interpolated color through
src/
  main.cpp                  creates the window, context and renderer, then runs it
  core/Window.hpp/.cpp      GLFW lifetime, resize flag, VkSurfaceKHR creation
  vk/VkCheck.hpp            VK_CHECK macro and VkResult -> string helper
  vk/VulkanContext.hpp/.cpp instance, debug messenger, surface, device, queues
  vk/Swapchain.hpp/.cpp     swapchain, image views, format/present-mode/extent selection
  vk/GraphicsPipeline.hpp/.cpp  render pass and the triangle pipeline
  vk/Renderer.hpp/.cpp      framebuffers, command pool/buffers, sync, the frame loop
tools/
  capture_window.ps1        screenshots the running window and samples pixels
```

Ownership flows one way: `Renderer` owns `Swapchain` and `GraphicsPipeline`, both of
which only borrow the shared `VulkanContext`. Framebuffers live in `Renderer` because
they reference *both* a swapchain image and the render pass, which is why a resize
rebuilds them in the middle of the swapchain/pipeline recreation.

## Frame loop

Each iteration of `Renderer::drawFrame()`:

1. Waits on the fence of the current frame slot.
2. Acquires the next swapchain image with `vkAcquireNextImageKHR`.
3. Waits on the fence of the frame that last used that image, so the CPU never writes
   a command buffer that is still executing.
4. Records the command buffer: begin render pass, bind pipeline, push the rotation
   angle, set the dynamic viewport and scissor, `vkCmdDraw(3, 1, 0, 0)`, end render pass.
5. Submits, waiting on `imageAvailable` and signalling `renderFinished`.
6. Presents, and rebuilds the swapchain if the surface went out of date or suboptimal
   (`VK_ERROR_OUT_OF_DATE_KHR`, `VK_SUBOPTIMAL_KHR`) or the window was resized.

Viewport and scissor are dynamic state, so a resize never invalidates the pipeline.
The pipeline is only rebuilt if the swapchain image format actually changes.

## Next steps

- Vertex and index buffers with a staging-buffer upload path.
- Aspect-correct projection: the shader rotates in NDC, where x and y map to
  different pixel scales at 16:9, so the triangle stretches slightly as it spins.
  Fixing it means scaling x by the aspect ratio (and pushing that ratio in).
- Depth buffering (a depth attachment plus `VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL`).
- Uniform buffers and descriptor sets, then a per-frame camera.
- MSAA, and a fullscreen-triangle post-processing pass.
- Move to `VK_KHR_dynamic_rendering` to drop the render pass and framebuffers entirely.
