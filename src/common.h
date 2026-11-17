// Shared definitions for the 32-bit-on-64-bit wrapper.
// The 32-bit game code ("guest") lives in a 4 GB block of this process's memory. A guest address
// is a 32-bit offset into that block, so guest pointers convert to host pointers by adding the base.
#pragma once
#include <android/log.h>
#include <cstdint>
#include <cstring>
#include <string>

using u8 = uint8_t;
using u16 = uint16_t;
using u32 = uint32_t;
using u64 = uint64_t;
using s8 = int8_t;
using s16 = int16_t;
using s32 = int32_t;
using s64 = int64_t;

#define LOG_TAG "boz64"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
extern bool g_trace;  // log every thunk call (set with the boz64.trace system property)
#define TRACE(...) do { if (g_trace) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__); } while (0)

extern bool g_precise;  // exact fault addresses: dynarmic checks for a halt after every memory access
[[noreturn]] void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
extern void (*g_fatal_hook)();  // called once by fatal() before aborting (dumps the guest state)

namespace g {

// Guest memory map
constexpr u32 NULL_GUARD = 0x00010000;   // [0, 64K) is unmapped so null pointers fault
constexpr u32 ALLOC_START = 0x00100000;  // page allocator range (libraries, heap, stacks, mmap)
constexpr u32 ALLOC_END = 0xF0000000;
constexpr u32 THUNK_BASE = 0xF0000000;   // 8-byte "svc #n; bx lr" stubs, one per host function
constexpr u32 THUNK_MAX = 0x20000;
constexpr u32 DATA_BASE = 0xF0200000;    // host-provided guest data (JNI tables, __sF, strings)
constexpr u32 DATA_END = 0xF1000000;
constexpr u32 KUSER_PAGE = 0xFFFF0000;   // Linux ARM kernel user helpers
constexpr u32 PAGE = 4096;

extern u8* mem;

inline void* h(u32 a) { return a ? mem + a : nullptr; }
template <class T> inline T* p(u32 a) { return a ? reinterpret_cast<T*>(mem + a) : nullptr; }
inline bool in_guest(const void* hp) {
    return hp >= mem && hp < mem + 0x100000000ull;
}
inline u32 to_guest(const void* hp) {
    if (!hp) return 0;
    if (!in_guest(hp)) fatal("host pointer %p is outside guest memory", hp);
    return static_cast<u32>(static_cast<const u8*>(hp) - mem);
}
inline const char* str(u32 a) { return static_cast<const char*>(h(a)); }
inline u8 rd8(u32 a) { return mem[a]; }
inline u16 rd16(u32 a) { u16 v; memcpy(&v, mem + a, 2); return v; }
inline u32 rd32(u32 a) { u32 v; memcpy(&v, mem + a, 4); return v; }
inline u64 rd64(u32 a) { u64 v; memcpy(&v, mem + a, 8); return v; }
inline void wr8(u32 a, u8 v) { mem[a] = v; }
inline void wr16(u32 a, u16 v) { memcpy(mem + a, &v, 2); }
inline void wr32(u32 a, u32 v) { memcpy(mem + a, &v, 4); }
inline void wr64(u32 a, u64 v) { memcpy(mem + a, &v, 8); }
inline float rdf(u32 a) { float v; memcpy(&v, mem + a, 4); return v; }
inline void wrf(u32 a, float v) { memcpy(mem + a, &v, 4); }

// memory.cpp
void init_memory();
u32 page_alloc(u32 size, u32 align = PAGE);  // zero-filled pages, 0 on failure
void page_free(u32 addr, u32 size);
u32 data_alloc(u32 size, u32 align = 8);       // permanent allocation in the host data area
u32 data_str(const char* s);                   // permanent copy of a string
u32 malloc(u32 n);
void free(u32 p);
u32 realloc(u32 p, u32 n);
u32 calloc(u32 n, u32 sz);
u32 memalign(u32 align, u32 n);
u32 strdup(const char* s);                      // guest heap copy of a host string
u32 usable_size(u32 p);

}  // namespace g
