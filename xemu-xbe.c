/*
 * xemu XBE accessing
 *
 * Helper functions to get details about the currently running executable.
 *
 * Copyright (C) 2020-2021 Matt Borgerson
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "xemu-xbe.h"
#include "qemu/osdep.h"
#include "hw/hw.h"
#include "hw/i386/pc.h"
#include "hw/pci/pci.h"
#include "system/hw_accel.h"
#include "cpu.h"
#include "exec/target_page.h"

static int virt_to_phys_synced(CPUState *cs, vaddr vaddr, hwaddr *phys_addr)
{
    MemTxAttrs attrs;
    hwaddr gpa = cpu_get_phys_page_attrs_debug(
        cs, vaddr & TARGET_PAGE_MASK, &attrs);

    if (gpa == -1) {
        return 1; // Unmapped
    }
    *phys_addr = gpa + (vaddr & ~TARGET_PAGE_MASK);
    return 0;
}

static ssize_t virt_dma_memory_read_synced(CPUState *cs, vaddr vaddr,
                                           void *buf, size_t len)
{
    size_t num_bytes_read = 0;

    while (num_bytes_read < len) {
        // Get physical page for this offset
        hwaddr phys_addr = 0;
        if (virt_to_phys_synced(cs, vaddr + num_bytes_read, &phys_addr) != 0) {
            return -1;
        }

        // Read contents from the page
        size_t bytes_remaining_in_page =
            TARGET_PAGE_SIZE - (phys_addr & ~TARGET_PAGE_MASK);
        size_t num_bytes_to_read =
            MIN(len - num_bytes_read, bytes_remaining_in_page);

        // FIXME: Check return value
        dma_memory_read(&address_space_memory,
                        phys_addr,
                        buf + num_bytes_read,
                        num_bytes_to_read,
                        MEMTXATTRS_UNSPECIFIED);

        num_bytes_read += num_bytes_to_read;
    }

    return num_bytes_read;
}

bool xemu_get_xbe_title_id(uint32_t *title_id)
{
    const vaddr hdr_addr_virt = 0x10000;
    CPUState *cs = qemu_get_cpu(0);
    hwaddr hdr_addr_phys = 0;

    if (title_id == NULL || cs == NULL) {
        return false;
    }
    cpu_synchronize_state(cs);
    if (virt_to_phys_synced(cs, hdr_addr_virt, &hdr_addr_phys) != 0) {
        return false;
    }

    /* All of these fixed header fields live in the first XBE header page. */
    if (ldl_le_phys(&address_space_memory, hdr_addr_phys) != 0x48454258) {
        return false;
    }

    uint32_t headers_len = ldl_le_phys(
        &address_space_memory,
        hdr_addr_phys + offsetof(struct xbe_header, m_sizeof_headers));
    if (headers_len < sizeof(struct xbe_header) ||
        headers_len > 8 * TARGET_PAGE_SIZE) {
        return false;
    }

    vaddr cert_addr_virt = ldl_le_phys(
        &address_space_memory,
        hdr_addr_phys + offsetof(struct xbe_header, m_certificate_addr));
    if (cert_addr_virt < hdr_addr_virt ||
        cert_addr_virt + sizeof(struct xbe_certificate) < cert_addr_virt ||
        cert_addr_virt + sizeof(struct xbe_certificate) >
            hdr_addr_virt + headers_len) {
        return false;
    }

    uint32_t tid_le = 0;
    const vaddr tid_virt =
        cert_addr_virt + offsetof(struct xbe_certificate, m_titleid);
    if (virt_dma_memory_read_synced(cs, tid_virt, &tid_le, sizeof(tid_le)) !=
        sizeof(tid_le)) {
        return false;
    }

    *title_id = le32_to_cpu(tid_le);
    return true;
}

struct xbe *xemu_get_xbe_info(void)
{
    vaddr hdr_addr_virt = 0x10000;
    CPUState *cs = qemu_get_cpu(0);

    static struct xbe xbe = {0};
    static size_t headers_capacity;

    if (!cs) {
        return NULL;
    }
    cpu_synchronize_state(cs);

    // Get physical page of headers
    hwaddr hdr_addr_phys = 0;
    if (virt_to_phys_synced(cs, hdr_addr_virt, &hdr_addr_phys) != 0) {
        return NULL;
    }

    // Check `XBEH` signature
    uint32_t sig = ldl_le_phys(&address_space_memory, hdr_addr_phys);
    if (sig != 0x48454258) {
        return NULL;
    }

    // Determine full length of headers
    xbe.headers_len = ldl_le_phys(&address_space_memory,
        hdr_addr_phys + offsetof(struct xbe_header, m_sizeof_headers));
    if (xbe.headers_len < sizeof(struct xbe_header) ||
        xbe.headers_len > 8 * TARGET_PAGE_SIZE) {
        // Invalid or unusually large header block.
        return NULL;
    }

    /* XBE header sizes are normally stable for the lifetime of a title. Reuse
     * the buffer across calls instead of free()/malloc() churn in UI features
     * that occasionally need the full certificate/title string. */
    if (xbe.headers_len > headers_capacity) {
        void *new_headers = realloc(xbe.headers, xbe.headers_len);
        assert(new_headers != NULL);
        xbe.headers = new_headers;
        headers_capacity = xbe.headers_len;
    }

    // Read all XBE headers
    ssize_t bytes_read = virt_dma_memory_read_synced(cs, hdr_addr_virt,
                                                     xbe.headers,
                                                     xbe.headers_len);
    if (bytes_read != xbe.headers_len) {
        // Failed to read headers
        return NULL;
    }

    // Extract XBE header fields
    xbe.header = (struct xbe_header *)xbe.headers;

    // Get certificate
    vaddr cert_addr_virt = ldl_le_p(&xbe.header->m_certificate_addr);
    const vaddr header_end = hdr_addr_virt + xbe.headers_len;
    if (cert_addr_virt < hdr_addr_virt ||
        cert_addr_virt > header_end ||
        sizeof(struct xbe_certificate) > header_end - cert_addr_virt) {
        // Invalid certificate header (a valid certificate is expected for official titles)
        return NULL;
    }
    xbe.cert = (struct xbe_certificate *)(xbe.headers + cert_addr_virt - hdr_addr_virt);

    return &xbe;
}
