// Resolution scale (see resscale.h).
//
// While a frame is redirected, the guest's binds of framebuffer 0 bind the offscreen framebuffer instead,
// and its viewport and scissor rectangles are scaled to it. The game never sees the difference: queries
// return its own values. Stretching the image onto the screen saves and restores every piece of GL state
// it touches, because the game's renderer caches state.
#include "resscale.h"

#include <EGL/egl.h>
#include <GLES2/gl2ext.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "settings.h"

namespace {
constexpr int WORLD_MIN_DRAWS = 20;  // a depth-only clear after this many draws ends a 3D world

struct Rect {
    GLint x, y;
    GLsizei w, h;
};

thread_local bool render_thread = false;

EGLContext ctx = EGL_NO_CONTEXT;
GLuint fbo, color_tex, depth_rb, program;
bool has_stencil, failed;
int fbo_w, fbo_h, scr_w, scr_h;
bool redirect;             // the offscreen framebuffer stands in for the screen
int frame_draws, fbo_draws;
bool world_seen, world_last;  // this / the previous frame drew a 3D world followed by a depth-only clear
GLuint game_fb;            // the framebuffer the game believes is bound
Rect game_vp, game_sc;     // the game's viewport and scissor box

using InvalidateFn = void (*)(GLenum, GLsizei, const GLenum*);
using BindVaoFn = void (*)(GLuint);
InvalidateFn invalidate;   // glInvalidateFramebuffer (ES 3) or glDiscardFramebufferEXT
BindVaoFn bind_vao;        // glBindVertexArray (ES 3) or glBindVertexArrayOES

bool ours() { return render_thread && redirect && game_fb == 0; }

Rect scaled(const Rect& r) {
    double sx = double(fbo_w) / scr_w, sy = double(fbo_h) / scr_h;
    GLint x0 = GLint(std::lround(r.x * sx)), y0 = GLint(std::lround(r.y * sy));
    GLint x1 = GLint(std::lround((r.x + r.w) * sx)), y1 = GLint(std::lround((r.y + r.h) * sy));
    return {x0, y0, x1 - x0, y1 - y0};
}

void apply_rects() {  // the game's viewport and scissor box, for whichever target is now bound
    Rect v = ours() ? scaled(game_vp) : game_vp, s = ours() ? scaled(game_sc) : game_sc;
    glViewport(v.x, v.y, v.w, v.h);
    glScissor(s.x, s.y, s.w, s.h);
}

GLuint compile(GLenum type, const char* src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512] = {};
        glGetShaderInfoLog(s, sizeof log, nullptr, log);
        LOGW("resolution scale: shader failed: %s", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

GLuint make_program() {
    GLuint vs = compile(GL_VERTEX_SHADER,
                        "attribute vec2 p; varying vec2 uv;\n"
                        "void main() { uv = p * 0.5 + 0.5; gl_Position = vec4(p, 0.0, 1.0); }\n");
    GLuint fs = compile(GL_FRAGMENT_SHADER,
                        "precision mediump float; varying vec2 uv; uniform sampler2D t;\n"
                        "void main() { gl_FragColor = texture2D(t, uv); }\n");
    if (!vs || !fs) return 0;
    GLuint p = glCreateProgram();
    glAttachShader(p, vs);
    glAttachShader(p, fs);
    glBindAttribLocation(p, 0, "p");
    glLinkProgram(p);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) {
        LOGW("resolution scale: program failed to link");
        glDeleteProgram(p);
        return 0;
    }
    return p;  // the sampler uniform defaults to texture unit 0
}

bool has_ext(const char* name) {
    const char* e = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (!e) return false;
    size_t n = strlen(name);
    for (const char* p = e; (p = strstr(p, name)) != nullptr; p += n)
        if ((p == e || p[-1] == ' ') && (p[n] == ' ' || p[n] == 0)) return true;
    return false;
}

// Creates (or resizes) the offscreen framebuffer for the current context.
bool resize(int w, int h) {
    if (!program) {
        const char* ver = reinterpret_cast<const char*>(glGetString(GL_VERSION));
        bool es3 = ver && strncmp(ver, "OpenGL ES 3", 11) == 0;
        invalidate = reinterpret_cast<InvalidateFn>(eglGetProcAddress(es3 ? "glInvalidateFramebuffer" : "glDiscardFramebufferEXT"));
        if (!es3 && !has_ext("GL_EXT_discard_framebuffer")) invalidate = nullptr;
        bind_vao = reinterpret_cast<BindVaoFn>(eglGetProcAddress(es3 ? "glBindVertexArray" : "glBindVertexArrayOES"));
        if (!es3 && !has_ext("GL_OES_vertex_array_object")) bind_vao = nullptr;
        has_stencil = es3 || has_ext("GL_OES_packed_depth_stencil");
        program = make_program();
        if (!program) return false;
        glGenFramebuffers(1, &fbo);
        glGenTextures(1, &color_tex);
        glGenRenderbuffers(1, &depth_rb);
    }
    GLint old_tex = 0, old_rb = 0, old_fb = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &old_rb);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &old_fb);

    glBindTexture(GL_TEXTURE_2D, color_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindRenderbuffer(GL_RENDERBUFFER, depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER,
                          has_stencil ? GL_DEPTH24_STENCIL8_OES
                          : has_ext("GL_OES_depth24") ? GL_DEPTH_COMPONENT24_OES : GL_DEPTH_COMPONENT16,
                          w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color_tex, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth_rb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, has_stencil ? depth_rb : 0);
    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);

    glBindFramebuffer(GL_FRAMEBUFFER, old_fb);
    glBindRenderbuffer(GL_RENDERBUFFER, old_rb);
    glBindTexture(GL_TEXTURE_2D, old_tex);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        LOGW("resolution scale: offscreen framebuffer incomplete (0x%x); drawing at full resolution", status);
        return false;
    }
    fbo_w = w;
    fbo_h = h;
    LOGI("resolution scale: 3D view drawn at %dx%d (screen %dx%d)", w, h, scr_w, scr_h);
    return true;
}

// Draws the offscreen image over the whole screen, leaving all GL state as it was.
void stretch_to_screen() {
    GLint prog = 0, abuf = 0, active = 0, tex = 0, vao = 0;
    GLint a_en = 0, a_size = 4, a_type = GL_FLOAT, a_norm = 0, a_stride = 0, a_buf = 0;
    void* a_ptr = nullptr;
    GLboolean cmask[4], dmask;
    GLint smask = 0;
    const GLenum caps[] = {GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST};
    GLboolean cap_on[5];
    for (int i = 0; i < 5; i++) cap_on[i] = glIsEnabled(caps[i]);
    glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &abuf);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    if (bind_vao) {
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING_OES, &vao);
        if (vao) bind_vao(0);
    }
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &a_en);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_SIZE, &a_size);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_TYPE, &a_type);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &a_norm);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &a_stride);
    glGetVertexAttribiv(0, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &a_buf);
    glGetVertexAttribPointerv(0, GL_VERTEX_ATTRIB_ARRAY_POINTER, &a_ptr);
    glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
    glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
    glGetIntegerv(GL_STENCIL_WRITEMASK, &smask);

    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    for (GLenum c : caps) glDisable(c);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_TRUE);
    glStencilMask(0xff);
    // clearing first means a tiled GPU doesn't load the old screen contents from memory
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    glViewport(0, 0, scr_w, scr_h);
    glUseProgram(program);
    glBindTexture(GL_TEXTURE_2D, color_tex);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    static const float quad[8] = {-1, -1, 1, -1, -1, 1, 1, 1};
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glEnableVertexAttribArray(0);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (invalidate) {  // the offscreen depth is not needed any more: don't write it back to memory
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        const GLenum att[2] = {GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT};
        invalidate(GL_FRAMEBUFFER, has_stencil ? 2 : 1, att);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
    }

    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(a_buf));
    glVertexAttribPointer(0, a_size, static_cast<GLenum>(a_type), a_norm ? GL_TRUE : GL_FALSE, a_stride, a_ptr);
    if (!a_en) glDisableVertexAttribArray(0);
    glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(abuf));
    glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(tex));
    glActiveTexture(static_cast<GLenum>(active));
    glUseProgram(static_cast<GLuint>(prog));
    if (vao) bind_vao(static_cast<GLuint>(vao));
    for (int i = 0; i < 5; i++)
        if (cap_on[i]) glEnable(caps[i]);
    glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
    glDepthMask(dmask);
    glStencilMask(static_cast<GLuint>(smask));
}

// Ends the redirection: the world drawn so far goes onto the screen, which is bound again.
void finish() {
    redirect = false;
    if (fbo_draws > 0) stretch_to_screen();
    else glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (game_fb != 0) glBindFramebuffer(GL_FRAMEBUFFER, game_fb);
    apply_rects();
}
}  // namespace

void res_frame_start() {
    render_thread = true;
    world_last = world_seen;
    world_seen = false;
    frame_draws = fbo_draws = 0;
    EGLContext c = eglGetCurrentContext();
    EGLDisplay d = eglGetCurrentDisplay();
    EGLSurface s = eglGetCurrentSurface(EGL_DRAW);
    if (c == EGL_NO_CONTEXT || s == EGL_NO_SURFACE) return;
    if (c != ctx) {  // a new context: the old one's objects went with it
        ctx = c;
        fbo = color_tex = depth_rb = program = 0;
        fbo_w = fbo_h = 0;
        failed = false;
        game_fb = 0;
    }
    int pct = settings().res_scale;
    if (pct >= 100 || !world_last || failed) return;
    EGLint w = 0, h = 0;
    eglQuerySurface(d, s, EGL_WIDTH, &w);
    eglQuerySurface(d, s, EGL_HEIGHT, &h);
    if (w <= 0 || h <= 0) return;
    scr_w = w;
    scr_h = h;
    int fw = std::max(16, int(std::lround(w * pct / 100.0))), fh = std::max(16, int(std::lround(h * pct / 100.0)));
    if ((fw != fbo_w || fh != fbo_h) && !resize(fw, fh)) {
        failed = true;
        return;
    }
    // the screen is bound and nothing is scaled, so GL holds the game's own rectangles
    GLint v[4];
    glGetIntegerv(GL_VIEWPORT, v);
    game_vp = {v[0], v[1], v[2], v[3]};
    glGetIntegerv(GL_SCISSOR_BOX, v);
    game_sc = {v[0], v[1], v[2], v[3]};
    redirect = true;
    if (game_fb == 0) glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    apply_rects();
}

void res_frame_end() {
    if (render_thread && redirect) finish();
}

void res_note_draw() {
    if (!render_thread) return;
    frame_draws++;
    if (ours()) fbo_draws++;
}

GLuint res_bind_framebuffer(GLenum target, GLuint fb) {
    if (!render_thread || target != GL_FRAMEBUFFER) return fb;
    bool was = ours();
    game_fb = fb;
    if (was != ours()) apply_rects();  // switching between the screen's stand-in and the game's own target
    return fb == 0 && redirect ? fbo : fb;
}

void res_viewport(GLint& x, GLint& y, GLsizei& w, GLsizei& h) {
    if (!render_thread) return;
    game_vp = {x, y, w, h};
    if (!ours()) return;
    Rect r = scaled(game_vp);
    x = r.x, y = r.y, w = r.w, h = r.h;
}

void res_scissor(GLint& x, GLint& y, GLsizei& w, GLsizei& h) {
    if (!render_thread) return;
    game_sc = {x, y, w, h};
    if (!ours()) return;
    Rect r = scaled(game_sc);
    x = r.x, y = r.y, w = r.w, h = r.h;
}

void res_clear(GLbitfield mask) {
    if (!render_thread || (mask & GL_COLOR_BUFFER_BIT) || !(mask & GL_DEPTH_BUFFER_BIT)) return;
    if (frame_draws >= WORLD_MIN_DRAWS) world_seen = true;
    if (ours()) finish();  // the weapon and the HUD follow, at full resolution
}

void res_fix_get(GLenum pname, GLint* out) {
    if (!out || !ours()) return;
    if (pname == GL_VIEWPORT) memcpy(out, &game_vp, sizeof game_vp);
    else if (pname == GL_SCISSOR_BOX) memcpy(out, &game_sc, sizeof game_sc);
    else if (pname == GL_FRAMEBUFFER_BINDING && out[0] == GLint(fbo)) out[0] = 0;
}
