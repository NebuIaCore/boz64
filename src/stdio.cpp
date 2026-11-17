// stdio for guest code: FILE handles, printf and scanf.
#include <unistd.h>

#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <string>
#include <unordered_map>

#include "cpu.h"
#include "vfs.h"

namespace {
constexpr u32 GUEST_FILE_SIZE = 84;  // sizeof(FILE) in 32-bit bionic, so &__sF[n] arithmetic works
u32 sF = 0;
std::mutex file_lock;
std::unordered_map<u32, FILE*> files;
std::string log_line[2];
std::mutex log_lock;

FILE* hf(u32 gf) {
    if (!gf) return nullptr;
    if (gf >= sF && gf < sF + 3 * GUEST_FILE_SIZE) {
        u32 i = (gf - sF) / GUEST_FILE_SIZE;
        return i == 0 ? stdin : i == 1 ? stdout : stderr;
    }
    std::lock_guard lk(file_lock);
    auto it = files.find(gf);
    return it == files.end() ? nullptr : it->second;
}

u32 wrap_file(FILE* f) {
    if (!f) return 0;
    u32 gf = g::calloc(1, GUEST_FILE_SIZE);
    std::lock_guard lk(file_lock);
    files[gf] = f;
    return gf;
}

// Android sends stdout/stderr nowhere, so the guest's console output goes to logcat line by line.
size_t emit(FILE* f, const char* data, size_t n) {
    if (f != stdout && f != stderr) return fwrite(data, 1, n, f);
    std::lock_guard lk(log_lock);
    std::string& line = log_line[f == stderr];
    for (size_t i = 0; i < n; i++) {
        if (data[i] == '\n') {
            __android_log_print(ANDROID_LOG_INFO, "boz64-guest", "%s", line.c_str());
            line.clear();
        } else {
            line += data[i];
        }
    }
    return n;
}

const char* FAKE_CPUINFO_HEAD =
    "processor\t: %d\n"
    "BogoMIPS\t: 38.40\n"
    "Features\t: half thumb fastmult vfp edsp neon vfpv3 tls vfpv4 idiva idivt lpae evtstrm aes pmull sha1 sha2 "
    "crc32\n"
    "CPU implementer\t: 0x41\n"
    "CPU architecture: 8\n"
    "CPU variant\t: 0x1\n"
    "CPU part\t: 0xd05\n"
    "CPU revision\t: 0\n\n";
}  // namespace

// What /proc/cpuinfo looks like to a 32-bit app on this kind of phone.
std::string fake_cpuinfo() {
    std::string s;
    int n = sysconf(_SC_NPROCESSORS_CONF);
    for (int i = 0; i < n; i++) {
        char buf[512];
        snprintf(buf, sizeof buf, FAKE_CPUINFO_HEAD, i);
        s += buf;
    }
    s += "Hardware\t: Qualcomm Technologies, Inc\nRevision\t: 0000\nSerial\t\t: 0000000000000000\n";
    return s;
}

// ---- printf ----

struct VaSource {
    ArgCursor* c = nullptr;  // "..." arguments
    u32 ap = 0;              // or a guest va_list
    u32 word() {
        if (c) return c->word();
        u32 v = g::rd32(ap);
        ap += 4;
        return v;
    }
    u64 dword() {
        if (c) return c->dword();
        ap = (ap + 7) & ~7u;
        u64 v = g::rd64(ap);
        ap += 8;
        return v;
    }
};

std::string guest_format(const char* fmt, VaSource& va) {
    std::string out;
    char piece[64], tmp[512];
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            out += *p++;
            continue;
        }
        const char* start = p++;
        if (*p == '%') {
            out += '%';
            p++;
            continue;
        }
        std::string spec = "%";
        while (*p && strchr("-+ #0'", *p)) spec += *p++;
        if (*p == '*') {
            spec += std::to_string(static_cast<s32>(va.word()));
            p++;
        } else {
            while (*p >= '0' && *p <= '9') spec += *p++;
        }
        if (*p == '.') {
            spec += *p++;
            if (*p == '*') {
                spec += std::to_string(static_cast<s32>(va.word()));
                p++;
            } else {
                while (*p >= '0' && *p <= '9') spec += *p++;
            }
        }
        int len = 0;  // 0 int, 1 char, 2 short, 3 long, 4 long long
        for (;;) {
            if (*p == 'h') {
                len = (len == 2) ? 1 : 2;
                p++;
            } else if (*p == 'l') {
                len = (len == 3) ? 4 : 3;
                p++;
            } else if (*p == 'q' || *p == 'j' || *p == 'L') {
                len = 4;
                p++;
            } else if (*p == 'z' || *p == 't') {
                len = 3;
                p++;
            } else {
                break;
            }
        }
        char conv = *p ? *p++ : 0;
        switch (conv) {
        case 'd':
        case 'i': {
            s64 v = len == 4 ? static_cast<s64>(va.dword()) : static_cast<s32>(va.word());
            if (len == 1) v = static_cast<s8>(v);
            if (len == 2) v = static_cast<s16>(v);
            snprintf(piece, sizeof piece, "%slld", spec.c_str());
            snprintf(tmp, sizeof tmp, piece, static_cast<long long>(v));
            out += tmp;
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            u64 v = len == 4 ? va.dword() : va.word();
            if (len == 1) v = static_cast<u8>(v);
            if (len == 2) v = static_cast<u16>(v);
            snprintf(piece, sizeof piece, "%sll%c", spec.c_str(), conv);
            snprintf(tmp, sizeof tmp, piece, static_cast<unsigned long long>(v));
            out += tmp;
            break;
        }
        case 'c': {
            snprintf(piece, sizeof piece, "%sc", spec.c_str());
            snprintf(tmp, sizeof tmp, piece, static_cast<int>(va.word()));
            out += tmp;
            break;
        }
        case 's': {
            u32 sp = va.word();
            const char* s = sp ? g::str(sp) : "(null)";
            snprintf(piece, sizeof piece, "%ss", spec.c_str());
            int n = snprintf(nullptr, 0, piece, s);
            std::string buf(n + 1, '\0');
            snprintf(buf.data(), n + 1, piece, s);
            buf.resize(n);
            out += buf;
            break;
        }
        case 'p': {
            char hex[16];
            snprintf(hex, sizeof hex, "0x%x", va.word());
            snprintf(piece, sizeof piece, "%ss", spec.c_str());
            snprintf(tmp, sizeof tmp, piece, hex);
            out += tmp;
            break;
        }
        case 'f':
        case 'F':
        case 'e':
        case 'E':
        case 'g':
        case 'G':
        case 'a':
        case 'A': {
            double d = std::bit_cast<double>(va.dword());
            snprintf(piece, sizeof piece, "%s%c", spec.c_str(), conv);
            snprintf(tmp, sizeof tmp, piece, d);
            out += tmp;
            break;
        }
        case 'n': {
            u32 ptr = va.word();
            if (ptr) {
                if (len == 4)
                    g::wr64(ptr, out.size());
                else
                    g::wr32(ptr, static_cast<u32>(out.size()));
            }
            break;
        }
        default:  // unknown: copy it through
            out.append(start, p - start);
            break;
        }
    }
    return out;
}

static u32 put_bounded(u32 buf, u32 size, const std::string& s) {
    if (size) {
        u32 n = std::min<u32>(s.size(), size - 1);
        memcpy(g::mem + buf, s.data(), n);
        g::wr8(buf + n, 0);
    }
    return static_cast<u32>(s.size());
}

// ---- scanf ----
// Rewrites the guest format for the host's type sizes, then calls the host scanf with pointers
// straight into guest memory.
static std::string scan_format(const char* fmt, ArgCursor& c, std::vector<void*>& ptrs) {
    std::string f;
    for (const char* p = fmt; *p;) {
        if (*p != '%') {
            f += *p++;
            continue;
        }
        f += *p++;
        if (*p == '%') {
            f += *p++;
            continue;
        }
        bool suppress = false;
        if (*p == '*') {
            suppress = true;
            f += *p++;
        }
        while (*p >= '0' && *p <= '9') f += *p++;
        std::string len;
        while (*p && strchr("hlLqjzt", *p)) len += *p++;
        char conv = *p;
        if (!conv) break;
        p++;
        std::string hostlen = len;
        if (strchr("diouxXn", conv)) {
            if (len == "l" || len == "z" || len == "t") hostlen = "";
            if (len == "q" || len == "j" || len == "L") hostlen = "ll";
        } else if (strchr("feEgGaA", conv)) {
            if (len == "L") hostlen = "l";
        } else if (conv == 'p') {
            conv = 'x';
            hostlen = "";
        }
        f += hostlen;
        f += conv;
        if (conv == '[') {
            if (*p == '^') f += *p++;
            if (*p == ']') f += *p++;
            while (*p && *p != ']') f += *p++;
            if (*p) f += *p++;
        }
        if (!suppress) ptrs.push_back(g::h(c.word()));
    }
    return f;
}

#define PTRS40(v)                                                                                            \
    v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11], v[12], v[13], v[14], v[15], \
        v[16], v[17], v[18], v[19], v[20], v[21], v[22], v[23], v[24], v[25], v[26], v[27], v[28], v[29],   \
        v[30], v[31], v[32], v[33], v[34], v[35], v[36], v[37], v[38], v[39]

static void do_scan(GuestThread& t, int first_vararg, const char* src, FILE* file) {
    ArgCursor c{t, first_vararg};
    std::vector<void*> ptrs;
    std::string f = scan_format(g::str(arg(t, first_vararg - 1)), c, ptrs);
    if (ptrs.size() > 40) fatal("scanf with more than 40 conversions");
    ptrs.resize(40, nullptr);
    int r = src ? sscanf(src, f.c_str(), PTRS40(ptrs)) : fscanf(file, f.c_str(), PTRS40(ptrs));
    ret(t, static_cast<u32>(r));
}

void register_stdio() {
    sF = g::data_alloc(3 * GUEST_FILE_SIZE, 8);
    add_data_symbol("__sF", sF);

    reg("fopen", +[](const char* path, const char* mode) -> u32 {
        if (strcmp(path, "/proc/cpuinfo") == 0) {
            static std::string info = fake_cpuinfo();
            return wrap_file(fmemopen(info.data(), info.size(), "r"));
        }
        if (!strpbrk(mode, "wa+")) {
            if (const Bundled* b = vfs_lookup(path)) return wrap_file(vfs_fopen(b));
        }
#ifdef BOZ_PC
        if (strncmp(path, "/proc/", 6) == 0) {
            std::string c = boz_fake_proc(path);
            if (!c.empty()) return wrap_file(fmemopen(c.data(), c.size(), "r"));
        }
        // Android paths map into the PC build's data folder; Linux has no text mode, so files are always binary.
        std::string m = mode;
        if (m.find('b') == std::string::npos) m += 'b';
        return wrap_file(fopen(boz_host_path(path).c_str(), m.c_str()));
#else
        return wrap_file(fopen(path, mode));
#endif
    });
    reg("fdopen", +[](int fd, const char* mode) -> u32 { return wrap_file(fdopen(fd, mode)); });
    reg("tmpfile", +[]() -> u32 { return wrap_file(tmpfile()); });
    reg("fclose", +[](u32 gf) -> int {
        FILE* f = hf(gf);
        if (!f) return -1;
        if (f == stdin || f == stdout || f == stderr) return 0;
        {
            std::lock_guard lk(file_lock);
            files.erase(gf);
        }
        g::free(gf);
        return fclose(f);
    });
    reg("fread", +[](void* buf, u32 size, u32 n, u32 gf) -> u32 {
        FILE* f = hf(gf);
        return f ? fread(buf, size, n, f) : 0;
    });
    reg("fwrite", +[](const void* buf, u32 size, u32 n, u32 gf) -> u32 {
        FILE* f = hf(gf);
        if (!f || !size) return 0;
        return emit(f, static_cast<const char*>(buf), static_cast<size_t>(size) * n) / size;
    });
    reg("fgets", +[](char* buf, int n, u32 gf) -> char* {
        FILE* f = hf(gf);
        return f ? fgets(buf, n, f) : nullptr;
    });
    reg("fgetc", +[](u32 gf) -> int { FILE* f = hf(gf); return f ? fgetc(f) : EOF; });
    reg("getc", +[](u32 gf) -> int { FILE* f = hf(gf); return f ? getc(f) : EOF; });
    reg("ungetc", +[](int ch, u32 gf) -> int { FILE* f = hf(gf); return f ? ungetc(ch, f) : EOF; });
    reg("fputs", +[](const char* s, u32 gf) -> int {
        FILE* f = hf(gf);
        return f ? static_cast<int>(emit(f, s, strlen(s))) : EOF;
    });
    reg("puts", +[](const char* s) -> int {
        emit(stdout, s, strlen(s));
        emit(stdout, "\n", 1);
        return 1;
    });
    reg("fputc", +[](int ch, u32 gf) -> int {
        FILE* f = hf(gf);
        char c = static_cast<char>(ch);
        return f && emit(f, &c, 1) ? (ch & 0xff) : EOF;
    });
    reg("putc", +[](int ch, u32 gf) -> int {
        FILE* f = hf(gf);
        char c = static_cast<char>(ch);
        return f && emit(f, &c, 1) ? (ch & 0xff) : EOF;
    });
    reg("putchar", +[](int ch) -> int {
        char c = static_cast<char>(ch);
        emit(stdout, &c, 1);
        return ch & 0xff;
    });
    reg("fseek", +[](u32 gf, s32 off, int whence) -> int { FILE* f = hf(gf); return f ? fseek(f, off, whence) : -1; });
    reg("ftell", +[](u32 gf) -> s32 { FILE* f = hf(gf); return f ? static_cast<s32>(ftell(f)) : -1; });
    reg("rewind", +[](u32 gf) { if (FILE* f = hf(gf)) rewind(f); });
    reg("fflush", +[](u32 gf) -> int { FILE* f = hf(gf); return f && f != stdout && f != stderr ? fflush(f) : 0; });
    reg("feof", +[](u32 gf) -> int { FILE* f = hf(gf); return f ? feof(f) : 1; });
    reg("ferror", +[](u32 gf) -> int { FILE* f = hf(gf); return f ? ferror(f) : 1; });
    reg("clearerr", +[](u32 gf) { if (FILE* f = hf(gf)) clearerr(f); });
    reg("fileno", +[](u32 gf) -> int { FILE* f = hf(gf); return f ? fileno(f) : -1; });
    reg("setvbuf", +[](u32, u32, int, u32) -> int { return 0; });

    reg_raw("sprintf", [](GuestThread& t) {
        ArgCursor c{t, 2};
        VaSource va{&c};
        std::string s = guest_format(g::str(arg(t, 1)), va);
        memcpy(g::mem + arg(t, 0), s.c_str(), s.size() + 1);
        ret(t, s.size());
    });
    reg_raw("snprintf", [](GuestThread& t) {
        ArgCursor c{t, 3};
        VaSource va{&c};
        ret(t, put_bounded(arg(t, 0), arg(t, 1), guest_format(g::str(arg(t, 2)), va)));
    });
    reg_raw("vsnprintf", [](GuestThread& t) {
        VaSource va{nullptr, arg(t, 3)};
        ret(t, put_bounded(arg(t, 0), arg(t, 1), guest_format(g::str(arg(t, 2)), va)));
    });
    reg_raw("vsprintf", [](GuestThread& t) {
        VaSource va{nullptr, arg(t, 2)};
        std::string s = guest_format(g::str(arg(t, 1)), va);
        memcpy(g::mem + arg(t, 0), s.c_str(), s.size() + 1);
        ret(t, s.size());
    });
    reg_raw("fprintf", [](GuestThread& t) {
        ArgCursor c{t, 2};
        VaSource va{&c};
        std::string s = guest_format(g::str(arg(t, 1)), va);
        FILE* f = hf(arg(t, 0));
        ret(t, f ? emit(f, s.data(), s.size()) : static_cast<u32>(-1));
    });
    reg_raw("vfprintf", [](GuestThread& t) {
        VaSource va{nullptr, arg(t, 2)};
        std::string s = guest_format(g::str(arg(t, 1)), va);
        FILE* f = hf(arg(t, 0));
        ret(t, f ? emit(f, s.data(), s.size()) : static_cast<u32>(-1));
    });
    reg_raw("printf", [](GuestThread& t) {
        ArgCursor c{t, 1};
        VaSource va{&c};
        std::string s = guest_format(g::str(arg(t, 0)), va);
        ret(t, emit(stdout, s.data(), s.size()));
    });
    reg_raw("vprintf", [](GuestThread& t) {
        VaSource va{nullptr, arg(t, 1)};
        std::string s = guest_format(g::str(arg(t, 0)), va);
        ret(t, emit(stdout, s.data(), s.size()));
    });
    reg_raw("__android_log_print", [](GuestThread& t) {
        ArgCursor c{t, 3};
        VaSource va{&c};
        std::string s = guest_format(g::str(arg(t, 2)), va);
        ret(t, __android_log_write(static_cast<int>(arg(t, 0)), arg(t, 1) ? g::str(arg(t, 1)) : "", s.c_str()));
    });
    reg_raw("__android_log_vprint", [](GuestThread& t) {
        VaSource va{nullptr, arg(t, 3)};
        std::string s = guest_format(g::str(arg(t, 2)), va);
        ret(t, __android_log_write(static_cast<int>(arg(t, 0)), arg(t, 1) ? g::str(arg(t, 1)) : "", s.c_str()));
    });
    reg_raw("sscanf", [](GuestThread& t) { do_scan(t, 2, g::str(arg(t, 0)), nullptr); });
    reg_raw("fscanf", [](GuestThread& t) {
        FILE* f = hf(arg(t, 0));
        if (!f) return ret(t, static_cast<u32>(EOF));
        do_scan(t, 2, nullptr, f);
    });
}
