// The game window and the UI thread for the Unix PC builds, through SDL3 (window_win.cpp is the Windows version and
// the model for this one; behaviour should match it):
//   - the main thread runs SDL's event loop and stands in for Android's main thread (input, surface and lifecycle
//     callbacks, and work posted to "the OS thread");
//   - in menus the mouse is a touch and keys go to the game as Android key presses; during zombies play the mouse is
//     locked (SDL's relative mode) and turns the view, and in Dead Ops Arcade the pointer stays to aim with;
//   - every key and button change also goes to pad.cpp, so nothing it tracks stays held;
//   - a controller (any SDL knows: Xbox, PlayStation, Switch and others) works as java/boz64/Pad.java makes it work on
//     the phone: its state goes to pad.cpp during play, and in menus the left stick or D-pad moves the pointer, A
//     touches the screen there, and B or Start is Back.
#include <SDL3/SDL.h>

#include <algorithm>
#include <bitset>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <mutex>

#include <sys/system_properties.h>

#include "common.h"
#include "pad.h"
#include "platform.h"

namespace {

constexpr u64 CHECK_MS = 15;  // how often to check the mouse lock and read the controller (window_win.cpp's timer)

SDL_Window* g_win = nullptr;
SDL_ThreadID g_ui_tid = 0;
u32 g_run_event = 0;  // SDL user event: run the queued functions
int g_w = 0, g_h = 0;
int g_exit_code = 0;
bool g_running = true;

std::mutex q_lock;
std::deque<std::function<void()>> q;

void run_queue() {
    for (;;) {
        std::function<void()> f;
        {
            std::lock_guard lk(q_lock);
            if (q.empty()) return;
            f = std::move(q.front());
            q.pop_front();
        }
        f();
    }
}

// Physical keys -> Android KeyEvent codes (by position, so WASD stays WASD on any layout)
int android_key(SDL_Scancode sc) {
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return 29 + (sc - SDL_SCANCODE_A);
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return 8 + (sc - SDL_SCANCODE_1);
    if (sc >= SDL_SCANCODE_KP_1 && sc <= SDL_SCANCODE_KP_9) return 145 + (sc - SDL_SCANCODE_KP_1);
    if (sc >= SDL_SCANCODE_F1 && sc <= SDL_SCANCODE_F12) return 131 + (sc - SDL_SCANCODE_F1);
    switch (sc) {
    case SDL_SCANCODE_0: return 7;
    case SDL_SCANCODE_KP_0: return 144;
    case SDL_SCANCODE_ESCAPE: return 4;  // BACK (the game's pause / back)
    case SDL_SCANCODE_BACKSPACE: return 67;
    case SDL_SCANCODE_RETURN: return 66;
    case SDL_SCANCODE_KP_ENTER: return 160;
    case SDL_SCANCODE_SPACE: return 62;
    case SDL_SCANCODE_TAB: return 61;
    case SDL_SCANCODE_UP: return 19;
    case SDL_SCANCODE_DOWN: return 20;
    case SDL_SCANCODE_LEFT: return 21;
    case SDL_SCANCODE_RIGHT: return 22;
    case SDL_SCANCODE_LSHIFT:
    case SDL_SCANCODE_RSHIFT: return 59;
    case SDL_SCANCODE_LCTRL:
    case SDL_SCANCODE_RCTRL: return 113;
    case SDL_SCANCODE_LALT:
    case SDL_SCANCODE_RALT: return 57;
    case SDL_SCANCODE_DELETE: return 112;
    case SDL_SCANCODE_HOME: return 122;
    case SDL_SCANCODE_END: return 123;
    case SDL_SCANCODE_PAGEUP: return 92;
    case SDL_SCANCODE_PAGEDOWN: return 93;
    case SDL_SCANCODE_INSERT: return 124;
    case SDL_SCANCODE_APPLICATION: return 82;  // MENU
    case SDL_SCANCODE_COMMA: return 55;
    case SDL_SCANCODE_PERIOD: return 56;
    case SDL_SCANCODE_MINUS: return 69;
    case SDL_SCANCODE_EQUALS: return 70;
    case SDL_SCANCODE_SEMICOLON: return 74;
    case SDL_SCANCODE_SLASH: return 76;
    case SDL_SCANCODE_GRAVE: return 68;
    case SDL_SCANCODE_LEFTBRACKET: return 71;
    case SDL_SCANCODE_BACKSLASH: return 73;
    case SDL_SCANCODE_RIGHTBRACKET: return 72;
    case SDL_SCANCODE_APOSTROPHE: return 75;
    default: return 0;
    }
}

// The character a key types on the user's layout (printable ASCII only: the game's text needs no more)
int unicode_of(const SDL_KeyboardEvent& k) {
    SDL_Keycode c = k.key;
    if (c < 0x20 || c >= 0x7f) return 0;
    bool upper = ((k.mod & SDL_KMOD_SHIFT) != 0) != ((k.mod & SDL_KMOD_CAPS) != 0);
    if (upper && c >= 'a' && c <= 'z') c -= 'a' - 'A';
    return static_cast<int>(c);
}

bool g_mouse_down = false;  // a touch from the left button is in progress
int g_mouse_x = 0, g_mouse_y = 0;
bool g_captured = false;    // the pointer is hidden and locked (zombies play)
float g_wheel_rest = 0;     // wheel motion short of a notch (precise wheels and touchpads)
std::bitset<512> g_marm_down;  // Android key codes the game has seen go down

bool in_front() {
    if (!g_win) return false;
    SDL_WindowFlags f = SDL_GetWindowFlags(g_win);
    return (f & SDL_WINDOW_INPUT_FOCUS) && !(f & SDL_WINDOW_MINIMIZED);
}

// The pointer's look over the window (decided in check_pointer)
SDL_Cursor* g_arrow = nullptr;
SDL_Cursor* g_cross = nullptr;
bool g_pointer_hidden = false;  // hidden over the window for the controller

void set_captured(bool on) {
    if (on == g_captured) return;
    if (on && g_mouse_down) {  // the click that resumed play: let the touch end
        g_mouse_down = false;
        marm_touch(0, 5, g_mouse_x, g_mouse_y);
    }
    g_captured = on;
    SDL_SetWindowRelativeMouseMode(g_win, on);
    LOGI("mouse: %s", on ? "locked" : "let go");
    if (!on && in_front()) SDL_WarpMouseInWindow(g_win, g_w / 2.0f, g_h / 2.0f);  // back in the middle (pause menu)
}

bool g_no_capture = false;  // debug.boz64.nocapture=1: never capture (test copies running next to someone at work)
void check_capture() { set_captured(!g_no_capture && kbm_mouse_mode() == KBM_MOUSE_LOOK && in_front()); }

// ---- the controller ----
// Buttons as Pad.java sends them (pad.h)
constexpr u32 PB_A = 1, PB_B = 2, PB_START = 256, PB_UP = 1024, PB_DOWN = 2048, PB_LEFT = 4096, PB_RIGHT = 8192;
constexpr struct { SDL_GamepadButton sdl; u32 bit; } PAD_BUTTONS[] = {
    {SDL_GAMEPAD_BUTTON_SOUTH, PB_A},           {SDL_GAMEPAD_BUTTON_EAST, PB_B},
    {SDL_GAMEPAD_BUTTON_WEST, 4},               {SDL_GAMEPAD_BUTTON_NORTH, 8},
    {SDL_GAMEPAD_BUTTON_LEFT_SHOULDER, 16},     {SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER, 32},
    {SDL_GAMEPAD_BUTTON_LEFT_STICK, 64},        {SDL_GAMEPAD_BUTTON_RIGHT_STICK, 128},
    {SDL_GAMEPAD_BUTTON_START, PB_START},       {SDL_GAMEPAD_BUTTON_BACK, 512},
    {SDL_GAMEPAD_BUTTON_DPAD_UP, PB_UP},        {SDL_GAMEPAD_BUTTON_DPAD_DOWN, PB_DOWN},
    {SDL_GAMEPAD_BUTTON_DPAD_LEFT, PB_LEFT},    {SDL_GAMEPAD_BUTTON_DPAD_RIGHT, PB_RIGHT},
};
constexpr float CURSOR_DEADZONE = 0.2f, CURSOR_DPAD = 0.6f;
constexpr float CURSOR_SPEED = 1.1f;  // full stick: about a window height a second
constexpr float CURSOR_MAX_DT = 0.05f;

SDL_Gamepad* g_pad = nullptr;
bool g_pad_fresh = true;      // send the next state even if it is the same
u32 g_pad_blocked = ~0u;      // buttons held since before the window had focus: up until released (as on Android)
u32 g_pad_buttons = 0;        // as last sent
float g_pad_last[6] = {};     // sticks and triggers as last sent
bool g_pad_sent = false;      // something other than all-zero was sent
float g_pad_lx = 0, g_pad_ly = 0;  // the left stick, as last sent (it moves the menu cursor)
bool g_pad_touching = false;  // A is touching the screen at the cursor (menus)
float g_cur_x = 0, g_cur_y = 0;  // the menu cursor in window pixels, with the fraction the pointer can't show
int g_cur_put_x = -1, g_cur_put_y = -1;  // where the cursor last put the pointer: its motion isn't the mouse
u64 g_cur_time = 0;           // when the cursor last moved (ns)
bool g_pad_in_control = false;  // the controller was used after the mouse: it hides the pointer during play
bool g_was_in_game = false;

u64 steady_ns() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

float axis(SDL_GamepadAxis a) { return std::max(-1.0f, SDL_GetGamepadAxis(g_pad, a) / 32767.0f); }

void pointer_in_window(int& x, int& y) {
    float fx = g_w / 2.0f, fy = g_h / 2.0f;
    SDL_GetMouseState(&fx, &fy);
    x = std::clamp(static_cast<int>(fx), 0, std::max(0, g_w - 1));
    y = std::clamp(static_cast<int>(fy), 0, std::max(0, g_h - 1));
}

void put_pointer(float x, float y) {
    g_cur_put_x = static_cast<int>(std::lround(x));
    g_cur_put_y = static_cast<int>(std::lround(y));
    SDL_WarpMouseInWindow(g_win, static_cast<float>(g_cur_put_x), static_cast<float>(g_cur_put_y));
}

// Where the menu cursor is: the pointer, unless the pointer is still where the cursor put it (keep the fraction).
void sync_cursor() {
    int x, y;
    pointer_in_window(x, y);
    if (x != g_cur_put_x || y != g_cur_put_y) {
        g_cur_x = static_cast<float>(x);
        g_cur_y = static_cast<float>(y);
        put_pointer(g_cur_x, g_cur_y);
    }
}

// A touch at the cursor, as the game's own touch handling would send it (4 down, 5 up, 6 move)
void cursor_touch(int action) {
    if (action == 4) {
        if (g_mouse_down) return;  // the mouse is touching
        sync_cursor();
    }
    g_pad_touching = action != 5;
    marm_touch(0, action, static_cast<int>(std::lround(g_cur_x)), static_cast<int>(std::lround(g_cur_y)));
}

void end_cursor_touch() {
    if (g_pad_touching) cursor_touch(5);
}

void pad_back() {  // the game's Back (Android's KEYCODE_BACK), pressed and released
    marm_key(4, 0, true);
    marm_key(4, 0, false);
}

// Nothing from the controller is held (focus lost, controller gone, window closing)
void pad_clear() {
    end_cursor_touch();
    if (g_pad_sent) pad_input(0, 0, 0, 0, 0, 0, 0);
    g_pad_sent = false;
    g_pad_buttons = 0;
    g_pad_lx = g_pad_ly = 0;
    std::fill(std::begin(g_pad_last), std::end(g_pad_last), 0.0f);
    g_cur_time = 0;
}

void pad_open_first() {
    if (g_pad) return;
    int n = 0;
    SDL_JoystickID* ids = SDL_GetGamepads(&n);
    if (ids && n > 0) g_pad = SDL_OpenGamepad(ids[0]);
    SDL_free(ids);
    if (g_pad) {
        LOGI("controller: %s connected", SDL_GetGamepadName(g_pad));
        g_pad_fresh = true;
    }
}

bool pad_hides_pointer() { return g_pad_in_control && pad_in_game() && kbm_mouse_mode() == KBM_MOUSE_TOUCH; }

// Reads the controller (every CHECK_MS): sends changes to pad.cpp; outside play, runs the menu cursor.
void pad_poll() {
    if (!g_pad) return;
    bool front = in_front();
    bool in_game = pad_in_game();
    if (in_game) end_cursor_touch();  // play started during a touch (A on "Resume")
    if (g_was_in_game && !in_game && g_pad_in_control && front) {  // paused with the controller: cursor in the middle
        g_cur_x = g_w / 2.0f;
        g_cur_y = g_h / 2.0f;
        put_pointer(g_cur_x, g_cur_y);
    }
    g_was_in_game = in_game;
    if (!front) return;  // the controller is the focused window's, as on Android (lost_focus cleared it)

    u32 raw = 0;
    for (const auto& m : PAD_BUTTONS)
        if (SDL_GetGamepadButton(g_pad, m.sdl)) raw |= m.bit;
    g_pad_blocked &= raw;
    u32 b = raw & ~g_pad_blocked;
    // SDL's stick Y is already positive downwards (XInput's is upwards, so window_win.cpp negates it)
    float v[6] = {axis(SDL_GAMEPAD_AXIS_LEFTX), axis(SDL_GAMEPAD_AXIS_LEFTY), axis(SDL_GAMEPAD_AXIS_RIGHTX),
                  axis(SDL_GAMEPAD_AXIS_RIGHTY), std::max(0.0f, axis(SDL_GAMEPAD_AXIS_LEFT_TRIGGER)),
                  std::max(0.0f, axis(SDL_GAMEPAD_AXIS_RIGHT_TRIGGER))};
    if (g_pad_fresh || b != g_pad_buttons || !std::equal(std::begin(v), std::end(v), std::begin(g_pad_last))) {
        g_pad_fresh = false;
        u32 before = g_pad_buttons;
        g_pad_buttons = b;
        std::copy(std::begin(v), std::end(v), std::begin(g_pad_last));
        g_pad_lx = v[0], g_pad_ly = v[1];
        pad_input(b, v[0], v[1], v[2], v[3], v[4], v[5]);
        g_pad_sent = true;
        // as pad.cpp decides the controller is in use
        float mag = std::max(std::max(std::fabs(v[0]), std::fabs(v[1])), std::max(std::fabs(v[2]), std::fabs(v[3])));
        if (b || mag > 0.5f || v[4] >= 0.5f || v[5] >= 0.5f) g_pad_in_control = true;
        if (!in_game) {
            u32 pressed = b & ~before, released = before & ~b;
            if (pressed & PB_A) cursor_touch(4);
            if ((released & PB_A) && g_pad_touching) cursor_touch(5);
            if (pressed & (PB_B | PB_START)) pad_back();
        }
    }

    // the menu cursor: the left stick or the D-pad move the pointer
    u64 now = steady_ns();
    float dt = g_cur_time ? std::min(CURSOR_MAX_DT, (now - g_cur_time) / 1e9f) : 0;
    g_cur_time = now;
    if (in_game) return;
    float sx = g_pad_lx, sy = g_pad_ly;
    if (std::sqrt(sx * sx + sy * sy) < CURSOR_DEADZONE) sx = sy = 0;
    if (g_pad_buttons & PB_LEFT) sx = -CURSOR_DPAD;
    if (g_pad_buttons & PB_RIGHT) sx = CURSOR_DPAD;
    if (g_pad_buttons & PB_UP) sy = -CURSOR_DPAD;
    if (g_pad_buttons & PB_DOWN) sy = CURSOR_DPAD;
    if (sx == 0 && sy == 0) return;
    sync_cursor();
    float speed = g_h * CURSOR_SPEED;
    g_cur_x = std::clamp(g_cur_x + sx * std::fabs(sx) * speed * dt, 0.0f, static_cast<float>(std::max(0, g_w - 1)));
    g_cur_y = std::clamp(g_cur_y + sy * std::fabs(sy) * speed * dt, 0.0f, static_cast<float>(std::max(0, g_h - 1)));
    put_pointer(g_cur_x, g_cur_y);
    if (g_pad_touching) marm_touch(0, 6, static_cast<int>(std::lround(g_cur_x)), static_cast<int>(std::lround(g_cur_y)));
}

// After pad_poll: the pointer's look follows the mode (crosshair in Dead Ops), and it hides while the controller
// is in control of play. (Relative mode hides it on its own while captured.)
void check_pointer() {
    bool hide = pad_hides_pointer();
    if (hide != g_pointer_hidden) {
        g_pointer_hidden = hide;
        if (hide) SDL_HideCursor();
        else SDL_ShowCursor();
    }
    SDL_Cursor* want = kbm_mouse_mode() == KBM_MOUSE_AIM ? g_cross : g_arrow;
    if (want && SDL_GetCursor() != want) SDL_SetCursor(want);
}

void lost_focus() {
    set_captured(false);
    kbm_release_all();
    pad_clear();
    g_pad_blocked = ~0u;  // buttons still held when focus comes back do nothing until released
    g_pad_fresh = true;
}

// The mouse moved: unless the menu cursor moved it, the mouse is in control of the pointer again.
void mouse_moved(int x, int y) {
    bool moved = x != g_mouse_x || y != g_mouse_y;
    g_mouse_x = x;
    g_mouse_y = y;
    if (moved && (x != g_cur_put_x || y != g_cur_put_y)) {
        g_cur_put_x = g_cur_put_y = -1;
        g_pad_in_control = false;
    }
}

// A mouse button: always to pad.cpp; the left one is also a touch outside play.
void mouse_button(int code, bool down, float fx, float fy) {
    kbm_key(code, down);
    if (code != KEY_MOUSE_LEFT || g_captured) return;
    g_mouse_x = static_cast<int>(fx);
    g_mouse_y = static_cast<int>(fy);
    if (down) {
        if (kbm_mouse_mode() != KBM_MOUSE_TOUCH) return;
        SDL_CaptureMouse(true);
        g_mouse_down = true;
        marm_touch(0, 4, g_mouse_x, g_mouse_y);
    } else if (g_mouse_down) {
        g_mouse_down = false;
        SDL_CaptureMouse(false);
        marm_touch(0, 5, g_mouse_x, g_mouse_y);
    }
}

int mouse_code(Uint8 button) {
    switch (button) {
    case SDL_BUTTON_LEFT: return KEY_MOUSE_LEFT;
    case SDL_BUTTON_RIGHT: return KEY_MOUSE_RIGHT;
    case SDL_BUTTON_MIDDLE: return KEY_MOUSE_MIDDLE;
    case SDL_BUTTON_X1: return KEY_MOUSE_BACK;
    case SDL_BUTTON_X2: return KEY_MOUSE_FORWARD;
    default: return 0;
    }
}

void size_changed() {
    int w = 0, h = 0;
    SDL_GetWindowSize(g_win, &w, &h);
    marm_resume();
    if (w > 0 && h > 0 && (w != g_w || h != g_h)) {
        g_w = w;
        g_h = h;
        marm_surface_changed(w, h);
    }
}

void handle(const SDL_Event& e) {
    if (e.type == g_run_event) {
        run_queue();
        return;
    }
    switch (e.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        marm_shutdown();
        set_captured(false);
        pad_clear();
        g_running = false;
        break;
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        bool down = e.type == SDL_EVENT_KEY_DOWN;
        if (down && e.key.repeat) break;
        int code = android_key(e.key.scancode);
        if (!code) break;
        kbm_key(code, down);
        // to the game as a key press outside play, and its release whenever the game saw it go down
        if (down && kbm_mouse_mode() == KBM_MOUSE_TOUCH && code < 512) {
            g_marm_down.set(code);
            marm_key(code, unicode_of(e.key), true);
        } else if (!down && code < 512 && g_marm_down.test(code)) {
            g_marm_down.reset(code);
            marm_key(code, 0, false);
        }
        break;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        if (int code = mouse_code(e.button.button))
            mouse_button(code, e.type == SDL_EVENT_MOUSE_BUTTON_DOWN, e.button.x, e.button.y);
        break;
    case SDL_EVENT_MOUSE_MOTION:
        if (g_captured) {  // relative motion turns the view
            kbm_mouse_move(e.motion.xrel, e.motion.yrel);
            break;
        }
        mouse_moved(static_cast<int>(e.motion.x), static_cast<int>(e.motion.y));
        kbm_cursor(static_cast<float>(g_mouse_x - g_w / 2), static_cast<float>(g_mouse_y - g_h / 2));
        if (g_mouse_down) marm_touch(0, 6, g_mouse_x, g_mouse_y);
        break;
    case SDL_EVENT_MOUSE_WHEEL: {
        float y = e.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -e.wheel.y : e.wheel.y;
        g_wheel_rest += y;
        int n = static_cast<int>(g_wheel_rest);  // toward zero: whole notches only
        g_wheel_rest -= static_cast<float>(n);
        if (n) kbm_wheel(n);
        break;
    }
    case SDL_EVENT_WINDOW_FOCUS_LOST: lost_focus(); break;
    case SDL_EVENT_WINDOW_FOCUS_GAINED: check_capture(); break;
    case SDL_EVENT_WINDOW_MINIMIZED: marm_suspend(); break;
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_MAXIMIZED: size_changed(); break;
    case SDL_EVENT_GAMEPAD_ADDED: pad_open_first(); break;
    case SDL_EVENT_GAMEPAD_REMOVED:
        if (g_pad && SDL_GetGamepadID(g_pad) == e.gdevice.which) {
            LOGI("controller: disconnected");
            SDL_CloseGamepad(g_pad);
            g_pad = nullptr;
            pad_clear();
            pad_open_first();  // another one may be connected
        }
        break;
    default: break;
    }
}

}  // namespace

bool win_create(int width, int height) {
    g_ui_tid = SDL_GetCurrentThreadID();
    // EGL everywhere (also on X11), so the game's own EGL calls see SDL's display and context (egl_sdl.cpp)
    SDL_SetHint(SDL_HINT_VIDEO_FORCE_EGL, "1");
    SDL_SetHint(SDL_HINT_OPENGL_ES_DRIVER, "1");
    SDL_SetHint(SDL_HINT_APP_NAME, "BOZ64");
    SDL_SetHint(SDL_HINT_APP_ID, "io.github.nebuiacore.BOZ64");  // matches the .desktop file, for the window's icon
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_EVENTS)) {
        LOGE("SDL could not start: %s", SDL_GetError());
        return false;
    }
    g_run_event = SDL_RegisterEvents(1);
    // the pixel format, fixed when the window is made: at least what the game's [GL] settings ask for
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    g_win = SDL_CreateWindow("Call of Duty: Black Ops Zombies (PC)", width, height, SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE);
    if (!g_win) {
        LOGE("SDL could not make the window: %s", SDL_GetError());
        return false;
    }
    SDL_GetWindowSize(g_win, &g_w, &g_h);
    char v[PROP_VALUE_MAX] = {};
    g_no_capture = __system_property_get("debug.boz64.nocapture", v) > 0 && v[0] == '1';
    g_arrow = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_DEFAULT);
    g_cross = SDL_CreateSystemCursor(SDL_SYSTEM_CURSOR_CROSSHAIR);
    pad_open_first();
    if (!g_pad) LOGI("controller: none connected yet");
    LOGI("window %dx%d (SDL %d.%d.%d, %s)", g_w, g_h, SDL_VERSIONNUM_MAJOR(SDL_GetVersion()),
         SDL_VERSIONNUM_MINOR(SDL_GetVersion()), SDL_VERSIONNUM_MICRO(SDL_GetVersion()), SDL_GetCurrentVideoDriver());
    return true;
}

int win_run_loop() {
    u64 next_check = 0;
    while (g_running) {
        SDL_Event e;
        u64 now = SDL_GetTicks();
        if (SDL_WaitEventTimeout(&e, static_cast<Sint32>(next_check > now ? next_check - now : 0))) {
            handle(e);
            while (g_running && SDL_PollEvent(&e)) handle(e);
        }
        now = SDL_GetTicks();
        if (now >= next_check) {
            next_check = now + CHECK_MS;
            check_capture();
            pad_poll();
            check_pointer();
        }
    }
    run_queue();
    return g_exit_code;
}

void win_quit(int code) {
    g_exit_code = code;
    ui_post([] { g_running = false; });
}

int win_width() { return g_w; }
int win_height() { return g_h; }
void* win_hwnd() { return g_win; }
bool on_ui_thread() { return SDL_GetCurrentThreadID() == g_ui_tid; }

void ui_post(std::function<void()> f) {
    {
        std::lock_guard lk(q_lock);
        q.push_back(std::move(f));
    }
    SDL_Event e{};
    e.type = g_run_event;
    SDL_PushEvent(&e);
}

void ui_call(std::function<void()> f) {
    if (on_ui_thread()) {
        f();
        return;
    }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    ui_post([&] {
        f();
        std::lock_guard lk(m);
        done = true;
        cv.notify_all();
    });
    std::unique_lock lk(m);
    cv.wait(lk, [&] { return done; });
}

void win_set_title(const std::string& s) {
    ui_post([s] { SDL_SetWindowTitle(g_win, s.c_str()); });
}
