#!/bin/bash
set -e

baseDir="$PWD"
buildBaseDir="$PWD/build.$(getarch)"
installDir="$PWD/install.$(getarch)"

jobs="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

# Optimised builds by default: -O2 with assertions off, keeping debug info
# (tools/make-package.sh strips it). DEBUG_BUILD=1 gives -O0 with
# assertions, as the original Build.sh did. NVIDIA's own objects are always
# built by NVIDIA's Makefiles at -O2.
if [ "${DEBUG_BUILD:-0}" = 1 ]; then
	buildTypeArgs=(-Dbuildtype=debug -Db_ndebug=false)
else
	buildTypeArgs=(-Dbuildtype=debugoptimized -Doptimization=2 -Db_ndebug=true)
fi

# Projects live either under source/ (the original author's layout) or at the
# repository root (this repository as checked out, with its submodules).
function projectDir {
	if [ -d "$baseDir/source/$1" ]; then
		echo "$baseDir/source/$1"
	elif [ -d "$baseDir/$1" ]; then
		echo "$baseDir/$1"
	fi
}

# nv-kernel.o and nv-modeset-kernel.o come from NVIDIA's own Makefiles;
# nvrm_sdk links them and expects the tree at nvrm_sdk/open-gpu-kernel-modules.
function buildNvidiaObjects {
	local nvDir
	nvDir="$(projectDir open-gpu-kernel-modules)"
	if [ -z "$nvDir" ]; then
		echo "open-gpu-kernel-modules not found; run: git submodule update --init" >&2
		exit 1
	fi
	local sdkDir
	sdkDir="$(projectDir nvrm_sdk)"
	if [ ! -e "$sdkDir/open-gpu-kernel-modules" ]; then
		ln -s "$nvDir" "$sdkDir/open-gpu-kernel-modules"
	fi
	# NVK's RM backend reads NVIDIA's headers from here (libnvk in
	# src/nouveau/vulkan/meson.build); they must match the RM being built.
	local nvkDir
	nvkDir="$(projectDir mesa-nvk)"
	if [ -n "$nvkDir" ] && [ ! -e "$nvkDir/src/nouveau/vulkan/nvkmd/nvrm/open-gpu-kernel-modules" ]; then
		ln -s "$nvDir" "$nvkDir/src/nouveau/vulkan/nvkmd/nvrm/open-gpu-kernel-modules"
	fi
	make -C "$nvDir/src/nvidia" -j"$jobs" ${NV_MAKE_ARGS:-}
	make -C "$nvDir/src/nvidia-modeset" -j"$jobs" ${NV_MAKE_ARGS:-}
}

function buildProject {
	projectName="$1"
	shift
	buildDir="$buildBaseDir/$projectName"

	local dir
	dir="$(projectDir "$projectName")"
	if [ -z "$dir" ]; then
		echo "skipping $projectName: not present" >&2
		return 0
	fi
	cd "$dir"

	# pkg.generate() writes .pc files to <prefix>/<libdir>/pkgconfig
	# (lib/pkgconfig on Haiku). Meson keeps only the last -Dpkg_config_path,
	# so merge any project-specific paths into one option rather than letting
	# them replace the install directory's.
	local pcPath="$installDir/lib/pkgconfig,$installDir/develop/lib/pkgconfig"
	local args=()
	local arg
	for arg in "$@"; do
		case "$arg" in
			-Dpkg_config_path=*) pcPath="$pcPath,${arg#-Dpkg_config_path=}" ;;
			*) args+=("$arg") ;;
		esac
	done

	# On an already-configured directory, plain "meson setup" ignores new
	# options; --reconfigure applies them.
	local reconfigure=()
	if [ -f "$buildDir/meson-private/coredata.dat" ]; then
		reconfigure=(--reconfigure)
		# A new release of the project (mesa-nvk moving from 26.2 to 26.3)
		# changes generated files and options enough that reusing the old
		# configuration is not safe: start that build directory over.
		if [ -f "$dir/VERSION" ] &&
				[ "$(cat "$buildDir/.quasar-source-version" 2>/dev/null)" != "$(cat "$dir/VERSION")" ]; then
			echo "$projectName: version changed to $(cat "$dir/VERSION"), configuring from scratch"
			reconfigure=(--wipe)
		fi
	fi

	meson setup "${reconfigure[@]}" "$buildDir" \
		-Dpkg_config_path="$pcPath" \
		-Dprefix="$installDir" "${buildTypeArgs[@]}" "${args[@]}"
	if [ -f "$dir/VERSION" ]; then
		cp "$dir/VERSION" "$buildDir/.quasar-source-version"
	fi

	ninja -C "$buildDir"
	ninja -C "$buildDir" install
}

if [ "${SKIP_NVIDIA_OBJECTS:-0}" != 1 ]; then
	buildNvidiaObjects
fi
buildProject nvrm_sdk
buildProject nvrm_cpp_sdk
buildProject nvidia_gsp
buildProject accelerant
buildProject NvKmsTest
# Local fixes for submodules live in tools/patches/<submodule>/*.patch and are
# applied in order on top of the pinned commit; already-applied patches are
# skipped, so this is safe to run on every build.
function applyPatches {
	local sub="$1" dir patch
	dir="$(projectDir "$sub")"
	[ -n "$dir" ] && [ -d "$baseDir/tools/patches/$sub" ] || return 0
	for patch in "$baseDir/tools/patches/$sub"/*.patch; do
		[ -e "$patch" ] || continue
		if git -C "$dir" apply --reverse --check "$patch" 2>/dev/null; then
			continue
		fi
		echo "Applying $(basename "$patch") to $sub"
		git -C "$dir" apply "$patch"
	done
}
# Current Haiku's <pthread.h> only defines the _NP spelling of the
# recursive mutex initializer, which VideoStreamsWsi uses unsuffixed. Map
# it only when the compiler lacks the plain name, so older Haiku is unchanged.
function hasPthreadMacro {
	printf '#include <pthread.h>\n#ifndef %s\n#error missing\n#endif\n' "$1" |
		c++ -fsyntax-only -x c++ - >/dev/null 2>&1
}
vswArgs=()
if ! hasPthreadMacro PTHREAD_RECURSIVE_MUTEX_INITIALIZER &&
		hasPthreadMacro PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP; then
	vswArgs+=(-Dcpp_args=-DPTHREAD_RECURSIVE_MUTEX_INITIALIZER=PTHREAD_RECURSIVE_MUTEX_INITIALIZER_NP)
fi
# Window-system layer. On this branch swapchains are in NVK (Mesa's
# wsi_common_renga.cpp) and RengaWsi only keeps surface objects; see
# docs/RENGA-WSI.md. QUASAR_WSI=videostreams builds the old VideoStreamsWsi
# layer instead, as a fallback while the new path is being tested. Only one
# of the two may be installed: both claim vkCreateHeadlessSurfaceEXT.
if [ "${QUASAR_WSI:-renga}" = videostreams ]; then
	applyPatches VideoStreamsWsi
	buildProject VideoStreamsWsi "${vswArgs[@]}"
else
	buildProject RengaWsi
fi

# libclc and SPIRV-LLVM-Translator are not in HaikuPorts; tools/build-clc-deps.sh
# builds them here. The /boot/data/packages paths are the original author's.
clcDeps="${DEPS_DIR:-$baseDir/deps.$(getarch)}/install"
if [ -n "$(projectDir mesa-nvk)" ] && [ ! -e "$clcDeps/share/pkgconfig/libclc.pc" ] &&
		[ ! -e /boot/data/packages/llvm-project/build-libclc.x86_64/install/share/pkgconfig/libclc.pc ]; then
	echo "libclc not found: run tools/build-clc-deps.sh first (NVK needs it to build)." >&2
	exit 1
fi
# mesa-nvk needs no patch step on this branch: the submodule is the
# "renga-26.3" branch of Techniix/mesa, where X547's Haiku port and the
# former tools/patches/mesa-nvk series are ordinary commits on top of Mesa
# 26.3 (see the first commit after upstream's 26.3 on that branch).

# Newer NVK (X547's event-sync branch) takes the NVIDIA tree and release as
# options and refuses to start against a kernel driver from another release.
nvkArgs=()
nvkDir="$(projectDir mesa-nvk)"
nvTree="$(projectDir open-gpu-kernel-modules)"
if [ -n "$nvkDir" ] && grep -q "'nvk-nvrm-source-path'" "$nvkDir/meson.options" 2>/dev/null; then
	nvVersion="$(sed -n 's/^NVIDIA_VERSION *= *//p' "$nvTree/version.mk" | tr -d '[:space:]')"
	nvkArgs+=(-Dnvk-nvrm-source-path="$nvTree" -Dnvk-nvrm-version="$nvVersion")
fi
buildProject mesa-nvk "${nvkArgs[@]}" \
	-Dpkg_config_path="$clcDeps/share/pkgconfig:$clcDeps/lib/pkgconfig:/boot/data/packages/llvm-project/build-libclc.x86_64/install/share/pkgconfig:/boot/data/packages/SPIRV-LLVM-Translator/build.x86_64/install/lib/pkgconfig:/boot/data/packages/Vulkan/SPIRV-Tools/build.x86_64/install/lib/pkgconfig" \
	-Dgallium-drivers= \
	-Dvulkan-drivers=nouveau \
	-Dvideo-codecs=all_free,h264dec \
	-Dplatforms=wayland \
	-Dgallium-rusticl=false \
	-Degl=disabled \
	-Dglvnd=disabled \
	-Dglx=disabled \
	-Ddisplay-info=disabled
# Zink (OpenGL on Vulkan) builds against libglvnd's headers. HaikuPorts has
# no x86_64 libglvnd package; tools/build-glvnd-deps.sh builds one into
# deps.<arch> for the build only. Without it, skip Zink rather than fail: the
# kernel driver, accelerant and Vulkan driver above do not need it.
glvndDeps="${DEPS_DIR:-$baseDir/deps.$(getarch)}/install"
glvndPcPath="$glvndDeps/lib/pkgconfig:$glvndDeps/develop/lib/pkgconfig:/boot/data/packages/libglvnd/build.x86_64/install/develop/lib/pkgconfig"
if [ -n "$(projectDir mesa-zink)" ] &&
		! PKG_CONFIG_PATH="$glvndPcPath${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" pkg-config --exists libglvnd; then
	echo
	echo "Skipping mesa-zink (OpenGL): libglvnd not found. Everything else is built."
	echo "Run tools/build-glvnd-deps.sh, then Build.sh again, to build Zink too."
	exit 0
fi
applyPatches mesa-zink
buildProject mesa-zink \
	-Dpkg_config_path="$glvndPcPath:/boot/data/packages/Vulkan/SPIRV-Tools/build.x86_64/install/lib/pkgconfig" \
	-Dplatforms=haiku \
	-Dgallium-opencl=disabled \
	-Dgallium-rusticl=false \
	-Dgallium-drivers=zink \
	-Dglx=disabled \
	-Dglvnd=enabled \
	-Degl=enabled \
	-Dvulkan-drivers= \
	-Dllvm=disabled \
	-Dshader-cache=enabled
