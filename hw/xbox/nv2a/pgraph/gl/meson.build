# libwebp is optional (see the libwebp block in the root meson.build). A
# not-found dependency cannot be passed to specific_ss.add(), so build the
# list conditionally rather than listing it inline.
gl_deps = [sdl, gloffscreen]
if have_libwebp
  gl_deps += [libwebp, libwebpdemux]
endif

specific_ss.add([gl_deps, files(
	'blit.c',
	'debug.c',
	'display.c',
	'draw.c',
	'gpuprops.c',
	'renderer.c',
	'reports.c',
	'shaders.c',
	'surface.c',
	'texture.c',
	'texture-io.c',
	'vertex.c',
	)])
