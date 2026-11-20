#!/bin/bash
# Builds SPIRV-Tools into deps.<arch>/install for the Mesa build.
#
# Mesa 26.3 requires SPIRV-Tools (>= 2024.1) whenever it builds CLC, which
# NVK always does: mesa_clc uses it to link and validate the SPIR-V of
# NVK's internal OpenCL shaders. The HaikuPorts repository the fresh Renga
# install sees has no package providing devel:libSPIRV_Tools, so
# tools/renga-setup.sh runs this script when pkg-config cannot find
# SPIRV-Tools.
#
# Built as static, position-independent libraries: Mesa links them into
# mesa_clc (run during the build) and into the driver, so nothing has to be
# installed for run time and nothing in deps.<arch> has to be on the
# library search path later.
#
# SPIRV-Tools is pinned to a release tag, and SPIRV-Headers to the revision
# that release's DEPS file names, so the two always match.
#
# Prerequisites: cmake, ninja, git, python3, a C++17 compiler.
# Environment: DEPS_DIR, JOBS, SPIRV_TOOLS_TAG (default below).
set -euo pipefail

baseDir="$(cd "$(dirname "$0")/.." && pwd)"
arch="$(getarch 2>/dev/null || uname -m)"
work="${DEPS_DIR:-$baseDir/deps.$arch}"
prefix="$work/install"
src="$work/src"
jobs="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
tag="${SPIRV_TOOLS_TAG:-v2026.4}"

die() { echo "build-spirv-tools: $*" >&2; exit 1; }

for tool in cmake ninja git python3; do
	command -v "$tool" >/dev/null || die "'$tool' not found (run tools/renga-setup.sh)"
done

mkdir -p "$src"

# Shallow fetch of one ref; reuses an existing checkout directory.
function fetchRef {
	local url="$1" ref="$2" dir="$3"
	if [ ! -d "$dir/.git" ]; then
		git init -q "$dir"
		git -C "$dir" remote add origin "$url"
	fi
	git -C "$dir" fetch -q --depth 1 origin "$ref"
	git -C "$dir" checkout -q --force FETCH_HEAD
}

toolsDir="$src/SPIRV-Tools"
fetchRef https://github.com/KhronosGroup/SPIRV-Tools.git "$tag" "$toolsDir"
headersRev="$(sed -n "s/.*'spirv_headers_revision': *'\([0-9a-f]*\)'.*/\1/p" "$toolsDir/DEPS")"
[ -n "$headersRev" ] || die "could not read spirv_headers_revision from SPIRV-Tools $tag DEPS"
# A separate checkout from the one build-clc-deps.sh uses for
# SPIRV-LLVM-Translator, which pins its own revision.
headersDir="$src/SPIRV-Headers-for-tools"
fetchRef https://github.com/KhronosGroup/SPIRV-Headers.git "$headersRev" "$headersDir"
echo "SPIRV-Tools $tag, SPIRV-Headers ${headersRev:0:12}"
echo "output: $prefix"

cmake -S "$toolsDir" -B "$work/build-SPIRV-Tools" -G Ninja \
	-DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_INSTALL_PREFIX="$prefix" \
	-DCMAKE_INSTALL_LIBDIR=lib \
	-DCMAKE_POSITION_INDEPENDENT_CODE=ON \
	-DBUILD_SHARED_LIBS=OFF \
	-DSPIRV-Headers_SOURCE_DIR="$headersDir" \
	-DSPIRV_SKIP_TESTS=ON \
	-DSPIRV_SKIP_EXECUTABLES=ON \
	-DSPIRV_WERROR=OFF
ninja -C "$work/build-SPIRV-Tools" -j"$jobs"
ninja -C "$work/build-SPIRV-Tools" install

export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
version="$(pkg-config --modversion SPIRV-Tools)" || die "SPIRV-Tools.pc was not installed"
pkg-config --exists 'SPIRV-Tools >= 2024.1' || die "SPIRV-Tools $version is older than Mesa needs (2024.1)"
echo
echo "Done: SPIRV-Tools $version. Build.sh finds it in $prefix automatically."
