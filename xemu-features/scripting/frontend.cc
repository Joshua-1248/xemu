// SPDX-License-Identifier: GPL-2.0-or-later
//
// xemu custom fork - isolated Misc custom-feature frontend aggregator
//
// Copyright (C) 2026 Joshua-1248
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include "frontend.hh"
#include "ui/xui/common.hh"
#include "script-console.hh"
#include "xemu-features/cheats/frontend.hh"
#include "xemu-features/freecam/frontend.hh"

void FeatureScriptToolsDrawMenu()
{
    if (ImGui::BeginMenu("Misc")) {
        FeatureCodesDrawMiscMenuItem();
#ifdef CONFIG_XEMU_FEATURE_CHEATS
        ImGui::Separator();
#endif
#ifdef CONFIG_XEMU_FEATURE_DEBUG_TOOLS
        FeatureFreecamDrawMiscMenuItem();
        ImGui::Separator();
#endif
        if (ImGui::MenuItem("Lua Console")) {
            ShowLuaConsole();
        }
        if (ImGui::MenuItem("Python Console")) {
            ShowPythonConsole();
        }
        ImGui::EndMenu();
    }
}

void FeatureScriptToolsShowWindows()
{
    FeatureCodesShowWindows();
    ShowScriptConsoleWindows();
}

bool FeatureScriptToolsWindowsOpen()
{
    return FeatureCodesWindowsOpen() || ScriptConsoleWindowsOpen();
}
