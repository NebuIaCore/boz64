#!/usr/bin/env bash
# Builds both versions after a change, so they stay in step:
#   Android: the 64-bit wrapper (tools/build.sh), packaged into out/boz64.apk (tools/package.py)
#   Windows: boz64.exe (tools/build_pc.bat), copied into the game folder cod bo mobile\BOZ64 PC
#   both: the game data pack with Mule Kick and the extra voice lines (tools/mulekick.py, out/data): put in the APK
#         and linked into the Windows game folder
# Nothing is installed: tools/run.sh installs the APK on the phone.
# usage: tools/build_all.sh
set -e
cd "$(dirname "$0")/.."
echo "== Android: library"
bash tools/build.sh
echo "== Android: APK"
python -I tools/package.py
echo "== Windows"
cmd //c 'tools\build_pc.bat boz64'
python -I tools/mulekick.py --link-pc
echo "== done"
ls -la out/boz64.apk "../../BOZ64 PC/boz64.exe"
