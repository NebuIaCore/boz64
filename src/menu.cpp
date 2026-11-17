// The port's options on the game's "Controls Sensitivity" page (pause menu > Settings > Controls Sensitivity).
// The page is IwUI (Marmalade UI): a tree of CIwUIElement objects built from the game's resources when it opens.
// Each X/Y row there is a caption label, three track images ("-", bar, "+") and an invisible slider that
// draws only its knob. Our rows are copies (IwUI's own Clone) of the Y row, so they look and behave like the
// game's. A copied slider keeps the original's "OnSliderChanged -> SensitivityY" binding, so moving one calls
// the page's SensitivityY handler with the copy, where a hook (patches.cpp) sends it here instead.
// The page only has room for its two rows, so all four rows are spaced closer together. The page's panel
// places its own elements with a form layout whenever it is laid out again (adding elements does that, as can
// a caption change), so the positions are applied again after each layout of the panel.
#include "menu.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>

#include "cpu.h"
#include "settings.h"

namespace {
// Addresses in the game image as dumped at this base (the image is loaded the same way every run; the
// functions are found relative to where the image actually is).
constexpr u32 DUMP_BASE = 0x02454000;
u32 image = 0;
u32 at(u32 a) { return a - DUMP_BASE + image; }

constexpr u32 UI_CONTROLLER = 0x028bd280;       // pointer to the game's UI controller (holds the open page)
constexpr u32 FN_FIND = 0x02669014 | 1;         // (controller, name, 0): element of the open page, by name
constexpr u32 FN_RES_NAME = 0x02669ae0 | 1;     // (page name): its resource name for this screen size
constexpr u32 FN_CREATE = 0x02756b24;           // (resource name, 0): a new element tree from the resource
constexpr u32 FN_CHILD_NAMED = 0x02756e2c;      // CIwUIElement::GetChildNamed(this, name, recurse, 0)
constexpr u32 FN_CLONE = 0x02756aec;            // CIwUIElement::Clone(this)
constexpr u32 FN_ADD_CHILD = 0x02758e4c;        // CIwUIElement::AddChild(this, child)
constexpr u32 FN_SET_POS = 0x02757790;          // CIwUIElement::SetPos(this, const CIwVec2*, relayout)
constexpr u32 FN_SET_SIZE = 0x027586f0;         // CIwUIElement::SetSize(this, const CIwVec2*, relayout)
constexpr u32 FN_SLIDER_SET = 0x02771680;       // CIwUISlider::SetValue(this, s16)
constexpr u32 FN_SLIDER_GET = 0x02770e60;       // CIwUISlider::GetValue(this)
constexpr u32 FN_SLIDER_RANGE = 0x02770e90;     // CIwUISlider: min | max << 16
constexpr u32 FN_LABEL_CAPTION = 0x0275f1c0;    // CIwUILabel::SetCaption(this, const char*)
constexpr u32 FN_SET_VISIBLE = 0x0275a518;      // CIwUIElement::SetVisible(this, bool)
// CIwUILabel virtual (CIwVec2* out, this, const CIwVec2* space): the size its text needs, margins included
// (0x7fffffff: no limit). The generic preferred size (+0x5c) only asks this when the space is unlimited.
constexpr u32 VT_MEASURE = 0xc8;
constexpr u32 NO_LIMIT = 0x7fffffff;
constexpr u32 VT_DELETE = 0x04;   // virtual deleting destructor (how the game frees a page it closes)

// CIwUIElement fields
constexpr u32 EL_HASH = 4, EL_PARENT = 0x18, EL_CHILDREN = 0x1c, EL_NCHILDREN = 0x20, EL_POS = 0x5c, EL_SIZE = 0x64,
              EL_LAYOUT = 0x8c;

// The page's own elements, by name hash (their names are only in the game's resource files)
constexpr u32 H_X_CAPTION = 0x88fe5c61, H_X_MINUS = 0xd00c64b7, H_X_BAR = 0x3c44f13e, H_X_PLUS = 0x8a309e8f;
constexpr u32 H_Y_CAPTION = 0x891074e2, H_Y_MINUS = 0x04b87ae8, H_Y_BAR = 0x3c44f13f, H_Y_PLUS = 0xd8d28ac0;
constexpr u32 H_CALIBRATE_BUTTON = 0x180b6508;  // invisible button in "calibrate_element" (shown for tilt controls)
// The game's Settings page ("options"): each checkbox there is an empty box image, a cross image the page shows
// or hides, and an invisible button over them. This is Auto Aim's box (its cross is "autoAimCross").
constexpr u32 H_OPTIONS_BOX = 0x52d136be;
constexpr s32 OPTIONS_UNITS = 1280;  // the Settings page's width in its own units, if its tree has no size

// Layout, in the coordinates of the panel as the game lays it out to cover a 2316x1080 screen (2316x1447; the
// game's rows: X caption at y 248, slider 357; Y caption 591, slider 682; the Back button's touch area is
// x 72-629 from y 1066 down). The panel is scaled to fit the screen (menu_fit_panel), and its form layout scales
// the positions with it, so these are scaled by the panel's actual width.
constexpr double DESIGN_W = 2316;
constexpr int ROW_TOP[4] = {200, 420, 640, 860};  // caption tops: X, Y, frame rate, resolution
constexpr int CAPTION_X = 244, TRACK_DY = 125;    // the track images sit this far below the caption's top
constexpr int MINUS_X = 454, BAR_X = 488, PLUS_X = 1588, SLIDER_X = 470;
constexpr int NEW_SLIDER_H = 100;
// "Zombies ignore you": a checkbox row to the right of the Back button, centred at this height
constexpr int TOGGLE_X = 700, TOGGLE_CY = 1170, TOGGLE_GAP = 40, TOGGLE_PAD = 30;
// The tilt controls' "calibrate" picture and button (shown only with tilt controls) move right, clear of the rows
constexpr int CALIBRATE_DX = 880, CALIBRATE_DY = -300;

// IwHashString: djb2 over the lower-cased name
u32 hash(const char* s) {
    u32 h = 5381;
    for (; *s; s++) h = h * 33 + static_cast<u32>((*s >= 'A' && *s <= 'Z') ? *s + 32 : *s);
    return h;
}

u32 gstr(const char* s) {  // a permanent guest copy of a string
    static std::unordered_map<std::string, u32> cache;
    auto it = cache.find(s);
    if (it != cache.end()) return it->second;
    return cache[s] = g::data_str(s);
}

bool valid(u32 a) { return a >= g::NULL_GUARD && a < g::ALLOC_END && !(a & 3); }

std::string class_of(u32 obj) {
    u32 vt = g::rd32(obj), ti = valid(vt) ? g::rd32(vt - 4) : 0, n = valid(ti) ? g::rd32(ti + 4) : 0;
    std::string s;
    for (u32 i = 0; n && i < 60; i++) {
        char c = static_cast<char>(g::rd8(n + i));
        if (!c) break;
        s += c;
    }
    return s;
}

// debugging: an element tree with each element's class, name hash, position and size
void dump(u32 e, int depth, int max_depth) {
    if (!valid(e) || depth > max_depth) return;
    u32 layout = g::rd32(e + EL_LAYOUT);
    LOGI("ui: %*s%s %08x hash %08x flags %x pos %d,%d size %d,%d layout %s", depth * 2, "", class_of(e).c_str(), e,
         g::rd32(e + EL_HASH), g::rd16(e + 0x2c), static_cast<s32>(g::rd32(e + EL_POS)), static_cast<s32>(g::rd32(e + EL_POS + 4)),
         static_cast<s32>(g::rd32(e + EL_SIZE)), static_cast<s32>(g::rd32(e + EL_SIZE + 4)), valid(layout) ? class_of(layout).c_str() : "-");
    u32 kids = g::rd32(e + EL_CHILDREN), n = g::rd32(e + EL_NCHILDREN);
    for (u32 i = 0; i < n && i < 64; i++) dump(g::rd32(kids + 4 * i), depth + 1, max_depth);
}

u32 call(u32 fn, std::initializer_list<u32> args) { return static_cast<u32>(call_guest(at(fn), args)); }

u32 find(const char* name) {
    u32 ctl = g::rd32(at(UI_CONTROLLER));
    return valid(ctl) ? call(FN_FIND, {ctl, gstr(name), 0}) : 0;
}

u32 child_by_hash(u32 parent, u32 h) {
    u32 kids = g::rd32(parent + EL_CHILDREN), n = g::rd32(parent + EL_NCHILDREN);
    for (u32 i = 0; i < n && i < 256; i++) {
        u32 c = g::rd32(kids + 4 * i);
        if (valid(c) && g::rd32(c + EL_HASH) == h) return c;
    }
    return 0;
}

u32 vec = 0;  // guest scratch for CIwVec2 arguments (vec, vec + 8, vec + 16)
double scale = 1;  // the panel's width / DESIGN_W
int px(double design) { return static_cast<int>(std::lround(design * scale)); }
void set_pos(u32 e, int x, int y) {
    if (!valid(e)) return;
    g::wr32(vec, static_cast<u32>(x));
    g::wr32(vec + 4, static_cast<u32>(y));
    call(FN_SET_POS, {e, vec, 0});
}
void set_size(u32 e, int w, int h) {
    if (!valid(e)) return;
    g::wr32(vec, static_cast<u32>(w));
    g::wr32(vec + 4, static_cast<u32>(h));
    call(FN_SET_SIZE, {e, vec, 0});
}
int width(u32 e) { return static_cast<s32>(g::rd32(e + EL_SIZE)); }
int height(u32 e) { return static_cast<s32>(g::rd32(e + EL_SIZE + 4)); }

struct Row {
    u32 caption = 0, minus = 0, bar = 0, plus = 0, slider = 0;
    u32 text = 0;            // guest buffer for the caption
    s32 text_w = 0, text_h = 0;  // the size the caption's text needs (0: keep the label's size)
};
// The caption's label sized to its text. The page's layout gives a copied label the original's size again each
// time, so apply_layout sizes it again from what is measured here.
void fit_to_text(Row& r) {
    if (!valid(r.caption)) return;
    u32 out = vec + 8, space = vec + 16;
    g::wr32(space, NO_LIMIT);
    g::wr32(space + 4, NO_LIMIT);
    g::wr32(out, 0);
    g::wr32(out + 4, 0);
    call_guest(g::rd32(g::rd32(r.caption) + VT_MEASURE), {out, r.caption, space});
    s32 w = static_cast<s32>(g::rd32(out)), h = static_cast<s32>(g::rd32(out + 4));
    static int logged = 0;
    if (logged++ < 6) LOGI("menu: caption \"%s\" measures %dx%d (label %dx%d)", g::str(r.text), w, h, width(r.caption), height(r.caption));
    if (w <= 0 || h <= 0 || w >= 10000 || h >= 10000) return;
    r.text_w = w, r.text_h = h;
    set_size(r.caption, w, h);
}
int caption_width(const Row& r) { return r.text_w > 0 ? r.text_w : width(r.caption); }

struct Toggle {
    Row label;  // only its caption
    u32 box = 0, cross = 0, button = 0;
    // in the Settings page's units: the box's and cross's sizes and where the cross sits on the box
    s32 box_w = 0, box_h = 0, cross_w = 0, cross_h = 0, cross_dx = 0, cross_dy = 0;
    s32 units = OPTIONS_UNITS;  // the Settings page's width in those units
};
struct Page {
    u32 panel = 0, calibrate = 0;
    Row rows[4];  // X, Y, frame rate, resolution
    Toggle zombies;
    Settings s;   // the values shown
    bool setting_up = false, changed = false;
    int layouts = 0;
} page;
Row& fps_row() { return page.rows[2]; }
Row& res_row() { return page.rows[3]; }

void apply_layout() {
    s32 panel_w = static_cast<s32>(g::rd32(page.panel + EL_SIZE));
    scale = panel_w > 0 ? panel_w / DESIGN_W : 1;
    for (int i = 0; i < 4; i++) {
        Row& r = page.rows[i];
        if (!valid(r.slider)) continue;
        int top = ROW_TOP[i], track = top + TRACK_DY;
        set_pos(r.caption, px(CAPTION_X), px(top));
        if (r.text_w > 0) set_size(r.caption, r.text_w, r.text_h);
        set_pos(r.minus, px(MINUS_X), px(track));
        set_pos(r.bar, px(BAR_X), px(track));
        set_pos(r.plus, px(PLUS_X), px(track));
        if (i >= 2) set_size(r.slider, width(r.slider), px(NEW_SLIDER_H));
        set_pos(r.slider, px(SLIDER_X), px(track + 24) - height(r.slider) / 2);
    }
    Toggle& z = page.zombies;
    u32 caption = z.label.caption;
    if (valid(caption) && valid(z.box) && valid(z.cross) && valid(z.button)) {
        double u = width(page.panel) / static_cast<double>(z.units);  // Settings page units to pixels
        auto upx = [&](s32 v) { return static_cast<int>(std::lround(v * u)); };
        int cy = px(TOGGLE_CY), x = px(TOGGLE_X);
        if (z.label.text_w > 0) set_size(caption, z.label.text_w, z.label.text_h);
        set_pos(caption, x, cy - height(caption) / 2);
        int bx = x + caption_width(z.label) + px(TOGGLE_GAP), bw = upx(z.box_w), bh = upx(z.box_h);
        set_size(z.box, bw, bh);
        set_pos(z.box, bx, cy - bh / 2);
        set_size(z.cross, upx(z.cross_w), upx(z.cross_h));
        set_pos(z.cross, bx + upx(z.cross_dx), cy - bh / 2 + upx(z.cross_dy));
        // the button covers the caption and the box
        int pad = px(TOGGLE_PAD), left = x - pad, top = std::min(cy - height(caption) / 2, cy - bh / 2) - pad;
        set_pos(z.button, left, top);
        set_size(z.button, bx + bw + pad - left, 2 * (cy - top));
    }
    if (valid(page.calibrate)) set_pos(page.calibrate, px(CALIBRATE_DX), px(CALIBRATE_DY));
}

// The sliders run 0..SLIDER_MAX (the Y slider's range, copied with it).
constexpr int SLIDER_MAX = 4096;
constexpr int FPS_UNCAPPED_FROM = SLIDER_MAX - 64;  // the far right end: no limit
int fps_from_slider(int v) {
    if (v >= FPS_UNCAPPED_FROM) return 0;
    int fps = 1 + static_cast<int>(std::lround(v * 499.0 / FPS_UNCAPPED_FROM));
    for (int common : {30, 60, 90, 120, 144, 165, 240})  // easy to land on the usual rates
        if (std::abs(fps - common) <= 2) return common;
    return fps;
}
int slider_from_fps(int fps) {
    return fps <= 0 ? SLIDER_MAX : static_cast<int>(std::lround((fps - 1) * static_cast<double>(FPS_UNCAPPED_FROM) / 499));
}
int res_from_slider(int v) { return 25 + 5 * static_cast<int>(std::lround(v * 15.0 / SLIDER_MAX)); }  // 25-100% by 5
int slider_from_res(int res) { return static_cast<int>(std::lround((res - 25) / 5 * SLIDER_MAX / 15.0)); }

void set_caption(Row& r, const char* text) {
    if (!valid(r.caption)) return;
    if (!r.text) r.text = g::data_alloc(64, 4);
    snprintf(static_cast<char*>(g::h(r.text)), 64, "%s", text);
    call(FN_LABEL_CAPTION, {r.caption, r.text});
    fit_to_text(r);
}
void show_values() {
    char t[64];
    if (page.s.fps_limit > 0) snprintf(t, sizeof t, "FRAME RATE LIMIT - %d", page.s.fps_limit);
    else snprintf(t, sizeof t, "FRAME RATE LIMIT - NONE");
    set_caption(fps_row(), t);
    snprintf(t, sizeof t, "3D RESOLUTION - %d%%", page.s.res_scale);
    set_caption(res_row(), t);
}

// A copy of one of the page's elements, added to the panel under a name of its own.
u32 copy(u32 e, const char* name) {
    if (!valid(e)) return 0;
    u32 c = call(FN_CLONE, {e});
    if (!valid(c)) return 0;
    g::wr32(c + EL_HASH, hash(name));
    call(FN_ADD_CHILD, {page.panel, c});
    return c;
}

void show_toggle() {
    Toggle& z = page.zombies;
    if (valid(z.cross)) call(FN_SET_VISIBLE, {z.cross, page.s.zombies_ignore ? 1u : 0u});
}

// The "zombies ignore you" checkbox: a caption like the page's own, the box and cross images of the game's
// Settings page (built off screen to copy them from, then freed), and a copy of this page's invisible
// "calibrate" button over them, whose clicks go to the page's Calibrate slot (hooked: menu_on_button).
void add_toggle(u32 caption_like) {
    Toggle& z = page.zombies;
    u32 cal_button = valid(page.calibrate) ? child_by_hash(page.calibrate, H_CALIBRATE_BUTTON) : 0;
    u32 res = call(FN_RES_NAME, {gstr("options")});
    u32 tree = res ? call(FN_CREATE, {res, 0}) : 0;
    u32 box = valid(tree) ? child_by_hash(tree, H_OPTIONS_BOX) : 0;
    u32 cross = valid(tree) ? call(FN_CHILD_NAMED, {tree, gstr("autoAimCross"), 1, 0}) : 0;
    if (valid(box) && valid(cross) && valid(cal_button)) {
        if (width(tree) > 0) z.units = width(tree);
        z.box_w = width(box), z.box_h = height(box), z.cross_w = width(cross), z.cross_h = height(cross);
        z.cross_dx = static_cast<s32>(g::rd32(cross + EL_POS) - g::rd32(box + EL_POS));
        z.cross_dy = static_cast<s32>(g::rd32(cross + EL_POS + 4) - g::rd32(box + EL_POS + 4));
        z.label.caption = copy(caption_like, "boz64_zombies_caption");
        z.box = copy(box, "boz64_zombies_box");
        z.cross = copy(cross, "boz64_zombies_cross");
        z.button = copy(cal_button, "boz64_zombies_button");  // last, so it is on top
        LOGI("menu: checkbox parts: box %dx%d, cross %dx%d at %+d,%+d (of %d); button flags %x", z.box_w, z.box_h,
             z.cross_w, z.cross_h, z.cross_dx, z.cross_dy, z.units, valid(z.button) ? g::rd16(z.button + 0x2c) : 0);
    } else {
        LOGW("menu: no checkbox parts (Settings page %08x, box %08x, cross %08x, calibrate button %08x)", tree, box,
             cross, cal_button);
    }
    if (valid(tree)) call_guest(g::rd32(g::rd32(tree) + VT_DELETE), {tree});
    if (!valid(z.label.caption)) return;
    set_caption(z.label, "ZOMBIES IGNORE YOU");
    show_toggle();
}
}  // namespace

void menu_init(u32 image_lo) {
    image = image_lo;
    if (!vec) vec = g::data_alloc(24, 4);
}

void menu_on_enter() {
    if (!image) return;
    page = Page{};
    u32 y = find("sensitivityYSlider"), x = find("sensitivityXSlider");
    u32 panel = valid(y) ? g::rd32(y + EL_PARENT) : 0;
    if (!valid(x) || !valid(panel) || g::rd32(x + EL_PARENT) != panel) {
        LOGW("menu: the sensitivity page is not laid out as expected; leaving it alone");
        return;
    }
    Row rx{child_by_hash(panel, H_X_CAPTION), child_by_hash(panel, H_X_MINUS), child_by_hash(panel, H_X_BAR),
           child_by_hash(panel, H_X_PLUS), x};
    Row ry{child_by_hash(panel, H_Y_CAPTION), child_by_hash(panel, H_Y_MINUS), child_by_hash(panel, H_Y_BAR),
           child_by_hash(panel, H_Y_PLUS), y};
    if (!rx.caption || !rx.minus || !rx.bar || !rx.plus || !ry.caption || !ry.minus || !ry.bar || !ry.plus) {
        LOGW("menu: the sensitivity page's rows are not as expected; leaving it alone");
        return;
    }
    u32 range = call(FN_SLIDER_RANGE, {y});
    if (static_cast<s16>(range) != 0 || static_cast<s16>(range >> 16) != SLIDER_MAX)
        LOGW("menu: the Y slider's range is %d..%d", static_cast<s16>(range), static_cast<s16>(range >> 16));
    page.panel = panel;
    page.calibrate = call(FN_CHILD_NAMED, {panel, gstr("calibrate_element"), 0, 0});
    page.rows[0] = rx;
    page.rows[1] = ry;
    const char* names[2] = {"boz64_fps", "boz64_res"};
    for (int i = 0; i < 2; i++) {
        std::string n = names[i];
        Row& r = page.rows[2 + i];
        r.minus = copy(ry.minus, (n + "_minus").c_str());
        r.bar = copy(ry.bar, (n + "_bar").c_str());
        r.plus = copy(ry.plus, (n + "_plus").c_str());
        r.caption = copy(ry.caption, (n + "_caption").c_str());
        r.slider = copy(y, (n + "_slider").c_str());
    }
    page.s = settings();
    page.setting_up = true;
    if (valid(fps_row().slider)) call(FN_SLIDER_SET, {fps_row().slider, static_cast<u32>(slider_from_fps(page.s.fps_limit))});
    if (valid(res_row().slider)) call(FN_SLIDER_SET, {res_row().slider, static_cast<u32>(slider_from_res(page.s.res_scale))});
    page.setting_up = false;
    show_values();
    add_toggle(ry.caption);
    apply_layout();
    LOGI("menu: added frame rate, resolution and zombies rows (fps limit %d, resolution %d%%, zombies ignore %d)",
         page.s.fps_limit, page.s.res_scale, page.s.zombies_ignore);
}

void menu_fit_panel(u32 panel, u32 size, u32 avail) {
    if (!valid(panel) || g::rd32(panel + EL_NCHILDREN) == 0) return;  // a plain backdrop image may cover the screen
    s32 w = static_cast<s32>(g::rd32(size)), h = static_cast<s32>(g::rd32(size + 4));
    s32 aw = static_cast<s32>(g::rd32(avail)), ah = static_cast<s32>(g::rd32(avail + 4));
    if (w <= 0 || h <= 0 || aw <= 0 || ah <= 0 || aw > 100000 || ah > 100000 || (w <= aw && h <= ah)) return;
    double s = std::min(aw / static_cast<double>(w), ah / static_cast<double>(h));
    s32 nw = static_cast<s32>(std::lround(w * s)), nh = static_cast<s32>(std::lround(h * s));
    static s32 logged_w = 0, logged_h = 0;
    if (w != logged_w || h != logged_h) {
        logged_w = w, logged_h = h;
        LOGI("menu: a %dx%d menu panel would not fit the %dx%d screen; using %dx%d", w, h, aw, ah, nw, nh);
    }
    g::wr32(size, static_cast<u32>(nw));
    g::wr32(size + 4, static_cast<u32>(nh));
}

void menu_after_layout(u32 element) {
    if (!element || element != page.panel) return;
    if (++page.layouts <= 5) LOGI("menu: the page was laid out again (%d); placing the rows", page.layouts);
    apply_layout();
}

bool menu_on_slider(u32 slider) {
    if (!slider || !page.panel) return false;
    bool fps = slider == fps_row().slider, res = slider == res_row().slider;
    if (!fps && !res) return false;
    if (page.setting_up) return true;
    int v = static_cast<s16>(call(FN_SLIDER_GET, {slider}));
    Settings s = page.s;
    if (fps) s.fps_limit = fps_from_slider(v);
    else s.res_scale = res_from_slider(v);
    if (s.fps_limit != page.s.fps_limit || s.res_scale != page.s.res_scale) {
        page.s = s;
        page.changed = true;
        settings_set(s, false);  // in use at once; written to the file when the page closes
        show_values();
    }
    return true;
}

bool menu_on_button(u32 button) {
    if (!button || !page.panel || button != page.zombies.button) return false;
    page.s.zombies_ignore = !page.s.zombies_ignore;
    page.changed = true;
    settings_set(page.s, false);
    show_toggle();
    LOGI("menu: zombies ignore you: %s", page.s.zombies_ignore ? "on" : "off");
    return true;
}

void menu_on_exit() {
    if (page.changed) {
        settings_set(page.s, true);
        LOGI("menu: saved fps limit %d, resolution %d%%, zombies ignore %d", page.s.fps_limit, page.s.res_scale,
             page.s.zombies_ignore);
    }
    page = Page{};
}
