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

// Guest RAM as the engine sees it: physical offsets, forwarded to the shared
// accessor in xemu-features/shared/guest-memory.h. The engine never learns whether it is talking
// to a real machine or the test harness's flat buffer, which is what let the
// interpreter be verified on the host before any of this existed.
class GuestMemory : public xcodes::Memory {
public:
    bool Read(uint32_t off, void *buf, size_t len) override;
    size_t ReadPartial(uint32_t off, void *buf, size_t len) override;
    void Write(uint32_t off, const void *buf, size_t len) override;
    uint32_t RamSize() const override;
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

private:
    // Resolve the directory for one kind. Caller frees.
    static char *DirFor(const char *folder);

    struct CompiledBlock {
        const xcheat::Node *node = nullptr;
        xcodes::CodeList codes;
        uint32_t bid = 0;
    };

    void LoadOne(Section &sec, const char *folder);
    void SaveOne(Section &sec, const char *folder);
    void RebuildLive();
    void RebuildLiveFrom(const xcheat::NodeList &nodes);

    std::string m_stem, m_title;
    uint32_t    m_last_identify_ms = 0;
    uint32_t    m_last_apply_ms = 0;
    Section m_cheats, m_patches;
    std::vector<CompiledBlock> m_live;
    GuestMemory m_mem;
    xcodes::Engine m_engine;
    bool m_engine_attached = false;
};

extern CodesManager g_codes;
