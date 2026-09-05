// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Read-only CHDv5 DVD block-format driver for xemu.
 *
 * This intentionally exposes only the CHD logical byte stream. Xbox/XDVDFS
 * interpretation remains in the normal xemu DVD stack above this driver.
 */
#include "qemu/osdep.h"
#include "block/block-io.h"
#include "block/block_int.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "qemu/iov.h"
#include "qemu/module.h"

#include <libchdr/chd.h>
#include <libchdr/coretypes.h>

#define XEMU_CHD_DVD_SECTOR_SIZE 2048u
/* Current chdman accepts hunks up to 1 MiB; reject larger values before
 * allocating our additional decode/cache buffers. */
#define XEMU_CHD_MAX_HUNK_SIZE (1024u * 1024u)
#define XEMU_CHD_CACHE_TARGET_BYTES (16u * 1024u * 1024u)
#define XEMU_CHD_CACHE_WAYS 4u
#define XEMU_CHD_INVALID_HUNK UINT32_MAX

typedef struct XemuChdIo {
    BdrvChild *child;
    int64_t size;
    int64_t pos;
    int last_error;
} XemuChdIo;

typedef struct XemuChdCacheSlot {
    uint32_t hunk;
    uint64_t stamp;
} XemuChdCacheSlot;

typedef struct BDRVXemuChdState {
    CoMutex lock;
    XemuChdIo io;
    chd_file *chd;
    uint8_t *cache_data;
    XemuChdCacheSlot *cache_slots;
    size_t cache_entries;
    size_t cache_sets;
    size_t cache_ways;
    size_t cache_hot_slot;
    uint64_t cache_clock;
    uint32_t hunkbytes;
    uint64_t logicalbytes;
} BDRVXemuChdState;

static uint64_t xemu_chd_io_size(void *opaque)
{
    XemuChdIo *io = opaque;
    return io->size >= 0 ? (uint64_t)io->size : UINT64_MAX;
}

static int xemu_chd_io_seek(void *opaque, int64_t offset, int whence)
{
    XemuChdIo *io = opaque;
    int64_t base;

    switch (whence) {
    case SEEK_SET:
        base = 0;
        break;
    case SEEK_CUR:
        base = io->pos;
        break;
    case SEEK_END:
        base = io->size;
        break;
    default:
        return -1;
    }

    if ((offset > 0 && base > INT64_MAX - offset) ||
        (offset < 0 && base < INT64_MIN - offset)) {
        return -1;
    }
    int64_t next = base + offset;
    if (next < 0) {
        return -1;
    }
    io->pos = next;
    return 0;
}

static size_t xemu_chd_io_read(void *ptr, size_t size, size_t nmemb,
                               void *opaque)
{
    XemuChdIo *io = opaque;
    size_t requested;
    int64_t available;
    int64_t bytes;
    int ret;

    if (!size || !nmemb) {
        return 0;
    }
    if (nmemb > SIZE_MAX / size) {
        io->last_error = -EOVERFLOW;
        return 0;
    }
    requested = size * nmemb;
    if (io->pos < 0 || io->pos >= io->size) {
        return 0;
    }
    available = io->size - io->pos;
    bytes = MIN((uint64_t)requested, (uint64_t)available);
    bytes -= bytes % size;
    if (!bytes) {
        return 0;
    }

    ret = bdrv_pread(io->child, io->pos, bytes, ptr, 0);
    if (ret < 0) {
        io->last_error = ret;
        return 0;
    }
    io->pos += bytes;
    return bytes / size;
}

static int xemu_chd_io_close(void *opaque)
{
    /* QEMU owns the BdrvChild. libchdr must not close/reopen the host file. */
    (void)opaque;
    return 0;
}

static const core_file_callbacks xemu_chd_io_callbacks = {
    .fsize = xemu_chd_io_size,
    .fread = xemu_chd_io_read,
    .fclose = xemu_chd_io_close,
    .fseek = xemu_chd_io_seek,
};

static bool xemu_chd_sha1_is_zero(const uint8_t sha1[CHD_SHA1_BYTES])
{
    uint8_t aggregate = 0;
    for (size_t i = 0; i < CHD_SHA1_BYTES; ++i) {
        aggregate |= sha1[i];
    }
    return aggregate == 0;
}

static void xemu_chd_cache_free(BDRVXemuChdState *s)
{
    g_free(s->cache_data);
    g_free(s->cache_slots);
    s->cache_data = NULL;
    s->cache_slots = NULL;
    s->cache_entries = 0;
    s->cache_sets = 0;
    s->cache_ways = 0;
    s->cache_hot_slot = SIZE_MAX;
    s->cache_clock = 0;
}

static int xemu_chd_cache_init(BDRVXemuChdState *s, uint32_t hunkbytes,
                               uint64_t logicalbytes, Error **errp)
{
    const uint64_t total_hunks =
        1 + (logicalbytes - 1) / (uint64_t)hunkbytes;
    size_t wanted = XEMU_CHD_CACHE_TARGET_BYTES / hunkbytes;

    if (wanted == 0) {
        wanted = 1;
    }
    if (total_hunks < wanted) {
        wanted = (size_t)total_hunks;
    }

    /*
     * Use a small fixed-associativity cache rather than a single decoded hunk.
     * Xbox titles commonly revisit nearby filesystem/streaming blocks; keeping
     * those hunks avoids repeating both compressed backing I/O and codec work.
     *
     * Allocation is deliberately best-effort.  If the preferred 16 MiB cache
     * cannot be allocated, halve it until the old one-hunk behavior fits.
     * A memory-constrained host therefore never loses compatibility merely
     * because the optimization exists.
     */
    for (size_t capacity = wanted; capacity != 0; capacity /= 2) {
        size_t ways = MIN(capacity, (size_t)XEMU_CHD_CACHE_WAYS);
        size_t sets = capacity / ways;
        size_t entries = sets * ways;
        uint8_t *data = g_try_malloc_n(entries, hunkbytes);
        XemuChdCacheSlot *slots = g_try_new(XemuChdCacheSlot, entries);

        if (data && slots) {
            for (size_t i = 0; i < entries; ++i) {
                slots[i].hunk = XEMU_CHD_INVALID_HUNK;
                slots[i].stamp = 0;
            }
            s->cache_data = data;
            s->cache_slots = slots;
            s->cache_entries = entries;
            s->cache_sets = sets;
            s->cache_ways = ways;
            s->cache_hot_slot = SIZE_MAX;
            s->cache_clock = 0;
            return 0;
        }
        g_free(data);
        g_free(slots);

        if (capacity == 1) {
            break;
        }
    }

    error_setg(errp, "Could not allocate a CHD decode buffer");
    return -ENOMEM;
}

static uint64_t xemu_chd_cache_next_stamp(BDRVXemuChdState *s)
{
    ++s->cache_clock;
    if (unlikely(s->cache_clock == 0)) {
        /* Practically unreachable, but keep LRU ordering valid after wrap. */
        for (size_t i = 0; i < s->cache_entries; ++i) {
            s->cache_slots[i].stamp = 0;
        }
        s->cache_clock = 1;
    }
    return s->cache_clock;
}

static int xemu_chd_get_hunk(BDRVXemuChdState *s, uint32_t hunk,
                             const uint8_t **data)
{
    const uint64_t stamp = xemu_chd_cache_next_stamp(s);

    /* Default DVD hunks hold two 2048-byte sectors, so consecutive sector
     * requests frequently hit the same decoded hunk.  Keep that case to one
     * tag comparison before doing the set lookup. */
    if (s->cache_hot_slot < s->cache_entries) {
        XemuChdCacheSlot *hot = &s->cache_slots[s->cache_hot_slot];
        if (hot->hunk == hunk) {
            hot->stamp = stamp;
            *data = s->cache_data + s->cache_hot_slot * s->hunkbytes;
            return 0;
        }
    }

    const size_t set = hunk % s->cache_sets;
    const size_t base = set * s->cache_ways;
    size_t victim = base;
    uint64_t oldest = UINT64_MAX;

    for (size_t way = 0; way < s->cache_ways; ++way) {
        const size_t index = base + way;
        XemuChdCacheSlot *slot = &s->cache_slots[index];

        if (slot->hunk == hunk) {
            slot->stamp = stamp;
            s->cache_hot_slot = index;
            *data = s->cache_data + index * s->hunkbytes;
            return 0;
        }
        if (slot->hunk == XEMU_CHD_INVALID_HUNK) {
            victim = index;
            oldest = 0;
            break;
        }
        if (slot->stamp < oldest) {
            oldest = slot->stamp;
            victim = index;
        }
    }

    XemuChdCacheSlot *slot = &s->cache_slots[victim];
    uint8_t *dst = s->cache_data + victim * s->hunkbytes;
    chd_error err;

    /* A failed decode may have overwritten dst; never leave the old tag live. */
    slot->hunk = XEMU_CHD_INVALID_HUNK;
    slot->stamp = 0;
    s->io.last_error = 0;
    err = chd_read(s->chd, hunk, dst);
    if (err != CHDERR_NONE) {
        return s->io.last_error < 0 ? s->io.last_error : -EIO;
    }
    slot->hunk = hunk;
    slot->stamp = stamp;
    s->cache_hot_slot = victim;
    *data = dst;
    return 0;
}

static int xemu_chd_probe(const uint8_t *buf, int buf_size,
                          const char *filename)
{
    (void)filename;
    return buf_size >= 8 && !memcmp(buf, "MComprHD", 8) ? 100 : 0;
}

static int xemu_chd_open(BlockDriverState *bs, QDict *options, int flags,
                         Error **errp)
{
    BDRVXemuChdState *s = bs->opaque;
    chd_error chd_err;
    const chd_header *header;
    uint8_t dvd_metadata[64];
    uint32_t metadata_length = 0;
    int ret;

    GLOBAL_STATE_CODE();
    (void)flags;

    bdrv_graph_rdlock_main_loop();
    ret = bdrv_apply_auto_read_only(bs, NULL, errp);
    bdrv_graph_rdunlock_main_loop();
    if (ret < 0) {
        return ret;
    }

    ret = bdrv_open_file_child(NULL, options, "file", bs, errp);
    if (ret < 0) {
        return ret;
    }

    GRAPH_RDLOCK_GUARD_MAINLOOP();

    s->io.child = bs->file;
    s->io.size = bdrv_getlength(bs->file->bs);
    s->io.pos = 0;
    s->io.last_error = 0;
    if (s->io.size <= 0) {
        error_setg(errp, "CHD backing file has an invalid size");
        return s->io.size < 0 ? s->io.size : -EINVAL;
    }

    chd_err = chd_open_core_file_callbacks(&xemu_chd_io_callbacks, &s->io,
                                           CHD_OPEN_READ, NULL, &s->chd);
    if (chd_err != CHDERR_NONE) {
        if (chd_err == CHDERR_REQUIRES_PARENT) {
            error_setg(errp, "Parent/delta CHDs are not supported yet; use a standalone DVD CHD");
        } else {
            error_setg(errp, "Could not open CHD: %s", chd_error_string(chd_err));
        }
        return s->io.last_error < 0 ? s->io.last_error : -EINVAL;
    }

    header = chd_get_header(s->chd);
    if (!header || header->version != CHD_HEADER_VERSION) {
        error_setg(errp, "Unsupported CHD version; Xbox DVD CHDs must be CHDv5");
        ret = -ENOTSUP;
        goto fail;
    }
    if (!header->logicalbytes || header->logicalbytes > INT64_MAX ||
        (header->logicalbytes % XEMU_CHD_DVD_SECTOR_SIZE) != 0) {
        error_setg(errp, "Invalid DVD CHD logical size (must be a non-zero multiple of 2048 bytes)");
        ret = -EINVAL;
        goto fail;
    }
    if (header->unitbytes != XEMU_CHD_DVD_SECTOR_SIZE) {
        error_setg(errp, "Not an Xbox-compatible DVD CHD: unit size is %u, expected 2048",
                   header->unitbytes);
        ret = -EINVAL;
        goto fail;
    }
    if (header->hunkbytes < XEMU_CHD_DVD_SECTOR_SIZE ||
        header->hunkbytes > XEMU_CHD_MAX_HUNK_SIZE ||
        (header->hunkbytes % XEMU_CHD_DVD_SECTOR_SIZE) != 0) {
        error_setg(errp,
                   "Invalid DVD CHD hunk size %u (expected a 2048-byte multiple up to 1 MiB)",
                   header->hunkbytes);
        ret = -EINVAL;
        goto fail;
    }
    if (!xemu_chd_sha1_is_zero(header->parentsha1)) {
        error_setg(errp, "Parent/delta CHDs are not supported yet; use a standalone DVD CHD");
        ret = -ENOTSUP;
        goto fail;
    }

    chd_err = chd_get_metadata(s->chd, DVD_METADATA_TAG, 0,
                               dvd_metadata, sizeof(dvd_metadata),
                               &metadata_length, NULL, NULL);
    if (chd_err != CHDERR_NONE) {
        error_setg(errp,
                   "This CHD is not tagged as DVD media. Convert the Xbox ISO/XISO with 'chdman createdvd'.");
        ret = -EINVAL;
        goto fail;
    }

    ret = xemu_chd_cache_init(s, header->hunkbytes, header->logicalbytes,
                              errp);
    if (ret < 0) {
        goto fail;
    }
    s->hunkbytes = header->hunkbytes;
    s->logicalbytes = header->logicalbytes;
    qemu_co_mutex_init(&s->lock);

    bs->total_sectors = header->logicalbytes >> BDRV_SECTOR_BITS;
    return 0;

fail:
    xemu_chd_cache_free(s);
    chd_close(s->chd);
    s->chd = NULL;
    return ret;
}

static void xemu_chd_close(BlockDriverState *bs)
{
    BDRVXemuChdState *s = bs->opaque;
    if (s->chd) {
        chd_close(s->chd);
        s->chd = NULL;
    }
    xemu_chd_cache_free(s);
}

static int coroutine_fn GRAPH_RDLOCK
xemu_chd_co_preadv(BlockDriverState *bs, int64_t offset, int64_t bytes,
                   QEMUIOVector *qiov, BdrvRequestFlags flags)
{
    BDRVXemuChdState *s = bs->opaque;
    uint64_t pos;
    uint64_t remaining;
    size_t qiov_offset = 0;
    int ret = 0;

    (void)flags;
    if (offset < 0 || bytes < 0 ||
        (uint64_t)offset > s->logicalbytes ||
        (uint64_t)bytes > s->logicalbytes - (uint64_t)offset) {
        return -EIO;
    }

    pos = offset;
    remaining = bytes;
    qemu_co_mutex_lock(&s->lock);
    while (remaining) {
        uint64_t hunk64 = pos / s->hunkbytes;
        uint32_t hunk_offset = pos % s->hunkbytes;
        size_t take = MIN(remaining, (uint64_t)s->hunkbytes - hunk_offset);
        const uint8_t *hunk_data;

        if (hunk64 > UINT32_MAX) {
            ret = -EIO;
            break;
        }
        ret = xemu_chd_get_hunk(s, (uint32_t)hunk64, &hunk_data);
        if (ret < 0) {
            break;
        }
        qemu_iovec_from_buf(qiov, qiov_offset,
                           hunk_data + hunk_offset, take);
        pos += take;
        remaining -= take;
        qiov_offset += take;
    }
    qemu_co_mutex_unlock(&s->lock);
    return ret;
}

static void xemu_chd_refresh_limits(BlockDriverState *bs, Error **errp)
{
    (void)errp;
    bs->bl.request_alignment = BDRV_SECTOR_SIZE;
}

static BlockDriver bdrv_xemu_chd = {
    .format_name = "chd",
    .instance_size = sizeof(BDRVXemuChdState),
    .bdrv_probe = xemu_chd_probe,
    .bdrv_open = xemu_chd_open,
    .bdrv_child_perm = bdrv_default_perms,
    .bdrv_refresh_limits = xemu_chd_refresh_limits,
    .bdrv_co_preadv = xemu_chd_co_preadv,
    .bdrv_close = xemu_chd_close,
    .is_format = true,
};

static void xemu_chd_init(void)
{
    bdrv_register(&bdrv_xemu_chd);
}

block_init(xemu_chd_init);
