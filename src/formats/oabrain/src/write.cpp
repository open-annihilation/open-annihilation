// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/oabrain/manifest.hpp"

#include "oa/formats/oamut/shared.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace oa::formats::oabrain {
namespace {

namespace shared = oa::formats::oamut::shared;
namespace oamut = oa::formats::oamut;

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

/// Writes one language, then its level names.
///
/// @param[in,out] out the text
/// @param translation the translation
void write_translation(std::string& out, const Translation& translation) {
    const oamut::Translation& words = translation.words;
    const bool any = !words.name.empty() || !words.description.empty() || !words.options.empty() ||
                     !words.labels.empty() || !translation.levels.empty();
    if (!any) {
        field(out, 1, words.language, "{}");
        return;
    }
    line(out, 1, words.language + ":");
    if (!words.name.empty())
        field(out, 2, "name", shared::quoted(words.name));
    if (!words.description.empty())
        field(out, 2, "description", shared::quoted(words.description));
    if (!words.options.empty()) {
        line(out, 2, "options:");
        for (const oamut::OptionLabel& label : words.options)
            field(out, 3, label.id, shared::quoted(label.label));
    }
    if (!words.labels.empty()) {
        line(out, 2, "labels:");
        for (const oamut::ValueLabels& labels : words.labels) {
            std::vector<std::string> items;
            for (const std::string& label : labels.labels)
                items.push_back(shared::quoted(label));
            write_list(out, 3, labels.id, items);
        }
    }
    if (!translation.levels.empty()) {
        line(out, 2, "levels:");
        for (const LevelName& level : translation.levels)
            field(out, 3, level.id, shared::quoted(level.name));
    }
}

} // namespace

std::string write_manifest(const Manifest& manifest) {
    std::string out;
    field(out, 0, "oabrain", std::to_string(manifest.format));
    field(out, 0, "id", shared::quoted(manifest.id));
    field(out, 0, "name", shared::quoted(manifest.name));
    field(out, 0, "version", shared::quoted(oamut::version_text(manifest.version)));
    shared::write_author(out, manifest.author);
    field(out, 0, "description", shared::quoted(manifest.description));
    if (!manifest.homepage.empty())
        field(out, 0, "homepage", shared::quoted(manifest.homepage));
    if (!manifest.icon.empty())
        field(out, 0, "icon", shared::quoted(manifest.icon));
    std::vector<std::string> tags;
    for (const std::string& tag : manifest.tags)
        tags.push_back(shared::quoted(tag));
    write_list(out, 0, "tags", tags);
    std::vector<std::string> works;
    for (const oamut::Condition& condition : manifest.works_with)
        works.push_back(shared::condition_text(condition));
    write_list(out, 0, "works-with", works);
    if (!manifest.requires_engine.empty() || !manifest.requires_expansions.empty()) {
        line(out, 0, "requires:");
        if (!manifest.requires_engine.empty())
            field(out, 1, "engine", shared::quoted(manifest.requires_engine));
        if (!manifest.requires_expansions.empty()) {
            std::vector<std::string> expansions;
            for (const std::string& id : manifest.requires_expansions)
                expansions.push_back(shared::quoted(id));
            write_list(out, 1, "expansions", expansions);
        }
    }
    line(out, 0, "levels:");
    for (const Level& level : manifest.levels) {
        line(out, 1, "- id: " + shared::quoted(level.id));
        field(out, 2, "name", shared::quoted(level.name));
        field(out, 2, "difficulty", difficulty_name(level.difficulty));
    }
    if (manifest.full_awareness)
        field(out, 0, "fullAwareness", "true");
    if (!manifest.networks.empty()) {
        line(out, 0, "networks:");
        for (const Network& network : manifest.networks) {
            line(out, 1, "- id: " + shared::quoted(network.id));
            field(out, 2, "file", shared::quoted(network.file));
            field(out, 2, "sha256", shared::quoted(network.sha256));
        }
    }
    shared::write_options(out, manifest.options);
    if (!manifest.strings.empty()) {
        line(out, 0, "strings:");
        for (const Translation& translation : manifest.strings)
            write_translation(out, translation);
    }
    if (manifest.entry != default_entry)
        field(out, 0, "entry", shared::quoted(manifest.entry));
    return out;
}

} // namespace oa::formats::oabrain
