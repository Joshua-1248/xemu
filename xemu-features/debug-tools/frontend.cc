// SPDX-License-Identifier: GPL-2.0-or-later
// xemu custom fork - optional disassembler/debug extension
#include "frontend.hh"
#include "ui/xui/common.hh"
#include "disassembler.hh"

/* Feature-owned build shims; avoid upstream Meson edits for renderer/debug
 * utility windows that are implemented entirely under xemu-features/. */
#include "xemu-features/geometry-dumper/frontend.cc"
#include "xemu-features/freecam/frontend.cc"

void FeatureDebugToolsDrawMenuItems()
{
    // Geometry Dumper and Free Camera are user-facing tools rather than core
    // debugger panes; both now live under Misc with the rest of the custom
    // feature suite.
    ImGui::MenuItem("Disassembler", nullptr, &disassembler_window.m_is_open);
}

void FeatureDebugToolsDrawWindows()
{
    disassembler_window.Draw();
}
