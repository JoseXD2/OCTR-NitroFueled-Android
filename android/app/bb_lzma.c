/* SacredFire Android: a native LzmaDec_DecodeReal2 for the game's LZMA decoder.
 *
 * The game decompresses its archives with LZMA SDK 9.20's decoder (public domain, Igor Pavlov).
 * Through Box64 that is most of the loading time, so its inner function (0x15835d0 in the 1.21
 * eboot: LzmaDec_DecodeReal2 with LzmaDec_DecodeReal and LzmaDec_WriteRem inlined) is replaced by
 * this native copy that works on the game's own CLzmaDec. Written after the SDK's algorithm and
 * checked against the game's x86 code (android/test/lzma).
 *
 * BB_NATIVE_LZMA=0 keeps the game's code. */
#include <stddef.h>
#include <stdint.h>

typedef uint16_t CLzmaProb;
typedef struct {
    unsigned lc, lp, pb;
    uint32_t dicSize;
} BbLzmaProps;
/* LZMA SDK 9.20 CLzmaDec (x86-64 and ARM64 alike). */
typedef struct {
    BbLzmaProps prop;            /* 0x00 */
    CLzmaProb *probs;            /* 0x10 */
    uint8_t *dic;                /* 0x18 */
    const uint8_t *buf;          /* 0x20 */
    uint32_t range, code;        /* 0x28 0x2c */
    size_t dicPos;               /* 0x30 */
    size_t dicBufSize;           /* 0x38 */
    uint32_t processedPos;       /* 0x40 */
    uint32_t checkDicSize;       /* 0x44 */
    unsigned state;              /* 0x48 */
    uint32_t reps[4];            /* 0x4c */
    unsigned remainLen;          /* 0x5c */
} BbLzmaDec;
_Static_assert(offsetof(BbLzmaDec, remainLen) == 0x5c, "CLzmaDec layout");

#define kNumTopBits 24
#define kTopValue ((uint32_t)1 << kNumTopBits)
#define kNumBitModelTotalBits 11
#define kBitModelTotal (1 << kNumBitModelTotalBits)
#define kNumMoveBits 5

#define NORMALIZE if (range < kTopValue) { range <<= 8; code = (code << 8) | (*buf++); }
#define IF_BIT_0(p) ttt = *(p); NORMALIZE; bound = (range >> kNumBitModelTotalBits) * ttt; if (code < bound)
#define UPDATE_0(p) range = bound; *(p) = (CLzmaProb)(ttt + ((kBitModelTotal - ttt) >> kNumMoveBits));
#define UPDATE_1(p) range -= bound; code -= bound; *(p) = (CLzmaProb)(ttt - (ttt >> kNumMoveBits));
#define GET_BIT2(p, i, A0, A1) IF_BIT_0(p) { UPDATE_0(p); i = (i + i); A0; } else { UPDATE_1(p); i = (i + i) + 1; A1; }
#define GET_BIT(p, i) GET_BIT2(p, i, ;, ;)
#define TREE_GET_BIT(probs, i) { GET_BIT((probs + i), i); }
#define TREE_DECODE(probs, limit, i) { i = 1; do { TREE_GET_BIT(probs, i); } while (i < limit); i -= limit; }
#define TREE_6_DECODE(probs, i) TREE_DECODE(probs, (1 << 6), i)

#define kNumPosBitsMax 4
#define kNumPosStatesMax (1 << kNumPosBitsMax)
#define kLenNumLowBits 3
#define kLenNumLowSymbols (1 << kLenNumLowBits)
#define kLenNumMidBits 3
#define kLenNumMidSymbols (1 << kLenNumMidBits)
#define kLenNumHighBits 8
#define kLenNumHighSymbols (1 << kLenNumHighBits)
#define LenChoice 0
#define LenChoice2 (LenChoice + 1)
#define LenLow (LenChoice2 + 1)
#define LenMid (LenLow + (kNumPosStatesMax << kLenNumLowBits))
#define LenHigh (LenMid + (kNumPosStatesMax << kLenNumMidBits))
#define kNumLenProbs (LenHigh + kLenNumHighSymbols)

#define LZMA_LIT_SIZE 0x300
#define kNumStates 12
#define kNumLitStates 7
#define kStartPosModelIndex 4
#define kEndPosModelIndex 14
#define kNumFullDistances (1 << (kEndPosModelIndex >> 1))
#define kNumPosSlotBits 6
#define kNumLenToPosStates 4
#define kNumAlignBits 4
#define kAlignTableSize (1 << kNumAlignBits)
#define kMatchMinLen 2
#define kMatchSpecLenStart (kMatchMinLen + kLenNumLowSymbols + kLenNumMidSymbols + kLenNumHighSymbols)

#define IsMatch 0
#define IsRep (IsMatch + (kNumStates << kNumPosBitsMax))
#define IsRepG0 (IsRep + kNumStates)
#define IsRepG1 (IsRepG0 + kNumStates)
#define IsRepG2 (IsRepG1 + kNumStates)
#define IsRep0Long (IsRepG2 + kNumStates)
#define PosSlot (IsRep0Long + (kNumStates << kNumPosBitsMax))
#define SpecPos (PosSlot + (kNumLenToPosStates << kNumPosSlotBits))
#define Align (SpecPos + kNumFullDistances - kEndPosModelIndex)
#define LenCoder (Align + kAlignTableSize)
#define RepLenCoder (LenCoder + kNumLenProbs)
#define Literal (RepLenCoder + kNumLenProbs)
_Static_assert(LenCoder == 818 && RepLenCoder == 1332 && Literal == 1846, "LZMA 9.20 probabilities");

#define SZ_OK 0
#define SZ_ERROR_DATA 1

static int decode_real(BbLzmaDec *p, size_t limit, const uint8_t *bufLimit) {
    CLzmaProb *probs = p->probs;
    unsigned state = p->state;
    uint32_t rep0 = p->reps[0], rep1 = p->reps[1], rep2 = p->reps[2], rep3 = p->reps[3];
    unsigned pbMask = ((unsigned)1 << (p->prop.pb)) - 1;
    unsigned lpMask = ((unsigned)1 << (p->prop.lp)) - 1;
    unsigned lc = p->prop.lc;

    uint8_t *dic = p->dic;
    size_t dicBufSize = p->dicBufSize;
    size_t dicPos = p->dicPos;
    uint32_t processedPos = p->processedPos;
    uint32_t checkDicSize = p->checkDicSize;
    unsigned len = 0;

    const uint8_t *buf = p->buf;
    uint32_t range = p->range;
    uint32_t code = p->code;

    do {
        CLzmaProb *prob;
        uint32_t bound;
        unsigned ttt;
        unsigned posState = processedPos & pbMask;

        prob = probs + IsMatch + (state << kNumPosBitsMax) + posState;
        IF_BIT_0(prob) {
            unsigned symbol;
            UPDATE_0(prob);
            prob = probs + Literal;
            if (checkDicSize != 0 || processedPos != 0)
                prob += (LZMA_LIT_SIZE * (((processedPos & lpMask) << lc) +
                                          (dic[(dicPos == 0 ? dicBufSize : dicPos) - 1] >> (8 - lc))));
            if (state < kNumLitStates) {
                state -= (state < 4) ? state : 3;
                symbol = 1;
                do { GET_BIT(prob + symbol, symbol) } while (symbol < 0x100);
            } else {
                unsigned matchByte = dic[(dicPos - rep0) + ((dicPos < rep0) ? dicBufSize : 0)];
                unsigned offs = 0x100;
                state -= (state < 10) ? 3 : 6;
                symbol = 1;
                do {
                    unsigned bit;
                    CLzmaProb *probLit;
                    matchByte <<= 1;
                    bit = (matchByte & offs);
                    probLit = prob + offs + bit + symbol;
                    GET_BIT2(probLit, symbol, offs &= ~bit, offs &= bit)
                } while (symbol < 0x100);
            }
            dic[dicPos++] = (uint8_t)symbol;
            processedPos++;
            continue;
        } else {
            UPDATE_1(prob);
            prob = probs + IsRep + state;
            IF_BIT_0(prob) {
                UPDATE_0(prob);
                state += kNumStates;
                prob = probs + LenCoder;
            } else {
                UPDATE_1(prob);
                if (checkDicSize == 0 && processedPos == 0) return SZ_ERROR_DATA;
                prob = probs + IsRepG0 + state;
                IF_BIT_0(prob) {
                    UPDATE_0(prob);
                    prob = probs + IsRep0Long + (state << kNumPosBitsMax) + posState;
                    IF_BIT_0(prob) {
                        UPDATE_0(prob);
                        dic[dicPos] = dic[(dicPos - rep0) + ((dicPos < rep0) ? dicBufSize : 0)];
                        dicPos++;
                        processedPos++;
                        state = state < kNumLitStates ? 9 : 11;
                        continue;
                    }
                    UPDATE_1(prob);
                } else {
                    uint32_t distance;
                    UPDATE_1(prob);
                    prob = probs + IsRepG1 + state;
                    IF_BIT_0(prob) {
                        UPDATE_0(prob);
                        distance = rep1;
                    } else {
                        UPDATE_1(prob);
                        prob = probs + IsRepG2 + state;
                        IF_BIT_0(prob) {
                            UPDATE_0(prob);
                            distance = rep2;
                        } else {
                            UPDATE_1(prob);
                            distance = rep3;
                            rep3 = rep2;
                        }
                        rep2 = rep1;
                    }
                    rep1 = rep0;
                    rep0 = distance;
                }
                state = state < kNumLitStates ? 8 : 11;
                prob = probs + RepLenCoder;
            }
            {
                unsigned lim, offset;
                CLzmaProb *probLen = prob + LenChoice;
                IF_BIT_0(probLen) {
                    UPDATE_0(probLen);
                    probLen = prob + LenLow + (posState << kLenNumLowBits);
                    offset = 0;
                    lim = (1 << kLenNumLowBits);
                } else {
                    UPDATE_1(probLen);
                    probLen = prob + LenChoice2;
                    IF_BIT_0(probLen) {
                        UPDATE_0(probLen);
                        probLen = prob + LenMid + (posState << kLenNumMidBits);
                        offset = kLenNumLowSymbols;
                        lim = (1 << kLenNumMidBits);
                    } else {
                        UPDATE_1(probLen);
                        probLen = prob + LenHigh;
                        offset = kLenNumLowSymbols + kLenNumMidSymbols;
                        lim = (1 << kLenNumHighBits);
                    }
                }
                TREE_DECODE(probLen, lim, len);
                len += offset;
            }

            if (state >= kNumStates) {
                uint32_t distance;
                prob = probs + PosSlot + ((len < kNumLenToPosStates ? len : kNumLenToPosStates - 1) << kNumPosSlotBits);
                TREE_6_DECODE(prob, distance);
                if (distance >= kStartPosModelIndex) {
                    unsigned posSlot = (unsigned)distance;
                    int numDirectBits = (int)(((distance >> 1) - 1));
                    distance = (2 | (distance & 1));
                    if (posSlot < kEndPosModelIndex) {
                        distance <<= numDirectBits;
                        prob = probs + SpecPos + distance - posSlot - 1;
                        {
                            uint32_t mask = 1;
                            unsigned i = 1;
                            do {
                                GET_BIT2(prob + i, i, ;, distance |= mask);
                                mask <<= 1;
                            } while (--numDirectBits != 0);
                        }
                    } else {
                        numDirectBits -= kNumAlignBits;
                        do {
                            NORMALIZE
                            range >>= 1;
                            {
                                uint32_t t;
                                code -= range;
                                t = (0 - ((uint32_t)code >> 31));
                                distance = (distance << 1) + (t + 1);
                                code += range & t;
                            }
                        } while (--numDirectBits != 0);
                        prob = probs + Align;
                        distance <<= kNumAlignBits;
                        {
                            unsigned i = 1;
                            GET_BIT2(prob + i, i, ;, distance |= 1);
                            GET_BIT2(prob + i, i, ;, distance |= 2);
                            GET_BIT2(prob + i, i, ;, distance |= 4);
                            GET_BIT2(prob + i, i, ;, distance |= 8);
                        }
                        if (distance == (uint32_t)0xFFFFFFFF) {
                            len += kMatchSpecLenStart;
                            state -= kNumStates;
                            break;
                        }
                    }
                }
                rep3 = rep2;
                rep2 = rep1;
                rep1 = rep0;
                rep0 = distance + 1;
                if (checkDicSize == 0) {
                    if (distance >= processedPos) return SZ_ERROR_DATA;
                } else if (distance >= checkDicSize)
                    return SZ_ERROR_DATA;
                state = (state < kNumStates + kNumLitStates) ? kNumLitStates : kNumLitStates + 3;
            }

            len += kMatchMinLen;

            if (limit == dicPos) return SZ_ERROR_DATA;
            {
                size_t rem = limit - dicPos;
                unsigned curLen = ((rem < len) ? (unsigned)rem : len);
                size_t pos = (dicPos - rep0) + ((dicPos < rep0) ? dicBufSize : 0);

                processedPos += curLen;

                len -= curLen;
                if (pos + curLen <= dicBufSize) {
                    uint8_t *dest = dic + dicPos;
                    ptrdiff_t src = (ptrdiff_t)pos - (ptrdiff_t)dicPos;
                    const uint8_t *lim = dest + curLen;
                    dicPos += curLen;
                    do *(dest) = (uint8_t) * (dest + src);
                    while (++dest != lim);
                } else {
                    do {
                        dic[dicPos++] = dic[pos];
                        if (++pos == dicBufSize) pos = 0;
                    } while (--curLen != 0);
                }
            }
        }
    } while (dicPos < limit && buf < bufLimit);
    NORMALIZE;
    p->buf = buf;
    p->range = range;
    p->code = code;
    p->remainLen = len;
    p->dicPos = dicPos;
    p->processedPos = processedPos;
    p->reps[0] = rep0;
    p->reps[1] = rep1;
    p->reps[2] = rep2;
    p->reps[3] = rep3;
    p->state = state;
    return SZ_OK;
}

static void write_rem(BbLzmaDec *p, size_t limit) {
    if (p->remainLen != 0 && p->remainLen < kMatchSpecLenStart) {
        uint8_t *dic = p->dic;
        size_t dicPos = p->dicPos;
        size_t dicBufSize = p->dicBufSize;
        unsigned len = p->remainLen;
        uint32_t rep0 = p->reps[0];
        if (limit - dicPos < len) len = (unsigned)(limit - dicPos);
        if (p->checkDicSize == 0 && p->prop.dicSize - p->processedPos <= len) p->checkDicSize = p->prop.dicSize;
        p->processedPos += (uint32_t)len;
        p->remainLen -= len;
        while (len != 0) {
            len--;
            dic[dicPos] = dic[(dicPos - rep0) + ((dicPos < rep0) ? dicBufSize : 0)];
            dicPos++;
        }
        p->dicPos = dicPos;
    }
}

/* LzmaDec_DecodeReal2: the game's entry point is replaced by a jump to this function. */
int bb_lzma_decode_real2(void *decoder, size_t limit, const uint8_t *bufLimit) {
    BbLzmaDec *p = (BbLzmaDec *)decoder;
    do {
        size_t limit2 = limit;
        if (p->checkDicSize == 0) {
            uint32_t rem = p->prop.dicSize - p->processedPos;
            if (limit - p->dicPos > rem) limit2 = p->dicPos + rem;
        }
        if (decode_real(p, limit2, bufLimit) != SZ_OK) return SZ_ERROR_DATA;
        if (p->processedPos >= p->prop.dicSize) p->checkDicSize = p->prop.dicSize;
        write_rem(p, limit);
    } while (p->dicPos < limit && p->buf < bufLimit && p->remainLen < kMatchSpecLenStart);
    if (p->remainLen > kMatchSpecLenStart) p->remainLen = kMatchSpecLenStart;
    return SZ_OK;
}
