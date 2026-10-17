# vk_renderer_ai

A minimal Vulkan renderer in C++20 that opens a window, imports a binary glTF
(`.glb`) mesh and draws it with a physically based material.

![status](https://img.shields.io/badge/status-glTF%20import-blue)

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
- Imports meshes from binary glTF files, with a hand-written reader (`glb` container plus a
  small JSON parser) rather than a dependency. Positions, normals and texture coordinates
  are read from any accessor layout glTF allows - interleaved or not, `uint8`/`uint16`/
  `uint32` indices, normalized integer attributes - node transforms are baked in during a
  walk of the scene graph, and normals are generated for primitives that lack them. The
  data is rotated into the renderer's world on the way in (see *Coordinate system*).
- Renders a `Mesh`: vertices and indices uploaded once into device-local buffers
  through a staging buffer, bound and drawn with `vkCmdDrawIndexed`.
- Draws the mesh through a `Material` - a shader pair plus the PBR factors of glTF's
  `pbrMetallicRoughness` model, which is what an imported file fills in.
- Shades with a basic Cook-Torrance PBR fragment shader (GGX, Smith, Schlick) lit by one
  directional light plus an ambient fill, honouring `baseColorFactor`, `metallicFactor`,
  `roughnessFactor` and `emissiveFactor`.
- Depth tests against a depth attachment, so a solid mesh hides its own far side.
- Passes the per-frame matrices, camera position and light through a uniform buffer and a
  descriptor set, and the per-material factors through a push constant.
- Renders with 2 frames in flight, using per-frame fences, per-image present semaphores
  and per-image command buffers.
- Spins the mesh and orbits a perspective camera, both editable at runtime.
- Draws a Dear ImGui menu bar, toolbar and model window into the same render pass as the
  mesh, so the UI needs no render pass, framebuffers or descriptor sets of its own. The
  model window shows what was imported and edits the material and camera live.
- Imports another mesh at run time from **File > Import > Mesh...**, which opens the
  platform's native file dialog.

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
  mesh.vert                 vertex transform, hands world position/normal to the fragment stage
  mesh.frag                 Cook-Torrance PBR shading from the material's glTF factors
data/
  cube.glb                  the model loaded at start-up
src/
  main.cpp                  creates the window, context and renderer, then runs it
  core/Math.hpp             Vec3/Mat4, the perspective and lookAt matrices
  core/Window.hpp/.cpp      GLFW lifetime, resize flag, VkSurfaceKHR, native handle
  core/FileDialog.hpp/.cpp  the native open-file dialog (Windows; nullopt elsewhere)
  io/Json.hpp/.cpp          a minimal JSON reader, for the glTF JSON chunk
  io/MeshLoader.hpp/.cpp    the .glb reader plus the built-in fallback cube
  vk/VkCheck.hpp            VK_CHECK macro and VkResult -> string helper
  vk/VulkanContext.hpp/.cpp instance, debug messenger, surface, device, queues
  vk/Swapchain.hpp/.cpp     swapchain, image views, format/present-mode/extent selection
  vk/Buffer.hpp/.cpp        buffer + memory creation and memory-type selection
  vk/Material.hpp/.cpp      the shaders a mesh is drawn with, plus its PBR factors
  vk/Mesh.hpp/.cpp          Vertex layout, mesh buffers, indexed draw
  vk/GraphicsPipeline.hpp/.cpp  render pass (color + depth), descriptor set layout,
                            pipeline layout and the mesh pipeline
  vk/SceneUniformBuffer.hpp/.cpp  the per-frame uniform buffer and its descriptor sets
  vk/DebugUi.hpp/.cpp       ImGui menu bar, toolbar, model window; shares the render pass
  vk/Renderer.hpp/.cpp      the mesh, depth attachment, framebuffers, sync, the frame loop
tools/
  capture_window.ps1        screenshots the running window and samples pixels
```

Ownership flows one way: `Renderer` owns a `Material`, a `Mesh`, a `Swapchain`, the
`SceneUniformBuffer` and the `GraphicsPipeline` built from the material. Everything only
borrows the shared `VulkanContext`. The mesh and the pipeline only *reference* the
material, so `Renderer` declares it first and destroys it last. The uniform buffer is
declared after the pipeline, because its descriptor sets are allocated from the
pipeline's descriptor set layout - and that is also the order the spec wants at
teardown: the descriptor sets are released before the layout they came from.
Framebuffers live in `Renderer` because they reference *both* a swapchain image and the
render pass, which is why a resize rebuilds them in the middle of the
swapchain/pipeline recreation. The depth attachment is rebuilt with them, since its size
has to follow the swapchain extent.

`Mesh` holds no geometry of its own. `io/MeshLoader.cpp` produces a `MeshData` from a
file - or a built-in cube when the file is missing - and `Renderer::applyMeshData` hands
its vectors to `Mesh`, its material to `Material` and its bounds to the camera framing.
Importing at run time is the same call with another path, so a file loaded from the menu
and the one loaded at start-up cannot drift apart.

`Mesh` owns the vertex and index buffers, so both the vertex layout (`Vertex`) and the
buffers come from one place - the pipeline just declares the layout it was given, which
is why `GraphicsPipeline` uses `Vertex` from `vk/Mesh.hpp`. `Vertex` is the three
attributes glTF calls POSITION, NORMAL and TEXCOORD_0, so a loader fills the struct
directly.

## Coordinate system

The renderer's world is right-handed with **+Z up and -X forward**, and everything follows
from that: the camera orbits the origin with +Z as its up vector, the toolbar's spin is a
rotation about +Z, the directional light comes mostly from +Z, and the ambient term in the
fragment shader weighs the normal's Z component.

glTF is +Y up, and its spec says the front of an asset faces +Z, so
`io/MeshLoader.cpp` rotates every imported file into the renderer's convention with
`worldConventionTransform()`:

| glTF | world |
| --- | --- |
| +Y (up) | +Z (up) |
| +Z (the front of the asset) | -X (forward) |
| +X | -Y |

The third row follows from the first two: a rotation that is right-handed can only send
+Z to -X one way. It is a rotation and not a reflection, so face winding is unchanged.

The conversion is prepended as the root of the scene-graph walk, so it flows through the
same matrix as the node transforms - the normal matrix and the bounds pick it up with
nothing extra to keep in sync. `MeshData` is always handed to the renderer in the world
convention, which makes adding a format that uses another convention (OBJ, FBX) a matter
of writing the same conversion, and is why `makeCubeMesh` is authored in it directly.

## Frame loop

Each iteration of `Renderer::drawFrame()`:

1. Waits on the fence of the current frame slot.
2. Acquires the next swapchain image with `vkAcquireNextImageKHR`.
3. Waits on the fence of the frame that last used that image, so the CPU never writes
   a command buffer that is still executing.
4. Writes this frame's `SceneUniforms` into the slice of the uniform buffer that belongs
   to the current frame slot.
5. Records the command buffer: begin render pass (clearing color and depth), bind the
   pipeline, bind the scene descriptor set, push the material constants, set the dynamic
   viewport and scissor, bind the mesh's vertex and index buffers and `vkCmdDrawIndexed`
   the whole mesh, record the ImGui draw data into that same pass, end render pass.
6. Submits, waiting on `imageAvailable` and signalling `renderFinished`.
7. Presents, and rebuilds the swapchain if the surface went out of date or suboptimal
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

The render pass the UI shares has two attachments (color and depth). The backend only
uses the color one; it is created against the render pass, so it stays compatible.

**Importing a mesh.** `File > Import > Mesh...` asks `core/FileDialog` for a path and
hands it to `Renderer::importMesh`, which runs inside the frame - the load is fast enough
for the one-frame hitch to be invisible. A failed import leaves the current mesh on
screen and reports the error in red in the model window, because losing the mesh you are
looking at is a worse outcome than the message. On Windows the dialog is
`GetOpenFileNameW` (hence the `comdlg32` link); elsewhere `filedialog::available()`
returns false and the model window shows a path box the same menu item reads instead.

## Next steps

- Textures: `baseColorTexture` and friends need image decoding, samplers and a
  descriptor set, which is also what turns `Material` from a shader pair into a real
  material.
- Tangents and a normal map, so a glTF file's `normalTexture` can be used.
- Alpha modes: `Material` carries no blending state, so `MASK` and `BLEND` materials
  render as opaque.
- Back-face culling driven by the material's `doubleSided` flag, which needs the winding
  order (Vulkan's flipped Y reverses it) settled first.
- Image based lighting to replace the constant ambient fill, and a shadow map for the
  directional light.
- A scene rather than a single mesh: several `Mesh` instances, several materials, and a
  per-object uniform (or push constant) instead of one model matrix.
- MSAA, and a fullscreen-triangle post-processing pass.
- Move to `VK_KHR_dynamic_rendering` to drop the render pass and framebuffers entirely.
