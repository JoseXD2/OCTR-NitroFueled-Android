#!/usr/bin/env bash
# Builds an Android arm64 sysroot (headers, stub libs, crt objects) from AOSP sources
# (bionic, frameworks/base) and Debian's Android headers, for clang --target=aarch64-linux-android$API.
set -euo pipefail
W=/home/claude/w
API=${API:-29}
S=$W/sysroot
B=$W/bionic
rm -rf "$S"; mkdir -p "$S/usr/include/aarch64-linux-android" "$S/usr/lib/aarch64-linux-android/$API"
I=$S/usr/include
cp -r $B/libc/include/. "$I/"
for d in $B/libc/kernel/uapi/*; do
    case $(basename "$d") in asm-arm|asm-arm64|asm-x86|asm-riscv) ;; *) cp -r "$d" "$I/" ;; esac
done
cp -r $B/libc/kernel/uapi/asm-arm64/asm "$I/aarch64-linux-android/asm"
cp -r $B/libc/kernel/android/uapi/. "$I/" 2>/dev/null || true
# Android framework headers (Debian android-platform-frameworks-native-headers, liblog, jni)
mkdir -p "$I/android"
cp -r /usr/include/android/android/. "$I/android/"
cp /usr/include/android/log.h "$I/android/log.h" 2>/dev/null || true
cp /usr/include/android/jni.h "$I/jni.h"
cp -r $W/fwbase/native/android/include/android/. "$I/android/" 2>/dev/null || true
cp -r $W/tools/extra_include/. "$I/"
# Vulkan
cp -r $W/vkh/include/vulkan $W/vkh/include/vk_video "$I/"
# Stub libraries
L=$S/usr/lib/aarch64-linux-android/$API
T=$(mktemp -d)
CC="clang --target=aarch64-linux-android$API --sysroot=$S"
stub() { # name map
    python3 $W/tools/mkstubs.py "$2" "$API" arm64 "$T/$1.c" "$T/$1.map"
    $CC -nostdlib -shared -fPIC -Wno-incompatible-library-redeclaration -Wno-builtin-requires-header \
        -fno-builtin -w -Wl,--version-script="$T/$1.map" -Wl,-soname,$1.so "$T/$1.c" -o "$L/$1.so"
}
stub libc $B/libc/libc.map.txt
stub libm $B/libm/libm.map.txt
stub libdl $B/libdl/libdl.map.txt
stub libandroid $W/fwbase/native/android/libandroid.map.txt
for extra in $W/tools/maps/*.map.txt; do stub "$(basename "$extra" .map.txt)" "$extra"; done
# crt objects
CRT="$CC -O2 -fPIC -I$B/libc -I$B/libc/include -DPLATFORM_SDK_VERSION=$API -D__ANDROID_API__=$API -DPAGE_SIZE=4096 -w"
$CRT -c $B/libc/arch-common/bionic/crtbegin.c -o "$L/crtbegin_dynamic.o"
$CRT -c $B/libc/arch-common/bionic/crtbegin.c -o "$L/crtbegin_static.o"
$CRT -c $B/libc/arch-common/bionic/crtbegin_so.c -o "$L/crtbegin_so.o"
$CRT -c $W/tools/crtend.S -o "$L/crtend_android.o"
$CRT -c $W/tools/crtend.S -o "$L/crtend_so.o"
echo "sysroot: $S (API $API)"
for x in pthread rt; do llvm-ar rcs "$L/lib$x.a"; done
