project('nvidia_gsp', 'cpp', 'c',
	default_options: ['cpp_std=c++2a']
)

dep_nv_kernel = dependency('nv-kernel')
dep_nv_modeset_kernel = dependency('nv-modeset-kernel')
dep_nvrm = dependency('nvrm')
dep_nvkms = dependency('nvkms')

add_global_arguments(
	'-Wno-missing-field-initializers',
	language: ['c', 'cpp']
)

add_global_arguments(
	'-D_KERNEL_MODE',
	'-D_DEFAULT_SOURCE',
	'-DNV_PLATFORM_MAX_IOCTL_SIZE=16384',
	'-mno-red-zone',
	'-fno-exceptions',
	language: ['c', 'cpp']
)

cpp = meson.get_compiler('cpp')
fs = import('fs')

# Kernel add-ons are linked with -nostdlib, so the compiler's own
# crtbegin.o and crtend.o (which run the static constructors and
# destructors) have to be named explicitly. Their directory contains the
# GCC version (13.3.0 on older images, 15.3.0 on Renga 143b8c30), so ask
# the compiler where its copies are instead of hard-coding a path that
# breaks on every GCC update.
crt_files = []
foreach crt : ['crtbegin.o', 'crtend.o']
	crt_path = run_command(cpp.cmd_array(), '-print-file-name=' + crt,
		check: true).stdout().strip()
	if not fs.is_absolute(crt_path) or not fs.exists(crt_path)
		error('The compiler does not know where ' + crt + ' is (it answered "'
			+ crt_path + '"). Is the gcc development package installed?')
	endif
	crt_files += crt_path
endforeach

add_global_link_arguments(
	'/boot/system/develop/lib/_KERNEL_',
	crt_files,
	'/boot/system/develop/lib/haiku_version_glue.o',
	'-nostdlib',
	'-Wl,-z,max-page-size=0x1000',
	language: ['c', 'cpp']
)

lib_nvhaiku_common = static_library('nvhaiku_common',
	'CppUtils.cpp',
	'DeviceNamesArray.cpp',
)


subdir('nvidia')
subdir('nvidia-modeset')
