// Double Tap as Black Ops II's Double Tap Root Beer II (each shot fires two bullets).
#pragma once
#include "common.h"

struct GuestThread;

void doubletap_set_image(u32 image_lo);         // patches the game once its code is loaded
bool doubletap_svc(GuestThread& t, u32 svc);    // false if svc is not one of ours
void doubletap_pause(bool paused);              // hits sent meanwhile are not bullets (the flamethrower's)
