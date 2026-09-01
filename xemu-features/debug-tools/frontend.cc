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
    ImGui::MenuItem("Disassembler", nullptr, &disassembler_window.m_is_open);
    FeatureGeometryDumperDrawMenuItem();
}

void FeatureDebugToolsDrawWindows()
{
    disassembler_window.Draw();
    FeatureGeometryDumperDrawWindow();
    FeatureFreecamDrawWindow();
}
