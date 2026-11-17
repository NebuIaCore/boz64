// Resolution scale: the 3D world is drawn into an offscreen framebuffer at res_scale percent of the
// screen's resolution and stretched onto the screen when the game starts the first-person weapon and the
// HUD (its depth-only clear), which are then drawn at full resolution. Frames without that clear (menus,
// loading screens) are drawn at full resolution. Everything here runs on the game's render thread.
#pragma once
#include <GLES2/gl2.h>

#include "common.h"

void res_frame_start();  // after the frame is shown (decides whether the next frame is redirected)
void res_frame_end();    // before the frame is shown (stretches a redirected frame onto the screen)

// Called from the guest's GL calls: they may change the arguments.
GLuint res_bind_framebuffer(GLenum target, GLuint fb);
void res_viewport(GLint& x, GLint& y, GLsizei& w, GLsizei& h);
void res_scissor(GLint& x, GLint& y, GLsizei& w, GLsizei& h);
void res_clear(GLbitfield mask);  // before glClear
void res_note_draw();             // after glDrawArrays / glDrawElements
void res_fix_get(GLenum pname, GLint* out);  // after glGetIntegerv
