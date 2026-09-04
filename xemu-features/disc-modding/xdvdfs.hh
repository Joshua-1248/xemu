// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "xemu-features/disc-modding/core.hh"

#include <filesystem>
#include <string>

namespace xemu_disc_modding {

/* Pure, host-side XDVDFS parser. No QEMU/Xemu runtime dependencies. */
bool ParseXdvdfsImage(const std::filesystem::path &image, Snapshot *snapshot,
                      std::string *error);

/* Converts one raw XDVDFS path component into a portable host component. */
std::string MakeSafeHostComponent(const std::string &raw);

} // namespace xemu_disc_modding
