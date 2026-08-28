/*
 * xemu custom fork - optional Fast Forward guest/presentation timing policy.
 *
 * Core video code supplies ordinary VBLANK timing and host timestamps. This
 * module owns multiplier/unlimited policy and all feature-specific render
 * throttling state.
 */
#include "qemu/osdep.h"
#include "system/cpu-timers.h"
#include "xemu-features/fast-forward/fast-forward.h"
#include "xemu-features/fast-forward/timing.h"

static int g_render_divider;
static int g_last_multiplier = 1;
static bool g_last_unlimited;
static int64_t g_last_render_ns;

static int effective_mode(bool running, bool tas_stepping)
{
    return (running && !tas_stepping) ? xemu_fast_forward_mode() : 1;
}

bool xemu_fast_forward_should_render_vblank(int64_t normal_interval_ns,
                                             bool running,
                                             bool tas_stepping,
                                             int64_t host_now_ns)
{
    int mode = effective_mode(running, tas_stepping);
    bool unlimited = mode == 0;
    int multiplier = mode >= 2 ? mode : 1;
    bool render = true;

    if (unlimited != g_last_unlimited || multiplier != g_last_multiplier) {
        g_render_divider = 0;
        g_last_render_ns = host_now_ns;
        g_last_multiplier = multiplier;
        g_last_unlimited = unlimited;
    }

    if (unlimited) {
        render = (host_now_ns - g_last_render_ns) >= normal_interval_ns;
        if (render) {
            g_last_render_ns = host_now_ns;
        }
    } else if (multiplier > 1) {
        g_render_divider++;
        if (g_render_divider >= multiplier) {
            g_render_divider = 0;
        } else {
            render = false;
        }
    }

    return render;
}

int64_t xemu_fast_forward_timer_interval_ns(int64_t normal_interval_ns,
                                             bool running,
                                             bool tas_stepping)
{
    int mode = effective_mode(running, tas_stepping);
    if (mode == 0) {
        return 1;
    }
    return MAX(1LL, normal_interval_ns / (mode >= 2 ? mode : 1));
}

int64_t xemu_fast_forward_thread_interval_ns(int64_t normal_interval_ns,
                                              bool running)
{
    /* Preserve the existing thread path exactly: TAS frame stepping affects
     * render/guest-clock policy at the VBLANK boundary, but not this helper
     * thread's wake cadence. */
    int mode = running ? xemu_fast_forward_mode() : 1;
    if (mode == 0) {
        return 0;
    }
    return MAX(1LL, normal_interval_ns / (mode >= 2 ? mode : 1));
}

void xemu_fast_forward_advance_guest_clock(int64_t normal_interval_ns,
                                            bool running,
                                            bool tas_stepping)
{
    if (!running || tas_stepping) {
        return;
    }

    int mode = xemu_fast_forward_mode();
    if (mode == 1) {
        return;
    }
    if (mode == 0) {
        xemu_virtual_clock_advance_ns(normal_interval_ns);
        return;
    }

    int64_t normal_part = normal_interval_ns / mode;
    xemu_virtual_clock_advance_ns(normal_interval_ns - normal_part);
}
