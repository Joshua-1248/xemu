//
// xemu User Interface - Disassembler / CPU debugger
//
// Layout follows the PCSX2/DuckStation shape: a disassembly listing with
// registers beside it, plus breakpoints and a memory view. Window plumbing is
// copied from DebugApuWindow so it behaves like the existing Debug windows.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2 of the License, or
// (at your option) any later version.
//
#include <cinttypes>
#include <cstdio>
#include <cstring>
#include <algorithm>

#include "disassembler.hh"
#include "common.hh"
#include "misc.hh"
#include "font-manager.hh"
#include "viewport-manager.hh"

#include "../xemu-dbg.h"
#include "../xemu-guestmem.h"

DisassemblerWindow disassembler_window;

#define BP_MEM_READ  0x01
#define BP_MEM_WRITE 0x02

static const int kLineCount = 64;

DisassemblerWindow::DisassemblerWindow() : m_is_open(false)
{
}

// One register read per frame, shared by the listing and the register pane.
// Each read calls cpu_synchronize_state(); doing it several times a frame was
// pure waste.
const XemuDbgRegs &DisassemblerWindow::Regs()
{
    if (!m_regs_fresh) {
        xemu_dbg_get_regs(&m_regs);
        m_regs_fresh = true;
    }
    return m_regs;
}

bool DisassemblerWindow::ReadEip(uint32_t *out)
{
    const XemuDbgRegs &r = Regs();
    if (!r.valid) {
        return false;
    }
    *out = r.eip;
    return true;
}

// x86 has no way to step backwards exactly. Decode forward from a few
// earlier offsets and keep the first stream that lands cleanly on `addr` -
// the same heuristic every disassembler UI uses. Falls back to a fixed
// guess so scrolling up never gets stuck.
uint32_t DisassemblerWindow::BackUp(uint32_t addr, int instructions) const
{
    const uint32_t probe = (uint32_t)(instructions * 8);
    if (addr < probe) {
        return 0;
    }

    for (uint32_t back = probe; back >= (uint32_t)instructions; back--) {
        uint32_t pc = addr - back;
        XemuDbgInsn tmp[64];
        int n = xemu_dbg_disasm(pc, std::min(instructions * 2, 64), tmp);
        for (int i = 0; i < n; i++) {
            if (tmp[i].addr == addr) {
                // Landed on it: rewind `instructions` entries from here.
                int idx = i - instructions;
                return tmp[idx < 0 ? 0 : idx].addr;
            }
            if (tmp[i].addr > addr) {
                break;      // overshot, this alignment is wrong
            }
        }
    }
    return addr - (uint32_t)instructions;
}

void DisassemblerWindow::GoTo(uint32_t addr, bool centre)
{
    m_base = centre ? BackUp(addr, 8) : addr;
    m_selected = addr;
    m_have_selection = true;
    m_dirty = true;
}

void DisassemblerWindow::Refresh()
{
    // Re-decoding reads guest memory through the page tables; only do it when
    // the view moved, or on a polling tick.
    if (!m_dirty && m_base == m_last_base && !m_poll_now) {
        return;
    }
    m_lines.clear();

    XemuDbgInsn buf[kLineCount];
    int n = xemu_dbg_disasm(m_base, kLineCount, buf);
    m_lines.reserve(n);
    for (int i = 0; i < n; i++) {
        Line l;
        l.addr = buf[i].addr;
        l.len = buf[i].len;
        memcpy(l.bytes, buf[i].bytes, sizeof(l.bytes));
        memcpy(l.mnemonic, buf[i].mnemonic, sizeof(l.mnemonic));
        memcpy(l.ops, buf[i].ops, sizeof(l.ops));
        l.valid = buf[i].valid;
        m_lines.push_back(l);
    }

    m_last_base = m_base;
    m_dirty = false;
}

void DisassemblerWindow::DrawToolbar()
{
    bool running = xemu_dbg_is_running();

    if (ImGui::Button(running ? "Pause" : "Resume")) {
        if (running) {
            xemu_dbg_pause();
        } else {
            xemu_dbg_resume();
        }
        m_dirty = true;
    }
    ImGui::SameLine();

    // Stepping only means anything while halted.
    ImGui::BeginDisabled(running);

    if (ImGui::Button("Step Into")) {
        xemu_dbg_step();
        m_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Step Over")) {
        // A call counts as one step: temporary breakpoint after it, continue.
        xemu_dbg_step_over();
        m_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Step Out")) {
        const char *err = xemu_dbg_step_out();
        if (err) {
            m_status = err;
            m_status_ms = SDL_GetTicks();
        }
        m_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Run to cursor")) {
        if (m_have_selection) {
            xemu_dbg_run_to(m_selected);
        } else {
            m_status = "click an instruction first";
            m_status_ms = SDL_GetTicks();
        }
    }

    ImGui::EndDisabled();
    ImGui::SameLine();

    if (ImGui::Button("Go to EIP")) {
        uint32_t eip;
        if (ReadEip(&eip)) {
            GoTo(eip, true);
        }
    }
    ImGui::SameLine();

    ImGui::Checkbox("Follow EIP", &m_follow_eip);
    ImGui::SameLine();

    /*
     * Live off means the panes only refresh when you ask (Refresh button, or
     * any action that moves the view). That is the difference between a
     * debugger you can leave open and one that costs frame rate, because
     * every refresh reads guest memory and synchronises CPU state.
     */
    ImGui::Checkbox("Live", &m_live);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(130 * g_viewport_mgr.m_scale);
    ImGui::SliderInt("ms", &m_interval_ms, 1, 1000);
    if (m_interval_ms < 1) m_interval_ms = 1;
    if (m_interval_ms > 1000) m_interval_ms = 1000;
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        m_dirty = true;
        m_regs_fresh = false;
    }
    ImGui::SameLine();

    ImGui::SetNextItemWidth(110 * g_viewport_mgr.m_scale);
    if (ImGui::InputText("Address", m_goto_buf, sizeof(m_goto_buf),
                         ImGuiInputTextFlags_CharsHexadecimal |
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        GoTo((uint32_t)strtoul(m_goto_buf, NULL, 16), false);
    }

    ImGui::SameLine();
    ImGui::TextDisabled(running ? "running" : "paused");

    if (!m_status.empty() && SDL_GetTicks() - m_status_ms < 6000) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s",
                           m_status.c_str());
    }
}

void DisassemblerWindow::DrawDisassembly()
{
    uint32_t eip = 0;
    bool have_eip = ReadEip(&eip);

    // Following EIP only makes sense while paused; chasing it at 60 Hz on a
    // running guest just produces a blur and re-decodes every frame.
    if (m_follow_eip && have_eip && !xemu_dbg_is_running()) {
        bool visible = false;
        for (const auto &l : m_lines) {
            if (l.addr == eip) { visible = true; break; }
        }
        if (!visible) {
            GoTo(eip, true);
        }
    }

    Refresh();

    ImGui::BeginChild("##disasm",
                      ImVec2(0, 320.0f * g_viewport_mgr.m_scale), true,
                      ImGuiWindowFlags_NoMove);

    // Mouse wheel scrolls by instructions, not pixels, so the listing does
    // not tear itself apart on variable-length instructions.
    if (ImGui::IsWindowHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel > 0) {
            m_base = BackUp(m_base, 3);
            m_dirty = true;
        } else if (wheel < 0 && m_lines.size() > 3) {
            m_base = m_lines[3].addr;
            m_dirty = true;
        }
    }

    ImGui::PushFont(g_font_mgr.m_fixed_width_font);

    for (const auto &l : m_lines) {
        bool is_eip = have_eip && l.addr == eip;
        bool is_sel = m_have_selection && l.addr == m_selected;

        bool has_bp = false;
        for (const auto &b : m_bps) {
            if (b.addr == l.addr) { has_bp = true; break; }
        }

        ImGui::PushID((int)l.addr);

        // Breakpoint gutter: click the dot to toggle, like every other
        // debugger.
        if (ImGui::SmallButton(has_bp ? "*" : " ")) {
            if (has_bp) {
                xemu_dbg_bp_remove(l.addr);
                for (size_t i = 0; i < m_bps.size(); i++) {
                    if (m_bps[i].addr == l.addr) {
                        m_bps.erase(m_bps.begin() + i);
                        break;
                    }
                }
            } else if (xemu_dbg_bp_insert(l.addr)) {
                m_bps.push_back({ l.addr, true });
            }
        }
        ImGui::SameLine();

        char bytes[48] = { 0 };
        int p = 0;
        for (int i = 0; i < l.len && i < 8; i++) {
            p += snprintf(bytes + p, sizeof(bytes) - p, "%02X ", l.bytes[i]);
        }

        char text[192];
        snprintf(text, sizeof(text), "%08X  %-24s %-8s %s",
                 l.addr, bytes, l.mnemonic, l.ops);

        if (is_eip) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(120, 220, 120, 255));
        } else if (!l.valid) {
            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(200, 120, 120, 255));
        }

        if (ImGui::Selectable(text, is_sel)) {
            m_selected = l.addr;
            m_have_selection = true;
        }

        if (is_eip || !l.valid) {
            ImGui::PopStyleColor();
        }
        ImGui::PopID();
    }

    ImGui::PopFont();
    ImGui::EndChild();
}

void DisassemblerWindow::DrawRegisters()
{
    const XemuDbgRegs &r = Regs();

    if (!r.valid) {
        ImGui::TextDisabled("No CPU.");
        return;
    }

    ImGui::PushFont(g_font_mgr.m_fixed_width_font);

    static const char *names[] = { "EAX", "ECX", "EDX", "EBX",
                                   "ESP", "EBP", "ESI", "EDI" };
    const uint32_t vals[] = { r.eax, r.ecx, r.edx, r.ebx,
                              r.esp, r.ebp, r.esi, r.edi };

    for (int i = 0; i < 8; i++) {
        char buf[16];
        snprintf(buf, sizeof(buf), "%08X", vals[i]);
        ImGui::SetNextItemWidth(90 * g_viewport_mgr.m_scale);
        ImGui::PushID(i);
        if (ImGui::InputText(names[i], buf, sizeof(buf),
                            ImGuiInputTextFlags_CharsHexadecimal |
                            ImGuiInputTextFlags_EnterReturnsTrue)) {
            char lower[8];
            snprintf(lower, sizeof(lower), "%s", names[i]);
            for (char *c = lower; *c; c++) {
                *c = (char)tolower((unsigned char)*c);
            }
            xemu_dbg_set_reg(lower, (uint32_t)strtoul(buf, NULL, 16));
            m_dirty = true;
        }
        ImGui::PopID();
    }

    ImGui::Separator();
    ImGui::Text("EIP %08X", r.eip);
    ImGui::Text("FLG %08X", r.eflags);

    // The flags people actually look for while reversing.
    ImGui::TextDisabled("%s %s %s %s %s %s",
                        (r.eflags & 0x0001) ? "CF" : "cf",
                        (r.eflags & 0x0040) ? "ZF" : "zf",
                        (r.eflags & 0x0080) ? "SF" : "sf",
                        (r.eflags & 0x0800) ? "OF" : "of",
                        (r.eflags & 0x0004) ? "PF" : "pf",
                        (r.eflags & 0x0010) ? "AF" : "af");

    ImGui::Separator();
    ImGui::Text("CS %04X  SS %04X", r.cs, r.ss);
    ImGui::Text("DS %04X  ES %04X", r.ds, r.es);
    ImGui::Text("FS %04X  GS %04X", r.fs, r.gs);
    ImGui::Separator();
    ImGui::Text("CR0 %08X", r.cr0);
    ImGui::Text("CR2 %08X", r.cr2);
    ImGui::Text("CR3 %08X", r.cr3);

    ImGui::PopFont();
}

void DisassemblerWindow::DrawBreakpoints()
{
    static char wp_addr[16] = "00000000";
    static int wp_len = 4;
    static int wp_kind = 1;   // 0 read, 1 write, 2 both

    ImGui::SetNextItemWidth(90 * g_viewport_mgr.m_scale);
    ImGui::InputText("Addr##wp", wp_addr, sizeof(wp_addr),
                     ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(50 * g_viewport_mgr.m_scale);
    ImGui::InputInt("Len", &wp_len, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * g_viewport_mgr.m_scale);
    ImGui::Combo("##wpkind", &wp_kind, "Read\0Write\0Both\0");
    ImGui::SameLine();

    if (ImGui::Button("Add watch")) {
        int flags = wp_kind == 0 ? BP_MEM_READ
                  : wp_kind == 1 ? BP_MEM_WRITE
                                 : (BP_MEM_READ | BP_MEM_WRITE);
        uint32_t a = (uint32_t)strtoul(wp_addr, NULL, 16);
        uint32_t n = (uint32_t)(wp_len > 0 ? wp_len : 1);
        if (xemu_dbg_wp_insert(a, n, flags)) {
            m_wps.push_back({ a, n, flags, true });
        }
    }

    ImGui::Separator();

    for (size_t i = 0; i < m_bps.size(); i++) {
        ImGui::PushID((int)(1000 + i));
        ImGui::Text("exec  %08X", m_bps[i].addr);
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            xemu_dbg_bp_remove(m_bps[i].addr);
            m_bps.erase(m_bps.begin() + i);
            ImGui::PopID();
            break;
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Show")) {
            GoTo(m_bps[i].addr, true);
        }
        ImGui::PopID();
    }

    for (size_t i = 0; i < m_wps.size(); i++) {
        ImGui::PushID((int)(2000 + i));
        ImGui::Text("watch %08X +%u %s", m_wps[i].addr, m_wps[i].len,
                    m_wps[i].flags == BP_MEM_READ ? "r" :
                    m_wps[i].flags == BP_MEM_WRITE ? "w" : "rw");
        ImGui::SameLine();
        if (ImGui::SmallButton("Remove")) {
            xemu_dbg_wp_remove(m_wps[i].addr, m_wps[i].len, m_wps[i].flags);
            m_wps.erase(m_wps.begin() + i);
            ImGui::PopID();
            break;
        }
        ImGui::PopID();
    }

    if (m_bps.empty() && m_wps.empty()) {
        ImGui::TextDisabled("None. Click the gutter beside an instruction to "
                            "set an execution breakpoint.");
    }
}

// Region presets, lifted from the external viewer's XBOX_SCAN_REGIONS.
// [V] entries are virtual, [P] physical - selecting one sets the address
// space as well as the address, because a kernel-window address means
// nothing read as a physical offset.
struct MemRegion { const char *name; uint32_t base; bool virt; };
static const MemRegion kRegions[] = {
    { "Custom (type an address)",              0x00000000, true  },
    { "[V] User space (heap + XBE)",           0x00010000, true  },
    { "[V] XBE image only (statics)",          0x00010000, true  },
    { "[V] User heap only",                    0x00720000, true  },
    { "[V] Kernel window 0x80000000+",         0x80000000, true  },
    { "[P] Physical 0-64MB",                   0x00000000, false },
    { "[P] Physical 64-128MB",                 0x04000000, false },
};

void DisassemblerWindow::DrawMemory()
{
    // --- address + region ---
    ImGui::SetNextItemWidth(110 * g_viewport_mgr.m_scale);
    if (ImGui::InputText("Address##mem", m_mem_buf, sizeof(m_mem_buf),
                         ImGuiInputTextFlags_CharsHexadecimal |
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        m_mem_addr = (uint32_t)strtoul(m_mem_buf, NULL, 16);
        m_mem_region = 0;
    }
    ImGui::SameLine();

    ImGui::SetNextItemWidth(240 * g_viewport_mgr.m_scale);
    if (ImGui::BeginCombo("Region", kRegions[m_mem_region].name)) {
        for (int i = 0; i < (int)(sizeof(kRegions) / sizeof(kRegions[0])); i++) {
            if (ImGui::Selectable(kRegions[i].name, i == m_mem_region)) {
                m_mem_region = i;
                if (i != 0) {
                    m_mem_virtual = kRegions[i].virt;
                    m_mem_addr = kRegions[i].base;
                    snprintf(m_mem_buf, sizeof(m_mem_buf), "%08X", m_mem_addr);
                }
            }
        }
        ImGui::EndCombo();
    }

    // --- address space ---
    // Translating the address as the box is ticked keeps the same byte on
    // screen. Just flipping the flag would leave the number pointing at a
    // completely different byte in the other space.
    bool was_virtual = m_mem_virtual;
    if (ImGui::Checkbox("Virtual", &m_mem_virtual)) {
        uint32_t translated = 0;
        bool ok = was_virtual ? xemu_dbg_to_phys(m_mem_addr, &translated)
                              : xemu_dbg_to_virt(m_mem_addr, &translated);
        if (ok) {
            m_mem_addr = translated;
            snprintf(m_mem_buf, sizeof(m_mem_buf), "%08X", m_mem_addr);
        } else {
            m_status = m_mem_virtual
                ? "that physical address is not mapped anywhere virtual"
                : "that virtual address is not mapped";
            m_status_ms = SDL_GetTicks();
        }
        m_mem_region = 0;
    }
    ImGui::SameLine();
    ImGui::TextDisabled(m_mem_virtual ? "(guest virtual)" : "(physical RAM)");
    ImGui::SameLine();
    ImGui::Checkbox("Big endian", &m_mem_big_endian);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(80 * g_viewport_mgr.m_scale);
    ImGui::SliderInt("bytes/row", &m_mem_bytes_per_row, 8, 32);

    if (!m_status.empty() && SDL_GetTicks() - m_status_ms < 5000) {
        ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.2f, 1.0f), "%s",
                           m_status.c_str());
    }

    // --- the dump ---
    const int per = m_mem_bytes_per_row;
    ImGui::BeginChild("##memdump",
                      ImVec2(0, 300.0f * g_viewport_mgr.m_scale), true);
    ImGui::PushFont(g_font_mgr.m_fixed_width_font);

    for (int row = 0; row < 24; row++) {
        uint32_t a = m_mem_addr + (uint32_t)(row * per);
        uint8_t buf[32];
        ssize_t got = xemu_dbg_read_space(a, buf, (size_t)per, m_mem_virtual);

        char hex[128] = { 0 };
        char asc[40] = { 0 };
        int p = 0, q = 0;
        for (int i = 0; i < per; i++) {
            // Big endian reverses within each 4-byte group, which is how the
            // external viewer presented it - handy when a value was found as
            // a dword rather than as bytes.
            int idx = m_mem_big_endian ? (i & ~3) + (3 - (i & 3)) : i;
            if (idx < got) {
                p += snprintf(hex + p, sizeof(hex) - p, "%02X ", buf[idx]);
                asc[q++] = (buf[idx] >= 0x20 && buf[idx] < 0x7F)
                           ? (char)buf[idx] : '.';
            } else {
                p += snprintf(hex + p, sizeof(hex) - p, "-- ");
                asc[q++] = ' ';
            }
        }
        asc[q] = 0;

        char line[200];
        snprintf(line, sizeof(line), "%08X  %s %s", a, hex, asc);
        if (ImGui::Selectable(line, m_mem_have_sel && m_mem_sel == a)) {
            m_mem_sel = a;
            m_mem_have_sel = true;
        }
    }

    ImGui::PopFont();
    ImGui::EndChild();

    if (ImGui::IsItemHovered()) {
        float wheel = ImGui::GetIO().MouseWheel;
        if (wheel != 0.0f) {
            m_mem_addr -= (uint32_t)(wheel * per * 3);
            snprintf(m_mem_buf, sizeof(m_mem_buf), "%08X", m_mem_addr);
        }
    }

    // Show the other space's address for whatever is selected: the single
    // most useful thing when moving between a cheat code and a dump.
    if (m_mem_have_sel) {
        uint32_t other = 0;
        bool ok = m_mem_virtual ? xemu_dbg_to_phys(m_mem_sel, &other)
                                : xemu_dbg_to_virt(m_mem_sel, &other);
        if (ok) {
            ImGui::Text("%08X %s = %08X %s", m_mem_sel,
                        m_mem_virtual ? "virtual" : "physical",
                        other, m_mem_virtual ? "physical" : "virtual");
        } else {
            ImGui::TextDisabled("%08X has no counterpart mapping", m_mem_sel);
        }
        ImGui::SameLine();
        if (ImGui::SmallButton("Disassemble here")) {
            GoTo(m_mem_sel, true);
        }
    }
}

void DisassemblerWindow::Draw()
{
    if (!m_is_open) {
        return;
    }

    /*
     * Size and clamp to the actual viewport rather than a fixed 900x600.
     * The old default could open wider than the window and push the register
     * pane off the right edge with no way to reach it. Also give a minimum
     * so the panes cannot be squeezed into nothing, and let the user resize
     * and move it freely afterwards.
     */
    const ImVec2 avail = ImGui::GetMainViewport()->WorkSize;
    ImVec2 want(900.0f * g_viewport_mgr.m_scale,
                600.0f * g_viewport_mgr.m_scale);
    if (want.x > avail.x * 0.95f) want.x = avail.x * 0.95f;
    if (want.y > avail.y * 0.95f) want.y = avail.y * 0.95f;

    ImGui::SetNextWindowSize(want, ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(avail.x * 0.5f, avail.y * 0.5f),
                            ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(
        ImVec2(520.0f * g_viewport_mgr.m_scale,
               320.0f * g_viewport_mgr.m_scale),
        avail);

    if (!ImGui::Begin("Disassembler", &m_is_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }

    if (!xemu_dbg_have_disasm()) {
        ImGui::TextWrapped(
            "This build has no disassembler. Rebuild with capstone enabled:\n\n"
            "    sudo apt install libcapstone-dev\n"
            "    ./pyvenv/bin/meson configure -Dcapstone=enabled   (in build/)");
        ImGui::End();
        return;
    }

    /*
     * Decide once per frame whether anything may re-read guest state. With
     * Live off, or inside the interval, the panes draw from the last snapshot
     * and cost nothing.
     */
    uint32_t now = SDL_GetTicks();
    if (m_live && (now - m_last_poll_ms) >= (uint32_t)m_interval_ms) {
        m_last_poll_ms = now;
        m_regs_fresh = false;
        m_poll_now = true;
    } else {
        m_poll_now = false;
    }

    DrawToolbar();
    ImGui::Separator();

    // Registers take a share of the width with a floor, so neither pane can
    // starve the other or spill outside the window.
    float total = ImGui::GetContentRegionAvail().x;
    float reg_w = total * 0.24f;
    float reg_min = 170.0f * g_viewport_mgr.m_scale;
    if (reg_w < reg_min) reg_w = reg_min;
    if (reg_w > total * 0.45f) reg_w = total * 0.45f;

    ImGui::BeginChild("##left",
                      ImVec2(total - reg_w - ImGui::GetStyle().ItemSpacing.x,
                             0),
                      false);
    /*
     * Collapsible sections rather than tabs, matching xemu's own Video Debug
     * window: everything is reachable at once and you fold away what you are
     * not using, instead of losing sight of it behind a tab.
     */
    if (ImGui::CollapsingHeader("Disassembly", ImGuiTreeNodeFlags_DefaultOpen)) {
        DrawDisassembly();
    }
    if (ImGui::CollapsingHeader("Memory")) {
        DrawMemory();
    }
    if (ImGui::CollapsingHeader("Breakpoints")) {
        DrawBreakpoints();
    }
    ImGui::EndChild();

    ImGui::SameLine();

    ImGui::BeginChild("##regs", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    DrawRegisters();
    ImGui::EndChild();

    ImGui::End();
}
