# Generates src/gl_thunks.inc: one guest->host thunk per OpenGL ES / EGL function in the NDK headers.
# usage: python gen_gl.py <ndk sysroot include dir> <output file>
import re
import sys

INC, OUT = sys.argv[1], sys.argv[2]
HEADERS = ['GLES/gl.h', 'GLES/glext.h', 'GLES2/gl2.h', 'GLES2/gl2ext.h', 'GLES3/gl3.h', 'GLES3/gl31.h',
           'GLES3/gl32.h', 'EGL/egl.h', 'EGL/eglext.h']
PROTO = re.compile(r'^(?:GL_API|GL_APICALL|EGLAPI)\s+(.*?)\s*(?:GL_APIENTRY|EGLAPIENTRY)\s+(\w+)\s*\((.*?)\)\s*;',
                   re.M | re.S)

HANDLE = {'EGLDisplay', 'EGLSurface', 'EGLContext', 'EGLConfig', 'EGLClientBuffer', 'EGLImage', 'EGLImageKHR',
          'EGLSync', 'EGLSyncKHR', 'EGLNativeWindowType', 'EGLNativePixmapType', 'EGLNativeDisplayType', 'GLsync',
          'GLeglImageOES', 'EGLStreamKHR', 'EGLDeviceEXT', 'EGLOutputLayerEXT', 'EGLOutputPortEXT',
          'GLeglClientBufferEXT', 'EGLObjectKHR', 'EGLLabelKHR', 'EGLSyncNV'}
I64 = {'GLint64', 'GLuint64', 'GLint64EXT', 'GLuint64EXT', 'EGLTime', 'EGLTimeKHR', 'EGLuint64KHR',
       'EGLnsecsANDROID', 'EGLTimeNV', 'EGLuint64NV'}
FLOAT = {'GLfloat', 'GLclampf'}
INTPTR = {'GLintptr', 'GLsizeiptr', 'EGLAttrib', 'EGLAttribKHR', 'EGLsizeiANDROID'}
INT = {'GLenum', 'GLboolean', 'GLbitfield', 'GLbyte', 'GLshort', 'GLint', 'GLsizei', 'GLubyte', 'GLushort',
       'GLuint', 'GLfixed', 'GLclampx', 'GLhalf', 'EGLint', 'EGLBoolean', 'EGLenum', 'EGLNativeFileDescriptorKHR',
       'GLchar', 'int', 'unsigned int'}
# Written by hand in gl.cpp.
MANUAL = {'glGetString', 'glGetStringi', 'glMapBufferRange', 'glMapBufferOES', 'glMapBufferRangeEXT',
          'glUnmapBuffer', 'glUnmapBufferOES', 'glFlushMappedBufferRange', 'glFlushMappedBufferRangeEXT',
          'glGetBufferPointerv', 'glGetBufferPointervOES', 'glGetPointerv', 'glGetPointervKHR',
          'glGetVertexAttribPointerv', 'glShaderSource', 'glTransformFeedbackVaryings', 'glGetUniformIndices',
          'eglGetProcAddress', 'eglQueryString', 'eglGetConfigs', 'eglChooseConfig'}
POST_HOOK = {
    'glBindBuffer': 'gl_note_bind_buffer(a0, a1);',
    'glDeleteBuffers': 'gl_note_delete_buffers(a0, a1);',
    'glBindVertexArray': 'gl_note_context_change();',
    'glBindVertexArrayOES': 'gl_note_context_change();',
    'eglMakeCurrent': 'gl_note_context_change();',
    'eglSwapBuffers': 'gl_note_context_change(); frame_swap_end();',
    'glUniformMatrix4fv': 'frame_note_matrix(a0, a3, a1 * 64);',
    'glUniform4fv': 'frame_note_matrix(a0, a2, a1 * 16);',
    'glGetUniformLocation': 'frame_note_uniform_name(a0, a1, r); smooth_note_uniform_name(a0, a1, r);',
    'glUseProgram': 'frame_note_program(a0); smooth_note_program(a0);',
    'glLoadMatrixf': 'frame_note_data(a0, 64);',
    'glClear': "frame_note_gl('C', a0);",
    'glDepthRangef': "frame_note_gl('R', static_cast<u32>(a1 * 1000));",
    'glDepthMask': "frame_note_gl('K', a0);",
    'glViewport': "frame_note_gl('V', static_cast<u32>(a2));",
    'glDrawElements': "frame_note_gl('d', static_cast<u32>(a1)); res_note_draw();",
    'glDrawArrays': "frame_note_gl('a', static_cast<u32>(a2)); res_note_draw();",
    'glGetIntegerv': 'res_fix_get(a0, a1);',
}
PRE_HOOK = {
    'eglSwapBuffers': 'frame_swap_begin();',
    'glDrawElements': 'if (frame_skip_draw()) return;',
    'glDrawArrays': 'if (frame_skip_draw()) return;',
    'glUniform4fv': 'a2 = smooth_uniform4fv(a0, a1, a2);',
    'glClear': 'smooth_note_clear(a0); res_clear(a0);',
    'glBindFramebuffer': 'a1 = res_bind_framebuffer(a0, a1);',
    'glBindFramebufferOES': 'a1 = res_bind_framebuffer(a0, a1);',
    'glViewport': 'res_viewport(a0, a1, a2, a3);',
    'glScissor': 'res_scissor(a0, a1, a2, a3);',
    'glUniformMatrix4fv': 'a3 = smooth_matrix(a0, a1, a2 != 0, a3);',
}


def split_params(s):
    s = ' '.join(s.split())
    if s in ('', 'void'):
        return []
    out = []
    for p in s.split(','):
        p = p.strip()
        m = re.match(r'^(.*?)(\w+)$', p)
        typ, name = (m.group(1).strip(), m.group(2)) if m else ('', '')
        if not typ:  # unnamed parameter
            typ, name = p, ''
        out.append((typ, name))
    return out


def base_type(t):
    return t.replace('const', '').replace('*', '').strip()


def arg_expr(typ, name):
    """C++ expression that reads one guest argument of this type, or None if unsupported."""
    b = base_type(typ)
    stars = typ.count('*')
    if stars == 0:
        if b in FLOAT:
            return f'std::bit_cast<{typ}>(c.word())'
        if b in I64:
            return f'static_cast<{typ}>(c.dword())'
        if b in INTPTR:
            return f'static_cast<{typ}>(static_cast<s32>(c.word()))'
        if b in HANDLE:
            return f'reinterpret_cast<{typ}>(gl_from_handle(c.word()))'
        if b in INT:
            return f'static_cast<{typ}>(c.word())'
        return None
    if stars > 1 or 'const*' in typ.replace(' ', '') and typ.count('*') > 1:
        return None
    if b in HANDLE or b in INTPTR or b.startswith('GLDEBUGPROC') or 'Func' in b or b.startswith('struct'):
        return None  # arrays of handles / pointer-sized values / callbacks need conversion
    if b == 'void' and name == 'pointer':
        return f'reinterpret_cast<{typ}>(gl_attrib_ptr(c.word()))'
    if b == 'void' and name == 'indices':
        return f'reinterpret_cast<{typ}>(gl_index_ptr(c.word()))'
    if b == 'void' and name == 'indirect':
        return f'reinterpret_cast<{typ}>(static_cast<uintptr_t>(c.word()))'
    return f'reinterpret_cast<{typ}>(g::h(c.word()))'


def ret_stmt(rtype):
    b = base_type(rtype)
    if rtype == 'void':
        return ''
    if '*' in rtype:
        return None
    if b in FLOAT:
        return 'ret(t, std::bit_cast<u32>(r));'
    if b in I64:
        return 'ret64(t, static_cast<u64>(r));'
    if b in INTPTR or b in INT:
        return 'ret(t, static_cast<u32>(r));'
    if b in HANDLE:
        return 'ret(t, gl_to_handle(r));'
    return None


funcs = {}
for h in HEADERS:
    text = open(f'{INC}/{h}', encoding='latin1').read()
    for m in PROTO.finditer(text):
        rtype, name, params = ' '.join(m.group(1).split()), m.group(2), m.group(3)
        if name not in funcs:
            funcs[name] = (rtype, split_params(params))

out = ['// Generated by tools/gen_gl.py from the NDK GLES/EGL headers. Do not edit.', '']
names = []
unsupported = []
for name, (rtype, params) in sorted(funcs.items()):
    if name in MANUAL:
        continue
    exprs = [arg_expr(t, n) for t, n in params]
    rs = ret_stmt(rtype)
    sig = ', '.join(t for t, _ in params)
    if None in exprs or rs is None:
        unsupported.append(name)
        out.append(f'static void th_{name}(GuestThread& t) {{ gl_unsupported(t, "{name}"); }}')
        names.append(name)
        continue
    body = [f'static void th_{name}(GuestThread& t) {{',
            f'    using F = {rtype} (*)({sig});',
            f'    static F fp = reinterpret_cast<F>(gl_resolve("{name}"));',
            f'    if (!fp) return gl_unsupported(t, "{name}");']
    if params:
        body.append('    ArgCursor c{t};')
    for i, ((typ, _), e) in enumerate(zip(params, exprs)):
        body.append(f'    {typ} a{i} = {e};')
    if name == 'eglSwapInterval':
        body.append('    a1 = frame_swap_interval(a1);')
    if name in PRE_HOOK:
        body.append('    ' + PRE_HOOK[name])
    call = f'fp({", ".join(f"a{i}" for i in range(len(params)))})'
    if rtype == 'void':
        body.append(f'    {call};')
    else:
        body.append(f'    {rtype} r = {call};')
    if name in POST_HOOK:
        body.append('    ' + POST_HOOK[name])
    if rs:
        body.append('    ' + rs)
    body.append('}')
    out += body
    names.append(name)

out.append('')
out.append('static void register_generated_gl() {')
for n in names:
    out.append(f'    reg_raw("{n}", th_{n});')
out.append('}')
open(OUT, 'w', newline='\n').write('\n'.join(out) + '\n')
print(f'{len(names)} functions ({len(unsupported)} without conversion support), {len(MANUAL)} manual')
print('unsupported:', ' '.join(unsupported))
