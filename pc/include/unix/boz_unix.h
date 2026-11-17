// Force-included into every port source file in the Unix PC builds (Linux, the BSDs, macOS, iOS): the PC layer's
// pieces the shared code uses, implemented in pc/src/host_unix.cpp and pc/src/vfs_win.cpp (which is portable).
#pragma once
#ifndef _WIN32
#include <string>

// Guest paths are Android paths ("/data/...", "/sdcard/..."); the PC builds keep them under the data folder.
std::string boz_host_path(const char* guest_path);

// Contents for Linux /proc files the game reads (meminfo); empty if not one of them.
std::string boz_fake_proc(const char* guest_path);

// The folder BOZ64 keeps its files in (boz64.log, boz64.props, peers.txt, data/), with no trailing slash: the
// program's own folder when a data/ folder is next to it (an unpacked .tar.gz), else the user's BOZ64 folder
// (~/.local/share/BOZ64 on Linux and the BSDs, ~/Library/Application Support/BOZ64 on macOS, Documents on iOS).
const char* boz_exe_dir();

void boz_log_flush();

#ifndef __linux__
int gettid();  // Linux has it; elsewhere host_unix.cpp gives each thread a number
#endif
#endif
