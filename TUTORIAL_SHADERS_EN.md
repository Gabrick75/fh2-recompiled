# Tutorial — Android build and native shader library (English)

Step-by-step, from cloning the repository to running the game in native mode on an Android device
using the fallback without descriptor indexing (method A), without needing the Turnip driver.

## 1. Prerequisites (Linux PC)

```sh
sudo apt update
sudo apt install -y git clang-20 clang++-20 cmake ninja-build python3 openjdk-17-jdk unzip wget curl \
  libasound2-dev libdbus-1-dev libdecor-0-dev libegl1-mesa-dev libpipewire-0.3-dev libpulse-dev \
  libudev-dev libwayland-dev libx11-dev libx11-xcb-dev libxcursor-dev libxext-dev libxfixes-dev libxi-dev \
  libxkbcommon-dev libxrandr-dev libxss-dev libxtst-dev
```

(The `lib*` packages are the SDL3/X11 build dependencies; without them the SDK configure fails. `./build_pc.sh` installs any missing ones itself.)

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

The fallback code without descriptor indexing (method A) is already on branch `main`:
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

## 5. PC build: host SDK, codegen and app (generates `app/generated`)

A single command does everything (builds the SDK `rexglue` host if needed, runs codegen over your game and builds the app; slow the first time):

```sh
./build_pc.sh
```

This generates `app/generated/rexglue.cmake` and `app/generated/default/sources.cmake`, required for the Android build.

(The script uses `-march=x86-64-v3`, like the reference PC — the SDK needs SSSE3+. If your CPU is older than ~2013, run `MARCH=x86-64-v2 ./build_pc.sh`.)

Then dump the executable image (**required** for step 7; opens the game on PC — wait for it to boot, then close):

```sh
tools/rodar_pc.sh --xex_despejar_imagem=app/out/shaders_fh2_imagem
```

## 6. Shader library tools (automatic)

The `fh2_shaders.nfsp` library needs DXC (HLSL → SPIR-V) and spirv-val/opt. **Nothing manual:** `tools/xenos/biblioteca.sh` downloads the exact pinned versions itself (same on every PC):
- DXC `v1.9.2607` from GitHub (~490 MB, one-time download, with sha256 check);
- SPIR-V from NDK `29.0.14206865`, installed itself via `sdkmanager` (needs the Android SDK with cmdline-tools and Java).

If local versions differ, the script warns, downloads/installs the exact ones and continues. Do not install DXC or NDK manually for this.

## 7. Build the native shader library

```sh
tools/xenos/biblioteca.sh   # downloads the exact toolchain itself (DXC + NDK)
```

What happens:
1. Unpacks track `.fxobj` (LZX).
2. Extracts shader containers from the executable and tracks.
3. Translates to HLSL with XenosRecomp (`FH2_RECOMP`).
4. Compiles with DXC to SPIR-V Vulkan 1.2.
5. Packs into `app/out/shaders_fh2/fh2_shaders.nfsp`.

Expected output: `spirv ok=4074 falhou=4` and the `fh2_shaders.nfsp` file (~72 MB).

> **Important:** this library is built from your own game files. Do not share or commit it.

### D3D internal shaders (sueltos): complete library in two passes

The first library lacks the D3D internal shaders (the script warns `[aviso] sem .../shaders_fh2_sueltos`). It already opens the game — use it to dump what's missing, running the game in native mode (browse menus/play a little so the renderer meets the shaders; then close):

```sh
tools/rodar_pc.sh --nfsmw_renderizador=nativo --fh2_nativo_volcar_sueltos=$PWD/app/out/shaders_fh2_sueltos
```

Then rebuild for the complete library:

```sh
tools/xenos/biblioteca.sh
```

Reference values (`app/out/shaders_fh2/versoes.txt`): `contenedores=4142`, `hlsl=4078`, `spirv ok=4074 falhou=4`, `sueltos=42`, `nfsp_bytes=75497272`.

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
# 1. clone
git clone https://github.com/Gabrick75/fh2-recompiled.git
cd fh2-recompiled

# 2. thirdparty
python3 tools/fetch_thirdparty.py

# 3. extract game (your ISO)
python3 tools/fase1_extraer.py YourFH2.iso -o assets/game_root

# 4. PC build (host SDK + codegen + app)
./build_pc.sh

# 4b. executable image dump (required for library)
tools/rodar_pc.sh --xex_despejar_imagem=app/out/shaders_fh2_imagem

# 5. shader library (exact toolchain downloaded itself)
tools/xenos/biblioteca.sh

# 5b. D3D sueltos + rebuild for the complete library
tools/rodar_pc.sh --nfsmw_renderizador=nativo --fh2_nativo_volcar_sueltos=$PWD/app/out/shaders_fh2_sueltos
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
