#!/bin/bash
# Uninstalls the quasar package.
#
#   tools/uninstall-package.sh                   uninstall quasar
#   tools/uninstall-package.sh --restore-nebula  also reinstall the nebula
#                                                package that quasar replaced
#   tools/uninstall-package.sh --yes             don't ask
#
# Files moved aside before the install can be put back separately with
# tools/uninstall.sh --restore.
set -euo pipefail

restoreNebula=0; assumeYes=0
for arg in "$@"; do
	case "$arg" in
		--restore-nebula) restoreNebula=1 ;;
		--yes|-y) assumeYes=1 ;;
		-h|--help) sed -n '2,10p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "unknown option: $arg (try --help)" >&2; exit 2 ;;
	esac
done

die() { echo "uninstall-package: $*" >&2; exit 1; }
userSettings="$(finddir B_USER_SETTINGS_DIRECTORY)"
state="$userSettings/nvidia-haiku"
userPackages="$(finddir B_USER_PACKAGES_DIRECTORY)"
systemPackages="$(finddir B_SYSTEM_PACKAGES_DIRECTORY)"

installed="$(ls "$userPackages"/quasar-*.hpkg "$systemPackages"/quasar-*.hpkg 2>/dev/null | head -1 || true)"
nebula="$(sed -n 's/^nebula=//p' "$state/package.info" 2>/dev/null || true)"
nebulaHome="$(sed -n 's/^nebula_home=//p' "$state/package.info" 2>/dev/null || echo 0)"

[ -n "$installed" ] || [ $restoreNebula = 1 ] || die "quasar is not installed"
if [ -n "$installed" ]; then echo "Will uninstall $(basename "$installed")."; fi
if [ $restoreNebula = 1 ]; then
	if [ -n "$nebula" ] && [ -f "$nebula" ]; then
		echo "Will reinstall $(basename "$nebula")."
	else
		die "no saved nebula package to restore (looked for it in $state/package.info)"
	fi
fi
if [ $assumeYes = 0 ]; then
	printf 'Continue? [y/N] '
	read -r answer
	case "$answer" in y|Y|yes|YES) ;; *) echo "Nothing changed."; exit 0 ;; esac
fi

if [ -n "$installed" ]; then
	case "$installed" in
		"$userPackages"/*) pkgman uninstall -H -y quasar ;;
		*) pkgman uninstall -y quasar ;;
	esac
fi
if [ $restoreNebula = 1 ]; then
	if [ "$nebulaHome" = 1 ]; then pkgman install -H -y "$nebula"; else pkgman install -y "$nebula"; fi
	rm -f "$state/package.info"
	rmdir "$state" 2>/dev/null || true
fi
echo "Done. Reboot to finish."
