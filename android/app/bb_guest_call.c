/* SacredFire Android: native code calling a function pointer that may be the game's (x86-64, run on
 * Box64) or the port's own (src/guest_call.h). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include "bb_box64.h"
#include "guest_call.h"

int bb_is_native_code(const void *address) {
    Dl_info info;
    return address && dladdr(address, &info) && info.dli_fname;
}

typedef uint64_t (*Native6)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);
uint64_t bb_guest_call(const void *function, int nargs, const uint64_t *args) {
    if (!function) return 0;
    if (bb_is_native_code(function) && nargs <= 6) {
        uint64_t a[6] = {0};
        for (int i = 0; i < nargs; ++i) a[i] = args[i];
        /* Extra zero arguments are harmless in AAPCS64. */
        return ((Native6)function)(a[0], a[1], a[2], a[3], a[4], a[5]);
    }
    return bbx_call((uintptr_t)function, nargs, args);
}
