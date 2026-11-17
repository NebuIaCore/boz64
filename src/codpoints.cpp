// 1 COD point for every 1000 points earned. The game's score manager totals the points the local player earns
// (CScoreManager, its "add score" path: patches.cpp hooks it and calls codpoints_on_earned with each amount).
// COD points are added through the game's own call, which also saves the balance to the game's COD points file,
// as when it takes COD points for a purchase. Progress toward the next COD point (0-999) is kept in
// <app files dir>/boz64_codpoints.cfg, so points left over at the end of a game count toward the next.
#include "codpoints.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cpu.h"

namespace {
constexpr u32 DUMP_BASE = 0x02454000;        // addresses below are in the game image as dumped at this base
constexpr u32 COD_POINTS = 0x028c031c;       // pointer to the game's COD points object
constexpr u32 COD_BALANCE = 0x10;            // its balance
constexpr u32 FN_COD_ADD = 0x025ed3cc | 1;   // (object, change): changes the balance (not below 0) and saves it
constexpr int POINTS_PER_COD_POINT = 1000;
constexpr int SAVE_EVERY = 100;              // points of progress between writes of the progress file

u32 image = 0;
std::string path;
int progress = 0, saved_progress = 0;

u32 at(u32 a) { return a - DUMP_BASE + image; }
bool valid(u32 a) { return a >= g::NULL_GUARD && a < g::ALLOC_END && !(a & 3); }

void save() {
    if (path.empty()) return;
    std::string tmp = path + ".tmp";
    if (FILE* f = fopen(tmp.c_str(), "w")) {
        fprintf(f, "progress=%d\n", progress);
        fclose(f);
        rename(tmp.c_str(), path.c_str());
        saved_progress = progress;
    }
}
}  // namespace

void codpoints_init(const std::string& files_dir) {
    path = files_dir + "/boz64_codpoints.cfg";
    if (FILE* f = fopen(path.c_str(), "r")) {
        char line[64];
        while (fgets(line, sizeof line, f))
            if (!strncmp(line, "progress=", 9)) progress = atoi(line + 9);
        fclose(f);
    }
    if (progress < 0 || progress >= POINTS_PER_COD_POINT) progress = 0;
    saved_progress = progress;
    LOGI("COD points: %d of %d points toward the next one", progress, POINTS_PER_COD_POINT);
}

void codpoints_set_image(u32 image_lo) { image = image_lo; }

void codpoints_on_earned(s32 points) {
    if (points <= 0 || points > 1000000) return;
    progress += points;
    int award = progress / POINTS_PER_COD_POINT;
    if (award > 0) {
        u32 obj = image ? g::rd32(at(COD_POINTS)) : 0;
        if (valid(obj)) {
            progress %= POINTS_PER_COD_POINT;
            u32 before = g::rd32(obj + COD_BALANCE);
            call_guest(at(FN_COD_ADD), {obj, static_cast<u32>(award)});
            LOGI("COD points: +%d (balance %u -> %u), %d points toward the next", award, before,
                 g::rd32(obj + COD_BALANCE), progress);
        } else {  // not loaded yet: keep the points until it is
            static bool warned = false;
            if (!warned) warned = true, LOGW("COD points: no COD points object (%08x); keeping %d points", obj, progress);
        }
    }
    if (award > 0 || std::abs(progress - saved_progress) >= SAVE_EVERY) save();
}
