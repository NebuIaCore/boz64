// JNI for guest code. The guest gets its own JNIEnv / JavaVM function tables whose entries are
// thunks into the real (64-bit) JNI. Java objects are passed to the guest as 32-bit handles:
//   (index << 2) | 1  local reference, per thread
//   (index << 2) | 2  global reference
//   (index << 2) | 3  weak global reference
// Native methods the guest registers get a host entry stub (native_stubs.S) that converts the
// call back into a guest call.
#include <jni.h>

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include "cpu.h"
#include "frame.h"
#include "elf.h"

JavaVM* g_vm = nullptr;
u32 g_guest_vm = 0, g_guest_env = 0;

namespace {
constexpr u32 K_LOCAL = 1, K_GLOBAL = 2, K_WEAK = 3;

std::mutex glob_lock;
std::vector<jobject> globals{nullptr};
std::vector<u32> glob_free;

JNIEnv* env(GuestThread& t) {
    if (!t.env) {
        JNIEnv* e = nullptr;
        if (g_vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK)
            fatal("guest used JNI on a thread that is not attached to Java");
        t.env = e;
    }
    return t.env;
}

jobject H(GuestThread& t, u32 h) {
    if (!h) return nullptr;
    u32 kind = h & 3, idx = h >> 2;
    if (kind == K_LOCAL) {
        if (idx >= t.locals.size()) {
            LOGE("guest used a stale local reference 0x%x", h);
            return nullptr;
        }
        return t.locals[idx];
    }
    if (kind == 0) {
        LOGE("guest passed a bad reference 0x%x", h);
        return nullptr;
    }
    std::lock_guard lk(glob_lock);
    return idx < globals.size() ? globals[idx] : nullptr;
}

u32 L(GuestThread& t, jobject o) {
    if (!o) return 0;
    u32 idx;
    if (!t.local_free.empty()) {
        idx = t.local_free.back();
        t.local_free.pop_back();
        t.locals[idx] = o;
    } else {
        idx = static_cast<u32>(t.locals.size());
        t.locals.push_back(o);
    }
    return (idx << 2) | K_LOCAL;
}

u32 G(jobject o, u32 kind) {
    if (!o) return 0;
    std::lock_guard lk(glob_lock);
    u32 idx;
    if (!glob_free.empty()) {
        idx = glob_free.back();
        glob_free.pop_back();
        globals[idx] = o;
    } else {
        idx = static_cast<u32>(globals.size());
        globals.push_back(o);
    }
    return (idx << 2) | kind;
}

void drop_global(u32 h) {
    std::lock_guard lk(glob_lock);
    u32 idx = h >> 2;
    if (idx && idx < globals.size() && globals[idx]) {
        globals[idx] = nullptr;
        glob_free.push_back(idx);
    }
}

void push_frame(GuestThread& t) { t.frames.emplace_back(t.locals.size(), t.local_free.size()); }
void pop_frame(GuestThread& t) {  // host references are released by the JVM (or PopLocalFrame)
    if (t.frames.empty()) return;
    auto [n, f] = t.frames.back();
    t.frames.pop_back();
    t.locals.resize(n);
    t.local_free.resize(std::min(f, t.local_free.size()));
}

// ---- method and field IDs ----
struct MInfo {
    jmethodID id;
    std::string params;  // one char per parameter: ZBCSIJFDL
    char ret;
    bool is_swap = false;  // LoaderView.glSwapBuffers: the end of a frame (frame.cpp paces it)
    // LoaderThread.touchSetWait(ms): after every touch event the UI thread then sleeps that long, which
    // holds touch input (and so the camera) to about 1000/ms updates a second. Always set to 0.
    bool is_touch_wait = false;
    // LoaderThread.fixOrientation(s3e lock): the game locks the screen to landscape (4, LANDSCAPE_FIXED), so it
    // never turns over when the phone does. Landscape that follows the sensor (2, LANDSCAPE) is used instead.
    bool is_fix_orientation = false;
};
struct FInfo {
    jfieldID id;
    char type;
};
constexpr u32 MAX_IDS = 1 << 16;
MInfo minfo[MAX_IDS];
FInfo finfo[MAX_IDS];
std::atomic<u32> nm{1}, nf{1};
std::mutex id_lock;
std::unordered_map<jmethodID, u32> mid_map;
std::unordered_map<jfieldID, u32> fid_map;

bool parse_sig(const char* sig, std::string& params, char& ret) {
    const char* p = sig;
    if (*p++ != '(') return false;
    params.clear();
    while (*p && *p != ')') {
        char c = *p;
        if (c == '[') {
            while (*p == '[') p++;
            if (*p == 'L') p = strchr(p, ';');
            if (!p) return false;
            p++;
            params += 'L';
        } else if (c == 'L') {
            p = strchr(p, ';');
            if (!p) return false;
            p++;
            params += 'L';
        } else {
            params += c;
            p++;
        }
    }
    if (*p != ')') return false;
    p++;
    ret = (*p == '[' || *p == 'L') ? 'L' : *p;
    return true;
}

u32 intern_method(jmethodID id, const char* sig) {
    if (!id) return 0;
    std::lock_guard lk(id_lock);
    auto it = mid_map.find(id);
    if (it != mid_map.end()) return it->second;
    u32 g = nm.load();
    if (g >= MAX_IDS) fatal("too many JNI method IDs");
    minfo[g].id = id;
    if (!sig || !parse_sig(sig, minfo[g].params, minfo[g].ret)) {
        LOGW("JNI method with unknown signature %s", sig ? sig : "(reflected)");
        minfo[g].params.clear();
        minfo[g].ret = 'V';
    }
    nm.store(g + 1);
    mid_map[id] = g;
    return g;
}
const MInfo& method(u32 g) {
    if (!g || g >= nm.load()) fatal("guest used a bad JNI method ID %u", g);
    return minfo[g];
}

u32 intern_field(jfieldID id, const char* sig) {
    if (!id) return 0;
    std::lock_guard lk(id_lock);
    auto it = fid_map.find(id);
    if (it != fid_map.end()) return it->second;
    u32 g = nf.load();
    if (g >= MAX_IDS) fatal("too many JNI field IDs");
    finfo[g] = {id, (sig[0] == '[' || sig[0] == 'L') ? 'L' : sig[0]};
    nf.store(g + 1);
    fid_map[id] = g;
    return g;
}
const FInfo& field(u32 g) {
    if (!g || g >= nf.load()) fatal("guest used a bad JNI field ID %u", g);
    return finfo[g];
}

// ---- arguments for Call*Method ----
// mode 0: "..." (promoted: float -> double), 1: va_list, 2: jvalue array (8 bytes per entry)
void read_args(GuestThread& t, const MInfo& m, int mode, int first, std::vector<jvalue>& out) {
    ArgCursor c{t, first};
    u32 ap = arg(t, first);
    auto word = [&]() -> u32 {
        if (mode == 0) return c.word();
        u32 v = g::rd32(ap);
        ap += 4;
        return v;
    };
    auto dword = [&]() -> u64 {
        if (mode == 0) return c.dword();
        ap = (ap + 7) & ~7u;
        u64 v = g::rd64(ap);
        ap += 8;
        return v;
    };
    out.resize(m.params.size());
    for (size_t i = 0; i < m.params.size(); i++) {
        jvalue& v = out[i];
        v.j = 0;
        char ty = m.params[i];
        if (mode == 2) {
            u32 a = ap + 8 * i;
            switch (ty) {
            case 'Z': v.z = g::rd8(a); break;
            case 'B': v.b = static_cast<s8>(g::rd8(a)); break;
            case 'C': v.c = g::rd16(a); break;
            case 'S': v.s = static_cast<s16>(g::rd16(a)); break;
            case 'I': v.i = static_cast<s32>(g::rd32(a)); break;
            case 'J': v.j = static_cast<s64>(g::rd64(a)); break;
            case 'F': v.f = std::bit_cast<float>(g::rd32(a)); break;
            case 'D': v.d = std::bit_cast<double>(g::rd64(a)); break;
            default: v.l = H(t, g::rd32(a)); break;
            }
            continue;
        }
        switch (ty) {
        case 'Z': v.z = static_cast<u8>(word()); break;
        case 'B': v.b = static_cast<s8>(word()); break;
        case 'C': v.c = static_cast<u16>(word()); break;
        case 'S': v.s = static_cast<s16>(word()); break;
        case 'I': v.i = static_cast<s32>(word()); break;
        case 'J': v.j = static_cast<s64>(dword()); break;
        case 'F': v.f = static_cast<float>(std::bit_cast<double>(dword())); break;
        case 'D': v.d = std::bit_cast<double>(dword()); break;
        default: v.l = H(t, word()); break;
        }
    }
}

// kind 0: virtual (env, obj, mid, ...)  1: nonvirtual (env, obj, cls, mid, ...)  2: static (env, cls, mid, ...)
template <int kind, char rt, int mode>
void jcall(GuestThread& t) {
    JNIEnv* e = env(t);
    const int midx = kind == 1 ? 3 : 2;
    const MInfo& m = method(arg(t, midx));
    std::vector<jvalue> a;
    read_args(t, m, mode, midx + 1, a);
    jobject o = H(t, arg(t, 1));
    jclass cl = kind == 1 ? static_cast<jclass>(H(t, arg(t, 2))) : nullptr;
    const jvalue* av = a.data();
#define CALLX(T)                                                                                            \
    (kind == 0   ? e->Call##T##MethodA(o, m.id, av)                                                       \
     : kind == 1 ? e->CallNonvirtual##T##MethodA(o, cl, m.id, av)                                         \
                 : e->CallStatic##T##MethodA(static_cast<jclass>(o), m.id, av))
    if constexpr (rt == 'L') ret(t, L(t, CALLX(Object)));
    else if constexpr (rt == 'Z') ret(t, CALLX(Boolean));
    else if constexpr (rt == 'B') ret(t, static_cast<u32>(static_cast<s32>(CALLX(Byte))));
    else if constexpr (rt == 'C') ret(t, CALLX(Char));
    else if constexpr (rt == 'S') ret(t, static_cast<u32>(static_cast<s32>(CALLX(Short))));
    else if constexpr (rt == 'I') ret(t, static_cast<u32>(CALLX(Int)));
    else if constexpr (rt == 'J') ret64(t, static_cast<u64>(CALLX(Long)));
    else if constexpr (rt == 'F') ret(t, std::bit_cast<u32>(CALLX(Float)));
    else if constexpr (rt == 'D') ret64(t, std::bit_cast<u64>(CALLX(Double)));
    else if (m.is_swap) {
        frame_swap_begin();
        CALLX(Void);
        frame_swap_end();
    } else if (m.is_touch_wait && !a.empty()) {
        LOGI("game set the touch wait to %d ms; using 0", a[0].i);
        a[0].i = 0;
        CALLX(Void);
    } else if (m.is_fix_orientation && !a.empty()) {
        constexpr jint LANDSCAPE = 2, LANDSCAPE_FIXED = 4;  // s3eSurfaceOrientationLock
        LOGI("game locked the screen orientation to %d%s", a[0].i, a[0].i == LANDSCAPE_FIXED ? "; using landscape that follows the sensor" : "");
        if (a[0].i == LANDSCAPE_FIXED) a[0].i = LANDSCAPE;
        CALLX(Void);
    } else CALLX(Void);
#undef CALLX
}

template <int mode>
void jnewobject(GuestThread& t) {
    JNIEnv* e = env(t);
    const MInfo& m = method(arg(t, 2));
    std::vector<jvalue> a;
    read_args(t, m, mode, 3, a);
    ret(t, L(t, e->NewObjectA(static_cast<jclass>(H(t, arg(t, 1))), m.id, a.data())));
}

template <char ty, bool is_static>
void jget(GuestThread& t) {
    JNIEnv* e = env(t);
    jobject o = H(t, arg(t, 1));
    jfieldID f = field(arg(t, 2)).id;
    auto c = static_cast<jclass>(o);
#define GETX(T) (is_static ? e->GetStatic##T##Field(c, f) : e->Get##T##Field(o, f))
    if constexpr (ty == 'L') ret(t, L(t, GETX(Object)));
    else if constexpr (ty == 'Z') ret(t, GETX(Boolean));
    else if constexpr (ty == 'B') ret(t, static_cast<u32>(static_cast<s32>(GETX(Byte))));
    else if constexpr (ty == 'C') ret(t, GETX(Char));
    else if constexpr (ty == 'S') ret(t, static_cast<u32>(static_cast<s32>(GETX(Short))));
    else if constexpr (ty == 'I') ret(t, static_cast<u32>(GETX(Int)));
    else if constexpr (ty == 'J') ret64(t, static_cast<u64>(GETX(Long)));
    else if constexpr (ty == 'F') ret(t, std::bit_cast<u32>(GETX(Float)));
    else if constexpr (ty == 'D') ret64(t, std::bit_cast<u64>(GETX(Double)));
#undef GETX
}

template <char ty, bool is_static>
void jset(GuestThread& t) {
    JNIEnv* e = env(t);
    jobject o = H(t, arg(t, 1));
    jfieldID f = field(arg(t, 2)).id;
    auto c = static_cast<jclass>(o);
    ArgCursor a{t, 3};
#define SETX(T, v) (is_static ? e->SetStatic##T##Field(c, f, v) : e->Set##T##Field(o, f, v))
    if constexpr (ty == 'L') SETX(Object, H(t, a.word()));
    else if constexpr (ty == 'Z') SETX(Boolean, static_cast<jboolean>(a.word()));
    else if constexpr (ty == 'B') SETX(Byte, static_cast<jbyte>(a.word()));
    else if constexpr (ty == 'C') SETX(Char, static_cast<jchar>(a.word()));
    else if constexpr (ty == 'S') SETX(Short, static_cast<jshort>(a.word()));
    else if constexpr (ty == 'I') SETX(Int, static_cast<jint>(a.word()));
    else if constexpr (ty == 'J') SETX(Long, static_cast<jlong>(a.dword()));
    else if constexpr (ty == 'F') SETX(Float, std::bit_cast<float>(a.word()));
    else if constexpr (ty == 'D') SETX(Double, std::bit_cast<double>(a.dword()));
#undef SETX
}

// ---- primitive arrays ----
template <char ty> struct Prim;
#define PRIM(ch, T, Name)                                                                                    \
    template <> struct Prim<ch> {                                                                           \
        using type = T;                                                                                      \
        static jarray make(JNIEnv* e, jsize n) { return e->New##Name##Array(n); }                           \
        static void get(JNIEnv* e, jarray a, jsize s, jsize n, T* b) {                                       \
            e->Get##Name##ArrayRegion(static_cast<T##Array>(a), s, n, b);                                    \
        }                                                                                                    \
        static void set(JNIEnv* e, jarray a, jsize s, jsize n, const T* b) {                                 \
            e->Set##Name##ArrayRegion(static_cast<T##Array>(a), s, n, b);                                    \
        }                                                                                                    \
    };
PRIM('Z', jboolean, Boolean)
PRIM('B', jbyte, Byte)
PRIM('C', jchar, Char)
PRIM('S', jshort, Short)
PRIM('I', jint, Int)
PRIM('J', jlong, Long)
PRIM('F', jfloat, Float)
PRIM('D', jdouble, Double)
#undef PRIM

template <char ty>
void jnewarray(GuestThread& t) {
    ret(t, L(t, Prim<ty>::make(env(t), static_cast<jsize>(arg(t, 1)))));
}
template <char ty>
void jgetelements(GuestThread& t) {  // (env, array, isCopy*) -> guest copy of the array
    JNIEnv* e = env(t);
    auto a = static_cast<jarray>(H(t, arg(t, 1)));
    jsize n = e->GetArrayLength(a);
    u32 buf = g::malloc(n * sizeof(typename Prim<ty>::type) + 8);
    Prim<ty>::get(e, a, 0, n, static_cast<typename Prim<ty>::type*>(g::h(buf)));
    if (arg(t, 2)) g::wr8(arg(t, 2), 1);
    ret(t, buf);
}
template <char ty>
void jreleaseelements(GuestThread& t) {  // (env, array, elems, mode)
    JNIEnv* e = env(t);
    auto a = static_cast<jarray>(H(t, arg(t, 1)));
    u32 buf = arg(t, 2), mode = arg(t, 3);
    if (!buf) return;
    if (mode != JNI_ABORT) Prim<ty>::set(e, a, 0, e->GetArrayLength(a), static_cast<typename Prim<ty>::type*>(g::h(buf)));
    if (mode != JNI_COMMIT) g::free(buf);
}
template <char ty>
void jgetregion(GuestThread& t) {  // (env, array, start, len, buf)
    Prim<ty>::get(env(t), static_cast<jarray>(H(t, arg(t, 1))), static_cast<jsize>(arg(t, 2)),
                  static_cast<jsize>(arg(t, 3)), static_cast<typename Prim<ty>::type*>(g::h(arg(t, 4))));
}
template <char ty>
void jsetregion(GuestThread& t) {
    Prim<ty>::set(env(t), static_cast<jarray>(H(t, arg(t, 1))), static_cast<jsize>(arg(t, 2)),
                  static_cast<jsize>(arg(t, 3)), static_cast<const typename Prim<ty>::type*>(g::h(arg(t, 4))));
}

// GetPrimitiveArrayCritical doesn't say the element type, so look at the array's class.
std::mutex crit_lock;
std::unordered_map<u32, char> crit_type;
char array_type(JNIEnv* e, jarray a) {
    static jclass cls[8];
    static const char* names[8] = {"[Z", "[B", "[C", "[S", "[I", "[J", "[F", "[D"};
    static const char codes[8] = {'Z', 'B', 'C', 'S', 'I', 'J', 'F', 'D'};
    for (int i = 0; i < 8; i++) {
        if (!cls[i]) cls[i] = static_cast<jclass>(e->NewGlobalRef(e->FindClass(names[i])));
        if (e->IsInstanceOf(a, cls[i])) return codes[i];
    }
    return 'B';
}
int prim_size(char ty) { return ty == 'Z' || ty == 'B' ? 1 : ty == 'C' || ty == 'S' ? 2 : ty == 'J' || ty == 'D' ? 8 : 4; }

void jgetcritical(GuestThread& t) {
    JNIEnv* e = env(t);
    auto a = static_cast<jarray>(H(t, arg(t, 1)));
    char ty = array_type(e, a);
    jsize n = e->GetArrayLength(a);
    u32 buf = g::malloc(n * prim_size(ty) + 8);
    void* p = e->GetPrimitiveArrayCritical(a, nullptr);
    memcpy(g::h(buf), p, n * prim_size(ty));
    e->ReleasePrimitiveArrayCritical(a, p, JNI_ABORT);
    if (arg(t, 2)) g::wr8(arg(t, 2), 1);
    std::lock_guard lk(crit_lock);
    crit_type[buf] = ty;
    ret(t, buf);
}
void jreleasecritical(GuestThread& t) {
    JNIEnv* e = env(t);
    auto a = static_cast<jarray>(H(t, arg(t, 1)));
    u32 buf = arg(t, 2), mode = arg(t, 3);
    if (!buf) return;
    char ty;
    {
        std::lock_guard lk(crit_lock);
        ty = crit_type[buf];
        if (mode != JNI_COMMIT) crit_type.erase(buf);
    }
    if (mode != JNI_ABORT) {
        jsize n = e->GetArrayLength(a);
        void* p = e->GetPrimitiveArrayCritical(a, nullptr);
        memcpy(p, g::h(buf), n * prim_size(ty));
        e->ReleasePrimitiveArrayCritical(a, p, 0);
    }
    if (mode != JNI_COMMIT) g::free(buf);
}

// ---- native methods registered by the guest ----
struct NativeEntry {
    u32 fn;
    std::string params;
    char ret;
    std::string name;
    bool touch = false;
};
constexpr int MAX_NATIVES = 512;
NativeEntry natives[MAX_NATIVES];
std::atomic<int> nnatives{0};
}  // namespace

#ifdef BOZ_PC
// The PC build has no Java: its stand-in JVM (pc/src/jvm.cpp) calls the natives the guest registered directly, with
// jvalue arguments. boz_native_stubs is only an address range there (one 8-byte slot per native), so a registered
// function pointer still says which native it is.
extern "C" char boz_native_stubs[];
jvalue boz_native_call(const void* fn, JNIEnv* env, jobject thiz, const jvalue* args) {
    jvalue result{};
    auto idx = static_cast<int>((static_cast<const char*>(fn) - boz_native_stubs) / 8);
    if (idx < 0 || idx >= nnatives.load()) fatal("call of an unknown native method %p", fn);
    const NativeEntry& ne = natives[idx];
    GuestThread& t = self();
    JNIEnv* saved_env = t.env;
    t.env = env;
    push_frame(t);
    std::vector<u32> w;
    w.reserve(16);
    w.push_back(g_guest_env);
    w.push_back(L(t, thiz));
    auto align = [&] {
        if (w.size() & 1) w.push_back(0);
    };
    for (size_t i = 0; i < ne.params.size(); i++) {
        const jvalue& v = args[i];
        switch (ne.params[i]) {
        case 'Z': w.push_back(v.z); break;
        case 'B': w.push_back(static_cast<u32>(static_cast<s32>(v.b))); break;
        case 'C': w.push_back(v.c); break;
        case 'S': w.push_back(static_cast<u32>(static_cast<s32>(v.s))); break;
        case 'I': w.push_back(static_cast<u32>(v.i)); break;
        case 'J':
            align();
            w.push_back(static_cast<u32>(static_cast<u64>(v.j)));
            w.push_back(static_cast<u32>(static_cast<u64>(v.j) >> 32));
            break;
        case 'F': w.push_back(std::bit_cast<u32>(v.f)); break;
        case 'D': {
            u64 d = std::bit_cast<u64>(v.d);
            align();
            w.push_back(static_cast<u32>(d));
            w.push_back(static_cast<u32>(d >> 32));
            break;
        }
        default: w.push_back(L(t, v.l)); break;
        }
    }
    TRACE("native %s", ne.name.c_str());
    u64 r = call_guest_v(ne.fn, w.data(), static_cast<int>(w.size()));
    switch (ne.ret) {
    case 'V': break;
    case 'Z': result.z = static_cast<jboolean>(r & 0xff); break;
    case 'B': result.b = static_cast<jbyte>(r); break;
    case 'C': result.c = static_cast<jchar>(r); break;
    case 'S': result.s = static_cast<jshort>(r); break;
    case 'I': result.i = static_cast<jint>(r); break;
    case 'J': result.j = static_cast<jlong>(r); break;
    case 'F': result.f = std::bit_cast<float>(static_cast<u32>(r)); break;
    case 'D': result.d = std::bit_cast<double>(r); break;
    default: result.l = H(t, static_cast<u32>(r)); break;
    }
    pop_frame(t);
    t.env = saved_env ? saved_env : t.env;
    return result;
}
#else
extern "C" char boz_native_stubs[];  // native_stubs.S: MAX_NATIVES entries of 8 bytes

// Called by the stubs with x0-x7 and d0-d7 saved in regs[0..15]. The double/float result goes
// back in regs[8] (d0).
extern "C" u64 boz_native_dispatch(u32 idx, u64* regs, u64* stack) {
    const NativeEntry& ne = natives[idx];
    GuestThread& t = self();
    JNIEnv* saved_env = t.env;
    t.env = reinterpret_cast<JNIEnv*>(regs[0]);
    push_frame(t);
    std::vector<u32> w;
    w.reserve(16);
    w.push_back(g_guest_env);
    w.push_back(L(t, reinterpret_cast<jobject>(regs[1])));
    int xi = 2, di = 0, si = 0;
    auto next_x = [&]() -> u64 { return xi < 8 ? regs[xi++] : stack[si++]; };
    auto next_d = [&]() -> u64 { return di < 8 ? regs[8 + di++] : stack[si++]; };
    auto align = [&] {
        if (w.size() & 1) w.push_back(0);
    };
    for (char p : ne.params) {
        switch (p) {
        case 'Z': w.push_back(static_cast<u8>(next_x())); break;
        case 'B': w.push_back(static_cast<u32>(static_cast<s32>(static_cast<s8>(next_x())))); break;
        case 'C': w.push_back(static_cast<u16>(next_x())); break;
        case 'S': w.push_back(static_cast<u32>(static_cast<s32>(static_cast<s16>(next_x())))); break;
        case 'I': w.push_back(static_cast<u32>(next_x())); break;
        case 'J': {
            u64 v = next_x();
            align();
            w.push_back(static_cast<u32>(v));
            w.push_back(static_cast<u32>(v >> 32));
            break;
        }
        case 'F': w.push_back(static_cast<u32>(next_d())); break;
        case 'D': {
            u64 v = next_d();
            align();
            w.push_back(static_cast<u32>(v));
            w.push_back(static_cast<u32>(v >> 32));
            break;
        }
        default: w.push_back(L(t, reinterpret_cast<jobject>(next_x()))); break;
        }
    }
    TRACE("native %s", ne.name.c_str());
    timespec t0{}, t1{};
    if (ne.touch) clock_gettime(CLOCK_MONOTONIC, &t0);
    u64 r = call_guest_v(ne.fn, w.data(), static_cast<int>(w.size()));
    if (ne.touch) {
        clock_gettime(CLOCK_MONOTONIC, &t1);
        frame_note_touch(static_cast<u64>((t1.tv_sec - t0.tv_sec) * 1000000000ll + (t1.tv_nsec - t0.tv_nsec)), w.size() >= 6 ? static_cast<int>(w[4]) : -1);
    }
    u64 result = 0;
    switch (ne.ret) {
    case 'V': break;
    case 'Z': result = r & 0xff; break;
    case 'B': result = static_cast<u64>(static_cast<s64>(static_cast<s8>(r))); break;
    case 'C': result = static_cast<u16>(r); break;
    case 'S': result = static_cast<u64>(static_cast<s64>(static_cast<s16>(r))); break;
    case 'I': result = static_cast<u64>(static_cast<s64>(static_cast<s32>(r))); break;
    case 'J': result = r; break;
    case 'F': regs[8] = static_cast<u32>(r); break;
    case 'D': regs[8] = r; break;
    default: result = reinterpret_cast<u64>(H(t, static_cast<u32>(r))); break;
    }
    pop_frame(t);
    t.env = saved_env ? saved_env : t.env;
    return result;
}
#endif  // BOZ_PC

namespace {

void register_natives(GuestThread& t) {  // (env, clazz, const JNINativeMethod* methods, n)
    JNIEnv* e = env(t);
    auto cls = static_cast<jclass>(H(t, arg(t, 1)));
    u32 m = arg(t, 2), n = arg(t, 3);
    std::vector<JNINativeMethod> hm(n);
    for (u32 i = 0; i < n; i++) {
        const char* name = g::str(g::rd32(m + 12 * i));
        const char* sig = g::str(g::rd32(m + 12 * i + 4));
        u32 fn = g::rd32(m + 12 * i + 8);
        int idx = nnatives++;
        if (idx >= MAX_NATIVES) fatal("too many native methods");
        NativeEntry& ne = natives[idx];
        ne.fn = fn;
        ne.name = name;
        ne.touch = ne.name == "onMotionEvent";
        if (!parse_sig(sig, ne.params, ne.ret)) fatal("bad native signature %s", sig);
        hm[i] = {name, sig, boz_native_stubs + 8 * idx};
        TRACE("RegisterNatives %s%s -> %s", name, sig, elf_describe(fn).c_str());
    }
    jint r = e->RegisterNatives(cls, hm.data(), static_cast<jint>(n));
    if (r != JNI_OK) {
        LOGE("RegisterNatives failed (%d)", r);
        e->ExceptionDescribe();
        e->ExceptionClear();
    }
    ret(t, static_cast<u32>(r));
}

// JNIEnv function table index -> implementation
void fill_env_table(u32 table) {
    std::vector<std::pair<int, void (*)(GuestThread&)>> f;
    auto add = [&](int i, void (*fn)(GuestThread&)) { f.emplace_back(i, fn); };
#define E(t) env(t)
    add(4, [](GuestThread& t) { ret(t, E(t)->GetVersion()); });
    add(5, [](GuestThread& t) { ret(t, 0); });  // DefineClass
    add(6, [](GuestThread& t) { ret(t, L(t, E(t)->FindClass(g::str(arg(t, 1))))); });
    add(7, [](GuestThread& t) { ret(t, intern_method(E(t)->FromReflectedMethod(H(t, arg(t, 1))), nullptr)); });
    add(8, [](GuestThread& t) { ret(t, intern_field(E(t)->FromReflectedField(H(t, arg(t, 1))), "L")); });
    add(9, [](GuestThread& t) {
        ret(t, L(t, E(t)->ToReflectedMethod(static_cast<jclass>(H(t, arg(t, 1))), method(arg(t, 2)).id, arg(t, 3) & 0xff)));
    });
    add(10, [](GuestThread& t) { ret(t, L(t, E(t)->GetSuperclass(static_cast<jclass>(H(t, arg(t, 1)))))); });
    add(11, [](GuestThread& t) {
        ret(t, E(t)->IsAssignableFrom(static_cast<jclass>(H(t, arg(t, 1))), static_cast<jclass>(H(t, arg(t, 2)))));
    });
    add(12, [](GuestThread& t) {
        ret(t, L(t, E(t)->ToReflectedField(static_cast<jclass>(H(t, arg(t, 1))), field(arg(t, 2)).id, arg(t, 3) & 0xff)));
    });
    add(13, [](GuestThread& t) { ret(t, E(t)->Throw(static_cast<jthrowable>(H(t, arg(t, 1))))); });
    add(14, [](GuestThread& t) { ret(t, E(t)->ThrowNew(static_cast<jclass>(H(t, arg(t, 1))), g::str(arg(t, 2)))); });
    add(15, [](GuestThread& t) { ret(t, L(t, E(t)->ExceptionOccurred())); });
    add(16, [](GuestThread& t) { E(t)->ExceptionDescribe(); });
    add(17, [](GuestThread& t) { E(t)->ExceptionClear(); });
    add(18, [](GuestThread& t) { fatal("guest JNI FatalError: %s", g::str(arg(t, 1))); });
    add(19, [](GuestThread& t) {  // PushLocalFrame
        jint r = E(t)->PushLocalFrame(static_cast<jint>(arg(t, 1)));
        if (r == 0) push_frame(t);
        ret(t, r);
    });
    add(20, [](GuestThread& t) {  // PopLocalFrame
        jobject res = H(t, arg(t, 1));
        jobject r = E(t)->PopLocalFrame(res);
        pop_frame(t);
        ret(t, L(t, r));
    });
    add(21, [](GuestThread& t) { ret(t, G(E(t)->NewGlobalRef(H(t, arg(t, 1))), K_GLOBAL)); });
    add(22, [](GuestThread& t) {  // DeleteGlobalRef
        u32 h = arg(t, 1);
        if ((h & 3) != K_GLOBAL) return;
        if (jobject o = H(t, h)) E(t)->DeleteGlobalRef(o);
        drop_global(h);
    });
    add(23, [](GuestThread& t) {  // DeleteLocalRef
        u32 h = arg(t, 1);
        if ((h & 3) != K_LOCAL) return;
        u32 idx = h >> 2;
        if (idx >= t.locals.size() || !t.locals[idx]) return;
        E(t)->DeleteLocalRef(t.locals[idx]);
        t.locals[idx] = nullptr;
        size_t frame_start = t.frames.empty() ? 0 : t.frames.back().first;
        if (idx >= frame_start) t.local_free.push_back(idx);
    });
    add(24, [](GuestThread& t) { ret(t, E(t)->IsSameObject(H(t, arg(t, 1)), H(t, arg(t, 2)))); });
    add(25, [](GuestThread& t) { ret(t, L(t, E(t)->NewLocalRef(H(t, arg(t, 1))))); });
    add(26, [](GuestThread& t) { ret(t, E(t)->EnsureLocalCapacity(static_cast<jint>(arg(t, 1)))); });
    add(27, [](GuestThread& t) { ret(t, L(t, E(t)->AllocObject(static_cast<jclass>(H(t, arg(t, 1)))))); });
    add(28, jnewobject<0>);
    add(29, jnewobject<1>);
    add(30, jnewobject<2>);
    add(31, [](GuestThread& t) { ret(t, L(t, E(t)->GetObjectClass(H(t, arg(t, 1))))); });
    add(32, [](GuestThread& t) { ret(t, E(t)->IsInstanceOf(H(t, arg(t, 1)), static_cast<jclass>(H(t, arg(t, 2))))); });
    add(33, [](GuestThread& t) {
        const char* sig = g::str(arg(t, 3));
        const char* name = g::str(arg(t, 2));
        jmethodID id = E(t)->GetMethodID(static_cast<jclass>(H(t, arg(t, 1))), name, sig);
        u32 g = intern_method(id, sig);
        if (id && strcmp(name, "glSwapBuffers") == 0) minfo[g].is_swap = true;
        if (id && strcmp(name, "touchSetWait") == 0) minfo[g].is_touch_wait = true;
        if (id && strcmp(name, "fixOrientation") == 0 && strcmp(sig, "(I)V") == 0) minfo[g].is_fix_orientation = true;
        ret(t, g);
    });
    add(113, [](GuestThread& t) {
        const char* sig = g::str(arg(t, 3));
        jmethodID id = E(t)->GetStaticMethodID(static_cast<jclass>(H(t, arg(t, 1))), g::str(arg(t, 2)), sig);
        ret(t, intern_method(id, sig));
    });
    add(94, [](GuestThread& t) {
        const char* sig = g::str(arg(t, 3));
        jfieldID id = E(t)->GetFieldID(static_cast<jclass>(H(t, arg(t, 1))), g::str(arg(t, 2)), sig);
        ret(t, intern_field(id, sig));
    });
    add(144, [](GuestThread& t) {
        const char* sig = g::str(arg(t, 3));
        jfieldID id = E(t)->GetStaticFieldID(static_cast<jclass>(H(t, arg(t, 1))), g::str(arg(t, 2)), sig);
        ret(t, intern_field(id, sig));
    });

    // Call<Type>Method{,V,A}: 34 virtual, 64 nonvirtual, 114 static; types in JNI order.
#define CALLS(base, kind)                                                                     \
    add(base + 0, jcall<kind, 'L', 0>), add(base + 1, jcall<kind, 'L', 1>), add(base + 2, jcall<kind, 'L', 2>);    \
    add(base + 3, jcall<kind, 'Z', 0>), add(base + 4, jcall<kind, 'Z', 1>), add(base + 5, jcall<kind, 'Z', 2>);    \
    add(base + 6, jcall<kind, 'B', 0>), add(base + 7, jcall<kind, 'B', 1>), add(base + 8, jcall<kind, 'B', 2>);    \
    add(base + 9, jcall<kind, 'C', 0>), add(base + 10, jcall<kind, 'C', 1>), add(base + 11, jcall<kind, 'C', 2>);  \
    add(base + 12, jcall<kind, 'S', 0>), add(base + 13, jcall<kind, 'S', 1>), add(base + 14, jcall<kind, 'S', 2>); \
    add(base + 15, jcall<kind, 'I', 0>), add(base + 16, jcall<kind, 'I', 1>), add(base + 17, jcall<kind, 'I', 2>); \
    add(base + 18, jcall<kind, 'J', 0>), add(base + 19, jcall<kind, 'J', 1>), add(base + 20, jcall<kind, 'J', 2>); \
    add(base + 21, jcall<kind, 'F', 0>), add(base + 22, jcall<kind, 'F', 1>), add(base + 23, jcall<kind, 'F', 2>); \
    add(base + 24, jcall<kind, 'D', 0>), add(base + 25, jcall<kind, 'D', 1>), add(base + 26, jcall<kind, 'D', 2>); \
    add(base + 27, jcall<kind, 'V', 0>), add(base + 28, jcall<kind, 'V', 1>), add(base + 29, jcall<kind, 'V', 2>);
    CALLS(34, 0)
    CALLS(64, 1)
    CALLS(114, 2)
#undef CALLS
    // Get/Set<Type>Field: 95/104 instance, 145/154 static; types Object Boolean Byte Char Short Int Long Float Double.
#define FIELDS(gbase, sbase, st)                                                                   \
    add(gbase + 0, jget<'L', st>), add(gbase + 1, jget<'Z', st>), add(gbase + 2, jget<'B', st>);   \
    add(gbase + 3, jget<'C', st>), add(gbase + 4, jget<'S', st>), add(gbase + 5, jget<'I', st>);   \
    add(gbase + 6, jget<'J', st>), add(gbase + 7, jget<'F', st>), add(gbase + 8, jget<'D', st>);   \
    add(sbase + 0, jset<'L', st>), add(sbase + 1, jset<'Z', st>), add(sbase + 2, jset<'B', st>);   \
    add(sbase + 3, jset<'C', st>), add(sbase + 4, jset<'S', st>), add(sbase + 5, jset<'I', st>);   \
    add(sbase + 6, jset<'J', st>), add(sbase + 7, jset<'F', st>), add(sbase + 8, jset<'D', st>);
    FIELDS(95, 104, false)
    FIELDS(145, 154, true)
#undef FIELDS

    // strings
    add(163, [](GuestThread& t) {
        ret(t, L(t, E(t)->NewString(static_cast<const jchar*>(g::h(arg(t, 1))), static_cast<jsize>(arg(t, 2)))));
    });
    add(164, [](GuestThread& t) { ret(t, E(t)->GetStringLength(static_cast<jstring>(H(t, arg(t, 1))))); });
    add(165, [](GuestThread& t) {  // GetStringChars
        JNIEnv* e = E(t);
        auto s = static_cast<jstring>(H(t, arg(t, 1)));
        jsize n = e->GetStringLength(s);
        u32 buf = g::malloc(2 * n + 2);
        e->GetStringRegion(s, 0, n, static_cast<jchar*>(g::h(buf)));
        g::wr16(buf + 2 * n, 0);
        if (arg(t, 2)) g::wr8(arg(t, 2), 1);
        ret(t, buf);
    });
    add(166, [](GuestThread& t) { g::free(arg(t, 2)); });
    add(167, [](GuestThread& t) { ret(t, L(t, E(t)->NewStringUTF(g::str(arg(t, 1))))); });
    add(168, [](GuestThread& t) { ret(t, E(t)->GetStringUTFLength(static_cast<jstring>(H(t, arg(t, 1))))); });
    add(169, [](GuestThread& t) {  // GetStringUTFChars
        JNIEnv* e = E(t);
        auto s = static_cast<jstring>(H(t, arg(t, 1)));
        if (!s) return ret(t, 0);
        const char* c = e->GetStringUTFChars(s, nullptr);
        u32 buf = c ? g::strdup(c) : 0;
        if (c) e->ReleaseStringUTFChars(s, c);
        if (arg(t, 2)) g::wr8(arg(t, 2), 1);
        ret(t, buf);
    });
    add(170, [](GuestThread& t) { g::free(arg(t, 2)); });
    add(220, [](GuestThread& t) {
        E(t)->GetStringRegion(static_cast<jstring>(H(t, arg(t, 1))), static_cast<jsize>(arg(t, 2)),
                              static_cast<jsize>(arg(t, 3)), static_cast<jchar*>(g::h(arg(t, 4))));
    });
    add(221, [](GuestThread& t) {
        E(t)->GetStringUTFRegion(static_cast<jstring>(H(t, arg(t, 1))), static_cast<jsize>(arg(t, 2)),
                                 static_cast<jsize>(arg(t, 3)), static_cast<char*>(g::h(arg(t, 4))));
    });
    add(224, [](GuestThread& t) {  // GetStringCritical
        JNIEnv* e = E(t);
        auto s = static_cast<jstring>(H(t, arg(t, 1)));
        jsize n = e->GetStringLength(s);
        u32 buf = g::malloc(2 * n + 2);
        e->GetStringRegion(s, 0, n, static_cast<jchar*>(g::h(buf)));
        g::wr16(buf + 2 * n, 0);
        if (arg(t, 2)) g::wr8(arg(t, 2), 1);
        ret(t, buf);
    });
    add(225, [](GuestThread& t) { g::free(arg(t, 2)); });

    // arrays
    add(171, [](GuestThread& t) { ret(t, E(t)->GetArrayLength(static_cast<jarray>(H(t, arg(t, 1))))); });
    add(172, [](GuestThread& t) {
        ret(t, L(t, E(t)->NewObjectArray(static_cast<jsize>(arg(t, 1)), static_cast<jclass>(H(t, arg(t, 2))), H(t, arg(t, 3)))));
    });
    add(173, [](GuestThread& t) {
        ret(t, L(t, E(t)->GetObjectArrayElement(static_cast<jobjectArray>(H(t, arg(t, 1))), static_cast<jsize>(arg(t, 2)))));
    });
    add(174, [](GuestThread& t) {
        E(t)->SetObjectArrayElement(static_cast<jobjectArray>(H(t, arg(t, 1))), static_cast<jsize>(arg(t, 2)), H(t, arg(t, 3)));
    });
#define ARRAYS(i, ch)                                                                            \
    add(175 + i, jnewarray<ch>), add(183 + i, jgetelements<ch>), add(191 + i, jreleaseelements<ch>); \
    add(199 + i, jgetregion<ch>), add(207 + i, jsetregion<ch>);
    ARRAYS(0, 'Z')
    ARRAYS(1, 'B')
    ARRAYS(2, 'C')
    ARRAYS(3, 'S')
    ARRAYS(4, 'I')
    ARRAYS(5, 'J')
    ARRAYS(6, 'F')
    ARRAYS(7, 'D')
#undef ARRAYS
    add(222, jgetcritical);
    add(223, jreleasecritical);

    add(215, register_natives);
    add(216, [](GuestThread& t) { ret(t, E(t)->UnregisterNatives(static_cast<jclass>(H(t, arg(t, 1))))); });
    add(217, [](GuestThread& t) { ret(t, E(t)->MonitorEnter(H(t, arg(t, 1)))); });
    add(218, [](GuestThread& t) { ret(t, E(t)->MonitorExit(H(t, arg(t, 1)))); });
    add(219, [](GuestThread& t) {
        g::wr32(arg(t, 1), g_guest_vm);
        ret(t, 0);
    });
    add(226, [](GuestThread& t) { ret(t, G(E(t)->NewWeakGlobalRef(H(t, arg(t, 1))), K_WEAK)); });
    add(227, [](GuestThread& t) {
        u32 h = arg(t, 1);
        if ((h & 3) != K_WEAK) return;
        if (jobject o = H(t, h)) E(t)->DeleteWeakGlobalRef(o);
        drop_global(h);
    });
    add(228, [](GuestThread& t) { ret(t, E(t)->ExceptionCheck()); });
    add(229, [](GuestThread& t) {  // NewDirectByteBuffer(env, addr, jlong capacity)
        ArgCursor c{t, 2};
        jlong cap = static_cast<jlong>(c.dword());
        ret(t, L(t, E(t)->NewDirectByteBuffer(g::h(arg(t, 1)), cap)));
    });
    add(230, [](GuestThread& t) {
        void* p = E(t)->GetDirectBufferAddress(H(t, arg(t, 1)));
        if (p && !g::in_guest(p)) {
            LOGE("guest asked for the address of a Java direct buffer outside its memory");
            return ret(t, 0);
        }
        ret(t, g::to_guest(p));
    });
    add(231, [](GuestThread& t) { ret64(t, static_cast<u64>(E(t)->GetDirectBufferCapacity(H(t, arg(t, 1))))); });
    add(232, [](GuestThread& t) {
        u32 h = arg(t, 1);
        ret(t, h ? (h & 3) : 0);  // JNILocalRefType 1, JNIGlobalRefType 2, JNIWeakGlobalRefType 3
    });
#undef E

    for (int i = 0; i < 233; i++) g::wr32(table + 4 * i, 0);
    for (auto& [i, fn] : f) {
        std::string name = "JNIEnv#" + std::to_string(i);
        g::wr32(table + 4 * i, reg_raw(name.c_str(), fn));
    }
    for (int i = 4; i < 233; i++) {
        if (!g::rd32(table + 4 * i)) {
            std::string name = "JNIEnv#" + std::to_string(i);
            g::wr32(table + 4 * i, thunk_for_missing(name.c_str()));
        }
    }
}

void fill_vm_table(u32 table) {
    auto attach = [](GuestThread& t, bool daemon) {
        JNIEnv* e = nullptr;
        JavaVMAttachArgs a{JNI_VERSION_1_6, nullptr, nullptr};
        u32 ga = arg(t, 2);
        if (ga) {
            a.version = static_cast<jint>(g::rd32(ga));
            a.name = g::rd32(ga + 4) ? g::str(g::rd32(ga + 4)) : nullptr;
        }
        jint r = daemon ? g_vm->AttachCurrentThreadAsDaemon(&e, ga ? &a : nullptr)
                        : g_vm->AttachCurrentThread(&e, ga ? &a : nullptr);
        if (r == JNI_OK) t.env = e;
        if (arg(t, 1)) g::wr32(arg(t, 1), r == JNI_OK ? g_guest_env : 0);
        ret(t, static_cast<u32>(r));
    };
    for (int i = 0; i < 8; i++) g::wr32(table + 4 * i, 0);
    g::wr32(table + 4 * 3, reg_raw("JavaVM#DestroyJavaVM", [](GuestThread& t) { ret(t, static_cast<u32>(JNI_ERR)); }));
    static decltype(attach) s_attach = attach;
    g::wr32(table + 4 * 4, reg_raw("JavaVM#AttachCurrentThread", [](GuestThread& t) { s_attach(t, false); }));
    g::wr32(table + 4 * 5, reg_raw("JavaVM#DetachCurrentThread", [](GuestThread& t) {
                t.locals.clear();
                t.local_free.clear();
                t.frames.clear();
                t.env = nullptr;
                ret(t, static_cast<u32>(g_vm->DetachCurrentThread()));
            }));
    g::wr32(table + 4 * 6, reg_raw("JavaVM#GetEnv", [](GuestThread& t) {
                JNIEnv* e = nullptr;
                jint r = g_vm->GetEnv(reinterpret_cast<void**>(&e), static_cast<jint>(arg(t, 2)));
                if (r == JNI_OK) t.env = e;
                g::wr32(arg(t, 1), r == JNI_OK ? g_guest_env : 0);
                ret(t, static_cast<u32>(r));
            }));
    g::wr32(table + 4 * 7, reg_raw("JavaVM#AttachCurrentThreadAsDaemon", [](GuestThread& t) { s_attach(t, true); }));
}
}  // namespace

void register_jni() {
    u32 env_table = g::data_alloc(233 * 4, 8);
    u32 vm_table = g::data_alloc(8 * 4, 8);
    g_guest_env = g::data_alloc(4, 8);
    g_guest_vm = g::data_alloc(4, 8);
    g::wr32(g_guest_env, env_table);
    g::wr32(g_guest_vm, vm_table);
    fill_env_table(env_table);
    fill_vm_table(vm_table);
}

// Used by main.cpp around guest JNI_OnLoad.
void jni_push_frame() { push_frame(self()); }
void jni_pop_frame() { pop_frame(self()); }
