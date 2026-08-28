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

struct TasFrame {
    std::array<TasXidReport, XEMU_TAS_MAX_PORTS> xid{};
};
static_assert(sizeof(TasFrame) == XEMU_TAS_FRAME_REPORT_BYTES,
              "TasFrame must stay byte-identical to four packed XID reports");

struct TasStateMeta {
    bool valid = false;
    uint64_t frame = 0;
    uint32_t branch_id = 0;
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
};

struct TasRewindCheckpoint {
    bool valid = false;
    uint64_t frame = 0;
    uint32_t branch_id = 0;
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
static TasFrame g_tas_clipboard;
static bool g_tas_clipboard_valid = false;
static bool g_tas_follow_frame = true;
static std::string g_tas_movie_path;
static bool g_tas_goto_dialog_requested = false;
static int g_tas_goto_frame_value = 0;
static std::vector<uint8_t> g_tas_lag_flags(1, 0);
static bool g_tas_read_only = true;
static bool g_tas_power_on_recording = true;
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
static bool g_tas_rewind_enabled = false;
static int g_tas_rewind_interval = 120;
static int g_tas_rewind_distance = 60;
static int g_tas_rewind_slot = 0;
static uint64_t g_tas_rewind_next_frame = 0;
static std::chrono::steady_clock::time_point g_tas_rewind_next_host_checkpoint{};
static std::array<TasRewindCheckpoint, 64> g_tas_rewind_points{};
static std::string g_tas_undo_snapshot_name;
static std::deque<std::string> g_tas_snapshot_delete_queue;
static std::unordered_set<std::string> g_tas_snapshot_delete_queued;

/* Movie editing / TAStudio-style selection. */
static int g_tas_selection_anchor = 0;
static int g_tas_selection_end = 0;
static std::vector<TasFrame> g_tas_clipboard_frames;
static std::vector<uint8_t> g_tas_clipboard_lag;
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

static void TasAutosaveRecovery(bool force);

static void TasPushMovieToCore();
static bool TasSaveMovieToPathInternal(const char *path, bool set_current_path, bool notify);
static bool TasLoadMovieFromPath(const char *path);
static void TasStartPlayback(uint64_t frame);
static void TasInvalidateGreenzoneFrom(uint64_t frame);
static bool TasSeekFrame(uint64_t target);
static void TasStopOverdub();

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
    snap.lag.assign(g_tas_lag_flags.begin() + first, g_tas_lag_flags.begin() + last + 1);
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

static void TasPushUndoRange(const char *description, int first, int last)
{
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
        invalidate_from = (uint64_t)first;
    } else {
        g_tas_frames = snap.frames;
        g_tas_lag_flags = snap.lag;
        g_tas_chapters = snap.chapters;
        g_tas_markers = snap.markers;
        if (g_tas_frames.empty()) g_tas_frames.resize(1);
        if (g_tas_lag_flags.size() < g_tas_frames.size()) {
            g_tas_lag_flags.resize(g_tas_frames.size(), 0);
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
    return true;
}

static void TasUndoEdit()
{
    if (g_tas_undo_stack.empty()) return;
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
    g_tas_movie_dirty = true;
    ++g_tas_movie_revision;
    g_tas_core_pushed_revision = UINT64_MAX;
    g_tas_visible_frame_cache_dirty = true;
    TasInvalidateGreenzoneFrom(first_frame);
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

static uint64_t TasComputeStateHash()
{
    /* Hash guest RAM in chunks plus the canonical movie frame input. This is
     * deliberately an explicit verifier checkpoint, not a per-frame hot-path
     * hash. */
    const uint64_t ram_size = xemu_guest_ram_size();
    static std::vector<uint8_t> buf(1 << 20);
    uint64_t h = 1469598103934665603ULL;
    for (uint64_t off = 0; off < ram_size; off += buf.size()) {
        size_t n = (size_t)std::min<uint64_t>(buf.size(), ram_size - off);
        ssize_t got = xemu_phys_read((uint32_t)off, buf.data(), n);
        if (got <= 0) break;
        /* XXH3 uses the host's optimized SIMD implementation. Combine one
         * 64-bit hash per chunk instead of byte-walking all 128 MiB of Xbox RAM
         * in C++ for every verifier checkpoint. */
        const uint64_t chunk_hash = fast_hash(buf.data(), (size_t)got);
        h = TasFnv1a64(&chunk_hash, sizeof(chunk_hash), h);
    }
    uint64_t frame = xemu_tas_frame();
    h = TasFnv1a64(&frame, sizeof(frame), h);
    if (frame < g_tas_frames.size()) {
        h = TasFnv1a64(&g_tas_frames[(size_t)frame], sizeof(TasFrame), h);
        uint8_t lag = frame < g_tas_lag_flags.size() ? g_tas_lag_flags[(size_t)frame] : 0;
        h = TasFnv1a64(&lag, 1, h);
    }
    return h;
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
    if (!g_tas_input_display_open) return;
    if (!ImGui::Begin("TAS Input Display", &g_tas_input_display_open)) {
        ImGui::End();
        return;
    }
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
    g_tas_branches.push_back(std::move(archived));
}

static void TasArchiveCurrentBranch(uint64_t fork_frame, const char *reason)
{
    TasBranch archived;
    archived.id = g_tas_current_branch;
    archived.parent = g_tas_current_parent;
    archived.fork_frame = fork_frame;
    archived.name = reason ? reason : "Preserved future";

    /* Transfer ownership of the full old branch instead of copying it, then
     * copy back only the prefix the new branch actually needs. This turns a
     * potentially huge full-movie duplicate into O(prefix) work. */
    archived.frames = std::move(g_tas_frames);
    archived.lag = std::move(g_tas_lag_flags);
    const size_t keep = std::min<size_t>((size_t)fork_frame + 1, archived.frames.size());
    g_tas_frames.assign(archived.frames.begin(), archived.frames.begin() + keep);
    g_tas_lag_flags.assign(archived.lag.begin(), archived.lag.begin() +
                          std::min(keep, archived.lag.size()));
    if (g_tas_frames.empty()) g_tas_frames.resize(1);
    if (g_tas_lag_flags.size() < g_tas_frames.size()) {
        g_tas_lag_flags.resize(g_tas_frames.size(), 0);
    }
    g_tas_branches.push_back(std::move(archived));

    g_tas_current_parent = g_tas_current_branch;
    g_tas_current_branch = g_tas_next_branch_id++;
    g_tas_visible_frame_cache_dirty = true;
}

static void TasResumeMovieAtFrame(uint64_t frame, bool resume_vm)
{
    if (!xemu_tas_enabled()) xemu_tas_set_enabled(true);
    if (g_tas_frames.empty()) {
        g_tas_frames.resize(1);
        g_tas_lag_flags.resize(1, 0);
    }
    frame = std::min<uint64_t>(frame, g_tas_frames.size() - 1);

    if (!g_tas_read_only && frame + 1 < g_tas_frames.size()) {
        TasArchiveCurrentBranch(frame, "Preserved future before rerecord");
        g_tas_frames.resize((size_t)frame + 1);
        g_tas_lag_flags.resize((size_t)frame + 1);
        ++g_tas_rerecord_count;
    }

    g_tas_selected_frame = (int)std::min<uint64_t>(frame, INT_MAX);
    xemu_tas_set_frame(frame);

    if (g_tas_read_only) {
        xemu_tas_set_playback_movie(g_tas_frames.data(), g_tas_lag_flags.data(),
                                    g_tas_frames.size());
        xemu_tas_start_playback(frame);
    } else {
        xemu_tas_clear_recording();
        g_tas_record_synced = 0;
        g_tas_record_replace_placeholder = false;
        xemu_tas_start_recording(true);
    }
    if (resume_vm && !runstate_is_running()) {
        vm_start();
    }
}

static void TasSaveSelectedState()
{
    char state_name[64];
    if (!TasBuildStateName(state_name, sizeof(state_name))) {
        xemu_queue_error_message("TAS state: no running XBE/title ID is available yet");
        return;
    }

    Error *err = NULL;
    xemu_snapshots_save(state_name, &err);
    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_free(err);
        return;
    }

    TasSnapshotCacheInsert(state_name);
    uint32_t saved_title_id = 0;
    if (TasGetCurrentTitleId(&saved_title_id) &&
        g_tas_state_slot_cache_valid && g_tas_state_slot_cache_title == saved_title_id) {
        g_tas_state_slot_cache[(size_t)g_tas_state_slot] = true;
    }
    g_tas_state_meta[g_tas_state_slot].valid = true;
    g_tas_state_meta[g_tas_state_slot].frame = xemu_tas_frame();
    g_tas_state_meta[g_tas_state_slot].branch_id = g_tas_current_branch;
    TasAutosaveRecovery(true);

    char msg[160];
    snprintf(msg, sizeof(msg), "Saved TAS state %02d (%s) at frame %llu",
             g_tas_state_slot, state_name,
             (unsigned long long)g_tas_state_meta[g_tas_state_slot].frame);
    xemu_queue_notification(msg);
}

static void TasLoadSelectedState()
{
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

    TasAutosaveRecovery(true);

    uint32_t title_id = 0;
    TasGetCurrentTitleId(&title_id);
    char undo_name[64];
    snprintf(undo_name, sizeof(undo_name), "%08X_TAS_UNDO_LOAD", title_id);
    Error *undo_err = NULL;
    xemu_snapshots_save_no_thumbnail(undo_name, &undo_err);
    if (!undo_err) {
        TasSnapshotCacheInsert(undo_name);
        g_tas_undo_snapshot_name = undo_name;
    } else {
        error_free(undo_err);
    }

    Error *err = NULL;
    bool was_running = false;
    const bool loaded = xemu_snapshots_load_paused(state_name, &was_running, &err);
    if (err || !loaded) {
        if (err) {
            xemu_queue_error_message(error_get_pretty(err));
            error_free(err);
        } else {
            xemu_queue_error_message("TAS state load failed");
        }
        return;
    }

    /* The VM is deliberately still stopped here. Install the movie cursor and
     * input mode before any restored guest instruction can execute. This makes
     * Resume Recording From Savestate deterministic instead of racing VM resume. */
    if (g_tas_state_meta[g_tas_state_slot].valid) {
        TasResumeMovieAtFrame(g_tas_state_meta[g_tas_state_slot].frame, false);
    } else {
        xemu_queue_notification("Loaded TAS state; movie frame metadata was not available");
    }
    if (was_running && !runstate_is_running()) {
        vm_start();
    }
}

static void TasUndoStateLoad()
{
    if (g_tas_undo_snapshot_name.empty() || !TasSnapshotExists(g_tas_undo_snapshot_name.c_str())) {
        xemu_queue_error_message("No TAS savestate load to undo");
        return;
    }
    Error *err = NULL;
    xemu_snapshots_load(g_tas_undo_snapshot_name.c_str(), &err);
    if (err) {
        xemu_queue_error_message(error_get_pretty(err));
        error_free(err);
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
    g_tas_rewind_points = {};
    g_tas_rewind_slot = 0;
    g_tas_rewind_next_frame = 0;
    g_tas_rewind_next_host_checkpoint = {};
    g_tas_markers.clear();
    g_tas_properties = {};
    g_tas_loaded_environment = {};
    g_tas_verify_baseline.clear();
    g_tas_verify_mode = TasVerifyMode::Idle;
    g_tas_verify_status = "Not verified";
    g_tas_verify_failed = false;
    g_tas_verify_start_snapshot.clear();
    g_tas_verify_branch = 0;
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
    g_tas_overdub_ui_active = false;
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
    xemu_queue_notification("Created new TAS movie (power-on recording is the default)");
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

static bool TasSaveMovieToPathInternal(const char *path, bool set_current_path, bool notify)
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
    g_tas_movie_revision = 0;
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
        g_tas_movie_revision = 0;
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

        if (ok && g_tas_apply_movie_settings) {
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

        if (ok && saved_commit != (xemu_commit ? xemu_commit : "")) {
            xemu_queue_notification(".xmt was created by a different Xemu commit; deterministic verification is recommended");
        }
        if (ok && !disc_path.empty()) {
            char *current_disc = xemu_get_currently_loaded_disc_path();
            bool mismatch = !current_disc || disc_path != current_disc;
            if (current_disc) g_free(current_disc);
            if (mismatch) xemu_queue_notification(".xmt disc path differs from the currently loaded disc");
        }
        if (ok) {
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
    g_tas_movie_path = path;
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
    xemu_queue_notification("Loaded TAS movie");
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
    if (xemu_tas_set_playback_movie(g_tas_frames.data(), g_tas_lag_flags.data(),
                                    g_tas_frames.size())) {
        g_tas_core_pushed_revision = g_tas_movie_revision;
        g_tas_core_pushed_frame_count = g_tas_frames.size();
    }
}

static void TasStartPlayback(uint64_t frame)
{
    if (!xemu_tas_enabled()) xemu_tas_set_enabled(true);
    TasPushMovieToCore();
    frame = std::min<uint64_t>(frame, g_tas_frames.size() - 1);
    xemu_tas_set_frame(frame);
    if (xemu_tas_start_playback(frame)) {
        if (!runstate_is_running()) vm_start();
        xemu_queue_notification("TAS movie playback started");
    }
}

static void TasStartRecording(bool power_on)
{
    if (g_tas_overdub_ui_active) TasStopOverdub();
    if (g_tas_read_only) {
        g_tas_read_only = false;
        xemu_queue_notification("Read-only disabled for recording");
    }
    xemu_tas_stop_playback();
    xemu_tas_clear_recording();
    g_tas_record_synced = 0;
    g_tas_last_autosave_record_count = 0;
    g_tas_record_replace_placeholder = power_on;
    g_tas_power_on_recording = power_on;

    if (power_on) {
        g_tas_frames.assign(1, TasFrame{});
        g_tas_lag_flags.assign(1, 0);
        g_tas_selected_frame = 0;
        g_tas_rerecord_count = 0;
        g_tas_branches.clear();
        g_tas_current_branch = 0;
        g_tas_current_parent = UINT32_MAX;
        g_tas_next_branch_id = 1;
        g_tas_visible_frame_cache_dirty = true;
        xemu_tas_set_frame(0);
        ActionReset();
    }
    xemu_tas_start_recording(true);
    if (!runstate_is_running()) vm_start();
    xemu_queue_notification(power_on ? "TAS recording started from power-on/reset" :
                                        "TAS recording started from current state");
}

static void TasStartOverdub()
{
    if (g_tas_frames.empty()) return;
    if (g_tas_read_only) g_tas_read_only = false;
    TasPushUndo("Punch-in / overdub recording");
    TasPushMovieToCore();

    uint16_t digital_mask = 0;
    uint8_t analog_mask = 0;
    uint8_t stick_mask = 0;
    for (int i = 0; i < 8; ++i) if (g_tas_overdub_fields[i]) digital_mask |= (uint16_t)(1u << i);
    for (int i = 0; i < 8; ++i) if (g_tas_overdub_fields[8 + i]) analog_mask |= (uint8_t)(1u << i);
    for (int i = 0; i < 4; ++i) if (g_tas_overdub_fields[16 + i]) stick_mask |= (uint8_t)(1u << i);

    g_tas_overdub_start_frame = (uint64_t)std::clamp(g_tas_selected_frame, 0,
                                                      (int)g_tas_frames.size() - 1);
    g_tas_overdub_synced = 0;
    g_tas_record_synced = 0;
    g_tas_overdub_ui_active = xemu_tas_start_overdub(g_tas_overdub_start_frame,
        (uint8_t)g_tas_port, digital_mask, analog_mask, stick_mask);
    if (g_tas_overdub_ui_active) {
        xemu_tas_set_frame(g_tas_overdub_start_frame);
        if (!runstate_is_running()) vm_start();
        xemu_queue_notification("TAS punch-in/overdub recording started");
    } else {
        xemu_queue_error_message("Could not start TAS overdub; load/push a movie first");
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

    bool changed = false;
    if (!g_tas_overdub_ui_active) {
        const uint64_t remaining = count - g_tas_record_synced;
        g_tas_frames.reserve(g_tas_frames.size() + (size_t)remaining);
        g_tas_lag_flags.reserve(g_tas_lag_flags.size() + (size_t)remaining);
    }

    while (g_tas_record_synced < count) {
        const uint64_t wanted = std::min<uint64_t>(kBatchFrames,
                                                   count - g_tas_record_synced);
        const uint64_t copied = xemu_tas_copy_recorded_frames(
            g_tas_record_synced, wanted,
            reports.data(), (size_t)wanted * XEMU_TAS_FRAME_REPORT_BYTES,
            lag.data(), (size_t)wanted);
        if (!copied) {
            break;
        }

        for (uint64_t j = 0; j < copied; ++j) {
            const uint8_t *packed = reports.data() + j * XEMU_TAS_FRAME_REPORT_BYTES;
            const bool lagged = lag[(size_t)j] != 0;

            if (g_tas_record_replace_placeholder) {
                g_tas_frames.clear();
                g_tas_lag_flags.clear();
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
                    ++g_tas_overdub_synced;
                } else {
                    TasStopOverdub();
                    break;
                }
            } else {
                g_tas_frames.push_back(std::move(frame));
                g_tas_lag_flags.push_back(lagged ? 1 : 0);
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

static void TasAdvanceFrames(uint32_t count, bool skip_lag = false)
{
    if (!count) return;
    if (!xemu_tas_enabled()) xemu_tas_set_enabled(true);
    if (runstate_is_running()) vm_stop(RUN_STATE_PAUSED);
    xemu_tas_request_frame_advance_ex(count, skip_lag);
    vm_start();
}

static void TasInsertMacro()
{
    TasPushUndo("Insert combo / macro");
    int start = std::clamp(g_tas_selected_frame, 0, (int)g_tas_frames.size());
    int total = std::max(1, g_tas_macro_repeats) *
                (std::max(1, g_tas_macro_press_frames) + std::max(0, g_tas_macro_gap_frames));
    if (start + total > (int)g_tas_frames.size()) {
        g_tas_frames.resize(start + total);
        g_tas_lag_flags.resize(start + total, 0);
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

static void TasUpdateRewindCache()
{
    if ((!g_tas_rewind_enabled && !g_tas_greenzone_enabled) ||
        !xemu_tas_enabled() || !runstate_is_running()) return;
    uint64_t frame = xemu_tas_frame();
    if (frame < g_tas_rewind_next_frame) return;

    /* Snapshot creation is inherently nontrivial I/O. Guest-frame intervals
     * collapse under Fast Forward, so enforce a small host-time floor to keep
     * Greenzone/Rewind from generating many QCOW2 states per real second. */
    const auto now = std::chrono::steady_clock::now();
    if (g_tas_rewind_next_host_checkpoint.time_since_epoch().count() != 0 &&
        now < g_tas_rewind_next_host_checkpoint) {
        return;
    }

    uint32_t title_id = 0;
    if (!TasGetCurrentTitleId(&title_id)) return;
    int capacity = std::clamp(g_tas_greenzone_enabled ? g_tas_greenzone_capacity :
                              (int)g_tas_rewind_points.size(), 4,
                              (int)g_tas_rewind_points.size());
    int slot = g_tas_rewind_slot % capacity;
    char name[64];
    snprintf(name, sizeof(name), "%08X_TAS_GZ_%02d", title_id, slot);

    if (TasSnapshotExists(name)) {
        Error *del_err = NULL;
        xemu_snapshots_delete(name, &del_err);
        if (del_err) {
            error_free(del_err);
        } else {
            TasSnapshotCacheErase(name);
        }
    }
    Error *err = NULL;
    xemu_snapshots_save_no_thumbnail(name, &err);
    if (!err) {
        TasSnapshotCacheInsert(name);
        g_tas_rewind_points[slot].valid = true;
        g_tas_rewind_points[slot].frame = frame;
        g_tas_rewind_points[slot].branch_id = g_tas_current_branch;
        g_tas_rewind_points[slot].snapshot_name = name;
        g_tas_rewind_slot = (slot + 1) % capacity;
    } else {
        error_free(err);
    }
    int interval = g_tas_greenzone_enabled ? g_tas_greenzone_interval : g_tas_rewind_interval;
    g_tas_rewind_next_frame = frame + (uint64_t)std::max(30, interval);
    g_tas_rewind_next_host_checkpoint = now + std::chrono::seconds(2);
}

static bool TasSeekFrame(uint64_t target)
{
    if (g_tas_frames.empty()) return false;
    target = std::min<uint64_t>(target, g_tas_frames.size() - 1);
    const TasRewindCheckpoint *best = nullptr;
    for (const auto &cp : g_tas_rewind_points) {
        if (cp.valid && cp.branch_id == g_tas_current_branch && cp.frame <= target &&
            (!best || cp.frame > best->frame)) best = &cp;
    }

    uint64_t start = 0;
    if (best) {
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
    } else if (g_tas_power_on_recording) {
        /* Power-on movies always have a canonical reset baseline. */
        ActionReset();
        start = 0;
    } else {
        xemu_queue_error_message("No greenzone/state checkpoint exists before that frame");
        return false;
    }

    if (!xemu_tas_enabled()) xemu_tas_set_enabled(true);
    xemu_tas_set_frame(start);
    TasPushMovieToCore();
    if (start < g_tas_frames.size()) xemu_tas_start_playback(start);
    uint64_t delta = target - start;
    if (delta) {
        xemu_tas_request_frame_advance((uint32_t)std::min<uint64_t>(delta, UINT32_MAX));
        vm_start();
    } else if (runstate_is_running()) {
        vm_stop(RUN_STATE_PAUSED);
    }
    g_tas_selected_frame = (int)std::min<uint64_t>(target, INT_MAX);
    g_tas_selection_anchor = g_tas_selected_frame;
    g_tas_selection_end = g_tas_selected_frame;
    g_tas_follow_frame = false;
    return true;
}

static void TasRewindFrames(uint64_t distance)
{
    uint64_t current = xemu_tas_frame();
    uint64_t target = current > distance ? current - distance : 0;
    TasSeekFrame(target);
}

static void TasStartVerifier(bool capture)
{
    if (g_tas_frames.empty()) return;
    if (!xemu_tas_enabled()) xemu_tas_set_enabled(true);
    xemu_tas_set_deterministic_mode(true);
    if (runstate_is_running()) vm_stop(RUN_STATE_PAUSED);

    if (capture) {
        uint32_t title_id = 0;
        if (!TasGetCurrentTitleId(&title_id)) {
            xemu_queue_error_message("Determinism verifier requires a running XBE/title ID");
            return;
        }
        char name[64];
        snprintf(name, sizeof(name), "%08X_TAS_VERIFY_BASE", title_id);
        Error *err = NULL;
        xemu_snapshots_save_no_thumbnail(name, &err);
        if (err) {
            xemu_queue_error_message(error_get_pretty(err));
            error_free(err);
            return;
        }
        TasSnapshotCacheInsert(name);
        g_tas_verify_start_snapshot = name;
        g_tas_verify_start_frame = std::min<uint64_t>(xemu_tas_frame(),
                                                       g_tas_frames.size() - 1);
        g_tas_verify_branch = g_tas_current_branch;
        g_tas_verify_baseline.clear();
    } else {
        if (g_tas_current_branch != g_tas_verify_branch) {
            xemu_queue_error_message(
                "Verifier baseline belongs to a different movie branch. Switch to that branch first.");
            return;
        }
        if (g_tas_verify_baseline.empty() || g_tas_verify_start_snapshot.empty() ||
            !TasSnapshotExists(g_tas_verify_start_snapshot.c_str())) {
            xemu_queue_error_message(
                "No local verifier start-state is available. Capture a baseline first.");
            return;
        }
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
                xemu_queue_error_message("Could not restore verifier baseline state");
            }
            return;
        }
        xemu_tas_set_frame(g_tas_verify_start_frame);
    }

    g_tas_verify_mode = capture ? TasVerifyMode::Capture : TasVerifyMode::Verify;
    g_tas_verify_failed = false;
    g_tas_verify_first_bad_frame = UINT64_MAX;
    g_tas_verify_index = 0;
    g_tas_verify_next_frame = g_tas_verify_start_frame;
    g_tas_verify_status = capture ? "Capturing RAM/input baseline"
                                  : "Verifying RAM/input baseline";

    TasPushMovieToCore();
    if (!xemu_tas_start_playback(g_tas_verify_start_frame)) {
        g_tas_verify_mode = TasVerifyMode::Idle;
        g_tas_verify_status = "Could not start verifier movie playback";
    }
}

static void TasVerifierTick()
{
    if (g_tas_verify_mode == TasVerifyMode::Idle || g_tas_frames.empty()) return;
    uint64_t frame = std::min<uint64_t>(xemu_tas_frame(), g_tas_frames.size() - 1);

    /* The verifier owns the VM in short deterministic chunks. Hash only while
     * paused so the RAM image cannot change underneath the checksum. */
    if (runstate_is_running()) return;

    uint64_t hash = TasComputeStateHash();
    if (g_tas_verify_mode == TasVerifyMode::Capture) {
        g_tas_verify_baseline.push_back({frame, hash});
    } else {
        while (g_tas_verify_index < g_tas_verify_baseline.size() &&
               g_tas_verify_baseline[g_tas_verify_index].frame < frame) ++g_tas_verify_index;
        if (g_tas_verify_index >= g_tas_verify_baseline.size() ||
            g_tas_verify_baseline[g_tas_verify_index].frame != frame) {
            g_tas_verify_status = "Baseline has no checkpoint for current frame";
            g_tas_verify_mode = TasVerifyMode::Idle;
            return;
        }
        uint64_t expected = g_tas_verify_baseline[g_tas_verify_index].hash;
        if (expected != hash) {
            g_tas_verify_failed = true;
            g_tas_verify_first_bad_frame = frame;
            g_tas_verify_expected = expected;
            g_tas_verify_actual = hash;
            g_tas_verify_status = "DESYNC detected at frame " + std::to_string(frame);
            g_tas_verify_mode = TasVerifyMode::Idle;
            return;
        }
        ++g_tas_verify_index;
    }

    if (frame >= g_tas_frames.size() - 1) {
        g_tas_verify_status = g_tas_verify_mode == TasVerifyMode::Capture
            ? "Baseline captured" : "Verification passed";
        g_tas_verify_mode = TasVerifyMode::Idle;
        xemu_tas_stop_playback();
        return;
    }

    uint64_t next = std::min<uint64_t>(frame + (uint64_t)std::max(1, g_tas_verify_interval),
                                       g_tas_frames.size() - 1);
    g_tas_verify_next_frame = next;
    uint64_t delta = next - frame;
    xemu_tas_request_frame_advance((uint32_t)std::min<uint64_t>(delta, UINT32_MAX));
    vm_start();
}


static void TasCopySelection()
{
    auto [a,b] = TasSelectionBounds();
    g_tas_clipboard_frames.assign(g_tas_frames.begin() + a, g_tas_frames.begin() + b + 1);
    g_tas_clipboard_lag.assign(g_tas_lag_flags.begin() + a, g_tas_lag_flags.begin() + b + 1);
}

static void TasDeleteSelection()
{
    if (g_tas_frames.size() <= 1) return;
    auto [a,b] = TasSelectionBounds();
    TasPushUndo("Delete frame range");
    g_tas_frames.erase(g_tas_frames.begin() + a, g_tas_frames.begin() + b + 1);
    if (b < (int)g_tas_lag_flags.size()) g_tas_lag_flags.erase(g_tas_lag_flags.begin() + a, g_tas_lag_flags.begin() + b + 1);
    if (g_tas_frames.empty()) { g_tas_frames.resize(1); g_tas_lag_flags.assign(1,0); }
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
    TasPushUndo(insert ? "Paste insert" : "Paste overwrite");
    if (insert) {
        g_tas_frames.insert(g_tas_frames.begin() + at, g_tas_clipboard_frames.begin(), g_tas_clipboard_frames.end());
        g_tas_lag_flags.insert(g_tas_lag_flags.begin() + at, g_tas_clipboard_lag.begin(), g_tas_clipboard_lag.end());
    } else {
        size_t need = (size_t)at + g_tas_clipboard_frames.size();
        if (need > g_tas_frames.size()) { g_tas_frames.resize(need); g_tas_lag_flags.resize(need,0); }
        std::copy(g_tas_clipboard_frames.begin(), g_tas_clipboard_frames.end(), g_tas_frames.begin() + at);
        for (size_t i=0;i<g_tas_clipboard_lag.size();++i) g_tas_lag_flags[(size_t)at+i]=g_tas_clipboard_lag[i];
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
    if(frames.empty()){xemu_queue_error_message("CSV contained no valid TAS frames");return;} TasPushUndo("Import CSV"); g_tas_frames=std::move(frames);g_tas_lag_flags=std::move(lag);g_tas_selected_frame=0;g_tas_selection_anchor=g_tas_selection_end=0;TasMarkMovieEdited(0);xemu_queue_notification("Imported TAS CSV");
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
    if (!g_tas_studio_open) return;
    ImGui::SetNextWindowSize(ImVec2(1180, 640), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("TAS Studio - XMT Piano Roll", &g_tas_studio_open,
                      ImGuiWindowFlags_MenuBar)) {
        ImGui::End();
        return;
    }

    if (g_tas_frames.empty()) {
        g_tas_frames.resize(1);
        g_tas_lag_flags.resize(1, 0);
    }
    if (g_tas_lag_flags.size() < g_tas_frames.size()) {
        g_tas_lag_flags.resize(g_tas_frames.size(), 0);
    }

    const uint64_t tas_frame = xemu_tas_frame();
    static bool scroll_to_followed_frame = false;
    if (xemu_tas_enabled() && g_tas_follow_frame) {
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
                if (ImGui::MenuItem("Record From Power-On")) TasStartRecording(true);
                if (ImGui::MenuItem("Record From Current State")) TasStartRecording(false);
            } else if (ImGui::MenuItem("Stop Recording")) {
                xemu_tas_stop_recording();
                TasSyncRecordingFromCore();
                TasAutosaveRecovery(true);
            }
            if (!xemu_tas_playback()) {
                if (ImGui::MenuItem("Play From Selected Frame"))
                    TasStartPlayback((uint64_t)g_tas_selected_frame);
            } else if (ImGui::MenuItem("Stop Playback")) {
                xemu_tas_stop_playback();
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
                TasPushUndo("Insert frame");
                g_tas_frames.insert(g_tas_frames.begin() + at, TasFrame{});
                g_tas_lag_flags.insert(g_tas_lag_flags.begin() + at, 0);
                TasSetSelection(at, false);
                TasMarkMovieEdited((uint64_t)at);
            }
            if (ImGui::MenuItem("Append 60 Blank Frames")) {
                TasPushUndo("Append 60 blank frames");
                size_t old = g_tas_frames.size();
                g_tas_frames.resize(old + 60);
                g_tas_lag_flags.resize(g_tas_frames.size(), 0);
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
            if (ImGui::MenuItem("Seek To Selected (Greenzone)"))
                TasSeekFrame((uint64_t)g_tas_selected_frame);
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
        const bool running_compact = runstate_is_running();
        if (ImGui::Button(running_compact ? "Pause" : "Resume")) ActionTogglePause();
        ImGui::SameLine(); if (ImGui::Button("+1")) TasAdvanceFrames(1);
        ImGui::SameLine(); if (ImGui::Button("+1 Skip Lag")) TasAdvanceFrames(1, true);
        ImGui::SameLine(); if (ImGui::Button("+10")) TasAdvanceFrames(10);
        ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
        if (!xemu_tas_recording()) {
            if (ImGui::Button("Record")) TasStartRecording(true);
        } else if (ImGui::Button("Stop Rec")) {
            xemu_tas_stop_recording(); TasSyncRecordingFromCore(); TasAutosaveRecovery(true);
        }
        ImGui::SameLine();
        if (!xemu_tas_playback()) {
            if (ImGui::Button("Play")) TasStartPlayback((uint64_t)g_tas_selected_frame);
        } else if (ImGui::Button("Stop Play")) xemu_tas_stop_playback();
        ImGui::SameLine(); if (ImGui::Button("Save Movie")) TasSaveMovie();
        ImGui::SameLine(); ImGui::Checkbox("Follow", &g_tas_follow_frame);
        ImGui::SameLine(); ImGui::Checkbox("RO", &g_tas_read_only);
        ImGui::SameLine();
        bool det_compact = xemu_tas_deterministic_mode();
        if (ImGui::Checkbox("Det", &det_compact)) xemu_tas_set_deterministic_mode(det_compact);

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
                char label[32]; snprintf(label, sizeof(label), "%02d%s", slot, compact_state_slots[slot] ? " [saved]" : "");
                if (ImGui::Selectable(label, slot == g_tas_state_slot)) g_tas_state_slot = slot;
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
        if (ImGui::Button("Save State")) TasSaveSelectedState();
        ImGui::SameLine(); if (ImGui::Button("Load")) TasLoadSelectedState();
        ImGui::SameLine(); if (ImGui::Button("Resume Rec")) { g_tas_read_only = false; TasLoadSelectedState(); }
        if (!compact_have_title) ImGui::EndDisabled();
        ImGui::SameLine(); if (ImGui::Button("Undo Load")) TasUndoStateLoad();
        ImGui::SameLine(); ImGui::TextDisabled("|"); ImGui::SameLine();
        const char *compact_ports[] = {"P1", "P2", "P3", "P4"};
        ImGui::SetNextItemWidth(54.0f); ImGui::Combo("##tas_port_c", &g_tas_port, compact_ports, 4);
        ImGui::SameLine(); if (ImGui::Button("Capture")) TasCaptureCurrentFrame();
        ImGui::SameLine(); if (ImGui::Button("Apply")) TasApplySelectedFrame();
        ImGui::SameLine(); if (ImGui::Button("Release")) xemu_tas_clear_xid_report((uint8_t)g_tas_port);
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
    if (ImGui::Checkbox("Deterministic (experimental)", &deterministic_mode)) {
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

    bool running = runstate_is_running();
    if (ImGui::Button(running ? "Pause" : "Resume")) ActionTogglePause();
    ImGui::SameLine();
    if (ImGui::Button("Frame +1")) TasAdvanceFrames(1);
    ImGui::SameLine();
    if (ImGui::Button("Frame +1 Skip Lag")) TasAdvanceFrames(1, true);
    ImGui::SameLine();
    if (ImGui::Button("Frame +10")) TasAdvanceFrames(10);
    ImGui::SameLine();
    ImGui::TextDisabled("Last frame: %s  streak: %llu",
                        xemu_tas_last_frame_lagged() ? "LAG" : "input",
                        (unsigned long long)xemu_tas_lag_streak());

    if (!xemu_tas_recording()) {
        if (ImGui::Button("Record (Power-On)")) TasStartRecording(true);
        ImGui::SameLine();
        if (ImGui::Button("Record From Current State")) TasStartRecording(false);
    } else {
        if (ImGui::Button("Stop Recording")) {
            xemu_tas_stop_recording();
            TasSyncRecordingFromCore();
            TasAutosaveRecovery(true);
        }
        ImGui::SameLine();
        ImGui::TextDisabled("RECORDING");
    }
    ImGui::SameLine();
    if (!g_tas_overdub_ui_active && !xemu_tas_overdub()) {
        if (ImGui::Button("Punch-In / Overdub")) TasStartOverdub();
    } else {
        if (ImGui::Button("Stop Overdub")) TasStopOverdub();
        ImGui::SameLine();
        ImGui::TextDisabled("OVERDUB @ %llu", (unsigned long long)g_tas_overdub_start_frame);
    }
    ImGui::SameLine();
    if (!xemu_tas_playback() || g_tas_overdub_ui_active) {
        if (!g_tas_overdub_ui_active && ImGui::Button("Play From Selected")) TasStartPlayback((uint64_t)g_tas_selected_frame);
    } else {
        if (ImGui::Button("Stop Playback")) xemu_tas_stop_playback();
        ImGui::SameLine();
        ImGui::TextDisabled("PLAY %llu/%llu",
                            (unsigned long long)xemu_tas_playback_frame(),
                            (unsigned long long)xemu_tas_playback_frame_count());
    }
    ImGui::SameLine();
    if (ImGui::Button("Save Movie")) TasSaveMovie();
    ImGui::SameLine();
    if (ImGui::Button("Movie Properties")) g_tas_properties_open = !g_tas_properties_open;
    ImGui::SameLine();
    if (ImGui::Button("Compatibility")) g_tas_compatibility_open = true;
    ImGui::SameLine();
    if (ImGui::Button("RAM Tools")) g_tas_ram_tools_open = true;
    ImGui::SameLine();
    ImGui::Checkbox("TAS HUD", &g_tas_hud_enabled);

    // Keep TAS state selection on its own row so it can never be pushed or
    // clipped off the right side by the playback/frame controls above.
    ImGui::TextUnformatted("State Slot");
    ImGui::SameLine();
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
            if (ImGui::Selectable(slot_label, selected)) {
                g_tas_state_slot = slot;
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Select TAS save-state slot 00-99");
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
        snprintf(tas_state_name, sizeof(tas_state_name), "--------_TAS_%02d",
                 g_tas_state_slot);
        ImGui::BeginDisabled();
    }
    if (ImGui::Button("Save State")) TasSaveSelectedState();
    ImGui::SameLine();
    if (ImGui::Button("Load State")) TasLoadSelectedState();
    ImGui::SameLine();
    if (ImGui::Button("Resume Recording From State")) {
        g_tas_read_only = false;
        TasLoadSelectedState();
    }
    if (!have_title_id) ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Undo State Load")) TasUndoStateLoad();
    ImGui::SameLine();
    ImGui::TextDisabled("%s%s", tas_state_name,
                        g_tas_state_meta[g_tas_state_slot].valid ? "  [movie-linked]" : "");

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
            TasPushUndo("Create branch");
            uint64_t f = (uint64_t)std::clamp(g_tas_selected_frame, 0, (int)g_tas_frames.size() - 1);
            TasArchiveCurrentBranch(f, "Manual branch snapshot");
            g_tas_frames.resize((size_t)f + 1);
            g_tas_lag_flags.resize((size_t)f + 1);
            ++g_tas_rerecord_count;
            TasMarkMovieEdited(f);
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
                    /* A branch becomes the live movie, so transfer its storage
                     * rather than cloning an entire potentially long TAS. */
                    TasBranch target = std::move(g_tas_branches[i]);
                    g_tas_branches.erase(g_tas_branches.begin() + i);
                    /* A branch switch does not need the prefix copy performed
                     * when forking for rerecording. Store the current branch by
                     * move and immediately make the target live. */
                    TasStoreCurrentBranch((uint64_t)g_tas_selected_frame,
                                          "Branch before switch");
                    g_tas_frames = std::move(target.frames);
                    g_tas_lag_flags = std::move(target.lag);
                    if (g_tas_frames.empty()) g_tas_frames.resize(1);
                    if (g_tas_lag_flags.size() < g_tas_frames.size()) {
                        g_tas_lag_flags.resize(g_tas_frames.size(), 0);
                    }
                    g_tas_current_branch = target.id;
                    g_tas_current_parent = target.parent;
                    g_tas_selected_frame = (int)std::min<uint64_t>(target.fork_frame, INT_MAX);
                    g_tas_follow_frame = false;
                    g_tas_core_pushed_revision = UINT64_MAX;
                    g_tas_visible_frame_cache_dirty = true;
                    TasAutosaveRecovery(true);
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
        ImGui::TextWrapped("Verifier checkpoints hash guest RAM plus the canonical XID movie input while paused at deterministic frame boundaries. This catches practical desyncs; device-specific hashes can be added later.");
        ImGui::SetNextItemWidth(120); ImGui::InputInt("Hash interval",&g_tas_verify_interval); g_tas_verify_interval=std::clamp(g_tas_verify_interval,1,36000);
        if(g_tas_verify_mode==TasVerifyMode::Idle){
            if(ImGui::Button("Capture Baseline From Current State")) TasStartVerifier(true);
            ImGui::SameLine(); bool can_verify=!g_tas_verify_baseline.empty(); if(!can_verify)ImGui::BeginDisabled(); if(ImGui::Button("Verify Against Baseline")) TasStartVerifier(false); if(!can_verify)ImGui::EndDisabled();
        } else { ImGui::Text("Verifier running: %s",g_tas_verify_status.c_str()); ImGui::SameLine(); if(ImGui::Button("Stop Verification")){g_tas_verify_mode=TasVerifyMode::Idle;xemu_tas_stop_playback();if(runstate_is_running())vm_stop(RUN_STATE_PAUSED);} }
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
    auto [sel_a, sel_b] = TasSelectionBounds();
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
        TasPushUndo("Append 60 blank frames");
        size_t old=g_tas_frames.size(); g_tas_frames.resize(old+60); g_tas_lag_flags.resize(g_tas_frames.size(),0); TasMarkMovieEdited((uint64_t)old);
    }
    ImGui::SameLine();
    if (ImGui::Button("Insert frame")) {
        int at=std::clamp(g_tas_selected_frame,0,(int)g_tas_frames.size()); TasPushUndo("Insert frame"); g_tas_frames.insert(g_tas_frames.begin()+at,TasFrame{});g_tas_lag_flags.insert(g_tas_lag_flags.begin()+at,0);TasSetSelection(at,false);TasMarkMovieEdited((uint64_t)at);
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
                auto bounds_now = TasSelectionBounds();
                bool selected_range = f >= bounds_now.first && f <= bounds_now.second;
                if (ImGui::Selectable(frame_text, selected_range,
                                      ImGuiSelectableFlags_None,
                                      ImVec2(0.0f, row_h - 2.0f))) {
                    TasSetSelection(f, ImGui::GetIO().KeyShift);
                }
                if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    TasSeekFrame((uint64_t)f);
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
        ImGui::TextDisabled("The roll is bounded by the current movie length. Follow TAS frame auto-scrolls downward as recording/playback appends frames; manual wheel scrolling disables Follow so frame 0 is always reachable. Shift-click frames selects a range; double-click a frame seeks through the greenzone.");
    } else {
        ImGui::TextDisabled("Shift-click = range | double-click = seek | wheel = manual timeline scroll");
    }
    ImGui::End();
}

static void DrawTasCompatibilityWindow()
{
    if (!g_tas_compatibility_open) return;
    ImGui::SetNextWindowSize(ImVec2(640, 480), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("TAS Movie Compatibility", &g_tas_compatibility_open)) { ImGui::End(); return; }
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
    if(!g_tas_history_open)return;
    if(!ImGui::Begin("TAS Movie Backup History",&g_tas_history_open)){ImGui::End();return;}
    const auto &files = TasMovieHistoryFiles();
    if(files.empty())ImGui::TextDisabled("No backup history exists for the current movie yet.");
    for(size_t i=0;i<files.size();++i){ImGui::PushID((int)i);ImGui::TextUnformatted(files[i].filename().string().c_str());ImGui::SameLine();if(ImGui::Button("Load"))TasLoadMovieFromPath(files[i].string().c_str());ImGui::SameLine();if(ImGui::Button("Restore Over Current")&&!g_tas_movie_path.empty()){std::error_code ec;std::filesystem::copy_file(files[i],g_tas_movie_path,std::filesystem::copy_options::overwrite_existing,ec);if(!ec)TasLoadMovieFromPath(g_tas_movie_path.c_str());}ImGui::PopID();}
    ImGui::End();
}

static void DrawTasRamToolsWindow()
{
    if(!g_tas_ram_tools_open)return;
    ImGui::SetNextWindowSize(ImVec2(860,620),ImGuiCond_FirstUseEver);
    if(!ImGui::Begin("TAS RAM Watch / Search / RNG",&g_tas_ram_tools_open)){ImGui::End();return;}
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
                if (ImGui::MenuItem("Start Recording (Power-On)")) TasStartRecording(true);
                if (ImGui::MenuItem("Start Recording (Current State)")) TasStartRecording(false);
            } else if (ImGui::MenuItem("Stop Recording")) {
                xemu_tas_stop_recording();
                TasSyncRecordingFromCore();
                TasAutosaveRecovery(true);
            }
            if (!xemu_tas_playback()) {
                if (ImGui::MenuItem("Play From Selected Frame")) TasStartPlayback((uint64_t)g_tas_selected_frame);
            } else if (ImGui::MenuItem("Stop Playback")) {
                xemu_tas_stop_playback();
            }
            ImGui::Separator();
            ImGui::Text("Frame: %llu   Lag: %llu", (unsigned long long)xemu_tas_frame(),
                        (unsigned long long)xemu_tas_lag_count());
            if (ImGui::MenuItem("Frame Advance")) TasAdvanceFrames(1);
            if (ImGui::MenuItem("Frame Advance - Skip Lag")) TasAdvanceFrames(1, true);
            if (ImGui::MenuItem("Advance 10 Frames")) TasAdvanceFrames(10);
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
            if (ImGui::MenuItem("Rewind 60 Frames")) TasRewindFrames(60);
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
                ? "Deterministic boundary: Xbox guest VBLANK (experimental)."
                : "Deterministic TAS Mode is available above.");
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
        !g_tas_snapshot_delete_queue.empty() ||
        g_tas_verify_mode != TasVerifyMode::Idle || g_tas_rng_watch >= 0;
    if (!runtime_active && !ui_active && !maintenance_active) {
        return;
    }

    /* Recording/playback bookkeeping must continue even with TAS Studio closed. */
    TasSyncRecordingFromCore();
    TasUpdateRewindCache();
    TasServiceDeferredSnapshotDeletes();
    TasVerifierTick();
    TasUpdateRamWatches();
    TasCheckRecoveryNotice();
    DrawTasStudio();
    DrawTasInputDisplay();
    DrawTasGoToFrameDialog();
    DrawTasCompatibilityWindow();
    DrawTasHistoryWindow();
    DrawTasRamToolsWindow();
    DrawTasHud();
}

void FeatureTasDrawGeneralSettings()
{
    SectionTitle("TAS Tools (Experimental)");

    bool tas_enabled = xemu_tas_enabled();
    if (Toggle("Enable TAS mode", &tas_enabled,
               "Enable the additive TAS core. Normal Xemu features, including "
               "Fast Forward, remain available. Host controller input is only "
               "bypassed on a port when TAS playback explicitly injects an "
               "exact Xbox XID report.")) {
        xemu_tas_set_enabled(tas_enabled);
    }

    ImGui::Text("TAS frame: %llu",
                (unsigned long long)xemu_tas_frame());

    uint8_t xid[20];
    if (xemu_tas_get_last_xid_report(0, xid, sizeof(xid))) {
        uint16_t buttons;
        int16_t lx, ly, rx, ry;
        memcpy(&buttons, &xid[2], sizeof(buttons));
        memcpy(&lx, &xid[12], sizeof(lx));
        memcpy(&ly, &xid[14], sizeof(ly));
        memcpy(&rx, &xid[16], sizeof(rx));
        memcpy(&ry, &xid[18], sizeof(ry));
        ImGui::Text("Port 1 XID: buttons=%04X  LT=%u RT=%u",
                    buttons, xid[10], xid[11]);
        ImGui::Text("A=%u B=%u X=%u Y=%u Black=%u White=%u",
                    xid[4], xid[5], xid[6], xid[7], xid[8], xid[9]);
        ImGui::Text("LX=%d LY=%d  RX=%d RY=%d", lx, ly, rx, ry);
    } else {
        ImGui::TextDisabled("Port 1 XID: waiting for first guest poll");
    }

    ImGui::TextWrapped(
        "Milestone 1 uses guest Xbox VBLANK as the canonical frame boundary "
        "and provides exact 20-byte XID report injection/capture. Deterministic "
        "frame advance, movie recording/playback, state hashing, and TAS Studio "
        "build on this isolated core.");
}
