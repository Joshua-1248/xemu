// SPDX-License-Identifier: GPL-2.0-or-later
//
// xemu User Interface - Disassembler / CPU debugger
//
// Features #5 refresh: the window is now shaped around the user's external
// Xemu Cheat Engine workflow, but uses direct in-process Xemu debug APIs.
// Reserved cheat code Type F is intentionally not touched: patch helpers emit
// the already-supported ordinary virtual write codes (8/9/A).

#include <algorithm>
#include <cctype>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "disassembler.hh"
#include "xemu-features/shared/detachable-windows.hh"
#include "ui/xui/common.hh"
#include "ui/xui/font-manager.hh"
#include "ui/xui/misc.hh"
#include "ui/xui/viewport-manager.hh"

#include "xemu-features/debug-tools/debug-api.h"
#include "xemu-features/cheats/debug-bridge.hh"
#include "xemu-features/shared/guest-memory.h"

DisassemblerWindow disassembler_window;

#define BP_MEM_READ  0x01
#define BP_MEM_WRITE 0x02

static const int kLineCount = 256;

namespace {

static unsigned char AsciiLower(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ? (unsigned char)(c + ('a' - 'A')) : c;
}

static bool ContainsI(const std::string &hay, const char *needle)
{
    if (!needle || !*needle) return true;
    const size_t needle_len = strlen(needle);
    if (needle_len > hay.size()) return false;

    /* Function filtering runs while drawing the debugger. Avoid constructing
     * lower-cased copies of the function name, note and query for every entry
     * in a large symbol index. Xbox symbols/search text are ASCII here. */
    for (size_t i = 0; i + needle_len <= hay.size(); ++i) {
        size_t j = 0;
        while (j < needle_len &&
               AsciiLower((unsigned char)hay[i + j]) ==
               AsciiLower((unsigned char)needle[j])) {
            ++j;
        }
        if (j == needle_len) return true;
    }
    return false;
}

static bool ContainsI(const char *hay, const char *needle)
{
    if (!hay) return false;
    if (!needle || !*needle) return true;
    const size_t hay_len = strlen(hay);
    const size_t needle_len = strlen(needle);
    if (needle_len > hay_len) return false;
    for (size_t i = 0; i + needle_len <= hay_len; ++i) {
        size_t j = 0;
        while (j < needle_len &&
               AsciiLower((unsigned char)hay[i + j]) ==
               AsciiLower((unsigned char)needle[j])) {
            ++j;
        }
        if (j == needle_len) return true;
    }
    return false;
}

static uint32_t ReadLe32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static std::string HexBytes(const uint8_t *p, size_t n)
{
    std::string s;
    char tmp[8];
    for (size_t i = 0; i < n; ++i) {
        if (i) s += ' ';
        snprintf(tmp, sizeof(tmp), "%02X", p[i]);
        s += tmp;
    }
    return s;
}

static bool ParseHexBytes(const char *text, std::vector<uint8_t> *out)
{
    out->clear();
    if (!text) return false;
    const char *p = text;
    while (*p) {
        while (*p && (std::isspace((unsigned char)*p) || *p == ',' || *p == ';')) ++p;
        if (!*p) break;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        if (!std::isxdigit((unsigned char)p[0])) return false;
        char h[3] = {0, 0, 0};
        h[0] = *p++;
        if (std::isxdigit((unsigned char)*p)) h[1] = *p++;
        else { h[1] = h[0]; h[0] = '0'; }
        char *end = nullptr;
        unsigned long v = strtoul(h, &end, 16);
        if (end != h + 2 || v > 0xFF) return false;
        out->push_back((uint8_t)v);
    }
    return !out->empty();
}

static bool IsRetMnemonic(const char *m)
{
    return m && (strcmp(m, "ret") == 0 || strcmp(m, "retn") == 0 ||
                 strcmp(m, "retf") == 0);
}

static bool IsCallMnemonic(const char *m)
{
    return m && strncmp(m, "call", 4) == 0;
}

static bool IsFlowMnemonic(const char *m)
{
    if (!m || !*m) return false;
    if (m[0] == 'j') return true;
    return IsRetMnemonic(m) || strcmp(m, "loop") == 0 ||
           strcmp(m, "loope") == 0 || strcmp(m, "loopne") == 0;
}

static int32_t Signed32(uint32_t v)
{
    return (int32_t)v;
}

static float AsFloat(uint32_t v)
{
    float f;
    memcpy(&f, &v, sizeof(f));
    return f;
}

static int HexNibble(ImWchar c)
{
    if (c >= '0' && c <= '9') return (int)(c - '0');
    if (c >= 'a' && c <= 'f') return 10 + (int)(c - 'a');
    if (c >= 'A' && c <= 'F') return 10 + (int)(c - 'A');
    return -1;
}

static void SetClipboard(const std::string &text)
{
    ImGui::SetClipboardText(text.c_str());
}

static std::string FormatRamSize(uint64_t bytes)
{
    static constexpr uint64_t kMiB = 1024ULL * 1024ULL;
    char tmp[64];
    if (bytes != 0 && (bytes % kMiB) == 0) {
        snprintf(tmp, sizeof(tmp), "%" PRIu64 " MiB", bytes / kMiB);
    } else {
        snprintf(tmp, sizeof(tmp), "%" PRIu64 " bytes", bytes);
    }
    return tmp;
}

static bool DumpPhysicalRamToPath(const char *path, uint64_t *dumped_size,
                                  std::string *error)
{
    if (!path || !*path) {
        if (error) *error = "no output path selected";
        return false;
    }

    const uint64_t ram_size = xemu_guest_ram_size();
    if (ram_size == 0) {
        if (error) *error = "guest RAM size is unavailable";
        return false;
    }

    FILE *fp = fopen(path, "wb");
    if (!fp) {
        if (error) *error = "could not open output file";
        return false;
    }

    // Stream the dump rather than allocating an entire 64/128/256 MiB image.
    // This remains bounded even if future Xbox RAM configurations grow again.
    static constexpr size_t kChunkSize = 4 * 1024 * 1024;
    std::vector<uint8_t> chunk(kChunkSize);
    uint64_t offset = 0;

    while (offset < ram_size) {
        const size_t want = (size_t)std::min<uint64_t>(
            (uint64_t)chunk.size(), ram_size - offset);
        const ssize_t got = xemu_phys_read(offset, chunk.data(), want);
        if (got != (ssize_t)want) {
            fclose(fp);
            std::remove(path);
            if (error) {
                char tmp[128];
                snprintf(tmp, sizeof(tmp),
                         "physical RAM read failed at 0x%08" PRIX64, offset);
                *error = tmp;
            }
            return false;
        }
        if (fwrite(chunk.data(), 1, want, fp) != want) {
            fclose(fp);
            std::remove(path);
            if (error) *error = "could not write complete RAM dump";
            return false;
        }
        offset += want;
    }

    const bool flush_ok = fflush(fp) == 0;
    const bool close_ok = fclose(fp) == 0;
    if (!flush_ok || !close_ok) {
        std::remove(path);
        if (error) *error = "could not finalize RAM dump";
        return false;
    }

    if (dumped_size) *dumped_size = ram_size;
    return true;
}

} // namespace

DisassemblerWindow::DisassemblerWindow() : m_is_open(false)
{
    // These collections have tight natural bounds. Reserving once avoids the
    // first-use allocation churn while keeping the data structures obvious.
    m_lines.reserve(kLineCount);
    m_history.reserve(256);
    m_selected_instructions.reserve(kLineCount);
}

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
    if (!r.valid) return false;
    *out = r.eip;
    return true;
}

uint32_t DisassemblerWindow::BackUp(uint32_t addr, int instructions) const
{
    const uint32_t probe = (uint32_t)(instructions * 8);
    if (addr < probe) return 0;

    for (uint32_t back = probe; back >= (uint32_t)instructions; --back) {
        uint32_t pc = addr - back;
        XemuDbgInsn tmp[64];
        int n = xemu_dbg_disasm(pc, std::min(instructions * 2, 64), tmp);
        for (int i = 0; i < n; ++i) {
            if (tmp[i].addr == addr) {
                int idx = i - instructions;
                return tmp[idx < 0 ? 0 : idx].addr;
            }
            if (tmp[i].addr > addr) break;
        }
    }
    return addr - (uint32_t)instructions;
}

void DisassemblerWindow::GoTo(uint32_t addr, bool centre, bool push_history)
{
    if (push_history && m_have_selection && m_selected != addr) {
        if (m_history.empty() || m_history.back() != m_selected)
            m_history.push_back(m_selected);
        if (m_history.size() > 256) m_history.erase(m_history.begin());
    }
    m_base = centre ? BackUp(addr, 8) : addr;
    SelectOnlyInstruction(addr);
    m_dirty = true;
}

void DisassemblerWindow::Back()
{
    if (m_history.empty()) {
        m_status = "navigation history is empty";
        m_status_ms = SDL_GetTicks();
        return;
    }
    uint32_t addr = m_history.back();
    m_history.pop_back();
    GoTo(addr, true, false);
}

void DisassemblerWindow::Refresh()
{
    if (!m_dirty && m_base == m_last_base && !m_poll_now) return;
    m_lines.clear();

    XemuDbgInsn buf[kLineCount];
    int n = xemu_dbg_disasm(m_base, kLineCount, buf);
    m_lines.reserve(n);
    for (int i = 0; i < n; ++i) {
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

const DisassemblerWindow::Line *DisassemblerWindow::SelectedLine() const
{
    if (!m_have_selection) return nullptr;
    for (const auto &l : m_lines) if (l.addr == m_selected) return &l;
    return nullptr;
}

bool DisassemblerWindow::IsInstructionSelected(uint32_t addr) const
{
    return std::find(m_selected_instructions.begin(),
                     m_selected_instructions.end(), addr) !=
           m_selected_instructions.end();
}

void DisassemblerWindow::SelectOnlyInstruction(uint32_t addr)
{
    m_selected_instructions.clear();
    m_selected_instructions.push_back(addr);
    m_selected = addr;
    m_selection_anchor = addr;
    m_have_selection = true;
    snprintf(m_goto_buf, sizeof(m_goto_buf), "%08X", addr);
}

void DisassemblerWindow::ToggleInstructionSelection(uint32_t addr)
{
    auto it = std::find(m_selected_instructions.begin(),
                        m_selected_instructions.end(), addr);
    if (it == m_selected_instructions.end()) {
        m_selected_instructions.push_back(addr);
        m_selected = addr;
        m_selection_anchor = addr;
        m_have_selection = true;
    } else {
        m_selected_instructions.erase(it);
        if (m_selected_instructions.empty()) {
            m_have_selection = false;
            m_selected = 0;
            m_selection_anchor = 0;
        } else {
            m_selected = m_selected_instructions.back();
            m_have_selection = true;
        }
    }
    snprintf(m_goto_buf, sizeof(m_goto_buf), "%08X", addr);
}

void DisassemblerWindow::SelectInstructionRange(uint32_t addr)
{
    if (!m_have_selection || m_selection_anchor == 0) {
        SelectOnlyInstruction(addr);
        return;
    }

    int anchor_idx = -1;
    int addr_idx = -1;
    for (size_t i = 0; i < m_lines.size(); ++i) {
        if (m_lines[i].addr == m_selection_anchor) anchor_idx = (int)i;
        if (m_lines[i].addr == addr) addr_idx = (int)i;
    }
    if (anchor_idx < 0 || addr_idx < 0) {
        SelectOnlyInstruction(addr);
        return;
    }

    const int lo = std::min(anchor_idx, addr_idx);
    const int hi = std::max(anchor_idx, addr_idx);
    m_selected_instructions.clear();
    m_selected_instructions.reserve((size_t)(hi - lo + 1));
    for (int i = lo; i <= hi; ++i) {
        m_selected_instructions.push_back(m_lines[(size_t)i].addr);
    }
    m_selected = addr;
    m_have_selection = true;
    snprintf(m_goto_buf, sizeof(m_goto_buf), "%08X", addr);
}

std::vector<const DisassemblerWindow::Line *>
DisassemblerWindow::SelectedLines() const
{
    std::vector<const Line *> out;
    if (!m_have_selection) return out;
    for (const auto &l : m_lines) {
        if (IsInstructionSelected(l.addr)) out.push_back(&l);
    }
    std::sort(out.begin(), out.end(), [](const Line *a, const Line *b) {
        return a->addr < b->addr;
    });
    return out;
}

void DisassemblerWindow::CopySelectedInstructions()
{
    auto lines = SelectedLines();
    if (lines.empty()) return;

    std::string text;
    char ab[16];
    for (const Line *line : lines) {
        snprintf(ab, sizeof(ab), "%08X", line->addr);
        std::string bytes = HexBytes(line->bytes, line->len);
        std::string asmtext = std::string(line->mnemonic) +
                              (line->ops[0] ? " " : "") + line->ops;
        text += ab;
        text += "  ";
        text += bytes;
        if (bytes.size() < 30) text.append(30 - bytes.size(), ' ');
        text += "  ";
        text += asmtext;
        text += '\n';
    }
    if (!text.empty()) text.pop_back();
    SetClipboard(text);
    m_status = lines.size() == 1 ? "copied 1 instruction" :
        "copied " + std::to_string(lines.size()) + " instructions";
    m_status_ms = SDL_GetTicks();
}

void DisassemblerWindow::CopySelectedBytes()
{
    auto lines = SelectedLines();
    if (lines.empty()) return;

    std::string text;
    for (const Line *line : lines) {
        if (!text.empty()) text += ' ';
        text += HexBytes(line->bytes, line->len);
    }
    SetClipboard(text);
    m_status = "copied selected instruction bytes";
    m_status_ms = SDL_GetTicks();
}

void DisassemblerWindow::CopySelectedNopCheats()
{
    auto lines = SelectedLines();
    if (lines.empty()) return;

    std::string all;
    for (const Line *line : lines) {
        std::vector<uint8_t> nops(line->len, 0x90);
        std::string codes = GenerateVirtualWriteCodes(line->addr, nops);
        if (codes.empty()) {
            m_status = "one or more selected addresses cannot be represented by virtual write codes";
            m_status_ms = SDL_GetTicks();
            return;
        }
        if (!all.empty()) all += '\n';
        all += codes;
    }
    SetClipboard(all);
    m_status = "copied selected NOP patch as virtual write cheat codes";
    m_status_ms = SDL_GetTicks();
}

bool DisassemblerWindow::ParseBranchTarget(const Line &line, uint32_t *target)
{
    if (!(IsCallMnemonic(line.mnemonic) || line.mnemonic[0] == 'j')) return false;
    const char *p = strstr(line.ops, "0x");
    if (!p) return false;
    char *end = nullptr;
    unsigned long v = strtoul(p + 2, &end, 16);
    if (end == p + 2) return false;
    *target = (uint32_t)v;
    return true;
}

bool DisassemblerWindow::HasBreakpoint(uint32_t addr) const
{
    for (const auto &b : m_bps) if (b.addr == addr) return true;
    return false;
}

void DisassemblerWindow::ToggleBreakpoint(uint32_t addr)
{
    for (size_t i = 0; i < m_bps.size(); ++i) {
        if (m_bps[i].addr != addr) continue;
        xemu_dbg_bp_remove_space(addr, true);
        m_bps.erase(m_bps.begin() + i);
        return;
    }
    if (xemu_dbg_bp_insert_space(addr, true)) m_bps.push_back({addr, true, true});
}

void DisassemblerWindow::ScanFunctions()
{
    std::string status;
    if (m_functions.Scan(&status)) {
        m_functions_scanned = true;
        m_visible_functions_dirty = true;
        m_status = status;
    } else {
        m_status = status.empty() ? "function scan failed" : status;
    }
    m_status_ms = SDL_GetTicks();
}

void DisassemblerWindow::ImportSymbolsDialog()
{
    SDL_DialogFileFilter filters[] = {
        { "Symbol / map files", "map;sym;txt;symbols" },
        { "All files", "*" },
    };
    ShowOpenFileDialog(filters, 2, nullptr, [this](const char *path) {
        if (!path || !*path) return;
        if (!m_functions_scanned) {
            std::string scan_status;
            if (m_functions.Scan(&scan_status)) {
                m_functions_scanned = true;
                m_visible_functions_dirty = true;
            }
        }
        std::string st;
        m_functions.ImportSymbols(path, &st);
        m_visible_functions_dirty = true;
        m_status = st;
        m_status_ms = SDL_GetTicks();
        m_dirty = true;
    });
}

bool DisassemblerWindow::FunctionVisible(const XemuDbgFunctionEntry &entry) const
{
    switch (m_func_filter) {
    case 1: // named only
        if (entry.name.rfind("sub_", 0) == 0) return false;
        break;
    case 2:
        if (entry.source != XemuDbgFunctionSource::Symbol) return false;
        break;
    case 3:
        if (entry.source != XemuDbgFunctionSource::Rtti) return false;
        break;
    case 4:
        if (entry.source == XemuDbgFunctionSource::Symbol ||
            entry.source == XemuDbgFunctionSource::Rtti) return false;
        break;
    default:
        break;
    }
    if (!m_func_search[0]) return true;
    char a[16];
    snprintf(a, sizeof(a), "%08X", entry.addr);
    return ContainsI(entry.name, m_func_search) || ContainsI(a, m_func_search) ||
           ContainsI(entry.note, m_func_search);
}

void DisassemblerWindow::RebuildVisibleFunctions()
{
    m_visible_functions.clear();
    m_visible_functions.reserve(m_functions.Entries().size());
    for (const auto &entry : m_functions.Entries()) {
        if (FunctionVisible(entry)) {
            m_visible_functions.push_back(&entry);
        }
    }
    m_visible_functions_dirty = false;
}

void DisassemblerWindow::FormatFunctionDisplay(const XemuDbgFunctionEntry &entry,
                                                char *out, size_t out_size)
{
    if (!out || out_size == 0) return;
    const char *source = XemuDbgFunctionIndex::SourceLabel(entry.source);
    if (entry.xrefs && !entry.note.empty()) {
        snprintf(out, out_size, "%08X  %s  [%s, %u xref%s, %s]",
                 entry.addr, entry.name.c_str(), source, entry.xrefs,
                 entry.xrefs == 1 ? "" : "s", entry.note.c_str());
    } else if (entry.xrefs) {
        snprintf(out, out_size, "%08X  %s  [%s, %u xref%s]",
                 entry.addr, entry.name.c_str(), source, entry.xrefs,
                 entry.xrefs == 1 ? "" : "s");
    } else if (!entry.note.empty()) {
        snprintf(out, out_size, "%08X  %s  [%s, %s]",
                 entry.addr, entry.name.c_str(), source, entry.note.c_str());
    } else {
        snprintf(out, out_size, "%08X  %s  [%s]",
                 entry.addr, entry.name.c_str(), source);
    }
}

std::string DisassemblerWindow::NearestName(uint32_t addr) const
{
    const auto *e = m_functions.Nearest(addr);
    if (!e) return {};
    if (e->addr == addr) return e->name;
    char tmp[512];
    snprintf(tmp, sizeof(tmp), "%s + 0x%X", e->name.c_str(), addr - e->addr);
    return tmp;
}

std::string DisassemblerWindow::DescribePointer(uint32_t value) const
{
    if (!value) return {};
    if (const auto *sec = m_functions.SectionOf(value)) {
        std::string n = NearestName(value);
        if (!n.empty() && (sec->name == ".text" || sec->name == ".text?"))
            return n;
        char tmp[128];
        snprintf(tmp, sizeof(tmp), "%s + 0x%X", sec->name.c_str(), value - sec->lo);
        return tmp;
    }
    if (value >= 0x80000000 && value < 0xDFFFFFFF) return "kernel";
    uint32_t pa = 0;
    if (xemu_dbg_to_phys(value, &pa)) {
        char tmp[64]; snprintf(tmp, sizeof(tmp), "mapped -> PA %08X", pa);
        return tmp;
    }
    return {};
}

void DisassemblerWindow::DrawNavigationBar()
{
    ImGui::SetNextItemWidth(125 * g_viewport_mgr.m_scale);
    if (ImGui::InputText("Address", m_goto_buf, sizeof(m_goto_buf),
                         ImGuiInputTextFlags_CharsHexadecimal |
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        GoTo((uint32_t)strtoul(m_goto_buf, nullptr, 16), false);
    }
    ImGui::SameLine();
    if (ImGui::Button("Go")) GoTo((uint32_t)strtoul(m_goto_buf, nullptr, 16), false);
    ImGui::SameLine();
    ImGui::BeginDisabled(m_history.empty());
    if (ImGui::Button("<- Back")) Back();
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Rescan functions")) ScanFunctions();
    ImGui::SameLine();
    if (ImGui::Button("Import symbols...")) ImportSymbolsDialog();
    ImGui::SameLine();
    if (ImGui::Button("Dump RAM...")) {
        static const SDL_DialogFileFilter filters[] = {
            { "Raw RAM dump (*.bin)", "bin" },
            { "All files", "*" },
        };
        ShowSaveFileDialog(filters, 2, nullptr, [this](const char *path) {
            if (!path || !*path) return;
            uint64_t dumped_size = 0;
            std::string error;
            if (DumpPhysicalRamToPath(path, &dumped_size, &error)) {
                m_status = "RAM dump saved (" + FormatRamSize(dumped_size) + "): " + path;
            } else {
                m_status = "RAM dump failed: " + error;
            }
            m_status_ms = SDL_GetTicks();
        });
    }
    if (ImGui::IsItemHovered()) {
        const std::string ram_size = FormatRamSize(xemu_guest_ram_size());
        ImGui::SetTooltip("Dump all detected physical Xbox RAM (%s)",
                          ram_size.c_str());
    }
    ImGui::SameLine();
    ImGui::Checkbox("Follow EIP", &m_follow_eip);

    if (!m_status.empty() && SDL_GetTicks() - m_status_ms < 7000) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.65f, 1.0f), "%s", m_status.c_str());
    }
}

void DisassemblerWindow::DrawDebugBar()
{
    bool running = xemu_dbg_is_running();

    if (ImGui::Button(running ? "Break" : "Run (F5)")) {
        if (running) xemu_dbg_pause(); else xemu_dbg_resume();
        m_dirty = true; m_regs_fresh = false;
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(running);
    if (ImGui::Button("Step Into (F11)")) { xemu_dbg_step(); m_dirty = true; }
    ImGui::SameLine();
    if (ImGui::Button("Step Over (F10)")) { xemu_dbg_step_over(); m_dirty = true; }
    ImGui::SameLine();
    if (ImGui::Button("Step Out (Shift+F11)")) {
        if (const char *err = xemu_dbg_step_out()) {
            m_status = err; m_status_ms = SDL_GetTicks();
        }
        m_dirty = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Run to cursor")) {
        if (m_have_selection) xemu_dbg_run_to(m_selected);
        else { m_status = "select an instruction first"; m_status_ms = SDL_GetTicks(); }
    }
    ImGui::SameLine();
    if (ImGui::Button("Go to EIP")) {
        uint32_t eip; if (ReadEip(&eip)) GoTo(eip, true);
    }
    ImGui::SameLine();
    if (ImGui::Button("Add Breakpoint...")) {
        if (m_have_selection) ToggleBreakpoint(m_selected);
        else { m_status = "select an instruction first"; m_status_ms = SDL_GetTicks(); }
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::BeginDisabled(!m_have_selection);
    if (ImGui::Button("NOP Selected")) NopSelected();
    ImGui::SameLine();
    if (ImGui::Button("Copy Selected")) CopySelectedInstructions();
    ImGui::EndDisabled();

#ifdef CONFIG_XEMU_FEATURE_CHEATS
    ImGui::SameLine();
    bool selected_patch = m_have_selection && FindPatch(m_selected) != nullptr;
    ImGui::BeginDisabled(!selected_patch);
    if (ImGui::Button("Save ASM Cheat...")) {
        OpenCreateCheatModal(m_selected);
    }
    ImGui::EndDisabled();
#endif

    ImGui::SameLine();
    ImGui::Checkbox("Live", &m_live);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(85 * g_viewport_mgr.m_scale);
    ImGui::SliderInt("ms", &m_interval_ms, 1, 1000);
    if (m_interval_ms < 1) m_interval_ms = 1;
    if (m_interval_ms > 1000) m_interval_ms = 1000;
    ImGui::SameLine();
    if (ImGui::Button("Refresh")) {
        m_dirty = true; m_regs_fresh = false; m_mem_cache_valid = false;
    }
    ImGui::SameLine();
    ImGui::TextDisabled(running ? "running" : "paused");

    // Keyboard shortcuts only when the debugger window has focus and no text
    // box is actively eating keyboard input.
    if (!ImGui::GetIO().WantTextInput && ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        if (ImGui::IsKeyPressed(ImGuiKey_F5)) {
            if (running) xemu_dbg_pause(); else xemu_dbg_resume();
        }
        if (!running && ImGui::IsKeyPressed(ImGuiKey_F10)) xemu_dbg_step_over();
        if (!running && ImGui::IsKeyPressed(ImGuiKey_F11)) {
            if (ImGui::GetIO().KeyShift) xemu_dbg_step_out();
            else xemu_dbg_step();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_F9) && m_have_selection) ToggleBreakpoint(m_selected);
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C) &&
            m_have_selection) {
            CopySelectedInstructions();
        }
    }
}

void DisassemblerWindow::DrawFunctionBrowser(float width)
{
    ImGui::BeginChild("##functions", ImVec2(width, 0), true);
    ImGui::SetNextItemWidth(-1);
    if (ImGui::InputTextWithHint("##funcsearch", "Search functions / addresses...",
                                 m_func_search, sizeof(m_func_search))) {
        m_visible_functions_dirty = true;
    }

    ImGui::TextDisabled("Show:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(125 * g_viewport_mgr.m_scale);
    const char *filters[] = { "All", "Named only", "Symbols", "RTTI", "Detected" };
    if (ImGui::Combo("##funcfilter", &m_func_filter, filters, IM_ARRAYSIZE(filters))) {
        m_visible_functions_dirty = true;
    }

    if (m_visible_functions_dirty) {
        RebuildVisibleFunctions();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%zu / %zu", m_visible_functions.size(),
                        m_functions.Entries().size());
    ImGui::Separator();

    if (!m_functions_scanned) {
        ImGui::TextWrapped("Press Rescan functions to build the call/prologue/RTTI index for this title.");
    } else {
        ImGuiListClipper clipper;
        clipper.Begin((int)m_visible_functions.size());
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const auto &e = *m_visible_functions[(size_t)i];
                char label[512];
                FormatFunctionDisplay(e, label, sizeof(label));
                ImGui::PushID((int)e.addr);
                bool sel = m_have_selection && m_selected == e.addr;
                if (ImGui::Selectable(label, sel)) GoTo(e.addr, false);
                if (ImGui::BeginPopupContextItem("##fctx")) {
                    if (ImGui::MenuItem("Go to function")) GoTo(e.addr, false);
                    const auto *callers = m_functions.CallersOf(e.addr);
                    bool have = callers && !callers->empty();
                    ImGui::BeginDisabled(!have);
                    if (ImGui::MenuItem("Show call xrefs")) {
                        m_xref_target = e.addr; m_open_xref_popup = true;
                    }
                    ImGui::EndDisabled();
                    if (ImGui::MenuItem("Copy address")) {
                        char b[16]; snprintf(b, sizeof(b), "%08X", e.addr); SetClipboard(b);
                    }
                    ImGui::EndPopup();
                }
                ImGui::PopID();
            }
        }
    }
    ImGui::EndChild();
}

DisassemblerWindow::CodePatch *DisassemblerWindow::FindPatch(uint32_t addr)
{
    for (auto &p : m_patches) if (p.addr == addr) return &p;
    return nullptr;
}

const DisassemblerWindow::CodePatch *DisassemblerWindow::FindPatch(uint32_t addr) const
{
    for (const auto &p : m_patches) if (p.addr == addr) return &p;
    return nullptr;
}

bool DisassemblerWindow::RangeOverlapsPatch(uint32_t addr, size_t len,
                                            const CodePatch **hit) const
{
    uint64_t a0 = addr, a1 = a0 + len;
    for (const auto &p : m_patches) {
        uint64_t b0 = p.addr, b1 = b0 + p.patched.size();
        if (a0 < b1 && b0 < a1) {
            if (hit) *hit = &p;
            return true;
        }
    }
    return false;
}

bool DisassemblerWindow::ApplyPatch(uint32_t addr,
                                    const std::vector<uint8_t> &bytes,
                                    std::string *error)
{
    if (bytes.empty()) { if (error) *error = "empty patch"; return false; }
    CodePatch *existing = FindPatch(addr);
    if (!existing) {
        const CodePatch *overlap = nullptr;
        if (RangeOverlapsPatch(addr, bytes.size(), &overlap)) {
            if (error) *error = "patch overlaps an existing patch; undo it first";
            return false;
        }
        CodePatch p;
        p.addr = addr;
        p.original.resize(bytes.size());
        if (xemu_dbg_read_space(addr, p.original.data(), p.original.size(), true) !=
            (ssize_t)p.original.size()) {
            if (error) *error = "could not read original bytes";
            return false;
        }
        p.patched = bytes;
        if (xemu_dbg_write_space(addr, bytes.data(), bytes.size(), true) != (ssize_t)bytes.size()) {
            if (error) *error = "guest write failed";
            return false;
        }
        m_patches.push_back(std::move(p));
    } else {
        if (bytes.size() != existing->original.size()) {
            if (error) *error = "changing patch length would make undo ambiguous; undo it first";
            return false;
        }
        if (xemu_dbg_write_space(addr, bytes.data(), bytes.size(), true) != (ssize_t)bytes.size()) {
            if (error) *error = "guest write failed";
            return false;
        }
        existing->patched = bytes;
    }
    m_dirty = true; m_mem_cache_valid = false;
    return true;
}

bool DisassemblerWindow::UndoPatch(uint32_t addr, std::string *error)
{
    for (size_t i = 0; i < m_patches.size(); ++i) {
        if (m_patches[i].addr != addr) continue;
        auto &p = m_patches[i];
        if (xemu_dbg_write_space(p.addr, p.original.data(), p.original.size(), true) !=
            (ssize_t)p.original.size()) {
            if (error) *error = "restore write failed";
            return false;
        }
        m_patches.erase(m_patches.begin() + i);
        m_dirty = true; m_mem_cache_valid = false;
        return true;
    }
    if (error) *error = "no patch recorded at that address";
    return false;
}

void DisassemblerWindow::UndoAllPatches()
{
    size_t failed = 0;
    for (size_t i = m_patches.size(); i-- > 0;) {
        auto &p = m_patches[i];
        if (xemu_dbg_write_space(p.addr, p.original.data(), p.original.size(), true) ==
            (ssize_t)p.original.size()) {
            m_patches.erase(m_patches.begin() + i);
        } else {
            failed++;
        }
    }
    m_dirty = true; m_mem_cache_valid = false;
    m_status = failed ? "some patches could not be restored" : "restored all debugger patches";
    m_status_ms = SDL_GetTicks();
}

void DisassemblerWindow::OpenPatchModal(uint32_t addr)
{
    const Line *line = nullptr;
    for (const auto &l : m_lines) if (l.addr == addr) { line = &l; break; }
    if (!line || !line->valid || !line->len) return;
    m_patch_addr = addr;
    std::vector<uint8_t> cur(line->len);
    if (xemu_dbg_read_space(addr, cur.data(), cur.size(), true) != (ssize_t)cur.size()) return;
    std::string s = HexBytes(cur.data(), cur.size());
    snprintf(m_patch_bytes, sizeof(m_patch_bytes), "%s", s.c_str());
    m_patch_modal_requested = true;
}

void DisassemblerWindow::NopSelected()
{
    auto lines = SelectedLines();
    if (lines.empty()) return;

    size_t done = 0;
    size_t bytes_done = 0;
    std::string first_error;
    for (const Line *line : lines) {
        if (!line->valid || !line->len) continue;
        std::vector<uint8_t> nops(line->len, 0x90);
        std::string err;
        if (ApplyPatch(line->addr, nops, &err)) {
            ++done;
            bytes_done += line->len;
        } else if (first_error.empty()) {
            first_error = err;
        }
    }

    if (done == lines.size()) {
        m_status = "NOPed " + std::to_string(done) + " instruction(s), " +
                   std::to_string(bytes_done) + " byte(s)";
    } else {
        m_status = "NOPed " + std::to_string(done) + "/" +
                   std::to_string(lines.size()) + " selected instruction(s)";
        if (!first_error.empty()) m_status += ": " + first_error;
    }
    m_status_ms = SDL_GetTicks();
}

std::vector<std::pair<uint32_t, uint32_t>>
DisassemblerWindow::GenerateVirtualWriteCodePairs(
    uint32_t addr, const std::vector<uint8_t> &bytes) const
{
    std::vector<std::pair<uint32_t, uint32_t>> out;
    if (bytes.empty()) return out;
    if (addr > 0x0FFFFFFF || (uint64_t)addr + bytes.size() - 1 > 0x0FFFFFFF)
        return out;

    size_t i = 0;
    while (i < bytes.size()) {
        size_t left = bytes.size() - i;
        uint32_t a = addr + (uint32_t)i;
        if (left >= 4) {
            out.push_back({0xA0000000u | a, ReadLe32(&bytes[i])});
            i += 4;
        } else if (left >= 2) {
            uint32_t v = (uint32_t)bytes[i] | ((uint32_t)bytes[i + 1] << 8);
            out.push_back({0x90000000u | a, v});
            i += 2;
        } else {
            out.push_back({0x80000000u | a, (uint32_t)bytes[i]});
            ++i;
        }
    }
    return out;
}

std::string DisassemblerWindow::GenerateVirtualWriteCodes(
    uint32_t addr, const std::vector<uint8_t> &bytes) const
{
    auto pairs = GenerateVirtualWriteCodePairs(addr, bytes);
    if (pairs.empty()) return {};
    std::string out;
    char line[64];
    for (const auto &code : pairs) {
        snprintf(line, sizeof(line), "%08X %08X\n", code.first, code.second);
        out += line;
    }
    if (!out.empty()) out.pop_back();
    return out;
}

void DisassemblerWindow::CopyPatchAsCheat(uint32_t addr)
{
    const CodePatch *p = FindPatch(addr);
    if (!p) {
        m_status = "no debugger patch recorded at this address";
    } else {
        std::string codes = GenerateVirtualWriteCodes(p->addr, p->patched);
        if (codes.empty()) {
            m_status = "this address cannot be represented by virtual write code types 8/9/A";
        } else {
            SetClipboard(codes);
            m_status = "copied patch as ordinary virtual write cheat codes";
        }
    }
    m_status_ms = SDL_GetTicks();
}

void DisassemblerWindow::CopyNopAsCheat(uint32_t addr, size_t len)
{
    std::vector<uint8_t> nops(len, 0x90);
    std::string codes = GenerateVirtualWriteCodes(addr, nops);
    if (codes.empty()) m_status = "address cannot be represented by virtual write code types 8/9/A";
    else { SetClipboard(codes); m_status = "copied NOP patch as virtual write cheat codes"; }
    m_status_ms = SDL_GetTicks();
}

void DisassemblerWindow::OpenCreateCheatModal(uint32_t addr)
{
    const CodePatch *patch = FindPatch(addr);
    if (!patch) {
        m_status = "apply/test a debugger patch first";
        m_status_ms = SDL_GetTicks();
        return;
    }
    if (GenerateVirtualWriteCodePairs(patch->addr, patch->patched).empty()) {
        m_status = "patch address cannot be represented by virtual write code types 8/9/A";
        m_status_ms = SDL_GetTicks();
        return;
    }

    m_create_cheat_addr = addr;
    std::string near = NearestName(addr);
    if (near.empty()) {
        snprintf(m_create_cheat_name, sizeof(m_create_cheat_name),
                 "Code patch 0x%08X", addr);
    } else {
        snprintf(m_create_cheat_name, sizeof(m_create_cheat_name),
                 "%s patch", near.c_str());
    }
    snprintf(m_create_cheat_desc, sizeof(m_create_cheat_desc),
             "Generated from the in-Xemu debugger at virtual address 0x%08X.",
             addr);
    m_create_cheat_enabled = true;
    m_create_cheat_requested = true;
}

bool DisassemblerWindow::TransferPatchToCheats(uint32_t addr,
                                                const char *name,
                                                const char *desc,
                                                bool enabled)
{
    const CodePatch *patch_ptr = FindPatch(addr);
    if (!patch_ptr) {
        m_status = "debugger patch disappeared before it could be saved";
        return false;
    }
    CodePatch patch = *patch_ptr;
    auto pairs = GenerateVirtualWriteCodePairs(patch.addr, patch.patched);
    if (pairs.empty()) {
        m_status = "patch cannot be represented by existing 8/9/A virtual writes";
        return false;
    }

    std::vector<uint32_t> cmds, vals;
    cmds.reserve(pairs.size());
    vals.reserve(pairs.size());
    for (const auto &code : pairs) {
        cmds.push_back(code.first);
        vals.push_back(code.second);
    }

    // The live debugger patch currently owns these bytes and knows their true
    // original value. Restore it before Cheats takes ownership. If we left the
    // patch in place, the [ASM] journal would incorrectly capture the patched
    // bytes as its "original" and disabling the cheat would restore nothing.
    std::string undo_error;
    if (!UndoPatch(addr, &undo_error)) {
        m_status = "could not hand patch to Cheats: " + undo_error;
        return false;
    }

    bool ok = FeatureCodesAddGeneratedAsmCheat(
        name, desc, cmds.data(), vals.data(), cmds.size(), enabled);
    if (!ok) {
        // Transactional handoff: if the optional Cheats side rejects the new
        // block, put the tested debugger patch back exactly as it was.
        std::string apply_error;
        if (!ApplyPatch(patch.addr, patch.patched, &apply_error)) {
            m_status = "Cheats handoff failed, and debugger patch rollback failed: " + apply_error;
        } else {
            m_status = "Cheats handoff failed; restored the live debugger patch";
        }
        return false;
    }

    m_status = enabled
        ? "saved under [ASM] and transferred live patch ownership to Cheats"
        : "saved under [ASM] disabled; original code restored";
    m_status_ms = SDL_GetTicks();
    return true;
}

void DisassemblerWindow::DrawDisassembly()
{
    uint32_t eip = 0;
    bool have_eip = ReadEip(&eip);
    if (m_follow_eip && have_eip && !xemu_dbg_is_running()) {
        bool visible = false;
        for (const auto &l : m_lines) {
            if (l.addr == eip) {
                visible = true;
                break;
            }
        }
        if (!visible) {
            GoTo(eip, true);
        }
    }
    Refresh();

    // This is an address-driven disassembly view.  Do not give ImGui a second
    // vertical scroll position inside the finite decoded line cache: that made
    // the user hit the end of a 96-line "document" even though the guest address
    // space continues.  The mouse wheel/Page keys now advance m_base and decode
    // the next/previous instructions, so scrolling can continue indefinitely.
    ImGui::BeginChild("##disasm", ImVec2(0, 0), true,
                      ImGuiWindowFlags_NoMove |
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);

    const bool disasm_hovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    // Table rows contain plain fixed-width text, not framed widgets.  Using
    // GetFrameHeightWithSpacing() here substantially overestimated each row
    // and caused the renderer to stop early, leaving a large black slab under
    // the final instruction.  Size against the actual text row height and add
    // a small clipped overscan so the code pane always fills to its bottom.
    const float row_h = ImGui::GetTextLineHeightWithSpacing();
    const float header_h = ImGui::GetFrameHeightWithSpacing();
    int visible_table_rows =
        (int)ceilf((ImGui::GetContentRegionAvail().y - header_h) /
                   std::max(1.0f, row_h)) + 3;
    if (visible_table_rows < 4) visible_table_rows = 4;
    if (visible_table_rows > kLineCount) visible_table_rows = kLineCount;

    int rendered_instructions = 0;
    if (ImGui::BeginTable("##codetable", 4,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_BordersInnerV |
                          ImGuiTableFlags_Resizable |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed,
                                28 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed,
                                90 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed,
                                175 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Instruction", ImGuiTableColumnFlags_WidthStretch);

        // Explicitly freeze the header contract.  The table itself no longer
        // owns vertical scrolling, so Address/Bytes/Instruction always remain
        // at the top while the decoded base address changes underneath them.
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();
        ImGui::PushFont(xemu_feature_detach::FixedWidthFont(g_font_mgr.m_fixed_width_font));

        int table_rows_used = 0;
        for (const auto &l : m_lines) {
            if (table_rows_used >= visible_table_rows) {
                break;
            }

            if (const auto *fn = m_functions.Exact(l.addr)) {
                if (table_rows_used + 1 >= visible_table_rows) {
                    break;
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(3);
                ImGui::TextColored(ImVec4(0.90f, 0.75f, 0.25f, 1.0f),
                                   "---- %s ----", fn->name.c_str());
                ++table_rows_used;
            }

            bool is_eip = have_eip && l.addr == eip;
            bool is_sel = IsInstructionSelected(l.addr);
            bool has_bp = HasBreakpoint(l.addr);
            bool patched = FindPatch(l.addr) != nullptr;

            ImGui::PushID((int)l.addr);
            ImGui::TableNextRow();
            ++table_rows_used;
            ++rendered_instructions;

            ImGui::TableSetColumnIndex(0);
            if (ImGui::SmallButton(has_bp ? "*" : " ")) {
                ToggleBreakpoint(l.addr);
            }

            ImGui::TableSetColumnIndex(1);
            char ab[16];
            snprintf(ab, sizeof(ab), "%08X", l.addr);
            if (is_eip) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(130, 255, 170, 255));
            } else if (has_bp) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 130, 130, 255));
            } else if (patched) {
                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 210, 90, 255));
            }
            ImGui::TextUnformatted(ab);
            if (is_eip || has_bp || patched) {
                ImGui::PopStyleColor();
            }

            ImGui::TableSetColumnIndex(2);
            char bytes[16 * 3] = {};
            char *bp = bytes;
            size_t left = sizeof(bytes);
            for (uint8_t bi = 0; bi < l.len && left > 1; ++bi) {
                int wrote = snprintf(bp, left, bi ? " %02X" : "%02X", l.bytes[bi]);
                if (wrote <= 0 || (size_t)wrote >= left) break;
                bp += wrote;
                left -= (size_t)wrote;
            }
            ImGui::TextUnformatted(bytes);

            ImGui::TableSetColumnIndex(3);
            char asmtext[sizeof(l.mnemonic) + sizeof(l.ops) + 2];
            snprintf(asmtext, sizeof(asmtext), "%s%s%s", l.mnemonic,
                     l.ops[0] ? " " : "", l.ops);
            ImVec4 col(0.78f, 0.88f, 0.65f, 1.0f);
            if (IsCallMnemonic(l.mnemonic)) {
                col = ImVec4(0.55f, 0.70f, 1.0f, 1.0f);
            } else if (IsFlowMnemonic(l.mnemonic)) {
                col = ImVec4(1.0f, 0.65f, 0.55f, 1.0f);
            } else if (!l.valid) {
                col = ImVec4(1.0f, 0.45f, 0.45f, 1.0f);
            }
            if (is_eip) col = ImVec4(0.55f, 1.0f, 0.65f, 1.0f);
            if (patched) col = ImVec4(1.0f, 0.82f, 0.35f, 1.0f);

            ImGui::PushStyleColor(ImGuiCol_Text, col);
            if (ImGui::Selectable(asmtext, is_sel,
                                  ImGuiSelectableFlags_SpanAllColumns)) {
                const ImGuiIO &io = ImGui::GetIO();
                if (io.KeyShift) {
                    SelectInstructionRange(l.addr);
                } else if (io.KeyCtrl) {
                    ToggleInstructionSelection(l.addr);
                } else {
                    SelectOnlyInstruction(l.addr);
                }
            }
            ImGui::PopStyleColor();

            /* Right-clicking an unselected row makes it the active selection;
             * right-clicking anywhere inside an existing multi-selection keeps
             * the whole selection intact for NOP/copy actions. */
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right) &&
                !IsInstructionSelected(l.addr)) {
                SelectOnlyInstruction(l.addr);
            }

            if (ImGui::IsItemHovered() &&
                ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                uint32_t t;
                if (ParseBranchTarget(l, &t)) {
                    GoTo(t, true);
                }
            }

            if (ImGui::BeginPopupContextItem("##insnctx")) {
                uint32_t target = 0;
                bool have_target = ParseBranchTarget(l, &target);
                ImGui::BeginDisabled(!have_target);
                if (ImGui::MenuItem("Follow branch/call target")) GoTo(target, true);
                ImGui::EndDisabled();
                const size_t selection_count = SelectedLines().size();
                if (selection_count > 1) {
                    if (ImGui::MenuItem("Copy selected instructions"))
                        CopySelectedInstructions();
                    if (ImGui::MenuItem("Copy selected bytes"))
                        CopySelectedBytes();
                } else {
                    if (ImGui::MenuItem("Copy address")) SetClipboard(ab);
                    if (ImGui::MenuItem("Copy instruction")) SetClipboard(asmtext);
                    if (ImGui::MenuItem("Copy bytes")) SetClipboard(bytes);
                }
                ImGui::Separator();
                if (ImGui::MenuItem(has_bp ? "Remove breakpoint (F9)"
                                           : "Set breakpoint (F9)")) {
                    ToggleBreakpoint(l.addr);
                }
                if (ImGui::MenuItem("Run to here")) xemu_dbg_run_to(l.addr);
                if (ImGui::MenuItem("Set EIP here")) {
                    if (!xemu_dbg_is_running() &&
                        xemu_dbg_set_reg("eip", l.addr)) {
                        m_regs_fresh = false;
                        m_dirty = true;
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem(selection_count > 1
                                        ? "NOP selected instructions now"
                                        : "NOP this instruction now")) {
                    NopSelected();
                }
                ImGui::BeginDisabled(selection_count > 1);
                if (ImGui::MenuItem("Patch bytes...")) OpenPatchModal(l.addr);
                ImGui::EndDisabled();
                bool have_patch = FindPatch(l.addr) != nullptr;
                ImGui::BeginDisabled(!have_patch);
                if (ImGui::MenuItem("Undo this patch")) {
                    std::string err;
                    if (!UndoPatch(l.addr, &err)) m_status = err;
                    else m_status = "restored original bytes";
                    m_status_ms = SDL_GetTicks();
                }
                if (ImGui::MenuItem("Copy current patch as cheat codes"))
                    CopyPatchAsCheat(l.addr);
#ifdef CONFIG_XEMU_FEATURE_CHEATS
                if (ImGui::MenuItem("Save current patch as [ASM] cheat..."))
                    OpenCreateCheatModal(l.addr);
#endif
                ImGui::EndDisabled();
                if (ImGui::MenuItem(selection_count > 1
                                        ? "Copy selected NOP cheat codes"
                                        : "Copy NOP cheat code")) {
                    if (selection_count > 1) CopySelectedNopCheats();
                    else CopyNopAsCheat(l.addr, l.len);
                }
                if (!m_patches.empty() &&
                    ImGui::MenuItem("Undo all debugger patches")) {
                    UndoAllPatches();
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Open in Memory viewer")) {
                    m_mem_addr = l.addr;
                    snprintf(m_mem_buf, sizeof(m_mem_buf), "%08X", l.addr);
                    m_mem_virtual = true;
                    m_mem_cache_valid = false;
                }
                if (const auto *callers = m_functions.CallersOf(l.addr);
                    callers && !callers->empty()) {
                    if (ImGui::MenuItem("Show call xrefs")) {
                        m_xref_target = l.addr;
                        m_open_xref_popup = true;
                    }
                }
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }

        ImGui::PopFont();
        ImGui::EndTable();
    }

    int scroll_instructions = 0;
    bool page_up = false;
    bool page_down = false;
    if (disasm_hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel > 0.0f) scroll_instructions = -3;
        if (wheel < 0.0f) scroll_instructions = 3;
        page_up = ImGui::IsKeyPressed(ImGuiKey_PageUp);
        page_down = ImGui::IsKeyPressed(ImGuiKey_PageDown);
    }

    if (page_up) {
        const int n = std::max(1, rendered_instructions - 1);
        m_base = BackUp(m_base, n);
        m_dirty = true;
    } else if (page_down && !m_lines.empty()) {
        const int n = std::max(1, rendered_instructions - 1);
        const size_t idx =
            std::min((size_t)n, m_lines.size() - 1);
        m_base = m_lines[idx].addr;
        m_dirty = true;
    } else if (scroll_instructions < 0) {
        m_base = BackUp(m_base, -scroll_instructions);
        m_dirty = true;
    } else if (scroll_instructions > 0 && !m_lines.empty()) {
        const size_t idx =
            std::min((size_t)scroll_instructions, m_lines.size() - 1);
        m_base = m_lines[idx].addr;
        m_dirty = true;
    }

    if (m_dirty) {
        snprintf(m_goto_buf, sizeof(m_goto_buf), "%08X", m_base);
    }

    ImGui::EndChild();
}

void DisassemblerWindow::DrawPatchModal()
{
    if (m_patch_modal_requested) {
        ImGui::OpenPopup("Patch instruction bytes###xemu_dbg_patch");
        m_patch_modal_requested = false;
    }
    bool open = true;
    if (!ImGui::BeginPopupModal("Patch instruction bytes###xemu_dbg_patch", &open,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;

    const Line *line = nullptr;
    for (const auto &l : m_lines) if (l.addr == m_patch_addr) { line = &l; break; }
    ImGui::Text("Address: 0x%08X", m_patch_addr);
    if (line) ImGui::Text("Current instruction: %s %s", line->mnemonic, line->ops);
    ImGui::TextWrapped("Enter raw x86 bytes. If you want a shorter replacement for this instruction, use NOP bytes explicitly or press Pad with NOPs.");
    ImGui::SetNextItemWidth(520 * g_viewport_mgr.m_scale);
    ImGui::InputTextMultiline("##patchbytes", m_patch_bytes, sizeof(m_patch_bytes),
                              ImVec2(520 * g_viewport_mgr.m_scale, 85 * g_viewport_mgr.m_scale));

    if (ImGui::Button("Pad with NOPs") && line) {
        std::vector<uint8_t> data;
        if (ParseHexBytes(m_patch_bytes, &data) && data.size() < line->len) {
            data.resize(line->len, 0x90);
            std::string s = HexBytes(data.data(), data.size());
            snprintf(m_patch_bytes, sizeof(m_patch_bytes), "%s", s.c_str());
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Apply live")) {
        std::vector<uint8_t> data;
        if (!ParseHexBytes(m_patch_bytes, &data)) {
            m_status = "patch bytes are not valid hex";
        } else {
            std::string err;
            if (ApplyPatch(m_patch_addr, data, &err)) {
                m_status = "live patch applied; original bytes saved for undo";
                ImGui::CloseCurrentPopup();
            } else m_status = "patch failed: " + err;
        }
        m_status_ms = SDL_GetTicks();
    }
#ifdef CONFIG_XEMU_FEATURE_CHEATS
    ImGui::SameLine();
    if (ImGui::Button("Apply + save ASM cheat...")) {
        std::vector<uint8_t> data;
        if (!ParseHexBytes(m_patch_bytes, &data)) {
            m_status = "patch bytes are not valid hex";
        } else {
            std::string err;
            if (ApplyPatch(m_patch_addr, data, &err)) {
                uint32_t addr = m_patch_addr;
                ImGui::CloseCurrentPopup();
                OpenCreateCheatModal(addr);
            } else {
                m_status = "patch failed: " + err;
            }
        }
        m_status_ms = SDL_GetTicks();
    }
#endif
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DisassemblerWindow::DrawCreateCheatModal()
{
#ifndef CONFIG_XEMU_FEATURE_CHEATS
    m_create_cheat_requested = false;
    return;
#else
    if (m_create_cheat_requested) {
        ImGui::OpenPopup("Create ASM cheat###xemu_dbg_create_cheat");
        m_create_cheat_requested = false;
    }

    bool open = true;
    if (!ImGui::BeginPopupModal("Create ASM cheat###xemu_dbg_create_cheat",
                                &open, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    const CodePatch *patch = FindPatch(m_create_cheat_addr);
    if (!patch) {
        ImGui::TextDisabled("The temporary debugger patch no longer exists.");
        if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }

    ImGui::Text("Patch address: 0x%08X", patch->addr);
    ImGui::Text("Original: %s", HexBytes(patch->original.data(), patch->original.size()).c_str());
    ImGui::Text("Patched:  %s", HexBytes(patch->patched.data(), patch->patched.size()).c_str());
    ImGui::Separator();

    ImGui::SetNextItemWidth(520 * g_viewport_mgr.m_scale);
    ImGui::InputText("Name", m_create_cheat_name, sizeof(m_create_cheat_name));
    ImGui::SetNextItemWidth(520 * g_viewport_mgr.m_scale);
    ImGui::InputTextMultiline("Description", m_create_cheat_desc,
                              sizeof(m_create_cheat_desc),
                              ImVec2(520 * g_viewport_mgr.m_scale,
                                     65 * g_viewport_mgr.m_scale));
    ImGui::Checkbox("Enable immediately", &m_create_cheat_enabled);
    ImGui::TextDisabled("Saved to Cheats -> [ASM]. The name is automatically given the [ASM] suffix.");
    ImGui::TextDisabled("Disabling it restores the original code bytes captured at first apply.");

    std::string preview = GenerateVirtualWriteCodes(patch->addr, patch->patched);
    ImGui::SeparatorText("Generated existing cheat codes");
    ImGui::PushFont(xemu_feature_detach::FixedWidthFont(g_font_mgr.m_fixed_width_font));
    ImGui::TextUnformatted(preview.empty() ? "(address cannot be represented)" : preview.c_str());
    ImGui::PopFont();

    bool can_create = !preview.empty() && m_create_cheat_name[0] != '\0';
    ImGui::BeginDisabled(!can_create);
    if (ImGui::Button("Create cheat")) {
        bool enabled = m_create_cheat_enabled;
        uint32_t addr = m_create_cheat_addr;
        if (TransferPatchToCheats(addr, m_create_cheat_name,
                                  m_create_cheat_desc, enabled)) {
            ImGui::CloseCurrentPopup();
        }
        m_status_ms = SDL_GetTicks();
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Copy codes")) SetClipboard(preview);
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
#endif
}

void DisassemblerWindow::DrawXrefPopup()
{
    if (m_open_xref_popup) {
        ImGui::OpenPopup("Call xrefs###xemu_dbg_xrefs");
        m_open_xref_popup = false;
    }
    bool open = true;
    if (!ImGui::BeginPopupModal("Call xrefs###xemu_dbg_xrefs", &open,
                                ImGuiWindowFlags_AlwaysAutoResize)) return;
    std::string name = NearestName(m_xref_target);
    ImGui::Text("Target: 0x%08X  %s", m_xref_target, name.c_str());
    const auto *callers = m_functions.CallersOf(m_xref_target);
    if (!callers || callers->empty()) {
        ImGui::TextDisabled("No direct call xrefs recorded.");
    } else {
        ImGui::BeginChild("##xreflist", ImVec2(500 * g_viewport_mgr.m_scale,
                                                280 * g_viewport_mgr.m_scale), true);
        for (uint32_t a : *callers) {
            std::string n = NearestName(a);
            char row[640]; snprintf(row, sizeof(row), "%08X  %s", a, n.c_str());
            if (ImGui::Selectable(row)) { GoTo(a, true); ImGui::CloseCurrentPopup(); break; }
        }
        ImGui::EndChild();
    }
    if (ImGui::Button("Close")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DisassemblerWindow::DrawRegisters()
{
    const XemuDbgRegs &r = Regs();
    if (!r.valid) { ImGui::TextDisabled("No CPU."); return; }

    struct R { const char *name; const char *api; uint32_t value; };
    R regs[] = {
        {"EAX","eax",r.eax},{"ECX","ecx",r.ecx},{"EDX","edx",r.edx},{"EBX","ebx",r.ebx},
        {"ESP","esp",r.esp},{"EBP","ebp",r.ebp},{"ESI","esi",r.esi},{"EDI","edi",r.edi},
        {"EIP","eip",r.eip},{"EFLAGS","eflags",r.eflags},
    };

    if (ImGui::BeginTable("##regs_table", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                             ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Register", ImGuiTableColumnFlags_WidthFixed, 80 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Hex", ImGuiTableColumnFlags_WidthFixed, 145 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Signed", ImGuiTableColumnFlags_WidthFixed, 120 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Points at", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        ImGui::PushFont(xemu_feature_detach::FixedWidthFont(g_font_mgr.m_fixed_width_font));
        for (int i = 0; i < IM_ARRAYSIZE(regs); ++i) {
            ImGui::PushID(i);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(regs[i].name);
            ImGui::TableSetColumnIndex(1);
            char buf[16]; snprintf(buf, sizeof(buf), "%08X", regs[i].value);
            ImGui::SetNextItemWidth(-1);
            if (ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_CharsHexadecimal |
                                                         ImGuiInputTextFlags_EnterReturnsTrue)) {
                if (!xemu_dbg_is_running()) {
                    xemu_dbg_set_reg(regs[i].api, (uint32_t)strtoul(buf, nullptr, 16));
                    m_regs_fresh = false; m_dirty = true;
                }
            }
            ImGui::TableSetColumnIndex(2); ImGui::Text("%" PRId32, Signed32(regs[i].value));
            ImGui::TableSetColumnIndex(3); std::string d = DescribePointer(regs[i].value); ImGui::TextUnformatted(d.c_str());
            ImGui::PopID();
        }
        ImGui::PopFont();
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Flags: %s %s %s %s %s %s",
                        (r.eflags & 0x0001) ? "CF" : "cf",
                        (r.eflags & 0x0040) ? "ZF" : "zf",
                        (r.eflags & 0x0080) ? "SF" : "sf",
                        (r.eflags & 0x0800) ? "OF" : "of",
                        (r.eflags & 0x0004) ? "PF" : "pf",
                        (r.eflags & 0x0200) ? "IF" : "if");
    ImGui::TextDisabled("CS %04X  SS %04X  DS %04X  ES %04X  FS %04X  GS %04X   CR0 %08X  CR2 %08X  CR3 %08X  CR4 %08X",
                        r.cs, r.ss, r.ds, r.es, r.fs, r.gs, r.cr0, r.cr2, r.cr3, r.cr4);
}

void DisassemblerWindow::DrawBreakpoints()
{
    static char wp_addr[16] = "00000000";
    static int wp_len = 4;
    static int wp_kind = 1;
    static int wp_space = 0; // 0 virtual, 1 physical

    ImGui::SetNextItemWidth(100 * g_viewport_mgr.m_scale);
    ImGui::InputText("Addr##wp", wp_addr, sizeof(wp_addr),
                     ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(60 * g_viewport_mgr.m_scale);
    ImGui::InputInt("Len", &wp_len, 0);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(95 * g_viewport_mgr.m_scale);
    ImGui::Combo("##wpkind", &wp_kind, "Read\0Write\0Both\0");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(90 * g_viewport_mgr.m_scale);
    ImGui::Combo("##wpspace", &wp_space, "Virtual\0Physical\0");
    ImGui::SameLine();
    if (ImGui::Button("Add watch")) {
        const int flags = wp_kind == 0 ? BP_MEM_READ
                         : wp_kind == 1 ? BP_MEM_WRITE
                                        : BP_MEM_READ | BP_MEM_WRITE;
        const uint32_t a = (uint32_t)strtoul(wp_addr, nullptr, 16);
        const uint32_t n = (uint32_t)(wp_len > 0 ? wp_len : 1);
        const bool virt = wp_space == 0;
        if (xemu_dbg_wp_insert_space(a, n, flags, virt)) {
            m_wps.push_back({a, n, flags, virt, true});
            m_status = std::string(virt ? "virtual" : "physical") +
                       " watchpoint added";
        } else {
            m_status = std::string("could not add ") +
                       (virt ? "virtual" : "physical") + " watchpoint";
        }
        m_status_ms = SDL_GetTicks();
    }

    if (ImGui::BeginTable("##bptable", 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                          ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed,
                                72 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Space", ImGuiTableColumnFlags_WidthFixed,
                                64 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed,
                                96 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed,
                                55 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Access", ImGuiTableColumnFlags_WidthFixed,
                                62 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Actions", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < m_bps.size(); ++i) {
            ImGui::PushID((int)(1000 + i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("execute");
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(m_bps[i].virt ? "virtual" : "physical");
            ImGui::TableSetColumnIndex(2); ImGui::Text("%08X", m_bps[i].addr);
            ImGui::TableSetColumnIndex(3); ImGui::TextUnformatted("1");
            ImGui::TableSetColumnIndex(4); ImGui::TextUnformatted("x");
            ImGui::TableSetColumnIndex(5);
            if (ImGui::SmallButton("Show")) {
                uint32_t va = m_bps[i].addr;
                if (m_bps[i].virt || xemu_dbg_to_virt(m_bps[i].addr, &va)) {
                    GoTo(va, true);
                } else {
                    m_status = "physical breakpoint currently has no virtual mapping";
                    m_status_ms = SDL_GetTicks();
                }
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                xemu_dbg_bp_remove_space(m_bps[i].addr, m_bps[i].virt);
                m_bps.erase(m_bps.begin() + i);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        for (size_t i = 0; i < m_wps.size(); ++i) {
            ImGui::PushID((int)(2000 + i));
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted("watch");
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(m_wps[i].virt ? "virtual" : "physical");
            ImGui::TableSetColumnIndex(2); ImGui::Text("%08X", m_wps[i].addr);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%u", m_wps[i].len);
            ImGui::TableSetColumnIndex(4);
            ImGui::TextUnformatted(m_wps[i].flags == BP_MEM_READ ? "r" :
                                   m_wps[i].flags == BP_MEM_WRITE ? "w" : "rw");
            ImGui::TableSetColumnIndex(5);
            if (ImGui::SmallButton("Show")) {
                snprintf(m_mem_buf, sizeof(m_mem_buf), "%08X", m_wps[i].addr);
                m_mem_addr = m_wps[i].addr;
                m_mem_virtual = m_wps[i].virt;
                m_mem_region = 0;
                m_mem_cache_valid = false;
                m_mem_have_sel = true;
                m_mem_sel = m_wps[i].addr;
            }
            ImGui::SameLine();
            if (ImGui::SmallButton("Remove")) {
                xemu_dbg_wp_remove_space(m_wps[i].addr, m_wps[i].len,
                                         m_wps[i].flags, m_wps[i].virt);
                m_wps.erase(m_wps.begin() + i);
                ImGui::PopID();
                break;
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void DisassemblerWindow::DrawStack()
{
    const XemuDbgRegs &r = Regs();
    if (!r.valid) return;

    /* Stack walking performs many nearby virtual reads. Reset translation
     * state once for this snapshot, then let all frame/raw-stack reads share
     * the feature-owned page cache instead of independently resynchronizing. */
    xemu_guestmem_invalidate_cache();

    // Live execution can change EBP depth every refresh.  If the call-chain
    // table sizes itself to its current number of frames, everything below it
    // jumps vertically.  Give both stack sections stable viewports and let
    // their contents change inside those viewports instead.
    const float avail_y = ImGui::GetContentRegionAvail().y;
    float frames_h = avail_y * 0.34f;
    frames_h = std::max(105.0f * g_viewport_mgr.m_scale, frames_h);
    frames_h = std::min(210.0f * g_viewport_mgr.m_scale, frames_h);
    if (frames_h > avail_y - 100.0f * g_viewport_mgr.m_scale) {
        frames_h = std::max(80.0f * g_viewport_mgr.m_scale,
                            avail_y - 100.0f * g_viewport_mgr.m_scale);
    }

    ImGui::TextUnformatted("EBP call chain");
    ImGui::BeginChild("##frames_area", ImVec2(0, frames_h), true);
    if (ImGui::BeginTable("##frames", 4,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_Borders |
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_ScrollY,
                          ImVec2(0, 0))) {
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed,
                                35 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Return", ImGuiTableColumnFlags_WidthFixed,
                                96 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Function", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("EBP", ImGuiTableColumnFlags_WidthFixed,
                                96 * g_viewport_mgr.m_scale);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        uint32_t ebp = r.ebp;
        uint32_t seen[32] = {};
        size_t seen_count = 0;
        for (int frame = 0; frame < 32 && ebp; ++frame) {
            bool repeated = false;
            for (size_t i = 0; i < seen_count; ++i) {
                if (seen[i] == ebp) {
                    repeated = true;
                    break;
                }
            }
            if (repeated) break;
            seen[seen_count++] = ebp;

            uint32_t vals[2] = {};
            if (xemu_dbg_read_space(ebp, vals, sizeof(vals), true) !=
                (ssize_t)sizeof(vals)) {
                break;
            }

            uint32_t next = vals[0];
            uint32_t ret = vals[1];
            if (ret < 0x1000 ||
                (m_functions.HaveImage() && !m_functions.InImage(ret))) {
                break;
            }

            ImGui::TableNextRow(ImGuiTableRowFlags_None,
                                ImGui::GetFrameHeight());
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%d", frame + 1);

            ImGui::TableSetColumnIndex(1);
            char id[32];
            snprintf(id, sizeof(id), "%08X##ret%d", ret, frame);
            if (ImGui::Selectable(id, false)) GoTo(ret, true);

            ImGui::TableSetColumnIndex(2);
            std::string n = NearestName(ret);
            ImGui::TextUnformatted(n.c_str());

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%08X", ebp);

            if (next <= ebp) break;
            ebp = next;
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();

    ImGui::SeparatorText("Raw stack");
    ImGui::BeginChild("##rawstack_area", ImVec2(0, 0), true);

    uint8_t raw[48 * 4] {};
    ssize_t got =
        r.esp ? xemu_dbg_read_space(r.esp, raw, sizeof(raw), true) : -1;
    int words = got > 0 ? (int)got / 4 : 0;

    if (ImGui::BeginTable("##rawstack", 4,
                          ImGuiTableFlags_RowBg |
                          ImGuiTableFlags_Borders |
                          ImGuiTableFlags_SizingStretchProp |
                          ImGuiTableFlags_ScrollY,
                          ImVec2(0, 0))) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed,
                                96 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed,
                                72 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthFixed,
                                96 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Points at", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableHeadersRow();

        for (int i = 0; i < words; ++i) {
            uint32_t a = r.esp + i * 4;
            uint32_t v = ReadLe32(raw + i * 4);
            ImGui::TableNextRow(ImGuiTableRowFlags_None,
                                ImGui::GetFrameHeight());
            ImGui::TableSetColumnIndex(0);
            ImGui::Text("%08X", a);
            ImGui::TableSetColumnIndex(1);
            ImGui::Text("+0x%X", i * 4);
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%08X", v);
            ImGui::TableSetColumnIndex(3);
            std::string d = DescribePointer(v);
            ImGui::TextUnformatted(d.c_str());
        }
        ImGui::EndTable();
    }
    ImGui::EndChild();
}

void DisassemblerWindow::DrawFrameSlots(bool parameters)
{
    const XemuDbgRegs &r = Regs();
    if (!r.valid || !r.ebp) return;
    if (parameters) {
        std::string d = DescribePointer(r.ecx);
        ImGui::TextDisabled("ECX (possible this): %08X  %s", r.ecx, d.c_str());
    }
    if (ImGui::BeginTable(parameters ? "##params" : "##locals", 6,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                          ImGuiTableFlags_SizingStretchProp)) {
        // Do not let changing live values feed back into column sizing.  In
        // particular, switching between "unmapped", small integers, large signed
        // values, floats, and pointer descriptions used to make this table reflow
        // on practically every debugger refresh.
        ImGui::TableSetupColumn("Slot", ImGuiTableColumnFlags_WidthFixed, 100 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 100 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Hex", ImGuiTableColumnFlags_WidthFixed, 105 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Signed", ImGuiTableColumnFlags_WidthFixed, 115 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Float", ImGuiTableColumnFlags_WidthFixed, 125 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Points at", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        int count = 16;
        for (int i = 0; i < count; ++i) {
            uint32_t addr;
            char slot[32];
            if (parameters) {
                uint32_t off = 8 + i * 4; addr = r.ebp + off;
                snprintf(slot, sizeof(slot), "[ebp+0x%X]", off);
            } else {
                if (!r.esp || r.esp > r.ebp) break;
                uint32_t off = (i + 1) * 4; addr = r.ebp - off;
                if (addr < r.esp) break;
                snprintf(slot, sizeof(slot), "[ebp-0x%X]", off);
            }
            uint32_t v = 0; bool ok = xemu_dbg_read_space(addr, &v, 4, true) == 4;
            ImGui::TableNextRow(); ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(slot);
            ImGui::TableSetColumnIndex(1); ImGui::Text("%08X", addr);
            ImGui::TableSetColumnIndex(2); if (ok) ImGui::Text("%08X", v); else ImGui::TextDisabled("unmapped");
            ImGui::TableSetColumnIndex(3); if (ok) ImGui::Text("%" PRId32, Signed32(v));
            ImGui::TableSetColumnIndex(4); if (ok) ImGui::Text("%.6g", AsFloat(v));
            ImGui::TableSetColumnIndex(5); if (ok) { std::string d = DescribePointer(v); ImGui::TextUnformatted(d.c_str()); }
        }
        ImGui::EndTable();
    }
}

void DisassemblerWindow::DrawThreads()
{
    const XemuDbgRegs &r = Regs();
    if (!r.valid) { ImGui::TextDisabled("No CPU."); return; }
    ImGui::Text("vCPU 0   %s   EIP %08X   %s", xemu_dbg_is_running() ? "running" : "halted",
                r.eip, NearestName(r.eip).c_str());
    ImGui::TextDisabled("QEMU exposes one Xbox vCPU here. Guest KTHREAD enumeration is separate from host/vCPU threads and is not guessed in this view.");
}

void DisassemblerWindow::AddGlobalsFromCurrentFunction()
{
    uint32_t start = m_have_selection ? m_selected : m_base;
    if (const auto *fn = m_functions.Nearest(start, 0x100000)) start = fn->addr;
    XemuDbgInsn ins[256];
    int n = xemu_dbg_disasm(start, 256, ins);
    size_t added = 0;
    for (int i = 0; i < n; ++i) {
        if (!ins[i].valid) continue;
        if (i > 0 && IsRetMnemonic(ins[i].mnemonic)) break;
        if (!strchr(ins[i].ops, '[')) continue;
        const char *p = ins[i].ops;
        while ((p = strstr(p, "0x")) != nullptr) {
            char *end = nullptr; uint32_t va = (uint32_t)strtoul(p + 2, &end, 16);
            if (end == p + 2) { p += 2; continue; }
            const auto *sec = m_functions.SectionOf(va);
            if (sec && (sec->name == ".data" || sec->name == ".rdata")) {
                bool exists = false; for (const auto &g : m_globals) if (g.addr == va) { exists = true; break; }
                if (!exists) {
                    char src[128]; snprintf(src, sizeof(src), "%s @ %08X", NearestName(start).c_str(), ins[i].addr);
                    m_globals.push_back({va, src}); added++;
                }
            }
            p = end;
        }
    }
    char st[128]; snprintf(st, sizeof(st), "%zu global reference(s) added from current function", added);
    m_status = st; m_status_ms = SDL_GetTicks();
}

void DisassemblerWindow::DrawGlobals()
{
    if (ImGui::Button("Find globals in current function")) AddGlobalsFromCurrentFunction();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(105 * g_viewport_mgr.m_scale);
    ImGui::InputText("##globaladdr", m_global_addr, sizeof(m_global_addr), ImGuiInputTextFlags_CharsHexadecimal);
    ImGui::SameLine();
    if (ImGui::Button("Add address")) {
        uint32_t a = (uint32_t)strtoul(m_global_addr, nullptr, 16);
        bool exists = false; for (const auto &g : m_globals) if (g.addr == a) exists = true;
        if (!exists) m_globals.push_back({a, "added by hand"});
    }
    ImGui::SameLine();
    if (ImGui::Button("Clear")) m_globals.clear();

    if (ImGui::BeginTable("##globals", 6, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                            ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 100 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Section", ImGuiTableColumnFlags_WidthFixed, 85 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Hex", ImGuiTableColumnFlags_WidthFixed, 105 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Signed", ImGuiTableColumnFlags_WidthFixed, 115 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Float", ImGuiTableColumnFlags_WidthFixed, 125 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Source", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < m_globals.size(); ++i) {
            uint32_t v = 0; bool ok = xemu_dbg_read_space(m_globals[i].addr, &v, 4, true) == 4;
            ImGui::PushID((int)i); ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            char a[32]; snprintf(a, sizeof(a), "%08X", m_globals[i].addr);
            if (ImGui::Selectable(a, false, ImGuiSelectableFlags_SpanAllColumns)) {
                m_mem_addr = m_globals[i].addr; snprintf(m_mem_buf, sizeof(m_mem_buf), "%08X", m_mem_addr);
                m_mem_virtual = true; m_mem_cache_valid = false;
            }
            ImGui::TableSetColumnIndex(1); const auto *sec = m_functions.SectionOf(m_globals[i].addr); ImGui::TextUnformatted(sec ? sec->name.c_str() : "?");
            ImGui::TableSetColumnIndex(2); if (ok) ImGui::Text("%08X", v);
            ImGui::TableSetColumnIndex(3); if (ok) ImGui::Text("%" PRId32, Signed32(v));
            ImGui::TableSetColumnIndex(4); if (ok) ImGui::Text("%.6g", AsFloat(v));
            ImGui::TableSetColumnIndex(5); ImGui::TextUnformatted(m_globals[i].source.c_str());
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

void DisassemblerWindow::DrawBottomPanels()
{
    if (!ImGui::BeginTabBar("##debugtabs")) return;
    if (ImGui::BeginTabItem("Registers")) { DrawRegisters(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Breakpoints")) { DrawBreakpoints(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Stack")) { DrawStack(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Threads")) { DrawThreads(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Locals")) { DrawFrameSlots(false); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Parameters")) { DrawFrameSlots(true); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Globals")) { DrawGlobals(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Memory")) { DrawMemory(); ImGui::EndTabItem(); }
    ImGui::EndTabBar();
}

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
    ImGui::SetNextItemWidth(110 * g_viewport_mgr.m_scale);
    if (ImGui::InputText("Address##mem", m_mem_buf, sizeof(m_mem_buf),
                         ImGuiInputTextFlags_CharsHexadecimal |
                         ImGuiInputTextFlags_EnterReturnsTrue)) {
        m_mem_addr = (uint32_t)strtoul(m_mem_buf, nullptr, 16);
        m_mem_region = 0;
        m_mem_cache_valid = false;
        m_mem_keyboard_active = false;
    }

    ImGui::SameLine();
    ImGui::SetNextItemWidth(240 * g_viewport_mgr.m_scale);
    if (ImGui::BeginCombo("Region", kRegions[m_mem_region].name)) {
        for (int i = 0; i < IM_ARRAYSIZE(kRegions); ++i) {
            if (ImGui::Selectable(kRegions[i].name, i == m_mem_region)) {
                m_mem_region = i;
                if (i != 0) {
                    m_mem_virtual = kRegions[i].virt;
                    m_mem_addr = kRegions[i].base;
                    snprintf(m_mem_buf, sizeof(m_mem_buf), "%08X", m_mem_addr);
                    m_mem_cache_valid = false;
                    m_mem_keyboard_active = false;
                }
            }
        }
        ImGui::EndCombo();
    }

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
                ? "physical address has no virtual mapping"
                : "virtual address is not mapped";
            m_status_ms = SDL_GetTicks();
        }
        m_mem_region = 0;
        m_mem_cache_valid = false;
        m_mem_keyboard_active = false;
    }

    ImGui::SameLine();
    ImGui::TextDisabled(m_mem_virtual ? "(guest virtual)" : "(physical RAM)");
    ImGui::SameLine();
    if (ImGui::Checkbox("Big endian", &m_mem_big_endian)) {
        m_mem_cache_valid = false;
    }

    // Match the user's standalone Xemu Cheat Engine memory viewer rather than
    // laying memory out as a spreadsheet.  That viewer derives bytes/row from
    // the available monospaced character width using:
    //   B = max(1, (width_chars - 19) / 4)
    // which leaves room for "0xAAAAAAAA | ", the hex byte field, four spaces,
    // and the text field.  Resizing the debugger therefore naturally shows
    // more/fewer bytes without a cramped manual slider.
    ImGui::PushFont(xemu_feature_detach::FixedWidthFont(g_font_mgr.m_fixed_width_font));
    const float char_w = std::max(1.0f, ImGui::CalcTextSize("0").x);
    ImGui::PopFont();
    const float viewer_width = std::max(1.0f, ImGui::GetContentRegionAvail().x -
                                               20.0f * g_viewport_mgr.m_scale);
    const int width_chars = (int)floorf(viewer_width / char_w);
    int per = std::max(1, (width_chars - 19) / 4);
    if (per > 128) per = 128;
    if (per != m_mem_bytes_per_row) {
        m_mem_bytes_per_row = per;
        m_mem_cache_valid = false;
    }

    float details_reserve = 0.0f;
    if (m_mem_have_sel) {
        details_reserve = ImGui::GetFrameHeightWithSpacing() * 2.0f;
    }
    float dump_h = ImGui::GetContentRegionAvail().y - details_reserve;
    const float min_dump_h = ImGui::GetTextLineHeightWithSpacing() * 4.0f;
    if (dump_h < min_dump_h) dump_h = min_dump_h;

    ImGui::PushFont(xemu_feature_detach::FixedWidthFont(g_font_mgr.m_fixed_width_font));
    const float line_h = ImGui::GetTextLineHeightWithSpacing();
    ImGui::PopFont();
    int visible_rows = (int)floorf(dump_h / std::max(1.0f, line_h));
    if (visible_rows < 1) visible_rows = 1;
    if (visible_rows > 64) visible_rows = 64;

    if (!m_mem_cache_valid || m_poll_now ||
        m_mem_cache_addr != m_mem_addr ||
        m_mem_cache_per != per ||
        m_mem_cache_rows != visible_rows ||
        m_mem_cache_virtual != m_mem_virtual) {
        /* One cache generation for the whole visible snapshot. Adjacent rows
         * normally share guest pages, so the 1-64 row reads reuse translations
         * while still preserving row-by-row behavior across unmapped holes. */
        xemu_guestmem_invalidate_cache();
        for (int row = 0; row < visible_rows; ++row) {
            uint32_t a = m_mem_addr + (uint32_t)(row * per);
            ssize_t got =
                xemu_dbg_read_space(a, m_mem_cache[row], (size_t)per,
                                    m_mem_virtual);
            m_mem_cache_got[row] = got > 0 ? (int)got : 0;
        }
        m_mem_cache_addr = m_mem_addr;
        m_mem_cache_per = per;
        m_mem_cache_rows = visible_rows;
        m_mem_cache_virtual = m_mem_virtual;
        m_mem_cache_valid = true;
    }

    bool clicked_mem_cell = false;
    bool mem_hovered = false;

    // The external viewer is one dark monospaced text surface, not three
    // bordered table columns.  Reproduce that structure here while keeping
    // each displayed byte individually clickable/editable on both sides.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(21, 21, 21, 255));
    ImGui::BeginChild("##memdump", ImVec2(0, dump_h), false,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();
    mem_hovered =
        ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    ImGui::PushFont(xemu_feature_detach::FixedWidthFont(g_font_mgr.m_fixed_width_font));

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImU32 normal_col = IM_COL32(138, 138, 138, 255); // #8A8A8A
    const ImU32 hex_sel_bg = IM_COL32(255, 152, 0, 255);   // #FF9800
    const ImU32 hex_sel_fg = IM_COL32(0, 0, 0, 255);
    const ImU32 text_sel_bg = IM_COL32(68, 68, 68, 255);   // #444444
    const ImU32 text_sel_fg = IM_COL32(255, 255, 255, 255);
    const float row_width = ImGui::GetContentRegionAvail().x;
    const float text_h = ImGui::GetTextLineHeight();

    for (int row = 0; row < visible_rows; ++row) {
        const uint32_t row_addr = m_mem_addr + (uint32_t)(row * per);
        const uint8_t *buf = m_mem_cache[row];
        const int got = m_mem_cache_got[row];
        const ImVec2 row_pos = ImGui::GetCursorScreenPos();

        char prefix[32];
        snprintf(prefix, sizeof(prefix), "0x%08X | ", row_addr);
        dl->AddText(row_pos, normal_col, prefix);

        const float hex_x = row_pos.x + char_w * 13.0f;
        const float text_x = row_pos.x + char_w * (float)(16 + 3 * per);
        const float cell_y = row_pos.y;

        // Reserve/advance exactly one text row before overlaying byte hitboxes.
        ImGui::Dummy(ImVec2(row_width, line_h));
        const ImVec2 next_row = ImGui::GetCursorScreenPos();

        for (int i = 0; i < per; ++i) {
            const int group = i & ~3;
            const int idx =
                (m_mem_big_endian && group + 3 < per)
                    ? group + (3 - (i & 3))
                    : i;
            const bool mapped = idx < got;
            const uint8_t b = mapped ? buf[idx] : 0;
            const uint32_t byte_addr = row_addr + (uint32_t)idx;
            const bool selected = m_mem_have_sel && m_mem_sel == byte_addr;

            const ImVec2 hp(hex_x + char_w * (float)(i * 3), cell_y);
            const ImVec2 hs(char_w * 2.0f, text_h);
            if (selected) {
                dl->AddRectFilled(hp, ImVec2(hp.x + hs.x, hp.y + hs.y),
                                  hex_sel_bg);
            }
            char hb[3] = {'?', '?', 0};
            if (mapped) snprintf(hb, sizeof(hb), "%02X", b);
            dl->AddText(hp, selected ? hex_sel_fg : normal_col, hb);

            ImGui::SetCursorScreenPos(hp);
            ImGui::PushID((int)byte_addr);
            if (ImGui::InvisibleButton("##hexbyte", hs)) {
                m_mem_sel = byte_addr;
                m_mem_have_sel = true;
                m_mem_edit_text = false;
                m_mem_keyboard_active = true;
                m_mem_nibble = 0;
                clicked_mem_cell = true;
            }
            ImGui::PopID();

            const ImVec2 tp(text_x + char_w * (float)i, cell_y);
            const ImVec2 ts(char_w, text_h);
            if (selected) {
                dl->AddRectFilled(tp, ImVec2(tp.x + ts.x, tp.y + ts.y),
                                  text_sel_bg);
            }
            char tc[2] = {
                (char)((mapped && b >= 0x20 && b <= 0x7E) ? b : '.'), 0
            };
            dl->AddText(tp, selected ? text_sel_fg : normal_col, tc);

            ImGui::SetCursorScreenPos(tp);
            ImGui::PushID((int)byte_addr);
            if (ImGui::InvisibleButton("##textbyte", ts)) {
                m_mem_sel = byte_addr;
                m_mem_have_sel = true;
                m_mem_edit_text = true;
                m_mem_keyboard_active = true;
                m_mem_nibble = 0;
                clicked_mem_cell = true;
            }
            ImGui::PopID();
        }

        // Overlay items change ImGui's cursor; restore the one-row progression.
        ImGui::SetCursorScreenPos(next_row);
    }

    ImGui::PopFont();

    int scroll_rows = 0;
    if (mem_hovered) {
        const float wheel = ImGui::GetIO().MouseWheel;
        if (wheel > 0.0f) scroll_rows -= 1;
        if (wheel < 0.0f) scroll_rows += 1;
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) scroll_rows -= visible_rows;
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) scroll_rows += visible_rows;
    }

    ImGui::EndChild();

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !mem_hovered && !clicked_mem_cell) {
        m_mem_keyboard_active = false;
    }

    auto clamp_addr = [](int64_t v) -> uint32_t {
        if (v < 0) return 0;
        if (v > 0xFFFFFFFFLL) return 0xFFFFFFFFu;
        return (uint32_t)v;
    };

    auto ensure_cursor_visible = [&]() {
        if (!m_mem_have_sel) return;
        const uint64_t span = (uint64_t)visible_rows * (uint64_t)per;
        if (m_mem_sel < m_mem_addr) {
            m_mem_addr = (m_mem_sel / (uint32_t)per) * (uint32_t)per;
            m_mem_region = 0;
            m_mem_cache_valid = false;
        } else if ((uint64_t)m_mem_sel >= (uint64_t)m_mem_addr + span) {
            uint64_t row = (uint64_t)m_mem_sel / (uint64_t)per;
            uint64_t first =
                row >= (uint64_t)(visible_rows - 1)
                ? row - (uint64_t)(visible_rows - 1)
                : 0;
            uint64_t base = first * (uint64_t)per;
            if (base > 0xFFFFFFFFULL) base = 0xFFFFFFFFULL;
            m_mem_addr = (uint32_t)base;
            m_mem_region = 0;
            m_mem_cache_valid = false;
        }
        snprintf(m_mem_buf, sizeof(m_mem_buf), "%08X", m_mem_addr);
    };

    if (scroll_rows != 0) {
        m_mem_addr =
            clamp_addr((int64_t)m_mem_addr + (int64_t)scroll_rows * per);
        snprintf(m_mem_buf, sizeof(m_mem_buf), "%08X", m_mem_addr);
        m_mem_region = 0;
        m_mem_cache_valid = false;
    }

    // Keep a tiny transparent ImGui InputText active while a memory cell owns
    // the keyboard. SDL3 only produces proper text/IME events while ImGui asks
    // for text input, so reading InputQueueCharacters without an active text
    // widget would make UTF-8 entry unreliable.
    if (m_mem_keyboard_active && m_mem_have_sel) {
        ImVec2 saved = ImGui::GetCursorPos();
        float sink_x = saved.x + std::max(0.0f,
            ImGui::GetContentRegionAvail().x - 2.0f);
        ImGui::SetCursorPos(ImVec2(sink_x, saved.y));
        ImGui::SetNextItemWidth(1.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0, 0, 0, 0));
        ImGui::SetKeyboardFocusHere();
        ImGui::InputText("##mem_keyboard_sink",
                         m_mem_input_sink, sizeof(m_mem_input_sink),
                         ImGuiInputTextFlags_NoHorizontalScroll);
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
        ImGui::SetCursorPos(saved);
    }

    // Hex-side input writes high/low nibbles.  Text-side input writes the
    // exact UTF-8 bytes supplied by ImGui and advances by the encoded length.
    if (m_mem_keyboard_active && m_mem_have_sel) {
        int64_t move = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) move = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) move = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move = -per;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move = per;

        if (move != 0) {
            m_mem_sel = clamp_addr((int64_t)m_mem_sel + move);
            m_mem_nibble = 0;
            ensure_cursor_visible();
        }

        if (m_mem_input_sink[0]) {
            if (!m_mem_edit_text) {
                for (const unsigned char *p =
                         (const unsigned char *)m_mem_input_sink;
                     *p; ++p) {
                    int nib = HexNibble((ImWchar)*p);
                    if (nib < 0) continue;

                    uint8_t cur = 0;
                    if (xemu_dbg_read_space(m_mem_sel, &cur, 1,
                                            m_mem_virtual) != 1) {
                        m_status =
                            "selected memory byte is not mapped/readable";
                        m_status_ms = SDL_GetTicks();
                        break;
                    }

                    uint8_t value = cur;
                    if (m_mem_nibble == 0) {
                        value = (uint8_t)((nib << 4) | (cur & 0x0F));
                    } else {
                        value = (uint8_t)((cur & 0xF0) | nib);
                    }

                    if (xemu_dbg_write_space(m_mem_sel, &value, 1,
                                             m_mem_virtual) != 1) {
                        m_status = "memory write failed";
                        m_status_ms = SDL_GetTicks();
                        break;
                    }

                    m_mem_cache_valid = false;
                    if (m_mem_nibble == 0) {
                        m_mem_nibble = 1;
                    } else {
                        m_mem_nibble = 0;
                        m_mem_sel = clamp_addr((int64_t)m_mem_sel + 1);
                        ensure_cursor_visible();
                    }
                }
            } else {
                const size_t n = strlen(m_mem_input_sink);
                if (xemu_dbg_write_space(m_mem_sel, m_mem_input_sink, n,
                                         m_mem_virtual) != (ssize_t)n) {
                    m_status = "UTF-8 memory write failed";
                    m_status_ms = SDL_GetTicks();
                } else {
                    m_mem_cache_valid = false;
                    m_mem_sel = clamp_addr((int64_t)m_mem_sel + (int64_t)n);
                    ensure_cursor_visible();
                }
            }
            m_mem_input_sink[0] = '\0';
        }
    }

    if (m_mem_have_sel) {
        uint32_t other = 0;
        bool ok = m_mem_virtual
            ? xemu_dbg_to_phys(m_mem_sel, &other)
            : xemu_dbg_to_virt(m_mem_sel, &other);

        if (ok) {
            ImGui::Text("%08X %s = %08X %s",
                        m_mem_sel,
                        m_mem_virtual ? "virtual" : "physical",
                        other,
                        m_mem_virtual ? "physical" : "virtual");
        } else {
            ImGui::TextDisabled("%08X has no counterpart mapping", m_mem_sel);
        }

        ImGui::SameLine();
        ImGui::TextDisabled(m_mem_edit_text
                                ? "[text edit: UTF-8]"
                                : (m_mem_nibble ? "[hex edit: low nibble]"
                                                : "[hex edit: high nibble]"));

        ImGui::SameLine();
        if (ImGui::SmallButton("Disassemble here")) {
            uint32_t va = m_mem_sel;
            if (m_mem_virtual || xemu_dbg_to_virt(m_mem_sel, &va)) {
                GoTo(va, true);
            } else {
                m_status = "physical address currently has no executable virtual mapping";
                m_status_ms = SDL_GetTicks();
            }
        }

        ImGui::SameLine();
        if (ImGui::SmallButton("Break execute##memsel")) {
            bool duplicate = false;
            for (const auto &bp : m_bps) {
                if (bp.addr == m_mem_sel && bp.virt == m_mem_virtual) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                m_status = "execute breakpoint already exists here";
            } else if (xemu_dbg_bp_insert_space(m_mem_sel, m_mem_virtual)) {
                m_bps.push_back({m_mem_sel, m_mem_virtual, true});
                m_status = std::string(m_mem_virtual ? "virtual" : "physical") +
                           " execute breakpoint added";
            } else {
                m_status = std::string("could not add ") +
                           (m_mem_virtual ? "virtual" : "physical") +
                           " execute breakpoint";
            }
            m_status_ms = SDL_GetTicks();
        }

        auto add_memory_watch = [&](const char *label, int flags) {
            ImGui::SameLine();
            if (!ImGui::SmallButton(label)) return;
            bool duplicate = false;
            for (const auto &wp : m_wps) {
                if (wp.addr == m_mem_sel && wp.len == 4 && wp.flags == flags &&
                    wp.virt == m_mem_virtual) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                m_status = "watchpoint already exists here";
            } else if (xemu_dbg_wp_insert_space(m_mem_sel, 4, flags,
                                                m_mem_virtual)) {
                m_wps.push_back({m_mem_sel, 4, flags, m_mem_virtual, true});
                m_status = std::string("4-byte ") +
                           (flags == BP_MEM_READ ? "read" :
                            flags == BP_MEM_WRITE ? "write" : "access") +
                           " watchpoint added in " +
                           (m_mem_virtual ? "virtual" : "physical") +
                           " space";
            } else {
                m_status = std::string("could not add ") +
                           (m_mem_virtual ? "virtual" : "physical") +
                           " watchpoint";
            }
            m_status_ms = SDL_GetTicks();
        };

        add_memory_watch("Break on read##memsel", BP_MEM_READ);
        add_memory_watch("Break on write##memsel", BP_MEM_WRITE);
        add_memory_watch("Break on access##memsel", BP_MEM_READ | BP_MEM_WRITE);
    }
}

void DisassemblerWindow::Draw()
{
    static constexpr const char *kDetachId = "debug-tools.debugger";
    xemu_feature_detach::Register(kDetachId, "Xemu Debugger", &m_is_open,
                                  [this]() { this->Draw(); });
    xemu_feature_detach::Pump();

    if (!m_is_open || !xemu_feature_detach::ShouldDraw(kDetachId)) return;

    const ImVec2 avail = ImGui::GetMainViewport()->WorkSize;
    if (xemu_feature_detach::IsDetachedPass(kDetachId)) {
        xemu_feature_detach::PrepareWindow(kDetachId);
    } else {
        ImVec2 want(1280 * g_viewport_mgr.m_scale, 820 * g_viewport_mgr.m_scale);
        if (want.x > avail.x * 0.96f) want.x = avail.x * 0.96f;
        if (want.y > avail.y * 0.96f) want.y = avail.y * 0.96f;
        ImGui::SetNextWindowSize(want, ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSizeConstraints(ImVec2(720 * g_viewport_mgr.m_scale, 480 * g_viewport_mgr.m_scale), avail);
    }

    const ImGuiWindowFlags window_flags = xemu_feature_detach::WindowFlags(
        kDetachId, ImGuiWindowFlags_NoCollapse);
    if (!ImGui::Begin("Debugger", &m_is_open, window_flags)) {
        ImGui::End(); return;
    }
    xemu_feature_detach::ObserveCurrentWindow(kDetachId);
    if (!xemu_dbg_have_disasm()) {
        ImGui::TextWrapped("This build has no disassembler. Rebuild with Capstone enabled.");
        ImGui::End(); return;
    }

    uint32_t now = SDL_GetTicks();
    if (m_live && (now - m_last_poll_ms) >= (uint32_t)m_interval_ms) {
        m_last_poll_ms = now; m_regs_fresh = false; m_poll_now = true;
    } else m_poll_now = false;

    DrawNavigationBar();
    DrawDebugBar();
    ImGui::Separator();

    // User-resizable vertical split: drag the bar between the code workspace
    // and lower panels to dedicate more/less room to instructions.  This is
    // deliberately a real splitter instead of a fixed 62% ratio, because RE
    // work alternates constantly between "show me lots of code" and "show me
    // lots of registers/memory". Double-click the bar to restore the default.
    const float total_h = ImGui::GetContentRegionAvail().y;
    const float splitter_h = 7.0f * g_viewport_mgr.m_scale;
    const float usable_h = std::max(1.0f, total_h - splitter_h);
    const float min_top = 150.0f * g_viewport_mgr.m_scale;
    const float min_bottom = 115.0f * g_viewport_mgr.m_scale;
    float top_h = usable_h * m_code_split_ratio;
    top_h = std::max(min_top, std::min(top_h, usable_h - min_bottom));

    ImGui::BeginChild("##top", ImVec2(0, top_h), false);
    float total_w = ImGui::GetContentRegionAvail().x;
    float func_w = std::max(250 * g_viewport_mgr.m_scale, total_w * 0.24f);
    DrawFunctionBrowser(func_w);
    ImGui::SameLine();
    ImGui::BeginChild("##codepane", ImVec2(0, 0), false);
    DrawDisassembly();
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::InvisibleButton("##code_bottom_splitter",
                           ImVec2(ImGui::GetContentRegionAvail().x, splitter_h));
    const bool split_hovered = ImGui::IsItemHovered();
    const bool split_active = ImGui::IsItemActive();
    if (split_hovered || split_active) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
    }
    if (split_active && ImGui::GetIO().MouseDelta.y != 0.0f) {
        const float next_top = std::max(
            min_top, std::min(top_h + ImGui::GetIO().MouseDelta.y,
                              usable_h - min_bottom));
        m_code_split_ratio = next_top / usable_h;
    }
    if (split_hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        m_code_split_ratio = 0.62f;
    }
    ImDrawList *split_dl = ImGui::GetWindowDrawList();
    const ImVec2 split_min = ImGui::GetItemRectMin();
    const ImVec2 split_max = ImGui::GetItemRectMax();
    const float split_y = (split_min.y + split_max.y) * 0.5f;
    const ImU32 split_col = (split_hovered || split_active)
        ? IM_COL32(76, 190, 76, 255)
        : ImGui::GetColorU32(ImGuiCol_Separator);
    split_dl->AddLine(ImVec2(split_min.x, split_y),
                      ImVec2(split_max.x, split_y), split_col,
                      split_active ? 3.0f : 1.0f);

    ImGui::BeginChild("##bottom", ImVec2(0, 0), true);
    DrawBottomPanels();
    ImGui::EndChild();

    DrawPatchModal();
    DrawCreateCheatModal();
    DrawXrefPopup();
    ImGui::End();
}
