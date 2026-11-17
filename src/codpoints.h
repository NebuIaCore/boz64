// 1 COD point for every 1000 points the player earns in a game. Points spent (doors, weapons, perks) still count:
// it goes by the points earned, as the game itself totals them for the local player.
#pragma once
#include <string>

#include "common.h"

void codpoints_init(const std::string& files_dir);  // loads the progress toward the next COD point
void codpoints_set_image(u32 image_lo);             // the game image is loaded (addresses are relative to it)
void codpoints_on_earned(s32 points);               // the local player just earned this many points
