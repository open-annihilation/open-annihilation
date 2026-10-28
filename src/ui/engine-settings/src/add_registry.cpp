// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Add registry window's words, its placement at each size class, and
// what a pointer, a finger, a key and typed text do to it. The window is a
// notice's chrome: the class's notice width and height limits, and the
// Compact metrics for the text, the field, the buttons and the footer, as a
// notice uses them. Its buttons' places hang on its height alone, so a
// press lands where a button is drawn.

#include "oa/ui/engine_settings/add_registry.hpp"

#include "oa/data/languages/interface_text.hpp"
#include "oa/formats/url.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::engine_settings {

namespace {

namespace kit = oa::ui::kit;
namespace library = oa::ui::library;

/// The Compact metrics the text, the field, the buttons and the footer are
/// placed by. The width and the height limits come from the size class.
constexpr const kit::Metrics& metrics = kit::compact_metrics;

/// The columns a fact's label keeps before its value, when the label is short.
constexpr int32_t kLabelWidth = 76;
/// The rows between the name and the facts under it.
constexpr int32_t kNameGap = 4;

/// The window's words, in English.
constexpr std::string_view kAddTitle = "Add registry";
constexpr std::string_view kMirrorTitle = "Add mirror";
constexpr std::string_view kPlaceholder = "http://…";
constexpr std::string_view kKeyPlaceholder = "ed25519:…";
/// A public key is 52 characters. A longer paste stays too long to be a key
/// rather than being cut into one.
constexpr std::size_t kMaxSigningKeyBytes = 128;
constexpr std::string_view kCancelCaption = "CANCEL";
constexpr std::string_view kCheckCaption = "CHECK";
constexpr std::string_view kAddCaption = "ADD";
constexpr std::string_view kAddMirrorCaption = "ADD MIRROR";
constexpr std::string_view kUnsignedText = "Unsigned · accepted in Developer mode only";
constexpr std::string_view kWarningText =
    "Not reviewed by the OA team. Only add registries you trust. OA checks every package it "
    "downloads.";

/// One line of the window's text, placed.
struct Line {
    std::string text{};                       ///< the words, as shown
    kit::Rect rect{};                         ///< where they are drawn
    kit::FontRole font{kit::FontRole::small}; ///< which font
    kit::Colour colour{kit::colour::text};    ///< their colour
    bool heading{};                           ///< drawn in the heading role
};

/// Which label a line belongs to.
using Label = AddRegistryLabel;

/// A line and the label it belongs to.
struct LabelledLine {
    Line line{};              ///< the line
    Label label{Label::none}; ///< its label
};

/// One button, placed.
struct PlacedButton {
    kit::ControlId id{kit::no_control}; ///< its number
    std::string caption{};              ///< the caption
    std::string name{};                 ///< its automation name
    bool accent{};                      ///< drawn as the accent button
    kit::Rect rect{};                   ///< where it lies
};

/// The window, placed.
struct Placed {
    int32_t width{};                      ///< the window's width
    int32_t height{};                     ///< its height
    int32_t footer_rule{};                ///< the row of the line over the footer
    std::optional<kit::Rect> field{};     ///< the address field; none for a file
    std::optional<kit::Rect> key_field{}; ///< the signing-key field; an address add only
    std::vector<LabelledLine> lines;      ///< the text that fits, top to bottom
    std::vector<PlacedButton> buttons;    ///< the footer's buttons, left to right
};

/// Where the address field and, for an address add, the signing-key field sit.
struct FieldGeometry {
    int32_t row{};                      ///< the address field's top
    int32_t width{};                    ///< the window's width
    int32_t line_width{};               ///< the text width inside the padding
    std::optional<int32_t> key_label{}; ///< the "Signing key" line, for an address add
    std::optional<int32_t> key_field{}; ///< the signing-key field
};

/// Returns the address field's row, and the signing-key field's for an address add.
///
/// @param window the window
/// @return the rows, in points
FieldGeometry field_geometry(const AddRegistry& window) noexcept {
    FieldGeometry geometry;
    geometry.width = add_registry_width(window.size_class);
    geometry.line_width = geometry.width - 2 * metrics.padding;
    int32_t row = metrics.text_top;
    if (window.mode == AddRegistry::Mode::mirror)
        row += metrics.small_line + metrics.paragraph_gap;
    geometry.row = row;
    if (window.mode == AddRegistry::Mode::by_url) {
        row += metrics.field_height + metrics.paragraph_gap;
        geometry.key_label = row;
        row += metrics.small_line;
        geometry.key_field = row;
    }
    return geometry;
}

/// Returns a text's width in a font, or its estimated width without fonts.
///
/// @param fonts the fonts; null to estimate
/// @param role which font
/// @return the measure
kit::Measure measure_of(const kit::Fonts* fonts, kit::FontRole role) {
    if (fonts == nullptr)
        return kit::estimated_width;
    return [fonts, role](std::string_view text) { return kit::text_width(*fonts, role, text); };
}

/// How wide a fact's label column is. A signing-key label longer than
/// kLabelWidth widens the column, with a gap before the value, so the word
/// is not cut at Compact. The measure is the wider of the font and the estimate.
///
/// @param fonts the fonts; null estimates
/// @param label the looked-up label
/// @return the column, in points
int32_t fact_column(const kit::Fonts* fonts, std::string_view label) {
    const int32_t measured = measure_of(fonts, kit::FontRole::small)(label);
    const int32_t estimated = kit::estimated_width(label);
    return std::max(kLabelWidth, std::max(measured, estimated) + metrics.label_gap);
}

/// Fills a text's {name} places.
///
/// @param text the text, in English
/// @param place the place's name
/// @param value its value
/// @return the text
std::string filled(std::string_view text, std::string_view place, std::string_view value) {
    std::string out(text);
    const std::string marker = "{" + std::string(place) + "}";
    const std::size_t at = out.find(marker);
    if (at != std::string::npos)
        out.replace(at, marker.size(), value);
    return out;
}

/// Returns the window's title.
///
/// @param window the window
/// @return "Add registry", or "Add mirror" for a mirror
std::string title_of(const AddRegistry& window) {
    return std::string(window.mode == AddRegistry::Mode::mirror ? kMirrorTitle : kAddTitle);
}

/// Returns the Packages line's value: the count, and the kinds joined.
///
/// @param shown what the check found
/// @return such as "31 · maps" or "4 · mods, maps"
std::string packages_value(const ProbeView& shown) {
    std::string text = std::to_string(shown.packages);
    if (shown.kinds.empty())
        return text;
    text += " · ";
    for (std::size_t index = 0; index < shown.kinds.size(); ++index) {
        if (index > 0)
            text += ", ";
        text += shown.kinds[index];
    }
    return text;
}

/// Returns the fingerprint lines: at most shown_fingerprints, then "+N more".
///
/// @param fingerprints the keys' fingerprints
/// @return the lines
std::vector<std::string> fingerprint_lines(const std::vector<std::string>& fingerprints) {
    std::vector<std::string> lines;
    for (std::size_t index = 0; index < fingerprints.size() && index < shown_fingerprints; ++index)
        lines.push_back(fingerprints[index]);
    if (fingerprints.size() > shown_fingerprints)
        lines.push_back("+" + std::to_string(fingerprints.size() - shown_fingerprints) + " more");
    return lines;
}

/// Returns the row of the line over the footer.
///
/// @param height the window's height
/// @return the row
int32_t footer_rule_of(int32_t height) noexcept {
    return height - metrics.edge - metrics.footer_height - 1;
}

/// Returns the top row of the footer's buttons.
///
/// @param height the window's height
/// @return the row
int32_t button_top_of(int32_t height) noexcept {
    return footer_rule_of(height) + 1 + (metrics.footer_height - metrics.button_height) / 2;
}

/// Tells whether a rectangle sits above the footer with the gap the text keeps.
///
/// @param rect the rectangle
/// @param footer_rule the row of the line over the footer
/// @return true when it fits
bool fits_over(const kit::Rect& rect, int32_t footer_rule) noexcept {
    return rect.y + rect.height <= footer_rule - metrics.text_bottom_gap / 2;
}

/// Returns a text in the language shown.
///
/// @param english the text in English
/// @return the text
std::string worded(std::string_view english) {
    return std::string(oa::data::languages::interface_text(english));
}

/// Places the window: its text from the top, its height from that, and its
/// buttons right-aligned in the footer.
///
/// @param window the window
/// @param fonts the fonts; null estimates each character
/// @return the placed window
Placed place(const AddRegistry& window, const kit::Fonts* fonts) {
    const AddRegistryContent content = add_registry_content(window);
    Placed placed;
    placed.width = add_registry_width(window.size_class);
    const int32_t line_width = placed.width - 2 * metrics.padding;
    const kit::Measure small = measure_of(fonts, kit::FontRole::small);
    int32_t row = metrics.text_top;
    std::vector<LabelledLine> lines;

    const auto text_line = [&](std::string text, kit::Colour colour, Label label) {
        Line line;
        line.text = kit::fit(text, line_width, small);
        line.rect = {metrics.padding, row, line_width, metrics.small_line};
        line.colour = colour;
        lines.push_back({std::move(line), label});
        row += metrics.small_line;
    };
    const auto wrapped = [&](std::string_view text, kit::Colour colour, Label label) {
        for (const std::string& part : kit::wrap(text, line_width, small))
            text_line(part, colour, label);
    };
    const auto heading_line = [&](std::string text, Label label, int32_t inset) {
        const int32_t width = std::max(line_width - inset, int32_t{0});
        Line line;
        line.text = kit::fit(text, width, small);
        line.rect = {metrics.padding + inset, row, width, metrics.small_line};
        line.heading = true;
        lines.push_back({std::move(line), label});
        row += metrics.small_line;
    };
    // A fact: its label at the left, in the hint colour, and its value after
    // it. A label longer than kLabelWidth widens the column for every fact.
    int32_t column = kLabelWidth;
    for (const AddRegistryLine& line : content.lines)
        if (line.kind == AddRegistryLine::Kind::fact && !line.label.empty())
            column = std::max(column, fact_column(fonts, line.label));
    const auto fact = [&](std::string_view label_text, std::string value, Label label) {
        if (!label_text.empty()) {
            Line name;
            name.text = std::string(label_text);
            name.rect = {metrics.padding, row, column, metrics.small_line};
            name.colour = kit::colour::hint;
            lines.push_back({std::move(name), label});
        }
        const int32_t value_left = metrics.padding + column;
        const int32_t value_width = std::max(line_width - column, int32_t{0});
        Line value_line;
        value_line.text = kit::fit(value, value_width, small);
        value_line.rect = {value_left, row, value_width, metrics.small_line};
        lines.push_back({std::move(value_line), label});
        row += metrics.small_line;
    };

    const FieldGeometry fields = field_geometry(window);
    if (!content.heading.empty()) {
        heading_line(content.heading, Label::name, 0);
        row += metrics.paragraph_gap;
    }
    if (!content.file.empty()) {
        text_line(content.file, kit::colour::text, Label::none);
    } else if (content.address_field) {
        placed.field =
            kit::Rect{metrics.padding, fields.row, fields.line_width, metrics.field_height};
        row = fields.row + metrics.field_height;
    }
    if (content.key_field && fields.key_label && fields.key_field) {
        row = *fields.key_label;
        text_line(content.key_label, kit::colour::hint, Label::none);
        placed.key_field =
            kit::Rect{metrics.padding, *fields.key_field, fields.line_width, metrics.field_height};
        row = *fields.key_field + metrics.field_height;
    }
    row += metrics.paragraph_gap;

    for (const AddRegistryLine& line : content.lines) {
        if (line.gap == AddRegistryGap::name)
            row += kNameGap;
        else if (line.gap == AddRegistryGap::paragraph)
            row += metrics.paragraph_gap;
        const Label label = line.belongs;
        const kit::Colour colour = line.ink == AddRegistryInk::warning ? kit::colour::lock
                                   : line.ink == AddRegistryInk::hint  ? kit::colour::hint
                                                                       : kit::colour::text;
        switch (line.kind) {
        case AddRegistryLine::Kind::heading: {
            // The icon sits in the name's line, at that line's height. The
            // name stays put while the picture has not arrived.
            int32_t name_inset = 0;
            const oa::ui::frontend_renderer::RgbaPicture icon_picture{
                window.shown.icon_width, window.shown.icon_height, window.shown.icon_rgba
            };
            if (oa::ui::frontend_renderer::picture_drawable(icon_picture))
                name_inset = metrics.small_line + metrics.list_row_gap;
            heading_line(line.text, label, name_inset);
            break;
        }
        case AddRegistryLine::Kind::line:
            text_line(line.text, colour, label);
            break;
        case AddRegistryLine::Kind::paragraph:
            wrapped(line.text, colour, label);
            break;
        case AddRegistryLine::Kind::fact:
            fact(line.label, line.text, label);
            break;
        }
    }

    const kit::Metrics& sized = kit::metrics_of(window.size_class);
    placed.height = std::clamp(
        row + metrics.text_bottom_gap + 1 + metrics.footer_height + metrics.edge,
        sized.least_notice_height,
        sized.greatest_notice_height
    );
    placed.footer_rule = footer_rule_of(placed.height);
    for (LabelledLine& line : lines)
        if (fits_over(line.line.rect, placed.footer_rule))
            placed.lines.push_back(std::move(line));
    if (placed.field && !fits_over(*placed.field, placed.footer_rule))
        placed.field.reset();
    if (placed.key_field && !fits_over(*placed.key_field, placed.footer_rule))
        placed.key_field.reset();

    for (const AddRegistryButton& button : content.buttons)
        placed.buttons.push_back({button.id, button.caption, button.name, button.accent, {}});
    int32_t right = placed.width - metrics.padding;
    for (std::size_t index = placed.buttons.size(); index-- > 0;) {
        PlacedButton& button = placed.buttons[index];
        const int32_t width = std::max(
            metrics.button_width,
            kit::estimated_width(button.caption) + metrics.prompt_button_padding
        );
        button.rect = {right - width, button_top_of(placed.height), width, metrics.button_height};
        right -= width + metrics.button_gap;
    }
    return placed;
}

/// Returns what pressing a control asks.
///
/// @param window the window
/// @param id the control
/// @return its action, or none when it is not a button
AddRegistryAction action_for(const AddRegistry& window, kit::ControlId id) noexcept {
    if (id == add_registry_cancel_control)
        return AddRegistryAction::cancel;
    if (id == add_registry_check_control && add_registry_editable(window))
        return AddRegistryAction::check;
    if (id == add_registry_add_control && window.phase == AddRegistry::Phase::ready)
        return AddRegistryAction::add;
    return AddRegistryAction::none;
}

/// Returns what one pointer event asks.
///
/// @param[in,out] window the window; a release on the field focuses it
/// @param outcome what the kit's pointer did
/// @return the control's action, redraw, or none
AddRegistryAction from_pointer(AddRegistry& window, const kit::PointerOutcome& outcome) noexcept {
    if (outcome.result == kit::PointerResult::activated) {
        if (outcome.control == add_registry_url_control ||
            outcome.control == add_registry_key_field_control) {
            window.interaction.focused = outcome.control;
            window.interaction.focus_shown = true;
            return AddRegistryAction::start_typing;
        }
        return action_for(window, outcome.control);
    }
    if (outcome.result == kit::PointerResult::redraw)
        return AddRegistryAction::redraw;
    return AddRegistryAction::none;
}

/// Appends a label automation reads and a press does not reach, over the
/// lines that belong to it.
///
/// @param[in,out] list the display list
/// @param placed the placed window
/// @param label which label
/// @param id its number
/// @param name its automation name
void add_label(
    kit::DisplayList& list,
    const Placed& placed,
    Label label,
    kit::ControlId id,
    std::string_view name
) {
    std::optional<kit::Rect> area;
    std::string text;
    int32_t last_row = -1;
    for (const LabelledLine& line : placed.lines) {
        if (line.label != label || line.line.text.empty())
            continue;
        const kit::Rect& rect = line.line.rect;
        if (!area) {
            area = rect;
        } else {
            const int32_t left = std::min(area->x, rect.x);
            const int32_t top = std::min(area->y, rect.y);
            const int32_t right = std::max(area->x + area->width, rect.x + rect.width);
            const int32_t bottom = std::max(area->y + area->height, rect.y + rect.height);
            area = kit::Rect{left, top, right - left, bottom - top};
        }
        // A fact's label and value share a row; the next row starts a line.
        if (!text.empty())
            text += line.line.rect.y == last_row ? " " : "\n";
        text += line.line.text;
        last_row = line.line.rect.y;
    }
    if (!area)
        return;
    kit::Control control;
    control.id = id;
    control.rect = *area;
    control.enabled = false;
    control.name = std::string(name);
    control.kind = kit::ControlKind::area;
    control.focusable = false;
    control.text = std::move(text);
    list.controls.push_back(std::move(control));
}

/// Inserts text into one field, keeping only the first line and at most `most` bytes.
///
/// @param[in,out] text the field's text
/// @param[in,out] caret the caret, in bytes
/// @param utf8 the text being typed or pasted
/// @param most the most bytes the field holds
/// @return redraw when the field changed; none when it did not
AddRegistryAction
insert_into(std::string& text, std::size_t& caret, std::string_view utf8, std::size_t most) {
    const std::size_t line_break = utf8.find_first_of("\r\n");
    if (line_break != std::string_view::npos)
        utf8 = utf8.substr(0, line_break);
    const std::size_t room = most > text.size() ? most - text.size() : 0;
    if (utf8.size() > room) {
        std::size_t cut = room;
        while (cut > 0 && kit::continuation_byte(utf8[cut]))
            --cut;
        utf8 = utf8.substr(0, cut);
    }
    kit::TextField field{text, std::min(caret, text.size())};
    if (!kit::insert_text(field, utf8))
        return AddRegistryAction::none;
    text = std::move(field.text);
    caret = field.caret;
    return AddRegistryAction::redraw;
}

} // namespace

AddRegistryContent add_registry_content(const AddRegistry& window) {
    using Kind = AddRegistryLine::Kind;
    AddRegistryContent content;
    const bool mirror = window.mode == AddRegistry::Mode::mirror;
    content.title = worded(title_of(window));
    if (mirror)
        content.heading = filled(worded("Mirror of {name}"), "name", window.mirror_of);
    if (window.mode == AddRegistry::Mode::from_file) {
        content.file = filled(worded("From {file}"), "file", window.file_name);
    } else {
        content.address_field = true;
        content.address_placeholder = std::string(kPlaceholder);
    }
    if (window.mode == AddRegistry::Mode::by_url) {
        content.key_field = true;
        content.key_label = worded(library::words::registry_key_one);
        content.key_placeholder = std::string(kKeyPlaceholder);
    }

    std::vector<AddRegistryLine>& lines = content.lines;
    const auto add = [&lines](Kind kind,
                              std::string label,
                              std::string text,
                              AddRegistryInk ink,
                              AddRegistryLabel belongs,
                              AddRegistryGap gap) {
        lines.push_back({kind, std::move(label), std::move(text), ink, belongs, gap});
    };
    switch (window.phase) {
    case AddRegistry::Phase::editing:
        break;
    case AddRegistry::Phase::checking:
        add(Kind::line, {}, filled(worded("Checking {address}…"), "address", window.checking),
            AddRegistryInk::text, AddRegistryLabel::none, AddRegistryGap::none);
        break;
    case AddRegistry::Phase::ready: {
        const ProbeView& shown = window.shown;
        if (!mirror)
            add(Kind::heading, {}, shown.name, AddRegistryInk::text, AddRegistryLabel::name,
                AddRegistryGap::none);
        add(Kind::fact, worded("Address"), shown.address, AddRegistryInk::text,
            AddRegistryLabel::address, mirror ? AddRegistryGap::none : AddRegistryGap::name);
        if (mirror) {
            add(Kind::paragraph, {},
                filled(worded("Checked against the keys you trust for {name}"), "name",
                       window.mirror_of),
                AddRegistryInk::text, AddRegistryLabel::none, AddRegistryGap::paragraph);
            for (std::string& key : fingerprint_lines(shown.fingerprints))
                add(Kind::line, {}, std::move(key), AddRegistryInk::text, AddRegistryLabel::key,
                    AddRegistryGap::none);
            break;
        }
        add(Kind::fact, worded("Packages"), packages_value(shown), AddRegistryInk::text,
            AddRegistryLabel::none, AddRegistryGap::none);
        if (shown.is_signed) {
            // One key and several keys are different texts.
            const std::string key_label = worded(
                shown.fingerprints.size() > 1 ? library::words::registry_key_many
                                              : library::words::registry_key_one
            );
            const std::vector<std::string> keys = fingerprint_lines(shown.fingerprints);
            for (std::size_t index = 0; index < keys.size(); ++index)
                add(Kind::fact, index == 0 ? key_label : std::string(), keys[index],
                    AddRegistryInk::text, AddRegistryLabel::key, AddRegistryGap::none);
        } else {
            add(Kind::line, {}, worded(kUnsignedText), AddRegistryInk::warning,
                AddRegistryLabel::key, AddRegistryGap::none);
        }
        add(Kind::fact, worded("Downloads"), shown.downloads, AddRegistryInk::text,
            AddRegistryLabel::none, AddRegistryGap::none);
        add(Kind::paragraph, {}, worded(kWarningText), AddRegistryInk::warning,
            AddRegistryLabel::none, AddRegistryGap::paragraph);
        break;
    }
    case AddRegistry::Phase::refused:
        add(Kind::paragraph, {}, window.shown.refusal, AddRegistryInk::warning,
            AddRegistryLabel::refusal, AddRegistryGap::none);
        break;
    }

    content.buttons.push_back(
        {add_registry_cancel_control, worded(kCancelCaption), "add-registry.cancel", false}
    );
    if (add_registry_editable(window))
        content.buttons.push_back(
            {add_registry_check_control, worded(kCheckCaption), "add-registry.check", true}
        );
    else if (window.phase == AddRegistry::Phase::ready)
        content.buttons.push_back(
            {add_registry_add_control, worded(mirror ? kAddMirrorCaption : kAddCaption),
             "add-registry.add", true}
        );
    return content;
}

int32_t add_registry_width(kit::SizeClass size_class) noexcept {
    return kit::metrics_of(size_class).notice_width;
}

int32_t add_registry_height(const AddRegistry& window, const kit::Fonts* fonts) {
    return place(window, fonts).height;
}

bool add_registry_editable(const AddRegistry& window) noexcept {
    return window.mode != AddRegistry::Mode::from_file &&
           (window.phase == AddRegistry::Phase::editing ||
            window.phase == AddRegistry::Phase::refused);
}

std::optional<kit::Rect> add_registry_field(const AddRegistry& window) noexcept {
    if (!add_registry_editable(window) || !window.interaction.focus_shown)
        return std::nullopt;
    const FieldGeometry fields = field_geometry(window);
    if (window.interaction.focused == add_registry_key_field_control && fields.key_field)
        return kit::Rect{
            metrics.padding, *fields.key_field, fields.line_width, metrics.field_height
        };
    if (window.interaction.focused == add_registry_url_control &&
        window.mode != AddRegistry::Mode::from_file)
        return kit::Rect{metrics.padding, fields.row, fields.line_width, metrics.field_height};
    return std::nullopt;
}

kit::DisplayList add_registry_list(const AddRegistry& window, const kit::Fonts* fonts) {
    const Placed placed = place(window, fonts);
    const int32_t inner_width = placed.width - 2 * metrics.edge;
    kit::DisplayList list;

    kit::Item face;
    face.role = kit::Role::fill;
    face.rect = {0, 0, placed.width, placed.height};
    face.colour = kit::colour::panel;
    list.items.push_back(std::move(face));

    const std::string title = add_registry_content(window).title;
    kit::HeaderLook header{};
    header.width = placed.width;
    header.title = title;
    header.title_width = placed.width - metrics.padding -
                         (kit::notice_mark.x + kit::notice_mark.width + metrics.header_gap);
    header.tracking = metrics.heading_tracking;
    kit::Item header_item;
    header_item.role = kit::Role::header;
    header_item.rect = {metrics.edge, metrics.edge, inner_width, metrics.header_height};
    header_item.text = title;
    header_item.look = header;
    list.items.push_back(std::move(header_item));

    const kit::Interaction& interaction = window.interaction;
    const bool editable = add_registry_editable(window);
    const auto text_field = [&](kit::ControlId id,
                                const kit::Rect& rect,
                                std::string_view name,
                                std::string_view placeholder,
                                const std::string& value,
                                std::size_t caret) {
        const bool focused = editable && interaction.focus_shown && interaction.focused == id;
        kit::SearchLook look;
        look.placeholder = std::string(placeholder);
        look.field.text = value;
        look.field.caret = std::min(caret, value.size());
        look.focused = focused;
        look.hovered = editable && interaction.hovered == id;
        look.magnifier = false;
        kit::Item item;
        item.role = kit::Role::field;
        item.rect = rect;
        item.control = id;
        item.text = value;
        item.state.hovered = look.hovered;
        item.state.focused = focused;
        item.state.disabled = !editable;
        item.look = std::move(look);
        list.items.push_back(std::move(item));
        kit::Control control;
        control.id = id;
        control.rect = rect;
        control.enabled = editable;
        control.focusable = editable;
        control.steps = true;
        control.name = std::string(name);
        control.kind = kit::ControlKind::text_field;
        control.text = value;
        list.controls.push_back(std::move(control));
        if (editable)
            list.tab_order.push_back(id);
    };
    if (placed.field)
        text_field(
            add_registry_url_control,
            *placed.field,
            "add-registry.url",
            kPlaceholder,
            window.url,
            window.caret
        );
    if (placed.key_field)
        text_field(
            add_registry_key_field_control,
            *placed.key_field,
            "add-registry.key-field",
            kKeyPlaceholder,
            window.signing_key,
            window.key_caret
        );

    for (const LabelledLine& labelled : placed.lines) {
        const Line& line = labelled.line;
        kit::Item item;
        item.rect = line.rect;
        item.text = line.text;
        item.font = line.font;
        if (line.heading) {
            item.role = kit::Role::heading;
            item.look = kit::HeadingLook{line.text};
        } else {
            item.role = kit::Role::text;
            item.align = kit::Align::left;
            item.colour = line.colour;
        }
        list.items.push_back(std::move(item));
        if (labelled.label == Label::name && window.phase == AddRegistry::Phase::ready &&
            window.mode != AddRegistry::Mode::mirror) {
            const oa::ui::frontend_renderer::RgbaPicture icon_picture{
                window.shown.icon_width, window.shown.icon_height, window.shown.icon_rgba
            };
            if (oa::ui::frontend_renderer::picture_drawable(icon_picture)) {
                kit::Item picture;
                picture.role = kit::Role::picture;
                picture.rect = {
                    metrics.padding, line.rect.y, metrics.small_line, metrics.small_line
                };
                picture.look = kit::PictureLook{icon_picture};
                list.items.push_back(std::move(picture));
            }
        }
    }

    kit::Item footer;
    footer.role = kit::Role::footer_band;
    footer.rect = {metrics.edge, placed.footer_rule, inner_width, 1 + metrics.footer_height};
    footer.look = kit::FooterBandLook{placed.width, placed.footer_rule};
    list.items.push_back(std::move(footer));

    for (const PlacedButton& button : placed.buttons) {
        const bool marked = interaction.focus_shown && interaction.focused == button.id;
        const bool hovered = interaction.hovered == button.id;
        const bool held = interaction.pressed == button.id && hovered;
        kit::ButtonLook look{};
        look.caption = button.caption;
        look.style = button.accent ? kit::ButtonStyle::accent : kit::ButtonStyle::plain;
        look.hovered = hovered;
        look.held = held;
        look.enabled = true;
        kit::Item item;
        item.role = kit::Role::button;
        item.rect = button.rect;
        item.control = button.id;
        item.text = button.caption;
        item.state.hovered = hovered;
        item.state.pressed = held;
        item.state.focused = marked;
        item.look = std::move(look);
        list.items.push_back(std::move(item));
        if (marked)
            kit::add_focus_ring(list, button.rect, true);
        kit::Control control;
        control.id = button.id;
        control.rect = button.rect;
        control.name = button.name;
        control.kind = kit::ControlKind::button;
        control.text = button.caption;
        list.controls.push_back(std::move(control));
        list.tab_order.push_back(button.id);
    }

    add_label(list, placed, Label::name, add_registry_name_control, "add-registry.name");
    add_label(list, placed, Label::address, add_registry_address_control, "add-registry.address");
    add_label(list, placed, Label::key, add_registry_key_control, "add-registry.key");
    add_label(list, placed, Label::refusal, add_registry_refusal_control, "add-registry.refusal");

    kit::Item edge;
    edge.role = kit::Role::bevel;
    edge.rect = {0, 0, placed.width, placed.height};
    edge.colour = kit::colour::edge_light;
    list.items.push_back(std::move(edge));
    return list;
}

void add_registry_settle(AddRegistry& window) noexcept {
    window.interaction.focused =
        add_registry_editable(window) ? add_registry_url_control : add_registry_cancel_control;
    window.interaction.focus_shown = true;
    window.interaction.hovered = kit::no_control;
    window.interaction.pressed = kit::no_control;
    window.interaction.finger_shift = {};
}

AddRegistryAction add_registry_text(AddRegistry& window, std::string_view utf8) {
    if (!add_registry_editable(window))
        return AddRegistryAction::none;
    const bool signing = window.interaction.focused == add_registry_key_field_control;
    const AddRegistryAction action =
        signing ? insert_into(window.signing_key, window.key_caret, utf8, kMaxSigningKeyBytes)
                : insert_into(window.url, window.caret, utf8, oa::formats::url::max_url_bytes);
    if (action == AddRegistryAction::redraw && window.phase == AddRegistry::Phase::refused) {
        window.phase = AddRegistry::Phase::editing;
        window.shown = {};
    }
    return action;
}

AddRegistryAction add_registry_key(AddRegistry& window, kit::Key pressed, const kit::Fonts* fonts) {
    return add_registry_key(window, pressed, add_registry_list(window, fonts));
}

AddRegistryAction
add_registry_key(AddRegistry& window, kit::Key pressed, const kit::DisplayList& list) {
    const kit::KeyOutcome outcome = kit::key(window.interaction, list, pressed);
    switch (outcome.result) {
    case kit::KeyResult::none:
    case kit::KeyResult::scroll:
        return AddRegistryAction::none;
    case kit::KeyResult::redraw:
        return AddRegistryAction::redraw;
    case kit::KeyResult::cancel:
        return AddRegistryAction::cancel;
    case kit::KeyResult::accept:
        return add_registry_editable(window) ? AddRegistryAction::check : AddRegistryAction::none;
    case kit::KeyResult::to_control:
        break;
    }
    const bool url_field = outcome.control == add_registry_url_control;
    const bool key_field = outcome.control == add_registry_key_field_control;
    if (!url_field && !key_field)
        return action_for(window, outcome.control);
    if (pressed == kit::Key::space)
        return AddRegistryAction::start_typing;
    std::string& current = key_field ? window.signing_key : window.url;
    std::size_t& caret = key_field ? window.key_caret : window.caret;
    kit::TextField field{current, std::min(caret, current.size())};
    if (!kit::edit_text(field, pressed))
        return AddRegistryAction::none;
    const bool changed = field.text != current;
    current = std::move(field.text);
    caret = field.caret;
    if (changed && window.phase == AddRegistry::Phase::refused) {
        window.phase = AddRegistry::Phase::editing;
        window.shown = {};
    }
    return AddRegistryAction::redraw;
}

AddRegistryAction
add_registry_pointer_move(AddRegistry& window, kit::Point at, const kit::Fonts* fonts) {
    const kit::DisplayList list = add_registry_list(window, fonts);
    return from_pointer(window, kit::pointer_move(window.interaction, list, at));
}

AddRegistryAction
add_registry_pointer_down(AddRegistry& window, kit::Point at, const kit::Fonts* fonts) {
    const kit::DisplayList list = add_registry_list(window, fonts);
    return from_pointer(window, kit::pointer_down(window.interaction, list, at));
}

AddRegistryAction add_registry_finger_down(
    AddRegistry& window, kit::Point at, int32_t reach, const kit::Fonts* fonts
) {
    const kit::DisplayList list = add_registry_list(window, fonts);
    return from_pointer(window, kit::finger_down(window.interaction, list, at, reach));
}

AddRegistryAction
add_registry_pointer_up(AddRegistry& window, kit::Point at, const kit::Fonts* fonts) {
    const kit::DisplayList list = add_registry_list(window, fonts);
    return from_pointer(window, kit::pointer_up(window.interaction, list, at));
}

AddRegistryAction add_registry_pointer(AddRegistry& window, const kit::PointerOutcome& outcome) {
    return from_pointer(window, outcome);
}

void draw_add_registry(const kit::Canvas& canvas, const AddRegistry& window) {
    kit::paint(canvas, add_registry_list(window, canvas.fonts));
}

} // namespace oa::ui::engine_settings
