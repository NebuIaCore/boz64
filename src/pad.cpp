// Game controller, keyboard and mouse support (see pad.h).
// The controller layout follows Black Ops (2010) Zombies on the Xbox 360:
//   left stick  move, click (L3) to sprint        right stick  look, click (R3) to knife
//   RT  fire (hold)                               LT  aim down the sights (hold)
//   RB  lethal grenade (hold to cook)             LB  tactical grenade (hold)
//   X   reload; hold near something to buy or use it (doors, wall weapons, perks, the box)
//   Y   switch weapon                             B   crouch; hold to go prone; while sprinting, dolphin dive
//   A   use (the game has no jump)                Start  pause menu
//   D-pad up  the weapon's alternate fire mode    D-pad down  prone
// The keyboard and mouse follow Black Ops' PC defaults:
//   W A S D (or the arrow keys)  move             mouse  look
//   left button  fire                             right button  aim down the sights (hold)
//   Shift  sprint (hold)                          C  crouch     Ctrl  prone     Space  stand up
//   C or Ctrl while sprinting  dolphin dive (dive.cpp)
//   R  reload                                     F  use or buy (doors, wall weapons, perks, the box)
//   V, E, middle button or back side button  knife
//   G  lethal grenade (hold to cook)              Q or 4  tactical grenade (hold)
//   1, 2 or the wheel  switch weapon (the game only has "next weapon")
//   3 or B  the weapon's alternate fire mode      Esc  pause menu
// Dead Ops Arcade is twin-stick, as on PC (WASD to move, arrow keys or the mouse to aim and shoot, G or the mouse
// wheel for a bomb, Space for a speed boost):
//   controller: left stick  move                  right stick  aim and shoot
//               RB or Y  drop a bomb (nuke)       LB, LT or A  speed boost       Start  pause menu
//   keyboard and mouse: W A S D  move; arrow keys, or hold the left button, to shoot (toward the cursor, measured
//               from the middle of the screen, as the PC version did); G, the wheel or the right button  bomb;
//               Space  speed boost; Esc  pause menu
// The game's own addresses below are in the game image as dumped at DUMP_BASE (the image is loaded the same
// way every run; they are moved to where it actually is). They were found in the game's code as follows:
//   - CAnalogTouchPad: the Xperia PLAY touchpad pair as two analog sticks. Its "+0x31" flag forces "gamepad in
//     use" on: the main mode's HUD then switches to its gamepad layout (no sticks, knife or sight buttons; the
//     rest is hidden by show_touch_controls), and Dead Ops uses the pads too. FUN_02578e48 posts a pad's position as the game's analog pad event (CInputManager passes it
//     on to the player).
//   - CInputManager: the player reads its flags each update; the in-game HUD's button handlers call into it.
//   - CPlayerController (the local player): its message handler (FUN_0263350c) takes the right pad's position as a
//     turn and pitch rate (+0x148, +0x14c) and sets "looked" (+0x154); its update (FUN_026320f8) turns the view by
//     those rates, clamps the pitch to MaxPitch (0.75 rad) and rebuilds the view from yaw +0x170 and pitch +0x174
//     (radians), but only when it was looked (or tilt steering is on). FUN_0262cdf4 clears "looked" once no look
//     input came in. The mouse adds its turn to yaw and pitch directly and sets "looked" (pad_on_look).
//   - Dead Ops Arcade still has its PC keyboard and mouse input (CDOKeyboardInput, off on phones). It turns the
//     keys into the same analog pad events (an angle and a strength; the angle is measured with y pointing down
//     the screen), and its bomb and speed boost commands into CDOPlayerInput calls. Its mouse aims from the middle
//     of the screen toward the cursor while the left button is down. The controller and keyboard do the same.
#include "pad.h"

#include <sys/system_properties.h>

#include <algorithm>
#include <atomic>
#include <bitset>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>

#include "cpu.h"
#include "dive.h"
#include "settings.h"

namespace {
constexpr u32 DUMP_BASE = 0x02454000;
// globals (each holds a pointer)
constexpr u32 ANALOG_PAD = 0x028bb2c8;  // CAnalogTouchPad
constexpr u32 INPUT_MGR = 0x028c1498;   // CInputManager
constexpr u32 GAME = 0x028ba484;        // the game object
constexpr u32 OPTIONS = 0x028bb2c4;     // the object whose +0x51 flag turns the right pad's up/down around
constexpr u32 DO_PLAYER_INPUT = 0x028bc18c;  // Dead Ops Arcade's CDOPlayerInput
constexpr u32 VIEW_LOCK = 0x028badac;   // while its object's +0xd4 bit 17 is set, the player's view is scripted
// fields
constexpr u32 GAME_PLAYER = 0x1a0;      // the local player (an entity reference)
constexpr u32 GAME_POI = 0x214;         // the point of interest in reach (door, wall weapon, perk...), or 0
constexpr u32 AP_FORCE_GAMEPAD = 0x31;  // "gamepad in use" (IsGamepadActive is true while it is set)
constexpr u32 AP_ANNOUNCED = 0x32;      // what the pad last told the HUD (it tells it when this changes)
constexpr u32 AP_LEFT = 0x34;           // left pad position: x, y (floats, -1..1, y down like the touchpad's)
constexpr u32 AP_RIGHT = 0x3c;          // right pad position
constexpr u32 OPTIONS_FLIP_Y = 0x51;
constexpr u32 IM_FIRE = 0x41;           // fire pressed this update (cleared each update unless IM_FIRE_HOLD)
constexpr u32 IM_FIRE_HOLD = 0x4c;      // keep firing (the touch controls set it on double tap and hold)
constexpr u32 IM_SPRINT = 0x4d;         // sprinting (OnAction's sprint on / off set it)
// CPlayerController
constexpr u32 PC_LOOK_SUM = 0x144;      // float: look input gathered this update (0 if none)
constexpr u32 PC_LOOK_X = 0x148, PC_LOOK_Y = 0x14c;  // floats: the right pad's turn and pitch rates
constexpr u32 PC_LOOKED = 0x154;        // byte: looked this update (the view is rebuilt)
constexpr u32 PC_CAN_LOOK = 0x15b;      // byte: the view update runs at all
constexpr u32 PC_YAW = 0x170, PC_PITCH = 0x174;  // floats, radians; yaw grows to the left, pitch looks down
constexpr u32 PC_POSTURE = 0x121;       // byte: 2 prone, 4 crouched, 8 last stand
constexpr u32 PC_PITCH_GLIDE = 0x2c0;   // {current, target, step, int count}: the game is moving the pitch itself
constexpr u32 POSTURE_PRONE = 2, POSTURE_CROUCH = 4;
// functions (Thumb)
constexpr u32 FN_POST_ANALOG = 0x02578e48 | 1;  // (float xy[2], pad 0/1): posts the analog pad event
constexpr u32 FN_PAD_RESET = 0x0257927c | 1;    // (CAnalogTouchPad): centres both pads (done before pausing)
constexpr u32 FN_ACTION = 0x0260024c | 1;       // CInputManager::OnAction(this, action, 0)
constexpr u32 FN_GRENADE = 0x02600754 | 1;      // CInputManager (this, held): lethal grenade held / let go
constexpr u32 FN_TACTICAL = 0x0260078c | 1;     // the same for the tactical grenade
constexpr u32 FN_REF_GET = 0x02512568 | 1;      // (entity reference): the entity, or 0
constexpr u32 FN_PLAYER_CTL = 0x0262cb88 | 1;   // (entity reference): the player's CPlayerController
constexpr u32 FN_IS_AIMING = 0x0262ca02 | 1;    // (CPlayerController): aiming down the sights
constexpr u32 FN_MELEE = 0x025f8b40 | 1;        // the in-game HUD's button handlers (no arguments)
constexpr u32 FN_RELOAD = 0x025f8b6c | 1;
constexpr u32 FN_SWAP_WEAPON = 0x025f8b98 | 1;
constexpr u32 FN_IRON_SIGHT = 0x025f8bc4 | 1;   // toggles aiming down the sights
constexpr u32 FN_ALT_FIRE = 0x025f8c10 | 1;
constexpr u32 FN_POI_ACTION = 0x025f91f8 | 1;   // use the point of interest in reach
constexpr u32 FN_PRONE = 0x025f9a14 | 1;
constexpr u32 FN_CROUCH = 0x025f9a5c | 1;
constexpr u32 FN_PAUSE = 0x025f959c | 1;        // (CIngameStatePlaying): opens the pause menu, as the Back key does
constexpr u32 FN_DO_ACTION = 0x025b3370 | 1;    // (CDOPlayerInput, DO_BOMB or DO_BOOST)
constexpr u32 FN_DO_PAUSE = 0x025a0ac0 | 1;     // (CDOIngameStatePlaying): Dead Ops Arcade's pause menu
constexpr u32 DO_BOMB = 0, DO_BOOST = 1;        // CDOKeyboardInput's "dropTacticalBomb" and "startSpeedBoost"
// CInputManager::OnAction codes
constexpr u32 ACT_SPRINT_OFF = 3, ACT_FIRE_UP = 10;  // (2, sprint on, also uses what is in reach: see sprint_on)
// IwUI (ARM library code): the HUD's interaction prompt is resized with these
constexpr u32 FN_CHILD_NAMED = 0x02756e2c;  // CIwUIElement::GetChildNamed(this, name, recurse, 0)
constexpr u32 FN_SET_POS = 0x02757790;      // CIwUIElement::SetPos(this, const CIwVec2*, relayout)
constexpr u32 FN_SET_SIZE = 0x027586f0;     // CIwUIElement::SetSize(this, const CIwVec2*, relayout)
constexpr u32 EL_POS = 0x5c, EL_SIZE = 0x64;  // CIwUIElement: position and size (CIwVec2, s32 pairs)
constexpr u32 FN_SET_VISIBLE = 0x0275a518;  // CIwUIElement::SetVisible(this, bool)
constexpr u32 FN_GET_FLAG = 0x02758858;     // CIwUIElement (this, flag): whether the flag is set (ELF_VISIBLE...)
constexpr u32 EL_FLAGS_CACHED = 0x2c;       // u16: bit 0 set when the flags below are up to date
constexpr u32 EL_FLAGS = 0x2e, ELF_VISIBLE = 2;
// the game's UI manager (global holding a pointer) and its lookup of an element anywhere in its tree by name
constexpr u32 UI_MANAGER = 0x028bd280;
constexpr u32 FN_FIND_ELEMENT = 0x02669014 | 1;  // (manager, const char* name, 0): the element, or 0
// first instructions of some of the above, checked before anything is called
struct Check { u32 fn, word; };
constexpr Check CHECKS[] = {
    {FN_POST_ANALOG, 0x47f0e92d}, {FN_ACTION, 0xb5f71e4b}, {FN_GRENADE, 0x460ab507}, {FN_TACTICAL, 0x460ab507},
    {FN_MELEE, 0x4b08b508},       {FN_RELOAD, 0x4b08b508}, {FN_SWAP_WEAPON, 0x4b08b508}, {FN_IRON_SIGHT, 0x4b08b508},
    {FN_POI_ACTION, 0x4e0fb573},  {FN_PRONE, 0xb5304b0f},  {FN_CROUCH, 0xb086b570},  {FN_PAUSE, 0x447b4b05},
    {FN_PAD_RESET, 0x4a0b4b0a},   {FN_REF_GET, 0x4603b508}, {FN_ALT_FIRE, 0x21014b02},
    {FN_DO_ACTION, 0x460ab507},   {FN_DO_PAUSE, 0x2103b510}, {FN_PLAYER_CTL, 0x4603b508},
    {FN_IS_AIMING, 0xf500b508},   {FN_SET_VISIBLE, 0xe92d407f}, {FN_FIND_ELEMENT, 0xf100b570},
    {FN_GET_FLAG, 0xe92d4013},
};

// buttons, as java/boz64/Pad.java sends them
enum : u32 {
    B_A = 1u << 0, B_B = 1u << 1, B_X = 1u << 2, B_Y = 1u << 3, B_LB = 1u << 4, B_RB = 1u << 5, B_L3 = 1u << 6,
    B_R3 = 1u << 7, B_START = 1u << 8, B_BACK = 1u << 9, B_UP = 1u << 10, B_DOWN = 1u << 11, B_LEFT = 1u << 12,
    B_RIGHT = 1u << 13, B_LT = 1u << 14, B_RT = 1u << 15,
};
constexpr float TRIGGER_ON = 0.5f, TRIGGER_OFF = 0.3f;
constexpr float MOVE_DEADZONE = 0.2f, LOOK_DEADZONE = 0.15f;
constexpr float LOOK_CURVE = 1.6f;            // look speed grows with the stick's travel to this power
constexpr float AIM_DEADZONE = 0.25f;         // Dead Ops: the right stick shoots once it is pushed this far
constexpr float SPRINT_STOP = 0.3f;           // sprinting ends when the left stick is back under this
constexpr u64 MS = 1000000ull;
constexpr u64 USE_HOLD = 300 * MS;            // X held this long near a point of interest uses it
constexpr u64 PRONE_HOLD = 400 * MS;          // B held this long goes prone
constexpr u64 AWAY = 500 * MS;                // no in-game update for this long: play stopped (menus, loading)
constexpr u64 MOUSE_AWAY = 1000 * MS;         // the same, for letting the mouse go: long enough to ride out a
                                              // hitch (a gun or model loading), since Esc lets go at once
constexpr u64 HUD_GAP = 100 * MS;             // no in-game update for this long: the HUD may have been rebuilt
constexpr float PROMPT_BUTTON_SCALE = 0.5f;   // the prompt's round button while a controller or the keyboard is used
constexpr u64 PROMPT_WINDOW = 500 * MS;       // the prompt is resized for this long after the game last showed it
constexpr u64 WHEEL_GAP = 200 * MS;           // at most one weapon switch from the wheel this often
constexpr u64 AIM_RETRY = 400 * MS;           // the sight is toggled again if aiming still differs after this
constexpr float PI = 3.14159265f, TWO_PI = 6.2831853f;
constexpr float MOUSE_RAD = 0.022f * PI / 180;  // radians per mouse count at sensitivity 1 (Quake's m_yaw)

struct Input {
    u32 buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0, lt = 0, rt = 0;
};
std::mutex input_lock;
Input input;                         // latest from the controller (UI thread)
std::atomic<u32> input_seq{0};       // changes with each controller update
std::atomic<u32> touch_seq{0};       // changes when the touch screen is touched
std::atomic<u64> ingame_until{0};    // the in-game update ran recently (read by the UI thread)

// ---- keyboard and mouse (written by the UI thread) ----
enum Key : u32 {
    KW, KA, KS, KD, KUP, KDOWN, KLEFT, KRIGHT, KSPACE, KSHIFT, KCTRL, KC, KR, KF, KE, KV, KG, KQ, K1, K2, K3, K4, KB,
    KESC, KM1, KM2, KM3, KM4, KM5, KEY_COUNT
};
int key_of(int code) {
    switch (code) {  // Android key codes
    case 51: return KW;
    case 29: return KA;
    case 47: return KS;
    case 32: return KD;
    case 19: return KUP;
    case 20: return KDOWN;
    case 21: return KLEFT;
    case 22: return KRIGHT;
    case 62: return KSPACE;
    case 59: case 60: return KSHIFT;
    case 113: case 114: return KCTRL;
    case 31: return KC;
    case 46: return KR;
    case 34: return KF;
    case 33: return KE;
    case 50: return KV;
    case 35: return KG;
    case 45: return KQ;
    case 8: return K1;
    case 9: return K2;
    case 10: return K3;
    case 11: return K4;
    case 30: return KB;
    case 4: case 111: return KESC;  // BACK (the PC window's Esc), ESCAPE
    case KEY_MOUSE_LEFT: return KM1;
    case KEY_MOUSE_RIGHT: return KM2;
    case KEY_MOUSE_MIDDLE: return KM3;
    case KEY_MOUSE_BACK: return KM4;
    case KEY_MOUSE_FORWARD: return KM5;
    default: return -1;
    }
}
constexpr int CODE_LIMIT = 0x1010;
struct Kbm {
    std::bitset<CODE_LIMIT> codes;   // key codes down (two codes can be one key: left and right Shift)
    u64 keys = 0;                    // Key bits down
    int wheel = 0;                   // notches not yet used
    float dx = 0, dy = 0;            // mouse motion not yet applied
    float cx = 0, cy = 0;            // the cursor, from the middle of the view
};
std::mutex kbm_lock;
Kbm kbm;
std::atomic<u32> kbm_seq{0};         // changes with each key, button, wheel or captured mouse motion
std::atomic<u64> play_until{0};      // in play until then (for the UI thread's mouse mode)
std::atomic<int> play_mode{0};       // 1 zombies, 2 Dead Ops
std::atomic<bool> kbm_active{false};  // the keyboard and mouse are in control (for the UI thread)
u64 pause_hold_until = 0;            // Esc paused the game: play doesn't hold the mouse until then (game thread)

u32 image = 0;
bool enabled = false;
float look_scale = 1.0f, move_scale = 1.0f;

// game thread state
u64 last_update = 0;
u32 seen_input = 0, seen_touch = 0, seen_kbm = 0;
enum Source { TOUCH, PAD, KBM };
#ifdef BOZ_PC
Source source = KBM;  // the PC has no touch screen
#else
Source source = TOUCH;
#endif
Source applied = source;  // whose input the held state below came from
u32 prev = 0;
bool lt_on = false, rt_on = false;
bool firing = false, lethal_held = false, tactical_held = false, sprinting = false, aim_held = false;
u64 aim_toggled = 0;  // when the sight was last toggled
bool moving = false, looking = false;
u64 x_since = 0, b_since = 0;  // when X / B went down, while waiting to see if it is held
u64 keys_prev = 0, keys_blocked = 0;  // keyboard: last update's keys; keys held when play resumed (ignored until up)
u64 last_wheel_swap = 0;
enum Mode { NONE, ZOMBIES, DEAD_OPS };
Mode mode = NONE;

u32 at(u32 a) { return a - DUMP_BASE + image; }
bool valid(u32 a) { return a >= g::NULL_GUARD && a < g::ALLOC_END && !(a & 3); }
u64 now_ns() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000000000ull + static_cast<u64>(ts.tv_nsec);
}
u32 call(u32 fn, std::initializer_list<u32> args = {}) { return static_cast<u32>(call_guest(at(fn), args)); }

u32 game() { return g::rd32(at(GAME)); }
bool has_player() {
    u32 g0 = game();
    return valid(g0) && call(FN_REF_GET, {g::rd32(g0 + GAME_PLAYER)}) != 0;
}
u32 player_controller() {
    u32 g0 = game();
    if (!valid(g0)) return 0;
    u32 ref = g::rd32(g0 + GAME_PLAYER);
    if (!ref || !call(FN_REF_GET, {ref})) return 0;
    u32 c = call(FN_PLAYER_CTL, {ref});
    return valid(c) ? c : 0;
}
bool poi_in_reach() {
    u32 g0 = game();
    if (!valid(g0)) return false;
    u32 ref = g::rd32(g0 + GAME_POI);
    return ref && call(FN_REF_GET, {ref}) != 0;
}
// Sprinting starts by setting the flag OnAction(sprint on) sets: that action also uses the point of interest in
// reach (the touch controls' gesture for both), so Shift or L3 next to a door, gun or perk would buy it.
void sprint_on(u32 im) { g::wr8(im + IM_SPRINT, 1); }

// The HUD's interaction prompt ("Tap to buy ..."): a caption over a big round "Button" with its "Image", sized as a
// touch target. While a controller or the keyboard is in use it is only a reminder of the key, so it is drawn at
// PROMPT_BUTTON_SCALE about its centre; with the touch screen it is put back to the game's size. The game shows the
// prompt through FUN_025f8e80 (patches.cpp hooks it and passes its element here). Its layout can put the button back
// to its own size when the caption changes, so for a moment after each show the size is checked every update. The
// element is only used then, while it is surely the live HUD's.
u32 prompt = 0;          // the prompt element the game last showed
u64 prompt_shown = 0;    // when
u32 prompt_names[2] = {};  // guest strings "Button", "Image"
u32 prompt_vec = 0;      // guest CIwVec2 scratch
struct Shrunk {
    u32 el = 0;
    s32 orig[4] = {};  // x, y, w, h as the game laid it out
    s32 set[4] = {};   // as we left it
    bool on = false;   // we resized it (and it still has our size)
};
Shrunk shrunk[2];

void read_box(u32 el, s32 b[4]) {
    for (int i = 0; i < 2; i++) b[i] = static_cast<s32>(g::rd32(el + EL_POS + 4 * i));
    for (int i = 0; i < 2; i++) b[2 + i] = static_cast<s32>(g::rd32(el + EL_SIZE + 4 * i));
}
void put_box(u32 el, const s32 b[4]) {
    g::wr32(prompt_vec, static_cast<u32>(b[2]));
    g::wr32(prompt_vec + 4, static_cast<u32>(b[3]));
    call(FN_SET_SIZE, {el, prompt_vec, 0});
    g::wr32(prompt_vec, static_cast<u32>(b[0]));
    g::wr32(prompt_vec + 4, static_cast<u32>(b[1]));
    call(FN_SET_POS, {el, prompt_vec, 0});
}

void fit_prompt(bool small, u64 now) {
    if (!valid(prompt) || now - prompt_shown > PROMPT_WINDOW) return;
    if (!prompt_vec) {
        prompt_names[0] = g::data_str("Button");
        prompt_names[1] = g::data_str("Image");
        prompt_vec = g::data_alloc(8, 4);
    }
    for (int i = 0; i < 2; i++) {
        Shrunk& s = shrunk[i];
        u32 el = call(FN_CHILD_NAMED, {prompt, prompt_names[i], 1, 0});
        if (!valid(el)) continue;
        if (el != s.el) s = Shrunk{}, s.el = el;
        s32 now_box[4];
        read_box(el, now_box);
        if (s.on && std::memcmp(now_box, s.set, sizeof now_box) != 0) s.on = false;  // the game laid it out again
        if (!s.on) std::memcpy(s.orig, now_box, sizeof now_box);
        if (small && !s.on && s.orig[2] > 0 && s.orig[3] > 0) {
            s32 w = std::max(1, static_cast<s32>(std::lround(s.orig[2] * PROMPT_BUTTON_SCALE)));
            s32 h = std::max(1, static_cast<s32>(std::lround(s.orig[3] * PROMPT_BUTTON_SCALE)));
            const s32 box[4] = {s.orig[0] + (s.orig[2] - w) / 2, s.orig[1] + (s.orig[3] - h) / 2, w, h};
            put_box(el, box);
            read_box(el, s.set);
            s.on = true;
        } else if (!small && s.on) {
            put_box(el, s.orig);
            s.on = false;
        }
    }
}

// The HUD's touch controls. While "gamepad in use" is set the HUD switches to its gamepad layout (CHUDManager's
// control scheme 3, made for the Xperia PLAY), which drops the sticks and most buttons but still shows the crouch
// button, the pause button and faint circles marking the PLAY's touchpads (a stick base and a knob for each). While a
// controller or the keyboard is in use the crouch button and the circles are hidden too (the pause button stays, by
// request); when the touch screen is used again, what was hidden here is shown again (except the circles, which only
// belong to the gamepad layout). The elements are looked up by name after a gap in play (the HUD may have been rebuilt) and every
// TOUCH_REFIND, and only used in between. (IwUI keeps only a hash of each name, lowercase djb2 at element +4; the
// pause button is "pause_Btn", a 96x96 touch area, with its icon drawn by "pause_blood", the splat behind the score.)
constexpr const char* TOUCH_ONLY[] = {
    // the touchpad circles (first): the knobs, and the stick bases under them
    "analoguePad1", "analoguePad2", "analoguePad1_lefty", "analoguePad2_lefty", "left_stick_img", "right_stick_img",
    "Crouch_Btn",   "crouch_element", "Crouch_blood",     "Crouch_Btn_lefty",    "crouch_element_lefty",
    "Crouch_blood_lefty",
};
constexpr int TOUCH_COUNT = sizeof TOUCH_ONLY / sizeof TOUCH_ONLY[0];
constexpr int TOUCHPAD_MARKERS = 6;   // the first six: never shown again here
constexpr u64 TOUCH_REFIND = 1000 * MS;
u32 touch_names[TOUCH_COUNT] = {};    // guest copies of the names
u32 touch_els[TOUCH_COUNT] = {};      // the elements as last looked up (0: not in this HUD)
u32 touch_hidden[TOUCH_COUNT] = {};   // elements hidden here (to show again for the touch screen)
u64 touch_found = 0, touch_seen = 0;  // when the elements were looked up; the last update this ran
u64 touch_reshown = 0;                // last logged that the game showed a hidden one again

u32 find_element(int i) {
    u32 mgr = g::rd32(at(UI_MANAGER));
    if (!valid(mgr)) return 0;
    if (!touch_names[i]) touch_names[i] = g::data_str(TOUCH_ONLY[i]);
    u32 el = call(FN_FIND_ELEMENT, {mgr, touch_names[i], 0});
    return valid(el) ? el : 0;
}

void show_touch_controls(bool touch, u64 now) {
    bool fresh = now - touch_seen > HUD_GAP;  // back in play: the HUD may be a new one
    touch_seen = now;
    if (touch) {
        if (fresh) std::memset(touch_hidden, 0, sizeof touch_hidden);  // not this HUD's
        for (int i = TOUCHPAD_MARKERS; i < TOUCH_COUNT; i++) {
            if (!touch_hidden[i]) continue;
            if (find_element(i) == touch_hidden[i]) call(FN_SET_VISIBLE, {touch_hidden[i], 1});
            touch_hidden[i] = 0;
        }
        touch_found = 0;
        return;
    }
    if (fresh || now - touch_found > TOUCH_REFIND) {
        if (fresh) std::memset(touch_hidden, 0, sizeof touch_hidden);
        for (int i = 0; i < TOUCH_COUNT; i++) touch_els[i] = find_element(i);
        touch_found = now;
    }
    for (int i = 0; i < TOUCH_COUNT; i++) {
        u32 el = touch_els[i];
        if (!el) continue;
        bool visible = (g::rd16(el + EL_FLAGS_CACHED) & 1) ? (g::rd16(el + EL_FLAGS) & ELF_VISIBLE) != 0
                                                            : (call(FN_GET_FLAG, {el, ELF_VISIBLE}) & 0xff) != 0;
        if (!visible) continue;
        call(FN_SET_VISIBLE, {el, 0});
        if (touch_hidden[i] != el) LOGI("controller: hid the HUD's %s", TOUCH_ONLY[i]);
        else if (now - touch_reshown > 10000 * MS) touch_reshown = now, LOGI("controller: the HUD showed %s again", TOUCH_ONLY[i]);
        touch_hidden[i] = el;
    }
}

// Aim down the sights while a button is held. The game's sight button toggles, so it is pressed when the button goes
// down or up and the player's aim differs, and again whenever it still differs a moment later (the game drops out of
// the sights to reload or sprint, or did not take the press).
void hold_aim(bool want, u64 now) {
    u32 c = player_controller();
    if (!c) return;
    bool aiming = call(FN_IS_AIMING, {c}) != 0;
    bool edge = want != aim_held;
    aim_held = want;
    if (aiming == want) return;
    if (edge || now - aim_toggled >= AIM_RETRY) call(FN_IRON_SIGHT), aim_toggled = now;
}

// A stick's position after a radial dead zone (rescaled so it still reaches 1) and a response curve.
void shape(float x, float y, float dead, float curve, float scale, float& ox, float& oy) {
    float r = std::sqrt(x * x + y * y);
    if (r <= dead) {
        ox = oy = 0;
        return;
    }
    float m = std::min(1.0f, (r - dead) / (1.0f - dead));
    m = std::pow(m, curve) * scale;
    ox = x / r * m;
    oy = y / r * m;
}

void post_pad(u32 pad, u32 field, u32 which, float x, float y) {
    g::wrf(pad + field, x);
    g::wrf(pad + field + 4, y);
    call(FN_POST_ANALOG, {pad + field, which});
}

// Lets go of whatever is held (the input source changed, or play stopped).
void release_all(u32 pad, u32 im) {
    if (firing) call(FN_ACTION, {im, ACT_FIRE_UP, 0});
    if (lethal_held) call(FN_GRENADE, {im, 0});
    if (tactical_held) call(FN_TACTICAL, {im, 0});
    if (sprinting) call(FN_ACTION, {im, ACT_SPRINT_OFF, 0});
    if (moving) post_pad(pad, AP_LEFT, 0, 0, 0);
    if (looking) post_pad(pad, AP_RIGHT, 1, 0, 0);
    firing = lethal_held = tactical_held = sprinting = aim_held = moving = looking = false;
    x_since = b_since = 0;
    aim_toggled = 0;
}

float prop_float(const char* name, float def) {
    char v[PROP_VALUE_MAX] = {};
    if (__system_property_get(name, v) <= 0) return def;
    float f = static_cast<float>(atof(v));
    return f > 0 ? f : def;
}

void JNICALL n_input(JNIEnv*, jclass, jint buttons, jfloat lx, jfloat ly, jfloat rx, jfloat ry, jfloat lt, jfloat rt) {
    pad_input(static_cast<u32>(buttons), lx, ly, rx, ry, lt, rt);
}
void JNICALL n_touch(JNIEnv*, jclass) { pad_touch(); }
jboolean JNICALL n_in_game(JNIEnv*, jclass) { return pad_in_game() ? JNI_TRUE : JNI_FALSE; }
// the keyboard and mouse (Pad.java reads them on the phone, as window_win.cpp does on the PC)
void JNICALL n_key(JNIEnv*, jclass, jint code, jboolean down) { kbm_key(code, down != JNI_FALSE); }
void JNICALL n_mouse_move(JNIEnv*, jclass, jfloat dx, jfloat dy) { kbm_mouse_move(dx, dy); }
void JNICALL n_wheel(JNIEnv*, jclass, jint notches) { kbm_wheel(notches); }
void JNICALL n_cursor(JNIEnv*, jclass, jfloat x, jfloat y) { kbm_cursor(x, y); }
void JNICALL n_release_all(JNIEnv*, jclass) { kbm_release_all(); }
jint JNICALL n_mouse_mode(JNIEnv*, jclass) { return kbm_mouse_mode(); }
}  // namespace

void pad_register(JNIEnv* env) {
    jclass c = env->FindClass("boz64/Pad");
    if (!c) {
        env->ExceptionClear();
        LOGW("controller: boz64.Pad not found");
        return;
    }
    static const JNINativeMethod methods[] = {
        {const_cast<char*>("nativeInput"), const_cast<char*>("(IFFFFFF)V"), reinterpret_cast<void*>(n_input)},
        {const_cast<char*>("nativeTouch"), const_cast<char*>("()V"), reinterpret_cast<void*>(n_touch)},
        {const_cast<char*>("nativeInGame"), const_cast<char*>("()Z"), reinterpret_cast<void*>(n_in_game)},
        {const_cast<char*>("nativeKey"), const_cast<char*>("(IZ)V"), reinterpret_cast<void*>(n_key)},
        {const_cast<char*>("nativeMouseMove"), const_cast<char*>("(FF)V"), reinterpret_cast<void*>(n_mouse_move)},
        {const_cast<char*>("nativeWheel"), const_cast<char*>("(I)V"), reinterpret_cast<void*>(n_wheel)},
        {const_cast<char*>("nativeCursor"), const_cast<char*>("(FF)V"), reinterpret_cast<void*>(n_cursor)},
        {const_cast<char*>("nativeReleaseAll"), const_cast<char*>("()V"), reinterpret_cast<void*>(n_release_all)},
        {const_cast<char*>("nativeMouseMode"), const_cast<char*>("()I"), reinterpret_cast<void*>(n_mouse_mode)},
    };
    if (env->RegisterNatives(c, methods, sizeof methods / sizeof methods[0]) != 0) {
        env->ExceptionClear();
        LOGW("controller: could not register boz64.Pad's natives");
    }
    env->DeleteLocalRef(c);
}

void pad_set_image(u32 image_lo) {
    image = image_lo;
    for (const Check& c : CHECKS) {
        u32 w = g::rd32(at(c.fn & ~1u));
        if (w != c.word) {
            LOGW("controller: unexpected code at 0x%08x (%08x, expected %08x); controller support is off", c.fn & ~1u, w,
                 c.word);
            return;
        }
    }
    look_scale = prop_float("debug.boz64.padlook", 1.0f);
    move_scale = prop_float("debug.boz64.padmove", 1.0f);
    enabled = true;
}

// ---- the controller: the UI thread's side ----

void pad_input(u32 buttons, float lx, float ly, float rx, float ry, float lt, float rt) {
    {
        std::lock_guard lk(input_lock);
        input.buttons = buttons;
        input.lx = lx, input.ly = ly, input.rx = rx, input.ry = ry, input.lt = lt, input.rt = rt;
    }
    input_seq.fetch_add(1);
}

void pad_touch() { touch_seq.fetch_add(1); }

bool pad_in_game() { return now_ns() < ingame_until.load(); }

// ---- keyboard and mouse: the UI thread's side ----

void kbm_key(int code, bool down) {
    if (code < 0 || code >= CODE_LIMIT) return;
    int k = key_of(code);
    {
        std::lock_guard lk(kbm_lock);
        if (kbm.codes.test(code) == down) return;  // a repeat
        kbm.codes.set(code, down);
        if (k < 0) return;
        bool any = false;  // is the key still down through another code?
        for (int c = 0; c < CODE_LIMIT && !any; c++)
            if (kbm.codes.test(c) && key_of(c) == k) any = true;
        if (any) kbm.keys |= 1ull << k;
        else kbm.keys &= ~(1ull << k);
    }
    kbm_seq.fetch_add(1);
}

void kbm_mouse_move(float dx, float dy) {
    if (dx == 0 && dy == 0) return;
    {
        std::lock_guard lk(kbm_lock);
        kbm.dx += dx;
        kbm.dy += dy;
    }
    kbm_seq.fetch_add(1);
}

void kbm_wheel(int notches) {
    if (!notches) return;
    {
        std::lock_guard lk(kbm_lock);
        kbm.wheel += notches;
    }
    kbm_seq.fetch_add(1);
}

void kbm_cursor(float x, float y) {
    std::lock_guard lk(kbm_lock);
    kbm.cx = x;
    kbm.cy = y;
}

void kbm_release_all() {
    std::lock_guard lk(kbm_lock);
    kbm.codes.reset();
    kbm.keys = 0;
    kbm.wheel = 0;
    kbm.dx = kbm.dy = 0;
}

KbmMouse kbm_mouse_mode() {
    if (!enabled || !kbm_active.load() || now_ns() >= play_until.load()) return KBM_MOUSE_TOUCH;
    return play_mode.load() == DEAD_OPS ? KBM_MOUSE_AIM : KBM_MOUSE_LOOK;
}

namespace {
// The start of either mode's in-game update: notes that play is on, takes the controller's and the keyboard's latest
// state, and works out who is in control. Returns the analog pad object (0 if it isn't there yet).
u32 begin_update(Mode m, Input& in, Kbm& k, u32& b, u64& now) {
    now = now_ns();
    u64 gap = now - last_update;
    bool resumed = gap > AWAY || m != mode;
    if (m != mode) LOGI("controller: %s", m == DEAD_OPS ? "Dead Ops Arcade" : "zombies");
    mode = m;
    last_update = now;
    ingame_until = now + AWAY;
    if (now >= pause_hold_until) play_until = now + MOUSE_AWAY;  // (if the game didn't pause after all, from then on)
    play_mode = m;
    {
        std::lock_guard lk(input_lock);
        in = input;
    }
    {
        std::lock_guard lk(kbm_lock);
        if (resumed) kbm.dx = kbm.dy = 0, kbm.wheel = 0;  // motion from before play resumed
        k.keys = kbm.keys;
        k.wheel = kbm.wheel;
        k.cx = kbm.cx;
        k.cy = kbm.cy;
        kbm.wheel = 0;
    }
    lt_on = in.lt >= (lt_on ? TRIGGER_OFF : TRIGGER_ON);
    rt_on = in.rt >= (rt_on ? TRIGGER_OFF : TRIGGER_ON);
    b = in.buttons | (lt_on ? B_LT : 0) | (rt_on ? B_RT : 0);
    if (resumed) {  // back in play (from a menu or loading): buttons already down there do nothing here
        firing = lethal_held = tactical_held = sprinting = aim_held = moving = looking = false;
        x_since = b_since = 0;
        prev = b;
        keys_prev = k.keys;
        constexpr u64 MOVE_KEYS = 1ull << KW | 1ull << KA | 1ull << KS | 1ull << KD | 1ull << KUP | 1ull << KDOWN |
                                  1ull << KLEFT | 1ull << KRIGHT | 1ull << KSHIFT;
        keys_blocked = k.keys & ~MOVE_KEYS;  // e.g. the click on "Resume" must not fire
    }
    keys_blocked &= k.keys;
    u32 pad = g::rd32(at(ANALOG_PAD));
    if (!valid(pad)) return 0;

    // Who is in control: the controller once it is used, the keyboard or mouse once used, the touch screen once
    // touched.
    u32 ts = touch_seq.load(), is = input_seq.load(), ks = kbm_seq.load();
    Source was = source;
    if (ts != seen_touch) seen_touch = ts, source = TOUCH;
    if (is != seen_input) {
        seen_input = is;
        float mag = std::max(std::max(std::fabs(in.lx), std::fabs(in.ly)), std::max(std::fabs(in.rx), std::fabs(in.ry)));
        if (b || mag > 0.5f) source = PAD;
    }
    if (ks != seen_kbm) seen_kbm = ks, source = KBM;
    if (source != was)
        LOGI("controller: %s", source == PAD ? "in use" : source == KBM ? "keyboard and mouse in use" : "touch screen in use");
    kbm_active = source == KBM;
    // the game's "gamepad in use": hides the on-screen controls (both modes check it)
    bool force = source != TOUCH;
    if ((g::rd8(pad + AP_FORCE_GAMEPAD) != 0) != force) g::wr8(pad + AP_FORCE_GAMEPAD, force ? 1 : 0);
    // The pad tells the HUD only when "gamepad in use" changes, and the HUD shows its on-screen controls again after
    // the pause menu: after a gap in play the pad is made to tell it again.
    if (force && (gap > HUD_GAP || resumed)) g::wr8(pad + AP_ANNOUNCED, 0);
    return pad;
}

void pad_zombies(u32 state, u32 pad, u32 im, const Input& in, u32 b, u64 now) {
    // Sticks: the analog pad events, each update while a stick is off centre, and once more when it centres.
    float mx, my, lx, ly;
    shape(in.lx, in.ly, MOVE_DEADZONE, 1.0f, move_scale, mx, my);  // y down, as Android reports the stick
    if (mx != 0 || my != 0 || moving) post_pad(pad, AP_LEFT, 0, mx, my);
    moving = mx != 0 || my != 0;
    shape(in.rx, in.ry, LOOK_DEADZONE, LOOK_CURVE, look_scale, lx, ly);
    u32 opts = g::rd32(at(OPTIONS));
    if (valid(opts) && g::rd8(opts + OPTIONS_FLIP_Y)) ly = -ly;  // as the game does with the right touchpad
    if (lx != 0 || ly != 0 || looking) post_pad(pad, AP_RIGHT, 1, lx, ly);
    looking = lx != 0 || ly != 0;

    auto down = [&](u32 m) { return (b & m) && !(prev & m); };
    auto up = [&](u32 m) { return !(b & m) && (prev & m); };

    // RT: fire while held (the touch controls' "double tap and hold")
    if (b & B_RT) {
        g::wr8(im + IM_FIRE, 1);
        g::wr8(im + IM_FIRE_HOLD, 1);
        firing = true;
    } else if (firing) {
        call(FN_ACTION, {im, ACT_FIRE_UP, 0});
        firing = false;
    }
    // LT: aim while held
    hold_aim((b & B_LT) != 0, now);
    // RB / LB: grenades, thrown on release
    if (down(B_RB) && !lethal_held && has_player()) call(FN_GRENADE, {im, 1}), lethal_held = true;
    if (up(B_RB) && lethal_held) call(FN_GRENADE, {im, 0}), lethal_held = false;
    if (down(B_LB) && !tactical_held && has_player()) call(FN_TACTICAL, {im, 1}), tactical_held = true;
    if (up(B_LB) && tactical_held) call(FN_TACTICAL, {im, 0}), tactical_held = false;
    // Y: switch weapon; R3: knife; A: use
    if (down(B_Y)) call(FN_SWAP_WEAPON);
    if (down(B_R3)) call(FN_MELEE);
    if (down(B_A)) call(FN_POI_ACTION);
    // X: reload, or hold it to use what is in reach
    if (down(B_X)) {
        if (poi_in_reach()) x_since = now;
        else call(FN_RELOAD);
    }
    if ((b & B_X) && x_since && now - x_since >= USE_HOLD) call(FN_POI_ACTION), x_since = 0;
    if (up(B_X) && x_since) call(FN_RELOAD), x_since = 0;
    // B: crouch, or hold it to go prone; while sprinting it dives at once (dive.cpp: the crouch button dives then)
    if (down(B_B)) {
        if (dive_ready()) call(FN_CROUCH);
        else b_since = now;
    }
    if ((b & B_B) && b_since && now - b_since >= PRONE_HOLD) call(FN_PRONE), b_since = 0;
    if (up(B_B) && b_since) call(FN_CROUCH), b_since = 0;
    // D-pad
    if (down(B_UP)) call(FN_ALT_FIRE);
    if (down(B_DOWN)) call(FN_PRONE);
    // L3: sprint until the stick is let go (or L3 again)
    if (down(B_L3)) {
        if (sprinting) call(FN_ACTION, {im, ACT_SPRINT_OFF, 0});
        else sprint_on(im);
        sprinting = !sprinting;
    } else if (sprinting && std::sqrt(in.lx * in.lx + in.ly * in.ly) < SPRINT_STOP) {
        call(FN_ACTION, {im, ACT_SPRINT_OFF, 0});
        sprinting = false;
    }
    // Start: pause menu
    if (down(B_START)) {
        release_all(pad, im);
        call(FN_PAD_RESET, {pad});
        call(FN_PAUSE, {state});
    }
}

void kbm_zombies(u32 state, u32 pad, u32 im, const Kbm& k, u64 now) {
    auto held = [&](Key i) { return (((k.keys & ~keys_blocked) >> i) & 1) != 0; };
    auto down = [&](Key i) { return held(i) && !((keys_prev >> i) & 1); };

    // movement keys: the left pad at full strength (y down), each update while held and once more when let go
    float mx = static_cast<float>((held(KD) || held(KRIGHT)) - (held(KA) || held(KLEFT)));
    float my = static_cast<float>((held(KS) || held(KDOWN)) - (held(KW) || held(KUP)));
    if (mx != 0 && my != 0) mx *= 0.70710678f, my *= 0.70710678f;
    if (mx != 0 || my != 0 || moving) post_pad(pad, AP_LEFT, 0, mx, my);
    moving = mx != 0 || my != 0;
    if (looking) post_pad(pad, AP_RIGHT, 1, 0, 0), looking = false;  // the mouse looks another way (pad_on_look)

    // left button: fire while held
    if (held(KM1)) {
        g::wr8(im + IM_FIRE, 1);
        g::wr8(im + IM_FIRE_HOLD, 1);
        firing = true;
    } else if (firing) {
        call(FN_ACTION, {im, ACT_FIRE_UP, 0});
        firing = false;
    }
    // right button: aim while held
    hold_aim(held(KM2), now);
    // Shift: sprint while held and moving
    if (held(KSHIFT) && moving && !sprinting) sprint_on(im), sprinting = true;
    else if ((!held(KSHIFT) || !moving) && sprinting) call(FN_ACTION, {im, ACT_SPRINT_OFF, 0}), sprinting = false;
    // grenades: held to cook, thrown when let go
    bool lethal = held(KG), tactical = held(KQ) || held(K4);
    if (lethal && !lethal_held && has_player()) call(FN_GRENADE, {im, 1}), lethal_held = true;
    if (!lethal && lethal_held) call(FN_GRENADE, {im, 0}), lethal_held = false;
    if (tactical && !tactical_held && has_player()) call(FN_TACTICAL, {im, 1}), tactical_held = true;
    if (!tactical && tactical_held) call(FN_TACTICAL, {im, 0}), tactical_held = false;
    // presses
    if (down(KR)) call(FN_RELOAD);
    if (down(KF)) call(FN_POI_ACTION);
    if (down(KV) || down(KE) || down(KM3) || down(KM4)) call(FN_MELEE);
    if (down(K3) || down(KB)) call(FN_ALT_FIRE);
    if (down(K1) || down(K2) || down(KM5)) call(FN_SWAP_WEAPON), last_wheel_swap = now;
    if (k.wheel && now - last_wheel_swap >= WHEEL_GAP) call(FN_SWAP_WEAPON), last_wheel_swap = now;
    if (down(KC)) call(FN_CROUCH);
    if (down(KCTRL)) call(FN_PRONE);
    if (down(KSPACE)) {  // stand up
        u32 c = player_controller();
        u8 posture = c ? g::rd8(c + PC_POSTURE) : 0;
        if (posture & POSTURE_PRONE) call(FN_PRONE);
        else if (posture & POSTURE_CROUCH) call(FN_CROUCH);
    }
    if (down(KESC)) {
        release_all(pad, im);
        call(FN_PAD_RESET, {pad});
        call(FN_PAUSE, {state});
        play_until = 0;  // the cursor comes back for the pause menu right away
        pause_hold_until = now + AWAY;
    }
}
}  // namespace

void pad_on_ingame(u32 state) {
    if (!enabled) return;
    Input in;
    Kbm k;
    u32 b;
    u64 now;
    u32 pad = begin_update(ZOMBIES, in, k, b, now);
    u32 im = g::rd32(at(INPUT_MGR));
    if (pad && valid(im)) {
        if (source != applied) release_all(pad, im), applied = source;
        if (source == PAD) pad_zombies(state, pad, im, in, b, now);
        else if (source == KBM) kbm_zombies(state, pad, im, k, now);
        else release_all(pad, im);
    }
    fit_prompt(source != TOUCH, now);
    if (pad) show_touch_controls(source == TOUCH, now);
    prev = b;
    keys_prev = k.keys;
}

void pad_on_prompt(u32 element) {
    if (element != prompt) LOGI("HUD prompt element %08x", element);
    prompt = element;
    prompt_shown = now_ns();
}

void pad_on_dead_ops(u32 state) {
    if (!enabled) return;
    Input in;
    Kbm k;
    u32 b;
    u64 now;
    u32 pad = begin_update(DEAD_OPS, in, k, b, now);
    if (pad && source != TOUCH) {
        // Like the PC's keys and mouse, an event each update while moving or shooting and none otherwise (the player
        // stops when the events stop). Android's stick y already points down the screen, as Dead Ops expects.
        float mx = 0, my = 0, ax = 0, ay = 0;
        bool bomb = false, boost = false, pause = false;
        if (source == PAD) {
            auto down = [&](u32 m) { return (b & m) && !(prev & m); };
            shape(in.lx, in.ly, MOVE_DEADZONE, 1.0f, move_scale, mx, my);
            shape(in.rx, in.ry, AIM_DEADZONE, 1.0f, 1.0f, ax, ay);
            bomb = down(B_RB) || down(B_Y);
            boost = down(B_LB) || down(B_LT) || down(B_A);
            pause = down(B_START);
        } else {
            auto held = [&](Key i) { return (((k.keys & ~keys_blocked) >> i) & 1) != 0; };
            auto down = [&](Key i) { return held(i) && !((keys_prev >> i) & 1); };
            mx = static_cast<float>(held(KD) - held(KA));
            my = static_cast<float>(held(KS) - held(KW));
            ax = static_cast<float>(held(KRIGHT) - held(KLEFT));
            ay = static_cast<float>(held(KDOWN) - held(KUP));
            if (ax == 0 && ay == 0 && held(KM1)) ax = k.cx, ay = k.cy;  // toward the cursor
            bomb = down(KG) || down(KM2) || k.wheel != 0;
            boost = down(KSPACE);
            pause = down(KESC);
        }
        float m = std::sqrt(mx * mx + my * my);
        if (m > 1) mx /= m, my /= m;
        if (mx != 0 || my != 0) post_pad(pad, AP_LEFT, 0, mx, my);
        float r = std::sqrt(ax * ax + ay * ay);
        if (r > 0) post_pad(pad, AP_RIGHT, 1, ax / r, ay / r);  // aim at full strength, as the mouse does
        moving = looking = false;  // nothing to let go of later
        u32 pi = g::rd32(at(DO_PLAYER_INPUT));
        if (valid(pi)) {
            if (bomb) call(FN_DO_ACTION, {pi, DO_BOMB});
            if (boost) call(FN_DO_ACTION, {pi, DO_BOOST});
        }
        if (pause) {
            call(FN_PAD_RESET, {pad});
            call(FN_DO_PAUSE, {state});
        }
    }
    prev = b;
    keys_prev = k.keys;
}

// The mouse turns the view: its motion goes straight onto the player's yaw and pitch, which the view update (about
// to run) rebuilds the view from. The view update only does that when the player looked this update, so "looked"
// is set too, with the right pad's turn rates zeroed unless a pad really moved (they are left over otherwise).
void pad_on_look(u32 player) {
    float dx, dy;
    {
        std::lock_guard lk(kbm_lock);
        dx = kbm.dx;
        dy = kbm.dy;
        kbm.dx = kbm.dy = 0;
    }
    if (!enabled || (dx == 0 && dy == 0) || mode != ZOMBIES || source != KBM || !valid(player)) return;
    // the game is steering the view itself
    if (!g::rd8(player + PC_CAN_LOOK) || g::rd32(player + PC_PITCH_GLIDE + 12)) return;
    u32 lock = g::rd32(at(VIEW_LOCK));
    if (valid(lock) && ((g::rd32(lock + 0xd4) >> 17) & 1)) return;

    Settings s = settings();
    float k = s.mouse_sensitivity * MOUSE_RAD;
    if (call(FN_IS_AIMING, {player})) k *= s.mouse_ads / 100.0f;
    u32 opts = g::rd32(at(OPTIONS));
    if (valid(opts) && g::rd8(opts + OPTIONS_FLIP_Y)) dy = -dy;  // the game's "invert look" option
    float yaw = g::rdf(player + PC_YAW) - dx * k;
    if (yaw > TWO_PI) yaw -= TWO_PI;  // as the game wraps it (it lets it go below 0)
    float pitch = g::rdf(player + PC_PITCH) + dy * k;
    pitch = std::max(-1.5f, std::min(1.5f, pitch));  // the view update clamps it to MaxPitch
    g::wrf(player + PC_YAW, yaw);
    g::wrf(player + PC_PITCH, pitch);
    if (g::rdf(player + PC_LOOK_SUM) == 0) {
        g::wrf(player + PC_LOOK_X, 0);
        g::wrf(player + PC_LOOK_Y, 0);
    }
    g::wr8(player + PC_LOOKED, 1);
}
