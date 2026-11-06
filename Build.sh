#!/bin/bash
set -e

baseDir="$PWD"
buildBaseDir="$PWD/build.$(getarch)"
installDir="$PWD/install.$(getarch)"

jobs="${JOBS:-$(nproc 2>/dev/null || echo 2)}"

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
	fi

	meson setup "${reconfigure[@]}" "$buildDir" \
		-Dpkg_config_path="$pcPath" \
		-Dprefix="$installDir" "${args[@]}"

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
buildProject VideoStreamsWsi "${vswArgs[@]}"

# libclc and SPIRV-LLVM-Translator are not in HaikuPorts; tools/build-clc-deps.sh
# builds them here. The /boot/data/packages paths are the original author's.
clcDeps="${DEPS_DIR:-$baseDir/deps.$(getarch)}/install"
if [ -n "$(projectDir mesa-nvk)" ] && [ ! -e "$clcDeps/share/pkgconfig/libclc.pc" ] &&
		[ ! -e /boot/data/packages/llvm-project/build-libclc.x86_64/install/share/pkgconfig/libclc.pc ]; then
	echo "libclc not found: run tools/build-clc-deps.sh first (NVK needs it to build)." >&2
	exit 1
fi
buildProject mesa-nvk \
	-Dpkg_config_path="$clcDeps/share/pkgconfig:$clcDeps/lib/pkgconfig:/boot/data/packages/llvm-project/build-libclc.x86_64/install/share/pkgconfig:/boot/data/packages/SPIRV-LLVM-Translator/build.x86_64/install/lib/pkgconfig:/boot/data/packages/Vulkan/SPIRV-Tools/build.x86_64/install/lib/pkgconfig" \
	-Dgallium-drivers= \
	-Dvulkan-drivers=nouveau \
	-Dplatforms=wayland \
	-Dgallium-rusticl=false \
	-Degl=disabled \
	-Dglvnd=disabled \
	-Dglx=disabled \
	-Ddisplay-info=disabled
# Zink (OpenGL on Vulkan) needs libglvnd. HaikuPorts has a recipe but no
# x86_64 package, and libglvnd replaces Haiku's OpenGL kit when installed, so
# skip Zink when it is absent rather than failing: the kernel driver,
# accelerant and Vulkan driver above do not need it.
glvndPcPath="${DEPS_DIR:-$baseDir/deps.$(getarch)}/install/develop/lib/pkgconfig:/boot/data/packages/libglvnd/build.x86_64/install/develop/lib/pkgconfig"
if [ -n "$(projectDir mesa-zink)" ] &&
		! PKG_CONFIG_PATH="$glvndPcPath${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}" pkg-config --exists libglvnd; then
	echo
	echo "Skipping mesa-zink (OpenGL): libglvnd not found. Everything else is built;"
	echo "Zink is only needed for OpenGL testing."
	exit 0
fi
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
