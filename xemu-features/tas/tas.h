/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef XEMU_FEATURES_TAS_TAS_H
#define XEMU_FEATURES_TAS_TAS_H

#include "config-host.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define XEMU_TAS_MAX_PORTS 4
#define XEMU_TAS_XID_REPORT_SIZE 20
#define XEMU_TAS_FRAME_REPORT_BYTES (XEMU_TAS_MAX_PORTS * XEMU_TAS_XID_REPORT_SIZE)

typedef struct XemuTasTransactionId {
    uint64_t hi;
    uint64_t lo;
} XemuTasTransactionId;

typedef struct XemuTasDesyncInfo {
    bool valid;
    uint64_t frame;
    uint8_t port;
    uint32_t expected_polls;
    uint32_t actual_polls;
} XemuTasDesyncInfo;

#ifdef CONFIG_XEMU_FEATURE_TAS
bool xemu_tas_enabled(void);
void xemu_tas_set_enabled(bool enabled);
uint64_t xemu_tas_frame(void);
void xemu_tas_set_frame(uint64_t frame);
void xemu_tas_on_vblank(void);
bool xemu_tas_deterministic_mode(void);
void xemu_tas_set_deterministic_mode(bool enabled);
void xemu_tas_request_frame_advance(uint32_t frames);
void xemu_tas_request_frame_advance_ex(uint32_t frames, bool skip_lag_frames);
/* Cancel a pending frame-advance/pause request. TAS Studio uses this when a
 * strict checkpoint operation is superseded by another transport action. */
void xemu_tas_cancel_frame_advance(void);
uint32_t xemu_tas_frame_advance_remaining(void);
bool xemu_tas_consume_pause_request(void);
bool xemu_tas_seek_catchup(void);
void xemu_tas_set_seek_catchup(bool active);
bool xemu_tas_last_frame_lagged(void);
uint64_t xemu_tas_lag_count(void);
uint64_t xemu_tas_lag_streak(void);
void xemu_tas_reset_lag_counters(void);
/* Restore feature-owned lag bookkeeping after loading a TAS-owned native
 * snapshot. These counters participate in Strict Sync fingerprints but are
 * intentionally not part of ordinary Xemu/QEMU VMState. */
void xemu_tas_set_lag_state(bool last_frame_lagged, uint64_t lag_count,
                            uint64_t lag_streak);
/* TAS-owned snapshots mint a feature-owned transaction ID immediately before
 * saving. The ID is part of QEMU VMState, so loading the native snapshot
 * restores the same value and lets TAStudio reject a mismatched movie bundle.
 * This bookkeeping ID is intentionally excluded from deterministic hashes. */
void xemu_tas_transaction_mint(XemuTasTransactionId *out);
void xemu_tas_transaction_get(XemuTasTransactionId *out);
bool xemu_tas_transaction_matches(const XemuTasTransactionId *id);
/* Compatibility no-op retained for Studio call sites. The passive TAS VMState
 * loader is registered at module init but is omitted from ordinary save files. */
void xemu_tas_prepare_runtime(void);
/* Scope xemu/tas-transaction serialization around TAS-owned native snapshot
 * saves only. Ordinary Xemu Save State files never get TAS bookkeeping. */
void xemu_tas_transaction_snapshot_begin(void);
void xemu_tas_transaction_snapshot_end(void);
/* Hash QEMU's canonical serialized non-RAM VM/device state at a paused
 * boundary. The caller supplies a rolling seed so RAM/movie/device layers can
 * be composed without copying large buffers. Failure is reported explicitly;
 * determinism verification must fail closed rather than silently fall back to
 * a weaker hash. */
bool xemu_tas_machine_state_hash(uint64_t seed, uint64_t *out_hash);
/* Arm one TAS-owned system reset. The feature reset observer requests a VM
 * stop as part of the real QEMU reset transaction and advances a sequence only
 * when that reset callback actually runs. This lets TAStudio establish frame 0
 * after reset completion instead of racing qemu_system_reset_request(). */
uint64_t xemu_tas_arm_post_reset_pause(void);
void xemu_tas_cancel_post_reset_pause(void);
uint64_t xemu_tas_post_reset_sequence(void);
void xemu_tas_start_recording(bool clear_existing);
void xemu_tas_stop_recording(void);
bool xemu_tas_recording(void);
void xemu_tas_clear_recording(void);
uint64_t xemu_tas_recorded_frame_count(void);
bool xemu_tas_get_recorded_frame(uint64_t index, void *reports, size_t reports_size,
                                 bool *lagged);
uint64_t xemu_tas_copy_recorded_frames(uint64_t start, uint64_t max_frames,
                                       void *reports, size_t reports_size,
                                       uint8_t *lag_flags, size_t lag_flags_size);
uint64_t xemu_tas_copy_recorded_frames_ex(uint64_t start, uint64_t max_frames,
                                          void *reports, size_t reports_size,
                                          uint8_t *lag_flags, size_t lag_flags_size,
                                          uint32_t *poll_counts,
                                          size_t poll_counts_count);
uint64_t xemu_tas_copy_playback_poll_trace(uint64_t start, uint64_t max_frames,
                                           uint32_t *poll_counts,
                                           size_t poll_counts_count);
bool xemu_tas_set_playback_movie(const void *reports, const uint8_t *lag_flags,
                                 uint64_t frame_count);
bool xemu_tas_set_playback_movie_ex(const void *reports, const uint8_t *lag_flags,
                                    const uint32_t *poll_counts,
                                    size_t poll_counts_count,
                                    uint64_t frame_count);
/* Replace the immutable movie copy while preserving an active playback cursor.
 * Used when TAStudio edits only future input during playback. */
bool xemu_tas_update_playback_movie(const void *reports, const uint8_t *lag_flags,
                                    uint64_t frame_count);
bool xemu_tas_update_playback_movie_ex(const void *reports,
                                       const uint8_t *lag_flags,
                                       const uint32_t *poll_counts,
                                       size_t poll_counts_count,
                                       uint64_t frame_count);
bool xemu_tas_start_playback(uint64_t start_frame);
void xemu_tas_stop_playback(void);
bool xemu_tas_playback(void);
uint64_t xemu_tas_playback_frame(void);
uint64_t xemu_tas_playback_frame_count(void);
bool xemu_tas_start_overdub(uint64_t start_frame, uint8_t port,
                            uint16_t digital_mask, uint8_t analog_mask,
                            uint8_t stick_mask);
void xemu_tas_stop_overdub(void);
bool xemu_tas_overdub(void);
bool xemu_tas_take_desync(XemuTasDesyncInfo *out);
void xemu_tas_process_xid_report(uint8_t port, void *report, size_t size);
bool xemu_tas_override_xid_report(uint8_t port, void *report, size_t size);
void xemu_tas_observe_xid_report(uint8_t port, const void *report, size_t size);
bool xemu_tas_get_last_xid_report(uint8_t port, void *report, size_t size);
void xemu_tas_set_xid_report(uint8_t port, const void *report, size_t size);
void xemu_tas_clear_xid_report(uint8_t port);
void xemu_tas_clear_all_xid_reports(void);
void xemu_tas_set_auto_hold(uint8_t port, uint8_t control, bool enabled, uint8_t value);
void xemu_tas_set_autofire(uint8_t port, uint8_t control, bool enabled, uint8_t value,
                           uint32_t period, uint32_t phase);
void xemu_tas_clear_automation(uint8_t port);
void xemu_tas_apply_xid_automation(uint8_t port, void *report, size_t size);
#else
static inline bool xemu_tas_enabled(void) { return false; }
static inline void xemu_tas_set_enabled(bool enabled) { (void)enabled; }
static inline uint64_t xemu_tas_frame(void) { return 0; }
static inline void xemu_tas_set_frame(uint64_t frame) { (void)frame; }
static inline void xemu_tas_on_vblank(void) {}
static inline bool xemu_tas_deterministic_mode(void) { return false; }
static inline void xemu_tas_set_deterministic_mode(bool enabled) { (void)enabled; }
static inline void xemu_tas_request_frame_advance(uint32_t frames) { (void)frames; }
static inline void xemu_tas_request_frame_advance_ex(uint32_t frames, bool skip_lag_frames)
{ (void)frames; (void)skip_lag_frames; }
static inline void xemu_tas_cancel_frame_advance(void) {}
static inline uint32_t xemu_tas_frame_advance_remaining(void) { return 0; }
static inline bool xemu_tas_consume_pause_request(void) { return false; }
static inline bool xemu_tas_seek_catchup(void) { return false; }
static inline void xemu_tas_set_seek_catchup(bool active) { (void)active; }
static inline bool xemu_tas_last_frame_lagged(void) { return false; }
static inline uint64_t xemu_tas_lag_count(void) { return 0; }
static inline uint64_t xemu_tas_lag_streak(void) { return 0; }
static inline void xemu_tas_reset_lag_counters(void) {}
static inline void xemu_tas_set_lag_state(bool last_frame_lagged,
                                           uint64_t lag_count, uint64_t lag_streak)
{ (void)last_frame_lagged; (void)lag_count; (void)lag_streak; }
static inline void xemu_tas_transaction_mint(XemuTasTransactionId *out)
{ if (out) { out->hi = 0; out->lo = 0; } }
static inline void xemu_tas_transaction_get(XemuTasTransactionId *out)
{ if (out) { out->hi = 0; out->lo = 0; } }
static inline bool xemu_tas_transaction_matches(const XemuTasTransactionId *id)
{ return id && id->hi == 0 && id->lo == 0; }
static inline void xemu_tas_prepare_runtime(void) {}
static inline void xemu_tas_transaction_snapshot_begin(void) {}
static inline void xemu_tas_transaction_snapshot_end(void) {}
static inline bool xemu_tas_machine_state_hash(uint64_t seed, uint64_t *out_hash)
{ if (out_hash) *out_hash = seed; return false; }
static inline uint64_t xemu_tas_arm_post_reset_pause(void) { return 0; }
static inline void xemu_tas_cancel_post_reset_pause(void) {}
static inline uint64_t xemu_tas_post_reset_sequence(void) { return 0; }
static inline void xemu_tas_start_recording(bool clear_existing) { (void)clear_existing; }
static inline void xemu_tas_stop_recording(void) {}
static inline bool xemu_tas_recording(void) { return false; }
static inline void xemu_tas_clear_recording(void) {}
static inline uint64_t xemu_tas_recorded_frame_count(void) { return 0; }
static inline bool xemu_tas_get_recorded_frame(uint64_t index, void *reports,
                                               size_t reports_size, bool *lagged)
{ (void)index; (void)reports; (void)reports_size; if (lagged) *lagged = false; return false; }
static inline uint64_t xemu_tas_copy_recorded_frames(uint64_t start, uint64_t max_frames,
                                                      void *reports, size_t reports_size,
                                                      uint8_t *lag_flags, size_t lag_flags_size)
{ (void)start; (void)max_frames; (void)reports; (void)reports_size; (void)lag_flags; (void)lag_flags_size; return 0; }
static inline uint64_t xemu_tas_copy_recorded_frames_ex(uint64_t start,
                                                         uint64_t max_frames,
                                                         void *reports,
                                                         size_t reports_size,
                                                         uint8_t *lag_flags,
                                                         size_t lag_flags_size,
                                                         uint32_t *poll_counts,
                                                         size_t poll_counts_count)
{ (void)start; (void)max_frames; (void)reports; (void)reports_size; (void)lag_flags; (void)lag_flags_size; (void)poll_counts; (void)poll_counts_count; return 0; }
static inline uint64_t xemu_tas_copy_playback_poll_trace(uint64_t start,
                                                          uint64_t max_frames,
                                                          uint32_t *poll_counts,
                                                          size_t poll_counts_count)
{ (void)start; (void)max_frames; (void)poll_counts; (void)poll_counts_count; return 0; }
static inline bool xemu_tas_set_playback_movie(const void *reports, const uint8_t *lag_flags,
                                                uint64_t frame_count)
{ (void)reports; (void)lag_flags; (void)frame_count; return false; }
static inline bool xemu_tas_set_playback_movie_ex(const void *reports,
                                                   const uint8_t *lag_flags,
                                                   const uint32_t *poll_counts,
                                                   size_t poll_counts_count,
                                                   uint64_t frame_count)
{ (void)reports; (void)lag_flags; (void)poll_counts; (void)poll_counts_count; (void)frame_count; return false; }
static inline bool xemu_tas_update_playback_movie(const void *reports, const uint8_t *lag_flags,
                                                   uint64_t frame_count)
{ (void)reports; (void)lag_flags; (void)frame_count; return false; }
static inline bool xemu_tas_update_playback_movie_ex(const void *reports,
                                                      const uint8_t *lag_flags,
                                                      const uint32_t *poll_counts,
                                                      size_t poll_counts_count,
                                                      uint64_t frame_count)
{ (void)reports; (void)lag_flags; (void)poll_counts; (void)poll_counts_count; (void)frame_count; return false; }
static inline bool xemu_tas_start_playback(uint64_t start_frame) { (void)start_frame; return false; }
static inline void xemu_tas_stop_playback(void) {}
static inline bool xemu_tas_playback(void) { return false; }
static inline uint64_t xemu_tas_playback_frame(void) { return 0; }
static inline uint64_t xemu_tas_playback_frame_count(void) { return 0; }
static inline bool xemu_tas_start_overdub(uint64_t start_frame, uint8_t port,
                                           uint16_t digital_mask, uint8_t analog_mask,
                                           uint8_t stick_mask)
{ (void)start_frame; (void)port; (void)digital_mask; (void)analog_mask; (void)stick_mask; return false; }
static inline void xemu_tas_stop_overdub(void) {}
static inline bool xemu_tas_overdub(void) { return false; }
static inline bool xemu_tas_take_desync(XemuTasDesyncInfo *out)
{ if (out) { out->valid = false; out->frame = 0; out->port = 0; out->expected_polls = 0; out->actual_polls = 0; } return false; }
static inline void xemu_tas_process_xid_report(uint8_t port, void *report, size_t size)
{ (void)port; (void)report; (void)size; }
static inline bool xemu_tas_override_xid_report(uint8_t port, void *report, size_t size)
{ (void)port; (void)report; (void)size; return false; }
static inline void xemu_tas_observe_xid_report(uint8_t port, const void *report, size_t size)
{ (void)port; (void)report; (void)size; }
static inline bool xemu_tas_get_last_xid_report(uint8_t port, void *report, size_t size)
{ (void)port; (void)report; (void)size; return false; }
static inline void xemu_tas_set_xid_report(uint8_t port, const void *report, size_t size)
{ (void)port; (void)report; (void)size; }
static inline void xemu_tas_clear_xid_report(uint8_t port) { (void)port; }
static inline void xemu_tas_clear_all_xid_reports(void) {}
static inline void xemu_tas_set_auto_hold(uint8_t port, uint8_t control, bool enabled, uint8_t value)
{ (void)port; (void)control; (void)enabled; (void)value; }
static inline void xemu_tas_set_autofire(uint8_t port, uint8_t control, bool enabled,
                                          uint8_t value, uint32_t period, uint32_t phase)
{ (void)port; (void)control; (void)enabled; (void)value; (void)period; (void)phase; }
static inline void xemu_tas_clear_automation(uint8_t port) { (void)port; }
static inline void xemu_tas_apply_xid_automation(uint8_t port, void *report, size_t size)
{ (void)port; (void)report; (void)size; }
#endif

#ifdef __cplusplus
}
#endif
#endif
