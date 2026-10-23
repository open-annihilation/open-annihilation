// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/oamut/manifest.hpp"

#include "oa/formats/oamod.hpp"
#include "oa/formats/oamut/shared.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::formats::oamut {
namespace {

/// Appends a line at an indentation of two spaces per level.
///
/// @param[in,out] out the text
/// @param indent how many levels in the line sits
/// @param text the line, without its line break
void line(std::string& out, int indent, std::string_view text) {
    out.append(static_cast<std::size_t>(indent) * 2, ' ');
    out.append(text);
    out.push_back('\n');
}

/// Writes a string as a double-quoted YAML scalar.
///
/// @param text the string
/// @return the quoted scalar, quotes included
std::string quoted(std::string_view text) {
    std::string out;
    out.push_back('"');
    for (const unsigned char character : text) {
        switch (character) {
        case '\\':
            out += "\\\\";
            break;
        case '"':
            out += "\\\"";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        default:
            if (character < 0x20) {
                constexpr char hex[] = "0123456789abcdef";
                out += "\\x";
                out.push_back(hex[character >> 4]);
                out.push_back(hex[character & 0x0F]);
            } else {
                out.push_back(static_cast<char>(character));
            }
            break;
        }
    }
    out.push_back('"');
    return out;
}

/// Writes one key and a value that is already in the form to write.
///
/// @param[in,out] out the text
/// @param indent how many levels in the key sits
/// @param key the key
/// @param value the value
void field(std::string& out, int indent, std::string_view key, std::string_view value) {
    line(out, indent, std::string{key} + ": " + std::string{value});
}

/// Writes a list as a flow sequence when the line is short, and as a block otherwise.
///
/// @param[in,out] out the text
/// @param indent how many levels in the key sits
/// @param key the key
/// @param items the items, each already a scalar
void write_list(
    std::string& out, int indent, std::string_view key, const std::vector<std::string>& items
) {
    std::string flow = "[";
    for (std::size_t index = 0; index < items.size(); ++index) {
        if (index != 0)
            flow += ", ";
        flow += items[index];
    }
    flow += "]";
    const std::size_t width = static_cast<std::size_t>(indent) * 2 + key.size() + 2 + flow.size();
    if (width <= 100) {
        field(out, indent, key, flow);
        return;
    }
    line(out, indent, std::string{key} + ":");
    for (const std::string& item : items)
        line(out, indent + 1, "- " + item);
}

/// Writes a condition as a quoted scalar.
///
/// @param condition the condition
/// @return the quoted text
std::string condition_scalar(const Condition& condition) {
    std::string text = condition.id;
    if (!condition.range_text.empty())
        text += " " + condition.range_text;
    return quoted(text);
}

/// Writes a choice value as a scalar.
///
/// @param value the value
/// @return a number's canonical text, or a quoted string
std::string choice_scalar(const ChoiceValue& value) {
    if (value.number)
        return oamod::canonical_number_text(value.number_value);
    return quoted(value.text);
}

/// Writes one option.
///
/// @param[in,out] out the text
/// @param option the option
void write_option(std::string& out, const Option& option) {
    line(out, 1, option.id + ":");
    field(
        out,
        2,
        "type",
        quoted(
            option.type == OptionType::toggle   ? "toggle"
            : option.type == OptionType::choice ? "choice"
                                                : "number"
        )
    );
    field(out, 2, "label", quoted(option.label));
    if (option.type == OptionType::choice) {
        std::vector<std::string> values;
        for (const ChoiceValue& value : option.values)
            values.push_back(choice_scalar(value));
        write_list(out, 2, "values", values);
        if (!option.value_labels.empty()) {
            std::vector<std::string> labels;
            for (const std::string& label : option.value_labels)
                labels.push_back(quoted(label));
            write_list(out, 2, "labels", labels);
        }
        if (option.default_index < option.values.size())
            field(out, 2, "default", choice_scalar(option.values[option.default_index]));
        return;
    }
    if (option.type == OptionType::number) {
        field(out, 2, "min", oamod::canonical_number_text(option.minimum));
        field(out, 2, "max", oamod::canonical_number_text(option.maximum));
        field(out, 2, "step", oamod::canonical_number_text(option.step));
        field(out, 2, "default", oamod::canonical_number_text(option.number_default));
        return;
    }
    field(out, 2, "default", option.toggle_default ? "true" : "false");
}

/// Writes one translation.
///
/// @param[in,out] out the text
/// @param translation the translation
void write_translation(std::string& out, const Translation& translation) {
    const bool any = !translation.name.empty() || !translation.description.empty() ||
                     !translation.options.empty() || !translation.labels.empty();
    if (!any) {
        field(out, 1, translation.language, "{}");
        return;
    }
    line(out, 1, translation.language + ":");
    if (!translation.name.empty())
        field(out, 2, "name", quoted(translation.name));
    if (!translation.description.empty())
        field(out, 2, "description", quoted(translation.description));
    if (!translation.options.empty()) {
        line(out, 2, "options:");
        for (const OptionLabel& label : translation.options)
            field(out, 3, label.id, quoted(label.label));
    }
    if (!translation.labels.empty()) {
        line(out, 2, "labels:");
        for (const ValueLabels& labels : translation.labels) {
            std::vector<std::string> items;
            for (const std::string& label : labels.labels)
                items.push_back(quoted(label));
            write_list(out, 3, labels.id, items);
        }
    }
}

/// Writes the top-level author block.
///
/// @param[in,out] out the text
/// @param author the author
void write_author_block(std::string& out, const Author& author) {
    line(out, 0, "author:");
    field(out, 1, "name", quoted(author.name));
    if (!author.email.empty())
        field(out, 1, "email", quoted(author.email));
}

/// Writes the options block. Nothing is written when `options` is empty.
///
/// @param[in,out] out the text
/// @param options the options
void write_options_block(std::string& out, const std::vector<Option>& options) {
    if (options.empty())
        return;
    line(out, 0, "options:");
    for (const Option& option : options)
        write_option(out, option);
}

/// The quoted-scalar writer, named apart from the shared entry point.
///
/// @param text the string
/// @return the quoted scalar
std::string relay_quoted(std::string_view text) {
    return quoted(text);
}

/// Writes one legacy record.
///
/// @param[in,out] out the text
/// @param record the record
/// @param first whether this is the first record, written beside the dash
void write_legacy(std::string& out, const Legacy& record) {
    line(out, 1, "- name: " + quoted(record.name));
    field(out, 2, "prefix", quoted(record.prefix));
    std::vector<std::string> codes;
    for (const std::string& code : record.compatibility)
        codes.push_back(quoted(code));
    write_list(out, 2, "compatibility", codes);
    std::vector<std::string> files;
    for (const std::string& file : record.files)
        files.push_back(quoted(file));
    write_list(out, 2, "files", files);
}

} // namespace

std::string write_manifest(const Manifest& manifest) {
    std::string out;
    field(out, 0, "oamut", std::to_string(manifest.format));
    field(out, 0, "id", quoted(manifest.id));
    field(out, 0, "name", quoted(manifest.name));
    field(out, 0, "version", quoted(version_text(manifest.version)));
    shared::write_author(out, manifest.author);
    if (!manifest.description.empty())
        field(out, 0, "description", quoted(manifest.description));
    if (!manifest.homepage.empty())
        field(out, 0, "homepage", quoted(manifest.homepage));
    if (!manifest.icon.empty())
        field(out, 0, "icon", quoted(manifest.icon));
    std::vector<std::string> tags;
    for (const std::string& tag : manifest.tags)
        tags.push_back(quoted(tag));
    write_list(out, 0, "tags", tags);
    std::vector<std::string> works;
    for (const Condition& condition : manifest.works_with)
        works.push_back(condition_scalar(condition));
    write_list(out, 0, "works-with", works);
    if (!manifest.requires_engine.empty() || !manifest.requires_mutators.empty()) {
        line(out, 0, "requires:");
        if (!manifest.requires_engine.empty())
            field(out, 1, "engine", quoted(manifest.requires_engine));
        if (!manifest.requires_mutators.empty()) {
            std::vector<std::string> mutators;
            for (const Condition& condition : manifest.requires_mutators)
                mutators.push_back(condition_scalar(condition));
            write_list(out, 1, "mutators", mutators);
        }
    }
    if (!manifest.conflicts_with.empty()) {
        std::vector<std::string> conflicts;
        for (const Condition& condition : manifest.conflicts_with)
            conflicts.push_back(condition_scalar(condition));
        write_list(out, 0, "conflicts-with", conflicts);
    }
    std::vector<std::string> layers;
    for (const Layer layer : manifest.layers)
        layers.push_back(quoted(layer_name(layer)));
    write_list(out, 0, "layers", layers);
    if (manifest.clashes.has_value())
        field(out, 0, "clashes", quoted(clash_mode_name(*manifest.clashes)));
    shared::write_options(out, manifest.options);
    if (!manifest.strings.empty()) {
        line(out, 0, "strings:");
        for (const Translation& translation : manifest.strings)
            write_translation(out, translation);
    }
    if (manifest.entry != default_entry)
        field(out, 0, "entry", quoted(manifest.entry));
    if (!manifest.legacy.empty()) {
        line(out, 0, "legacy:");
        for (const Legacy& record : manifest.legacy)
            write_legacy(out, record);
    }
    return out;
}

namespace shared {

std::string quoted(std::string_view text) {
    return relay_quoted(text);
}

std::string condition_text(const Condition& condition) {
    return condition_scalar(condition);
}

void write_author(std::string& out, const Author& author) {
    write_author_block(out, author);
}

void write_options(std::string& out, const std::vector<Option>& options) {
    write_options_block(out, options);
}

} // namespace shared

} // namespace oa::formats::oamut
