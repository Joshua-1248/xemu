if vulkan.found()

specific_ss.add([volk, libglslang, vma, vulkan, spirv_reflect, 
	files(
		'blit.c',
		'buffer.c',
		'command.c',
		'debug.c',
		'display.c',
		'draw.c',
		'glsl.c',
		'gpuprops.c',
		'image.c',
		'instance.c',
		'renderer.c',
		'reports.c',
		'shaders.c',
		'surface-compute.c',
		'surface.c',
		'texture.c',
		'vertex.c',
		)
	])

if get_option('xemu_feature_texture_packs')
	specific_ss.add([volk, libglslang, vma, vulkan, spirv_reflect,
		files('../../../../../xemu-features/texture-packs/texture-packs-vk.c')])
endif

endif
