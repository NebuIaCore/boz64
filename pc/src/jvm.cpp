// The stand-in Java VM (see jvm.h): JNIEnv / JavaVM function tables over a tiny C++ object model.
#include "jvm.h"

#include <cstdarg>
#include <cstdlib>
#include <cstring>
#include <type_traits>
#include <unordered_set>

#include "common.h"

extern JavaVM* g_vm;  // jni_bridge.cpp
jvalue boz_native_call(const void* fn, JNIEnv* env, jobject thiz, const jvalue* args);

// One 8-byte slot per native the guest registers: a registered function pointer is boz_native_stubs + 8 * index
// (jni_bridge.cpp), so the address says which native it is. Nothing is ever executed here.
extern "C" {
char boz_native_stubs[512 * 8];
}

namespace {

constexpr int PERMANENT = 1 << 30;  // classes and other objects that are never freed

std::mutex class_lock;
std::unordered_map<std::string, JClass*> classes;
JClass* class_class = nullptr;
JClass* string_class = nullptr;
JClass* throwable_class = nullptr;
JClass* buffer_class = nullptr;

struct JBuffer : JObj {
    void* addr = nullptr;
    jlong cap = 0;
};

// ---- local references and pending exceptions (per thread) ----
struct Locals {
    std::vector<JObj*> refs;
    std::vector<size_t> frames;
};
thread_local Locals tl;
thread_local bool tl_exc = false;
thread_local std::string tl_exc_msg;

jobject make_local(JObj* owned) {
    if (!owned) return nullptr;
    tl.refs.push_back(owned);
    return reinterpret_cast<jobject>(owned);
}
jobject new_local(JObj* o) {
    if (!o) return nullptr;
    return make_local(jref(o));
}
void delete_local(JObj* o) {
    if (!o) return;
    for (size_t i = tl.refs.size(); i-- > 0;) {
        if (tl.refs[i] == o) {
            tl.refs[i] = nullptr;
            junref(o);
            while (!tl.refs.empty() && !tl.refs.back() &&
                   (tl.frames.empty() || tl.refs.size() > tl.frames.back()))
                tl.refs.pop_back();
            return;
        }
    }
}

void throw_msg(const std::string& m) {
    tl_exc = true;
    tl_exc_msg = m;
}

void log_once(const std::string& what) {
    static std::mutex m;
    static std::unordered_set<std::string> seen;
    std::lock_guard lk(m);
    if (seen.insert(what).second) LOGW("%s", what.c_str());
}

// ---- strings: Java's UTF-16 <-> modified UTF-8 ----
std::u16string from_mutf8(const char* s) {
    std::u16string out;
    if (!s) return out;
    const auto* p = reinterpret_cast<const unsigned char*>(s);
    while (*p) {
        unsigned c = *p++;
        if (c < 0x80) {
            out += static_cast<char16_t>(c);
        } else if ((c & 0xE0) == 0xC0 && *p) {
            out += static_cast<char16_t>(((c & 0x1F) << 6) | (*p++ & 0x3F));
        } else if ((c & 0xF0) == 0xE0 && p[0] && p[1]) {
            out += static_cast<char16_t>(((c & 0x0F) << 12) | ((p[0] & 0x3F) << 6) | (p[1] & 0x3F));
            p += 2;
        } else if ((c & 0xF8) == 0xF0 && p[0] && p[1] && p[2]) {  // standard UTF-8 for a supplementary character
            unsigned cp = ((c & 0x07) << 18) | ((p[0] & 0x3F) << 12) | ((p[1] & 0x3F) << 6) | (p[2] & 0x3F);
            p += 3;
            cp -= 0x10000;
            out += static_cast<char16_t>(0xD800 + (cp >> 10));
            out += static_cast<char16_t>(0xDC00 + (cp & 0x3FF));
        } else {
            out += u'?';
        }
    }
    return out;
}
std::string to_mutf8(const char16_t* s, size_t n) {
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; i++) {
        unsigned c = s[i];
        if (c != 0 && c < 0x80) {
            out += static_cast<char>(c);
        } else if (c < 0x800) {
            out += static_cast<char>(0xC0 | (c >> 6));
            out += static_cast<char>(0x80 | (c & 0x3F));
        } else {
            out += static_cast<char>(0xE0 | (c >> 12));
            out += static_cast<char>(0x80 | ((c >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (c & 0x3F));
        }
    }
    return out;
}

JString* S(jobject o) { return static_cast<JString*>(O(o)); }
JArray* A(jobject o) { return static_cast<JArray*>(O(o)); }
JClass* C(jobject o) { return static_cast<JClass*>(O(o)); }
JMethod* M(jmethodID m) { return reinterpret_cast<JMethod*>(m); }
JField* F(jfieldID f) { return reinterpret_cast<JField*>(f); }

bool parse_sig(const std::string& sig, std::string& params, char& ret) {
    params.clear();
    size_t i = 0;
    if (sig.empty() || sig[i++] != '(') return false;
    while (i < sig.size() && sig[i] != ')') {
        char c = sig[i];
        if (c == '[') {
            while (i < sig.size() && sig[i] == '[') i++;
            if (i < sig.size() && sig[i] == 'L') i = sig.find(';', i);
            if (i == std::string::npos) return false;
            i++;
            params += 'L';
        } else if (c == 'L') {
            i = sig.find(';', i);
            if (i == std::string::npos) return false;
            i++;
            params += 'L';
        } else {
            params += c;
            i++;
        }
    }
    if (i >= sig.size()) return false;
    i++;
    ret = i < sig.size() ? ((sig[i] == '[' || sig[i] == 'L') ? 'L' : sig[i]) : 'V';
    return true;
}

bool is_subclass(const JClass* c, const JClass* of) {
    for (; c; c = c->super)
        if (c == of) return true;
    return false;
}

JClass* class_of_name(const std::string& name);

// ---- calls ----
jvalue call_native_ptr(JMethod* m, JObj* thiz, const jvalue* a) {
    jlocal_push();
    if (thiz) make_local(jref(thiz));
    for (size_t i = 0; i < m->params.size(); i++)
        if (m->params[i] == 'L' && a[i].l) make_local(jref(O(a[i].l)));
    jvalue r = boz_native_call(m->native, &g_jenv, reinterpret_cast<jobject>(thiz), a);
    if (m->ret == 'L' && r.l) jref(O(r.l));
    jlocal_pop();
    return r;
}

jvalue invoke(JObj* thiz, JMethod* m, const jvalue* a, bool virt) {
    if (!m) {
        LOGE("JNI call with a null method ID");
        return {};
    }
    if (virt && thiz && !m->is_static && thiz->cls != m->cls) {
        if (JMethod* o = jmethod_find(thiz->cls, m->name.c_str(), m->sig.c_str())) m = o;
    }
    if (m->native) return call_native_ptr(m, thiz ? thiz : m->cls, a);
    if (m->impl) return m->impl(thiz, a);
    log_once("Java method " + m->cls->name + "." + m->name + m->sig + " is not implemented");
    return {};
}

std::vector<jvalue> from_va(JMethod* m, va_list ap) {
    std::vector<jvalue> v(m ? m->params.size() : 0);
    for (size_t i = 0; i < v.size(); i++) {
        switch (m->params[i]) {
        case 'Z': v[i].z = static_cast<jboolean>(va_arg(ap, int)); break;
        case 'B': v[i].b = static_cast<jbyte>(va_arg(ap, int)); break;
        case 'C': v[i].c = static_cast<jchar>(va_arg(ap, int)); break;
        case 'S': v[i].s = static_cast<jshort>(va_arg(ap, int)); break;
        case 'I': v[i].i = va_arg(ap, jint); break;
        case 'J': v[i].j = va_arg(ap, jlong); break;
        case 'F': v[i].f = static_cast<jfloat>(va_arg(ap, double)); break;
        case 'D': v[i].d = va_arg(ap, double); break;
        default: v[i].l = va_arg(ap, jobject); break;
        }
    }
    return v;
}

template <class T> T jv_get(const jvalue& v);
template <class T> void jv_put(jvalue& v, T x);
#define PRIM_TYPES(X)                                                                                         \
    X(Boolean, jboolean, z, 'Z') X(Byte, jbyte, b, 'B') X(Char, jchar, c, 'C') X(Short, jshort, s, 'S')      \
    X(Int, jint, i, 'I') X(Long, jlong, j, 'J') X(Float, jfloat, f, 'F') X(Double, jdouble, d, 'D')
#define X(N, T, m, ch)                                                                                        \
    template <> T jv_get<T>(const jvalue& v) { return v.m; }                                                  \
    template <> void jv_put<T>(jvalue & v, T x) { v.m = x; }
PRIM_TYPES(X)
#undef X

template <class R>
R conv_ret(const jvalue& v) {
    if constexpr (std::is_void_v<R>) return;
    else if constexpr (std::is_same_v<R, jobject>) return make_local(O(v.l));  // invoke's result is owned
    else return jv_get<R>(v);
}

template <class R> R CallM(JNIEnv*, jobject o, jmethodID m, ...) {
    va_list ap;
    va_start(ap, m);
    auto a = from_va(M(m), ap);
    va_end(ap);
    return conv_ret<R>(invoke(O(o), M(m), a.data(), true));
}
template <class R> R CallMV(JNIEnv*, jobject o, jmethodID m, va_list ap) {
    auto a = from_va(M(m), ap);
    return conv_ret<R>(invoke(O(o), M(m), a.data(), true));
}
template <class R> R CallMA(JNIEnv*, jobject o, jmethodID m, const jvalue* a) {
    return conv_ret<R>(invoke(O(o), M(m), a, true));
}
template <class R> R CallNM(JNIEnv*, jobject o, jclass, jmethodID m, ...) {
    va_list ap;
    va_start(ap, m);
    auto a = from_va(M(m), ap);
    va_end(ap);
    return conv_ret<R>(invoke(O(o), M(m), a.data(), false));
}
template <class R> R CallNMV(JNIEnv*, jobject o, jclass, jmethodID m, va_list ap) {
    auto a = from_va(M(m), ap);
    return conv_ret<R>(invoke(O(o), M(m), a.data(), false));
}
template <class R> R CallNMA(JNIEnv*, jobject o, jclass, jmethodID m, const jvalue* a) {
    return conv_ret<R>(invoke(O(o), M(m), a, false));
}
template <class R> R CallSM(JNIEnv*, jclass, jmethodID m, ...) {
    va_list ap;
    va_start(ap, m);
    auto a = from_va(M(m), ap);
    va_end(ap);
    return conv_ret<R>(invoke(nullptr, M(m), a.data(), false));
}
template <class R> R CallSMV(JNIEnv*, jclass, jmethodID m, va_list ap) {
    auto a = from_va(M(m), ap);
    return conv_ret<R>(invoke(nullptr, M(m), a.data(), false));
}
template <class R> R CallSMA(JNIEnv*, jclass, jmethodID m, const jvalue* a) {
    return conv_ret<R>(invoke(nullptr, M(m), a, false));
}

jobject new_object(jclass c, JMethod* ctor, const jvalue* a) {
    JObj* o = jnew(C(c));
    if (ctor && ctor->impl) ctor->impl(o, a);
    return make_local(o);
}
jobject NewObj(JNIEnv*, jclass c, jmethodID m, ...) {
    va_list ap;
    va_start(ap, m);
    auto a = from_va(M(m), ap);
    va_end(ap);
    return new_object(c, M(m), a.data());
}
jobject NewObjV(JNIEnv*, jclass c, jmethodID m, va_list ap) {
    auto a = from_va(M(m), ap);
    return new_object(c, M(m), a.data());
}
jobject NewObjA(JNIEnv*, jclass c, jmethodID m, const jvalue* a) { return new_object(c, M(m), a); }

// ---- fields ----
jvalue field_get(JObj* o, JField* f) {
    if (!o || !f) return {};
    std::lock_guard lk(o->monitor);
    auto it = o->fields.find(f);
    return it == o->fields.end() ? jvalue{} : it->second;
}
template <class T> T GetF(JNIEnv*, jobject o, jfieldID f) { return jv_get<T>(field_get(O(o), F(f))); }
template <class T> void SetF(JNIEnv*, jobject o, jfieldID f, T x) {
    jvalue v{};
    jv_put<T>(v, x);
    if (O(o) && F(f)) {
        std::lock_guard lk(O(o)->monitor);
        O(o)->fields[F(f)] = v;
    }
}
jobject GetObjF(JNIEnv*, jobject o, jfieldID f) { return new_local(O(field_get(O(o), F(f)).l)); }
void SetObjF(JNIEnv*, jobject o, jfieldID f, jobject v) {
    if (O(o) && F(f)) jfield_set(O(o), F(f), jv_obj(O(v)));
}
template <class T> T GetSF(JNIEnv*, jclass, jfieldID f) { return F(f) ? jv_get<T>(F(f)->static_value) : T{}; }
template <class T> void SetSF(JNIEnv*, jclass, jfieldID f, T x) {
    if (F(f)) jv_put<T>(F(f)->static_value, x);
}
jobject GetSObjF(JNIEnv*, jclass, jfieldID f) { return F(f) ? new_local(O(F(f)->static_value.l)) : nullptr; }
void SetSObjF(JNIEnv*, jclass, jfieldID f, jobject v) {
    if (!F(f)) return;
    JObj* old = O(F(f)->static_value.l);
    F(f)->static_value.l = reinterpret_cast<jobject>(v ? jref(O(v)) : nullptr);
    if (old) junref(old);
}

// ---- arrays ----
template <class T, class AT, char CH> AT NewArr(JNIEnv*, jsize n) {
    return static_cast<AT>(make_local(jarray_new(CH, n)));
}
template <class T, class AT> T* GetElems(JNIEnv*, AT a, jboolean* is_copy) {
    if (is_copy) *is_copy = JNI_FALSE;
    return a ? reinterpret_cast<T*>(A(a)->data.data()) : nullptr;
}
template <class T, class AT> void RelElems(JNIEnv*, AT, T*, jint) {}
template <class T, class AT> void GetRegion(JNIEnv*, AT a, jsize s, jsize n, T* buf) {
    JArray* ar = A(a);
    if (!ar || s < 0 || n < 0 || s + n > ar->n) {
        throw_msg("ArrayIndexOutOfBoundsException");
        return;
    }
    memcpy(buf, ar->data.data() + s * sizeof(T), n * sizeof(T));
}
template <class T, class AT> void SetRegion(JNIEnv*, AT a, jsize s, jsize n, const T* buf) {
    JArray* ar = A(a);
    if (!ar || s < 0 || n < 0 || s + n > ar->n) {
        throw_msg("ArrayIndexOutOfBoundsException");
        return;
    }
    memcpy(ar->data.data() + s * sizeof(T), buf, n * sizeof(T));
}

// ---- the JNIEnv table ----
JNINativeInterface iface{};
JNIInvokeInterface viface{};

jmethodID get_method(jclass c, const char* name, const char* sig, bool is_static) {
    JClass* cl = C(c);
    if (!cl) {
        throw_msg("NullPointerException in GetMethodID");
        return nullptr;
    }
    if (JMethod* m = jmethod_find(cl, name, sig)) return reinterpret_cast<jmethodID>(m);
    if (!cl->stub_missing) {
        throw_msg(std::string("NoSuchMethodError: ") + cl->name + "." + name + sig);
        return nullptr;
    }
    LOGI("JVM: made up %s method %s.%s%s (not implemented)", is_static ? "static" : "instance", cl->name.c_str(), name, sig);
    std::string what = cl->name + "." + name + sig;
    JMethod* m = jmethod_def(cl, name, sig, [what](JObj*, const jvalue*) {
        log_once("JVM: unimplemented Java method " + what + " called");
        return jvalue{};
    }, is_static);
    return reinterpret_cast<jmethodID>(m);
}

jfieldID get_field(jclass c, const char* name, const char* sig, bool is_static) {
    JClass* cl = C(c);
    if (!cl) {
        throw_msg("NullPointerException in GetFieldID");
        return nullptr;
    }
    for (JClass* k = cl; k; k = k->super) {
        auto it = k->fieldmap.find(std::string(name) + ":" + sig);
        if (it != k->fieldmap.end()) return reinterpret_cast<jfieldID>(it->second);
    }
    if (!cl->stub_missing) {
        throw_msg(std::string("NoSuchFieldError: ") + cl->name + "." + name);
        return nullptr;
    }
    LOGI("JVM: made up field %s.%s %s", cl->name.c_str(), name, sig);
    return reinterpret_cast<jfieldID>(jfield_def(cl, name, sig, is_static));
}

void fill_iface() {
    iface.GetVersion = [](JNIEnv*) -> jint { return JNI_VERSION_1_6; };
    iface.DefineClass = [](JNIEnv*, const char*, jobject, const jbyte*, jsize) -> jclass { return nullptr; };
    iface.FindClass = [](JNIEnv*, const char* name) -> jclass {
        JClass* c = jclass_find(name);
        if (!c) {
            LOGI("JVM: FindClass(%s): no such class", name);
            throw_msg(std::string("NoClassDefFoundError: ") + name);
            return nullptr;
        }
        return static_cast<jclass>(new_local(c));
    };
    iface.FromReflectedMethod = [](JNIEnv*, jobject) -> jmethodID { return nullptr; };
    iface.FromReflectedField = [](JNIEnv*, jobject) -> jfieldID { return nullptr; };
    iface.ToReflectedMethod = [](JNIEnv*, jclass, jmethodID, jboolean) -> jobject { return nullptr; };
    iface.GetSuperclass = [](JNIEnv*, jclass c) -> jclass { return C(c) ? static_cast<jclass>(new_local(C(c)->super)) : nullptr; };
    iface.IsAssignableFrom = [](JNIEnv*, jclass a, jclass b) -> jboolean { return is_subclass(C(a), C(b)); };
    iface.ToReflectedField = [](JNIEnv*, jclass, jfieldID, jboolean) -> jobject { return nullptr; };
    iface.Throw = [](JNIEnv*, jthrowable) -> jint {
        throw_msg("(thrown by native code)");
        return 0;
    };
    iface.ThrowNew = [](JNIEnv*, jclass c, const char* msg) -> jint {
        throw_msg((C(c) ? C(c)->name : std::string("?")) + ": " + (msg ? msg : ""));
        LOGW("JVM: native code threw %s", tl_exc_msg.c_str());
        return 0;
    };
    iface.ExceptionOccurred = [](JNIEnv*) -> jthrowable {
        return tl_exc ? static_cast<jthrowable>(make_local(jnew(throwable_class))) : nullptr;
    };
    iface.ExceptionDescribe = [](JNIEnv*) {
        if (tl_exc) LOGW("JVM: pending exception: %s", tl_exc_msg.c_str());
    };
    iface.ExceptionClear = [](JNIEnv*) { tl_exc = false; };
    iface.FatalError = [](JNIEnv*, const char* msg) { fatal("JNI FatalError: %s", msg ? msg : ""); };
    iface.PushLocalFrame = [](JNIEnv*, jint) -> jint {
        jlocal_push();
        return 0;
    };
    iface.PopLocalFrame = [](JNIEnv*, jobject res) -> jobject {
        JObj* keep = res ? jref(O(res)) : nullptr;
        jlocal_pop();
        return make_local(keep);
    };
    iface.NewGlobalRef = [](JNIEnv*, jobject o) -> jobject { return o ? reinterpret_cast<jobject>(jref(O(o))) : nullptr; };
    iface.DeleteGlobalRef = [](JNIEnv*, jobject o) {
        if (o) junref(O(o));
    };
    iface.DeleteLocalRef = [](JNIEnv*, jobject o) { delete_local(O(o)); };
    iface.IsSameObject = [](JNIEnv*, jobject a, jobject b) -> jboolean { return a == b; };
    iface.NewLocalRef = [](JNIEnv*, jobject o) -> jobject { return new_local(O(o)); };
    iface.EnsureLocalCapacity = [](JNIEnv*, jint) -> jint { return 0; };
    iface.AllocObject = [](JNIEnv*, jclass c) -> jobject { return C(c) ? make_local(jnew(C(c))) : nullptr; };
    iface.NewObject = NewObj;
    iface.NewObjectV = NewObjV;
    iface.NewObjectA = NewObjA;
    iface.GetObjectClass = [](JNIEnv*, jobject o) -> jclass { return o ? static_cast<jclass>(new_local(O(o)->cls)) : nullptr; };
    iface.IsInstanceOf = [](JNIEnv*, jobject o, jclass c) -> jboolean { return !o || is_subclass(O(o)->cls, C(c)); };
    iface.GetMethodID = [](JNIEnv*, jclass c, const char* n, const char* s) { return get_method(c, n, s, false); };
    iface.GetStaticMethodID = [](JNIEnv*, jclass c, const char* n, const char* s) { return get_method(c, n, s, true); };
    iface.GetFieldID = [](JNIEnv*, jclass c, const char* n, const char* s) { return get_field(c, n, s, false); };
    iface.GetStaticFieldID = [](JNIEnv*, jclass c, const char* n, const char* s) { return get_field(c, n, s, true); };

#define CALLS(N, R)                                                                                           \
    iface.Call##N##Method = CallM<R>;                                                                         \
    iface.Call##N##MethodV = CallMV<R>;                                                                       \
    iface.Call##N##MethodA = CallMA<R>;                                                                       \
    iface.CallNonvirtual##N##Method = CallNM<R>;                                                              \
    iface.CallNonvirtual##N##MethodV = CallNMV<R>;                                                            \
    iface.CallNonvirtual##N##MethodA = CallNMA<R>;                                                            \
    iface.CallStatic##N##Method = CallSM<R>;                                                                  \
    iface.CallStatic##N##MethodV = CallSMV<R>;                                                                \
    iface.CallStatic##N##MethodA = CallSMA<R>;
    CALLS(Object, jobject)
    CALLS(Void, void)
#define X(N, T, m, ch) CALLS(N, T)
    PRIM_TYPES(X)
#undef X
#undef CALLS

    iface.GetObjectField = GetObjF;
    iface.SetObjectField = SetObjF;
    iface.GetStaticObjectField = GetSObjF;
    iface.SetStaticObjectField = SetSObjF;
#define X(N, T, m, ch)                                                                                        \
    iface.Get##N##Field = GetF<T>;                                                                            \
    iface.Set##N##Field = SetF<T>;                                                                            \
    iface.GetStatic##N##Field = GetSF<T>;                                                                     \
    iface.SetStatic##N##Field = SetSF<T>;                                                                     \
    iface.New##N##Array = NewArr<T, T##Array, ch>;                                                            \
    iface.Get##N##ArrayElements = GetElems<T, T##Array>;                                                      \
    iface.Release##N##ArrayElements = RelElems<T, T##Array>;                                                  \
    iface.Get##N##ArrayRegion = GetRegion<T, T##Array>;                                                       \
    iface.Set##N##ArrayRegion = SetRegion<T, T##Array>;
    PRIM_TYPES(X)
#undef X

    iface.NewString = [](JNIEnv*, const jchar* c, jsize n) -> jstring {
        auto* s = static_cast<JString*>(jstring_new(""));
        s->s.assign(reinterpret_cast<const char16_t*>(c), n);
        return static_cast<jstring>(make_local(s));
    };
    iface.GetStringLength = [](JNIEnv*, jstring s) -> jsize { return s ? static_cast<jsize>(S(s)->s.size()) : 0; };
    iface.GetStringChars = [](JNIEnv*, jstring s, jboolean* is_copy) -> const jchar* {
        if (is_copy) *is_copy = JNI_FALSE;
        return s ? reinterpret_cast<const jchar*>(S(s)->s.c_str()) : nullptr;
    };
    iface.ReleaseStringChars = [](JNIEnv*, jstring, const jchar*) {};
    iface.NewStringUTF = [](JNIEnv*, const char* c) -> jstring {
        if (!c) return nullptr;
        auto* s = static_cast<JString*>(jstring_new(""));
        s->s = from_mutf8(c);
        return static_cast<jstring>(make_local(s));
    };
    iface.GetStringUTFLength = [](JNIEnv*, jstring s) -> jsize {
        return s ? static_cast<jsize>(to_mutf8(S(s)->s.data(), S(s)->s.size()).size()) : 0;
    };
    iface.GetStringUTFChars = [](JNIEnv*, jstring s, jboolean* is_copy) -> const char* {
        if (is_copy) *is_copy = JNI_TRUE;
        if (!s) return nullptr;
        std::string u = to_mutf8(S(s)->s.data(), S(s)->s.size());
        char* b = static_cast<char*>(malloc(u.size() + 1));
        memcpy(b, u.c_str(), u.size() + 1);
        return b;
    };
    iface.ReleaseStringUTFChars = [](JNIEnv*, jstring, const char* c) { free(const_cast<char*>(c)); };
    iface.GetStringRegion = [](JNIEnv*, jstring s, jsize st, jsize n, jchar* buf) {
        if (!s || st < 0 || n < 0 || static_cast<size_t>(st + n) > S(s)->s.size()) {
            throw_msg("StringIndexOutOfBoundsException");
            return;
        }
        memcpy(buf, S(s)->s.data() + st, n * sizeof(jchar));
    };
    iface.GetStringUTFRegion = [](JNIEnv*, jstring s, jsize st, jsize n, char* buf) {
        if (!s || st < 0 || n < 0 || static_cast<size_t>(st + n) > S(s)->s.size()) {
            throw_msg("StringIndexOutOfBoundsException");
            return;
        }
        std::string u = to_mutf8(S(s)->s.data() + st, n);
        memcpy(buf, u.c_str(), u.size() + 1);
    };
    iface.GetStringCritical = [](JNIEnv*, jstring s, jboolean* is_copy) -> const jchar* {
        if (is_copy) *is_copy = JNI_FALSE;
        return s ? reinterpret_cast<const jchar*>(S(s)->s.c_str()) : nullptr;
    };
    iface.ReleaseStringCritical = [](JNIEnv*, jstring, const jchar*) {};

    iface.GetArrayLength = [](JNIEnv*, jarray a) -> jsize { return a ? A(a)->n : 0; };
    iface.NewObjectArray = [](JNIEnv*, jsize n, jclass c, jobject init) -> jobjectArray {
        JArray* a = jarray_new('L', n);
        if (C(c)) a->cls = class_of_name("[L" + C(c)->name + ";");
        for (auto& e : a->objs) e = init ? jref(O(init)) : nullptr;
        return static_cast<jobjectArray>(make_local(a));
    };
    iface.GetObjectArrayElement = [](JNIEnv*, jobjectArray a, jsize i) -> jobject {
        if (!a || i < 0 || i >= A(a)->n) {
            throw_msg("ArrayIndexOutOfBoundsException");
            return nullptr;
        }
        return new_local(A(a)->objs[i]);
    };
    iface.SetObjectArrayElement = [](JNIEnv*, jobjectArray a, jsize i, jobject v) {
        if (!a || i < 0 || i >= A(a)->n) {
            throw_msg("ArrayIndexOutOfBoundsException");
            return;
        }
        JObj* old = A(a)->objs[i];
        A(a)->objs[i] = v ? jref(O(v)) : nullptr;
        if (old) junref(old);
    };
    iface.GetPrimitiveArrayCritical = [](JNIEnv*, jarray a, jboolean* is_copy) -> void* {
        if (is_copy) *is_copy = JNI_FALSE;
        return a ? A(a)->data.data() : nullptr;
    };
    iface.ReleasePrimitiveArrayCritical = [](JNIEnv*, jarray, void*, jint) {};

    iface.RegisterNatives = [](JNIEnv*, jclass c, const JNINativeMethod* m, jint n) -> jint {
        JClass* cl = C(c);
        if (!cl) return JNI_ERR;
        for (jint i = 0; i < n; i++) {
            JMethod* jm = nullptr;
            {
                auto it = cl->methods.find(std::string(m[i].name) + m[i].signature);
                if (it != cl->methods.end()) jm = it->second;
            }
            if (!jm) jm = jmethod_def(cl, m[i].name, m[i].signature, nullptr);
            jm->native = m[i].fnPtr;
        }
        return JNI_OK;
    };
    iface.UnregisterNatives = [](JNIEnv*, jclass c) -> jint {
        if (C(c))
            for (auto& [k, m] : C(c)->methods) m->native = nullptr;
        return JNI_OK;
    };
    iface.MonitorEnter = [](JNIEnv*, jobject o) -> jint {
        if (o) O(o)->monitor.lock();
        return 0;
    };
    iface.MonitorExit = [](JNIEnv*, jobject o) -> jint {
        if (o) O(o)->monitor.unlock();
        return 0;
    };
    iface.GetJavaVM = [](JNIEnv*, JavaVM** vm) -> jint {
        *vm = &g_jvm;
        return 0;
    };
    iface.NewWeakGlobalRef = [](JNIEnv*, jobject o) -> jweak { return o ? reinterpret_cast<jweak>(jref(O(o))) : nullptr; };
    iface.DeleteWeakGlobalRef = [](JNIEnv*, jweak o) {
        if (o) junref(O(o));
    };
    iface.ExceptionCheck = [](JNIEnv*) -> jboolean { return tl_exc; };
    iface.NewDirectByteBuffer = [](JNIEnv*, void* addr, jlong cap) -> jobject {
        auto* b = new JBuffer;
        b->cls = buffer_class;
        b->addr = addr;
        b->cap = cap;
        return make_local(b);
    };
    iface.GetDirectBufferAddress = [](JNIEnv*, jobject o) -> void* {
        return o && O(o)->cls == buffer_class ? static_cast<JBuffer*>(O(o))->addr : nullptr;
    };
    iface.GetDirectBufferCapacity = [](JNIEnv*, jobject o) -> jlong {
        return o && O(o)->cls == buffer_class ? static_cast<JBuffer*>(O(o))->cap : -1;
    };
    iface.GetObjectRefType = [](JNIEnv*, jobject o) -> jobjectRefType { return o ? JNILocalRefType : JNIInvalidRefType; };

    viface.DestroyJavaVM = [](JavaVM*) -> jint { return JNI_ERR; };
    viface.AttachCurrentThread = [](JavaVM*, JNIEnv** e, void*) -> jint {
        *e = &g_jenv;
        return JNI_OK;
    };
    viface.AttachCurrentThreadAsDaemon = [](JavaVM*, JNIEnv** e, void*) -> jint {
        *e = &g_jenv;
        return JNI_OK;
    };
    viface.DetachCurrentThread = [](JavaVM*) -> jint {
        for (JObj* o : tl.refs)
            if (o) junref(o);
        tl.refs.clear();
        tl.frames.clear();
        return JNI_OK;
    };
    viface.GetEnv = [](JavaVM*, void** e, jint) -> jint {
        *e = &g_jenv;
        return JNI_OK;
    };
}

JClass* class_of_name(const std::string& name) {
    std::lock_guard lk(class_lock);
    auto it = classes.find(name);
    if (it != classes.end()) return it->second;
    if (name.empty() || name[0] != '[') return nullptr;
    auto* c = new JClass;  // array classes are made on demand
    c->cls = class_class;
    c->refs = PERMANENT;
    c->name = name;
    auto o = classes.find("java/lang/Object");
    c->super = o != classes.end() ? o->second : nullptr;
    classes[name] = c;
    return c;
}

}  // namespace

JNIEnv g_jenv{&iface};
JavaVM g_jvm{&viface};

JArray::~JArray() {
    for (JObj* o : objs)
        if (o) junref(o);
}

JObj* jref(JObj* o) {
    if (o) o->refs.fetch_add(1, std::memory_order_relaxed);
    return o;
}
void junref(JObj* o) {
    if (!o) return;
    if (o->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) delete o;
}

JClass* jclass_def(const char* name, const char* super, bool stub_missing) {
    JClass* sup = nullptr;
    if (super && strcmp(name, "java/lang/Object") != 0) sup = jclass_def(super, "java/lang/Object", false);
    std::lock_guard lk(class_lock);
    auto it = classes.find(name);
    if (it != classes.end()) {
        if (sup && !it->second->super) it->second->super = sup;
        it->second->stub_missing |= stub_missing;
        return it->second;
    }
    auto* c = new JClass;
    c->cls = class_class;
    c->refs = PERMANENT;
    c->name = name;
    c->super = sup;
    c->stub_missing = stub_missing;
    classes[name] = c;
    return c;
}

JClass* jclass_find(const char* name) {
    std::string n = name;
    for (char& ch : n)
        if (ch == '.') ch = '/';
    return class_of_name(n);
}

JMethod* jmethod_def(JClass* c, const char* name, const char* sig, JImpl impl, bool is_static) {
    std::lock_guard lk(class_lock);
    std::string key = std::string(name) + sig;
    JMethod*& m = c->methods[key];
    if (!m) m = new JMethod{c, name, sig};
    if (!parse_sig(sig, m->params, m->ret)) LOGW("JVM: bad method signature %s", sig);
    m->is_static = is_static;
    if (impl) m->impl = std::move(impl);
    return m;
}

JMethod* jmethod_find(JClass* c, const char* name, const char* sig) {
    std::lock_guard lk(class_lock);
    std::string key = std::string(name) + sig;
    for (; c; c = c->super) {
        auto it = c->methods.find(key);
        if (it != c->methods.end()) return it->second;
    }
    return nullptr;
}

JField* jfield_def(JClass* c, const char* name, const char* sig, bool is_static) {
    std::lock_guard lk(class_lock);
    JField*& f = c->fieldmap[std::string(name) + ":" + sig];
    if (!f) f = new JField{c, name, sig, is_static};
    return f;
}

void jfield_set(JObj* o, JField* f, jvalue v) {
    if (!o || !f) return;
    if (f->sig[0] == 'L' || f->sig[0] == '[') {
        if (v.l) jref(O(v.l));
        JObj* old;
        {
            std::lock_guard lk(o->monitor);
            old = O(o->fields[f].l);
            o->fields[f] = v;
        }
        if (old) junref(old);
    } else {
        std::lock_guard lk(o->monitor);
        o->fields[f] = v;
    }
}

JObj* jnew(JClass* c) {
    auto* o = new JObj;
    o->cls = c;
    return o;
}

JString* jstring_new(const std::string& utf8) {
    auto* s = new JString;
    s->cls = string_class;
    s->s = from_mutf8(utf8.c_str());
    return s;
}
JString* jstring_new_or_null(const char* utf8) { return utf8 ? jstring_new(utf8) : nullptr; }

std::string jstring_utf8(jobject s) {
    if (!s) return {};
    auto* js = static_cast<JString*>(O(s));
    return to_mutf8(js->s.data(), js->s.size());
}

JArray* jarray_new(char type, jsize n) {
    auto* a = new JArray;
    a->type = type;
    a->n = n < 0 ? 0 : n;
    if (type == 'L') {
        a->objs.assign(a->n, nullptr);
        a->cls = class_of_name("[Ljava/lang/Object;");
    } else {
        int sz = type == 'Z' || type == 'B' ? 1 : type == 'C' || type == 'S' ? 2 : type == 'J' || type == 'D' ? 8 : 4;
        a->data.assign(static_cast<size_t>(a->n) * sz, 0);
        a->cls = class_of_name(std::string("[") + type);
    }
    return a;
}

bool jnative_registered(JClass* c, const char* name, const char* sig) {
    JMethod* m = c ? jmethod_find(c, name, sig) : nullptr;
    return m && m->native;
}

jvalue jnative_call(JClass* c, const char* name, const char* sig, JObj* thiz, std::initializer_list<jvalue> args) {
    JMethod* m = c ? jmethod_find(c, name, sig) : nullptr;
    if ((!m || !m->native) && c) {  // registered with another signature?
        std::lock_guard lk(class_lock);
        for (JClass* k = c; k && (!m || !m->native); k = k->super)
            for (auto& [key, cand] : k->methods)
                if (cand->native && cand->name == name) {
                    m = cand;
                    break;
                }
        if (m && m->native) LOGW("JVM: calling native %s.%s as %s (asked for %s)", c->name.c_str(), name, m->sig.c_str(), sig);
    }
    if (!m || !m->native) {
        log_once(std::string("JVM: the engine registered no native ") + (c ? c->name : "?") + "." + name + sig);
        return {};
    }
    std::vector<jvalue> a(args);
    a.resize(std::max(a.size(), m->params.size()));
    return call_native_ptr(m, thiz ? thiz : c, a.data());
}

void jlocal_push() { tl.frames.push_back(tl.refs.size()); }
void jlocal_pop() {
    if (tl.frames.empty()) return;
    size_t n = tl.frames.back();
    tl.frames.pop_back();
    for (size_t i = n; i < tl.refs.size(); i++)
        if (tl.refs[i]) junref(tl.refs[i]);
    tl.refs.resize(std::min(n, tl.refs.size()));
}

void jvm_init() {
    fill_iface();
    class_class = jclass_def("java/lang/Class", "java/lang/Object", false);
    JClass* object = jclass_def("java/lang/Object", nullptr, false);
    class_class->cls = class_class;
    object->cls = class_class;
    string_class = jclass_def("java/lang/String", "java/lang/Object", false);
    throwable_class = jclass_def("java/lang/Throwable", "java/lang/Object", false);
    buffer_class = jclass_def("java/nio/DirectByteBuffer", "java/lang/Object", false);
    for (const char* n : {"[Z", "[B", "[C", "[S", "[I", "[J", "[F", "[D", "[Ljava/lang/Object;", "[Ljava/lang/String;"})
        class_of_name(n);
    g_vm = &g_jvm;
}
