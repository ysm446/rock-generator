# Rock Generator

Windows desktop prototype for a node-based SDF rock generation tool.

## Current Prototype

- C++20 / CMake application
- Win32 + DirectX 12 renderer
- Dear ImGui shell UI
- imgui-node-editor backed node graph
- Internal `NodeGraph` model with nodes, pins, links, parameters, and evaluation status
- CPU dense SDF preview evaluator for primitive, noise, and crack parameters
- First-pass layout:
  - menu bar
  - node graph area
  - central viewport
  - properties panel
  - asset/export panel
- Animated cube placeholder in the viewport
- Editable prototype parameters for primitive, noise, and crack settings
- SDF preview stats for resolution, SDF range, fill ratio, and estimated volume
- Viewport overlay showing the evaluated SDF center slice
- Viewport point-cloud preview sampled near the SDF zero surface
- Debug wire preview generated from SDF sign-change cells
- Prototype triangle surface preview built from SDF sign-change quads
- Debug OBJ export to `exports/rock_debug.obj`
- Interactive viewport orbit, pan, zoom, and reset controls
- Node-stage preview: selecting graph nodes shows that generation stage in the viewport

OpenVDB, real SDF evaluation, mesh extraction, and export nodes are intentionally left for later phases.

## Build

Install dependencies with vcpkg, then configure with the vcpkg toolchain:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=C:/path/to/vcpkg/scripts/buildsystems/vcpkg.cmake
cmake --build build --config Debug
```

Run:

```powershell
./build/Debug/rock_generator.exe
```
