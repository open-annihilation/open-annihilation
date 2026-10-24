// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/saves/content_record.hpp"

#include "oa/data/content_bundles.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::saves {
namespace {

namespace bundles = content_bundles;

/// The expansion whose tracks do not change the rules.
constexpr std::string_view kMusic = "music";

/// Reports whether an id is an expansion other than music.
///
/// @param id the bundle id
/// @return true when the note keeps it
bool rule_changing(std::string_view id) {
    const bundles::Bundle* bundle = bundles::find_bundle(id);
    return bundle != nullptr && bundle->kind == bundles::Kind::expansion && bundle->id != kMusic;
}

/// Reports whether an expansion extends a base game.
///
/// @param bundle the expansion
/// @param base_game the base game's id
/// @return true when it extends that game
bool extends_game(const bundles::Bundle& bundle, std::string_view base_game) {
    for (const std::string_view game : bundle.extends) {
        if (game == base_game)
            return true;
    }
    return false;
}

/// Reports whether a bundle id is among those copied.
///
/// @param copied the copied ids
/// @param id the id
/// @return true when it is copied
bool is_copied(std::span<const std::string_view> copied, std::string_view id) {
    for (const std::string_view one : copied) {
        if (one == id)
            return true;
    }
    return false;
}

/// Tells whether text holds a line break.
///
/// @param text the value
/// @return true when it holds CR or LF
bool has_line_break(std::string_view text) {
    return text.find('\n') != std::string_view::npos || text.find('\r') != std::string_view::npos;
}

/// Tells whether text is 64 lower-case hexadecimal digits.
///
/// @param text the token
/// @return true when it is a SHA-256 written that way
bool lower_hex64(std::string_view text) {
    if (text.size() != 64)
        return false;
    for (const char character : text) {
        const bool digit =
            (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
        if (!digit)
            return false;
    }
    return true;
}

/// Tells whether an id is lower-case kebab-case.
///
/// Words of a-z and 0-9 joined by single hyphens, not empty and not starting
/// or ending with a hyphen.
///
/// @param text the id
/// @return true when it is such an id
bool kebab_id(std::string_view text) {
    if (text.empty() || text.front() == '-' || text.back() == '-')
        return false;
    char previous = 0;
    for (const char character : text) {
        const bool word =
            (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
        if (!word && character != '-')
            return false;
        if (character == '-' && previous == '-')
            return false;
        previous = character;
    }
    return true;
}

/// Splits a `mutator` value into a SHA-256 and an id, when both are well formed.
///
/// @param value the text after `mutator:`
/// @param[out] sha256 the hash, when the value is well formed
/// @param[out] id the id, when the value is well formed
/// @return true when the value is one hash, one space and one kebab-case id
bool mutator_tokens(std::string_view value, std::string& sha256, std::string& id) {
    const std::size_t space = value.find(' ');
    if (space == std::string_view::npos || value.find(' ', space + 1) != std::string_view::npos)
        return false;
    const std::string_view hash = value.substr(0, space);
    const std::string_view name = value.substr(space + 1);
    if (!lower_hex64(hash) || !kebab_id(name))
        return false;
    sha256.assign(hash);
    id.assign(name);
    return true;
}

/// Splits a `brain` value into a player and an id, when both are well formed.
///
/// The player is one digit, 0 to 9. The id is lower-case kebab-case, and
/// nothing follows it.
///
/// @param value the text after `brain:`
/// @param[out] player the player, when the value is well formed
/// @param[out] id the id, when the value is well formed
/// @return true when the value is one player, one space and one kebab-case id
bool brain_tokens(std::string_view value, int32_t& player, std::string& id) {
    if (value.size() < 3 || value[1] != ' ')
        return false;
    if (value[0] < '0' || value[0] > '9')
        return false;
    const std::string_view name = value.substr(2);
    if (name.find(' ') != std::string_view::npos || !kebab_id(name))
        return false;
    player = static_cast<int32_t>(value[0] - '0');
    id.assign(name);
    return true;
}

/// Joins English names with a comma and a space.
///
/// @param names the names
/// @return the line, or empty when there are none
std::string join_names(const std::vector<std::string>& names) {
    std::string line;
    for (const std::string& name : names) {
        if (!line.empty())
            line += ", ";
        line += name;
    }
    return line;
}

/// Reads the value after "key:" on one line.
///
/// @param line the line
/// @param key the key, without the colon
/// @param[out] value the text after the colon, with surrounding spaces removed
/// @return true when the line is that key
bool take_field(std::string_view line, std::string_view key, std::string& value) {
    const std::string prefix = std::string(key) + ":";
    if (line.size() < prefix.size() || line.substr(0, prefix.size()) != prefix)
        return false;
    std::string_view rest = line.substr(prefix.size());
    while (!rest.empty() && (rest.front() == ' ' || rest.front() == '\t'))
        rest.remove_prefix(1);
    while (!rest.empty() && (rest.back() == ' ' || rest.back() == '\t' || rest.back() == '\r'))
        rest.remove_suffix(1);
    value.assign(rest);
    return true;
}

} // namespace

ContentRecord content_record_from(
    std::string_view base_game, std::span<const std::string> expansions, std::string_view mod
) {
    ContentRecord record;
    const bundles::Bundle* base = bundles::find_bundle(base_game);
    if (base == nullptr || base->kind != bundles::Kind::base_game)
        return record;
    record.present = true;
    record.base_game = std::string(base->id);
    record.mod = std::string(mod);
    for (const bundles::Bundle& bundle : bundles::bundles()) {
        if (!rule_changing(bundle.id))
            continue;
        for (const std::string& id : expansions) {
            if (id == bundle.id)
                record.expansions.emplace_back(bundle.id);
        }
    }
    return record;
}

std::string content_record_text(const ContentRecord& record) {
    if (!record.present)
        return {};
    for (const ContentMutator& mutator : record.mutators) {
        if (has_line_break(mutator.sha256) || has_line_break(mutator.id) ||
            has_line_break(mutator.version) || has_line_break(mutator.name))
            return {};
    }
    for (const ContentBrain& brain : record.brains) {
        if (has_line_break(brain.id) || has_line_break(brain.version) ||
            has_line_break(brain.level) || has_line_break(brain.name))
            return {};
    }
    std::string text = "base-game: " + record.base_game + "\nexpansions: ";
    for (std::size_t index = 0; index < record.expansions.size(); ++index) {
        if (index != 0)
            text += ',';
        text += record.expansions[index];
    }
    text += "\nmod: ";
    text += record.mod;
    text += '\n';
    for (const ContentMutator& mutator : record.mutators) {
        text += "mutator: ";
        text += mutator.sha256;
        text += ' ';
        text += mutator.id;
        text += "\nmutator-version: ";
        text += mutator.version;
        text += "\nmutator-name: ";
        text += mutator.name;
        text += '\n';
    }
    std::vector<const ContentBrain*> brains;
    brains.reserve(record.brains.size());
    for (const ContentBrain& brain : record.brains)
        brains.push_back(&brain);
    std::stable_sort(
        brains.begin(), brains.end(), [](const ContentBrain* left, const ContentBrain* right) {
            return left->player < right->player;
        }
    );
    for (const ContentBrain* brain : brains) {
        text += "brain: ";
        text += std::to_string(brain->player);
        text += ' ';
        text += brain->id;
        text += "\nbrain-version: ";
        text += brain->version;
        text += "\nbrain-level: ";
        text += brain->level;
        text += "\nbrain-name: ";
        text += brain->name;
        text += '\n';
    }
    return text;
}

ContentRecord content_record_from_text(std::string_view text) {
    ContentRecord record;
    std::string base;
    std::string expansions;
    std::string mod;
    bool saw_base = false;
    // How many mutators a note keeps. Further mutator lines are skipped.
    constexpr std::size_t most_mutators = 32;
    // How many brains a note keeps. Further brain lines are skipped.
    constexpr std::size_t most_brains = 10;
    std::vector<ContentMutator> mutators;
    std::vector<ContentBrain> brains;
    bool follow = false;
    bool keep = false;
    bool brain_follow = false;
    bool brain_keep = false;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = text.find('\n', at);
        const std::string_view line =
            text.substr(at, (end == std::string_view::npos ? text.size() : end) - at);
        std::string value;
        if (take_field(line, "base-game", value)) {
            base = std::move(value);
            saw_base = true;
        } else if (take_field(line, "expansions", value)) {
            expansions = std::move(value);
        } else if (take_field(line, "mod", value)) {
            mod = std::move(value);
        } else if (take_field(line, "mutator", value)) {
            std::string hash_text;
            std::string mutator_id;
            follow = true;
            if (mutators.size() >= most_mutators || !mutator_tokens(value, hash_text, mutator_id)) {
                keep = false;
            } else {
                mutators.push_back(
                    ContentMutator{
                        std::move(hash_text), std::move(mutator_id), std::string{}, std::string{}
                    }
                );
                keep = true;
            }
        } else if (take_field(line, "mutator-version", value)) {
            if (follow && keep)
                mutators.back().version = std::move(value);
        } else if (take_field(line, "mutator-name", value)) {
            if (follow && keep)
                mutators.back().name = std::move(value);
        } else if (take_field(line, "brain", value)) {
            int32_t player = 0;
            std::string brain_id;
            brain_follow = true;
            if (brains.size() >= most_brains || !brain_tokens(value, player, brain_id)) {
                brain_keep = false;
            } else {
                brains.push_back(
                    ContentBrain{
                        player, std::move(brain_id), std::string{}, std::string{}, std::string{}
                    }
                );
                brain_keep = true;
            }
        } else if (take_field(line, "brain-version", value)) {
            if (brain_follow && brain_keep)
                brains.back().version = std::move(value);
        } else if (take_field(line, "brain-level", value)) {
            if (brain_follow && brain_keep)
                brains.back().level = std::move(value);
        } else if (take_field(line, "brain-name", value)) {
            if (brain_follow && brain_keep)
                brains.back().name = std::move(value);
        }
        if (end == std::string_view::npos)
            break;
        at = end + 1;
    }
    if (!saw_base)
        return record;
    std::vector<std::string> listed;
    std::size_t comma = 0;
    while (comma < expansions.size()) {
        const std::size_t next = expansions.find(',', comma);
        const std::string id = expansions.substr(
            comma, (next == std::string::npos ? expansions.size() : next) - comma
        );
        if (!id.empty())
            listed.push_back(id);
        if (next == std::string::npos)
            break;
        comma = next + 1;
    }
    record = content_record_from(base, listed, mod);
    if (record.present) {
        record.mutators = std::move(mutators);
        record.brains = std::move(brains);
    }
    return record;
}

ContentGap content_gap(
    const ContentRecord& saved,
    const bundles::ContentChoice& now,
    std::span<const std::string_view> copied
) {
    ContentGap gap;
    gap.next = now;
    if (!saved.present)
        return gap;
    std::vector<std::string> missing;
    std::vector<std::string> missing_ids;
    if (saved.base_game != now.base_game) {
        gap.base_differs = true;
        missing.push_back(bundle_english_name(saved.base_game));
        missing_ids.push_back(saved.base_game);
    }
    for (const std::string& id : saved.expansions) {
        bool on = false;
        for (const std::string& current : now.expansions) {
            if (current == id)
                on = true;
        }
        if (!on) {
            missing.push_back(bundle_english_name(id));
            missing_ids.push_back(id);
        }
    }
    if (missing.empty())
        return gap;
    gap.needed = true;
    gap.focus = gap.base_differs ? bundle_english_name(saved.base_game) : join_names(missing);
    if (gap.base_differs)
        gap.focus_ids.push_back(saved.base_game);
    else
        gap.focus_ids = std::move(missing_ids);

    std::vector<std::string_view> enabled;
    for (const std::string& id : now.expansions) {
        const bundles::Bundle* bundle = bundles::find_bundle(id);
        if (bundle == nullptr || bundle->kind != bundles::Kind::expansion)
            continue;
        if (extends_game(*bundle, saved.base_game))
            enabled.emplace_back(bundle->id);
    }
    for (const std::string& id : saved.expansions)
        enabled = bundles::after_turning_on(enabled, id);

    gap.next.base_game = saved.base_game;
    gap.next.command_line = false;
    gap.next.migration = false;
    gap.next.error.clear();
    gap.next.expansions.clear();
    for (const std::string_view id : enabled) {
        const bundles::Bundle* bundle = bundles::find_bundle(id);
        if (bundle != nullptr && bundle->kind == bundles::Kind::expansion)
            gap.next.expansions.emplace_back(id);
    }
    gap.clears_mod = gap.base_differs;

    if (now.command_line) {
        gap.block = ContentBlock::command_line;
        return gap;
    }
    if (!is_copied(copied, saved.base_game)) {
        gap.block = ContentBlock::not_copied;
        return gap;
    }
    for (const std::string& id : saved.expansions) {
        const std::string_view one[]{id};
        for (const std::string_view need : bundles::requirement_closure(one)) {
            const bundles::Bundle* bundle = bundles::find_bundle(need);
            if (bundle == nullptr || bundle->kind != bundles::Kind::expansion ||
                bundle->id == kMusic)
                continue;
            if (!is_copied(copied, need)) {
                gap.block = ContentBlock::not_copied;
                return gap;
            }
        }
    }
    gap.can_switch = true;
    return gap;
}

std::string bundle_english_name(std::string_view id) {
    const bundles::Bundle* bundle = bundles::find_bundle(id);
    if (bundle == nullptr)
        return std::string(id);
    return std::string(bundle->english_name);
}

} // namespace oa::data::saves
