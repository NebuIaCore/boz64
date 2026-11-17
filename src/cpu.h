// Running guest (32-bit ARM) code with dynarmic, one JIT per host thread, and the table of host
// functions ("thunks") that guest code reaches through "svc" stubs.
#pragma once
#include <jni.h>

#include <array>
#include <bit>
#include <initializer_list>
#include <memory>
#include <tuple>
#include <type_traits>
#include <vector>

#include "common.h"

namespace Dynarmic::A32 {
class Jit;
}
struct Callbacks;
struct CP15;

struct GuestThread {
    std::unique_ptr<Dynarmic::A32::Jit> jit;
    std::unique_ptr<Callbacks> cb;
    std::shared_ptr<CP15> cp15;
    u32 id = 0;              // guest pthread_t
    u32 stack_lo = 0, stack_hi = 0;
    bool own_stack = false;
    u32 tls = 0;             // bionic-style TLS block: slot 0 self, slot 1 thread id, slot 2 &errno
    u32 errno_addr = 0;
    u32 tpidrurw = 0;
    u32 pending_svc = 0;
    u32 exc_pc = 0;
    int exc = -1;
    int depth = 0;
    // JNI state (jni_bridge.cpp)
    JNIEnv* env = nullptr;
    std::vector<jobject> locals;                    // guest local handle n refers to locals[n]
    std::vector<u32> local_free;                    // reusable slots in the innermost frame
    std::vector<std::pair<size_t, size_t>> frames;  // {locals size, local_free size} at frame entry
    // pthread_key values live in guest memory, in the thread's TLS block (so pthread_getspecific runs as
    // guest code, without leaving the JIT)
    static constexpr u32 KEYS_OFF = 0x110;
    u32 key_addr(u32 k) const { return tls + KEYS_OFF + k * 4; }
    u32 exit_value = 0;
    bool finished = false;
    u32 scratch = 0;         // per-thread guest buffer for functions that return static strings
    // The last thunk calls, for crash reports.
    struct CallRec {
        u32 idx, a0, a1, a2, a3, lr, r0;
    };
    std::array<CallRec, 32> hist{};
    u32 hist_n = 0;
    u32 fault_addr = 0;
    const char* fault_what = nullptr;

    GuestThread();
    ~GuestThread();
    std::array<u32, 16>& regs();
    u32 cpsr();
    void set_cpsr(u32 v);
    std::array<u32, 64>& ext_regs();
};

void init_cpu();
GuestThread& self();
GuestThread* create_guest_thread(u32 stack_lo, u32 stack_size);  // stack 0 = allocate 8 MB
void bind_guest_thread(GuestThread* t);                          // make it the calling thread's
void end_guest_thread();                                         // frees the calling thread's JIT
GuestThread* find_guest_thread(u32 id);
u64 call_guest(u32 fn, std::initializer_list<u32> args);
u64 call_guest_v(u32 fn, const u32* args, int n);
void invalidate_code(u32 addr, u32 size);
u32 guest_scratch(GuestThread& t, u32 size);  // 4 KB per-thread buffer

// ---- thunks ----
using Invoker = void (*)(GuestThread&, void*);
u32 add_thunk(const char* name, Invoker inv, void* fp = nullptr);  // returns the guest stub address
// A function written as guest (ARM) code: the stub jumps to a copy of these instructions.
u32 add_guest_code(const char* name, const u32* insns, u32 count);
u32 find_thunk(const char* name);
u32 thunk_for_missing(const char* name);
void add_data_symbol(const char* name, u32 addr);
u32 find_data_symbol(const char* name);
const char* thunk_name_at(u32 addr);  // for logging

inline u32 arg(GuestThread& t, int i) {
    auto& r = t.regs();
    return i < 4 ? r[i] : g::rd32(r[13] + 4 * (i - 4));
}
inline void ret(GuestThread& t, u32 v) { t.regs()[0] = v; }
inline void ret64(GuestThread& t, u64 v) {
    t.regs()[0] = static_cast<u32>(v);
    t.regs()[1] = static_cast<u32>(v >> 32);
}

// Reads guest arguments in AAPCS (softfp) order: 32-bit words in r0-r3 then the stack;
// 64-bit values start on an even register / 8-byte stack slot.
struct ArgCursor {
    GuestThread& t;
    int i = 0;
    u32 word() { return arg(t, i++); }
    u64 dword() {
        i = (i + 1) & ~1;
        u64 lo = word();
        u64 hi = word();
        return lo | (hi << 32);
    }
};

// Converting host lambdas into thunks. Parameter types describe the guest call:
//  - integer types are one 32-bit word (use u32/s32 for size_t, long, off_t, time_t...)
//  - float is one word, double is two
//  - pointer types are guest pointers, converted to host pointers into guest memory
// Return values follow the same rules (pointers must point into guest memory).
template <class T>
T read_arg(ArgCursor& c) {
    if constexpr (std::is_same_v<T, float>) {
        return std::bit_cast<float>(c.word());
    } else if constexpr (std::is_same_v<T, double>) {
        return std::bit_cast<double>(c.dword());
    } else if constexpr (std::is_pointer_v<T>) {
        return reinterpret_cast<T>(g::h(c.word()));
    } else if constexpr (std::is_same_v<T, u64> || std::is_same_v<T, s64>) {
        return static_cast<T>(c.dword());
    } else {
        static_assert(std::is_integral_v<T> || std::is_enum_v<T>);
        static_assert(sizeof(T) <= 4, "use u32/s32 for guest long/size_t");
        return static_cast<T>(c.word());
    }
}

template <class R>
void write_ret(GuestThread& t, R v) {
    if constexpr (std::is_same_v<R, float>) {
        ret(t, std::bit_cast<u32>(v));
    } else if constexpr (std::is_same_v<R, double>) {
        ret64(t, std::bit_cast<u64>(v));
    } else if constexpr (std::is_pointer_v<R>) {
        ret(t, g::to_guest(v));
    } else if constexpr (std::is_same_v<R, u64> || std::is_same_v<R, s64>) {
        ret64(t, static_cast<u64>(v));
    } else {
        ret(t, static_cast<u32>(v));
    }
}

template <class R, class... A>
void invoke_typed(GuestThread& t, void* fp) {
    auto f = reinterpret_cast<R (*)(A...)>(fp);
    ArgCursor c{t};
    std::tuple<A...> args{read_arg<A>(c)...};  // braced init keeps left-to-right order
    if constexpr (std::is_void_v<R>) {
        std::apply(f, args);
    } else {
        write_ret<R>(t, std::apply(f, args));
    }
}

template <class R, class... A>
u32 reg(const char* name, R (*f)(A...)) {
    return add_thunk(name, &invoke_typed<R, A...>, reinterpret_cast<void*>(f));
}
inline u32 reg_raw(const char* name, void (*f)(GuestThread&)) {
    return add_thunk(
        name, [](GuestThread& t, void* fp) { reinterpret_cast<void (*)(GuestThread&)>(fp)(t); },
        reinterpret_cast<void*>(f));
}

// thunk modules
void register_libc();
void register_jni();
void register_gl();
