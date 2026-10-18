#!/usr/bin/env bash
# Packages libsacredfire.so into an installable APK (aapt, zipalign, apksigner; debug key).
# usage: build_apk.sh <libsacredfire.so> <out.apk> [keystore dir]
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
lib=$1 apk=$2 keys=${3:-$here/../../out/android-keys}
jar=${ANDROID_JAR:-/usr/lib/android-sdk/platforms/android-23/android.jar}
work=$(mktemp -d)
mkdir -p "$work/root/lib/arm64-v8a" "$keys"
llvm-strip --strip-unneeded -o "$work/root/lib/arm64-v8a/libsacredfire.so" "$lib"
aapt package -f -M "$here/AndroidManifest.xml" -I "$jar" -F "$work/unsigned.apk"
(cd "$work/root" && zip -q -r -0 "$work/unsigned.apk" lib)   # stored: the .so stays page-aligned
zipalign -f -p 16 "$work/unsigned.apk" "$work/aligned.apk"
if [[ ! -f $keys/debug.pk8 ]]; then
    openssl req -x509 -newkey rsa:2048 -nodes -days 10000 -subj "/CN=SacredFire debug" \
        -keyout "$keys/debug.key" -out "$keys/debug.x509.pem" 2>/dev/null
    openssl pkcs8 -topk8 -inform PEM -outform DER -in "$keys/debug.key" -out "$keys/debug.pk8" -nocrypt
fi
apksigner sign --key "$keys/debug.pk8" --cert "$keys/debug.x509.pem" --out "$apk" "$work/aligned.apk"
apksigner verify "$apk"
rm -rf "$work"
echo "APK: $apk ($(du -h "$apk" | cut -f1))"
