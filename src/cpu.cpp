#include "cpu.h"
#include "elf.h"
#include "frame.h"
#include "patches.h"

#ifndef _WIN32
#include <sys/syscall.h>
#endif
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <mutex>
#include <string>
#include <unordered_map>

#include <dynarmic/interface/A32/a32.h>
#include <algorithm>
#include <dynarmic/interface/A32/config.h>
#include <sys/system_properties.h>
#include <dynarmic/interface/A32/coprocessor.h>
#include <dynarmic/interface/exclusive_monitor.h>

using namespace Dynarmic;

namespace {
constexpr u32 SVC_THUNK = 0x800000;
constexpr u32 SVC_RETURN = 0x7FFF00;
constexpr u32 SVC_KCMPXCHG = 0x7FFF01;
constexpr u32 SVC_KGETTLS = 0x7FFF02;
constexpr u32 SVC_KCMPXCHG64 = 0x7FFF03;
constexpr int MONITOR_CPUS = 64;
constexpr u32 DEFAULT_STACK = 8 * 1024 * 1024;

u32 svc_insn(u32 imm) { return 0xEF000000u | (imm & 0xFFFFFF); }
constexpr u32 BX_LR = 0xE12FFF1Eu;

struct ThunkEntry {
    const char* name;
    Invoker inv;
    void* fp;
    u8 str_args;  // bit i: argument i is a C string (only used for tracing)
};

// Functions whose string arguments are worth showing in the trace.
u8 string_args_of(const char* n) {
    static const std::unordered_map<std::string, u8> m = {
        {"open", 1}, {"fopen", 1}, {"stat", 1}, {"lstat", 1}, {"access", 1}, {"dlopen", 1}, {"opendir", 1},
        {"mkdir", 1}, {"unlink", 1}, {"remove", 1}, {"rename", 3}, {"getenv", 1}, {"dlsym", 2}, {"statfs", 1},
        {"rmdir", 1}, {"chdir", 1}, {"realpath", 1}, {"__android_log_print", 6}, {"__android_log_write", 6},
        {"eglGetProcAddress", 1}, {"__system_property_get", 1}, {"property_get", 1}, {"truncate", 1},
        {"utime", 1}, {"chmod", 1}, {"fputs", 1}, {"puts", 1}};
    auto it = m.find(n);
    return it == m.end() ? 0 : it->second;
}
ThunkEntry thunks[g::THUNK_MAX / 8];
std::atomic<u32>* g_thunk_counts = nullptr;  // statistics: calls per host function (debug.boz64.thunks=1)
bool prop_is(const char* name, char value) {
    char v[PROP_VALUE_MAX] = {};
    return __system_property_get(name, v) > 0 && v[0] == value;
}
bool g_accurate_fp = prop_is("debug.boz64.accurate", '1');
bool g_slow_dispatch = prop_is("debug.boz64.fastdispatch", '0');
std::atomic<u32> nthunks{0};
std::mutex reg_lock;
std::unordered_map<std::string, u32> thunk_index;
std::unordered_map<std::string, u32> data_syms;

u32 return_stub = 0;
ExclusiveMonitor* monitor = nullptr;
std::mutex threads_lock;
std::vector<GuestThread*> threads;
std::atomic<u32> next_tid{1};
thread_local GuestThread* tl_self = nullptr;

const char* exc_name(int e) {
    static const char* n[] = {"UndefinedInstruction", "UnpredictableInstruction", "DecodeError", "SendEvent",
                              "SendEventLocal", "WaitForInterrupt", "WaitForEvent", "Yield", "Breakpoint",
                              "PreloadData", "PreloadDataWithIntentToWrite", "PreloadInstruction", "NoExecuteFault"};
    return e >= 0 && e < 13 ? n[e] : "?";
}
}  // namespace

struct Callbacks final : A32::UserCallbacks {
    GuestThread* t = nullptr;

    // Guest access to the unmapped first 64 KB (a NULL pointer). The JIT is halted; run_loop reports it.
    bool bad(u32 a, const char* what) {
        if (a >= g::NULL_GUARD) return false;
        t->fault_addr = a;
        t->fault_what = what;
        t->jit->HaltExecution(HaltReason::MemoryAbort);
        return true;
    }
    std::optional<u32> MemoryReadCode(u32 a) override {
        if (a < g::NULL_GUARD) return std::nullopt;
        if (g_patch_regions_pending.load(std::memory_order_relaxed)) patches_on_fetch(a);
        return g::rd32(a);
    }
    u8 MemoryRead8(u32 a) override { return bad(a, "read") ? 0 : g::rd8(a); }
    u16 MemoryRead16(u32 a) override { return bad(a, "read") ? 0 : g::rd16(a); }
    u32 MemoryRead32(u32 a) override { return bad(a, "read") ? 0 : g::rd32(a); }
    u64 MemoryRead64(u32 a) override { return bad(a, "read") ? 0 : g::rd64(a); }
    void MemoryWrite8(u32 a, u8 v) override { if (!bad(a, "write")) g::wr8(a, v); }
    void MemoryWrite16(u32 a, u16 v) override { if (!bad(a, "write")) g::wr16(a, v); }
    void MemoryWrite32(u32 a, u32 v) override { if (!bad(a, "write")) g::wr32(a, v); }
    void MemoryWrite64(u32 a, u64 v) override { if (!bad(a, "write")) g::wr64(a, v); }
    template <class T>
    bool cas(u32 a, T v, T expected) {
        if (bad(a, "exclusive write")) return false;
        return __atomic_compare_exchange_n(reinterpret_cast<T*>(g::mem + a), &expected, v, false, __ATOMIC_SEQ_CST,
                                           __ATOMIC_SEQ_CST);
    }
    bool MemoryWriteExclusive8(u32 a, u8 v, u8 e) override { return cas<u8>(a, v, e); }
    bool MemoryWriteExclusive16(u32 a, u16 v, u16 e) override { return cas<u16>(a, v, e); }
    bool MemoryWriteExclusive32(u32 a, u32 v, u32 e) override { return cas<u32>(a, v, e); }
    bool MemoryWriteExclusive64(u32 a, u64 v, u64 e) override { return cas<u64>(a, v, e); }
    void InterpreterFallback(u32 pc, size_t n) override { fatal("interpreter fallback at 0x%08x (%zu)", pc, n); }
    void CallSVC(u32 swi) override {
        t->pending_svc = swi;
        t->jit->HaltExecution(HaltReason::UserDefined1);
    }
    void ExceptionRaised(u32 pc, A32::Exception e) override {
        t->exc_pc = pc;
        t->exc = static_cast<int>(e);
        t->jit->HaltExecution(HaltReason::UserDefined3);
    }
    void AddTicks(u64) override {}
    u64 GetTicksRemaining() override { return 0x7fffffff; }
};

// cp15 access used for TLS (mrc p15, 0, rX, c13, c0, 3) and old-style barriers (mcr p15, 0, rX, c7, ...).
struct CP15 final : A32::Coprocessor {
    GuestThread* t = nullptr;
    static u64 nop(void*, u32, u32) { return 0; }
    std::optional<Callback> CompileInternalOperation(bool, unsigned, A32::CoprocReg, A32::CoprocReg, A32::CoprocReg,
                                                     unsigned) override {
        return std::nullopt;
    }
    CallbackOrAccessOneWord CompileSendOneWord(bool two, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm,
                                               unsigned opc2) override {
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C7) return Callback{&nop, std::nullopt};
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C13 && CRm == A32::CoprocReg::C0 && opc2 == 2)
            return &t->tpidrurw;
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileSendTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    CallbackOrAccessOneWord CompileGetOneWord(bool two, unsigned opc1, A32::CoprocReg CRn, A32::CoprocReg CRm,
                                              unsigned opc2) override {
        if (!two && opc1 == 0 && CRn == A32::CoprocReg::C13 && CRm == A32::CoprocReg::C0) {
            if (opc2 == 3) return &t->tls;
            if (opc2 == 2) return &t->tpidrurw;
        }
        return std::monostate{};
    }
    CallbackOrAccessTwoWords CompileGetTwoWords(bool, unsigned, A32::CoprocReg) override { return std::monostate{}; }
    std::optional<Callback> CompileLoadWords(bool, bool, A32::CoprocReg, std::optional<u8>) override {
        return std::nullopt;
    }
    std::optional<Callback> CompileStoreWords(bool, bool, A32::CoprocReg, std::optional<u8>) override {
        return std::nullopt;
    }
};

GuestThread::GuestThread() = default;
GuestThread::~GuestThread() = default;
std::array<u32, 16>& GuestThread::regs() { return jit->Regs(); }
u32 GuestThread::cpsr() { return jit->Cpsr(); }
void GuestThread::set_cpsr(u32 v) { jit->SetCpsr(v); }
std::array<u32, 64>& GuestThread::ext_regs() { return jit->ExtRegs(); }

static void dump_guest_state();

// ---- thunk table ----

u32 add_thunk(const char* name, Invoker inv, void* fp) {
    std::lock_guard lk(reg_lock);
    auto it = thunk_index.find(name);
    u32 idx;
    if (it != thunk_index.end()) {
        idx = it->second;  // later registration replaces an earlier one
    } else {
        idx = nthunks.load();
        if (idx >= g::THUNK_MAX / 8) fatal("too many thunks");
        thunk_index[name] = idx;
    }
    thunks[idx] = {strdup(name), inv, fp, string_args_of(name)};
    u32 addr = g::THUNK_BASE + idx * 8;
    g::wr32(addr, svc_insn(SVC_THUNK + idx));
    g::wr32(addr + 4, BX_LR);
    if (idx == nthunks.load()) nthunks.store(idx + 1);
    return addr;
}

u32 add_guest_code(const char* name, const u32* insns, u32 count) {
    u32 code = g::calloc(1, count * 4);
    for (u32 i = 0; i < count; i++) g::wr32(code + i * 4, insns[i]);
    u32 addr = add_thunk(name, [](GuestThread&, void*) {});
    g::wr32(addr, 0xe51ff004);  // ldr pc, [pc, #-4]
    g::wr32(addr + 4, code);
    return addr;
}

u32 find_thunk(const char* name) {
    std::lock_guard lk(reg_lock);
    auto it = thunk_index.find(name);
    return it == thunk_index.end() ? 0 : g::THUNK_BASE + it->second * 8;
}

u32 thunk_for_missing(const char* name) {
    LOGW("guest import %s has no implementation; calling it will stop the game", name);
    return add_thunk(
        name, [](GuestThread& t, void*) {
            u32 idx = t.pending_svc - SVC_THUNK;
            fatal("guest called unimplemented function %s (lr 0x%08x)", thunks[idx].name, t.regs()[14]);
        });
}

void add_data_symbol(const char* name, u32 addr) {
    std::lock_guard lk(reg_lock);
    data_syms[name] = addr;
}
u32 find_data_symbol(const char* name) {
    std::lock_guard lk(reg_lock);
    auto it = data_syms.find(name);
    return it == data_syms.end() ? 0 : it->second;
}

const char* thunk_name_at(u32 addr) {
    if (addr < g::THUNK_BASE || addr >= g::THUNK_BASE + nthunks.load() * 8) return nullptr;
    return thunks[(addr - g::THUNK_BASE) / 8].name;
}

// ---- threads ----

void init_cpu() {
    g_fatal_hook = dump_guest_state;
    monitor = new ExclusiveMonitor(MONITOR_CPUS);
    return_stub = g::data_alloc(8, 8);
    g::wr32(return_stub, svc_insn(SVC_RETURN));
    g::wr32(return_stub + 4, BX_LR);
    // Kernel user helpers (old ARM code calls these fixed addresses for atomics and TLS).
    const u32 kp = g::KUSER_PAGE;
    g::wr32(kp + 0x0fa0, BX_LR);  // __kuser_memory_barrier
    g::wr32(kp + 0x0fc0, svc_insn(SVC_KCMPXCHG));
    g::wr32(kp + 0x0fc4, BX_LR);
    g::wr32(kp + 0x0fe0, svc_insn(SVC_KGETTLS));
    g::wr32(kp + 0x0fe4, BX_LR);
    g::wr32(kp + 0x0f60, svc_insn(SVC_KCMPXCHG64));
    g::wr32(kp + 0x0f64, BX_LR);
    g::wr32(kp + 0x0ffc, 5);  // __kuser_helper_version
}


// ---- crash reports ----

static void dump_guest_state() {
    GuestThread* tp = tl_self;
    if (!tp || !tp->jit) return;
    GuestThread& t = *tp;
    auto& r = t.regs();
    LOGE("guest thread %u, cpsr %08x (%s)", t.id, t.cpsr(), (t.cpsr() & 0x20) ? "thumb" : "arm");
    for (int i = 0; i < 16; i += 4)
        LOGE("  r%-2d %08x  r%-2d %08x  r%-2d %08x  r%-2d %08x", i, r[i], i + 1, r[i + 1], i + 2, r[i + 2], i + 3,
             r[i + 3]);
    LOGE("  pc %s, lr %s", elf_describe(r[15]).c_str(), elf_describe(r[14]).c_str());
    u32 n = std::min<u32>(t.hist_n, t.hist.size());
    LOGE("last %u calls into the wrapper (oldest first):", n);
    for (u32 k = t.hist_n - n; k < t.hist_n; k++) {
        const auto& h = t.hist[k % t.hist.size()];
        LOGE("  %s(%x, %x, %x, %x) from %s -> %x", thunks[h.idx].name, h.a0, h.a1, h.a2, h.a3,
             elf_describe(h.lr).c_str(), h.r0);
    }
    u32 sp = r[13];
    if (sp >= g::NULL_GUARD && sp < g::KUSER_PAGE - 256) {
        LOGE("stack at %08x:", sp);
        for (int i = 0; i < 64; i += 8) {
            u32 a = sp + 4 * i;
            LOGE("  %08x: %08x %08x %08x %08x %08x %08x %08x %08x", a, g::rd32(a), g::rd32(a + 4), g::rd32(a + 8),
                 g::rd32(a + 12), g::rd32(a + 16), g::rd32(a + 20), g::rd32(a + 24), g::rd32(a + 28));
        }
    }
    u32 pc = r[15] & ~3u;
    if (pc >= g::NULL_GUARD + 64 && pc < g::KUSER_PAGE) {
        LOGE("code around pc:");
        for (u32 a = pc - 32; a < pc + 32; a += 16)
            LOGE("  %08x: %08x %08x %08x %08x", a, g::rd32(a), g::rd32(a + 4), g::rd32(a + 8), g::rd32(a + 12));
    }
}

void bind_guest_thread(GuestThread* t) { tl_self = t; }

GuestThread* create_guest_thread(u32 stack_lo, u32 stack_size) {
    auto* t = new GuestThread;
    t->id = next_tid++;
    if (!stack_lo || !stack_size) {
        stack_size = DEFAULT_STACK;
        stack_lo = g::page_alloc(stack_size);
        if (!stack_lo) fatal("no guest memory for a thread stack");
        t->own_stack = true;
    }
    t->stack_lo = stack_lo;
    t->stack_hi = stack_lo + stack_size;
    t->tls = g::calloc(1, GuestThread::KEYS_OFF + 128 * 4);  // 64 TLS words, errno, pthread key values
    t->errno_addr = t->tls + 64 * 4;
    g::wr32(t->tls + 0, t->tls);
    g::wr32(t->tls + 4, t->id);
    g::wr32(t->tls + 8, t->errno_addr);
    t->cb = std::make_unique<Callbacks>();
    t->cb->t = t;
    t->cp15 = std::make_shared<CP15>();
    t->cp15->t = t;

    A32::UserConfig cfg;
    cfg.callbacks = t->cb.get();
    cfg.processor_id = t->id % MONITOR_CPUS;
    cfg.global_monitor = monitor;
    cfg.arch_version = A32::ArchVersion::v7;
    cfg.fastmem_pointer = reinterpret_cast<uintptr_t>(g::mem);
    cfg.enable_cycle_counting = false;
    cfg.fast_dispatch = !g_slow_dispatch;  // debug.boz64.fastdispatch=0 turns the dispatcher's block cache off
    cfg.define_unpredictable_behaviour = true;
    cfg.code_cache_size = 64 * 1024 * 1024;
    // Faster floating point: ARM's default-NaN and rounding-mode details are not reproduced exactly (the game
    // does not depend on them). debug.boz64.accurate=1 turns this off.
    if (!g_accurate_fp) {
        cfg.unsafe_optimizations = true;
        cfg.optimizations |= OptimizationFlag::Unsafe_ReducedErrorFP | OptimizationFlag::Unsafe_InaccurateNaN |
                             OptimizationFlag::Unsafe_IgnoreStandardFPCRValue;
    }
    cfg.coprocessors[15] = t->cp15;
    cfg.check_halt_on_memory_access = g_precise;
    t->jit = std::make_unique<A32::Jit>(cfg);
    t->regs().fill(0);
    t->regs()[13] = t->stack_hi & ~15u;
    t->set_cpsr(0x10);  // user mode, ARM state
    t->jit->SetFpscr(0);
    {
        std::lock_guard lk(threads_lock);
        threads.push_back(t);
    }
    return t;
}

void end_guest_thread() {
    GuestThread* t = tl_self;
    if (!t) return;
    {
        std::lock_guard lk(threads_lock);
        std::erase(threads, t);
    }
    t->jit.reset();
    if (t->own_stack) g::page_free(t->stack_lo, t->stack_hi - t->stack_lo);
    t->finished = true;
    tl_self = nullptr;  // the GuestThread object stays for pthread_join
}

GuestThread* find_guest_thread(u32 id) {
    std::lock_guard lk(threads_lock);
    for (auto* t : threads)
        if (t->id == id) return t;
    return nullptr;
}

GuestThread& self() {
    if (!tl_self) tl_self = create_guest_thread(0, 0);
    return *tl_self;
}

u32 guest_scratch(GuestThread& t, u32 size) {
    if (size > 4096) fatal("guest_scratch too big");
    if (!t.scratch) t.scratch = g::malloc(4096);
    return t.scratch;
}

void invalidate_code(u32 addr, u32 size) {
    std::lock_guard lk(threads_lock);
    for (auto* t : threads)
        if (t->jit) t->jit->InvalidateCacheRange(addr, size);
}

// ---- running guest code ----

static void dispatch_svc(GuestThread& t, u32 svc) {
    auto& r = t.regs();
    if (svc >= SVC_THUNK) {
        u32 idx = svc - SVC_THUNK;
        if (idx >= nthunks.load()) fatal("bad thunk index %u", idx);
        ThunkEntry& e = thunks[idx];
        if (g_trace) {
            if (e.str_args) {
                std::string a[4];
                for (int i = 0; i < 4; i++) {
                    char b[16];
                    snprintf(b, sizeof b, "%x", r[i]);
                    a[i] = b;
                    if ((e.str_args >> i) & 1 && r[i] >= g::NULL_GUARD) a[i] = '"' + std::string(g::str(r[i])).substr(0, 200) + '"';
                }
                TRACE("%s(%s, %s, %s, %s) lr=%x", e.name, a[0].c_str(), a[1].c_str(), a[2].c_str(), a[3].c_str(), r[14]);
            } else {
                TRACE("%s(%x, %x, %x, %x) lr=%x", e.name, r[0], r[1], r[2], r[3], r[14]);
            }
        }
        tl_thunk_calls++;
        if (g_thunk_counts) g_thunk_counts[idx].fetch_add(1, std::memory_order_relaxed);
        GuestThread::CallRec& h = t.hist[t.hist_n++ % t.hist.size()];
        h = {idx, r[0], r[1], r[2], r[3], r[14], 0xdeadbeef};
        errno = 0;
        e.inv(t, e.fp);
        if (errno) g::wr32(t.errno_addr, errno);
        h.r0 = r[0];
        return;
    }
    switch (svc) {
    case SVC_KCMPXCHG: {  // r0 = old, r1 = new, r2 = ptr; r0 = 0 and C set on success
        u32 expected = r[0];
        bool ok = __atomic_compare_exchange_n(reinterpret_cast<u32*>(g::mem + r[2]), &expected, r[1], false,
                                              __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        r[0] = ok ? 0 : 1;
        t.set_cpsr(ok ? (t.cpsr() | 0x20000000) : (t.cpsr() & ~0x20000000u));
        return;
    }
    case SVC_KCMPXCHG64: {  // r0 = &old, r1 = &new, r2 = ptr
        u64 expected = g::rd64(r[0]);
        bool ok = __atomic_compare_exchange_n(reinterpret_cast<u64*>(g::mem + r[2]), &expected, g::rd64(r[1]), false,
                                              __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
        r[0] = ok ? 0 : 1;
        t.set_cpsr(ok ? (t.cpsr() | 0x20000000) : (t.cpsr() & ~0x20000000u));
        return;
    }
    case SVC_KGETTLS:
        r[0] = t.tls;
        return;
    case 0: {  // raw Linux system call, number in r7
        u32 nr = r[7];
        if (nr == 0xf0002) {  // cacheflush(start, end, flags)
            invalidate_code(r[0], r[1] - r[0]);
            patches_on_code(r[0], r[1]);
            r[0] = 0;
        } else if (nr == 224) {
            r[0] = gettid();
        } else if (nr == 20) {
            r[0] = getpid();
        } else if (nr == 158) {
            sched_yield();
            r[0] = 0;
        } else {
            LOGW("guest made system call %u directly; returning ENOSYS", nr);
            r[0] = static_cast<u32>(-ENOSYS);
        }
        return;
    }
    default:
        if (patches_svc(t, svc)) return;
        fatal("unknown svc 0x%x at 0x%08x", svc, r[15] - 4);
    }
}

static void run_loop(GuestThread& t) {
    for (;;) {
        HaltReason hr = t.jit->Run();
        if (Has(hr, HaltReason::UserDefined3)) {
            u32 pc = t.exc_pc;
            bool thumb = t.cpsr() & 0x20;
            fatal("guest CPU exception %s at 0x%08x (%s, insn %08x, lr 0x%08x)", exc_name(t.exc), pc,
                  thumb ? "thumb" : "arm", pc >= g::NULL_GUARD ? g::rd32(pc & ~1u) : 0, t.regs()[14]);
        }
        if (Has(hr, HaltReason::MemoryAbort)) {
            fatal("guest %s at bad address 0x%08x (pc %s0x%08x)", t.fault_what ? t.fault_what : "access", t.fault_addr,
                  g_precise ? "" : "near block ", t.regs()[15]);
        }
        if (Has(hr, HaltReason::UserDefined1)) {
            u32 svc = t.pending_svc;
            if (svc == SVC_RETURN) return;
            dispatch_svc(t, svc);
        }
    }
}

u64 call_guest_v(u32 fn, const u32* args, int n) {
    GuestThread& t = self();
    auto& r = t.regs();
    const auto saved = r;
    const u32 saved_cpsr = t.cpsr();
    u32 sp = r[13];
    int nstack = n > 4 ? n - 4 : 0;
    sp = (sp - 4 * nstack) & ~7u;
    for (int i = 0; i < n; i++) {
        if (i < 4)
            r[i] = args[i];
        else
            g::wr32(sp + 4 * (i - 4), args[i]);
    }
    r[13] = sp;
    r[14] = return_stub;
    r[15] = fn & ~1u;
    t.set_cpsr((saved_cpsr & ~0x20u) | ((fn & 1) ? 0x20u : 0u));
    t.depth++;
    run_loop(t);
    t.depth--;
    u64 result = r[0] | (static_cast<u64>(r[1]) << 32);
    r = saved;
    t.set_cpsr(saved_cpsr);
    return result;
}

u64 call_guest(u32 fn, std::initializer_list<u32> args) {
    return call_guest_v(fn, args.begin(), static_cast<int>(args.size()));
}

// Statistics: the most called host functions since the last call (debug.boz64.thunks=1 at start).
void thunk_stats_init() {
    char v[PROP_VALUE_MAX] = {};
    if (__system_property_get("debug.boz64.thunks", v) > 0 && v[0] == '1') g_thunk_counts = new std::atomic<u32>[g::THUNK_MAX / 8]();
}
void thunk_stats(u32 frames) {
    if (!g_thunk_counts || !frames) return;
    std::vector<std::pair<u32, u32>> v;
    u32 n = nthunks.load();
    for (u32 i = 0; i < n; i++)
        if (u32 c = g_thunk_counts[i].exchange(0)) v.push_back({c, i});
    std::sort(v.begin(), v.end(), std::greater<>());
    std::string line;
    for (size_t i = 0; i < v.size() && i < 25; i++) line += std::string(" ") + thunks[v[i].second].name + ":" + std::to_string(v[i].first / frames);
    LOGI("stats: host calls per frame:%s", line.c_str());
}
