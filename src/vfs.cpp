#include "vfs.h"

#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {
std::string apk_path;
std::vector<Bundled> bundled;
std::mutex fd_lock;
std::unordered_map<int, const Bundled*> open_fds;

u16 le16(const u8* p) { return static_cast<u16>(p[0] | (p[1] << 8)); }
u32 le32(const u8* p) { return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<u32>(p[3]) << 24); }

bool read_at(int fd, void* buf, size_t n, off64_t off) {
    return pread64(fd, buf, n, off) == static_cast<ssize_t>(n);
}

// Reads the APK's zip directory and records the stored entries under assets/boz64/.
void scan_apk() {
    int fd = open(apk_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        LOGW("cannot open %s to look for bundled game data", apk_path.c_str());
        return;
    }
    off64_t size = lseek64(fd, 0, SEEK_END);
    size_t tail = static_cast<size_t>(std::min<off64_t>(size, 65536 + 22));
    std::vector<u8> t(tail);
    if (!read_at(fd, t.data(), tail, size - tail)) {
        close(fd);
        return;
    }
    long eocd = -1;
    for (long i = static_cast<long>(tail) - 22; i >= 0; i--) {
        if (le32(&t[i]) == 0x06054b50) {
            eocd = i;
            break;
        }
    }
    if (eocd < 0) {
        close(fd);
        return;
    }
    u32 count = le16(&t[eocd + 10]), cd_size = le32(&t[eocd + 12]), cd_off = le32(&t[eocd + 16]);
    std::vector<u8> cd(cd_size);
    if (!read_at(fd, cd.data(), cd_size, cd_off)) {
        close(fd);
        return;
    }
    const std::string prefix = "assets/boz64/";
    size_t p = 0;
    for (u32 i = 0; i < count && p + 46 <= cd.size(); i++) {
        const u8* e = &cd[p];
        if (le32(e) != 0x02014b50) break;
        u16 method = le16(e + 10), nlen = le16(e + 28), xlen = le16(e + 30), clen = le16(e + 32);
        u32 usize = le32(e + 24), lho = le32(e + 42);
        std::string name(reinterpret_cast<const char*>(e + 46), nlen);
        p += 46 + nlen + xlen + clen;
        if (name.rfind(prefix, 0) != 0 || name.size() == prefix.size()) continue;
        if (method != 0) {
            LOGW("bundled %s is compressed; it must be stored", name.c_str());
            continue;
        }
        u8 lh[30];
        if (!read_at(fd, lh, sizeof lh, lho) || le32(lh) != 0x04034b50) continue;
        Bundled b;
        b.name = name.substr(prefix.size());
        b.off = lho + 30 + le16(lh + 26) + le16(lh + 28);
        b.size = usize;
        LOGI("bundled game data: %s (%llu MB)", b.name.c_str(), static_cast<unsigned long long>(b.size >> 20));
        bundled.push_back(b);
    }
    close(fd);
}

const Bundled* find(const std::string& name) {
    for (const auto& b : bundled)
        if (b.name == name) return &b;
    return nullptr;
}
}  // namespace

void vfs_init(const std::string& libdir) {
    // libdir is <app>/lib/arm64; the APK is <app>/base.apk
    size_t cut = libdir.rfind("/lib/");
    apk_path = (cut == std::string::npos ? libdir : libdir.substr(0, cut)) + "/base.apk";
    scan_apk();
}

const Bundled* vfs_lookup(const char* path) {
    if (bundled.empty() || !path || !strstr(path, "/Android/obb/")) return nullptr;
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    // The game asks for blackops_<texture format>.dz by GPU (atitc, etc, dxt, pvrtc). The bundled pack
    // works on any GPU: each texture in it records its own format, and ETC is readable everywhere.
    if (strncmp(base, "blackops_", 9) != 0) return nullptr;
    const char* dot = strchr(base + 9, '.');
    if (!dot) return nullptr;
    const Bundled* b = nullptr;
    if (strcmp(dot, ".dz") == 0)
        b = find("blackops.dz");
    else if (strcmp(dot, ".dz.dat") == 0)
        b = find("blackops.dz.dat");
    if (!b) return nullptr;
    struct stat st;
    if (stat(path, &st) == 0) return nullptr;  // a real copy exists
    return b;
}

void vfs_stat(const Bundled* b, struct stat* st) {
    stat(apk_path.c_str(), st);
    st->st_mode = S_IFREG | 0444;
    st->st_size = static_cast<off_t>(b->size);
    st->st_blocks = static_cast<blkcnt_t>((b->size + 511) / 512);
    st->st_nlink = 1;
}

int vfs_open(const Bundled* b) {
    int fd = open(apk_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    lseek64(fd, static_cast<off64_t>(b->off), SEEK_SET);
    std::lock_guard lk(fd_lock);
    open_fds[fd] = b;
    return fd;
}

static const Bundled* fd_entry(int fd) {
    std::lock_guard lk(fd_lock);
    auto it = open_fds.find(fd);
    return it == open_fds.end() ? nullptr : it->second;
}

bool vfs_is_fd(int fd) { return fd_entry(fd) != nullptr; }

void vfs_close(int fd) {
    std::lock_guard lk(fd_lock);
    open_fds.erase(fd);
}

s64 vfs_lseek(int fd, s64 off, int whence) {
    const Bundled* b = fd_entry(fd);
    if (!b) return -2;
    s64 cur = lseek64(fd, 0, SEEK_CUR) - static_cast<s64>(b->off);
    s64 pos = whence == SEEK_SET ? off : whence == SEEK_CUR ? cur + off : static_cast<s64>(b->size) + off;
    if (pos < 0) {
        errno = EINVAL;
        return -1;
    }
    lseek64(fd, static_cast<off64_t>(b->off + pos), SEEK_SET);
    return pos;
}

s64 vfs_read_limit(int fd, u64 n) {
    const Bundled* b = fd_entry(fd);
    if (!b) return -2;
    s64 cur = lseek64(fd, 0, SEEK_CUR) - static_cast<s64>(b->off);
    s64 left = static_cast<s64>(b->size) - cur;
    return left <= 0 ? 0 : std::min<s64>(left, static_cast<s64>(n));
}

s64 vfs_pread(int fd, void* buf, u64 n, s64 off) {
    const Bundled* b = fd_entry(fd);
    if (!b) return -2;
    if (off < 0) {
        errno = EINVAL;
        return -1;
    }
    s64 left = static_cast<s64>(b->size) - off;
    if (left <= 0) return 0;
    return pread64(fd, buf, std::min<u64>(n, static_cast<u64>(left)), static_cast<off64_t>(b->off + off));
}

bool vfs_fix_fstat(int fd, struct stat* st) {
    const Bundled* b = fd_entry(fd);
    if (!b) return false;
    st->st_mode = S_IFREG | 0444;
    st->st_size = static_cast<off_t>(b->size);
    st->st_blocks = static_cast<blkcnt_t>((b->size + 511) / 512);
    return true;
}

// FILE* over a bundled file.
namespace {
struct Cookie {
    int fd;
    const Bundled* b;
    s64 pos;
};
int ck_read(void* c, char* buf, int n) {
    auto* k = static_cast<Cookie*>(c);
    s64 left = static_cast<s64>(k->b->size) - k->pos;
    if (left <= 0) return 0;
    ssize_t r = pread64(k->fd, buf, std::min<s64>(left, n), static_cast<off64_t>(k->b->off + k->pos));
    if (r > 0) k->pos += r;
    return static_cast<int>(r);
}
fpos64_t ck_seek(void* c, fpos64_t off, int whence) {
    auto* k = static_cast<Cookie*>(c);
    s64 pos = whence == SEEK_SET ? off : whence == SEEK_CUR ? k->pos + off : static_cast<s64>(k->b->size) + off;
    if (pos < 0) {
        errno = EINVAL;
        return -1;
    }
    k->pos = pos;
    return pos;
}
int ck_close(void* c) {
    auto* k = static_cast<Cookie*>(c);
    close(k->fd);
    delete k;
    return 0;
}
}  // namespace

FILE* vfs_fopen(const Bundled* b) {
    int fd = open(apk_path.c_str(), O_RDONLY | O_CLOEXEC);
    if (fd < 0) return nullptr;
    auto* k = new Cookie{fd, b, 0};
    FILE* f = funopen64(k, ck_read, nullptr, ck_seek, ck_close);
    if (!f) {
        close(fd);
        delete k;
    }
    return f;
}
