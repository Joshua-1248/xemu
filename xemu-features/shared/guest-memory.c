/*
 * xemu guest memory access
 *
 * virt_to_phys and virt_dma_memory_read were private statics in xemu-xbe.c.
 * They are exactly the primitive a cheat engine needs, so they are lifted here
 * and given a write counterpart rather than duplicated.
 *
 * This replaces, wholesale, the external toolchain's mem.py (process_vm_readv,
 * /proc/pid/maps scanning, ReadProcessMemory, the PROCESSENTRY32 layout) and
 * pagemap.py (a manual walk of the Xbox page directory at physical 0xF000).
 * In-process the CPU's own translation is authoritative and always current, so
 * there is no cached map to go stale across a level load.
 *
 * The include set below is copied from xemu-xbe.c deliberately: that file
 * already calls dma_memory_read() and references address_space_memory with
 * exactly these headers and compiles in this tree, so mirroring it avoids
 * guessing which of QEMU's many headers actually declares what.
 *
 * Copyright (C) 2020-2021 Matt Borgerson  (original virt_to_phys)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/i386/pc.h"
#include "hw/pci/pci.h"
#include "hw/boards.h"
#include "system/hw_accel.h"
#include "cpu.h"
#include "exec/target_page.h"
#include "xemu-features/shared/guest-memory.h"

#ifdef CONFIG_TCG
#include "exec/tb-flush.h"
#endif

/*
 * Page translation cache.
 *
 * cpu_get_phys_page_attrs_debug() walks the guest page tables in software,
 * and cpu_synchronize_state() is called alongside it. Doing both on every
 * byte accessed is what made applying codes expensive: a patch with a few
 * hundred lines translates a few hundred times per pass, and at a 16 ms
 * interval that is tens of thousands of page walks a second.
 *
 * Within a single apply pass the guest is not executing, so its paging cannot
 * change and the mapping is stable. The cache is therefore reset once per
 * pass by xemu_guestmem_invalidate_cache() rather than being kept live.
 * Direct-mapped and tiny; the access pattern of a cheat list is a handful of
 * pages hit repeatedly, which is exactly what this suits.
 */
#define XLATE_CACHE_ENTRIES 256

static struct {
    uint64_t va_page;
    uint64_t pa_page;
    uint32_t generation;
} xlate_cache[XLATE_CACHE_ENTRIES];

static uint32_t xlate_generation = 1;
static bool xlate_synced;

void xemu_guestmem_invalidate_cache(void)
{
    /* O(1) invalidation: this is called once per cheat/TAS memory batch and
     * can be extremely hot in Instantaneous mode. */
    ++xlate_generation;
    if (xlate_generation == 0) {
        memset(xlate_cache, 0, sizeof(xlate_cache));
        xlate_generation = 1;
    }
    xlate_synced = false;
}

int xemu_virt_to_phys(uint64_t vaddr_in, uint64_t *phys_addr)
{
    MemTxAttrs attrs;
    CPUState *cs;
    hwaddr gpa;
    uint64_t va_page = vaddr_in & TARGET_PAGE_MASK;
    unsigned slot = (unsigned)((va_page >> 12) & (XLATE_CACHE_ENTRIES - 1));

    if (xlate_cache[slot].generation == xlate_generation &&
        xlate_cache[slot].va_page == va_page) {
        *phys_addr = xlate_cache[slot].pa_page + (vaddr_in & ~TARGET_PAGE_MASK);
        return 0;
    }

    cs = qemu_get_cpu(0);
    if (cs == NULL) {
        return 1; /* No cpu */
    }

    /* Once per pass, not once per translation. */
    if (!xlate_synced) {
        cpu_synchronize_state(cs);
        xlate_synced = true;
    }

    gpa = cpu_get_phys_page_attrs_debug(cs, (vaddr)vaddr_in & TARGET_PAGE_MASK,
                                        &attrs);
    if (gpa == (hwaddr)-1) {
        return 1; /* Unmapped */
    }

    xlate_cache[slot].va_page = va_page;
    xlate_cache[slot].pa_page = gpa;
    xlate_cache[slot].generation = xlate_generation;

    *phys_addr = gpa + (vaddr_in & ~TARGET_PAGE_MASK);
    return 0;
}

uint64_t xemu_guest_ram_size(void)
{
    /* RAM size is immutable for the lifetime of an Xbox machine. Cache it so
     * every tiny cheat/script memory access does not walk QOM just to learn
     * whether this is a 64 MiB retail or 128 MiB devkit configuration. */
    static uint64_t cached_ram_size;
    if (cached_ram_size != 0) {
        return cached_ram_size;
    }

    MachineState *ms = MACHINE(qdev_get_machine());
    if (ms == NULL || ms->ram_size == 0) {
        return 64 * 1024 * 1024;
    }
    cached_ram_size = ms->ram_size;
    return cached_ram_size;
}

ssize_t xemu_phys_read(uint64_t phys, void *buf, size_t len)
{
    if (len == 0) {
        return 0;
    }
    const uint64_t ram_size = xemu_guest_ram_size();
    if (phys >= ram_size || len > ram_size - phys) {
        return -1;
    }
    if (dma_memory_read(&address_space_memory, phys, buf, len,
                        MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return -1;
    }
    return (ssize_t)len;
}

ssize_t xemu_phys_write(uint64_t phys, const void *buf, size_t len)
{
    uint8_t current[64];

    if (len == 0) {
        return 0;
    }
    const uint64_t ram_size = xemu_guest_ram_size();
    if (phys >= ram_size || len > ram_size - phys) {
        return -1;
    }

    /*
     * Skip the write entirely when the bytes are already what we want.
     *
     * This is a correctness-neutral optimisation with an enormous effect. The
     * cheat engine re-applies every enabled cheat once per frame, and almost
     * every one of those writes is storing a value that is already there
     * (that is the whole point of a "freeze"). Without this check each of
     * them still counted as a write, and each write queued a full TB flush -
     * so a single enabled cheat threw away every translated block 60 times a
     * second and the guest spent its life retranslating. That is the stutter.
     *
     * Bounded by the on-stack buffer; longer writes (only type 5 block
     * copies) skip the check and always write, which is fine because they are
     * rare.
     */
    if (len <= sizeof(current) &&
        dma_memory_read(&address_space_memory, phys, current, len,
                        MEMTXATTRS_UNSPECIFIED) == MEMTX_OK &&
        memcmp(current, buf, len) == 0) {
        return (ssize_t)len;
    }

    if (dma_memory_write(&address_space_memory, phys, buf, len,
                         MEMTXATTRS_UNSPECIFIED) != MEMTX_OK) {
        return -1;
    }

    /*
     * Deliberately NO explicit code invalidation here.
     *
     * dma_memory_write() already routes through invalidate_and_set_dirty()
     * (system/physmem.c), which calls tb_invalidate_phys_range() itself - but
     * only for the written range, and only when that range is actually marked
     * DIRTY_MEMORY_CODE, i.e. only when translated code really lives there.
     *
     * The manual queue_tb_flush() that used to be here was therefore both
     * redundant and far too broad: it threw away EVERY translated block in
     * the machine on any write whose value changed. That is why performance
     * collapsed the moment a cheat wrote a moving value - a moon jump writing
     * a fresh position each frame, or an infinite-health code firing whenever
     * damage was taken - while a cheat writing a constant cost nothing.
     *
     * Data writes to ordinary RAM now cost nothing extra, and a write that
     * genuinely patches code is still invalidated correctly by the memory
     * subsystem.
     */
    return (ssize_t)len;
}

/*
 * Page by page, because a virtual range that looks contiguous need not be
 * contiguous physically. The external tooling got this wrong once by
 * translating only the first page of a multi-page read.
 */
static ssize_t virt_rw(uint64_t vaddr_in, void *buf, size_t len, bool write)
{
    size_t done = 0;

    while (done < len) {
        uint64_t phys = 0;
        size_t left_in_page, chunk;
        ssize_t r;

        if (xemu_virt_to_phys(vaddr_in + done, &phys) != 0) {
            return done ? (ssize_t)done : -1;
        }

        left_in_page = TARGET_PAGE_SIZE - (size_t)(phys & ~TARGET_PAGE_MASK);
        chunk = MIN(len - done, left_in_page);

        r = write ? xemu_phys_write(phys, (const uint8_t *)buf + done, chunk)
                  : xemu_phys_read(phys, (uint8_t *)buf + done, chunk);
        if (r < 0) {
            return done ? (ssize_t)done : -1;
        }
        done += chunk;
    }

    return (ssize_t)done;
}

ssize_t xemu_virt_read(uint64_t vaddr_in, void *buf, size_t len)
{
    return virt_rw(vaddr_in, buf, len, false);
}

ssize_t xemu_virt_write(uint64_t vaddr_in, const void *buf, size_t len)
{
    return virt_rw(vaddr_in, (void *)buf, len, true);
}

void xemu_invalidate_code_range(uint64_t phys, size_t len)
{
#ifdef CONFIG_TCG
    CPUState *cs;

    /* The flush is whole-cache, so the range itself is not used. Kept in the
     * signature because a future ranged invalidate would need it. */
    (void)phys;

    if (len == 0 || !tcg_enabled()) {
        return;
    }
    cs = qemu_get_cpu(0);
    if (cs == NULL) {
        return;
    }

    /*
     * Only reachable if a caller asks for it explicitly; the normal write
     * path no longer does, because dma_memory_write() already invalidates
     * correctly and narrowly. Kept for a caller that bypasses the memory API.
     *
     * queue_tb_flush(), NOT tb_invalidate_phys_range().
     *
     * We are called from the UI thread while the vCPU thread may be executing
     * translated code. tb_invalidate_phys_range() has to run with the page
     * locks held and no concurrent execution; calling it from here tripped
     *
     *   tb-maint.c: do_tb_phys_invalidate: assertion failed (existing == NULL)
     *
     * because a TB was being invalidated underneath a thread still using it.
     *
     * queue_tb_flush() hands the work to async_safe_run_on_cpu(), which runs
     * it with every vCPU halted - the exact guarantee the direct call was
     * missing. It is heavier (it drops every translation, not just the range
     * written), but cheat writes are rare and correctness beats saving a few
     * retranslations.
     */
    queue_tb_flush(cs);
#else
    (void)phys;
    (void)len;
#endif
}
