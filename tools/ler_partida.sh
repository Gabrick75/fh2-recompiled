#!/bin/bash
# Pulls the newest play session's logs from a phone and summarizes them ("ler partida" means "read the session"):
# the FPS lines of the game log and the GPU/CPU/RAM lines of the usage log (GameActivity writes both to
# Internal storage/jogos/fh2-logs).
#
#   ANDROID_SERIAL=<device> tools/ler_partida.sh          the newest session
#   ANDROID_SERIAL=<device> tools/ler_partida.sh 2        the session before it
set -e
RAIZ=$(cd "$(dirname "$0")/.." && pwd)
ADB=${ADB:-$HOME/Android/Sdk/platform-tools/adb}
N=${1:-1}
DIR=/sdcard/jogos/fh2-logs
BASE=$("$ADB" shell ls -t "$DIR" | tr -d '\r' | grep -v _uso | sed -n "${N}p")
[ -n "$BASE" ] || { echo "no session logs in $DIR"; exit 1; }
OUT=$RAIZ/app/out/partidas
mkdir -p "$OUT"
"$ADB" pull "$DIR/$BASE" "$OUT/" > /dev/null 2>&1
"$ADB" pull "$DIR/${BASE%.log}_uso.log" "$OUT/" > /dev/null 2>&1 || true
echo "== $BASE"
# The settings GameActivity wrote at the top (device, launcher options, game arguments).
sed -n '/^===== partida/,/^=====$/p' "$OUT/$BASE" | grep -v "^--app_data\|^--game_data_root\|^--user_data_root\|^--cache_root"
echo "-- FPS (every 5 s): time, fps, worst frame, command processor CPU per frame"
grep "fps: " "$OUT/$BASE" | sed -E 's/^\[[0-9-]+ ([0-9:]+)\.[0-9]+\].*fps: ([0-9.]+) .*worst ([0-9.]+) ms, CPU do processador de comandos ([-0-9.]+).*/\1  \2 fps  pior \3 ms  CP \4 ms/'
if [ -f "$OUT/${BASE%.log}_uso.log" ]; then
  echo "-- Usage (every 5 s)"
  cat "$OUT/${BASE%.log}_uso.log"
fi
echo "-- Errors"
grep -E "\[error\]|\[critical\]|FATAL" "$OUT/$BASE" | grep -iv "renderdoc\|ResolvePath\|XMsgStartIORequestEx\|Unimplemented XAM" | head -5 || true
