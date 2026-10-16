#pragma once
#include <stdint.h>
#include <sys/cdefs.h>
#include <android/rect.h>
__BEGIN_DECLS
enum ANativeWindow_LegacyFormat {
    WINDOW_FORMAT_RGBA_8888 = 1, WINDOW_FORMAT_RGBX_8888 = 2, WINDOW_FORMAT_RGB_565 = 4,
};
struct ANativeWindow;
typedef struct ANativeWindow ANativeWindow;
typedef struct ANativeWindow_Buffer {
    int32_t width, height, stride, format; void *bits; uint32_t reserved[6];
} ANativeWindow_Buffer;
void ANativeWindow_acquire(ANativeWindow *window);
void ANativeWindow_release(ANativeWindow *window);
int32_t ANativeWindow_getWidth(ANativeWindow *window);
int32_t ANativeWindow_getHeight(ANativeWindow *window);
int32_t ANativeWindow_getFormat(ANativeWindow *window);
int32_t ANativeWindow_setBuffersGeometry(ANativeWindow *window, int32_t width, int32_t height, int32_t format);
int32_t ANativeWindow_lock(ANativeWindow *window, ANativeWindow_Buffer *outBuffer, ARect *inOutDirtyBounds);
int32_t ANativeWindow_unlockAndPost(ANativeWindow *window);
int32_t ANativeWindow_setFrameRate(ANativeWindow *window, float frameRate, int8_t compatibility);
__END_DECLS
