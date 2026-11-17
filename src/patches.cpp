#include "patches.h"

#include <sys/stat.h>
#include <sys/system_properties.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

#include "codpoints.h"
#include "cpu.h"
#include "lan.h"
#include "menu.h"
#include "mulekick.h"
#include "vox.h"
#include "voxev.h"
#include "dogs.h"
#include "dive.h"
#include "flame.h"
#include "doubletap.h"
#include "pad.h"
#include "settings.h"
#include "smooth.h"

namespace {
// The game clock (Ideaworks CIwGameClock). SetFPS(this, min_fps, max_fps) sets the shortest time between
// game ticks to 1000/max_fps ms and the longest tick to 1000/min_fps ms; each tick then advances the game
// by the real time since the last one. The game asks for (10, 30): at most 30 ticks a second, so the
// player's view and movement change only every 4th frame at 120 fps. Raising max_fps to 1000 gives one
// tick per drawn frame, each as long as the frame took.
//   push {r3,r4,r5,lr}; mov r5,r1; mov r1,r2   <- svc HOOK_SETFPS (r1 = max fps to use); mov r4,r0; bl ...
const char SIG_SETFPS[] = "38b50d4611460446fff7d9ff294620612046fff7d4ff216901446061204601eb";
constexpr u32 SETFPS_HOOK_AT = 4;
constexpr u32 TICK_MAX_FPS = 1000;
// Bullet physics step, called once per game tick with a fixed 1/30 s:
//   world->stepSimulation(1/30.f, 0, 1/60.f)    "ldr r1, =1/30" <- svc HOOK_PHYS (r1 = this tick's length)
const char SIG_PHYS[] = "10b5406b28b10368002202491c6b024ba04710bd8988083d8988883c";
constexpr u32 PHYS_HOOK_AT = 10;

// The "network universe" update: a countdown that resets to 1000 / NetworkUniverseFPS ms, then steps
// the universe. "ldr r2, [r3, #0x14]" (the NetworkUniverseFPS setting) <- svc HOOK_UNIVERSE
const char SIG_UNIVERSE[] = "1b4b9fed137a7b445a69d2ed047af8eee77ac7ee277afdeee77ac3ed037a154b7b441868";
constexpr u32 UNIVERSE_HOOK_AT = 8;

// The game's scheduler (CIsGameClock, made with 7 channels at 60 ticks/s). Every channel ticks each
// 1000/60 = 16 ms, and channel i spreads its objects over 2^i slots, one slot per tick. The player and the
// camera are in channel 1, so they move only every 2nd tick: about 31 times a second, whatever the frame
// rate (zombies look smooth because the game blends them between ticks; the camera is not blended).
// Before each update the tick length is set so channel 1 ticks once per drawn frame: two ticks a frame,
// each half the frame's time in whole ms, the remainder carried over, so game time still follows real
// time. Below ~31 fps the tick stays at the original 16 ms.
//   caller: ldr r3,..; push {r4-r6,lr}; add r3,pc; mov r6,r0; sub sp,#48; ldr r0,[r3]; bl ..;
//           mov r0, r6   <- svc HOOK_SCHED (r6 = the scheduler); bl CIsGameClock::Update
const char SIG_SCHED[] = "2e4b70b57b4406468cb0186800f0b0fd304601f0b7fcd6e9";
constexpr u32 SCHED_HOOK_AT = 16;
constexpr u32 SCHED_UPDATE_BL_AT = 18;
constexpr u32 SCHED_TIMER_BLX_IN_UPDATE = 0x16;  // Update's "blx s3eTimerGetMs"
constexpr u32 SCHED_TICK_CAP_AT = 0x5e;           // Update's "mov.w r10, #5": the tick loop's counter
constexpr u32 SCHED_TICK_CAP_OLD = 0x0a05f04f;    // mov.w r10, #5   (at most 4 ticks)
constexpr u32 SCHED_TICK_CAP_NEW = 0x0a40f04f;    // mov.w r10, #64  (at most 63)

// "Zombies ignore you" option: the AI never picks the local player as its target.
// Every AI kind looks for the nearest player through one shared search, which already drops players it should
// not chase (each candidate goes through "mov r0, r9" before its checks); the local player is dropped the same
// way, by going to the search's own "drop this candidate" code.
//   ldr r3, =global; add r3, pc; ldr r3, [r3]; ldr.w r3, [r3, #0x1a0] (the local player) ...
const char SIG_NEAREST[] = "914b7b441b68d3f8a03106938f4b59f803305a689b46d2f80890dbf804200435";
constexpr u32 NEAREST_HOOK_AT = 0xb8;   // "mov r0, r9"
constexpr u32 NEAREST_DROP_AT = 0x192;  // "movs r3, #0; str r3, [r6, r5]; b next"
constexpr u32 PLAYERS_LOCAL = 0x1a0;    // the global game object's local player entity
// Each AI kind's SetTarget(this, entity) returns straight away for the local player; this also covers the
// "random player" pick, which falls back to the local player when playing alone.
//   zombie: push.w {r4-r11, lr}; mov r5, r0 <- svc; ...     the others: push {...} <- svc; mov r4, r0; ...
const char SIG_TARGET_ZOMBIE[] = "2de9f04f054687b008460c46cff04cfa064620469cf030f9";
const char SIG_TARGET_DOG[] = "f7b50446084616460d46bef763fa164f7f44014604f59070";
const char SIG_TARGET_AI3[] = "f7b50446084616460d46baf77bf9164f7f44014604f59e70";
const char SIG_TARGET_AI4[] = "73b5044690f88930092b16d0c0f838110846b1f7a9f80a4b";

// The "Controls Sensitivity" page (CIngameStateSensitivity), where our options are added (menu.cpp).
// End of its OnEnter, once the page is built:  ...; bl ..; "add sp, #8" <- svc; vpop {d8}; pop.w {r4-r8, pc}
const char SIG_SENS_ENTER[] = "1e490022134606f12800794435f760f81b4b1c497b447944186835f733fb02b0bdec028bbde8f081";
constexpr u32 SENS_ENTER_HOOK_AT = 0x1e;
// Its "SensitivityY" slot, run when the Y slider (or one of our copies of it) moves; r1 = the slider:
//   "push {r3, lr}" <- svc; mov r0, r1; blx CIwUISlider::GetValue; ...
const char SIG_SENS_Y[] = "08b5084673f100eb044b7b441b6807ee900afaeeca7ac3ed3f7a08bd";
// Its "Calibrate" slot (the tilt controls' calibrate button, or our checkbox's copy of it); r1 = the button:
//   ldr r3, [pc, #0x24]; "movs r2, #0" <- svc; push {r0, r1, r4, lr}; ...
const char SIG_SENS_CALIBRATE[] = "094b002213b57b44009204461868074b7b441968134632f754fe2046";
constexpr u32 SENS_CALIBRATE_HOOK_AT = 2;
// Its OnExit, before the page is taken down:  push {r3-r5, lr}; "mov r4, r0" <- svc; ...
const char SIG_SENS_EXIT[] = "38b504460c4b0d4d7b447d44186850f095fb0b4b04f10c01";
constexpr u32 SENS_EXIT_HOOK_AT = 2;
// The panel every menu page sits on (CUIBackgroundImage) keeps the game's 16:10 shape and is sized by
// CUIBackgroundImage::Layout(this, pos, size) to cover the screen, so on wider screens its top and bottom (the
// page title, the Back button) are off screen. After it works out the size (blx; "ldr r2, [sp, #0xc]" <- svc,
// size at sp+8, the space it has at r4), a panel that would not fit is made smaller to fit instead.
const char SIG_PANEL_SIZE[] = "2de9f0418eb00df11808064602ad14460f4640463146fff7cbff00232846009341462246fdf0c4e8039a";
constexpr u32 PANEL_SIZE_HOOK_AT = 0x28;
// The end of the same function, once the panel and everything on it are laid out (r6 = the panel):
//   blx CIwUIElement::Layout; "add sp, #0x38" <- svc; pop.w {r4-r8, pc}
const char SIG_PANEL_LAID_OUT[] = "3046d31a7a6803ebd373a2eb6301029a2368059104a9d31a3a6803ebd373a2eb63032a460493f0f0e8ef0eb0bde8f081";
constexpr u32 PANEL_LAID_OUT_HOOK_AT = 0x2a;

// Points the local player earns (CScoreManager's add-score path, FUN_02650564 with "earned" set): the game adds
// them to its per-game total of earned points:  ...; bl map[]; ldr r3, [r0]; "add r3, r6" <- svc; str r3, [r0]; ...
// (r6 = the points). They count toward COD points (codpoints.cpp).
const char SIG_SCORE_EARNED[] = "07b0f0bd04f14c002946fff752ff036833440360d0e7";
constexpr u32 SCORE_EARNED_HOOK_AT = 0x10;

// The main mode's in-game update (CIngameStatePlaying, vtable slot 3), once per frame during play; the
// controller's input is applied there (pad.cpp):  push {r3, r4, r5, lr}; "mov r4, r0" <- svc; bl ...
const char SIG_INGAME_UPDATE[] = "38b504461df03dfe214b224d7b447d4418680368db6b9847286890f8083283b1";
constexpr u32 INGAME_UPDATE_HOOK_AT = 2;
// Dead Ops Arcade's in-game update (CDOIngameStatePlaying, vtable slot 3), the same shape:
//   push {r3, r4, r5, lr}; "mov r4, r0" <- svc; bl ...
const char SIG_DO_UPDATE[] = "38b5044676f0ebfa0c4d7d442868d8f714f910b1204600210ce07e20b9f6b8e9";
constexpr u32 DO_UPDATE_HOOK_AT = 2;

// The network update (FUN_025240bc), called every frame by the multiplayer manager's update in menus, lobbies and
// play. LAN discovery results are handed to the game here (lan.cpp): this is game code on the app fibre, where the
// game's callbacks can make s3e calls (connect) safely.  push.w {r0-r10, lr}; "mov r6, r0" <- svc; ldr r4, ...
const char SIG_NET_UPDATE[] = "2de9ff4706462f4cdff8bc807c44f844e3681b1ae360002b3edc2c4b7b441b68";
constexpr u32 NET_UPDATE_HOOK_AT = 4;

// The other players in co-op (CRemotePlayer). Its update runs once per drawn frame. Each position the network
// brings, it glides to over 8 updates (count +0x150, step +0x144, target +0x138, position +0x12c), and each aim
// angle (yaw +0x154, pitch +0x164: {current, target, step, int count}) over 5; it plays the walk animation only while
// gliding. The game was made for 30 frames a second: 8 frames is 267 ms, longer than the time between network
// positions (50-150 ms apart while walking), so a walking player glides without a break. At 200 fps the glide ends
// in 40 ms and the player stands still until the next position: the walk animation stops and restarts each time and
// the player slides. Each new glide is stretched to last as long as at 30 fps (measured on a two-copy PC match:
// gliding 88% of the time while the other player walked, the rest being where they started and stopped).
//   push.w {r4-r11, lr}; vpush {d8, d9}; "mov r4, r0" <- svc; ...
const char SIG_REMOTE_UPDATE[] = "2de9f04f2ded048b044690ed4b8ad74f8bb0d0f8503190ed4c9a7f44";
constexpr u32 REMOTE_UPDATE_HOOK_AT = 8;
// What other players see a co-op player hold. CRemotePlayer's "change the upper-body animation set" (FUN_02642a58),
// called only by its message handler (CRemotePlayer v30) on SUBJECT_WEAPON_SET, when that player switches weapons:
// r1 = the weapon's mpArmsAsm (the name hash of a CIsAnimStateMachine), r7 = the weapon's name hash (the handler's
// copy of the message's first value; callee-saved and not touched before the hook). See remote_arms.
//   push {r4, r5, r6, lr}; "mov r4, r0" <- svc; ...
const char SIG_REMOTE_POSE[] = "70b50446124d0e46c0683bf013fc114b7d44114a3146eb587a44186802234bf09dfb00220146d4f8a00032f7d7fa94f8";
constexpr u32 REMOTE_POSE_HOOK_AT = 2;
// The local player's view update (CPlayerController, FUN_026320f8), run once per player update: it turns the view
// by the look input and rebuilds it from yaw and pitch. The mouse's turn is added just before (pad.cpp):
//   push.w {r4-r11, lr}; vpush {d8-d10}; "mov r4, r0" <- svc; ...
const char SIG_PLAYER_LOOK[] = "2de9f04f2ded068b0446b24db3b000f5d070dff8c4927d44fdf75afa";
constexpr u32 PLAYER_LOOK_HOOK_AT = 8;
// The HUD's interaction prompt (FUN_025f8e80: (element, caption, show, show the button)), which sets the caption of
// its "Label" and shows or hides its round "Button" and "Image". pad.cpp shrinks the button for the keyboard and
// controllers:   push {r3-r7, lr}; mov r5, r3; ldr r3, [pc, #..]; "mov r4, r0" <- svc; ...
const char SIG_PROMPT[] = "f8b51d461a4b04460f4616467b441b682bb1002a14bf2a46002283f88820";
constexpr u32 PROMPT_HOOK_AT = 6;

enum : u32 {
    HOOK_REMOTE_UPDATE = 0x94, HOOK_REMOTE_POSE = 0x95, HOOK_PLAYER_LOOK = 0x96, HOOK_PROMPT = 0x97,
    HOOK_SETFPS = 0x81, HOOK_PHYS = 0x82, HOOK_UNIVERSE = 0x83, HOOK_SCHED = 0x84,
    HOOK_NEAREST = 0x85, HOOK_TARGET_AFTER_PUSHW = 0x86, HOOK_TARGET_DOG = 0x87, HOOK_TARGET_AI3 = 0x88,
    HOOK_TARGET_AI4 = 0x89, HOOK_SENS_ENTER = 0x8a, HOOK_SENS_Y = 0x8b,
    HOOK_SENS_EXIT = 0x8c, HOOK_PANEL_SIZE = 0x8d, HOOK_PANEL_LAID_OUT = 0x8e, HOOK_SENS_CALIBRATE = 0x8f,
    HOOK_SCORE_EARNED = 0x90, HOOK_INGAME_UPDATE = 0x91, HOOK_DO_UPDATE = 0x92, HOOK_NET_UPDATE = 0x93,
};

struct Patch {
    const char* name;
    const char* sig;
    u32 at;
    u32 svc;
    u32 addr = 0;  // where the signature was found
    u16 orig = 0;  // the instruction the svc replaced
};
Patch patches[] = {
    {"game clock tick rate", SIG_SETFPS, SETFPS_HOOK_AT, HOOK_SETFPS},
    {"network universe rate", SIG_UNIVERSE, UNIVERSE_HOOK_AT, HOOK_UNIVERSE},
    {"game scheduler tick", SIG_SCHED, SCHED_HOOK_AT, HOOK_SCHED},
    {"AI nearest player", SIG_NEAREST, NEAREST_HOOK_AT, HOOK_NEAREST},
    {"zombie SetTarget", SIG_TARGET_ZOMBIE, 4, HOOK_TARGET_AFTER_PUSHW},
    {"hellhound SetTarget", SIG_TARGET_DOG, 0, HOOK_TARGET_DOG},
    {"AI kind 3 SetTarget", SIG_TARGET_AI3, 0, HOOK_TARGET_AI3},
    {"AI kind 4 SetTarget", SIG_TARGET_AI4, 0, HOOK_TARGET_AI4},
    {"sensitivity page OnEnter", SIG_SENS_ENTER, SENS_ENTER_HOOK_AT, HOOK_SENS_ENTER},
    {"sensitivity page Y slider", SIG_SENS_Y, 0, HOOK_SENS_Y},
    {"sensitivity page OnExit", SIG_SENS_EXIT, SENS_EXIT_HOOK_AT, HOOK_SENS_EXIT},
    {"sensitivity page Calibrate", SIG_SENS_CALIBRATE, SENS_CALIBRATE_HOOK_AT, HOOK_SENS_CALIBRATE},
    {"points earned", SIG_SCORE_EARNED, SCORE_EARNED_HOOK_AT, HOOK_SCORE_EARNED},
    {"in-game update", SIG_INGAME_UPDATE, INGAME_UPDATE_HOOK_AT, HOOK_INGAME_UPDATE},
    {"Dead Ops update", SIG_DO_UPDATE, DO_UPDATE_HOOK_AT, HOOK_DO_UPDATE},
    {"network update", SIG_NET_UPDATE, NET_UPDATE_HOOK_AT, HOOK_NET_UPDATE},
    {"remote player update", SIG_REMOTE_UPDATE, REMOTE_UPDATE_HOOK_AT, HOOK_REMOTE_UPDATE},
    {"remote player arms", SIG_REMOTE_POSE, REMOTE_POSE_HOOK_AT, HOOK_REMOTE_POSE},
    {"player view update", SIG_PLAYER_LOOK, PLAYER_LOOK_HOOK_AT, HOOK_PLAYER_LOOK},
    {"HUD prompt", SIG_PROMPT, PROMPT_HOOK_AT, HOOK_PROMPT},
    {"menu panel size", SIG_PANEL_SIZE, PANEL_SIZE_HOOK_AT, HOOK_PANEL_SIZE},
    {"menu panel laid out", SIG_PANEL_LAID_OUT, PANEL_LAID_OUT_HOOK_AT, HOOK_PANEL_LAID_OUT},
};
const Patch* patch_by_svc(u32 svc) {
    for (const Patch& p : patches)
        if (p.svc == svc) return &p;
    return nullptr;
}

std::atomic<bool> zombies_ignore{false};
u32 players_global = 0;  // address of the pointer to the game object that holds the local player
u32 local_player() {
    u32 obj = players_global ? g::rd32(players_global) : 0;
    return obj >= g::NULL_GUARD ? g::rd32(obj + PLAYERS_LOCAL) : 0;
}
bool ignore_target(u32 entity) { return zombies_ignore && entity && entity == local_player(); }

u32 clock_obj = 0;
u32 sched_timer = 0;  // the guest timer the scheduler reads (ms, 64-bit)
constexpr u32 SCHED_ORIGINAL_MS = 16;

// Target of a Thumb BL/BLX at a (bit 0 set for Thumb targets).
u32 thumb_call_target(u32 a) {
    u16 h1 = g::rd16(a), h2 = g::rd16(a + 2);
    if ((h1 & 0xf800) != 0xf000 || (h2 & 0xc000) != 0xc000) return 0;
    u32 S = (h1 >> 10) & 1, J1 = (h2 >> 13) & 1, J2 = (h2 >> 11) & 1;
    u32 I1 = 1 - (J1 ^ S), I2 = 1 - (J2 ^ S);
    s32 off = static_cast<s32>((S << 24) | (I1 << 23) | (I2 << 22) | ((h1 & 0x3ffu) << 12) | ((h2 & 0x7ffu) << 1));
    if (S) off -= 1 << 25;
    u32 t = a + 4 + static_cast<u32>(off);
    return (h2 & 0x1000) ? (t | 1) : (t & ~3u);  // BL stays Thumb, BLX goes to ARM
}
u32 image_lo = 0, image_hi = 0;  // the game image (first executable region scanned)
std::mutex region_lock;
std::vector<std::pair<u32, u32>> regions;  // executable regions not yet scanned

std::vector<u8> unhex(const char* s) {
    std::vector<u8> v;
    for (size_t i = 0; s[i] && s[i + 1]; i += 2) {
        auto h = [](char c) { return c <= '9' ? c - '0' : (c | 0x20) - 'a' + 10; };
        v.push_back(static_cast<u8>(h(s[i]) << 4 | h(s[i + 1])));
    }
    return v;
}
u32 sched_obj = 0;  // the game scheduler, once seen
SchedInfo sched_info[2] = {};  // channels 0 and 1, for the latest update

// debugging: lists the scheduled objects (class name from RTTI, channel, slot, update function)
std::string class_name(u32 obj) {
    u32 vt = g::rd32(obj), ti = vt ? g::rd32(vt - 4) : 0, n = ti ? g::rd32(ti + 4) : 0;
    std::string s;
    for (u32 i = 0; n && i < 80; i++) {
        char c = static_cast<char>(g::rd8(n + i));
        if (!c) break;
        s += c;
    }
    return s;
}

void dump_scheduled(u32 sched) {
    u32 vt = g::rd32(sched), step = g::rd32(vt + 8) & ~1u;
    // step: ... ldr r3, [pc, #0x2c] at +0x28, add r3, pc at +0x2e; r0 = [r3] is the object list owner
    u32 lit = g::rd32(step + 0x58), mgr = g::rd32(lit + step + 0x32);
    LOGI("objs: scheduler %08x, step %08x, object list %08x", sched, step, mgr);
    auto walk = [&](u32 head, u32 ch_slot, const char* kind) {
        int k = 0;
        auto ok = [](u32 a) { return a >= g::NULL_GUARD && a < 0xf0000000u; };
        if (!ok(head)) return;
        for (u32 node = g::rd32(head); ok(node) && node != head && k < 400; node = g::rd32(node), k++) {
            u32 obj = g::rd32(node + 8);
            if (!ok(obj) || !ok(g::rd32(obj))) continue;
            u32 ovt = g::rd32(obj);
            u32 ch = static_cast<u32>(call_guest(g::rd32(ovt + ch_slot), {obj}));
            LOGI("objs: %s %08x %-28s vt %08x channel %u slot %u active %u update %08x", kind, obj, class_name(obj).c_str(),
                 ovt, ch, g::rd32(obj + 0x10), g::rd8(obj + 0x18), g::rd32(ovt + (ch_slot == 0 ? 0xc : 0x24)));
        }
    };
    if (mgr) walk(g::rd32(mgr + 0x10), 0, "object");
    // step ends with "bl <registry getter>" at +0x42, then tail-calls the registry's per-channel update
    u32 getter = thumb_call_target(step + 0x42);
    u32 reg = getter ? static_cast<u32>(call_guest(getter, {})) : 0;
    LOGI("objs: listener registry %08x (getter %08x)", reg, getter);
    if (reg >= g::NULL_GUARD) walk(g::rd32(reg + 0xc), 0x18, "listener");
}
}  // namespace

std::atomic<int> g_patch_regions_pending{0};

// The engine's main heap, which holds the game's data: the game's settings make it 46,000,000 bytes ([S3E] MemSize),
// which Kino almost fills, and Mule Kick and the extra voice lines took it over the edge (an allocation failed while
// the level loaded). It gets 16 MB more: in the heap setup, "add r3, r3, #3" (rounding MemSize up to a multiple of
// 4; it is one) becomes "add r3, r3, #0x1000000".
void patches_on_engine(u32 bias) {
    constexpr u32 AT = 0x6ff04;
    constexpr u32 CODE[] = {0xe59d3048, 0xe2833003, 0xe3c33003, 0xe5883008};  // ldr r3,[sp,#0x48]; add; bic; str
    for (u32 i = 0; i < 4; ++i)
        if (g::rd32(bias + AT - 4 + 4 * i) != CODE[i]) {
            LOGW("engine heap: unexpected code at 0x%x; left at the game's size", AT);
            return;
        }
    g::wr32(bias + AT, 0xe2833401);
    invalidate_code(bias + AT, 4);
    LOGI("engine heap: the game's size + 16 MB");
}

void patches_note_exec_region(u32 start, u32 len) {
    if (len < (1u << 20)) return;  // the game image is several MB; skip small executable buffers
    std::lock_guard lk(region_lock);
    regions.emplace_back(start, start + len);
    g_patch_regions_pending = static_cast<int>(regions.size());
}

void patches_on_fetch(u32 addr) {
    std::pair<u32, u32> r{0, 0};
    {
        std::lock_guard lk(region_lock);
        for (size_t i = 0; i < regions.size(); i++) {
            if (addr >= regions[i].first && addr < regions[i].second) {
                r = regions[i];
                regions.erase(regions.begin() + static_cast<long>(i));
                break;
            }
        }
        g_patch_regions_pending = static_cast<int>(regions.size());
    }
    if (r.second) {
        bool first = !image_lo;
        if (first) image_lo = r.first, image_hi = r.second;
        patches_on_code(r.first, r.second);
        if (first) smooth_init(image_lo, image_hi), menu_init(image_lo), codpoints_set_image(image_lo), pad_set_image(image_lo), lan_set_image(image_lo), mulekick_set_image(image_lo), vox_set_image(image_lo), voxev_set_image(image_lo), dogs_set_image(image_lo), doubletap_set_image(image_lo), dive_set_image(image_lo), flame_set_image(image_lo);
    }
}

// Remote players' glides, stretched to their 30 fps length (see SIG_REMOTE_UPDATE).
struct RemoteGlide {
    u32 obj = 0;
    u64 last_ns = 0;
    float frame_ms = 1000.f / 30;  // this player's updates (= frames) lately
    u8 pending = 0;                // glides the previous update started: 1 position, 2 yaw, 4 pitch
};
static RemoteGlide remote_glides[4];

static u32 glide_frames(u32 orig, float frame_ms) {
    float n = static_cast<float>(orig) * (1000.f / 30) / frame_ms;
    return n <= static_cast<float>(orig) ? orig : static_cast<u32>(std::min(n + 0.5f, 1000.f));
}

static void remote_glide(u32 obj) {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    u64 now = static_cast<u64>(ts.tv_sec) * 1000000000ull + static_cast<u64>(ts.tv_nsec);
    RemoteGlide* rg = nullptr;
    for (RemoteGlide& e : remote_glides)
        if (e.obj == obj) rg = &e;
    if (!rg) {  // a player not seen before takes the slot used longest ago
        rg = &remote_glides[0];
        for (RemoteGlide& e : remote_glides)
            if (e.last_ns < rg->last_ns) rg = &e;
        *rg = RemoteGlide{};
        rg->obj = obj;
    }
    if (rg->last_ns) {
        float ms = static_cast<float>(now - rg->last_ns) / 1e6f;
        if (ms > 0.2f && ms < 250.f) rg->frame_ms += (ms - rg->frame_ms) * 0.1f;
    }
    rg->last_ns = now;
    // The previous update started these glides (each is stepped first thing in an update, so they are untouched)
    if ((rg->pending & 1) && g::rd32(obj + 0x150) == 8) {
        u32 n = glide_frames(8, rg->frame_ms);
        g::wr32(obj + 0x150, n);
        for (u32 i = 0; i < 3; i++) {
            float from = g::rdf(obj + 0x12c + 4 * i), to = g::rdf(obj + 0x138 + 4 * i);
            g::wrf(obj + 0x144 + 4 * i, (to - from) / static_cast<float>(n));
        }
    }
    for (u32 k = 0; k < 2; k++) {
        u32 a = obj + (k ? 0x164 : 0x154);
        if (!(rg->pending & (2 << k)) || g::rd32(a + 0xc) != 5) continue;
        u32 n = glide_frames(5, rg->frame_ms);
        g::wr32(a + 0xc, n);
        g::wrf(a + 8, (g::rdf(a + 4) - g::rdf(a)) / static_cast<float>(n));
    }
    // dirty flags of the network values this update reads: position, yaw, pitch
    rg->pending = static_cast<u8>(((g::rd8(obj + 0xc8) & 2) ? 1 : 0) | ((g::rd8(obj + 0xe0) & 2) ? 2 : 0) |
                                  ((g::rd8(obj + 0xf0) & 2) ? 4 : 0));
}

// What other players see a co-op player hold. Every weapon names the animation set it is held with in third person
// (mpArmsAsm). The game's data gives the M14, the FN FAL and the China Lake (and their upgrades, EPC and China Beach)
// the pistols' set, so other players held those rifles like a pistol. They get the set of the other rifles instead
// (the automatic rifles, SMGs and LMGs). The new set's idle shows at once, as for any weapon switch.
constexpr u32 ARMS_PISTOL = 0x0ebe5480, ARMS_RIFLE = 0x1f9e1a37;
constexpr u32 RIFLES_IN_PISTOL_SET[] = {
    0x0b888917,  // m14
    0xfdc9932b,  // fn_fal
    0x0b886f5d,  // epc (FN FAL upgraded)
    0x934d4665,  // chinalake
    0xfc43251b,  // chinabeach (China Lake upgraded)
};

static u32 remote_arms(u32 weapon, u32 arms) {
    if (arms == ARMS_PISTOL && std::find(std::begin(RIFLES_IN_PISTOL_SET), std::end(RIFLES_IN_PISTOL_SET), weapon) !=
                                   std::end(RIFLES_IN_PISTOL_SET))
        return ARMS_RIFLE;
    return arms;
}

void patches_on_code(u32 start, u32 end) {
    if (start < g::NULL_GUARD || end <= start || end - start > 0x10000000) return;
    for (Patch& p : patches) {
        if (p.addr) continue;
        std::vector<u8> sig = unhex(p.sig);
        if (end - start < sig.size()) continue;
        const u8* base = g::mem + start;
        const u8* hit = static_cast<const u8*>(memmem(base, end - start, sig.data(), sig.size()));
        if (!hit) continue;
        p.addr = start + static_cast<u32>(hit - base);
        if (p.svc == HOOK_SCHED) {
            u32 update = thumb_call_target(p.addr + SCHED_UPDATE_BL_AT);
            sched_timer = update ? thumb_call_target((update & ~1u) + SCHED_TIMER_BLX_IN_UPDATE) : 0;
            LOGI("game scheduler update at 0x%08x, timer 0x%08x", update, sched_timer);
            // The update runs at most 4 ticks (64 ms) and drops the rest, so below ~15 fps the game slowed down.
            // Up to 63 ticks keeps it at normal speed down to about 1 frame a second.
            u32 cap = update ? (update & ~1u) + SCHED_TICK_CAP_AT : 0;
            if (cap && g::rd32(cap) == SCHED_TICK_CAP_OLD) {
                g::wr32(cap, SCHED_TICK_CAP_NEW);
                invalidate_code(cap, 4);
                LOGI("game scheduler: up to 63 ticks per update");
            }
        }
        if (p.svc == HOOK_NEAREST) {
            // "ldr r3, [pc, #imm8*4]; add r3, pc" at the start of the signature
            u32 lit = ((p.addr + 4) & ~3u) + (g::rd16(p.addr) & 0xffu) * 4;
            players_global = g::rd32(lit) + p.addr + 6;
        }
        p.orig = g::rd16(p.addr + p.at);
        g::wr16(p.addr + p.at, static_cast<u16>(0xdf00 | p.svc));  // Thumb svc #imm8
        invalidate_code(p.addr + p.at, 2);
        LOGI("patched %s at 0x%08x", p.name, p.addr);
    }
}

bool patches_svc(GuestThread& t, u32 svc) {
    auto& r = t.regs();
    switch (svc) {
    case HOOK_SETFPS: {  // r0 = clock, r5 = min fps, r2 = max fps; the replaced instruction was "mov r1, r2"
        clock_obj = r[0];
        u32 min = r[5], max = r[2];
        // In a level the game asks for fixed 33 ms ticks (30, 30) and blends some objects between them,
        // but not the player's view. Ticking every frame instead keeps the longest tick at 1000/min ms.
        // The clock's lockstep flag (+0x2f: exactly one tick per update, whatever the time) is left alone.
        bool lockstep = g::rd8(r[0] + 0x2f) != 0;
        r[1] = lockstep ? max : TICK_MAX_FPS;
        LOGI("game clock SetFPS(%u, %u) from 0x%08x%s -> max %u ticks/s", min, max, r[14], lockstep ? " (lockstep)" : "", r[1]);
        return true;
    }
    case HOOK_UNIVERSE: {  // replaced "ldr r2, [r3, #0x14]": r2 = setting object, its int value at +16
        static int logged = 0;
        u32 real = g::rd32(r[3] + 0x14);
        if (!logged++) LOGI("NetworkUniverseFPS is %d", real ? static_cast<int>(g::rd32(real + 16)) : -1);
        r[2] = real;
        return true;
    }
    case HOOK_SCHED: {  // replaced "mov r0, r6"
        u32 sched = r[6];
        r[0] = sched;
        u32 table = g::rd32(sched + 0x10), n = g::rd32(sched + 0x14);
        if (!sched_timer || !table || n == 0 || n > 16) return true;
        // The game's scheduler steps every channel in fixed 16 ms steps; the player (channel 1, two slots) moves
        // every other step, about 31 times a second, and moves a fixed distance per update. Its timing is left as
        // the game made it (stepping more often made the player faster); the view is smoothed at render time.
        sched_obj = sched;
        {
            // which ticks this update will run and the time left over, for the view smoothing
            timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            u64 at_ns = static_cast<u64>(ts.tv_sec) * 1000000000ull + static_cast<u64>(ts.tv_nsec);
            u64 now = call_guest(sched_timer, {}), last = g::rd64(sched + 8);
            u64 elapsed = now >= last ? now - last : 0;
            for (u32 i = 0; i < 2 && i < n; i++) {
                u32 e = table + 0x28 * i;
                u64 period = g::rd64(e + 16), acc = g::rd64(e + 24) + elapsed;
                SchedInfo& si = sched_info[i];
                si.ticks = period ? static_cast<u32>(std::min<u64>(4, acc / period)) : 0;
                si.first_slot = g::rd32(e);
                si.slots = g::rd32(e + 4);
                si.period_ms = static_cast<u32>(period);
                si.left_ms = static_cast<u32>(std::min<u64>(acc - si.ticks * period, 1000));
                si.at_ns = at_ns;
                si.seq++;
            }
        }
        static u32 calls = 0;
        if (++calls % 30 == 1) {
            bool on = settings().zombies_ignore;
            if (on != zombies_ignore.exchange(on)) LOGI("zombies ignore you: %s", on ? "on" : "off");
        }
        if (calls == 3000) {
            char v[PROP_VALUE_MAX] = {};
            if (__system_property_get("debug.boz64.objs", v) > 0 && v[0] == '1') dump_scheduled(sched);
        }
        return true;
    }
    case HOOK_REMOTE_UPDATE:  // replaced "mov r4, r0"; r0 = the CRemotePlayer
        r[4] = r[0];
        remote_glide(r[0]);
        return true;
    case HOOK_REMOTE_POSE:  // replaced "mov r4, r0"; r0 = the CRemotePlayer, r1 = the set, r7 = the weapon
        r[4] = r[0];
        r[1] = remote_arms(r[7], r[1]);
        return true;
    case HOOK_NEAREST: {  // replaced "mov r0, r9": r9 = the candidate player
        if (ignore_target(r[9])) {
            r[15] = patch_by_svc(svc)->addr + NEAREST_DROP_AT;
            return true;
        }
        r[0] = r[9];
        return true;
    }
    case HOOK_TARGET_AFTER_PUSHW: {  // after "push.w {list}" at the function's start; r1 = the new target
        const Patch* p = patch_by_svc(svc);
        u32 list = g::rd16(p->addr + 2), sp = r[13];
        u32 n = static_cast<u32>(__builtin_popcount(list & 0x1fff));
        if ((list & 0x4000) && ignore_target(r[1]) && (g::rd32(sp + 4 * n) & 1)) {
            // return to the (Thumb) caller: pop the saved registers and lr into pc
            for (u32 i = 0; i < 13; i++)
                if (list & (1u << i)) r[i] = g::rd32(sp), sp += 4;
            r[15] = g::rd32(sp) & ~1u;
            r[13] = sp + 4;
            return true;
        }
        u16 m = p->orig;  // "mov rd, rm" (high-register form)
        r[((m >> 4) & 8) | (m & 7)] = r[(m >> 3) & 15];
        return true;
    }
    case HOOK_TARGET_DOG:
    case HOOK_TARGET_AI3:
    case HOOK_TARGET_AI4: {  // replaced the function's first instruction "push {list}"; r1 = the new target
        if (ignore_target(r[1]) && (r[14] & 1)) {
            r[15] = r[14] & ~1u;  // return to the (Thumb) caller
            return true;
        }
        u16 push = patch_by_svc(svc)->orig;
        u32 sp = r[13] - 4 * static_cast<u32>(__builtin_popcount(push & 0x1ff));
        r[13] = sp;
        for (u32 i = 0; i < 8; i++)
            if (push & (1u << i)) g::wr32(sp, r[i]), sp += 4;
        if (push & 0x100) g::wr32(sp, r[14]);
        return true;
    }
    case HOOK_SENS_ENTER:  // replaced "add sp, #8"
        menu_on_enter();
        r[13] += 8;
        return true;
    case HOOK_INGAME_UPDATE:  // replaced "mov r4, r0"; r0 = the in-game state
        r[4] = r[0];
        pad_on_ingame(r[0]);
        return true;
    case HOOK_DO_UPDATE:  // replaced "mov r4, r0"; r0 = Dead Ops Arcade's in-game state
        r[4] = r[0];
        pad_on_dead_ops(r[0]);
        return true;
    case HOOK_PLAYER_LOOK:  // replaced "mov r4, r0"; r0 = the local player's CPlayerController
        r[4] = r[0];
        pad_on_look(r[0]);
        voxev_on_player(r[0]);
        dive_on_player(r[0]);
        flame_on_player(r[0]);
        return true;
    case HOOK_PROMPT:  // replaced "mov r4, r0"; r0 = the HUD's prompt element
        r[4] = r[0];
        pad_on_prompt(r[0]);
        return true;
    case HOOK_NET_UPDATE:  // replaced "mov r6, r0"; r0 = ms since the last update
        r[6] = r[0];
        lan_poll();
        return true;
    case HOOK_SCORE_EARNED:  // replaced "add r3, r6"; r6 = the points the local player just earned
        r[3] += r[6];
        codpoints_on_earned(static_cast<s32>(r[6]));
        return true;
    case HOOK_SENS_CALIBRATE:  // replaced "movs r2, #0" (the flags it sets are not read); r1 = the button
        if (menu_on_button(r[1])) {  // our checkbox: return to the caller without running the slot
            r[15] = r[14] & ~1u;
            t.set_cpsr((t.cpsr() & ~0x20u) | ((r[14] & 1) ? 0x20u : 0u));
            return true;
        }
        r[2] = 0;
        return true;
    case HOOK_SENS_EXIT:  // replaced "mov r4, r0"
        menu_on_exit();
        r[4] = r[0];
        return true;
    case HOOK_PANEL_SIZE:  // replaced "ldr r2, [sp, #0xc]"; r6 = the panel
        menu_fit_panel(r[6], r[13] + 8, r[4]);
        r[2] = g::rd32(r[13] + 0xc);
        return true;
    case HOOK_PANEL_LAID_OUT:  // replaced "add sp, #0x38"
        menu_after_layout(r[6]);
        r[13] += 0x38;
        return true;
    case HOOK_SENS_Y: {  // replaced the slot's first instruction "push {r3, lr}"; r1 = the slider
        if (menu_on_slider(r[1])) {  // one of ours: return to the caller (ARM or Thumb) without running the slot
            r[15] = r[14] & ~1u;
            t.set_cpsr((t.cpsr() & ~0x20u) | ((r[14] & 1) ? 0x20u : 0u));
            return true;
        }
        u32 sp = r[13] - 8;
        g::wr32(sp, r[3]);
        g::wr32(sp + 4, r[14]);
        r[13] = sp;
        return true;
    }
    case HOOK_PHYS: {  // replaced "ldr r1, =1/30.f": the step length in seconds, as float bits in r1
        // the real time since the previous step (the game passed 1/30 s whatever its tick rate)
        static u64 last_ns = 0;
        timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        u64 ns = static_cast<u64>(ts.tv_sec) * 1000000000ull + static_cast<u64>(ts.tv_nsec);
        float secs = last_ns ? (ns - last_ns) / 1e9f : 1 / 30.f;
        last_ns = ns;
        if (secs < 0.001f) secs = 0.001f;
        if (secs > 0.1f) secs = 0.1f;
        memcpy(&r[1], &secs, 4);
        return true;
    }
    }
    return mulekick_svc(t, svc) || vox_svc(t, svc) || voxev_svc(t, svc) || dogs_svc(t, svc) || doubletap_svc(t, svc) || dive_svc(t, svc) || flame_svc(t, svc);
}

static bool dump_file(const char* name, u32 lo, u32 hi) {
    const char* dirs[] = {"/sdcard/Android/data/com.activision.boz64/files", "/sdcard/Android/obb/com.activision.boz64"};
    for (const char* d : dirs) {
        mkdir(d, 0777);
        std::string path = std::string(d) + "/" + name;
        FILE* f = fopen(path.c_str(), "wb");
        if (!f) continue;
        fwrite(g::mem + lo, 1, hi - lo, f);
        fclose(f);
        LOGI("dumped guest 0x%08x-0x%08x to %s", lo, hi, path.c_str());
        return true;
    }
    LOGW("could not write %s", name);
    return false;
}

// debugging (debug.boz64.owner=<hex>,<hex>...): which object (by RTTI class) holds each address
static void find_owners(const char* list) {
    auto valid_vt = [](u32 vt) {
        if (vt < image_lo || vt >= image_hi || (vt & 3)) return false;
        u32 ti = g::rd32(vt - 4);
        if (ti < image_lo || ti >= image_hi) return false;
        u32 n = g::rd32(ti + 4);
        return n >= image_lo && n < image_hi && g::rd8(n) >= '0' && g::rd8(n) <= '9';
    };
    for (const char* p = list; *p;) {
        u32 a = static_cast<u32>(strtoul(p, const_cast<char**>(&p), 16));
        if (*p == ',') p++;
        bool found = false;
        for (u32 back = 0; back <= 0x800 && a >= back + g::NULL_GUARD; back += 4) {
            u32 obj = a - back;
            if (valid_vt(g::rd32(obj))) {
                LOGI("owner: %08x is %s+0x%x (object %08x)", a, class_name(obj).c_str(), back, obj);
                found = true;
                break;
            }
        }
        if (!found) LOGI("owner: %08x: no object within 0x800 bytes", a);
    }
}

void patches_stats() {
    {
        char v[PROP_VALUE_MAX] = {};
        if (__system_property_get("debug.boz64.owner", v) > 0 && v[0] && v[0] != '0') find_owners(v);
    }
    static u32 last_ticks = 0;
    static bool dumped = false;
    char v[PROP_VALUE_MAX] = {};
    if (!dumped && image_lo && __system_property_get("debug.boz64.dump", v) > 0 && v[0] == '1') {
        dumped = true;
        char name[64];
        snprintf(name, sizeof name, "image_%08x.bin", image_lo);
        dump_file(name, image_lo, image_hi);
    }
    if (!clock_obj) return;
    u32 c = clock_obj, ticks = g::rd32(c + 0x34);
    u32 del = g::rd32(c + 0x3c), interp = g::rd32(c + 0x38);
    u32 dvt = del ? g::rd32(del) : 0;
    LOGI("stats: clock ticks %u (+%u), min period %u max tick %u tick %u scaled %u acc %d flags %u%u%u%u delegate %08x vt %08x vt[8] %08x vt[12] %08x interp %08x",
         ticks, ticks - last_ticks, g::rd32(c + 0x10), g::rd32(c + 0x14), g::rd32(c + 0x18), g::rd32(c + 0x1c),
         static_cast<int>(g::rd32(c + 0x24)), g::rd8(c + 0x2c), g::rd8(c + 0x2d), g::rd8(c + 0x2e), g::rd8(c + 0x2f), del, dvt,
         dvt ? g::rd32(dvt + 8) : 0, dvt ? g::rd32(dvt + 12) : 0, interp);
    last_ticks = ticks;
}


void patches_sched_last(SchedInfo* out) {
    for (int i = 0; i < 2; i++) out[i] = sched_info[i];
}
