#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include "bb_box64.h"
static uint64_t native_add1(uint64_t x) { return x + 1; }
static double native_half(double d) { return d / 2; }
static uint64_t native_many(uint64_t a,uint64_t b,uint64_t c,uint64_t d,uint64_t e,uint64_t f,uint64_t g,uint64_t h,uint64_t i) { return a+2*b+3*c+4*d+5*e+6*f+7*g+8*h+9*i; }
static uint64_t guest_cb;
static uint64_t native_calls_back(uint64_t x) { return bbx_call1(guest_cb, x) * 10; }
static void put(uint8_t **p, const void *b, size_t n) { memcpy(*p, b, n); *p += n; }
static void put64(uint8_t **p, uint64_t v) { memcpy(*p, &v, 8); *p += 8; }
int main(void) {
    bbx_init();
    uint8_t *code = mmap((void*)0x800000000ULL, 0x10000, PROT_READ|PROT_WRITE|PROT_EXEC, MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED, -1, 0);
    uint8_t *p = code;
    /* f1(a,b) = half(add1(gs:[0]+a+b)) */
    put(&p, "\x55\x48\x89\xe5", 4);
    put(&p, "\x65\x48\x8b\x04\x25\x00\x00\x00\x00", 9);
    put(&p, "\x48\x01\xf8\x48\x01\xf0\x48\x89\xc7", 9);
    put(&p, "\x48\xb8", 2); put64(&p, (uint64_t)bbx_bridge_sig(native_add1, "add1", "LFL"));
    put(&p, "\xff\xd0", 2);
    put(&p, "\xf2\x48\x0f\x2a\xc0", 5);
    put(&p, "\x48\xb8", 2); put64(&p, (uint64_t)bbx_bridge_sig(native_half, "half", "dFd"));
    put(&p, "\xff\xd0", 2);
    put(&p, "\xf2\x48\x0f\x2c\xc0", 5);
    put(&p, "\x5d\xc3", 2);
    /* f2(n) = sum 1..n loop */
    uint8_t *f2 = p;
    put(&p, "\x31\xc0\x48\x85\xff\x74\x08\x48\x01\xf8\x48\xff\xcf\x75\xf8\xc3", 16);
    /* f3(): many(1..9) via universal bridge (9 args: 3 on stack) */
    uint8_t *f3 = p;
    put(&p, "\x48\x83\xec\x08\x6a\x09\x6a\x08\x6a\x07", 10);  /* align, push 9, 8, 7 */
    put(&p, "\xbf\x01\x00\x00\x00\xbe\x02\x00\x00\x00\xba\x03\x00\x00\x00\xb9\x04\x00\x00\x00", 20);
    put(&p, "\x41\xb8\x05\x00\x00\x00\x41\xb9\x06\x00\x00\x00", 12);
    put(&p, "\x48\xb8", 2); put64(&p, (uint64_t)bbx_bridge(native_many, "many"));
    put(&p, "\xff\xd0\x48\x83\xc4\x20\xc3", 7); /* call, pop 4 words */
    /* f4(x): native_calls_back(x) + 1 ; guest_cb = f5(x) = x*3 */
    uint8_t *f4 = p;
    put(&p, "\x48\x83\xec\x08", 4);
    put(&p, "\x48\xb8", 2); put64(&p, (uint64_t)bbx_bridge_sig(native_calls_back, "callsback", "LFL"));
    put(&p, "\xff\xd0\x48\xff\xc0\x48\x83\xc4\x08\xc3", 10);
    uint8_t *f5 = p;
    put(&p, "\x48\x8d\x04\x7f\xc3", 5);            /* lea rax,[rdi+rdi*2]; ret */
    guest_cb = (uint64_t)f5;
    bbx_code_mapped(code, 0x10000, PROT_READ|PROT_EXEC);
    static uint64_t tcb[4] = {100};
    bbx_set_gs_base((uint64_t)tcb);
    printf("f1 = %llu (expect 54)\n", (unsigned long long)bbx_call2((uintptr_t)code, 3, 4));
    printf("f2 = %llu (expect 500000500000)\n", (unsigned long long)bbx_call1((uintptr_t)f2, 1000000));
    printf("f3 = %llu (expect 285)\n", (unsigned long long)bbx_call0((uintptr_t)f3));
    printf("f4 = %llu (expect 61)\n", (unsigned long long)bbx_call1((uintptr_t)f4, 2));
    for (int i = 0; i < 3; ++i) printf("f1 again = %llu\n", (unsigned long long)bbx_call2((uintptr_t)code, 3, 4));
    return 0;
}
