// More takes of single voice lines: the game's spec lookup picks one from a CSoundVariation of the same name.
// The variations themselves are data (tools/voxpatch.py); this is the game code side.
#pragma once
#include "common.h"

struct GuestThread;

void vox_set_image(u32 image_lo);          // patches the game once its code is loaded
bool vox_svc(GuestThread& t, u32 svc);     // false if svc is not one of ours
