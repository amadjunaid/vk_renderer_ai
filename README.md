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
- Renders a `Mesh`: vertices and indices uploaded once into device-local buffers
  through a staging buffer, bound and drawn with `vkCmdDrawIndexed`. The demo mesh is
  a generated triangle whose data sits in `Renderer.cpp`; loading one from a file is a
  matter of filling the same two vectors.
- Draws the mesh through a `Material`, which for now is the shader pair the mesh is
  drawn with. No descriptor sets are involved yet.
- Renders with 2 frames in flight, using per-frame fences, per-image present semaphores
  and per-image command buffers.
- Rotates the mesh from a push constant, which the vertex shader turns into a 2D
  rotation matrix.
- Draws a Dear ImGui menu bar and toolbar into the same render pass as the mesh, so
  the UI needs no render pass, framebuffers or descriptor sets of its own. The toolbar
  drives the rotation speed at runtime.

## Requirements

| Dependency | Notes |
| --- | --- |
| CMake | 3.20 or newer |
| C++ compiler | MSVC 2022 (Visual Studio Build Tools are enough), GCC 11+, or Clang 14+ |
| [Vulkan SDK](https://vulkan.lunarg.com) | Provides the loader, validation layers and `glslc`. On macOS, MoltenVK is mandatory — Windows and Linux have native drivers. |
| Git | Used by CMake to fetch GLFW |

On Windows, one way to get everything:

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

### Debugging (VS Code, Windows)

`.vscode/launch.json` ships a `Debug (MSVC)` configuration: pressing F5 builds the
Debug configuration and launches it under `cppvsdbg`, with the renderer's console
output in the integrated terminal. `cppvsdbg` comes from the C/C++ extension
(`ms-vscode.cpptools`), which `.vscode/extensions.json` recommends; without it F5
fails with *"The debug type 'cppvsdbg' is not supported"*.

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
positions, which is handy after changing the mesh or the clear color:

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
CMakeLists.txt              build: fetches GLFW + Dear ImGui, finds Vulkan + glslc
shaders/
  triangle.vert             rotates the mesh's vertex positions/colors by a push constant
  triangle.frag             passes the interpolated color through
src/
  main.cpp                  creates the window, context and renderer, then runs it
  core/Window.hpp/.cpp      GLFW lifetime, resize flag, VkSurfaceKHR creation
  vk/VkCheck.hpp            VK_CHECK macro and VkResult -> string helper
  vk/VulkanContext.hpp/.cpp instance, debug messenger, surface, device, queues
  vk/Swapchain.hpp/.cpp     swapchain, image views, format/present-mode/extent selection
  vk/Material.hpp/.cpp      the shaders a mesh is drawn with
  vk/Mesh.hpp/.cpp          Vertex layout, mesh buffers, indexed draw
  vk/GraphicsPipeline.hpp/.cpp  render pass and the mesh pipeline (built from a Material)
  vk/DebugUi.hpp/.cpp       ImGui menu bar and toolbar, sharing the render pass
  vk/Renderer.hpp/.cpp      the demo mesh's data, framebuffers, sync, the frame loop
tools/
  capture_window.ps1        screenshots the running window and samples pixels
```

Ownership flows one way: `Renderer` owns a `Material`, a `Mesh` and a `Swapchain`, and
the `GraphicsPipeline` built from the material. Everything only borrows the shared
`VulkanContext`. The mesh and the pipeline only *reference* the material, so `Renderer`
declares it first and destroys it last. Framebuffers live in `Renderer` because they
reference *both* a swapchain image and the render pass, which is why a resize rebuilds
them in the middle of the swapchain/pipeline recreation.

`Mesh` owns the vertex and index buffers, so both the vertex layout (`Vertex`) and the
buffers come from one place - the pipeline just declares the layout it was given, which
is why `GraphicsPipeline` uses `Vertex` from `vk/Mesh.hpp`.

`Mesh` holds no geometry of its own: the demo triangle is the `kMeshVertices` /
`kMeshIndices` constant pair in `Renderer.cpp`, and the constructor uploads whatever it
is handed. Likewise `Material` knows nothing about the demo: Renderer names the shader
files (`kMaterial*`) and hands it the full paths.

## Frame loop

Each iteration of `Renderer::drawFrame()`:

1. Waits on the fence of the current frame slot.
2. Acquires the next swapchain image with `vkAcquireNextImageKHR`.
3. Waits on the fence of the frame that last used that image, so the CPU never writes
   a command buffer that is still executing.
4. Records the command buffer: begin render pass, bind pipeline, push the rotation
   angle, set the dynamic viewport and scissor, bind the mesh's vertex and index
   buffers and `vkCmdDrawIndexed` the whole mesh, record the ImGui draw data into
   that same pass, end render pass.
5. Submits, waiting on `imageAvailable` and signalling `renderFinished`.
6. Presents, and rebuilds the swapchain if the surface went out of date or suboptimal
   (`VK_ERROR_OUT_OF_DATE_KHR`, `VK_SUBOPTIMAL_KHR`) or the window was resized.

Viewport and scissor are dynamic state, so a resize never invalidates the pipeline.
The pipeline is only rebuilt if the swapchain image format actually changes.

## Debug UI

`DebugUi` wraps Dear ImGui (fetched by CMake and pinned to a release tag) and gives the
window a menu bar and a toolbar. The design choice that matters is that **ImGui shares the
render pass `GraphicsPipeline` already created** rather than owning one:

- The Vulkan backend records draw commands into whatever render pass is active. It only
  calls `vkCmdBeginRenderPass` from the `ImGui_ImplVulkanH_*` helpers, which this project
  does not use.
- So the UI needs no render pass, no framebuffers and no image views of its own. There is
  nothing to rebuild on a resize, and the mesh and the UI are one pass over one image.
- The render pass is handed over through
  `ImGui_ImplVulkan_InitInfo::PipelineInfoMain.RenderPass` (the older top-level
  `RenderPass` field was replaced in this release). When the swapchain format changes,
  `GraphicsPipeline` destroys and recreates the render pass, so `onSwapchainRecreated()`
  rebuilds the UI pipeline with `ImGui_ImplVulkan_CreateMainPipeline()` instead of
  shutting the backend down, which would discard the ImGui context and its textures.
- The backend creates and owns its own descriptor pool and command pool, so no Vulkan
  state is shared with `Renderer`.

Note that Dear ImGui 1.92 uploads its font atlas lazily through the renderer texture
system (`ImGuiBackendFlags_RendererHasTextures`), so there is no `CreateFontsTexture()`
call — that function no longer exists. Pass `false` as the last `Renderer` argument to
render the bare mesh with no UI at all.

## Next steps

- Loading a mesh from a file (OBJ or glTF) to replace the generated triangle in
  `Renderer.cpp`, plus the per-vertex normals and UVs that a real mesh needs.
- Aspect-correct projection: the shader rotates in NDC, where x and y map to
  different pixel scales at 16:9, so the mesh stretches slightly as it spins.
  Fixing it means scaling x by the aspect ratio (and pushing that ratio in).
- Depth buffering (a depth attachment plus `VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL`).
- Moving pipeline state and textures into `Material`, which is what turns it from a
  shader pair into a real material.
- Uniform buffers and descriptor sets, then a per-frame camera.
- MSAA, and a fullscreen-triangle post-processing pass.
- Move to `VK_KHR_dynamic_rendering` to drop the render pass and framebuffers entirely.
