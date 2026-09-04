/*
 * Xemu custom-fork detachable feature windows
 *
 * Copyright (C) 2026 Joshua-1248/xemu custom fork contributors
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This helper deliberately lives entirely under xemu-features/.  It does not
 * require Dear ImGui's docking/multi-viewport branch and does not patch Xemu's
 * native SDL event loop.  Instead, feature windows that opt in can be hosted
 * in their own SDL3 + OpenGL ImGui context while continuing to use Xemu's
 * existing frontend OpenGL context/resources.
 */
#pragma once

#include "ui/xui/common.hh"
#include "ui/xui/xemu-hud.h"
#include "data/Roboto-Medium.ttf.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace xemu_feature_detach {

struct Host {
    std::string id;
    std::string title;
    std::function<void()> draw;
    bool *open = nullptr;

    bool detached = false;
    bool pending_detach = false;
    bool transfer_drag = false;
    bool moved_after_grace = false;
    uint32_t detached_at_ms = 0;
    uint32_t last_move_ms = 0;

    // When an attached ImGui title-bar drag crosses the main-window boundary,
    // keep the new native host glued to the same mouse gesture. This avoids
    // the old release-then-grab-again tear-off behavior.
    int transfer_drag_offset_x = 0;
    int transfer_drag_offset_y = 0;

    int pending_x = SDL_WINDOWPOS_CENTERED;
    int pending_y = SDL_WINDOWPOS_CENTERED;
    int pending_w = 900;
    int pending_h = 650;

    SDL_Window *window = nullptr;
    SDL_GLContext gl = nullptr;
    ImGuiContext *imgui = nullptr;
    ImFont *default_font = nullptr;
    ImFont *fixed_font = nullptr;
    float font_size = 0.0f;
    float pixel_density = 0.0f;
    SDL_WindowID window_id = 0;
};

/* Only a small fixed set of feature windows is registered. A transparent map
 * lets the every-frame Find/Register path use const char* ids without creating
 * temporary std::string keys (heterogeneous unordered lookup is C++20-only). */
inline std::map<std::string, Host, std::less<>> g_hosts;
inline std::mutex g_event_mutex;
inline std::vector<SDL_Event> g_events;
/* Reused by Pump(). Keeping a second vector lets producer/consumer capacity
 * ping-pong between frames instead of allocating and freeing a local event
 * vector every frame while detached tools are active. */
inline std::vector<SDL_Event> g_dispatch_events;
inline bool g_event_watch_installed = false;
inline std::string g_rendering_id;
inline int g_last_pump_frame = -1;
inline std::atomic_uint g_detached_host_count{0};
/* Set only while one or more attached ImGui windows are waiting to create a
 * native host. This lets Pump() avoid even scanning g_hosts in the overwhelmingly
 * common all-attached/no-drag state. The frontend thread recomputes it after
 * servicing pending requests, so a bool is sufficient. */
inline std::atomic_bool g_pending_detach_work{false};

inline SDL_WindowID EventWindowID(const SDL_Event &event)
{
    if (event.type >= SDL_EVENT_WINDOW_FIRST &&
        event.type <= SDL_EVENT_WINDOW_LAST) {
        return event.window.windowID;
    }

    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        return event.key.windowID;
    case SDL_EVENT_TEXT_EDITING:
        return event.edit.windowID;
    case SDL_EVENT_TEXT_INPUT:
        return event.text.windowID;
    case SDL_EVENT_MOUSE_MOTION:
        return event.motion.windowID;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        return event.button.windowID;
    case SDL_EVENT_MOUSE_WHEEL:
        return event.wheel.windowID;
    default:
        return 0;
    }
}

inline bool SDLCALL EventWatch(void *, SDL_Event *event)
{
    if (!event) {
        return true;
    }
    // The watch remains installed for process lifetime, but when every custom
    // tool is attached there is nothing to route. Avoid window-id queries,
    // mutex traffic and queue growth on the normal Xemu event path.
    if (g_detached_host_count.load(std::memory_order_relaxed) == 0) {
        return true;
    }
    const SDL_WindowID window_id = EventWindowID(*event);
    if (window_id == 0) {
        return true;
    }
    SDL_Window *main_window = xemu_get_window();
    if (main_window && window_id == SDL_GetWindowID(main_window)) {
        return true;
    }

    std::lock_guard<std::mutex> lock(g_event_mutex);

    /* Motion/move events can arrive far faster than we render a detached
     * window. Only the newest position matters to ImGui and to our reattach
     * logic, so coalesce consecutive events for the same native window. This
     * keeps dragging a tool from turning into thousands of queued copies. */
    if (!g_events.empty()) {
        SDL_Event &last = g_events.back();
        const bool same_window = EventWindowID(last) == window_id;
        const bool coalescible =
            (event->type == SDL_EVENT_MOUSE_MOTION &&
             last.type == SDL_EVENT_MOUSE_MOTION) ||
            (event->type == SDL_EVENT_WINDOW_MOVED &&
             last.type == SDL_EVENT_WINDOW_MOVED) ||
            (event->type == SDL_EVENT_WINDOW_RESIZED &&
             last.type == SDL_EVENT_WINDOW_RESIZED) ||
            (event->type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED &&
             last.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED);
        if (same_window && coalescible) {
            last = *event;
            return true;
        }
    }

    // Keep a generous hard ceiling for unusual event storms. This path should
    // now be extremely rare because the high-frequency events above coalesce.
    if (g_events.size() >= 8192) {
        g_events.erase(g_events.begin(), g_events.begin() + 2048);
    }
    g_events.push_back(*event);
    return true;
}

inline void EnsureEventWatch()
{
    if (g_event_watch_installed) {
        return;
    }
    if (SDL_AddEventWatch(EventWatch, nullptr)) {
        g_event_watch_installed = true;
    }
}

inline Host *Find(const char *id)
{
    auto it = g_hosts.find(id ? id : "");
    return it == g_hosts.end() ? nullptr : &it->second;
}

inline const Host *FindConst(const char *id)
{
    auto it = g_hosts.find(id ? id : "");
    return it == g_hosts.end() ? nullptr : &it->second;
}

inline bool IsDetachedPass(const char *id)
{
    return id && !g_rendering_id.empty() && g_rendering_id == id;
}

inline bool IsDetached(const char *id)
{
    const Host *host = FindConst(id);
    return host && host->detached;
}

inline bool HasNativeInputFocus(const char *id)
{
    const Host *host = FindConst(id);
    if (!host || !host->detached || !host->window) {
        // Attached feature windows share Xemu's main native window; ImGui
        // focus is the authoritative discriminator in that mode.
        return true;
    }
    return (SDL_GetWindowFlags(host->window) & SDL_WINDOW_INPUT_FOCUS) != 0;
}

inline ImFont *FixedWidthFont(ImFont *attached_font)
{
    if (!g_rendering_id.empty()) {
        Host *host = Find(g_rendering_id.c_str());
        if (host && host->fixed_font) {
            return host->fixed_font;
        }
    }
    return attached_font;
}

inline bool ShouldDraw(const char *id)
{
    const Host *host = FindConst(id);
    if (!host || !host->detached) {
        return true;
    }
    return IsDetachedPass(id);
}

template<typename DrawFn>
inline void Register(const char *id, const char *title, bool *open,
                     DrawFn &&draw)
{
    if (!id || !*id) {
        return;
    }

    auto it = g_hosts.find(id);
    if (it == g_hosts.end()) {
        it = g_hosts.emplace(id, Host{}).first;
    }
    Host &host = it->second;
    if (host.id.empty()) {
        host.id = id;
    }
    const char *requested_title = title ? title : id;
    const bool title_changed = host.title != requested_title;
    if (title_changed) {
        host.title = requested_title;
    }
    host.open = open;
    // Registration is intentionally idempotent.  A detached callback invokes
    // the same Draw() function recursively in another ImGui context; replacing
    // the std::function while it is executing would invalidate its target.
    if (!host.draw) {
        host.draw = std::forward<DrawFn>(draw);
    }

    if (title_changed && host.window && host.title.size()) {
        SDL_SetWindowTitle(host.window, host.title.c_str());
    }
}

inline ImGuiWindowFlags WindowFlags(const char *id, ImGuiWindowFlags base)
{
    if (!IsDetachedPass(id)) {
        return base;
    }

    // The native SDL window supplies title/move/resize/close chrome.  Make the
    // ImGui window fill its client area so a detached tool does not acquire a
    // second fake title bar inside the real OS window.
    return base | ImGuiWindowFlags_NoTitleBar |
           ImGuiWindowFlags_NoMove |
           ImGuiWindowFlags_NoResize |
           ImGuiWindowFlags_NoCollapse |
           ImGuiWindowFlags_NoSavedSettings;
}

inline void PrepareWindow(const char *id)
{
    if (!IsDetachedPass(id)) {
        return;
    }

    ImGuiViewport *viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos, ImGuiCond_Always);
    ImGui::SetNextWindowSize(viewport->WorkSize, ImGuiCond_Always);
}

inline void RequestDetachFromWindow(const char *id, ImGuiWindow *window)
{
    Host *host = Find(id);
    if (!host || host->detached || host->pending_detach || !window) {
        return;
    }

    SDL_Window *main_window = xemu_get_window();
    int main_x = 0, main_y = 0;
    if (main_window) {
        SDL_GetWindowPosition(main_window, &main_x, &main_y);
    }

    host->pending_x = main_x + (int)std::lround(window->Pos.x);
    host->pending_y = main_y + (int)std::lround(window->Pos.y);
    host->pending_w = std::max(420, (int)std::lround(window->Size.x));
    host->pending_h = std::max(260, (int)std::lround(window->Size.y));

    const bool left_drag = ImGui::IsMouseDown(ImGuiMouseButton_Left);
    host->transfer_drag = left_drag;
    if (left_drag) {
        float global_x = 0.0f, global_y = 0.0f;
        SDL_GetGlobalMouseState(&global_x, &global_y);
        host->transfer_drag_offset_x =
            (int)std::lround(global_x) - host->pending_x;
        host->transfer_drag_offset_y =
            (int)std::lround(global_y) - host->pending_y;
    }
    host->pending_detach = true;
    g_pending_detach_work.store(true, std::memory_order_relaxed);
}

inline void RequestDetach(const char *id)
{
    RequestDetachFromWindow(id, ImGui::GetCurrentWindowRead());
}

inline bool MouseOutsideMainWindow()
{
    SDL_Window *main_window = xemu_get_window();
    if (!main_window) {
        return false;
    }

    int wx = 0, wy = 0, ww = 0, wh = 0;
    const bool have_pos = SDL_GetWindowPosition(main_window, &wx, &wy);
    const bool have_size = SDL_GetWindowSize(main_window, &ww, &wh);
    if (have_pos && have_size && ww > 0 && wh > 0) {
        float gx = 0.0f, gy = 0.0f;
        SDL_GetGlobalMouseState(&gx, &gy);
        constexpr float margin = 10.0f;
        if (gx < wx - margin || gy < wy - margin ||
            gx > wx + ww + margin || gy > wy + wh + margin) {
            return true;
        }
    }

    // Fallback for compositors where global mouse coordinates are restricted
    // (notably Wayland).  If ImGui itself reports a drag beyond its display
    // bounds, treat that as an undock gesture too.
    const ImGuiIO &io = ImGui::GetIO();
    constexpr float local_margin = 8.0f;
    return io.MousePos.x < -local_margin || io.MousePos.y < -local_margin ||
           io.MousePos.x > io.DisplaySize.x + local_margin ||
           io.MousePos.y > io.DisplaySize.y + local_margin;
}

inline void ObserveCurrentWindow(const char *id)
{
    if (IsDetachedPass(id)) {
        return;
    }

    Host *host = Find(id);
    ImGuiWindow *window = ImGui::GetCurrentWindowRead();
    if (!host || !window) {
        return;
    }

    ImGuiContext &g = *GImGui;
    ImGuiWindow *moving_root = g.MovingWindow ? g.MovingWindow->RootWindow : nullptr;
    if (moving_root == window->RootWindow &&
        ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        MouseOutsideMainWindow()) {
        RequestDetachFromWindow(id, window);
    }

    // Reliable fallback on window managers/compositors that do not expose
    // global drag coordinates: right-click the ImGui title bar and choose
    // "Detach to native window".
    const ImRect title_bar = window->TitleBarRect();
    ImGui::PushID(id);
    if (title_bar.Contains(ImGui::GetIO().MousePos) &&
        ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        ImGui::OpenPopup("##feature_detach_title_popup");
    }
    if (ImGui::BeginPopup("##feature_detach_title_popup")) {
        if (ImGui::MenuItem("Detach to native window")) {
            RequestDetachFromWindow(id, window);
        }
        ImGui::EndPopup();
    }
    ImGui::PopID();
}

inline void BuildHostFonts(Host &host, float logical_font_size,
                           float pixel_density)
{
    ImGuiIO &io = ImGui::GetIO();
    io.Fonts->Clear();

    const float size = logical_font_size > 1.0f ? logical_font_size : 16.0f;
    const float density = std::max(1.0f, pixel_density);

    ImFontConfig regular;
    regular.FontDataOwnedByAtlas = false;
    regular.RasterizerDensity = density;
    host.default_font = io.Fonts->AddFontFromMemoryTTF(
        (void *)Roboto_Medium_data, Roboto_Medium_size, size, &regular);

    ImFontConfig fixed;
    fixed.OversampleH = fixed.OversampleV = 1;
    fixed.PixelSnapH = true;
    fixed.RasterizerDensity = density;
    fixed.SizePixels = size * (13.0f / 16.0f);
    host.fixed_font = io.Fonts->AddFontDefault(&fixed);

    io.FontDefault = host.default_font;
    host.font_size = size;
    host.pixel_density = density;
}

inline bool CreateNativeHost(Host &host)
{
    if (host.window && host.gl && host.imgui) {
        const bool was_detached = host.detached;
        host.detached = true;
        host.pending_detach = false;
        host.detached_at_ms = SDL_GetTicks();
        host.moved_after_grace = false;
        SDL_SetWindowPosition(host.window, host.pending_x, host.pending_y);
        SDL_SetWindowSize(host.window, host.pending_w, host.pending_h);
        SDL_SetWindowTitle(host.window, host.title.c_str());
        SDL_ShowWindow(host.window);
        SDL_RaiseWindow(host.window);
        if (!was_detached) {
            g_detached_host_count.fetch_add(1, std::memory_order_relaxed);
        }
        return true;
    }

    ImGuiContext *main_imgui = ImGui::GetCurrentContext();
    SDL_Window *main_window = SDL_GL_GetCurrentWindow();
    SDL_GLContext main_gl = SDL_GL_GetCurrentContext();
    if (!main_imgui || !main_window || !main_gl) {
        host.pending_detach = false;
        host.transfer_drag = false;
        return false;
    }

    const ImGuiStyle main_style = ImGui::GetStyle();
    ImGuiIO &main_io = ImGui::GetIO();
    const ImGuiConfigFlags main_config_flags = main_io.ConfigFlags;
    const float main_font_size = main_io.FontDefault ? main_io.FontDefault->FontSize : 16.0f;

    int old_share = 0;
    SDL_GL_GetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, &old_share);
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, 1);

    const SDL_WindowFlags flags = (SDL_WindowFlags)(
        SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE |
        SDL_WINDOW_HIGH_PIXEL_DENSITY | SDL_WINDOW_HIDDEN);
    SDL_Window *window = SDL_CreateWindow(host.title.c_str(),
                                          host.pending_w, host.pending_h,
                                          flags);
    SDL_GLContext gl = nullptr;
    if (window) {
        SDL_SetWindowMinimumSize(window, 420, 260);
        SDL_SetWindowPosition(window, host.pending_x, host.pending_y);
        gl = SDL_GL_CreateContext(window);
    }
    SDL_GL_SetAttribute(SDL_GL_SHARE_WITH_CURRENT_CONTEXT, old_share);

    if (!window || !gl) {
        std::fprintf(stderr,
                     "[xemu-features] failed to detach '%s': %s\n",
                     host.title.c_str(), SDL_GetError());
        if (gl) SDL_GL_DestroyContext(gl);
        if (window) SDL_DestroyWindow(window);
        SDL_GL_MakeCurrent(main_window, main_gl);
        ImGui::SetCurrentContext(main_imgui);
        host.pending_detach = false;
        host.transfer_drag = false;
        return false;
    }

    SDL_GL_MakeCurrent(window, gl);
    // Detached tools are rendered once per Xemu frontend frame already.  A
    // second blocking swap interval would unnecessarily halve/stutter the UI.
    SDL_GL_SetSwapInterval(0);

    ImGuiContext *ctx = ImGui::CreateContext();
    ImGui::SetCurrentContext(ctx);
    ImGui::GetStyle() = main_style;
    ImGuiIO &io = ImGui::GetIO();
    io.ConfigFlags = main_config_flags;
    io.IniFilename = nullptr;

    host.window = window;
    host.gl = gl;
    host.imgui = ctx;
    BuildHostFonts(host, main_font_size, SDL_GetWindowPixelDensity(window));

    bool platform_ok = ImGui_ImplSDL3_InitForOpenGL(window, gl);
    bool renderer_ok = platform_ok && ImGui_ImplOpenGL3_Init("#version 150");
    if (!renderer_ok) {
        std::fprintf(stderr,
                     "[xemu-features] failed to initialize detached ImGui host '%s'\n",
                     host.title.c_str());
        if (platform_ok) {
            ImGui_ImplSDL3_Shutdown();
        }
        ImGui::DestroyContext(ctx);
        SDL_GL_MakeCurrent(main_window, main_gl);
        SDL_GL_DestroyContext(gl);
        SDL_DestroyWindow(window);
        host.window = nullptr;
        host.gl = nullptr;
        host.imgui = nullptr;
        host.default_font = nullptr;
        host.fixed_font = nullptr;
        host.pending_detach = false;
        host.transfer_drag = false;
        ImGui::SetCurrentContext(main_imgui);
        return false;
    }

    host.window_id = SDL_GetWindowID(window);
    host.detached = true;
    host.pending_detach = false;
    host.detached_at_ms = SDL_GetTicks();
    host.last_move_ms = host.detached_at_ms;
    host.moved_after_grace = false;
    g_detached_host_count.fetch_add(1, std::memory_order_relaxed);

    EnsureEventWatch();
    SDL_ShowWindow(window);
    SDL_RaiseWindow(window);

    SDL_GL_MakeCurrent(main_window, main_gl);
    ImGui::SetCurrentContext(main_imgui);
    return true;
}

inline void Reattach(Host &host)
{
    const bool was_detached = host.detached;
    host.detached = false;
    host.pending_detach = false;
    host.transfer_drag = false;
    host.moved_after_grace = false;
    if (host.window) {
        SDL_HideWindow(host.window);
    }
    if (was_detached) {
        g_detached_host_count.fetch_sub(1, std::memory_order_relaxed);
    }
}

inline bool DetachedWindowCenterIsInsideMain(const Host &host)
{
    if (!host.window) {
        return false;
    }
    SDL_Window *main_window = xemu_get_window();
    if (!main_window) {
        return false;
    }

    int mx = 0, my = 0, mw = 0, mh = 0;
    int hx = 0, hy = 0, hw = 0, hh = 0;
    if (!SDL_GetWindowPosition(main_window, &mx, &my) ||
        !SDL_GetWindowSize(main_window, &mw, &mh) ||
        !SDL_GetWindowPosition(host.window, &hx, &hy) ||
        !SDL_GetWindowSize(host.window, &hw, &hh)) {
        return false;
    }

    const int cx = hx + hw / 2;
    const int cy = hy + hh / 2;
    return cx >= mx && cx < mx + mw && cy >= my && cy < my + mh;
}

inline const std::vector<SDL_Event> &ProcessQueuedEvents()
{
    std::lock_guard<std::mutex> lock(g_event_mutex);
    g_dispatch_events.clear();
    g_dispatch_events.swap(g_events);
    return g_dispatch_events;
}

inline void RenderHost(Host &host, const std::vector<SDL_Event> &events,
                       ImGuiContext *main_imgui, SDL_Window *main_window,
                       SDL_GLContext main_gl, const ImGuiStyle &main_style,
                       ImFont *main_default_font)
{
    if (!host.detached || !host.window || !host.gl || !host.imgui || !host.draw) {
        return;
    }

    if (host.open && !*host.open) {
        Reattach(host);
        return;
    }

    const uint32_t now = SDL_GetTicks();
    for (const SDL_Event &event : events) {
        if (EventWindowID(event) != host.window_id) {
            continue;
        }

        if (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            if (host.open) {
                *host.open = false;
            }
            Reattach(host);
            return;
        }
        if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat &&
            event.key.scancode == SDL_SCANCODE_D &&
            (event.key.mod & SDL_KMOD_CTRL) &&
            (event.key.mod & SDL_KMOD_SHIFT)) {
            Reattach(host);
            return;
        }
        if (event.type == SDL_EVENT_WINDOW_MOVED &&
            now - host.detached_at_ms > 600) {
            host.moved_after_grace = true;
            host.last_move_ms = now;
        }

        ImGui::SetCurrentContext(host.imgui);
        ImGui_ImplSDL3_ProcessEvent(&event);
    }

    // Drag the native tool window back so its center is over Xemu and release:
    // after the OS finishes the move sequence it automatically reattaches.
    if (!host.transfer_drag && host.moved_after_grace &&
        now - host.last_move_ms > 180 &&
        DetachedWindowCenterIsInsideMain(host)) {
        Reattach(host);
        ImGui::SetCurrentContext(main_imgui);
        SDL_GL_MakeCurrent(main_window, main_gl);
        return;
    }

    ImGui::SetCurrentContext(host.imgui);
    ImGui::GetStyle() = main_style;
    SDL_GL_MakeCurrent(host.window, host.gl);

    const float desired_font_size = main_default_font ? main_default_font->FontSize : 16.0f;
    const float desired_density = std::max(1.0f, SDL_GetWindowPixelDensity(host.window));
    if (std::fabs(host.font_size - desired_font_size) > 0.01f ||
        std::fabs(host.pixel_density - desired_density) > 0.01f) {
        ImGui_ImplOpenGL3_DestroyFontsTexture();
        BuildHostFonts(host, desired_font_size, desired_density);
    }
    ImGui::GetIO().FontDefault = host.default_font;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();

    g_rendering_id = host.id;
    host.draw();
    g_rendering_id.clear();

    ImGui::Render();

    int pixel_w = 0, pixel_h = 0;
    SDL_GetWindowSizeInPixels(host.window, &pixel_w, &pixel_h);
    if (pixel_w > 0 && pixel_h > 0) {
        const ImVec4 bg = ImGui::GetStyle().Colors[ImGuiCol_WindowBg];
        glViewport(0, 0, pixel_w, pixel_h);
        glClearColor(bg.x, bg.y, bg.z, bg.w);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        SDL_GL_SwapWindow(host.window);
    }

    ImGui::SetCurrentContext(main_imgui);
    SDL_GL_MakeCurrent(main_window, main_gl);
}

inline void Pump()
{
    // A detached callback can invoke a feature's top-level Show... function.
    // Never recurse into the detached-host pump from that secondary context.
    if (!g_rendering_id.empty()) {
        return;
    }

    // Normal gameplay with every tool attached/closed should make detachable
    // support almost disappear. Pending detach is a rare UI action, so only
    // scan the tiny registered-host map when there is no native host alive.
    if (g_detached_host_count.load(std::memory_order_relaxed) == 0 &&
        !g_pending_detach_work.load(std::memory_order_relaxed)) {
        return;
    }

    ImGuiContext *main_imgui = ImGui::GetCurrentContext();
    if (!main_imgui) {
        return;
    }
    const int frame = ImGui::GetFrameCount();
    if (frame == g_last_pump_frame) {
        return;
    }
    g_last_pump_frame = frame;

    SDL_Window *main_window = SDL_GL_GetCurrentWindow();
    SDL_GLContext main_gl = SDL_GL_GetCurrentContext();
    if (!main_window || !main_gl) {
        return;
    }

    for (auto &entry : g_hosts) {
        Host &host = entry.second;
        if (host.pending_detach && host.open && *host.open) {
            if (host.transfer_drag) {
                // Tear off immediately while the original title-bar button is
                // still held. The native host will follow the same global
                // mouse gesture until release below. Explicitly end ImGui's
                // attached-window move so the main context cannot stay stuck
                // after the OS window appears.
                ImGuiContext &g = *GImGui;
                g.MovingWindow = nullptr;
                ImGui::ClearActiveID();
                ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                CreateNativeHost(host);
            } else {
                // Context-menu/manual detach: avoid transferring an unrelated
                // left-button gesture into the new native window.
                const SDL_MouseButtonFlags global_buttons =
                    SDL_GetGlobalMouseState(nullptr, nullptr);
                if (global_buttons & SDL_BUTTON_LMASK) {
                    continue;
                }
                if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                    ImGui::GetIO().AddMouseButtonEvent(ImGuiMouseButton_Left, false);
                    continue;
                }
                CreateNativeHost(host);
            }
        }
    }

    /* A request can disappear because its tool closed before host creation, or
     * can remain pending while a manual detach waits for the mouse button to be
     * released. Recompute one global hint here; idle Pump() calls then become
     * two relaxed loads and a return instead of a map walk. */
    bool still_pending = false;
    for (auto &entry : g_hosts) {
        Host &host = entry.second;
        if (host.pending_detach && (!host.open || !*host.open)) {
            host.pending_detach = false;
            host.transfer_drag = false;
        }
        still_pending |= host.pending_detach;
    }
    g_pending_detach_work.store(still_pending, std::memory_order_relaxed);

    // Continue an attached title-bar drag seamlessly in native-window space.
    // The mouse-down originated in Xemu, so SDL cannot ask the window manager
    // to continue that same move on a freshly-created window. Following the
    // global cursor here reproduces the expected tear-off interaction without
    // requiring the user to release and grab the title bar again.
    for (auto &entry : g_hosts) {
        Host &host = entry.second;
        if (!host.detached || !host.window || !host.transfer_drag) {
            continue;
        }

        float global_x = 0.0f, global_y = 0.0f;
        const SDL_MouseButtonFlags buttons =
            SDL_GetGlobalMouseState(&global_x, &global_y);
        if (buttons & SDL_BUTTON_LMASK) {
            SDL_SetWindowPosition(
                host.window,
                (int)std::lround(global_x) - host.transfer_drag_offset_x,
                (int)std::lround(global_y) - host.transfer_drag_offset_y);
        } else {
            host.transfer_drag = false;
            host.detached_at_ms = SDL_GetTicks();
            host.last_move_ms = host.detached_at_ms;
            host.moved_after_grace = false;
            SDL_RaiseWindow(host.window);
        }
    }

    const std::vector<SDL_Event> &events = ProcessQueuedEvents();

    const ImGuiStyle main_style = ImGui::GetStyle();
    ImFont *main_default_font = ImGui::GetIO().FontDefault;
    for (auto &entry : g_hosts) {
        RenderHost(entry.second, events, main_imgui, main_window, main_gl,
                   main_style, main_default_font);
    }

    /* Preserve capacity for the next producer/consumer swap. */
    g_dispatch_events.clear();

    ImGui::SetCurrentContext(main_imgui);
    SDL_GL_MakeCurrent(main_window, main_gl);
}

} // namespace xemu_feature_detach
