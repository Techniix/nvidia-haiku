#!/bin/sh
# Host test for RengaWsi: builds the layer and the test natively (Linux or
# Haiku) and runs it. Needs Vulkan headers: VULKAN_HEADERS, or the mesa-nvk
# submodule's include/.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."
inc="${VULKAN_HEADERS:-$root/mesa-nvk/include}"
out="${TMPDIR:-/tmp}/renga-wsi-layer-test"
${CXX:-c++} -std=c++20 -Wall -Wextra -Wno-missing-field-initializers -g ${SANITIZE:-} \
	-I"$inc" "$root/RengaWsi/RengaWsi.cpp" "$here/test.cpp" -o "$out"
"$out"
