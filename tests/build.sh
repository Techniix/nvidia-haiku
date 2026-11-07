#!/bin/sh
# Build the regression tests in tests/ into tests/build.<arch>/.
#
#   tests/build.sh          build both tests
#   tests/build.sh run      build, then run both (exit status 0 only if all pass)
#
# vk-binary-wait-reuse needs the Vulkan headers and loader (vulkan_headers,
# vulkan_loader). egl-surfaceless-threads needs EGL headers and libEGL; the
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

eglFlags=""
if [ -d "$glvnd/include/EGL" ]; then
	eglFlags="-I$glvnd/include -L$glvnd/lib"
fi

echo "Building vk-binary-wait-reuse"
# shellcheck disable=SC2086
$CC $CFLAGS -o "$out/vk-binary-wait-reuse" "$testsDir/vk-binary-wait-reuse.c" -lvulkan $threadLib

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
