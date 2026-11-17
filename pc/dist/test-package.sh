#!/bin/sh
# Installs one BOZ64 Linux package in a fresh container (Debian/Ubuntu, Fedora or Arch) and checks it, with no game
# files anywhere:
#   1. it starts, logs, and stops with the message saying where the game files go (exit code 1);
#   2. with debug_boz64_selftest=1 it opens a window, an OpenGL ES context and sound, and draws a frame (exit code 0),
#      on Xvfb with Mesa's software renderer.
# usage: test-package.sh deb|rpm|arch|tgz|appimage|bin <folder holding the downloaded artifacts, or the build folder>
set -eu
kind=$1
art=$(cd "$2" && pwd)
pkg() { find "$art" -name "$1" | head -n 1; }

if command -v apt-get >/dev/null; then
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -q
    apt-get install -y -q --no-install-recommends xvfb libgl1-mesa-dri libegl1 libgles2 ca-certificates gdb
elif command -v dnf >/dev/null; then
    dnf install -y -q xorg-x11-server-Xvfb mesa-dri-drivers mesa-libEGL mesa-libGLES findutils gdb
elif command -v pacman >/dev/null; then
    pacman -Syu --noconfirm --needed xorg-server-xvfb mesa libglvnd gdb
fi

run=boz64
case $kind in
deb) apt-get install -y -q "$(pkg '*.deb')" ;;
rpm) dnf install -y -q "$(pkg '*.rpm')" ;;
arch) pacman -U --noconfirm "$(pkg '*.pkg.tar.zst')" ;;
tgz)
    mkdir -p /opt/boz64 && tar -xzf "$(pkg '*.tar.gz')" -C /opt/boz64
    run=$(find /opt/boz64 -name boz64 -type f | head -n 1)
    ;;
bin) run=$(find "$art" -maxdepth 1 -name boz64 -type f) ;;  # a build folder: its unstripped program, for backtraces
appimage)
    run=$(pkg '*.AppImage')
    chmod +x "$run"
    export APPIMAGE_EXTRACT_AND_RUN=1  # no FUSE in containers
    ;;
*) echo "unknown package kind $kind" >&2; exit 2 ;;
esac
echo "== running $run"

check() {  # check <name> <expected exit code> <text the log must hold> [env...]
    name=$1 want=$2 text=$3
    shift 3
    home=$(mktemp -d)
    set +e
    env BOZ64_HOME="$home" SDL_AUDIODRIVER=dummy "$@" "$run"
    code=$?
    set -e
    echo "== $name: exit code $code; log:"
    cat "$home/boz64.log" || true
    if [ "$code" -gt 128 ] && command -v gdb >/dev/null && [ "$kind" != appimage ]; then
        echo "== $name: killed by signal $((code - 128)); where:"
        env BOZ64_HOME="$home" SDL_AUDIODRIVER=dummy "$@" gdb -q -batch -ex run -ex bt -ex 'info registers rip'             -ex 'x/6i $pc-12' --args "$run" 2>&1 | tail -40
    fi
    [ "$code" -eq "$want" ] && grep -q "$text" "$home/boz64.log" && echo "== $name: OK" && return 0
    echo "== $name: FAILED" >&2
    return 1
}

check "start without game files" 1 "needs the game files" SDL_VIDEODRIVER=dummy

Xvfb :99 -screen 0 1280x720x24 >/dev/null 2>&1 &
sleep 2
check "self-test (window, OpenGL ES, sound)" 0 "self-test: all work" DISPLAY=:99 LIBGL_ALWAYS_SOFTWARE=1 debug_boz64_selftest=1
