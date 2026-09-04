// SPDX-License-Identifier: GPL-2.0-or-later
// xemu custom fork - feature-owned XDVDFS virtual disc overlay bridge
#pragma once

#include "config-host.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#ifdef CONFIG_XEMU_FEATURE_DISC_MODDING
/*
 * Notify the feature when the host disc image changes. The implementation
 * parses the XDVDFS metadata and prepares an immutable overlay snapshot.
 * Passing NULL or an empty path invalidates the current disc.
 */
void xemu_disc_overlay_notify_disc_path(const char *path);

/* Cancel/join host extraction before replacing or ejecting the medium. */
void xemu_disc_overlay_prepare_disc_change(void);

/* Refresh the logical reader from ide0-cd1. Direct form runs on QEMU thread;
 * scheduled form is safe for the UI thread after a QMP medium change. */
void xemu_disc_overlay_refresh_mounted_backend(void);
void xemu_disc_overlay_schedule_refresh(void);

/* Return the guest-visible 2048-byte sector count. */
uint64_t xemu_disc_overlay_total_sectors(uint64_t original_total_sectors);

/* True when every sector in [lba, lba + count) belongs to virtual host data. */
bool xemu_disc_overlay_is_virtual_range(uint64_t lba, uint32_t count);

/*
 * Fill an all-virtual range from host replacement files. Returns true only
 * when the whole request was handled. The final partial file sector is zero
 * padded. On a host read failure it still returns true and provides zeroes;
 * the error is retained for the UI rather than exposing stale backing bytes.
 */
bool xemu_disc_overlay_read_virtual(uint64_t lba, uint32_t count,
                                    uint8_t *buffer);

/* Patch XDVDFS directory metadata in a successfully read backing range. */
void xemu_disc_overlay_patch_read(uint64_t lba, uint32_t count,
                                  uint8_t *buffer);
#else
static inline void xemu_disc_overlay_notify_disc_path(const char *path)
{
    (void)path;
}
static inline void xemu_disc_overlay_prepare_disc_change(void) {}
static inline void xemu_disc_overlay_refresh_mounted_backend(void) {}
static inline void xemu_disc_overlay_schedule_refresh(void) {}
static inline uint64_t xemu_disc_overlay_total_sectors(
    uint64_t original_total_sectors)
{
    return original_total_sectors;
}
static inline bool xemu_disc_overlay_is_virtual_range(uint64_t lba,
                                                      uint32_t count)
{
    (void)lba;
    (void)count;
    return false;
}
static inline bool xemu_disc_overlay_read_virtual(uint64_t lba,
                                                  uint32_t count,
                                                  uint8_t *buffer)
{
    (void)lba;
    (void)count;
    (void)buffer;
    return false;
}
static inline void xemu_disc_overlay_patch_read(uint64_t lba, uint32_t count,
                                                uint8_t *buffer)
{
    (void)lba;
    (void)count;
    (void)buffer;
}
#endif

#ifdef __cplusplus
}
#endif
