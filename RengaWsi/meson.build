# RengaWsi: Vulkan surface layer for Renga. See RengaWsi.cpp and
# docs/RENGA-WSI.md. Swapchains are implemented by NVK (Mesa's
# wsi_common_renga.cpp); this layer keeps the surface objects applications
# call SetBitmapHook() on, and passes the hook to the driver.
project('RengaWsi', 'cpp',
	version : '1',
	default_options : ['warning_level=3', 'cpp_std=c++20']
)

add_global_arguments('-Wno-missing-field-initializers', language : 'cpp')

# Vulkan headers: Mesa's copy in the mesa-nvk submodule, the same headers
# NVK itself is compiled against, so the layer and the driver always agree
# on the Vulkan version and on the layer interface (vk_layer.h). Renga's
# vulkan_loader package carries no headers, and a separate headers package
# could be older or newer than the driver. tools/crosscheck uses the same
# directory.
fs = import('fs')
vulkan_include = meson.current_source_dir() / '..' / 'mesa-nvk' / 'include'
if not fs.exists(vulkan_include / 'vulkan' / 'vk_layer.h')
	error('Vulkan headers not found in ' + vulkan_include + '. Run '
		+ 'tools/submodules.sh update to check out the mesa-nvk submodule.')
endif

shared_library('RengaWsi',
	'RengaWsi.cpp',
	name_prefix: '',
	# Vulkan headers only (see above). A layer must not link the Vulkan
	# loader: the loader loads the layer.
	cpp_args: ['-I' + vulkan_include],
	link_args: ['-Wl,--no-undefined'],
	gnu_symbol_visibility: 'hidden',
	install_dir: 'add-ons/vulkan/implicit_layer.d',
	install: true
)

install_data(
	'RengaWsi.json',
	install_dir: 'add-ons/vulkan/implicit_layer.d',
)
