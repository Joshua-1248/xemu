// SPDX-License-Identifier: GPL-2.0-or-later
//
// xemu custom fork - isolated TAS / TAStudio frontend
//
// Copyright (C) 2026 Joshua-1248
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// The emulator-facing TAS core lives beside this file in core.c. This file owns the
// large UI/editor/tooling surface so generic menubar/HUD code only sees a few
// narrow entry points.
//

#include "studio.hh"
#include "xemu-features/shared/detachable-windows.hh"

#include "ui/xui/common.hh"
#include "ui/xui/misc.hh"
#include "ui/xui/widgets.hh"
#include "ui/xui/actions.hh"
#include "ui/xui/snapshot-manager.hh"
#include "ui/xemu-notifications.h"
#include "ui/xemu-settings.h"
#include "ui/xemu-os-utils.h"
#include "xemu-xbe.h"
#include "ui/xemu-snapshots.h"
#include "xemu-features/shared/guest-memory.h"

extern "C" {
#include "xemu-features/tas/tas.h"
#include "xemu-features/fast-forward/fast-forward.h"
#include "xemu-version.h"
#include "qemu/fast-hash.h"
#include "hw/xbox/nv2a/nv2a.h"
#include "system/runstate.h"
}

#include <array>
#include <vector>
#include <algorithm>
#include <cstring>
#include <climits>
#include <string>
#include <filesystem>
#include <cerrno>
#include <ctime>
#include <deque>
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
#include <cmath>
#include <map>
#include <set>
#include <optional>
#include <functional>
#include <unordered_set>
#include <unordered_map>
#include <chrono>

extern float g_main_menu_height;

namespace {
using TasXidReport = std::array<uint8_t, XEMU_TAS_XID_REPORT_SIZE>;
using TasPollCounts = std::array<uint32_t, XEMU_TAS_MAX_PORTS>;
static constexpr uint32_t kTasUnknownPollCount = UINT32_MAX;

static TasPollCounts TasUnknownPollCounts()
{
    TasPollCounts p{};
    p.fill(kTasUnknownPollCount);
    return p;
}

struct TasFrame {
    std::array<TasXidReport, XEMU_TAS_MAX_PORTS> xid{};
};
static_assert(sizeof(TasFrame) == XEMU_TAS_FRAME_REPORT_BYTES,
              "TasFrame must stay byte-identical to four packed XID reports");

struct TasStateMeta {
    bool valid = false;
    uint64_t frame = 0;
    uint32_t branch_id = 0;
    XemuTasTransactionId transaction{};
};

struct TasChapter {
    uint64_t frame = 0;
    std::string name;
};

struct TasMarker {
    uint64_t frame = 0;
    std::string name;
    std::string note;
};

struct TasMovieProperties {
    std::string author;
    std::string category;
    std::string game_version;
    std::string comments;
};

struct TasHashRecord {
    uint64_t frame = 0;
    uint64_t hash = 0;
};

struct TasEditSnapshot {
    /* Partial snapshots cover input-only edits whose movie length does not
     * change. Structural edits still fall back to a full snapshot. This keeps
     * single-cell/curve editing fast even on very long movies. */
    bool partial = false;
    int range_start = 0;
    size_t total_frames = 0;
    std::vector<TasFrame> frames;
    std::vector<uint8_t> lag;
    std::vector<TasPollCounts> polls;
    std::vector<TasChapter> chapters;
    std::vector<TasMarker> markers;
    int selected_frame = 0;
    int selection_anchor = 0;
    int selection_end = 0;
    std::string description;
};

struct TasEnvironmentInfo {
    bool valid = false;
    uint32_t title_id = 0;
    std::string xemu_version;
    std::string xemu_commit;
    std::string disc_path;
    std::string bootrom_md5;
    std::string flashrom_md5;
    std::string eeprom_md5;
    std::string hdd_path;
    uint32_t renderer = 0;
    uint32_t surface_scale = 1;
    uint32_t fit = 0;
    uint32_t filtering = 0;
    uint32_t aspect = 0;
    uint32_t fast_forward = 0;
    bool deterministic = false;
};

struct TasRamWatch {
    uint32_t address = 0;
    int size = 4;
    bool hex = true;
    bool is_signed = false;
    std::string label;
    uint64_t last_value = 0;
    uint64_t previous_value = 0;
    bool valid = false;
};

struct TasBranch {
    uint32_t id = 0;
    uint32_t parent = UINT32_MAX;
    uint64_t fork_frame = 0;
    std::string name;
    std::vector<TasFrame> frames;
    std::vector<uint8_t> lag;
    std::vector<TasPollCounts> polls;
};

struct TasRewindCheckpoint {
    bool valid = false;
    uint64_t frame = 0;
    uint32_t branch_id = 0;
    uint64_t state_hash = 0;
    bool last_frame_lagged = false;
    uint64_t lag_count = 0;
    uint64_t lag_streak = 0;
    XemuTasTransactionId transaction{};
    std::string snapshot_name;
};

static bool g_tas_studio_open = false;
static bool g_tas_input_display_open = false;
/* Compact TAS Studio layout: keep the timeline primary and move infrequent
 * controls into the window menu bar. Expanded mode remains available. */
static bool g_tas_compact_ui = true;
static bool g_tas_exact_editor_open = false;
static bool g_tas_greenzone_panel_open = false;
static bool g_tas_verifier_panel_open = false;
static bool g_tas_punch_panel_open = false;
static bool g_tas_rewind_panel_open = false;
static int g_tas_port = 0;
static int g_tas_selected_frame = 0;
static std::vector<TasFrame> g_tas_frames(1);
static std::vector<TasPollCounts> g_tas_poll_counts(1, TasUnknownPollCounts());
static TasFrame g_tas_clipboard;
static bool g_tas_clipboard_valid = false;
static bool g_tas_follow_frame = true;
/* Double-clicking a piano-roll frame can either behave like a precise
 * greenzone seek (stop on the requested frame) or like a scrub-to-here
 * operation that resumes normal movie playback after the target is reached. */
static bool g_tas_double_click_continue = false;
static bool g_tas_seek_continue_pending = false;
static uint64_t g_tas_seek_continue_target = 0;
/* Every greenzone seek is verified after the VM reaches its requested pause.
 * This catches stale transport work or a broken frame boundary immediately
 * instead of letting the piano roll silently claim the wrong destination. */
static bool g_tas_seek_completion_pending = false;
static bool g_tas_step_completion_pending = false;
static uint64_t g_tas_step_completion_target = 0;
/* Strict Sync never allows the live VM to continue on a state produced by
 * movie input that has just been edited in its past. Re-simulate that state
 * from the last still-valid checkpoint on the next maintenance tick. */
static bool g_tas_strict_resim_pending = false;
static uint64_t g_tas_strict_resim_target = 0;
static bool g_tas_strict_resim_continue = false;
static std::string g_tas_movie_path;
static bool g_tas_goto_dialog_requested = false;
static int g_tas_goto_frame_value = 0;
static std::vector<uint8_t> g_tas_lag_flags(1, 0);
static bool g_tas_read_only = true;
static bool g_tas_power_on_recording = true;
static bool g_tas_power_on_reset_pending = false;
static bool g_tas_power_on_reset_start_vm = true;
static uint64_t g_tas_power_on_reset_sequence_before = 0;
static std::chrono::steady_clock::time_point g_tas_power_on_reset_deadline{};
/* Current-state movies need the exact VM state that existed before movie frame
 * 0. The native snapshot lives in Xemu's snapshot store; the .xmt records the
 * snapshot name + RAM fingerprint so Play From Beginning can restore the same
 * starting machine state instead of replaying frame 0 from whatever state the
 * Xbox happens to be in later. */
static std::string g_tas_movie_start_snapshot;
static uint64_t g_tas_movie_start_state_hash = 0;
static uint64_t g_tas_movie_start_legacy_ram_hash = 0;
static XemuTasTransactionId g_tas_movie_start_transaction{};
static bool g_tas_apply_movie_settings = true;
static uint64_t g_tas_rerecord_count = 0;
static uint64_t g_tas_record_synced = 0;
static bool g_tas_record_replace_placeholder = false;
static uint64_t g_tas_last_autosave_record_count = 0;
static std::chrono::steady_clock::time_point g_tas_next_autosave_host{};
static std::array<TasStateMeta, 100> g_tas_state_meta{};

/* Native snapshot metadata is comparatively expensive to enumerate from the
 * QCOW2 image. Cache names briefly and update the cache on TAS-owned changes
 * instead of refreshing the whole snapshot list during UI/rewind operations. */
static std::unordered_set<std::string> g_tas_snapshot_name_cache;
static bool g_tas_snapshot_cache_valid = false;
static std::chrono::steady_clock::time_point g_tas_snapshot_cache_deadline{};
static std::array<bool, 100> g_tas_state_slot_cache{};
static uint32_t g_tas_state_slot_cache_title = 0;
static bool g_tas_state_slot_cache_valid = false;

static std::vector<TasChapter> g_tas_chapters;
static std::vector<TasBranch> g_tas_branches;
static uint32_t g_tas_current_branch = 0;
static uint32_t g_tas_current_parent = UINT32_MAX;
static uint32_t g_tas_next_branch_id = 1;
static bool g_tas_branches_open = false;
static bool g_tas_chapters_open = false;
static bool g_tas_automation_open = false;
static bool g_tas_macro_open = false;
static bool g_tas_rewind_enabled = true;
static int g_tas_rewind_interval = 120;
static int g_tas_rewind_distance = 60;
static int g_tas_rewind_slot = 0;
static uint64_t g_tas_rewind_next_frame = 0;
static std::array<TasRewindCheckpoint, 64> g_tas_rewind_points{};
/* In deterministic mode checkpoints are never taken from an arbitrary point
 * inside a running frame. We schedule a one-frame advance, let the VBLANK TAS
 * hook pause the VM at the next canonical boundary, save there, then resume. */
static bool g_tas_strict_checkpoint_pending = false;
static bool g_tas_strict_checkpoint_resume = false;
/* User frame-advance in Strict Sync is already an exact VBLANK pause. Cache
 * that destination immediately so frame-by-frame TAS work gets exact rewind
 * anchors instead of replaying from a much older periodic checkpoint. */
static bool g_tas_strict_step_checkpoint_pending = false;
static std::string g_tas_undo_snapshot_name;
static XemuTasTransactionId g_tas_undo_transaction{};
static std::deque<std::string> g_tas_snapshot_delete_queue;
static std::unordered_set<std::string> g_tas_snapshot_delete_queued;

/* Movie editing / TAStudio-style selection. */
static int g_tas_selection_anchor = 0;
static int g_tas_selection_end = 0;
static std::vector<TasFrame> g_tas_clipboard_frames;
static std::vector<uint8_t> g_tas_clipboard_lag;
static std::vector<TasPollCounts> g_tas_clipboard_polls;
static std::deque<TasEditSnapshot> g_tas_undo_stack;
static std::deque<TasEditSnapshot> g_tas_redo_stack;
static uint64_t g_tas_movie_revision = 0;
static bool g_tas_movie_dirty = false;
static uint64_t g_tas_core_pushed_revision = UINT64_MAX;
static size_t g_tas_core_pushed_frame_count = 0;
static bool g_tas_hide_lag_frames = false;
static bool g_tas_dim_lag_frames = true;
static bool g_tas_all_ports_view = false;
static bool g_tas_visible_frame_cache_dirty = true;
static std::vector<int> g_tas_visible_frame_cache;

/* Movie annotations / metadata. */
static std::vector<TasMarker> g_tas_markers;
static TasMovieProperties g_tas_properties;
static bool g_tas_markers_open = false;
static bool g_tas_properties_open = false;
static TasEnvironmentInfo g_tas_loaded_environment;
static bool g_tas_compatibility_open = false;
static bool g_tas_history_open = false;

/* Determinism verifier. This intentionally hashes RAM + canonical movie input
 * at explicit deterministic checkpoints; later device-specific hashes can be
 * added without changing the .xmt extension trailer. */
enum class TasVerifyMode { Idle, Capture, Verify };
static TasVerifyMode g_tas_verify_mode = TasVerifyMode::Idle;
static std::vector<TasHashRecord> g_tas_verify_baseline;
static size_t g_tas_verify_index = 0;
static uint64_t g_tas_verify_start_frame = 0;
static uint64_t g_tas_verify_next_frame = 0;
static int g_tas_verify_interval = 120;
static bool g_tas_verify_failed = false;
static uint64_t g_tas_verify_first_bad_frame = UINT64_MAX;
static uint64_t g_tas_verify_expected = 0;
static uint64_t g_tas_verify_actual = 0;
static std::string g_tas_verify_status = "Not verified";
static std::string g_tas_verify_start_snapshot;
static uint32_t g_tas_verify_branch = 0;
static uint64_t g_tas_verify_revision = UINT64_MAX;
static XemuTasTransactionId g_tas_verify_start_transaction{};
static int g_tas_verify_baseline_interval = 120;
static int g_tas_verify_runs_total = 1;
static int g_tas_verify_runs_remaining = 0;
static int g_tas_verify_run_index = 0;
static bool g_tas_verify_exhaustive = false;
static uint64_t g_tas_verify_poll_sync_from = 0;

/* Greenzone / cached timeline. Uses native xemu internal snapshots and movie
 * replay, so the .xmt itself stays small. */
static bool g_tas_greenzone_enabled = false;
static int g_tas_greenzone_interval = 300;
static int g_tas_greenzone_capacity = 64;

/* Analog curve / pattern editing. */
static bool g_tas_curve_open = false;
static int g_tas_curve_control = 0; /* 0..3 sticks, 4..11 pressure bytes */
static int g_tas_curve_start_value = 0;
static int g_tas_curve_end_value = 0;
static int g_tas_curve_type = 0;
static int g_tas_circle_stick = 0;
static int g_tas_circle_radius = 32767;
static float g_tas_circle_turns = 1.0f;

/* Punch-in / overdub. */
static bool g_tas_overdub_ui_active = false;
static uint64_t g_tas_overdub_start_frame = 0;
static uint64_t g_tas_overdub_synced = 0;
static std::array<bool, 20> g_tas_overdub_fields = []{ std::array<bool,20> a{}; a.fill(true); return a; }();

/* Movie comparison. */
static bool g_tas_compare_open = false;
static std::string g_tas_compare_path;
static std::vector<TasFrame> g_tas_compare_frames;
static std::vector<uint8_t> g_tas_compare_lag;
static uint64_t g_tas_compare_first_diff = UINT64_MAX;
static uint64_t g_tas_compare_diff_count = 0;

/* RAM watch/search/RNG utilities. */
static bool g_tas_ram_tools_open = false;
static std::vector<TasRamWatch> g_tas_ram_watches;
static uint32_t g_tas_new_watch_address = 0;
static int g_tas_new_watch_size = 4;
static char g_tas_new_watch_label[128]{};
static uint32_t g_tas_search_value = 0;
static int g_tas_search_size = 4;
static std::vector<uint32_t> g_tas_search_results;
/* Previous-value snapshots are positionally paired with search_results.
 * A hash table wastes memory and lookup work for a list that is already kept
 * in stable ascending order. */
static std::vector<uint64_t> g_tas_search_previous;
static int g_tas_rng_watch = -1;
static std::deque<std::pair<uint64_t,uint64_t>> g_tas_rng_history;
static uint64_t g_tas_rng_last_frame = UINT64_MAX;
static uint64_t g_tas_ram_watch_last_tas_frame = UINT64_MAX;
static std::chrono::steady_clock::time_point g_tas_ram_watch_next_host_sample{};

/* HUD / status / crash recovery. */
static bool g_tas_hud_enabled = false;
static bool g_tas_recovery_notice_done = false;

static int g_tas_auto_control = 8;
static bool g_tas_auto_hold_enabled = false;
static bool g_tas_autofire_enabled = false;
static int g_tas_auto_value = 255;
static int g_tas_autofire_period = 2;
static int g_tas_autofire_phase = 0;

static int g_tas_macro_control = 8;
static int g_tas_macro_value = 255;
static int g_tas_macro_press_frames = 1;
static int g_tas_macro_gap_frames = 1;
static int g_tas_macro_repeats = 4;

enum class TasRuntimeMode : uint8_t {
    Idle = 0,
    Playback = 1,
    Recording = 2,
    Overdub = 3,
};

struct TasStateBundleRuntime {
    bool valid = false;
    XemuTasTransactionId transaction{};
    uint64_t boundary = 0;
    uint64_t state_hash = 0;
    bool last_frame_lagged = false;
    uint64_t lag_count = 0;
    uint64_t lag_streak = 0;
    TasRuntimeMode mode = TasRuntimeMode::Idle;
    bool vm_was_running = false;
    int selected_frame = 0;
    int selection_anchor = 0;
    int selection_end = 0;
    bool follow = false;
    bool read_only = true;
    uint32_t branch_id = 0;
    uint64_t movie_revision = 0;
    uint64_t overdub_start = 0;
    uint64_t overdub_synced = 0;
    uint32_t overdub_port = 0;
    uint32_t overdub_field_mask = 0;
    std::string owner_movie_path;
};

static TasStateBundleRuntime g_tas_loaded_state_bundle;
static bool g_tas_silent_bundle_load = false;

/* Manual piano-roll mutations must not race an active movie runtime.  The
 * first edit freezes and drains Record/Overdub, keeps the original runtime
 * intent, and defers reconstruction until the UI edit gesture is complete.
 * This also makes analog drags atomic instead of restarting the VM after the
 * first changed pixel while the mouse is still down. */
struct TasTimelineMutationTxn {
    bool active = false;
    bool edited = false;
    bool reconstructing = false;
    TasRuntimeMode mode = TasRuntimeMode::Idle;
    bool vm_was_running = false;
    uint64_t boundary = 0;
    uint64_t first_frame = UINT64_MAX;
    uint32_t overdub_port = 0;
    uint32_t overdub_field_mask = 0;
};
static TasTimelineMutationTxn g_tas_timeline_mutation;

struct TasBranchSwitchTxn {
    bool active = false;
    bool reconstructing = false;
    std::string guard_snapshot;
    std::string guard_bundle_path;
    TasStateBundleRuntime old_runtime;
    uint64_t target_boundary = 0;
};
static TasBranchSwitchTxn g_tas_branch_switch;

struct TasPendingOverdubStart {
    bool active = false;
    uint64_t boundary = 0;
    uint8_t port = 0;
    uint16_t digital_mask = 0;
    uint8_t analog_mask = 0;
    uint8_t stick_mask = 0;
};
static TasPendingOverdubStart g_tas_pending_overdub_start;

static void TasAutosaveRecovery(bool force);

static void TasPushMovieToCore();
static bool TasSaveMovieToPathInternal(const char *path, bool set_current_path,
                                       bool notify,
                                       const TasStateBundleRuntime *state_bundle = nullptr);
static bool TasLoadMovieFromPath(const char *path);
static void TasStartPlayback(uint64_t frame);
static void TasPlayMovieFromBeginning();
static void TasInvalidateGreenzoneFrom(uint64_t frame);
static bool TasSaveRewindCheckpointAtBoundary(uint64_t frame);
static bool TasSeekFrame(uint64_t target);
static bool TasSeekFrameEx(uint64_t target, bool continue_after_target);
static void TasSeekContinueTick();
static void TasCancelPendingTransportAdvance();
static void TasStopOverdub();
static bool TasFinishPendingOverdubStart();
static void TasSyncRecordingFromCore();
static void TasPowerOnResetTick();
static void TasPrepareTimelineMutation(uint64_t first_frame);
static void TasNoteTimelineMutation(uint64_t first_frame);
static void TasTimelineMutationTick();
static void TasAbortTimelineMutation(const char *reason);
static bool TasSwitchToBranchIndex(size_t index);
static void TasFinishBranchSwitch();
static void TasRollbackBranchSwitch(const char *reason);

static std::pair<int,int> TasSelectionBounds()
{
    int last = std::max(0, (int)g_tas_frames.size() - 1);
    int a = std::clamp(g_tas_selection_anchor, 0, last);
    int b = std::clamp(g_tas_selection_end, 0, last);
    if (a > b) std::swap(a, b);
    return {a, b};
}

static void TasSetSelection(int frame, bool extend)
{
    int last = std::max(0, (int)g_tas_frames.size() - 1);
    frame = std::clamp(frame, 0, last);
    if (!extend) g_tas_selection_anchor = frame;
    g_tas_selection_end = frame;
    g_tas_selected_frame = frame;
    g_tas_follow_frame = false;
}

static TasEditSnapshot TasCaptureEditSnapshot(const char *description)
{
    TasEditSnapshot snap;
    snap.partial = false;
    snap.total_frames = g_tas_frames.size();
    snap.frames = g_tas_frames;
    snap.lag = g_tas_lag_flags;
    snap.polls = g_tas_poll_counts;
    snap.chapters = g_tas_chapters;
    snap.markers = g_tas_markers;
    snap.selected_frame = g_tas_selected_frame;
    snap.selection_anchor = g_tas_selection_anchor;
    snap.selection_end = g_tas_selection_end;
    snap.description = description ? description : "Edit";
    return snap;
}

static TasEditSnapshot TasCaptureEditSnapshotRange(const char *description, int first, int last)
{
    TasEditSnapshot snap;
    const int movie_last = std::max(0, (int)g_tas_frames.size() - 1);
    first = std::clamp(first, 0, movie_last);
    last = std::clamp(last, first, movie_last);
    snap.partial = true;
    snap.range_start = first;
    snap.total_frames = g_tas_frames.size();
    snap.frames.assign(g_tas_frames.begin() + first, g_tas_frames.begin() + last + 1);
    if (g_tas_lag_flags.size() < g_tas_frames.size()) {
        g_tas_lag_flags.resize(g_tas_frames.size(), 0);
    }
    if (g_tas_poll_counts.size() < g_tas_frames.size()) {
        g_tas_poll_counts.resize(g_tas_frames.size(), TasUnknownPollCounts());
    }
    snap.lag.assign(g_tas_lag_flags.begin() + first, g_tas_lag_flags.begin() + last + 1);
    snap.polls.assign(g_tas_poll_counts.begin() + first,
                      g_tas_poll_counts.begin() + last + 1);
    snap.selected_frame = g_tas_selected_frame;
    snap.selection_anchor = g_tas_selection_anchor;
    snap.selection_end = g_tas_selection_end;
    snap.description = description ? description : "Edit";
    return snap;
}

static TasEditSnapshot TasCaptureEditSnapshotLike(const TasEditSnapshot &shape,
                                                   const char *description)
{
    if (shape.partial && !shape.frames.empty() &&
        shape.total_frames == g_tas_frames.size()) {
        const int first = shape.range_start;
        const int last = first + (int)shape.frames.size() - 1;
        if (first >= 0 && last < (int)g_tas_frames.size()) {
            return TasCaptureEditSnapshotRange(description, first, last);
        }
    }
    return TasCaptureEditSnapshot(description);
}

static size_t TasEditSnapshotApproxBytes(const TasEditSnapshot &snap)
{
    size_t bytes = sizeof(snap);
    bytes += snap.frames.size() * sizeof(TasFrame);
    bytes += snap.lag.size();
    bytes += snap.polls.size() * sizeof(TasPollCounts);
    bytes += snap.chapters.size() * sizeof(TasChapter);
    bytes += snap.markers.size() * sizeof(TasMarker);
    for (const TasChapter &c : snap.chapters) bytes += c.name.size();
    for (const TasMarker &m : snap.markers) bytes += m.name.size() + m.note.size();
    bytes += snap.description.size();
    return bytes;
}

static void TasTrimEditStack(std::deque<TasEditSnapshot> &stack)
{
    constexpr size_t kMaxUndoEntries = 64;
    constexpr size_t kUndoMemoryBudget = 128u * 1024u * 1024u;
    while (stack.size() > kMaxUndoEntries) {
        stack.pop_front();
    }
    size_t bytes = 0;
    for (const TasEditSnapshot &snap : stack) {
        bytes += TasEditSnapshotApproxBytes(snap);
    }
    /* Keep at least the newest entry even if a single structural edit is very
     * large; don't let a long TAS multiply that cost across the entire stack. */
    while (stack.size() > 1 && bytes > kUndoMemoryBudget) {
        bytes -= TasEditSnapshotApproxBytes(stack.front());
        stack.pop_front();
    }
}

static void TasPushUndo(const char *description)
{
    g_tas_undo_stack.push_back(TasCaptureEditSnapshot(description));
    TasTrimEditStack(g_tas_undo_stack);
    g_tas_redo_stack.clear();
}

static void TasPushTimelineUndo(const char *description, uint64_t first_frame)
{
    TasPrepareTimelineMutation(first_frame);
    TasPushUndo(description);
}

static void TasPushUndoRange(const char *description, int first, int last)
{
    TasPrepareTimelineMutation((uint64_t)std::max(0, first));
    g_tas_undo_stack.push_back(TasCaptureEditSnapshotRange(description, first, last));
    TasTrimEditStack(g_tas_undo_stack);
    g_tas_redo_stack.clear();
}

static bool TasRestoreEditSnapshot(const TasEditSnapshot &snap)
{
    uint64_t invalidate_from = 0;
    if (snap.partial) {
        const int first = snap.range_start;
        const int count = (int)snap.frames.size();
        if (snap.total_frames != g_tas_frames.size() || first < 0 || count <= 0 ||
            first + count > (int)g_tas_frames.size()) {
            xemu_queue_error_message("TAS undo range no longer matches the movie structure");
            return false;
        }
        std::copy(snap.frames.begin(), snap.frames.end(), g_tas_frames.begin() + first);
        if (g_tas_lag_flags.size() < g_tas_frames.size()) {
            g_tas_lag_flags.resize(g_tas_frames.size(), 0);
        }
        std::copy(snap.lag.begin(), snap.lag.end(), g_tas_lag_flags.begin() + first);
        if (g_tas_poll_counts.size() < g_tas_frames.size()) {
            g_tas_poll_counts.resize(g_tas_frames.size(), TasUnknownPollCounts());
        }
        std::copy(snap.polls.begin(), snap.polls.end(),
                  g_tas_poll_counts.begin() + first);
        invalidate_from = (uint64_t)first;
    } else {
        g_tas_frames = snap.frames;
        g_tas_lag_flags = snap.lag;
        g_tas_poll_counts = snap.polls;
        g_tas_chapters = snap.chapters;
        g_tas_markers = snap.markers;
        if (g_tas_frames.empty()) g_tas_frames.resize(1);
        if (g_tas_lag_flags.size() < g_tas_frames.size()) {
            g_tas_lag_flags.resize(g_tas_frames.size(), 0);
        }
        if (g_tas_poll_counts.size() < g_tas_frames.size()) {
            g_tas_poll_counts.resize(g_tas_frames.size(), TasUnknownPollCounts());
        }
        invalidate_from = (uint64_t)std::max(0, std::min(snap.selection_anchor,
                                                         snap.selection_end));
    }

    g_tas_selected_frame = std::clamp(snap.selected_frame, 0, (int)g_tas_frames.size() - 1);
    g_tas_selection_anchor = std::clamp(snap.selection_anchor, 0, (int)g_tas_frames.size() - 1);
    g_tas_selection_end = std::clamp(snap.selection_end, 0, (int)g_tas_frames.size() - 1);
    g_tas_movie_dirty = true;
    ++g_tas_movie_revision;
    g_tas_core_pushed_revision = UINT64_MAX;
    g_tas_visible_frame_cache_dirty = true;
    TasInvalidateGreenzoneFrom(invalidate_from);
    TasNoteTimelineMutation(invalidate_from);
    return true;
}

static void TasUndoEdit()
{
    if (g_tas_undo_stack.empty()) return;
    const TasEditSnapshot &pending = g_tas_undo_stack.back();
    const uint64_t first = pending.partial ? (uint64_t)std::max(0, pending.range_start) : 0;
    TasPrepareTimelineMutation(first);
    TasEditSnapshot snap = std::move(g_tas_undo_stack.back());
    g_tas_undo_stack.pop_back();
    TasEditSnapshot current = TasCaptureEditSnapshotLike(snap, "Redo");
    if (TasRestoreEditSnapshot(snap)) {
        g_tas_redo_stack.push_back(std::move(current));
        TasTrimEditStack(g_tas_redo_stack);
        xemu_queue_notification(("TAS undo: " + snap.description).c_str());
    } else {
        g_tas_undo_stack.push_back(std::move(snap));
    }
}

static void TasRedoEdit()
{
    if (g_tas_redo_stack.empty()) return;
    const TasEditSnapshot &pending = g_tas_redo_stack.back();
    const uint64_t first = pending.partial ? (uint64_t)std::max(0, pending.range_start) : 0;
    TasPrepareTimelineMutation(first);
    TasEditSnapshot snap = std::move(g_tas_redo_stack.back());
    g_tas_redo_stack.pop_back();
    TasEditSnapshot current = TasCaptureEditSnapshotLike(snap, "Undo");
    if (TasRestoreEditSnapshot(snap)) {
        g_tas_undo_stack.push_back(std::move(current));
        TasTrimEditStack(g_tas_undo_stack);
        xemu_queue_notification("TAS redo");
    } else {
        g_tas_redo_stack.push_back(std::move(snap));
    }
}

static void TasMarkMovieEdited(uint64_t first_frame)
{
    TasNoteTimelineMutation(first_frame);
    g_tas_movie_dirty = true;
    ++g_tas_movie_revision;
    g_tas_core_pushed_revision = UINT64_MAX;
    g_tas_visible_frame_cache_dirty = true;
    TasInvalidateGreenzoneFrom(first_frame);
    if (g_tas_poll_counts.size() < g_tas_frames.size()) {
        g_tas_poll_counts.resize(g_tas_frames.size(), TasUnknownPollCounts());
    }
    for (uint64_t i = first_frame; i < g_tas_poll_counts.size(); ++i) {
        g_tas_poll_counts[(size_t)i] = TasUnknownPollCounts();
    }

    /* A verifier hash at/after an edited input frame describes the old movie,
     * not the new branch. Keeping it would turn an intentional edit into a
     * misleading "desync" later. Preserve only the still-valid prefix. */
    g_tas_verify_baseline.erase(
        std::remove_if(g_tas_verify_baseline.begin(), g_tas_verify_baseline.end(),
                       [first_frame](const TasHashRecord &h) {
                           return h.frame >= first_frame;
                       }),
        g_tas_verify_baseline.end());
    if (g_tas_verify_mode != TasVerifyMode::Idle) {
        g_tas_verify_mode = TasVerifyMode::Idle;
        g_tas_verify_status = "Verifier baseline invalidated by movie edit";
    }
    g_tas_verify_revision = UINT64_MAX;

    const uint64_t live_frame = xemu_tas_playback()
        ? xemu_tas_playback_frame() : xemu_tas_frame();
    if (!g_tas_timeline_mutation.active && xemu_tas_deterministic_mode() &&
        !xemu_tas_recording() && first_frame < live_frame) {
        /* The VM has already executed input that no longer exists in the
         * edited movie. Freeze immediately; on the next maintenance tick seek
         * from a surviving pre-edit checkpoint through the new input history. */
        g_tas_strict_resim_target = std::min<uint64_t>(
            live_frame, g_tas_frames.size());
        g_tas_strict_resim_continue =
            xemu_tas_playback() && runstate_is_running();
        g_tas_strict_resim_pending = true;
        TasCancelPendingTransportAdvance();
        xemu_tas_stop_playback();
        if (runstate_is_running()) {
            vm_stop(RUN_STATE_PAUSED);
        }
    } else if (xemu_tas_playback() && first_frame >= live_frame) {
        /* Future-only edits do not invalidate the current VM state. Publish
         * them to the active core copy without rewinding the playback cursor. */
        if (xemu_tas_update_playback_movie_ex(
                g_tas_frames.data(), g_tas_lag_flags.data(),
                reinterpret_cast<const uint32_t *>(g_tas_poll_counts.data()),
                g_tas_poll_counts.size() * XEMU_TAS_MAX_PORTS,
                g_tas_frames.size())) {
            g_tas_core_pushed_revision = g_tas_movie_revision;
            g_tas_core_pushed_frame_count = g_tas_frames.size();
        }
    }
}

static uint64_t TasFnv1a64(const void *data, size_t size, uint64_t h = 1469598103934665603ULL)
{
    const uint8_t *p = static_cast<const uint8_t *>(data);
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= 1099511628211ULL;
    }
    return h;
}

static bool TasComputeRamHash(uint64_t *out_hash)
{
    if (!out_hash) return false;
    const uint64_t ram_size = xemu_guest_ram_size();
    if (!ram_size) return false;
    static std::vector<uint8_t> buf(1 << 20);
    uint64_t h = 1469598103934665603ULL;
    uint64_t hashed = 0;
    for (uint64_t off = 0; off < ram_size; off += buf.size()) {
        size_t n = (size_t)std::min<uint64_t>(buf.size(), ram_size - off);
        ssize_t got = xemu_phys_read((uint32_t)off, buf.data(), n);
        if (got != (ssize_t)n) return false;
        const uint64_t chunk_hash = fast_hash(buf.data(), (size_t)got);
        h = TasFnv1a64(&chunk_hash, sizeof(chunk_hash), h);
        hashed += (uint64_t)got;
    }
    if (hashed != ram_size) return false;
    *out_hash = h;
    return true;
}

/* QEMU's standalone non-RAM serializer is stronger than RAM-only hashing,
 * but some device configurations cannot be serialized through that helper even
 * though ordinary native Xemu snapshots work. Detect support once per process.
 * A first-probe failure deliberately falls back to RAM + TAS boundary state so
 * recording itself is never disabled by an optional verifier layer. If the
 * serializer succeeds once, later failures remain hard errors rather than
 * silently weakening an established proof mode. */
static int g_tas_nonram_fingerprint_capability = -1; /* -1 unknown, 0 fallback, 1 full */

static bool TasComputeStateHash(uint64_t *out_hash)
{
    if (!out_hash) return false;
    /* Full strict fingerprint: guest RAM plus QEMU's canonical serialized
     * non-RAM VM/device state (CPU, clocks/timers, NV2A/APU/IDE/USB, etc.) and
     * the TAS movie/input/lag state associated with this machine boundary. */
    uint64_t h = 0;
    if (!TasComputeRamHash(&h)) return false;

    if (g_tas_nonram_fingerprint_capability != 0) {
        uint64_t full_h = h;
        if (xemu_tas_machine_state_hash(h, &full_h)) {
            h = full_h;
            g_tas_nonram_fingerprint_capability = 1;
        } else if (g_tas_nonram_fingerprint_capability < 0) {
            /* First probe failed on this device configuration. Keep the
             * transaction-bound native snapshot authoritative and use a
             * stable, explicitly tagged RAM+TAS fallback fingerprint. */
            g_tas_nonram_fingerprint_capability = 0;
        } else {
            /* Full VMState hashing was already proven available earlier in
             * this session. Losing it later is a real strict-sync failure. */
            return false;
        }
    }
    if (g_tas_nonram_fingerprint_capability == 0) {
        static const uint64_t fallback_tag =
            UINT64_C(0x58454d5554415346); /* XEMUTASF */
        h = TasFnv1a64(&fallback_tag, sizeof(fallback_tag), h);
    }

    uint64_t frame = xemu_tas_frame();
    h = TasFnv1a64(&frame, sizeof(frame), h);
    /* Boundary N means rows [0,N) have executed and row N is still future.
     * Fingerprint the just-consumed row, never an unexecuted future input. */
    if (frame > 0 && frame - 1 < g_tas_frames.size()) {
        const size_t executed = (size_t)(frame - 1);
        h = TasFnv1a64(&g_tas_frames[executed], sizeof(TasFrame), h);
        uint8_t lag = executed < g_tas_lag_flags.size() ? g_tas_lag_flags[executed] : 0;
        h = TasFnv1a64(&lag, 1, h);
        if (executed < g_tas_poll_counts.size()) {
            h = TasFnv1a64(&g_tas_poll_counts[executed],
                           sizeof(TasPollCounts), h);
        }
    }
    const uint8_t last_lag = xemu_tas_last_frame_lagged() ? 1 : 0;
    const uint64_t lag_count = xemu_tas_lag_count();
    const uint64_t lag_streak = xemu_tas_lag_streak();
    h = TasFnv1a64(&last_lag, sizeof(last_lag), h);
    h = TasFnv1a64(&lag_count, sizeof(lag_count), h);
    h = TasFnv1a64(&lag_streak, sizeof(lag_streak), h);
    *out_hash = h;
    return true;
}

static bool TasReadMemoryValue(uint32_t address, int size, uint64_t *value)
{
    if (!value || (size != 1 && size != 2 && size != 4 && size != 8)) return false;
    uint64_t v = 0;
    if (xemu_phys_read(address, &v, (size_t)size) != size) return false;
    *value = v;
    return true;
}

static void TasUpdateRamWatches()
{
    if (g_tas_ram_watches.empty()) {
        return;
    }

    /* Watches are useful background data only while something can consume
     * them. Do not silently read guest RAM every host UI frame. */
    const bool consumer_active = g_tas_ram_tools_open || g_tas_hud_enabled ||
                                 (g_tas_rng_watch >= 0);
    if (!consumer_active) {
        return;
    }

    if (xemu_tas_enabled()) {
        const uint64_t frame = xemu_tas_frame();
        if (frame == g_tas_ram_watch_last_tas_frame) {
            return;
        }
        g_tas_ram_watch_last_tas_frame = frame;
    } else {
        const auto now = std::chrono::steady_clock::now();
        if (now < g_tas_ram_watch_next_host_sample) {
            return;
        }
        /* Non-TAS use does not need UI-refresh-rate polling. 20 Hz keeps watch
         * displays responsive while making the background cost negligible. */
        g_tas_ram_watch_next_host_sample = now + std::chrono::milliseconds(50);
    }

    for (TasRamWatch &w : g_tas_ram_watches) {
        uint64_t v = 0;
        w.previous_value = w.last_value;
        w.valid = TasReadMemoryValue(w.address, w.size, &v);
        if (w.valid) {
            w.last_value = v;
        }
    }
    if (g_tas_rng_watch >= 0 && g_tas_rng_watch < (int)g_tas_ram_watches.size()) {
        const uint64_t frame = xemu_tas_frame();
        TasRamWatch &w = g_tas_ram_watches[g_tas_rng_watch];
        if (w.valid && frame != g_tas_rng_last_frame) {
            g_tas_rng_history.emplace_back(frame, w.last_value);
            while (g_tas_rng_history.size() > 256) {
                g_tas_rng_history.pop_front();
            }
            g_tas_rng_last_frame = frame;
        }
    }
}

static std::string TasHexReport(const TasXidReport &r)
{
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t b : r) ss << std::setw(2) << (unsigned)b;
    return ss.str();
}

static uint16_t tas_read_u16(const TasXidReport &r, int off)
{
    uint16_t v;
    memcpy(&v, &r[off], sizeof(v));
    return v;
}

static int16_t tas_read_s16(const TasXidReport &r, int off)
{
    int16_t v;
    memcpy(&v, &r[off], sizeof(v));
    return v;
}

static void tas_write_s16(TasXidReport &r, int off, int16_t v)
{
    memcpy(&r[off], &v, sizeof(v));
}

static void TasCaptureCurrentFrame()
{
    if (g_tas_selected_frame < 0 || g_tas_selected_frame >= (int)g_tas_frames.size()) {
        return;
    }
    xemu_tas_get_last_xid_report(
        (uint8_t)g_tas_port,
        g_tas_frames[g_tas_selected_frame].xid[g_tas_port].data(),
        XEMU_TAS_XID_REPORT_SIZE);
}

static void TasApplySelectedFrame()
{
    if (g_tas_selected_frame < 0 || g_tas_selected_frame >= (int)g_tas_frames.size()) {
        return;
    }
    if (!xemu_tas_enabled()) {
        xemu_tas_set_enabled(true);
    }
    xemu_tas_set_xid_report(
        (uint8_t)g_tas_port,
        g_tas_frames[g_tas_selected_frame].xid[g_tas_port].data(),
        XEMU_TAS_XID_REPORT_SIZE);
}

static void DrawTasInputDisplay()
{
    static constexpr const char *kDetachId = "tas.input-display";
    xemu_feature_detach::Register(kDetachId, "TAS Input Display",
                                  &g_tas_input_display_open, DrawTasInputDisplay);
    if (!g_tas_input_display_open || !xemu_feature_detach::ShouldDraw(kDetachId)) return;
    if (xemu_feature_detach::IsDetachedPass(kDetachId)) {
        xemu_feature_detach::PrepareWindow(kDetachId);
    }
    if (!ImGui::Begin("TAS Input Display", &g_tas_input_display_open,
                      xemu_feature_detach::WindowFlags(kDetachId, 0))) {
        ImGui::End();
        return;
    }
    xemu_feature_detach::ObserveCurrentWindow(kDetachId);
    ImGui::Text("TAS frame: %llu", (unsigned long long)xemu_tas_frame());
    for (int port = 0; port < XEMU_TAS_MAX_PORTS; ++port) {
        uint8_t r[20]{};
        if (xemu_tas_get_last_xid_report((uint8_t)port, r, sizeof(r))) {
            uint16_t buttons; int16_t lx,ly,rx,ry;
            memcpy(&buttons, &r[2], 2); memcpy(&lx,&r[12],2); memcpy(&ly,&r[14],2);
            memcpy(&rx,&r[16],2); memcpy(&ry,&r[18],2);
            ImGui::Text("P%d Btn:%04X A:%u B:%u X:%u Y:%u Bl:%u Wh:%u LT:%u RT:%u  L(%d,%d) R(%d,%d)",
                        port + 1, buttons, r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11],
                        lx, ly, rx, ry);
        } else {
            ImGui::TextDisabled("P%d: waiting for first XID poll", port + 1);
        }
    }
    ImGui::End();
}

static void DrawTasGoToFrameDialog()
{
    if (!g_tas_goto_dialog_requested) return;
    ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_Appearing);
    if (!ImGui::Begin("Go To TAS Frame", &g_tas_goto_dialog_requested,
                      ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::End();
        return;
    }

    const int last_frame = std::max(0, (int)g_tas_frames.size() - 1);
    ImGui::Text("Movie range: 0 - %d", last_frame);
    ImGui::SetNextItemWidth(180.0f);
    ImGui::InputInt("Frame", &g_tas_goto_frame_value, 1, 10);
    g_tas_goto_frame_value = std::clamp(g_tas_goto_frame_value, 0, last_frame);

    if (ImGui::Button("Select")) {
        TasSetSelection(g_tas_goto_frame_value, false);
        g_tas_studio_open = true;
        g_tas_goto_dialog_requested = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Seek / Replay")) {
        TasSeekFrame((uint64_t)g_tas_goto_frame_value);
        g_tas_studio_open = true;
        g_tas_goto_dialog_requested = false;
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) {
        g_tas_goto_dialog_requested = false;
    }
    ImGui::End();
}

static int g_tas_state_slot = 0;

static bool TasGetCurrentTitleId(uint32_t *title_id)
{
    /* TAS Studio can ask for the state-slot name several times per UI frame.
     * The running title cannot meaningfully change at UI-frame granularity,
     * while xemu_get_xbe_title_id() must synchronize CPU architectural state.
     * Cache the tiny result for 50 ms (20 Hz) and keep failure short-lived so
     * title startup/reset detection remains responsive. */
    static uint32_t cached_title_id = 0;
    static bool cached_valid = false;
    static std::chrono::steady_clock::time_point refresh_at{};

    const auto now = std::chrono::steady_clock::now();
    if (now >= refresh_at) {
        cached_valid = xemu_get_xbe_title_id(&cached_title_id);
        refresh_at = now + std::chrono::milliseconds(50);
    }
    if (cached_valid && title_id) {
        *title_id = cached_title_id;
    }
    return cached_valid;
}

static bool TasBuildStateName(char *buf, size_t buf_size)
{
    uint32_t title_id = 0;
    if (!TasGetCurrentTitleId(&title_id)) {
        if (buf_size) {
            buf[0] = 0;
        }
        return false;
    }
    snprintf(buf, buf_size, "%08X_TAS_%02d", title_id,
             std::clamp(g_tas_state_slot, 0, 99));
    return true;
}

static void TasRefreshSnapshotCache(bool force = false)
{
    const auto now = std::chrono::steady_clock::now();
    if (!force && g_tas_snapshot_cache_valid && now < g_tas_snapshot_cache_deadline) {
        return;
    }

    /* Do not use SnapshotManager::Refresh() here: it decodes every snapshot's
     * Xemu metadata/PNG thumbnail and may create GL textures. TAS only needs
     * names for slots/greenzone bookkeeping. */
    Error *err = NULL;
    int count = 0;
    char **names = xemu_snapshots_list_names(&count, &err);
    if (err) {
        error_free(err);
        g_strfreev(names);
        g_tas_snapshot_cache_valid = false;
        return;
    }

    g_tas_snapshot_name_cache.clear();
    g_tas_snapshot_name_cache.reserve((size_t)std::max(16, count * 2));
    for (int i = 0; i < count; ++i) {
        if (names && names[i]) {
            g_tas_snapshot_name_cache.emplace(names[i]);
        }
    }
    g_strfreev(names);
    g_tas_snapshot_cache_valid = true;
    g_tas_snapshot_cache_deadline = now + std::chrono::seconds(1);
}

static bool TasSnapshotExists(const char *name)
{
    if (!name || !*name) return false;
    TasRefreshSnapshotCache();
    return g_tas_snapshot_name_cache.find(name) != g_tas_snapshot_name_cache.end();
}

static void TasSnapshotCacheInsert(const char *name)
{
    if (!name || !*name || !g_tas_snapshot_cache_valid) return;
    /* The snapshot was just created by us; no need to enumerate the QCOW2. */
    g_tas_snapshot_name_cache.emplace(name);
}

static void TasSnapshotCacheErase(const char *name)
{
    if (!name || !*name || !g_tas_snapshot_cache_valid) return;
    g_tas_snapshot_name_cache.erase(name);
}

static void TasInvalidateSnapshotCache()
{
    g_tas_snapshot_cache_valid = false;
    g_tas_state_slot_cache_valid = false;
}

static bool TasCaptureMovieStartSnapshot()
{
    /* Caller keeps the VM paused until movie-frame-0 recording is armed. */
    if (runstate_is_running()) {
        xemu_queue_error_message("Internal TAS error: movie-start snapshot requested while VM is running");
        return false;
    }

    uint32_t title_id = 0;
    TasGetCurrentTitleId(&title_id);
    const uint64_t stamp = (uint64_t)std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    char name[96];
    snprintf(name, sizeof(name), "%08X_TAS_START_%016llX", title_id,
             (unsigned long long)stamp);

    XemuTasTransactionId transaction{};
    xemu_tas_transaction_mint(&transaction);

    Error *err = NULL;
    xemu_tas_transaction_snapshot_begin();
    xemu_snapshots_save_no_thumbnail(name, &err);
    xemu_tas_transaction_snapshot_end();
    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_free(err);
        return false;
    }

    TasSnapshotCacheInsert(name);
    g_tas_movie_start_snapshot = name;
    g_tas_movie_start_transaction = transaction;
    if (!TasComputeRamHash(&g_tas_movie_start_legacy_ram_hash) ||
        !TasComputeStateHash(&g_tas_movie_start_state_hash)) {
        xemu_queue_error_message(
            "Could not fingerprint canonical TAS movie-start state even with the safe fallback; snapshot was not accepted");
        if (g_tas_snapshot_delete_queued.emplace(name).second) {
            g_tas_snapshot_delete_queue.push_back(name);
        }
        g_tas_movie_start_snapshot.clear();
        g_tas_movie_start_transaction = {};
        g_tas_movie_start_legacy_ram_hash = 0;
        g_tas_movie_start_state_hash = 0;
        return false;
    }
    return true;
}

static bool TasRestoreMovieStartSnapshot()
{
    if (g_tas_movie_start_snapshot.empty()) {
        xemu_queue_error_message(
            "This TAS has no canonical movie-start state. Re-record it from the desired starting point so playback can be deterministic.");
        return false;
    }
    if (!TasSnapshotExists(g_tas_movie_start_snapshot.c_str())) {
        xemu_queue_error_message(
            "The TAS movie-start snapshot is not available in this Xemu snapshot store; playback cannot be reproduced exactly.");
        return false;
    }

    xemu_tas_prepare_runtime();
    Error *err = NULL;
    bool was_running = false;
    const bool loaded = xemu_snapshots_load_paused(g_tas_movie_start_snapshot.c_str(),
                                                    &was_running, &err);
    (void)was_running;
    if (err || !loaded) {
        if (err) {
            xemu_queue_error_message(error_get_pretty(err));
            error_free(err);
        } else {
            xemu_queue_error_message("Could not restore TAS movie-start snapshot");
        }
        return false;
    }

    /* Native snapshots do not own TAStudio's feature counters. Canonical
     * movie-start state is always boundary 0 with fresh lag bookkeeping. */
    if (!xemu_tas_enabled()) xemu_tas_set_enabled(true);
    xemu_tas_set_frame(0);
    xemu_tas_reset_lag_counters();

    if ((g_tas_movie_start_transaction.hi || g_tas_movie_start_transaction.lo) &&
        !xemu_tas_transaction_matches(&g_tas_movie_start_transaction)) {
        xemu_queue_error_message(
            "TAS movie-start snapshot transaction ID does not match the movie metadata; playback remains paused");
        return false;
    }

    if (g_tas_movie_start_state_hash) {
        uint64_t actual = 0;
        if (!TasComputeStateHash(&actual)) {
            xemu_queue_error_message(
                "Could not compute TAS movie-start machine fingerprint; restore rejected");
            return false;
        }
        if (actual != g_tas_movie_start_state_hash) {
            xemu_queue_error_message(
                "TAS movie-start snapshot failed Strict Sync machine-state validation; playback remains paused");
            return false;
        }
    } else if (g_tas_movie_start_legacy_ram_hash) {
        uint64_t actual = 0;
        if (!TasComputeRamHash(&actual)) {
            xemu_queue_error_message(
                "Could not compute legacy TAS movie-start RAM fingerprint; restore rejected");
            return false;
        }
        if (actual != g_tas_movie_start_legacy_ram_hash) {
            xemu_queue_error_message(
                "Legacy TAS movie-start snapshot failed RAM validation; playback remains paused");
            return false;
        }
    }
    return true;
}

static std::string TasDefaultMovieDirectory()
{
    std::filesystem::path dir = xemu_settings_get_base_path();
    dir /= "tas_movies";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

static std::string TasPatternDirectory()
{
    std::filesystem::path dir = xemu_settings_get_base_path();
    dir /= "tas_patterns";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    return dir.string();
}

static std::string TasRecoveryPath()
{
    if (!g_tas_movie_path.empty()) return g_tas_movie_path + ".autosave";
    uint32_t title_id = 0;
    TasGetCurrentTitleId(&title_id);
    char name[64];
    snprintf(name, sizeof(name), "%08X_Untitled.autosave.xmt", title_id);
    return (std::filesystem::path(TasDefaultMovieDirectory()) / name).string();
}

static void TasBackupExistingMovie(const std::string &path)
{
    std::error_code ec;
    std::filesystem::path src(path);
    if (!std::filesystem::exists(src, ec) || !std::filesystem::is_regular_file(src, ec)) return;
    std::filesystem::path hist = src.parent_path() / ".xmt_history";
    std::filesystem::create_directories(hist, ec);
    std::time_t now = std::time(nullptr);
    char stamp[32];
    std::tm tm{};
#if defined(_WIN32)
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tm);
    std::filesystem::path dst = hist / (src.stem().string() + "_" + stamp + ".xmt");
    std::filesystem::copy_file(src, dst, std::filesystem::copy_options::overwrite_existing, ec);

    std::vector<std::filesystem::directory_entry> files;
    for (auto it = std::filesystem::directory_iterator(hist, ec); !ec && it != std::filesystem::directory_iterator(); ++it) {
        if (it->is_regular_file(ec) && it->path().extension() == ".xmt" &&
            it->path().stem().string().rfind(src.stem().string() + "_", 0) == 0) {
            files.push_back(*it);
        }
    }
    std::sort(files.begin(), files.end(), [](const auto &a, const auto &b) {
        std::error_code ea, eb;
        return std::filesystem::last_write_time(a, ea) >
               std::filesystem::last_write_time(b, eb);
    });
    for (size_t i = 20; i < files.size(); ++i) std::filesystem::remove(files[i].path(), ec);
}

static const std::vector<std::filesystem::path> &TasMovieHistoryFiles()
{
    /* The history window can stay open for minutes; do not rescan/sort the
     * directory on every ImGui frame. A one-second metadata cache is ample. */
    static std::string cached_movie_path;
    static std::vector<std::filesystem::path> cached;
    static std::chrono::steady_clock::time_point refresh_at{};
    const auto now = std::chrono::steady_clock::now();
    if (cached_movie_path == g_tas_movie_path && now < refresh_at) {
        return cached;
    }
    cached_movie_path = g_tas_movie_path;
    refresh_at = now + std::chrono::seconds(1);

    cached.clear();
    if (g_tas_movie_path.empty()) {
        return cached;
    }
    std::error_code ec;
    std::filesystem::path src(g_tas_movie_path);
    std::filesystem::path hist = src.parent_path() / ".xmt_history";
    if (!std::filesystem::exists(hist, ec)) {
        return cached;
    }
    for (auto it = std::filesystem::directory_iterator(hist, ec);
         !ec && it != std::filesystem::directory_iterator(); ++it) {
        if (it->is_regular_file(ec) && it->path().extension() == ".xmt" &&
            it->path().stem().string().rfind(src.stem().string() + "_", 0) == 0) {
            cached.push_back(it->path());
        }
    }
    std::sort(cached.begin(), cached.end(), [](const auto &a, const auto &b) {
        std::error_code ea, eb;
        return std::filesystem::last_write_time(a, ea) >
               std::filesystem::last_write_time(b, eb);
    });
    return cached;
}

static void TasRecoverAutosave()
{
    std::string path = TasRecoveryPath();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        xemu_queue_error_message("No TAS recovery autosave exists for the current movie/title");
        return;
    }
    if (TasLoadMovieFromPath(path.c_str())) {
        xemu_queue_notification("Recovered TAS autosave");
        g_tas_movie_dirty = true;
    }
}


static void TasStoreCurrentBranch(uint64_t fork_frame, const char *reason)
{
    TasBranch archived;
    archived.id = g_tas_current_branch;
    archived.parent = g_tas_current_parent;
    archived.fork_frame = fork_frame;
    archived.name = reason ? reason : "Preserved branch";
    archived.frames = std::move(g_tas_frames);
    archived.lag = std::move(g_tas_lag_flags);
    archived.polls = std::move(g_tas_poll_counts);
    g_tas_branches.push_back(std::move(archived));
}

static void TasArchiveCurrentBranch(uint64_t fork_boundary, const char *reason)
{
    TasBranch archived;
    archived.id = g_tas_current_branch;
    archived.parent = g_tas_current_parent;
    archived.fork_frame = fork_boundary;
    archived.name = reason ? reason : "Preserved future";

    /* Transfer ownership of the full old branch instead of copying it, then
     * copy back only the prefix the new branch actually needs. This turns a
     * potentially huge full-movie duplicate into O(prefix) work. */
    archived.frames = std::move(g_tas_frames);
    archived.lag = std::move(g_tas_lag_flags);
    archived.polls = std::move(g_tas_poll_counts);
    /* fork_boundary is a machine boundary: inputs [0, boundary) have already
     * executed and row boundary is the first input owned by the new branch. */
    const size_t keep = std::min<size_t>((size_t)fork_boundary,
                                         archived.frames.size());
    g_tas_frames.assign(archived.frames.begin(), archived.frames.begin() + keep);
    g_tas_lag_flags.assign(archived.lag.begin(), archived.lag.begin() +
                          std::min(keep, archived.lag.size()));
    g_tas_poll_counts.assign(archived.polls.begin(), archived.polls.begin() +
                             std::min(keep, archived.polls.size()));
    if (g_tas_frames.empty()) g_tas_frames.resize(1);
    if (g_tas_lag_flags.size() < g_tas_frames.size()) {
        g_tas_lag_flags.resize(g_tas_frames.size(), 0);
    }
    if (g_tas_poll_counts.size() < g_tas_frames.size()) {
        g_tas_poll_counts.resize(g_tas_frames.size(), TasUnknownPollCounts());
    }
    g_tas_branches.push_back(std::move(archived));

    g_tas_current_parent = g_tas_current_branch;
    g_tas_current_branch = g_tas_next_branch_id++;
    g_tas_visible_frame_cache_dirty = true;
}

static void TasResumeMovieAtFrame(uint64_t boundary, bool resume_vm)
{
    TasCancelPendingTransportAdvance();
    if (!xemu_tas_enabled()) xemu_tas_set_enabled(true);
    if (g_tas_frames.empty()) {
        g_tas_frames.resize(1);
        g_tas_lag_flags.resize(1, 0);
        g_tas_poll_counts.resize(1, TasUnknownPollCounts());
    }
    boundary = std::min<uint64_t>(boundary, g_tas_frames.size());

    if (!g_tas_read_only && boundary < g_tas_frames.size()) {
        TasArchiveCurrentBranch(boundary, "Preserved future before rerecord");
        /* TasArchiveCurrentBranch keeps exactly [0,boundary). Keep a single
         * placeholder only for an otherwise empty editor; it is replaced by
         * the first newly recorded frame. */
        if (boundary == 0) {
            g_tas_frames.assign(1, TasFrame{});
            g_tas_lag_flags.assign(1, 0);
            g_tas_poll_counts.assign(1, TasUnknownPollCounts());
            g_tas_record_replace_placeholder = true;
        }
        ++g_tas_rerecord_count;
    }

    const uint64_t selected = g_tas_frames.empty() ? 0 :
        std::min<uint64_t>(boundary, g_tas_frames.size() - 1);
    g_tas_selected_frame = (int)std::min<uint64_t>(selected, INT_MAX);
    xemu_tas_set_frame(boundary);

    if (g_tas_read_only) {
        xemu_tas_set_playback_movie_ex(g_tas_frames.data(), g_tas_lag_flags.data(),
                                       reinterpret_cast<const uint32_t *>(g_tas_poll_counts.data()),
                                       g_tas_poll_counts.size() * XEMU_TAS_MAX_PORTS,
                                       g_tas_frames.size());
        if (boundary < g_tas_frames.size()) {
            xemu_tas_start_playback(boundary);
        } else {
            xemu_tas_stop_playback();
        }
    } else {
        xemu_tas_clear_recording();
        g_tas_record_synced = 0;
        if (boundary != 0) g_tas_record_replace_placeholder = false;
        xemu_tas_start_recording(true);
    }
    if (resume_vm && !runstate_is_running()) {
        vm_start();
    }
}

static std::string TasStateBundlePath(const char *snapshot_name)
{
    std::filesystem::path dir(TasDefaultMovieDirectory());
    dir /= ".tas_states";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::filesystem::path p = dir / (std::string(snapshot_name ? snapshot_name : "state") + ".xmt");
    return p.string();
}

static uint32_t TasOverdubFieldMask()
{
    uint32_t mask = 0;
    for (unsigned i = 0; i < g_tas_overdub_fields.size(); ++i) {
        if (g_tas_overdub_fields[i]) mask |= 1u << i;
    }
    return mask;
}

static TasStateBundleRuntime TasCaptureRuntimeBundle(
    const XemuTasTransactionId &transaction, bool vm_was_running)
{
    TasStateBundleRuntime bundle;
    bundle.valid = true;
    bundle.transaction = transaction;
    bundle.boundary = xemu_tas_frame();
    bundle.vm_was_running = vm_was_running;
    bundle.selected_frame = g_tas_selected_frame;
    bundle.selection_anchor = g_tas_selection_anchor;
    bundle.selection_end = g_tas_selection_end;
    bundle.follow = g_tas_follow_frame;
    bundle.read_only = g_tas_read_only;
    bundle.branch_id = g_tas_current_branch;
    bundle.movie_revision = g_tas_movie_revision;
    bundle.overdub_start = g_tas_overdub_start_frame;
    bundle.overdub_synced = g_tas_overdub_synced;
    bundle.overdub_port = (uint32_t)std::clamp(g_tas_port, 0, XEMU_TAS_MAX_PORTS - 1);
    bundle.overdub_field_mask = TasOverdubFieldMask();
    bundle.owner_movie_path = g_tas_movie_path;
    bundle.last_frame_lagged = xemu_tas_last_frame_lagged();
    bundle.lag_count = xemu_tas_lag_count();
    bundle.lag_streak = xemu_tas_lag_streak();

    if (g_tas_overdub_ui_active || xemu_tas_overdub()) {
        bundle.mode = TasRuntimeMode::Overdub;
    } else if (xemu_tas_recording()) {
        bundle.mode = TasRuntimeMode::Recording;
    } else if (xemu_tas_playback()) {
        bundle.mode = TasRuntimeMode::Playback;
    } else {
        bundle.mode = TasRuntimeMode::Idle;
    }
    if (!TasComputeStateHash(&bundle.state_hash)) {
        bundle.valid = false;
    }
    return bundle;
}

static bool TasSavePairedSnapshot(const char *snapshot_name,
                                  const std::string &bundle_path,
                                  TasStateBundleRuntime *saved_bundle)
{
    if (runstate_is_running()) {
        xemu_queue_error_message("Internal TAS error: paired snapshot requested while VM is running");
        return false;
    }

    /* Flush every completed recording/overdub frame into the movie half while
     * the VM is frozen, but do not dismantle the active runtime mode. */
    TasSyncRecordingFromCore();

    XemuTasTransactionId transaction{};
    xemu_tas_transaction_mint(&transaction);
    TasStateBundleRuntime bundle = TasCaptureRuntimeBundle(transaction, false);
    if (!bundle.valid) {
        xemu_queue_error_message("Could not fingerprint TAS paired state; snapshot was not saved");
        return false;
    }

    if (!TasSaveMovieToPathInternal(bundle_path.c_str(), false, false, &bundle)) {
        xemu_queue_error_message("Could not save the TAS movie/runtime half of the paired state");
        return false;
    }

    Error *err = NULL;
    xemu_tas_transaction_snapshot_begin();
    xemu_snapshots_save_no_thumbnail(snapshot_name, &err);
    xemu_tas_transaction_snapshot_end();
    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_free(err);
        std::error_code ec;
        std::filesystem::remove(bundle_path, ec);
        return false;
    }
    TasSnapshotCacheInsert(snapshot_name);
    if (saved_bundle) *saved_bundle = std::move(bundle);
    return true;
}

static bool TasRestoreRuntimeBundle(const TasStateBundleRuntime &bundle)
{
    if (!bundle.valid || bundle.boundary > g_tas_frames.size() ||
        bundle.branch_id != g_tas_current_branch ||
        bundle.movie_revision != g_tas_movie_revision) {
        return false;
    }

    xemu_tas_stop_overdub();
    xemu_tas_stop_recording();
    xemu_tas_stop_playback();
    xemu_tas_set_frame(bundle.boundary);
    xemu_tas_set_lag_state(bundle.last_frame_lagged, bundle.lag_count,
                           bundle.lag_streak);

    if (bundle.state_hash) {
        uint64_t actual = 0;
        if (!TasComputeStateHash(&actual) || actual != bundle.state_hash) {
            return false;
        }
    }

    const int last = std::max(0, (int)g_tas_frames.size() - 1);
    g_tas_selected_frame = std::clamp(bundle.selected_frame, 0, last);
    g_tas_selection_anchor = std::clamp(bundle.selection_anchor, 0, last);
    g_tas_selection_end = std::clamp(bundle.selection_end, 0, last);
    g_tas_follow_frame = bundle.follow;
    g_tas_read_only = bundle.read_only;
    g_tas_port = std::clamp((int)bundle.overdub_port, 0, XEMU_TAS_MAX_PORTS - 1);
    g_tas_overdub_start_frame = bundle.overdub_start;
    g_tas_overdub_synced = bundle.overdub_synced;
    for (unsigned i = 0; i < g_tas_overdub_fields.size(); ++i) {
        g_tas_overdub_fields[i] = (bundle.overdub_field_mask & (1u << i)) != 0;
    }
    if (!bundle.owner_movie_path.empty()) g_tas_movie_path = bundle.owner_movie_path;

    TasPushMovieToCore();
    g_tas_record_synced = 0;
    g_tas_record_replace_placeholder = false;
    g_tas_overdub_ui_active = false;
    g_tas_power_on_reset_pending = false;
    g_tas_power_on_reset_deadline = {};
    xemu_tas_cancel_post_reset_pause();

    switch (bundle.mode) {
    case TasRuntimeMode::Idle:
        break;
    case TasRuntimeMode::Playback:
        if (bundle.boundary < g_tas_frames.size() &&
            !xemu_tas_start_playback(bundle.boundary)) {
            return false;
        }
        break;
    case TasRuntimeMode::Recording:
        if (bundle.boundary == 0 && g_tas_frames.size() == 1) {
            g_tas_record_replace_placeholder = true;
        } else if (bundle.boundary != g_tas_frames.size()) {
            /* A recording transaction cannot own unexecuted future rows. */
            return false;
        }
        xemu_tas_start_recording(true);
        break;
    case TasRuntimeMode::Overdub: {
        if (bundle.boundary >= g_tas_frames.size()) return false;
        uint16_t digital_mask = 0;
        uint8_t analog_mask = 0, stick_mask = 0;
        for (int i = 0; i < 8; ++i) if (g_tas_overdub_fields[i]) digital_mask |= (uint16_t)(1u << i);
        for (int i = 0; i < 8; ++i) if (g_tas_overdub_fields[8 + i]) analog_mask |= (uint8_t)(1u << i);
        for (int i = 0; i < 4; ++i) if (g_tas_overdub_fields[16 + i]) stick_mask |= (uint8_t)(1u << i);
        g_tas_overdub_start_frame = bundle.boundary;
        g_tas_overdub_synced = 0;
        g_tas_overdub_ui_active = xemu_tas_start_overdub(
            bundle.boundary, (uint8_t)g_tas_port, digital_mask,
            analog_mask, stick_mask);
        if (!g_tas_overdub_ui_active) return false;
        break;
    }
    }

    if (bundle.vm_was_running && !runstate_is_running()) vm_start();
    return true;
}

static bool TasLoadPairedSnapshot(const char *snapshot_name,
                                  const std::string &bundle_path)
{
    std::error_code ec;
    if (!std::filesystem::exists(bundle_path, ec)) {
        xemu_queue_error_message("TAS paired movie/runtime bundle is missing");
        return false;
    }

    xemu_tas_prepare_runtime();
    Error *err = NULL;
    bool ignored_running = false;
    const bool loaded = xemu_snapshots_load_paused(snapshot_name, &ignored_running, &err);
    if (err || !loaded) {
        if (err) {
            xemu_queue_error_message(error_get_pretty(err));
            error_free(err);
        } else {
            xemu_queue_error_message("TAS native snapshot load failed");
        }
        return false;
    }

    g_tas_silent_bundle_load = true;
    const bool movie_loaded = TasLoadMovieFromPath(bundle_path.c_str());
    g_tas_silent_bundle_load = false;
    if (!movie_loaded || !g_tas_loaded_state_bundle.valid) {
        xemu_queue_error_message("TAS paired movie/runtime bundle is invalid");
        return false;
    }
    const TasStateBundleRuntime bundle = g_tas_loaded_state_bundle;
    if (!xemu_tas_transaction_matches(&bundle.transaction)) {
        xemu_queue_error_message("TAS snapshot/movie transaction ID mismatch; pair rejected");
        return false;
    }
    if (!TasRestoreRuntimeBundle(bundle)) {
        xemu_queue_error_message("TAS paired state failed runtime/state-hash validation");
        return false;
    }
    return true;
}

static void TasSaveSelectedState()
{
    char state_name[64];
    if (!TasBuildStateName(state_name, sizeof(state_name))) {
        xemu_queue_error_message("TAS state: no running XBE/title ID is available yet");
        return;
    }

    const bool was_running = runstate_is_running();
    if (was_running) vm_stop(RUN_STATE_PAUSED);

    TasStateBundleRuntime bundle;
    const std::string bundle_path = TasStateBundlePath(state_name);
    if (!TasSavePairedSnapshot(state_name, bundle_path, &bundle)) {
        if (was_running && !runstate_is_running()) vm_start();
        return;
    }
    bundle.vm_was_running = was_running;
    /* Rewrite only the small movie sidecar so its runtime header records the
     * pre-save runstate. Native VM state is unchanged while paused. The pair
     * is not committed unless this second half succeeds. */
    if (!TasSaveMovieToPathInternal(bundle_path.c_str(), false, false, &bundle)) {
        std::error_code ec;
        std::filesystem::remove(bundle_path, ec);
        if (g_tas_snapshot_delete_queued.emplace(state_name).second) {
            g_tas_snapshot_delete_queue.push_back(state_name);
        }
        if (was_running && !runstate_is_running()) vm_start();
        xemu_queue_error_message("Could not finalize TAS state movie/runtime bundle; state was discarded");
        return;
    }

    TasSnapshotCacheInsert(state_name);
    uint32_t saved_title_id = 0;
    if (TasGetCurrentTitleId(&saved_title_id) &&
        g_tas_state_slot_cache_valid && g_tas_state_slot_cache_title == saved_title_id) {
        g_tas_state_slot_cache[(size_t)g_tas_state_slot] = true;
    }
    g_tas_state_meta[g_tas_state_slot].valid = true;
    g_tas_state_meta[g_tas_state_slot].frame = bundle.boundary;
    g_tas_state_meta[g_tas_state_slot].branch_id = g_tas_current_branch;
    g_tas_state_meta[g_tas_state_slot].transaction = bundle.transaction;
    TasAutosaveRecovery(true);

    if (was_running && !runstate_is_running()) vm_start();

    char msg[160];
    snprintf(msg, sizeof(msg), "Saved TAS state %02d (%s) at frame %llu",
             g_tas_state_slot, state_name,
             (unsigned long long)g_tas_state_meta[g_tas_state_slot].frame);
    xemu_queue_notification(msg);
}

static void TasLoadSelectedState()
{
    TasCancelPendingTransportAdvance();
    char state_name[64];
    if (!TasBuildStateName(state_name, sizeof(state_name))) {
        xemu_queue_error_message("TAS state: no running XBE/title ID is available yet");
        return;
    }
    if (!TasSnapshotExists(state_name)) {
        char msg[128];
        snprintf(msg, sizeof(msg), "TAS state slot %02d is empty for this game",
                 g_tas_state_slot);
        xemu_queue_error_message(msg);
        return;
    }

    uint32_t title_id = 0;
    TasGetCurrentTitleId(&title_id);
    char undo_name[64];
    snprintf(undo_name, sizeof(undo_name), "%08X_TAS_UNDO_LOAD", title_id);
    const bool was_running = runstate_is_running();
    if (was_running) vm_stop(RUN_STATE_PAUSED);
    TasSyncRecordingFromCore();
    TasAutosaveRecovery(true);

    /* Undo Load is itself a complete pair. If loading the target fails after
     * touching either half, immediately roll back through this pair. */
    const std::string undo_bundle_path = TasStateBundlePath(undo_name);
    TasStateBundleRuntime undo_bundle;
    if (!TasSavePairedSnapshot(undo_name, undo_bundle_path, &undo_bundle)) {
        if (was_running && !runstate_is_running()) vm_start();
        xemu_queue_error_message("Could not create transactional Undo Load guard");
        return;
    }
    undo_bundle.vm_was_running = was_running;
    if (!TasSaveMovieToPathInternal(undo_bundle_path.c_str(), false, false,
                                    &undo_bundle)) {
        std::error_code ec;
        std::filesystem::remove(undo_bundle_path, ec);
        if (g_tas_snapshot_delete_queued.emplace(undo_name).second) {
            g_tas_snapshot_delete_queue.push_back(undo_name);
        }
        if (was_running && !runstate_is_running()) vm_start();
        xemu_queue_error_message("Could not finalize transactional Undo Load guard");
        return;
    }
    g_tas_undo_snapshot_name = undo_name;
    g_tas_undo_transaction = undo_bundle.transaction;

    if (!TasLoadPairedSnapshot(state_name, TasStateBundlePath(state_name))) {
        TasLoadPairedSnapshot(undo_name, undo_bundle_path);
        return;
    }
    xemu_queue_notification("Loaded transactional TAS state");
}

static void TasUndoStateLoad()
{
    TasCancelPendingTransportAdvance();
    if (g_tas_undo_snapshot_name.empty() || !TasSnapshotExists(g_tas_undo_snapshot_name.c_str())) {
        xemu_queue_error_message("No TAS savestate load to undo");
        return;
    }
    const std::string bundle_path = TasStateBundlePath(g_tas_undo_snapshot_name.c_str());
    if (!TasLoadPairedSnapshot(g_tas_undo_snapshot_name.c_str(), bundle_path)) {
        xemu_queue_error_message("Could not restore transactional Undo Load pair");
        return;
    }
    xemu_queue_notification("Undid last TAS savestate load");
}

static void TasGetStateSlotStatus(std::array<bool, 100> &occupied)
{
    uint32_t title_id = 0;
    if (!TasGetCurrentTitleId(&title_id)) {
        occupied.fill(false);
        return;
    }

    /* Slot occupancy changes only when snapshots/title change. Do not enumerate
     * the QCOW2 snapshot table once per second simply because TAS Studio is
     * visible. */
    if (g_tas_state_slot_cache_valid && g_tas_state_slot_cache_title == title_id) {
        occupied = g_tas_state_slot_cache;
        return;
    }

    g_tas_state_slot_cache.fill(false);
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%08X_TAS_", title_id);
    const size_t prefix_len = strlen(prefix);

    TasRefreshSnapshotCache(true);
    for (const std::string &entry : g_tas_snapshot_name_cache) {
        const char *name = entry.c_str();
        if (strncmp(name, prefix, prefix_len) != 0) {
            continue;
        }
        const char *slot_text = name + prefix_len;
        if (strlen(slot_text) != 2 || slot_text[0] < '0' || slot_text[0] > '9' ||
            slot_text[1] < '0' || slot_text[1] > '9') {
            continue;
        }
        const int slot = (slot_text[0] - '0') * 10 + (slot_text[1] - '0');
        if (slot >= 0 && slot < 100) {
            g_tas_state_slot_cache[(size_t)slot] = true;
        }
    }
    g_tas_state_slot_cache_title = title_id;
    g_tas_state_slot_cache_valid = true;
    occupied = g_tas_state_slot_cache;
}

static void TasClearRewindCheckpoints()
{
    /* Rewind states are only valid for the exact movie/runtime session that
     * produced them. Drop the in-memory index immediately when that baseline
     * changes and retire the native snapshots lazily while the VM is paused. */
    for (auto &cp : g_tas_rewind_points) {
        if (cp.valid && !cp.snapshot_name.empty() &&
            g_tas_snapshot_delete_queued.emplace(cp.snapshot_name).second) {
            g_tas_snapshot_delete_queue.push_back(cp.snapshot_name);
        }
        cp = {};
    }
    g_tas_rewind_slot = 0;
    g_tas_rewind_next_frame = 0;
    TasCancelPendingTransportAdvance();
}

static void TasResetMovieMetadata()
{
    g_tas_lag_flags.assign(1, 0);
    g_tas_state_meta = {};
    g_tas_chapters.clear();
    g_tas_branches.clear();
    g_tas_current_branch = 0;
    g_tas_current_parent = UINT32_MAX;
    g_tas_next_branch_id = 1;
    g_tas_rerecord_count = 0;
    g_tas_record_synced = 0;
    g_tas_last_autosave_record_count = 0;
    g_tas_next_autosave_host = {};
    TasClearRewindCheckpoints();
    g_tas_markers.clear();
    g_tas_properties = {};
    g_tas_loaded_environment = {};
    g_tas_verify_baseline.clear();
    g_tas_verify_mode = TasVerifyMode::Idle;
    g_tas_verify_status = "Not verified";
    g_tas_verify_failed = false;
    g_tas_verify_start_snapshot.clear();
    g_tas_verify_branch = 0;
    g_tas_movie_start_snapshot.clear();
    g_tas_movie_start_state_hash = 0;
    g_tas_movie_start_legacy_ram_hash = 0;
    g_tas_movie_start_transaction = {};
    g_tas_poll_counts.assign(1, TasUnknownPollCounts());
    g_tas_verify_revision = UINT64_MAX;
    g_tas_verify_start_transaction = {};
    g_tas_verify_baseline_interval = std::max(1, g_tas_verify_interval);
    g_tas_verify_runs_total = 1;
    g_tas_verify_runs_remaining = 0;
    g_tas_verify_run_index = 0;
    g_tas_verify_exhaustive = false;
    g_tas_verify_poll_sync_from = 0;
    g_tas_timeline_mutation = {};
    g_tas_branch_switch = {};
    g_tas_loaded_state_bundle = {};
    g_tas_undo_transaction = {};
    g_tas_movie_revision = 0;
    g_tas_movie_dirty = false;
    g_tas_core_pushed_revision = UINT64_MAX;
    g_tas_core_pushed_frame_count = 0;
    g_tas_visible_frame_cache.clear();
    g_tas_visible_frame_cache_dirty = true;
    g_tas_undo_stack.clear();
    g_tas_redo_stack.clear();
    g_tas_selection_anchor = 0;
    g_tas_selection_end = 0;
    g_tas_clipboard_frames.clear();
    g_tas_clipboard_lag.clear();
    g_tas_clipboard_polls.clear();
    g_tas_overdub_ui_active = false;
    g_tas_power_on_reset_pending = false;
    xemu_tas_cancel_post_reset_pause();
    g_tas_power_on_reset_deadline = {};
    g_tas_pending_overdub_start = {};
    g_tas_strict_resim_pending = false;
    g_tas_strict_resim_target = 0;
    g_tas_strict_resim_continue = false;
    g_tas_compare_frames.clear();
    g_tas_compare_lag.clear();
}

static void TasNewMovie()
{
    xemu_tas_stop_recording();
    xemu_tas_stop_playback();
    g_tas_frames.assign(1, TasFrame{});
    TasResetMovieMetadata();
    g_tas_selected_frame = 0;
    g_tas_clipboard_valid = false;
    g_tas_movie_path.clear();
    g_tas_follow_frame = true;
    xemu_tas_clear_all_xid_reports();
    xemu_tas_set_deterministic_mode(true);
    xemu_queue_notification("Created new TAS movie (Strict Sync enabled; power-on recording is the default)");
}

static bool TasWriteU8(FILE *f, uint8_t v)
{
    return fwrite(&v, 1, 1, f) == 1;
}

static bool TasWriteU32(FILE *f, uint32_t v)
{
    uint8_t b[4] = {
        (uint8_t)(v), (uint8_t)(v >> 8), (uint8_t)(v >> 16), (uint8_t)(v >> 24)
    };
    return fwrite(b, 1, sizeof(b), f) == sizeof(b);
}

static bool TasWriteU64(FILE *f, uint64_t v)
{
    uint8_t b[8];
    for (int i = 0; i < 8; ++i) b[i] = (uint8_t)(v >> (8 * i));
    return fwrite(b, 1, sizeof(b), f) == sizeof(b);
}

static bool TasReadU8(FILE *f, uint8_t *v)
{
    return fread(v, 1, 1, f) == 1;
}

static bool TasReadU32(FILE *f, uint32_t *v)
{
    uint8_t b[4];
    if (fread(b, 1, sizeof(b), f) != sizeof(b)) return false;
    *v = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
         ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

static bool TasReadU64(FILE *f, uint64_t *v)
{
    uint8_t b[8];
    if (fread(b, 1, sizeof(b), f) != sizeof(b)) return false;
    uint64_t out = 0;
    for (int i = 0; i < 8; ++i) out |= (uint64_t)b[i] << (8 * i);
    *v = out;
    return true;
}

struct TasMd5CacheEntry {
    uintmax_t size = 0;
    std::filesystem::file_time_type mtime{};
    std::string hash;
};

static std::map<std::string, TasMd5CacheEntry> g_tas_md5_cache;

static std::string TasFileMD5(const char *path)
{
    if (!path || !*path) return {};
    std::error_code ec;
    const std::filesystem::path p(path);
    const uintmax_t size = std::filesystem::file_size(p, ec);
    if (ec) return {};
    const auto mtime = std::filesystem::last_write_time(p, ec);
    if (ec) return {};

    auto it = g_tas_md5_cache.find(path);
    if (it != g_tas_md5_cache.end() && it->second.size == size &&
        it->second.mtime == mtime) {
        return it->second.hash;
    }

    gchar *sum = GetFileMD5Checksum(path);
    std::string out = sum ? sum : "";
    if (sum) g_free(sum);
    g_tas_md5_cache[std::string(path)] = TasMd5CacheEntry{size, mtime, out};
    return out;
}

static bool TasWriteString(FILE *f, const std::string &s)
{
    if (s.size() > UINT32_MAX) return false;
    return TasWriteU32(f, (uint32_t)s.size()) &&
           (s.empty() || fwrite(s.data(), 1, s.size(), f) == s.size());
}

static bool TasReadString(FILE *f, std::string *s, uint32_t max_len = 1 << 20)
{
    uint32_t len = 0;
    if (!TasReadU32(f, &len) || len > max_len) return false;
    s->assign(len, '\0');
    return !len || fread(s->data(), 1, len, f) == len;
}

static std::string TasEnsureXmtExtension(const char *path)
{
    std::filesystem::path p(path ? path : "");
    if (p.extension() != ".xmt" && p.extension() != ".XMT") {
        p += ".xmt";
    }
    return p.string();
}

static bool TasWriteFrames(FILE *f, const std::vector<TasFrame> &frames,
                           const std::vector<uint8_t> &lag)
{
    if (!TasWriteU64(f, frames.size())) return false;
    for (size_t i = 0; i < frames.size(); ++i) {
        for (int port = 0; port < XEMU_TAS_MAX_PORTS; ++port) {
            if (fwrite(frames[i].xid[port].data(), 1, XEMU_TAS_XID_REPORT_SIZE, f) !=
                XEMU_TAS_XID_REPORT_SIZE) return false;
        }
        uint8_t l = i < lag.size() ? lag[i] : 0;
        if (!TasWriteU8(f, l)) return false;
    }
    return true;
}

static bool TasReadFrames(FILE *f, std::vector<TasFrame> *frames,
                          std::vector<uint8_t> *lag)
{
    uint64_t count = 0;
    if (!TasReadU64(f, &count) || count == 0 || count > 10000000ULL) return false;
    frames->assign((size_t)count, TasFrame{});
    lag->assign((size_t)count, 0);
    for (size_t i = 0; i < (size_t)count; ++i) {
        for (int port = 0; port < XEMU_TAS_MAX_PORTS; ++port) {
            if (fread((*frames)[i].xid[port].data(), 1, XEMU_TAS_XID_REPORT_SIZE, f) !=
                XEMU_TAS_XID_REPORT_SIZE) return false;
        }
        if (!TasReadU8(f, &(*lag)[i])) return false;
    }
    return true;
}

static bool TasWritePollCounts(FILE *f, const std::vector<TasPollCounts> &polls,
                               size_t frame_count)
{
    if (!TasWriteU64(f, frame_count)) return false;
    const TasPollCounts unknown = TasUnknownPollCounts();
    for (size_t i = 0; i < frame_count; ++i) {
        const TasPollCounts &p = i < polls.size() ? polls[i] : unknown;
        for (uint32_t count : p) {
            if (!TasWriteU32(f, count)) return false;
        }
    }
    return true;
}

static bool TasReadPollCounts(FILE *f, std::vector<TasPollCounts> *polls,
                              size_t expected_frames)
{
    uint64_t count = 0;
    if (!TasReadU64(f, &count) || count != expected_frames ||
        count > 10000000ULL) {
        return false;
    }
    polls->assign((size_t)count, TasUnknownPollCounts());
    for (size_t i = 0; i < (size_t)count; ++i) {
        for (uint32_t &value : (*polls)[i]) {
            if (!TasReadU32(f, &value)) return false;
        }
    }
    return true;
}

static bool TasSaveMovieToPathInternal(const char *path, bool set_current_path,
                                       bool notify,
                                       const TasStateBundleRuntime *state_bundle)
{
    if (!path || !*path) return false;
    std::string final_path = TasEnsureXmtExtension(path);
    std::filesystem::path fp(final_path);
    std::error_code ec;
    if (fp.has_parent_path()) std::filesystem::create_directories(fp.parent_path(), ec);
    if (notify) TasBackupExistingMovie(final_path);

    FILE *f = qemu_fopen(final_path.c_str(), "wb");
    if (!f) {
        if (notify) {
            char msg[512];
            snprintf(msg, sizeof(msg), "Could not save TAS movie: %s", strerror(errno));
            xemu_queue_error_message(msg);
        }
        return false;
    }
    /* Large sequential movie writes benefit from a larger stdio buffer,
     * especially periodic recovery saves on long TASes. */
    setvbuf(f, NULL, _IOFBF, 1024 * 1024);

    uint32_t title_id = 0;
    TasGetCurrentTitleId(&title_id);
    uint32_t flags = 0;
    if (xemu_tas_deterministic_mode()) flags |= 1u;
    if (g_tas_power_on_recording) flags |= 2u;
    if (g_tas_read_only) flags |= 4u;

    char *disc_path_c = xemu_get_currently_loaded_disc_path();
    std::string disc_path = disc_path_c ? disc_path_c : "";
    if (disc_path_c) g_free(disc_path_c);

    std::string bootrom_md5 = TasFileMD5(g_config.sys.files.bootrom_path);
    std::string flashrom_md5 = TasFileMD5(g_config.sys.files.flashrom_path);
    std::string eeprom_md5 = TasFileMD5(g_config.sys.files.eeprom_path);

    bool ok = fwrite("XMT2", 1, 4, f) == 4 &&
              TasWriteU32(f, 2) && TasWriteU32(f, title_id) &&
              TasWriteU32(f, flags) && TasWriteU32(f, XEMU_TAS_MAX_PORTS) &&
              TasWriteU32(f, XEMU_TAS_XID_REPORT_SIZE) &&
              TasWriteU64(f, g_tas_rerecord_count) &&
              TasWriteU32(f, g_tas_current_branch) &&
              TasWriteU32(f, g_tas_current_parent) &&
              TasWriteU32(f, g_tas_next_branch_id) &&
              TasWriteString(f, xemu_version ? xemu_version : "") &&
              TasWriteString(f, xemu_commit ? xemu_commit : "") &&
              TasWriteString(f, disc_path) &&
              TasWriteString(f, bootrom_md5) &&
              TasWriteString(f, flashrom_md5) &&
              TasWriteString(f, eeprom_md5) &&
              TasWriteString(f, g_config.sys.files.hdd_path ? g_config.sys.files.hdd_path : "") &&
              TasWriteU32(f, (uint32_t)g_config.display.renderer) &&
              TasWriteU32(f, (uint32_t)nv2a_get_surface_scale_factor()) &&
              TasWriteU32(f, (uint32_t)g_config.display.ui.fit) &&
              TasWriteU32(f, (uint32_t)g_config.display.filtering) &&
              TasWriteU32(f, (uint32_t)g_config.display.ui.aspect_ratio) &&
              TasWriteU32(f, (uint32_t)g_config.general.fast_forward_multiplier) &&
              TasWriteFrames(f, g_tas_frames, g_tas_lag_flags);

    for (int i = 0; ok && i < 100; ++i) {
        ok = TasWriteU8(f, g_tas_state_meta[i].valid ? 1 : 0) &&
             TasWriteU64(f, g_tas_state_meta[i].frame) &&
             TasWriteU32(f, g_tas_state_meta[i].branch_id);
    }

    if (ok) ok = TasWriteU32(f, (uint32_t)g_tas_chapters.size());
    for (const TasChapter &c : g_tas_chapters) {
        if (!ok) break;
        ok = TasWriteU64(f, c.frame) && TasWriteString(f, c.name);
    }

    if (ok) ok = TasWriteU32(f, (uint32_t)g_tas_branches.size());
    for (const TasBranch &b : g_tas_branches) {
        if (!ok) break;
        ok = TasWriteU32(f, b.id) && TasWriteU32(f, b.parent) &&
             TasWriteU64(f, b.fork_frame) && TasWriteString(f, b.name) &&
             TasWriteFrames(f, b.frames, b.lag);
    }

    /* Backward-compatible XMT2 extension trailer. Older XMT2 readers stop
     * after branches and harmlessly ignore these bytes. */
    if (ok) {
        ok = fwrite("XEX3", 1, 4, f) == 4 && TasWriteU32(f, 1) &&
             TasWriteU64(f, g_tas_movie_revision) &&
             TasWriteString(f, g_tas_properties.author) &&
             TasWriteString(f, g_tas_properties.category) &&
             TasWriteString(f, g_tas_properties.game_version) &&
             TasWriteString(f, g_tas_properties.comments) &&
             TasWriteU32(f, (uint32_t)g_tas_markers.size());
    }
    for (const TasMarker &m : g_tas_markers) {
        if (!ok) break;
        ok = TasWriteU64(f, m.frame) && TasWriteString(f, m.name) &&
             TasWriteString(f, m.note);
    }
    if (ok) ok = TasWriteU32(f, (uint32_t)g_tas_verify_baseline.size());
    for (const TasHashRecord &h : g_tas_verify_baseline) {
        if (!ok) break;
        ok = TasWriteU64(f, h.frame) && TasWriteU64(f, h.hash);
    }
    /* Optional current-state start reference. Older XMT2 readers ignore bytes
     * after the verifier list, so the existing XEX3 version remains valid. */
    if (ok) {
        ok = fwrite("TSST", 1, 4, f) == 4 &&
             TasWriteString(f, g_tas_movie_start_snapshot) &&
             TasWriteU64(f, g_tas_movie_start_legacy_ram_hash);
    }
    /* Determinism-v2 canonical start ownership + strong state fingerprint. */
    if (ok) {
        ok = fwrite("TST2", 1, 4, f) == 4 && TasWriteU32(f, 1) &&
             TasWriteU64(f, g_tas_movie_start_transaction.hi) &&
             TasWriteU64(f, g_tas_movie_start_transaction.lo) &&
             TasWriteU64(f, g_tas_movie_start_state_hash);
    }
    /* Exact per-frame XID poll traces. Unknown entries are UINT32_MAX and are
     * intentionally not treated as proof until a recording/verifier pass
     * repopulates them. Branch traces travel with their branch ownership. */
    if (ok) {
        ok = fwrite("TPOL", 1, 4, f) == 4 && TasWriteU32(f, 1) &&
             TasWritePollCounts(f, g_tas_poll_counts, g_tas_frames.size()) &&
             TasWriteU32(f, (uint32_t)g_tas_branches.size());
    }
    for (const TasBranch &b : g_tas_branches) {
        if (!ok) break;
        ok = TasWriteU32(f, b.id) &&
             TasWritePollCounts(f, b.polls, b.frames.size());
    }
    /* Slot transaction identities bind each native snapshot to the movie
     * bundle that created it without changing the legacy fixed slot table. */
    if (ok) {
        ok = fwrite("TSM2", 1, 4, f) == 4 && TasWriteU32(f, 1);
    }
    for (const TasStateMeta &meta : g_tas_state_meta) {
        if (!ok) break;
        ok = TasWriteU64(f, meta.transaction.hi) &&
             TasWriteU64(f, meta.transaction.lo);
    }
    if (ok) {
        ok = fwrite("TVR2", 1, 4, f) == 4 && TasWriteU32(f, 2) &&
             TasWriteU64(f, g_tas_verify_revision) &&
             TasWriteU32(f, (uint32_t)std::max(1, g_tas_verify_interval)) &&
             TasWriteU32(f, g_tas_verify_branch) &&
             TasWriteU64(f, g_tas_verify_start_frame) &&
             TasWriteU64(f, g_tas_verify_start_transaction.hi) &&
             TasWriteU64(f, g_tas_verify_start_transaction.lo) &&
             TasWriteString(f, g_tas_verify_start_snapshot);
    }
    if (ok && state_bundle && state_bundle->valid) {
        ok = fwrite("TBND", 1, 4, f) == 4 && TasWriteU32(f, 2) &&
             TasWriteU64(f, state_bundle->transaction.hi) &&
             TasWriteU64(f, state_bundle->transaction.lo) &&
             TasWriteU64(f, state_bundle->boundary) &&
             TasWriteU64(f, state_bundle->state_hash) &&
             TasWriteU8(f, state_bundle->last_frame_lagged ? 1 : 0) &&
             TasWriteU64(f, state_bundle->lag_count) &&
             TasWriteU64(f, state_bundle->lag_streak) &&
             TasWriteU8(f, (uint8_t)state_bundle->mode) &&
             TasWriteU8(f, state_bundle->vm_was_running ? 1 : 0) &&
             TasWriteU32(f, (uint32_t)std::max(0, state_bundle->selected_frame)) &&
             TasWriteU32(f, (uint32_t)std::max(0, state_bundle->selection_anchor)) &&
             TasWriteU32(f, (uint32_t)std::max(0, state_bundle->selection_end)) &&
             TasWriteU8(f, state_bundle->follow ? 1 : 0) &&
             TasWriteU8(f, state_bundle->read_only ? 1 : 0) &&
             TasWriteU32(f, state_bundle->branch_id) &&
             TasWriteU64(f, state_bundle->movie_revision) &&
             TasWriteU64(f, state_bundle->overdub_start) &&
             TasWriteU64(f, state_bundle->overdub_synced) &&
             TasWriteU32(f, state_bundle->overdub_port) &&
             TasWriteU32(f, state_bundle->overdub_field_mask) &&
             TasWriteString(f, state_bundle->owner_movie_path);
    }

    if (fclose(f) != 0) ok = false;
    if (!ok) {
        if (notify) xemu_queue_error_message("Failed while writing TAS movie");
        return false;
    }

    if (set_current_path) g_tas_movie_path = final_path;
    if (notify) g_tas_movie_dirty = false;
    if (notify) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Saved TAS movie: %s", final_path.c_str());
        xemu_queue_notification(msg);
    }
    return true;
}

static bool TasSaveMovieToPath(const char *path)
{
    return TasSaveMovieToPathInternal(path, true, true);
}

static void TasAutosaveRecovery(bool force)
{
    /* Recovery cadence is based on HOST time, not guest/TAS frames. Otherwise
     * 5x/Unlimited Fast Forward would turn a nominal 30-second autosave into a
     * filesystem write every few real seconds. */
    constexpr auto kAutosavePeriod = std::chrono::seconds(30);
    const auto now = std::chrono::steady_clock::now();
    const uint64_t recorded = xemu_tas_recorded_frame_count();
    if (!force) {
        if (g_tas_next_autosave_host.time_since_epoch().count() != 0 &&
            now < g_tas_next_autosave_host) {
            return;
        }
        if (!g_tas_movie_dirty && recorded == g_tas_last_autosave_record_count) {
            g_tas_next_autosave_host = now + kAutosavePeriod;
            return;
        }
    }
    g_tas_next_autosave_host = now + kAutosavePeriod;
    g_tas_last_autosave_record_count = recorded;

    std::string path;
    if (!g_tas_movie_path.empty()) {
        path = g_tas_movie_path + ".autosave";
    } else {
        uint32_t title_id = 0;
        TasGetCurrentTitleId(&title_id);
        char name[64];
        snprintf(name, sizeof(name), "%08X_Untitled.autosave.xmt", title_id);
        path = (std::filesystem::path(TasDefaultMovieDirectory()) / name).string();
    }
    TasSaveMovieToPathInternal(path.c_str(), false, false);
}

static bool TasLoadXmt1(FILE *f, uint32_t title_id, uint32_t flags,
                        uint32_t ports, uint32_t report_size)
{
    uint64_t frame_count = 0;
    if (ports != XEMU_TAS_MAX_PORTS || report_size != XEMU_TAS_XID_REPORT_SIZE ||
        !TasReadU64(f, &frame_count) || !frame_count || frame_count > 10000000ULL) return false;
    std::vector<TasFrame> loaded((size_t)frame_count);
    for (TasFrame &frame : loaded) {
        for (int port = 0; port < XEMU_TAS_MAX_PORTS; ++port) {
            if (fread(frame.xid[port].data(), 1, XEMU_TAS_XID_REPORT_SIZE, f) !=
                XEMU_TAS_XID_REPORT_SIZE) return false;
        }
    }
    g_tas_frames = std::move(loaded);
    g_tas_lag_flags.assign(g_tas_frames.size(), 0);
    g_tas_poll_counts.assign(g_tas_frames.size(), TasUnknownPollCounts());
    g_tas_rerecord_count = 0;
    g_tas_state_meta = {};
    g_tas_chapters.clear();
    g_tas_branches.clear();
    g_tas_current_branch = 0;
    g_tas_current_parent = UINT32_MAX;
    g_tas_next_branch_id = 1;
    g_tas_markers.clear();
    g_tas_properties = {};
    g_tas_verify_baseline.clear();
    g_tas_verify_revision = UINT64_MAX;
    g_tas_verify_start_transaction = {};
    g_tas_movie_revision = 0;
    g_tas_movie_start_snapshot.clear();
    g_tas_movie_start_legacy_ram_hash = 0;
    g_tas_movie_start_state_hash = 0;
    g_tas_movie_start_transaction = {};
    g_tas_loaded_state_bundle = {};
    g_tas_loaded_environment = {};
    xemu_tas_set_deterministic_mode((flags & 1u) != 0);
    return true;
}

static bool TasLoadMovieFromPath(const char *path)
{
    if (!path || !*path) return false;
    FILE *f = qemu_fopen(path, "rb");
    if (!f) {
        char msg[512];
        snprintf(msg, sizeof(msg), "Could not open TAS movie: %s", strerror(errno));
        xemu_queue_error_message(msg);
        return false;
    }

    char magic[4];
    uint32_t version = 0, title_id = 0, flags = 0, ports = 0, report_size = 0;
    bool ok = fread(magic, 1, 4, f) == 4 && TasReadU32(f, &version) &&
              TasReadU32(f, &title_id) && TasReadU32(f, &flags) &&
              TasReadU32(f, &ports) && TasReadU32(f, &report_size);

    if (!ok) {
        fclose(f);
        xemu_queue_error_message("Invalid or unsupported .xmt movie");
        return false;
    }

    if (!memcmp(magic, "XMT1", 4) && version == 1) {
        ok = TasLoadXmt1(f, title_id, flags, ports, report_size);
    } else if (!memcmp(magic, "XMT2", 4) && version == 2 &&
               ports == XEMU_TAS_MAX_PORTS && report_size == XEMU_TAS_XID_REPORT_SIZE) {
        uint32_t current_branch = 0, current_parent = UINT32_MAX, next_branch = 1;
        std::string saved_version, saved_commit, disc_path;
        std::string bootrom_md5, flashrom_md5, eeprom_md5, hdd_path;
        uint32_t renderer = 0, surface_scale = 1, fit = 0, filtering = 0, aspect = 0, ff = 0;
        ok = TasReadU64(f, &g_tas_rerecord_count) &&
             TasReadU32(f, &current_branch) && TasReadU32(f, &current_parent) &&
             TasReadU32(f, &next_branch) &&
             TasReadString(f, &saved_version) && TasReadString(f, &saved_commit) &&
             TasReadString(f, &disc_path) && TasReadString(f, &bootrom_md5) &&
             TasReadString(f, &flashrom_md5) && TasReadString(f, &eeprom_md5) &&
             TasReadString(f, &hdd_path) && TasReadU32(f, &renderer) &&
             TasReadU32(f, &surface_scale) && TasReadU32(f, &fit) &&
             TasReadU32(f, &filtering) && TasReadU32(f, &aspect) &&
             TasReadU32(f, &ff) && TasReadFrames(f, &g_tas_frames, &g_tas_lag_flags);

        g_tas_current_branch = current_branch;
        g_tas_current_parent = current_parent;
        g_tas_next_branch_id = std::max(next_branch, current_branch + 1);
        g_tas_power_on_recording = (flags & 2u) != 0;
        g_tas_read_only = (flags & 4u) != 0;

        for (int i = 0; ok && i < 100; ++i) {
            uint8_t valid = 0;
            ok = TasReadU8(f, &valid) && TasReadU64(f, &g_tas_state_meta[i].frame) &&
                 TasReadU32(f, &g_tas_state_meta[i].branch_id);
            g_tas_state_meta[i].valid = valid != 0;
        }

        uint32_t chapter_count = 0;
        if (ok) ok = TasReadU32(f, &chapter_count) && chapter_count <= 10000;
        g_tas_chapters.clear();
        for (uint32_t i = 0; ok && i < chapter_count; ++i) {
            TasChapter c;
            ok = TasReadU64(f, &c.frame) && TasReadString(f, &c.name, 4096);
            if (ok) g_tas_chapters.push_back(std::move(c));
        }

        uint32_t branch_count = 0;
        if (ok) ok = TasReadU32(f, &branch_count) && branch_count <= 1000;
        g_tas_branches.clear();
        for (uint32_t i = 0; ok && i < branch_count; ++i) {
            TasBranch b;
            ok = TasReadU32(f, &b.id) && TasReadU32(f, &b.parent) &&
                 TasReadU64(f, &b.fork_frame) && TasReadString(f, &b.name, 4096) &&
                 TasReadFrames(f, &b.frames, &b.lag);
            if (ok) g_tas_branches.push_back(std::move(b));
        }

        g_tas_markers.clear();
        g_tas_properties = {};
        g_tas_verify_baseline.clear();
        g_tas_verify_revision = UINT64_MAX;
        g_tas_verify_start_transaction = {};
        g_tas_movie_revision = 0;
        g_tas_movie_start_snapshot.clear();
        g_tas_movie_start_legacy_ram_hash = 0;
        g_tas_movie_start_state_hash = 0;
        g_tas_movie_start_transaction = {};
        g_tas_loaded_state_bundle = {};
        g_tas_poll_counts.assign(g_tas_frames.size(), TasUnknownPollCounts());
        for (TasBranch &b : g_tas_branches) {
            b.polls.assign(b.frames.size(), TasUnknownPollCounts());
        }
        if (ok) {
            char ext_magic[4];
            size_t ext_read = fread(ext_magic, 1, sizeof(ext_magic), f);
            if (ext_read == sizeof(ext_magic) && !memcmp(ext_magic, "XEX3", 4)) {
                uint32_t ext_version = 0;
                uint32_t marker_count = 0, hash_count = 0;
                ok = TasReadU32(f, &ext_version) && ext_version == 1 &&
                     TasReadU64(f, &g_tas_movie_revision) &&
                     TasReadString(f, &g_tas_properties.author, 16384) &&
                     TasReadString(f, &g_tas_properties.category, 16384) &&
                     TasReadString(f, &g_tas_properties.game_version, 16384) &&
                     TasReadString(f, &g_tas_properties.comments, 1 << 20) &&
                     TasReadU32(f, &marker_count) && marker_count <= 100000;
                for (uint32_t i = 0; ok && i < marker_count; ++i) {
                    TasMarker m;
                    ok = TasReadU64(f, &m.frame) && TasReadString(f, &m.name, 16384) &&
                         TasReadString(f, &m.note, 1 << 20);
                    if (ok) g_tas_markers.push_back(std::move(m));
                }
                if (ok) ok = TasReadU32(f, &hash_count) && hash_count <= 1000000;
                for (uint32_t i = 0; ok && i < hash_count; ++i) {
                    TasHashRecord h;
                    ok = TasReadU64(f, &h.frame) && TasReadU64(f, &h.hash);
                    if (ok) g_tas_verify_baseline.push_back(h);
                }
                while (ok) {
                    char trailer[4];
                    const size_t trailer_read = fread(trailer, 1, sizeof(trailer), f);
                    if (trailer_read == 0) {
                        clearerr(f);
                        break;
                    }
                    if (trailer_read != sizeof(trailer)) {
                        ok = false;
                        break;
                    }

                    if (!memcmp(trailer, "TSST", 4)) {
                        ok = TasReadString(f, &g_tas_movie_start_snapshot, 4096) &&
                             TasReadU64(f, &g_tas_movie_start_legacy_ram_hash);
                    } else if (!memcmp(trailer, "TST2", 4)) {
                        uint32_t v = 0;
                        ok = TasReadU32(f, &v) && v == 1 &&
                             TasReadU64(f, &g_tas_movie_start_transaction.hi) &&
                             TasReadU64(f, &g_tas_movie_start_transaction.lo) &&
                             TasReadU64(f, &g_tas_movie_start_state_hash);
                    } else if (!memcmp(trailer, "TPOL", 4)) {
                        uint32_t v = 0, poll_branch_count = 0;
                        ok = TasReadU32(f, &v) && v == 1 &&
                             TasReadPollCounts(f, &g_tas_poll_counts,
                                               g_tas_frames.size()) &&
                             TasReadU32(f, &poll_branch_count) &&
                             poll_branch_count <= 1000;
                        for (uint32_t i = 0; ok && i < poll_branch_count; ++i) {
                            uint32_t id = 0;
                            ok = TasReadU32(f, &id);
                            auto it = std::find_if(g_tas_branches.begin(),
                                                   g_tas_branches.end(),
                                                   [id](const TasBranch &b) {
                                                       return b.id == id;
                                                   });
                            if (!ok || it == g_tas_branches.end()) {
                                ok = false;
                                break;
                            }
                            ok = TasReadPollCounts(f, &it->polls,
                                                   it->frames.size());
                        }
                    } else if (!memcmp(trailer, "TSM2", 4)) {
                        uint32_t v = 0;
                        ok = TasReadU32(f, &v) && v == 1;
                        for (TasStateMeta &meta : g_tas_state_meta) {
                            if (!ok) break;
                            ok = TasReadU64(f, &meta.transaction.hi) &&
                                 TasReadU64(f, &meta.transaction.lo);
                        }
                    } else if (!memcmp(trailer, "TVR2", 4)) {
                        uint32_t v = 0, interval = 0;
                        ok = TasReadU32(f, &v) && (v == 1 || v == 2) &&
                             TasReadU64(f, &g_tas_verify_revision) &&
                             TasReadU32(f, &interval) &&
                             TasReadU32(f, &g_tas_verify_branch) &&
                             TasReadU64(f, &g_tas_verify_start_frame) &&
                             TasReadU64(f, &g_tas_verify_start_transaction.hi) &&
                             TasReadU64(f, &g_tas_verify_start_transaction.lo);
                        if (ok && v >= 2) {
                            ok = TasReadString(f, &g_tas_verify_start_snapshot, 4096);
                        }
                        if (ok) g_tas_verify_interval = std::max(1, (int)interval);
                    } else if (!memcmp(trailer, "TBND", 4)) {
                        uint32_t v = 0, selected = 0, anchor = 0, end = 0;
                        uint32_t branch = 0, overdub_port = 0, field_mask = 0;
                        uint8_t mode = 0, was_running = 0, follow = 0, read_only = 0;
                        uint8_t last_lag = 0;
                        TasStateBundleRuntime bundle;
                        ok = TasReadU32(f, &v) && (v == 1 || v == 2) &&
                             TasReadU64(f, &bundle.transaction.hi) &&
                             TasReadU64(f, &bundle.transaction.lo) &&
                             TasReadU64(f, &bundle.boundary) &&
                             TasReadU64(f, &bundle.state_hash);
                        if (ok && v >= 2) {
                            ok = TasReadU8(f, &last_lag) &&
                                 TasReadU64(f, &bundle.lag_count) &&
                                 TasReadU64(f, &bundle.lag_streak);
                            bundle.last_frame_lagged = last_lag != 0;
                        }
                        if (ok) {
                            ok = TasReadU8(f, &mode) && mode <= (uint8_t)TasRuntimeMode::Overdub &&
                             TasReadU8(f, &was_running) &&
                             TasReadU32(f, &selected) && TasReadU32(f, &anchor) &&
                             TasReadU32(f, &end) && TasReadU8(f, &follow) &&
                             TasReadU8(f, &read_only) && TasReadU32(f, &branch) &&
                             TasReadU64(f, &bundle.movie_revision) &&
                             TasReadU64(f, &bundle.overdub_start) &&
                             TasReadU64(f, &bundle.overdub_synced) &&
                             TasReadU32(f, &overdub_port) &&
                             TasReadU32(f, &field_mask) &&
                             TasReadString(f, &bundle.owner_movie_path, 1 << 20);
                        }
                        if (ok) {
                            bundle.valid = true;
                            bundle.mode = (TasRuntimeMode)mode;
                            bundle.vm_was_running = was_running != 0;
                            bundle.selected_frame = (int)std::min<uint32_t>(selected, INT_MAX);
                            bundle.selection_anchor = (int)std::min<uint32_t>(anchor, INT_MAX);
                            bundle.selection_end = (int)std::min<uint32_t>(end, INT_MAX);
                            bundle.follow = follow != 0;
                            bundle.read_only = read_only != 0;
                            bundle.branch_id = branch;
                            bundle.overdub_port = overdub_port;
                            bundle.overdub_field_mask = field_mask;
                            g_tas_loaded_state_bundle = std::move(bundle);
                        }
                    } else {
                        /* Forward-compatible unknown tail: older code did not
                         * have section lengths, so stop rather than guessing. */
                        clearerr(f);
                        break;
                    }
                }
            } else if (ext_read != 0) {
                /* Unknown trailing data is ignored to preserve forward compatibility. */
                clearerr(f);
            } else {
                clearerr(f);
            }
        }

        g_tas_loaded_environment.valid = ok;
        g_tas_loaded_environment.title_id = title_id;
        g_tas_loaded_environment.xemu_version = saved_version;
        g_tas_loaded_environment.xemu_commit = saved_commit;
        g_tas_loaded_environment.disc_path = disc_path;
        g_tas_loaded_environment.bootrom_md5 = bootrom_md5;
        g_tas_loaded_environment.flashrom_md5 = flashrom_md5;
        g_tas_loaded_environment.eeprom_md5 = eeprom_md5;
        g_tas_loaded_environment.hdd_path = hdd_path;
        g_tas_loaded_environment.renderer = renderer;
        g_tas_loaded_environment.surface_scale = surface_scale;
        g_tas_loaded_environment.fit = fit;
        g_tas_loaded_environment.filtering = filtering;
        g_tas_loaded_environment.aspect = aspect;
        g_tas_loaded_environment.fast_forward = ff;
        g_tas_loaded_environment.deterministic = (flags & 1u) != 0;

        if (ok && g_tas_apply_movie_settings && !g_tas_silent_bundle_load) {
            g_config.display.renderer = (int)renderer;
            nv2a_set_surface_scale_factor(std::clamp((int)surface_scale, 1, 10));
            g_config.display.ui.fit = (int)fit;
            g_config.display.filtering = (int)filtering;
            g_config.display.ui.aspect_ratio = (int)aspect;
            g_config.general.fast_forward_multiplier = (int)ff;
            if (xemu_fast_forward_active()) {
                xemu_fast_forward_set_active(true);
            }
        }
        xemu_tas_set_deterministic_mode((flags & 1u) != 0);

        if (ok && !g_tas_silent_bundle_load &&
            saved_commit != (xemu_commit ? xemu_commit : "")) {
            xemu_queue_notification(".xmt was created by a different Xemu commit; deterministic verification is recommended");
        }
        if (ok && !g_tas_silent_bundle_load && !disc_path.empty()) {
            char *current_disc = xemu_get_currently_loaded_disc_path();
            bool mismatch = !current_disc || disc_path != current_disc;
            if (current_disc) g_free(current_disc);
            if (mismatch) xemu_queue_notification(".xmt disc path differs from the currently loaded disc");
        }
        if (ok && !g_tas_silent_bundle_load) {
            std::string cur_boot = TasFileMD5(g_config.sys.files.bootrom_path);
            std::string cur_flash = TasFileMD5(g_config.sys.files.flashrom_path);
            std::string cur_eeprom = TasFileMD5(g_config.sys.files.eeprom_path);
            bool rom_mismatch = (!bootrom_md5.empty() && bootrom_md5 != cur_boot) ||
                                (!flashrom_md5.empty() && flashrom_md5 != cur_flash) ||
                                (!eeprom_md5.empty() && eeprom_md5 != cur_eeprom);
            if (rom_mismatch) xemu_queue_notification(".xmt firmware/EEPROM identity differs from this Xemu setup");
            if (!hdd_path.empty() && g_config.sys.files.hdd_path && hdd_path != g_config.sys.files.hdd_path)
                xemu_queue_notification(".xmt HDD base path differs from this Xemu setup");
        }
    } else {
        ok = false;
    }
    fclose(f);

    if (!ok || g_tas_frames.empty()) {
        xemu_queue_error_message("Invalid, truncated, or unsupported .xmt movie");
        return false;
    }

    uint32_t current_title_id = 0;
    if (title_id && TasGetCurrentTitleId(&current_title_id) && current_title_id != title_id) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Warning: .xmt title ID %08X does not match running title %08X",
                 title_id, current_title_id);
        xemu_queue_notification(msg);
    }

    g_tas_selected_frame = 0;
    g_tas_selection_anchor = 0;
    g_tas_selection_end = 0;
    if (!g_tas_silent_bundle_load) g_tas_movie_path = path;
    g_tas_follow_frame = true;
    g_tas_movie_dirty = false;
    g_tas_core_pushed_revision = UINT64_MAX;
    g_tas_core_pushed_frame_count = 0;
    g_tas_visible_frame_cache.clear();
    g_tas_visible_frame_cache_dirty = true;
    g_tas_undo_stack.clear();
    g_tas_redo_stack.clear();
    g_tas_record_synced = 0;
    xemu_tas_stop_recording();
    xemu_tas_stop_playback();
    TasClearRewindCheckpoints();
    if (!g_tas_silent_bundle_load) xemu_queue_notification("Loaded TAS movie");
    return true;
}

static void TasSaveMovie()
{
    if (!g_tas_movie_path.empty()) {
        TasSaveMovieToPath(g_tas_movie_path.c_str());
        return;
    }

    static const SDL_DialogFileFilter filters[] = {
        { "Xemu TAS Movie (*.xmt)", "xmt" },
        { "All Files", "*" },
    };
    static std::string default_dir;
    default_dir = TasDefaultMovieDirectory();
    ShowSaveFileDialog(filters, 2, default_dir.c_str(), [](const char *path) {
        TasSaveMovieToPath(path);
    });
}

static void TasSaveMovieAs()
{
    static const SDL_DialogFileFilter filters[] = {
        { "Xemu TAS Movie (*.xmt)", "xmt" },
        { "All Files", "*" },
    };
    static std::string default_dir;
    default_dir = TasDefaultMovieDirectory();
    ShowSaveFileDialog(filters, 2,
                       g_tas_movie_path.empty() ? default_dir.c_str() : g_tas_movie_path.c_str(),
                       [](const char *path) { TasSaveMovieToPath(path); });
}

static void TasOpenMovie()
{
    static const SDL_DialogFileFilter filters[] = {
        { "Xemu TAS Movie (*.xmt)", "xmt" },
        { "All Files", "*" },
    };
    static std::string default_dir;
    default_dir = TasDefaultMovieDirectory();
    ShowOpenFileDialog(filters, 2,
                       g_tas_movie_path.empty() ? default_dir.c_str() : g_tas_movie_path.c_str(),
                       [](const char *path) { TasLoadMovieFromPath(path); });
}

static void TasPushMovieToCore()
{
    if (g_tas_frames.empty()) return;
    if (g_tas_core_pushed_revision == g_tas_movie_revision &&
        g_tas_core_pushed_frame_count == g_tas_frames.size()) {
        return;
    }
    /* TasFrame is exactly four packed 20-byte reports, so avoid allocating and
     * repacking the whole movie before the core makes its immutable playback
     * copy. Re-copy only after a movie edit/reload. */
    if (g_tas_poll_counts.size() != g_tas_frames.size()) {
        g_tas_poll_counts.resize(g_tas_frames.size(), TasUnknownPollCounts());
    }
    if (xemu_tas_set_playback_movie_ex(
            g_tas_frames.data(), g_tas_lag_flags.data(),
            reinterpret_cast<const uint32_t *>(g_tas_poll_counts.data()),
            g_tas_poll_counts.size() * XEMU_TAS_MAX_PORTS,
            g_tas_frames.size())) {
        g_tas_core_pushed_revision = g_tas_movie_revision;
        g_tas_core_pushed_frame_count = g_tas_frames.size();
    }
}

static void TasStartPlayback(uint64_t frame)
{
    TasCancelPendingTransportAdvance();
    if (!xemu_tas_enabled()) xemu_tas_set_enabled(true);
    if (g_tas_frames.empty()) return;
    frame = std::min<uint64_t>(frame, g_tas_frames.size() - 1);

    /* Playing from an arbitrary selected frame is always an exact seek. Never
     * relabel whatever live VM state happens to exist as that movie frame; that
     * was a direct source of rerecord desync even with Det unchecked. */
    if (!TasSeekFrameEx(frame, true)) {
        xemu_queue_error_message(
            "Play From Selected needs a valid checkpoint at/before that frame; use Play Beginning first to establish the movie baseline");
    }
}

static void TasStopPlaybackFromUi()
{
    /* Playback and the global TAS frame counter are deliberately independent:
     * stopping movie input must not stop normal emulation.  The piano roll,
     * however, should stay on the frame where playback was stopped instead of
     * immediately following the still-advancing global TAS counter. */
    const bool was_playing = xemu_tas_playback();
    const uint64_t stopped_frame = was_playing
        ? xemu_tas_playback_frame()
        : xemu_tas_frame();

    xemu_tas_stop_playback();
    TasCancelPendingTransportAdvance();

    if (was_playing && !g_tas_frames.empty()) {
        const int frame = (int)std::min<uint64_t>(
            std::min<uint64_t>(stopped_frame, g_tas_frames.size() - 1),
            INT_MAX);
        g_tas_selected_frame = frame;
        g_tas_selection_anchor = frame;
        g_tas_selection_end = frame;
    }

    /* A manual Stop Playback is an explicit request to stop following the
     * live timeline. The user can re-enable Follow whenever desired. */
    g_tas_follow_frame = false;
}

static void TasPlayMovieFromBeginning()
{
    g_tas_seek_continue_pending = false;
    if (g_tas_frames.empty()) {
        xemu_queue_error_message("TAS movie has no frames to play");
        return;
    }
    if (xemu_tas_recording() || g_tas_overdub_ui_active || xemu_tas_overdub()) {
        xemu_queue_error_message("Stop TAS recording/overdub before restarting movie playback");
        return;
    }

    /* Restart from the same machine state that originally preceded frame 0.
     * Current-state movies restore their captured baseline before installing
     * movie input, rather than using whatever state the Xbox is in now. */
    xemu_tas_stop_playback();
    if (!xemu_tas_enabled()) xemu_tas_set_enabled(true);
    TasClearRewindCheckpoints();

    if (runstate_is_running()) vm_stop(RUN_STATE_PAUSED);
    /* Both power-on and current-state movies replay from the exact canonical
     * frame-0 snapshot captured when the movie was created. Reissuing Reset
     * here would reintroduce mutable HDD/RTC/external-state differences. */
    if (!TasRestoreMovieStartSnapshot()) {
        return;
    }

    g_tas_selected_frame = 0;
    g_tas_selection_anchor = 0;
    g_tas_selection_end = 0;
    xemu_tas_clear_all_xid_reports();
    xemu_tas_set_frame(0);
    xemu_tas_reset_lag_counters();

    TasPushMovieToCore();
    if (xemu_tas_start_playback(0)) {
        if (g_tas_rewind_enabled || g_tas_greenzone_enabled) {
            TasSaveRewindCheckpointAtBoundary(0);
        }
        if (!runstate_is_running()) vm_start();
        xemu_queue_notification(g_tas_power_on_recording
            ? "TAS movie restarted from beginning (power-on/reset)"
            : "TAS movie restarted from captured current state");
    } else {
        xemu_queue_error_message("Could not start TAS movie from the beginning");
    }
}

static void TasStartRecording(bool power_on, bool start_vm = true)
{
    g_tas_seek_continue_pending = false;
    /* New recordings default to the reproducible policy. Loaded legacy movies
     * still retain their stored deterministic flag until the user changes it. */
    xemu_tas_set_deterministic_mode(true);
    if (g_tas_overdub_ui_active) TasStopOverdub();
    if (g_tas_read_only) {
        g_tas_read_only = false;
        xemu_queue_notification("Read-only disabled for recording");
    }
    xemu_tas_stop_playback();
    xemu_tas_clear_recording();
    g_tas_record_synced = 0;
    g_tas_last_autosave_record_count = 0;
    /* A fresh/empty movie owns one editor placeholder frame. Replace that
     * placeholder with the first captured input frame for either power-on or
     * current-state recording so recordings never acquire an artificial blank
     * frame zero. Existing non-empty movies keep their timeline intact. */
    g_tas_record_replace_placeholder = power_on || g_tas_frames.size() == 1;
    g_tas_power_on_recording = power_on;
    TasClearRewindCheckpoints();

    if (power_on) {
        g_tas_movie_start_snapshot.clear();
        g_tas_movie_start_state_hash = 0;
        g_tas_movie_start_legacy_ram_hash = 0;
        g_tas_movie_start_transaction = {};
        g_tas_frames.assign(1, TasFrame{});
        g_tas_lag_flags.assign(1, 0);
        g_tas_poll_counts.assign(1, TasUnknownPollCounts());
        g_tas_selected_frame = 0;
        g_tas_selection_anchor = 0;
        g_tas_selection_end = 0;
        g_tas_rerecord_count = 0;
        g_tas_branches.clear();
        g_tas_current_branch = 0;
        g_tas_current_parent = UINT32_MAX;
        g_tas_next_branch_id = 1;
        g_tas_visible_frame_cache_dirty = true;
        xemu_tas_clear_all_xid_reports();
        xemu_tas_set_frame(0);
        xemu_tas_reset_lag_counters();

        /* qemu_system_reset_request() is asynchronous. Arm the feature-owned
         * reset observer first; it will request a VM stop during the actual
         * reset transaction. Recording is armed only by TasPowerOnResetTick
         * after that callback has fired and the VM is confirmed paused. */
        g_tas_power_on_reset_sequence_before = xemu_tas_arm_post_reset_pause();
        g_tas_power_on_reset_start_vm = start_vm;
        g_tas_power_on_reset_deadline = std::chrono::steady_clock::now() +
                                        std::chrono::seconds(10);
        g_tas_power_on_reset_pending = true;
        ActionReset();
        xemu_queue_notification("TAS power-on reset requested; waiting for canonical post-reset frame 0");
        return;
    }

    xemu_tas_start_recording(true);
    if (start_vm && !runstate_is_running()) vm_start();
    xemu_queue_notification("TAS recording started from current state");
}

static void TasPowerOnResetTick()
{
    if (!g_tas_power_on_reset_pending) return;
    const bool reset_observed =
        xemu_tas_post_reset_sequence() != g_tas_power_on_reset_sequence_before;
    if (!reset_observed) {
        if (std::chrono::steady_clock::now() >= g_tas_power_on_reset_deadline) {
            g_tas_power_on_reset_pending = false;
            g_tas_power_on_reset_deadline = {};
            xemu_tas_cancel_post_reset_pause();
            xemu_tas_stop_recording();
            xemu_queue_error_message(
                "TAS power-on reset did not reach the canonical post-reset pause; recording was refused");
        }
        return;
    }
    /* The reset observer proves QEMU has traversed the real system reset. Its
     * vmstop request is intentionally only a race-prevention hint; some main
     * loop paths may still emerge running. Once the reset sequence changes we
     * are allowed to stop synchronously here and establish the canonical
     * post-reset boundary without guessing that ActionReset() was immediate. */
    if (runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED);
    }
    if (runstate_is_running()) {
        if (std::chrono::steady_clock::now() >= g_tas_power_on_reset_deadline) {
            g_tas_power_on_reset_pending = false;
            g_tas_power_on_reset_deadline = {};
            xemu_tas_cancel_post_reset_pause();
            xemu_tas_stop_recording();
            xemu_queue_error_message(
                "TAS reset completed but Xemu could not establish the canonical post-reset pause; recording was refused");
        }
        return;
    }

    g_tas_power_on_reset_pending = false;
    g_tas_power_on_reset_deadline = {};
    xemu_tas_clear_all_xid_reports();
    xemu_tas_set_frame(0);
    xemu_tas_reset_lag_counters();

    if (!TasCaptureMovieStartSnapshot()) {
        xemu_tas_stop_recording();
        xemu_queue_error_message(
            "Power-on TAS reset completed, but canonical frame-0 capture failed; recording was not started");
        return;
    }

    xemu_tas_start_recording(true);
    if (g_tas_nonram_fingerprint_capability == 0) {
        xemu_queue_notification(
            "TAS recording active: native snapshot + RAM/TAS Strict Sync fallback (non-RAM fingerprint unavailable on this configuration)");
    }
    if (g_tas_rewind_enabled || g_tas_greenzone_enabled) {
        TasSaveRewindCheckpointAtBoundary(0);
    }
    if (g_tas_power_on_reset_start_vm && !runstate_is_running()) vm_start();
    xemu_queue_notification(
        "TAS recording started from canonical post-reset frame 0");
}

static void TasStartFreshRecordingFromCurrentState()
{
    g_tas_seek_continue_pending = false;
    /* Make current VM state an atomic movie frame-0 baseline: pause, snapshot,
     * arm recording, then resume. This removes the old mid-frame start race. */
    const bool was_running = runstate_is_running();
    if (was_running) vm_stop(RUN_STATE_PAUSED);

    if (g_tas_overdub_ui_active) TasStopOverdub();
    xemu_tas_stop_recording();
    xemu_tas_stop_playback();

    g_tas_frames.assign(1, TasFrame{});
    TasResetMovieMetadata();
    g_tas_selected_frame = 0;
    g_tas_selection_anchor = 0;
    g_tas_selection_end = 0;
    g_tas_movie_path.clear();
    g_tas_power_on_recording = false;
    g_tas_record_replace_placeholder = true;
    xemu_tas_clear_all_xid_reports();
    xemu_tas_set_frame(0);
    xemu_tas_reset_lag_counters();

    if (!TasCaptureMovieStartSnapshot()) {
        xemu_queue_error_message(
            "Could not capture the current-state TAS baseline; recording was not started");
        if (was_running && !runstate_is_running()) vm_start();
        return;
    }

    TasStartRecording(false, false);
    if (g_tas_nonram_fingerprint_capability == 0) {
        xemu_queue_notification(
            "TAS recording active: native snapshot + RAM/TAS Strict Sync fallback (non-RAM fingerprint unavailable on this configuration)");
    }
    if (g_tas_rewind_enabled || g_tas_greenzone_enabled) {
        TasSaveRewindCheckpointAtBoundary(0);
    }
    if (!runstate_is_running()) vm_start();
    xemu_queue_notification("Captured exact TAS start state; recording from current state");
}

static void TasStartContextualRecording()
{
    g_tas_seek_continue_pending = false;
    /* TAStudio-style primary Record:
     *  - inside an existing movie, rerecord from the exact live frame and
     *    preserve the displaced future as a branch;
     *  - at the movie end, continue recording;
     *  - with no usable movie position, make the current VM a fresh frame 0.
     * This action never resets the Xbox. */
    const uint64_t live_frame = xemu_tas_playback() ? xemu_tas_playback_frame()
                                                    : xemu_tas_frame();
    const bool have_movie = g_tas_frames.size() > 1 || !g_tas_movie_path.empty();
    if (have_movie && live_frame <= g_tas_frames.size()) {
        if (g_tas_read_only) {
            g_tas_read_only = false;
            xemu_queue_notification("Read-only disabled for rerecording");
        }
        TasResumeMovieAtFrame(live_frame, true);
        g_tas_rewind_next_frame = live_frame;

        char msg[128];
        snprintf(msg, sizeof(msg), "TAS recording resumed from movie frame %llu",
                 (unsigned long long)live_frame);
        xemu_queue_notification(msg);
        return;
    }

    TasStartFreshRecordingFromCurrentState();
}

static bool TasFinishPendingOverdubStart()
{
    if (!g_tas_pending_overdub_start.active) return true;
    const TasPendingOverdubStart pending = g_tas_pending_overdub_start;
    g_tas_pending_overdub_start = {};

    const uint64_t boundary = xemu_tas_frame();
    if (boundary != pending.boundary || boundary >= g_tas_frames.size()) {
        xemu_queue_error_message(
            "TAS overdub exact-seek target was not reached; overdub was not started");
        return false;
    }

    TasPushMovieToCore();
    g_tas_overdub_start_frame = boundary;
    g_tas_overdub_synced = 0;
    g_tas_record_synced = 0;
    g_tas_port = std::clamp((int)pending.port, 0, XEMU_TAS_MAX_PORTS - 1);
    g_tas_overdub_ui_active = xemu_tas_start_overdub(
        boundary, pending.port, pending.digital_mask,
        pending.analog_mask, pending.stick_mask);
    if (!g_tas_overdub_ui_active) {
        xemu_queue_error_message("Could not start TAS overdub at the exact reconstructed boundary");
        return false;
    }
    if (!runstate_is_running()) vm_start();
    xemu_queue_notification("TAS punch-in/overdub recording started at exact movie boundary");
    return true;
}

static void TasStartOverdub()
{
    TasCancelPendingTransportAdvance();
    if (g_tas_frames.empty() || g_tas_pending_overdub_start.active) return;
    if (g_tas_read_only) g_tas_read_only = false;

    /* Drain any completed live recording first. Starting a punch-in is an
     * explicit runtime transition; it must never relabel an unrelated live VM
     * state as the selected movie frame. */
    if (runstate_is_running()) vm_stop(RUN_STATE_PAUSED);
    TasSyncRecordingFromCore();
    xemu_tas_stop_recording();
    xemu_tas_stop_overdub();
    xemu_tas_stop_playback();
    g_tas_overdub_ui_active = false;

    TasPushUndo("Punch-in / overdub recording");
    const uint64_t start = (uint64_t)std::clamp(
        g_tas_selected_frame, 0, (int)g_tas_frames.size() - 1);
    g_tas_overdub_start_frame = start;

    /* The old future poll trace is proof for the old input history. Punch-in
     * input may legitimately change the game's later polling behavior, so do
     * not compare the rerecord against stale evidence. The unchanged prefix
     * remains useful; newly recorded frames repopulate their own poll counts. */
    if (g_tas_poll_counts.size() < g_tas_frames.size()) {
        g_tas_poll_counts.resize(g_tas_frames.size(), TasUnknownPollCounts());
    }
    for (uint64_t i = start; i < g_tas_poll_counts.size(); ++i) {
        g_tas_poll_counts[(size_t)i] = TasUnknownPollCounts();
    }
    g_tas_verify_baseline.erase(
        std::remove_if(g_tas_verify_baseline.begin(), g_tas_verify_baseline.end(),
                       [start](const TasHashRecord &h) { return h.frame >= start; }),
        g_tas_verify_baseline.end());
    g_tas_verify_revision = UINT64_MAX;
    if (g_tas_verify_mode != TasVerifyMode::Idle) {
        g_tas_verify_mode = TasVerifyMode::Idle;
        g_tas_verify_status = "Verifier baseline invalidated by overdub";
    }
    g_tas_core_pushed_revision = UINT64_MAX;

    uint16_t digital_mask = 0;
    uint8_t analog_mask = 0;
    uint8_t stick_mask = 0;
    for (int i = 0; i < 8; ++i) if (g_tas_overdub_fields[i]) digital_mask |= (uint16_t)(1u << i);
    for (int i = 0; i < 8; ++i) if (g_tas_overdub_fields[8 + i]) analog_mask |= (uint8_t)(1u << i);
    for (int i = 0; i < 4; ++i) if (g_tas_overdub_fields[16 + i]) stick_mask |= (uint8_t)(1u << i);

    g_tas_pending_overdub_start.active = true;
    g_tas_pending_overdub_start.boundary = start;
    g_tas_pending_overdub_start.port = (uint8_t)std::clamp(g_tas_port, 0, XEMU_TAS_MAX_PORTS - 1);
    g_tas_pending_overdub_start.digital_mask = digital_mask;
    g_tas_pending_overdub_start.analog_mask = analog_mask;
    g_tas_pending_overdub_start.stick_mask = stick_mask;

    if (!TasSeekFrameEx(start, false)) {
        g_tas_pending_overdub_start = {};
        xemu_queue_error_message(
            "Could not reconstruct the selected TAS boundary; overdub was not started");
        return;
    }
    if (!g_tas_seek_completion_pending && !runstate_is_running()) {
        TasFinishPendingOverdubStart();
    }
}

static void TasStopOverdub()
{
    if (!g_tas_overdub_ui_active && !xemu_tas_overdub()) return;
    xemu_tas_stop_overdub();
    g_tas_overdub_ui_active = false;
    TasMarkMovieEdited(g_tas_overdub_start_frame);
    TasAutosaveRecovery(true);
    xemu_queue_notification("TAS punch-in/overdub stopped");
}

static void TasSyncRecordingFromCore()
{
    const uint64_t count = xemu_tas_recorded_frame_count();
    if (g_tas_record_synced >= count) {
        return;
    }

    /* Pull many frames under one core mutex. This matters under Fast Forward,
     * where hundreds of recorded guest frames may arrive between UI draws. */
    constexpr uint64_t kBatchFrames = 256;
    std::array<uint8_t, kBatchFrames * XEMU_TAS_FRAME_REPORT_BYTES> reports{};
    std::array<uint8_t, kBatchFrames> lag{};
    std::array<uint32_t, kBatchFrames * XEMU_TAS_MAX_PORTS> polls{};

    bool changed = false;
    if (!g_tas_overdub_ui_active) {
        const uint64_t remaining = count - g_tas_record_synced;
        g_tas_frames.reserve(g_tas_frames.size() + (size_t)remaining);
        g_tas_lag_flags.reserve(g_tas_lag_flags.size() + (size_t)remaining);
        g_tas_poll_counts.reserve(g_tas_poll_counts.size() + (size_t)remaining);
    }

    while (g_tas_record_synced < count) {
        const uint64_t wanted = std::min<uint64_t>(kBatchFrames,
                                                   count - g_tas_record_synced);
        const uint64_t copied = xemu_tas_copy_recorded_frames_ex(
            g_tas_record_synced, wanted,
            reports.data(), (size_t)wanted * XEMU_TAS_FRAME_REPORT_BYTES,
            lag.data(), (size_t)wanted,
            polls.data(), (size_t)wanted * XEMU_TAS_MAX_PORTS);
        if (!copied) {
            break;
        }

        for (uint64_t j = 0; j < copied; ++j) {
            const uint8_t *packed = reports.data() + j * XEMU_TAS_FRAME_REPORT_BYTES;
            const bool lagged = lag[(size_t)j] != 0;
            TasPollCounts poll{};
            for (int port = 0; port < XEMU_TAS_MAX_PORTS; ++port) {
                poll[(size_t)port] = polls[(size_t)j * XEMU_TAS_MAX_PORTS + (size_t)port];
            }

            if (g_tas_record_replace_placeholder) {
                g_tas_frames.clear();
                g_tas_lag_flags.clear();
                g_tas_poll_counts.clear();
                g_tas_record_replace_placeholder = false;
            }

            TasFrame frame;
            for (int port = 0; port < XEMU_TAS_MAX_PORTS; ++port) {
                memcpy(frame.xid[port].data(),
                       packed + port * XEMU_TAS_XID_REPORT_SIZE,
                       XEMU_TAS_XID_REPORT_SIZE);
            }

            if (g_tas_overdub_ui_active) {
                const uint64_t target = g_tas_overdub_start_frame + g_tas_overdub_synced;
                if (target < g_tas_frames.size()) {
                    g_tas_frames[(size_t)target] = std::move(frame);
                    if (target < g_tas_lag_flags.size()) {
                        g_tas_lag_flags[(size_t)target] = lagged ? 1 : 0;
                    }
                    if (target >= g_tas_poll_counts.size()) {
                        g_tas_poll_counts.resize(g_tas_frames.size(), TasUnknownPollCounts());
                    }
                    g_tas_poll_counts[(size_t)target] = poll;
                    ++g_tas_overdub_synced;
                } else {
                    TasStopOverdub();
                    break;
                }
            } else {
                g_tas_frames.push_back(std::move(frame));
                g_tas_lag_flags.push_back(lagged ? 1 : 0);
                g_tas_poll_counts.push_back(poll);
            }
            ++g_tas_record_synced;
            changed = true;
        }
    }

    if (changed) {
        g_tas_movie_dirty = true;
        ++g_tas_movie_revision;
        g_tas_core_pushed_revision = UINT64_MAX;
        g_tas_visible_frame_cache_dirty = true;
        /* Appending doesn't invalidate earlier greenzone checkpoints. Overdub
         * does, but defer that potentially expensive cleanup until recording
         * stops rather than doing it every UI refresh. */
    }

    if (g_tas_overdub_ui_active && !xemu_tas_overdub()) {
        g_tas_overdub_ui_active = false;
        TasMarkMovieEdited(g_tas_overdub_start_frame);
        TasAutosaveRecovery(true);
    }
    if (xemu_tas_recording() && count) {
        TasAutosaveRecovery(false);
    }
}

static TasRuntimeMode TasCurrentRuntimeMode()
{
    if (g_tas_overdub_ui_active || xemu_tas_overdub()) return TasRuntimeMode::Overdub;
    if (xemu_tas_recording()) return TasRuntimeMode::Recording;
    if (xemu_tas_playback()) return TasRuntimeMode::Playback;
    return TasRuntimeMode::Idle;
}

static void TasNoteTimelineMutation(uint64_t first_frame)
{
    if (!g_tas_timeline_mutation.active) return;
    g_tas_timeline_mutation.edited = true;
    g_tas_timeline_mutation.first_frame = std::min(
        g_tas_timeline_mutation.first_frame, first_frame);
}

static void TasPrepareTimelineMutation(uint64_t first_frame)
{
    if (g_tas_timeline_mutation.active) {
        g_tas_timeline_mutation.first_frame = std::min(
            g_tas_timeline_mutation.first_frame, first_frame);
        return;
    }

    const TasRuntimeMode mode = TasCurrentRuntimeMode();
    if (mode == TasRuntimeMode::Idle) return;

    TasTimelineMutationTxn txn;
    txn.active = true;
    txn.mode = mode;
    txn.vm_was_running = runstate_is_running();
    txn.first_frame = first_frame;
    txn.overdub_port = (uint32_t)std::clamp(g_tas_port, 0, XEMU_TAS_MAX_PORTS - 1);
    txn.overdub_field_mask = TasOverdubFieldMask();
    g_tas_timeline_mutation = txn;

    /* Freeze before draining core-owned frames.  From this point until commit,
     * Record/Overdub cannot advance the Xbox on history that the editor is in
     * the process of changing. */
    if (runstate_is_running()) vm_stop(RUN_STATE_PAUSED);
    TasCancelPendingTransportAdvance();
    TasSyncRecordingFromCore();
    g_tas_timeline_mutation.boundary = xemu_tas_frame();

    /* Overdub can naturally reach the movie end while the final completed
     * frames are being drained.  Treat that as a cleanly finished runtime,
     * rather than trying to resurrect an already-completed punch-in. */
    if (g_tas_timeline_mutation.mode == TasRuntimeMode::Overdub &&
        !g_tas_overdub_ui_active && !xemu_tas_overdub()) {
        g_tas_timeline_mutation.mode = TasRuntimeMode::Idle;
    } else if (g_tas_timeline_mutation.mode == TasRuntimeMode::Recording &&
               !xemu_tas_recording()) {
        g_tas_timeline_mutation.mode = TasRuntimeMode::Idle;
    }

    xemu_tas_stop_overdub();
    xemu_tas_stop_recording();
    xemu_tas_stop_playback();
    g_tas_overdub_ui_active = false;
}

static void TasAbortTimelineMutation(const char *reason)
{
    if (!g_tas_timeline_mutation.active) return;
    g_tas_timeline_mutation = {};
    xemu_tas_stop_overdub();
    xemu_tas_stop_recording();
    xemu_tas_stop_playback();
    g_tas_overdub_ui_active = false;
    if (runstate_is_running()) vm_stop(RUN_STATE_PAUSED);
    if (reason && *reason) xemu_queue_error_message(reason);
}

static bool TasRestoreTimelineRuntimeAfterMutation()
{
    if (!g_tas_timeline_mutation.active) return true;
    const TasTimelineMutationTxn txn = g_tas_timeline_mutation;
    g_tas_timeline_mutation = {};

    uint64_t boundary = std::min<uint64_t>(txn.boundary, g_tas_frames.size());
    xemu_tas_set_frame(boundary);
    g_tas_record_synced = 0;
    g_tas_record_replace_placeholder = false;
    g_tas_overdub_ui_active = false;

    switch (txn.mode) {
    case TasRuntimeMode::Idle:
        break;
    case TasRuntimeMode::Playback:
        TasPushMovieToCore();
        if (boundary < g_tas_frames.size() && !xemu_tas_start_playback(boundary)) {
            xemu_queue_error_message(
                "Edited TAS was rebuilt, but playback mode could not be restored; VM remains paused");
            return false;
        }
        break;
    case TasRuntimeMode::Recording:
        /* An insertion before the live boundary can create unexecuted future
         * rows even though Record previously owned the end of the movie.  Keep
         * that future as a branch, then continue recording from exact boundary N. */
        if (boundary < g_tas_frames.size()) {
            TasArchiveCurrentBranch(boundary,
                                    "Preserved future after live recording edit");
            ++g_tas_rerecord_count;
            g_tas_movie_dirty = true;
            ++g_tas_movie_revision;
            g_tas_core_pushed_revision = UINT64_MAX;
        }
        if (boundary == 0 && g_tas_frames.size() == 1) {
            g_tas_record_replace_placeholder = true;
        }
        xemu_tas_clear_recording();
        xemu_tas_start_recording(true);
        break;
    case TasRuntimeMode::Overdub: {
        if (boundary >= g_tas_frames.size()) {
            xemu_queue_error_message(
                "Edited TAS no longer has an input row at the overdub boundary; overdub was not resumed");
            return false;
        }
        TasPushMovieToCore();
        uint16_t digital_mask = (uint16_t)(txn.overdub_field_mask & 0xffu);
        uint8_t analog_mask = (uint8_t)((txn.overdub_field_mask >> 8) & 0xffu);
        uint8_t stick_mask = (uint8_t)((txn.overdub_field_mask >> 16) & 0x0fu);
        g_tas_port = std::clamp((int)txn.overdub_port, 0, XEMU_TAS_MAX_PORTS - 1);
        g_tas_overdub_start_frame = boundary;
        g_tas_overdub_synced = 0;
        g_tas_record_synced = 0;
        g_tas_overdub_ui_active = xemu_tas_start_overdub(
            boundary, (uint8_t)g_tas_port, digital_mask, analog_mask, stick_mask);
        if (!g_tas_overdub_ui_active) {
            xemu_queue_error_message(
                "Edited TAS was rebuilt, but overdub mode could not be restored; VM remains paused");
            return false;
        }
        break;
    }
    }

    if (txn.vm_was_running && !runstate_is_running()) vm_start();
    return true;
}

static void TasTimelineMutationTick()
{
    if (!g_tas_timeline_mutation.active ||
        g_tas_timeline_mutation.reconstructing) {
        return;
    }

    /* Keep a multi-frame ImGui drag as one atomic edit transaction. */
    if (ImGui::IsAnyItemActive()) return;

    if (!g_tas_timeline_mutation.edited) {
        TasRestoreTimelineRuntimeAfterMutation();
        return;
    }

    const uint64_t target = std::min<uint64_t>(
        g_tas_timeline_mutation.boundary, g_tas_frames.size());
    if (g_tas_timeline_mutation.first_frame >= target) {
        TasRestoreTimelineRuntimeAfterMutation();
        return;
    }

    g_tas_timeline_mutation.reconstructing = true;
    if (!TasSeekFrameEx(target, false)) {
        TasAbortTimelineMutation(
            "TAS edit transaction could not reconstruct the edited timeline; VM remains paused");
        return;
    }

    /* A target that is already the restored anchor completes synchronously. */
    if (!g_tas_seek_completion_pending && !runstate_is_running()) {
        TasRestoreTimelineRuntimeAfterMutation();
    }
}

static void TasRetireBranchGuard(const TasBranchSwitchTxn &txn)
{
    if (!txn.guard_snapshot.empty() &&
        g_tas_snapshot_delete_queued.emplace(txn.guard_snapshot).second) {
        g_tas_snapshot_delete_queue.push_back(txn.guard_snapshot);
    }
    if (!txn.guard_bundle_path.empty()) {
        std::error_code ec;
        std::filesystem::remove(txn.guard_bundle_path, ec);
    }
}

static void TasRollbackBranchSwitch(const char *reason)
{
    if (!g_tas_branch_switch.active) return;
    TasBranchSwitchTxn txn = std::move(g_tas_branch_switch);
    g_tas_branch_switch = {};

    xemu_tas_stop_overdub();
    xemu_tas_stop_recording();
    xemu_tas_stop_playback();
    g_tas_overdub_ui_active = false;
    if (runstate_is_running()) vm_stop(RUN_STATE_PAUSED);

    const bool restored = TasLoadPairedSnapshot(
        txn.guard_snapshot.c_str(), txn.guard_bundle_path);
    TasRetireBranchGuard(txn);
    if (!restored) {
        xemu_queue_error_message(
            "TAS branch switch failed and its transactional rollback also failed; VM remains paused");
        return;
    }
    if (reason && *reason) xemu_queue_error_message(reason);
}

static void TasFinishBranchSwitch()
{
    if (!g_tas_branch_switch.active) return;
    TasBranchSwitchTxn txn = std::move(g_tas_branch_switch);
    g_tas_branch_switch = {};

    const uint64_t boundary = std::min<uint64_t>(
        txn.target_boundary, g_tas_frames.size());
    const uint64_t selected = g_tas_frames.empty() ? 0 :
        std::min<uint64_t>(boundary, g_tas_frames.size() - 1);
    g_tas_selected_frame = (int)std::min<uint64_t>(selected, INT_MAX);
    g_tas_selection_anchor = g_tas_selected_frame;
    g_tas_selection_end = g_tas_selected_frame;
    g_tas_follow_frame = txn.old_runtime.follow;
    g_tas_read_only = txn.old_runtime.read_only;

    /* Reuse the same runtime-intent restorer used by atomic live edits.  It
     * resumes Playback/Record/Overdub only after the target branch has been
     * reconstructed and verified at the exact requested boundary. */
    g_tas_timeline_mutation.active = true;
    g_tas_timeline_mutation.mode = txn.old_runtime.mode;
    g_tas_timeline_mutation.vm_was_running = txn.old_runtime.vm_was_running;
    g_tas_timeline_mutation.boundary = boundary;
    g_tas_timeline_mutation.overdub_port = txn.old_runtime.overdub_port;
    g_tas_timeline_mutation.overdub_field_mask = txn.old_runtime.overdub_field_mask;
    const bool resumed = TasRestoreTimelineRuntimeAfterMutation();

    if (!resumed) {
        xemu_tas_stop_overdub();
        xemu_tas_stop_recording();
        xemu_tas_stop_playback();
        g_tas_overdub_ui_active = false;
        if (runstate_is_running()) vm_stop(RUN_STATE_PAUSED);
        const bool rolled_back = TasLoadPairedSnapshot(
            txn.guard_snapshot.c_str(), txn.guard_bundle_path);
        TasRetireBranchGuard(txn);
        if (rolled_back) {
            xemu_queue_error_message(
                "Target TAS branch could not restore the previous runtime mode; restored previous branch");
        } else {
            xemu_queue_error_message(
                "Target TAS branch runtime restore failed and rollback failed; VM remains paused");
        }
        return;
    }

    TasRetireBranchGuard(txn);
    xemu_queue_notification("Switched TAS branch transactionally");
    TasAutosaveRecovery(true);
}

static bool TasSwitchToBranchIndex(size_t index)
{
    if (index >= g_tas_branches.size() || g_tas_branch_switch.active ||
        g_tas_timeline_mutation.active) {
        return false;
    }

    const bool was_running = runstate_is_running();
    if (was_running) vm_stop(RUN_STATE_PAUSED);
    TasSyncRecordingFromCore();

    uint32_t title_id = 0;
    if (!TasGetCurrentTitleId(&title_id)) {
        if (was_running && !runstate_is_running()) vm_start();
        xemu_queue_error_message("TAS branch switch requires a running XBE/title ID");
        return false;
    }

    const uint64_t stamp = (uint64_t)std::chrono::steady_clock::now()
        .time_since_epoch().count();
    char guard_name[96];
    snprintf(guard_name, sizeof(guard_name), "%08X_TAS_BRANCH_GUARD_%016llX",
             title_id, (unsigned long long)stamp);
    const std::string guard_bundle = TasStateBundlePath(guard_name);

    TasStateBundleRuntime old_runtime;
    if (!TasSavePairedSnapshot(guard_name, guard_bundle, &old_runtime)) {
        if (was_running && !runstate_is_running()) vm_start();
        return false;
    }
    old_runtime.vm_was_running = was_running;
    if (!TasSaveMovieToPathInternal(guard_bundle.c_str(), false, false,
                                    &old_runtime)) {
        TasBranchSwitchTxn failed;
        failed.guard_snapshot = guard_name;
        failed.guard_bundle_path = guard_bundle;
        TasRetireBranchGuard(failed);
        if (was_running && !runstate_is_running()) vm_start();
        xemu_queue_error_message("Could not finalize TAS branch rollback guard");
        return false;
    }

    TasCancelPendingTransportAdvance();
    xemu_tas_stop_overdub();
    xemu_tas_stop_recording();
    xemu_tas_stop_playback();
    g_tas_overdub_ui_active = false;

    TasBranch target = std::move(g_tas_branches[index]);
    g_tas_branches.erase(g_tas_branches.begin() + index);
    const uint64_t old_boundary = old_runtime.boundary;
    TasStoreCurrentBranch(old_boundary, "Branch before switch");

    g_tas_frames = std::move(target.frames);
    g_tas_lag_flags = std::move(target.lag);
    g_tas_poll_counts = std::move(target.polls);
    if (g_tas_frames.empty()) g_tas_frames.resize(1);
    if (g_tas_lag_flags.size() < g_tas_frames.size()) {
        g_tas_lag_flags.resize(g_tas_frames.size(), 0);
    }
    if (g_tas_poll_counts.size() < g_tas_frames.size()) {
        g_tas_poll_counts.resize(g_tas_frames.size(), TasUnknownPollCounts());
    }
    g_tas_current_branch = target.id;
    g_tas_current_parent = target.parent;
    ++g_tas_movie_revision;
    g_tas_movie_dirty = true;
    g_tas_core_pushed_revision = UINT64_MAX;
    g_tas_visible_frame_cache_dirty = true;
    g_tas_verify_revision = UINT64_MAX;
    g_tas_verify_baseline.clear();

    g_tas_branch_switch.active = true;
    g_tas_branch_switch.reconstructing = true;
    g_tas_branch_switch.guard_snapshot = guard_name;
    g_tas_branch_switch.guard_bundle_path = guard_bundle;
    g_tas_branch_switch.old_runtime = old_runtime;
    g_tas_branch_switch.target_boundary = std::min<uint64_t>(
        old_boundary, g_tas_frames.size());

    if (!TasSeekFrameEx(g_tas_branch_switch.target_boundary, false)) {
        TasRollbackBranchSwitch(
            "TAS branch switch could not reconstruct the target branch; restored previous branch");
        return false;
    }
    if (!g_tas_seek_completion_pending && !runstate_is_running()) {
        TasFinishBranchSwitch();
    }
    return true;
}

static void TasCancelPendingTransportAdvance()
{
    /* User transport always wins over maintenance transport. A Strict Sync
     * checkpoint may have scheduled a one-frame pause+resume in the background;
     * carrying that resume flag through a rewind/seek can restart the VM after
     * the requested target was reached and look like a ~60-frame overshoot. */
    if (xemu_tas_frame_advance_remaining() != 0 ||
        g_tas_strict_checkpoint_pending || g_tas_strict_step_checkpoint_pending) {
        xemu_tas_cancel_frame_advance();
    }
    g_tas_strict_checkpoint_pending = false;
    g_tas_strict_checkpoint_resume = false;
    g_tas_strict_step_checkpoint_pending = false;
    xemu_tas_set_seek_catchup(false);
    g_tas_seek_continue_pending = false;
    g_tas_seek_completion_pending = false;
    g_tas_step_completion_pending = false;
    if (g_tas_verify_mode != TasVerifyMode::Idle) {
        g_tas_verify_mode = TasVerifyMode::Idle;
        g_tas_verify_status = "Verifier stopped by manual TAS transport";
    }
}

static void TasAdvanceFrames(uint32_t count, bool skip_lag = false)
{
    if (!count) return;
    if (!xemu_tas_enabled()) xemu_tas_set_enabled(true);

    /* Freeze first, then discard every older transport request. This makes
     * repeated/spammed +1/-1 operations replace the previous request instead
     * of stacking a stale resume onto the new one. */
    if (runstate_is_running()) vm_stop(RUN_STATE_PAUSED);
    TasCancelPendingTransportAdvance();

    const uint64_t start_frame = xemu_tas_frame();
    if (!skip_lag) {
        g_tas_step_completion_pending = true;
        g_tas_step_completion_target = start_frame + (uint64_t)count;
    }
    /* Every manual frame step is already an exact paused VBLANK boundary.
     * Cache it regardless of the Det checkbox so +/- transport remains exact
     * for normal users too. */
    g_tas_strict_step_checkpoint_pending =
        (g_tas_rewind_enabled || g_tas_greenzone_enabled);
    xemu_tas_request_frame_advance_ex(count, skip_lag);
    vm_start();
}

static void TasInsertMacro()
{
    TasPushTimelineUndo("Insert combo / macro", (uint64_t)std::max(0, g_tas_selected_frame));
    int start = std::clamp(g_tas_selected_frame, 0, (int)g_tas_frames.size());
    int total = std::max(1, g_tas_macro_repeats) *
                (std::max(1, g_tas_macro_press_frames) + std::max(0, g_tas_macro_gap_frames));
    if (start + total > (int)g_tas_frames.size()) {
        g_tas_frames.resize(start + total);
        g_tas_lag_flags.resize(start + total, 0);
        g_tas_poll_counts.resize(start + total, TasUnknownPollCounts());
    }
    for (int rep = 0; rep < std::max(1, g_tas_macro_repeats); ++rep) {
        int base = start + rep * (std::max(1, g_tas_macro_press_frames) + std::max(0, g_tas_macro_gap_frames));
        for (int k = 0; k < std::max(1, g_tas_macro_press_frames); ++k) {
            TasXidReport &r = g_tas_frames[base + k].xid[g_tas_port];
            if (g_tas_macro_control < 8) {
                uint16_t b = tas_read_u16(r, 2);
                b |= (uint16_t)(1u << g_tas_macro_control);
                memcpy(&r[2], &b, sizeof(b));
            } else {
                r[4 + (g_tas_macro_control - 8)] = (uint8_t)std::clamp(g_tas_macro_value, 0, 255);
            }
        }
    }
    TasMarkMovieEdited((uint64_t)start);
    xemu_queue_notification("Inserted TAS combo/macro into movie");
    TasAutosaveRecovery(true);
}

static void TasUpdateAutomation()
{
    uint8_t control = (uint8_t)std::clamp(g_tas_auto_control, 0, 15);
    uint8_t value = (uint8_t)std::clamp(g_tas_auto_value, 0, 255);
    xemu_tas_set_auto_hold((uint8_t)g_tas_port, control, g_tas_auto_hold_enabled, value);
    xemu_tas_set_autofire((uint8_t)g_tas_port, control, g_tas_autofire_enabled, value,
                          (uint32_t)std::max(1, g_tas_autofire_period),
                          (uint32_t)std::max(0, g_tas_autofire_phase));
}

static void TasInvalidateGreenzoneFrom(uint64_t frame)
{
    for (auto &cp : g_tas_rewind_points) {
        if (!cp.valid || cp.branch_id != g_tas_current_branch || cp.frame < frame) continue;
        if (!cp.snapshot_name.empty()) {
            /* Invalidate immediately but defer QCOW snapshot deletion until the
             * VM is paused. Avoid queuing the same native state more than once
             * when several edits invalidate overlapping greenzone ranges. */
            if (g_tas_snapshot_delete_queued.emplace(cp.snapshot_name).second) {
                g_tas_snapshot_delete_queue.push_back(cp.snapshot_name);
            }
        }
        cp = {};
    }
}

static void TasServiceDeferredSnapshotDeletes()
{
    if (g_tas_snapshot_delete_queue.empty() || runstate_is_running()) {
        return;
    }
    /* One deletion per UI tick bounds pause-time latency as well. */
    std::string name = std::move(g_tas_snapshot_delete_queue.front());
    g_tas_snapshot_delete_queue.pop_front();
    g_tas_snapshot_delete_queued.erase(name);
    Error *err = NULL;
    xemu_snapshots_delete(name.c_str(), &err);
    if (err) {
        error_free(err);
    } else {
        TasSnapshotCacheErase(name.c_str());
    }
}

static bool TasSaveRewindCheckpointAtBoundary(uint64_t frame)
{
    uint32_t title_id = 0;
    if (!TasGetCurrentTitleId(&title_id)) return false;

    const bool can_fingerprint = !runstate_is_running() &&
                                 xemu_tas_deterministic_mode();
    uint64_t current_hash = 0;
    if (can_fingerprint && !TasComputeStateHash(&current_hash)) {
        xemu_queue_error_message(
            "Could not compute Strict Sync checkpoint fingerprint; checkpoint was not saved");
        return false;
    }
    if (can_fingerprint) {
        for (const TasRewindCheckpoint &known : g_tas_rewind_points) {
            if (!known.valid || known.branch_id != g_tas_current_branch ||
                known.frame != frame || !known.state_hash) {
                continue;
            }
            if (known.state_hash != current_hash) {
                g_tas_verify_failed = true;
                g_tas_verify_first_bad_frame = frame;
                g_tas_verify_expected = known.state_hash;
                g_tas_verify_actual = current_hash;
                g_tas_verify_status =
                    "STRICT SYNC DESYNC at frame " + std::to_string(frame);
                xemu_queue_error_message(g_tas_verify_status.c_str());
                return false;
            }
            /* An identical checkpoint already proves this frame. Do not churn
             * QCOW metadata or overwrite the known-good anchor unnecessarily.
             * Still move the periodic checkpoint deadline forward so resuming
             * from this exact anchor does not immediately schedule another save. */
            const int interval = g_tas_greenzone_enabled
                ? g_tas_greenzone_interval : g_tas_rewind_interval;
            g_tas_rewind_next_frame = frame + (uint64_t)std::max(30, interval);
            return true;
        }
    }

    int capacity = std::clamp(g_tas_greenzone_enabled ? g_tas_greenzone_capacity :
                              (int)g_tas_rewind_points.size(), 4,
                              (int)g_tas_rewind_points.size());
    int slot = g_tas_rewind_slot % capacity;
    char name[64];
    snprintf(name, sizeof(name), "%08X_TAS_GZ_%02d", title_id, slot);

    if (g_tas_snapshot_delete_queued.erase(name)) {
        g_tas_snapshot_delete_queue.erase(
            std::remove(g_tas_snapshot_delete_queue.begin(),
                        g_tas_snapshot_delete_queue.end(), std::string(name)),
            g_tas_snapshot_delete_queue.end());
    }

    if (TasSnapshotExists(name)) {
        Error *del_err = NULL;
        xemu_snapshots_delete(name, &del_err);
        if (del_err) {
            error_free(del_err);
        } else {
            TasSnapshotCacheErase(name);
        }
    }

    XemuTasTransactionId transaction{};
    xemu_tas_transaction_mint(&transaction);

    Error *err = NULL;
    xemu_tas_transaction_snapshot_begin();
    xemu_snapshots_save_no_thumbnail(name, &err);
    xemu_tas_transaction_snapshot_end();
    if (err) {
        error_free(err);
        return false;
    }

    TasSnapshotCacheInsert(name);
    TasRewindCheckpoint &cp = g_tas_rewind_points[slot];
    cp.valid = true;
    cp.frame = frame;
    cp.branch_id = g_tas_current_branch;
    /* Every transport checkpoint is canonical/paused. Strict Sync additionally
     * fingerprints it; ordinary mode keeps exact navigation without treating
     * legitimate nondeterminism as a verifier failure. */
    cp.state_hash = current_hash;
    cp.last_frame_lagged = xemu_tas_last_frame_lagged();
    cp.lag_count = xemu_tas_lag_count();
    cp.lag_streak = xemu_tas_lag_streak();
    cp.transaction = transaction;
    cp.snapshot_name = name;
    g_tas_rewind_slot = (slot + 1) % capacity;

    int interval = g_tas_greenzone_enabled ? g_tas_greenzone_interval
                                           : g_tas_rewind_interval;
    g_tas_rewind_next_frame = frame + (uint64_t)std::max(30, interval);
    return true;
}

static void TasUpdateRewindCache()
{
    if (g_tas_strict_resim_pending) {
        return;
    }
    if ((!g_tas_rewind_enabled && !g_tas_greenzone_enabled) ||
        !xemu_tas_enabled()) {
        return;
    }

    /* Rewind/seek correctness must not depend on the Det checkbox. All TAS
     * transport checkpoints are canonical paused VBLANK boundaries. */
    uint64_t frame = xemu_tas_frame();

    {
        if (g_tas_strict_step_checkpoint_pending) {
            if (runstate_is_running() || xemu_tas_frame_advance_remaining() != 0) {
                return;
            }
            g_tas_strict_step_checkpoint_pending = false;
            frame = xemu_tas_frame();
            TasSaveRewindCheckpointAtBoundary(frame);
            /* A manual frame-step must remain paused. */
            return;
        }

        if (g_tas_strict_checkpoint_pending) {
            /* A manual pause may happen before our requested VBLANK. Only a
             * consumed frame-advance (remaining == 0) proves the canonical
             * boundary was reached. */
            if (runstate_is_running() || xemu_tas_frame_advance_remaining() != 0) {
                return;
            }

            const bool resume = g_tas_strict_checkpoint_resume;
            g_tas_strict_checkpoint_pending = false;
            g_tas_strict_checkpoint_resume = false;
            frame = xemu_tas_frame();
            const bool saved = TasSaveRewindCheckpointAtBoundary(frame);
            if (resume && saved && !runstate_is_running()) {
                vm_start();
            } else if (resume && !saved) {
                xemu_queue_error_message(
                    "Strict Sync checkpoint failed; VM remains paused");
            }
            return;
        }

        if (!runstate_is_running() || frame < g_tas_rewind_next_frame ||
            xemu_tas_frame_advance_remaining() != 0) {
            return;
        }

        /* Do not snapshot a running frame. Arrange for the existing TAS VBLANK
         * hook to stop us at the next exact boundary, then save on the UI tick
         * above. Host time may delay the UI tick, but guest state cannot move
         * while paused. */
        g_tas_strict_checkpoint_pending = true;
        g_tas_strict_checkpoint_resume = true;
        xemu_tas_request_frame_advance(1);
        return;
    }
}

static bool TasSeekFrameEx(uint64_t target, bool continue_after_target)
{
    g_tas_seek_continue_pending = false;
    g_tas_seek_completion_pending = false;
    if (g_tas_frames.empty()) return false;
    target = std::min<uint64_t>(target, g_tas_frames.size());
    const TasRewindCheckpoint *best = nullptr;
    for (const auto &cp : g_tas_rewind_points) {
        if (cp.valid && cp.branch_id == g_tas_current_branch && cp.frame <= target &&
            (!best || cp.frame > best->frame)) best = &cp;
    }

    uint64_t start = 0;
    if (best) {
        /* Do not let an older Strict Sync checkpoint or frame-step resume the
         * VM after this seek reaches its target. */
        TasCancelPendingTransportAdvance();
        xemu_tas_prepare_runtime();
        Error *err = NULL;
        bool was_running = false;
        const bool loaded = xemu_snapshots_load_paused(best->snapshot_name.c_str(),
                                                        &was_running, &err);
        (void)was_running;
        if (err || !loaded) {
            if (err) {
                xemu_queue_error_message(error_get_pretty(err));
                error_free(err);
            } else {
                xemu_queue_error_message("Could not restore TAS greenzone checkpoint");
            }
            return false;
        }
        start = best->frame;
        if ((best->transaction.hi || best->transaction.lo) &&
            !xemu_tas_transaction_matches(&best->transaction)) {
            xemu_queue_error_message(
                "TAS greenzone snapshot transaction ID mismatch; checkpoint was rejected");
            return false;
        }

        /* TAS frame metadata is feature-owned rather than part of ordinary VM
         * state, so restore it before checking the checkpoint fingerprint. */
        if (!xemu_tas_enabled()) xemu_tas_set_enabled(true);
        xemu_tas_set_frame(start);
        xemu_tas_set_lag_state(best->last_frame_lagged, best->lag_count,
                               best->lag_streak);
        uint64_t restored_hash = 0;
        if (best->state_hash &&
            (!TasComputeStateHash(&restored_hash) || restored_hash != best->state_hash)) {
            char msg[192];
            snprintf(msg, sizeof(msg),
                     "TAS checkpoint %llu failed Strict Sync validation; playback was not resumed",
                     (unsigned long long)start);
            xemu_queue_error_message(msg);
            return false;
        }
    } else {
        /* Greenzone is performance-only. If no cached anchor survives, restore
         * the authoritative canonical movie-start snapshot and reconstruct from
         * boundary 0. Cache settings may change seek cost, never correctness. */
        TasCancelPendingTransportAdvance();
        if (runstate_is_running()) vm_stop(RUN_STATE_PAUSED);
        if (!TasRestoreMovieStartSnapshot()) {
            xemu_queue_error_message(
                "Exact seek has no valid greenzone and the canonical movie-start state could not be restored");
            return false;
        }
        start = 0;
    }

    if (!xemu_tas_enabled()) xemu_tas_set_enabled(true);
    xemu_tas_set_frame(start);
    TasPushMovieToCore();
    if (start < target && start < g_tas_frames.size() &&
        !xemu_tas_start_playback(start)) {
        xemu_queue_error_message(
            "Could not start TAS playback while reconstructing the requested frame");
        return false;
    } else if (start >= target) {
        xemu_tas_stop_playback();
    }
    uint64_t delta = target - start;
    if (delta) {
        g_tas_seek_completion_pending = true;
        g_tas_seek_continue_target = target;
        g_tas_seek_continue_pending =
            continue_after_target && target < g_tas_frames.size();
        /* The restored checkpoint is only an internal reconstruction anchor.
         * Suppress its intermediate presentation frames so a <1 rewind or a
         * double-click looks like an exact jump to the requested frame rather
         * than visibly playing from the older checkpoint. Guest timing/input
         * remains ordinary TAS frame stepping. */
        xemu_tas_set_seek_catchup(true);
        xemu_tas_request_frame_advance((uint32_t)std::min<uint64_t>(delta, UINT32_MAX));
        vm_start();
    } else {
        xemu_tas_set_seek_catchup(false);
        if (continue_after_target && target < g_tas_frames.size()) {
            /* The requested boundary may itself be the restored checkpoint.
             * Install row N explicitly; boundary N means row N is still the
             * next input, including when N is the final movie row. */
            TasPushMovieToCore();
            if (!xemu_tas_start_playback(target)) {
                xemu_queue_error_message(
                    "Could not resume TAS playback from the exact seek boundary");
                return false;
            }
            if (!runstate_is_running()) vm_start();
        } else {
            xemu_tas_stop_playback();
            if (runstate_is_running()) vm_stop(RUN_STATE_PAUSED);
        }
    }
    const uint64_t selected_target = g_tas_frames.empty() ? 0 :
        std::min<uint64_t>(target, g_tas_frames.size() - 1);
    g_tas_selected_frame = (int)std::min<uint64_t>(selected_target, INT_MAX);
    g_tas_selection_anchor = g_tas_selected_frame;
    g_tas_selection_end = g_tas_selected_frame;
    /* Follow is a user preference. Seeking never toggles the checkbox. */
    return true;
}

static bool TasSeekFrame(uint64_t target)
{
    return TasSeekFrameEx(target, false);
}

static void TasStrictResimTick()
{
    if (!g_tas_strict_resim_pending) return;
    if (runstate_is_running()) return;

    const uint64_t target = g_tas_strict_resim_target;
    const bool continue_after = g_tas_strict_resim_continue;
    g_tas_strict_resim_pending = false;
    g_tas_strict_resim_continue = false;

    if (!TasSeekFrameEx(target, continue_after)) {
        xemu_queue_error_message(
            "Strict Sync could not rebuild the edited timeline; VM remains paused");
    }
}

static void TasSeekContinueTick()
{
    if (!g_tas_seek_completion_pending && !g_tas_seek_continue_pending) return;

    /* A user pause during hidden reconstruction is a cancellation, not a
     * reason to leave a half-finished seek armed forever. */
    if (g_tas_seek_completion_pending && !runstate_is_running() &&
        xemu_tas_frame_advance_remaining() != 0) {
        const uint64_t stopped = xemu_tas_frame();
        xemu_tas_cancel_frame_advance();
        xemu_tas_set_seek_catchup(false);
        g_tas_seek_completion_pending = false;
        g_tas_seek_continue_pending = false;
        if (g_tas_pending_overdub_start.active) {
            g_tas_pending_overdub_start = {};
        }
        if (g_tas_branch_switch.active && g_tas_branch_switch.reconstructing) {
            TasRollbackBranchSwitch(
                "TAS branch reconstruction was interrupted; restored previous branch");
        } else if (g_tas_timeline_mutation.active &&
                   g_tas_timeline_mutation.reconstructing) {
            TasAbortTimelineMutation(nullptr);
        }
        char msg[160];
        snprintf(msg, sizeof(msg),
                 "TAS exact seek interrupted at frame %llu",
                 (unsigned long long)stopped);
        xemu_queue_notification(msg);
        return;
    }

    /* Wait for the VBLANK transport to produce its requested pause. */
    if (xemu_tas_frame_advance_remaining() != 0 || runstate_is_running()) return;

    const uint64_t target = g_tas_seek_continue_target;
    const uint64_t vm_frame = xemu_tas_frame();
    const bool playing = xemu_tas_playback();
    const uint64_t movie_frame = playing ? xemu_tas_playback_frame() : vm_frame;

    xemu_tas_set_seek_catchup(false);

    if (g_tas_seek_completion_pending &&
        (vm_frame != target || movie_frame != target)) {
        char msg[224];
        snprintf(msg, sizeof(msg),
                 "TAS exact seek failed: requested frame %llu, VM stopped at %llu, movie cursor at %llu",
                 (unsigned long long)target,
                 (unsigned long long)vm_frame,
                 (unsigned long long)movie_frame);
        g_tas_seek_completion_pending = false;
        g_tas_seek_continue_pending = false;
        if (g_tas_pending_overdub_start.active) {
            g_tas_pending_overdub_start = {};
        }
        if (g_tas_branch_switch.active && g_tas_branch_switch.reconstructing) {
            TasRollbackBranchSwitch(
                "TAS branch reconstruction missed its exact target; restored previous branch");
        } else if (g_tas_timeline_mutation.active &&
                   g_tas_timeline_mutation.reconstructing) {
            TasAbortTimelineMutation(nullptr);
        }
        xemu_queue_error_message(msg);
        return;
    }

    g_tas_seek_completion_pending = false;

    /* Turn every successfully reached seek destination into a canonical exact
     * anchor. Repeated frame-by-frame work therefore becomes progressively
     * cheaper and never replaces the target with the checkpoint interval. */
    if (g_tas_rewind_enabled || g_tas_greenzone_enabled) {
        TasSaveRewindCheckpointAtBoundary(target);
    }

    if (g_tas_branch_switch.active && g_tas_branch_switch.reconstructing) {
        TasFinishBranchSwitch();
    } else if (g_tas_timeline_mutation.active &&
               g_tas_timeline_mutation.reconstructing) {
        TasRestoreTimelineRuntimeAfterMutation();
    } else if (g_tas_pending_overdub_start.active) {
        TasFinishPendingOverdubStart();
    }

    if (!g_tas_seek_continue_pending) return;

    g_tas_seek_continue_pending = false;
    if (!playing || g_tas_frames.empty() || target >= g_tas_frames.size()) {
        return;
    }

    /* Continue movie playback from the exact requested frame. Follow remains
     * whatever the user selected before/during the seek. */
    vm_start();
}

static void TasFrameStepCompletionTick()
{
    if (!g_tas_step_completion_pending) return;
    if (xemu_tas_frame_advance_remaining() != 0 || runstate_is_running()) return;

    const uint64_t actual = xemu_tas_frame();
    const uint64_t expected = g_tas_step_completion_target;
    g_tas_step_completion_pending = false;
    if (actual != expected) {
        char msg[192];
        snprintf(msg, sizeof(msg),
                 "TAS frame-step failed: requested stop at %llu, stopped at %llu",
                 (unsigned long long)expected,
                 (unsigned long long)actual);
        xemu_queue_error_message(msg);
        return;
    }

    if (!g_tas_frames.empty()) {
        const int selected = (int)std::min<uint64_t>(
            std::min<uint64_t>(actual, g_tas_frames.size() - 1), INT_MAX);
        g_tas_selected_frame = selected;
        g_tas_selection_anchor = selected;
        g_tas_selection_end = selected;
    }
}

static void TasRewindFrames(uint64_t distance)
{
    /* Never throw away input that is still buffered in the recording core just
     * because the user pressed a transport rewind button. */
    if (g_tas_overdub_ui_active || xemu_tas_overdub()) {
        TasSyncRecordingFromCore();
        TasStopOverdub();
    } else if (xemu_tas_recording()) {
        xemu_tas_stop_recording();
        TasSyncRecordingFromCore();
        TasAutosaveRecovery(true);
    }

    const uint64_t current = xemu_tas_playback()
        ? xemu_tas_playback_frame() : xemu_tas_frame();
    uint64_t target = current > distance ? current - distance : 0;
    TasSeekFrame(target);
}

static void TasSelectCurrentFrame()
{
    if (g_tas_frames.empty()) return;
    const uint64_t source = xemu_tas_playback() ? xemu_tas_playback_frame()
                                                 : xemu_tas_frame();
    const int frame = (int)std::min<uint64_t>(
        std::min<uint64_t>(source, g_tas_frames.size() - 1), INT_MAX);
    g_tas_selected_frame = frame;
    g_tas_selection_anchor = frame;
    g_tas_selection_end = frame;
    g_tas_follow_frame = false;
}

static void TasVerifierFail(const std::string &reason, uint64_t frame = UINT64_MAX,
                            uint64_t expected = 0, uint64_t actual = 0)
{
    xemu_tas_stop_playback();
    xemu_tas_stop_recording();
    xemu_tas_stop_overdub();
    g_tas_overdub_ui_active = false;
    TasCancelPendingTransportAdvance();
    g_tas_seek_continue_pending = false;
    g_tas_seek_completion_pending = false;
    g_tas_strict_resim_pending = false;
    if (runstate_is_running()) vm_stop(RUN_STATE_PAUSED);

    g_tas_verify_mode = TasVerifyMode::Idle;
    g_tas_verify_failed = true;
    g_tas_verify_first_bad_frame = frame;
    g_tas_verify_expected = expected;
    g_tas_verify_actual = actual;
    g_tas_verify_runs_remaining = 0;
    g_tas_verify_status = reason;
    xemu_queue_error_message(reason.c_str());
}

static bool TasRestoreVerifierStart()
{
    if (g_tas_verify_start_snapshot.empty() ||
        !TasSnapshotExists(g_tas_verify_start_snapshot.c_str())) {
        xemu_queue_error_message(
            "Verifier canonical start snapshot is unavailable; capture a new baseline");
        return false;
    }

    xemu_tas_prepare_runtime();
    Error *err = NULL;
    bool was_running = false;
    const bool loaded = xemu_snapshots_load_paused(
        g_tas_verify_start_snapshot.c_str(), &was_running, &err);
    (void)was_running;
    if (err || !loaded) {
        if (err) {
            xemu_queue_error_message(error_get_pretty(err));
            error_free(err);
        } else {
            xemu_queue_error_message("Could not restore verifier canonical start state");
        }
        return false;
    }
    if ((g_tas_verify_start_transaction.hi || g_tas_verify_start_transaction.lo) &&
        !xemu_tas_transaction_matches(&g_tas_verify_start_transaction)) {
        xemu_queue_error_message(
            "Verifier snapshot transaction ID mismatch; baseline was rejected");
        return false;
    }
    xemu_tas_set_frame(g_tas_verify_start_frame);
    /* Verifier baselines are captured from canonical movie boundary 0 after
     * fresh lag bookkeeping. Restore those feature-owned counters too. */
    if (g_tas_verify_start_frame == 0) {
        xemu_tas_reset_lag_counters();
    }
    return true;
}

static bool TasBeginVerifierPass()
{
    if (!TasRestoreVerifierStart()) return false;
    TasPushMovieToCore();
    xemu_tas_stop_playback();
    if (g_tas_verify_start_frame < g_tas_frames.size() &&
        !xemu_tas_start_playback(g_tas_verify_start_frame)) {
        xemu_queue_error_message("Could not start deterministic verifier playback");
        return false;
    }
    g_tas_verify_index = 0;
    g_tas_verify_next_frame = g_tas_verify_start_frame;
    return true;
}

static void TasStartVerifier(bool capture, int runs = 1, bool exhaustive = false)
{
    if (g_tas_frames.empty()) return;
    TasCancelPendingTransportAdvance();
    if (!xemu_tas_enabled()) xemu_tas_set_enabled(true);
    xemu_tas_set_deterministic_mode(true);
    if (runstate_is_running()) vm_stop(RUN_STATE_PAUSED);
    xemu_tas_stop_recording();
    xemu_tas_stop_overdub();
    g_tas_overdub_ui_active = false;
    xemu_tas_stop_playback();

    if (capture) {
        /* Poll traces are verifier evidence for this exact movie revision, not
         * user input.  Drop stale proof before the capture pass so the core
         * measures actual polls without comparing against an older run. */
        g_tas_poll_counts.assign(g_tas_frames.size(), TasUnknownPollCounts());
        g_tas_core_pushed_revision = UINT64_MAX;
        g_tas_verify_poll_sync_from = 0;

        /* A proof always starts from the movie's authoritative boundary 0,
         * never from an arbitrary current VM state. */
        if (!TasRestoreMovieStartSnapshot()) {
            xemu_queue_error_message(
                "Determinism baseline requires the canonical movie-start state");
            return;
        }
        xemu_tas_set_frame(0);

        uint32_t title_id = 0;
        if (!TasGetCurrentTitleId(&title_id)) {
            xemu_queue_error_message("Determinism verifier requires a running XBE/title ID");
            return;
        }
        char name[64];
        snprintf(name, sizeof(name), "%08X_TAS_VERIFY_BASE", title_id);

        XemuTasTransactionId transaction{};
        xemu_tas_transaction_mint(&transaction);
        Error *err = NULL;
        xemu_tas_transaction_snapshot_begin();
        xemu_snapshots_save_no_thumbnail(name, &err);
        xemu_tas_transaction_snapshot_end();
        if (err) {
            xemu_queue_error_message(error_get_pretty(err));
            error_free(err);
            return;
        }
        TasSnapshotCacheInsert(name);
        g_tas_verify_start_snapshot = name;
        g_tas_verify_start_transaction = transaction;
        g_tas_verify_start_frame = 0;
        g_tas_verify_branch = g_tas_current_branch;
        g_tas_verify_baseline.clear();
        g_tas_verify_revision = UINT64_MAX; /* proof becomes valid only at boundary N */
        g_tas_verify_exhaustive = exhaustive;
        g_tas_verify_baseline_interval = exhaustive ? 1 : std::max(1, g_tas_verify_interval);
        g_tas_verify_runs_total = 1;
        g_tas_verify_runs_remaining = 1;
        g_tas_verify_run_index = 1;
    } else {
        runs = std::clamp(runs, 1, 100);
        if (g_tas_current_branch != g_tas_verify_branch) {
            xemu_queue_error_message(
                "Verifier baseline belongs to a different movie branch. Switch to that branch first.");
            return;
        }
        if (g_tas_verify_revision != g_tas_movie_revision) {
            xemu_queue_error_message(
                "Verifier proof belongs to an older movie revision. Capture a new baseline after edits.");
            return;
        }
        const uint64_t end_boundary = g_tas_frames.size();
        if (g_tas_verify_baseline.empty() ||
            g_tas_verify_baseline.front().frame != 0 ||
            g_tas_verify_baseline.back().frame != end_boundary) {
            xemu_queue_error_message(
                "Verifier baseline does not include canonical boundary 0 through post-movie boundary N; recapture it");
            return;
        }
        g_tas_verify_runs_total = runs;
        g_tas_verify_runs_remaining = runs;
        g_tas_verify_run_index = 1;
    }

    g_tas_verify_mode = capture ? TasVerifyMode::Capture : TasVerifyMode::Verify;
    g_tas_verify_failed = false;
    g_tas_verify_first_bad_frame = UINT64_MAX;
    g_tas_verify_expected = 0;
    g_tas_verify_actual = 0;
    g_tas_verify_status = capture
        ? (exhaustive ? "Capturing exhaustive every-boundary baseline"
                      : "Capturing deterministic baseline")
        : "Verifier pass 1/" + std::to_string(g_tas_verify_runs_total);

    if (!TasBeginVerifierPass()) {
        g_tas_verify_mode = TasVerifyMode::Idle;
        g_tas_verify_runs_remaining = 0;
        g_tas_verify_status = "Could not establish verifier canonical start";
    }
}

static void TasVerifierTick()
{
    if (g_tas_verify_mode == TasVerifyMode::Idle || g_tas_frames.empty()) return;
    if (runstate_is_running()) return;

    const uint64_t end_boundary = g_tas_frames.size();
    const uint64_t frame = std::min<uint64_t>(xemu_tas_frame(), end_boundary);
    if (frame != g_tas_verify_next_frame) {
        char msg[224];
        snprintf(msg, sizeof(msg),
                 "Verifier transport mismatch: expected boundary %llu, stopped at %llu",
                 (unsigned long long)g_tas_verify_next_frame,
                 (unsigned long long)frame);
        TasVerifierFail(msg, frame);
        return;
    }

    if (g_tas_verify_mode == TasVerifyMode::Capture &&
        frame > g_tas_verify_poll_sync_from) {
        const uint64_t count = frame - g_tas_verify_poll_sync_from;
        std::vector<uint32_t> polls((size_t)count * XEMU_TAS_MAX_PORTS, 0);
        const uint64_t copied = xemu_tas_copy_playback_poll_trace(
            g_tas_verify_poll_sync_from, count, polls.data(), polls.size());
        if (copied != count) {
            TasVerifierFail("Verifier could not capture the complete XID poll trace",
                            frame, count, copied);
            return;
        }
        if (g_tas_poll_counts.size() < g_tas_frames.size()) {
            g_tas_poll_counts.resize(g_tas_frames.size(), TasUnknownPollCounts());
        }
        for (uint64_t i = 0; i < copied; ++i) {
            TasPollCounts pc{};
            for (int port = 0; port < XEMU_TAS_MAX_PORTS; ++port) {
                pc[(size_t)port] = polls[(size_t)i * XEMU_TAS_MAX_PORTS + port];
            }
            g_tas_poll_counts[(size_t)(g_tas_verify_poll_sync_from + i)] = pc;
        }
        g_tas_verify_poll_sync_from = frame;
    }

    uint64_t hash = 0;
    if (!TasComputeStateHash(&hash)) {
        TasVerifierFail("Verifier could not compute complete machine fingerprint", frame);
        return;
    }
    if (g_tas_verify_mode == TasVerifyMode::Capture) {
        g_tas_verify_baseline.push_back({frame, hash});
    } else {
        if (g_tas_verify_index >= g_tas_verify_baseline.size() ||
            g_tas_verify_baseline[g_tas_verify_index].frame != frame) {
            TasVerifierFail("Verifier baseline checkpoint sequence is invalid", frame);
            return;
        }
        const uint64_t expected = g_tas_verify_baseline[g_tas_verify_index].hash;
        if (expected != hash) {
            char msg[224];
            snprintf(msg, sizeof(msg),
                     "TAS DESYNC at boundary %llu: machine fingerprint %016llX != %016llX",
                     (unsigned long long)frame,
                     (unsigned long long)hash,
                     (unsigned long long)expected);
            TasVerifierFail(msg, frame, expected, hash);
            return;
        }
        ++g_tas_verify_index;
    }

    if (frame == end_boundary) {
        xemu_tas_stop_playback();
        if (g_tas_verify_mode == TasVerifyMode::Capture) {
            /* The baseline is proof for exactly this movie revision and branch.
             * It includes the state after final input row N-1 has executed. */
            g_tas_verify_revision = g_tas_movie_revision;
            /* Captured poll traces are synchronization evidence belonging to
             * this movie revision. Persist them and force the next playback
             * copy to carry the now-known expectations. */
            g_tas_core_pushed_revision = UINT64_MAX;
            g_tas_movie_dirty = true;
            g_tas_verify_runs_remaining = 0;
            g_tas_verify_status = g_tas_verify_exhaustive
                ? "Exhaustive every-boundary baseline captured"
                : "Deterministic baseline captured through post-movie boundary N";
            g_tas_verify_mode = TasVerifyMode::Idle;
            return;
        }

        --g_tas_verify_runs_remaining;
        if (g_tas_verify_runs_remaining <= 0) {
            g_tas_verify_status = "Verification passed x" +
                                  std::to_string(g_tas_verify_runs_total);
            g_tas_verify_mode = TasVerifyMode::Idle;
            return;
        }

        ++g_tas_verify_run_index;
        if (!TasBeginVerifierPass()) {
            TasVerifierFail("Could not restore canonical verifier start for stress pass");
            return;
        }
        g_tas_verify_status = "Verifier pass " + std::to_string(g_tas_verify_run_index) +
                              "/" + std::to_string(g_tas_verify_runs_total);
        return; /* hash boundary 0 on next paused maintenance tick */
    }

    uint64_t next = end_boundary;
    if (g_tas_verify_mode == TasVerifyMode::Capture) {
        next = std::min<uint64_t>(frame + (uint64_t)g_tas_verify_baseline_interval,
                                  end_boundary);
    } else {
        if (g_tas_verify_index >= g_tas_verify_baseline.size()) {
            TasVerifierFail("Verifier baseline ended before post-movie boundary N", frame);
            return;
        }
        next = g_tas_verify_baseline[g_tas_verify_index].frame;
        if (next <= frame || next > end_boundary) {
            TasVerifierFail("Verifier baseline contains a non-monotonic checkpoint sequence", frame);
            return;
        }
    }

    g_tas_verify_next_frame = next;
    const uint64_t delta = next - frame;
    xemu_tas_request_frame_advance((uint32_t)std::min<uint64_t>(delta, UINT32_MAX));
    vm_start();
}

static void TasCheckCoreDesync()
{
    XemuTasDesyncInfo info{};
    if (!xemu_tas_take_desync(&info) || !info.valid) return;

    char msg[256];
    snprintf(msg, sizeof(msg),
             "TAS DESYNC at frame %llu: XID polls P%u %u != %u",
             (unsigned long long)info.frame, (unsigned)info.port + 1,
             info.actual_polls, info.expected_polls);
    TasVerifierFail(msg, info.frame, info.expected_polls, info.actual_polls);
}


static void TasCopySelection()
{
    auto [a,b] = TasSelectionBounds();
    g_tas_clipboard_frames.assign(g_tas_frames.begin() + a, g_tas_frames.begin() + b + 1);
    g_tas_clipboard_lag.assign(g_tas_lag_flags.begin() + a, g_tas_lag_flags.begin() + b + 1);
    if (g_tas_poll_counts.size() < g_tas_frames.size()) {
        g_tas_poll_counts.resize(g_tas_frames.size(), TasUnknownPollCounts());
    }
    g_tas_clipboard_polls.assign(g_tas_poll_counts.begin() + a,
                                 g_tas_poll_counts.begin() + b + 1);
}

static void TasDeleteSelection()
{
    if (g_tas_frames.size() <= 1) return;
    auto [a,b] = TasSelectionBounds();
    TasPushTimelineUndo("Delete frame range", (uint64_t)a);
    g_tas_frames.erase(g_tas_frames.begin() + a, g_tas_frames.begin() + b + 1);
    if (b < (int)g_tas_lag_flags.size()) g_tas_lag_flags.erase(g_tas_lag_flags.begin() + a, g_tas_lag_flags.begin() + b + 1);
    if (b < (int)g_tas_poll_counts.size()) g_tas_poll_counts.erase(g_tas_poll_counts.begin() + a, g_tas_poll_counts.begin() + b + 1);
    if (g_tas_frames.empty()) {
        g_tas_frames.resize(1);
        g_tas_lag_flags.assign(1,0);
        g_tas_poll_counts.assign(1, TasUnknownPollCounts());
    }
    g_tas_selected_frame = std::min(a, (int)g_tas_frames.size() - 1);
    g_tas_selection_anchor = g_tas_selection_end = g_tas_selected_frame;
    TasMarkMovieEdited((uint64_t)a);
}

static void TasClearSelection()
{
    auto [a,b] = TasSelectionBounds();
    TasPushUndoRange("Clear frame range", a, b);
    for (int f = a; f <= b; ++f) g_tas_frames[f] = TasFrame{};
    TasMarkMovieEdited((uint64_t)a);
}

static void TasPasteSelection(bool insert)
{
    if (g_tas_clipboard_frames.empty()) return;
    int at = std::clamp(g_tas_selected_frame, 0, (int)g_tas_frames.size());
    TasPushTimelineUndo(insert ? "Paste insert" : "Paste overwrite", (uint64_t)at);
    if (insert) {
        g_tas_frames.insert(g_tas_frames.begin() + at, g_tas_clipboard_frames.begin(), g_tas_clipboard_frames.end());
        g_tas_lag_flags.insert(g_tas_lag_flags.begin() + at, g_tas_clipboard_lag.begin(), g_tas_clipboard_lag.end());
        if (g_tas_clipboard_polls.size() == g_tas_clipboard_frames.size()) {
            g_tas_poll_counts.insert(g_tas_poll_counts.begin() + at,
                                     g_tas_clipboard_polls.begin(),
                                     g_tas_clipboard_polls.end());
        } else {
            g_tas_poll_counts.insert(g_tas_poll_counts.begin() + at,
                                     g_tas_clipboard_frames.size(),
                                     TasUnknownPollCounts());
        }
    } else {
        size_t need = (size_t)at + g_tas_clipboard_frames.size();
        if (need > g_tas_frames.size()) {
            g_tas_frames.resize(need);
            g_tas_lag_flags.resize(need,0);
            g_tas_poll_counts.resize(need, TasUnknownPollCounts());
        }
        std::copy(g_tas_clipboard_frames.begin(), g_tas_clipboard_frames.end(), g_tas_frames.begin() + at);
        for (size_t i=0;i<g_tas_clipboard_lag.size();++i) g_tas_lag_flags[(size_t)at+i]=g_tas_clipboard_lag[i];
        if (g_tas_clipboard_polls.size() == g_tas_clipboard_frames.size()) {
            std::copy(g_tas_clipboard_polls.begin(), g_tas_clipboard_polls.end(),
                      g_tas_poll_counts.begin() + at);
        } else {
            std::fill(g_tas_poll_counts.begin() + at,
                      g_tas_poll_counts.begin() + at + g_tas_clipboard_frames.size(),
                      TasUnknownPollCounts());
        }
    }
    g_tas_selection_anchor = at;
    g_tas_selection_end = at + (int)g_tas_clipboard_frames.size() - 1;
    g_tas_selected_frame = g_tas_selection_end;
    TasMarkMovieEdited((uint64_t)at);
}

static void TasCloneSelection()
{
    TasCopySelection();
    auto [a,b] = TasSelectionBounds();
    g_tas_selected_frame = b + 1;
    TasPasteSelection(true);
}

static bool TasFrameInputsEqual(const TasFrame &a, const TasFrame &b)
{
    return !memcmp(&a, &b, sizeof(TasFrame));
}

static void TasFindFrame(int direction, int kind)
{
    /* kind: 0 input change, 1 lag, 2 chapter, 3 marker, 4 branch fork */
    if (g_tas_frames.empty()) return;
    int i = g_tas_selected_frame + direction;
    int last = (int)g_tas_frames.size() - 1;
    for (; i >= 0 && i <= last; i += direction) {
        bool match = false;
        if (kind == 0 && i > 0) match = !TasFrameInputsEqual(g_tas_frames[i], g_tas_frames[i-1]);
        else if (kind == 1) match = i < (int)g_tas_lag_flags.size() && g_tas_lag_flags[i];
        else if (kind == 2) match = std::any_of(g_tas_chapters.begin(), g_tas_chapters.end(), [i](const TasChapter &c){ return c.frame == (uint64_t)i; });
        else if (kind == 3) match = std::any_of(g_tas_markers.begin(), g_tas_markers.end(), [i](const TasMarker &m){ return m.frame == (uint64_t)i; });
        else if (kind == 4) match = std::any_of(g_tas_branches.begin(), g_tas_branches.end(), [i](const TasBranch &b){ return b.fork_frame == (uint64_t)i; });
        if (match) { TasSetSelection(i, false); return; }
    }
}

static double TasCurveT(double t, int type)
{
    t = std::clamp(t, 0.0, 1.0);
    switch (type) {
    case 1: return t*t;                       /* ease in */
    case 2: return 1.0 - (1.0-t)*(1.0-t);   /* ease out */
    case 3: return t*t*(3.0-2.0*t);         /* smoothstep */
    default: return t;
    }
}

static void TasApplyAnalogCurve()
{
    auto [a,b] = TasSelectionBounds();
    if (a == b) return;
    TasPushUndoRange("Analog curve", a, b);
    int off = g_tas_curve_control < 4 ? 12 + g_tas_curve_control * 2 : 4 + (g_tas_curve_control - 4);
    for (int f=a; f<=b; ++f) {
        double t = (double)(f-a)/(double)(b-a);
        double q = TasCurveT(t, g_tas_curve_type);
        int v = (int)std::llround((double)g_tas_curve_start_value +
                                  ((double)g_tas_curve_end_value-g_tas_curve_start_value)*q);
        if (g_tas_curve_control < 4) tas_write_s16(g_tas_frames[f].xid[g_tas_port], off, (int16_t)std::clamp(v,-32768,32767));
        else g_tas_frames[f].xid[g_tas_port][off] = (uint8_t)std::clamp(v,0,255);
    }
    TasMarkMovieEdited((uint64_t)a);
}

static void TasApplyStickCircle()
{
    auto [a,b] = TasSelectionBounds();
    if (a == b) return;
    TasPushUndoRange("Stick circle pattern", a, b);
    int ox = g_tas_circle_stick == 0 ? 12 : 16;
    int oy = ox + 2;
    int radius = std::clamp(g_tas_circle_radius, 0, 32767);
    for (int f=a; f<=b; ++f) {
        double t = (double)(f-a)/(double)(b-a);
        double ang = t * g_tas_circle_turns * 2.0 * 3.14159265358979323846;
        tas_write_s16(g_tas_frames[f].xid[g_tas_port], ox, (int16_t)std::llround(std::cos(ang)*radius));
        tas_write_s16(g_tas_frames[f].xid[g_tas_port], oy, (int16_t)std::llround(std::sin(ang)*radius));
    }
    TasMarkMovieEdited((uint64_t)a);
}

static bool TasSavePatternToPath(const char *path)
{
    if (!path || !*path) return false;
    std::filesystem::path p(path);
    if (p.extension() != ".xip") p += ".xip";
    auto [a,b] = TasSelectionBounds();
    FILE *f = qemu_fopen(p.string().c_str(), "wb");
    if (!f) return false;
    std::vector<TasFrame> frames(g_tas_frames.begin()+a, g_tas_frames.begin()+b+1);
    std::vector<uint8_t> lag(g_tas_lag_flags.begin()+a, g_tas_lag_flags.begin()+b+1);
    bool ok = fwrite("XIP1",1,4,f)==4 && TasWriteU32(f,1) && TasWriteU32(f,XEMU_TAS_MAX_PORTS) &&
              TasWriteU32(f,XEMU_TAS_XID_REPORT_SIZE) && TasWriteFrames(f,frames,lag);
    fclose(f);
    if (ok) xemu_queue_notification("Saved TAS input pattern");
    return ok;
}

static bool TasLoadPatternFromPath(const char *path)
{
    if (!path || !*path) return false;
    FILE *f=qemu_fopen(path,"rb"); if(!f) return false;
    char magic[4]; uint32_t ver=0,ports=0,size=0;
    std::vector<TasFrame> frames; std::vector<uint8_t> lag;
    bool ok=fread(magic,1,4,f)==4 && !memcmp(magic,"XIP1",4) && TasReadU32(f,&ver) && ver==1 &&
            TasReadU32(f,&ports) && ports==XEMU_TAS_MAX_PORTS && TasReadU32(f,&size) && size==XEMU_TAS_XID_REPORT_SIZE &&
            TasReadFrames(f,&frames,&lag);
    fclose(f); if(!ok) return false;
    g_tas_clipboard_frames=std::move(frames); g_tas_clipboard_lag=std::move(lag);
    TasPasteSelection(true);
    xemu_queue_notification("Inserted TAS input pattern");
    return true;
}

static void TasSavePatternDialog()
{
    static const SDL_DialogFileFilter filters[]={{"Xemu TAS Input Pattern (*.xip)","xip"},{"All Files","*"}};
    static std::string dir; dir=TasPatternDirectory();
    ShowSaveFileDialog(filters,2,dir.c_str(),[](const char *path){ TasSavePatternToPath(path); });
}

static void TasLoadPatternDialog()
{
    static const SDL_DialogFileFilter filters[]={{"Xemu TAS Input Pattern (*.xip)","xip"},{"All Files","*"}};
    static std::string dir; dir=TasPatternDirectory();
    ShowOpenFileDialog(filters,2,dir.c_str(),[](const char *path){ if(!TasLoadPatternFromPath(path)) xemu_queue_error_message("Invalid TAS input pattern"); });
}

static bool TasLoadMovieFramesOnly(const char *path, std::vector<TasFrame> *frames, std::vector<uint8_t> *lag)
{
    if (!path || !*path || !frames || !lag) return false;
    FILE *f=qemu_fopen(path,"rb"); if(!f) return false;
    char magic[4]; uint32_t version=0,title=0,flags=0,ports=0,report=0;
    bool ok=fread(magic,1,4,f)==4 && TasReadU32(f,&version) && TasReadU32(f,&title) && TasReadU32(f,&flags) &&
            TasReadU32(f,&ports) && TasReadU32(f,&report) && ports==XEMU_TAS_MAX_PORTS && report==XEMU_TAS_XID_REPORT_SIZE;
    if (ok && !memcmp(magic,"XMT1",4) && version==1) {
        uint64_t count=0; ok=TasReadU64(f,&count)&&count&&count<=10000000ULL;
        if(ok){ frames->assign((size_t)count,TasFrame{}); lag->assign((size_t)count,0); for(auto &fr:*frames) for(int p=0;p<4&&ok;++p) ok=fread(fr.xid[p].data(),1,20,f)==20; }
    } else if (ok && !memcmp(magic,"XMT2",4) && version==2) {
        uint64_t rerecord=0; uint32_t a=0,b=0,c=0; std::string tmp;
        ok=TasReadU64(f,&rerecord)&&TasReadU32(f,&a)&&TasReadU32(f,&b)&&TasReadU32(f,&c);
        for(int i=0;i<7&&ok;++i) ok=TasReadString(f,&tmp);
        for(int i=0;i<6&&ok;++i) ok=TasReadU32(f,&a);
        if(ok) ok=TasReadFrames(f,frames,lag);
    } else ok=false;
    fclose(f); return ok;
}

static void TasCompareWithPath(const char *path)
{
    std::vector<TasFrame> other; std::vector<uint8_t> otherlag;
    if (!TasLoadMovieFramesOnly(path,&other,&otherlag)) { xemu_queue_error_message("Could not load comparison .xmt"); return; }
    g_tas_compare_path=path; g_tas_compare_frames=std::move(other); g_tas_compare_lag=std::move(otherlag);
    g_tas_compare_first_diff=UINT64_MAX; g_tas_compare_diff_count=0;
    size_t n=std::max(g_tas_frames.size(),g_tas_compare_frames.size());
    for(size_t i=0;i<n;++i){
        bool diff=i>=g_tas_frames.size()||i>=g_tas_compare_frames.size();
        if(!diff) diff=!TasFrameInputsEqual(g_tas_frames[i],g_tas_compare_frames[i]) ||
                      ((i<g_tas_lag_flags.size()?g_tas_lag_flags[i]:0)!=(i<g_tas_compare_lag.size()?g_tas_compare_lag[i]:0));
        if(diff){ if(g_tas_compare_first_diff==UINT64_MAX) g_tas_compare_first_diff=i; ++g_tas_compare_diff_count; }
    }
    g_tas_compare_open=true;
}

static void TasCompareDialog()
{
    static const SDL_DialogFileFilter filters[]={{"Xemu TAS Movie (*.xmt)","xmt"},{"All Files","*"}};
    static std::string dir; dir=TasDefaultMovieDirectory();
    ShowOpenFileDialog(filters,2,dir.c_str(),[](const char *path){ TasCompareWithPath(path); });
}

static void TasExportCsvToPath(const char *path)
{
    if (!path || !*path) {
        return;
    }
    std::filesystem::path p(path);
    if (p.extension() != ".csv") {
        p += ".csv";
    }
    std::ofstream o(p); if(!o){xemu_queue_error_message("Could not create TAS CSV");return;}
    o<<"frame,lag"; for(int port=1;port<=4;++port)o<<",p"<<port<<"_xid_hex"; o<<"\n";
    for(size_t i=0;i<g_tas_frames.size();++i){ o<<i<<","<<(i<g_tas_lag_flags.size()?(int)g_tas_lag_flags[i]:0); for(int pidx=0;pidx<4;++pidx)o<<","<<TasHexReport(g_tas_frames[i].xid[pidx]); o<<"\n"; }
    xemu_queue_notification("Exported TAS movie CSV");
}

static void TasExportCsvDialog()
{
    static const SDL_DialogFileFilter filters[]={{"CSV (*.csv)","csv"},{"All Files","*"}}; static std::string dir; dir=TasDefaultMovieDirectory();
    ShowSaveFileDialog(filters,2,dir.c_str(),[](const char *p){TasExportCsvToPath(p);});
}

static bool TasParseHexReport(const std::string &hex, TasXidReport *out)
{
    if (!out || hex.size() != 40) {
        return false;
    }
    for (int i = 0; i < 20; ++i) {
        char tmp[3] = {hex[i * 2], hex[i * 2 + 1], 0};
        char *e = nullptr;
        long v = strtol(tmp, &e, 16);
        if (!e || *e) {
            return false;
        }
        (*out)[i] = (uint8_t)v;
    }
    return true;
}

static void TasImportCsvFromPath(const char *path)
{
    std::ifstream in(path?path:""); if(!in){xemu_queue_error_message("Could not open TAS CSV");return;}
    std::string line; std::getline(in,line); std::vector<TasFrame> frames; std::vector<uint8_t> lag;
    while(std::getline(in,line)){ if(line.empty())continue; std::stringstream ss(line); std::vector<std::string> f; std::string x; while(std::getline(ss,x,','))f.push_back(x); if(f.size()<6)continue; TasFrame fr{}; bool ok=true; for(int pidx=0;pidx<4;++pidx)ok&=TasParseHexReport(f[2+pidx],&fr.xid[pidx]); if(!ok)continue; frames.push_back(fr); lag.push_back((uint8_t)(atoi(f[1].c_str())?1:0)); }
    if(frames.empty()){xemu_queue_error_message("CSV contained no valid TAS frames");return;} TasPushTimelineUndo("Import CSV", 0); g_tas_frames=std::move(frames);g_tas_lag_flags=std::move(lag);g_tas_poll_counts.assign(g_tas_frames.size(),TasUnknownPollCounts());g_tas_selected_frame=0;g_tas_selection_anchor=g_tas_selection_end=0;TasMarkMovieEdited(0);xemu_queue_notification("Imported TAS CSV");
}

static void TasImportCsvDialog()
{
    static const SDL_DialogFileFilter filters[]={{"CSV (*.csv)","csv"},{"All Files","*"}}; static std::string dir; dir=TasDefaultMovieDirectory();
    ShowOpenFileDialog(filters,2,dir.c_str(),[](const char *p){TasImportCsvFromPath(p);});
}

static bool TasValidateProject(std::string *report)
{
    std::vector<std::string> problems;
    if (g_tas_frames.empty()) {
        problems.emplace_back("movie has no frames");
    }
    if (g_tas_lag_flags.size() != g_tas_frames.size()) {
        problems.emplace_back("lag flag count does not match movie frame count");
    }
    if (g_tas_poll_counts.size() != g_tas_frames.size()) {
        problems.emplace_back("XID poll-trace count does not match movie frame count");
    }
    if (!g_tas_frames.empty()) {
        const int last = (int)g_tas_frames.size() - 1;
        if (g_tas_selected_frame < 0 || g_tas_selected_frame > last) {
            problems.emplace_back("selected frame is outside movie bounds");
        }
        for (const TasChapter &c : g_tas_chapters) {
            if (c.frame >= g_tas_frames.size()) {
                problems.emplace_back("chapter is outside movie bounds: " + c.name);
                break;
            }
        }
        for (const TasMarker &m : g_tas_markers) {
            if (m.frame >= g_tas_frames.size()) {
                problems.emplace_back("marker is outside movie bounds: " + m.name);
                break;
            }
        }
    }

    std::unordered_set<uint32_t> branch_ids;
    branch_ids.emplace(g_tas_current_branch);
    for (const TasBranch &b : g_tas_branches) {
        if (!branch_ids.emplace(b.id).second) {
            problems.emplace_back("duplicate branch id " + std::to_string(b.id));
        }
        if (b.frames.empty()) {
            problems.emplace_back("branch " + std::to_string(b.id) + " has no frames");
        }
        if (b.lag.size() != b.frames.size()) {
            problems.emplace_back("branch " + std::to_string(b.id) +
                                  " lag/frame counts differ");
        }
        if (b.polls.size() != b.frames.size()) {
            problems.emplace_back("branch " + std::to_string(b.id) +
                                  " XID poll/frame counts differ");
        }
    }

    if (!g_tas_verify_baseline.empty()) {
        uint64_t prev = UINT64_MAX;
        for (const TasHashRecord &h : g_tas_verify_baseline) {
            if (prev != UINT64_MAX && h.frame <= prev) {
                problems.emplace_back("verifier baseline checkpoints are not strictly increasing");
                break;
            }
            prev = h.frame;
        }
        if (g_tas_verify_revision == g_tas_movie_revision &&
            g_tas_verify_branch == g_tas_current_branch) {
            if (g_tas_verify_baseline.front().frame != 0 ||
                g_tas_verify_baseline.back().frame != g_tas_frames.size()) {
                problems.emplace_back(
                    "current verifier proof does not span boundary 0 through post-movie boundary N");
            }
        }
    }

    if (report) {
        if (problems.empty()) {
            *report = "TAS project validation passed";
        } else {
            std::ostringstream ss;
            ss << "TAS project validation found " << problems.size() << " issue(s):";
            for (const std::string &p : problems) {
                ss << "\n - " << p;
            }
            *report = ss.str();
        }
    }
    return problems.empty();
}

static void TasRunProjectValidation()
{
    std::string report;
    if (TasValidateProject(&report)) {
        xemu_queue_notification(report.c_str());
    } else {
        xemu_queue_error_message(report.c_str());
    }
}

static std::vector<std::pair<std::string,bool>> TasCompatibilityChecks()
{
    std::vector<std::pair<std::string,bool>> c;
    if(!g_tas_loaded_environment.valid){ c.push_back({"No stored XMT2 environment manifest is loaded",false}); return c; }
    uint32_t tid=0; bool have=TasGetCurrentTitleId(&tid);
    c.push_back({"Xbox Title ID", have && tid==g_tas_loaded_environment.title_id});
    c.push_back({"Xemu commit", g_tas_loaded_environment.xemu_commit==(xemu_commit?xemu_commit:"")});
    char *disc=xemu_get_currently_loaded_disc_path(); std::string dp=disc?disc:""; if(disc)g_free(disc);
    c.push_back({"Disc path", dp==g_tas_loaded_environment.disc_path});
    c.push_back({"MCPX Boot ROM", g_tas_loaded_environment.bootrom_md5.empty()||TasFileMD5(g_config.sys.files.bootrom_path)==g_tas_loaded_environment.bootrom_md5});
    c.push_back({"BIOS", g_tas_loaded_environment.flashrom_md5.empty()||TasFileMD5(g_config.sys.files.flashrom_path)==g_tas_loaded_environment.flashrom_md5});
    c.push_back({"EEPROM", g_tas_loaded_environment.eeprom_md5.empty()||TasFileMD5(g_config.sys.files.eeprom_path)==g_tas_loaded_environment.eeprom_md5});
    c.push_back({"HDD base", g_tas_loaded_environment.hdd_path.empty()||(g_config.sys.files.hdd_path&&g_tas_loaded_environment.hdd_path==g_config.sys.files.hdd_path)});
    c.push_back({"Renderer", (uint32_t)g_config.display.renderer==g_tas_loaded_environment.renderer});
    c.push_back({"Internal resolution", (uint32_t)nv2a_get_surface_scale_factor()==g_tas_loaded_environment.surface_scale});
    c.push_back({"Display mode", (uint32_t)g_config.display.ui.fit==g_tas_loaded_environment.fit});
    c.push_back({"Filtering", (uint32_t)g_config.display.filtering==g_tas_loaded_environment.filtering});
    c.push_back({"Aspect ratio", (uint32_t)g_config.display.ui.aspect_ratio==g_tas_loaded_environment.aspect});
    return c;
}

static void TasRamSearchExact()
{
    g_tas_search_results.clear();
    g_tas_search_previous.clear();
    g_tas_search_results.reserve(65536);
    g_tas_search_previous.reserve(65536);
    uint64_t ram=xemu_guest_ram_size(); int sz=(g_tas_search_size==1||g_tas_search_size==2||g_tas_search_size==4)?g_tas_search_size:4;
    static std::vector<uint8_t> buf(1 << 20); constexpr size_t cap=1000000;
    for(uint64_t base=0;base<ram && g_tas_search_results.size()<cap;base+=buf.size()){
        size_t n=(size_t)std::min<uint64_t>(buf.size(),ram-base); ssize_t got=xemu_phys_read((uint32_t)base,buf.data(),n); if(got<=0)continue;
        for(size_t off=0;off+(size_t)sz<=(size_t)got;off+=sz){ uint64_t v=0;memcpy(&v,buf.data()+off,sz); uint64_t mask=sz==1?0xffu:sz==2?0xffffu:0xffffffffu; if((v&mask)==((uint64_t)g_tas_search_value&mask)){g_tas_search_results.push_back((uint32_t)(base+off));g_tas_search_previous.push_back(v&mask);if(g_tas_search_results.size()>=cap)break;} }
    }
}

static void TasRamSearchRefine(int mode)
{
    /* 0 changed, 1 unchanged, 2 increased, 3 decreased.
     *
     * Search results are produced in ascending physical-address order. The old
     * refinement path issued one dma_memory_read() per candidate, which can be
     * close to a million QEMU memory transactions after a broad 8-bit search.
     * Read each 1 MiB RAM window once and evaluate every candidate inside it.
     * This preserves the exact candidate/value semantics while turning the
     * worst case into ~64/128 bulk reads on retail/devkit RAM. */
    std::vector<uint32_t> keep;
    std::vector<uint64_t> keep_previous;
    keep.reserve(g_tas_search_results.size());
    keep_previous.reserve(g_tas_search_results.size());
    if (g_tas_search_results.empty()) {
        return;
    }

    const int sz = (g_tas_search_size == 1 || g_tas_search_size == 2 ||
                    g_tas_search_size == 4) ? g_tas_search_size : 4;
    static std::vector<uint8_t> buf(1 << 20);
    const uint64_t ram = xemu_guest_ram_size();
    const uint64_t chunk_mask = ~((uint64_t)buf.size() - 1);

    size_t i = 0;
    while (i < g_tas_search_results.size()) {
        const uint32_t first_addr = g_tas_search_results[i];
        const uint64_t base = (uint64_t)first_addr & chunk_mask;

        size_t end = i + 1;
        while (end < g_tas_search_results.size() &&
               (((uint64_t)g_tas_search_results[end] & chunk_mask) == base)) {
            ++end;
        }

        if (base >= ram) {
            i = end;
            continue;
        }
        const size_t want = (size_t)std::min<uint64_t>(buf.size(), ram - base);
        const ssize_t got = xemu_phys_read((uint32_t)base, buf.data(), want);

        if (got > 0) {
            for (; i < end; ++i) {
                const uint32_t addr = g_tas_search_results[i];
                const size_t off = (size_t)((uint64_t)addr - base);
                if (off + (size_t)sz > (size_t)got) {
                    continue;
                }

                uint64_t v = 0;
                memcpy(&v, buf.data() + off, (size_t)sz);
                const uint64_t mask = sz == 1 ? 0xffu :
                                      sz == 2 ? 0xffffu : 0xffffffffu;
                v &= mask;

                if (i >= g_tas_search_previous.size()) {
                    continue;
                }
                const uint64_t prev = g_tas_search_previous[i];
                const bool yes = mode == 0 ? v != prev :
                                 mode == 1 ? v == prev :
                                 mode == 2 ? v > prev : v < prev;
                if (yes) {
                    keep.push_back(addr);
                    keep_previous.push_back(v);
                }
            }
        } else {
            i = end;
        }
    }

    g_tas_search_results.swap(keep);
    g_tas_search_previous.swap(keep_previous);
}


static void TasDrawBranchTreeNode(uint32_t id, std::set<uint32_t> &visited)
{
    if (visited.count(id)) return;
    visited.insert(id);
    const TasBranch *branch = nullptr;
    for (const auto &b : g_tas_branches) if (b.id == id) { branch = &b; break; }
    bool current = id == g_tas_current_branch;
    std::string name = current ? "Current" : branch ? branch->name : "Branch";
    uint64_t fork = branch ? branch->fork_frame : (uint64_t)g_tas_selected_frame;
    char label[256]; snprintf(label,sizeof(label),"%s%u: %s  [fork %llu]##branch_tree_%u",current?"* ":"",id,name.c_str(),(unsigned long long)fork,id);
    bool has_children = false; for(const auto &b:g_tas_branches) if(b.parent==id){has_children=true;break;}
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_DefaultOpen | (has_children?0:ImGuiTreeNodeFlags_Leaf);
    bool open=ImGui::TreeNodeEx(label,flags);
    if(open){for(const auto &b:g_tas_branches)if(b.parent==id)TasDrawBranchTreeNode(b.id,visited);ImGui::TreePop();}
}


static void DrawTasStudio()
{
    static constexpr const char *kDetachId = "tas.studio";
    xemu_feature_detach::Register(kDetachId, "TAS Studio - XMT Piano Roll",
                                  &g_tas_studio_open, DrawTasStudio);
    // TAS Studio is the first custom-tool draw in Xemu's normal frame order.
    // Pumping here keeps already-detached windows responsive even when the
    // Studio itself is closed; later pumps are frame-deduplicated.
    xemu_feature_detach::Pump();
    if (!g_tas_studio_open || !xemu_feature_detach::ShouldDraw(kDetachId)) return;
    if (xemu_feature_detach::IsDetachedPass(kDetachId)) {
        xemu_feature_detach::PrepareWindow(kDetachId);
    } else {
        ImGui::SetNextWindowSize(ImVec2(1180, 640), ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("TAS Studio - XMT Piano Roll", &g_tas_studio_open,
                      xemu_feature_detach::WindowFlags(kDetachId, ImGuiWindowFlags_MenuBar))) {
        ImGui::End();
        return;
    }
    xemu_feature_detach::ObserveCurrentWindow(kDetachId);

    if (g_tas_frames.empty()) {
        g_tas_frames.resize(1);
        g_tas_lag_flags.resize(1, 0);
    }
    if (g_tas_lag_flags.size() < g_tas_frames.size()) {
        g_tas_lag_flags.resize(g_tas_frames.size(), 0);
    }
    if (g_tas_poll_counts.size() < g_tas_frames.size()) {
        g_tas_poll_counts.resize(g_tas_frames.size(), TasUnknownPollCounts());
    }

    const uint64_t tas_frame = xemu_tas_frame();
    static bool scroll_to_followed_frame = false;
    if (xemu_tas_enabled() && g_tas_follow_frame && !xemu_tas_seek_catchup()) {
        const uint64_t last_movie_frame = g_tas_frames.size() - 1;
        const uint64_t source_frame = xemu_tas_playback() ? xemu_tas_playback_frame() : tas_frame;
        const int followed = (int)std::min<uint64_t>(
            std::min<uint64_t>(source_frame, last_movie_frame), INT_MAX);
        if (followed != g_tas_selected_frame) {
            g_tas_selected_frame = followed;
            g_tas_selection_anchor = followed;
            g_tas_selection_end = followed;
            scroll_to_followed_frame = true;
        }
    }

    std::string movie_name = "Untitled.xmt";
    if (!g_tas_movie_path.empty()) {
        try {
            movie_name = std::filesystem::path(g_tas_movie_path).filename().string();
        } catch (...) {
            movie_name = g_tas_movie_path;
        }
    }
    if (ImGui::BeginMenuBar()) {
        if (ImGui::BeginMenu("Movie")) {
            if (ImGui::MenuItem("New Movie")) TasNewMovie();
            if (ImGui::MenuItem("Open Movie...")) TasOpenMovie();
            if (ImGui::MenuItem("Save Movie")) TasSaveMovie();
            if (ImGui::MenuItem("Save Movie As...")) TasSaveMovieAs();
            ImGui::Separator();
            if (!xemu_tas_recording()) {
                if (ImGui::MenuItem("Record / Rerecord From Current Position")) TasStartContextualRecording();
                if (ImGui::MenuItem("New Movie From Current State")) TasStartFreshRecordingFromCurrentState();
                if (ImGui::MenuItem("Record From Power-On / Reset")) TasStartRecording(true);
            } else if (ImGui::MenuItem("Stop Recording")) {
                xemu_tas_stop_recording();
                TasSyncRecordingFromCore();
                TasAutosaveRecovery(true);
            }
            if (ImGui::MenuItem("Play Movie From Beginning")) {
                TasPlayMovieFromBeginning();
            }
            if (!xemu_tas_playback()) {
                if (ImGui::MenuItem("Play From Selected Frame"))
                    TasStartPlayback((uint64_t)g_tas_selected_frame);
            } else if (ImGui::MenuItem("Stop Playback")) {
                TasStopPlaybackFromUi();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Movie Properties / Comments"))
                g_tas_properties_open = true;
            if (ImGui::MenuItem("Compatibility..."))
                g_tas_compatibility_open = true;
            if (ImGui::MenuItem("Backup History..."))
                g_tas_history_open = true;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Edit")) {
            const bool can_undo_menu = !g_tas_undo_stack.empty();
            const bool can_redo_menu = !g_tas_redo_stack.empty();
            const bool have_clip_menu = !g_tas_clipboard_frames.empty();
            if (ImGui::MenuItem("Undo", nullptr, false, can_undo_menu)) TasUndoEdit();
            if (ImGui::MenuItem("Redo", nullptr, false, can_redo_menu)) TasRedoEdit();
            ImGui::Separator();
            if (ImGui::MenuItem("Copy Range")) TasCopySelection();
            if (ImGui::MenuItem("Paste Insert", nullptr, false, have_clip_menu)) TasPasteSelection(true);
            if (ImGui::MenuItem("Paste Overwrite", nullptr, false, have_clip_menu)) TasPasteSelection(false);
            if (ImGui::MenuItem("Clone Range")) TasCloneSelection();
            if (ImGui::MenuItem("Delete Range")) TasDeleteSelection();
            if (ImGui::MenuItem("Clear Range")) TasClearSelection();
            ImGui::Separator();
            if (ImGui::MenuItem("Insert Frame")) {
                int at = std::clamp(g_tas_selected_frame, 0, (int)g_tas_frames.size());
                TasPushTimelineUndo("Insert frame", (uint64_t)at);
                g_tas_frames.insert(g_tas_frames.begin() + at, TasFrame{});
                g_tas_lag_flags.insert(g_tas_lag_flags.begin() + at, 0);
                g_tas_poll_counts.insert(g_tas_poll_counts.begin() + at, TasUnknownPollCounts());
                TasSetSelection(at, false);
                TasMarkMovieEdited((uint64_t)at);
            }
            if (ImGui::MenuItem("Append 60 Blank Frames")) {
                size_t old = g_tas_frames.size();
                TasPushTimelineUndo("Append 60 blank frames", (uint64_t)old);
                g_tas_frames.resize(old + 60);
                g_tas_lag_flags.resize(g_tas_frames.size(), 0);
                g_tas_poll_counts.resize(g_tas_frames.size(), TasUnknownPollCounts());
                TasMarkMovieEdited((uint64_t)old);
            }
            if (ImGui::MenuItem("Analog Curves...")) g_tas_curve_open = true;
            ImGui::Separator();
            if (ImGui::MenuItem("Save Input Pattern...")) TasSavePatternDialog();
            if (ImGui::MenuItem("Insert Input Pattern...")) TasLoadPatternDialog();
            if (ImGui::MenuItem("Compare Movie...")) TasCompareDialog();
            if (ImGui::MenuItem("Export CSV...")) TasExportCsvDialog();
            if (ImGui::MenuItem("Import CSV...")) TasImportCsvDialog();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Navigate")) {
            if (ImGui::MenuItem("Rewind 1 Frame")) TasRewindFrames(1);
            if (ImGui::MenuItem("Rewind 10 Frames")) TasRewindFrames(10);
            if (ImGui::MenuItem("Rewind 60 Frames")) TasRewindFrames(60);
            if (ImGui::MenuItem("Rewind 300 Frames")) TasRewindFrames(300);
            ImGui::Separator();
            if (ImGui::MenuItem("Advance 1 Frame")) TasAdvanceFrames(1);
            if (ImGui::MenuItem("Advance 10 Frames")) TasAdvanceFrames(10);
            if (ImGui::MenuItem("Advance 60 Frames")) TasAdvanceFrames(60);
            if (ImGui::MenuItem("Advance 300 Frames")) TasAdvanceFrames(300);
            ImGui::Separator();
            if (ImGui::MenuItem("Select Current TAS Frame")) TasSelectCurrentFrame();
            if (ImGui::MenuItem("Seek To Selected (Greenzone)"))
                TasSeekFrame((uint64_t)g_tas_selected_frame);
            if (ImGui::MenuItem("Play Movie From Beginning")) TasPlayMovieFromBeginning();
            if (ImGui::BeginMenu("Double-Click Frame Behavior")) {
                if (ImGui::MenuItem("Seek && Stop at Frame", nullptr,
                                    !g_tas_double_click_continue))
                    g_tas_double_click_continue = false;
                if (ImGui::MenuItem("Seek && Continue Playback", nullptr,
                                    g_tas_double_click_continue))
                    g_tas_double_click_continue = true;
                ImGui::EndMenu();
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Previous Input Change")) TasFindFrame(-1, 0);
            if (ImGui::MenuItem("Next Input Change")) TasFindFrame(1, 0);
            if (ImGui::MenuItem("Previous Lag Frame")) TasFindFrame(-1, 1);
            if (ImGui::MenuItem("Next Lag Frame")) TasFindFrame(1, 1);
            if (ImGui::MenuItem("Previous Chapter")) TasFindFrame(-1, 2);
            if (ImGui::MenuItem("Next Chapter")) TasFindFrame(1, 2);
            ImGui::Separator();
            if (ImGui::MenuItem("Go To Frame...")) {
                g_tas_goto_frame_value = g_tas_selected_frame;
                g_tas_goto_dialog_requested = true;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tools")) {
            ImGui::MenuItem("Auto-Hold / Autofire", nullptr, &g_tas_automation_open);
            ImGui::MenuItem("Combo / Macro", nullptr, &g_tas_macro_open);
            ImGui::MenuItem("Chapters", nullptr, &g_tas_chapters_open);
            ImGui::MenuItem("Markers / Notes", nullptr, &g_tas_markers_open);
            ImGui::MenuItem("Branches", nullptr, &g_tas_branches_open);
            ImGui::Separator();
            ImGui::MenuItem("Greenzone / Timeline Cache", nullptr, &g_tas_greenzone_panel_open);
            ImGui::MenuItem("Determinism Verifier", nullptr, &g_tas_verifier_panel_open);
            ImGui::MenuItem("Punch-In / Overdub Fields", nullptr, &g_tas_punch_panel_open);
            ImGui::MenuItem("Rewind / Recovery", nullptr, &g_tas_rewind_panel_open);
            ImGui::Separator();
            if (ImGui::MenuItem("RAM Watch / Search / RNG...")) g_tas_ram_tools_open = true;
            if (ImGui::MenuItem("Validate TAS Project")) TasRunProjectValidation();
            if (ImGui::MenuItem("Recover Autosave")) TasRecoverAutosave();
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("View")) {
            ImGui::MenuItem("Compact Layout", nullptr, &g_tas_compact_ui);
            ImGui::MenuItem("Exact Input Inspector", nullptr, &g_tas_exact_editor_open);
            ImGui::MenuItem("All 4 Controllers", nullptr, &g_tas_all_ports_view);
            ImGui::Separator();
            ImGui::MenuItem("Follow TAS Frame", nullptr, &g_tas_follow_frame);
            ImGui::MenuItem("Read-only", nullptr, &g_tas_read_only);
            bool det_menu = xemu_tas_deterministic_mode();
            if (ImGui::MenuItem("Deterministic (experimental)", nullptr, det_menu))
                xemu_tas_set_deterministic_mode(!det_menu);
            ImGui::Separator();
            ImGui::MenuItem("Dim Lag Frames", nullptr, &g_tas_dim_lag_frames);
            if (ImGui::MenuItem("Hide Lag Frames", nullptr, &g_tas_hide_lag_frames)) {
                g_tas_visible_frame_cache_dirty = true;
            }
            ImGui::MenuItem("TAS HUD", nullptr, &g_tas_hud_enabled);
            ImGui::EndMenu();
        }
        ImGui::EndMenuBar();
    }

    if (g_tas_compact_ui) {
        ImGui::Text("%s%s   F:%llu/%zu   Lag:%llu (%llu)   RR:%llu   Rev:%llu",
                    movie_name.c_str(), g_tas_movie_dirty ? " *" : "",
                    (unsigned long long)tas_frame, g_tas_frames.size(),
                    (unsigned long long)xemu_tas_lag_count(),
                    (unsigned long long)xemu_tas_lag_streak(),
                    (unsigned long long)g_tas_rerecord_count,
                    (unsigned long long)g_tas_movie_revision);
        ImGui::SameLine();
        if (g_tas_read_only)
            ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f), "READ ONLY");
        else if (xemu_tas_recording())
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "RECORDING");
        else if (xemu_tas_playback())
            ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.45f, 1.0f), "PLAYBACK");

        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));

        /* Session row: keep the high-frequency movie controls and savestate
         * workflow at the top. Dynamic labels keep fixed slots, so recording
         * or playback state changes never push neighboring buttons around. */
        const bool running_compact = runstate_is_running();
        if (ImGui::Button(running_compact ? "Pause###tas_run_c" : "Resume###tas_run_c",
                          ImVec2(64.0f, 0))) ActionTogglePause();
        ImGui::SameLine();
        if (!xemu_tas_recording()) {
            if (ImGui::Button("Record###tas_record_c", ImVec2(72.0f, 0)))
                TasStartContextualRecording();
            if (ImGui::IsItemHovered())
                ImGui::SetTooltip("Record/rerecord from the exact current movie/emulation position. Never resets the Xbox.");
        } else if (ImGui::Button("Stop Rec###tas_record_c", ImVec2(72.0f, 0))) {
            xemu_tas_stop_recording();
            TasSyncRecordingFromCore();
            TasAutosaveRecovery(true);
        }
        ImGui::SameLine();
        if (!xemu_tas_playback()) {
            if (ImGui::Button("Play###tas_play_c", ImVec2(72.0f, 0)))
                TasStartPlayback((uint64_t)g_tas_selected_frame);
        } else if (ImGui::Button("Stop Play###tas_play_c", ImVec2(72.0f, 0))) {
            TasStopPlaybackFromUi();
        }
        ImGui::SameLine();
        if (ImGui::Button("Play Start", ImVec2(76.0f, 0))) TasPlayMovieFromBeginning();
        ImGui::SameLine();
        if (ImGui::Button("Save Movie", ImVec2(82.0f, 0))) TasSaveMovie();
        ImGui::SameLine(); ImGui::Checkbox("Follow", &g_tas_follow_frame);
        ImGui::SameLine(); ImGui::Checkbox("RO", &g_tas_read_only);
        ImGui::SameLine();
        bool det_compact = xemu_tas_deterministic_mode();
        if (ImGui::Checkbox("Det", &det_compact)) xemu_tas_set_deterministic_mode(det_compact);

        /* Savestates belong directly under Pause/Record/Play, not below
         * navigation. This row is stable regardless of transport mode. */
        ImGui::TextUnformatted("State"); ImGui::SameLine();
        const bool compact_prev_disabled = g_tas_state_slot == 0;
        if (compact_prev_disabled) ImGui::BeginDisabled();
        if (ImGui::Button("<##tas_state_prev_c")) --g_tas_state_slot;
        if (compact_prev_disabled) ImGui::EndDisabled();
        ImGui::SameLine();
        static std::array<bool, 100> compact_state_slots{};
        char compact_slot_preview[8];
        snprintf(compact_slot_preview, sizeof(compact_slot_preview), "%02d", g_tas_state_slot);
        ImGui::SetNextItemWidth(58.0f);
        if (ImGui::BeginCombo("##tas_state_slot_c", compact_slot_preview)) {
            TasGetStateSlotStatus(compact_state_slots);
            for (int slot = 0; slot < 100; ++slot) {
                char label[32];
                snprintf(label, sizeof(label), "%02d%s", slot,
                         compact_state_slots[slot] ? " [saved]" : "");
                if (ImGui::Selectable(label, slot == g_tas_state_slot))
                    g_tas_state_slot = slot;
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        const bool compact_next_disabled = g_tas_state_slot == 99;
        if (compact_next_disabled) ImGui::BeginDisabled();
        if (ImGui::Button(">##tas_state_next_c")) ++g_tas_state_slot;
        if (compact_next_disabled) ImGui::EndDisabled();
        ImGui::SameLine();
        char compact_state_name[64];
        const bool compact_have_title = TasBuildStateName(compact_state_name, sizeof(compact_state_name));
        if (!compact_have_title) ImGui::BeginDisabled();
        if (ImGui::Button("Save State", ImVec2(82.0f, 0))) TasSaveSelectedState();
        ImGui::SameLine(); if (ImGui::Button("Load", ImVec2(54.0f, 0))) TasLoadSelectedState();
        ImGui::SameLine();
        if (ImGui::Button("Resume Rec", ImVec2(82.0f, 0))) {
            g_tas_read_only = false;
            TasLoadSelectedState();
        }
        if (!compact_have_title) ImGui::EndDisabled();
        ImGui::SameLine(); if (ImGui::Button("Undo Load", ImVec2(78.0f, 0))) TasUndoStateLoad();
        ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
        const char *compact_ports[] = {"P1", "P2", "P3", "P4"};
        ImGui::SetNextItemWidth(54.0f);
        ImGui::Combo("##tas_port_c", &g_tas_port, compact_ports, 4);
        ImGui::SameLine(); if (ImGui::Button("Capture", ImVec2(66.0f, 0))) TasCaptureCurrentFrame();
        ImGui::SameLine(); if (ImGui::Button("Apply", ImVec2(54.0f, 0))) TasApplySelectedFrame();
        ImGui::SameLine(); if (ImGui::Button("Release", ImVec2(62.0f, 0))) xemu_tas_clear_xid_report((uint8_t)g_tas_port);

        /* Symmetric transport pairs. Left is rewind, right is advance. The
         * labels intentionally read like a timeline rather than +/- math. */
        ImGui::TextDisabled("Frames"); ImGui::SameLine();
        if (ImGui::Button("< 1##tas_rw1_c", ImVec2(42.0f, 0))) TasRewindFrames(1);
        ImGui::SameLine(); if (ImGui::Button("1 >##tas_fw1_c", ImVec2(42.0f, 0))) TasAdvanceFrames(1);
        ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
        if (ImGui::Button("< 10##tas_rw10_c", ImVec2(48.0f, 0))) TasRewindFrames(10);
        ImGui::SameLine(); if (ImGui::Button("10 >##tas_fw10_c", ImVec2(48.0f, 0))) TasAdvanceFrames(10);
        ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
        if (ImGui::Button("< 60##tas_rw60_c", ImVec2(48.0f, 0))) TasRewindFrames(60);
        ImGui::SameLine(); if (ImGui::Button("60 >##tas_fw60_c", ImVec2(48.0f, 0))) TasAdvanceFrames(60);
        ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
        if (ImGui::Button("< 300##tas_rw300_c", ImVec2(56.0f, 0))) TasRewindFrames(300);
        ImGui::SameLine(); if (ImGui::Button("300 >##tas_fw300_c", ImVec2(56.0f, 0))) TasAdvanceFrames(300);
        ImGui::SameLine();
        if (ImGui::Button("1 > Skip Lag##tas_skip_c", ImVec2(88.0f, 0))) TasAdvanceFrames(1, true);

        ImGui::TextDisabled("Seek"); ImGui::SameLine();
        if (ImGui::Button("Select Current##tas_c", ImVec2(98.0f, 0))) TasSelectCurrentFrame();
        ImGui::SameLine();
        if (ImGui::Button("Seek Selected##tas_c", ImVec2(96.0f, 0)))
            TasSeekFrame((uint64_t)g_tas_selected_frame);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Restore an internal checkpoint, reconstruct invisibly, and land on the exact selected frame");
        ImGui::SameLine();
        ImGui::Checkbox("Double-click continues", &g_tas_double_click_continue);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Off: land exactly on the double-clicked frame and remain paused. On: land exactly there, then continue playback. This never changes Follow.");

        ImGui::PopStyleVar();
    } else {
    ImGui::Text("Movie: %s%s   TAS frame: %llu   Movie frames: %zu   Lag: %llu   Rerecords: %llu   Rev: %llu",
                movie_name.c_str(), g_tas_movie_dirty ? " *" : "",
                (unsigned long long)tas_frame, g_tas_frames.size(),
                (unsigned long long)xemu_tas_lag_count(),
                (unsigned long long)g_tas_rerecord_count,
                (unsigned long long)g_tas_movie_revision);
    ImGui::SameLine();
    ImGui::Checkbox("Follow TAS frame", &g_tas_follow_frame);

    bool deterministic_mode = xemu_tas_deterministic_mode();
    ImGui::SameLine();
    if (ImGui::Checkbox("Strict Sync / Deterministic", &deterministic_mode)) {
        xemu_tas_set_deterministic_mode(deterministic_mode);
    }
    ImGui::SameLine();
    ImGui::Checkbox("Read-only", &g_tas_read_only);
    ImGui::SameLine();
    if (g_tas_read_only) {
        ImGui::TextColored(ImVec4(0.35f, 0.85f, 1.0f, 1.0f), "READ ONLY");
    } else if (xemu_tas_recording()) {
        ImGui::TextColored(ImVec4(1.0f, 0.35f, 0.25f, 1.0f), "RECORDING");
    } else if (xemu_tas_playback()) {
        ImGui::TextColored(ImVec4(0.45f, 1.0f, 0.45f, 1.0f), "PLAYBACK");
    } else {
        ImGui::TextDisabled("EDITABLE");
    }

    /* Primary session controls. Keep dynamic labels in fixed slots. */
    const bool running = runstate_is_running();
    const bool recording_now = xemu_tas_recording();
    const bool overdub_now = g_tas_overdub_ui_active || xemu_tas_overdub();
    const bool playback_now = xemu_tas_playback();

    if (ImGui::Button(running ? "Pause###tas_run" : "Resume###tas_run", ImVec2(78.0f, 0)))
        ActionTogglePause();
    ImGui::SameLine();
    if (!recording_now) {
        if (ImGui::Button("Record###tas_record", ImVec2(94.0f, 0))) TasStartContextualRecording();
    } else if (ImGui::Button("Stop Recording###tas_record", ImVec2(94.0f, 0))) {
        xemu_tas_stop_recording();
        TasSyncRecordingFromCore();
        TasAutosaveRecovery(true);
    }
    ImGui::SameLine();
    if (overdub_now) ImGui::BeginDisabled();
    if (!playback_now || overdub_now) {
        if (ImGui::Button("Play###tas_play", ImVec2(94.0f, 0)) && !overdub_now)
            TasStartPlayback((uint64_t)g_tas_selected_frame);
    } else if (ImGui::Button("Stop Playback###tas_play", ImVec2(94.0f, 0))) {
        TasStopPlaybackFromUi();
    }
    if (overdub_now) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Play Beginning", ImVec2(112.0f, 0))) TasPlayMovieFromBeginning();
    ImGui::SameLine();
    if (ImGui::Button("Save Movie", ImVec2(90.0f, 0))) TasSaveMovie();

    /* Savestate workflow stays next to the primary playback/record controls. */
    ImGui::TextUnformatted("State"); ImGui::SameLine();
    const bool prev_state_disabled = g_tas_state_slot == 0;
    if (prev_state_disabled) ImGui::BeginDisabled();
    if (ImGui::Button("<##tas_state_prev")) --g_tas_state_slot;
    if (prev_state_disabled) ImGui::EndDisabled();
    ImGui::SameLine();
    static std::array<bool, 100> state_slots{};
    char slot_preview[16];
    snprintf(slot_preview, sizeof(slot_preview), "Slot %02d", g_tas_state_slot);
    ImGui::SetNextItemWidth(86.0f);
    if (ImGui::BeginCombo("##tas_state_slot", slot_preview)) {
        TasGetStateSlotStatus(state_slots);
        for (int slot = 0; slot < 100; ++slot) {
            char slot_label[32];
            snprintf(slot_label, sizeof(slot_label), "%02d%s", slot,
                     state_slots[slot] ? "   [saved]" : "");
            const bool selected = slot == g_tas_state_slot;
            if (ImGui::Selectable(slot_label, selected)) g_tas_state_slot = slot;
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    ImGui::SameLine();
    const bool next_state_disabled = g_tas_state_slot == 99;
    if (next_state_disabled) ImGui::BeginDisabled();
    if (ImGui::Button(">##tas_state_next")) ++g_tas_state_slot;
    if (next_state_disabled) ImGui::EndDisabled();
    ImGui::SameLine();
    char tas_state_name[64];
    const bool have_title_id = TasBuildStateName(tas_state_name, sizeof(tas_state_name));
    if (!have_title_id) {
        snprintf(tas_state_name, sizeof(tas_state_name), "--------_TAS_%02d", g_tas_state_slot);
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Save State", ImVec2(88.0f, 0))) TasSaveSelectedState();
    ImGui::SameLine(); if (ImGui::Button("Load State", ImVec2(86.0f, 0))) TasLoadSelectedState();
    ImGui::SameLine();
    if (ImGui::Button("Resume Recording", ImVec2(126.0f, 0))) {
        g_tas_read_only = false;
        TasLoadSelectedState();
    }
    if (!have_title_id) ImGui::EndDisabled();
    ImGui::SameLine(); if (ImGui::Button("Undo Load", ImVec2(82.0f, 0))) TasUndoStateLoad();

    /* Symmetric exact-frame transport. Checkpoint spacing is not transport
     * spacing: rewind restores an internal anchor and reconstructs invisibly to
     * the exact target. */
    ImGui::TextDisabled("Frames"); ImGui::SameLine();
    if (ImGui::Button("< 1##tas_rw1", ImVec2(46.0f, 0))) TasRewindFrames(1);
    ImGui::SameLine(); if (ImGui::Button("1 >##tas_fw1", ImVec2(46.0f, 0))) TasAdvanceFrames(1);
    ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
    if (ImGui::Button("< 10##tas_rw10", ImVec2(52.0f, 0))) TasRewindFrames(10);
    ImGui::SameLine(); if (ImGui::Button("10 >##tas_fw10", ImVec2(52.0f, 0))) TasAdvanceFrames(10);
    ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
    if (ImGui::Button("< 60##tas_rw60", ImVec2(52.0f, 0))) TasRewindFrames(60);
    ImGui::SameLine(); if (ImGui::Button("60 >##tas_fw60", ImVec2(52.0f, 0))) TasAdvanceFrames(60);
    ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
    if (ImGui::Button("< 300##tas_rw300", ImVec2(60.0f, 0))) TasRewindFrames(300);
    ImGui::SameLine(); if (ImGui::Button("300 >##tas_fw300", ImVec2(60.0f, 0))) TasAdvanceFrames(300);
    ImGui::SameLine(); if (ImGui::Button("1 > Skip Lag", ImVec2(92.0f, 0))) TasAdvanceFrames(1, true);
    ImGui::SameLine();
    ImGui::TextDisabled("Last: %s (%llu)",
                        xemu_tas_last_frame_lagged() ? "LAG" : "input",
                        (unsigned long long)xemu_tas_lag_streak());

    ImGui::TextDisabled("Seek"); ImGui::SameLine();
    if (ImGui::Button("Select Current", ImVec2(104.0f, 0))) TasSelectCurrentFrame();
    ImGui::SameLine();
    if (ImGui::Button("Seek Selected", ImVec2(102.0f, 0)))
        TasSeekFrame((uint64_t)g_tas_selected_frame);
    ImGui::SameLine();
    ImGui::Checkbox("Double-click continues", &g_tas_double_click_continue);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Off: exact seek and stop. On: exact seek, then continue. Follow is never changed by seeking.");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(88.0f);
    ImGui::InputInt("##tas_rw_custom", &g_tas_rewind_distance, 0, 0);
    g_tas_rewind_distance = std::clamp(g_tas_rewind_distance, 1, 36000);
    ImGui::SameLine();
    if (ImGui::Button("< Custom", ImVec2(80.0f, 0))) TasRewindFrames((uint64_t)g_tas_rewind_distance);

    /* Less-frequent movie creation/overdub controls stay below transport. */
    if (recording_now) ImGui::BeginDisabled();
    if (ImGui::Button("New Current-State Movie", ImVec2(154.0f, 0)))
        TasStartFreshRecordingFromCurrentState();
    ImGui::SameLine();
    if (ImGui::Button("Record From Power-On", ImVec2(150.0f, 0))) TasStartRecording(true);
    if (recording_now) ImGui::EndDisabled();
    ImGui::SameLine();
    if (!overdub_now) {
        if (ImGui::Button("Punch-In / Overdub###tas_overdub", ImVec2(136.0f, 0))) TasStartOverdub();
    } else if (ImGui::Button("Stop Overdub###tas_overdub", ImVec2(136.0f, 0))) {
        TasStopOverdub();
    }
    ImGui::SameLine();
    if (ImGui::Button("Movie Properties", ImVec2(112.0f, 0))) g_tas_properties_open = !g_tas_properties_open;
    ImGui::SameLine(); if (ImGui::Button("Compatibility", ImVec2(104.0f, 0))) g_tas_compatibility_open = true;
    ImGui::SameLine(); if (ImGui::Button("RAM Tools", ImVec2(86.0f, 0))) g_tas_ram_tools_open = true;
    ImGui::SameLine(); ImGui::Checkbox("TAS HUD", &g_tas_hud_enabled);

    const char *ports[] = {"Port 1", "Port 2", "Port 3", "Port 4"};
    ImGui::SetNextItemWidth(100);
    ImGui::Combo("Controller", &g_tas_port, ports, 4);
    ImGui::SameLine();
    if (ImGui::Button("Capture current XID")) TasCaptureCurrentFrame();
    ImGui::SameLine();
    if (ImGui::Button("Apply selected frame")) TasApplySelectedFrame();
    ImGui::SameLine();
    if (ImGui::Button("Release override")) xemu_tas_clear_xid_report((uint8_t)g_tas_port);

    ImGui::SameLine();
    if (ImGui::Button("Auto-Hold / Autofire")) g_tas_automation_open = !g_tas_automation_open;
    ImGui::SameLine();
    if (ImGui::Button("Combo / Macro")) g_tas_macro_open = !g_tas_macro_open;
    ImGui::SameLine();
    if (ImGui::Button("Chapters")) g_tas_chapters_open = !g_tas_chapters_open;
    ImGui::SameLine();
    if (ImGui::Button("Markers")) g_tas_markers_open = !g_tas_markers_open;
    ImGui::SameLine();
    if (ImGui::Button("Branches")) g_tas_branches_open = !g_tas_branches_open;

    }

    static const char *automation_controls[] = {
        "D-Up", "D-Down", "D-Left", "D-Right", "Start", "Back", "LStick", "RStick",
        "A", "B", "X", "Y", "Black", "White", "Left Trigger", "Right Trigger"
    };

    if (g_tas_automation_open && ImGui::CollapsingHeader("Auto-Hold / Autofire", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::Combo("Input##tas_auto", &g_tas_auto_control, automation_controls, 16)) TasUpdateAutomation();
        ImGui::SameLine();
        if (ImGui::Checkbox("Auto-hold", &g_tas_auto_hold_enabled)) TasUpdateAutomation();
        ImGui::SameLine();
        if (ImGui::Checkbox("Autofire", &g_tas_autofire_enabled)) TasUpdateAutomation();
        if (g_tas_auto_control >= 8) {
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderInt("Pressure", &g_tas_auto_value, 0, 255)) TasUpdateAutomation();
        }
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderInt("Autofire period", &g_tas_autofire_period, 1, 60)) {
            g_tas_autofire_phase = std::min(g_tas_autofire_phase, g_tas_autofire_period - 1);
            TasUpdateAutomation();
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        if (ImGui::SliderInt("Phase", &g_tas_autofire_phase, 0,
                             std::max(0, g_tas_autofire_period - 1))) TasUpdateAutomation();
        ImGui::SameLine();
        if (ImGui::Button("Clear Port Automation")) {
            xemu_tas_clear_automation((uint8_t)g_tas_port);
            g_tas_auto_hold_enabled = false;
            g_tas_autofire_enabled = false;
        }
    }

    if (g_tas_macro_open && ImGui::CollapsingHeader("Combo / Macro Generator", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::SetNextItemWidth(160.0f);
        ImGui::Combo("Input##tas_macro", &g_tas_macro_control, automation_controls, 16);
        if (g_tas_macro_control >= 8) {
            ImGui::SetNextItemWidth(220.0f);
            ImGui::SliderInt("Pressure##tas_macro", &g_tas_macro_value, 0, 255);
        }
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("Press frames", &g_tas_macro_press_frames);
        g_tas_macro_press_frames = std::clamp(g_tas_macro_press_frames, 1, 600);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("Gap frames", &g_tas_macro_gap_frames);
        g_tas_macro_gap_frames = std::clamp(g_tas_macro_gap_frames, 0, 600);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(180.0f);
        ImGui::InputInt("Repeats", &g_tas_macro_repeats);
        g_tas_macro_repeats = std::clamp(g_tas_macro_repeats, 1, 1000);
        ImGui::SameLine();
        if (ImGui::Button("Insert Macro At Selected Frame")) TasInsertMacro();
    }

    if (g_tas_chapters_open && ImGui::CollapsingHeader("Movie Chapters", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Add Chapter At Selected Frame")) {
            TasPushUndo("Add chapter");
            TasChapter c;
            c.frame = (uint64_t)std::max(0, g_tas_selected_frame);
            c.name = "Chapter " + std::to_string(g_tas_chapters.size() + 1);
            g_tas_chapters.push_back(std::move(c));
            g_tas_movie_dirty = true; ++g_tas_movie_revision;
            TasAutosaveRecovery(true);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("%zu chapter(s)", g_tas_chapters.size());
        for (size_t i = 0; i < g_tas_chapters.size(); ++i) {
            ImGui::PushID((int)i);
            char name_buf[128];
            snprintf(name_buf, sizeof(name_buf), "%s", g_tas_chapters[i].name.c_str());
            ImGui::Text("%llu", (unsigned long long)g_tas_chapters[i].frame);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(260.0f);
            if (ImGui::InputText("##chapter_name", name_buf, sizeof(name_buf))) {
                g_tas_chapters[i].name = name_buf;
            }
            ImGui::SameLine();
            if (ImGui::Button("Go")) {
                g_tas_selected_frame = (int)std::min<uint64_t>(g_tas_chapters[i].frame, INT_MAX);
                g_tas_follow_frame = false;
            }
            ImGui::SameLine();
            if (ImGui::Button("Delete")) {
                g_tas_chapters.erase(g_tas_chapters.begin() + i);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
    }

    if (g_tas_branches_open && ImGui::CollapsingHeader("Branches Tree", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Current branch: %u   archived futures: %zu", g_tas_current_branch, g_tas_branches.size());
        std::set<uint32_t> visited_branches;
        bool drew_root = false;
        for (const auto &root : g_tas_branches) {
            if (root.parent == UINT32_MAX) { TasDrawBranchTreeNode(root.id, visited_branches); drew_root = true; }
        }
        if (!drew_root || !visited_branches.count(g_tas_current_branch)) TasDrawBranchTreeNode(g_tas_current_branch, visited_branches);
        if (ImGui::Button("Create Branch From Selected Frame")) {
            const uint64_t row = (uint64_t)std::clamp(
                g_tas_selected_frame, 0, (int)g_tas_frames.size() - 1);
            const uint64_t fork_boundary = std::min<uint64_t>(row + 1, g_tas_frames.size());
            TasPushTimelineUndo("Create branch", row);
            TasArchiveCurrentBranch(fork_boundary, "Manual branch snapshot");
            ++g_tas_rerecord_count;
            TasMarkMovieEdited(row);
            TasAutosaveRecovery(true);
        }
        if (ImGui::BeginTable("##tas_branches", 5, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Branch");
            ImGui::TableSetupColumn("Parent");
            ImGui::TableSetupColumn("Fork");
            ImGui::TableSetupColumn("Frames");
            ImGui::TableSetupColumn("Action");
            ImGui::TableHeadersRow();
            for (size_t i = 0; i < g_tas_branches.size(); ++i) {
                TasBranch &b = g_tas_branches[i];
                ImGui::PushID((int)i);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%u  %s", b.id, b.name.c_str());
                ImGui::TableSetColumnIndex(1); ImGui::Text("%s", b.parent == UINT32_MAX ? "-" : std::to_string(b.parent).c_str());
                ImGui::TableSetColumnIndex(2); ImGui::Text("%llu", (unsigned long long)b.fork_frame);
                ImGui::TableSetColumnIndex(3); ImGui::Text("%zu", b.frames.size());
                ImGui::TableSetColumnIndex(4);
                if (ImGui::Button("Switch")) {
                    TasSwitchToBranchIndex(i);
                    ImGui::PopID();
                    break;
                }
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
    }

    if (g_tas_properties_open && ImGui::CollapsingHeader("Movie Properties / Annotations", ImGuiTreeNodeFlags_DefaultOpen)) {
        char author[256]; snprintf(author,sizeof(author),"%s",g_tas_properties.author.c_str());
        char category[256]; snprintf(category,sizeof(category),"%s",g_tas_properties.category.c_str());
        char gamever[256]; snprintf(gamever,sizeof(gamever),"%s",g_tas_properties.game_version.c_str());
        if (ImGui::InputText("Author", author, sizeof(author))) { g_tas_properties.author=author; g_tas_movie_dirty=true; }
        if (ImGui::InputText("Category / Goal", category, sizeof(category))) { g_tas_properties.category=category; g_tas_movie_dirty=true; }
        if (ImGui::InputText("Game / Version Notes", gamever, sizeof(gamever))) { g_tas_properties.game_version=gamever; g_tas_movie_dirty=true; }
        static char comments_buf[8192]{};
        static std::string comments_last;
        if (comments_last != g_tas_properties.comments) { snprintf(comments_buf,sizeof(comments_buf),"%s",g_tas_properties.comments.c_str()); comments_last=g_tas_properties.comments; }
        if (ImGui::InputTextMultiline("Comments", comments_buf, sizeof(comments_buf), ImVec2(-1,90))) { g_tas_properties.comments=comments_buf; comments_last=g_tas_properties.comments; g_tas_movie_dirty=true; }
    }

    if (g_tas_markers_open && ImGui::CollapsingHeader("Bookmarks / Markers / Notes", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (ImGui::Button("Add Marker At Selection")) {
            TasMarker m; m.frame=(uint64_t)std::max(0,g_tas_selected_frame); m.name="Marker "+std::to_string(g_tas_markers.size()+1); g_tas_markers.push_back(std::move(m)); g_tas_movie_dirty=true;
        }
        ImGui::SameLine();
        if (ImGui::Button("Prev Marker")) TasFindFrame(-1,3);
        ImGui::SameLine();
        if (ImGui::Button("Next Marker")) TasFindFrame(1,3);
        for (size_t i=0;i<g_tas_markers.size();++i) {
            ImGui::PushID((int)i+50000); TasMarker &m=g_tas_markers[i];
            char name[192]; snprintf(name,sizeof(name),"%s",m.name.c_str());
            char note[512]; snprintf(note,sizeof(note),"%s",m.note.c_str());
            ImGui::Text("%llu",(unsigned long long)m.frame); ImGui::SameLine();
            ImGui::SetNextItemWidth(180); if(ImGui::InputText("##marker_name",name,sizeof(name))){m.name=name;g_tas_movie_dirty=true;}
            ImGui::SameLine(); ImGui::SetNextItemWidth(360); if(ImGui::InputText("##marker_note",note,sizeof(note))){m.note=note;g_tas_movie_dirty=true;}
            ImGui::SameLine(); if(ImGui::Button("Go")) TasSetSelection((int)std::min<uint64_t>(m.frame,INT_MAX),false);
            ImGui::SameLine(); if(ImGui::Button("Delete")){g_tas_markers.erase(g_tas_markers.begin()+i);g_tas_movie_dirty=true;ImGui::PopID();break;}
            ImGui::PopID();
        }
    }

    if ((!g_tas_compact_ui || g_tas_greenzone_panel_open) &&
        ImGui::CollapsingHeader("Greenzone / Timeline Cache",
            g_tas_compact_ui ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        ImGui::Checkbox("Enable greenzone checkpoints", &g_tas_greenzone_enabled);
        ImGui::SameLine(); ImGui::SetNextItemWidth(100); ImGui::InputInt("Interval",&g_tas_greenzone_interval); g_tas_greenzone_interval=std::clamp(g_tas_greenzone_interval,30,36000);
        ImGui::SameLine(); ImGui::SetNextItemWidth(90); ImGui::InputInt("Capacity",&g_tas_greenzone_capacity); g_tas_greenzone_capacity=std::clamp(g_tas_greenzone_capacity,4,(int)g_tas_rewind_points.size());
        ImGui::SameLine(); if(ImGui::Button("Seek To Selected")) TasSeekFrame((uint64_t)g_tas_selected_frame);
        int cached=0; for(const auto &cp:g_tas_rewind_points) if(cp.valid&&cp.branch_id==g_tas_current_branch)++cached;
        ImGui::SameLine(); ImGui::TextDisabled("%d cached state(s) on branch %u",cached,g_tas_current_branch);
    }

    if ((!g_tas_compact_ui || g_tas_verifier_panel_open) &&
        ImGui::CollapsingHeader("Determinism Verifier",
            g_tas_compact_ui ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        ImGui::TextWrapped("Verifier starts at canonical movie boundary 0 and hashes RAM + QEMU non-RAM VM/device state + TAS input/lag metadata through post-movie boundary N. Known XID poll counts are checked every frame and fail closed on the first mismatch.");
        ImGui::SetNextItemWidth(120); ImGui::InputInt("Hash interval",&g_tas_verify_interval); g_tas_verify_interval=std::clamp(g_tas_verify_interval,1,36000);
        if(g_tas_verify_mode==TasVerifyMode::Idle){
            if(ImGui::Button("Capture Baseline")) TasStartVerifier(true);
            ImGui::SameLine();
            if(ImGui::Button("Capture Every-Frame")) TasStartVerifier(true, 1, true);
            bool can_verify=!g_tas_verify_baseline.empty() && g_tas_verify_revision==g_tas_movie_revision;
            if(!can_verify)ImGui::BeginDisabled();
            if(ImGui::Button("Verify x1")) TasStartVerifier(false,1);
            ImGui::SameLine(); if(ImGui::Button("Stress Verify x10")) TasStartVerifier(false,10);
            ImGui::SameLine(); if(ImGui::Button("Stress Verify x100")) TasStartVerifier(false,100);
            if(!can_verify)ImGui::EndDisabled();
        } else { ImGui::Text("Verifier running: %s",g_tas_verify_status.c_str()); ImGui::SameLine(); if(ImGui::Button("Stop Verification")){g_tas_verify_mode=TasVerifyMode::Idle;g_tas_verify_runs_remaining=0;xemu_tas_stop_playback();TasCancelPendingTransportAdvance();if(runstate_is_running())vm_stop(RUN_STATE_PAUSED);} }
        ImGui::Text("Status: %s   checkpoints: %zu",g_tas_verify_status.c_str(),g_tas_verify_baseline.size());
        if(g_tas_verify_failed) ImGui::Text("First divergence: %llu  expected=%016llX actual=%016llX",(unsigned long long)g_tas_verify_first_bad_frame,(unsigned long long)g_tas_verify_expected,(unsigned long long)g_tas_verify_actual);
    }

    if ((!g_tas_compact_ui || g_tas_punch_panel_open) &&
        ImGui::CollapsingHeader("Punch-In / Overdub Fields",
            g_tas_compact_ui ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        if (ImGui::Button("All Fields")) {
            g_tas_overdub_fields.fill(true);
        }
        ImGui::SameLine();
        if (ImGui::Button("No Fields")) {
            g_tas_overdub_fields.fill(false);
        }
        static const char *od_names[20]={"Up","Down","Left","Right","Start","Back","LStick","RStick","A","B","X","Y","Black","White","LT","RT","LX","LY","RX","RY"};
        for(int i=0;i<20;++i){ImGui::PushID(60000+i);ImGui::Checkbox(od_names[i],&g_tas_overdub_fields[i]);ImGui::PopID();if((i%10)!=9)ImGui::SameLine();}
        ImGui::TextDisabled("Punch-in plays the existing movie as the base and replaces only checked fields with live controller input.");
    }

    if (g_tas_curve_open && ImGui::CollapsingHeader("Analog Curve / Stick Pattern Editor", ImGuiTreeNodeFlags_DefaultOpen)) {
        static const char *curve_controls[]={"LX","LY","RX","RY","A","B","X","Y","Black","White","LT","RT"};
        static const char *curve_types[]={"Linear","Ease In","Ease Out","Smoothstep"};
        ImGui::Combo("Control##curve",&g_tas_curve_control,curve_controls,12); ImGui::SameLine(); ImGui::Combo("Curve",&g_tas_curve_type,curve_types,4);
        int minv=g_tas_curve_control<4?-32768:0,maxv=g_tas_curve_control<4?32767:255;
        ImGui::SetNextItemWidth(170);ImGui::SliderInt("Start",&g_tas_curve_start_value,minv,maxv);ImGui::SameLine();ImGui::SetNextItemWidth(170);ImGui::SliderInt("End",&g_tas_curve_end_value,minv,maxv);ImGui::SameLine();if(ImGui::Button("Apply Curve To Selection"))TasApplyAnalogCurve();
        ImGui::Separator(); const char *sticks[]={"Left Stick","Right Stick"};ImGui::Combo("Circle stick",&g_tas_circle_stick,sticks,2);ImGui::SameLine();ImGui::SetNextItemWidth(180);ImGui::SliderInt("Radius",&g_tas_circle_radius,0,32767);ImGui::SameLine();ImGui::SetNextItemWidth(160);ImGui::SliderFloat("Turns",&g_tas_circle_turns,-8.0f,8.0f,"%.2f");ImGui::SameLine();if(ImGui::Button("Apply Circle"))TasApplyStickCircle();
    }

    if (g_tas_compare_open && ImGui::CollapsingHeader("Movie Comparison / Diff", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::Text("Compare: %s",g_tas_compare_path.c_str());
        if(g_tas_compare_first_diff==UINT64_MAX) ImGui::Text("Movies are identical across compared frames."); else { ImGui::Text("First differing frame: %llu   differing frames: %llu",(unsigned long long)g_tas_compare_first_diff,(unsigned long long)g_tas_compare_diff_count); if(ImGui::Button("Go To First Difference"))TasSetSelection((int)std::min<uint64_t>(g_tas_compare_first_diff,INT_MAX),false); }
    }

    if ((!g_tas_compact_ui || g_tas_rewind_panel_open) &&
        ImGui::CollapsingHeader("Rewind / Recovery",
            g_tas_compact_ui ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        ImGui::Checkbox("Enable rewind checkpoint cache", &g_tas_rewind_enabled);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("Checkpoint interval", &g_tas_rewind_interval);
        g_tas_rewind_interval = std::clamp(g_tas_rewind_interval, 30, 3600);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120.0f);
        ImGui::InputInt("Rewind frames", &g_tas_rewind_distance);
        g_tas_rewind_distance = std::clamp(g_tas_rewind_distance, 1, 36000);
        ImGui::SameLine();
        if (ImGui::Button("Rewind")) TasRewindFrames((uint64_t)g_tas_rewind_distance);
        ImGui::SameLine();
        if (ImGui::Button("Save Recovery Now")) TasAutosaveRecovery(true);
        ImGui::SameLine(); if (ImGui::Button("Recover Autosave")) TasRecoverAutosave();
        ImGui::SameLine(); if (ImGui::Button("Backup History")) g_tas_history_open = true;
        ImGui::SameLine();
        ImGui::Checkbox("Apply stored movie settings on load", &g_tas_apply_movie_settings);
    }

    auto [sel_a, sel_b] = TasSelectionBounds();
    if (g_tas_compact_ui) {
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(6.0f, 2.0f));
        ImGui::Text("Selection %d-%d (%d)", sel_a, sel_b, sel_b - sel_a + 1);
        ImGui::SameLine();
        const bool can_undo = !g_tas_undo_stack.empty();
        if (!can_undo) ImGui::BeginDisabled();
        if (ImGui::Button("Undo")) TasUndoEdit();
        if (!can_undo) ImGui::EndDisabled();
        ImGui::SameLine();
        const bool can_redo = !g_tas_redo_stack.empty();
        if (!can_redo) ImGui::BeginDisabled();
        if (ImGui::Button("Redo")) TasRedoEdit();
        if (!can_redo) ImGui::EndDisabled();
        ImGui::SameLine(); if (ImGui::Button("Copy")) TasCopySelection();
        ImGui::SameLine();
        const bool have_clip = !g_tas_clipboard_frames.empty();
        if (!have_clip) ImGui::BeginDisabled();
        if (ImGui::Button("Paste")) TasPasteSelection(false);
        if (!have_clip) ImGui::EndDisabled();
        ImGui::SameLine(); if (ImGui::Button("Delete")) TasDeleteSelection();
        ImGui::SameLine(); if (ImGui::Button("Clear")) TasClearSelection();
        ImGui::SameLine(); ImGui::TextDisabled("More editing/navigation is in the Edit and Navigate menus");
        ImGui::PopStyleVar();
    } else {
    ImGui::Text("Selection: %d - %d (%d frame%s)", sel_a, sel_b, sel_b-sel_a+1,
                sel_b==sel_a ? "" : "s");
    ImGui::SameLine();
    bool can_undo=!g_tas_undo_stack.empty(); if(!can_undo)ImGui::BeginDisabled(); if(ImGui::Button("Undo"))TasUndoEdit(); if(!can_undo)ImGui::EndDisabled();
    ImGui::SameLine(); bool can_redo=!g_tas_redo_stack.empty(); if(!can_redo)ImGui::BeginDisabled(); if(ImGui::Button("Redo"))TasRedoEdit(); if(!can_redo)ImGui::EndDisabled();
    ImGui::SameLine(); if(ImGui::Button("Copy Range"))TasCopySelection();
    ImGui::SameLine(); bool have_clip=!g_tas_clipboard_frames.empty(); if(!have_clip)ImGui::BeginDisabled(); if(ImGui::Button("Paste Insert"))TasPasteSelection(true); ImGui::SameLine(); if(ImGui::Button("Paste Overwrite"))TasPasteSelection(false); if(!have_clip)ImGui::EndDisabled();
    ImGui::SameLine(); if(ImGui::Button("Clone Range"))TasCloneSelection();
    ImGui::SameLine(); if(ImGui::Button("Delete Range"))TasDeleteSelection();
    ImGui::SameLine(); if(ImGui::Button("Clear Range"))TasClearSelection();

    if (ImGui::Button("+60 blank frames")) {
        size_t old=g_tas_frames.size();
        TasPushTimelineUndo("Append 60 blank frames", (uint64_t)old);
        g_tas_frames.resize(old+60);
        g_tas_lag_flags.resize(g_tas_frames.size(),0);
        g_tas_poll_counts.resize(g_tas_frames.size(), TasUnknownPollCounts());
        TasMarkMovieEdited((uint64_t)old);
    }
    ImGui::SameLine();
    if (ImGui::Button("Insert frame")) {
        int at=std::clamp(g_tas_selected_frame,0,(int)g_tas_frames.size());
        TasPushTimelineUndo("Insert frame", (uint64_t)at);
        g_tas_frames.insert(g_tas_frames.begin()+at,TasFrame{});
        g_tas_lag_flags.insert(g_tas_lag_flags.begin()+at,0);
        g_tas_poll_counts.insert(g_tas_poll_counts.begin()+at,TasUnknownPollCounts());
        TasSetSelection(at,false);
        TasMarkMovieEdited((uint64_t)at);
    }
    ImGui::SameLine(); if(ImGui::Button("Analog Curves"))g_tas_curve_open=!g_tas_curve_open;
    ImGui::SameLine(); if(ImGui::Button("Save Pattern..."))TasSavePatternDialog();
    ImGui::SameLine(); if(ImGui::Button("Insert Pattern..."))TasLoadPatternDialog();
    ImGui::SameLine(); if(ImGui::Button("Compare Movie..."))TasCompareDialog();
    ImGui::SameLine(); if(ImGui::Button("Export CSV..."))TasExportCsvDialog();
    ImGui::SameLine(); if(ImGui::Button("Import CSV..."))TasImportCsvDialog();

    if (ImGui::Button("Prev Input Change")) {
        TasFindFrame(-1, 0);
    }
    ImGui::SameLine();
    if (ImGui::Button("Next Input Change")) {
        TasFindFrame(1, 0);
    }
    ImGui::SameLine(); if(ImGui::Button("Prev Lag"))TasFindFrame(-1,1); ImGui::SameLine(); if(ImGui::Button("Next Lag"))TasFindFrame(1,1);
    ImGui::SameLine(); if(ImGui::Button("Prev Chapter"))TasFindFrame(-1,2); ImGui::SameLine(); if(ImGui::Button("Next Chapter"))TasFindFrame(1,2);
    ImGui::SameLine(); ImGui::Checkbox("Dim lag", &g_tas_dim_lag_frames);
    ImGui::SameLine();
    if (ImGui::Checkbox("Hide lag", &g_tas_hide_lag_frames)) {
        g_tas_visible_frame_cache_dirty = true;
    }

    }

    ImGui::Separator();
    const uint64_t transport_vm_frame = xemu_tas_frame();
    const uint64_t transport_movie_frame = xemu_tas_playback()
        ? xemu_tas_playback_frame() : transport_vm_frame;
    ImGui::TextDisabled("Transport: VM %llu  |  Movie %llu  |  Selected %d",
                        (unsigned long long)transport_vm_frame,
                        (unsigned long long)transport_movie_frame,
                        g_tas_selected_frame);
    if (g_tas_seek_completion_pending) {
        ImGui::SameLine();
        ImGui::TextDisabled("| reconstructing exact frame %llu...",
                            (unsigned long long)g_tas_seek_continue_target);
    } else if (g_tas_step_completion_pending) {
        ImGui::SameLine();
        ImGui::TextDisabled("| stepping to exact frame %llu...",
                            (unsigned long long)g_tas_step_completion_target);
    }
    ImGui::TextUnformatted("Piano roll - frames run downward; mouse wheel scrolls back to frame 0");

    static const char *headers[20] = {
        "Up", "Dn", "Lt", "Rt", "Start", "Back", "LS", "RS",
        "A", "B", "X", "Y", "Black", "White", "LT", "RT",
        "LX", "LY", "RX", "RY"
    };
    static const uint16_t digital_masks[8] = {
        0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080
    };

    const float row_h = ImGui::GetTextLineHeightWithSpacing() + 5.0f;
    const float desired_roll_h = row_h * ((float)g_tas_frames.size() + 1.35f);
    // Do not create a giant empty region for short movies. Long movies get a
    // compact vertical viewport and scroll naturally through recorded frames.
    const float roll_h = std::clamp(desired_roll_h, row_h * 2.35f, 360.0f);

    ImGuiTableFlags table_flags =
        ImGuiTableFlags_Borders |
        ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingStretchSame |
        ImGuiTableFlags_NoSavedSettings;

    if (ImGui::BeginTable("##tas_vertical_roll", 22, table_flags,
                          ImVec2(0.0f, roll_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Frame", ImGuiTableColumnFlags_WidthFixed, 62.0f);
        ImGui::TableSetupColumn("Lag", ImGuiTableColumnFlags_WidthFixed, 34.0f);
        for (int i = 0; i < 20; ++i) {
            const float weight = (i >= 16) ? 1.55f : 1.0f;
            ImGui::TableSetupColumn(headers[i], ImGuiTableColumnFlags_WidthStretch, weight);
        }
        ImGui::TableHeadersRow();

        ImGuiIO &io = ImGui::GetIO();
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) &&
            io.MouseWheel != 0.0f) {
            // Manual scrolling always wins. This lets the user travel all the
            // way back to frame 0 without Follow immediately snapping forward.
            g_tas_follow_frame = false;
            scroll_to_followed_frame = false;
        }

        if (scroll_to_followed_frame && g_tas_follow_frame) {
            const float target_y = std::max(0.0f,
                (float)g_tas_selected_frame * row_h - roll_h * 0.65f);
            ImGui::SetScrollY(target_y);
            scroll_to_followed_frame = false;
        }

        if (g_tas_hide_lag_frames && g_tas_visible_frame_cache_dirty) {
            g_tas_visible_frame_cache.clear();
            g_tas_visible_frame_cache.reserve(g_tas_frames.size());
            for (int i = 0; i < (int)g_tas_frames.size(); ++i) {
                if (i >= (int)g_tas_lag_flags.size() || !g_tas_lag_flags[(size_t)i]) {
                    g_tas_visible_frame_cache.push_back(i);
                }
            }
            g_tas_visible_frame_cache_dirty = false;
        }
        const int visible_count = g_tas_hide_lag_frames
            ? (int)g_tas_visible_frame_cache.size()
            : (int)g_tas_frames.size();
        auto visible_selection = TasSelectionBounds();
        ImGuiListClipper clipper;
        clipper.Begin(visible_count, row_h);
        while (clipper.Step()) {
            for (int vi = clipper.DisplayStart; vi < clipper.DisplayEnd; ++vi) {
                int f = g_tas_hide_lag_frames ? g_tas_visible_frame_cache[(size_t)vi] : vi;
                TasFrame &frame = g_tas_frames[f];
                TasXidReport &report = frame.xid[g_tas_port];
                ImGui::TableNextRow(ImGuiTableRowFlags_None, row_h);
                const bool lag_row = f < (int)g_tas_lag_flags.size() && g_tas_lag_flags[f];
                if (lag_row && g_tas_dim_lag_frames) {
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0, IM_COL32(70,70,70,70));
                }

                ImGui::TableSetColumnIndex(0);
                ImGui::PushID(f);
                char frame_text[32];
                snprintf(frame_text, sizeof(frame_text), "%d", f);
                bool selected_range = f >= visible_selection.first &&
                                      f <= visible_selection.second;
                if (ImGui::Selectable(frame_text, selected_range,
                                      ImGuiSelectableFlags_None,
                                      ImVec2(0.0f, row_h - 2.0f))) {
                    TasSetSelection(f, ImGui::GetIO().KeyShift);
                    // Keep later visible rows in this same frame consistent
                    // without recomputing selection bounds for every row.
                    visible_selection = TasSelectionBounds();
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    TasSeekFrameEx((uint64_t)f, g_tas_double_click_continue);
                }

                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted((f < (int)g_tas_lag_flags.size() && g_tas_lag_flags[f]) ? "L" : ".");

                const uint16_t buttons = tas_read_u16(report, 2);
                for (int i = 0; i < 8; ++i) {
                    ImGui::TableSetColumnIndex(i + 2);
                    const bool on = (buttons & digital_masks[i]) != 0;
                    ImGui::PushID(i);
                    if (ImGui::SmallButton(on ? "X" : ".")) {
                        TasPushUndoRange("Toggle digital input", f, f);
                        uint16_t b = tas_read_u16(report, 2);
                        b ^= digital_masks[i];
                        memcpy(&report[2], &b, sizeof(b));
                        TasSetSelection(f, false);
                        TasMarkMovieEdited((uint64_t)f);
                    }
                    if (ImGui::BeginPopupContextItem("##input_auto_popup")) {
                        g_tas_auto_control = i;
                        ImGui::Text("%s automation", headers[i]);
                        if (ImGui::MenuItem("Toggle Auto-Hold")) { g_tas_auto_hold_enabled=!g_tas_auto_hold_enabled; TasUpdateAutomation(); }
                        if (ImGui::MenuItem("Toggle Autofire")) { g_tas_autofire_enabled=!g_tas_autofire_enabled; TasUpdateAutomation(); }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }

                for (int i = 0; i < 8; ++i) {
                    ImGui::TableSetColumnIndex(10 + i);
                    const uint8_t value = report[4 + i];
                    char value_text[8];
                    if (value == 0) snprintf(value_text, sizeof(value_text), ".");
                    else snprintf(value_text, sizeof(value_text), "%u", value);
                    ImGui::PushID(100 + i);
                    if (ImGui::SmallButton(value_text)) {
                        TasPushUndoRange("Toggle pressure input", f, f);
                        report[4 + i] = value ? 0 : 255;
                        TasSetSelection(f, false);
                        TasMarkMovieEdited((uint64_t)f);
                    }
                    if (ImGui::BeginPopupContextItem("##analog_auto_popup")) {
                        g_tas_auto_control = 8 + i;
                        ImGui::Text("%s automation", headers[8+i]);
                        ImGui::SliderInt("Pressure",&g_tas_auto_value,0,255);
                        if (ImGui::MenuItem("Toggle Auto-Hold")) { g_tas_auto_hold_enabled=!g_tas_auto_hold_enabled; TasUpdateAutomation(); }
                        if (ImGui::MenuItem("Toggle Autofire")) { g_tas_autofire_enabled=!g_tas_autofire_enabled; TasUpdateAutomation(); }
                        ImGui::EndPopup();
                    }
                    ImGui::PopID();
                }

                const int stick_offsets[4] = {12, 14, 16, 18};
                for (int i = 0; i < 4; ++i) {
                    ImGui::TableSetColumnIndex(18 + i);
                    const int value = tas_read_s16(report, stick_offsets[i]);
                    char value_text[16];
                    if (value == 0) snprintf(value_text, sizeof(value_text), ".");
                    else snprintf(value_text, sizeof(value_text), "%d", value);
                    ImGui::PushID(200 + i);
                    if (ImGui::SmallButton(value_text)) {
                        // Stick cells select the frame; precise editing belongs
                        // in the exact editor below instead of guessing a value.
                        TasSetSelection(f, false);
                    }
                    ImGui::PopID();
                }
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    g_tas_selected_frame = std::clamp(g_tas_selected_frame, 0,
                                      (int)g_tas_frames.size() - 1);
    TasFrame &fr = g_tas_frames[g_tas_selected_frame];
    TasXidReport &fr_report = fr.xid[g_tas_port];
    if (!g_tas_compact_ui || g_tas_exact_editor_open) {
        ImGui::Separator();
        ImGui::Text("Exact input inspector - frame %d", g_tas_selected_frame);
        const char *pressure_names[8] = {
            "A", "B", "X", "Y", "Black", "White", "Left Trigger", "Right Trigger"
        };
        const char *stick_names[4] = {"LX", "LY", "RX", "RY"};
        const int stick_offsets[4] = {12, 14, 16, 18};

        if (g_tas_compact_ui) {
            if (ImGui::BeginTable("##tas_compact_exact", 4,
                    ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_NoSavedSettings)) {
                ImGui::TableSetupColumn("##label1", ImGuiTableColumnFlags_WidthFixed, 82.0f);
                ImGui::TableSetupColumn("##slider1", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                ImGui::TableSetupColumn("##label2", ImGuiTableColumnFlags_WidthFixed, 82.0f);
                ImGui::TableSetupColumn("##slider2", ImGuiTableColumnFlags_WidthStretch, 1.0f);
                for (int row = 0; row < 6; ++row) {
                    ImGui::TableNextRow();
                    for (int side = 0; side < 2; ++side) {
                        const int col = side * 2;
                        ImGui::TableSetColumnIndex(col);
                        ImGui::AlignTextToFramePadding();
                        if (row < 4) {
                            const int i = row * 2 + side;
                            ImGui::TextUnformatted(pressure_names[i]);
                            ImGui::TableSetColumnIndex(col + 1);
                            int v = fr_report[4 + i];
                            ImGui::PushID(1000 + i);
                            ImGui::SetNextItemWidth(-1.0f);
                            bool changed = ImGui::SliderInt("##pressure", &v, 0, 255);
                            if (ImGui::IsItemActivated()) TasPushUndoRange("Edit analog pressure", g_tas_selected_frame, g_tas_selected_frame);
                            if (changed) { fr_report[4 + i] = (uint8_t)v; TasMarkMovieEdited((uint64_t)g_tas_selected_frame); }
                            ImGui::PopID();
                        } else {
                            const int i = (row - 4) * 2 + side;
                            ImGui::TextUnformatted(stick_names[i]);
                            ImGui::TableSetColumnIndex(col + 1);
                            int v = tas_read_s16(fr_report, stick_offsets[i]);
                            ImGui::PushID(2000 + i);
                            ImGui::SetNextItemWidth(-1.0f);
                            bool changed = ImGui::SliderInt("##stick", &v, -32768, 32767);
                            if (ImGui::IsItemActivated()) TasPushUndoRange("Edit analog stick", g_tas_selected_frame, g_tas_selected_frame);
                            if (changed) { tas_write_s16(fr_report, stick_offsets[i], (int16_t)v); TasMarkMovieEdited((uint64_t)g_tas_selected_frame); }
                            ImGui::PopID();
                        }
                    }
                }
                ImGui::EndTable();
            }
        } else {
            for (int i = 0; i < 8; ++i) {
                int v = fr_report[4 + i];
                ImGui::PushID(1000 + i);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(pressure_names[i]);
                ImGui::SameLine(112.0f);
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                bool changed = ImGui::SliderInt("##pressure", &v, 0, 255);
                if (ImGui::IsItemActivated()) TasPushUndoRange("Edit analog pressure", g_tas_selected_frame, g_tas_selected_frame);
                if (changed) { fr_report[4 + i] = (uint8_t)v; TasMarkMovieEdited((uint64_t)g_tas_selected_frame); }
                ImGui::PopID();
            }
            for (int i = 0; i < 4; ++i) {
                int v = tas_read_s16(fr_report, stick_offsets[i]);
                ImGui::PushID(2000 + i);
                ImGui::AlignTextToFramePadding();
                ImGui::TextUnformatted(stick_names[i]);
                ImGui::SameLine(112.0f);
                ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                bool changed = ImGui::SliderInt("##stick", &v, -32768, 32767);
                if (ImGui::IsItemActivated()) TasPushUndoRange("Edit analog stick", g_tas_selected_frame, g_tas_selected_frame);
                if (changed) { tas_write_s16(fr_report, stick_offsets[i], (int16_t)v); TasMarkMovieEdited((uint64_t)g_tas_selected_frame); }
                ImGui::PopID();
            }
        }
    }

    if (!g_tas_compact_ui || g_tas_exact_editor_open || g_tas_all_ports_view)
        ImGui::Checkbox("All 4 controllers - selected frame", &g_tas_all_ports_view);
    if (g_tas_all_ports_view) {
        ImGuiTableFlags ap_flags = ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                                   ImGuiTableFlags_ScrollX | ImGuiTableFlags_SizingFixedFit;
        if (ImGui::BeginTable("##tas_all_ports", 21, ap_flags, ImVec2(0, 150))) {
            ImGui::TableSetupScrollFreeze(1,1);
            ImGui::TableSetupColumn("Port", ImGuiTableColumnFlags_WidthFixed, 55);
            static const char *all_headers[20]={"Up","Dn","Lt","Rt","Start","Back","LS","RS","A","B","X","Y","Black","White","LT","RT","LX","LY","RX","RY"};
            for(int i=0;i<20;++i)ImGui::TableSetupColumn(all_headers[i],ImGuiTableColumnFlags_WidthFixed,i>=16?72:48);
            ImGui::TableHeadersRow();
            for(int port=0;port<4;++port){
                TasXidReport &r=g_tas_frames[g_tas_selected_frame].xid[port];
                ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::Text("P%d",port+1);
                uint16_t buttons=tas_read_u16(r,2);
                static const uint16_t masks[8]={1,2,4,8,0x10,0x20,0x40,0x80};
                for(int i=0;i<8;++i){ImGui::TableSetColumnIndex(1+i);ImGui::PushID(70000+port*100+i);bool on=(buttons&masks[i])!=0;if(ImGui::SmallButton(on?"X":".")){TasPushUndoRange("Edit all-port input", g_tas_selected_frame, g_tas_selected_frame);buttons^=masks[i];memcpy(&r[2],&buttons,2);TasMarkMovieEdited((uint64_t)g_tas_selected_frame);}ImGui::PopID();}
                for(int i=0;i<8;++i){ImGui::TableSetColumnIndex(9+i);ImGui::PushID(71000+port*100+i);char t[8];snprintf(t,sizeof(t),r[4+i]?"%u":".",r[4+i]);if(ImGui::SmallButton(t)){TasPushUndoRange("Edit all-port pressure", g_tas_selected_frame, g_tas_selected_frame);r[4+i]=r[4+i]?0:255;TasMarkMovieEdited((uint64_t)g_tas_selected_frame);}ImGui::PopID();}
                for(int i=0;i<4;++i){ImGui::TableSetColumnIndex(17+i);ImGui::PushID(72000+port*100+i);int v=tas_read_s16(r,12+i*2);ImGui::SetNextItemWidth(68);bool ch=ImGui::DragInt("##axis",&v,256.0f,-32768,32767,"%d");if(ImGui::IsItemActivated())TasPushUndoRange("Edit all-port stick", g_tas_selected_frame, g_tas_selected_frame);if(ch){tas_write_s16(r,12+i*2,(int16_t)v);TasMarkMovieEdited((uint64_t)g_tas_selected_frame);}ImGui::PopID();}
            }
            ImGui::EndTable();
        }
    }

    if (!g_tas_compact_ui) {
        ImGui::TextDisabled("The roll is bounded by the current movie length. Follow TAS frame auto-scrolls downward as recording/playback appends frames; manual wheel scrolling disables Follow so frame 0 is always reachable. Shift-click frames selects a range; double-click seeks through the greenzone and either stops or continues according to the toolbar option.");
    } else {
        ImGui::TextDisabled(g_tas_double_click_continue
            ? "Shift-click = range | double-click = seek + continue | wheel = manual timeline scroll"
            : "Shift-click = range | double-click = seek + stop | wheel = manual timeline scroll");
    }
    ImGui::End();
}

static void DrawTasCompatibilityWindow()
{
    static constexpr const char *kDetachId = "tas.compatibility";
    xemu_feature_detach::Register(kDetachId, "TAS Movie Compatibility",
                                  &g_tas_compatibility_open, DrawTasCompatibilityWindow);
    if (!g_tas_compatibility_open || !xemu_feature_detach::ShouldDraw(kDetachId)) return;
    if (xemu_feature_detach::IsDetachedPass(kDetachId)) {
        xemu_feature_detach::PrepareWindow(kDetachId);
    } else {
        ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_FirstUseEver);
    }
    if (!ImGui::Begin("TAS Movie Compatibility", &g_tas_compatibility_open,
                      xemu_feature_detach::WindowFlags(kDetachId, 0))) { ImGui::End(); return; }
    xemu_feature_detach::ObserveCurrentWindow(kDetachId);
    static std::vector<std::pair<std::string,bool>> checks;
    static std::chrono::steady_clock::time_point next_refresh{};
    const auto now = std::chrono::steady_clock::now();
    if (checks.empty() || now >= next_refresh) {
        checks = TasCompatibilityChecks();
        next_refresh = now + std::chrono::seconds(1);
    }
    int good=0; for(const auto &c:checks) if(c.second)++good;
    ImGui::Text("Compatibility: %d/%zu checks match",good,checks.size());
    if(ImGui::Button("Apply Stored Display/TAS Settings") && g_tas_loaded_environment.valid){
        g_config.display.renderer=(int)g_tas_loaded_environment.renderer;
        nv2a_set_surface_scale_factor(std::clamp((int)g_tas_loaded_environment.surface_scale,1,10));
        g_config.display.ui.fit=(int)g_tas_loaded_environment.fit;
        g_config.display.filtering=(int)g_tas_loaded_environment.filtering;
        g_config.display.ui.aspect_ratio=(int)g_tas_loaded_environment.aspect;
        g_config.general.fast_forward_multiplier=(int)g_tas_loaded_environment.fast_forward;
        if (xemu_fast_forward_active()) {
            xemu_fast_forward_set_active(true);
        }
        xemu_tas_set_deterministic_mode(g_tas_loaded_environment.deterministic);
    }
    if(ImGui::BeginTable("##compat",2,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg)){
        ImGui::TableSetupColumn("Item");ImGui::TableSetupColumn("Status",ImGuiTableColumnFlags_WidthFixed,120);ImGui::TableHeadersRow();
        for(const auto &c:checks){ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::TextUnformatted(c.first.c_str());ImGui::TableSetColumnIndex(1);if(c.second)ImGui::TextColored(ImVec4(.4f,1,.4f,1),"MATCH");else ImGui::TextColored(ImVec4(1,.5f,.25f,1),"MISMATCH");}
        ImGui::EndTable();
    }
    if(g_tas_loaded_environment.valid){
        ImGui::Separator();
        ImGui::TextWrapped("Stored Xemu: %s (%s)",g_tas_loaded_environment.xemu_version.c_str(),g_tas_loaded_environment.xemu_commit.c_str());
        ImGui::TextWrapped("Stored disc: %s",g_tas_loaded_environment.disc_path.c_str());
    }
    ImGui::End();
}

static void DrawTasHistoryWindow()
{
    static constexpr const char *kDetachId = "tas.history";
    xemu_feature_detach::Register(kDetachId, "TAS Movie Backup History",
                                  &g_tas_history_open, DrawTasHistoryWindow);
    if(!g_tas_history_open || !xemu_feature_detach::ShouldDraw(kDetachId))return;
    if (xemu_feature_detach::IsDetachedPass(kDetachId)) {
        xemu_feature_detach::PrepareWindow(kDetachId);
    }
    if(!ImGui::Begin("TAS Movie Backup History",&g_tas_history_open,
                     xemu_feature_detach::WindowFlags(kDetachId, 0))){ImGui::End();return;}
    xemu_feature_detach::ObserveCurrentWindow(kDetachId);
    const auto &files = TasMovieHistoryFiles();
    if(files.empty())ImGui::TextDisabled("No backup history exists for the current movie yet.");
    for(size_t i=0;i<files.size();++i){ImGui::PushID((int)i);ImGui::TextUnformatted(files[i].filename().string().c_str());ImGui::SameLine();if(ImGui::Button("Load"))TasLoadMovieFromPath(files[i].string().c_str());ImGui::SameLine();if(ImGui::Button("Restore Over Current")&&!g_tas_movie_path.empty()){std::error_code ec;std::filesystem::copy_file(files[i],g_tas_movie_path,std::filesystem::copy_options::overwrite_existing,ec);if(!ec)TasLoadMovieFromPath(g_tas_movie_path.c_str());}ImGui::PopID();}
    ImGui::End();
}

static void DrawTasRamToolsWindow()
{
    static constexpr const char *kDetachId = "tas.ram-tools";
    xemu_feature_detach::Register(kDetachId, "TAS RAM Watch / Search / RNG",
                                  &g_tas_ram_tools_open, DrawTasRamToolsWindow);
    if(!g_tas_ram_tools_open || !xemu_feature_detach::ShouldDraw(kDetachId))return;
    if (xemu_feature_detach::IsDetachedPass(kDetachId)) {
        xemu_feature_detach::PrepareWindow(kDetachId);
    } else {
        ImGui::SetNextWindowSize(ImVec2(860,620),ImGuiCond_FirstUseEver);
    }
    if(!ImGui::Begin("TAS RAM Watch / Search / RNG",&g_tas_ram_tools_open,
                     xemu_feature_detach::WindowFlags(kDetachId, 0))){ImGui::End();return;}
    xemu_feature_detach::ObserveCurrentWindow(kDetachId);
    ImGui::TextUnformatted("RAM Watches");
    ImGui::SetNextItemWidth(130);ImGui::InputScalar("Address",ImGuiDataType_U32,&g_tas_new_watch_address,nullptr,nullptr,"%08X",ImGuiInputTextFlags_CharsHexadecimal);ImGui::SameLine();const char *sizes[]={"8-bit","16-bit","32-bit","64-bit"};int si=g_tas_new_watch_size==1?0:g_tas_new_watch_size==2?1:g_tas_new_watch_size==4?2:3;ImGui::SetNextItemWidth(100);if(ImGui::Combo("Size",&si,sizes,4))g_tas_new_watch_size=1<<si;ImGui::SameLine();ImGui::SetNextItemWidth(180);ImGui::InputText("Label",g_tas_new_watch_label,sizeof(g_tas_new_watch_label));ImGui::SameLine();if(ImGui::Button("Add Watch")){TasRamWatch w;w.address=g_tas_new_watch_address;w.size=g_tas_new_watch_size;w.label=g_tas_new_watch_label[0]?g_tas_new_watch_label:"Watch";g_tas_ram_watches.push_back(std::move(w));}
    if(ImGui::BeginTable("##ramwatches",6,ImGuiTableFlags_Borders|ImGuiTableFlags_RowBg|ImGuiTableFlags_ScrollY,ImVec2(0,210))){
        ImGui::TableSetupColumn("Label");ImGui::TableSetupColumn("Address");ImGui::TableSetupColumn("Value");ImGui::TableSetupColumn("Previous");ImGui::TableSetupColumn("RNG");ImGui::TableSetupColumn("Action");ImGui::TableHeadersRow();
        for(size_t i=0;i<g_tas_ram_watches.size();++i){auto &w=g_tas_ram_watches[i];ImGui::PushID((int)i);ImGui::TableNextRow();ImGui::TableSetColumnIndex(0);ImGui::TextUnformatted(w.label.c_str());ImGui::TableSetColumnIndex(1);ImGui::Text("%08X",w.address);ImGui::TableSetColumnIndex(2);if(w.valid)ImGui::Text("%0*llX",w.size*2,(unsigned long long)w.last_value);else ImGui::TextDisabled("invalid");ImGui::TableSetColumnIndex(3);ImGui::Text("%0*llX",w.size*2,(unsigned long long)w.previous_value);ImGui::TableSetColumnIndex(4);bool rng=g_tas_rng_watch==(int)i;if(ImGui::RadioButton("##rng",rng)){g_tas_rng_watch=(int)i;g_tas_rng_history.clear();}ImGui::TableSetColumnIndex(5);if(ImGui::Button("Delete")){g_tas_ram_watches.erase(g_tas_ram_watches.begin()+i);if(g_tas_rng_watch==(int)i)g_tas_rng_watch=-1;else if(g_tas_rng_watch>(int)i)--g_tas_rng_watch;ImGui::PopID();break;}ImGui::PopID();}
        ImGui::EndTable();
    }
    ImGui::Separator();ImGui::TextUnformatted("RAM Search (physical guest RAM)");
    ImGui::SetNextItemWidth(150);ImGui::InputScalar("Value##search",ImGuiDataType_U32,&g_tas_search_value,nullptr,nullptr,"%08X",ImGuiInputTextFlags_CharsHexadecimal);ImGui::SameLine();int ssi=g_tas_search_size==1?0:g_tas_search_size==2?1:2;const char *ssizes[]={"8-bit","16-bit","32-bit"};ImGui::SetNextItemWidth(100);if(ImGui::Combo("Size##search",&ssi,ssizes,3))g_tas_search_size=1<<ssi;ImGui::SameLine();if(ImGui::Button("Exact Search"))TasRamSearchExact();ImGui::SameLine();if(ImGui::Button("Changed"))TasRamSearchRefine(0);ImGui::SameLine();if(ImGui::Button("Unchanged"))TasRamSearchRefine(1);ImGui::SameLine();if(ImGui::Button("Increased"))TasRamSearchRefine(2);ImGui::SameLine();if(ImGui::Button("Decreased"))TasRamSearchRefine(3);
    ImGui::Text("Results: %zu%s",g_tas_search_results.size(),g_tas_search_results.size()>=1000000?" (capped)":"");
    if (ImGui::BeginChild("##ramresults", ImVec2(0, 120), true)) {
        /* Only read values for rows that are actually visible. A million-result
         * search used to perform 500 guest-memory reads every ImGui frame even
         * though this child shows only a handful of rows. */
        const int shown = (int)std::min<size_t>(g_tas_search_results.size(), 500);
        ImGuiListClipper clipper;
        clipper.Begin(shown);
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                uint32_t addr = g_tas_search_results[(size_t)i];
                uint64_t v = 0;
                TasReadMemoryValue(addr, g_tas_search_size, &v);
                ImGui::PushID(i + 80000);
                ImGui::Text("%08X = %0*llX", addr, g_tas_search_size * 2,
                            (unsigned long long)v);
                ImGui::SameLine();
                if (ImGui::SmallButton("Watch")) {
                    TasRamWatch w;
                    w.address = addr;
                    w.size = g_tas_search_size;
                    char lab[64];
                    snprintf(lab, sizeof(lab), "%08X", addr);
                    w.label = lab;
                    g_tas_ram_watches.push_back(std::move(w));
                }
                ImGui::PopID();
            }
        }
        ImGui::EndChild();
    }
    ImGui::Separator();ImGui::Text("RNG history: %s",g_tas_rng_watch>=0&&g_tas_rng_watch<(int)g_tas_ram_watches.size()?g_tas_ram_watches[g_tas_rng_watch].label.c_str():"<select a watch>");ImGui::SameLine();if(ImGui::Button("Clear RNG History"))g_tas_rng_history.clear();
    if (ImGui::BeginChild("##rnghist", ImVec2(0, 100), true)) {
        ImGuiListClipper clipper;
        clipper.Begin((int)g_tas_rng_history.size());
        while (clipper.Step()) {
            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
                const size_t idx = g_tas_rng_history.size() - 1 - (size_t)row;
                const auto &entry = g_tas_rng_history[idx];
                ImGui::Text("Frame %llu  %016llX",
                            (unsigned long long)entry.first,
                            (unsigned long long)entry.second);
            }
        }
        ImGui::EndChild();
    }
    ImGui::End();
}

static void DrawTasHud()
{
    if(!g_tas_hud_enabled)return;
    ImGui::SetNextWindowPos(ImVec2(12.0f,g_main_menu_height+12.0f),ImGuiCond_Always);
    ImGuiWindowFlags flags=ImGuiWindowFlags_NoDecoration|ImGuiWindowFlags_AlwaysAutoResize|ImGuiWindowFlags_NoSavedSettings|ImGuiWindowFlags_NoFocusOnAppearing|ImGuiWindowFlags_NoNav|ImGuiWindowFlags_NoInputs;
    ImGui::SetNextWindowBgAlpha(0.70f);
    if(ImGui::Begin("##tas_hud",nullptr,flags)){
        const char *mode=xemu_tas_recording()?"REC":xemu_tas_playback()?"PLAY":runstate_is_running()?"RUN":"PAUSE";
        ImGui::Text("TAS %s  F:%llu  Lag:%llu  RR:%llu  B:%u",mode,(unsigned long long)xemu_tas_frame(),(unsigned long long)xemu_tas_lag_count(),(unsigned long long)g_tas_rerecord_count,g_tas_current_branch);
        ImGui::Text("%s  %s",g_tas_read_only?"READ ONLY":"READ/WRITE",g_tas_verify_failed?"DESYNC":g_tas_verify_status.c_str());
    }
    ImGui::End();
}

static void TasCheckRecoveryNotice()
{
    if (g_tas_recovery_notice_done) {
        return;
    }
    g_tas_recovery_notice_done = true;
    std::error_code ec;std::string rp=TasRecoveryPath();if(std::filesystem::exists(rp,ec))xemu_queue_notification("TAS recovery autosave found; TAS > Recovery can restore it if needed");
}

}

bool TasWindowsOpen()
{
    return g_tas_studio_open || g_tas_input_display_open ||
           g_tas_compatibility_open || g_tas_history_open ||
           g_tas_ram_tools_open;
}

void TasNotifySnapshotCreated()
{
    TasInvalidateSnapshotCache();
}

void DrawTasMenu()
{
        if (ImGui::BeginMenu("TAS"))
        {
            /* Recovery probing is intentionally lazy so merely compiling TAS
             * support does not touch the filesystem during normal emulation. */
            TasCheckRecoveryNotice();
            bool enabled = xemu_tas_enabled();
            if (ImGui::MenuItem("Enable TAS Mode", NULL, enabled)) {
                xemu_tas_set_enabled(!enabled);
            }
            bool deterministic = xemu_tas_deterministic_mode();
            if (ImGui::MenuItem("Deterministic TAS Mode (Experimental)", NULL,
                                deterministic)) {
                xemu_tas_set_deterministic_mode(!deterministic);
                if (!enabled) xemu_tas_set_enabled(true);
            }
            if (ImGui::MenuItem("Read-only Movie", NULL, g_tas_read_only)) {
                g_tas_read_only = !g_tas_read_only;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("TAS Studio / Piano Roll...")) g_tas_studio_open = true;
            if (ImGui::MenuItem("Input Display...")) g_tas_input_display_open = true;
            ImGui::Separator();
            if (ImGui::BeginMenu("Controller Input")) {
                for (int port = 0; port < XEMU_TAS_MAX_PORTS; ++port) {
                    char label[64]; snprintf(label, sizeof(label), "Latch Current XID - Port %d", port + 1);
                    if (ImGui::MenuItem(label)) {
                        uint8_t report[XEMU_TAS_XID_REPORT_SIZE];
                        if (xemu_tas_get_last_xid_report((uint8_t)port, report, sizeof(report))) {
                            if (!xemu_tas_enabled()) xemu_tas_set_enabled(true);
                            xemu_tas_set_xid_report((uint8_t)port, report, sizeof(report));
                        }
                    }
                }
                if (ImGui::MenuItem("Release All Overrides")) xemu_tas_clear_all_xid_reports();
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::TextDisabled("Movie (.xmt)");
            if (ImGui::MenuItem("New Movie")) TasNewMovie();
            if (ImGui::MenuItem("Open Movie...")) TasOpenMovie();
            if (ImGui::MenuItem("Save Movie")) TasSaveMovie();
            if (ImGui::MenuItem("Save Movie As...")) TasSaveMovieAs();
            ImGui::Separator();
            if (!xemu_tas_recording()) {
                if (ImGui::MenuItem("Record / Rerecord From Current Position")) TasStartContextualRecording();
                if (ImGui::MenuItem("New Movie From Current State")) TasStartFreshRecordingFromCurrentState();
                if (ImGui::MenuItem("Start Recording (Power-On / Reset)")) TasStartRecording(true);
            } else if (ImGui::MenuItem("Stop Recording")) {
                xemu_tas_stop_recording();
                TasSyncRecordingFromCore();
                TasAutosaveRecovery(true);
            }
            if (ImGui::MenuItem("Play Movie From Beginning")) {
                TasPlayMovieFromBeginning();
            }
            if (!xemu_tas_playback()) {
                if (ImGui::MenuItem("Play From Selected Frame")) TasStartPlayback((uint64_t)g_tas_selected_frame);
            } else if (ImGui::MenuItem("Stop Playback")) {
                TasStopPlaybackFromUi();
            }
            ImGui::Separator();
            ImGui::Text("Frame: %llu   Lag: %llu", (unsigned long long)xemu_tas_frame(),
                        (unsigned long long)xemu_tas_lag_count());
            if (ImGui::MenuItem("Frame Advance")) TasAdvanceFrames(1);
            if (ImGui::MenuItem("Frame Advance - Skip Lag")) TasAdvanceFrames(1, true);
            if (ImGui::MenuItem("Advance 10 Frames")) TasAdvanceFrames(10);
            if (ImGui::MenuItem("Advance 60 Frames")) TasAdvanceFrames(60);
            if (ImGui::MenuItem("Advance 300 Frames")) TasAdvanceFrames(300);
            if (ImGui::MenuItem("Go To Frame...")) {
                g_tas_goto_frame_value = g_tas_selected_frame;
                g_tas_goto_dialog_requested = true;
            }
            ImGui::Separator();
            if (ImGui::MenuItem("Undo Last TAS State Load")) TasUndoStateLoad();
            if (ImGui::MenuItem("Save Bulletproof Recovery Now")) TasAutosaveRecovery(true);
            if (ImGui::MenuItem("Rewind Cache", NULL, g_tas_rewind_enabled)) {
                g_tas_rewind_enabled = !g_tas_rewind_enabled;
            }
            if (ImGui::MenuItem("Rewind 1 Frame")) TasRewindFrames(1);
            if (ImGui::MenuItem("Rewind 10 Frames")) TasRewindFrames(10);
            if (ImGui::MenuItem("Rewind 60 Frames")) TasRewindFrames(60);
            if (ImGui::MenuItem("Rewind 300 Frames")) TasRewindFrames(300);
            if (ImGui::MenuItem("Greenzone Cache", NULL, g_tas_greenzone_enabled)) g_tas_greenzone_enabled = !g_tas_greenzone_enabled;
            if (ImGui::MenuItem("Seek To Selected Frame")) TasSeekFrame((uint64_t)g_tas_selected_frame);
            ImGui::Separator();
            if (ImGui::MenuItem("Movie Properties / Comments")) { g_tas_properties_open = true; g_tas_studio_open = true; }
            if (ImGui::MenuItem("Bookmarks / Markers")) { g_tas_markers_open = true; g_tas_studio_open = true; }
            if (ImGui::MenuItem("Movie Compatibility...")) g_tas_compatibility_open = true;
            if (ImGui::MenuItem("Movie Backup History...")) g_tas_history_open = true;
            if (ImGui::MenuItem("Recover Autosave")) TasRecoverAutosave();
            if (ImGui::MenuItem("Compare Movie...")) TasCompareDialog();
            if (ImGui::MenuItem("Export Movie CSV...")) TasExportCsvDialog();
            if (ImGui::MenuItem("Import Movie CSV...")) TasImportCsvDialog();
            ImGui::Separator();
            if (ImGui::MenuItem("RAM Watch / Search / RNG...")) g_tas_ram_tools_open = true;
            if (ImGui::MenuItem("TAS HUD Overlay", NULL, g_tas_hud_enabled)) g_tas_hud_enabled = !g_tas_hud_enabled;
            ImGui::Separator();
            ImGui::TextDisabled(deterministic
                ? "Strict Sync: canonical VBLANK checkpoints; Fast Forward locked off."
                : "Strict Sync / Deterministic TAS is available above.");
            ImGui::EndMenu();
        }
}

void ShowTasWindows()
{
    /* Keep a compiled-in but completely unused TAS installation out of the
     * normal frontend path. This also defers recovery-file filesystem I/O
     * until the user actually enables/opens TAS tooling. */
    const bool runtime_active = xemu_tas_enabled() || xemu_tas_recording() ||
                                xemu_tas_playback() || xemu_tas_overdub();
    const bool ui_active = TasWindowsOpen() || g_tas_hud_enabled ||
                           g_tas_goto_dialog_requested;
    const bool maintenance_active =
        !g_tas_snapshot_delete_queue.empty() || g_tas_seek_continue_pending ||
        g_tas_seek_completion_pending || xemu_tas_seek_catchup() ||
        g_tas_step_completion_pending || g_tas_verify_mode != TasVerifyMode::Idle ||
        g_tas_power_on_reset_pending || g_tas_rng_watch >= 0 ||
        g_tas_timeline_mutation.active || g_tas_branch_switch.active ||
        g_tas_pending_overdub_start.active;
    if (!runtime_active && !ui_active && !maintenance_active) {
        return;
    }

    /* Recording/playback bookkeeping must continue even with TAS Studio closed. */
    TasPowerOnResetTick();
    TasSyncRecordingFromCore();
    TasUpdateRewindCache();
    TasServiceDeferredSnapshotDeletes();
    TasStrictResimTick();
    TasSeekContinueTick();
    TasFrameStepCompletionTick();
    TasCheckCoreDesync();
    TasVerifierTick();
    TasUpdateRamWatches();
    TasCheckRecoveryNotice();
    DrawTasStudio();
    /* Commit atomic editor transactions only after the Studio has drawn, so an
     * active analog drag remains paused until the user releases the control. */
    TasTimelineMutationTick();
    DrawTasInputDisplay();
    DrawTasGoToFrameDialog();
    DrawTasCompatibilityWindow();
    DrawTasHistoryWindow();
    DrawTasRamToolsWindow();
    DrawTasHud();
}

void FeatureTasDrawGeneralSettings()
{
    // TAS configuration and status now live exclusively under the TAS menu.
    // Keep the existing native General hook neutral so no duplicate TAS block
    // appears in Settings.
}
