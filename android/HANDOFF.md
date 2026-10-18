# Port Android (APK único) — estado e próximos passos

Leia isto no começo de um chat novo. O workspace da nuvem do chat anterior se perde, mas tudo o
que importa está nesta pasta `android/` e nas alterações em `gpu/`.

## Arquitetura (decidida)
- Um APK só (NativeActivity, sem Java). `libsacredfire.so` contém tudo.
- O código x86-64 do jogo roda no **núcleo do Box64** embutido (`android/box64core`): interpretador +
  dynarec ARM64, sem o loader ELF/wrappers do Box64. Ponte jogo→nativo = bridge do Box64
  (`CC 'S' 'C' wrapper função C3`); nativo→jogo = `bbx_call*` (DynaCall). GS do jogo =
  `emu->segs_offs[_GS]` (`bbx_set_gs_base`). Interface em `box64core/include/bb_box64.h`.
- Runtime (`src/`) e renderer (`gpu/`, shadPS4/Vulkan) compilados nativos ARM64.
- SDL3: implementação própria do subconjunto usado, sobre o NDK (`android/sdl_android`, ainda vazio),
  usando os headers oficiais do SDL3.
- Preparação do jogo (scripts Python) será portada para C dentro do APK.
- Alvo do usuário: Mali (sem geometry shaders → pelo/cabelo não renderiza; o resto deve funcionar).

## Toolchain (sem NDK: dl.google.com é bloqueado no ambiente da nuvem)
`android/toolchain/`:
- `mksysroot.sh`: monta sysroot API 29 a partir de github.com/aosp-mirror/platform_bionic
  (headers + stubs gerados de `*.map.txt` por `mkstubs.py` + crt), frameworks_base (libandroid.map.txt),
  pacotes Ubuntu `android-platform-frameworks-native-headers android-libnativehelper-dev
  android-liblog-dev`, Vulkan-Headers (KhronosGroup, HEAD 1.4.365) e `extra_include/`.
  Os caminhos estão fixos em /home/claude/w — ajuste W=.
- compiler-rt builtins (LLVM 18.1.3) instalado em `$(clang -print-resource-dir)/lib/linux/libclang_rt.builtins-aarch64-android.a`.
- libc++/libc++abi/libunwind **LLVM 19.1.7** estáticos (namespace `__ndk1`, experimental ligado —
  shadPS4 precisa de jthread/stop_token/atomic_ref), com `android-bare.cmake`, em /home/claude/w/prefix19.
- `android.cmake`: toolchain CMake (clang 18 do Ubuntu, alvo aarch64-linux-android29, lld).
- Dependências do renderer: `android/deps.sh` (fmt, robin-map, VMA, xxhash, zydis+zycore*, magic_enum,
  headers SDL3) em /home/claude/w/prefix-android. *zycore precisa ser instalado à parte
  (zydis/dependencies/zycore). Boost: symlink prefix-android/include/boost -> /usr/include/boost (libboost-dev).
- FFmpeg n7.1.2 estático (h264/hevc/aac, mov) cross-compilado no mesmo prefix (comando no histórico:
  configure --target-os=android --arch=aarch64 --disable-everything + decoders).
- Pacotes apt usados: qemu-user-static gcc/g++-aarch64-linux-gnu aapt apksigner zipalign
  android-sdk-platform-23 glslang-tools libvulkan-dev libboost-dev.
- Submódulos de gpu/third_party e third_party/LibAtrac9 precisam estar nos commits do .gitmodules.

## Feito e testado
- Box64 core compila (aarch64 linux-gnu e android). `box64core/test/t1.c` roda sob
  `qemu-aarch64-static -L /usr/aarch64-linux-gnu`: bridges (direta e universal com args na pilha),
  float/double, GS, callback nativo→jogo aninhado, dynarec — tudo OK.
- `android/CMakeLists.txt` configura box64core + sdl_android + gpu/ para Android.
- gpu/ compilando para Android: ~360/511 objetos sem erro quando parou; correções já feitas:
  - `gpu/CMakeLists.txt`: ramo `ANDROID` (deps do prefix, bbgpu STATIC, BB_ANDROID, arm_compat).
  - shim: diagnósticos só-Linux-x86 desligados como no Windows (`_WIN32 || BB_ANDROID`):
    free_check, gnm_hooks, guest_hooks, guest_memory, wait_trace; write_log e clobber leem
    registros ARM (Box64: x10..x25 = rax rcx rdx rbx rsp rbp rsi rdi r8..r15).
  - `common/signal_context.cpp`: GetRip/IsWriteError para Linux ARM64 (ESR WnR).
  - `page_manager.cpp`, `region_manager.h`: SpinLock incluído sempre (bionic sem mutex adaptativo).
  - `android/arm_compat/include`: __rdtsc (cntvct), pause/lfence, execinfo.h (backtrace via unwind).

## Sessão 2 (toolchain refeita na nuvem, mesmas versões)
- Reconstruído do zero em /home/claude/w: sysroot (mksysroot.sh), compiler-rt builtins e libc++ 19.1.7
  (compilados de llvm-project llvmorg-19.1.7, sparse), deps.sh + zycore, FFmpeg n7.1.2, submódulos.
  O Box64 **não fica no repositório**: clonar ptitSeb/box64 em `./box64` no commit de `box64core/BOX64_COMMIT`.
  Ele precisa dos headers gerados `git_head.h` e `dynacache_hashes.h` (gerar no configure do box64core — pendente).
- Configuração: `cmake -S android -B build/android -G Ninja -DCMAKE_TOOLCHAIN_FILE=$PWD/android/toolchain/android.cmake
  -DBB_DEPS_PREFIX=/home/claude/w/prefix-android -DCMAKE_BUILD_TYPE=Release -DBB_LTO=OFF` (LTO desligado só p/ iterar).
- Feito: superfície Vulkan Android (`WindowSystemType::Android`, `vk_platform.cpp`, `shim/window.cpp` driver
  "android" via `SDL_PROP_WINDOW_ANDROID_WINDOW_POINTER`).
- Feito: SRT walkers no ARM64 — o pass continua gerando x86-64 com xbyak (buffer não executável) e
  `gpu/shim/bbport_srt_vm.cpp` decodifica uma vez (Zydis) e interpreta; loads de ponteiros do jogo via
  `bb_srt_load32/64` (falha → 0, como o patch "xor reg,reg" do x86). Chamadas via `Shader::CallSrtWalker`.
  Ainda não compilado/testado.

## Próximos passos (ordem)
1. Terminar a compilação de bbgpu (`ninja -C build/android -k 0 bbgpu`), corrigir o resto.
   Pendentes conhecidos: `vk_platform.cpp` CreateSurface para Android (VK_USE_PLATFORM_ANDROID_KHR,
   WindowSystemType::Android) e `window.cpp` ramo driver "android"; gerador SRT (xbyak gera x86 →
   executar via `bbx_call`, chamar `bbx_code_mapped/written` no buffer).
2. `sdl_android`: ANativeActivity_onCreate, janela, fila de eventos, teclado, gamepads (AKeyEvent/
   AMotionEvent), toque → mouse (menu ImGui) + controle virtual na tela, áudio AAudio (downmix p/
   estéreo), propriedades, displays. Lista de funções SDL usadas: grep `SDL_` em src/ e gpu/shim.
3. Runtime `src/` para ARM64: `ABI` vazio fora de x86; probe.c: sem cpuid, mapear imagem,
   relocações de função → `bbx_bridge(resolved)`, traps x86 continuam (handler = bridge de
   `unresolved`), `bbx_code_mapped` nos segmentos executáveis, entrada via `bbx_call_on_stack`,
   módulos via `bbx_call3`; runtime_thread.c: arch_prctl(GS) → `bbx_set_gs_base`, entrada da
   thread via `bbx_call_on_stack`; chamadas a ponteiros de função do jogo (runtime.c atexit,
   runtime_kernel.c once/destrutores/hooks, avplayer callbacks) via `bbx_call*`; ponteiros de
   funções nativas entregues ao jogo (ex.: runtime_application_heap_api) via `bbx_bridge`;
   handler de SIGSEGV: `bbx_handle_fault` primeiro, depois bbgpu_handle_fault; rdtsc → cntvct.
   Funções com struct por valor / retorno de struct grande precisam de wrapper próprio.
4. Preparação do jogo em C (prepare/link_libc/link_modules/patches/content_profile/game_check/
   merge_update; nomes do aerolib embutidos). Validar byte a byte contra o Python com o dump.
5. Launcher nativo (Vulkan + ImGui) com permissão MANAGE_EXTERNAL_STORAGE via JNI; APK com
   aapt (android.jar 23 do Ubuntu), zipalign, apksigner (chave debug).
6. Usuário testa no celular (Mali) e manda o log.
