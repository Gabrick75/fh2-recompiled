#!/bin/bash
# Unattended test up to the first drive ("Drive to the Festival"): presses through the start screens
# and the intro videos with the script pad, holds the throttle from 190 s and saves PNG captures every
# 15 s in app/out/linux/capturas (the last ones are in game). Extra arguments go to the game.
#   tools/teste_jogabilidade.sh --vulkan_readback_resolve=true
set -e
RAIZ=$(cd "$(dirname "$0")/.." && pwd)
ROTEIRO="8:start,10:a,12:start,14:a,16:start,18:a,20:start,22:a,50:start,60:start,70:start,80:start"
ROTEIRO="$ROTEIRO,90:a,100:a,190-225:rt"
rm -rf "$RAIZ/app/out/linux/capturas"
SEGUNDOS=${SEGUNDOS:-230} "$RAIZ/tools/rodar_pc.sh" --fh2_teste_botoes="$ROTEIRO" --fh2_captura_cada_s=15 \
  --fh2_captura_max=15 "$@" || true
ls "$RAIZ/app/out/linux/capturas"
