#!/bin/bash
# Builds quasar-<version>-x86_64.hpkg, a Haiku package with everything the
# NVIDIA driver needs:
#
#   kernel driver and NVKMS module     add-ons/kernel/drivers
#   accelerant                         add-ons/accelerants
#   NVK Vulkan driver                  add-ons/vulkan/icd.d
#   Vulkan window-system layer         add-ons/vulkan/implicit_layer.d
#   Zink OpenGL driver (if built)      add-ons/opengl/egl_vendor.d, lib/
#   libglvnd OpenGL dispatch (if built) lib/
#   GSP firmware                       data/firmware/nvidia/<version>
#
#   tools/make-package.sh [--revision N] [--output DIR]
#
# Inputs: install.<arch> from Build.sh, firmware/<version>, and libglvnd from
# deps.<arch>/install (tools/build-glvnd-deps.sh) when Zink was built.
# Debug information is stripped. The package conflicts with "nebula", the
# original Nebula package, so the two are never active together; install it
# with tools/install-package.sh, which handles that.
#
# Environment: NVIDIA_HAIKU_SRC (default install.<arch>), DEPS_DIR,
# PACKAGE_TOOL (default "package"), STRIP (default "strip").
set -euo pipefail

revision=1
outDir=""
while [ $# -gt 0 ]; do
	case "$1" in
		--revision) revision="$2"; shift 2 ;;
		--output) outDir="$2"; shift 2 ;;
		-h|--help) sed -n '2,23p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "unknown option: $1 (try --help)" >&2; exit 2 ;;
	esac
done

here="$(cd "$(dirname "$0")/.." && pwd)"
arch="$(getarch 2>/dev/null || uname -m)"
src="${NVIDIA_HAIKU_SRC:-$here/install.$arch}"
deps="${DEPS_DIR:-$here/deps.$arch}/install"
outDir="${outDir:-$here}"
packageTool="${PACKAGE_TOOL:-package}"
strip="${STRIP:-strip}"
name=quasar

die() { echo "make-package: $*" >&2; exit 1; }
note() { echo "  $*"; }

if [ -f "$here/VERSION" ]; then
	version="$(tr -d '[:space:]' < "$here/VERSION")"
else
	version="$(sed -n 's/^NVIDIA_VERSION *= *//p' "$here/open-gpu-kernel-modules/version.mk" 2>/dev/null | tr -d '[:space:]')"
fi
[ -n "$version" ] || die "can't tell the NVIDIA version"
fw="$here/firmware/$version"
for f in gsp_ga10x.bin gsp_tu10x.bin; do [ -s "$fw/$f" ] || die "missing $fw/$f"; done
[ -d "$src" ] || die "no build at $src (run Build.sh)"
command -v "$packageTool" >/dev/null || die "'$packageTool' not found (it is part of Haiku)"

stage="$(mktemp -d "${TMPDIR:-/tmp}/quasar-stage.XXXX")"
trap 'rm -rf "$stage"' EXIT
put() {   # put <source> <destination relative to the package root>
	[ -e "$1" ] || die "missing $1"
	mkdir -p "$stage/$(dirname "$2")"
	cp -P "$1" "$stage/$2"
}
link() {  # link <target> <link path relative to the package root>
	mkdir -p "$stage/$(dirname "$2")"
	ln -s "$1" "$stage/$2"
}

echo "Packaging $name $version-$revision from $src"

# Kernel side and accelerant.
put "$src/add-ons/kernel/drivers/bin/nvidia_gsp" add-ons/kernel/drivers/bin/nvidia_gsp
put "$src/add-ons/kernel/drivers/bin/nvidia_gsp_modeset" add-ons/kernel/drivers/bin/nvidia_gsp_modeset
link ../../bin/nvidia_gsp add-ons/kernel/drivers/dev/graphics/nvidia_gsp
link ../bin/nvidia_gsp_modeset add-ons/kernel/drivers/dev/nvidia_gsp_modeset
put "$src/add-ons/accelerants/nvidia_gsp.accelerant" add-ons/accelerants/nvidia_gsp.accelerant
for f in gsp_ga10x.bin gsp_tu10x.bin; do put "$fw/$f" "data/firmware/nvidia/$version/$f"; done
note "kernel driver, accelerant, firmware $version"

# NVK, next to its manifest as Haiku's own Vulkan drivers are, so the
# manifest needs no absolute path.
put "$src/lib/libvulkan_nouveau.so" add-ons/vulkan/icd.d/libvulkan_nouveau.so
icd="$(ls "$src"/data/vulkan/icd.d/nouveau_icd.*.json "$src"/share/vulkan/icd.d/nouveau_icd.*.json 2>/dev/null | head -1 || true)"
[ -n "$icd" ] || die "NVK manifest (nouveau_icd.*.json) not found in $src"
mkdir -p "$stage/add-ons/vulkan/icd.d"
sed -E 's#"library_path": *"[^"]*"#"library_path": "./libvulkan_nouveau.so"#' "$icd" \
	> "$stage/add-ons/vulkan/icd.d/$(basename "$icd")"
note "NVK"

for f in VideoStreamsWsi.so VideoStreamsWsi.json; do
	put "$src/add-ons/vulkan/implicit_layer.d/$f" "add-ons/vulkan/implicit_layer.d/$f"
done
note "Vulkan window-system layer"

if [ -e "$src/apps/Nebula/ModesetTool" ]; then
	put "$src/apps/Nebula/ModesetTool" apps/Quasar/ModesetTool
	link ../../../../../apps/Quasar/ModesetTool data/deskbar/menu/Applications/Quasar/ModesetTool
fi

# Zink: the EGL vendor library goes where libglvnd looks for vendors on
# Haiku (add-ons/opengl/egl_vendor.d, home folders first); the Mesa
# libraries it links against go in lib/.
withZink=0
eglVendor="$(ls "$src"/lib/libEGL_mesa.so.0.* 2>/dev/null | head -1 || true)"
if [ -n "$eglVendor" ]; then
	withZink=1
	put "$eglVendor" "add-ons/opengl/egl_vendor.d/$(basename "$eglVendor")"
	link "$(basename "$eglVendor")" add-ons/opengl/egl_vendor.d/libEGL_mesa.so.0
	cat > "$stage/add-ons/opengl/egl_vendor.d/50_mesa.json" <<-EOF
		{
		    "file_format_version" : "1.0.0",
		    "ICD" : {
		        "library_path" : "./libEGL_mesa.so.0"
		    }
		}
	EOF
	# Mesa's shared libraries other than the two drivers placed above.
	for f in "$src"/lib/lib*.so*; do
		b="$(basename "$f")"
		case "$b" in libvulkan_nouveau.so|libEGL_mesa.so*) continue ;; esac
		put "$f" "lib/$b"
	done
	note "Zink (OpenGL)"
	[ -e "$deps/lib/libGL.so.1" ] || die "Zink is built but libglvnd isn't in $deps; run tools/build-glvnd-deps.sh"
fi
if [ $withZink = 1 ]; then
	for f in "$deps"/lib/lib{GL,EGL,GLdispatch,OpenGL,GLESv1_CM,GLESv2}.so*; do
		[ -e "$f" ] && put "$f" "lib/$(basename "$f")"
	done
	note "libglvnd (OpenGL dispatch)"
fi

# Strip debug information from every Haiku ELF file (not symbols, which the
# kernel and dynamic loader need). The GSP firmware is ELF too, but for the
# GPU's own processor, and must stay byte-for-byte as NVIDIA ships it.
stripped=0
while IFS= read -r -d '' f; do
	if head -c 4 "$f" | grep -q $'\x7fELF'; then
		"$strip" --strip-debug "$f"
		stripped=$((stripped + 1))
	fi
done < <(find "$stage" -path "$stage/data/firmware" -prune -o -type f -print0)
note "stripped debug info from $stripped files"

# Each library the package needs must come from the package or from one of
# the packages listed in requires.
requires=(
	"haiku"
	"lib:libz"
	"lib:libzstd"
	"lib:libwayland_client"
	"lib:libSPIRV_Tools"
	"lib:libexpat"
)
provides=("$name = $version")
if [ $withZink = 1 ]; then
	provides+=("lib:libGL = 1.0.0 compat >= 1" "lib:libEGL = 1.1.0 compat >= 1" "lib:libGLdispatch" "lib:libOpenGL")
fi

copyright="2024-2026 X547 (nvidia-haiku)"
cat > "$stage/.PackageInfo" <<-EOF
	name			$name
	version			$version-$revision
	architecture	$arch
	summary			"NVIDIA GPU driver for Haiku: kernel driver, NVK Vulkan$([ $withZink = 1 ] && echo " and Zink OpenGL")"
	description		"Hardware-accelerated graphics for NVIDIA GPUs with GSP firmware (Turing and newer), built on NVIDIA's open GPU kernel modules $version and X547's nvidia-haiku (Nebula). Contains the nvidia_gsp kernel driver and accelerant, Mesa's NVK Vulkan driver$([ $withZink = 1 ] && echo ", Zink for OpenGL with libglvnd") and NVIDIA's GSP firmware. Tested on a GeForce RTX 5070."
	packager		"Techniix <Techniix@users.noreply.github.com>"
	vendor			"Techniix"
	copyrights		{
		"$copyright"
		"NVIDIA Corporation"
		"The Mesa contributors"
	}
	licenses		{
		"MIT"
		"GNU GPL v2"
	}
	urls			{
		"https://github.com/Techniix/nvidia-haiku"
	}
	provides		{
$(for p in "${provides[@]}"; do printf '\t\t%s\n' "$p"; done)
	}
	requires		{
$(for r in "${requires[@]}"; do printf '\t\t%s\n' "$r"; done)
	}
	conflicts		{
		nebula
	}
EOF

pkg="$outDir/$name-$version-$revision-$arch.hpkg"
rm -f "$pkg"
(cd "$stage" && "$packageTool" create -q "$pkg")
echo
echo "Created $pkg ($(du -h "$pkg" | cut -f1))"
echo "Install it with tools/install-package.sh $pkg"
