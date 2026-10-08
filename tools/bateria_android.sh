#!/bin/bash
# Runs a set of phone measurements back to back ("bateria" means "battery" of tests), each one a
# tools/medir_android.sh run at the event menu, and prints one summary line per run. The first and last runs are the
# same setup, as a control for the phone heating up along the way.
#
#   tools/bateria_android.sh
set -u
RAIZ=$(cd "$(dirname "$0")/.." && pwd)
RESUMO=$RAIZ/app/out/bateria_$(date +%Y%m%d_%H%M%S).txt

rodar() {  # name, environment assignments..., -- game options...
  local nome=$1; shift
  local env=()
  while [ $# -gt 0 ] && [ "$1" != "--" ]; do env+=("$1"); shift; done
  [ $# -gt 0 ] && shift
  local saida
  saida=$(env INSTALAR=0 "${env[@]}" "$RAIZ/tools/medir_android.sh" "$@" 2>&1)
  local media antes depois log
  media=$(echo "$saida" | grep -oE "no menu\): [0-9.]+ fps" | grep -oE "[0-9.]+ fps")
  antes=$(echo "$saida" | grep -oE "antes: Thermal Status: [0-9]" | grep -oE "[0-9]$")
  depois=$(echo "$saida" | grep -oE "depois: Thermal Status: [0-9]" | grep -oE "[0-9]$")
  log=$(echo "$saida" | grep -oE "log: [^ ]+" | sed 's/log: //')
  printf "%-28s %10s  térmico %s->%s  %s\n" "$nome" "${media:-?}" "${antes:-?}" "${depois:-?}" "$log" | tee -a "$RESUMO"
}

"$RAIZ/tools/medir_android.sh" > /dev/null 2>&1 || true   # installs the APK (a first, discarded run)
rodar "base"
rodar "sem faixa única" -- --fh2_uma_faixa=false
rodar "buffer 720p" RESOLUCAO=720p
rodar "afinidade CP no cpu7" -- "--fh2_android_afinidade=GPU Commands=7;*=3-6"
rodar "sem vblank adaptativo" -- --vblank_adaptativo=false
rodar "base (controle)"
# The same build compiled for armv8-a (outline atomics), kept by the armv8.2-a commit, then the current APK again.
ANTIGO=$RAIZ/app/out/app-release-armv8a.apk
if [ -f "$ANTIGO" ]; then
  "${ADB:-$HOME/Android/Sdk/platform-tools/adb}" install -r "$ANTIGO" > /dev/null
  rodar "APK armv8-a (antigo)"
  "${ADB:-$HOME/Android/Sdk/platform-tools/adb}" install -r "$RAIZ/android/app/build/outputs/apk/release/app-release.apk" > /dev/null
  rodar "APK armv8.2-a (atual)"
fi
echo "resumo: $RESUMO"
