#!/bin/bash
# Removes what tools/install.sh installed.
#
#   tools/uninstall.sh            remove the driver and its Vulkan setting
#   tools/uninstall.sh --restore  also put back everything install.sh moved
#                                 into its backups, returning the system to
#                                 how it was before the first install
#   tools/uninstall.sh --yes      don't ask
#
# Only files listed in the install manifest are removed. Packages and system
# settings are never touched. --restore also works after a plain uninstall.
set -euo pipefail

restore=0; assumeYes=0
for arg in "$@"; do
	case "$arg" in
		--restore) restore=1 ;;
		--yes|-y) assumeYes=1 ;;
		-h|--help) sed -n '2,12p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "unknown option: $arg (try --help)" >&2; exit 2 ;;
	esac
done

die() { echo "uninstall.sh: $*" >&2; exit 1; }
command -v finddir >/dev/null || die "finddir not found; this runs on Haiku"

userNP="$(finddir B_USER_NONPACKAGED_DIRECTORY)"
userSettings="$(finddir B_USER_SETTINGS_DIRECTORY)"
userHome="$(finddir B_USER_DIRECTORY)"
state="$userSettings/nvidia-haiku"
manifest="$state/manifest"
backups="$state/backups"
envBegin="# >>> nvidia-haiku: managed by tools/install.sh, removed by tools/uninstall.sh >>>"
envEnd="# <<< nvidia-haiku <<<"
envFile="$(sed -n 's/^envfile=//p' "$state/install.info" 2>/dev/null || true)"
envFile="${envFile:-$userSettings/boot/UserSetupEnvironment}"

haveManifest=0; [ -f "$manifest" ] && haveManifest=1
haveBackups=0; [ -s "$backups" ] && haveBackups=1
if [ $haveManifest = 0 ] && { [ $restore = 0 ] || [ $haveBackups = 0 ]; }; then
	die "nothing to do: no install by tools/install.sh found ($state)"
fi

if [ $haveManifest = 1 ]; then
	echo "Will remove $(wc -l < "$manifest") files from $userNP."
fi
if [ -f "$envFile" ] && grep -qF "$envBegin" "$envFile"; then
	echo "Will remove the nvidia-haiku Vulkan setting from $envFile."
fi
if [ $restore = 1 ]; then
	if [ $haveBackups = 1 ]; then
		n=0; while read -r b; do [ -f "$b/restore.list" ] && n=$((n + $(wc -l < "$b/restore.list"))); done < "$backups"
		echo "Will restore $n files from $(wc -l < "$backups") backup(s) in $userHome/nvidia-haiku-backups."
	else
		echo "No backups to restore."
	fi
fi
if [ $assumeYes = 0 ]; then
	printf 'Continue? [y/N] '
	read -r answer
	case "$answer" in y|Y|yes|YES) ;; *) echo "Nothing changed."; exit 0 ;; esac
fi

removed=0
if [ $haveManifest = 1 ]; then
	while read -r rel; do
		if [ -e "$userNP/$rel" ] || [ -L "$userNP/$rel" ]; then
			rm -f "$userNP/$rel"
			removed=$((removed + 1))
		fi
		# Remove directories this install left empty (never the top level).
		dir="$(dirname "$userNP/$rel")"
		while [ "$dir" != "$userNP" ] && rmdir "$dir" 2>/dev/null; do
			dir="$(dirname "$dir")"
		done
	done < "$manifest"
	rm -f "$manifest" "$state/install.info"
fi

if [ -f "$envFile" ] && grep -qF "$envBegin" "$envFile"; then
	sed -i "\\|^$envBegin\$|,\\|^$envEnd\$|d" "$envFile"
	[ -s "$envFile" ] || rm -f "$envFile"
fi

restored=0; skipped=0
if [ $restore = 1 ] && [ $haveBackups = 1 ]; then
	# Newest backup first, so the oldest (the system before the first
	# install) is applied last.
	for b in $(tac "$backups"); do
		[ -f "$b/restore.list" ] || continue
		while read -r path; do
			if [ -e "$path" ] || [ -L "$path" ]; then
				echo "  exists, not restoring: $path"
				skipped=$((skipped + 1))
				continue
			fi
			mkdir -p "$(dirname "$path")"
			mv "$b/files$path" "$path"
			restored=$((restored + 1))
		done < "$b/restore.list"
	done
	rm -f "$backups"
fi
rmdir "$state" 2>/dev/null || true

if [ $haveManifest = 1 ]; then echo "Removed $removed files."; fi
if [ $restore = 1 ]; then echo "Restored $restored files ($skipped skipped)."; fi
if [ -s "$backups" ]; then
	echo "Files from before the install are kept in $userHome/nvidia-haiku-backups;"
	echo "tools/uninstall.sh --restore puts them back."
fi
echo "Reboot to finish."
