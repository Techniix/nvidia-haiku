#!/bin/bash
# Host test (Linux, no Haiku) for tools/install-package.sh and
# tools/uninstall-package.sh: fake pkgman, package and finddir commands
# record what would happen in a temporary home.
#
# Scenario: quasar A (e.g. a stock-Haiku build) is installed; installing
# quasar B must keep A, and --restore-previous must reinstall A. Also: a
# stray VideoStreamsWsi layer is reported.
set -u
here="$(cd "$(dirname "$0")" && pwd)"
root="$here/../.."
t="$(mktemp -d)"
trap 'rm -rf "$t"' EXIT
fail() { echo "FAIL: $*"; cat "$t/log" 2>/dev/null; exit 1; }

mkdir -p "$t/bin" "$t/home/config/packages" "$t/system/packages" "$t/home/config/settings" \
	"$t/home/config/add-ons/vulkan/implicit_layer.d" "$t/home/config/non-packaged/add-ons/vulkan/implicit_layer.d"

cat > "$t/bin/finddir" <<F
#!/bin/sh
case "\$1" in
	B_USER_SETTINGS_DIRECTORY) echo "$t/home/config/settings" ;;
	B_USER_DIRECTORY) echo "$t/home" ;;
	B_SYSTEM_PACKAGES_DIRECTORY) echo "$t/system/packages" ;;
	B_USER_PACKAGES_DIRECTORY) echo "$t/home/config/packages" ;;
	B_USER_CONFIG_DIRECTORY) echo "$t/home/config" ;;
	B_SYSTEM_DIRECTORY) echo "$t/system" ;;
	B_SYSTEM_NONPACKAGED_DIRECTORY) echo "$t/system/non-packaged" ;;
	B_USER_NONPACKAGED_DIRECTORY) echo "$t/home/config/non-packaged" ;;
esac
F
# package list -p/-i on our fake .hpkg files: the file holds its own listing.
cat > "$t/bin/package" <<'F'
#!/bin/sh
[ "$1" = list ] || exit 1
case "$2" in
	-i) sed -n 's/^INFO //p' "$3" ;;
	-p) sed -n 's/^FILE //p' "$3" ;;
esac
F
# pkgman: installs by copying into the packages dir, uninstall removes.
cat > "$t/bin/pkgman" <<F
#!/bin/sh
echo "pkgman \$*" >> "$t/log"
cmd=\$1; shift
home=0; files=""
for a in "\$@"; do case "\$a" in -H) home=1 ;; -y) ;; *) files="\$files \$a" ;; esac; done
dir="$t/system/packages"; [ \$home = 1 ] && dir="$t/home/config/packages"
case "\$cmd" in
	install) for f in \$files; do cp "\$f" "\$dir/"; done ;;
	uninstall) for n in \$files; do rm -f "\$dir/\$n"-*.hpkg; done ;;
esac
F
cat > "$t/bin/uname" <<'F'
#!/bin/sh
echo Haiku
F
chmod +x "$t/bin/"*

mkpkg() { # file version
	{ echo "INFO name: quasar"; echo "INFO version: $2";
	  echo "FILE add-ons/opengl/egl_vendor.d/50_mesa.json"; } > "$1"
}
mkpkg "$t/quasar-A-1-x86_64.hpkg" A
mkpkg "$t/home/config/packages/quasar-A-1-x86_64.hpkg" A
mkpkg "$t/quasar-B-1-x86_64.hpkg" B
touch "$t/home/config/non-packaged/add-ons/vulkan/implicit_layer.d/VideoStreamsWsi.json"

export PATH="$t/bin:$PATH" NVIDIA_HAIKU_TEST=1
# install.sh --clean-only is real code that walks the system; stub it here.
cp -r "$root/tools" "$t/tools"
printf '#!/bin/sh\necho "Clean-up: nothing to do"\n' > "$t/tools/install.sh"
chmod +x "$t/tools/install.sh"

out="$("$t/tools/install-package.sh" --yes "$t/quasar-B-1-x86_64.hpkg" 2>&1)" || fail "install B: $out"
echo "$out" | grep -q "Kept a copy of quasar-A-1-x86_64.hpkg" || fail "A not kept: $out"
[ -f "$t/home/nvidia-haiku-backups/quasar-previous/quasar-A-1-x86_64.hpkg" ] || fail "A copy missing"
[ -f "$t/home/config/packages/quasar-B-1-x86_64.hpkg" ] || fail "B not installed"
[ ! -f "$t/home/config/packages/quasar-A-1-x86_64.hpkg" ] || fail "A still installed"
echo "$out" | grep -q "WARNING: another window-system layer: .*non-packaged.*VideoStreamsWsi.json" || fail "stray layer not reported: $out"

# Reinstalling the same file must not replace the saved previous one.
out="$("$t/tools/install-package.sh" --yes "$t/quasar-B-1-x86_64.hpkg" 2>&1)" || fail "reinstall B: $out"
[ -f "$t/home/nvidia-haiku-backups/quasar-previous/quasar-A-1-x86_64.hpkg" ] || fail "A copy lost on reinstall"

out="$("$t/tools/uninstall-package.sh" --yes --restore-previous 2>&1)" || fail "restore: $out"
[ -f "$t/home/config/packages/quasar-A-1-x86_64.hpkg" ] || fail "A not restored: $out"
[ ! -f "$t/home/config/packages/quasar-B-1-x86_64.hpkg" ] || fail "B still installed"

"$t/tools/uninstall-package.sh" --yes --restore-previous --restore-nebula >/dev/null 2>&1 && fail "both restore options accepted"
echo "install-scripts: all checks passed"
