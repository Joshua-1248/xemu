//
// xemu User Interface - Script Consoles
//
// Copyright (C) 2026
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// Lua/Python consoles intentionally launch interpreters as child processes.
// This keeps the initial scripting layer isolated from emulation timing and
// avoids adding a mandatory Python/Lua build dependency to xemu.  Emulator
// API bindings can be layered on top through an explicit IPC/API bridge.
//

#include "script-console.hh"
#include "ui/xui/common.hh"
#include "ui/xui/misc.hh"
#include "ui/xemu-settings.h"
#include "xemu-features/shared/guest-memory.h"
#include "ui/xemu-snapshots.h"
#include "xemu-xbe.h"
#include "ui/xemu-notifications.h"
#include "xemu-features/tas/tas.h"
#include "ui/xui/actions.hh"
#include "system/runstate.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <fstream>
#include <sstream>
#include <iomanip>
#include <cstdint>

namespace {

enum class ScriptLanguage {
    Lua,
    Python,
};

struct ScriptOverlayText {
    float x = 0.0f;
    float y = 0.0f;
    std::string text;
};

struct ScriptEntry {
    std::string path;
    SDL_Process *process = nullptr;
    bool running = false;
    int last_exit_code = 0;
    std::string pending_output;
    bool wait_pending = false;
    uint64_t wait_id = 0;
    uint64_t wait_target = 0;
    std::vector<ScriptOverlayText> overlays;
};

static std::string Basename(const std::string &path)
{
    try {
        return std::filesystem::path(path).filename().string();
    } catch (...) {
        return path;
    }
}

static bool IsExecutableFile(const std::filesystem::path &p)
{
    std::error_code ec;
    return std::filesystem::exists(p, ec) &&
           std::filesystem::is_regular_file(p, ec);
}

static std::string FindExecutable(const std::vector<std::string> &candidates)
{
    const char *path_env = std::getenv("PATH");
    if (!path_env) {
        return candidates.empty() ? std::string() : candidates.front();
    }

#if defined(_WIN32)
    constexpr char kSep = ';';
    const char *extensions[] = {"", ".exe", ".bat", ".cmd"};
#else
    constexpr char kSep = ':';
    const char *extensions[] = {""};
#endif

    std::string path_list(path_env);
    size_t begin = 0;
    while (begin <= path_list.size()) {
        size_t end = path_list.find(kSep, begin);
        std::string dir = path_list.substr(
            begin, end == std::string::npos ? std::string::npos : end - begin);
        if (dir.empty()) {
            dir = ".";
        }
        for (const auto &candidate : candidates) {
            for (const char *ext : extensions) {
                std::filesystem::path p = std::filesystem::path(dir) /
                                          (candidate + ext);
                if (IsExecutableFile(p)) {
                    return p.string();
                }
            }
        }
        if (end == std::string::npos) {
            break;
        }
        begin = end + 1;
    }

    return candidates.empty() ? std::string() : candidates.front();
}

class ScriptConsole {
public:
    explicit ScriptConsole(ScriptLanguage language)
        : m_language(language)
    {
        memset(m_interpreter, 0, sizeof(m_interpreter));
        memset(m_command, 0, sizeof(m_command));
    }

    ~ScriptConsole()
    {
        StopAll(true);
    }

    void Open()
    {
        m_open = true;
        EnsureInterpreter();
    }

    bool IsOpen() const
    {
        return m_open;
    }

    bool NeedsFrameService() const
    {
        return m_open || m_running_count > 0 || m_overlay_count > 0;
    }

    void Service()
    {
        /* Script IPC must keep running even when the console window is hidden.
         * With no active child process this is an immediate return. */
        if (AnyRunning()) {
            PollProcesses();
        }
    }

    void Draw()
    {
        if (!m_open) {
            return;
        }

        EnsureInterpreter();

        ImGui::SetNextWindowSize(ImVec2(820, 520), ImGuiCond_FirstUseEver);
        const char *title = IsLua() ? "Lua Console" : "Python Console";
        if (!ImGui::Begin(title, &m_open, ImGuiWindowFlags_MenuBar)) {
            ImGui::End();
            return;
        }

        DrawMenuBar();
        DrawToolbar();
        ImGui::Separator();

        const float footer_height = ImGui::GetFrameHeightWithSpacing() * 2.0f +
                                    ImGui::GetStyle().ItemSpacing.y;
        const float content_height = std::max(150.0f,
            ImGui::GetContentRegionAvail().y - footer_height);

        ImGui::BeginChild("##script_list", ImVec2(310, content_height), true);
        DrawScriptList();
        ImGui::EndChild();

        ImGui::SameLine();

        ImGui::BeginChild("##script_output", ImVec2(0, content_height), true,
                          ImGuiWindowFlags_HorizontalScrollbar);
        DrawOutput();
        ImGui::EndChild();

        DrawCommandLine();
        DrawStatusLine();
        ImGui::End();
    }

    void DrawOverlays()
    {
        if (m_overlay_count == 0) {
            return;
        }
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        for (const auto &entry : m_scripts) {
            for (const auto &o : entry.overlays) {
                dl->AddText(ImVec2(o.x, o.y), IM_COL32(255,255,255,255), o.text.c_str());
            }
        }
    }

private:
    bool IsLua() const
    {
        return m_language == ScriptLanguage::Lua;
    }

    const char *Extension() const
    {
        return IsLua() ? "lua" : "py";
    }

    const char *LanguageName() const
    {
        return IsLua() ? "Lua" : "Python";
    }

    void EnsureInterpreter()
    {
        if (m_interpreter_initialized) {
            return;
        }
        m_interpreter_initialized = true;

        std::string executable;
        if (IsLua()) {
            executable = FindExecutable({"lua", "lua5.4", "lua54",
                                         "lua5.3", "luajit"});
        } else {
#if defined(_WIN32)
            executable = FindExecutable({"python3", "python"});
#else
            executable = FindExecutable({"python3", "python"});
#endif
        }
        SDL_strlcpy(m_interpreter, executable.c_str(), sizeof(m_interpreter));
    }

    std::string DefaultScriptDirectory() const
    {
        std::filesystem::path p(xemu_settings_get_base_path());
        p /= "scripts";
        p /= IsLua() ? "lua" : "python";
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p.string();
    }

    std::string ApiDirectory() const
    {
        std::filesystem::path p(xemu_settings_get_base_path());
        p /= "scripts";
        p /= "api";
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p.string();
    }

    static std::string PythonQuote(const std::string &v)
    {
        std::string out = "'";
        for (char c : v) {
            if (c == '\\' || c == '\'') out.push_back('\\');
            out.push_back(c);
        }
        out.push_back('\'');
        return out;
    }

    static std::string LuaQuote(const std::string &v)
    {
        std::string out = "\"";
        for (char c : v) {
            if (c == '\\' || c == '"') out.push_back('\\');
            if (c == '\n') { out += "\\n"; continue; }
            out.push_back(c);
        }
        out.push_back('"');
        return out;
    }

    void EnsureApiHelpers()
    {
        std::filesystem::path dir(ApiDirectory());
        {
            std::ofstream py(dir / "xemu.py", std::ios::binary | std::ios::trunc);
            py << R"PY(# Auto-generated by xemu's Python Console.
import sys, itertools, time
_seq = itertools.count(1)
def _call(cmd, *args):
    i = next(_seq)
    payload = "|".join(str(a) for a in args)
    print(f"@@XEMUAPI|{i}|{cmd}|{payload}", flush=True)
    while True:
        line = sys.stdin.readline()
        if line == "": raise RuntimeError("xemu API connection closed")
        line = line.rstrip("\r\n")
        p = line.split("|", 3)
        if len(p) >= 3 and p[0] == "@@XEMURESP" and p[1] == str(i):
            data = p[3] if len(p) > 3 else ""
            if p[2] != "OK": raise RuntimeError(data)
            return data

def frame(): return int(_call("frame"))
def lag_count(): return int(_call("lag_count"))
def title_id(): return _call("title_id")
def tas_enabled(): return _call("tas_enabled") == "1"
def set_tas_enabled(v=True): _call("set_tas_enabled", 1 if v else 0)
def pause(): _call("pause")
def resume(): _call("resume")
def frame_advance(n=1): _call("frame_advance", int(n))
def wait_frame(target=None):
    if target is None: target = frame() + 1
    return int(_call("wait_frame", int(target)))
def on_frame(callback, count=None):
    n = 0
    while count is None or n < count:
        f = wait_frame()
        callback(f)
        n += 1

def read_u8(addr): return int(_call("read8", int(addr,0) if isinstance(addr,str) else int(addr)), 0)
def read_u16(addr): return int(_call("read16", int(addr,0) if isinstance(addr,str) else int(addr)), 0)
def read_u32(addr): return int(_call("read32", int(addr,0) if isinstance(addr,str) else int(addr)), 0)
def write_u8(addr,v): _call("write8", int(addr,0) if isinstance(addr,str) else int(addr), int(v))
def write_u16(addr,v): _call("write16", int(addr,0) if isinstance(addr,str) else int(addr), int(v))
def write_u32(addr,v): _call("write32", int(addr,0) if isinstance(addr,str) else int(addr), int(v))
def input_get(port=1): return bytes.fromhex(_call("input_get", int(port)-1))
def input_set(data, port=1):
    if isinstance(data,(bytes,bytearray)): data=data.hex()
    _call("input_set", int(port)-1, str(data))
def snapshot_save(name): _call("snapshot_save", str(name).encode().hex())
def snapshot_load(name): _call("snapshot_load", str(name).encode().hex())
def notify(text): _call("notify", str(text).encode().hex())
def overlay_text(x,y,text): _call("overlay_text", float(x), float(y), str(text).encode().hex())
def overlay_clear(): _call("overlay_clear")
)PY";
        }
        {
            std::ofstream lua(dir / "xemu.lua", std::ios::binary | std::ios::trunc);
            lua << R"LUA(-- Auto-generated by xemu's Lua Console.
local M = {}
local seq = 0
local function call(cmd, ...)
  seq = seq + 1
  local args = {...}
  local t = {}
  for i,v in ipairs(args) do t[#t+1] = tostring(v) end
  io.write("@@XEMUAPI|"..seq.."|"..cmd.."|"..table.concat(t,"|").."\n")
  io.flush()
  while true do
    local line = io.read("*l")
    if not line then error("xemu API connection closed") end
    local id,status,data = line:match("^@@XEMURESP|(%d+)|([^|]+)|?(.*)$")
    if id and tonumber(id)==seq then
      if status ~= "OK" then error(data) end
      return data or ""
    end
  end
end
local function hex(s) return (s:gsub('.', function(c) return string.format('%02x', string.byte(c)) end)) end
function M.frame() return tonumber(call("frame")) end
function M.lag_count() return tonumber(call("lag_count")) end
function M.title_id() return call("title_id") end
function M.tas_enabled() return call("tas_enabled")=="1" end
function M.set_tas_enabled(v) call("set_tas_enabled",v and 1 or 0) end
function M.pause() call("pause") end
function M.resume() call("resume") end
function M.frame_advance(n) call("frame_advance",n or 1) end
function M.wait_frame(target) return tonumber(call("wait_frame",target or (M.frame()+1))) end
function M.on_frame(fn,count) local n=0 while not count or n<count do fn(M.wait_frame());n=n+1 end end
function M.read_u8(a) return tonumber(call("read8",a)) end
function M.read_u16(a) return tonumber(call("read16",a)) end
function M.read_u32(a) return tonumber(call("read32",a)) end
function M.write_u8(a,v) call("write8",a,v) end
function M.write_u16(a,v) call("write16",a,v) end
function M.write_u32(a,v) call("write32",a,v) end
function M.input_get(port) return call("input_get",(port or 1)-1) end
function M.input_set(hexreport,port) call("input_set",(port or 1)-1,hexreport) end
function M.snapshot_save(name) call("snapshot_save",hex(name)) end
function M.snapshot_load(name) call("snapshot_load",hex(name)) end
function M.notify(text) call("notify",hex(text)) end
function M.overlay_text(x,y,text) call("overlay_text",x,y,hex(text)) end
function M.overlay_clear() call("overlay_clear") end
return M
)LUA";
        }
    }

    void DrawMenuBar()
    {
        if (!ImGui::BeginMenuBar()) {
            return;
        }
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Add Script...")) {
                AddScriptDialog();
            }
            if (ImGui::MenuItem("Remove Selected", nullptr, false,
                                HasSelection())) {
                RemoveSelected();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Close")) {
                m_open = false;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Script")) {
            if (ImGui::MenuItem("Run / Reload Selected", nullptr, false,
                                HasSelection())) {
                RunSelected();
            }
            if (ImGui::MenuItem("Stop Selected", nullptr, false,
                                SelectedIsRunning())) {
                StopSelected();
            }
            if (ImGui::MenuItem("Run All", nullptr, false,
                                !m_scripts.empty())) {
                for (int i = 0; i < (int)m_scripts.size(); ++i) {
                    RunScript(i);
                }
            }
            if (ImGui::MenuItem("Stop All", nullptr, false,
                                AnyRunning())) {
                StopAll(false);
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Settings")) {
            ImGui::TextUnformatted("Interpreter executable");
            ImGui::SetNextItemWidth(360.0f);
            ImGui::InputText("##interpreter", m_interpreter,
                             sizeof(m_interpreter));
            ImGui::Checkbox("Auto-scroll output", &m_auto_scroll);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            ImGui::TextWrapped(
                "%s scripts run asynchronously in a child interpreter so "
                "emulation is never blocked. stdout/stderr and an interactive "
                "stdin console are connected live.", LanguageName());
            ImGui::Separator();
            ImGui::TextWrapped(
                "Emulator API bridge is active. Python: import xemu. Lua: local xemu = require('xemu'). APIs include memory read/write, TAS frame/lag, frame advance/wait_frame, controller XID, snapshots, notifications, and overlay text.");
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    void DrawToolbar()
    {
        if (ImGui::Button("Add Script...")) {
            AddScriptDialog();
        }
        ImGui::SameLine();
        if (!HasSelection()) ImGui::BeginDisabled();
        if (ImGui::Button("Run / Reload")) {
            RunSelected();
        }
        ImGui::SameLine();
        if (!SelectedIsRunning()) ImGui::BeginDisabled();
        if (ImGui::Button("Stop")) {
            StopSelected();
        }
        if (!SelectedIsRunning()) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Remove")) {
            RemoveSelected();
        }
        if (!HasSelection()) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Clear Output")) {
            m_output.clear();
        }
        ImGui::SameLine();
        ImGui::Checkbox("Auto-scroll", &m_auto_scroll);
    }

    void DrawScriptList()
    {
        ImGui::Text("%zu script%s", m_scripts.size(),
                    m_scripts.size() == 1 ? "" : "s");
        ImGui::Separator();

        if (ImGui::BeginTable("##scripts", 3,
            ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable,
            ImVec2(0, 0))) {
            ImGui::TableSetupColumn("Script", ImGuiTableColumnFlags_WidthFixed,
                                    115.0f);
            ImGui::TableSetupColumn("Status", ImGuiTableColumnFlags_WidthFixed,
                                    62.0f);
            ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (int i = 0; i < (int)m_scripts.size(); ++i) {
                ScriptEntry &entry = m_scripts[i];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                std::string label = Basename(entry.path) + "##script_" +
                                    std::to_string(i);
                if (ImGui::Selectable(label.c_str(), m_selected == i,
                                      ImGuiSelectableFlags_SpanAllColumns)) {
                    m_selected = i;
                }
                ImGui::TableSetColumnIndex(1);
                if (entry.running) {
                    ImGui::TextUnformatted("Running");
                } else {
                    ImGui::Text("Exit %d", entry.last_exit_code);
                }
                ImGui::TableSetColumnIndex(2);
                ImGui::TextUnformatted(entry.path.c_str());
            }
            ImGui::EndTable();
        }
    }

    void DrawOutput()
    {
        if (m_output.empty()) {
            ImGui::TextDisabled("Output from %s scripts appears here.",
                                LanguageName());
        } else {
            ImGui::TextUnformatted(m_output.c_str(),
                                   m_output.c_str() + m_output.size());
        }
        if (m_scroll_to_bottom && m_auto_scroll) {
            ImGui::SetScrollHereY(1.0f);
            m_scroll_to_bottom = false;
        }
    }

    void DrawCommandLine()
    {
        const bool can_send = SelectedIsRunning();
        if (!can_send) ImGui::BeginDisabled();
        ImGui::SetNextItemWidth(-70.0f);
        bool enter = ImGui::InputText("##command", m_command,
                                     sizeof(m_command),
                                     ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ImGui::Button("Send") || enter) {
            SendCommand();
        }
        if (!can_send) ImGui::EndDisabled();
    }

    void DrawStatusLine()
    {
        ImGui::TextDisabled("%s interpreter: %s", LanguageName(),
                            m_interpreter[0] ? m_interpreter : "<not set>");
    }

    bool HasSelection() const
    {
        return m_selected >= 0 && m_selected < (int)m_scripts.size();
    }

    bool SelectedIsRunning() const
    {
        return HasSelection() && m_scripts[m_selected].running;
    }

    bool AnyRunning() const
    {
        return m_running_count > 0;
    }

    void AddScriptDialog()
    {
        SDL_DialogFileFilter filters[] = {
            {IsLua() ? "Lua scripts" : "Python scripts", Extension()},
            {"All files", "*"},
        };
        std::string start = DefaultScriptDirectory();
        ShowOpenFileDialog(filters, 2, start.c_str(),
            [this](const char *path) {
                if (!path || !*path) return;
                auto it = std::find_if(m_scripts.begin(), m_scripts.end(),
                    [path](const ScriptEntry &e) { return e.path == path; });
                if (it != m_scripts.end()) {
                    m_selected = (int)std::distance(m_scripts.begin(), it);
                    return;
                }
                ScriptEntry e;
                e.path = path;
                m_scripts.push_back(std::move(e));
                m_selected = (int)m_scripts.size() - 1;
                AppendOutput("[console] added " + std::string(path) + "\n");
            });
    }

    void RemoveSelected()
    {
        if (!HasSelection()) return;
        StopScript(m_selected, true);
        m_scripts.erase(m_scripts.begin() + m_selected);
        if (m_scripts.empty()) {
            m_selected = -1;
        } else if (m_selected >= (int)m_scripts.size()) {
            m_selected = (int)m_scripts.size() - 1;
        }
    }

    void RunSelected()
    {
        if (HasSelection()) {
            RunScript(m_selected);
        }
    }

    void StopSelected()
    {
        if (HasSelection()) {
            StopScript(m_selected, false);
        }
    }

    void RunScript(int index)
    {
        if (index < 0 || index >= (int)m_scripts.size()) return;
        ScriptEntry &entry = m_scripts[index];
        StopScript(index, true);

        if (!m_interpreter[0]) {
            AppendOutput("[console] no interpreter configured\n");
            return;
        }
        if (!std::filesystem::exists(entry.path)) {
            AppendOutput("[console] script not found: " + entry.path + "\n");
            return;
        }

        EnsureApiHelpers();
        std::vector<std::string> owned_args;
        owned_args.emplace_back(m_interpreter);
        if (IsLua()) {
            std::string code = "io.stdout:setvbuf('no'); io.stderr:setvbuf('no'); package.path=" +
                               LuaQuote((std::filesystem::path(ApiDirectory()) / "?.lua").string() + ";") +
                               "..package.path; dofile(" + LuaQuote(entry.path) + ")";
            owned_args.emplace_back("-e");
            owned_args.emplace_back(code);
            owned_args.emplace_back("-i");
        } else {
            std::string api_dir = ApiDirectory();
            std::string code = "import sys; sys.path.insert(0," + PythonQuote(api_dir) +
                               "); _p=" + PythonQuote(entry.path) +
                               "; exec(compile(open(_p,'rb').read(),_p,'exec'), {'__name__':'__main__','__file__':_p})";
            owned_args.emplace_back("-u");
            owned_args.emplace_back("-i");
            owned_args.emplace_back("-c");
            owned_args.emplace_back(code);
        }

        std::vector<const char *> args;
        args.reserve(owned_args.size() + 1);
        for (const auto &arg : owned_args) args.push_back(arg.c_str());
        args.push_back(nullptr);

        SDL_PropertiesID props = SDL_CreateProperties();
        if (!props) {
            AppendOutput("[console] SDL_CreateProperties failed: " +
                         std::string(SDL_GetError()) + "\n");
            return;
        }
        SDL_SetPointerProperty(props, SDL_PROP_PROCESS_CREATE_ARGS_POINTER,
                               const_cast<char **>(args.data()));
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDIN_NUMBER,
                              SDL_PROCESS_STDIO_APP);
        SDL_SetNumberProperty(props, SDL_PROP_PROCESS_CREATE_STDOUT_NUMBER,
                              SDL_PROCESS_STDIO_APP);
        SDL_SetBooleanProperty(
            props, SDL_PROP_PROCESS_CREATE_STDERR_TO_STDOUT_BOOLEAN, true);
        try {
            std::filesystem::path p(entry.path);
            std::string cwd = p.parent_path().string();
            if (!cwd.empty()) {
                SDL_SetStringProperty(
                    props, SDL_PROP_PROCESS_CREATE_WORKING_DIRECTORY_STRING,
                    cwd.c_str());
            }
        } catch (...) {
        }

        entry.process = SDL_CreateProcessWithProperties(props);
        SDL_DestroyProperties(props);
        if (!entry.process) {
            AppendOutput("[console] failed to start " +
                         std::string(m_interpreter) + ": " +
                         SDL_GetError() + "\n");
            return;
        }
        entry.running = true;
        ++m_running_count;
        entry.last_exit_code = 0;
        entry.pending_output.clear();
        entry.wait_pending = false;
        if (m_overlay_count >= entry.overlays.size()) {
            m_overlay_count -= entry.overlays.size();
        } else {
            m_overlay_count = 0;
        }
        entry.overlays.clear();
        m_selected = index;
        AppendOutput("\n[run] " + Basename(entry.path) + "\n");
    }

    void StopScript(int index, bool quiet)
    {
        if (index < 0 || index >= (int)m_scripts.size()) return;
        ScriptEntry &entry = m_scripts[index];
        if (!entry.process) {
            if (entry.running && m_running_count > 0) --m_running_count;
            entry.running = false;
            return;
        }
        if (entry.running) {
            SDL_KillProcess(entry.process, true);
            if (!quiet) {
                AppendOutput("[console] stopped " + Basename(entry.path) +
                             "\n");
            }
        }
        int exitcode = 0;
        SDL_WaitProcess(entry.process, true, &exitcode);
        entry.last_exit_code = exitcode;
        SDL_DestroyProcess(entry.process);
        entry.process = nullptr;
        if (entry.running && m_running_count > 0) --m_running_count;
        entry.running = false;
        entry.wait_pending = false;
        entry.pending_output.clear();
        if (m_overlay_count >= entry.overlays.size()) {
            m_overlay_count -= entry.overlays.size();
        } else {
            m_overlay_count = 0;
        }
        entry.overlays.clear();
    }

    void StopAll(bool quiet)
    {
        for (int i = 0; i < (int)m_scripts.size(); ++i) {
            StopScript(i, quiet);
        }
    }

    static std::vector<std::string> SplitPipe(const std::string &line)
    {
        std::vector<std::string> out;
        size_t start = 0;
        while (true) {
            size_t p = line.find('|', start);
            if (p == std::string::npos) { out.push_back(line.substr(start)); break; }
            out.push_back(line.substr(start, p-start));
            start = p + 1;
        }
        return out;
    }

    static bool DecodeHex(const std::string &hex, std::vector<uint8_t> *out)
    {
        if (!out || (hex.size() & 1)) return false;
        out->clear(); out->reserve(hex.size()/2);
        for (size_t i=0;i<hex.size();i+=2) {
            char t[3] = {hex[i], hex[i+1], 0}; char *e=nullptr; long v=strtol(t,&e,16);
            if (!e || *e) return false; out->push_back((uint8_t)v);
        }
        return true;
    }

    static std::string EncodeHex(const void *data, size_t size)
    {
        const uint8_t *p = static_cast<const uint8_t *>(data);
        std::ostringstream ss; ss << std::hex << std::setfill('0');
        for (size_t i=0;i<size;++i) ss << std::setw(2) << (unsigned)p[i];
        return ss.str();
    }

    void SendApiResponse(ScriptEntry &entry, uint64_t id, bool ok, const std::string &payload)
    {
        if (!entry.process) return;
        SDL_IOStream *in = SDL_GetProcessInput(entry.process);
        if (!in) return;
        std::string line = "@@XEMURESP|" + std::to_string(id) + "|" +
                           (ok ? "OK|" : "ERR|") + payload + "\n";
        SDL_WriteIO(in, line.data(), line.size());
        SDL_FlushIO(in);
    }

    void HandleApiRequest(ScriptEntry &entry, const std::string &line)
    {
        auto p = SplitPipe(line);
        if (p.size() < 3 || p[0] != "@@XEMUAPI") return;
        uint64_t id = strtoull(p[1].c_str(), nullptr, 10);
        const std::string &cmd = p[2];
        auto arg = [&](size_t i)->std::string { return i+3 < p.size() ? p[i+3] : std::string(); };
        auto num = [&](size_t i)->uint64_t { return strtoull(arg(i).c_str(), nullptr, 0); };
        bool ok = true; std::string out;

        if (cmd == "frame") out = std::to_string(xemu_tas_frame());
        else if (cmd == "lag_count") out = std::to_string(xemu_tas_lag_count());
        else if (cmd == "tas_enabled") out = xemu_tas_enabled() ? "1" : "0";
        else if (cmd == "set_tas_enabled") xemu_tas_set_enabled(num(0) != 0);
        else if (cmd == "pause") { if (runstate_is_running()) vm_stop(RUN_STATE_PAUSED); }
        else if (cmd == "resume") { if (!runstate_is_running()) vm_start(); }
        else if (cmd == "frame_advance") {
            uint32_t n = (uint32_t)std::max<uint64_t>(1, std::min<uint64_t>(num(0), 1000000));
            if (runstate_is_running()) vm_stop(RUN_STATE_PAUSED);
            if (!xemu_tas_enabled()) xemu_tas_set_enabled(true);
            xemu_tas_request_frame_advance(n); vm_start();
        }
        else if (cmd == "wait_frame") {
            uint64_t target = num(0);
            if (xemu_tas_frame() >= target) out = std::to_string(xemu_tas_frame());
            else { entry.wait_pending = true; entry.wait_id = id; entry.wait_target = target; return; }
        }
        else if (cmd == "title_id") {
            uint32_t title_id = 0;
            if (xemu_get_xbe_title_id(&title_id)) {
                char t[16];
                snprintf(t, sizeof(t), "%08X", title_id);
                out = t;
            } else {
                ok = false;
                out = "no running XBE";
            }
        }
        else if (cmd == "read8" || cmd == "read16" || cmd == "read32") {
            int sz = cmd=="read8"?1:cmd=="read16"?2:4; uint64_t v=0;
            if (xemu_phys_read((uint32_t)num(0), &v, sz) != sz) { ok=false; out="memory read failed"; }
            else out = std::to_string(v);
        }
        else if (cmd == "write8" || cmd == "write16" || cmd == "write32") {
            int sz = cmd=="write8"?1:cmd=="write16"?2:4; uint64_t v=num(1);
            if (xemu_phys_write((uint32_t)num(0), &v, sz) != sz) { ok=false; out="memory write failed"; }
        }
        else if (cmd == "input_get") {
            uint8_t port=(uint8_t)num(0), r[XEMU_TAS_XID_REPORT_SIZE]{};
            if (!xemu_tas_get_last_xid_report(port,r,sizeof(r))) { ok=false; out="no XID report yet"; }
            else out=EncodeHex(r,sizeof(r));
        }
        else if (cmd == "input_set") {
            uint8_t port=(uint8_t)num(0); std::vector<uint8_t> r;
            if (port>=XEMU_TAS_MAX_PORTS || !DecodeHex(arg(1),&r) || r.size()!=XEMU_TAS_XID_REPORT_SIZE) { ok=false; out="expected port 0-3 and 40 hex chars"; }
            else { if(!xemu_tas_enabled())xemu_tas_set_enabled(true);xemu_tas_set_xid_report(port,r.data(),r.size()); }
        }
        else if (cmd == "snapshot_save" || cmd == "snapshot_load" || cmd == "notify" || cmd == "overlay_text") {
            if (cmd == "overlay_text") {
                if (p.size() < 6) { ok=false; out="overlay_text requires x,y,text"; }
                else { std::vector<uint8_t> text; if(!DecodeHex(arg(2),&text)){ok=false;out="bad text encoding";} else { ScriptOverlayText o; o.x=(float)atof(arg(0).c_str());o.y=(float)atof(arg(1).c_str());o.text.assign((const char*)text.data(),text.size());entry.overlays.push_back(std::move(o)); ++m_overlay_count; } }
            } else {
                std::vector<uint8_t> bytes; if(!DecodeHex(arg(0),&bytes)){ok=false;out="bad text encoding";} else { std::string text((const char*)bytes.data(),bytes.size()); if(cmd=="notify")xemu_queue_notification(text.c_str()); else { Error *err=nullptr; if(cmd=="snapshot_save")xemu_snapshots_save(text.c_str(),&err); else xemu_snapshots_load(text.c_str(),&err); if(err){ok=false;out=error_get_pretty(err);error_free(err);} } }
            }
        }
        else if (cmd == "overlay_clear") {
            if (m_overlay_count >= entry.overlays.size()) {
                m_overlay_count -= entry.overlays.size();
            } else {
                m_overlay_count = 0;
            }
            entry.overlays.clear();
        }
        else { ok=false; out="unknown xemu API command: "+cmd; }

        SendApiResponse(entry,id,ok,out);
    }

    void ProcessOutputChunk(ScriptEntry &entry, const char *data, size_t n)
    {
        entry.pending_output.append(data, n);
        size_t consumed = 0;
        while (true) {
            const size_t nl = entry.pending_output.find('\n', consumed);
            if (nl == std::string::npos) break;
            std::string line = entry.pending_output.substr(consumed, nl - consumed);
            consumed = nl + 1;
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("@@XEMUAPI|", 0) == 0) HandleApiRequest(entry, line);
            else AppendOutput(line + "\n");
        }
        if (consumed) {
            entry.pending_output.erase(0, consumed);
        }
        /* A script that writes an enormous unterminated line must not grow the
         * pending buffer without bound. Flush it as ordinary output in chunks;
         * API requests are line-oriented and intentionally stay below this. */
        constexpr size_t kMaxPendingLine = 256 * 1024;
        if (entry.pending_output.size() > kMaxPendingLine) {
            AppendOutput(entry.pending_output.substr(0, kMaxPendingLine));
            AppendOutput("\n[console] output line truncated/broken for safety\n");
            entry.pending_output.erase(0, kMaxPendingLine);
        }
    }

    void PollProcesses()
    {
        /* A runaway print loop must never monopolize Xemu's UI/emulation
         * service. Limit how much child output one console drains per host
         * frame; the rest remains buffered by the OS for the next tick. */
        constexpr size_t kTotalReadBudget = 64 * 1024;
        constexpr size_t kPerScriptReadBudget = 16 * 1024;
        std::array<char, 4096> buffer{};
        size_t total_budget = kTotalReadBudget;

        for (auto &entry : m_scripts) {
            if (!entry.process) continue;

            const uint64_t tas_frame = xemu_tas_frame();
            if (entry.wait_pending && tas_frame >= entry.wait_target) {
                entry.wait_pending = false;
                SendApiResponse(entry, entry.wait_id, true, std::to_string(tas_frame));
            }

            SDL_IOStream *out = SDL_GetProcessOutput(entry.process);
            if (out && total_budget) {
                size_t script_budget = std::min(kPerScriptReadBudget, total_budget);
                while (script_budget) {
                    const size_t request = std::min(buffer.size(), script_budget);
                    const size_t n = SDL_ReadIO(out, buffer.data(), request);
                    if (!n) break;
                    ProcessOutputChunk(entry, buffer.data(), n);
                    script_budget -= n;
                    total_budget -= n;
                    if (!total_budget) break;
                }
            }

            int exitcode = 0;
            if (SDL_WaitProcess(entry.process, false, &exitcode)) {
                if (!entry.pending_output.empty()) {
                    AppendOutput(entry.pending_output);
                    entry.pending_output.clear();
                }
                AppendOutput("\n[exit " + std::to_string(exitcode) + "] " +
                             Basename(entry.path) + "\n");
                entry.last_exit_code = exitcode;
                SDL_DestroyProcess(entry.process);
                entry.process = nullptr;
                if (entry.running && m_running_count > 0) --m_running_count;
                entry.running = false;
                entry.wait_pending = false;
            }
        }
    }

    void SendCommand()
    {
        if (!SelectedIsRunning() || !m_command[0]) return;
        ScriptEntry &entry = m_scripts[m_selected];
        SDL_IOStream *in = SDL_GetProcessInput(entry.process);
        if (!in) {
            AppendOutput("[console] interpreter stdin is unavailable\n");
            return;
        }
        std::string line(m_command);
        line.push_back('\n');
        size_t written = SDL_WriteIO(in, line.data(), line.size());
        SDL_FlushIO(in);
        if (written != line.size()) {
            AppendOutput("[console] could not write complete command\n");
        }
        m_command[0] = '\0';
    }

    void AppendOutput(const std::string &text)
    {
        constexpr size_t kMaxOutput = 2 * 1024 * 1024;
        constexpr size_t kTrimmedOutput = 1536 * 1024;
        m_output += text;
        if (m_output.size() > kMaxOutput) {
            /* Trim in large batches instead of memmoving ~2 MiB for every new
             * line once the cap has been reached. Prefer a line boundary. */
            size_t cut = m_output.size() - kTrimmedOutput;
            const size_t newline = m_output.find('\n', cut);
            if (newline != std::string::npos) {
                cut = newline + 1;
            }
            m_output.erase(0, cut);
        }
        m_scroll_to_bottom = true;
    }

private:
    ScriptLanguage m_language;
    bool m_open = false;
    bool m_auto_scroll = true;
    bool m_scroll_to_bottom = false;
    bool m_interpreter_initialized = false;
    int m_selected = -1;
    int m_running_count = 0;
    size_t m_overlay_count = 0;
    char m_interpreter[1024];
    char m_command[2048];
    std::vector<ScriptEntry> m_scripts;
    std::string m_output;
};

static ScriptConsole g_lua_console(ScriptLanguage::Lua);
static ScriptConsole g_python_console(ScriptLanguage::Python);

} // namespace

void ShowLuaConsole()
{
    g_lua_console.Open();
}

void ShowPythonConsole()
{
    g_python_console.Open();
}

void ShowScriptConsoleWindows()
{
    /* Compiled-in scripting should disappear from the normal frontend path
     * when both consoles are closed, no child is running, and no overlay is
     * active. */
    if (!g_lua_console.NeedsFrameService() &&
        !g_python_console.NeedsFrameService()) {
        return;
    }

    g_lua_console.Service();
    g_python_console.Service();
    g_lua_console.Draw();
    g_python_console.Draw();
    g_lua_console.DrawOverlays();
    g_python_console.DrawOverlays();
}

bool ScriptConsoleWindowsOpen()
{
    return g_lua_console.IsOpen() || g_python_console.IsOpen();
}
