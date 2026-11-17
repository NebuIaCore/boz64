// More takes of the voice lines the game plays as one sound.
//
// Most of the characters' voice events are CSoundVariations, which the game picks a take from at random. Some lines
// are a single CIwSoundSpec played by its name hash instead: the explosive kill ("kill_explo"), the mystery box's
// poor weapon ("wpck_crappy") and the weapons' pickup lines (CPlayerWeapon.playerBuySound). tools/voxpatch.py adds,
// next to each of these specs, a CSoundVariation with the same name holding the spec and Black Ops PC's other takes.
// Playing by hash goes through one spec lookup (0x025303e8: GetResHashed(hash, "CIwSoundSpec")); for these hashes
// it now looks for that variation and returns one of its specs (not the one played last time), or the spec itself
// when there is no variation (an unpatched pack, or another map's characters).
// debug.boz64.voxtest=1: once the variations are loaded, the log lists what each line would pick in 8 plays.
#include "vox.h"

#include <sys/system_properties.h>

#include <cstdio>
#include <ctime>

#include "cpu.h"

namespace {
constexpr u32 DUMP_BASE = 0x02454000;  // addresses below are in the game image as dumped at this base
u32 image = 0;
u32 at(u32 a) { return a - DUMP_BASE + image; }

constexpr u32 FN_GET_RES_HASHED = 0x0268e1b4;    // CIwResManager::GetResHashed(this, hash, class name, flags)
constexpr u32 STR_VARIATION = 0x027fd4c9;        // "CSoundVariation"
constexpr u32 STR_SPEC = 0x027f1a03;             // "CIwSoundSpec"
constexpr u32 VAR_SPECS = 0x54, VAR_SPECS_END = 0x58;  // CSoundVariation: its specs (pointers once loaded)

// the spec lookup: "movs r3, #2" before the tail call to GetResHashed (r0 = the manager, r1 = hash, r2 = class)
constexpr u32 HOOK_AT = 0x025303f6;
constexpr u16 HOOK_ORIG = 0x2302;
constexpr u32 HOOK_SPEC_LOOKUP = 0x98;

// the lines with variants (tools/voxpatch.py's SINGLES)
constexpr u32 LINES[] = {
    0xb77bc8b8,  // kill_explo
    0xf9d67c68,  // wpck_crappy
    0x2995494b,  // pickup lines: launcher
    0xd88f6a8d,  //   machine gun
    0xedf5a161,  //   shotgun
    0xea7cd6e0,  //   SMG
    0x1e872cd4,  //   sickle
};
u32 last[sizeof(LINES) / sizeof(LINES[0])];  // the take each line played last
u32 seed = 0;

int line_index(u32 hash) {
    for (int i = 0; i < static_cast<int>(sizeof(LINES) / sizeof(LINES[0])); ++i)
        if (LINES[i] == hash) return i;
    return -1;
}

u32 rnd() {  // xorshift32
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

// one of line i's specs (not the one it played last), or 0 if it has no variation (mgr: the resource manager)
u32 pick(u32 mgr, int i) {
    u32 v = static_cast<u32>(call_guest(at(FN_GET_RES_HASHED) | 1, {mgr, LINES[i], at(STR_VARIATION), 2}));
    if (v < g::NULL_GUARD) return 0;
    u32 b = g::rd32(v + VAR_SPECS), n = (g::rd32(v + VAR_SPECS_END) - b) / 4;
    if (b < g::NULL_GUARD || n == 0 || n > 64) return 0;
    u32 k = rnd() % n;
    if (n > 1 && g::rd32(b + 4 * k) == last[i]) k = (k + 1 + rnd() % (n - 1)) % n;  // not the same take twice
    u32 spec = g::rd32(b + 4 * k);
    if (spec < g::NULL_GUARD) return 0;
    last[i] = spec;
    return spec;
}

bool selftest = false;  // debug.boz64.voxtest

void run_selftest(u32 mgr) {
    for (int i = 0; i < static_cast<int>(sizeof(LINES) / sizeof(LINES[0])); ++i) {
        u32 v = static_cast<u32>(call_guest(at(FN_GET_RES_HASHED) | 1, {mgr, LINES[i], at(STR_VARIATION), 2}));
        if (v < g::NULL_GUARD) {
            if (i == 0) return;  // not loaded yet: try again at the next sound
            LOGI("voice test: %08x has no variation", LINES[i]);
            continue;
        }
        char picks[160] = {};
        int len = 0;
        for (int k = 0; k < 8; ++k) {
            u32 spec = pick(mgr, i);
            len += snprintf(picks + len, sizeof(picks) - len, " %08x", spec ? g::rd32(spec + 4) : 0);
        }
        LOGI("voice test: %08x, %u takes, picks:%s", LINES[i], (g::rd32(v + VAR_SPECS_END) - g::rd32(v + VAR_SPECS)) / 4,
             picks);
    }
    selftest = false;
}

bool same_str(u32 guest, const char* s) {
    for (;; ++guest, ++s) {
        if (g::rd8(guest) != static_cast<u8>(*s)) return false;
        if (!*s) return true;
    }
}
}  // namespace

void vox_set_image(u32 image_lo) {
    image = image_lo;
    if (g::rd16(at(HOOK_AT)) != HOOK_ORIG || !same_str(at(STR_VARIATION), "CSoundVariation") ||
        !same_str(at(STR_SPEC), "CIwSoundSpec")) {
        LOGW("voice lines: unexpected code at 0x%08x; the extra takes of single lines are off", HOOK_AT);
        return;
    }
    seed = static_cast<u32>(time(nullptr)) | 1;
    char v[PROP_VALUE_MAX] = {};
    selftest = __system_property_get("debug.boz64.voxtest", v) > 0 && v[0] == '1';
    g::wr16(at(HOOK_AT), static_cast<u16>(0xdf00 | HOOK_SPEC_LOOKUP));  // Thumb svc #imm8
    invalidate_code(at(HOOK_AT), 2);
    LOGI("voice lines: extra takes of single lines are on");
}

bool vox_svc(GuestThread& t, u32 svc) {
    if (svc != HOOK_SPEC_LOOKUP) return false;
    auto& r = t.regs();
    r[3] = 2;  // the replaced instruction
    if (r[2] != at(STR_SPEC)) return true;
    if (selftest) run_selftest(r[0]);
    int i = line_index(r[1]);
    if (i < 0) return true;
    u32 spec = pick(r[0], i);
    if (!spec) return true;
    // return the spec to the lookup's caller (it was reached with bl, so lr says ARM or Thumb)
    r[0] = spec;
    r[15] = r[14] & ~1u;
    t.set_cpsr((t.cpsr() & ~0x20u) | ((r[14] & 1) ? 0x20u : 0u));
    return true;
}
