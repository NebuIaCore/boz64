// Black Ops' dolphin dive: sprint, then crouch or prone, and the player dives forward to land prone. Also sets
// World at War's sprint timing (dive.cpp: tune_sprint).
#pragma once
#include "common.h"

struct GuestThread;

void dive_set_image(u32 image_lo);          // patches the game once its code is loaded
bool dive_svc(GuestThread& t, u32 svc);     // false if svc is not one of ours
void dive_on_player(u32 controller);        // the local player's update is starting (every frame; game thread)
bool dive_ready();                          // a crouch or prone press would dive now (game thread)
