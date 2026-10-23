// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A brain's files, and the facts `check` reports for one. Scripts are
// compiled and not run. A network file is hashed and not interpreted.
#pragma once

#include "check_mutator.hpp"

#include "oa/formats/oabrain/manifest.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace oa::tool {

/// One network file, hashed, in the order the manifest names networks.
struct NetworkFact {
    std::string id{};     ///< the name a script asks for
    std::string file{};   ///< the path the manifest names
    std::string sha256{}; ///< 64 lower-case hex digits of the file's bytes
    uint64_t size{};      ///< the file's size in bytes
};

/// How many scripts compiled, and the networks and tables that were read.
struct BrainChecked {
    uint32_t scripts{};                  ///< `.lua` files that compiled
    std::vector<NetworkFact> networks{}; ///< one per network whose bytes were read
    uint64_t network_bytes{};            ///< those networks' sizes added together
    std::vector<std::string> data{};     ///< table paths, in unsigned byte order
};

/// Checks the files a brain may hold.
///
/// Every file is asked of `file_role`. The entry script and every module are
/// compiled, each at most 1 MiB. Each named network is read whole, at most
/// 64 MiB, and hashed; the networks together may take 64 MiB. Each data table
/// may take 4 MiB. The icon must be a square PNG within the icon limits.
/// Nothing is run, and a network's bytes are not interpreted. Each problem is
/// appended. A file outside the layout is `<path>: outside the files a brain
/// may hold`. A network whose hash differs is `<file>: its SHA-256 is <hex>,
/// not the <hex> oabrain.yaml gives`.
///
/// @param entries the package's files
/// @param manifest the manifest `oabrain.yaml` already read
/// @param[in,out] problems one line appended for each problem
/// @param[out] checked how many scripts compiled, and the networks and tables
void check_brain_files(
    const PackageEntries& entries,
    const oa::formats::oabrain::Manifest& manifest,
    std::vector<std::string>& problems,
    BrainChecked& checked
);

/// Writes a brain's catalogue facts into an object that is already open.
///
/// Reads `oabrain.yaml` from the package again. The keys, when the manifest
/// has them, are summary, description, homepage, tags, author, requires,
/// works_with, levels, full_awareness, networks, network_bytes, options, icon,
/// entry, scripts and data. A key the manifest does not have is left out.
/// `full_awareness` and `network_bytes` are always written. `scripts` is how
/// many Lua files compiled. `data` is the table paths in byte order.
///
/// @param package a brain `open_package` accepted
/// @param[in,out] writer the object the facts are written into
/// @param checked the file check of this package
void describe_oabrain(
    const oa::app::package_install::Package& package,
    oa::formats::json::JsonWriter& writer,
    const BrainChecked& checked
);

} // namespace oa::tool
