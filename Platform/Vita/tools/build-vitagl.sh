#!/bin/sh
# Builds the vitaGL the PS Vita port draws with: the commit of the SDK's package
# (vitasdk/packages, vitaGL/VITABUILD), built the same way (make NO_DEBUG=1), with
# these patches from ../patches applied in this order:
#
# vitagl-light-range.patch gives a point light a range beyond which it gives no
# light, as Direct3D does; vitaGL's lighting has no such end, and the flashes of
# guns and explosions lit far sides of buildings.
#
# vitagl-texcoord-gen.patch lets the fixed function shaders compute texture
# coordinates from the camera space position, normal or reflection vector
# (vglTexCoordGen), as Direct3D's D3DTSS_TCI_CAMERASPACE* modes do; vitaGL has no
# texture coordinate generation, and reflections on buildings were only approximated
# from the normal. It
# also keeps the compiled fixed function shaders apart from those of the original
# vitaGL (vglSetFfpShaderCachePath and a folder named after the patch revision):
# vitaGL keeps them in a folder other applications share, named only after its
# own version, and took shaders compiled from the original sources from there.
#
# vitagl-specular-power.patch keeps the fixed function lighting from computing
# pow(0, 0) for the specular term, which the GPU makes NaN. It is computed for every
# light also when the material has no specular color, and with the shininess of 0 that
# most of the game's materials have, the faces lit by the sun but turned away from the
# camera came out white and flickered while the camera turned.
#
# usage: build-vitagl.sh PREFIX
#
# Needs VITASDK, git, make and a POSIX shell (on Windows MSYS2, as for
# build-ffmpeg.sh; TMP/TEMP must be Windows paths for the SDK's compiler). The
# library and its header go to PREFIX/lib and PREFIX/include, where the vita preset
# looks for them in build/vita-vitagl (VITA_VITAGL_DIR); without them it links the
# SDK's vitaGL, the lights have no range and reflections are approximated.

set -e

VITAGL_COMMIT=2bdbe895ddc6a7e410a9ea55a8f53d8173c61ad8

if [ -z "$1" ] || [ -z "$VITASDK" ]; then
	echo "usage: VITASDK=... $0 PREFIX" >&2
	exit 1
fi
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$1"
prefix=$(cd "$1" && pwd)
source="$prefix/src/vitaGL"

if [ ! -d "$source/.git" ]; then
	rm -rf "$source"
	git clone https://github.com/Rinnegatamante/vitaGL.git "$source"
fi
git -C "$source" checkout -q -f "$VITAGL_COMMIT"
git -C "$source" clean -q -f -d -x
git -C "$source" apply "$here/../patches/vitagl-light-range.patch"
git -C "$source" apply "$here/../patches/vitagl-texcoord-gen.patch"
git -C "$source" apply "$here/../patches/vitagl-specular-power.patch"

PATH="$VITASDK/bin:$PATH"
make -C "$source" NO_DEBUG=1 -j4

mkdir -p "$prefix/lib" "$prefix/include"
cp "$source/libvitaGL.a" "$prefix/lib/"
cp "$source/source/vitaGL.h" "$prefix/include/"
