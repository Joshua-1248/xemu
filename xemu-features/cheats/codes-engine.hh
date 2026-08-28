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
// codes-engine.hh - Original Xbox cheat code interpreter.
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <array>

namespace xcodes {

// One `AAAAAAAA VVVVVVVV` line.
struct Code {
    uint32_t cmd;
    uint32_t val;
};
using CodeList = std::vector<Code>;

// Guest RAM, addressed as a PHYSICAL offset. In-tree this forwards to
// dma_memory_read/write on address_space_memory; in the test harness it is a
// flat buffer.
class Memory {
public:
    virtual ~Memory() = default;
    // All-or-nothing: false if the full length could not be read.
    virtual bool Read(uint32_t off, void *buf, size_t len) = 0;
    // Best-effort, returns bytes actually read. Only type 5 needs this, and
    // only because the Python reference copies a short read rather than
    // failing.
    virtual size_t ReadPartial(uint32_t off, void *buf, size_t len) = 0;
    virtual void Write(uint32_t off, const void *buf, size_t len) = 0;
    virtual uint32_t RamSize() const = 0;
};

// Guest virtual -> physical via the live page tables.
class PageMap {
public:
    static constexpr uint32_t PD_PHYS = 0x0000F000;

    void Attach(Memory *mem)
    {
        m_mem = mem;
        m_ram_size = mem->RamSize();
        Invalidate();
    }
    bool ToPhys(uint32_t va, uint32_t *out) const;
    void Invalidate();
    bool Valid() const;

private:
    bool rd32_phys(uint32_t pa, uint32_t *out) const;

    struct CacheEntry {
        uint32_t page = 0;
        uint32_t base = 0;
        uint32_t generation = 0;
    };
    static constexpr size_t CACHE_ENTRIES = 4096;

    Memory *m_mem = nullptr;
    uint32_t m_ram_size = 0;
    uint32_t m_generation = 1;
    mutable std::array<CacheEntry, CACHE_ENTRIES> m_cache {};
};

class Engine {
public:
    void Attach(Memory *mem) {
        m_mem = mem;
        m_ram_size = mem->RamSize();
        m_pagemap.Attach(mem);
        m_pagemap_dirty = true;
        m_pagemap_checked = false;
        m_pagemap_valid = false;
        m_switches.reserve(64);
        m_increment_applied.reserve(64);
    }

    // Run one cheat's code list once. `bid` identifies the cheat block and is
    // what type 3's once-per-activation latch and type E's switch state are
    // keyed on.
    void ExecuteBlock(const CodeList &codes, uint32_t bid);

    // Drop switch and increment state. Called when a game is deactivated: a
    // switch left on would apply its guarded lines the moment the next game
    // attached, with no button pressed.
    void ClearSwitches(uint32_t bid = 0, bool all = true);

    // Tell the translator the guest may have re-paged (level load).
    void InvalidatePageMap() {
        m_pagemap_dirty = true;
        m_pagemap_checked = false;
    }

private:
    struct SwitchKey {
        uint32_t bid, line;
        bool operator==(const SwitchKey &o) const {
            return bid == o.bid && line == o.line;
        }
    };
    struct SwitchKeyHash {
        size_t operator()(const SwitchKey &k) const {
            return (size_t)k.bid * (size_t)0x9e3779b1u ^ (size_t)k.line;
        }
    };
    struct SwitchState { bool on = false; bool prev = false; };

    bool R8(uint32_t off, uint8_t *out);
    bool R16(uint32_t off, uint16_t *out);
    bool R32(uint32_t off, uint32_t *out);
    void W8(uint32_t off, uint8_t val);
    void W16(uint32_t off, uint16_t val);
    void W32(uint32_t off, uint32_t val);
    bool RamOffset(uint32_t addr, uint32_t *out) const;

    static uint32_t PointerLines(const CodeList &codes, size_t idx);
    static uint32_t GuardedLines(uint32_t cmd, uint32_t val, uint8_t type);
    static void DecodeSpace(uint32_t val, bool *is_8bit, bool *is_virtual);
    static bool TestCondition(uint32_t test, uint32_t mem_val, uint32_t cmp);

    bool EnsurePageMap();
    bool DReadValue(uint32_t offset, bool is_8bit, bool is_virtual, uint32_t *out);
    bool ResolvePhysicalChain(uint32_t base_off, const uint32_t *offs,
                              size_t off_count, uint32_t *out);
    bool ResolveVirtualChain(uint32_t base_va, const uint32_t *offs,
                             size_t off_count, uint32_t *out);

    size_t Type3(const CodeList &codes, size_t idx, uint32_t offset, uint32_t bid);
    size_t Type4(const CodeList &codes, size_t idx, uint32_t offset, uint32_t val);
    size_t Type5(const CodeList &codes, size_t idx, uint32_t src_off, uint32_t length);
    size_t Type6(const CodeList &codes, size_t idx, uint32_t base, uint32_t val);
    void   Type89A(uint32_t va, uint32_t size, uint32_t val);
    size_t TypeB(size_t idx, uint32_t offset, uint32_t val);
    size_t TypeD(size_t idx, uint32_t cmd, uint32_t val);
    size_t TypeE(size_t idx, uint32_t cmd, uint32_t val, uint32_t bid);

    Memory *m_mem = nullptr;
    uint32_t m_ram_size = 0;
    PageMap m_pagemap;
    bool m_pagemap_dirty = true;
    bool m_pagemap_checked = false;
    bool m_pagemap_valid = false;
    std::unordered_map<SwitchKey, SwitchState, SwitchKeyHash> m_switches;
    std::unordered_set<uint32_t> m_increment_applied;

    /* Type 5 block copies can execute every Instantaneous tick. Reuse the
     * largest buffer already allocated instead of malloc/free churn on every
     * pass. resize() only grows capacity when a larger copy is encountered. */
    std::vector<uint8_t> m_copy_scratch;
};

} // namespace xcodes
