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
#include "qemu/module.h"
#include "qemu/timer.h"
#include "io/channel-buffer.h"
#include "migration/qemu-file.h"
#include "migration/savevm.h"
#include "migration/vmstate.h"
#include "system/runstate.h"
#include "system/reset.h"
#include "xemu-features/tas/tas.h"
#include "xemu-features/fast-forward/fast-forward.h"

/* Feature-owned identity carried inside native QEMU snapshots. It does not
 * alter Xbox-visible state and is deliberately zeroed while deterministic
 * device-state fingerprints are serialized. */
typedef struct XemuTasVMState {
    uint64_t transaction_hi;
    uint64_t transaction_lo;
} XemuTasVMState;

static XemuTasVMState tas_vmstate;
static uint64_t tas_transaction_sequence;
static gint tas_post_reset_armed;
static uint64_t tas_post_reset_sequence;
static gint tas_transaction_snapshot_active;
static gsize tas_reset_registration_once;

static bool xemu_tas_vmstate_needed(void *opaque)
{
    (void)opaque;
    /* This handler is permanently available for load compatibility, but an
     * ordinary Xemu Save State must not acquire TAS-only metadata just because
     * TAStudio happened to be opened earlier in the session. */
    return qatomic_read(&tas_transaction_snapshot_active) != 0;
}

static const VMStateDescription vmstate_xemu_tas = {
    .name = "xemu/tas-transaction",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = xemu_tas_vmstate_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT64(transaction_hi, XemuTasVMState),
        VMSTATE_UINT64(transaction_lo, XemuTasVMState),
        VMSTATE_END_OF_LIST()
    }
};

static void xemu_tas_system_reset_observer(void *opaque)
{
    (void)opaque;
    if (!g_atomic_int_compare_and_exchange(&tas_post_reset_armed, 1, 0)) {
        return;
    }

    /* The callback runs inside QEMU's actual system-reset traversal. Request
     * a stop before the main loop can resume guest execution. The sequence is
     * advanced only after the reset callback is reached, so Studio can wait
     * for both this proof and RUN_STATE_PAUSED before establishing frame 0. */
    qatomic_inc(&tas_post_reset_sequence);
    qemu_system_vmstop_request_prepare();
    qemu_system_vmstop_request(RUN_STATE_PAUSED);
}

static void xemu_tas_register_vmstate_compat(void)
{
    /* Passive compatibility registration is safe here: unlike reset
     * registration it does not construct a QOM object. .needed keeps this
     * section out of normal snapshots while still allowing old regular states
     * accidentally containing xemu/tas-transaction to deserialize. */
    vmstate_register(NULL, 0, &vmstate_xemu_tas, &tas_vmstate);
}
type_init(xemu_tas_register_vmstate_compat);

static void xemu_tas_ensure_reset_observer(void)
{
    /* The reset API constructs legacy-reset QOM state immediately. Keep this
     * lazy so it only runs after QOM type initialization has completed. */
    if (g_once_init_enter(&tas_reset_registration_once)) {
        qemu_register_reset_nosnapshotload(xemu_tas_system_reset_observer, NULL);
        g_once_init_leave(&tas_reset_registration_once, 1);
    }
}

void xemu_tas_prepare_runtime(void)
{
    /* Stable API retained for Studio load paths. VMState load compatibility
     * is already registered passively; power-on TAS registers reset handling
     * only when xemu_tas_arm_post_reset_pause() is actually used. */
}

void xemu_tas_transaction_snapshot_begin(void)
{
    qatomic_set(&tas_transaction_snapshot_active, 1);
}

void xemu_tas_transaction_snapshot_end(void)
{
    qatomic_set(&tas_transaction_snapshot_active, 0);
}

static gint tas_enabled;
static gint tas_deterministic_mode;
static uint64_t tas_frame_counter;
static uint32_t tas_frame_advance_remaining;
static gint tas_frame_advance_skip_lag;
static gint tas_pause_request;
/* True only while TAStudio is reconstructing an exact seek target from an
 * earlier greenzone checkpoint. Presentation code may suppress intermediate
 * frames, but guest execution/input timing remains ordinary TAS stepping. */
static gint tas_seek_catchup;

static gint tas_polled_mask;
static uint32_t tas_frame_poll_counts[XEMU_TAS_MAX_PORTS];
static gint tas_last_frame_lag;
static uint64_t tas_lag_count_value;
static uint64_t tas_lag_streak_value;

static gint tas_desync_valid;
static XemuTasDesyncInfo tas_desync_info;

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
/* Frame-mode recording is intentionally immutable within a VBLANK: the first
 * guest-visible report for each port becomes canonical and every later poll
 * in that frame sees the exact same bytes. Poll-specific input can be added
 * later as an explicit movie mode rather than leaking host timing into a
 * nominally frame-based movie. */
static uint8_t tas_record_latched_reports[XEMU_TAS_MAX_PORTS][XEMU_TAS_XID_REPORT_SIZE];
static uint64_t tas_record_latched_frame[XEMU_TAS_MAX_PORTS];

/* Recording buffer. */
typedef struct XemuTasRecordedFrame {
    uint8_t reports[XEMU_TAS_FRAME_REPORT_BYTES];
    uint32_t poll_counts[XEMU_TAS_MAX_PORTS];
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
static uint32_t *tas_playback_poll_counts;
static size_t tas_playback_poll_capacity;
static uint32_t *tas_playback_actual_poll_counts;
static size_t tas_playback_actual_poll_capacity;
static gint tas_playback_has_poll_trace;
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

static uint64_t tas_hash_bytes(const void *data, size_t size, uint64_t h)
{
    const uint8_t *p = data;
    if (!h) {
        h = UINT64_C(1469598103934665603);
    }
    for (size_t i = 0; i < size; ++i) {
        h ^= p[i];
        h *= UINT64_C(1099511628211);
    }
    return h;
}

static uint64_t tas_mix64(uint64_t x)
{
    x ^= x >> 30;
    x *= UINT64_C(0xbf58476d1ce4e5b9);
    x ^= x >> 27;
    x *= UINT64_C(0x94d049bb133111eb);
    x ^= x >> 31;
    return x;
}

void xemu_tas_transaction_mint(XemuTasTransactionId *out)
{
    const uint64_t seq = qatomic_fetch_inc(&tas_transaction_sequence) + 1;
    const uint64_t wall = (uint64_t)g_get_real_time();
    const uint64_t host = (uint64_t)qemu_clock_get_ns(QEMU_CLOCK_HOST);
    uint64_t hi = tas_mix64(wall ^ (seq * UINT64_C(0x9e3779b97f4a7c15)));
    uint64_t lo = tas_mix64(host ^ (seq * UINT64_C(0xd6e8feb86659fd93)) ^
                            (wall << 17) ^ (wall >> 11));
    if (!(hi | lo)) {
        lo = seq ? seq : 1;
    }
    qatomic_set(&tas_vmstate.transaction_hi, hi);
    qatomic_set(&tas_vmstate.transaction_lo, lo);
    if (out) {
        out->hi = hi;
        out->lo = lo;
    }
}

void xemu_tas_transaction_get(XemuTasTransactionId *out)
{
    if (!out) {
        return;
    }
    out->hi = qatomic_read(&tas_vmstate.transaction_hi);
    out->lo = qatomic_read(&tas_vmstate.transaction_lo);
}

bool xemu_tas_transaction_matches(const XemuTasTransactionId *id)
{
    if (!id) {
        return false;
    }
    return qatomic_read(&tas_vmstate.transaction_hi) == id->hi &&
           qatomic_read(&tas_vmstate.transaction_lo) == id->lo;
}

bool xemu_tas_machine_state_hash(uint64_t seed, uint64_t *out_hash)
{
    /* qemu_save_device_state() serializes every registered non-RAM VMState
     * section (CPU, timers, NV2A/APU/IDE/USB, etc.) in canonical migration
     * encoding. This is a substantially stronger verifier surface than a
     * hand-picked register list and automatically follows future device state
     * additions. Callers invoke it only at a paused canonical TAS boundary. */
    if (!out_hash || runstate_is_running()) {
        return false;
    }

    QIOChannelBuffer *bioc = qio_channel_buffer_new(1024 * 1024);
    if (!bioc) {
        return false;
    }
    QEMUFile *f = qemu_file_new_output(QIO_CHANNEL(bioc));
    if (!f) {
        object_unref(OBJECT(bioc));
        return false;
    }

    /* Transaction ownership is intentionally not part of emulated-machine
     * determinism. Temporarily zero the feature-owned fields before producing
     * the canonical non-RAM stream, then restore them before returning. */
    const uint64_t saved_hi = qatomic_read(&tas_vmstate.transaction_hi);
    const uint64_t saved_lo = qatomic_read(&tas_vmstate.transaction_lo);
    qatomic_set(&tas_vmstate.transaction_hi, 0);
    qatomic_set(&tas_vmstate.transaction_lo, 0);

    const int save_ret = qemu_save_device_state(f);
    const int flush_ret = qemu_fflush(f);
    bool ok = save_ret == 0 && flush_ret == 0 && bioc->usage != 0;
    uint64_t h = seed;
    if (ok) {
        static const uint64_t format_tag = UINT64_C(0x58454d5554415332); /* XEMUTAS2 */
        h = tas_hash_bytes(&format_tag, sizeof(format_tag), h);
        h = tas_hash_bytes(bioc->data, bioc->usage, h);
    }

    qatomic_set(&tas_vmstate.transaction_hi, saved_hi);
    qatomic_set(&tas_vmstate.transaction_lo, saved_lo);
    qemu_fclose(f);
    object_unref(OBJECT(bioc));
    if (ok) {
        *out_hash = h;
    }
    return ok;
}


uint64_t xemu_tas_arm_post_reset_pause(void)
{
    xemu_tas_ensure_reset_observer();
    const uint64_t before = qatomic_read(&tas_post_reset_sequence);
    g_atomic_int_set(&tas_post_reset_armed, 1);
    return before;
}

void xemu_tas_cancel_post_reset_pause(void)
{
    g_atomic_int_set(&tas_post_reset_armed, 0);
}

uint64_t xemu_tas_post_reset_sequence(void)
{
    return qatomic_read(&tas_post_reset_sequence);
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
        g_atomic_int_set(&tas_desync_valid, 0);
        xemu_tas_reset_lag_counters();
        for (unsigned port = 0; port < XEMU_TAS_MAX_PORTS; ++port) {
            qatomic_set(&tas_last_observed_frame[port], UINT64_MAX);
            qatomic_set(&tas_record_latched_frame[port], UINT64_MAX);
            qatomic_set(&tas_frame_poll_counts[port], 0);
        }
    }

    if (!enabled) {
        xemu_tas_cancel_post_reset_pause();
        qatomic_set(&tas_frame_advance_remaining, 0);
        g_atomic_int_set(&tas_frame_advance_skip_lag, 0);
        g_atomic_int_set(&tas_pause_request, 0);
        g_atomic_int_set(&tas_seek_catchup, 0);
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
        qatomic_set(&tas_record_latched_frame[port], UINT64_MAX);
        qatomic_set(&tas_frame_poll_counts[port], 0);
    }
}

bool xemu_tas_deterministic_mode(void)
{
    return g_atomic_int_get(&tas_deterministic_mode) != 0;
}

void xemu_tas_set_deterministic_mode(bool enabled)
{
    g_atomic_int_set(&tas_deterministic_mode, enabled ? 1 : 0);
    if (enabled) {
        /* Host-paced Fast Forward changes the realtime/VBLANK relationship and
         * is incompatible with strict TAS replay. Deterministic mode owns this
         * policy so loading a deterministic movie cannot accidentally leave FF
         * active from an earlier gameplay session. */
        xemu_fast_forward_set_active(false);
        if (!xemu_tas_enabled()) {
            xemu_tas_set_enabled(true);
        }
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

void xemu_tas_cancel_frame_advance(void)
{
    qatomic_set(&tas_frame_advance_remaining, 0);
    g_atomic_int_set(&tas_frame_advance_skip_lag, 0);
    g_atomic_int_set(&tas_pause_request, 0);
}

uint32_t xemu_tas_frame_advance_remaining(void)
{
    return qatomic_read(&tas_frame_advance_remaining);
}

bool xemu_tas_consume_pause_request(void)
{
    return qatomic_xchg(&tas_pause_request, 0) != 0;
}

bool xemu_tas_seek_catchup(void)
{
    return g_atomic_int_get(&tas_seek_catchup) != 0;
}

void xemu_tas_set_seek_catchup(bool active)
{
    g_atomic_int_set(&tas_seek_catchup, active ? 1 : 0);
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
    xemu_tas_set_lag_state(false, 0, 0);
}

void xemu_tas_set_lag_state(bool last_frame_lagged, uint64_t lag_count,
                            uint64_t lag_streak)
{
    g_atomic_int_set(&tas_last_frame_lag, last_frame_lagged ? 1 : 0);
    qatomic_set(&tas_lag_count_value, lag_count);
    qatomic_set(&tas_lag_streak_value, lag_streak);
}

static void tas_append_recorded_frame(bool lagged,
                                      const uint32_t polls[XEMU_TAS_MAX_PORTS])
{
    if (!g_atomic_int_get(&tas_recording_active)) {
        return;
    }

    XemuTasRecordedFrame frame = { 0 };
    frame.lagged = lagged ? 1 : 0;
    for (uint8_t port = 0; port < XEMU_TAS_MAX_PORTS; ++port) {
        frame.poll_counts[port] = polls[port];
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

    uint32_t polls[XEMU_TAS_MAX_PORTS];
    for (unsigned port = 0; port < XEMU_TAS_MAX_PORTS; ++port) {
        polls[port] = qatomic_xchg(&tas_frame_poll_counts[port], 0);
    }
    const bool lagged = qatomic_xchg(&tas_polled_mask, 0) == 0;
    g_atomic_int_set(&tas_last_frame_lag, lagged ? 1 : 0);
    if (lagged) {
        qatomic_inc(&tas_lag_count_value);
        qatomic_inc(&tas_lag_streak_value);
    } else {
        qatomic_set(&tas_lag_streak_value, 0);
    }

    tas_append_recorded_frame(lagged, polls);

    bool poll_desync = false;
    if (g_atomic_int_get(&tas_playback_active) &&
        g_atomic_int_get(&tas_playback_has_poll_trace)) {
        const uint64_t cursor = qatomic_read(&tas_playback_cursor);
        const uint64_t count = qatomic_read(&tas_playback_count);
        if (cursor < count && tas_playback_poll_counts) {
            const uint32_t *expected = tas_playback_poll_counts +
                cursor * XEMU_TAS_MAX_PORTS;
            for (uint8_t port = 0; port < XEMU_TAS_MAX_PORTS; ++port) {
                if (expected[port] != UINT32_MAX && expected[port] != polls[port]) {
                    if (!g_atomic_int_get(&tas_desync_valid)) {
                        tas_desync_info.valid = true;
                        tas_desync_info.frame = cursor;
                        tas_desync_info.port = port;
                        tas_desync_info.expected_polls = expected[port];
                        tas_desync_info.actual_polls = polls[port];
                        g_atomic_int_set(&tas_desync_valid, 1);
                    }
                    poll_desync = true;
                    break;
                }
            }
        }
    }

    if (g_atomic_int_get(&tas_playback_active)) {
        const uint64_t cursor = qatomic_read(&tas_playback_cursor);
        const uint64_t count = qatomic_read(&tas_playback_count);
        if (cursor < count && tas_playback_actual_poll_counts) {
            memcpy(tas_playback_actual_poll_counts + cursor * XEMU_TAS_MAX_PORTS,
                   polls, sizeof(polls));
        }
    }

    qatomic_inc(&tas_frame_counter);

    if (poll_desync) {
        /* Fail closed at the first controller-poll divergence. The completed
         * movie frame is reported in tas_desync_info; the VM is paused at the
         * immediately following canonical VBLANK boundary. */
        g_atomic_int_set(&tas_playback_active, 0);
        g_atomic_int_set(&tas_overdub_active, 0);
        g_atomic_int_set(&tas_recording_active, 0);
        xemu_tas_clear_all_xid_reports();
        qatomic_set(&tas_frame_advance_remaining, 0);
        g_atomic_int_set(&tas_frame_advance_skip_lag, 0);
        g_atomic_int_set(&tas_pause_request, 1);
        return;
    }

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
    g_atomic_int_set(&tas_desync_valid, 0);
    for (unsigned port = 0; port < XEMU_TAS_MAX_PORTS; ++port) {
        qatomic_set(&tas_record_latched_frame[port], UINT64_MAX);
    }
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
    return xemu_tas_copy_recorded_frames_ex(start, max_frames, reports,
                                             reports_size, lag_flags,
                                             lag_flags_size, NULL, 0);
}

uint64_t xemu_tas_copy_recorded_frames_ex(uint64_t start, uint64_t max_frames,
                                          void *reports, size_t reports_size,
                                          uint8_t *lag_flags, size_t lag_flags_size,
                                          uint32_t *poll_counts,
                                          size_t poll_counts_count)
{
    if (!reports || !max_frames ||
        max_frames > SIZE_MAX / XEMU_TAS_FRAME_REPORT_BYTES) {
        return 0;
    }

    const size_t needed_reports = (size_t)max_frames * XEMU_TAS_FRAME_REPORT_BYTES;
    if (reports_size < needed_reports ||
        (lag_flags && lag_flags_size < (size_t)max_frames) ||
        (poll_counts &&
         (max_frames > SIZE_MAX / XEMU_TAS_MAX_PORTS ||
          poll_counts_count < (size_t)max_frames * XEMU_TAS_MAX_PORTS))) {
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
            if (poll_counts) {
                memcpy(poll_counts + i * XEMU_TAS_MAX_PORTS,
                       f->poll_counts, sizeof(f->poll_counts));
            }
        }
    }
    g_mutex_unlock(&tas_recording_lock);
    return copied;
}

uint64_t xemu_tas_copy_playback_poll_trace(uint64_t start, uint64_t max_frames,
                                           uint32_t *poll_counts,
                                           size_t poll_counts_count)
{
    if (!poll_counts || !max_frames ||
        max_frames > SIZE_MAX / XEMU_TAS_MAX_PORTS ||
        poll_counts_count < (size_t)max_frames * XEMU_TAS_MAX_PORTS) {
        return 0;
    }

    uint64_t copied = 0;
    g_mutex_lock(&tas_playback_lock);
    const uint64_t count = qatomic_read(&tas_playback_count);
    if (tas_playback_actual_poll_counts && start < count) {
        copied = MIN(max_frames, count - start);
        memcpy(poll_counts,
               tas_playback_actual_poll_counts + start * XEMU_TAS_MAX_PORTS,
               (size_t)copied * XEMU_TAS_MAX_PORTS * sizeof(uint32_t));
    }
    g_mutex_unlock(&tas_playback_lock);
    return copied;
}

bool xemu_tas_set_playback_movie(const void *reports, const uint8_t *lag_flags,
                                 uint64_t frame_count)
{
    return xemu_tas_set_playback_movie_ex(reports, lag_flags, NULL, 0,
                                           frame_count);
}

bool xemu_tas_set_playback_movie_ex(const void *reports, const uint8_t *lag_flags,
                                    const uint32_t *poll_counts,
                                    size_t poll_counts_count,
                                    uint64_t frame_count)
{
    if ((!reports && frame_count) || frame_count > 10000000ULL ||
        (poll_counts &&
         (frame_count > SIZE_MAX / XEMU_TAS_MAX_PORTS ||
          poll_counts_count < (size_t)frame_count * XEMU_TAS_MAX_PORTS))) {
        return false;
    }

    g_mutex_lock(&tas_playback_lock);
    qatomic_set(&tas_playback_count, 0);
    qatomic_set(&tas_playback_cursor, 0);
    g_atomic_int_set(&tas_playback_has_poll_trace, 0);

    if (frame_count) {
        const size_t bytes = (size_t)frame_count * XEMU_TAS_FRAME_REPORT_BYTES;
        const size_t poll_count = (size_t)frame_count * XEMU_TAS_MAX_PORTS;
        if (bytes > tas_playback_capacity) {
            tas_playback_reports = g_realloc(tas_playback_reports, bytes);
            tas_playback_capacity = bytes;
        }
        memcpy(tas_playback_reports, reports, bytes);
        if (poll_count > tas_playback_actual_poll_capacity) {
            tas_playback_actual_poll_counts = g_realloc_n(
                tas_playback_actual_poll_counts, poll_count, sizeof(uint32_t));
            tas_playback_actual_poll_capacity = poll_count;
        }
        memset(tas_playback_actual_poll_counts, 0xFF,
               poll_count * sizeof(uint32_t));
        if (poll_counts) {
            if (poll_count > tas_playback_poll_capacity) {
                tas_playback_poll_counts = g_realloc_n(tas_playback_poll_counts,
                                                        poll_count,
                                                        sizeof(uint32_t));
                tas_playback_poll_capacity = poll_count;
            }
            memcpy(tas_playback_poll_counts, poll_counts,
                   poll_count * sizeof(uint32_t));
            g_atomic_int_set(&tas_playback_has_poll_trace, 1);
        } else {
            g_atomic_int_set(&tas_playback_has_poll_trace, 0);
        }
        /* Stored lag flags are editor metadata; playback re-detects guest
         * polling live and does not need a second per-frame lag allocation. */
        (void)lag_flags;
        qatomic_set(&tas_playback_count, frame_count);
    }
    g_mutex_unlock(&tas_playback_lock);
    return true;
}

bool xemu_tas_update_playback_movie(const void *reports, const uint8_t *lag_flags,
                                    uint64_t frame_count)
{
    return xemu_tas_update_playback_movie_ex(reports, lag_flags, NULL, 0,
                                              frame_count);
}

bool xemu_tas_update_playback_movie_ex(const void *reports,
                                       const uint8_t *lag_flags,
                                       const uint32_t *poll_counts,
                                       size_t poll_counts_count,
                                       uint64_t frame_count)
{
    if ((!reports && frame_count) || frame_count > 10000000ULL ||
        (poll_counts &&
         (frame_count > SIZE_MAX / XEMU_TAS_MAX_PORTS ||
          poll_counts_count < (size_t)frame_count * XEMU_TAS_MAX_PORTS))) {
        return false;
    }

    g_mutex_lock(&tas_playback_lock);
    const bool active = g_atomic_int_get(&tas_playback_active) != 0;
    uint64_t cursor = qatomic_read(&tas_playback_cursor);

    if (frame_count) {
        const size_t bytes = (size_t)frame_count * XEMU_TAS_FRAME_REPORT_BYTES;
        const size_t poll_count = (size_t)frame_count * XEMU_TAS_MAX_PORTS;
        if (bytes > tas_playback_capacity) {
            tas_playback_reports = g_realloc(tas_playback_reports, bytes);
            tas_playback_capacity = bytes;
        }
        memcpy(tas_playback_reports, reports, bytes);
        if (poll_count > tas_playback_actual_poll_capacity) {
            tas_playback_actual_poll_counts = g_realloc_n(
                tas_playback_actual_poll_counts, poll_count, sizeof(uint32_t));
            tas_playback_actual_poll_capacity = poll_count;
        }
        memset(tas_playback_actual_poll_counts, 0xFF,
               poll_count * sizeof(uint32_t));
        if (poll_counts) {
            if (poll_count > tas_playback_poll_capacity) {
                tas_playback_poll_counts = g_realloc_n(tas_playback_poll_counts,
                                                        poll_count,
                                                        sizeof(uint32_t));
                tas_playback_poll_capacity = poll_count;
            }
            memcpy(tas_playback_poll_counts, poll_counts,
                   poll_count * sizeof(uint32_t));
            g_atomic_int_set(&tas_playback_has_poll_trace, 1);
        } else {
            g_atomic_int_set(&tas_playback_has_poll_trace, 0);
        }
        (void)lag_flags;
        qatomic_set(&tas_playback_count, frame_count);
        cursor = MIN(cursor, frame_count - 1);
        qatomic_set(&tas_playback_cursor, cursor);
        if (active) {
            tas_install_playback_frame_locked(cursor);
        }
    } else {
        qatomic_set(&tas_playback_count, 0);
        qatomic_set(&tas_playback_cursor, 0);
        g_atomic_int_set(&tas_playback_has_poll_trace, 0);
        if (active) {
            g_atomic_int_set(&tas_playback_active, 0);
            xemu_tas_clear_all_xid_reports();
        }
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
    g_atomic_int_set(&tas_overdub_active, 0);
    g_atomic_int_set(&tas_desync_valid, 0);

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
    g_atomic_int_set(&tas_desync_valid, 0);
    for (unsigned p = 0; p < XEMU_TAS_MAX_PORTS; ++p) {
        qatomic_set(&tas_record_latched_frame[p], UINT64_MAX);
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
    /* Poll counts are frame-boundary telemetry, not a state-publication
     * primitive. A relaxed RMW preserves exact counts without imposing a
     * global sequential-consistency fence on every controller read. */
    __atomic_fetch_add(&tas_frame_poll_counts[port], 1, __ATOMIC_RELAXED);
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

    /* Frame-mode recording latches the first guest-visible report per port and
     * forces every later poll in this VBLANK to observe the same bytes. This
     * removes accidental host-subframe variation from ordinary movies. */
    const bool recording = g_atomic_int_get(&tas_recording_active) != 0;
    if (G_UNLIKELY(recording)) {
        const uint64_t frame = xemu_tas_frame();
        if (qatomic_read(&tas_record_latched_frame[port]) != frame) {
            memcpy(tas_record_latched_reports[port], report,
                   XEMU_TAS_XID_REPORT_SIZE);
            qatomic_set(&tas_record_latched_frame[port], frame);
        } else {
            memcpy(report, tas_record_latched_reports[port],
                   XEMU_TAS_XID_REPORT_SIZE);
        }
    }

    /* Recording always needs the canonical guest-visible report. Otherwise a
     * consumer (input display/script) explicitly requests one and the request
     * is satisfied by the next guest poll. */
    if (G_UNLIKELY(recording ||
                   (g_atomic_int_get(&tas_capture_request_mask) & bit) != 0)) {
        tas_capture_xid_report(port, report);
        if (!recording) {
            g_atomic_int_and(&tas_capture_request_mask, ~bit);
        }
    }
}

bool xemu_tas_take_desync(XemuTasDesyncInfo *out)
{
    if (!out || !qatomic_xchg(&tas_desync_valid, 0)) {
        return false;
    }
    *out = tas_desync_info;
    /* tas_desync_valid is the ownership flag. Do not mutate the shared
     * payload after releasing that flag: the emulation thread may already be
     * publishing the next first-divergence record. */
    return true;
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
