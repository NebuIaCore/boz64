// Black Ops PC's other character voice lines (perks, power-ups, "no money", kill and pickup lines), played when and
// as often as BO1's scripts play them. The takes themselves are data (tools/voxpatch.py's EVENTS).
#pragma once
#include "common.h"

struct GuestThread;

void voxev_set_image(u32 image_lo);         // patches the game once its code is loaded
void voxev_on_player(u32 player);           // the local player's view update (its CPlayerController); game thread
bool voxev_svc(GuestThread& t, u32 svc);    // false if svc is not one of ours
