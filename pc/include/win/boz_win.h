// Force-included into every port source file in the Windows build: the few POSIX pieces the shared code uses,
// declared here and implemented in pc/src/posix_win.cpp. windows.h is deliberately not included (its macros
// collide with the shared code); Windows-only files include it themselves.
#pragma once
#ifdef _WIN32
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <direct.h>
#include <process.h>
#include <string>

#ifndef _SSIZE_T_DEFINED
#define _SSIZE_T_DEFINED
typedef std::intptr_t ssize_t;
#endif
typedef std::int64_t off64_t;
typedef std::uint32_t uid_t;
typedef std::uint32_t gid_t;
typedef int pid_t;
typedef std::uint32_t mode_t;
typedef std::uint32_t socklen_t_boz;

// clocks (Linux numbering, which is also what the guest uses)
#ifndef CLOCK_REALTIME
#define CLOCK_REALTIME 0
#define CLOCK_MONOTONIC 1
#define CLOCK_PROCESS_CPUTIME_ID 2
#define CLOCK_THREAD_CPUTIME_ID 3
#define CLOCK_MONOTONIC_RAW 4
#define CLOCK_REALTIME_COARSE 5
#define CLOCK_MONOTONIC_COARSE 6
#define CLOCK_BOOTTIME 7
#endif
#define TIMER_ABSTIME 1
typedef int clockid_t;
int clock_gettime(clockid_t clk, struct timespec* ts);
int clock_getres(clockid_t clk, struct timespec* ts);
int clock_nanosleep(clockid_t clk, int flags, const struct timespec* req, struct timespec* rem);
int nanosleep(const struct timespec* req, struct timespec* rem);
int usleep(unsigned int us);
unsigned int sleep(unsigned int s);
int sched_yield(void);
int gettid(void);

#define _SC_NPROCESSORS_CONF 83
#define _SC_NPROCESSORS_ONLN 84
#define _SC_PAGESIZE 39
long sysconf(int name);

inline int strcasecmp(const char* a, const char* b) { return _stricmp(a, b); }
inline int strncasecmp(const char* a, const char* b, size_t n) { return _strnicmp(a, b, n); }

inline void* memmem(const void* h, size_t hn, const void* n, size_t nn) {
    if (nn == 0) return const_cast<void*>(h);
    const unsigned char* p = static_cast<const unsigned char*>(h);
    const unsigned char* e = p + hn;
    const unsigned char first = *static_cast<const unsigned char*>(n);
    while (hn >= nn) {
        const void* q = memchr(p, first, hn - nn + 1);
        if (!q) return nullptr;
        p = static_cast<const unsigned char*>(q);
        if (memcmp(p, n, nn) == 0) return const_cast<unsigned char*>(p);
        p++;
        hn = static_cast<size_t>(e - p);
    }
    return nullptr;
}
inline int mkdir(const char* p, mode_t) { return _mkdir(p); }
FILE* fmemopen(void* buf, size_t size, const char* mode);  // read-only: a temporary file holding the bytes

// Guest paths are Android paths ("/data/...", "/sdcard/..."); the PC build keeps them under its data folder
// (pc/src/vfs_win.cpp).
std::string boz_host_path(const char* guest_path);

// Contents for Linux /proc files the game reads (meminfo); empty if not one of them (pc/src/vfs_win.cpp).
std::string boz_fake_proc(const char* guest_path);

// The folder the PC build keeps its files in (next to the .exe), with forward slashes and no trailing slash.
const char* boz_exe_dir();
#endif
