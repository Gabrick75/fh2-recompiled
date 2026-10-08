#!/bin/bash
# Compiles this project's own GLSL shaders into SPIR-V headers next to the SDK's ("compilar" means "compile").
# Needs glslc (from the Android NDK, or the Vulkan SDK).
#
#   tools/compilar_shaders.sh
set -e
RAIZ=$(cd "$(dirname "$0")/.." && pwd)
GLSLC=${GLSLC:-$(ls "$HOME"/Android/Sdk/ndk/*/shader-tools/linux-x86_64/glslc 2>/dev/null | tail -1)}
[ -x "$GLSLC" ] || GLSLC=$(command -v glslc)
SHADERS=$RAIZ/sdk/src/graphics/shaders
for src in "$SHADERS"/atalho_conversao.cs.glsl; do
  name=$(basename "$src" .cs.glsl)_cs
  out=$SHADERS/vulkan_spirv/$name.h
  {
    echo "// Generated with tools/compilar_shaders.sh from $(basename "$src")."
    echo "const uint32_t $name[] = {"
    "$GLSLC" -fshader-stage=compute --target-env=vulkan1.1 -O -mfmt=num -o - "$src" | tr -d '\n ' |
        tr ',' '\n' | grep . | paste -d, - - - - - - - - | sed 's/,*$//; s/^/    /; s/$/,/'
    echo "};"
  } > "$out"
  echo "$out"
done
