// The port's options on the game's own "Controls Sensitivity" page (pause menu > Settings > Controls
// Sensitivity): frame rate, resolution scale and "zombies ignore you", built from copies of the page's IwUI
// elements; and menu pages kept on screen at any aspect ratio.
#pragma once
#include "common.h"

void menu_init(u32 image_lo);  // the game image is loaded (addresses below are relative to it)
void menu_on_enter();          // the page was just built (end of CIngameStateSensitivity's OnEnter)
// The page's "SensitivityY" slider slot is about to run for this slider; true if it was one of ours
// (handled here, and the game's handler must not run).
bool menu_on_slider(u32 slider);
// The page's "Calibrate" slot is about to run for this button; true if it was ours (the zombies checkbox).
bool menu_on_button(u32 button);
void menu_on_exit();  // the page is about to close
// Menu panels (CUIBackgroundImage): the size worked out for one (a CIwVec2 at size, with the space it has at
// avail) may be changed to fit the screen; and after a panel is laid out.
void menu_fit_panel(u32 panel, u32 size, u32 avail);
void menu_after_layout(u32 panel);
