/* Calls from native code into the game's code, and native functions handed to the game.
 *
 * On x86-64 the game's code runs natively: a guest function pointer is called directly and a
 * native function is its own guest-callable address. SacredFire Android runs the game's code on
 * Box64 (android/box64core): guest functions are entered with bbx_call and native functions reach
 * the game through bridges. A pointer the runtime stores for later may also be one of its own
 * native functions (the AvPlayer's default callbacks): bb_guest_call calls those directly.
 * Integer and pointer arguments and results only (what every call site here passes). */
#ifndef BB_GUEST_CALL_H
#define BB_GUEST_CALL_H
#include <stdint.h>
#ifdef BB_ANDROID
#include "bb_box64.h"
#ifdef __cplusplus
extern "C" {
#endif
/* 1 when `address` is native code (in a loaded shared object), not the game's x86-64 code. */
int bb_is_native_code(const void *address);
uint64_t bb_guest_call(const void *function, int nargs, const uint64_t *args);
#ifdef __cplusplus
}
#endif
#define BB_A(x) ((uint64_t)(uintptr_t)(x))
#define GUEST_CALL0(f) bb_guest_call((const void *)(f), 0, (const uint64_t *)0)
#define GUEST_CALL1(f, a) bb_guest_call((const void *)(f), 1, (const uint64_t[]){BB_A(a)})
#define GUEST_CALL2(f, a, b) bb_guest_call((const void *)(f), 2, (const uint64_t[]){BB_A(a), BB_A(b)})
#define GUEST_CALL3(f, a, b, c) bb_guest_call((const void *)(f), 3, (const uint64_t[]){BB_A(a), BB_A(b), BB_A(c)})
#define GUEST_CALL4(f, a, b, c, d) \
    bb_guest_call((const void *)(f), 4, (const uint64_t[]){BB_A(a), BB_A(b), BB_A(c), BB_A(d)})
#define GUEST_CALL5(f, a, b, c, d, e) \
    bb_guest_call((const void *)(f), 5, (const uint64_t[]){BB_A(a), BB_A(b), BB_A(c), BB_A(d), BB_A(e)})
/* A native function as the game sees it. */
#define GUEST_EXPORT(f) ((uintptr_t)bbx_bridge((const void *)(f), #f))
#else
#define GUEST_CALL0(f) (f)()
#define GUEST_CALL1(f, a) (f)(a)
#define GUEST_CALL2(f, a, b) (f)(a, b)
#define GUEST_CALL3(f, a, b, c) (f)(a, b, c)
#define GUEST_CALL4(f, a, b, c, d) (f)(a, b, c, d)
#define GUEST_CALL5(f, a, b, c, d, e) (f)(a, b, c, d, e)
#define GUEST_EXPORT(f) ((uintptr_t)(f))
#endif

#ifdef __cplusplus
/* C++: BB_GUEST_CALL(fn, args...) has fn's result type. */
#ifdef BB_ANDROID
#include <type_traits>
template <typename R, typename... P, typename... A>
inline R BbGuestCall(R (*fn)(P...), A... args) {
    static_assert(sizeof...(A) <= 16, "too many arguments");
    const uint64_t values[sizeof...(A) + 1] = {(uint64_t)(args)..., 0};
    const uint64_t result = bb_guest_call(reinterpret_cast<const void *>(fn), int(sizeof...(A)), values);
    if constexpr (std::is_void_v<R>) {
        (void)result;
    } else if constexpr (std::is_pointer_v<R>) {
        return reinterpret_cast<R>(static_cast<uintptr_t>(result));
    } else {
        return static_cast<R>(result);
    }
}
#define BB_GUEST_CALL(fn, ...) BbGuestCall((fn), ##__VA_ARGS__)
#else
#define BB_GUEST_CALL(fn, ...) (fn)(__VA_ARGS__)
#endif
#endif
#endif
