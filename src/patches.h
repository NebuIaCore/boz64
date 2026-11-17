// Fixes applied to the game's own code once the engine has loaded it (found by byte signature, so they
// do not depend on where the code lands in guest memory). A patched instruction becomes a Thumb
// "svc #0x8N", handled by patches_svc on the host, which then carries on with the next instruction.
#pragma once
#include "common.h"

struct GuestThread;
#include <atomic>
#include <string>

void patches_on_code(u32 start, u32 end);   // the guest flushed newly written code in [start, end)
// The original engine (libs3e_android.so) once it is loaded and relocated (bias: guest address = bias + ELF address),
// before it starts the game.
void patches_on_engine(u32 bias);
// The game's code is read into an anonymous executable mmap and never flushed, so the scan happens the
// first time the JIT fetches code from such a region (by then it is loaded and relocated).
void patches_note_exec_region(u32 start, u32 len);
extern std::atomic<int> g_patch_regions_pending;
void patches_on_fetch(u32 addr);
bool patches_svc(GuestThread& t, u32 svc);  // false if svc is not one of ours
void patches_stats();  // statistics: log the game clock's state
// The game scheduler's latest update for channels 0 and 1: ticks it ran, the slot of the first one, slots in
// the channel, tick length, time accumulated toward the next tick, when the update read its clock, and a count
// of updates.
struct SchedInfo { u32 ticks, first_slot, slots, period_ms, left_ms; u64 at_ns; u32 seq; };
void patches_sched_last(SchedInfo* out);
