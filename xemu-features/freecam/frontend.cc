// SPDX-License-Identifier: GPL-2.0-or-later
// xemu custom fork - renderer free camera UI/input frontend
#include "frontend.hh"
#include "ui/xui/common.hh"
#include "ui/xui/xemu-hud.h"
#include "ui/xemu-notifications.h"
#include "xemu-features/freecam/freecam.h"
#include "xemu-features/shared/detachable-windows.hh"

#include <algorithm>
#include <cmath>

static constexpr const char *kFreecamDetachId = "freecam.window";
static bool g_freecam_window_open;
static bool g_freecam_advanced_info;
static bool g_freecam_relative_owned;
static bool g_freecam_relative_restore;

static void FreecamApplyRelativeMouse(bool want_capture)
{
    SDL_Window *window = xemu_get_window();
    if (!window) {
        return;
    }

    if (want_capture) {
        if (!g_freecam_relative_owned) {
            g_freecam_relative_restore = SDL_GetWindowRelativeMouseMode(window);
            g_freecam_relative_owned = true;
        }
        if (!SDL_GetWindowRelativeMouseMode(window)) {
            SDL_SetWindowRelativeMouseMode(window, true);
        }
    } else if (g_freecam_relative_owned) {
        SDL_SetWindowRelativeMouseMode(window, g_freecam_relative_restore);
        g_freecam_relative_owned = false;
    }
}

static void FreecamSetEnabledFromFrontend(bool enabled)
{
    xemu_freecam_set_enabled(enabled);
    if (!enabled) {
        FreecamApplyRelativeMouse(false);
    }
    xemu_queue_notification(enabled ? "Free Camera: ON" : "Free Camera: OFF");
}

static void FeatureFreecamTick()
{
    ImGuiIO &io = ImGui::GetIO();
    bool enabled = xemu_freecam_is_enabled();

    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_F10, false)) {
        enabled = !enabled;
        FreecamSetEnabledFromFrontend(enabled);
    }

    /* The camera is normally disabled. Avoid collecting ~20 diagnostic
     * atomics plus taking the settings/pose mutex on every frontend frame just
     * to rediscover that fact. Relative mouse restoration is only needed if
     * this frontend previously acquired it. */
    if (!enabled) {
        if (g_freecam_relative_owned) {
            FreecamApplyRelativeMouse(false);
        }
        return;
    }

    XemuFreecamStatus status{};
    xemu_freecam_get_status(&status);
    if (!io.WantTextInput && ImGui::IsKeyPressed(ImGuiKey_Home, false)) {
        xemu_freecam_reset_pose();
        xemu_queue_notification("Free Camera: pose reset");
    }

    const bool capture = status.enabled && status.settings.capture_mouse &&
                         !io.WantTextInput;
    FreecamApplyRelativeMouse(capture);

    if (!status.enabled || io.WantTextInput) {
        return;
    }

    float dt = std::clamp(io.DeltaTime, 0.0f, 0.1f);
    float speed = status.settings.move_speed;
    if (ImGui::IsKeyDown(ImGuiKey_LeftShift) ||
        ImGui::IsKeyDown(ImGuiKey_RightShift)) {
        speed *= status.settings.boost_multiplier;
    }
    if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) ||
        ImGui::IsKeyDown(ImGuiKey_RightCtrl)) {
        speed *= status.settings.precision_multiplier;
    }

    float right = 0.0f, up = 0.0f, forward = 0.0f;
    if (ImGui::IsKeyDown(ImGuiKey_D)) right += 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_A)) right -= 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_E)) up += 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_Q)) up -= 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_W)) forward += 1.0f;
    if (ImGui::IsKeyDown(ImGuiKey_S)) forward -= 1.0f;

    float move_len = std::sqrt(right * right + up * up + forward * forward);
    if (move_len > 0.0f) {
        float scale = speed * dt / move_len;
        xemu_freecam_move_local(right * scale, up * scale, forward * scale);
    }

    float yaw = 0.0f, pitch = 0.0f, roll = 0.0f;
    if (capture) {
        yaw += io.MouseDelta.x * status.settings.mouse_sensitivity_deg;
        float y = io.MouseDelta.y * status.settings.mouse_sensitivity_deg;
        pitch += status.settings.invert_mouse_y ? y : -y;
    }
    const float keyboard_turn = 90.0f * dt;
    if (ImGui::IsKeyDown(ImGuiKey_LeftArrow)) yaw -= keyboard_turn;
    if (ImGui::IsKeyDown(ImGuiKey_RightArrow)) yaw += keyboard_turn;
    if (ImGui::IsKeyDown(ImGuiKey_UpArrow)) pitch += keyboard_turn;
    if (ImGui::IsKeyDown(ImGuiKey_DownArrow)) pitch -= keyboard_turn;
    if (ImGui::IsKeyDown(ImGuiKey_Z)) roll -= keyboard_turn;
    if (ImGui::IsKeyDown(ImGuiKey_C)) roll += keyboard_turn;
    if (yaw != 0.0f || pitch != 0.0f || roll != 0.0f) {
        xemu_freecam_rotate(yaw, pitch, roll);
    }

    if (capture && io.MouseWheel != 0.0f) {
        XemuFreecamSettings settings = status.settings;
        settings.move_speed *= std::pow(1.25f, io.MouseWheel);
        settings.move_speed = std::clamp(settings.move_speed, 0.0001f, 100000.0f);
        xemu_freecam_set_settings(&settings);
    }
}

static void DrawFreecamAdvancedInfo(const XemuFreecamStatus &status)
{
    ImGui::SeparatorText("Renderer diagnostics");
    ImGui::Text("NV2A renderer hook: %s", status.renderer_seen ? "Ready" : "Waiting");
    ImGui::Text("Fixed-function draws seen: %llu",
                (unsigned long long)status.fixed_function_draws);
    ImGui::Text("  Exact compatibility transforms: %llu",
                (unsigned long long)status.fixed_exact_transformed_draws);
    ImGui::Text("  Reconstructed-view transforms: %llu",
                (unsigned long long)status.fixed_reconstructed_transformed_draws);
    ImGui::Text("    CMAT-inferred true-view: %llu",
                (unsigned long long)status.fixed_composite_inferred_transformed_draws);
    if (status.fixed_composite_inference_attempts) {
        ImGui::Text("      CMAT perspective attempts: %llu  rejected: %llu",
                    (unsigned long long)status.fixed_composite_inference_attempts,
                    (unsigned long long)status.fixed_composite_inference_rejected);
    }
    ImGui::Text("    MMAT-assisted true-view: %llu",
                (unsigned long long)status.fixed_mmat_reconstructed_transformed_draws);
    ImGui::Text("    Validated PMAT view: %llu",
                (unsigned long long)status.fixed_pmat_reconstructed_transformed_draws);
    if (status.fixed_pmat_inference_attempts) {
        ImGui::Text("      PMAT split attempts: %llu  rejected: %llu",
                    (unsigned long long)status.fixed_pmat_inference_attempts,
                    (unsigned long long)status.fixed_pmat_inference_rejected);
    }
    ImGui::Text("    General affine CMAT view: %llu",
                (unsigned long long)status.fixed_affine_inferred_transformed_draws);
    if (status.fixed_affine_inference_attempts) {
        ImGui::Text("      Affine perspective attempts: %llu  rejected: %llu",
                    (unsigned long long)status.fixed_affine_inference_attempts,
                    (unsigned long long)status.fixed_affine_inference_rejected);
    }
    ImGui::Text("  Projective transforms/fallbacks: %llu",
                (unsigned long long)status.fixed_projective_transformed_draws);
    if (status.fixed_reconstructed_fallback_draws) {
        ImGui::Text("  Reconstructed -> projective fallback: %llu",
                    (unsigned long long)status.fixed_reconstructed_fallback_draws);
    }
    if (status.fixed_nonperspective_passthrough_draws) {
        ImGui::Text("  Flat no-depth screen draws left unchanged: %llu",
                    (unsigned long long)status.fixed_nonperspective_passthrough_draws);
    }
    if (status.fixed_nonperspective_depth_eligible_draws) {
        ImGui::Text("  Flat depth-active draws allowed through: %llu",
                    (unsigned long long)
                        status.fixed_nonperspective_depth_eligible_draws);
        ImGui::Text("    ...successfully transformed: %llu",
                    (unsigned long long)
                        status.fixed_nonperspective_depth_transformed_draws);
    }
    if (status.fixed_validated_world_flat_guard_bypasses) {
        ImGui::Text("  Validated 3D draws bypassing flat-screen guard: %llu",
                    (unsigned long long)
                        status.fixed_validated_world_flat_guard_bypasses);
    }
    ImGui::Text("Programmable VSH draws seen: %llu",
                (unsigned long long)status.programmable_draws);
    ImGui::Text("  Post-VSH tail eligible: %llu",
                (unsigned long long)status.programmable_tail_eligible_draws);
    ImGui::Text("  Programmable draws transformed: %llu",
                (unsigned long long)status.programmable_transformed_draws);
    if (status.programmable_classification_deferred_draws) {
        ImGui::Text("  Screen-space classification deferred: %llu",
                    (unsigned long long)
                        status.programmable_classification_deferred_draws);
    }
    if (status.programmable_screen_space_detected_draws) {
        ImGui::Text("  Fullscreen triangles detected: %llu",
                    (unsigned long long)
                        status.programmable_screen_space_detected_draws);
    }
    if (status.programmable_screen_space_passthrough_draws) {
        ImGui::Text("  Screen-space draws left unchanged: %llu",
                    (unsigned long long)
                        status.programmable_screen_space_passthrough_draws);
    }
    if (status.programmable_relative_constant_draws) {
        ImGui::Text("  Skipped: relative c[A0+n] reads: %llu",
                    (unsigned long long)status.programmable_relative_constant_draws);
    }
    if (status.programmable_no_room_draws) {
        ImGui::Text("  Skipped: no VSH tail room: %llu",
                    (unsigned long long)status.programmable_no_room_draws);
    }
    if (status.programmable_no_constants_draws) {
        ImGui::Text("  Skipped: no scratch constants: %llu",
                    (unsigned long long)status.programmable_no_constants_draws);
    }
    ImGui::Text("All draws transformed: %llu",
                (unsigned long long)status.transformed_draws);
    if (status.transform_failures) {
        ImGui::Text("Hard transform failures: %llu",
                    (unsigned long long)status.transform_failures);
    }
}

static void DrawFreecamControls(XemuFreecamStatus &status)
{
    XemuFreecamSettings settings = status.settings;

    bool enabled = settings.enabled;
    if (ImGui::Checkbox("Enable free camera", &enabled)) {
        FreecamSetEnabledFromFrontend(enabled);
        settings.enabled = enabled;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("F10");

    ImGui::Checkbox("Show advanced renderer info", &g_freecam_advanced_info);
    if (g_freecam_advanced_info) {
        DrawFreecamAdvancedInfo(status);
    }

    bool settings_changed = false;

    ImGui::SeparatorText("Camera mode");
    int render_mode = (int)settings.render_mode;
    static const char *render_modes[] = {
        "Projective compatibility",
        "Reconstructed View (portal-aware)",
    };
    ImGui::SetNextItemWidth(280.0f);
    if (ImGui::Combo("Render mode", &render_mode, render_modes,
                     IM_ARRAYSIZE(render_modes))) {
        settings.render_mode = (uint32_t)render_mode;
        settings_changed = true;
    }
    settings_changed |= ImGui::Checkbox(
        "Protect fullscreen / screen-space passes",
        &settings.protect_screen_space);
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip(
            "Leave recognized fullscreen programmable triangles and "
            "high-confidence flat/no-depth fixed-function presentation passes "
            "anchored to the guest screen. Depth-active fixed-function geometry "
            "is allowed through even when its CMAT has no perspective-W.");
    }

    ImGui::SeparatorText("Movement");
    settings_changed |= ImGui::Checkbox("Capture mouse while enabled",
                                        &settings.capture_mouse);
    settings_changed |= ImGui::Checkbox("Invert mouse Y",
                                        &settings.invert_mouse_y);

    ImGui::SetNextItemWidth(180.0f);
    settings_changed |= ImGui::DragFloat("Movement speed", &settings.move_speed,
                                         0.05f, 0.0001f, 100000.0f, "%.4g");
    ImGui::SetNextItemWidth(180.0f);
    settings_changed |= ImGui::DragFloat("Boost multiplier", &settings.boost_multiplier,
                                         0.1f, 1.0f, 100.0f, "%.2fx");
    ImGui::SetNextItemWidth(180.0f);
    settings_changed |= ImGui::DragFloat("Precision multiplier",
                                         &settings.precision_multiplier,
                                         0.01f, 0.001f, 1.0f, "%.3fx");
    ImGui::SetNextItemWidth(180.0f);
    settings_changed |= ImGui::DragFloat("Mouse sensitivity",
                                         &settings.mouse_sensitivity_deg,
                                         0.005f, 0.001f, 10.0f, "%.3f deg/px");

    ImGui::TextWrapped(
        "WASD move  |  Q/E down/up  |  Mouse look  |  Z/C roll  |  "
        "Shift boost  |  Ctrl precision  |  Mouse wheel changes speed  |  "
        "Arrow keys provide keyboard look  |  Home resets pose");

    ImGui::SeparatorText("Projection");
    settings_changed |= ImGui::Checkbox("Override vertical FOV",
                                        &settings.fov_override);
    if (!settings.fov_override) {
        ImGui::BeginDisabled();
    }
    ImGui::SetNextItemWidth(180.0f);
    settings_changed |= ImGui::SliderFloat("Vertical FOV", &settings.fov_degrees,
                                           10.0f, 170.0f, "%.1f deg");
    if (!settings.fov_override) {
        ImGui::EndDisabled();
    }

    if (settings_changed) {
        xemu_freecam_set_settings(&settings);
    }

    ImGui::SeparatorText("Pose offset from game camera");
    xemu_freecam_get_status(&status);
    ImGui::Text("Position:  X %.4f   Y %.4f   Z %.4f",
                status.pose.position[0], status.pose.position[1],
                status.pose.position[2]);
    ImGui::Text("Rotation:  Yaw %.2f   Pitch %.2f   Roll %.2f",
                status.pose.yaw_degrees, status.pose.pitch_degrees,
                status.pose.roll_degrees);
    if (ImGui::Button("Reset Pose (Home)")) {
        xemu_freecam_reset_pose();
    }
}

static void DrawFreecamInfo(const XemuFreecamStatus &status)
{
    if (status.settings.render_mode == XEMU_FREECAM_RENDER_RECONSTRUCTED_VIEW) {
        ImGui::TextWrapped(
            "Reconstructed View keeps camera motion before the perspective "
            "divide. It validates several fixed-function routes: camera-like "
            "CMAT recovery, MMAT-assisted recovery, a guest-PMAT split, then "
            "the general affine CMAT reconstruction. Only perspective draws "
            "that fail all validated routes fall back to Projective "
            "compatibility; obvious 2D/HUD draws stay in screen space.");
    } else {
        ImGui::TextWrapped(
            "Projective compatibility preserves the Milestone 3.1 behavior: "
            "it transforms the already-composed/projected draw. This is the "
            "broadest compatibility mode and remains available unchanged.");
    }

    if (status.programmable_draws) {
        ImGui::Spacing();
        ImGui::TextWrapped(
            "Programmable VSH draws keep the Milestone 3.1 post-VSH "
            "compatibility transform in both camera modes, but screen-space "
            "protection now recognizes the oversized right-triangle pattern "
            "commonly used for fullscreen post-processing/compositing. Those "
            "passes stay anchored to the guest screen while ordinary 3D VSH "
            "draws continue through the free-camera transform.");
    }

    ImGui::Spacing();
    ImGui::TextWrapped(
        "The free camera remains an offset relative to the game's current "
        "camera. Projective compatibility keeps the original M3.1 renderer "
        "reprojection. Reconstructed View moves supported fixed-function "
        "geometry in recovered view space and falls back per draw when needed. "
        "Programmable-VSH 3D draws still use the safe post-VSH tail while "
        "recognized fullscreen passes remain untouched. CPU-side "
        "portal/frustum/LOD culling is still owned by the game, so geometry the "
        "game never submits cannot be recovered by either renderer mode.");
}

void FeatureFreecamDrawMiscMenuItem()
{
    ImGui::MenuItem("Free Camera", "F10", &g_freecam_window_open);
}

void FeatureFreecamDrawWindow()
{
    // Detached callbacks recurse into this function in a second ImGui
    // context. Camera input/timing must still be serviced exactly once from
    // the main frontend context each frame.
    if (!xemu_feature_detach::IsDetachedPass(kFreecamDetachId)) {
        FeatureFreecamTick();
    }

    xemu_feature_detach::Register(kFreecamDetachId, "Free Camera",
                                  &g_freecam_window_open,
                                  []() { FeatureFreecamDrawWindow(); });
    xemu_feature_detach::Pump();

    if (!g_freecam_window_open ||
        !xemu_feature_detach::ShouldDraw(kFreecamDetachId)) {
        return;
    }

    if (xemu_feature_detach::IsDetachedPass(kFreecamDetachId)) {
        xemu_feature_detach::PrepareWindow(kFreecamDetachId);
    } else {
        ImGui::SetNextWindowSize(ImVec2(520.0f, 620.0f),
                                 ImGuiCond_FirstUseEver);
    }
    const ImGuiWindowFlags flags =
        xemu_feature_detach::WindowFlags(kFreecamDetachId, 0);
    if (!ImGui::Begin("Free Camera", &g_freecam_window_open, flags)) {
        ImGui::End();
        return;
    }
    xemu_feature_detach::ObserveCurrentWindow(kFreecamDetachId);

    XemuFreecamStatus status{};
    xemu_freecam_get_status(&status);

    if (ImGui::BeginTabBar("##freecam_tabs")) {
        if (ImGui::BeginTabItem("Controls")) {
            DrawFreecamControls(status);
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Info")) {
            DrawFreecamInfo(status);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }

    ImGui::End();
}

bool FeatureFreecamWindowOpen()
{
    return g_freecam_window_open;
}
