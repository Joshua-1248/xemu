// SPDX-License-Identifier: GPL-2.0-or-later
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
#include "xemu-features/shared/detachable-windows.hh"
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
#ifdef CONFIG_XEMU_FEATURE_DEBUG_TOOLS
#include "xemu-features/debug-tools/debug-api.h"
#endif
#include "ui/thirdparty/stb_image/stb_image.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>
#include <unordered_map>
#include <functional>
#include <cmath>
#include <limits>
// QEMU maps close() to qemu_close_wrap() on Windows.  Keep that macro out
// of libstdc++'s <fstream>, where it would otherwise rename
// std::basic_filebuf::close() and produce release/LTO link failures.
#ifdef _WIN32
#pragma push_macro("close")
#undef close
#endif
#include <fstream>
#ifdef _WIN32
#pragma pop_macro("close")
#endif
#include <sstream>
#include <iomanip>
#include <cstdint>
#include <cinttypes>
#include <cfloat>

namespace {

enum class ScriptLanguage {
    Lua,
    Python,
};

struct ScriptOverlayText {
    std::string id;
    float x = 0.0f;
    float y = 0.0f;
    std::string text;
    ImU32 color = IM_COL32(255, 255, 255, 255);
    float scale = 1.0f;
    ImU32 background = IM_COL32(0, 0, 0, 0);
};

enum class ScriptPrimitiveType {
    Line,
    Rect,
    Circle,
    Bar,
    Crosshair,
    Image,
};

struct ScriptPrimitive {
    std::string id;
    ScriptPrimitiveType type = ScriptPrimitiveType::Line;
    float x = 0.0f;
    float y = 0.0f;
    float x2 = 0.0f;
    float y2 = 0.0f;
    float w = 0.0f;
    float h = 0.0f;
    float radius = 0.0f;
    float thickness = 1.0f;
    float rounding = 0.0f;
    float value = 0.0f;
    float min_value = 0.0f;
    float max_value = 100.0f;
    float gap = 3.0f;
    bool filled = false;
    bool vertical = false;
    ImU32 color = IM_COL32(255, 255, 255, 255);
    ImU32 color2 = IM_COL32(64, 200, 255, 255);
    ImU32 background = IM_COL32(0, 0, 0, 160);
    ImU32 border = IM_COL32(255, 255, 255, 160);
    std::string image_path;
    GLuint texture = 0;
    int image_w = 0;
    int image_h = 0;
};

enum class ScriptWatchKind {
    Text,
    Bar,
};

/* Parsed once when a watch is created.  Keeping the hot sampling path on an
 * enum avoids repeating half a dozen std::string comparisons every frame. */
enum class ScriptWatchValueType : uint8_t {
    U8, I8, Hex8,
    U16, I16, Hex16,
    U32, I32, Hex32,
    F32, F64,
};

struct ScriptMemoryWatch {
    std::string id;
    // Stable internal draw-object id, built once when the watch is registered.
    std::string visual_id;
    ScriptWatchKind kind = ScriptWatchKind::Text;
    uint32_t addr = 0;
    bool virt = true;
    std::string value_type = "u32";
    ScriptWatchValueType parsed_type = ScriptWatchValueType::U32;
    bool external = false;
    float x = 0.0f;
    float y = 0.0f;
    float w = 180.0f;
    float h = 16.0f;
    float min_value = 0.0f;
    float max_value = 100.0f;
    std::string prefix;
    std::string suffix;
    ImU32 color = IM_COL32(255,255,255,255);
    ImU32 background = IM_COL32(0,0,0,128);
    ImU32 bar_color = IM_COL32(70,200,100,255);
    ImU32 border = IM_COL32(255,255,255,120);
    float scale = 1.0f;

    // Display text is rebuilt only when the sampled value/mapping state
    // changes.  This matters for stable HUD values watched at 60+ Hz.
    bool have_cached_sample = false;
    bool visual_initialized = false;
    bool cached_mapped = false;
    double cached_value = 0.0;
};

struct ScriptControllerWidget {
    std::string id;
    uint8_t port = 0;
    bool external = false;
    float x = 0.0f;
    float y = 0.0f;
    float scale = 1.0f;
    bool labels = true;
};

enum class ScriptWaitKind {
    None,
    Frame,
    Debug,
    Runstate,
    Title,
};

struct ScriptEntry {
    std::string path;
    SDL_Process *process = nullptr;
    bool running = false;
    int last_exit_code = 0;
    std::string pending_output;
    ScriptWaitKind wait_kind = ScriptWaitKind::None;
    uint64_t wait_id = 0;
    uint64_t wait_target = 0;
    uint64_t wait_debug_sequence = 0;
    int wait_runstate = -1;
    std::string wait_title;
    std::vector<ScriptOverlayText> overlays;
    std::vector<ScriptOverlayText> external_overlays;
    std::vector<ScriptPrimitive> primitives;
    std::vector<ScriptPrimitive> external_primitives;
    std::vector<ScriptMemoryWatch> watches;
    std::vector<ScriptControllerWidget> controllers;
};

static std::string Basename(const std::string &path)
{
    try {
        return std::filesystem::path(path).filename().string();
    } catch (...) {
        return path;
    }
}

static int HexDigit(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return 10 + c - 'a';
    if (c >= 'A' && c <= 'F') return 10 + c - 'A';
    return -1;
}

static bool ParseRgbaColor(const std::string &value, ImU32 *out)
{
    if (!out) {
        return false;
    }

    const char *p = value.data();
    size_t n = value.size();
    if (n && p[0] == '#') {
        ++p;
        --n;
    } else if (n > 2 && p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
        n -= 2;
    }
    if (n != 6 && n != 8) {
        return false;
    }

    uint32_t v = 0;
    for (size_t i = 0; i < n; ++i) {
        const int digit = HexDigit(p[i]);
        if (digit < 0) {
            return false;
        }
        v = (v << 4) | (uint32_t)digit;
    }

    uint8_t r, g, b, a = 255;
    if (n == 6) {
        r = (uint8_t)(v >> 16);
        g = (uint8_t)(v >> 8);
        b = (uint8_t)v;
    } else {
        r = (uint8_t)(v >> 24);
        g = (uint8_t)(v >> 16);
        b = (uint8_t)(v >> 8);
        a = (uint8_t)v;
    }
    *out = IM_COL32(r, g, b, a);
    return true;
}

static void DrawOverlayText(ImDrawList *dl, const ImVec2 &origin,
                            const ScriptOverlayText &o)
{
    if (!dl || o.text.empty()) {
        return;
    }
    ImFont *font = ImGui::GetFont();
    const float size = std::max(1.0f, ImGui::GetFontSize() * o.scale);
    const ImVec2 pos(origin.x + o.x, origin.y + o.y);
    if ((o.background >> IM_COL32_A_SHIFT) != 0) {
        const ImVec2 text_size = font->CalcTextSizeA(
            size, FLT_MAX, 0.0f, o.text.c_str());
        constexpr float pad_x = 4.0f;
        constexpr float pad_y = 2.0f;
        dl->AddRectFilled(ImVec2(pos.x - pad_x, pos.y - pad_y),
                          ImVec2(pos.x + text_size.x + pad_x,
                                 pos.y + text_size.y + pad_y),
                          o.background, 3.0f);
    }
    dl->AddText(font, size, pos, o.color, o.text.c_str());
}

static void DestroyPrimitiveTexture(ScriptPrimitive &p)
{
    if (p.texture != 0) {
        if (SDL_GL_GetCurrentContext() != nullptr) {
            GLuint tex = p.texture;
            glDeleteTextures(1, &tex);
        }
        p.texture = 0;
    }
    p.image_w = 0;
    p.image_h = 0;
}

static bool EnsurePrimitiveTexture(ScriptPrimitive &p)
{
    if (p.type != ScriptPrimitiveType::Image) {
        return false;
    }
    if (p.texture != 0) {
        return true;
    }
    if (p.image_path.empty()) {
        return false;
    }

    int w = 0, h = 0, n = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char *pixels = stbi_load(p.image_path.c_str(), &w, &h, &n, 4);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) {
            stbi_image_free(pixels);
        }
        return false;
    }

    glGenTextures(1, &p.texture);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, p.texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    stbi_image_free(pixels);
    p.image_w = w;
    p.image_h = h;
    return true;
}

static void DrawPrimitive(ImDrawList *dl, const ImVec2 &origin,
                          ScriptPrimitive &p)
{
    if (!dl) {
        return;
    }
    const ImVec2 a(origin.x + p.x, origin.y + p.y);
    const float thickness = std::max(0.25f, p.thickness);

    switch (p.type) {
    case ScriptPrimitiveType::Line:
        dl->AddLine(a, ImVec2(origin.x + p.x2, origin.y + p.y2),
                    p.color, thickness);
        break;
    case ScriptPrimitiveType::Rect: {
        const ImVec2 b(a.x + p.w, a.y + p.h);
        if (p.filled) {
            dl->AddRectFilled(a, b, p.color, p.rounding);
        } else {
            dl->AddRect(a, b, p.color, p.rounding, 0, thickness);
        }
        break;
    }
    case ScriptPrimitiveType::Circle:
        if (p.filled) {
            dl->AddCircleFilled(a, std::max(0.5f, p.radius), p.color);
        } else {
            dl->AddCircle(a, std::max(0.5f, p.radius), p.color, 0, thickness);
        }
        break;
    case ScriptPrimitiveType::Bar: {
        const ImVec2 b(a.x + p.w, a.y + p.h);
        dl->AddRectFilled(a, b, p.background, p.rounding);
        float t = 0.0f;
        if (p.max_value > p.min_value) {
            t = (p.value - p.min_value) / (p.max_value - p.min_value);
        }
        t = std::max(0.0f, std::min(1.0f, t));
        if (p.vertical) {
            const float fy = a.y + p.h * (1.0f - t);
            dl->AddRectFilled(ImVec2(a.x, fy), b, p.color2, p.rounding);
        } else {
            dl->AddRectFilled(a, ImVec2(a.x + p.w * t, b.y),
                              p.color2, p.rounding);
        }
        if ((p.border >> IM_COL32_A_SHIFT) != 0) {
            dl->AddRect(a, b, p.border, p.rounding, 0, thickness);
        }
        break;
    }
    case ScriptPrimitiveType::Crosshair: {
        const float s = std::max(1.0f, p.radius);
        const float g = std::max(0.0f, p.gap);
        dl->AddLine(ImVec2(a.x - s, a.y), ImVec2(a.x - g, a.y), p.color, thickness);
        dl->AddLine(ImVec2(a.x + g, a.y), ImVec2(a.x + s, a.y), p.color, thickness);
        dl->AddLine(ImVec2(a.x, a.y - s), ImVec2(a.x, a.y - g), p.color, thickness);
        dl->AddLine(ImVec2(a.x, a.y + g), ImVec2(a.x, a.y + s), p.color, thickness);
        if (p.filled) {
            dl->AddCircleFilled(a, std::max(1.0f, thickness), p.color);
        }
        break;
    }
    case ScriptPrimitiveType::Image:
        if (EnsurePrimitiveTexture(p)) {
            float w = p.w > 0.0f ? p.w : (float)p.image_w;
            float h = p.h > 0.0f ? p.h : (float)p.image_h;
            dl->AddImage((ImTextureID)(intptr_t)p.texture, a,
                         ImVec2(a.x + w, a.y + h), ImVec2(0,0), ImVec2(1,1),
                         p.color);
        }
        break;
    }
}

static void DrawControllerWidget(ImDrawList *dl, const ImVec2 &origin,
                                 const ScriptControllerWidget &widget)
{
    if (!dl) {
        return;
    }
    uint8_t r[XEMU_TAS_XID_REPORT_SIZE]{};
    const bool have = xemu_tas_get_last_xid_report(widget.port, r, sizeof(r));
    const float sc = std::max(0.35f, std::min(4.0f, widget.scale));
    const ImVec2 o(origin.x + widget.x, origin.y + widget.y);
    const ImU32 inactive = IM_COL32(90, 90, 96, 220);
    const ImU32 outline = IM_COL32(210, 210, 220, 230);
    const ImU32 active = IM_COL32(255, 190, 60, 255);
    const ImU32 stick = IM_COL32(100, 190, 255, 255);

    uint16_t buttons = 0;
    int16_t lx = 0, ly = 0, rx = 0, ry = 0;
    if (have) {
        memcpy(&buttons, &r[2], sizeof(buttons));
        memcpy(&lx, &r[12], sizeof(lx));
        memcpy(&ly, &r[14], sizeof(ly));
        memcpy(&rx, &r[16], sizeof(rx));
        memcpy(&ry, &r[18], sizeof(ry));
    }
    auto held = [&](int bit) { return have && (buttons & (1u << bit)); };
    auto analog = [&](int index) { return have ? r[4 + index] : 0; };
    auto pt = [&](float x, float y) { return ImVec2(o.x + x * sc, o.y + y * sc); };

    dl->AddRectFilled(pt(8, 18), pt(212, 112), IM_COL32(24,24,28,210), 22.0f * sc);
    dl->AddRect(pt(8,18), pt(212,112), outline, 22.0f * sc, 0, 1.5f * sc);

    // D-pad.
    dl->AddRectFilled(pt(28,48), pt(62,62), held(2) || held(3) ? active : inactive, 3*sc);
    dl->AddRectFilled(pt(38,38), pt(52,72), held(0) || held(1) ? active : inactive, 3*sc);
    if (held(0)) dl->AddTriangleFilled(pt(45,39), pt(38,48), pt(52,48), active);
    if (held(1)) dl->AddTriangleFilled(pt(45,71), pt(38,62), pt(52,62), active);
    if (held(2)) dl->AddTriangleFilled(pt(29,55), pt(38,48), pt(38,62), active);
    if (held(3)) dl->AddTriangleFilled(pt(61,55), pt(52,48), pt(52,62), active);

    // Analog sticks.
    auto draw_stick = [&](float cx, float cy, int16_t sx, int16_t sy) {
        dl->AddCircleFilled(pt(cx, cy), 13.0f * sc, inactive);
        dl->AddCircle(pt(cx, cy), 13.0f * sc, outline, 0, 1.0f * sc);
        const float nx = std::max(-1.0f, std::min(1.0f, sx / 32767.0f));
        const float ny = std::max(-1.0f, std::min(1.0f, sy / 32767.0f));
        dl->AddCircleFilled(pt(cx + nx * 7.0f, cy - ny * 7.0f), 5.0f * sc,
                            (std::abs(sx) > 2500 || std::abs(sy) > 2500) ? stick : outline);
    };
    draw_stick(82, 82, lx, ly);
    draw_stick(140, 82, rx, ry);

    // A/B/X/Y analog face buttons.
    const char *labels[4] = {"A", "B", "X", "Y"};
    const float bx[4] = {177, 195, 159, 177};
    const float by[4] = {69, 53, 53, 37};
    for (int i = 0; i < 4; ++i) {
        const bool on = analog(i) > 8;
        dl->AddCircleFilled(pt(bx[i], by[i]), 8.0f * sc, on ? active : inactive);
        dl->AddCircle(pt(bx[i], by[i]), 8.0f * sc, outline, 0, 1.0f * sc);
        if (widget.labels) {
            dl->AddText(pt(bx[i]-3, by[i]-6), IM_COL32_WHITE, labels[i]);
        }
    }

    // Black/White + triggers + Start/Back + stick clicks.
    dl->AddCircleFilled(pt(164, 28), 5.5f * sc, analog(4) > 8 ? active : inactive);
    dl->AddCircleFilled(pt(180, 22), 5.5f * sc, analog(5) > 8 ? active : inactive);
    dl->AddRectFilled(pt(20, 8), pt(70, 14), inactive, 2*sc);
    dl->AddRectFilled(pt(20, 8), pt(20 + 50.0f * analog(6) / 255.0f, 14), active, 2*sc);
    dl->AddRectFilled(pt(150, 8), pt(200, 14), inactive, 2*sc);
    dl->AddRectFilled(pt(150, 8), pt(150 + 50.0f * analog(7) / 255.0f, 14), active, 2*sc);
    dl->AddCircleFilled(pt(101, 51), 4.0f*sc, held(4) ? active : inactive);
    dl->AddCircleFilled(pt(119, 51), 4.0f*sc, held(5) ? active : inactive);
    dl->AddCircle(pt(82,82), 15.5f*sc, held(6) ? active : IM_COL32(0,0,0,0), 0, 2*sc);
    dl->AddCircle(pt(140,82), 15.5f*sc, held(7) ? active : IM_COL32(0,0,0,0), 0, 2*sc);

    if (widget.labels) {
        char buf[64];
        snprintf(buf, sizeof(buf), "P%u  LT:%u RT:%u", (unsigned)widget.port + 1,
                 have ? r[10] : 0, have ? r[11] : 0);
        dl->AddText(pt(12, 118), have ? outline : IM_COL32(180,80,80,255), buf);
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
        return {};
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

    // Do not return a guessed executable name here.  The script console uses
    // SDL_CreateProcessWithProperties(), so a fabricated fallback such as
    // "lua" only turns "interpreter not installed" into a confusing
    // posix_spawn()/CreateProcess failure.  An empty result lets the UI report
    // the missing interpreter before attempting to launch a child process.
    return {};
}

class ScriptConsole {
public:
    explicit ScriptConsole(ScriptLanguage language)
        : m_language(language)
    {
        memset(m_interpreter, 0, sizeof(m_interpreter));
        memset(m_script_directory, 0, sizeof(m_script_directory));
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
        EnsureScriptDirectory();
    }

    bool IsOpen() const
    {
        return m_open || m_external_open;
    }

    bool NeedsFrameService() const
    {
        return m_open || m_running_count > 0 || m_overlay_count > 0 ||
               m_primitive_count > 0 || m_watch_count > 0 ||
               m_controller_count > 0 || m_external_open;
    }

    void Service()
    {
        /* Script IPC must keep running even when the console window is hidden.
         * With no active child process this is an immediate return. */
        if (AnyRunning()) {
            PollProcesses();
        }
        if (m_watch_count > 0) {
            UpdateMemoryWatches();
        }
    }

    void Draw()
    {
        const char *detach_id = IsLua() ? "scripting.lua-console"
                                        : "scripting.python-console";
        const char *title = IsLua() ? "Lua Console" : "Python Console";
        xemu_feature_detach::Register(detach_id, title, &m_open,
                                      [this]() { this->Draw(); });
        xemu_feature_detach::Pump();
        DrawExternalDisplay();

        if (!m_open || !xemu_feature_detach::ShouldDraw(detach_id)) {
            return;
        }

        EnsureInterpreter();

        if (xemu_feature_detach::IsDetachedPass(detach_id)) {
            xemu_feature_detach::PrepareWindow(detach_id);
        } else {
            ImGui::SetNextWindowSize(ImVec2(820, 520), ImGuiCond_FirstUseEver);
        }
        const ImGuiWindowFlags flags = xemu_feature_detach::WindowFlags(
            detach_id, ImGuiWindowFlags_MenuBar);
        if (!ImGui::Begin(title, &m_open, flags)) {
            ImGui::End();
            return;
        }
        xemu_feature_detach::ObserveCurrentWindow(detach_id);

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
        if (m_overlay_count == 0 && m_primitive_count == 0 &&
            m_controller_count == 0) {
            return;
        }
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        for (auto &entry : m_scripts) {
            for (auto &p : entry.primitives) {
                DrawPrimitive(dl, ImVec2(0.0f, 0.0f), p);
            }
            for (const auto &c : entry.controllers) {
                if (!c.external) {
                    DrawControllerWidget(dl, ImVec2(0.0f, 0.0f), c);
                }
            }
            for (const auto &o : entry.overlays) {
                DrawOverlayText(dl, ImVec2(0.0f, 0.0f), o);
            }
        }
    }

private:
    const char *ExternalDetachId() const
    {
        return IsLua() ? "scripting.lua-display" : "scripting.python-display";
    }

    void DrawExternalDisplay()
    {
        const char *detach_id = ExternalDetachId();
        const std::string default_title = IsLua() ? "Lua Script Display"
                                                   : "Python Script Display";
        const char *title = m_external_title.empty() ? default_title.c_str()
                                                     : m_external_title.c_str();
        xemu_feature_detach::Register(detach_id, title, &m_external_open,
            [this]() { this->DrawExternalDisplay(); });

        if (!m_external_open || !xemu_feature_detach::ShouldDraw(detach_id)) {
            return;
        }

        if (xemu_feature_detach::IsDetachedPass(detach_id)) {
            xemu_feature_detach::PrepareWindow(detach_id);
        } else {
            ImGui::SetNextWindowSize(
                ImVec2((float)m_external_width, (float)m_external_height),
                ImGuiCond_FirstUseEver);
        }
        const ImGuiWindowFlags flags = xemu_feature_detach::WindowFlags(
            detach_id, ImGuiWindowFlags_NoScrollbar |
                       ImGuiWindowFlags_NoScrollWithMouse);
        if (!ImGui::Begin(title, &m_external_open, flags)) {
            ImGui::End();
            return;
        }
        xemu_feature_detach::ObserveCurrentWindow(detach_id);

        if (m_external_auto_detach &&
            !xemu_feature_detach::IsDetachedPass(detach_id)) {
            xemu_feature_detach::RequestDetach(detach_id);
            m_external_auto_detach = false;
        }

        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 origin = ImGui::GetCursorScreenPos();
        for (auto &entry : m_scripts) {
            for (auto &p : entry.external_primitives) {
                DrawPrimitive(dl, origin, p);
            }
            for (const auto &c : entry.controllers) {
                if (c.external) {
                    DrawControllerWidget(dl, origin, c);
                }
            }
            for (const auto &o : entry.external_overlays) {
                DrawOverlayText(dl, origin, o);
            }
        }

        // Keep the full client area available as a free-form script canvas.
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        m_external_canvas_width = std::max(1, (int)avail.x);
        m_external_canvas_height = std::max(1, (int)avail.y);
        if (avail.x > 0.0f && avail.y > 0.0f) {
            ImGui::Dummy(avail);
        }
        ImGui::End();
    }

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

    std::string BuiltinScriptDirectory() const
    {
        std::filesystem::path p(xemu_settings_get_base_path());
        p /= "scripts";
        p /= IsLua() ? "lua" : "python";
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        return p.string();
    }

    std::string ScriptDirectorySettingsPath() const
    {
        std::filesystem::path p(xemu_settings_get_base_path());
        p /= IsLua() ? "lua-console.ini" : "python-console.ini";
        return p.string();
    }

    void EnsureScriptDirectory()
    {
        if (m_script_directory_initialized) {
            return;
        }
        m_script_directory_initialized = true;

        std::ifstream in(ScriptDirectorySettingsPath());
        std::string line;
        while (std::getline(in, line)) {
            constexpr const char prefix[] = "script_directory=";
            if (line.rfind(prefix, 0) == 0) {
                const std::string value = line.substr(sizeof(prefix) - 1);
                SDL_strlcpy(m_script_directory, value.c_str(),
                            sizeof(m_script_directory));
                break;
            }
        }
    }

    void SaveScriptDirectory()
    {
        EnsureScriptDirectory();
        std::ofstream out(ScriptDirectorySettingsPath(), std::ios::trunc);
        if (!out) {
            AppendOutput("[console] could not save script directory setting\n");
            return;
        }
        out << "script_directory=" << m_script_directory << '\n';
    }

    std::string DefaultScriptDirectory()
    {
        EnsureScriptDirectory();
        if (m_script_directory[0]) {
            return m_script_directory;
        }
        return BuiltinScriptDirectory();
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
import sys, itertools, time, struct
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

def _addr(v): return int(v, 0) if isinstance(v, str) else int(v)
def _hexs(s): return str(s).encode("utf-8").hex()
def _color(v): return str(v)
def _target(v): return "display" if str(v).lower() in ("display","external","window") else "overlay"

def api_version(): return int(_call("api_version"))
def capabilities(): return set(filter(None,_call("capabilities").split(",")))
def frame(): return int(_call("frame"))
def lag_count(): return int(_call("lag_count"))
def title_id(): return _call("title_id")
def runstate(): return _call("runstate")
def tas_enabled(): return _call("tas_enabled") == "1"
def set_tas_enabled(v=True): _call("set_tas_enabled", 1 if v else 0)
def pause(): _call("pause")
def resume(): _call("resume")
def frame_advance(n=1): _call("frame_advance", int(n))
def wait_frame(target=None):
    if target is None: target = frame() + 1
    return int(_call("wait_frame", int(target)))
def sleep_frames(n=1):
    target = frame() + max(1, int(n))
    return wait_frame(target)
def on_frame(callback, count=None):
    n = 0
    while count is None or n < count:
        f = wait_frame()
        callback(f)
        n += 1

def wait_runstate_change(previous=None):
    if previous is None: previous = runstate()
    return _call("wait_runstate", _hexs(previous))
def on_runstate(callback, count=None):
    n, state = 0, runstate()
    while count is None or n < count:
        state = wait_runstate_change(state)
        callback(state)
        n += 1
def on_pause(callback, count=None):
    n = 0
    while count is None or n < count:
        state = wait_runstate_change()
        if state in ("paused", "debug", "suspended"):
            callback(state); n += 1
def on_resume(callback, count=None):
    n = 0
    while count is None or n < count:
        state = wait_runstate_change()
        if state == "running": callback(state); n += 1

def wait_title_change(previous=None):
    if previous is None:
        try: previous = title_id()
        except RuntimeError: previous = ""
    return _call("wait_title", _hexs(previous))
def on_title_change(callback, count=None):
    n = 0
    while count is None or n < count:
        t = wait_title_change()
        callback(t)
        n += 1

# Memory. Legacy read_u*/write_u* remain PHYSICAL for compatibility.
def read_u8(addr): return int(_call("read_space", _addr(addr), 1, 0), 0)
def read_u16(addr): return int(_call("read_space", _addr(addr), 2, 0), 0)
def read_u32(addr): return int(_call("read_space", _addr(addr), 4, 0), 0)
def write_u8(addr,v): _call("write_space", _addr(addr), 1, 0, int(v))
def write_u16(addr,v): _call("write_space", _addr(addr), 2, 0, int(v))
def write_u32(addr,v): _call("write_space", _addr(addr), 4, 0, int(v))
def read_phys_u8(a): return read_u8(a)
def read_phys_u16(a): return read_u16(a)
def read_phys_u32(a): return read_u32(a)
def write_phys_u8(a,v): write_u8(a,v)
def write_phys_u16(a,v): write_u16(a,v)
def write_phys_u32(a,v): write_u32(a,v)
def read_virt_u8(a): return int(_call("read_space", _addr(a), 1, 1), 0)
def read_virt_u16(a): return int(_call("read_space", _addr(a), 2, 1), 0)
def read_virt_u32(a): return int(_call("read_space", _addr(a), 4, 1), 0)
def write_virt_u8(a,v): _call("write_space", _addr(a), 1, 1, int(v))
def write_virt_u16(a,v): _call("write_space", _addr(a), 2, 1, int(v))
def write_virt_u32(a,v): _call("write_space", _addr(a), 4, 1, int(v))
def read_bytes(addr, size, virtual=True):
    return bytes.fromhex(_call("read_bytes", _addr(addr), int(size), 1 if virtual else 0))
def write_bytes(addr, data, virtual=True):
    if not isinstance(data,(bytes,bytearray,memoryview)): data = bytes(data)
    _call("write_bytes", _addr(addr), 1 if virtual else 0, bytes(data).hex())
def read_f32(addr, virtual=True): return struct.unpack("<f", read_bytes(addr,4,virtual))[0]
def read_f64(addr, virtual=True): return struct.unpack("<d", read_bytes(addr,8,virtual))[0]
def write_f32(addr, value, virtual=True): write_bytes(addr, struct.pack("<f",float(value)), virtual)
def write_f64(addr, value, virtual=True): write_bytes(addr, struct.pack("<d",float(value)), virtual)
def read_cstring(addr, max_len=4096, virtual=True, encoding="utf-8"):
    data=read_bytes(addr,max_len,virtual); data=data.split(b"\0",1)[0]
    return data.decode(encoding,errors="replace")
def write_cstring(addr, text, virtual=True, encoding="utf-8", nul=True):
    data=str(text).encode(encoding)+(b"\0" if nul else b"")
    write_bytes(addr,data,virtual)
def pointer_chain(base, offsets, virtual=True):
    p=_addr(base)
    for off in offsets:
        raw=read_bytes(p,4,virtual); p=struct.unpack("<I",raw)[0]+int(off)
    return p

def input_get(port=1): return bytes.fromhex(_call("input_get", int(port)-1))
def input_set(data, port=1):
    if isinstance(data,(bytes,bytearray)): data=data.hex()
    _call("input_set", int(port)-1, str(data))
def input_release(port=1): _call("input_release", int(port)-1)
def input_release_all(): _call("input_release_all")
def input_state(port=1):
    d=_call("input_state",int(port)-1).split(",")
    if len(d)<15: raise RuntimeError("no XID report yet")
    names=("buttons","a","b","x","y","black","white","lt","rt","lx","ly","rx","ry","start","back")
    return {k:int(v) for k,v in zip(names,d)}
def on_input(callback,port=1,count=None):
    n=0; previous=None
    while count is None or n<count:
        wait_frame(); current=input_state(port)
        if current != previous:
            callback(current); previous=current; n+=1
def on_memory_change(addr,size=4,virtual=True,callback=None,count=None):
    n=0; previous=read_bytes(addr,size,virtual)
    while count is None or n<count:
        wait_frame(); current=read_bytes(addr,size,virtual)
        if current != previous:
            if callback: callback(current,previous)
            previous=current; n+=1

def snapshot_save(name): _call("snapshot_save", _hexs(name))
def snapshot_load(name): _call("snapshot_load", _hexs(name))
def notify(text): _call("notify", _hexs(text))
def screen_size():
    w,h=_call("screen_size").split(",",1); return int(w),int(h)

# Text overlays.
def overlay_text(x,y,text): _call("overlay_text",float(x),float(y),_hexs(text))
def overlay_set(id,x,y,text,color="#FFFFFFFF",scale=1.0,background="#00000000"):
    _call("overlay_set",_hexs(id),float(x),float(y),_hexs(text),_color(color),float(scale),_color(background))
def overlay_remove(id): _call("overlay_remove",_hexs(id))
def overlay_clear(): _call("overlay_clear")

def display_open(title="Python Script Display",width=640,height=360,detached=True):
    _call("display_open",_hexs(title),int(width),int(height),1 if detached else 0)
def display_close(): _call("display_close")
def display_size():
    w,h=_call("display_size").split(",",1); return int(w),int(h)
def display_text(id,x,y,text,color="#FFFFFFFF",scale=1.0,background="#00000000"):
    _call("display_text",_hexs(id),float(x),float(y),_hexs(text),_color(color),float(scale),_color(background))
def display_remove(id): _call("display_remove",_hexs(id))
def display_clear(): _call("display_clear")

# Free-form drawing primitives. target="overlay" or "display".
def line(id,x1,y1,x2,y2,color="#FFFFFFFF",thickness=1.0,target="overlay"):
    _call("draw_line",_target(target),_hexs(id),x1,y1,x2,y2,_color(color),thickness)
def rect(id,x,y,w,h,color="#FFFFFFFF",filled=False,thickness=1.0,rounding=0.0,target="overlay"):
    _call("draw_rect",_target(target),_hexs(id),x,y,w,h,_color(color),1 if filled else 0,thickness,rounding)
def circle(id,x,y,radius,color="#FFFFFFFF",filled=False,thickness=1.0,target="overlay"):
    _call("draw_circle",_target(target),_hexs(id),x,y,radius,_color(color),1 if filled else 0,thickness)
def bar(id,x,y,w,h,value,min_value=0,max_value=100,color="#46C864FF",background="#000000A0",border="#FFFFFF80",vertical=False,target="overlay"):
    _call("draw_bar",_target(target),_hexs(id),x,y,w,h,value,min_value,max_value,_color(color),_color(background),_color(border),1 if vertical else 0)
def crosshair(id,x,y,size=12,color="#FFFFFFFF",thickness=1.5,gap=3.0,dot=False,target="overlay"):
    _call("draw_crosshair",_target(target),_hexs(id),x,y,size,_color(color),thickness,gap,1 if dot else 0)
def image(id,path,x,y,w=0,h=0,tint="#FFFFFFFF",target="overlay"):
    _call("draw_image",_target(target),_hexs(id),_hexs(path),x,y,w,h,_color(tint))
def draw_remove(id,target="overlay"): _call("draw_remove",_target(target),_hexs(id))
def draw_clear(target="overlay"): _call("draw_clear",_target(target))

# Direct memory -> HUD bindings, updated by Xemu without Python/Lua polling.
def watch_text(id,addr,value_type="u32",x=20,y=20,prefix="",suffix="",color="#FFFFFFFF",scale=1.0,background="#00000080",virtual=True,target="overlay"):
    _call("watch_text",_hexs(id),_addr(addr),value_type,1 if virtual else 0,_target(target),x,y,_hexs(prefix),_hexs(suffix),_color(color),scale,_color(background))
def watch_bar(id,addr,value_type="u32",x=20,y=20,w=180,h=16,min_value=0,max_value=100,color="#46C864FF",background="#000000A0",border="#FFFFFF80",virtual=True,target="overlay"):
    _call("watch_bar",_hexs(id),_addr(addr),value_type,1 if virtual else 0,_target(target),x,y,w,h,min_value,max_value,_color(color),_color(background),_color(border))
def watch_remove(id): _call("watch_remove",_hexs(id))
def watch_clear(): _call("watch_clear")

# Live Xbox controller visualization.
def controller_show(id="controller",port=1,x=20,y=20,scale=1.0,target="overlay",labels=True):
    _call("controller_show",_hexs(id),int(port)-1,x,y,scale,_target(target),1 if labels else 0)
def controller_remove(id="controller"): _call("controller_remove",_hexs(id))
def controller_clear(): _call("controller_clear")

# CPU debugger/disassembler. Available when xemu_feature_debug_tools is enabled.
def debug_available(): return _call("debug_available")=="1"
def regs():
    out={}
    for part in _call("debug_regs").split(","):
        if "=" in part:
            k,v=part.split("=",1); out[k]=int(v,0)
    return out
def set_reg(name,value): _call("debug_set_reg",str(name),_addr(value))
def disasm(addr,count=16):
    result=[]
    raw=_call("debug_disasm",_addr(addr),int(count))
    if not raw: return result
    for row in raw.split(";"):
        if not row: continue
        a,b,m,o=row.split(",",3)
        result.append({"address":int(a),"bytes":bytes.fromhex(b),"mnemonic":bytes.fromhex(m).decode("utf-8","replace"),"operands":bytes.fromhex(o).decode("utf-8","replace")})
    return result
def breakpoint_add(addr,physical=False): _call("debug_bp_add",_addr(addr),0 if physical else 1)
def breakpoint_remove(addr,physical=False): _call("debug_bp_remove",_addr(addr),0 if physical else 1)
def watchpoint_add(addr,length=1,access="rw",physical=False):
    flags=3 if "r" in access.lower() and "w" in access.lower() else (1 if "r" in access.lower() else 2)
    _call("debug_wp_add",_addr(addr),int(length),flags,0 if physical else 1)
def watchpoint_remove(addr,length=1,access="rw",physical=False):
    flags=3 if "r" in access.lower() and "w" in access.lower() else (1 if "r" in access.lower() else 2)
    _call("debug_wp_remove",_addr(addr),int(length),flags,0 if physical else 1)
def step(): _call("debug_step")
def step_over(): _call("debug_step_over")
def step_out(): _call("debug_step_out")
def run_to(addr): _call("debug_run_to",_addr(addr))
def to_phys(addr): return int(_call("debug_to_phys",_addr(addr)),0)
def to_virt(addr): return int(_call("debug_to_virt",_addr(addr)),0)
def debug_event():
    data=_call("debug_event")
    if not data: return None
    seq,typ,pc,addr,length,flags,physical=data.split(",")
    return {"sequence":int(seq),"type":typ,"pc":int(pc),"address":int(addr),"length":int(length),"flags":int(flags),"physical":bool(int(physical))}
def wait_debug_event(since=None):
    if since is None:
        ev=debug_event(); since=ev["sequence"] if ev else 0
    data=_call("debug_wait",int(since))
    seq,typ,pc,addr,length,flags,physical=data.split(",")
    return {"sequence":int(seq),"type":typ,"pc":int(pc),"address":int(addr),"length":int(length),"flags":int(flags),"physical":bool(int(physical))}
def on_debug_stop(callback,count=None):
    n,seq=0,None
    while count is None or n<count:
        ev=wait_debug_event(seq); seq=ev["sequence"]; callback(ev); n+=1
def on_breakpoint(callback,count=None):
    n,seq=0,None
    while count is None or n<count:
        ev=wait_debug_event(seq); seq=ev["sequence"]
        if ev["type"]=="breakpoint": callback(ev); n+=1
def on_watchpoint(callback,count=None):
    n,seq=0,None
    while count is None or n<count:
        ev=wait_debug_event(seq); seq=ev["sequence"]
        if ev["type"]=="watchpoint": callback(ev); n+=1
)PY";
        }
        {
            std::ofstream lua(dir / "xemu.lua", std::ios::binary | std::ios::trunc);
            lua << R"LUA(-- Auto-generated by xemu's Lua Console.
local M = {}
local seq = 0
local function call(cmd, ...)
  seq = seq + 1
  local args = {...}; local t = {}
  for i,v in ipairs(args) do t[#t+1] = tostring(v) end
  io.write("@@XEMUAPI|"..seq.."|"..cmd.."|"..table.concat(t,"|").."\n"); io.flush()
  while true do
    local line = io.read("*l"); if not line then error("xemu API connection closed") end
    local id,status,data = line:match("^@@XEMURESP|(%d+)|([^|]+)|?(.*)$")
    if id and tonumber(id)==seq then if status ~= "OK" then error(data) end return data or "" end
  end
end
local function hex(s) return (tostring(s):gsub('.', function(c) return string.format('%02x', string.byte(c)) end)) end
local function unhex(s) return (s:gsub('..', function(cc) return string.char(tonumber(cc,16)) end)) end
local function target(v) v=tostring(v or "overlay"):lower(); if v=="display" or v=="external" or v=="window" then return "display" end return "overlay" end
function M.api_version() return tonumber(call("api_version")) end
function M.capabilities() local t={};for v in call("capabilities"):gmatch("[^,]+") do t[v]=true end;return t end
function M.frame() return tonumber(call("frame")) end
function M.lag_count() return tonumber(call("lag_count")) end
function M.title_id() return call("title_id") end
function M.runstate() return call("runstate") end
function M.tas_enabled() return call("tas_enabled")=="1" end
function M.set_tas_enabled(v) call("set_tas_enabled",v and 1 or 0) end
function M.pause() call("pause") end
function M.resume() call("resume") end
function M.frame_advance(n) call("frame_advance",n or 1) end
function M.wait_frame(t) return tonumber(call("wait_frame",t or (M.frame()+1))) end
function M.sleep_frames(n) return M.wait_frame(M.frame()+math.max(1,n or 1)) end
function M.on_frame(fn,count) local n=0 while not count or n<count do fn(M.wait_frame());n=n+1 end end
function M.wait_runstate_change(prev) return call("wait_runstate",hex(prev or M.runstate())) end
function M.on_runstate(fn,count) local n=0; local s=M.runstate(); while not count or n<count do s=M.wait_runstate_change(s);fn(s);n=n+1 end end
function M.on_pause(fn,count) local n=0;while not count or n<count do local s=M.wait_runstate_change();if s=="paused" or s=="debug" or s=="suspended" then fn(s);n=n+1 end end end
function M.on_resume(fn,count) local n=0;while not count or n<count do local s=M.wait_runstate_change();if s=="running" then fn(s);n=n+1 end end end
function M.wait_title_change(prev) return call("wait_title",hex(prev or M.title_id())) end
function M.on_title_change(fn,count) local n=0; while not count or n<count do fn(M.wait_title_change());n=n+1 end end
function M.read_u8(a) return tonumber(call("read_space",a,1,0)) end
function M.read_u16(a) return tonumber(call("read_space",a,2,0)) end
function M.read_u32(a) return tonumber(call("read_space",a,4,0)) end
function M.write_u8(a,v) call("write_space",a,1,0,v) end
function M.write_u16(a,v) call("write_space",a,2,0,v) end
function M.write_u32(a,v) call("write_space",a,4,0,v) end
function M.read_virt_u8(a) return tonumber(call("read_space",a,1,1)) end
function M.read_virt_u16(a) return tonumber(call("read_space",a,2,1)) end
function M.read_virt_u32(a) return tonumber(call("read_space",a,4,1)) end
function M.write_virt_u8(a,v) call("write_space",a,1,1,v) end
function M.write_virt_u16(a,v) call("write_space",a,2,1,v) end
function M.write_virt_u32(a,v) call("write_space",a,4,1,v) end
function M.read_bytes(a,n,virt) if virt==nil then virt=true end return unhex(call("read_bytes",a,n,virt and 1 or 0)) end
function M.write_bytes(a,data,virt) if virt==nil then virt=true end call("write_bytes",a,virt and 1 or 0,hex(data)) end
function M.read_f32(a,virt) if virt==nil then virt=true end return tonumber(call("read_float",a,4,virt and 1 or 0)) end
function M.read_f64(a,virt) if virt==nil then virt=true end return tonumber(call("read_float",a,8,virt and 1 or 0)) end
function M.write_f32(a,v,virt) if virt==nil then virt=true end call("write_float",a,4,virt and 1 or 0,v) end
function M.write_f64(a,v,virt) if virt==nil then virt=true end call("write_float",a,8,virt and 1 or 0,v) end
function M.read_cstring(a,maxlen,virt) local s=M.read_bytes(a,maxlen or 4096,virt); return (s:match("^[^%z]*") or "") end
function M.write_cstring(a,s,virt,nul) if nul==nil then nul=true end M.write_bytes(a,tostring(s)..(nul and "\0" or ""),virt) end
function M.input_get(port) return call("input_get",(port or 1)-1) end
function M.input_set(hexreport,port) call("input_set",(port or 1)-1,hexreport) end
function M.input_release(port) call("input_release",(port or 1)-1) end
function M.input_release_all() call("input_release_all") end
function M.input_state(port) local d={}; for v in call("input_state",(port or 1)-1):gmatch("[^,]+") do d[#d+1]=tonumber(v) end; return {buttons=d[1],a=d[2],b=d[3],x=d[4],y=d[5],black=d[6],white=d[7],lt=d[8],rt=d[9],lx=d[10],ly=d[11],rx=d[12],ry=d[13],start=d[14],back=d[15]} end
function M.on_input(fn,port,count) local n=0;local prev=nil;while not count or n<count do M.wait_frame();local cur=M.input_state(port);local sig=table.concat({cur.buttons,cur.a,cur.b,cur.x,cur.y,cur.black,cur.white,cur.lt,cur.rt,cur.lx,cur.ly,cur.rx,cur.ry},",");if sig~=prev then fn(cur);prev=sig;n=n+1 end end end
function M.on_memory_change(addr,size,virt,fn,count) local n=0;local prev=M.read_bytes(addr,size or 4,virt);while not count or n<count do M.wait_frame();local cur=M.read_bytes(addr,size or 4,virt);if cur~=prev then if fn then fn(cur,prev) end;prev=cur;n=n+1 end end end
function M.snapshot_save(name) call("snapshot_save",hex(name)) end
function M.snapshot_load(name) call("snapshot_load",hex(name)) end
function M.notify(text) call("notify",hex(text)) end
function M.screen_size() local s=call("screen_size");local w,h=s:match("^(%d+),(%d+)$");return tonumber(w),tonumber(h) end
function M.overlay_text(x,y,text) call("overlay_text",x,y,hex(text)) end
function M.overlay_set(id,x,y,text,color,scale,bg) call("overlay_set",hex(id),x,y,hex(text),color or "#FFFFFFFF",scale or 1,bg or "#00000000") end
function M.overlay_remove(id) call("overlay_remove",hex(id)) end
function M.overlay_clear() call("overlay_clear") end
function M.display_open(title,w,h,detached) if detached==nil then detached=true end call("display_open",hex(title or "Lua Script Display"),w or 640,h or 360,detached and 1 or 0) end
function M.display_close() call("display_close") end
function M.display_size() local s=call("display_size");local w,h=s:match("^(%d+),(%d+)$");return tonumber(w),tonumber(h) end
function M.display_text(id,x,y,text,color,scale,bg) call("display_text",hex(id),x,y,hex(text),color or "#FFFFFFFF",scale or 1,bg or "#00000000") end
function M.display_remove(id) call("display_remove",hex(id)) end
function M.display_clear() call("display_clear") end
function M.line(id,x1,y1,x2,y2,color,thickness,t) call("draw_line",target(t),hex(id),x1,y1,x2,y2,color or "#FFFFFFFF",thickness or 1) end
function M.rect(id,x,y,w,h,color,filled,thickness,rounding,t) call("draw_rect",target(t),hex(id),x,y,w,h,color or "#FFFFFFFF",filled and 1 or 0,thickness or 1,rounding or 0) end
function M.circle(id,x,y,r,color,filled,thickness,t) call("draw_circle",target(t),hex(id),x,y,r,color or "#FFFFFFFF",filled and 1 or 0,thickness or 1) end
function M.bar(id,x,y,w,h,value,minv,maxv,color,bg,border,vertical,t) call("draw_bar",target(t),hex(id),x,y,w,h,value,minv or 0,maxv or 100,color or "#46C864FF",bg or "#000000A0",border or "#FFFFFF80",vertical and 1 or 0) end
function M.crosshair(id,x,y,size,color,thickness,gap,dot,t) call("draw_crosshair",target(t),hex(id),x,y,size or 12,color or "#FFFFFFFF",thickness or 1.5,gap or 3,dot and 1 or 0) end
function M.image(id,path,x,y,w,h,tint,t) call("draw_image",target(t),hex(id),hex(path),x,y,w or 0,h or 0,tint or "#FFFFFFFF") end
function M.draw_remove(id,t) call("draw_remove",target(t),hex(id)) end
function M.draw_clear(t) call("draw_clear",target(t)) end
function M.watch_text(id,addr,typ,x,y,prefix,suffix,color,scale,bg,virt,t) if virt==nil then virt=true end call("watch_text",hex(id),addr,typ or "u32",virt and 1 or 0,target(t),x or 20,y or 20,hex(prefix or ""),hex(suffix or ""),color or "#FFFFFFFF",scale or 1,bg or "#00000080") end
function M.watch_bar(id,addr,typ,x,y,w,h,minv,maxv,color,bg,border,virt,t) if virt==nil then virt=true end call("watch_bar",hex(id),addr,typ or "u32",virt and 1 or 0,target(t),x or 20,y or 20,w or 180,h or 16,minv or 0,maxv or 100,color or "#46C864FF",bg or "#000000A0",border or "#FFFFFF80") end
function M.watch_remove(id) call("watch_remove",hex(id)) end
function M.watch_clear() call("watch_clear") end
function M.controller_show(id,port,x,y,scale,t,labels) if labels==nil then labels=true end call("controller_show",hex(id or "controller"),(port or 1)-1,x or 20,y or 20,scale or 1,target(t),labels and 1 or 0) end
function M.controller_remove(id) call("controller_remove",hex(id or "controller")) end
function M.controller_clear() call("controller_clear") end
function M.debug_available() return call("debug_available")=="1" end
function M.regs() local out={}; for p in call("debug_regs"):gmatch("[^,]+") do local k,v=p:match("([^=]+)=(.+)");if k then out[k]=tonumber(v) end end; return out end
function M.set_reg(n,v) call("debug_set_reg",n,v) end
function M.disasm(addr,count) local out={};local raw=call("debug_disasm",addr,count or 16);for row in raw:gmatch("[^;]+") do local a,b,m,o=row:match("([^,]*),([^,]*),([^,]*),(.*)");if a then out[#out+1]={address=tonumber(a),bytes=unhex(b),mnemonic=unhex(m),operands=unhex(o)} end end;return out end
function M.breakpoint_add(a,physical) call("debug_bp_add",a,physical and 0 or 1) end
function M.breakpoint_remove(a,physical) call("debug_bp_remove",a,physical and 0 or 1) end
function M.watchpoint_add(a,len,access,physical) access=access or "rw";local f=(access:find("r") and access:find("w")) and 3 or (access:find("r") and 1 or 2);call("debug_wp_add",a,len or 1,f,physical and 0 or 1) end
function M.watchpoint_remove(a,len,access,physical) access=access or "rw";local f=(access:find("r") and access:find("w")) and 3 or (access:find("r") and 1 or 2);call("debug_wp_remove",a,len or 1,f,physical and 0 or 1) end
function M.step() call("debug_step") end
function M.step_over() call("debug_step_over") end
function M.step_out() call("debug_step_out") end
function M.run_to(a) call("debug_run_to",a) end
function M.to_phys(a) return tonumber(call("debug_to_phys",a)) end
function M.to_virt(a) return tonumber(call("debug_to_virt",a)) end
local function parse_event(s) if not s or s=="" then return nil end; local a={};for v in s:gmatch("[^,]+") do a[#a+1]=v end;return {sequence=tonumber(a[1]),type=a[2],pc=tonumber(a[3]),address=tonumber(a[4]),length=tonumber(a[5]),flags=tonumber(a[6]),physical=tonumber(a[7])~=0} end
function M.debug_event() return parse_event(call("debug_event")) end
function M.wait_debug_event(since) if not since then local e=M.debug_event();since=e and e.sequence or 0 end return parse_event(call("debug_wait",since)) end
function M.on_debug_stop(fn,count) local n=0;local seq=nil;while not count or n<count do local e=M.wait_debug_event(seq);seq=e.sequence;fn(e);n=n+1 end end
function M.on_breakpoint(fn,count) local n=0;local seq=nil;while not count or n<count do local e=M.wait_debug_event(seq);seq=e.sequence;if e.type=="breakpoint" then fn(e);n=n+1 end end end
function M.on_watchpoint(fn,count) local n=0;local seq=nil;while not count or n<count do local e=M.wait_debug_event(seq);seq=e.sequence;if e.type=="watchpoint" then fn(e);n=n+1 end end end
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
            EnsureScriptDirectory();

            ImGui::TextUnformatted("Interpreter executable");
            ImGui::SetNextItemWidth(420.0f);
            ImGui::InputText("##interpreter", m_interpreter,
                             sizeof(m_interpreter));

            ImGui::Separator();
            ImGui::TextUnformatted("Default script directory");
            ImGui::SetNextItemWidth(420.0f);
            ImGui::InputTextWithHint(
                "##script_directory", "blank = xemu data/scripts/<language>",
                m_script_directory, sizeof(m_script_directory));
            if (ImGui::IsItemDeactivatedAfterEdit()) {
                SaveScriptDirectory();
            }
            if (ImGui::Button("Browse...##script_directory")) {
                const std::string start = DefaultScriptDirectory();
                ShowOpenFolderDialog(start.c_str(), [this](const char *path) {
                    if (!path || !*path) {
                        return;
                    }
                    SDL_strlcpy(m_script_directory, path,
                                sizeof(m_script_directory));
                    SaveScriptDirectory();
                });
            }
            ImGui::SameLine();
            if (ImGui::Button("Use Default##script_directory")) {
                m_script_directory[0] = '\0';
                SaveScriptDirectory();
            }
            ImGui::TextDisabled("Current: %s", DefaultScriptDirectory().c_str());

            ImGui::Separator();
            ImGui::Checkbox("Auto-scroll output", &m_auto_scroll);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Help")) {
            // A wrapped TextWrapped() item inside an auto-sized menu can begin
            // life at effectively zero content width, producing the tall
            // one-character column seen in the old Lua/Python Help popup.
            // Give the popup a deterministic wrap width and render ordinary
            // unformatted text through that boundary instead.
            constexpr float kHelpWrapWidth = 460.0f;
            const float wrap_pos = ImGui::GetCursorPosX() + kHelpWrapWidth;
            ImGui::PushTextWrapPos(wrap_pos);
            const std::string intro =
                std::string(LanguageName()) +
                " scripts run asynchronously in a child interpreter so "
                "emulation is never blocked. stdout/stderr and an interactive "
                "stdin console are connected live.";
            ImGui::TextUnformatted(intro.c_str());
            ImGui::Separator();
            ImGui::TextUnformatted(
                "Emulator API bridge is active. Python: import xemu. Lua: "
                "local xemu = require('xemu'). APIs include virtual/physical "
                "memory, HUD drawing and images, memory-bound watches, "
                "controller visualization, event callbacks, TAS/snapshots, "
                "and optional debugger/disassembler control.");
            ImGui::PopTextWrapPos();
            // Ensure the popup's auto-fit width never collapses back to the
            // width of the Help label on the first hover frame.
            ImGui::Dummy(ImVec2(kHelpWrapWidth, 0.0f));
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    void DrawToolbar()
    {
        // Snapshot UI state once for this toolbar pass.  StopSelected() changes
        // SelectedIsRunning(), and RemoveSelected() can change HasSelection().
        // Re-evaluating either condition around an ImGui BeginDisabled()/
        // EndDisabled() pair can therefore unbalance ImGui's disabled stack and
        // trigger a fatal assertion in the same frame.
        const bool has_selection = HasSelection();
        const bool selected_running = SelectedIsRunning();

        if (ImGui::Button("Add Script...")) {
            AddScriptDialog();
        }
        ImGui::SameLine();
        if (!has_selection) ImGui::BeginDisabled();
        if (ImGui::Button("Run / Reload")) {
            RunSelected();
        }
        ImGui::SameLine();
        if (!selected_running) ImGui::BeginDisabled();
        if (ImGui::Button("Stop")) {
            StopSelected();
        }
        if (!selected_running) ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Remove")) {
            RemoveSelected();
        }
        if (!has_selection) ImGui::EndDisabled();
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

                // Toggle only on the exact second click of a double-click
                // sequence. A third/ordinary click therefore just selects the
                // row and cannot immediately undo the requested run/stop.
                if (ImGui::IsItemHovered() &&
                    ImGui::GetMouseClickedCount(ImGuiMouseButton_Left) == 2) {
                    m_selected = i;
                    if (entry.running) {
                        StopScript(i, false);
                    } else {
                        RunScript(i);
                    }
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
            AppendOutput("[console] no " + std::string(LanguageName()) +
                         " interpreter found in PATH; set one under "
                         "Settings > Interpreter executable\n");
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
        entry.wait_kind = ScriptWaitKind::None;
        ClearOverlayList(entry.overlays, &m_overlay_count);
        ClearOverlayList(entry.external_overlays, &m_external_overlay_count);
        ClearPrimitiveList(entry.primitives, &m_primitive_count);
        ClearPrimitiveList(entry.external_primitives, &m_external_primitive_count);
        ClearWatches(entry);
        if (m_controller_count >= entry.controllers.size()) {
            m_controller_count -= entry.controllers.size();
        } else {
            m_controller_count = 0;
        }
        entry.controllers.clear();
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
            if (quiet) {
                ClearOverlayList(entry.overlays, &m_overlay_count);
                ClearOverlayList(entry.external_overlays, &m_external_overlay_count);
                ClearPrimitiveList(entry.primitives, &m_primitive_count);
                ClearPrimitiveList(entry.external_primitives, &m_external_primitive_count);
                ClearWatches(entry);
                if (m_controller_count >= entry.controllers.size()) {
                    m_controller_count -= entry.controllers.size();
                } else {
                    m_controller_count = 0;
                }
                entry.controllers.clear();
            }
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
        entry.wait_kind = ScriptWaitKind::None;
        entry.pending_output.clear();
        ClearOverlayList(entry.overlays, &m_overlay_count);
        ClearOverlayList(entry.external_overlays, &m_external_overlay_count);
        ClearPrimitiveList(entry.primitives, &m_primitive_count);
        ClearPrimitiveList(entry.external_primitives, &m_external_primitive_count);
        ClearWatches(entry);
        if (m_controller_count >= entry.controllers.size()) {
            m_controller_count -= entry.controllers.size();
        } else {
            m_controller_count = 0;
        }
        entry.controllers.clear();
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
        out.reserve(1 + (size_t)std::count(line.begin(), line.end(), '|'));
        size_t start = 0;
        while (true) {
            size_t p = line.find('|', start);
            if (p == std::string::npos) {
                out.emplace_back(line, start, std::string::npos);
                break;
            }
            out.emplace_back(line, start, p - start);
            start = p + 1;
        }
        return out;
    }

    static bool DecodeHex(const std::string &hex, std::vector<uint8_t> *out)
    {
        if (!out || (hex.size() & 1)) return false;
        out->clear();
        out->reserve(hex.size() / 2);
        for (size_t i = 0; i < hex.size(); i += 2) {
            const int hi = HexDigit(hex[i]);
            const int lo = HexDigit(hex[i + 1]);
            if (hi < 0 || lo < 0) return false;
            out->push_back((uint8_t)((hi << 4) | lo));
        }
        return true;
    }

    static std::string EncodeHex(const void *data, size_t size)
    {
        static constexpr char digits[] = "0123456789abcdef";
        const uint8_t *p = static_cast<const uint8_t *>(data);
        std::string out(size * 2, '0');
        for (size_t i = 0; i < size; ++i) {
            out[i * 2] = digits[p[i] >> 4];
            out[i * 2 + 1] = digits[p[i] & 0x0f];
        }
        return out;
    }

    void SendApiResponse(ScriptEntry &entry, uint64_t id, bool ok, const std::string &payload)
    {
        if (!entry.process) return;
        SDL_IOStream *in = SDL_GetProcessInput(entry.process);
        if (!in) return;
        std::string line;
        line.reserve(payload.size() + 48);
        line += "@@XEMURESP|";
        line += std::to_string(id);
        line += ok ? "|OK|" : "|ERR|";
        line += payload;
        line += '\n';
        SDL_WriteIO(in, line.data(), line.size());
        SDL_FlushIO(in);
    }

    static ScriptOverlayText *FindOverlay(std::vector<ScriptOverlayText> &items,
                                          const std::string &id)
    {
        for (auto &item : items) {
            if (item.id == id) {
                return &item;
            }
        }
        return nullptr;
    }

    static bool DecodeStringArg(const std::string &hex, std::string *out)
    {
        if (!out || (hex.size() & 1)) return false;
        out->resize(hex.size() / 2);
        for (size_t i = 0; i < out->size(); ++i) {
            const int hi = HexDigit(hex[i * 2]);
            const int lo = HexDigit(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) {
                out->clear();
                return false;
            }
            (*out)[i] = (char)((hi << 4) | lo);
        }
        return true;
    }

    bool SetOverlayFromArgs(std::vector<ScriptOverlayText> &items,
                            size_t *total_count,
                            const std::function<std::string(size_t)> &arg,
                            std::string *error)
    {
        std::string id, text;
        if (!DecodeStringArg(arg(0), &id) || id.empty()) {
            if (error) *error = "overlay id must be non-empty UTF-8 text";
            return false;
        }
        if (!DecodeStringArg(arg(3), &text)) {
            if (error) *error = "bad overlay text encoding";
            return false;
        }
        ImU32 color = IM_COL32(255,255,255,255);
        ImU32 background = IM_COL32(0,0,0,0);
        if (!ParseRgbaColor(arg(4), &color)) {
            if (error) *error = "color must be #RRGGBB or #RRGGBBAA";
            return false;
        }
        if (!ParseRgbaColor(arg(6), &background)) {
            if (error) *error = "background must be #RRGGBB or #RRGGBBAA";
            return false;
        }
        ScriptOverlayText *item = FindOverlay(items, id);
        if (!item) {
            items.emplace_back();
            item = &items.back();
            item->id = id;
            if (total_count) ++*total_count;
        }
        item->x = (float)atof(arg(1).c_str());
        item->y = (float)atof(arg(2).c_str());
        item->text = std::move(text);
        item->color = color;
        item->scale = std::max(0.25f, std::min(8.0f,
            (float)atof(arg(5).c_str())));
        item->background = background;
        return true;
    }

    static bool RemoveOverlayById(std::vector<ScriptOverlayText> &items,
                                  size_t *total_count,
                                  const std::string &encoded_id,
                                  std::string *error)
    {
        std::string id;
        if (!DecodeStringArg(encoded_id, &id)) {
            if (error) *error = "bad overlay id encoding";
            return false;
        }
        auto it = std::find_if(items.begin(), items.end(),
            [&](const ScriptOverlayText &o) { return o.id == id; });
        if (it != items.end()) {
            items.erase(it);
            if (total_count && *total_count) --*total_count;
        }
        return true;
    }

    static void ClearOverlayList(std::vector<ScriptOverlayText> &items,
                                 size_t *total_count)
    {
        if (total_count) {
            *total_count = *total_count >= items.size()
                ? *total_count - items.size() : 0;
        }
        items.clear();
    }

    static ScriptPrimitive *FindPrimitive(std::vector<ScriptPrimitive> &items,
                                          const std::string &id)
    {
        for (auto &item : items) {
            if (item.id == id) {
                return &item;
            }
        }
        return nullptr;
    }

    static void ClearPrimitiveList(std::vector<ScriptPrimitive> &items,
                                   size_t *total_count)
    {
        for (auto &item : items) {
            DestroyPrimitiveTexture(item);
        }
        if (total_count) {
            *total_count = *total_count >= items.size()
                ? *total_count - items.size() : 0;
        }
        items.clear();
    }

    static bool RemovePrimitiveById(std::vector<ScriptPrimitive> &items,
                                    size_t *total_count,
                                    const std::string &id)
    {
        auto it = std::find_if(items.begin(), items.end(),
            [&](const ScriptPrimitive &item) { return item.id == id; });
        if (it == items.end()) {
            return false;
        }
        DestroyPrimitiveTexture(*it);
        items.erase(it);
        if (total_count && *total_count) {
            --*total_count;
        }
        return true;
    }

    ScriptPrimitive *GetPrimitive(ScriptEntry &entry, bool external,
                                  const std::string &id,
                                  ScriptPrimitiveType type)
    {
        auto &items = external ? entry.external_primitives : entry.primitives;
        size_t *count = external ? &m_external_primitive_count
                                 : &m_primitive_count;
        ScriptPrimitive *item = FindPrimitive(items, id);
        if (!item) {
            items.emplace_back();
            item = &items.back();
            item->id = id;
            ++*count;
        } else if (item->type == ScriptPrimitiveType::Image &&
                   type != ScriptPrimitiveType::Image) {
            DestroyPrimitiveTexture(*item);
        }
        item->type = type;
        if (external && !m_external_open) {
            m_external_open = true;
            m_external_auto_detach = true;
        }
        return item;
    }

    static bool ParseWatchValueType(const std::string &name,
                                    ScriptWatchValueType *out)
    {
        if (!out) return false;
        struct Entry { const char *name; ScriptWatchValueType type; };
        static constexpr Entry types[] = {
            {"u8", ScriptWatchValueType::U8}, {"i8", ScriptWatchValueType::I8},
            {"hex8", ScriptWatchValueType::Hex8},
            {"u16", ScriptWatchValueType::U16}, {"i16", ScriptWatchValueType::I16},
            {"hex16", ScriptWatchValueType::Hex16},
            {"u32", ScriptWatchValueType::U32}, {"i32", ScriptWatchValueType::I32},
            {"hex32", ScriptWatchValueType::Hex32},
            {"f32", ScriptWatchValueType::F32}, {"f64", ScriptWatchValueType::F64},
        };
        for (const Entry &entry : types) {
            if (name == entry.name) {
                *out = entry.type;
                return true;
            }
        }
        return false;
    }

    static bool ReadWatchNumeric(const ScriptMemoryWatch &watch,
                                 double *value)
    {
        if (!value) return false;
        auto read = [&](void *buf, size_t len) {
            return watch.virt ? xemu_virt_read(watch.addr, buf, len)
                              : xemu_phys_read(watch.addr, buf, len);
        };

        switch (watch.parsed_type) {
        case ScriptWatchValueType::U8:
        case ScriptWatchValueType::I8:
        case ScriptWatchValueType::Hex8: {
            uint8_t v = 0;
            if (read(&v, sizeof(v)) != (ssize_t)sizeof(v)) return false;
            *value = watch.parsed_type == ScriptWatchValueType::I8
                ? (double)(int8_t)v : (double)v;
            return true;
        }
        case ScriptWatchValueType::U16:
        case ScriptWatchValueType::I16:
        case ScriptWatchValueType::Hex16: {
            uint16_t v = 0;
            if (read(&v, sizeof(v)) != (ssize_t)sizeof(v)) return false;
            *value = watch.parsed_type == ScriptWatchValueType::I16
                ? (double)(int16_t)v : (double)v;
            return true;
        }
        case ScriptWatchValueType::U32:
        case ScriptWatchValueType::I32:
        case ScriptWatchValueType::Hex32: {
            uint32_t v = 0;
            if (read(&v, sizeof(v)) != (ssize_t)sizeof(v)) return false;
            *value = watch.parsed_type == ScriptWatchValueType::I32
                ? (double)(int32_t)v : (double)v;
            return true;
        }
        case ScriptWatchValueType::F32: {
            float v = 0.0f;
            if (read(&v, sizeof(v)) != (ssize_t)sizeof(v)) return false;
            *value = (double)v;
            return std::isfinite(*value);
        }
        case ScriptWatchValueType::F64: {
            double v = 0.0;
            if (read(&v, sizeof(v)) != (ssize_t)sizeof(v)) return false;
            *value = v;
            return std::isfinite(*value);
        }
        }
        return false;
    }

    static std::string FormatWatchValue(const ScriptMemoryWatch &watch,
                                        double value)
    {
        char value_buf[64];
        switch (watch.parsed_type) {
        case ScriptWatchValueType::Hex8:
            snprintf(value_buf, sizeof(value_buf), "0x%02X", (unsigned)(uint8_t)value);
            break;
        case ScriptWatchValueType::Hex16:
            snprintf(value_buf, sizeof(value_buf), "0x%04X", (unsigned)(uint16_t)value);
            break;
        case ScriptWatchValueType::Hex32:
            snprintf(value_buf, sizeof(value_buf), "0x%08X", (uint32_t)value);
            break;
        case ScriptWatchValueType::F32:
        case ScriptWatchValueType::F64:
            snprintf(value_buf, sizeof(value_buf), "%.3f", value);
            break;
        default:
            snprintf(value_buf, sizeof(value_buf), "%lld", (long long)value);
            break;
        }

        std::string out;
        out.reserve(watch.prefix.size() + strlen(value_buf) + watch.suffix.size());
        out += watch.prefix;
        out += value_buf;
        out += watch.suffix;
        return out;
    }

    void UpdateMemoryWatches()
    {
        /* One translation-cache generation per sampling batch.  All virtual
         * watches in this frame then share the same page translations. */
        xemu_guestmem_invalidate_cache();

        for (auto &entry : m_scripts) {
            for (auto &watch : entry.watches) {
                double value = 0.0;
                const bool mapped = ReadWatchNumeric(watch, &value);
                const bool changed = !watch.have_cached_sample ||
                    watch.cached_mapped != mapped ||
                    (mapped && watch.cached_value != value);

                if (watch.kind == ScriptWatchKind::Text) {
                    auto &items = watch.external ? entry.external_overlays
                                                 : entry.overlays;
                    size_t *count = watch.external ? &m_external_overlay_count
                                                   : &m_overlay_count;
                    ScriptOverlayText *item = FindOverlay(items, watch.visual_id);
                    if (!item) {
                        items.emplace_back();
                        item = &items.back();
                        item->id = watch.visual_id;
                        item->x = watch.x;
                        item->y = watch.y;
                        item->color = watch.color;
                        item->scale = watch.scale;
                        item->background = watch.background;
                        ++*count;
                    }
                    if (changed) {
                        item->text = mapped
                            ? FormatWatchValue(watch, value)
                            : watch.prefix + "<unmapped>" + watch.suffix;
                    }
                } else {
                    ScriptPrimitive *item = GetPrimitive(entry, watch.external,
                                                          watch.visual_id,
                                                          ScriptPrimitiveType::Bar);
                    if (!watch.visual_initialized) {
                        item->x = watch.x; item->y = watch.y;
                        item->w = watch.w; item->h = watch.h;
                        item->min_value = watch.min_value;
                        item->max_value = watch.max_value;
                        item->color2 = watch.bar_color;
                        item->background = watch.background;
                        item->border = watch.border;
                        item->rounding = 3.0f;
                        watch.visual_initialized = true;
                    }
                    if (changed) {
                        item->value = mapped ? (float)value : watch.min_value;
                    }
                }

                watch.have_cached_sample = true;
                watch.cached_mapped = mapped;
                if (mapped) watch.cached_value = value;

                if (watch.external && !m_external_open) {
                    m_external_open = true;
                    m_external_auto_detach = true;
                }
            }
        }
    }

    bool RemoveWatchById(ScriptEntry &entry, const std::string &id)
    {
        auto it = std::find_if(entry.watches.begin(), entry.watches.end(),
            [&](const ScriptMemoryWatch &w) { return w.id == id; });
        if (it == entry.watches.end()) {
            return false;
        }
        const bool external = it->external;
        const ScriptWatchKind kind = it->kind;
        const std::string &visual_id = it->visual_id;
        if (kind == ScriptWatchKind::Text) {
            auto &items = external ? entry.external_overlays : entry.overlays;
            size_t *count = external ? &m_external_overlay_count : &m_overlay_count;
            auto vit = std::find_if(items.begin(), items.end(), [&](const ScriptOverlayText &o) {
                return o.id == visual_id;
            });
            if (vit != items.end()) {
                items.erase(vit); if (*count) --*count;
            }
        } else {
            auto &items = external ? entry.external_primitives : entry.primitives;
            size_t *count = external ? &m_external_primitive_count : &m_primitive_count;
            RemovePrimitiveById(items, count, visual_id);
        }
        entry.watches.erase(it);
        if (m_watch_count) --m_watch_count;
        return true;
    }

    void ClearWatches(ScriptEntry &entry)
    {
        while (!entry.watches.empty()) {
            RemoveWatchById(entry, entry.watches.back().id);
        }
    }

    static std::string CurrentTitleIdString()
    {
        uint32_t title_id = 0;
        if (!xemu_get_xbe_title_id(&title_id)) {
            return std::string();
        }
        char buf[16];
        snprintf(buf, sizeof(buf), "%08X", title_id);
        return buf;
    }

    static std::string CurrentRunstateString()
    {
        return RunState_str(runstate_get());
    }

#ifdef CONFIG_XEMU_FEATURE_DEBUG_TOOLS
    static std::string DebugEventPayload(const XemuDbgStopEvent &ev)
    {
        const char *type = "debug";
        if (ev.type == XEMU_DBG_STOP_BREAKPOINT) type = "breakpoint";
        else if (ev.type == XEMU_DBG_STOP_WATCHPOINT) type = "watchpoint";
        else if (ev.type == XEMU_DBG_STOP_STEP) type = "step";
        char buf[192];
        snprintf(buf, sizeof(buf), "%" PRIu64 ",%s,%u,%u,%u,%d,%d",
                 ev.sequence, type, ev.pc, ev.address, ev.length, ev.flags,
                 ev.physical ? 1 : 0);
        return buf;
    }

#endif

    void HandleApiRequest(ScriptEntry &entry, const std::string &line)
    {
        auto p = SplitPipe(line);
        if (p.size() < 3 || p[0] != "@@XEMUAPI") return;
        uint64_t id = strtoull(p[1].c_str(), nullptr, 10);
        const std::string &cmd = p[2];
        static const std::string kEmptyArg;
        auto arg = [&](size_t i) -> const std::string & {
            return i + 3 < p.size() ? p[i + 3] : kEmptyArg;
        };
        auto num = [&](size_t i)->uint64_t {
            return strtoull(arg(i).c_str(), nullptr, 0);
        };
        auto fnum = [&](size_t i)->double {
            return strtod(arg(i).c_str(), nullptr);
        };
        auto is_external = [&](size_t i)->bool {
            const std::string &v = arg(i);
            return v == "display" || v == "external" || v == "window";
        };
        bool ok = true;
        std::string out;

        if (cmd == "api_version") out = "2";
        else if (cmd == "capabilities") {
#ifdef CONFIG_XEMU_FEATURE_DEBUG_TOOLS
            out = "memory,draw,images,watches,controller,events,external,debug";
#else
            out = "memory,draw,images,watches,controller,events,external";
#endif
        }
        else if (cmd == "frame") out = std::to_string(xemu_tas_frame());
        else if (cmd == "lag_count") out = std::to_string(xemu_tas_lag_count());
        else if (cmd == "runstate") out = CurrentRunstateString();
        else if (cmd == "tas_enabled") out = xemu_tas_enabled() ? "1" : "0";
        else if (cmd == "set_tas_enabled") xemu_tas_set_enabled(num(0) != 0);
        else if (cmd == "pause") { if (runstate_is_running()) vm_stop(RUN_STATE_PAUSED); }
        else if (cmd == "resume") { if (!runstate_is_running()) vm_start(); }
        else if (cmd == "frame_advance") {
            uint32_t n = (uint32_t)std::max<uint64_t>(1,
                std::min<uint64_t>(num(0), 1000000));
            if (runstate_is_running()) vm_stop(RUN_STATE_PAUSED);
            if (!xemu_tas_enabled()) xemu_tas_set_enabled(true);
            xemu_tas_request_frame_advance(n); vm_start();
        }
        else if (cmd == "wait_frame") {
            uint64_t target = num(0);
            if (xemu_tas_frame() >= target) {
                out = std::to_string(xemu_tas_frame());
            } else {
                entry.wait_kind = ScriptWaitKind::Frame;
                entry.wait_id = id;
                entry.wait_target = target;
                return;
            }
        }
        else if (cmd == "wait_runstate") {
            std::string previous;
            if (!DecodeStringArg(arg(0), &previous)) {
                ok = false; out = "bad runstate encoding";
            } else if (CurrentRunstateString() != previous) {
                out = CurrentRunstateString();
            } else {
                entry.wait_kind = ScriptWaitKind::Runstate;
                entry.wait_id = id;
                entry.wait_runstate = (int)runstate_get();
                return;
            }
        }
        else if (cmd == "wait_title") {
            std::string previous;
            if (!DecodeStringArg(arg(0), &previous)) {
                ok = false; out = "bad title encoding";
            } else if (CurrentTitleIdString() != previous) {
                out = CurrentTitleIdString();
            } else {
                entry.wait_kind = ScriptWaitKind::Title;
                entry.wait_id = id;
                entry.wait_title = previous;
                return;
            }
        }
        else if (cmd == "title_id") {
            out = CurrentTitleIdString();
            if (out.empty()) { ok = false; out = "no running XBE"; }
        }
        else if (cmd == "read8" || cmd == "read16" || cmd == "read32") {
            /* Legacy commands retained for existing scripts: physical RAM. */
            int sz = cmd == "read8" ? 1 : cmd == "read16" ? 2 : 4;
            uint64_t v = 0;
            if (xemu_phys_read((uint32_t)num(0), &v, sz) != sz) {
                ok = false; out = "memory read failed";
            } else out = std::to_string(v);
        }
        else if (cmd == "write8" || cmd == "write16" || cmd == "write32") {
            int sz = cmd == "write8" ? 1 : cmd == "write16" ? 2 : 4;
            uint64_t v = num(1);
            if (xemu_phys_write((uint32_t)num(0), &v, sz) != sz) {
                ok = false; out = "memory write failed";
            }
        }
        else if (cmd == "read_space") {
            xemu_guestmem_invalidate_cache();
            const uint32_t addr = (uint32_t)num(0);
            const size_t sz = (size_t)num(1);
            const bool virt = num(2) != 0;
            if (sz != 1 && sz != 2 && sz != 4) {
                ok = false; out = "read_space size must be 1, 2, or 4";
            } else {
                uint64_t v = 0;
                ssize_t got = virt ? xemu_virt_read(addr, &v, sz)
                                   : xemu_phys_read(addr, &v, sz);
                if (got != (ssize_t)sz) { ok = false; out = "memory read failed"; }
                else out = std::to_string(v);
            }
        }
        else if (cmd == "write_space") {
            xemu_guestmem_invalidate_cache();
            const uint32_t addr = (uint32_t)num(0);
            const size_t sz = (size_t)num(1);
            const bool virt = num(2) != 0;
            uint64_t v = num(3);
            if (sz != 1 && sz != 2 && sz != 4) {
                ok = false; out = "write_space size must be 1, 2, or 4";
            } else {
                ssize_t put = virt ? xemu_virt_write(addr, &v, sz)
                                   : xemu_phys_write(addr, &v, sz);
                if (put != (ssize_t)sz) { ok = false; out = "memory write failed"; }
            }
        }
        else if (cmd == "read_bytes") {
            xemu_guestmem_invalidate_cache();
            const uint32_t addr = (uint32_t)num(0);
            const size_t sz = std::min<size_t>((size_t)num(1), 1024 * 1024);
            const bool virt = num(2) != 0;
            std::vector<uint8_t> bytes(sz);
            ssize_t got = sz ? (virt ? xemu_virt_read(addr, bytes.data(), sz)
                                     : xemu_phys_read(addr, bytes.data(), sz)) : 0;
            if (got != (ssize_t)sz) { ok = false; out = "memory read failed"; }
            else out = EncodeHex(bytes.data(), bytes.size());
        }
        else if (cmd == "write_bytes") {
            xemu_guestmem_invalidate_cache();
            const uint32_t addr = (uint32_t)num(0);
            const bool virt = num(1) != 0;
            std::vector<uint8_t> bytes;
            if (!DecodeHex(arg(2), &bytes) || bytes.size() > 1024 * 1024) {
                ok = false; out = "bad or oversized byte payload";
            } else {
                ssize_t put = bytes.empty() ? 0 :
                    (virt ? xemu_virt_write(addr, bytes.data(), bytes.size())
                          : xemu_phys_write(addr, bytes.data(), bytes.size()));
                if (put != (ssize_t)bytes.size()) { ok = false; out = "memory write failed"; }
            }
        }
        else if (cmd == "read_float") {
            xemu_guestmem_invalidate_cache();
            const uint32_t addr=(uint32_t)num(0);const size_t sz=(size_t)num(1);const bool virt=num(2)!=0;
            if(sz==4){float v=0;ssize_t got=virt?xemu_virt_read(addr,&v,4):xemu_phys_read(addr,&v,4);if(got!=4){ok=false;out="memory read failed";}else{std::ostringstream ss;ss<<std::setprecision(9)<<v;out=ss.str();}}
            else if(sz==8){double v=0;ssize_t got=virt?xemu_virt_read(addr,&v,8):xemu_phys_read(addr,&v,8);if(got!=8){ok=false;out="memory read failed";}else{std::ostringstream ss;ss<<std::setprecision(17)<<v;out=ss.str();}}
            else {ok=false;out="float size must be 4 or 8";}
        }
        else if (cmd == "write_float") {
            xemu_guestmem_invalidate_cache();
            const uint32_t addr=(uint32_t)num(0);const size_t sz=(size_t)num(1);const bool virt=num(2)!=0;const double dv=fnum(3);
            if(sz==4){float v=(float)dv;ssize_t put=virt?xemu_virt_write(addr,&v,4):xemu_phys_write(addr,&v,4);if(put!=4){ok=false;out="memory write failed";}}
            else if(sz==8){double v=dv;ssize_t put=virt?xemu_virt_write(addr,&v,8):xemu_phys_write(addr,&v,8);if(put!=8){ok=false;out="memory write failed";}}
            else {ok=false;out="float size must be 4 or 8";}
        }
        else if (cmd == "input_get") {
            uint8_t port = (uint8_t)num(0), r[XEMU_TAS_XID_REPORT_SIZE]{};
            if (!xemu_tas_get_last_xid_report(port, r, sizeof(r))) {
                ok = false; out = "no XID report yet";
            } else out = EncodeHex(r, sizeof(r));
        }
        else if (cmd == "input_set") {
            uint8_t port = (uint8_t)num(0); std::vector<uint8_t> r;
            if (port >= XEMU_TAS_MAX_PORTS || !DecodeHex(arg(1), &r) ||
                r.size() != XEMU_TAS_XID_REPORT_SIZE) {
                ok = false; out = "expected port 0-3 and 40 hex chars";
            } else {
                if (!xemu_tas_enabled()) xemu_tas_set_enabled(true);
                xemu_tas_set_xid_report(port, r.data(), r.size());
            }
        }
        else if (cmd == "input_release") {
            uint8_t port = (uint8_t)num(0);
            if (port >= XEMU_TAS_MAX_PORTS) { ok = false; out = "port must be 0-3"; }
            else xemu_tas_clear_xid_report(port);
        }
        else if (cmd == "input_release_all") {
            xemu_tas_clear_all_xid_reports();
        }
        else if (cmd == "input_state") {
            uint8_t port = (uint8_t)num(0), r[XEMU_TAS_XID_REPORT_SIZE]{};
            if (!xemu_tas_get_last_xid_report(port, r, sizeof(r))) {
                ok = false; out = "no XID report yet";
            } else {
                uint16_t buttons = 0; int16_t lx=0,ly=0,rx=0,ry=0;
                memcpy(&buttons,&r[2],2); memcpy(&lx,&r[12],2); memcpy(&ly,&r[14],2);
                memcpy(&rx,&r[16],2); memcpy(&ry,&r[18],2);
                std::ostringstream ss;
                ss << buttons;
                for (int i=0;i<8;++i) ss << ',' << (unsigned)r[4+i];
                ss << ',' << lx << ',' << ly << ',' << rx << ',' << ry
                   << ',' << ((buttons & (1u<<4)) ? 1 : 0)
                   << ',' << ((buttons & (1u<<5)) ? 1 : 0);
                out = ss.str();
            }
        }
        else if (cmd == "screen_size") {
            int w = 0, h = 0; SDL_Window *window = xemu_get_window();
            if (!window || !SDL_GetWindowSize(window, &w, &h)) {
                ok = false; out = "main xemu window is unavailable";
            } else out = std::to_string(w) + "," + std::to_string(h);
        }
        else if (cmd == "snapshot_save" || cmd == "snapshot_load" ||
                 cmd == "notify" || cmd == "overlay_text") {
            if (cmd == "overlay_text") {
                if (p.size() < 6) { ok=false; out="overlay_text requires x,y,text"; }
                else {
                    std::string text;
                    if (!DecodeStringArg(arg(2), &text)) { ok=false; out="bad text encoding"; }
                    else {
                        ScriptOverlayText o; o.x=(float)fnum(0); o.y=(float)fnum(1);
                        o.text=std::move(text); entry.overlays.push_back(std::move(o));
                        ++m_overlay_count;
                    }
                }
            } else {
                std::vector<uint8_t> bytes;
                if (!DecodeHex(arg(0), &bytes)) { ok=false; out="bad text encoding"; }
                else {
                    std::string text((const char*)bytes.data(), bytes.size());
                    if (cmd == "notify") xemu_queue_notification(text.c_str());
                    else {
                        Error *err = nullptr;
                        if (cmd == "snapshot_save") xemu_snapshots_save(text.c_str(), &err);
                        else xemu_snapshots_load(text.c_str(), &err);
                        if (err) { ok=false; out=error_get_pretty(err); error_free(err); }
                    }
                }
            }
        }
        else if (cmd == "overlay_set") {
            if (p.size() < 10) { ok=false; out="overlay_set requires id,x,y,text,color,scale,background"; }
            else ok = SetOverlayFromArgs(entry.overlays, &m_overlay_count, arg, &out);
        }
        else if (cmd == "overlay_remove") {
            ok = RemoveOverlayById(entry.overlays, &m_overlay_count, arg(0), &out);
        }
        else if (cmd == "overlay_clear") {
            ClearOverlayList(entry.overlays, &m_overlay_count);
        }
        else if (cmd == "display_open") {
            std::string title;
            if (!DecodeStringArg(arg(0), &title)) { ok=false; out="bad display title encoding"; }
            else {
                m_external_title = title.empty() ?
                    (IsLua() ? "Lua Script Display" : "Python Script Display") : title;
                m_external_width = std::max(320, std::min(3840, (int)num(1)));
                m_external_height = std::max(180, std::min(2160, (int)num(2)));
                m_external_open = true; m_external_auto_detach = num(3) != 0;
            }
        }
        else if (cmd == "display_close") {
            m_external_open = false; m_external_auto_detach = false;
        }
        else if (cmd == "display_size") {
            const int w = m_external_canvas_width > 0 ? m_external_canvas_width : m_external_width;
            const int h = m_external_canvas_height > 0 ? m_external_canvas_height : m_external_height;
            out = std::to_string(w) + "," + std::to_string(h);
        }
        else if (cmd == "display_text") {
            if (p.size() < 10) { ok=false; out="display_text requires id,x,y,text,color,scale,background"; }
            else {
                ok = SetOverlayFromArgs(entry.external_overlays,
                                        &m_external_overlay_count, arg, &out);
                if (ok && !m_external_open) { m_external_open=true; m_external_auto_detach=true; }
            }
        }
        else if (cmd == "display_remove") {
            ok = RemoveOverlayById(entry.external_overlays,
                                   &m_external_overlay_count, arg(0), &out);
        }
        else if (cmd == "display_clear") {
            ClearOverlayList(entry.external_overlays, &m_external_overlay_count);
        }
        else if (cmd == "draw_line" || cmd == "draw_rect" ||
                 cmd == "draw_circle" || cmd == "draw_bar" ||
                 cmd == "draw_crosshair" || cmd == "draw_image") {
            const bool external = is_external(0);
            std::string draw_id;
            if (!DecodeStringArg(arg(1), &draw_id) || draw_id.empty()) {
                ok=false; out="draw id must be non-empty UTF-8 text";
            } else {
                ScriptPrimitiveType type = ScriptPrimitiveType::Line;
                if (cmd == "draw_rect") type = ScriptPrimitiveType::Rect;
                else if (cmd == "draw_circle") type = ScriptPrimitiveType::Circle;
                else if (cmd == "draw_bar") type = ScriptPrimitiveType::Bar;
                else if (cmd == "draw_crosshair") type = ScriptPrimitiveType::Crosshair;
                else if (cmd == "draw_image") type = ScriptPrimitiveType::Image;
                ScriptPrimitive *item = GetPrimitive(entry, external, draw_id, type);
                if (cmd == "draw_line") {
                    item->x=fnum(2);item->y=fnum(3);item->x2=fnum(4);item->y2=fnum(5);
                    ok=ParseRgbaColor(arg(6),&item->color); item->thickness=fnum(7);
                } else if (cmd == "draw_rect") {
                    item->x=fnum(2);item->y=fnum(3);item->w=fnum(4);item->h=fnum(5);
                    ok=ParseRgbaColor(arg(6),&item->color);item->filled=num(7)!=0;
                    item->thickness=fnum(8);item->rounding=fnum(9);
                } else if (cmd == "draw_circle") {
                    item->x=fnum(2);item->y=fnum(3);item->radius=fnum(4);
                    ok=ParseRgbaColor(arg(5),&item->color);item->filled=num(6)!=0;
                    item->thickness=fnum(7);
                } else if (cmd == "draw_bar") {
                    item->x=fnum(2);item->y=fnum(3);item->w=fnum(4);item->h=fnum(5);
                    item->value=fnum(6);item->min_value=fnum(7);item->max_value=fnum(8);
                    ok=ParseRgbaColor(arg(9),&item->color2) &&
                       ParseRgbaColor(arg(10),&item->background) &&
                       ParseRgbaColor(arg(11),&item->border);
                    item->vertical=num(12)!=0;item->rounding=3.0f;
                } else if (cmd == "draw_crosshair") {
                    item->x=fnum(2);item->y=fnum(3);item->radius=fnum(4);
                    ok=ParseRgbaColor(arg(5),&item->color);item->thickness=fnum(6);
                    item->gap=fnum(7);item->filled=num(8)!=0;
                } else if (cmd == "draw_image") {
                    std::string path;
                    if (!DecodeStringArg(arg(2), &path) || path.empty()) {
                        ok=false;out="image path must be UTF-8 text";
                    } else {
                        try {
                            std::filesystem::path ip(path);
                            if (ip.is_relative()) {
                                ip = std::filesystem::path(entry.path).parent_path() / ip;
                            }
                            path = ip.lexically_normal().string();
                        } catch (...) {
                        }
                        if (item->image_path != path) DestroyPrimitiveTexture(*item);
                        item->image_path=path;item->x=fnum(3);item->y=fnum(4);
                        item->w=fnum(5);item->h=fnum(6);
                        ok=ParseRgbaColor(arg(7),&item->color);
                    }
                }
                if (!ok && out.empty()) out="invalid color; use #RRGGBB or #RRGGBBAA";
            }
        }
        else if (cmd == "draw_remove") {
            const bool external = is_external(0); std::string draw_id;
            if (!DecodeStringArg(arg(1),&draw_id)) { ok=false;out="bad draw id encoding"; }
            else {
                auto &items = external ? entry.external_primitives : entry.primitives;
                size_t *count = external ? &m_external_primitive_count : &m_primitive_count;
                RemovePrimitiveById(items,count,draw_id);
            }
        }
        else if (cmd == "draw_clear") {
            const bool external = is_external(0);
            if (external) ClearPrimitiveList(entry.external_primitives,&m_external_primitive_count);
            else ClearPrimitiveList(entry.primitives,&m_primitive_count);
        }
        else if (cmd == "watch_text" || cmd == "watch_bar") {
            std::string watch_id;
            if (!DecodeStringArg(arg(0),&watch_id) || watch_id.empty()) {
                ok=false;out="watch id must be non-empty UTF-8 text";
            } else {
                auto it=std::find_if(entry.watches.begin(),entry.watches.end(),
                    [&](const ScriptMemoryWatch &w){return w.id==watch_id;});
                if (it != entry.watches.end()) {
                    RemoveWatchById(entry, watch_id);
                }
                entry.watches.emplace_back();
                ScriptMemoryWatch &w=entry.watches.back();
                w.id=watch_id;
                ++m_watch_count;
                w.kind = cmd=="watch_text" ? ScriptWatchKind::Text : ScriptWatchKind::Bar;
                w.visual_id = (w.kind == ScriptWatchKind::Text
                    ? "__watch_text:" : "__watch_bar:") + w.id;
                w.addr=(uint32_t)num(1);w.value_type=arg(2);w.virt=num(3)!=0;
                w.external=is_external(4);
                if(!ParseWatchValueType(w.value_type, &w.parsed_type)){
                    ok=false;out="unsupported watch type";
                }
                else if(cmd=="watch_text") {
                    w.x=fnum(5);w.y=fnum(6);
                    if(!DecodeStringArg(arg(7),&w.prefix)||!DecodeStringArg(arg(8),&w.suffix)) {ok=false;out="bad watch text encoding";}
                    else if(!ParseRgbaColor(arg(9),&w.color)||!ParseRgbaColor(arg(11),&w.background)) {ok=false;out="invalid watch color";}
                    else {w.scale=std::max(0.25f,std::min(8.0f,(float)fnum(10)));}
                } else {
                    w.x=fnum(5);w.y=fnum(6);w.w=fnum(7);w.h=fnum(8);
                    w.min_value=fnum(9);w.max_value=fnum(10);
                    if(!ParseRgbaColor(arg(11),&w.bar_color)||!ParseRgbaColor(arg(12),&w.background)||!ParseRgbaColor(arg(13),&w.border)) {ok=false;out="invalid watch bar color";}
                }
                if (!ok) {
                    entry.watches.pop_back();
                    if (m_watch_count) --m_watch_count;
                } else if (w.external && !m_external_open) {
                    m_external_open=true;
                    m_external_auto_detach=true;
                }
            }
        }
        else if (cmd == "watch_remove") {
            std::string watch_id;if(!DecodeStringArg(arg(0),&watch_id)){ok=false;out="bad watch id encoding";}
            else RemoveWatchById(entry,watch_id);
        }
        else if (cmd == "watch_clear") {
            ClearWatches(entry);
        }
        else if (cmd == "controller_show") {
            std::string cid;
            if(!DecodeStringArg(arg(0),&cid)||cid.empty()){ok=false;out="controller id must be text";}
            else {
                uint8_t port=(uint8_t)num(1);
                if(port>=XEMU_TAS_MAX_PORTS){ok=false;out="controller port must be 0-3";}
                else {
                    auto it=std::find_if(entry.controllers.begin(),entry.controllers.end(),[&](const ScriptControllerWidget &c){return c.id==cid;});
                    if(it==entry.controllers.end()){entry.controllers.emplace_back();it=entry.controllers.end()-1;it->id=cid;++m_controller_count;}
                    it->port=port;it->x=fnum(2);it->y=fnum(3);it->scale=fnum(4);it->external=is_external(5);it->labels=num(6)!=0;
                    if(it->external&&!m_external_open){m_external_open=true;m_external_auto_detach=true;}
                }
            }
        }
        else if (cmd == "controller_remove") {
            std::string cid;if(!DecodeStringArg(arg(0),&cid)){ok=false;out="bad controller id encoding";}
            else {auto it=std::find_if(entry.controllers.begin(),entry.controllers.end(),[&](const ScriptControllerWidget &c){return c.id==cid;});if(it!=entry.controllers.end()){entry.controllers.erase(it);if(m_controller_count)--m_controller_count;}}
        }
        else if (cmd == "controller_clear") {
            if(m_controller_count>=entry.controllers.size())m_controller_count-=entry.controllers.size();else m_controller_count=0;
            entry.controllers.clear();
        }
#ifdef CONFIG_XEMU_FEATURE_DEBUG_TOOLS
        else if (cmd == "debug_available") out = "1";
        else if (cmd == "debug_regs") {
            XemuDbgRegs r{};xemu_dbg_get_regs(&r);
            if(!r.valid){ok=false;out="no CPU";} else {
                std::ostringstream ss;
                ss << "eax="<<r.eax<<",ecx="<<r.ecx<<",edx="<<r.edx<<",ebx="<<r.ebx
                   << ",esp="<<r.esp<<",ebp="<<r.ebp<<",esi="<<r.esi<<",edi="<<r.edi
                   << ",eip="<<r.eip<<",eflags="<<r.eflags<<",cr0="<<r.cr0<<",cr2="<<r.cr2
                   << ",cr3="<<r.cr3<<",cr4="<<r.cr4;out=ss.str();
            }
        }
        else if (cmd == "debug_set_reg") { if(!xemu_dbg_set_reg(arg(0).c_str(),(uint32_t)num(1))){ok=false;out="unknown register or no CPU";} }
        else if (cmd == "debug_disasm") {
            int count=std::max(1,std::min(256,(int)num(1)));std::vector<XemuDbgInsn> insn(count);
            int got=xemu_dbg_disasm((uint32_t)num(0),count,insn.data());std::ostringstream ss;
            for(int i=0;i<got;++i){if(i)ss<<';';ss<<insn[i].addr<<','<<EncodeHex(insn[i].bytes,insn[i].len)<<','<<EncodeHex(insn[i].mnemonic,strlen(insn[i].mnemonic))<<','<<EncodeHex(insn[i].ops,strlen(insn[i].ops));}out=ss.str();
        }
        else if (cmd == "debug_bp_add") { if(!xemu_dbg_bp_insert_space((uint32_t)num(0),num(1)!=0)){ok=false;out="breakpoint insert failed";} }
        else if (cmd == "debug_bp_remove") { if(!xemu_dbg_bp_remove_space((uint32_t)num(0),num(1)!=0)){ok=false;out="breakpoint remove failed";} }
        else if (cmd == "debug_wp_add") { if(!xemu_dbg_wp_insert_space((uint32_t)num(0),(uint32_t)num(1),(int)num(2),num(3)!=0)){ok=false;out="watchpoint insert failed";} }
        else if (cmd == "debug_wp_remove") { if(!xemu_dbg_wp_remove_space((uint32_t)num(0),(uint32_t)num(1),(int)num(2),num(3)!=0)){ok=false;out="watchpoint remove failed";} }
        else if (cmd == "debug_step") xemu_dbg_step();
        else if (cmd == "debug_step_over") xemu_dbg_step_over();
        else if (cmd == "debug_step_out") { const char *e=xemu_dbg_step_out();if(e){ok=false;out=e;} }
        else if (cmd == "debug_run_to") xemu_dbg_run_to((uint32_t)num(0));
        else if (cmd == "debug_to_phys") {uint32_t v=0;if(!xemu_dbg_to_phys((uint32_t)num(0),&v)){ok=false;out="address is not mapped";}else out=std::to_string(v);}
        else if (cmd == "debug_to_virt") {uint32_t v=0;if(!xemu_dbg_to_virt((uint32_t)num(0),&v)){ok=false;out="physical address has no mapped virtual alias";}else out=std::to_string(v);}
        else if (cmd == "debug_event") {
            XemuDbgStopEvent ev{}; if(xemu_dbg_get_stop_event(&ev)&&ev.valid)out=DebugEventPayload(ev);else out.clear();
        }
        else if (cmd == "debug_wait") {
            uint64_t since=num(0);XemuDbgStopEvent ev{};
            if(xemu_dbg_get_stop_event(&ev)&&ev.valid&&ev.sequence>since)out=DebugEventPayload(ev);
            else {entry.wait_kind=ScriptWaitKind::Debug;entry.wait_id=id;entry.wait_debug_sequence=since;return;}
        }
#else
        else if (cmd == "debug_available") { out = "0"; }
        else if (cmd.rfind("debug_",0)==0) { ok=false;out="xemu debug-tools feature is disabled"; }
#endif
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
            if (entry.wait_kind == ScriptWaitKind::Frame &&
                tas_frame >= entry.wait_target) {
                entry.wait_kind = ScriptWaitKind::None;
                SendApiResponse(entry, entry.wait_id, true, std::to_string(tas_frame));
            } else if (entry.wait_kind == ScriptWaitKind::Runstate &&
                       (int)runstate_get() != entry.wait_runstate) {
                entry.wait_kind = ScriptWaitKind::None;
                SendApiResponse(entry, entry.wait_id, true, CurrentRunstateString());
            } else if (entry.wait_kind == ScriptWaitKind::Title &&
                       CurrentTitleIdString() != entry.wait_title) {
                entry.wait_kind = ScriptWaitKind::None;
                SendApiResponse(entry, entry.wait_id, true, CurrentTitleIdString());
#ifdef CONFIG_XEMU_FEATURE_DEBUG_TOOLS
            } else if (entry.wait_kind == ScriptWaitKind::Debug) {
                XemuDbgStopEvent ev{};
                if (xemu_dbg_get_stop_event(&ev) && ev.valid &&
                    ev.sequence > entry.wait_debug_sequence) {
                    entry.wait_kind = ScriptWaitKind::None;
                    SendApiResponse(entry, entry.wait_id, true,
                                    DebugEventPayload(ev));
                }
#endif
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
                entry.wait_kind = ScriptWaitKind::None;
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
    bool m_script_directory_initialized = false;
    int m_selected = -1;
    int m_running_count = 0;
    size_t m_overlay_count = 0;
    size_t m_external_overlay_count = 0;
    size_t m_primitive_count = 0;
    size_t m_external_primitive_count = 0;
    size_t m_watch_count = 0;
    size_t m_controller_count = 0;
    bool m_external_open = false;
    bool m_external_auto_detach = false;
    int m_external_width = 640;
    int m_external_height = 360;
    int m_external_canvas_width = 640;
    int m_external_canvas_height = 360;
    std::string m_external_title;
    char m_interpreter[1024];
    char m_script_directory[1024];
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
