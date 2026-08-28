// SPDX-License-Identifier: GPL-2.0-or-later
//
// Copyright (c) 2026 Joshua-1248
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
// GNU General Public License for more details.
//
// codes-engine.cc - Original Xbox cheat code interpreter.
//
// Direct port of xemu_trainer_lib/codes.py's execute_block() and helpers,
// intended to drop into xemu as ui/xui/codes-engine.cc. Kept free of ImGui and
// of QEMU headers so it can be built and differentially tested on the host
// against the Python reference before it is wired to guest memory.
//
// The memory backend is an interface with two implementations: FlatMemory for
// the test harness, and (later, in-tree) one forwarding to dma_memory_read /
// dma_memory_write via cpu_get_phys_page_attrs_debug. The interpreter itself
// never learns which it is talking to.
//
// Behaviour is specified by CODE_TYPES.txt. Where that file and codes.py
// disagree, codes.py wins - it says so itself, and it is what shipped.

#include "codes-engine.hh"
#include <cstring>
#include <algorithm>

namespace xcodes {

// ---------------------------------------------------------------------------
// PageMap - guest virtual -> physical, walking the live tables
// ---------------------------------------------------------------------------
// Mirrors XboxPageMap.on_demand(). NOT the full-map constructor: building a
// 1M-entry table read all of RAM and took seconds in Python. Walking costs two
// 4-byte reads per lookup and is always current, which is what makes the
// staleness problem tractable at all.

bool PageMap::rd32_phys(uint32_t pa, uint32_t *out) const
{
    uint8_t buf[4];
    if (!m_mem->Read(pa, buf, 4)) return false;
    *out = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
           ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
    return true;
}

void PageMap::Invalidate()
{
    /*
     * Generation invalidation makes the once-per-apply reset O(1). A full
     * unordered_map::clear() used to walk thousands of nodes and could free
     * memory on the hottest Instantaneous-cheat path.
     */
    ++m_generation;
    if (m_generation == 0) {
        for (auto &entry : m_cache) {
            entry.generation = 0;
        }
        m_generation = 1;
    }
}

bool PageMap::ToPhys(uint32_t va, uint32_t *out) const
{
    const uint32_t page = va >> 12;
    CacheEntry &entry = m_cache[page & (CACHE_ENTRIES - 1)];
    if (entry.generation == m_generation && entry.page == page) {
        *out = entry.base + (va & 0xFFF);
        return true;
    }

    uint32_t pde;
    if (!rd32_phys(PD_PHYS + ((va >> 22) * 4), &pde)) return false;
    if (!(pde & 1)) return false;

    uint32_t base;
    if (pde & 0x80) {                       // 4 MB large page
        base = (pde & 0xFFC00000) | (va & 0x003FF000);
    } else {
        uint32_t pt = pde & 0xFFFFF000;
        uint32_t pte;
        if (!rd32_phys(pt + (((va >> 12) & 0x3FF) * 4), &pte)) return false;
        if (!(pte & 1)) return false;
        base = pte & 0xFFFFF000;
    }
    if (base >= m_ram_size) return false;

    /* Direct-mapped, allocation-free cache. Collisions only cost another
     * page-table walk; they can never change the translated result. */
    entry.page = page;
    entry.base = base;
    entry.generation = m_generation;
    *out = base + (va & 0xFFF);
    return true;
}

bool PageMap::Valid() const
{
    uint32_t dummy;
    return ToPhys(0x00010000, &dummy);
}

// ---------------------------------------------------------------------------
// Engine - primitive reads and writes
// ---------------------------------------------------------------------------
// Every read returns success separately from the value. The Python code uses
// None for "could not read", and several handlers branch on it - collapsing
// that into a sentinel value would silently turn an unreadable address into a
// real comparison.

bool Engine::R8(uint32_t off, uint8_t *out)
{
    return m_mem->Read(off, out, 1);
}

bool Engine::R16(uint32_t off, uint16_t *out)
{
    uint8_t b[2];
    if (!m_mem->Read(off, b, 2)) return false;
    *out = (uint16_t)b[0] | ((uint16_t)b[1] << 8);
    return true;
}

bool Engine::R32(uint32_t off, uint32_t *out)
{
    uint8_t b[4];
    if (!m_mem->Read(off, b, 4)) return false;
    *out = (uint32_t)b[0] | ((uint32_t)b[1] << 8) |
           ((uint32_t)b[2] << 16) | ((uint32_t)b[3] << 24);
    return true;
}

void Engine::W8(uint32_t off, uint8_t val)
{
    m_mem->Write(off, &val, 1);
}

void Engine::W16(uint32_t off, uint16_t val)
{
    uint8_t b[2] = { (uint8_t)(val & 0xFF), (uint8_t)((val >> 8) & 0xFF) };
    m_mem->Write(off, b, 2);
}

void Engine::W32(uint32_t off, uint32_t val)
{
    uint8_t b[4] = { (uint8_t)(val & 0xFF), (uint8_t)((val >> 8) & 0xFF),
                     (uint8_t)((val >> 16) & 0xFF), (uint8_t)((val >> 24) & 0xFF) };
    m_mem->Write(off, b, 4);
}

// _off() - fold the identity-mapped kernel window down to a RAM offset.
bool Engine::RamOffset(uint32_t addr, uint32_t *out) const
{
    uint32_t maxb = m_ram_size;
    if (addr >= 0x80000000u && addr < 0x80000000u + maxb) {
        *out = addr - 0x80000000u;
        return true;
    }
    if (addr < maxb) {
        *out = addr;
        return true;
    }
    return false;
}

// ---------------------------------------------------------------------------
// Line counting
// ---------------------------------------------------------------------------
// Shared by the execute path and the skip path so the two can never disagree
// about how far a code reaches. A type 6 chain miscounted as 2 lines leaves its
// offset words to be read as fresh codes, and `0000000C 000001E3` is a valid
// type-0 write of 0xE3 into the interrupt vector table.

uint32_t Engine::PointerLines(const CodeList &codes, size_t idx)
{
    if (idx + 1 >= codes.size()) return 1;
    uint32_t n = codes[idx + 1].cmd & 0xFF;
    // NN is an 8-bit field: 0 preserves the legacy one-offset encoding, while
    // 0x01..0xFF represent 1..255 offsets.
    if (n < 1) n = 1;
    return 2 + (n / 2);
}

uint32_t Engine::GuardedLines(uint32_t cmd, uint32_t val, uint8_t type)
{
    // D takes N from the value word, E from the command word. This asymmetry
    // is load-bearing: it is what the Python _d_n() does, and the E handler
    // below reads N the same way.
    if (type == 0xD) return (val >> 24) & 0xFF;
    return (cmd >> 16) & 0xFF;
}

// ---------------------------------------------------------------------------
// Type D/E field decode
// ---------------------------------------------------------------------------
// The size+space field is the WHOLE nibble at bits 16-19, read as the literal
// digit: 0=16-bit phys, 1=16-bit virt, 2=8-bit phys, 3=8-bit virt.

void Engine::DecodeSpace(uint32_t val, bool *is_8bit, bool *is_virtual)
{
    uint32_t field = (val >> 16) & 0xF;
    *is_8bit    = (field & 0x2) != 0;
    *is_virtual = (field & 0x1) != 0;
}

bool Engine::DReadValue(uint32_t offset, bool is_8bit, bool is_virtual,
                        uint32_t *out)
{
    uint32_t phys;
    if (is_virtual) {
        if (!EnsurePageMap()) return false;
        if (!m_pagemap.ToPhys(offset, &phys)) return false;
    } else {
        phys = offset;
    }
    if (is_8bit) {
        uint8_t v;
        if (!R8(phys, &v)) return false;
        *out = v;
    } else {
        uint16_t v;
        if (!R16(phys, &v)) return false;
        *out = v;
    }
    return true;
}

bool Engine::TestCondition(uint32_t test, uint32_t mem_val, uint32_t cmp)
{
    switch (test) {
    case 0: return mem_val == cmp;
    case 1: return mem_val != cmp;
    case 2: return mem_val <  cmp;
    case 3: return mem_val >  cmp;
    case 4: return (mem_val & cmp) == 0;
    case 5: return (mem_val & cmp) != 0;
    case 6: return (mem_val | cmp) == 0;
    default: return (mem_val | cmp) != 0;
    }
}

bool Engine::EnsurePageMap()
{
    // A block can contain many virtual conditions/pointer dereferences. The
    // page tables are only invalidated once at the start of an apply pass, so
    // validating the kernel page map repeatedly inside that same pass just
    // rereads the same PDE/PTE. Cache that one validity result until the next
    // explicit invalidation.
    if (m_pagemap_dirty) {
        m_pagemap.Invalidate();
        m_pagemap_dirty = false;
        m_pagemap_checked = false;
    }
    if (!m_pagemap_checked) {
        m_pagemap_valid = m_pagemap.Valid();
        m_pagemap_checked = true;
    }
    return m_pagemap_valid;
}

// ---------------------------------------------------------------------------
// Pointer chains
// ---------------------------------------------------------------------------

bool Engine::ResolvePhysicalChain(uint32_t base_off, const uint32_t *offs,
                                  size_t off_count, uint32_t *out)
{
    // The base is physical, but every pointer *stored in guest memory* is a
    // guest virtual address - so each dereference folds back through the
    // kernel-window mapping. That asymmetry is why the two walkers stay apart.
    uint32_t cur;
    if (!R32(base_off, &cur) || cur == 0) return false;

    for (size_t i = 0; i < off_count; i++) {
        uint32_t ram;
        if (!RamOffset(cur, &ram)) return false;
        uint32_t target = ram + offs[i];
        if (i + 1 == off_count) {
            *out = target;
            return true;
        }
        if (!R32(target, &cur) || cur == 0) return false;
    }
    return false;
}

bool Engine::ResolveVirtualChain(uint32_t base_va, const uint32_t *offs,
                                 size_t off_count, uint32_t *out)
{
    if (!EnsurePageMap()) return false;

    uint32_t phys, cur;
    if (!m_pagemap.ToPhys(base_va, &phys)) return false;
    if (!R32(phys, &cur) || cur == 0) return false;

    for (size_t i = 0; i < off_count; i++) {
        cur = (uint32_t)(cur + offs[i]);        // wraps at 32 bits, as Python's & 0xFFFFFFFF
        if (i + 1 == off_count) break;
        if (!m_pagemap.ToPhys(cur, &phys)) return false;
        if (!R32(phys, &cur) || cur == 0) return false;
    }
    return m_pagemap.ToPhys(cur, out);
}

// ---------------------------------------------------------------------------
// Individual type handlers
// ---------------------------------------------------------------------------

size_t Engine::Type3(const CodeList &codes, size_t idx, uint32_t offset,
                     uint32_t bid)
{
    // APPLIED ONCE PER ACTIVATION, not once per tick - otherwise the freeze
    // loop adds the delta 60+ times a second. Re-armed when the cheat is
    // toggled off and on.
    uint32_t cmd = codes[idx].cmd;
    uint32_t param = cmd & 0x0FFFFFFF;

    if (m_increment_applied.count(bid)) {
        if (param == 0x00400000 || param == 0x00500000) return idx + 2;
        return idx + 1;
    }

    uint32_t addr = offset;

    if ((param & 0xFFFFFF00) == 0x00000000) {
        uint8_t cur;
        if (R8(addr, &cur)) W8(addr, (uint8_t)(cur + (param & 0xFF)));
        m_increment_applied.insert(bid);
        return idx + 1;
    }
    if ((param & 0xFFFFFF00) == 0x00100000) {
        uint8_t cur;
        if (R8(addr, &cur)) W8(addr, (uint8_t)(cur - (param & 0xFF)));
        m_increment_applied.insert(bid);
        return idx + 1;
    }
    if ((param & 0xFFFF0000) == 0x00200000) {
        uint16_t cur;
        if (R16(addr, &cur)) W16(addr, (uint16_t)(cur + (param & 0xFFFF)));
        m_increment_applied.insert(bid);
        return idx + 1;
    }
    if ((param & 0xFFFF0000) == 0x00300000) {
        uint16_t cur;
        if (R16(addr, &cur)) W16(addr, (uint16_t)(cur - (param & 0xFFFF)));
        m_increment_applied.insert(bid);
        return idx + 1;
    }
    if (param == 0x00400000) {
        if (idx + 1 >= codes.size()) return idx + 1;
        uint32_t inc = codes[idx + 1].val;
        uint32_t cur;
        if (R32(addr, &cur)) W32(addr, cur + inc);
        m_increment_applied.insert(bid);
        return idx + 2;
    }
    if (param == 0x00500000) {
        if (idx + 1 >= codes.size()) return idx + 1;
        uint32_t dec = codes[idx + 1].val;
        uint32_t cur;
        if (R32(addr, &cur)) W32(addr, cur - dec);
        m_increment_applied.insert(bid);
        return idx + 2;
    }
    return idx + 1;
}

size_t Engine::Type4(const CodeList &codes, size_t idx, uint32_t offset,
                     uint32_t val)
{
    if (idx + 1 >= codes.size()) return idx + 1;
    uint32_t nnnn = (val >> 16) & 0xFFFF;
    uint32_t ssss = val & 0xFFFF;
    uint32_t v = codes[idx + 1].cmd;
    uint32_t i = codes[idx + 1].val;
    uint32_t addr = offset;
    for (uint32_t k = 0; k < nnnn; k++) {
        W32(addr, v);
        addr += ssss * 4;       // step is in WORDS
        v += i;
    }
    return idx + 2;
}

size_t Engine::Type5(const CodeList &codes, size_t idx, uint32_t src_off,
                     uint32_t length)
{
    if (idx + 1 >= codes.size()) return idx + 1;
    uint32_t dest_off = codes[idx + 1].cmd & 0x0FFFFFFF;

    // Python reads the whole span then writes whatever came back, which for a
    // short read writes a short buffer rather than nothing. Matched here, and
    // matched in the harness's FlatMemory, so a copy running off the end of RAM
    // behaves identically in both.
    m_copy_scratch.resize(length);
    size_t got = m_mem->ReadPartial(src_off, m_copy_scratch.data(), length);
    // Called unconditionally, even for got == 0. The Python reference passes
    // whatever read_mem returned straight to write_mem without checking, so a
    // zero-length copy still performs a (no-op) write. Guarding it here made
    // the two implementations disagree on the write log even though memory
    // ended up identical - and write COUNT is what the freeze loop and the
    // JIT invalidation cost scale with, so it is not a difference to paper
    // over.
    m_mem->Write(dest_off, m_copy_scratch.data(), got);
    return idx + 2;
}

size_t Engine::Type6(const CodeList &codes, size_t idx, uint32_t base,
                     uint32_t val)
{
    if (idx + 1 >= codes.size()) return idx + 1;

    uint32_t hdr       = codes[idx + 1].cmd;
    uint32_t first_off = codes[idx + 1].val;
    bool virtual_base  = ((hdr >> 24) & 0xFF) == 0x01;
    uint32_t size      = (hdr >> 16) & 0xFF;
    uint32_t n         = hdr & 0xFF;
    // NN is the full low byte. 0 is the backward-compatible one-offset form;
    // nonzero values directly encode 1..255 offsets.
    if (n < 1) n = 1;

    /* Preserve the allocation-free hot path without changing the code type:
     * the format can describe up to 255 offsets, which is only 1020 bytes of
     * temporary stack storage. Only offs[0..off_count) are ever read. */
    std::array<uint32_t, 0xFF> offs;
    size_t off_count = 0;
    offs[off_count++] = first_off;
    size_t consumed = 2;
    while (off_count < n && idx + consumed < codes.size()) {
        offs[off_count++] = codes[idx + consumed].cmd;
        if (off_count < n) offs[off_count++] = codes[idx + consumed].val;
        consumed++;
    }

    // Always advance by the DECLARED count even if the block ran out, so a
    // truncated chain cannot bleed into the next code.
    size_t total_lines = 2 + (n / 2);

    uint32_t target;
    bool ok = virtual_base
                  ? ResolveVirtualChain(base, offs.data(), off_count, &target)
                  : ResolvePhysicalChain(base, offs.data(), off_count, &target);
    if (!ok) return idx + total_lines;

    if (size == 0x00)      W8(target,  (uint8_t)(val & 0xFF));
    else if (size == 0x01) W16(target, (uint16_t)(val & 0xFFFF));
    else                   W32(target, val);
    return idx + total_lines;
}

void Engine::Type89A(uint32_t va, uint32_t size, uint32_t val)
{
    if (!EnsurePageMap()) return;
    uint32_t phys;
    if (!m_pagemap.ToPhys(va, &phys)) return;
    if (size == 1)      W8(phys,  (uint8_t)(val & 0xFF));
    else if (size == 2) W16(phys, (uint16_t)(val & 0xFFFF));
    else                W32(phys, val);
}

size_t Engine::TypeB(size_t idx, uint32_t offset, uint32_t val)
{
    uint32_t param = val & 0xFFFF0000;
    uint8_t  c8;
    uint16_t c16;
    switch (param) {
    case 0x00000000: if (R8(offset, &c8))   W8(offset,  (uint8_t)(c8  | (val & 0xFF)));   break;
    case 0x00100000: if (R16(offset, &c16)) W16(offset, (uint16_t)(c16 | (val & 0xFFFF))); break;
    case 0x00200000: if (R8(offset, &c8))   W8(offset,  (uint8_t)(c8  & (val & 0xFF)));   break;
    case 0x00300000: if (R16(offset, &c16)) W16(offset, (uint16_t)(c16 & (val & 0xFFFF))); break;
    case 0x00400000: if (R8(offset, &c8))   W8(offset,  (uint8_t)(c8  ^ (val & 0xFF)));   break;
    case 0x00500000: if (R16(offset, &c16)) W16(offset, (uint16_t)(c16 ^ (val & 0xFFFF))); break;
    default: break;
    }
    return idx + 1;
}

size_t Engine::TypeD(size_t idx, uint32_t cmd, uint32_t val)
{
    uint32_t n       = GuardedLines(cmd, val, 0xD);
    uint32_t offset  = cmd & 0x0FFFFFFF;
    uint32_t cmp     = val & 0xFFFF;
    uint32_t test    = (val >> 20) & 0x7;
    bool is_8bit, is_virtual;
    DecodeSpace(val, &is_8bit, &is_virtual);

    uint32_t mem_val;
    if (!DReadValue(offset, is_8bit, is_virtual, &mem_val)) {
        // Unreadable: Python returns idx+1, i.e. does NOT skip the guarded
        // lines. Deliberately matched even though skipping would be safer -
        // the reference is the specification.
        return idx + 1;
    }
    return TestCondition(test, mem_val, cmp) ? idx + 1 : idx + 1 + n;
}

size_t Engine::TypeE(size_t idx, uint32_t cmd, uint32_t val, uint32_t bid)
{
    uint32_t n      = GuardedLines(cmd, val, 0xE);
    uint32_t offset = cmd & 0x0FFFFFFF;
    uint32_t cmp    = val & 0xFFFF;
    uint32_t test   = (val >> 20) & 0x7;
    bool is_8bit, is_virtual;
    DecodeSpace(val, &is_8bit, &is_virtual);

    SwitchKey key{ bid, (uint32_t)idx };
    auto &st = m_switches[key];             // value-initialised: off, prev=false

    uint32_t mem_val;
    if (!DReadValue(offset, is_8bit, is_virtual, &mem_val)) {
        // Hold the switch where it is. A failed read is not a button release,
        // and it is certainly not permission to run the guarded lines.
        return st.on ? idx + 1 : idx + 1 + n;
    }

    bool cond = TestCondition(test, mem_val, cmp);
    // Edge, not level: flip only on false -> true, so holding the button does
    // not toggle every tick.
    if (cond && !st.prev) st.on = !st.on;
    st.prev = cond;

    return st.on ? idx + 1 : idx + 1 + n;
}

// ---------------------------------------------------------------------------
// The dispatch loop
// ---------------------------------------------------------------------------

void Engine::ExecuteBlock(const CodeList &codes, uint32_t bid)
{
    bool exec_enabled = true;
    size_t idx = 0;

    while (idx < codes.size()) {
        uint32_t cmd = codes[idx].cmd;
        uint32_t val = codes[idx].val;
        uint8_t  type = (cmd >> 28) & 0xF;
        uint32_t offset = cmd & 0x0FFFFFFF;

        // Type C is evaluated even while disabled - it is what can re-enable
        // execution, so it must run before the skip path.
        if (type == 0xC) {
            uint32_t cur;
            exec_enabled = R32(offset, &cur) && cur == val;
            idx += 1;
            continue;
        }

        if (!exec_enabled) {
            // Skip with the SAME line counts the execute path uses.
            switch (type) {
            case 0x3: {
                uint32_t param = cmd & 0x0FFFFFFF;
                idx += (param == 0x00400000 || param == 0x00500000) ? 2 : 1;
                break;
            }
            case 0x6:
            case 0x7:
                idx += PointerLines(codes, idx);
                break;
            case 0x4:
            case 0x5:
                idx += 2;
                break;
            case 0xD:
            case 0xE:
                idx += 1 + GuardedLines(cmd, val, type);
                break;
            default:
                idx += 1;
                break;
            }
            continue;
        }

        switch (type) {
        case 0x0: W8(offset,  (uint8_t)(val & 0xFF));    idx += 1; break;
        case 0x1: W16(offset, (uint16_t)(val & 0xFFFF)); idx += 1; break;
        case 0x2: W32(offset, val);                      idx += 1; break;
        case 0x3: idx = Type3(codes, idx, offset, bid);            break;
        case 0x4: idx = Type4(codes, idx, offset, val);            break;
        case 0x5: idx = Type5(codes, idx, offset, val);            break;
        case 0x6: idx = Type6(codes, idx, offset, val);            break;
        case 0x7:
            // Retired (was the virtual-base pointer write, now type 6's XX
            // flag). Consumed with type 6's line count, NOT skipped as one
            // line - leaving the chain's tail to be read as fresh codes is
            // how you get a write into the interrupt vector table.
            idx += PointerLines(codes, idx);
            break;
        case 0x8: Type89A(offset, 1, val); idx += 1; break;
        case 0x9: Type89A(offset, 2, val); idx += 1; break;
        case 0xA: Type89A(offset, 4, val); idx += 1; break;
        case 0xB: idx = TypeB(idx, offset, val);       break;
        case 0xD: idx = TypeD(idx, cmd, val);          break;
        case 0xE: idx = TypeE(idx, cmd, val, bid);     break;
        case 0xF: idx += 1; break;      // hook code - reserved, does nothing
        default:  idx += 1; break;
        }
    }
}

void Engine::ClearSwitches(uint32_t bid, bool all)
{
    if (all) {
        m_switches.clear();
        m_increment_applied.clear();
        return;
    }
    for (auto it = m_switches.begin(); it != m_switches.end(); ) {
        if (it->first.bid == bid) it = m_switches.erase(it);
        else ++it;
    }
    m_increment_applied.erase(bid);
}

} // namespace xcodes
