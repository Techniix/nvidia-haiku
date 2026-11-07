#!/bin/sh
# Builds and runs the channel-report test against the patched mesa-nvk tree
# (mesa-nvk patch 0009). Set MESA_NVK to a tree with the patches applied;
# by default the one Build.sh prepares is used.
set -e
here="$(cd "$(dirname "$0")" && pwd)"
base="$(cd "$here/../.." && pwd)"
mesa="${MESA_NVK:-$base/mesa-nvk}"
[ -d "$mesa/src/nouveau" ] || mesa="$base/source/mesa-nvk"
src="$mesa/src/nouveau/vulkan/nvkmd/nvrm/nvkmd_nvrm_ctx.c"
if ! grep -q "nvkmd_nvrm_channel_report" "$src" 2>/dev/null; then
	echo "nvkmd_nvrm_channel_report not found in $src: run Build.sh first (patch 0009)." >&2
	exit 1
fi
out="$here/build.$(uname -m)"
mkdir -p "$out"
awk '/^\/\* RM.s robust-channel error status/{p=1} p{print} /^nvkmd_nvrm_channel_report\(/{r=1} r&&/^}/{exit}' \
	"$src" > "$out/extracted.c"
san=""
[ "$(uname -s)" = Haiku ] || san="-fsanitize=address,undefined"
lib=""
[ "$(uname -s)" = Haiku ] || lib="-lpthread"
${CC:-gcc} -std=gnu11 -Wall -Wextra -Wno-unused-parameter $san \
	-DEXTRACTED="\"$out/extracted.c\"" -o "$out/chan-report-test" \
	"$here/chan-report-test.c" $lib
"$out/chan-report-test"
