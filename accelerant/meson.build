project('nvidia_gsp', 'cpp', 'c',
	default_options: ['cpp_std=c++20']
)

cpp = meson.get_compiler('cpp')

dep_nvrm_client_cpp = dependency('nvrm-client-cpp')
dep_nvkms_client_cpp = dependency('nvkms-client-cpp')
# nvKmsResetNotifier/nvKmsParseNotifier (NVIDIA's nvkms-sync.c) for flip
# completion notifiers in NvPresent.cpp. Without it the accelerant still
# builds (a shared module may leave symbols undefined) and then fails to
# load in app_server.
dep_nvkms_common = dependency('nvkms-common')

dep_be = cpp.find_library('be')
# eventfd() for presentation events (NvPresent.cpp); Renga's libgnu.
dep_gnu = cpp.find_library('gnu')
# libgnu's headers (sys/eventfd.h) are in develop/headers/gnu, which GCC on
# Renga does not search by default (Renga's 3rdparty/ce/PORTING.md).
gnu_headers = '/boot/system/develop/headers/gnu'
# (A compile_args pair "-isystem <dir>" was dropped by meson on Renga; an
# include_directories object is passed through.)
dep_gnu = declare_dependency(
	dependencies: dep_gnu,
	include_directories: include_directories(gnu_headers, is_system: true),
)

subdir('common')

shared_module('nvidia_gsp',
	'Accelerant.cpp',
	'NvUtils.cpp',
	'NvKmsBitmap.cpp',
	'NvPresent.cpp',
	include_directories: include_directories('../headers'),
	dependencies: [
		dep_nvrm_client_cpp,
		dep_nvkms_client_cpp,
		dep_accelerant_common,
		dep_gnu,
		dep_nvkms_common,
	],
	name_prefix: '',
	name_suffix: 'accelerant',
	# Fail at build time, not when app_server loads the accelerant.
	link_args: ['-Wl,--no-undefined'],
	gnu_symbol_visibility: 'hidden',
	install: true,
	install_dir: 'add-ons/accelerants',
)

executable('AccelerantTest',
	'AccelerantTest.cpp',
	gnu_symbol_visibility: 'hidden',
	install: false,
)

executable('ModesetTest',
	'ModesetTest.cpp',
	dependencies: [
		cpp.find_library('be'),
		dep_nvkms_client_cpp,
	],
	gnu_symbol_visibility: 'hidden',
	install: false,
)

executable('ModesetTool',
	'ModesetTool.cpp',
	dependencies: [
		dep_be,
	],
	gnu_symbol_visibility: 'hidden',
	install: true,
	install_dir: 'apps/Nebula',
)

install_symlink('ModesetTool', pointing_to: '../../../../../apps/Nebula/ModesetTool', install_dir: 'data/deskbar/menu/Applications/Nebula')
