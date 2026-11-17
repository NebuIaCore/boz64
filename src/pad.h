// Game controller, keyboard and mouse support.
// The controller layout is Call of Duty: Black Ops (2010) Zombies on the Xbox 360; java/boz64/Pad.java reads the
// controller and sends its state here (pad_input), as the PC window does from XInput. The keyboard and mouse follow Black Ops' PC defaults (layout in pad.cpp); the
// PC window (pc/src/window_win.cpp) and, on the phone, Pad.java send them here. During play (the main mode's in-game update, hooked in
// patches.cpp) they drive the game through its own input paths:
//   - movement goes in as the analog pad events the game made for the Xperia PLAY's touchpads (CAnalogTouchPad),
//     and the game's "gamepad in use" flag hides the on-screen sticks and buttons while they are in use;
//   - the controller's right stick looks around the same way; the mouse turns the view directly (pad_on_look);
//   - buttons and keys call the same functions as the HUD buttons (CInputManager and the in-game HUD handlers).
// Dead Ops Arcade (the top-down mode) uses twin-stick controls instead, following its PC version's.
// Outside play (menus), the controller moves a cursor (Pad.java draws one; the PC window moves the mouse pointer) and
// A touches the screen there, B and Start are Back; the mouse is a touch.
#pragma once
#include <jni.h>

#include "common.h"

void pad_register(JNIEnv* env);  // registers boz64.Pad's native methods
void pad_set_image(u32 image_lo);  // the game image is loaded (addresses are relative to it)
void pad_on_ingame(u32 state);     // the in-game update (CIngameStatePlaying) is starting; on the game thread
void pad_on_dead_ops(u32 state);   // the same for Dead Ops Arcade (CDOIngameStatePlaying)
void pad_on_look(u32 player);      // the local player's view update (CPlayerController) is starting; game thread
void pad_on_prompt(u32 element);   // the HUD is showing its interaction prompt (this element); game thread

// ---- keyboard and mouse, from the UI thread ----
// Keys are Android key codes (KeyEvent.KEYCODE_*); the mouse buttons have these codes:
constexpr int KEY_MOUSE_LEFT = 0x1001, KEY_MOUSE_RIGHT = 0x1002, KEY_MOUSE_MIDDLE = 0x1003, KEY_MOUSE_BACK = 0x1004,
              KEY_MOUSE_FORWARD = 0x1005;
void kbm_key(int code, bool down);         // every key and button change, in menus too (so nothing stays held)
void kbm_mouse_move(float dx, float dy);   // relative mouse motion in counts, while the mouse is captured
void kbm_wheel(int notches);               // mouse wheel, positive away from the user
void kbm_cursor(float x, float y);         // the cursor relative to the middle of the game's view (Dead Ops aims at it)
void kbm_release_all();                    // the window lost focus: every key and button is up
// What the mouse is for right now (the PC window and Pad.java ask often, and capture the mouse or let it go to match):
enum KbmMouse { KBM_MOUSE_TOUCH = 0, KBM_MOUSE_LOOK = 1, KBM_MOUSE_AIM = 2 };
//   TOUCH  menus and anything else: the mouse is a touch, keys go to the game as key presses
//   LOOK   zombies play: hide and capture the mouse; its motion turns the view, keys and buttons are controls
//   AIM    Dead Ops Arcade play: the cursor stays visible; buttons and keys are controls
KbmMouse kbm_mouse_mode();

// ---- the controller, from the UI thread (Pad.java on the phone, XInput in the PC window) ----
// Buttons as Pad.java's bits (A 1, B 2, X 4, Y 8, LB 16, RB 32, L3 64, R3 128, Start 256, Back 512, D-pad up 1024,
// down 2048, left 4096, right 8192); sticks -1..1 with y positive down; triggers 0..1. Send every change.
void pad_input(u32 buttons, float lx, float ly, float rx, float ry, float lt, float rt);
void pad_touch();    // the touch screen was touched (its on-screen controls come back)
bool pad_in_game();  // play is on: the controller drives the game, not the menu cursor
