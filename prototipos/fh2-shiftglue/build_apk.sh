#!/bin/bash
# Prototype: builds the FH2-on-ShiftGlue APK and installs it on the S23 Ultra if it is connected.
cd "$(dirname "$0")/android" || exit 1
export ANDROID_HOME=$HOME/Android/Sdk ANDROID_SDK_ROOT=$HOME/Android/Sdk
timeout 7200 ./gradlew -q assembleRelease > ../android_build.log 2>&1
echo "gradle:$?" >> ../android_build.log
A=$(ls app/build/outputs/apk/release/*.apk 2>/dev/null | head -1)
{
  grep -E " error:|What went wrong|gradle:" ../android_build.log | head -8
  echo "APK: $A"
  if [ -n "$A" ] && adb -s RQCXB01R9LM get-state >/dev/null 2>&1; then
    adb -s RQCXB01R9LM shell dumpsys window | grep -m1 mFocusedApp
    adb -s RQCXB01R9LM install -r "$A" | tail -1
  fi
} > ../resultado.txt 2>&1
