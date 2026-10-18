#!/bin/bash
# Builds the test of the Direct3D 8 layer (see driver.cpp) with the layer's sources in a
# directory, and runs it without and with the render thread. The two traces have to be
# the same; compare.sh compares the traces of two versions of the layer.
#
#   run.sh <directory with the layer's sources> <name> [frames] [seed]
#
# CXX         the compiler, g++ unless set; it has to have the Direct3D 8 headers (MinGW-w64)
# VITASDK     where vitaGL.h comes from, unless the vitaGL of tools/build-vitagl.sh is
#             built (build/vita-vitagl), which the game then links
# D3D8TEST_OUT  where everything goes, build/hosttest/d3d8 of the repository unless set
set -e

HERE=$(cd "$(dirname "$0")" && pwd)
LAYER=$1
NAME=$2
FRAMES=${3:-60}
SEED=${4:-1}
OUT=${D3D8TEST_OUT:-$HERE/../../../../build/hosttest/d3d8}
CXX=${CXX:-g++}

if [ -z "$LAYER" ] || [ -z "$NAME" ]; then
	echo "run.sh <directory with the layer's sources> <name> [frames] [seed]"
	exit 2
fi

VITAGL_HEADER=$HERE/../../../../build/vita-vitagl/include/vitaGL.h
[ -f "$VITAGL_HEADER" ] || VITAGL_HEADER=$VITASDK/arm-vita-eabi/include/vitaGL.h

mkdir -p "$OUT/include"
cp "$VITAGL_HEADER" "$OUT/include/"

"$CXX" -O1 -g -std=c++17 -Wall -DNOMINMAX -DWIN32_LEAN_AND_MEAN -DVITA_FRAME_PROFILE -static \
	-I "$HERE/stubs" -I "$OUT/include" -I "$LAYER" -I "$HERE/../../compat/include" \
	-o "$OUT/test-$NAME" \
	"$HERE/driver.cpp" "$HERE/mockgl.cpp" \
	"$LAYER/VitaD3D8Device.cpp" "$LAYER/VitaD3D8Direct3D.cpp" "$LAYER/VitaD3D8Format.cpp" \
	"$LAYER/VitaD3D8RenderThread.cpp" "$LAYER/VitaD3D8Resources.cpp" "$LAYER/VitaD3D8State.cpp" \
	-lpthread

VITA_RENDER_THREAD=0 "$OUT/test-$NAME" "$OUT/$NAME-direct.txt" "$FRAMES" "$SEED" 2> "$OUT/$NAME-direct.log" || { tail -n 30 "$OUT/$NAME-direct.log"; exit 1; }
tail -n 2 "$OUT/$NAME-direct.log"
VITA_RENDER_THREAD=1 "$OUT/test-$NAME" "$OUT/$NAME-threaded.txt" "$FRAMES" "$SEED" 2> "$OUT/$NAME-threaded.log" || { tail -n 30 "$OUT/$NAME-threaded.log"; exit 1; }
tail -n 2 "$OUT/$NAME-threaded.log"

if cmp -s "$OUT/$NAME-direct.txt" "$OUT/$NAME-threaded.txt"; then
	echo "$NAME: the render thread draws what the game's thread would ($(wc -l < "$OUT/$NAME-direct.txt") lines of trace)"
else
	echo "$NAME: THE TRACES DIFFER with and without the render thread"
	diff "$OUT/$NAME-direct.txt" "$OUT/$NAME-threaded.txt" | head -n 30
	exit 1
fi
