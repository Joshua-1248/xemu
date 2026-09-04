// SPDX-License-Identifier: GPL-2.0-or-later
// xemu custom fork - XDVDFS browser / per-title virtual file overlay core
#include "xemu-features/disc-modding/core.hh"
#include "xemu-features/disc-modding/disc-overlay.h"
#include "xemu-features/disc-modding/xdvdfs.hh"

#include "ui/xemu-settings.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace xemu_disc_modding {
namespace {

constexpr size_t kExtractChunk = 1024 * 1024;
constexpr size_t kMaxRetiredStates = 32;

static bool SymlinkFreePath(const fs::path &root, const fs::path &candidate,
                            bool include_leaf, std::string *error);

struct HostReader {
    HostReader(fs::path p, fs::path trusted_root)
        : path(std::move(p)), root(std::move(trusted_root)) {}

    bool Read(uint64_t offset, void *dst, size_t len, std::string *error)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!stream.is_open()) {
            if (!PathIsWithin(root, path) ||
                !SymlinkFreePath(root, path, true, error)) {
                if (error && error->empty()) *error = "override escaped trusted mod root";
                return false;
            }
            std::error_code ec;
            const fs::file_status st = fs::symlink_status(path, ec);
            if (ec || fs::is_symlink(st) || !fs::is_regular_file(st)) {
                if (error) *error = "override is no longer a regular file: " + path.u8string();
                return false;
            }
            stream.open(path, std::ios::binary);
            if (!stream) {
                if (error) *error = "cannot open override: " + path.u8string();
                return false;
            }
        }
        if (offset > uint64_t(std::numeric_limits<std::streamoff>::max())) {
            if (error) *error = "override read offset is too large";
            return false;
        }
        stream.clear();
        stream.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
        if (!stream) {
            if (error) *error = "cannot seek override: " + path.u8string();
            return false;
        }
        stream.read(static_cast<char *>(dst), static_cast<std::streamsize>(len));
        if (!stream.good() && !(stream.eof() && size_t(stream.gcount()) == len)) {
            if (error) *error = "short/failed override read: " + path.u8string();
            return false;
        }
        return true;
    }

    fs::path path;
    fs::path root;
    std::mutex mutex;
    std::ifstream stream;
};

struct Patch {
    uint16_t offset_in_sector = 0;
    std::array<uint8_t, 4> bytes{};
};

struct VirtualFile {
    uint64_t start_sector = 0;
    uint64_t sector_count = 0;
    uint64_t size = 0;
    std::string path_key;
    std::shared_ptr<HostReader> reader;
};

struct OverlayState {
    std::shared_ptr<Snapshot> view;
    std::unordered_map<uint64_t, std::vector<Patch>> patches_by_sector;
    std::vector<VirtualFile> virtual_files;
};

std::mutex g_control_mutex;
std::shared_ptr<const OverlayState> g_state;
std::vector<std::shared_ptr<const OverlayState>> g_retired_states;
std::atomic<bool> g_active{false};
std::atomic<bool> g_virtual_available{false};
std::atomic<uint64_t> g_virtual_sector_limit{0};
uint64_t g_next_virtual_sector = 0; // guarded by g_control_mutex
std::string g_disc_path;
bool g_settings_loaded = false;
bool g_overlay_enabled = true;
std::string g_configured_mod_base;
std::mutex g_runtime_error_mutex;
std::string g_runtime_error;

static std::shared_ptr<const OverlayState> LoadState()
{
    return std::atomic_load_explicit(&g_state, std::memory_order_acquire);
}

static void StoreState(std::shared_ptr<const OverlayState> state)
{
    std::atomic_store_explicit(&g_state, std::move(state),
                               std::memory_order_release);
}

static void SetRuntimeError(const std::string &error)
{
    std::lock_guard<std::mutex> lock(g_runtime_error_mutex);
    g_runtime_error = error;
}

static std::string TitleIdString(uint32_t title_id)
{
    std::ostringstream os;
    os << std::uppercase << std::hex << std::setw(8) << std::setfill('0')
       << title_id;
    return os.str();
}

static fs::path SettingsPath()
{
    const char *base = xemu_settings_get_base_path();
    fs::path p = base && base[0] ? fs::u8path(base) : fs::path(".");
    return p / "disc-modding" / "settings.txt";
}

static fs::path DefaultModBasePath()
{
    const char *base = xemu_settings_get_base_path();
    fs::path p = base && base[0] ? fs::u8path(base) : fs::path(".");
    return p / "mods";
}

static void LoadSettingsLocked()
{
    if (g_settings_loaded) return;
    g_settings_loaded = true;
    std::ifstream f(SettingsPath());
    if (!f) return;
    std::string key;
    while (f >> key) {
        if (key == "enabled") {
            int v = 1;
            if (f >> v) g_overlay_enabled = v != 0;
        } else if (key == "mod_base") {
            std::string v;
            if (f >> std::quoted(v)) g_configured_mod_base = v;
        } else {
            std::string ignored;
            std::getline(f, ignored);
        }
    }
}

static bool ReadWholeFile(const fs::path &p, std::string *out)
{
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    return f.good() || f.eof();
}

static bool SaveSettingsLocked(std::string *error)
{
    const fs::path path = SettingsPath();
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) {
        if (error) *error = "cannot create disc-modding settings directory: " + ec.message();
        return false;
    }
    fs::path tmp = path;
    tmp += ".tmp";
    fs::path bak = path;
    bak += ".bak";
    std::ostringstream contents;
    contents << "enabled " << (g_overlay_enabled ? 1 : 0) << "\n";
    contents << "mod_base " << std::quoted(g_configured_mod_base) << "\n";
    const std::string expected = contents.str();
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) {
            if (error) *error = "cannot open temporary disc-modding settings";
            return false;
        }
        f.write(expected.data(), static_cast<std::streamsize>(expected.size()));
        f.flush();
        if (!f) {
            if (error) *error = "cannot write temporary disc-modding settings";
            return false;
        }
    }
    std::string readback;
    if (!ReadWholeFile(tmp, &readback) || readback != expected) {
        fs::remove(tmp, ec);
        if (error) *error = "disc-modding settings readback verification failed";
        return false;
    }
    if (fs::exists(path, ec) && !ec) {
        fs::copy_file(path, bak, fs::copy_options::overwrite_existing, ec);
        ec.clear(); // Backup is best effort; the verified commit remains authoritative.
    }
    fs::rename(tmp, path, ec);
    if (ec) {
        fs::remove(path, ec);
        ec.clear();
        fs::rename(tmp, path, ec);
    }
    if (ec) {
        if (error) *error = "cannot commit disc-modding settings: " + ec.message();
        return false;
    }
    readback.clear();
    if (!ReadWholeFile(path, &readback) || readback != expected) {
        if (error) *error = "disc-modding settings final verification failed";
        return false;
    }
    return true;
}

static fs::path EffectiveRoot(const Snapshot &snapshot)
{
    fs::path base = g_configured_mod_base.empty()
                        ? DefaultModBasePath()
                        : fs::u8path(g_configured_mod_base);
    if (!snapshot.title_id) return {};
    return base / TitleIdString(snapshot.title_id) / "disc";
}

static std::vector<std::string> HostComponents(const Snapshot &snapshot,
                                               size_t index)
{
    std::vector<std::string> parts;
    while (index != kNoIndex && index != 0 && index < snapshot.entries.size()) {
        parts.push_back(snapshot.entries[index].host_name);
        index = snapshot.entries[index].parent;
    }
    std::reverse(parts.begin(), parts.end());
    return parts;
}

static fs::path HostRelativePath(const Snapshot &snapshot, size_t index)
{
    fs::path result;
    for (const std::string &part : HostComponents(snapshot, index)) {
        result /= part;
    }
    return result;
}

static void WriteLe32(std::array<uint8_t, 4> *out, uint32_t v)
{
    (*out)[0] = uint8_t(v);
    (*out)[1] = uint8_t(v >> 8);
    (*out)[2] = uint8_t(v >> 16);
    (*out)[3] = uint8_t(v >> 24);
}

static void AddPatch(OverlayState *state, uint64_t absolute_offset, uint32_t v)
{
    Patch p;
    p.offset_in_sector = uint16_t(absolute_offset % kSectorSize);
    WriteLe32(&p.bytes, v);
    state->patches_by_sector[absolute_offset / kSectorSize].push_back(p);
}

static bool SymlinkFreePath(const fs::path &root, const fs::path &candidate,
                            bool include_leaf, std::string *error)
{
    std::error_code ec;
    fs::path rel = candidate.lexically_relative(root);
    if (rel.empty() && candidate != root) {
        if (error) *error = "destination escapes selected root";
        return false;
    }
    fs::path cur = root;
    size_t i = 0, count = std::distance(rel.begin(), rel.end());
    for (const fs::path &part : rel) {
        ++i;
        cur /= part;
        if (!include_leaf && i == count) break;
        fs::file_status st = fs::symlink_status(cur, ec);
        if (ec) {
            if (ec == std::errc::no_such_file_or_directory) {
                ec.clear();
                continue; // nonexistent component is safe and will be created
            }
            if (error) {
                *error = "cannot inspect path for symlinks: " + cur.u8string() +
                         ": " + ec.message();
            }
            return false;
        }
        if (fs::is_symlink(st)) {
            if (error) *error = "refusing to follow symlink: " + cur.string();
            return false;
        }
    }
    return true;
}

static bool NormalizeHostMirrorRelativePath(const fs::path &relative,
                                            std::string *normalized,
                                            std::string *error)
{
    if (normalized) normalized->clear();
    if (error) error->clear();
    if (relative.empty() || relative.is_absolute()) {
        if (error) *error = "host mirror path is empty or absolute";
        return false;
    }
    std::string out;
    for (const fs::path &part_path : relative) {
        std::string part = part_path.u8string();
        if (part.empty() || part == "." || part == "..") {
            if (error) *error = "dot/empty host path component is rejected";
            return false;
        }
        for (unsigned char c : part) {
            if (c < 0x20 || c == 0x7f || c == '/' || c == '\\' || c == ':' ||
                c == '*' || c == '?' || c == '"' || c == '<' || c == '>' ||
                c == '|') {
                if (error) *error = "host mirror component contains an unencoded unsafe character";
                return false;
            }
        }
        if (part.back() == ' ' || part.back() == '.') {
            if (error) *error = "host mirror component has unsafe trailing space/dot";
            return false;
        }
        if (!out.empty()) out.push_back('/');
        for (unsigned char c : part) {
            out.push_back(c >= 'A' && c <= 'Z' ? char(c + ('a' - 'A')) : char(c));
        }
    }
    if (out.empty()) {
        if (error) *error = "host mirror path has no components";
        return false;
    }
    if (normalized) *normalized = std::move(out);
    return true;
}

static std::shared_ptr<OverlayState> BuildStateLocked(const std::string &path)
{
    auto state = std::make_shared<OverlayState>();
    state->view = std::make_shared<Snapshot>();
    Snapshot &snapshot = *state->view;
    snapshot.overlay_enabled = g_overlay_enabled;

    if (path.empty()) {
        snapshot.source_path.clear();
        snapshot.parse_error = "No disc loaded.";
        return state;
    }

    std::string parse_error;
    if (!ParseXdvdfsImage(fs::u8path(path), &snapshot, &parse_error)) {
        snapshot.parse_error = parse_error;
        snapshot.overlay_enabled = g_overlay_enabled;
        return state;
    }
    snapshot.overlay_enabled = g_overlay_enabled;

    fs::path root = EffectiveRoot(snapshot);
    snapshot.effective_mod_root = root.u8string();
    if (!snapshot.title_id) {
        snapshot.warning = "default.xbe title ID could not be read; per-title overrides are disabled for safety.";
        return state;
    }

    std::unordered_map<std::string, size_t> disc_by_key;
    std::unordered_set<std::string> ambiguous_disc;
    for (size_t i = 1; i < snapshot.entries.size(); ++i) {
        Entry &e = snapshot.entries[i];
        if (e.directory) continue;
        auto [it, inserted] = disc_by_key.emplace(e.path_key, i);
        if (!inserted) ambiguous_disc.insert(e.path_key);
    }
    for (const auto &key : ambiguous_disc) disc_by_key.erase(key);

    std::error_code ec;
    if (root.empty() || !fs::exists(root, ec) || ec) {
        return state;
    }
    if (!fs::is_directory(root, ec) || ec) {
        snapshot.warning = "Configured per-title mod root is not a directory.";
        return state;
    }

    std::map<std::string, fs::path> override_by_key;
    std::unordered_set<std::string> ambiguous_override;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    if (ec) {
        snapshot.warning = "Cannot enumerate per-title mod root: " + ec.message();
        return state;
    }
    for (; it != end; it.increment(ec)) {
        if (ec) {
            snapshot.overlay_messages.push_back("Directory scan warning: " + ec.message());
            ec.clear();
            continue;
        }
        fs::file_status lst = it->symlink_status(ec);
        if (ec) {
            ++snapshot.rejected_override_files;
            ec.clear();
            continue;
        }
        if (fs::is_symlink(lst)) {
            if (fs::is_directory(lst)) it.disable_recursion_pending();
            ++snapshot.rejected_override_files;
            snapshot.overlay_messages.push_back("Rejected symlink override: " + it->path().string());
            continue;
        }
        if (fs::is_directory(lst)) continue;
        if (!fs::is_regular_file(lst)) {
            ++snapshot.rejected_override_files;
            continue;
        }
        if (!PathIsWithin(root, it->path())) {
            ++snapshot.rejected_override_files;
            snapshot.overlay_messages.push_back("Rejected path outside mod root: " + it->path().string());
            continue;
        }
        fs::path rel = it->path().lexically_relative(root);
        std::string key, err;
        if (!NormalizeHostMirrorRelativePath(rel, &key, &err)) {
            ++snapshot.rejected_override_files;
            snapshot.overlay_messages.push_back("Rejected override path " + rel.u8string() + ": " + err);
            continue;
        }
        auto [oit, inserted] = override_by_key.emplace(key, it->path());
        if (!inserted) ambiguous_override.insert(key);
    }
    for (const std::string &key : ambiguous_override) {
        override_by_key.erase(key);
        ++snapshot.ambiguous_override_paths;
        snapshot.overlay_messages.push_back("Ambiguous case-colliding override ignored: " + key);
    }

    struct Match { std::string key; size_t index; fs::path path; uint64_t size; };
    std::vector<Match> matches;
    for (const auto &kv : override_by_key) {
        auto dit = disc_by_key.find(kv.first);
        if (dit == disc_by_key.end()) {
            ++snapshot.unmatched_override_files;
            snapshot.overlay_messages.push_back("No disc file matches override: " + kv.first);
            continue;
        }
        Entry &e = snapshot.entries[dit->second];
        e.override_present = true;
        e.override_path = kv.second;
        uint64_t size = fs::file_size(kv.second, ec);
        if (ec) {
            ++snapshot.rejected_override_files;
            snapshot.overlay_messages.push_back("Cannot stat override: " + kv.second.string());
            ec.clear();
            continue;
        }
        e.override_size = size;
        if (size > UINT32_MAX) {
            ++snapshot.rejected_override_files;
            snapshot.overlay_messages.push_back("Override exceeds XDVDFS 4 GiB file-size limit: " + kv.first);
            continue;
        }
        matches.push_back({kv.first, dit->second, kv.second, size});
    }

    if (!g_overlay_enabled || matches.empty()) return state;

    // Deterministic virtual sectors: sorted normalized path order, after the
    // real image. This never mutates or aliases original XISO sectors.
    uint64_t cursor = std::max(snapshot.original_sectors, g_next_virtual_sector);
    for (const Match &m : matches) {
        Entry &e = snapshot.entries[m.index];
        if (e.dirent_offset == UINT64_MAX) continue;
        const uint64_t sectors = (m.size + kSectorSize - 1) / kSectorSize;
        if (cursor < snapshot.volume_base_sector ||
            cursor - snapshot.volume_base_sector > UINT32_MAX ||
            sectors > UINT32_MAX || cursor > UINT32_MAX ||
            sectors > UINT32_MAX - cursor) {
            ++snapshot.rejected_override_files;
            snapshot.overlay_messages.push_back("Virtual extent exceeds XDVDFS 32-bit sector limits: " + m.key);
            continue;
        }
        e.override_active = true;
        e.override_size = m.size;
        e.virtual_sector = uint32_t(cursor);
        ++snapshot.active_overrides;

        if (sectors) {
            VirtualFile vf;
            vf.start_sector = cursor;
            vf.sector_count = sectors;
            vf.size = m.size;
            vf.path_key = m.key;
            vf.reader = std::make_shared<HostReader>(m.path, root);
            state->virtual_files.push_back(std::move(vf));
            const uint32_t relative_sector = uint32_t(cursor - snapshot.volume_base_sector);
            AddPatch(state.get(), e.dirent_offset + 4, relative_sector);
            cursor += sectors;
        }
        // For a zero-byte replacement, preserving the old start sector is
        // harmless because XDVDFS will not issue data reads for size 0.
        AddPatch(state.get(), e.dirent_offset + 8, uint32_t(m.size));
    }
    snapshot.virtual_sectors = std::max(snapshot.original_sectors, cursor);
    g_next_virtual_sector = snapshot.virtual_sectors;
    std::sort(state->virtual_files.begin(), state->virtual_files.end(),
              [](const VirtualFile &a, const VirtualFile &b) {
                  return a.start_sector < b.start_sector;
              });
    return state;
}

static void PublishRebuildLocked(bool disc_changed)
{
    LoadSettingsLocked();
    std::shared_ptr<const OverlayState> old = LoadState();
    if (disc_changed) {
        g_retired_states.clear();
        g_next_virtual_sector = 0;
    } else if (old && old->view && old->view->valid && !old->virtual_files.empty()) {
        g_retired_states.push_back(old);
        if (g_retired_states.size() > kMaxRetiredStates) {
            g_retired_states.erase(g_retired_states.begin());
        }
    }
    auto fresh = BuildStateLocked(g_disc_path);
    const bool active = fresh && fresh->view && fresh->view->valid &&
                        fresh->view->overlay_enabled &&
                        fresh->view->active_overrides != 0;
    // Capacity is derived from the current generation only. Retired mappings
    // are retained solely to complete reads that were already queued before a
    // reload; they must not expose stale LBAs to newly issued guest commands.
    uint64_t virtual_limit = 0;
    bool virtual_available = active && !fresh->virtual_files.empty();
    if (virtual_available) virtual_limit = fresh->view->virtual_sectors;
    for (const auto &retired : g_retired_states) {
        if (retired && !retired->virtual_files.empty()) {
            virtual_available = true;
        }
    }
    StoreState(std::move(fresh));
    g_active.store(active, std::memory_order_release);
    g_virtual_sector_limit.store(virtual_limit, std::memory_order_release);
    g_virtual_available.store(virtual_available, std::memory_order_release);
    SetRuntimeError("");
}

static const VirtualFile *FindVirtualFile(const OverlayState &state,
                                          uint64_t sector)
{
    auto it = std::upper_bound(state.virtual_files.begin(), state.virtual_files.end(),
                               sector, [](uint64_t v, const VirtualFile &f) {
                                   return v < f.start_sector;
                               });
    if (it == state.virtual_files.begin()) return nullptr;
    --it;
    if (sector < it->start_sector || sector - it->start_sector >= it->sector_count) {
        return nullptr;
    }
    return &*it;
}

static bool StateCoversVirtualRange(const OverlayState &state, uint64_t lba,
                                    uint32_t count)
{
    if (!count) return true;
    if (lba > UINT64_MAX - count) return false;
    uint64_t cur = lba;
    const uint64_t end = lba + count;
    while (cur < end) {
        const VirtualFile *f = FindVirtualFile(state, cur);
        if (!f) return false;
        uint64_t f_end = f->start_sector + f->sector_count;
        cur = std::min(end, f_end);
    }
    return true;
}

static bool ReadVirtualFromState(const OverlayState &state, uint64_t lba,
                                 uint32_t count, uint8_t *buffer,
                                 std::string *error)
{
    if (!StateCoversVirtualRange(state, lba, count)) return false;
    const uint64_t total_bytes = uint64_t(count) * kSectorSize;
    std::fill(buffer, buffer + total_bytes, uint8_t(0));
    uint64_t cur = lba;
    uint64_t out_off = 0;
    const uint64_t end = lba + count;
    while (cur < end) {
        const VirtualFile *f = FindVirtualFile(state, cur);
        if (!f) return false;
        const uint64_t f_end = f->start_sector + f->sector_count;
        const uint64_t take_sectors = std::min(end, f_end) - cur;
        const uint64_t file_off = (cur - f->start_sector) * kSectorSize;
        const uint64_t requested = take_sectors * kSectorSize;
        const uint64_t readable = file_off < f->size
                                      ? std::min(requested, f->size - file_off)
                                      : 0;
        if (readable && !f->reader->Read(file_off, buffer + out_off,
                                         size_t(readable), error)) {
            std::fill(buffer, buffer + total_bytes, uint8_t(0));
            return true; // handled fail-closed: caller sees only zero bytes
        }
        cur += take_sectors;
        out_off += requested;
    }
    return true;
}

class ExtractionManager {
public:
    ~ExtractionManager()
    {
        cancel.store(true, std::memory_order_release);
        if (worker.joinable()) worker.join();
    }

    bool Start(std::shared_ptr<const Snapshot> snapshot, size_t entry_index,
               bool entire_disc, fs::path destination, CollisionPolicy policy,
               bool to_override_root, std::string *error)
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (progress.active) {
            if (error) *error = "an extraction is already running";
            return false;
        }
        if (worker.joinable()) worker.join();
        if (!snapshot || !snapshot->valid || snapshot->entries.empty()) {
            if (error) *error = "no valid disc filesystem snapshot";
            return false;
        }
        if (!entire_disc && entry_index >= snapshot->entries.size()) {
            if (error) *error = "invalid filesystem entry";
            return false;
        }
        if (to_override_root && snapshot->effective_mod_root.empty()) {
            if (error) *error = "per-title override root is unavailable";
            return false;
        }
        cancel.store(false, std::memory_order_release);
        progress = ExtractProgress{};
        progress.active = true;
        progress.destination = (to_override_root
                                    ? snapshot->effective_mod_root
                                    : destination.string());
        worker = std::thread([this, snapshot = std::move(snapshot), entry_index,
                              entire_disc, destination = std::move(destination),
                              policy, to_override_root]() mutable {
            Run(snapshot, entry_index, entire_disc, destination, policy,
                to_override_root);
        });
        return true;
    }

    ExtractProgress Get()
    {
        std::lock_guard<std::mutex> lock(mutex);
        ExtractProgress out = progress;
        out.cancel_requested = cancel.load(std::memory_order_acquire);
        return out;
    }

    void Cancel() { cancel.store(true, std::memory_order_release); }

private:
    struct JobEntry {
        size_t index;
        fs::path relative;
        bool directory;
    };

    static fs::path RenameCandidate(const fs::path &p, unsigned n)
    {
        fs::path parent = p.parent_path();
        std::string stem = p.stem().u8string();
        std::string ext = p.extension().u8string();
        return parent / fs::u8path(stem + " (" + std::to_string(n) + ")" + ext);
    }

    static bool StatusNoFollow(const fs::path &p, fs::file_status *status,
                               std::string *error)
    {
        std::error_code ec;
        *status = fs::symlink_status(p, ec);
        if (ec == std::errc::no_such_file_or_directory) {
            ec.clear();
            *status = fs::file_status(fs::file_type::not_found);
            return true;
        }
        if (ec) {
            if (error) *error = "cannot inspect destination path " + p.u8string() +
                                ": " + ec.message();
            return false;
        }
        return true;
    }

    static bool UniqueSibling(const fs::path &target, const char *tag,
                              fs::path *result, std::string *error)
    {
        static std::atomic<uint64_t> nonce{1};
        for (unsigned attempt = 0; attempt < 1024; ++attempt) {
            const uint64_t n = nonce.fetch_add(1, std::memory_order_relaxed);
            fs::path candidate = target;
            candidate += std::string(tag) + std::to_string(n);
            fs::file_status st;
            if (!StatusNoFollow(candidate, &st, error)) return false;
            if (!fs::exists(st)) {
                *result = std::move(candidate);
                return true;
            }
        }
        if (error) *error = "cannot allocate a unique temporary extraction filename";
        return false;
    }

    static bool CommitTemporary(const fs::path &temporary, const fs::path &target,
                                bool replace_existing, std::string *error)
    {
        fs::file_status now;
        if (!StatusNoFollow(target, &now, error)) return false;
        const bool exists_now = fs::exists(now);
        if (exists_now && (fs::is_symlink(now) || !fs::is_regular_file(now))) {
            if (error) *error = "refusing to replace non-regular destination: " +
                                target.u8string();
            return false;
        }
        if (!replace_existing && exists_now) {
            if (error) *error = "destination appeared during extraction: " + target.u8string();
            return false;
        }
        if (replace_existing && !exists_now) {
            if (error) *error = "destination changed/disappeared during extraction: " +
                                target.u8string();
            return false;
        }

        std::error_code ec;
        if (!replace_existing) {
            fs::rename(temporary, target, ec);
            if (ec) {
                if (error) *error = "cannot commit extracted file: " + ec.message();
                return false;
            }
            return true;
        }

        // Keep the previous destination intact until the new file has been
        // completely written, flushed and size-verified. This is portable to
        // hosts where rename() does not replace an existing file.
        fs::path backup;
        if (!UniqueSibling(target, ".xemu-old-", &backup, error)) return false;
        fs::rename(target, backup, ec);
        if (ec) {
            if (error) *error = "cannot preserve previous destination before overwrite: " + ec.message();
            return false;
        }
        fs::rename(temporary, target, ec);
        if (ec) {
            const std::string commit_error = ec.message();
            std::error_code restore_ec;
            fs::rename(backup, target, restore_ec);
            if (error) {
                *error = "cannot commit extracted overwrite: " + commit_error;
                if (restore_ec) *error += "; restoring previous file also failed: " + restore_ec.message();
            }
            return false;
        }
        fs::remove(backup, ec);
        if (ec) {
            if (error) *error = "overwrite committed but old-file cleanup failed: " + ec.message();
            return false;
        }
        return true;
    }

    void SetCurrent(const std::string &s)
    {
        std::lock_guard<std::mutex> lock(mutex);
        progress.current_path = s;
    }

    void AddBytes(uint64_t n)
    {
        std::lock_guard<std::mutex> lock(mutex);
        progress.bytes_done += n;
    }

    void FileDone()
    {
        std::lock_guard<std::mutex> lock(mutex);
        ++progress.files_done;
    }

    void Finish(bool success, bool cancelled, const std::string &error)
    {
        std::lock_guard<std::mutex> lock(mutex);
        progress.active = false;
        progress.completed = true;
        progress.cancelled = cancelled;
        progress.success = success;
        progress.error = error;
        progress.current_path.clear();
    }

    static void GatherSubtree(const Snapshot &s, size_t root,
                              const fs::path &prefix,
                              std::vector<JobEntry> *out)
    {
        const Entry &e = s.entries[root];
        fs::path here = prefix / e.host_name;
        out->push_back({root, here, e.directory});
        if (e.directory) {
            for (size_t child : e.children) GatherSubtree(s, child, here, out);
        }
    }

    void Run(const std::shared_ptr<const Snapshot> &s, size_t entry_index,
             bool entire_disc, fs::path destination, CollisionPolicy policy,
             bool to_override_root)
    {
        std::vector<JobEntry> jobs;
        if (entire_disc) {
            for (size_t child : s->entries[0].children) {
                GatherSubtree(*s, child, fs::path(), &jobs);
            }
        } else if (to_override_root) {
            // Mirror the exact on-disc hierarchy under the per-title mod root.
            const Entry &e = s->entries[entry_index];
            jobs.push_back({entry_index, HostRelativePath(*s, entry_index), e.directory});
            if (e.directory) {
                // Template-copy of a directory includes its complete subtree.
                jobs.clear();
                const fs::path full = HostRelativePath(*s, entry_index);
                fs::path parent = full.parent_path();
                GatherSubtree(*s, entry_index, parent, &jobs);
            }
        } else {
            GatherSubtree(*s, entry_index, fs::path(), &jobs);
        }

        uint64_t bytes_total = 0;
        size_t files_total = 0;
        for (const JobEntry &j : jobs) {
            if (!j.directory) {
                if (bytes_total > UINT64_MAX - s->entries[j.index].size) {
                    Finish(false, false, "extraction byte count overflow");
                    return;
                }
                bytes_total += s->entries[j.index].size;
                ++files_total;
            }
        }
        {
            std::lock_guard<std::mutex> lock(mutex);
            progress.bytes_total = bytes_total;
            progress.files_total = files_total;
        }

        fs::path root = to_override_root ? fs::u8path(s->effective_mod_root)
                                         : destination;
        std::error_code ec;
        fs::create_directories(root, ec);
        if (ec) {
            Finish(false, false, "cannot create extraction root: " + ec.message());
            return;
        }
        root = fs::weakly_canonical(root, ec);
        if (ec) {
            Finish(false, false, "cannot canonicalize extraction root: " + ec.message());
            return;
        }

        std::ifstream source(fs::u8path(s->source_path), std::ios::binary);
        if (!source) {
            Finish(false, false, "cannot reopen source XISO for extraction");
            return;
        }
        std::vector<char> buffer(kExtractChunk);

        for (const JobEntry &j : jobs) {
            if (cancel.load(std::memory_order_acquire)) {
                Finish(false, true, "");
                return;
            }
            const Entry &e = s->entries[j.index];
            fs::path target = (root / j.relative).lexically_normal();
            if (!PathIsWithin(root, target)) {
                Finish(false, false, "extraction target escaped destination root");
                return;
            }
            std::string safe_error;
            if (!SymlinkFreePath(root, target, true, &safe_error)) {
                Finish(false, false, safe_error);
                return;
            }
            SetCurrent(e.path);
            if (j.directory) {
                fs::create_directories(target, ec);
                if (ec) {
                    Finish(false, false, "cannot create directory " + target.string() + ": " + ec.message());
                    return;
                }
                continue;
            }

            fs::create_directories(target.parent_path(), ec);
            if (ec) {
                Finish(false, false, "cannot create destination directory: " + ec.message());
                return;
            }
            if (!SymlinkFreePath(root, target.parent_path(), true, &safe_error)) {
                Finish(false, false, safe_error);
                return;
            }

            fs::file_status existing;
            if (!StatusNoFollow(target, &existing, &safe_error)) {
                Finish(false, false, safe_error);
                return;
            }
            bool replace_existing = false;
            if (fs::exists(existing)) {
                if (fs::is_symlink(existing)) {
                    Finish(false, false, "refusing to overwrite symlink: " + target.u8string());
                    return;
                }
                if (policy == CollisionPolicy::Skip) {
                    AddBytes(e.size);
                    FileDone();
                    continue;
                }
                if (policy == CollisionPolicy::Rename) {
                    const fs::path original_target = target;
                    unsigned n = 1;
                    for (;;) {
                        target = RenameCandidate(original_target, n++);
                        if (n == 1000000) {
                            Finish(false, false, "too many destination filename collisions");
                            return;
                        }
                        fs::file_status candidate_status;
                        if (!StatusNoFollow(target, &candidate_status, &safe_error)) {
                            Finish(false, false, safe_error);
                            return;
                        }
                        if (!fs::exists(candidate_status)) break;
                    }
                    if (!PathIsWithin(root, target) ||
                        !SymlinkFreePath(root, target, true, &safe_error)) {
                        Finish(false, false, safe_error.empty()
                                                  ? "renamed extraction target escaped destination root"
                                                  : safe_error);
                        return;
                    }
                } else {
                    if (!fs::is_regular_file(existing)) {
                        Finish(false, false, "refusing to overwrite non-regular file: " + target.u8string());
                        return;
                    }
                    replace_existing = true;
                }
            }

            // Stream into a sibling temporary file. A cancellation or I/O
            // error therefore never destroys an existing destination and never
            // leaves a partial file under its final name.
            fs::path temporary;
            if (!UniqueSibling(target, ".xemu-part-", &temporary, &safe_error)) {
                Finish(false, false, safe_error);
                return;
            }
            auto discard_temp = [&]() {
                std::error_code remove_ec;
                fs::remove(temporary, remove_ec);
            };
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            if (!out) {
                discard_temp();
                Finish(false, false, "cannot create temporary extracted file: " + temporary.u8string());
                return;
            }
            uint64_t source_off = uint64_t(e.start_sector) * kSectorSize;
            uint64_t left = e.size;
            if (source_off > uint64_t(std::numeric_limits<std::streamoff>::max())) {
                out.close();
                discard_temp();
                Finish(false, false, "source offset exceeds host stream range");
                return;
            }
            source.clear();
            source.seekg(static_cast<std::streamoff>(source_off), std::ios::beg);
            if (!source) {
                out.close();
                discard_temp();
                Finish(false, false, "cannot seek source XISO");
                return;
            }
            while (left) {
                if (cancel.load(std::memory_order_acquire)) {
                    out.close();
                    discard_temp();
                    Finish(false, true, "");
                    return;
                }
                const size_t take = size_t(std::min<uint64_t>(left, buffer.size()));
                source.read(buffer.data(), static_cast<std::streamsize>(take));
                if (!source.good() && !(source.eof() && size_t(source.gcount()) == take)) {
                    out.close();
                    discard_temp();
                    Finish(false, false, "short/failed source XISO read while extracting " + e.path);
                    return;
                }
                out.write(buffer.data(), static_cast<std::streamsize>(take));
                if (!out) {
                    out.close();
                    discard_temp();
                    Finish(false, false, "failed writing temporary extracted file: " + temporary.u8string());
                    return;
                }
                left -= take;
                AddBytes(take);
            }
            out.flush();
            if (!out) {
                out.close();
                discard_temp();
                Finish(false, false, "failed flushing temporary extracted file: " + temporary.u8string());
                return;
            }
            out.close();
            if (!out) {
                discard_temp();
                Finish(false, false, "failed closing temporary extracted file: " + temporary.u8string());
                return;
            }
            const uint64_t written_size = fs::file_size(temporary, ec);
            if (ec || written_size != e.size) {
                const std::string why = ec ? ec.message() : "size verification mismatch";
                ec.clear();
                discard_temp();
                Finish(false, false, "temporary extracted file verification failed: " + why);
                return;
            }
            if (cancel.load(std::memory_order_acquire)) {
                discard_temp();
                Finish(false, true, "");
                return;
            }
            if (!CommitTemporary(temporary, target, replace_existing, &safe_error)) {
                discard_temp();
                Finish(false, false, safe_error);
                return;
            }
            FileDone();
        }
        Finish(true, false, "");
    }

    std::mutex mutex;
    std::atomic<bool> cancel{false};
    std::thread worker;
    ExtractProgress progress;
};

ExtractionManager g_extraction;

} // namespace

std::shared_ptr<const Snapshot> GetSnapshot()
{
    auto state = LoadState();
    return state ? state->view : std::shared_ptr<const Snapshot>();
}

bool GetOverlayEnabled()
{
    std::lock_guard<std::mutex> lock(g_control_mutex);
    LoadSettingsLocked();
    return g_overlay_enabled;
}

void SetOverlayEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(g_control_mutex);
    LoadSettingsLocked();
    if (g_overlay_enabled == enabled) return;
    g_overlay_enabled = enabled;
    std::string error;
    if (!SaveSettingsLocked(&error)) SetRuntimeError(error);
    PublishRebuildLocked(false);
}

std::string GetConfiguredModBase()
{
    std::lock_guard<std::mutex> lock(g_control_mutex);
    LoadSettingsLocked();
    return g_configured_mod_base;
}

std::string GetDefaultModBase()
{
    return DefaultModBasePath().u8string();
}

void SetConfiguredModBase(const std::string &path)
{
    std::lock_guard<std::mutex> lock(g_control_mutex);
    LoadSettingsLocked();
    if (g_configured_mod_base == path) return;
    g_configured_mod_base = path;
    std::string error;
    if (!SaveSettingsLocked(&error)) SetRuntimeError(error);
    PublishRebuildLocked(false);
}

void Reload()
{
    std::lock_guard<std::mutex> lock(g_control_mutex);
    PublishRebuildLocked(false);
}

void SyncDiscPathFromFrontend(const char *path)
{
    const std::string p = path ? path : "";
    std::lock_guard<std::mutex> lock(g_control_mutex);
    if (p == g_disc_path && LoadState()) return;
    const bool changed = p != g_disc_path;
    g_disc_path = p;
    PublishRebuildLocked(changed);
}

fs::path OverridePathForEntry(const Snapshot &snapshot, size_t entry_index)
{
    if (entry_index >= snapshot.entries.size() || snapshot.effective_mod_root.empty()) {
        return {};
    }
    return fs::u8path(snapshot.effective_mod_root) / HostRelativePath(snapshot, entry_index);
}

bool EnsureOverrideParent(const Snapshot &snapshot, size_t entry_index,
                          std::string *error)
{
    fs::path p = OverridePathForEntry(snapshot, entry_index);
    if (p.empty()) {
        if (error) *error = "override path is unavailable";
        return false;
    }
    fs::path root = fs::u8path(snapshot.effective_mod_root);
    fs::path parent = snapshot.entries[entry_index].directory ? p : p.parent_path();
    if (!PathIsWithin(root, parent)) {
        if (error) *error = "override path escaped per-title mod root";
        return false;
    }
    std::string safe_error;
    if (!SymlinkFreePath(root, parent, true, &safe_error)) {
        if (error) *error = safe_error;
        return false;
    }
    std::error_code ec;
    fs::create_directories(parent, ec);
    if (ec) {
        if (error) *error = "cannot create override directory: " + ec.message();
        return false;
    }
    return true;
}

bool StartExtraction(const std::shared_ptr<const Snapshot> &snapshot,
                     size_t entry_index, bool entire_disc,
                     const fs::path &destination, CollisionPolicy policy,
                     bool to_override_root, std::string *error)
{
    return g_extraction.Start(snapshot, entry_index, entire_disc, destination,
                              policy, to_override_root, error);
}

ExtractProgress GetExtractionProgress() { return g_extraction.Get(); }
void CancelExtraction() { g_extraction.Cancel(); }

std::string GetRuntimeOverlayError()
{
    std::lock_guard<std::mutex> lock(g_runtime_error_mutex);
    return g_runtime_error;
}

} // namespace xemu_disc_modding

extern "C" {

void xemu_disc_overlay_notify_disc_path(const char *path)
{
    xemu_disc_modding::SyncDiscPathFromFrontend(path);
}

uint64_t xemu_disc_overlay_total_sectors(uint64_t original_total_sectors)
{
    if (!xemu_disc_modding::g_virtual_available.load(std::memory_order_acquire)) {
        return original_total_sectors;
    }
    return std::max(original_total_sectors,
                    xemu_disc_modding::g_virtual_sector_limit.load(
                        std::memory_order_acquire));
}

bool xemu_disc_overlay_is_virtual_range(uint64_t lba, uint32_t count)
{
    if (!xemu_disc_modding::g_virtual_available.load(std::memory_order_acquire)) {
        return false;
    }
    auto state = xemu_disc_modding::LoadState();
    if (state && xemu_disc_modding::StateCoversVirtualRange(*state, lba, count)) {
        return true;
    }
    std::lock_guard<std::mutex> lock(xemu_disc_modding::g_control_mutex);
    for (auto it = xemu_disc_modding::g_retired_states.rbegin();
         it != xemu_disc_modding::g_retired_states.rend(); ++it) {
        if (*it && xemu_disc_modding::StateCoversVirtualRange(**it, lba, count)) {
            return true;
        }
    }
    return false;
}

bool xemu_disc_overlay_read_virtual(uint64_t lba, uint32_t count,
                                    uint8_t *buffer)
{
    if ((!buffer && count) ||
        !xemu_disc_modding::g_virtual_available.load(std::memory_order_acquire)) {
        return false;
    }
    std::string error;
    auto current = xemu_disc_modding::LoadState();
    if (current && xemu_disc_modding::ReadVirtualFromState(*current, lba, count,
                                                           buffer, &error)) {
        if (!error.empty()) xemu_disc_modding::SetRuntimeError(error);
        return true;
    }
    // Retired snapshots make explicit UI reload safe for already queued ATAPI
    // reads and for games that briefly keep an old extent cached.
    std::lock_guard<std::mutex> lock(xemu_disc_modding::g_control_mutex);
    for (auto it = xemu_disc_modding::g_retired_states.rbegin();
         it != xemu_disc_modding::g_retired_states.rend(); ++it) {
        error.clear();
        if (*it && xemu_disc_modding::ReadVirtualFromState(**it, lba, count,
                                                            buffer, &error)) {
            if (!error.empty()) xemu_disc_modding::SetRuntimeError(error);
            return true;
        }
    }
    return false;
}

void xemu_disc_overlay_patch_read(uint64_t lba, uint32_t count, uint8_t *buffer)
{
    if (!buffer || !count ||
        !xemu_disc_modding::g_active.load(std::memory_order_acquire)) {
        return;
    }
    auto state = xemu_disc_modding::LoadState();
    if (!state) return;
    for (uint32_t i = 0; i < count; ++i) {
        auto it = state->patches_by_sector.find(lba + i);
        if (it == state->patches_by_sector.end()) continue;
        uint8_t *sector = buffer + uint64_t(i) * xemu_disc_modding::kSectorSize;
        for (const auto &patch : it->second) {
            if (patch.offset_in_sector + patch.bytes.size() <= xemu_disc_modding::kSectorSize) {
                std::copy(patch.bytes.begin(), patch.bytes.end(),
                          sector + patch.offset_in_sector);
            }
        }
    }
}

} // extern "C"
