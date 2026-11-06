#!/bin/bash
# Builds libglvnd (GL Vendor-Neutral Dispatch), which Zink, the OpenGL
# driver, is built against. HaikuPorts has a recipe but no x86_64 package.
#
# Uses the same release and Haiku patch as HaikuPorts'
# sys-libs/libglvnd/libglvnd-1.7.0.recipe. Results go to deps.<arch>/install,
# where Build.sh and tools/make-package.sh find them. Nothing is installed
# into the system.
#
# Prerequisites: meson, ninja, gcc, python3, git (all in HaikuPorts).
# Environment: DEPS_DIR, JOBS.
set -euo pipefail

version=1.7.0
# The NVIDIA GitHub repository mirrors freedesktop.org's; either works.
repos=(https://gitlab.freedesktop.org/glvnd/libglvnd.git https://github.com/NVIDIA/libglvnd.git)
commit=faa23f21fc677af5792825dc30cb1ccef4bf33a6   # tag v1.7.0

baseDir="$(cd "$(dirname "$0")/.." && pwd)"
arch="$(getarch 2>/dev/null || uname -m)"
work="${DEPS_DIR:-$baseDir/deps.$arch}"
prefix="$work/install"
srcDir="$work/src/libglvnd"
buildDir="$work/build-libglvnd"
jobs="${JOBS:-$(nproc 2>/dev/null || echo 2)}"
patchset="$baseDir/tools/patches/libglvnd-$version.patchset"

die() { echo "build-glvnd-deps: $*" >&2; exit 1; }
for tool in meson ninja git; do
	command -v "$tool" >/dev/null || die "'$tool' not found"
done
[ -f "$patchset" ] || die "missing $patchset"

if [ ! -d "$srcDir/.git" ]; then
	mkdir -p "$(dirname "$srcDir")"
	fetched=0
	for url in "${repos[@]}"; do
		if git clone -q --depth 1 -b "v$version" "$url" "$srcDir"; then fetched=1; break; fi
		rm -rf "$srcDir"
	done
	[ $fetched = 1 ] || die "couldn't download libglvnd $version"
fi
[ "$(git -C "$srcDir" rev-parse HEAD)" = "$commit" ] || die "$srcDir is not libglvnd v$version ($commit)"

# HaikuPorts' patch: guard BGLView against a missing EGL renderer.
if ! git -C "$srcDir" apply --reverse --check "$patchset" 2>/dev/null; then
	git -C "$srcDir" apply "$patchset"
fi

# Current Haiku's <pthread.h> spells the recursive mutex initializer only
# with _NP, which libglvnd's BGLView uses unsuffixed (as VideoStreamsWsi does;
# see Build.sh). Map it only when the compiler lacks the plain name.
hasPthreadMacro() {
	printf '#include <pthread.h>\n#ifndef %s\n#error missing\n#endif\n' "$1" |
		"${CXX:-c++}" -fsyntax-only -x c++ - >/dev/null 2>&1
}
extraArgs=()
if ! hasPthreadMacro PTHREAD_RECURSIVE_MUTEX_INITIALIZER &&
		hasPthreadMacro PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP; then
	extraArgs+=(-Dcpp_args=-DPTHREAD_RECURSIVE_MUTEX_INITIALIZER=PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP)
fi

reconfigure=()
[ -f "$buildDir/meson-private/coredata.dat" ] && reconfigure=(--reconfigure)
meson setup "${reconfigure[@]}" "$buildDir" "$srcDir" \
	--buildtype=release \
	--prefix="$prefix" --libdir=lib --includedir=include \
	"${extraArgs[@]+"${extraArgs[@]}"}" ${MESON_EXTRA_ARGS:-}
ninja -C "$buildDir" -j"$jobs"
ninja -C "$buildDir" install

export PKG_CONFIG_PATH="$prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
v="$(pkg-config --modversion libglvnd)" || die "libglvnd.pc not installed"
for lib in libEGL.so.1 libGL.so.1 libGLdispatch.so.0 libOpenGL.so.0; do
	[ -e "$prefix/lib/$lib" ] || die "missing $prefix/lib/$lib"
done
echo
echo "Done: libglvnd $v in $prefix"
echo "Build.sh now builds Zink as well."
