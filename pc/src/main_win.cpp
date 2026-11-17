// boz64.exe: Call of Duty: Black Ops Zombies on Windows. The original 32-bit ARM engine and game run under the
// port's dynarmic-based runtime (as in the 64-bit Android build); this file does what Android and the app's Java
// code do at startup: load the engine, call its JNI_OnLoad, and run the loader's startup sequence.
//
// Files (all under data\ next to the .exe, in the layout the game sees on Android):
//   data\data\app\com.activision.boz\base.apk                the original APK (the engine reads its assets)
//   data\data\app\com.activision.boz\lib\arm\*.so            the original 32-bit engine and extensions
//   data\sdcard\Android\obb\com.activision.boz\blackops_etc.dz (+ .dat)   the game data pack
//   data\data\data\com.activision.boz\files                  saves and settings
// plus libEGL.dll, libGLESv2.dll (ANGLE) and zlib1.dll next to the .exe. tools/setup_pc_data.ps1 copies them.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmsystem.h>

#include <sys/system_properties.h>

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
extern std::string g_libdir;
void jni_push_frame();
void jni_pop_frame();
void boz_log_flush();

namespace {

const char* APK = "/data/app/com.activision.boz/base.apk";
const char* LIBS = "/data/app/com.activision.boz/lib/arm";
const char* FILES = "/data/data/com.activision.boz/files";
const char* OBB = "/sdcard/Android/obb/com.activision.boz/blackops_etc.dz";

bool exists(const std::string& host) {
    int n = MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, host.c_str(), -1, w.data(), n);
    return GetFileAttributesW(w.c_str()) != INVALID_FILE_ATTRIBUTES;
}

void fail_box(const std::string& msg) {
    LOGE("%s", msg.c_str());
    boz_log_flush();
    MessageBoxA(nullptr, msg.c_str(), "boz64", MB_OK | MB_ICONERROR);
}

void on_abort(int) {
    if (marm_quitting()) {
        // The game's own teardown after "Quit" crashes: its last step uses the memory manager an earlier step already
        // shut down (FUN_0260e544 -> FUN_024e8404 in the game image). By then it has saved and freed the game, so
        // this is the normal end of a quit: no error box.
        LOGI("the game's shutdown stopped at its usual crash after quitting; exiting normally");
        boz_log_flush();
        _flushall();
        TerminateProcess(GetCurrentProcess(), 0);
    }
    boz_log_flush();
    _flushall();
    MessageBoxA(nullptr, "The game stopped with an error. Details are at the end of boz64.log next to boz64.exe.",
                "boz64", MB_OK | MB_ICONERROR | MB_SETFOREGROUND | MB_TOPMOST);
    TerminateProcess(GetCurrentProcess(), 3);
}

LONG WINAPI on_crash(EXCEPTION_POINTERS* e) {
    LOGE("host crash: exception 0x%08lx at %p", e->ExceptionRecord->ExceptionCode, e->ExceptionRecord->ExceptionAddress);
    on_abort(0);
    return EXCEPTION_EXECUTE_HANDLER;
}

int prop_int(const char* name, int def) {
    char v[PROP_VALUE_MAX] = {};
    return __system_property_get(name, v) > 0 ? atoi(v) : def;
}

}  // namespace

int main() {
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    signal(SIGABRT, on_abort);
    SetUnhandledExceptionFilter(on_crash);
    timeBeginPeriod(1);

    char prop[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.boz64.trace", prop) > 0 && prop[0] == '1') g_trace = true;
    if (__system_property_get("debug.boz64.precise", prop) > 0 && prop[0] == '1') g_precise = true;
    LOGI("boz64 for Windows starting in %s", boz_exe_dir());

    g_libdir = boz_host_path(LIBS);
    for (const std::string& f : {g_libdir + "/libs3e_android.so", boz_host_path(APK), boz_host_path(OBB)}) {
        if (!exists(f)) {
            fail_box("A game file is missing:\n" + f + "\n\nRun tools\\setup_pc_data.ps1 to copy the game files next to boz64.exe.");
            return 1;
        }
    }
    for (const char* dll : {"libEGL.dll", "libGLESv2.dll", "zlib1.dll"}) {
        if (!exists(std::string(boz_exe_dir()) + "/" + dll)) {
            fail_box(std::string(dll) + " is missing next to boz64.exe.\n\nRun tools\\setup_pc_data.ps1.");
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
        fail_box("Could not create the game window.");
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
    _flushall();
    TerminateProcess(GetCurrentProcess(), static_cast<UINT>(code));
    return code;
}
