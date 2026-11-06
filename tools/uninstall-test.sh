#!/bin/bash
# Removes exactly the files install-test.sh installed, using its manifest.
set -euo pipefail
DEST=$(finddir B_USER_NONPACKAGED_DIRECTORY)
MANIFEST="$DEST/nvidia-rtx5070-test.manifest"
if [ ! -f "$MANIFEST" ]; then
	echo "No test build installed (no $MANIFEST)." >&2
	exit 0
fi
while read -r rel; do
	if [ -e "$DEST/$rel" ] || [ -L "$DEST/$rel" ]; then
		rm -f "$DEST/$rel"
		echo "removed $rel"
	fi
done < "$MANIFEST"
rm -f "$MANIFEST"
echo "Test build removed. Reboot to return to the previous graphics driver."
