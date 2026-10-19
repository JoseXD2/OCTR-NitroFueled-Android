/* SacredFire Android: the SDL3 subset the port uses (gpu/shim window, overlay, src/runtime_pad.c and
 * runtime_audio.c), implemented on the NDK against SDL3's own public headers. Not SDL: one
 * fullscreen window (the NativeActivity's), gamepads seen through input events, touch as mouse plus a
 * virtual gamepad, AAudio output downmixed to stereo.
 *
 * The NativeActivity glue is here too: ANativeActivity_onCreate starts the app's main thread
 * (bb_android_main, from the runtime), an input thread with its own looper, and copies stdout/stderr
 * to logcat (tag SacredFire) and to sacredfire.log (see bb_android_log_path). */
#define _GNU_SOURCE
#include <SDL3/SDL.h>
#include <aaudio/AAudio.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/log.h>
#include <android/looper.h>
#include <android/native_activity.h>
#include <android/native_window.h>
#include <android/window.h>
#include <errno.h>
#include <fcntl.h>
#include <malloc.h>
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "sdl_android.h"

#define TAG "SacredFire"

/* ---------------------------------------------------------------- basics */

static _Thread_local char error_text[256];
static bool set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(error_text, sizeof error_text, fmt, ap);
    va_end(ap);
    return false;
}
const char *SDL_GetError(void) { return error_text; }
void SDL_free(void *mem) { free(mem); }
int SDL_strcasecmp(const char *a, const char *b) { return strcasecmp(a, b); }
char *SDL_strcasestr(const char *haystack, const char *needle) { return strcasestr(haystack, needle); }

static uint64_t now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
static uint64_t start_ns;
Uint64 SDL_GetTicks(void) { return (now_ns() - start_ns) / 1000000ull; }
void SDL_Delay(Uint32 ms) {
    struct timespec ts = {ms / 1000, (long)(ms % 1000) * 1000000l};
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) {}
}

void SDL_GUIDToString(SDL_GUID guid, char *out, int size) {
    static const char hex[] = "0123456789abcdef";
    if (!out || size <= 0) return;
    int n = 0;
    for (int i = 0; i < 16 && n + 2 < size; ++i) {
        out[n++] = hex[guid.data[i] >> 4];
        out[n++] = hex[guid.data[i] & 15];
    }
    out[n] = 0;
}

/* ---------------------------------------------------------------- properties */

typedef enum { PROP_NONE, PROP_POINTER, PROP_STRING, PROP_NUMBER, PROP_BOOLEAN } PropType;
typedef struct Prop {
    struct Prop *next;
    char *name;
    PropType type;
    void *pointer;
    char *string;
    Sint64 number;
    bool boolean;
} Prop;
typedef struct PropSet {
    SDL_PropertiesID id;
    Prop *props;
    struct PropSet *next;
} PropSet;
static pthread_mutex_t props_lock = PTHREAD_MUTEX_INITIALIZER;
static PropSet *prop_sets;
static SDL_PropertiesID next_props_id = 1;

SDL_PropertiesID SDL_CreateProperties(void) {
    PropSet *set = calloc(1, sizeof *set);
    if (!set) return 0;
    pthread_mutex_lock(&props_lock);
    set->id = next_props_id++;
    set->next = prop_sets;
    prop_sets = set;
    pthread_mutex_unlock(&props_lock);
    return set->id;
}
static PropSet *find_set(SDL_PropertiesID id) {
    for (PropSet *s = prop_sets; s; s = s->next)
        if (s->id == id) return s;
    return NULL;
}
static Prop *find_prop(PropSet *set, const char *name, bool create) {
    for (Prop *p = set->props; p; p = p->next)
        if (!strcmp(p->name, name)) return p;
    if (!create) return NULL;
    Prop *p = calloc(1, sizeof *p);
    if (!p) return NULL;
    p->name = strdup(name);
    p->next = set->props;
    set->props = p;
    return p;
}
static void clear_prop(Prop *p) {
    free(p->string);
    p->string = NULL;
    p->type = PROP_NONE;
}
void SDL_DestroyProperties(SDL_PropertiesID id) {
    pthread_mutex_lock(&props_lock);
    for (PropSet **s = &prop_sets; *s; s = &(*s)->next) {
        if ((*s)->id != id) continue;
        PropSet *dead = *s;
        *s = dead->next;
        for (Prop *p = dead->props, *n; p; p = n) {
            n = p->next;
            clear_prop(p);
            free(p->name);
            free(p);
        }
        free(dead);
        break;
    }
    pthread_mutex_unlock(&props_lock);
}
#define SET_PROP(KIND, ASSIGN)                                                        \
    pthread_mutex_lock(&props_lock);                                                  \
    PropSet *set = find_set(props);                                                   \
    Prop *p = set && name ? find_prop(set, name, true) : NULL;                       \
    if (p) {                                                                          \
        clear_prop(p);                                                                \
        p->type = KIND;                                                               \
        ASSIGN;                                                                       \
    }                                                                                 \
    pthread_mutex_unlock(&props_lock);                                                \
    return p != NULL;
bool SDL_SetPointerProperty(SDL_PropertiesID props, const char *name, void *value) {
    SET_PROP(PROP_POINTER, p->pointer = value)
}
bool SDL_SetStringProperty(SDL_PropertiesID props, const char *name, const char *value) {
    SET_PROP(PROP_STRING, p->string = strdup(value ? value : ""))
}
bool SDL_SetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 value) {
    SET_PROP(PROP_NUMBER, p->number = value)
}
bool SDL_SetBooleanProperty(SDL_PropertiesID props, const char *name, bool value) {
    SET_PROP(PROP_BOOLEAN, p->boolean = value)
}
#define GET_PROP(KIND, FIELD)                                                         \
    pthread_mutex_lock(&props_lock);                                                  \
    PropSet *set = find_set(props);                                                   \
    Prop *p = set && name ? find_prop(set, name, false) : NULL;                      \
    if (p && p->type == KIND) default_value = p->FIELD;                               \
    pthread_mutex_unlock(&props_lock);                                                \
    return default_value;
void *SDL_GetPointerProperty(SDL_PropertiesID props, const char *name, void *default_value) {
    GET_PROP(PROP_POINTER, pointer)
}
Sint64 SDL_GetNumberProperty(SDL_PropertiesID props, const char *name, Sint64 default_value) {
    GET_PROP(PROP_NUMBER, number)
}
bool SDL_GetBooleanProperty(SDL_PropertiesID props, const char *name, bool default_value) {
    GET_PROP(PROP_BOOLEAN, boolean)
}
const char *SDL_GetStringProperty(SDL_PropertiesID props, const char *name, const char *default_value) {
    GET_PROP(PROP_STRING, string)
}

/* ---------------------------------------------------------------- activity glue */

static ANativeActivity *activity;
static pthread_mutex_t app_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t app_cond = PTHREAD_COND_INITIALIZER;
static ANativeWindow *native_window; /* under app_lock */
static atomic_bool app_focused, app_paused;
static char log_path[512];

ANativeActivity *bb_android_activity(void) { return activity; }
const char *bb_android_log_path(void) { return log_path; }

/* The runtime's entry point (src/, step 3 of the port). Weak so the library links before it. */
__attribute__((weak)) int bb_android_main(ANativeActivity *a) {
    (void)a;
    printf("SacredFire: bb_android_main is not linked in this build\n");
    return 1;
}

static int log_fd = -1;
/* stdout/stderr go straight to the log file (written synchronously: an _Exit right after a
 * message keeps it). Before the shared storage is accessible the file is in the app's own
 * external folder; bb_android_reopen_log moves it to /sdcard/SacredFire once it is. */
static void point_output_at(const char *path) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0664);
    if (fd < 0) return;
    fflush(stdout);
    fflush(stderr);
    dup2(fd, STDOUT_FILENO);
    dup2(fd, STDERR_FILENO);
    if (log_fd >= 0) close(log_fd);
    log_fd = fd;
    snprintf(log_path, sizeof log_path, "%s", path);
    __android_log_print(ANDROID_LOG_INFO, TAG, "log: %s", path);
}
static void start_logging(void) {
    setvbuf(stdout, NULL, _IOLBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    mkdir("/sdcard/SacredFire", 0775);
    if (access("/sdcard/SacredFire", W_OK) == 0) point_output_at("/sdcard/SacredFire/sacredfire.log");
    if (log_fd < 0 && activity && activity->externalDataPath) {
        char path[512];
        snprintf(path, sizeof path, "%s/sacredfire.log", activity->externalDataPath);
        point_output_at(path);
    }
}
void bb_android_reopen_log(void) {
    if (strcmp(log_path, "/sdcard/SacredFire/sacredfire.log") != 0 && access("/sdcard/SacredFire", W_OK) == 0)
        point_output_at("/sdcard/SacredFire/sacredfire.log");
}

static void *main_thread(void *arg) {
    (void)arg;
    const int code = bb_android_main(activity);
    printf("SacredFire: exit %d\n", code);
    fflush(stdout);
    usleep(200000); /* lets the log thread drain */
    _exit(code);
    return NULL;
}

/* ---------------------------------------------------------------- events */

#define EVENT_QUEUE 1024
static pthread_mutex_t events_lock = PTHREAD_MUTEX_INITIALIZER;
static SDL_Event events[EVENT_QUEUE];
static unsigned events_head, events_count;

static void push_event(SDL_Event *e) {
    if (!e->common.timestamp) e->common.timestamp = now_ns();
    pthread_mutex_lock(&events_lock);
    if (events_count == EVENT_QUEUE) { /* drop the oldest */
        events_head = (events_head + 1) % EVENT_QUEUE;
        --events_count;
    }
    events[(events_head + events_count) % EVENT_QUEUE] = *e;
    ++events_count;
    pthread_mutex_unlock(&events_lock);
}
bool SDL_PushEvent(SDL_Event *e) {
    push_event(e);
    return true;
}
bool SDL_PollEvent(SDL_Event *e) {
    pthread_mutex_lock(&events_lock);
    const bool any = events_count != 0;
    if (any) {
        if (e) *e = events[events_head];
        events_head = (events_head + 1) % EVENT_QUEUE;
        --events_count;
    }
    pthread_mutex_unlock(&events_lock);
    return any;
}

/* ---------------------------------------------------------------- init */

static SDL_InitFlags init_flags;
bool SDL_InitSubSystem(SDL_InitFlags flags) {
    if (!start_ns) start_ns = now_ns();
    init_flags |= flags;
    return true;
}
void SDL_QuitSubSystem(SDL_InitFlags flags) { init_flags &= ~flags; }
SDL_InitFlags SDL_WasInit(SDL_InitFlags flags) { return flags ? (init_flags & flags) : init_flags; }
const char *SDL_GetCurrentVideoDriver(void) { return "android"; }
const char *SDL_GetCurrentAudioDriver(void) { return "aaudio"; }

/* ---------------------------------------------------------------- window and display */

struct SDL_Window {
    SDL_WindowID id;
    SDL_PropertiesID props;
    bool fullscreen; /* as requested: the activity is always fullscreen */
    bool text_input;
};
static SDL_Window the_window = {.id = 1};
static bool window_created;
static const SDL_DisplayID the_display = 1;

/* With app_lock held (the input thread handles events under it). */
static void window_size_locked(int *w, int *h) {
    int ww = native_window ? ANativeWindow_getWidth(native_window) : 0;
    int hh = native_window ? ANativeWindow_getHeight(native_window) : 0;
    if (w) *w = ww > 0 ? ww : 1920;
    if (h) *h = hh > 0 ? hh : 1080;
}
static void window_size(int *w, int *h) {
    pthread_mutex_lock(&app_lock);
    window_size_locked(w, h);
    pthread_mutex_unlock(&app_lock);
}

SDL_DisplayID SDL_GetPrimaryDisplay(void) { return the_display; }
SDL_DisplayID *SDL_GetDisplays(int *count) {
    SDL_DisplayID *ids = calloc(2, sizeof *ids);
    if (ids) ids[0] = the_display;
    if (count) *count = ids ? 1 : 0;
    return ids;
}
const char *SDL_GetDisplayName(SDL_DisplayID id) { return id == the_display ? "Android" : NULL; }
bool SDL_GetDisplayUsableBounds(SDL_DisplayID id, SDL_Rect *rect) {
    if (id != the_display || !rect) return set_error("invalid display");
    rect->x = rect->y = 0;
    window_size(&rect->w, &rect->h);
    return true;
}
const SDL_DisplayMode *SDL_GetDesktopDisplayMode(SDL_DisplayID id) {
    static SDL_DisplayMode mode;
    if (id != the_display) return NULL;
    mode.displayID = the_display;
    mode.format = SDL_PIXELFORMAT_RGBA32;
    window_size(&mode.w, &mode.h);
    mode.pixel_density = 1.0f;
    mode.refresh_rate = 60.0f;
    return &mode;
}

SDL_Window *SDL_CreateWindowWithProperties(SDL_PropertiesID props) {
    (void)props;
    if (window_created) {
        set_error("only one window on Android");
        return NULL;
    }
    /* The activity's surface: wait for it (it comes shortly after start, or when the app returns
     * to the foreground). */
    pthread_mutex_lock(&app_lock);
    while (!native_window) pthread_cond_wait(&app_cond, &app_lock);
    ANativeWindow *w = native_window;
    pthread_mutex_unlock(&app_lock);
    the_window.props = SDL_CreateProperties();
    SDL_SetPointerProperty(the_window.props, SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER, w);
    the_window.fullscreen = SDL_GetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_FULLSCREEN_BOOLEAN, true);
    window_created = true;
    printf("SDL (Android): window %dx%d\n", ANativeWindow_getWidth(w), ANativeWindow_getHeight(w));
    return &the_window;
}
void SDL_DestroyWindow(SDL_Window *window) {
    if (window != &the_window || !window_created) return;
    SDL_DestroyProperties(the_window.props);
    the_window.props = 0;
    window_created = false;
}
SDL_PropertiesID SDL_GetWindowProperties(SDL_Window *window) { return window ? window->props : 0; }
bool SDL_GetWindowSizeInPixels(SDL_Window *window, int *w, int *h) {
    (void)window;
    window_size(w, h);
    return true;
}
SDL_WindowFlags SDL_GetWindowFlags(SDL_Window *window) {
    SDL_WindowFlags flags = SDL_WINDOW_VULKAN;
    if (window && window->fullscreen) flags |= SDL_WINDOW_FULLSCREEN;
    if (atomic_load(&app_focused)) flags |= SDL_WINDOW_INPUT_FOCUS | SDL_WINDOW_MOUSE_FOCUS;
    return flags;
}
bool SDL_SetWindowFullscreen(SDL_Window *window, bool fullscreen) {
    if (window) window->fullscreen = fullscreen;
    return true;
}
bool SDL_SetWindowTitle(SDL_Window *window, const char *title) {
    (void)window;
    (void)title;
    return true;
}
bool SDL_SetWindowIcon(SDL_Window *window, SDL_Surface *icon) {
    (void)window;
    (void)icon;
    return true;
}
float SDL_GetWindowPixelDensity(SDL_Window *window) {
    (void)window;
    return 1.0f; /* sizes and mouse positions are all in pixels */
}
SDL_WindowID SDL_GetWindowID(SDL_Window *window) { return window ? window->id : 0; }
SDL_Window *SDL_GetWindowFromID(SDL_WindowID id) { return id == the_window.id && window_created ? &the_window : NULL; }
bool SDL_ShowCursor(void) { return true; }
bool SDL_HideCursor(void) { return true; }

SDL_Surface *SDL_CreateSurfaceFrom(int width, int height, SDL_PixelFormat format, void *pixels, int pitch) {
    SDL_Surface *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->format = format;
    s->w = width;
    s->h = height;
    s->pitch = pitch;
    s->pixels = pixels;
    s->refcount = 1;
    return s;
}
void SDL_DestroySurface(SDL_Surface *surface) { free(surface); }

/* ---------------------------------------------------------------- keyboard */

static bool key_state[SDL_SCANCODE_COUNT];
static SDL_Keymod key_mod;

const bool *SDL_GetKeyboardState(int *numkeys) {
    if (numkeys) *numkeys = SDL_SCANCODE_COUNT;
    return key_state;
}

typedef struct { int32_t android; SDL_Scancode scancode; SDL_Keycode key; } KeyMap;
static const KeyMap key_map[] = {
    {AKEYCODE_A, SDL_SCANCODE_A, SDLK_A}, {AKEYCODE_B, SDL_SCANCODE_B, SDLK_B}, {AKEYCODE_C, SDL_SCANCODE_C, SDLK_C},
    {AKEYCODE_D, SDL_SCANCODE_D, SDLK_D}, {AKEYCODE_E, SDL_SCANCODE_E, SDLK_E}, {AKEYCODE_F, SDL_SCANCODE_F, SDLK_F},
    {AKEYCODE_G, SDL_SCANCODE_G, SDLK_G}, {AKEYCODE_H, SDL_SCANCODE_H, SDLK_H}, {AKEYCODE_I, SDL_SCANCODE_I, SDLK_I},
    {AKEYCODE_J, SDL_SCANCODE_J, SDLK_J}, {AKEYCODE_K, SDL_SCANCODE_K, SDLK_K}, {AKEYCODE_L, SDL_SCANCODE_L, SDLK_L},
    {AKEYCODE_M, SDL_SCANCODE_M, SDLK_M}, {AKEYCODE_N, SDL_SCANCODE_N, SDLK_N}, {AKEYCODE_O, SDL_SCANCODE_O, SDLK_O},
    {AKEYCODE_P, SDL_SCANCODE_P, SDLK_P}, {AKEYCODE_Q, SDL_SCANCODE_Q, SDLK_Q}, {AKEYCODE_R, SDL_SCANCODE_R, SDLK_R},
    {AKEYCODE_S, SDL_SCANCODE_S, SDLK_S}, {AKEYCODE_T, SDL_SCANCODE_T, SDLK_T}, {AKEYCODE_U, SDL_SCANCODE_U, SDLK_U},
    {AKEYCODE_V, SDL_SCANCODE_V, SDLK_V}, {AKEYCODE_W, SDL_SCANCODE_W, SDLK_W}, {AKEYCODE_X, SDL_SCANCODE_X, SDLK_X},
    {AKEYCODE_Y, SDL_SCANCODE_Y, SDLK_Y}, {AKEYCODE_Z, SDL_SCANCODE_Z, SDLK_Z},
    {AKEYCODE_0, SDL_SCANCODE_0, SDLK_0}, {AKEYCODE_1, SDL_SCANCODE_1, SDLK_1}, {AKEYCODE_2, SDL_SCANCODE_2, SDLK_2},
    {AKEYCODE_3, SDL_SCANCODE_3, SDLK_3}, {AKEYCODE_4, SDL_SCANCODE_4, SDLK_4}, {AKEYCODE_5, SDL_SCANCODE_5, SDLK_5},
    {AKEYCODE_6, SDL_SCANCODE_6, SDLK_6}, {AKEYCODE_7, SDL_SCANCODE_7, SDLK_7}, {AKEYCODE_8, SDL_SCANCODE_8, SDLK_8},
    {AKEYCODE_9, SDL_SCANCODE_9, SDLK_9},
    {AKEYCODE_ENTER, SDL_SCANCODE_RETURN, SDLK_RETURN}, {AKEYCODE_NUMPAD_ENTER, SDL_SCANCODE_KP_ENTER, SDLK_KP_ENTER},
    {AKEYCODE_ESCAPE, SDL_SCANCODE_ESCAPE, SDLK_ESCAPE}, {AKEYCODE_DEL, SDL_SCANCODE_BACKSPACE, SDLK_BACKSPACE},
    {AKEYCODE_FORWARD_DEL, SDL_SCANCODE_DELETE, SDLK_DELETE}, {AKEYCODE_TAB, SDL_SCANCODE_TAB, SDLK_TAB},
    {AKEYCODE_SPACE, SDL_SCANCODE_SPACE, SDLK_SPACE}, {AKEYCODE_INSERT, SDL_SCANCODE_INSERT, SDLK_INSERT},
    {AKEYCODE_MOVE_HOME, SDL_SCANCODE_HOME, SDLK_HOME}, {AKEYCODE_MOVE_END, SDL_SCANCODE_END, SDLK_END},
    {AKEYCODE_PAGE_UP, SDL_SCANCODE_PAGEUP, SDLK_PAGEUP}, {AKEYCODE_PAGE_DOWN, SDL_SCANCODE_PAGEDOWN, SDLK_PAGEDOWN},
    {AKEYCODE_DPAD_UP, SDL_SCANCODE_UP, SDLK_UP}, {AKEYCODE_DPAD_DOWN, SDL_SCANCODE_DOWN, SDLK_DOWN},
    {AKEYCODE_DPAD_LEFT, SDL_SCANCODE_LEFT, SDLK_LEFT}, {AKEYCODE_DPAD_RIGHT, SDL_SCANCODE_RIGHT, SDLK_RIGHT},
    {AKEYCODE_SHIFT_LEFT, SDL_SCANCODE_LSHIFT, SDLK_LSHIFT}, {AKEYCODE_SHIFT_RIGHT, SDL_SCANCODE_RSHIFT, SDLK_RSHIFT},
    {AKEYCODE_CTRL_LEFT, SDL_SCANCODE_LCTRL, SDLK_LCTRL}, {AKEYCODE_CTRL_RIGHT, SDL_SCANCODE_RCTRL, SDLK_RCTRL},
    {AKEYCODE_ALT_LEFT, SDL_SCANCODE_LALT, SDLK_LALT}, {AKEYCODE_ALT_RIGHT, SDL_SCANCODE_RALT, SDLK_RALT},
    {AKEYCODE_MINUS, SDL_SCANCODE_MINUS, SDLK_MINUS}, {AKEYCODE_EQUALS, SDL_SCANCODE_EQUALS, SDLK_EQUALS},
    {AKEYCODE_LEFT_BRACKET, SDL_SCANCODE_LEFTBRACKET, SDLK_LEFTBRACKET},
    {AKEYCODE_RIGHT_BRACKET, SDL_SCANCODE_RIGHTBRACKET, SDLK_RIGHTBRACKET},
    {AKEYCODE_BACKSLASH, SDL_SCANCODE_BACKSLASH, SDLK_BACKSLASH}, {AKEYCODE_SEMICOLON, SDL_SCANCODE_SEMICOLON, SDLK_SEMICOLON},
    {AKEYCODE_APOSTROPHE, SDL_SCANCODE_APOSTROPHE, SDLK_APOSTROPHE}, {AKEYCODE_GRAVE, SDL_SCANCODE_GRAVE, SDLK_GRAVE},
    {AKEYCODE_COMMA, SDL_SCANCODE_COMMA, SDLK_COMMA}, {AKEYCODE_PERIOD, SDL_SCANCODE_PERIOD, SDLK_PERIOD},
    {AKEYCODE_SLASH, SDL_SCANCODE_SLASH, SDLK_SLASH},
    {AKEYCODE_F1, SDL_SCANCODE_F1, SDLK_F1}, {AKEYCODE_F2, SDL_SCANCODE_F2, SDLK_F2}, {AKEYCODE_F3, SDL_SCANCODE_F3, SDLK_F3},
    {AKEYCODE_F4, SDL_SCANCODE_F4, SDLK_F4}, {AKEYCODE_F5, SDL_SCANCODE_F5, SDLK_F5}, {AKEYCODE_F6, SDL_SCANCODE_F6, SDLK_F6},
    {AKEYCODE_F7, SDL_SCANCODE_F7, SDLK_F7}, {AKEYCODE_F8, SDL_SCANCODE_F8, SDLK_F8}, {AKEYCODE_F9, SDL_SCANCODE_F9, SDLK_F9},
    {AKEYCODE_F10, SDL_SCANCODE_F10, SDLK_F10}, {AKEYCODE_F11, SDL_SCANCODE_F11, SDLK_F11}, {AKEYCODE_F12, SDL_SCANCODE_F12, SDLK_F12},
};

/* SDL's names for the keys bbport.ini may bind (SDL_GetScancodeName spelling). */
static const struct { const char *name; SDL_Scancode scancode; } scancode_names[] = {
    {"Return", SDL_SCANCODE_RETURN}, {"Escape", SDL_SCANCODE_ESCAPE}, {"Backspace", SDL_SCANCODE_BACKSPACE},
    {"Tab", SDL_SCANCODE_TAB}, {"Space", SDL_SCANCODE_SPACE}, {"-", SDL_SCANCODE_MINUS}, {"=", SDL_SCANCODE_EQUALS},
    {"[", SDL_SCANCODE_LEFTBRACKET}, {"]", SDL_SCANCODE_RIGHTBRACKET}, {"\\", SDL_SCANCODE_BACKSLASH},
    {";", SDL_SCANCODE_SEMICOLON}, {"'", SDL_SCANCODE_APOSTROPHE}, {"`", SDL_SCANCODE_GRAVE}, {",", SDL_SCANCODE_COMMA},
    {".", SDL_SCANCODE_PERIOD}, {"/", SDL_SCANCODE_SLASH}, {"CapsLock", SDL_SCANCODE_CAPSLOCK},
    {"Insert", SDL_SCANCODE_INSERT}, {"Home", SDL_SCANCODE_HOME}, {"PageUp", SDL_SCANCODE_PAGEUP},
    {"Delete", SDL_SCANCODE_DELETE}, {"End", SDL_SCANCODE_END}, {"PageDown", SDL_SCANCODE_PAGEDOWN},
    {"Right", SDL_SCANCODE_RIGHT}, {"Left", SDL_SCANCODE_LEFT}, {"Down", SDL_SCANCODE_DOWN}, {"Up", SDL_SCANCODE_UP},
    {"Keypad Enter", SDL_SCANCODE_KP_ENTER}, {"Left Ctrl", SDL_SCANCODE_LCTRL}, {"Left Shift", SDL_SCANCODE_LSHIFT},
    {"Left Alt", SDL_SCANCODE_LALT}, {"Right Ctrl", SDL_SCANCODE_RCTRL}, {"Right Shift", SDL_SCANCODE_RSHIFT},
    {"Right Alt", SDL_SCANCODE_RALT},
};
SDL_Scancode SDL_GetScancodeFromName(const char *name) {
    if (!name || !*name) return SDL_SCANCODE_UNKNOWN;
    if (!name[1]) {
        const char c = name[0];
        if (c >= 'a' && c <= 'z') return (SDL_Scancode)(SDL_SCANCODE_A + (c - 'a'));
        if (c >= 'A' && c <= 'Z') return (SDL_Scancode)(SDL_SCANCODE_A + (c - 'A'));
        if (c >= '1' && c <= '9') return (SDL_Scancode)(SDL_SCANCODE_1 + (c - '1'));
        if (c == '0') return SDL_SCANCODE_0;
    }
    if ((name[0] == 'F' || name[0] == 'f') && name[1] >= '1' && name[1] <= '9') {
        const int n = atoi(name + 1);
        if (n >= 1 && n <= 12) return (SDL_Scancode)(SDL_SCANCODE_F1 + n - 1);
    }
    for (size_t i = 0; i < sizeof scancode_names / sizeof *scancode_names; ++i)
        if (!strcasecmp(name, scancode_names[i].name)) return scancode_names[i].scancode;
    return SDL_SCANCODE_UNKNOWN;
}

bool SDL_StartTextInput(SDL_Window *window) {
    if (window) window->text_input = true;
    if (activity) ANativeActivity_showSoftInput(activity, ANATIVEACTIVITY_SHOW_SOFT_INPUT_FORCED);
    return true;
}
bool SDL_StopTextInput(SDL_Window *window) {
    if (window) window->text_input = false;
    if (activity) ANativeActivity_hideSoftInput(activity, 0);
    return true;
}
bool SDL_TextInputActive(SDL_Window *window) { return window && window->text_input; }

static char text_buffers[16][8];
static unsigned text_next;
static void push_text(uint32_t codepoint) {
    char *t = text_buffers[text_next++ % 16];
    if (codepoint < 0x80) {
        t[0] = (char)codepoint;
        t[1] = 0;
    } else if (codepoint < 0x800) {
        t[0] = (char)(0xC0 | (codepoint >> 6));
        t[1] = (char)(0x80 | (codepoint & 0x3F));
        t[2] = 0;
    } else {
        t[0] = (char)(0xE0 | (codepoint >> 12));
        t[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        t[2] = (char)(0x80 | (codepoint & 0x3F));
        t[3] = 0;
    }
    SDL_Event e = {0};
    e.text.type = SDL_EVENT_TEXT_INPUT;
    e.text.windowID = the_window.id;
    e.text.text = t;
    push_event(&e);
}
/* The character a key types (US layout; NativeActivity has no access to the IME's text). */
static uint32_t key_char(int32_t code, bool shift) {
    if (code >= AKEYCODE_A && code <= AKEYCODE_Z) return (uint32_t)((shift ? 'A' : 'a') + code - AKEYCODE_A);
    if (code >= AKEYCODE_0 && code <= AKEYCODE_9) {
        static const char shifted[] = ")!@#$%^&*(";
        return shift ? (uint32_t)shifted[code - AKEYCODE_0] : (uint32_t)('0' + code - AKEYCODE_0);
    }
    switch (code) {
    case AKEYCODE_SPACE: return ' ';
    case AKEYCODE_MINUS: return shift ? '_' : '-';
    case AKEYCODE_EQUALS: return shift ? '+' : '=';
    case AKEYCODE_PERIOD: return shift ? '>' : '.';
    case AKEYCODE_COMMA: return shift ? '<' : ',';
    case AKEYCODE_SLASH: return shift ? '?' : '/';
    case AKEYCODE_SEMICOLON: return shift ? ':' : ';';
    case AKEYCODE_APOSTROPHE: return shift ? '"' : '\'';
    case AKEYCODE_AT: return '@';
    case AKEYCODE_PLUS: return '+';
    default: return 0;
    }
}

static void key_event(SDL_Scancode scancode, SDL_Keycode key, bool down, bool repeat) {
    if (scancode > SDL_SCANCODE_UNKNOWN && scancode < SDL_SCANCODE_COUNT) key_state[scancode] = down;
    SDL_Keymod bit = 0;
    switch (scancode) {
    case SDL_SCANCODE_LSHIFT: bit = SDL_KMOD_LSHIFT; break;
    case SDL_SCANCODE_RSHIFT: bit = SDL_KMOD_RSHIFT; break;
    case SDL_SCANCODE_LCTRL: bit = SDL_KMOD_LCTRL; break;
    case SDL_SCANCODE_RCTRL: bit = SDL_KMOD_RCTRL; break;
    case SDL_SCANCODE_LALT: bit = SDL_KMOD_LALT; break;
    case SDL_SCANCODE_RALT: bit = SDL_KMOD_RALT; break;
    default: break;
    }
    if (bit) key_mod = down ? (key_mod | bit) : (key_mod & ~bit);
    SDL_Event e = {0};
    e.key.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = the_window.id;
    e.key.scancode = scancode;
    e.key.key = key;
    e.key.mod = key_mod;
    e.key.down = down;
    e.key.repeat = repeat;
    push_event(&e);
}
/* A tap of a key (virtual buttons: menu toggle). */
static void key_tap(SDL_Scancode scancode, SDL_Keycode key) {
    key_event(scancode, key, true, false);
    key_event(scancode, key, false, false);
}

/* ---------------------------------------------------------------- gamepads */

#define MAX_PADS 8
#define VIRTUAL_PAD_ID 0x7f000001u
typedef struct {
    bool used, connected, virtual_pad;
    int32_t device;          /* Android input device id */
    SDL_JoystickID id;
    char name[64];
    bool buttons[SDL_GAMEPAD_BUTTON_COUNT];
    Sint16 axes[SDL_GAMEPAD_AXIS_COUNT];
    bool right_stick_rxry;   /* the device reports the right stick on RX/RY instead of Z/RZ */
    int open_count;
} Pad;
struct SDL_Gamepad { Pad *pad; };
static pthread_mutex_t pads_lock = PTHREAD_MUTEX_INITIALIZER;
static Pad pads[MAX_PADS];
static struct SDL_Gamepad pad_handles[MAX_PADS];
static SDL_JoystickID next_pad_id = 1;

static Pad *pad_by_id(SDL_JoystickID id) {
    for (int i = 0; i < MAX_PADS; ++i)
        if (pads[i].used && pads[i].id == id) return &pads[i];
    return NULL;
}
/* The pad for an input device, added on its first event (the NDK cannot list devices). */
static Pad *pad_for_device(int32_t device) {
    for (int i = 0; i < MAX_PADS; ++i)
        if (pads[i].used && !pads[i].virtual_pad && pads[i].device == device) return &pads[i];
    for (int i = 0; i < MAX_PADS; ++i) {
        if (pads[i].used) continue;
        memset(&pads[i], 0, sizeof pads[i]);
        pads[i].used = pads[i].connected = true;
        pads[i].device = device;
        pads[i].id = next_pad_id++;
        snprintf(pads[i].name, sizeof pads[i].name, "Android gamepad %d", (int)device);
        pad_handles[i].pad = &pads[i];
        printf("SDL (Android): gamepad %u (input device %d) added\n", (unsigned)pads[i].id, (int)device);
        return &pads[i];
    }
    return NULL;
}
static Pad *virtual_pad(void) {
    for (int i = 0; i < MAX_PADS; ++i)
        if (pads[i].used && pads[i].virtual_pad) return &pads[i];
    for (int i = 0; i < MAX_PADS; ++i) {
        if (pads[i].used) continue;
        memset(&pads[i], 0, sizeof pads[i]);
        pads[i].used = pads[i].connected = pads[i].virtual_pad = true;
        pads[i].device = -1;
        pads[i].id = VIRTUAL_PAD_ID;
        snprintf(pads[i].name, sizeof pads[i].name, "Touch controls");
        pad_handles[i].pad = &pads[i];
        return &pads[i];
    }
    return NULL;
}
static void pad_button(Pad *pad, SDL_GamepadButton button, bool down) {
    if (!pad || button < 0 || button >= SDL_GAMEPAD_BUTTON_COUNT || pad->buttons[button] == down) return;
    pad->buttons[button] = down;
    SDL_Event e = {0};
    e.gbutton.type = down ? SDL_EVENT_GAMEPAD_BUTTON_DOWN : SDL_EVENT_GAMEPAD_BUTTON_UP;
    e.gbutton.which = pad->id;
    e.gbutton.button = (Uint8)button;
    e.gbutton.down = down;
    push_event(&e);
}

SDL_JoystickID *SDL_GetGamepads(int *count) {
    pthread_mutex_lock(&pads_lock);
    SDL_JoystickID *ids = calloc(MAX_PADS + 1, sizeof *ids);
    int n = 0;
    for (int i = 0; ids && i < MAX_PADS; ++i)
        if (pads[i].used && pads[i].connected) ids[n++] = pads[i].id;
    pthread_mutex_unlock(&pads_lock);
    if (count) *count = n;
    return ids;
}
SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id) {
    pthread_mutex_lock(&pads_lock);
    Pad *pad = pad_by_id(id);
    if (pad) ++pad->open_count;
    pthread_mutex_unlock(&pads_lock);
    if (!pad) {
        set_error("no gamepad %u", (unsigned)id);
        return NULL;
    }
    return &pad_handles[pad - pads];
}
void SDL_CloseGamepad(SDL_Gamepad *g) {
    if (!g) return;
    pthread_mutex_lock(&pads_lock);
    if (g->pad->open_count > 0) --g->pad->open_count;
    pthread_mutex_unlock(&pads_lock);
}
bool SDL_GamepadConnected(SDL_Gamepad *g) { return g && g->pad->used && g->pad->connected; }
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *g) { return g ? g->pad->id : 0; }
const char *SDL_GetGamepadName(SDL_Gamepad *g) { return g ? g->pad->name : NULL; }
const char *SDL_GetGamepadNameForID(SDL_JoystickID id) {
    Pad *pad = pad_by_id(id);
    return pad ? pad->name : NULL;
}
SDL_GUID SDL_GetGamepadGUIDForID(SDL_JoystickID id) {
    SDL_GUID guid = {{0}};
    Pad *pad = pad_by_id(id);
    if (pad) {
        memcpy(guid.data, "andr", 4);
        memcpy(guid.data + 4, &pad->device, sizeof pad->device);
        guid.data[8] = pad->virtual_pad;
    }
    return guid;
}
bool SDL_GetGamepadButton(SDL_Gamepad *g, SDL_GamepadButton button) {
    return g && button >= 0 && button < SDL_GAMEPAD_BUTTON_COUNT && g->pad->buttons[button];
}
Sint16 SDL_GetGamepadAxis(SDL_Gamepad *g, SDL_GamepadAxis axis) {
    return g && axis >= 0 && axis < SDL_GAMEPAD_AXIS_COUNT ? g->pad->axes[axis] : 0;
}
bool SDL_RumbleGamepad(SDL_Gamepad *g, Uint16 low, Uint16 high, Uint32 ms) {
    (void)g; (void)low; (void)high; (void)ms;
    return set_error("rumble is not supported"); /* needs the Vibrator service (JNI) */
}
int SDL_GetNumGamepadTouchpads(SDL_Gamepad *g) {
    (void)g;
    return 0;
}
int SDL_GetNumGamepadTouchpadFingers(SDL_Gamepad *g, int touchpad) {
    (void)g; (void)touchpad;
    return 0;
}
bool SDL_GetGamepadTouchpadFinger(SDL_Gamepad *g, int touchpad, int finger, bool *down, float *x, float *y,
                                  float *pressure) {
    (void)g; (void)touchpad; (void)finger;
    if (down) *down = false;
    if (x) *x = 0;
    if (y) *y = 0;
    if (pressure) *pressure = 0;
    return false;
}
SDL_GamepadButton SDL_GetGamepadButtonFromString(const char *s) {
    static const char *names[SDL_GAMEPAD_BUTTON_COUNT] = {
        [SDL_GAMEPAD_BUTTON_SOUTH] = "a", [SDL_GAMEPAD_BUTTON_EAST] = "b", [SDL_GAMEPAD_BUTTON_WEST] = "x",
        [SDL_GAMEPAD_BUTTON_NORTH] = "y", [SDL_GAMEPAD_BUTTON_BACK] = "back", [SDL_GAMEPAD_BUTTON_GUIDE] = "guide",
        [SDL_GAMEPAD_BUTTON_START] = "start", [SDL_GAMEPAD_BUTTON_LEFT_STICK] = "leftstick",
        [SDL_GAMEPAD_BUTTON_RIGHT_STICK] = "rightstick", [SDL_GAMEPAD_BUTTON_LEFT_SHOULDER] = "leftshoulder",
        [SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER] = "rightshoulder", [SDL_GAMEPAD_BUTTON_DPAD_UP] = "dpup",
        [SDL_GAMEPAD_BUTTON_DPAD_DOWN] = "dpdown", [SDL_GAMEPAD_BUTTON_DPAD_LEFT] = "dpleft",
        [SDL_GAMEPAD_BUTTON_DPAD_RIGHT] = "dpright", [SDL_GAMEPAD_BUTTON_MISC1] = "misc1",
        [SDL_GAMEPAD_BUTTON_RIGHT_PADDLE1] = "paddle1", [SDL_GAMEPAD_BUTTON_LEFT_PADDLE1] = "paddle2",
        [SDL_GAMEPAD_BUTTON_RIGHT_PADDLE2] = "paddle3", [SDL_GAMEPAD_BUTTON_LEFT_PADDLE2] = "paddle4",
        [SDL_GAMEPAD_BUTTON_TOUCHPAD] = "touchpad",
    };
    if (!s) return SDL_GAMEPAD_BUTTON_INVALID;
    for (int i = 0; i < SDL_GAMEPAD_BUTTON_COUNT; ++i)
        if (names[i] && !strcasecmp(s, names[i])) return (SDL_GamepadButton)i;
    /* positional names */
    if (!strcasecmp(s, "south")) return SDL_GAMEPAD_BUTTON_SOUTH;
    if (!strcasecmp(s, "east")) return SDL_GAMEPAD_BUTTON_EAST;
    if (!strcasecmp(s, "west")) return SDL_GAMEPAD_BUTTON_WEST;
    if (!strcasecmp(s, "north")) return SDL_GAMEPAD_BUTTON_NORTH;
    return SDL_GAMEPAD_BUTTON_INVALID;
}

static SDL_GamepadButton pad_button_for_key(int32_t code) {
    switch (code) {
    case AKEYCODE_BUTTON_A: return SDL_GAMEPAD_BUTTON_SOUTH;
    case AKEYCODE_BUTTON_B: return SDL_GAMEPAD_BUTTON_EAST;
    case AKEYCODE_BUTTON_X: return SDL_GAMEPAD_BUTTON_WEST;
    case AKEYCODE_BUTTON_Y: return SDL_GAMEPAD_BUTTON_NORTH;
    case AKEYCODE_BUTTON_L1: return SDL_GAMEPAD_BUTTON_LEFT_SHOULDER;
    case AKEYCODE_BUTTON_R1: return SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER;
    case AKEYCODE_BUTTON_THUMBL: return SDL_GAMEPAD_BUTTON_LEFT_STICK;
    case AKEYCODE_BUTTON_THUMBR: return SDL_GAMEPAD_BUTTON_RIGHT_STICK;
    case AKEYCODE_BUTTON_START: return SDL_GAMEPAD_BUTTON_START;
    case AKEYCODE_BUTTON_SELECT: return SDL_GAMEPAD_BUTTON_BACK;
    case AKEYCODE_BUTTON_MODE: return SDL_GAMEPAD_BUTTON_GUIDE;
    case AKEYCODE_DPAD_UP: return SDL_GAMEPAD_BUTTON_DPAD_UP;
    case AKEYCODE_DPAD_DOWN: return SDL_GAMEPAD_BUTTON_DPAD_DOWN;
    case AKEYCODE_DPAD_LEFT: return SDL_GAMEPAD_BUTTON_DPAD_LEFT;
    case AKEYCODE_DPAD_RIGHT: return SDL_GAMEPAD_BUTTON_DPAD_RIGHT;
    default: return SDL_GAMEPAD_BUTTON_INVALID;
    }
}

static Sint16 to_axis(float v) {
    if (v > 1.0f) v = 1.0f;
    if (v < -1.0f) v = -1.0f;
    return (Sint16)lrintf(v * 32767.0f);
}

static void joystick_motion(const AInputEvent *event) {
    Pad *pad = pad_for_device(AInputEvent_getDeviceId(event));
    if (!pad) return;
    const float rx = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RX, 0);
    const float ry = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RY, 0);
    const float z = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Z, 0);
    const float rz = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RZ, 0);
    if ((rx != 0 || ry != 0) && z == 0 && rz == 0) pad->right_stick_rxry = true;
    pad->axes[SDL_GAMEPAD_AXIS_LEFTX] = to_axis(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_X, 0));
    pad->axes[SDL_GAMEPAD_AXIS_LEFTY] = to_axis(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_Y, 0));
    pad->axes[SDL_GAMEPAD_AXIS_RIGHTX] = to_axis(pad->right_stick_rxry ? rx : z);
    pad->axes[SDL_GAMEPAD_AXIS_RIGHTY] = to_axis(pad->right_stick_rxry ? ry : rz);
    const float lt = fmaxf(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_LTRIGGER, 0),
                           AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_BRAKE, 0));
    const float rt = fmaxf(AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_RTRIGGER, 0),
                           AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_GAS, 0));
    pad->axes[SDL_GAMEPAD_AXIS_LEFT_TRIGGER] = to_axis(lt);
    pad->axes[SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] = to_axis(rt);
    const float hx = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_X, 0);
    const float hy = AMotionEvent_getAxisValue(event, AMOTION_EVENT_AXIS_HAT_Y, 0);
    pad_button(pad, SDL_GAMEPAD_BUTTON_DPAD_LEFT, hx < -0.5f);
    pad_button(pad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT, hx > 0.5f);
    pad_button(pad, SDL_GAMEPAD_BUTTON_DPAD_UP, hy < -0.5f);
    pad_button(pad, SDL_GAMEPAD_BUTTON_DPAD_DOWN, hy > 0.5f);
}

/* ---------------------------------------------------------------- touch: mouse + virtual gamepad */

/* Layout in units of the screen height, x measured from the left (negative: from the right edge).
 * The overlay draws it from bb_android_touch_controls(). */
static const BbTouchControl controls[] = {
    {BB_TOUCH_STICK, -1, 0.30f, 0.70f, 0.20f, "stick"},
    {BB_TOUCH_BUTTON, SDL_GAMEPAD_BUTTON_SOUTH, -0.30f, 0.80f, 0.10f, "X"},
    {BB_TOUCH_BUTTON, SDL_GAMEPAD_BUTTON_WEST, -0.48f, 0.66f, 0.09f, "\xE2\x96\xA1"},
    {BB_TOUCH_BUTTON, SDL_GAMEPAD_BUTTON_EAST, -0.13f, 0.66f, 0.09f, "O"},
    {BB_TOUCH_BUTTON, SDL_GAMEPAD_BUTTON_NORTH, -0.30f, 0.52f, 0.09f, "\xE2\x96\xB3"},
    {BB_TOUCH_BUTTON, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, -0.15f, 0.28f, 0.10f, "R1"},
    {BB_TOUCH_BUTTON, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 0.15f, 0.28f, 0.10f, "L1"},
    {BB_TOUCH_BUTTON, SDL_GAMEPAD_BUTTON_START, -0.60f, 0.08f, 0.06f, "Start"},
    {BB_TOUCH_BUTTON, SDL_GAMEPAD_BUTTON_DPAD_UP, 0.60f, 0.08f, 0.06f, "\xE2\x96\xB2"},
    {BB_TOUCH_MENU, -1, 0.50f, 0.06f, 0.06f, "Menu"}, /* x 0.5: centered */
};
const BbTouchControl *bb_android_touch_controls(int *count) {
    if (count) *count = (int)(sizeof controls / sizeof *controls);
    return controls;
}
static atomic_bool touch_controls_visible = true;
void bb_android_set_touch_controls(bool visible) { atomic_store(&touch_controls_visible, visible); }
bool bb_android_touch_controls_visible(void) { return atomic_load(&touch_controls_visible); }
/* A control's center in pixels. */
void bb_android_touch_control_center(const BbTouchControl *c, float width, float height, float *x, float *y) {
    if (c->x == 0.5f && c->kind == BB_TOUCH_MENU) *x = width * 0.5f;
    else *x = c->x >= 0 ? c->x * height : width + c->x * height;
    *y = c->y * height;
}

#define MAX_TOUCHES 10
typedef struct { int32_t pointer; int control; float x0, y0; bool mouse; } Touch;
static Touch touches[MAX_TOUCHES];
static int touch_count;
static float mouse_x, mouse_y;
static bool mouse_down, mouse_seen;
static int stick_touch = -1;

static int control_at(float x, float y, float w, float h) {
    if (!atomic_load(&touch_controls_visible)) return -1;
    int best = -1;
    float best_d = 1e30f;
    for (int i = 0; i < (int)(sizeof controls / sizeof *controls); ++i) {
        float cx, cy;
        bb_android_touch_control_center(&controls[i], w, h, &cx, &cy);
        const float r = controls[i].radius * h * (controls[i].kind == BB_TOUCH_STICK ? 1.6f : 1.3f);
        const float d = (x - cx) * (x - cx) + (y - cy) * (y - cy);
        if (d < r * r && d < best_d) {
            best = i;
            best_d = d;
        }
    }
    return best;
}
static void mouse_event(Uint32 type, float x, float y) {
    mouse_x = x;
    mouse_y = y;
    mouse_seen = true;
    SDL_Event e = {0};
    if (type == SDL_EVENT_MOUSE_MOTION) {
        e.motion.type = type;
        e.motion.windowID = the_window.id;
        e.motion.x = x;
        e.motion.y = y;
        e.motion.state = mouse_down ? SDL_BUTTON_LMASK : 0;
    } else {
        mouse_down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        e.button.type = type;
        e.button.windowID = the_window.id;
        e.button.button = SDL_BUTTON_LEFT;
        e.button.down = mouse_down;
        e.button.clicks = 1;
        e.button.x = x;
        e.button.y = y;
    }
    push_event(&e);
}
static void update_stick(Pad *pad, const Touch *t, float x, float y, float h) {
    const float range = controls[0].radius * h;
    pad->axes[SDL_GAMEPAD_AXIS_LEFTX] = to_axis((x - t->x0) / range);
    pad->axes[SDL_GAMEPAD_AXIS_LEFTY] = to_axis((y - t->y0) / range);
}
static void touch_down(int32_t pointer, float x, float y, float w, float h) {
    if (touch_count == MAX_TOUCHES) return;
    Touch *t = &touches[touch_count++];
    t->pointer = pointer;
    t->x0 = x;
    t->y0 = y;
    t->control = control_at(x, y, w, h);
    t->mouse = false;
    Pad *pad = virtual_pad();
    if (t->control < 0) {
        /* Outside the controls: the first such finger is the mouse (menu, launcher UI). */
        bool mouse_taken = false;
        for (int i = 0; i < touch_count - 1; ++i) mouse_taken |= touches[i].mouse;
        if (!mouse_taken) {
            t->mouse = true;
            mouse_event(SDL_EVENT_MOUSE_MOTION, x, y);
            mouse_event(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y);
        }
        return;
    }
    const BbTouchControl *c = &controls[t->control];
    if (c->kind == BB_TOUCH_MENU) key_tap(SDL_SCANCODE_INSERT, SDLK_INSERT);
    else if (c->kind == BB_TOUCH_BUTTON) pad_button(pad, (SDL_GamepadButton)c->button, true);
    else if (c->kind == BB_TOUCH_STICK) {
        stick_touch = pointer;
        update_stick(pad, t, x, y, h);
    }
}
static Touch *find_touch(int32_t pointer) {
    for (int i = 0; i < touch_count; ++i)
        if (touches[i].pointer == pointer) return &touches[i];
    return NULL;
}
static void touch_move(int32_t pointer, float x, float y, float h) {
    Touch *t = find_touch(pointer);
    if (!t) return;
    if (t->mouse) mouse_event(SDL_EVENT_MOUSE_MOTION, x, y);
    else if (t->control >= 0 && controls[t->control].kind == BB_TOUCH_STICK) update_stick(virtual_pad(), t, x, y, h);
}
static void touch_up(int32_t pointer, float x, float y) {
    Touch *t = find_touch(pointer);
    if (!t) return;
    Pad *pad = virtual_pad();
    if (t->mouse) mouse_event(SDL_EVENT_MOUSE_BUTTON_UP, x, y);
    else if (t->control >= 0) {
        const BbTouchControl *c = &controls[t->control];
        if (c->kind == BB_TOUCH_BUTTON) pad_button(pad, (SDL_GamepadButton)c->button, false);
        else if (c->kind == BB_TOUCH_STICK && stick_touch == pointer) {
            stick_touch = -1;
            pad->axes[SDL_GAMEPAD_AXIS_LEFTX] = pad->axes[SDL_GAMEPAD_AXIS_LEFTY] = 0;
        }
    }
    *t = touches[--touch_count];
}
static void touch_event(const AInputEvent *event) {
    int w, h;
    window_size_locked(&w, &h); /* app_lock is held: window_size() would deadlock (ANR) */
    const int32_t action = AMotionEvent_getAction(event);
    const int32_t masked = action & AMOTION_EVENT_ACTION_MASK;
    const size_t index = (size_t)((action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >>
                                  AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT);
    switch (masked) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        touch_down(AMotionEvent_getPointerId(event, index), AMotionEvent_getX(event, index),
                   AMotionEvent_getY(event, index), (float)w, (float)h);
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        for (size_t i = 0; i < AMotionEvent_getPointerCount(event); ++i)
            touch_move(AMotionEvent_getPointerId(event, i), AMotionEvent_getX(event, i), AMotionEvent_getY(event, i),
                       (float)h);
        break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
        touch_up(AMotionEvent_getPointerId(event, index), AMotionEvent_getX(event, index),
                 AMotionEvent_getY(event, index));
        break;
    case AMOTION_EVENT_ACTION_CANCEL:
        while (touch_count) touch_up(touches[0].pointer, mouse_x, mouse_y);
        break;
    default:
        break;
    }
}

SDL_MouseButtonFlags SDL_GetMouseState(float *x, float *y) {
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
    return mouse_down ? SDL_BUTTON_LMASK : 0;
}
SDL_Window *SDL_GetMouseFocus(void) { return mouse_seen && window_created ? &the_window : NULL; }

/* ---------------------------------------------------------------- input thread */

static ALooper *input_looper;
static AInputQueue *input_queue; /* under app_lock */

/* 1 when the event was handled. */
static int handle_input(const AInputEvent *event) {
    const int32_t type = AInputEvent_getType(event);
    const int32_t source = AInputEvent_getSource(event);
    pthread_mutex_lock(&pads_lock);
    int handled = 0;
    if (type == AINPUT_EVENT_TYPE_KEY) {
        const int32_t code = AKeyEvent_getKeyCode(event);
        const int32_t action = AKeyEvent_getAction(event);
        const bool down = action == AKEY_EVENT_ACTION_DOWN;
        const bool repeat = down && AKeyEvent_getRepeatCount(event) > 0;
        const bool from_pad = (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD ||
                              (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK;
        const SDL_GamepadButton button = pad_button_for_key(code);
        if (code == AKEYCODE_BACK) {
            /* Back toggles the settings menu instead of closing the app. */
            if (down && !repeat) key_tap(SDL_SCANCODE_INSERT, SDLK_INSERT);
            handled = 1;
        } else if (code == AKEYCODE_BUTTON_L2 || code == AKEYCODE_BUTTON_R2) {
            Pad *pad = pad_for_device(AInputEvent_getDeviceId(event));
            if (pad) pad->axes[code == AKEYCODE_BUTTON_L2 ? SDL_GAMEPAD_AXIS_LEFT_TRIGGER : SDL_GAMEPAD_AXIS_RIGHT_TRIGGER] =
                down ? 32767 : 0;
            handled = 1;
        } else if (button != SDL_GAMEPAD_BUTTON_INVALID && (from_pad || code >= AKEYCODE_BUTTON_A)) {
            if (!repeat) pad_button(pad_for_device(AInputEvent_getDeviceId(event)), button, down);
            handled = 1;
        } else if (code == AKEYCODE_VOLUME_UP || code == AKEYCODE_VOLUME_DOWN || code == AKEYCODE_VOLUME_MUTE) {
            handled = 0; /* the system's */
        } else {
            for (size_t i = 0; i < sizeof key_map / sizeof *key_map; ++i) {
                if (key_map[i].android != code) continue;
                if (action == AKEY_EVENT_ACTION_DOWN || action == AKEY_EVENT_ACTION_UP)
                    key_event(key_map[i].scancode, key_map[i].key, down, repeat);
                handled = 1;
                break;
            }
            if (down && the_window.text_input) {
                const uint32_t c = key_char(code, (AKeyEvent_getMetaState(event) & AMETA_SHIFT_ON) != 0);
                if (c) push_text(c);
                handled = 1;
            }
        }
    } else if (type == AINPUT_EVENT_TYPE_MOTION) {
        if ((source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {
            joystick_motion(event);
            handled = 1;
        } else if ((source & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN ||
                   (source & AINPUT_SOURCE_MOUSE) == AINPUT_SOURCE_MOUSE) {
            touch_event(event);
            handled = 1;
        }
    }
    pthread_mutex_unlock(&pads_lock);
    return handled;
}

static int input_callback(int fd, int events, void *data) {
    (void)fd; (void)events; (void)data;
    pthread_mutex_lock(&app_lock);
    AInputQueue *queue = input_queue;
    if (queue) {
        AInputEvent *event = NULL;
        while (AInputQueue_getEvent(queue, &event) >= 0) {
            if (AInputQueue_preDispatchEvent(queue, event)) continue;
            AInputQueue_finishEvent(queue, event, handle_input(event));
        }
    }
    pthread_mutex_unlock(&app_lock);
    return 1;
}

static void *input_thread(void *arg) {
    (void)arg;
    ALooper *looper = ALooper_prepare(0);
    pthread_mutex_lock(&app_lock);
    input_looper = looper;
    pthread_cond_broadcast(&app_cond);
    pthread_mutex_unlock(&app_lock);
    for (;;) ALooper_pollOnce(-1, NULL, NULL, NULL);
    return NULL;
}

/* ---------------------------------------------------------------- activity callbacks */

static void on_window_created(ANativeActivity *a, ANativeWindow *window) {
    (void)a;
    pthread_mutex_lock(&app_lock);
    if (native_window && native_window != window) ANativeWindow_release(native_window);
    if (native_window != window) ANativeWindow_acquire(window);
    native_window = window;
    pthread_cond_broadcast(&app_cond);
    pthread_mutex_unlock(&app_lock);
    printf("SDL (Android): native window %dx%d\n", ANativeWindow_getWidth(window), ANativeWindow_getHeight(window));
}
static void on_window_destroyed(ANativeActivity *a, ANativeWindow *window) {
    (void)a;
    /* The renderer keeps its reference (acquired above) until the app returns: presenting fails
     * while in the background. */
    printf("SDL (Android): native window %p destroyed (app in the background)\n", (void *)window);
}
static void on_window_resized(ANativeActivity *a, ANativeWindow *window) {
    (void)a; (void)window;
    SDL_Event e = {0};
    e.window.type = SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED;
    e.window.windowID = the_window.id;
    window_size(&e.window.data1, &e.window.data2);
    push_event(&e);
}
static void on_input_queue_created(ANativeActivity *a, AInputQueue *queue) {
    (void)a;
    pthread_mutex_lock(&app_lock);
    while (!input_looper) pthread_cond_wait(&app_cond, &app_lock);
    input_queue = queue;
    AInputQueue_attachLooper(queue, input_looper, 1, input_callback, NULL);
    pthread_mutex_unlock(&app_lock);
}
static void on_input_queue_destroyed(ANativeActivity *a, AInputQueue *queue) {
    (void)a;
    pthread_mutex_lock(&app_lock);
    AInputQueue_detachLooper(queue);
    if (input_queue == queue) input_queue = NULL;
    pthread_mutex_unlock(&app_lock);
}
static void on_focus(ANativeActivity *a, int focused) {
    (void)a;
    atomic_store(&app_focused, focused != 0);
    SDL_Event e = {0};
    e.window.type = focused ? SDL_EVENT_WINDOW_FOCUS_GAINED : SDL_EVENT_WINDOW_FOCUS_LOST;
    e.window.windowID = the_window.id;
    push_event(&e);
}
static void on_pause(ANativeActivity *a) {
    (void)a;
    atomic_store(&app_paused, true);
    printf("SDL (Android): paused\n");
}
static void on_resume(ANativeActivity *a) {
    (void)a;
    atomic_store(&app_paused, false);
    printf("SDL (Android): resumed\n");
}
static void on_destroy(ANativeActivity *a) {
    (void)a;
    printf("SDL (Android): activity destroyed, exiting\n");
    fflush(stdout);
    usleep(200000);
    _exit(0);
}
static void on_low_memory(ANativeActivity *a) {
    (void)a;
    printf("SDL (Android): the system reports low memory\n");
}

bool bb_android_paused(void) { return atomic_load(&app_paused); }

/* Heap pointer tagging (Android 11+, top byte of malloc results) off: the game keeps host pointers
 * (thread handles, TLS blocks) in 40-bit fields, and a tag lost there makes free() abort. The
 * level can only be lowered at run time; this runs when the library is loaded, before the port
 * allocates anything it hands to the game. */
__attribute__((constructor(101))) static void disable_heap_tagging(void) {
    mallopt(-204 /* M_BIONIC_SET_HEAP_TAGGING_LEVEL */, 0 /* M_HEAP_TAGGING_LEVEL_NONE */);
}

JNIEXPORT void ANativeActivity_onCreate(ANativeActivity *a, void *saved_state, size_t saved_state_size) {
    (void)saved_state; (void)saved_state_size;
    static bool started;
    activity = a;
    a->callbacks->onNativeWindowCreated = on_window_created;
    a->callbacks->onNativeWindowDestroyed = on_window_destroyed;
    a->callbacks->onNativeWindowResized = on_window_resized;
    a->callbacks->onInputQueueCreated = on_input_queue_created;
    a->callbacks->onInputQueueDestroyed = on_input_queue_destroyed;
    a->callbacks->onWindowFocusChanged = on_focus;
    a->callbacks->onPause = on_pause;
    a->callbacks->onResume = on_resume;
    a->callbacks->onDestroy = on_destroy;
    a->callbacks->onLowMemory = on_low_memory;
    if (started) return; /* the activity was recreated: the app keeps running */
    started = true;
    start_ns = now_ns();
    ANativeActivity_setWindowFlags(a, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    start_logging();
    printf("SacredFire Android: internal %s, external %s, SDK %d\n", a->internalDataPath ? a->internalDataPath : "?",
           a->externalDataPath ? a->externalDataPath : "?", a->sdkVersion);
    pthread_t t;
    pthread_create(&t, NULL, input_thread, NULL);
    pthread_detach(t);
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    pthread_create(&t, &attr, main_thread, NULL);
    pthread_attr_destroy(&attr);
    pthread_detach(t);
}

/* ---------------------------------------------------------------- audio (AAudio) */

struct SDL_AudioStream {
    SDL_AudioSpec spec;
    AAudioStream *aaudio;
    pthread_mutex_t lock;
    uint8_t *ring;
    size_t size, head, used; /* bytes */
    atomic_bool reopen;
    bool started;
};

static size_t sample_bytes(SDL_AudioFormat f) { return SDL_AUDIO_BYTESIZE(f); }
static float read_sample(const uint8_t *p, SDL_AudioFormat f) {
    if (f == SDL_AUDIO_F32) {
        float v;
        memcpy(&v, p, 4);
        return v;
    }
    if (f == SDL_AUDIO_S16) {
        int16_t v;
        memcpy(&v, p, 2);
        return v / 32768.0f;
    }
    if (f == SDL_AUDIO_S32) {
        int32_t v;
        memcpy(&v, p, 4);
        return (float)v / 2147483648.0f;
    }
    return 0.0f;
}

/* PS4 channel order: L R C LFE SL SR (BL BR). */
static void downmix(const float *in, int channels, float *l, float *r) {
    switch (channels) {
    case 1: *l = *r = in[0]; return;
    case 2: *l = in[0]; *r = in[1]; return;
    default: {
        const float c = channels > 2 ? in[2] * 0.7071f : 0.0f;
        float sl = channels > 4 ? in[4] : 0.0f, sr = channels > 5 ? in[5] : 0.0f;
        if (channels > 7) {
            sl += in[6];
            sr += in[7];
        }
        *l = in[0] + c + sl * 0.5f;
        *r = in[1] + c + sr * 0.5f;
        return;
    }
    }
}

static aaudio_data_callback_result_t audio_callback(AAudioStream *as, void *user, void *data, int32_t frames) {
    (void)as;
    SDL_AudioStream *s = user;
    float *out = data;
    const int channels = s->spec.channels;
    const size_t bps = sample_bytes(s->spec.format), frame_bytes = bps * (size_t)channels;
    pthread_mutex_lock(&s->lock);
    for (int32_t i = 0; i < frames; ++i) {
        float l = 0, r = 0;
        if (s->used >= frame_bytes) {
            float in[8] = {0};
            for (int c = 0; c < channels && c < 8; ++c) {
                uint8_t tmp[4];
                for (size_t b = 0; b < bps; ++b) tmp[b] = s->ring[(s->head + (size_t)c * bps + b) % s->size];
                in[c] = read_sample(tmp, s->spec.format);
            }
            s->head = (s->head + frame_bytes) % s->size;
            s->used -= frame_bytes;
            downmix(in, channels, &l, &r);
        }
        out[2 * i] = fminf(fmaxf(l, -1.0f), 1.0f);
        out[2 * i + 1] = fminf(fmaxf(r, -1.0f), 1.0f);
    }
    pthread_mutex_unlock(&s->lock);
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void audio_error(AAudioStream *as, void *user, aaudio_result_t error) {
    (void)as;
    SDL_AudioStream *s = user;
    printf("SDL (Android): audio stream error %d (%s), reopening\n", error, AAudio_convertResultToText(error));
    atomic_store(&s->reopen, true); /* reopened from the producer's thread (not allowed here) */
}
static bool open_aaudio(SDL_AudioStream *s) {
    AAudioStreamBuilder *b = NULL;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return set_error("AAudio_createStreamBuilder failed");
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_FLOAT);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, s->spec.freq);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setSharingMode(b, AAUDIO_SHARING_MODE_SHARED);
    AAudioStreamBuilder_setDataCallback(b, audio_callback, s);
    AAudioStreamBuilder_setErrorCallback(b, audio_error, s);
    const aaudio_result_t r = AAudioStreamBuilder_openStream(b, &s->aaudio);
    AAudioStreamBuilder_delete(b);
    if (r != AAUDIO_OK) {
        s->aaudio = NULL;
        return set_error("AAudio openStream: %s", AAudio_convertResultToText(r));
    }
    printf("SDL (Android): audio %d Hz, %d channels in, stereo out, rate %d, burst %d\n", s->spec.freq,
           s->spec.channels, AAudioStream_getSampleRate(s->aaudio), AAudioStream_getFramesPerBurst(s->aaudio));
    return true;
}

SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID device, const SDL_AudioSpec *spec,
                                           SDL_AudioStreamCallback callback, void *userdata) {
    (void)device; (void)userdata;
    if (callback) {
        set_error("audio stream callbacks are not supported");
        return NULL;
    }
    if (!spec || spec->channels < 1 || spec->channels > 8 || !sample_bytes(spec->format)) {
        set_error("unsupported audio format");
        return NULL;
    }
    SDL_AudioStream *s = calloc(1, sizeof *s);
    if (!s) return NULL;
    s->spec = *spec;
    pthread_mutex_init(&s->lock, NULL);
    /* 0.5 s of input: the runtime keeps a few periods queued. */
    s->size = (size_t)spec->freq / 2 * sample_bytes(spec->format) * (size_t)spec->channels;
    s->ring = malloc(s->size);
    if (!s->ring || !open_aaudio(s)) {
        free(s->ring);
        pthread_mutex_destroy(&s->lock);
        free(s);
        return NULL;
    }
    return s;
}
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *s) {
    if (!s || !s->aaudio) return set_error("no audio stream");
    const aaudio_result_t r = AAudioStream_requestStart(s->aaudio);
    s->started = r == AAUDIO_OK;
    return s->started || set_error("AAudio start: %s", AAudio_convertResultToText(r));
}
static void reopen_if_needed(SDL_AudioStream *s) {
    if (!atomic_exchange(&s->reopen, false)) return;
    if (s->aaudio) AAudioStream_close(s->aaudio);
    s->aaudio = NULL;
    if (open_aaudio(s) && s->started) AAudioStream_requestStart(s->aaudio);
}
bool SDL_PutAudioStreamData(SDL_AudioStream *s, const void *buf, int len) {
    if (!s || len < 0) return set_error("invalid audio stream");
    reopen_if_needed(s);
    const uint8_t *in = buf;
    pthread_mutex_lock(&s->lock);
    size_t n = (size_t)len;
    if (n > s->size - s->used) n = s->size - s->used; /* full: the newest data is dropped */
    for (size_t i = 0; i < n; ++i) s->ring[(s->head + s->used + i) % s->size] = in[i];
    s->used += n;
    pthread_mutex_unlock(&s->lock);
    return true;
}
int SDL_GetAudioStreamQueued(SDL_AudioStream *s) {
    if (!s) return -1;
    pthread_mutex_lock(&s->lock);
    const int used = (int)s->used;
    pthread_mutex_unlock(&s->lock);
    return used;
}
void SDL_DestroyAudioStream(SDL_AudioStream *s) {
    if (!s) return;
    if (s->aaudio) {
        AAudioStream_requestStop(s->aaudio);
        AAudioStream_close(s->aaudio);
    }
    free(s->ring);
    pthread_mutex_destroy(&s->lock);
    free(s);
}
