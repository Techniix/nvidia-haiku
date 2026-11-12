# RengaWsi: Vulkan surface layer for Renga. See RengaWsi.cpp and
# docs/RENGA-WSI.md. Swapchains are implemented by NVK (Mesa's
# wsi_common_renga.cpp); this layer keeps the surface objects applications
# call SetBitmapHook() on, and passes the hook to the driver.
project('RengaWsi', 'cpp',
	version : '1',
	default_options : ['warning_level=3', 'cpp_std=c++20']
)

add_global_arguments('-Wno-missing-field-initializers', language : 'cpp')

shared_library('RengaWsi',
	'RengaWsi.cpp',
	name_prefix: '',
	dependencies: [dependency('vulkan')],
	gnu_symbol_visibility: 'hidden',
	install_dir: 'add-ons/vulkan/implicit_layer.d',
	install: true
)

install_data(
	'RengaWsi.json',
	install_dir: 'add-ons/vulkan/implicit_layer.d',
)
