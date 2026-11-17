// The original app's Java side (Marmalade loader 8.0.1, com.ideaworks3d.marmalade), written in C++ for the stand-in
// JVM: the classes and methods the engine looks up, and the startup sequence Java runs. Behaviour follows the
// app's bytecode (see the notes on each method); Android-only services answer as "not available".
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#else
// the Unix PC builds (Linux, the BSDs, macOS, iOS): POSIX, with SDL for the window's scale, links and dialogs
#include <SDL3/SDL.h>
#include <pthread.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>

#include <chrono>
#include <thread>
#endif

#include <algorithm>
#include <atomic>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "common.h"
#include "jvm.h"
#include "platform.h"

jclass g_ui_class = nullptr;  // the port's boz64.Ui class (frame.cpp asks it for the refresh rate)

namespace {

constexpr const char* PKG = "com/ideaworks3d/marmalade/";

JClass *LT, *MPM, *LV, *LK, *SP, *SR, *API, *LA, *TP;
JField *f_mpm, *f_kbd;
JObj *g_activity, *g_lt, *g_mpm, *g_sp;
std::mutex view_lock;
JObj* g_view = nullptr;    // the current LoaderView
JArray* g_pixels = nullptr;  // the surface's int[] (the software frame buffer; unused with GL, but must exist)
int g_w = 0, g_h = 0;

std::atomic<bool> g_started{false}, g_paused{false}, g_use_gl{false};
std::atomic<bool> g_quitting{false};  // see marm_quitting
std::atomic<int> g_init_term{0};  // m_AppDoingInitTerm
#ifdef _WIN32
HANDLE g_eng = nullptr;
#else
bool g_eng = false;                   // the engine thread was started
std::atomic<bool> g_eng_done{false};  // and has finished
#endif

// guest paths handed to the engine
std::string g_files, g_apk, g_libs;
const std::string g_cache = "/data/data/com.activision.boz/cache";
const std::string g_ext_files = "/sdcard/Android/data/com.activision.boz/files";
const std::string g_ext_cache = "/sdcard/Android/data/com.activision.boz/cache";
const std::string g_rst = "/sdcard";

// sound
std::mutex sfx_lock;
JArray* g_sfx_buf = nullptr;
std::atomic<bool> g_sfx_on{false};
std::atomic<int> g_sfx_channels{2};

JClass* cls(const char* short_name, bool stub = true) { return jclass_def((std::string(PKG) + short_name).c_str(), "java/lang/Object", stub); }

jvalue jstr(const std::string& s) { return jv_obj(jstring_new(s)); }
jvalue jstr_or_null(const char* s) { return jv_obj(jstring_new_or_null(s)); }

#ifdef _WIN32
std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}

FILE* host_fopen(const std::string& path, const wchar_t* mode) { return _wfopen(widen(path).c_str(), mode); }

void mkdirs(const std::string& guest) {
    std::wstring w = widen(boz_host_path(guest.c_str()));
    for (size_t i = 3; i <= w.size(); i++)
        if (i == w.size() || w[i] == L'/' || w[i] == L'\\') CreateDirectoryW(w.substr(0, i).c_str(), nullptr);
}
#else
FILE* host_fopen(const std::string& path, const wchar_t* mode) { return fopen(path.c_str(), mode[0] == L'w' ? "wb" : "rb"); }

void mkdirs(const std::string& guest) {
    std::string h = boz_host_path(guest.c_str());
    for (size_t i = 1; i <= h.size(); i++)
        if (i == h.size() || h[i] == '/') mkdir(h.substr(0, i).c_str(), 0755);
}
#endif

JObj* current_view() {
    std::lock_guard lk(view_lock);
    return g_view;
}

JObj* make_view() {
    JObj* v = jnew(LV);
    JObj* k = jnew(LK);
    jfield_set(v, f_kbd, jv_obj(k));
    junref(k);
    return v;  // owned
}

// LoaderThread.setViewNative(view)
void set_view_native(JObj* v) { jnative_call(LT, "setViewNative", "(Lcom/ideaworks3d/marmalade/LoaderView;)V", g_lt, {jv_obj(v)}); }

// LoaderView.surfaceChanged: (suspend,) new int[w*h], setPixelsNative(w, h, pixels, newly), (resume)
void surface_set(JObj* view, int w, int h, bool newly, bool suspend) {
    if (suspend) jnative_call(LT, "suspendAppThreads", "()V", g_lt, {});
    JArray* old = g_pixels;
    g_pixels = jarray_new('I', w * h);
    g_w = w;
    g_h = h;
    jnative_call(LV, "setPixelsNative", "(II[IZ)V", view, {jv_int(w), jv_int(h), jv_obj(g_pixels), jv_bool(newly)});
    if (old) junref(old);
    if (suspend) jnative_call(LT, "resumeAppThreads", "()V", g_lt, {});
}

// LoaderAPI.s3eConfigGetInt(group, key, int[1]): 0 on success
bool icf_int(const char* group, const char* key, int& out) {
    JString* g = jstring_new(group);
    JString* k = jstring_new(key);
    JArray* a = jarray_new('I', 1);
    jvalue r = jnative_call(API, "s3eConfigGetInt", "(Ljava/lang/String;Ljava/lang/String;[I)I", nullptr,
                            {jv_obj(g), jv_obj(k), jv_obj(a)});
    bool ok = r.i == 0;
    if (ok) memcpy(&out, a->data.data(), 4);
    junref(g);
    junref(k);
    junref(a);
    return ok;
}

// LoaderGL.chooseConfigs: the [GL] settings that are set, in this order
std::vector<int> gl_config_attribs() {
    static const std::pair<const char*, int> keys[] = {
        {"EGL_BUFFER_SIZE", 0x3020}, {"EGL_DEPTH_ENCODING_NV", 0x30E2}, {"EGL_DEPTH_SIZE", 0x3025},
        {"EGL_SURFACE_TYPE", 0x3033}, {"EGL_RED_SIZE", 0x3024},        {"EGL_GREEN_SIZE", 0x3023},
        {"EGL_BLUE_SIZE", 0x3022},   {"EGL_ALPHA_SIZE", 0x3021},       {"EGL_STENCIL_SIZE", 0x3026},
        {"EGL_SAMPLE_BUFFERS", 0x3032}, {"EGL_SAMPLES", 0x3031}};
    std::vector<int> v;
    for (auto& [k, attr] : keys) {
        int val = -1;
        if (icf_int("GL", k, val) && val != -1) {
            LOGI("[GL] %s = %d", k, val);
            if (attr == 0x30E2) continue;  // NVIDIA-only; ANGLE rejects it
            v.push_back(attr);
            v.push_back(val);
        }
    }
    v.push_back(0x3038);  // EGL_NONE
    // the engine's heaps (its [S3E] MemSize settings), for out-of-memory reports
    for (const char* k : {"MemSize", "MemSize0", "MemSize1", "MemSize2", "MemSizeDebug"}) {
        int val = -1;
        if (icf_int("S3E", k, val)) LOGI("[S3E] %s = %d", k, val);
    }
    return v;
}

#ifdef _WIN32
DWORD WINAPI eng_main(void*) {
#else
void* eng_main(void*) {
#endif
    jlocal_push();
    // LoaderThread.run: waitForView (setViewNative on this thread), then runNative(files, apk, libs)
    set_view_native(current_view());
    g_started = true;
    JString* a = jstring_new(g_files);
    JString* b = jstring_new(g_apk);
    JString* c = jstring_new(g_libs);
    LOGI("runNative(%s, %s, %s)", g_files.c_str(), g_apk.c_str(), g_libs.c_str());
    jnative_call(LT, "runNative", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)V", g_lt,
                 {jv_obj(a), jv_obj(b), jv_obj(c)});
    LOGI("runNative returned: the game has quit");
    junref(a);
    junref(b);
    junref(c);
    jlocal_pop();
    win_quit(0);
#ifdef _WIN32
    return 0;
#else
    g_eng_done = true;
    return nullptr;
#endif
}

std::string device_id() {
    u64 h = 1469598103934665603ull;
#ifdef _WIN32
    wchar_t name[256] = {};
    DWORD n = 256;
    GetComputerNameW(name, &n);
    for (DWORD i = 0; i < n; i++) h = (h ^ name[i]) * 1099511628211ull;
#else
    char name[256] = {};
    gethostname(name, sizeof name - 1);
    for (size_t i = 0; name[i]; i++) h = (h ^ static_cast<unsigned char>(name[i])) * 1099511628211ull;
#endif
    char b[32];
    snprintf(b, sizeof b, "%016llx", static_cast<unsigned long long>(h));
    return b;
}

void define_loader_thread() {
    LT = cls("LoaderThread");
    MPM = cls("LoaderThread$MediaPlayerManager");
    f_mpm = jfield_def(LT, "m_MediaPlayerManager", "Lcom/ideaworks3d/marmalade/LoaderThread$MediaPlayerManager;");
    auto def = [](const char* n, const char* s, JImpl f) { jmethod_def(LT, n, s, std::move(f)); };
    auto vv = [](JObj*, const jvalue*) { return jv_void(); };

    def("doSuspend", "()V", [](JObj*, const jvalue*) {
        LOGI("doSuspend");
        music_suspend();
        audio_suspend();
        return jv_void();
    });
    def("doResume", "()V", [](JObj*, const jvalue*) {
        LOGI("doResume");
        set_view_native(current_view());  // waitForView
        audio_resume();
        music_resume_all();
        return jv_void();
    });
    def("fixOrientation", "(I)V", vv);
    def("touchSetWait", "(I)V", vv);
    def("soundInit", "(IZI)I", [](JObj*, const jvalue* a) {
        int r = audio_sfx_init(a[0].i, a[1].z, a[2].i);
        LOGI("soundInit(volume %d, %s, rate %d) -> %d", a[0].i, a[1].z ? "stereo" : "mono", a[2].i, r);
        g_sfx_channels = a[1].z ? 2 : 1;
        return jv_int(r);
    });
    def("soundStart", "()V", [](JObj*, const jvalue*) {
        LOGI("soundStart");
        g_sfx_on = true;
        audio_sfx_start();
        return jv_void();
    });
    def("soundStop", "()V", [](JObj*, const jvalue*) {
        LOGI("soundStop");
        g_sfx_on = false;
        audio_sfx_stop();
        return jv_void();
    });
    def("soundSetVolume", "(I)V", [](JObj*, const jvalue* a) {
        audio_sfx_volume(a[0].i);
        return jv_void();
    });
    def("getSilentMode", "()Z", [](JObj*, const jvalue*) { return jv_bool(false); });
    def("getPrivateExternalDir", "()Ljava/lang/String;", [](JObj*, const jvalue*) { return jstr(g_ext_files); });
    def("getRstDir", "()Ljava/lang/String;", [](JObj*, const jvalue*) { return jstr(g_rst); });
    def("getCacheDir", "()Ljava/lang/String;", [](JObj*, const jvalue*) { return jstr(g_cache); });
    def("getTmpDir", "()Ljava/lang/String;", [](JObj*, const jvalue*) { return jstr(g_ext_cache); });
    def("glInit", "(I)V", [](JObj*, const jvalue* a) {
        // On Android the whole SurfaceView is rebuilt as a GL view first: the UI thread installs it
        // (setViewNative) and its surface arrives (setPixelsNative, without a suspend); then this thread picks the
        // view up and creates the EGL context and surface on itself.
        if (g_use_gl.exchange(true)) return jv_void();
        int version = a[0].i;
        LOGI("glInit(%d)", version);
        g_init_term++;
        ui_call([] {
            JObj* v = make_view();
            JObj* old;
            {
                std::lock_guard lk(view_lock);
                old = g_view;
                g_view = v;
            }
            set_view_native(v);
            surface_set(v, win_width(), win_height(), true, false);
            if (old) junref(old);
        });
        set_view_native(current_view());
        std::vector<int> attribs = gl_config_attribs();
        if (!egl_start(version, attribs.data())) fatal("could not create the OpenGL ES %d context (ANGLE)", version);
        g_init_term--;
        return jv_void();
    });
    def("glReInit", "()V", [](JObj*, const jvalue*) {
        LOGI("glReInit");
        egl_restart_surface();
        return jv_void();
    });
    def("glTerm", "()V", [](JObj*, const jvalue*) {
        LOGI("glTerm");
        if (!g_paused) g_quitting = true;  // not for a suspend: the game is shutting down
        egl_stop();
        g_use_gl = false;
        return jv_void();
    });
    def("hasMultitouch", "()Z", [](JObj*, const jvalue*) { return jv_bool(true); });
    def("getBatteryLevel", "()I", [](JObj*, const jvalue*) { return jv_int(100); });
    def("chargerIsConnected", "()Z", [](JObj*, const jvalue*) { return jv_bool(true); });
    def("getDeviceId", "()Ljava/lang/String;", [](JObj*, const jvalue*) { return jstr(device_id()); });
    def("getDeviceModel", "()Ljava/lang/String;", [](JObj*, const jvalue*) { return jstr("PC"); });
    def("getDeviceIMSI", "()Ljava/lang/String;", [](JObj*, const jvalue*) { return jv_obj(nullptr); });
    def("getDeviceNumber", "()Ljava/lang/String;", [](JObj*, const jvalue*) { return jv_obj(nullptr); });
    def("getNetworkType", "()I", [](JObj*, const jvalue*) { return jv_int(1); });  // WIFI
    def("getNetworkSubType", "()I", [](JObj*, const jvalue*) { return jv_int(0); });
    def("getOrientation", "()I", [](JObj*, const jvalue*) { return jv_int(0); });
    def("getDeviceDpi", "(Z)I", [](JObj*, const jvalue*) {
#ifdef _WIN32
        UINT dpi = 96;
        if (HWND h = static_cast<HWND>(win_hwnd())) dpi = GetDpiForWindow(h);
        return jv_int(static_cast<jint>(160 * dpi / 96));
#else
        float scale = win_hwnd() ? SDL_GetWindowDisplayScale(static_cast<SDL_Window*>(win_hwnd())) : 1.0f;
        return jv_int(static_cast<jint>(160 * (scale > 0 ? scale : 1.0f)));
#endif
    });
    def("accelStart", "()V", vv);
    def("accelStop", "()V", vv);
    def("compassStart", "()V", vv);
    def("compassStop", "()V", vv);
    def("locationStart", "()Z", [](JObj*, const jvalue*) { return jv_bool(false); });
    def("locationStop", "()Z", [](JObj*, const jvalue*) { return jv_bool(false); });
    def("locationGpsData", "()Z", [](JObj*, const jvalue*) { return jv_bool(false); });
    def("recordAvailable", "()I", [](JObj*, const jvalue*) { return jv_int(0); });
    def("recordStart", "(I)I", [](JObj*, const jvalue*) { return jv_int(0); });
    def("recordStop", "()I", [](JObj*, const jvalue*) { return jv_int(0); });
    def("clipboardGet", "()Ljava/lang/String;", [](JObj*, const jvalue*) { return jv_obj(nullptr); });
    def("clipboardSet", "(Ljava/lang/String;)V", vv);
    def("networkCheckStart", "()Z", [](JObj*, const jvalue*) { return jv_bool(true); });
    def("networkCheckStop", "()Z", [](JObj*, const jvalue*) { return jv_bool(true); });
    def("runRunnable", "(Ljava/lang/Runnable;)V", [](JObj*, const jvalue* a) {
        JObj* r = O(a[0].l);
        if (r && r->host) (*static_cast<std::function<void()>*>(r->host))();
        else LOGW("runRunnable with a Runnable the PC build did not make");
        return jv_void();
    });
    def("runOnOSSignal", "()V", [](JObj*, const jvalue*) {
        ui_post([] { marm_run_on_os_tick(); });
        return jv_void();
    });
    def("acquireMulticastLock", "()Z", [](JObj*, const jvalue*) { return jv_bool(true); });
    def("releaseMulticastLock", "()Z", [](JObj*, const jvalue*) { return jv_bool(true); });
    def("sendEmail", "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Z", [](JObj*, const jvalue*) { return jv_bool(false); });
    def("launchBrowser", "(Ljava/lang/String;)Z", [](JObj*, const jvalue* a) {
        std::string url = jstring_utf8(a[0].l);
        LOGI("launchBrowser(%s)", url.c_str());
        if (url.rfind("http://", 0) != 0 && url.rfind("https://", 0) != 0) return jv_bool(false);
#ifdef _WIN32
        ShellExecuteW(nullptr, L"open", widen(url).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
        SDL_OpenURL(url.c_str());
#endif
        return jv_bool(true);
    });
    def("smsStart", "()Z", [](JObj*, const jvalue*) { return jv_bool(false); });
    def("smsStop", "()V", vv);

    // LoaderThread$MediaPlayerManager: 16 music channels (MediaPlayer each)
    auto mdef = [](const char* n, const char* s, JImpl f) { jmethod_def(MPM, n, s, std::move(f)); };
    mdef("audioPlay", "(Ljava/lang/String;IJJI)I", [](JObj*, const jvalue* a) {
        std::string path = jstring_utf8(a[0].l);
        int r = music_play(boz_host_path(path.c_str()), a[1].i, a[2].j, a[3].j, a[4].i);
        LOGI("audioPlay(%s, repeat %d, offset %lld, size %lld, channel %d) -> %d", path.c_str(), a[1].i,
             static_cast<long long>(a[2].j), static_cast<long long>(a[3].j), a[4].i, r);
        return jv_int(r);
    });
    mdef("audioStop", "(I)V", [](JObj*, const jvalue* a) {
        music_stop(a[0].i);
        return jv_void();
    });
    mdef("audioPause", "(I)I", [](JObj*, const jvalue* a) { return jv_int(music_pause(a[0].i)); });
    mdef("audioResume", "(I)I", [](JObj*, const jvalue* a) { return jv_int(music_resume(a[0].i)); });
    mdef("audioGetPosition", "(I)I", [](JObj*, const jvalue* a) { return jv_int(music_position_ms(a[0].i)); });
    mdef("audioSetPosition", "(II)V", [](JObj*, const jvalue* a) {  // (ms, channel)
        music_set_position_ms(a[1].i, a[0].i);
        return jv_void();
    });
    mdef("audioGetStatus", "(I)I", [](JObj*, const jvalue* a) { return jv_int(music_status(a[0].i)); });
    mdef("audioGetDuration", "(I)I", [](JObj*, const jvalue* a) { return jv_int(music_duration_ms(a[0].i)); });
    mdef("audioSetVolume", "(II)V", [](JObj*, const jvalue* a) {  // (volume, channel)
        music_set_volume(a[1].i, a[0].i);
        return jv_void();
    });
    mdef("audioIsPlaying", "(I)Z", [](JObj*, const jvalue* a) { return jv_bool(music_is_playing(a[0].i)); });
    mdef("audioGetNumChannels", "()I", [](JObj*, const jvalue*) { return jv_int(MUSIC_CHANNELS); });
}

void define_loader_view() {
    LV = cls("LoaderView");
    LK = cls("LoaderKeyboard");
    f_kbd = jfield_def(LV, "m_LoaderKeyboard", "Lcom/ideaworks3d/marmalade/LoaderKeyboard;");
    auto def = [](const char* n, const char* s, JImpl f) { jmethod_def(LV, n, s, std::move(f)); };
    auto vv = [](JObj*, const jvalue*) { return jv_void(); };
    def("doDraw", "()V", vv);  // software frames: the game uses GL
    def("glSwapBuffers", "()V", [](JObj*, const jvalue*) {
        if (g_use_gl) egl_swap();
        return jv_void();
    });
    def("vibrateStart", "(J)V", vv);
    def("vibrateStop", "()V", vv);
    def("vibrateAvailable", "()Z", [](JObj*, const jvalue*) { return jv_bool(false); });
    def("showError", "(Ljava/lang/String;Ljava/lang/String;I)I", [](JObj*, const jvalue* a) {
        // Buttons: Continue (0), plus Stop (1) when type > 0, plus Ignore (2) when type > 1.
        std::string title = jstring_utf8(a[0].l), text = jstring_utf8(a[1].l);
        int type = a[2].i;
        LOGW("showError(%s): %s", title.c_str(), text.c_str());
#ifdef _WIN32
        UINT style = type <= 0 ? MB_OK : type == 1 ? MB_OKCANCEL : MB_YESNOCANCEL;
        std::wstring t = widen(title), b = widen(text);
        if (type == 1) b += L"\n\nOK = Continue, Cancel = Stop";
        if (type > 1) b += L"\n\nYes = Continue, No = Stop, Cancel = Ignore";
        int r = MessageBoxW(nullptr, b.c_str(), t.c_str(), style | MB_ICONWARNING | MB_TOPMOST);
        int code = 0;
        if (type == 1) code = r == IDCANCEL ? 1 : 0;
        if (type > 1) code = r == IDNO ? 1 : r == IDCANCEL ? 2 : 0;
        return jv_int(code);
#else
        // the button ids are the codes the engine expects: Continue 0, Stop 1, Ignore 2
        const SDL_MessageBoxButtonData buttons[] = {{SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 0, "Continue"},
                                                    {0, 1, "Stop"},
                                                    {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 2, "Ignore"}};
        SDL_MessageBoxData box{SDL_MESSAGEBOX_WARNING, static_cast<SDL_Window*>(win_hwnd()), title.c_str(),
                               text.c_str(), type <= 0 ? 1 : type == 1 ? 2 : 3, buttons, nullptr};
        int code = 0;
        if (!SDL_ShowMessageBox(&box, &code)) code = 0;
        return jv_int(code);
#endif
    });
    def("backlightOn", "()V", vv);
    def("getLocale", "()Ljava/lang/String;", [](JObj*, const jvalue*) { return jstr("en_US"); });
    def("getInputString", "(Ljava/lang/String;Ljava/lang/String;I)V", [](JObj* thiz, const jvalue* a) {
        // Android shows a dialog and answers later with setInputText; there is no text entry on the PC yet, so the
        // answer is "cancelled".
        LOGI("getInputString(%s): no text entry on the PC yet; cancelled", jstring_utf8(a[0].l).c_str());
        JObj* v = jref(thiz);
        ui_post([v] {
            jnative_call(LV, "setInputText", "(Ljava/lang/String;)V", v, {jv_obj(nullptr)});
            junref(v);
        });
        return jv_void();
    });
    def("videoPlay", "(Ljava/lang/String;IIIIIIZJJ)I", [](JObj*, const jvalue* a) {
        LOGI("videoPlay(%s): no video on the PC", jstring_utf8(a[0].l).c_str());
        return jv_int(-1);
    });
    def("videoStop", "()V", vv);
    def("videoSetVolume", "(I)V", vv);
    def("videoPause", "()I", [](JObj*, const jvalue*) { return jv_int(-1); });
    def("videoResume", "()I", [](JObj*, const jvalue*) { return jv_int(-1); });
    def("videoGetStatus", "()I", [](JObj*, const jvalue*) { return jv_int(0); });
    def("videoGetPosition", "()I", [](JObj*, const jvalue*) { return jv_int(0); });
    def("runOnOSThread", "()V", [](JObj* thiz, const jvalue*) {
        // blocks until LoaderView.runOnOSThreadNative has run on the UI thread
        ui_call([thiz] { jnative_call(LV, "runOnOSThreadNative", "()V", thiz, {}); });
        return jv_void();
    });
    def("enableRespondingToRotation", "()V", vv);

    jmethod_def(LK, "setShowOnScreenKeyboard", "(Z)V", vv);
    jmethod_def(LK, "getKeyboardInfo", "()I", [](JObj*, const jvalue*) { return jv_int(1); });  // qwerty
}

void define_others() {
    SP = cls("SoundPlayer");
    SR = cls("SoundRecord");
    API = cls("LoaderAPI");
    LA = cls("LoaderActivity");
    TP = cls("s3eTouchpad");
    jmethod_def(TP, "s3eTouchpadInit", "()Z", [](JObj*, const jvalue*) { return jv_bool(false); }, true);
    jmethod_def(TP, "s3eTouchpadGetInt", "(I)I", [](JObj*, const jvalue*) { return jv_int(0); }, true);
    jmethod_def(TP, "s3eTouchpadTerminate", "()V", [](JObj*, const jvalue*) { return jv_void(); }, true);
    for (const char* n : {"LoaderSMSReceiver", "LoaderLocation", "s3eCamera", "s3eCamera2", "CameraCapture",
                          "s3eContacts", "s3eImagePicker", "s3eTest", "LoaderActivitySlave", "LoaderGL"})
        cls(n);

    JClass* ver = jclass_def("android/os/Build$VERSION");
    JField* sdk = jfield_def(ver, "SDK_INT", "I", true);
    sdk->static_value.i = 34;  // what the phone the port is tested on reports
    jclass_def("java/lang/Runnable", "java/lang/Object", false);

    JClass* thr = jclass_def("java/lang/Throwable", "java/lang/Object", false);
    jmethod_def(thr, "getMessage", "()Ljava/lang/String;", [](JObj*, const jvalue*) { return jstr("(exception in the PC JVM)"); });
    jmethod_def(thr, "toString", "()Ljava/lang/String;", [](JObj*, const jvalue*) { return jstr("java.lang.Throwable"); });

    // the port's own Java helper (Android: java/boz64/Ui.java)
    JClass* ui = jclass_def("boz64/Ui", "java/lang/Object", false);
    jmethod_def(ui, "refreshRate", "()F", [](JObj*, const jvalue*) {
#ifdef _WIN32
        DEVMODEW dm{};
        dm.dmSize = sizeof dm;
        float hz = EnumDisplaySettingsW(nullptr, ENUM_CURRENT_SETTINGS, &dm) && dm.dmDisplayFrequency > 1
                       ? static_cast<float>(dm.dmDisplayFrequency) : 60.0f;
#else
        const SDL_DisplayMode* m =
            win_hwnd() ? SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(static_cast<SDL_Window*>(win_hwnd()))) : nullptr;
        float hz = m && m->refresh_rate > 1 ? m->refresh_rate : 60.0f;
#endif
        return jv_float(hz);
    }, true);
    g_ui_class = reinterpret_cast<jclass>(ui);
}

// ---- com.ideaworks3d.studio: the Java side of the game's own extensions ----
// The activity is com.activision.boz.Main, an IsDeviceActivity, which is a LoaderActivity. libIsDevice.so reads
// IsDeviceActivity.m_Activity and calls IsDevice methods; the engine uses IsIOSUtils' "keychain".
JClass *IDA, *MAIN;
JField *f_la_activity, *f_ida_activity;

// IsDevice: storage figures for the external storage, the display class, and the expansion file paths.
JObj* g_isdevice = nullptr;

struct DiskFigures {
    jint block_size = 4096, blocks = 0, free_blocks = 0;
};
DiskFigures disk_figures() {
    DiskFigures d;
#ifdef _WIN32
    ULARGE_INTEGER avail{}, total{}, free{};
    if (GetDiskFreeSpaceExW(widen(boz_host_path("/sdcard")).c_str(), &avail, &total, &free)) {
        auto blocks = [](ULONGLONG bytes) { return static_cast<jint>(std::min<ULONGLONG>(bytes / 4096, 0x7fffffff)); };
        d.blocks = blocks(total.QuadPart);
        d.free_blocks = blocks(avail.QuadPart);
    }
#else
    struct statvfs v{};
    if (statvfs(boz_host_path("/sdcard").c_str(), &v) == 0) {
        auto blocks = [](u64 bytes) { return static_cast<jint>(std::min<u64>(bytes / 4096, 0x7fffffff)); };
        d.blocks = blocks(static_cast<u64>(v.f_blocks) * v.f_frsize);
        d.free_blocks = blocks(static_cast<u64>(v.f_bavail) * v.f_frsize);
    }
#endif
    return d;
}

// IsIOSUtils' keychain: key -> CRC pairs the engine keeps for its save files, stored in files/.temp.bin as the
// Java code writes it (per entry: the key's UTF-16 big-endian chars, '#', the CRC as a big-endian int).
std::mutex kc_lock;
std::map<std::u16string, int32_t> kc_map;
bool kc_load_pending = true;  // initKobjMap

std::string kc_path() { return boz_host_path((g_files + "/.temp.bin").c_str()); }

bool kc_save() {
    FILE* f = host_fopen(kc_path(), L"wb");
    if (!f) return false;
    std::vector<unsigned char> out;
    for (const auto& [k, crc] : kc_map) {
        for (char16_t c : k) out.insert(out.end(), {static_cast<unsigned char>(c >> 8), static_cast<unsigned char>(c)});
        out.insert(out.end(), {0, '#'});
        auto u = static_cast<uint32_t>(crc);
        out.insert(out.end(), {static_cast<unsigned char>(u >> 24), static_cast<unsigned char>(u >> 16),
                               static_cast<unsigned char>(u >> 8), static_cast<unsigned char>(u)});
    }
    bool ok = out.empty() || fwrite(out.data(), 1, out.size(), f) == out.size();
    return fclose(f) == 0 && ok;
}

bool kc_init() {
    std::lock_guard lk(kc_lock);
    if (!kc_load_pending) return true;
    FILE* f = host_fopen(kc_path(), L"rb");
    if (!f) {  // first run: an empty file is made
        kc_load_pending = false;
        if (FILE* n = host_fopen(kc_path(), L"wb")) fclose(n);
        return true;
    }
    std::vector<unsigned char> buf(0xffff);
    size_t n = fread(buf.data(), 1, buf.size(), f);
    fclose(f);
    if (n == 0) return false;  // as the Java code: an empty file reads as "no keychain" (and is read again next time)
    size_t pos = 0;
    while (pos < n) {
        std::u16string key;
        while (pos + 2 <= buf.size()) {
            char16_t c = static_cast<char16_t>(buf[pos] << 8 | buf[pos + 1]);
            pos += 2;
            if (c == u'#') break;
            key += c;
        }
        if (pos + 4 > buf.size()) break;
        kc_map[key] = static_cast<int32_t>(static_cast<uint32_t>(buf[pos]) << 24 | buf[pos + 1] << 16 | buf[pos + 2] << 8 | buf[pos + 3]);
        pos += 4;
    }
    kc_load_pending = false;
    return true;
}

std::u16string jstr16(jobject s) {
    auto* js = static_cast<JString*>(O(s));
    return js ? js->s : std::u16string();
}

// Integer.parseInt
bool parse_int(const std::string& s, int32_t& out) {
    size_t i = 0;
    bool neg = false;
    if (i < s.size() && (s[i] == '-' || s[i] == '+')) neg = s[i++] == '-';
    if (i == s.size()) return false;
    int64_t v = 0;
    for (; i < s.size(); i++) {
        if (s[i] < '0' || s[i] > '9') return false;
        v = v * 10 + (s[i] - '0');
        if (v > 0x80000000ll) return false;
    }
    if (neg) v = -v;
    if (v > 0x7fffffffll || v < -0x80000000ll) return false;
    out = static_cast<int32_t>(v);
    return true;
}

void define_studio() {
    IDA = jclass_def("com/ideaworks3d/studio/IsDeviceActivity", "com/ideaworks3d/marmalade/LoaderActivity");
    MAIN = jclass_def("com/activision/boz/Main", "com/ideaworks3d/studio/IsDeviceActivity");
    f_la_activity = jfield_def(LA, "m_Activity", "Lcom/ideaworks3d/marmalade/LoaderActivity;", true);
    f_ida_activity = jfield_def(IDA, "m_Activity", "Lcom/ideaworks3d/studio/IsDeviceActivity;", true);

    JClass* isd = jclass_def("com/ideaworks3d/studio/IsDevice");
    JField* threshold = jfield_def(isd, "TabletThreshold", "I");
    JField* activated = jfield_def(isd, "m_IsActivated", "Z");
    jmethod_def(isd, "<init>", "()V", [threshold](JObj* o, const jvalue*) {
        jfield_set(o, threshold, jv_int(461));
        return jv_void();
    });
    jmethod_def(isd, "GetInstance", "()Lcom/ideaworks3d/studio/IsDevice;", [isd, threshold](JObj*, const jvalue*) {
        static std::mutex m;
        std::lock_guard lk(m);
        if (!g_isdevice) {
            g_isdevice = jnew(isd);
            jfield_set(g_isdevice, threshold, jv_int(461));
        }
        return jv_obj(jref(g_isdevice));
    }, true);
    jmethod_def(isd, "Activate", "()V", [activated](JObj* o, const jvalue*) {
        jfield_set(o, activated, jv_bool(true));
        return jv_void();
    });
    jmethod_def(isd, "IsActivated", "()Z", [activated](JObj* o, const jvalue*) {
        return jv_bool(o && o->fields.count(activated) && o->fields[activated].z);
    });
    // Helpers.getSaveFilePath: <external storage>/Android/obb/<package>, plus a separator
    jmethod_def(isd, "GetExpansionPath", "()Ljava/lang/String;", [](JObj*, const jvalue*) {
        return jstr("/sdcard/Android/obb/com.activision.boz/");
    });
    jmethod_def(isd, "IsDeviceGetAbsolutePath", "()Ljava/lang/String;", [](JObj*, const jvalue*) {
        return jstr("/sdcard/Android/data/");
    });
    jmethod_def(isd, "IsDeviceGetBlockSize", "()I", [](JObj*, const jvalue*) { return jv_int(disk_figures().block_size); });
    jmethod_def(isd, "IsDeviceGetBlockCount", "()I", [](JObj*, const jvalue*) { return jv_int(disk_figures().blocks); });
    jmethod_def(isd, "IsDeviceGetAvailableBlocks", "()I", [](JObj*, const jvalue*) { return jv_int(disk_figures().free_blocks); });
    jmethod_def(isd, "IsDeviceGetFreeBlocks", "()I", [](JObj*, const jvalue*) { return jv_int(disk_figures().free_blocks); });
    jmethod_def(isd, "IsDeviceGetFreeStorage", "()I", [](JObj*, const jvalue*) { return jv_int(-1); });
    // The Java code classes the screen by its physical size (smartphone 2, tablet 3, against TabletThreshold). A
    // PC monitor would class as a tablet; the game is kept on the smartphone layout it is tested with on the phone.
    jmethod_def(isd, "IsDeviceGetDisplayType", "()I", [](JObj*, const jvalue*) { return jv_int(2); });
    jmethod_def(isd, "IsDeviceSetTabletThreshold", "(I)I", [threshold](JObj* o, const jvalue* a) {
        jfield_set(o, threshold, jv_int(a[0].i));
        return jv_int(a[0].i);
    });
    jmethod_def(isd, "IsDeviceGetExternalResources", "(II)[Ljava/lang/String;", [](JObj*, const jvalue*) {
        JArray* r = jarray_new('L', 2);
        r->objs[0] = jstring_new("stub");
        r->objs[1] = jstring_new("stub");
        return jv_obj(r);
    });
    // no main.<version>.<package>.obb: the game's data is the blackops_*.dz pack
    jmethod_def(isd, "IsDeviceGetMainExpansionFileSize", "()I", [](JObj*, const jvalue*) { return jv_int(0); });
    jmethod_def(isd, "GetMainExpansionFileSize", "()J", [](JObj*, const jvalue*) { return jv_long(0); });
    jmethod_def(isd, "GetMainExpansionUri", "()Ljava/lang/String;", [](JObj*, const jvalue*) { return jstr(""); });
    jmethod_def(isd, "IsDeviceSetPublicKey", "(Ljava/lang/String;)V", [](JObj*, const jvalue*) { return jv_void(); });

    // IsIOSUtils (a top-level class in the app): the keychain, and handlers that do nothing on Android
    JClass* ios = jclass_def("IsIOSUtils");
    jmethod_def(ios, "<init>", "()V", [](JObj*, const jvalue*) { return jv_void(); });
    jmethod_def(ios, "IsKeychainInit", "(Ljava/lang/String;)Z", [](JObj*, const jvalue*) { return jv_bool(kc_init()); }, true);
    jmethod_def(ios, "IsKeychainTerminate", "()V", [](JObj*, const jvalue*) {
        std::lock_guard lk(kc_lock);
        kc_map.clear();
        return jv_void();
    }, true);
    jmethod_def(ios, "IsKeychainSearchKey", "(Ljava/lang/String;)Z", [](JObj*, const jvalue* a) {
        std::lock_guard lk(kc_lock);
        auto it = kc_map.find(jstr16(a[0].l));
        return jv_bool(it != kc_map.end() && it->second != -1);
    }, true);
    jmethod_def(ios, "IsKeychainReadKey", "(Ljava/lang/String;)I", [](JObj*, const jvalue* a) {
        std::lock_guard lk(kc_lock);
        auto it = kc_map.find(jstr16(a[0].l));
        return jv_int(it != kc_map.end() && it->second != -1 ? it->second : 0);
    }, true);
    jmethod_def(ios, "IsKeychainWriteKey", "(Ljava/lang/String;Ljava/lang/String;)Z", [](JObj*, const jvalue* a) {
        std::lock_guard lk(kc_lock);
        std::u16string key = jstr16(a[0].l);
        auto it = kc_map.try_emplace(key, -1).first;
        int32_t crc;
        if (!parse_int(jstring_utf8(a[1].l), crc)) return jv_bool(false);
        if (it->second == crc) return jv_bool(true);
        it->second = crc;
        return jv_bool(kc_save());
    }, true);
    for (const char* n : {"IsInstallExceptionHandler", "IsUninstallExceptionHandler", "IsUninstallMemoryWarningHandler",
                          "IsSimulateMemoryWarning", "IsOSFree"})
        jmethod_def(ios, n, "()V", [](JObj*, const jvalue*) { return jv_void(); }, true);
    jmethod_def(ios, "IsInstallMemoryWarningHandler", "(Ljava/lang/String;Ljava/lang/String;Z)V",
                [](JObj*, const jvalue*) { return jv_void(); }, true);
    for (const char* n : {"IsOSMalloc", "IsOSRealloc"})
        jmethod_def(ios, n, "(I)V", [](JObj*, const jvalue*) { return jv_void(); }, true);
}

}  // namespace

void marm_define_classes() {
    define_loader_thread();
    define_loader_view();
    define_others();
    define_studio();
}

void marm_start(const std::string& files_dir, const std::string& apk_path, const std::string& lib_dir) {
    g_files = files_dir;
    g_apk = apk_path;
    g_libs = lib_dir;
    for (const std::string& d : {g_files, g_cache, g_ext_files, g_ext_cache}) mkdirs(d);
    g_music_stopped = [](int ch) { jnative_call(LT, "audioStoppedNotify", "(I)V", g_lt, {jv_int(ch)}); };

    jlocal_push();  // the UI thread's own frame, for the whole run
    // LoaderActivity.onCreate: setART(true)
    g_activity = jnew(MAIN);
    {
        jvalue a = jv_obj(g_activity);
        f_la_activity->static_value = a;  // the field's own count
        jref(g_activity);
        f_ida_activity->static_value = a;
        jref(g_activity);
    }
    jnative_call(LA, "setART", "(Z)V", g_activity, {jv_bool(true)});
    // LoaderThread's constructor: SoundPlayer, MediaPlayerManager (16 channels), initNative()
    g_lt = jnew(LT);
    g_mpm = jnew(MPM);
    jfield_set(g_lt, f_mpm, jv_obj(g_mpm));
    g_sp = jnew(SP);
    jnative_call(LT, "initNative", "()V", g_lt, {});
    // setView(view)
    {
        std::lock_guard lk(view_lock);
        g_view = make_view();
    }
    set_view_native(current_view());
    // the first surfaceChanged (with the suspend around it, before runNative)
    surface_set(current_view(), win_width(), win_height(), true, true);
    // start the engine thread (a large host stack: guest code calls back into the host and back again)
#ifdef _WIN32
    g_eng = CreateThread(nullptr, 16 << 20, eng_main, nullptr, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
#else
    pthread_attr_t ta;
    pthread_attr_init(&ta);
    pthread_attr_setstacksize(&ta, 16 << 20);
    pthread_attr_setdetachstate(&ta, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    g_eng = pthread_create(&th, &ta, eng_main, nullptr) == 0;
    pthread_attr_destroy(&ta);
#endif
    if (!g_eng) fatal("could not start the engine thread");
}

void marm_touch(int id, int type, int x, int y) {
    if (!g_lt) return;
    jnative_call(LT, "onMotionEvent", "(IIII)V", g_lt, {jv_int(id), jv_int(type), jv_int(x), jv_int(y)});
}

bool marm_key(int code, int unicode, bool down) {
    JObj* v = current_view();
    if (!v) return false;
    JObj* k;
    {
        std::lock_guard lk(v->monitor);
        auto it = v->fields.find(f_kbd);
        k = it == v->fields.end() ? nullptr : O(it->second.l);
    }
    if (!k) return false;
    jvalue r = jnative_call(LK, "onKeyEventNative", "(III)Z", k, {jv_int(code), jv_int(unicode), jv_int(down ? 1 : 0)});
    return r.z != 0;
}

void marm_run_on_os_tick() {
    if (g_lt) jnative_call(LT, "runOnOSTickNative", "()V", g_lt, {});
}

void marm_surface_changed(int w, int h) {
    if (!g_lt || (w == g_w && h == g_h) || w <= 0 || h <= 0) return;
    LOGI("surface changed to %dx%d", w, h);
    surface_set(current_view(), w, h, false, g_init_term == 0);
}

void marm_suspend() {
    if (!g_started || g_paused.exchange(true)) return;
    LOGI("onPause: signalSuspend(true)");
    jnative_call(LT, "signalSuspend", "(Z)Z", g_lt, {jv_bool(true)});
}

void marm_resume() {
    if (!g_started || !g_paused.exchange(false)) return;
    LOGI("onResume: signalResume(false)");
    jnative_call(LT, "signalResume", "(Z)Z", g_lt, {jv_bool(false)});
}

bool marm_quitting() { return g_quitting.load(); }

void marm_shutdown() {
    LOGI("shutting down");
    audio_sfx_stop();
    music_stop_all();
    if (g_lt && g_started) jnative_call(LT, "shutdownNative", "()V", g_lt, {});
#ifdef _WIN32
    if (g_eng && WaitForSingleObject(g_eng, 3000) == WAIT_TIMEOUT) LOGW("the engine thread did not stop in 3 s");
#else
    for (int i = 0; g_eng && !g_eng_done && i < 300; i++) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (g_eng && !g_eng_done) LOGW("the engine thread did not stop in 3 s");
#endif
}

bool marm_generate_audio(int16_t* out, int frames, int channels) {
    if (!g_sfx_on || !g_sp) return false;
    std::lock_guard lk(sfx_lock);
    if (channels != g_sfx_channels) return false;
    jsize need = frames * channels;
    if (!g_sfx_buf || g_sfx_buf->n < need) {
        if (g_sfx_buf) junref(g_sfx_buf);
        g_sfx_buf = jarray_new('S', need * 2);  // Java's buffer is bigger than needed too
    }
    jnative_call(SP, "generateAudio", "([SI)V", g_sp, {jv_obj(g_sfx_buf), jv_int(frames)});
    memcpy(out, g_sfx_buf->data.data(), need * sizeof(int16_t));
    return true;
}
