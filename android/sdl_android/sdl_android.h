/* SacredFire Android: what the NDK SDL3 subset (sdl_android.c) offers beyond SDL's API: the activity,
 * the log file and the touch controls (drawn by the overlay, read by the input thread). */
#ifndef BB_SDL_ANDROID_H
#define BB_SDL_ANDROID_H
#include <stdbool.h>
#ifdef __cplusplus
extern "C" {
#endif

struct ANativeActivity;
struct ANativeActivity *bb_android_activity(void);
/* The runtime's entry point, called on the app's main thread (not the UI thread). */
int bb_android_main(struct ANativeActivity *activity);
/* Where stdout/stderr are copied (with logcat): /sdcard/SacredFire/sacredfire.log when writable. */
const char *bb_android_log_path(void);
/* True while the activity is paused (in the background). */
bool bb_android_paused(void);

enum { BB_TOUCH_STICK, BB_TOUCH_BUTTON, BB_TOUCH_MENU };
typedef struct {
    int kind;
    int button;   /* SDL_GamepadButton for BB_TOUCH_BUTTON */
    float x;      /* center, in screen heights from the left edge (negative: from the right edge) */
    float y;      /* center, in screen heights from the top */
    float radius; /* in screen heights */
    const char *label;
} BbTouchControl;
const BbTouchControl *bb_android_touch_controls(int *count);
void bb_android_touch_control_center(const BbTouchControl *control, float width, float height, float *x, float *y);
void bb_android_set_touch_controls(bool visible);
bool bb_android_touch_controls_visible(void);

#ifdef __cplusplus
}
#endif
#endif
