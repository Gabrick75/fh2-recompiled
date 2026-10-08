# Native renderer

Copied from [codepdbh/nfsmw-android](https://github.com/codepdbh/nfsmw-android) `app/src` at commit `a7e6e4c`
(2026-10-08): the native renderer of NFSMW Android Evolved, built on the one of
[StevensND/nfsmw-nx](https://github.com/StevensND/nfsmw-nx). Both are GPL-3.0, like this project. File names are kept
so upstream changes can be compared.

Instead of emulating the Xbox 360 GPU, it reads the game's PM4 command ring, answers what the game expects from the
GPU, and draws with Vulkan: render targets are Vulkan images, resolves are copies, and the shaders come from a library
translated ahead of time (`tools/xenos`). NFS Carbon's port (codepdbh/NFS-CARBON-360-ANDROID-RECOMP, `NFSC_RECOMP`)
showed how to build it for another game; here every `NFSC_RECOMP` branch also applies to `FH2_RECOMP`, and FH2's own
values (addresses, shared constant layout) are in `FH2_RECOMP` branches. See `docs/renderizador_nativo.md`.
