#!/bin/bash
# Check out Quasar's submodules, repairing any that a cut-off download left
# broken. Used by renga-setup.sh and by renga-test.sh build.
#
#   tools/submodules.sh check    exit 0 if every submodule is checked out
#                                and its current commit can be read
#   tools/submodules.sh update   repair broken ones, then check out the
#                                commits this Quasar commit records
#
# Why a repair step: a submodule directory can exist while its repository
# has no usable HEAD, typically when the first clone of a large one (Mesa,
# NVIDIA's kernel sources) was interrupted. "git submodule status" then
# doesn't flag it as missing, and "git submodule update" stops with "Unable
# to find current revision in submodule path". Such a submodule is removed
# (its working directory and its repository under .git/modules) and cloned
# again.
#
# The submodules are shallow (shallow = true in .gitmodules): only the
# commit Quasar needs is downloaded, not the whole history, which for Mesa
# is most of the download and makes an interrupted clone less likely.
set -u
base="$(cd "$(dirname "$0")/.." && pwd)"
cd "$base" || exit 1

# Names and paths from .gitmodules, one "name path" per line.
submodules() {
	git config -f .gitmodules --get-regexp '^submodule\..*\.path$' |
		sed -e 's/^submodule\.//' -e 's/\.path / /'
}

# 0 if the submodule at $1 has a commit checked out that git can read.
healthy() {
	[ -e "$1/.git" ] && git -C "$1" rev-parse -q --verify HEAD >/dev/null 2>&1
}

check() {
	local ok=0 name path
	while read -r name path; do
		if ! healthy "$path"; then
			echo "submodule $path: not checked out or broken"
			ok=1
		fi
	done < <(submodules)
	return $ok
}

update() {
	local name path gitdir
	# Repair first: "git submodule sync" itself fails on a broken one.
	while read -r name path; do
		# Present but unreadable: start that one over.
		if [ -e "$path/.git" ] && ! healthy "$path"; then
			echo "submodule $path is broken (an interrupted download?); fetching it again"
			git submodule deinit -f -- "$path" >/dev/null 2>&1
			gitdir="$(git rev-parse --git-dir)/modules/$name"
			rm -rf -- "$gitdir" "$path"
		fi
	done < <(submodules)
	git submodule sync --recursive >/dev/null || return 1
	git submodule update --init --recommend-shallow || return 1
	check
}

case "${1:-}" in
	check) check ;;
	update) update ;;
	*) echo "usage: $0 check|update" >&2; exit 2 ;;
esac
