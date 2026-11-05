project('nvrm_sdk', 'cpp', 'c',
	version: '570.133.07',
	default_options: ['cpp_std=c++20']
)

pkg = import('pkgconfig')


dep_nvrm = declare_dependency(
	include_directories: [
		'open-gpu-kernel-modules/src/common/sdk/nvidia/inc',
		'open-gpu-kernel-modules/src/common/unix/common/inc',
		'open-gpu-kernel-modules/src/common/inc',
		'open-gpu-kernel-modules/src/nvidia/arch/nvalloc/common/inc',
		'open-gpu-kernel-modules/src/nvidia/arch/nvalloc/unix/include',
		'open-gpu-kernel-modules/src/nvidia/inc/kernel',
	],
)


lib_nv_kernel = static_library('nv-kernel',
	objects: [
		'open-gpu-kernel-modules/src/nvidia/_out/Haiku_x86_64/nv-kernel.o',
	],
	install: true,
	install_dir: 'develop/lib',
)

lib_nv_modeset_kernel = static_library('nv-modeset-kernel',
	objects: [
		'open-gpu-kernel-modules/src/nvidia-modeset/_out/Haiku_x86_64/nv-modeset-kernel.o',
	],
	install: true,
	install_dir: 'develop/lib',
)

lib_nvrm_common = static_library('nvrm-common',
	'open-gpu-kernel-modules/src/common/shared/nvstatus/nvstatus.c',
	dependencies: [
		dep_nvrm,
	],
	install: true,
	install_dir: 'develop/lib',
)

lib_nvkms_common = static_library('nvkms-common',
	'open-gpu-kernel-modules/src/nvidia-modeset/lib/nvkms-format.c',
	'open-gpu-kernel-modules/src/nvidia-modeset/lib/nvkms-sync.c',
	dependencies: [
		dep_nvrm,
	],
	include_directories: [
		'open-gpu-kernel-modules/src/nvidia-modeset/interface',
	],
	install: true,
	install_dir: 'develop/lib',
)


pkg.generate(lib_nv_kernel, version: '570.133.07')
pkg.generate(lib_nv_modeset_kernel, version: '570.133.07')
pkg.generate(lib_nvrm_common, version: '570.133.07', requires: ['nvrm'])
pkg.generate(lib_nvkms_common, version: '570.133.07', requires: ['nvkms'])
pkg.generate(
	name: 'nvrm',
	description: 'NVRM',
	version: '570.133.07',
	extra_cflags: ['-D_DEFAULT_SOURCE', '-DNV_PLATFORM_MAX_IOCTL_SIZE=16384'],
	subdirs: ['nvrm-sdk/common', 'nvrm-sdk/nvidia']
)
pkg.generate(
	name: 'nvkms',
	description: 'NVKMS',
	version: '570.133.07',
	requires: ['nvrm'],
	subdirs: ['nvrm-sdk/nvidia-modeset']
)


executable('CompileTest',
	'CompileTest.c',
	'open-gpu-kernel-modules/src/common/shared/nvstatus/nvstatus.c',
	include_directories: [
		'open-gpu-kernel-modules/src/common/sdk/nvidia/inc',
		'open-gpu-kernel-modules/src/common/unix/common/inc',
		'open-gpu-kernel-modules/src/common/inc',
		'open-gpu-kernel-modules/src/nvidia/arch/nvalloc/common/inc',
		'open-gpu-kernel-modules/src/nvidia/arch/nvalloc/unix/include',
		'open-gpu-kernel-modules/src/nvidia/inc/kernel',
		'open-gpu-kernel-modules/src/nvidia-modeset/interface',
	],
)

install_subdir('open-gpu-kernel-modules/src/common/sdk/nvidia/inc/.', install_dir : 'develop/headers/nvrm-sdk/common')
install_subdir('open-gpu-kernel-modules/src/common/unix/common/inc/.', install_dir : 'develop/headers/nvrm-sdk/common')
install_subdir('open-gpu-kernel-modules/src/common/inc/.', install_dir : 'develop/headers/nvrm-sdk/common')
install_subdir('open-gpu-kernel-modules/src/common/nvlink/.', install_dir : 'develop/headers/nvrm-sdk/common/nvlink')
install_subdir('open-gpu-kernel-modules/src/nvidia/arch/nvalloc/common/inc/.', install_dir : 'develop/headers/nvrm-sdk/nvidia')
install_subdir('open-gpu-kernel-modules/src/nvidia/arch/nvalloc/unix/include/.', install_dir : 'develop/headers/nvrm-sdk/nvidia')
install_subdir('open-gpu-kernel-modules/src/nvidia/inc/kernel/os/.', install_dir : 'develop/headers/nvrm-sdk/nvidia/os')
install_subdir('open-gpu-kernel-modules/src/nvidia-modeset/interface/.', install_dir : 'develop/headers/nvrm-sdk/nvidia-modeset')
install_subdir('open-gpu-kernel-modules/src/nvidia-modeset/os-interface/include/.', install_dir : 'develop/headers/nvrm-sdk/nvidia-modeset')
install_subdir('open-gpu-kernel-modules/src/nvidia-modeset/kapi/interface/.', install_dir : 'develop/headers/nvrm-sdk/nvidia-modeset')

install_headers('open-gpu-kernel-modules/src/nvidia/interface/nvrm_registry.h', subdir : 'nvrm-sdk/nvidia')
install_headers('nv-haiku.h', subdir : 'nvrm-sdk/nvidia')
install_headers('nvkms-haiku.h', subdir : 'nvrm-sdk/nvidia-modeset')
