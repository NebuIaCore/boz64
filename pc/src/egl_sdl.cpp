// The game's OpenGL ES context through SDL (what egl_win.cpp does with ANGLE's EGL on Windows, and the app's
// LoaderGL Java class on Android). SDL is told to use EGL (window_sdl.cpp), so the context lives on the same EGL
// display the shared code queries directly (src/gl.cpp, src/frame.cpp): Mesa's on Linux and the BSDs.
// The context is made on the UI thread, which SDL prefers, then handed to the engine thread that draws.
#include <SDL3/SDL.h>

#include <EGL/egl.h>
#include <GLES2/gl2.h>

#include "common.h"
#include "platform.h"

namespace {

SDL_GLContext ctx = nullptr;

void log_gl_once() {
    static bool told = false;
    if (told) return;
    told = true;
    const char* ext = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    LOGI("GL: %s, %s, %s", glGetString(GL_VENDOR), glGetString(GL_RENDERER), glGetString(GL_VERSION));
    LOGI("GL extensions: %s", ext ? ext : "(none)");
    int r = 0, g = 0, b = 0, a = 0, d = 0, s = 0;
    SDL_GL_GetAttribute(SDL_GL_RED_SIZE, &r);
    SDL_GL_GetAttribute(SDL_GL_GREEN_SIZE, &g);
    SDL_GL_GetAttribute(SDL_GL_BLUE_SIZE, &b);
    SDL_GL_GetAttribute(SDL_GL_ALPHA_SIZE, &a);
    SDL_GL_GetAttribute(SDL_GL_DEPTH_SIZE, &d);
    SDL_GL_GetAttribute(SDL_GL_STENCIL_SIZE, &s);
    LOGI("GL config: RGBA %d%d%d%d depth %d stencil %d", r, g, b, a, d, s);
}

}  // namespace

// The window's pixel format was fixed when it was made (window_sdl.cpp asks for RGBA8888, depth 24, stencil 8: at
// least what the game's [GL] settings ask for), so only the version is taken from the game here.
bool egl_start(int version, const int*) {
    auto* win = static_cast<SDL_Window*>(win_hwnd());
    if (!win) return false;
    if (!ctx) {
        int major = version >= 2 ? version : 1, minor = version >= 2 ? 0 : 1;
        bool ok = false;
        ui_call([&] {
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major);
            SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor);
            ctx = SDL_GL_CreateContext(win);
            if (!ctx) {
                LOGE("could not create the OpenGL ES %d.%d context: %s", major, minor, SDL_GetError());
                return;
            }
            SDL_GL_MakeCurrent(win, nullptr);  // released here, made current on the engine thread below
            ok = true;
        });
        if (!ok) return false;
    }
    if (!SDL_GL_MakeCurrent(win, ctx)) {
        LOGE("could not make the OpenGL ES context current: %s", SDL_GetError());
        return false;
    }
    log_gl_once();
    return true;
}

void egl_swap() {
    if (ctx) SDL_GL_SwapWindow(static_cast<SDL_Window*>(win_hwnd()));
}

// SDL resizes the window's surface itself; making the context current again is all that is left to do.
void egl_restart_surface() {
    if (ctx) SDL_GL_MakeCurrent(static_cast<SDL_Window*>(win_hwnd()), ctx);
}

void egl_stop() {
    if (!ctx) return;
    SDL_GL_MakeCurrent(static_cast<SDL_Window*>(win_hwnd()), nullptr);
    SDL_GL_DestroyContext(ctx);
    ctx = nullptr;
}
