#!/bin/bash
# Installs the quasar package and retires what it replaces.
#
#   tools/install-package.sh --check [quasar-*.hpkg]   report only
#   tools/install-package.sh [quasar-*.hpkg]           install (asks first)
#   tools/install-package.sh --yes [quasar-*.hpkg]     install without asking
#
# Without a file argument it uses the newest quasar-*.hpkg next to tools/.
#
# Steps:
#   1. Moves earlier non-packaged copies of the driver aside
#      (tools/install.sh --clean-only; restore with tools/uninstall.sh --restore).
#   2. Keeps a copy of the "nebula" package file, then uninstalls nebula,
#      which quasar replaces. If quasar has no OpenGL (Zink) but nebula does,
#      it stops unless --without-opengl is given, so games don't lose OpenGL.
#   3. Installs quasar into your home packages (pkgman install --home), which
#      also fetches any libraries it needs. If that fails, nebula is put back.
#
#   4. Keeps a copy of the quasar package it replaces (for example a build of
#      another branch), so tools/uninstall-package.sh --restore-previous can
#      put it back.
#   5. Warns about Vulkan drivers and window-system layers that would load
#      next to quasar's.
#
# Undo with tools/uninstall-package.sh --restore-previous (the quasar that
# was installed before) or --restore-nebula.
set -euo pipefail

mode=install; assumeYes=0; allowNoGL=0; hpkg=""
for arg in "$@"; do
	case "$arg" in
		--check) mode=check ;;
		--yes|-y) assumeYes=1 ;;
		--without-opengl) allowNoGL=1 ;;
		-h|--help) sed -n '2,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		-*) echo "unknown option: $arg (try --help)" >&2; exit 2 ;;
		*) hpkg="$arg" ;;
	esac
done

here="$(cd "$(dirname "$0")/.." && pwd)"
die() { echo "install-package: $*" >&2; exit 1; }
if [ "${NVIDIA_HAIKU_TEST:-0}" != 1 ] && [ "$(uname -s)" != Haiku ]; then die "this runs on Haiku"; fi

if [ -z "$hpkg" ]; then
	hpkg="$(ls -t "$here"/quasar-*.hpkg 2>/dev/null | head -1 || true)"
	[ -n "$hpkg" ] || die "no quasar-*.hpkg in $here; pass the package file"
fi
[ -f "$hpkg" ] || die "not found: $hpkg"
hpkg="$(cd "$(dirname "$hpkg")" && pwd)/$(basename "$hpkg")"

contents="$(package list -p "$hpkg" 2>/dev/null)" || die "$hpkg is not a valid package"
info="$(package list -i "$hpkg" 2>/dev/null || true)"
echo "$info" | grep -qE '^[[:space:]]*name: quasar$' || die "$hpkg is not a quasar package"
version="$(echo "$info" | sed -n 's/^[[:space:]]*version: //p' | head -1)"
quasarGL=0; echo "$contents" | grep -q 'add-ons/opengl/egl_vendor.d/50_mesa.json' && quasarGL=1

userSettings="$(finddir B_USER_SETTINGS_DIRECTORY)"
userHome="$(finddir B_USER_DIRECTORY)"
state="$userSettings/nvidia-haiku"
systemPackages="$(finddir B_SYSTEM_PACKAGES_DIRECTORY)"
userPackages="$(finddir B_USER_PACKAGES_DIRECTORY)"

nebula="$(ls "$systemPackages"/nebula-*.hpkg "$userPackages"/nebula-*.hpkg 2>/dev/null | head -1 || true)"
nebulaGL=0; nebulaHome=0
if [ -n "$nebula" ]; then
	package list -p "$nebula" 2>/dev/null | grep -q 'add-ons/opengl/egl_vendor.d' && nebulaGL=1
	case "$nebula" in "$userPackages"/*) nebulaHome=1 ;; esac
fi
installedQuasar="$(ls "$userPackages"/quasar-*.hpkg "$systemPackages"/quasar-*.hpkg 2>/dev/null | head -1 || true)"

echo "Package: $(basename "$hpkg") (quasar $version$([ $quasarGL = 1 ] && echo ", with OpenGL" || echo ", without OpenGL"))"
if [ -n "$installedQuasar" ]; then echo "Installed now: $(basename "$installedQuasar") (will be replaced)"; fi
if [ -n "$nebula" ]; then
	echo "Replaces: $(basename "$nebula")$([ $nebulaGL = 1 ] && echo ", which provides OpenGL")"
	echo "  A copy of it is kept so tools/uninstall-package.sh --restore-nebula can reinstall it."
fi
if [ $nebulaGL = 1 ] && [ $quasarGL = 0 ] && [ $allowNoGL = 0 ]; then
	echo
	echo "This quasar package has no OpenGL (Zink), but nebula does: OpenGL games would"
	echo "stop working. Build Zink first (tools/build-glvnd-deps.sh, Build.sh,"
	echo "tools/make-package.sh), or pass --without-opengl to go ahead anyway."
	exit 1
fi

echo
echo "Earlier non-packaged copies:"
if [ "$mode" = check ]; then
	"$here/tools/install.sh" --clean-only --check | sed -n '/^Earlier copies/,/^$/p'
	echo "Check only: nothing was changed. Run tools/install-package.sh to install."
	exit 0
fi

if [ $assumeYes = 0 ]; then
	printf 'Install quasar now? [y/N] '
	read -r answer
	case "$answer" in y|Y|yes|YES) ;; *) echo "Nothing changed."; exit 0 ;; esac
fi

# 1. Non-packaged copies.
"$here/tools/install.sh" --clean-only --yes | sed -n '/Moved\|Removed\|Clean-up/p'

# 2. nebula.
mkdir -p "$state"
if [ -n "$nebula" ]; then
	keep="$userHome/nvidia-haiku-backups/packages"
	mkdir -p "$keep"
	cp "$nebula" "$keep/"
	{ echo "nebula=$keep/$(basename "$nebula")"; echo "nebula_home=$nebulaHome"; } > "$state/package.info"
	echo "Kept a copy of $(basename "$nebula") in $keep."
	if [ $nebulaHome = 1 ]; then pkgman uninstall -H -y nebula; else pkgman uninstall -y nebula; fi
fi

# 3. quasar. The one installed now is kept first, unless it is this very
# file: going back to a known-good build must always be one command.
if [ -n "$installedQuasar" ] && ! cmp -s "$installedQuasar" "$hpkg"; then
	keepPrev="$userHome/nvidia-haiku-backups/quasar-previous"
	mkdir -p "$keepPrev"
	rm -f "$keepPrev"/quasar-*.hpkg
	cp "$installedQuasar" "$keepPrev/"
	case "$installedQuasar" in "$userPackages"/*) prevHome=1 ;; *) prevHome=0 ;; esac
	{ echo "previous=$keepPrev/$(basename "$installedQuasar")"; echo "previous_home=$prevHome"; } \
		> "$state/previous-quasar.info"
	echo "Kept a copy of $(basename "$installedQuasar") in $keepPrev."
	echo "  To go back to it: tools/uninstall-package.sh --restore-previous"
fi
if [ -n "$installedQuasar" ]; then
	case "$installedQuasar" in
		"$userPackages"/*) pkgman uninstall -H -y quasar ;;
		*) pkgman uninstall -y quasar ;;
	esac
fi
if ! pkgman install -H -y "$hpkg"; then
	echo "install-package: pkgman could not install quasar." >&2
	if [ -n "$nebula" ]; then
		echo "Reinstalling nebula..." >&2
		if [ $nebulaHome = 1 ]; then pkgman install -H -y "$keep/$(basename "$nebula")"; else pkgman install -y "$keep/$(basename "$nebula")"; fi
	fi
	exit 1
fi

# 4. Check that the Vulkan loader will see exactly one NVK: this package's.
# Anything else (a non-packaged copy, a system-wide one, or VK_DRIVER_FILES /
# VK_ICD_FILENAMES pointing somewhere) would let apps load two drivers for
# the same GPU, or the wrong one.
echo
echo "Checking for other copies of NVK..."
configDir="$(finddir B_USER_CONFIG_DIRECTORY)"
ours="$configDir/add-ons/vulkan/icd.d"
strays=()
for base in "$(finddir B_SYSTEM_DIRECTORY)" "$(finddir B_SYSTEM_NONPACKAGED_DIRECTORY)" \
		"$configDir" "$(finddir B_USER_NONPACKAGED_DIRECTORY)"; do
	for f in "$base"/add-ons/vulkan/icd.d/nouveau_icd*.json "$base"/data/vulkan/icd.d/nouveau_icd*.json \
			"$base"/add-ons/vulkan/icd.d/libvulkan_nouveau.so* "$base"/lib/libvulkan_nouveau.so*; do
		[ -e "$f" ] || continue
		case "$f" in "$ours"/*) continue ;; esac
		strays+=("$f")
	done
done
envFile="$(finddir B_USER_SETTINGS_DIRECTORY)/boot/UserSetupEnvironment"
envHits="$(grep -nE 'VK_(DRIVER_FILES|ICD_FILENAMES|ADD_DRIVER_FILES)' "$envFile" 2>/dev/null || true)"
if [ ${#strays[@]} -eq 0 ] && [ -z "$envHits" ]; then
	echo "  only quasar's NVK is installed."
else
	for f in "${strays[@]}"; do echo "  WARNING: another NVK file: $f"; done
	if [ -n "$envHits" ]; then
		echo "  WARNING: $envFile selects Vulkan drivers:"
		echo "$envHits" | sed 's/^/    /'
	fi
	echo "  Apps may load two NVKs, or the wrong one. Move these aside (or run"
	echo "  tools/install.sh --clean-only), then reboot."
fi
# Window-system layers: exactly one of RengaWsi or VideoStreamsWsi may be
# active, and it must be the package's. A layer left elsewhere (an earlier
# manual install, a nebula remnant) would claim vkCreateHeadlessSurfaceEXT
# too.
echo
echo "Checking Vulkan window-system layers..."
ourLayers="$configDir/add-ons/vulkan/implicit_layer.d"
layerStrays=()
for base in "$(finddir B_SYSTEM_DIRECTORY)" "$(finddir B_SYSTEM_NONPACKAGED_DIRECTORY)" \
		"$configDir" "$(finddir B_USER_NONPACKAGED_DIRECTORY)"; do
	for f in "$base"/add-ons/vulkan/implicit_layer.d/VideoStreamsWsi* \
			"$base"/add-ons/vulkan/implicit_layer.d/RengaWsi*; do
		[ -e "$f" ] || continue
		case "$f" in "$ourLayers"/*) continue ;; esac
		layerStrays+=("$f")
	done
done
# (|| true: with set -o pipefail, ls failing on an absent file would end the
# script here.)
ourCount="$( (ls "$ourLayers"/RengaWsi.json "$ourLayers"/VideoStreamsWsi.json 2>/dev/null || true) | wc -l)"
if [ ${#layerStrays[@]} -eq 0 ] && [ "$ourCount" -le 1 ]; then
	echo "  one layer, from quasar."
else
	for f in "${layerStrays[@]}"; do echo "  WARNING: another window-system layer: $f"; done
	if [ "$ourCount" -gt 1 ]; then
		echo "  WARNING: both RengaWsi and VideoStreamsWsi are in $ourLayers"
	fi
	echo "  Two layers would both handle Vulkan surfaces. Move the extra files"
	echo "  aside, then reboot."
fi

for v in VK_DRIVER_FILES VK_ICD_FILENAMES VK_ADD_DRIVER_FILES; do
	if [ -n "${!v:-}" ]; then
		echo "  Note: $v is set in this shell (${!v}); unset it before testing."
	fi
done

echo
echo "quasar $version is installed in your home packages."
echo "Reboot to load the driver."
echo "If the system doesn't reach the desktop: hold Space at boot, choose"
echo "'Disable user add-ons', then run tools/uninstall-package.sh --restore-previous"
echo "(or --restore-nebula if quasar was not installed before)."
