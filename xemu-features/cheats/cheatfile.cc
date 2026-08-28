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
// cheatfile.cc - Xbox cheat/patch .txt parser.
//
// Direct port of xemu_trainer_lib/cheatfiles.py's parse_cheat_text() and the
// helpers it depends on. Intended as ui/xui/cheatfile.cc.
//
// The grammar has more history in it than it looks: three generations of file
// format still load, and the parser is expected to accept all of them because
// the 736-file database was written across all three. Every apparent oddity
// below is load-bearing - see the comments, and cheatfiles.py, before
// "simplifying" anything.

#include "cheatfile.hh"
#include <cctype>
#include <cstdio>
#include <algorithm>

namespace xcheat {

// ---------------------------------------------------------------------------
// Small string helpers
// ---------------------------------------------------------------------------

static std::string Strip(const std::string &s)
{
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

static std::string Lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

static bool StartsWith(const std::string &s, const char *p)
{
    size_t n = strlen(p);
    return s.size() >= n && s.compare(0, n, p) == 0;
}

// ---------------------------------------------------------------------------
// Line-level matchers
// ---------------------------------------------------------------------------
// Hand-written rather than <regex>: libstdc++'s regex is slow enough that
// parsing 736 files is noticeable, and these three patterns are trivial.

// _HEAD: ^\[(.*)\][ \t]*\{?[ \t]*$
// `(.*)` is GREEDY, so it backtracks to the LAST `]` on the line. Two groups
// in the database are literally named `[ASM]`; a non-greedy match drops those
// blocks silently.
static bool MatchHead(const std::string &line, std::string *name)
{
    if (line.empty() || line[0] != '[') return false;
    size_t end = line.rfind(']');
    if (end == std::string::npos || end == 0) return false;
    std::string tail = Strip(line.substr(end + 1));
    if (!tail.empty() && tail != "{") return false;
    *name = line.substr(1, end - 1);
    return true;
}

// _CODE: ^([0-9A-Fa-f]{1,8})[ \t]+([0-9A-Fa-f]{1,8})[ \t]*$
static bool MatchCode(const std::string &line, uint32_t *a, uint32_t *b)
{
    size_t i = 0, n = line.size();
    size_t s1 = i;
    while (i < n && isxdigit((unsigned char)line[i])) i++;
    size_t l1 = i - s1;
    if (l1 < 1 || l1 > 8) return false;
    if (i >= n || (line[i] != ' ' && line[i] != '\t')) return false;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    size_t s2 = i;
    while (i < n && isxdigit((unsigned char)line[i])) i++;
    size_t l2 = i - s2;
    if (l2 < 1 || l2 > 8) return false;
    while (i < n && (line[i] == ' ' || line[i] == '\t')) i++;
    if (i != n) return false;
    *a = (uint32_t)strtoul(line.substr(s1, l1).c_str(), nullptr, 16);
    *b = (uint32_t)strtoul(line.substr(s2, l2).c_str(), nullptr, 16);
    return true;
}

// _KV: ^([A-Za-z][A-Za-z0-9_ -]*?)[ \t]*=(.*)$
// Cannot collide with a code line, which has no `=`.
static bool MatchKV(const std::string &line, std::string *key, std::string *val)
{
    if (line.empty() || !isalpha((unsigned char)line[0])) return false;
    size_t eq = line.find('=');
    if (eq == std::string::npos) return false;
    std::string k = line.substr(0, eq);
    for (char c : k) {
        if (!(isalnum((unsigned char)c) || c == '_' || c == ' ' || c == '-'))
            return false;
    }
    // The Python pattern is non-greedy up to optional trailing whitespace,
    // which is exactly "strip the key".
    *key = Strip(k);
    if (key->empty()) return false;
    *val = line.substr(eq + 1);
    return true;
}

// ---------------------------------------------------------------------------
// Group paths
// ---------------------------------------------------------------------------
// Backslash alone is the separator, NOT backslash-or-slash: one group in the
// database is named "Driving/On-Rails", and treating `/` as a separator
// silently turns that single group into two nested ones. A literal backslash
// is written `\\`. sep_slash=true is only for clipboard imports.

std::vector<std::string> SplitPath(const std::string &name, bool sep_slash)
{
    std::vector<std::string> parts;
    std::string cur;
    size_t i = 0;
    while (i < name.size()) {
        char ch = name[i];
        if (sep_slash && ch == '/') {
            parts.push_back(cur); cur.clear(); i++; continue;
        }
        if (ch == '\\') {
            if (i + 1 < name.size() && name[i + 1] == '\\') {
                cur += '\\'; i += 2; continue;
            }
            parts.push_back(cur); cur.clear(); i++; continue;
        }
        cur += ch;
        i++;
    }
    parts.push_back(cur);

    std::vector<std::string> out;
    for (auto &p : parts) {
        std::string s = Strip(p);
        if (!s.empty()) out.push_back(s);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Tree construction
// ---------------------------------------------------------------------------

static NodeList *EnsureGroup(NodeList *root,
                             const std::vector<std::string> &parts,
                             bool expanded)
{
    NodeList *parent = root;
    for (const auto &comp : parts) {
        Node *found = nullptr;
        for (auto &n : *parent) {
            if (n->is_group && Lower(n->name) == Lower(comp)) {
                found = n.get();
                break;
            }
        }
        if (!found) {
            auto g = std::make_unique<Node>();
            g->is_group = true;
            g->name = comp;
            g->expanded = expanded;
            found = g.get();
            parent->push_back(std::move(g));
        }
        parent = &found->children;
    }
    return parent;
}

// ---------------------------------------------------------------------------
// Logical lines
// ---------------------------------------------------------------------------
// Almost every line is one unit. The exception is the oldest form, all on one
// line: `[Name] { AAAAAAAA BBBBBBBB }`. The bracketed name comes off the front
// first, up to the LAST `]`, so a brace inside a cheat's own name is never
// mistaken for a delimiter.

struct LogicalLine { int lineno; std::string text; };

static std::vector<LogicalLine> LogicalLines(const std::string &content)
{
    std::vector<LogicalLine> out;
    size_t pos = 0;
    int lineno = 0;
    while (pos <= content.size()) {
        size_t nl = content.find('\n', pos);
        std::string raw = content.substr(pos, nl == std::string::npos
                                              ? std::string::npos : nl - pos);
        pos = (nl == std::string::npos) ? content.size() + 1 : nl + 1;
        lineno++;

        std::string s = Strip(raw);
        if (s.empty()) continue;

        if (StartsWith(s, "//") || s[0] == ';') {
            out.push_back({lineno, s});
            continue;
        }

        if (s[0] == '[') {
            size_t end = s.rfind(']');
            if (end != std::string::npos) {
                std::string head = s.substr(0, end + 1);
                s = Strip(s.substr(end + 1));
                // An opening brace still belongs to the header line, so
                // `[Name] {` stays one unit and MatchHead accepts it.
                if (!s.empty() && s[0] == '{') {
                    head += " {";
                    s = Strip(s.substr(1));
                }
                out.push_back({lineno, head});
                if (s.empty()) continue;
            }
        }

        // Split what remains on braces, keeping the braces as their own units.
        std::string cur;
        for (char c : s) {
            if (c == '{' || c == '}') {
                std::string piece = Strip(cur);
                if (!piece.empty()) out.push_back({lineno, piece});
                cur.clear();
                out.push_back({lineno, std::string(1, c)});
            } else {
                cur += c;
            }
        }
        std::string piece = Strip(cur);
        if (!piece.empty()) out.push_back({lineno, piece});
    }
    return out;
}

// ---------------------------------------------------------------------------
// Metadata application
// ---------------------------------------------------------------------------

struct Block {
    std::vector<std::string> parts;
    std::vector<Code> codes;
    std::vector<std::string> notes;
    std::string author;
    bool enabled = false;
    bool collapsed = false;
};

static void ApplyComment(Block &blk, const std::string &raw)
{
    size_t i = 0;
    while (i < raw.size() && raw[i] == ';') i++;
    std::string line = Strip(raw.substr(i));
    if (line.empty()) return;
    std::string low = Lower(line);
    if (StartsWith(low, "author:")) {
        blk.author = Strip(line.substr(line.find(':') + 1));
    } else if (StartsWith(low, "desc:") || StartsWith(low, "description:")) {
        blk.notes.push_back(Strip(line.substr(line.find(':') + 1)));
    } else {
        blk.notes.push_back(line);
    }
}

static bool Truthy(const std::string &v)
{
    std::string s = Lower(Strip(v));
    return !(s.empty() || s == "0" || s == "no" || s == "off" || s == "false");
}

static bool ApplyKey(Block &blk, const std::string &key_in,
                     const std::string &val_in)
{
    std::string key = Lower(Strip(key_in));
    std::string val = Strip(val_in);
    if (key == "author")        { blk.author = val; return true; }
    if (key == "desc" || key == "description" ||
        key == "note" || key == "notes") { blk.notes.push_back(val); return true; }
    if (key == "enabled")       { blk.enabled = Truthy(val); return true; }
    if (key == "collapsed")     { blk.collapsed = Truthy(val); return true; }
    return false;
}

static bool IsFileKey(const std::string &k)
{
    return k == "gametitle" || k == "game" || k == "serial" ||
           k == "titleid" || k == "kind";
}

static bool IsLegacyDirective(const std::string &k)
{
    return k == "game" || k == "serial" || k == "titleid" || k == "kind" ||
           k == "group" || k == "collapsed" || k == "enabled";
}

// ---------------------------------------------------------------------------
// The parser
// ---------------------------------------------------------------------------

bool ParseCheatText(const std::string &content, NodeList *root, Meta *meta,
                    bool sep_slash)
{
    root->clear();
    *meta = Meta();

    // Legacy //collapsed= is gathered up front: it may FOLLOW the //group=
    // line it refers to, and groups are created on sight.
    std::set<std::string> collapsed;
    {
        size_t pos = 0;
        while (pos <= content.size()) {
            size_t nl = content.find('\n', pos);
            std::string raw = content.substr(pos, nl == std::string::npos
                                                  ? std::string::npos : nl - pos);
            pos = (nl == std::string::npos) ? content.size() + 1 : nl + 1;
            std::string s = Strip(raw);
            if (StartsWith(s, "//collapsed=")) {
                collapsed.insert(Lower(Strip(s.substr(12))));
            }
        }
    }

    std::unique_ptr<Block> cur;
    bool seen_block = false;
    std::vector<std::string> pending_head;
    int pending_enabled = -1;       // -1 none, 0 false, 1 true

    auto SetFileKey = [&](const std::string &k, const std::string &v) {
        if (k == "gametitle" || k == "game") meta->game = v;
        else if (k == "serial")  meta->serial = v;
        else if (k == "titleid") meta->titleid = v;
        else if (k == "kind")    meta->kind = v;
    };

    auto Flush = [&]() {
        if (!cur) return;
        std::unique_ptr<Block> blk = std::move(cur);
        cur.reset();
        if (blk->parts.empty()) return;
        if (!blk->codes.empty()) {
            std::vector<std::string> parents(blk->parts.begin(),
                                             blk->parts.end() - 1);
            NodeList *parent = EnsureGroup(root, parents, true);
            auto n = std::make_unique<Node>();
            n->is_group = false;
            n->name = blk->parts.back();
            n->codes = blk->codes;
            n->enabled = blk->enabled;
            n->author = blk->author;
            // Notes are joined with a single space, exactly as Python's
            // " ".join(notes) - not newline-joined.
            for (size_t i = 0; i < blk->notes.size(); i++) {
                if (i) n->desc += " ";
                n->desc += blk->notes[i];
            }
            parent->push_back(std::move(n));
        } else {
            EnsureGroup(root, blk->parts, !blk->collapsed);
        }
    };

    char wbuf[256];

    for (const auto &ll : LogicalLines(content)) {
        const std::string &line = ll.text;

        if (line == "{" || line == "}") {
            if (line == "}") Flush();
            continue;
        }

        if (StartsWith(line, "//")) {
            std::string body = line.substr(2);
            size_t eq = body.find('=');
            std::string key = Lower(Strip(eq == std::string::npos
                                          ? body : body.substr(0, eq)));
            std::string val = (eq == std::string::npos)
                              ? "" : Strip(body.substr(eq + 1));
            if (eq == std::string::npos || !IsLegacyDirective(key)) {
                continue;           // an ordinary comment: no effect at all
            }
            if (IsFileKey(key)) { SetFileKey(key, val); continue; }
            // A legacy structural directive ends the block it follows.
            Flush();
            if (key == "group") {
                auto parts = SplitPath(val, sep_slash);
                if (!parts.empty())
                    EnsureGroup(root, parts, !collapsed.count(Lower(val)));
            } else if (key == "enabled") {
                pending_enabled = (val == "1") ? 1 : 0;
            }
            continue;
        }

        std::string head_name;
        if (MatchHead(line, &head_name)) {
            Flush();
            seen_block = true;
            auto parts = SplitPath(Strip(head_name), sep_slash);
            cur = std::make_unique<Block>();
            cur->parts = parts;
            cur->enabled = (pending_enabled == 1);
            for (const auto &c : pending_head) ApplyComment(*cur, c);
            pending_head.clear();
            pending_enabled = -1;
            if (parts.empty()) {
                snprintf(wbuf, sizeof(wbuf), "line %d: block has no name",
                         ll.lineno);
                meta->warnings.push_back(wbuf);
                cur.reset();
            }
            continue;
        }

        if (!line.empty() && line[0] == ';') {
            if (!cur) pending_head.push_back(line);
            else ApplyComment(*cur, line);
            continue;
        }

        uint32_t a, b;
        if (MatchCode(line, &a, &b)) {
            if (!cur) {
                snprintf(wbuf, sizeof(wbuf),
                         "line %d: code line before any [Name]", ll.lineno);
                meta->warnings.push_back(wbuf);
            } else {
                cur->codes.push_back({a, b});
            }
            continue;
        }

        std::string key, val;
        if (MatchKV(line, &key, &val)) {
            if (!cur && !seen_block) {
                if (IsFileKey(Lower(key))) SetFileKey(Lower(key), Strip(val));
                else {
                    snprintf(wbuf, sizeof(wbuf),
                             "line %d: unknown file key '%s'", ll.lineno,
                             key.c_str());
                    meta->warnings.push_back(wbuf);
                }
            } else if (!cur) {
                snprintf(wbuf, sizeof(wbuf), "line %d: %s= belongs to no [Name]",
                         ll.lineno, key.c_str());
                meta->warnings.push_back(wbuf);
            } else if (!ApplyKey(*cur, key, val)) {
                snprintf(wbuf, sizeof(wbuf), "line %d: unknown key '%s'",
                         ll.lineno, key.c_str());
                meta->warnings.push_back(wbuf);
            }
            continue;
        }

        snprintf(wbuf, sizeof(wbuf), "line %d: not understood: '%s'",
                 ll.lineno, line.c_str());
        meta->warnings.push_back(wbuf);
    }

    Flush();
    return true;
}

// ---------------------------------------------------------------------------
// Filename stem <-> ids
// ---------------------------------------------------------------------------

// `AV-032_41560020` -> ("AV-032", "41560020"); ("","") if it is a slug.
bool SplitStem(const std::string &stem, std::string *serial, std::string *titleid)
{
    // ^([A-Za-z]{2}-[0-9]{3})_([0-9A-Fa-f]{8})$
    if (stem.size() != 6 + 1 + 8) return false;
    if (!isalpha((unsigned char)stem[0]) || !isalpha((unsigned char)stem[1])) return false;
    if (stem[2] != '-') return false;
    for (int i = 3; i < 6; i++) if (!isdigit((unsigned char)stem[i])) return false;
    if (stem[6] != '_') return false;
    for (int i = 7; i < 15; i++) if (!isxdigit((unsigned char)stem[i])) return false;
    *serial = stem.substr(0, 6);
    std::string t = stem.substr(7, 8);
    std::transform(t.begin(), t.end(), t.begin(),
                   [](unsigned char c) { return (char)std::toupper(c); });
    *titleid = t;
    return true;
}

// 0x4D530064 -> "MS-100". The hex title id *is* the serial: two ASCII bytes of
// publisher then a 16-bit game number. This is why no external database is
// needed to identify a running game.
bool SerialFromTitleId(uint32_t tid, std::string *out)
{
    uint8_t a = (tid >> 24) & 0xFF, b = (tid >> 16) & 0xFF;
    if (a < 0x20 || a >= 0x7F || b < 0x20 || b >= 0x7F) return false;
    char buf[16];
    snprintf(buf, sizeof(buf), "%c%c-%03u", (char)a, (char)b, tid & 0xFFFF);
    *out = buf;
    return true;
}


// ---------------------------------------------------------------------------
// Writer
// ---------------------------------------------------------------------------
// Inverse of ParseCheatText. Ported from cheatfiles.render_cheat_text().
//
// `kind` is never written: the folder the file sits in already says whether it
// holds cheats or patches, and nothing reads it back.
//
// Groups are emitted as code-less [Path] blocks interleaved with cheats in
// depth-first order, so the reader rebuilds the tree by replaying the file top
// to bottom. Emitting all groups up front instead would reorder siblings on
// reload.

// Inverse of SplitPath: escape literal backslashes, join with `\`.
static std::string JoinPath(const std::vector<std::string> &parts)
{
    std::string out;
    for (size_t i = 0; i < parts.size(); i++) {
        if (i) out += '\\';
        for (char c : parts[i]) {
            if (c == '\\') out += '\\';
            out += c;
        }
    }
    return out;
}

static void EmitNodes(const NodeList &nodes, std::vector<std::string> path,
                      std::string *out)
{
    for (const auto &n : nodes) {
        path.push_back(n->name);
        std::string p = JoinPath(path);
        if (n->is_group) {
            // Declared even when it has children: the declaration is what
            // fixes where the group sits among its siblings. Without it the
            // group would spring into being at its first cheat instead.
            *out += "[" + p + "]\n";
            if (!n->expanded) *out += "collapsed=1\n";
            *out += "\n";
            EmitNodes(n->children, path, out);
        } else {
            *out += "[" + p + "]\n";
            if (!n->author.empty()) *out += "author=" + n->author + "\n";
            // desc is stored joined with spaces by the parser, but a
            // hand-written file may have had several lines; split on newlines
            // so a round trip does not merge them into one key.
            size_t start = 0;
            while (start <= n->desc.size() && !n->desc.empty()) {
                size_t nl = n->desc.find('\n', start);
                std::string ln = n->desc.substr(
                    start, nl == std::string::npos ? std::string::npos : nl - start);
                ln = Strip(ln);
                if (!ln.empty()) *out += "desc=" + ln + "\n";
                if (nl == std::string::npos) break;
                start = nl + 1;
            }
            // Always written, never inferred. Off is the default, so an
            // omitted key and enabled=0 mean the same thing -- but being
            // explicit is what makes a hand-edited file obvious.
            *out += std::string("enabled=") + (n->enabled ? "1" : "0") + "\n";
            char buf[24];
            for (const auto &c : n->codes) {
                snprintf(buf, sizeof(buf), "%08X %08X\n", c.cmd, c.val);
                *out += buf;
            }
            *out += "\n";
        }
        path.pop_back();
    }
}

std::string RenderCheatText(const std::string &title,
                            const std::string &serial,
                            const std::string &titleid,
                            const NodeList &tree,
                            const std::string &stem)
{
    std::string out = "gametitle=" + title + "\n";

    // serial/titleid are written only when the filename stem cannot supply
    // them, which in practice means only the handful of games with no known
    // title id -- and those have neither to write.
    std::string have_serial, have_titleid;
    SplitStem(stem, &have_serial, &have_titleid);
    if (!serial.empty() && Lower(serial) != Lower(have_serial))
        out += "serial=" + serial + "\n";
    if (!titleid.empty() && Lower(titleid) != Lower(have_titleid))
        out += "titleid=" + titleid + "\n";
    out += "\n";

    EmitNodes(tree, {}, &out);

    // rstrip, then exactly one trailing newline.
    size_t e = out.find_last_not_of(" \t\r\n");
    out = (e == std::string::npos) ? "" : out.substr(0, e + 1);
    out += "\n";
    return out;
}

} // namespace xcheat
