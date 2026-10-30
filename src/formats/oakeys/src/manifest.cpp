// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/oakeys/manifest.hpp"

#include "detail.hpp"

#include "oa/formats/oamod.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/formats/oamut/shared.hpp"

#include <array>
#include <cstdint>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::formats::oakeys {
namespace {

namespace oamod = oa::formats::oamod;
namespace oamut = oa::formats::oamut;
namespace shared = oa::formats::oamut::shared;

/// The top-level keys a manifest may hold, in the order they are written.
constexpr std::array<std::string_view, 18> manifest_keys{
    "oakeys",
    "id",
    "name",
    "version",
    "author",
    "description",
    "homepage",
    "icon",
    "tags",
    "base",
    "works-with",
    "requires",
    "keyboard",
    "pointer",
    "bindings",
    "helpers",
    "strings",
    "imported-from",
};

/// The keys of requires. A layout names an engine and nothing else.
constexpr std::array<std::string_view, 1> requires_keys{"engine"};

/// The keys of helpers, in the order they are written.
constexpr std::array<std::string_view, 6> helper_keys{
    "toggle",
    "sticky-modifiers",
    "hold-to-confirm",
    "pointer-keys",
    "radial-orders",
    "edge-scroll",
};

/// The keys of imported-from.
constexpr std::array<std::string_view, 3> imported_keys{"tool", "file", "date"};

/// The most bytes of an import's tool name.
constexpr std::size_t most_tool_bytes = 64;

/// The most bytes of an import's file name.
constexpr std::size_t most_file_bytes = 256;

/// The most bytes of an import's date.
constexpr std::size_t most_date_bytes = 32;

static_assert(most_works_with == oamut::most_works_with);
static_assert(most_languages == oamut::most_languages);
static_assert(std::size(manifest_keys) == 18);

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

/// Joins a block path and a child key.
///
/// @param block the parent path, or empty at the top of a paste
/// @param key the child key
/// @return the child path
std::string child_path(std::string_view block, std::string_view key) {
    if (block.empty())
        return std::string{key};
    return std::string{block} + "." + std::string{key};
}

/// Joins a list path and a 0-based index.
///
/// @param block the list path
/// @param index the index
/// @return the item path
std::string index_path(std::string_view block, std::size_t index) {
    return std::string{block} + "[" + std::to_string(index) + "]";
}

/// Tells whether a segment is a kebab word: letters and digits, hyphens
/// only between them.
///
/// @param segment the segment
/// @return true when it has that shape and is not empty
bool kebab_segment(std::string_view segment) noexcept {
    if (segment.empty() || segment.front() == '-' || segment.back() == '-')
        return false;
    char previous = 0;
    for (const char character : segment) {
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

/// Tells whether text is an action id.
///
/// Lower-case kebab-case words joined by dots, 1 to 96 bytes, at least two
/// words. No empty word, and no hyphen at the end of a word.
///
/// @param text the id
/// @return true when it is such an id
bool action_id(std::string_view text) noexcept {
    if (text.empty() || text.size() > most_action_id_bytes)
        return false;
    if (text.front() == '.' || text.back() == '.')
        return false;
    bool dotted = false;
    std::size_t start = 0;
    for (std::size_t index = 0; index <= text.size(); ++index) {
        if (index != text.size() && text[index] != '.')
            continue;
        if (!kebab_segment(text.substr(start, index - start)))
            return false;
        if (index != text.size())
            dotted = true;
        start = index + 1;
    }
    return dotted;
}

/// Tells whether text is one of the built-in layout ids.
///
/// @param text the id
/// @return true for oa-default or classic
bool built_in_id(std::string_view text) noexcept {
    for (const std::string_view id : built_in_ids)
        if (id == text)
            return true;
    return false;
}

/// Tells whether text ends with a suffix, byte for byte.
///
/// @param text the text
/// @param suffix the suffix
/// @return true when it ends with that suffix
bool ends_with(std::string_view text, std::string_view suffix) noexcept {
    return text.size() >= suffix.size() && text.substr(text.size() - suffix.size()) == suffix;
}

/// Reads a list of action ids.
///
/// @param node the list
/// @param block the list's path
/// @param[out] ids the ids, when every entry passed
/// @param[in,out] problems the problems
/// @return true when the list was kept
bool read_action_list(
    const oamod::Node& node,
    std::string_view block,
    std::vector<std::string>& ids,
    std::vector<Problem>& problems
) {
    if (node.kind != oamod::NodeKind::sequence) {
        shared::add_problem(
            problems,
            node.position,
            std::string{block},
            std::string{block} + " must be a list of action ids"
        );
        return false;
    }
    if (node.children.size() > most_bindings) {
        shared::add_problem(
            problems,
            node.position,
            std::string{block},
            std::string{block} + " lists " + std::to_string(node.children.size()) +
                " actions; at most " + std::to_string(most_bindings)
        );
        return false;
    }
    std::vector<std::string> read;
    bool passed = true;
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        const oamod::Node& item = node.children[index];
        const std::string path = index_path(block, index);
        if (item.kind != oamod::NodeKind::string || !action_id(item.text)) {
            shared::add_problem(
                problems,
                item.position,
                path,
                path + " must be lower-case kebab-case words joined by dots"
            );
            passed = false;
            continue;
        }
        read.push_back(item.text);
    }
    if (!passed)
        return false;
    ids = std::move(read);
    return true;
}

/// Reads one short import string.
///
/// @param node the value
/// @param block the value's path
/// @param limit the most bytes
/// @param[out] text the text, when it passed
/// @param[in,out] problems the problems
/// @return true when `text` was set
bool read_import_text(
    const oamod::Node& node,
    std::string_view block,
    std::size_t limit,
    std::string& text,
    std::vector<Problem>& problems
) {
    if (node.kind != oamod::NodeKind::string || node.text.empty() || node.text.size() > limit ||
        shared::has_control(node.text, false)) {
        shared::add_problem(
            problems,
            node.position,
            std::string{block},
            std::string{block} + " must be 1 to " + std::to_string(limit) +
                " bytes with no control character"
        );
        return false;
    }
    text = node.text;
    return true;
}

/// Reads helpers. A key the section does not name is refused.
///
/// @param node the helpers value
/// @param[out] helpers the helpers
/// @param[in,out] problems the problems
void read_helpers(const oamod::Node& node, Helpers& helpers, std::vector<Problem>& problems) {
    if (node.kind != oamod::NodeKind::mapping) {
        shared::add_problem(problems, node.position, "helpers", "helpers must be a mapping");
        return;
    }
    shared::refuse_unknown_keys(node, helper_keys, "helpers", problems);
    if (const oamod::Node* toggle = oamod::find_entry(node, "toggle")) {
        std::vector<std::string> ids;
        if (read_action_list(*toggle, "helpers.toggle", ids, problems))
            helpers.toggle = std::move(ids);
    }
    if (const oamod::Node* sticky = oamod::find_entry(node, "sticky-modifiers")) {
        if (sticky->kind != oamod::NodeKind::boolean) {
            shared::add_problem(
                problems,
                sticky->position,
                "helpers.sticky-modifiers",
                "sticky-modifiers must be true or false"
            );
        } else {
            helpers.sticky_modifiers = sticky->boolean;
        }
    }
    if (const oamod::Node* confirm = oamod::find_entry(node, "hold-to-confirm")) {
        std::vector<std::string> ids;
        if (read_action_list(*confirm, "helpers.hold-to-confirm", ids, problems))
            helpers.hold_to_confirm = std::move(ids);
    }
    if (const oamod::Node* pointer_keys = oamod::find_entry(node, "pointer-keys")) {
        if (pointer_keys->kind != oamod::NodeKind::boolean) {
            shared::add_problem(
                problems,
                pointer_keys->position,
                "helpers.pointer-keys",
                "pointer-keys must be true or false"
            );
        } else {
            helpers.pointer_keys = pointer_keys->boolean;
        }
    }
    if (const oamod::Node* radial = oamod::find_entry(node, "radial-orders")) {
        if (radial->kind != oamod::NodeKind::string) {
            shared::add_problem(
                problems,
                radial->position,
                "helpers.radial-orders",
                "radial-orders must be off, hold-right or hold-middle"
            );
        } else if (radial->text == "off") {
            helpers.radial_orders = RadialOrders::off;
        } else if (radial->text == "hold-right") {
            helpers.radial_orders = RadialOrders::hold_right;
        } else if (radial->text == "hold-middle") {
            helpers.radial_orders = RadialOrders::hold_middle;
        } else {
            shared::add_problem(
                problems,
                radial->position,
                "helpers.radial-orders",
                "radial-orders must be off, hold-right or hold-middle"
            );
        }
    }
    if (const oamod::Node* edge = oamod::find_entry(node, "edge-scroll")) {
        if (edge->kind != oamod::NodeKind::string) {
            shared::add_problem(
                problems, edge->position, "helpers.edge-scroll", "edge-scroll must be on or off"
            );
        } else if (edge->text == "on") {
            helpers.edge_scroll = EdgeScroll::on;
        } else if (edge->text == "off") {
            helpers.edge_scroll = EdgeScroll::off;
        } else {
            shared::add_problem(
                problems, edge->position, "helpers.edge-scroll", "edge-scroll must be on or off"
            );
        }
    }
}

/// Reads imported-from. Each of its three keys is required.
///
/// @param node the value
/// @param[out] imported the record, when every field passed
/// @param[in,out] problems the problems
void read_imported(
    const oamod::Node& node, ImportedFrom& imported, std::vector<Problem>& problems
) {
    if (node.kind != oamod::NodeKind::mapping) {
        shared::add_problem(
            problems, node.position, "imported-from", "imported-from must be a mapping"
        );
        return;
    }
    shared::refuse_unknown_keys(node, imported_keys, "imported-from", problems);
    bool passed = true;
    if (const oamod::Node* tool = oamod::find_entry(node, "tool")) {
        if (!read_import_text(
                *tool, "imported-from.tool", most_tool_bytes, imported.tool, problems
            ))
            passed = false;
    } else {
        shared::add_problem(
            problems, node.position, "imported-from.tool", "imported-from.tool is required"
        );
        passed = false;
    }
    if (const oamod::Node* file = oamod::find_entry(node, "file")) {
        if (!read_import_text(
                *file, "imported-from.file", most_file_bytes, imported.file, problems
            ))
            passed = false;
    } else {
        shared::add_problem(
            problems, node.position, "imported-from.file", "imported-from.file is required"
        );
        passed = false;
    }
    if (const oamod::Node* date = oamod::find_entry(node, "date")) {
        if (!read_import_text(
                *date, "imported-from.date", most_date_bytes, imported.date, problems
            ))
            passed = false;
    } else {
        shared::add_problem(
            problems, node.position, "imported-from.date", "imported-from.date is required"
        );
        passed = false;
    }
    if (!passed)
        imported = ImportedFrom{};
}

/// Reads one keyboard name.
///
/// @param text the name
/// @return the keyboard, or nothing
std::optional<Keyboard> parse_keyboard(std::string_view text) noexcept {
    if (text == "any")
        return Keyboard::any;
    if (text == "ansi")
        return Keyboard::ansi;
    if (text == "iso")
        return Keyboard::iso;
    if (text == "jis")
        return Keyboard::jis;
    return std::nullopt;
}

/// Reads one pointer scheme.
///
/// @param text the name
/// @return the scheme, or nothing
std::optional<Pointer> parse_pointer(std::string_view text) noexcept {
    if (text == "right-click")
        return Pointer::right_click;
    if (text == "left-click")
        return Pointer::left_click;
    return std::nullopt;
}

/// Reads the header and every block into a fresh manifest.
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
        shared::add_problem(
            problems,
            entry.position,
            entry.key.text,
            entry.key.text + " is not a key of oakeys.yaml"
        );
    }

    const oamod::Node* format = oamod::find_entry(root, "oakeys");
    int64_t format_number = 0;
    if (format == nullptr || format->kind != oamod::NodeKind::number || !format->number.integer ||
        !oamod::integer_value(format->number, format_number) || format_number != format_version) {
        shared::add_problem(
            problems,
            format != nullptr ? format->position : root.position,
            "oakeys",
            "oakeys must be 1"
        );
    } else {
        manifest.format = format_number;
    }

    const oamod::Node* id = oamod::find_entry(root, "id");
    if (id == nullptr || id->kind != oamod::NodeKind::string || !oamut::mutator_id(id->text)) {
        shared::add_problem(
            problems,
            id != nullptr ? id->position : root.position,
            "id",
            "id must be 1 to " + std::to_string(most_id_bytes) + " bytes of lower-case kebab-case"
        );
    } else if (built_in_id(id->text)) {
        shared::add_problem(problems, id->position, "id", "id is a built-in layout");
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

    if (const oamod::Node* version = oamod::find_entry(root, "version")) {
        if (version->kind == oamod::NodeKind::number ||
            (version->kind == oamod::NodeKind::string && !version->quoted)) {
            shared::add_problem(
                problems, version->position, "version", "write the version in quotes, as \"1.0\""
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
                    "version must be a mutator version, such as \"1.0\""
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
    }

    const oamod::Node* requires_node = oamod::find_entry(root, "requires");
    const oamod::Node* requires_block = nullptr;
    if (requires_node != nullptr) {
        if (requires_node->kind != oamod::NodeKind::mapping) {
            shared::add_problem(
                problems,
                requires_node->position,
                "requires",
                "requires must be a mapping of engine"
            );
        } else {
            requires_block = requires_node;
            shared::refuse_unknown_keys(*requires_node, requires_keys, "requires", problems);
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
            manifest.tags = std::move(keys.tags);
        }
    } else {
        shared::add_problem(problems, root.position, "tags", "tags is required");
    }

    if (const oamod::Node* icon = oamod::find_entry(root, "icon")) {
        if (icon->kind != oamod::NodeKind::string || !shared::package_path(icon->text) ||
            !ends_with(icon->text, ".png")) {
            shared::add_problem(
                problems,
                icon->position,
                "icon",
                "icon must be a relative path ending in .png, with / separators and no empty, '.' "
                "or '..' part"
            );
        } else {
            manifest.icon = icon->text;
        }
    }

    if (const oamod::Node* base = oamod::find_entry(root, "base")) {
        const std::optional<Base> parsed =
            base->kind == oamod::NodeKind::string ? parse_base(base->text) : std::nullopt;
        if (!parsed) {
            shared::add_problem(
                problems, base->position, "base", "base must be oa-default or none"
            );
        } else {
            manifest.base = *parsed;
        }
    } else {
        shared::add_problem(problems, root.position, "base", "base is required");
    }

    if (const oamod::Node* works = oamod::find_entry(root, "works-with"))
        shared::read_works_with(*works, manifest.works_with, problems);

    if (const oamod::Node* keyboard = oamod::find_entry(root, "keyboard")) {
        const std::optional<Keyboard> parsed = keyboard->kind == oamod::NodeKind::string
                                                   ? parse_keyboard(keyboard->text)
                                                   : std::nullopt;
        if (!parsed) {
            shared::add_problem(
                problems, keyboard->position, "keyboard", "keyboard must be any, ansi, iso or jis"
            );
        } else {
            manifest.keyboard = *parsed;
        }
    }

    if (const oamod::Node* pointer = oamod::find_entry(root, "pointer")) {
        const std::optional<Pointer> parsed =
            pointer->kind == oamod::NodeKind::string ? parse_pointer(pointer->text) : std::nullopt;
        if (!parsed) {
            shared::add_problem(
                problems, pointer->position, "pointer", "pointer must be right-click or left-click"
            );
        } else {
            manifest.pointer = *parsed;
        }
    }

    if (const oamod::Node* bindings = oamod::find_entry(root, "bindings")) {
        if (bindings->kind != oamod::NodeKind::mapping) {
            shared::add_problem(
                problems, bindings->position, "bindings", "bindings must be a mapping of action ids"
            );
        } else {
            read_binding_mapping(*bindings, "bindings", manifest.bindings, problems);
        }
    }

    if (const oamod::Node* helpers = oamod::find_entry(root, "helpers"))
        read_helpers(*helpers, manifest.helpers, problems);

    if (const oamod::Node* strings = oamod::find_entry(root, "strings")) {
        std::vector<oamut::Option> options;
        shared::read_strings(*strings, options, {}, manifest.strings, problems);
    }

    if (const oamod::Node* imported = oamod::find_entry(root, "imported-from")) {
        ImportedFrom record;
        const std::size_t before = problems.size();
        read_imported(*imported, record, problems);
        if (problems.size() == before && !record.tool.empty())
            manifest.imported_from = std::move(record);
    }
}

} // namespace

const char* base_name(Base base) noexcept {
    switch (base) {
    case Base::oa_default:
        return "oa-default";
    case Base::none:
        return "none";
    }
    return "oa-default";
}

std::optional<Base> parse_base(std::string_view text) noexcept {
    if (text == "oa-default")
        return Base::oa_default;
    if (text == "none")
        return Base::none;
    return std::nullopt;
}

void read_binding_mapping(
    const oamod::Node& node,
    std::string_view block,
    std::vector<BindingEntry>& bindings,
    std::vector<Problem>& problems
) {
    const std::string count_key = block.empty() ? std::string{"bindings"} : std::string{block};
    if (node.children.size() > most_bindings) {
        shared::add_problem(
            problems,
            node.position,
            count_key,
            count_key + " lists " + std::to_string(node.children.size()) + " actions; at most " +
                std::to_string(most_bindings)
        );
    }
    for (const oamod::Node& entry : node.children) {
        const std::string path = child_path(block, entry.key.text);
        if (entry.key.kind != oamod::KeyKind::string || !action_id(entry.key.text)) {
            shared::add_problem(
                problems,
                entry.key.position,
                path,
                path + " must be lower-case kebab-case words joined by dots"
            );
            continue;
        }
        if (entry.kind != oamod::NodeKind::sequence) {
            shared::add_problem(problems, entry.position, path, path + " must be a list of chords");
            continue;
        }
        BindingEntry binding;
        binding.action = entry.key.text;
        binding.line = entry.key.position.line;
        binding.column = entry.key.position.column;
        bool chords_ok = true;
        if (entry.children.size() > most_chords) {
            shared::add_problem(
                problems,
                entry.position,
                path,
                path + " lists " + std::to_string(entry.children.size()) + " chords; at most " +
                    std::to_string(most_chords)
            );
            chords_ok = false;
        }
        for (std::size_t index = 0; index < entry.children.size(); ++index) {
            const oamod::Node& chord = entry.children[index];
            const std::string chord_path = index_path(path, index);
            if (chord.kind != oamod::NodeKind::string) {
                shared::add_problem(
                    problems, chord.position, chord_path, chord_path + " must be a string"
                );
                chords_ok = false;
                continue;
            }
            ChordText text;
            text.text = chord.text;
            text.line = chord.position.line;
            text.column = chord.position.column;
            binding.chords.push_back(std::move(text));
        }
        if (chords_ok)
            bindings.push_back(std::move(binding));
    }
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

std::string describe(const Problem& problem) {
    return std::string{manifest_file} + ":" + std::to_string(problem.line) + ":" +
           std::to_string(problem.column) + ": " + problem.key + ": " + problem.message;
}

} // namespace oa::formats::oakeys
