// Mule Kick, Black Ops PC's 8th perk: the player can carry a third gun.
//
// The game has 7 perks (types 0-6: Deadshot, Double Tap, Juggernog, Stamin-Up, PHD Flopper, Quick Revive, Speed
// Cola) and uses 7 to mean "none". Mule Kick is type 7. A machine with type 7 (level data, tools/mulekick.py) is
// bought and drunk like any other: the owned perks are a 32-bit mask (CPerkManager+0x58, synced in co-op as is).
// What had to change in the game's code:
//   - the drink: the bottle comes from a 7-entry table (type 7 read past it and crashed); Mule Kick uses Speed
//     Cola's bottle. The player's "no drink pending" value (CPlayerController+0x130) was 7; it is now 0xff.
//   - the HUD icon: the icon switches stop at 6, so type 7 gets "Perk_Mule_Kick_Image" (an IwUI element in the
//     ingame resources; without it the game shows no icon) when granted, hidden, refreshed or flashed.
//   - the per-perk icon flash timers are sized 7; they are sized 8 (and their loops run to 8).
//   - the third gun: CWeaponManager keeps 5 slots (current, holstered, knife, grenade, special). The third gun is
//     kept here, outside the game's data. Buying a gun (mystery box, wall) with both slots full keeps the current
//     gun as the third instead of dropping it; swapping goes current -> holstered -> third -> current; "has this
//     gun", "is its ammo full" and Max Ammo count it. Going down, a new loadout, or losing the perk drops it.
//   - saved games keep it: the weapon manager saves its guns as a list (CWeaponManager vtable 23, the save message:
//     {hash, clip, reserve, alt clip, alt reserve} per gun) and loading puts entry i in slot i for the first 5. The
//     third gun is saved as entry 5 (empty entries pad the list to 5 first), which the game's own loading never reads
//     (so old builds and the phone are fine with such saves); loading takes it back from there. A restored third gun
//     is kept for a while even if the perks are not loaded yet.
//   - the perk limit: as in Black Ops PC, a player can own 4 perks (Mule Kick counts). A machine refuses a 5th the way
//     it refuses a player who cannot afford it (the "buy_fail" sound). Perks given for free are not limited.
// debug.boz64.mulekick_icon=1: the HUD shows Mule Kick's icon from the start (to see it without buying the perk).
#include "mulekick.h"

#include <sys/system_properties.h>

#include <cstring>
#include <ctime>

#include "cpu.h"

namespace {
constexpr u32 DUMP_BASE = 0x02454000;  // addresses below are in the game image as dumped at this base
u32 image = 0;
u32 at(u32 a) { return a - DUMP_BASE + image; }

constexpr u32 PERK_MULE_KICK = 7;
constexpr u32 PERK_LIMIT = 4;
constexpr u32 BOTTLE_OF_SPEED_COLA = 6;
u32 icon_name = 0;  // guest copy of "Perk_Mule_Kick_Image"
bool show_icon = false;  // debug.boz64.mulekick_icon

// game functions
constexpr u32 FN_SET_PERK_ICON = 0x02625174;        // CPerkManager::SetPerkIcon(this, name, show)
constexpr u32 FN_UPDATE_ICON_SPACING = 0x026252e4;  // CPerkManager::UpdateIconSpacing(this)
constexpr u32 FN_REFILL_GUN = 0x02676c48;           // refills a gun's ammo (gun, update the HUD)
constexpr u32 FN_GUN_BY_HASH = 0x02676e3e | 1;      // CWeaponManager (this, weapon hash): its gun of that kind, or 0
constexpr u32 FN_SAVED_GUN_INIT = 0x02676cc0 | 1;   // a saved-gun entry (0x18 bytes): set up, all fields 0
constexpr u32 FN_SAVED_GUN_PUSH = 0x02679cdc | 1;   // (std::vector of saved guns, entry): push_back
// CWeaponManager
constexpr u32 WM_PERKS = 0x88;  // its CPerkManager
constexpr u32 WM_CURRENT = 0x1e0;  // the 5 slots, 4 bytes each
// a saved gun (the save list's entries)
constexpr u32 SAVED_SIZE = 0x18, SAVED_HASH = 4, SAVED_CLIP = 8, SAVED_RESERVE = 0xc, SAVED_ALT_CLIP = 0x10,
              SAVED_ALT_RESERVE = 0x14;
constexpr u32 SAVED_THIRD = 5;  // the third gun's entry (the game loads entries 0-4 only)
// CWeaponManager vtable 23 (its messages), saving: the list is a std::vector at [sp + 0x14], the entry being built at
// [sp + 0x20]; loading: the list read back is at [sp + 0x14]
constexpr u32 SP_SAVED = 0x14, SP_ENTRY = 0x20;
constexpr u64 RESTORED_GRACE = 10000000000ull;  // ns a restored third gun stays while the perks are not loaded yet
// CPerkManager
constexpr u32 PM_BITS = 0x58;  // owned perks, bit = 1 << type
// CPerk (a perk machine)
constexpr u32 PERK_MANAGER = 0xb8;  // the perk manager it checks and grants to
// a gun
constexpr u32 GUN_AMMO = 0x30, GUN_AMMO_MAX = 0x84;
constexpr u32 GUN_CLIP = 0x2c, GUN_ALT = 0x3c, GUN_ALT_ON = 0x40, GUN_HASH = 0x5c;
// Where the guns that can become a third come from: SetWeaponInSlot's caller (GiveWeaponByHash tail-calls it).
// Others give back guns the player already had (revive, Death Machine, Lightning Bolt) or Pack-a-Punch's upgrade.
constexpr u32 BOUGHT_FROM[] = {
    0x02613ba0,  // CMysteryBox
    0x02675332,  // CWeaponChalkOutline (wall guns)
};

enum : u32 {
    HOOK_GRANT_ICON = 0xa0, HOOK_HIDE_ICONS, HOOK_REFRESH_ICONS, HOOK_BOTTLE, HOOK_SLOT0_REPLACE, HOOK_SWAP,
    HOOK_HAS_WEAPON, HOOK_AMMO_FULL, HOOK_MAX_AMMO, HOOK_LAST_STAND, HOOK_RESET_LOADOUT, HOOK_WM_INIT, HOOK_PERK_LIMIT,
    HOOK_SAVE, HOOK_LOAD,
};
struct Hook { u32 addr; u16 orig; u32 svc; };
constexpr Hook HOOKS[] = {
    // CPerkManager::GrantPerk: "cmp r6, #6" before the switch on the type that shows its icon
    {0x02625382, 0x2e06, HOOK_GRANT_ICON},
    // CPerkManager::HideAllPerkIcons: "movs r3, #0" after hiding the 7 icons
    {0x0262528e, 0x2300, HOOK_HIDE_ICONS},
    // CPerkManager::RefreshAllPerkIcons: "mov r0, r4" before the last perk's (Deadshot's) icon
    {0x025ef10c, 0x4620, HOOK_REFRESH_ICONS},
    // CWeaponManager::SetPerkBottle(this, show, type): "add r3, pc" (the bottle table) before it is indexed by type
    {0x02679b24, 0x447b, HOOK_BOTTLE},
    // CWeaponManager::SetWeaponInSlot, slot 0 taken by another gun: "cbnz r3, drop" (r3 = the holstered gun)
    {0x0267933a, 0xb923, HOOK_SLOT0_REPLACE},
    // CWeaponManager::SwapWeapons: "mov r0, r4" before the current gun (r3) is holstered and r1 taken out
    {0x0267927c, 0x4620, HOOK_SWAP},
    // CWeaponManager::HasWeapon: "movs r0, #0" when the gun is in no slot
    {0x02677d80, 0x2000, HOOK_HAS_WEAPON},
    // CWeaponManager::IsAmmoFull: "movs r0, #0" when the gun is in no slot
    {0x02677dde, 0x2000, HOOK_AMMO_FULL},
    // CWeaponManager::MaxAmmoRefillAll: "add r3, pc" after refilling the 5 slots
    {0x02678024, 0x447b, HOOK_MAX_AMMO},
    // "mov r4, r0" near the start of: CWeaponManager::EnterLastStand, ResetToStartLoadout, and its reset (vtable 19)
    {0x0267967c, 0x4604, HOOK_LAST_STAND},
    {0x026798e2, 0x4604, HOOK_RESET_LOADOUT},
    {0x026788d8, 0x4604, HOOK_WM_INIT},
    // CPerk (vtable 24, buying from a machine): "cmp r0, r3" (the player's points, the cost) after the power, owned and
    // available checks
    {0x02624010, 0x4298, HOOK_PERK_LIMIT},
    // CWeaponManager vtable 23, saving: "add r0, sp, #8" before the list of guns is written (both of its loadouts)
    {0x0267bc46, 0xa802, HOOK_SAVE},
    // ... loading: "ldr r3, [sp, #0xc]" after the slots are filled from the list read back
    {0x0267bda0, 0x9b03, HOOK_LOAD},
};
struct Edit { u32 addr; u16 orig, now; };
constexpr Edit EDITS[] = {
    // CPerkManager (vtable 22): the flash timers and fade directions, "movs r1, #7" -> 8, and their setup loop
    {0x02625f74, 0x2107, 0x2108},
    {0x02625f80, 0x2107, 0x2108},
    {0x02625f86, 0x2107, 0x2108},
    {0x02625fa4, 0x2d07, 0x2d08},  // cmp r5, #7
    {0x02625526, 0x2c07, 0x2c08},  // CPerkManager::UpdateIconFlash: cmp r4, #7
    {0x02626002, 0x2e07, 0x2e08},  // loading a saved game: the owned perks' icons, cmp r6, #7
    // CPlayerController's "no drink pending" (+0x130): "movs r3, #7" -> #0xff in its constructor and when a drink ends
    {0x0262fa70, 0x2307, 0x23ff},
    {0x02636798, 0x2307, 0x23ff},
};
// PerkType_GetHudImageName: the literal of its default case ("ldr r0, =lit; add r0, pc" at 0x02624c0c, pc 0x02624c12)
constexpr u32 HUD_NAME_LIT = 0x02624c3c, HUD_NAME_LIT_ORIG = 0x001c4d01, HUD_NAME_PC = 0x02624c12;

// The third gun of each weapon manager (one per local player).
struct Third { u32 wm = 0, gun = 0; u64 restored_until = 0; };
Third thirds[4];

u64 now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000000000ull + ts.tv_nsec;
}

bool has_mule_kick(u32 wm) {
    u32 pm = g::rd32(wm + WM_PERKS);
    return pm >= g::NULL_GUARD && (g::rd32(pm + PM_BITS) & (1u << PERK_MULE_KICK));
}

void set_third(u32 wm, u32 gun, u64 restored_until = 0) {
    Third* e = nullptr;
    for (Third& t : thirds)
        if (t.wm == wm) e = &t;
    if (!e && !gun) return;
    if (!e)
        for (Third& t : thirds)
            if (!e && !t.gun) e = &t;
    if (!e) e = &thirds[0];
    if (e->gun != gun) LOGI("Mule Kick: third gun of %08x %08x -> %08x", wm, e->wm == wm ? e->gun : 0, gun);
    e->wm = wm;
    e->gun = gun;
    e->restored_until = restored_until;
}

u32 third(u32 wm) {  // the third gun, dropped once the perk is gone
    for (Third& t : thirds) {
        if (t.wm != wm || !t.gun) continue;
        if (has_mule_kick(wm)) {
            t.restored_until = 0;
        } else if (!t.restored_until || now_ns() > t.restored_until) {  // (a loaded game's perks may come later)
            set_third(wm, 0);
            return 0;
        }
        return t.gun;
    }
    return 0;
}

// saving: appends the third gun to the list as entry SAVED_THIRD (after empty entries, if the list is shorter)
void save_third(u32 wm, u32 sp) {
    u32 x = third(wm);
    if (!x) return;
    u32 list = sp + SP_SAVED, entry = sp + SP_ENTRY;
    auto count = [&] { return (g::rd32(list + 4) - g::rd32(list)) / SAVED_SIZE; };
    while (count() < SAVED_THIRD) {
        call_guest(at(FN_SAVED_GUN_INIT), {entry});
        call_guest(at(FN_SAVED_GUN_PUSH), {list, entry});
    }
    if (count() != SAVED_THIRD) return;
    call_guest(at(FN_SAVED_GUN_INIT), {entry});
    g::wr32(entry + SAVED_HASH, g::rd32(x + GUN_HASH));
    g::wr32(entry + SAVED_CLIP, g::rd32(x + GUN_CLIP));
    g::wr32(entry + SAVED_RESERVE, g::rd32(x + GUN_AMMO));
    if (u32 alt = g::rd32(x + GUN_ALT)) {
        g::wr32(entry + SAVED_ALT_CLIP, g::rd32(alt + GUN_CLIP));
        g::wr32(entry + SAVED_ALT_RESERVE, g::rd32(alt + GUN_AMMO));
    }
    call_guest(at(FN_SAVED_GUN_PUSH), {list, entry});
    LOGI("Mule Kick: saved the third gun (%08x)", g::rd32(x + GUN_HASH));
}

// loading: takes the third gun back from entry SAVED_THIRD (the slots are filled already)
void load_third(u32 wm, u32 sp) {
    set_third(wm, 0);
    u32 list = sp + SP_SAVED;
    u32 begin = g::rd32(list), end = g::rd32(list + 4);
    if ((end - begin) / SAVED_SIZE <= SAVED_THIRD) return;
    u32 e = begin + SAVED_THIRD * SAVED_SIZE;
    u32 hash = g::rd32(e + SAVED_HASH);
    u32 gun = hash ? static_cast<u32>(call_guest(at(FN_GUN_BY_HASH), {wm, hash})) : 0;
    if (gun < g::NULL_GUARD) return;
    for (u32 i = 0; i < 5; i++)
        if (g::rd32(wm + WM_CURRENT + 4 * i) == gun) return;  // already in a slot
    g::wr32(gun + GUN_CLIP, g::rd32(e + SAVED_CLIP));
    g::wr32(gun + GUN_AMMO, g::rd32(e + SAVED_RESERVE));
    if (u32 alt = g::rd32(gun + GUN_ALT)) {
        g::wr32(alt + GUN_CLIP, g::rd32(e + SAVED_ALT_CLIP));
        g::wr32(alt + GUN_AMMO, g::rd32(e + SAVED_ALT_RESERVE));
    }
    g::wr8(gun + GUN_ALT_ON, 0);  // as when a gun is holstered
    set_third(wm, gun, now_ns() + RESTORED_GRACE);
    LOGI("Mule Kick: loaded the third gun (%08x)", hash);
}
}  // namespace

void mulekick_set_image(u32 image_lo) {
    image = image_lo;
    for (const Hook& h : HOOKS)
        if (g::rd16(at(h.addr)) != h.orig) {
            LOGW("Mule Kick: unexpected code at 0x%08x (%04x, expected %04x); Mule Kick is off", h.addr,
                 g::rd16(at(h.addr)), h.orig);
            return;
        }
    for (const Edit& e : EDITS)
        if (g::rd16(at(e.addr)) != e.orig) {
            LOGW("Mule Kick: unexpected code at 0x%08x (%04x, expected %04x); Mule Kick is off", e.addr,
                 g::rd16(at(e.addr)), e.orig);
            return;
        }
    if (g::rd32(at(HUD_NAME_LIT)) != HUD_NAME_LIT_ORIG) {
        LOGW("Mule Kick: unexpected HUD icon name literal; Mule Kick is off");
        return;
    }
    icon_name = g::data_str("Perk_Mule_Kick_Image");
    char v[PROP_VALUE_MAX] = {};
    show_icon = __system_property_get("debug.boz64.mulekick_icon", v) > 0 && v[0] == '1';
    for (const Hook& h : HOOKS) {
        g::wr16(at(h.addr), static_cast<u16>(0xdf00 | h.svc));  // Thumb svc #imm8
        invalidate_code(at(h.addr), 2);
    }
    for (const Edit& e : EDITS) {
        g::wr16(at(e.addr), e.now);
        invalidate_code(at(e.addr), 2);
    }
    g::wr32(at(HUD_NAME_LIT), icon_name - at(HUD_NAME_PC));
    invalidate_code(at(HUD_NAME_LIT), 4);
    LOGI("Mule Kick: perk type 7, the third gun and the %u-perk limit are on", PERK_LIMIT);
}

bool mulekick_svc(GuestThread& t, u32 svc) {
    auto& r = t.regs();
    switch (svc) {
    case HOOK_GRANT_ICON:  // replaced "cmp r6, #6": r4 = the perk manager, r5 = 1 << type, r6 = the type
        if (r[6] == PERK_MULE_KICK) {
            r[0] = r[4];
            r[1] = icon_name;
            r[2] = (g::rd32(r[4] + PM_BITS) & r[5]) ? 1 : 0;
            r[15] = at(0x026253ea);  // bl SetPerkIcon, then the icon spacing, as for the others
        } else {
            r[15] = at(r[6] > 6 ? 0x026253f8 : 0x02625386);  // "bhi return" : "tbb [pc, r6]"
        }
        return true;
    case HOOK_HIDE_ICONS:  // replaced "movs r3, #0": r4 = the perk manager
        call_guest(at(FN_SET_PERK_ICON) | 1, {r[4], icon_name, show_icon ? 1u : 0u});
        if (show_icon) call_guest(at(FN_UPDATE_ICON_SPACING) | 1, {r[4]});
        r[3] = 0;
        return true;
    case HOOK_REFRESH_ICONS:  // replaced "mov r0, r4": r4 = the perk manager
        if (g::rd32(r[4] + PM_BITS) & (1u << PERK_MULE_KICK)) {
            call_guest(at(FN_SET_PERK_ICON) | 1, {r[4], icon_name, 1});
            call_guest(at(FN_UPDATE_ICON_SPACING) | 1, {r[4]});
        }
        r[0] = r[4];
        return true;
    case HOOK_BOTTLE:  // replaced "add r3, pc" at 0x02679b24: r2 = the perk type
        r[3] += at(0x02679b24) + 4;
        if (r[2] == PERK_MULE_KICK) r[2] = BOTTLE_OF_SPEED_COLA;
        return true;
    case HOOK_SLOT0_REPLACE: {  // replaced "cbnz r3, drop": r4 = the weapon manager, r3 = the holstered gun, r5 = the new one
        u32 wm = r[4];
        if (!r[3]) {  // a free slot: the current gun is holstered
            r[15] = at(0x0267933c);
            return true;
        }
        u32 caller = g::rd32(r[13] + 28) & ~1u;  // pushed lr (push {r0-r2, r4-r7, lr} at the start)
        bool bought = false;
        for (u32 a : BOUGHT_FROM) bought |= caller == at(a);
        u32 x = third(wm);
        if (bought && has_mule_kick(wm) && (!x || x == r[5])) {
            set_third(wm, g::rd32(wm + WM_CURRENT));  // the current gun becomes the third
            r[15] = at(0x02679362);
        } else {
            r[15] = at(0x02679346);  // the current gun is dropped
        }
        return true;
    }
    case HOOK_SWAP:  // replaced "mov r0, r4": r4 = the weapon manager, r3 = the current gun (to be holstered)
        r[0] = r[4];
        if (u32 x = third(r[4])) {  // the third gun is holstered instead and the current one becomes the third
            set_third(r[4], r[3]);
            r[3] = x;
        }
        return true;
    case HOOK_HAS_WEAPON:  // replaced "movs r0, #0": r4 = the weapon manager, r0 = the gun
        r[0] = (r[0] && r[0] == third(r[4])) ? 1 : 0;
        return true;
    case HOOK_AMMO_FULL:  // replaced "movs r0, #0": r4 = the weapon manager, r2 = the gun
        r[0] = (r[2] && r[2] == third(r[4])) ? (g::rd32(r[2] + GUN_AMMO) == g::rd32(r[2] + GUN_AMMO_MAX)) : 0;
        return true;
    case HOOK_MAX_AMMO:  // replaced "add r3, pc" at 0x02678024: r4 = the weapon manager
        r[3] += at(0x02678024) + 4;
        if (u32 x = third(r[4])) call_guest(at(FN_REFILL_GUN) | 1, {x, 0});
        return true;
    case HOOK_LAST_STAND:
    case HOOK_RESET_LOADOUT:
    case HOOK_WM_INIT:  // replaced "mov r4, r0": r0 = the weapon manager
        r[4] = r[0];
        set_third(r[0], 0);
        return true;
    case HOOK_PERK_LIMIT: {  // replaced "cmp r0, r3; bcs buy": r4 = the machine (CPerk), r0 = points, r3 = cost
        u32 pm = g::rd32(r[4] + PERK_MANAGER);
        u32 owned = pm >= g::NULL_GUARD ? g::rd32(pm + PM_BITS) & 0xff : 0;
        if (static_cast<u32>(__builtin_popcount(owned)) >= PERK_LIMIT)
            r[15] = at(0x026240da);  // refused, as when the player cannot afford it
        else
            r[15] = at(r[0] >= r[3] ? 0x02624026 : 0x02624014);  // "bcs buy", or the next check
        return true;
    }
    case HOOK_SAVE:  // replaced "add r0, sp, #8": r4 = the weapon manager (call_guest keeps the registers)
        save_third(r[4], r[13]);
        r[0] = r[13] + 8;
        return true;
    case HOOK_LOAD:  // replaced "ldr r3, [sp, #0xc]": r4 = the weapon manager
        load_third(r[4], r[13]);
        r[3] = g::rd32(r[13] + 0xc);
        return true;
    }
    return false;
}
