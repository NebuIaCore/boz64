// Mule Kick (from Black Ops PC): an 8th perk machine (perk type 7) whose perk lets the player carry a third gun.
// The machines themselves are level data (tools/mulekick.py); this is the game code side.
#pragma once
#include "common.h"

struct GuestThread;

void mulekick_set_image(u32 image_lo);              // patches the game once its code is loaded
bool mulekick_svc(GuestThread& t, u32 svc);         // false if svc is not one of ours
