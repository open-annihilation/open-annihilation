// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/oakeys/manifest.hpp"

#include "detail.hpp"

#include "oa/formats/oamod.hpp"
#include "oa/formats/oamut/shared.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace oa::formats::oakeys {
namespace {

namespace oamod = oa::formats::oamod;
namespace oamut = oa::formats::oamut;
namespace shared = oa::formats::oamut::shared;

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

/// Tells whether a bare chord would be a number, a boolean or null.
///
/// @param text the chord
/// @return true when the strict reader would not keep it as a string
bool plain_special(std::string_view text) {
    if (text.empty() || text == "~" || text == "null" || text == "Null" || text == "NULL")
        return true;
    if (text == "true" || text == "True" || text == "TRUE" || text == "false" || text == "False" ||
        text == "FALSE")
        return true;
    oamod::Number number{};
    return oamod::parse_number(text, number) != oamod::NumberStatus::not_a_number;
}

/// Tells whether a chord may be written without quotes.
///
/// @param text the chord
/// @return true when every character is a letter, a digit or `+`, and the
///         text is not a number, a boolean or null
bool bare_chord(std::string_view text) {
    if (plain_special(text))
        return false;
    for (const unsigned char character : text) {
        const bool word = (character >= 'A' && character <= 'Z') ||
                          (character >= 'a' && character <= 'z') ||
                          (character >= '0' && character <= '9') || character == '+';
        if (!word)
            return false;
    }
    return true;
}

/// Returns a keyboard's manifest name.
///
/// @param keyboard the keyboard
/// @return any, ansi, iso or jis
const char* keyboard_name(Keyboard keyboard) noexcept {
    switch (keyboard) {
    case Keyboard::any:
        return "any";
    case Keyboard::ansi:
        return "ansi";
    case Keyboard::iso:
        return "iso";
    case Keyboard::jis:
        return "jis";
    }
    return "any";
}

/// Returns a pointer scheme's manifest name.
///
/// @param pointer the scheme
/// @return right-click or left-click
const char* pointer_name(Pointer pointer) noexcept {
    switch (pointer) {
    case Pointer::right_click:
        return "right-click";
    case Pointer::left_click:
        return "left-click";
    }
    return "right-click";
}

/// Returns a radial-orders value's name.
///
/// @param orders the value
/// @return off, hold-right or hold-middle
const char* radial_name(RadialOrders orders) noexcept {
    switch (orders) {
    case RadialOrders::off:
        return "off";
    case RadialOrders::hold_right:
        return "hold-right";
    case RadialOrders::hold_middle:
        return "hold-middle";
    }
    return "off";
}

/// Returns an edge-scroll value's name.
///
/// @param edge the value
/// @return on or off
const char* edge_name(EdgeScroll edge) noexcept {
    switch (edge) {
    case EdgeScroll::on:
        return "on";
    case EdgeScroll::off:
        return "off";
    }
    return "off";
}

/// Writes one language of a strings block.
///
/// @param[in,out] out the text
/// @param translation the translation
void write_translation(std::string& out, const oamut::Translation& translation) {
    const bool any = !translation.name.empty() || !translation.description.empty() ||
                     !translation.options.empty() || !translation.labels.empty();
    if (!any) {
        field(out, 1, translation.language, "{}");
        return;
    }
    line(out, 1, translation.language + ":");
    if (!translation.name.empty())
        field(out, 2, "name", shared::quoted(translation.name));
    if (!translation.description.empty())
        field(out, 2, "description", shared::quoted(translation.description));
    if (!translation.options.empty()) {
        line(out, 2, "options:");
        for (const oamut::OptionLabel& label : translation.options)
            field(out, 3, label.id, shared::quoted(label.label));
    }
    if (!translation.labels.empty()) {
        line(out, 2, "labels:");
        for (const oamut::ValueLabels& labels : translation.labels) {
            std::vector<std::string> items;
            for (const std::string& label : labels.labels)
                items.push_back(shared::quoted(label));
            write_list(out, 3, labels.id, items);
        }
    }
}

/// Writes the action ids of one helper, already quoted.
///
/// @param[in,out] out the text
/// @param key the helper's key
/// @param ids the action ids
void write_actions(std::string& out, std::string_view key, const std::vector<std::string>& ids) {
    std::vector<std::string> items;
    for (const std::string& id : ids)
        items.push_back(shared::quoted(id));
    write_list(out, 1, key, items);
}

/// Tells whether any helper was set.
///
/// @param helpers the helpers
/// @return true when one of the six keys was written
bool helpers_set(const Helpers& helpers) noexcept {
    return helpers.toggle.has_value() || helpers.sticky_modifiers.has_value() ||
           helpers.hold_to_confirm.has_value() || helpers.pointer_keys.has_value() ||
           helpers.radial_orders.has_value() || helpers.edge_scroll.has_value();
}

/// Writes the helpers block. Nothing is written when every helper was left out.
///
/// @param[in,out] out the text
/// @param helpers the helpers
void write_helpers(std::string& out, const Helpers& helpers) {
    if (!helpers_set(helpers))
        return;
    line(out, 0, "helpers:");
    if (helpers.toggle.has_value())
        write_actions(out, "toggle", *helpers.toggle);
    if (helpers.sticky_modifiers.has_value())
        field(out, 1, "sticky-modifiers", *helpers.sticky_modifiers ? "true" : "false");
    if (helpers.hold_to_confirm.has_value())
        write_actions(out, "hold-to-confirm", *helpers.hold_to_confirm);
    if (helpers.pointer_keys.has_value())
        field(out, 1, "pointer-keys", *helpers.pointer_keys ? "true" : "false");
    if (helpers.radial_orders.has_value())
        field(out, 1, "radial-orders", radial_name(*helpers.radial_orders));
    if (helpers.edge_scroll.has_value())
        field(out, 1, "edge-scroll", edge_name(*helpers.edge_scroll));
}

} // namespace

std::string chord_scalar(std::string_view text) {
    if (bare_chord(text))
        return std::string{text};
    return shared::quoted(text);
}

std::string write_binding_lines(const std::vector<BindingEntry>& bindings, int indent) {
    std::string out;
    for (const BindingEntry& binding : bindings) {
        std::string flow = "[";
        for (std::size_t index = 0; index < binding.chords.size(); ++index) {
            if (index != 0)
                flow += ", ";
            flow += chord_scalar(binding.chords[index].text);
        }
        flow += "]";
        line(out, indent, binding.action + ": " + flow);
    }
    return out;
}

std::string write_manifest(const Manifest& manifest) {
    std::string out;
    field(out, 0, "oakeys", std::to_string(manifest.format));
    field(out, 0, "id", shared::quoted(manifest.id));
    field(out, 0, "name", shared::quoted(manifest.name));
    field(out, 0, "version", shared::quoted(oamut::version_text(manifest.version)));
    shared::write_author(out, manifest.author);
    if (!manifest.description.empty())
        field(out, 0, "description", shared::quoted(manifest.description));
    if (!manifest.homepage.empty())
        field(out, 0, "homepage", shared::quoted(manifest.homepage));
    if (!manifest.icon.empty())
        field(out, 0, "icon", shared::quoted(manifest.icon));
    std::vector<std::string> tags;
    for (const std::string& tag : manifest.tags)
        tags.push_back(shared::quoted(tag));
    write_list(out, 0, "tags", tags);
    field(out, 0, "base", base_name(manifest.base));
    if (!manifest.works_with.empty()) {
        std::vector<std::string> works;
        for (const oamut::Condition& condition : manifest.works_with)
            works.push_back(shared::condition_text(condition));
        write_list(out, 0, "works-with", works);
    }
    if (!manifest.requires_engine.empty()) {
        line(out, 0, "requires:");
        field(out, 1, "engine", shared::quoted(manifest.requires_engine));
    }
    if (manifest.keyboard != Keyboard::any)
        field(out, 0, "keyboard", keyboard_name(manifest.keyboard));
    if (manifest.pointer.has_value())
        field(out, 0, "pointer", pointer_name(*manifest.pointer));
    if (!manifest.bindings.empty()) {
        line(out, 0, "bindings:");
        out += write_binding_lines(manifest.bindings, 1);
    }
    write_helpers(out, manifest.helpers);
    if (!manifest.strings.empty()) {
        line(out, 0, "strings:");
        for (const oamut::Translation& translation : manifest.strings)
            write_translation(out, translation);
    }
    if (manifest.imported_from.has_value()) {
        line(out, 0, "imported-from:");
        field(out, 1, "tool", shared::quoted(manifest.imported_from->tool));
        field(out, 1, "file", shared::quoted(manifest.imported_from->file));
        field(out, 1, "date", shared::quoted(manifest.imported_from->date));
    }
    return out;
}

} // namespace oa::formats::oakeys
