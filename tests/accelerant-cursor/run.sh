#!/bin/sh
# Builds and runs the accelerant hardware-cursor test against the real
# accelerant and nvrm_cpp_sdk sources, with a fake kernel underneath.
#
# On Haiku: tests/accelerant-cursor/run.sh
#   (uses the NVIDIA headers in nvrm_sdk/open-gpu-kernel-modules; set NV_SRC
#   to point elsewhere)
# Elsewhere (a compile-and-logic check only): set HAIKU_HEADERS to a Haiku
#   source tree's headers/ directory and NV_SRC to an open-gpu-kernel-modules
#   570.133.07 tree.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"
NV_SRC="${NV_SRC:-$repo/nvrm_sdk/open-gpu-kernel-modules}"
out="$here/build.$(uname -m)"
mkdir -p "$out"

if [ ! -f "$NV_SRC/src/nvidia-modeset/interface/nvkms-api.h" ]; then
	echo "NVIDIA headers not found in $NV_SRC (set NV_SRC)" >&2
	exit 1
fi

inc="-I$NV_SRC/src/common/sdk/nvidia/inc -I$NV_SRC/src/common/unix/common/inc \
 -I$NV_SRC/src/common/inc -I$NV_SRC/src/nvidia/arch/nvalloc/common/inc \
 -I$NV_SRC/src/nvidia/arch/nvalloc/unix/include -I$NV_SRC/src/nvidia/inc/kernel \
 -I$NV_SRC/src/nvidia-modeset/interface \
 -I$repo/nvrm_sdk -I$repo/nvrm_cpp_sdk -I$repo/accelerant"
defs=""
san=""
if [ "$(uname -s)" != Haiku ]; then
	H="${HAIKU_HEADERS:?set HAIKU_HEADERS to a Haiku tree's headers/ directory}"
	inc="$inc -I$H -I$H/os -I$H/os/support -I$H/os/kernel -I$H/os/interface \
 -I$H/os/app -I$H/os/storage -I$H/os/add-ons/graphics -I$H/os/drivers \
 -include config/types.h"
	# -w: Haiku's and glibc's errno macros collide here; the accelerant
	# itself is compile-checked with warnings on elsewhere.
	defs="-D__HAIKU__ -DCURSOR_TEST_HOST -DPAGESIZE=4096 -w"
	san="-fsanitize=address,undefined -fno-omit-frame-pointer"
fi

CXX="${CXX:-g++}"
CC="${CC:-gcc}"
flags="-O1 -g -U_FORTIFY_SOURCE $defs $san $inc"

$CC $flags -c "$NV_SRC/src/common/shared/nvstatus/nvstatus.c" -o "$out/nvstatus.o"
srcs="$repo/accelerant/Accelerant.cpp $repo/accelerant/NvKmsBitmap.cpp \
 $repo/accelerant/NvUtils.cpp $repo/nvrm_cpp_sdk/ErrorUtils.cpp \
 $repo/nvrm_cpp_sdk/NvRmApi.cpp $repo/nvrm_cpp_sdk/NvRmDevice.cpp \
 $repo/nvrm_cpp_sdk/NvKmsApi.cpp $repo/nvrm_cpp_sdk/NvKmsDevice.cpp \
 $repo/nvrm_cpp_sdk/NvKmsSurface.cpp $here/cursor-test.cpp"
wraps="-Wl,--wrap=open -Wl,--wrap=close -Wl,--wrap=ioctl -Wl,--wrap=delete_area \
 -Wl,--wrap=load_driver_settings -Wl,--wrap=get_driver_boolean_parameter \
 -Wl,--wrap=unload_driver_settings"
$CXX -std=c++20 $flags $srcs "$out/nvstatus.o" $wraps -o "$out/cursor-test"
"$out/cursor-test"
