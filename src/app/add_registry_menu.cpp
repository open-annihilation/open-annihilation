// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Add registry window in the game's look: where its parts lie on the
// screen, and how they are drawn with the game's art.

#include "add_registry_menu.hpp"

#include "menu_paint.hpp"

#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/text.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <string>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace kit = oa::ui::kit;
namespace settings = oa::ui::engine_settings;
using namespace menu_paint;

// The window's measures, in rows of a window 1080 rows high, as the
// Library's.

/// The narrowest width, in the same rows, the measures keep their size in.
constexpr double design_least_columns = 1480.0;
constexpr double panel_columns = 880.0;     ///< the window's width
constexpr double rim_columns = 11.0;        ///< the frame's width inside the panel
constexpr double label_left_columns = 10.0; ///< from the frame to the title
constexpr double label_top_rows = 5.0;      ///< from the panel's top to the title
constexpr double label_rows = 34.0;         ///< the title's height
constexpr double band_rows = 46.0;          ///< the title's band
constexpr double inset_columns = 18.0;      ///< inside the panel, round its content
constexpr double field_rows = 44.0;         ///< a field's height
constexpr double gap_rows = 10.0;           ///< between two parts
constexpr double pad_columns = 12.0;        ///< inside a field, and after a fact's label
constexpr double pill_rows = 36.0;          ///< the line the game's font is sized to, as the Library's
constexpr double button_rows = 58.0;        ///< a button's height
constexpr double button_gap_columns = 16.0; ///< between two buttons
/// The most lines a paragraph takes.
constexpr std::size_t most_paragraph_lines = 6;
/// How strongly the screen is shaded under the window, in 256ths.
constexpr uint32_t shade_opacity = 128;
/// The shade's colour: the Library's wells'.
constexpr kit::Colour shade_colour{12, 15, 18};

/// Returns the tint a line's colour is drawn in.
///
/// @param ink the line's colour
/// @return the tint
Ink ink_of(settings::AddRegistryInk ink) noexcept {
    switch (ink) {
    case settings::AddRegistryInk::hint:
        return Ink::faded;
    case settings::AddRegistryInk::warning:
        return Ink::lit;
    case settings::AddRegistryInk::text:
        break;
    }
    return Ink::plain;
}

} // namespace

/// One line of the window's text, placed.
struct PlacedLine {
    std::string text{};                                        ///< its words, fitted
    kit::Rect rect{};                                          ///< where they are drawn
    Ink ink{Ink::plain};                                       ///< their tint
    settings::AddRegistryLabel belongs{settings::AddRegistryLabel::none}; ///< its label
};

/// A field, placed.
struct PlacedField {
    kit::ControlId id{kit::no_control}; ///< its number
    std::string name{};                 ///< its automation name
    std::string placeholder{};          ///< what it shows empty
    kit::Rect rect{};                   ///< where it lies
};

/// The window, placed.
struct AddRegistryGameLook::Placed {
    kit::Rect panel{};                              ///< the window
    std::string title{};                            ///< its title
    std::vector<PlacedLine> lines{};                ///< its text, top to bottom
    std::vector<PlacedField> fields{};              ///< its fields, top to bottom
    std::optional<kit::Rect> icon{};                ///< the registry's icon, by its name
    std::vector<settings::AddRegistryButton> buttons{}; ///< its buttons, left to right
    std::vector<kit::Rect> button_rects{};          ///< where they lie
};

AddRegistryGameLook::AddRegistryGameLook(GameMenuArt art) : painter_(std::move(art)) {}

bool AddRegistryGameLook::lay_out(kit::Point size) {
    if (size.x == size_.x && size.y == size_.y)
        return false;
    size_ = size;
    const kit::Rect stage = game_menu_stage(size_.x, size_.y);
    unit_ = std::max(
        std::min(stage.height / design_rows, stage.width / design_least_columns), 0.01
    );
    painter_.lay_out(size_.x, size_.y, unit_);
    return true;
}

int32_t AddRegistryGameLook::square() const {
    return painter_.text_square(nearest(pill_rows * unit_));
}

int32_t AddRegistryGameLook::line() const {
    const int32_t each = square();
    return static_cast<int32_t>(painter_.caption("A", Ink::plain).height) * each + each;
}

AddRegistryGameLook::Placed AddRegistryGameLook::place(const settings::AddRegistry& window) const {
    using Kind = settings::AddRegistryLine::Kind;
    const settings::AddRegistryContent content = settings::add_registry_content(window);
    const auto across = [this](double design) { return nearest(design * unit_); };
    const int32_t inset = across(inset_columns);
    const int32_t gap = std::max(across(gap_rows), 2);
    const int32_t pad = std::max(across(pad_columns), 3);
    const int32_t text_line = line();
    const int32_t label_top = across(label_top_rows);
    const int32_t band = std::max(across(band_rows), label_top + std::max(across(label_rows), text_line) + 2);
    const int32_t field_height = std::max(across(field_rows), text_line + 4);
    const int32_t button_height = across(button_rows);
    const kit::Measure measure = painter_.measure(square());

    Placed placed;
    placed.title = content.title;
    const int32_t width = std::min(across(panel_columns), std::max(size_.x - 2 * inset, 1));
    const int32_t text_width = std::max(width - 2 * inset, 1);
    // Placed from the panel's top left; moved to the middle at the end.
    int32_t y = band;
    const auto add_line = [&](std::string text, int32_t left, Ink ink, settings::AddRegistryLabel belongs) {
        const int32_t room = std::max(text_width - left, 1);
        placed.lines.push_back({kit::fit(text, room, measure), {inset + left, y, room, text_line}, ink, belongs});
    };
    const auto add_field = [&](kit::ControlId id, std::string name, std::string placeholder) {
        placed.fields.push_back({id, std::move(name), std::move(placeholder), {inset, y, text_width, field_height}});
        y += field_height;
    };

    if (!content.heading.empty()) {
        add_line(content.heading, 0, Ink::plain, settings::AddRegistryLabel::name);
        y += text_line + gap;
    }
    if (!content.file.empty()) {
        add_line(content.file, 0, Ink::plain, settings::AddRegistryLabel::none);
        y += text_line;
    } else if (content.address_field) {
        add_field(settings::add_registry_url_control, "add-registry.url", content.address_placeholder);
    }
    if (content.key_field) {
        y += gap;
        add_line(content.key_label, 0, Ink::faded, settings::AddRegistryLabel::none);
        y += text_line;
        add_field(settings::add_registry_key_field_control, "add-registry.key-field", content.key_placeholder);
    }

    // A fact's value stands after the widest of the facts' labels.
    int32_t column = 0;
    for (const settings::AddRegistryLine& shown : content.lines)
        if (shown.kind == Kind::fact && !shown.label.empty())
            column = std::max(column, measure(shown.label) + pad);
    column = std::min(column, text_width / 2);
    if (!content.lines.empty())
        y += gap;
    for (const settings::AddRegistryLine& shown : content.lines) {
        if (shown.gap == settings::AddRegistryGap::name)
            y += gap / 2;
        else if (shown.gap == settings::AddRegistryGap::paragraph)
            y += gap;
        const Ink ink = ink_of(shown.ink);
        switch (shown.kind) {
        case Kind::heading: {
            // The icon stands before the name, as high as its line, once it
            // has arrived.
            int32_t left = 0;
            const oa::ui::frontend_renderer::RgbaPicture picture{
                window.shown.icon_width, window.shown.icon_height, window.shown.icon_rgba
            };
            if (oa::ui::frontend_renderer::picture_drawable(picture)) {
                placed.icon = kit::Rect{inset, y, text_line, text_line};
                left = text_line + pad;
            }
            add_line(shown.text, left, ink, shown.belongs);
            y += text_line;
            break;
        }
        case Kind::line:
            add_line(shown.text, 0, ink, shown.belongs);
            y += text_line;
            break;
        case Kind::paragraph: {
            kit::WrapRules rules;
            rules.most_lines = most_paragraph_lines;
            for (const std::string& part : kit::wrap(shown.text, text_width, measure, rules)) {
                add_line(part, 0, ink, shown.belongs);
                y += text_line;
            }
            break;
        }
        case Kind::fact:
            if (!shown.label.empty())
                placed.lines.push_back(
                    {kit::fit(shown.label, column, measure), {inset, y, column, text_line}, Ink::faded, shown.belongs}
                );
            add_line(shown.text, column, ink, shown.belongs);
            y += text_line;
            break;
        }
    }

    // The buttons at the foot, right-aligned, as wide as their plates are
    // for their height.
    y += 2 * gap;
    placed.buttons = content.buttons;
    const int32_t button_width = painter_.big_button_width(button_height);
    const int32_t button_gap = across(button_gap_columns);
    const auto count = static_cast<int32_t>(placed.buttons.size());
    int32_t x = width - inset - (count * button_width + std::max(count - 1, 0) * button_gap);
    for (std::size_t button = 0; button < placed.buttons.size(); ++button) {
        placed.button_rects.push_back({x, y, button_width, button_height});
        x += button_width + button_gap;
    }
    const int32_t height = y + button_height + inset;

    // The panel in the middle of the screen.
    placed.panel = {(size_.x - width) / 2, (size_.y - height) / 2, width, height};
    const kit::Point by{placed.panel.x, placed.panel.y};
    const auto moved = [&by](kit::Rect& rect) {
        rect.x += by.x;
        rect.y += by.y;
    };
    for (PlacedLine& shown : placed.lines)
        moved(shown.rect);
    for (PlacedField& field : placed.fields)
        moved(field.rect);
    if (placed.icon)
        moved(*placed.icon);
    for (kit::Rect& rect : placed.button_rects)
        moved(rect);
    return placed;
}

kit::DisplayList AddRegistryGameLook::list(const settings::AddRegistry& window) const {
    const Placed placed = place(window);
    const bool editable = settings::add_registry_editable(window);
    kit::DisplayList list;
    for (const PlacedField& field : placed.fields) {
        kit::Control control;
        control.id = field.id;
        control.rect = field.rect;
        control.enabled = editable;
        control.focusable = editable;
        control.steps = true;
        control.name = field.name;
        control.kind = kit::ControlKind::text_field;
        control.text = field.id == settings::add_registry_key_field_control ? window.signing_key : window.url;
        list.controls.push_back(std::move(control));
        if (editable)
            list.tab_order.push_back(field.id);
    }
    for (std::size_t button = 0; button < placed.buttons.size(); ++button) {
        kit::Control control;
        control.id = placed.buttons[button].id;
        control.rect = placed.button_rects[button];
        control.name = placed.buttons[button].name;
        control.kind = kit::ControlKind::button;
        control.text = placed.buttons[button].caption;
        list.controls.push_back(std::move(control));
        list.tab_order.push_back(placed.buttons[button].id);
    }
    // The labels automation reads: each label's lines as one, a fact's
    // label and value on one row joined by a space.
    const auto label = [&](settings::AddRegistryLabel which, kit::ControlId id, std::string_view name) {
        std::optional<kit::Rect> area;
        std::string text;
        int32_t last_row = -1;
        for (const PlacedLine& shown : placed.lines) {
            if (shown.belongs != which || shown.text.empty())
                continue;
            const kit::Rect& rect = shown.rect;
            if (!area) {
                area = rect;
            } else {
                const int32_t left = std::min(area->x, rect.x);
                const int32_t top = std::min(area->y, rect.y);
                const int32_t right = std::max(area->x + area->width, rect.x + rect.width);
                const int32_t bottom = std::max(area->y + area->height, rect.y + rect.height);
                area = kit::Rect{left, top, right - left, bottom - top};
            }
            if (!text.empty())
                text += rect.y == last_row ? " " : "\n";
            text += shown.text;
            last_row = rect.y;
        }
        if (!area)
            return;
        kit::Control control;
        control.id = id;
        control.rect = *area;
        control.enabled = false;
        control.focusable = false;
        control.name = std::string(name);
        control.kind = kit::ControlKind::area;
        control.text = std::move(text);
        list.controls.push_back(std::move(control));
    };
    label(settings::AddRegistryLabel::name, settings::add_registry_name_control, "add-registry.name");
    label(settings::AddRegistryLabel::address, settings::add_registry_address_control, "add-registry.address");
    label(settings::AddRegistryLabel::key, settings::add_registry_key_control, "add-registry.key");
    label(settings::AddRegistryLabel::refusal, settings::add_registry_refusal_control, "add-registry.refusal");
    return list;
}

std::optional<kit::Rect> AddRegistryGameLook::field(const settings::AddRegistry& window) const {
    if (!settings::add_registry_editable(window) || !window.interaction.focus_shown)
        return std::nullopt;
    for (const PlacedField& field : place(window).fields)
        if (field.id == window.interaction.focused)
            return field.rect;
    return std::nullopt;
}

void AddRegistryGameLook::draw(const kit::Canvas& canvas, const settings::AddRegistry& window) const {
    const Placed placed = place(window);
    const kit::Interaction& interaction = window.interaction;
    const bool editable = settings::add_registry_editable(window);
    const int32_t each = square();
    const int32_t ring = std::max(each, 1);
    const int32_t pad = std::max(nearest(pad_columns * unit_), 3);
    const kit::Measure measure = painter_.measure(each);

    kit::blend_rect(canvas, {0, 0, size_.x, size_.y}, shade_colour, shade_opacity);
    painter_.draw_frame(canvas, placed.panel);
    // The title where the Library's panels have their labels.
    const int32_t rim = nearest(rim_columns * unit_);
    const int32_t label_left = placed.panel.x + rim + nearest(label_left_columns * unit_);
    painter_.draw_text(
        canvas,
        {label_left, placed.panel.y + nearest(label_top_rows * unit_),
         placed.panel.x + placed.panel.width - rim - nearest(inset_columns * unit_) - label_left,
         std::max(nearest(label_rows * unit_), line())},
        placed.title, kit::Align::left, Ink::plain
    );

    // The fields: the game's list box, the words in its font, the end of
    // what was typed showing; an amber ring and caret on the one typed into.
    for (const PlacedField& field : placed.fields) {
        const bool focused = editable && interaction.focus_shown && interaction.focused == field.id;
        painter_.draw_box(canvas, field.rect);
        if (focused)
            draw_ring(canvas, field.rect, ring, game_amber);
        const std::string& value =
            field.id == settings::add_registry_key_field_control ? window.signing_key : window.url;
        const kit::Rect words{field.rect.x + pad, field.rect.y, field.rect.width - 2 * pad, field.rect.height};
        const bool hint = value.empty();
        std::string shown = hint ? field.placeholder : value;
        const int32_t caret_room = focused ? 2 * ring : 0;
        if (!hint && measure(shown) > words.width - caret_room) {
            while (!shown.empty() && measure("…" + shown) > words.width - caret_room)
                shown.erase(0, kit::character_bytes(shown));
            shown = "…" + shown;
        }
        painter_.draw_text_at(
            canvas, words, hint ? kit::fit(shown, words.width, measure) : shown, kit::Align::left,
            hint || !editable ? Ink::faded : Ink::plain, each
        );
        if (focused) {
            const int32_t caret_height = line();
            kit::fill_rect(
                canvas,
                {words.x + (hint ? 0 : measure(shown)) + 1, field.rect.y + (field.rect.height - caret_height) / 2,
                 std::max(each, 2), caret_height},
                game_amber
            );
        }
    }

    for (const PlacedLine& shown : placed.lines)
        painter_.draw_text_at(canvas, shown.rect, shown.text, kit::Align::left, shown.ink, each);
    if (placed.icon) {
        const oa::ui::frontend_renderer::RgbaPicture picture{
            window.shown.icon_width, window.shown.icon_height, window.shown.icon_rgba
        };
        kit::draw_picture(canvas, *placed.icon, picture);
    }

    // The buttons, the one Enter presses ringed in amber.
    for (std::size_t button = 0; button < placed.buttons.size(); ++button) {
        const kit::ControlId id = placed.buttons[button].id;
        const kit::Rect& rect = placed.button_rects[button];
        const bool lit = interaction.hovered == id;
        painter_.draw_big_button(
            canvas, rect, placed.buttons[button].caption, true, lit, lit && interaction.pressed == id
        );
        if (interaction.focus_shown && interaction.focused == id)
            draw_ring(canvas, rect, ring, game_amber);
    }
}

} // namespace oa::app
