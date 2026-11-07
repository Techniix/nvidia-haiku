#!/bin/sh
# Build and run the sync-pool host test against the patched mesa-nvk tree.
# Run Build.sh first (it applies tools/patches/mesa-nvk to mesa-nvk/).
set -e
here="$(cd "$(dirname "$0")" && pwd)"
base="$(dirname "$(dirname "$here")")"
mesa="${MESA_NVK:-$base/mesa-nvk}"
[ -d "$mesa/src/nouveau" ] || mesa="$base/source/mesa-nvk"
nvrm="$mesa/src/nouveau/vulkan/nvkmd/nvrm"
[ -f "$nvrm/nvkmd_nvrm_retire.h" ] || {
	echo "nvkmd_nvrm_retire.h not found in $nvrm: run Build.sh first (patch 0008)." >&2
	exit 2
}
work="$here/build.$(getarch 2>/dev/null || uname -m)"
mkdir -p "$work"

# The pool functions, verbatim: from the retire-kind enum to the
# end of nvkmd_nvrm_sync_pool_finish().
src="$nvrm/nvkmd_nvrm_sync.c"
start=$(grep -n "^/\* What a retire entry frees" "$src" | cut -d: -f1)
finish=$(grep -n "^nvkmd_nvrm_sync_pool_finish" "$src" | cut -d: -f1)
end=$(awk -v s="$finish" 'NR > s && /^}/ { print NR; exit }' "$src")
[ -n "$start" ] && [ -n "$end" ] || { echo "could not find the pool functions in $src" >&2; exit 2; }
sed -n "${start},${end}p" "$src" > "$work/pool_extract.c"

# sync_reset_data(), verbatim.
rstart=$(grep -n "^sync_reset_data(struct nvkmd_nvrm_sync \*sync)" "$src" | cut -d: -f1)
rend=$(awk -v s="$rstart" 'NR > s && /^}/ { print NR; exit }' "$src")
[ -n "$rstart" ] && [ -n "$rend" ] || { echo "could not find sync_reset_data in $src" >&2; exit 2; }
{ echo "static void"; sed -n "${rstart},${rend}p" "$src"; } > "$work/reset_extract.c"

${CC:-cc} -std=c11 -g -Wall -Wno-unused-function ${SANITIZE:-} \
	-I"$work" -I"$nvrm" -I"$mesa/src/util" -I"$mesa/src" \
	-o "$work/sync-pool-test" "$here/sync-pool-test.c" "$here/stubs.c" -lpthread 2>/dev/null ||
${CC:-cc} -std=c11 -g -Wall -Wno-unused-function ${SANITIZE:-} \
	-I"$work" -I"$nvrm" -I"$mesa/src/util" -I"$mesa/src" \
	-o "$work/sync-pool-test" "$here/sync-pool-test.c" "$here/stubs.c"
"$work/sync-pool-test"
