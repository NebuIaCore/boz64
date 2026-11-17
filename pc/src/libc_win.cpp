// C library, libdl, liblog and zlib for guest code, on Windows (the Android build's src/libc.cpp passes most calls
// straight to bionic; here the 32-bit Linux ABI is translated):
//   - guest file descriptors are this file's own table over CRT files and Winsock sockets;
//   - flags, errno values, socket levels/options and struct layouts are converted between Linux and Windows;
//   - paths are Android paths, kept under the data folder (vfs_win.cpp);
//   - pthread objects are Windows critical sections and condition variables;
//   - zlib comes from zlib1.dll next to the .exe.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <fcntl.h>
#include <io.h>
#include <sys/stat.h>
#include <sys/types.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <climits>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpu.h"
#include "elf.h"
#include "frame.h"
#include "lan.h"
#include "patches.h"

#define ZLIB_CONST
#include <zlib.h>

void register_stdio();
std::string fake_cpuinfo();
extern std::string g_libdir;  // folder holding the original 32-bit libraries

namespace {

// ---- Linux values ----
namespace lx {
constexpr int L_EPERM = 1, L_ENOENT = 2, L_EINTR = 4, L_EIO = 5, L_EBADF = 9, L_EAGAIN = 11, L_ENOMEM = 12, L_EACCES = 13, L_EFAULT = 14,
              L_EBUSY = 16, L_EEXIST = 17, L_ENOTDIR = 20, L_EISDIR = 21, L_EINVAL = 22, L_EMFILE = 24, L_ENOSPC = 28, L_ESPIPE = 29,
              L_ERANGE = 34, L_EDEADLK = 35, L_ENAMETOOLONG = 36, L_ENOSYS = 38, L_ENOTEMPTY = 39, L_ENOTSOCK = 88,
              L_EMSGSIZE = 90, L_EPROTONOSUPPORT = 93, L_EOPNOTSUPP = 95, L_EAFNOSUPPORT = 97, L_EADDRINUSE = 98,
              L_EADDRNOTAVAIL = 99, L_ENETDOWN = 100, L_ENETUNREACH = 101, L_ECONNABORTED = 103, L_ECONNRESET = 104,
              L_ENOBUFS = 105, L_EISCONN = 106, L_ENOTCONN = 107, L_ETIMEDOUT = 110, L_ECONNREFUSED = 111, L_EHOSTUNREACH = 113,
              L_EALREADY = 114, L_EINPROGRESS = 115;
constexpr int L_O_ACCMODE = 3, L_O_WRONLY = 1, L_O_RDWR = 2, L_O_CREAT = 0x40, L_O_EXCL = 0x80, L_O_TRUNC = 0x200,
              L_O_APPEND = 0x400, L_O_NONBLOCK = 0x800, L_O_DIRECTORY = 0x4000;
constexpr int L_F_GETFD = 1, L_F_SETFD = 2, L_F_GETFL = 3, L_F_SETFL = 4;
constexpr int L_AF_INET6_ = 10, L_SOCK_NONBLOCK_ = 0x800, L_SOCK_CLOEXEC_ = 0x80000;
constexpr int L_SOL_SOCKET_ = 1;
constexpr int L_SO_REUSEADDR_ = 2, L_SO_TYPE_ = 3, L_SO_ERROR_ = 4, L_SO_DONTROUTE_ = 5, L_SO_BROADCAST_ = 6, L_SO_SNDBUF_ = 7,
              L_SO_RCVBUF_ = 8, L_SO_KEEPALIVE_ = 9, L_SO_OOBINLINE_ = 10, L_SO_LINGER_ = 13, L_SO_REUSEPORT_ = 15,
              L_SO_RCVTIMEO_ = 20, L_SO_SNDTIMEO_ = 21;
constexpr int L_IP_TTL_ = 2, L_IP_MULTICAST_IF_ = 32, L_IP_MULTICAST_TTL_ = 33, L_IP_MULTICAST_LOOP_ = 34,
              L_IP_ADD_MEMBERSHIP_ = 35, L_IP_DROP_MEMBERSHIP_ = 36;
constexpr int L_MSG_DONTWAIT_ = 0x40, L_MSG_WAITALL_ = 0x100, L_MSG_NOSIGNAL_ = 0x4000;
constexpr int L_MAP_FIXED_ = 0x10, L_MAP_ANONYMOUS_ = 0x20;
constexpr int L_PROT_EXEC_ = 4;
}  // namespace lx

// CRT errno -> Linux errno
int lx_from_crt(int e) {
    switch (e) {
    case EDEADLK: return lx::L_EDEADLK;
    case ENAMETOOLONG: return lx::L_ENAMETOOLONG;
    case ENOSYS: return lx::L_ENOSYS;
    case ENOTEMPTY: return lx::L_ENOTEMPTY;
    default: return e <= 34 ? e : lx::L_EIO;  // 1-34 are the same numbers
    }
}
// Winsock error -> Linux errno
int lx_from_wsa(int e) {
    switch (e) {
    case WSAEINTR: return lx::L_EINTR;
    case WSAEBADF: return lx::L_EBADF;
    case WSAEACCES: return lx::L_EACCES;
    case WSAEFAULT: return lx::L_EFAULT;
    case WSAEINVAL: return lx::L_EINVAL;
    case WSAEMFILE: return lx::L_EMFILE;
    case WSAEWOULDBLOCK: return lx::L_EAGAIN;
    case WSAEINPROGRESS: return lx::L_EINPROGRESS;
    case WSAEALREADY: return lx::L_EALREADY;
    case WSAENOTSOCK: return lx::L_ENOTSOCK;
    case WSAEMSGSIZE: return lx::L_EMSGSIZE;
    case WSAEPROTONOSUPPORT: return lx::L_EPROTONOSUPPORT;
    case WSAEOPNOTSUPP: return lx::L_EOPNOTSUPP;
    case WSAEAFNOSUPPORT: return lx::L_EAFNOSUPPORT;
    case WSAEADDRINUSE: return lx::L_EADDRINUSE;
    case WSAEADDRNOTAVAIL: return lx::L_EADDRNOTAVAIL;
    case WSAENETDOWN: return lx::L_ENETDOWN;
    case WSAENETUNREACH: return lx::L_ENETUNREACH;
    case WSAECONNABORTED: return lx::L_ECONNABORTED;
    case WSAECONNRESET: return lx::L_ECONNRESET;
    case WSAENOBUFS: return lx::L_ENOBUFS;
    case WSAEISCONN: return lx::L_EISCONN;
    case WSAENOTCONN: return lx::L_ENOTCONN;
    case WSAETIMEDOUT: return lx::L_ETIMEDOUT;
    case WSAECONNREFUSED: return lx::L_ECONNREFUSED;
    case WSAEHOSTUNREACH: return lx::L_EHOSTUNREACH;
    default: return lx::L_EIO;
    }
}
// Failure helpers: set the guest-visible (Linux) errno and return the failure value.
int fail(int lx_err) {
    errno = lx_err;
    return -1;
}
int fail_crt() { return fail(lx_from_crt(errno)); }
int fail_wsa() { return fail(lx_from_wsa(WSAGetLastError())); }

// ---- file descriptors ----
struct Fd {
    enum Kind { NONE, FILE_, SOCK } kind = NONE;
    int crt = -1;
    SOCKET sock = INVALID_SOCKET;
    bool nonblock = false;
    std::string path;  // files: for fstat
};
std::mutex fd_lock;
std::vector<Fd> fds(3);  // 0-2: stdin, stdout, stderr (left to stdio.cpp)

int fd_add(const Fd& f) {
    std::lock_guard lk(fd_lock);
    for (size_t i = 3; i < fds.size(); i++)
        if (fds[i].kind == Fd::NONE) {
            fds[i] = f;
            return static_cast<int>(i);
        }
    fds.push_back(f);
    return static_cast<int>(fds.size() - 1);
}
Fd fd_get(int fd) {
    std::lock_guard lk(fd_lock);
    return fd >= 0 && static_cast<size_t>(fd) < fds.size() ? fds[fd] : Fd{};
}
void fd_set_nonblock(int fd, bool nb) {
    std::lock_guard lk(fd_lock);
    if (fd >= 0 && static_cast<size_t>(fd) < fds.size()) fds[fd].nonblock = nb;
}
Fd fd_take(int fd) {
    std::lock_guard lk(fd_lock);
    if (fd < 3 || static_cast<size_t>(fd) >= fds.size()) return {};
    Fd f = fds[fd];
    fds[fd] = Fd{};
    return f;
}

std::wstring widen(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
    std::wstring w(n > 0 ? n - 1 : 0, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
    return w;
}
std::string narrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n - 1 : 0, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, s.data(), n, nullptr, nullptr);
    return s;
}
std::wstring host_w(const char* guest) { return widen(boz_host_path(guest)); }

// ---- struct conversions (32-bit bionic layouts) ----
void put_timespec(u32 a, s64 sec, s64 nsec) {
    g::wr32(a, static_cast<u32>(sec));
    g::wr32(a + 4, static_cast<u32>(nsec));
}

// struct stat (104 bytes)
void put_stat(u32 a, const struct _stat64& s, u64 ino) {
    memset(g::mem + a, 0, 104);
    u32 mode = (s.st_mode & _S_IFDIR) ? (0040000 | 0775) : (0100000 | ((s.st_mode & _S_IWRITE) ? 0664 : 0444));
    g::wr64(a + 0, 0xfd00);
    g::wr32(a + 12, static_cast<u32>(ino));
    g::wr32(a + 16, mode);
    g::wr32(a + 20, 1);
    g::wr32(a + 24, 10123);
    g::wr32(a + 28, 10123);
    g::wr64(a + 48, static_cast<u64>(s.st_size));
    g::wr32(a + 56, 4096);
    g::wr64(a + 64, (static_cast<u64>(s.st_size) + 511) / 512);
    put_timespec(a + 72, s.st_atime, 0);
    put_timespec(a + 80, s.st_mtime, 0);
    put_timespec(a + 88, s.st_ctime, 0);
    g::wr64(a + 96, ino);
}
u64 path_ino(const std::string& p) { return std::hash<std::string>{}(p) & 0x7fffffff; }

int do_stat(const char* guest, u32 out) {
    struct _stat64 s;
    std::wstring w = host_w(guest);
    while (w.size() > 3 && (w.back() == L'/' || w.back() == L'\\')) w.pop_back();  // _wstat64 rejects "dir/"
    if (_wstat64(w.c_str(), &s) != 0) return fail_crt();
    put_stat(out, s, path_ino(guest));
    return 0;
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
void put_tm(u32 a, const struct tm& tm, long gmtoff, const char* zone) {
    const int v[9] = {tm.tm_sec, tm.tm_min, tm.tm_hour, tm.tm_mday, tm.tm_mon, tm.tm_year, tm.tm_wday, tm.tm_yday, tm.tm_isdst};
    for (int i = 0; i < 9; i++) g::wr32(a + 4 * i, static_cast<u32>(v[i]));
    g::wr32(a + 36, static_cast<u32>(gmtoff));
    g::wr32(a + 40, zone_str(zone));
}
struct tm get_tm(u32 a) {
    struct tm tm {};
    int* f[9] = {&tm.tm_sec, &tm.tm_min, &tm.tm_hour, &tm.tm_mday, &tm.tm_mon, &tm.tm_year, &tm.tm_wday, &tm.tm_yday, &tm.tm_isdst};
    for (int i = 0; i < 9; i++) *f[i] = static_cast<s32>(g::rd32(a + 4 * i));
    return tm;
}
long local_gmtoff(int isdst) {
    long tz = 0, dst = 0;
    _get_timezone(&tz);
    _get_dstbias(&dst);
    return -(tz + (isdst > 0 ? dst : 0));
}

u32 daylight_addr = 0, tzname_addr = 0;
void sync_tz() {
    _tzset();
    int dl = 0;
    _get_daylight(&dl);
    g::wr32(daylight_addr, static_cast<u32>(dl));
    g::wr32(tzname_addr, zone_str(_tzname[0]));
    g::wr32(tzname_addr + 4, zone_str(_tzname[1]));
}

// ---- pthreads ----
// Guest mutexes and condition variables are 4-byte words. On first use each one gets a host object and its word is
// replaced by a tag + index into these tables.
constexpr u32 TAG_MASK = 0xF0000000, MUTEX_TAG = 0xB0000000, COND_TAG = 0xC0000000;
constexpr u32 MAX_SYNC = 1 << 18;
struct Mutex {
    CRITICAL_SECTION cs;
    int type;  // 0 normal, 1 recursive, 2 error-checking
    std::atomic<DWORD> owner{0};
    int depth = 0;
    Mutex(int t) : type(t) { InitializeCriticalSectionAndSpinCount(&cs, 1000); }
};
std::atomic<Mutex*> mutex_tab[MAX_SYNC];
std::atomic<CONDITION_VARIABLE*> cond_tab[MAX_SYNC];
std::atomic<u32> next_mutex{1}, next_cond{1};

Mutex* get_mutex(u32 gm, int type_if_new = -1) {
    auto* word = reinterpret_cast<std::atomic<u32>*>(g::mem + gm);
    for (;;) {
        u32 v = word->load();
        if ((v & TAG_MASK) == MUTEX_TAG) return mutex_tab[v & ~TAG_MASK].load();
        int type = type_if_new;
        if (type < 0)  // static initialisers of old bionic: 0 normal, 0x4000 recursive, 0x8000 error-checking
            type = (v & 0xC000) == 0x4000 ? 1 : (v & 0xC000) == 0x8000 ? 2 : 0;
        u32 idx = next_mutex++;
        if (idx >= MAX_SYNC) fatal("too many guest mutexes");
        auto* m = new Mutex(type);
        mutex_tab[idx] = m;
        if (word->compare_exchange_strong(v, MUTEX_TAG | idx)) return m;
        mutex_tab[idx] = nullptr;  // another thread got there first (the object is leaked: it is tiny)
    }
}
int mutex_lock(Mutex* m) {
    DWORD me = GetCurrentThreadId();
    if (m->owner.load() == me && m->type != 1) return m->type == 2 ? lx::L_EDEADLK : lx::L_EDEADLK;
    EnterCriticalSection(&m->cs);
    m->owner = me;
    m->depth++;
    return 0;
}
int mutex_trylock(Mutex* m) {
    DWORD me = GetCurrentThreadId();
    if (m->owner.load() == me && m->type != 1) return lx::L_EBUSY;
    if (!TryEnterCriticalSection(&m->cs)) return lx::L_EBUSY;
    m->owner = me;
    m->depth++;
    return 0;
}
int mutex_unlock(Mutex* m) {
    if (m->owner.load() != GetCurrentThreadId()) return lx::L_EPERM;
    if (--m->depth == 0) m->owner = 0;
    LeaveCriticalSection(&m->cs);
    return 0;
}

CONDITION_VARIABLE* get_cond(u32 gc) {
    auto* word = reinterpret_cast<std::atomic<u32>*>(g::mem + gc);
    for (;;) {
        u32 v = word->load();
        if ((v & TAG_MASK) == COND_TAG) return cond_tab[v & ~TAG_MASK].load();
        u32 idx = next_cond++;
        if (idx >= MAX_SYNC) fatal("too many guest condition variables");
        auto* c = new CONDITION_VARIABLE;
        InitializeConditionVariable(c);
        cond_tab[idx] = c;
        if (word->compare_exchange_strong(v, COND_TAG | idx)) return c;
        cond_tab[idx] = nullptr;
    }
}
// Waits with the mutex held once (a recursive mutex held deeper keeps the extra levels, as with bionic).
int cond_wait(CONDITION_VARIABLE* c, Mutex* m, DWORD ms) {
    int depth = m->depth;
    m->depth = 0;
    m->owner = 0;
    BOOL ok = SleepConditionVariableCS(c, &m->cs, ms);
    DWORD err = ok ? 0 : GetLastError();
    m->owner = GetCurrentThreadId();
    m->depth = depth;
    if (!ok) return err == ERROR_TIMEOUT ? lx::L_ETIMEDOUT : lx::L_EINVAL;
    return 0;
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

DWORD WINAPI thread_entry(void* p) {
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
    return 0;
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
bool is_guest_elf(const std::string& path) {
    FILE* f = _wfopen(widen(path).c_str(), L"rb");
    if (!f) return false;
    unsigned char h[20] = {};
    size_t n = fread(h, 1, sizeof h, f);
    fclose(f);
    return n == sizeof h && memcmp(h, "\x7f" "ELF", 4) == 0 && h[4] == 1 && h[18] == 40;
}

// ---- zlib (zlib1.dll) ----
struct Zlib {
    int (*inflateInit_)(z_stream*, const char*, int) = nullptr;
    int (*inflateInit2_)(z_stream*, int, const char*, int) = nullptr;
    int (*inflate)(z_stream*, int) = nullptr;
    int (*inflateReset)(z_stream*) = nullptr;
    int (*inflateEnd)(z_stream*) = nullptr;
    const char* (*zlibVersion)() = nullptr;
} zl;
void load_zlib() {
    std::string p = std::string(boz_exe_dir()) + "/zlib1.dll";
    HMODULE h = LoadLibraryW(widen(p).c_str());
    if (!h) {
        LOGE("zlib1.dll is missing next to the .exe; compressed game data will not load");
        return;
    }
    zl.inflateInit_ = reinterpret_cast<decltype(zl.inflateInit_)>(GetProcAddress(h, "inflateInit_"));
    zl.inflateInit2_ = reinterpret_cast<decltype(zl.inflateInit2_)>(GetProcAddress(h, "inflateInit2_"));
    zl.inflate = reinterpret_cast<decltype(zl.inflate)>(GetProcAddress(h, "inflate"));
    zl.inflateReset = reinterpret_cast<decltype(zl.inflateReset)>(GetProcAddress(h, "inflateReset"));
    zl.inflateEnd = reinterpret_cast<decltype(zl.inflateEnd)>(GetProcAddress(h, "inflateEnd"));
    zl.zlibVersion = reinterpret_cast<decltype(zl.zlibVersion)>(GetProcAddress(h, "zlibVersion"));
    LOGI("zlib %s", zl.zlibVersion ? zl.zlibVersion() : "?");
}
// zlib's z_stream holds pointers, so each guest stream (56 bytes in 32-bit) gets a host twin.
std::mutex z_lock;
std::unordered_map<u32, z_stream*> zstreams;
z_stream* zs_in(u32 gs) {
    std::lock_guard lk(z_lock);
    auto it = zstreams.find(gs);
    if (it == zstreams.end()) return nullptr;
    z_stream* z = it->second;
    z->next_in = static_cast<const Bytef*>(g::h(g::rd32(gs + 0)));
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
    g::wr32(gs + 44, static_cast<u32>(z->data_type));
    g::wr32(gs + 48, static_cast<u32>(z->adler));
}
int z_init(u32 gs, int window_bits, bool with_bits) {
    if (!zl.inflateInit_ || !zl.inflateInit2_) return Z_VERSION_ERROR;
    auto* z = new z_stream{};
    z->next_in = static_cast<const Bytef*>(g::h(g::rd32(gs + 0)));
    z->avail_in = g::rd32(gs + 4);
    int r = with_bits ? zl.inflateInit2_(z, window_bits, ZLIB_VERSION, static_cast<int>(sizeof(z_stream)))
                      : zl.inflateInit_(z, ZLIB_VERSION, static_cast<int>(sizeof(z_stream)));
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

// ---- sockets ----
bool wsa_ready = false;
void init_winsock() {
    WSADATA d;
    wsa_ready = WSAStartup(MAKEWORD(2, 2), &d) == 0;
}
SOCKET sock_of(int fd) {
    Fd f = fd_get(fd);
    return f.kind == Fd::SOCK ? f.sock : INVALID_SOCKET;
}
// Guest sockaddrs are Linux's; only the AF_INET6 number differs from Windows.
std::vector<u8> sa_in(const void* a, u32 len) {
    std::vector<u8> v(static_cast<const u8*>(a), static_cast<const u8*>(a) + len);
    if (len >= 2 && (v[0] | (v[1] << 8)) == lx::L_AF_INET6_) v[0] = AF_INET6, v[1] = 0;
    return v;
}
void sa_out(void* a, u32 len) {
    auto* b = static_cast<u8*>(a);
    if (len >= 2 && (b[0] | (b[1] << 8)) == AF_INET6) b[0] = lx::L_AF_INET6_, b[1] = 0;
}
int family_in(int f) { return f == lx::L_AF_INET6_ ? AF_INET6 : f; }
int family_out(int f) { return f == AF_INET6 ? lx::L_AF_INET6_ : f; }

// setsockopt/getsockopt level and name: Linux -> Windows (0 if unknown)
bool sockopt_map(int lvl, int name, int& wl, int& wn) {
    if (lvl == lx::L_SOL_SOCKET_) {
        wl = SOL_SOCKET;
        switch (name) {
        case lx::L_SO_REUSEADDR_: wn = SO_REUSEADDR; return true;
        case lx::L_SO_REUSEPORT_: wn = SO_REUSEADDR; return true;
        case lx::L_SO_TYPE_: wn = SO_TYPE; return true;
        case lx::L_SO_ERROR_: wn = SO_ERROR; return true;
        case lx::L_SO_DONTROUTE_: wn = SO_DONTROUTE; return true;
        case lx::L_SO_BROADCAST_: wn = SO_BROADCAST; return true;
        case lx::L_SO_SNDBUF_: wn = SO_SNDBUF; return true;
        case lx::L_SO_RCVBUF_: wn = SO_RCVBUF; return true;
        case lx::L_SO_KEEPALIVE_: wn = SO_KEEPALIVE; return true;
        case lx::L_SO_OOBINLINE_: wn = SO_OOBINLINE; return true;
        case lx::L_SO_LINGER_: wn = SO_LINGER; return true;
        case lx::L_SO_RCVTIMEO_: wn = SO_RCVTIMEO; return true;
        case lx::L_SO_SNDTIMEO_: wn = SO_SNDTIMEO; return true;
        default: return false;
        }
    }
    if (lvl == IPPROTO_IP) {
        wl = IPPROTO_IP;
        switch (name) {
        case lx::L_IP_TTL_: wn = IP_TTL; return true;
        case lx::L_IP_MULTICAST_IF_: wn = IP_MULTICAST_IF; return true;
        case lx::L_IP_MULTICAST_TTL_: wn = IP_MULTICAST_TTL; return true;
        case lx::L_IP_MULTICAST_LOOP_: wn = IP_MULTICAST_LOOP; return true;
        case lx::L_IP_ADD_MEMBERSHIP_: wn = IP_ADD_MEMBERSHIP; return true;
        case lx::L_IP_DROP_MEMBERSHIP_: wn = IP_DROP_MEMBERSHIP; return true;
        default: return false;
        }
    }
    wl = lvl;  // IPPROTO_TCP (TCP_NODELAY is 1 on both) and others pass through
    wn = name;
    return true;
}
int msg_flags(int f, bool& dontwait) {
    dontwait = f & lx::L_MSG_DONTWAIT_;
    int w = f & (MSG_OOB | MSG_PEEK | MSG_DONTROUTE);
    if (f & lx::L_MSG_WAITALL_) w |= MSG_WAITALL;
    return w;
}
// Runs a socket call non-blocking for MSG_DONTWAIT on a blocking socket.
template <class F>
int with_dontwait(int fd, SOCKET s, bool dontwait, F f) {
    bool nb = fd_get(fd).nonblock;
    u_long on = 1, off = 0;
    if (dontwait && !nb) ioctlsocket(s, FIONBIO, &on);
    int r = f();
    int err = WSAGetLastError();
    if (dontwait && !nb) ioctlsocket(s, FIONBIO, &off);
    WSASetLastError(err);
    return r;
}

// Guest fd_set (Linux: 1024-bit map) <-> Windows fd_set (list of SOCKETs)
struct SelSet {
    fd_set w;
    std::vector<std::pair<int, SOCKET>> map;
};
bool sel_in(u32 gset, int n, SelSet& s, int& files_ready) {
    FD_ZERO(&s.w);
    if (!gset) return false;
    bool any = false;
    for (int fd = 0; fd < n && fd < 1024; fd++) {
        if (!(g::rd32(gset + 4 * (fd / 32)) & (1u << (fd % 32)))) continue;
        Fd f = fd_get(fd);
        if (f.kind == Fd::SOCK) {
            if (s.w.fd_count < FD_SETSIZE) FD_SET(f.sock, &s.w);
            s.map.push_back({fd, f.sock});
            any = true;
        } else {
            files_ready++;  // files are always ready
        }
    }
    return any;
}
int sel_out(u32 gset, int n, const SelSet& s) {
    if (!gset) return 0;
    int count = 0;
    for (int i = 0; i < (n + 31) / 32 && i < 32; i++) g::wr32(gset + 4 * i, 0);
    for (auto& [fd, sk] : s.map)
        if (FD_ISSET(sk, &s.w)) {
            g::wr32(gset + 4 * (fd / 32), g::rd32(gset + 4 * (fd / 32)) | (1u << (fd % 32)));
            count++;
        }
    return count;
}

// struct addrinfo (32 bytes): flags, family, socktype, protocol, addrlen, canonname*, addr*, next*
u32 put_addrinfo(const addrinfo* ai) {
    u32 head = 0, prev = 0;
    for (; ai; ai = ai->ai_next) {
        u32 len = static_cast<u32>(ai->ai_addrlen);
        u32 n = g::calloc(1, 32 + len);
        g::wr32(n + 0, static_cast<u32>(ai->ai_flags));
        g::wr32(n + 4, static_cast<u32>(family_out(ai->ai_family)));
        g::wr32(n + 8, static_cast<u32>(ai->ai_socktype));
        g::wr32(n + 12, static_cast<u32>(ai->ai_protocol));
        g::wr32(n + 16, len);
        g::wr32(n + 20, ai->ai_canonname ? g::strdup(ai->ai_canonname) : 0);
        if (ai->ai_addr) {
            memcpy(g::mem + n + 32, ai->ai_addr, len);
            sa_out(g::mem + n + 32, len);
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

u32 scratch_str(const char* s) {
    GuestThread& t = self();
    u32 b = guest_scratch(t, 4096);
    strncpy(static_cast<char*>(g::h(b)), s ? s : "", 4095);
    g::wr8(b + 4095, 0);
    return b;
}

// The guest's environment: what an Android app sees (the Windows environment is not passed on).
const std::map<std::string, std::string>& guest_env() {
    static const std::map<std::string, std::string> e = {
        {"EXTERNAL_STORAGE", "/sdcard"}, {"ANDROID_DATA", "/data"}, {"ANDROID_ROOT", "/system"},
        {"ANDROID_STORAGE", "/storage"}, {"PATH", "/sbin:/system/bin"}, {"TMPDIR", "/data/local/tmp"}};
    return e;
}

const char* lx_strerror(int e) {
    switch (e) {
    case 0: return "Success";
    case lx::L_EPERM: return "Operation not permitted";
    case lx::L_ENOENT: return "No such file or directory";
    case lx::L_EINTR: return "Interrupted system call";
    case lx::L_EIO: return "I/O error";
    case lx::L_EBADF: return "Bad file descriptor";
    case lx::L_EAGAIN: return "Try again";
    case lx::L_ENOMEM: return "Out of memory";
    case lx::L_EACCES: return "Permission denied";
    case lx::L_EEXIST: return "File exists";
    case lx::L_ENOTDIR: return "Not a directory";
    case lx::L_EISDIR: return "Is a directory";
    case lx::L_EINVAL: return "Invalid argument";
    case lx::L_ENOSPC: return "No space left on device";
    case lx::L_ETIMEDOUT: return "Connection timed out";
    case lx::L_ECONNREFUSED: return "Connection refused";
    case lx::L_ECONNRESET: return "Connection reset by peer";
    case lx::L_EINPROGRESS: return "Operation now in progress";
    case lx::L_ENOTCONN: return "Transport endpoint is not connected";
    default: return "Unknown error";
    }
}

// drand48 family (48-bit LCG, as bionic)
std::mutex rand48_lock;
u64 rand48_state = 0x1234ABCD330Eull;
s32 lrand48_next() {
    std::lock_guard lk(rand48_lock);
    rand48_state = (0x5DEECE66Dull * rand48_state + 0xB) & ((1ull << 48) - 1);
    return static_cast<s32>(rand48_state >> 17);
}

int open_text(const std::string& info, const char* name) {
    wchar_t dir[MAX_PATH], file[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, L"boz", 0, file);
    int crt = _wopen(file, _O_RDWR | _O_BINARY | _O_CREAT | _O_TRUNC | _O_TEMPORARY, _S_IREAD | _S_IWRITE);
    if (crt < 0) return fail_crt();
    _write(crt, info.data(), static_cast<unsigned>(info.size()));
    _lseeki64(crt, 0, SEEK_SET);
    Fd f;
    f.kind = Fd::FILE_;
    f.crt = crt;
    f.path = name;
    return fd_add(f);
}
int open_cpuinfo() {
    static std::string info = fake_cpuinfo();
    return open_text(info, "/proc/cpuinfo");
}

// opendir handles: a guest buffer that also holds the current struct dirent (bionic 32-bit layout:
// u64 d_ino, s64 d_off, u16 d_reclen, u8 d_type, char d_name[256]; 280 bytes)
constexpr u32 DIRENT_SIZE = 280;
struct Dir {
    HANDLE h = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW fd{};
    bool first = true;
    s64 off = 0;
};
std::mutex dir_lock;
std::unordered_map<u32, Dir*> dirs;
}  // namespace

std::string g_libdir;

void register_libc() {
    init_winsock();
    load_zlib();
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
        if (!p) return lx::L_ENOMEM;
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
    reg("strlen", +[](const char* s) -> u32 { return static_cast<u32>(strlen(s)); });
    reg("strcmp", +[](const char* a, const char* b) -> int { return strcmp(a, b); });
    reg("strncmp", +[](const char* a, const char* b, u32 n) -> int { return strncmp(a, b, n); });
    reg("strcasecmp", +[](const char* a, const char* b) -> int { return _stricmp(a, b); });
    reg("strncasecmp", +[](const char* a, const char* b, u32 n) -> int { return _strnicmp(a, b, n); });
    reg("strcpy", +[](char* d, const char* s) -> char* { return strcpy(d, s); });
    reg("strncpy", +[](char* d, const char* s, u32 n) -> char* { return strncpy(d, s, n); });
    reg("strcat", +[](char* d, const char* s) -> char* { return strcat(d, s); });
    reg("strncat", +[](char* d, const char* s, u32 n) -> char* { return strncat(d, s, n); });
    reg("strchr", +[](char* s, int c) -> char* { return strchr(s, c); });
    reg("strrchr", +[](char* s, int c) -> char* { return strrchr(s, c); });
    reg("strstr", +[](char* h, const char* n) -> char* { return strstr(h, n); });
    reg("strpbrk", +[](char* s, const char* a) -> char* { return strpbrk(s, a); });
    reg("strspn", +[](const char* s, const char* a) -> u32 { return static_cast<u32>(strspn(s, a)); });
    reg("strcspn", +[](const char* s, const char* a) -> u32 { return static_cast<u32>(strcspn(s, a)); });
    reg("strtok", +[](char* s, const char* d) -> char* { return strtok(s, d); });  // per-thread state in the CRT
    reg("strdup", +[](const char* s) -> u32 { return g::strdup(s); });
    reg("strerror", +[](int e) -> u32 { return scratch_str(lx_strerror(e)); });
    reg("tolower", +[](int c) -> int { return tolower(c); });
    reg("toupper", +[](int c) -> int { return toupper(c); });
    reg("isxdigit", +[](int c) -> int { return isxdigit(c & 0xff); });
    reg("isdigit", +[](int c) -> int { return isdigit(c & 0xff); });
    reg("isspace", +[](int c) -> int { return isspace(c & 0xff); });
    reg("isalpha", +[](int c) -> int { return isalpha(c & 0xff); });
    reg("isalnum", +[](int c) -> int { return isalnum(c & 0xff); });
    reg("isupper", +[](int c) -> int { return isupper(c & 0xff); });
    reg("islower", +[](int c) -> int { return islower(c & 0xff); });
    reg("isprint", +[](int c) -> int { return isprint(c & 0xff); });
    reg("atoi", +[](const char* s) -> int { return atoi(s); });
    reg("atol", +[](const char* s) -> s32 { return static_cast<s32>(atol(s)); });
    reg("atof", +[](const char* s) -> double { return atof(s); });
    reg_raw("strtol", [](GuestThread& t) {
        const char* s = g::str(arg(t, 0));
        char* end = nullptr;
        long long v = strtoll(s, &end, static_cast<int>(arg(t, 2)));
        if (v > INT_MAX) { v = INT_MAX; errno = lx::L_ERANGE; }
        if (v < INT_MIN) { v = INT_MIN; errno = lx::L_ERANGE; }
        if (arg(t, 1)) g::wr32(arg(t, 1), g::to_guest(end));
        ret(t, static_cast<u32>(static_cast<s32>(v)));
    });
    reg_raw("strtoul", [](GuestThread& t) {
        const char* s = g::str(arg(t, 0));
        char* end = nullptr;
        unsigned long long v = strtoull(s, &end, static_cast<int>(arg(t, 2)));
        if (v > UINT_MAX && !(v >> 63)) { v = UINT_MAX; errno = lx::L_ERANGE; }
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
        if (strcmp(path, "/proc/cpuinfo") == 0) return open_cpuinfo();
        if (strncmp(path, "/proc/", 6) == 0) {
            std::string c = boz_fake_proc(path);
            if (!c.empty()) return open_text(c, path);
        }
        int acc = flags & lx::L_O_ACCMODE;
        int w = _O_BINARY | _O_NOINHERIT | (acc == lx::L_O_WRONLY ? _O_WRONLY : acc == lx::L_O_RDWR ? _O_RDWR : _O_RDONLY);
        if (flags & lx::L_O_CREAT) w |= _O_CREAT;
        if (flags & lx::L_O_EXCL) w |= _O_EXCL;
        if (flags & lx::L_O_TRUNC) w |= _O_TRUNC;
        if (flags & lx::L_O_APPEND) w |= _O_APPEND;
        std::wstring hp = host_w(path);
        if (flags & lx::L_O_DIRECTORY) {
            DWORD a = GetFileAttributesW(hp.c_str());
            if (a == INVALID_FILE_ATTRIBUTES) return fail(lx::L_ENOENT);
            if (!(a & FILE_ATTRIBUTE_DIRECTORY)) return fail(lx::L_ENOTDIR);
        }
        DWORD a = GetFileAttributesW(hp.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) {
            if (acc != 0) return fail(lx::L_EISDIR);
            Fd f;  // a directory opened read-only: a placeholder that fstat understands
            f.kind = Fd::FILE_;
            f.crt = -1;
            f.path = path;
            return fd_add(f);
        }
        int crt = _wopen(hp.c_str(), w, (mode & 0200) ? (_S_IREAD | _S_IWRITE) : _S_IREAD);
        if (crt < 0) return fail_crt();
        Fd f;
        f.kind = Fd::FILE_;
        f.crt = crt;
        f.path = path;
        return fd_add(f);
    });
    reg("close", +[](int fd) -> int {
        if (fd >= 0 && fd < 3) return 0;
        Fd f = fd_take(fd);
        if (f.kind == Fd::SOCK) return closesocket(f.sock) == 0 ? 0 : fail_wsa();
        if (f.kind == Fd::FILE_) return f.crt < 0 || _close(f.crt) == 0 ? 0 : fail_crt();
        return fail(lx::L_EBADF);
    });
    reg("read", +[](int fd, void* b, u32 n) -> s32 {
        Fd f = fd_get(fd);
        if (f.kind == Fd::SOCK) {
            int r = recv(f.sock, static_cast<char*>(b), static_cast<int>(n), 0);
            return r >= 0 ? r : fail_wsa();
        }
        if (f.kind != Fd::FILE_ || f.crt < 0) return fail(lx::L_EBADF);
        int r = _read(f.crt, b, n);
        return r >= 0 ? r : fail_crt();
    });
    reg("write", +[](int fd, const void* b, u32 n) -> s32 {
        if (fd == 1 || fd == 2) {
            std::string s(static_cast<const char*>(b), n);
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
            if (!s.empty()) __android_log_write(ANDROID_LOG_INFO, "boz64-guest", s.c_str());
            return static_cast<s32>(n);
        }
        Fd f = fd_get(fd);
        if (f.kind == Fd::SOCK) {
            int r = send(f.sock, static_cast<const char*>(b), static_cast<int>(n), 0);
            return r >= 0 ? r : fail_wsa();
        }
        if (f.kind != Fd::FILE_ || f.crt < 0) return fail(lx::L_EBADF);
        int r = _write(f.crt, b, n);
        return r >= 0 ? r : fail_crt();
    });
    reg("lseek", +[](int fd, s32 off, int whence) -> s32 {
        Fd f = fd_get(fd);
        if (f.kind == Fd::SOCK) return fail(lx::L_ESPIPE);
        if (f.kind != Fd::FILE_ || f.crt < 0) return fail(lx::L_EBADF);
        s64 r = _lseeki64(f.crt, off, whence);
        return r >= 0 ? static_cast<s32>(r) : fail_crt();
    });
    reg("fcntl", +[](int fd, int cmd, u32 a) -> int {
        Fd f = fd_get(fd);
        if (f.kind == Fd::NONE && fd >= 3) return fail(lx::L_EBADF);
        switch (cmd) {
        case lx::L_F_GETFD: return 0;
        case lx::L_F_SETFD: return 0;
        case lx::L_F_GETFL: return lx::L_O_RDWR | (f.nonblock ? lx::L_O_NONBLOCK : 0);
        case lx::L_F_SETFL:
            if (f.kind == Fd::SOCK) {
                u_long nb = (a & lx::L_O_NONBLOCK) ? 1 : 0;
                if (ioctlsocket(f.sock, FIONBIO, &nb) != 0) return fail_wsa();
            }
            fd_set_nonblock(fd, a & lx::L_O_NONBLOCK);
            return 0;
        default: return fail(lx::L_EINVAL);
        }
    });
    reg("ftruncate", +[](int fd, s32 len) -> int {
        Fd f = fd_get(fd);
        if (f.kind != Fd::FILE_ || f.crt < 0) return fail(lx::L_EBADF);
        return _chsize_s(f.crt, len) == 0 ? 0 : fail_crt();
    });
    reg("access", +[](const char* p, int m) -> int {
        if (strcmp(p, "/proc/cpuinfo") == 0) return 0;
        return _waccess(host_w(p).c_str(), m & 6) == 0 ? 0 : fail_crt();
    });
    reg("chmod", +[](const char* p, u32 m) -> int {
        return _wchmod(host_w(p).c_str(), (m & 0200) ? (_S_IREAD | _S_IWRITE) : _S_IREAD) == 0 ? 0 : fail_crt();
    });
    reg("mkdir", +[](const char* p, u32) -> int { return _wmkdir(host_w(p).c_str()) == 0 ? 0 : fail_crt(); });
    reg("rmdir", +[](const char* p) -> int { return _wrmdir(host_w(p).c_str()) == 0 ? 0 : fail_crt(); });
    reg("unlink", +[](const char* p) -> int { return _wunlink(host_w(p).c_str()) == 0 ? 0 : fail_crt(); });
    reg("remove", +[](const char* p) -> int {
        std::wstring w = host_w(p);
        DWORD a = GetFileAttributesW(w.c_str());
        if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return _wrmdir(w.c_str()) == 0 ? 0 : fail_crt();
        return _wunlink(w.c_str()) == 0 ? 0 : fail_crt();
    });
    reg("rename", +[](const char* a, const char* b) -> int {
        // POSIX rename replaces the target
        if (MoveFileExW(host_w(a).c_str(), host_w(b).c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) return 0;
        DWORD e = GetLastError();
        return fail(e == ERROR_FILE_NOT_FOUND || e == ERROR_PATH_NOT_FOUND ? lx::L_ENOENT : lx::L_EACCES);
    });
    reg("stat", +[](const char* p, u32 out) -> int { return do_stat(p, out); });
    reg("lstat", +[](const char* p, u32 out) -> int { return do_stat(p, out); });
    reg("fstat", +[](int fd, u32 out) -> int {
        Fd f = fd_get(fd);
        if (f.kind == Fd::SOCK) {
            memset(g::mem + out, 0, 104);
            g::wr32(out + 16, 0140000 | 0777);  // S_IFSOCK
            return 0;
        }
        if (f.kind != Fd::FILE_) return fail(lx::L_EBADF);
        if (f.crt < 0) return do_stat(f.path.c_str(), out);
        struct _stat64 s;
        if (_fstat64(f.crt, &s) != 0) return fail_crt();
        put_stat(out, s, path_ino(f.path));
        return 0;
    });
    reg("statfs", +[](const char* p, u32 out) -> int {
        std::string hp = boz_host_path(p);
        std::wstring dir = widen(hp.substr(0, 3));  // the drive
        ULARGE_INTEGER avail{}, total{}, freeb{};
        GetDiskFreeSpaceExW(dir.c_str(), &avail, &total, &freeb);
        memset(g::mem + out, 0, 84);
        g::wr32(out + 0, 0xEF53);  // ext4
        g::wr32(out + 4, 4096);
        g::wr64(out + 8, total.QuadPart / 4096);
        g::wr64(out + 16, freeb.QuadPart / 4096);
        g::wr64(out + 24, avail.QuadPart / 4096);
        g::wr64(out + 32, 1000000);
        g::wr64(out + 40, 500000);
        g::wr32(out + 56, 255);
        g::wr32(out + 60, 4096);
        return 0;
    });
    reg("opendir", +[](const char* p) -> u32 {
        std::wstring w = host_w(p);
        while (!w.empty() && (w.back() == L'/' || w.back() == L'\\')) w.pop_back();
        auto* d = new Dir;
        d->h = FindFirstFileW((w + L"/*").c_str(), &d->fd);
        if (d->h == INVALID_HANDLE_VALUE) {
            delete d;
            errno = lx::L_ENOENT;
            return 0;
        }
        u32 gd = g::calloc(1, DIRENT_SIZE);
        std::lock_guard lk(dir_lock);
        dirs[gd] = d;
        return gd;
    });
    reg("readdir", +[](u32 gd) -> u32 {
        Dir* d;
        {
            std::lock_guard lk(dir_lock);
            auto it = dirs.find(gd);
            if (it == dirs.end()) return 0;
            d = it->second;
        }
        if (!d->first && !FindNextFileW(d->h, &d->fd)) return 0;
        d->first = false;
        std::string name = narrow(d->fd.cFileName);
        memset(g::mem + gd, 0, DIRENT_SIZE);
        g::wr64(gd + 0, path_ino(name) + 1);
        g::wr64(gd + 8, static_cast<u64>(++d->off));
        g::wr16(gd + 16, static_cast<u16>(DIRENT_SIZE));
        g::wr8(gd + 18, (d->fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 4 : 8);  // DT_DIR, DT_REG
        strncpy(reinterpret_cast<char*>(g::mem + gd + 19), name.c_str(), 255);
        return gd;
    });
    reg("closedir", +[](u32 gd) -> int {
        Dir* d;
        {
            std::lock_guard lk(dir_lock);
            auto it = dirs.find(gd);
            if (it == dirs.end()) return fail(lx::L_EBADF);
            d = it->second;
            dirs.erase(it);
        }
        FindClose(d->h);
        delete d;
        g::free(gd);
        return 0;
    });
    reg("mmap", +[](u32 addr, u32 len, int prot, int flags, int fd, s32 off) -> u32 {
        u32 a = (flags & lx::L_MAP_FIXED_) ? addr : g::page_alloc(len);
        if (!a) {
            errno = lx::L_ENOMEM;
            return 0xffffffff;
        }
        if (flags & lx::L_MAP_FIXED_) memset(g::mem + a, 0, len);
        if (!(flags & lx::L_MAP_ANONYMOUS_) && fd >= 0) {
            Fd f = fd_get(fd);
            if (f.kind == Fd::FILE_ && f.crt >= 0) {
                s64 cur = _lseeki64(f.crt, 0, SEEK_CUR);
                _lseeki64(f.crt, off, SEEK_SET);
                _read(f.crt, g::mem + a, len);
                _lseeki64(f.crt, cur, SEEK_SET);
            }
        }
        if (prot & lx::L_PROT_EXEC_) {
            invalidate_code(a, len);
            if (flags & lx::L_MAP_ANONYMOUS_) patches_note_exec_region(a, len);
        }
        return a;
    });
    reg("munmap", +[](u32 a, u32 len) -> int {
        if (a >= g::ALLOC_START && a + len <= g::ALLOC_END) g::page_free(a, len);
        invalidate_code(a, len);
        return 0;
    });
    reg("mprotect", +[](u32 a, u32 len, int prot) -> int {
        if (prot & lx::L_PROT_EXEC_) invalidate_code(a, len);
        return 0;
    });

    // time
    reg("time", +[](u32 out) -> s32 {
        s32 now = static_cast<s32>(::time(nullptr));
        if (out) g::wr32(out, static_cast<u32>(now));
        return now;
    });
    reg("gettimeofday", +[](u32 tv, u32 tz) -> int {
        timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        if (tv) {
            g::wr32(tv, static_cast<u32>(ts.tv_sec));
            g::wr32(tv + 4, static_cast<u32>(ts.tv_nsec / 1000));
        }
        if (tz) g::wr64(tz, 0);
        return 0;
    });
    reg("clock_gettime", +[](int clk, u32 out) -> int {
        timespec ts;
        int r = clock_gettime(clk, &ts);
        if (r == 0 && out) put_timespec(out, ts.tv_sec, ts.tv_nsec);
        return r;
    });
    reg("clock_getres", +[](int clk, u32 out) -> int {
        timespec ts;
        int r = clock_getres(clk, &ts);
        if (r == 0 && out) put_timespec(out, ts.tv_sec, ts.tv_nsec);
        return r;
    });
    reg("gmtime_r", +[](u32 tp, u32 out) -> u32 {
        __time64_t tt = static_cast<s32>(g::rd32(tp));
        struct tm tm;
        if (_gmtime64_s(&tm, &tt) != 0) return 0;
        put_tm(out, tm, 0, "GMT");
        return out;
    });
    reg("localtime_r", +[](u32 tp, u32 out) -> u32 {
        __time64_t tt = static_cast<s32>(g::rd32(tp));
        struct tm tm;
        if (_localtime64_s(&tm, &tt) != 0) return 0;
        put_tm(out, tm, local_gmtoff(tm.tm_isdst), _tzname[tm.tm_isdst > 0 ? 1 : 0]);
        return out;
    });
    reg("mktime", +[](u32 tp) -> s32 {
        struct tm tm = get_tm(tp);
        __time64_t r = _mktime64(&tm);
        put_tm(tp, tm, local_gmtoff(tm.tm_isdst), _tzname[tm.tm_isdst > 0 ? 1 : 0]);
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
        usleep(us);
        frame_sleep_end(us);
        return 0;
    });
    reg("sleep", +[](u32 s) -> u32 { return sleep(s); });
    reg("nanosleep", +[](u32 req, u32) -> int {
        timespec ts{static_cast<time_t>(static_cast<s32>(g::rd32(req))), static_cast<long>(g::rd32(req + 4))};
        return nanosleep(&ts, nullptr);
    });
    reg("nice", +[](int) -> int { return 0; });
    reg("sched_yield", +[]() -> int { return sched_yield(); });

    // process
    reg("getpid", +[]() -> int { return _getpid(); });
    reg("gettid", +[]() -> int { return gettid(); });
    reg("getuid", +[]() -> u32 { return 10123; });
    reg_raw("abort", [](GuestThread& t) { fatal("guest called abort() from %s", elf_describe(t.regs()[14]).c_str()); });
    reg("exit", +[](int code) {
        LOGI("guest called exit(%d)", code);
        exit(code);
    });
    reg("_exit", +[](int code) { _exit(code); });
    reg("getenv", +[](const char* name) -> u32 {
        static std::mutex m;
        static std::unordered_map<std::string, u32> cache;
        auto it = guest_env().find(name);
        if (it == guest_env().end()) return 0;
        std::lock_guard lk(m);
        u32& a = cache[name];
        if (!a) a = g::data_str(it->second.c_str());
        return a;
    });
    reg("uname", +[](u32 u) -> int {
        // struct utsname: six 65-byte fields
        const char* f[6] = {"Linux", "localhost", "4.19.113", "#1 SMP PREEMPT", "armv8l", "(none)"};
        memset(g::mem + u, 0, 6 * 65);
        for (int i = 0; i < 6; i++) strncpy(reinterpret_cast<char*>(g::mem + u + 65 * i), f[i], 64);
        return 0;
    });
    reg("gethostname", +[](char* b, u32 n) -> int { return gethostname(b, static_cast<int>(n)) == 0 ? 0 : fail_wsa(); });
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
    reg("arc4random", +[]() -> u32 {
        static std::mutex m;
        static std::random_device rd;  // the OS generator (rand_s) in this CRT
        std::lock_guard lk(m);
        return static_cast<u32>(rd());
    });
    reg("lrand48", +[]() -> s32 { return lrand48_next(); });
    reg("srand48", +[](s32 s) {
        std::lock_guard lk(rand48_lock);
        rand48_state = (static_cast<u64>(static_cast<u32>(s)) << 16) | 0x330E;
    });
    reg("rand", +[]() -> int { return lrand48_next() & 0x7fffffff; });
    reg("srand", +[](u32 s) {
        std::lock_guard lk(rand48_lock);
        rand48_state = (static_cast<u64>(s) << 16) | 0x330E;
    });
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
        g::wr32(a, static_cast<u32>(s));
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
        HANDLE h = CreateThread(nullptr, 1 << 20, thread_entry, si, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
        if (!h) return lx::L_EAGAIN;
        CloseHandle(h);
        return 0;
    });
    reg("pthread_join", +[](u32 id, u32 out) -> int {
        std::shared_ptr<JoinState> js;
        {
            std::lock_guard lk(join_lock);
            auto it = joins.find(id);
            if (it == joins.end()) return 3;  // ESRCH
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
                g::wr32(out, static_cast<u32>(k));
                return 0;
            }
        }
        return lx::L_EAGAIN;
    });
    reg("pthread_key_delete", +[](u32 k) -> int {
        if (k >= 128) return lx::L_EINVAL;
        std::lock_guard lk(key_lock);
        key_slots[k] = {};
        return 0;
    });
    {
        // pthread_getspecific(key) as guest code, as in the Android build (it is called very often)
        static_assert(GuestThread::KEYS_OFF < 0x1000, "ldr immediate");
        const u32 code[] = {0xe350007f, 0x83a00000, 0x812fff1e, 0xee1d1f70, 0xe0811100, 0xe5910000 | GuestThread::KEYS_OFF, 0xe12fff1e};
        add_guest_code("pthread_getspecific", code, 7);
    }
    reg_raw("pthread_setspecific", [](GuestThread& t) {
        u32 k = arg(t, 0);
        if (k >= 128) return ret(t, lx::L_EINVAL);
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
        int type = 0;
        if (attr) {
            u32 a = g::rd32(attr) & 3;
            type = a == 1 ? 1 : a == 2 ? 2 : 0;
        }
        g::wr32(m, 0);
        get_mutex(m, type);
        return 0;
    });
    reg("pthread_mutex_destroy", +[](u32 m) -> int {
        g::wr32(m, 0);  // the host mutex is left allocated: it may still be referenced
        return 0;
    });
    reg("pthread_mutex_lock", +[](u32 m) -> int { return mutex_lock(get_mutex(m)); });
    reg("pthread_mutex_unlock", +[](u32 m) -> int { return mutex_unlock(get_mutex(m)); });
    reg("pthread_mutex_trylock", +[](u32 m) -> int { return mutex_trylock(get_mutex(m)); });
    reg("pthread_mutexattr_init", +[](u32 a) -> int { g::wr32(a, 0); return 0; });
    reg("pthread_mutexattr_settype", +[](u32 a, int type) -> int { g::wr32(a, static_cast<u32>(type)); return 0; });
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
    reg("pthread_cond_wait", +[](u32 c, u32 m) -> int { return cond_wait(get_cond(c), get_mutex(m), INFINITE); });
    reg("pthread_cond_timedwait", +[](u32 c, u32 m, u32 abst) -> int {
        // absolute CLOCK_REALTIME deadline
        s64 want = static_cast<s64>(static_cast<s32>(g::rd32(abst))) * 1000000000ll + static_cast<s32>(g::rd32(abst + 4));
        timespec now;
        clock_gettime(CLOCK_REALTIME, &now);
        s64 left = want - (static_cast<s64>(now.tv_sec) * 1000000000ll + now.tv_nsec);
        DWORD ms = left <= 0 ? 0 : static_cast<DWORD>((left + 999999) / 1000000);
        frame_sleep_begin();
        int r = cond_wait(get_cond(c), get_mutex(m), ms);
        frame_sleep_end(1000000);
        return r;
    });
    reg("pthread_cond_signal", +[](u32 c) -> int { WakeConditionVariable(get_cond(c)); return 0; });
    reg("pthread_cond_broadcast", +[](u32 c) -> int { WakeAllConditionVariable(get_cond(c)); return 0; });

    // network
    reg("socket", +[](int d, int t, int p) -> int {
        bool nb = t & lx::L_SOCK_NONBLOCK_;
        t &= ~(lx::L_SOCK_NONBLOCK_ | lx::L_SOCK_CLOEXEC_);
        SOCKET s = ::socket(family_in(d), t, p);
        if (s == INVALID_SOCKET) return fail_wsa();
        if (nb) {
            u_long on = 1;
            ioctlsocket(s, FIONBIO, &on);
        }
        Fd f;
        f.kind = Fd::SOCK;
        f.sock = s;
        f.nonblock = nb;
        return fd_add(f);
    });
    reg("bind", +[](int fd, const void* a, u32 l) -> int {
        SOCKET s = sock_of(fd);
        if (s == INVALID_SOCKET) return fail(lx::L_ENOTSOCK);
        auto sa = sa_in(a, l);
        return ::bind(s, reinterpret_cast<const sockaddr*>(sa.data()), static_cast<int>(l)) == 0 ? 0 : fail_wsa();
    });
    reg("connect", +[](int fd, const void* a, u32 l) -> int {
        SOCKET s = sock_of(fd);
        if (s == INVALID_SOCKET) return fail(lx::L_ENOTSOCK);
        auto sa = sa_in(a, l);
        if (l >= sizeof(sockaddr_in) && (sa[0] | (sa[1] << 8)) == AF_INET) {  // (see lan_redirect_probe)
            auto* in = reinterpret_cast<sockaddr_in*>(sa.data());
            u32 ip = in->sin_addr.s_addr;
            if (lan_redirect_probe(ip, in->sin_port)) in->sin_addr.s_addr = ip;
        }
        if (::connect(s, reinterpret_cast<const sockaddr*>(sa.data()), static_cast<int>(l)) == 0) return 0;
        int e = WSAGetLastError();
        // a non-blocking connect: Windows says "would block", Linux "in progress"
        if (e == WSAEWOULDBLOCK) return fail(lx::L_EINPROGRESS);
        return fail(lx_from_wsa(e));
    });
    reg("listen", +[](int fd, int n) -> int {
        SOCKET s = sock_of(fd);
        if (s == INVALID_SOCKET) return fail(lx::L_ENOTSOCK);
        return ::listen(s, n) == 0 ? 0 : fail_wsa();
    });
    reg("accept", +[](int fd, void* a, u32* l) -> int {
        SOCKET s = sock_of(fd);
        if (s == INVALID_SOCKET) return fail(lx::L_ENOTSOCK);
        sockaddr_storage ss{};
        int len = sizeof ss;
        SOCKET c = ::accept(s, reinterpret_cast<sockaddr*>(&ss), &len);
        if (c == INVALID_SOCKET) return fail_wsa();
        if (ss.ss_family == AF_INET) lan_note_peer(reinterpret_cast<sockaddr_in*>(&ss)->sin_addr.s_addr);
        if (a && l) {
            u32 n = std::min<u32>(*l, static_cast<u32>(len));
            memcpy(a, &ss, n);
            sa_out(a, n);
            *l = static_cast<u32>(len);
        }
        Fd f;
        f.kind = Fd::SOCK;
        f.sock = c;
        return fd_add(f);
    });
    reg("recv", +[](int fd, void* b, u32 n, int fl) -> s32 {
        SOCKET s = sock_of(fd);
        if (s == INVALID_SOCKET) return fail(lx::L_ENOTSOCK);
        bool dw;
        int wf = msg_flags(fl, dw);
        int r = with_dontwait(fd, s, dw, [&] { return ::recv(s, static_cast<char*>(b), static_cast<int>(n), wf); });
        return r >= 0 ? r : fail_wsa();
    });
    reg("send", +[](int fd, const void* b, u32 n, int fl) -> s32 {
        SOCKET s = sock_of(fd);
        if (s == INVALID_SOCKET) return fail(lx::L_ENOTSOCK);
        bool dw;
        int wf = msg_flags(fl, dw);
        int r = with_dontwait(fd, s, dw, [&] { return ::send(s, static_cast<const char*>(b), static_cast<int>(n), wf); });
        return r >= 0 ? r : fail_wsa();
    });
    reg("recvfrom", +[](int fd, void* b, u32 n, int fl, void* a, u32* l) -> s32 {
        SOCKET s = sock_of(fd);
        if (s == INVALID_SOCKET) return fail(lx::L_ENOTSOCK);
        bool dw;
        int wf = msg_flags(fl, dw);
        sockaddr_storage ss{};
        int len = sizeof ss;
        int r = with_dontwait(fd, s, dw, [&] {
            return ::recvfrom(s, static_cast<char*>(b), static_cast<int>(n), wf, reinterpret_cast<sockaddr*>(&ss), &len);
        });
        if (r < 0) {
            // a UDP datagram bigger than the buffer: Linux truncates silently
            if (WSAGetLastError() == WSAEMSGSIZE) r = static_cast<int>(n);
            else return fail_wsa();
        }
        if (a && l) {
            u32 cp = std::min<u32>(*l, static_cast<u32>(len));
            memcpy(a, &ss, cp);
            sa_out(a, cp);
            *l = static_cast<u32>(len);
        }
        return r;
    });
    reg("sendto", +[](int fd, const void* b, u32 n, int fl, const void* a, u32 l) -> s32 {
        SOCKET s = sock_of(fd);
        if (s == INVALID_SOCKET) return fail(lx::L_ENOTSOCK);
        bool dw;
        int wf = msg_flags(fl, dw);
        std::vector<u8> sa = a ? sa_in(a, l) : std::vector<u8>{};
        int r = with_dontwait(fd, s, dw, [&] {
            return ::sendto(s, static_cast<const char*>(b), static_cast<int>(n), wf,
                            a ? reinterpret_cast<const sockaddr*>(sa.data()) : nullptr, a ? static_cast<int>(l) : 0);
        });
        return r >= 0 ? r : fail_wsa();
    });
    reg("getpeername", +[](int fd, void* a, u32* l) -> int {
        SOCKET s = sock_of(fd);
        if (s == INVALID_SOCKET) return fail(lx::L_ENOTSOCK);
        sockaddr_storage ss{};
        int len = sizeof ss;
        if (::getpeername(s, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return fail_wsa();
        u32 cp = std::min<u32>(*l, static_cast<u32>(len));
        memcpy(a, &ss, cp);
        sa_out(a, cp);
        *l = static_cast<u32>(len);
        return 0;
    });
    reg("getsockname", +[](int fd, void* a, u32* l) -> int {
        SOCKET s = sock_of(fd);
        if (s == INVALID_SOCKET) return fail(lx::L_ENOTSOCK);
        sockaddr_storage ss{};
        int len = sizeof ss;
        if (::getsockname(s, reinterpret_cast<sockaddr*>(&ss), &len) != 0) return fail_wsa();
        u32 cp = std::min<u32>(*l, static_cast<u32>(len));
        memcpy(a, &ss, cp);
        sa_out(a, cp);
        *l = static_cast<u32>(len);
        return 0;
    });
    reg("setsockopt", +[](int fd, int lvl, int name, const void* v, u32 len) -> int {
        SOCKET s = sock_of(fd);
        if (s == INVALID_SOCKET) return fail(lx::L_ENOTSOCK);
        int wl, wn;
        if (!sockopt_map(lvl, name, wl, wn)) {
            LOGW("guest setsockopt(%d, %d) not supported; ignored", lvl, name);
            return 0;
        }
        // Linux's SO_REUSEADDR only lets a port be bound again while old connections linger; Windows' lets a
        // second socket take a port that is in use, so two copies of the game would share the game port. Windows
        // allows the Linux case by default, so the option is dropped.
        if (wl == SOL_SOCKET && wn == SO_REUSEADDR) return 0;
        if (wl == SOL_SOCKET && (wn == SO_RCVTIMEO || wn == SO_SNDTIMEO) && len >= 8) {
            const u32* w = static_cast<const u32*>(v);
            DWORD ms = w[0] * 1000 + w[1] / 1000;
            return ::setsockopt(s, wl, wn, reinterpret_cast<const char*>(&ms), sizeof ms) == 0 ? 0 : fail_wsa();
        }
        if (wl == SOL_SOCKET && wn == SO_LINGER && len >= 8) {
            const s32* w = static_cast<const s32*>(v);
            linger lg{static_cast<u_short>(w[0] != 0), static_cast<u_short>(w[1])};
            return ::setsockopt(s, wl, wn, reinterpret_cast<const char*>(&lg), sizeof lg) == 0 ? 0 : fail_wsa();
        }
        if (wl == IPPROTO_IP && (wn == IP_MULTICAST_TTL || wn == IP_MULTICAST_LOOP) && len == 1) {
            DWORD d = *static_cast<const u8*>(v);
            return ::setsockopt(s, wl, wn, reinterpret_cast<const char*>(&d), sizeof d) == 0 ? 0 : fail_wsa();
        }
        return ::setsockopt(s, wl, wn, static_cast<const char*>(v), static_cast<int>(len)) == 0 ? 0 : fail_wsa();
    });
    reg("getsockopt", +[](int fd, int lvl, int name, void* v, u32* len) -> int {
        SOCKET s = sock_of(fd);
        if (s == INVALID_SOCKET) return fail(lx::L_ENOTSOCK);
        int wl, wn;
        if (!sockopt_map(lvl, name, wl, wn)) return fail(lx::L_EINVAL);
        if (wl == SOL_SOCKET && (wn == SO_RCVTIMEO || wn == SO_SNDTIMEO)) {
            DWORD ms = 0;
            int l = sizeof ms;
            if (::getsockopt(s, wl, wn, reinterpret_cast<char*>(&ms), &l) != 0) return fail_wsa();
            u32* w = static_cast<u32*>(v);
            w[0] = ms / 1000;
            w[1] = (ms % 1000) * 1000;
            *len = 8;
            return 0;
        }
        if (wl == SOL_SOCKET && wn == SO_ERROR) {
            int e = 0, l = sizeof e;
            if (::getsockopt(s, wl, wn, reinterpret_cast<char*>(&e), &l) != 0) return fail_wsa();
            *static_cast<s32*>(v) = e ? lx_from_wsa(e) : 0;
            *len = 4;
            return 0;
        }
        if (wl == SOL_SOCKET && wn == SO_LINGER) {
            linger lg{};
            int l = sizeof lg;
            if (::getsockopt(s, wl, wn, reinterpret_cast<char*>(&lg), &l) != 0) return fail_wsa();
            s32* w = static_cast<s32*>(v);
            w[0] = lg.l_onoff;
            w[1] = lg.l_linger;
            *len = 8;
            return 0;
        }
        int l = static_cast<int>(*len);
        if (::getsockopt(s, wl, wn, static_cast<char*>(v), &l) != 0) return fail_wsa();
        *len = static_cast<u32>(l);
        return 0;
    });
    reg("select", +[](int n, u32 r, u32 w, u32 e, u32 tvp) -> int {
        SelSet rs, ws, es;
        int files = 0;
        bool any = sel_in(r, n, rs, files);
        any = sel_in(w, n, ws, files) || any;
        any = sel_in(e, n, es, files) || any;
        timeval tv{}, *ptv = nullptr;
        if (tvp) {
            tv = {static_cast<long>(g::rd32(tvp)), static_cast<long>(g::rd32(tvp + 4))};
            ptv = &tv;
        }
        if (!any) {
            if (files) return files;
            if (ptv) usleep(static_cast<unsigned>(tv.tv_sec * 1000000 + tv.tv_usec));
            return 0;
        }
        if (files) tv = {0, 0}, ptv = &tv;  // don't wait when a file is "ready"
        frame_sleep_begin();
        int res = ::select(0, rs.map.empty() ? nullptr : &rs.w, ws.map.empty() ? nullptr : &ws.w,
                           es.map.empty() ? nullptr : &es.w, ptv);
        frame_sleep_end(ptv ? static_cast<u32>(tv.tv_sec * 1000000 + tv.tv_usec) : 1000000);
        if (res < 0) return fail_wsa();
        int count = sel_out(r, n, rs) + sel_out(w, n, ws) + sel_out(e, n, es);
        if (tvp) g::wr64(tvp, 0);
        return count + files;
    });
    reg("getaddrinfo", +[](const char* node, const char* svc, u32 hints, u32 out) -> int {
        addrinfo h{}, *res = nullptr;
        if (hints) {
            h.ai_flags = static_cast<int>(g::rd32(hints));
            h.ai_family = family_in(static_cast<int>(g::rd32(hints + 4)));
            h.ai_socktype = static_cast<int>(g::rd32(hints + 8));
            h.ai_protocol = static_cast<int>(g::rd32(hints + 12));
        }
        int r = ::getaddrinfo(node, svc, hints ? &h : nullptr, &res);
        if (r == 0) {
            g::wr32(out, put_addrinfo(res));
            freeaddrinfo(res);
            return 0;
        }
        return 8;  // EAI_NONAME (bionic)
    });
    reg("freeaddrinfo", +[](u32 ai) {
        while (ai) {
            u32 next = g::rd32(ai + 28);
            if (u32 cn = g::rd32(ai + 20)) g::free(cn);
            g::free(ai);
            ai = next;
        }
    });
    reg("inet_aton", +[](const char* s, u32* a) -> int { return inet_pton(AF_INET, s, a) == 1; });
    reg("inet_pton", +[](int af, const char* s, void* d) -> int { return inet_pton(family_in(af), s, d); });
    reg("inet_ntop", +[](int af, const void* s, char* d, u32 n) -> char* {
        return const_cast<char*>(inet_ntop(family_in(af), s, d, n));
    });
    reg("inet_ntoa", +[](u32 a) -> u32 {
        char b[INET_ADDRSTRLEN] = {};
        in_addr ia;
        ia.s_addr = a;
        inet_ntop(AF_INET, &ia, b, sizeof b);
        return scratch_str(b);
    });

    // libdl
    reg("dlopen", +[](const char* path, int) -> u32 {
        if (!path) return H_GLOBAL;
        std::string bn = base_name(path);
        if (is_host_lib(bn)) return H_HOSTLIB;
        if (GuestLib* l = elf_find(bn)) return l->base;
        std::string file;
        for (const std::string& c : {g_libdir + "/" + bn, boz_host_path(path)}) {
            if (is_guest_elf(c)) {
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
        int r = zl.inflate(z, flush);
        zs_out(gs, z);
        return r;
    });
    reg("inflateReset", +[](u32 gs) -> int {
        z_stream* z = zs_in(gs);
        if (!z) return Z_STREAM_ERROR;
        int r = zl.inflateReset(z);
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
        int r = zl.inflateEnd(z);
        delete z;
        g::wr32(gs + 28, 0);
        return r;
    });
}
