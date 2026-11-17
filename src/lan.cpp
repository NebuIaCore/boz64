// LAN and direct co-op: the s3eZeroConf extension the game's local Wi-Fi mode needs (see lan.h).
//
// How the game uses s3eZeroConf (CIwNetworkDiscoveryBonjour, plus CGameNetwork's "re-join" search):
//   StartSearch(type, domain, added, updated, removed, user) -> search handle
//       type "_PROJECT_KIWI._tcp", domain "local" (or null). Callbacks are cb(search, data, user) -> 0, where data is
//       added:   {+0 id, +8 name, +0xc type, +0x18 u16 port, +0x1a u16 text count, +0x1c text[], +0x20 IPv4}
//       updated: {+0 id, +6 u16 text count, +8 text[]}
//       removed: the id itself
//   StopSearch(search)
//   PublishService(u16 port, name, type, domain, text count, text[]) -> service handle
//   UpdateTxtRecord(service, text count, text[])
//   UnpublishService(service)
// Ports and addresses are kept as the game has them (Marmalade's s3eInetAddress: network byte order), so the port a
// host publishes reaches the joining game unchanged.
//
// Discovery packets (UDP, port LAN_DISCOVERY_PORT), all starting "BZ64", version 1, kind, sender id (u32):
//   1 announce / 3 goodbye: u16 port, type, name, u8 text count, texts   (strings: u8 length + bytes)
//   2 query: type   -> each matching published game answers the sender with an announce
#include "lan.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <mstcpip.h>
#ifndef SIO_UDP_CONNRESET
#define SIO_UDP_CONNRESET _WSAIOW(IOC_VENDOR, 12)
#endif
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#include <sys/system_properties.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <random>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "cpu.h"

namespace {
constexpr u32 DUMP_BASE = 0x02454000;
// The game's s3eZeroConf slot, filled by s3eExtGetHash when the extension exists:
//   +0 loaded, +1 load tried, +4 the five functions (in the order above), +0x18 "available" asked
constexpr u32 ZC_SLOT = 0x028b9550;
constexpr u32 ZC_LOADER = 0x0254ebf4;       // the game's loader for that slot (checked, to be sure of the addresses)
constexpr u32 ZC_LOADER_WORD = 0x4c0fb510;  // push {r4, lr}; ldr r4, [pc, #0x3c]
// CIwNetworkConnectionTcp's listening port: a function-static (guard word, then the u16), set to
// rand() % 0xfbff + 0x400 by its local-address function the first time.
constexpr u32 TCP_PORT_GUARD = 0x028b8b20;
constexpr u32 TCP_PORT = 0x028b8b24;
constexpr u32 TCP_PORT_FN = 0x0252d02c;
constexpr u32 TCP_PORT_FN_WORD = 0x4605b538;  // push {r3, r4, r5, lr}; mov r5, r0

constexpr char MAGIC[4] = {'B', 'Z', '6', '4'};
constexpr u8 VERSION = 1;
enum Kind : u8 { ANNOUNCE = 1, QUERY = 2, GOODBYE = 3 };
constexpr u64 ANNOUNCE_MS = 1000;  // hosts announce, and searches ask, this often
constexpr u64 EXPIRE_MS = 5000;    // a game not heard from for this long is gone
constexpr size_t MAX_TEXTS = 8;

u32 image = 0;
u32 at(u32 a) { return a - DUMP_BASE + image; }
std::string files_dir;

u64 now_ms() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<u64>(ts.tv_sec) * 1000 + static_cast<u64>(ts.tv_nsec) / 1000000;
}

#ifdef _WIN32
using sock_t = SOCKET;
const sock_t NO_SOCK = INVALID_SOCKET;
int net_errno() { return WSAGetLastError(); }
#else
using sock_t = int;
const sock_t NO_SOCK = -1;
int net_errno() { return errno; }
#endif

std::string ip_str(u32 ip) {
    char b[INET_ADDRSTRLEN] = {};
    in_addr a{};
    a.s_addr = ip;
    inet_ntop(AF_INET, &a, b, sizeof b);
    return b;
}

// Service types match without regard to case or a trailing dot.
std::string norm_type(std::string s) {
    while (!s.empty() && s.back() == '.') s.pop_back();
    for (char& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

std::string joined(const std::vector<std::string>& v) {
    std::string s;
    for (const std::string& t : v) s += (s.empty() ? "" : " | ") + t;
    return s;
}

std::vector<std::string> guest_texts(u32 count, u32 arr) {
    std::vector<std::string> v;
    for (u32 i = 0; i < count && i < MAX_TEXTS && arr; i++) {
        u32 s = g::rd32(arr + 4 * i);
        v.emplace_back(s ? g::str(s) : "");
    }
    return v;
}

struct Published {
    u32 handle;
    u16 port;  // as the game gave it
    std::string name, type;
    std::vector<std::string> texts;
};

struct Found {
    u32 id;
    u32 ip;      // network byte order
    u32 sender;  // the host's random id
    u16 port;
    std::string name, type;
    std::vector<std::string> texts;
    u64 seen;
};

struct Search {
    u32 handle;
    std::string type;
    u32 added, updated, removed, user;
    std::vector<Found> found;
};

enum EventKind { ADDED, UPDATED, REMOVED };
struct Event {
    EventKind kind;
    u32 search;
    Found f;
};

// Every game heard from lately, searched for or not, so a search the game starts can report them at once: the game
// looks for only about 0.2 s before hosting its own game, too short to wait for a query to be answered from far.
// A host on several networks (home Wi-Fi and a VPN such as Radmin, Hamachi or ZeroTier) is heard on each; the
// address it was first heard at is kept while it keeps answering there, instead of flipping between them.
struct Heard {
    u32 sender;
    std::string name, type;
    u32 ip;
    u16 port;
    std::vector<std::string> texts;
    u64 seen;     // heard on any address
    u64 ip_seen;  // heard on the address kept
};
constexpr u64 SWITCH_MS = 2500;  // a host not heard at its address for this long moves to the one it is heard at

std::mutex mu;
std::vector<Published> published;
std::vector<Search> searches;
std::vector<Event> events;
std::vector<Heard> heard;
std::vector<u32> peers;                     // the direct-play addresses, resolved (refreshed by the network thread)
std::string last_type = "_PROJECT_KIWI._tcp";  // what the game searches for (asked of direct-play peers meanwhile)
std::atomic<u32> route_hint{0};             // the other player last heard from or joined (see lan_redirect_probe)
std::atomic<u64> route_hint_seen{0};        // when route_hint was last set or heard from
u32 next_id = 1;
u32 self_id = 0;
sock_t sock = NO_SOCK;
std::atomic<bool> net_started{false};
u32 record_buf = 0;  // guest memory for the records handed to the game's callbacks
constexpr u32 RECORD_BUF_SIZE = 4096;
bool installed = false;

// ---- packets ----
struct Writer {
    std::vector<u8> b;
    void u8_(u8 v) { b.push_back(v); }
    void u16_(u16 v) { b.push_back(static_cast<u8>(v)), b.push_back(static_cast<u8>(v >> 8)); }
    void u32_(u32 v) { for (int i = 0; i < 4; i++) b.push_back(static_cast<u8>(v >> (8 * i))); }
    void str(const std::string& s) {
        size_t n = std::min<size_t>(s.size(), 255);
        b.push_back(static_cast<u8>(n));
        b.insert(b.end(), s.begin(), s.begin() + static_cast<long>(n));
    }
};
struct Reader {
    const u8* p;
    size_t n, i = 0;
    bool ok = true;
    bool need(size_t k) { return ok = ok && i + k <= n; }
    u8 u8_() { return need(1) ? p[i++] : 0; }
    u16 u16_() {
        if (!need(2)) return 0;
        u16 v = static_cast<u16>(p[i] | (p[i + 1] << 8));
        i += 2;
        return v;
    }
    u32 u32_() {
        if (!need(4)) return 0;
        u32 v = static_cast<u32>(p[i]) | static_cast<u32>(p[i + 1]) << 8 | static_cast<u32>(p[i + 2]) << 16 |
                static_cast<u32>(p[i + 3]) << 24;
        i += 4;
        return v;
    }
    std::string str() {
        u8 k = u8_();
        if (!need(k)) return {};
        std::string s(reinterpret_cast<const char*>(p + i), k);
        i += k;
        return s;
    }
};

Writer header(Kind k) {
    Writer w;
    w.b.insert(w.b.end(), MAGIC, MAGIC + 4);
    w.u8_(VERSION);
    w.u8_(k);
    w.u32_(self_id);
    return w;
}

std::vector<u8> announce_packet(const Published& p, Kind k) {
    Writer w = header(k);
    w.u16_(p.port);
    w.str(p.type);
    w.str(p.name);
    w.u8_(static_cast<u8>(std::min(p.texts.size(), MAX_TEXTS)));
    for (size_t i = 0; i < p.texts.size() && i < MAX_TEXTS; i++) w.str(p.texts[i]);
    return w.b;
}

std::vector<u8> query_packet(const std::string& type) {
    Writer w = header(QUERY);
    w.str(type);
    return w.b;
}

void send_to(const std::vector<u8>& b, u32 ip, u16 port) {
    if (sock == NO_SOCK) return;
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(port);
    a.sin_addr.s_addr = ip;
    sendto(sock, reinterpret_cast<const char*>(b.data()), static_cast<int>(b.size()), 0, reinterpret_cast<sockaddr*>(&a),
           sizeof a);
}

bool link_local(u32 ip) { return (ntohl(ip) >> 16) == 0xa9fe; }  // 169.254/16: an adapter with no network

// The broadcast address of a network, from one of its addresses and its prefix length. Networks of one address
// (/31, /32: point-to-point links, Tailscale, WireGuard) have none.
u32 subnet_broadcast(u32 ip, int prefix) {
    if (prefix <= 0 || prefix > 30 || link_local(ip)) return 0;
    return ip | ~htonl(~0u << (32 - prefix));
}

// Every IPv4 broadcast address of the networks this device is on, plus 255.255.255.255 (which Windows sends on one
// adapter only). Virtual adapters count: VPNs that act as a LAN (Radmin VPN, Hamachi, ZeroTier) pass broadcasts
// between their members, so games are found over them as on a home network.
std::vector<u32> broadcast_addrs() {
    std::vector<u32> v{INADDR_BROADCAST};
#ifdef _WIN32
    ULONG size = 16 * 1024;
    std::vector<unsigned char> buf(size);
    auto* list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
    ULONG flags = GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER;
    ULONG r = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(size);
        list = reinterpret_cast<IP_ADAPTER_ADDRESSES*>(buf.data());
        r = GetAdaptersAddresses(AF_INET, flags, nullptr, list, &size);
    }
    if (r == NO_ERROR) {
        for (auto* ad = list; ad; ad = ad->Next) {
            if (ad->OperStatus != IfOperStatusUp || ad->IfType == IF_TYPE_SOFTWARE_LOOPBACK) continue;
            for (auto* u = ad->FirstUnicastAddress; u; u = u->Next) {
                if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
                u32 ip = reinterpret_cast<sockaddr_in*>(u->Address.lpSockaddr)->sin_addr.s_addr;
                u32 b = subnet_broadcast(ip, u->OnLinkPrefixLength);
                if (b && std::find(v.begin(), v.end(), b) == v.end()) v.push_back(b);
            }
        }
    }
#else
    ifaddrs* list = nullptr;
    if (getifaddrs(&list) == 0) {
        for (ifaddrs* i = list; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_netmask) continue;
            if (!(i->ifa_flags & IFF_UP) || (i->ifa_flags & IFF_LOOPBACK)) continue;
            // VPN apps' tun interfaces have no broadcast flag; their network's broadcast address still reaches the
            // other members when the VPN carries broadcasts (ZeroTier does)
            u32 ip = reinterpret_cast<sockaddr_in*>(i->ifa_addr)->sin_addr.s_addr;
            u32 mask = ntohl(reinterpret_cast<sockaddr_in*>(i->ifa_netmask)->sin_addr.s_addr);
            int prefix = 0;
            while (prefix < 32 && (mask & (0x80000000u >> prefix))) prefix++;
            u32 b = subnet_broadcast(ip, prefix);
            if (b && std::find(v.begin(), v.end(), b) == v.end()) v.push_back(b);
        }
        freeifaddrs(list);
    }
#endif
    return v;
}

void broadcast(const std::vector<u8>& b) {
    // debug.boz64.lanbroadcast=0: direct-play addresses only (tests the path VPNs without broadcast take)
    static const bool off = [] {
        char v[PROP_VALUE_MAX] = {};
        bool o = __system_property_get("debug.boz64.lanbroadcast", v) > 0 && v[0] == '0';
        if (o) LOGI("lan: broadcasts off (debug.boz64.lanbroadcast=0)");
        return o;
    }();
    if (off) return;
    for (u32 a : broadcast_addrs()) send_to(b, a, LAN_DISCOVERY_PORT);
}

// Where the direct-play address list can be written (all are read):
//   files/boz64_peers.txt                      the app's private folder (adb on the phone)
//   PC: peers.txt next to boz64.exe
//   phone: Android/data/<package>/files/peers.txt on shared storage, which a PC reaches over USB
std::vector<std::string> peer_files() {
    std::vector<std::string> v;
    if (!files_dir.empty()) v.push_back(files_dir + "/boz64_peers.txt");
#ifdef BOZ_PC
    v.push_back(std::string(boz_exe_dir()) + "/peers.txt");
#else
    std::string d = files_dir;
    while (!d.empty() && d.back() == '/') d.pop_back();
    if (d.size() > 6 && d.compare(d.size() - 6, 6, "/files") == 0) d.resize(d.size() - 6);
    size_t s = d.rfind('/');
    if (s != std::string::npos && s + 1 < d.size()) v.push_back("/sdcard/Android/data/" + d.substr(s + 1) + "/files/peers.txt");
#endif
    return v;
}

// Writes a commented peers.txt where players can find it (next to the PC exe; the phone's Android/data folder) if
// there is none, so there is always a file to edit.
void write_peer_template() {
    std::vector<std::string> files = peer_files();
    if (files.empty()) return;
    std::string path = files.back();
#ifndef BOZ_PC
    std::string dir = path.substr(0, path.rfind('/'));
    mkdir(dir.substr(0, dir.rfind('/')).c_str(), 0777);
    mkdir(dir.c_str(), 0777);
#endif
    if (FILE* f = fopen(path.c_str(), "r")) {
        fclose(f);
        return;
    }
    FILE* f = fopen(path.c_str(), "w");
    if (!f) return;
    fputs("# BOZ64 direct play: the other players' addresses, one per line (an IP, or a computer's name).\n"
          "#\n"
          "# Not needed on the same Wi-Fi, or with Radmin VPN, Hamachi or ZeroTier: games are found on their own.\n"
          "# Needed with Tailscale or WireGuard (write the other player's 100.x.x.x address), or over the internet\n"
          "# when the host has forwarded UDP port 47710 and TCP port 47711 (write the host's public IP).\n"
          "# Only one side needs the other's address. Lines starting with # are ignored. For example:\n"
          "#   100.101.102.103\n"
          "#   friends-pc\n",
          f);
    fclose(f);
    LOGI("lan: wrote %s", path.c_str());
}

// The addresses written down for direct play (debug.boz64.peers and the files above): IPs or host names (a
// Tailscale machine name works), separated by commas, spaces or lines; '#' starts a comment.
std::vector<u32> direct_peers() {
    std::string text;
    char v[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.boz64.peers", v) > 0) text = std::string(v) + "\n";
    for (const std::string& path : peer_files()) {
        if (FILE* f = fopen(path.c_str(), "r")) {
            char buf[4096];
            size_t n = fread(buf, 1, sizeof buf - 1, f);
            buf[n] = 0;
            text += buf;
            text += "\n";
            fclose(f);
        }
    }
    std::vector<u32> ips;
    size_t line_start = 0;
    while (line_start < text.size()) {
        size_t line_end = text.find('\n', line_start);
        if (line_end == std::string::npos) line_end = text.size();
        std::string line = text.substr(line_start, line_end - line_start);
        line_start = line_end + 1;
        line = line.substr(0, line.find('#'));
        size_t i = 0;
        while (i < line.size()) {
            size_t j = line.find_first_of(", \t\r;", i);
            if (j == std::string::npos) j = line.size();
            std::string host = line.substr(i, j - i);
            i = j + 1;
            if (host.empty()) continue;
            u32 ip = 0;
            in_addr a{};
            if (inet_pton(AF_INET, host.c_str(), &a) == 1) {
                ip = a.s_addr;
            } else {
                addrinfo hints{}, *res = nullptr;
                hints.ai_family = AF_INET;
                hints.ai_socktype = SOCK_DGRAM;
                if (getaddrinfo(host.c_str(), nullptr, &hints, &res) == 0 && res) {
                    ip = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
                    freeaddrinfo(res);
                } else {
                    LOGW("lan: direct-play address \"%s\" not found", host.c_str());
                }
            }
            if (ip && std::find(ips.begin(), ips.end(), ip) == ips.end()) ips.push_back(ip);
        }
    }
    return ips;
}

// ---- receiving (network thread, under mu) ----
// Brings a search's list in line with a game heard (under mu).
void sync_search(Search& s, const Heard& h, u64 t) {
    if (norm_type(s.type) != norm_type(h.type)) return;
    auto it = std::find_if(s.found.begin(), s.found.end(),
                           [&](const Found& f) { return f.sender == h.sender && f.name == h.name; });
    if (it != s.found.end() && (it->ip != h.ip || it->port != h.port)) {  // moved: report it as a new game
        LOGI("lan: game \"%s\" moved from %s to %s", h.name.c_str(), ip_str(it->ip).c_str(), ip_str(h.ip).c_str());
        events.push_back({REMOVED, s.handle, *it});
        s.found.erase(it);
        it = s.found.end();
    }
    if (it == s.found.end()) {
        Found f{next_id++, h.ip, h.sender, h.port, h.name, h.type, h.texts, t};
        LOGI("lan: found game \"%s\" at %s", h.name.c_str(), ip_str(h.ip).c_str());
        s.found.push_back(f);
        events.push_back({ADDED, s.handle, f});
    } else {
        it->seen = t;
        if (it->texts != h.texts) {
            it->texts = h.texts;
            events.push_back({UPDATED, s.handle, *it});
        }
    }
}

void on_announce(Reader& r, u32 sender, u32 ip, bool goodbye) {
    u16 port = r.u16_();
    std::string type = r.str(), name = r.str();
    u8 n = r.u8_();
    std::vector<std::string> texts;
    for (u8 k = 0; k < n && r.ok; k++) texts.push_back(r.str());
    if (!r.ok) return;
    u64 t = now_ms();
    auto hit = std::find_if(heard.begin(), heard.end(), [&](const Heard& h) { return h.sender == sender && h.name == name; });
    if (goodbye) {
        if (hit != heard.end()) heard.erase(hit);
        for (Search& s : searches) {
            auto it = std::find_if(s.found.begin(), s.found.end(),
                                   [&](const Found& f) { return f.sender == sender && f.name == name; });
            if (it == s.found.end()) continue;
            LOGI("lan: game \"%s\" at %s closed", name.c_str(), ip_str(it->ip).c_str());
            events.push_back({REMOVED, s.handle, *it});
            s.found.erase(it);
        }
        return;
    }
    if (hit == heard.end()) {
        heard.push_back({sender, name, type, ip, port, texts, t, t});
        hit = heard.end() - 1;
    } else {
        if (ip == hit->ip) {
            hit->ip_seen = t;
        } else if (t - hit->ip_seen > SWITCH_MS) {  // no longer heard where it was: it moved
            hit->ip = ip;
            hit->ip_seen = t;
        }
        if (ip == hit->ip) hit->port = port;
        hit->type = type;
        hit->texts = texts;
        hit->seen = t;
    }
    for (Search& s : searches) sync_search(s, *hit, t);
}

void on_query(Reader& r, const sockaddr_in& from) {
    std::string nt = norm_type(r.str());
    if (!r.ok) return;
    for (const Published& p : published)
        if (norm_type(p.type) == nt) send_to(announce_packet(p, ANNOUNCE), from.sin_addr.s_addr, ntohs(from.sin_port));
}

// The other player's address, for the game's own-address probe (see lan_redirect_probe).
// heard: from a discovery packet. A player on Wi-Fi and a VPN is heard at both addresses, so a heard
// address only replaces one that has gone quiet; joining or being joined always sets it.
void set_route_hint(u32 ip, const char* why, bool heard = false) {
    if (!ip || ip == INADDR_BROADCAST || (ntohl(ip) >> 24) == 127 || link_local(ip)) return;
    u64 t = now_ms();
    u32 cur = route_hint.load();
    if (heard && cur && ip != cur && t - route_hint_seen.load() <= SWITCH_MS) return;
    route_hint_seen = t;
    if (route_hint.exchange(ip) != ip) LOGI("lan: other player at %s (%s)", ip_str(ip).c_str(), why);
}

void on_packet(const u8* b, size_t n, const sockaddr_in& from) {
    Reader r{b, n};
    if (n < 10 || memcmp(b, MAGIC, 4) != 0) return;
    r.i = 4;
    if (r.u8_() != VERSION) return;
    u8 kind = r.u8_();
    u32 sender = r.u32_();
    if (sender == self_id) return;  // our own broadcast
    std::lock_guard lk(mu);
    // a joining player is the one a host's game should reach; any other copy will do until then
    if (published.empty() || kind == QUERY) set_route_hint(from.sin_addr.s_addr, "heard", true);
    if (kind == ANNOUNCE || kind == GOODBYE) on_announce(r, sender, from.sin_addr.s_addr, kind == GOODBYE);
    else if (kind == QUERY) on_query(r, from);
}

void net_loop() {
    u64 next_tick = 0;
    u64 next_peers = 0;
    u8 buf[2048];
    for (;;) {
        pollfd pf{sock, POLLIN, 0};
#ifdef _WIN32
        int ready = WSAPoll(&pf, 1, 200);
#else
        int ready = poll(&pf, 1, 200);
#endif
        if (ready > 0 && (pf.revents & POLLIN)) {
            sockaddr_in from{};
            socklen_t fl = sizeof from;
            auto n = recvfrom(sock, reinterpret_cast<char*>(buf), sizeof buf, 0, reinterpret_cast<sockaddr*>(&from), &fl);
            if (n > 0) on_packet(buf, static_cast<size_t>(n), from);
        }
        u64 t = now_ms();
        if (t < next_tick) continue;
        next_tick = t + ANNOUNCE_MS;
        if (t >= next_peers) {  // resolving names can block, so not under the lock; once every 10 s is enough
            std::vector<u32> p = direct_peers();
            std::lock_guard lk(mu);
            if (p != peers) {
                std::string list;
                for (u32 ip : p) list += (list.empty() ? "" : ", ") + ip_str(ip);
                LOGI("lan: direct-play addresses: %s", list.empty() ? "none" : list.c_str());
            }
            peers = std::move(p);
            next_peers = t + 10000;
        }
        std::vector<std::vector<u8>> announces, queries;
        std::vector<u32> to;
        {
            std::lock_guard lk(mu);
            to = peers;
            for (const Published& p : published) announces.push_back(announce_packet(p, ANNOUNCE));
            for (const Search& s : searches) queries.push_back(query_packet(s.type));
            // direct-play peers are asked all the time, so their games are known before the game's short search
            if (queries.empty() && !to.empty()) queries.push_back(query_packet(last_type));
            for (size_t i = 0; i < heard.size();) {
                if (t - heard[i].seen > EXPIRE_MS) heard.erase(heard.begin() + static_cast<long>(i));
                else i++;
            }
            // games not heard from lately are gone
            for (Search& s : searches) {
                for (size_t i = 0; i < s.found.size();) {
                    if (t - s.found[i].seen > EXPIRE_MS) {
                        LOGI("lan: lost game \"%s\" at %s", s.found[i].name.c_str(), ip_str(s.found[i].ip).c_str());
                        events.push_back({REMOVED, s.handle, s.found[i]});
                        s.found.erase(s.found.begin() + static_cast<long>(i));
                    } else {
                        i++;
                    }
                }
            }
        }
        for (auto& a : announces) {
            broadcast(a);
            for (u32 ip : to) send_to(a, ip, LAN_DISCOVERY_PORT);
        }
        for (auto& q : queries) {
            broadcast(q);
            for (u32 ip : to) send_to(q, ip, LAN_DISCOVERY_PORT);
        }
    }
}

// Opens the discovery socket and starts the network thread, when the extension is installed (game start).
void start_net() {
    if (net_started.exchange(true)) return;
    std::random_device rd;
    self_id = rd() | 1;
#ifdef _WIN32
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
    sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == NO_SOCK) {
        LOGE("lan: no UDP socket (error %d)", net_errno());
        return;
    }
    int one = 1;
#ifndef _WIN32
    // (Windows' SO_REUSEADDR would let a second copy of the game share the port and take the answers meant for
    // the first; there a second copy falls back to another port, below)
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&one), sizeof one);
#endif
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof one);
#ifdef _WIN32
    // Windows reports an ICMP "port unreachable" for an earlier datagram as an error on the next receive; ignore it
    BOOL off = FALSE;
    DWORD got = 0;
    WSAIoctl(sock, SIO_UDP_CONNRESET, &off, sizeof off, nullptr, 0, &got, nullptr, nullptr);
#endif
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(LAN_DISCOVERY_PORT);
    if (bind(sock, reinterpret_cast<sockaddr*>(&a), sizeof a) != 0) {
        // Another app has the port: searching still works (hosts answer queries at the port they came from),
        // but hosting here can't be found by broadcast queries.
        LOGW("lan: UDP port %u is in use (error %d); using another", LAN_DISCOVERY_PORT, net_errno());
        a.sin_port = 0;
        bind(sock, reinterpret_cast<sockaddr*>(&a), sizeof a);
    }
    socklen_t al = sizeof a;
    getsockname(sock, reinterpret_cast<sockaddr*>(&a), &al);
    std::string nets;
    for (u32 b : broadcast_addrs()) nets += (nets.empty() ? "" : ", ") + ip_str(b);
    LOGI("lan: discovery on UDP port %u, games on TCP port %u; broadcasts to %s", ntohs(a.sin_port), LAN_GAME_PORT,
         nets.c_str());
    write_peer_template();
    std::thread(net_loop).detach();
}

// ---- the extension's functions (game thread) ----
u32 new_handle() { return g::data_alloc(16); }  // opaque to the game; never read

u32 zc_start_search(u32 type, u32 domain, u32 added, u32 updated, u32 removed, u32 user) {
    start_net();
    Search s{new_handle(), type ? g::str(type) : "", added, updated, removed, user, {}};
    std::vector<u8> q = query_packet(s.type);
    std::vector<u32> to;
    size_t known = 0;
    {
        std::lock_guard lk(mu);
        if (!s.type.empty()) last_type = s.type;
        searches.push_back(s);
        // games already heard are reported at once (handed to the game on its next frame)
        u64 t = now_ms();
        Search& added_s = searches.back();
        for (const Heard& h : heard)
            if (t - h.seen <= EXPIRE_MS) sync_search(added_s, h, t);
        known = added_s.found.size();
        to = peers;
    }
    LOGI("lan: looking for games (%s), %zu known", s.type.c_str(), known);
    broadcast(q);  // ask at once instead of waiting for the next announcement
    for (u32 ip : to) send_to(q, ip, LAN_DISCOVERY_PORT);
    return s.handle;
}

u32 zc_stop_search(u32 h) {
    std::lock_guard lk(mu);
    auto it = std::find_if(searches.begin(), searches.end(), [&](const Search& s) { return s.handle == h; });
    if (it != searches.end()) {
        LOGI("lan: stopped looking for games");
        searches.erase(it);
    }
    return 0;
}

u32 zc_publish(u32 port, u32 name, u32 type, u32 domain, u32 count, u32 texts) {
    start_net();
    Published p{new_handle(), static_cast<u16>(port), name ? g::str(name) : "", type ? g::str(type) : "",
                guest_texts(count & 0xffff, texts)};
    LOGI("lan: hosting game \"%s\" (%s, TCP port %u): %s", p.name.c_str(), p.type.c_str(), ntohs(p.port),
         joined(p.texts).c_str());
    std::vector<u8> a = announce_packet(p, ANNOUNCE);
    {
        std::lock_guard lk(mu);
        published.push_back(p);
    }
    broadcast(a);
    return p.handle;
}

u32 zc_update_texts(u32 h, u32 count, u32 texts) {
    std::lock_guard lk(mu);
    for (Published& p : published) {
        if (p.handle != h) continue;
        std::vector<std::string> t = guest_texts(count & 0xffff, texts);
        if (t == p.texts) continue;  // the game sends its text every frame
        p.texts = std::move(t);
        LOGI("lan: game \"%s\" now: %s", p.name.c_str(), joined(p.texts).c_str());
    }
    return 0;
}

u32 zc_unpublish(u32 h) {
    std::vector<u8> bye;
    {
        std::lock_guard lk(mu);
        auto it = std::find_if(published.begin(), published.end(), [&](const Published& p) { return p.handle == h; });
        if (it == published.end()) return 0;
        LOGI("lan: stopped hosting \"%s\"", it->name.c_str());
        bye = announce_packet(*it, GOODBYE);
        published.erase(it);
    }
    broadcast(bye);
    return 0;
}

// ---- handing results to the game (game thread) ----
struct RecordWriter {
    u32 base, pos, end;
    u32 str(const std::string& s) {
        u32 n = static_cast<u32>(std::min<size_t>(s.size(), 255));
        if (pos + n + 1 > end) return base + RECORD_BUF_SIZE - 1;  // the buffer's last byte stays 0: ""
        u32 a = pos;
        memcpy(g::h(a), s.data(), n);
        g::wr8(a + n, 0);
        pos += n + 1;
        return a;
    }
    u32 texts(const std::vector<std::string>& v) {
        pos = (pos + 3) & ~3u;
        u32 arr = pos;
        pos += 4 * static_cast<u32>(v.size());
        for (size_t i = 0; i < v.size(); i++) g::wr32(arr + 4 * static_cast<u32>(i), str(v[i]));
        return arr;
    }
};

void deliver(const Event& e, const Search& s) {
    if (!record_buf) return;
    memset(g::h(record_buf), 0, RECORD_BUF_SIZE);
    RecordWriter w{record_buf, record_buf + 0x40, record_buf + RECORD_BUF_SIZE - 1};
    const Found& f = e.f;
    std::vector<std::string> texts(f.texts.begin(), f.texts.begin() + std::min(f.texts.size(), MAX_TEXTS));
    u32 r = record_buf;
    if (e.kind == ADDED) {
        set_route_hint(f.ip, "found their game");
        g::wr32(r + 0x00, f.id);
        g::wr32(r + 0x04, s.handle);
        g::wr32(r + 0x08, w.str(f.name));
        g::wr32(r + 0x0c, w.str(f.type));
        g::wr32(r + 0x10, w.str("local."));
        g::wr32(r + 0x14, w.str(ip_str(f.ip)));
        g::wr16(r + 0x18, f.port);
        g::wr16(r + 0x1a, static_cast<u16>(texts.size()));
        g::wr32(r + 0x1c, w.texts(texts));
        g::wr32(r + 0x20, f.ip);
        call_guest(s.added, {s.handle, r, s.user});
    } else if (e.kind == UPDATED) {
        g::wr32(r + 0x00, f.id);
        g::wr16(r + 0x06, static_cast<u16>(texts.size()));
        g::wr32(r + 0x08, w.texts(texts));
        call_guest(s.updated, {s.handle, r, s.user});
    } else {
        call_guest(s.removed, {s.handle, f.id, s.user});
    }
}

void install() {
    installed = true;
    u32 w = g::rd32(at(ZC_LOADER));
    if (w != ZC_LOADER_WORD) {
        LOGW("lan: unexpected code at 0x%08x (%08x); local Wi-Fi play is off", ZC_LOADER, w);
        return;
    }
    u32 slot = at(ZC_SLOT);
    if (g::rd8(slot) || g::rd8(slot + 1)) {
        LOGW("lan: the game already looked for s3eZeroConf; local Wi-Fi play is off");
        return;
    }
    record_buf = g::data_alloc(RECORD_BUF_SIZE);
    g::wr32(slot + 0x04, reg("s3eZeroConfStartSearch", &zc_start_search));
    g::wr32(slot + 0x08, reg("s3eZeroConfStopSearch", &zc_stop_search));
    g::wr32(slot + 0x0c, reg("s3eZeroConfPublishService", &zc_publish));
    g::wr32(slot + 0x10, reg("s3eZeroConfUpdateTxtRecord", &zc_update_texts));
    g::wr32(slot + 0x14, reg("s3eZeroConfUnpublishService", &zc_unpublish));
    g::wr8(slot + 1, 1);
    g::wr8(slot, 1);  // loaded
    // A fixed game port, so a host can forward it for direct play over the internet.
    if (g::rd32(at(TCP_PORT_FN)) == TCP_PORT_FN_WORD && !(g::rd32(at(TCP_PORT_GUARD)) & 1)) {
        g::wr16(at(TCP_PORT), htons(LAN_GAME_PORT));
        g::wr32(at(TCP_PORT_GUARD), 1);
    } else {
        LOGW("lan: could not fix the game's TCP port; it stays random");
    }
    LOGI("lan: s3eZeroConf installed");
    start_net();  // listen from the start, so games are known before the game's short search
}
}  // namespace

void lan_init(const std::string& dir) { files_dir = dir; }

void lan_note_peer(u32 ip) { set_route_hint(ip, "joined us"); }

bool lan_redirect_probe(u32& ip, u16 port) {
    constexpr u32 PROBE = 0x63636363;  // 99.99.99.99, any byte order
    if (ip != PROBE) {
        if (port == htons(LAN_GAME_PORT)) set_route_hint(ip, "joining");  // the game connecting to a host
        return false;
    }
    u32 hint = route_hint.load();
    if (!hint) return false;
    static std::atomic<u32> logged{0};
    if (logged.exchange(hint) != hint) LOGI("lan: own-address probe sent toward %s", ip_str(hint).c_str());
    ip = hint;
    return true;
}

void lan_set_image(u32 image_lo) { image = image_lo; }

void lan_frame() {
    if (image && !installed) install();
}

void lan_poll() {
    if (!installed) return;
    std::vector<Event> ev;
    {
        std::lock_guard lk(mu);
        if (events.empty()) return;
        ev.swap(events);
    }
    for (const Event& e : ev) {
        Search s{};
        bool live = false;
        {
            std::lock_guard lk(mu);
            for (const Search& x : searches)
                if (x.handle == e.search) s = Search{x.handle, x.type, x.added, x.updated, x.removed, x.user, {}}, live = true;
        }
        if (live) deliver(e, s);  // without the lock: the game may stop the search from its callback
    }
}
