// SPDX-License-Identifier: GPL-2.0-or-later
// xemu custom fork - NV2A Geometry Dumper UI
#include "frontend.hh"
#include "ui/xui/common.hh"
#include "ui/xui/misc.hh"
#include "ui/xemu-notifications.h"
#include "xemu-features/geometry-dumper/geometry-dumper.h"
#include "xemu-features/shared/detachable-windows.hh"

#include <cstring>
#include <cmath>

static constexpr const char *kGeometryDetachId = "geometry-dumper.window";
static bool g_geometry_window_open;
static char g_geometry_output_root[1024];
static int g_geometry_frame_count = 1;
static bool g_geometry_disable_backface_culling;
static bool g_geometry_export_placed_geometry = true;
static float g_geometry_export_scale = 1.0f;
static bool g_geometry_dump_textures = true;

static const char *GeometryCaptureStateText(XemuGeometryCaptureMode mode)
{
    switch (mode) {
    case XEMU_GEOMETRY_CAPTURE_NEXT_DRAW: return "Waiting for next draw";
    case XEMU_GEOMETRY_CAPTURE_NEXT_FRAME_WAIT: return "Waiting for frame boundary";
    case XEMU_GEOMETRY_CAPTURE_FRAME_ACTIVE: return "Capturing frames";
    default: return "Idle";
    }
}

static XemuGeometryCaptureOptions GeometryCurrentOptions()
{
    XemuGeometryCaptureOptions options{};
    options.frame_count = (uint32_t)(g_geometry_frame_count < 1
                                         ? 1 : g_geometry_frame_count);
    options.disable_backface_culling = g_geometry_disable_backface_culling;
    options.export_placed_geometry = g_geometry_export_placed_geometry;
    options.export_scale = g_geometry_export_scale;
    options.dump_textures = g_geometry_dump_textures;
    return options;
}

static void DrawGeometryInfo()
{
    ImGui::TextWrapped(
        "glTF 2.0 is the primary export. geometry.gltf uses fixed-function "
        "draw-time placement when enabled (otherwise raw/local positions), "
        "reconstructed post-vertex-shader UVs, PNG texture references and "
        "per-draw metadata. With placement enabled, geometry_raw.gltf "
        "preserves every raw/local draw. OBJ/MTL files remain "
        "compatibility/debug exports.");

    ImGui::Spacing();
    ImGui::TextWrapped(
        "Every capture contains geometry.gltf + geometry.bin as the primary "
        "glTF 2.0 asset, plus draws.jsonl and vertices.csv. With placed export "
        "enabled, geometry.gltf contains fixed-function view-space placement "
        "and geometry_raw.gltf + geometry_raw.bin preserve all raw/local "
        "draws. glTF materials use the reconstructed post-VSH PROJECT2D "
        "coordinates and dumped PNGs only where that mapping is safe; "
        "NV2A-specific draw/primitive details are kept in glTF extras and "
        "JSON. Legacy OBJ/MTL exports are retained for compatibility. Export "
        "scale changes glTF/OBJ positions only; CSV/JSON remain "
        "native/unscaled.");

    ImGui::Spacing();
    ImGui::TextWrapped(
        "Multi-frame capture currently writes on the renderer thread and can "
        "hitch for geometry-heavy scenes. Moving file serialization to an "
        "asynchronous writer remains a possible optimization without changing "
        "capture semantics.");
}

static void DrawGeometryCaptureTab(const XemuGeometryDumperStatus &status)
{
    ImGui::Text("NV2A hook: %s",
                status.renderer_hooked ? "Ready" : "Waiting for renderer");
    ImGui::Text("Capture: %s", GeometryCaptureStateText(status.mode));
    if (status.mode == XEMU_GEOMETRY_CAPTURE_FRAME_ACTIVE ||
        status.mode == XEMU_GEOMETRY_CAPTURE_NEXT_FRAME_WAIT) {
        ImGui::Text("Frames: %u / %u", status.frames_completed,
                    status.frames_requested);
    }
    ImGui::Separator();

    ImGui::TextUnformatted("Dump directory");
    ImGui::SetNextItemWidth(-92.0f);
    ImGui::InputTextWithHint("##geometry_output_root",
                             "blank = xemu data/geometry",
                             g_geometry_output_root,
                             sizeof(g_geometry_output_root));
    ImGui::SameLine();
    if (ImGui::Button("Browse...", ImVec2(84.0f, 0))) {
        const char *start = g_geometry_output_root[0]
                                ? g_geometry_output_root : nullptr;
        ShowOpenFolderDialog(start, [](const char *path) {
            if (!path || !path[0]) {
                return;
            }
            g_strlcpy(g_geometry_output_root, path,
                      sizeof(g_geometry_output_root));
        });
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Choose the root directory for geometry captures.");
    }
    if (g_geometry_output_root[0] && ImGui::Button("Use Default Directory")) {
        g_geometry_output_root[0] = '\0';
    }

    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::InputInt("Frames to capture", &g_geometry_frame_count, 1, 10)) {
        if (g_geometry_frame_count < 1) {
            g_geometry_frame_count = 1;
        } else if (g_geometry_frame_count > 10000) {
            g_geometry_frame_count = 10000;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Captures this many complete consecutive frames after the next "
            "frame boundary. Range: 1-10000.");
    }

    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::InputFloat("Export scale", &g_geometry_export_scale,
                          0.1f, 1.0f, "%.9g")) {
        if (!std::isfinite(g_geometry_export_scale) ||
            g_geometry_export_scale <= 0.0f) {
            g_geometry_export_scale = 1.0f;
        } else if (g_geometry_export_scale > 1000.0f) {
            g_geometry_export_scale = 1000.0f;
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Uniformly scales glTF/OBJ vertex positions only. 1.0 is the "
            "current/native dump size. Any finite positive float is accepted "
            "up to 1000.0. CSV/JSON retain native unscaled coordinates.");
    }

    ImGui::Checkbox("Dump textures + glTF/OBJ materials",
                    &g_geometry_dump_textures);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Exports active base-level NV2A textures into textures/. glTF and "
            "legacy OBJ materials are restricted to directly representable "
            "PROJECT2D sampling and use reconstructed post-vertex-shader "
            "coordinates. Complex dependent/cube/bump stages remain metadata.");
    }

    ImGui::Checkbox("Export original draw placement (fixed-function)",
                    &g_geometry_export_placed_geometry);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Makes geometry.gltf the placed fixed-function scene. A "
            "geometry_raw.gltf companion preserves every raw/local draw, "
            "including programmable Xbox vertex-shader draws that do not "
            "expose a universal world transform.");
    }

    ImGui::Checkbox("Disable backface culling while capturing",
                    &g_geometry_disable_backface_culling);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Temporarily disables the NV2A rasterizer cull bit only for draws "
            "being captured, then restores the exact guest state.");
    }

    ImGui::Spacing();
    const bool busy = status.mode != XEMU_GEOMETRY_CAPTURE_IDLE;
    if (busy) {
        ImGui::BeginDisabled();
    }

    if (ImGui::Button("Capture Next Draw")) {
        XemuGeometryCaptureOptions options = GeometryCurrentOptions();
        options.frame_count = 1;
        if (xemu_geometry_dumper_capture_next_draw_ex(g_geometry_output_root,
                                                       &options)) {
            xemu_queue_notification(
                "Geometry Dumper: waiting for next NV2A draw");
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Capture Frames")) {
        XemuGeometryCaptureOptions options = GeometryCurrentOptions();
        if (xemu_geometry_dumper_capture_frames(g_geometry_output_root,
                                                &options)) {
            xemu_queue_notification(
                "Geometry Dumper: multi-frame capture armed");
        }
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Capture begins after the next completed frame boundary, then "
            "records exactly the requested number of complete frames.");
    }

    if (busy) {
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Cancel Capture")) {
            xemu_geometry_dumper_cancel_capture();
            xemu_queue_notification("Geometry Dumper: capture cancelled");
        }
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Last / active capture");
    ImGui::Text("Frame time: %d", status.frame_time);
    ImGui::Text("Draws: %llu", (unsigned long long)status.draws_captured);
    ImGui::Text("Vertices: %llu", (unsigned long long)status.vertices_captured);
    ImGui::Text("Primitives: %llu",
                (unsigned long long)status.primitives_captured);
    if (status.export_placed_geometry) {
        ImGui::Text("Placed fixed-function draws: %llu",
                    (unsigned long long)status.placed_draws_captured);
        ImGui::Text("Programmable/unsupported placed draws: %llu",
                    (unsigned long long)status.placed_draws_unsupported);
    }
    ImGui::Text("Export scale: %.9g", status.export_scale);
    if (status.dump_textures) {
        ImGui::Text("Texture stage references: %llu",
                    (unsigned long long)status.textures_referenced);
        ImGui::Text("Unique texture images dumped: %llu",
                    (unsigned long long)status.textures_dumped);
        if (status.texture_dump_failures) {
            ImGui::Text("Texture dump failures/unsupported: %llu",
                        (unsigned long long)status.texture_dump_failures);
        }
    }

    if (status.output_path[0]) {
        ImGui::TextWrapped("Output: %s", status.output_path);
        if (ImGui::Button("Copy Output Path")) {
            ImGui::SetClipboardText(status.output_path);
        }
    }

    if (status.last_error[0]) {
        ImGui::Spacing();
        ImGui::TextWrapped("Error: %s", status.last_error);
    }
}

void FeatureGeometryDumperDrawMenuItem()
{
    ImGui::MenuItem("Geometry Dumper", nullptr, &g_geometry_window_open);
}

void FeatureGeometryDumperDrawWindow()
{
    xemu_feature_detach::Register(kGeometryDetachId, "Geometry Dumper",
                                  &g_geometry_window_open,
                                  []() { FeatureGeometryDumperDrawWindow(); });
    xemu_feature_detach::Pump();

    if (!g_geometry_window_open ||
        !xemu_feature_detach::ShouldDraw(kGeometryDetachId)) {
        return;
    }

    if (xemu_feature_detach::IsDetachedPass(kGeometryDetachId)) {
        xemu_feature_detach::PrepareWindow(kGeometryDetachId);
    } else {
        ImGui::SetNextWindowSize(ImVec2(620.0f, 620.0f),
                                 ImGuiCond_FirstUseEver);
    }
    const ImGuiWindowFlags flags =
        xemu_feature_detach::WindowFlags(kGeometryDetachId, 0);
    if (!ImGui::Begin("Geometry Dumper", &g_geometry_window_open, flags)) {
        ImGui::End();
        return;
    }
    xemu_feature_detach::ObserveCurrentWindow(kGeometryDetachId);

    XemuGeometryDumperStatus status{};
    xemu_geometry_dumper_get_status(&status);

    if (ImGui::BeginTabBar("##geometry_tabs")) {
        if (ImGui::BeginTabItem("Capture")) {
            DrawGeometryCaptureTab(status);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Info")) {
            DrawGeometryInfo();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

bool FeatureGeometryDumperWindowOpen()
{
    return g_geometry_window_open;
}
