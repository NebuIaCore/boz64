// Windows stand-in for Android's logging: lines go to boz64.log next to the .exe (and the console when there is one).
#pragma once
#include <cstdarg>

enum {
    ANDROID_LOG_UNKNOWN = 0,
    ANDROID_LOG_DEFAULT,
    ANDROID_LOG_VERBOSE,
    ANDROID_LOG_DEBUG,
    ANDROID_LOG_INFO,
    ANDROID_LOG_WARN,
    ANDROID_LOG_ERROR,
    ANDROID_LOG_FATAL,
    ANDROID_LOG_SILENT,
};

extern "C" int __android_log_write(int prio, const char* tag, const char* text);
extern "C" int __android_log_vprint(int prio, const char* tag, const char* fmt, va_list ap);
extern "C" int __android_log_print(int prio, const char* tag, const char* fmt, ...);
