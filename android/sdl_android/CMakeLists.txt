# NDK implementation of the SDL3 subset the port uses, against SDL3's own public headers
# (installed into the dependency prefix by deps.sh).
add_library(sdl_android STATIC sdl_android.c)
target_include_directories(sdl_android PUBLIC ${BB_DEPS_PREFIX}/include)
target_link_libraries(sdl_android PUBLIC android log aaudio)
