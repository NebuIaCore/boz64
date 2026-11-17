// 64-bit libs3e_android.so: Java loads this in place of the original 32-bit engine. It sets up the
// 32-bit environment, loads the original engine (shipped as libg32_s3e_android.so) and runs it.
#include <dlfcn.h>
#include <jni.h>
#include <sys/system_properties.h>

#include <string>

#include "cpu.h"
#include "elf.h"
#include "vfs.h"
#include "frame.h"
#include "settings.h"
#include "codpoints.h"
#include "pad.h"
#include "lan.h"
#include "patches.h"

extern JavaVM* g_vm;
extern u32 g_guest_vm;
extern std::string g_libdir;
void jni_push_frame();
void jni_pop_frame();

static bool g_started = false;
jclass g_ui_class = nullptr;  // boz64.Ui

static void start(JNIEnv* env) {
    if (g_started) return;
    g_started = true;
    char prop[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.boz64.trace", prop) > 0 && prop[0] == '1') g_trace = true;
    if (__system_property_get("debug.boz64.precise", prop) > 0 && prop[0] == '1') g_precise = true;

    Dl_info info{};
    dladdr(reinterpret_cast<void*>(&start), &info);
    std::string self_path = info.dli_fname ? info.dli_fname : "";
    g_libdir = self_path.substr(0, self_path.rfind('/'));
    LOGI("64-bit wrapper starting from %s", g_libdir.c_str());

    vfs_init(g_libdir);
    frame_init();
    g::init_memory();
    init_cpu();
    register_libc();
    register_jni();
    register_gl();
    self().env = env;
}

extern "C" JNIEXPORT jint JNI_OnLoad(JavaVM* vm, void*) {
    g_vm = vm;
    JNIEnv* env = nullptr;
    vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
    start(env);
    // The port's Java side (java/boz64/Ui.java, in classes2.dex): full screen, the files folder, the refresh
    // rate. Looked up here, where the app's class loader is used; other threads use the global reference.
    std::string files_dir;
    if (jclass ui = env->FindClass("boz64/Ui")) {
        g_ui_class = static_cast<jclass>(env->NewGlobalRef(ui));
        if (jmethodID install = env->GetStaticMethodID(ui, "install", "()V")) env->CallStaticVoidMethod(ui, install);
        if (jmethodID dir = env->GetStaticMethodID(ui, "filesDir", "()Ljava/lang/String;")) {
            if (auto js = static_cast<jstring>(env->CallStaticObjectMethod(ui, dir))) {
                const char* c = env->GetStringUTFChars(js, nullptr);
                files_dir = c;
                env->ReleaseStringUTFChars(js, c);
                env->DeleteLocalRef(js);
            }
        }
        env->DeleteLocalRef(ui);
    }
    if (env->ExceptionCheck()) {
        env->ExceptionDescribe();
        env->ExceptionClear();
    }
    if (files_dir.empty()) files_dir = "/data/data/com.activision.boz64/files";
    settings_init(files_dir);
    codpoints_init(files_dir);
    lan_init(files_dir);
    pad_register(env);

    GuestLib* lib = elf_load(g_libdir + "/libg32_s3e_android.so", "libs3e_android.so");
    if (!lib) fatal("could not load the original 32-bit engine");
    patches_on_engine(lib->bias);
    u32 onload = lib->lookup("JNI_OnLoad");
    if (!onload) fatal("the 32-bit engine has no JNI_OnLoad");
    self().env = env;
    jni_push_frame();
    u32 version = static_cast<u32>(call_guest(onload, {g_guest_vm, 0}));
    jni_pop_frame();
    LOGI("32-bit JNI_OnLoad returned 0x%x", version);
    return static_cast<jint>(version);
}
