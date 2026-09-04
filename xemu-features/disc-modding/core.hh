// SPDX-License-Identifier: GPL-2.0-or-later
// xemu custom fork - XDVDFS browser / virtual file overlay core
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace xemu_disc_modding {

constexpr uint64_t kSectorSize = 2048;
constexpr size_t kNoIndex = static_cast<size_t>(-1);

struct Entry {
    size_t index = kNoIndex;
    size_t parent = kNoIndex;
    std::string name;
    std::string raw_name;
    std::string host_name;
    std::string path;
    std::string path_key;
    uint8_t attributes = 0;
    bool directory = false;
    uint32_t start_sector = 0;
    uint32_t size = 0;
    uint64_t dirent_offset = UINT64_MAX;
    std::vector<size_t> children;

    bool override_present = false;
    bool override_active = false;
    uint64_t override_size = 0;
    uint32_t virtual_sector = 0;
    std::filesystem::path override_path;
};

struct Snapshot {
    bool valid = false;
    bool overlay_enabled = false;
    std::string source_path;
    std::string effective_mod_root;
    std::string parse_error;
    std::string warning;
    uint64_t source_size = 0;
    uint64_t media_generation = 0;
    uint64_t original_sectors = 0;
    uint64_t virtual_sectors = 0;
    uint32_t volume_base_sector = 0;
    uint32_t root_sector = 0;
    uint32_t root_size = 0;
    uint32_t title_id = 0;
    std::string title_name;
    size_t active_overrides = 0;
    size_t unmatched_override_files = 0;
    size_t rejected_override_files = 0;
    size_t ambiguous_disc_paths = 0;
    size_t ambiguous_override_paths = 0;
    std::vector<std::string> overlay_messages;
    std::vector<Entry> entries;
};

enum class CollisionPolicy {
    Skip,
    Overwrite,
    Rename,
};

struct ExtractProgress {
    bool active = false;
    bool cancel_requested = false;
    bool completed = false;
    bool cancelled = false;
    bool success = false;
    uint64_t bytes_done = 0;
    uint64_t bytes_total = 0;
    size_t files_done = 0;
    size_t files_total = 0;
    std::string current_path;
    std::string error;
    std::string destination;
};

/* Immutable state used by the browser and extraction jobs. */
std::shared_ptr<const Snapshot> GetSnapshot();

/* Settings / lifecycle. */
bool GetOverlayEnabled();
void SetOverlayEnabled(bool enabled);
std::string GetConfiguredModBase();
std::string GetDefaultModBase();
void SetConfiguredModBase(const std::string &path);
void Reload();
void SyncDiscPathFromFrontend(const char *path);

/* Safe path helpers used by UI actions. */
std::filesystem::path OverridePathForEntry(const Snapshot &snapshot,
                                           size_t entry_index);
bool EnsureOverrideParent(const Snapshot &snapshot, size_t entry_index,
                          std::string *error);

/* Background extraction. */
bool StartExtraction(const std::shared_ptr<const Snapshot> &snapshot,
                     size_t entry_index, bool entire_disc,
                     const std::filesystem::path &destination,
                     CollisionPolicy policy, bool to_override_root,
                     std::string *error);
ExtractProgress GetExtractionProgress();
void CancelExtraction();
std::string GetRuntimeOverlayError();

/* Validation helpers intentionally kept pure/testable. */
bool NormalizeXboxPath(const std::string &input, std::string *normalized,
                       std::string *error);
bool PathIsWithin(const std::filesystem::path &root,
                  const std::filesystem::path &candidate);

} // namespace xemu_disc_modding
