lib_nvidia_gsp = shared_library('nvidia_gsp',
	'IntrSafePool.cpp',
	'Driver.cpp',
	'NvidiaModule.cpp',
	'BaseDevice.cpp',
	'ControlDevice.cpp',
	'Device.cpp',
	'os-haiku.cpp',
	'WorkQueue.cpp',
	'stubs.c',
	include_directories: [
		'..',

	    '/boot/system/develop/headers/private',
	    '/boot/system/develop/headers/private/shared',
	    '/boot/system/develop/headers/private/kernel',
	    '/boot/system/develop/headers/private/kernel/arch/x86',
	    '/boot/system/develop/headers/private/kernel/arch/x86/64',
	    '/boot/system/develop/headers/private/system',
	    '/boot/system/develop/headers/private/system/arch/x86_64',
	    '/boot/system/develop/headers/private/kernel/boot/platform/efi',
	],
	link_whole: [
		lib_nvhaiku_common,
	],
	dependencies: [
		dep_nvrm,
		dep_nv_kernel,
	],
	name_prefix: '',
	gnu_symbol_visibility: 'hidden',
)

# Haiku kernel add-ons have no file extension. Meson 1.12 rejects
# name_suffix: '', so build with the default suffix and install a copy
# under the add-on's name; the binary is unchanged.
custom_target('nvidia_gsp_addon',
	input: lib_nvidia_gsp,
	output: 'nvidia_gsp',
	command: [find_program('cp'), '@INPUT@', '@OUTPUT@'],
	install: true,
	install_dir: 'add-ons/kernel/drivers/bin',
)

install_symlink('nvidia_gsp', pointing_to: '../../bin/nvidia_gsp', install_dir: 'add-ons/kernel/drivers/dev/graphics')
