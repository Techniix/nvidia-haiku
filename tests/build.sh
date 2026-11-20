#!/bin/sh
# Build the regression tests in tests/ into tests/build.<arch>/.
#
#   tests/build.sh          build both tests
#   tests/build.sh run      build, then run both (exit status 0 only if all pass)
#
# vk-binary-wait-reuse needs the Vulkan headers and loader. The headers come
# from the mesa-nvk submodule (VULKAN_HEADERS overrides), the ones NVK is
# built with; the loader is the vulkan_loader package. egl-surfaceless-threads needs EGL headers and libEGL; the
# libglvnd that tools/build-glvnd-deps.sh builds is used when present, and the
# system one otherwise.
set -e

testsDir="$(cd "$(dirname "$0")" && pwd)"
baseDir="$(dirname "$testsDir")"
arch="$(getarch 2>/dev/null || uname -m)"
out="$testsDir/build.$arch"
glvnd="${DEPS_DIR:-$baseDir/deps.$arch}/install"
mkdir -p "$out"

CC="${CC:-cc}"
CFLAGS="${CFLAGS:--O2 -g -Wall}"

# Haiku has pthreads in libroot; elsewhere link libpthread.
threadLib="-lpthread"
[ "$(uname -s)" = Haiku ] && threadLib=""

vkInclude="${VULKAN_HEADERS:-$baseDir/mesa-nvk/include}"

# Renga's vulkan_loader package installs only the runtime library
# (lib/libvulkan.so.1), without the unversioned libvulkan.so development
# link that -lvulkan looks for. Link the runtime library by path when the
# development link is missing; its soname is libvulkan.so.1 either way.
vkLib="-lvulkan"
if [ "$(uname -s)" = Haiku ] && [ ! -e /boot/system/develop/lib/libvulkan.so ]; then
	for f in /boot/system/lib/libvulkan.so.1 \
			/boot/home/config/non-packaged/lib/libvulkan.so.1; do
		if [ -e "$f" ]; then vkLib="$f"; break; fi
	done
fi

eglFlags=""
if [ -d "$glvnd/include/EGL" ]; then
	eglFlags="-I$glvnd/include -L$glvnd/lib"
fi

echo "Building vk-binary-wait-reuse"
# shellcheck disable=SC2086
$CC $CFLAGS -I"$vkInclude" -o "$out/vk-binary-wait-reuse" \
	"$testsDir/vk-binary-wait-reuse.c" $vkLib $threadLib

echo "Building egl-surfaceless-threads"
# shellcheck disable=SC2086
$CC $CFLAGS $eglFlags -o "$out/egl-surfaceless-threads" \
	"$testsDir/egl-surfaceless-threads.c" -lEGL $threadLib

echo "Built into $out"

[ "$1" = run ] || exit 0

status=0
run() {
	echo
	echo "== $*"
	if "$@"; then :; else
		echo "== FAILED (exit $?): $*"
		status=1
	fi
}
run "$out/vk-binary-wait-reuse"
run "$out/vk-binary-wait-reuse" --no-thread
run "$out/egl-surfaceless-threads" --seconds 60
run "$out/egl-surfaceless-threads" --seconds 20 --fps 30
run "$out/egl-surfaceless-threads" --seconds 20 --pbuffer
echo
if [ $status -eq 0 ]; then echo "All tests passed."; else echo "Some tests FAILED."; fi
exit $status
