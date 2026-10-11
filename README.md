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
- Runs on Windows, Linux and macOS. On macOS that means MoltenVK, so the renderer enables
  `VK_KHR_portability_enumeration` (with its create flag) and `VK_KHR_portability_subset`
  wherever the loader and device advertise them. Both are conditional, so one code path
  works on every platform.
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
| [Vulkan SDK](https://vulkan.lunarg.com) | Provides the loader, validation layers and `glslc`. On macOS, MoltenVK is mandatory — Windows and Linux have native drivers. |
| Git | Used by CMake to fetch GLFW |

GLFW 3.4 is downloaded automatically by CMake, so it does not need to be installed.

**Windows** — one way to get everything:

```powershell
winget install --id Kitware.CMake -e
winget install --id Microsoft.VisualStudio.2022.BuildTools -e --override "--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended"
winget install --id KhronosGroup.VulkanSDK -e
```

If the SDK is missing, CMake falls back to the Khronos headers plus the `vulkan-1` loader
that ships with the GPU driver. That builds fine but cannot enable validation.

**Linux** — the loader, headers, validation layers and `glslc` all come from distro
packages (exact names vary a little between distributions):

```bash
# Debian / Ubuntu
sudo apt install build-essential cmake git ninja-build \
    libvulkan-dev vulkan-validationlayers \
    libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev
```

The `libx*` packages are GLFW's X11 backend. For Wayland instead, use
`libwayland-dev libxkbcommon-dev` — GLFW picks the backend at configure time.

For SPIR-V compilation you need `glslc` (from the Vulkan SDK, or your distribution's
`glslc`/`shaderc` package) or `glslangValidator` (from `glslang-tools`). The build accepts
either.

**macOS** — macOS has no Vulkan driver of its own, so MoltenVK translates to Metal.
Install the LunarG macOS SDK (it bundles MoltenVK, the loader and `glslc`) and point
`VULKAN_SDK` at it:

```bash
export VULKAN_SDK="$HOME/VulkanSDK/1.4.363.0"   # match the version you installed
export PATH="$VULKAN_SDK/bin:$PATH"
```

Homebrew's `molten-vk` also works, but `find_package(Vulkan)` will not discover it on its
own, so pass `-DVulkan_INCLUDE_DIR=... -DVulkan_LIBRARY=...` explicitly.

## Build and run

The renderer looks for its SPIR-V next to the build directory, so it can be launched
from anywhere.

### Windows / MSVC

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config RelWithDebInfo
.\build\bin\vk_triangle.exe
```

### Linux / macOS (Ninja)

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build
./build/bin/vk_triangle
```

On macOS, launch it from a terminal. The binary is not inside a `.app` bundle, so the
window opens without focus — click it once.

If CMake cannot find a shader compiler, pass one explicitly:

```powershell
cmake -S . -B build -DGLSL_COMPILER="C:/VulkanSDK/1.4.363.0/Bin/glslc.exe"
```

### Verifying the output

`tools/capture_window.ps1` screenshots the running window and samples a few known pixel
positions, which is handy after changing the triangle or the clear color:

```powershell
powershell -ExecutionPolicy Bypass -File tools/capture_window.ps1 -Out build/verify.png
```

It is Windows-only (it uses `PrintWindow` via P/Invoke). On Linux use
`import -window root out.png` (ImageMagick) or `grim` under Wayland; on macOS use
`screencapture -x out.png`.

## Platform notes

**Shader compiler.** The build looks for `glslc` first and falls back to
`glslangValidator` (invoked with `-V`), since some Linux distributions package only the
latter. Both produce the same SPIR-V; `glslc` ships with the Vulkan SDK, `glslangValidator`
comes from `glslang-tools`.

**Validation layers** are optional on every platform. If `VK_LAYER_KHRONOS_validation` is
missing, the renderer prints a warning and continues without it.

**macOS** needs the portability extensions described above; this is handled automatically,
but it is why the code checks extension availability before enabling anything.

**`.vscode/launch.json`** has one configuration per platform — pick the one matching your
OS (Windows uses `cppvsdbg`, macOS/Linux use `cppdbg` with lldb/gdb). The build task is
shared because `--config` is ignored by single-config generators.

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
