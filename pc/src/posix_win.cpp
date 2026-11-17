// The POSIX and Android pieces the shared code uses (boz_win.h, android/log.h, sys/system_properties.h, dlfcn.h),
// on Windows.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <dlfcn.h>
#include <android/log.h>
#include <sys/system_properties.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

#include "boz_win.h"

namespace {
// Function-local statics throughout: shared code reads properties and logs from static initialisers, which may run
// before this file's own globals are constructed.
const std::string& exe_dir() {
    static std::string exe_dir_str;
    static std::once_flag exe_dir_once;
    std::call_once(exe_dir_once, [] {
        wchar_t w[MAX_PATH * 2] = {};
        DWORD n = GetModuleFileNameW(nullptr, w, MAX_PATH * 2);
        std::wstring p(w, n);
        size_t cut = p.find_last_of(L"\\/");
        if (cut != std::wstring::npos) p.resize(cut);
        int len = WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, nullptr, 0, nullptr, nullptr);
        std::string s(len > 0 ? len - 1 : 0, '\0');
        WideCharToMultiByte(CP_UTF8, 0, p.c_str(), -1, s.data(), len, nullptr, nullptr);
        for (char& c : s)
            if (c == '\\') c = '/';
        exe_dir_str = s;
    });
    return exe_dir_str;
}

// ---- clocks ----
LARGE_INTEGER qpc_freq() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER v;
        QueryPerformanceFrequency(&v);
        return v;
    }();
    return f;
}
std::int64_t mono_ns() {
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    std::int64_t f = qpc_freq().QuadPart;
    return static_cast<std::int64_t>(c.QuadPart / f) * 1000000000ll + (c.QuadPart % f) * 1000000000ll / f;
}
std::int64_t real_ns() {
    FILETIME ft;
    GetSystemTimePreciseAsFileTime(&ft);
    std::int64_t t = (static_cast<std::int64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    return (t - 116444736000000000ll) * 100;  // 100 ns ticks since 1601 -> ns since 1970
}

// Sleeps with a high-resolution waitable timer (plain Sleep() rounds to the 1-16 ms scheduler tick).
void sleep_ns(std::int64_t ns) {
    if (ns <= 0) return;
    thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) {
        Sleep(static_cast<DWORD>((ns + 999999) / 1000000));
        return;
    }
    LARGE_INTEGER due;
    due.QuadPart = -(ns / 100);  // relative, 100 ns units
    if (due.QuadPart == 0) due.QuadPart = -1;
    SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForSingleObject(timer, INFINITE);
}

// ---- log ----
std::mutex log_lock;
FILE* log_file = nullptr;
bool have_console = false;

// ---- properties ----
std::mutex prop_lock;
std::unordered_map<std::string, std::string>& props_map() {
    static std::unordered_map<std::string, std::string> m;
    return m;
}
FILETIME props_time{};
std::int64_t props_checked_ns = 0;

void load_props() {
    auto& props = props_map();
    // reread boz64.props when it changes (checked at most twice a second)
    std::int64_t now = mono_ns();
    if (props_checked_ns && now - props_checked_ns < 500000000ll) return;
    props_checked_ns = now;
    std::string path = exe_dir() + "/boz64.props";
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &a)) {
        props.clear();
        return;
    }
    if (CompareFileTime(&a.ftLastWriteTime, &props_time) == 0) return;
    props_time = a.ftLastWriteTime;
    props.clear();
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        auto trim = [](std::string s) {
            size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
            return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
        };
        props[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
}

// ---- dlopen ----
thread_local std::string dl_err;
}  // namespace

const char* boz_exe_dir() { return exe_dir().c_str(); }

int clock_gettime(clockid_t clk, timespec* ts) {
    std::int64_t ns;
    switch (clk) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE: ns = real_ns(); break;
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID: {
        FILETIME c, e, k, u;
        bool ok = clk == CLOCK_THREAD_CPUTIME_ID ? GetThreadTimes(GetCurrentThread(), &c, &e, &k, &u)
                                                 : GetProcessTimes(GetCurrentProcess(), &c, &e, &k, &u);
        if (!ok) return -1;
        auto v = [](FILETIME f) { return (static_cast<std::int64_t>(f.dwHighDateTime) << 32) | f.dwLowDateTime; };
        ns = (v(k) + v(u)) * 100;
        break;
    }
    default: ns = mono_ns(); break;
    }
    ts->tv_sec = static_cast<time_t>(ns / 1000000000ll);
    ts->tv_nsec = static_cast<long>(ns % 1000000000ll);
    return 0;
}

int clock_getres(clockid_t, timespec* ts) {
    ts->tv_sec = 0;
    ts->tv_nsec = 100;
    return 0;
}

int clock_nanosleep(clockid_t clk, int flags, const timespec* req, timespec*) {
    std::int64_t want = static_cast<std::int64_t>(req->tv_sec) * 1000000000ll + req->tv_nsec;
    if (flags & TIMER_ABSTIME) want -= (clk == CLOCK_REALTIME ? real_ns() : mono_ns());
    sleep_ns(want);
    return 0;
}

int nanosleep(const timespec* req, timespec* rem) { return clock_nanosleep(CLOCK_MONOTONIC, 0, req, rem); }
int usleep(unsigned int us) {
    sleep_ns(static_cast<std::int64_t>(us) * 1000);
    return 0;
}
unsigned int sleep(unsigned int s) {
    sleep_ns(static_cast<std::int64_t>(s) * 1000000000ll);
    return 0;
}
int sched_yield() {
    SwitchToThread();
    return 0;
}
int gettid() { return static_cast<int>(GetCurrentThreadId()); }
FILE* fmemopen(void* buf, size_t size, const char*) {
    FILE* f = tmpfile();
    if (!f) return nullptr;
    fwrite(buf, 1, size, f);
    rewind(f);
    return f;
}
long sysconf(int name) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    if (name == _SC_NPROCESSORS_CONF || name == _SC_NPROCESSORS_ONLN) return static_cast<long>(si.dwNumberOfProcessors);
    if (name == _SC_PAGESIZE) return 4096;
    return -1;
}

extern "C" int __android_log_write(int prio, const char* tag, const char* text) {
    static const char lv[] = "??VDIWEFS";
    SYSTEMTIME st;
    GetLocalTime(&st);
    char head[64];
    snprintf(head, sizeof head, "%02d:%02d:%02d.%03d %c/%s(%5lu): ", st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
             prio >= 0 && prio < 9 ? lv[prio] : '?', "", GetCurrentThreadId());
    std::lock_guard lk(log_lock);
    if (!log_file) {
        // debug_boz64_log (environment): another log file, for a second copy of the game run from the same folder
        const char* lp = getenv("debug_boz64_log");
        std::string p = lp && *lp ? std::string(lp) : exe_dir() + "/boz64.log";
        log_file = fopen(p.c_str(), "w");
        have_console = GetConsoleWindow() != nullptr;
    }
    std::string line = std::string(head) + (tag ? tag : "") + ": " + (text ? text : "") + "\n";
    if (log_file) {
        fputs(line.c_str(), log_file);
        if (prio >= ANDROID_LOG_INFO) fflush(log_file);  // trace (debug) lines stay buffered: there are many
    }
    if (have_console) fputs(line.c_str(), stderr);
    if (prio >= ANDROID_LOG_FATAL && log_file) fflush(log_file);
    return 1;
}

extern "C" int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap) {
    char buf[4096];
    vsnprintf(buf, sizeof buf, fmt, ap);
    return __android_log_write(prio, tag, buf);
}

extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int r = __android_log_vprint(prio, tag, fmt, ap);
    va_end(ap);
    return r;
}

void boz_log_flush() {
    std::lock_guard lk(log_lock);
    if (log_file) fflush(log_file);
}

extern "C" int __system_property_get(const char* name, char* value) {
    value[0] = 0;
    std::string v;
    {
        std::lock_guard lk(prop_lock);
        load_props();
        auto it = props_map().find(name);
        if (it != props_map().end()) v = it->second;
    }
    if (v.empty()) {
        std::string env = name;
        for (char& c : env)
            if (c == '.') c = '_';
        if (const char* e = getenv(env.c_str())) v = e;
    }
    if (v.size() >= PROP_VALUE_MAX) v.resize(PROP_VALUE_MAX - 1);
    memcpy(value, v.c_str(), v.size() + 1);
    return static_cast<int>(v.size());
}

void* dlopen(const char* name, int) {
    if (!name) return GetModuleHandleW(nullptr);
    std::string n = name;
    size_t slash = n.find_last_of('/');
    if (slash != std::string::npos) n = n.substr(slash + 1);
    // Android library names -> the DLLs shipped next to the .exe (ANGLE has GLES 1-3 in libGLESv2.dll)
    if (n == "libGLESv2.so" || n == "libGLESv3.so" || n == "libGLESv1_CM.so") n = "libGLESv2.dll";
    else if (n == "libEGL.so") n = "libEGL.dll";
    else if (n.size() > 3 && n.compare(n.size() - 3, 3, ".so") == 0) n = n.substr(0, n.size() - 3) + ".dll";
    std::string path = exe_dir() + "/" + n;
    HMODULE h = LoadLibraryExA(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!h) h = LoadLibraryA(n.c_str());
    if (!h) dl_err = "cannot load " + n;
    return h;
}

void* dlsym(void* handle, const char* sym) {
    void* p = reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), sym));
    if (!p) dl_err = std::string("undefined symbol: ") + sym;
    return p;
}

int dlclose(void*) { return 0; }

const char* dlerror() {
    if (dl_err.empty()) return nullptr;
    static thread_local std::string last;
    last = dl_err;
    dl_err.clear();
    return last.c_str();
}

int dladdr(const void*, Dl_info* info) {
    static std::string exe = exe_dir() + "/boz64.exe";
    info->dli_fname = exe.c_str();
    info->dli_fbase = GetModuleHandleW(nullptr);
    info->dli_sname = nullptr;
    info->dli_saddr = nullptr;
    return 1;
}
