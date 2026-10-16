# Tutorial — Android build and native shader library (English)

Step-by-step, from cloning the repository to running the game in native mode on an Android device
using the fallback without descriptor indexing (method A), without needing the Turnip driver.

## 1. Prerequisites (Linux PC)

```sh
sudo apt update
sudo apt install -y git clang-20 clang++-20 cmake ninja-build python3 openjdk-17-jdk unzip wget curl
```

Verify:

```sh
clang-20 --version   # >= 20
cmake --version      # >= 3.25 (PC) / 3.30.5 (Android)
ninja --version
java -version        # 17+
```

## 2. Clone the repository

```sh
git clone https://github.com/Gabrick75/fh2-recompiled.git
cd fh2-recompiled
```

Switch to the branch with method A (fallback without descriptor indexing):

```sh
git fetch origin
git checkout -b compat/nativo-sem-indexing origin/compat/nativo-sem-indexing 2>/dev/null ||
git checkout -b compat/nativo-sem-indexing
```

If there is no remote/branch, apply the method A commits manually to your local history. The fallback code is in:
- `app/src/nativo/nfsmw_nativo_dibujos.cpp`
- `app/src/nativo/nfsmw_spirv_descriptores.h`
- `android/app/src/main/cpp/android_diagnostico.cpp`

## 3. Download third-party sources (ReXGlue SDK)

```sh
python3 tools/fetch_thirdparty.py
```

Downloads the ReXGlue SDK and dependencies into `sdk/thirdparty`. Takes a few minutes.

## 4. Extract your own copy of the game

You need your own legal ISO of Forza Horizon 2 (Xbox 360, USA, Title ID `4D530AA4`). The repository does not contain game files.

```sh
python3 tools/fase1_extraer.py /path/to/YourForzaHorizon2.iso -o assets/game_root
```

The `assets/game_root/` folder is ignored by Git and must never be shared.

## 5. Run codegen on PC (generates `app/generated`)

```sh
cmake -S app -B app/out/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 \
      -DREXSDK_DIR="$PWD/sdk"
cmake --build app/out/linux
```

This generates `app/generated/default/sources.cmake`, required for the Android build.

## 6. Install tools to build the native shader library

The `fh2_shaders.nfsp` library needs DXC and spirv-val/opt.

**DXC** (HLSL → SPIR-V):

```sh
mkdir -p ~/.local/share/fh2-tools
# Download a DXC release (https://github.com/microsoft/DirectXShaderCompiler)
# and place it at: ~/.local/share/fh2-tools/dxc-<version>/bin/dxc
```

**spirv-val / spirv-opt** (already in the Android NDK):

```sh
ls $HOME/Android/Sdk/ndk/*/shader-tools/linux-x86_64/
# should contain spirv-val and spirv-opt
```

## 7. Build the native shader library

```sh
export DXC=~/.local/share/fh2-tools/dxc-*/bin/dxc   # adjust to the real path
tools/xenos/biblioteca.sh
```

What happens:
1. Unpacks track `.fxobj` (LZX).
2. Extracts shader containers from the executable and tracks.
3. Translates to HLSL with XenosRecomp (`FH2_RECOMP`).
4. Compiles with DXC to SPIR-V Vulkan 1.2.
5. Packs into `app/out/shaders_fh2/fh2_shaders.nfsp`.

Expected output: something like `spirv ok=2559 falhou=0` and the `fh2_shaders.nfsp` file (~72 MB).

> **Important:** this library is built from your own game files. Do not share or commit it.

## 8. Build the Android APK

```sh
export ANDROID_HOME="$HOME/Android/Sdk"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
./build_android.sh
```

The APK is at:

```
android/app/build/outputs/apk/release/app-release.apk
```

## 9. Install on Android device via ADB

```sh
adb devices -l
# confirm your device serial appears in the list

adb -s <YOUR_SERIAL> install -r android/app/build/outputs/apk/release/app-release.apk
```

## 10. Prepare game files on the device

Copy the extracted folder or ISO to device storage:

```sh
# option A: extracted folder
adb -s <YOUR_SERIAL> push assets/game_root /sdcard/jogos/ForzaHorizon2

# option B: ISO (to read directly from the image)
adb -s <YOUR_SERIAL> push /path/to/YourForzaHorizon2.iso /sdcard/jogos/
```

And the shader library:

```sh
adb -s <YOUR_SERIAL> push app/out/shaders_fh2/fh2_shaders.nfsp /sdcard/jogos/ForzaHorizon2/
```

The library must be next to the game files (or next to the ISO), because the native renderer loads it from `fh2_nativo_biblioteca_dir`.

## 11. Configure the launcher on the device

Open the app on the Android device and set:

| Setting | Value |
|---------|-------|
| Renderer | **Native** |
| Resolution | 720p or lower (50% or less recommended on Adreno GPUs) |
| Anti-aliasing | Off |
| GPU driver | System (empty) — Turnip not needed for the fallback |
| FPS | 30 |

Make sure these arguments are active (via menu or options file):

```
--nfsmw_renderizador=nativo
--fh2_nativo_sin_indexing=true     # FORCE the fallback without descriptor indexing
```

`sin_indexing` auto-enables when `runtimeDescriptorArray` is missing (case of stock drivers on some Adreno GPUs), but forcing `true` guarantees method A even if detection changes.

## 12. Start the game

Tap **Play**. The native renderer should initialize without Turnip.

If the screen stays black, generate a diagnostic and check the log:

```sh
# in the launcher, About > Diagnostics > create report
adb -s <YOUR_SERIAL> pull /sdcard/jogos/fh2-logs/fh2_$(date +%Y%m%d)_*.log .
```

Look for:
- `[nativo] C6: sin descriptor indexing: metodo A (heaps com tamano fijo)` — fallback active.
- `spirv ok=` — library loaded.
- Absence of `el dispositivo Vulkan no tiene runtimeDescriptorArray: no se dibuja`.

## Summary commands

```sh
# 1. clone + branch
git clone https://github.com/Gabrick75/fh2-recompiled.git
cd fh2-recompiled
git checkout -b compat/nativo-sem-indexing

# 2. thirdparty
python3 tools/fetch_thirdparty.py

# 3. extract game (your ISO)
python3 tools/fase1_extraer.py YourFH2.iso -o assets/game_root

# 4. PC codegen
cmake -S app -B app/out/linux -G Ninja -DCMAKE_BUILD_TYPE=Release \
      -DCMAKE_C_COMPILER=clang-20 -DCMAKE_CXX_COMPILER=clang++-20 -DREXSDK_DIR="$PWD/sdk"
cmake --build app/out/linux

# 5. shader library
export DXC=~/.local/share/fh2-tools/dxc-*/bin/dxc
tools/xenos/biblioteca.sh

# 6. build APK
export ANDROID_HOME="$HOME/Android/Sdk"
./build_android.sh

# 7. install on Android device
adb -s <YOUR_SERIAL> install -r android/app/build/outputs/apk/release/app-release.apk

# 8. push data and shader
adb -s <YOUR_SERIAL> push assets/game_root /sdcard/jogos/ForzaHorizon2
adb -s <YOUR_SERIAL> push app/out/shaders_fh2/fh2_shaders.nfsp /sdcard/jogos/ForzaHorizon2/
```

Then set **Native** in the launcher and play.
