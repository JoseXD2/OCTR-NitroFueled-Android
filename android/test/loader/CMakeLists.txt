# Loader test on aarch64-linux-gnu (qemu-aarch64): the Android port's loader and runtime (src/)
# with Box64's core, the GPU library and SDL stubbed (run with --cpu-only). Runs the game's
# libc and module initializers and its entry on Box64 until the first GPU or unsupported import.
cmake_minimum_required(VERSION 3.24)
project(bb_loader_test C ASM)
set(BB_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
add_subdirectory(${BB_ROOT}/android/box64core box64core)
file(GLOB BB_RUNTIME ${BB_ROOT}/src/runtime*.c)
file(GLOB BB_ATRAC9 ${BB_ROOT}/third_party/LibAtrac9/C/src/*.c)
add_library(atrac9 STATIC ${BB_ATRAC9})
target_compile_options(atrac9 PRIVATE -w)
add_executable(bb-loader-test ${BB_ROOT}/src/probe.c ${BB_RUNTIME} stubs.c ${BB_ROOT}/android/app/bb_guest_call.c)
target_compile_definitions(bb-loader-test PRIVATE BB_ANDROID=1 _GNU_SOURCE)
target_compile_options(bb-loader-test PRIVATE -include ${BB_ROOT}/android/arm_compat/include/bb_arm_compat.h
    -Wall -Wno-unused-parameter -fno-omit-frame-pointer -O1 -g)
target_include_directories(bb-loader-test PRIVATE ${BB_ROOT} ${BB_ROOT}/src ${BB_ROOT}/android/arm_compat/include
    /home/claude/w/prefix-android/include)
target_link_libraries(bb-loader-test PRIVATE box64core atrac9 m pthread dl)
