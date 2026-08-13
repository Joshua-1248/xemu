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
// cheatfile.hh - Xbox cheat/patch .txt parser.
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>
#include <memory>
#include <set>

namespace xcheat {

struct Code { uint32_t cmd, val; };

struct Node;
using NodeList = std::vector<std::unique_ptr<Node>>;

// A cheat or a group. Groups have children and no codes; cheats the reverse.
// One struct rather than a hierarchy because the tree is walked far more often
// than it is built, and a virtual call per node in the freeze loop is not
// worth the tidiness.
struct Node {
    bool is_group = false;
    std::string name;
    // cheat
    std::vector<Code> codes;
    bool enabled = false;
    std::string desc;
    std::string author;
    // group
    NodeList children;
    bool expanded = true;
};

struct Meta {
    std::string game, serial, titleid, kind;
    std::vector<std::string> warnings;
};

// Parse a cheat/patch file's contents. Always succeeds; anything unrecognised
// lands in meta->warnings with a line number rather than being swallowed.
bool ParseCheatText(const std::string &content, NodeList *root, Meta *meta,
                    bool sep_slash = false);

// Inverse of ParseCheatText. `kind` is deliberately not a parameter: the
// folder the file sits in already records it.
std::string RenderCheatText(const std::string &title,
                            const std::string &serial,
                            const std::string &titleid,
                            const NodeList &tree,
                            const std::string &stem);

std::vector<std::string> SplitPath(const std::string &name, bool sep_slash = false);
bool SplitStem(const std::string &stem, std::string *serial, std::string *titleid);
bool SerialFromTitleId(uint32_t tid, std::string *out);

} // namespace xcheat
