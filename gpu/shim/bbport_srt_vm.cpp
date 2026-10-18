// SacredFire Android: the SRT walkers that flatten_extended_userdata_pass generates are x86-64
// code (xbyak). On ARM64 they are not executed: each walker is decoded once (Zydis) into a short
// list of operations and interpreted here. The instruction subset is exactly what the pass emits
// (mov/push/pop/add/sub/neg/imul/shl/shr/and/or/xor/not/dec/cmp/cmova/cmovb/ret on rdi, rsi,
// rsp, rcx, rdx, r10). Loads from guest pointers go through bb_srt_load32/64: a fault there
// (stale or garbage pointer in the SRT) reads 0, like the x86 handler that patches the faulting
// load into "xor reg, reg".
#ifdef BB_ANDROID
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <ucontext.h>
#include <unordered_map>
#include <vector>
#include <Zydis/Zydis.h>
#include "common/assert.h"
#include "common/logging/log.h"
#include "core/signals.h"
#include "shader_recompiler/ir/passes/srt.h"

extern "C" {
u32 bb_srt_load32(const void* address);
u64 bb_srt_load64(const void* address);
extern const char bb_srt_load32_insn[];
extern const char bb_srt_load64_insn[];
}

#if defined(__aarch64__)
asm(R"(
    .text
    .p2align 2
    .globl bb_srt_load32
    .hidden bb_srt_load32
    .type bb_srt_load32, %function
bb_srt_load32:
    .globl bb_srt_load32_insn
    .hidden bb_srt_load32_insn
bb_srt_load32_insn:
    ldr w0, [x0]
    ret
    .size bb_srt_load32, .-bb_srt_load32
    .globl bb_srt_load64
    .hidden bb_srt_load64
    .type bb_srt_load64, %function
bb_srt_load64:
    .globl bb_srt_load64_insn
    .hidden bb_srt_load64_insn
bb_srt_load64_insn:
    ldr x0, [x0]
    ret
    .size bb_srt_load64, .-bb_srt_load64
)");
#else
#error "bbport_srt_vm.cpp: ARM64 only"
#endif

namespace Shader {
namespace {

enum class Kind : u8 { None, Reg, Imm, Mem };

struct Operand {
    Kind kind = Kind::None;
    u8 size = 0;      // bytes
    u8 reg = 0;       // Reg: register index (rax=0 .. r15=15)
    u8 base = 0xff;   // Mem: base register index or 0xff
    u8 index = 0xff;  // Mem: index register index or 0xff
    u8 scale = 0;
    s64 value = 0;    // Imm: immediate, Mem: displacement
};

enum class Op : u8 { Mov, Push, Pop, Add, Sub, Neg, Imul, Shl, Shr, And, Or, Xor, Not, Dec, Cmp, Cmova, Cmovb, Ret };

struct Insn {
    Op op;
    u8 count;
    Operand o[3];
};

using Program = std::vector<Insn>;

constexpr u8 RSP = 4;
constexpr size_t StackSlots = 1024;

u8 RegIndex(ZydisRegister reg) {
    const ZydisRegister full = ZydisRegisterGetLargestEnclosing(ZYDIS_MACHINE_MODE_LONG_64, reg);
    if (full < ZYDIS_REGISTER_RAX || full > ZYDIS_REGISTER_R15) {
        return 0xff;
    }
    return static_cast<u8>(full - ZYDIS_REGISTER_RAX);
}

bool Translate(const u8* code, Program& out) {
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    // Walkers are at most a few KiB; the bound only stops a runaway decode.
    for (size_t offset = 0; offset < 1u << 20;) {
        ZydisDecodedInstruction in;
        ZydisDecodedOperand ops[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code + offset, 15, &in, ops))) {
            LOG_ERROR(Render_Recompiler, "SRT VM: undecodable walker byte at {}+{:#x}",
                      fmt::ptr(code), offset);
            return false;
        }
        Insn insn{};
        switch (in.mnemonic) {
        case ZYDIS_MNEMONIC_MOV: insn.op = Op::Mov; break;
        case ZYDIS_MNEMONIC_PUSH: insn.op = Op::Push; break;
        case ZYDIS_MNEMONIC_POP: insn.op = Op::Pop; break;
        case ZYDIS_MNEMONIC_ADD: insn.op = Op::Add; break;
        case ZYDIS_MNEMONIC_SUB: insn.op = Op::Sub; break;
        case ZYDIS_MNEMONIC_NEG: insn.op = Op::Neg; break;
        case ZYDIS_MNEMONIC_IMUL: insn.op = Op::Imul; break;
        case ZYDIS_MNEMONIC_SHL: insn.op = Op::Shl; break;
        case ZYDIS_MNEMONIC_SHR: insn.op = Op::Shr; break;
        case ZYDIS_MNEMONIC_AND: insn.op = Op::And; break;
        case ZYDIS_MNEMONIC_OR: insn.op = Op::Or; break;
        case ZYDIS_MNEMONIC_XOR: insn.op = Op::Xor; break;
        case ZYDIS_MNEMONIC_NOT: insn.op = Op::Not; break;
        case ZYDIS_MNEMONIC_DEC: insn.op = Op::Dec; break;
        case ZYDIS_MNEMONIC_CMP: insn.op = Op::Cmp; break;
        case ZYDIS_MNEMONIC_CMOVNBE: insn.op = Op::Cmova; break;
        case ZYDIS_MNEMONIC_CMOVB: insn.op = Op::Cmovb; break;
        case ZYDIS_MNEMONIC_RET: insn.op = Op::Ret; break;
        case ZYDIS_MNEMONIC_NOP: offset += in.length; continue;
        default:
            LOG_ERROR(Render_Recompiler, "SRT VM: unsupported instruction {} in walker {}",
                      ZydisMnemonicGetString(in.mnemonic), fmt::ptr(code));
            return false;
        }
        insn.count = std::min<u8>(in.operand_count_visible, 3);
        for (u8 i = 0; i < insn.count; i++) {
            const ZydisDecodedOperand& src = ops[i];
            Operand& dst = insn.o[i];
            dst.size = static_cast<u8>(src.size / 8);
            switch (src.type) {
            case ZYDIS_OPERAND_TYPE_REGISTER:
                dst.kind = Kind::Reg;
                dst.reg = RegIndex(src.reg.value);
                if (dst.reg == 0xff) {
                    LOG_ERROR(Render_Recompiler, "SRT VM: unsupported register in walker {}",
                              fmt::ptr(code));
                    return false;
                }
                break;
            case ZYDIS_OPERAND_TYPE_IMMEDIATE:
                dst.kind = Kind::Imm;
                dst.value = src.imm.is_signed ? src.imm.value.s : static_cast<s64>(src.imm.value.u);
                break;
            case ZYDIS_OPERAND_TYPE_MEMORY:
                dst.kind = Kind::Mem;
                dst.base = src.mem.base == ZYDIS_REGISTER_NONE ? 0xff : RegIndex(src.mem.base);
                dst.index = src.mem.index == ZYDIS_REGISTER_NONE ? 0xff : RegIndex(src.mem.index);
                dst.scale = src.mem.scale;
                dst.value = src.mem.disp.value;
                break;
            default:
                return false;
            }
        }
        out.push_back(insn);
        offset += in.length;
        if (insn.op == Op::Ret) {
            return true;
        }
    }
    return false;
}

bool FaultHandler(void* context, void* /*fault_address*/) {
    auto* uc = static_cast<ucontext_t*>(context);
    const uintptr_t pc = uc->uc_mcontext.pc;
    if (pc != reinterpret_cast<uintptr_t>(bb_srt_load32_insn) &&
        pc != reinterpret_cast<uintptr_t>(bb_srt_load64_insn)) {
        return false;
    }
    uc->uc_mcontext.regs[0] = 0;
    uc->uc_mcontext.pc = pc + 4;
    return true;
}

std::shared_mutex g_programs_mutex;
std::unordered_map<const void*, std::shared_ptr<const Program>> g_programs;

std::shared_ptr<const Program> GetProgram(const void* code) {
    {
        std::shared_lock lock{g_programs_mutex};
        if (const auto it = g_programs.find(code); it != g_programs.end()) {
            return it->second;
        }
    }
    auto program = std::make_shared<Program>();
    if (!Translate(static_cast<const u8*>(code), *program)) {
        program.reset(); // cached as null: the walker is skipped from now on
    }
    std::unique_lock lock{g_programs_mutex};
    return g_programs.emplace(code, std::move(program)).first->second;
}

inline u64 Mask(u64 v, u8 size) {
    return size >= 8 ? v : v & ((1ull << (size * 8)) - 1);
}

} // namespace

void BbSrtVmRun(PFN_SrtWalker walker, const u32* user_data, u32* flat_dst) {
    static std::once_flag registered;
    std::call_once(registered, [] {
        Core::Signals::Instance()->RegisterAccessViolationHandler(FaultHandler, 0);
    });
    const auto program = GetProgram(reinterpret_cast<const void*>(walker));
    if (!program) {
        return;
    }

    u64 stack[StackSlots];
    u64 r[16] = {};
    r[7] = reinterpret_cast<u64>(user_data); // rdi
    r[6] = reinterpret_cast<u64>(flat_dst);  // rsi
    r[RSP] = reinterpret_cast<u64>(stack + StackSlots);
    const u64 stack_lo = reinterpret_cast<u64>(stack);
    u64 cmp_a = 0, cmp_b = 0;

    const auto addr = [&](const Operand& o) {
        u64 a = static_cast<u64>(o.value);
        if (o.base != 0xff) a += r[o.base];
        if (o.index != 0xff) a += r[o.index] * o.scale;
        return a;
    };
    const auto read = [&](const Operand& o) -> u64 {
        switch (o.kind) {
        case Kind::Reg:
            return Mask(r[o.reg], o.size);
        case Kind::Imm:
            return Mask(static_cast<u64>(o.value), o.size);
        case Kind::Mem: {
            const void* p = reinterpret_cast<const void*>(addr(o));
            return o.size == 8 ? bb_srt_load64(p) : bb_srt_load32(p);
        }
        default:
            return 0;
        }
    };
    const auto write = [&](const Operand& o, u64 v) {
        if (o.kind == Kind::Reg) {
            r[o.reg] = Mask(v, o.size); // 32-bit writes zero-extend
        } else if (o.kind == Kind::Mem) {
            const u64 a = addr(o);
            if (o.size == 8) {
                std::memcpy(reinterpret_cast<void*>(a), &v, 8);
            } else {
                const u32 v32 = static_cast<u32>(v);
                std::memcpy(reinterpret_cast<void*>(a), &v32, 4);
            }
        }
    };

    for (const Insn& in : *program) {
        const Operand& d = in.o[0];
        switch (in.op) {
        case Op::Mov: write(d, read(in.o[1])); break;
        case Op::Push:
            if (r[RSP] - 8 < stack_lo) {
                LOG_ERROR(Render_Recompiler, "SRT VM: stack overflow in walker {}",
                          fmt::ptr(walker));
                return;
            }
            r[RSP] -= 8;
            *reinterpret_cast<u64*>(r[RSP]) = read(d);
            break;
        case Op::Pop:
            write(d, *reinterpret_cast<const u64*>(r[RSP]));
            r[RSP] += 8;
            break;
        case Op::Add: write(d, read(d) + read(in.o[1])); break;
        case Op::Sub: write(d, read(d) - read(in.o[1])); break;
        case Op::Neg: write(d, 0 - read(d)); break;
        case Op::Imul:
            if (in.count == 3) write(d, read(in.o[1]) * read(in.o[2]));
            else write(d, read(d) * read(in.o[1]));
            break;
        case Op::Shl: write(d, read(d) << (read(in.o[1]) & (d.size == 8 ? 63 : 31))); break;
        case Op::Shr: write(d, read(d) >> (read(in.o[1]) & (d.size == 8 ? 63 : 31))); break;
        case Op::And: write(d, read(d) & read(in.o[1])); break;
        case Op::Or: write(d, read(d) | read(in.o[1])); break;
        case Op::Xor: write(d, read(d) ^ read(in.o[1])); break;
        case Op::Not: write(d, ~read(d)); break;
        case Op::Dec: write(d, read(d) - 1); break;
        case Op::Cmp: cmp_a = read(d); cmp_b = read(in.o[1]); break;
        case Op::Cmova: if (cmp_a > cmp_b) write(d, read(in.o[1])); break;
        case Op::Cmovb: if (cmp_a < cmp_b) write(d, read(in.o[1])); break;
        case Op::Ret: return;
        }
    }
}

} // namespace Shader
#endif
