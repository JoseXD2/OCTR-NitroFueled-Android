/* SacredFire Android: the game's x86-64 code runs on Box64's CPU core (interpreter + ARM64
 * dynarec); the port's runtime and renderer are native ARM64. This header is the whole
 * interface between them. */
#ifndef BB_BOX64_H
#define BB_BOX64_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

/* Once per process, before any other call. Returns 0 on success. */
int bbx_init(void);

/* A guest-callable entry point for a native function: the address of a Box64 bridge
 * ("int3 'S' 'C' wrapper function; ret"). Arguments and results map from the x86-64 SysV
 * convention to AAPCS64: up to 6 integer arguments in registers and further ones from the
 * guest stack, up to 8 vector arguments, results in rax/rdx and xmm0. The same function always
 * gets the same bridge. NULL gives NULL. */
void *bbx_bridge(const void *native_fn, const char *name);
/* Bridges with a known signature (box64 letters: v,i,u,I,U,l,L,p,f,d; e.g. "iFpi") take the
 * dynarec's direct native call path, without leaving translated code. */
void *bbx_bridge_sig(const void *native_fn, const char *name, const char *signature);
/* The native function behind a bridge address, or NULL when the address is not a bridge. */
void *bbx_bridge_target(uintptr_t address);

/* Per thread. The calling native thread gets an x86-64 CPU state (lazily created on first use,
 * with a 1 MiB guest stack below 4 GiB unless bbx_thread_stack gave one). */
void bbx_thread_stack(void *base, size_t size);
void bbx_set_gs_base(uint64_t base);
void bbx_set_fs_base(uint64_t base);
uint64_t bbx_get_gs_base(void);
void bbx_thread_exit(void);

/* Calls the guest function at `address` on this thread with up to 16 integer arguments; returns
 * rax. Nested calls (guest -> native -> guest) are allowed. */
uint64_t bbx_call(uintptr_t address, int nargs, const uint64_t *args);
uint64_t bbx_call0(uintptr_t address);
uint64_t bbx_call1(uintptr_t address, uint64_t a0);
uint64_t bbx_call2(uintptr_t address, uint64_t a0, uint64_t a1);
uint64_t bbx_call3(uintptr_t address, uint64_t a0, uint64_t a1, uint64_t a2);
uint64_t bbx_call4(uintptr_t address, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3);
/* Like bbx_call2 but with the guest stack pointer set to `stack_top` (16-byte aligned) first:
 * the game's entry point and new guest threads. */
uint64_t bbx_call_on_stack(uintptr_t address, uint64_t a0, uint64_t a1, void *stack_top);

/* Guest code memory: executable ranges must be announced (image segments, bridges, generated
 * x86 code); bbx_code_written invalidates translations after the bytes change. */
void bbx_code_mapped(void *address, size_t size, int prot);
void bbx_code_unmapped(void *address, size_t size);
void bbx_code_written(void *address, size_t size);

/* SIGSEGV/SIGBUS from the process handler: 1 when Box64 resolved it (a write to a page that
 * holds translated code), the handler then returns. */
int bbx_handle_fault(void *fault_address, void *ucontext);
/* The guest RIP for a native PC inside translated code (0 when not translated code), and the
 * current thread's guest registers for crash reports. */
uintptr_t bbx_guest_pc(uintptr_t native_pc);
void bbx_dump_thread(int fd);
/* 1 when the calling thread is executing guest code right now (inside bbx_call). */
int bbx_in_guest(void);

/* Settings (before bbx_init): 0/1 dynarec, strong memory ordering level 0-3, log level. */
void bbx_configure(int dynarec, int strongmem, int log_level);

#ifdef __cplusplus
}
#endif
#endif
