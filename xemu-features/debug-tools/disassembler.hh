// SPDX-License-Identifier: GPL-2.0-or-later
//
// xemu User Interface - Disassembler / CPU debugger
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
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

    struct SavedAddress {
        uint64_t id = 0;
        uint64_t parent_id = 0;
        bool is_group = false;
        bool expanded = true;
        std::string description;
        uint32_t addr = 0;
        bool virt = true;
        int value_type = 2; // int32; same numbering as Memory Search
        uint32_t byte_size = 4;
        bool frozen = false;
        std::vector<uint8_t> frozen_bytes;
    };

    void DrawNavigationBar();
    void DrawDebugBar();
    void DrawFunctionBrowser(float width);
    void DrawDisassembly();
    void DrawRegisters();
    void DrawBreakpoints();
    void DrawMemory();
    void DrawMemorySearch();
    void DrawSavedAddresses();
    void DrawUpperLeftPane();
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

    // Integrated memory scanner. This is a native/in-process port of the
    // useful value-scanning workflow from the user's standalone Xemu Cheat
    // Engine, but it reads guest memory through Xemu's feature-owned APIs.
    enum class MemScanPhase : uint8_t {
        Idle,
        ReadFirst,
        EvalFirst,
        ReadNext,
        EvalNext,
    };

    void TickMemorySearch();
    void ResetMemorySearch();
    void CancelMemorySearch();
    bool StartMemoryFirstScan();
    bool StartMemoryNextScan();
    bool ConfigureMemoryScanRange(std::string *error);
    bool ParseMemoryScanTarget(bool first_scan, std::string *error);
    void PrepareMemoryNextReadPlan();
    bool MemoryScanCandidateValid(const std::vector<uint8_t> &valid_pages,
                                  uint32_t addr, size_t len) const;
    bool MemoryScanFirstMatch(uint32_t addr) const;
    bool MemoryScanNextMatch(uint32_t addr) const;
    bool MemoryScanBit(uint64_t slot) const;
    void SetMemoryScanBit(uint64_t slot, bool value);
    void RebuildMemoryScanPage();
    void RefreshMemoryScanLiveValues(size_t begin, size_t end);
    std::string FormatMemoryScanRawValue(const uint8_t *raw) const;
    std::string FormatMemoryScanSnapshotValue(const std::vector<uint8_t> &snapshot,
                                              uint32_t addr) const;
    std::string FormatMemoryScanLiveValue(uint32_t addr) const;
    bool MemoryScanAddressToVirtual(uint32_t addr, uint32_t *va) const;

    // Cheat Engine-style saved-address table, implemented natively in C++.
    void TickSavedAddresses();
    void EnsureSavedAddressesLoaded();
    void InvalidateSavedAddressIndex();
    void EnsureSavedAddressIndex();
    bool SaveSavedAddresses(std::string *error = nullptr);
    bool LoadSavedAddresses(std::string *error = nullptr);
    void DrawSavedAddressChildren(uint64_t parent_id, int depth);
    void OpenSavedAddressEditor(uint64_t id, uint64_t parent_id, bool group);
    void DrawSavedAddressEditor();
    void DrawSavedValueEditor();
    void DeleteSavedAddressSubtree(uint64_t id);
    void SetSavedAddressFrozen(uint64_t id, bool frozen, bool recurse);
    void AddScanResultToSavedAddresses(uint32_t addr, bool virt, int value_type,
                                       uint32_t byte_size);
    std::string FormatSavedAddressValue(const SavedAddress &entry) const;
    SavedAddress *FindSavedAddress(uint64_t id);
    const SavedAddress *FindSavedAddress(uint64_t id) const;
    bool SavedAddressSelected(uint64_t id) const;
    void SelectSavedAddress(uint64_t id, bool toggle);
    uint32_t DefaultSavedAddressSizeForType(int value_type) const;

    uint32_t m_base = 0x00010000;
    uint32_t m_selected = 0;
    uint32_t m_selection_anchor = 0;
    std::vector<uint32_t> m_selected_instructions;
    bool m_follow_eip = true;
    uint32_t m_last_follow_eip = 0;
    bool m_last_follow_eip_valid = false;
    bool m_last_debug_running = true;
    float m_disasm_wheel_accum = 0.0f;
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
    uint32_t m_mem_sel_anchor = 0;
    bool     m_mem_have_sel = false;
    bool     m_mem_edit_text = false;
    bool     m_mem_keyboard_active = false;
    bool     m_mem_input_focused = false;
    bool     m_mem_text_focus_pending = false;
    bool     m_mem_drag_selecting = false;
    bool     m_mem_drag_text = false;
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
    bool     m_open_memory_tab_requested = false;
    float    m_mem_wheel_accum = 0.0f;
    // Selected-value interpretation used by the Memory context menu/details.
    // 0=u8, 1=u16, 2=u32, 3=u64, 4=float32, 5=float64.
    int      m_mem_value_view = 2;

    // Memory search / Cheat Engine style scanner. Candidate membership is a
    // compact bitset indexed by aligned scan slots, so an unknown int8 scan
    // across 128 MiB stays bounded instead of allocating one integer object
    // per byte. Two snapshots are enough: baseline + work/previous-display.
    int      m_scan_value_type = 2; // int32
    int      m_scan_compare = 0;    // Equal To
    int      m_scan_region = 0;     // All physical RAM
    bool     m_scan_hex = false;
    bool     m_scan_custom_virtual = false;
    char     m_scan_value[160] = "";
    char     m_scan_value_max[160] = "";
    char     m_scan_custom_lo[16] = "00000000";
    char     m_scan_custom_hi[16] = "08000000";

    MemScanPhase m_scan_phase = MemScanPhase::Idle;
    bool     m_scan_baseline_ready = false;
    bool     m_scan_baseline_all_valid = false;
    bool     m_scan_work_all_valid = false;
    bool     m_scan_have_previous_display = false;
    bool     m_scan_operation_unknown = false;
    int      m_scan_operation_compare = 0;
    int      m_scan_locked_type = 2;
    bool     m_scan_virtual = false;
    uint32_t m_scan_range_lo = 0;
    uint32_t m_scan_range_hi = 0; // exclusive
    uint32_t m_scan_first_addr = 0;
    uint32_t m_scan_item_size = 4;
    uint32_t m_scan_step = 4;
    uint64_t m_scan_candidate_slots = 0;
    uint64_t m_scan_candidate_count = 0;
    uint64_t m_scan_work_candidate_count = 0;
    uint64_t m_scan_eval_slot = 0;
    size_t   m_scan_eval_word_cursor = 0;
    uint64_t m_scan_io_cursor = 0;
    size_t   m_scan_io_page_index = 0;
    uint64_t m_scan_io_pages_done = 0;
    uint64_t m_scan_io_pages_total = 0;
    uint32_t m_scan_valid_first_page = 0;
    bool     m_scan_sparse_read = false;

    uint64_t m_scan_target_u64 = 0;
    uint64_t m_scan_target2_u64 = 0;
    double   m_scan_target_f64 = 0.0;
    double   m_scan_target2_f64 = 0.0;
    std::vector<uint8_t> m_scan_target_bytes;
    std::vector<uint8_t> m_scan_target_mask;
    int      m_scan_aob_anchor = -1;
    uint8_t  m_scan_aob_anchor_byte = 0;
    int      m_scan_search_anchor = -1;
    uint8_t  m_scan_search_anchor_byte = 0;

    std::vector<uint8_t> m_scan_baseline;
    std::vector<uint8_t> m_scan_work;
    std::vector<uint8_t> m_scan_baseline_valid_pages;
    std::vector<uint8_t> m_scan_work_valid_pages;
    std::vector<uint8_t> m_scan_needed_pages;
    std::vector<uint64_t> m_scan_candidate_bits;
    std::vector<uint64_t> m_scan_work_candidate_bits;
    // When candidates are sparse, retain the exact non-empty bitset-word
    // indices. This turns later scans into O(survivors) traversal instead of
    // walking the full 16 MiB int8 candidate bitset just to skip zero words.
    std::vector<uint32_t> m_scan_candidate_words;
    std::vector<uint32_t> m_scan_work_candidate_words;
    bool     m_scan_candidate_words_valid = true;
    bool     m_scan_work_candidate_words_valid = true;

    uint64_t m_scan_generation = 1;
    uint64_t m_scan_page_generation = 0;
    uint64_t m_scan_page_index = 0;
    std::vector<uint32_t> m_scan_page_addresses;
    std::vector<std::string> m_scan_live_values;
    bool     m_scan_have_selected = false;
    uint32_t m_scan_selected_addr = 0;
    std::string m_scan_status = "Ready for a new scan";

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
    float m_left_pane_ratio = 0.30f;

    // Saved Addresses
    std::vector<SavedAddress> m_saved_addresses;
    std::vector<uint64_t> m_saved_selected;
    std::unordered_set<uint64_t> m_saved_selected_set;
    // Retained tree/freeze index. Saved-address structure changes only on
    // explicit UI mutations or title loads, so rebuilding hierarchy/frozen
    // ancestry every ImGui frame is pure overhead.
    bool m_saved_index_dirty = true;
    std::unordered_map<uint64_t, size_t> m_saved_index;
    std::unordered_map<uint64_t, std::vector<uint64_t>> m_saved_children;
    std::unordered_set<uint64_t> m_saved_groups_with_frozen_descendants;
    std::vector<size_t> m_saved_frozen_indices;
    // Per-depth child snapshots keep draw iteration mutation-safe without
    // allocating a fresh std::vector for every open group every frame.
    std::vector<std::vector<uint64_t>> m_saved_draw_children_scratch;
    uint64_t m_saved_next_id = 1;
    uint32_t m_saved_title_id = 0;
    bool m_saved_loaded = false;
    uint32_t m_saved_last_title_poll_ms = 0;
    uint32_t m_saved_last_freeze_ms = 0;
    bool m_saved_editor_open_requested = false;
    bool m_saved_editor_is_group = false;
    uint64_t m_saved_editor_id = 0;
    uint64_t m_saved_editor_parent_id = 0;
    char m_saved_editor_desc[192] = "";
    char m_saved_editor_addr[16] = "00000000";
    int m_saved_editor_type = 2;
    int m_saved_editor_size = 4;
    bool m_saved_editor_virtual = true;
    std::string m_saved_editor_error;
    bool m_saved_value_open_requested = false;
    char m_saved_value_buf[256] = "";
    std::string m_saved_value_error;

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
