// Black Ops' dolphin dive, as in Black Ops (2010) Zombies: while sprinting, the crouch or prone button throws the
// player forward in a low arc and they land prone. On PC: Shift + C or Ctrl; on a controller: B while sprinting; on
// the touch screen: the crouch or prone button while the move pad is pushed into a sprint.
//
// How it works (all found in the game's CPlayerController; addresses as dumped at DUMP_BASE):
//  - The HUD's crouch and prone buttons (FUN_025f9a5c, FUN_025f9a14) send ON_CROUCH_BUTTON / ON_PRONE_BUTTON to the
//    local player. Every input path goes through them (the touch buttons, and pad.cpp for the keyboard and
//    controllers), so both are hooked at their first instruction. When a dive can start (see can_dive), the button
//    dives instead: it calls the prone button's own handler (so the posture and the HUD change exactly as if prone
//    had been pressed) and then starts the flight.
//  - The game already flies the player when they step off a ledge: +0x2f4 is a ballistic path {+4 duration,
//    +8 elapsed, +0xc gravity, +0x10 velocity, +0x1c start, +0x28 end} (FUN_025db95c sets one up from start to end in
//    a given time), and while +0x259 "falling" is set, the update (FUN_026320f8) moves the player along it each frame
//    instead of walking (FUN_0262f464). When it ends, the player is put down at the end point (with fall damage only
//    past a drop of m_fallHeightDamageThreshold, and never with PhD Flopper), the navmesh polygon is looked up again
//    and walking resumes. A dive is such a path, from the eye to a point ahead at prone eye height, in DIVE_TIME.
//  - The landing point comes from the navmesh move walking uses (FUN_0262d2e0: Detour's moveAlongSurface, then the
//    polygon's height there), from the feet toward DIVE_DISTANCE ahead: the dive slides along walls and stops at
//    ledges, so it always lands on the navmesh.
//  - Ahead is the way the player was moving in the frames before (sprinting is always forward).
//  - The eye height (+0x194) follows a smoothed value (+0x1a0 {current, target, step, frames}) toward the posture's
//    height. The flight ends at prone eye height and the smoothing is finished on landing, so the view stays there.
// Settings (boz64.props): debug.boz64.dive=0 turns it off; debug.boz64.dive_distance (metres, 3.0) and
// debug.boz64.dive_time (seconds, 0.55).
#include "dive.h"

#include <sys/system_properties.h>

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <ctime>

#include "cpu.h"

namespace {
constexpr u32 DUMP_BASE = 0x02454000;
u32 image = 0;
u32 at(u32 a) { return a - DUMP_BASE + image; }
bool valid(u32 a) { return a >= g::NULL_GUARD && a < g::ALLOC_END; }

// the HUD's buttons, hooked at their first instruction
constexpr u32 PRONE_BUTTON = 0x025f9a14;    // "ldr r3, [pc, #0x3c]"
constexpr u16 PRONE_FIRST = 0x4b0f;
constexpr u32 PRONE_LITERAL = 0x025f9a54;   // what that loads
constexpr u32 CROUCH_BUTTON = 0x025f9a5c;   // "push {r4, r5, r6, lr}"
constexpr u16 CROUCH_FIRST = 0xb570;
constexpr u32 HOOK_PRONE = 0xbd, HOOK_CROUCH = 0xbe;

// globals (each holds a pointer)
constexpr u32 GAME = 0x028ba484, GAME_PLAYER = 0x1a0;  // the game object; its local player (entity reference)
constexpr u32 VIEW_LOCK = 0x028badac;  // while its object's +0xd4 bit 17 is set, the view is scripted
// functions (Thumb)
constexpr u32 FN_REF_GET = 0x02512568 | 1;     // (entity reference): the entity, or 0
constexpr u32 FN_PLAYER_CTL = 0x0262cb88 | 1;  // (entity reference): its CPlayerController
constexpr u32 FN_ANIM_BUSY = 0x0253ab10 | 1;   // (CPlayerController +0xb0 -> +0x34): an animation moves the player
constexpr u32 FN_NAV_MOVE = 0x0262d2e0 | 1;    // (controller, from, to, out, &poly, &height): walk on the navmesh
// first instructions, checked before anything is patched or called
struct Check { u32 fn, word; };
constexpr Check CHECKS[] = {
    {FN_REF_GET, 0x4603b508}, {FN_PLAYER_CTL, 0x4603b508}, {FN_ANIM_BUSY, 0x2b006c43}, {FN_NAV_MOVE, 0x4bf0e92d},
};

// CPlayerController
constexpr u32 CTL_ANIM = 0xb0;
constexpr u32 CTL_POSTURE = 0x121;          // byte: 2 prone, 4 crouched, 8 last stand
constexpr u32 CTL_SPRINTING = 0x156;        // byte
constexpr u32 CTL_IN_WATER = 0x15c;         // byte: posture changes are refused while it is set
constexpr u32 CTL_ENTITY = 0x168;           // the player entity: its position at +0xa8 (x, y, z: the eye)
constexpr u32 ENT_POS = 0xa8;
constexpr u32 CTL_PRONE_HEIGHT = 0x190;     // m_ProneHeight (eye above the navmesh, world units)
constexpr u32 CTL_EYE_HEIGHT = 0x194;       // the eye's height now
constexpr u32 CTL_EYE_SMOOTH = 0x1a0;       // {current, target, step, int frames}
constexpr u32 CTL_NAV_POLY = 0x1b0;         // the navmesh polygon the player is on (0: looked up next update)
constexpr u32 CTL_GLIDE = 0x248;            // frames left of a scripted glide
constexpr u32 CTL_FALLING = 0x259;          // byte
constexpr u32 CTL_FALL_START = 0x25c;       // 0 when a fall starts
constexpr u32 CTL_FALL_HEIGHT = 0x260;      // where the fall started (fall damage is measured from it)
constexpr u32 CTL_FALL_MOVE = 0x264;        // fallingMoveVec
constexpr u32 CTL_FALL_HEIGHT2 = 0x270;
constexpr u32 CTL_PATH = 0x2f4;             // the ballistic path
constexpr u32 CTL_FALLING2 = 0x32c;         // byte, set with CTL_FALLING
constexpr u32 PATH_TIME = 4, PATH_ELAPSED = 8, PATH_GRAVITY = 0xc, PATH_VEL = 0x10, PATH_START = 0x1c, PATH_END = 0x28;
constexpr u8 POSTURE_ANY = 2 | 4 | 8;

constexpr float WORLD = 100.0f;     // world units per navmesh unit (metre)
constexpr float GRAVITY = -981.0f;  // the game's (FUN_0262e944)
constexpr u64 MS = 1000000ull;
constexpr u64 DIR_FRESH = 250 * MS;  // the movement direction must be this recent
constexpr u64 MAX_FLIGHT = 3000 * MS;

bool enabled = false;
bool passing = false;  // the prone handler is being called by a dive: let it run

struct Vec { float x, y, z; };
Vec rdv(u32 a) { return {g::rdf(a), g::rdf(a + 4), g::rdf(a + 8)}; }
void wrv(u32 a, Vec v) { g::wrf(a, v.x), g::wrf(a + 4, v.y), g::wrf(a + 8, v.z); }

// the local player's movement, sampled at the start of each update
u32 last_ctl = 0;
Vec last_pos{};
float dir_x = 0, dir_z = 0;  // horizontal, unit length
u64 dir_at = 0;
// the dive in flight
u32 dive_ctl = 0;
u64 dive_since = 0;

// calls one of the game's functions above (their addresses are in the dump: moved to where the image really is)
u64 call_game(u32 fn, std::initializer_list<u32> args) { return call_guest(at(fn), args); }

u64 now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

float prop_float(const char* name, float def) {
    char v[PROP_VALUE_MAX] = {};
    if (__system_property_get(name, v) <= 0) return def;
    return static_cast<float>(atof(v));
}

u32 local_controller() {
    u32 game = g::rd32(at(GAME));
    if (!valid(game)) return 0;
    u32 ref = g::rd32(game + GAME_PLAYER);
    if (!valid(ref) || !call_game(FN_REF_GET, {ref})) return 0;
    u32 ctl = static_cast<u32>(call_game(FN_PLAYER_CTL, {ref}));
    return valid(ctl) ? ctl : 0;
}

// whether ctl may dive now: standing, sprinting, walking on the navmesh, nothing else moving or steering it
bool can_dive(u32 ctl) {
    if (!enabled || !ctl || dive_ctl) return false;
    if (prop_float("debug.boz64.dive", 1.0f) == 0.0f) return false;
    if (g::rd8(ctl + CTL_POSTURE) & POSTURE_ANY) return false;
    if (!g::rd8(ctl + CTL_SPRINTING) || g::rd8(ctl + CTL_IN_WATER) || g::rd8(ctl + CTL_FALLING)) return false;
    if (g::rd32(ctl + CTL_GLIDE) || !g::rd32(ctl + CTL_NAV_POLY) || !valid(g::rd32(ctl + CTL_ENTITY))) return false;
    u32 anim = g::rd32(ctl + CTL_ANIM);
    if (!valid(anim) || call_game(FN_ANIM_BUSY, {anim + 0x34})) return false;
    u32 lock = g::rd32(at(VIEW_LOCK));
    if (valid(lock) && ((g::rd32(lock + 0xd4) >> 17) & 1)) return false;
    return ctl == last_ctl && now_ns() - dir_at < DIR_FRESH;
}

// starts the flight (the posture is already prone)
void launch(GuestThread& t, u32 ctl) {
    float dist = std::clamp(prop_float("debug.boz64.dive_distance", 3.0f), 0.5f, 8.0f);
    float time = std::clamp(prop_float("debug.boz64.dive_time", 0.55f), 0.2f, 1.5f);
    u32 ent = g::rd32(ctl + CTL_ENTITY);
    Vec eye = rdv(ent + ENT_POS);
    float eye_h = g::rdf(ctl + CTL_EYE_HEIGHT), prone_h = g::rdf(ctl + CTL_PRONE_HEIGHT);
    // the landing point: walk the navmesh from the feet toward dist ahead (navmesh units)
    u32 buf = guest_scratch(t, 64);
    Vec from{eye.x / WORLD, (eye.y - eye_h) / WORLD, eye.z / WORLD};
    Vec to{from.x + dir_x * dist, from.y, from.z + dir_z * dist};
    wrv(buf, from);
    wrv(buf + 12, to);
    wrv(buf + 24, from);
    g::wr32(buf + 36, g::rd32(ctl + CTL_NAV_POLY));
    g::wrf(buf + 40, 0.0f);
    call_game(FN_NAV_MOVE, {ctl, buf, buf + 12, buf + 24, buf + 36, buf + 40});
    Vec land = rdv(buf + 24);
    float ground = g::rdf(buf + 40);
    if (ground == 0.0f) ground = land.y;  // (the game also takes 0 as "no height found")
    float moved = std::hypot(land.x - from.x, land.z - from.z);
    // a short dive (a wall ahead) is quicker
    float T = time * std::clamp(moved / dist, 0.4f, 1.0f);
    Vec end{land.x * WORLD, ground * WORLD + prone_h, land.z * WORLD};
    u32 p = ctl + CTL_PATH;
    g::wrf(p + PATH_TIME, T);
    g::wrf(p + PATH_ELAPSED, 0.0f);
    g::wrf(p + PATH_GRAVITY, GRAVITY);
    wrv(p + PATH_VEL, {(end.x - eye.x) / T, ((end.y - eye.y) - GRAVITY * 0.5f * T * T) / T, (end.z - eye.z) / T});
    wrv(p + PATH_START, eye);
    wrv(p + PATH_END, end);
    // fall damage counts only what the feet really dropped (going down a slope), not the eye going down to prone
    float drop = std::max(0.0f, (eye.y - eye_h) - ground * WORLD);
    g::wrf(ctl + CTL_FALL_HEIGHT, end.y + drop);
    g::wrf(ctl + CTL_FALL_HEIGHT2, end.y + drop);
    g::wr32(ctl + CTL_FALL_START, 0);
    wrv(ctl + CTL_FALL_MOVE, {0, 0, 0});
    g::wr8(ctl + CTL_FALLING2, 1);
    g::wr8(ctl + CTL_FALLING, 1);
    dive_ctl = ctl;
    dive_since = now_ns();
    LOGI("dive: %.2f m of %.2f in %.2f s, landing %.0f below the start", moved, dist, T, eye.y - end.y);
}

// a crouch or prone press: dives instead when it can. Returns true if it dived.
bool try_dive(GuestThread& t) {
    if (passing) return false;
    u32 ctl = local_controller();
    if (!can_dive(ctl)) return false;
    passing = true;
    call_guest(at(PRONE_BUTTON) | 1, {});  // prone, as the button does
    passing = false;
    if (!(g::rd8(ctl + CTL_POSTURE) & 2)) return false;  // it was refused: no dive
    launch(t, ctl);
    return true;
}

// ---- World at War's sprint (WaW PC: player_sprintTime 4 s, Stamin-Up doubles it) on Black Ops' stamina
// (0x0262d94e): fatigue rises at SprintingRatePerSec while sprinting; past SprintingCapacity (100, perked 200) the
// player is worn out and walks, and may sprint again once fatigue is back down to SprintingThreshold and sprint has
// been let go. Black Ops' 80 brought a sprint back 0.2 s after running out with 0.7 s in it: short bursts and walks.
// The touch move pad (0x026339e8) sprints past sprintUp (1.5 radii) and stops under sprintDown (1.2) or more than
// SprintAngle off straight ahead, so a thumb easing back or sideways dropped the sprint.
// The tweakables are objects (float value at +0x10) the game makes on first use; the stamina ones and SprintAngle
// are kept in slots, sprintDown is looked up each time under the control layout's name for it (FN_LAYOUT_NAME).
// debug.boz64.waw_sprint=0 keeps Black Ops' values.
constexpr u32 TWEAKS = 0x028c6534;
struct Tune { u32 slot; float value; };
constexpr Tune TUNES[] = {
    {0xa0, 25.0f},    // SprintingRatePerSec (30): 100 / 25 = 4 s
    {0x98, 0.0f},     // SprintingThreshold (80): rested, at SprintingRecoveryRatePerSec 100, in 1 s
    {0xc0, 25.0f},    // PerkedSprintingRatePerSec (30): 200 / 25 = 8 s
    {0xb8, 0.0f},     // PerkedSprintingThreshold (200): rested, at 400, in 0.5 s
    {0x370, 0.785f},  // SprintAngle (0.5 rad): 45 degrees
};
constexpr float SPRINT_DOWN = 0.9f;  // sprintDown (1.2): WalkFraction, where the pad slows to a walk anyway
constexpr u32 FN_TWEAK = 0x025407c0 | 1;        // (name, default, 0, description): the float tweakable, made if new
constexpr u32 FN_LAYOUT_NAME = 0x02669ae0 | 1;  // (name): its name in the current control layout
constexpr u32 STR_SPRINT_DOWN = 0x02811e87, STR_SPRINT_DOWN_DEF = 0x02811dfa, STR_SPRINT_DESC = 0x02811e5a;
constexpr Check SPRINT_CHECKS[] = {
    {FN_TWEAK, 0x47ffe92d}, {FN_LAYOUT_NAME, 0x4607b5f8},
    {0x0262d974, 0x1090f8d3}, {0x0262d95e, 0x10b0f8d3}, {0x02633a42, 0x3370f8d3},  // the reads of slots 0x90, 0xb0, 0x370
};
bool sprint_ok = false;
u32 sprint_tuned = 0;  // the controller it was done for

void tune_sprint(u32 ctl) {
    if (!sprint_ok || sprint_tuned == ctl) return;
    bool all = true;
    for (const Tune& t : TUNES) {
        u32 o = g::rd32(at(TWEAKS + t.slot));
        if (valid(o)) g::wrf(o + 0x10, t.value);
        else all = false;  // not made yet: again next update
    }
    if (!all) return;
    u32 name = static_cast<u32>(call_game(FN_LAYOUT_NAME, {at(STR_SPRINT_DOWN)}));
    u32 down = valid(name) ? static_cast<u32>(call_game(FN_TWEAK, {name, at(STR_SPRINT_DOWN_DEF), 0,
                                                                   at(STR_SPRINT_DESC)})) : 0;
    if (valid(down)) g::wrf(down + 0x10, SPRINT_DOWN);
    sprint_tuned = ctl;  // shortcut: done once a game, not again if the control layout is changed mid-game
    LOGI("dive: World at War's sprint (4 s, 8 s with Stamin-Up; again once rested)%s",
         valid(down) ? "" : "; sprintDown not found");
}

// returns from the hooked function at once (it has not touched the stack yet): "bx lr"
void return_now(GuestThread& t) {
    auto& r = t.regs();
    u32 lr = r[14];
    r[15] = lr & ~1u;
    t.set_cpsr((t.cpsr() & ~0x20u) | ((lr & 1) ? 0x20u : 0u));
}
}  // namespace

void dive_set_image(u32 image_lo) {
    image = image_lo;
    sprint_ok = prop_float("debug.boz64.waw_sprint", 1.0f) != 0.0f;
    for (const Check& c : SPRINT_CHECKS) {
        if (sprint_ok && g::rd32(at(c.fn & ~1u)) != c.word) {
            LOGW("dive: unexpected code at 0x%08x; Black Ops' sprint is kept", c.fn & ~1u);
            sprint_ok = false;
        }
    }
    for (const Check& c : CHECKS) {
        if (g::rd32(at(c.fn & ~1u)) != c.word) {
            LOGW("dive: unexpected code at 0x%08x; the dolphin dive is off", c.fn & ~1u);
            return;
        }
    }
    if (g::rd16(at(PRONE_BUTTON)) != PRONE_FIRST || g::rd16(at(CROUCH_BUTTON)) != CROUCH_FIRST) {
        LOGW("dive: unexpected code at the crouch and prone buttons; the dolphin dive is off");
        return;
    }
    g::wr16(at(PRONE_BUTTON), static_cast<u16>(0xdf00 | HOOK_PRONE));  // Thumb svc #imm8
    g::wr16(at(CROUCH_BUTTON), static_cast<u16>(0xdf00 | HOOK_CROUCH));
    invalidate_code(at(PRONE_BUTTON), 2);
    invalidate_code(at(CROUCH_BUTTON), 2);
    enabled = true;
    LOGI("dive: the dolphin dive is on (sprint, then crouch or prone)");
}

bool dive_svc(GuestThread& t, u32 svc) {
    auto& r = t.regs();
    switch (svc) {
    case HOOK_PRONE:
        // mid-dive the posture stays prone (the game's prone toggle does not look at falling; crouch already does)
        if (dive_ctl && !passing) return_now(t);
        else if (try_dive(t)) return_now(t);
        else r[3] = g::rd32(at(PRONE_LITERAL));  // the replaced "ldr r3, [pc, #0x3c]"
        return true;
    case HOOK_CROUCH:
        if (try_dive(t)) {
            return_now(t);
        } else {  // the replaced "push {r4, r5, r6, lr}"
            u32 sp = r[13] - 16;
            g::wr32(sp, r[4]), g::wr32(sp + 4, r[5]), g::wr32(sp + 8, r[6]), g::wr32(sp + 12, r[14]);
            r[13] = sp;
        }
        return true;
    default:
        return false;
    }
}

void dive_on_player(u32 ctl) {
    if (!valid(ctl)) return;
    tune_sprint(ctl);
    if (!enabled) return;
    u64 now = now_ns();
    if (dive_ctl) {
        if (dive_ctl != ctl || now - dive_since > MAX_FLIGHT) {
            dive_ctl = 0;
        } else if (!g::rd8(ctl + CTL_FALLING)) {
            // landed last update: finish the eye's smoothing so the view stays at the height the flight ended at
            u32 s = ctl + CTL_EYE_SMOOTH;
            g::wrf(s, g::rdf(s + 4));
            g::wr32(s + 12, 0);
            g::wrf(ctl + CTL_EYE_HEIGHT, g::rdf(s));
            dive_ctl = 0;
        }
    }
    u32 ent = g::rd32(ctl + CTL_ENTITY);
    if (!valid(ent) || g::rd8(ctl + CTL_FALLING)) {
        last_ctl = 0;
        return;
    }
    Vec p = rdv(ent + ENT_POS);
    if (ctl == last_ctl) {
        float dx = p.x - last_pos.x, dz = p.z - last_pos.z, d = std::hypot(dx, dz);
        if (d > 0.5f && d < 200.0f) dir_x = dx / d, dir_z = dz / d, dir_at = now;
    }
    last_ctl = ctl;
    last_pos = p;
}

bool dive_ready() { return can_dive(local_controller()); }
