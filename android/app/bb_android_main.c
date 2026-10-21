/* SacredFire Android: the app's main thread (started by sdl_android's NativeActivity glue). Gets
 * access to the shared storage, sets what run.sh sets on the desktop and starts the loader
 * (src/probe.c) on the prepared game.
 *
 * Folder layout on the device (/sdcard/SacredFire, BB_DATA_DIR):
 *   CUSA13795/      the game: the 1.00 dump with the 1.21 update copied over it
 *   out/            the prepared files (boot-linked.bin, import_names.txt, content.bin, patches.bin)
 *   user/           saves, shader cache, settings written by the game
 *   bbport.ini      settings (the in-game menu writes it)
 *   sacredfire.log  the log of the last run
 *   env.txt         optional NAME=VALUE lines (one per line, # comments) set before the defaults */
#define _GNU_SOURCE
#include <android/native_activity.h>
#include <dirent.h>
#include <sched.h>
#include <dlfcn.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <ucontext.h>
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sdl_android.h"
#include "bb_box64.h"

int bb_probe_main(int argc, char **argv);

#define DATA_DIR "/sdcard/SacredFire"

static int exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static jboolean has_storage_access(JNIEnv *env, ANativeActivity *activity) {
    if (activity->sdkVersion >= 30) {
        jclass environment = (*env)->FindClass(env, "android/os/Environment");
        jmethodID manager = (*env)->GetStaticMethodID(env, environment, "isExternalStorageManager", "()Z");
        jboolean granted = (*env)->CallStaticBooleanMethod(env, environment, manager);
        (*env)->DeleteLocalRef(env, environment);
        return granted;
    }
    jclass context = (*env)->GetObjectClass(env, activity->clazz);
    jmethodID check = (*env)->GetMethodID(env, context, "checkSelfPermission", "(Ljava/lang/String;)I");
    jstring permission = (*env)->NewStringUTF(env, "android.permission.WRITE_EXTERNAL_STORAGE");
    jint result = (*env)->CallIntMethod(env, activity->clazz, check, permission);
    (*env)->DeleteLocalRef(env, permission);
    (*env)->DeleteLocalRef(env, context);
    return result == 0; /* PERMISSION_GRANTED */
}

static void request_storage_access(JNIEnv *env, ANativeActivity *activity) {
    jclass activity_class = (*env)->GetObjectClass(env, activity->clazz);
    if (activity->sdkVersion >= 30) {
        /* "All files access" for this app: the settings page, the player switches it on. */
        jmethodID get_package = (*env)->GetMethodID(env, activity_class, "getPackageName", "()Ljava/lang/String;");
        jstring package = (jstring)(*env)->CallObjectMethod(env, activity->clazz, get_package);
        const char *name = (*env)->GetStringUTFChars(env, package, NULL);
        char uri_text[256];
        snprintf(uri_text, sizeof uri_text, "package:%s", name);
        (*env)->ReleaseStringUTFChars(env, package, name);
        jclass uri_class = (*env)->FindClass(env, "android/net/Uri");
        jmethodID parse = (*env)->GetStaticMethodID(env, uri_class, "parse", "(Ljava/lang/String;)Landroid/net/Uri;");
        jstring uri_string = (*env)->NewStringUTF(env, uri_text);
        jobject uri = (*env)->CallStaticObjectMethod(env, uri_class, parse, uri_string);
        jclass intent_class = (*env)->FindClass(env, "android/content/Intent");
        jmethodID intent_init = (*env)->GetMethodID(env, intent_class, "<init>", "(Ljava/lang/String;Landroid/net/Uri;)V");
        jstring action = (*env)->NewStringUTF(env, "android.settings.MANAGE_APP_ALL_FILES_ACCESS_PERMISSION");
        jobject intent = (*env)->NewObject(env, intent_class, intent_init, action, uri);
        jmethodID start = (*env)->GetMethodID(env, activity_class, "startActivity", "(Landroid/content/Intent;)V");
        (*env)->CallVoidMethod(env, activity->clazz, start, intent);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    } else {
        jclass string_class = (*env)->FindClass(env, "java/lang/String");
        jobjectArray permissions = (*env)->NewObjectArray(env, 2, string_class, NULL);
        (*env)->SetObjectArrayElement(env, permissions, 0,
                                      (*env)->NewStringUTF(env, "android.permission.READ_EXTERNAL_STORAGE"));
        (*env)->SetObjectArrayElement(env, permissions, 1,
                                      (*env)->NewStringUTF(env, "android.permission.WRITE_EXTERNAL_STORAGE"));
        jmethodID request = (*env)->GetMethodID(env, activity_class, "requestPermissions", "([Ljava/lang/String;I)V");
        (*env)->CallVoidMethod(env, activity->clazz, request, permissions, 1);
        if ((*env)->ExceptionCheck(env)) (*env)->ExceptionClear(env);
    }
}

/* Blocks until the app may read and write /sdcard/SacredFire. */
static void wait_for_storage(ANativeActivity *activity) {
    JNIEnv *env = NULL;
    if ((*activity->vm)->AttachCurrentThread(activity->vm, &env, NULL) != JNI_OK) {
        printf("SacredFire: JNI attach failed; storage access not checked\n");
        return;
    }
    if (!has_storage_access(env, activity)) {
        printf("SacredFire: asking for access to the shared storage (%s)\n", DATA_DIR);
        request_storage_access(env, activity);
        while (!has_storage_access(env, activity)) usleep(500000);
        printf("SacredFire: storage access granted\n");
    }
    (*activity->vm)->DetachCurrentThread(activity->vm);
}

/* The GPU thread's optimization switches (src/runtime_memory.c, gpu/shim/bbport_toggles.h). */
extern uint64_t runtime_disabled_optimizations;
static void set_default(const char *name, const char *value) { setenv(name, value, 0); }

/* patches.bin (BBPATCH2) carries the 60 FPS Unlock write (mov [r14+0x488], edx). */
static int patches_have_60fps(const char *path) {
    static const unsigned char want[7] = {0x41, 0x89, 0x96, 0x88, 0x04, 0x00, 0x00};
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    unsigned char head[24];
    int found = 0;
    if (fread(head, 1, 24, f) == 24 && memcmp(head, "BBPATCH2", 8) == 0) {
        uint64_t count;
        memcpy(&count, head + 16, 8);
        for (uint64_t i = 0; i < count && i < 4096 && !found; i++) {
            uint64_t entry[2];
            if (fread(entry, 8, 2, f) != 2 || entry[1] > (1u << 20)) break;
            unsigned char data[16];
            if (entry[1] == sizeof want) {
                if (fread(data, 1, sizeof want, f) != sizeof want) break;
                found = memcmp(data, want, sizeof want) == 0;
            } else if (fseek(f, (long)entry[1], SEEK_CUR) != 0) {
                break;
            }
        }
    }
    fclose(f);
    return found;
}

/* env.txt: overrides for tests without rebuilding the APK. */
static void load_env_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *end = line + strlen(line);
        while (end > line && (end[-1] == '\n' || end[-1] == '\r' || end[-1] == ' ')) *--end = 0;
        char *name = line;
        while (*name == ' ' || *name == '\t') ++name;
        char *eq = strchr(name, '=');
        if (*name == '#' || !eq || eq == name) continue;
        *eq = 0;
        setenv(name, eq + 1, 1);
        printf("SacredFire Android: env.txt %s=%s\n", name, eq + 1);
    }
    fclose(f);
}

/* Every 5 s: the threads that used the most CPU (BB_THREAD_STATS=0 turns it off). Shows whether
 * a slow game is bound by one guest thread (Box64), the GPU thread or the driver. */
struct thread_sample { int tid; int cpu; unsigned long long ticks; char name[32]; };
static int read_threads(struct thread_sample *out, int max) {
    DIR *dir = opendir("/proc/self/task");
    if (!dir) return 0;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(dir)) && n < max) {
        if (e->d_name[0] == '.') continue;
        char path[64], buf[512];
        snprintf(path, sizeof path, "/proc/self/task/%s/stat", e->d_name);
        FILE *f = fopen(path, "r");
        if (!f) continue;
        size_t len = fread(buf, 1, sizeof buf - 1, f);
        fclose(f);
        buf[len] = 0;
        char *open = strchr(buf, '('), *close = strrchr(buf, ')');
        if (!open || !close) continue;
        struct thread_sample *t = &out[n];
        t->tid = atoi(e->d_name);
        size_t nl = (size_t)(close - open - 1);
        if (nl >= sizeof t->name) nl = sizeof t->name - 1;
        memcpy(t->name, open + 1, nl);
        t->name[nl] = 0;
        unsigned long long ut = 0, st = 0;
        /* after ")": state ppid pgrp session tty tpgid flags minflt cminflt majflt cmajflt utime stime */
        if (sscanf(close + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %llu %llu", &ut, &st) != 2) continue;
        t->ticks = ut + st;
        /* field 39 (processor): the 37th after ")" */
        t->cpu = -1;
        {
            const char *q = close + 2;
            for (int field = 3; field < 39 && q; ++field) {
                q = strchr(q, ' ');
                if (q) ++q;
            }
            if (q) t->cpu = atoi(q);
        }
        ++n;
    }
    closedir(dir);
    return n;
}
/* The busiest threads are sampled (SIGPROF every 2 ms) to show where they run: a guest
 * address (+offset in the game image, from Box64's dynarec blocks) or a host function. */
unsigned char *bb_guest_image(void);
enum { SAMPLE_MAX = 16384, SAMPLED_THREADS = 6, SAMPLE_FRAMES = 6 };
static struct { int tid; uintptr_t pc, lr; uintptr_t frames[SAMPLE_FRAMES]; } samples[SAMPLE_MAX];
static atomic_int sample_count;
static void sample_handler(int sig, siginfo_t *info, void *context) {
    (void)sig; (void)info;
    const int i = atomic_fetch_add(&sample_count, 1);
    if (i >= SAMPLE_MAX) return;
    const ucontext_t *uc = (const ucontext_t *)context;
    samples[i].tid = (int)syscall(SYS_gettid);
    samples[i].pc = (uintptr_t)uc->uc_mcontext.pc;
    samples[i].lr = (uintptr_t)uc->uc_mcontext.regs[30];
    /* Callers through the frame pointers (x29: [previous x29, return address]), read with
     * process_vm_readv so a bad chain cannot fault in the handler. */
    uintptr_t fp = (uintptr_t)uc->uc_mcontext.regs[29];
    const uintptr_t sp = (uintptr_t)uc->uc_mcontext.sp;
    for (int k = 0; k < SAMPLE_FRAMES; ++k) {
        samples[i].frames[k] = 0;
        if ((fp & 7) || fp < sp || fp - sp > (8u << 20)) continue;
        uintptr_t pair[2];
        struct iovec local = {pair, sizeof pair}, remote = {(void *)fp, sizeof pair};
        if (syscall(SYS_process_vm_readv, getpid(), &local, 1, &remote, 1, 0) != (long)sizeof pair) { fp = 1; continue; }
        samples[i].frames[k] = pair[1];
        fp = pair[0] > fp ? pair[0] : 1;
    }
}
static int describe_pc(uintptr_t pc, char *out, size_t size) {
    const uintptr_t guest = bbx_guest_pc(pc);
    const uintptr_t base = (uintptr_t)bb_guest_image();
    if (guest) {
        if (base && guest >= base && guest < base + 0x6000000) return snprintf(out, size, "+%#lx", (unsigned long)(guest - base));
        return snprintf(out, size, "g%#lx", (unsigned long)guest);
    }
    Dl_info dl = {0};
    if (dladdr((void *)pc, &dl) && dl.dli_fname) {
        const char *module = strrchr(dl.dli_fname, '/');
        module = module ? module + 1 : dl.dli_fname;
        if (dl.dli_sname) return snprintf(out, size, "%s:%s+%#lx", module, dl.dli_sname, (unsigned long)(pc - (uintptr_t)dl.dli_saddr));
        return snprintf(out, size, "%s+%#lx", module, (unsigned long)(pc - (uintptr_t)dl.dli_fbase));
    }
    return snprintf(out, size, "?%#lx", (unsigned long)pc);
}
static int cmp_desc(const void *a, const void *b) {
    const int x = *(const int *)a, y = *(const int *)b;
    return (y >> 16) - (x >> 16);
}
static void sample_report(const int *tids, char names[][32], int n) {
    int total = atomic_exchange(&sample_count, 0);
    if (total > SAMPLE_MAX) total = SAMPLE_MAX;
    for (int t = 0; t < n; ++t) {
        /* distinct (pc description) counts for this thread */
        static char keys[256][320];
        static int counts[256];
        int nk = 0, all = 0;
        for (int i = 0; i < total; ++i) {
            if (samples[i].tid != tids[t]) continue;
            char key[320];
            int len = describe_pc(samples[i].pc, key, 96);
            if (!bbx_guest_pc(samples[i].pc) && len < 96) {
                key[len++] = '<';
                len += describe_pc(samples[i].lr, key + len, 96);
                /* the port's own callers: the waits are named by them */
                for (int f = 0; f < SAMPLE_FRAMES && samples[i].frames[f] && len < (int)sizeof key - 70; ++f) {
                    Dl_info dl = {0};
                    if (!dladdr((void *)samples[i].frames[f], &dl) || !dl.dli_fname || !strstr(dl.dli_fname, "sacredfire") || !dl.dli_sname) continue;
                    len += snprintf(key + len, sizeof key - len, "<%.60s", dl.dli_sname);
                }
            }
            ++all;
            int k = 0;
            while (k < nk && strcmp(keys[k], key)) ++k;
            if (k == nk) {
                if (nk == 256) continue;
                strcpy(keys[nk], key);
                counts[nk++] = 0;
            }
            ++counts[k];
        }
        if (!all) continue;
        int order[256];
        for (int k = 0; k < nk; ++k) order[k] = (counts[k] << 16) | k;
        qsort(order, nk, sizeof *order, cmp_desc);
        char line[3000];
        int at = snprintf(line, sizeof line, "Samples %s (%d):", names[t], all);
        for (int k = 0; k < nk && k < 8 && at < (int)sizeof line - 340; ++k) {
            const int idx = order[k] & 0xffff;
            at += snprintf(line + at, sizeof line - at, " %s %d%%", keys[idx], counts[idx] * 100 / all);
        }
        printf("%s\n", line);
    }
}

/* All threads may run on every core: some phones start apps on the small cores only. */
static void print_allowed(const char *when) {
    char buf[4096];
    FILE *f = fopen("/proc/self/status", "r");
    if (!f) return;
    while (fgets(buf, sizeof buf, f))
        if (!strncmp(buf, "Cpus_allowed_list:", 18)) printf("CPU (%s): allowed %s", when, buf + 18);
    fclose(f);
    f = fopen("/proc/self/cpuset", "r");
    if (f) {
        if (fgets(buf, sizeof buf, f)) printf("CPU (%s): cpuset %s", when, buf);
        fclose(f);
    }
}
/* Thread placement. Every core of the phone may run every thread (some phones start apps on
 * the slow cores only), and the threads that pace the frame run on the fastest cores: the
 * game's main thread, its command stream, the GPU command thread and the Vulkan threads. The
 * fast cores are those whose top frequency is within 10 % of the highest (cpufreq), e.g. on
 * the Dimensity 8400 (all Cortex-A725) cpu4-7 at 3.0/3.25 GHz, not cpu0-3 at 2.1 GHz.
 * bionic has no pthread_setaffinity_np: sched_setaffinity on the thread id does the same.
 * BB_FAST_THREADS: comma-separated name prefixes (empty: no pinning); BB_FAST_CPUS: a mask. */
static cpu_set_t fast_cpus, all_cpus;
static int placement_ready, pin_fast;
static char fast_names[16][16];
static int n_fast_names;

static unsigned long cpu_max_khz(long cpu) {
    char path[96];
    snprintf(path, sizeof path, "/sys/devices/system/cpu/cpu%ld/cpufreq/cpuinfo_max_freq", cpu);
    FILE *f = fopen(path, "r");
    unsigned long khz = 0;
    if (f) {
        if (fscanf(f, "%lu", &khz) != 1) khz = 0;
        fclose(f);
    }
    return khz;
}

static void placement_init(void) {
    placement_ready = 1;
    const long cpus = sysconf(_SC_NPROCESSORS_CONF);
    CPU_ZERO(&all_cpus);
    CPU_ZERO(&fast_cpus);
    unsigned long khz[CPU_SETSIZE] = {0}, top = 0;
    for (long c = 0; c < cpus && c < CPU_SETSIZE; ++c) {
        CPU_SET(c, &all_cpus);
        khz[c] = cpu_max_khz(c);
        if (khz[c] > top) top = khz[c];
    }
    const char *mask = getenv("BB_FAST_CPUS");
    if (mask && *mask) {
        const unsigned long long m = strtoull(mask, NULL, 0);
        for (long c = 0; c < cpus && c < 64; ++c)
            if (m >> c & 1) CPU_SET(c, &fast_cpus);
    } else {
        for (long c = 0; c < cpus && c < CPU_SETSIZE; ++c)
            if (top == 0 || khz[c] * 10 >= top * 9) CPU_SET(c, &fast_cpus);
    }
    const char *names = getenv("BB_FAST_THREADS");
    if (!names) names = "bb:GameMain,igCommandStream,shadPS4:GpuComm,bb:VkRecorder,bb:Present";
    char list[512];
    snprintf(list, sizeof list, "%s", names);
    for (char *save = NULL, *tok = strtok_r(list, ",", &save); tok && n_fast_names < 16;
         tok = strtok_r(NULL, ",", &save)) {
        snprintf(fast_names[n_fast_names++], sizeof fast_names[0], "%s", tok);
    }
    /* Pinning only helps when the fast cores are a strict subset. */
    pin_fast = n_fast_names > 0 && CPU_COUNT(&fast_cpus) > 0 && CPU_COUNT(&fast_cpus) < CPU_COUNT(&all_cpus);
    char line[256];
    int at = snprintf(line, sizeof line, "CPU: top frequencies (MHz)");
    for (long c = 0; c < cpus && c < CPU_SETSIZE && at < (int)sizeof line - 24; ++c)
        at += snprintf(line + at, sizeof line - at, " %lu%s", khz[c] / 1000, CPU_ISSET(c, &fast_cpus) ? "*" : "");
    printf("%s; frame threads on the * cores: %s\n", line, pin_fast ? names : "no (all cores alike)");
}

static int is_fast_thread(const char *name) {
    for (int k = 0; k < n_fast_names; ++k)
        if (!strncmp(name, fast_names[k], strlen(fast_names[k]))) return 1;
    return 0;
}

static atomic_int background_throttled;

static void affinity_all(const struct thread_sample *threads, int n) {
    static int reported;
    if (atomic_load(&background_throttled)) return; /* background_watch has them */
    if (!placement_ready) placement_init();
    int failed = 0;
    for (int i = 0; i < n; ++i) {
        const cpu_set_t *want = pin_fast && is_fast_thread(threads[i].name) ? &fast_cpus : &all_cpus;
        cpu_set_t now;
        if (sched_getaffinity(threads[i].tid, sizeof now, &now) == 0 && CPU_EQUAL(&now, want)) continue;
        if (sched_setaffinity(threads[i].tid, sizeof *want, want)) ++failed;
    }
    if (failed && !reported) {
        reported = 1;
        printf("CPU: %d thread(s) could not be placed (errno %d)\n", failed, errno);
    }
}
/* In the background the game kept running at full speed on every core (it only stops
 * presenting), and the phone crawled after leaving the app. While the app is paused every
 * thread of the process gets the lowest priority and the slowest core; on return the priority
 * is restored and the next placement round puts them back. BB_BACKGROUND_THROTTLE=0: off. */
static void throttle_all(int on) {
    enum { MAX = 512 };
    static struct thread_sample threads[MAX];
    const int n = read_threads(threads, MAX);
    cpu_set_t slow;
    CPU_ZERO(&slow);
    if (!placement_ready) placement_init();
    /* The core with the lowest top frequency (cpu0 on most phones). */
    long slowest = 0;
    unsigned long low = ~0ul;
    const long cpus = sysconf(_SC_NPROCESSORS_CONF);
    for (long c = 0; c < cpus && c < CPU_SETSIZE; ++c) {
        const unsigned long khz = cpu_max_khz(c);
        if (khz && khz < low) { low = khz; slowest = c; }
    }
    CPU_SET(slowest, &slow);
    const pid_t self = (pid_t)syscall(SYS_gettid);
    for (int i = 0; i < n; ++i) {
        if (threads[i].tid == self) continue;
        setpriority(PRIO_PROCESS, (id_t)threads[i].tid, on ? 19 : 0);
        sched_setaffinity(threads[i].tid, sizeof(cpu_set_t), on ? &slow : &all_cpus);
    }
    printf("SacredFire Android: %s (%d threads%s)\n",
           on ? "in the background: game threads at the lowest priority on one slow core"
              : "back in the foreground: game threads restored",
           n, on ? "" : "");
}
static void *background_watch(void *arg) {
    (void)arg;
    pthread_setname_np(pthread_self(), "bb:bgwatch");
    for (;;) {
        const int paused = bb_android_paused();
        if (paused != atomic_load(&background_throttled)) {
            /* Placement first sees the restored threads after the flag drops. */
            if (paused) atomic_store(&background_throttled, 1);
            throttle_all(paused);
            if (!paused) atomic_store(&background_throttled, 0);
        }
        struct timespec t = {0, 200000000};
        nanosleep(&t, NULL);
    }
    return NULL;
}

static void *thread_stats_main(void *arg) {
    (void)arg;
    print_allowed("start");
    enum { MAX = 512 };
    static struct thread_sample prev[MAX], cur[MAX];
    int np = read_threads(prev, MAX);
    const long hz = sysconf(_SC_CLK_TCK);
    const int sampling = !getenv("BB_SAMPLE") || getenv("BB_SAMPLE")[0] != '0';
    if (sampling) {
        struct sigaction action = {0};
        action.sa_sigaction = sample_handler;
        action.sa_flags = SA_SIGINFO | SA_RESTART;
        sigemptyset(&action.sa_mask);
        sigaction(SIGPROF, &action, NULL);
    }
    int sampled[SAMPLED_THREADS], n_sampled = 0;
    static char sampled_names[SAMPLED_THREADS][32];
    for (;;) {
        if (n_sampled) {
            const pid_t pid = getpid();
            for (int round = 0; round < 2500; ++round) {
                for (int k = 0; k < n_sampled; ++k) syscall(SYS_tgkill, pid, sampled[k], SIGPROF);
                struct timespec t = {0, 2000000};
                nanosleep(&t, NULL);
            }
            sample_report(sampled, sampled_names, n_sampled);
        } else {
            sleep(5);
        }
        int nc = read_threads(cur, MAX);
        affinity_all(cur, nc);
        int top[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
        unsigned long long dt[MAX], total = 0;
        for (int i = 0; i < nc; ++i) {
            dt[i] = cur[i].ticks;
            for (int j = 0; j < np; ++j)
                if (prev[j].tid == cur[i].tid) { dt[i] = cur[i].ticks - prev[j].ticks; break; }
            total += dt[i];
            for (int k = 0; k < 8; ++k) {
                if (top[k] < 0 || dt[i] > dt[top[k]]) {
                    memmove(&top[k + 1], &top[k], sizeof(int) * (7 - k));
                    top[k] = i;
                    break;
                }
            }
        }
        char line[1024];
        int at = snprintf(line, sizeof line, "CPU 5s: total %llu%%:", total * 100 / (5 * hz));
        for (int k = 0; k < 8 && top[k] >= 0 && at < (int)sizeof line - 64; ++k)
            at += snprintf(line + at, sizeof line - at, " %s %llu%% (cpu%d)", cur[top[k]].name,
                           dt[top[k]] * 100 / (5 * hz), cur[top[k]].cpu);
        printf("%s\n", line);
        static int cycles;
        if (++cycles % 12 == 1) print_allowed("now");
        n_sampled = 0;
        for (int k = 0; sampling && k < 8 && top[k] >= 0 && n_sampled < SAMPLED_THREADS; ++k) {
            if (dt[top[k]] * 100 / (5 * hz) < 25 || cur[top[k]].tid == (int)syscall(SYS_gettid)) continue;
            sampled[n_sampled] = cur[top[k]].tid;
            snprintf(sampled_names[n_sampled], sizeof sampled_names[0], "%s", cur[top[k]].name);
            ++n_sampled;
        }
        /* The GPU threads always (their waits decide the frame rate). */
        for (int i = 0; sampling && i < nc && n_sampled < SAMPLED_THREADS; ++i) {
            if (strncmp(cur[i].name, "shadPS4:GpuComm", 15) && strncmp(cur[i].name, "bb:VkRecorder", 13) && strncmp(cur[i].name, "bb:DrawRec", 10) && strncmp(cur[i].name, "bb:Present", 10)) continue;
            int dup = 0;
            for (int k = 0; k < n_sampled; ++k) dup |= sampled[k] == cur[i].tid;
            if (dup) continue;
            sampled[n_sampled] = cur[i].tid;
            snprintf(sampled_names[n_sampled], sizeof sampled_names[0], "%s", cur[i].name);
            ++n_sampled;
        }
        memcpy(prev, cur, sizeof(cur[0]) * nc);
        np = nc;
    }
    return NULL;
}

/* The pipeline cache (user/cache) holds shaders translated by this build; another APK may
 * translate them differently (Mali workarounds) and stale ones drew black. The cache is moved
 * aside when libsacredfire.so is not the one that wrote it. */
static void check_shader_cache(const char *user) {
    /* Keyed on the shader translation, not on the APK: every test APK started with a cold
     * cache, and the compile stalls of a cold cache (seconds on Mali) let the game rebuild a
     * command buffer before its late labels (issue #2: the crash at Adventure's kart select).
     * Bump SHADER_CACHE_VERSION when gpu/shadps4/shader_recompiler or the pipeline keys change;
     * BB_SHADER_CACHE_PER_BUILD=1 brings back the per-APK stamp. */
#define SHADER_CACHE_VERSION "shaders-2026-10-21\n"
    char stamp[128], path[600], old[600], dir[600];
    snprintf(stamp, sizeof stamp, "%s", SHADER_CACHE_VERSION);
    if (getenv("BB_SHADER_CACHE_PER_BUILD") && getenv("BB_SHADER_CACHE_PER_BUILD")[0] == '1') {
        Dl_info dl = {0};
        struct stat so;
        if (!dladdr((void *)check_shader_cache, &dl) || !dl.dli_fname || stat(dl.dli_fname, &so)) return;
        snprintf(stamp, sizeof stamp, "%lld %lld\n", (long long)so.st_size, (long long)so.st_mtime);
    }
    snprintf(dir, sizeof dir, "%s/cache", user);
    snprintf(path, sizeof path, "%s/cache/.build", user);
    char seen[128] = {0};
    FILE *f = fopen(path, "r");
    if (f) {
        if (!fgets(seen, sizeof seen, f)) seen[0] = 0;
        fclose(f);
    }
    if (strcmp(seen, stamp) != 0) {
        if (exists(dir)) {
            snprintf(old, sizeof old, "%s/cache_old_%lld", user, (long long)time(NULL));
            if (rename(dir, old) == 0) printf("SacredFire Android: new build, shader cache moved to %s\n", old);
        }
        mkdir(dir, 0775);
        f = fopen(path, "w");
        if (f) {
            fputs(stamp, f);
            fclose(f);
        }
    }
}

int bb_android_main(ANativeActivity *activity) {
    pthread_setname_np(pthread_self(), "bb:GameMain"); /* thread placement finds it by name */
    wait_for_storage(activity);
    mkdir(DATA_DIR, 0775);
    mkdir(DATA_DIR "/user", 0775);
    bb_android_reopen_log();
    const char *data = getenv("BB_DATA_DIR") ? getenv("BB_DATA_DIR") : DATA_DIR;
    char game[512], out[512], user[512], config[512];
    snprintf(game, sizeof game, "%s/CUSA13795", data);
    snprintf(out, sizeof out, "%s/out", data);
    snprintf(user, sizeof user, "%s/user", data);
    snprintf(config, sizeof config, "%s/bbport.ini", data);
    printf("SacredFire Android: game %s, prepared files %s, log %s\n", game, out, bb_android_log_path());
    char env_file[600];
    snprintf(env_file, sizeof env_file, "%s/env.txt", data);
    load_env_file(env_file);
    check_shader_cache(user);
    if (!getenv("BB_THREAD_STATS") || getenv("BB_THREAD_STATS")[0] != '0') {
        pthread_t stats;
        if (pthread_create(&stats, NULL, thread_stats_main, NULL) == 0) pthread_detach(stats);
    }
    if (!getenv("BB_BACKGROUND_THROTTLE") || getenv("BB_BACKGROUND_THROTTLE")[0] != '0') {
        pthread_t watch;
        if (pthread_create(&watch, NULL, background_watch, NULL) == 0) pthread_detach(watch);
    }

    char eboot[600];
    snprintf(eboot, sizeof eboot, "%s/eboot.bin", game);
    if (!exists(eboot)) {
        printf("STOP: no eboot.bin in %s. Copy the game (1.00 with the 1.21 update over it) there.\n", game);
        return 2;
    }
    /* What run.sh sets for the game (docs/sacredfire), the same defaults. */
    set_default("BB_CONFIG", config);
    set_default("BB_GAME_PROFILE", "ctr");
    set_default("BB_GPU_LOG", "info"); /* test builds: the device, driver and features in the log */
    set_default("BB_NEO", "0");
    /* Mali: no second queue for readbacks and no background pre-upload: both move guest memory
     * on other queues/threads, and the title still mixed in stale image contents with them. */
    set_default("BB_READBACK_QUEUE", "0");
    set_default("BB_PREUPLOAD", "0");
    set_default("BB_GUEST_IN_PLACE", "0");
    set_default("BB_UFFD", "0");
    set_default("BB_COPY_GPU_BUFFERS", "1");
    set_default("BB_GPU_WRITE_TWINS", "1");
    set_default("BB_GPU_WRITE_TWINS_MAX", "65536");
    /* The 60 FPS patch (patches.bin): the game paces its own frames, a 60 Hz virtual vblank
     * would quantize late frames to 30; 480 Hz as run.sh does on the PC. */
    {
        char pb[600];
        snprintf(pb, sizeof pb, "%s/patches.bin", out);
        if (patches_have_60fps(pb)) {
            set_default("BB_VBLANK_HZ", "480");
            printf("SacredFire Android: 60 FPS patch in patches.bin: virtual vblank %s Hz\n",
                   getenv("BB_VBLANK_HZ"));
        }
    }
    set_default("BB_VBLANK_HZ", "60");
    /* Mali: with the Vulkan recording thread (and its submissions) or the draw pipeline the
     * title's display pass came out black (the scene stretched into the display buffer, only
     * the HUD showed). Recording on the GPU command thread showed it (test 23). env.txt can
     * still turn them on. */
    set_default("BB_VK_RECORD_THREAD", "0");
    set_default("BB_DRAW_PIPE", "0");
    /* Mali: two groups of the GPU thread's optimizations mixed images of different frames on
     * the title (the left 640x720 showed another pass's target, or Crash went black there).
     * Off by default, measured on the phone with BB_TOGGLE_FILE at no FPS cost:
     * draw preparation (4096 8192, bits 35 38 40 46) and deferred/parallel uploads (256 16384
     * 131072 262144 524288, bits 36 42). BB_DISABLED_OPTIMIZATIONS=0 turns them back on;
     * a BB_TOGGLE_FILE overrides the mask while the game runs. */
    /* Pipelines of passes drawn every frame compile on worker threads (the driver's compile is
     * the stall on Mali); the frames here are 150-300 draws, so the "not a loading screen"
     * threshold is 100 draws instead of the PC's 300. Opt-in (BB_ASYNC_PIPELINES=1): test32
     * had it on and crashed at the first loading screen (SubmitPendingArenaBinds), together
     * with 64 MiB arenas; which of the two is being checked. */
    set_default("BB_ASYNC_MIN_DRAWS", "100");
    /* Memory: dense buffer arenas (no sparse buffers on Mali) are whole buffers per touched
     * 64 MiB page instead of 256 MiB: in the hub 7 pages of 256 MiB (1.8 GB) were allocated
     * and Android's low-memory killer closed the game. (64 MiB pages crashed in test32: the
     * page table was a fixed array that overflowed past page 1024, fixed in buffer_cache.h.)
     * Textures: a 1 GiB budget (no driver budget on Mali). */
    set_default("BB_DENSE_ARENA_MIB", "64");
    set_default("BB_GC_BUDGET_MB", "1024");
    set_default("BB_DISABLED_OPTIMIZATIONS", "865101651093913504"); /* + parallel recording, readback queue, UpdateImage fast path */ /* all GPU-thread optimization groups off: the only mask without image mixups from the first frame (test videos 1_tudo, 9_tudo) */
    runtime_disabled_optimizations = strtoull(getenv("BB_DISABLED_OPTIMIZATIONS"), NULL, 0);
    printf("SacredFire Android: disabled optimizations mask=%llu\n",
           (unsigned long long)runtime_disabled_optimizations);
    set_default("BB_GPU_USER_DIR", user);
    /* Box64 settings tested on the phone (env.txt can still override them): weaker memory
     * ordering, bigger translated blocks and no exact flags for unused results. */
    set_default("BB_STRONGMEM", "0");
    set_default("BOX64_DYNAREC_BIGBLOCK", "3");
    set_default("BOX64_DYNAREC_SAFEFLAGS", "0");
    set_default("BOX64_DYNAREC_FORWARD", "1024");

    char boot[600], names[600], content[600], patches[600];
    snprintf(boot, sizeof boot, "%s/boot-linked.bin", out);
    snprintf(names, sizeof names, "%s/import_names.txt", out);
    snprintf(content, sizeof content, "%s/content.bin", out);
    snprintf(patches, sizeof patches, "%s/patches.bin", out);
    if (!exists(boot) || !exists(names) || !exists(content)) {
        printf("STOP: the prepared files are missing from %s (boot-linked.bin, import_names.txt, content.bin).\n", out);
        return 2;
    }
    char *argv[16];
    int argc = 0;
    argv[argc++] = "sacredfire";
    argv[argc++] = boot;
    argv[argc++] = "--import-names";
    argv[argc++] = names;
    argv[argc++] = "--content-profile";
    argv[argc++] = content;
    if (exists(patches)) {
        argv[argc++] = "--patches";
        argv[argc++] = patches;
    }
    argv[argc++] = "--app0";
    argv[argc++] = game;
    argv[argc++] = "--user";
    argv[argc++] = user;
    argv[argc++] = "--timeout";
    argv[argc++] = "0";
    argv[argc] = NULL;
    return bb_probe_main(argc, argv);
}
