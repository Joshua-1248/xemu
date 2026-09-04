# SPDX-License-Identifier: GPL-2.0-or-later
# Feature-owned build integration for the unmodified Capstone 5.0.9 source.

capstone_upstream = meson.current_source_dir() / 'upstream'
capstone_inc = include_directories('upstream/include', 'upstream/include/capstone', 'upstream')

capstone_sources = files(
  'upstream/cs.c',
  'upstream/utils.c',
  'upstream/SStream.c',
  'upstream/MCInstrDesc.c',
  'upstream/MCRegisterInfo.c',
  'upstream/MCInst.c',
  'upstream/Mapping.c',
  'upstream/arch/X86/X86DisassemblerDecoder.c',
  'upstream/arch/X86/X86Disassembler.c',
  'upstream/arch/X86/X86InstPrinterCommon.c',
  'upstream/arch/X86/X86IntelInstPrinter.c',
  'upstream/arch/X86/X86ATTInstPrinter.c',
  'upstream/arch/X86/X86Mapping.c',
  'upstream/arch/X86/X86Module.c',
)

xemu_bundled_capstone = static_library(
  'xemu-capstone-5.0.9',
  capstone_sources,
  include_directories: capstone_inc,
  c_args: [
    '-DCAPSTONE_STATIC',
    '-DCAPSTONE_USE_SYS_DYN_MEM',
    '-DCAPSTONE_HAS_X86',
  ],
  pic: true,
  install: false,
  override_options: ['c_std=gnu99', 'warning_level=0', 'werror=false'],
)

xemu_bundled_capstone_dep = declare_dependency(
  link_with: xemu_bundled_capstone,
  include_directories: capstone_inc,
)
