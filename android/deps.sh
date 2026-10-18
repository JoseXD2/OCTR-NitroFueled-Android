#!/usr/bin/env bash
# Fetches and builds the GPU library's third-party dependencies for a cross target into a prefix.
# usage: deps.sh <cmake-toolchain-file> <prefix>
set -euo pipefail
toolchain=$1 prefix=$2
here=$(cd -- "$(dirname -- "$0")" && pwd)
src=${DEPS_SRC:-$here/../out/android-deps-src}
mkdir -p "$src" "$prefix"
clone() { # dir url tag
    [[ -d $src/$1/.git ]] || git clone -q --depth 1 --branch "$3" ${4:-} "$2" "$src/$1"
}
build() { # dir [cmake args]
    local d=$1; shift
    cmake -S "$src/$d" -B "$src/$d/build-$(basename "$prefix")" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_INSTALL_PREFIX="$prefix" -DCMAKE_POSITION_INDEPENDENT_CODE=ON \
        -DBUILD_SHARED_LIBS=OFF "$@" > /dev/null
    cmake --build "$src/$d/build-$(basename "$prefix")" > /dev/null
    cmake --install "$src/$d/build-$(basename "$prefix")" > /dev/null
    echo "deps: $d"
}
clone fmt https://github.com/fmtlib/fmt.git 11.1.4
clone robin-map https://github.com/Tessil/robin-map.git v1.3.0
clone vma https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator.git v3.3.0
clone xxhash https://github.com/Cyan4973/xxHash.git v0.8.3
clone zydis https://github.com/zyantific/zydis.git v4.1.1 --recursive
clone magic_enum https://github.com/Neargye/magic_enum.git v0.9.7
clone SDL https://github.com/libsdl-org/SDL.git release-3.2.24
[[ -f $prefix/lib/libfmt.a ]] || build fmt -DFMT_TEST=OFF -DFMT_DOC=OFF
[[ -f $prefix/include/tsl/robin_map.h ]] || build robin-map
[[ -f $prefix/include/vk_mem_alloc.h ]] || build vma
[[ -f $prefix/include/magic_enum/magic_enum.hpp ]] || build magic_enum -DMAGIC_ENUM_OPT_BUILD_EXAMPLES=OFF -DMAGIC_ENUM_OPT_BUILD_TESTS=OFF -DMAGIC_ENUM_OPT_INSTALL=ON
[[ -f $prefix/lib/libxxhash.a ]] || build xxhash/cmake_unofficial -DXXHASH_BUILD_XXHSUM=OFF
[[ -f $prefix/lib/libZydis.a ]] || build zydis -DZYDIS_BUILD_TOOLS=OFF -DZYDIS_BUILD_EXAMPLES=OFF -DZYDIS_BUILD_DOXYGEN=OFF -DZYDIS_BUILD_MAN=OFF
# SDL3: public headers only (the port implements the subset it uses on the NDK, sdl_android/).
mkdir -p "$prefix/include"
[[ -d $prefix/include/SDL3 ]] || cp -r "$src/SDL/include/SDL3" "$prefix/include/"
echo "deps: prefix $prefix"
