// What the PC build's pieces offer each other: the window and its thread (window_win.cpp), EGL (egl_win.cpp),
// sound and music (audio_win.cpp) and the stand-in Java classes (marmalade_win.cpp).
#pragma once
#include <cstdint>
#include <functional>
#include <string>

// ---- window and the UI thread (the thread that made the window and runs its message loop) ----
bool win_create(int width, int height);
int win_run_loop();  // returns the exit code
void win_quit(int code);
int win_width();
int win_height();
void* win_hwnd();
bool on_ui_thread();
void ui_post(std::function<void()> f);  // runs f on the UI thread later
void ui_call(std::function<void()> f);  // runs f on the UI thread and waits (runs it directly on the UI thread)
void win_set_title(const std::string& s);

// ---- EGL (ANGLE) for the game window ----
// Creates the display, config, context and window surface and makes them current on the calling thread.
// attribs: the config attributes from the game's [GL] settings (EGL_NONE-terminated pairs, without the
// renderable type, which is added for version >= 2).
bool egl_start(int version, const int* attribs);
void egl_swap();
void egl_stop();             // destroys the surface and context and terminates the display
void egl_restart_surface();  // recreates the window surface (keeps the context)

// ---- sound: one output stream with the game's PCM sound and up to 16 music channels mixed in ----
void audio_open();  // the output device and its thread; music works from here on
int audio_sfx_init(int volume, bool stereo, int rate);  // returns the rate the game should mix at, 0 on failure
void audio_sfx_start();
void audio_sfx_stop();
void audio_sfx_volume(int volume);  // 0..100
void audio_suspend();
void audio_resume();

constexpr int MUSIC_CHANNELS = 16;
// path is a host path; size > 0 plays the byte range [offset, offset + size) of the file. repeat 0 = loop forever.
// Returns 0, -1 (file not found) or -2 (other error).
int music_play(const std::string& path, int repeat, int64_t offset, int64_t size, int ch);
void music_stop(int ch);
int music_pause(int ch);   // 0 if paused, -1 if it was not playing
int music_resume(int ch);  // 0 if resumed, -1 if it was not paused
int music_status(int ch);  // 1 playing, 2 paused, 3 error, 0 otherwise
int music_position_ms(int ch);
int music_duration_ms(int ch);
void music_set_position_ms(int ch, int ms);
void music_set_volume(int ch, int volume);  // 0..100
bool music_is_playing(int ch);
void music_stop_all();
void music_suspend();
void music_resume_all();
extern void (*g_music_stopped)(int ch);  // called (from the audio thread) when a channel stops by itself

// ---- the stand-in Java side (marmalade_win.cpp) ----
void marm_define_classes();
void marm_start(const std::string& files_dir, const std::string& apk_path, const std::string& lib_dir);
void marm_touch(int id, int type, int x, int y);  // type 4 down, 5 up, 6 move, 7 cancel
bool marm_key(int android_keycode, int unicode, bool down);
void marm_run_on_os_tick();
void marm_surface_changed(int w, int h);
void marm_suspend();
void marm_resume();
void marm_shutdown();
// The game is quitting (it shut its graphics down while it was not paused: "Quit" in its menu, or the window closed).
// Its own teardown then crashes (see main_win.cpp's on_abort), which only ends the game a moment early.
bool marm_quitting();

// The game's PCM sound for the audio thread: fills frames * channels interleaved samples (false before soundStart).
bool marm_generate_audio(int16_t* out, int frames, int channels);
