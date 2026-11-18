// Double Tap as Black Ops II's Double Tap Root Beer II: on top of the faster fire rate the game already gives (its
// own Double Tap), every shot fires two bullets. Both bullets of a shot follow the same path, so each hit counts
// twice: the bullet hit the game sends (SUBJECT_WEAPON_HIT, built by FUN_0263a8d0 for every entity a bullet or
// shotgun pellet goes through, after body part, penetration and range falloff) carries twice the damage when the
// shooter owns Double Tap. Projectiles (Ray Gun, launchers), the Thundergun and the knife are not bullets and are
// unchanged, as in Black Ops II.
#include "doubletap.h"

#include "cpu.h"

namespace {
constexpr u32 DUMP_BASE = 0x02454000;  // addresses below are in the game image as dumped at this base
u32 image = 0;
u32 at(u32 a) { return a - DUMP_BASE + image; }
bool valid(u32 a) { return a >= g::NULL_GUARD && a < g::ALLOC_END && !(a & 3); }

constexpr u32 GUN_WEAPONS = 0x2ec;  // a gun: its CWeaponManager
constexpr u32 WM_PERKS = 0x88;      // CWeaponManager: its owner's CPerkManager
constexpr u32 PM_BITS = 0x58;       // CPerkManager: owned perks, bit = 1 << type
constexpr u32 DOUBLE_TAP = 1u << 1;

// FUN_0263a8d0(gun, entity hit, ...), the hit message built on its stack: "add r2, sp, #0x20" (the message) before
// it is sent; the gun is at [sp + 0x10], the damage at [sp + 0x24]
constexpr u32 HOOK_AT = 0x0263ab9e;
constexpr u16 HOOK_ORIG = 0xaa08;
constexpr u32 HOOK_BULLET_HIT = 0xbc;
constexpr u32 SP_GUN = 0x10, SP_DAMAGE = 0x24;
bool paused = false;
u32 forced = 0;  // a damage every hit carries meanwhile (src/flame.cpp's Wunderwaffe chain), or 0
bool hooked = false;
}  // namespace

void doubletap_pause(bool p) { paused = p; }

bool doubletap_force_damage(u32 damage) {
    forced = damage;
    return hooked;
}

void doubletap_set_image(u32 image_lo) {
    image = image_lo;
    if (g::rd16(at(HOOK_AT)) != HOOK_ORIG) {
        LOGW("double tap: unexpected code at 0x%08x; Double Tap II is off", HOOK_AT);
        return;
    }
    g::wr16(at(HOOK_AT), static_cast<u16>(0xdf00 | HOOK_BULLET_HIT));  // Thumb svc #imm8
    invalidate_code(at(HOOK_AT), 2);
    hooked = true;
    LOGI("double tap: Double Tap II is on");
}

bool doubletap_svc(GuestThread& t, u32 svc) {
    if (svc != HOOK_BULLET_HIT) return false;
    auto& r = t.regs();
    u32 sp = r[13];
    r[2] = sp + 0x20;  // the replaced instruction
    u32 gun = g::rd32(sp + SP_GUN);
    u32 wm = valid(gun) ? g::rd32(gun + GUN_WEAPONS) : 0;
    u32 pm = valid(wm) ? g::rd32(wm + WM_PERKS) : 0;
    if (forced) g::wr32(sp + SP_DAMAGE, forced);
    else if (!paused && valid(pm) && (g::rd32(pm + PM_BITS) & DOUBLE_TAP)) g::wr32(sp + SP_DAMAGE, g::rd32(sp + SP_DAMAGE) * 2);
    return true;
}
