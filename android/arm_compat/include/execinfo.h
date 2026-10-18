/* SacredFire Android: backtrace() for API levels before 33 (bionic's starts there), on the
 * unwinder linked into the app (libunwind). */
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <unwind.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct { void **frames; int max, n; } bb_bt_state;
static _Unwind_Reason_Code bb_bt_step(struct _Unwind_Context *c, void *arg) {
    bb_bt_state *s = (bb_bt_state *)arg;
    uintptr_t pc = _Unwind_GetIP(c);
    if (pc && s->n < s->max) s->frames[s->n++] = (void *)pc;
    return s->n < s->max ? _URC_NO_REASON : _URC_END_OF_STACK;
}
static inline int bb_backtrace(void **frames, int max) {
    bb_bt_state s = {frames, max, 0};
    _Unwind_Backtrace(bb_bt_step, &s);
    return s.n;
}
static inline void bb_backtrace_symbols_fd(void *const *frames, int n, int fd) {
    for (int i = 0; i < n; ++i) dprintf(fd, "  #%d %p\n", i, frames[i]);
}
#define backtrace bb_backtrace
#define backtrace_symbols_fd bb_backtrace_symbols_fd
#ifdef __cplusplus
}
#endif
