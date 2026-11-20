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
#   5. libglvnd, libclc, SPIRV-LLVM-Translator and SPIRV-Tools, built into deps.<arch>
#      by tools/build-glvnd-deps.sh, build-clc-deps.sh and build-spirv-tools.sh. These take
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

# command  what to ask pkgman for: a provides name, or a package name where
# several packages provide the command (HaikuPorts' cmake and cmake3 both
# provide cmd:cmake, which pkgman can't choose between).
# vulkaninfo isn't listed: HaikuPorts has no Vulkan tools package, and the
# test script records it only when it is installed.
tools=(
	"git cmd:git"
	"meson cmd:meson"
	"ninja cmd:ninja"
	"cmake cmake"
	"make make"
	"m4 m4"
	"objcopy binutils"
	"ar binutils"
	"python3 cmd:python3"
	"pkg-config cmd:pkg_config"
	"bison cmd:bison"
	"flex cmd:flex"
	"zip cmd:zip"
	"strings cmd:strings"
	"rustc cmd:rustc"
	"bindgen cmd:bindgen"
	"cbindgen cmd:cbindgen"
)
missing=()
for t in "${tools[@]}"; do
	set -- $t
	have "$1" || missing+=("$2")
done

# The Vulkan loader: NVK is an ICD, and applications (and the tests) load
# it through libvulkan. Renga images from 2026.11.13-devel-vulkan don't
# include the loader yet.
# Checked per file: ls with several patterns fails when any one of them
# matches nothing, even if another does.
vulkanFound=0
for f in /boot/system/lib/libvulkan.so* /boot/system/develop/lib/libvulkan.so* \
		/boot/home/config/non-packaged/lib/libvulkan.so*; do
	[ -e "$f" ] && vulkanFound=1
done
[ $vulkanFound = 1 ] || missing+=("vulkan_loader")

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

# Libraries Mesa 26.3 finds through pkg-config, with the minimum versions
# its meson.build asks for, and what to ask pkgman for when one is missing.
# The list is the set of required dependencies from a complete NVK
# configure on Renga; a fresh install stopped at the first of them
# (SPIRV-Tools, checked separately below).
#   wayland-*            -Dplatforms=wayland (Mesa's WSI common code);
#                        wayland-scanner.pc is in wayland_devel too
#   libelf               meson.build stops with "NVK requires libelf" without it
# Build.sh also adds the deps.<arch> directories to Mesa's pkg-config path,
# so look there too.
pcPath="$deps/lib/pkgconfig:$deps/share/pkgconfig:$deps/develop/lib/pkgconfig"
pcMods=(
	"libelf|devel:libelf"
	"wayland-protocols >= 1.49|wayland_protocols"
	"wayland-client >= 1.18|devel:libwayland_client"
	"wayland-server >= 1.18|devel:libwayland_server"
	"wayland-scanner|devel:libwayland_client"
)
if have pkg-config; then
	for m in "${pcMods[@]}"; do
		PKG_CONFIG_PATH="$pcPath${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
			pkg-config --exists "${m%%|*}" || missing+=("${m##*|}")
	done
fi

# SPIRV-Tools: Mesa 26.3 requires it whenever CLC is built, as NVK does.
# The HaikuPorts repository has no package providing devel:libSPIRV_Tools
# ("Name not found" on Renga 143b8c30), so tools/build-spirv-tools.sh
# builds it into deps.<arch>, like libclc. An installed copy is used if
# pkg-config finds one.
spirvToolsOk=0
if have pkg-config && PKG_CONFIG_PATH="$pcPath${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" \
		pkg-config --exists 'SPIRV-Tools >= 2024.1'; then
	spirvToolsOk=1
fi

pyMissing=()
python3 -c 'import mako' 2>/dev/null || pyMissing+=(mako)
python3 -c 'import yaml' 2>/dev/null || pyMissing+=(pyyaml)

llvmOk=0
if have llvm-config && have clang; then llvmOk=1; fi

submodulesOk=1
"$base/tools/submodules.sh" check >/dev/null 2>&1 || submodulesOk=0

glvndOk=0; [ -e "$deps/lib/pkgconfig/libglvnd.pc" ] && glvndOk=1
clcOk=0; [ -e "$deps/share/pkgconfig/libclc.pc" ] && clcOk=1

echo "Build tools and development libraries: ${missing[*]:-all present}"
echo "Python modules:   ${pyMissing[*]:-all present}"
echo "LLVM and clang:   $([ $llvmOk = 1 ] && llvm-config --version || echo missing)"
echo "Submodules:       $([ $submodulesOk = 1 ] && echo checked out || echo not all checked out)"
echo "libglvnd:         $([ $glvndOk = 1 ] && echo built || echo not built)"
echo "libclc, SPIRV-LLVM-Translator: $([ $clcOk = 1 ] && echo built || echo not built)"
echo "SPIRV-Tools:      $([ $spirvToolsOk = 1 ] && echo present || echo not built)"

if [ ${#missing[@]} -eq 0 ] && [ ${#pyMissing[@]} -eq 0 ] && [ $llvmOk = 1 ] && [ $mesonOld = 0 ] &&
		[ $submodulesOk = 1 ] && [ $glvndOk = 1 ] && [ $clcOk = 1 ] && [ $spirvToolsOk = 1 ]; then
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
	# One package at a time: pkgman installs a list as one transaction, so a
	# single name it can't resolve would block all the others.
	failed=()
	for p in "${missing[@]}"; do
		pkgman install -y "$p" || failed+=("$p")
	done
	[ ${#failed[@]} -eq 0 ] || fail "pkgman could not install: ${failed[*]}"
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
	"$base/tools/submodules.sh" update || fail "git submodule update failed"
fi
if [ $glvndOk = 0 ]; then
	"$base/tools/build-glvnd-deps.sh" || fail "tools/build-glvnd-deps.sh failed"
fi
if [ $clcOk = 0 ]; then
	"$base/tools/build-clc-deps.sh" || fail "tools/build-clc-deps.sh failed"
fi
if [ $spirvToolsOk = 0 ]; then
	"$base/tools/build-spirv-tools.sh" || fail "tools/build-spirv-tools.sh failed"
fi
echo
echo "Done. Next: tools/renga-test.sh build"
