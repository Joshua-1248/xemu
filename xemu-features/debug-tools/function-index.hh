// SPDX-License-Identifier: GPL-2.0-or-later
//
// Function discovery / symbol index for the custom in-xemu debugger.
// The discovery policy is a native C++ port of the user's earlier
// Xemu Cheat Engine FunctionIndex workflow.  It intentionally stays under
// xemu-features/debug-tools/ so native Xemu ownership remains untouched.
#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

enum class XemuDbgFunctionSource : uint8_t {
    Prologue = 1,
    Call     = 2,
    String   = 3,
    Entry    = 4,
    Rtti     = 5,
    Symbol   = 6,
};

struct XemuDbgSectionRange {
    std::string name;
    uint32_t lo = 0;
    uint32_t hi = 0;
};

struct XemuDbgFunctionEntry {
    uint32_t addr = 0;
    std::string name;
    XemuDbgFunctionSource source = XemuDbgFunctionSource::Prologue;
    uint32_t xrefs = 0;
    std::string note;
};

class XemuDbgFunctionIndex {
public:
    void Clear();

    // Synchronous on purpose: the button is an explicit RE action and doing
    // guest-memory reads from another host thread would be far less safe than
    // letting the UI spend a moment on the scan.
    bool Scan(std::string *status = nullptr);
    bool ImportSymbols(const char *path, std::string *status = nullptr);

    const std::vector<XemuDbgFunctionEntry> &Entries() const { return m_entries; }
    const std::vector<XemuDbgSectionRange> &Sections() const { return m_sections; }

    const XemuDbgFunctionEntry *Exact(uint32_t addr) const;
    const XemuDbgFunctionEntry *Nearest(uint32_t addr, uint32_t max_delta = 0x4000) const;
    const XemuDbgSectionRange *SectionOf(uint32_t addr) const;
    const std::vector<uint32_t> *CallersOf(uint32_t addr) const;

    bool HaveImage() const { return m_image_hi > m_image_lo; }
    uint32_t ImageLo() const { return m_image_lo; }
    uint32_t ImageHi() const { return m_image_hi; }
    uint32_t TextLo() const { return m_text_lo; }
    uint32_t TextHi() const { return m_text_hi; }
    bool InImage(uint32_t addr) const { return addr >= m_image_lo && addr < m_image_hi; }
    bool InText(uint32_t addr) const { return addr >= m_text_lo && addr < m_text_hi; }

    static const char *SourceLabel(XemuDbgFunctionSource source);

private:
    bool Add(uint32_t addr, const std::string &name,
             XemuDbgFunctionSource source, const std::string &note = {});
    void Finalize();
    bool BuildSections(std::string *status);
    void ScanRtti();

    std::unordered_map<uint32_t, XemuDbgFunctionEntry> m_by_addr;
    std::unordered_map<uint32_t, std::vector<uint32_t>> m_callers;
    std::vector<XemuDbgFunctionEntry> m_entries;
    std::vector<XemuDbgSectionRange> m_sections;
    uint32_t m_image_lo = 0;
    uint32_t m_image_hi = 0;
    uint32_t m_text_lo = 0;
    uint32_t m_text_hi = 0;
};
