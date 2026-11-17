// The game window and the UI thread: the Win32 message loop stands in for Android's main thread (input,
// surface and lifecycle callbacks, and work posted to "the OS thread").
// Input: in menus the mouse is a touch and keys go to the game as Android key presses. During play the keyboard and
// mouse are game controls (src/pad.cpp): in zombies the cursor is hidden and locked in the middle of the window while
// the window is in front (as other PC shooters do), and raw mouse motion turns the view; in Dead Ops Arcade the cursor
// stays (the player shoots toward it).
// Every key and button change also goes to pad.cpp outside play, so nothing it tracks stays held.
// A controller (XInput: Xbox controllers, and others through Steam Input or similar) works as java/boz64/Pad.java
// makes it work on the phone: its state goes to pad.cpp (the game's controls during play), and in menus the left
// stick or D-pad moves the mouse pointer, A touches the screen there, and B or Start is Back. While the controller
// is in control during play, the pointer is hidden over the window.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <windowsx.h>
#include <xinput.h>

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

constexpr UINT WM_APP_RUN = WM_APP + 1;  // run the queued functions
constexpr UINT WM_APP_MOUSE_MOTION = WM_APP + 2;  // for test scripts: mouse motion (wParam dx, lParam dy, as ints)
constexpr UINT_PTR TIMER_MOUSE = 1;      // checks whether the mouse should be captured, and reads the controller
constexpr UINT MOUSE_CHECK_MS = 15;

HWND g_hwnd = nullptr;
DWORD g_ui_tid = 0;
int g_w = 0, g_h = 0;
int g_exit_code = 0;

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

// Windows virtual keys -> Android KeyEvent codes
int android_key(WPARAM vk, LPARAM lp) {
    if (vk >= 'A' && vk <= 'Z') return 29 + static_cast<int>(vk - 'A');
    if (vk >= '0' && vk <= '9') return 7 + static_cast<int>(vk - '0');
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return 144 + static_cast<int>(vk - VK_NUMPAD0);
    if (vk >= VK_F1 && vk <= VK_F12) return 131 + static_cast<int>(vk - VK_F1);
    bool ext = (lp >> 24) & 1;
    switch (vk) {
    case VK_ESCAPE: return 4;  // BACK (the game's pause / back)
    case VK_BACK: return 67;   // DEL
    case VK_RETURN: return ext ? 160 : 66;
    case VK_SPACE: return 62;
    case VK_TAB: return 61;
    case VK_UP: return 19;
    case VK_DOWN: return 20;
    case VK_LEFT: return 21;
    case VK_RIGHT: return 22;
    case VK_SHIFT: return 59;
    case VK_CONTROL: return 113;
    case VK_MENU: return 57;
    case VK_DELETE: return 112;
    case VK_HOME: return 122;
    case VK_END: return 123;
    case VK_PRIOR: return 92;
    case VK_NEXT: return 93;
    case VK_INSERT: return 124;
    case VK_APPS: return 82;  // MENU
    case VK_OEM_COMMA: return 55;
    case VK_OEM_PERIOD: return 56;
    case VK_OEM_MINUS: return 69;
    case VK_OEM_PLUS: return 70;
    case VK_OEM_1: return 74;
    case VK_OEM_2: return 76;
    case VK_OEM_3: return 68;
    case VK_OEM_4: return 71;
    case VK_OEM_5: return 73;
    case VK_OEM_6: return 72;
    case VK_OEM_7: return 75;
    default: return 0;
    }
}

int unicode_of(WPARAM vk, LPARAM lp) {
    BYTE ks[256];
    if (!GetKeyboardState(ks)) return 0;
    wchar_t buf[4] = {};
    int n = ToUnicode(static_cast<UINT>(vk), (lp >> 16) & 0xff, ks, buf, 4, 4);  // flag 4: keep the dead-key state
    return n == 1 && buf[0] >= 0x20 ? buf[0] : 0;
}

bool g_mouse_down = false;  // a touch from the left button is in progress
int g_mouse_x = 0, g_mouse_y = 0;
bool g_captured = false;     // the cursor is hidden and locked in the middle of the window (zombies play)
bool g_modal = false;        // the window is being moved or sized, or its system menu is open: no lock
int g_wheel_rest = 0;        // wheel motion short of a notch (precise wheels and touchpads)
std::bitset<512> g_marm_down;  // Android key codes the game has seen go down

bool in_front() { return g_hwnd && GetForegroundWindow() == g_hwnd && !IsIconic(g_hwnd); }

RECT client_on_screen() {
    RECT r;
    GetClientRect(g_hwnd, &r);
    MapWindowPoints(g_hwnd, nullptr, reinterpret_cast<POINT*>(&r), 2);
    return r;
}

void set_captured(bool on) {
    if (on) {
        if (!g_captured && g_mouse_down) {  // the click that resumed play: let the touch end
            g_mouse_down = false;
            ReleaseCapture();
            marm_touch(0, 5, g_mouse_x, g_mouse_y);
        }
        // held on the middle pixel: it can't reach the window's edges, the taskbar or another screen, and it is in the
        // middle when it comes back. Again on every check: the window may have moved, and other programs can reset it.
        RECT r = client_on_screen();
        LONG cx = (r.left + r.right) / 2, cy = (r.top + r.bottom) / 2;
        RECT mid{cx, cy, cx + 1, cy + 1};
        ClipCursor(&mid);
        if (!g_captured) {
            SetCursorPos(cx, cy);
            SetCursor(nullptr);
            LOGI("mouse: locked");
        }
        g_captured = true;
        return;
    }
    if (!g_captured) return;
    g_captured = false;
    ClipCursor(nullptr);
    LOGI("mouse: let go");
    if (in_front()) {  // play stopped (pause menu): the cursor comes back in the middle
        RECT r = client_on_screen();
        SetCursorPos((r.left + r.right) / 2, (r.top + r.bottom) / 2);
    }
    SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)));
}

bool g_no_capture = false;  // debug.boz64.nocapture=1: never capture (test copies running next to someone at work)
void check_capture() {
    set_captured(!g_no_capture && !g_modal && kbm_mouse_mode() == KBM_MOUSE_LOOK && in_front());
}

// ---- the controller (XInput) ----
// Buttons as Pad.java sends them (pad.h)
constexpr u32 PB_A = 1, PB_B = 2, PB_START = 256, PB_UP = 1024, PB_DOWN = 2048, PB_LEFT = 4096, PB_RIGHT = 8192;
constexpr struct { WORD xinput; u32 bit; } PAD_BUTTONS[] = {
    {XINPUT_GAMEPAD_A, PB_A},           {XINPUT_GAMEPAD_B, PB_B},
    {XINPUT_GAMEPAD_X, 4},              {XINPUT_GAMEPAD_Y, 8},
    {XINPUT_GAMEPAD_LEFT_SHOULDER, 16}, {XINPUT_GAMEPAD_RIGHT_SHOULDER, 32},
    {XINPUT_GAMEPAD_LEFT_THUMB, 64},    {XINPUT_GAMEPAD_RIGHT_THUMB, 128},
    {XINPUT_GAMEPAD_START, PB_START},   {XINPUT_GAMEPAD_BACK, 512},
    {XINPUT_GAMEPAD_DPAD_UP, PB_UP},    {XINPUT_GAMEPAD_DPAD_DOWN, PB_DOWN},
    {XINPUT_GAMEPAD_DPAD_LEFT, PB_LEFT}, {XINPUT_GAMEPAD_DPAD_RIGHT, PB_RIGHT},
};
constexpr ULONGLONG PAD_SCAN_MS = 1500;  // with no controller, look for one this often (asking empty slots is slow)
constexpr float CURSOR_DEADZONE = 0.2f, CURSOR_DPAD = 0.6f;
constexpr float CURSOR_SPEED = 1.1f;     // full stick: about a window height a second
constexpr float CURSOR_MAX_DT = 0.05f;

using XInputGetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
XInputGetStateFn g_xinput = nullptr;
int g_pad_slot = -1;           // the XInput slot being read (-1: no controller)
DWORD g_pad_packet = 0;        // its last state's packet number
bool g_pad_fresh = true;       // send the next state even if its packet number is the same
ULONGLONG g_pad_scan_at = 0;   // when to look for a controller next
u32 g_pad_blocked = ~0u;       // buttons held since before the window had focus: up until released (as on Android)
u32 g_pad_buttons = 0;         // as last sent
bool g_pad_sent = false;       // something other than all-zero was sent
float g_pad_lx = 0, g_pad_ly = 0;  // the left stick, as last sent (it moves the menu cursor)
bool g_pad_touching = false;   // A is touching the screen at the cursor (menus)
float g_cur_x = 0, g_cur_y = 0;  // the menu cursor in client pixels, with the fraction the pointer can't show
POINT g_cur_put{-1, -1};       // where the cursor last put the pointer (client): its WM_MOUSEMOVE isn't the mouse
u64 g_cur_time = 0;            // when the cursor last moved (ns)
bool g_pad_in_control = false; // the controller was used after the mouse: it hides the pointer during play
bool g_pointer_hidden = false; // the pointer is hidden over the window for the controller
bool g_was_in_game = false;

u64 steady_ns() {
    return static_cast<u64>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
}

void pad_load() {
    // from the system folder only (not next to the .exe)
    static const struct { const wchar_t* w; const char* a; } DLLS[] = {
        {L"xinput1_4.dll", "xinput1_4"}, {L"xinput1_3.dll", "xinput1_3"}, {L"xinput9_1_0.dll", "xinput9_1_0"}};
    for (const auto& d : DLLS) {
        HMODULE m = LoadLibraryExW(d.w, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!m) continue;
        g_xinput = reinterpret_cast<XInputGetStateFn>(reinterpret_cast<void*>(GetProcAddress(m, "XInputGetState")));
        if (g_xinput) {
            LOGI("controller: XInput from %s", d.a);
            return;
        }
        FreeLibrary(m);
    }
    LOGW("controller: XInput not found; controllers won't work");
}

float stick(SHORT v) { return std::max(-1.0f, static_cast<float>(v) / 32767.0f); }

// The pointer in client pixels, kept inside the client area.
POINT pointer_in_client() {
    POINT p{g_w / 2, g_h / 2};
    if (GetCursorPos(&p)) ScreenToClient(g_hwnd, &p);
    p.x = std::clamp<LONG>(p.x, 0, std::max(0, g_w - 1));
    p.y = std::clamp<LONG>(p.y, 0, std::max(0, g_h - 1));
    return p;
}

void put_pointer(float x, float y) {
    POINT c{std::lround(x), std::lround(y)}, s = c;
    g_cur_put = c;
    ClientToScreen(g_hwnd, &s);
    SetCursorPos(s.x, s.y);
}

// Where the menu cursor is: the pointer, unless the pointer is still where the cursor put it (keep the fraction).
void sync_cursor() {
    POINT p = pointer_in_client();
    if (p.x != g_cur_put.x || p.y != g_cur_put.y) {
        g_cur_x = static_cast<float>(p.x);
        g_cur_y = static_cast<float>(p.y);
        put_pointer(g_cur_x, g_cur_y);  // into the window if it was outside
    }
}

// A touch at the cursor, as the game's own touch handling would send it (4 down, 5 up, 6 move)
void cursor_touch(int action) {
    if (action == 4) {
        if (g_mouse_down) return;  // the mouse is touching
        sync_cursor();
    }
    g_pad_touching = action != 5;
    marm_touch(0, action, std::lround(g_cur_x), std::lround(g_cur_y));
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
    g_cur_time = 0;
}

bool pad_hides_pointer() { return g_pad_in_control && pad_in_game() && kbm_mouse_mode() == KBM_MOUSE_TOUCH; }

bool pointer_over_client() {
    POINT p;
    if (!GetCursorPos(&p)) return false;
    RECT r = client_on_screen();
    return PtInRect(&r, p) && WindowFromPoint(p) == g_hwnd;
}

void cursor_for_window() {  // the pointer's look over the window right now
    if (g_captured || pad_hides_pointer()) SetCursor(nullptr);
    else SetCursor(LoadCursorW(nullptr, MAKEINTRESOURCEW(kbm_mouse_mode() == KBM_MOUSE_AIM ? 32515 : 32512)));
}

// Reads the controller (every MOUSE_CHECK_MS): sends changes to pad.cpp; outside play, runs the menu cursor.
void pad_poll() {
    if (!g_xinput) return;
    XINPUT_STATE s{};
    if (g_pad_slot < 0) {
        ULONGLONG t = GetTickCount64();
        if (t < g_pad_scan_at) return;
        g_pad_scan_at = t + PAD_SCAN_MS;
        for (DWORD i = 0; i < XUSER_MAX_COUNT && g_pad_slot < 0; i++)
            if (g_xinput(i, &s) == ERROR_SUCCESS) g_pad_slot = static_cast<int>(i);
        if (g_pad_slot < 0) return;
        LOGI("controller: XInput controller %d connected", g_pad_slot);
        g_pad_fresh = true;
    } else if (g_xinput(static_cast<DWORD>(g_pad_slot), &s) != ERROR_SUCCESS) {
        LOGI("controller: XInput controller %d disconnected", g_pad_slot);
        g_pad_slot = -1;
        g_pad_scan_at = 0;  // another one may be connected
        pad_clear();
        return;
    }
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

    if (g_pad_fresh || s.dwPacketNumber != g_pad_packet) {
        g_pad_fresh = false;
        g_pad_packet = s.dwPacketNumber;
        const XINPUT_GAMEPAD& gp = s.Gamepad;
        u32 raw = 0;
        for (const auto& m : PAD_BUTTONS)
            if (gp.wButtons & m.xinput) raw |= m.bit;
        g_pad_blocked &= raw;
        u32 b = raw & ~g_pad_blocked;
        float lx = stick(gp.sThumbLX), ly = -stick(gp.sThumbLY), rx = stick(gp.sThumbRX), ry = -stick(gp.sThumbRY);
        float lt = gp.bLeftTrigger / 255.0f, rt = gp.bRightTrigger / 255.0f;
        u32 before = g_pad_buttons;
        g_pad_buttons = b;
        g_pad_lx = lx, g_pad_ly = ly;
        pad_input(b, lx, ly, rx, ry, lt, rt);
        g_pad_sent = true;
        // as pad.cpp decides the controller is in use
        float mag = std::max(std::max(std::fabs(lx), std::fabs(ly)), std::max(std::fabs(rx), std::fabs(ry)));
        if (b || mag > 0.5f || lt >= 0.5f || rt >= 0.5f) g_pad_in_control = true;
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
    if (g_pad_touching) marm_touch(0, 6, std::lround(g_cur_x), std::lround(g_cur_y));
}

// After pad_poll: hide or show the pointer as the controller takes over play or lets go of it.
void check_pointer() {
    bool hide = pad_hides_pointer();
    if (hide == g_pointer_hidden) return;
    g_pointer_hidden = hide;
    if (pointer_over_client()) cursor_for_window();
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
    if (moved && (x != g_cur_put.x || y != g_cur_put.y)) {
        g_cur_put = {-1, -1};
        g_pad_in_control = false;
    }
}

// A mouse button: always to pad.cpp; the left one is also a touch outside play.
void mouse_button(HWND h, int code, bool down, LPARAM lp) {
    kbm_key(code, down);
    if (code != KEY_MOUSE_LEFT) return;
    g_mouse_x = GET_X_LPARAM(lp);
    g_mouse_y = GET_Y_LPARAM(lp);
    if (down) {
        if (kbm_mouse_mode() != KBM_MOUSE_TOUCH) return;
        SetCapture(h);
        g_mouse_down = true;
        marm_touch(0, 4, g_mouse_x, g_mouse_y);
    } else if (g_mouse_down) {
        g_mouse_down = false;
        ReleaseCapture();
        marm_touch(0, 5, g_mouse_x, g_mouse_y);
    }
}

LRESULT CALLBACK wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_APP_RUN:
        run_queue();
        return 0;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        mouse_button(h, KEY_MOUSE_LEFT, msg == WM_LBUTTONDOWN, lp);
        return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        mouse_button(h, KEY_MOUSE_RIGHT, msg == WM_RBUTTONDOWN, lp);
        return 0;
    case WM_MBUTTONDOWN:
    case WM_MBUTTONUP:
        mouse_button(h, KEY_MOUSE_MIDDLE, msg == WM_MBUTTONDOWN, lp);
        return 0;
    case WM_XBUTTONDOWN:
    case WM_XBUTTONUP:
        mouse_button(h, GET_XBUTTON_WPARAM(wp) == XBUTTON1 ? KEY_MOUSE_BACK : KEY_MOUSE_FORWARD, msg == WM_XBUTTONDOWN, lp);
        return TRUE;
    case WM_MOUSEMOVE:
        mouse_moved(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        kbm_cursor(static_cast<float>(g_mouse_x - g_w / 2), static_cast<float>(g_mouse_y - g_h / 2));
        if (g_mouse_down) marm_touch(0, 6, g_mouse_x, g_mouse_y);
        return 0;
    case WM_MOUSEWHEEL: {
        g_wheel_rest += GET_WHEEL_DELTA_WPARAM(wp);
        int n = g_wheel_rest / WHEEL_DELTA;
        g_wheel_rest -= n * WHEEL_DELTA;
        kbm_wheel(n);
        return 0;
    }
    case WM_INPUT: {  // raw mouse motion: turns the view while captured
        if (g_captured) {
            RAWINPUT ri;
            UINT size = sizeof ri;
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lp), RID_INPUT, &ri, &size, sizeof(RAWINPUTHEADER)) != UINT(-1) &&
                ri.header.dwType == RIM_TYPEMOUSE && !(ri.data.mouse.usFlags & MOUSE_MOVE_ABSOLUTE))
                kbm_mouse_move(static_cast<float>(ri.data.mouse.lLastX), static_cast<float>(ri.data.mouse.lLastY));
        }
        break;  // DefWindowProc cleans up after WM_INPUT
    }
    case WM_APP_MOUSE_MOTION:
        kbm_mouse_move(static_cast<float>(static_cast<int>(wp)), static_cast<float>(static_cast<int>(lp)));
        return 0;
    case WM_TIMER:
        if (wp == TIMER_MOUSE) {
            check_capture();
            pad_poll();
            check_pointer();
        }
        return 0;
    case WM_SETCURSOR:
        if (LOWORD(lp) == HTCLIENT) {
            cursor_for_window();
            return TRUE;
        }
        break;
    case WM_MOUSEACTIVATE:  // clicking back into zombies play only brings the window back (no shot)
        if (kbm_mouse_mode() == KBM_MOUSE_LOOK) return MA_ACTIVATEANDEAT;
        break;
    case WM_ACTIVATE:
        if (LOWORD(wp) == WA_INACTIVE) lost_focus();
        else check_capture();
        return 0;
    case WM_KILLFOCUS:
        lost_focus();
        return 0;
    case WM_ENTERSIZEMOVE:  // moving or sizing the window (title bar, borders, Alt+Space): the lock would stop it
    case WM_ENTERMENULOOP:
        g_modal = true;
        set_captured(false);
        break;
    case WM_EXITSIZEMOVE:
    case WM_EXITMENULOOP:
        g_modal = false;
        check_capture();
        break;
    case WM_CAPTURECHANGED:
        if (g_mouse_down && reinterpret_cast<HWND>(lp) != h) {
            g_mouse_down = false;
            marm_touch(0, 7, 0, 0);  // cancel
        }
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN:
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        bool down = msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN;
        if (msg == WM_SYSKEYDOWN && wp == VK_F4) break;  // Alt+F4 closes the window
        if (down && (lp & (1 << 30))) return 0;          // auto-repeat
        int code = android_key(wp, lp);
        if (!code) return 0;
        kbm_key(code, down);
        // to the game as a key press outside play, and its release whenever the game saw it go down
        if (down && kbm_mouse_mode() == KBM_MOUSE_TOUCH && code < 512) {
            g_marm_down.set(code);
            marm_key(code, unicode_of(wp, lp), true);
        } else if (!down && code < 512 && g_marm_down.test(code)) {
            g_marm_down.reset(code);
            marm_key(code, 0, false);
        }
        return 0;
    }
    case WM_CHAR:
    case WM_SYSCHAR:
        return 0;
    case WM_SIZE:
        if (wp == SIZE_MINIMIZED) {
            marm_suspend();
        } else {
            int w = LOWORD(lp), hh = HIWORD(lp);
            marm_resume();
            if (w > 0 && hh > 0 && (w != g_w || hh != g_h)) {
                g_w = w;
                g_h = hh;
                marm_surface_changed(w, hh);
            }
        }
        return 0;
    case WM_CLOSE:
        marm_shutdown();
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        set_captured(false);
        pad_clear();
        PostQuitMessage(g_exit_code);
        return 0;
    case WM_ERASEBKGND:
        return 1;
    }
    return DefWindowProcW(h, msg, wp, lp);
}

}  // namespace

bool win_create(int width, int height) {
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    g_ui_tid = GetCurrentThreadId();
    HINSTANCE inst = GetModuleHandleW(nullptr);
    WNDCLASSEXW wc{sizeof wc};
    wc.style = CS_OWNDC;
    wc.lpfnWndProc = wndproc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512));
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"boz64";
    if (!RegisterClassExW(&wc)) return false;
    DWORD style = WS_OVERLAPPEDWINDOW;
    RECT r{0, 0, width, height};
    AdjustWindowRectEx(&r, style, FALSE, 0);
    g_hwnd = CreateWindowExW(0, L"boz64", L"Call of Duty: Black Ops Zombies (PC)", style, CW_USEDEFAULT, CW_USEDEFAULT,
                             r.right - r.left, r.bottom - r.top, nullptr, nullptr, inst, nullptr);
    if (!g_hwnd) return false;
    RECT c;
    GetClientRect(g_hwnd, &c);
    g_w = c.right;
    g_h = c.bottom;
    RAWINPUTDEVICE mouse{0x01, 0x02, 0, g_hwnd};  // generic desktop, mouse: WM_INPUT while the window has focus
    if (!RegisterRawInputDevices(&mouse, 1, sizeof mouse)) LOGW("window: no raw mouse input (%lu)", GetLastError());
    char v[PROP_VALUE_MAX] = {};
    g_no_capture = __system_property_get("debug.boz64.nocapture", v) > 0 && v[0] == '1';
    pad_load();
    SetTimer(g_hwnd, TIMER_MOUSE, MOUSE_CHECK_MS, nullptr);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    // the first WM_SIZE arrives before the game exists; take the real client size now
    GetClientRect(g_hwnd, &c);
    g_w = c.right;
    g_h = c.bottom;
    LOGI("window %dx%d", g_w, g_h);
    return true;
}

int win_run_loop() {
    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) {
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
    return static_cast<int>(m.wParam);
}

void win_quit(int code) {
    g_exit_code = code;
    ui_post([] {
        if (g_hwnd) DestroyWindow(g_hwnd);
    });
}

int win_width() { return g_w; }
int win_height() { return g_h; }
void* win_hwnd() { return g_hwnd; }
bool on_ui_thread() { return GetCurrentThreadId() == g_ui_tid; }

void ui_post(std::function<void()> f) {
    {
        std::lock_guard lk(q_lock);
        q.push_back(std::move(f));
    }
    PostMessageW(g_hwnd, WM_APP_RUN, 0, 0);
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
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    SetWindowTextW(g_hwnd, w.c_str());
}
