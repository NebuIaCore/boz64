// The Unix PC builds (Linux, the BSDs, macOS, iOS): what pc/src/posix_win.cpp does on Windows, minus what POSIX
// already has (clocks, sleeps, dlopen):
//   - the folder BOZ64 keeps its files in (boz_exe_dir, see boz_unix.h);
//   - the log: boz64.log in that folder (or debug_boz64_log), and stderr;
//   - the debug switches: boz64.props in that folder ("name=value" lines), else environment variables named like the
//     property with dots as underscores (debug_boz64_trace=1);
//   - the Android build's bundled-file lookups (src/vfs.h), which have nothing to serve on a PC.
#include <SDL3/SDL.h>
#include <android/log.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <sys/time.h>
#include <unistd.h>

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

#include "vfs.h"

namespace {

std::string program_dir() {
    const char* b = SDL_GetBasePath();  // SDL keeps it; ends with '/'
    std::string s = b ? b : "./";
    while (s.size() > 1 && s.back() == '/') s.pop_back();
    return s;
}

bool is_dir(const std::string& p) {
    struct stat st{};
    return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

const std::string& home() {
    static std::string h;  // function-local: may be needed during static initialisation
    static std::once_flag once;
    std::call_once(once, [] {
        if (const char* e = getenv("BOZ64_HOME"); e && *e) {
            h = e;
        } else if (std::string prog = program_dir(); is_dir(prog + "/data")) {
            h = prog;  // portable: an unpacked .tar.gz with the game files next to the program
        } else {
#ifdef SDL_PLATFORM_IOS
            const char* d = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);  // shown in the Files app (UIFileSharingEnabled)
            h = d ? d : prog;
#else
            char* p = SDL_GetPrefPath("", "BOZ64");
            h = p ? p : prog;
            SDL_free(p);
#endif
        }
        while (h.size() > 1 && h.back() == '/') h.pop_back();
        mkdir(h.c_str(), 0755);
    });
    return h;
}

std::mutex log_lock;
FILE* log_file = nullptr;

std::mutex prop_lock;
std::unordered_map<std::string, std::string> props;
timespec props_time{};
std::chrono::steady_clock::time_point props_checked{};

void load_props() {
    auto now = std::chrono::steady_clock::now();
    if (props_checked.time_since_epoch().count() && now - props_checked < std::chrono::milliseconds(500)) return;
    props_checked = now;
    std::string path = home() + "/boz64.props";
    struct stat st{};
    if (stat(path.c_str(), &st) != 0) {
        props.clear();
        props_time = {};
        return;
    }
#ifdef __APPLE__
    timespec t = st.st_mtimespec;
#else
    timespec t = st.st_mtim;
#endif
    if (t.tv_sec == props_time.tv_sec && t.tv_nsec == props_time.tv_nsec) return;
    props_time = t;
    props.clear();
    std::ifstream f(path);
    std::string line;
    auto trim = [](const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r"), b = s.find_last_not_of(" \t\r");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    while (std::getline(f, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t eq = line.find('=');
        if (eq != std::string::npos) props[trim(line.substr(0, eq))] = trim(line.substr(eq + 1));
    }
}

}  // namespace

const char* boz_exe_dir() { return home().c_str(); }

#ifndef __linux__
// Linux's gettid for the shared code (src/libc.cpp hands it to the guest): any number unique to the thread does.
int gettid() { return static_cast<int>(SDL_GetCurrentThreadID()); }
#endif

extern "C" int __android_log_write(int prio, const char* tag, const char* text) {
    static const char lv[] = "??VDIWEFS";
    timeval tv{};
    gettimeofday(&tv, nullptr);
    time_t sec = tv.tv_sec;
    tm t{};
    localtime_r(&sec, &t);
    char head[64];
    snprintf(head, sizeof head, "%02d:%02d:%02d.%03d %c/(%5lu): ", t.tm_hour, t.tm_min, t.tm_sec,
             static_cast<int>(tv.tv_usec / 1000), prio >= 0 && prio < 9 ? lv[prio] : '?',
             static_cast<unsigned long>(SDL_GetCurrentThreadID() % 100000));
    std::string line = std::string(head) + (tag ? tag : "") + ": " + (text ? text : "") + "\n";
    std::lock_guard lk(log_lock);
    if (!log_file) {
        // debug_boz64_log (environment): another log file, for a second copy of the game run from the same folder
        const char* lp = getenv("debug_boz64_log");
        log_file = fopen(lp && *lp ? lp : (home() + "/boz64.log").c_str(), "w");
    }
    if (log_file) {
        fputs(line.c_str(), log_file);
        if (prio >= ANDROID_LOG_INFO) fflush(log_file);  // trace (debug) lines stay buffered: there are many
    }
    if (prio >= ANDROID_LOG_INFO) fputs(line.c_str(), stderr);
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
    std::string v;
    {
        std::lock_guard lk(prop_lock);
        load_props();
        auto it = props.find(name);
        if (it != props.end()) v = it->second;
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

// src/vfs.h: the Android build serves a data pack bundled inside its APK; the PC builds read real files only.
// (vfs_lookup and vfs_fopen are in vfs_win.cpp, which both PC builds share.)
void vfs_init(const std::string&) {}
void vfs_stat(const Bundled*, struct stat*) {}
int vfs_open(const Bundled*) { return -1; }
bool vfs_is_fd(int) { return false; }
void vfs_close(int) {}
s64 vfs_lseek(int, s64, int) { return -2; }
s64 vfs_read_limit(int, u64) { return -2; }
s64 vfs_pread(int, void*, u64, s64) { return -2; }
bool vfs_fix_fstat(int, struct stat*) { return false; }
