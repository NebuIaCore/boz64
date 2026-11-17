#include "frame.h"
#include "lan.h"
#include "patches.h"
#include "flame.h"
#include "resscale.h"
#include "settings.h"
#include "smooth.h"

#include <EGL/egl.h>
#include <jni.h>

#include <sys/system_properties.h>
#include <time.h>

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <map>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <unordered_map>
#include <vector>
#include <string>

thread_local u32 tl_thunk_calls = 0;

extern JavaVM* g_vm;
extern jclass g_ui_class;
void thunk_stats_init();
void thunk_stats(u32 frames);
void prof_start_this_thread();
void prof_stats();

namespace {
bool stats = false;
inline u64 now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}
// statistics (render thread only)
thread_local bool tl_render = false;
thread_local u64 tl_sleep_start = 0;
u64 win_start = 0, swap_start = 0, frames = 0, swap_ns = 0, sleep_ns = 0, max_frame = 0, last_swap = 0;
u64 calls = 0;
u64 frame_hash = 0, last_hash = 0, repeats = 0;
std::map<u32, u32> sleep_hist;
std::atomic<u32> touch_frame{0};
std::atomic<int> touch_x{-1};
std::atomic<u64> touch_block_max{0}, touch_block_sum{0}, touch_n{0};
u32 touch_hist[4] = {};
// camera probe: the most repeated inPMVMat of a frame is the world geometry's (projection x view); the camera
// sits where that matrix maps to clip x = y = w = 0. Two layouts are tried (rows or columns uploaded).
std::unordered_map<u64, std::string> uni_names;  // program << 32 | location
u32 cur_program = 0;
struct Cand { u64 key; u32 n; float m[16]; };
Cand cands[48];
u32 n_cands = 0;
// one-frame dump (debug.boz64.dumpframe=1): every inPMVMat and draw-related call, in order
bool dump_this = false, dump_next = false;
struct Ev { char kind; u32 value; u32 program; float m[16]; };
std::vector<Ev> evs;
u32 caller_lr = 0, caller_ptr = 0;
int skip_prog = 0;  // debug.boz64.skip: a program's draws, or -1 for draws after the depth-only clear
bool after_depth_clear = false;
struct Vec { double x = 0, y = 0, z = 0; bool ok = false; };
Vec cam_prev[2];
double cam_path[2] = {};
std::string cam_trace[2];

Vec solve(const float* m, bool rows) {
    // the three equations: rows 0, 1 and 3 of the 4x4 matrix, each a*x + b*y + c*z + d = 0
    double e[3][4];
    const int pick[3] = {0, 1, 3};
    for (int k = 0; k < 3; k++)
        for (int j = 0; j < 4; j++) e[k][j] = rows ? m[pick[k] * 4 + j] : m[j * 4 + pick[k]];
    auto det = [](double a, double b, double c, double d, double f, double g, double h, double i, double j) {
        return a * (f * j - g * i) - b * (d * j - g * h) + c * (d * i - f * h);
    };
    double D = det(e[0][0], e[0][1], e[0][2], e[1][0], e[1][1], e[1][2], e[2][0], e[2][1], e[2][2]);
    Vec v;
    if (std::fabs(D) < 1e-12) return v;
    double r0 = -e[0][3], r1 = -e[1][3], r2 = -e[2][3];
    v.x = det(r0, e[0][1], e[0][2], r1, e[1][1], e[1][2], r2, e[2][1], e[2][2]) / D;
    v.y = det(e[0][0], r0, e[0][2], e[1][0], r1, e[1][2], e[2][0], r2, e[2][2]) / D;
    v.z = det(e[0][0], e[0][1], r0, e[1][0], e[1][1], r1, e[2][0], e[2][1], r2) / D;
    v.ok = true;
    return v;
}

// debugging (debug.boz64.findcam=1): where in guest memory the camera position is stored
int find_cam = 0;
// debugging (debug.boz64.findcam=1 snapshot, =2 compare): float triplets that moved by what the camera moved
constexpr u32 SCAN_LO = 0x02400000, SCAN_HI = 0x0a000000;
std::vector<float> snap;
Vec snap_cam;
void find_camera(const Vec& c, int mode) {
    const float* f = reinterpret_cast<const float*>(g::mem + SCAN_LO);
    size_t n = (SCAN_HI - SCAN_LO) / 4;
    if (mode == 1) {
        snap.assign(f, f + n);
        snap_cam = c;
        LOGI("findcam: snapshot at camera %.2f %.2f %.2f", c.x, c.y, c.z);
        return;
    }
    if (snap.size() != n) return;
    double dx = c.x - snap_cam.x, dy = c.y - snap_cam.y, dz = c.z - snap_cam.z;
    double len = std::sqrt(dx * dx + dy * dy + dz * dz);
    LOGI("findcam: camera moved %.2f %.2f %.2f (%.1f)", dx, dy, dz, len);
    if (len < 20) return;
    const double ks[] = {1, -1, 0.01, 0.1, 2, 4, 8, 16, 32, 0.5, 0.25, 0.125, 1 / 16.0, 1 / 32.0, 1 / 64.0, 1 / 100.0};
    int hits = 0;
    for (size_t i = 0; i + 3 < n && hits < 60; i++) {
        double ex = f[i] - snap[i], ey = f[i + 1] - snap[i + 1], ez = f[i + 2] - snap[i + 2];
        if (ex == 0 && ey == 0 && ez == 0) continue;
        for (double k : ks) {
            double t = 0.03 * len * std::fabs(k) + 1e-4;
            // same axes, or y and z swapped
            bool same = std::fabs(ex - k * dx) < t && std::fabs(ey - k * dy) < t && std::fabs(ez - k * dz) < t;
            bool swap = std::fabs(ex - k * dx) < t && std::fabs(ey - k * dz) < t && std::fabs(ez - k * dy) < t;
            if (!same && !swap) continue;
            u32 a = SCAN_LO + static_cast<u32>(i * 4);
            const float* m = f + i;
            LOGI("findcam: %08x k=%g%s now %.3f %.3f %.3f was %.3f %.3f %.3f | before: %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f",
                 a, k, swap ? " (y/z swapped)" : "", m[0], m[1], m[2], snap[i], snap[i + 1], snap[i + 2], m[-9], m[-8], m[-7], m[-6],
                 m[-5], m[-4], m[-3], m[-2], m[-1]);
            hits++;
            break;
        }
    }
    LOGI("findcam: %d places", hits);
}

void camera_frame(bool touched) {
    const Cand* best = nullptr;
    for (u32 i = 0; i < n_cands; i++)
        if (!best || cands[i].n > best->n) best = &cands[i];
    for (int l = 0; l < 2; l++) {
        Vec v = best && best->n >= 3 ? solve(best->m, l == 0) : Vec{};
        if (l == 1 && find_cam && v.ok) {
            find_camera(v, find_cam);
            find_cam = 0;
            const float* m = best->m;
            LOGI("findcam: world matrix %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.2f %.2f %.2f %.2f",
                 m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
        }
        std::string& t = cam_trace[l];
        if (v.ok && cam_prev[l].ok) {
            double d = std::sqrt((v.x - cam_prev[l].x) * (v.x - cam_prev[l].x) + (v.y - cam_prev[l].y) * (v.y - cam_prev[l].y) +
                                 (v.z - cam_prev[l].z) * (v.z - cam_prev[l].z));
            cam_path[l] += d;
            if (t.size() < 900) t += std::to_string(static_cast<int>(d * 1000)) + (touched ? "t " : " ");
        } else if (t.size() < 900) {
            t += "? ";
        }
        cam_prev[l] = v;
    }
    n_cands = 0;
}
// Frame rate limit (settings fps_limit). Uncapped: no vsync wait at all. A limit at or below the display's
// refresh rate keeps vsync on (the display rate itself needs nothing more); other limits sleep until each
// frame is due. Game speed never depends on this: the game times itself by the real clock.
float refresh_hz = 0;
u64 next_refresh_query = 0;
int cur_interval = -1;
u64 next_frame = 0;

float display_refresh(u64 t) {
    if (t >= next_refresh_query && g_vm && g_ui_class) {
        next_refresh_query = t + 3000000000ull;
        JNIEnv* e = nullptr;
        if (g_vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) == JNI_OK && e) {
            static jmethodID m = e->GetStaticMethodID(g_ui_class, "refreshRate", "()F");
            float r = m ? e->CallStaticFloatMethod(g_ui_class, m) : 0;
            if (e->ExceptionCheck()) e->ExceptionClear();
            else if (r > 1) refresh_hz = r;
        }
    }
    return refresh_hz > 1 ? refresh_hz : 60;
}

void pace_frame() {
    settings_poll();
    u64 t = now_ns();
    int limit = settings().fps_limit;
    float hz = display_refresh(t);
    int want = limit > 0 && limit <= hz + 0.5f ? 1 : 0;
    if (want != cur_interval) {
        EGLDisplay d = eglGetCurrentDisplay();
        bool ok = d != EGL_NO_DISPLAY && eglSwapInterval(d, want);
        LOGI("frame limit %d fps (display %.0f Hz): vsync %s%s", limit, hz, want ? "on" : "off", ok ? "" : " (eglSwapInterval failed)");
        cur_interval = want;
    }
    if (limit <= 0 || (want && limit >= hz - 0.5f)) {
        next_frame = 0;
        return;
    }
    u64 period = 1000000000ull / static_cast<u64>(limit);
    if (!next_frame || t > next_frame + 2 * period) next_frame = t;  // the first frame, or far behind: start over
    if (next_frame > t) {
        timespec ts{static_cast<time_t>(next_frame / 1000000000ull), static_cast<long>(next_frame % 1000000000ull)};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, nullptr) == EINTR) {}
    }
    next_frame += period;
}
}  // namespace

void frame_init() {
    char v[PROP_VALUE_MAX] = {};
    stats = __system_property_get("debug.boz64.stats", v) > 0 && v[0] == '1';
    if (stats) thunk_stats_init();
}

void frame_swap_begin() {
    lan_frame();
    res_frame_end();
    flame_draw();  // over the finished frame: the flamethrower's heat gauge
    if (stats) {
        static bool prof = false;
        if (!prof) prof = true, prof_start_this_thread();
    }
    pace_frame();
    tl_render = true;
    swap_start = now_ns();
}

void frame_swap_end() {
    u64 t = now_ns();
    smooth_frame_end();
    res_frame_start();
    if (!stats) return;
    swap_ns += t - swap_start;
    if (last_swap) max_frame = std::max(max_frame, t - last_swap);
    last_swap = t;
    if (frame_hash == last_hash) repeats++;
    last_hash = frame_hash;
    frame_hash = 0;
    if (dump_this) {
        const Cand* best = nullptr;
        for (u32 i = 0; i < n_cands; i++)
            if (!best || cands[i].n > best->n) best = &cands[i];
        std::string line;
        u32 draws = 0;
        auto flush = [&] { if (draws) line += "d" + std::to_string(draws) + " "; draws = 0; };
        for (const Ev& e : evs) {
            if (e.kind == 'd' || e.kind == 'a') { draws++; continue; }
            flush();
            if (e.kind == 'M') {
                if (best && memcmp(e.m, best->m, 64) == 0) { line += "W "; continue; }
                Vec v = solve(e.m, false);
                if (!v.ok) { line += "O "; continue; }
                char b[64];
                snprintf(b, sizeof b, "p%u(%.0f,%.0f,%.0f) ", e.program, v.x, v.y, v.z);
                line += b;
            } else {
                line += std::string(1, e.kind) + std::to_string(e.value) + " ";
            }
            if (line.size() > 3000) { LOGI("dump: %s", line.c_str()); line.clear(); }
        }
        flush();
        LOGI("dump: %s", line.c_str());
        if (best) {
            Vec c = solve(best->m, false);
            LOGI("dump: world matrix used %u times, camera %.1f %.1f %.1f; %zu events", best->n, c.x, c.y, c.z, evs.size());
        }
        evs.clear();
        dump_this = false;
    }
    if (dump_next) { dump_this = true; dump_next = false; }
    after_depth_clear = false;
    camera_frame(touch_frame.load() != 0);
    { u32 n = touch_frame.exchange(0); touch_hist[std::min<u32>(n, 3)]++; }
    calls += tl_thunk_calls;
    tl_thunk_calls = 0;
    frames++;
    if (!win_start) win_start = t;
    if (t - win_start >= 2000000000ull) {
        double secs = (t - win_start) / 1e9;
        std::string h;
        for (auto& [us, n] : sleep_hist) h += " " + std::to_string(us) + "us*" + std::to_string(n);
        LOGI("stats: %.1f fps, %llu%% same as previous, frame %.2f ms (worst %.1f), swap %.2f ms, guest sleep %.2f ms, %llu calls/frame, sleeps:%s; touches/frame 0:%u 1:%u 2:%u 3+:%u, touch block avg %.3f max %.2f ms",
             frames / secs, static_cast<unsigned long long>(repeats * 100 / frames), secs * 1000 / frames, max_frame / 1e6, swap_ns / 1e6 / frames, sleep_ns / 1e6 / frames,
             static_cast<unsigned long long>(calls / frames), h.substr(0, 300).c_str(), touch_hist[0], touch_hist[1], touch_hist[2], touch_hist[3],
             touch_n ? touch_block_sum / 1e6 / touch_n : 0.0, touch_block_max / 1e6);
        touch_hist[0] = touch_hist[1] = touch_hist[2] = touch_hist[3] = 0;
        touch_block_max = touch_block_sum = touch_n = 0;
        patches_stats();
        thunk_stats(static_cast<u32>(frames));
        prof_stats();
        {
            char line[400];
            smooth_stats(line, sizeof line);
            LOGI("stats: %s", line);
        }
        {
            char v[PROP_VALUE_MAX] = {};
            dump_next = __system_property_get("debug.boz64.dumpframe", v) > 0 && v[0] == '1';
            find_cam = __system_property_get("debug.boz64.findcam", v) > 0 ? atoi(v) : 0;
            skip_prog = __system_property_get("debug.boz64.skip", v) > 0 ? (v[0] == 'z' ? -1 : atoi(v)) : 0;
        }
        for (int l = 0; l < 2; l++) {
            const Vec& c = cam_prev[l];
            LOGI("stats: camera (%s) at %.2f %.2f %.2f, moved %.2f/s, per frame (x1000, t = touch): %s", l ? "columns" : "rows",
                 c.x, c.y, c.z, cam_path[l] / secs, cam_trace[l].c_str());
            cam_path[l] = 0;
            cam_trace[l].clear();
        }
        win_start = t;
        frames = swap_ns = sleep_ns = max_frame = calls = repeats = 0;
        sleep_hist.clear();
    }
}

int frame_swap_interval(int requested) {
    if (stats) LOGI("game asked for eglSwapInterval(%d)", requested);
    return requested;
}

void frame_sleep_begin() {
    if (stats && tl_render) tl_sleep_start = now_ns();
}

void frame_sleep_end(u32 requested_us) {
    if (!stats || !tl_render || !tl_sleep_start) return;
    sleep_ns += now_ns() - tl_sleep_start;
    tl_sleep_start = 0;
    sleep_hist[requested_us]++;
}

void frame_note_touch(u64 blocked_ns, int x) {
    if (!stats) return;
    if (x >= 0) touch_x = x;
    touch_frame++;
    touch_n++;
    touch_block_sum += blocked_ns;
    u64 m = touch_block_max.load();
    while (blocked_ns > m && !touch_block_max.compare_exchange_weak(m, blocked_ns)) {}
}

void frame_note_data(const void* p, u32 bytes) {
    if (!stats || !p) return;
    const u8* b = static_cast<const u8*>(p);
    u64 h = frame_hash ^ 0x9e3779b97f4a7c15ull;
    for (u32 i = 0; i < bytes; i++) h = (h ^ b[i]) * 0x100000001b3ull;
    frame_hash = h;
}

void frame_note_uniform_name(u32 program, const char* name, int location) {
    if (!stats || !name || location < 0) return;
    uni_names[(static_cast<u64>(program) << 32) | static_cast<u32>(location)] = name;
}

void frame_note_program(u32 program) { cur_program = program; }

void frame_note_matrix(int location, const float* m, u32 bytes) {
    if (!stats || !m) return;
    frame_note_data(m, bytes);
    if (bytes < 64) return;
    auto it = uni_names.find((static_cast<u64>(cur_program) << 32) | static_cast<u32>(location));
    if (it == uni_names.end() || it->second != "inPMVMat") return;
    if (dump_this && evs.size() < 20000) {
        static int callers_logged = 0;
        if (callers_logged < 12) {
            callers_logged++;
            LOGI("dump: inPMVMat upload from lr %08x, data at %08x", caller_lr, caller_ptr);
        }
        Ev e{'M', 0, cur_program, {}};
        std::copy(m, m + 16, e.m);
        evs.push_back(e);
    }
    const u8* b = reinterpret_cast<const u8*>(m);
    u64 k = 0xcbf29ce484222325ull;
    for (u32 i = 0; i < 64; i++) k = (k ^ b[i]) * 0x100000001b3ull;
    for (u32 i = 0; i < n_cands; i++)
        if (cands[i].key == k) {
            cands[i].n++;
            return;
        }
    if (n_cands == sizeof cands / sizeof cands[0]) return;
    Cand& c = cands[n_cands++];
    c.key = k;
    c.n = 1;
    std::copy(m, m + 16, c.m);
}

bool frame_skip_draw() {
    if (!skip_prog) return false;
    return skip_prog < 0 ? after_depth_clear : static_cast<int>(cur_program) == skip_prog;
}

void frame_note_gl(char kind, u32 value) {
    if (kind == 'C' && value == 0x100) after_depth_clear = true;
    if (!dump_this || evs.size() >= 20000) return;
    evs.push_back(Ev{kind, value, cur_program, {}});
}

void frame_note_caller(u32 lr, u32 ptr) {
    caller_lr = lr;
    caller_ptr = ptr;
}
