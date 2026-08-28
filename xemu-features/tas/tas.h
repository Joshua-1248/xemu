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
uint32_t xemu_tas_frame_advance_remaining(void);
bool xemu_tas_consume_pause_request(void);
bool xemu_tas_last_frame_lagged(void);
uint64_t xemu_tas_lag_count(void);
uint64_t xemu_tas_lag_streak(void);
void xemu_tas_reset_lag_counters(void);
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
bool xemu_tas_set_playback_movie(const void *reports, const uint8_t *lag_flags,
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
static inline uint32_t xemu_tas_frame_advance_remaining(void) { return 0; }
static inline bool xemu_tas_consume_pause_request(void) { return false; }
static inline bool xemu_tas_last_frame_lagged(void) { return false; }
static inline uint64_t xemu_tas_lag_count(void) { return 0; }
static inline uint64_t xemu_tas_lag_streak(void) { return 0; }
static inline void xemu_tas_reset_lag_counters(void) {}
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
static inline bool xemu_tas_set_playback_movie(const void *reports, const uint8_t *lag_flags,
                                                uint64_t frame_count)
{ (void)reports; (void)lag_flags; (void)frame_count; return false; }
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
