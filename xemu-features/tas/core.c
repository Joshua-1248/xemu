/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * xemu deterministic TAS core
 *
 * Additive by design: normal host input/emulation are untouched unless TAS
 * mode or a TAS input automation feature is enabled. Movie data is not added
 * to the ordinary XID VMState; TAS movie/rerecord metadata lives in .xmt and
 * the TAS state layer.
 */
#include "qemu/osdep.h"
#include "qemu/atomic.h"
#include "system/runstate.h"
#include "xemu-features/tas/tas.h"

static gint tas_enabled;
static gint tas_deterministic_mode;
static uint64_t tas_frame_counter;
static uint32_t tas_frame_advance_remaining;
static gint tas_frame_advance_skip_lag;
static gint tas_pause_request;

static gint tas_polled_mask;
static gint tas_last_frame_lag;
static uint64_t tas_lag_count_value;
static uint64_t tas_lag_streak_value;

/* Hot-path summary masks. These let XID polling take one cheap branch when
 * a feature is inactive instead of walking per-control/per-port state. */
static gint tas_report_valid_mask;
static gint tas_capture_request_mask;
static gint tas_automation_active_mask;
static gint tas_automation_control_mask[XEMU_TAS_MAX_PORTS];

/* Double-buffered injected reports. */
static uint8_t tas_reports[XEMU_TAS_MAX_PORTS][2][XEMU_TAS_XID_REPORT_SIZE];
static gint tas_report_index[XEMU_TAS_MAX_PORTS];

/* Double-buffered observed guest-visible reports. */
static uint8_t tas_last_reports[XEMU_TAS_MAX_PORTS][2][XEMU_TAS_XID_REPORT_SIZE];
static gint tas_last_report_index[XEMU_TAS_MAX_PORTS];
static gint tas_last_report_valid[XEMU_TAS_MAX_PORTS];
static uint64_t tas_last_observed_frame[XEMU_TAS_MAX_PORTS];

/* Recording buffer. */
typedef struct XemuTasRecordedFrame {
    uint8_t reports[XEMU_TAS_FRAME_REPORT_BYTES];
    uint8_t lagged;
} XemuTasRecordedFrame;

static GMutex tas_recording_lock;
static GArray *tas_recording_frames;
static gint tas_recording_active;
static uint64_t tas_recording_count;

/* Playback buffer. */
static GMutex tas_playback_lock;
static uint8_t *tas_playback_reports;
static size_t tas_playback_capacity;
static uint64_t tas_playback_count;
static uint64_t tas_playback_cursor;
static gint tas_playback_active;

/* Punch-in/overdub state. The playback report remains the base input; selected
 * fields from the live host report replace it before the guest sees it.
 * Configuration fields are atomic because the XID path consumes them without
 * taking the playback/UI mutex. */
static gint tas_overdub_active;
static gint tas_overdub_port;
static gint tas_overdub_digital_mask;
static gint tas_overdub_aux_masks; /* bits 0..7 analog, 8..11 sticks */

/* Per-input automation. Each control is packed into one atomic 64-bit word,
 * so active auto-hold/autofire never takes a mutex in the XID polling path.
 * Period/phase are intentionally capped at 16 bits (the UI currently exposes
 * 1..60), leaving ample range while keeping every update/read coherent. */
#define TAS_AUTO_HOLD_ENABLED_BIT   0
#define TAS_AUTO_FIRE_ENABLED_BIT   1
#define TAS_AUTO_HOLD_VALUE_SHIFT   2
#define TAS_AUTO_FIRE_VALUE_SHIFT   10
#define TAS_AUTO_PERIOD_SHIFT       18
#define TAS_AUTO_PHASE_SHIFT        34
#define TAS_AUTO_BYTE_MASK          UINT64_C(0xff)
#define TAS_AUTO_U16_MASK           UINT64_C(0xffff)

static uint64_t tas_automation_packed[XEMU_TAS_MAX_PORTS][16];

static bool tas_automation_word_active(uint64_t word)
{
    return (word & ((UINT64_C(1) << TAS_AUTO_HOLD_ENABLED_BIT) |
                    (UINT64_C(1) << TAS_AUTO_FIRE_ENABLED_BIT))) != 0;
}

static void tas_refresh_automation_mask(uint8_t port)
{
    unsigned control_mask = 0;
    for (unsigned i = 0; i < 16; ++i) {
        if (tas_automation_word_active(
                qatomic_read(&tas_automation_packed[port][i]))) {
            control_mask |= 1u << i;
        }
    }
    g_atomic_int_set(&tas_automation_control_mask[port], (gint)control_mask);

    const gint bit = 1 << port;
    if (control_mask) {
        g_atomic_int_or(&tas_automation_active_mask, bit);
    } else {
        g_atomic_int_and(&tas_automation_active_mask, ~bit);
    }
}

static void tas_install_playback_frame_locked(uint64_t frame)
{
    if (!tas_playback_reports || frame >= qatomic_read(&tas_playback_count)) {
        return;
    }
    const uint8_t *base = tas_playback_reports + frame * XEMU_TAS_FRAME_REPORT_BYTES;
    for (uint8_t port = 0; port < XEMU_TAS_MAX_PORTS; ++port) {
        xemu_tas_set_xid_report(port,
                                base + port * XEMU_TAS_XID_REPORT_SIZE,
                                XEMU_TAS_XID_REPORT_SIZE);
    }
}

bool xemu_tas_enabled(void)
{
    return g_atomic_int_get(&tas_enabled) != 0;
}

void xemu_tas_set_enabled(bool enabled)
{
    bool was_enabled = xemu_tas_enabled();
    g_atomic_int_set(&tas_enabled, enabled ? 1 : 0);

    if (enabled && !was_enabled) {
        qatomic_set(&tas_frame_counter, 0);
        g_atomic_int_set(&tas_polled_mask, 0);
        xemu_tas_reset_lag_counters();
        for (unsigned port = 0; port < XEMU_TAS_MAX_PORTS; ++port) {
            qatomic_set(&tas_last_observed_frame[port], UINT64_MAX);
        }
    }

    if (!enabled) {
        qatomic_set(&tas_frame_advance_remaining, 0);
        g_atomic_int_set(&tas_frame_advance_skip_lag, 0);
        g_atomic_int_set(&tas_pause_request, 0);
        xemu_tas_stop_recording();
        xemu_tas_stop_playback();
        xemu_tas_clear_all_xid_reports();
        g_atomic_int_set(&tas_capture_request_mask, 0);
    }
}

uint64_t xemu_tas_frame(void)
{
    return qatomic_read(&tas_frame_counter);
}

void xemu_tas_set_frame(uint64_t frame)
{
    qatomic_set(&tas_frame_counter, frame);
    g_atomic_int_set(&tas_polled_mask, 0);
    for (unsigned port = 0; port < XEMU_TAS_MAX_PORTS; ++port) {
        qatomic_set(&tas_last_observed_frame[port], UINT64_MAX);
    }
}

bool xemu_tas_deterministic_mode(void)
{
    return g_atomic_int_get(&tas_deterministic_mode) != 0;
}

void xemu_tas_set_deterministic_mode(bool enabled)
{
    g_atomic_int_set(&tas_deterministic_mode, enabled ? 1 : 0);
    if (enabled && !xemu_tas_enabled()) {
        xemu_tas_set_enabled(true);
    }
}

void xemu_tas_request_frame_advance(uint32_t frames)
{
    xemu_tas_request_frame_advance_ex(frames, false);
}

void xemu_tas_request_frame_advance_ex(uint32_t frames, bool skip_lag_frames)
{
    if (!frames) {
        return;
    }
    if (!xemu_tas_enabled()) {
        xemu_tas_set_enabled(true);
    }
    g_atomic_int_set(&tas_pause_request, 0);
    g_atomic_int_set(&tas_frame_advance_skip_lag, skip_lag_frames ? 1 : 0);
    qatomic_set(&tas_frame_advance_remaining, frames);
}

uint32_t xemu_tas_frame_advance_remaining(void)
{
    return qatomic_read(&tas_frame_advance_remaining);
}

bool xemu_tas_consume_pause_request(void)
{
    return qatomic_xchg(&tas_pause_request, 0) != 0;
}

bool xemu_tas_last_frame_lagged(void)
{
    return g_atomic_int_get(&tas_last_frame_lag) != 0;
}

uint64_t xemu_tas_lag_count(void)
{
    return qatomic_read(&tas_lag_count_value);
}

uint64_t xemu_tas_lag_streak(void)
{
    return qatomic_read(&tas_lag_streak_value);
}

void xemu_tas_reset_lag_counters(void)
{
    g_atomic_int_set(&tas_last_frame_lag, 0);
    qatomic_set(&tas_lag_count_value, 0);
    qatomic_set(&tas_lag_streak_value, 0);
}

static void tas_append_recorded_frame(bool lagged)
{
    if (!g_atomic_int_get(&tas_recording_active)) {
        return;
    }

    XemuTasRecordedFrame frame = { 0 };
    frame.lagged = lagged ? 1 : 0;
    for (uint8_t port = 0; port < XEMU_TAS_MAX_PORTS; ++port) {
        int index = g_atomic_int_get(&tas_last_report_index[port]) & 1;
        if (g_atomic_int_get(&tas_last_report_valid[port])) {
            memcpy(frame.reports + port * XEMU_TAS_XID_REPORT_SIZE,
                   tas_last_reports[port][index], XEMU_TAS_XID_REPORT_SIZE);
        }
    }

    g_mutex_lock(&tas_recording_lock);
    if (!tas_recording_frames) {
        /* Avoid the early realloc churn common at the start of recordings. */
        tas_recording_frames = g_array_sized_new(FALSE, FALSE,
                                                  sizeof(XemuTasRecordedFrame),
                                                  4096);
    }
    g_array_append_val(tas_recording_frames, frame);
    qatomic_set(&tas_recording_count, tas_recording_frames->len);
    g_mutex_unlock(&tas_recording_lock);
}

void xemu_tas_on_vblank(void)
{
    /* UI/presentation VBLANKs can occur while paused. TAS frames cannot. */
    if (!xemu_tas_enabled() || !runstate_is_running()) {
        return;
    }

    const bool lagged = qatomic_xchg(&tas_polled_mask, 0) == 0;
    g_atomic_int_set(&tas_last_frame_lag, lagged ? 1 : 0);
    if (lagged) {
        qatomic_inc(&tas_lag_count_value);
        qatomic_inc(&tas_lag_streak_value);
    } else {
        qatomic_set(&tas_lag_streak_value, 0);
    }

    tas_append_recorded_frame(lagged);

    qatomic_inc(&tas_frame_counter);

    if (g_atomic_int_get(&tas_playback_active)) {
        g_mutex_lock(&tas_playback_lock);
        if (g_atomic_int_get(&tas_playback_active)) {
            uint64_t cursor = qatomic_read(&tas_playback_cursor) + 1;
            qatomic_set(&tas_playback_cursor, cursor);
            if (cursor >= qatomic_read(&tas_playback_count)) {
                g_atomic_int_set(&tas_playback_active, 0);
                if (g_atomic_int_get(&tas_overdub_active)) {
                    g_atomic_int_set(&tas_overdub_active, 0);
                    g_atomic_int_set(&tas_recording_active, 0);
                }
                xemu_tas_clear_all_xid_reports();
            } else {
                tas_install_playback_frame_locked(cursor);
            }
        }
        g_mutex_unlock(&tas_playback_lock);
    }

    uint32_t remaining = qatomic_read(&tas_frame_advance_remaining);
    if (remaining) {
        bool consume = !g_atomic_int_get(&tas_frame_advance_skip_lag) || !lagged;
        if (consume) {
            --remaining;
            qatomic_set(&tas_frame_advance_remaining, remaining);
            if (!remaining) {
                g_atomic_int_set(&tas_frame_advance_skip_lag, 0);
                g_atomic_int_set(&tas_pause_request, 1);
            }
        }
    }
}

void xemu_tas_start_recording(bool clear_existing)
{
    g_atomic_int_set(&tas_overdub_active, 0);
    if (!xemu_tas_enabled()) {
        xemu_tas_set_enabled(true);
    }
    xemu_tas_stop_playback();
    if (clear_existing) {
        xemu_tas_clear_recording();
    }
    g_atomic_int_set(&tas_recording_active, 1);
}

void xemu_tas_stop_recording(void)
{
    g_atomic_int_set(&tas_recording_active, 0);
}

bool xemu_tas_recording(void)
{
    return g_atomic_int_get(&tas_recording_active) != 0;
}

void xemu_tas_clear_recording(void)
{
    g_mutex_lock(&tas_recording_lock);
    if (tas_recording_frames) {
        g_array_set_size(tas_recording_frames, 0);
    }
    qatomic_set(&tas_recording_count, 0);
    g_mutex_unlock(&tas_recording_lock);
}

uint64_t xemu_tas_recorded_frame_count(void)
{
    /* Read-mostly UI query: no mutex needed. The writer publishes this only
     * after the corresponding GArray append is complete. */
    return qatomic_read(&tas_recording_count);
}

bool xemu_tas_get_recorded_frame(uint64_t index, void *reports, size_t reports_size,
                                 bool *lagged)
{
    if (!reports || reports_size != XEMU_TAS_FRAME_REPORT_BYTES) {
        return false;
    }
    bool ok = false;
    g_mutex_lock(&tas_recording_lock);
    if (tas_recording_frames && index < tas_recording_frames->len) {
        const XemuTasRecordedFrame *f = &g_array_index(tas_recording_frames,
                                                       XemuTasRecordedFrame,
                                                       (guint)index);
        memcpy(reports, f->reports, XEMU_TAS_FRAME_REPORT_BYTES);
        if (lagged) {
            *lagged = f->lagged != 0;
        }
        ok = true;
    }
    g_mutex_unlock(&tas_recording_lock);
    return ok;
}

uint64_t xemu_tas_copy_recorded_frames(uint64_t start, uint64_t max_frames,
                                       void *reports, size_t reports_size,
                                       uint8_t *lag_flags, size_t lag_flags_size)
{
    if (!reports || !max_frames ||
        max_frames > SIZE_MAX / XEMU_TAS_FRAME_REPORT_BYTES) {
        return 0;
    }

    const size_t needed_reports = (size_t)max_frames * XEMU_TAS_FRAME_REPORT_BYTES;
    if (reports_size < needed_reports ||
        (lag_flags && lag_flags_size < (size_t)max_frames)) {
        return 0;
    }

    uint64_t copied = 0;
    g_mutex_lock(&tas_recording_lock);
    if (tas_recording_frames && start < tas_recording_frames->len) {
        uint64_t available = (uint64_t)tas_recording_frames->len - start;
        copied = MIN(max_frames, available);
        uint8_t *dst = reports;
        for (uint64_t i = 0; i < copied; ++i) {
            const XemuTasRecordedFrame *f = &g_array_index(
                tas_recording_frames, XemuTasRecordedFrame, (guint)(start + i));
            memcpy(dst + i * XEMU_TAS_FRAME_REPORT_BYTES, f->reports,
                   XEMU_TAS_FRAME_REPORT_BYTES);
            if (lag_flags) {
                lag_flags[i] = f->lagged;
            }
        }
    }
    g_mutex_unlock(&tas_recording_lock);
    return copied;
}

bool xemu_tas_set_playback_movie(const void *reports, const uint8_t *lag_flags,
                                 uint64_t frame_count)
{
    if ((!reports && frame_count) || frame_count > 10000000ULL) {
        return false;
    }

    g_mutex_lock(&tas_playback_lock);
    qatomic_set(&tas_playback_count, 0);
    qatomic_set(&tas_playback_cursor, 0);

    if (frame_count) {
        const size_t bytes = (size_t)frame_count * XEMU_TAS_FRAME_REPORT_BYTES;
        if (bytes > tas_playback_capacity) {
            tas_playback_reports = g_realloc(tas_playback_reports, bytes);
            tas_playback_capacity = bytes;
        }
        memcpy(tas_playback_reports, reports, bytes);
        /* Stored lag flags are editor metadata; playback re-detects guest
         * polling live and does not need a second per-frame lag allocation. */
        (void)lag_flags;
        qatomic_set(&tas_playback_count, frame_count);
    }
    g_mutex_unlock(&tas_playback_lock);
    return true;
}

bool xemu_tas_start_playback(uint64_t start_frame)
{
    if (!xemu_tas_enabled()) {
        xemu_tas_set_enabled(true);
    }
    xemu_tas_stop_recording();

    bool ok = false;
    g_mutex_lock(&tas_playback_lock);
    if (tas_playback_reports && start_frame < qatomic_read(&tas_playback_count)) {
        qatomic_set(&tas_playback_cursor, start_frame);
        g_atomic_int_set(&tas_playback_active, 1);
        tas_install_playback_frame_locked(start_frame);
        ok = true;
    }
    g_mutex_unlock(&tas_playback_lock);
    return ok;
}

bool xemu_tas_start_overdub(uint64_t start_frame, uint8_t port,
                            uint16_t digital_mask, uint8_t analog_mask,
                            uint8_t stick_mask)
{
    if (port >= XEMU_TAS_MAX_PORTS) {
        return false;
    }
    if (!xemu_tas_enabled()) {
        xemu_tas_set_enabled(true);
    }

    bool ok = false;
    g_mutex_lock(&tas_playback_lock);
    if (tas_playback_reports && start_frame < qatomic_read(&tas_playback_count)) {
        qatomic_set(&tas_playback_cursor, start_frame);
        g_atomic_int_set(&tas_overdub_port, port);
        g_atomic_int_set(&tas_overdub_digital_mask, digital_mask);
        g_atomic_int_set(&tas_overdub_aux_masks,
                         (gint)analog_mask | ((gint)(stick_mask & 0x0f) << 8));
        g_atomic_int_set(&tas_overdub_active, 1);
        g_atomic_int_set(&tas_playback_active, 1);
        tas_install_playback_frame_locked(start_frame);
        ok = true;
    }
    g_mutex_unlock(&tas_playback_lock);

    if (ok) {
        xemu_tas_clear_recording();
        g_atomic_int_set(&tas_recording_active, 1);
    }
    return ok;
}

void xemu_tas_stop_overdub(void)
{
    if (!g_atomic_int_get(&tas_overdub_active)) {
        return;
    }
    g_atomic_int_set(&tas_overdub_active, 0);
    g_atomic_int_set(&tas_recording_active, 0);
    g_atomic_int_set(&tas_playback_active, 0);
    xemu_tas_clear_all_xid_reports();
}

bool xemu_tas_overdub(void)
{
    return g_atomic_int_get(&tas_overdub_active) != 0;
}

void xemu_tas_stop_playback(void)
{
    bool was_overdub = g_atomic_int_get(&tas_overdub_active) != 0;
    g_atomic_int_set(&tas_playback_active, 0);
    g_atomic_int_set(&tas_overdub_active, 0);
    if (was_overdub) {
        g_atomic_int_set(&tas_recording_active, 0);
    }
    xemu_tas_clear_all_xid_reports();
}

bool xemu_tas_playback(void)
{
    return g_atomic_int_get(&tas_playback_active) != 0;
}

uint64_t xemu_tas_playback_frame(void)
{
    return qatomic_read(&tas_playback_cursor);
}

uint64_t xemu_tas_playback_frame_count(void)
{
    return qatomic_read(&tas_playback_count);
}

static inline void tas_note_xid_poll(uint8_t port)
{
    const gint bit = 1 << port;
    /* The first poll in a frame performs the atomic RMW. Subsequent polls only
     * read the already-set bit, which is substantially cheaper in games that
     * hammer the controller endpoint many times per VBLANK. */
    if (!(qatomic_read(&tas_polled_mask) & bit)) {
        qatomic_or(&tas_polled_mask, bit);
    }
}

static void tas_capture_xid_report(uint8_t port, const void *report)
{
    /* One canonical observation per TAS/VBLANK frame is enough for display,
     * recording and scripting. */
    const uint64_t frame = xemu_tas_frame();
    if (qatomic_read(&tas_last_observed_frame[port]) == frame) {
        return;
    }

    const int current = g_atomic_int_get(&tas_last_report_index[port]) & 1;
    const int next = current ^ 1;
    memcpy(tas_last_reports[port][next], report, XEMU_TAS_XID_REPORT_SIZE);
    g_atomic_int_set(&tas_last_report_index[port], next);
    qatomic_set(&tas_last_observed_frame[port], frame);
    g_atomic_int_set(&tas_last_report_valid[port], 1);
}

static void tas_apply_automation_unchecked(uint8_t port, uint8_t *r)
{
    uint16_t buttons;
    memcpy(&buttons, r + 2, sizeof(buttons));
    const uint64_t frame = xemu_tas_frame();

    unsigned controls =
        (unsigned)g_atomic_int_get(&tas_automation_control_mask[port]) & 0xffffu;
    for (unsigned control = 0; control < 16 && controls; ++control) {
        const unsigned bit = 1u << control;
        if (!(controls & bit)) {
            continue;
        }
        controls &= ~bit;
        const uint64_t word = qatomic_read(&tas_automation_packed[port][control]);
        const bool hold_enabled =
            (word & (UINT64_C(1) << TAS_AUTO_HOLD_ENABLED_BIT)) != 0;
        const bool autofire_enabled =
            (word & (UINT64_C(1) << TAS_AUTO_FIRE_ENABLED_BIT)) != 0;
        const uint8_t hold_value =
            (uint8_t)((word >> TAS_AUTO_HOLD_VALUE_SHIFT) & TAS_AUTO_BYTE_MASK);
        const uint8_t autofire_value =
            (uint8_t)((word >> TAS_AUTO_FIRE_VALUE_SHIFT) & TAS_AUTO_BYTE_MASK);
        const uint32_t period =
            (uint32_t)((word >> TAS_AUTO_PERIOD_SHIFT) & TAS_AUTO_U16_MASK);
        const uint32_t phase =
            (uint32_t)((word >> TAS_AUTO_PHASE_SHIFT) & TAS_AUTO_U16_MASK);

        if (hold_enabled) {
            if (control < 8) {
                buttons |= (uint16_t)(1u << control);
            } else {
                r[4 + (control - 8)] = hold_value;
            }
            continue;
        }

        if (autofire_enabled && period) {
            const bool on = ((frame + phase) % period) == 0;
            if (control < 8) {
                if (on) {
                    buttons |= (uint16_t)(1u << control);
                } else {
                    buttons &= (uint16_t)~(1u << control);
                }
            } else {
                r[4 + (control - 8)] = on ? autofire_value : 0;
            }
        }
    }
    memcpy(r + 2, &buttons, sizeof(buttons));
}

static void tas_override_report_unchecked(uint8_t port, uint8_t *report)
{
    const int index = g_atomic_int_get(&tas_report_index[port]) & 1;
    const uint8_t *base = tas_reports[port][index];

    if (g_atomic_int_get(&tas_overdub_active) &&
        port == (uint8_t)g_atomic_int_get(&tas_overdub_port)) {
        uint8_t host[XEMU_TAS_XID_REPORT_SIZE];
        uint8_t merged[XEMU_TAS_XID_REPORT_SIZE];
        memcpy(host, report, sizeof(host));
        memcpy(merged, base, sizeof(merged));

        const uint16_t digital_mask =
            (uint16_t)g_atomic_int_get(&tas_overdub_digital_mask);
        const unsigned aux_masks = (unsigned)g_atomic_int_get(&tas_overdub_aux_masks);
        const uint8_t analog_mask = (uint8_t)(aux_masks & 0xffu);
        const uint8_t stick_mask = (uint8_t)((aux_masks >> 8) & 0x0fu);

        uint16_t base_buttons, host_buttons;
        memcpy(&base_buttons, merged + 2, sizeof(base_buttons));
        memcpy(&host_buttons, host + 2, sizeof(host_buttons));
        base_buttons = (base_buttons & ~digital_mask) |
                       (host_buttons & digital_mask);
        memcpy(merged + 2, &base_buttons, sizeof(base_buttons));

        for (unsigned i = 0; i < 8; ++i) {
            if (analog_mask & (1u << i)) {
                merged[4 + i] = host[4 + i];
            }
        }
        for (unsigned i = 0; i < 4; ++i) {
            if (stick_mask & (1u << i)) {
                memcpy(merged + 12 + i * 2, host + 12 + i * 2, 2);
            }
        }
        memcpy(report, merged, sizeof(merged));
    } else {
        memcpy(report, base, XEMU_TAS_XID_REPORT_SIZE);
    }
}

void xemu_tas_process_xid_report(uint8_t port, void *report, size_t size)
{
    if (G_LIKELY(!xemu_tas_enabled()) || port >= XEMU_TAS_MAX_PORTS ||
        !report || size != XEMU_TAS_XID_REPORT_SIZE) {
        return;
    }

    const gint bit = 1 << port;
    tas_note_xid_poll(port);

    /* Automation and injected playback are uncommon relative to ordinary XID
     * polling. Summary masks keep both paths effectively free when unused. */
    if (G_UNLIKELY((g_atomic_int_get(&tas_automation_active_mask) & bit) != 0) &&
        !g_atomic_int_get(&tas_playback_active)) {
        tas_apply_automation_unchecked(port, report);
    }

    if (G_UNLIKELY((g_atomic_int_get(&tas_report_valid_mask) & bit) != 0)) {
        tas_override_report_unchecked(port, report);
    }

    /* Recording always needs the canonical guest-visible report. Otherwise a
     * consumer (input display/script) explicitly requests one and the request
     * is satisfied by the next guest poll. */
    const bool recording = g_atomic_int_get(&tas_recording_active) != 0;
    if (G_UNLIKELY(recording ||
                   (g_atomic_int_get(&tas_capture_request_mask) & bit) != 0)) {
        tas_capture_xid_report(port, report);
        if (!recording) {
            g_atomic_int_and(&tas_capture_request_mask, ~bit);
        }
    }
}

void xemu_tas_observe_xid_report(uint8_t port, const void *report, size_t size)
{
    if (!xemu_tas_enabled() || port >= XEMU_TAS_MAX_PORTS || !report ||
        size != XEMU_TAS_XID_REPORT_SIZE) {
        return;
    }
    tas_note_xid_poll(port);
    tas_capture_xid_report(port, report);
}

bool xemu_tas_get_last_xid_report(uint8_t port, void *report, size_t size)
{
    if (port >= XEMU_TAS_MAX_PORTS || !report ||
        size != XEMU_TAS_XID_REPORT_SIZE) {
        return false;
    }

    /* Request a fresh sample without forcing every XID poll to copy reports
     * when no UI/script consumer is interested. */
    g_atomic_int_or(&tas_capture_request_mask, 1 << port);
    if (!g_atomic_int_get(&tas_last_report_valid[port])) {
        return false;
    }

    const int index = g_atomic_int_get(&tas_last_report_index[port]) & 1;
    memcpy(report, tas_last_reports[port][index], XEMU_TAS_XID_REPORT_SIZE);
    return true;
}

bool xemu_tas_override_xid_report(uint8_t port, void *report, size_t size)
{
    if (!xemu_tas_enabled() || port >= XEMU_TAS_MAX_PORTS || !report ||
        size != XEMU_TAS_XID_REPORT_SIZE ||
        !(g_atomic_int_get(&tas_report_valid_mask) & (1 << port))) {
        return false;
    }
    tas_override_report_unchecked(port, report);
    tas_note_xid_poll(port);
    tas_capture_xid_report(port, report);
    return true;
}

void xemu_tas_set_xid_report(uint8_t port, const void *report, size_t size)
{
    if (port >= XEMU_TAS_MAX_PORTS || !report ||
        size != XEMU_TAS_XID_REPORT_SIZE) {
        return;
    }

    const int current = g_atomic_int_get(&tas_report_index[port]) & 1;
    const int next = current ^ 1;
    memcpy(tas_reports[port][next], report, XEMU_TAS_XID_REPORT_SIZE);
    g_atomic_int_set(&tas_report_index[port], next);
    g_atomic_int_or(&tas_report_valid_mask, 1 << port);
}

void xemu_tas_clear_xid_report(uint8_t port)
{
    if (port >= XEMU_TAS_MAX_PORTS) {
        return;
    }
    g_atomic_int_and(&tas_report_valid_mask, ~(1 << port));
}

void xemu_tas_clear_all_xid_reports(void)
{
    g_atomic_int_set(&tas_report_valid_mask, 0);
}

void xemu_tas_set_auto_hold(uint8_t port, uint8_t control, bool enabled, uint8_t value)
{
    if (port >= XEMU_TAS_MAX_PORTS || control >= 16) {
        return;
    }
    uint64_t word = qatomic_read(&tas_automation_packed[port][control]);
    word &= ~((UINT64_C(1) << TAS_AUTO_HOLD_ENABLED_BIT) |
              (TAS_AUTO_BYTE_MASK << TAS_AUTO_HOLD_VALUE_SHIFT));
    if (enabled) {
        word |= UINT64_C(1) << TAS_AUTO_HOLD_ENABLED_BIT;
    }
    word |= (uint64_t)value << TAS_AUTO_HOLD_VALUE_SHIFT;
    qatomic_set(&tas_automation_packed[port][control], word);
    tas_refresh_automation_mask(port);
}

void xemu_tas_set_autofire(uint8_t port, uint8_t control, bool enabled, uint8_t value,
                           uint32_t period, uint32_t phase)
{
    if (port >= XEMU_TAS_MAX_PORTS || control >= 16) {
        return;
    }
    period = MIN(MAX(period, 1u), UINT16_MAX);
    phase = MIN(phase % period, UINT16_MAX);

    uint64_t word = qatomic_read(&tas_automation_packed[port][control]);
    word &= ~((UINT64_C(1) << TAS_AUTO_FIRE_ENABLED_BIT) |
              (TAS_AUTO_BYTE_MASK << TAS_AUTO_FIRE_VALUE_SHIFT) |
              (TAS_AUTO_U16_MASK << TAS_AUTO_PERIOD_SHIFT) |
              (TAS_AUTO_U16_MASK << TAS_AUTO_PHASE_SHIFT));
    if (enabled) {
        word |= UINT64_C(1) << TAS_AUTO_FIRE_ENABLED_BIT;
    }
    word |= (uint64_t)value << TAS_AUTO_FIRE_VALUE_SHIFT;
    word |= (uint64_t)period << TAS_AUTO_PERIOD_SHIFT;
    word |= (uint64_t)phase << TAS_AUTO_PHASE_SHIFT;
    qatomic_set(&tas_automation_packed[port][control], word);
    tas_refresh_automation_mask(port);
}

void xemu_tas_clear_automation(uint8_t port)
{
    if (port >= XEMU_TAS_MAX_PORTS) {
        return;
    }
    for (unsigned control = 0; control < 16; ++control) {
        qatomic_set(&tas_automation_packed[port][control], 0);
    }
    g_atomic_int_set(&tas_automation_control_mask[port], 0);
    g_atomic_int_and(&tas_automation_active_mask, ~(1 << port));
}

void xemu_tas_apply_xid_automation(uint8_t port, void *report, size_t size)
{
    if (!xemu_tas_enabled() || port >= XEMU_TAS_MAX_PORTS || !report ||
        size != XEMU_TAS_XID_REPORT_SIZE || xemu_tas_playback() ||
        !(g_atomic_int_get(&tas_automation_active_mask) & (1 << port))) {
        return;
    }
    tas_apply_automation_unchecked(port, report);
}
