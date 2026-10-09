#!/bin/bash
# Copies the translated shader library (app/out/shaders_fh2/fh2_shaders.nfsp, built by tools/xenos/biblioteca.sh from
# the game's own files) to the game folder of every connected phone, where the native renderer looks for it
# (Internal storage/jogos/ForzaHorizon2). "Enviar" means "send".
#   tools/enviar_biblioteca_android.sh [serial ...]
set -eu
RAIZ=$(cd "$(dirname "$0")/.." && pwd)
ARQUIVO=$RAIZ/app/out/shaders_fh2/fh2_shaders.nfsp
[ -f "$ARQUIVO" ] || { echo "Falta $ARQUIVO: rode tools/xenos/biblioteca.sh" >&2; exit 1; }
SERIAIS=("$@")
[ ${#SERIAIS[@]} -gt 0 ] || mapfile -t SERIAIS < <(adb devices | awk 'NR > 1 && $2 == "device" {print $1}')
for s in "${SERIAIS[@]}"; do
  adb -s "$s" push "$ARQUIVO" /sdcard/jogos/ForzaHorizon2/fh2_shaders.nfsp
done
