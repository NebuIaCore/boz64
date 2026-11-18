// World at War's flamethrower, rebuilt for Black Ops (tools/wawguns.py adds the gun to the mystery boxes): it runs on
// heat instead of ammo, throws World at War's flame (spheres of fire that do the damage, drawn as World at War draws
// them), and the HUD shows a heat gauge where the ammo counter was.
//
// World at War (kiwi, ARM, VA = file offset + 0x1000; tunables from data/console.bin, WorldScale 4.0, units mm):
//  - heat: the gun record's +0x100 is how long it fires from cold to overheating (16384 = 4 s; Pack-a-Punched 28672 =
//    7 s). Each update (0x20fd00) the heat rises by dt / fire time while it fires and falls as fast while it doesn't;
//    past full it overheats and won't fire (0x206ac8) until it has cooled to half (0x20fc30). The HUD (0x13d0d4) hides
//    the clip and reserve and shows "flamethrower_fuel", a bar filled to the heat.
//  - the flame (0x1df0f0, each update, the state at the player's gun +0x140; set up at 0x1df9d8): while the trigger is
//    held, and for at least FlamethrowerMinSpurtTime (250 ms) once it was, a sphere is emitted from the muzzle every
//    (FlamethrowerMaxRange / speed) / FlamethrowerNumSpheres ms (0x1dfcc8: 2800 / 5 / 6 = 93 ms) into a ring of 6
//    (0x1deab8). Each keeps the aim it left with and flies at FlamethrowerSpeed x WorldScale (1.25 x 4 = 5 mm per ms);
//    it is put out once its distance travelled plus its radius (FlamethrowerGirth x WorldScale = 38 x 4 = 152 mm)
//    reaches the first wall along its path (a ray cast when it is emitted) or FlamethrowerMaxRange (2800 mm)
//    (0x1def3c).
//  - damage (0x1df2ec, every FlamethrowerDamagePeriod = 80 ms, while any sphere is alive): every zombie overlapping a
//    sphere takes the gun's damage once (0x238a40, no range falloff), through the player's own hit function.
//  - drawing (CRenderableEffectFlamethrower, 0x1eefc8): between each sphere and the next, FlamethrowerEffectNumSides
//    (12) quads fan out from the line joining them, each carrying the whole flame texture, at a radius taken from
//    FlamethrowerEffectRadius over the sphere's age (0 ms 2, 51 ms 9, 187 ms 18, 250 ms 30, x WorldScale). Two particle
//    emitters run alongside (effects.group.bin #e1aa229c): fast flames (10-15 m/s forward, from 40-60 cm ahead) and
//    slow smoke drifting up; their speed is scaled down by how close the wall ahead is (0x1dedd8).
//
// Black Ops here (addresses as dumped at DUMP_BASE; Black Ops' units are centimetres: World at War's mm / 10):
//  - the gun is an automatic (CPlayerWeapon) with a clip of 100 and no reserve. Its shots fire no bullets: the shot
//    function (FUN_0263aff6) asks the gun's behaviour how many bullets a shot fires and passes that on ("mov r1, r0");
//    for the local player's flamethrower that is made 0 (a hook), and the shot tells us it fired.
//  - heat as World at War, from those shots: the clip is filled every frame (never runs dry, never reloads) and kept
//    empty while overheated, so the trigger just clicks (a gun with no ammo and no lightningBoltPickup flag is not
//    switched away from: CSingleShotBehaviour__v15, CPlayerWeapon +0x275). Holstered, it cools.
//  - the spheres as World at War, from the player's view (CWeaponManager +0x50, the transform shots fire from: rows
//    at +0x38, forward at +0x50, eye at +0x5c). The wall ahead comes from the bullets' own ray cast (FUN_0257fa54);
//    zombies along the way are looked past.
//  - damage: every 80 ms each live sphere asks the physics world what overlaps it (FUN_025806f0, the Thundergun's
//    sphere query: a btSphereShape contact test filling a BulletUtils::EntityResults set of entity references), and
//    each zombie found once gets the game's own bullet hit (FUN_0263a8d0: the gun's damage, the SUBJECT_WEAPON_HIT
//    message with its points, and, the gun having incendiaryRounds, the message that sets it on fire). Double Tap
//    doesn't double it (it is not a bullet).
//  - drawing: the world is drawn first, then a depth-only clear, then the first-person gun and the HUD. Just before
//    that clear (smooth.cpp), with the world's own transform (the most used inPMVMat) and its depth buffer, the
//    flame is drawn: World at War's fins between the spheres (smoothed into a curve), its fast flame particles
//    (additive) and its smoke (blended), textured with a fire noise. The gun is drawn over its base.
//  - the HUD: while it is the current gun, the ammo counter's bullet row and number (and the low / no ammo labels) are
//    hidden, and flame_draw (just before the frame is shown) draws the gauge in their place.
//  - the sound as World at War's fire state plays it (playSound x2 entering it, stopSound leaving it): the ignition
//    (2 s) once a spurt and the flame (6 s, its fire sound) until the spurt ends, through Black Ops' looping-sound
//    helper (FUN_0265246c, the Death Machine's). Black Ops would play a gun's fire sound with every shot (4 a second
//    here, each 6 s long: they piled up) and the Pack-a-Punch shot after it, so tools/wawguns.py gives the
//    flamethrower neither.
// Also here (it shares the sphere query, the ray and the bullet hit): the Wunderwaffe's chain lightning as Black Ops
// (2010) runs it, see "the Wunderwaffe's chain lightning" below; debug.boz64.tesla=0 leaves the game's own chain.
// Settings (boz64.props): debug.boz64.flamethrower=0 turns the flamethrower part off; debug.boz64.flame_range (metres, World at
// War's 2.8), debug.boz64.flame_size (how big it is drawn, 1.0) and debug.boz64.flame_nozzle (where it starts on the
// screen: centimetres forward, right, up of the eye, "28,7,-9").
#include "flame.h"

#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <vector>

#include "cpu.h"
#include "doubletap.h"

namespace {
constexpr u32 DUMP_BASE = 0x02454000;
u32 image = 0;
bool enabled = false;
u32 at(u32 a) { return a - DUMP_BASE + image; }
bool valid(u32 a) { return a >= g::NULL_GUARD && a < g::ALLOC_END; }
u32 call(u32 fn, std::initializer_list<u32> args = {}) { return static_cast<u32>(call_guest(at(fn), args)); }
u32 fbits(float f) {
    u32 u;
    memcpy(&u, &f, 4);
    return u;
}

u64 now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}
constexpr u64 MS = 1000000ull;

u32 djb2(const char* s) {
    u32 h = 5381;
    while (*s) h = h * 33 + static_cast<u8>(*s++);
    return h;
}

struct V3 {
    float x = 0, y = 0, z = 0;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float len(V3 a) { return std::sqrt(dot(a, a)); }
V3 norm(V3 a) {
    float l = len(a);
    return l > 1e-6f ? a * (1 / l) : V3{};
}
V3 rdv(u32 a) { return {g::rdf(a), g::rdf(a + 4), g::rdf(a + 8)}; }
void wrv(u32 a, V3 v, float w = 0) {
    g::wrf(a, v.x), g::wrf(a + 4, v.y), g::wrf(a + 8, v.z);
    g::wrf(a + 12, w);
}

// ---- World at War's numbers, in Black Ops units (cm) and ms ----
constexpr int NUM_SPHERES = 6;            // FlamethrowerNumSpheres
constexpr float GIRTH = 15.2f;            // FlamethrowerGirth 38 x WorldScale 4 = 152 mm: each sphere's radius
constexpr float SPEED = 0.5f;             // FlamethrowerSpeed 1.25 x 4 = 5 mm/ms
float max_range = 280.0f;                 // FlamethrowerMaxRange 2800 mm (debug.boz64.flame_range)
float emit_period = 0;                    // (max range / speed) / spheres: 93 ms
constexpr float MIN_SPURT = 250.0f;       // FlamethrowerMinSpurtTime
constexpr float DAMAGE_PERIOD = 80.0f;    // FlamethrowerDamagePeriod
constexpr int NUM_SIDES = 12;             // FlamethrowerEffectNumSides
// FlamethrowerEffectRadius: the drawn radius over a sphere's age (ms, cm)
constexpr float RADIUS_KEYS[][2] = {{0, 0.8f}, {51, 3.6f}, {187, 7.2f}, {250, 12.0f}};
float size_scale = 1.0f;                  // debug.boz64.flame_size
constexpr float FIN_SCALE = 2.2f;         // the fins against World at War's small screen
V3 nozzle_ofs = {28, 7, -9};              // forward, right, up of the eye (debug.boz64.flame_nozzle)

// heat
u32 gun_normal = 0, gun_upgraded = 0;
constexpr float FIRE_TIME = 4.0f, FIRE_TIME_UPGRADED = 7.0f;
constexpr float COOLED = 0.5f;            // an overheated gun fires again at half heat
constexpr float MAX_DT = 0.1f;            // longer gaps (loading, the pause menu) count as this
constexpr u64 AWAY = 500 * MS;            // no update for this long: play stopped (the HUD may be rebuilt)

// ---- game objects and functions (addresses as dumped at DUMP_BASE) ----
constexpr u32 CTL_WEAPONS = 0x88;  // CPlayerController: its CWeaponManager
constexpr u32 WM_OWNER = 0x48;     // CWeaponManager: its owner (entity reference)
constexpr u32 WM_VIEW = 0x50;      // CWeaponManager: the transform shots fire from
constexpr u32 WM_SLOTS = 0x1e0;    // CWeaponManager: current, holstered, knife, grenade, special
constexpr u32 TR_X = 0x38, TR_Y = 0x44, TR_FORWARD = 0x50, TR_POS = 0x5c;  // CIsTransform's world matrix
constexpr u32 GUN_CLIP = 0x2c, GUN_AMMO = 0x30, GUN_HASH = 0x5c, GUN_CLIP_SIZE = 0x80, GUN_WEAPONS = 0x2ec;
constexpr u32 INPUT_MGR = 0x028c1498;  // global holding the CInputManager
constexpr u32 IM_FIRE = 0x41, IM_FIRE_HOLD = 0x4c;
// the shot: FUN_0263aff6(gun) "blx [behaviour + 0x2c]; mov r1, r0" (r0 = bullets per shot; r5 = the gun)
constexpr u32 SHOT_HOOK_AT = 0x0263b012;
constexpr u16 SHOT_HOOK_ORIG = 0x4601;
constexpr u32 HOOK_SHOT = 0xbf;
constexpr u32 FN_TRANSFORM_UPDATE = 0x0251c138 | 1;  // (CIsTransform): brings its world matrix up to date
constexpr u32 FN_REF_GET = 0x02512568 | 1;           // (entity reference): the entity, or 0
// (from, to (vec4s), out hit point, out normal, out entity reference, out part, group, mask, out material): hit?
constexpr u32 FN_RAY = 0x0257fa54 | 1;
constexpr u32 RAY_GROUP = 0xffffffef, RAY_MASK = 0xffffffff;  // as the bullets cast
// (matrix: 3 rows + position, radius (float bits), BulletUtils::EntityResults*, mask, group)
constexpr u32 FN_SPHERE_QUERY = 0x025806f0 | 1;
constexpr u32 QUERY_MASK = 4, QUERY_GROUP = 0x20;  // as the Thundergun asks
constexpr u32 GOT_ENTITY_RESULTS = 0x0286b390;     // its vtable (+8)
constexpr u32 FN_RESULTS_DTOR = 0x02577100 | 1;
constexpr u32 FN_BUCKET_PUSH = 0x02577088 | 1, FN_BUCKET_POP = 0x025770ac | 1;  // around the set's allocation
constexpr u32 FN_NEW = 0x027a57b0;                                               // operator new (ARM)
// (gun, entity reference, hit part (0: none), &point, &normal, hit number, material): the bullet hit
constexpr u32 FN_HIT = 0x0263a8d0 | 1;
// sound: Black Ops' looping-sound helper (the Death Machine's spin; 0x18 bytes: +0 on, +4 the CIwSoundInst playing,
// +8 its CIwSoundSpec, +0x14 repeats left). It plays 2D with no position; the instance's end callback (natural end,
// a stop (processed on the sound manager's next update) or its channel taken) clears +4.
constexpr u32 FN_LOOP_SPEC = 0x02652440 | 1;  // (helper, name hash): +8 = the CIwSoundSpec so named, or 0
constexpr u32 FN_LOOP_SET = 0x0265246c | 1;   // (helper, on, position (0: none)): plays (unless playing) or stops
constexpr u32 INST_FLAGS = 0x16, INST_IN_USE = 4, INST_USER = 0x24;  // CIwSoundInst (allocation clears them)
// IwUI
constexpr u32 UI_MANAGER = 0x028bd280;            // global holding the UI manager
constexpr u32 FN_FIND_ELEMENT = 0x02669014 | 1;   // (manager, const char* name, 0): the element, or 0
constexpr u32 FN_SET_VISIBLE = 0x0275a518;        // CIwUIElement::SetVisible(this, bool) (ARM)
constexpr u32 FN_GET_FLAG = 0x02758858;           // CIwUIElement (this, flag): whether it is set (ARM)
constexpr u32 EL_PARENT = 0x18, EL_FLAGS_CACHED = 0x2c, EL_FLAGS = 0x2e, ELF_VISIBLE = 2, EL_POS = 0x5c, EL_SIZE = 0x64;
struct Check { u32 fn, word; };
constexpr Check CHECKS[] = {
    {FN_FIND_ELEMENT, 0xf100b570}, {FN_SET_VISIBLE, 0xe92d407f}, {FN_GET_FLAG, 0xe92d4013},
    {FN_TRANSFORM_UPDATE, 0x3030f890}, {FN_REF_GET, 0x4603b508}, {FN_RAY, 0x4ff0e92d},
    {FN_SPHERE_QUERY, 0x43f0e92d}, {FN_RESULTS_DTOR, 0x4a154b14}, {FN_BUCKET_PUSH, 0x4a074b06},
    {FN_BUCKET_POP, 0x4a074b06}, {FN_NEW, 0xe92d4010}, {FN_HIT, 0x4ff0e92d},
    {FN_LOOP_SPEC, 0xb5104b07}, {FN_LOOP_SET, 0x460db537},
};
u32 buf = 0;  // guest memory for the calls above
constexpr u32 BUF_SIZE = 512, BUF_SOUNDS = 384;  // the sound helpers live at the end (their callbacks write them)

// ---- heat (game thread) ----
struct Heat {
    u32 gun = 0;
    float heat = 0;
    bool over = false;
    bool seen = false;
};
Heat heats[3];
u64 last_update = 0;

bool is_flame(u32 gun) {
    if (!valid(gun)) return false;
    u32 h = g::rd32(gun + GUN_HASH);
    return h == gun_normal || h == gun_upgraded;
}

Heat* heat_of(u32 gun) {
    for (Heat& s : heats)
        if (s.gun == gun) return &s;
    for (Heat& s : heats) {
        if (s.gun) continue;
        s = Heat{};
        s.gun = gun;
        return &s;
    }
    return nullptr;
}

// ---- the flame (game thread) ----
struct Sphere {
    bool on = false;
    V3 pos, dir, start;
    float limit = 0, age = 0;  // how far it may travel (cm); ms since it left
};
struct Flame {
    u32 gun = 0;  // the local player's flamethrower in hand (0: none); the shot hook stops its bullets
    Sphere s[NUM_SPHERES];
    int head = 0;               // the newest sphere
    float emit_in = 0;          // ms until the next sphere
    float damage_in = 0;        // ms until the next damage
    u64 last_shot = 0, prev_shot = 0, spurt_from = 0;
    float shot_gap = 100;       // ms between shots (measured)
    float wall = 280;           // the wall ahead when the last sphere left (cm)
    bool held = false;          // the input manager's fire flags, this update
    int held_run = 0;           // updates in a row they were set
    bool hold_known = false;    // seen held this spurt: their release ends it
} fl;

// Firing: World at War's flame runs while the trigger is held, and for at least MIN_SPURT. Here the shots tell: they
// come every shot_gap ms (the flamethrower's: World at War's 266 ms fire animation), so between two the trigger is
// still held. Where the input manager's fire flags stay set while it is (the mouse, a pad: src/pad.cpp; the touch
// controls' hold), their release ends the spurt at once (after MIN_SPURT).
bool firing(u64 now) {
    if (!fl.last_shot) return false;
    if ((now - fl.spurt_from) / 1e6f < MIN_SPURT) return true;
    float since = (now - fl.last_shot) / 1e6f;
    bool recent = since < std::clamp(fl.shot_gap * 1.25f + 30.0f, 60.0f, 450.0f);
    return recent && (!fl.hold_known || fl.held);
}

void note_held(bool held) {
    fl.held = held;
    fl.held_run = held ? fl.held_run + 1 : 0;
    if (fl.held_run >= 3) fl.hold_known = true;  // (not just the one update a tap sets them for)
}

void note_shot(u64 now) {
    if (!firing(now)) fl.spurt_from = now, fl.emit_in = 0, fl.hold_known = false;
    if (fl.last_shot) {
        float d = (now - fl.last_shot) / 1e6f;
        if (d > 5 && d < 400) fl.shot_gap = fl.shot_gap * 0.7f + d * 0.3f;
    }
    fl.prev_shot = fl.last_shot;
    fl.last_shot = now;
}

// how far the flame can go from `from` along `dir` before a wall (zombies and players are looked past)
float wall_distance(V3 from, V3 dir, float range) {
    u32 a = buf, b = buf + 16, hit = buf + 32, nrm = buf + 48, ref = buf + 64, part = buf + 68, mat = buf + 72;
    float done = 0;
    for (int i = 0; i < 4 && done < range; i++) {
        V3 s = from + dir * done, e = from + dir * range;
        wrv(a, s), wrv(b, e);
        g::wr32(ref, 0), g::wr32(part, 0), g::wr8(mat, 0);
        if (!call(FN_RAY, {a, b, hit, nrm, ref, part, RAY_GROUP, RAY_MASK, mat})) return range;
        float d = dot(rdv(hit) - from, dir);
        if (!g::rd32(ref)) return std::clamp(d, 0.0f, range);  // the world
        done = std::max(done, d) + 60;  // a zombie: look on past it
    }
    return range;
}

void emit(u32 wm) {
    u32 view = g::rd32(wm + WM_VIEW);
    if (!valid(view)) return;
    call(FN_TRANSFORM_UPDATE, {view});
    V3 eye = rdv(view + TR_POS), fwd = norm(rdv(view + TR_FORWARD));
    if (len(fwd) < 0.5f) return;
    float wall = wall_distance(eye, fwd, max_range + nozzle_ofs.x) - nozzle_ofs.x;
    fl.wall = std::max(0.0f, wall);
    fl.head = (fl.head + NUM_SPHERES - 1) % NUM_SPHERES;
    Sphere& s = fl.s[fl.head];
    s = Sphere{};
    s.start = s.pos = eye + fwd * nozzle_ofs.x;
    s.dir = fwd;
    s.limit = std::min(wall, max_range);
    s.on = s.limit > GIRTH;  // a wall right in front: nothing comes out (World at War: 0x1dee9c)
}

void move_spheres(float dt_ms) {
    for (Sphere& s : fl.s) {
        if (!s.on) continue;
        s.pos = s.pos + s.dir * (SPEED * dt_ms);
        s.age += dt_ms;
        if (len(s.pos - s.start) + GIRTH >= s.limit) s.on = false;
    }
}

// the entity references overlapping a sphere (BulletUtils::EntityResults is a std::set: a red-black tree whose header
// node holds the root at +4; each node its children at +8 / +0xc and the reference at +0x10)
int query(V3 c, float radius, u32* out, int max, int n) {
    u32 res = buf + 96, mat = buf + 128, tmp = buf + 176;
    const float rows[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
    for (int i = 0; i < 9; i++) g::wrf(mat + 4 * i, rows[i]);
    g::wrf(mat + 36, c.x), g::wrf(mat + 40, c.y), g::wrf(mat + 44, c.z);
    g::wr32(res, g::rd32(at(GOT_ENTITY_RESULTS)) + 8);
    g::wr16(res + 4, 1), g::wr16(res + 6, 0xffff);
    g::wr32(res + 8, 0);
    call(FN_BUCKET_PUSH, {tmp});
    u32 hdr = call(FN_NEW, {0x14});
    call(FN_BUCKET_POP, {tmp});
    if (!valid(hdr)) return n;
    g::wr8(hdr, 0), g::wr32(hdr + 4, 0), g::wr32(hdr + 8, hdr), g::wr32(hdr + 12, hdr);
    g::wr32(res + 12, hdr), g::wr32(res + 16, 0);
    call(FN_SPHERE_QUERY, {mat, fbits(radius), res, QUERY_MASK, QUERY_GROUP});
    u32 stack[64];
    int sp = 0;
    u32 root = g::rd32(hdr + 4);
    if (valid(root) && g::rd32(res + 16)) stack[sp++] = root;
    for (int guard = 0; sp && guard < 256; guard++) {
        u32 node = stack[--sp];
        u32 ref = g::rd32(node + 16);
        bool have = false;
        for (int i = 0; i < n; i++) have |= out[i] == ref;
        if (!have && ref && n < max) out[n++] = ref;
        for (u32 k : {g::rd32(node + 8), g::rd32(node + 12)})
            if (valid(k) && k != hdr && sp < 64) stack[sp++] = k;
    }
    call(FN_RESULTS_DTOR, {res, 0});
    return n;
}

void damage(u32 gun) {
    u32 wm = g::rd32(gun + GUN_WEAPONS);
    u32 owner = valid(wm) ? g::rd32(wm + WM_OWNER) : 0;
    u32 refs[32];
    int from[32];
    int n = 0;
    for (int i = 0; i < NUM_SPHERES; i++) {
        if (!fl.s[i].on) continue;
        int was = n;
        n = query(fl.s[i].pos, GIRTH, refs, 32, n);
        for (int k = was; k < n; k++) from[k] = i;
    }
    u32 pos = buf + 224, nrm = buf + 240;
    doubletap_pause(true);
    for (int k = 0; k < n; k++) {
        if (refs[k] == owner || !call(FN_REF_GET, {refs[k]})) continue;
        const Sphere& s = fl.s[from[k]];
        wrv(pos, s.pos), wrv(nrm, s.dir * -1.0f);
        call(FN_HIT, {gun, refs[k], 0, pos, nrm, 1, 0});
    }
    doubletap_pause(false);
}

// ---- the Wunderwaffe's chain lightning, as Black Ops (2010) runs it (game thread) ----
// Black Ops' tesla (maps/_zombiemode_weap_tesla.gsc): the first zombie the bolt hurts is arc 1. From each zombie hit at
// arc n, every zombie not yet hit within 300 - 20 n inches of its head and in sight of it (characters looked past) is hit
// at arc n + 1, depth first, nearest (to the first zombie) first, until arc 5 or 20 zombies. Each search waits a
// network frame (50 ms); a zombie at arc n > 1 dies 0.2-0.6 s x n later, after its arc (a bolt flying 0.5 s from the
// zombie before it; none when they are within 128 inches of each other), at most 5 deaths a 100 ms, each
// DoDamage(health + 666) from the player: their kill, with its points and power-up drop chance.
// Black Ops (mobile) has one chain lightning instead (CExplosion, 0x025da08e, spawns "chain_lightning": up to 10 jumps
// from zombie to zombie, each within CChainLightning m_jumpRange), which kills by a message: no kill for the player, so
// no points and only the bare 2 % drop chance (the drops also come from points: CPickupManager, 0x0262a2d4). While the
// local player holds a Wunderwaffe (Call of the Dead's DG-2 or World at War's), that spawn is skipped and the tesla
// above runs, on the next update: zombies, hellhounds and space monkeys found with the Thundergun's sphere query, sight
// from the bullets' ray, each kill the gun's own bullet hit with its damage made health + 666 (doubletap.cpp), each arc
// drawn with the chain's own trail (fired from 'player_weapon' as CChainLightning fires it, 0x02583188, timed to land in
// 0.5 s). A bolt that hurts George Romero spawns the game's chain as before (his reaction to it is the game's own).
constexpr u32 CHAIN_HOOK_AT = 0x025da08c;  // CExplosion: "adds r3, #0x98" (r4 = the explosion) before the spawn
constexpr u16 CHAIN_HOOK_ORIG = 0x3398;
constexpr u32 CHAIN_SKIP = 0x025d9f72;     // where a failed spawn goes on (the area damage)
constexpr u32 HOOK_CHAIN = 0xc0;
constexpr u32 EXPL_RADIUS = 0x40, EXPL_TRANSFORM = 0x6c;  // CExplosion
constexpr u32 FN_TABLE_GET = 0x024a89e8 | 1;    // (component table, entity id): the entity's component, or 0
constexpr u32 FN_BY_ID = 0x025171c0 | 1;        // (entity id, the hash of its name): the entity, or 0
constexpr u32 FN_TRANSFORM_OF = 0x0251c094 | 1; // (entity): its CIsTransform
constexpr u32 FN_HEALTH_OF = 0x025f2bc4 | 1;    // (entity): its CHealth
constexpr u32 FN_LOOK_AT = 0x02512f7a | 1;      // (out matrix: 3 rows (the third forward) + position, &from, &to, &up)
constexpr u32 FN_PROJ_IDS = 0x025e5e70 | 1;     // (): +0xd4 plus +0xd8 (counted up) names the next projectile
constexpr u32 FN_LAUNCHER_OF = 0x02676d54 | 1;  // (entity): its projectile launcher
constexpr u32 FN_FIRE = 0x0267a350 | 1;         // (launcher, spec hash, &matrix, &velocity, -1, projectile id)
constexpr u32 GOT_UP = 0x0286b79c;              // -> (0, 1, 0): y is up
constexpr u32 TABLE_ZOMBIE = 0x028ba968, TABLE_HELLHOUND = 0x028ba264, TABLE_MONKEY = 0x028ba4c8;  // AI components
constexpr u32 TABLE_ROMERO = 0x028ba654;
constexpr u32 HEALTH = 0x48;  // CHealth: health (float)
constexpr Check TESLA_CHECKS[] = {
    {FN_TABLE_GET, 0xab02b513}, {FN_BY_ID, 0xb5134b07}, {FN_TRANSFORM_OF, 0x4603b508}, {FN_HEALTH_OF, 0x4603b508},
    {FN_LOOK_AT, 0xedd2b570}, {FN_PROJ_IDS, 0x4a034b02}, {FN_LAUNCHER_OF, 0x4603b508}, {FN_FIRE, 0x461cb530},
};
// Black Ops' zombie_vars, inches as centimetres
constexpr float INCH = 2.54f;
constexpr int MAX_ARCS = 5, MAX_KILLS = 20, CHOKE = 4;  // tesla_max_arcs, _max_enemies_killed, _network_death_choke
constexpr float RADIUS_START = 300 * INCH, RADIUS_DECAY = 20 * INCH, MIN_FX = 128 * INCH;
constexpr float ARC_MS = 500, FRAME_MS = 50;  // tesla_arc_travel_time; a network frame
constexpr float REACH = (4 * RADIUS_START - 10 * RADIUS_DECAY) * 1.02f;  // arcs 1-4 end to end: all a chain can reach
constexpr float HEAD = 140, TRAIL = 100, BODY = 30;  // cm: its head above its feet, where the arcs fly; its girth
const u32 HASH_TRAIL = djb2("projectile_wunderwaffe_trail_dummy_bullet"), HASH_LAUNCHER = djb2("player_weapon");
const u32 WONDER[] = {djb2("wunderwaffe"), djb2("waw_wunderwaffe"), djb2("waw_wunderwaffe_upgrade")};

bool tesla_on = false;
u32 tb = 0;  // guest memory: from, to (16 each), matrix (48), velocity
u32 ww_gun = 0, ww_cur = 0, ww_hol = 0;  // the local player's Wunderwaffe in hand (0: none), current and holstered guns
u64 ww_seen = 0;
struct Start { V3 pos; float radius; u32 gun; };
std::vector<Start> starts;  // bolts gone off since the last update
struct Zap {
    u64 at;
    bool kill;          // its death (else its arc)
    u32 ref, id, gun;   // the zombie (and its id, to tell it is still there); the gun it dies to
    V3 pos;             // where it was
    u32 src, src_id;    // the zombie the arc comes from (0: the bolt)
    V3 src_pos;
};
std::vector<Zap> zaps;     // pending, soonest not first
std::vector<u32> doomed;   // zombies a chain has hit and not killed yet (zombie_tesla_hit): no other chain takes them
u64 choke_from = 0;
int choked = 0;
u32 zap_seed = 1;

float rnd01() {  // xorshift32
    zap_seed ^= zap_seed << 13, zap_seed ^= zap_seed >> 17, zap_seed ^= zap_seed << 5;
    return (zap_seed >> 8) / 16777216.0f;
}

bool is_wonder(u32 gun) {
    if (!valid(gun)) return false;
    u32 h = g::rd32(gun + GUN_HASH);
    return std::find(std::begin(WONDER), std::end(WONDER), h) != std::end(WONDER);
}

bool has(u32 table, u32 ref) { return call(FN_TABLE_GET, {at(table), g::rd32(ref + 4)}) != 0; }

// a reference kept from an earlier update, if its entity is still there
u32 still(u32 ref, u32 id) { return valid(ref) && g::rd32(ref + 4) == id && call(FN_REF_GET, {ref}) ? ref : 0; }

V3 position(u32 ref, V3 old) {
    u32 tr = ref ? call(FN_TRANSFORM_OF, {ref}) : 0;
    if (!valid(tr)) return old;
    call(FN_TRANSFORM_UPDATE, {tr});
    return rdv(tr + TR_POS);
}

float health(u32 ref) {
    u32 h = call(FN_HEALTH_OF, {ref});
    return valid(h) ? g::rdf(h + HEALTH) : 0;
}

bool in_sight(V3 a, V3 b) {
    float d = len(b - a);
    return d < 1 || wall_distance(a, (b - a) * (1 / d), d) >= d - 1;
}

// the bolt has gone off (inside CExplosion): the tesla instead of the game's chain?
void tesla_hook(std::array<u32, 16>& r) {
    r[3] += 0x98;  // the replaced "adds r3, #0x98"
    if (!tesla_on || !ww_gun) return;
    u32 tr = g::rd32(r[4] + EXPL_TRANSFORM);
    if (!valid(tr)) return;
    call(FN_TRANSFORM_UPDATE, {tr});
    Start s{rdv(tr + TR_POS), g::rdf(r[4] + EXPL_RADIUS), ww_gun};
    u32 refs[32];
    int n = query(s.pos, s.radius + BODY, refs, 32, 0);
    for (int k = 0; k < n; k++)
        if (valid(refs[k]) && has(TABLE_ROMERO, refs[k])) return;  // George: the game's chain
    starts.push_back(s);
    r[15] = at(CHAIN_SKIP);
}

void tesla_build(const Start& s, u64 now) {
    struct C { u32 ref, id; V3 pos, head; bool hit; };
    std::vector<C> c;
    u32 refs[64];
    int n = query(s.pos, REACH + s.radius + BODY, refs, 64, 0);
    for (int k = 0; k < n; k++) {
        u32 ref = refs[k];
        if (!valid(ref)) continue;
        u32 id = g::rd32(ref + 4);
        if (!(has(TABLE_ZOMBIE, ref) || has(TABLE_HELLHOUND, ref) || has(TABLE_MONKEY, ref))) continue;
        if (health(ref) < 0.5f || std::find(doomed.begin(), doomed.end(), id) != doomed.end()) continue;
        V3 p = position(ref, {});
        c.push_back({ref, id, p, p + V3{0, HEAD, 0}, false});
    }
    // arc 1: the one the bolt hurts (its explosion's reach, from the feet to the top of the head), nearest first
    int first = -1;
    float best = s.radius + BODY;
    for (int i = 0; i < static_cast<int>(c.size()); i++) {
        V3 q = c[i].pos + V3{0, std::clamp(s.pos.y - c[i].pos.y, 0.0f, HEAD + 40), 0};
        float d = len(s.pos - q);
        if (d <= best) best = d, first = i;
    }
    if (first < 0) return;
    // get_array_of_closest(the first zombie's head)
    std::vector<int> order(c.size());
    for (size_t i = 0; i < c.size(); i++) order[i] = static_cast<int>(i);
    V3 o = c[first].head;
    std::sort(order.begin(), order.end(), [&](int a, int b) { return len(c[a].head - o) < len(c[b].head - o); });

    int hits = 1, visits = 0;
    auto schedule = [&](int i, int arc, int src, u64 t) {
        u64 at = t + (arc > 1 ? static_cast<u64>((0.2f + 0.4f * rnd01()) * arc * 1000) * MS : 0);
        Zap z{at, false, c[i].ref, c[i].id, s.gun, c[i].pos, 0, 0, s.pos};
        if (src >= 0) z.src = c[src].ref, z.src_id = c[src].id, z.src_pos = c[src].pos;
        if (src >= 0 && len(c[i].pos - c[src].pos) >= MIN_FX) {
            zaps.push_back(z);
            z.at += static_cast<u64>(ARC_MS) * MS;
        }
        z.kill = true;
        zaps.push_back(z);
        doomed.push_back(c[i].id);
    };
    // tesla_arc_damage (recursive, depth first)
    auto visit = [&](auto& self, int i, int arc, int src) -> void {
        u64 t = now + static_cast<u64>(visits++ * FRAME_MS) * MS;
        float radius = RADIUS_START - RADIUS_DECAY * arc;
        std::vector<int> found;
        for (int j : order)
            if (!c[j].hit && len(c[j].head - c[i].head) <= radius && in_sight(c[i].head, c[j].head))
                c[j].hit = true, found.push_back(j);
        schedule(i, arc, src, t);
        for (int j : found) {
            if (arc + 1 >= MAX_ARCS || hits >= MAX_KILLS) {
                c[j].hit = false;
                continue;
            }
            hits++;
            self(self, j, arc + 1, i);
        }
    };
    c[first].hit = true;
    visit(visit, first, 1, -1);
    LOGI("wunderwaffe: chain of %d (%d in reach)", hits, static_cast<int>(c.size()));
}

// an arc: the chain's trail from the zombie before to this one, landing in ARC_MS
void tesla_arc(const Zap& z) {
    V3 a = position(still(z.src, z.src_id), z.src_pos) + V3{0, TRAIL, 0};
    V3 b = position(still(z.ref, z.id), z.pos) + V3{0, TRAIL, 0};
    float d = len(b - a);
    if (d < 1) return;
    u32 from = tb, to = tb + 16, mat = tb + 32, vel = tb + 80;
    wrv(from, a), wrv(to, b);
    call(FN_LOOK_AT, {mat, from, to, g::rd32(at(GOT_UP))});
    g::wrf(mat + 36, a.x), g::wrf(mat + 40, a.y), g::wrf(mat + 44, a.z);
    wrv(vel, norm(rdv(mat + 24)) * (d / (ARC_MS / 1000)));
    u32 ids = call(FN_PROJ_IDS);
    if (!valid(ids)) return;
    u32 k = g::rd32(ids + 0xd8);
    g::wr32(ids + 0xd8, k + 1);
    u32 pw = call(FN_BY_ID, {HASH_LAUNCHER});
    u32 launcher = valid(pw) ? call(FN_LAUNCHER_OF, {pw}) : 0;
    if (valid(launcher)) call(FN_FIRE, {launcher, HASH_TRAIL, mat, vel, 0xffffffffu, g::rd32(ids + 0xd4) + k});
}

// a death (tesla_do_damage): false if it was not dealt
bool tesla_kill(const Zap& z) {
    doomed.erase(std::remove(doomed.begin(), doomed.end(), z.id), doomed.end());
    u32 ref = still(z.ref, z.id);
    float hp = ref ? health(ref) : 0;
    if (hp < 0.5f) return false;  // "guy died on us"
    u32 gun = z.gun == ww_cur || z.gun == ww_hol ? z.gun : ww_cur;  // (a gun since put away: the one in hand)
    if (!valid(gun)) return false;
    V3 p = position(ref, z.pos) + V3{0, TRAIL, 0};
    V3 dir = norm(p - (z.src_pos + V3{0, TRAIL, 0}));
    u32 pos = buf + 224, nrm = buf + 240;
    wrv(pos, p), wrv(nrm, len(dir) > 0.5f ? dir * -1.0f : V3{0, 1, 0});
    doubletap_force_damage(static_cast<u32>(hp) + 666);
    call(FN_HIT, {gun, ref, 0, pos, nrm, 1, 0});
    doubletap_force_damage(0);
    return true;
}

void tesla_update(u32 ctl) {
    if (!tesla_on) return;
    u64 now = now_ns();
    if (ww_seen && now - ww_seen > AWAY)  // paused (or another map: kept references are checked before use)
        for (Zap& z : zaps) z.at += now - ww_seen;
    ww_seen = now;
    u32 wm = g::rd32(ctl + CTL_WEAPONS);
    ww_cur = valid(wm) ? g::rd32(wm + WM_SLOTS) : 0, ww_hol = valid(wm) ? g::rd32(wm + WM_SLOTS + 4) : 0;
    ww_gun = is_wonder(ww_cur) ? ww_cur : 0;
    for (const Start& s : starts) tesla_build(s, now);
    starts.clear();
    if (now - choke_from >= static_cast<u64>(2 * FRAME_MS) * MS) choke_from = now, choked = 0;
    for (size_t i = 0; i < zaps.size();) {
        if (zaps[i].at > now || (zaps[i].kill && choked > CHOKE)) {
            i++;
            continue;
        }
        Zap z = zaps[i];
        zaps.erase(zaps.begin() + static_cast<std::ptrdiff_t>(i));
        if (!z.kill) tesla_arc(z);
        else if (tesla_kill(z)) choked++;
    }
    if (zaps.empty()) doomed.clear();
}

void tesla_init() {
    char v[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.boz64.tesla", v) > 0 && v[0] == '0') {
        LOGI("wunderwaffe: Black Ops' chain is off (debug.boz64.tesla=0)");
        return;
    }
    for (const Check& c : TESLA_CHECKS) {
        if (g::rd32(at(c.fn & ~1u)) != c.word) {
            LOGW("wunderwaffe: unexpected code at 0x%08x; the game's own chain stays", c.fn & ~1u);
            return;
        }
    }
    if (g::rd16(at(CHAIN_HOOK_AT)) != CHAIN_HOOK_ORIG) {
        LOGW("wunderwaffe: unexpected code at 0x%08x; the game's own chain stays", CHAIN_HOOK_AT);
        return;
    }
    if (!doubletap_force_damage(0)) {
        LOGW("wunderwaffe: the bullet hit hook (doubletap.cpp) is off; the game's own chain stays");
        return;
    }
    tb = g::data_alloc(96, 16);
    if (!tb) return;
    zap_seed = static_cast<u32>(now_ns()) | 1;
    g::wr16(at(CHAIN_HOOK_AT), static_cast<u16>(0xdf00 | HOOK_CHAIN));  // Thumb svc #imm8
    invalidate_code(at(CHAIN_HOOK_AT), 2);
    tesla_on = true;
    LOGI("wunderwaffe: Black Ops' chain is on (%d arcs, %d kills, %.0f cm less %.0f cm an arc)", MAX_ARCS, MAX_KILLS,
         RADIUS_START, RADIUS_DECAY);
}

// ---- the sound (game thread): World at War's fire state plays the ignition and the flame as the trigger is pulled
// and stops the flame as it is let go; tools/wawguns.py gives the gun no sound per shot (FLAME_SOUNDS) ----
u32 snd_ignite = 0, snd_flame = 0;  // Black Ops' looping-sound helpers, in guest memory
u32 hash_ignite = 0, hash_flame = 0;
bool sounding = false;

bool sound_live(u32 s) {
    u32 inst = g::rd32(s + 4);
    return valid(inst) && (g::rd16(inst + INST_FLAGS) & INST_IN_USE) && g::rd32(inst + INST_USER) == s;
}

void sound_start(u32 s, u32 hash) {
    if (sound_live(s)) return;
    g::wr32(s + 4, 0);
    call(FN_LOOP_SPEC, {s, hash});  // looked up each time: the map's groups may have been reloaded
    call(FN_LOOP_SET, {s, 1, 0});
}

void sound_stop(u32 s) {
    if (sound_live(s) && g::rd32(s + 8)) call(FN_LOOP_SET, {s, 0, 0});
    g::wr32(s + 4, 0);
}

void sound_update(bool fire) {
    if (fire) {
        if (!sounding) sound_start(snd_ignite, hash_ignite);  // once a spurt (not over one still sounding)
        sound_start(snd_flame, hash_flame);                    // (and again should its 6 s run out)
    } else if (sounding) {
        sound_stop(snd_flame);
    }
    sounding = fire;
}

// ---- what the render thread draws ----
struct SnapSphere {
    V3 pos, dir, start;
    float limit, age;
};
struct Snap {
    u64 t = 0;
    int n = 0;
    SnapSphere s[NUM_SPHERES];  // newest first
    bool firing = false;
    float wall = 280;
};
std::mutex snap_mu;
Snap snap;

void publish(u64 now, bool fire) {
    Snap s;
    s.t = now;
    s.firing = fire;
    s.wall = fl.wall;
    for (int k = 0; k < NUM_SPHERES; k++) {
        const Sphere& p = fl.s[(fl.head + k) % NUM_SPHERES];
        if (p.on) s.s[s.n++] = {p.pos, p.dir, p.start, p.limit, p.age};
    }
    std::lock_guard<std::mutex> lk(snap_mu);
    snap = s;
}

void update_gun(Heat& s, bool current, float dt, u64 now) {
    u32 gun = s.gun;
    s32 size = static_cast<s32>(g::rd32(gun + GUN_CLIP_SIZE));
    if (size <= 0 || size > 1000) size = 100;
    bool fire = current && fl.gun == gun && firing(now);
    u32 im = g::rd32(at(INPUT_MGR));
    bool held = current && valid(im) && (g::rd8(im + IM_FIRE) || g::rd8(im + IM_FIRE_HOLD));
    float rate = 1.0f / (g::rd32(gun + GUN_HASH) == gun_upgraded ? FIRE_TIME_UPGRADED : FIRE_TIME);
    s.heat += (fire || (s.over && held) ? dt : -dt) * rate;
    s.heat = std::clamp(s.heat, 0.0f, 1.0f);
    if (s.heat >= 1.0f && !s.over) s.over = true, LOGI("flamethrower: overheated");
    if (s.over && s.heat <= COOLED) s.over = false;
    // the clip: full, or empty while overheated; never a reserve (so never a reload)
    s32 want = s.over ? 0 : size;
    if (static_cast<s32>(g::rd32(gun + GUN_CLIP)) != want) g::wr32(gun + GUN_CLIP, static_cast<u32>(want));
    if (g::rd32(gun + GUN_AMMO)) g::wr32(gun + GUN_AMMO, 0);
}

void flame_update(u32 wm, u32 cur, float dt, u64 now) {
    u32 gun = is_flame(cur) ? cur : 0;
    if (gun != fl.gun) {  // another gun in hand: no more spheres (those out burn on)
        fl.gun = gun;
        fl.last_shot = 0;
    }
    float dt_ms = dt * 1000;
    move_spheres(dt_ms);
    const Heat* h = nullptr;
    for (const Heat& s : heats)
        if (s.gun && s.gun == gun) h = &s;
    bool fire = gun && firing(now) && !(h && h->over);
    if (fire) {
        fl.emit_in -= dt_ms;
        if (fl.emit_in <= 0) {
            emit(wm);
            fl.emit_in = fl.emit_in < -emit_period ? emit_period : fl.emit_in + emit_period;
        }
    } else {
        fl.emit_in = 0;
    }
    bool any = false;
    for (const Sphere& s : fl.s) any |= s.on;
    fl.damage_in -= dt_ms;
    if (fl.damage_in <= 0) {
        fl.damage_in = DAMAGE_PERIOD;
        u32 dmg_gun = gun ? gun : 0;
        if (any && dmg_gun) damage(dmg_gun);
    }
    sound_update(fire);
    publish(now, fire);
}

// ---- the HUD's ammo counter (game thread) ----
constexpr const char* AMMO_NAMES[] = {"ammo_images", "ammo_labels", "Low_Ammo_Label", "No_Ammo_Label"};
constexpr int AMMO_COUNT = 4;
constexpr int AMMO_RESTORE = 2;  // the first two are shown again afterwards (the game shows the labels itself)
constexpr u64 REFIND = 1000 * MS;
u32 ammo_names[AMMO_COUNT] = {};
u32 ammo_els[AMMO_COUNT] = {};
u32 ammo_hidden[AMMO_COUNT] = {};
u64 ammo_found = 0;

std::atomic<s32> gauge_rect[4];
std::atomic<float> gauge_heat{0};
std::atomic<bool> gauge_over{false};
std::atomic<u64> gauge_until{0};

u32 find_element(int i) {
    u32 mgr = g::rd32(at(UI_MANAGER));
    if (!valid(mgr)) return 0;
    if (!ammo_names[i]) ammo_names[i] = g::data_str(AMMO_NAMES[i]);
    u32 el = call(FN_FIND_ELEMENT, {mgr, ammo_names[i], 0});
    return valid(el) ? el : 0;
}

bool visible(u32 el) {
    return (g::rd16(el + EL_FLAGS_CACHED) & 1) ? (g::rd16(el + EL_FLAGS) & ELF_VISIBLE) != 0
                                               : (call(FN_GET_FLAG, {el, ELF_VISIBLE}) & 0xff) != 0;
}

constexpr u32 EL_CHILDREN = 0x1c, EL_CHILD_COUNT = 0x20;  // CIwUIElement: its children (array, count)

void add_box(s32 box[4], bool& have, const s32 r[4]) {
    if (!have) {
        std::copy(r, r + 4, box);
        have = true;
        return;
    }
    s32 x0 = std::min(box[0], r[0]), y0 = std::min(box[1], r[1]);
    s32 x1 = std::max(box[0] + box[2], r[0] + r[2]), y1 = std::max(box[1] + box[3], r[1] + r[3]);
    box[0] = x0, box[1] = y0, box[2] = x1 - x0, box[3] = y1 - y0;
}

// the element's rectangle on the screen (x, y, w, h; pixels from the top left); a container without a size of its
// own covers its children
bool screen_rect(u32 el, s32 out[4], int depth = 0) {
    s32 w = static_cast<s32>(g::rd32(el + EL_SIZE)), h = static_cast<s32>(g::rd32(el + EL_SIZE + 4));
    if (w > 0 && h > 0 && w < 10000 && h < 10000) {
        s32 x = static_cast<s32>(g::rd32(el + EL_POS)), y = static_cast<s32>(g::rd32(el + EL_POS + 4));
        u32 p = g::rd32(el + EL_PARENT);
        for (int up = 0; valid(p) && up < 32; up++) {
            x += static_cast<s32>(g::rd32(p + EL_POS));
            y += static_cast<s32>(g::rd32(p + EL_POS + 4));
            p = g::rd32(p + EL_PARENT);
        }
        out[0] = x, out[1] = y, out[2] = w, out[3] = h;
        return true;
    }
    if (depth >= 3) return false;
    u32 kids = g::rd32(el + EL_CHILDREN), n = g::rd32(el + EL_CHILD_COUNT);
    if (!valid(kids) || n > 64) return false;
    bool have = false;
    for (u32 i = 0; i < n; i++) {
        u32 k = g::rd32(kids + 4 * i);
        s32 r[4];
        if (valid(k) && screen_rect(k, r, depth + 1)) add_box(out, have, r);
    }
    return have;
}

// the ammo counter itself and everything above it are shown (not the pause menu or a cut scene)
bool counter_shown(u32 el) {
    for (u32 p = g::rd32(el + EL_PARENT); valid(p); p = g::rd32(p + EL_PARENT))
        if (!visible(p)) return false;
    return true;
}

void hud_update(const Heat* s, u64 now, bool fresh) {
    if (fresh) {  // after a gap the HUD may be another one: look again, and forget hidden ones it no longer has
        for (int i = 0; i < AMMO_COUNT; i++) {
            ammo_els[i] = find_element(i);
            if (ammo_hidden[i] != ammo_els[i]) ammo_hidden[i] = 0;  // (compared, never touched: it may be gone)
        }
        ammo_found = now;
    }
    if (!s) {
        for (int i = 0; i < AMMO_RESTORE; i++)
            if (ammo_hidden[i] && find_element(i) == ammo_hidden[i]) call(FN_SET_VISIBLE, {ammo_hidden[i], 1});
        for (int i = 0; i < AMMO_COUNT; i++) ammo_hidden[i] = 0;
        gauge_until = 0;
        return;
    }
    if (!ammo_found || now - ammo_found > REFIND) {
        for (int i = 0; i < AMMO_COUNT; i++) ammo_els[i] = find_element(i);
        ammo_found = now;
    }
    s32 box[4] = {0, 0, 0, 0};
    bool have = false;
    for (int i = 0; i < AMMO_RESTORE; i++) {
        s32 r[4];
        if (ammo_els[i] && screen_rect(ammo_els[i], r)) add_box(box, have, r);
    }
    bool shown = have && counter_shown(ammo_els[0] ? ammo_els[0] : ammo_els[1]);
    for (int i = 0; i < AMMO_COUNT; i++) {
        u32 el = ammo_els[i];
        if (!el || !visible(el)) continue;
        call(FN_SET_VISIBLE, {el, 0});
        if (ammo_hidden[i] != el && i < AMMO_RESTORE) LOGI("flamethrower: hid the HUD's %s", AMMO_NAMES[i]);
        ammo_hidden[i] = el;
    }
    if (!shown) {
        gauge_until = 0;
        return;
    }
    for (int i = 0; i < 4; i++) gauge_rect[i] = box[i];
    gauge_heat = s->heat;
    gauge_over = s->over;
    gauge_until = now + 250 * MS;
}

// ---- GL (render thread) ----
bool gl_failed = false, gl_ready = false;
using BindVaoFn = void (*)(GLuint);
BindVaoFn bind_vao = nullptr;
GLuint prog_gauge = 0, prog_fins = 0, prog_parts = 0, noise_tex = 0;
GLint u_color = -1;
struct ProgU { GLint pmv, right, up, noise, time, gain, mode; } uf{}, up_{};
constexpr GLenum NOISE_UNIT = GL_TEXTURE7;  // a unit the game leaves alone

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        LOGW("flamethrower: a shader did not compile: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint link(const char* vs_src, const char* fs_src, std::initializer_list<const char*> attrs) {
    GLuint vs = compile(GL_VERTEX_SHADER, vs_src), fs = compile(GL_FRAGMENT_SHADER, fs_src);
    if (!vs || !fs) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    GLuint i = 0;
    for (const char* a : attrs) glBindAttribLocation(p, i++, a);
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        LOGW("flamethrower: a program did not link");
        glDeleteProgram(p);
        return 0;
    }
    return p;
}

#define FS_HEAD                          \
    "#ifdef GL_FRAGMENT_PRECISION_HIGH\n" \
    "precision highp float;\n"           \
    "#else\n"                            \
    "precision mediump float;\n"         \
    "#endif\n"
// World at War's flame colours: white-yellow at the nozzle, orange, red, then dark at the end
#define RAMP                                                                     \
    "vec3 ramp(float a) {\n"                                                     \
    "  vec3 c = mix(vec3(1.0, 0.93, 0.68), vec3(1.0, 0.56, 0.14), smoothstep(0.0, 0.3, a));\n" \
    "  c = mix(c, vec3(0.86, 0.21, 0.04), smoothstep(0.3, 0.72, a));\n"          \
    "  return mix(c, vec3(0.32, 0.07, 0.02), smoothstep(0.72, 1.0, a));\n"       \
    "}\n"

const char* GAUGE_VS = "attribute vec2 p;\nvoid main() { gl_Position = vec4(p, 0.0, 1.0); }\n";
const char* GAUGE_FS = "precision mediump float;\nuniform vec4 c;\nvoid main() { gl_FragColor = c; }\n";

const char* FINS_VS =
    "attribute vec3 a_pos;\n"
    "attribute vec4 a_q;\n"  // distance along the flame (cm), across the fin (0 axis, 1 edge), age (0-1), fin
    "uniform mat4 u_pmv;\n"
    "varying vec4 v_q;\n"
    "void main() { gl_Position = u_pmv * vec4(a_pos, 1.0); v_q = a_q; }\n";
const char* FINS_FS = FS_HEAD RAMP
    "uniform sampler2D u_noise;\n"
    "uniform float u_time;\n"
    "uniform float u_gain;\n"
    "varying vec4 v_q;\n"
    "void main() {\n"
    "  float u = v_q.x * 0.011;\n"
    "  float n1 = texture2D(u_noise, vec2(u - u_time * 2.1, v_q.y * 0.45 + v_q.w * 0.137)).r;\n"
    "  float n2 = texture2D(u_noise, vec2(u * 2.3 - u_time * 3.3, v_q.y * 0.9 + v_q.w * 0.291 + 0.5)).r;\n"
    "  float n = n1 * 0.6 + n2 * 0.4;\n"
    "  float e = clamp(1.0 - v_q.y - (n - 0.5) * 0.9, 0.0, 1.0);\n"
    "  float fade = smoothstep(0.0, 0.06, v_q.z) * (1.0 - smoothstep(0.62, 1.0, v_q.z));\n"
    "  float a = e * e * (0.35 + n) * fade * u_gain;\n"
    "  gl_FragColor = vec4(ramp(v_q.z * 0.9) * a, 1.0);\n"
    "}\n";

const char* PARTS_VS =
    "attribute vec3 a_pos;\n"   // the particle's centre
    "attribute vec4 a_q;\n"     // corner x, corner y, radius, age (0-1)
    "attribute float a_seed;\n"
    "uniform mat4 u_pmv;\n"
    "uniform vec3 u_right;\n"
    "uniform vec3 u_up;\n"
    "varying vec2 v_c;\n"
    "varying float v_age;\n"
    "varying float v_seed;\n"
    "void main() {\n"
    "  float ang = a_seed * 6.2831853 + a_q.w * (a_seed - 0.5) * 4.0;\n"
    "  float cs = cos(ang), sn = sin(ang);\n"
    "  vec2 c = vec2(cs * a_q.x - sn * a_q.y, sn * a_q.x + cs * a_q.y);\n"
    "  gl_Position = u_pmv * vec4(a_pos + (u_right * c.x + u_up * c.y) * a_q.z, 1.0);\n"
    "  v_c = a_q.xy; v_age = a_q.w; v_seed = a_seed;\n"
    "}\n";
const char* PARTS_FS = FS_HEAD RAMP
    "uniform sampler2D u_noise;\n"
    "uniform float u_time;\n"
    "uniform float u_gain;\n"
    "uniform float u_mode;\n"  // 0 flame (added), 1 smoke (blended)
    "varying vec2 v_c;\n"
    "varying float v_age;\n"
    "varying float v_seed;\n"
    "void main() {\n"
    "  float r = length(v_c);\n"
    "  vec2 b = v_c * 0.3 + vec2(v_seed * 7.13, v_seed * 3.71);\n"
    "  float n1 = texture2D(u_noise, b + vec2(0.0, u_time * 0.9)).r;\n"
    "  float n2 = texture2D(u_noise, b * 2.1 - vec2(u_time * 0.4, u_time * 1.4)).r;\n"
    "  float n = n1 * 0.65 + n2 * 0.35;\n"
    "  float body = clamp(1.0 - r - (n - 0.5) * 0.9, 0.0, 1.0);\n"
    "  if (u_mode < 0.5) {\n"
    "    float fade = smoothstep(0.0, 0.08, v_age) * (1.0 - smoothstep(0.55, 1.0, v_age));\n"
    "    float a = body * body * (0.5 + n) * fade * u_gain;\n"
    "    gl_FragColor = vec4(ramp(v_age) * a, 1.0);\n"
    "  } else {\n"
    "    float fade = smoothstep(0.0, 0.2, v_age) * (1.0 - smoothstep(0.4, 1.0, v_age));\n"
    "    gl_FragColor = vec4(vec3(0.10, 0.09, 0.08), body * body * fade * u_gain);\n"
    "  }\n"
    "}\n";

// a tiling fire noise (value noise, three octaves)
void make_noise() {
    constexpr int N = 64;
    static u8 px[N * N];
    u32 seed = 0x2545f491;
    auto rnd = [&]() {
        seed ^= seed << 13, seed ^= seed >> 17, seed ^= seed << 5;
        return (seed & 0xffff) / 65535.0f;
    };
    float lat[3][16][16];
    for (auto& o : lat)
        for (auto& row : o)
            for (float& v : row) v = rnd();
    for (int y = 0; y < N; y++)
        for (int x = 0; x < N; x++) {
            float sum = 0, amp = 0.55f, tot = 0;
            for (int o = 0; o < 3; o++) {
                int cells = 4 << o;  // 4, 8, 16 cells across: tiles
                float fx = x * cells / float(N), fy = y * cells / float(N);
                int x0 = int(fx), y0 = int(fy);
                float tx = fx - x0, ty = fy - y0;
                tx = tx * tx * (3 - 2 * tx), ty = ty * ty * (3 - 2 * ty);
                auto L = [&](int i, int j) { return lat[o][j % cells][i % cells]; };
                float a = L(x0, y0) + (L(x0 + 1, y0) - L(x0, y0)) * tx;
                float b = L(x0, y0 + 1) + (L(x0 + 1, y0 + 1) - L(x0, y0 + 1)) * tx;
                sum += (a + (b - a) * ty) * amp;
                tot += amp;
                amp *= 0.5f;
            }
            px[y * N + x] = static_cast<u8>(std::clamp(sum / tot, 0.0f, 1.0f) * 255);
        }
    GLint active = 0, old = 0, align = 4;
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    glActiveTexture(NOISE_UNIT);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &align);
    glGenTextures(1, &noise_tex);
    glBindTexture(GL_TEXTURE_2D, noise_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, N, N, 0, GL_LUMINANCE, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glPixelStorei(GL_UNPACK_ALIGNMENT, align);
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(old));
    glActiveTexture(static_cast<GLenum>(active));
}

bool gl_init() {
    if (gl_ready) return true;
    if (gl_failed) return false;
    gl_failed = true;
    prog_gauge = link(GAUGE_VS, GAUGE_FS, {"p"});
    prog_fins = link(FINS_VS, FINS_FS, {"a_pos", "a_q"});
    prog_parts = link(PARTS_VS, PARTS_FS, {"a_pos", "a_q", "a_seed"});
    if (!prog_gauge || !prog_fins || !prog_parts) return false;
    u_color = glGetUniformLocation(prog_gauge, "c");
    auto get = [](GLuint p, ProgU& u) {
        u.pmv = glGetUniformLocation(p, "u_pmv");
        u.right = glGetUniformLocation(p, "u_right");
        u.up = glGetUniformLocation(p, "u_up");
        u.noise = glGetUniformLocation(p, "u_noise");
        u.time = glGetUniformLocation(p, "u_time");
        u.gain = glGetUniformLocation(p, "u_gain");
        u.mode = glGetUniformLocation(p, "u_mode");
    };
    get(prog_fins, uf);
    get(prog_parts, up_);
    make_noise();
    const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
    bool es3 = ver && std::strncmp(ver, "OpenGL ES 3", 11) == 0;
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (es3 || (ext && std::strstr(ext, "GL_OES_vertex_array_object")))
        bind_vao = reinterpret_cast<BindVaoFn>(eglGetProcAddress(es3 ? "glBindVertexArray" : "glBindVertexArrayOES"));
    gl_failed = false;
    gl_ready = true;
    return true;
}

// everything the drawing touches, put back afterwards (the game's renderer caches GL state)
struct SavedGL {
    static constexpr int ATTRS = 3;
    GLint prog = 0, abuf = 0, vao = 0, active = 0, tex7 = 0, depth_func = GL_LESS;
    GLint b_src_rgb = GL_ONE, b_dst_rgb = GL_ZERO, b_src_a = GL_ONE, b_dst_a = GL_ZERO, b_eq_rgb = GL_FUNC_ADD,
          b_eq_a = GL_FUNC_ADD;
    struct Attr { GLint en = 0, size = 4, type = GL_FLOAT, normed = 0, stride = 0, buf = 0; void* ptr = nullptr; } a[ATTRS];
    GLboolean cmask[4] = {1, 1, 1, 1}, dmask = 1;
    static constexpr GLenum CAPS[5] = {GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST};
    GLboolean cap_on[5] = {};
    bool textured = false;

    void save(bool tex) {
        textured = tex;
        for (int i = 0; i < 5; i++) cap_on[i] = glIsEnabled(CAPS[i]);
        glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
        if (bind_vao) {
            glGetIntegerv(GL_VERTEX_ARRAY_BINDING_OES, &vao);
            if (vao) bind_vao(0);
        }
        for (int i = 0; i < ATTRS; i++) {
            Attr& t = a[i];
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &t.en);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_SIZE, &t.size);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_TYPE, &t.type);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &t.normed);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &t.stride);
            glGetVertexAttribiv(i, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &t.buf);
            glGetVertexAttribPointerv(i, GL_VERTEX_ATTRIB_ARRAY_POINTER, &t.ptr);
        }
        glGetIntegerv(GL_BLEND_SRC_RGB, &b_src_rgb);
        glGetIntegerv(GL_BLEND_DST_RGB, &b_dst_rgb);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &b_src_a);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &b_dst_a);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &b_eq_rgb);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &b_eq_a);
        glGetIntegerv(GL_DEPTH_FUNC, &depth_func);
        glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
        if (textured) {
            glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
            glActiveTexture(NOISE_UNIT);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex7);
        }
        glBindBuffer(GL_ARRAY_BUFFER, 0);
    }
    void restore() {
        if (textured) {
            glActiveTexture(NOISE_UNIT);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(tex7));
            glActiveTexture(static_cast<GLenum>(active));
        }
        for (int i = 0; i < ATTRS; i++) {
            const Attr& t = a[i];
            glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(t.buf));
            glVertexAttribPointer(i, t.size, static_cast<GLenum>(t.type), t.normed ? GL_TRUE : GL_FALSE, t.stride, t.ptr);
            if (t.en) glEnableVertexAttribArray(i);
            else glDisableVertexAttribArray(i);
        }
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(abuf));
        glUseProgram(static_cast<GLuint>(prog));
        if (vao) bind_vao(static_cast<GLuint>(vao));
        glBlendEquationSeparate(static_cast<GLenum>(b_eq_rgb), static_cast<GLenum>(b_eq_a));
        glBlendFuncSeparate(static_cast<GLenum>(b_src_rgb), static_cast<GLenum>(b_dst_rgb), static_cast<GLenum>(b_src_a),
                            static_cast<GLenum>(b_dst_a));
        glDepthFunc(static_cast<GLenum>(depth_func));
        for (int i = 0; i < 5; i++)
            if (cap_on[i]) glEnable(CAPS[i]);
            else glDisable(CAPS[i]);
        glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
        glDepthMask(dmask);
    }
};

// ---- the flame's drawing (render thread) ----
struct Particle {
    V3 p, v;
    float age, life, r0, r1, seed;
    bool smoke;
};
std::vector<Particle> parts;
u32 rseed = 0x9e3779b9;
float frand(float a, float b) {
    rseed ^= rseed << 13, rseed ^= rseed >> 17, rseed ^= rseed << 5;
    return a + (b - a) * ((rseed & 0xffffff) / float(0xffffff));
}
float flame_acc = 0, smoke_acc = 0;
u64 last_draw = 0;
constexpr float FLAME_RATE = 170, SMOKE_RATE = 28;  // particles per second while firing
constexpr size_t MAX_PARTS = 480;

float radius_at(float age_ms) {
    const int n = sizeof RADIUS_KEYS / sizeof RADIUS_KEYS[0];
    if (age_ms <= RADIUS_KEYS[0][0]) return RADIUS_KEYS[0][1];
    for (int i = 1; i < n; i++)
        if (age_ms < RADIUS_KEYS[i][0]) {
            float t = (age_ms - RADIUS_KEYS[i - 1][0]) / (RADIUS_KEYS[i][0] - RADIUS_KEYS[i - 1][0]);
            return RADIUS_KEYS[i - 1][1] + (RADIUS_KEYS[i][1] - RADIUS_KEYS[i - 1][1]) * t;
        }
    return RADIUS_KEYS[n - 1][1];
}

struct Cam {
    V3 pos, fwd, right, up;
};
// the camera of a projection x view matrix (GL column-major): right, up and forward are its rows 0, 1 and 3 made
// perpendicular to the view direction; its position is where rows 0, 1 and 3 are all 0
bool camera_of(const float* m, Cam& c) {
    auto row = [&](int i) { return V3{m[i], m[4 + i], m[8 + i]}; };
    V3 r0 = row(0), r1 = row(1), r3 = row(3);
    float w0 = m[12], w1 = m[13], w3 = m[15];
    V3 f = norm(r3);
    if (len(f) < 0.5f) return false;
    c.fwd = f;
    c.right = norm(r0 - f * dot(r0, f));
    c.up = norm(r1 - f * dot(r1, f));
    V3 a = r0, b = r1, d = r3;
    float det = dot(a, cross(b, d));
    if (std::fabs(det) < 1e-9f) return false;
    // solve a.p = -w0, b.p = -w1, d.p = -w3
    V3 p = (cross(b, d) * -w0 + cross(d, a) * -w1 + cross(a, b) * -w3) * (1 / det);
    c.pos = p;
    return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) && len(c.right) > 0.5f && len(c.up) > 0.5f;
}

struct ChainPt {
    V3 p;
    float r, age;  // radius (cm), age along its life (0-1)
};

void simulate_particles(const Cam& cam, V3 nozzle, bool spawn, float wall, float dt) {
    for (Particle& q : parts) {
        q.age += dt;
        q.p = q.p + q.v * dt;
        if (q.smoke) q.v = q.v + cam.up * (40 * dt);  // drifting up
    }
    parts.erase(std::remove_if(parts.begin(), parts.end(), [](const Particle& q) { return q.age >= q.life; }),
                parts.end());
    if (!spawn) {
        flame_acc = smoke_acc = 0;
        return;
    }
    // World at War scales its emitters' speed by how far the wall ahead is (0x1dedd8)
    float ratio = std::clamp(wall / max_range, 0.05f, 1.0f);
    V3 aim = cam.pos + cam.fwd * (max_range * 0.9f);
    flame_acc += FLAME_RATE * dt;
    while (flame_acc >= 1 && parts.size() < MAX_PARTS) {
        flame_acc -= 1;
        Particle q{};
        q.p = nozzle + cam.fwd * frand(2, 14) + cam.right * frand(-1.5f, 1.5f) + cam.up * frand(-1.5f, 1.5f);
        V3 d = norm(aim - q.p);
        // World at War: 10-15 m/s forward, +-0.5 m/s across
        q.v = (d * frand(1000, 1500) + cam.right * frand(-50, 50) + cam.up * frand(-50, 50)) * ratio;
        q.life = (max_range / 1250.0f) * frand(0.85f, 1.1f);
        q.r0 = frand(2.0f, 3.5f) * size_scale;
        q.r1 = frand(24, 34) * size_scale;
        q.seed = frand(0, 1);
        parts.push_back(q);
    }
    smoke_acc += SMOKE_RATE * dt;
    while (smoke_acc >= 1 && parts.size() < MAX_PARTS) {
        smoke_acc -= 1;
        Particle q{};
        // World at War: from 50-60 cm ahead, up to 10 cm up; 1.5-2 m/s forward, 0.5-1 m/s up, +-0.3 m/s across
        float ahead = std::min(frand(50, 60), std::max(10.0f, wall));
        q.p = nozzle + cam.fwd * ahead + cam.up * frand(0, 10);
        q.v = (cam.fwd * frand(150, 200) + cam.up * frand(50, 100) + cam.right * frand(-30, 30)) * ratio;
        q.life = frand(0.7f, 0.95f);
        q.r0 = frand(6, 9) * size_scale;
        q.r1 = frand(32, 46) * size_scale;
        q.seed = frand(0, 1);
        q.smoke = true;
        parts.push_back(q);
    }
}

std::vector<float> verts;

void push_particle(const Particle& q) {
    float t = std::clamp(q.age / q.life, 0.0f, 1.0f);
    float r = q.r0 + (q.r1 - q.r0) * std::sqrt(t);
    static const float corner[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
    for (const auto& c : corner) {
        const float v[8] = {q.p.x, q.p.y, q.p.z, c[0], c[1], r, t, q.seed};
        verts.insert(verts.end(), v, v + 8);
    }
}

// World at War's fins: NUM_SIDES quads fanning out from the line through the chain
void push_fins(const std::vector<ChainPt>& ch, V3 ref_up, float spin) {
    size_t n = ch.size();
    std::vector<V3> A(n), B(n);
    std::vector<float> dist(n, 0);
    for (size_t i = 0; i < n; i++) {
        V3 t = norm(i + 1 < n ? ch[i + 1].p - ch[i].p : ch[i].p - ch[i - 1].p);
        V3 a = cross(t, ref_up);
        if (len(a) < 0.1f) a = cross(t, V3{1, 0, 0});
        A[i] = norm(a);
        B[i] = norm(cross(t, A[i]));
        if (i) dist[i] = dist[i - 1] + len(ch[i].p - ch[i - 1].p);
    }
    for (int k = 0; k < NUM_SIDES; k++) {
        float ang = spin + k * (6.2831853f / NUM_SIDES);
        float cs = std::cos(ang), sn = std::sin(ang);
        for (size_t i = 0; i + 1 < n; i++) {
            V3 e0 = ch[i].p + (A[i] * cs + B[i] * sn) * ch[i].r;
            V3 e1 = ch[i + 1].p + (A[i + 1] * cs + B[i + 1] * sn) * ch[i + 1].r;
            const float q[4][7] = {
                {ch[i].p.x, ch[i].p.y, ch[i].p.z, dist[i], 0, ch[i].age, float(k)},
                {e0.x, e0.y, e0.z, dist[i], 1, ch[i].age, float(k)},
                {e1.x, e1.y, e1.z, dist[i + 1], 1, ch[i + 1].age, float(k)},
                {ch[i + 1].p.x, ch[i + 1].p.y, ch[i + 1].p.z, dist[i + 1], 0, ch[i + 1].age, float(k)},
            };
            for (int idx : {0, 1, 2, 0, 2, 3}) verts.insert(verts.end(), q[idx], q[idx] + 7);
        }
    }
}

// a smooth curve through the chain (Catmull-Rom, 4 steps a link)
std::vector<ChainPt> smooth_chain(const std::vector<ChainPt>& c) {
    if (c.size() < 3) return c;
    std::vector<ChainPt> out;
    size_t n = c.size();
    for (size_t i = 0; i + 1 < n; i++) {
        const ChainPt& p0 = c[i ? i - 1 : 0];
        const ChainPt& p1 = c[i];
        const ChainPt& p2 = c[i + 1];
        const ChainPt& p3 = c[i + 2 < n ? i + 2 : n - 1];
        for (int s = 0; s < 4; s++) {
            float t = s / 4.0f, t2 = t * t, t3 = t2 * t;
            auto cr = [&](float a, float b, float cc, float d) {
                return 0.5f * (2 * b + (cc - a) * t + (2 * a - 5 * b + 4 * cc - d) * t2 + (3 * b - a - 3 * cc + d) * t3);
            };
            ChainPt q;
            q.p = {cr(p0.p.x, p1.p.x, p2.p.x, p3.p.x), cr(p0.p.y, p1.p.y, p2.p.y, p3.p.y),
                   cr(p0.p.z, p1.p.z, p2.p.z, p3.p.z)};
            q.r = p1.r + (p2.r - p1.r) * t;
            q.age = p1.age + (p2.age - p1.age) * t;
            out.push_back(q);
        }
    }
    out.push_back(c.back());
    return out;
}

void draw_flame(const float* pmv) {
    u64 now = now_ns();
    float dt = last_draw ? std::min(0.05f, (now - last_draw) / 1e9f) : 0.0f;
    last_draw = now;
    Cam cam;
    if (!camera_of(pmv, cam)) return;
    Snap s;
    {
        std::lock_guard<std::mutex> lk(snap_mu);
        s = snap;
    }
    bool live = s.t && now >= s.t && now - s.t < 300 * MS;
    V3 nozzle = cam.pos + cam.fwd * nozzle_ofs.x + cam.right * nozzle_ofs.y + cam.up * nozzle_ofs.z;
    V3 side = cam.right * nozzle_ofs.y + cam.up * nozzle_ofs.z;  // the nozzle's offset from the aim line
    simulate_particles(cam, nozzle, live && s.firing, s.wall, dt);

    // the chain: the nozzle while it fires, then the spheres moved on to now
    std::vector<ChainPt> chain;
    if (live) {
        float ahead = std::min(150.0f, (now - s.t) / 1e6f);
        if (s.firing) chain.push_back({nozzle, radius_at(0) * FIN_SCALE * size_scale, 0});
        for (int i = 0; i < s.n; i++) {
            const SnapSphere& p = s.s[i];
            V3 pos = p.pos + p.dir * (SPEED * ahead);
            float travel = len(pos - p.start);
            if (travel + GIRTH >= p.limit) break;
            // leaving the nozzle, it joins the aim line within a metre
            pos = pos + side * std::max(0.0f, 1.0f - travel / 100.0f);
            float age = std::clamp((travel + GIRTH) / max_range, 0.0f, 1.0f);
            chain.push_back({pos, radius_at(p.age + ahead) * FIN_SCALE * size_scale, age});
        }
    }
    if (parts.empty() && chain.size() < 2) return;
    if (!gl_init()) return;

    SavedGL st;
    st.save(true);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_FALSE);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glActiveTexture(NOISE_UNIT);
    glBindTexture(GL_TEXTURE_2D, noise_tex);
    float t = static_cast<float>(std::fmod(now / 1e9, 1000.0));

    // smoke first, blended, far to near
    std::vector<const Particle*> smoke, flame;
    for (const Particle& q : parts) (q.smoke ? smoke : flame).push_back(&q);
    std::sort(smoke.begin(), smoke.end(), [&](const Particle* a, const Particle* b) {
        return dot(a->p - cam.pos, cam.fwd) > dot(b->p - cam.pos, cam.fwd);
    });
    auto draw_parts = [&](const std::vector<const Particle*>& list, float mode, float gain) {
        if (list.empty()) return;
        verts.clear();
        for (const Particle* q : list) push_particle(*q);
        glUseProgram(prog_parts);
        glUniformMatrix4fv(up_.pmv, 1, GL_FALSE, pmv);
        glUniform3f(up_.right, cam.right.x, cam.right.y, cam.right.z);
        glUniform3f(up_.up, cam.up.x, cam.up.y, cam.up.z);
        glUniform1i(up_.noise, 7);
        glUniform1f(up_.time, t);
        glUniform1f(up_.gain, gain);
        glUniform1f(up_.mode, mode);
        const GLsizei stride = 8 * sizeof(float);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, verts.data());
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, verts.data() + 3);
        glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, stride, verts.data() + 7);
        for (int i = 0; i < 3; i++) glEnableVertexAttribArray(i);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / 8));
    };
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ZERO, GL_ONE);
    draw_parts(smoke, 1, 0.24f);

    // then the fire, added
    glBlendFuncSeparate(GL_ONE, GL_ONE, GL_ZERO, GL_ONE);
    if (chain.size() >= 2) {
        std::vector<ChainPt> curve = smooth_chain(chain);
        verts.clear();
        push_fins(curve, cam.up, t * 1.3f);
        glUseProgram(prog_fins);
        glUniformMatrix4fv(uf.pmv, 1, GL_FALSE, pmv);
        glUniform1i(uf.noise, 7);
        glUniform1f(uf.time, t);
        glUniform1f(uf.gain, 0.42f);
        const GLsizei stride = 7 * sizeof(float);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, stride, verts.data());
        glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, stride, verts.data() + 3);
        glEnableVertexAttribArray(0);
        glEnableVertexAttribArray(1);
        glDisableVertexAttribArray(2);
        glDrawArrays(GL_TRIANGLES, 0, static_cast<GLsizei>(verts.size() / 7));
    }
    draw_parts(flame, 0, 0.85f);
    st.restore();
}

void quad(float sw, float sh, float x, float y, float w, float h, float r, float g, float b, float a) {
    if (w <= 0 || h <= 0) return;
    float x0 = x / sw * 2 - 1, x1 = (x + w) / sw * 2 - 1;
    float y0 = 1 - y / sh * 2, y1 = 1 - (y + h) / sh * 2;
    const float v[8] = {x0, y1, x1, y1, x0, y0, x1, y0};
    glUniform4f(u_color, r, g, b, a);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, v);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

// heat colour: yellow when cool, orange, then red
void heat_colour(float t, float c[3]) {
    const float a[3] = {1.0f, 0.86f, 0.25f}, m[3] = {1.0f, 0.52f, 0.10f}, z[3] = {0.95f, 0.13f, 0.05f};
    float u = std::clamp(t * 2.0f, 0.0f, 1.0f), v = std::clamp(t * 2.0f - 1.0f, 0.0f, 1.0f);
    for (int i = 0; i < 3; i++) c[i] = t < 0.5f ? a[i] + (m[i] - a[i]) * u : m[i] + (z[i] - m[i]) * v;
}

float prop_float(const char* name, float def, float lo, float hi) {
    char v[PROP_VALUE_MAX] = {};
    if (__system_property_get(name, v) <= 0) return def;
    float f = static_cast<float>(std::atof(v));
    return f >= lo && f <= hi ? f : def;
}
}  // namespace

void flame_set_image(u32 image_lo) {
    image = image_lo;
    for (const Check& c : CHECKS) {
        if (g::rd32(at(c.fn & ~1u)) != c.word) {
            LOGW("flamethrower: unexpected code at 0x%08x; World at War's heat, flame and gauge (and the Wunderwaffe's "
                 "Black Ops chain) are off", c.fn & ~1u);
            return;
        }
    }
    buf = g::data_alloc(BUF_SIZE, 16);
    if (!buf) return;
    for (u32 i = 0; i < BUF_SIZE; i += 4) g::wr32(buf + i, 0);
    tesla_init();
    char v[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.boz64.flamethrower", v) > 0 && v[0] == '0') {
        LOGI("flamethrower: World at War's heat, flame and gauge are off (debug.boz64.flamethrower=0)");
        return;
    }
    if (g::rd16(at(SHOT_HOOK_AT)) != SHOT_HOOK_ORIG) {
        LOGW("flamethrower: unexpected code at 0x%08x; World at War's heat, flame and gauge are off", SHOT_HOOK_AT);
        return;
    }
    snd_ignite = buf + BUF_SOUNDS, snd_flame = buf + BUF_SOUNDS + 32;
    hash_ignite = djb2("waw_snd_ae363ff4");  // tools/wawguns.py FLAME_SOUNDS, as it names them
    hash_flame = djb2("waw_snd_7629b1ba");
    max_range = prop_float("debug.boz64.flame_range", 2.8f, 0.5f, 20.0f) * 100;
    emit_period = max_range / SPEED / NUM_SPHERES;
    size_scale = prop_float("debug.boz64.flame_size", 1.0f, 0.2f, 5.0f);
    if (__system_property_get("debug.boz64.flame_nozzle", v) > 0) {
        float f = 0, r = 0, u = 0;
        if (std::sscanf(v, "%f,%f,%f", &f, &r, &u) == 3 && f >= 0 && f < 200) nozzle_ofs = {f, r, u};
    }
    gun_normal = djb2("waw_flamethrower");
    gun_upgraded = djb2("waw_flamethrower_upgrade");
    g::wr16(at(SHOT_HOOK_AT), static_cast<u16>(0xdf00 | HOOK_SHOT));  // Thumb svc #imm8
    invalidate_code(at(SHOT_HOOK_AT), 2);
    enabled = true;
    LOGI("flamethrower: World at War's heat, flame and gauge are on (range %.1f m, a sphere every %.0f ms)",
         max_range / 100, emit_period);
}

bool flame_svc(GuestThread& t, u32 svc) {
    if (svc == HOOK_CHAIN) {
        tesla_hook(t.regs());
        return true;
    }
    if (svc != HOOK_SHOT) return false;
    auto& r = t.regs();
    r[1] = r[0];  // the replaced "mov r1, r0": bullets per shot
    if (enabled && fl.gun && r[5] == fl.gun) {
        r[1] = 0;  // the flame instead
        note_shot(now_ns());
    }
    return true;
}

void flame_on_player(u32 ctl) {
    if (!valid(ctl)) return;
    tesla_update(ctl);
    if (!enabled) return;
    u64 now = now_ns();
    bool fresh = !last_update || now - last_update > AWAY;
    float dt = fresh ? 0.0f : std::min(MAX_DT, static_cast<float>(now - last_update) / 1e9f);
    last_update = now;
    u32 wm = g::rd32(ctl + CTL_WEAPONS);
    u32 cur = valid(wm) ? g::rd32(wm + WM_SLOTS) : 0, hol = valid(wm) ? g::rd32(wm + WM_SLOTS + 4) : 0;
    u32 im = g::rd32(at(INPUT_MGR));
    note_held(valid(im) && (g::rd8(im + IM_FIRE) || g::rd8(im + IM_FIRE_HOLD)));
    for (Heat& s : heats) s.seen = false;
    const Heat* shown = nullptr;
    for (u32 gun : {cur, hol}) {
        if (!is_flame(gun)) continue;
        Heat* s = heat_of(gun);
        if (!s || s->seen) continue;
        s->seen = true;
        update_gun(*s, gun == cur, dt, now);
        if (gun == cur) shown = s;
    }
    for (Heat& s : heats)
        if (!s.seen) s = Heat{};  // not in either slot any more
    if (valid(wm)) flame_update(wm, cur, dt, now);
    hud_update(shown, now, fresh);
}

void flame_world_end(const float* pmv) {
    if (!enabled || !pmv) return;
    draw_flame(pmv);
}

void flame_draw() {
    u64 until = gauge_until.load();
    if (!enabled || !until || now_ns() > until) return;
    EGLDisplay dpy = eglGetCurrentDisplay();
    EGLSurface surf = eglGetCurrentSurface(EGL_DRAW);
    EGLint sw = 0, sh = 0;
    if (dpy == EGL_NO_DISPLAY || surf == EGL_NO_SURFACE || !eglQuerySurface(dpy, surf, EGL_WIDTH, &sw) ||
        !eglQuerySurface(dpy, surf, EGL_HEIGHT, &sh) || sw <= 0 || sh <= 0)
        return;
    if (!gl_init()) return;
    float rx = static_cast<float>(gauge_rect[0].load()), ry = static_cast<float>(gauge_rect[1].load());
    float rw = static_cast<float>(gauge_rect[2].load()), rh = static_cast<float>(gauge_rect[3].load());
    float heat = gauge_heat.load();
    bool over = gauge_over.load();

    SavedGL st;
    st.save(false);
    GLint fb = 0, vp[4];
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    glGetIntegerv(GL_VIEWPORT, vp);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, sw, sh);
    for (GLenum c : SavedGL::CAPS) glDisable(c);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);
    glUseProgram(prog_gauge);
    glEnableVertexAttribArray(0);

    // a bar across the middle of where the bullet row and the number were: a light frame, a dark trough, and the heat
    float fw = static_cast<float>(sw), fh = static_cast<float>(sh);
    float bh = std::clamp(rh * 0.42f, 6.0f, rh), bw = rw * 0.96f;
    float bx = rx + (rw - bw) / 2, by = ry + (rh - bh) / 2;
    float edge = std::max(1.0f, std::round(bh * 0.12f));
    quad(fw, fh, bx - edge, by - edge, bw + 2 * edge, bh + 2 * edge, 0.88f, 0.82f, 0.70f, 0.85f);
    quad(fw, fh, bx, by, bw, bh, 0.05f, 0.04f, 0.04f, 0.75f);
    if (over) {  // overheated: the whole bar flashes red until it has cooled to half
        float t = static_cast<float>(now_ns() % 400000000ull) / 400000000.0f;
        float pulse = 0.55f + 0.45f * std::sin(t * 6.2831853f);
        quad(fw, fh, bx, by, bw, bh, 0.95f, 0.10f, 0.04f, pulse);
    } else {
        float c[3];
        heat_colour(heat, c);
        quad(fw, fh, bx, by, bw * heat, bh, c[0], c[1], c[2], 0.95f);
        quad(fw, fh, bx, by, bw * heat, std::max(1.0f, bh * 0.3f), 1.0f, 1.0f, 0.85f, 0.25f);  // a highlight
    }

    st.restore();
    glViewport(vp[0], vp[1], vp[2], vp[3]);
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(fb));
}
