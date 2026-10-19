# SacredFire Android: one APK. Builds Box64's CPU core, the NDK SDL3 subset, the GPU library
# (gpu/) and the runtime (src/) into libsacredfire.so, the NativeActivity of the app.
cmake_minimum_required(VERSION 3.24)
project(sacredfire_android C CXX ASM)
set(BB_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/.." CACHE PATH "Repository root")
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
add_compile_definitions(ANDROID)
set(BB_DEPS_PREFIX "" CACHE PATH "Prefix with the cross-built dependencies (deps.sh)")
include_directories(SYSTEM ${BB_DEPS_PREFIX}/include)
add_subdirectory(box64core)
add_subdirectory(sdl_android)
set(BB_ARM_COMPAT_DIR ${CMAKE_CURRENT_SOURCE_DIR}/arm_compat/include)
add_subdirectory(${BB_ROOT}/gpu gpu)

# The runtime (src/: loader and PS4 libraries) and the app's entry: libsacredfire.so, the
# NativeActivity. ANativeActivity_onCreate lives in sdl_android (kept with -u).
file(GLOB BB_RUNTIME CONFIGURE_DEPENDS ${BB_ROOT}/src/runtime*.c)
file(GLOB BB_ATRAC9 CONFIGURE_DEPENDS ${BB_ROOT}/third_party/LibAtrac9/C/src/*.c)
add_library(atrac9 STATIC ${BB_ATRAC9})
target_compile_options(atrac9 PRIVATE -w)
set_target_properties(atrac9 PROPERTIES C_STANDARD 99)
add_library(sacredfire SHARED ${BB_ROOT}/src/probe.c ${BB_RUNTIME} app/bb_android_main.c app/bb_guest_call.c app/bb_lzma.c)
set_target_properties(sacredfire PROPERTIES C_STANDARD 11 C_EXTENSIONS ON)
target_compile_definitions(sacredfire PRIVATE BB_ANDROID=1 _GNU_SOURCE)
target_compile_options(sacredfire PRIVATE -include ${BB_ARM_COMPAT_DIR}/bb_arm_compat.h -Wall -Wextra
    -Wno-unused-parameter -fno-omit-frame-pointer)
target_include_directories(sacredfire PRIVATE ${BB_ROOT} ${BB_ROOT}/src ${BB_ARM_COMPAT_DIR})
target_link_libraries(sacredfire PRIVATE bbgpu atrac9 sdl_android box64core android log aaudio vulkan m dl)
target_link_options(sacredfire PRIVATE -u ANativeActivity_onCreate -Wl,--no-undefined -Wl,-z,max-page-size=16384
    -Wl,--exclude-libs,libavcodec.a:libavformat.a:libavutil.a:libswscale.a:libswresample.a -Wl,-Bsymbolic)
