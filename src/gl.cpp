// OpenGL ES / EGL for guest code. Most functions are generated (gl_thunks.inc); the ones that pass
// pointers the guest can't use directly (strings, mapped buffers, pointer arrays) are here.
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>
// gl2ext.h must come after the core header
#include <GLES2/gl2ext.h>
#include <dlfcn.h>

#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpu.h"
#include "frame.h"
#include "resscale.h"
#include "smooth.h"

typedef khronos_int32_t GLclampx;

namespace {

void* gl_resolve(const char* name) {
    static void* libs[] = {dlopen("libGLESv2.so", RTLD_NOW), dlopen("libGLESv1_CM.so", RTLD_NOW),
                           dlopen("libGLESv3.so", RTLD_NOW), dlopen("libEGL.so", RTLD_NOW)};
    for (void* l : libs)
        if (l)
            if (void* p = dlsym(l, name)) return p;
    return reinterpret_cast<void*>(eglGetProcAddress(name));
}

void gl_unsupported(GuestThread& t, const char* name) {
    static std::mutex m;
    static std::unordered_map<std::string, int> seen;
    std::lock_guard lk(m);
    if (seen[name]++ == 0) LOGW("guest called %s, which the wrapper does not support", name);
    ret(t, 0);
}

// Opaque EGL objects and GLsync are host pointers; the guest gets small numbers instead.
std::mutex handle_lock;
std::unordered_map<void*, u32> handle_of;
std::vector<void*> handle_ptr{nullptr};
u32 gl_to_handle(void* p) {
    if (!p) return 0;
    std::lock_guard lk(handle_lock);
    auto it = handle_of.find(p);
    if (it != handle_of.end()) return it->second;
    u32 h = static_cast<u32>(handle_ptr.size());
    handle_ptr.push_back(p);
    handle_of[p] = h;
    return h;
}
void* gl_from_handle(u32 h) {
    if (!h) return nullptr;
    std::lock_guard lk(handle_lock);
    if (h < handle_ptr.size()) return handle_ptr[h];
    LOGW("guest passed unknown EGL/GL handle 0x%x", h);
    return reinterpret_cast<void*>(static_cast<uintptr_t>(h));
}

// Vertex/index "pointers" are offsets when a buffer object is bound, real pointers otherwise.
// Bindings are tracked per thread; -1 means unknown (ask GL).
thread_local s64 array_binding = -1, element_binding = -1;
void gl_note_bind_buffer(GLenum target, GLuint buf) {
    if (target == GL_ARRAY_BUFFER) array_binding = buf;
    if (target == GL_ELEMENT_ARRAY_BUFFER) element_binding = buf;
}
void gl_note_delete_buffers(GLsizei, const GLuint*) { array_binding = element_binding = -1; }
void gl_note_context_change() { array_binding = element_binding = -1; }
s64 query_binding(GLenum pname) {
    GLint v = 0;
    glGetIntegerv(pname, &v);
    return v;
}
const void* gl_attrib_ptr(u32 v) {
    if (array_binding < 0) array_binding = query_binding(GL_ARRAY_BUFFER_BINDING);
    if (array_binding || v < g::ALLOC_START) return reinterpret_cast<const void*>(static_cast<uintptr_t>(v));
    return g::h(v);
}
const void* gl_index_ptr(u32 v) {
    if (element_binding < 0) element_binding = query_binding(GL_ELEMENT_ARRAY_BUFFER_BINDING);
    if (element_binding || v < g::ALLOC_START) return reinterpret_cast<const void*>(static_cast<uintptr_t>(v));
    return g::h(v);
}

#include "gl_thunks.inc"

std::mutex str_lock;
std::map<std::pair<u32, u32>, u32> gl_strings;
u32 guest_gl_string(u32 key, u32 index, const char* s) {
    if (!s) return 0;
    std::lock_guard lk(str_lock);
    auto& slot = gl_strings[{key, index}];
    if (!slot || strcmp(g::str(slot), s) != 0) slot = g::data_str(s);
    return slot;
}

// Mapped buffers: the guest gets a copy in its own memory, written back on flush/unmap.
struct Mapping {
    void* host;
    u32 guest, length, access;
};
std::mutex map_lock;
std::map<std::pair<void*, GLenum>, Mapping> mappings;  // keyed by (current context, target)
std::pair<void*, GLenum> map_key(GLenum target) { return {eglGetCurrentContext(), target}; }

u32 start_mapping(GLenum target, void* host, u32 length, u32 access) {
    if (!host) return 0;
    u32 gbuf = g::malloc(length ? length : 1);
    if (access & GL_MAP_READ_BIT) memcpy(g::h(gbuf), host, length);
    std::lock_guard lk(map_lock);
    mappings[map_key(target)] = {host, gbuf, length, access};
    return gbuf;
}

GLboolean end_mapping(GLenum target, GLboolean (*unmap)(GLenum)) {
    Mapping m{};
    {
        std::lock_guard lk(map_lock);
        auto it = mappings.find(map_key(target));
        if (it != mappings.end()) {
            m = it->second;
            mappings.erase(it);
        }
    }
    if (m.host && (m.access & GL_MAP_WRITE_BIT) && !(m.access & GL_MAP_FLUSH_EXPLICIT_BIT))
        memcpy(m.host, g::h(m.guest), m.length);
    GLboolean r = unmap(target);
    if (m.guest) g::free(m.guest);
    return r;
}

void write_ptr_out(u32 out, void* p) {
    // Pointer-valued queries: guest addresses go back as they are; buffer offsets as numbers.
    g::wr32(out, g::in_guest(p) ? g::to_guest(p) : static_cast<u32>(reinterpret_cast<uintptr_t>(p)));
}

std::vector<const char*> guest_string_array(u32 arr, u32 count) {
    std::vector<const char*> v(count);
    for (u32 i = 0; i < count; i++) v[i] = g::str(g::rd32(arr + 4 * i));
    return v;
}

template <class F>
F fn(const char* name) {
    return reinterpret_cast<F>(gl_resolve(name));
}

// ETC1 where the host has none (ANGLE on Windows): the game's data pack is ETC1, and ETC1 data is valid ETC2 RGB8
// data, which every ES 3.0 context takes. The game is told ETC1 is there and its uploads are relabelled.
bool etc1_as_etc2() {
    static std::once_flag once;
    static bool v = false;
    std::call_once(once, [] {
        static auto get = fn<const GLubyte* (*)(GLenum)>("glGetString");
        const char* e = reinterpret_cast<const char*>(get(GL_EXTENSIONS));
        const char* ver = reinterpret_cast<const char*>(get(GL_VERSION));
        v = e && ver && !strstr(e, "GL_OES_compressed_ETC1_RGB8_texture") && strstr(ver, "OpenGL ES 3");
        if (v) LOGI("GL: no ETC1 on this GPU; ETC1 textures go up as ETC2");
    });
    return v;
}
GLenum etc1_format(GLenum f) { return f == GL_ETC1_RGB8_OES && etc1_as_etc2() ? GL_COMPRESSED_RGB8_ETC2 : f; }
}  // namespace

void register_gl() {
    register_generated_gl();

    reg("glGetString", +[](GLenum name) -> u32 {
        static auto f = fn<const GLubyte* (*)(GLenum)>("glGetString");
        const char* s = reinterpret_cast<const char*>(f(name));
        if (name == GL_EXTENSIONS && s && etc1_as_etc2()) {
            std::string ext = std::string(s) + " GL_OES_compressed_ETC1_RGB8_texture";
            return guest_gl_string(name, 0xffffffff, ext.c_str());
        }
        return guest_gl_string(name, 0xffffffff, s);
    });
    reg("glCompressedTexImage2D", +[](GLenum target, GLint level, GLenum ifmt, GLsizei w, GLsizei h, GLint border,
                                      GLsizei size, u32 data) {
        static auto f = fn<void (*)(GLenum, GLint, GLenum, GLsizei, GLsizei, GLint, GLsizei, const void*)>("glCompressedTexImage2D");
        f(target, level, etc1_format(ifmt), w, h, border, size, g::h(data));
    });
    reg("glCompressedTexSubImage2D", +[](GLenum target, GLint level, GLint x, GLint y, GLsizei w, GLsizei h,
                                         GLenum fmt, GLsizei size, u32 data) {
        static auto f = fn<void (*)(GLenum, GLint, GLint, GLint, GLsizei, GLsizei, GLenum, GLsizei, const void*)>("glCompressedTexSubImage2D");
        f(target, level, x, y, w, h, etc1_format(fmt), size, g::h(data));
    });
    reg("glGetStringi", +[](GLenum name, GLuint index) -> u32 {
        static auto f = fn<const GLubyte* (*)(GLenum, GLuint)>("glGetStringi");
        return f ? guest_gl_string(name, index, reinterpret_cast<const char*>(f(name, index))) : 0;
    });
    reg("glMapBufferRange", +[](GLenum target, s32 offset, s32 length, GLbitfield access) -> u32 {
        static auto f = fn<void* (*)(GLenum, GLintptr, GLsizeiptr, GLbitfield)>("glMapBufferRange");
        return f ? start_mapping(target, f(target, offset, length, access), length, access) : 0;
    });
    reg("glMapBufferRangeEXT", +[](GLenum target, s32 offset, s32 length, GLbitfield access) -> u32 {
        static auto f = fn<void* (*)(GLenum, GLintptr, GLsizeiptr, GLbitfield)>("glMapBufferRangeEXT");
        return f ? start_mapping(target, f(target, offset, length, access), length, access) : 0;
    });
    reg("glMapBufferOES", +[](GLenum target, GLenum access) -> u32 {
        static auto f = fn<void* (*)(GLenum, GLenum)>("glMapBufferOES");
        if (!f) return 0;
        GLint size = 0;
        glGetBufferParameteriv(target, GL_BUFFER_SIZE, &size);
        u32 a = access == GL_WRITE_ONLY_OES ? GL_MAP_WRITE_BIT : (GL_MAP_READ_BIT | GL_MAP_WRITE_BIT);
        return start_mapping(target, f(target, access), static_cast<u32>(size), a);
    });
    reg("glUnmapBuffer", +[](GLenum target) -> u32 {
        static auto f = fn<GLboolean (*)(GLenum)>("glUnmapBuffer");
        return f ? end_mapping(target, f) : 0;
    });
    reg("glUnmapBufferOES", +[](GLenum target) -> u32 {
        static auto f = fn<GLboolean (*)(GLenum)>("glUnmapBufferOES");
        return f ? end_mapping(target, f) : 0;
    });
    auto flush = +[](GLenum target, s32 offset, s32 length) {
        static auto f = fn<void (*)(GLenum, GLintptr, GLsizeiptr)>("glFlushMappedBufferRange");
        std::lock_guard lk(map_lock);
        auto it = mappings.find(map_key(target));
        if (it != mappings.end())
            memcpy(static_cast<u8*>(it->second.host) + offset, static_cast<u8*>(g::h(it->second.guest)) + offset, length);
        if (f) f(target, offset, length);
    };
    reg("glFlushMappedBufferRange", flush);
    reg("glFlushMappedBufferRangeEXT", flush);
    auto buffer_pointer = +[](GLenum target, GLenum, u32 out) {
        std::lock_guard lk(map_lock);
        auto it = mappings.find(map_key(target));
        g::wr32(out, it != mappings.end() ? it->second.guest : 0);
    };
    reg("glGetBufferPointerv", buffer_pointer);
    reg("glGetBufferPointervOES", buffer_pointer);
    reg("glGetPointerv", +[](GLenum pname, u32 out) {
        static auto f = fn<void (*)(GLenum, void**)>("glGetPointerv");
        void* p = nullptr;
        if (f) f(pname, &p);
        write_ptr_out(out, p);
    });
    reg("glGetVertexAttribPointerv", +[](GLuint index, GLenum pname, u32 out) {
        void* p = nullptr;
        glGetVertexAttribPointerv(index, pname, &p);
        write_ptr_out(out, p);
    });
    reg("glShaderSource", +[](GLuint shader, GLsizei count, u32 strings, const GLint* lengths) {
        auto v = guest_string_array(strings, count);
        glShaderSource(shader, count, v.data(), lengths);
    });
    reg("glTransformFeedbackVaryings", +[](GLuint program, GLsizei count, u32 names, GLenum mode) {
        static auto f = fn<void (*)(GLuint, GLsizei, const GLchar* const*, GLenum)>("glTransformFeedbackVaryings");
        auto v = guest_string_array(names, count);
        if (f) f(program, count, v.data(), mode);
    });
    reg("glGetUniformIndices", +[](GLuint program, GLsizei count, u32 names, GLuint* indices) {
        static auto f = fn<void (*)(GLuint, GLsizei, const GLchar* const*, GLuint*)>("glGetUniformIndices");
        auto v = guest_string_array(names, count);
        if (f) f(program, count, v.data(), indices);
    });

    reg("eglGetProcAddress", +[](const char* name) -> u32 {
        u32 a = find_thunk(name);
        if (!a) LOGI("guest eglGetProcAddress(%s): not available", name);
        return a;
    });
    reg("eglQueryString", +[](u32 dpy, EGLint name) -> u32 {
        const char* s = eglQueryString(static_cast<EGLDisplay>(gl_from_handle(dpy)), name);
        return guest_gl_string(0x10000 + name, dpy, s);
    });
    reg("eglGetConfigs", +[](u32 dpy, u32 configs, EGLint size, EGLint* num) -> u32 {
        std::vector<EGLConfig> c(configs ? size : 0);
        EGLBoolean r = eglGetConfigs(static_cast<EGLDisplay>(gl_from_handle(dpy)), configs ? c.data() : nullptr,
                                     configs ? size : 0, num);
        if (r && configs)
            for (EGLint i = 0; i < *num && i < size; i++) g::wr32(configs + 4 * i, gl_to_handle(c[i]));
        return r;
    });
    reg("eglChooseConfig", +[](u32 dpy, const EGLint* attribs, u32 configs, EGLint size, EGLint* num) -> u32 {
        std::vector<EGLConfig> c(configs ? size : 0);
        EGLBoolean r = eglChooseConfig(static_cast<EGLDisplay>(gl_from_handle(dpy)), attribs,
                                       configs ? c.data() : nullptr, configs ? size : 0, num);
        if (r && configs)
            for (EGLint i = 0; i < *num && i < size; i++) g::wr32(configs + 4 * i, gl_to_handle(c[i]));
        return r;
    });
}
