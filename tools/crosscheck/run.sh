#!/bin/bash
# Syntax-check Quasar's kernel driver, SDK wrappers and accelerant for
# Haiku/Renga on a machine that is not Haiku.
#
# clang compiles every source with -fsyntax-only for x86_64-unknown-haiku,
# against the system and private kernel headers of a Haiku or Renga source
# tree and the NVIDIA headers of the open-gpu-kernel-modules submodule.
#
# It catches: missing includes, wrong types and signatures against the kernel
# and NVIDIA headers, code that does not compile in the Haiku branches.
# It does not catch: GCC-only diagnostics (Haiku builds with GCC 13), link
# errors (an undefined kernel symbol), or anything at run time. A clean run
# here is "Built: no" in docs/RENGA-STATUS.md terms; it only lowers the odds
# of a failed build on the test machine.
#
# Usage: HAIKU_TREE=/path/to/haiku tools/crosscheck/run.sh [file...]
#   HAIKU_TREE  a checkout of Techniix/haiku (ce/main) or haiku/haiku; only
#               its headers/ directory is read.
set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
H="${HAIKU_TREE:?set HAIKU_TREE to a Haiku or Renga source tree}/headers"
nv="$root/open-gpu-kernel-modules"
[ -d "$nv/src" ] || { echo "open-gpu-kernel-modules is not checked out" >&2; exit 2; }
res="$(clang -print-resource-dir)"
cxxdir="${CROSSCHECK_LIBSTDCXX:-$(ls -d /usr/include/c++/* | sort -V | tail -1)}"
cxxarch="${CROSSCHECK_LIBSTDCXX_ARCH:-/usr/include/x86_64-linux-gnu/c++/$(basename "$cxxdir")}"

sys=(-isystem "$res/include")
for d in "" app device drivers game interface kernel locale media net storage \
		support translation add-ons/graphics; do
	sys+=(-isystem "$H/os${d:+/$d}")
done
sys+=(-isystem "$H/compatibility/bsd" -isystem "$H/glibc"
	-isystem "$H/compatibility/gnu" -isystem "$H/posix" -isystem "$H")

kernel=(-D_KERNEL_MODE -mno-red-zone -fno-exceptions
	-I"$H/private" -I"$H/private/shared" -I"$H/private/kernel"
	-I"$H/private/kernel/arch/x86" -I"$H/private/kernel/arch/x86/64"
	-I"$H/private/system" -I"$H/private/system/arch/x86_64"
	-I"$H/private/kernel/boot/platform/efi")

nvinc=(-D_DEFAULT_SOURCE -DNV_PLATFORM_MAX_IOCTL_SIZE=16384
	-I"$nv/src/common/sdk/nvidia/inc" -I"$nv/src/common/unix/common/inc"
	-I"$nv/src/common/inc" -I"$nv/src/nvidia/arch/nvalloc/common/inc"
	-I"$nv/src/nvidia/arch/nvalloc/unix/include" -I"$nv/src/nvidia/inc/kernel"
	-I"$nv/src/nvidia/interface" -I"$nv/src/common" -I"$root/nvrm_sdk")
kmsinc=(-I"$nv/src/nvidia-modeset/interface"
	-I"$nv/src/nvidia-modeset/os-interface/include"
	-I"$nv/src/nvidia-modeset/kapi/interface")

fail=0
count=0
check() { # kind file
	local kind=$1 f=$2 cmd
	case "$f" in
		*.c) cmd=(clang -std=gnu11) ;;
		*)   cmd=(clang++ -std=c++20 -nostdinc++ -isystem "$cxxdir"
		          -isystem "$cxxarch" -isystem "$here/shim"
		          "-D__GLIBC_PREREQ(a,b)=0" -D__locale_t=locale_t) ;;
	esac
	cmd+=(--target=x86_64-unknown-haiku -nostdinc -fsyntax-only
		-Wno-missing-field-initializers -Wno-unknown-attributes
		-Wno-c++11-narrowing)
	case "$kind" in
		kernel)  cmd+=("${kernel[@]}" "${nvinc[@]}" -I"$root/nvidia_gsp") ;;
		modeset) cmd+=("${kernel[@]}" "${nvinc[@]}" "${kmsinc[@]}" -I"$root/nvidia_gsp") ;;
		vulkan)  cmd+=(-I"${VULKAN_HEADERS:-$root/mesa-nvk/include}" -Wall -Wextra) ;;
		user)    cmd+=("${nvinc[@]}" "${kmsinc[@]}" -I"$root/nvrm_cpp_sdk"
		               -I"$root/accelerant" -I"$H/private/graphics/common"
		               -I"$H/private/shared" -I"$H/private/interface"
		               -I"$H/private/app") ;;
	esac
	count=$((count + 1))
	if ! "${cmd[@]}" "${sys[@]}" "$f" 2>"$here/.last.log"; then
		fail=$((fail + 1))
		echo "=== FAILED ($kind) ${f#$root/}"
		grep -E "error|fatal" "$here/.last.log" | head -${CROSSCHECK_MAX_ERRORS:-12}
	fi
}

if [ $# -gt 0 ]; then
	for f in "$@"; do
		case "$f" in
			*nvidia-modeset*) check modeset "$(realpath "$f")" ;;
			*nvidia_gsp*)     check kernel "$(realpath "$f")" ;;
			*RengaWsi*)       check vulkan "$(realpath "$f")" ;;
			*)                check user "$(realpath "$f")" ;;
		esac
	done
else
	for f in "$root"/nvidia_gsp/*.cpp "$root"/nvidia_gsp/nvidia/*.cpp \
			"$root"/nvidia_gsp/nvidia/*.c; do
		check kernel "$f"
	done
	for f in "$root"/nvidia_gsp/nvidia-modeset/*.cpp; do check modeset "$f"; done
	for f in "$root"/RengaWsi/*.cpp; do check vulkan "$f"; done
	for f in "$root"/nvrm_cpp_sdk/*.cpp "$root"/accelerant/Accelerant.cpp \
			"$root"/accelerant/NvUtils.cpp "$root"/accelerant/NvKmsBitmap.cpp \
			"$root"/accelerant/NvPresent.cpp \
			"$root"/accelerant/ModesetTool.cpp; do
		check user "$f"
	done
fi
rm -f "$here/.last.log"
echo "crosscheck: $count files, $fail failed"
[ "$fail" -eq 0 ]
