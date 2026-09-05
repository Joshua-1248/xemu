// SPDX-License-Identifier: GPL-2.0-or-later
// Feature-owned C bridge between Disc Files & Mods C++ and QEMU block APIs.

#include "qemu/osdep.h"
#include "xemu-features/disc-modding/mounted-disc-bridge.h"

#include "block/aio.h"
#include "block/aio-wait.h"
#include "qapi/error.h"
#include "qemu/iov.h"
#include "qemu/main-loop.h"
#include "qemu/thread.h"
#include "system/block-backend-global-state.h"
#include "system/block-backend-io.h"

struct XemuMountedDiscBackend {
    BlockBackend *blk;
};

typedef struct XemuMountedDiscReadWait {
    XemuMountedDiscBackend *backend;
    int64_t offset;
    QEMUIOVector qiov;
    QemuSemaphore done;
    int result;
} XemuMountedDiscReadWait;

static void xemu_mounted_disc_read_done(void *opaque, int ret)
{
    XemuMountedDiscReadWait *wait = opaque;

    blk_dec_in_flight(wait->backend->blk);
    wait->result = ret;
    qemu_sem_post(&wait->done);
}

static void xemu_mounted_disc_read_start(void *opaque)
{
    XemuMountedDiscReadWait *wait = opaque;
    BlockBackend *blk = wait->backend->blk;

    assert(qemu_get_current_aio_context() == blk_get_aio_context(blk));
    blk_aio_preadv(blk, wait->offset, &wait->qiov, (BdrvRequestFlags)0,
                   xemu_mounted_disc_read_done, wait);
}

XemuMountedDiscBackend *xemu_mounted_disc_pin(const char *backend_name,
                                               int64_t *length,
                                               char **error_message)
{
    XemuMountedDiscBackend *result = NULL;
    BlockBackend *mounted;
    BlockBackend *reader;
    Error *local_err = NULL;

    GLOBAL_STATE_CODE();

    if (length) {
        *length = -1;
    }
    if (error_message) {
        *error_message = NULL;
    }

    mounted = backend_name ? blk_by_name(backend_name) : NULL;
    if (!mounted || !blk_bs(mounted)) {
        if (error_message) {
            *error_message = g_strdup("mounted block backend is unavailable");
        }
        return NULL;
    }

    reader = blk_new_with_bs(blk_bs(mounted), BLK_PERM_CONSISTENT_READ,
                             BLK_PERM_ALL, &local_err);
    if (!reader) {
        if (error_message) {
            *error_message = g_strdup(local_err ? error_get_pretty(local_err)
                                                : "cannot pin mounted block backend");
        }
        error_free(local_err);
        return NULL;
    }

    /* Follow the source BDS if QEMU ever moves it to another IOThread. */
    blk_set_allow_aio_context_change(reader, true);

    result = g_new0(XemuMountedDiscBackend, 1);
    result->blk = reader;
    if (length) {
        *length = blk_getlength(reader);
    }
    return result;
}

void xemu_mounted_disc_unref(XemuMountedDiscBackend *backend)
{
    if (!backend) {
        return;
    }
    GLOBAL_STATE_CODE();
    if (backend->blk) {
        blk_unref(backend->blk);
    }
    g_free(backend);
}

void xemu_mounted_disc_free_error(char *error_message)
{
    g_free(error_message);
}

int xemu_mounted_disc_pread(XemuMountedDiscBackend *backend, int64_t offset,
                            int64_t bytes, void *buffer)
{
    if (!backend || !backend->blk) {
        return -ENOMEDIUM;
    }
    return blk_pread(backend->blk, offset, bytes, buffer,
                     (BdrvRequestFlags)0);
}

int xemu_mounted_disc_pread_worker(XemuMountedDiscBackend *backend,
                                   int64_t offset, size_t bytes, void *buffer)
{
    XemuMountedDiscReadWait wait = { 0 };
    AioContext *ctx;

    if (!backend || !backend->blk || offset < 0 || bytes > INT64_MAX) {
        return -EINVAL;
    }
    if (bytes == 0) {
        return 0;
    }

    wait.backend = backend;
    wait.offset = offset;
    wait.result = -EIO;
    qemu_iovec_init_buf(&wait.qiov, buffer, bytes);
    qemu_sem_init(&wait.done, 0);

    /*
     * Pin the backend's AioContext across the worker->BH scheduling gap.
     * blk_aio_preadv() takes its own in-flight reference once started.
     */
    blk_inc_in_flight(backend->blk);
    ctx = blk_get_aio_context(backend->blk);
    aio_bh_schedule_oneshot(ctx, xemu_mounted_disc_read_start, &wait);

    qemu_sem_wait(&wait.done);
    qemu_sem_destroy(&wait.done);
    return wait.result;
}

void xemu_mounted_disc_wait_kick(void)
{
    aio_wait_kick();
}

void xemu_mounted_disc_pump_while_bql(XemuMountedDiscBackend *backend,
                                      XemuMountedDiscWaitPredicate predicate,
                                      void *opaque)
{
    if (!backend || !backend->blk || !predicate || !bql_locked()) {
        return;
    }

    AioContext *ctx = blk_get_aio_context(backend->blk);
    AIO_WAIT_WHILE(ctx, predicate(opaque));
}

void xemu_mounted_disc_schedule_main_bh(XemuMountedDiscBhFn callback,
                                        void *opaque)
{
    if (!callback) {
        return;
    }
    aio_bh_schedule_oneshot(qemu_get_aio_context(), callback, opaque);
}
