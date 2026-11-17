// Windows stand-in for dlopen/dlsym on host libraries: "libGLESv2.so" and friends load ANGLE's DLLs from the .exe's
// folder (pc/src/posix_win.cpp).
#pragma once
#define RTLD_LAZY 1
#define RTLD_NOW 2
#define RTLD_LOCAL 0
#define RTLD_GLOBAL 0x100
typedef struct {
    const char* dli_fname;
    void* dli_fbase;
    const char* dli_sname;
    void* dli_saddr;
} Dl_info;
void* dlopen(const char* name, int flags);
void* dlsym(void* handle, const char* sym);
int dlclose(void* handle);
const char* dlerror(void);
int dladdr(const void* addr, Dl_info* info);
