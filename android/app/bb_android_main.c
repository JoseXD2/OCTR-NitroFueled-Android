/* SacredFire Android: the app's main thread (started by sdl_android's NativeActivity glue). Gets
 * access to the shared storage, sets what run.sh sets on the desktop and starts the loader
 * (src/probe.c) on the prepared game.
 *
 * Folder layout on the device (/sdcard/SacredFire, BB_DATA_DIR):
 *   CUSA13795/      the game: the 1.00 dump with the 1.21 update copied over it
 *   out/            the prepared files (boot-linked.bin, import_names.txt, content.bin, patches.bin)
 *   user/           saves, shader cache, settings written by the game
 *   bbport.ini      settings (the in-game menu writes it)
 *   sacredfire.log  the log of the last run */
#define _GNU_SOURCE
#include <android/native_activity.h>
#include <errno.h>
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

int bb_android_main(ANativeActivity *activity) {
    wait_for_storage(activity);
    mkdir(DATA_DIR, 0775);
    mkdir(DATA_DIR "/user", 0775);
    const char *data = getenv("BB_DATA_DIR") ? getenv("BB_DATA_DIR") : DATA_DIR;
    char game[512], out[512], user[512], config[512];
    snprintf(game, sizeof game, "%s/CUSA13795", data);
    snprintf(out, sizeof out, "%s/out", data);
    snprintf(user, sizeof user, "%s/user", data);
    snprintf(config, sizeof config, "%s/bbport.ini", data);
    printf("SacredFire Android: game %s, prepared files %s, log %s\n", game, out, bb_android_log_path());

    char eboot[600];
    snprintf(eboot, sizeof eboot, "%s/eboot.bin", game);
    if (!exists(eboot)) {
        printf("STOP: no eboot.bin in %s. Copy the game (1.00 with the 1.21 update over it) there.\n", game);
        return 2;
    }
    /* What run.sh sets for the game (docs/sacredfire), the same defaults. */
    set_default("BB_CONFIG", config);
    set_default("BB_GAME_PROFILE", "ctr");
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
