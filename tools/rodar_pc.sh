#!/bin/bash
# Runs the Linux PC build of fh2 ("rodar" means "run") with the game in assets/game_root and the
# emulated Xenos GPU. Logs and saves go to app/out/run. The keyboard works as a controller
# (mnk_mode): Enter or X = Start, Space = A, Backspace = B, L = X, P = Y, WASD = left stick,
# arrows = right stick, E / O = right trigger (throttle), Q / I = left trigger (brake),
# Tab = Back, 1 / 3 = shoulders, Shift + arrows = d-pad.
# That is the "pad" layout. The "race" layout (--mnk_preset=race) is the driving one: A/D steer,
# W throttle, S brake, Space handbrake, arrows camera, IJKL d-pad, Enter start, Esc back.
# A key takes mnk_rampa seconds (0.15 by default) to reach the full turn; 0 turns at once.
# gpu_allow_invalid_fetch_constants: the game leaves vertex fetch constants with type 0 in some draws
# (vfetch 90); without it the emulated GPU dropped every such draw. The log rotates at 200 MB instead
# of 5 MB, because the empty resolves of every frame are logged.
# Extra arguments go to the game (cvars):
#   tools/rodar_pc.sh                       play until the window is closed
#   SEGUNDOS=120 tools/rodar_pc.sh          stop after 120 s (SIGINT), for unattended tests
#   tools/rodar_pc.sh --mnk_preset=race    the driving layout of the keyboard
set -e
RAIZ=$(cd "$(dirname "$0")/.." && pwd)
BUILD=${BUILD:-$RAIZ/app/out/linux}
RUN=$RAIZ/app/out/run
# USUARIO picks another user data folder (saves, cache partition), e.g. a fresh profile for a test.
USER_DIR=${USUARIO:-$RUN/user}
mkdir -p "$USER_DIR"
export LD_LIBRARY_PATH=$RAIZ/sdk/out/linux-amd64:$BUILD${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
cd "$RUN"
rm -f fh2.log
CMD=("$BUILD/fh2" --game_data_root="$RAIZ/assets/game_root" --user_data_root="$USER_DIR"
     --log_file="$RUN/fh2.log" --gpu_plugin=xenos --mnk_mode=true
     --gpu_allow_invalid_fetch_constants=true --log_max_file_size_mb=200 "$@")
# Muted unless SOM=1 ("som" means "sound"): the user asked for silent test runs.
if [ "${SOM:-0}" != 1 ]; then
  CMD+=(--audio_mute=true)
fi
if [ -n "${SEGUNDOS:-}" ]; then
  exec timeout -s INT "$SEGUNDOS" "${CMD[@]}" > "$RUN/stdout.txt" 2>&1
else
  exec "${CMD[@]}"
fi
