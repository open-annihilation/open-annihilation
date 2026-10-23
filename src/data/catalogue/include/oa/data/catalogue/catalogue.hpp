// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A registry's catalogue, format 1: the packages it offers, named by their
// SHA-256, and the rules for one entry. The signature, the sequence and
// expiry are checked separately. Nothing here fetches.
#pragma once

#include "oa/base/sha256.hpp"
#include "oa/data/registry/descriptor.hpp"
#include "oa/formats/oabrain/manifest.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/url.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::catalogue {

/// The catalogue format this build reads.
inline constexpr int64_t catalogue_version = 1;

/// The most bytes of a catalogue this build reads.
inline constexpr std::size_t max_catalogue_bytes = 16 << 20;

/// The most bytes of a detached signature file.
inline constexpr std::size_t max_signature_file_bytes = 256;

/// The most packages one catalogue lists.
inline constexpr std::size_t max_packages = 20000;

/// The most skipped entries whose reason is kept.
inline constexpr std::size_t max_problems = 100;

/// Which kind of package an entry offers.
enum class Kind : uint8_t {
    oalang,    ///< a language pack
    oamod,     ///< a mod
    oamap,     ///< a map pack
    expansion, ///< an expansion package
    oamut,     ///< a mutator
    oabrain,   ///< a brain
};

/// The kind as the catalogue writes it.
///
/// @param kind the kind
/// @return `oalang`, `oamod`, `oamap`, `expansion`, `oamut` or `oabrain`; never null
[[nodiscard]] inline const char* kind_name(Kind kind) noexcept {
    switch (kind) {
    case Kind::oalang:
        return "oalang";
    case Kind::oamod:
        return "oamod";
    case Kind::oamap:
        return "oamap";
    case Kind::expansion:
        return "expansion";
    case Kind::oamut:
        return "oamut";
    case Kind::oabrain:
        return "oabrain";
    }
    return "";
}

/// The package file's extension, without the dot.
///
/// An expansion's catalogue kind is `expansion` and its file ends `.oaexp`.
/// Every other kind's file ends with the kind's own name.
///
/// @param kind the kind
/// @return `oalang`, `oamod`, `oamap`, `oaexp`, `oamut` or `oabrain`; never null
[[nodiscard]] inline const char* package_extension(Kind kind) noexcept {
    if (kind == Kind::expansion)
        return "oaexp";
    return kind_name(kind);
}

/// One map inside a map pack, as the catalogue lists it.
struct MapEntry {
    std::string map;     ///< `<stem>@<id>`, the name the game uses
    std::string stem;    ///< the part of `map` before the one `@`
    std::string name;    ///< the title a player sees, 1 to 64 bytes
    int32_t players{};   ///< 1 to 10
    int32_t width{};     ///< map width, 1 to 256
    int32_t height{};    ///< map height, 1 to 256
    std::string preview; ///< a picture reference ending `.png`; empty when the map has none
    /// Mod names this map was published against, in the order written, each with whether it fits.
    std::vector<std::pair<std::string, bool>> compatible;
};

/// One mutator a catalogue entry names, with any version range as written.
struct MutatorRequirement {
    std::string id; ///< a mutator key, lower-case kebab-case of 1 to 64 bytes
    /// The version range as written. Empty when the entry names no range.
    std::string range_text;
    /// The range, read with the mutator range parser. Absent when `range_text` is empty.
    std::optional<formats::oamut::VersionRange> range;
};

/// One value of a choice option, as the catalogue writes it.
struct ChoiceValueText {
    bool number{false}; ///< true when the value is a JSON number
    std::string text;   ///< the value as written
};

/// One level a brain offers that this build knows.
struct BrainLevel {
    std::string id;   ///< a level id: lower-case kebab-case of 1 to 32 bytes
    std::string name; ///< the name a player sees, 1 to 64 bytes
    /// `easy`, `medium` or `hard`.
    formats::oabrain::Difficulty difficulty{formats::oabrain::Difficulty::easy};
};

/// One option as the catalogue summarises it, for the Library to show.
struct OptionSummary {
    std::string id;    ///< an option id, a Lua name of 1 to 32 bytes
    std::string label; ///< 1 to 64 bytes
    /// `toggle`, `choice` or `number`.
    formats::oamut::OptionType type{formats::oamut::OptionType::toggle};
    /// The default as written: `true` or `false`, a choice value's text, or a
    /// number's text. Empty when the entry names none.
    std::string default_text;
    /// A choice's values, as written. Empty for a toggle or a number.
    std::vector<ChoiceValueText> values;
    /// One label per choice value, when the entry names them. Empty otherwise.
    std::vector<std::string> value_labels;
    /// A number option's minimum, as written. Empty for a toggle or a choice.
    std::string minimum;
    /// A number option's maximum, as written. Empty for a toggle or a choice.
    std::string maximum;
    /// A number option's step, as written. Empty for a toggle or a choice.
    std::string step;
};

/// One package the catalogue offers. A skipped entry is not stored here.
struct Package {
    Kind kind = Kind::oamod;
    std::string id;   ///< the package key; unique in the catalogue, ignoring case
    std::string name; ///< the name a player sees, 1 to 64 bytes
    /// 1 to 32 bytes; empty when this kind names none. A mutator's or a brain's is a
    /// version of one to three parts.
    std::string version;
    int32_t revision{};            ///< a mod's revision, 1 to 65535; 0 when this package names none
    int64_t release{};             ///< the release number, 1 to 2^31-1
    uint64_t size{};               ///< the package's size in bytes, 1 to 2^32
    base::sha256::Digest sha256{}; ///< the package's SHA-256
    std::string file;              ///< the reference whose last segment is `<sha256>.<kind>`
    std::string publisher;         ///< 1 to 64 bytes; empty when the entry names none
    std::string author;            ///< 1 to 64 bytes; empty when the entry names none
    std::string summary;           ///< up to 200 bytes
    std::string notes;             ///< what this release changes, up to 2000 bytes
    std::string homepage; ///< an address the game can open; empty when the entry names none
    std::string badge;    ///< a picture reference ending `.png`; empty when the entry names none
    /// The day this release was published, `YYYY-MM-DD` as written; empty
    /// when the entry names none.
    std::string published;
    std::vector<std::string> tags; ///< up to 16 tags, each as a package manifest may write one
    std::string
        requires_engine_text; ///< the engine requirement as written; empty when there is none
    /// The engine requirement, read with the shared range parser. Absent when the entry names none.
    std::optional<formats::oamod::EngineRange> requires_engine;
    std::string requires_base; ///< the base the package names; empty when it names none
    std::optional<base::sha256::Digest> sim_hash; ///< the rules hash, when the entry names one
    std::optional<int32_t> hacks_sim;             ///< how many hacks change the game
    std::optional<int32_t> hacks_view;            ///< how many hacks change only what a player sees
    std::vector<std::string> hack_ids;            ///< the hack ids the mod turns on, at most 256
    std::optional<double> coverage; ///< how much of the game a language pack translates, 0 to 1
    std::string english_name;       ///< a language's English name, 1 to 64 bytes; empty when absent
    std::string word;               ///< the manifest's word for the language, 1 to 32 bytes
    std::vector<std::string> locales;   ///< locale tags this language matches, at most 16
    std::vector<std::string> fallbacks; ///< locale tags it falls back to, at most 8
    std::string needs;                  ///< `game-fonts` or `modern-fonts`; empty when absent
    /// Base games the package runs with, as `supportedGames` names them.
    /// Empty when the entry omits the key. An empty list is refused.
    std::vector<std::string> supported_games;
    /// Expansions that must be on, as `requiredExpansions` names them.
    /// Empty when the entry omits the key, and empty when it names none.
    std::vector<std::string> required_expansions;
    /// Base games an expansion extends, in the table's order. Empty for
    /// every other kind.
    std::vector<std::string> extends;
    std::vector<MapEntry> maps; ///< a map pack's maps; empty for every other kind
    /// Prerequisites, in the order they apply. Empty for every other kind.
    std::vector<MutatorRequirement> requires_mutators;
    /// Mutators this one cannot run beside. Empty for every other kind.
    std::vector<MutatorRequirement> conflicts_with;
    /// Conditions a mutator or a brain works with, as written. Empty for every other kind.
    std::vector<std::string> works_with;
    /// Layers this mutator uses, known names kept once. Empty for every other kind.
    std::vector<formats::oamut::Layer> layers;
    /// How this mutator treats a unit or weapon that already has that name.
    /// Absent when the entry names none, and absent for every other kind.
    std::optional<formats::oamut::ClashMode> clashes;
    /// One summary per option of a mutator or a brain. Empty for every other kind.
    std::vector<OptionSummary> options;
    /// Levels this build knows, in the order written. Empty, false or 0 for every other kind.
    std::vector<BrainLevel> levels;
    /// Whether a brain sees every unit. Empty, false or 0 for every other kind.
    bool full_awareness{false};
    /// The bytes of a brain's networks taken together. Empty, false or 0 for every other kind.
    uint64_t network_bytes{};
};

/// One catalogue that was read. `problems` holds why entries were skipped.
struct Catalogue {
    std::string registry;                    ///< the registry id the catalogue names
    int64_t sequence{};                      ///< the publisher's sequence, at least 0
    std::string generated_text;              ///< `generated`, as written
    std::string expires_text;                ///< `expires`, as written
    int64_t generated{};                     ///< `generated`, seconds since 1970
    int64_t expires{};                       ///< `expires`, seconds since 1970
    std::string downloads;                   ///< an informational http address; empty when absent
    std::vector<registry::RegistryKey> keys; ///< the registry's current keys; empty when absent
    std::vector<formats::url::Url> mirrors;  ///< other copies of this catalogue, at most 8
    std::string icon; ///< a picture reference ending `.png`; empty when the catalogue names none
    std::vector<Package> packages;     ///< the entries that passed their rules
    std::vector<std::string> problems; ///< why entries were skipped, at most max_problems
};

/// Reads a catalogue's structure and its entries.
///
/// The bytes are not checked against a signature or against an older
/// sequence. `catalogue` must be 1. A member this build does not know is
/// ignored. A member named twice in one object refuses the catalogue. One
/// package that breaks a rule is left out, and the reason is kept in
/// `problems` when fewer than max_problems reasons are kept already; the
/// other packages stay. Two packages with one key, ignoring case, are both
/// left out.
///
/// @param bytes the catalogue's bytes
/// @param[out] out the catalogue; left unchanged when the bytes are refused
/// @param[out] error why the bytes were refused; may be null
/// @return true when the catalogue was read
[[nodiscard]] bool
read_catalogue(std::span<const uint8_t> bytes, Catalogue& out, std::string* error);

/// Reports whether a path is a catalogue reference.
///
/// A reference is absolute from the host's root (`/v1/p/name`) or relative
/// to the catalogue's folder (`p/name`). It is at most 512 bytes. It has no
/// scheme, no `//`, no `.` or `..` segment, no empty segment, and no
/// backslash, `%`, `?`, `#`, space, control character or byte above 0x7E.
///
/// @param text the path
/// @return true when the path is such a reference
[[nodiscard]] bool valid_reference(std::string_view text) noexcept;

/// Reports whether a package key is the key for its kind.
///
/// A mod, a map pack, an expansion, a mutator or a brain uses lower-case
/// kebab-case of 1 to 64 bytes: words of `a`-`z` and `0`-`9` joined by single hyphens.
/// A language pack uses a language tag of 2 to 35 bytes: letters, then
/// hyphen-joined parts of letters and digits. No key contains an at sign.
///
/// @param kind the package's kind
/// @param text the key
/// @return true when the key is of that form
[[nodiscard]] bool valid_package_key(Kind kind, std::string_view text) noexcept;

/// Reads a UTC time written `YYYY-MM-DDTHH:MM:SSZ`.
///
/// A fraction of a second may follow the seconds, a dot and one or more
/// digits. The fraction is accepted and does not change the second. The
/// date is a real day, February 29 included in a Gregorian leap year. The
/// result is that whole second counted from 1970-01-01T00:00:00Z.
///
/// @param text the time
/// @return the seconds since 1970, or nothing when the text is not such a time
[[nodiscard]] std::optional<int64_t> parse_utc_time(std::string_view text);

/// Reads a day written `YYYY-MM-DD`, as a package's `published` names one.
///
/// The year is four digits and the month and day two each. The date is a
/// real day, February 29 included in a Gregorian leap year. Nothing may
/// come before or after it.
///
/// @param text the day
/// @return the days since 1970-01-01, or nothing when the text is not such a day
[[nodiscard]] std::optional<int64_t> parse_day(std::string_view text);

} // namespace oa::data::catalogue
