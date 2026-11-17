// A small stand-in for the Java VM, for the PC build: just enough of JNI (jni.h's JNIEnv / JavaVM tables) for
// the port's JNI bridge (src/jni_bridge.cpp), which passes the 32-bit engine's JNI calls on to it, and Java
// classes written in C++ (marmalade_win.cpp) in place of the original app's Java code.
//
// Objects are reference counted: a local reference (per thread, released when its frame is popped) or a global
// reference holds one count. Functions here that return a JObj* hand over one count ("owned").
#pragma once
#include <jni.h>

#include <atomic>
#include <functional>
#include <initializer_list>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct JClass;

struct JObj {
    JClass* cls = nullptr;
    std::atomic<int> refs{1};
    std::recursive_mutex monitor;
    std::unordered_map<const void*, jvalue> fields;  // instance fields, by JField
    void* host = nullptr;                             // C++ state for host-implemented classes
    virtual ~JObj() = default;
};

struct JString : JObj {
    std::u16string s;
};

struct JArray : JObj {
    char type = 0;  // ZBCSIJFD for primitive arrays, L for object arrays
    jsize n = 0;
    std::vector<unsigned char> data;  // primitive elements
    std::vector<JObj*> objs;          // object elements (each holds a count)
    ~JArray() override;
};

// A method's C++ body: receives the object (null for static methods) and the arguments; an object result is owned.
using JImpl = std::function<jvalue(JObj* thiz, const jvalue* args)>;

struct JMethod {
    JClass* cls;
    std::string name, sig, params;  // params: one char per parameter (ZBCSIJFDL)
    char ret = 'V';
    bool is_static = false;
    JImpl impl;
    void* native = nullptr;  // set when the guest registers a native for it
};

struct JField {
    JClass* cls;
    std::string name, sig;
    bool is_static = false;
    jvalue static_value{};
};

struct JClass : JObj {
    std::string name;
    JClass* super = nullptr;
    bool stub_missing = false;  // unknown methods and fields are made up (and logged) instead of failing
    std::unordered_map<std::string, JMethod*> methods;  // name + sig
    std::unordered_map<std::string, JField*> fieldmap;
};

extern JNIEnv g_jenv;
extern JavaVM g_jvm;

void jvm_init();  // also sets the bridge's g_vm

// classes, methods and fields
JClass* jclass_def(const char* name, const char* super = "java/lang/Object", bool stub_missing = true);
JClass* jclass_find(const char* name);  // null if unknown
JMethod* jmethod_def(JClass* c, const char* name, const char* sig, JImpl impl, bool is_static = false);
JMethod* jmethod_find(JClass* c, const char* name, const char* sig);
JField* jfield_def(JClass* c, const char* name, const char* sig, bool is_static = false);
void jfield_set(JObj* o, JField* f, jvalue v);  // an object value gains a count

// objects
JObj* jref(JObj* o);  // +1 (returns o)
void junref(JObj* o);
JObj* jnew(JClass* c);                       // owned
JString* jstring_new(const std::string& utf8);  // owned
JString* jstring_new_or_null(const char* utf8);
std::string jstring_utf8(jobject s);
JArray* jarray_new(char type, jsize n);  // owned, zero-filled

// jvalue helpers
inline jvalue jv_int(jint v) { jvalue r{}; r.i = v; return r; }
inline jvalue jv_bool(bool v) { jvalue r{}; r.z = v ? 1 : 0; return r; }
inline jvalue jv_long(jlong v) { jvalue r{}; r.j = v; return r; }
inline jvalue jv_float(jfloat v) { jvalue r{}; r.f = v; return r; }
inline jvalue jv_obj(JObj* owned) { jvalue r{}; r.l = reinterpret_cast<jobject>(owned); return r; }
inline jvalue jv_void() { return jvalue{}; }
inline JObj* O(jobject o) { return reinterpret_cast<JObj*>(o); }

// Calls a native method the guest registered for class c (inside a local frame on this thread). Returns a zero
// jvalue, and logs, if the guest registered none. An object result is owned.
jvalue jnative_call(JClass* c, const char* name, const char* sig, JObj* thiz, std::initializer_list<jvalue> args);
bool jnative_registered(JClass* c, const char* name, const char* sig);

// Local frames for host threads that call into the guest outside a native call.
void jlocal_push();
void jlocal_pop();
