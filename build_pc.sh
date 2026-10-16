#!/bin/bash
# Build completo do PC (passo 5 do tutorial) sem nada manual: SDK host (rexglue) ->
# codegen (app/generated) -> cmake configure -> cmake build.
# Idempotente: o ninja pula o que já está feito; o codegen roda sempre.
#
#   ./build_pc.sh
#
# Precisa antes: passo 4 do tutorial (assets/game_root com default.xex extraído
# do seu próprio ISO). O resto (thirdparty, binário rexglue, generated) é feito aqui.
set -euo pipefail

RAIZ=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
CC_BIN=${CC:-clang-20}
CXX_BIN=${CXX:-clang++-20}
PYBIN=${PYTHON:-python3}
# x86-64-v3 = baseline do PC de referência (o SDK usa intrínsecos SSSE3+;
# sem -march o build falha). Troque se a CPU for anterior a ~2013: MARCH=x86-64-v2.
MARCHA=${MARCH:-x86-64-v3}

# ---- 0. jogo extraído (não há como baixar: é do ISO do jogador) ----
if [ ! -f "$RAIZ/assets/game_root/default.xex" ]; then
  echo "ERRO: assets/game_root/default.xex não encontrado." >&2
  echo "      Extraia seu próprio ISO primeiro (passo 4 do tutorial):" >&2
  echo "      python3 tools/fase1_extraer.py /caminho/para/SeuForzaHorizon2.iso -o assets/game_root" >&2
  exit 1
fi

# ---- 1. fontes de terceiros do SDK (passo 3; pula se já feito) ----
if [ ! -f "$RAIZ/sdk/thirdparty/CMakeLists.txt" ]; then
  echo "[info] baixando fontes de terceiros do SDK..."
  "$PYBIN" "$RAIZ/tools/fetch_thirdparty.py" || exit 1
else
  echo "[info] thirdparty já presente; pulando download."
fi

# ---- 2. dependências de sistema do SDL3/X11 (falha fatal sem elas; instala sozinho) ----
DEPS_SDL="libasound2-dev libdbus-1-dev libdecor-0-dev libegl1-mesa-dev libpipewire-0.3-dev libpulse-dev libudev-dev libwayland-dev libx11-dev libx11-xcb-dev libxcursor-dev libxext-dev libxfixes-dev libxi-dev libxkbcommon-dev libxrandr-dev libxss-dev libxtst-dev"
FALTAM=""
[ -f /usr/include/X11/extensions/XTest.h ] || FALTAM=1
pkg-config --exists x11-xcb 2>/dev/null || FALTAM=1
pkg-config --exists wayland-client 2>/dev/null || FALTAM=1
if [ -n "$FALTAM" ]; then
  echo "[info] instalando dependências de build do SDL3/X11..."
  if command -v apt-get >/dev/null 2>&1 && command -v sudo >/dev/null 2>&1; then
    sudo apt-get update && sudo apt-get install -y $DEPS_SDL || {
      echo "ERRO: falha ao instalar dependências; rode manualmente:" >&2
      echo "      sudo apt-get install -y $DEPS_SDL" >&2
      exit 1
    }
  else
    echo "ERRO: cabeçalhos X11 ausentes e sem apt-get/sudo automático." >&2
    echo "      No Ubuntu/Pop!_OS, rode: sudo apt-get install -y $DEPS_SDL" >&2
    echo "      (lista completa: https://wiki.libsdl.org/SDL3/README-linux)" >&2
    exit 1
  fi
fi

# ---- 3. binário rexglue host (gera app/generated); compila se ausente ----
REXGLUE="$RAIZ/sdk/out/linux-amd64/rexglue"
if [ ! -x "$REXGLUE" ]; then
  echo "[info] compilando o rexglue host (só na primeira vez; demora)..."
  command -v "$CC_BIN" >/dev/null 2>&1 || { echo "ERRO: $CC_BIN não encontrado." >&2; exit 1; }
  cmake -S "$RAIZ/sdk" -B "$RAIZ/sdk/out/linux-amd64" -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_C_COMPILER="$CC_BIN" -DCMAKE_CXX_COMPILER="$CXX_BIN" \
    -DCMAKE_C_FLAGS="-march=$MARCHA" -DCMAKE_CXX_FLAGS="-march=$MARCHA" || exit 1
  cmake --build "$RAIZ/sdk/out/linux-amd64" --target rexglue || exit 1
else
  echo "[info] rexglue host já presente; pulando compilação."
fi
[ -x "$REXGLUE" ] || { echo "ERRO: binário rexglue não gerado em $REXGLUE." >&2; exit 1; }

# ---- 4. codegen (gera app/generated; ~7 min na primeira vez) ----
echo "[info] rodando codegen..."
REXGLUE="$REXGLUE" PYTHON="$PYBIN" "$RAIZ/tools/codegen.sh" app || exit 1
[ -f "$RAIZ/app/generated/rexglue.cmake" ] || {
  echo "ERRO: codegen não gerou app/generated/rexglue.cmake; veja app/codegen.log." >&2
  exit 1
}
[ -f "$RAIZ/app/generated/default/sources.cmake" ] || {
  echo "ERRO: codegen não gerou app/generated/default/sources.cmake; veja app/codegen.log." >&2
  exit 1
}

# ---- 5. configure + build do app (passo 5 do tutorial) ----
echo "[info] configurando e compilando o app..."
cmake -S "$RAIZ/app" -B "$RAIZ/app/out/linux" -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_C_COMPILER="$CC_BIN" -DCMAKE_CXX_COMPILER="$CXX_BIN" \
  -DCMAKE_C_FLAGS="-march=$MARCHA" -DCMAKE_CXX_FLAGS="-march=$MARCHA" \
  -DREXSDK_DIR="$RAIZ/sdk" || exit 1
cmake --build "$RAIZ/app/out/linux" || exit 1

echo "[ok] build do PC pronto: $RAIZ/app/out/linux/fh2"
