// Black Ops PC's other character voice lines, played the way BO1's scripts (_zombiemode_audio.gsc and friends) play
// them. The phone game only voices a few events (kill streaks, low ammo, reloads, going down, reviving, the box
// moving); BO1 also has a line for each perk bought, each power-up grabbed, being unable to afford something, the
// perk limit, some kills and the Ray Gun and Thundergun pickups. tools/voxpatch.py adds these takes to the four
// characters' banks (Kino's and Ascension's crew) as CSoundVariations named "boz64_<line>"; this plays them.
//
// BO1's rules, kept here:
//   - one line at a time: a line due while one is playing is dropped; a player in last stand says nothing
//   - takes rotate: a random one that has not played since all of them last did
//   - perk bought: 1.5 s after the perk is granted (perk_vox)
//   - power-up grabbed: 4.5 to 5.5 s after the grab, by the player who grabbed it (powerup_vo); Death Machine,
//     the perk bottle and Lightning Bolt have no line (no BO1 takes for this crew)
//   - cannot afford: a perk (take 0), a wall gun (take 1), its ammo (take 0), the mystery box (take 2)
//   - a perk machine refusing a 5th perk: "sigh" (Ascension's takes)
//   - kills (player_zombie_kill_vox, get_mod_type, get_mod_chance), at most one kill line per 2 s:
//       Ray Gun from over 400 units: kill_ray 75%;  head shot from over 400 units: kill_headshot 99%;
//       any kill within 64 units: kill_close 15%;  other gun kills: kill_streak's takes 10%;
//       during Insta-Kill: kill_insta 10%;  Thundergun: kill_thunder 30%;  a hellhound: kill_hellhound 20%
//     (BO1 units: the phone's maps are 2.45 times larger.) Melee kills are counted as gun kills (the game does not
//     say which it was here), and the gas crawlers keep the phone's own line.
//   - picking up the Ray Gun or the Thundergun (their weapon data has no pickup line): wpck_raygun, wpck_thunder
// Only the local player's events are voiced. Insta-Kill counts as on for 30 s after the local player grabs it.
// debug.boz64.voxlog=1: the log says what each event would play and why.
#include "voxev.h"

#include <sys/system_properties.h>

#include <cstdio>
#include <ctime>
#include <initializer_list>

#include "cpu.h"

namespace {
constexpr u32 DUMP_BASE = 0x02454000;  // addresses below are in the game image as dumped at this base
u32 image = 0;
u32 at(u32 a) { return a - DUMP_BASE + image; }
bool valid(u32 a) { return a >= g::NULL_GUARD && a < g::ALLOC_END && !(a & 3); }
u32 call(u32 fn, std::initializer_list<u32> args) { return static_cast<u32>(call_guest(at(fn) | 1, args)); }

constexpr u64 MS = 1000000ull;
u64 now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000000000ull + static_cast<u64>(ts.tv_nsec);
}

// game functions
constexpr u32 FN_VARIATION_BY_NAME = 0x02566ddc;  // (name, 2): the CSoundVariation of that name, or 0
constexpr u32 FN_PLAY_TAKE = 0x02653464;          // (variation, index, position or 0): plays one of its specs
constexpr u32 FN_PLAYER_OF = 0x0262cb88;          // (entity): its CPlayerController (only the local player has one)
constexpr u32 FN_POINTS = 0x0264f2cc;             // (score manager): the local player's points
constexpr u32 FN_UPDATE_NODE = 0x0251c138;        // (node): brings its world position (+0x5c) up to date
// CSoundVariation
constexpr u32 VAR_SPECS = 0x54, VAR_SPECS_END = 0x58;
// CPlayerController
constexpr u32 PC_WEAPONS = 0x88;   // its CWeaponManager
constexpr u32 PC_PERKS = 0xa0;     // its CPerkManager
constexpr u32 PC_POS = 0xdc;       // floats x, y, z
constexpr u32 PC_POSTURE = 0x121;  // byte: 8 = last stand
constexpr u32 WM_CURRENT = 0x1e0;  // CWeaponManager: the gun in hand
constexpr u32 PM_BITS = 0x58;      // CPerkManager: owned perks, bit = 1 << type
constexpr u32 GUN_MODEL = 0x5c;    // a gun: its model's name hash
constexpr u32 GUN_BUY_LINE = 0x254;  // a gun: CPlayerWeapon.playerBuySound (0: none)
// CAIControllerZombie
constexpr u32 Z_NODE = 0xc4, Z_HIT = 0xe4, Z_FROZEN = 0xb7, Z_NO_SCORE = 0x10d, Z_KILLER = 0x138;
constexpr u32 HIT_HEAD = 0x58;  // byte in Z_HIT's object: the killing hit was a head shot
// CAIControllerHellhound
constexpr u32 DOG_NO_SCORE = 0xf9, DOG_KILLER = 0x11c;
// CPerk (a perk machine), CWeaponChalkOutline (a wall gun), CMysteryBox
constexpr u32 PERK_COST = 0x84;
constexpr u32 WALL_COST = 0x84, WALL_KIND = 0x70;
constexpr u32 BOX_COST = 0x80, BOX_KIND = 0x78;

// model name hashes (djb2 of "raygun", "thundergun"; the upgraded guns use the same models)
constexpr u32 MODEL_RAYGUN = 0x18ea84db;
constexpr u32 MODEL_THUNDERGUN = 0x5b49c309;

enum Line : int {
    PERK = 0,         // + perk type 0..7
    POWERUP = 8,      // + pickup type 0..5
    NOMONEY_0 = 14, NOMONEY_1, NOMONEY_2, SIGH,
    KILL_HEADSHOT, KILL_INSTA, KILL_RAY, KILL_HELLHOUND, KILL_THUNDER, KILL_CLOSE, KILL_BULLET,
    WPCK_RAYGUN, WPCK_THUNDER, LINES
};
const char* const LINE_NAMES[LINES] = {
    "boz64_perk_0", "boz64_perk_1", "boz64_perk_2", "boz64_perk_3",  // Deadshot, Double Tap, Juggernog, Stamin-Up
    "boz64_perk_4", "boz64_perk_5", "boz64_perk_6", "boz64_perk_7",  // PHD Flopper, Quick Revive, Speed Cola, Mule Kick
    "boz64_powerup_0", "boz64_powerup_1", "boz64_powerup_2",  // Double Points, Insta-Kill, Max Ammo
    "boz64_powerup_3", "boz64_powerup_4", "boz64_powerup_5",  // Nuke, Carpenter, Fire Sale
    "boz64_nomoney_0", "boz64_nomoney_1", "boz64_nomoney_2", "boz64_sigh",
    "boz64_kill_headshot", "boz64_kill_insta", "boz64_kill_ray", "boz64_kill_hellhound", "boz64_kill_thunder",
    "low_health",   // the phone's own kill_close takes (it plays them when the player's health is low)
    "kill_streak",  // the phone's own; BO1 uses the kill streak takes for the 10% of gun kills too
    "boz64_wpck_raygun", "boz64_wpck_thunder",
};
u32 line_name[LINES];  // guest copies
u32 used[LINES];       // takes played since the line's last full rotation

// hooks
enum : u32 {
    HOOK_PICKUP = 0xb0, HOOK_PERK_DENY, HOOK_WALL_DENY, HOOK_AMMO_DENY, HOOK_BOX_DENY, HOOK_ZOMBIE_DEATH, HOOK_DOG_DEATH,
    HOOK_BUY_LINE,
};
struct Hook { u32 addr; u16 orig; u32 svc; };
constexpr Hook HOOKS[] = {
    // CPickupBase (collide): the local player touched a power-up: "mov r0, r4" (the power-up) before its Collect
    {0x02626c90, 0x4620, HOOK_PICKUP},
    // CPerk (use): refused (cannot afford, or mulekick.cpp's perk limit): "ldr r5, [pc, #..]"
    {0x026240da, 0x4d23, HOOK_PERK_DENY},
    // CWeaponChalkOutline (use): a new gun costs more than the player has: "ldr r3, [pc, #..]" (the free-buy check)
    {0x026752ae, 0x4b66, HOOK_WALL_DENY},
    // the same, for its ammo (r5 = the ammo's cost)
    {0x02675306, 0x4b53, HOOK_AMMO_DENY},
    // CMysteryBox (use): costs more than the player has: "ldr r3, [pc, #..]" (the free-buy check)
    {0x02613b56, 0x4b84, HOOK_BOX_DENY},
    // CAIControllerZombie, entering the death state, after scoring the kill: "mov r0, r4" (the zombie)
    {0x0256ec1e, 0x4620, HOOK_ZOMBIE_DEATH},
    // CAIControllerHellhound, entering the death state, after scoring the kill: "mov r0, r4" (the dog)
    {0x0255e69a, 0x4620, HOOK_DOG_DEATH},
    // a gun's "bought" sounds: "cmp r1, #0" on its playerBuySound (r4 = the gun) before "beq end"
    {0x02638e96, 0x2900, HOOK_BUY_LINE},
};
constexpr u32 BUY_LINE_PLAY = 0x02638e9a, BUY_LINE_END = 0x02638f34;

bool logging = false;  // debug.boz64.voxlog
u32 seed = 1;
u32 rnd() {  // xorshift32
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}
bool chance(int percent) { return static_cast<int>(rnd() % 99 + 1) < percent; }  // BO1: chance > randomintrange(1, 100)

// the local player's state
u32 player = 0;          // its CPlayerController (from its view update)
u64 player_since = 0;    // when it appeared (a new game or a loaded one)
u32 last_perks = 0;
u64 busy_until = 0;      // one of our lines is playing
u64 kill_wait_until = 0;  // BO1's force_wait_on_kill_line
u64 insta_until = 0;
struct Pending { int line; u64 due; };
Pending pending[4];
int npending = 0;

constexpr u64 LINE_TIME = 3500 * MS;  // how long a line keeps others quiet (BO1 waits for the sound to end)
constexpr float SCALE = 2.45f;         // phone map units per BO1 unit
constexpr float FAR = 400 * SCALE, CLOSE = 64 * SCALE;

bool in_last_stand(u32 pc) { return (g::rd8(pc + PC_POSTURE) & 8) != 0; }

bool play(int line, u64 now, const char* why) {
    if (now < busy_until || !valid(player) || in_last_stand(player)) {
        if (logging) LOGI("voice: %s (%s) dropped: %s", LINE_NAMES[line], why, now < busy_until ? "busy" : "down");
        return false;
    }
    u32 var = call(FN_VARIATION_BY_NAME, {line_name[line], 2});
    if (!valid(var)) {
        if (logging) LOGI("voice: %s (%s): no such variation", LINE_NAMES[line], why);
        return false;
    }
    u32 b = g::rd32(var + VAR_SPECS), n = (g::rd32(var + VAR_SPECS_END) - b) / 4;
    if (!valid(b) || n == 0 || n > 32) return false;
    u32 all = n == 32 ? ~0u : (1u << n) - 1;
    if ((used[line] & all) == all) used[line] = 0;
    u32 left = n - static_cast<u32>(__builtin_popcount(used[line] & all)), k = rnd() % left;
    u32 take = 0;
    for (; take < n; ++take)
        if (!(used[line] & (1u << take)) && k-- == 0) break;
    used[line] |= 1u << take;
    call(FN_PLAY_TAKE, {var, take, 0});
    busy_until = now + LINE_TIME;
    if (logging) LOGI("voice: %s take %u of %u (%s)", LINE_NAMES[line], take, n, why);
    return true;
}

void queue(int line, u64 due) {
    if (npending < static_cast<int>(sizeof(pending) / sizeof(pending[0]))) pending[npending++] = {line, due};
}

float dist2(u32 node, u32 pc) {
    call(FN_UPDATE_NODE, {node});
    float dx = g::rdf(node + 0x5c) - g::rdf(pc + PC_POS), dy = g::rdf(node + 0x60) - g::rdf(pc + PC_POS + 4),
          dz = g::rdf(node + 0x64) - g::rdf(pc + PC_POS + 8);
    return dx * dx + dy * dy + dz * dz;
}

u32 gun_model(u32 pc) {
    u32 wm = g::rd32(pc + PC_WEAPONS);
    u32 gun = valid(wm) ? g::rd32(wm + WM_CURRENT) : 0;
    return valid(gun) ? g::rd32(gun + GUN_MODEL) : 0;
}

// the local player's CPlayerController if it is the killer
u32 killer(u32 entity) {
    if (!valid(entity) || !valid(player)) return 0;
    u32 pc = call(FN_PLAYER_OF, {entity});
    return pc == player ? pc : 0;
}

void on_zombie_death(u32 z, u64 now) {
    if (g::rd8(z + Z_FROZEN) || g::rd8(z + Z_NO_SCORE)) return;
    u32 pc = killer(g::rd32(z + Z_KILLER));
    u32 node = g::rd32(z + Z_NODE);
    if (!pc || !valid(node) || now < kill_wait_until) return;
    u32 hit = g::rd32(z + Z_HIT);
    bool head = valid(hit) && g::rd8(hit + HIT_HEAD);
    bool insta = now < insta_until;
    u32 model = gun_model(pc);
    float d2 = dist2(node, pc);
    int line = -1, percent = 0;
    if (model == MODEL_THUNDERGUN) {
        line = KILL_THUNDER, percent = 30;
    } else if (model == MODEL_RAYGUN) {  // a projectile: not a bullet kill
        if (d2 > FAR * FAR) line = insta ? KILL_INSTA : KILL_RAY, percent = insta ? 10 : 75;
        else if (!insta && d2 < CLOSE * CLOSE) line = KILL_CLOSE, percent = 15;
    } else if (insta) {
        line = KILL_INSTA, percent = 10;
    } else if (head && d2 > FAR * FAR) {
        line = KILL_HEADSHOT, percent = 99;
    } else if (d2 < CLOSE * CLOSE) {
        line = KILL_CLOSE, percent = 15;
    } else {
        line = KILL_BULLET, percent = 10;
    }
    if (logging)
        LOGI("voice: kill: head %d, insta %d, gun %08x, %.0f units -> %s %d%%", head, insta, model, __builtin_sqrtf(d2),
             line >= 0 ? LINE_NAMES[line] : "nothing", percent);
    if (line < 0 || !chance(percent)) return;
    kill_wait_until = now + 2000 * MS;
    play(line, now, "kill");
}

void on_dog_death(u32 dog, u64 now) {
    if (g::rd8(dog + DOG_NO_SCORE) || !killer(g::rd32(dog + DOG_KILLER))) return;
    if (rnd() % 100 >= 80) play(KILL_HELLHOUND, now, "hellhound");
}

// a "can the player afford it" check that is about to run: denied unless free buys apply (as the game checks next)
bool denied(u32 addr, u32 lit_value, u32 obj, u32 cost, u32 kind_off) {
    u32 free_buys = g::rd32(lit_value + at(addr + 2) + 4);  // "add r3, pc; ldr r3, [r3]"
    return !(valid(free_buys) && g::rd32(free_buys + 0x10) >= cost && g::rd32(obj + kind_off) == 3);
}

const Hook* hook_of(u32 svc) {
    for (const Hook& h : HOOKS)
        if (h.svc == svc) return &h;
    return nullptr;
}

// emulates the hook's "ldr rN, [pc, #imm]"; returns the value loaded
u32 ldr_literal(GuestThread& t, const Hook& h) {
    u32 lit = ((h.addr + 4) & ~3u) + (h.orig & 0xff) * 4;
    u32 v = g::rd32(at(lit));
    t.regs()[(h.orig >> 8) & 7] = v;
    return v;
}

}  // namespace

void voxev_set_image(u32 image_lo) {
    image = image_lo;
    for (const Hook& h : HOOKS) {
        if (g::rd16(at(h.addr)) != h.orig) {
            LOGW("voice events: unexpected code at 0x%08x; BO1's other voice lines are off", h.addr);
            return;
        }
    }
    char v[PROP_VALUE_MAX] = {};
    logging = __system_property_get("debug.boz64.voxlog", v) > 0 && v[0] == '1';
    seed = static_cast<u32>(time(nullptr)) | 1;
    for (int i = 0; i < LINES; ++i) line_name[i] = g::data_str(LINE_NAMES[i]);
    for (const Hook& h : HOOKS) {
        g::wr16(at(h.addr), static_cast<u16>(0xdf00 | h.svc));  // Thumb svc #imm8
        invalidate_code(at(h.addr), 2);
    }
    LOGI("voice events: BO1's other voice lines are on");
}

void voxev_on_player(u32 pc) {
    if (!image || !valid(pc)) return;
    u64 now = now_ns();
    u32 pm = g::rd32(pc + PC_PERKS);
    u32 perks = valid(pm) ? g::rd32(pm + PM_BITS) & 0xff : 0;
    if (pc != player) {  // a new game, or a loaded one
        player = pc, player_since = now, last_perks = perks;
        busy_until = kill_wait_until = insta_until = 0, npending = 0;
    }
    if (now - player_since < 3000 * MS) last_perks = perks;  // a loaded game's perks come back: no lines
    if (u32 got = perks & ~last_perks) queue(PERK + __builtin_ctz(got), now + 1500 * MS);
    last_perks = perks;
    for (int i = 0; i < npending;) {
        if (now < pending[i].due) {
            ++i;
            continue;
        }
        play(pending[i].line, now, "after a delay");
        pending[i] = pending[--npending];
    }
}

bool voxev_svc(GuestThread& t, u32 svc) {
    const Hook* h = hook_of(svc);
    if (!h) return false;
    auto& r = t.regs();
    u64 now = now_ns();
    switch (svc) {
    case HOOK_PICKUP: {  // r4 = the power-up (the local player touched it)
        r[0] = r[4];
        u32 vt = g::rd32(r[4]);
        u32 type = valid(vt) ? static_cast<u32>(call_guest(g::rd32(vt + 0x64), {r[4]})) : 99;  // its type
        if (type == 1) insta_until = now + 30000 * MS;
        if (logging) LOGI("voice: power-up %u grabbed", type);
        if (type <= 5) queue(POWERUP + static_cast<int>(type), now + (4500 + rnd() % 1001) * MS);
        return true;
    }
    case HOOK_PERK_DENY: {  // r4 = the machine
        ldr_literal(t, *h);
        u32 scores = g::rd32(g::rd32(at(0x02624144)) + at(0x02624004) + 4);  // as the machine reads it
        bool poor = valid(scores) && call(FN_POINTS, {scores}) < g::rd32(r[4] + PERK_COST);
        play(poor ? NOMONEY_0 : SIGH, now, poor ? "perk: cannot afford" : "perk: limit");
        return true;
    }
    case HOOK_WALL_DENY:  // r4 = the wall gun
        if (denied(h->addr, ldr_literal(t, *h), r[4], g::rd32(r[4] + WALL_COST), WALL_KIND))
            play(NOMONEY_1, now, "wall gun: cannot afford");
        return true;
    case HOOK_AMMO_DENY:  // r4 = the wall gun, r5 = its ammo's cost
        if (denied(h->addr, ldr_literal(t, *h), r[4], r[5], WALL_KIND)) play(NOMONEY_0, now, "ammo: cannot afford");
        return true;
    case HOOK_BOX_DENY:  // r4 = the box
        if (denied(h->addr, ldr_literal(t, *h), r[4], g::rd32(r[4] + BOX_COST), BOX_KIND))
            play(NOMONEY_2, now, "box: cannot afford");
        return true;
    case HOOK_ZOMBIE_DEATH:
        r[0] = r[4];
        on_zombie_death(r[4], now);
        return true;
    case HOOK_DOG_DEATH:
        r[0] = r[4];
        on_dog_death(r[4], now);
        return true;
    case HOOK_BUY_LINE: {  // r4 = the gun, r1 = its pickup line
        if (r[1]) {
            r[15] = at(BUY_LINE_PLAY);
            return true;
        }
        u32 model = g::rd32(r[4] + GUN_MODEL);
        if (model == MODEL_RAYGUN) play(WPCK_RAYGUN, now, "Ray Gun picked up");
        if (model == MODEL_THUNDERGUN) play(WPCK_THUNDER, now, "Thundergun picked up");
        r[15] = at(BUY_LINE_END);
        return true;
    }
    }
    return false;
}

static_assert(sizeof(LINE_NAMES) / sizeof(LINE_NAMES[0]) == LINES, "a name per line");
