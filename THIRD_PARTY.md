# Third party software

Kestral is licensed under the GNU Affero General Public License v3 or later (see `LICENSE`).
It is built on these projects. Thank you to everyone who made them.

| Project | Used for | License |
|---|---|---|
| [Open CASCADE Technology](https://dev.opencascade.org) 7.9.3 | Solid modeling kernel, STEP and STL | LGPL 2.1 with the Open CASCADE exception |
| [Dear ImGui](https://github.com/ocornut/imgui) 1.92.9 (its unchanged source is in `third_party/imgui`) | User interface | MIT |
| [GLFW](https://www.glfw.org) | Window and input | zlib/libpng |
| [GLM](https://github.com/g-truc/glm) | Math | MIT |
| [Vulkan headers and loader](https://github.com/KhronosGroup) | Graphics | Apache 2.0 |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | Graphics memory | MIT |
| [FreeType](https://freetype.org) (with libpng, bzip2, Brotli) | Font rendering | FreeType License |
| [OpenSSL](https://www.openssl.org) 3 | Secure connections (AI providers, printers) | Apache 2.0 |
| [zlib](https://zlib.net) | Compression (3MF, PNG) | zlib |
| [stb_image](https://github.com/nothings/stb) | Reading pictures | MIT or public domain |
| [NanoSVG](https://github.com/memononen/nanosvg) | Icons | zlib |
| [OrcaSlicer](https://github.com/SoftFever/OrcaSlicer) | Slicing engine (downloaded from OrcaSlicer's official releases, not included here). `src/print/OrcaOptions.inc` lists OrcaSlicer's setting keys, names, units, choices and limits so Kestral can drive its engine; the setting descriptions are Kestral's own (`tools/orca_setting_descriptions.json`) | AGPL 3.0 |

The Windows download also contains the libraries above as DLL files, unchanged from their
[vcpkg](https://github.com/microsoft/vcpkg) builds. Their source code is at the links above; the exact
versions are pinned in `vcpkg.json`.

Kestral does not include Bambu Lab's network plugin. When OrcaSlicer has installed it on your
computer, Kestral can use it to talk to Bambu Lab printers, the same way OrcaSlicer does.

Kestral is not affiliated with Autodesk, Bambu Lab or the OrcaSlicer project. Fusion 360 is a
trademark of Autodesk.
