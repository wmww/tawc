#!/bin/bash
# Build a signed release APK (zipaligned, v2/v3 signed).
#
# Keystore: $KEYSTORE_PATH (default: $HOME/Android/keystore.jks)
# Alias:    $KEY_ALIAS     (default: the keystore's sole PrivateKeyEntry,
#                                    or error if there are multiple)
# Prompts for the keystore password unless $KEYSTORE_PASS is set.
# Key password defaults to the keystore password; override with $KEY_PASS.
#
# Flags:
#   --no-build   reuse the existing app-release-unsigned.apk
#   --graphics=list
#              override production graphics backend set
#
# Output: app/build/outputs/apk/release/tawc-v<version>.apk
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-21-openjdk}"
export ANDROID_HOME="${ANDROID_HOME:-$HOME/Android/Sdk}"

KEYSTORE_PATH="${KEYSTORE_PATH:-$HOME/Android/keystore.jks}"

DO_BUILD=1
GRAPHICS="${TAWC_RELEASE_GRAPHICS:-libhybris,cpu}"
for arg in "$@"; do
    case "$arg" in
        --no-build) DO_BUILD=0 ;;
        --graphics=*) GRAPHICS="${arg#--graphics=}" ;;
        -h|--help)
            sed -n '2,/^set -/p' "$0" | sed 's/^# \?//;$d'
            exit 0
            ;;
        *) echo "ERROR: unknown arg: $arg" >&2; exit 2 ;;
    esac
done

[ -f "$KEYSTORE_PATH" ] || { echo "ERROR: keystore not found at $KEYSTORE_PATH" >&2; exit 1; }

BUILD_TOOLS_VER="$(ls -1 "$ANDROID_HOME/build-tools" | sort -V | tail -1)"
[ -n "$BUILD_TOOLS_VER" ] || { echo "ERROR: no build-tools under $ANDROID_HOME/build-tools" >&2; exit 1; }
ZIPALIGN="$ANDROID_HOME/build-tools/$BUILD_TOOLS_VER/zipalign"
APKSIGNER="$ANDROID_HOME/build-tools/$BUILD_TOOLS_VER/apksigner"
AAPT2="$ANDROID_HOME/build-tools/$BUILD_TOOLS_VER/aapt2"

if [ -z "${KEYSTORE_PASS+x}" ]; then
    read -rsp "Keystore password (if any): " KEYSTORE_PASS
    echo
fi
export KEYSTORE_PASS
export KEY_PASS="${KEY_PASS:-$KEYSTORE_PASS}"

# Verify the keystore password and pick an alias up front so we don't
# run the whole release build only to fail at the signing step.
if ! keystore_listing="$(keytool -list -keystore "$KEYSTORE_PATH" -storepass:env KEYSTORE_PASS 2>/dev/null)"; then
    echo "ERROR: keystore password rejected by keytool" >&2
    exit 1
fi
mapfile -t aliases < <(awk -F', ' '/PrivateKeyEntry/ {print $1}' <<<"$keystore_listing")
if [ -z "${KEY_ALIAS+x}" ]; then
    case "${#aliases[@]}" in
        0) echo "ERROR: no PrivateKeyEntry in $KEYSTORE_PATH" >&2; exit 1 ;;
        1) KEY_ALIAS="${aliases[0]}" ;;
        *) echo "ERROR: keystore has multiple aliases; set KEY_ALIAS to one of:" >&2
           printf '  %s\n' "${aliases[@]}" >&2
           exit 1 ;;
    esac
elif ! printf '%s\n' "${aliases[@]}" | grep -Fxq "$KEY_ALIAS"; then
    echo "ERROR: alias '$KEY_ALIAS' not found in keystore. Available:" >&2
    printf '  %s\n' "${aliases[@]}" >&2
    exit 1
fi
# -certreq actually unlocks the private key, which is what apksigner does
# and what -list does not. The stdout CSR is discarded.
verify_key_pass() {
    keytool -certreq -keystore "$KEYSTORE_PATH" -alias "$KEY_ALIAS" \
        -storepass:env KEYSTORE_PASS -keypass:env KEY_PASS >/dev/null 2>&1
}
if ! verify_key_pass; then
    read -rsp "Key password for alias '$KEY_ALIAS' (differs from keystore password): " KEY_PASS
    echo
    export KEY_PASS
    if ! verify_key_pass; then
        echo "ERROR: key password rejected by keytool" >&2
        exit 1
    fi
fi

UNSIGNED="$ROOT_DIR/app/build/outputs/apk/release/app-release-unsigned.apk"
ALIGNED="$ROOT_DIR/app/build/outputs/apk/release/app-release-aligned.apk"
SIGNED="$ROOT_DIR/app/build/outputs/apk/release/app-release.apk"

if [ "$DO_BUILD" -eq 1 ]; then
    echo "=== Building release APK (graphics=$GRAPHICS) ==="
    ( cd "$ROOT_DIR" && ./gradlew "-PtawcGraphics=$GRAPHICS" assembleRelease --quiet )
fi

[ -f "$UNSIGNED" ] || { echo "ERROR: $UNSIGNED not found (drop --no-build to build it)" >&2; exit 1; }

echo "=== Zipaligning ==="
"$ZIPALIGN" -p -f 4 "$UNSIGNED" "$ALIGNED"

echo "=== Signing ==="
"$APKSIGNER" sign \
    --ks "$KEYSTORE_PATH" \
    --ks-key-alias "$KEY_ALIAS" \
    --ks-pass env:KEYSTORE_PASS \
    --key-pass env:KEY_PASS \
    --out "$SIGNED" \
    "$ALIGNED"
rm -f "$ALIGNED"

echo "=== Verifying ==="
"$APKSIGNER" verify --verbose "$SIGNED"

VERSION="$("$AAPT2" dump badging "$SIGNED" | sed -n "s/.*versionName='\([^']*\)'.*/\1/p" | head -1)"
[ -n "$VERSION" ] || { echo "ERROR: could not read versionName from $SIGNED" >&2; exit 1; }
FINAL="$(dirname "$SIGNED")/tawc-v$VERSION.apk"
mv "$SIGNED" "$FINAL"

echo
echo "Signed APK: $FINAL"
