// SPDX-License-Identifier: GPL-2.0-or-later
//
// xemu User Interface - Disassembler / CPU debugger
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "xemu-features/debug-tools/debug-api.h"

class DisassemblerWindow
{
public:
    bool m_is_open;

    DisassemblerWindow();
    void Draw();

private:
    struct Line {
        uint32_t addr;
        uint8_t  bytes[16];
        uint8_t  len;
        char     mnemonic[32];
        char     ops[160];
        bool     valid;
    };

    struct Bp {
        uint32_t addr;
        bool     enabled;
    };

    struct Wp {
        uint32_t addr;
        uint32_t len;
        int      flags;      // BP_MEM_READ / BP_MEM_WRITE / both
        bool     enabled;
    };

    void DrawToolbar();
    void DrawDisassembly();
    void DrawRegisters();
    void DrawBreakpoints();
    void DrawMemory();

    void Refresh();
    void GoTo(uint32_t addr, bool centre = false);
    bool ReadEip(uint32_t *out);
    const XemuDbgRegs &Regs();

    // Walk backwards from `addr` far enough to land on a plausible
    // instruction boundary. x86 is variable-length, so there is no exact
    // answer -- this decodes forward from several earlier offsets and keeps
    // whichever stream lands cleanly on `addr`, which is what every
    // disassembler UI does.
    uint32_t BackUp(uint32_t addr, int instructions) const;

    uint32_t m_base = 0x00010000;   // top of the listing
    uint32_t m_selected = 0;
    bool     m_follow_eip = true;
    bool     m_have_selection = false;
    char     m_goto_buf[16] = "00010000";
    char     m_mem_buf[16] = "00010000";
    uint32_t m_mem_addr = 0x00010000;

    /*
     * Memory viewer state, mirroring the external viewer's controls.
     *
     * m_mem_virtual is the one that matters: the Xbox maps its XBE through
     * page tables, so the same byte has a guest VIRTUAL address (what a cheat
     * code uses) and a PHYSICAL offset in the RAM block. Flipping the tick
     * box translates the address under the cursor so the same byte stays put,
     * rather than jumping.
     */
    bool     m_mem_virtual = true;
    bool     m_mem_big_endian = false;
    int      m_mem_region = 0;
    int      m_mem_bytes_per_row = 16;
    uint32_t m_mem_sel = 0;
    bool     m_mem_have_sel = false;

    /* 24-row memory-view snapshot. Guest reads are refreshed only on the
     * configured Live polling tick or when the view moves; repainting ImGui
     * itself never touches guest memory. */
    uint8_t  m_mem_cache[24][32] {};
    int      m_mem_cache_got[24] {};
    uint32_t m_mem_cache_addr = 0;
    int      m_mem_cache_per = 0;
    bool     m_mem_cache_virtual = true;
    bool     m_mem_cache_valid = false;

    // Debugger state
    char     m_sym_path[512] = "";
    std::string m_status;
    uint32_t m_status_ms = 0;

    std::vector<Line> m_lines;
    std::vector<Bp> m_bps;
    std::vector<Wp> m_wps;

    // Rebuilt only when something moved, not every frame: decoding 64
    // instructions through the page tables on every frame of a 60 Hz UI is
    // pure waste when the guest is paused.
    uint32_t m_last_base = 0xFFFFFFFF;
    bool     m_dirty = true;

    XemuDbgRegs m_regs {};
    bool        m_regs_fresh = false;

    // Live polling. Off, or between ticks, the window redraws from the last
    // snapshot and touches no guest state at all.
    bool     m_live = true;
    int      m_interval_ms = 100;
    uint32_t m_last_poll_ms = 0;
    bool     m_poll_now = false;
};

extern DisassemblerWindow disassembler_window;
