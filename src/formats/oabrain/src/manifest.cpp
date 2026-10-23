// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/oabrain/manifest.hpp"

#include "oa/formats/oamod.hpp"
#include "oa/formats/oamut/shared.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::formats::oabrain {
namespace {

namespace oamod = oa::formats::oamod;
namespace oamut = oa::formats::oamut;
namespace shared = oa::formats::oamut::shared;

/// The top-level keys a manifest may hold.
constexpr std::array<std::string_view, 17> manifest_keys{
    "oabrain",
    "id",
    "name",
    "description",
    "version",
    "author",
    "homepage",
    "icon",
    "tags",
    "works-with",
    "requires",
    "levels",
    "fullAwareness",
    "networks",
    "options",
    "strings",
    "entry",
};

/// Keys that belong to a mod profile, never to a brain.
constexpr std::array<std::string_view, 11> profile_keys{
    "identity",
    "layout",
    "limits",
    "script-extensions",
    "data-keys",
    "hacks",
    "settings",
    "media",
    "packaging",
    "supportedGames",
    "requiredExpansions",
};

/// Keys that belong to a mutator, never to a brain.
constexpr std::array<std::string_view, 4> mutator_keys{
    "layers",
    "clashes",
    "conflicts-with",
    "legacy",
};

/// The keys of one level.
constexpr std::array<std::string_view, 3> level_keys{"id", "name", "difficulty"};

/// The keys of requires.
constexpr std::array<std::string_view, 2> requires_keys{"engine", "expansions"};

/// The keys of one network.
constexpr std::array<std::string_view, 3> network_keys{"id", "file", "sha256"};

/// The language key a brain reads itself.
constexpr std::array<std::string_view, 1> level_language_key{"levels"};

/// Tells whether a key is one of a list.
///
/// @param key the key
/// @param allowed the keys that may appear
/// @return true when `key` is one of them
bool listed(std::string_view key, std::span<const std::string_view> allowed) {
    for (const std::string_view known : allowed)
        if (known == key)
            return true;
    return false;
}

/// Joins a list path and a 0-based index.
///
/// @param block the list path
/// @param index the index
/// @return the item path, such as levels[0]
std::string index_path(std::string_view block, std::size_t index) {
    return std::string{block} + "[" + std::to_string(index) + "]";
}

/// Tells whether a list already holds an id.
///
/// @param ids the ids so far
/// @param id the id
/// @return true when `id` is already there
bool repeats(const std::vector<std::string>& ids, std::string_view id) {
    for (const std::string& earlier : ids)
        if (earlier == id)
            return true;
    return false;
}

/// The nine tags, written the way a refusal names them.
///
/// @return classic, economy, rush, turtle, air, naval, learned, teaching, experimental
std::string tag_names() {
    std::string text;
    for (std::size_t index = 0; index < brain_tags.size(); ++index) {
        if (index != 0)
            text += ", ";
        text += brain_tags[index];
    }
    return text;
}

/// Tells whether a stem is 1 to 64 bytes of lower-case letters, digits and `_`.
///
/// @param stem the stem
/// @return true when it has that shape
bool file_stem(std::string_view stem) noexcept {
    if (stem.empty() || stem.size() > most_file_stem_bytes)
        return false;
    for (const char character : stem) {
        const bool word = (character >= 'a' && character <= 'z') ||
                          (character >= '0' && character <= '9') || character == '_';
        if (!word)
            return false;
    }
    return true;
}

/// Tells whether text is nets/<stem>.oann.
///
/// @param text the file
/// @return true when the stem is a file stem and nothing else is in the path
bool network_file(std::string_view text) noexcept {
    constexpr std::string_view folder = "nets/";
    constexpr std::string_view suffix = ".oann";
    if (text.size() < folder.size() + 1 + suffix.size() || text.substr(0, folder.size()) != folder)
        return false;
    if (text.substr(text.size() - suffix.size()) != suffix)
        return false;
    const std::string_view stem =
        text.substr(folder.size(), text.size() - folder.size() - suffix.size());
    if (stem.find('/') != std::string_view::npos)
        return false;
    return file_stem(stem);
}

/// Tells whether text is a Lua file at the package's top.
///
/// @param text the entry
/// @return true for <stem>.lua with a file stem and no slash
bool entry_file(std::string_view text) noexcept {
    constexpr std::string_view suffix = ".lua";
    if (text.size() <= suffix.size() || text.substr(text.size() - suffix.size()) != suffix)
        return false;
    return file_stem(text.substr(0, text.size() - suffix.size()));
}

/// Tells whether text is 64 lower-case hex digits.
///
/// @param text the text
/// @return true when it is
bool sha256_text(std::string_view text) noexcept {
    if (text.size() != 64)
        return false;
    for (const char character : text) {
        const bool hex =
            (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
        if (!hex)
            return false;
    }
    return true;
}

/// Finds a level already read.
///
/// @param levels the levels
/// @param id the level id
/// @return the level, or null
const Level* level_named(const std::vector<Level>& levels, std::string_view id) noexcept {
    for (const Level& level : levels)
        if (level.id == id)
            return &level;
    return nullptr;
}

/// Reads the expansions list.
///
/// @param node the expansions value
/// @param[out] expansions the ids that passed
/// @param[in,out] problems the problems
void read_expansions(
    const oamod::Node& node, std::vector<std::string>& expansions, std::vector<Problem>& problems
) {
    constexpr std::string_view block = "requires.expansions";
    if (node.kind != oamod::NodeKind::sequence) {
        shared::add_problem(
            problems, node.position, std::string{block}, std::string{block} + " must be a list"
        );
        return;
    }
    if (node.children.empty() || node.children.size() > most_expansions) {
        shared::add_problem(
            problems,
            node.position,
            std::string{block},
            std::string{block} + " must list 1 to " + std::to_string(most_expansions) +
                " expansions"
        );
    }
    std::vector<std::string> seen;
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        const oamod::Node& item = node.children[index];
        const std::string path = index_path(block, index);
        if (item.kind != oamod::NodeKind::string || !oamut::mutator_id(item.text)) {
            shared::add_problem(
                problems,
                item.position,
                path,
                path + " must be 1 to " + std::to_string(most_id_bytes) +
                    " bytes of lower-case kebab-case"
            );
            continue;
        }
        if (repeats(seen, item.text)) {
            shared::add_problem(problems, item.position, path, path + " repeats " + item.text);
            continue;
        }
        seen.push_back(item.text);
        expansions.push_back(item.text);
    }
}

/// Reads one level.
///
/// @param item the level
/// @param index its index in the list
/// @param[in,out] seen the ids already accepted
/// @param[out] levels the levels that passed
/// @param[in,out] problems the problems
void read_level(
    const oamod::Node& item,
    std::size_t index,
    std::vector<std::string>& seen,
    std::vector<Level>& levels,
    std::vector<Problem>& problems
) {
    const std::string block = index_path("levels", index);
    if (item.kind != oamod::NodeKind::mapping) {
        shared::add_problem(problems, item.position, block, block + " must be an object");
        return;
    }
    shared::refuse_unknown_keys(item, level_keys, block, problems);
    Level level;
    bool fields_ok = true;
    const oamod::Node* id = oamod::find_entry(item, "id");
    if (id == nullptr || id->kind != oamod::NodeKind::string || !level_id(id->text)) {
        shared::add_problem(
            problems,
            id != nullptr ? id->position : item.position,
            block + ".id",
            block + ".id must be 1 to " + std::to_string(most_level_id_bytes) +
                " bytes of lower-case kebab-case"
        );
        fields_ok = false;
    } else if (repeats(seen, id->text)) {
        shared::add_problem(
            problems, id->position, block + ".id", block + ".id repeats " + id->text
        );
        fields_ok = false;
    } else {
        level.id = id->text;
        seen.push_back(level.id);
    }
    const oamod::Node* name = oamod::find_entry(item, "name");
    if (name == nullptr || name->kind != oamod::NodeKind::string || name->text.empty() ||
        name->text.size() > most_level_name_bytes || shared::has_control(name->text, false)) {
        shared::add_problem(
            problems,
            name != nullptr ? name->position : item.position,
            block + ".name",
            block + ".name must be 1 to " + std::to_string(most_level_name_bytes) +
                " bytes with no control character"
        );
        fields_ok = false;
    } else {
        level.name = name->text;
    }
    const oamod::Node* difficulty = oamod::find_entry(item, "difficulty");
    const std::optional<Difficulty> parsed =
        difficulty != nullptr && difficulty->kind == oamod::NodeKind::string
            ? parse_difficulty(difficulty->text)
            : std::nullopt;
    if (!parsed) {
        shared::add_problem(
            problems,
            difficulty != nullptr ? difficulty->position : item.position,
            block + ".difficulty",
            block + ".difficulty must be easy, medium or hard"
        );
        fields_ok = false;
    } else {
        level.difficulty = *parsed;
    }
    if (fields_ok)
        levels.push_back(std::move(level));
}

/// Reads the levels list.
///
/// @param node the levels value
/// @param[out] levels the levels that passed
/// @param[in,out] problems the problems
void read_levels(
    const oamod::Node& node, std::vector<Level>& levels, std::vector<Problem>& problems
) {
    if (node.kind != oamod::NodeKind::sequence) {
        shared::add_problem(problems, node.position, "levels", "levels must be a list");
        return;
    }
    if (node.children.empty() || node.children.size() > most_levels) {
        shared::add_problem(
            problems,
            node.position,
            "levels",
            "levels must list 1 to " + std::to_string(most_levels) + " levels"
        );
    }
    std::vector<std::string> seen;
    for (std::size_t index = 0; index < node.children.size(); ++index)
        read_level(node.children[index], index, seen, levels, problems);
}

/// Reads one network.
///
/// @param item the network
/// @param index its index in the list
/// @param[in,out] seen_ids the ids already accepted
/// @param[in,out] seen_files the files already accepted
/// @param[out] networks the networks that passed
/// @param[in,out] problems the problems
void read_network(
    const oamod::Node& item,
    std::size_t index,
    std::vector<std::string>& seen_ids,
    std::vector<std::string>& seen_files,
    std::vector<Network>& networks,
    std::vector<Problem>& problems
) {
    const std::string block = index_path("networks", index);
    if (item.kind != oamod::NodeKind::mapping) {
        shared::add_problem(problems, item.position, block, block + " must be an object");
        return;
    }
    shared::refuse_unknown_keys(item, network_keys, block, problems);
    Network network;
    bool fields_ok = true;
    const oamod::Node* id = oamod::find_entry(item, "id");
    if (id == nullptr || id->kind != oamod::NodeKind::string || !oamut::option_id(id->text)) {
        const bool reserved = id != nullptr && id->kind == oamod::NodeKind::string &&
                              oamut::lua_reserved_word(id->text);
        shared::add_problem(
            problems,
            id != nullptr ? id->position : item.position,
            block + ".id",
            reserved ? "is a word Lua reserves"
                     : block + ".id must be a Lua name, [a-z][a-z0-9_]*, of 1 to 32 bytes"
        );
        fields_ok = false;
    } else if (repeats(seen_ids, id->text)) {
        shared::add_problem(
            problems, id->position, block + ".id", block + ".id repeats " + id->text
        );
        fields_ok = false;
    } else {
        network.id = id->text;
        seen_ids.push_back(network.id);
    }
    const oamod::Node* file = oamod::find_entry(item, "file");
    if (file == nullptr || file->kind != oamod::NodeKind::string || !network_file(file->text)) {
        shared::add_problem(
            problems,
            file != nullptr ? file->position : item.position,
            block + ".file",
            block + ".file must be nets/<stem>.oann, with a stem of 1 to " +
                std::to_string(most_file_stem_bytes) + " bytes of lower-case letters, digits and _"
        );
        fields_ok = false;
    } else if (repeats(seen_files, file->text)) {
        shared::add_problem(
            problems, file->position, block + ".file", block + ".file repeats " + file->text
        );
        fields_ok = false;
    } else {
        network.file = file->text;
        seen_files.push_back(network.file);
    }
    const oamod::Node* hash = oamod::find_entry(item, "sha256");
    if (hash == nullptr || hash->kind != oamod::NodeKind::string || !hash->quoted ||
        !sha256_text(hash->text)) {
        shared::add_problem(
            problems,
            hash != nullptr ? hash->position : item.position,
            block + ".sha256",
            block + ".sha256 must be 64 lower-case hex digits in quotes"
        );
        fields_ok = false;
    } else {
        network.sha256 = hash->text;
    }
    if (fields_ok)
        networks.push_back(std::move(network));
}

/// Reads the networks list.
///
/// @param node the networks value
/// @param[out] networks the networks that passed
/// @param[in,out] problems the problems
void read_networks(
    const oamod::Node& node, std::vector<Network>& networks, std::vector<Problem>& problems
) {
    if (node.kind != oamod::NodeKind::sequence) {
        shared::add_problem(problems, node.position, "networks", "networks must be a list");
        return;
    }
    if (node.children.empty() || node.children.size() > most_networks) {
        shared::add_problem(
            problems,
            node.position,
            "networks",
            "networks must list 1 to " + std::to_string(most_networks) + " networks"
        );
    }
    std::vector<std::string> seen_ids;
    std::vector<std::string> seen_files;
    for (std::size_t index = 0; index < node.children.size(); ++index)
        read_network(node.children[index], index, seen_ids, seen_files, networks, problems);
}

/// Reads the level names of one language.
///
/// @param node the levels value
/// @param language the language tag
/// @param defined the brain's levels
/// @param[out] translation the translation
/// @param[in,out] problems the problems
void read_translated_levels(
    const oamod::Node& node,
    std::string_view language,
    const std::vector<Level>& defined,
    Translation& translation,
    std::vector<Problem>& problems
) {
    const std::string block = "strings." + std::string{language} + ".levels";
    if (node.kind != oamod::NodeKind::mapping) {
        shared::add_problem(problems, node.position, block, block + " must be an object");
        return;
    }
    for (const oamod::Node& entry : node.children) {
        const std::string path = block + "." + entry.key.text;
        if (entry.key.kind != oamod::KeyKind::string ||
            level_named(defined, entry.key.text) == nullptr) {
            shared::add_problem(
                problems, entry.position, path, path + " is not a level of this brain"
            );
            continue;
        }
        if (entry.kind != oamod::NodeKind::string || entry.text.empty() ||
            entry.text.size() > most_level_name_bytes || shared::has_control(entry.text, false)) {
            shared::add_problem(
                problems,
                entry.position,
                path,
                path + " must be 1 to " + std::to_string(most_level_name_bytes) +
                    " bytes with no control character"
            );
            continue;
        }
        translation.levels.push_back(LevelName{entry.key.text, entry.text});
    }
}

/// Reads the header and every block.
///
/// @param root the document
/// @param[out] manifest the manifest
/// @param[in,out] problems the problems
void read_document_manifest(
    const oamod::Node& root, Manifest& manifest, std::vector<Problem>& problems
) {
    for (const oamod::Node& entry : root.children) {
        if (entry.key.kind == oamod::KeyKind::string && listed(entry.key.text, manifest_keys))
            continue;
        if (entry.key.kind == oamod::KeyKind::string && listed(entry.key.text, profile_keys)) {
            shared::add_problem(
                problems,
                entry.position,
                entry.key.text,
                entry.key.text + " belongs in a mod's oamod.yaml; a brain cannot carry it"
            );
            continue;
        }
        if (entry.key.kind == oamod::KeyKind::string && listed(entry.key.text, mutator_keys)) {
            shared::add_problem(
                problems,
                entry.position,
                entry.key.text,
                entry.key.text + " is a mutator's key; a brain cannot carry it"
            );
            continue;
        }
        shared::add_problem(
            problems,
            entry.position,
            entry.key.text,
            entry.key.text + " is not a key of oabrain.yaml"
        );
    }

    const oamod::Node* format = oamod::find_entry(root, "oabrain");
    int64_t format_number = 0;
    if (format == nullptr || format->kind != oamod::NodeKind::number || !format->number.integer ||
        !oamod::integer_value(format->number, format_number) || format_number != format_version) {
        shared::add_problem(
            problems,
            format != nullptr ? format->position : root.position,
            "oabrain",
            "oabrain must be 1"
        );
    } else {
        manifest.format = format_number;
    }

    const oamod::Node* id = oamod::find_entry(root, "id");
    if (id == nullptr || id->kind != oamod::NodeKind::string || !brain_id(id->text)) {
        shared::add_problem(
            problems,
            id != nullptr ? id->position : root.position,
            "id",
            "id must be 1 to " + std::to_string(most_id_bytes) + " bytes of lower-case kebab-case"
        );
    } else {
        manifest.id = id->text;
    }

    if (const oamod::Node* name = oamod::find_entry(root, "name")) {
        if (name->kind != oamod::NodeKind::string || name->text.empty() ||
            name->text.size() > most_name_bytes || shared::has_control(name->text, false)) {
            shared::add_problem(
                problems,
                name->position,
                "name",
                "name must be 1 to " + std::to_string(most_name_bytes) +
                    " bytes with no control character"
            );
        } else {
            manifest.name = name->text;
        }
    } else {
        shared::add_problem(problems, root.position, "name", "name is required");
    }

    if (const oamod::Node* description = oamod::find_entry(root, "description")) {
        if (description->kind != oamod::NodeKind::string) {
            shared::add_problem(
                problems, description->position, "description", "description must be a string"
            );
        } else {
            if (description->text.empty() || description->text.size() > most_description_bytes) {
                shared::add_problem(
                    problems,
                    description->position,
                    "description",
                    "description must be 1 to " + std::to_string(most_description_bytes) + " bytes"
                );
            }
            if (shared::has_control(description->text, true)) {
                shared::add_problem(
                    problems,
                    description->position,
                    "description",
                    "description must not contain a control character other than a line feed"
                );
            }
            if (!description->text.empty() && description->text.size() <= most_description_bytes &&
                !shared::has_control(description->text, true))
                manifest.description = description->text;
        }
    } else {
        shared::add_problem(problems, root.position, "description", "description is required");
    }

    if (const oamod::Node* version = oamod::find_entry(root, "version")) {
        if (version->kind == oamod::NodeKind::number ||
            (version->kind == oamod::NodeKind::string && !version->quoted)) {
            shared::add_problem(
                problems, version->position, "version", "write the version in quotes, as \"2.0\""
            );
        } else if (version->kind != oamod::NodeKind::string) {
            shared::add_problem(problems, version->position, "version", "version must be a string");
        } else {
            const std::optional<oamut::Version> parsed = oamut::parse_version(version->text);
            if (!parsed) {
                shared::add_problem(
                    problems,
                    version->position,
                    "version",
                    "version must be a mutator version, such as \"2.0\""
                );
            } else {
                manifest.version = *parsed;
            }
        }
    } else {
        shared::add_problem(problems, root.position, "version", "version is required");
    }

    if (const oamod::Node* author = oamod::find_entry(root, "author"))
        shared::read_author(*author, manifest.author, problems);
    else
        shared::add_problem(problems, root.position, "author", "author is required");

    const oamod::Node* requires_node = oamod::find_entry(root, "requires");
    const oamod::Node* requires_block = nullptr;
    if (requires_node != nullptr) {
        if (requires_node->kind != oamod::NodeKind::mapping) {
            shared::add_problem(
                problems,
                requires_node->position,
                "requires",
                "requires must be an object with engine and expansions"
            );
        } else {
            requires_block = requires_node;
            shared::refuse_unknown_keys(*requires_node, requires_keys, "requires", problems);
            if (const oamod::Node* expansions = oamod::find_entry(*requires_node, "expansions"))
                read_expansions(*expansions, manifest.requires_expansions, problems);
        }
    }

    oamod::PackageKeys keys;
    std::vector<oamod::KeyProblem> key_problems;
    oamod::read_package_keys(root, requires_block, keys, key_problems);
    bool tags_broken = oamod::find_entry(root, "tags") == nullptr;
    for (const oamod::KeyProblem& problem : key_problems) {
        if (problem.path == "tags" || problem.path.rfind("tags[", 0) == 0)
            tags_broken = true;
        shared::add_problem(problems, problem.position, problem.path, problem.message);
    }
    manifest.homepage = std::move(keys.homepage);
    manifest.requires_engine = std::move(keys.requires_engine);
    if (const oamod::Node* tags = oamod::find_entry(root, "tags")) {
        if (!tags_broken && keys.tags.size() > most_tags) {
            shared::add_problem(
                problems,
                tags->position,
                "tags",
                "tags lists " + std::to_string(keys.tags.size()) + " tags; at most " +
                    std::to_string(most_tags)
            );
        } else if (!tags_broken) {
            bool tags_ok = true;
            for (std::size_t index = 0; index < keys.tags.size(); ++index) {
                if (brain_tag(keys.tags[index]))
                    continue;
                shared::add_problem(
                    problems,
                    tags->children[index].position,
                    "tags[" + std::to_string(index) + "]",
                    "tags[" + std::to_string(index) + "] must be one of " + tag_names()
                );
                tags_ok = false;
            }
            if (tags_ok)
                manifest.tags = std::move(keys.tags);
        }
    } else {
        shared::add_problem(problems, root.position, "tags", "tags is required");
    }

    if (const oamod::Node* icon = oamod::find_entry(root, "icon")) {
        if (icon->kind != oamod::NodeKind::string || icon->text != icon_file) {
            shared::add_problem(
                problems,
                icon->position,
                "icon",
                "icon must be icon.png, the picture at the package's top"
            );
        } else {
            manifest.icon = icon->text;
        }
    }

    if (const oamod::Node* works = oamod::find_entry(root, "works-with"))
        shared::read_works_with(*works, manifest.works_with, problems);
    else
        shared::add_problem(problems, root.position, "works-with", "works-with is required");

    if (const oamod::Node* levels = oamod::find_entry(root, "levels"))
        read_levels(*levels, manifest.levels, problems);
    else
        shared::add_problem(problems, root.position, "levels", "levels is required");

    if (const oamod::Node* awareness = oamod::find_entry(root, "fullAwareness")) {
        if (awareness->kind != oamod::NodeKind::boolean) {
            shared::add_problem(
                problems,
                awareness->position,
                "fullAwareness",
                "fullAwareness must be true or false"
            );
        } else {
            manifest.full_awareness = awareness->boolean;
        }
    }

    if (const oamod::Node* networks = oamod::find_entry(root, "networks"))
        read_networks(*networks, manifest.networks, problems);

    if (const oamod::Node* options = oamod::find_entry(root, "options"))
        shared::read_options(*options, manifest.options, problems);

    if (const oamod::Node* strings = oamod::find_entry(root, "strings")) {
        std::vector<oamut::Translation> words;
        shared::read_strings(*strings, manifest.options, level_language_key, words, problems);
        if (strings->kind == oamod::NodeKind::mapping) {
            for (oamut::Translation& word : words) {
                Translation translation;
                translation.words = std::move(word);
                if (const oamod::Node* language =
                        oamod::find_entry(*strings, translation.words.language)) {
                    if (language->kind == oamod::NodeKind::mapping) {
                        if (const oamod::Node* names = oamod::find_entry(*language, "levels"))
                            read_translated_levels(
                                *names,
                                translation.words.language,
                                manifest.levels,
                                translation,
                                problems
                            );
                    }
                }
                manifest.strings.push_back(std::move(translation));
            }
        }
    }

    manifest.entry = std::string{default_entry};
    if (const oamod::Node* entry = oamod::find_entry(root, "entry")) {
        if (entry->kind != oamod::NodeKind::string || !entry_file(entry->text)) {
            shared::add_problem(
                problems,
                entry->position,
                "entry",
                "entry must be a Lua file at the package's top, named with lower-case letters, "
                "digits and _"
            );
        } else {
            manifest.entry = entry->text;
        }
    }
}

/// The first line of a description, cut to most_summary_bytes on a character boundary.
///
/// @param description the description
/// @return the summary
std::string cut_summary(std::string_view description) {
    const std::size_t break_at = description.find('\n');
    if (break_at != std::string_view::npos)
        description = description.substr(0, break_at);
    if (description.size() <= most_summary_bytes)
        return std::string{description};
    std::size_t cut = most_summary_bytes;
    while (cut > 0 && (static_cast<unsigned char>(description[cut]) & 0xC0) == 0x80)
        --cut;
    return std::string{description.substr(0, cut)};
}

} // namespace

bool brain_id(std::string_view text) noexcept {
    return oamut::mutator_id(text);
}

bool brain_tag(std::string_view text) noexcept {
    for (const std::string_view tag : brain_tags)
        if (tag == text)
            return true;
    return false;
}

bool level_id(std::string_view text) noexcept {
    return text.size() <= most_level_id_bytes && oamut::mutator_id(text);
}

static_assert(static_cast<uint8_t>(Difficulty::easy) == 0);
static_assert(static_cast<uint8_t>(Difficulty::medium) == 1);
static_assert(static_cast<uint8_t>(Difficulty::hard) == 2);

const char* difficulty_name(Difficulty difficulty) noexcept {
    switch (difficulty) {
    case Difficulty::easy:
        return "easy";
    case Difficulty::medium:
        return "medium";
    case Difficulty::hard:
        return "hard";
    }
    return "easy";
}

std::optional<Difficulty> parse_difficulty(std::string_view text) noexcept {
    if (text == "easy")
        return Difficulty::easy;
    if (text == "medium")
        return Difficulty::medium;
    if (text == "hard")
        return Difficulty::hard;
    return std::nullopt;
}

bool read_manifest(
    std::span<const uint8_t> bytes, Manifest& manifest, std::vector<Problem>& problems
) {
    oamod::Node root;
    oamod::ReadError error{};
    std::vector<Problem> found;
    if (!oamod::read_document(bytes, root, error)) {
        const uint32_t line = error.position.line == 0 ? 1 : error.position.line;
        const uint32_t column = error.position.column == 0 ? 1 : error.position.column;
        found.push_back(Problem{line, column, "document", oamod::rule_message(error.rule)});
    } else {
        Manifest read;
        read_document_manifest(root, read, found);
        if (found.empty()) {
            manifest = std::move(read);
            return true;
        }
    }
    problems.insert(problems.end(), found.begin(), found.end());
    return false;
}

std::string summary_of(const Manifest& manifest) {
    return cut_summary(manifest.description);
}

std::string describe(const Problem& problem) {
    return std::string{manifest_file} + ":" + std::to_string(problem.line) + ":" +
           std::to_string(problem.column) + ": " + problem.key + ": " + problem.message;
}

const Level* find_level(const Manifest& manifest, std::string_view id) noexcept {
    for (const Level& level : manifest.levels)
        if (level.id == id)
            return &level;
    return nullptr;
}

} // namespace oa::formats::oabrain
