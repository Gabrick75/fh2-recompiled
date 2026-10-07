#!/bin/bash
# Measures the game's FPS on the connected Android phone ("medir" means "measure"), without touching it:
# installs the APK, starts the game through the launcher's test bench with a button script (START / A every
# 2 s until it is in game, then B and the throttle), and prints the "fps:" lines of the log.
#
#   tools/medir_android.sh                  install, run 180 s, print the fps lines
#   SEGUNDOS=200 tools/medir_android.sh --gpu_teste_constante=...   longer, with extra game options
#   INSTALAR=0 tools/medir_android.sh       do not reinstall the APK
#
# The phone heats up fast and its clocks drop with it: the script prints the thermal status before and after.
# Compare two builds only from a similar thermal status.
set -e
RAIZ=$(cd "$(dirname "$0")/.." && pwd)
ADB=${ADB:-$HOME/Android/Sdk/platform-tools/adb}
PKG=com.forzahorizon2.android
APK=$RAIZ/android/app/build/outputs/apk/release/app-release.apk
SEGUNDOS=${SEGUNDOS:-180}
SAIDA=$RAIZ/app/out/medicao_android_$(date +%Y%m%d_%H%M%S).log

if [ "${INSTALAR:-1}" = 1 ]; then
  "$ADB" install -r "$APK" | tail -1
fi
termico() { "$ADB" shell dumpsys thermalservice 2>/dev/null | grep -m1 "Thermal Status" | tr -d '\r'; }
echo "antes: $(termico)"

ROTEIRO=""
# The phone loads slower than the PC: keep skipping until 90 s.
for t in $(seq 6 2 90); do
  if [ $((t % 4)) -eq 2 ]; then ROTEIRO="$ROTEIRO$t:start;"; else ROTEIRO="$ROTEIRO$t:a;"; fi
done
ROTEIRO="${ROTEIRO}92:b;94:b;96-$((SEGUNDOS - 5)):rt"

ARGS=("--fh2_teste_botoes=$ROTEIRO" "$@")
"$ADB" shell am force-stop "$PKG"
"$ADB" logcat -c
# am splits --esa on commas (the button script separates its steps with ';' for that reason); the single
# quotes keep the phone's shell away from the ';'.
JUNTOS=$(IFS=,; echo "${ARGS[*]}")
"$ADB" shell "am start -n $PKG/.MainActivity --ez fh2.banco true --esa fh2.args '$JUNTOS'" > /dev/null
# Waits by reading the log for SEGUNDOS seconds (timeout ends it), keeping the lines that matter.
timeout "$SEGUNDOS" "$ADB" logcat -v time 2>/dev/null |
  grep --line-buffered -E "fps: |MSAA|Unhandled guest|Fatal signal|\[FATAL\]|\[fh2\]|\[teste\] script pad with" > "$SAIDA" || true
"$ADB" shell am force-stop "$PKG"
echo "depois: $(termico)"
grep -E "Unhandled guest|Fatal signal|\[FATAL\]" "$SAIDA" | head -3 || true
grep "fps: " "$SAIDA" | sed -E 's/^.*fps: /fps: /'
python3 - "$SAIDA" <<'EOF'
import re, sys
values = [float(m.group(1)) for line in open(sys.argv[1]) if (m := re.search(r"fps: ([0-9.]+)", line))]
tail = values[-6:]
if tail:
    print(f"média das últimas {len(tail)} medições (~{5 * len(tail)} s finais): {sum(tail) / len(tail):.1f} fps")
EOF
echo "log: $SAIDA"
