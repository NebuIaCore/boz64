# The Unix PC builds of the port (Linux and the BSDs now; macOS and iOS to come): the same 32-bit ARM engine and
# game, run by dynarmic's x86-64 or ARM64 backend, with
#   - SDL3 stand-ins for Android's window, input, sound and GL context (pc/src/*_sdl.cpp, pc/src/host_unix.cpp),
#   - the Android build's POSIX C-library bridge (src/libc.cpp), with guest paths kept under the data folder,
#   - the stand-in JVM and Java classes the Windows build uses (pc/src/jvm.cpp, marmalade_win.cpp: portable),
#   - the host's OpenGL ES through EGL (Mesa).
# Included by pc/CMakeLists.txt when not building for Windows.
include(GNUInstallDirs)

set(PORT ${CMAKE_CURRENT_SOURCE_DIR}/..)
set(SHARED ${PORT}/src)
set(PC ${CMAKE_CURRENT_SOURCE_DIR}/src)

# ---- dynarmic: ARM32 -> x86-64 or ARM64 ----
set(DYNARMIC_FRONTENDS "A32" CACHE STRING "" FORCE)
set(DYNARMIC_TESTS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_WARNINGS_AS_ERRORS OFF CACHE BOOL "" FORCE)
set(DYNARMIC_USE_BUNDLED_EXTERNALS ON CACHE BOOL "" FORCE)
set(DYNARMIC_USE_PRECOMPILED_HEADERS OFF CACHE BOOL "" FORCE)
find_package(Boost 1.57 QUIET)  # headers only; the copy in third_party when the system has none
if (NOT Boost_FOUND)
    set(Boost_INCLUDE_DIR "${PORT}/third_party/boost" CACHE PATH "" FORCE)
endif()
if (CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64)$")
    # xbyak (header-only, at the commit dynarmic pins); zydis only disassembles dynarmic's output for debugging
    add_library(xbyak::xbyak INTERFACE IMPORTED GLOBAL)
    set_target_properties(xbyak::xbyak PROPERTIES INTERFACE_INCLUDE_DIRECTORIES ${PORT}/third_party/xbyak)
    add_library(Zydis::Zydis INTERFACE IMPORTED GLOBAL)
    set_target_properties(Zydis::Zydis PROPERTIES INTERFACE_COMPILE_DEFINITIONS DYNARMIC_NO_ZYDIS)
endif()
add_subdirectory(${PORT}/third_party/dynarmic ${CMAKE_BINARY_DIR}/dynarmic EXCLUDE_FROM_ALL)

# ---- SDL3: the system's when it has one (distribution and Flatpak builds), else downloaded and built in ----
option(BOZ_FETCH_SDL "Download and build SDL3 when the system has none" ON)
find_package(SDL3 3.2 CONFIG QUIET)
if (SDL3_FOUND)
    set(BOZ_SDL SDL3::SDL3)
elseif (BOZ_FETCH_SDL)
    include(FetchContent)
    set(SDL_SHARED OFF CACHE BOOL "" FORCE)
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    set(SDL_TEST_LIBRARY OFF CACHE BOOL "" FORCE)
    set(SDL_TESTS OFF CACHE BOOL "" FORCE)
    set(SDL_EXAMPLES OFF CACHE BOOL "" FORCE)
    FetchContent_Declare(SDL3
        URL https://github.com/libsdl-org/SDL/releases/download/release-3.4.16/SDL3-3.4.16.tar.gz
        URL_HASH SHA256=7322236cd12090c3eb40b9728be4d49c76f66ad17d04369584d4ecad5cf77c68)
    FetchContent_MakeAvailable(SDL3)
    set(BOZ_SDL SDL3::SDL3-static)
else()
    message(FATAL_ERROR "SDL3 not found (install its development files, or turn BOZ_FETCH_SDL on)")
endif()

find_package(ZLIB REQUIRED)
find_package(Threads REQUIRED)
find_library(BOZ_EGL EGL REQUIRED)
find_library(BOZ_GLES GLESv2 REQUIRED)

# ---- the game runner ----
add_executable(boz64
    ${SHARED}/cpu.cpp ${SHARED}/memory.cpp ${SHARED}/elf.cpp ${SHARED}/jni_bridge.cpp ${SHARED}/gl.cpp
    ${SHARED}/frame.cpp ${SHARED}/stdio.cpp ${SHARED}/libc.cpp ${SHARED}/patches.cpp ${SHARED}/smooth.cpp
    ${SHARED}/settings.cpp ${SHARED}/resscale.cpp ${SHARED}/menu.cpp ${SHARED}/codpoints.cpp ${SHARED}/lan.cpp
    ${SHARED}/pad.cpp ${SHARED}/mulekick.cpp ${SHARED}/vox.cpp ${SHARED}/voxev.cpp ${SHARED}/dogs.cpp
    ${SHARED}/doubletap.cpp ${SHARED}/dive.cpp ${SHARED}/flame.cpp
    ${PC}/jvm.cpp ${PC}/marmalade_win.cpp ${PC}/vfs_win.cpp ${PC}/stubs_win.cpp
    ${PC}/host_unix.cpp ${PC}/window_sdl.cpp ${PC}/egl_sdl.cpp ${PC}/audio_sdl.cpp ${PC}/main_sdl.cpp
)
# The shared folder is for quoted includes only (-iquote): its elf.h is the loader's, not the system's <elf.h>.
target_include_directories(boz64 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include ${CMAKE_CURRENT_SOURCE_DIR}/include/unix
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/dr_libs)
target_compile_options(boz64 PRIVATE -iquote ${SHARED} -include boz_unix.h -U_FORTIFY_SOURCE -fexceptions
    -Wno-unused-function -Wno-unused-variable -Wno-deprecated-declarations)
# BOZ_PC: the shared code's PC behaviour (stand-in JVM, keyboard and mouse first, the data folder).
# USE_OZONE: plain integer EGL native types (SDL makes the EGL surfaces; nothing here needs X11's).
target_compile_definitions(boz64 PRIVATE BOZ_PC _GNU_SOURCE _USE_MATH_DEFINES $<$<NOT:$<BOOL:${APPLE}>>:USE_OZONE>)
target_link_libraries(boz64 PRIVATE dynarmic ${BOZ_SDL} ZLIB::ZLIB ${BOZ_EGL} ${BOZ_GLES} Threads::Threads ${CMAKE_DL_LIBS})

install(TARGETS boz64 RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ---- packages (.deb and .rpm through cpack; the CI workflow and pc/dist make the others) ----
# None of them carry the game files: the first start says where to copy them.
set(BOZ64_VERSION 2026.11.17)
if (NOT APPLE)
    set(DIST ${CMAKE_CURRENT_SOURCE_DIR}/dist/linux)
    set(APP_ID io.github.nebuiacore.BOZ64)
    install(FILES ${DIST}/${APP_ID}.desktop DESTINATION ${CMAKE_INSTALL_DATADIR}/applications)
    install(FILES ${DIST}/${APP_ID}.metainfo.xml DESTINATION ${CMAKE_INSTALL_DATADIR}/metainfo)
    install(FILES ${DIST}/boz64.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/192x192/apps RENAME ${APP_ID}.png)
    # AppStream (software centres, the Flatpak build) only looks at the standard sizes; 128 is the one it wants
    install(FILES ${DIST}/boz64-128.png DESTINATION ${CMAKE_INSTALL_DATADIR}/icons/hicolor/128x128/apps RENAME ${APP_ID}.png)
endif()
set(CPACK_PACKAGE_NAME boz64)
set(CPACK_PACKAGE_VERSION ${BOZ64_VERSION})
set(CPACK_PACKAGE_CONTACT "NebuIaCore")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY "Call of Duty: Black Ops Zombies on Linux (needs your own game files)")
set(CPACK_PACKAGING_INSTALL_PREFIX /usr)
set(CPACK_DEBIAN_FILE_NAME DEB-DEFAULT)
set(CPACK_DEBIAN_PACKAGE_SHLIBDEPS ON)
set(CPACK_DEBIAN_PACKAGE_SECTION games)
set(CPACK_RPM_FILE_NAME RPM-DEFAULT)
set(CPACK_RPM_PACKAGE_LICENSE "Proprietary")
set(CPACK_RPM_PACKAGE_GROUP "Amusements/Games")
set(CPACK_RPM_EXCLUDE_FROM_AUTO_FILELIST_ADDITION /usr/share/applications /usr/share/metainfo /usr/share/icons
    /usr/share/icons/hicolor /usr/share/icons/hicolor/192x192 /usr/share/icons/hicolor/192x192/apps
    /usr/share/icons/hicolor/128x128 /usr/share/icons/hicolor/128x128/apps)
include(CPack)
