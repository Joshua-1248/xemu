#ifndef XEMU_FEATURES_FAST_FORWARD_TIMING_H
#define XEMU_FEATURES_FAST_FORWARD_TIMING_H

#include "config-host.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_XEMU_FEATURE_FAST_FORWARD
bool xemu_fast_forward_should_render_vblank(int64_t normal_interval_ns,
                                             bool running,
                                             bool tas_stepping,
                                             int64_t host_now_ns);
int64_t xemu_fast_forward_timer_interval_ns(int64_t normal_interval_ns,
                                             bool running,
                                             bool tas_stepping);
int64_t xemu_fast_forward_thread_interval_ns(int64_t normal_interval_ns,
                                              bool running);
void xemu_fast_forward_advance_guest_clock(int64_t normal_interval_ns,
                                            bool running,
                                            bool tas_stepping);
#else
static inline bool xemu_fast_forward_should_render_vblank(
    int64_t normal_interval_ns, bool running, bool tas_stepping,
    int64_t host_now_ns)
{
    (void)normal_interval_ns; (void)running; (void)tas_stepping;
    (void)host_now_ns; return true;
}
static inline int64_t xemu_fast_forward_timer_interval_ns(
    int64_t normal_interval_ns, bool running, bool tas_stepping)
{
    (void)running; (void)tas_stepping; return normal_interval_ns;
}
static inline int64_t xemu_fast_forward_thread_interval_ns(
    int64_t normal_interval_ns, bool running)
{
    (void)running; return normal_interval_ns;
}
static inline void xemu_fast_forward_advance_guest_clock(
    int64_t normal_interval_ns, bool running, bool tas_stepping)
{
    (void)normal_interval_ns; (void)running; (void)tas_stepping;
}
#endif

#ifdef __cplusplus
}
#endif
#endif
