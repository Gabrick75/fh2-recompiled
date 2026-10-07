#!/usr/bin/env sh
# Builds the Android APK (android/app/build/outputs/apk/release/app-release.apk), signed with the debug key.
# Needs Java 17+, the Android SDK (ANDROID_HOME, or ~/Android/Sdk) with NDK 28.2.13676358 and CMake 3.30.5, and
# the generated game code in app/generated (run the codegen on the PC first).
set -eu
repo_root=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
command -v java >/dev/null 2>&1 || { echo 'Java 17 or newer is required.' >&2; exit 1; }
if [ -z "${ANDROID_HOME:-}" ] && [ -d "$HOME/Android/Sdk" ]; then
    ANDROID_HOME="$HOME/Android/Sdk"
    export ANDROID_HOME ANDROID_SDK_ROOT="$ANDROID_HOME"
fi
[ -f "$repo_root/app/generated/default/sources.cmake" ] || { echo 'Run the codegen first (app/generated).' >&2; exit 1; }
cd "$repo_root/android"
./gradlew assembleRelease
printf 'APK: %s\n' "$repo_root/android/app/build/outputs/apk/release/app-release.apk"
