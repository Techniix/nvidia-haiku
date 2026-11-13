#!/bin/bash
# Syntax-check the Haiku/Renga parts of the Mesa tree (mesa-nvk) on a machine
# that is not Haiku, so that compile errors show up before a test build on
# the hardware.
#
# What it checks: NVK's RM back end (src/nouveau/vulkan/nvkmd/nvrm), the
# Renga WSI (wsi_common_renga.cpp, wsi_common_headless.c), the rest of NVK's
# C sources and the shared files the Haiku port changes, compiled with
# clang -fsyntax-only for x86_64-unknown-haiku against a Haiku or Renga
# tree's headers, the NVIDIA SDK headers from open-gpu-kernel-modules, and
# the headers Mesa generates at build time. Flags follow the meson build
# (nvk_flags: -DNVTYPES_USE_STDINT=1; __HAIKU__ for the WSI).
#
# Like run.sh next to it, a clean run means "compiles with clang for the
# Haiku target", not "builds": Haiku builds Mesa with GCC, so GCC-only
# diagnostics, link errors and anything at run time are not covered. It
# does catch the usual causes of a failed build on the test machine: a typo,
# a missing include, a wrong type or signature, code in a Haiku-only branch
# that never compiled.
#
# Mesa's generated headers are made here with Mesa's own generators, the
# way meson runs them: the NVIDIA class headers (class_parser.py), the
# Vulkan runtime and util headers, the format enums, and nil.h (cbindgen,
# from Rust). They go in tools/crosscheck/.mesa-gen and are made again when
# the Mesa tree's HEAD or this script changes.
#
# Needs: clang, python3 with mako and yaml, cbindgen (cargo install
# cbindgen). Without cbindgen, files that include NVK's device headers are
# skipped, and said so.
#
# Usage: HAIKU_TREE=/path/to/haiku tools/crosscheck/mesa.sh [file...]
#   HAIKU_TREE  a checkout of Techniix/haiku (ce/main) or haiku/haiku
#   MESA_TREE   the Mesa tree, default: the mesa-nvk submodule
#   With no files, checks every .c/.cpp in the nvrm back end and the two
#   WSI files. Exit status: 0 when every file compiles.
set -u

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../.." && pwd)"
H="${HAIKU_TREE:?set HAIKU_TREE to a Haiku or Renga source tree}/headers"
M="$(cd "${MESA_TREE:-$root/mesa-nvk}" && pwd)"
nv="$root/open-gpu-kernel-modules"
gen="$here/.mesa-gen"
[ -d "$nv/src" ] || { echo "open-gpu-kernel-modules is not checked out" >&2; exit 2; }
[ -f "$M/src/nouveau/vulkan/nvkmd/nvrm/nvkmd_nvrm.h" ] \
	|| { echo "$M is not a Mesa tree with the nvrm back end (set MESA_TREE)" >&2; exit 2; }
python3 -c "import mako, yaml" 2>/dev/null \
	|| { echo "python3 needs mako and yaml (pip install mako pyyaml)" >&2; exit 2; }

# --- Mesa's generated headers -------------------------------------------

generate() {
	local X="$M/src/vulkan/registry/vk.xml" U="$M/src/vulkan/util"
	local R="$M/src/vulkan/runtime" C="$M/src/nouveau/headers"
	rm -rf "$gen" && mkdir -p "$gen/util/format" || return 1
	(
	set -e
	cd "$M"
	# NVIDIA class push headers, as src/nouveau/headers/meson.build makes
	# them: for each class in nv_classes, the previous class is the last
	# one before it in the list whose name ends in the same two letters.
	python3 - "$C" "$gen" <<'PYEOF'
import re, subprocess, sys
headers, out = sys.argv[1], sys.argv[2]
meson = open(headers + "/meson.build").read()
block = re.search(r"nv_classes\s*=\s*\[(.*?)\]", meson, re.S).group(1)
classes = re.findall(r"'(cl[0-9a-f]+)'", block)
for i, cls in enumerate(classes):
    cmd = ["python3", headers + "/class_parser.py",
           "--in-h", f"{headers}/nvidia/classes/{cls}.h"]
    prev = [c for c in classes[:i] if c[-2:] == cls[-2:]]
    if prev:
        cmd += ["--prev-in-h", f"{headers}/nvidia/classes/{prev[-1]}.h"]
    cmd += ["--out-h", f"{out}/nv_push_{cls}.h",
            "--out-c", f"{out}/nv_push_{cls}.c"]
    subprocess.run(cmd, check=True)
PYEOF
	mkdir -p $gen/compiler/spirv
	python3 src/compiler/spirv/spirv_info_gen.py \
		--json src/compiler/spirv/spirv.core.grammar.json \
		--out-h $gen/compiler/spirv/spirv_info.h \
		--out-c $gen/compiler/spirv/spirv_info.c
	python3 $U/vk_dispatch_table_gen.py --xml $X --beta false \
		--out-c $gen/vk_dispatch_table.c --out-h $gen/vk_dispatch_table.h
	python3 $U/gen_enum_to_str.py --xml $X --beta false --out-c $gen/vk_enum_to_str.c \
		--out-h $gen/vk_enum_to_str.h --out-d $gen/vk_enum_defines.h
	python3 $U/vk_struct_type_cast_gen.py --xml $X --beta false \
		--out $gen/vk_struct_type_cast.h
	python3 $U/vk_extensions_gen.py --xml $X \
		--out-c $gen/vk_extensions.c --out-h $gen/vk_extensions.h
	python3 $U/vk_entrypoints_gen.py --xml $X --proto --weak --beta false \
		--out-h $gen/vk_common_entrypoints.h --out-c $gen/vk_common_entrypoints.c \
		--prefix vk_common
	python3 $U/vk_entrypoints_gen.py --xml $X --proto --weak --beta false \
		--out-h $gen/wsi_common_entrypoints.h --out-c $gen/wsi_common_entrypoints.c \
		--prefix wsi
	python3 $U/vk_cmd_queue_gen.py --xml $X --beta false \
		--out-c $gen/vk_cmd_queue.c --out-h $gen/vk_cmd_queue.h
	python3 $U/vk_physical_device_features_gen.py --xml $X --beta false \
		--out-c $gen/vk_physical_device_features.c \
		--out-h $gen/vk_physical_device_features.h
	python3 $U/vk_physical_device_properties_gen.py --xml $X --beta false \
		--out-c $gen/vk_physical_device_properties.c \
		--out-h $gen/vk_physical_device_properties.h
	python3 $R/vk_format_info_gen.py --xml $X \
		--out-c $gen/vk_format_info.c --out-h $gen/vk_format_info.h
	python3 src/util/format/u_format_table.py src/util/format/u_format.yaml \
		--enums > $gen/util/format/u_format_gen.h
	python3 $U/vk_entrypoints_gen.py --xml $X --proto --weak --beta false \
		--out-h $gen/nvk_entrypoints.h --out-c $gen/nvk_entrypoints.c \
		--prefix nvk
	python3 bin/git_sha1_gen.py --output $gen/git_sha1.h
	python3 src/compiler/builtin_types_h.py $gen/builtin_types.h
	# NIR's opcode and intrinsic headers, which NVK's shader headers use.
	N=src/compiler/nir
	python3 $N/nir_opcodes_h.py > $gen/nir_opcodes.h
	python3 $N/nir_builder_opcodes_h.py > $gen/nir_builder_opcodes.h
	python3 $N/nir_intrinsics_h.py --out $gen/nir_intrinsics.h
	python3 $N/nir_intrinsics_indices_h.py --out $gen/nir_intrinsics_indices.h
	# NVK's driconf header (Mesa 26.3 and later).
	if [ -f src/nouveau/vulkan/nvk_drirc_gen.py ]; then
		python3 src/nouveau/vulkan/nvk_drirc_gen.py --import-path src/util \
			--drirc-src $gen/nvk_drirc.c --drirc-hdr $gen/nvk_drirc.h \
			--validate src/nouveau/vulkan/00-nvk-defaults.conf
	fi
	if command -v cbindgen >/dev/null; then
		cbindgen -q --config src/nouveau/nil/cbindgen.toml --lang c \
			--output $gen/nil.h -- src/nouveau/nil/lib.rs
		# NVK's own Rust (Mesa 26.3 and later: video decode).
		if [ -f src/nouveau/vulkan/rust/cbindgen.toml ]; then
			cbindgen -q --config src/nouveau/vulkan/rust/cbindgen.toml --lang c \
				--output $gen/nvk_rust.h -- src/nouveau/vulkan/rust/lib.rs
		fi
	fi
	) || { echo "generating Mesa's headers failed" >&2; rm -rf "$gen"; return 1; }
	stamp > "$gen/stamp"
}

# Regenerate when the Mesa tree or this script changes.
stamp() { echo "$(git -C "$M" rev-parse HEAD) $(sha1sum < "$0" | cut -c1-12)"; }

if [ "$(cat "$gen/stamp" 2>/dev/null)" != "$(stamp)" ]; then
	echo "crosscheck: generating Mesa headers for $(git -C "$M" rev-parse --short HEAD)"
	generate || exit 2
fi
have_nil=0
[ -f "$gen/nil.h" ] && have_nil=1

# --- compiler setup ------------------------------------------------------

res="$(clang -print-resource-dir)"
cxxdir="${CROSSCHECK_LIBSTDCXX:-$(ls -d /usr/include/c++/* | sort -V | tail -1)}"
cxxarch="${CROSSCHECK_LIBSTDCXX_ARCH:-/usr/include/x86_64-linux-gnu/c++/$(basename "$cxxdir")}"

sys=(-isystem "$res/include")
for d in "" app device drivers game interface kernel locale media net storage \
		support translation add-ons/graphics; do
	sys+=(-isystem "$H/os${d:+/$d}")
done
sys+=(-isystem "$H/compatibility/bsd" -isystem "$H/glibc"
	-isystem "$H/compatibility/gnu" -isystem "$H/posix" -isystem "$H")

# Defines meson's config gives a Haiku build that the headers depend on.
defs=(-D__HAIKU__ -DHAVE_PTHREAD -DHAVE_TIMESPEC_GET -DHAVE_STRUCT_TIMESPEC
	-DUTIL_ARCH_LITTLE_ENDIAN=1 -DUTIL_ARCH_BIG_ENDIAN=0
	-DNVTYPES_USE_STDINT=1 -DNVKMD_BACKEND_NVRM -DNVKMD_NVRM_VERSION='"crosscheck"'
	-DPACKAGE_VERSION="\"$(cat "$M/VERSION")\"")
# What meson finds GCC supports (meson.build: _attributes and the GCC
# builtins). Mesa's macros depend on them: without HAVE_FUNC_ATTRIBUTE_PACKED,
# ENUM_PACKED is empty and NVK's static size checks fail. __builtin_*_p
# overflow checks are left out, since clang lacks them.
for a in CONST FLATTEN MALLOC PURE UNUSED WARN_UNUSED_RESULT WEAK FORMAT \
		PACKED RETURNS_NONNULL ALIAS NORETURN OPTIMIZE COLD VISIBILITY; do
	defs+=(-DHAVE_FUNC_ATTRIBUTE_$a)
done
for b in BSWAP32 BSWAP64 CLZ CLZLL CTZ EXPECT FFS FFSLL POPCOUNT POPCOUNTLL \
		UNREACHABLE TYPES_COMPATIBLE_P; do
	defs+=(-DHAVE___BUILTIN_$b)
done
# meson defines every codec (-Dvideo-codecs); Quasar's build enables H.264
# decode, the one NVK supports.
for c in VC1DEC H264ENC H265DEC H265ENC AV1DEC AV1ENC VP9DEC MPEG12DEC JPEGDEC; do
	defs+=(-DVIDEO_CODEC_$c=0)
done
defs+=(-DVIDEO_CODEC_H264DEC=1)

inc=(-I"$M/include" -I"$M/src" -I"$gen" -I"$M/src/nouveau/headers"
	-I"$M/src/nouveau/headers/nvidia/classes" -I"$M/src/nouveau/winsys"
	-I"$M/src/nouveau/nil" -I"$M/src/nouveau/compiler" -I"$M/src/nouveau/mme"
	-I"$M/src/nouveau/vulkan"
	-I"$M/src/nouveau/vulkan/nvkmd/nvrm"
	-I"$nv/src/nvidia/arch/nvalloc/common/inc"
	-I"$nv/src/nvidia/arch/nvalloc/unix/include"
	-I"$nv/src/common/sdk/nvidia/inc" -I"$nv/src/nvidia/inc/kernel"
	-I"$M/src/vulkan/runtime" -I"$M/src/vulkan/util" -I"$M/src/vulkan/wsi"
	-I"$M/src/util" -I"$M/src/compiler" -I"$M/src/compiler/nir")

# Known noise, not from the Haiku code:
#  -Wno-return-type: nvk_mem_arena.h returns p_atomic_set() from a void
#    function; GCC accepts it, clang calls it an error in C.
#  -Wno-macro-redefined: Mesa's drf.h and NVIDIA's nvmisc.h both define
#    DRF_MASK; the nvrm back end includes both, as on Haiku.
common=(--target=x86_64-unknown-haiku -nostdinc -fsyntax-only -Wall
	-Wno-return-type -Wno-macro-redefined -Wno-unused-function
	-Wno-unused-variable -Wno-pointer-sign)

if [ $# -eq 0 ]; then
	# The RM back end, the Renga WSI, all of NVK (it is built on Haiku as a
	# whole, and the RM back end changes what it compiles against), and the
	# shared runtime and util files the Haiku port touches.
	set -- "$M"/src/nouveau/vulkan/nvkmd/nvrm/*.c \
		"$M/src/vulkan/wsi/wsi_common_renga.cpp" \
		"$M/src/vulkan/wsi/wsi_common_headless.c" \
		"$M/src/nouveau/vulkan/nvkmd/nvkmd.c"
	for f in "$M"/src/nouveau/vulkan/nvk_*.c; do
		set -- "$@" "$f"
	done
	set -- "$@" "$M/src/vulkan/runtime/vk_device.c" \
		"$M/src/vulkan/runtime/vk_image.c" "$M/src/util/os_misc.c" \
		"$M/src/util/u_process.c"
fi

fail=0
count=0
skipped=0
for f in "$@"; do
	case "$f" in
		*.c) cmd=(clang -std=gnu11) ;;
		*)   cmd=(clang++ -std=c++17 -nostdinc++ -isystem "$cxxdir"
		          -isystem "$cxxarch" -isystem "$here/shim"
		          "-D__GLIBC_PREREQ(a,b)=0" -D__locale_t=locale_t) ;;
	esac
	if [ $have_nil = 0 ] && grep -q '"nvk_' "$f"; then
		echo "--- skipped (needs nil.h, install cbindgen) ${f#$M/}"
		skipped=$((skipped + 1))
		continue
	fi
	# nvkcl.h comes from OpenCL C through mesa_clc and vtn_bindgen2, which
	# need LLVM and libclc; not made here.
	if grep -q '"nvkcl.h"' "$f"; then
		echo "--- skipped (needs nvkcl.h from mesa_clc) ${f#$M/}"
		skipped=$((skipped + 1))
		continue
	fi
	count=$((count + 1))
	if ! "${cmd[@]}" "${common[@]}" "${defs[@]}" "${inc[@]}" "${sys[@]}" "$f" \
			2>"$here/.last-mesa.log"; then
		fail=$((fail + 1))
		echo "=== FAILED ${f#$M/}"
		grep -E "error|fatal" "$here/.last-mesa.log" | head -${CROSSCHECK_MAX_ERRORS:-12}
	elif [ -n "${CROSSCHECK_WARNINGS:-}" ] && [ -s "$here/.last-mesa.log" ]; then
		echo "--- warnings ${f#$M/}"
		grep -A2 "warning:" "$here/.last-mesa.log" | head -30
	fi
done
echo "crosscheck mesa: $count files, $fail failed$([ $skipped -gt 0 ] && echo ", $skipped skipped")"
[ $fail -eq 0 ]
