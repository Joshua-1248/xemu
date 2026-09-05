// SPDX-License-Identifier: GPL-2.0-or-later
// Feature-owned C bridge between Disc Files & Mods C++ and QEMU block APIs.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct XemuMountedDiscBackend XemuMountedDiscBackend;
typedef bool (*XemuMountedDiscWaitPredicate)(void *opaque);
typedef void (*XemuMountedDiscBhFn)(void *opaque);

/* Pin the exact BlockDriverState currently mounted on @backend_name. */
XemuMountedDiscBackend *xemu_mounted_disc_pin(const char *backend_name,
                                               int64_t *length,
                                               char **error_message);

/* Release a pin created by xemu_mounted_disc_pin(). */
void xemu_mounted_disc_unref(XemuMountedDiscBackend *backend);

/* Free an error string returned by this bridge. */
void xemu_mounted_disc_free_error(char *error_message);

/* Synchronous read for callers already running in a valid QEMU I/O context. */
int xemu_mounted_disc_pread(XemuMountedDiscBackend *backend, int64_t offset,
                            int64_t bytes, void *buffer);

/*
 * Worker-thread-safe blocking read.  The actual BlockBackend I/O is marshalled
 * to the backend's home AioContext before blk_aio_preadv() is entered.
 */
int xemu_mounted_disc_pread_worker(XemuMountedDiscBackend *backend,
                                   int64_t offset, size_t bytes, void *buffer);

/* Wake/pump helpers used by extraction cancellation during QEMU shutdown. */
void xemu_mounted_disc_wait_kick(void);
void xemu_mounted_disc_pump_while_bql(XemuMountedDiscBackend *backend,
                                      XemuMountedDiscWaitPredicate predicate,
                                      void *opaque);

/* Schedule a feature callback on QEMU's main AioContext. */
void xemu_mounted_disc_schedule_main_bh(XemuMountedDiscBhFn callback,
                                        void *opaque);

#ifdef __cplusplus
}
#endif
