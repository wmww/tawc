#!/bin/bash
# Build the Termux-side launcher for TAWC's external Wayland clients.
#
# Needs a JDK and `d8`, and nothing else: the launcher touches only a
# handful of Android types, so if no android.jar is available it compiles
# against the compile-only stubs in stubs/ instead.
#
# Env:
#   ANDROID_JAR  path to an android.jar. Defaults to
#                $ANDROID_HOME/platforms/android-$TAWC_API/android.jar.
#                If unset/missing, stubs/ is used.
#   TAWC_API     API level for the default android.jar (default 36).
#
# Examples:
#   ANDROID_HOME=$HOME/Android/Sdk ./build.sh
#   ./build.sh                                  # stubs only
#   ANDROID_JAR=$HOME/android.jar ./build.sh
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
api="${TAWC_API:-36}"
jar="$here/tawc-wayland.jar"

command -v javac >/dev/null || { echo "build.sh: javac not found" >&2; exit 1; }
command -v zip   >/dev/null || { echo "build.sh: zip not found"   >&2; exit 1; }

d8="$(command -v d8 || true)"
if [ -z "$d8" ] && [ -n "${ANDROID_HOME:-}" ]; then
  d8="$(ls -1 "$ANDROID_HOME"/build-tools/*/d8 2>/dev/null | sort -V | tail -1 || true)"
fi
if [ -z "$d8" ] || [ ! -x "$d8" ]; then
  echo "build.sh: d8 not found (set PATH or ANDROID_HOME)" >&2
  exit 1
fi

out="$here/build"
rm -rf "$out"
mkdir -p "$out/classes"

android_jar="${ANDROID_JAR:-${ANDROID_HOME:-}/platforms/android-$api/android.jar}"
lib_args=()
if [ -f "$android_jar" ]; then
  lib_args=(--lib "$android_jar")
  # Compile against the real SDK. Note it is only a compile classpath: the
  # dex must reference the boot-classpath classes, not bundle them.
  javac --release 11 -classpath "$android_jar" -d "$out/classes" \
    $(find "$here/src" -name '*.java')
else
  echo "build.sh: no android.jar at $android_jar; using bundled stubs" >&2
  mkdir -p "$out/stubs"
  javac --release 11 -d "$out/stubs" $(find "$here/stubs" -name '*.java')
  javac --release 11 -classpath "$out/stubs" -d "$out/classes" \
    $(find "$here/src" -name '*.java')
fi

"$d8" "${lib_args[@]}" --min-api 29 --output "$out" \
  $(find "$out/classes" -name '*.class')

rm -f "$jar"
(cd "$out" && zip -q -X "$jar" classes.dex)
rm -rf "$out"

echo "built $jar"
