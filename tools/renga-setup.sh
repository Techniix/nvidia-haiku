#!/bin/bash
# One-time preparation of a fresh Renga install for building Quasar.
#
#   tools/renga-setup.sh --check   report what is missing, change nothing
#   tools/renga-setup.sh           install what is missing (asks first)
#   tools/renga-setup.sh --yes     install without asking
#
# Steps:
#   1. Build tools from HaikuPorts/RengaPorts through pkgman, asked for by
#      what they provide (cmd:meson ...), so package renames do not matter.
#   2. Python modules Mesa's generators need (mako, PyYAML).
#   3. LLVM with clang, which NVK's internal shaders are compiled with.
#   4. The submodules (git submodule update --init).
#   5. libglvnd, libclc and SPIRV-LLVM-Translator, built into deps.<arch>
#      by tools/build-glvnd-deps.sh and tools/build-clc-deps.sh. These take
#      the longest (libclc and the translator build against LLVM); they are
#      skipped when already built.
#
# Nothing is installed outside pkgman and this repository's deps.<arch>.
set -uo pipefail

mode=install; assumeYes=0
for arg in "$@"; do
	case "$arg" in
		--check) mode=check ;;
		--yes|-y) assumeYes=1 ;;
		-h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "unknown option: $arg (try --help)" >&2; exit 2 ;;
	esac
done

base="$(cd "$(dirname "$0")/.." && pwd)"
arch="$(getarch 2>/dev/null || uname -m)"
deps="${DEPS_DIR:-$base/deps.$arch}/install"
have() { command -v "$1" >/dev/null 2>&1; }

if [ "$(uname -s)" != Haiku ]; then
	echo "renga-setup: this runs on Renga" >&2
	exit 1
fi

# command  provides-name-for-pkgman
tools=(
	"git cmd:git"
	"meson cmd:meson"
	"ninja cmd:ninja"
	"cmake cmd:cmake"
	"python3 cmd:python3"
	"pkg-config cmd:pkg_config"
	"bison cmd:bison"
	"flex cmd:flex"
	"glslangValidator cmd:glslangValidator"
	"zip cmd:zip"
	"strings cmd:strings"
)
missing=()
for t in "${tools[@]}"; do
	set -- $t
	have "$1" || missing+=("$2")
done

# Quasar builds against Renga's own headers: the GPU presentation and
# cursor interfaces with the R-1 semaphore fields (Renga f6b8442e), eventfd in
# libroot, and user_fd_kernel_ioctl() with C linkage (0b53eaf9). Images from
# 2026.11.13-devel-vulkan on have them, with haiku_devel installed.
H=/boot/system/develop/headers
if ! grep -qs B_GPU_PRESENT_SEMAPHORES "$H/os/add-ons/graphics/GpuPresent.h" ||
		[ ! -f "$H/os/add-ons/graphics/HardwareCursor.h" ] ||
		[ ! -f "$H/posix/sys/eventfd.h" ]; then
	echo "renga-setup: this Renga image is too old for Quasar: it needs Renga image 2026.11.13-devel-vulkan or newer (GpuPresent.h with semaphores, HardwareCursor.h, eventfd in libroot). Update Renga, then run setup again." >&2
	exit 1
fi

# Mesa 26.3 needs meson 1.5.0 or later.
mesonOld=0
if have meson; then
	mv="$(meson --version 2>/dev/null)"
	if [ "$(printf '%s\n1.5.0\n' "$mv" | sort -V | head -1)" != 1.5.0 ]; then
		mesonOld=1
		echo "meson $mv is older than 1.5.0, which Mesa 26.3 needs: pkgman update meson"
	fi
fi

# Python's XML parser: Mesa's and libglvnd's generators parse the Khronos
# XML registries with xml.etree, which needs pyexpat. If the python3
# package was built against a newer libexpat than the one installed,
# pyexpat fails with "Symbol not found" and every generator step of the
# build fails. Check it before anything else.
if ! python3 -c 'import pyexpat' 2>/dev/null; then
	echo "Python cannot load its XML parser (pyexpat):"
	python3 -c 'import pyexpat' 2>&1 | tail -1 | sed 's/^/  /'
	echo "The installed libexpat is probably older than the one python3 was"
	echo "built with. Try: pkgman update expat"
	echo "If expat is already the newest available, the OS image needs a newer"
	echo "expat (or a python3 built against the one it has)."
	exit 1
fi

# Development libraries Mesa links against. pkg-config can find a library's
# .pc file from its runtime package while the libX.so link file is only in
# the _devel package; the first Renga build failed that way on zstd
# ("cannot find -lzstd" linking mesa_clc). Checked by the link file.
for lib in zstd z expat; do
	found=0
	for d in /boot/system/develop/lib /boot/home/config/develop/lib; do
		[ -e "$d/lib$lib.so" ] && found=1
	done
	[ $found = 1 ] || missing+=("devel:lib$lib")
done

pyMissing=()
python3 -c 'import mako' 2>/dev/null || pyMissing+=(mako)
python3 -c 'import yaml' 2>/dev/null || pyMissing+=(pyyaml)

llvmOk=0
if have llvm-config && have clang; then llvmOk=1; fi

submodulesOk=1
git -C "$base" submodule status 2>/dev/null | grep -q '^-' && submodulesOk=0

glvndOk=0; [ -e "$deps/lib/pkgconfig/libglvnd.pc" ] && glvndOk=1
clcOk=0; [ -e "$deps/share/pkgconfig/libclc.pc" ] && clcOk=1

echo "Build tools and development libraries: ${missing[*]:-all present}"
echo "Python modules:   ${pyMissing[*]:-all present}"
echo "LLVM and clang:   $([ $llvmOk = 1 ] && llvm-config --version || echo missing)"
echo "Submodules:       $([ $submodulesOk = 1 ] && echo checked out || echo not all checked out)"
echo "libglvnd:         $([ $glvndOk = 1 ] && echo built || echo not built)"
echo "libclc, SPIRV-LLVM-Translator: $([ $clcOk = 1 ] && echo built || echo not built)"

if [ ${#missing[@]} -eq 0 ] && [ ${#pyMissing[@]} -eq 0 ] && [ $llvmOk = 1 ] && [ $mesonOld = 0 ] &&
		[ $submodulesOk = 1 ] && [ $glvndOk = 1 ] && [ $clcOk = 1 ]; then
	echo; echo "Everything needed to build is in place."
	exit 0
fi
if [ "$mode" = check ]; then
	echo; echo "Check only: nothing was changed. Run tools/renga-setup.sh to install."
	exit 1
fi
if [ $assumeYes = 0 ]; then
	printf '\nInstall what is missing now? Building libclc can take an hour. [y/N] '
	read -r answer
	case "$answer" in y|Y|yes|YES) ;; *) echo "Nothing changed."; exit 0 ;; esac
fi

fail() { echo "renga-setup: $*" >&2; exit 1; }

if [ ${#missing[@]} -gt 0 ]; then
	pkgman install -y "${missing[@]}" || fail "pkgman could not install: ${missing[*]}"
fi
if [ $mesonOld = 1 ]; then
	pkgman update -y meson || fail "could not update meson to 1.5.0 or later"
fi
if [ $llvmOk = 0 ]; then
	# The newest LLVM set in the repositories; build-clc-deps.sh matches it.
	pkgman install -y llvm20 llvm20_clang llvm20_libs ||
		fail "could not install LLVM 20 with clang; install an llvmNN, llvmNN_clang and llvmNN_libs set and rerun"
fi
for m in "${pyMissing[@]}"; do
	pkgman install -y "${m}_python3" 2>/dev/null ||
		python3 -m pip install --user "$m" ||
		fail "could not install the Python module $m"
done
if [ $submodulesOk = 0 ]; then
	git -C "$base" submodule update --init || fail "git submodule update failed"
fi
if [ $glvndOk = 0 ]; then
	"$base/tools/build-glvnd-deps.sh" || fail "tools/build-glvnd-deps.sh failed"
fi
if [ $clcOk = 0 ]; then
	"$base/tools/build-clc-deps.sh" || fail "tools/build-clc-deps.sh failed"
fi
echo
echo "Done. Next: tools/renga-test.sh build"
