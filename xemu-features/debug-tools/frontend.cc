// SPDX-License-Identifier: GPL-2.0-or-later
// xemu custom fork - optional disassembler/debug extension
#include "frontend.hh"
#include "ui/xui/common.hh"
#include "disassembler.hh"

void FeatureDebugToolsDrawMenuItems()
{
    ImGui::MenuItem("Disassembler", nullptr, &disassembler_window.m_is_open);
}

void FeatureDebugToolsDrawWindows()
{
    disassembler_window.Draw();
}
