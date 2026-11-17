cmake_minimum_required(VERSION 3.20)
project(boz64 C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

# dynarmic: ARM32 -> ARM64 translator
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(Boost_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/boost" CACHE PATH "" FORCE)
add_subdirectory(third_party/dynarmic EXCLUDE_FROM_ALL)

add_library(s3e_android SHARED
    src/main.cpp
    src/memory.cpp
    src/cpu.cpp
    src/elf.cpp
    src/libc.cpp
    src/stdio.cpp
    src/jni_bridge.cpp
    src/gl.cpp
    src/vfs.cpp
    src/frame.cpp src/patches.cpp src/smooth.cpp src/settings.cpp src/prof.cpp src/resscale.cpp src/menu.cpp src/codpoints.cpp src/pad.cpp src/lan.cpp src/mulekick.cpp src/vox.cpp src/voxev.cpp src/dogs.cpp src/doubletap.cpp src/dive.cpp src/flame.cpp
    src/native_stubs.S
)
target_compile_options(s3e_android PRIVATE -U_FORTIFY_SOURCE -fexceptions -Wall -Wno-unused-function -Wno-unused-variable)
target_link_libraries(s3e_android PRIVATE dynarmic log dl z EGL GLESv2)
target_link_options(s3e_android PRIVATE -Wl,--no-undefined -Wl,-z,max-page-size=16384)
