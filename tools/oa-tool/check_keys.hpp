// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A key layout's files, and the facts `check` reports for one. The picture
// marks the same triggers. Nothing here resolves bindings against a mod.
#pragma once

#include "check_mutator.hpp"

#include "oa/data/key_library/library.hpp"
#include "oa/formats/oakeys/manifest.hpp"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace oa::tool {

/// What a layout changes, and the warnings a check keeps beside the errors.
struct KeysChecked {
    uint32_t changes{};                  ///< actions the layout binds
    std::vector<std::string> triggers{}; ///< each trigger once, in the order of first use
    uint32_t unknown_actions{};          ///< warnings for an action this build does not know
    uint32_t clashes{};                  ///< warnings for one key bound twice
    std::vector<std::string> warnings{}; ///< layout warnings, in diagnostic form
};

/// A folder read as a key layout, with every problem named.
struct KeyFolder {
    std::vector<oa::data::key_library::PackageFile> files{}; ///< the files a package may hold
    oa::formats::oakeys::Manifest manifest{};                ///< the manifest, when it was read
    bool have_files{false};              ///< `read_folder` returned the files; the manifest parsed
    std::vector<std::string> problems{}; ///< errors; warnings stay on `checked`
    KeysChecked checked{};               ///< the facts, filled when the manifest was read
};

/// Reads a layout folder the way the packer does, and names every problem.
///
/// Calls `read_folder`. A manifest that does not read contributes every
/// `describe` line. A folder that does contributes `check_keys_files`: a file
/// outside the layout, an icon outside the limits, and each layout error.
/// Warnings are kept on `checked` and are not problems.
///
/// @param folder the folder
/// @return the files, the manifest and the problems
[[nodiscard]] KeyFolder inspect_key_folder(const std::filesystem::path& folder);

/// Checks the files a key layout may hold.
///
/// Every file is asked of `file_role`. The icon must be a square PNG within
/// the icon limits. `layout_from_manifest` is called with no keyboard map.
/// An error is appended to `problems`. A warning is kept on `checked`, as
/// `oakeys.yaml:<line>:<column>: <key>: <message>`. `changes` is how many
/// actions the manifest binds. `triggers` are the layout's own triggers, each
/// once, in the order of first use. `unknown_actions` counts warnings whose
/// message is `Not in this version of Open Annihilation`. `clashes` counts
/// warnings whose message says `both bind`. Two actions whose chords are
/// written apart but pressed as one key on some platforms, such as
/// `Primary+5` and `Ctrl+5`, are such a warning too, naming those platforms:
/// Windows, Linux and the Steam Deck where Primary is Ctrl, macOS where it is
/// the command key.
///
/// @param entries the package's files
/// @param manifest the manifest `oakeys.yaml` already read
/// @param[in,out] problems one line appended for each error
/// @param[out] checked the facts and the warnings
void check_keys_files(
    const PackageEntries& entries,
    const oa::formats::oakeys::Manifest& manifest,
    std::vector<std::string>& problems,
    KeysChecked& checked
);

/// Reads `oakeys.yaml` from a package's entries.
///
/// @param entries the package's files
/// @param[out] manifest the manifest; left unchanged when it does not read
/// @param[in,out] problems one `describe` line appended for each broken rule
/// @return true when the manifest was read
[[nodiscard]] bool read_oakeys_manifest(
    const PackageEntries& entries,
    oa::formats::oakeys::Manifest& manifest,
    std::vector<std::string>& problems
);

/// The W3C codes of the keys a layout's own triggers change.
///
/// A character is matched on the US keyboard, without regard to ASCII case.
/// A place and a named key use their code. A mouse button, a wheel turn, a
/// pad button and a modifier alone are not codes. Each code is kept once, in
/// the order of first use.
///
/// @param manifest the layout
/// @return the codes
[[nodiscard]] std::vector<std::string>
changed_key_codes(const oa::formats::oakeys::Manifest& manifest);

/// Writes a key layout's catalogue facts into an object that is already open.
///
/// Reads `oakeys.yaml` from the package again. The keys, when the manifest
/// has them, are summary, description, homepage, tags, author, requires,
/// works_with, base, keyboard, pointer, helpers, changes, triggers,
/// unknown_actions, clashes and icon. `keyboard`, `base`, `changes`,
/// `triggers`, `unknown_actions` and `clashes` are always written. A text
/// the manifest does not have is left out. `requires` holds `engine` only.
/// `helpers` names the helpers the manifest sets, in the standard order.
///
/// @param package a key layout `open_package` accepted
/// @param[in,out] writer the object the facts are written into
/// @param checked the file check of this package
void describe_oakeys(
    const oa::app::package_install::Package& package,
    oa::formats::json::JsonWriter& writer,
    const KeysChecked& checked
);

} // namespace oa::tool
