// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The resolver: a profile's tree checked block by block against the
// registry, then each limit's and hack's parameters resolved in the
// registry's order (baseline, default, preset, profile, settings), the
// player's overrides of standard hacks, then the host's match choices, the
// effective profile
// assembled, and its canonical form and hashes made.

#include "oa/data/mod_profile.hpp"
#include "oa/data/mod_profile/registry.hpp"
#include "oa/data/content_bundles/content_needs.hpp"
#include "oa/formats/oamod/package_keys.hpp"

#ifndef OA_ENGINE_VERSION
#error                                                                                             \
    "OA_ENGINE_VERSION names this build's version, which a profile's engine requirement is checked against"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <map>
#include <set>
#include <optional>
#include <string>
#include <utility>

namespace oa::data::mod_profile {

/// Fills a profile's records from its effective profile (records.cpp).
///
/// @param effective the effective profile
/// @param[out] profile the records; left at 3.1c where the effective profile says nothing
void fill_records(const Value& effective, ModProfile& profile);

namespace {

/// Names the limit check_limits found out of its range.
///
/// @param error the limit
/// @return its name, as the LimitsError value is spelt
std::string_view limits_error_name(limits::LimitsError error) {
    switch (error) {
    case limits::LimitsError::none:
        return "none";
    case limits::LimitsError::units_per_player:
        return "units_per_player";
    case limits::LimitsError::unit_type_bits:
        return "unit_type_bits";
    case limits::LimitsError::category_mask_types:
        return "category_mask_types";
    case limits::LimitsError::effect_queue:
        return "effect_queue";
    case limits::LimitsError::effect_reserve:
        return "effect_reserve";
    case limits::LimitsError::path_search_nodes:
        return "path_search_nodes";
    case limits::LimitsError::build_list_copy:
        return "build_list_copy";
    case limits::LimitsError::build_list_overflow:
        return "build_list_overflow";
    case limits::LimitsError::model_composite_sides:
        return "model_composite_sides";
    case limits::LimitsError::weapon_types:
        return "weapon_types";
    case limits::LimitsError::sides:
        return "sides";
    }
    return "unknown";
}

using formats::oamod::KeyKind;
using formats::oamod::Node;
using formats::oamod::NodeKind;
using formats::oamod::Number;
using formats::oamod::TextPosition;
using registry::Adjustable;
using registry::Entry;
using registry::EntryKind;
using registry::Parameter;
using registry::Scope;
using registry::ValueSpec;
using registry::ValueType;

/// The top-level keys a profile may hold.
constexpr std::array<std::string_view, 21> top_keys{
    "oamod",
    "id",
    "name",
    "version",
    "description",
    "homepage",
    "tags",
    "requires",
    "author",
    "packaging",
    "identity",
    "layout",
    "limits",
    "script-extensions",
    "data-keys",
    "hacks",
    "strings",
    "media",
    "settings",
    "supportedGames",
    "requiredExpansions",
};
/// The top-level keys whose values the effective profile carries as written.
/// None of them enters the sim hash except oamod. supportedGames and
/// requiredExpansions are among them: written, they enter the full hash;
/// omitted, they are not filled in.
constexpr std::array<std::string_view, 12> meta_keys{
    "oamod",
    "id",
    "name",
    "version",
    "description",
    "homepage",
    "tags",
    "supportedGames",
    "requiredExpansions",
    "requires",
    "author",
    "packaging"
};
/// The keys of the author block; name is required.
constexpr std::array<std::string_view, 2> author_keys{"name", "email"};
/// The keys of the packaging block; all are required.
constexpr std::array<std::string_view, 3> packaging_keys{"revision", "date", "packager"};
/// The longest author or packager name, in bytes.
constexpr size_t person_name_max_length = 128;
/// The longest e-mail address, in bytes, as mail paths allow.
constexpr size_t email_max_length = 254;
/// The range of a package's revision.
constexpr int64_t packaging_revision_min = 1;
constexpr int64_t packaging_revision_max = 65535;
/// The blocks that must be mappings.
constexpr std::array<std::string_view, 8> map_blocks{
    "identity",
    "layout",
    "limits",
    "hacks",
    "data-keys",
    "strings",
    "media",
    "settings",
};
/// The base game every profile builds on.
constexpr std::string_view base_game = base_game_id;
/// The grammar version a profile is written in.
constexpr int64_t grammar_version = 1;
/// The settings key that holds registry seeds rather than a binding.
constexpr std::string_view seeds_key = "registry-seeds";
/// How many bits of a unit-type count make one step of the unit-type
/// bitset, and the size of a step.
constexpr int64_t unit_type_step_shift = 9;
constexpr int64_t unit_type_step = int64_t{1} << unit_type_step_shift;

/// Tells whether a name is lower-case kebab-case.
///
/// @param text the name
/// @return true for words of a-z and 0-9 joined by single hyphens
bool kebab_case(std::string_view text) {
    if (text.empty() || text.front() == '-' || text.back() == '-')
        return false;
    char previous = 0;
    for (const char c : text) {
        const bool word = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9');
        if (!word && c != '-')
            return false;
        if (c == '-' && previous == '-')
            return false;
        previous = c;
    }
    return true;
}

/// Tells whether text reads as an e-mail address.
///
/// Checks the shape only: one @ with text on both sides, a dot inside the
/// part after it, and no white space or control characters.
///
/// @param text the address
/// @return true for an address of at most email_max_length bytes in that shape
bool email_address(std::string_view text) {
    if (text.empty() || text.size() > email_max_length)
        return false;
    for (const char c : text) {
        if (static_cast<unsigned char>(c) <= ' ' || c == '\x7f')
            return false;
    }
    const size_t at = text.find('@');
    if (at == 0 || at == std::string_view::npos || text.find('@', at + 1) != std::string_view::npos)
        return false;
    const std::string_view domain = text.substr(at + 1);
    const size_t dot = domain.find('.');
    return dot != std::string_view::npos && dot != 0 && domain.back() != '.';
}

/// Tells whether text is an ISO 8601 calendar date, YYYY-MM-DD, that exists.
///
/// @param text the date
/// @return true for a four-digit year, a month 01 to 12 and a day within that
///         month, February counting 29 days in Gregorian leap years
bool calendar_date(std::string_view text) {
    constexpr size_t year_digits = 4;
    constexpr size_t month_start = year_digits + 1;
    constexpr size_t day_start = month_start + 3;
    constexpr size_t date_length = day_start + 2;
    if (text.size() != date_length || text[month_start - 1] != '-' || text[day_start - 1] != '-')
        return false;
    const auto digits = [text](size_t start, size_t count, int& value) {
        value = 0;
        for (const char c : text.substr(start, count)) {
            if (c < '0' || c > '9')
                return false;
            value = value * 10 + (c - '0');
        }
        return true;
    };
    int year = 0;
    int month = 0;
    int day = 0;
    if (!digits(0, year_digits, year) || !digits(month_start, 2, month) ||
        !digits(day_start, 2, day))
        return false;
    if (month < 1 || month > 12 || day < 1)
        return false;
    constexpr std::array<int, 12> month_days{31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    const bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    const int days = month == 2 && leap ? 29 : month_days[static_cast<size_t>(month - 1)];
    return day <= days;
}

/// The first code point that is not a C0 control character.
constexpr char32_t first_printable = 0x20;
/// The delete character, and the C1 control characters U+0080 to U+009F.
constexpr char32_t delete_character = 0x7F;
constexpr char32_t last_c1_control = 0x9F;
/// The line separator and the paragraph separator.
constexpr char32_t line_separator = 0x2028;
constexpr char32_t paragraph_separator = 0x2029;
/// The payload bits of a UTF-8 continuation byte, and of the lead bytes.
constexpr char32_t continuation_bits = 0x3F;
constexpr char32_t two_byte_lead_bits = 0x1F;
constexpr char32_t three_byte_lead_bits = 0x0F;
constexpr char32_t four_byte_lead_bits = 0x07;
/// The lead-byte thresholds of two, three and four byte sequences.
constexpr unsigned char first_lead = 0xC0;
constexpr unsigned char first_three_byte_lead = 0xE0;
constexpr unsigned char first_four_byte_lead = 0xF0;
/// The payload bits each continuation byte adds.
constexpr unsigned continuation_shift = 6;

/// Decodes UTF-8 into its code points.
///
/// @param text valid UTF-8, as the profile reader accepts it
/// @return its code points
std::u32string code_points(std::string_view text) {
    std::u32string characters;
    size_t at = 0;
    while (at < text.size()) {
        const auto lead = static_cast<unsigned char>(text[at]);
        char32_t code_point = lead;
        size_t length = 1;
        if (lead >= first_four_byte_lead) {
            code_point = lead & four_byte_lead_bits;
            length = 4;
        } else if (lead >= first_three_byte_lead) {
            code_point = lead & three_byte_lead_bits;
            length = 3;
        } else if (lead >= first_lead) {
            code_point = lead & two_byte_lead_bits;
            length = 2;
        }
        for (size_t index = 1; index < length && at + index < text.size(); ++index)
            code_point = (code_point << continuation_shift) |
                         (static_cast<unsigned char>(text[at + index]) & continuation_bits);
        at += length;
        characters.push_back(code_point);
    }
    return characters;
}

/// Tells whether a character breaks a line of plain text: a control
/// character (C0, delete or C1), a line separator or a paragraph separator.
///
/// @param character the code point
/// @return true when a one-line text may not hold it
bool breaks_line(char32_t character) {
    return character < first_printable ||
           (character >= delete_character && character <= last_c1_control) ||
           character == line_separator || character == paragraph_separator;
}

/// Lower-cases ASCII letters.
///
/// @param text the text
/// @return the text, A-Z made a-z
std::string lower(std::string_view text) {
    std::string out{text};
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

/// The refusal when installation-archive names differ only in case.
///
/// Discovery matches an archive's name without case, so two spellings name
/// one file. Exact repeats are already refused by the value's distinct rule.
///
/// @param value the list of names
/// @return the message, or empty when no two names fold together
std::string case_duplicate_archives(const Value& value) {
    if (value.kind != ValueKind::list)
        return {};
    for (size_t left = 0; left < value.items.size(); ++left) {
        if (value.items[left].kind != ValueKind::string)
            continue;
        const std::string folded = lower(value.items[left].text);
        for (size_t right = left + 1; right < value.items.size(); ++right) {
            if (value.items[right].kind == ValueKind::string &&
                lower(value.items[right].text) == folded)
                return "names that differ only in case name one archive";
        }
    }
    return {};
}

/// Removes white space at both ends.
///
/// @param text the text
/// @return the text without leading and trailing spaces, tabs and line breaks
std::string_view trim(std::string_view text) {
    const auto blank = [](char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
    };
    while (!text.empty() && blank(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && blank(text.back()))
        text.remove_suffix(1);
    return text;
}

/// Returns the block of the effective profile an entry kind's values sit in.
///
/// @param kind identity, layout, string or media
/// @return the block's key
std::string_view value_block(EntryKind kind) {
    switch (kind) {
    case EntryKind::identity:
        return "identity";
    case EntryKind::layout:
        return "layout";
    case EntryKind::string:
        return "strings";
    default:
        return "media";
    }
}

/// The value kinds of the four blocks of plain values, in the order they resolve.
constexpr std::array<EntryKind, 4> value_kinds{
    EntryKind::identity, EntryKind::layout, EntryKind::string, EntryKind::media
};

/// A limit or hack as the profile writes it.
struct WrittenEntry {
    const Entry* entry{};
    std::string key{};    ///< its key in the block
    bool off{};           ///< written false
    std::string preset{}; ///< the preset named; empty for none
    std::vector<std::optional<Value>>
        values{}; ///< the parameters written, by index within the entry
    TextPosition position{};
};

/// One mount as the profile writes it.
struct WrittenMount {
    int64_t index{};
    std::optional<std::string> extension{}; ///< nullopt for false
};

/// One settings binding as the profile writes it.
struct WrittenBinding {
    std::string path{};
    std::optional<std::pair<std::string, std::string>>
        source{}; ///< kind and name; nullopt for false
    TextPosition position{};
};

/// The release this build is, parsed once from OA_ENGINE_VERSION.
///
/// @return that release; 0.0.0 when the macro is not a version
formats::oamod::EngineVersion this_build_version() {
    static const formats::oamod::EngineVersion version = [] {
        if (const std::optional<formats::oamod::EngineVersion> parsed =
                formats::oamod::parse_engine_version(OA_ENGINE_VERSION))
            return *parsed;
        return formats::oamod::EngineVersion{};
    }();
    return version;
}

/// The entry and parameter a settings or match path names: ENTRY.PARAM,
/// or an entry with a shorthand parameter.
std::pair<const Entry*, int> bound_parameter(std::string_view path) {
    if (const Entry* entry = registry::find_entry(path); entry != nullptr && entry->shorthand >= 0)
        return {entry, entry->shorthand};
    const size_t dot = path.rfind('.');
    if (dot == std::string_view::npos)
        return {nullptr, -1};
    const Entry* entry = registry::find_entry(path.substr(0, dot));
    if (entry == nullptr || (entry->kind != EntryKind::limit && entry->kind != EntryKind::hack))
        return {nullptr, -1};
    return {entry, registry::find_parameter(*entry, path.substr(dot + 1))};
}

/// The registry-only checks of a match option: a known parameter, adjustable
/// per match, and a value of the right type inside its bounds.
///
/// @param entry the parameter's entry
/// @param index the parameter within the entry
/// @param raw the value, written as in a profile
/// @param where the diagnostic path
/// @param[out] normal the value's normal form when it fits
/// @return the message a match option would be refused with, or none
std::optional<std::string> registry_match_problem(
    const Entry& entry, int index, std::string_view raw, const std::string& where, Value& normal
) {
    const Parameter& parameter = registry::parameters_of(entry)[static_cast<size_t>(index)];
    if (parameter.adjustable != Adjustable::match) {
        return std::string{"the parameter is "} +
               (parameter.adjustable == Adjustable::fixed ? "fixed" : "install") + ", not match";
    }
    Node node{};
    formats::oamod::ReadError read_error{};
    const std::span<const uint8_t> bytes{reinterpret_cast<const uint8_t*>(raw.data()), raw.size()};
    if (!formats::oamod::read_value(bytes, node, read_error))
        return std::string{formats::oamod::rule_message(read_error.rule)};
    if (const std::optional<registry::Problem> problem =
            registry::check_value(parameter.value, value_from_node(node), where, normal))
        return problem->message;
    return std::nullopt;
}

/// A limit or hack being resolved.
struct ResolvedEntry {
    const Entry* entry{};
    bool limit{};
    std::string key{};
    std::vector<Value> values{};        ///< by index within the entry
    std::vector<std::string> sources{}; ///< which step set each value
};

/// One profile being resolved.
class Resolver {
  public:

    /// Starts resolving.
    ///
    /// @param source the profile's name for diagnostics
    /// @param options settings, match options and the unimplemented-hack choice
    /// @param result receives diagnostics
    Resolver(std::string_view source, const ResolveOptions& options, ResolveResult& result)
        : source_{source}, options_{options}, result_{result} {}

    /// Resolves a profile's tree.
    ///
    /// @param root the profile's top-level mapping
    void run(const Node& root) {
        normalise(root);
        if (!result_.errors.empty())
            return;
        resolve();
        if (!result_.errors.empty())
            return;
        finish();
    }

  private:

    // -------------------------------------------------------------- diagnostics

    void error(TextPosition position, std::string path, std::string message) {
        result_.errors.push_back(
            Diagnostic{std::string{source_}, position, std::move(path), std::move(message)}
        );
    }

    void warn(TextPosition position, std::string path, std::string message) {
        result_.warnings.push_back(
            Diagnostic{std::string{source_}, position, std::move(path), std::move(message)}
        );
    }

    void error(const registry::Problem& problem) {
        error(problem.position, problem.path, problem.message);
    }

    // -------------------------------------------------------------- checking the file

    /// The profile's id, for provenance: which file set a value.
    std::string profile_id_{};

    /// Checks one profile on its own and keeps what it writes.
    void normalise(const Node& root) {
        for (const Node& entry : root.children) {
            const std::string& key = entry.key.text;
            if (entry.key.kind != KeyKind::string ||
                std::find(top_keys.begin(), top_keys.end(), key) == top_keys.end())
                error(entry.key.position, key, "unknown top-level key '" + key + "'");
        }
        const Node* oamod = formats::oamod::find_entry(root, "oamod");
        int64_t grammar = 0;
        if (oamod == nullptr || oamod->kind != NodeKind::number ||
            !formats::oamod::integer_value(oamod->number, grammar) || grammar != grammar_version)
            error(oamod ? oamod->position : TextPosition{}, "oamod", "oamod must be 1");
        for (const std::string_view key : {"id", "name", "version"}) {
            const Node* value = formats::oamod::find_entry(root, key);
            if (value == nullptr || value->kind != NodeKind::string)
                error(
                    value ? value->position : TextPosition{},
                    std::string{key},
                    std::string{key} + " must be a string"
                );
        }
        if (const Node* id = formats::oamod::find_entry(root, "id");
            id != nullptr && id->kind == NodeKind::string) {
            profile_id_ = id->text;
            if (!kebab_case(id->text))
                error(id->position, "id", "id must be kebab-case");
        }
        check_description(root);
        check_requires(root);
        check_package_keys(root);
        check_content_needs(root);
        check_author(root);
        check_packaging(root);
        for (const Node& entry : root.children) {
            if (entry.key.kind == KeyKind::string &&
                std::find(meta_keys.begin(), meta_keys.end(), entry.key.text) != meta_keys.end())
                set_member(meta_, entry.key.text, value_from_node(entry));
        }
        for (const std::string_view block : map_blocks) {
            const Node* value = formats::oamod::find_entry(root, block);
            if (value != nullptr && value->kind != NodeKind::mapping)
                error(
                    value->position, std::string{block}, std::string{block} + " must be an object"
                );
        }
        for (const EntryKind kind : value_kinds)
            normalise_values(root, kind);
        normalise_entries(root, "limits");
        normalise_entries(root, "hacks");
        normalise_script_extensions(root);
        normalise_data_keys(root);
        normalise_settings(root);
    }

    /// Returns a block of the profile when it is a mapping.
    static const Node* block_of(const Node& root, std::string_view block) {
        const Node* value = formats::oamod::find_entry(root, block);
        return value != nullptr && value->kind == NodeKind::mapping ? value : nullptr;
    }

    /// Checks the profile's description, when it has one: a string of one
    /// line of plain text, at most max_description_characters characters.
    void check_description(const Node& root) {
        const Node* description = formats::oamod::find_entry(root, "description");
        if (description == nullptr)
            return;
        if (description->kind != NodeKind::string) {
            error(description->position, "description", "description must be a string");
            return;
        }
        const std::u32string characters = code_points(description->text);
        if (std::any_of(characters.begin(), characters.end(), breaks_line))
            error(
                description->position,
                "description",
                "description must be one line, without line breaks or control characters"
            );
        if (characters.size() > max_description_characters)
            error(
                description->position,
                "description",
                "description must be at most " + std::to_string(max_description_characters) +
                    " characters (it has " + std::to_string(characters.size()) + ")"
            );
    }

    void check_requires(const Node& root) {
        const Node* requires_node = formats::oamod::find_entry(root, "requires");
        if (requires_node == nullptr)
            return;
        bool shape = requires_node->kind == NodeKind::mapping;
        for (const Node& entry : requires_node->children) {
            shape = shape && entry.key.kind == KeyKind::string &&
                    (entry.key.text == "base" || entry.key.text == "catalogue" ||
                     entry.key.text == "engine");
        }
        if (!shape) {
            error(requires_node->position, "requires", "requires takes base, catalogue and engine");
            return;
        }
        const Node* base = formats::oamod::find_entry(*requires_node, "base");
        const Node* catalogue = formats::oamod::find_entry(*requires_node, "catalogue");
        int64_t catalogue_number = registry::table().catalogue;
        const bool base_ok =
            base == nullptr || (base->kind == NodeKind::string && base->text == base_game);
        const bool catalogue_ok =
            catalogue == nullptr ||
            (catalogue->kind == NodeKind::number &&
             formats::oamod::integer_value(catalogue->number, catalogue_number) &&
             catalogue_number == registry::table().catalogue);
        if (!base_ok || !catalogue_ok)
            error(
                requires_node->position,
                "requires",
                "requires " + value_text(value_from_node(*requires_node)) +
                    " does not match base " + std::string{base_game} + ", catalogue " +
                    std::to_string(registry::table().catalogue)
            );
    }

    /// Checks homepage, tags and requires.engine, and refuses an engine
    /// requirement this build does not meet.
    ///
    /// @param root the profile's top-level mapping
    void check_package_keys(const Node& root) {
        formats::oamod::PackageKeys keys;
        std::vector<formats::oamod::KeyProblem> problems;
        const Node* requires_node = formats::oamod::find_entry(root, "requires");
        const Node* requires_block =
            requires_node != nullptr && requires_node->kind == NodeKind::mapping ? requires_node
                                                                                 : nullptr;
        formats::oamod::read_package_keys(root, requires_block, keys, problems);
        for (const formats::oamod::KeyProblem& problem : problems)
            error(problem.position, problem.path, problem.message);
        if (keys.requires_engine.empty())
            return;
        const std::optional<formats::oamod::EngineRange> range =
            formats::oamod::parse_engine_range(keys.requires_engine);
        if (!range || formats::oamod::engine_range_met(*range, this_build_version()))
            return;
        const Node* engine = requires_block != nullptr
                                 ? formats::oamod::find_entry(*requires_block, "engine")
                                 : nullptr;
        error(
            engine != nullptr ? engine->position : TextPosition{},
            "requires.engine",
            "needs Open Annihilation " + formats::oamod::describe_engine_range(*range) +
                "; this is Open Annihilation " +
                formats::oamod::engine_version_text(this_build_version())
        );
    }

    /// Checks supportedGames and requiredExpansions. requires.base of ta-3.1c
    /// stays a constraint of its own; with supportedGames omitted it reads as
    /// totala, and it does not warn.
    ///
    /// @param root the profile's top-level mapping
    void check_content_needs(const Node& root) {
        const Node* requires_node = formats::oamod::find_entry(root, "requires");
        const Node* requires_block =
            requires_node != nullptr && requires_node->kind == NodeKind::mapping ? requires_node
                                                                                 : nullptr;
        std::vector<formats::oamod::KeyProblem> problems;
        oa::data::content_bundles::read_content_needs(
            root,
            requires_block,
            oa::data::content_bundles::GamesWhenOmitted::totala,
            content_needs_,
            problems
        );
        for (const formats::oamod::KeyProblem& problem : problems)
            error(problem.position, problem.path, problem.message);
    }

    /// Checks that a block is a mapping of known keys, and reports each other key.
    ///
    /// @param root the profile's top-level mapping
    /// @param block the block's key
    /// @param keys the keys the block may hold
    /// @param shape what the block must be, for the message when it is missing or not a mapping
    /// @return the block, or nullptr when it is missing or not a mapping
    template <size_t count>
    const Node* known_block(
        const Node& root,
        std::string_view block,
        const std::array<std::string_view, count>& keys,
        std::string_view shape
    ) {
        const Node* node = formats::oamod::find_entry(root, block);
        if (node == nullptr || node->kind != NodeKind::mapping) {
            error(
                node ? node->position : TextPosition{},
                std::string{block},
                std::string{block} + " must be " + std::string{shape}
            );
            return nullptr;
        }
        std::string have;
        for (const std::string_view key : keys)
            have += (have.empty() ? "" : ", ") + std::string{key};
        for (const Node& entry : node->children) {
            if (entry.key.kind != KeyKind::string ||
                std::find(keys.begin(), keys.end(), entry.key.text) == keys.end())
                error(
                    entry.key.position,
                    std::string{block} + "." + entry.key.text,
                    "unknown " + std::string{block} + " key '" + entry.key.text + "' (have [" +
                        have + "])"
                );
        }
        return node;
    }

    /// Checks one person's name in a block: a string of 1 to person_name_max_length bytes.
    ///
    /// @param block the block holding it
    /// @param path the name's path, such as author.name
    /// @param key its key in the block
    void check_person_name(const Node& block, const std::string& path, std::string_view key) {
        const Node* value = formats::oamod::find_entry(block, key);
        if (value == nullptr || value->kind != NodeKind::string || value->text.empty() ||
            value->text.size() > person_name_max_length)
            error(
                value ? value->position : block.position,
                path,
                path + " must be a string of 1 to " + std::to_string(person_name_max_length) +
                    " bytes"
            );
    }

    /// Checks the author block: a name, "unknown" when nobody is known, and
    /// an optional e-mail address.
    ///
    /// @param root the profile's top-level mapping
    void check_author(const Node& root) {
        const Node* author =
            known_block(root, "author", author_keys, "an object with a name and an optional email");
        if (author == nullptr)
            return;
        check_person_name(*author, "author.name", "name");
        const Node* email = formats::oamod::find_entry(*author, "email");
        if (email != nullptr && (email->kind != NodeKind::string || !email_address(email->text)))
            error(
                email->position,
                "author.email",
                "author.email must be an e-mail address such as name@example.com"
            );
    }

    /// Checks the packaging block: the package's revision, the day it was
    /// made and who made it, each required.
    ///
    /// @param root the profile's top-level mapping
    void check_packaging(const Node& root) {
        const Node* packaging = known_block(
            root, "packaging", packaging_keys, "an object with a revision, a date and a packager"
        );
        if (packaging == nullptr)
            return;
        const Node* revision = formats::oamod::find_entry(*packaging, "revision");
        int64_t number = 0;
        if (revision == nullptr || revision->kind != NodeKind::number ||
            !formats::oamod::integer_value(revision->number, number) ||
            number < packaging_revision_min || number > packaging_revision_max)
            error(
                revision ? revision->position : packaging->position,
                "packaging.revision",
                "packaging.revision must be an integer from " +
                    std::to_string(packaging_revision_min) + " to " +
                    std::to_string(packaging_revision_max)
            );
        const Node* date = formats::oamod::find_entry(*packaging, "date");
        if (date == nullptr || date->kind != NodeKind::string || !calendar_date(date->text))
            error(
                date ? date->position : packaging->position,
                "packaging.date",
                "packaging.date must be an ISO 8601 date, YYYY-MM-DD"
            );
        check_person_name(*packaging, "packaging.packager", "packager");
    }

    /// Finds the identity, layout, string or media entry of a profile key.
    static const Entry* value_entry(EntryKind kind, std::string_view key) {
        for (const Entry& entry : registry::table().entries) {
            if (entry.kind == kind && entry.key == key)
                return &entry;
        }
        return nullptr;
    }

    void normalise_values(const Node& root, EntryKind kind) {
        const std::string block{value_block(kind)};
        const Node* values = block_of(root, block);
        if (values == nullptr)
            return;
        auto& given = given_values_[static_cast<size_t>(kind)];
        for (const Node& entry : values->children) {
            // Layout, strings and media nest one level: directories: {units: unitsE}.
            if (entry.kind == NodeKind::mapping && kind != EntryKind::identity) {
                for (const Node& inner : entry.children)
                    check_given(kind, block, entry.key.text + "." + inner.key.text, inner, given);
            } else {
                check_given(kind, block, entry.key.text, entry, given);
            }
        }
    }

    void check_given(
        EntryKind kind,
        const std::string& block,
        const std::string& key,
        const Node& node,
        std::vector<std::pair<std::string, Value>>& given
    ) {
        const Entry* entry = value_entry(kind, key);
        const std::string path = block + "." + key;
        if (entry == nullptr) {
            error(node.key.position, path, "unknown key");
            return;
        }
        Value normal{};
        if (auto problem =
                registry::check_value(entry->value, value_from_node(node), path, normal)) {
            error(*problem);
            return;
        }
        // distinct compares the spellings as written. This key is matched
        // without case when the archives are sought, so folded repeats are
        // the same fault.
        if (key == "installation-archives") {
            if (const std::string message = case_duplicate_archives(value_from_node(node));
                !message.empty()) {
                error(node.position, path, message);
                return;
            }
        }
        given.emplace_back(key, value_from_node(node));
    }

    void normalise_entries(const Node& root, std::string_view block) {
        const Node* entries = block_of(root, block);
        if (entries == nullptr)
            return;
        const bool limits = block == "limits";
        for (const Node& node : entries->children) {
            const std::string& key = node.key.text;
            const std::string path = std::string{block} + "." + key;
            const Entry* entry = node.key.kind != KeyKind::string
                                     ? nullptr
                                     : registry::find_entry(limits ? "limits." + key : key);
            if (entry == nullptr || entry->kind != (limits ? EntryKind::limit : EntryKind::hack)) {
                error(node.key.position, path, limits ? "unknown limit" : "unknown hack");
                continue;
            }
            WrittenEntry written{};
            written.entry = entry;
            written.key = key;
            written.position = node.position;
            written.values.resize(entry->parameter_count);
            if (!read_entry_value(*entry, node, path, written))
                continue;
            if (!limits && !written.off && !entry->implemented) {
                const std::string message = "this engine does not implement the hack yet";
                if (options_.accept_unimplemented_hacks)
                    warn(node.key.position, path, message + "; accepted for development");
                else
                    error(node.key.position, path, message);
            }
            (limits ? written_limits_ : written_hacks_).push_back(std::move(written));
        }
    }

    /// Reads a limit's or hack's value: true, false, a mapping of a preset
    /// and parameters, or a shorthand scalar.
    bool read_entry_value(
        const Entry& entry, const Node& node, const std::string& path, WrittenEntry& written
    ) {
        const auto parameters = registry::parameters_of(entry);
        if (node.kind == NodeKind::boolean) {
            written.off = !node.boolean;
            return true;
        }
        if (node.kind == NodeKind::mapping) {
            bool ok = true;
            for (const Node& member : node.children) {
                const std::string& key = member.key.text;
                if (member.key.kind == KeyKind::string && key == "preset") {
                    bool known = false;
                    std::string names;
                    for (const auto& preset : registry::presets_of(entry)) {
                        known = known ||
                                (member.kind == NodeKind::string && preset.name == member.text);
                        names += (names.empty() ? "" : ", ") + std::string{preset.name};
                    }
                    if (!known) {
                        error(
                            member.position,
                            path,
                            "unknown preset " + value_text(value_from_node(member)) + " (have [" +
                                names + "])"
                        );
                        ok = false;
                        continue;
                    }
                    written.preset = member.text;
                    continue;
                }
                const int index =
                    member.key.kind == KeyKind::string ? registry::find_parameter(entry, key) : -1;
                if (index < 0) {
                    std::string names;
                    std::vector<std::string_view> sorted;
                    for (const Parameter& parameter : parameters)
                        sorted.push_back(parameter.name);
                    std::sort(sorted.begin(), sorted.end());
                    for (const auto name : sorted)
                        names += (names.empty() ? "" : ", ") + std::string{name};
                    error(
                        member.key.position,
                        path,
                        "unknown parameter '" + key + "' (have [" + names + "])"
                    );
                    ok = false;
                    continue;
                }
                Value normal{};
                if (auto problem = registry::check_value(
                        parameters[static_cast<size_t>(index)].value,
                        value_from_node(member),
                        path + "." + key,
                        normal
                    )) {
                    error(*problem);
                    ok = false;
                    continue;
                }
                written.values[static_cast<size_t>(index)] = std::move(normal);
            }
            return ok;
        }
        if (entry.shorthand >= 0 && node.kind != NodeKind::null_value) {
            Value normal{};
            if (auto problem = registry::check_value(
                    parameters[static_cast<size_t>(entry.shorthand)].value,
                    value_from_node(node),
                    path,
                    normal
                )) {
                error(*problem);
                return false;
            }
            written.values[static_cast<size_t>(entry.shorthand)] = std::move(normal);
            return true;
        }
        error(
            node.position,
            path,
            "expected true, false or an object, got " + value_text(value_from_node(node))
        );
        return false;
    }

    void normalise_script_extensions(const Node& root) {
        const Node* block = formats::oamod::find_entry(root, "script-extensions");
        if (block == nullptr)
            return;
        bool shape = block->kind == NodeKind::mapping;
        for (const Node& entry : block->children)
            shape = shape && entry.key.kind == KeyKind::string &&
                    (entry.key.text == "fidelity" || entry.key.text == "get" ||
                     entry.key.text == "set");
        if (!shape) {
            error(
                block->position,
                "script-extensions",
                "script-extensions takes fidelity, get and set"
            );
            return;
        }
        if (const Node* fidelity = formats::oamod::find_entry(*block, "fidelity")) {
            if (fidelity->kind != NodeKind::string ||
                (fidelity->text != "exact" && fidelity->text != "safe"))
                error(
                    fidelity->position,
                    "script-extensions.fidelity",
                    "script-extensions.fidelity must be exact or safe"
                );
            else
                fidelity_ = fidelity->text;
        }
        for (const std::string_view direction : {"get", "set"}) {
            const Node* mounts = formats::oamod::find_entry(*block, direction);
            if (mounts != nullptr)
                read_mounts(direction, *mounts);
        }
    }

    void read_mounts(std::string_view direction, const Node& node) {
        const std::string path = "script-extensions." + std::string{direction};
        const auto& table = registry::table();
        std::vector<std::pair<WrittenMount, TextPosition>> items;
        if (node.kind == NodeKind::null_value) {
            // An empty block mounts nothing.
        } else if (node.kind == NodeKind::sequence) {
            for (const Node& item : node.children) {
                const Entry* entry =
                    item.kind == NodeKind::string ? registry::find_entry(item.text) : nullptr;
                if (entry == nullptr || entry->kind != EntryKind::script_extension) {
                    error(
                        item.position,
                        path,
                        "unknown script extension " + value_text(value_from_node(item))
                    );
                    return;
                }
                items.push_back({WrittenMount{entry->default_index, item.text}, item.position});
            }
        } else if (node.kind == NodeKind::mapping) {
            for (const Node& item : node.children) {
                int64_t index = 0;
                if (item.key.kind == KeyKind::integer) {
                    index = item.key.integer;
                } else {
                    Number number{};
                    const std::string& text = item.key.text;
                    if (text.empty() || text.front() == '-' ||
                        formats::oamod::parse_number(text, number) !=
                            formats::oamod::NumberStatus::ok ||
                        !number.integer || !formats::oamod::integer_value(number, index)) {
                        error(
                            item.key.position, path, "index '" + text + "' is not a decimal integer"
                        );
                        return;
                    }
                }
                std::optional<std::string> extension;
                if (item.kind == NodeKind::boolean && !item.boolean)
                    extension = std::nullopt;
                else
                    extension = item.kind == NodeKind::string ? item.text
                                                              : value_text(value_from_node(item));
                if (!(item.kind == NodeKind::boolean && !item.boolean) &&
                    item.kind != NodeKind::string) {
                    error(
                        item.position,
                        path + "." + std::to_string(index),
                        "unknown script extension " + value_text(value_from_node(item))
                    );
                    return;
                }
                items.push_back({WrittenMount{index, extension}, item.position});
            }
        } else {
            error(node.position, path, "expected a list of extension ids or an index map");
            return;
        }
        auto& mounts = direction == "get" ? get_mounts_ : set_mounts_;
        mounts.emplace();
        std::set<int64_t> seen;
        for (const auto& [mount, position] : items) {
            if (!seen.insert(mount.index).second) {
                error(position, path, "index " + std::to_string(mount.index) + " is mounted twice");
                return;
            }
            if (mount.index < table.script_index_minimum ||
                mount.index > table.script_index_maximum ||
                (mount.index >= table.base_index_first && mount.index <= table.base_index_last)) {
                error(
                    position,
                    path,
                    "index " + std::to_string(mount.index) + " is outside " +
                        std::to_string(table.script_index_minimum) + "-" +
                        std::to_string(table.script_index_maximum) + " or inside the 3.1c range [" +
                        std::to_string(table.base_index_first) + ", " +
                        std::to_string(table.base_index_last) + "]"
                );
                return;
            }
            if (mount.extension) {
                const Entry* entry = registry::find_entry(*mount.extension);
                const std::string where = path + "." + std::to_string(mount.index);
                if (entry == nullptr || entry->kind != EntryKind::script_extension) {
                    error(position, where, "unknown script extension '" + *mount.extension + "'");
                    return;
                }
                const bool get = entry->direction == registry::ScriptDirection::get;
                if ((direction == "get") != get) {
                    error(
                        position,
                        where,
                        *mount.extension + " is a " + (get ? "get" : "set") + " extension"
                    );
                    return;
                }
            }
            mounts->push_back(mount);
        }
    }

    void normalise_data_keys(const Node& root) {
        const Node* block = block_of(root, "data-keys");
        if (block == nullptr)
            return;
        for (const Node& file : block->children) {
            const std::string& name = file.key.text;
            const std::string path = "data-keys." + name;
            if (file.key.kind != KeyKind::string || (name != "unit" && name != "weapon") ||
                file.kind != NodeKind::mapping) {
                error(file.key.position, path, "expected unit or weapon key maps");
                continue;
            }
            auto& keys = name == "unit" ? unit_keys_ : weapon_keys_;
            for (const Node& key : file.children) {
                const std::string where = path + "." + key.key.text;
                if (key.key.kind != KeyKind::string) {
                    error(key.key.position, where, "a data key must be a string");
                    continue;
                }
                if (key.kind == NodeKind::boolean && !key.boolean) {
                    keys.emplace_back(key.key.text, std::nullopt);
                    continue;
                }
                const Entry* entry =
                    key.kind == NodeKind::string ? registry::find_entry(key.text) : nullptr;
                const registry::DataFile want =
                    name == "unit" ? registry::DataFile::unit : registry::DataFile::weapon;
                if (entry == nullptr || entry->kind != EntryKind::data_key || entry->file != want) {
                    error(
                        key.position,
                        where,
                        value_text(value_from_node(key)) + " is not a " + name + " data-key meaning"
                    );
                    continue;
                }
                keys.emplace_back(key.key.text, key.text);
            }
        }
    }

    /// The entry and parameter a settings or match path names: ENTRY.PARAM,
    /// or an entry with a shorthand parameter.
    static std::pair<const Entry*, int> binding_target(std::string_view path) {
        return bound_parameter(path);
    }

    void normalise_settings(const Node& root) {
        const Node* block = block_of(root, "settings");
        if (block == nullptr)
            return;
        for (const Node& binding : block->children) {
            const std::string& path = binding.key.text;
            const std::string where = "settings." + path;
            if (binding.key.kind == KeyKind::string && path == seeds_key) {
                bool ok = binding.kind == NodeKind::mapping;
                for (const Node& seed : binding.children) {
                    ok = ok && seed.key.kind == KeyKind::string &&
                         (seed.kind == NodeKind::string ||
                          (seed.kind == NodeKind::number && seed.number.integer));
                }
                if (!ok) {
                    error(
                        binding.position,
                        where,
                        "settings.registry-seeds maps registry names to integers or strings"
                    );
                    continue;
                }
                for (const Node& seed : binding.children) {
                    RegistrySeed made{seed.key.text, seed.text, seed.kind == NodeKind::number};
                    if (made.integer)
                        made.value = formats::oamod::canonical_number_text(seed.number);
                    seeds_.push_back(std::move(made));
                }
                seeds_value_ = value_from_node(binding);
                continue;
            }
            const auto [entry, parameter] =
                binding.key.kind == KeyKind::string ? binding_target(path) : std::pair{nullptr, -1};
            if (entry == nullptr || parameter < 0) {
                error(binding.key.position, where, "unknown parameter");
            } else if (
                registry::parameters_of(*entry)[static_cast<size_t>(parameter)].adjustable ==
                Adjustable::fixed
            ) {
                error(binding.key.position, where, "the parameter is fixed, not install or match");
            }
            WrittenBinding written{path, std::nullopt, binding.position};
            if (binding.kind == NodeKind::boolean && !binding.boolean) {
                bindings_.push_back(std::move(written));
                continue;
            }
            const bool shape = binding.kind == NodeKind::mapping && binding.children.size() == 1 &&
                               binding.children[0].key.kind == KeyKind::string &&
                               (binding.children[0].key.text == "ini" ||
                                binding.children[0].key.text == "registry") &&
                               binding.children[0].kind == NodeKind::string;
            if (!shape) {
                error(binding.position, where, "expected {ini: Section/Key} or {registry: Name}");
                continue;
            }
            const Node& source = binding.children[0];
            if (source.key.text == "ini" && source.text.find('/') == std::string::npos) {
                error(source.position, where, "ini binding must be Section/Key");
                continue;
            }
            written.source = std::pair{source.key.text, source.text};
            bindings_.push_back(std::move(written));
        }
    }

    // -------------------------------------------------------------- resolving

    void resolve() {
        resolve_values();
        resolve_entries();
        apply_default_from();
        check_constraints("profile");
        apply_settings();
        apply_overrides();
        apply_match_options();
        apply_default_from();
        apply_clamps();
        check_constraints("final");
    }

    void resolve_values() {
        for (const EntryKind kind : value_kinds) {
            const auto& given = given_values_[static_cast<size_t>(kind)];
            const std::string block{value_block(kind)};
            Value out = make_map();
            for (const Entry& entry : registry::table().entries) {
                if (entry.kind != kind)
                    continue;
                const Value* written = nullptr;
                for (const auto& [key, value] : given) {
                    if (key == entry.key)
                        written = &value;
                }
                provenance_[block + "." + std::string{entry.key}] =
                    written ? "profile" : "baseline";
                Value value = written ? *written : registry::literal_value(entry.value.baseline);
                // A dotted key nests: directories.units sits under directories.
                Value* node = &out;
                std::string_view key = entry.key;
                for (size_t dot = key.find('.'); dot != std::string_view::npos;
                     dot = key.find('.')) {
                    Value* inner = find_member(*node, key.substr(0, dot));
                    if (inner == nullptr)
                        inner = &set_member(*node, key.substr(0, dot), make_map());
                    node = inner;
                    key.remove_prefix(dot + 1);
                }
                set_member(*node, key, std::move(value));
            }
            set_member(effective_value_blocks_, block, std::move(out));
        }
    }

    void resolve_entries() {
        for (const Entry& entry : registry::table().entries) {
            if (entry.kind != EntryKind::limit)
                continue;
            const std::string key{entry.id.substr(std::string_view{"limits."}.size())};
            const WrittenEntry* written = nullptr;
            for (const WrittenEntry& candidate : written_limits_) {
                if (candidate.entry == &entry)
                    written = &candidate;
            }
            resolve_entry(entry, true, key, written);
        }
        for (const WrittenEntry& written : written_hacks_) {
            if (!written.off)
                resolve_entry(*written.entry, false, written.key, &written);
        }
    }

    void resolve_entry(
        const Entry& entry, bool limit, const std::string& key, const WrittenEntry* written
    ) {
        const auto parameters = registry::parameters_of(entry);
        ResolvedEntry resolved{&entry, limit, key, {}, {}};
        const bool on = written != nullptr && !written->off;
        for (const Parameter& parameter : parameters) {
            resolved.values.push_back(
                registry::literal_value(on ? parameter.default_value : parameter.value.baseline)
            );
            resolved.sources.emplace_back(on ? "default" : "baseline");
        }
        if (on) {
            present_.insert(std::string{entry.id});
            if (!written->preset.empty()) {
                for (const auto& preset : registry::presets_of(entry)) {
                    if (preset.name != written->preset)
                        continue;
                    const auto values = registry::table().preset_values.subspan(
                        preset.first_value, preset.value_count
                    );
                    for (const auto& value : values) {
                        resolved.values[value.parameter] = registry::literal_value(value.literal);
                        resolved.sources[value.parameter] =
                            "preset " + written->preset + " (" + profile_id_ + ")";
                    }
                }
            }
            for (size_t index = 0; index < written->values.size(); ++index) {
                if (written->values[index]) {
                    resolved.values[index] = *written->values[index];
                    resolved.sources[index] = profile_id_;
                }
            }
        }
        resolved_.push_back(std::move(resolved));
    }

    ResolvedEntry* find_resolved(const Entry* entry) {
        for (ResolvedEntry& resolved : resolved_) {
            if (resolved.entry == entry)
                return &resolved;
        }
        return nullptr;
    }

    void apply_default_from() {
        for (ResolvedEntry& resolved : resolved_) {
            const auto parameters = registry::parameters_of(*resolved.entry);
            for (size_t index = 0; index < parameters.size(); ++index) {
                const Parameter& parameter = parameters[index];
                if (parameter.default_from < 0 || (resolved.sources[index] != "default" &&
                                                   resolved.sources[index] != "default-from"))
                    continue;
                const Value& from = resolved.values[static_cast<size_t>(parameter.default_from)];
                int64_t base = 0;
                int64_t factor = 0;
                if (!formats::oamod::integer_value(from.number, base) ||
                    !formats::oamod::integer_value(parameter.default_factor, factor)) {
                    error(
                        {},
                        std::string{resolved.entry->id} + "." + std::string{parameter.name},
                        "default-from needs whole numbers"
                    );
                    continue;
                }
                Value product = make_integer(base * factor);
                Value normal{};
                if (auto problem = registry::check_value(
                        parameter.value,
                        product,
                        std::string{resolved.entry->id} + "." + std::string{parameter.name},
                        normal
                    )) {
                    error(*problem);
                    continue;
                }
                resolved.values[index] = normal;
                resolved.sources[index] =
                    values_equal(normal, registry::literal_value(parameter.default_value))
                        ? "default"
                        : "default-from";
            }
        }
    }

    static bool
    constraint_holds(const registry::Constraint& constraint, const ResolvedEntry& resolved) {
        const Value& left = resolved.values[constraint.left];
        const Value& right = resolved.values[constraint.right];
        if ((left.kind == ValueKind::string && left.text == "none") ||
            (right.kind == ValueKind::string && right.text == "none"))
            return true;
        const int order = formats::oamod::compare_numbers(left.number, right.number);
        switch (constraint.comparison) {
        case registry::Comparison::less_equal:
            return order <= 0;
        case registry::Comparison::less:
            return order < 0;
        case registry::Comparison::greater_equal:
            return order >= 0;
        case registry::Comparison::greater:
            return order > 0;
        }
        return false;
    }

    static std::string values_text(const ResolvedEntry& resolved) {
        Value map = make_map();
        const auto parameters = registry::parameters_of(*resolved.entry);
        for (size_t index = 0; index < parameters.size(); ++index)
            set_member(map, parameters[index].name, resolved.values[index]);
        return value_text(map);
    }

    void check_constraints(std::string_view stage) {
        for (const ResolvedEntry& resolved : resolved_) {
            for (const auto& constraint : registry::constraints_of(*resolved.entry)) {
                if (!constraint_holds(constraint, resolved))
                    error(
                        {},
                        std::string{resolved.entry->id},
                        std::string{stage} + " values break " + std::string{constraint.text} +
                            ": " + values_text(resolved)
                    );
            }
        }
    }

    /// Converts a setting's raw text into a parameter's value.
    std::optional<Value>
    coerce_setting(const Parameter& parameter, std::string_view raw, const std::string& where) {
        const std::string text{trim(raw)};
        const ValueType type = parameter.value.type;
        if (type == ValueType::boolean) {
            const std::string folded = lower(text);
            if (folded == "1" || folded == "true" || folded == "yes" || folded == "on")
                return make_boolean(true);
            if (folded == "0" || folded == "false" || folded == "no" || folded == "off")
                return make_boolean(false);
            warn({}, where, "'" + std::string{raw} + "' is not a boolean; ignored");
            return std::nullopt;
        }
        if (type == ValueType::integer || type == ValueType::integer_or_none ||
            type == ValueType::decimal) {
            const bool decimal = type == ValueType::decimal;
            Number number{};
            const bool exponent = text.find_first_of("eE") != std::string::npos;
            auto status = exponent ? formats::oamod::NumberStatus::not_a_number
                                   : formats::oamod::parse_number(text, number, true);
            if (status == formats::oamod::NumberStatus::integer_too_large) {
                // Beyond every bound the registry sets; clamping brings it back.
                number = formats::oamod::number_from_integer(
                    text.front() == '-'
                        ? -static_cast<int64_t>(formats::oamod::max_integer_magnitude)
                        : static_cast<int64_t>(formats::oamod::max_integer_magnitude)
                );
                status = formats::oamod::NumberStatus::ok;
            }
            if (status != formats::oamod::NumberStatus::ok || (!decimal && !number.integer)) {
                warn({}, where, "'" + std::string{raw} + "' is not a decimal number; ignored");
                return std::nullopt;
            }
            number.integer = !decimal;
            if (parameter.transform == registry::SettingTransform::unit_type_bits) {
                int64_t value = 0;
                if (formats::oamod::integer_value(number, value))
                    number = formats::oamod::number_from_integer(
                        value <= unit_type_step
                            ? unit_type_step
                            : ((value >> unit_type_step_shift) + 1) * unit_type_step
                    );
            }
            return make_number(number);
        }
        if (type == ValueType::enumeration) {
            const std::string folded = lower(text);
            for (const std::string_view value : registry::enum_values_of(parameter.value)) {
                if (value == folded)
                    return make_string(folded);
            }
            warn({}, where, "'" + std::string{raw} + "' is not one of the values; ignored");
            return std::nullopt;
        }
        if (type == ValueType::string)
            return make_string(text);
        warn({}, where, "list parameters cannot be bound to a setting; ignored");
        return std::nullopt;
    }

    /// Clamps a number into a parameter's bounds.
    static Value clamp(const ValueSpec& spec, Value value) {
        if (value.kind != ValueKind::number)
            return value;
        const bool integer = value.number.integer;
        if (spec.has_minimum && formats::oamod::compare_numbers(value.number, spec.minimum) < 0)
            value.number = spec.minimum;
        if (spec.has_maximum && formats::oamod::compare_numbers(value.number, spec.maximum) > 0)
            value.number = spec.maximum;
        value.number.integer = integer;
        return value;
    }

    void apply_settings() {
        std::map<std::string, std::string> ini;
        for (const SettingValue& value : options_.settings.ini)
            ini[lower(value.name)] = value.value;
        std::map<std::string, std::string> registry_values;
        for (const RegistrySeed& seed : seeds_)
            registry_values[lower(seed.name)] = seed.value;
        for (const SettingValue& value : options_.settings.registry)
            registry_values[lower(value.name)] = value.value;
        std::vector<const WrittenBinding*> sorted;
        for (const WrittenBinding& binding : bindings_) {
            if (binding.source)
                sorted.push_back(&binding);
        }
        std::sort(sorted.begin(), sorted.end(), [](const auto* left, const auto* right) {
            return left->path < right->path;
        });
        for (const WrittenBinding* binding : sorted) {
            const auto [entry, index] = binding_target(binding->path);
            const std::string where = "settings." + binding->path;
            if (entry == nullptr || index < 0)
                continue;
            if (!present_.contains(std::string{entry->id})) {
                warn(
                    binding->position,
                    where,
                    std::string{entry->id} + " is not in the profile; binding ignored"
                );
                continue;
            }
            const auto& [kind, name] = *binding->source;
            const auto& values = kind == "ini" ? ini : registry_values;
            const auto found = values.find(lower(name));
            if (found == values.end())
                continue;
            const Parameter& parameter =
                registry::parameters_of(*entry)[static_cast<size_t>(index)];
            const std::string label = where + " (" + kind + " " + name + ")";
            auto coerced = coerce_setting(parameter, found->second, label);
            if (!coerced)
                continue;
            Value clamped = clamp(parameter.value, *coerced);
            if (!values_equal(clamped, *coerced))
                warn(
                    binding->position,
                    where,
                    value_text(*coerced) + " clamped to " + value_text(clamped)
                );
            Value normal{};
            if (auto problem = registry::check_value(parameter.value, clamped, where, normal)) {
                error(*problem);
                continue;
            }
            ResolvedEntry* resolved = find_resolved(entry);
            resolved->values[static_cast<size_t>(index)] = normal;
            resolved->sources[static_cast<size_t>(index)] = "settings " + kind + ":" + name;
        }
    }

    /// Puts an entry that was off at the registry's defaults and marks it present.
    ///
    /// A limit is already in the resolved list at its baseline; that row is
    /// replaced, not duplicated. A hack that is off is not in the list yet.
    void turn_entry_on(const Entry& entry) {
        const auto parameters = registry::parameters_of(entry);
        ResolvedEntry* existing = find_resolved(&entry);
        if (existing == nullptr) {
            const bool limit = entry.kind == EntryKind::limit;
            std::string key = limit
                                  ? std::string{entry.id.substr(std::string_view{"limits."}.size())}
                                  : std::string{entry.id};
            ResolvedEntry made{&entry, limit, std::move(key), {}, {}};
            for (const Parameter& parameter : parameters) {
                made.values.push_back(registry::literal_value(parameter.default_value));
                made.sources.emplace_back("default");
            }
            resolved_.push_back(std::move(made));
        } else {
            existing->values.clear();
            existing->sources.clear();
            existing->values.reserve(parameters.size());
            existing->sources.reserve(parameters.size());
            for (const Parameter& parameter : parameters) {
                existing->values.push_back(registry::literal_value(parameter.default_value));
                existing->sources.emplace_back("default");
            }
        }
        present_.insert(std::string{entry.id});
    }

    void apply_match_options() {
        for (const MatchOption& option : options_.match) {
            const auto [entry, index] = binding_target(option.path);
            const std::string where = "match " + option.path;
            if (entry == nullptr || index < 0) {
                error({}, where, "unknown parameter");
                continue;
            }
            if (option.turn_on && !entry->mutator) {
                error({}, where, std::string{entry->id} + " is not marked mutator: true");
                continue;
            }
            Value normal{};
            if (const std::optional<std::string> problem =
                    registry_match_problem(*entry, index, option.value, where, normal)) {
                error({}, where, *problem);
                continue;
            }
            const bool present = present_.contains(std::string{entry->id});
            if (!present && !option.turn_on) {
                error({}, where, std::string{entry->id} + " is not in the profile");
                continue;
            }
            if (!present)
                turn_entry_on(*entry);
            normal.position = {};
            ResolvedEntry* resolved = find_resolved(entry);
            resolved->values[static_cast<size_t>(index)] = normal;
            std::string provenance = "match";
            if (!option.source.empty())
                provenance += " " + option.source;
            if (!present)
                provenance += " (turned on)";
            resolved->sources[static_cast<size_t>(index)] = std::move(provenance);
        }
    }

    /// Tells whether a data key the profile maps needs a hack on.
    ///
    /// @param hack the hack's id
    /// @return the first such key's path, as data-keys.FILE.KEY; empty for none
    std::string data_key_needing(std::string_view hack) const {
        for (const std::string_view file : {"unit", "weapon"}) {
            const auto& written = file == "unit" ? unit_keys_ : weapon_keys_;
            for (const auto& [key, meaning] : written) {
                if (!meaning)
                    continue;
                const Entry* entry = registry::find_entry(*meaning);
                if (entry != nullptr && entry->required_hack == hack)
                    return "data-keys." + std::string{file} + "." + key;
            }
        }
        return {};
    }

    /// Lays the player's overrides of standard hacks over the resolved
    /// values, each checked as a profile's hack is; one that does not fit
    /// is left out whole, with a warning.
    void apply_overrides() {
        for (const HackOverride& override : options_.overrides) {
            const std::string path = "hacks." + override.hack;
            const auto refuse = [&](const std::string& where, const std::string& message) {
                warn({}, where, message + "; the override is left out");
            };
            const Entry* entry = registry::find_entry(override.hack);
            if (entry == nullptr || entry->kind != EntryKind::hack) {
                refuse(path, "unknown hack");
                continue;
            }
            const auto at = std::find_if(resolved_.begin(), resolved_.end(), [&](const auto& item) {
                return item.entry == entry;
            });
            if (!override.on) {
                if (const std::string key = data_key_needing(entry->id); !key.empty()) {
                    refuse(path, key + " needs the hack on");
                    continue;
                }
                if (at != resolved_.end())
                    resolved_.erase(at);
                present_.erase(std::string{entry->id});
                continue;
            }
            if (!entry->implemented && !options_.accept_unimplemented_hacks) {
                refuse(path, "this engine does not implement the hack yet");
                continue;
            }
            const auto parameters = registry::parameters_of(*entry);
            ResolvedEntry candidate{};
            if (at != resolved_.end()) {
                candidate = *at;
            } else {
                candidate = ResolvedEntry{entry, false, std::string{entry->id}, {}, {}};
                for (const Parameter& parameter : parameters) {
                    candidate.values.push_back(registry::literal_value(parameter.default_value));
                    candidate.sources.emplace_back("default");
                }
            }
            bool fits = true;
            for (const ParameterOverride& parameter : override.parameters) {
                const std::string where = path + "." + parameter.name;
                const int index = registry::find_parameter(*entry, parameter.name);
                if (index < 0) {
                    refuse(where, "unknown parameter");
                    fits = false;
                    break;
                }
                Value normal{};
                if (auto problem = registry::check_value(
                        parameters[static_cast<size_t>(index)].value, parameter.value, where, normal
                    )) {
                    refuse(problem->path, problem->message);
                    fits = false;
                    break;
                }
                normal.position = {};
                candidate.values[static_cast<size_t>(index)] = std::move(normal);
                candidate.sources[static_cast<size_t>(index)] = "override";
            }
            if (!fits)
                continue;
            for (const auto& constraint : registry::constraints_of(*entry)) {
                if (!constraint_holds(constraint, candidate)) {
                    refuse(
                        path,
                        "the values break " + std::string{constraint.text} + ": " +
                            values_text(candidate)
                    );
                    fits = false;
                    break;
                }
            }
            if (!fits)
                continue;
            if (at != resolved_.end())
                *at = std::move(candidate);
            else
                resolved_.push_back(std::move(candidate));
            present_.insert(std::string{entry->id});
        }
    }

    void apply_clamps() {
        for (ResolvedEntry& resolved : resolved_) {
            const auto parameters = registry::parameters_of(*resolved.entry);
            for (size_t index = 0; index < parameters.size(); ++index) {
                const Parameter& parameter = parameters[index];
                const std::string& source = resolved.sources[index];
                if (parameter.clamp_low < 0 ||
                    !(source.starts_with("settings") || source.starts_with("match")))
                    continue;
                Value& value = resolved.values[index];
                const Value& low = resolved.values[static_cast<size_t>(parameter.clamp_low)];
                const Value& high = resolved.values[static_cast<size_t>(parameter.clamp_high)];
                Value clamped = value;
                if (formats::oamod::compare_numbers(clamped.number, low.number) < 0)
                    clamped.number = low.number;
                if (formats::oamod::compare_numbers(clamped.number, high.number) > 0)
                    clamped.number = high.number;
                clamped.number.integer = value.number.integer;
                if (!values_equal(clamped, value)) {
                    warn(
                        {},
                        std::string{resolved.entry->id} + "." + std::string{parameter.name},
                        value_text(value) + " clamped to [" +
                            std::string{parameters[static_cast<size_t>(parameter.clamp_low)].name} +
                            ", " +
                            std::string{
                                parameters[static_cast<size_t>(parameter.clamp_high)].name
                            } +
                            "] = " + value_text(clamped)
                    );
                    value = clamped;
                }
            }
        }
    }

    // -------------------------------------------------------------- the effective profile

    void finish() {
        Value effective = make_map();
        for (const Member& member : meta_.members)
            set_member(effective, member.key, member.value);
        for (const Member& member : effective_value_blocks_.members)
            set_member(effective, member.key, member.value);
        Value limits = make_map();
        Value hacks = make_map();
        for (const ResolvedEntry& resolved : resolved_) {
            Value values = make_map();
            const auto parameters = registry::parameters_of(*resolved.entry);
            const std::string block = resolved.limit ? "limits" : "hacks";
            for (size_t index = 0; index < parameters.size(); ++index) {
                Value value = resolved.values[index];
                value.position = {};
                set_member(values, parameters[index].name, std::move(value));
                provenance_
                    [block + "." + resolved.key + "." + std::string{parameters[index].name}] =
                        resolved.sources[index];
            }
            set_member(resolved.limit ? limits : hacks, resolved.key, std::move(values));
        }
        set_member(effective, "limits", std::move(limits));
        set_member(effective, "hacks", std::move(hacks));

        const auto& table = registry::table();
        Value extensions = make_map();
        set_member(
            extensions,
            "fidelity",
            make_string(fidelity_.empty() ? table.default_fidelity : fidelity_)
        );
        for (const std::string_view direction : {"get", "set"}) {
            const auto& written = direction == "get" ? get_mounts_ : set_mounts_;
            std::vector<WrittenMount> mounted;
            if (written) {
                for (const WrittenMount& mount : *written) {
                    if (mount.extension)
                        mounted.push_back(mount);
                }
            }
            std::sort(mounted.begin(), mounted.end(), [](const auto& left, const auto& right) {
                return left.index < right.index;
            });
            Value map = make_map();
            std::map<std::string, int64_t> seen;
            for (const WrittenMount& mount : mounted) {
                if (const auto found = seen.find(*mount.extension); found != seen.end())
                    error(
                        {},
                        "script-extensions." + std::string{direction},
                        *mount.extension + " is mounted at " + std::to_string(found->second) +
                            " and " + std::to_string(mount.index)
                    );
                seen[*mount.extension] = mount.index;
                set_member(map, std::to_string(mount.index), make_string(*mount.extension));
            }
            set_member(extensions, direction, std::move(map));
        }
        set_member(effective, "script-extensions", std::move(extensions));

        Value data_keys = make_map();
        std::map<std::string, std::string> per_type_keys;
        for (const std::string_view file : {"unit", "weapon"}) {
            const auto& written = file == "unit" ? unit_keys_ : weapon_keys_;
            Value map = make_map();
            for (const auto& [key, meaning] : written) {
                if (!meaning)
                    continue;
                set_member(map, key, make_string(*meaning));
                per_type_keys[*meaning] = key;
                const Entry* entry = registry::find_entry(*meaning);
                if (entry != nullptr && !entry->required_hack.empty() &&
                    find_member(*find_member(effective, "hacks"), entry->required_hack) == nullptr)
                    error(
                        {},
                        "data-keys." + std::string{file} + "." + key,
                        *meaning + " needs hack " + std::string{entry->required_hack} +
                            ", which is off"
                    );
            }
            set_member(data_keys, file, std::move(map));
        }
        set_member(effective, "data-keys", std::move(data_keys));
        for (const ResolvedEntry& resolved : resolved_) {
            const auto parameters = registry::parameters_of(*resolved.entry);
            for (const Parameter& parameter : parameters) {
                if (parameter.per_type_key.empty())
                    continue;
                const std::string path = std::string{resolved.limit ? "limits." : "hacks."} +
                                         resolved.key + "." + std::string{parameter.name};
                const auto found = per_type_keys.find(std::string{parameter.per_type_key});
                provenance_[path] += found != per_type_keys.end()
                                         ? "; per-unit key " + found->second
                                         : "; per-unit meaning " +
                                               std::string{parameter.per_type_key} + " not mapped";
            }
        }

        Value settings = make_map();
        Value bindings = make_map();
        std::vector<const WrittenBinding*> sorted;
        for (const WrittenBinding& binding : bindings_) {
            if (binding.source)
                sorted.push_back(&binding);
        }
        std::sort(sorted.begin(), sorted.end(), [](const auto* left, const auto* right) {
            return left->path < right->path;
        });
        for (const WrittenBinding* binding : sorted) {
            Value source = make_map();
            set_member(source, binding->source->first, make_string(binding->source->second));
            set_member(bindings, binding->path, std::move(source));
        }
        set_member(settings, "bindings", std::move(bindings));
        Value seeds = seeds_value_.kind == ValueKind::map ? seeds_value_ : make_map();
        set_member(settings, seeds_key, std::move(seeds));
        set_member(effective, "settings", std::move(settings));
        if (!result_.errors.empty())
            return;
        strip_positions(effective);

        // The hashes read a copy with the keys added after grammar 1's first
        // release left out at their baselines, or the effective profile
        // itself when there is nothing to leave out. The effective profile still holds the list, and
        // --print-profile prints the effective profile.
        const std::optional<Value> stripped = canonical_source(effective);
        const Value& hashed = stripped ? *stripped : effective;
        Resolution resolution{};
        resolution.sim = sim_projection(hashed);
        resolution.canonical = canonical_json(hashed);
        const std::string sim_canonical = canonical_json(resolution.sim);
        resolution.profile.full_hash = base::sha256::digest_of(
            std::span<const uint8_t>{
                reinterpret_cast<const uint8_t*>(resolution.canonical.data()),
                resolution.canonical.size()
            }
        );
        resolution.profile.sim_hash = base::sha256::digest_of(
            std::span<const uint8_t>{
                reinterpret_cast<const uint8_t*>(sim_canonical.data()), sim_canonical.size()
            }
        );
        for (const std::string_view key : {"id", "name", "version"}) {
            const Value* value = find_member(effective, key);
            std::string text = value != nullptr ? value->text : std::string{};
            (key == "id"     ? resolution.profile.id
             : key == "name" ? resolution.profile.name
                             : resolution.profile.version) = std::move(text);
        }
        const auto text_of = [](const Value* block, std::string_view key) {
            const Value* value = block != nullptr ? find_member(*block, key) : nullptr;
            return value != nullptr ? value->text : std::string{};
        };
        const Value* author = find_member(effective, "author");
        resolution.profile.author.name = text_of(author, "name");
        resolution.profile.author.email = text_of(author, "email");
        const Value* packaging = find_member(effective, "packaging");
        if (const Value* revision =
                packaging != nullptr ? find_member(*packaging, "revision") : nullptr;
            revision != nullptr)
            (void)formats::oamod::integer_value(
                revision->number, resolution.profile.packaging.revision
            );
        resolution.profile.packaging.date = text_of(packaging, "date");
        resolution.profile.packaging.packager = text_of(packaging, "packager");
        if (const Value* description = find_member(effective, "description");
            description != nullptr)
            resolution.profile.description = description->text;
        if (const Value* homepage = find_member(effective, "homepage");
            homepage != nullptr && homepage->kind == ValueKind::string)
            resolution.profile.homepage = homepage->text;
        if (const Value* tags = find_member(effective, "tags");
            tags != nullptr && tags->kind == ValueKind::list) {
            for (const Value& item : tags->items)
                if (item.kind == ValueKind::string)
                    resolution.profile.tags.push_back(item.text);
        }
        if (const Value* requires_block = find_member(effective, "requires");
            requires_block != nullptr && requires_block->kind == ValueKind::map) {
            if (const Value* engine = find_member(*requires_block, "engine");
                engine != nullptr && engine->kind == ValueKind::string)
                resolution.profile.requires_engine = engine->text;
        }
        resolution.profile.supported_games = content_needs_.supported_games;
        resolution.profile.required_expansions = content_needs_.required_expansions;
        resolution.profile.registry_seeds = seeds_;
        fill_records(effective, resolution.profile);
        // The registry's ranges and constraints keep every limit in the range
        // the engine's storage holds; a limit outside it would be a registry
        // that disagrees with the engine.
        if (const limits::LimitsError limits_error =
                limits::check_limits(resolution.profile.limits);
            limits_error != limits::LimitsError::none) {
            error(
                {},
                "limits",
                "a limit is outside the range this engine holds (" +
                    std::string{limits_error_name(limits_error)} + ")"
            );
            return;
        }
        for (const auto& [path, source] : provenance_)
            resolution.provenance.push_back(Provenance{path, source});
        resolution.effective = std::move(effective);
        result_.resolution = std::move(resolution);
    }

    static void strip_positions(Value& value) {
        value.position = {};
        for (Value& item : value.items)
            strip_positions(item);
        for (Member& member : value.members)
            strip_positions(member.value);
    }

    /// Returns the profile the hashes read, when it is not the effective one.
    ///
    /// Six keys were added after grammar 1's first release: an empty
    /// `layout.installation-archives` list mounts nothing, an
    /// `identity.watcher-side-frame` of 2 shows 3.1c's watcher art, and an
    /// empty `identity.skirmish-hidden-sides` or `battle-room-hidden-sides`
    /// list hides no side. A `limits.weapon-types` of 256 weapon types and a
    /// `limits.sides` of five sides number and load what 3.1c does. Each of
    /// those is what a profile that never wrote the key does, so the canonical
    /// form leaves each out at that value, and existing profiles keep both
    /// hashes. Any other value stays, and the effective profile is read as it
    /// is, uncopied, when none of the keys is at its baseline. The effective
    /// profile itself still holds every key.
    ///
    /// @param effective the effective profile
    /// @return a copy without those keys; nothing when there are none to leave out
    static std::optional<Value> canonical_source(const Value& effective) {
        struct LateKey {
            std::string_view block;
            std::string_view key;
            bool (*at_baseline)(const Value&);
        };

        static constexpr std::array<LateKey, 6> late_keys{{
            {"layout",
             "installation-archives",
             [](const Value& value) {
                 return value.kind == ValueKind::list && value.items.empty();
             }},
            {"identity",
             "watcher-side-frame",
             [](const Value& value) {
                 int64_t frame = 0;
                 return value.kind == ValueKind::number &&
                        formats::oamod::integer_value(value.number, frame) && frame == 2;
             }},
            {"identity",
             "skirmish-hidden-sides",
             [](const Value& value) {
                 return value.kind == ValueKind::list && value.items.empty();
             }},
            {"identity",
             "battle-room-hidden-sides",
             [](const Value& value) {
                 return value.kind == ValueKind::list && value.items.empty();
             }},
            {"limits",
             "weapon-types",
             [](const Value& value) {
                 const Value* types = find_member(value, "types");
                 int64_t numbered = 0;
                 return value.kind == ValueKind::map && types != nullptr &&
                        types->kind == ValueKind::number &&
                        formats::oamod::integer_value(types->number, numbered) &&
                        numbered == limits::base_weapon_types;
             }},
            {"limits", "sides", [](const Value& value) {
                 const Value* count = find_member(value, "count");
                 int64_t loaded = 0;
                 return value.kind == ValueKind::map && count != nullptr &&
                        count->kind == ValueKind::number &&
                        formats::oamod::integer_value(count->number, loaded) &&
                        loaded == limits::base_sides;
             }},
        }};
        std::optional<Value> copy;
        for (const LateKey& late : late_keys) {
            const Value* source = copy ? &*copy : &effective;
            const Value* found_block = find_member(*source, late.block);
            if (found_block == nullptr)
                continue;
            const Value* found = find_member(*found_block, late.key);
            if (found == nullptr || !late.at_baseline(*found))
                continue;
            if (!copy)
                copy = effective;
            Value* block = find_member(*copy, late.block);
            std::vector<Member> kept;
            kept.reserve(block->members.size());
            for (Member& member : block->members) {
                if (member.key != late.key)
                    kept.push_back(std::move(member));
            }
            block->members = std::move(kept);
        }
        return copy;
    }

    /// The part of an effective profile the network-play hash covers: sim-scope values only.
    static Value sim_projection(const Value& effective) {
        const auto& table = registry::table();
        Value out = make_map();
        const Value* oamod = find_member(effective, "oamod");
        set_member(out, "oamod", oamod != nullptr ? *oamod : Value{});
        set_member(out, "catalogue", make_integer(table.catalogue));
        for (const EntryKind kind : value_kinds) {
            const std::string_view block = value_block(kind);
            Value flat = make_map();
            for (const Entry& entry : table.entries) {
                if (entry.kind != kind || entry.value.scope != Scope::sim)
                    continue;
                const Value* node = find_member(effective, block);
                std::string_view key = entry.key;
                while (node != nullptr) {
                    const size_t dot = key.find('.');
                    node = find_member(*node, key.substr(0, dot));
                    if (dot == std::string_view::npos)
                        break;
                    key.remove_prefix(dot + 1);
                }
                if (node != nullptr)
                    set_member(flat, entry.key, *node);
            }
            set_member(out, block, std::move(flat));
        }
        for (const std::string_view block : {"limits", "hacks"}) {
            Value projected = make_map();
            const Value* values = find_member(effective, block);
            for (const Member& member : values->members) {
                const Entry* entry =
                    registry::find_entry(block == "limits" ? "limits." + member.key : member.key);
                if (entry == nullptr || entry->scope != Scope::sim)
                    continue;
                Value kept = make_map();
                for (const Member& parameter : member.value.members) {
                    const int index = registry::find_parameter(*entry, parameter.key);
                    if (index >= 0 &&
                        registry::parameters_of(*entry)[static_cast<size_t>(index)].value.scope ==
                            Scope::sim)
                        set_member(kept, parameter.key, parameter.value);
                }
                set_member(projected, member.key, std::move(kept));
            }
            set_member(out, block, std::move(projected));
        }
        set_member(out, "script-extensions", *find_member(effective, "script-extensions"));
        Value data_keys = make_map();
        for (const Member& file : find_member(effective, "data-keys")->members) {
            Value kept = make_map();
            for (const Member& key : file.value.members) {
                const Entry* entry = registry::find_entry(key.value.text);
                if (entry != nullptr && entry->scope == Scope::sim)
                    set_member(kept, key.key, key.value);
            }
            set_member(data_keys, file.key, std::move(kept));
        }
        set_member(out, "data-keys", std::move(data_keys));
        return out;
    }

    std::string_view source_{};
    const ResolveOptions& options_;
    ResolveResult& result_;

    Value meta_{make_map()};
    oa::data::content_bundles::ContentNeeds content_needs_{};
    std::array<std::vector<std::pair<std::string, Value>>, 8> given_values_{};
    std::vector<WrittenEntry> written_limits_{};
    std::vector<WrittenEntry> written_hacks_{};
    std::string fidelity_{};
    std::optional<std::vector<WrittenMount>> get_mounts_{};
    std::optional<std::vector<WrittenMount>> set_mounts_{};
    std::vector<std::pair<std::string, std::optional<std::string>>> unit_keys_{};
    std::vector<std::pair<std::string, std::optional<std::string>>> weapon_keys_{};
    std::vector<WrittenBinding> bindings_{};
    std::vector<RegistrySeed> seeds_{};
    Value seeds_value_{};

    Value effective_value_blocks_{make_map()};
    std::vector<ResolvedEntry> resolved_{};
    std::set<std::string> present_{};
    std::map<std::string, std::string> provenance_{};
};

} // namespace

std::string format_diagnostic(const Diagnostic& diagnostic) {
    std::string text = diagnostic.source;
    if (diagnostic.position.line != 0)
        text += ":" + std::to_string(diagnostic.position.line) + ":" +
                std::to_string(diagnostic.position.column);
    if (!text.empty())
        text += ": ";
    if (!diagnostic.path.empty())
        text += diagnostic.path + ": ";
    return text + diagnostic.message;
}

MatchOptionReading match_option_reading(const MatchOption& option) {
    MatchOptionReading reading;
    const auto [entry, index] = bound_parameter(option.path);
    if (entry == nullptr || index < 0) {
        reading.problem = std::string{"unknown parameter"};
        return reading;
    }
    Value normal{};
    reading.problem =
        registry_match_problem(*entry, index, option.value, "match " + option.path, normal);
    if (!reading.problem.has_value())
        reading.normal = value_text(normal);
    return reading;
}

std::optional<std::string> match_option_problem(const MatchOption& option) {
    return match_option_reading(option).problem;
}

std::optional<MatchOption> parse_match_option(std::string_view text) {
    const size_t equals = text.find('=');
    if (equals == std::string_view::npos)
        return std::nullopt;
    return MatchOption{std::string{text.substr(0, equals)}, std::string{text.substr(equals + 1)}};
}

ResolveResult resolve_profile(
    std::span<const uint8_t> text, std::string_view source, const ResolveOptions& options
) {
    ResolveResult result{};
    Node root{};
    formats::oamod::ReadError read_error{};
    if (!formats::oamod::read_document(text, root, read_error)) {
        result.errors.push_back(
            Diagnostic{
                std::string{source},
                read_error.position,
                std::string{},
                formats::oamod::rule_message(read_error.rule)
            }
        );
        return result;
    }
    Resolver resolver{source, options, result};
    resolver.run(root);
    if (!result.errors.empty())
        result.resolution.reset();
    return result;
}

std::string digest_text(const base::sha256::Digest& digest) {
    const auto hex = base::sha256::to_hex(digest);
    return std::string{hex.data(), hex.size()};
}

std::string describe_resolution(const Resolution& resolution) {
    return pretty_json(resolution.effective) + "sha256-sim  " +
           digest_text(resolution.profile.sim_hash) + "\n" + "sha256-full " +
           digest_text(resolution.profile.full_hash) + "\n";
}

} // namespace oa::data::mod_profile
