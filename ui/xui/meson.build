xemu_ss.add(files(
  'actions.cc',
  'animation.cc',
  'compat.cc',
  'debug.cc',
  'font-manager.cc',
  'gl-helpers.cc',
  'input-manager.cc',
  'main-menu.cc',
  'main.cc',
  'misc.cc',
  'menubar.cc',
  'monitor.cc',
  'notifications.cc',
  'popup-menu.cc',
  'reporting.cc',
  'scene-components.cc',
  'scene-manager.cc',
  'scene.cc',
  'snapshot-manager.cc',
  'viewport-manager.cc',
  'welcome.cc',
  'widgets.cc',
))

if host_os == 'windows'
  xemu_ss.add(files('update.cc'))
endif


# Optional custom-fork features. Sources live physically under top-level
# xemu-features/ and disappear from the build when their Meson option is OFF.
if get_option('xemu_feature_audio_packs')
  xemu_ss.add(files('../../xemu-features/audio-packs/frontend.cc'))
endif
if get_option('xemu_feature_texture_packs')
  xemu_ss.add(files('../../xemu-features/texture-packs/frontend.cc'))
endif
if get_option('xemu_feature_cheats')
  xemu_ss.add(files(
    '../../xemu-features/cheats/cheatfile.cc',
    '../../xemu-features/cheats/codes-engine.cc',
    '../../xemu-features/cheats/codes.cc',
    '../../xemu-features/cheats/runtime.cc',
  ))
endif
if get_option('xemu_feature_tas')
  xemu_ss.add(files('../../xemu-features/tas/studio.cc'))
endif
if get_option('xemu_feature_scripting')
  xemu_ss.add(files(
    '../../xemu-features/scripting/script-console.cc',
    '../../xemu-features/scripting/frontend.cc',
  ))
endif
if get_option('xemu_feature_debug_tools')
  xemu_ss.add(files(
    '../../xemu-features/debug-tools/function-index.cc',
    '../../xemu-features/debug-tools/disassembler.cc',
    '../../xemu-features/debug-tools/frontend.cc',
  ))
endif
if get_option('xemu_feature_fast_forward')
  xemu_ss.add(files(
    '../../xemu-features/fast-forward/frontend.cc',
    '../../xemu-features/fast-forward/timing.c',
  ))
endif
if get_option('xemu_feature_disc_modding')
  xemu_ss.add(files(
    '../../xemu-features/disc-modding/xdvdfs.cc',
    '../../xemu-features/disc-modding/mounted-disc-bridge.c',
    '../../xemu-features/disc-modding/core.cc',
    '../../xemu-features/disc-modding/frontend.cc',
  ))
endif
