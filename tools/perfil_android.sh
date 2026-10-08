#!/bin/bash
# CPU profile of the game on the connected phone ("perfil" means "profile"), at the measuring scene of
# tools/medir_android.sh (the event menu the save opens on; only START on the title screen, never A).
#
#   tools/perfil_android.sh                      flat profile by thread, library and function
#   ESPERA=90 DURACAO=15 tools/perfil_android.sh --gpu_sin_msaa=true     other wait/length, extra game options
#
# Needs the APK to be profileable (AndroidManifest.xml). Symbols come from the unstripped libraries of the
# Android build (binary cache in app/out/perfil_simbolos).
set -e
RAIZ=$(cd "$(dirname "$0")/.." && pwd)
ADB=${ADB:-$HOME/Android/Sdk/platform-tools/adb}
PKG=com.forzahorizon2.android
SP_HOST=$HOME/Android/Sdk/ndk/28.2.13676358/simpleperf
ESPERA=${ESPERA:-75}
DURACAO=${DURACAO:-10}
SAIDA=$RAIZ/app/out/perfil_$(date +%Y%m%d_%H%M%S)
mkdir -p "$SAIDA"

"$ADB" shell svc power stayon usb
"$ADB" shell input keyevent KEYCODE_WAKEUP
ARGS=("--fh2_teste_botoes=14:start;20:start" "$@")
JUNTOS=$(IFS=,; echo "${ARGS[*]}")
"$ADB" shell am force-stop "$PKG"
"$ADB" shell "am start -n $PKG/.MainActivity --ez fh2.banco true --esa fh2.args '$JUNTOS'" > /dev/null
sleep "$ESPERA"
"$ADB" exec-out screencap -p > "$SAIDA/tela.png"
"$ADB" shell simpleperf record --app "$PKG" -e cpu-clock -f 2000 --duration "$DURACAO" \
    -o /data/local/tmp/fh2_perf.data > /dev/null 2>&1
"$ADB" shell am force-stop "$PKG"
"$ADB" pull /data/local/tmp/fh2_perf.data "$SAIDA/perf.data" > /dev/null

# Unstripped libraries for the symbols, laid out as simpleperf's binary cache expects (by build id).
SIMBOLOS=$RAIZ/app/out/perfil_simbolos
LIBS=$(find "$RAIZ/android/app/build/intermediates/cxx" -path "*arm64-v8a*" -name "*.so" 2>/dev/null)
python3 "$SP_HOST/binary_cache_builder.py" -i "$SAIDA/perf.data" -lib $(for l in $LIBS; do dirname "$l"; done | sort -u) \
    --binary_cache_dir "$SIMBOLOS" > /dev/null 2>&1 || true
"$SP_HOST/bin/linux/x86_64/simpleperf" report -i "$SAIDA/perf.data" --symfs "$SIMBOLOS" \
    --sort comm,dso --percent-limit 1 2>/dev/null > "$SAIDA/threads.txt" || true
"$SP_HOST/bin/linux/x86_64/simpleperf" report -i "$SAIDA/perf.data" --symfs "$SIMBOLOS" \
    --sort comm,symbol --percent-limit 0.4 2>/dev/null > "$SAIDA/funcoes.txt" || true
sed -n '1,40p' "$SAIDA/threads.txt"
echo
sed -n '1,70p' "$SAIDA/funcoes.txt"
echo "perfil: $SAIDA"
