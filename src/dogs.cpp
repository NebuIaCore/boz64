// Kino der Toten's hellhound rounds as Black Ops PC plays them (_zombiemode_ai_dogs.gsc); tools/dogpatch.py does
// the level data (dogs per round, alive at once, spawn pacing, health of the first three dog rounds, bite damage).
//
// When: BO1's first dog round is round 5, 6 or 7 (randomintrange(5, 8)); each later one comes 4 or 5 rounds after
// the last. The phone game's wave manager (FUN_0266e974, when a wave ends, on the host) instead makes the next wave
// a dog wave when d = (waves since the last dog wave) reaches the dog wave's maximum (5), or reaches its minimum (3)
// and a roll beats its chance (0% for the first, 50% after): the first dog round was always round 6, later ones
// 4, 5 or 6 rounds apart. Here the next wave is a dog wave when d + 1 equals a gap drawn once per dog cycle, 5-7 for
// the first and 4-5 after.
//
// Where: BO1 spawns a dog 400 to 1000 units from its target player. The phone game picks among the spawn points
// within 5000 units of it (one of the 4 closest, so often right next to the player); for dogs only those 400 to 1000
// BO1 units away (980 to 2450 on the phone's Kino, 2.45 times larger) are candidates. If none is, the game falls back
// to the closest one, as it does when none is in range at all.
//
// Health: the phone game gives dog round 3 on 990, 1980, 2970, ... (the third dog wave's health times its
// multiplier times the dog round's number past it). BO1: 400, 900, 1300, then 1600 for every later dog round. The
// data has 400, 900, 1300; past the third dog round the health is 1600.
#include "dogs.h"

#include <cstring>
#include <ctime>

#include "cpu.h"

namespace {
constexpr u32 DUMP_BASE = 0x02454000;  // addresses below are in the game image as dumped at this base
u32 image = 0;
u32 at(u32 a) { return a - DUMP_BASE + image; }
bool valid(u32 a) { return a >= g::NULL_GUARD && a < g::ALLOC_END && !(a & 3); }

// the wave manager's speciality wave state (FUN_0266e974's first argument)
constexpr u32 SW_LAST = 0x0, SW_COUNT = 0x4;  // the wave the last dog round was (0: none yet); dog rounds so far + 1
// CWaveSpeciality
constexpr u32 WAVE_MAX_GAP = 0xdc;   // m_maxNormalWavesInterval
constexpr u32 WAVE_HELLHOUND = 0xe8;  // m_HellhoundWave (byte)
constexpr float DOG_HEALTH_CAP = 1600;  // BO1's dog health from the 4th dog round on

// spawn types (name hashes)
constexpr u32 TYPE_DOG = 0xb0c61324, TYPE_FLAMING_DOG = 0x0fd12ae1;  // "zombie_dog", "zombie_dog_flaming"
constexpr float SCALE = 2.45f;  // Kino: phone units per BO1 unit
constexpr float SPAWN_MIN = 400 * SCALE, SPAWN_MAX = 1000 * SCALE;

enum : u32 { HOOK_SCHED_ENTRY = 0xb8, HOOK_SCHED, HOOK_HEALTH, HOOK_SPAWN_POINT };
struct Hook { u32 addr; u16 orig; u32 svc; };
constexpr Hook HOOKS[] = {
    // FUN_0266e974 (is the next wave a speciality wave?): "mov r10, r0" (r0 = the speciality wave state)
    {0x0266e978, 0x4682, HOOK_SCHED_ENTRY},
    // ... the speciality wave picked (r5), d (r8): "ldr.w r6, [r5, #0xdc]" (first half) before the gap checks
    {0x0266ea36, 0xf8d5, HOOK_SCHED},
    // FUN_0266df44 (speciality wave setup): "vmul.f32 s15, s15, s14" (first half): base health times the round
    // number past the last wave in the data (r0 = the wave)
    {0x0266df76, 0xee67, HOOK_HEALTH},
    // FUN_02654530 (pick a spawn point for a type, *r6): a point within range (s17 = its distance squared): "ldr r3,
    // [r4, #0x10]" (the point; 0 = not a candidate)
    {0x026546c6, 0x6923, HOOK_SPAWN_POINT},
};
constexpr u32 SCHED_DECIDED = 0x0266ea5e;  // "cmp r6, #0; beq not special"
constexpr u32 SCHED_NEXT = 0x0266ea3a, HEALTH_NEXT = 0x0266df7a;

u32 state = 0;          // the speciality wave state FUN_0266e974 was called with
u32 cycle_last = ~0u;   // SW_LAST when the gap was drawn
u32 gap = 0;            // waves from the last dog round to the next
u32 seed = 1;
u32 rnd() {  // xorshift32
    seed ^= seed << 13;
    seed ^= seed >> 17;
    seed ^= seed << 5;
    return seed;
}

float sreg(GuestThread& t, int i) {
    float f;
    memcpy(&f, &t.ext_regs()[i], 4);
    return f;
}
void set_sreg(GuestThread& t, int i, float f) { memcpy(&t.ext_regs()[i], &f, 4); }
}  // namespace

void dogs_set_image(u32 image_lo) {
    image = image_lo;
    for (const Hook& h : HOOKS) {
        if (g::rd16(at(h.addr)) != h.orig) {
            LOGW("hellhounds: unexpected code at 0x%08x; BO1's dog rounds are off", h.addr);
            return;
        }
    }
    seed = static_cast<u32>(time(nullptr)) | 1;
    for (const Hook& h : HOOKS) {
        g::wr16(at(h.addr), static_cast<u16>(0xdf00 | h.svc));  // Thumb svc #imm8
        invalidate_code(at(h.addr), 2);
    }
    LOGI("hellhounds: BO1's dog rounds are on");
}

bool dogs_svc(GuestThread& t, u32 svc) {
    auto& r = t.regs();
    switch (svc) {
    case HOOK_SCHED_ENTRY:
        r[10] = r[0];
        state = r[0];
        return true;
    case HOOK_SCHED: {  // r5 = the speciality wave, r8 = waves since the last one
        if (!g::rd8(r[5] + WAVE_HELLHOUND) || !valid(state)) {
            r[6] = g::rd32(r[5] + WAVE_MAX_GAP);  // the replaced instruction
            r[15] = at(SCHED_NEXT);
            return true;
        }
        u32 last = g::rd32(state + SW_LAST), count = g::rd32(state + SW_COUNT);
        if (last != cycle_last || !gap) {
            cycle_last = last;
            gap = count <= 1 ? 5 + rnd() % 3 : 4 + rnd() % 2;
            LOGI("hellhounds: the next dog round is %u waves after wave %u", gap, last);
        }
        r[6] = r[8] + 1 == gap ? 1 : 0;
        r[15] = at(SCHED_DECIDED);
        return true;
    }
    case HOOK_SPAWN_POINT: {
        r[3] = g::rd32(r[4] + 0x10);  // the replaced instruction
        u32 type = g::rd32(r[6]);
        float d2 = sreg(t, 17);
        if ((type == TYPE_DOG || type == TYPE_FLAMING_DOG) && (d2 < SPAWN_MIN * SPAWN_MIN || d2 > SPAWN_MAX * SPAWN_MAX))
            r[3] = 0;
        return true;
    }
    case HOOK_HEALTH: {  // s15 = base health (times the multiplier), s14 = rounds past the last wave in the data
        float health = sreg(t, 15), n = sreg(t, 14);
        if (valid(r[0]) && g::rd8(r[0] + WAVE_HELLHOUND))
            health = n <= 1 ? health : DOG_HEALTH_CAP;
        else
            health *= n;  // the replaced instruction
        set_sreg(t, 15, health);
        r[15] = at(HEALTH_NEXT);
        return true;
    }
    }
    return false;
}
