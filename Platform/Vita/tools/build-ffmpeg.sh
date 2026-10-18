#!/bin/sh
# Builds the FFmpeg libraries the PS Vita port plays the game's movies with: the
# Bink demuxer, the Bink video and audio decoders, and the colour conversion of
# libswscale, as static libraries for arm-vita-eabi. The SDK's own FFmpeg package
# leaves Bink out.
#
# usage: build-ffmpeg.sh PREFIX
#
# Needs VITASDK, a POSIX shell with make, curl, tar, xz and sha256sum, and a C
# compiler for the build machine, HOST_CC (default gcc). On Windows that is MSYS2
# with its MinGW compiler; the SDK's compiler needs TMP/TEMP as Windows paths:
#   C:\msys64\usr\bin\env.exe PATH=/usr/bin:/mingw64/bin TMPDIR=/tmp TMP=D:/some/dir TEMP=D:/some/dir
#       VITASDK=... HOST_CC=/mingw64/bin/gcc C:\msys64\usr\bin\bash.exe build-ffmpeg.sh PREFIX
# The libraries and headers go to PREFIX/lib and PREFIX/include, where the vita
# preset looks for them in build/vita-ffmpeg (VITA_FFMPEG_DIR).

set -e

FFMPEG_VERSION=9.0.2
FFMPEG_SHA256=8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e

if [ -z "$1" ] || [ -z "$VITASDK" ]; then
	echo "usage: VITASDK=... $0 PREFIX" >&2
	exit 1
fi
mkdir -p "$1"
prefix=$(cd "$1" && pwd)
work="$prefix/src"
mkdir -p "$work"
cd "$work"

archive="ffmpeg-$FFMPEG_VERSION.tar.xz"
if [ ! -f "$archive" ]; then
	curl -sSL -o "$archive.part" "https://ffmpeg.org/releases/$archive"
	mv "$archive.part" "$archive"
fi
echo "$FFMPEG_SHA256  $archive" | sha256sum -c -

rm -rf "ffmpeg-$FFMPEG_VERSION"
tar -xJf "$archive"
cd "ffmpeg-$FFMPEG_VERSION"

# configure compares files with cmp, which MSYS2 does not install by default.
if ! command -v cmp > /dev/null 2>&1; then
	mkdir -p "$work/shim"
	cat > "$work/shim/cmp" << 'EOF'
#!/bin/sh
# Enough of cmp for FFmpeg's configure: "cmp -s A B" tells whether two files are equal.
[ "$1" = "-s" ] && shift
[ -f "$1" ] && [ -f "$2" ] || exit 2
a=$(sha256sum < "$1")
b=$(sha256sum < "$2")
[ "$a" = "$b" ]
EOF
	chmod +x "$work/shim/cmp"
	PATH="$work/shim:$PATH"
fi

# The switches follow the SDK's package (vitasdk/packages, ffmpeg/VITABUILD) without
# -ffast-math, which this project does not use, and without --enable-small, which
# trades speed for size. -fno-short-enums is what the game is built with
# (cmake/vita.cmake): the compiler's default makes an enum as small as its values,
# and FFmpeg's structures, full of enums, would be laid out differently in the
# libraries and in the game. The game read the frame counter of a movie from the
# wrong place and waited for a frame that never came.
./configure --prefix="$prefix" \
	--enable-cross-compile \
	--cross-prefix="$VITASDK/bin/arm-vita-eabi-" \
	--host-cc="${HOST_CC:-gcc}" \
	--arch=armv7-a \
	--cpu=cortex-a9 \
	--target-os=none \
	--disable-shared \
	--enable-static \
	--disable-runtime-cpudetect \
	--disable-armv5te \
	--disable-armv6t2 \
	--disable-programs \
	--disable-doc \
	--disable-network \
	--disable-debug \
	--disable-everything \
	--disable-avdevice \
	--disable-avfilter \
	--disable-swresample \
	--enable-decoder=bink,binkaudio_rdft,binkaudio_dct \
	--enable-demuxer=bink \
	--disable-bzlib \
	--disable-iconv \
	--disable-lzma \
	--disable-zlib \
	--disable-sdl2 \
	--disable-securetransport \
	--disable-xlib \
	--enable-pthreads \
	--extra-cflags="-std=gnu11 -O2 -ftree-vectorize -fomit-frame-pointer -fno-short-enums -D_BSD_SOURCE -Wno-error=implicit-function-declaration -Wno-error=int-conversion -Wno-error=incompatible-pointer-types"

make -j4
make install
