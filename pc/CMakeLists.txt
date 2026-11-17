# Native Windows build of the port: the same 32-bit ARM engine and game, run by dynarmic's x86-64 backend, with
# Windows stand-ins for Android (pc/src) and the port's shared code (../src). Built by tools/build_pc.bat.
# Other systems (Linux, the BSDs, macOS, iOS) build from unix.cmake instead.
cmake_minimum_required(VERSION 3.20)
project(boz64pc C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if (NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()
if (NOT WIN32)
    include(${CMAKE_CURRENT_SOURCE_DIR}/unix.cmake)
    return()
endif()
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")  # static CRT: one .exe plus the DLLs we ship next to it

set(PORT ${CMAKE_CURRENT_SOURCE_DIR}/..)

# dynarmic: ARM32 -> x86-64 translator
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
set(Boost_INCLUDE_DIR "${PORT}/third_party/boost" CACHE PATH "" FORCE)
# xbyak (header-only, third_party/xbyak at the commit dynarmic pins). zydis is left out: dynarmic only uses it to
# disassemble its own output for debugging (DYNARMIC_NO_ZYDIS stubs that out).
add_library(xbyak::xbyak INTERFACE IMPORTED GLOBAL)
set_target_properties(xbyak::xbyak PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${PORT}/third_party/xbyak)
add_library(Zydis::Zydis INTERFACE IMPORTED GLOBAL)
set_target_properties(Zydis::Zydis PROPERTIES INTERFACE_COMPILE_DEFINITIONS DYNARMIC_NO_ZYDIS)
add_subdirectory(${PORT}/third_party/dynarmic ${CMAKE_BINARY_DIR}/dynarmic EXCLUDE_FROM_ALL)

add_executable(dyntest src/dyntest.cpp)
target_link_libraries(dyntest PRIVATE dynarmic)

# ---- the game runner ----
set(SHARED ${PORT}/src)
set(PC ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(BOZ_DEFS BOZ_PC _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS _WINSOCK_DEPRECATED_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN
    _USE_MATH_DEFINES BOZ_GL_NOIMPORT)
set(BOZ_OPTS /FIboz_win.h /Z7 -Wno-unused-function -Wno-unused-variable -Wno-deprecated-declarations -Wno-microsoft-cast
    -Wno-unused-command-line-argument -Wno-ignored-attributes -Wno-missing-field-initializers)

# The port's shared code (also built for Android), compiled against the Windows stand-in headers in pc/include.
add_library(boz_shared OBJECT
    ${SHARED}/cpu.cpp ${SHARED}/memory.cpp ${SHARED}/elf.cpp ${SHARED}/jni_bridge.cpp ${SHARED}/gl.cpp
    ${SHARED}/frame.cpp ${SHARED}/stdio.cpp ${SHARED}/patches.cpp ${SHARED}/smooth.cpp ${SHARED}/settings.cpp
    ${SHARED}/resscale.cpp ${SHARED}/menu.cpp ${SHARED}/codpoints.cpp ${SHARED}/lan.cpp ${SHARED}/pad.cpp ${SHARED}/mulekick.cpp ${SHARED}/vox.cpp ${SHARED}/voxev.cpp ${SHARED}/dogs.cpp ${SHARED}/doubletap.cpp ${SHARED}/dive.cpp ${SHARED}/flame.cpp
)
target_include_directories(boz_shared PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/include/win ${SHARED})
target_compile_definitions(boz_shared PRIVATE ${BOZ_DEFS})
target_compile_options(boz_shared PRIVATE ${BOZ_OPTS})
target_link_libraries(boz_shared PRIVATE dynarmic)

# Windows stand-ins for Android: libc/libdl/zlib for the guest, the Java layer, window, graphics, sound and input.
# The shared folder comes first here so "elf.h" means the loader's header, not pc/include's system one.
add_library(boz_pc OBJECT
    ${PC}/posix_win.cpp ${PC}/vfs_win.cpp ${PC}/libc_win.cpp ${PC}/jvm.cpp ${PC}/marmalade_win.cpp
    ${PC}/window_win.cpp ${PC}/egl_win.cpp ${PC}/audio_win.cpp ${PC}/stubs_win.cpp ${PC}/main_win.cpp
)
target_include_directories(boz_pc PRIVATE ${SHARED} ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/include/win)
target_compile_definitions(boz_pc PRIVATE ${BOZ_DEFS})
target_compile_options(boz_pc PRIVATE ${BOZ_OPTS})
target_link_libraries(boz_pc PRIVATE dynarmic)

# Import libraries for ANGLE (libEGL.dll and libGLESv2.dll ship next to the .exe), from their export lists.
find_program(BOZ_LIB_EXE lib REQUIRED)
set(ANGLE_LIBS)
foreach(dll libEGL libGLESv2)
    add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/${dll}.lib
        COMMAND ${BOZ_LIB_EXE} /nologo /machine:x64 /def:${CMAKE_CURRENT_SOURCE_DIR}/angle/${dll}.def
                /out:${CMAKE_BINARY_DIR}/${dll}.lib
        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/angle/${dll}.def)
    list(APPEND ANGLE_LIBS ${CMAKE_BINARY_DIR}/${dll}.lib)
endforeach()
add_custom_target(angle_implibs DEPENDS ${ANGLE_LIBS})

# boz64.rc: icon and version information; boz64.manifest: merged into the linker's manifest. (An .exe with neither
# looks suspicious to Windows Defender's machine-learning checks.)
add_executable(boz64 $<TARGET_OBJECTS:boz_shared> $<TARGET_OBJECTS:boz_pc>
    ${CMAKE_CURRENT_SOURCE_DIR}/boz64.rc ${CMAKE_CURRENT_SOURCE_DIR}/boz64.manifest)
add_dependencies(boz64 angle_implibs)
# a windowed program: no console window opens next to the game (the log is boz64.log); main() stays the entry point
set_target_properties(boz64 PROPERTIES WIN32_EXECUTABLE TRUE)
target_link_options(boz64 PRIVATE /ENTRY:mainCRTStartup)
target_link_libraries(boz64 PRIVATE ${ANGLE_LIBS} dynarmic ws2_32 iphlpapi winmm mfplat mfreadwrite mfuuid shlwapi ole32 user32 gdi32 shell32)
target_link_options(boz64 PRIVATE /STACK:16777216 /DEBUG)
