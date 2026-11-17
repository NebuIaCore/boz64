// Debugging: a sampling profiler for the render thread (debug.boz64.prof=1 with stats on). A CPU-time timer
// interrupts the thread every millisecond and records where it was; the statistics show the share of samples
// per library/function (code without a symbol, such as the JIT's generated code, is listed as "jit").
#include <dlfcn.h>
#include <signal.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <ucontext.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <ctime>
#include <map>
#include <string>
#include <vector>

#include "common.h"

namespace {
constexpr u32 RING = 1 << 16;
u64 ring[RING];
std::atomic<u32> head{0};
u32 tail = 0;
bool on = false;

void on_sample(int, siginfo_t*, void* uc) {
    u64 pc = reinterpret_cast<ucontext_t*>(uc)->uc_mcontext.pc;
    u32 h = head.load(std::memory_order_relaxed);
    ring[h % RING] = pc;
    head.store(h + 1, std::memory_order_release);
}
}  // namespace

void prof_start_this_thread() {
    char v[PROP_VALUE_MAX] = {};
    if (on || __system_property_get("debug.boz64.prof", v) <= 0 || v[0] != '1') return;
    on = true;
    struct sigaction sa{};
    sa.sa_sigaction = on_sample;
    sa.sa_flags = SA_SIGINFO | SA_RESTART;
    sigaction(SIGPROF, &sa, nullptr);
    sigevent ev{};
    ev.sigev_notify = SIGEV_THREAD_ID;
    ev.sigev_signo = SIGPROF;
    ev._sigev_un._tid = static_cast<pid_t>(syscall(SYS_gettid));
    timer_t timer;
    if (timer_create(CLOCK_THREAD_CPUTIME_ID, &ev, &timer) != 0) {
        LOGW("profiler: timer_create failed");
        return;
    }
    itimerspec its{{0, 1000000}, {0, 1000000}};
    timer_settime(timer, 0, &its, nullptr);
    LOGI("profiler on for the render thread");
}

void prof_stats() {
    if (!on) return;
    u32 h = head.load(std::memory_order_acquire);
    if (h - tail > RING) tail = h - RING;
    std::map<std::string, u32> by;
    u32 n = h - tail;
    for (; tail != h; tail++) {
        u64 pc = ring[tail % RING];
        Dl_info info{};
        std::string key;
        if (dladdr(reinterpret_cast<void*>(pc), &info) && info.dli_fname) {
            const char* lib = strrchr(info.dli_fname, '/');
            key = std::string(lib ? lib + 1 : info.dli_fname) + ":" + (info.dli_sname ? info.dli_sname : "?");
        } else {
            key = "jit";
        }
        by[key]++;
    }
    std::vector<std::pair<u32, std::string>> v;
    std::map<std::string, u32> libs;
    for (auto& [k, c] : by) {
        v.push_back({c, k});
        libs[k.substr(0, k.find(':'))] += c;
    }
    std::sort(v.begin(), v.end(), std::greater<>());
    std::string line, lines;
    for (auto& [lib, c] : libs) lines += " " + lib + " " + std::to_string(c * 100 / std::max(n, 1u)) + "%";
    for (size_t i = 0; i < v.size() && i < 30; i++) {
        std::string k = v[i].second;
        if (k.size() > 70) k = k.substr(0, 70);
        line += " " + k + " " + std::to_string(v[i].first * 1000 / std::max(n, 1u) / 10.0).substr(0, 4) + "%";
    }
    LOGI("stats: profile (%u samples) by library:%s", n, lines.c_str());
    LOGI("stats: profile top:%s", line.c_str());
}
