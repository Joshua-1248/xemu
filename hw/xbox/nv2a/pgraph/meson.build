specific_ss.add(files(
	'pgraph.c',
	'profile.c',
	'rdi.c',
	's3tc.c',
	'swizzle.c',
	'texture.c',
	'vertex.c',
	))

# Renderer-neutral texture-pack implementation. Compile it once regardless of
# which renderer(s) are enabled; GL/Vulkan adapters are added by their own
# renderer subdirectories.
if get_option('xemu_feature_texture_packs')
  texture_pack_deps = []
  if have_libwebp
    texture_pack_deps += [libwebp, libwebpdemux]
  endif
  specific_ss.add([texture_pack_deps, files(
    '../../../../xemu-features/texture-packs/texture-packs.c',
  )])
endif

if have_renderdoc
	specific_ss.add(files('debug_renderdoc.c'))
endif
subdir('thirdparty')
subdir('null')
subdir('gl')
subdir('glsl')
subdir('vk')
specific_ss.add(nv2a_vsh_cpu)
