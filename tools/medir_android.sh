#!/bin/bash
# Measures the game's FPS on the connected Android phone ("medir" means "measure"), without touching it:
# installs the APK, starts the game through the launcher's test bench and prints the "fps:" lines of the log.
#
# The measuring scene is the event menu the player's save opens on ("Festival Race / Horizon Heats", the 3D world
# behind it): the button script only presses START on the title screen (twice; the second one lands on the
# loading screen. A third one reached the event menu once, where START starts the race) and never A, so nothing
# is chosen and the save is never touched. The menu shows at about 45 s.
#
#   tools/medir_android.sh                  install, run 120 s, print the fps lines and the menu average
#   tools/medir_android.sh --gpu_sin_msaa=true          with extra game options
#   INSTALAR=0 tools/medir_android.sh       do not reinstall the APK
#   ROTEIRO="10:start;16:start;22:start;60:a" tools/medir_android.sh    another button script
#
# The phone heats up fast and its clocks drop with it: the script prints the thermal status before and after.
# Compare two builds only from a similar thermal status (tools/esfriar_android.sh waits for it).
set -e
RAIZ=$(cd "$(dirname "$0")/.." && pwd)
ADB=${ADB:-$HOME/Android/Sdk/platform-tools/adb}
PKG=com.forzahorizon2.android
APK=$RAIZ/android/app/build/outputs/apk/release/app-release.apk
SEGUNDOS=${SEGUNDOS:-120}
SAIDA=$RAIZ/app/out/medicao_android_$(date +%Y%m%d_%H%M%S).log

if [ "${INSTALAR:-1}" = 1 ]; then
  # An APK signed with another key (debug before the release key existed) cannot update the installed one:
  # uninstall it first (this erases the app's data).
  if ! "$ADB" install -r "$APK" 2>&1 | tee /dev/stderr | grep -q Success; then
    "$ADB" uninstall "$PKG" > /dev/null 2>&1 || true
    "$ADB" install "$APK" | tail -1
  fi
  # The launcher reads the game files from shared storage (All files access).
  "$ADB" shell appops set "$PKG" MANAGE_EXTERNAL_STORAGE allow
fi
# Screen on while plugged in (a dozing screen pauses the game and ruins the measurement), and awake now.
"$ADB" shell svc power stayon usb
"$ADB" shell input keyevent KEYCODE_WAKEUP
# Past the lock screen (no PIN on the test phones): behind it the game starts paused, with no surface.
"$ADB" shell wm dismiss-keyguard 2>/dev/null || true
termico() { "$ADB" shell dumpsys thermalservice 2>/dev/null | grep -m1 "Thermal Status" | tr -d '\r'; }
echo "antes: $(termico)"

ROTEIRO=${ROTEIRO:-"14:start;20:start"}

ARGS=("--fh2_teste_botoes=$ROTEIRO" "$@")
"$ADB" shell am force-stop "$PKG"
"$ADB" logcat -c
# am splits --esa on commas (the button script separates its steps with ';' for that reason); the single
# quotes keep the phone's shell away from the ';'.
JUNTOS=$(IFS=,; echo "${ARGS[*]}")
# RESOLUCAO=480p|720p|native overrides the display resolution for this run.
EXTRA_RES=${RESOLUCAO:+--es fh2.resolucao $RESOLUCAO}
# USO=1 shows the FPS and the GPU/CPU/RAM use on screen for this run.
[ "${USO:-0}" = 1 ] && EXTRA_RES="$EXTRA_RES --ez fh2.uso true"
"$ADB" shell "am start -n $PKG/.MainActivity --ez fh2.banco true $EXTRA_RES --esa fh2.args '$JUNTOS'" > /dev/null
# Waits by reading the log for SEGUNDOS seconds (timeout ends it), keeping the lines that matter.
timeout "$SEGUNDOS" "$ADB" logcat -v time 2>/dev/null |
  grep --line-buffered -E "fps: |tempos por quadro|faixas \(|MSAA|Unhandled guest|Fatal signal|\[FATAL\]|\[fh2\]|\[teste\] script pad|\[entrada\]|\[quadro\]|GPU por quadro|atalho de resolve|fora da escala" > "$SAIDA" || true
# The last screen, to check what was measured (a video runs at 30 fps and looks like a good result).
"$ADB" exec-out screencap -p > "${SAIDA%.log}.png"
"$ADB" shell am force-stop "$PKG"
echo "depois: $(termico)"
grep -E "Unhandled guest|Fatal signal|\[FATAL\]" "$SAIDA" | head -3 || true
grep "fps: " "$SAIDA" | sed -E 's/^.*fps: /fps: /'
python3 - "$SAIDA" <<'EOF'
import re, sys
values = [float(m.group(1)) for line in open(sys.argv[1]) if (m := re.search(r"fps: ([0-9.]+)", line))]
tail = values[-8:]
if tail:
    print(f"média das últimas {len(tail)} medições (~{5 * len(tail)} s finais, no menu): {sum(tail) / len(tail):.1f} fps")
EOF
echo "log: $SAIDA (tela final: ${SAIDA%.log}.png)"
