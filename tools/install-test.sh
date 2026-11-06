#!/bin/bash
# Installs a test build of the NVIDIA driver into the user's non-packaged
# folder (~/config/non-packaged) without touching the system or packages.
#
#   install-test.sh <meson install dir> <firmware dir>
#   e.g. install-test.sh ~/nvidia-haiku/install.x86_64 ~/nvidia-haiku/firmware/570.86.16
#
# - copies runtime files only (develop/ headers and static libs are skipped)
# - refuses to overwrite any existing file
# - copies the GSP firmware to data/firmware/nvidia/<version>/
# - makes Vulkan ICD library paths relative to the installed manifest
# - writes a manifest; uninstall-test.sh removes exactly those files
set -euo pipefail

if [ $# -ne 2 ]; then
	echo "usage: $0 <meson install dir> <firmware dir named after its version>" >&2
	exit 2
fi
SRC=$(cd "$1" && pwd)
FW=$(cd "$2" && pwd)
VERSION=$(basename "$FW")
DEST=$(finddir B_USER_NONPACKAGED_DIRECTORY)
MANIFEST="$DEST/nvidia-rtx5070-test.manifest"

if [ -e "$MANIFEST" ]; then
	echo "A test build is already installed ($MANIFEST). Run uninstall-test.sh first." >&2
	exit 1
fi
for f in gsp_ga10x.bin gsp_tu10x.bin; do
	if [ ! -f "$FW/$f" ]; then
		echo "missing $FW/$f" >&2
		exit 1
	fi
done

# Everything to copy, as paths relative to DEST.
LIST=$(mktemp)
(cd "$SRC" && find . -path ./develop -prune -o \( -type f -o -type l \) -print | sed 's#^\./##') > "$LIST"
for f in gsp_ga10x.bin gsp_tu10x.bin; do
	echo "data/firmware/nvidia/$VERSION/$f" >> "$LIST"
done

# Never overwrite something that is not ours.
conflicts=0
while read -r rel; do
	if [ -e "$DEST/$rel" ] || [ -L "$DEST/$rel" ]; then
		echo "exists, not overwriting: $DEST/$rel" >&2
		conflicts=1
	fi
done < "$LIST"
if [ $conflicts -ne 0 ]; then
	echo "Nothing installed. Move the files above aside (or run uninstall-test.sh) and retry." >&2
	rm -f "$LIST"
	exit 1
fi

: > "$MANIFEST"
while read -r rel; do
	mkdir -p "$DEST/$(dirname "$rel")"
	case $rel in
		data/firmware/nvidia/*) cp "$FW/$(basename "$rel")" "$DEST/$rel" ;;
		data/vulkan/icd.d/*.json)
			# Mesa writes an absolute library_path into the build's install
			# directory; point it at the installed copy, relative to the
			# manifest, so the install does not depend on the build tree.
			sed -E 's#"library_path": *"[^"]*/([^/"]+)"#"library_path": "../../../lib/\1"#' \
				"$SRC/$rel" > "$DEST/$rel" ;;
		*) cp -P "$SRC/$rel" "$DEST/$rel" ;;
	esac
	echo "$rel" >> "$MANIFEST"
	echo "installed $rel"
done < "$LIST"
rm -f "$LIST"

echo
echo "Installed $(wc -l < "$MANIFEST") files into $DEST (firmware $VERSION)."
echo "Reboot to load the driver. To undo: uninstall-test.sh, or hold Space at"
echo "boot and choose 'Disable user add-ons' in the Safe mode options."
