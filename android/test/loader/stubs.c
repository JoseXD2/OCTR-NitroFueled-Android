/* Stubs for the loader test: the GPU library and SDL are not linked. */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "gpu/bbgpu.h"
int bb_probe_main(int argc, char **argv);
int main(int argc, char **argv) { return bb_probe_main(argc, argv); }
void bbgpu_register_kernel(void) {}
int bbgpu_init(const BbGpuConfig *config) { (void)config; return 1; }
uintptr_t bbgpu_resolve(const char *nid) { (void)nid; return 0; }
int bbgpu_handle_fault(void *uc, void *a) { (void)uc; (void)a; return 0; }
void bbgpu_dump_guest_writes(void *uc) { (void)uc; }
int bbgpu_text_input_begin(const char *a, const char *b) { (void)a; (void)b; return 0; }
int bbgpu_text_input_poll(char *o, uint64_t n) { (void)o; (void)n; return 2; }
int bbgpu_overlay_captures_input(void) { return 0; }
int bbgpu_pad_rescan_take(void) { return 0; }
void bbgpu_patch_image(unsigned char *i, uint64_t s) { (void)i; (void)s; }
unsigned bbgpu_symbol_count(void) { return 0; }
int vulkan_smoke(void) { return 1; }
/* SDL: nothing connected. */
static bool keys[SDL_SCANCODE_COUNT];
void SDL_CloseGamepad(SDL_Gamepad *g) { (void)g; }
void SDL_DestroyAudioStream(SDL_AudioStream *s) { (void)s; }
void SDL_GUIDToString(SDL_GUID g, char *o, int n) { (void)g; if (n > 0) o[0] = 0; }
bool SDL_GamepadConnected(SDL_Gamepad *g) { (void)g; return false; }
int SDL_GetAudioStreamQueued(SDL_AudioStream *s) { (void)s; return 0; }
const char *SDL_GetCurrentAudioDriver(void) { return "stub"; }
const char *SDL_GetError(void) { return "stub"; }
Sint16 SDL_GetGamepadAxis(SDL_Gamepad *g, SDL_GamepadAxis a) { (void)g; (void)a; return 0; }
bool SDL_GetGamepadButton(SDL_Gamepad *g, SDL_GamepadButton b) { (void)g; (void)b; return false; }
SDL_GamepadButton SDL_GetGamepadButtonFromString(const char *s) { (void)s; return SDL_GAMEPAD_BUTTON_INVALID; }
SDL_GUID SDL_GetGamepadGUIDForID(SDL_JoystickID id) { (void)id; SDL_GUID g = {{0}}; return g; }
SDL_JoystickID SDL_GetGamepadID(SDL_Gamepad *g) { (void)g; return 0; }
const char *SDL_GetGamepadName(SDL_Gamepad *g) { (void)g; return ""; }
const char *SDL_GetGamepadNameForID(SDL_JoystickID id) { (void)id; return ""; }
bool SDL_GetGamepadTouchpadFinger(SDL_Gamepad *g, int t, int f, bool *d, float *x, float *y, float *p) { return false; }
SDL_JoystickID *SDL_GetGamepads(int *c) { if (c) *c = 0; return calloc(1, sizeof(SDL_JoystickID)); }
const bool *SDL_GetKeyboardState(int *n) { if (n) *n = SDL_SCANCODE_COUNT; return keys; }
int SDL_GetNumGamepadTouchpadFingers(SDL_Gamepad *g, int t) { return 0; }
int SDL_GetNumGamepadTouchpads(SDL_Gamepad *g) { return 0; }
SDL_Scancode SDL_GetScancodeFromName(const char *n) { (void)n; return SDL_SCANCODE_UNKNOWN; }
bool SDL_InitSubSystem(SDL_InitFlags f) { (void)f; return false; }
SDL_AudioStream *SDL_OpenAudioDeviceStream(SDL_AudioDeviceID d, const SDL_AudioSpec *s, SDL_AudioStreamCallback c, void *u) { return NULL; }
SDL_Gamepad *SDL_OpenGamepad(SDL_JoystickID id) { (void)id; return NULL; }
bool SDL_PutAudioStreamData(SDL_AudioStream *s, const void *b, int l) { return false; }
void SDL_QuitSubSystem(SDL_InitFlags f) { (void)f; }
bool SDL_ResumeAudioStreamDevice(SDL_AudioStream *s) { return false; }
bool SDL_RumbleGamepad(SDL_Gamepad *g, Uint16 a, Uint16 b, Uint32 c) { return false; }
SDL_InitFlags SDL_WasInit(SDL_InitFlags f) { (void)f; return 0; }
void SDL_free(void *p) { free(p); }
int SDL_strcasecmp(const char *a, const char *b) { return strcasecmp(a, b); }
