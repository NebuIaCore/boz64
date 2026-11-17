#include "smooth.h"
#include "flame.h"
#include "patches.h"

#include <sys/system_properties.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <unordered_map>
#include <string>
#include <vector>

// All of this runs on the game's render thread.
namespace {

u64 now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000000000ull + static_cast<u64>(ts.tv_nsec);
}

// Camera objects: CIsTransform (primary vtable found through RTTI); its world matrix is a CIwFMat at +0x38:
// three rows (the local x, y, z axes in world space) and the position.
constexpr u32 TRANSFORM_MATRIX = 0x38;
constexpr u32 SCAN_END = 0x10000000;

u32 img_lo = 0, img_hi = 0;
std::vector<u32> transform_vts;
bool enabled = true;

std::unordered_map<u64, bool> pmv_locs;  // program << 32 | location -> is inPMVMat
u32 cur_prog = 0;
bool is_pmv(int location) {
    auto it = pmv_locs.find((static_cast<u64>(cur_prog) << 32) | static_cast<u32>(location));
    return it != pmv_locs.end() && it->second;
}

enum Phase { BEFORE_WORLD, WORLD, AFTER_WORLD } phase = BEFORE_WORLD;

struct Cam {
    double r[3][3];  // rows: the camera's axes in world space
    double t[3];     // position
};

u32 cam_obj = 0;  // the CIsTransform the renderer's view comes from
u64 next_scan = 0;
u32 cam_misses = 0;

bool read_cam(u32 obj, Cam& c) {
    const float* f = reinterpret_cast<const float*>(g::mem + obj + TRANSFORM_MATRIX);
    for (int i = 0; i < 12; i++)
        if (!std::isfinite(f[i])) return false;
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) c.r[i][j] = f[i * 3 + j];
        c.t[i] = f[9 + i];
        double n = c.r[i][0] * c.r[i][0] + c.r[i][1] * c.r[i][1] + c.r[i][2] * c.r[i][2];
        if (n < 0.98 || n > 1.02) return false;
    }
    return true;
}

// The projection, learned from the world matrix (projection x view) of the last frame:
// rows v0 = (W0 - cx W3) / sx, v1 = (W1 - cy W3) / sy, v2 = W3 are the view rows, and each equals
// a camera axis up to a sign (flip[i]).
struct Proj {
    bool ok = false;
    double p[4][4], inv[4][4];
    double flip[3];
} proj;

bool invert4(const double m[4][4], double out[4][4]) {
    double a[4][8];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 8; j++) a[i][j] = j < 4 ? m[i][j] : (j - 4 == i ? 1 : 0);
    for (int c = 0; c < 4; c++) {
        int piv = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        if (std::fabs(a[piv][c]) < 1e-12) return false;
        if (piv != c)
            for (int j = 0; j < 8; j++) std::swap(a[c][j], a[piv][j]);
        double d = a[c][c];
        for (int j = 0; j < 8; j++) a[c][j] /= d;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            double f = a[r][c];
            for (int j = 0; j < 8; j++) a[r][j] -= f * a[c][j];
        }
    }
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) out[i][j] = a[i][j + 4];
    return true;
}

// GL matrices here are column-major: element (row i, column j) is m[j * 4 + i].
double el(const float* m, int i, int j) { return m[j * 4 + i]; }

bool learn_proj(const float* w, const Cam& cam, Proj& out) {
    double row[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) row[i][j] = el(w, i, j);
    auto dot3 = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    double n3 = std::sqrt(dot3(row[3], row[3]));
    if (n3 < 1e-6) return false;  // not a perspective matrix
    double v[3][4];
    for (int j = 0; j < 4; j++) v[2][j] = row[3][j] / n3;
    double cx = dot3(row[0], v[2]), cy = dot3(row[1], v[2]), a = dot3(row[2], v[2]);
    double r0[4], r1[4];
    for (int j = 0; j < 4; j++) r0[j] = row[0][j] - cx * v[2][j], r1[j] = row[1][j] - cy * v[2][j];
    double sx = std::sqrt(dot3(r0, r0)), sy = std::sqrt(dot3(r1, r1));
    if (sx < 1e-6 || sy < 1e-6) return false;
    for (int j = 0; j < 4; j++) v[0][j] = r0[j] / sx, v[1][j] = r1[j] / sy;
    for (int i = 0; i < 3; i++) {
        double d = dot3(v[i], cam.r[i]);
        if (std::fabs(d) < 0.999) return false;  // this camera is not the one the frame was drawn with
        out.flip[i] = d > 0 ? 1 : -1;
    }
    double b = row[2][3] - a * v[2][3];
    // W = P * V, with V's rows (v0, v1, v2) unit length
    std::memset(out.p, 0, sizeof out.p);
    out.p[0][0] = sx, out.p[0][2] = cx;
    out.p[1][1] = sy, out.p[1][2] = cy;
    out.p[2][2] = a, out.p[2][3] = b;
    out.p[3][2] = n3;
    if (!invert4(out.p, out.inv)) return false;
    out.ok = true;
    return true;
}

// rotations as quaternions for blending
struct Quat { double w, x, y, z; };
Quat to_quat(const double m[3][3]) {
    Quat q;
    double tr = m[0][0] + m[1][1] + m[2][2];
    if (tr > 0) {
        double s = std::sqrt(tr + 1.0) * 2;
        q = {0.25 * s, (m[2][1] - m[1][2]) / s, (m[0][2] - m[2][0]) / s, (m[1][0] - m[0][1]) / s};
    } else if (m[0][0] > m[1][1] && m[0][0] > m[2][2]) {
        double s = std::sqrt(1.0 + m[0][0] - m[1][1] - m[2][2]) * 2;
        q = {(m[2][1] - m[1][2]) / s, 0.25 * s, (m[0][1] + m[1][0]) / s, (m[0][2] + m[2][0]) / s};
    } else if (m[1][1] > m[2][2]) {
        double s = std::sqrt(1.0 + m[1][1] - m[0][0] - m[2][2]) * 2;
        q = {(m[0][2] - m[2][0]) / s, (m[0][1] + m[1][0]) / s, 0.25 * s, (m[1][2] + m[2][1]) / s};
    } else {
        double s = std::sqrt(1.0 + m[2][2] - m[0][0] - m[1][1]) * 2;
        q = {(m[1][0] - m[0][1]) / s, (m[0][2] + m[2][0]) / s, (m[1][2] + m[2][1]) / s, 0.25 * s};
    }
    return q;
}
void from_quat(Quat q, double m[3][3]) {
    double n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
    q.w /= n, q.x /= n, q.y /= n, q.z /= n;
    m[0][0] = 1 - 2 * (q.y * q.y + q.z * q.z), m[0][1] = 2 * (q.x * q.y - q.z * q.w), m[0][2] = 2 * (q.x * q.z + q.y * q.w);
    m[1][0] = 2 * (q.x * q.y + q.z * q.w), m[1][1] = 1 - 2 * (q.x * q.x + q.z * q.z), m[1][2] = 2 * (q.y * q.z - q.x * q.w);
    m[2][0] = 2 * (q.x * q.z - q.y * q.w), m[2][1] = 2 * (q.y * q.z + q.x * q.w), m[2][2] = 1 - 2 * (q.x * q.x + q.y * q.y);
}
Quat slerp(Quat a, Quat b, double t) {
    double d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
    if (d < 0) b = {-b.w, -b.x, -b.y, -b.z}, d = -d;
    if (d > 0.9995) return {a.w + (b.w - a.w) * t, a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
    double th = std::acos(d), s = std::sin(th), wa = std::sin((1 - t) * th) / s, wb = std::sin(t * th) / s;
    return {a.w * wa + b.w * wb, a.x * wa + b.x * wb, a.y * wa + b.y * wb, a.z * wa + b.z * wb};
}
Cam blend(const Cam& a, const Cam& b, double t) {
    if (t >= 1) return b;
    if (t <= 0) return a;
    Cam c;
    from_quat(slerp(to_quat(a.r), to_quat(b.r), t), c.r);
    for (int i = 0; i < 3; i++) c.t[i] = a.t[i] + (b.t[i] - a.t[i]) * t;
    return c;
}
bool same_cam(const Cam& a, const Cam& b) { return std::memcmp(&a, &b, sizeof a) == 0; }
double dist(const Cam& a, const Cam& b) {
    double d = 0;
    for (int i = 0; i < 3; i++) d += (a.t[i] - b.t[i]) * (a.t[i] - b.t[i]);
    return std::sqrt(d);
}
double angle_between(const Cam& a, const Cam& b) {
    double tr = 0;
    for (int i = 0; i < 3; i++)
        for (int k = 0; k < 3; k++) tr += a.r[i][k] * b.r[i][k];
    return std::acos(std::max(-1.0, std::min(1.0, (tr - 1) / 2)));
}

// blending state
bool have_real = false;
Cam last_real, seg_from, seg_to;
u64 key_ns = 0, last_change = 0, last_frame = 0;  // key_ns: the latest player update, on the real clock
u32 last_seq = 0;
u32 slot_score[8] = {};  // big camera changes seen per channel-1 slot: the player's slot scores highest
double frame_secs = 1 / 60.0;

// this frame
Cam frame_cam;
bool frame_cam_ok = false;
bool correcting = false;
float corr[16];  // column-major
float out_buf[16];

// world matrix of this frame (the most used inPMVMat), to learn the projection and check the camera
struct Cand { u64 key; u32 n; float m[16]; };
Cand cands[32];
u32 n_cands = 0;

// the world matrix as drawn (after any correction), between the colour + depth clear and the depth-only one: what
// flame.cpp draws the flamethrower's flame with, just before that clear
bool fw_open = false;
Cand fw_cands[16];
u32 fw_n = 0;
void note_flame_world(const float* m) {
    for (u32 i = 0; i < fw_n; i++)
        if (memcmp(fw_cands[i].m, m, 64) == 0) {
            fw_cands[i].n++;
            return;
        }
    if (fw_n < 16) {
        memcpy(fw_cands[fw_n].m, m, 64);
        fw_cands[fw_n].n = 1;
        fw_n++;
    }
}

// statistics
std::string st_trace;
u64 st_prev_ns = 0;
Cam st_prev;
bool st_prev_ok = false;
u32 st_passes = 0, st_pmv = 0, st_all4 = 0, st_world_frames = 0, st_frames = 0, st_changes = 0, st_corrected = 0, st_snaps = 0, st_gap[6] = {};

void count_world(const float* m) {
    u64 k = 0xcbf29ce484222325ull;
    const u8* b = reinterpret_cast<const u8*>(m);
    for (int i = 0; i < 64; i++) k = (k ^ b[i]) * 0x100000001b3ull;
    for (u32 i = 0; i < n_cands; i++)
        if (cands[i].key == k) {
            cands[i].n++;
            return;
        }
    if (n_cands < 32) {
        cands[n_cands].key = k;
        cands[n_cands].n = 1;
        std::memcpy(cands[n_cands].m, m, 64);
        n_cands++;
    }
}

void scan_for_camera(const float* w) {
    // A CIsTransform whose axes match the world matrix's view rows. Heap objects live above the image.
    u64 t0 = now_ns();
    u32 found = 0, matches = 0, objs = 0;
    for (u32 a = (img_hi + 3) & ~3u; a < SCAN_END; a += 4) {
        u32 v = *reinterpret_cast<const u32*>(g::mem + a);
        bool is_vt = false;
        for (u32 vt : transform_vts) is_vt |= v == vt;
        if (!is_vt) continue;
        objs++;
        Cam c;
        Proj p;
        if (!read_cam(a, c) || !learn_proj(w, c, p)) continue;
        matches++;
        if (!found) found = a;
    }
    LOGI("smooth: camera transform %08x (%u matching of %u) found in %.1f ms; world rows %.3f %.3f %.3f | %.3f %.3f %.3f | %.3f %.3f %.3f",
         found, matches, objs, (now_ns() - t0) / 1e6, w[0], w[4], w[8], w[1], w[5], w[9], w[3], w[7], w[11]);
    if (found) cam_obj = found;
}

void make_correction(const Cam& shown, const Cam& real) {
    // C = P * F * inv(shown) * real * F * inv(P): moves what the real camera sees to where the shown one sees it.
    double d[4][4] = {};
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) {
            double s = 0;
            for (int k = 0; k < 3; k++) s += shown.r[i][k] * real.r[j][k];
            d[i][j] = s * proj.flip[i] * proj.flip[j];
        }
        double s = 0;
        for (int k = 0; k < 3; k++) s += shown.r[i][k] * (real.t[k] - shown.t[k]);
        d[i][3] = s * proj.flip[i];
    }
    d[3][3] = 1;
    double pd[4][4] = {}, c[4][4] = {};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++) pd[i][j] += proj.p[i][k] * d[k][j];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++) c[i][j] += pd[i][k] * proj.inv[k][j];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) corr[j * 4 + i] = static_cast<float>(c[i][j]);
}

void start_world_pass() {
    phase = WORLD;
    st_passes++;
    correcting = false;
    frame_cam_ok = cam_obj && read_cam(cam_obj, frame_cam);
    if (!frame_cam_ok || !enabled) return;
    u64 now = now_ns();
    const Cam& real = frame_cam;
    if (!have_real) {
        have_real = true;
        last_real = seg_from = seg_to = real;
        return;
    }
    // The player (and the camera with it) moves on one slot of scheduler channel 1: a fixed step every 32 ms
    // of game time. Channel 0 adds small head-bob changes every 16 ms. Each frame shows the view between the
    // player's last two positions by the time passed since its latest update, measured on the game's own
    // scheduler clock, so the pace stays even whatever the frame rate or frame timing.
    SchedInfo si[2];
    patches_sched_last(si);
    SchedInfo c1 = si[1];
    bool fresh = c1.seq != last_seq;
    if (!fresh) c1.ticks = 0;  // no scheduler update since the previous frame
    last_seq = c1.seq;
    // The frame's time: when the game's update read its clock (evenly spaced, unlike the time this pass starts,
    // which moves with how long the game's update took).
    u64 t_frame = fresh && c1.at_ns <= now ? c1.at_ns : now;
    u32 nslots = c1.slots >= 1 && c1.slots <= 8 ? c1.slots : 2;
    double period_ms = c1.period_ms ? c1.period_ms : 16;
    u64 update_ns = static_cast<u64>(period_ms * nslots * 1e6);
    // the player's slot: the one whose updates move the camera
    bool big = dist(real, last_real) > 2 || angle_between(real, last_real) > 0.005;
    if (big && c1.ticks == 1 && ++slot_score[c1.first_slot % nslots] > 1000)
        for (u32& sc : slot_score) sc /= 2;
    u32 pslot = 0;
    for (u32 i = 1; i < nslots; i++)
        if (slot_score[i] > slot_score[pslot]) pslot = i;
    int last_j = -1;
    for (u32 j = 0; j < c1.ticks; j++)
        if ((c1.first_slot + j) % nslots == pslot) last_j = static_cast<int>(j);
    bool player_moved = last_j >= 0;
    if (player_moved) {
        // when, on the real clock, the game time of that update was; smoothed, as the game clock counts in ms
        double back_ms = (c1.ticks - 1 - last_j) * period_ms + c1.left_ms;
        u64 est = c1.at_ns - static_cast<u64>(back_ms * 1e6);
        u64 pred = key_ns + update_ns;
        s64 err = static_cast<s64>(est - pred);
        key_ns = key_ns && err > -4000000 && err < 4000000 ? pred + err / 8 : est;
        u64 gap = now - last_change;
        st_gap[gap < 12000000 ? 0 : gap < 20000000 ? 1 : gap < 28000000 ? 2 : gap < 36000000 ? 3 : gap < 50000000 ? 4 : 5]++;
        last_change = now;
    }
    double a = t_frame > key_ns ? std::min(1.0, static_cast<double>(t_frame - key_ns) / update_ns) : 0;
    if (!same_cam(real, last_real)) {
        st_changes++;
        Cam shown_now = blend(seg_from, seg_to, a);
        // a teleport, respawn or camera cut: show the new view straight away
        bool snap = dist(real, shown_now) > 250 || angle_between(real, shown_now) > 0.8;
        if (snap) {
            st_snaps++;
            seg_from = real;
        } else if (player_moved) {
            seg_from = seg_to;
        }
        seg_to = real;  // between player updates: head bob, folded into the current blend
        last_real = real;
    }
    // Below the update rate every frame already shows a new position; blending would only add delay.
    Cam shown = real;
    if (frame_secs * 1e9 < update_ns * 0.9 && proj.ok && a < 1) {
        shown = blend(seg_from, seg_to, a);
        make_correction(shown, real);
        correcting = true;
    }
    if (st_trace.size() < 1800) {
        // the shown camera's speed each frame (units/s and degrees/s), * where the player moved
        double secs = st_prev_ns && t_frame > st_prev_ns ? (t_frame - st_prev_ns) / 1e9 : 0;
        char b[40];
        snprintf(b, sizeof b, "%s%d/%d ", player_moved ? "*" : "", st_prev_ok && secs ? static_cast<int>(dist(shown, st_prev) / secs) : 0,
                 st_prev_ok && secs ? static_cast<int>(angle_between(shown, st_prev) * 57.2958 / secs) : 0);
        st_trace += b;
    }
    st_prev_ns = t_frame;
    st_prev = shown;
    st_prev_ok = true;
}

}  // namespace

void smooth_init(u32 lo, u32 hi) {
    img_lo = lo, img_hi = hi;
    char v[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.boz64.smooth", v) > 0 && v[0] == '0') enabled = false;
    // RTTI: the type name "12CIsTransform", its type_info (name pointer at +4), and vtables (type_info at -4,
    // offset-to-top 0 at -8)
    static const char name[] = "\0" "12CIsTransform";
    const u8* base = g::mem + lo;
    const u8* hit = static_cast<const u8*>(memmem(base, hi - lo, name, sizeof name));
    if (!hit) {
        LOGW("smooth: CIsTransform type name not found; view smoothing is off");
        return;
    }
    u32 str = lo + static_cast<u32>(hit - base) + 1;
    for (u32 a = lo; a + 8 <= hi; a += 4) {
        if (g::rd32(a) != str) continue;
        u32 ti = a - 4;
        for (u32 b = lo + 8; b + 4 <= hi; b += 4)
            if (g::rd32(b) == ti && g::rd32(b - 4) == 0) transform_vts.push_back(b + 4);
    }
    LOGI("smooth: %zu CIsTransform vtable(s)%s", transform_vts.size(), enabled ? "" : ", smoothing turned off by debug.boz64.smooth");
}

void smooth_note_uniform_name(u32 program, const char* name, int location) {
    if (!name || location < 0) return;
    pmv_locs[(static_cast<u64>(program) << 32) | static_cast<u32>(location)] = strcmp(name, "inPMVMat") == 0;
}

void smooth_note_program(u32 program) { cur_prog = program; }

void smooth_note_clear(u32 mask) {
    if ((mask & 0x4100) == 0x4100) {  // color and depth: the 3D world starts
        fw_open = true;
        fw_n = 0;
        if (!transform_vts.empty()) start_world_pass();
    } else if (mask == 0x100) {  // depth only: the first-person weapon, then the HUD
        if (fw_open) {
            fw_open = false;
            const Cand* best = nullptr;
            for (u32 i = 0; i < fw_n; i++)
                if (!best || fw_cands[i].n > best->n) best = &fw_cands[i];
            if (best && best->n >= 3) flame_world_end(best->m);
        }
        if (phase == WORLD) phase = AFTER_WORLD;
    }
}

const float* smooth_uniform4fv(int location, int count, const float* v) {
    return count == 4 ? smooth_matrix(location, 1, false, v) : v;
}

const float* smooth_matrix(int location, int count, bool transpose, const float* v) {
    if (count == 1) st_all4++;
    if (count == 1 && is_pmv(location)) st_pmv++;
    if (count != 1 || transpose || !v || !is_pmv(location)) return v;
    if (v[3] == 0 && v[7] == 0 && v[11] == 0) return v;  // not a perspective transform
    const float* out = v;
    if (phase == WORLD) {
        count_world(v);
        if (correcting) {
            for (int j = 0; j < 4; j++)
                for (int i = 0; i < 4; i++)
                    out_buf[j * 4 + i] = corr[i] * v[j * 4] + corr[4 + i] * v[j * 4 + 1] + corr[8 + i] * v[j * 4 + 2] + corr[12 + i] * v[j * 4 + 3];
            st_corrected++;
            out = out_buf;
        }
    }
    if (fw_open) note_flame_world(out);
    return out;
}

void smooth_frame_end() {
    u64 now = now_ns();
    if (last_frame) frame_secs = frame_secs * 0.9 + (now - last_frame) / 1e9 * 0.1;
    last_frame = now;
    st_frames++;
    if (phase != BEFORE_WORLD && n_cands) {
        const Cand* best = nullptr;
        for (u32 i = 0; i < n_cands; i++)
            if (!best || cands[i].n > best->n) best = &cands[i];
        st_world_frames++;
        if (best->n >= 3) {
            Proj p;
            if (frame_cam_ok && learn_proj(best->m, frame_cam, p)) {
                proj = p;
                cam_misses = 0;
            } else if (++cam_misses > 30) {
                // the camera object is gone (new level) or was never found: look for it (at most every 3 s)
                proj.ok = false;
                cam_obj = 0;
                if (now >= next_scan) {
                    static u32 failed = 0;
                    scan_for_camera(best->m);
                    failed = cam_obj ? 0 : failed + 1;
                    next_scan = now + (failed < 3 ? 3000000000ull : 30000000000ull);
                    cam_misses = 0;
                }
            }
        }
    }
    n_cands = 0;
    phase = BEFORE_WORLD;
}

void smooth_stats(char* out, size_t n) {
    snprintf(out, n, "smooth: passes %u, 4-vector uploads %u (inPMVMat %u), world frames %u, %u pmv locations; camera %08x proj %d, %u frames, %u camera changes, player moves %u (slot scores %u/%u; gaps <12ms %u, <20 %u, <28 %u, <36 %u, <50 %u, more %u), %u snaps, %u corrected uploads",
             st_passes, st_all4, st_pmv, st_world_frames, static_cast<u32>(pmv_locs.size()), cam_obj, proj.ok, st_frames, st_changes, st_gap[0] + st_gap[1] + st_gap[2] + st_gap[3] + st_gap[4] + st_gap[5], slot_score[0], slot_score[1], st_gap[0], st_gap[1], st_gap[2], st_gap[3], st_gap[4], st_gap[5], st_snaps, st_corrected);
    if (!st_trace.empty()) LOGI("smooth: shown speed per frame (units/s / deg/s, * = player update): %s", st_trace.c_str());
    st_trace.clear();
    st_passes = st_pmv = st_all4 = st_world_frames = 0;
    st_frames = st_changes = st_snaps = st_corrected = 0;
    std::memset(st_gap, 0, sizeof st_gap);
}
