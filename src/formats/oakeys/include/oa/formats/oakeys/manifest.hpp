// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A key layout's manifest, oakeys.yaml, and the short text of its bindings.
// Chords stay text here: reading a chord, and knowing an action, is the
// key-bindings layer's work.
#pragma once

#include "oa/formats/oamut/manifest.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::formats::oakeys {

/// The manifest format this build reads: the manifest's oakeys value.
inline constexpr int64_t format_version = 1;

/// The name of a layout's manifest.
inline constexpr std::string_view manifest_file = "oakeys.yaml";

/// The most bytes of a layout id.
inline constexpr std::size_t most_id_bytes = 64;

/// The most bytes of a layout's name.
inline constexpr std::size_t most_name_bytes = 64;

/// The most bytes of a layout's description, line feeds included.
inline constexpr std::size_t most_description_bytes = 2000;

/// The most tags a layout carries.
inline constexpr std::size_t most_tags = 4;

/// The most bytes of one tag.
inline constexpr std::size_t most_tag_bytes = 32;

/// The most works-with conditions one layout names.
inline constexpr std::size_t most_works_with = 32;

/// The most languages a layout translates into.
inline constexpr std::size_t most_languages = 16;

/// The most actions one layout names.
inline constexpr std::size_t most_bindings = 2048;

/// The most bytes of an action id.
inline constexpr std::size_t most_action_id_bytes = 96;

/// The most chords one action lists.
inline constexpr std::size_t most_chords = 4;

/// The ids a layout may not take for itself: the built-in layout, and the
/// id the draft standard kept for a classic layout this build does not ship.
inline constexpr std::string_view built_in_ids[] = {"oa-default", "classic"};

/// Which base a layout changes.
enum class Base {
    oa_default, ///< the game's own layout
    none,       ///< nothing bound, apart from what the game puts back
};

/// The keyboard a layout was drawn for. It is for the picture only.
enum class Keyboard {
    any,  ///< no particular keyboard
    ansi, ///< an ANSI keyboard
    iso,  ///< an ISO keyboard
    jis,  ///< a JIS keyboard
};

/// The pointer scheme a layout was drawn for.
enum class Pointer {
    right_click, ///< the right button commands
    left_click,  ///< the left button commands
};

/// How a layout opens the order wheel.
enum class RadialOrders {
    off,         ///< the wheel stays closed
    hold_right,  ///< holding the right button opens it
    hold_middle, ///< holding the middle button opens it
};

/// Whether a layout scrolls at the window's edge.
enum class EdgeScroll {
    on,  ///< the view scrolls at the edge
    off, ///< the view stays put at the edge
};

/// One chord as the file writes it, and where that text starts.
struct ChordText {
    std::string text{}; ///< the chord, as written
    uint32_t line{};    ///< the 1-based line of the chord
    uint32_t column{};  ///< the 1-based byte column of the chord
};

/// One action's chords, in the order the file writes them.
struct BindingEntry {
    std::string action{};            ///< the action id
    std::vector<ChordText> chords{}; ///< its chords, at most four
    uint32_t line{};                 ///< the 1-based line of the action id
    uint32_t column{};               ///< the 1-based byte column of the action id
};

/// The input helpers a layout sets. A helper left unset is left out of the file.
struct Helpers {
    /// Hold actions that latch. Set and empty means none of them latch.
    std::optional<std::vector<std::string>> toggle{};
    /// A tapped modifier holds for the next key or click.
    std::optional<bool> sticky_modifiers{};
    /// Actions that need a hold. Set and empty means none of them do.
    std::optional<std::vector<std::string>> hold_to_confirm{};
    /// The pointer actions move and click the pointer.
    std::optional<bool> pointer_keys{};
    /// How the order wheel opens.
    std::optional<RadialOrders> radial_orders{};
    /// Whether the view scrolls at the window's edge.
    std::optional<EdgeScroll> edge_scroll{};
};

/// What an import made a layout from, as text the details can show.
struct ImportedFrom {
    std::string tool{}; ///< the tool the keys came from
    std::string file{}; ///< the file they were read from
    std::string date{}; ///< when they were read
};

/// One broken rule, where it is written.
using Problem = oamut::Problem;

/// A layout's manifest, as oakeys.yaml states it.
struct Manifest {
    int64_t format{format_version};              ///< oakeys: the format, format_version
    std::string id{};                            ///< the layout's id
    std::string name{};                          ///< the name players see
    oamut::Version version{};                    ///< the version other packages name
    oamut::Author author{};                      ///< who wrote it
    std::string description{};                   ///< the description; empty when unwritten
    std::string homepage{};                      ///< an http or https address; empty when unwritten
    std::string icon{};                          ///< a PNG in the package; empty when unwritten
    std::vector<std::string> tags{};             ///< one to four tags, in the order written
    Base base{Base::oa_default};                 ///< what this layout changes
    std::vector<oamut::Condition> works_with{};  ///< mods it is made for; empty means any
    std::string requires_engine{};               ///< the engine requirement, as written
    Keyboard keyboard{Keyboard::any};            ///< the keyboard it was drawn for
    std::optional<Pointer> pointer{};            ///< the pointer scheme, when the file names one
    std::vector<BindingEntry> bindings{};        ///< the actions it names, in file order
    Helpers helpers{};                           ///< the helpers it sets
    std::vector<oamut::Translation> strings{};   ///< translations, in the order written
    std::optional<ImportedFrom> imported_from{}; ///< what an import made it from
};

/// Reads a manifest.
///
/// Collects every broken rule. A document that is not the strict YAML of a
/// mod profile is one problem on the key `document`. The manifest is left
/// unchanged when any rule is broken.
///
/// @param bytes the manifest's bytes
/// @param[out] manifest the manifest; left unchanged when any rule is broken
/// @param[in,out] problems one entry appended for each broken rule
/// @return true when the manifest was read and `problems` gained nothing
[[nodiscard]] bool
read_manifest(std::span<const uint8_t> bytes, Manifest& manifest, std::vector<Problem>& problems);

/// Writes a manifest as strict YAML.
///
/// Keys are written in the manifest's order, with two-space indentation.
/// Every string is double-quoted. Each binding is one flow sequence, and a
/// chord is quoted when it holds anything but letters, digits and `+`, or
/// when leaving it bare would make it a number or a boolean. An optional
/// key that was left out is left out again. `keyboard` is left out when it
/// is `any`.
///
/// @param manifest the manifest
/// @return its text, ending in a line break
[[nodiscard]] std::string write_manifest(const Manifest& manifest);

/// Describes a problem the way a manifest's diagnostics read.
///
/// @param problem the problem
/// @return `oakeys.yaml:<line>:<column>: <key>: <message>`
[[nodiscard]] std::string describe(const Problem& problem);

/// Writes the bindings as the text a player can paste.
///
/// One comment names the base, then each binding on its own line, in the
/// same form `write_manifest` gives a binding.
///
/// @param manifest the manifest; only the base and the bindings are written
/// @return the text, ending in a line break
[[nodiscard]] std::string write_bindings_text(const Manifest& manifest);

/// Reads the paste text, or a whole manifest.
///
/// A text whose top mapping has an `oakeys` key is a whole manifest, read
/// by `read_manifest`. Anything else is the bindings alone. The base is the
/// comment `# oakeys bindings, base <base>`, and `oa-default` when that
/// comment is absent. The manifest is left unchanged when any rule is broken.
///
/// @param bytes the text
/// @param[out] manifest the manifest; left unchanged when any rule is broken
/// @param[in,out] problems one entry appended for each broken rule
/// @return true when the text was read and `problems` gained nothing
[[nodiscard]] bool read_bindings_text(
    std::span<const uint8_t> bytes, Manifest& manifest, std::vector<Problem>& problems
);

} // namespace oa::formats::oakeys
