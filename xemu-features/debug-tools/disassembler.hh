// SPDX-License-Identifier: GPL-2.0-or-later
//
// xemu User Interface - Disassembler / CPU debugger
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <utility>

#include "xemu-features/debug-tools/debug-api.h"
#include "xemu-features/debug-tools/function-index.hh"

class DisassemblerWindow
{
public:
    bool m_is_open;

    DisassemblerWindow();
    void Draw();

private:
    struct Line {
        uint32_t addr = 0;
        uint8_t  bytes[16] {};
        uint8_t  len = 0;
        char     mnemonic[32] {};
        char     ops[160] {};
        bool     valid = false;
    };

    struct Bp {
        uint32_t addr = 0;
        bool virt = true;
        bool enabled = true;
    };

    struct Wp {
        uint32_t addr = 0;
        uint32_t len = 1;
        int flags = 0;
        bool virt = true;
        bool enabled = true;
    };

    struct CodePatch {
        uint32_t addr = 0;
        std::vector<uint8_t> original;
        std::vector<uint8_t> patched;
    };

    struct GlobalWatch {
        uint32_t addr = 0;
        std::string source;
    };

    void DrawNavigationBar();
    void DrawDebugBar();
    void DrawFunctionBrowser(float width);
    void DrawDisassembly();
    void DrawRegisters();
    void DrawBreakpoints();
    void DrawMemory();
    void DrawBottomPanels();
    void DrawStack();
    void DrawFrameSlots(bool parameters);
    void DrawThreads();
    void DrawGlobals();
    void DrawPatchModal();
    void DrawCreateCheatModal();
    void DrawXrefPopup();

    void Refresh();
    void GoTo(uint32_t addr, bool centre = false, bool push_history = true);
    void Back();
    bool ReadEip(uint32_t *out);
    const XemuDbgRegs &Regs();
    uint32_t BackUp(uint32_t addr, int instructions) const;

    const Line *SelectedLine() const;
    bool IsInstructionSelected(uint32_t addr) const;
    void SelectOnlyInstruction(uint32_t addr);
    void ToggleInstructionSelection(uint32_t addr);
    void SelectInstructionRange(uint32_t addr);
    std::vector<const Line *> SelectedLines() const;
    void CopySelectedInstructions();
    void CopySelectedBytes();
    void CopySelectedNopCheats();
    static bool ParseBranchTarget(const Line &line, uint32_t *target);
    bool HasBreakpoint(uint32_t addr) const;
    void ToggleBreakpoint(uint32_t addr);

    void ScanFunctions();
    void ImportSymbolsDialog();
    bool FunctionVisible(const XemuDbgFunctionEntry &entry) const;
    void RebuildVisibleFunctions();
    static void FormatFunctionDisplay(const XemuDbgFunctionEntry &entry,
                                      char *out, size_t out_size);
    std::string NearestName(uint32_t addr) const;
    std::string DescribePointer(uint32_t value) const;

    bool ApplyPatch(uint32_t addr, const std::vector<uint8_t> &bytes,
                    std::string *error = nullptr);
    bool UndoPatch(uint32_t addr, std::string *error = nullptr);
    void UndoAllPatches();
    CodePatch *FindPatch(uint32_t addr);
    const CodePatch *FindPatch(uint32_t addr) const;
    bool RangeOverlapsPatch(uint32_t addr, size_t len,
                            const CodePatch **hit = nullptr) const;
    void OpenPatchModal(uint32_t addr);
    void NopSelected();
    std::vector<std::pair<uint32_t, uint32_t>> GenerateVirtualWriteCodePairs(
        uint32_t addr, const std::vector<uint8_t> &bytes) const;
    std::string GenerateVirtualWriteCodes(uint32_t addr,
                                          const std::vector<uint8_t> &bytes) const;
    void CopyPatchAsCheat(uint32_t addr);
    void CopyNopAsCheat(uint32_t addr, size_t len);
    void OpenCreateCheatModal(uint32_t addr);
    bool TransferPatchToCheats(uint32_t addr, const char *name,
                               const char *desc, bool enabled);

    void AddGlobalsFromCurrentFunction();

    uint32_t m_base = 0x00010000;
    uint32_t m_selected = 0;
    uint32_t m_selection_anchor = 0;
    std::vector<uint32_t> m_selected_instructions;
    bool m_follow_eip = true;
    bool m_have_selection = false;
    char m_goto_buf[16] = "00010000";

    std::vector<uint32_t> m_history;

    // Function/symbol browser
    XemuDbgFunctionIndex m_functions;
    bool m_functions_scanned = false;
    char m_func_search[128] = "";
    int m_func_filter = 0; // all, named, symbols, RTTI, detected
    // Search/filter results are rebuilt only when the query/index changes.
    // The entries vector can contain tens of thousands of functions, so doing
    // two full scans plus allocations every ImGui frame was unnecessary work.
    std::vector<const XemuDbgFunctionEntry *> m_visible_functions;
    bool m_visible_functions_dirty = true;
    uint32_t m_xref_target = 0;
    bool m_open_xref_popup = false;

    // Memory viewer
    char     m_mem_buf[16] = "00010000";
    uint32_t m_mem_addr = 0x00010000;
    bool     m_mem_virtual = true;
    bool     m_mem_big_endian = false;
    int      m_mem_region = 0;
    int      m_mem_bytes_per_row = 16;
    uint32_t m_mem_sel = 0;
    bool     m_mem_have_sel = false;
    bool     m_mem_edit_text = false;
    bool     m_mem_keyboard_active = false;
    int      m_mem_nibble = 0; // 0 = high nibble next, 1 = low nibble next
    char     m_mem_input_sink[64] = "";
    // Sized to match the standalone Xemu Cheat Engine viewer: the
    // visible bytes/row are derived from the pane width and can grow well
    // beyond the old fixed 8-32 byte table.
    uint8_t  m_mem_cache[64][128] {};
    int      m_mem_cache_got[64] {};
    uint32_t m_mem_cache_addr = 0;
    int      m_mem_cache_per = 0;
    int      m_mem_cache_rows = 0;
    bool     m_mem_cache_virtual = true;
    bool     m_mem_cache_valid = false;

    // Debugger state
    std::string m_status;
    uint32_t m_status_ms = 0;
    std::vector<Line> m_lines;
    std::vector<Bp> m_bps;
    std::vector<Wp> m_wps;

    uint32_t m_last_base = 0xFFFFFFFF;
    bool m_dirty = true;
    XemuDbgRegs m_regs {};
    bool m_regs_fresh = false;

    bool m_live = true;
    int m_interval_ms = 100;
    uint32_t m_last_poll_ms = 0;
    bool m_poll_now = false;

    // User-adjustable vertical split between the disassembly workspace and
    // the lower register/memory/debugger panels.
    float m_code_split_ratio = 0.62f;

    // Live code patches / ASM-cheat authoring helpers.  These deliberately
    // generate ordinary existing write codes; reserved Type F is untouched.
    std::vector<CodePatch> m_patches;
    bool m_patch_modal_requested = false;
    uint32_t m_patch_addr = 0;
    char m_patch_bytes[768] = "";

    // Direct Debugger -> Cheats handoff. The debugger restores its temporary
    // live patch before the Cheats feature takes ownership, so the [ASM]
    // journal captures the true original code bytes.
    bool m_create_cheat_requested = false;
    uint32_t m_create_cheat_addr = 0;
    char m_create_cheat_name[160] = "";
    char m_create_cheat_desc[320] = "";
    bool m_create_cheat_enabled = true;

    std::vector<GlobalWatch> m_globals;
    char m_global_addr[16] = "00000000";
};

extern DisassemblerWindow disassembler_window;
