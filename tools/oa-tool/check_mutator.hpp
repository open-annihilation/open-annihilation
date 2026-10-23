// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A mutator's scripts and rule files, and the facts `check` reports for one.
// Scripts are compiled and not run. Rule files are parsed and not applied.
#pragma once

#include "oa/app/package_install.hpp"
#include "oa/formats/json.hpp"
#include "oa/formats/oamut/manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::tool {

/// How many scripts compiled, and the rule files that were read.
struct MutatorChecked {
    uint32_t scripts{};             ///< `.lua` files that compiled
    std::vector<std::string> rules; ///< `.sm` paths, in unsigned byte order
};

/// Reads package entries by their path.
///
/// `list` appends every file, not a directory. `read` copies one file into
/// `bytes` when it holds at most `limit` bytes. A larger file sets
/// `too_large` and returns false, and `bytes` is left empty. A missing or
/// unreadable file returns false with `too_large` false.
struct PackageEntries {
    void* context{}; ///< passed back to list and read
    void (*list)(void* context, std::vector<std::string>& paths){};
    bool (*read)(
        void* context,
        std::string_view path,
        std::size_t limit,
        std::vector<uint8_t>& bytes,
        bool& too_large
    ){};
};

/// The entries of an opened package. `package` must outlive the reader.
///
/// @param package the package
/// @return a reader of its files
[[nodiscard]] PackageEntries entries_of(const oa::app::package_install::Package& package);

/// Checks a mutator's Lua sources and its rule files.
///
/// The entry script and every `lua/**/*.lua` file are compiled, each at most
/// 1 MiB. Every `rules/**/*.sm` file is parsed, each at most 4 MiB. Nothing
/// is run and no rule is applied. Each problem is appended as
/// `<path>:<line>: <message>`.
///
/// @param entries the package's files
/// @param[in,out] problems one line appended for each problem
/// @param[out] checked how many scripts compiled, and the rule paths
void check_mutator_scripts_and_rules(
    const PackageEntries& entries, std::vector<std::string>& problems, MutatorChecked& checked
);

/// Writes a mutator's catalogue facts into an object that is already open.
///
/// Reads `oamut.yaml` from the package again. The keys, when the manifest
/// has them, are summary, description, homepage, tags, author, requires,
/// works_with, conflicts_with, layers, options, icon, entry, scripts, rules
/// and legacy. A key the manifest does not have is left out. `scripts` is
/// how many Lua files compiled. `rules` is the rule paths in byte order.
///
/// @param package a mutator `open_package` accepted
/// @param[in,out] writer the object the facts are written into
/// @param checked the script and rule check of this package
void describe_oamut(
    const oa::app::package_install::Package& package,
    oa::formats::json::JsonWriter& writer,
    const MutatorChecked& checked
);

/// Writes one option's summary, in the mutator's shape.
///
/// The keys are id, label, type and default. A choice adds values and, when
/// the manifest names them, labels. A number adds min, max and step.
///
/// @param[in,out] writer the array the object is written into
/// @param option the option
void write_option_facts(
    oa::formats::json::JsonWriter& writer, const oa::formats::oamut::Option& option
);

/// Writes the icon's path, digest, size and pixel size.
///
/// The key is left out when `path` is empty, the package has no such file,
/// the file is larger than `size_limit`, or the bytes are not a PNG.
/// `path` is the manifest's spelling.
///
/// @param package the package
/// @param path the icon's path in the manifest
/// @param size_limit the most bytes of that file
/// @param[in,out] writer the object the fact is written into
void write_icon_facts(
    const oa::app::package_install::Package& package,
    std::string_view path,
    std::uint64_t size_limit,
    oa::formats::json::JsonWriter& writer
);

} // namespace oa::tool
