// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "xemu-features/disc-modding/core.hh"

#include <filesystem>
#include <string>

namespace xemu_disc_modding {

/*
 * Logical random-access medium. The XDVDFS parser deliberately consumes this
 * abstraction rather than a host pathname so raw XISO and decoded CHD media
 * share exactly the same filesystem interpretation.
 */
class LogicalDiscReader {
public:
    virtual ~LogicalDiscReader() = default;
    virtual uint64_t Size() const = 0;
    virtual bool ReadAt(uint64_t offset, void *dst, size_t len,
                        std::string *error) = 0;
};

bool ParseXdvdfsReader(LogicalDiscReader &reader, const std::string &source_name,
                       Snapshot *snapshot, std::string *error);

/* Pure host-file convenience wrapper retained for tests/tools using raw XISO. */
bool ParseXdvdfsImage(const std::filesystem::path &image, Snapshot *snapshot,
                      std::string *error);

/* Converts one raw XDVDFS path component into a portable host component. */
std::string MakeSafeHostComponent(const std::string &raw);

} // namespace xemu_disc_modding
