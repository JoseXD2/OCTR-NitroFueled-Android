/* Checks android/app/bb_lzma.c against the game's own LzmaDec_DecodeReal2 (x86-64 host only):
 * the function's bytes are copied out of eboot.elf and run natively, both decoders get the same
 * state for every call and must leave the same state, dictionary and probabilities; the output
 * must match the original data.
 *
 *   cc -O2 -o test_lzma test_lzma.c ../../app/bb_lzma.c
 *   xz --format=lzma -k -c data > data.lzma
 *   ./test_lzma eboot.elf data.lzma data [seed] */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>

int bb_lzma_decode_real2(void *decoder, size_t limit, const uint8_t *bufLimit);

#define FN_OFFSET 0x15835d0u
#define FN_SIZE (0x1584ed0u - 0x15835d0u)
#define TEXT_FILE_OFFSET 0x4000u /* first LOAD: vaddr 0 at file offset 0x4000 */
#define kMatchSpecLenStart 274
#define Literal 1846

typedef struct {
    unsigned lc, lp, pb;
    uint32_t dicSize;
    uint16_t *probs;
    uint8_t *dic;
    const uint8_t *buf;
    uint32_t range, code;
    size_t dicPos, dicBufSize;
    uint32_t processedPos, checkDicSize;
    unsigned state;
    uint32_t reps[4];
    unsigned remainLen;
} Dec;

static uint8_t *read_file(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); exit(2); }
    fseek(f, 0, SEEK_END);
    *size = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc(*size + 64);
    if (fread(data, 1, *size, f) != *size) { perror(path); exit(2); }
    memset(data + *size, 0, 64);
    fclose(f);
    return data;
}

static uint64_t rng = 88172645463325252ull;
static uint64_t next(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return rng; }

static void write_rem(Dec *p, size_t limit) { /* LzmaDec_WriteRem (the caller does it first) */
    if (p->remainLen != 0 && p->remainLen < kMatchSpecLenStart) {
        unsigned len = p->remainLen;
        if (limit - p->dicPos < len) len = (unsigned)(limit - p->dicPos);
        if (p->checkDicSize == 0 && p->dicSize - p->processedPos <= len) p->checkDicSize = p->dicSize;
        p->processedPos += len;
        p->remainLen -= len;
        while (len--) {
            p->dic[p->dicPos] = p->dic[(p->dicPos - p->reps[0]) + ((p->dicPos < p->reps[0]) ? p->dicBufSize : 0)];
            p->dicPos++;
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: test_lzma eboot.elf data.lzma data [seed]\n"); return 2; }
    if (argc > 4) rng ^= strtoull(argv[4], NULL, 0) * 0x9E3779B97F4A7C15ull;
    size_t elf_size, in_size, ref_size;
    uint8_t *elf = read_file(argv[1], &elf_size);
    uint8_t *in = read_file(argv[2], &in_size);
    uint8_t *ref = read_file(argv[3], &ref_size);
    uint8_t *code = mmap(NULL, FN_SIZE + 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    memcpy(code, elf + TEXT_FILE_OFFSET + FN_OFFSET, FN_SIZE);
    if (memcmp(code, "\x55\x41\x57\x41\x56\x41\x55\x41\x54\x53\x48\x83\xec\x30", 14)) { fprintf(stderr, "unexpected prologue\n"); return 2; }
    int (*game)(void *, size_t, const uint8_t *) = (int (*)(void *, size_t, const uint8_t *))code;

    unsigned d = in[0];
    Dec a = {0};
    a.lc = d % 9; d /= 9; a.lp = d % 5; a.pb = d / 5;
    a.dicSize = in[1] | in[2] << 8 | in[3] << 16 | (uint32_t)in[4] << 24;
    if (a.dicSize < 4096) a.dicSize = 4096;
    const size_t num_probs = Literal + ((size_t)0x300 << (a.lc + a.lp));
    /* a ring dictionary smaller than the data exercises the wrap-around paths */
    a.dicBufSize = a.dicSize < ref_size ? a.dicSize : ref_size + 1; /* small dictionaries wrap around */
    a.probs = malloc(num_probs * 2);
    a.dic = calloc(1, a.dicBufSize);
    for (size_t i = 0; i < num_probs; ++i) a.probs[i] = 1024;
    const uint8_t *rc = in + 13;
    a.code = (uint32_t)rc[1] << 24 | rc[2] << 16 | rc[3] << 8 | rc[4];
    a.range = 0xFFFFFFFF;
    a.buf = rc + 5;
    a.reps[0] = a.reps[1] = a.reps[2] = a.reps[3] = 1;
    const uint8_t *end = in + in_size;

    Dec b = a;
    b.probs = malloc(num_probs * 2);
    b.dic = malloc(a.dicBufSize);
    uint8_t *out = malloc(ref_size + 4096);
    size_t out_size = 0, calls = 0;
    while (a.remainLen != kMatchSpecLenStart && out_size < ref_size) {
        if (a.dicPos == a.dicBufSize) a.dicPos = 0;
        const size_t start = a.dicPos;
        size_t limit = a.dicPos + 1 + (next() % ((next() & 3) ? 64 : 65536));
        if (limit > a.dicBufSize) limit = a.dicBufSize;
        size_t avail = (size_t)(end - a.buf);
        const uint8_t *bufLimit = a.buf + ((next() & 1) ? avail : 1 + next() % (avail + 1));
        if (bufLimit > end) bufLimit = end;
        write_rem(&a, limit);
        if (a.dicPos < limit && a.buf < bufLimit && a.remainLen < kMatchSpecLenStart) {
            uint16_t *probs_b = b.probs;
            uint8_t *dic_b = b.dic;
            b = a;
            b.probs = probs_b;
            b.dic = dic_b;
            memcpy(b.probs, a.probs, num_probs * 2);
            memcpy(b.dic, a.dic, a.dicBufSize);
            b.buf = a.buf; /* same input buffer */
            const int ra = game(&a, limit, bufLimit);
            const int rb = bb_lzma_decode_real2(&b, limit, bufLimit);
            ++calls;
            uint16_t *pa = a.probs, *pb2 = b.probs;
            uint8_t *da = a.dic, *db = b.dic;
            b.probs = a.probs;
            b.dic = a.dic;
            if (ra != rb || memcmp(&a, &b, sizeof a) || memcmp(pa, pb2, num_probs * 2) || memcmp(da, db, a.dicBufSize)) {
                fprintf(stderr, "MISMATCH at call %zu: game %d native %d (dicPos %zu/%zu buf +%td/%td)\n", calls, ra,
                        rb, a.dicPos, b.dicPos, a.buf - in, b.buf - in);
                return 1;
            }
            b.probs = pb2;
            b.dic = db;
            if (ra) { fprintf(stderr, "decoder error at call %zu\n", calls); return 1; }
        }
        memcpy(out + out_size, a.dic + start, a.dicPos - start);
        out_size += a.dicPos - start;
        if (a.buf >= end && a.dicPos < limit && a.remainLen == 0) break;
    }
    if (out_size < ref_size || memcmp(out, ref, ref_size)) {
        fprintf(stderr, "OUTPUT differs (%zu of %zu bytes)\n", out_size, ref_size);
        return 1;
    }
    printf("OK: %zu bytes, %zu calls, lc=%u lp=%u pb=%u dicBufSize=%zu\n", ref_size, calls, a.lc, a.lp, a.pb, a.dicBufSize);
    return 0;
}
