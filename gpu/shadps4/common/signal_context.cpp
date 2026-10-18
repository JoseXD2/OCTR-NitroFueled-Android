// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/arch.h"
#include "common/signal_context.h"

#ifdef _WIN32
#include <windows.h>
#elif defined(__FreeBSD__)
#include <machine/npx.h>
#include <sys/ucontext.h>
#else
#include <sys/ucontext.h>
#endif
#if defined(__linux__) && defined(ARCH_ARM64)
#include <asm/sigcontext.h>
#endif

namespace Common {

void* GetRip(void* ctx) {
#if defined(_WIN32)
    return (void*)((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rip;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__rip;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext->__ss.__pc;
#elif defined(__FreeBSD__)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.mc_rip;
#elif defined(ARCH_X86_64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RIP];
#elif defined(__linux__) && defined(ARCH_ARM64)
    return (void*)((ucontext_t*)ctx)->uc_mcontext.pc;
#else
#error "Unsupported architecture"
#endif
}

u64 GetRsp(void* ctx) {
#if defined(_WIN32)
    return ((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rsp;
#elif defined(__linux__) && defined(ARCH_X86_64)
    return u64(((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RSP]);
#elif defined(__linux__) && defined(ARCH_ARM64)
    // SacredFire Android: Box64 keeps the guest rsp in x14.
    return u64(((ucontext_t*)ctx)->uc_mcontext.regs[14]);
#else
    (void)ctx;
    return 0;
#endif
}

u64 GetRbp(void* ctx) {
#if defined(_WIN32)
    return ((EXCEPTION_POINTERS*)ctx)->ContextRecord->Rbp;
#elif defined(__linux__) && defined(ARCH_X86_64)
    return u64(((ucontext_t*)ctx)->uc_mcontext.gregs[REG_RBP]);
#elif defined(__linux__) && defined(ARCH_ARM64)
    return u64(((ucontext_t*)ctx)->uc_mcontext.regs[15]); // guest rbp under Box64
#else
    (void)ctx;
    return 0;
#endif
}

bool IsWriteError(void* ctx) {
#if defined(_WIN32)
    return ((EXCEPTION_POINTERS*)ctx)->ExceptionRecord->ExceptionInformation[0] == 1;
#elif defined(__APPLE__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__err & 0x2;
#elif defined(__APPLE__) && defined(ARCH_ARM64)
    return ((ucontext_t*)ctx)->uc_mcontext->__es.__esr & 0x40;
#elif defined(__FreeBSD__) && defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.mc_err & 0x2;
#elif defined(ARCH_X86_64)
    return ((ucontext_t*)ctx)->uc_mcontext.gregs[REG_ERR] & 0x2;
#elif defined(__linux__) && defined(ARCH_ARM64)
    // The fault's ESR (WnR, bit 6) is in the signal frame's extension records.
    const auto* p = reinterpret_cast<const u8*>(((ucontext_t*)ctx)->uc_mcontext.__reserved);
    for (int i = 0; i < 64; ++i) {
        const auto* h = reinterpret_cast<const _aarch64_ctx*>(p);
        if (h->magic == 0 || h->size == 0) {
            break;
        }
        if (h->magic == ESR_MAGIC) {
            return (reinterpret_cast<const esr_context*>(h)->esr & 0x40) != 0;
        }
        p += h->size;
    }
    return false;
#else
#error "Unsupported architecture"
#endif
}

} // namespace Common
