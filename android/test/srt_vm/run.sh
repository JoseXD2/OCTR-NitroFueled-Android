#!/usr/bin/env bash
# SRT walker interpreter test (gpu/shim/bbport_srt_vm.cpp) on aarch64-linux-gnu under qemu:
# xbyak emits the patterns of flatten_extended_userdata_pass, the VM runs them (incl. a faulting load).
# usage: run.sh <zydis source dir (with dependencies/zycore)> <fmt source dir>
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd); root=$here/../../..
D=$1 F=$2 Z=$1/dependencies/zycore out=$(mktemp -d)
cp "$root/gpu/shim/bbport_srt_vm.cpp" "$out/vm.cpp"   # so its includes resolve to the stubs
for f in "$D"/src/*.c "$Z"/src/*.c "$Z"/src/API/*.c; do
    aarch64-linux-gnu-gcc -O1 -c -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD -I"$D/include" -I"$D/src" -I"$Z/include" \
        "$f" -o "$out/$(echo "$f" | md5sum | cut -c1-8).o"
done
aarch64-linux-gnu-g++ -std=c++23 -O2 -DBB_ANDROID -DFMT_HEADER_ONLY -DZYDIS_STATIC_BUILD -DZYCORE_STATIC_BUILD \
    -I"$here/stub" -I"$F/include" -I"$D/include" -I"$Z/include" -I"$root/gpu/third_party/xbyak" \
    "$here/srt_vm_test.cpp" "$out/vm.cpp" "$out"/*.o -o "$out/srt_vm_test" -static
qemu-aarch64-static "$out/srt_vm_test" && echo "srt_vm: OK"
