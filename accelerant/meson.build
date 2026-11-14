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

subdir('common')

shared_module('nvidia_gsp',
	'Accelerant.cpp',
	'NvUtils.cpp',
	'NvKmsBitmap.cpp',
	'NvPresent.cpp',
	dependencies: [
		dep_nvrm_client_cpp,
		dep_nvkms_client_cpp,
		dep_accelerant_common,
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
