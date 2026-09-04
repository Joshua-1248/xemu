// SPDX-License-Identifier: GPL-2.0-or-later
// Pure XDVDFS parsing and path-safety helpers for the custom disc tools.
#include "xemu-features/disc-modding/xdvdfs.hh"

#include <algorithm>
#include <array>
#include <cctype>
#include <fstream>
#include <limits>
#include <queue>
#include <set>
#include <sstream>
#include <unordered_set>

namespace fs = std::filesystem;

namespace xemu_disc_modding {
namespace {

constexpr uint64_t kVolumeDescriptorSector = 32;
// Xbox full-disc image layouts seen in common XGD/XISO tooling.  The
// XDVDFS volume descriptor itself is still 32 sectors into the data partition.
constexpr uint64_t kGlobalDataPartitionSector = 0x0FD90000ULL / kSectorSize;
constexpr uint64_t kXgd3DataPartitionSector   = 0x02080000ULL / kSectorSize;
constexpr uint64_t kXgd1DataPartitionSector   = 0x18300000ULL / kSectorSize;
constexpr size_t kMaxDirectoryBytes = 128u * 1024u * 1024u;
constexpr size_t kMaxEntries = 1000000;
constexpr unsigned kMaxDepth = 256;
constexpr uint8_t kDirectoryAttribute = 0x10;
constexpr char kMediaMagic[] = "MICROSOFT*XBOX*MEDIA";
constexpr size_t kMediaMagicLen = sizeof(kMediaMagic) - 1;

static uint16_t ReadLe16(const uint8_t *p)
{
    return uint16_t(p[0]) | (uint16_t(p[1]) << 8);
}

static uint32_t ReadLe32(const uint8_t *p)
{
    return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
           (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}

static bool CheckedMul(uint64_t a, uint64_t b, uint64_t *out)
{
    if (a && b > std::numeric_limits<uint64_t>::max() / a) {
        return false;
    }
    *out = a * b;
    return true;
}

static bool ReadAt(LogicalDiscReader &reader, uint64_t offset, void *dst,
                   size_t len)
{
    return reader.ReadAt(offset, dst, len, nullptr);
}

class FileLogicalDiscReader final : public LogicalDiscReader {
public:
    explicit FileLogicalDiscReader(const fs::path &path, std::string *error)
    {
        std::error_code ec;
        size_ = fs::file_size(path, ec);
        if (ec) {
            if (error) *error = "cannot stat disc image: " + ec.message();
            return;
        }
        stream_.open(path, std::ios::binary);
        if (!stream_ && error) *error = "cannot open disc image";
    }

    bool Valid() const { return stream_.is_open(); }
    uint64_t Size() const override { return size_; }

    bool ReadAt(uint64_t offset, void *dst, size_t len,
                std::string *error) override
    {
        if (offset > uint64_t(std::numeric_limits<std::streamoff>::max()) ||
            offset > size_ || len > size_ - offset) {
            if (error) *error = "disc read lies outside logical image";
            return false;
        }
        stream_.clear();
        stream_.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!stream_) {
            if (error) *error = "cannot seek disc image";
            return false;
        }
        stream_.read(static_cast<char *>(dst), static_cast<std::streamsize>(len));
        if (!stream_.good() && !(stream_.eof() && size_t(stream_.gcount()) == len)) {
            if (error) *error = "short/failed disc image read";
            return false;
        }
        return true;
    }

private:
    uint64_t size_ = 0;
    std::ifstream stream_;
};

static std::string HexByte(uint8_t v)
{
    static const char kHex[] = "0123456789ABCDEF";
    std::string r;
    r += kHex[v >> 4];
    r += kHex[v & 15];
    return r;
}

static bool Utf8SequenceLength(const uint8_t *p, size_t left, size_t *n)
{
    if (!left) {
        return false;
    }
    const uint8_t c = p[0];
    if (c < 0x80) {
        *n = 1;
        return true;
    }
    size_t need = 0;
    uint32_t cp = 0;
    uint32_t min_cp = 0;
    if ((c & 0xe0) == 0xc0) {
        need = 2; cp = c & 0x1f; min_cp = 0x80;
    } else if ((c & 0xf0) == 0xe0) {
        need = 3; cp = c & 0x0f; min_cp = 0x800;
    } else if ((c & 0xf8) == 0xf0) {
        need = 4; cp = c & 0x07; min_cp = 0x10000;
    } else {
        return false;
    }
    if (left < need) {
        return false;
    }
    for (size_t i = 1; i < need; ++i) {
        if ((p[i] & 0xc0) != 0x80) {
            return false;
        }
        cp = (cp << 6) | (p[i] & 0x3f);
    }
    if (cp < min_cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) {
        return false;
    }
    *n = need;
    return true;
}

static std::string DisplayName(const std::string &raw)
{
    std::string out;
    const auto *p = reinterpret_cast<const uint8_t *>(raw.data());
    size_t i = 0;
    while (i < raw.size()) {
        size_t n = 0;
        if (Utf8SequenceLength(p + i, raw.size() - i, &n)) {
            out.append(raw, i, n);
            i += n;
        } else {
            out += "\\x";
            out += HexByte(p[i++]);
        }
    }
    return out;
}

static void AppendUtf8(std::string &out, uint32_t cp)
{
    if (cp <= 0x7f) {
        out.push_back(char(cp));
    } else if (cp <= 0x7ff) {
        out.push_back(char(0xc0 | (cp >> 6)));
        out.push_back(char(0x80 | (cp & 0x3f)));
    } else if (cp <= 0xffff) {
        out.push_back(char(0xe0 | (cp >> 12)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(char(0x80 | (cp & 0x3f)));
    } else {
        out.push_back(char(0xf0 | (cp >> 18)));
        out.push_back(char(0x80 | ((cp >> 12) & 0x3f)));
        out.push_back(char(0x80 | ((cp >> 6) & 0x3f)));
        out.push_back(char(0x80 | (cp & 0x3f)));
    }
}

static std::string DecodeTitleName(const uint8_t *p, size_t units)
{
    std::string out;
    for (size_t i = 0; i < units; ++i) {
        uint16_t w = ReadLe16(p + i * 2);
        if (!w) {
            break;
        }
        uint32_t cp = w;
        if (w >= 0xd800 && w <= 0xdbff && i + 1 < units) {
            uint16_t w2 = ReadLe16(p + (i + 1) * 2);
            if (w2 >= 0xdc00 && w2 <= 0xdfff) {
                cp = 0x10000 + ((uint32_t(w - 0xd800) << 10) | (w2 - 0xdc00));
                ++i;
            } else {
                cp = 0xfffd;
            }
        } else if (w >= 0xdc00 && w <= 0xdfff) {
            cp = 0xfffd;
        }
        AppendUtf8(out, cp);
    }
    return out;
}

static bool LooksLikeDescriptor(const std::array<uint8_t, kSectorSize> &buf)
{
    return std::equal(kMediaMagic, kMediaMagic + kMediaMagicLen, buf.data()) &&
           std::equal(kMediaMagic, kMediaMagic + kMediaMagicLen,
                      buf.data() + kSectorSize - kMediaMagicLen);
}

struct DirTask {
    uint32_t sector;
    uint32_t size;
    size_t parent;
    std::string display_path;
    std::string key_path;
    unsigned depth;
};

static bool ParseTitle(LogicalDiscReader &f, Snapshot *snapshot)
{
    size_t default_index = kNoIndex;
    for (size_t idx : snapshot->entries[0].children) {
        const Entry &e = snapshot->entries[idx];
        if (!e.directory && e.path_key == "default.xbe") {
            default_index = idx;
            break;
        }
    }
    if (default_index == kNoIndex) {
        return false;
    }

    const Entry &e = snapshot->entries[default_index];
    uint64_t base = 0;
    if (!CheckedMul(e.start_sector, kSectorSize, &base)) {
        return false;
    }
    std::array<uint8_t, 0x200> hdr{};
    const size_t initial = std::min<uint64_t>(hdr.size(), e.size);
    if (initial < 0x120 || !ReadAt(f, base, hdr.data(), initial)) {
        return false;
    }
    if (ReadLe32(hdr.data()) != 0x48454258u) { // XBEH little endian
        return false;
    }
    const uint32_t image_base = ReadLe32(hdr.data() + 0x104);
    const uint32_t headers_size = ReadLe32(hdr.data() + 0x108);
    const uint32_t cert_va = ReadLe32(hdr.data() + 0x118);
    if (cert_va < image_base) {
        return false;
    }
    const uint64_t cert_off = uint64_t(cert_va) - image_base;
    if (cert_off > e.size || e.size - cert_off < 12 || cert_off >= headers_size) {
        return false;
    }
    std::array<uint8_t, 92> cert{};
    const size_t want = std::min<uint64_t>(cert.size(), e.size - cert_off);
    if (want < 12 || !ReadAt(f, base + cert_off, cert.data(), want)) {
        return false;
    }
    const uint32_t cert_size = ReadLe32(cert.data());
    if (cert_size < 12) {
        return false;
    }
    snapshot->title_id = ReadLe32(cert.data() + 8);
    if (want >= 92) {
        snapshot->title_name = DecodeTitleName(cert.data() + 12, 40);
    }
    return true;
}

} // namespace

std::string MakeSafeHostComponent(const std::string &raw)
{
    if (raw.empty()) {
        return "_empty_";
    }
    std::string out;
    const auto *p = reinterpret_cast<const uint8_t *>(raw.data());
    size_t i = 0;
    while (i < raw.size()) {
        const uint8_t c = p[i];
        size_t utf_n = 0;
        const bool safe_ascii = c >= 0x20 && c < 0x7f && c != '/' &&
                                c != '\\' && c != ':' && c != '*' &&
                                c != '?' && c != '"' && c != '<' &&
                                c != '>' && c != '|';
        if (safe_ascii) {
            out.push_back(char(c));
            ++i;
        } else if (c >= 0x80 && Utf8SequenceLength(p + i, raw.size() - i, &utf_n)) {
            out.append(raw, i, utf_n);
            i += utf_n;
        } else {
            out += "_x";
            out += HexByte(c);
            out += "_";
            ++i;
        }
    }
    if (out == "." || out == "..") {
        out = "_" + out + "_";
    }
    while (!out.empty() && (out.back() == ' ' || out.back() == '.')) {
        const uint8_t c = uint8_t(out.back());
        out.pop_back();
        out += "_x" + HexByte(c) + "_";
    }
    if (out.empty()) {
        out = "_empty_";
    }
    // Keep extraction/override mirrors portable to Windows as well as POSIX.
    // Reserved DOS device names are forbidden even when followed by an
    // extension, so prefix them rather than silently failing extraction.
    std::string stem = out.substr(0, out.find('.'));
    for (char &ch : stem) {
        if (ch >= 'a' && ch <= 'z') ch = char(ch - ('a' - 'A'));
    }
    const bool reserved = stem == "CON" || stem == "PRN" || stem == "AUX" ||
                          stem == "NUL" ||
                          (stem.size() == 4 &&
                           ((stem.rfind("COM", 0) == 0) ||
                            (stem.rfind("LPT", 0) == 0)) &&
                           stem[3] >= '1' && stem[3] <= '9');
    if (reserved) out.insert(out.begin(), '_');
    return out;
}

bool NormalizeXboxPath(const std::string &input, std::string *normalized,
                       std::string *error)
{
    if (normalized) normalized->clear();
    if (error) error->clear();
    if (input.empty()) {
        if (error) *error = "empty path";
        return false;
    }
    if (input[0] == '/' || input[0] == '\\') {
        if (error) *error = "absolute Xbox paths are not accepted";
        return false;
    }
    std::string out;
    std::string part;
    auto flush = [&]() -> bool {
        if (part.empty()) {
            return true; // repeated separators are benign
        }
        if (part == "." || part == "..") {
            if (error) *error = "dot path components are rejected";
            return false;
        }
        if (part.find(':') != std::string::npos) {
            if (error) *error = "drive/colon path components are rejected";
            return false;
        }
        if (!out.empty()) out.push_back('/');
        for (unsigned char c : part) {
            out.push_back(c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : char(c));
        }
        part.clear();
        return true;
    };
    for (unsigned char c : input) {
        if (c == '/' || c == '\\') {
            if (!flush()) return false;
        } else if (c == 0) {
            if (error) *error = "NUL byte in path";
            return false;
        } else {
            part.push_back(char(c));
        }
    }
    if (!flush()) return false;
    if (out.empty()) {
        if (error) *error = "path has no components";
        return false;
    }
    if (normalized) *normalized = out;
    return true;
}

bool PathIsWithin(const fs::path &root, const fs::path &candidate)
{
    std::error_code ec1, ec2;
    fs::path r = fs::weakly_canonical(root, ec1);
    fs::path c = fs::weakly_canonical(candidate, ec2);
    if (ec1 || ec2) {
        r = root.lexically_normal();
        c = candidate.lexically_normal();
    }
    auto ri = r.begin(), re = r.end();
    auto ci = c.begin(), ce = c.end();
    for (; ri != re; ++ri, ++ci) {
        if (ci == ce || *ri != *ci) {
            return false;
        }
    }
    return true;
}

bool ParseXdvdfsReader(LogicalDiscReader &f, const std::string &source_name,
                       Snapshot *snapshot, std::string *error)
{
    if (!snapshot) {
        if (error) *error = "null snapshot";
        return false;
    }
    *snapshot = Snapshot{};
    snapshot->source_path = source_name;
    if (error) error->clear();

    const uint64_t file_size = f.Size();
    if (!file_size) {
        if (error) *error = "logical disc image is empty";
        return false;
    }
    snapshot->source_size = file_size;
    snapshot->original_sectors = (file_size + kSectorSize - 1) / kSectorSize;
    snapshot->virtual_sectors = snapshot->original_sectors;

    std::array<uint8_t, kSectorSize> vd{};
    uint64_t base_sector = 0;
    bool found = false;
    const uint64_t candidates[] = {
        0,
        kGlobalDataPartitionSector,
        kXgd3DataPartitionSector,
        kXgd1DataPartitionSector,
    };
    for (uint64_t candidate : candidates) {
        uint64_t vd_sector = candidate + kVolumeDescriptorSector;
        uint64_t off = 0;
        if (!CheckedMul(vd_sector, kSectorSize, &off) ||
            off > file_size || file_size - off < kSectorSize) {
            continue;
        }
        if (ReadAt(f, off, vd.data(), vd.size()) && LooksLikeDescriptor(vd)) {
            base_sector = candidate;
            found = true;
            break;
        }
    }
    if (!found) {
        if (error) {
            *error = "XDVDFS volume descriptor not found at the supported "
                     "XISO/full-disc locations";
        }
        return false;
    }

    const uint32_t root_rel_sector = ReadLe32(vd.data() + 0x14);
    const uint32_t root_size = ReadLe32(vd.data() + 0x18);
    if (root_size == 0 || root_size > kMaxDirectoryBytes) {
        if (error) *error = "invalid XDVDFS root directory size";
        return false;
    }
    const uint64_t root_abs64 = base_sector + uint64_t(root_rel_sector);
    if (root_abs64 > UINT32_MAX) {
        if (error) *error = "XDVDFS root sector exceeds 32-bit range";
        return false;
    }
    uint64_t root_off = 0;
    if (!CheckedMul(root_abs64, kSectorSize, &root_off) ||
        root_off > file_size || file_size - root_off < root_size) {
        if (error) *error = "XDVDFS root directory lies outside image";
        return false;
    }

    snapshot->volume_base_sector = uint32_t(base_sector);
    snapshot->root_sector = uint32_t(root_abs64);
    snapshot->root_size = root_size;

    Entry root;
    root.index = 0;
    root.name = "/";
    root.raw_name = "/";
    root.host_name.clear();
    root.path.clear();
    root.path_key.clear();
    root.directory = true;
    root.attributes = kDirectoryAttribute;
    root.start_sector = snapshot->root_sector;
    root.size = root_size;
    snapshot->entries.push_back(std::move(root));

    std::queue<DirTask> tasks;
    tasks.push({snapshot->root_sector, root_size, 0, "", "", 0});
    std::set<std::pair<uint32_t, uint32_t>> seen_tables;

    while (!tasks.empty()) {
        DirTask task = std::move(tasks.front());
        tasks.pop();
        if (task.depth > kMaxDepth) {
            if (error) *error = "XDVDFS directory nesting exceeds safety limit";
            return false;
        }
        if (!seen_tables.insert({task.sector, task.size}).second) {
            continue;
        }
        if (task.size == 0 || task.size > kMaxDirectoryBytes) {
            if (error) *error = "invalid XDVDFS directory table size";
            return false;
        }
        uint64_t table_off = 0;
        if (!CheckedMul(task.sector, kSectorSize, &table_off) ||
            table_off > file_size || file_size - table_off < task.size) {
            if (error) *error = "XDVDFS directory table lies outside image";
            return false;
        }
        std::vector<uint8_t> table(task.size);
        if (!ReadAt(f, table_off, table.data(), table.size())) {
            if (error) *error = "failed reading XDVDFS directory table";
            return false;
        }

        std::vector<uint32_t> stack{0};
        std::unordered_set<uint32_t> visited;
        while (!stack.empty()) {
            const uint32_t off = stack.back();
            stack.pop_back();
            if (!visited.insert(off).second) {
                continue;
            }
            if (off > table.size() || table.size() - off < 14) {
                if (error) *error = "malformed XDVDFS directory entry offset";
                return false;
            }
            const uint8_t *p = table.data() + off;
            const uint16_t left_words = ReadLe16(p + 0);

            // An empty XDVDFS subdirectory is represented by one sector of
            // 0xff padding.  extract-xiso treats a leading 0xffff table
            // offset as the empty-directory sentinel.  A tree link must never
            // target padding, so only offset zero may terminate this table.
            if (left_words == 0xffffu) {
                if (off == 0) {
                    break;
                }
                if (error) *error = "malformed XDVDFS directory tree link targets padding";
                return false;
            }

            const uint16_t right_words = ReadLe16(p + 2);
            const uint32_t rel_start = ReadLe32(p + 4);
            const uint32_t size = ReadLe32(p + 8);
            const uint8_t attr = p[12];
            const uint8_t name_len = p[13];
            if (name_len == 0 || table.size() - off - 14 < name_len) {
                if (error) *error = "malformed XDVDFS directory entry name";
                return false;
            }

            const uint32_t child_words[] = {right_words, left_words};
            for (uint16_t words : child_words) {
                if (!words) continue;
                const uint64_t child = uint64_t(words) * 4;
                if (child > UINT32_MAX || child >= table.size()) {
                    if (error) *error = "malformed XDVDFS directory tree link";
                    return false;
                }
                stack.push_back(uint32_t(child));
            }

            std::string raw(reinterpret_cast<const char *>(p + 14), name_len);
            std::string display = DisplayName(raw);
            // The normalized match key intentionally uses the same portable
            // host encoding shown by OverridePathForEntry(). This makes names
            // containing Windows-forbidden bytes (for example '?' or ':')
            // moddable on every host instead of requiring an impossible host
            // filename on Windows. ASCII case remains Xbox-insensitive.
            std::string key_component = MakeSafeHostComponent(raw);
            for (char &ch : key_component) {
                if (ch >= 'A' && ch <= 'Z') ch = char(ch + ('a' - 'A'));
            }

            const uint64_t abs_start64 = base_sector + uint64_t(rel_start);
            if (abs_start64 > UINT32_MAX) {
                if (error) *error = "XDVDFS file sector exceeds 32-bit range";
                return false;
            }
            const bool is_dir = (attr & kDirectoryAttribute) != 0;
            uint64_t data_off = 0;
            if (!CheckedMul(abs_start64, kSectorSize, &data_off) ||
                data_off > file_size || (!is_dir && uint64_t(size) > file_size - data_off) ||
                (is_dir && uint64_t(size) > file_size - data_off)) {
                if (error) *error = "XDVDFS file/directory extent lies outside image";
                return false;
            }

            Entry e;
            e.index = snapshot->entries.size();
            e.parent = task.parent;
            e.name = display;
            e.raw_name = raw;
            e.host_name = MakeSafeHostComponent(raw);
            e.path = task.display_path.empty() ? display : task.display_path + "/" + display;
            e.path_key = task.key_path.empty() ? key_component : task.key_path + "/" + key_component;
            e.attributes = attr;
            e.directory = is_dir;
            e.start_sector = uint32_t(abs_start64);
            e.size = size;
            e.dirent_offset = table_off + off;
            const size_t new_index = e.index;
            snapshot->entries.push_back(std::move(e));
            snapshot->entries[task.parent].children.push_back(new_index);
            if (snapshot->entries.size() > kMaxEntries) {
                if (error) *error = "XDVDFS entry count exceeds safety limit";
                return false;
            }
            if (is_dir && size) {
                tasks.push({uint32_t(abs_start64), size, new_index,
                            snapshot->entries[new_index].path,
                            snapshot->entries[new_index].path_key,
                            task.depth + 1});
            }
        }
    }

    for (Entry &e : snapshot->entries) {
        std::stable_sort(e.children.begin(), e.children.end(),
                         [&](size_t a, size_t b) {
            const Entry &ea = snapshot->entries[a];
            const Entry &eb = snapshot->entries[b];
            if (ea.directory != eb.directory) return ea.directory > eb.directory;
            if (ea.path_key != eb.path_key) return ea.path_key < eb.path_key;
            return ea.raw_name < eb.raw_name;
        });
    }

    std::unordered_set<std::string> path_keys;
    for (size_t i = 1; i < snapshot->entries.size(); ++i) {
        if (!path_keys.insert(snapshot->entries[i].path_key).second) {
            ++snapshot->ambiguous_disc_paths;
        }
    }

    ParseTitle(f, snapshot); // Metadata is optional; filesystem validity is not.
    snapshot->valid = true;
    return true;
}

bool ParseXdvdfsImage(const fs::path &image, Snapshot *snapshot,
                      std::string *error)
{
    std::string open_error;
    FileLogicalDiscReader reader(image, &open_error);
    if (!reader.Valid()) {
        if (error) *error = open_error;
        return false;
    }
    return ParseXdvdfsReader(reader, image.u8string(), snapshot, error);
}

} // namespace xemu_disc_modding
