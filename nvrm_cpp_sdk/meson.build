project('nvrm_cpp_sdk', 'cpp', 'c',
	version: '570.133.07',
	default_options: ['cpp_std=c++20']
)

pkg = import('pkgconfig')

dep_nvrm = dependency('nvrm')
dep_nvkms = dependency('nvkms')

lib_nvrm_client_cpp = static_library('nvrm-client-cpp',
	'ErrorUtils.cpp',
	'NvRmApi.cpp',
	'NvRmDevice.cpp',
	dependencies: [
		dep_nvrm,
	],
	install: true,
	install_dir: 'develop/lib',
)

lib_nvkms_client_cpp = static_library('nvkms-client-cpp',
	'NvKmsApi.cpp',
	'NvKmsDevice.cpp',
	'NvKmsSurface.cpp',
	dependencies: [
		dep_nvkms,
	],
	link_with: [
		lib_nvrm_client_cpp,
	],
	install: true,
	install_dir: 'develop/lib',
)

pkg.generate(lib_nvrm_client_cpp, requires: ['nvrm-common'], subdirs: ['nvrm-cpp-sdk/nvidia'])
pkg.generate(lib_nvkms_client_cpp, subdirs: ['nvrm-cpp-sdk/nvidia-modeset'])

install_headers('FileDesc.h', subdir : 'nvrm-cpp-sdk/nvidia')
install_headers('ErrorUtils.h', subdir : 'nvrm-cpp-sdk/nvidia')
install_headers('NvRmApi.h', subdir : 'nvrm-cpp-sdk/nvidia')
install_headers('NvRmDevice.h', subdir : 'nvrm-cpp-sdk/nvidia')

install_headers('NvKmsApi.h', subdir : 'nvrm-cpp-sdk/nvidia-modeset')
install_headers('NvKmsDevice.h', subdir : 'nvrm-cpp-sdk/nvidia-modeset')
install_headers('NvKmsSurface.h', subdir : 'nvrm-cpp-sdk/nvidia-modeset')
