// Smooth view: the game moves the player and the camera about 31 times a second (its scheduler steps
// them every 32 ms, a fixed distance each step, whatever the frame rate). Game logic is left exactly as
// it is, so speed never depends on the frame rate. Each drawn frame instead shows the camera blended
// between its last two positions by real time, by correcting the world's transform uniforms
// (inPMVMat) as they are uploaded. The first-person weapon and the HUD are drawn after a depth-only
// clear and are left alone.
#pragma once
#include "common.h"

void smooth_init(u32 image_lo, u32 image_hi);              // the game image is mapped (finds CIsTransform)
void smooth_note_uniform_name(u32 program, const char* name, int location);
void smooth_note_program(u32 program);
void smooth_note_clear(u32 mask);                          // before glClear
const float* smooth_uniform4fv(int location, int count, const float* v);  // before glUniform4fv
const float* smooth_matrix(int location, int count, bool transpose, const float* v);  // before glUniformMatrix4fv
void smooth_frame_end();                                   // after the frame is shown
void smooth_stats(char* out, size_t n);                    // one line for the statistics
