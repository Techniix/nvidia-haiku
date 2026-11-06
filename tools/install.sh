#!/bin/bash
# nvidia-haiku installer.
#
#   tools/install.sh --check    report what would change; changes nothing
#   tools/install.sh            install (asks before changing anything)
#   tools/install.sh --yes      install without asking
#
# What it does:
#   1. Checks the system: Haiku x86_64, an NVIDIA GPU, the files to install,
#      and the libraries they need (missing ones can be fetched with pkgman).
#   2. Finds earlier copies of this driver in the non-packaged folders: a
#      previous install, a test install, or files copied by hand. It moves
#      them into a dated backup folder in your home folder; nothing is
#      deleted.
#   3. Installs the kernel driver, accelerant, NVK Vulkan driver, Vulkan
#      window-system layer and GSP firmware into ~/config/non-packaged and
#      records every file in a manifest.
#   4. If another NVK is installed by a package (for example "nebula"), makes
#      all applications use this one: it sets VK_DRIVER_FILES in
#      ~/config/settings/boot/UserSetupEnvironment, which Haiku applies at
#      boot. Packages and system settings are never modified.
#
# tools/uninstall.sh reverses all of it.
set -euo pipefail

mode=install
assumeYes=0
for arg in "$@"; do
	case "$arg" in
		--check) mode=check ;;
		--yes|-y) assumeYes=1 ;;
		-h|--help) sed -n '2,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "unknown option: $arg (try --help)" >&2; exit 2 ;;
	esac
done

here="$(cd "$(dirname "$0")/.." && pwd)"

# ---- environment -----------------------------------------------------------

errors=0; warnings=0
ok()   { printf '  ok    %s\n' "$*"; }
info() { printf '        %s\n' "$*"; }
warn() { printf '  WARN  %s\n' "$*"; warnings=$((warnings + 1)); }
fail() { printf '  FAIL  %s\n' "$*"; errors=$((errors + 1)); }
section() { printf '\n%s\n' "$*"; }
die() { echo "install.sh: $*" >&2; exit 1; }

if [ "${NVIDIA_HAIKU_TEST:-0}" != 1 ] && [ "$(uname -s)" != Haiku ]; then
	die "this installer runs on Haiku"
fi
command -v finddir >/dev/null || die "finddir not found; this installer runs on Haiku"

arch="$(getarch 2>/dev/null || uname -m)"
userNP="$(finddir B_USER_NONPACKAGED_DIRECTORY)"
systemNP="$(finddir B_SYSTEM_NONPACKAGED_DIRECTORY)"
userSettings="$(finddir B_USER_SETTINGS_DIRECTORY)"
userHome="$(finddir B_USER_DIRECTORY)"
systemDir="$(finddir B_SYSTEM_DIRECTORY)"
packageDirs=("$(finddir B_SYSTEM_PACKAGES_DIRECTORY)" "$(finddir B_USER_PACKAGES_DIRECTORY)")

state="$userSettings/nvidia-haiku"
manifest="$state/manifest"
envFile="$userSettings/boot/UserSetupEnvironment"
envBegin="# >>> nvidia-haiku: managed by tools/install.sh, removed by tools/uninstall.sh >>>"
envEnd="# <<< nvidia-haiku <<<"
legacyManifest="$userNP/nvidia-rtx5070-test.manifest"

src="${NVIDIA_HAIKU_SRC:-$here/install.$arch}"
version=""
if [ -f "$here/VERSION" ]; then
	version="$(tr -d '[:space:]' < "$here/VERSION")"
elif [ -f "$here/open-gpu-kernel-modules/version.mk" ]; then
	version="$(sed -n 's/^NVIDIA_VERSION *= *//p' "$here/open-gpu-kernel-modules/version.mk" | tr -d '[:space:]')"
fi
fw="$here/firmware/$version"
icdName="nouveau_icd.$arch.json"
icdPath="$userNP/data/vulkan/icd.d/$icdName"

# Files from any build of this driver, relative to a non-packaged folder.
# Directories in this list are handled as one unit.
remnantPatterns=(
	add-ons/kernel/drivers/bin/nvidia_gsp
	add-ons/kernel/drivers/bin/nvidia_gsp_modeset
	add-ons/kernel/drivers/dev/graphics/nvidia_gsp
	add-ons/kernel/drivers/dev/nvidia_gsp_modeset
	add-ons/accelerants/nvidia_gsp.accelerant
	lib/libvulkan_nouveau.so
	'data/vulkan/icd.d/nouveau_icd.*.json'
	'add-ons/vulkan/icd.d/nouveau_icd.*.json'
	add-ons/vulkan/icd.d/libvulkan_nouveau.so
	add-ons/vulkan/implicit_layer.d/VideoStreamsWsi.so
	add-ons/vulkan/implicit_layer.d/VideoStreamsWsi.json
	'data/firmware/nvidia/*'
	apps/Nebula
	data/deskbar/menu/Applications/Nebula
	lib/pkgconfig/nvrm.pc
	lib/pkgconfig/nvkms.pc
)

# Libraries the payload links against beyond Haiku's base system, with the
# pkgman name that provides each.
neededLibs=(
	"libz.so.1 lib:libz"
	"libzstd.so.1 lib:libzstd"
	"libwayland-client.so.0 lib:libwayland_client"
	"libSPIRV-Tools.so lib:libSPIRV_Tools"
	"libexpat.so.1 lib:libexpat"
)
libDirs=(
	"$(finddir B_USER_NONPACKAGED_LIB_DIRECTORY 2>/dev/null || echo "$userNP/lib")"
	"$(finddir B_USER_LIB_DIRECTORY 2>/dev/null || true)"
	"$(finddir B_SYSTEM_NONPACKAGED_LIB_DIRECTORY 2>/dev/null || echo "$systemNP/lib")"
	"$(finddir B_SYSTEM_LIB_DIRECTORY 2>/dev/null || echo "$systemDir/lib")"
)

# ---- 1. system -------------------------------------------------------------

section "System"
if [ "$arch" = x86_64 ]; then ok "Haiku $arch"; else fail "architecture $arch: this build is for x86_64"; fi

gpu="$(listdev 2>/dev/null | awk '
	/^device /           { display = ($0 ~ /Display controller/) }
	/vendor 10de/        { nv = display }
	/^ *device [0-9a-f]+:/ { if (nv) { print $2; nv = 0 } }' | tr -d ':' | head -1)"
if [ -n "$gpu" ]; then
	ok "NVIDIA GPU found: 10de:$gpu"
else
	warn "no NVIDIA display controller in listdev; the driver will not bind"
fi

# ---- 2. files to install ---------------------------------------------------

section "Files to install"
[ -n "$version" ] || fail "can't tell the NVIDIA version (no VERSION file or open-gpu-kernel-modules/version.mk)"
if [ -d "$src" ]; then
	ok "build: $src"
	for f in add-ons/kernel/drivers/bin/nvidia_gsp add-ons/kernel/drivers/bin/nvidia_gsp_modeset \
			add-ons/accelerants/nvidia_gsp.accelerant lib/libvulkan_nouveau.so "data/vulkan/icd.d/$icdName"; do
		[ -e "$src/$f" ] || fail "missing from the build: $f"
	done
else
	fail "build not found: $src (run Build.sh first, or run this from an unpacked release)"
fi
if [ -n "$version" ] && [ -s "$fw/gsp_ga10x.bin" ] && [ -s "$fw/gsp_tu10x.bin" ]; then
	ok "GSP firmware $version"
else
	fail "GSP firmware not found in $fw"
fi
if [ -f "$here/SHA256SUMS" ]; then
	if (cd "$here" && sha256sum --quiet -c SHA256SUMS >/dev/null 2>&1); then
		ok "checksums match (SHA256SUMS)"
	else
		fail "checksums don't match: the download may be damaged (cd $here && sha256sum -c SHA256SUMS)"
	fi
fi

# ---- 3. libraries ----------------------------------------------------------

section "Libraries the driver needs"
missingPkgs=()
for entry in "${neededLibs[@]}"; do
	lib="${entry%% *}"; pkg="${entry##* }"
	found=""
	for d in "${libDirs[@]}"; do
		if [ -n "$d" ] && [ -e "$d/$lib" ]; then found="$d/$lib"; break; fi
	done
	if [ -n "$found" ]; then ok "$lib"; else warn "$lib missing (provided by pkgman package $pkg)"; missingPkgs+=("$pkg"); fi
done

# ---- 4. earlier copies -----------------------------------------------------

section "Earlier copies of this driver"
remnants=()   # absolute paths to move into the backup
declare -A seen=()
addRemnant() { if [ -z "${seen[$1]:-}" ]; then seen[$1]=1; remnants+=("$1"); fi; }

ownFiles=()   # files of a previous install by this script: replaced, not backed up
if [ -f "$manifest" ]; then
	info "previous install by this installer ($(wc -l < "$manifest") files) will be replaced"
	while read -r rel; do
		if [ -e "$userNP/$rel" ] || [ -L "$userNP/$rel" ]; then
			ownFiles+=("$userNP/$rel"); seen["$userNP/$rel"]=1
		fi
	done < "$manifest"
fi
if [ -f "$legacyManifest" ]; then
	info "test install from install-test.sh ($(wc -l < "$legacyManifest") files) will be removed"
	while read -r rel; do
		if [ -e "$userNP/$rel" ] || [ -L "$userNP/$rel" ]; then addRemnant "$userNP/$rel"; fi
	done < "$legacyManifest"
	addRemnant "$legacyManifest"
fi
shopt -s nullglob
for root in "$userNP" "$systemNP"; do
	for pattern in "${remnantPatterns[@]}"; do
		for path in $root/$pattern; do
			[ -e "$path" ] || [ -L "$path" ] || continue
			# A firmware version directory is one unit; skip it if it holds
			# only files of the previous install, which are replaced anyway.
			if [ -d "$path" ] && [ ! -L "$path" ]; then
				foreign="$(find "$path" \( -type f -o -type l \) | while read -r f; do [ -n "${seen[$f]:-}" ] || echo x; done | head -1)"
				[ -n "$foreign" ] || continue
			fi
			addRemnant "$path"
		done
	done
done
shopt -u nullglob
# Drop entries inside a directory that is itself in the list.
filtered=()
for p in "${remnants[@]}"; do
	keep=1
	for q in "${remnants[@]}"; do
		if [ "$p" != "$q" ] && [ -d "$q" ] && [ ! -L "$q" ]; then
			case "$p" in "$q"/*) keep=0 ;; esac
		fi
	done
	if [ $keep = 1 ]; then filtered+=("$p"); fi
done
remnants=("${filtered[@]+"${filtered[@]}"}")
if [ ${#remnants[@]} -eq 0 ]; then
	ok "none found"
else
	for p in "${remnants[@]}"; do info "will move to backup: $p"; done
	if [ ! -w "$systemNP" ]; then
		for p in "${remnants[@]}"; do
			case "$p" in "$systemNP"/*) fail "can't write $systemNP to move $p" ;; esac
		done
	fi
fi

# ---- 5. driver packages ----------------------------------------------------

section "Installed packages with NVIDIA driver files"
otherIcds=()
packagesFound=0
packageSettings="$systemDir/settings/packages"
for dir in "${packageDirs[@]}"; do
	for hpkg in "$dir"/*.hpkg; do
		[ -f "$hpkg" ] || continue
		contents="$(package list -p "$hpkg" 2>/dev/null || package list "$hpkg" 2>/dev/null || true)"
		echo "$contents" | grep -qE '(^|/| )(nvidia_gsp|libvulkan_nouveau\.so)($| )' || continue
		packagesFound=1
		base="$(basename "$hpkg")"; name="${base%%-*}"
		info "$name ($hpkg)"
		# What else it carries, beyond the parts this build replaces.
		extra="$(echo "$contents" | sed 's/^ *//' | grep -vE '^$|^\.PackageInfo|nvidia_gsp|libvulkan_nouveau|nouveau_icd|VideoStreamsWsi|Nebula|firmware/nvidia|^develop|pkgconfig|drirc' |
			awk -F/ 'NF > 1 || /\./' | head -12 || true)"
		if [ -n "$extra" ]; then
			info "  also provides (left in place; may include the OpenGL/Zink libraries):"
			echo "$extra" | sed 's/^/            /'
		fi
		# Its kernel driver must be blocked, or two nvidia_gsp drivers compete.
		if grep -qE "^[[:space:]]*Package[[:space:]]+$name[[:space:]]*\{" "$packageSettings" 2>/dev/null &&
				sed -n "/Package[[:space:]]*$name/,/^}/p" "$packageSettings" | grep -q 'drivers/bin/nvidia_gsp$'; then
			ok "  its kernel driver is disabled in $packageSettings"
		elif echo "$contents" | grep -q 'drivers/bin/nvidia_gsp'; then
			fail "  its kernel driver is active and would compete with this one; uninstall it (pkgman uninstall $name) or block its kernel driver (see README)"
		fi
	done
done
if [ $packagesFound = 0 ]; then ok "none"; fi

# Vulkan drivers outside our install that the loader would also offer.
for d in "$systemDir/add-ons/vulkan/icd.d" "$systemDir/data/vulkan/icd.d" \
		"$(finddir B_USER_CONFIG_DIRECTORY)/add-ons/vulkan/icd.d" "$(finddir B_USER_CONFIG_DIRECTORY)/data/vulkan/icd.d"; do
	for j in "$d"/nouveau_icd*.json; do
		if [ -e "$j" ]; then otherIcds+=("$j"); fi
	done
done
section "Vulkan driver selection"
if [ ${#otherIcds[@]} -gt 0 ]; then
	for j in "${otherIcds[@]}"; do info "another NVK: $j"; done
	info "will set VK_DRIVER_FILES in $envFile so every application uses this build"
else
	ok "no other NVK installed; nothing to set"
fi
if [ -f "$envFile" ] && grep -qF "$envBegin" "$envFile"; then
	info "an earlier nvidia-haiku block in $envFile will be replaced"
fi

# ---- summary ---------------------------------------------------------------

section "Summary"
echo "  install into: $userNP (NVIDIA $version)"
echo "  errors: $errors, warnings: $warnings"
if [ $errors -gt 0 ]; then
	echo "  Fix the FAIL items above first; nothing was changed."
	exit 1
fi
if [ "$mode" = check ]; then
	echo "  Check only: nothing was changed. Run tools/install.sh to install."
	exit 0
fi

if [ $assumeYes = 0 ]; then
	printf '\nInstall now? Earlier copies are moved to a backup, not deleted. [y/N] '
	read -r answer
	case "$answer" in y|Y|yes|YES) ;; *) echo "Nothing changed."; exit 0 ;; esac
fi

# ---- install ---------------------------------------------------------------

stamp="$(date +%Y%m%d-%H%M%S)"
mkdir -p "$userHome/nvidia-haiku-backups" "$state"
# A fresh, uniquely named folder: never shared with an earlier backup.
backup="$(mktemp -d "$userHome/nvidia-haiku-backups/$stamp.XXXX")"
backupList="$backup/restore.list"
: > "$backupList"

moveToBackup() {
	local path="$1"
	mkdir -p "$backup/files$(dirname "$path")"
	mv "$path" "$backup/files$path"
	echo "$path" >> "$backupList"
}

if [ ${#missingPkgs[@]} -gt 0 ]; then
	echo; echo "Installing missing libraries: ${missingPkgs[*]}"
	pkgman install -y "${missingPkgs[@]}" || die "pkgman failed; install ${missingPkgs[*]} and run again (nothing else was changed)"
fi

for p in "${ownFiles[@]+"${ownFiles[@]}"}"; do
	rm -f "$p"
done
for p in "${remnants[@]+"${remnants[@]}"}"; do
	if [ -e "$p" ] || [ -L "$p" ]; then moveToBackup "$p"; fi
done
rm -f "$manifest"

# Payload: runtime files only.
list="$(mktemp)"
(cd "$src" && find . -path ./develop -prune -o -path ./lib/pkgconfig -prune -o \( -type f -o -type l \) -print | sed 's#^\./##' | sort) > "$list"
for f in gsp_ga10x.bin gsp_tu10x.bin; do echo "data/firmware/nvidia/$version/$f" >> "$list"; done

: > "$manifest.new"
while read -r rel; do
	target="$userNP/$rel"
	if [ -e "$target" ] || [ -L "$target" ]; then
		moveToBackup "$target"
	fi
	mkdir -p "$(dirname "$target")"
	case "$rel" in
		data/firmware/nvidia/*) cp "$fw/$(basename "$rel")" "$target" ;;
		data/vulkan/icd.d/*.json)
			# Mesa writes an absolute library_path into the build folder; point
			# it at the installed copy, relative to this manifest.
			sed -E 's#"library_path": *"[^"]*/([^/"]+)"#"library_path": "../../../lib/\1"#' "$src/$rel" > "$target" ;;
		*) cp -P "$src/$rel" "$target" ;;
	esac
	echo "$rel" >> "$manifest.new"
done < "$list"
rm -f "$list"
mv "$manifest.new" "$manifest"

# Vulkan driver selection for all applications.
if [ -f "$envFile" ] && grep -qF "$envBegin" "$envFile"; then
	cp "$envFile" "$backup/UserSetupEnvironment"
	sed -i "\\|^$envBegin\$|,\\|^$envEnd\$|d" "$envFile"
fi
if [ ${#otherIcds[@]} -gt 0 ]; then
	mkdir -p "$(dirname "$envFile")"
	if [ -f "$envFile" ] && [ ! -f "$backup/UserSetupEnvironment" ]; then cp "$envFile" "$backup/UserSetupEnvironment"; fi
	{
		echo "$envBegin"
		echo "export VK_DRIVER_FILES=\"$icdPath\""
		echo "export VK_ICD_FILENAMES=\"$icdPath\""
		echo "$envEnd"
	} >> "$envFile"
fi

# Backups are restored newest first by tools/uninstall.sh --restore.
if [ -s "$backupList" ] || [ -f "$backup/UserSetupEnvironment" ]; then
	echo "$backup" >> "$state/backups"
else
	# Nothing was moved: remove this run's empty folder only.
	rm -f "$backupList"; rmdir "$backup" 2>/dev/null || true
	rmdir "$userHome/nvidia-haiku-backups" 2>/dev/null || true
fi
{
	echo "version=$version"
	echo "installed=$stamp"
	echo "envfile=$envFile"
} > "$state/install.info"

echo
echo "Installed $(wc -l < "$manifest") files (NVIDIA $version) into $userNP."
if [ -s "$backupList" ]; then echo "Moved $(wc -l < "$backupList") earlier files to $backup."; fi
if [ ${#ownFiles[@]} -gt 0 ]; then echo "Replaced the previous install (${#ownFiles[@]} files)."; fi
if [ ${#otherIcds[@]} -gt 0 ]; then echo "Applications will use this NVK after the reboot ($envFile)."; fi
echo
echo "Reboot to load the driver."
echo "If Haiku doesn't reach the desktop: hold Space at boot, choose"
echo "'Disable user add-ons', then run tools/uninstall.sh."
