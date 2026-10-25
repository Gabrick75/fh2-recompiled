# Third-party notices

FH2 Recompiled is built on the work of other open-source projects. Their licenses apply to their code and to
the parts of this repository derived from it. The rest of this repository is licensed under the GNU General
Public License v3.0 (see [LICENSE](LICENSE)).

## Projects this one is derived from

| Project | Authors | Used for | License |
|---|---|---|---|
| [ReXGlue SDK](https://github.com/rexglue/rexglue-sdk) v0.10.0 | Tom Clay, with portions from [Xenia](https://xenia.jp) by Ben Vanik and the Xenia contributors | `sdk/`: the PowerPC to C++ code generator, the Xbox 360 kernel and runtime, the Xenos GPU command processing and the Vulkan backend | BSD-3-Clause ([sdk/LICENSE](sdk/LICENSE)) |
| [NFSMW Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled) | madelrandel-blip | The recompilation project layout this one started from: application setup, code generation setup and tools | GPL-3.0 |
| [nfsmw-nx](https://github.com/StevensND/nfsmw-nx) | StevensND | Changes to the SDK and tools inherited through nfsmw-android; the native renderer (through nfsmw-android) and the shader translator's changes (`tools/xenos/recomp`, from commit `4e3ffc2`) | GPL-3.0 |
| [nfsmw-android](https://github.com/codepdbh/nfsmw-android) | codepdbh | The Android application shell (launcher, SDL3 activity, touch controls, diagnostics) this one was adapted from; the native renderer in `app/src/nativo` (from commit `a7e6e4c`) | GPL-3.0 |
| [NFS-CARBON-360-ANDROID-RECOMP](https://github.com/codepdbh/NFS-CARBON-360-ANDROID-RECOMP) | codepdbh | The way the native renderer is built for another game (`NFSC_RECOMP`, game hooks and links) followed by `FH2_RECOMP`, and its NFS Carbon translator changes | GPL-3.0 |
| [XenosRecomp](https://github.com/hedge-dev/XenosRecomp) | hedge-dev and contributors | The shader translator in `tools/xenos/recomp` | MIT ([tools/xenos/recomp/LICENSE.md](tools/xenos/recomp/LICENSE.md)) |
| [NFSMW-Recompiled-Mobile](https://github.com/victorgbd/NFSMW-Recompiled-Mobile) | victorgbd | Performance changes ported to this project: MSAA cap, futex wait for the command ring, FPS counter, launcher menu layout | GPL-3.0 |
| [ShiftGlue SDK](https://github.com/arcanite24/shiftglue-sdk), the ReXGlue SDK fork of [Pinyon Shift](https://github.com/arcanite24/pinyon-shift) | arcanite24 and the Pinyon Shift contributors | The HDR output of the presenter (`present_hdr` in `sdk/src/ui/vulkan/vulkan_presenter.cpp`, `sdk/src/ui/shaders/hdr_output.ps.glsl`) | BSD-3-Clause |

Changes made by this project to files under `sdk/` are distributed under the SDK's BSD-3-Clause license, so
they can be reused by other projects built on the ReXGlue SDK.

## Libraries

Libraries used by the SDK and the Android application. Except for the few files this project modified (listed
below), they are not stored in this repository: `tools/fetch_thirdparty.py` fetches them from the submodules of
ReXGlue SDK v0.10.0.

| Library | License |
|---|---|
| [SDL3](https://libsdl.org) | zlib |
| [FFmpeg](https://ffmpeg.org) (libavcodec, libavutil), built without GPL-only parts | LGPL-2.1-or-later |
| [libmspack](https://www.cabextract.org.uk/libmspack/) by Stuart Caie | LGPL-2.1 |
| [glslang](https://github.com/KhronosGroup/glslang) | BSD-3-Clause and others (see its license file) |
| [SPIRV-Tools](https://github.com/KhronosGroup/SPIRV-Tools) | Apache-2.0 |
| [SPIRV-Headers](https://github.com/KhronosGroup/SPIRV-Headers), [Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | MIT, Apache-2.0 |
| [Vulkan Memory Allocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | MIT |
| [fmt](https://github.com/fmtlib/fmt), [spdlog](https://github.com/gabime/spdlog) | MIT |
| [xxHash](https://github.com/Cyan4973/xxHash) | BSD-2-Clause |
| [SIMDe](https://github.com/simd-everywhere/simde) | MIT |
| [toml++](https://github.com/marzer/tomlplusplus) | MIT |
| [Dear ImGui](https://github.com/ocornut/imgui) | MIT |
| [o1heap](https://github.com/pavel-kirienko/o1heap) | MIT |
| [utfcpp](https://github.com/nemtrif/utfcpp) | BSL-1.0 |
| [CLI11](https://github.com/CLIUtils/CLI11) | BSD-3-Clause |
| [inja](https://github.com/pantor/inja) | MIT |
| [Tracy](https://github.com/wolfpld/tracy) | BSD-3-Clause |
| AES-128 by LuoPeng | MIT |
| [AndroidX](https://developer.android.com/jetpack/androidx) (core, documentfile, appcompat) | Apache-2.0 |
| [Material Components for Android](https://github.com/material-components/material-components-android) | Apache-2.0 |
| [libadrenotools](https://github.com/bylaws/libadrenotools) by Billy Laws, with linkernsbypass (custom Vulkan drivers on Android), stored whole in `sdk/thirdparty/libadrenotools` as in nfsmw-android: it is not part of ReXGlue SDK | BSD-2-Clause |

### FFmpeg and libmspack (LGPL)

This project uses these files of the SDK's third-party tree in a modified form; the modified copies are in
`sdk/thirdparty`:

- `FFmpeg/libavcodec/fft_template.c` and `FFmpeg/libavcodec/mdct_template.c`
- `FFmpeg/config.h`, `FFmpeg/config_nfsmw_wmv3.h` and `FFmpeg/config_switch_aarch64.h`: build configuration
  limited to the WMV3 video decoder
- `ffmpeg-overlay/codec_list.c`: registers the WMV3 decoder
- `CMakeLists.txt`: builds the libraries

The rest of FFmpeg and libmspack is the unmodified upstream source at the commits pinned by ReXGlue SDK v0.10.0.
Everything needed to rebuild and relink the application with a modified version of either library is in this
repository and its build instructions.
