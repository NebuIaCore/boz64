#!/usr/bin/env bash
# Installs out/boz64.apk on the phone, starts it, and saves the game's log lines to out/last-run.log.
# usage: tools/run.sh [seconds to watch, default 15] [--no-install] [--trace] [--precise] [--stats]
cd "$(dirname "$0")/.."
ADB="${ADB:-/c/Users/nebula/AppData/Local/Microsoft/WinGet/Packages/Google.PlatformTools_Microsoft.Winget.Source_8wekyb3d8bbwe/platform-tools/adb.exe}"
SERIAL="${SERIAL:-R5CT912KMVP}"
PKG=com.activision.boz64
SECS=15 INSTALL=1 TRACE=0 PRECISE=0 STATS=0
for a in "$@"; do
    case "$a" in
    --no-install) INSTALL=0 ;;
    --trace) TRACE=1 ;;
    --precise) PRECISE=1 ;;
    --stats) STATS=1 ;;
    *) SECS="$a" ;;
    esac
done
adb() { "$ADB" -s "$SERIAL" "$@"; }

if [ $INSTALL = 1 ]; then
    adb install -r out/boz64.apk | tail -1
fi
adb shell setprop debug.boz64.trace $TRACE
adb shell setprop debug.boz64.precise $PRECISE
adb shell setprop debug.boz64.stats $STATS
adb shell am force-stop $PKG
adb logcat -c
adb shell monkey -p $PKG -c android.intent.category.LAUNCHER 1 > /dev/null 2>&1
timeout "$SECS" "$ADB" -s "$SERIAL" logcat -v brief > out/full-run.log 2>/dev/null
PID=$(grep -oE 'Start proc ([0-9]+):'$PKG out/full-run.log | head -1 | grep -oE '[0-9]+')
# Keep the game's own lines plus crash reports; hide e-mail addresses that system services log.
grep -E "\( *$PID\)|F/DEBUG|has died" out/full-run.log |
    grep -vE 'Access denied finding property|InsetsController|BLASTBuffer|SurfaceView|VRI\[|SV\[' |
    sed -E 's/[A-Za-z0-9._%+-]+@[A-Za-z0-9.-]+\.[a-z]{2,}/<email>/g' > out/last-run.log
echo "pid $PID, $(wc -l < out/last-run.log) lines in out/last-run.log"
adb shell pidof $PKG > /dev/null && echo "game is still running" || echo "game is not running"
