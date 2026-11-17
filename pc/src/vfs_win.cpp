// Guest file paths on Windows. The game sees an Android file system: absolute paths ("/data/data/...",
// "/sdcard/Android/obb/...") live under the PC build's data folder (data/ next to the .exe, or debug.boz64.data),
// with the same layout. The bundled-data lookups of the Android build (vfs.h) are not used.
#include "vfs.h"

#include <sys/system_properties.h>

#include <cstring>
#include <mutex>
#include <string>

const std::string& boz_data_root() {
    static std::string root;  // function-local: may be needed during static initialisation
    static std::once_flag root_once;
    std::call_once(root_once, [] {
        char v[PROP_VALUE_MAX] = {};
        root = __system_property_get("debug.boz64.data", v) > 0 ? std::string(v) : std::string(boz_exe_dir()) + "/data";
        for (char& c : root)
            if (c == '\\') c = '/';
        while (root.size() > 3 && root.back() == '/') root.pop_back();
    });
    return root;
}

std::string boz_host_path(const char* guest) {
    if (!guest) return {};
    std::string g = guest;
    if (g.empty()) return g;
    // already a Windows path (the stand-in Java layer hands out guest paths, but be lenient)
    if (g.size() > 2 && g[1] == ':' && (g[2] == '/' || g[2] == '\\')) return g;
    // The game asks for its data pack by texture format (blackops_atitc / etc / dxt / pvrtc .dz and .dz.dat) in
    // Android/obb/<package>/. The one pack there is works on any GPU (each texture records its own format), so
    // every name leads to it.
    size_t obb = g.find("/Android/obb/");
    size_t slash = g.rfind('/');
    if (obb != std::string::npos && slash != std::string::npos && g.compare(slash + 1, 9, "blackops_") == 0) {
        std::string base = g.substr(slash + 1);
        const char* ext = base.size() > 7 && base.compare(base.size() - 7, 7, ".dz.dat") == 0 ? ".dz.dat"
                          : base.size() > 3 && base.compare(base.size() - 3, 3, ".dz") == 0     ? ".dz"
                                                                                                 : nullptr;
        if (ext) g = "/sdcard/Android/obb/com.activision.boz/blackops_etc" + std::string(ext);
    }
    std::string out = boz_data_root();
    if (g[0] != '/') out += '/';
    out += g;
    return out;
}

// The Android build's bundled-pack lookups: nothing is bundled on the PC.
const Bundled* vfs_lookup(const char*) { return nullptr; }
FILE* vfs_fopen(const Bundled*) { return nullptr; }

// /proc/meminfo as a phone would show it. The engine checks it before loading the game; the 32-bit engine sees
// values that fit its address space whatever the PC has.
std::string boz_fake_proc(const char* guest) {
    if (!guest || strcmp(guest, "/proc/meminfo") != 0) return {};
    return "MemTotal:        2000000 kB\n"
           "MemFree:         1500000 kB\n"
           "MemAvailable:    1600000 kB\n"
           "Buffers:           50000 kB\n"
           "Cached:           300000 kB\n"
           "SwapCached:            0 kB\n"
           "SwapTotal:             0 kB\n"
           "SwapFree:              0 kB\n";
}
