<div align="center">

# FH2 Recompiled

**An unofficial static recompilation of Forza Horizon 2 (Xbox 360) for Android and Linux**

[Português](README.pt-BR.md) · [Third-party notices](THIRD_PARTY_NOTICES.md) · [License: GPL-3.0](LICENSE)

</div>

> [!IMPORTANT]
> This is a non-commercial fan project for interoperability and preservation research. It is **not** affiliated
> with, endorsed by or sponsored by Microsoft, Xbox Game Studios, Playground Games, Turn 10 Studios or any other
> rights holder. **This repository contains no game files and no game code. The APKs in Releases contain no game
> data and do nothing without files extracted from your own legally obtained copy of the game.** See
> [Legal](#legal).

## Table of contents

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

## Overview

FH2 Recompiled translates the PowerPC code of the Xbox 360 executable of Forza Horizon 2 into C++ ahead of time
and compiles it into a native program for ARM64 Android phones and x86-64 Linux. It is built on the
[ReXGlue SDK](https://github.com/rexglue/rexglue-sdk), which provides the Xbox 360 kernel, the runtime and the
emulation of the console's Xenos GPU on Vulkan.

The Android application includes a launcher with graphics and performance settings, touch controls with an
editor, gamepad support and an in-game performance overlay.

## How it works

| Part | How it runs |
|---|---|
| Game code (CPU) | Recompiled ahead of time: the PowerPC instructions of `default.xex` become C++, built into a native library. There is no interpreter or JIT for the game code. |
| Xbox 360 kernel and system libraries | Reimplemented by the ReXGlue SDK (derived from the Xenia project). |
| GPU (Xenos) | Emulated: the GPU command stream is interpreted, the EDRAM is reproduced with Vulkan render targets and the shaders are translated to SPIR-V. |
| Audio, input, video | ReXGlue SDK, SDL3 and FFmpeg. |

When you build from source, the code generated from the game is produced on your own machine from your own copy
of the game; it is excluded from this repository by `.gitignore`. The APKs in Releases are built the same way and
contain that translated code, but none of the game's data.

## Project status

Work in progress. The game boots, plays its menus and races, but it is far from the original console's frame
rate on phones.

What this project adds on top of the SDK:

- An Android launcher: render resolution in percent, MSAA/FXAA, anisotropic filtering, present mode, thread
  pinning, and an overlay with frame rate, GPU, CPU and RAM use.
- Performance work on the emulated GPU path: the predicated tiling of the scene played in a single pass, render
  resolution below 1x, an MSAA cap, a futex wait for the command ring, thread affinity, adaptive vblank.
- Fixes: real memory barriers in the code generator on ARM, a busy-waiting SDL event loop on Android, pipeline
  cache reuse on Android.
- Measurement tools: GPU timestamps per kind of work, per-frame logs of resolves and texture loads, a
  sampling profiler and scripted test runs.

Known issues:

- Low frame rate on phones, limited by the GPU emulation (command processing on fast phones, resolves and
  texture reloads on slower ones).
- Collisions are sometimes missed on Android (the car passes through some objects); under investigation.
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
2. Copy the extracted game files to `Internal storage/jogos/ForzaHorizon2`, for example with
   `adb push assets/game_root /sdcard/jogos/ForzaHorizon2`, or pick the folder in the launcher.
3. Grant the file access the launcher asks for and tap **Jogar** (play).

Starting points for the settings: render resolution 50 % or lower, anti-aliasing off, FIFO present mode.

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
- **Releases.** The APKs published in Releases contain the game's executable code translated into native code by
  the recompiler, so that owners of the game can play the copy they own on their phones. They contain none of the
  game's data, assets, audio, video, artwork or logos, and they do nothing without the files the user extracts
  from their own copy. They are free of charge and are provided only for that purpose.
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
