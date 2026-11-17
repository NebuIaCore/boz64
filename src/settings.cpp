#include "settings.h"

#include <sys/stat.h>
#include <sys/system_properties.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

namespace {
std::mutex lock;
Settings saved;    // as in the file
Settings current;  // with property overrides
std::string path;
timespec file_mtime{};
std::atomic<u64> next_poll{0};

int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

Settings sanitized(Settings s) {
    s.fps_limit = clamp(s.fps_limit, 0, 500);
    s.res_scale = clamp(s.res_scale, 25, 100);
    if (!(s.mouse_sensitivity > 0)) s.mouse_sensitivity = Settings{}.mouse_sensitivity;  // missing or not a number
    s.mouse_sensitivity = s.mouse_sensitivity < 0.1f ? 0.1f : s.mouse_sensitivity > 100 ? 100 : s.mouse_sensitivity;
    s.mouse_ads = clamp(s.mouse_ads, 5, 200);
    return s;
}

void apply(Settings& s, const char* name, const char* value) {
    int v = atoi(value);
    if (!strcmp(name, "fps_limit")) s.fps_limit = v;
    else if (!strcmp(name, "res_scale")) s.res_scale = v;
    else if (!strcmp(name, "zombies_ignore")) s.zombies_ignore = v != 0;
    else if (!strcmp(name, "mouse_sensitivity")) s.mouse_sensitivity = static_cast<float>(atof(value));
    else if (!strcmp(name, "mouse_ads")) s.mouse_ads = v;
}

void load_file() {
    Settings s;
    if (FILE* f = fopen(path.c_str(), "r")) {
        char line[128];
        while (fgets(line, sizeof line, f)) {
            char* eq = strchr(line, '=');
            if (!eq) continue;
            *eq = 0;
            apply(s, line, eq + 1);
        }
        fclose(f);
    }
    saved = sanitized(s);
}

Settings with_overrides(Settings s) {
    static const char* names[] = {"fps_limit", "res_scale", "zombies_ignore", "mouse_sensitivity", "mouse_ads"};
    for (const char* n : names) {
        char prop[64], v[PROP_VALUE_MAX] = {};
        snprintf(prop, sizeof prop, "debug.boz64.%s", n);
        if (__system_property_get(prop, v) > 0 && v[0]) apply(s, n, v);
    }
    return sanitized(s);
}

timespec mtime_of(const struct stat& st) {
#ifdef _WIN32
    return timespec{st.st_mtime, 0};
#else
    return st.st_mtim;
#endif
}

u64 now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000 + static_cast<u64>(ts.tv_nsec) / 1000000;
}
}  // namespace

void settings_init(const std::string& dir) {
    std::lock_guard<std::mutex> g(lock);
    path = dir + "/boz64.cfg";
    struct stat st{};
    if (stat(path.c_str(), &st) == 0) file_mtime = mtime_of(st);
    load_file();
    current = with_overrides(saved);
    LOGI("settings (%s): fps_limit %d, res_scale %d%%, zombies_ignore %d, mouse_sensitivity %g, mouse_ads %d%%",
         path.c_str(), current.fps_limit, current.res_scale, current.zombies_ignore, current.mouse_sensitivity,
         current.mouse_ads);
}

Settings settings() {
    std::lock_guard<std::mutex> g(lock);
    return current;
}

void settings_poll() {
    u64 t = now_ms();
    if (t < next_poll.load(std::memory_order_relaxed)) return;
    next_poll = t + 1000;
    std::lock_guard<std::mutex> g(lock);
    if (path.empty()) return;
    struct stat st{};
    if (stat(path.c_str(), &st) == 0 &&
        (mtime_of(st).tv_sec != file_mtime.tv_sec || mtime_of(st).tv_nsec != file_mtime.tv_nsec)) {
        file_mtime = mtime_of(st);
        load_file();
    }
    Settings s = with_overrides(saved);
    if (s.fps_limit != current.fps_limit || s.res_scale != current.res_scale || s.zombies_ignore != current.zombies_ignore ||
        s.mouse_sensitivity != current.mouse_sensitivity || s.mouse_ads != current.mouse_ads)
        LOGI("settings now: fps_limit %d, res_scale %d%%, zombies_ignore %d, mouse_sensitivity %g, mouse_ads %d%%",
             s.fps_limit, s.res_scale, s.zombies_ignore, s.mouse_sensitivity, s.mouse_ads);
    current = s;
}

void settings_save(const Settings& s) { settings_set(s, true); }

void settings_set(const Settings& s, bool write_file) {
    std::lock_guard<std::mutex> g(lock);
    saved = sanitized(s);
    current = with_overrides(saved);
    if (path.empty() || !write_file) return;
    std::string tmp = path + ".tmp";
    if (FILE* f = fopen(tmp.c_str(), "w")) {
        fprintf(f, "fps_limit=%d\nres_scale=%d\nzombies_ignore=%d\nmouse_sensitivity=%g\nmouse_ads=%d\n", saved.fps_limit,
                saved.res_scale, saved.zombies_ignore, saved.mouse_sensitivity, saved.mouse_ads);
        fclose(f);
#ifdef _WIN32
        remove(path.c_str());  // Windows' rename does not replace an existing file
#endif
        rename(tmp.c_str(), path.c_str());
        struct stat st{};
        if (stat(path.c_str(), &st) == 0) file_mtime = mtime_of(st);
    }
}
