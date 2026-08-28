mcpx_ss.add(sdl, files(
	'apu.c',
	'debug.c',
	'monitor.c',
	))

subdir('vp')
subdir('dsp')

if get_option('xemu_feature_audio_packs')
	mcpx_ss.add(sdl, files(
		'../../../../xemu-features/audio-packs/audio-packs.c',
		'../../../../xemu-features/audio-packs/audio-packs-apu.c',
	))
endif

if get_option('xemu_feature_fast_forward')
	mcpx_ss.add(sdl, files('../../../../xemu-features/fast-forward/audio.c'))
endif

if get_option('xemu_feature_volume_amplifier')
	mcpx_ss.add(sdl, files('../../../../xemu-features/volume-amplifier/volume.c'))
endif
