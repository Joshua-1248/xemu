//
// xemu custom fork - isolated scripting tools frontend
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

void FeatureScriptToolsDrawMenu()
{
    if (ImGui::BeginMenu("Misc")) {
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
    ShowScriptConsoleWindows();
}

bool FeatureScriptToolsWindowsOpen()
{
    return ScriptConsoleWindowsOpen();
}
