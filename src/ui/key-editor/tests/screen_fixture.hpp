// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the key editor's screen tests share: the made-up Ridge layout, the
// synthetic font those layouts are measured in, and the checks a layout's
// parts are held to.
#pragma once

#include "oa/formats/fnt.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_editor/model.hpp"
#include "oa/ui/key_editor/screen.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace key_editor_fixture {

namespace kit = oa::ui::kit;
namespace editor = oa::ui::key_editor;
namespace keys = oa::ui::key_bindings;

/// The synthetic font of the kit's key tests, for both roles. A glyph for
/// byte b is 2 + b % 4 columns by 8 rows, set where (x + y + byte) % 3 is not 0.
///
/// @return the fonts
inline kit::Fonts block_fonts() {
    oa::ui::frontend_renderer::TextFont font;
    font.font.nominal_height = 8;
    constexpr uint8_t ink_index = 1;
    for (int byte = 0; byte < 256; ++byte) {
        const auto width = static_cast<uint16_t>(2 + byte % 4);
        constexpr uint16_t height = 8;
        const auto count = static_cast<std::size_t>(width) * height;
        std::vector<uint8_t> pixels(count, ink_index);
        std::vector<uint8_t> coverage(count, 0);
        for (uint16_t y = 0; y < height; ++y) {
            for (uint16_t x = 0; x < width; ++x) {
                if ((static_cast<int>(x) + y + byte) % 3 != 0)
                    coverage[static_cast<std::size_t>(y) * width + x] = 1;
            }
        }
        font.font.glyphs[static_cast<std::size_t>(byte)] =
            oa::formats::fnt::Glyph{width, height, 0, 0, std::move(pixels), std::move(coverage)};
    }
    font.ink[ink_index] = static_cast<uint16_t>(oa::ui::frontend_renderer::blend_opaque);
    return {font, font, {}, {}};
}

/// The fonts every layout here is measured in.
///
/// @return the fonts
inline const kit::Fonts& fonts() {
    static const kit::Fonts loaded = block_fonts();
    return loaded;
}

/// Parses a chord the fixture expects to accept. An empty chord means the
/// text was refused.
///
/// @param text the chord, as a layout writes it
/// @return the chord
inline keys::Chord parsed_chord(std::string_view text) {
    keys::ChordError error;
    const std::optional<keys::Chord> chord = keys::parse_chord(text, error);
    if (!chord)
        return {};
    return *chord;
}

/// One binding of a single chord.
///
/// @param action the action id
/// @param text the chord
/// @return the binding
inline keys::LayoutBinding binding_of(std::string action, std::string_view text) {
    keys::LayoutBinding binding;
    binding.action = std::move(action);
    binding.chords.push_back(parsed_chord(text));
    return binding;
}

/// The frame of a viewport at a scale. 640x480 at 1x is Compact, 1280x720 at
/// 1x is Large, and 1920x1080 at 2x is 960x540 points, which is Regular.
///
/// @param width the canvas's width, in pixels
/// @param height the canvas's height, in pixels
/// @param scale_percent the scale, as a percentage
/// @return the frame
inline kit::Frame frame_for(int32_t width, int32_t height, int32_t scale_percent) {
    kit::Viewport viewport;
    viewport.width = width;
    viewport.height = height;
    viewport.scale_percent = scale_percent;
    return kit::frame_of(viewport);
}

/// The fixture. OA default under a made-up layout named Ridge grid, with the
/// player's own Ctrl+B on select.all. The mod is Ridge: one GUI key, the
/// category letters E and W, and the Category words KBOT, CTRL_W and
/// RIDGE_SCOUT. Ctrl+Z is already select.same-type, so Z would be dropped;
/// play does not bind Ctrl+E or Ctrl+W.
///
/// @return the inputs
inline editor::EditorInputs editor_fixture() {
    editor::EditorInputs inputs;
    inputs.base = keys::Base::oa_default;
    inputs.has_chosen = true;
    inputs.chosen.id = "ridge-grid";
    inputs.chosen.name = "Ridge grid";
    inputs.chosen.author = "Ridge";
    inputs.chosen.version = "1.0";
    inputs.chosen.layout.id = "ridge-grid";
    inputs.chosen.layout.base = keys::Base::oa_default;
    inputs.chosen.layout.bindings.push_back(binding_of("order.attack", "W"));

    inputs.own.base = keys::Base::oa_default;
    inputs.own.bindings.push_back(binding_of("select.all", "Ctrl+B"));

    keys::GuiKey ridge;
    ridge.action = "gui.ridge";
    ridge.gadget = "RIDGE";
    ridge.letter = U'R';
    ridge.help = "Ridge menu";
    inputs.mod.keys.push_back(std::move(ridge));
    inputs.mod.categories = "EW";
    inputs.mod.tags = {"KBOT", "CTRL_W", "RIDGE_SCOUT"};

    inputs.hacks_on = {"ui.megamap"};
    inputs.keyboard = keys::KeyboardMap::us();
    inputs.platform = {};
    inputs.scheme = keys::PointerScheme::right_click;
    inputs.number_keys_select_groups = false;
    inputs.developer_mode = false;
    inputs.hold_to_confirm_setting = true;
    inputs.mod_name = "Ridge";
    inputs.text = {};
    inputs.installed.push_back(inputs.chosen);
    return inputs;
}

/// Returns the part of an item that is drawn: its rectangle, within its clip
/// when it has one, and a focus ring's outline round its control.
///
/// @param item the item
/// @return the rectangle
inline kit::Rect drawn_area(const kit::Item& item) {
    kit::Rect area = item.rect;
    if (item.role == kit::Role::focus_ring) {
        const auto* look = std::get_if<kit::FocusRingLook>(&item.look);
        if (look != nullptr && look->around)
            area = kit::grown(area, kit::compact_metrics.focus_inset);
    }
    if (item.clip.width > 0 && item.clip.height > 0)
        area = kit::intersect(area, item.clip);
    return area;
}

/// Returns the part of a control a press reaches.
///
/// @param control the control
/// @return its rectangle, within its clip when it has one
inline kit::Rect press_area(const kit::Control& control) {
    if (control.clip.width > 0 && control.clip.height > 0)
        return kit::intersect(control.rect, control.clip);
    return control.rect;
}

/// Tells whether a rectangle holds any point.
///
/// @param rect the rectangle
/// @return true when it is wide and high
inline bool has_room(const kit::Rect& rect) {
    return rect.width > 0 && rect.height > 0;
}

/// Returns the first item a prompt draws: the one after the veil that dims
/// the editor under it.
///
/// @param list the display list
/// @return its place; the list's size when no prompt is drawn
inline std::size_t prompt_first(const kit::DisplayList& list) {
    for (std::size_t index = 0; index < list.items.size(); ++index) {
        const kit::Item& item = list.items[index];
        if (item.role == kit::Role::blend && item.colour == kit::colour::backdrop)
            return index + 1;
    }
    return list.items.size();
}

/// Lists what a prompt draws that meets another of its parts, and the words
/// it cut short. A text meets no other text, and no button, field, pill or
/// chord chip; nor do those meet each other. A text that holds "..." was
/// cut, unless it lies in one of `exempt`'s rectangles: a preview that says
/// it holds more lines, or a path cut in its middle.
///
/// @param list the display list
/// @param exempt where a text may hold "..."
/// @return a line for each meeting and each cut; none when there are none
inline std::vector<std::string>
prompt_problems(const kit::DisplayList& list, const std::vector<kit::Rect>& exempt) {
    std::vector<std::string> problems;
    std::vector<const kit::Item*> parts;
    const auto said = [](const kit::Item& item) {
        if (const auto* button = std::get_if<kit::ButtonLook>(&item.look))
            return button->caption;
        return item.text;
    };
    for (std::size_t index = prompt_first(list); index < list.items.size(); ++index) {
        const kit::Item& item = list.items[index];
        const bool part = item.role == kit::Role::text || item.role == kit::Role::button ||
                          item.role == kit::Role::field || item.role == kit::Role::pill ||
                          item.role == kit::Role::chord_chip;
        if (!part || !has_room(drawn_area(item)))
            continue;
        parts.push_back(&item);
        if (said(item).find("...") == std::string::npos)
            continue;
        bool allowed = false;
        for (const kit::Rect& rect : exempt)
            allowed = allowed || kit::wholly_in(drawn_area(item), rect);
        if (!allowed)
            problems.push_back("\"" + said(item) + "\" is cut");
    }
    for (std::size_t first = 0; first < parts.size(); ++first) {
        for (std::size_t second = first + 1; second < parts.size(); ++second) {
            const kit::Rect met =
                kit::intersect(drawn_area(*parts[first]), drawn_area(*parts[second]));
            if (has_room(met))
                problems.push_back(
                    "\"" + said(*parts[first]) + "\" meets \"" + said(*parts[second]) + "\""
                );
        }
    }
    return problems;
}

/// Lists the parts of a layout that lie outside its window.
///
/// @param list the display list
/// @param window the window's size
/// @return a line for each part outside; none when all lie inside
inline std::vector<std::string> outside_window(const kit::DisplayList& list, kit::Point window) {
    const kit::Rect whole{0, 0, window.x, window.y};
    std::vector<std::string> problems;
    for (const kit::Item& item : list.items) {
        const kit::Rect area = drawn_area(item);
        if (has_room(area) && !kit::wholly_in(area, whole))
            problems.push_back(
                "item " + std::to_string(static_cast<int>(item.role)) + " \"" + item.text + "\""
            );
    }
    for (const kit::Control& control : list.controls) {
        const kit::Rect area = press_area(control);
        if (has_room(area) && !kit::wholly_in(area, whole))
            problems.push_back("control " + control.name);
    }
    return problems;
}

/// Lists the texts of a layout that do not fit their rectangles by the kit's
/// measure. Buttons, chips, pills, drop-downs, menu items, the header, a
/// heading and an empty field's placeholder are measured.
///
/// @param list the display list
/// @param measured the fonts the layout measured in
/// @return a line for each text that does not fit; none when all fit
inline std::vector<std::string>
texts_not_fitting(const kit::DisplayList& list, const kit::Fonts& measured) {
    const kit::Metrics& metrics = kit::compact_metrics;
    std::vector<std::string> problems;
    const auto check = [&](bool fits, const std::string& what) {
        if (!fits)
            problems.push_back(what);
    };
    const auto width = [&measured](kit::FontRole role, std::string_view text) {
        return kit::text_width(measured, role, text);
    };
    for (const kit::Item& item : list.items) {
        switch (item.role) {
        case kit::Role::text:
            check(width(item.font, item.text) <= item.rect.width, "text \"" + item.text + "\"");
            break;
        case kit::Role::button:
            if (const auto* look = std::get_if<kit::ButtonLook>(&item.look))
                check(
                    width(kit::FontRole::small, look->caption) <= item.rect.width,
                    "button \"" + look->caption + "\""
                );
            break;
        case kit::Role::chip:
            if (const auto* look = std::get_if<kit::ChipLook>(&item.look))
                check(
                    kit::chip_width(measured, *look) <= item.rect.width,
                    "chip \"" + look->text + "\""
                );
            break;
        case kit::Role::pill:
            if (const auto* look = std::get_if<kit::PillLook>(&item.look))
                check(
                    kit::pill_width(measured, *look) <= item.rect.width,
                    "pill \"" + look->text + "\""
                );
            break;
        case kit::Role::choice:
            if (const auto* look = std::get_if<kit::ChoiceLook>(&item.look))
                check(
                    width(kit::FontRole::regular, look->text) <=
                        item.rect.width - metrics.choice_text_inset - metrics.choice_arrow_room,
                    "drop-down \"" + look->text + "\""
                );
            break;
        case kit::Role::choice_menu:
            if (const auto* look = std::get_if<kit::ChoiceMenuLook>(&item.look))
                for (std::size_t place = 0; place < look->shown.size(); ++place) {
                    const kit::Rect box = kit::choice_item(item.rect, static_cast<int32_t>(place));
                    check(
                        width(kit::FontRole::regular, look->shown[place]) <=
                            box.width - metrics.choice_item_text_inset,
                        "menu item \"" + look->shown[place] + "\""
                    );
                }
            break;
        case kit::Role::header:
            if (const auto* look = std::get_if<kit::HeaderLook>(&item.look)) {
                check(
                    kit::tracked_width(
                        measured, kit::FontRole::regular, look->title, look->tracking
                    ) <= look->title_width,
                    "header title \"" + look->title + "\""
                );
                check(
                    kit::tracked_width(
                        measured, kit::FontRole::regular, look->second, look->tracking
                    ) <= look->second_width,
                    "header word \"" + look->second + "\""
                );
                check(
                    width(kit::FontRole::small, look->version) <= look->version_width,
                    "header version \"" + look->version + "\""
                );
                const int32_t title_left = metrics.padding + metrics.mark_side + metrics.header_gap;
                const int32_t words_right =
                    title_left + look->title_width +
                    (look->second.empty() ? 0 : metrics.header_gap + look->second_width);
                const int32_t version_left =
                    look->width - metrics.edge - metrics.padding - look->version_width;
                check(words_right <= version_left, "header words reach the version");
            }
            break;
        case kit::Role::heading:
            if (const auto* look = std::get_if<kit::HeadingLook>(&item.look))
                check(
                    kit::tracked_width(
                        measured, kit::FontRole::small, look->text, metrics.heading_tracking
                    ) <= item.rect.width,
                    "heading \"" + look->text + "\""
                );
            break;
        case kit::Role::field:
            if (const auto* look = std::get_if<kit::SearchLook>(&item.look);
                look != nullptr && look->field.text.empty())
                check(
                    width(kit::FontRole::regular, look->placeholder) <=
                        kit::field_text_rect(item.rect, *look).width,
                    "placeholder \"" + look->placeholder + "\""
                );
            break;
        case kit::Role::toggle:
            if (const auto* look = std::get_if<kit::SwitchLook>(&item.look)) {
                const int32_t half = std::max((item.rect.width - 2) / 2, int32_t{0});
                check(
                    width(kit::FontRole::small, look->off_caption) <= half,
                    "switch off \"" + look->off_caption + "\""
                );
                check(
                    width(kit::FontRole::small, look->on_caption) <= half,
                    "switch on \"" + look->on_caption + "\""
                );
            }
            break;
        case kit::Role::levels:
            if (const auto* look = std::get_if<kit::LevelsLook>(&item.look)) {
                for (const std::string& caption : look->captions) {
                    check(
                        width(kit::FontRole::small, caption) <= look->level_width,
                        "level \"" + caption + "\""
                    );
                }
            }
            break;
        default:
            break;
        }
    }
    return problems;
}

} // namespace key_editor_fixture
