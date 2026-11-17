// Frame pacing: the frame-rate limiter that replaces the game's own, plus timing statistics.
#pragma once
#include "common.h"

void frame_init();
void frame_swap_begin();     // before eglSwapBuffers (the limiter waits here)
void frame_swap_end();       // after eglSwapBuffers
int frame_swap_interval(int requested);  // eglSwapInterval value to really use
void frame_sleep_begin();    // around guest sleeps, for the statistics
void frame_sleep_end(u32 requested_us);
extern thread_local u32 tl_thunk_calls;
void frame_note_data(const void* p, u32 bytes);     // statistics: transform data uploaded this frame
void frame_note_touch(u64 blocked_ns, int x = -1);  // statistics: a touch event went to the game (UI thread)
// statistics: which named matrix uniforms change from frame to frame
void frame_note_uniform_name(u32 program, const char* name, int location);
void frame_note_program(u32 program);
void frame_note_matrix(int location, const float* m, u32 bytes);
void frame_note_gl(char kind, u32 value);  // statistics: draw-order events for the one-frame dump
bool frame_skip_draw();  // debugging: debug.boz64.skip=<program> or z (after the depth-only clear)
void frame_note_caller(u32 lr, u32 ptr);  // debugging: guest caller of the next uniform upload
