// World at War's flamethrower (tools/wawguns.py adds the gun): heat instead of ammo, a flame, and a heat gauge. Also the
// Wunderwaffe's chain lightning as Black Ops (2010) runs it.
#pragma once
#include "common.h"

struct GuestThread;

void flame_set_image(u32 image_lo);  // once the game's code is loaded (patches the shot and the chain's spawn)
bool flame_svc(GuestThread& t, u32 svc);  // false if svc is not one of ours
void flame_on_player(u32 controller);  // the local player's update is starting (every frame; game thread)
void flame_world_end(const float* pmv);  // the 3D world is drawn, with this projection x view (render thread)
void flame_draw();                   // before the frame is shown (render thread): the heat gauge
