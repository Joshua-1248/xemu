// SPDX-License-Identifier: GPL-2.0-or-later
//
// xemu User Interface - Disassembler / CPU debugger
//
// Features #5 refresh: the window is now shaped around the user's external
// Xemu Cheat Engine workflow, but uses direct in-process Xemu debug APIs.
// Reserved cheat code Type F is intentionally not touched: patch helpers emit
// the already-supported ordinary virtual write codes (8/9/A).

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <new>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "disassembler.hh"
#include "xemu-features/shared/detachable-windows.hh"
#include "ui/xui/common.hh"
#include "ui/xui/font-manager.hh"
#include "ui/xui/misc.hh"
#include "ui/xui/viewport-manager.hh"
#include "ui/xemu-settings.h"
#include "xemu-xbe.h"

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

static int MemoryViewerPressedHexNibble()
{
    // Hex editing should not depend on an active InputText widget. Use native
    // ImGui key transitions so the initial press is consumed exactly once and
    // normal held-key repeat is generated from ImGui's KeyRepeatDelay/Rate.
    // Modifier shortcuts (Ctrl+C/V, Alt, Super) must never be interpreted as
    // hexadecimal input.
    const ImGuiIO &io = ImGui::GetIO();
    if (io.KeyCtrl || io.KeyAlt || io.KeySuper) return -1;
    static constexpr ImGuiKey digit_keys[] = {
        ImGuiKey_0, ImGuiKey_1, ImGuiKey_2, ImGuiKey_3, ImGuiKey_4,
        ImGuiKey_5, ImGuiKey_6, ImGuiKey_7, ImGuiKey_8, ImGuiKey_9,
    };
    static constexpr ImGuiKey keypad_keys[] = {
        ImGuiKey_Keypad0, ImGuiKey_Keypad1, ImGuiKey_Keypad2, ImGuiKey_Keypad3,
        ImGuiKey_Keypad4, ImGuiKey_Keypad5, ImGuiKey_Keypad6, ImGuiKey_Keypad7,
        ImGuiKey_Keypad8, ImGuiKey_Keypad9,
    };
    static constexpr ImGuiKey hex_keys[] = {
        ImGuiKey_A, ImGuiKey_B, ImGuiKey_C,
        ImGuiKey_D, ImGuiKey_E, ImGuiKey_F,
    };

    for (int i = 0; i < 10; ++i) {
        if (ImGui::IsKeyPressed(digit_keys[i], true) ||
            ImGui::IsKeyPressed(keypad_keys[i], true)) {
            return i;
        }
    }
    for (int i = 0; i < 6; ++i) {
        if (ImGui::IsKeyPressed(hex_keys[i], true)) return 10 + i;
    }
    return -1;
}

static void SetClipboard(const std::string &text)
{
    ImGui::SetClipboardText(text.c_str());
}

static std::string EscapeSavedField(const std::string &s)
{
    static const char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '%' || c == '\t' || c == '\r' || c == '\n') {
            out.push_back('%');
            out.push_back(hex[c >> 4]);
            out.push_back(hex[c & 0x0F]);
        } else {
            out.push_back((char)c);
        }
    }
    return out;
}

static std::string UnescapeSavedField(const std::string &s)
{
    std::string out;
    out.reserve(s.size());
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() &&
            std::isxdigit((unsigned char)s[i + 1]) &&
            std::isxdigit((unsigned char)s[i + 2])) {
            char tmp[3] = { s[i + 1], s[i + 2], 0 };
            out.push_back((char)strtoul(tmp, nullptr, 16));
            i += 2;
        } else {
            out.push_back(s[i]);
        }
    }
    return out;
}

static bool WriteFileVerified(const std::filesystem::path &path,
                              const std::string &data,
                              std::string *error)
{
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error) *error = "could not create directory: " + ec.message();
        return false;
    }

    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f || !f.write(data.data(), (std::streamsize)data.size()) || !f.flush()) {
            if (error) *error = "could not write temporary saved-address table";
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }

    {
        std::ifstream f(tmp, std::ios::binary);
        std::string verify((std::istreambuf_iterator<char>(f)),
                           std::istreambuf_iterator<char>());
        if (!f.good() && !f.eof()) {
            if (error) *error = "could not verify temporary saved-address table";
            std::filesystem::remove(tmp, ec);
            return false;
        }
        if (verify != data) {
            if (error) *error = "temporary saved-address verification mismatch";
            std::filesystem::remove(tmp, ec);
            return false;
        }
    }

    if (std::filesystem::exists(path, ec) && !ec) {
        const std::filesystem::path bak = path.string() + ".bak";
        std::filesystem::copy_file(path, bak,
                                   std::filesystem::copy_options::overwrite_existing,
                                   ec);
        ec.clear(); // A backup failure must not destroy a valid primary save.
    }

#ifdef _WIN32
    std::filesystem::remove(path, ec);
    ec.clear();
#endif
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        if (error) *error = "could not commit saved-address table: " + ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }

    std::ifstream f(path, std::ios::binary);
    std::string verify((std::istreambuf_iterator<char>(f)),
                       std::istreambuf_iterator<char>());
    if ((!f.good() && !f.eof()) || verify != data) {
        if (error) *error = "final saved-address verification failed";
        return false;
    }
    return true;
}

static void AppendUtf8Codepoint(ImWchar c, std::string *out)
{
    const uint32_t cp = (uint32_t)c;
    if (cp <= 0x7F) {
        out->push_back((char)cp);
    } else if (cp <= 0x7FF) {
        out->push_back((char)(0xC0 | (cp >> 6)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0xFFFF) {
        out->push_back((char)(0xE0 | (cp >> 12)));
        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    } else if (cp <= 0x10FFFF) {
        out->push_back((char)(0xF0 | (cp >> 18)));
        out->push_back((char)(0x80 | ((cp >> 12) & 0x3F)));
        out->push_back((char)(0x80 | ((cp >> 6) & 0x3F)));
        out->push_back((char)(0x80 | (cp & 0x3F)));
    }
}

static int MemoryViewerInputCallback(ImGuiInputTextCallbackData *data)
{
    auto *chars = static_cast<std::vector<ImWchar> *>(data->UserData);
    if (data->EventFlag == ImGuiInputTextFlags_CallbackCharFilter) {
        if (data->EventChar != 0) chars->push_back(data->EventChar);
        // Reject insertion into the hidden widget. The character has already
        // been consumed exactly once by the memory editor above.
        return 1;
    }
    if (data->EventFlag == ImGuiInputTextFlags_CallbackAlways &&
        data->BufTextLen > 0) {
        data->DeleteChars(0, data->BufTextLen);
    }
    return 0;
}

static uint16_t ReadLe16(const uint8_t *p)
{
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static uint64_t ReadLe64(const uint8_t *p)
{
    uint64_t v = 0;
    for (unsigned i = 0; i < 8; ++i) v |= (uint64_t)p[i] << (i * 8);
    return v;
}

static float ReadLeFloat32(const uint8_t *p)
{
    uint32_t bits = ReadLe32(p);
    float v = 0.0f;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

static double ReadLeFloat64(const uint8_t *p)
{
    uint64_t bits = ReadLe64(p);
    double v = 0.0;
    memcpy(&v, &bits, sizeof(v));
    return v;
}

static unsigned Popcount64(uint64_t v)
{
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_popcountll(v);
#else
    unsigned n = 0;
    while (v) { v &= v - 1; ++n; }
    return n;
#endif
}

static unsigned Ctz64(uint64_t v)
{
#if defined(__GNUC__) || defined(__clang__)
    return (unsigned)__builtin_ctzll(v);
#else
    unsigned n = 0;
    while ((v & 1) == 0) { v >>= 1; ++n; }
    return n;
#endif
}


static size_t MemScanSampleHistogram(const uint8_t *base, size_t total,
                                     uint32_t histogram[256])
{
    memset(histogram, 0, 256 * sizeof(histogram[0]));
    if (!base || total == 0) return 0;

    constexpr size_t kSampleBytes = 65536;
    constexpr size_t kBlocks = 8;
    if (total <= kSampleBytes) {
        for (size_t i = 0; i < total; ++i) ++histogram[base[i]];
        return total;
    }

    // Spread the sample across the range. Looking only at the first 64 KiB can
    // choose a pathological memchr path when a game keeps a zero-heavy arena
    // elsewhere in RAM. Eight cache-friendly windows preserve the tiny setup
    // cost while making the density decision representative of the scan.
    constexpr size_t kBlockBytes = kSampleBytes / kBlocks;
    const size_t max_start = total - kBlockBytes;
    size_t sampled = 0;
    for (size_t block = 0; block < kBlocks; ++block) {
        const size_t start = block * max_start / (kBlocks - 1);
        for (size_t i = 0; i < kBlockBytes; ++i) {
            ++histogram[base[start + i]];
        }
        sampled += kBlockBytes;
    }
    return sampled;
}

static bool ScanBudgetExpired(uint64_t start_counter, double budget_ms = 4.0)
{
    // SDL's performance-counter frequency is process-invariant. Cache it so
    // every scanner time slice does not query SDL again on its hot path.
    static const uint64_t freq = SDL_GetPerformanceFrequency();
    if (!freq) return false;
    const uint64_t now = SDL_GetPerformanceCounter();
    return (double)(now - start_counter) * 1000.0 / (double)freq >= budget_ms;
}

// A compact sparse-word index is deliberately bounded. Past this point the
// candidate population is dense enough that sequentially walking the bitset is
// cache-friendly and cheaper than carrying another multi-megabyte index.
static constexpr size_t kMemScanCandidateWordIndexLimit = 262144;

static std::string CompactScanText(const uint8_t *p, size_t n)
{
    std::string out;
    out.reserve(n);
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = p[i];
        out.push_back((c >= 32 && c < 127) ? (char)c : '.');
    }
    return out;
}

static bool ParseScanUnsigned(const char *text, bool force_hex,
                              uint64_t max_value, uint64_t *out)
{
    if (!text || !out) return false;
    std::string clean;
    for (const char *p = text; *p; ++p) {
        if (!std::isspace((unsigned char)*p) && *p != '_') clean.push_back(*p);
    }
    if (clean.empty()) return false;

    bool have_hex_letter = false;
    for (char c : clean) {
        if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) {
            have_hex_letter = true;
            break;
        }
    }
    int base = (force_hex || have_hex_letter ||
                (clean.size() > 2 && clean[0] == '0' &&
                 (clean[1] == 'x' || clean[1] == 'X'))) ? 16 : 10;
    const char *begin = clean.c_str();
    if (base == 16 && clean.size() > 2 && clean[0] == '0' &&
        (clean[1] == 'x' || clean[1] == 'X')) begin += 2;
    if (!*begin) return false;

    errno = 0;
    char *end = nullptr;
    unsigned long long v = strtoull(begin, &end, base);
    if (errno != 0 || end == begin || *end != '\0' || v > max_value) return false;
    *out = (uint64_t)v;
    return true;
}

static bool ParseScanFloat(const char *text, bool raw_hex, int bits, double *out)
{
    if (!text || !out) return false;
    if (raw_hex) {
        uint64_t raw = 0;
        uint64_t maxv = bits == 32 ? UINT32_MAX : UINT64_MAX;
        if (!ParseScanUnsigned(text, true, maxv, &raw)) return false;
        if (bits == 32) {
            uint32_t r32 = (uint32_t)raw;
            float f = 0.0f;
            memcpy(&f, &r32, sizeof(f));
            *out = (double)f;
        } else {
            double d = 0.0;
            memcpy(&d, &raw, sizeof(d));
            *out = d;
        }
        return true;
    }
    errno = 0;
    char *end = nullptr;
    double v = strtod(text, &end);
    while (end && *end && std::isspace((unsigned char)*end)) ++end;
    if (errno != 0 || !end || end == text || *end != '\0') return false;
    *out = v;
    return true;
}

static bool ParseScanAob(const char *text, std::vector<uint8_t> *bytes,
                         std::vector<uint8_t> *mask)
{
    if (!bytes || !mask) return false;
    bytes->clear();
    mask->clear();
    if (!text) return false;
    const char *p = text;
    while (*p) {
        while (*p && (std::isspace((unsigned char)*p) || *p == ',' || *p == ';')) ++p;
        if (!*p) break;
        if (p[0] == '?' && p[1] == '?') {
            bytes->push_back(0);
            mask->push_back(0);
            p += 2;
            continue;
        }
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
        if (!std::isxdigit((unsigned char)p[0])) return false;
        char h[3] = {0, 0, 0};
        h[0] = *p++;
        if (std::isxdigit((unsigned char)*p)) h[1] = *p++;
        else { h[1] = h[0]; h[0] = '0'; }
        char *end = nullptr;
        unsigned long v = strtoul(h, &end, 16);
        if (end != h + 2 || v > 0xFF) return false;
        bytes->push_back((uint8_t)v);
        mask->push_back(0xFF);
    }
    return !bytes->empty();
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
    std::string nearest_name = NearestName(addr);
    if (nearest_name.empty()) {
        snprintf(m_create_cheat_name, sizeof(m_create_cheat_name),
                 "Code patch 0x%08X", addr);
    } else {
        snprintf(m_create_cheat_name, sizeof(m_create_cheat_name),
                 "%s patch", nearest_name.c_str());
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
    const bool have_eip = ReadEip(&eip);
    const bool running = xemu_dbg_is_running();

    // Follow debugger *stops*, not every paused frame. This keeps Step Into /
    // Over / Out and breakpoint hits centered on the green current-instruction
    // row, while still allowing deliberate manual scrolling when paused.
    const bool stopped_now = m_last_debug_running && !running;
    const bool eip_changed = have_eip &&
        (!m_last_follow_eip_valid || eip != m_last_follow_eip);
    if (m_follow_eip && have_eip && !running &&
        (stopped_now || eip_changed)) {
        GoTo(eip, true, false);
    }
    m_last_debug_running = running;
    if (have_eip) {
        m_last_follow_eip = eip;
        m_last_follow_eip_valid = true;
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

            // PCSX2-style current instruction bar: the entire row follows EIP,
            // rather than coloring only the address/instruction text.
            if (is_eip) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       IM_COL32(36, 104, 54, 190));
            } else if (has_bp) {
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                                       IM_COL32(90, 36, 36, 105));
            }

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
            int current_style_colors = 0;
            if (is_eip) {
                const ImVec4 current_bg(0.14f, 0.41f, 0.21f, 0.90f);
                const ImVec4 current_hover(0.18f, 0.50f, 0.27f, 0.95f);
                ImGui::PushStyleColor(ImGuiCol_Header, current_bg);
                ImGui::PushStyleColor(ImGuiCol_HeaderHovered, current_hover);
                ImGui::PushStyleColor(ImGuiCol_HeaderActive, current_hover);
                current_style_colors = 3;
            }
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
            if (current_style_colors) ImGui::PopStyleColor(current_style_colors);
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
                    m_mem_sel = l.addr;
                    m_mem_sel_anchor = l.addr;
                    m_mem_have_sel = true;
                    m_mem_keyboard_active = false;
                    m_open_memory_tab_requested = true;
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
        // Accumulate fractional wheel input so both notched wheels and
        // high-resolution touchpads scroll predictably. One full wheel unit
        // remains three instructions, matching the previous comfortable rate.
        m_disasm_wheel_accum += ImGui::GetIO().MouseWheel * 3.0f;
        const int wheel_steps = (int)std::trunc(m_disasm_wheel_accum);
        if (wheel_steps != 0) {
            scroll_instructions = -wheel_steps;
            m_disasm_wheel_accum -= (float)wheel_steps;
        }
        page_up = ImGui::IsKeyPressed(ImGuiKey_PageUp);
        page_down = ImGui::IsKeyPressed(ImGuiKey_PageDown);
    } else {
        m_disasm_wheel_accum = 0.0f;
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
                m_mem_sel_anchor = m_mem_sel;
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


struct MemScanRegionDef {
    const char *name;
    uint32_t lo;
    uint32_t hi;
    bool virt;
    bool custom;
};

static const MemScanRegionDef kMemScanRegions[] = {
    { "All physical RAM",                  0x00000000, 0x00000000, false, false },
    { "[V] User space (heap + XBE)",       0x00010000, 0x08000000, true,  false },
    { "[V] XBE image only (statics)",      0x00010000, 0x00800000, true,  false },
    { "[V] User heap only",                0x00720000, 0x08000000, true,  false },
    { "[V] Kernel window 0x80000000+",     0x80000000, 0x88000000, true,  false },
    { "[P] Physical 0-64MB",               0x00000000, 0x04000000, false, false },
    { "[P] Physical 64-128MB",             0x04000000, 0x08000000, false, false },
    { "Custom range...",                   0x00000000, 0x00000000, false, true  },
};

static const char *kMemScanRegionNames[] = {
    "All physical RAM",
    "[V] User space (heap + XBE)",
    "[V] XBE image only (statics)",
    "[V] User heap only",
    "[V] Kernel window 0x80000000+",
    "[P] Physical 0-64MB",
    "[P] Physical 64-128MB",
    "Custom range...",
};

static const char *kMemScanTypeNames[] = {
    "int8", "int16", "int32", "float32", "float64",
    "String (UTF-8)", "Array of Bytes"
};

static const char *kMemScanCompareNames[] = {
    "Equal To", "Not Equal To", "Less Than", "Greater Than", "Between",
    "Increased Value", "Decreased Value", "Increased Value By",
    "Decreased Value By", "Changed Value", "Unchanged Value",
    "Unknown Value Search"
};

static size_t MemScanTypeSize(int type)
{
    switch (type) {
    case 0: return 1;
    case 1: return 2;
    case 2: return 4;
    case 3: return 4;
    case 4: return 8;
    default: return 1;
    }
}

static bool MemScanIsNumeric(int type)
{
    return type >= 0 && type <= 4;
}

static bool MemScanIsFloat(int type)
{
    return type == 3 || type == 4;
}

static bool MemScanCompareNeedsTarget(int compare)
{
    return compare == 0 || compare == 1 || compare == 4 ||
           compare == 7 || compare == 8;
}


void DisassemblerWindow::ResetMemorySearch()
{
    m_scan_phase = MemScanPhase::Idle;
    m_scan_baseline_ready = false;
    m_scan_baseline_all_valid = false;
    m_scan_work_all_valid = false;
    m_scan_have_previous_display = false;
    m_scan_operation_unknown = false;
    m_scan_operation_compare = 0;
    m_scan_virtual = false;
    m_scan_range_lo = 0;
    m_scan_range_hi = 0;
    m_scan_first_addr = 0;
    m_scan_item_size = 4;
    m_scan_step = 4;
    m_scan_candidate_slots = 0;
    m_scan_candidate_count = 0;
    m_scan_work_candidate_count = 0;
    m_scan_eval_slot = 0;
    m_scan_eval_word_cursor = 0;
    m_scan_io_cursor = 0;
    m_scan_io_page_index = 0;
    m_scan_io_pages_done = 0;
    m_scan_io_pages_total = 0;
    m_scan_valid_first_page = 0;
    m_scan_sparse_read = false;
    m_scan_target_u64 = 0;
    m_scan_target2_u64 = 0;
    m_scan_target_f64 = 0.0;
    m_scan_target2_f64 = 0.0;
    m_scan_target_bytes.clear();
    m_scan_target_mask.clear();
    m_scan_aob_anchor = -1;
    m_scan_aob_anchor_byte = 0;
    m_scan_search_anchor = -1;
    m_scan_search_anchor_byte = 0;

    // Keep capacities across New Scan. Large unknown-value scans commonly
    // allocate two RAM-sized snapshots plus a candidate bitset; freeing and
    // immediately reallocating all of that for the next search only creates
    // allocator/zero-fill churn. clear() drops logical contents while keeping
    // the retained storage available for a differently targeted scan.
    m_scan_baseline.clear();
    m_scan_work.clear();
    m_scan_baseline_valid_pages.clear();
    m_scan_work_valid_pages.clear();
    m_scan_needed_pages.clear();
    m_scan_candidate_bits.clear();
    m_scan_work_candidate_bits.clear();
    m_scan_candidate_words.clear();
    m_scan_work_candidate_words.clear();
    m_scan_candidate_words_valid = true;
    m_scan_work_candidate_words_valid = true;
    m_scan_page_index = 0;
    m_scan_page_generation = 0;
    m_scan_page_addresses.clear();
    m_scan_live_values.clear();
    m_scan_have_selected = false;
    ++m_scan_generation;
    m_scan_status = "Ready for a new scan";
}

void DisassemblerWindow::CancelMemorySearch()
{
    if (m_scan_phase == MemScanPhase::Idle) return;
    const bool first = m_scan_phase == MemScanPhase::ReadFirst ||
                       m_scan_phase == MemScanPhase::EvalFirst;
    if (first) {
        ResetMemorySearch();
        m_scan_status = "First scan cancelled";
        return;
    }

    m_scan_phase = MemScanPhase::Idle;
    m_scan_work.clear();
    m_scan_work_valid_pages.clear();
    m_scan_needed_pages.clear();
    m_scan_work_candidate_bits.clear();
    m_scan_work_candidate_words.clear();
    m_scan_work_candidate_words_valid = true;
    m_scan_work_candidate_count = 0;
    m_scan_eval_slot = 0;
    m_scan_eval_word_cursor = 0;
    m_scan_io_cursor = 0;
    m_scan_io_page_index = 0;
    m_scan_io_pages_done = 0;
    m_scan_io_pages_total = 0;
    m_scan_sparse_read = false;
    m_scan_status = "Next scan cancelled; previous candidates kept";
}

bool DisassemblerWindow::ConfigureMemoryScanRange(std::string *error)
{
    const uint64_t ram_size64 = xemu_guest_ram_size();
    if (ram_size64 == 0) {
        if (error) *error = "guest RAM size is unavailable";
        return false;
    }
    const uint32_t ram_size = (uint32_t)std::min<uint64_t>(ram_size64, UINT32_MAX);

    if (m_scan_region < 0 || m_scan_region >= IM_ARRAYSIZE(kMemScanRegions)) {
        if (error) *error = "invalid scan region";
        return false;
    }

    const MemScanRegionDef &region = kMemScanRegions[m_scan_region];
    uint32_t lo = region.lo;
    uint32_t hi = region.hi;
    bool virt = region.virt;

    if (m_scan_region == 0) {
        lo = 0;
        hi = ram_size;
        virt = false;
    } else if (m_scan_region == 2 && m_functions.HaveImage()) {
        lo = m_functions.ImageLo();
        hi = m_functions.ImageHi();
        virt = true;
    } else if (region.custom) {
        uint64_t lo64 = 0, hi64 = 0;
        if (!ParseScanUnsigned(m_scan_custom_lo, true, UINT32_MAX, &lo64) ||
            !ParseScanUnsigned(m_scan_custom_hi, true, UINT32_MAX, &hi64)) {
            if (error) *error = "custom range addresses must be hexadecimal";
            return false;
        }
        lo = (uint32_t)lo64;
        hi = (uint32_t)hi64;
        virt = m_scan_custom_virtual;
    }

    if (!virt) {
        lo = std::min(lo, ram_size);
        hi = std::min(hi, ram_size);
    }
    if (hi <= lo) {
        if (error) *error = "selected scan region is empty on this RAM configuration";
        return false;
    }

    const uint64_t span = (uint64_t)hi - lo;
    if (span > (256ULL * 1024ULL * 1024ULL)) {
        if (error) *error = "scan range is larger than 256 MiB";
        return false;
    }
    if (m_scan_item_size == 0 || span < m_scan_item_size) {
        if (error) *error = "scan range is smaller than the selected value";
        return false;
    }

    m_scan_virtual = virt;
    m_scan_range_lo = lo;
    m_scan_range_hi = hi;
    m_scan_valid_first_page = lo >> 12;

    const uint64_t first = ((uint64_t)lo + m_scan_step - 1) /
                           m_scan_step * m_scan_step;
    if (first + m_scan_item_size > hi) {
        if (error) *error = "no aligned values fit in the selected range";
        return false;
    }
    m_scan_first_addr = (uint32_t)first;
    m_scan_candidate_slots =
        ((uint64_t)hi - m_scan_item_size - first) / m_scan_step + 1;
    if (m_scan_candidate_slots == 0) {
        if (error) *error = "scan range has no candidate positions";
        return false;
    }
    return true;
}

bool DisassemblerWindow::ParseMemoryScanTarget(bool first_scan, std::string *error)
{
    const int type = first_scan ? m_scan_value_type : m_scan_locked_type;
    const int compare = m_scan_operation_compare;
    const bool unknown = compare == 11;

    if (unknown) {
        if (!first_scan) {
            if (error) *error = "Unknown Value Search is only valid for First Scan";
            return false;
        }
        if (!MemScanIsNumeric(type)) {
            if (error) *error = "Unknown Value Search requires a numeric value type";
            return false;
        }
        m_scan_item_size = (uint32_t)MemScanTypeSize(type);
        m_scan_step = m_scan_item_size;
        m_scan_target_bytes.clear();
        m_scan_target_mask.clear();
        m_scan_aob_anchor = -1;
        m_scan_search_anchor = -1;
        return true;
    }

    if (first_scan && compare >= 5 && compare <= 10) {
        if (error) {
            *error = "this comparison needs a previous snapshot; use Equal/Not Equal/"
                     "Less/Greater/Between or Unknown Value Search for First Scan";
        }
        return false;
    }

    if (!MemScanIsNumeric(type)) {
        if (compare != 0 && compare != 1 && compare != 9 && compare != 10) {
            if (error) *error = "String/AOB scans support Equal, Not Equal, Changed, or Unchanged";
            return false;
        }
        if (first_scan && (compare == 9 || compare == 10)) {
            if (error) *error = "Changed/Unchanged needs a previous snapshot";
            return false;
        }

        if (type == 5) {
            m_scan_aob_anchor = -1;
            if (MemScanCompareNeedsTarget(compare) || first_scan) {
                m_scan_target_bytes.assign(m_scan_value,
                    m_scan_value + strlen(m_scan_value));
                m_scan_target_mask.assign(m_scan_target_bytes.size(), 0xFF);
                if (m_scan_target_bytes.empty()) {
                    if (error) *error = "enter a UTF-8 string to search for";
                    return false;
                }
                m_scan_search_anchor = 0;
                m_scan_search_anchor_byte = m_scan_target_bytes[0];
            }
        } else {
            if (MemScanCompareNeedsTarget(compare) || first_scan) {
                if (!ParseScanAob(m_scan_value, &m_scan_target_bytes,
                                  &m_scan_target_mask)) {
                    if (error) *error = "enter bytes such as DE AD BE EF or ?? wildcards";
                    return false;
                }
                // Precompile one literal byte as a cheap reject anchor. This
                // is especially valuable for patterns with a wildcard prefix:
                // almost every candidate now fails one load/compare before
                // entering the full masked verifier.
                m_scan_aob_anchor = -1;
                for (size_t i = 0; i < m_scan_target_mask.size(); ++i) {
                    if (m_scan_target_mask[i]) {
                        m_scan_aob_anchor = (int)i;
                        m_scan_aob_anchor_byte = m_scan_target_bytes[i];
                        break;
                    }
                }
                m_scan_search_anchor = m_scan_aob_anchor;
                m_scan_search_anchor_byte = m_scan_aob_anchor_byte;
            }
        }

        if (first_scan) {
            m_scan_item_size = (uint32_t)m_scan_target_bytes.size();
            m_scan_step = 1;
        } else if ((compare == 0 || compare == 1) &&
                   m_scan_target_bytes.size() != m_scan_item_size) {
            if (error) *error = "next-scan String/AOB target must keep the original byte length";
            return false;
        }
        return true;
    }

    m_scan_item_size = first_scan ? (uint32_t)MemScanTypeSize(type)
                                  : m_scan_item_size;
    m_scan_step = first_scan ? m_scan_item_size : m_scan_step;
    m_scan_aob_anchor = -1;
    m_scan_search_anchor = -1;

    const bool need_target = first_scan || MemScanCompareNeedsTarget(compare);
    if (!need_target) return true;

    if (MemScanIsFloat(type)) {
        if (!ParseScanFloat(m_scan_value, m_scan_hex, type == 3 ? 32 : 64,
                            &m_scan_target_f64)) {
            if (error) *error = "invalid floating-point value";
            return false;
        }
        if (compare == 4) {
            if (!ParseScanFloat(m_scan_value_max, m_scan_hex,
                                type == 3 ? 32 : 64, &m_scan_target2_f64)) {
                if (error) *error = "invalid upper floating-point value";
                return false;
            }
            if (m_scan_target2_f64 < m_scan_target_f64)
                std::swap(m_scan_target_f64, m_scan_target2_f64);
        }
        return true;
    }

    const uint64_t maxv = type == 0 ? UINT8_MAX :
                          type == 1 ? UINT16_MAX : UINT32_MAX;
    if (!ParseScanUnsigned(m_scan_value, m_scan_hex, maxv, &m_scan_target_u64)) {
        if (error) *error = "invalid integer value for the selected width";
        return false;
    }
    if (compare == 4) {
        if (!ParseScanUnsigned(m_scan_value_max, m_scan_hex, maxv,
                               &m_scan_target2_u64)) {
            if (error) *error = "invalid upper integer value";
            return false;
        }
        if (m_scan_target2_u64 < m_scan_target_u64)
            std::swap(m_scan_target_u64, m_scan_target2_u64);
    }
    return true;
}

bool DisassemblerWindow::StartMemoryFirstScan()
{
    if (m_scan_phase != MemScanPhase::Idle) return false;

    // Keep the user's controls, but discard any previous scan state.
    const int type = m_scan_value_type;
    const int compare = m_scan_compare;
    const int region = m_scan_region;
    const bool hex = m_scan_hex;
    const bool custom_virtual = m_scan_custom_virtual;
    char value[sizeof(m_scan_value)];
    char value_max[sizeof(m_scan_value_max)];
    char custom_lo[sizeof(m_scan_custom_lo)];
    char custom_hi[sizeof(m_scan_custom_hi)];
    memcpy(value, m_scan_value, sizeof(value));
    memcpy(value_max, m_scan_value_max, sizeof(value_max));
    memcpy(custom_lo, m_scan_custom_lo, sizeof(custom_lo));
    memcpy(custom_hi, m_scan_custom_hi, sizeof(custom_hi));
    ResetMemorySearch();
    m_scan_value_type = type;
    m_scan_compare = compare;
    m_scan_region = region;
    m_scan_hex = hex;
    m_scan_custom_virtual = custom_virtual;
    memcpy(m_scan_value, value, sizeof(value));
    memcpy(m_scan_value_max, value_max, sizeof(value_max));
    memcpy(m_scan_custom_lo, custom_lo, sizeof(custom_lo));
    memcpy(m_scan_custom_hi, custom_hi, sizeof(custom_hi));

    m_scan_locked_type = m_scan_value_type;
    m_scan_operation_compare = m_scan_compare;
    m_scan_operation_unknown = m_scan_operation_compare == 11;

    std::string error;
    if (!ParseMemoryScanTarget(true, &error) ||
        !ConfigureMemoryScanRange(&error)) {
        m_scan_status = error;
        return false;
    }

    const size_t bytes = (size_t)((uint64_t)m_scan_range_hi - m_scan_range_lo);
    const size_t pages = (size_t)(((uint64_t)(m_scan_range_hi - 1) >> 12) -
                                  m_scan_valid_first_page + 1);
    try {
        // resize() reuses retained capacity and does not force a second full
        // zero pass over RAM that the snapshot reader is about to overwrite.
        m_scan_baseline.resize(bytes);
        m_scan_baseline_valid_pages.assign(pages, 0);
        m_scan_candidate_bits.assign(
            (size_t)((m_scan_candidate_slots + 63) / 64), 0);
    } catch (const std::bad_alloc &) {
        ResetMemorySearch();
        m_scan_status = "not enough host memory for this scan range";
        return false;
    }

    m_scan_candidate_count = 0;
    m_scan_work_candidate_count = 0;
    m_scan_io_cursor = m_scan_range_lo;
    m_scan_io_page_index = 0;
    m_scan_io_pages_done = 0;
    m_scan_io_pages_total = pages;
    m_scan_sparse_read = false;
    m_scan_eval_slot = 0;
    m_scan_eval_word_cursor = 0;
    m_scan_candidate_words.clear();
    m_scan_work_candidate_words.clear();
    m_scan_candidate_words_valid = true;
    m_scan_work_candidate_words_valid = true;
    m_scan_have_selected = false;
    m_scan_page_addresses.clear();
    xemu_guestmem_invalidate_cache();
    m_scan_phase = MemScanPhase::ReadFirst;
    m_scan_status = m_scan_operation_unknown
        ? "Capturing unknown-value baseline..."
        : "Reading first-scan snapshot...";
    return true;
}

void DisassemblerWindow::PrepareMemoryNextReadPlan()
{
    const size_t page_count = m_scan_baseline_valid_pages.size();
    m_scan_sparse_read = false;
    m_scan_needed_pages.clear();
    m_scan_io_page_index = 0;
    m_scan_io_pages_done = 0;
    m_scan_io_pages_total = page_count;

    if (page_count == 0 || m_scan_candidate_count == 0) {
        m_scan_sparse_read = true;
        m_scan_io_pages_total = 0;
        return;
    }

    /*
     * Building an exact page-use map pays off only once the result set is
     * genuinely sparse. For a dense Unknown baseline, enumerating tens of
     * millions of candidate bits just to rediscover that essentially every
     * page is needed would be slower than the existing sequential snapshot.
     */
    const uint64_t sparse_candidate_limit =
        std::max<uint64_t>(4096, (uint64_t)page_count * 16);
    if (m_scan_candidate_count > sparse_candidate_limit) {
        return;
    }

    try {
        m_scan_needed_pages.assign(page_count, 0);
    } catch (const std::bad_alloc &) {
        // Allocation failure is not fatal; the dense reader is always valid.
        m_scan_needed_pages.clear();
        return;
    }

    uint64_t needed = 0;
    auto mark_slot = [&](uint64_t slot) {
        const uint64_t addr64 =
            (uint64_t)m_scan_first_addr + slot * m_scan_step;
        const uint32_t first_page = (uint32_t)(addr64 >> 12);
        const uint32_t last_page = (uint32_t)(
            (addr64 + m_scan_item_size - 1) >> 12);
        for (uint32_t page = first_page; page <= last_page; ++page) {
            if (page < m_scan_valid_first_page) continue;
            const uint64_t idx64 =
                (uint64_t)page - m_scan_valid_first_page;
            if (idx64 >= m_scan_needed_pages.size()) continue;
            uint8_t &flag = m_scan_needed_pages[(size_t)idx64];
            if (!flag) {
                flag = 1;
                ++needed;
            }
        }
    };

    // If the sparse-word index was intentionally dropped, building the page
    // map would require walking the entire bitset. Only do that for a truly
    // tiny population where the read reduction can still dominate the walk.
    if (!m_scan_candidate_words_valid && m_scan_candidate_count > 65536) {
        m_scan_needed_pages.clear();
        return;
    }

    bool page_map_too_dense = false;
    auto page_map_is_dense = [&]() {
        if (needed * 4 >= (uint64_t)page_count * 3) {
            page_map_too_dense = true;
            return true;
        }
        return false;
    };

    if (m_scan_candidate_words_valid) {
        for (uint32_t wi32 : m_scan_candidate_words) {
            const size_t wi = wi32;
            if (wi >= m_scan_candidate_bits.size()) continue;
            uint64_t word = m_scan_candidate_bits[wi];
            while (word) {
                const unsigned bit = Ctz64(word);
                word &= word - 1;
                const uint64_t slot = (uint64_t)wi * 64 + bit;
                if (slot >= m_scan_candidate_slots) break;
                mark_slot(slot);
                if (page_map_is_dense()) break;
            }
            if (page_map_too_dense) break;
        }
    } else {
        for (size_t wi = 0; wi < m_scan_candidate_bits.size(); ++wi) {
            uint64_t word = m_scan_candidate_bits[wi];
            while (word) {
                const unsigned bit = Ctz64(word);
                word &= word - 1;
                const uint64_t slot = (uint64_t)wi * 64 + bit;
                if (slot >= m_scan_candidate_slots) break;
                mark_slot(slot);
                if (page_map_is_dense()) break;
            }
            if (page_map_too_dense) break;
        }
    }

    // If the candidate set touches most pages, one contiguous transfer wins
    // despite reading a few unnecessary pages. Keep sparse mode for the cases
    // where it materially reduces guest-memory traffic.
    if (page_map_too_dense) {
        m_scan_needed_pages.clear();
        return;
    }

    m_scan_sparse_read = true;
    m_scan_io_pages_total = needed;
}

bool DisassemblerWindow::StartMemoryNextScan()
{
    if (m_scan_phase != MemScanPhase::Idle || !m_scan_baseline_ready) return false;
    m_scan_operation_compare = m_scan_compare;
    m_scan_operation_unknown = false;

    std::string error;
    if (!ParseMemoryScanTarget(false, &error)) {
        m_scan_status = error;
        return false;
    }

    const size_t bytes = (size_t)((uint64_t)m_scan_range_hi - m_scan_range_lo);
    const size_t pages = m_scan_baseline_valid_pages.size();
    try {
        // m_scan_work was the previous-value display snapshot after the last
        // completed Next Scan. Reusing it here keeps peak snapshot memory at
        // exactly two scan-range images rather than three.
        // Only pages containing surviving candidates may be read below, so do
        // not memset an entire 64/128 MiB work image just to overwrite a tiny
        // fraction of it. Validity bits make untouched bytes unreachable.
        m_scan_work.resize(bytes);
        m_scan_work_valid_pages.assign(pages, 0);
        m_scan_work_candidate_bits.assign(m_scan_candidate_bits.size(), 0);
    } catch (const std::bad_alloc &) {
        m_scan_status = "not enough host memory for the next snapshot";
        return false;
    }

    m_scan_have_previous_display = false;
    m_scan_work_all_valid = false;
    m_scan_work_candidate_count = 0;
    m_scan_io_cursor = m_scan_range_lo;
    m_scan_io_page_index = 0;
    m_scan_io_pages_done = 0;
    m_scan_io_pages_total = pages;
    m_scan_eval_slot = 0;
    m_scan_eval_word_cursor = 0;
    m_scan_work_candidate_words.clear();
    m_scan_work_candidate_words_valid = true;
    PrepareMemoryNextReadPlan();
    xemu_guestmem_invalidate_cache();
    m_scan_phase = MemScanPhase::ReadNext;
    m_scan_status = "Reading next-scan snapshot...";
    return true;
}

bool DisassemblerWindow::MemoryScanCandidateValid(
    const std::vector<uint8_t> &valid_pages, uint32_t addr, size_t len) const
{
    if (len == 0 || addr < m_scan_range_lo ||
        (uint64_t)addr + len > m_scan_range_hi) return false;
    const uint32_t first = addr >> 12;
    const uint32_t last = (uint32_t)(((uint64_t)addr + len - 1) >> 12);
    for (uint32_t p = first; p <= last; ++p) {
        if (p < m_scan_valid_first_page) return false;
        const uint64_t idx = (uint64_t)p - m_scan_valid_first_page;
        if (idx >= valid_pages.size() || !valid_pages[(size_t)idx]) return false;
    }
    return true;
}

bool DisassemblerWindow::MemoryScanBit(uint64_t slot) const
{
    const size_t word = (size_t)(slot >> 6);
    if (word >= m_scan_candidate_bits.size()) return false;
    return (m_scan_candidate_bits[word] & (UINT64_C(1) << (slot & 63))) != 0;
}

void DisassemblerWindow::SetMemoryScanBit(uint64_t slot, bool value)
{
    const size_t word = (size_t)(slot >> 6);
    if (word >= m_scan_candidate_bits.size()) return;
    const uint64_t mask = UINT64_C(1) << (slot & 63);
    if (value) m_scan_candidate_bits[word] |= mask;
    else m_scan_candidate_bits[word] &= ~mask;
}

static bool MemScanAobMatches(const uint8_t *p, const std::vector<uint8_t> &bytes,
                              const std::vector<uint8_t> &mask,
                              int anchor = -1, uint8_t anchor_byte = 0)
{
    if (bytes.size() != mask.size()) return false;
    if (anchor >= 0 && (size_t)anchor < bytes.size() &&
        p[anchor] != anchor_byte) {
        return false;
    }
    for (size_t i = 0; i < bytes.size(); ++i) {
        if ((int)i == anchor) continue;
        if (mask[i] && p[i] != bytes[i]) return false;
    }
    return true;
}

bool DisassemblerWindow::MemoryScanFirstMatch(uint32_t addr) const
{
    if (!m_scan_baseline_all_valid &&
        !MemoryScanCandidateValid(m_scan_baseline_valid_pages, addr,
                                  m_scan_item_size)) return false;
    if (m_scan_operation_unknown) return true;

    const uint8_t *p = m_scan_baseline.data() + ((uint64_t)addr - m_scan_range_lo);
    const int type = m_scan_locked_type;
    const int cmp = m_scan_operation_compare;

    if (type == 5) {
        const bool eq = memcmp(p, m_scan_target_bytes.data(), m_scan_item_size) == 0;
        return cmp == 1 ? !eq : eq;
    }
    if (type == 6) {
        const bool eq = MemScanAobMatches(p, m_scan_target_bytes,
                                          m_scan_target_mask,
                                          m_scan_aob_anchor,
                                          m_scan_aob_anchor_byte);
        return cmp == 1 ? !eq : eq;
    }

    if (MemScanIsFloat(type)) {
        const double v = type == 3 ? (double)ReadLeFloat32(p) : ReadLeFloat64(p);
        switch (cmp) {
        case 0: return std::fabs(v - m_scan_target_f64) < 0.001;
        case 1: return std::fabs(v - m_scan_target_f64) >= 0.001;
        case 2: return v < m_scan_target_f64;
        case 3: return v > m_scan_target_f64;
        case 4: return v >= m_scan_target_f64 && v <= m_scan_target2_f64;
        default: return false;
        }
    }

    uint64_t v = type == 0 ? p[0] : type == 1 ? ReadLe16(p) : ReadLe32(p);
    switch (cmp) {
    case 0: return v == m_scan_target_u64;
    case 1: return v != m_scan_target_u64;
    case 2: return v < m_scan_target_u64;
    case 3: return v > m_scan_target_u64;
    case 4: return v >= m_scan_target_u64 && v <= m_scan_target2_u64;
    default: return false;
    }
}

bool DisassemblerWindow::MemoryScanNextMatch(uint32_t addr) const
{
    if (!m_scan_work_all_valid &&
        !MemoryScanCandidateValid(m_scan_work_valid_pages, addr,
                                  m_scan_item_size)) return false;
    const uint64_t rel = (uint64_t)addr - m_scan_range_lo;
    const uint8_t *oldp = m_scan_baseline.data() + rel;
    const uint8_t *newp = m_scan_work.data() + rel;
    const int type = m_scan_locked_type;
    const int cmp = m_scan_operation_compare;

    if (type == 5 || type == 6) {
        bool target_eq = false;
        if (cmp == 0 || cmp == 1) {
            target_eq = type == 5
                ? memcmp(newp, m_scan_target_bytes.data(), m_scan_item_size) == 0
                : MemScanAobMatches(newp, m_scan_target_bytes,
                                    m_scan_target_mask,
                                    m_scan_aob_anchor,
                                    m_scan_aob_anchor_byte);
        }
        const bool same = memcmp(oldp, newp, m_scan_item_size) == 0;
        if (cmp == 0) return target_eq;
        if (cmp == 1) return !target_eq;
        if (cmp == 9) return !same;
        if (cmp == 10) return same;
        return false;
    }

    if (MemScanIsFloat(type)) {
        const double oldv = type == 3 ? (double)ReadLeFloat32(oldp) : ReadLeFloat64(oldp);
        const double newv = type == 3 ? (double)ReadLeFloat32(newp) : ReadLeFloat64(newp);
        switch (cmp) {
        case 0: return std::fabs(newv - m_scan_target_f64) < 0.001;
        case 1: return std::fabs(newv - m_scan_target_f64) >= 0.001;
        case 2: return newv < oldv;
        case 3: return newv > oldv;
        case 4: return newv >= m_scan_target_f64 && newv <= m_scan_target2_f64;
        case 5: return newv > oldv;
        case 6: return newv < oldv;
        case 7: return std::fabs((newv - oldv) - m_scan_target_f64) < 0.001;
        case 8: return std::fabs((oldv - newv) - m_scan_target_f64) < 0.001;
        case 9: return std::fabs(newv - oldv) >= 0.001;
        case 10: return std::fabs(newv - oldv) < 0.001;
        default: return false;
        }
    }

    const uint64_t oldv = type == 0 ? oldp[0] : type == 1 ? ReadLe16(oldp) : ReadLe32(oldp);
    const uint64_t newv = type == 0 ? newp[0] : type == 1 ? ReadLe16(newp) : ReadLe32(newp);
    switch (cmp) {
    case 0: return newv == m_scan_target_u64;
    case 1: return newv != m_scan_target_u64;
    case 2: return newv < oldv;
    case 3: return newv > oldv;
    case 4: return newv >= m_scan_target_u64 && newv <= m_scan_target2_u64;
    case 5: return newv > oldv;
    case 6: return newv < oldv;
    case 7: return newv >= oldv && (newv - oldv) == m_scan_target_u64;
    case 8: return oldv >= newv && (oldv - newv) == m_scan_target_u64;
    case 9: return newv != oldv;
    case 10: return newv == oldv;
    default: return false;
    }
}

void DisassemblerWindow::TickMemorySearch()
{
    if (m_scan_phase == MemScanPhase::Idle) return;
    const uint64_t budget_start = SDL_GetPerformanceCounter();

    if (m_scan_phase == MemScanPhase::ReadFirst ||
        m_scan_phase == MemScanPhase::ReadNext) {
        std::vector<uint8_t> &snapshot =
            m_scan_phase == MemScanPhase::ReadFirst ? m_scan_baseline : m_scan_work;
        std::vector<uint8_t> &valid_pages =
            m_scan_phase == MemScanPhase::ReadFirst
                ? m_scan_baseline_valid_pages : m_scan_work_valid_pages;

        if (m_scan_phase == MemScanPhase::ReadNext && m_scan_sparse_read) {
            /*
             * Sparse Next Scan: read only pages that can contain a surviving
             * candidate. Physical pages are coalesced into <=1 MiB runs;
             * virtual pages stay page-sized so one unmapped page cannot hide
             * a later valid mapping in the same transfer.
             */
            while (m_scan_io_page_index < m_scan_needed_pages.size() &&
                   !ScanBudgetExpired(budget_start)) {
                while (m_scan_io_page_index < m_scan_needed_pages.size() &&
                       !m_scan_needed_pages[m_scan_io_page_index]) {
                    ++m_scan_io_page_index;
                }
                if (m_scan_io_page_index >= m_scan_needed_pages.size()) break;

                const size_t run_begin = m_scan_io_page_index;
                const size_t max_run_pages = m_scan_virtual ? 1 : 256;
                size_t run_end = run_begin + 1;
                while (run_end < m_scan_needed_pages.size() &&
                       run_end - run_begin < max_run_pages &&
                       m_scan_needed_pages[run_end]) {
                    ++run_end;
                }

                const uint64_t page0 =
                    (uint64_t)m_scan_valid_first_page + run_begin;
                const uint64_t page1 =
                    (uint64_t)m_scan_valid_first_page + run_end;
                const uint64_t addr = std::max<uint64_t>(
                    m_scan_range_lo, page0 << 12);
                const uint64_t end = std::min<uint64_t>(
                    m_scan_range_hi, page1 << 12);
                const size_t want = (size_t)(end - addr);
                uint8_t *dst = snapshot.data() + (addr - m_scan_range_lo);
                const ssize_t got = xemu_dbg_read_space(
                    (uint32_t)addr, dst, want, m_scan_virtual);

                if (!m_scan_virtual && got != (ssize_t)want) {
                    CancelMemorySearch();
                    m_scan_status =
                        "physical RAM read failed during memory scan";
                    return;
                }

                if (got == (ssize_t)want) {
                    for (size_t pi = run_begin; pi < run_end; ++pi) {
                        valid_pages[pi] = 1;
                    }
                } else {
                    memset(dst, 0, want);
                }
                m_scan_io_pages_done += run_end - run_begin;
                m_scan_io_page_index = run_end;
            }

            if (m_scan_io_page_index >= m_scan_needed_pages.size()) {
                m_scan_io_cursor = m_scan_range_hi;
            }
        } else {
            while (m_scan_io_cursor < m_scan_range_hi &&
                   !ScanBudgetExpired(budget_start)) {
                const uint64_t addr = m_scan_io_cursor;
                size_t want = 0;
                if (m_scan_virtual) {
                    const uint64_t page_end =
                        (addr & ~UINT64_C(0xFFF)) + 0x1000;
                    want = (size_t)std::min<uint64_t>(page_end - addr,
                                                      m_scan_range_hi - addr);
                } else {
                    want = (size_t)std::min<uint64_t>(1024 * 1024,
                                                      m_scan_range_hi - addr);
                }

                uint8_t *dst = snapshot.data() + (addr - m_scan_range_lo);
                const ssize_t got = xemu_dbg_read_space(
                    (uint32_t)addr, dst, want, m_scan_virtual);
                if (!m_scan_virtual && got != (ssize_t)want) {
                    CancelMemorySearch();
                    m_scan_status =
                        "physical RAM read failed during memory scan";
                    return;
                }

                if (got == (ssize_t)want) {
                    const uint32_t p0 = (uint32_t)addr >> 12;
                    const uint32_t p1 =
                        (uint32_t)((addr + want - 1) >> 12);
                    for (uint32_t pg = p0; pg <= p1; ++pg) {
                        const uint64_t idx =
                            (uint64_t)pg - m_scan_valid_first_page;
                        if (idx < valid_pages.size()) {
                            valid_pages[(size_t)idx] = 1;
                        }
                    }
                } else {
                    memset(dst, 0, want);
                }
                m_scan_io_cursor += want;
            }
        }

        if (m_scan_io_cursor >= m_scan_range_hi) {
            m_scan_eval_slot = 0;
            m_scan_eval_word_cursor = 0;
            const bool all_valid = !valid_pages.empty() &&
                std::all_of(valid_pages.begin(), valid_pages.end(),
                            [](uint8_t v) { return v != 0; });
            if (m_scan_phase == MemScanPhase::ReadFirst) {
                m_scan_baseline_all_valid = all_valid;
                m_scan_phase = MemScanPhase::EvalFirst;
                m_scan_status = "Evaluating first scan...";
            } else {
                m_scan_work_all_valid = all_valid;
                m_scan_phase = MemScanPhase::EvalNext;
                m_scan_status = "Filtering previous candidates...";
            }
        }
        return;
    }

    if (m_scan_phase == MemScanPhase::EvalFirst) {
        auto finish_first_scan = [&]() {
            m_scan_phase = MemScanPhase::Idle;
            m_scan_baseline_ready = true;
            m_scan_have_previous_display = false;
            ++m_scan_generation;
            m_scan_page_index = 0;
            m_scan_page_generation = 0;
            char tmp[160];
            snprintf(tmp, sizeof(tmp),
                     m_scan_operation_unknown
                         ? "Unknown baseline anchored: %" PRIu64 " candidate(s)"
                         : "First scan complete: %" PRIu64 " match(es)",
                     m_scan_candidate_count);
            m_scan_status = tmp;
        };

        auto remember_nonempty_word = [&](size_t wi) {
            if (!m_scan_candidate_words_valid ||
                m_scan_candidate_bits[wi] != 0) {
                return;
            }
            if (m_scan_candidate_words.size() <
                kMemScanCandidateWordIndexLimit) {
                m_scan_candidate_words.push_back((uint32_t)wi);
            } else {
                m_scan_candidate_words.clear();
                m_scan_candidate_words_valid = false;
            }
        };

        // The default Unknown scan over physical RAM has no holes. Once the
        // snapshot read proved every page valid, every aligned slot is a
        // candidate; fill the bitset directly instead of visiting 64/128M
        // individual byte positions just to set their bits.
        if (m_scan_operation_unknown && m_scan_eval_slot == 0 &&
            m_scan_baseline_all_valid) {
            std::fill(m_scan_candidate_bits.begin(),
                      m_scan_candidate_bits.end(), UINT64_MAX);
            if (!m_scan_candidate_bits.empty() &&
                (m_scan_candidate_slots & 63) != 0) {
                m_scan_candidate_bits.back() =
                    (UINT64_C(1) << (m_scan_candidate_slots & 63)) - 1;
            }
            m_scan_candidate_count = m_scan_candidate_slots;
            m_scan_candidate_words.clear();
            m_scan_candidate_words_valid = false;
            m_scan_eval_slot = m_scan_candidate_slots;
        }

        /* Exact int8 First Scan is common for cheat hunting. A libc memchr
         * walk is dramatically faster for the normal sparse-byte case, but it
         * becomes slower when the target byte is extremely common. Sample the
         * snapshot first and use the fast path only when it is a measured win;
         * otherwise fall through to the ordinary exact evaluator. */
        if (!m_scan_operation_unknown && m_scan_baseline_all_valid &&
            m_scan_locked_type == 0 &&
            (m_scan_operation_compare == 0 ||
             m_scan_operation_compare == 1)) {
            const uint8_t target = (uint8_t)m_scan_target_u64;
            const uint8_t *base = m_scan_baseline.data() +
                ((uint64_t)m_scan_first_addr - m_scan_range_lo);

            if (m_scan_eval_slot == 0) {
                uint32_t histogram[256];
                const size_t sample_len = MemScanSampleHistogram(
                    base, (size_t)m_scan_candidate_slots, histogram);
                const size_t hits = histogram[target];

                // Equal stops being consistently profitable around a few
                // percent target density; Not Equal can tolerate more because
                // it starts from a dense bitset and only clears exact hits.
                const size_t denominator =
                    m_scan_operation_compare == 0 ? 32 : 8;
                m_scan_search_anchor =
                    hits * denominator <= sample_len ? 0 : -2;
                m_scan_search_anchor_byte = target;

                if (m_scan_search_anchor >= 0 &&
                    m_scan_operation_compare == 1) {
                    std::fill(m_scan_candidate_bits.begin(),
                              m_scan_candidate_bits.end(), UINT64_MAX);
                    if (!m_scan_candidate_bits.empty() &&
                        (m_scan_candidate_slots & 63) != 0) {
                        m_scan_candidate_bits.back() =
                            (UINT64_C(1) <<
                             (m_scan_candidate_slots & 63)) - 1;
                    }
                    m_scan_candidate_count = m_scan_candidate_slots;
                    m_scan_candidate_words.clear();
                    m_scan_candidate_words_valid = false;
                }
            }

            if (m_scan_search_anchor >= 0) {
                while (m_scan_eval_slot < m_scan_candidate_slots &&
                       !ScanBudgetExpired(budget_start)) {
                    const uint64_t chunk_begin = m_scan_eval_slot;
                    const uint64_t chunk_end = std::min<uint64_t>(
                        m_scan_candidate_slots, chunk_begin + 256 * 1024);
                    const uint8_t *search = base + chunk_begin;
                    size_t remaining = (size_t)(chunk_end - chunk_begin);
                    while (remaining) {
                        const uint8_t *hit = static_cast<const uint8_t *>(
                            memchr(search, target, remaining));
                        if (!hit) break;
                        const uint64_t slot = (uint64_t)(hit - base);
                        const size_t wi = (size_t)(slot >> 6);
                        const uint64_t mask = UINT64_C(1) << (slot & 63);
                        if (m_scan_operation_compare == 0) {
                            remember_nonempty_word(wi);
                            if (!(m_scan_candidate_bits[wi] & mask)) {
                                m_scan_candidate_bits[wi] |= mask;
                                ++m_scan_candidate_count;
                            }
                        } else if (m_scan_candidate_bits[wi] & mask) {
                            m_scan_candidate_bits[wi] &= ~mask;
                            --m_scan_candidate_count;
                        }
                        const size_t consumed = (size_t)(hit - search) + 1;
                        search += consumed;
                        remaining -= consumed;
                    }
                    m_scan_eval_slot = chunk_end;
                }
                if (m_scan_eval_slot >= m_scan_candidate_slots) {
                    finish_first_scan();
                }
                return;
            }
        }

        /* String/AOB Equal/Not-Equal over a hole-free snapshot can skip almost
         * every candidate with libc's vectorized memchr. Pick the least-common
         * literal byte from a small snapshot sample as the search anchor, then
         * run the exact existing verifier only on anchor hits. This changes no
         * wildcard or UTF-8-byte semantics and remains time-sliced. */
        if (!m_scan_operation_unknown && m_scan_baseline_all_valid &&
            (m_scan_locked_type == 5 || m_scan_locked_type == 6) &&
            (m_scan_operation_compare == 0 ||
             m_scan_operation_compare == 1)) {
            const bool is_aob = m_scan_locked_type == 6;
            const uint8_t *base = m_scan_baseline.data() +
                ((uint64_t)m_scan_first_addr - m_scan_range_lo);

            if (m_scan_eval_slot == 0 && !m_scan_target_bytes.empty()) {
                uint32_t histogram[256];
                const size_t sample_len = MemScanSampleHistogram(
                    base, (size_t)((uint64_t)m_scan_range_hi -
                                   m_scan_first_addr),
                    histogram);
                uint32_t best_count = UINT32_MAX;
                int best_anchor = -1;
                for (size_t i = 0; i < m_scan_target_bytes.size(); ++i) {
                    if (is_aob && !m_scan_target_mask[i]) {
                        continue;
                    }
                    const uint8_t b = m_scan_target_bytes[i];
                    if (histogram[b] < best_count) {
                        best_count = histogram[b];
                        best_anchor = (int)i;
                    }
                }
                m_scan_search_anchor = best_anchor;
                if (best_anchor >= 0) {
                    // memchr is a win only if the chosen literal is actually
                    // selective. Avoid a pathological every-byte hit stream
                    // (for example a zero-heavy pattern in zero-heavy memory).
                    if ((uint64_t)best_count * 16 > sample_len) {
                        m_scan_search_anchor = -2;
                    }
                    m_scan_search_anchor_byte =
                        m_scan_target_bytes[(size_t)best_anchor];
                    if (is_aob && m_scan_search_anchor >= 0) {
                        // Reusing the chosen literal for exact verification
                        // makes the generic/Next-Scan path reject on the same
                        // low-frequency byte too.
                        m_scan_aob_anchor = best_anchor;
                        m_scan_aob_anchor_byte = m_scan_search_anchor_byte;
                    }
                }

                if (m_scan_search_anchor == -1) {
                    // An all-wildcard AOB is equal at every valid position.
                    if (m_scan_operation_compare == 0) {
                        std::fill(m_scan_candidate_bits.begin(),
                                  m_scan_candidate_bits.end(), UINT64_MAX);
                        if (!m_scan_candidate_bits.empty() &&
                            (m_scan_candidate_slots & 63) != 0) {
                            m_scan_candidate_bits.back() =
                                (UINT64_C(1) <<
                                 (m_scan_candidate_slots & 63)) - 1;
                        }
                        m_scan_candidate_count = m_scan_candidate_slots;
                        m_scan_candidate_words_valid = false;
                    }
                    m_scan_eval_slot = m_scan_candidate_slots;
                } else if (m_scan_search_anchor >= 0 &&
                           m_scan_operation_compare == 1) {
                    // Not Equal normally keeps almost everything. Start with
                    // the dense answer and clear only exact anchor-verified
                    // matches rather than memcmp'ing every byte position.
                    std::fill(m_scan_candidate_bits.begin(),
                              m_scan_candidate_bits.end(), UINT64_MAX);
                    if (!m_scan_candidate_bits.empty() &&
                        (m_scan_candidate_slots & 63) != 0) {
                        m_scan_candidate_bits.back() =
                            (UINT64_C(1) <<
                             (m_scan_candidate_slots & 63)) - 1;
                    }
                    m_scan_candidate_count = m_scan_candidate_slots;
                    m_scan_candidate_words.clear();
                    m_scan_candidate_words_valid = false;
                }
            }

            if (m_scan_search_anchor >= 0) {
                while (m_scan_eval_slot < m_scan_candidate_slots &&
                       !ScanBudgetExpired(budget_start)) {
                    const uint64_t chunk_begin = m_scan_eval_slot;
                    const uint64_t chunk_end = std::min<uint64_t>(
                        m_scan_candidate_slots, chunk_begin + 256 * 1024);
                    const size_t anchor = (size_t)m_scan_search_anchor;
                    const uint8_t *search = base + chunk_begin + anchor;
                    size_t remaining = (size_t)(chunk_end - chunk_begin);

                    while (remaining) {
                        const uint8_t *hit = static_cast<const uint8_t *>(
                            memchr(search, m_scan_search_anchor_byte, remaining));
                        if (!hit) {
                            break;
                        }
                        const uint64_t slot = chunk_begin +
                            (uint64_t)(hit - (base + chunk_begin + anchor));
                        const uint8_t *candidate = base + slot;
                        const bool equal = is_aob
                            ? MemScanAobMatches(candidate, m_scan_target_bytes,
                                                m_scan_target_mask,
                                                m_scan_aob_anchor,
                                                m_scan_aob_anchor_byte)
                            : memcmp(candidate, m_scan_target_bytes.data(),
                                     m_scan_item_size) == 0;
                        if (equal) {
                            const size_t wi = (size_t)(slot >> 6);
                            const uint64_t mask = UINT64_C(1) << (slot & 63);
                            if (m_scan_operation_compare == 0) {
                                remember_nonempty_word(wi);
                                if (!(m_scan_candidate_bits[wi] & mask)) {
                                    m_scan_candidate_bits[wi] |= mask;
                                    ++m_scan_candidate_count;
                                }
                            } else if (m_scan_candidate_bits[wi] & mask) {
                                m_scan_candidate_bits[wi] &= ~mask;
                                --m_scan_candidate_count;
                            }
                        }
                        const size_t consumed = (size_t)(hit - search) + 1;
                        search += consumed;
                        remaining -= consumed;
                    }
                    m_scan_eval_slot = chunk_end;
                }
                if (m_scan_eval_slot >= m_scan_candidate_slots) {
                    finish_first_scan();
                }
                return;
            }
        }

        while (m_scan_eval_slot < m_scan_candidate_slots &&
               !ScanBudgetExpired(budget_start)) {
            const uint64_t stop = std::min<uint64_t>(
                m_scan_candidate_slots, m_scan_eval_slot + 32768);
            for (; m_scan_eval_slot < stop; ++m_scan_eval_slot) {
                const uint32_t addr = (uint32_t)(
                    (uint64_t)m_scan_first_addr + m_scan_eval_slot * m_scan_step);
                if (MemoryScanFirstMatch(addr)) {
                    const size_t wi = (size_t)(m_scan_eval_slot >> 6);
                    const uint64_t mask =
                        UINT64_C(1) << (m_scan_eval_slot & 63);
                    remember_nonempty_word(wi);
                    m_scan_candidate_bits[wi] |= mask;
                    ++m_scan_candidate_count;
                }
            }
        }
        if (m_scan_eval_slot >= m_scan_candidate_slots) {
            finish_first_scan();
        }
        return;
    }

    if (m_scan_phase == MemScanPhase::EvalNext) {
        auto process_word = [&](size_t wi) {
            uint64_t word = m_scan_candidate_bits[wi];
            while (word) {
                const unsigned bit = Ctz64(word);
                word &= word - 1;
                const uint64_t slot = (uint64_t)wi * 64 + bit;
                if (slot >= m_scan_candidate_slots) break;
                const uint32_t addr = (uint32_t)(
                    (uint64_t)m_scan_first_addr + slot * m_scan_step);
                if (MemoryScanNextMatch(addr)) {
                    if (m_scan_work_candidate_bits[wi] == 0 &&
                        m_scan_work_candidate_words_valid) {
                        if (m_scan_work_candidate_words.size() <
                            kMemScanCandidateWordIndexLimit) {
                            m_scan_work_candidate_words.push_back((uint32_t)wi);
                        } else {
                            m_scan_work_candidate_words.clear();
                            m_scan_work_candidate_words_valid = false;
                        }
                    }
                    m_scan_work_candidate_bits[wi] |= UINT64_C(1) << bit;
                    ++m_scan_work_candidate_count;
                }
            }
        };

        bool complete = false;
        if (m_scan_candidate_words_valid) {
            /*
             * Sparse result sets retain the exact non-empty bitset words. A
             * three-result scan over 128 MiB therefore touches only the words
             * containing those results, not all 2,097,152 int8 bitset words.
             */
            while (m_scan_eval_word_cursor < m_scan_candidate_words.size() &&
                   !ScanBudgetExpired(budget_start)) {
                const size_t stop = std::min<size_t>(
                    m_scan_candidate_words.size(), m_scan_eval_word_cursor + 512);
                for (; m_scan_eval_word_cursor < stop;
                     ++m_scan_eval_word_cursor) {
                    const size_t wi =
                        m_scan_candidate_words[m_scan_eval_word_cursor];
                    if (wi < m_scan_candidate_bits.size()) process_word(wi);
                }
            }
            complete = m_scan_eval_word_cursor >= m_scan_candidate_words.size();
            if (complete) {
                m_scan_eval_slot = m_scan_candidate_slots;
            } else if (!m_scan_candidate_words.empty()) {
                m_scan_eval_slot = (uint64_t)(
                    (long double)m_scan_candidate_slots *
                    (long double)m_scan_eval_word_cursor /
                    (long double)m_scan_candidate_words.size());
            }
        } else {
            // Dense fallback: sequential bitset traversal remains the fastest
            // path when too many words are populated to justify an index.
            while (m_scan_eval_slot < m_scan_candidate_slots &&
                   !ScanBudgetExpired(budget_start)) {
                size_t wi = (size_t)(m_scan_eval_slot >> 6);
                const size_t stop_word = std::min<size_t>(
                    m_scan_candidate_bits.size(), wi + 512);
                for (; wi < stop_word; ++wi) process_word(wi);
                m_scan_eval_slot = std::min<uint64_t>(
                    m_scan_candidate_slots, (uint64_t)wi * 64);
            }
            complete = m_scan_eval_slot >= m_scan_candidate_slots;
        }

        if (complete) {
            // New snapshot becomes the baseline for the following scan. The old
            // baseline moves into m_scan_work so the result table can show the
            // exact previous value without allocating a third RAM image.
            m_scan_baseline.swap(m_scan_work);
            m_scan_baseline_valid_pages.swap(m_scan_work_valid_pages);
            std::swap(m_scan_baseline_all_valid, m_scan_work_all_valid);
            m_scan_candidate_bits.swap(m_scan_work_candidate_bits);
            m_scan_candidate_words.swap(m_scan_work_candidate_words);
            std::swap(m_scan_candidate_words_valid,
                      m_scan_work_candidate_words_valid);
            m_scan_candidate_count = m_scan_work_candidate_count;
            m_scan_work_candidate_count = 0;
            m_scan_work_candidate_bits.clear();
            m_scan_work_candidate_words.clear();
            m_scan_work_candidate_words_valid = true;
            m_scan_needed_pages.clear();
            m_scan_sparse_read = false;
            m_scan_phase = MemScanPhase::Idle;
            m_scan_have_previous_display = true;

            // A later narrowing scan can remove the row the user had selected.
            // Do not leave the action buttons targeting a stale address that is
            // no longer part of the current result set.
            if (m_scan_have_selected) {
                bool survives = false;
                if (m_scan_selected_addr >= m_scan_first_addr) {
                    const uint64_t delta =
                        (uint64_t)m_scan_selected_addr - m_scan_first_addr;
                    if (m_scan_step != 0 && delta % m_scan_step == 0) {
                        const uint64_t slot = delta / m_scan_step;
                        survives = slot < m_scan_candidate_slots &&
                                   MemoryScanBit(slot);
                    }
                }
                if (!survives) m_scan_have_selected = false;
            }

            ++m_scan_generation;
            m_scan_page_index = 0;
            m_scan_page_generation = 0;
            char tmp[128];
            snprintf(tmp, sizeof(tmp), "Next scan complete: %" PRIu64 " match(es)",
                     m_scan_candidate_count);
            m_scan_status = tmp;
        }
    }
}

std::string DisassemblerWindow::FormatMemoryScanRawValue(
    const uint8_t *raw) const
{
    if (!raw) return "?";
    char out[128];
    switch (m_scan_locked_type) {
    case 0:
        snprintf(out, sizeof(out), m_scan_hex ? "%02X" : "%u",
                 (unsigned)raw[0]);
        return out;
    case 1: {
        uint16_t v = ReadLe16(raw);
        snprintf(out, sizeof(out), m_scan_hex ? "%04X" : "%u", (unsigned)v);
        return out;
    }
    case 2: {
        uint32_t v = ReadLe32(raw);
        snprintf(out, sizeof(out), m_scan_hex ? "%08X" : "%" PRIu32, v);
        return out;
    }
    case 3:
        snprintf(out, sizeof(out), "%.9g", (double)ReadLeFloat32(raw));
        return out;
    case 4:
        snprintf(out, sizeof(out), "%.17g", ReadLeFloat64(raw));
        return out;
    case 5:
        return CompactScanText(raw, m_scan_item_size);
    case 6:
        return HexBytes(raw, m_scan_item_size);
    default:
        return "?";
    }
}

std::string DisassemblerWindow::FormatMemoryScanSnapshotValue(
    const std::vector<uint8_t> &snapshot, uint32_t addr) const
{
    if (snapshot.empty() || addr < m_scan_range_lo ||
        (uint64_t)addr + m_scan_item_size > m_scan_range_hi) return "?";
    const uint8_t *p = snapshot.data() + ((uint64_t)addr - m_scan_range_lo);
    return FormatMemoryScanRawValue(p);
}

std::string DisassemblerWindow::FormatMemoryScanLiveValue(uint32_t addr) const
{
    if (m_scan_item_size == 0 || m_scan_item_size > 512) return "?";
    uint8_t raw[512];
    if (xemu_dbg_read_space(addr, raw, m_scan_item_size, m_scan_virtual) !=
        (ssize_t)m_scan_item_size) return "?";
    return FormatMemoryScanRawValue(raw);
}

void DisassemblerWindow::RefreshMemoryScanLiveValues(size_t begin, size_t end)
{
    if (begin >= end || begin >= m_scan_page_addresses.size()) return;
    end = std::min(end, m_scan_page_addresses.size());
    if (m_scan_live_values.size() < m_scan_page_addresses.size()) {
        m_scan_live_values.resize(m_scan_page_addresses.size());
    }

    // Rows on the same guest page are coalesced into one read, eliminating
    // dozens of tiny guest-memory calls in common result tables while keeping
    // values live every frame. The caller opens one translation-cache
    // generation for the whole clipped table pass.
    size_t i = begin;
    while (i < end) {
        const uint32_t addr = m_scan_page_addresses[i];
        if (m_scan_item_size == 0 || m_scan_item_size > 512) {
            m_scan_live_values[i++] = "?";
            continue;
        }

        const uint64_t item_end = (uint64_t)addr + m_scan_item_size;
        const uint64_t page_end = ((uint64_t)addr & ~UINT64_C(0xFFF)) + 0x1000;
        if (item_end > page_end) {
            m_scan_live_values[i] = FormatMemoryScanLiveValue(addr);
            ++i;
            continue;
        }

        size_t j = i + 1;
        while (j < end) {
            const uint32_t next = m_scan_page_addresses[j];
            if (((uint64_t)next >> 12) != ((uint64_t)addr >> 12) ||
                (uint64_t)next + m_scan_item_size > page_end) {
                break;
            }
            ++j;
        }

        const uint32_t last_addr = m_scan_page_addresses[j - 1];
        const size_t span = (size_t)((uint64_t)last_addr + m_scan_item_size - addr);
        uint8_t raw[4096];
        const ssize_t got = xemu_dbg_read_space(addr, raw, span, m_scan_virtual);
        if (got == (ssize_t)span) {
            for (size_t k = i; k < j; ++k) {
                const size_t off = (size_t)(m_scan_page_addresses[k] - addr);
                m_scan_live_values[k] = FormatMemoryScanRawValue(raw + off);
            }
        } else {
            // Page-granular mappings normally make this all-or-nothing. Keep
            // exact per-row fallback semantics for any unusual partial read.
            for (size_t k = i; k < j; ++k) {
                m_scan_live_values[k] =
                    FormatMemoryScanLiveValue(m_scan_page_addresses[k]);
            }
        }
        i = j;
    }
}

bool DisassemblerWindow::MemoryScanAddressToVirtual(uint32_t addr, uint32_t *va) const
{
    if (!va) return false;
    if (m_scan_virtual) {
        *va = addr;
        return true;
    }
    return xemu_dbg_to_virt(addr, va);
}

void DisassemblerWindow::RebuildMemoryScanPage()
{
    static constexpr uint64_t kPageRows = 1024;
    if (m_scan_page_generation == m_scan_generation) return;
    m_scan_page_addresses.clear();
    if (!m_scan_baseline_ready || m_scan_candidate_count == 0) {
        m_scan_page_generation = m_scan_generation;
        return;
    }

    const uint64_t page_count =
        (m_scan_candidate_count + kPageRows - 1) / kPageRows;
    if (m_scan_page_index >= page_count) m_scan_page_index = page_count - 1;
    uint64_t skip = m_scan_page_index * kPageRows;

    auto append_word = [&](size_t wi) {
        uint64_t word = m_scan_candidate_bits[wi];
        if (!word) return;
        const unsigned count = Popcount64(word);
        if (skip >= count) {
            skip -= count;
            return;
        }
        while (word && m_scan_page_addresses.size() < kPageRows) {
            const unsigned bit = Ctz64(word);
            word &= word - 1;
            if (skip) { --skip; continue; }
            const uint64_t slot = (uint64_t)wi * 64 + bit;
            if (slot >= m_scan_candidate_slots) break;
            const uint32_t addr = (uint32_t)(
                (uint64_t)m_scan_first_addr + slot * m_scan_step);
            m_scan_page_addresses.push_back(addr);
        }
    };

    if (m_scan_candidate_words_valid) {
        for (uint32_t wi32 : m_scan_candidate_words) {
            const size_t wi = wi32;
            if (wi >= m_scan_candidate_bits.size()) continue;
            append_word(wi);
            if (m_scan_page_addresses.size() >= kPageRows) break;
        }
    } else {
        for (size_t wi = 0; wi < m_scan_candidate_bits.size() &&
                            m_scan_page_addresses.size() < kPageRows; ++wi) {
            append_word(wi);
        }
    }
    m_scan_page_generation = m_scan_generation;
}

void DisassemblerWindow::DrawMemorySearch()
{
    const bool running = m_scan_phase != MemScanPhase::Idle;
    const bool locked = m_scan_baseline_ready || running;

    ImGui::BeginDisabled(locked);
    ImGui::SetNextItemWidth(130 * g_viewport_mgr.m_scale);
    ImGui::Combo("Value type", &m_scan_value_type, kMemScanTypeNames,
                 IM_ARRAYSIZE(kMemScanTypeNames));
    ImGui::SameLine();
    ImGui::SetNextItemWidth(235 * g_viewport_mgr.m_scale);
    ImGui::Combo("Scan region", &m_scan_region, kMemScanRegionNames,
                 IM_ARRAYSIZE(kMemScanRegionNames));
    ImGui::EndDisabled();

    if (!locked && m_scan_region == IM_ARRAYSIZE(kMemScanRegions) - 1) {
        ImGui::SetNextItemWidth(105 * g_viewport_mgr.m_scale);
        ImGui::InputText("Start##scanrange", m_scan_custom_lo,
                         sizeof(m_scan_custom_lo), ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(105 * g_viewport_mgr.m_scale);
        ImGui::InputText("End##scanrange", m_scan_custom_hi,
                         sizeof(m_scan_custom_hi), ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::SameLine();
        ImGui::Checkbox("Virtual range", &m_scan_custom_virtual);
    }

    const int active_type = m_scan_baseline_ready ? m_scan_locked_type
                                                  : m_scan_value_type;
    const bool value_used = !m_scan_baseline_ready
        ? m_scan_compare != 11
        : MemScanCompareNeedsTarget(m_scan_compare);
    ImGui::BeginDisabled(!value_used);
    ImGui::SetNextItemWidth(170 * g_viewport_mgr.m_scale);
    ImGui::InputText("Value##memscan", m_scan_value, sizeof(m_scan_value));
    if (m_scan_compare == 4) {
        ImGui::SameLine();
        ImGui::TextUnformatted("to");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(170 * g_viewport_mgr.m_scale);
        ImGui::InputText("##memscanmax", m_scan_value_max,
                         sizeof(m_scan_value_max));
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(!MemScanIsNumeric(active_type));
    ImGui::Checkbox("HEX", &m_scan_hex);
    ImGui::EndDisabled();

    ImGui::SetNextItemWidth(220 * g_viewport_mgr.m_scale);
    ImGui::Combo("Compare", &m_scan_compare, kMemScanCompareNames,
                 IM_ARRAYSIZE(kMemScanCompareNames));
    if (m_scan_baseline_ready && (m_scan_compare == 2 || m_scan_compare == 3)) {
        ImGui::SameLine();
        ImGui::TextDisabled("(compared with previous snapshot)");
    }

    if (!m_scan_baseline_ready) {
        ImGui::BeginDisabled(running);
        if (ImGui::Button("First Scan")) StartMemoryFirstScan();
        ImGui::EndDisabled();
    } else {
        // Cheat Engine/PCSX2-style placement: New Scan is the primary left
        // action; Next Scan sits immediately to its right.
        ImGui::BeginDisabled(running);
        if (ImGui::Button("New Scan")) ResetMemorySearch();
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(running || m_scan_compare == 11);
        if (ImGui::Button("Next Scan")) StartMemoryNextScan();
        ImGui::EndDisabled();
    }
    if (running) {
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) CancelMemorySearch();
    }

    float progress = 0.0f;
    if (running) {
        if (m_scan_phase == MemScanPhase::ReadFirst ||
            m_scan_phase == MemScanPhase::ReadNext) {
            if (m_scan_phase == MemScanPhase::ReadNext && m_scan_sparse_read) {
                progress = m_scan_io_pages_total
                    ? (float)((double)m_scan_io_pages_done /
                              (double)m_scan_io_pages_total)
                    : 1.0f;
            } else {
                const uint64_t span =
                    (uint64_t)m_scan_range_hi - m_scan_range_lo;
                progress = span
                    ? (float)((double)(m_scan_io_cursor - m_scan_range_lo) /
                              (double)span)
                    : 0.0f;
            }
        } else {
            progress = m_scan_candidate_slots
                ? (float)((double)m_scan_eval_slot / (double)m_scan_candidate_slots)
                : 0.0f;
        }
        ImGui::ProgressBar(std::max(0.0f, std::min(progress, 1.0f)),
                           ImVec2(-1, 0), m_scan_status.c_str());
    } else {
        ImGui::TextDisabled("%s", m_scan_status.c_str());
    }

    if (!m_scan_baseline_ready) {
        ImGui::Separator();
        ImGui::TextWrapped(
            "Integrated in-process scanner from the standalone Xemu Cheat Engine workflow. "
            "Unknown Value Search captures a baseline first; then narrow it with Changed, "
            "Unchanged, Increased, Decreased, exact values, or value deltas. int8/int16/int32 "
            "retain the Cheat Engine's unsigned little-endian semantics.");
        return;
    }

    ImGui::Separator();
    ImGui::Text("Matches: %" PRIu64, m_scan_candidate_count);
    ImGui::SameLine();
    ImGui::TextDisabled("| %s | %s | %08X-%08X",
                        kMemScanTypeNames[m_scan_locked_type],
                        m_scan_virtual ? "virtual" : "physical",
                        m_scan_range_lo, m_scan_range_hi);

    if (running) {
        ImGui::TextDisabled("Results are held stable until the current scan completes.");
        return;
    }

    static constexpr uint64_t kPageRows = 1024;
    const uint64_t page_count = m_scan_candidate_count
        ? (m_scan_candidate_count + kPageRows - 1) / kPageRows : 0;
    if (page_count > 1) {
        ImGui::BeginDisabled(m_scan_page_index == 0);
        if (ImGui::SmallButton("< Prev")) {
            --m_scan_page_index;
            m_scan_page_generation = 0;
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::Text("Page %" PRIu64 " / %" PRIu64,
                    m_scan_page_index + 1, page_count);
        ImGui::SameLine();
        ImGui::BeginDisabled(m_scan_page_index + 1 >= page_count);
        if (ImGui::SmallButton("Next >")) {
            ++m_scan_page_index;
            m_scan_page_generation = 0;
        }
        ImGui::EndDisabled();
    }

    RebuildMemoryScanPage();
    if (m_scan_candidate_count == 0) return;

    const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders |
                                  ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
                                  ImGuiTableFlags_SizingStretchProp;
    const float table_h = std::max(120.0f * g_viewport_mgr.m_scale,
                                   ImGui::GetContentRegionAvail().y -
                                   ImGui::GetFrameHeightWithSpacing() * 2.2f);
    if (ImGui::BeginTable("##memscanresults", 4, flags, ImVec2(0, table_h))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed,
                                115 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Current value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Previous value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Space", ImGuiTableColumnFlags_WidthFixed,
                                72 * g_viewport_mgr.m_scale);
        ImGui::TableHeadersRow();

        // Guest page tables can change while emulation runs. Invalidate once
        // for this visible-table refresh, not once per clipper span. Physical
        // scans do not use translation state at all.
        if (m_scan_virtual) {
            xemu_guestmem_invalidate_cache();
        }
        ImGuiListClipper clipper;
        clipper.Begin((int)m_scan_page_addresses.size());
        while (clipper.Step()) {
            RefreshMemoryScanLiveValues((size_t)clipper.DisplayStart,
                                        (size_t)clipper.DisplayEnd);
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const uint32_t addr = m_scan_page_addresses[(size_t)i];
                ImGui::PushID((int)addr);
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                char abuf[16];
                snprintf(abuf, sizeof(abuf), "%08X", addr);
                const bool selected = m_scan_have_selected &&
                                      m_scan_selected_addr == addr;
                if (ImGui::Selectable(abuf, selected,
                                      ImGuiSelectableFlags_SpanAllColumns |
                                      ImGuiSelectableFlags_AllowDoubleClick)) {
                    m_scan_selected_addr = addr;
                    m_scan_have_selected = true;
                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                        m_mem_addr = addr;
                        snprintf(m_mem_buf, sizeof(m_mem_buf), "%08X", addr);
                        m_mem_virtual = m_scan_virtual;
                        m_mem_region = 0;
                        m_mem_cache_valid = false;
                        m_mem_sel = addr;
                        m_mem_sel_anchor = addr;
                        m_mem_have_sel = true;
                        m_mem_keyboard_active = false;
                        m_open_memory_tab_requested = true;
                        m_status = "memory viewer positioned at scan result";
                        m_status_ms = SDL_GetTicks();
                    }
                }
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted(m_scan_live_values[(size_t)i].c_str());
                ImGui::TableSetColumnIndex(2);
                if (m_scan_have_previous_display && !m_scan_work.empty())
                    ImGui::TextUnformatted(
                        FormatMemoryScanSnapshotValue(m_scan_work, addr).c_str());
                else
                    ImGui::TextDisabled("--");
                ImGui::TableSetColumnIndex(3);
                ImGui::TextDisabled(m_scan_virtual ? "Virtual" : "Physical");
                ImGui::PopID();
            }
        }
        ImGui::EndTable();
    }

    if (!m_scan_have_selected) return;
    const uint32_t addr = m_scan_selected_addr;
    if (ImGui::SmallButton("Copy address")) {
        char tmp[16]; snprintf(tmp, sizeof(tmp), "%08X", addr);
        SetClipboard(tmp);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("View in Memory")) {
        m_mem_addr = addr;
        snprintf(m_mem_buf, sizeof(m_mem_buf), "%08X", addr);
        m_mem_virtual = m_scan_virtual;
        m_mem_region = 0;
        m_mem_cache_valid = false;
        m_mem_sel = addr;
        m_mem_sel_anchor = addr;
        m_mem_have_sel = true;
        m_mem_keyboard_active = false;
        m_open_memory_tab_requested = true;
        m_status = "memory viewer positioned at scan result";
        m_status_ms = SDL_GetTicks();
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Save address")) {
        AddScanResultToSavedAddresses(addr, m_scan_virtual,
                                      m_scan_locked_type,
                                      m_scan_item_size);
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Disassemble")) {
        uint32_t va = 0;
        if (MemoryScanAddressToVirtual(addr, &va)) GoTo(va, true);
        else {
            m_status = "no virtual mapping found for this physical result";
            m_status_ms = SDL_GetTicks();
        }
    }
    ImGui::SameLine();
    if (ImGui::SmallButton("Add global watch")) {
        uint32_t va = 0;
        if (MemoryScanAddressToVirtual(addr, &va)) {
            bool exists = false;
            for (const auto &g : m_globals) if (g.addr == va) exists = true;
            if (!exists) m_globals.push_back({va, "memory search result"});
            m_status = exists ? "global watch already exists" : "scan result added to Globals";
        } else {
            m_status = "could not find a virtual alias for this physical result";
        }
        m_status_ms = SDL_GetTicks();
    }

    auto add_watch = [&](const char *label, int watch_flags) {
        ImGui::SameLine();
        if (!ImGui::SmallButton(label)) return;
        const uint32_t len = std::max<uint32_t>(1, std::min<uint32_t>(m_scan_item_size, 8));
        bool duplicate = false;
        for (const auto &wp : m_wps) {
            if (wp.addr == addr && wp.len == len && wp.flags == watch_flags &&
                wp.virt == m_scan_virtual) { duplicate = true; break; }
        }
        if (duplicate) {
            m_status = "watchpoint already exists here";
        } else if (xemu_dbg_wp_insert_space(addr, len, watch_flags,
                                            m_scan_virtual)) {
            m_wps.push_back({addr, len, watch_flags, m_scan_virtual, true});
            m_status = "watchpoint added from memory-search result";
        } else {
            m_status = "could not add watchpoint at scan result";
        }
        m_status_ms = SDL_GetTicks();
    };
    add_watch("Break read##scan", BP_MEM_READ);
    add_watch("Break write##scan", BP_MEM_WRITE);
}


uint32_t DisassemblerWindow::DefaultSavedAddressSizeForType(int value_type) const
{
    switch (value_type) {
    case 0: return 1;
    case 1: return 2;
    case 2: return 4;
    case 3: return 4;
    case 4: return 8;
    case 5: return 16;
    case 6: return 4;
    default: return 4;
    }
}

void DisassemblerWindow::InvalidateSavedAddressIndex()
{
    m_saved_index_dirty = true;
}

void DisassemblerWindow::EnsureSavedAddressIndex()
{
    if (!m_saved_index_dirty) {
        return;
    }

    m_saved_index.clear();
    m_saved_children.clear();
    m_saved_groups_with_frozen_descendants.clear();
    m_saved_frozen_indices.clear();

    const size_t count = m_saved_addresses.size();
    m_saved_index.reserve(count * 2 + 1);
    m_saved_children.reserve(count + 1);
    m_saved_groups_with_frozen_descendants.reserve(count + 1);
    m_saved_frozen_indices.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const SavedAddress &entry = m_saved_addresses[i];
        // Match FindSavedAddress() for a malformed duplicate id: the first
        // occurrence remains authoritative.
        m_saved_index.emplace(entry.id, i);
        m_saved_children[entry.parent_id].push_back(entry.id);
        if (!entry.is_group && entry.frozen &&
            entry.frozen_bytes.size() == entry.byte_size) {
            m_saved_frozen_indices.push_back(i);
        }
    }

    // Group freeze checkboxes ask whether any descendant is frozen. Compute
    // that ancestry only when the table changes instead of re-walking the
    // entire hierarchy for every visible group every frame.
    for (size_t index : m_saved_frozen_indices) {
        uint64_t parent = m_saved_addresses[index].parent_id;
        size_t guard = 0;
        while (parent && guard++ <= count) {
            m_saved_groups_with_frozen_descendants.insert(parent);
            const auto it = m_saved_index.find(parent);
            if (it == m_saved_index.end() ||
                it->second >= m_saved_addresses.size()) {
                break;
            }
            parent = m_saved_addresses[it->second].parent_id;
        }
    }

    m_saved_index_dirty = false;
}

DisassemblerWindow::SavedAddress *DisassemblerWindow::FindSavedAddress(uint64_t id)
{
    EnsureSavedAddressIndex();
    const auto it = m_saved_index.find(id);
    if (it == m_saved_index.end() || it->second >= m_saved_addresses.size()) {
        return nullptr;
    }
    return &m_saved_addresses[it->second];
}

const DisassemblerWindow::SavedAddress *
DisassemblerWindow::FindSavedAddress(uint64_t id) const
{
    if (!m_saved_index_dirty) {
        const auto it = m_saved_index.find(id);
        if (it == m_saved_index.end() || it->second >= m_saved_addresses.size()) {
            return nullptr;
        }
        return &m_saved_addresses[it->second];
    }
    for (const auto &entry : m_saved_addresses) {
        if (entry.id == id) return &entry;
    }
    return nullptr;
}

bool DisassemblerWindow::SavedAddressSelected(uint64_t id) const
{
    return m_saved_selected_set.find(id) != m_saved_selected_set.end();
}

void DisassemblerWindow::SelectSavedAddress(uint64_t id, bool toggle)
{
    auto it = std::find(m_saved_selected.begin(), m_saved_selected.end(), id);
    if (toggle) {
        if (it == m_saved_selected.end()) {
            m_saved_selected.push_back(id);
            m_saved_selected_set.insert(id);
        } else {
            m_saved_selected.erase(it);
            m_saved_selected_set.erase(id);
        }
    } else {
        m_saved_selected.clear();
        m_saved_selected_set.clear();
        m_saved_selected.push_back(id);
        m_saved_selected_set.insert(id);
    }
}

static std::filesystem::path SavedAddressTablePath(uint32_t title_id)
{
    std::filesystem::path path(xemu_settings_get_base_path());
    path /= "debugger";
    path /= "saved-addresses";
    char name[32];
    snprintf(name, sizeof(name), "%08X.txt", title_id);
    path /= name;
    return path;
}

bool DisassemblerWindow::SaveSavedAddresses(std::string *error)
{
    // Most callers mutate the table immediately before saving. Mark retained
    // hierarchy/freeze caches dirty here as a central safety net so rollback
    // and future draws can never observe stale indices.
    InvalidateSavedAddressIndex();

    if (!m_saved_title_id) {
        if (error) *error = "no running Xbox title is identified";
        return false;
    }

    std::ostringstream out;
    out << "XEMU_SAVED_ADDRESSES_V1\t" << std::hex << std::uppercase
        << m_saved_title_id << std::dec << "\n";
    for (const auto &entry : m_saved_addresses) {
        out << (entry.is_group ? 'G' : 'A') << '\t'
            << entry.id << '\t' << entry.parent_id << '\t'
            << (entry.expanded ? 1 : 0) << '\t'
            << (entry.virt ? 1 : 0) << '\t'
            << entry.value_type << '\t'
            << entry.byte_size << '\t'
            << (entry.frozen ? 1 : 0) << '\t';
        char abuf[16];
        snprintf(abuf, sizeof(abuf), "%08X", entry.addr);
        out << abuf << '\t' << EscapeSavedField(entry.description) << '\t';
        for (uint8_t b : entry.frozen_bytes) {
            char byte[3];
            snprintf(byte, sizeof(byte), "%02X", b);
            out << byte;
        }
        out << '\n';
    }

    return WriteFileVerified(SavedAddressTablePath(m_saved_title_id),
                             out.str(), error);
}

bool DisassemblerWindow::LoadSavedAddresses(std::string *error)
{
    m_saved_addresses.clear();
    m_saved_selected.clear();
    m_saved_selected_set.clear();
    m_saved_next_id = 1;
    InvalidateSavedAddressIndex();

    if (!m_saved_title_id) return true;
    const auto path = SavedAddressTablePath(m_saved_title_id);
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        return true; // No table yet is normal.
    }

    std::string line;
    if (!std::getline(f, line) ||
        line.rfind("XEMU_SAVED_ADDRESSES_V1\t", 0) != 0) {
        if (error) *error = "saved-address table has an unsupported header";
        return false;
    }

    std::vector<SavedAddress> loaded;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        std::vector<std::string> fields;
        size_t start = 0;
        while (true) {
            size_t tab = line.find('\t', start);
            fields.push_back(line.substr(start,
                tab == std::string::npos ? std::string::npos : tab - start));
            if (tab == std::string::npos) break;
            start = tab + 1;
        }
        if (fields.size() < 11 || (fields[0] != "G" && fields[0] != "A")) {
            continue;
        }

        SavedAddress entry;
        entry.is_group = fields[0] == "G";
        char *end = nullptr;
        entry.id = strtoull(fields[1].c_str(), &end, 10);
        if (!end || *end || !entry.id) continue;
        entry.parent_id = strtoull(fields[2].c_str(), nullptr, 10);
        entry.expanded = strtol(fields[3].c_str(), nullptr, 10) != 0;
        entry.virt = strtol(fields[4].c_str(), nullptr, 10) != 0;
        entry.value_type = (int)strtol(fields[5].c_str(), nullptr, 10);
        entry.byte_size = (uint32_t)strtoul(fields[6].c_str(), nullptr, 10);
        entry.frozen = strtol(fields[7].c_str(), nullptr, 10) != 0;
        entry.addr = (uint32_t)strtoul(fields[8].c_str(), nullptr, 16);
        entry.description = UnescapeSavedField(fields[9]);
        if (entry.value_type < 0 ||
            entry.value_type >= IM_ARRAYSIZE(kMemScanTypeNames)) {
            entry.value_type = 2;
        }
        entry.byte_size = std::max<uint32_t>(1,
            std::min<uint32_t>(entry.byte_size, 256));

        const std::string &hex = fields[10];
        if (!hex.empty() && (hex.size() & 1) == 0) {
            entry.frozen_bytes.reserve(hex.size() / 2);
            bool ok = true;
            for (size_t i = 0; i < hex.size(); i += 2) {
                char pair[3] = { hex[i], hex[i + 1], 0 };
                if (!std::isxdigit((unsigned char)pair[0]) ||
                    !std::isxdigit((unsigned char)pair[1])) {
                    ok = false;
                    break;
                }
                entry.frozen_bytes.push_back(
                    (uint8_t)strtoul(pair, nullptr, 16));
            }
            if (!ok) entry.frozen_bytes.clear();
        }
        if (entry.is_group) {
            entry.frozen = false;
            entry.frozen_bytes.clear();
        } else if (entry.frozen &&
                   entry.frozen_bytes.size() != entry.byte_size) {
            // Never freeze with a partial/stale byte image.
            entry.frozen = false;
            entry.frozen_bytes.clear();
        }

        m_saved_next_id = std::max(m_saved_next_id, entry.id + 1);
        loaded.push_back(std::move(entry));
    }

    // Drop impossible parent links/cycles to keep recursive drawing safe.
    for (auto &entry : loaded) {
        if (entry.parent_id == entry.id) entry.parent_id = 0;
        if (entry.parent_id) {
            auto it = std::find_if(loaded.begin(), loaded.end(),
                [&](const SavedAddress &candidate) {
                    return candidate.id == entry.parent_id &&
                           candidate.is_group;
                });
            if (it == loaded.end()) entry.parent_id = 0;
        }

        uint64_t parent = entry.parent_id;
        size_t depth = 0;
        while (parent && depth++ <= loaded.size()) {
            if (parent == entry.id) {
                entry.parent_id = 0;
                break;
            }
            auto it = std::find_if(loaded.begin(), loaded.end(),
                [&](const SavedAddress &candidate) {
                    return candidate.id == parent && candidate.is_group;
                });
            if (it == loaded.end()) {
                entry.parent_id = 0;
                break;
            }
            parent = it->parent_id;
        }
        if (depth > loaded.size()) entry.parent_id = 0;
    }

    m_saved_addresses = std::move(loaded);
    InvalidateSavedAddressIndex();
    return true;
}

void DisassemblerWindow::EnsureSavedAddressesLoaded()
{
    const uint32_t now = SDL_GetTicks();
    if (m_saved_last_title_poll_ms &&
        now - m_saved_last_title_poll_ms < 500 && m_saved_loaded) {
        return;
    }
    m_saved_last_title_poll_ms = now;

    uint32_t title_id = 0;
    if (!xemu_get_xbe_title_id(&title_id) || !title_id) return;
    if (m_saved_loaded && title_id == m_saved_title_id) return;

    m_saved_title_id = title_id;
    m_saved_loaded = true;
    std::string error;
    if (!LoadSavedAddresses(&error)) {
        m_status = "Saved Addresses load failed: " + error;
        m_status_ms = now;
    }
}

void DisassemblerWindow::TickSavedAddresses()
{
    EnsureSavedAddressesLoaded();
    if (!m_saved_loaded || !m_saved_title_id) return;

    EnsureSavedAddressIndex();
    if (m_saved_frozen_indices.empty()) return;

    const uint32_t now = SDL_GetTicks();
    if (m_saved_last_freeze_ms && now - m_saved_last_freeze_ms < 16) return;
    m_saved_last_freeze_ms = now;

    // A disc/title transition must never let the old game's frozen table write
    // into the new title during the normal 500 ms identification cadence.
    // Pay for the title-id check only while at least one freeze is active.
    uint32_t current_title_id = 0;
    if (!xemu_get_xbe_title_id(&current_title_id) ||
        current_title_id != m_saved_title_id) {
        m_saved_last_title_poll_ms = 0;
        EnsureSavedAddressesLoaded();
        return;
    }

    xemu_guestmem_invalidate_cache();
    for (size_t index : m_saved_frozen_indices) {
        if (index >= m_saved_addresses.size()) continue;
        const SavedAddress &entry = m_saved_addresses[index];
        // The retained index is rebuilt whenever freeze/structure state
        // changes; keep this defensive check for malformed external files.
        if (entry.is_group || !entry.frozen ||
            entry.frozen_bytes.size() != entry.byte_size) {
            continue;
        }
        (void)xemu_dbg_write_space(entry.addr, entry.frozen_bytes.data(),
                                   entry.frozen_bytes.size(), entry.virt);
    }
}

std::string DisassemblerWindow::FormatSavedAddressValue(
    const SavedAddress &entry) const
{
    if (entry.is_group) return {};
    const uint32_t n = std::max<uint32_t>(1,
        std::min<uint32_t>(entry.byte_size, 256));
    // Saved rows are refreshed while drawing. Avoid a heap allocation for
    // every visible row every frame; byte_size is already hard-capped at 256.
    uint8_t raw[256] = {};
    if (xemu_dbg_read_space(entry.addr, raw, n, entry.virt) != (ssize_t)n) {
        return "??";
    }

    char buf[256];
    switch (entry.value_type) {
    case 0:
        snprintf(buf, sizeof(buf), "%u", (unsigned)raw[0]);
        return buf;
    case 1:
        if (n < 2) return "??";
        snprintf(buf, sizeof(buf), "%u", (unsigned)ReadLe16(raw));
        return buf;
    case 2:
        if (n < 4) return "??";
        snprintf(buf, sizeof(buf), "%u", ReadLe32(raw));
        return buf;
    case 3: {
        if (n < 4) return "??";
        float value;
        memcpy(&value, raw, sizeof(value));
        snprintf(buf, sizeof(buf), "%.9g", value);
        return buf;
    }
    case 4: {
        if (n < 8) return "??";
        double value;
        uint64_t bits = ReadLe64(raw);
        memcpy(&value, &bits, sizeof(value));
        snprintf(buf, sizeof(buf), "%.17g", value);
        return buf;
    }
    case 5: {
        std::string text;
        text.reserve(n);
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t c = raw[i];
            if (!c) break;
            text.push_back(c >= 0x20 && c != 0x7F ? (char)c : '.');
        }
        return text;
    }
    case 6:
    default:
        return HexBytes(raw, n);
    }
}

void DisassemblerWindow::SetSavedAddressFrozen(uint64_t id, bool frozen,
                                                bool recurse)
{
    InvalidateSavedAddressIndex();
    SavedAddress *entry = FindSavedAddress(id);
    if (!entry) return;
    if (entry->is_group) {
        if (!recurse) return;
        std::vector<uint64_t> children;
        for (const auto &candidate : m_saved_addresses) {
            if (candidate.parent_id == id) children.push_back(candidate.id);
        }
        for (uint64_t child : children) {
            SetSavedAddressFrozen(child, frozen, true);
        }
        return;
    }

    if (!frozen) {
        entry->frozen = false;
        entry->frozen_bytes.clear();
        return;
    }

    entry->frozen_bytes.resize(entry->byte_size);
    if (xemu_dbg_read_space(entry->addr, entry->frozen_bytes.data(),
                            entry->frozen_bytes.size(), entry->virt) !=
        (ssize_t)entry->frozen_bytes.size()) {
        entry->frozen = false;
        entry->frozen_bytes.clear();
        m_status = "could not freeze unreadable saved address";
        m_status_ms = SDL_GetTicks();
        return;
    }
    entry->frozen = true;
}

void DisassemblerWindow::DeleteSavedAddressSubtree(uint64_t id)
{
    std::vector<uint64_t> remove { id };
    for (size_t i = 0; i < remove.size(); ++i) {
        for (const auto &entry : m_saved_addresses) {
            if (entry.parent_id == remove[i]) remove.push_back(entry.id);
        }
    }
    m_saved_addresses.erase(
        std::remove_if(m_saved_addresses.begin(), m_saved_addresses.end(),
            [&](const SavedAddress &entry) {
                return std::find(remove.begin(), remove.end(), entry.id) !=
                       remove.end();
            }),
        m_saved_addresses.end());
    m_saved_selected.erase(
        std::remove_if(m_saved_selected.begin(), m_saved_selected.end(),
            [&](uint64_t selected) {
                return std::find(remove.begin(), remove.end(), selected) !=
                       remove.end();
            }),
        m_saved_selected.end());
    m_saved_selected_set.clear();
    m_saved_selected_set.insert(m_saved_selected.begin(), m_saved_selected.end());
    InvalidateSavedAddressIndex();
}

void DisassemblerWindow::OpenSavedAddressEditor(uint64_t id,
                                                 uint64_t parent_id,
                                                 bool group)
{
    m_saved_editor_id = id;
    m_saved_editor_parent_id = parent_id;
    m_saved_editor_is_group = group;
    m_saved_editor_error.clear();

    if (const SavedAddress *entry = FindSavedAddress(id)) {
        snprintf(m_saved_editor_desc, sizeof(m_saved_editor_desc), "%s",
                 entry->description.c_str());
        snprintf(m_saved_editor_addr, sizeof(m_saved_editor_addr), "%08X",
                 entry->addr);
        m_saved_editor_virtual = entry->virt;
        m_saved_editor_type = entry->value_type;
        m_saved_editor_size = (int)entry->byte_size;
    } else {
        snprintf(m_saved_editor_desc, sizeof(m_saved_editor_desc), "%s",
                 group ? "New Group" : "Saved Address");
        snprintf(m_saved_editor_addr, sizeof(m_saved_editor_addr), "%08X",
                 m_mem_have_sel ? m_mem_sel : m_mem_addr);
        m_saved_editor_virtual = m_mem_virtual;
        m_saved_editor_type = 2;
        m_saved_editor_size = 4;
    }
    m_saved_editor_open_requested = true;
}

void DisassemblerWindow::DrawSavedAddressEditor()
{
    if (m_saved_editor_open_requested) {
        ImGui::OpenPopup(m_saved_editor_is_group
                             ? "Saved Address Group"
                             : "Saved Address Entry");
        m_saved_editor_open_requested = false;
    }

    const char *popup = m_saved_editor_is_group
        ? "Saved Address Group" : "Saved Address Entry";
    if (!ImGui::BeginPopupModal(popup, nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::SetNextItemWidth(390 * g_viewport_mgr.m_scale);
    ImGui::InputText("Description", m_saved_editor_desc,
                     sizeof(m_saved_editor_desc));

    // Addresses and groups can be moved into any group. Nested groups are
    // arbitrary-depth; when editing a group, exclude itself and descendants
    // so the table can never acquire a parent cycle.
    auto group_is_descendant_of_editor = [&](uint64_t group_id) {
        if (!m_saved_editor_is_group || !m_saved_editor_id) return false;
        uint64_t cur = group_id;
        size_t guard = 0;
        while (cur && guard++ <= m_saved_addresses.size()) {
            if (cur == m_saved_editor_id) return true;
            const SavedAddress *g = FindSavedAddress(cur);
            cur = g ? g->parent_id : 0;
        }
        return false;
    };
    const SavedAddress *parent_entry =
        FindSavedAddress(m_saved_editor_parent_id);
    const char *parent_preview =
        parent_entry ? parent_entry->description.c_str() : "<Root>";
    ImGui::SetNextItemWidth(260 * g_viewport_mgr.m_scale);
    if (ImGui::BeginCombo("Parent group", parent_preview)) {
        const bool root_selected = m_saved_editor_parent_id == 0;
        if (ImGui::Selectable("<Root>", root_selected))
            m_saved_editor_parent_id = 0;
        for (const auto &group : m_saved_addresses) {
            if (!group.is_group ||
                group.id == m_saved_editor_id ||
                group_is_descendant_of_editor(group.id)) {
                continue;
            }
            const bool selected = m_saved_editor_parent_id == group.id;
            if (ImGui::Selectable(group.description.c_str(), selected))
                m_saved_editor_parent_id = group.id;
        }
        ImGui::EndCombo();
    }

    if (!m_saved_editor_is_group) {
        ImGui::SetNextItemWidth(150 * g_viewport_mgr.m_scale);
        ImGui::InputText("Address", m_saved_editor_addr,
                         sizeof(m_saved_editor_addr),
                         ImGuiInputTextFlags_CharsHexadecimal);
        ImGui::Checkbox("Virtual address", &m_saved_editor_virtual);
        ImGui::SetNextItemWidth(180 * g_viewport_mgr.m_scale);
        if (ImGui::Combo("Type", &m_saved_editor_type, kMemScanTypeNames,
                         IM_ARRAYSIZE(kMemScanTypeNames))) {
            m_saved_editor_size =
                (int)DefaultSavedAddressSizeForType(m_saved_editor_type);
        }
        if (m_saved_editor_type == 5 || m_saved_editor_type == 6) {
            ImGui::SetNextItemWidth(120 * g_viewport_mgr.m_scale);
            ImGui::InputInt("Byte length", &m_saved_editor_size);
            m_saved_editor_size = std::max(1, std::min(m_saved_editor_size, 256));
        }
    }

    if (!m_saved_editor_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                           m_saved_editor_error.c_str());
    }

    if (ImGui::Button("Save")) {
        if (!m_saved_editor_desc[0]) {
            m_saved_editor_error = "Description cannot be empty.";
        } else {
            SavedAddress candidate;
            SavedAddress *existing = FindSavedAddress(m_saved_editor_id);
            if (existing) candidate = *existing;
            else {
                candidate.id = m_saved_next_id++;
                candidate.parent_id = m_saved_editor_parent_id;
                candidate.is_group = m_saved_editor_is_group;
                candidate.expanded = true;
            }

            candidate.description = m_saved_editor_desc;
            candidate.parent_id = m_saved_editor_parent_id;
            if (!candidate.is_group) {
                char *end = nullptr;
                unsigned long addr =
                    strtoul(m_saved_editor_addr, &end, 16);
                if (!end || *end) {
                    m_saved_editor_error = "Address is not valid hexadecimal.";
                    ImGui::EndPopup();
                    return;
                }
                candidate.addr = (uint32_t)addr;
                candidate.virt = m_saved_editor_virtual;
                candidate.value_type = m_saved_editor_type;
                candidate.byte_size = (uint32_t)std::max(
                    1, std::min(m_saved_editor_size, 256));
                // Changing shape/address invalidates an old freeze image.
                if (!existing || existing->addr != candidate.addr ||
                    existing->virt != candidate.virt ||
                    existing->value_type != candidate.value_type ||
                    existing->byte_size != candidate.byte_size) {
                    candidate.frozen = false;
                    candidate.frozen_bytes.clear();
                }
            }

            std::vector<SavedAddress> old = m_saved_addresses;
            if (existing) *existing = candidate;
            else m_saved_addresses.push_back(std::move(candidate));

            std::string error;
            if (!SaveSavedAddresses(&error)) {
                m_saved_addresses = std::move(old);
                m_saved_editor_error = "Save failed: " + error;
            } else {
                m_status = "Saved Addresses table saved and verified";
                m_status_ms = SDL_GetTicks();
                ImGui::CloseCurrentPopup();
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}


void DisassemblerWindow::DrawSavedValueEditor()
{
    if (m_saved_value_open_requested) {
        m_saved_value_buf[0] = '\0';
        m_saved_value_error.clear();
        ImGui::OpenPopup("Set Saved Address Value");
        m_saved_value_open_requested = false;
    }

    if (!ImGui::BeginPopupModal("Set Saved Address Value", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::TextWrapped(
        "The same text is interpreted according to each selected row's own "
        "type, matching Cheat Engine-style bulk set-value behavior.");
    ImGui::SetNextItemWidth(360 * g_viewport_mgr.m_scale);
    ImGui::InputText("Value", m_saved_value_buf, sizeof(m_saved_value_buf));

    if (!m_saved_value_error.empty()) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "%s",
                           m_saved_value_error.c_str());
    }

    if (ImGui::Button("Write")) {
        std::vector<std::string> failures;
        bool changed_freeze = false;

        for (uint64_t id : m_saved_selected) {
            SavedAddress *entry = FindSavedAddress(id);
            if (!entry || entry->is_group) continue;

            std::vector<uint8_t> bytes;
            bool parsed = true;
            switch (entry->value_type) {
            case 0:
            case 1:
            case 2: {
                char *end = nullptr;
                errno = 0;
                int base = 10;
                if (m_saved_value_buf[0] == '0' &&
                    (m_saved_value_buf[1] == 'x' ||
                     m_saved_value_buf[1] == 'X')) {
                    base = 0;
                } else {
                    for (const char *p = m_saved_value_buf; *p; ++p) {
                        if ((*p >= 'a' && *p <= 'f') ||
                            (*p >= 'A' && *p <= 'F')) {
                            base = 16;
                            break;
                        }
                    }
                }
                unsigned long long value =
                    strtoull(m_saved_value_buf, &end, base);
                const uint64_t max_value = entry->value_type == 0
                    ? 0xFFULL : entry->value_type == 1
                    ? 0xFFFFULL : 0xFFFFFFFFULL;
                if (errno || !end || *end || value > max_value) {
                    parsed = false;
                    break;
                }
                const size_t n = entry->value_type == 0 ? 1 :
                                 entry->value_type == 1 ? 2 : 4;
                bytes.resize(n);
                for (size_t i = 0; i < n; ++i)
                    bytes[i] = (uint8_t)(value >> (i * 8));
                break;
            }
            case 3: {
                char *end = nullptr;
                errno = 0;
                float value = strtof(m_saved_value_buf, &end);
                if (errno || !end || *end) {
                    parsed = false;
                    break;
                }
                bytes.resize(sizeof(value));
                memcpy(bytes.data(), &value, sizeof(value));
                break;
            }
            case 4: {
                char *end = nullptr;
                errno = 0;
                double value = strtod(m_saved_value_buf, &end);
                if (errno || !end || *end) {
                    parsed = false;
                    break;
                }
                bytes.resize(sizeof(value));
                memcpy(bytes.data(), &value, sizeof(value));
                break;
            }
            case 5: {
                const size_t n = std::min<size_t>(
                    strlen(m_saved_value_buf), entry->byte_size);
                bytes.assign((const uint8_t *)m_saved_value_buf,
                             (const uint8_t *)m_saved_value_buf + n);
                break;
            }
            case 6:
                parsed = ParseHexBytes(m_saved_value_buf, &bytes);
                if (parsed && bytes.size() > entry->byte_size) parsed = false;
                break;
            default:
                parsed = false;
                break;
            }

            if (!parsed || bytes.empty()) {
                failures.push_back(entry->description + ": invalid value");
                continue;
            }

            if (xemu_dbg_write_space(entry->addr, bytes.data(), bytes.size(),
                                     entry->virt) != (ssize_t)bytes.size()) {
                failures.push_back(entry->description + ": write failed");
                continue;
            }

            if (entry->frozen) {
                entry->frozen_bytes.resize(entry->byte_size);
                if (xemu_dbg_read_space(entry->addr,
                                        entry->frozen_bytes.data(),
                                        entry->frozen_bytes.size(),
                                        entry->virt) !=
                    (ssize_t)entry->frozen_bytes.size()) {
                    entry->frozen = false;
                    entry->frozen_bytes.clear();
                    failures.push_back(entry->description +
                                       ": freeze refresh failed");
                }
                changed_freeze = true;
            }
        }

        if (changed_freeze) {
            std::string save_error;
            if (!SaveSavedAddresses(&save_error)) {
                failures.push_back("table save: " + save_error);
            }
        }

        m_mem_cache_valid = false;
        if (failures.empty()) {
            m_status = "Saved Address value(s) written";
            m_status_ms = SDL_GetTicks();
            ImGui::CloseCurrentPopup();
        } else {
            m_saved_value_error.clear();
            for (size_t i = 0; i < failures.size(); ++i) {
                if (i) m_saved_value_error += "\n";
                m_saved_value_error += failures[i];
                if (i >= 7 && failures.size() > 8) {
                    m_saved_value_error += "\n...";
                    break;
                }
            }
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
}

void DisassemblerWindow::AddScanResultToSavedAddresses(uint32_t addr,
                                                        bool virt,
                                                        int value_type,
                                                        uint32_t byte_size)
{
    EnsureSavedAddressesLoaded();
    if (!m_saved_title_id) {
        m_status = "cannot save address until a running title is identified";
        m_status_ms = SDL_GetTicks();
        return;
    }

    SavedAddress entry;
    entry.id = m_saved_next_id++;
    entry.description = "Memory Search result";
    entry.addr = addr;
    entry.virt = virt;
    entry.value_type = value_type;
    entry.byte_size = std::max<uint32_t>(1, std::min<uint32_t>(byte_size, 256));
    m_saved_addresses.push_back(entry);

    std::string error;
    if (!SaveSavedAddresses(&error)) {
        m_saved_addresses.pop_back();
        --m_saved_next_id;
        m_status = "Saved Address failed: " + error;
    } else {
        m_status = "Memory Search result added to Saved Addresses";
    }
    m_status_ms = SDL_GetTicks();
}

void DisassemblerWindow::DrawSavedAddressChildren(uint64_t parent_id, int depth)
{
    EnsureSavedAddressIndex();
    const auto children_it = m_saved_children.find(parent_id);
    if (children_it == m_saved_children.end()) return;

    // Snapshot just this parent's ids so an edit/delete triggered by a context
    // menu can invalidate/rebuild the retained index without invalidating the
    // active traversal. Scratch storage is retained per recursion depth, so
    // steady-state drawing performs no vector allocation here.
    if (m_saved_draw_children_scratch.size() <= (size_t)depth) {
        m_saved_draw_children_scratch.resize((size_t)depth + 1);
    }
    std::vector<uint64_t> &children = m_saved_draw_children_scratch[(size_t)depth];
    children.assign(children_it->second.begin(), children_it->second.end());

    for (uint64_t id : children) {
        EnsureSavedAddressIndex();
        const auto index_it = m_saved_index.find(id);
        if (index_it == m_saved_index.end() ||
            index_it->second >= m_saved_addresses.size()) {
            continue;
        }
        SavedAddress *entry = &m_saved_addresses[index_it->second];
        const bool entry_is_group = entry->is_group;

        ImGui::PushID((int)(id & 0x7FFFFFFF));
        ImGui::TableNextRow();

        ImGui::TableSetColumnIndex(0);
        if (entry_is_group) {
            const bool any_frozen =
                m_saved_groups_with_frozen_descendants.find(entry->id) !=
                m_saved_groups_with_frozen_descendants.end();
            bool freeze_group = any_frozen;
            if (ImGui::Checkbox("##freeze", &freeze_group)) {
                const std::vector<SavedAddress> old = m_saved_addresses;
                SetSavedAddressFrozen(entry->id, freeze_group, true);
                std::string error;
                if (!SaveSavedAddresses(&error)) {
                    m_saved_addresses = old;
                    InvalidateSavedAddressIndex();
                    m_status = "Saved Addresses freeze-state save failed: " + error;
                    m_status_ms = SDL_GetTicks();
                }
                entry = FindSavedAddress(id);
                if (!entry) { ImGui::PopID(); continue; }
            }
        } else {
            bool frozen = entry->frozen;
            if (ImGui::Checkbox("##freeze", &frozen)) {
                const std::vector<SavedAddress> old = m_saved_addresses;
                SetSavedAddressFrozen(entry->id, frozen, false);
                std::string error;
                if (!SaveSavedAddresses(&error)) {
                    m_saved_addresses = old;
                    InvalidateSavedAddressIndex();
                    m_status = "Saved Addresses freeze-state save failed: " + error;
                    m_status_ms = SDL_GetTicks();
                }
                entry = FindSavedAddress(id);
                if (!entry) { ImGui::PopID(); continue; }
            }
        }

        ImGui::TableSetColumnIndex(1);
        ImGui::Indent(depth * 14.0f * g_viewport_mgr.m_scale);
        bool selected = SavedAddressSelected(entry->id);
        if (entry_is_group) {
            ImGuiTreeNodeFlags tf = ImGuiTreeNodeFlags_SpanAvailWidth |
                                    ImGuiTreeNodeFlags_OpenOnArrow;
            if (entry->expanded) tf |= ImGuiTreeNodeFlags_DefaultOpen;
            if (selected) tf |= ImGuiTreeNodeFlags_Selected;
            bool open = ImGui::TreeNodeEx("##group", tf, "%s",
                                          entry->description.c_str());
            if (ImGui::IsItemClicked(ImGuiMouseButton_Left)) {
                SelectSavedAddress(entry->id, ImGui::GetIO().KeyCtrl);
            }
            entry->expanded = open;
            if (ImGui::BeginPopupContextItem("##savedctx")) {
                if (ImGui::MenuItem("Add address inside..."))
                    OpenSavedAddressEditor(0, entry->id, false);
                if (ImGui::MenuItem("Add subgroup..."))
                    OpenSavedAddressEditor(0, entry->id, true);
                if (ImGui::MenuItem("Rename / edit..."))
                    OpenSavedAddressEditor(entry->id, entry->parent_id, true);
                if (ImGui::MenuItem("Freeze descendants")) {
                    const std::vector<SavedAddress> old = m_saved_addresses;
                    SetSavedAddressFrozen(entry->id, true, true);
                    std::string error;
                    if (!SaveSavedAddresses(&error)) {
                        m_saved_addresses = old;
                        m_status = "Saved Addresses freeze-state save failed: " + error;
                        m_status_ms = SDL_GetTicks();
                    }
                }
                if (ImGui::MenuItem("Unfreeze descendants")) {
                    const std::vector<SavedAddress> old = m_saved_addresses;
                    SetSavedAddressFrozen(entry->id, false, true);
                    std::string error;
                    if (!SaveSavedAddresses(&error)) {
                        m_saved_addresses = old;
                        m_status = "Saved Addresses freeze-state save failed: " + error;
                        m_status_ms = SDL_GetTicks();
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete group and contents")) {
                    std::vector<SavedAddress> old = m_saved_addresses;
                    DeleteSavedAddressSubtree(entry->id);
                    std::string error;
                    if (!SaveSavedAddresses(&error)) {
                        m_saved_addresses = std::move(old);
                        m_status = "Delete failed to save: " + error;
                        m_status_ms = SDL_GetTicks();
                    }
                    ImGui::EndPopup();
                    if (open) ImGui::TreePop();
                    ImGui::Unindent(depth * 14.0f * g_viewport_mgr.m_scale);
                    ImGui::PopID();
                    return;
                }
                ImGui::EndPopup();
            }
            if (open) {
                DrawSavedAddressChildren(entry->id, depth + 1);
                ImGui::TreePop();
            }
        } else {
            if (ImGui::Selectable(entry->description.c_str(), selected,
                                  ImGuiSelectableFlags_SpanAllColumns |
                                  ImGuiSelectableFlags_AllowDoubleClick)) {
                SelectSavedAddress(entry->id, ImGui::GetIO().KeyCtrl);
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                    m_mem_addr = entry->addr;
                    m_mem_virtual = entry->virt;
                    m_mem_region = 0;
                    m_mem_cache_valid = false;
                    m_mem_sel = entry->addr;
                    m_mem_sel_anchor = entry->addr;
                    m_mem_have_sel = true;
                    m_mem_keyboard_active = false;
                    m_open_memory_tab_requested = true;
                    snprintf(m_mem_buf, sizeof(m_mem_buf), "%08X",
                             entry->addr);
                }
            }
            if (ImGui::BeginPopupContextItem("##savedctx")) {
                if (ImGui::MenuItem("Edit..."))
                    OpenSavedAddressEditor(entry->id, entry->parent_id, false);
                if (ImGui::MenuItem("View in Memory")) {
                    m_mem_addr = entry->addr;
                    m_mem_virtual = entry->virt;
                    m_mem_region = 0;
                    m_mem_cache_valid = false;
                    m_mem_sel = entry->addr;
                    m_mem_sel_anchor = entry->addr;
                    m_mem_have_sel = true;
                    m_mem_keyboard_active = false;
                    m_open_memory_tab_requested = true;
                    snprintf(m_mem_buf, sizeof(m_mem_buf), "%08X",
                             entry->addr);
                }
                if (ImGui::MenuItem(entry->frozen ? "Unfreeze" : "Freeze")) {
                    const std::vector<SavedAddress> old = m_saved_addresses;
                    SetSavedAddressFrozen(entry->id, !entry->frozen, false);
                    std::string error;
                    if (!SaveSavedAddresses(&error)) {
                        m_saved_addresses = old;
                        m_status = "Saved Addresses freeze-state save failed: " + error;
                        m_status_ms = SDL_GetTicks();
                    }
                }
                if (ImGui::MenuItem("Set value...")) {
                    SelectSavedAddress(entry->id, false);
                    m_saved_value_open_requested = true;
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Delete")) {
                    std::vector<SavedAddress> old = m_saved_addresses;
                    DeleteSavedAddressSubtree(entry->id);
                    std::string error;
                    if (!SaveSavedAddresses(&error)) {
                        m_saved_addresses = std::move(old);
                        m_status = "Delete failed to save: " + error;
                        m_status_ms = SDL_GetTicks();
                    }
                    ImGui::EndPopup();
                    ImGui::Unindent(depth * 14.0f * g_viewport_mgr.m_scale);
                    ImGui::PopID();
                    return;
                }
                ImGui::EndPopup();
            }
        }
        ImGui::Unindent(depth * 14.0f * g_viewport_mgr.m_scale);

        if (!entry_is_group) {
            // Re-acquire after context-menu/recursive operations because a
            // descendant deletion can reallocate the backing vector.
            entry = FindSavedAddress(id);
            if (!entry) {
                ImGui::PopID();
                continue;
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::Text("%08X", entry->addr);
            ImGui::TableSetColumnIndex(3);
            ImGui::TextDisabled("%s", entry->virt ? "Virtual" : "Physical");
            ImGui::TableSetColumnIndex(4);
            ImGui::TextDisabled("%s", kMemScanTypeNames[entry->value_type]);
            ImGui::TableSetColumnIndex(5);
            const std::string value = FormatSavedAddressValue(*entry);
            if (entry->frozen) {
                ImGui::TextColored(ImVec4(0.55f, 1.0f, 0.65f, 1.0f),
                                   "%s", value.c_str());
            } else {
                ImGui::TextUnformatted(value.c_str());
            }
        }
        ImGui::PopID();
    }
}

void DisassemblerWindow::DrawSavedAddresses()
{
    EnsureSavedAddressesLoaded();
    if (!m_saved_title_id) {
        ImGui::TextDisabled("Start a game to use per-title Saved Addresses.");
        return;
    }

    EnsureSavedAddressIndex();

    if (!ImGui::GetIO().WantTextInput &&
        ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
        ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_A)) {
        m_saved_selected.clear();
        m_saved_selected_set.clear();
        for (const auto &entry : m_saved_addresses) {
            if (!entry.is_group) {
                m_saved_selected.push_back(entry.id);
                m_saved_selected_set.insert(entry.id);
            }
        }
    }

    if (ImGui::Button("Add address..."))
        OpenSavedAddressEditor(0, 0, false);
    ImGui::SameLine();
    if (ImGui::Button("Add group..."))
        OpenSavedAddressEditor(0, 0, true);
    ImGui::SameLine();
    ImGui::BeginDisabled(m_saved_selected.empty());
    if (ImGui::Button("Freeze")) {
        const std::vector<SavedAddress> old = m_saved_addresses;
        for (uint64_t id : m_saved_selected)
            SetSavedAddressFrozen(id, true, true);
        std::string error;
        if (!SaveSavedAddresses(&error)) {
            m_saved_addresses = old;
            m_status = "Saved Addresses freeze-state save failed: " + error;
            m_status_ms = SDL_GetTicks();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Unfreeze")) {
        const std::vector<SavedAddress> old = m_saved_addresses;
        for (uint64_t id : m_saved_selected)
            SetSavedAddressFrozen(id, false, true);
        std::string error;
        if (!SaveSavedAddresses(&error)) {
            m_saved_addresses = old;
            m_status = "Saved Addresses freeze-state save failed: " + error;
            m_status_ms = SDL_GetTicks();
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Set value...")) {
        bool have_leaf = false;
        for (uint64_t id : m_saved_selected) {
            const SavedAddress *entry = FindSavedAddress(id);
            if (entry && !entry->is_group) {
                have_leaf = true;
                break;
            }
        }
        if (have_leaf) m_saved_value_open_requested = true;
    }
    ImGui::SameLine();
    if (ImGui::Button("Delete")) {
        std::vector<SavedAddress> old = m_saved_addresses;
        const std::vector<uint64_t> selected = m_saved_selected;
        for (uint64_t id : selected) DeleteSavedAddressSubtree(id);
        std::string error;
        if (!SaveSavedAddresses(&error)) {
            m_saved_addresses = std::move(old);
            m_status = "Saved Addresses delete failed: " + error;
        } else {
            m_saved_selected.clear();
            m_saved_selected_set.clear();
            m_status = "Saved Addresses deleted and saved";
        }
        m_status_ms = SDL_GetTicks();
    }
    ImGui::EndDisabled();

    ImGui::SameLine();
    ImGui::TextDisabled("Ctrl+click selects multiple | double-click address -> Memory");

    const ImGuiTableFlags flags =
        ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV |
        ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY |
        ImGuiTableFlags_SizingStretchProp;
    if (ImGui::BeginTable("##saved_addresses", 6, flags, ImVec2(0, -1))) {
        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Freeze", ImGuiTableColumnFlags_WidthFixed,
                                58 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Description", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed,
                                90 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Space", ImGuiTableColumnFlags_WidthFixed,
                                70 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed,
                                90 * g_viewport_mgr.m_scale);
        ImGui::TableSetupColumn("Value", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        DrawSavedAddressChildren(0, 0);
        ImGui::EndTable();
    }
    DrawSavedAddressEditor();
    DrawSavedValueEditor();
}

void DisassemblerWindow::DrawUpperLeftPane()
{
    if (!ImGui::BeginTabBar("##upper_debug_tabs")) return;
    if (ImGui::BeginTabItem("Registers")) {
        DrawRegisters();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Functions")) {
        DrawFunctionBrowser(0);
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Memory Search")) {
        DrawMemorySearch();
        ImGui::EndTabItem();
    }
    ImGui::EndTabBar();
}

void DisassemblerWindow::DrawBottomPanels()
{
    if (!ImGui::BeginTabBar("##debugtabs")) return;

    const ImGuiTabItemFlags memory_flags = m_open_memory_tab_requested
        ? ImGuiTabItemFlags_SetSelected : 0;
    if (ImGui::BeginTabItem("Memory", nullptr, memory_flags)) {
        m_open_memory_tab_requested = false;
        DrawMemory();
        ImGui::EndTabItem();
    }
    if (ImGui::BeginTabItem("Breakpoints")) { DrawBreakpoints(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Threads")) { DrawThreads(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Stack")) { DrawStack(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Saved Addresses")) { DrawSavedAddresses(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Globals")) { DrawGlobals(); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Locals")) { DrawFrameSlots(false); ImGui::EndTabItem(); }
    if (ImGui::BeginTabItem("Parameters")) { DrawFrameSlots(true); ImGui::EndTabItem(); }

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
    // Endianness changes only display/interpretation order; the cached raw
    // bytes remain identical, so do not force another guest-memory read.
    ImGui::Checkbox("Big endian", &m_mem_big_endian);

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
        const bool range_selected = m_mem_sel_anchor != m_mem_sel;
        details_reserve = ImGui::GetFrameHeightWithSpacing() *
                          (range_selected ? 3.0f : 2.0f);
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
        /* Read the visible dump in large spans, then preserve the original
         * row-prefix validity semantics when unpacking. The old path issued up
         * to 64 guest-memory calls per refresh even though this entire viewer
         * is at most 8 KiB. Physical RAM normally needs one read; virtual
         * memory needs only one read per guest page so an unmapped page cannot
         * hide a later valid mapping. */
        constexpr size_t kMaxVisibleBytes = 64u * 128u;
        std::array<uint8_t, kMaxVisibleBytes> packed;
        std::array<uint8_t, kMaxVisibleBytes> valid{};
        const size_t total = (size_t)visible_rows * (size_t)per;
        const bool wraps_address_space =
            (uint64_t)m_mem_addr + (uint64_t)total > UINT64_C(0x100000000);
        bool row_fallback = wraps_address_space;

        xemu_guestmem_invalidate_cache();
        if (!row_fallback && !m_mem_virtual) {
            const uint64_t ram_size = xemu_guest_ram_size();
            size_t full_rows = 0;
            if ((uint64_t)m_mem_addr < ram_size) {
                const uint64_t available = ram_size - (uint64_t)m_mem_addr;
                full_rows = std::min<size_t>(
                    (size_t)visible_rows, (size_t)(available / (uint64_t)per));
            }
            const size_t readable = full_rows * (size_t)per;
            if (readable) {
                const ssize_t got = xemu_dbg_read_space(
                    m_mem_addr, packed.data(), readable, false);
                if (got == (ssize_t)readable) {
                    memset(valid.data(), 1, readable);
                } else {
                    // Physical RAM reads are all-or-nothing. If an unexpected
                    // aggregate DMA failure occurs, preserve the old per-row
                    // behavior instead of turning one failed batch into many
                    // invisible rows.
                    row_fallback = true;
                }
            }
        } else if (!row_fallback) {
            size_t offset = 0;
            while (offset < total) {
                const uint64_t addr64 = (uint64_t)m_mem_addr + offset;
                const size_t page_left =
                    0x1000u - (size_t)(addr64 & UINT64_C(0xFFF));
                const size_t want = std::min(total - offset, page_left);
                const ssize_t got = xemu_dbg_read_space(
                    (uint32_t)addr64, packed.data() + offset, want, true);
                if (got > 0) {
                    const size_t good =
                        std::min<size_t>((size_t)got, want);
                    memset(valid.data() + offset, 1, good);
                }
                // Advance by the whole page fragment even when it is unmapped
                // so a later mapped virtual page still appears, exactly as the
                // previous row-by-row viewer did.
                offset += want;
            }
        }

        if (row_fallback) {
            for (int row = 0; row < visible_rows; ++row) {
                const uint32_t addr =
                    m_mem_addr + (uint32_t)(row * per);
                const ssize_t got = xemu_dbg_read_space(
                    addr, m_mem_cache[row], (size_t)per, m_mem_virtual);
                m_mem_cache_got[row] = got > 0 ? (int)got : 0;
            }
        } else {
            for (int row = 0; row < visible_rows; ++row) {
                const size_t offset = (size_t)row * (size_t)per;
                int got = 0;
                while (got < per && valid[offset + (size_t)got]) {
                    ++got;
                }
                if (got > 0) {
                    memcpy(m_mem_cache[row], packed.data() + offset,
                           (size_t)got);
                }
                m_mem_cache_got[row] = got;
            }
        }
        for (int row = visible_rows; row < 64; ++row) {
            m_mem_cache_got[row] = 0;
        }

        m_mem_cache_addr = m_mem_addr;
        m_mem_cache_per = per;
        m_mem_cache_rows = visible_rows;
        m_mem_cache_virtual = m_mem_virtual;
        m_mem_cache_valid = true;
    }

    bool clicked_mem_cell = false;
    bool mem_hovered = false;
    bool mem_action_copy = false;
    bool mem_action_paste = false;
    bool mem_action_delete = false;
    bool mem_action_save = false;
    bool mem_action_disassemble = false;
    bool mem_action_follow = false;

    // Match the user's standalone Xemu Cheat Engine interaction model: the
    // entire dump is one mouse-owning surface.  Tk's <ButtonPress-1> followed
    // by <B1-Motion> keeps delivering drag coordinates until release; an ImGui
    // implementation must likewise keep one ActiveID for the whole gesture.
    // Coordinate hit-testing then decides which byte the pointer represents.
    ImGui::PushStyleColor(ImGuiCol_ChildBg, IM_COL32(21, 21, 21, 255));
    ImGui::BeginChild("##memdump", ImVec2(0, dump_h), false,
                      ImGuiWindowFlags_NoScrollbar |
                      ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleColor();

    const ImVec2 mem_child_min = ImGui::GetWindowPos();
    const ImVec2 mem_child_max(mem_child_min.x + ImGui::GetWindowSize().x,
                               mem_child_min.y + ImGui::GetWindowSize().y);
    const ImVec2 mouse = ImGui::GetIO().MousePos;
    mem_hovered = ImGui::IsMouseHoveringRect(mem_child_min, mem_child_max, false);

    ImGui::PushFont(xemu_feature_detach::FixedWidthFont(g_font_mgr.m_fixed_width_font));

    ImDrawList *dl = ImGui::GetWindowDrawList();
    const ImU32 normal_col = IM_COL32(138, 138, 138, 255); // #8A8A8A
    const ImU32 hex_sel_bg = IM_COL32(255, 152, 0, 255);   // #FF9800
    const ImU32 hex_sel_fg = IM_COL32(0, 0, 0, 255);
    const ImU32 text_sel_bg = IM_COL32(68, 68, 68, 255);   // #444444
    const ImU32 text_sel_fg = IM_COL32(255, 255, 255, 255);
    const float row_width = ImGui::GetContentRegionAvail().x;
    const float text_h = ImGui::GetTextLineHeight();
    const ImVec2 data_origin = ImGui::GetCursorScreenPos();
    const float hex_x0 = data_origin.x + char_w * 13.0f;
    const float text_x0 = data_origin.x + char_w * (float)(16 + 3 * per);
    const float hex_slot_w = char_w * 3.0f;
    const ImVec2 surface_size(std::max(1.0f, row_width),
                              line_h * (float)visible_rows);

    // A previous UTF-8 IME sink may still own ActiveID. Release it before the
    // memory-surface button processes a new press, otherwise the first click
    // can merely defocus the sink instead of beginning a memory gesture.
    if (mem_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        // A UTF-8 edit sink may still own ActiveID from the previous frame.
        // Release it before the memory surface processes this press. The sink
        // is re-focused once, after the gesture ends, if the final caret is on
        // the UTF-8 side. Re-focusing it every frame was also the source of the
        // visible cursor/focus flicker reported in V5.
        ImGui::ClearActiveID();
        m_mem_text_focus_pending = false;
    }

    // One invisible item owns the entire memory surface, exactly like the
    // standalone viewer's Text widget owns ButtonPress/B1-Motion/Button-3.
    // Accept BOTH left and right mouse buttons: V5 accepted only the default
    // left button, so right-click was not guaranteed to become an ImGui-owned
    // interaction and the context popup could be lost to normal Xemu input.
    ImGui::InvisibleButton("##mem_surface", surface_size,
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight);
    const bool surface_pressed =
        ImGui::IsItemClicked(ImGuiMouseButton_Left);
    const bool surface_right_pressed =
        ImGui::IsItemClicked(ImGuiMouseButton_Right);
    const bool surface_active = ImGui::IsItemActive();

    // The Memory viewer is an editor surface, not an ImGui text field. Keep a
    // stable arrow cursor over it (and throughout an active drag) instead of
    // allowing the hidden UTF-8 sink or adjacent widgets to alternate cursor
    // shapes from frame to frame.
    if (mem_hovered || surface_active || m_mem_drag_selecting) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
    }

    auto display_index_to_memory_index = [&](int display_index) {
        const int group = display_index & ~3;
        return (m_mem_big_endian && group + 3 < per)
            ? group + (3 - (display_index & 3))
            : display_index;
    };

    auto mouse_row = [&]() {
        int row = (int)floorf((mouse.y - data_origin.y) /
                              std::max(1.0f, line_h));
        return std::clamp(row, 0, visible_rows - 1);
    };

    auto mouse_display_index = [&](bool text_side) {
        if (text_side) {
            // Full character cells plus half-cell edge tolerance. This is
            // intentionally much more forgiving than clicking the glyph.
            float local = mouse.x - (text_x0 - char_w * 0.5f);
            int i = (int)floorf(local / std::max(1.0f, char_w));
            return std::clamp(i, 0, per - 1);
        }
        // A hex byte owns both digits and its separator. Clicking whitespace
        // between two byte glyphs therefore still lands on a byte slot.
        float local = mouse.x - (hex_x0 - char_w * 0.5f);
        int i = (int)floorf(local / std::max(1.0f, hex_slot_w));
        return std::clamp(i, 0, per - 1);
    };

    auto mouse_in_side = [&](bool text_side, bool require_vertical) {
        if (require_vertical &&
            (mouse.y < data_origin.y ||
             mouse.y >= data_origin.y + line_h * (float)visible_rows)) {
            return false;
        }
        if (text_side) {
            return mouse.x >= text_x0 - char_w * 0.75f &&
                   mouse.x < text_x0 + char_w * ((float)per + 0.75f);
        }
        return mouse.x >= hex_x0 - char_w * 0.75f &&
               mouse.x < hex_x0 + hex_slot_w * (float)per - char_w * 0.25f;
    };

    auto address_from_mouse = [&](bool text_side) {
        const int row = mouse_row();
        const int display_index = mouse_display_index(text_side);
        const int idx = display_index_to_memory_index(display_index);
        return m_mem_addr + (uint32_t)(row * per + idx);
    };

    const bool mouse_hex = mouse_in_side(false, true);
    const bool mouse_text = mouse_in_side(true, true);
    if (surface_pressed && (mouse_hex || mouse_text)) {
        // If the expanded edge hit boxes overlap, prefer the text side only
        // when the pointer is actually at/after the text field origin.
        const bool text_side = mouse_text && mouse.x >= text_x0 - char_w * 0.5f;
        const uint32_t addr = address_from_mouse(text_side);

        // Normal click establishes a new selection anchor. Shift+click keeps
        // the existing anchor and moves only the range end, matching ordinary
        // editor/hex-viewer selection semantics. The drag may then continue
        // from that Shift+click without losing the original anchor.
        const bool extend = ImGui::GetIO().KeyShift && m_mem_have_sel;
        if (!extend) m_mem_sel_anchor = addr;
        m_mem_sel = addr;
        m_mem_have_sel = true;
        m_mem_edit_text = text_side;
        m_mem_keyboard_active = (m_mem_sel == m_mem_sel_anchor);
        m_mem_input_focused = true;
        m_mem_text_focus_pending = false;
        m_mem_drag_selecting = true;
        m_mem_drag_text = text_side;
        m_mem_nibble = 0;
        clicked_mem_cell = true;
    }

    // Right-click operates on the current byte/range. The full memory surface
    // explicitly owns RMB, so this is driven by the item's right-button click
    // rather than a raw global mouse test. Clicking outside the existing range
    // first moves the selection; clicking inside preserves the whole range.
    if (surface_right_pressed && (mouse_hex || mouse_text)) {
        const bool text_side = mouse_text && mouse.x >= text_x0 - char_w * 0.5f;
        const uint32_t addr = address_from_mouse(text_side);
        const uint32_t old_lo = m_mem_have_sel
            ? std::min(m_mem_sel_anchor, m_mem_sel) : 0;
        const uint32_t old_hi = m_mem_have_sel
            ? std::max(m_mem_sel_anchor, m_mem_sel) : 0;
        if (!m_mem_have_sel || addr < old_lo || addr > old_hi) {
            m_mem_sel_anchor = addr;
            m_mem_sel = addr;
            m_mem_have_sel = true;
        }
        m_mem_edit_text = text_side;
        m_mem_keyboard_active = (m_mem_sel == m_mem_sel_anchor);
        m_mem_input_focused = true;
        // The popup owns keyboard focus while open. If the user returns to a
        // single-byte UTF-8 caret, request the invisible text sink once after
        // the popup closes instead of fighting the popup every frame.
        m_mem_text_focus_pending = false;
        m_mem_nibble = 0;
        ImGui::OpenPopup("Memory selection");
    }

    if (ImGui::BeginPopup("Memory selection")) {
        ImGui::TextDisabled("%08X-%08X",
                            std::min(m_mem_sel_anchor, m_mem_sel),
                            std::max(m_mem_sel_anchor, m_mem_sel));
        ImGui::Separator();
        if (ImGui::MenuItem("Copy", "Ctrl+C")) mem_action_copy = true;
        if (ImGui::MenuItem("Paste", "Ctrl+V")) mem_action_paste = true;
        if (ImGui::MenuItem("Delete (fill with 00)")) mem_action_delete = true;
        ImGui::Separator();
        if (ImGui::MenuItem("Add to Saved Addresses")) mem_action_save = true;
        if (ImGui::MenuItem("Go to in Disassembly")) mem_action_disassemble = true;
        if (ImGui::MenuItem("Follow Address (pointer)")) mem_action_follow = true;
        if (ImGui::BeginMenu("View value as")) {
            if (ImGui::MenuItem("1 byte (uint8)", nullptr, m_mem_value_view == 0))
                m_mem_value_view = 0;
            if (ImGui::MenuItem("2 bytes (uint16)", nullptr, m_mem_value_view == 1))
                m_mem_value_view = 1;
            if (ImGui::MenuItem("4 bytes (uint32)", nullptr, m_mem_value_view == 2))
                m_mem_value_view = 2;
            if (ImGui::MenuItem("8 bytes (uint64)", nullptr, m_mem_value_view == 3))
                m_mem_value_view = 3;
            if (ImGui::MenuItem("Float (32-bit)", nullptr, m_mem_value_view == 4))
                m_mem_value_view = 4;
            if (ImGui::MenuItem("Double (64-bit)", nullptr, m_mem_value_view == 5))
                m_mem_value_view = 5;
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }

    // Continuous B1-Motion equivalent.  Once selection starts, the active
    // full-surface item owns the gesture until release.  We use raw mouse
    // coordinates every frame and clamp to the side where the drag began, so
    // fast movement, row crossings, whitespace, and leaving the visible dump
    // cannot terminate selection prematurely.
    if (m_mem_drag_selecting &&
        (surface_active || ImGui::IsMouseDown(ImGuiMouseButton_Left))) {
        const int row = mouse_row();
        const int display_index = mouse_display_index(m_mem_drag_text);
        const int idx = display_index_to_memory_index(display_index);
        m_mem_sel = m_mem_addr + (uint32_t)(row * per + idx);
        m_mem_edit_text = m_mem_drag_text;
        // Like the reference viewer, a real range no longer has a single
        // keyboard caret. A click/pixel drift that remains on the anchor keeps
        // editing active; spanning another byte disables edit-on-type until a
        // new single-byte click is made.
        m_mem_keyboard_active = (m_mem_sel == m_mem_sel_anchor);
        clicked_mem_cell = true;
    }
    if (m_mem_drag_selecting && !ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        m_mem_drag_selecting = false;
        // SDL/ImGui text input only needs to be focused once after a UTF-8
        // click/drag settles on a single byte. Keeping SetKeyboardFocusHere()
        // running every frame caused focus churn and cursor flicker.
        m_mem_text_focus_pending =
            m_mem_edit_text && m_mem_keyboard_active && m_mem_have_sel;
    }

    const uint32_t sel_lo = m_mem_have_sel
        ? std::min(m_mem_sel_anchor, m_mem_sel) : 0;
    const uint32_t sel_hi = m_mem_have_sel
        ? std::max(m_mem_sel_anchor, m_mem_sel) : 0;

    // The InvisibleButton already reserves the whole drawing area. Build each
    // row into retained-size stack strings and submit a handful of text draws
    // instead of one AddText call per hex byte and per UTF-8 cell. Selected
    // glyphs use a parallel string so their colors remain byte-identical to
    // the old per-cell renderer; selection backgrounds stay per cell so the
    // intentional one-character gap between adjacent hex highlights remains.
    constexpr char kHexDigits[] = "0123456789ABCDEF";
    for (int row = 0; row < visible_rows; ++row) {
        const uint32_t row_addr = m_mem_addr + (uint32_t)(row * per);
        const uint8_t *buf = m_mem_cache[row];
        const int got = m_mem_cache_got[row];
        const ImVec2 row_pos(data_origin.x,
                             data_origin.y + line_h * (float)row);

        char prefix[32];
        snprintf(prefix, sizeof(prefix), "0x%08X | ", row_addr);
        dl->AddText(row_pos, normal_col, prefix);

        const float hex_x = row_pos.x + char_w * 13.0f;
        const float text_x = row_pos.x + char_w * (float)(16 + 3 * per);
        const float cell_y = row_pos.y;
        char hex_normal[128 * 3 + 1];
        char hex_selected[128 * 3 + 1];
        char text_normal[128 + 1];
        char text_selected[128 + 1];
        memset(hex_normal, ' ', (size_t)per * 3);
        memset(hex_selected, ' ', (size_t)per * 3);
        memset(text_normal, ' ', (size_t)per);
        memset(text_selected, ' ', (size_t)per);
        hex_normal[per * 3] = '\0';
        hex_selected[per * 3] = '\0';
        text_normal[per] = '\0';
        text_selected[per] = '\0';
        bool have_selected_glyphs = false;

        for (int i = 0; i < per; ++i) {
            const int idx = display_index_to_memory_index(i);
            const bool mapped = idx < got;
            const uint8_t b = mapped ? buf[idx] : 0;
            const uint32_t byte_addr = row_addr + (uint32_t)idx;
            const bool selected = m_mem_have_sel &&
                                  byte_addr >= sel_lo && byte_addr <= sel_hi;
            char *hex_dst = selected ? hex_selected : hex_normal;
            char *text_dst = selected ? text_selected : text_normal;
            const int hex_off = i * 3;
            if (mapped) {
                hex_dst[hex_off] = kHexDigits[b >> 4];
                hex_dst[hex_off + 1] = kHexDigits[b & 0x0F];
            } else {
                hex_dst[hex_off] = '?';
                hex_dst[hex_off + 1] = '?';
            }
            text_dst[i] =
                (char)((mapped && b >= 0x20 && b <= 0x7E) ? b : '.');

            if (selected) {
                have_selected_glyphs = true;
                const ImVec2 hp(hex_x + char_w * (float)(i * 3), cell_y);
                dl->AddRectFilled(
                    hp, ImVec2(hp.x + char_w * 2.0f, hp.y + text_h),
                    hex_sel_bg);
                const ImVec2 tp(text_x + char_w * (float)i, cell_y);
                dl->AddRectFilled(
                    tp, ImVec2(tp.x + char_w, tp.y + text_h), text_sel_bg);
            }
        }

        dl->AddText(ImVec2(hex_x, cell_y), normal_col, hex_normal);
        dl->AddText(ImVec2(text_x, cell_y), normal_col, text_normal);
        if (have_selected_glyphs) {
            dl->AddText(ImVec2(hex_x, cell_y), hex_sel_fg, hex_selected);
            dl->AddText(ImVec2(text_x, cell_y), text_sel_fg, text_selected);
        }
    }

    ImGui::PopFont();

    int scroll_rows = 0;
    if (mem_hovered) {
        // Match the reference viewer: one normal wheel notch moves one memory
        // row. Fractional/high-resolution wheel deltas are retained until they
        // add up to a full row rather than being discarded.
        m_mem_wheel_accum += ImGui::GetIO().MouseWheel;
        const int wheel_rows = (int)std::trunc(m_mem_wheel_accum);
        if (wheel_rows != 0) {
            scroll_rows -= wheel_rows;
            m_mem_wheel_accum -= (float)wheel_rows;
        }
        if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) scroll_rows -= visible_rows;
        if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) scroll_rows += visible_rows;
    } else {
        m_mem_wheel_accum = 0.0f;
    }

    // Continue edge auto-scroll even after the pointer leaves the child. The
    // full-surface ActiveID intentionally owns the drag until LMB release.
    if (m_mem_drag_selecting && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        const float edge = std::max(3.0f, line_h * 0.65f);
        if (mouse.y <= mem_child_min.y + edge) scroll_rows -= 1;
        if (mouse.y >= mem_child_max.y - edge) scroll_rows += 1;
    }

    ImGui::EndChild();

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) &&
        !mem_hovered && !clicked_mem_cell) {
        m_mem_keyboard_active = false;
        m_mem_input_focused = false;
        m_mem_text_focus_pending = false;
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

    auto selected_span = [&]() {
        const uint32_t lo = std::min(m_mem_sel_anchor, m_mem_sel);
        const uint32_t hi = std::max(m_mem_sel_anchor, m_mem_sel);
        return std::pair<uint32_t, uint64_t>(
            lo, (uint64_t)hi - (uint64_t)lo + 1);
    };

    auto read_exact = [&](uint32_t addr, void *dst, size_t len) {
        if ((uint64_t)addr + (uint64_t)len > 0x100000000ULL) return false;
        uint8_t *out = static_cast<uint8_t *>(dst);
        size_t done = 0;
        while (done < len) {
            const size_t remain = len - done;
            const ssize_t got = xemu_dbg_read_space(
                addr + (uint32_t)done, out + done, remain, m_mem_virtual);
            if (got <= 0) return false;
            done += (size_t)got;
        }
        return true;
    };

    auto write_exact = [&](uint32_t addr, const void *src, size_t len) {
        if ((uint64_t)addr + (uint64_t)len > 0x100000000ULL) return false;
        const uint8_t *in = static_cast<const uint8_t *>(src);
        size_t done = 0;
        while (done < len) {
            const size_t remain = len - done;
            const ssize_t put = xemu_dbg_write_space(
                addr + (uint32_t)done, in + done, remain, m_mem_virtual);
            if (put <= 0) return false;
            done += (size_t)put;
        }
        return true;
    };

    // Clipboard shortcuts use the same operations as the context menu. Keep
    // them scoped to a memory selection that still owns input focus so Ctrl+V
    // in the address box or another debugger field can never write guest RAM.
    if (m_mem_have_sel && m_mem_input_focused && ImGui::GetIO().KeyCtrl) {
        if (ImGui::IsKeyPressed(ImGuiKey_C, false)) mem_action_copy = true;
        if (ImGui::IsKeyPressed(ImGuiKey_V, false)) mem_action_paste = true;
    }

    if (m_mem_have_sel && mem_action_copy) {
        const auto [start, count64] = selected_span();
        constexpr uint64_t kMaxClipboardBytes = 16ULL * 1024ULL * 1024ULL;
        if (count64 > kMaxClipboardBytes) {
            m_status = "memory copy selection is larger than 16 MiB";
            m_status_ms = SDL_GetTicks();
        } else {
            const size_t count = (size_t)count64;
            std::vector<uint8_t> bytes(count);
            if (!read_exact(start, bytes.data(), count)) {
                m_status = "memory copy failed: selection contains unreadable memory";
                m_status_ms = SDL_GetTicks();
            } else {
                static const char hex[] = "0123456789ABCDEF";
                std::string text;
                text.reserve(count ? count * 3 - 1 : 0);
                for (size_t i = 0; i < count; ++i) {
                    if (i) text.push_back(' ');
                    text.push_back(hex[bytes[i] >> 4]);
                    text.push_back(hex[bytes[i] & 0x0F]);
                }
                SetClipboard(text);
                m_status = "Copied " + std::to_string(count) +
                           " memory byte" + (count == 1 ? "" : "s");
                m_status_ms = SDL_GetTicks();
            }
        }
    }

    if (m_mem_have_sel && mem_action_paste) {
        const char *clip = ImGui::GetClipboardText();
        std::string text = clip ? clip : "";
        std::vector<int> parsed;
        bool invalid = false;

        auto append_hex_token = [&](std::string token) {
            if (token.empty()) return;
            if (token == "??" || token == "--" || token == ".." ||
                token == "__") {
                parsed.push_back(-1);
                return;
            }
            if (token.size() >= 2 && token[0] == '0' &&
                (token[1] == 'x' || token[1] == 'X')) {
                token.erase(0, 2);
            }
            if (token.empty() || (token.size() & 1)) {
                invalid = true;
                return;
            }
            for (char c : token) {
                if (!std::isxdigit((unsigned char)c)) {
                    invalid = true;
                    return;
                }
            }
            for (size_t i = 0; i < token.size(); i += 2) {
                char pair[3] = { token[i], token[i + 1], 0 };
                parsed.push_back((int)strtoul(pair, nullptr, 16));
            }
        };

        std::string token;
        for (size_t i = 0; i <= text.size(); ++i) {
            const char c = i < text.size() ? text[i] : ' ';
            if (std::isspace((unsigned char)c) || c == ',' || c == ';') {
                append_hex_token(token);
                token.clear();
            } else {
                token.push_back(c);
            }
        }

        if (invalid || parsed.empty()) {
            m_status = invalid
                ? "Paste failed: clipboard is not a hexadecimal byte sequence"
                : "Paste failed: clipboard contains no hexadecimal bytes";
            m_status_ms = SDL_GetTicks();
        } else {
            const uint32_t start = selected_span().first;
            const uint64_t end64 = (uint64_t)start + (uint64_t)parsed.size();
            if (end64 > 0x100000000ULL) {
                m_status = "Paste failed: byte sequence runs past address space";
                m_status_ms = SDL_GetTicks();
            } else {
                bool ok = true;
                size_t written = 0;
                for (size_t i = 0; i < parsed.size();) {
                    if (parsed[i] < 0) { ++i; continue; }
                    const size_t run_start = i;
                    std::vector<uint8_t> run;
                    while (i < parsed.size() && parsed[i] >= 0) {
                        run.push_back((uint8_t)parsed[i]);
                        ++i;
                    }
                    if (!write_exact(start + (uint32_t)run_start,
                                     run.data(), run.size())) {
                        ok = false;
                        break;
                    }
                    written += run.size();
                }
                if (!ok) {
                    m_status = "Paste failed: destination contains unwritable memory";
                    m_status_ms = SDL_GetTicks();
                } else {
                    m_mem_cache_valid = false;
                    const uint32_t next = parsed.size() == 0
                        ? start
                        : clamp_addr((int64_t)start + (int64_t)parsed.size());
                    m_mem_sel_anchor = next;
                    m_mem_sel = next;
                    m_mem_keyboard_active = true;
                    m_mem_nibble = 0;
                    ensure_cursor_visible();
                    m_status = "Pasted " + std::to_string(written) +
                               " memory byte" + (written == 1 ? "" : "s");
                    m_status_ms = SDL_GetTicks();
                }
            }
        }
    }

    if (m_mem_have_sel && mem_action_delete) {
        const auto [start, count] = selected_span();
        static const uint8_t zeroes[4096] = {};
        uint64_t done = 0;
        bool ok = true;
        while (done < count) {
            const size_t chunk = (size_t)std::min<uint64_t>(
                sizeof(zeroes), count - done);
            if (!write_exact(start + (uint32_t)done, zeroes, chunk)) {
                ok = false;
                break;
            }
            done += chunk;
        }
        m_mem_cache_valid = false;
        if (ok) {
            m_status = "Cleared " + std::to_string(count) +
                       " selected byte" + (count == 1 ? "" : "s") +
                       " to 00";
        } else {
            m_status = "Delete stopped at unwritable memory after " +
                       std::to_string(done) + " byte(s)";
        }
        m_status_ms = SDL_GetTicks();
    }

    if (m_mem_have_sel && mem_action_save) {
        const auto [start, count] = selected_span();
        OpenSavedAddressEditor(0, 0, false);
        snprintf(m_saved_editor_addr, sizeof(m_saved_editor_addr), "%08X", start);
        m_saved_editor_virtual = m_mem_virtual;
        if (count == 1) {
            m_saved_editor_type = 0; m_saved_editor_size = 1;
        } else if (count == 2) {
            m_saved_editor_type = 1; m_saved_editor_size = 2;
        } else if (count == 4) {
            m_saved_editor_type = (m_mem_value_view == 4) ? 3 : 2;
            m_saved_editor_size = 4;
        } else if (count == 8 && m_mem_value_view == 5) {
            m_saved_editor_type = 4; m_saved_editor_size = 8;
        } else {
            m_saved_editor_type = 6;
            m_saved_editor_size = (int)std::min<uint64_t>(count, 256);
        }
    }

    if (m_mem_have_sel && mem_action_disassemble) {
        const uint32_t addr = selected_span().first;
        uint32_t va = addr;
        if (m_mem_virtual || xemu_dbg_to_virt(addr, &va)) {
            GoTo(va, true);
        } else {
            m_status = "physical address currently has no executable virtual mapping";
            m_status_ms = SDL_GetTicks();
        }
    }

    if (m_mem_have_sel && mem_action_follow) {
        const uint32_t source = selected_span().first;
        uint8_t raw[4] = {};
        if (!read_exact(source, raw, sizeof(raw))) {
            m_status = "Follow Address failed: pointer bytes are unreadable";
            m_status_ms = SDL_GetTicks();
        } else {
            const uint32_t pointer = ReadLe32(raw);
            uint32_t destination = pointer;
            bool mapped = false;
            if (m_mem_virtual) {
                uint32_t phys = 0;
                mapped = xemu_dbg_to_phys(pointer, &phys);
            } else if (pointer < 0x08000000u) {
                destination = pointer;
                mapped = true;
            } else {
                mapped = xemu_dbg_to_phys(pointer, &destination);
            }

            if (!mapped) {
                char msg[96];
                snprintf(msg, sizeof(msg),
                         "Follow Address failed: %08X is not mapped", pointer);
                m_status = msg;
                m_status_ms = SDL_GetTicks();
            } else {
                m_mem_addr = (destination / (uint32_t)per) * (uint32_t)per;
                m_mem_region = 0;
                m_mem_sel_anchor = destination;
                m_mem_sel = destination;
                m_mem_have_sel = true;
                m_mem_keyboard_active = true;
                m_mem_input_focused = true;
                m_mem_nibble = 0;
                m_mem_cache_valid = false;
                snprintf(m_mem_buf, sizeof(m_mem_buf), "%08X", m_mem_addr);
                char msg[96];
                snprintf(msg, sizeof(msg), "Followed pointer %08X -> %08X",
                         source, destination);
                m_status = msg;
                m_status_ms = SDL_GetTicks();
            }
        }
    }

    if (scroll_rows != 0) {
        m_mem_addr =
            clamp_addr((int64_t)m_mem_addr + (int64_t)scroll_rows * per);
        snprintf(m_mem_buf, sizeof(m_mem_buf), "%08X", m_mem_addr);
        m_mem_region = 0;
        m_mem_cache_valid = false;
    }

    // Keep a tiny transparent InputText active solely to make SDL3/ImGui
    // request proper text/IME events, but consume each character in a
    // CharFilter callback and reject it from the widget's internal buffer.
    // This avoids the old hidden-buffer state leaking a single keypress into
    // later frames (the "press 0 once, fill many bytes with 0" bug).
    std::vector<ImWchar> input_chars;
    if (m_mem_keyboard_active && m_mem_have_sel && m_mem_edit_text &&
        !m_mem_drag_selecting) {
        // UTF-8 text entry still needs SDL/ImGui text-input activation for IME
        // correctness. Hex editing deliberately never creates this hidden
        // widget, so its active ID cannot interfere with ordinary byte clicks.
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
        if (m_mem_text_focus_pending) {
            ImGui::SetKeyboardFocusHere();
            m_mem_text_focus_pending = false;
        }
        m_mem_input_sink[0] = '\0';
        ImGui::InputText("##mem_keyboard_sink",
                         m_mem_input_sink, sizeof(m_mem_input_sink),
                         ImGuiInputTextFlags_NoHorizontalScroll |
                         ImGuiInputTextFlags_CallbackCharFilter |
                         ImGuiInputTextFlags_CallbackAlways,
                         MemoryViewerInputCallback, &input_chars);
        ImGui::PopStyleColor(4);
        ImGui::PopStyleVar();
        ImGui::SetCursorPos(saved);
    }

    // DrawMemory owns the cursor while the pointer is over its byte surface.
    // Reassert this after the hidden UTF-8 sink is submitted so that sink
    // focus can never make the host cursor oscillate between text/arrow.
    if (mem_hovered || m_mem_drag_selecting) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
    }

    // Hex-side input uses direct key-down transitions (one key -> one nibble).
    // Text-side input consumes each UTF-8 code point once through the sink.
    if (m_mem_keyboard_active && m_mem_have_sel) {
        int64_t move = 0;
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow)) move = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow)) move = 1;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) move = -per;
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow)) move = per;

        if (move != 0) {
            m_mem_sel = clamp_addr((int64_t)m_mem_sel + move);
            m_mem_sel_anchor = m_mem_sel;
            m_mem_nibble = 0;
            ensure_cursor_visible();
        }

        if (!m_mem_edit_text) {
            const int nib = MemoryViewerPressedHexNibble();
            if (nib >= 0) {
                uint8_t cur = 0;
                if (xemu_dbg_read_space(m_mem_sel, &cur, 1,
                                        m_mem_virtual) != 1) {
                    m_status = "selected memory byte is not mapped/readable";
                    m_status_ms = SDL_GetTicks();
                } else {
                    const uint8_t value = m_mem_nibble == 0
                        ? (uint8_t)((nib << 4) | (cur & 0x0F))
                        : (uint8_t)((cur & 0xF0) | nib);

                    if (xemu_dbg_write_space(m_mem_sel, &value, 1,
                                             m_mem_virtual) != 1) {
                        m_status = "memory write failed";
                        m_status_ms = SDL_GetTicks();
                    } else {
                        m_mem_cache_valid = false;
                        if (m_mem_nibble == 0) {
                            m_mem_nibble = 1;
                        } else {
                            m_mem_nibble = 0;
                            m_mem_sel = clamp_addr((int64_t)m_mem_sel + 1);
                            // Editing has a caret, not a range. Advancing to
                            // the next byte collapses the selection so the
                            // highlight follows the caret instead of leaving
                            // the just-edited byte selected as a two-byte span.
                            m_mem_sel_anchor = m_mem_sel;
                            ensure_cursor_visible();
                        }
                    }
                }
            }
        } else if (!input_chars.empty()) {
            std::string utf8;
            for (ImWchar c : input_chars) AppendUtf8Codepoint(c, &utf8);
            if (!utf8.empty()) {
                const size_t n = utf8.size();
                if (xemu_dbg_write_space(m_mem_sel, utf8.data(), n,
                                         m_mem_virtual) != (ssize_t)n) {
                    m_status = "UTF-8 memory write failed";
                    m_status_ms = SDL_GetTicks();
                } else {
                    m_mem_cache_valid = false;
                    m_mem_sel = clamp_addr((int64_t)m_mem_sel +
                                           (int64_t)n);
                    // UTF-8 typing advances a single insertion caret too.
                    // Keep the anchor synchronized so typed characters never
                    // turn into an accidental multi-byte selection.
                    m_mem_sel_anchor = m_mem_sel;
                    ensure_cursor_visible();
                }
            }
        }
    }

    if (m_mem_have_sel) {
        if (sel_lo != sel_hi) {
            const uint64_t selected_bytes =
                (uint64_t)sel_hi - (uint64_t)sel_lo + 1;
            ImGui::Text("Selection %08X-%08X (%" PRIu64 " bytes)",
                        sel_lo, sel_hi, selected_bytes);
        }

        // Live typed interpretation selected from the Memory context menu.
        // This never changes the underlying byte-oriented editor/selection; it
        // is simply another way to inspect the value beginning at the range
        // start, so drag/Shift selections remain exact byte selections.
        static const char *kValueViewNames[] = {
            "1 byte", "2 bytes", "4 bytes", "8 bytes",
            "Float (32-bit)", "Double (64-bit)"
        };
        const size_t value_bytes =
            (m_mem_value_view == 0) ? 1 :
            (m_mem_value_view == 1) ? 2 :
            (m_mem_value_view == 2 || m_mem_value_view == 4) ? 4 : 8;
        uint8_t value_raw[8] = {};
        if (read_exact(sel_lo, value_raw, value_bytes)) {
            uint64_t bits = 0;
            if (m_mem_big_endian) {
                for (size_t i = 0; i < value_bytes; ++i)
                    bits = (bits << 8) | value_raw[i];
            } else {
                for (size_t i = 0; i < value_bytes; ++i)
                    bits |= (uint64_t)value_raw[i] << (i * 8);
            }

            if (m_mem_value_view == 4) {
                const uint32_t u = (uint32_t)bits;
                float f = 0.0f;
                memcpy(&f, &u, sizeof(f));
                ImGui::Text("View as %s: %.9g",
                            kValueViewNames[m_mem_value_view], (double)f);
            } else if (m_mem_value_view == 5) {
                double d = 0.0;
                memcpy(&d, &bits, sizeof(d));
                ImGui::Text("View as %s: %.17g",
                            kValueViewNames[m_mem_value_view], d);
            } else if (value_bytes == 8) {
                ImGui::Text("View as %s: 0x%016" PRIX64 " (%" PRIu64 ")",
                            kValueViewNames[m_mem_value_view], bits, bits);
            } else {
                ImGui::Text("View as %s: 0x%0*" PRIX64 " (%" PRIu64 ")",
                            kValueViewNames[m_mem_value_view],
                            (int)(value_bytes * 2), bits, bits);
            }
        } else {
            ImGui::TextDisabled("View as %s: unreadable",
                                kValueViewNames[m_mem_value_view]);
        }

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
        if (ImGui::SmallButton("Save address##memsel")) {
            OpenSavedAddressEditor(0, 0, false);
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

    // Saved-address freezes are debugger functionality, not merely visible-tab
    // functionality. Keep them alive while the debugger window is closed.
    TickSavedAddresses();

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

    TickMemorySearch();

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
    const float total_w = ImGui::GetContentRegionAvail().x;
    const float side_splitter_w = 7.0f * g_viewport_mgr.m_scale;
    const float min_left = 260.0f * g_viewport_mgr.m_scale;
    const float min_code = 360.0f * g_viewport_mgr.m_scale;
    const float usable_w = std::max(1.0f, total_w - side_splitter_w);
    float left_w = usable_w * m_left_pane_ratio;
    left_w = std::max(min_left, std::min(left_w, usable_w - min_code));

    ImGui::BeginChild("##debug_sidepane", ImVec2(left_w, 0), true);
    DrawUpperLeftPane();
    ImGui::EndChild();

    ImGui::SameLine(0.0f, 0.0f);
    ImGui::InvisibleButton("##debug_side_splitter",
                           ImVec2(side_splitter_w,
                                  ImGui::GetContentRegionAvail().y));
    const bool side_hovered = ImGui::IsItemHovered();
    const bool side_active = ImGui::IsItemActive();
    if (side_hovered || side_active) {
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
    }
    if (side_active && ImGui::GetIO().MouseDelta.x != 0.0f) {
        const float next_left = std::max(
            min_left, std::min(left_w + ImGui::GetIO().MouseDelta.x,
                               usable_w - min_code));
        m_left_pane_ratio = next_left / usable_w;
    }
    if (side_hovered &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        m_left_pane_ratio = 0.30f;
    }
    {
        ImDrawList *dl = ImGui::GetWindowDrawList();
        const ImVec2 a = ImGui::GetItemRectMin();
        const ImVec2 b = ImGui::GetItemRectMax();
        const float x = (a.x + b.x) * 0.5f;
        const ImU32 col = (side_hovered || side_active)
            ? IM_COL32(76, 190, 76, 255)
            : ImGui::GetColorU32(ImGuiCol_Separator);
        dl->AddLine(ImVec2(x, a.y), ImVec2(x, b.y), col,
                    side_active ? 3.0f : 1.0f);
    }

    ImGui::SameLine(0.0f, 0.0f);
    ImGui::BeginChild("##codepane", ImVec2(0, 0), true);
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
