// C library, libdl, liblog and zlib for guest code.
#include <arpa/inet.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/utsname.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <zlib.h>

#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <locale.h>
#include <map>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>

#include "cpu.h"
#include "elf.h"
#include "vfs.h"
#include "frame.h"
#include "lan.h"
#include "patches.h"

void register_stdio();
std::string fake_cpuinfo();
extern std::string g_libdir;  // folder holding our .so and the original 32-bit libraries

namespace {

#ifdef BOZ_PC
// The Linux and other Unix PC builds: the game's Android paths live under the PC build's data folder
// (pc/src/vfs_unix.cpp), and its 32-bit ARM Linux open() flags are rebuilt from the host's values.
std::string HP(const char* p) { return boz_host_path(p); }
int host_open_flags(int g) {
    int h = g & 3;  // O_RDONLY / O_WRONLY / O_RDWR
    const struct { int guest, host; } F[] = {
        {0100, O_CREAT}, {0200, O_EXCL}, {0400, O_NOCTTY}, {01000, O_TRUNC}, {02000, O_APPEND},
        {04000, O_NONBLOCK}, {040000, O_DIRECTORY}, {0100000, O_NOFOLLOW}, {02000000, O_CLOEXEC}};
    for (const auto& f : F)
        if (g & f.guest) h |= f.host;
    return h;
}
// Files the game reads from /proc: the PC build's stand-ins, as a file descriptor.
int fake_proc_fd(const std::string& text) {
    FILE* f = tmpfile();
    if (!f) return -1;
    fwrite(text.data(), 1, text.size(), f);
    fflush(f);
    int fd = dup(fileno(f));
    fclose(f);
    if (fd >= 0) lseek(fd, 0, SEEK_SET);
    return fd;
}
#else
std::string HP(const char* p) { return p; }
#endif

#ifdef __GLIBC__  // nested: __GLIBC_PREREQ doesn't exist elsewhere, and an #if can't name it (Android)
#if !__GLIBC_PREREQ(2, 36)
// glibc only has arc4random since 2.36 (a Linux PC build may run on an older one)
u32 arc4random() {
    static thread_local std::mt19937 r{std::random_device{}()};
    return static_cast<u32>(r());
}
#endif
#endif

// ---- struct conversions (32-bit bionic layouts) ----

void put_timespec(u32 a, const timespec& ts) {
    g::wr32(a, static_cast<u32>(ts.tv_sec));
    g::wr32(a + 4, static_cast<u32>(ts.tv_nsec));
}
timespec get_timespec(u32 a) {
    return timespec{static_cast<s32>(g::rd32(a)), static_cast<s32>(g::rd32(a + 4))};
}

void put_stat(u32 a, const struct stat& s) {
    memset(g::mem + a, 0, 104);
    g::wr64(a + 0, s.st_dev);
    g::wr32(a + 12, static_cast<u32>(s.st_ino));
    g::wr32(a + 16, s.st_mode);
    g::wr32(a + 20, s.st_nlink);
    g::wr32(a + 24, s.st_uid);
    g::wr32(a + 28, s.st_gid);
    g::wr64(a + 32, s.st_rdev);
    g::wr64(a + 48, s.st_size);
    g::wr32(a + 56, s.st_blksize);
    g::wr64(a + 64, s.st_blocks);
    put_timespec(a + 72, s.st_atim);
    put_timespec(a + 80, s.st_mtim);
    put_timespec(a + 88, s.st_ctim);
    g::wr64(a + 96, s.st_ino);
}

void put_statfs(u32 a, const struct statfs& s) {
    memset(g::mem + a, 0, 84);
    g::wr32(a + 0, static_cast<u32>(s.f_type));
    g::wr32(a + 4, static_cast<u32>(s.f_bsize));
    g::wr64(a + 8, s.f_blocks);
    g::wr64(a + 16, s.f_bfree);
    g::wr64(a + 24, s.f_bavail);
    g::wr64(a + 32, s.f_files);
    g::wr64(a + 40, s.f_ffree);
    memcpy(g::mem + a + 48, &s.f_fsid, 8);
    g::wr32(a + 56, static_cast<u32>(s.f_namelen));
    g::wr32(a + 60, static_cast<u32>(s.f_frsize));
    g::wr32(a + 64, static_cast<u32>(s.f_flags));
}

std::mutex zone_lock;
std::unordered_map<std::string, u32> zone_names;
u32 zone_str(const char* z) {
    if (!z) return 0;
    std::lock_guard lk(zone_lock);
    auto it = zone_names.find(z);
    if (it != zone_names.end()) return it->second;
    return zone_names[z] = g::data_str(z);
}

// struct tm: 9 ints, long tm_gmtoff, const char* tm_zone  (44 bytes)
void put_tm(u32 a, const struct tm& tm) {
    const int v[9] = {tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon, tm.tm_year, tm.tm_wday, tm.tm_yday, tm.tm_isdst};
    for (int i = 0; i < 9; i++) g::wr32(a + 4 * i, static_cast<u32>(v[i]));
    g::wr32(a + 36, static_cast<u32>(tm.tm_gmtoff));
    g::wr32(a + 40, zone_str(tm.tm_zone));
}
struct tm get_tm(u32 a) {
    struct tm tm {};
    int* f[9] = {&tm.tm_sec, &tm.tm_min, &tm.tm_hour, &tm.tm_mday, &tm.tm_mon, &tm.tm_year, &tm.tm_wday, &tm.tm_yday, &tm.tm_isdst};
    for (int i = 0; i < 9; i++) *f[i] = static_cast<s32>(g::rd32(a + 4 * i));
    tm.tm_gmtoff = static_cast<s32>(g::rd32(a + 36));
    return tm;
}

u32 daylight_addr = 0, tzname_addr = 0;
void sync_tz() {
    tzset();
    g::wr32(daylight_addr, daylight);
    g::wr32(tzname_addr, zone_str(tzname[0]));
    g::wr32(tzname_addr + 4, zone_str(tzname[1]));
}

// ---- pthreads ----
// Guest mutexes and condition variables are 4-byte words. On first use each one gets a host
// object and its word is replaced by a tag + index into these tables.
constexpr u32 TAG_MASK = 0xF0000000, MUTEX_TAG = 0xB0000000, COND_TAG = 0xC0000000;
constexpr u32 MAX_SYNC = 1 << 18;
std::atomic<pthread_mutex_t*> mutex_tab[MAX_SYNC];
std::atomic<pthread_cond_t*> cond_tab[MAX_SYNC];
std::atomic<u32> next_mutex{1}, next_cond{1};

pthread_mutex_t* make_mutex(int type) {
    auto* m = new pthread_mutex_t;
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, type);
    pthread_mutex_init(m, &a);
    pthread_mutexattr_destroy(&a);
    return m;
}

pthread_mutex_t* get_mutex(u32 gm, int type_if_new = -1) {
    auto* word = reinterpret_cast<std::atomic<u32>*>(g::mem + gm);
    for (;;) {
        u32 v = word->load();
        if ((v & TAG_MASK) == MUTEX_TAG) return mutex_tab[v & ~TAG_MASK].load();
        int type = type_if_new;
        if (type < 0) {  // static initialisers of old bionic: 0 normal, 0x4000 recursive, 0x8000 error-checking
            type = (v & 0xC000) == 0x4000 ? PTHREAD_MUTEX_RECURSIVE
                   : (v & 0xC000) == 0x8000 ? PTHREAD_MUTEX_ERRORCHECK
                                            : PTHREAD_MUTEX_NORMAL;
        }
        u32 idx = next_mutex++;
        if (idx >= MAX_SYNC) fatal("too many guest mutexes");
        mutex_tab[idx] = make_mutex(type);
        if (word->compare_exchange_strong(v, MUTEX_TAG | idx)) return mutex_tab[idx].load();
        pthread_mutex_destroy(mutex_tab[idx]);  // another thread got there first
        delete mutex_tab[idx].exchange(nullptr);
    }
}

pthread_cond_t* get_cond(u32 gc) {
    auto* word = reinterpret_cast<std::atomic<u32>*>(g::mem + gc);
    for (;;) {
        u32 v = word->load();
        if ((v & TAG_MASK) == COND_TAG) return cond_tab[v & ~TAG_MASK].load();
        u32 idx = next_cond++;
        if (idx >= MAX_SYNC) fatal("too many guest condition variables");
        auto* c = new pthread_cond_t;
        pthread_cond_init(c, nullptr);
        cond_tab[idx] = c;
        if (word->compare_exchange_strong(v, COND_TAG | idx)) return c;
        pthread_cond_destroy(c);
        delete cond_tab[idx].exchange(nullptr);
    }
}

struct KeySlot {
    bool used = false;
    u32 dtor = 0;
};
std::mutex key_lock;
KeySlot key_slots[128];

struct JoinState {
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    u32 value = 0;
    bool detached = false;
};
std::mutex join_lock;
std::unordered_map<u32, std::shared_ptr<JoinState>> joins;

struct GuestThreadExit {
    u32 value;
};

void run_key_destructors(GuestThread& t) {
    for (int round = 0; round < 4; round++) {
        bool any = false;
        for (int k = 0; k < 128; k++) {
            u32 dtor, val = g::rd32(t.key_addr(k));
            {
                std::lock_guard lk(key_lock);
                dtor = key_slots[k].used ? key_slots[k].dtor : 0;
            }
            if (dtor && val) {
                g::wr32(t.key_addr(k), 0);
                call_guest(dtor, {val});
                any = true;
            }
        }
        if (!any) break;
    }
}

struct StartInfo {
    GuestThread* t;
    u32 fn, arg;
    std::shared_ptr<JoinState> js;
};

void* thread_entry(void* p) {
    auto* si = static_cast<StartInfo*>(p);
    bind_guest_thread(si->t);
    u32 value = 0;
    try {
        value = static_cast<u32>(call_guest(si->fn, {si->arg}));
    } catch (const GuestThreadExit& e) {
        value = e.value;
    }
    run_key_destructors(*si->t);
    end_guest_thread();
    {
        std::lock_guard lk(si->js->m);
        si->js->done = true;
        si->js->value = value;
    }
    si->js->cv.notify_all();
    delete si;
    return nullptr;
}

std::recursive_mutex once_lock;

// ---- setjmp / longjmp ----
// Our jmp_buf layout (the guest's buffer is 256 bytes): r4-r11, sp, lr, then d8-d15.
void do_setjmp(GuestThread& t) {
    u32 b = arg(t, 0);
    auto& r = t.regs();
    for (int i = 0; i < 8; i++) g::wr32(b + 4 * i, r[4 + i]);
    g::wr32(b + 32, r[13]);
    g::wr32(b + 36, r[14]);
    auto& x = t.ext_regs();
    for (int i = 0; i < 16; i++) g::wr32(b + 40 + 4 * i, x[16 + i]);
    ret(t, 0);
}
void do_longjmp(GuestThread& t) {
    u32 b = arg(t, 0), val = arg(t, 1);
    auto& r = t.regs();
    for (int i = 0; i < 8; i++) r[4 + i] = g::rd32(b + 4 * i);
    r[13] = g::rd32(b + 32);
    r[14] = g::rd32(b + 36);  // the stub's "bx lr" then returns from setjmp again
    auto& x = t.ext_regs();
    for (int i = 0; i < 16; i++) x[16 + i] = g::rd32(b + 40 + 4 * i);
    ret(t, val ? val : 1);
}

// ---- dlopen ----
constexpr u32 H_GLOBAL = 0x10, H_HOSTLIB = 0x20;
thread_local std::string dl_error;

std::string base_name(const char* path) {
    const char* s = strrchr(path, '/');
    return s ? s + 1 : path;
}

bool is_host_lib(const std::string& n) {
    static const char* hostlibs[] = {"libc.so", "libm.so", "libdl.so", "liblog.so", "libz.so", "libGLESv1_CM.so",
                                     "libGLESv2.so", "libGLESv3.so", "libEGL.so", "libstdc++.so", "libandroid.so"};
    for (auto* h : hostlibs)
        if (n == h) return true;
    return false;
}

// ---- zlib ----
// zlib's z_stream holds pointers, so each guest stream (56 bytes in 32-bit) gets a host twin.
std::mutex z_lock;
std::unordered_map<u32, z_stream*> zstreams;

z_stream* zs_in(u32 gs) {
    std::lock_guard lk(z_lock);
    auto it = zstreams.find(gs);
    if (it == zstreams.end()) return nullptr;
    z_stream* z = it->second;
    z->next_in = static_cast<Bytef*>(g::h(g::rd32(gs + 0)));
    z->avail_in = g::rd32(gs + 4);
    z->next_out = static_cast<Bytef*>(g::h(g::rd32(gs + 12)));
    z->avail_out = g::rd32(gs + 16);
    return z;
}
void zs_out(u32 gs, z_stream* z) {
    g::wr32(gs + 0, z->next_in ? g::to_guest(z->next_in) : 0);
    g::wr32(gs + 4, z->avail_in);
    g::wr32(gs + 8, static_cast<u32>(z->total_in));
    g::wr32(gs + 12, z->next_out ? g::to_guest(z->next_out) : 0);
    g::wr32(gs + 16, z->avail_out);
    g::wr32(gs + 20, static_cast<u32>(z->total_out));
    g::wr32(gs + 24, z->msg ? zone_str(z->msg) : 0);
    g::wr32(gs + 44, z->data_type);
    g::wr32(gs + 48, static_cast<u32>(z->adler));
}
int z_init(u32 gs, int window_bits, bool with_bits) {
    auto* z = new z_stream{};
    z->next_in = static_cast<Bytef*>(g::h(g::rd32(gs + 0)));
    z->avail_in = g::rd32(gs + 4);
    int r = with_bits ? inflateInit2(z, window_bits) : inflateInit(z);
    if (r != Z_OK) {
        delete z;
        return r;
    }
    {
        std::lock_guard lk(z_lock);
        zstreams[gs] = z;
    }
    g::wr32(gs + 28, 0x5a5a5a5a);  // non-null "state" for code that checks it
    zs_out(gs, z);
    return r;
}

// ---- getaddrinfo ----
// struct addrinfo (32 bytes): flags, family, socktype, protocol, addrlen, canonname*, addr*, next*
u32 put_addrinfo(const addrinfo* ai) {
    u32 head = 0, prev = 0;
    for (; ai; ai = ai->ai_next) {
        u32 n = g::calloc(1, 32 + ai->ai_addrlen);
        g::wr32(n + 0, ai->ai_flags);
        g::wr32(n + 4, ai->ai_family);
        g::wr32(n + 8, ai->ai_socktype);
        g::wr32(n + 12, ai->ai_protocol);
        g::wr32(n + 16, ai->ai_addrlen);
        g::wr32(n + 20, ai->ai_canonname ? g::strdup(ai->ai_canonname) : 0);
        if (ai->ai_addr) {
            memcpy(g::mem + n + 32, ai->ai_addr, ai->ai_addrlen);
            g::wr32(n + 24, n + 32);
        }
        if (prev)
            g::wr32(prev + 28, n);
        else
            head = n;
        prev = n;
    }
    return head;
}

std::mutex env_lock;
std::unordered_map<std::string, std::pair<std::string, u32>> env_cache;

u32 scratch_str(const char* s) {
    GuestThread& t = self();
    u32 b = guest_scratch(t, 4096);
    strncpy(static_cast<char*>(g::h(b)), s ? s : "", 4095);
    g::wr8(b + 4095, 0);
    return b;
}
}  // namespace

std::string g_libdir;

// True if the file is a 32-bit ARM ELF (a library the guest can load).
static bool is_guest_elf(const std::string& path) {
    FILE* f = fopen(path.c_str(), "rb");
    if (!f) return false;
    unsigned char h[20] = {};
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    return n == sizeof h && memcmp(h, "" "ELF", 4) == 0 && h[4] == 1 && h[18] == 40;
}

void register_libc() {
    register_stdio();

    // memory
    reg("malloc", +[](u32 n) -> u32 { return g::malloc(n); });
    reg("free", +[](u32 p) { g::free(p); });
    reg("realloc", +[](u32 p, u32 n) -> u32 { return g::realloc(p, n); });
    reg("calloc", +[](u32 n, u32 s) -> u32 { return g::calloc(n, s); });
    reg("valloc", +[](u32 n) -> u32 { return g::memalign(g::PAGE, n); });
    reg("memalign", +[](u32 a, u32 n) -> u32 { return g::memalign(a, n); });
    reg("posix_memalign", +[](u32 out, u32 a, u32 n) -> int {
        u32 p = g::memalign(a, n);
        if (!p) return ENOMEM;
        g::wr32(out, p);
        return 0;
    });
    reg("malloc_usable_size", +[](u32 p) -> u32 { return g::usable_size(p); });

    // strings
    reg("memcmp", +[](const void* a, const void* b, u32 n) -> int { return memcmp(a, b, n); });
    reg("memcpy", +[](void* d, const void* s, u32 n) -> void* { return memcpy(d, s, n); });
    reg("memmove", +[](void* d, const void* s, u32 n) -> void* { return memmove(d, s, n); });
    reg("memset", +[](void* d, int c, u32 n) -> void* { return memset(d, c, n); });
    reg("memchr", +[](void* s, int c, u32 n) -> void* { return memchr(s, c, n); });
    reg("strlen", +[](const char* s) -> u32 { return strlen(s); });
    reg("strcmp", +[](const char* a, const char* b) -> int { return strcmp(a, b); });
    reg("strncmp", +[](const char* a, const char* b, u32 n) -> int { return strncmp(a, b, n); });
    reg("strcasecmp", +[](const char* a, const char* b) -> int { return strcasecmp(a, b); });
    reg("strncasecmp", +[](const char* a, const char* b, u32 n) -> int { return strncasecmp(a, b, n); });
    reg("strcpy", +[](char* d, const char* s) -> char* { return strcpy(d, s); });
    reg("strncpy", +[](char* d, const char* s, u32 n) -> char* { return strncpy(d, s, n); });
    reg("strcat", +[](char* d, const char* s) -> char* { return strcat(d, s); });
    reg("strncat", +[](char* d, const char* s, u32 n) -> char* { return strncat(d, s, n); });
    reg("strchr", +[](char* s, int c) -> char* { return strchr(s, c); });
    reg("strrchr", +[](char* s, int c) -> char* { return strrchr(s, c); });
    reg("strstr", +[](char* h, const char* n) -> char* { return strstr(h, n); });
    reg("strpbrk", +[](char* s, const char* a) -> char* { return strpbrk(s, a); });
    reg("strspn", +[](const char* s, const char* a) -> u32 { return strspn(s, a); });
    reg("strcspn", +[](const char* s, const char* a) -> u32 { return strcspn(s, a); });
    reg("strtok", +[](char* s, const char* d) -> char* { return strtok(s, d); });
    reg("strdup", +[](const char* s) -> u32 { return g::strdup(s); });
    reg("strerror", +[](int e) -> u32 { return scratch_str(strerror(e)); });
    reg("tolower", +[](int c) -> int { return tolower(c); });
    reg("toupper", +[](int c) -> int { return toupper(c); });
    reg("isxdigit", +[](int c) -> int { return isxdigit(c); });
    reg("isdigit", +[](int c) -> int { return isdigit(c); });
    reg("isspace", +[](int c) -> int { return isspace(c); });
    reg("isalpha", +[](int c) -> int { return isalpha(c); });
    reg("isalnum", +[](int c) -> int { return isalnum(c); });
    reg("isupper", +[](int c) -> int { return isupper(c); });
    reg("islower", +[](int c) -> int { return islower(c); });
    reg("isprint", +[](int c) -> int { return isprint(c); });
    reg("atoi", +[](const char* s) -> int { return atoi(s); });
    reg("atol", +[](const char* s) -> s32 { return static_cast<s32>(atol(s)); });
    reg("atof", +[](const char* s) -> double { return atof(s); });
    reg_raw("strtol", [](GuestThread& t) {
        const char* s = g::str(arg(t, 0));
        char* end = nullptr;
        long long v = strtoll(s, &end, static_cast<int>(arg(t, 2)));
        if (v > INT_MAX) { v = INT_MAX; errno = ERANGE; }
        if (v < INT_MIN) { v = INT_MIN; errno = ERANGE; }
        if (arg(t, 1)) g::wr32(arg(t, 1), g::to_guest(end));
        ret(t, static_cast<u32>(static_cast<s32>(v)));
    });
    reg_raw("strtoul", [](GuestThread& t) {
        const char* s = g::str(arg(t, 0));
        char* end = nullptr;
        unsigned long long v = strtoull(s, &end, static_cast<int>(arg(t, 2)));
        if (v > UINT_MAX && !(v >> 63)) { v = UINT_MAX; errno = ERANGE; }
        if (arg(t, 1)) g::wr32(arg(t, 1), g::to_guest(end));
        ret(t, static_cast<u32>(v));
    });
    reg_raw("strtod", [](GuestThread& t) {
        const char* s = g::str(arg(t, 0));
        char* end = nullptr;
        double v = strtod(s, &end);
        if (arg(t, 1)) g::wr32(arg(t, 1), g::to_guest(end));
        ret64(t, std::bit_cast<u64>(v));
    });

    // files and folders
    reg("open", +[](const char* path, int flags, u32 mode) -> int {
#ifdef BOZ_PC
        if (strcmp(path, "/proc/cpuinfo") == 0) {
            static std::string info = fake_cpuinfo();
            return fake_proc_fd(info);
        }
        if (strncmp(path, "/proc/", 6) == 0) {
            std::string c = boz_fake_proc(path);
            if (!c.empty()) return fake_proc_fd(c);
        }
        flags = host_open_flags(flags);
#else
        if (strcmp(path, "/proc/cpuinfo") == 0) {
            static std::string info = fake_cpuinfo();
            int fd = static_cast<int>(syscall(__NR_memfd_create, "cpuinfo", 0));
            if (fd >= 0) {
                write(fd, info.data(), info.size());
                lseek(fd, 0, SEEK_SET);
                return fd;
            }
        }
#endif
        if ((flags & O_ACCMODE) == O_RDONLY) {
            if (const Bundled* b = vfs_lookup(path)) return vfs_open(b);
        }
        return open(HP(path).c_str(), flags, mode);
    });
    reg("close", +[](int fd) -> int {
        vfs_close(fd);
        return close(fd);
    });
    reg("read", +[](int fd, void* b, u32 n) -> s32 {
        s64 lim = vfs_read_limit(fd, n);
        if (lim != -2) n = static_cast<u32>(lim);
        return static_cast<s32>(read(fd, b, n));
    });
    reg("write", +[](int fd, const void* b, u32 n) -> s32 { return static_cast<s32>(write(fd, b, n)); });
    reg("lseek", +[](int fd, s32 off, int whence) -> s32 {
        s64 r = vfs_lseek(fd, off, whence);
        return static_cast<s32>(r != -2 ? r : lseek(fd, off, whence));
    });
    reg("fcntl", +[](int fd, int cmd, u32 a) -> int { return fcntl(fd, cmd, a); });
    reg("ftruncate", +[](int fd, s32 len) -> int { return ftruncate(fd, len); });
    reg("access", +[](const char* p, int m) -> int {
        if (vfs_lookup(p)) {
            if (!(m & W_OK)) return 0;
            errno = EACCES;
            return -1;
        }
        return access(HP(p).c_str(), m);
    });
    reg("chmod", +[](const char* p, u32 m) -> int { return chmod(HP(p).c_str(), m); });
    reg("mkdir", +[](const char* p, u32 m) -> int { return mkdir(HP(p).c_str(), m); });
    reg("rmdir", +[](const char* p) -> int { return rmdir(HP(p).c_str()); });
    reg("remove", +[](const char* p) -> int { return remove(HP(p).c_str()); });
    reg("unlink", +[](const char* p) -> int { return unlink(HP(p).c_str()); });
    reg("rename", +[](const char* a, const char* b) -> int { return rename(HP(a).c_str(), HP(b).c_str()); });
    reg("stat", +[](const char* p, u32 out) -> int {
        struct stat s;
        if (const Bundled* b = vfs_lookup(p)) {
            vfs_stat(b, &s);
            put_stat(out, s);
            return 0;
        }
        int r = stat(HP(p).c_str(), &s);
        if (r == 0) put_stat(out, s);
        return r;
    });
    reg("lstat", +[](const char* p, u32 out) -> int {
        struct stat s;
        if (const Bundled* b = vfs_lookup(p)) {
            vfs_stat(b, &s);
            put_stat(out, s);
            return 0;
        }
        int r = lstat(HP(p).c_str(), &s);
        if (r == 0) put_stat(out, s);
        return r;
    });
    reg("fstat", +[](int fd, u32 out) -> int {
        struct stat s;
        int r = fstat(fd, &s);
        if (r == 0) {
            vfs_fix_fstat(fd, &s);
            put_stat(out, s);
        }
        return r;
    });
    reg("statfs", +[](const char* p, u32 out) -> int {
        struct statfs s;
        int r = statfs(HP(p).c_str(), &s);
        if (r == 0) put_statfs(out, s);
        return r;
    });
    {
        // DIR handles: a guest buffer that also holds the current struct dirent (same layout on both).
        static std::mutex dir_lock;
        static std::unordered_map<u32, DIR*> dirs;
        reg("opendir", +[](const char* p) -> u32 {
            DIR* d = opendir(HP(p).c_str());
            if (!d) return 0;
            u32 gd = g::calloc(1, sizeof(struct dirent));
            std::lock_guard lk(dir_lock);
            dirs[gd] = d;
            return gd;
        });
        reg("readdir", +[](u32 gd) -> u32 {
            DIR* d;
            {
                std::lock_guard lk(dir_lock);
                auto it = dirs.find(gd);
                if (it == dirs.end()) return 0;
                d = it->second;
            }
            struct dirent* e = readdir(d);
            if (!e) return 0;
            memcpy(g::mem + gd, e, sizeof(struct dirent));
            return gd;
        });
        reg("closedir", +[](u32 gd) -> int {
            DIR* d;
            {
                std::lock_guard lk(dir_lock);
                auto it = dirs.find(gd);
                if (it == dirs.end()) return -1;
                d = it->second;
                dirs.erase(it);
            }
            g::free(gd);
            return closedir(d);
        });
    }
    reg("mmap", +[](u32 addr, u32 len, int prot, int flags, int fd, s32 off) -> u32 {
        u32 a = (flags & MAP_FIXED) ? addr : g::page_alloc(len);
        if (!a) {
            errno = ENOMEM;
            return 0xffffffff;
        }
        if (flags & MAP_FIXED) memset(g::mem + a, 0, len);
        if (!(flags & MAP_ANONYMOUS) && fd >= 0) {
            if (vfs_pread(fd, g::mem + a, len, off) == -2) pread(fd, g::mem + a, len, off);
        }
        if (prot & PROT_EXEC) {
            invalidate_code(a, len);
            if (flags & MAP_ANONYMOUS) patches_note_exec_region(a, len);
        }
        return a;
    });
    reg("munmap", +[](u32 a, u32 len) -> int {
        if (a >= g::ALLOC_START && a + len <= g::ALLOC_END) g::page_free(a, len);
        invalidate_code(a, len);
        return 0;
    });
    reg("mprotect", +[](u32 a, u32 len, int prot) -> int {
        if (prot & PROT_EXEC) invalidate_code(a, len);
        return 0;
    });

    // time
    reg("time", +[](u32 out) -> s32 {
        s32 now = static_cast<s32>(time(nullptr));
        if (out) g::wr32(out, now);
        return now;
    });
    reg("gettimeofday", +[](u32 tv, u32 tz) -> int {
        timeval v;
        int r = gettimeofday(&v, nullptr);
        if (tv) {
            g::wr32(tv, static_cast<u32>(v.tv_sec));
            g::wr32(tv + 4, static_cast<u32>(v.tv_usec));
        }
        if (tz) g::wr64(tz, 0);
        return r;
    });
    reg("clock_gettime", +[](int clk, u32 out) -> int {
        timespec ts;
        int r = clock_gettime(clk, &ts);
        if (r == 0 && out) put_timespec(out, ts);
        return r;
    });
    reg("clock_getres", +[](int clk, u32 out) -> int {
        timespec ts;
        int r = clock_getres(clk, &ts);
        if (r == 0 && out) put_timespec(out, ts);
        return r;
    });
    reg("gmtime_r", +[](u32 tp, u32 out) -> u32 {
        time_t tt = static_cast<s32>(g::rd32(tp));
        struct tm tm;
        if (!gmtime_r(&tt, &tm)) return 0;
        put_tm(out, tm);
        return out;
    });
    reg("localtime_r", +[](u32 tp, u32 out) -> u32 {
        time_t tt = static_cast<s32>(g::rd32(tp));
        struct tm tm;
        if (!localtime_r(&tt, &tm)) return 0;
        put_tm(out, tm);
        return out;
    });
    reg("mktime", +[](u32 tp) -> s32 {
        struct tm tm = get_tm(tp);
        time_t r = mktime(&tm);
        put_tm(tp, tm);
        return static_cast<s32>(r);
    });
    daylight_addr = g::data_alloc(4);
    tzname_addr = g::data_alloc(8);
    add_data_symbol("daylight", daylight_addr);
    add_data_symbol("tzname", tzname_addr);
    sync_tz();
    reg("tzset", +[]() { sync_tz(); });
    reg("usleep", +[](u32 us) -> int {
        frame_sleep_begin();
        int r = usleep(us);
        frame_sleep_end(us);
        return r;
    });
    reg("sleep", +[](u32 s) -> u32 { return sleep(s); });
    reg("nice", +[](int n) -> int { return nice(n); });
    reg("sched_yield", +[]() -> int { return sched_yield(); });

    // process
    reg("getpid", +[]() -> int { return getpid(); });
    reg("gettid", +[]() -> int { return gettid(); });
    reg("getuid", +[]() -> u32 { return getuid(); });
    reg_raw("abort", [](GuestThread& t) { fatal("guest called abort() from %s", elf_describe(t.regs()[14]).c_str()); });
    reg("exit", +[](int code) {
        LOGI("guest called exit(%d)", code);
        exit(code);
    });
    reg("_exit", +[](int code) { _exit(code); });
    reg("getenv", +[](const char* name) -> u32 {
        const char* v = getenv(name);
        if (!v) return 0;
        std::lock_guard lk(env_lock);
        auto& e = env_cache[name];
        if (!e.second || e.first != v) {
            e.first = v;
            e.second = g::data_str(v);
        }
        return e.second;
    });
    reg("uname", +[](struct utsname* u) -> int {
        int r = uname(u);
        strcpy(u->machine, "armv8l");  // what a 32-bit app sees on a 64-bit kernel
        return r;
    });
    reg("gethostname", +[](char* b, u32 n) -> int { return gethostname(b, n); });
    reg("raise", +[](int sig) -> int {
        LOGW("guest raise(%d) ignored", sig);
        return 0;
    });
    reg("bsd_signal", +[](int, u32) -> u32 { return 0; });
    reg("signal", +[](int, u32) -> u32 { return 0; });
    reg("sigaction", +[](int, u32, u32 old) -> int {
        if (old) memset(g::mem + old, 0, 16);
        return 0;
    });
    reg("setlocale", +[](int, u32) -> u32 {
        static u32 c = g::data_str("C");
        return c;
    });
    reg("arc4random", +[]() -> u32 { return arc4random(); });
    reg("lrand48", +[]() -> s32 { return static_cast<s32>(lrand48()); });
    reg("srand48", +[](s32 s) { srand48(s); });
    reg("rand", +[]() -> int { return rand(); });
    reg("srand", +[](u32 s) { srand(s); });
    reg_raw("__errno", [](GuestThread& t) { ret(t, t.errno_addr); });
    reg("__cxa_atexit", +[](u32, u32, u32) -> int { return 0; });
    reg("__cxa_finalize", +[](u32) {});
    reg("atexit", +[](u32) -> int { return 0; });
    reg("__gnu_Unwind_Find_exidx", +[](u32 pc, u32 pcount) -> u32 {
        GuestLib* l = elf_find_addr(pc);
        if (!l || !l->exidx) {
            if (pcount) g::wr32(pcount, 0);
            return 0;
        }
        if (pcount) g::wr32(pcount, l->exidx_count);
        return l->exidx;
    });
    reg("__android_log_write", +[](int prio, const char* tag, const char* text) -> int {
        return __android_log_write(prio, tag ? tag : "", text ? text : "");
    });
    reg("__google_potentially_blocking_region_begin", +[]() {});
    reg("__google_potentially_blocking_region_end", +[]() {});
    reg_raw("setjmp", do_setjmp);
    reg_raw("_setjmp", do_setjmp);
    reg_raw("sigsetjmp", do_setjmp);
    reg_raw("longjmp", do_longjmp);
    reg_raw("_longjmp", do_longjmp);
    reg_raw("siglongjmp", do_longjmp);

    // threads
    reg("pthread_attr_init", +[](u32 a) -> int {
        memset(g::mem + a, 0, 24);
        return 0;
    });
    reg("pthread_attr_destroy", +[](u32) -> int { return 0; });
    reg("pthread_attr_setstack", +[](u32 a, u32 base, u32 size) -> int {
        g::wr32(a + 4, base);
        g::wr32(a + 8, size);
        return 0;
    });
    reg("pthread_attr_setstacksize", +[](u32 a, u32 size) -> int {
        g::wr32(a + 8, size);
        return 0;
    });
    reg("pthread_attr_setdetachstate", +[](u32 a, int s) -> int {
        g::wr32(a, s);
        return 0;
    });
    reg("pthread_create", +[](u32 out, u32 attr, u32 fn, u32 parg) -> int {
        u32 sbase = attr ? g::rd32(attr + 4) : 0, ssize = attr ? g::rd32(attr + 8) : 0;
        if (!sbase) ssize = 0;  // only a size was given: we pick the stack (bigger is fine)
        GuestThread* nt = create_guest_thread(sbase, ssize);
        auto js = std::make_shared<JoinState>();
        {
            std::lock_guard lk(join_lock);
            joins[nt->id] = js;
        }
        if (out) g::wr32(out, nt->id);
        auto* si = new StartInfo{nt, fn, parg, js};
        pthread_attr_t ha;
        pthread_attr_init(&ha);
        pthread_attr_setstacksize(&ha, 1 << 20);
        pthread_attr_setdetachstate(&ha, PTHREAD_CREATE_DETACHED);
        pthread_t ht;
        int r = pthread_create(&ht, &ha, thread_entry, si);
        pthread_attr_destroy(&ha);
        return r;
    });
    reg("pthread_join", +[](u32 id, u32 out) -> int {
        std::shared_ptr<JoinState> js;
        {
            std::lock_guard lk(join_lock);
            auto it = joins.find(id);
            if (it == joins.end()) return ESRCH;
            js = it->second;
        }
        std::unique_lock lk(js->m);
        js->cv.wait(lk, [&] { return js->done; });
        if (out) g::wr32(out, js->value);
        std::lock_guard jl(join_lock);
        joins.erase(id);
        return 0;
    });
    reg("pthread_detach", +[](u32 id) -> int {
        std::lock_guard lk(join_lock);
        joins.erase(id);
        return 0;
    });
    reg("pthread_exit", +[](u32 value) { throw GuestThreadExit{value}; });  // caught in thread_entry
    reg_raw("pthread_self", [](GuestThread& t) { ret(t, t.id); });
    reg("pthread_equal", +[](u32 a, u32 b) -> int { return a == b; });
    reg("pthread_key_create", +[](u32 out, u32 dtor) -> int {
        std::lock_guard lk(key_lock);
        for (int k = 0; k < 128; k++) {
            if (!key_slots[k].used) {
                key_slots[k] = {true, dtor};
                g::wr32(out, k);
                return 0;
            }
        }
        return EAGAIN;
    });
    reg("pthread_key_delete", +[](u32 k) -> int {
        if (k >= 128) return EINVAL;
        std::lock_guard lk(key_lock);
        key_slots[k] = {};
        return 0;
    });
    {
        // pthread_getspecific(key): the game calls it ~20000 times a frame, so it is guest code (ARM):
        //   cmp r0, #127; movhi r0, #0; bxhi lr; mrc p15, 0, r1, c13, c0, 3 (the TLS block);
        //   add r1, r1, r0, lsl #2; ldr r0, [r1, #KEYS_OFF]; bx lr
        static_assert(GuestThread::KEYS_OFF < 0x1000, "ldr immediate");
        const u32 code[] = {0xe350007f, 0x83a00000, 0x812fff1e, 0xee1d1f70, 0xe0811100, 0xe5910000 | GuestThread::KEYS_OFF, 0xe12fff1e};
        add_guest_code("pthread_getspecific", code, 7);
    }
    reg_raw("pthread_setspecific", [](GuestThread& t) {
        u32 k = arg(t, 0);
        if (k >= 128) return ret(t, EINVAL);
        g::wr32(t.key_addr(k), arg(t, 1));
        ret(t, 0);
    });
    reg("pthread_once", +[](u32 once, u32 fn) -> int {
        std::lock_guard lk(once_lock);
        if (g::rd32(once) == 0) {
            g::wr32(once, 1);
            call_guest(fn, {});
            g::wr32(once, 2);
        }
        return 0;
    });
    reg("pthread_mutex_init", +[](u32 m, u32 attr) -> int {
        int type = PTHREAD_MUTEX_NORMAL;
        if (attr) {
            u32 a = g::rd32(attr) & 3;
            type = a == 1 ? PTHREAD_MUTEX_RECURSIVE : a == 2 ? PTHREAD_MUTEX_ERRORCHECK : PTHREAD_MUTEX_NORMAL;
        }
        g::wr32(m, 0);
        get_mutex(m, type);
        return 0;
    });
    reg("pthread_mutex_destroy", +[](u32 m) -> int {
        g::wr32(m, 0);  // the host mutex is left allocated: it may still be referenced
        return 0;
    });
    reg("pthread_mutex_lock", +[](u32 m) -> int { return pthread_mutex_lock(get_mutex(m)); });
    reg("pthread_mutex_unlock", +[](u32 m) -> int { return pthread_mutex_unlock(get_mutex(m)); });
    reg("pthread_mutex_trylock", +[](u32 m) -> int { return pthread_mutex_trylock(get_mutex(m)); });
    reg("pthread_mutexattr_init", +[](u32 a) -> int { g::wr32(a, 0); return 0; });
    reg("pthread_mutexattr_settype", +[](u32 a, int type) -> int { g::wr32(a, type); return 0; });
    reg("pthread_mutexattr_destroy", +[](u32) -> int { return 0; });
    reg("pthread_cond_init", +[](u32 c, u32) -> int {
        g::wr32(c, 0);
        get_cond(c);
        return 0;
    });
    reg("pthread_cond_destroy", +[](u32 c) -> int {
        g::wr32(c, 0);
        return 0;
    });
    reg("pthread_cond_wait", +[](u32 c, u32 m) -> int { return pthread_cond_wait(get_cond(c), get_mutex(m)); });
    reg("pthread_cond_timedwait", +[](u32 c, u32 m, u32 abst) -> int {
        timespec ts = get_timespec(abst);
        frame_sleep_begin();
        int r = pthread_cond_timedwait(get_cond(c), get_mutex(m), &ts);
        frame_sleep_end(1000000);  // shows as "1000000us" in the statistics
        return r;
    });
    reg("pthread_cond_signal", +[](u32 c) -> int { return pthread_cond_signal(get_cond(c)); });
    reg("pthread_cond_broadcast", +[](u32 c) -> int { return pthread_cond_broadcast(get_cond(c)); });

    // network
    reg("socket", +[](int d, int t, int p) -> int { return socket(d, t, p); });
    reg("bind", +[](int fd, const sockaddr* a, u32 l) -> int { return bind(fd, a, l); });
    reg("connect", +[](int fd, const sockaddr* a, u32 l) -> int {
        if (a && l >= sizeof(sockaddr_in) && a->sa_family == AF_INET) {  // (see lan_redirect_probe)
            sockaddr_in in;
            memcpy(&in, a, sizeof in);
            if (lan_redirect_probe(in.sin_addr.s_addr, in.sin_port))
                return connect(fd, reinterpret_cast<const sockaddr*>(&in), sizeof in);
        }
        return connect(fd, a, l);
    });
    reg("listen", +[](int fd, int n) -> int { return listen(fd, n); });
    reg("accept", +[](int fd, sockaddr* a, socklen_t* l) -> int {
        int c = accept(fd, a, l);
        sockaddr_in peer{};
        socklen_t pl = sizeof peer;
        if (c >= 0 && getpeername(c, reinterpret_cast<sockaddr*>(&peer), &pl) == 0 && peer.sin_family == AF_INET)
            lan_note_peer(peer.sin_addr.s_addr);
        return c;
    });
    reg("recv", +[](int fd, void* b, u32 n, int f) -> s32 { return static_cast<s32>(recv(fd, b, n, f)); });
    reg("send", +[](int fd, const void* b, u32 n, int f) -> s32 { return static_cast<s32>(send(fd, b, n, f)); });
    reg("recvfrom", +[](int fd, void* b, u32 n, int f, sockaddr* a, socklen_t* l) -> s32 {
        return static_cast<s32>(recvfrom(fd, b, n, f, a, l));
    });
    reg("sendto", +[](int fd, const void* b, u32 n, int f, const sockaddr* a, u32 l) -> s32 {
        return static_cast<s32>(sendto(fd, b, n, f, a, l));
    });
    reg("getpeername", +[](int fd, sockaddr* a, socklen_t* l) -> int { return getpeername(fd, a, l); });
    reg("getsockname", +[](int fd, sockaddr* a, socklen_t* l) -> int { return getsockname(fd, a, l); });
    reg("setsockopt", +[](int fd, int lvl, int name, const void* v, u32 len) -> int {
        if (lvl == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO) && len == 8) {
            const u32* w = static_cast<const u32*>(v);
            timeval tv{static_cast<s32>(w[0]), static_cast<s32>(w[1])};
            return setsockopt(fd, lvl, name, &tv, sizeof tv);
        }
        return setsockopt(fd, lvl, name, v, len);
    });
    reg("getsockopt", +[](int fd, int lvl, int name, void* v, socklen_t* len) -> int {
        if (lvl == SOL_SOCKET && (name == SO_RCVTIMEO || name == SO_SNDTIMEO)) {
            timeval tv{};
            socklen_t l = sizeof tv;
            int r = getsockopt(fd, lvl, name, &tv, &l);
            u32* w = static_cast<u32*>(v);
            w[0] = static_cast<u32>(tv.tv_sec);
            w[1] = static_cast<u32>(tv.tv_usec);
            *len = 8;
            return r;
        }
        return getsockopt(fd, lvl, name, v, len);
    });
    reg("select", +[](int n, fd_set* r, fd_set* w, fd_set* e, u32 tvp) -> int {
        timeval tv, *ptv = nullptr;
        if (tvp) {
            tv = {static_cast<s32>(g::rd32(tvp)), static_cast<s32>(g::rd32(tvp + 4))};
            ptv = &tv;
        }
        int res = select(n, r, w, e, ptv);
        if (tvp) {
            g::wr32(tvp, static_cast<u32>(tv.tv_sec));
            g::wr32(tvp + 4, static_cast<u32>(tv.tv_usec));
        }
        return res;
    });
    reg("getaddrinfo", +[](const char* node, const char* svc, u32 hints, u32 out) -> int {
        addrinfo h{}, *res = nullptr;
        if (hints) {
            h.ai_flags = static_cast<int>(g::rd32(hints));
            h.ai_family = static_cast<int>(g::rd32(hints + 4));
            h.ai_socktype = static_cast<int>(g::rd32(hints + 8));
            h.ai_protocol = static_cast<int>(g::rd32(hints + 12));
        }
        int r = getaddrinfo(node, svc, hints ? &h : nullptr, &res);
        if (r == 0) {
            g::wr32(out, put_addrinfo(res));
            freeaddrinfo(res);
        }
        return r;
    });
    reg("freeaddrinfo", +[](u32 ai) {
        while (ai) {
            u32 next = g::rd32(ai + 28);
            if (u32 cn = g::rd32(ai + 20)) g::free(cn);
            g::free(ai);
            ai = next;
        }
    });
    reg("inet_aton", +[](const char* s, in_addr* a) -> int { return inet_aton(s, a); });
    reg("inet_pton", +[](int af, const char* s, void* d) -> int { return inet_pton(af, s, d); });
    reg("inet_ntop", +[](int af, const void* s, char* d, u32 n) -> char* {
        return const_cast<char*>(inet_ntop(af, s, d, n));
    });
    reg("inet_ntoa", +[](u32 a) -> u32 {
        in_addr ia{a};
        return scratch_str(inet_ntoa(ia));
    });

    // libdl
    reg("dlopen", +[](const char* path, int) -> u32 {
        if (!path) return H_GLOBAL;
        std::string bn = base_name(path);
        if (is_host_lib(bn)) return H_HOSTLIB;
        if (GuestLib* l = elf_find(bn)) return l->base;
        // The original 32-bit libraries ship in our lib folder under their own names, except the engine
        // itself, which is libg32_s3e_android.so because the 64-bit wrapper took its name.
        std::string file;
        for (const std::string& c : {std::string(path), g_libdir + "/" + bn,
                                     g_libdir + "/libg32_" + (bn.rfind("lib", 0) == 0 ? bn.substr(3) : bn)}) {
            if (c.find('/') != std::string::npos && is_guest_elf(c)) {
                file = c;
                break;
            }
        }
        if (file.empty()) {
            LOGI("guest dlopen(%s): not available", path);
            dl_error = "library \"" + bn + "\" not found";
            return 0;
        }
        GuestLib* l = elf_load(file, bn);
        if (!l) {
            dl_error = "cannot load " + bn;
            return 0;
        }
        return l->base;
    });
    reg("dlsym", +[](u32 handle, const char* name) -> u32 {
        u32 r = 0;
        if (handle == H_HOSTLIB) {
            r = find_thunk(name);
        } else if (handle == H_GLOBAL || handle == 0xffffffff || handle == 0) {
            r = find_thunk(name);
            if (!r) r = elf_lookup_global(name);
        } else if (GuestLib* l = elf_find_addr(handle)) {
            r = l->lookup(name);
        }
        if (!r) {
            TRACE("guest dlsym(%x, %s) not found", handle, name);
            dl_error = std::string("undefined symbol: ") + name;
        }
        return r;
    });
    reg("dlclose", +[](u32) -> int { return 0; });
    reg("dlerror", +[]() -> u32 {
        if (dl_error.empty()) return 0;
        u32 s = scratch_str(dl_error.c_str());
        dl_error.clear();
        return s;
    });

    // zlib
    reg("inflateInit_", +[](u32 gs, const char*, int) -> int { return z_init(gs, 0, false); });
    reg("inflateInit2_", +[](u32 gs, int bits, const char*, int) -> int { return z_init(gs, bits, true); });
    reg("inflate", +[](u32 gs, int flush) -> int {
        z_stream* z = zs_in(gs);
        if (!z) return Z_STREAM_ERROR;
        int r = inflate(z, flush);
        zs_out(gs, z);
        return r;
    });
    reg("inflateReset", +[](u32 gs) -> int {
        z_stream* z = zs_in(gs);
        if (!z) return Z_STREAM_ERROR;
        int r = inflateReset(z);
        zs_out(gs, z);
        return r;
    });
    reg("inflateEnd", +[](u32 gs) -> int {
        z_stream* z;
        {
            std::lock_guard lk(z_lock);
            auto it = zstreams.find(gs);
            if (it == zstreams.end()) return Z_STREAM_ERROR;
            z = it->second;
            zstreams.erase(it);
        }
        int r = inflateEnd(z);
        delete z;
        g::wr32(gs + 28, 0);
        return r;
    });
}
