// boz64 for Linux, the BSDs, macOS and iOS: Call of Duty: Black Ops Zombies, its original 32-bit ARM engine and game
// run by the port's dynarmic-based runtime, with SDL3 for the window, input and sound. main_win.cpp is the Windows
// version; this does the same startup: load the engine, call its JNI_OnLoad, and run the loader's startup sequence.
//
// The game files are the Windows build's data folder, copied into the folder BOZ64 keeps its files in
// (boz_exe_dir, see boz_unix.h), in the layout the game sees on Android:
//   data/data/app/com.activision.boz/base.apk                    the APK (the engine reads its assets)
//   data/data/app/com.activision.boz/lib/arm/*.so                the original 32-bit engine and extensions
//   data/sdcard/Android/obb/com.activision.boz/blackops_etc.dz   (+ .dat) the game data pack
//   data/data/data/com.activision.boz/files                      saves and settings
// Packages don't carry them: the first start says where to put them.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "codpoints.h"
#include "cpu.h"
#include "elf.h"
#include "frame.h"
#include "jvm.h"
#include "lan.h"
#include "patches.h"
#include "platform.h"
#include "settings.h"

extern u32 g_guest_vm;
extern std::string g_libdir;  // src/libc.cpp: folder holding the original 32-bit libraries
void jni_push_frame();
void jni_pop_frame();

namespace {

const char* APK = "/data/app/com.activision.boz/base.apk";
const char* LIBS = "/data/app/com.activision.boz/lib/arm";
const char* FILES = "/data/data/com.activision.boz/files";
const char* OBB = "/sdcard/Android/obb/com.activision.boz/blackops_etc.dz";

bool exists(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0;
}

void fail_box(const std::string& msg) {
    LOGE("%s", msg.c_str());
    boz_log_flush();
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "BOZ64", msg.c_str(), nullptr);
}

std::string log_path() { return std::string(boz_exe_dir()) + "/boz64.log"; }

void on_abort(int) {
    if (marm_quitting()) {
        // The game's own teardown after "Quit" crashes (see main_win.cpp): by then it has saved, so this is the
        // normal end of a quit.
        LOGI("the game's shutdown stopped at its usual crash after quitting; exiting normally");
        boz_log_flush();
        _exit(0);
    }
    boz_log_flush();
    std::string msg = "The game stopped with an error. Details are at the end of " + log_path() + ".";
    SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "BOZ64", msg.c_str(), nullptr);
    _exit(3);
}

// A host crash outside the JIT's own faults (dynarmic installs its handler later and passes others on to this one).
void on_crash(int sig, siginfo_t* si, void*) {
    LOGE("host crash: signal %d at %p", sig, si ? si->si_addr : nullptr);
    if (marm_quitting()) {
        boz_log_flush();
        _exit(0);
    }
    boz_log_flush();
    _exit(3);
}

int prop_int(const char* name, int def) {
    char v[PROP_VALUE_MAX] = {};
    return __system_property_get(name, v) > 0 ? atoi(v) : def;
}

}  // namespace

int main(int, char**) {
    signal(SIGABRT, on_abort);
    signal(SIGPIPE, SIG_IGN);  // a LAN peer going away must not end the game
    struct sigaction sa{};
    sa.sa_sigaction = on_crash;
    sa.sa_flags = SA_SIGINFO;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);

    char prop[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.boz64.trace", prop) > 0 && prop[0] == '1') g_trace = true;
    if (__system_property_get("debug.boz64.precise", prop) > 0 && prop[0] == '1') g_precise = true;
    LOGI("boz64 for %s starting; files in %s", SDL_GetPlatform(), boz_exe_dir());

    g_libdir = boz_host_path(LIBS);
    for (const std::string& f : {g_libdir + "/libs3e_android.so", boz_host_path(APK), boz_host_path(OBB)}) {
        if (!exists(f)) {
            std::string data = std::string(boz_exe_dir()) + "/data";
            mkdir(data.c_str(), 0755);
            fail_box("BOZ64 needs the game files.\n\nCopy everything inside the \"data\" folder of BOZ64 PC (the Windows "
                     "version) into:\n" + data + "\n\nMissing: " + f);
            SDL_OpenURL(("file://" + data).c_str());
            return 1;
        }
    }

    frame_init();
    g::init_memory();
    init_cpu();
    register_libc();
    register_jni();
    register_gl();
    jvm_init();
    marm_define_classes();
    self().env = &g_jenv;

    std::string files_host = boz_host_path(FILES);
    settings_init(files_host);
    codpoints_init(files_host);
    lan_init(files_host);

    if (!win_create(prop_int("debug.boz64.width", 1280), prop_int("debug.boz64.height", 720))) {
        fail_box(std::string("Could not create the game window: ") + SDL_GetError());
        return 1;
    }
    audio_open();

    GuestLib* lib = elf_load(g_libdir + "/libs3e_android.so", "libs3e_android.so");
    if (!lib) fatal("could not load the original 32-bit engine");
    patches_on_engine(lib->bias);
    u32 onload = lib->lookup("JNI_OnLoad");
    if (!onload) fatal("the 32-bit engine has no JNI_OnLoad");
    jlocal_push();
    jni_push_frame();
    u32 version = static_cast<u32>(call_guest(onload, {g_guest_vm, 0}));
    jni_pop_frame();
    LOGI("32-bit JNI_OnLoad returned 0x%x", version);

    marm_start(FILES, APK, LIBS);
    int code = win_run_loop();
    LOGI("exiting (%d)", code);
    boz_log_flush();
    _exit(code);  // as on Windows: the engine thread may still be inside guest code
}
