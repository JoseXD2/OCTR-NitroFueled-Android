/* SacredFire Android: Box64's CPU core embedded in the port (see include/bb_box64.h).
 *
 * Box64 normally loads an x86-64 ELF program and wraps its native libraries. Here there is no
 * x86 program: the port maps the PS4 game's prepared image itself, the runtime and renderer are
 * native ARM64, and only the game's code runs through Box64's interpreter and dynarec. This file
 * supplies what Box64's loader, librarian, signal and thread layers would otherwise provide:
 * the process context, per-thread CPU state, bridges for native functions and calls into guest
 * code. Based on the structure of Box64's wowbox64 (wine/wow64/wowbox64.c). MIT, like Box64. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <ucontext.h>
#include <unistd.h>
#ifdef ANDROID
#include <android/log.h>
#endif

#include "debug.h"
#include "os.h"
#include "custommem.h"
#include "dynablock.h"
#include "env.h"
#include "emu/x64emu_private.h"
#include "emu/x87emu_private.h"
#include "emu/x64run_private.h"
#include "x64emu.h"
#include "x64trace.h"
#include "box64context.h"
#include "box64cpu.h"
#include "box64cpu_util.h"
#include "build_info.h"
#include "rbtree.h"
#include "hostext.h"
#include "sysinfo.h"
#include "pathcoll.h"
#include "elfloader.h"
#include "bridge.h"
#include "bb_box64.h"

/* ---- globals Box64's core expects from core.c / box64context.c ---- */
uintptr_t box64_pagesize = 4096;
uint32_t default_gs = 0x2b;
uint32_t default_fs = 0;
int box64_rdtsc = 0;
uint8_t box64_rdtsc_shift = 0;
int box64_is32bits = 0;
int box64_isAddressSpace32 = 0;
int box64_unittest_mode = 0;
int box64_wine = 0;
int box64_mapclean = 0;
int box64_in_fatal_handler = 0;
int box64_cycle_log_initialized = 0;
char *ftrace_name = NULL;
sysinfo_t box64_sysinfo = {0};
cpu_ext_t cpuext = {0};
static box64context_t context_storage;
box64context_t *my_context = &context_storage;

static uint32_t parity_tab[8] = {
    0x96696996, 0x69969669, 0x69969669, 0x96696996, 0x69969669, 0x96696996, 0x96696996, 0x69969669,
};

static int cfg_dynarec = 1, cfg_strongmem = -1, cfg_log = -1;

static void bbx_logv(const char *fmt, va_list ap) {
#ifdef ANDROID
    va_list copy;
    va_copy(copy, ap);
    __android_log_vprint(ANDROID_LOG_INFO, "box64", fmt, copy);
    va_end(copy);
#endif
    vfprintf(stderr, fmt, ap);
}
static void bbx_log(const char *fmt, ...) { va_list ap; va_start(ap, fmt); bbx_logv(fmt, ap); va_end(ap); }

/* ---- OS layer (os.h) ---- */
void PrintfFtrace(int prefix, const char *fmt, ...) {
    (void)prefix;
    va_list ap; va_start(ap, fmt); bbx_logv(fmt, ap); va_end(ap);
}
void *GetEnv(const char *name) { return getenv(name); }
int GetTID(void) { return (int)syscall(SYS_gettid); }
int SchedYield(void) { return sched_yield(); }
int IsBridgeSignature(char s, char c) { return s == 'S' && c == 'C'; }
void *InternalMmap(void *addr, unsigned long length, int prot, int flags, int fd, ssize_t offset) {
    return mmap(addr, length, prot, flags, fd, offset);
}
int InternalMunmap(void *addr, unsigned long length) { return munmap(addr, length); }
int FileExist(const char *filename, int flags) {
    struct stat sb;
    if (stat(filename, &sb) == -1) return 0;
    if ((flags & IS_FILE) && !S_ISREG(sb.st_mode)) return 0;
    if ((flags & IS_EXECUTABLE) && !(sb.st_mode & S_IXUSR)) return 0;
    return 1;
}
size_t FileSize(const char *filename) { struct stat sb; return stat(filename, &sb) ? 0 : (size_t)sb.st_size; }
int MakeDir(const char *folder) { return mkdir(folder, 0755) == 0 || errno == EEXIST; }
/* box_strdup is a macro for strdup in Box64's debug.h (no malloc hooks): nothing to define. */
void PersonalityAddrLimit32Bit(void) {}
int IsAddrElfOrFileMapped(uintptr_t addr) { (void)addr; return 0; }
void *GetSeg43Base(void *emu) { (void)emu; return NULL; }
void *GetSegmentBase(void *emu, uint32_t desc) {
    /* Selectors are never loaded by the game: the TCB base lives in segs_offs[_GS]. */
    (void)desc;
    return (void *)((x64emu_t *)emu)->segs_offs[_GS];
}
void *EmuFork(void *emu, int forktype) { (void)forktype; return emu; }
unsigned long real_getauxval(unsigned long type) { (void)type; return 0; }
void enter_critical_section(void) {}
void leave_critical_section(void) {}
void initCycleLog(box64context_t *context) { (void)context; }
void ShowNativeBT(int log_minimum) { (void)log_minimum; }
int PrintFunctionAddr(uintptr_t nextaddr, const char *text) { (void)nextaddr; (void)text; return 0; }
const char *getAddrFunctionName(uintptr_t addr) { (void)addr; return "guest"; }
elfheader_t *FindElfAddress(box64context_t *context, uintptr_t addr) { (void)context; (void)addr; return NULL; }
void ParseVolatileMetadata(char *filename, void *addr) { (void)filename; (void)addr; }
int VolatileRangesContains(uintptr_t addr) { (void)addr; return 0; }
int VolatileOpcodesHas(uintptr_t addr) { (void)addr; return 0; }
void DetectUnityPlayer(char *filename) { (void)filename; }
void AppendList(path_collection_t *collection, const char *list, int folder) { (void)collection; (void)list; (void)folder; }
void AddNewLibs(const char *list) { (void)list; }
void CheckExec(x64emu_t *emu, uintptr_t addr) { (void)emu; (void)addr; }
uintptr_t AddAltJump(bridge_t *bridge, uintptr_t addr, uintptr_t jump_to_addr) { (void)bridge; (void)addr; return jump_to_addr; }
int is_addr_unaligned(uintptr_t addr) { (void)addr; return 0; }
int is_addr_autosmc(uintptr_t addr) { (void)addr; return 0; }
void add_unaligned_address(uintptr_t addr) { (void)addr; }
int nUnalignedRange(uintptr_t start, size_t size) { (void)start; (void)size; return 0; }
void getUnalignedRange(uintptr_t start, size_t size, uintptr_t addrs[]) { (void)start; (void)size; (void)addrs; }
/* The game makes no Linux system calls (libkernel is the port's runtime). */
static void guest_fatal(x64emu_t *emu, const char *what, uintptr_t address);
void EmuX64Syscall(void *emu) { guest_fatal(emu, "syscall instruction", 0); }
void EmuX64Syscall_linux(void *emu) { guest_fatal(emu, "syscall instruction", 0); }
void EmuX86Syscall(void *emu) { guest_fatal(emu, "int 0x80", 0); }
void EmitSignal(x64emu_t *emu, int sig, void *addr, int code) {
    char what[64];
    snprintf(what, sizeof(what), "signal %d (code %d)", sig, code);
    guest_fatal(emu, what, (uintptr_t)addr);
}
void EmitInterruption(x64emu_t *emu, int num, void *addr) {
    char what[64];
    snprintf(what, sizeof(what), "interrupt 0x%x", num);
    guest_fatal(emu, what, (uintptr_t)addr);
}
void EmitDiv0(x64emu_t *emu, void *addr, int code) { (void)code; guest_fatal(emu, "integer division by zero", (uintptr_t)addr); }
void EmitWineInt(x64emu_t *emu, int num, void *addr) { EmitInterruption(emu, num, addr); }

/* ---- bridges ----
 * Layout (Box64's onebridge_t): CC 'S' 'C' <wrapper 8> <function 8> C3, padded to 32 bytes. */
typedef void (*wrapper_t)(x64emu_t *emu, uintptr_t fnc);
#pragma pack(push, 1)
typedef struct {
    uint8_t CC, S, C;
    wrapper_t w;
    uintptr_t f;
    uint8_t C3;
    uint8_t pad[32 - 3 - 8 - 8 - 1];
} bridge_t32;
#pragma pack(pop)
_Static_assert(sizeof(bridge_t32) == 32, "bridge size");

/* Native call with every argument register filled: x0-x7 from ints[0..7], v0-v7 from vecs,
 * `nstack` words from stack[] as stack arguments. out: x0, x1, v0 (128 bits). */
void bbx_native_call(const uint64_t *ints, const void *vecs, const uint64_t *stack, uint64_t nstack,
                     void *fn, uint64_t *out);
__asm__(
    ".text\n.balign 16\n.globl bbx_native_call\n.type bbx_native_call,%function\n"
    "bbx_native_call:\n"
    "  stp x29, x30, [sp, #-48]!\n"
    "  mov x29, sp\n"
    "  stp x19, x20, [sp, #16]\n"
    "  str x21, [sp, #32]\n"
    "  mov x19, x0\n  mov x20, x5\n  mov x21, x4\n"
    /* stack arguments: 16-byte aligned area of nstack words */
    "  add x9, x3, #1\n  and x9, x9, #~1\n  lsl x9, x9, #3\n  sub sp, sp, x9\n"
    "  mov x10, #0\n"
    "1: cmp x10, x3\n  b.hs 2f\n  ldr x11, [x2, x10, lsl #3]\n  str x11, [sp, x10, lsl #3]\n  add x10, x10, #1\n  b 1b\n"
    "2: ldp q0, q1, [x1]\n  ldp q2, q3, [x1, #32]\n  ldp q4, q5, [x1, #64]\n  ldp q6, q7, [x1, #96]\n"
    "  ldp x0, x1, [x19]\n  ldp x2, x3, [x19, #16]\n  ldp x4, x5, [x19, #32]\n  ldp x6, x7, [x19, #48]\n"
    "  blr x21\n"
    "  stp x0, x1, [x20]\n  str q0, [x20, #16]\n"
    "  mov sp, x29\n"
    "  ldp x19, x20, [sp, #16]\n  ldr x21, [sp, #32]\n"
    "  ldp x29, x30, [sp], #48\n  ret\n"
    ".size bbx_native_call, .-bbx_native_call\n");

/* The universal wrapper: SysV x86-64 -> AAPCS64 for scalar arguments and results. x86 passes
 * the 7th+ integer arguments on the stack ([rsp+8] at the bridge, after the return address);
 * AAPCS64 passes the 7th and 8th in x6/x7 and the rest on the stack, in the same order.
 * Variadic functions follow the same rules on Linux/Android AArch64. */
#define BBX_STACK_ARGS 10
static void w_universal(x64emu_t *emu, uintptr_t fnc) {
    uint64_t ints[8], out[4], *guest_stack = (uint64_t *)(R_RSP + 8);
    ints[0] = R_RDI; ints[1] = R_RSI; ints[2] = R_RDX; ints[3] = R_RCX;
    ints[4] = R_R8; ints[5] = R_R9; ints[6] = guest_stack[0]; ints[7] = guest_stack[1];
    _Static_assert(sizeof(emu->xmm[0]) == 16, "xmm size");
    bbx_native_call(ints, emu->xmm, guest_stack + 2, BBX_STACK_ARGS - 2, (void *)fnc, out);
    R_RAX = out[0];
    R_RDX = out[1];
    memcpy(&emu->xmm[0], &out[2], 16);
}

/* Signature-specific wrappers for the dynarec's direct native call path: isSimpleWrapper()
 * returns the call shape (>0: result in rax, <0: result in xmm0; |n|-1 vector arguments). */
#define SIMPLE_INT(n) static void w_int_##n(x64emu_t *emu, uintptr_t fnc) { w_universal(emu, fnc); }
#define SIMPLE_FP(n) static void w_fp_##n(x64emu_t *emu, uintptr_t fnc) { w_universal(emu, fnc); }
SIMPLE_INT(0) SIMPLE_INT(1) SIMPLE_INT(2) SIMPLE_INT(3) SIMPLE_INT(4) SIMPLE_INT(5) SIMPLE_INT(6) SIMPLE_INT(7) SIMPLE_INT(8)
SIMPLE_FP(0) SIMPLE_FP(1) SIMPLE_FP(2) SIMPLE_FP(3) SIMPLE_FP(4) SIMPLE_FP(5) SIMPLE_FP(6) SIMPLE_FP(7) SIMPLE_FP(8)
static const wrapper_t int_wrappers[9] = {w_int_0, w_int_1, w_int_2, w_int_3, w_int_4, w_int_5, w_int_6, w_int_7, w_int_8};
static const wrapper_t fp_wrappers[9] = {w_fp_0, w_fp_1, w_fp_2, w_fp_3, w_fp_4, w_fp_5, w_fp_6, w_fp_7, w_fp_8};
int isSimpleWrapper(wrapper_t fun) {
    for (int i = 0; i < 9; ++i) {
        if (fun == int_wrappers[i]) return 1 + i;
        if (fun == fp_wrappers[i]) return -(1 + i);
    }
    return 0;
}
int isRetX87Wrapper(wrapper_t fun) { (void)fun; return 0; }

/* Bridge pages: 64 KiB chunks below 4 GiB when possible (any address works). A small open
 * hash maps native functions to their bridge so each gets one. */
static pthread_mutex_t bridge_lock = PTHREAD_MUTEX_INITIALIZER;
static bridge_t32 *bridge_chunk;
static size_t bridge_used, bridge_capacity;
typedef struct { const void *fn; bridge_t32 *bridge; const char *name; } BridgeEntry;
static BridgeEntry *bridge_table;
static size_t bridge_table_size, bridge_count;
static uintptr_t bridge_ranges[64][2];
static int bridge_range_count;

static size_t hash_ptr(const void *p, size_t size) { uint64_t x = (uint64_t)(uintptr_t)p; x ^= x >> 33; x *= 0xff51afd7ed558ccdULL; x ^= x >> 33; return (size_t)x & (size - 1); }
static void table_insert(BridgeEntry e) {
    size_t i = hash_ptr(e.fn, bridge_table_size);
    while (bridge_table[i].fn) i = (i + 1) & (bridge_table_size - 1);
    bridge_table[i] = e;
}
static BridgeEntry *table_find(const void *fn) {
    if (!bridge_table_size) return NULL;
    size_t i = hash_ptr(fn, bridge_table_size);
    while (bridge_table[i].fn) {
        if (bridge_table[i].fn == fn) return &bridge_table[i];
        i = (i + 1) & (bridge_table_size - 1);
    }
    return NULL;
}
static bridge_t32 *new_bridge_slot(void) {
    if (bridge_used == bridge_capacity) {
        size_t size = 64 * 1024;
        void *p = MAP_FAILED;
#ifdef MAP_32BIT
        p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
#endif
        if (p == MAP_FAILED) p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (p == MAP_FAILED) { bbx_log("bbx: cannot map bridge memory\n"); abort(); }
        memset(p, 0xcc, size);
        bridge_chunk = p;
        bridge_used = 0;
        bridge_capacity = size / sizeof(bridge_t32);
        /* Writable in Box64's map too: the dynarec write-protects pages it translates (bridges
         * included) and gives write access back on the next write fault (bbx_handle_fault)
         * or unprotectDB; a page it believes read-only would stay read-only. */
        setProtection_box((uintptr_t)p, size, PROT_READ | PROT_WRITE | PROT_EXEC);
        if (bridge_range_count < 64) {
            bridge_ranges[bridge_range_count][0] = (uintptr_t)p;
            bridge_ranges[bridge_range_count][1] = (uintptr_t)p + size;
            ++bridge_range_count;
        }
    }
    return &bridge_chunk[bridge_used++];
}
static wrapper_t wrapper_for_signature(const char *sig) {
    /* "rFargs": r = result letter, args = letters after 'F'. */
    if (!sig || strlen(sig) < 2 || sig[1] != 'F') return w_universal;
    int ints = 0, fps = 0;
    for (const char *a = sig + 2; *a; ++a) {
        switch (*a) {
        case 'v': break;
        case 'f': case 'd': ++fps; break;
        case 'i': case 'u': case 'I': case 'U': case 'l': case 'L': case 'p': case 'c': case 'C': case 'w': case 'W': ++ints; break;
        default: return w_universal; /* structs, long double, varargs: universal path */
        }
    }
    if (ints > 6 || fps > 8) return w_universal;
    switch (sig[0]) {
    case 'f': case 'd': return fp_wrappers[fps];
    case 'v': case 'i': case 'u': case 'I': case 'U': case 'l': case 'L': case 'p': case 'c': case 'C': case 'w': case 'W':
        return int_wrappers[fps];
    default: return w_universal;
    }
}
static void *make_bridge(const void *fn, const char *name, const char *sig) {
    if (!fn) return NULL;
    pthread_mutex_lock(&bridge_lock);
    BridgeEntry *found = table_find(fn);
    if (found) { pthread_mutex_unlock(&bridge_lock); return found->bridge; }
    if ((bridge_count + 1) * 2 > bridge_table_size) {
        size_t old_size = bridge_table_size;
        BridgeEntry *old = bridge_table;
        bridge_table_size = old_size ? old_size * 2 : 1024;
        bridge_table = calloc(bridge_table_size, sizeof(*bridge_table));
        for (size_t i = 0; i < old_size; ++i) if (old[i].fn) table_insert(old[i]);
        free(old);
    }
    bridge_t32 *b = new_bridge_slot();
    unprotectDB((uintptr_t)b, sizeof(*b), 1); /* the page may hold translated bridges */
    b->CC = 0xcc; b->S = 'S'; b->C = 'C';
    b->w = wrapper_for_signature(sig);
    b->f = (uintptr_t)fn;
    b->C3 = 0xc3;
    __builtin___clear_cache((char *)b, (char *)(b + 1));
    table_insert((BridgeEntry){fn, b, name});
    ++bridge_count;
    pthread_mutex_unlock(&bridge_lock);
    return b;
}
void *bbx_bridge(const void *native_fn, const char *name) { return make_bridge(native_fn, name, NULL); }
void *bbx_bridge_sig(const void *native_fn, const char *name, const char *signature) { return make_bridge(native_fn, name, signature); }
static int in_bridges(uintptr_t a) {
    for (int i = 0; i < bridge_range_count; ++i)
        if (a >= bridge_ranges[i][0] && a < bridge_ranges[i][1]) return 1;
    return 0;
}
void *bbx_bridge_target(uintptr_t address) {
    if (!in_bridges(address)) return NULL;
    bridge_t32 *b = (bridge_t32 *)(address & ~(uintptr_t)31);
    return (b->CC == 0xcc && b->S == 'S' && b->C == 'C' && b->w) ? (void *)b->f : NULL;
}
const char *GetBridgeName(void *p) {
    void *fn = bbx_bridge_target((uintptr_t)p);
    if (!fn) return NULL;
    BridgeEntry *e = table_find(fn);
    return e ? e->name : NULL;
}
const char *GetNativeName(void *p, int lib) { (void)lib; const char *n = GetBridgeName(p); return n ? n : "native"; }
void *GetNativeFnc(uintptr_t fnc) { return bbx_bridge_target(fnc); }
int IsNativeCall(uintptr_t addr, int is32bits, uintptr_t *calladdress, uint16_t *retn) {
    if (is32bits) return 0;
    /* jmp [rip+disp32] (PLT-style) to a bridge */
    if (*(uint8_t *)addr == 0xff && *(uint8_t *)(addr + 1) == 0x25 && getProtection(addr) & PROT_READ) {
        uintptr_t slot = addr + 6 + *(int32_t *)(addr + 2);
        if (!(getProtection(slot) & PROT_READ)) return 0;
        addr = *(uintptr_t *)slot;
    }
    if (!in_bridges(addr)) return 0;
    bridge_t32 *b = (bridge_t32 *)addr;
    if (b->CC == 0xcc && b->S == 'S' && b->C == 'C' && b->w) {
        if (retn) *retn = 0;
        if (calladdress) *calladdress = addr + 1;
        return 1;
    }
    return 0;
}
/* int3 at a bridge (the dynarec's non-direct path and the interpreter). */
void EmuInt3(void *e, void *a) {
    x64emu_t *emu = e;
    uintptr_t *addr = a;
    bridge_t32 *b = (bridge_t32 *)(*addr - 1);
    if (b->CC == 0xcc && b->S == 'S' && b->C == 'C') {
        if (!b->w) { /* exit bridge */
            R_RIP = (uintptr_t)&b->C3;
            emu->quit = 1;
            return;
        }
        RESET_FLAGS(emu);
        R_RIP = (uintptr_t)&b->C3;
        b->w(emu, b->f);
        return;
    }
    guest_fatal(emu, "int3", *addr - 1);
}

/* ---- threads and calls ---- */
static __thread x64emu_t *thread_emu;
static __thread void *thread_stack_base;
static __thread size_t thread_stack_size;
static __thread int thread_depth;
static pthread_key_t emu_key;

static void free_thread_emu(void *p) {
    x64emu_t *emu = p;
    if (!emu) return;
    if (emu->stack2free) munmap(emu->stack2free, emu->size_stack);
    free(emu);
}
void bbx_thread_stack(void *base, size_t size) { thread_stack_base = base; thread_stack_size = size; }
static x64emu_t *current_emu(void) {
    x64emu_t *emu = thread_emu;
    if (emu) return emu;
    void *stack = thread_stack_base;
    size_t size = thread_stack_size;
    int own = 0;
    if (!stack) {
        size = 1024 * 1024;
        stack = MAP_FAILED;
#ifdef MAP_32BIT
        stack = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
#endif
        if (stack == MAP_FAILED) stack = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (stack == MAP_FAILED) { bbx_log("bbx: cannot map a guest stack\n"); abort(); }
        own = 1;
    }
    emu = calloc(1, sizeof(x64emu_t));
    emu->context = my_context;
    for (int i = 0; i < 16; ++i) emu->sbiidx[i] = &emu->regs[i];
    emu->sbiidx[4] = &emu->zero;
    emu->x64emu_parity_tab = parity_tab;
    emu->eflags.x64 = 0x202;
    emu->stack2free = own ? stack : NULL;
    emu->init_stack = stack;
    emu->size_stack = (uint32_t)size;
    R_RSP = ((uintptr_t)stack + size - 64) & ~(uintptr_t)15;
    emu->segs[_CS] = 0x33;
    emu->segs[_DS] = emu->segs[_ES] = emu->segs[_SS] = 0x2b;
    emu->segs[_FS] = 0;
    emu->segs[_GS] = 0;
    for (int i = 0; i < 16; i++) emu->segldt[i].limit = (uintptr_t)-1LL;
    memcpy(emu->seggdt, my_context->seggdt, sizeof(emu->seggdt));
    reset_fpu(emu);
    emu->mxcsr.x32 = 0x1f80;
    emu->flags.need_jmpbuf = 1;
    thread_emu = emu;
    pthread_setspecific(emu_key, emu);
    return emu;
}
void bbx_thread_exit(void) {
    x64emu_t *emu = thread_emu;
    thread_emu = NULL;
    pthread_setspecific(emu_key, NULL);
    free_thread_emu(emu);
}
void bbx_set_gs_base(uint64_t base) { x64emu_t *emu = current_emu(); emu->segs[_GS] = 0; emu->segs_offs[_GS] = base; }
void bbx_set_fs_base(uint64_t base) { x64emu_t *emu = current_emu(); emu->segs[_FS] = 0; emu->segs_offs[_FS] = base; }
uint64_t bbx_get_gs_base(void) { return current_emu()->segs_offs[_GS]; }
int bbx_in_guest(void) { return thread_depth > 0; }

static uint64_t call_with(x64emu_t *emu, uintptr_t address, int nargs, const uint64_t *args) {
    static const int order[6] = {_DI, _SI, _DX, _CX, _R8, _R9};
    uint64_t saved_rsp = R_RSP, saved_rbp = R_RBP;
    int stackn = nargs > 6 ? nargs - 6 : 0;
    R_RSP -= 8 * (size_t)(stackn + (stackn & 1));
    R_RSP &= ~(uint64_t)15;
    for (int i = 0; i < nargs; ++i) {
        if (i < 6) emu->regs[order[i]].q[0] = args[i];
        else ((uint64_t *)R_RSP)[i - 6] = args[i];
    }
    int old_quit = emu->quit;
    emu->quit = 0;
    ++thread_depth;
    DynaCall(emu, address, 0);
    --thread_depth;
    emu->quit = old_quit;
    uint64_t result = R_RAX;
    R_RSP = saved_rsp;
    R_RBP = saved_rbp;
    return result;
}
uint64_t bbx_call(uintptr_t address, int nargs, const uint64_t *args) { return call_with(current_emu(), address, nargs, args); }
uint64_t bbx_call0(uintptr_t a) { return bbx_call(a, 0, NULL); }
uint64_t bbx_call1(uintptr_t a, uint64_t a0) { uint64_t v[1] = {a0}; return bbx_call(a, 1, v); }
uint64_t bbx_call2(uintptr_t a, uint64_t a0, uint64_t a1) { uint64_t v[2] = {a0, a1}; return bbx_call(a, 2, v); }
uint64_t bbx_call3(uintptr_t a, uint64_t a0, uint64_t a1, uint64_t a2) { uint64_t v[3] = {a0, a1, a2}; return bbx_call(a, 3, v); }
uint64_t bbx_call4(uintptr_t a, uint64_t a0, uint64_t a1, uint64_t a2, uint64_t a3) { uint64_t v[4] = {a0, a1, a2, a3}; return bbx_call(a, 4, v); }
uint64_t bbx_call_on_stack(uintptr_t address, uint64_t a0, uint64_t a1, void *stack_top) {
    x64emu_t *emu = current_emu();
    uint64_t saved = R_RSP;
    R_RSP = (uintptr_t)stack_top & ~(uintptr_t)15;
    uint64_t v[2] = {a0, a1};
    uint64_t r = call_with(emu, address, 2, v);
    R_RSP = saved;
    return r;
}

/* ---- code memory ---- */
void bbx_code_mapped(void *address, size_t size, int prot) {
    setProtection_mmap((uintptr_t)address, size, (uint32_t)prot);
}
void bbx_code_unmapped(void *address, size_t size) {
    cleanDBFromAddressRange((uintptr_t)address, size, 1);
    freeProtection((uintptr_t)address, size);
}
void bbx_code_written(void *address, size_t size) {
    unprotectDB((uintptr_t)address, size, 1);
    cleanDBFromAddressRange((uintptr_t)address, size, 0);
}
int bbx_handle_fault(void *fault_address, void *ucontext) {
    (void)ucontext;
    uintptr_t addr = (uintptr_t)fault_address;
    uint32_t prot = getProtection(addr);
    if (prot & PROT_DYNAREC) {
        unprotectDB(addr, 1, 1);
        return 1;
    }
    return 0;
}
uintptr_t bbx_guest_pc(uintptr_t native_pc) {
    dynablock_t *db = FindDynablockFromNativeAddress((void *)native_pc);
    return db ? getX64Address(db, native_pc) : 0;
}
void bbx_dump_thread(int fd) {
    x64emu_t *emu = thread_emu;
    if (!emu) return;
    dprintf(fd, "  guest rip=%016" PRIx64 " rsp=%016" PRIx64 " rbp=%016" PRIx64 "\n"
                "  rax=%016" PRIx64 " rbx=%016" PRIx64 " rcx=%016" PRIx64 " rdx=%016" PRIx64 "\n"
                "  rsi=%016" PRIx64 " rdi=%016" PRIx64 " r8=%016" PRIx64 " r9=%016" PRIx64 " gs=%016" PRIx64 "\n",
            (uint64_t)R_RIP, (uint64_t)R_RSP, (uint64_t)R_RBP, (uint64_t)R_RAX, (uint64_t)R_RBX, (uint64_t)R_RCX,
            (uint64_t)R_RDX, (uint64_t)R_RSI, (uint64_t)R_RDI, (uint64_t)R_R8, (uint64_t)R_R9, (uint64_t)emu->segs_offs[_GS]);
}

static void guest_fatal(x64emu_t *emu, const char *what, uintptr_t address) {
    bbx_log("Guest fault: %s at guest rip %p (address %p)\n", what, emu ? (void *)R_RIP : NULL, (void *)address);
    if (emu) {
        bbx_log("  rax=%016" PRIx64 " rbx=%016" PRIx64 " rcx=%016" PRIx64 " rdx=%016" PRIx64 "\n"
                "  rsi=%016" PRIx64 " rdi=%016" PRIx64 " rbp=%016" PRIx64 " rsp=%016" PRIx64 "\n",
                (uint64_t)R_RAX, (uint64_t)R_RBX, (uint64_t)R_RCX, (uint64_t)R_RDX,
                (uint64_t)R_RSI, (uint64_t)R_RDI, (uint64_t)R_RBP, (uint64_t)R_RSP);
        if (R_RIP) {
            uint8_t *p = (uint8_t *)R_RIP;
            if (getProtection((uintptr_t)p) & PROT_READ)
                bbx_log("  bytes: %02x %02x %02x %02x %02x %02x %02x %02x\n", p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
        }
    }
    fflush(NULL);
    _exit(3);
}

/* ---- init ---- */
void bbx_configure(int dynarec, int strongmem, int log_level) {
    cfg_dynarec = dynarec;
    cfg_strongmem = strongmem;
    cfg_log = log_level;
}
static void init_mutex(void *m) {
#ifdef USE_CUSTOM_MUTEX
    native_lock_store(m, 0);
#else
    pthread_mutexattr_t attr;
    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_ERRORCHECK);
    pthread_mutex_init((pthread_mutex_t *)m, &attr);
    pthread_mutexattr_destroy(&attr);
#endif
}
int bbx_init(void) {
    static int done;
    if (done) return 0;
    done = 1;
    box64_pagesize = (uintptr_t)sysconf(_SC_PAGESIZE);
    LoadEnvVariables();
    InitializeEnvFiles();
    InitializeSystemInfo();
    if (cfg_log >= 0) SET_BOX64ENV(log, cfg_log);
    if (!DetectHostCpuFeatures()) {
        bbx_log("bbx: host CPU lacks what the dynarec needs; using the interpreter\n");
        cfg_dynarec = 0;
    }
    SET_BOX64ENV(dynarec, cfg_dynarec);
    if (cfg_strongmem >= 0) SET_BOX64ENV(dynarec_strongmem, cfg_strongmem);
    /* PS4 (AMD Jaguar): SSE4.2 and AVX, no AVX2. */
    SET_BOX64ENV(avx, 1);
    SET_BOX64ENV(sse42, 1);
    if (BOX64ENV(log) >= 1) PrintEnvVariables(&box64env, LOG_INFO);
    init_custommem_helper(my_context);
#ifdef DYNAREC
    my_context->db_sizes = rbtree_init("db_sizes");
#endif
    init_mutex(&my_context->mutex_trace);
    init_mutex(&my_context->mutex_tls);
    init_mutex(&my_context->mutex_thread);
    init_mutex(&my_context->mutex_bridge);
    init_mutex(&my_context->mutex_dyndump);
#ifndef DYNAREC
    init_mutex(&my_context->mutex_lock);
#endif
    for (int i = 0; i < 16; i++) my_context->seggdt[i].limit = (uintptr_t)-1LL;
    my_context->seggdt[10].present = 1;
    my_context->seggdt[8].present = 1;
    my_context->seggdt[6].present = 1;
    my_context->seggdt[5].present = 1;
    my_context->seggdt[4].present = 1;
    my_context->seggdt[4].is32bits = 1;
    pthread_key_create(&emu_key, free_thread_emu);
    /* exit bridge: wrapper 0 */
    pthread_mutex_lock(&bridge_lock);
    bridge_t32 *b = new_bridge_slot();
    pthread_mutex_unlock(&bridge_lock);
    b->CC = 0xcc; b->S = 'S'; b->C = 'C'; b->w = NULL; b->f = 0; b->C3 = 0xc3;
    my_context->exit_bridge = (uintptr_t)b;
    InitX64Trace(my_context);
    return 0;
}
