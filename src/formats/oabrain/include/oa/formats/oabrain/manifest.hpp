// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A brain's manifest, oabrain.yaml. The installer, the catalogue and the
// Library read one description of a brain without running any of its code.
#pragma once

#include "oa/formats/oamut/manifest.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::formats::oabrain {

/// The manifest format this build reads: the manifest's oabrain value.
inline constexpr int64_t format_version = 1;

/// The name of a brain's manifest.
inline constexpr std::string_view manifest_file = "oabrain.yaml";

/// The entry script a manifest uses when it names none.
inline constexpr std::string_view default_entry = "brain.lua";

/// The only icon file a manifest may name.
inline constexpr std::string_view icon_file = "icon.png";

/// The most bytes of a brain id.
inline constexpr std::size_t most_id_bytes = 64;

/// The most bytes of a brain's name.
inline constexpr std::size_t most_name_bytes = 64;

/// The most bytes of a brain's description, line feeds included.
inline constexpr std::size_t most_description_bytes = 2000;

/// The most bytes of the summary cut from the description's first line.
inline constexpr std::size_t most_summary_bytes = 200;

/// The most tags a brain carries.
inline constexpr std::size_t most_tags = 4;

/// The tags a brain may carry, in the order a catalogue lists them.
inline constexpr std::array<std::string_view, 9> brain_tags{
    "classic",
    "economy",
    "rush",
    "turtle",
    "air",
    "naval",
    "learned",
    "teaching",
    "experimental",
};

/// The most expansions one brain names.
inline constexpr std::size_t most_expansions = 16;

/// The most levels a brain offers.
inline constexpr std::size_t most_levels = 6;

/// The most bytes of a level id.
inline constexpr std::size_t most_level_id_bytes = 32;

/// The most bytes of a level's name.
inline constexpr std::size_t most_level_name_bytes = 64;

/// The most networks one brain ships.
inline constexpr std::size_t most_networks = 16;

/// The most bytes of a file stem in lua/, nets/ or data/.
inline constexpr std::size_t most_file_stem_bytes = 64;

/// The most bytes of one Lua source. The installer and oa-tool apply it.
inline constexpr std::size_t most_script_bytes = std::size_t{1} << 20;

/// The most bytes of every network in one brain, taken together.
inline constexpr std::size_t most_network_bytes = std::size_t{64} << 20;

/// The most bytes of one data table.
inline constexpr std::size_t most_data_bytes = std::size_t{4} << 20;

/// How hard a level plays, as the income rules a seat already uses.
///
/// The order is easy, then medium, then hard, so a caller can map the value
/// onto the seat's income setting without another table.
enum class Difficulty : uint8_t {
    easy,   ///< the easy income
    medium, ///< the medium income
    hard,   ///< the hard income
};

/// Returns a difficulty's manifest name.
///
/// @param difficulty the difficulty
/// @return its name: easy, medium or hard
[[nodiscard]] const char* difficulty_name(Difficulty difficulty) noexcept;

/// Reads a difficulty.
///
/// @param text the name, compared byte for byte
/// @return the difficulty, or nothing when the text is not easy, medium or hard
[[nodiscard]] std::optional<Difficulty> parse_difficulty(std::string_view text) noexcept;

/// One level the seat picker offers, in the order the manifest writes it.
struct Level {
    std::string id{};                        ///< lower-case kebab-case
    std::string name{};                      ///< the words the picker shows
    Difficulty difficulty{Difficulty::easy}; ///< the income rules for a seat that picks it
};

/// One network the brain ships.
struct Network {
    std::string id{};     ///< a Lua name, the name a script asks for
    std::string file{};   ///< nets/<stem>.oann
    std::string sha256{}; ///< 64 lower-case hex digits
};

/// A level's name in one language.
struct LevelName {
    std::string id{};   ///< a level this brain defines
    std::string name{}; ///< the name in this language
};

/// One language's names for the brain, its options and its levels.
struct Translation {
    oamut::Translation words{};      ///< the name, description, options and labels
    std::vector<LevelName> levels{}; ///< level names, in the order written
};

/// One broken rule, where it is written.
using Problem = oamut::Problem;

/// A brain's manifest, as oabrain.yaml states it.
struct Manifest {
    int64_t format{};                           ///< oabrain: the format, format_version
    std::string id{};                           ///< the brain's id
    std::string name{};                         ///< the name players see
    std::string description{};                  ///< the description
    oamut::Version version{};                   ///< the version other packages name in ranges
    oamut::Author author{};                     ///< who wrote it
    std::string homepage{};                     ///< an http or https address; empty when unwritten
    std::string icon{};                         ///< icon.png when the package has a picture
    std::vector<std::string> tags{};            ///< one to four tags, in the order written
    std::vector<oamut::Condition> works_with{}; ///< mods and the base game it fits
    std::string requires_engine{};              ///< the engine requirement, as written
    std::vector<std::string> requires_expansions{}; ///< expansion ids, in the order written
    std::vector<Level> levels{};                    ///< the levels, in the order written
    bool full_awareness{false};                     ///< sees every unit; false when unwritten
    std::vector<Network> networks{};                ///< the networks, in the order written
    std::vector<oamut::Option> options{};           ///< the player's settings, in the order written
    std::vector<Translation> strings{};             ///< translations, in the order written
    std::string entry{std::string{default_entry}};  ///< the entry script
};

/// Which part of a brain a file is, or that a brain may not hold it.
enum class FileRole : uint8_t {
    manifest, ///< oabrain.yaml
    entry,    ///< the manifest's entry script
    module,   ///< a Lua module under lua/
    network,  ///< a network the manifest names
    data,     ///< a table under data/
    icon,     ///< icon.png, when the manifest names it
    readme,   ///< README.md
    licence,  ///< LICENSE
    outside,  ///< a file a brain may not hold
};

/// Tells whether text is a brain id.
///
/// Lower-case kebab-case of 1 to 64 bytes: words of `a`-`z` and `0`-`9`
/// joined by single hyphens, not starting or ending with a hyphen.
///
/// @param text the id
/// @return true when it is such an id
[[nodiscard]] bool brain_id(std::string_view text) noexcept;

/// Tells whether text is one of the nine brain tags.
///
/// @param text the tag, compared byte for byte
/// @return true when it is one of brain_tags
[[nodiscard]] bool brain_tag(std::string_view text) noexcept;

/// Tells whether text is a level id.
///
/// Lower-case kebab-case of 1 to 32 bytes, the same shape as a brain id
/// with the shorter limit.
///
/// @param text the id
/// @return true when it is such an id
[[nodiscard]] bool level_id(std::string_view text) noexcept;

/// Reads a manifest.
///
/// Collects every broken rule. A document that is not the strict YAML of a
/// mod profile is one problem. The manifest is left unchanged when any rule
/// is broken.
///
/// @param bytes the manifest's bytes
/// @param[out] manifest the manifest; left unchanged when any rule is broken
/// @param[in,out] problems one entry appended for each broken rule
/// @return true when the manifest was read and `problems` gained nothing
[[nodiscard]] bool
read_manifest(std::span<const uint8_t> bytes, Manifest& manifest, std::vector<Problem>& problems);

/// Writes a manifest as strict YAML.
///
/// Keys are written in a fixed order, with two-space indentation. Every
/// string is double-quoted. `fullAwareness` is written only when true, and
/// `entry` only when it is not brain.lua. An empty optional block is left out.
///
/// @param manifest the manifest
/// @return its text, ending in a line break
[[nodiscard]] std::string write_manifest(const Manifest& manifest);

/// Returns the summary the Library shows: the description's first line.
///
/// The line is cut to most_summary_bytes at a UTF-8 character boundary.
///
/// @param manifest the manifest
/// @return the first line, cut to fit
[[nodiscard]] std::string summary_of(const Manifest& manifest);

/// Describes a problem the way a manifest's diagnostics read.
///
/// @param problem the problem
/// @return `oabrain.yaml:<line>:<column>: <key>: <message>`
[[nodiscard]] std::string describe(const Problem& problem);

/// Finds a level by id.
///
/// @param manifest the manifest
/// @param id the level id, compared byte for byte
/// @return the level, or null when the brain has none of that id
[[nodiscard]] const Level* find_level(const Manifest& manifest, std::string_view id) noexcept;

/// Tells which part of a brain a file is.
///
/// The path is compared as written. A folder is not asked. `oabrain.yaml`,
/// `README.md` and `LICENSE` match without regard to ASCII case. A module
/// or table name with a capital letter is outside.
///
/// @param path the file's path in the package
/// @param manifest the manifest, for the entry, the icon and the networks
/// @return the role, or outside when a brain may not hold the file
[[nodiscard]] FileRole file_role(std::string_view path, const Manifest& manifest) noexcept;

} // namespace oa::formats::oabrain
