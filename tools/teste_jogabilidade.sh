#!/bin/bash
# Unattended test up to the first drive ("Drive to the Festival", reached at about 56 s): alternates
# START and A every 2 s until 54 s, which skips the start screens and every intro video as soon as it
# can be skipped, presses B (leaves a pause), holds the throttle from 60 to 85 s, and saves PNG
# captures every 10 s in app/out/linux/capturas (from about 60 s on they are in game). About 90 s.
# Extra arguments go to the game:
#   tools/teste_jogabilidade.sh --gpu_traco_quadro_s=75
set -e
RAIZ=$(cd "$(dirname "$0")/.." && pwd)
ROTEIRO=""
for t in $(seq 6 2 54); do
  if [ $((t % 4)) -eq 2 ]; then ROTEIRO="$ROTEIRO$t:start,"; else ROTEIRO="$ROTEIRO$t:a,"; fi
done
ROTEIRO="${ROTEIRO}56:b,58:b,60-85:rt"
rm -rf "$RAIZ/app/out/linux/capturas"
# CAPTURAS sets how many captures (one every 10 s); the game rejects a cvar given twice.
SEGUNDOS=${SEGUNDOS:-90} "$RAIZ/tools/rodar_pc.sh" --fh2_teste_botoes="$ROTEIRO" --fh2_captura_cada_s=10 \
  --fh2_captura_max=${CAPTURAS:-9} "$@" || true
ls "$RAIZ/app/out/linux/capturas"
