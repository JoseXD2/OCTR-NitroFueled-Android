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
#include <errno.h>
#include <pthread.h>
#include <time.h>
#include <jni.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "sdl_android.h"

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

static void set_default(const char *name, const char *value) { setenv(name, value, 0); }

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
struct thread_sample { int tid; unsigned long long ticks; char name[32]; };
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
        ++n;
    }
    closedir(dir);
    return n;
}
static void *thread_stats_main(void *arg) {
    (void)arg;
    enum { MAX = 512 };
    static struct thread_sample prev[MAX], cur[MAX];
    int np = read_threads(prev, MAX);
    const long hz = sysconf(_SC_CLK_TCK);
    for (;;) {
        sleep(5);
        int nc = read_threads(cur, MAX);
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
            at += snprintf(line + at, sizeof line - at, " %s %llu%%", cur[top[k]].name,
                           dt[top[k]] * 100 / (5 * hz));
        printf("%s\n", line);
        memcpy(prev, cur, sizeof(cur[0]) * nc);
        np = nc;
    }
    return NULL;
}

int bb_android_main(ANativeActivity *activity) {
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
    if (!getenv("BB_THREAD_STATS") || getenv("BB_THREAD_STATS")[0] != '0') {
        pthread_t stats;
        if (pthread_create(&stats, NULL, thread_stats_main, NULL) == 0) pthread_detach(stats);
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
    set_default("BB_PREUPLOAD", "1");
    set_default("BB_GUEST_IN_PLACE", "0");
    set_default("BB_UFFD", "0");
    set_default("BB_COPY_GPU_BUFFERS", "1");
    set_default("BB_GPU_WRITE_TWINS", "1");
    set_default("BB_GPU_WRITE_TWINS_MAX", "65536");
    set_default("BB_VBLANK_HZ", "60");
    set_default("BB_GPU_USER_DIR", user);

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
