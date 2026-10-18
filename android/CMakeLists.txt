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
