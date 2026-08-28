/*
 * xemu guest memory access
 *
 * virt_to_phys / virt_dma_memory_read were private statics in xemu-xbe.c.
 * They are exactly the primitive a cheat engine needs, so they are lifted here
 * and given a write counterpart rather than duplicated.
 *
 * This replaces, wholesale, the external toolchain's mem.py (process_vm_readv,
 * /proc/pid/maps scanning, ReadProcessMemory, the PROCESSENTRY32 layout) and
 * pagemap.py (a manual walk of the Xbox page directory at physical 0xF000).
 * In-process, the CPU's own translation is authoritative and always current -
 * there is no cached map to go stale across a level load.
 *
 * Copyright (C) 2020-2021 Matt Borgerson  (original virt_to_phys)
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 */

#ifndef XEMU_GUESTMEM_H
#define XEMU_GUESTMEM_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Drop the page translation cache. Must be called at the start of any batch
 * of accesses (e.g. one cheat apply pass); the cache assumes guest paging is
 * stable for the duration of the batch.
 */
void xemu_guestmem_invalidate_cache(void);

/* Guest virtual -> guest physical. 0 on success. */
int xemu_virt_to_phys(uint64_t vaddr, uint64_t *phys_addr);

/* Read/write guest memory by PHYSICAL address. Return bytes moved, or -1. */
ssize_t xemu_phys_read(uint64_t phys, void *buf, size_t len);
ssize_t xemu_phys_write(uint64_t phys, const void *buf, size_t len);

/* Read/write guest memory by VIRTUAL address, translating page by page. */
ssize_t xemu_virt_read(uint64_t vaddr, void *buf, size_t len);
ssize_t xemu_virt_write(uint64_t vaddr, const void *buf, size_t len);

/*
 * Invalidate any translated code covering a physical range.
 *
 * This is the whole reason the external trainer needed a gdbstub client: a raw
 * write to code leaves TCG executing the block it already translated, so the
 * patch appears to do nothing. In-process the correct call is available
 * directly, which deletes the gdb client, the session broker, the halt/resume
 * ownership dance and the deferred write coalescing that existed only to
 * amortise socket round-trips.
 *
 * Call after writing to any address that might be executable. Cheap when
 * nothing is translated there.
 */
void xemu_invalidate_code_range(uint64_t phys, size_t len);

/* Size of guest RAM in bytes (64 or 128 MB). */
uint64_t xemu_guest_ram_size(void);

#ifdef __cplusplus
}
#endif

#endif
