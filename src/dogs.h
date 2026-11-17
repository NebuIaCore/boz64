// Kino der Toten's hellhound rounds as Black Ops PC plays them. The level data side is tools/dogpatch.py.
#pragma once
#include "common.h"

struct GuestThread;

void dogs_set_image(u32 image_lo);          // patches the game once its code is loaded
bool dogs_svc(GuestThread& t, u32 svc);     // false if svc is not one of ours
