#!/bin/bash
# Builds the 4 Linux variants of the Artist Tools editor plugin (float / double,
# release / "debug") inside WSL or on a Linux machine and copies the resulting
# .so files into the output folder.
#
# Prerequisites (Ubuntu 22.04):
#   - build-essential, cmake, ninja-build, rsync
#   - Qt 6.5.3 gcc_64 at /opt/qt/6.5.3/gcc_64
#   - the Linux UNIGINE 2.22 SDK: its include/ folder and, in bin/ (or lib/),
#       libUnigine_x64.so libUnigine_x64d.so
#       libUnigine_double_x64.so libUnigine_double_x64d.so
#       libEditorCore_x64.so libEditorCore_x64d.so
#       libEditorCore_double_x64.so libEditorCore_double_x64d.so
#
# The sources are mirrored into the native Linux filesystem first: building
# directly on /mnt/<drive> is an order of magnitude slower (9p filesystem).
set -e

# Override via environment:
#   SRC  - the plugin sources (this folder by default)
#   SDK  - the root of the Linux UNIGINE SDK
#   QT   - Qt gcc_64
#   OUT  - where the .so files are copied to
#   WORK - the build workspace on the native filesystem
SRC=${SRC:-$(cd "$(dirname "$0")" && pwd)}
SDK=${SDK:-~/.local/share/unigine/browser/sdks/sim_linux_2.22_bin}
QT=${QT:-/opt/qt/6.5.3/gcc_64}
WORK=${WORK:-~/artist-tool-linux}
OUT=${OUT:-$SRC/../../../../bin/plugins/zloy_pingvin/artist_tool}

[ -d "$SDK/include" ] || { echo "MISSING: $SDK/include - set SDK to the root of the Linux UNIGINE SDK"; exit 1; }
[ -d "$QT" ] || { echo "MISSING: $QT - set QT to Qt 6.5.3 gcc_64"; exit 1; }

echo "=== syncing sources ==="
mkdir -p "$WORK/src" "$WORK/out"
rsync -a --delete --exclude 'moc_*.cpp' --exclude 'x64' --exclude '.git' "$SRC/" "$WORK/src/"

build_variant() {
	local dir=$1 type=$2 double=$3
	echo "=== $dir (type=$type double=$double) ==="
	cmake -S "$WORK/src" -B "$WORK/$dir" -G Ninja \
		-DCMAKE_BUILD_TYPE=$type -DUNIGINE_DOUBLE=$double \
		-DCMAKE_PREFIX_PATH="$QT" -DUNIGINE_SDK_PATH="$SDK" \
		-DARTIST_TOOL_OUTPUT_DIR="$WORK/out" >/dev/null
	cmake --build "$WORK/$dir"
}

build_variant build_x64          Release        0
build_variant build_x64d         RelWithDebInfo 0
build_variant build_double_x64   Release        1
build_variant build_double_x64d  RelWithDebInfo 1

echo "=== copying results ==="
mkdir -p "$OUT"
cp -v "$WORK"/out/lib*.so "$OUT/"

echo "=== done ==="
ls -la "$OUT"/lib*.so
