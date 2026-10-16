set(INTERPRETER
    ${B}/src/emu/x64run.c ${B}/src/emu/x64run0f.c ${B}/src/emu/x64run66.c ${B}/src/emu/x64run660f.c
    ${B}/src/emu/x64run66f20f.c ${B}/src/emu/x64run66f30f.c ${B}/src/emu/x64run66d9.c ${B}/src/emu/x64run66dd.c
    ${B}/src/emu/x64run66f0.c ${B}/src/emu/x64rund8.c ${B}/src/emu/x64rund9.c ${B}/src/emu/x64runda.c
    ${B}/src/emu/x64rundb.c ${B}/src/emu/x64rundc.c ${B}/src/emu/x64rundd.c ${B}/src/emu/x64runde.c
    ${B}/src/emu/x64rundf.c ${B}/src/emu/x64runf0.c ${B}/src/emu/x64runf20f.c ${B}/src/emu/x64runf30f.c
    ${B}/src/emu/x64runavx.c ${B}/src/emu/x64runavx0f.c ${B}/src/emu/x64runavx0f38.c ${B}/src/emu/x64runavx660f.c
    ${B}/src/emu/x64runavxf20f.c ${B}/src/emu/x64runavxf30f.c ${B}/src/emu/x64runavx660f38.c
    ${B}/src/emu/x64runavx660f3a.c ${B}/src/emu/x64runavxf20f38.c ${B}/src/emu/x64runavxf30f38.c
    ${B}/src/emu/x64runavxf20f3a.c ${B}/src/emu/x64runavxf30f3a.c)
set(DYNAREC_ASM ${B}/src/dynarec/arm64/arm64_prolog.S ${B}/src/dynarec/arm64/arm64_epilog.S
    ${B}/src/dynarec/arm64/arm64_next.S ${B}/src/dynarec/arm64/arm64_lock.S)
file(GLOB DYNAREC_PASS ${B}/src/dynarec/arm64/dynarec_arm64_helper.c ${B}/src/dynarec/arm64/dynarec_arm64_emit_*.c
    ${B}/src/dynarec/arm64/dynarec_arm64_00.c ${B}/src/dynarec/arm64/dynarec_arm64_0f.c ${B}/src/dynarec/arm64/dynarec_arm64_66.c
    ${B}/src/dynarec/arm64/dynarec_arm64_d?.c ${B}/src/dynarec/arm64/dynarec_arm64_f0.c ${B}/src/dynarec/arm64/dynarec_arm64_660f.c
    ${B}/src/dynarec/arm64/dynarec_arm64_66f20f.c ${B}/src/dynarec/arm64/dynarec_arm64_66f30f.c ${B}/src/dynarec/arm64/dynarec_arm64_66f0.c
    ${B}/src/dynarec/arm64/dynarec_arm64_f20f.c ${B}/src/dynarec/arm64/dynarec_arm64_f30f.c ${B}/src/dynarec/arm64/dynarec_arm64_avx*.c
    ${B}/src/dynarec/arm64/updateflags_arm64_pass.c)
list(APPEND DYNAREC_PASS ${B}/src/dynarec/dynarec_native_pass.c)
set(CORE_SRC
    ${B}/src/build_info.c ${B}/src/custommem.c
    ${B}/src/dynarec/arm64/arm64_immenc.c ${B}/src/dynarec/arm64/arm64_printer.c
    ${B}/src/dynarec/arm64/dynarec_arm64_arch.c ${B}/src/dynarec/arm64/dynarec_arm64_functions.c
    ${B}/src/dynarec/arm64/dynarec_arm64_jmpnext.c ${B}/src/dynarec/arm64/dynarec_arm64_consts.c
    ${B}/src/dynarec/arm64/updateflags_arm64.c
    ${B}/src/dynarec/dynablock.c ${B}/src/dynarec/dynarec_native_functions.c ${B}/src/dynarec/dynarec_native.c
    ${B}/src/dynarec/dynarec.c ${B}/src/dynarec/dynacache_reloc.c ${B}/src/dynarec/dynacache_compress.c
    ${B}/src/emu/x64compstrings.c ${B}/src/emu/x64emu.c ${B}/src/emu/x64primop.c ${B}/src/emu/x64shaext.c
    ${B}/src/emu/x64test.c ${B}/src/emu/x64trace.c ${B}/src/emu/x64run_private.c ${B}/src/emu/x87emu_private.c
    ${B}/src/os/hostext_common.c ${B}/src/os/hostext_linux.c ${B}/src/os/freq_linux.c
    ${B}/src/os/perfmap.c ${B}/src/os/random_linux.c ${B}/src/os/my_cpuid.c ${B}/src/os/sysinfo.c
    ${B}/src/tools/alternate.c ${B}/src/tools/bitutils.c ${B}/src/tools/env.c ${B}/src/tools/dynacache.c
    ${B}/src/tools/rbtree.c ${B}/src/tools/cpumask.c)
