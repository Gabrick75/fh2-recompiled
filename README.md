<div align="center">

# FH2 Recompiled · Mali edition

**An unofficial static recompilation of Forza Horizon 2 (Xbox 360) for Android, tuned for phones with Arm Mali GPUs**

A fork of [Gabrick75](https://github.com/Gabrick75)'s FH2 Recompiled. Everything below the
[Mali edition](#mali-edition) section is the upstream project's documentation and applies here too.

[Português](README.pt-BR.md) · [Third-party notices](THIRD_PARTY_NOTICES.md) · [License: GPL-3.0](LICENSE)

[![Discord](https://img.shields.io/badge/Discord-Join%20the%20server-5865F2?logo=discord&logoColor=white)](https://discord.gg/tpwfAyYy3Q)

</div>

> [!IMPORTANT]
> This is a non-commercial fan project for interoperability and preservation research. It is **not** affiliated
> with, endorsed by or sponsored by Microsoft, Xbox Game Studios, Playground Games, Turn 10 Studios or any other
> rights holder. **This repository contains no game files and no game code. The APKs in Releases contain no game
> data and do nothing without files extracted from your own legally obtained copy of the game.** See
> [Legal](#legal).

## Table of contents

- [Mali edition](#mali-edition)
  - [Why Mali needs its own work](#why-mali-needs-its-own-work)
  - [What this fork changes](#what-this-fork-changes)
  - [Tested device](#tested-device)
  - [Recommended settings on Mali](#recommended-settings-on-mali)
  - [Known issues on Mali](#known-issues-on-mali)
  - [Reporting a problem](#reporting-a-problem)
- [Overview](#overview)
- [How it works](#how-it-works)
- [Project status](#project-status)
- [Requirements](#requirements)
- [Building from source](#building-from-source)
- [Running on Android](#running-on-android)
- [Repository layout](#repository-layout)
- [Contributing](#contributing)
- [Credits](#credits)
- [Legal](#legal)
- [License](#license)

## Mali edition

The upstream project was developed and tuned on Adreno phones (S23 Ultra, Moto G9 Play), and its native renderer
comes from a Nintendo Switch port of NFSMW. Mali GPUs
(MediaTek Dimensity, Google Tensor G1 to G4, Exynos before Xclipse) work differently enough that the same build runs slower
and looks worse on them. This fork works on those differences, one measured problem at a time.

### Why Mali needs its own work

- **No BC texture compression.** Many Mali drivers, the tested one included, do not sample the DXT/BC formats
  every Xbox 360 game uses. Upstream decodes them on the CPU into uncompressed RGBA8: 8 times the memory and bandwidth of DXT1, 4 times that of DXT5.
- **The emulated renderer cannot start.** It needs the Vulkan feature `vertexPipelineStoresAndAtomics`, which the
  tested Mali driver does not expose. There, only the native renderer runs, and it is the one with image errors left.
- **Mali is a tiled GPU.** Every time a render pass is closed and reopened on the same target, the whole target goes
  out to memory and comes back. FH2 resolves and clears render targets dozens of times per frame, which a tiler pays
  for far more than the console's EDRAM did.
- **Occlusion queries are switched off on Arm GPUs** by upstream's compatibility table, after crashes reported on
  older Mali drivers. The game gets a substitute answer instead.

### What this fork changes

| Change | Effect |
|---|---|
| **BC1-5 → ETC2/EAC transcoding** (`app/src/nativo/nfsmw_texturas_etc.h`) | Textures stay compressed on Mali: BC1 → ETC2 RGB8A1, BC2/BC3 → ETC2 RGBA8, BC4 → EAC R11, BC5 → EAC RG11, each the same block size as the original, so 4 to 8 times less GPU memory and bandwidth than RGBA8. A fast draft is uploaded at once and a background thread replaces it with a full quality encode. Checked against an independent decoder by `tools/probar_etc.py`. Switch: `--nfsmw_nativo_texturas_etc2=false` brings back RGBA8. |
| **Per-target pass report** | With *GPU measurements* on, the log gets a `C6 pases por destino` line every ~10 s naming the render targets that cost the most bandwidth and how often each is reopened. |
| **Shadow cascades in the GPU report** | FH2's sun shadow cascades (depth-only, pitch 1040) are now reported as shadows instead of showing `sombras 0.00 ms`. |
| **Steering pads** | Two ◀ ▶ pads replace the left stick on the touch controls, as in SpeedBreaker: full lock while held, slide between them without lifting. The stick stays available in the control editor. |
| **Extra game options** | A field in the launcher's advanced settings to pass game options (`--name=value`) for testing, without a new build. |

### Tested device

| | |
|---|---|
| Phone | vivo V2514 |
| SoC | MediaTek Dimensity 9500 |
| GPU | Arm Mali-G1 Ultra MC12, Vulkan 1.3 |
| Android | 17 |

The driver reports no BC formats, so every game texture goes through the ETC2/EAC path. Reports from other Mali
phones are welcome.

### Recommended settings on Mali

- **Renderer: native** (the emulated one cannot start on Mali).
- **FPS limit: 30**, the Xbox 360's rate. Phones in this class thermal-throttle after a few minutes (on the tested
  phone the big core drops from 3.5 to 2.6 GHz) and the frame rate halves; a steady 30 holds.
- **Anti-aliasing: FXAA** and **anisotropic filtering 4x or higher**. Without them edges shimmer and the road goes
  blurry at an angle. Reinstalling a build signed with a different key resets these.

### Known issues on Mali

- **Flat, grey lighting beyond a wedge near the car.** The sun's shadow cascades are drawn about once per frame
  instead of four times, so outside the nearest cascade everything reads as in shadow. Being investigated.
- **Physics glitches**: cars pop, jump or teleport. This comes from FH2's recompiled collision code (upstream patches
  some crashes in `app/src/fh2_correcoes.cpp` and notes the same crash on PC), not from the GPU. Open.
- **Frame drops in busy scenes**, mostly from render pass splits on the main scene target, which the per-target
  report measures. Open.

### Reporting a problem

1. In the launcher, turn on **GPU measurements**, and play until the problem shows.
2. Send the two logs from `Internal storage/jogos/fh2-logs/` (`fh2_<date>.log` and `fh2_<date>_uso.log`), your
   phone model and, for image problems, a screenshot.

Builds from this fork are signed with a debug key, so they do not install over builds signed with another key.
Before switching, use **Export save** in the launcher, then uninstall, install and **Import**. Automatic backups
also stay in `Internal storage/jogos/ForzaHorizon2-saves/`.

## Overview

FH2 Recompiled translates the PowerPC code of the Xbox 360 executable of Forza Horizon 2 into C++ ahead of time
and compiles it into a native program for ARM64 Android phones and x86-64 Linux. It is built on the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), which provides the Xbox 360 kernel, the runtime and the
emulation of the console's Xenos GPU on Vulkan. An experimental native renderer, which draws the game's GPU
commands straight in Vulkan with translated shaders, is also available.

The Android application includes a launcher with graphics and performance settings, the game read straight from
its disc image (.iso), custom Vulkan drivers, an achievements page, touch controls with an editor, gamepad support
and an in-game performance overlay.

## How it works

| Part | How it runs |
|---|---|
| Game code (CPU) | Recompiled ahead of time: the PowerPC instructions of `default.xex` become C++, built into a native library. There is no interpreter or JIT for the game code. |
| Xbox 360 kernel and system libraries | Reimplemented by the ReXGlue SDK (derived from the Xenia project). |
| GPU (Xenos), emulated | The default. The GPU command stream is interpreted, the EDRAM is reproduced with Vulkan render targets and the shaders are translated to SPIR-V while the game runs. |
| GPU, native (experimental) | Adapted from [nfsmw-nx](https://github.com/StevensND/nfsmw-nx): reads the same command stream and draws it straight in Vulkan with shaders translated ahead of time (XenosRecomp) into a library, `fh2_shaders.nfsp`, built from your own game files. Much faster, with some image errors left. |
| Audio, input, video | ReXGlue SDK, SDL3 and FFmpeg. |

When you build from source, the code generated from the game is produced on your own machine from your own copy
of the game; it is excluded from this repository by `.gitignore`.

## Project status

Work in progress. The game boots, plays its menus and races, but it is far from the original console's frame
rate on phones.

What this project adds on top of the SDK:

- An Android launcher: emulated or native renderer, render resolution in percent, MSAA/FXAA, anisotropic
  filtering, shadow quality (native), world detail (grass, objects, reflections, shadow distance and crowd from
  0.25x to 4x of the Xbox 360's, all but objects also off; grass, and shadows and crowd off, with the native
  renderer), FPS limit (30 to unlimited), present mode, thread pinning, game and app
  language, an overlay with frame rate, GPU, CPU and RAM use, save import/export and backups.
- A keyboard and a mouse as a controller, found on their own like a gamepad: the game reads them as an Xbox 360
  pad, with a racing layout, the mouse turning the camera and every key rebindable in game.
- The game played straight from its disc image (.iso): no extraction needed.
- Custom Vulkan drivers (Turnip, Qualcomm packages) loaded through libadrenotools, with a Vulkan test before use.
- An achievements page that lists the game's achievements, unlocked ones in color with their date, and a small
  unlock notification in game.
- The native renderer for FH2: grass and foliage, particles, the headlights at night and the sun's shadow cascades.
- Performance work on the emulated GPU path: the predicated tiling of the scene played in a single pass, render
  resolution below 1x, an MSAA cap, a futex wait for the command ring, thread affinity, adaptive vblank.
- Fixes: real memory barriers in the code generator on ARM, a busy-waiting SDL event loop on Android, pipeline
  cache reuse on Android.
- Measurement tools: GPU timestamps per kind of work, per-frame logs of resolves and texture loads, a
  sampling profiler and scripted test runs.

Known issues:

- Low frame rate on phones, limited by the GPU emulation (command processing on fast phones, resolves and
  texture reloads on slower ones).
- Cars collide with the world and with each other, but collision is not perfect yet: on Android the car still
  passes through some objects now and then; under investigation.
- In the paint shop, the car picture on the Designs card is stretched.
- The native renderer still has image errors in some places; the emulated renderer is the faithful one.
- Some menu options are not implemented yet (they are marked as such in the launcher).

## Requirements

**To play on Android**

- Android 8.0 or later, ARM64 processor.
- A GPU with Vulkan 1.1 or later.
- 4 GB of RAM or more (6 GB or more recommended).
- About 8 GB of free storage for the game files.
- Your own copy of **Forza Horizon 2 for Xbox 360**, USA edition (Title ID `4D530AA4`), dumped from your own
  disc.

**To build**

- Linux x86-64 (the build was developed on Ubuntu/Pop!_OS), Clang 20, CMake 3.25 or later, Ninja, Python 3.8 or
  later, Git.
- For Android: Java 17 or later, the Android SDK with NDK 28.2.13676358 and CMake 3.30.5.
- About 20 GB of free disk space and 12 GB of RAM or more (the generated code is large).

## Building from source

1. **Fetch the third-party sources** of the SDK (they are not stored in this repository):

   ```sh
   python3 tools/fetch_thirdparty.py
   ```

2. **Extract your own disc image** into `assets/game_root` (the folder is ignored by Git):

   ```sh
   python3 tools/fase1_extraer.py /path/to/your/ForzaHorizon2.iso -o assets/game_root
   ```

3. **Build for Linux.** The first build runs the code generator on `assets/game_root/default.xex` and writes the
   C++ into `app/generated` (ignored by Git); it takes a while.

   ```sh
   cmake -S app -B app/out/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
         -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DREXSDK_DIR="$PWD/sdk"
   cmake --build app/out/linux
   tools/rodar_pc.sh
   ```

   **Regenerate the code after pulling changes to `app/overrides.toml`** (or any other `app/*.toml`). The
   generated code is not in Git. This Linux build regenerates it when those files change, but the Android build
   (step 4) cannot run the code generator, so if you only build the APK a fix made there does not reach it. For
   example, the paint shop crash fix only declares the guest `setjmp` and `longjmp` in `app/overrides.toml`;
   without regenerating, the game still closes when entering the paint shop. Run this step's build again, or the
   generator alone:

   ```sh
   REXGLUE=$PWD/sdk/out/linux-amd64/rexglue tools/codegen.sh app
   ```

4. **Build the Android APK** (after step 3, which produced the generated code):

   ```sh
   ./build_android.sh
   ```

   The APK is written to `android/app/build/outputs/apk/release/app-release.apk`. It is signed with your release
   key if `~/.android-keys/fh2-recompiled/keystore.properties` (or the file named by `FH2_KEYSTORE_PROPERTIES`)
   exists, with `storeFile`, `storePassword`, `keyAlias` and `keyPassword`; otherwise with the debug key.

## Running on Android

1. Download the APK from [Releases](../../releases) and install it, or install your own build with
   `adb install -r android/app/build/outputs/apk/release/app-release.apk`. Builds signed with different keys
   (Releases and your own) cannot update each other: uninstall one before installing the other.
2. Give the launcher the game: either choose your disc image (.iso) in **Game and saves**, which is read in place,
   or copy the extracted game files to `Internal storage/jogos/ForzaHorizon2`, for example with
   `adb push assets/game_root /sdcard/jogos/ForzaHorizon2`.
3. Grant the file access the launcher asks for and tap **Play**.

Starting points for the settings: render resolution 50 % or lower, anti-aliasing off, FIFO present mode.

**Native renderer.** It needs the translated shader library, which is made from the game's own files and is
therefore not in the APK. Build it on the PC with `tools/xenos/biblioteca.sh` (after step 2 of
[Building from source](#building-from-source)) and copy `app/out/shaders_fh2/fh2_shaders.nfsp` next to the game
files on the phone (or next to the .iso), for example with `tools/enviar_biblioteca_android.sh`. Then choose
**Native** in **Graphics**.

**GPU drivers.** **Performance > GPU driver** imports an adrenotools driver package (.zip) and tests it with
Vulkan before it can be chosen. On Adreno 6xx and 7xx a Mesa Turnip build is often faster than the phone's own
driver.

**Keyboard and mouse.** A Bluetooth or USB keyboard and mouse are found on their own, the way a gamepad is:
there is nothing to turn on. The game reads them as an Xbox 360 pad, together with any pad that is connected,
in a racing layout: A/D steer, W throttle, S brake, Space handbrake (A), the mouse and the arrows turn the
camera, IJKL is the d-pad, Enter is Start, Esc and Tab are Back, C is B, X is X and Z is Y. The mouse takes the
pointer the first time it moves. Any key can be changed in game with **F4** (the game's own settings window,
category *Input/Keybinds/Controller*). The on-screen pad gets out of the way when a keyboard or a mouse is
connected or used, and comes back when they are gone or on the first touch.

## Tutorials

- [Tutorial — Build do Android e biblioteca de shaders (português)](TUTORIAL_SHADERS_PT-BR.md): clone, codegen, biblioteca `fh2_shaders.nfsp`, APK, instalação no Moto G9 Play (Adreno 610) e configuração do renderizador nativo com fallback sem descriptor indexing (método A), sem Turnip.
- [Tutorial — Android build and native shader library (English)](TUTORIAL_SHADERS_EN.md): same guide in English.

## Repository layout

| Path | Contents |
|---|---|
| `app/` | The game's application: hooks and fixes (`app/src`), code generation manifest and configuration |
| `android/` | The Android application: launcher, game activity, touch controls, native glue |
| `sdk/` | The ReXGlue SDK with this project's changes |
| `tools/` | Extraction, measurement, profiling and test scripts |
| `docs/` | Notes about the game's disc and formats |

## Contributing

Bug reports and pull requests are welcome. Please:

- never attach or link game files, dumps, generated code, or builds other than the official Releases;
- do not ask for game files or builds: such issues are closed;
- describe your device (model, GPU, Android version) and attach the diagnostics file from the launcher when
  reporting a problem.

## Credits

This project exists thanks to the work of:

- **[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk)** by Tom Clay: the recompiler and runtime everything
  here is built on.
- **[Xenia](https://xenia.jp)** by Ben Vanik and the Xenia contributors: the Xbox 360 research the SDK's kernel
  and GPU emulation derive from.
- **[NFSMW Recompiled](https://github.com/madelrandel-blip/NFSMW-Recompiled)** by madelrandel-blip: the
  recompilation project this one started from.
- **[nfsmw-nx](https://github.com/StevensND/nfsmw-nx)** by StevensND and
  **[nfsmw-android](https://github.com/codepdbh/nfsmw-android)** by codepdbh: the ports whose SDK changes and
  Android shell this project was adapted from.
- **[NFSMW-Recompiled-Mobile](https://github.com/victorgbd/NFSMW-Recompiled-Mobile)** by victorgbd: performance
  changes for phones ported to this project.
- The authors of the libraries listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

FH2 Recompiled is maintained by [Gabrick75](https://github.com/Gabrick75).

## Legal

- **No game content in the repository.** This repository contains only original source code and open-source
  code under the licenses listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md). It does not contain, and
  will never contain, the game's executable, data files, assets, audio, video, artwork, logos or the code
  generated from the game.
- **Releases.** The APKs published in Releases contain none of the game's data, assets, audio, video, artwork
  or logos, and they do nothing without the files the user extracts from their own copy. They are free of
  charge and are provided only for owners of the game.
- **Own copy required.** You must own a legitimate copy of the game and use your own dump of it. This project
  does not provide, and does not explain how to obtain, copies of the game: it works only from files the user
  extracts from a disc they own.
- **No Microsoft code.** This project does not include code from the Xbox development kit or any other
  proprietary Microsoft software.
- **Trademarks.** Forza, Forza Horizon, Xbox and Xbox 360 are trademarks of the Microsoft group of companies.
  All other trademarks belong to their respective owners. They are used here only to identify the game and
  platform this software is compatible with; no endorsement is implied.
- **Not for commercial use or piracy.** Do not sell this project or builds of it, and do not use it to
  distribute or play unauthorized copies of the game.
- **Takedown requests.** If you are a rights holder and believe something in this repository or its Releases
  infringes your rights, please open an issue or contact the maintainer, and it will be reviewed and removed
  promptly.
- **No warranty.** This software is provided "as is", without warranty of any kind, as stated in sections 15
  and 16 of the GPL-3.0. Use it at your own risk.

## License

FH2 Recompiled is free software, licensed under the
[GNU General Public License v3.0](LICENSE). The ReXGlue SDK in `sdk/`, and this project's changes to it, are
licensed under the [BSD-3-Clause license](sdk/LICENSE). Third-party components keep their own licenses, listed in
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
