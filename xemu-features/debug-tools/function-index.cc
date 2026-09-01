// SPDX-License-Identifier: GPL-2.0-or-later
//
// Native function/symbol discovery for the custom debugger.  The scanning
// heuristics mirror the user's MIT-licensed Xemu Cheat Engine FunctionIndex:
// symbol > RTTI > entry > string > call target > prologue.

#include "function-index.hh"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <limits>
#include <unordered_set>

#include "xemu-features/debug-tools/debug-api.h"
#include "xemu-xbe.h"

namespace {

constexpr uint32_t kMaxTextScan = 0x01000000; // 16 MiB, same external-tool cap
constexpr uint32_t kMaxDataScan = 0x00400000; // 4 MiB per data section

#pragma pack(push, 1)
struct XbeSectionHeaderLocal {
    uint32_t flags;
    uint32_t virtual_addr;
    uint32_t virtual_size;
    uint32_t raw_addr;
    uint32_t raw_size;
    uint32_t section_name_addr;
    uint32_t section_name_ref_count;
    uint32_t head_shared_ref_count_addr;
    uint32_t tail_shared_ref_count_addr;
    uint8_t digest[20];
};
#pragma pack(pop)
static_assert(sizeof(XbeSectionHeaderLocal) == 0x38,
              "XBE section header must be 0x38 bytes");

static uint32_t Rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static bool ReadVirtual(uint32_t va, void *out, size_t len)
{
    return xemu_dbg_read_space(va, out, len, true) == (ssize_t)len;
}

static bool ReadCString(uint32_t va, size_t cap, std::string *out)
{
    out->clear();
    if (!va || !cap) return false;

    /* Section/RTTI names are deliberately capped to short strings. Keep the
     * normal scan path on the stack instead of heap-allocating a vector for
     * every candidate type descriptor. */
    char stack_buf[256] = {};
    std::vector<char> heap_buf;
    char *buf = stack_buf;
    if (cap > sizeof(stack_buf)) {
        heap_buf.assign(cap + 1, 0);
        buf = heap_buf.data();
    }

    ssize_t got = xemu_dbg_read_space(va, buf, cap, true);
    if (got <= 0) return false;
    size_t n = 0;
    while (n < (size_t)got && buf[n]) ++n;
    out->assign(buf, n);
    return !out->empty();
}

static bool IsStartByte(uint8_t b)
{
    switch (b) {
    case 0x55: case 0x53: case 0x56: case 0x57: case 0x51: case 0x52:
    case 0x50: case 0x83: case 0x81: case 0x8B: case 0x89: case 0x8D:
    case 0xA1: case 0xA0: case 0xA3: case 0x6A: case 0x68: case 0xB8:
    case 0xB9: case 0xBA: case 0xBB: case 0xBE: case 0xBF: case 0x33:
    case 0x31: case 0x85: case 0x84: case 0xE9: case 0xEB: case 0xFF:
    case 0xC3: case 0xC2: case 0xD9: case 0xF3: case 0x0F: case 0x64:
    case 0x65:
        return true;
    default:
        return false;
    }
}

static bool EndsWithI(const std::string &s, const char *suffix)
{
    size_t n = strlen(suffix);
    if (s.size() < n) return false;
    size_t off = s.size() - n;
    for (size_t i = 0; i < n; ++i) {
        if (std::tolower((unsigned char)s[off + i]) !=
            std::tolower((unsigned char)suffix[i])) return false;
    }
    return true;
}

static bool LooksFileish(const std::string &s)
{
    if (s.size() < 3 || s.size() > 80) return false;
    static const char *exts[] = { ".c", ".cc", ".cpp", ".cxx", ".h",
                                  ".hpp", ".asm" };
    for (auto *e : exts) if (EndsWithI(s, e)) return true;
    return false;
}

static bool LooksFuncish(const std::string &s)
{
    if (s.size() < 4 || s.size() > 90) return false;
    if (!(std::isalpha((unsigned char)s[0]) || s[0] == '_')) return false;
    int colons = 0;
    for (char c : s) {
        if (c == ':') { colons++; continue; }
        if (!(std::isalnum((unsigned char)c) || c == '_' || c == '~')) return false;
    }
    return colons == 0 || colons == 2;
}

static std::string Basename(const std::string &s)
{
    size_t p = s.find_last_of("/\\");
    return p == std::string::npos ? s : s.substr(p + 1);
}

static std::string DemangleMsvcLight(std::string name)
{
    if (name.empty()) return name;
    if (name[0] != '?') {
        while (!name.empty() && (name[0] == '_' || name[0] == '@'))
            name.erase(name.begin());
        return name;
    }
    size_t end = name.find("@@");
    if (end == std::string::npos || end <= 1) return name;
    std::string body = name.substr(1, end - 1);
    std::vector<std::string> parts;
    size_t pos = 0;
    while (pos <= body.size()) {
        size_t at = body.find('@', pos);
        std::string part = body.substr(pos, at == std::string::npos
                                               ? std::string::npos : at - pos);
        if (!part.empty()) parts.push_back(part);
        if (at == std::string::npos) break;
        pos = at + 1;
    }
    if (parts.empty()) return name;
    std::reverse(parts.begin(), parts.end());
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i) out += "::";
        out += parts[i];
    }
    return out;
}

static bool ParseHexToken(const char *s, uint32_t *out)
{
    if (!s || !*s) return false;
    char *end = nullptr;
    unsigned long v = strtoul(s, &end, 16);
    if (end == s) return false;
    *out = (uint32_t)v;
    return true;
}

} // namespace

const char *XemuDbgFunctionIndex::SourceLabel(XemuDbgFunctionSource source)
{
    switch (source) {
    case XemuDbgFunctionSource::Symbol:   return "sym";
    case XemuDbgFunctionSource::Rtti:     return "rtti";
    case XemuDbgFunctionSource::Entry:    return "entry";
    case XemuDbgFunctionSource::String:   return "str";
    case XemuDbgFunctionSource::Call:     return "call";
    case XemuDbgFunctionSource::Prologue: return "prologue";
    }
    return "?";
}

void XemuDbgFunctionIndex::Clear()
{
    m_by_addr.clear();
    m_callers.clear();
    m_entries.clear();
    m_sections.clear();
    m_image_lo = m_image_hi = m_text_lo = m_text_hi = 0;
}

bool XemuDbgFunctionIndex::Add(uint32_t addr, const std::string &name,
                               XemuDbgFunctionSource source,
                               const std::string &note)
{
    auto it = m_by_addr.find(addr);
    if (it != m_by_addr.end()) {
        if ((unsigned)it->second.source >= (unsigned)source) {
            if (it->second.note.empty() && !note.empty()) it->second.note = note;
            return false;
        }
        uint32_t xrefs = it->second.xrefs;
        it->second = { addr, name, source, xrefs, note };
        return true;
    }
    m_by_addr.emplace(addr, XemuDbgFunctionEntry{ addr, name, source, 0, note });
    return true;
}

void XemuDbgFunctionIndex::Finalize()
{
    m_entries.clear();
    m_entries.reserve(m_by_addr.size());
    for (auto &kv : m_by_addr) m_entries.push_back(kv.second);
    std::sort(m_entries.begin(), m_entries.end(),
              [](const auto &a, const auto &b) { return a.addr < b.addr; });
}

bool XemuDbgFunctionIndex::BuildSections(std::string *status)
{
    struct xbe *xbe = xemu_get_xbe_info();
    if (!xbe || !xbe->header || !xbe->headers) {
        if (status) *status = "No loaded XBE header found.";
        return false;
    }

    const xbe_header *h = xbe->header;
    m_image_lo = h->m_base;
    m_image_hi = h->m_base + h->m_sizeof_image;

    uint32_t nsec = std::min(h->m_sections, 64u);
    m_sections.reserve((size_t)nsec + 1);
    uint32_t secva = h->m_section_headers_addr;
    for (uint32_t i = 0; i < nsec; ++i) {
        XbeSectionHeaderLocal sh{};
        if (!ReadVirtual(secva + i * sizeof(sh), &sh, sizeof(sh))) continue;
        if (!sh.virtual_size) continue;
        std::string name;
        if (!ReadCString(sh.section_name_addr, 31, &name)) {
            char tmp[24];
            snprintf(tmp, sizeof(tmp), "section_%u", i);
            name = tmp;
        }
        uint32_t hi = sh.virtual_addr + sh.virtual_size;
        if (hi < sh.virtual_addr) continue;
        m_sections.push_back({ name, sh.virtual_addr, hi });
        if (name == ".text") {
            m_text_lo = sh.virtual_addr;
            m_text_hi = hi;
        }
    }

    if (!m_text_lo || m_text_hi <= m_text_lo) {
        // Some unusual titles do not call the executable section .text.
        m_text_lo = m_image_lo;
        m_text_hi = m_image_hi;
        m_sections.push_back({ ".text?", m_text_lo, m_text_hi });
    }
    return true;
}

void XemuDbgFunctionIndex::ScanRtti()
{
    const XemuDbgSectionRange *rdata = nullptr;
    for (const auto &s : m_sections) if (s.name == ".rdata") { rdata = &s; break; }
    if (!rdata || rdata->hi <= rdata->lo) return;

    size_t want = std::min<size_t>(rdata->hi - rdata->lo, kMaxDataScan);
    std::vector<uint8_t> buf(want);
    ssize_t got = xemu_dbg_read_space(rdata->lo, buf.data(), buf.size(), true);
    if (got <= 8) return;
    buf.resize((size_t)got);

    for (size_t i = 0; i + 8 <= buf.size(); i += 4) {
        uint32_t locator = Rd32(&buf[i]);
        uint32_t first = Rd32(&buf[i + 4]);
        if (locator < rdata->lo || locator >= rdata->hi || !InText(first)) continue;

        uint32_t type_desc = 0;
        if (!ReadVirtual(locator + 0x0C, &type_desc, 4) || !type_desc) continue;
        std::string raw;
        if (!ReadCString(type_desc + 8, 96, &raw)) continue;
        if (raw.size() < 6 || raw.rfind(".?AV", 0) != 0 ||
            raw.compare(raw.size() - 2, 2, "@@") != 0) continue;
        std::string cls = raw.substr(4, raw.size() - 6);
        if (cls.empty()) continue;

        size_t slot = 0;
        for (size_t p = i + 4; p + 4 <= buf.size() && slot < 400; p += 4, ++slot) {
            uint32_t fn = Rd32(&buf[p]);
            if (!InText(fn)) break;
            char nm[256];
            snprintf(nm, sizeof(nm), "%s::vtable[%zu]", cls.c_str(), slot);
            Add(fn, nm, XemuDbgFunctionSource::Rtti);
        }
    }
}

bool XemuDbgFunctionIndex::Scan(std::string *status)
{
    Clear();
    if (!BuildSections(status)) return false;

    uint64_t span64 = (uint64_t)m_text_hi - m_text_lo;
    size_t span = (size_t)std::min<uint64_t>(span64, kMaxTextScan);
    if (!span) {
        if (status) *status = "The XBE executable section is empty.";
        return false;
    }

    std::vector<uint8_t> code(span);
    ssize_t got = xemu_dbg_read_space(m_text_lo, code.data(), code.size(), true);
    if (got <= 0) {
        if (status) *status = "Could not read the XBE executable section.";
        return false;
    }
    code.resize((size_t)got);
    m_text_hi = m_text_lo + (uint32_t)code.size();

    /* Reserve from executable size using a conservative function density.
     * This avoids repeated unordered-container rehashes on large retail XBEs
     * while keeping the estimate bounded for pathological sections. */
    const size_t candidate_hint = std::min<size_t>(code.size() / 64 + 64, 65536);
    m_by_addr.reserve(candidate_hint);
    m_callers.reserve(candidate_hint / 2 + 16);

    std::unordered_set<uint32_t> prologues;
    std::unordered_set<uint32_t> padded;
    prologues.reserve(candidate_hint);
    padded.reserve(candidate_hint);
    for (size_t i = 0; i + 5 <= code.size(); ++i) {
        if (code[i] == 0x55 && code[i + 1] == 0x8B && code[i + 2] == 0xEC)
            prologues.insert(m_text_lo + (uint32_t)i);
        if (code[i] == 0x8B && code[i + 1] == 0xFF && code[i + 2] == 0x55 &&
            code[i + 3] == 0x8B && code[i + 4] == 0xEC)
            prologues.insert(m_text_lo + (uint32_t)i);
    }
    for (size_t i = 3; i < code.size(); ++i) {
        uint8_t pad = code[i - 1];
        if ((pad != 0xCC && pad != 0x90) || code[i] == pad) continue;
        size_t run = 1;
        while (run < i && code[i - 1 - run] == pad) run++;
        if (run >= 3 && IsStartByte(code[i]))
            padded.insert(m_text_lo + (uint32_t)i);
    }

    std::unordered_map<uint32_t, uint32_t> calls;
    std::unordered_map<uint32_t, uint32_t> jmps;
    calls.reserve(candidate_hint);
    jmps.reserve(candidate_hint / 4 + 16);
    for (size_t i = 0; i + 5 <= code.size(); ++i) {
        uint8_t op = code[i];
        if (op != 0xE8 && op != 0xE9) continue;
        int32_t rel = (int32_t)Rd32(&code[i + 1]);
        uint32_t site = m_text_lo + (uint32_t)i;
        uint32_t target = site + 5 + rel;
        if (!InText(target)) continue;
        auto &map = op == 0xE8 ? calls : jmps;
        map[target]++;
        if (op == 0xE8) {
            auto &v = m_callers[target];
            if (v.size() < 4096) v.push_back(site);
        }
    }

    size_t n_call = 0, n_jmp = 0, n_pro = 0, n_pad = 0, n_str = 0;
    for (const auto &kv : calls) {
        uint32_t va = kv.first, cnt = kv.second;
        size_t off = va - m_text_lo;
        if (off >= code.size()) continue;
        if (cnt < 2 && !(IsStartByte(code[off]) &&
                         (prologues.count(va) || padded.count(va) || (va & 0xF) == 0)))
            continue;
        char nm[32]; snprintf(nm, sizeof(nm), "sub_%08X", va);
        if (Add(va, nm, XemuDbgFunctionSource::Call)) n_call++;
        m_by_addr[va].xrefs += cnt;
    }
    for (const auto &kv : jmps) {
        uint32_t va = kv.first;
        if (!prologues.count(va) && !padded.count(va)) continue;
        char nm[32]; snprintf(nm, sizeof(nm), "sub_%08X", va);
        if (Add(va, nm, XemuDbgFunctionSource::Call)) n_jmp++;
        m_by_addr[va].xrefs += kv.second;
    }
    for (uint32_t va : prologues) {
        char nm[32]; snprintf(nm, sizeof(nm), "sub_%08X", va);
        if (Add(va, nm, XemuDbgFunctionSource::Prologue)) n_pro++;
    }
    for (uint32_t va : padded) {
        if (prologues.count(va) || calls.count(va)) continue;
        char nm[32]; snprintf(nm, sizeof(nm), "sub_%08X", va);
        if (Add(va, nm, XemuDbgFunctionSource::Prologue)) n_pad++;
    }

    // Entry point in the in-memory XBE header has already been decrypted by
    // the loader.  If it lands in .text it deserves a stable name.
    if (struct xbe *xbe = xemu_get_xbe_info()) {
        uint32_t entry = xbe->header ? xbe->header->m_entry : 0;
        if (InText(entry)) Add(entry, "XBE entry point", XemuDbgFunctionSource::Entry);
    }

    // RTTI vtables are cheap and produce genuinely useful class names.
    ScanRtti();

    // String references, again mirroring the external tool's conservative
    // rule: only source-file-ish and function-ish literals may name a function.
    std::unordered_map<uint32_t, std::string> strings;
    strings.reserve(4096);
    for (const auto &sec : m_sections) {
        if (sec.name != ".rdata" && sec.name != ".data") continue;
        size_t want = std::min<size_t>(sec.hi - sec.lo, kMaxDataScan);
        std::vector<uint8_t> data(want);
        ssize_t n = xemu_dbg_read_space(sec.lo, data.data(), data.size(), true);
        if (n <= 0) continue;
        data.resize((size_t)n);
        for (size_t i = 0; i < data.size();) {
            size_t j = i;
            while (j < data.size() && data[j] >= 0x20 && data[j] <= 0x7E && j - i < 100) j++;
            if (j - i >= 4 && j < data.size() && data[j] == 0) {
                std::string s((const char *)&data[i], j - i);
                if (LooksFileish(s) || LooksFuncish(s)) strings[sec.lo + (uint32_t)i] = s;
                i = j + 1;
            } else {
                i++;
            }
        }
    }

    if (!strings.empty() && !m_by_addr.empty()) {
        // Build a temporary sorted start list before string names promote entries.
        Finalize();
        for (size_t i = 0; i + 5 <= code.size(); ++i) {
            uint8_t op = code[i];
            if (!(op == 0x68 || (op >= 0xB8 && op <= 0xBF))) continue;
            uint32_t imm = Rd32(&code[i + 1]);
            auto sit = strings.find(imm);
            if (sit == strings.end()) continue;
            uint32_t site = m_text_lo + (uint32_t)i;
            auto it = std::upper_bound(m_entries.begin(), m_entries.end(), site,
                [](uint32_t v, const XemuDbgFunctionEntry &e) { return v < e.addr; });
            if (it == m_entries.begin()) continue;
            --it;
            if (site - it->addr > 0x800) continue;
            std::string short_name = Basename(sit->second);
            if (LooksFileish(sit->second)) {
                auto f = m_by_addr.find(it->addr);
                if (f != m_by_addr.end() && f->second.note.empty()) f->second.note = short_name;
            } else if (Add(it->addr, short_name + "?", XemuDbgFunctionSource::String,
                           "from a literal")) {
                n_str++;
            }
        }
    }

    Finalize();
    if (status) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "%zu functions: %zu call targets, %zu jump thunks, %zu prologue/padding, %zu literal names",
                 m_entries.size(), n_call, n_jmp, n_pro + n_pad, n_str);
        *status = msg;
    }
    return true;
}

bool XemuDbgFunctionIndex::ImportSymbols(const char *path, std::string *status)
{
    if (!path || !*path) return false;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        if (status) *status = "Could not open the symbol file.";
        return false;
    }

    char line[2048];
    size_t added = 0, dropped = 0, parsed = 0;
    while (fgets(line, sizeof(line), fp) && parsed < 400000) {
        char seg[32] = {}, raw[1024] = {}, absbuf[32] = {};
        uint32_t va = 0;
        bool ok = false;

        // Microsoft linker map: "0001:00001234 symbol 00111234 ..."
        if (sscanf(line, " %31[0-9A-Fa-f]:%*8[0-9A-Fa-f] %1023s %31[0-9A-Fa-f]",
                   seg, raw, absbuf) == 3) {
            ok = ParseHexToken(absbuf, &va);
        } else {
            // Flat forms: "00111234 name", "0x00111234,name", "addr: name".
            char *p = line;
            while (*p && std::isspace((unsigned char)*p)) ++p;
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
            char *end = nullptr;
            unsigned long v = strtoul(p, &end, 16);
            if (end != p && v <= std::numeric_limits<uint32_t>::max()) {
                while (*end && (std::isspace((unsigned char)*end) || *end == ',' || *end == ':')) ++end;
                if (*end) {
                    va = (uint32_t)v;
                    size_t n = strcspn(end, " \t\r\n");
                    if (n > 0 && n < sizeof(raw)) {
                        memcpy(raw, end, n); raw[n] = 0; ok = true;
                    }
                }
            }
        }
        if (!ok) continue;
        parsed++;
        if (HaveImage() && !InImage(va)) { dropped++; continue; }
        if (Add(va, DemangleMsvcLight(raw), XemuDbgFunctionSource::Symbol)) added++;
    }
    fclose(fp);
    Finalize();

    if (status) {
        char msg[256];
        snprintf(msg, sizeof(msg), "%zu symbol(s) imported%s%zu%s",
                 added, dropped ? "; " : "", dropped,
                 dropped ? " outside-image line(s) dropped" : "");
        *status = msg;
    }
    return added != 0;
}

const XemuDbgFunctionEntry *XemuDbgFunctionIndex::Exact(uint32_t addr) const
{
    auto it = std::lower_bound(m_entries.begin(), m_entries.end(), addr,
        [](const XemuDbgFunctionEntry &e, uint32_t v) { return e.addr < v; });
    return it != m_entries.end() && it->addr == addr ? &*it : nullptr;
}

const XemuDbgFunctionEntry *XemuDbgFunctionIndex::Nearest(uint32_t addr,
                                                           uint32_t max_delta) const
{
    if (m_entries.empty()) return nullptr;
    auto it = std::upper_bound(m_entries.begin(), m_entries.end(), addr,
        [](uint32_t v, const XemuDbgFunctionEntry &e) { return v < e.addr; });
    if (it == m_entries.begin()) return nullptr;
    --it;
    return addr - it->addr <= max_delta ? &*it : nullptr;
}

const XemuDbgSectionRange *XemuDbgFunctionIndex::SectionOf(uint32_t addr) const
{
    for (const auto &s : m_sections) if (addr >= s.lo && addr < s.hi) return &s;
    return nullptr;
}

const std::vector<uint32_t> *XemuDbgFunctionIndex::CallersOf(uint32_t addr) const
{
    auto it = m_callers.find(addr);
    return it == m_callers.end() ? nullptr : &it->second;
}
