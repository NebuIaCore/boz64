#include <atomic>
// Guest address space: one 4 GB reservation, a page allocator for big blocks and a simple
// size-class heap for the guest's malloc/free.
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <sys/mman.h>
#endif
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>

#include "common.h"

bool g_trace = false;
bool g_precise = false;
void (*g_fatal_hook)() = nullptr;

void fatal(const char* fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    static std::atomic<bool> in_fatal{false};
    if (g_fatal_hook && !in_fatal.exchange(true)) {
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, "FATAL: %s", buf);
        g_fatal_hook();
    }
    __android_log_print(ANDROID_LOG_FATAL, LOG_TAG, "FATAL: %s", buf);
    abort();
}

namespace g {

u8* mem = nullptr;

namespace {
std::mutex page_lock;
std::map<u32, u32> free_ranges;  // start -> size, page aligned, non-adjacent
std::mutex data_lock;
u32 data_top = DATA_BASE;

u32 round_up(u32 v, u32 a) { return (v + a - 1) & ~(a - 1); }
}  // namespace

#ifdef _WIN32
// Windows has no overcommit: the 4 GB is reserved, and pages are committed the first time anything touches them
// (an access violation inside the reserved block commits that page and retries), which behaves like Android's
// MAP_NORESERVE mapping. The first 64 KB stays uncommitted so null pointers still fault.
namespace {
LONG CALLBACK commit_on_touch(EXCEPTION_POINTERS* ep) {
    if (ep->ExceptionRecord->ExceptionCode != EXCEPTION_ACCESS_VIOLATION || ep->ExceptionRecord->NumberParameters < 2)
        return EXCEPTION_CONTINUE_SEARCH;
    auto a = static_cast<uintptr_t>(ep->ExceptionRecord->ExceptionInformation[1]);
    auto base = reinterpret_cast<uintptr_t>(g::mem);
    if (!g::mem || a < base + g::NULL_GUARD || a >= base + 0x100000000ull + 0x10000) return EXCEPTION_CONTINUE_SEARCH;
    MEMORY_BASIC_INFORMATION mi;
    if (!VirtualQuery(reinterpret_cast<void*>(a), &mi, sizeof mi) || mi.State != MEM_RESERVE) return EXCEPTION_CONTINUE_SEARCH;
    // commit 64 KB at a time: fewer faults for sequential use
    uintptr_t lo = a & ~static_cast<uintptr_t>(0xffff);
    if (lo < base + g::NULL_GUARD) lo = base + g::NULL_GUARD;
    void* r = VirtualAlloc(reinterpret_cast<void*>(lo), 0x10000, MEM_COMMIT, PAGE_READWRITE);
    if (!r && !VirtualAlloc(reinterpret_cast<void*>(a & ~static_cast<uintptr_t>(0xfff)), 0x1000, MEM_COMMIT, PAGE_READWRITE))
        return EXCEPTION_CONTINUE_SEARCH;
    return EXCEPTION_CONTINUE_EXECUTION;
}
}  // namespace

void init_memory() {
    const size_t total = 0x100000000ull + 0x10000;
    void* m = VirtualAlloc(nullptr, total, MEM_RESERVE, PAGE_NOACCESS);
    if (!m) fatal("cannot reserve 4 GB for the 32-bit game (error %lu)", GetLastError());
    mem = static_cast<u8*>(m);
    AddVectoredExceptionHandler(1, commit_on_touch);
    // the host-provided areas are used from the start
    VirtualAlloc(mem + THUNK_BASE, DATA_END - THUNK_BASE, MEM_COMMIT, PAGE_READWRITE);
    VirtualAlloc(mem + KUSER_PAGE, 0x10000 + 0x10000, MEM_COMMIT, PAGE_READWRITE);
    free_ranges[ALLOC_START] = ALLOC_END - ALLOC_START;
    LOGI("guest memory at %p", mem);
}
#else
void init_memory() {
    // 4 GB plus a 64 KB tail so accesses that straddle the top of the space stay mapped.
    const size_t total = 0x100000000ull + 0x10000;
    void* m = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) fatal("cannot reserve 4 GB for the 32-bit game (errno %d)", errno);
    mem = static_cast<u8*>(m);
    mprotect(mem, NULL_GUARD, PROT_NONE);
    free_ranges[ALLOC_START] = ALLOC_END - ALLOC_START;
    LOGI("guest memory at %p", mem);
}
#endif

u32 page_alloc(u32 size, u32 align) {
    if (size == 0) size = PAGE;
    size = round_up(size, PAGE);
    if (align < PAGE) align = PAGE;
    std::lock_guard lk(page_lock);
    for (auto it = free_ranges.begin(); it != free_ranges.end(); ++it) {
        u32 start = it->first, len = it->second;
        u32 a = round_up(start, align);
        if (a < start || a - start + (u64)size > len) continue;
#ifdef _WIN32
        // Committed now, not on first touch: Windows itself (ReadFile, recv...) cannot write into pages that are
        // only reserved, and guest buffers are handed straight to it.
        if (!VirtualAlloc(mem + a, size, MEM_COMMIT, PAGE_READWRITE)) {
            LOGE("guest page_alloc(%u) failed: Windows has no memory left to commit (error %lu)", size, GetLastError());
            return 0;
        }
#endif
        free_ranges.erase(it);
        if (a > start) free_ranges[start] = a - start;
        u32 end = a + size, rend = start + len;
        if (end < rend) free_ranges[end] = rend - end;
        return a;
    }
    LOGE("guest page_alloc(%u) failed: out of guest memory", size);
    return 0;
}

void page_free(u32 addr, u32 size) {
    if (!addr || !size) return;
    size = round_up(size, PAGE);
#ifdef _WIN32
    VirtualFree(mem + addr, size, MEM_DECOMMIT);  // give the memory back; pages read as zero again
#else
    madvise(mem + addr, size, MADV_DONTNEED);  // give the memory back; pages read as zero again
#endif
    std::lock_guard lk(page_lock);
    u32 start = addr, end = addr + size;
    auto next = free_ranges.lower_bound(start);
    if (next != free_ranges.begin()) {
        auto prev = std::prev(next);
        if (prev->first + prev->second == start) {
            start = prev->first;
            free_ranges.erase(prev);
        }
    }
    next = free_ranges.lower_bound(end);
    if (next != free_ranges.end() && next->first == end) {
        end += next->second;
        free_ranges.erase(next);
    }
    free_ranges[start] = end - start;
}

u32 data_alloc(u32 size, u32 align) {
    std::lock_guard lk(data_lock);
    u32 a = round_up(data_top, align);
    if (a + size > DATA_END) fatal("guest data area full");
    data_top = a + size;
    return a;
}

u32 data_str(const char* s) {
    u32 n = strlen(s) + 1;
    u32 a = data_alloc(n, 1);
    memcpy(mem + a, s, n);
    return a;
}

// ---- heap ----
// Every block has an 8-byte header just before the user pointer: {u32 info, u32 magic}.
// Small blocks: info = size class index; they are carved from 256 KB slabs and recycled per class.
// Large blocks: info = total bytes of the page allocation.
namespace {
constexpr u32 MAGIC_SMALL = 0x5a110c01, MAGIC_LARGE = 0x5a110c02, MAGIC_ALIGNED = 0x5a110c03;
constexpr u32 class_sizes[] = {16, 24, 32, 48, 64, 96, 128, 192, 256, 384, 512, 768, 1024, 1536, 2048,
                               3072, 4096, 6144, 8192, 12288, 16384, 24576, 32768};
constexpr int NCLASS = sizeof(class_sizes) / sizeof(class_sizes[0]);
constexpr u32 SLAB = 256 * 1024;
std::mutex heap_lock;
u32 free_list[NCLASS];
u32 slab_cur = 0, slab_end = 0;

int class_for(u32 total) {
    for (int i = 0; i < NCLASS; i++)
        if (class_sizes[i] >= total) return i;
    return -1;
}
}  // namespace

u32 malloc(u32 n) {
    u32 total = n + 8;
    if (total < n) return 0;
    int c = class_for(total);
    if (c < 0) {
        u32 sz = round_up(total, PAGE);
        u32 base = page_alloc(sz);
        if (!base) return 0;
        wr32(base, sz);
        wr32(base + 4, MAGIC_LARGE);
        return base + 8;
    }
    std::lock_guard lk(heap_lock);
    u32 blk = free_list[c];
    if (blk) {
        free_list[c] = rd32(blk + 8);
    } else {
        u32 bs = class_sizes[c];
        if (slab_cur + bs > slab_end) {
            slab_cur = page_alloc(SLAB);
            if (!slab_cur) return 0;
            slab_end = slab_cur + SLAB;
        }
        blk = slab_cur;
        slab_cur += bs;
    }
    wr32(blk, c);
    wr32(blk + 4, MAGIC_SMALL);
    return blk + 8;
}

u32 usable_size(u32 p) {
    if (!p) return 0;
    u32 info = rd32(p - 8), magic = rd32(p - 4);
    if (magic == MAGIC_SMALL) return class_sizes[info] - 8;
    if (magic == MAGIC_LARGE) return info - 8;
    if (magic == MAGIC_ALIGNED) return info - PAGE;
    fatal("guest free/realloc of a bad pointer 0x%08x", p);
}

void free(u32 p) {
    if (!p) return;
    u32 info = rd32(p - 8), magic = rd32(p - 4);
    if (magic == MAGIC_SMALL) {
        if (info >= (u32)NCLASS) fatal("guest heap corrupt at 0x%08x", p);
        wr32(p - 4, 0);
        std::lock_guard lk(heap_lock);
        wr32(p, free_list[info]);
        free_list[info] = p - 8;
    } else if (magic == MAGIC_LARGE) {
        wr32(p - 4, 0);
        page_free(p - 8, info);
    } else if (magic == MAGIC_ALIGNED) {
        wr32(p - 4, 0);
        page_free(p - PAGE, info);
    } else {
        LOGE("guest free() of a bad pointer 0x%08x (ignored)", p);
    }
}

u32 realloc(u32 p, u32 n) {
    if (!p) return malloc(n);
    if (n == 0) {
        free(p);
        return 0;
    }
    u32 cap = usable_size(p);
    if (n <= cap) return p;
    u32 q = malloc(n);
    if (!q) return 0;
    memcpy(mem + q, mem + p, cap);
    free(p);
    return q;
}

u32 calloc(u32 n, u32 sz) {
    u64 total = (u64)n * sz;
    if (total > 0xF0000000u) return 0;
    u32 p = malloc((u32)total);
    if (p) memset(mem + p, 0, (u32)total);
    return p;
}

u32 memalign(u32 align, u32 n) {
    if (align <= 8) return malloc(n);
    // Layout: [header page][body], with the body aligned to `a`. The header sits at the end of
    // the page in front of the body.
    u32 a = align < PAGE ? PAGE : align;
    u32 body = round_up(n ? n : 1, PAGE);
    u32 base = page_alloc(body + a, a);
    if (!base) return 0;
    u32 user = base + a;
    if (a > PAGE) page_free(base, a - PAGE);  // only one page in front is needed
    wr32(user - 8, body + PAGE);
    wr32(user - 4, MAGIC_ALIGNED);
    return user;
}

u32 strdup(const char* s) {
    u32 n = strlen(s) + 1;
    u32 p = malloc(n);
    if (p) memcpy(mem + p, s, n);
    return p;
}

}  // namespace g
