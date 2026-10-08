#!/bin/bash
# Waits until the phone cools down to thermal status 1 or lower ("esfriar" means "cool down"), up to 10 min,
# so two measurements start from the same clocks.
ADB=${ADB:-$HOME/Android/Sdk/platform-tools/adb}
"$ADB" get-state > /dev/null 2>&1 || { echo "celular não conectado ao adb"; exit 1; }
for i in $(seq 1 60); do
  s=$("$ADB" shell dumpsys thermalservice | grep -m1 "Thermal Status" | grep -o "[0-9]*")
  [ "${s:-9}" -le 1 ] && { echo "status térmico $s após $((i * 10 - 10)) s"; exit 0; }
  sleep 10
done
echo "ainda quente (status $s) após 10 min"
exit 1
