#!/bin/bash
# Builds the two NVK build-time dependencies that HaikuPorts does not
# package, matched to the installed LLVM:
#
#   SPIRV-LLVM-Translator  (LLVMSPIRVLib, llvm-spirv)
#   libclc                 (spirv-mesa3d- and spirv64-mesa3d- targets)
#
# Both are only used while Mesa builds (mesa_clc / vtn_bindgen2 compile NVK's
# internal OpenCL shaders into the driver), so nothing here is needed at run
# time. Results go to deps.<arch>/install, where Build.sh looks for them.
#
# Prerequisites (HaikuPorts): llvm20 llvm20_clang llvm20_libs (or another
# llvmNN set), cmake, ninja, git, python3.
#
# Environment: LLVM_CONFIG (default llvm-config), DEPS_DIR, JOBS.
set -euo pipefail

baseDir="$(cd "$(dirname "$0")/.." && pwd)"
arch="$(getarch 2>/dev/null || uname -m)"
work="${DEPS_DIR:-$baseDir/deps.$arch}"
prefix="$work/install"
src="$work/src"
jobs="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
llvmConfig="${LLVM_CONFIG:-llvm-config}"

die() { echo "build-clc-deps: $*" >&2; exit 1; }

for tool in "$llvmConfig" cmake ninja git python3; do
	command -v "$tool" >/dev/null || die "'$tool' not found (see the prerequisites at the top of this script)"
done

llvmVersion="$("$llvmConfig" --version)"     # e.g. 20.1.8
llvmVersion="${llvmVersion%%[^0-9.]*}"        # drop suffixes such as "git"
llvmMajor="${llvmVersion%%.*}"
llvmMinor="${llvmVersion#*.}"; llvmMinor="${llvmMinor%%.*}"
llvmCmakeDir="$("$llvmConfig" --cmakedir)"
llvmBinDir="$("$llvmConfig" --bindir)"
echo "LLVM $llvmVersion (cmake: $llvmCmakeDir, tools: $llvmBinDir)"
echo "output: $prefix"

# libclc runs these four tools. It looks in LLVM's tools directory only, so
# point it elsewhere if a package installs them somewhere else.
toolDir="$llvmBinDir"
for tool in clang llvm-as llvm-link opt; do
	if [ ! -x "$toolDir/$tool" ]; then
		found="$(command -v "$tool" || true)"
		[ -n "$found" ] || die "'$tool' not found; install the LLVM $llvmMajor clang package (e.g. llvm${llvmMajor}_clang)"
		toolDir="$(dirname "$found")"
	fi
done
for tool in clang llvm-as llvm-link opt; do
	[ -x "$toolDir/$tool" ] || die "clang, llvm-as, llvm-link and opt must be in one directory (found some in $toolDir)"
done
clangVersion="$("$toolDir/clang" --version | sed -n 's/.*clang version \([0-9][0-9.]*\).*/\1/p' | head -1)"
[ "${clangVersion%%.*}" = "$llvmMajor" ] || die "clang $clangVersion does not match LLVM $llvmVersion"

mkdir -p "$src"

# Shallow clone of one ref; reuses an existing checkout of the same ref.
function fetchRef {
	local url="$1" ref="$2" dir="$3"
	if [ ! -d "$dir/.git" ]; then
		git init -q "$dir"
		git -C "$dir" remote add origin "$url"
	fi
	git -C "$dir" fetch -q --depth 1 origin "$ref"
	git -C "$dir" checkout -q --force FETCH_HEAD
}

# 1. SPIRV-LLVM-Translator, from the release branch for this LLVM major.
sltDir="$src/SPIRV-LLVM-Translator"
fetchRef https://github.com/KhronosGroup/SPIRV-LLVM-Translator.git "llvm_release_${llvmMajor}0" "$sltDir"
headersRef="$(tr -d '[:space:]' < "$sltDir/spirv-headers-tag.conf")"
headersDir="$src/SPIRV-Headers"
fetchRef https://github.com/KhronosGroup/SPIRV-Headers.git "$headersRef" "$headersDir"
echo "SPIRV-LLVM-Translator $(git -C "$sltDir" rev-parse --short HEAD), SPIRV-Headers ${headersRef:0:12}"

# Static: mesa_clc links it and runs during the build, where a shared copy
# in this directory would not be on the library search path.
cmake -S "$sltDir" -B "$work/build-SPIRV-LLVM-Translator" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$prefix" \
	-DLLVM_DIR="$llvmCmakeDir" \
	-DLLVM_EXTERNAL_SPIRV_HEADERS_SOURCE_DIR="$headersDir" \
	-DLLVM_SPIRV_INCLUDE_TESTS=OFF \
	-DBUILD_SHARED_LIBS=OFF
ninja -C "$work/build-SPIRV-LLVM-Translator" -j"$jobs"
ninja -C "$work/build-SPIRV-LLVM-Translator" install

# 2. libclc, from the llvm-project tag matching the installed LLVM exactly.
llvmSrc="$src/llvm-project"
if [ ! -d "$llvmSrc/.git" ]; then
	git clone -q --depth 1 --filter=blob:none --sparse --branch "llvmorg-$llvmVersion" \
		https://github.com/llvm/llvm-project.git "$llvmSrc"
fi
git -C "$llvmSrc" sparse-checkout set libclc cmake

clcTargets=(spirv-mesa3d- spirv64-mesa3d-)

if grep -q LIBCLC_TARGETS_TO_BUILD "$llvmSrc/libclc/CMakeLists.txt"; then
	# libclc up to LLVM 22: one build for a list of targets; installs
	# share/clc/<target>.spv and libclc.pc itself.
	libclcArgs=()
	if [ "$toolDir" != "$llvmBinDir" ]; then
		libclcArgs+=(-DLIBCLC_CUSTOM_LLVM_TOOLS_BINARY_DIR="$toolDir")
	fi
	cmake -S "$llvmSrc/libclc" -B "$work/build-libclc" -G Ninja \
		-DCMAKE_BUILD_TYPE=Release \
		-DCMAKE_INSTALL_PREFIX="$prefix" \
		-DLLVM_DIR="$llvmCmakeDir" \
		-DLLVM_SPIRV="$prefix/bin/llvm-spirv" \
		-DLIBCLC_TARGETS_TO_BUILD="$(IFS=';'; echo "${clcTargets[*]}")" \
		"${libclcArgs[@]}"
	ninja -C "$work/build-libclc" -j"$jobs"
	ninja -C "$work/build-libclc" install
else
	# Newer libclc (LLVM 23): one target per build, taken from
	# LLVM_DEFAULT_TARGET_TRIPLE; compiled through a CMake "CLC" language
	# that falls back to the C compiler unless told to use clang; installs
	# share/clc/<target>/libclc.spv and no libclc.pc. Build each target,
	# then lay the results out the way Mesa looks for them.
	llvmTool() { [ -x "$toolDir/$1" ] && echo "$toolDir/$1" || command -v "$1" || die "'$1' not found"; }
	for name in "${clcTargets[@]}"; do
		# The triple doubles as clang's --target, so it must be a real one:
		# clang rejects the "mesa3d" OS for SPIR-V, and plain "spirv" means
		# Vulkan-only logical SPIR-V. OpenCL's physical SPIR-V is spirv32 /
		# spirv64. ("mesa3d" was only ever a libclc target name; older libclc
		# compiled these with spir--/spir64--.) Mesa loads the results under
		# the old names.
		case "$name" in
			spirv-*)   t=spirv32-unknown-unknown ;;
			spirv64-*) t=spirv64-unknown-unknown ;;
		esac
		cmake -S "$llvmSrc/libclc" -B "$work/build-libclc-$t" -G Ninja \
			-DCMAKE_BUILD_TYPE=Release \
			-DCMAKE_INSTALL_PREFIX="$prefix" \
			-DLLVM_DIR="$llvmCmakeDir" \
			-DLLVM_SPIRV="$prefix/bin/llvm-spirv" \
			-DLLVM_DEFAULT_TARGET_TRIPLE="$t" \
			-DCMAKE_CLC_COMPILER="$toolDir/clang" \
			-DLLVM_TOOL_opt="$toolDir/opt" \
			-DLLVM_TOOL_llvm-link="$toolDir/llvm-link" \
			-DCMAKE_AR="$(llvmTool llvm-ar)" \
			-DCMAKE_RANLIB="$(llvmTool llvm-ranlib)"
		ninja -C "$work/build-libclc-$t" -j"$jobs"
		ninja -C "$work/build-libclc-$t" install
		[ -s "$prefix/share/clc/$t/libclc.spv" ] || die "libclc did not install share/clc/$t/libclc.spv"
		cp "$prefix/share/clc/$t/libclc.spv" "$prefix/share/clc/$name.spv"
	done
	# Same contents as the libclc.pc that older libclc installs.
	mkdir -p "$prefix/share/pkgconfig"
	cat > "$prefix/share/pkgconfig/libclc.pc" <<-EOF
		includedir=$prefix/include
		libexecdir=$prefix/share/clc

		Name: libclc
		Description: Library requirements of the OpenCL C programming language
		Version: 0.2.0
		Cflags: -I\${includedir}
		Libs: -L\${libexecdir}
	EOF
fi

# 3. Check what Mesa will look for.
export PKG_CONFIG_PATH="$prefix/lib/pkgconfig:$prefix/share/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
sltVersion="$(pkg-config --modversion LLVMSPIRVLib)" || die "LLVMSPIRVLib.pc not installed"
clcVersion="$(pkg-config --modversion libclc)" || die "libclc.pc not installed"
clcDir="$(pkg-config --variable=libexecdir libclc)"
for f in spirv-mesa3d-.spv spirv64-mesa3d-.spv; do
	[ -s "$clcDir/$f" ] || die "missing $clcDir/$f"
done
# Mesa accepts LLVMSPIRVLib >= major.minor and < major.(minor+1) of LLVM.
case "$sltVersion" in
	"$llvmMajor.$llvmMinor".*) ;;
	*) die "LLVMSPIRVLib $sltVersion does not match LLVM $llvmMajor.$llvmMinor; Mesa will reject it" ;;
esac

echo
echo "Done: LLVMSPIRVLib $sltVersion, libclc $clcVersion ($clcDir)"
echo "Build.sh finds them in $prefix automatically."
