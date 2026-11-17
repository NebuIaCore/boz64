// EGL for the game window through ANGLE (libEGL.dll / libGLESv2.dll next to the .exe): what the app's LoaderGL
// Java class does on Android (eglGetDisplay, eglInitialize, chooseConfigs, eglCreateContext, eglCreateWindowSurface,
// eglMakeCurrent on the engine thread).
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include <vector>

#include "common.h"
#include "platform.h"

namespace {

struct Egl {
    decltype(&::eglGetDisplay) GetDisplay;
    decltype(&::eglInitialize) Initialize;
    decltype(&::eglChooseConfig) ChooseConfig;
    decltype(&::eglGetConfigAttrib) GetConfigAttrib;
    decltype(&::eglCreateContext) CreateContext;
    decltype(&::eglCreateWindowSurface) CreateWindowSurface;
    decltype(&::eglMakeCurrent) MakeCurrent;
    decltype(&::eglSwapBuffers) SwapBuffers;
    decltype(&::eglDestroySurface) DestroySurface;
    decltype(&::eglDestroyContext) DestroyContext;
    decltype(&::eglTerminate) Terminate;
    decltype(&::eglGetError) GetError;
    decltype(&::eglQueryString) QueryString;
} e{};

EGLDisplay dpy = EGL_NO_DISPLAY;
EGLConfig cfg = nullptr;
EGLContext ctx = EGL_NO_CONTEXT;
EGLSurface surf = EGL_NO_SURFACE;
int gl_version = 2;

bool load() {
    if (e.GetDisplay) return true;
    std::string p = std::string(boz_exe_dir()) + "/libEGL.dll";
    HMODULE h = LoadLibraryA(p.c_str());
    if (!h) {
        LOGE("libEGL.dll (ANGLE) is missing next to the .exe");
        return false;
    }
#define L(name) e.name = reinterpret_cast<decltype(e.name)>(GetProcAddress(h, "egl" #name))
    L(GetDisplay);
    L(Initialize);
    L(ChooseConfig);
    L(GetConfigAttrib);
    L(CreateContext);
    L(CreateWindowSurface);
    L(MakeCurrent);
    L(SwapBuffers);
    L(DestroySurface);
    L(DestroyContext);
    L(Terminate);
    L(GetError);
    L(QueryString);
#undef L
    return e.GetDisplay && e.Initialize && e.ChooseConfig && e.CreateContext && e.CreateWindowSurface && e.MakeCurrent;
}

int attr(EGLConfig c, EGLint a) {
    EGLint v = 0;
    e.GetConfigAttrib(dpy, c, a, &v);
    return v;
}

// LoaderGL.chooseConfigs: the set [GL] attributes plus the renderable type; when nothing matches, drop one set
// attribute at a time (in order) and try again; take the first config.
bool choose_config(const int* icf) {
    std::vector<EGLint> set;
    for (const int* p = icf; *p != EGL_NONE; p += 2) set.insert(set.end(), {p[0], p[1]});
    for (size_t drop = 0;; drop++) {
        std::vector<EGLint> spec;
        for (size_t i = drop * 2; i < set.size(); i += 2) spec.insert(spec.end(), {set[i], set[i + 1]});
        if (gl_version >= 2) spec.insert(spec.end(), {EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT});
        spec.push_back(EGL_NONE);
        EGLint n = 0;
        EGLConfig c[64];
        if (e.ChooseConfig(dpy, spec.data(), c, 64, &n) && n > 0) {
            // Phone drivers list RGBA8888 first; ANGLE lists RGBA4444 first when no colour size is set, which
            // bands badly. Take the first RGBA8888 config of the matches (the game's blending can use destination
            // alpha), else the first 8-bit RGB one.
            cfg = c[0];
            int best = 0;
            for (EGLint i = 0; i < n && best < 2; i++) {
                if (attr(c[i], EGL_RED_SIZE) != 8 || attr(c[i], EGL_GREEN_SIZE) != 8 || attr(c[i], EGL_BLUE_SIZE) != 8) continue;
                int score = attr(c[i], EGL_ALPHA_SIZE) == 8 ? 2 : 1;
                if (score > best) {
                    best = score;
                    cfg = c[i];
                }
            }
            LOGI("EGL config: RGBA %d%d%d%d depth %d stencil %d samples %d (%d matched, %zu settings dropped)",
                 attr(cfg, EGL_RED_SIZE), attr(cfg, EGL_GREEN_SIZE), attr(cfg, EGL_BLUE_SIZE), attr(cfg, EGL_ALPHA_SIZE),
                 attr(cfg, EGL_DEPTH_SIZE), attr(cfg, EGL_STENCIL_SIZE), attr(cfg, EGL_SAMPLES), n, drop);
            return true;
        }
        if (drop * 2 >= set.size()) return false;
    }
}

bool make_surface() {
    surf = e.CreateWindowSurface(dpy, cfg, static_cast<EGLNativeWindowType>(win_hwnd()), nullptr);
    if (surf == EGL_NO_SURFACE) {
        LOGE("eglCreateWindowSurface failed (0x%x)", e.GetError());
        return false;
    }
    if (!e.MakeCurrent(dpy, surf, surf, ctx)) {
        LOGE("eglMakeCurrent failed (0x%x)", e.GetError());
        return false;
    }
    return true;
}

}  // namespace

bool egl_start(int version, const int* attribs) {
    if (!load()) return false;
    if (version) gl_version = version;
    if (dpy == EGL_NO_DISPLAY) {
        dpy = e.GetDisplay(EGL_DEFAULT_DISPLAY);
        EGLint maj = 0, min = 0;
        if (dpy == EGL_NO_DISPLAY || !e.Initialize(dpy, &maj, &min)) {
            LOGE("eglInitialize failed (0x%x)", e.GetError());
            return false;
        }
        LOGI("EGL %d.%d: %s, %s", maj, min, e.QueryString(dpy, EGL_VENDOR), e.QueryString(dpy, EGL_VERSION));
        if (!choose_config(attribs)) {
            LOGE("Failed to choose an EGL config");
            return false;
        }
        EGLint ca[] = {EGL_CONTEXT_CLIENT_VERSION, gl_version, EGL_NONE};
        ctx = e.CreateContext(dpy, cfg, EGL_NO_CONTEXT, gl_version >= 2 ? ca : nullptr);
        if (ctx == EGL_NO_CONTEXT) {
            LOGE("eglCreateContext failed (0x%x)", e.GetError());
            return false;
        }
    }
    if (!make_surface()) return false;
    static bool told = false;
    if (!told) {
        told = true;
        const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        LOGI("GL: %s, %s", glGetString(GL_RENDERER), glGetString(GL_VERSION));
        LOGI("GL extensions: %s", ext ? ext : "(none)");
    }
    return true;
}

void egl_swap() {
    if (surf != EGL_NO_SURFACE) e.SwapBuffers(dpy, surf);
}

void egl_restart_surface() {
    if (dpy == EGL_NO_DISPLAY) return;
    e.MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surf != EGL_NO_SURFACE) e.DestroySurface(dpy, surf);
    surf = EGL_NO_SURFACE;
    make_surface();
}

void egl_stop() {
    if (dpy == EGL_NO_DISPLAY) return;
    e.MakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (surf != EGL_NO_SURFACE) e.DestroySurface(dpy, surf);
    if (ctx != EGL_NO_CONTEXT) e.DestroyContext(dpy, ctx);
    e.Terminate(dpy);
    surf = EGL_NO_SURFACE;
    ctx = EGL_NO_CONTEXT;
    dpy = EGL_NO_DISPLAY;
}
