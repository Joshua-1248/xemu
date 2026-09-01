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
//
// xemu User Interface - Codes (cheats and patches)
//
#pragma once

#include "ui/xui/main-menu.hh"
#include "cheatfile.hh"
#include "codes-engine.hh"

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

// Guest RAM as the engine sees it: physical offsets, forwarded to the shared
// accessor in xemu-features/shared/guest-memory.h. The engine never learns whether it is talking
// to a real machine or the test harness's flat buffer, which is what let the
// interpreter be verified on the host before any of this existed.
class GuestMemory : public xcodes::Memory {
public:
    struct AsmRestoreResult {
        size_t restored_ranges = 0;
        size_t failed_ranges = 0;
    };

    bool Read(uint32_t off, void *buf, size_t len) override;
    size_t ReadPartial(uint32_t off, void *buf, size_t len) override;
    void Write(uint32_t off, const void *buf, size_t len) override;
    uint32_t RamSize() const override;

    // [ASM] blocks are ordinary existing cheat-code writes with one extra
    // contract: capture original code bytes once, and restore them when that
    // block is disabled. The reserved Type F code is not involved.
    void BeginAsmBlock(uint32_t bid);
    void EndAsmBlock();
    AsmRestoreResult RestoreAsm(uint32_t bid);
    AsmRestoreResult RestoreAllAsm();
    void ClearAsmJournal();
    size_t AsmPatchByteCount(uint32_t bid = 0, bool all = true) const;

private:
    void JournalAsmWrite(uint32_t off, size_t len);

    bool m_asm_active = false;
    uint32_t m_asm_bid = 0;
    // bid -> (physical byte address -> original byte). Byte-granular storage
    // makes overlapping 8/16/32-bit writes restore exactly once.
    std::unordered_map<uint32_t, std::unordered_map<uint32_t, uint8_t>>
        m_asm_orig;
    // Exact write ranges already journaled for a block. Normal [ASM] execution
    // repeats the same 1/2/4-byte writes, so this turns the steady-state check
    // into one lookup while overlapping/conditional writes still fall back to
    // the byte-granular journal above.
    std::unordered_map<uint32_t, std::unordered_set<uint64_t>>
        m_asm_journaled_ranges;
};

class CodesManager {
public:
    struct Section {
        xcheat::NodeList root;
        xcheat::Meta meta;
    };

    // Re-read the XBE certificate; reload the files if the game changed.
    bool IdentifyGame();
    void LoadForCurrentGame();

    // Apply every enabled cheat once. Call from the frame loop.
    void Tick();

    void DrawSection(Section &sec, const char *empty_msg);
    void DrawTree(xcheat::NodeList &nodes, int depth, Section *sec);

    // Write a section back to disk. Called on every toggle, not at exit:
    // atexit() does not run on a crash or force-close.
    void Save(Section &sec);

    const std::string &Stem() const { return m_stem; }
    const std::string &Title() const { return m_title; }
    Section &Cheats() { return m_cheats; }
    Section &Patches() { return m_patches; }

    // Add a debugger-generated code patch to the conventional [ASM] group.
    // The caller supplies ordinary existing code lines (typically 8/9/A).
    // Type F remains reserved and is neither generated nor interpreted here.
    bool AddGeneratedAsmCheat(const char *name, const char *desc,
                              const xcheat::Code *codes, size_t count,
                              bool enabled);

    // UI tree editing helpers. These keep the mutation policy inside the
    // feature rather than teaching native xemu anything about cheat nodes.
    // PrepareNodeMutation() must be called before changing/removing a cheat
    // whose currently-active state may need to be unwound (especially [ASM]).
    void PrepareNodeMutation(xcheat::Node &node);
    void FinishTreeMutation(Section &sec, xcheat::Node *changed = nullptr);

private:
    // Resolve the directory for one kind. Caller frees.
    static char *DirFor(const char *folder);

    struct CompiledBlock {
        const xcheat::Node *node = nullptr;
        xcodes::CodeList codes;
        uint32_t bid = 0;
        bool asm_patch = false;
    };

    void LoadOne(Section &sec, const char *folder);
    void SaveOne(Section &sec, const char *folder);
    void RebuildLive();
    void RebuildLiveFrom(const xcheat::NodeList &nodes);
    void ApplyBlockNow(const CompiledBlock &block);
    static bool IsAsmName(const std::string &name);

    std::string m_stem, m_title;
    // Last definitively identified title for which the [ASM] original-byte
    // journal is valid. Kept across a transient XBE-identification miss so a
    // single failed poll cannot destroy restore information.
    std::string m_asm_journal_stem;
    uint32_t    m_last_identify_ms = 0;
    uint32_t    m_last_apply_ms = 0;
    Section m_cheats, m_patches;
    std::vector<CompiledBlock> m_live;
    GuestMemory m_mem;
    xcodes::Engine m_engine;
    bool m_engine_attached = false;
    bool m_runtime_enabled_last = false;
};

extern CodesManager g_codes;
