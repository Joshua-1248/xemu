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
#include "xemu-features/shared/misc-menu.hh"

void FeatureScriptToolsDrawMenu()
{
    if (ImGui::BeginMenu("Misc")) {
        // Keep the primary runtime/modding tools together in a predictable
        // top-to-bottom order.
        FeatureCodesDrawMiscMenuItem();
        FeatureCustomToolsDrawMiscMenuItems();

        ImGui::Separator();
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
    FeatureCustomToolsShowWindows();
    ShowScriptConsoleWindows();
}

bool FeatureScriptToolsWindowsOpen()
{
    return FeatureCodesWindowsOpen() ||
           FeatureCustomToolsWindowsOpen() ||
           ScriptConsoleWindowsOpen();
}
