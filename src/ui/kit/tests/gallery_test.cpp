// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The UI kit gallery at Compact (640 by 480 points at 1x), Large (1280 by
// 720 at 1x) and Regular (960 by 540 points, a 1920 by 1080 canvas at 2x):
// every role of the kit's list that the kit's canvas draws shows in it;
// every component role that takes input shows idle, hovered, pressed,
// focused and disabled; every part lies in the frame or its scroll area; no
// two controls overlap; Tab visits every focusable control once in the
// declared order; Down goes to the nearest control below; the names are
// sound and start with gallery.; no text shows the build's version; and
// toggling, choosing and typing change the gallery's model alone. Each case
// runs in the classic look, a synthetic font, and again in the modern look,
// Barlow from the fonts beside the test, which draws other pixels and lays
// the header's title out at its own width.
//
// The synthetic font's glyph for byte b is 2 + b % 4 columns by 8 rows, as
// the other kit tests draw.

#include "oa/formats/fnt.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/gallery.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#ifndef OA_ENGINE_VERSION
#error "OA_ENGINE_VERSION names the build's version, which no text of the gallery may show"
#endif

namespace {

namespace kit = oa::ui::kit;
namespace renderer = oa::ui::frontend_renderer;

/// Each role's name, in the order of the kit's list, for the messages.
constexpr std::array<std::string_view, kit::all_roles.size()> kRoleNames{
    "fill",           "blend",         "outline",       "bevel",          "rule",
    "text",           "picture",       "mark",          "button",         "toggle",
    "levels",         "slider",        "choice",        "choice_menu",    "lock",
    "scroll_bar",     "focus_ring",    "oa_button",     "oa_mark",        "header",
    "footer_band",    "nav",           "heading",       "row_frame",      "locked_fade",
    "list_row",       "chip",          "field",         "tabs",           "card",
    "hover_card",     "link",          "progress",      "header_bar",     "badge",
    "header_text",    "version",       "title",         "panel",          "icon",
    "banner",         "row",           "divider",       "button_main",    "button_danger",
    "progress_track", "progress_fill", "progress_busy", "backdrop",       "sheet",
    "pill",           "language_card", "carousel",      "step_indicator", "setup_chrome",
    "choice_card",    "coach_mark",    "spotlight",     "picture_well",   "call_to_action",
    "package_badge",  "grip",          "keycap",        "chord_chip",     "keyboard",
    "mouse_picture",  "hold_ring",
};

/// Returns a role's name.
///
/// @param role the role
/// @return its name
std::string_view role_name(kit::Role role) {
    return kRoleNames[static_cast<std::size_t>(role)];
}

/// The synthetic font of the settings pixel test, for both roles.
kit::Fonts block_fonts() {
    renderer::TextFont font;
    font.font.nominal_height = 8;
    constexpr uint8_t ink_index = 1;
    for (int byte = 0; byte < 256; ++byte) {
        const auto width = static_cast<uint16_t>(2 + byte % 4);
        constexpr uint16_t height = 8;
        const auto count = static_cast<std::size_t>(width) * height;
        std::vector<uint8_t> pixels(count, ink_index);
        std::vector<uint8_t> coverage(count, 0);
        for (uint16_t y = 0; y < height; ++y)
            for (uint16_t x = 0; x < width; ++x)
                if ((static_cast<int>(x) + y + byte) % 3 != 0)
                    coverage[static_cast<std::size_t>(y) * width + x] = 1;
        font.font.glyphs[static_cast<std::size_t>(byte)] =
            oa::formats::fnt::Glyph{width, height, 0, 0, std::move(pixels), std::move(coverage)};
    }
    font.ink[ink_index] = static_cast<uint16_t>(renderer::blend_opaque);
    return {font, font, {}, {}};
}

/// The fonts the cases are laid out in now: the synthetic fonts, then the
/// modern look's.
const kit::Fonts* checked_fonts = nullptr;
/// The look the cases are laid out in now, as the messages name it.
std::string_view checked_look = "classic";

/// Returns the fonts every case is laid out in.
///
/// @return the fonts of the look being checked
const kit::Fonts& fonts() {
    return *checked_fonts;
}

/// One window the gallery is laid out for.
struct Case {
    std::string_view name{};     ///< as the messages name it
    kit::Viewport canvas{};      ///< the canvas, at its scale
    kit::SizeClass size_class{}; ///< the class it must be
    std::size_t columns{};       ///< the sections' columns
};

const std::array<Case, 3> kCases{{
    {"Compact at 640x480, 1x", {640, 480, 1.0F, 100, {}}, kit::SizeClass::compact, 1},
    {"Large at 1280x720, 1x", {1280, 720, 1.0F, 100, {}}, kit::SizeClass::large, 3},
    {"Regular at 960x540 points, 2x", {1920, 1080, 1.0F, 200, {}}, kit::SizeClass::regular, 2},
}};

/// Returns the frame of a case, its area from the gallery's own origin.
///
/// @param shown the case
/// @return the frame
kit::Frame frame_of(const Case& shown) {
    kit::Frame frame = kit::frame_of(shown.canvas);
    frame.area.x = 0;
    frame.area.y = 0;
    return frame;
}

/// Tells whether a rectangle has area.
///
/// @param rect the rectangle
/// @return true when it is wider and taller than nothing
bool has_area(const kit::Rect& rect) {
    return rect.width > 0 && rect.height > 0;
}

/// Returns the part of a control a press reaches: its rectangle within its
/// clip, nothing while it is disabled.
///
/// @param control the control
/// @return the rectangle; empty when no press reaches it
kit::Rect reachable(const kit::Control& control) {
    if (!control.enabled)
        return {};
    return has_area(control.clip) ? kit::intersect(control.rect, control.clip) : control.rect;
}

/// Calls a function with every part the gallery draws, its panels' parts
/// among them, each as the gallery places it.
///
/// @param laid the gallery
/// @param visit called with each part, its rectangle in the gallery's points
template <class Visit>
void each_part(const kit::GalleryLayout& laid, Visit&& visit) {
    for (const kit::Item& part : laid.list.items)
        visit(part, part.rect);
    for (const kit::GalleryPanel& panel : laid.panels)
        for (const kit::Item& part : panel.list.items)
            visit(
                part,
                kit::Rect{
                    part.rect.x + panel.origin.x,
                    part.rect.y + panel.origin.y,
                    part.rect.width,
                    part.rect.height
                }
            );
}

/// Returns every text a part shows: its own and its look's.
///
/// @param part the part
/// @return the texts
std::vector<std::string> texts_of(const kit::Item& part) {
    std::vector<std::string> texts{part.text};
    texts.insert(texts.end(), part.lines.begin(), part.lines.end());
    const auto add_chip = [&texts](const kit::ChipLook& chip) { texts.push_back(chip.text); };
    std::visit(
        [&](const auto& look) {
            using Look = std::decay_t<decltype(look)>;
            if constexpr (std::is_same_v<Look, kit::HeaderLook>) {
                texts.insert(texts.end(), {look.title, look.second, look.version, look.note});
            } else if constexpr (
                std::is_same_v<Look, kit::ButtonLook> || std::is_same_v<Look, kit::LinkLook>
            ) {
                texts.push_back(look.caption);
            } else if constexpr (std::is_same_v<Look, kit::SwitchLook>) {
                texts.insert(texts.end(), {look.off_caption, look.on_caption});
            } else if constexpr (
                std::is_same_v<Look, kit::LevelsLook> || std::is_same_v<Look, kit::TabsLook>
            ) {
                texts.insert(texts.end(), look.captions.begin(), look.captions.end());
            } else if constexpr (
                std::is_same_v<Look, kit::ChoiceLook> || std::is_same_v<Look, kit::LockLook> ||
                std::is_same_v<Look, kit::HeadingLook>
            ) {
                texts.push_back(look.text);
            } else if constexpr (std::is_same_v<Look, kit::ChoiceMenuLook>) {
                texts.insert(texts.end(), look.shown.begin(), look.shown.end());
                for (const kit::ChoiceMenuRow& row : look.items)
                    texts.push_back(row.aside);
            } else if constexpr (std::is_same_v<Look, kit::NavLook>) {
                for (const kit::NavEntry& entry : look.entries)
                    texts.push_back(entry.caption);
            } else if constexpr (std::is_same_v<Look, kit::RowFrame>) {
                texts.push_back(look.label_text);
                texts.insert(texts.end(), look.hint_texts.begin(), look.hint_texts.end());
            } else if constexpr (std::is_same_v<Look, kit::ListRowLook>) {
                texts.insert(texts.end(), {look.badge_text, look.title, look.by});
                texts.insert(texts.end(), look.lines.begin(), look.lines.end());
                texts.insert(texts.end(), look.aside.begin(), look.aside.end());
                if (look.chip)
                    add_chip(*look.chip);
                for (const kit::ChipLook& tag : look.tags)
                    add_chip(tag);
            } else if constexpr (std::is_same_v<Look, kit::ChipLook>) {
                add_chip(look);
            } else if constexpr (std::is_same_v<Look, kit::PillLook>) {
                texts.push_back(look.text);
            } else if constexpr (std::is_same_v<Look, kit::SearchLook>) {
                texts.insert(texts.end(), {look.placeholder, look.field.text});
            } else if constexpr (std::is_same_v<Look, kit::CardLook>) {
                texts.insert(texts.end(), {look.placeholder, look.title, look.subtitle});
                for (const kit::ChipLook& chip : look.chips)
                    add_chip(chip);
            } else if constexpr (std::is_same_v<Look, kit::HoverCardLook>) {
                texts.insert(texts.end(), {look.title, look.aside});
                for (const kit::HoverCardRow& row : look.rows)
                    texts.insert(texts.end(), {row.label, row.value});
                texts.insert(texts.end(), look.block.begin(), look.block.end());
            } else if constexpr (std::is_same_v<Look, kit::LanguageCardLook>) {
                texts.insert(
                    texts.end(),
                    {look.letters, look.own_name, look.english_name, look.facts, look.pill}
                );
            } else if constexpr (std::is_same_v<Look, kit::CarouselLook>) {
                for (const kit::CarouselCard& card : look.cards)
                    texts.insert(
                        texts.end(),
                        {card.letters, card.own_name, card.english_name, card.facts, card.pill}
                    );
            } else if constexpr (std::is_same_v<Look, kit::StepLook>) {
                texts.push_back(look.name);
            } else if constexpr (std::is_same_v<Look, kit::SetupChromeLook>) {
                texts.insert(
                    texts.end(), {look.page, look.step_name, look.skip, look.back, look.next}
                );
            } else if constexpr (std::is_same_v<Look, kit::ChoiceCardLook>) {
                texts.insert(texts.end(), {look.title, look.hint});
            } else if constexpr (std::is_same_v<Look, kit::CoachMarkLook>) {
                texts.insert(texts.end(), {look.title, look.body});
            } else if constexpr (std::is_same_v<Look, kit::PictureWellLook>) {
                texts.push_back(look.caption);
            } else if constexpr (std::is_same_v<Look, kit::CallToActionLook>) {
                texts.push_back(look.caption);
            } else if constexpr (std::is_same_v<Look, kit::BadgeLook>) {
                texts.push_back(look.letters);
            }
        },
        part.look
    );
    return texts;
}

/// Reports a failed expectation with what the gallery showed.
///
/// @param ok the expectation held
/// @param shown the case
/// @param what what was expected
void expect(bool ok, const Case& shown, const std::string& what) {
    if (!ok)
        std::fprintf(
            stderr,
            "%.*s, %.*s: %s\n",
            static_cast<int>(shown.name.size()),
            shown.name.data(),
            static_cast<int>(checked_look.size()),
            checked_look.data(),
            what.c_str()
        );
    OA_CHECK(ok);
}

/// Every role the kit's list holds and its canvas draws shows at least once,
/// and none it does not draw.
void every_role_shows(const Case& shown, const kit::GalleryLayout& laid) {
    std::set<kit::Role> seen;
    each_part(laid, [&seen](const kit::Item& part, const kit::Rect&) { seen.insert(part.role); });
    for (const kit::Role role : kit::all_roles) {
        if (kit::canvas_draws(role))
            expect(
                seen.contains(role),
                shown,
                "the gallery has no part of role " + std::string(role_name(role))
            );
        else
            expect(
                !seen.contains(role),
                shown,
                "the gallery has a part of role " + std::string(role_name(role)) +
                    ", which the kit's canvas does not draw"
            );
    }
}

/// Every component role that takes input shows in each of the five states:
/// hovered, pressed, focused, disabled, and idle with none of them set.
void every_input_role_shows_five_states(const Case& shown, const kit::GalleryLayout& laid) {
    // A role takes input when a part of it draws one of the gallery's controls.
    std::set<kit::Role> input_roles;
    for (const kit::Item& part : laid.list.items)
        if (part.control != kit::no_control && part.role >= kit::Role::button &&
            kit::control_of(laid.list, part.control) != nullptr)
            input_roles.insert(part.role);
    expect(input_roles.size() >= 12, shown, "fewer than twelve component roles take input");
    for (const kit::Role role : input_roles) {
        bool idle = false;
        bool hovered = false;
        bool pressed = false;
        bool focused = false;
        bool disabled = false;
        for (const kit::Item& part : laid.list.items) {
            if (part.role != role)
                continue;
            const kit::State& state = part.state;
            idle = idle || (!state.hovered && !state.pressed && !state.focused && !state.disabled &&
                            !state.locked);
            hovered = hovered || state.hovered;
            pressed = pressed || state.pressed;
            focused = focused || state.focused;
            disabled = disabled || state.disabled;
        }
        const std::string name(role_name(role));
        expect(idle, shown, name + " shows no idle sample");
        expect(hovered, shown, name + " shows no hovered sample");
        expect(pressed, shown, name + " shows no pressed sample");
        expect(focused, shown, name + " shows no focused sample");
        expect(disabled, shown, name + " shows no disabled sample");
    }
}

/// Every part lies inside the frame, or, clipped to a scroll area's view,
/// inside what that area scrolls; a panel's parts lie inside the panel.
void every_part_lies_in_the_frame_or_its_scroll_area(
    const Case& shown, const kit::Frame& frame, const kit::GalleryLayout& laid
) {
    const kit::Rect whole{0, 0, frame.area.width, frame.area.height};
    const auto content = [](const kit::ScrollArea& area) {
        return kit::Rect{
            area.view.x,
            area.view.y - area.offset,
            area.view.width,
            std::max(area.content_height, area.view.height)
        };
    };
    const kit::Rect sections = content(laid.sections);
    const kit::Rect rows = content(laid.rows);
    for (const kit::Item& part : laid.list.items) {
        if (!has_area(part.clip)) {
            expect(
                kit::wholly_in(part.rect, whole),
                shown,
                "a part of role " + std::string(role_name(part.role)) + " lies outside the frame"
            );
            continue;
        }
        expect(
            kit::wholly_in(part.clip, whole),
            shown,
            "a part of role " + std::string(role_name(part.role)) + " is clipped outside the frame"
        );
        expect(
            kit::wholly_in(part.rect, sections) || kit::wholly_in(part.rect, rows),
            shown,
            "a part of role " + std::string(role_name(part.role)) + " at " +
                std::to_string(part.rect.x) + "," + std::to_string(part.rect.y) + " " +
                std::to_string(part.rect.width) + "x" + std::to_string(part.rect.height) +
                " lies outside its scroll area"
        );
    }
    for (const kit::GalleryPanel& panel : laid.panels) {
        expect(kit::wholly_in(panel.place, sections), shown, "a panel lies outside the sections");
        expect(
            !has_area(panel.clip) || kit::wholly_in(panel.clip, laid.sections.view),
            shown,
            "a panel is clipped outside the sections' view"
        );
        const kit::Rect own{0, 0, panel.place.width, panel.place.height};
        for (const kit::Item& part : panel.list.items)
            expect(
                kit::wholly_in(part.rect, own),
                shown,
                "a panel's part of role " + std::string(role_name(part.role)) +
                    " lies outside the panel"
            );
    }
}

/// No two controls overlap where a press reaches them.
void no_two_controls_overlap(const Case& shown, const kit::GalleryLayout& laid) {
    const auto& controls = laid.list.controls;
    for (std::size_t first = 0; first < controls.size(); ++first)
        for (std::size_t second = first + 1; second < controls.size(); ++second) {
            const kit::Rect shared =
                kit::intersect(reachable(controls[first]), reachable(controls[second]));
            expect(
                !has_area(shared),
                shown,
                controls[first].name + " overlaps " + controls[second].name
            );
        }
}

/// Returns an event of a key.
///
/// @param pressed the key
/// @return the event
kit::GalleryEvent key_event(kit::Key pressed) {
    kit::GalleryEvent event;
    event.kind = kit::GalleryEventKind::key;
    event.key = pressed;
    return event;
}

/// Returns an event of the pointer.
///
/// @param kind what it did
/// @param at where, in the gallery's points
/// @return the event
kit::GalleryEvent pointer_event(kit::GalleryEventKind kind, kit::Point at) {
    kit::GalleryEvent event;
    event.kind = kind;
    event.at = at;
    return event;
}

/// Clicks a point: moves the pointer there, presses and releases.
///
/// @param[in,out] model the model
/// @param frame the frame
/// @param at where, in the gallery's points
/// @return what the release asked
kit::GalleryResult click(kit::GalleryModel& model, const kit::Frame& frame, kit::Point at) {
    std::ignore = kit::gallery_act(
        model, frame, fonts(), pointer_event(kit::GalleryEventKind::pointer_move, at)
    );
    std::ignore = kit::gallery_act(
        model, frame, fonts(), pointer_event(kit::GalleryEventKind::pointer_down, at)
    );
    return kit::gallery_act(
        model, frame, fonts(), pointer_event(kit::GalleryEventKind::pointer_up, at)
    );
}

/// Returns a named control of a layout.
///
/// @param laid the gallery
/// @param name the control's name
/// @return a copy of it; an empty control when there is none
kit::Control control_named(const kit::GalleryLayout& laid, std::string_view name) {
    const kit::ControlId id = kit::control_named(laid.list, name);
    const kit::Control* found = kit::control_of(laid.list, id);
    return found != nullptr ? *found : kit::Control{};
}

/// Tab visits every focusable control once, in the declared order, and comes
/// back to the first; each one it visits shows.
void tab_visits_every_control_once(const Case& shown, const kit::Frame& frame) {
    const kit::GalleryModel start;
    const kit::GalleryLayout laid = kit::gallery_layout(start, frame, fonts());
    const std::vector<kit::ControlId>& order = laid.list.tab_order;
    std::set<kit::ControlId> declared(order.begin(), order.end());
    expect(declared.size() == order.size(), shown, "the Tab order names a control twice");
    std::size_t focusable = 0;
    for (const kit::Control& control : laid.list.controls) {
        if (!control.focusable)
            continue;
        ++focusable;
        expect(declared.contains(control.id), shown, control.name + " is not in the Tab order");
    }
    expect(focusable == order.size(), shown, "the Tab order holds a control that takes no focus");
    expect(
        !order.empty() && order.back() == control_named(laid, "gallery.close").id,
        shown,
        "CLOSE is not last in the Tab order"
    );
    kit::GalleryModel model = start;
    std::set<kit::ControlId> visited;
    for (const kit::ControlId id : order) {
        const kit::GalleryResult result =
            kit::gallery_act(model, frame, fonts(), key_event(kit::Key::tab));
        expect(
            result == kit::GalleryResult::redraw && model.interaction.focused == id,
            shown,
            "Tab did not move to the next control of the declared order, " + std::to_string(id)
        );
        visited.insert(model.interaction.focused);
        // The control the focus moved to shows, and a press reaches it.
        const kit::GalleryLayout now = kit::gallery_layout(model, frame, fonts());
        const kit::Control* focused = kit::control_of(now.list, id);
        expect(
            focused != nullptr && has_area(reachable(*focused)),
            shown,
            "Tab moved the focus to a control out of view, " + std::to_string(id)
        );
    }
    expect(visited.size() == order.size(), shown, "Tab did not visit every control once");
    std::ignore = kit::gallery_act(model, frame, fonts(), key_event(kit::Key::tab));
    expect(model.interaction.focused == order.front(), shown, "Tab did not come back to the first");
}

/// Down from each control moves to the nearest control below it: one in
/// line with it at the smallest gap when there is one, else one below it,
/// and with none below it the focus stays. Where controls lie is judged in
/// the gallery laid out whole, as tall as shows every section unscrolled:
/// a control the view does not show yet lies where the sections put it, and
/// the footer's CLOSE under them all.
void down_moves_to_the_nearest_control_below(const Case& shown, const kit::Frame& frame) {
    kit::GalleryModel walker;
    const kit::GalleryLayout first = kit::gallery_layout(walker, frame, fonts());
    const std::size_t count = first.list.tab_order.size();
    kit::Frame tall = frame;
    tall.area.height +=
        std::max(first.sections.content_height - first.sections.view.height, int32_t{0});
    for (std::size_t step_at = 0; step_at < count; ++step_at) {
        std::ignore = kit::gallery_act(walker, frame, fonts(), key_event(kit::Key::tab));
        kit::GalleryModel unscrolled = walker;
        unscrolled.scroll = 0;
        const kit::GalleryLayout laid = kit::gallery_layout(unscrolled, tall, fonts());
        expect(laid.sections.limit == 0, shown, "the gallery laid out whole still scrolls");
        const kit::Control* from = kit::control_of(laid.list, walker.interaction.focused);
        if (from == nullptr)
            continue;
        const int32_t from_bottom = from->rect.y + from->rect.height;
        // The candidates: the focusable controls that take a press, a control
        // of a scroll area while some of it shows or while it shares the
        // area with the focused one.
        const auto candidate = [&](const kit::Control& control, bool same_area) {
            if (control.id == from->id || !control.focusable || !control.enabled)
                return false;
            if (same_area != (from->group >= 0 && control.group == from->group))
                return false;
            return control.group < 0 || same_area || has_area(reachable(control));
        };
        const auto in_line = [&](const kit::Control& control) {
            return control.rect.x < from->rect.x + from->rect.width &&
                   from->rect.x < control.rect.x + control.rect.width;
        };
        std::vector<const kit::Control*> below;
        for (const bool same_area : {true, false}) {
            for (const kit::Control& control : laid.list.controls)
                if (candidate(control, same_area) && control.rect.y >= from_bottom)
                    below.push_back(&control);
            if (!below.empty())
                break;
        }
        int32_t nearest = -1;
        for (const kit::Control* control : below)
            if (in_line(*control) && (nearest < 0 || control->rect.y - from_bottom < nearest))
                nearest = control->rect.y - from_bottom;
        kit::GalleryModel moved = walker;
        std::ignore = kit::gallery_act(moved, frame, fonts(), key_event(kit::Key::down));
        const kit::Control* landed = kit::control_of(laid.list, moved.interaction.focused);
        const std::string from_name = from->name;
        if (below.empty()) {
            expect(
                moved.interaction.focused == from->id,
                shown,
                "Down from " + from_name + " moved the focus with nothing below"
            );
            continue;
        }
        expect(
            landed != nullptr && landed->id != from->id && landed->rect.y >= from_bottom,
            shown,
            "Down from " + from_name + " did not move to a control below it"
        );
        if (landed != nullptr && nearest >= 0)
            expect(
                in_line(*landed) && landed->rect.y - from_bottom == nearest,
                shown,
                "Down from " + from_name + " moved to " + landed->name +
                    ", not the nearest control below it"
            );
    }
}

/// The names are sound, every one starts with gallery., and the working
/// samples carry the names automation finds them by.
void names_are_sound(const Case& shown, const kit::GalleryLayout& laid) {
    const std::string problem = kit::name_problem(laid.list);
    expect(problem.empty(), shown, "a name is not sound: " + problem);
    for (const kit::Control& control : laid.list.controls)
        expect(
            control.name.starts_with("gallery."),
            shown,
            control.name + " does not start with gallery."
        );
    for (const std::string_view name :
         {"gallery.close",
          "gallery.toggle.1",
          "gallery.drop-down.1",
          "gallery.tabs.1",
          "gallery.slider.1",
          "gallery.search-field.1",
          "gallery.chips.1",
          "gallery.list-rows.1",
          "gallery.scroll-list.4",
          "gallery.card-grid.3",
          "gallery.nav-list.4",
          "gallery.oa-mark.1"})
        expect(
            kit::control_named(laid.list, name) != kit::no_control,
            shown,
            std::string(name) + " is missing"
        );
    bool badges = false;
    each_part(laid, [&](const kit::Item& part, const kit::Rect&) {
        if (part.text == "BADGES AND TAGS")
            badges = true;
    });
    expect(badges, shown, "the badges section is missing");
    // The switch's halves and the levels are parts automation presses.
    const std::vector<kit::AutomationEntry> entries = kit::automation_parts(laid.list, {});
    for (const std::string_view part :
         {"gallery.toggle.1.on", "gallery.toggle.1.off", "gallery.level-strip.1.full"})
        expect(
            std::any_of(
                entries.begin(),
                entries.end(),
                [part](const kit::AutomationEntry& entry) { return entry.name == part; }
            ),
            shown,
            std::string(part) + " is not listed"
        );
}

/// No text shows the build's version, and the window chrome's sample shows
/// gallery_version.
void no_text_shows_the_version(const Case& shown, const kit::GalleryLayout& laid) {
    const std::string version = OA_ENGINE_VERSION;
    bool chrome_version = false;
    each_part(laid, [&](const kit::Item& part, const kit::Rect&) {
        for (const std::string& text : texts_of(part))
            expect(
                version.empty() || text.find(version) == std::string::npos,
                shown,
                "a part shows the build's version: " + text
            );
        if (const auto* header = std::get_if<kit::HeaderLook>(&part.look))
            chrome_version = chrome_version || header->version == kit::gallery_version;
    });
    for (const kit::Control& control : laid.list.controls)
        expect(
            control.text.find(version) == std::string::npos,
            shown,
            control.name + " shows the build's version"
        );
    expect(chrome_version, shown, "the window chrome's sample does not show v1.2.3");
}

/// Tells whether two models hold the same samples, their pointer, focus and
/// offsets left out.
///
/// @param a a model
/// @param b another
/// @return true when every working sample's value is the same
bool same_samples(const kit::GalleryModel& a, const kit::GalleryModel& b) {
    return a.tab == b.tab && a.presses == b.presses && a.toggle_on == b.toggle_on &&
           a.level == b.level && a.language == b.language && a.menu_open == b.menu_open &&
           a.slider_stop == b.slider_stop && a.search.text == b.search.text && a.chips == b.chips &&
           a.nav == b.nav && a.row == b.row && a.list_row == b.list_row && a.card == b.card;
}

/// Returns a working control's middle in a model's layout, once the focus on
/// it has scrolled it into view.
///
/// @param[in,out] model the model; Tab moves its focus to the control
/// @param frame the frame
/// @param name the control's name
/// @return its middle, in the gallery's points
kit::Point show_and_find(kit::GalleryModel& model, const kit::Frame& frame, std::string_view name) {
    for (int presses = 0; presses < 64; ++presses) {
        const kit::GalleryLayout laid = kit::gallery_layout(model, frame, fonts());
        const kit::Control found = control_named(laid, name);
        if (model.interaction.focused == found.id && model.interaction.focus_shown) {
            const kit::Rect shows = reachable(found);
            return {shows.x + shows.width / 2, shows.y + shows.height / 2};
        }
        std::ignore = kit::gallery_act(model, frame, fonts(), key_event(kit::Key::tab));
    }
    return {};
}

/// Toggling, choosing and typing change the model alone, each its own
/// sample's value, and the layout shows the new value.
void using_the_samples_changes_the_model_alone(const Case& shown, const kit::Frame& frame) {
    // The switch's On half turns it On, and Space turns it Off again.
    {
        kit::GalleryModel model;
        const kit::Point middle = show_and_find(model, frame, "gallery.toggle.1");
        kit::GalleryModel before = model;
        std::ignore = click(model, frame, {middle.x + 10, middle.y});
        kit::GalleryModel expected = before;
        expected.toggle_on = true;
        expect(
            model.toggle_on && same_samples(model, expected),
            shown,
            "a click on On did not turn the switch On alone"
        );
        const kit::GalleryLayout laid = kit::gallery_layout(model, frame, fonts());
        const kit::ControlId id = kit::control_named(laid.list, "gallery.toggle.1");
        const auto drawn = std::find_if(
            laid.list.items.begin(), laid.list.items.end(), [id](const kit::Item& part) {
                return part.control == id && part.role == kit::Role::toggle;
            }
        );
        expect(
            drawn != laid.list.items.end() && drawn->state.on, shown, "the switch does not show On"
        );
        std::ignore = kit::gallery_act(model, frame, fonts(), key_event(kit::Key::space));
        expect(
            !model.toggle_on && same_samples(model, before),
            shown,
            "Space did not turn the switch Off alone"
        );
    }
    // The drop-down opens, and a click on Deutsch chooses it and closes it.
    {
        kit::GalleryModel model;
        const kit::Point middle = show_and_find(model, frame, "gallery.drop-down.1");
        const kit::GalleryModel before = model;
        std::ignore = click(model, frame, middle);
        expect(model.menu_open, shown, "a click on the drop-down did not open its menu");
        const kit::GalleryLayout open = kit::gallery_layout(model, frame, fonts());
        expect(has_area(open.menu), shown, "the open menu is not laid out");
        const std::vector<kit::AutomationEntry> entries =
            kit::automation_parts(open.list, model.interaction);
        expect(
            std::any_of(
                entries.begin(),
                entries.end(),
                [](const kit::AutomationEntry& entry) {
                    return entry.name == "gallery.drop-down.1.deutsch";
                }
            ),
            shown,
            "the open menu's items are not listed"
        );
        const kit::Rect item = kit::choice_item(open.menu, 1);
        std::ignore = click(model, frame, {item.x + item.width / 2, item.y + item.height / 2});
        kit::GalleryModel expected = before;
        expected.language = 1;
        expect(
            model.language == 1 && !model.menu_open && same_samples(model, expected),
            shown,
            "a click on Deutsch did not choose it alone"
        );
        const kit::GalleryLayout chosen = kit::gallery_layout(model, frame, fonts());
        expect(
            control_named(chosen, "gallery.drop-down.1").text == "Deutsch",
            shown,
            "the drop-down does not show Deutsch"
        );
    }
    // The search field takes typing and Backspace.
    {
        kit::GalleryModel model;
        const kit::Point middle = show_and_find(model, frame, "gallery.search-field.1");
        const kit::GalleryModel before = model;
        std::ignore = click(model, frame, middle);
        kit::GalleryEvent typed;
        typed.kind = kit::GalleryEventKind::text;
        typed.text = "Sample";
        expect(
            kit::gallery_act(model, frame, fonts(), typed) == kit::GalleryResult::redraw,
            shown,
            "typing did not redraw"
        );
        std::ignore = kit::gallery_act(model, frame, fonts(), key_event(kit::Key::backspace));
        kit::GalleryModel expected = before;
        expected.search.text = "Sampl";
        expect(
            model.search.text == "Sampl" && same_samples(model, expected),
            shown,
            "typing and Backspace did not change the field alone"
        );
        const kit::GalleryLayout laid = kit::gallery_layout(model, frame, fonts());
        expect(
            control_named(laid, "gallery.search-field.1").text == "Sampl",
            shown,
            "the field does not show what was typed"
        );
        expect(
            kit::gallery_text_field(model, laid).has_value(),
            shown,
            "the field has the focus but gives no text field"
        );
    }
    // Escape closes the gallery, and CLOSE does.
    {
        kit::GalleryModel model;
        expect(
            kit::gallery_act(model, frame, fonts(), key_event(kit::Key::escape)) ==
                kit::GalleryResult::close,
            shown,
            "Escape did not close the gallery"
        );
        const kit::GalleryLayout laid = kit::gallery_layout(model, frame, fonts());
        const kit::Rect close = reachable(control_named(laid, "gallery.close"));
        expect(
            click(model, frame, {close.x + close.width / 2, close.y + close.height / 2}) ==
                kit::GalleryResult::close,
            shown,
            "CLOSE did not close the gallery"
        );
    }
}

/// Draws the gallery on a canvas of its frame at its scale.
///
/// @param frame the frame
/// @param laid the gallery laid out
/// @param drawn the fonts it is drawn in
/// @return the picture
renderer::Surface
gallery_picture(const kit::Frame& frame, const kit::GalleryLayout& laid, const kit::Fonts& drawn) {
    const int32_t scale = std::max(frame.scale_percent / 100, int32_t{1});
    renderer::Surface surface;
    surface.width = static_cast<uint32_t>(frame.area.width * scale);
    surface.height = static_cast<uint32_t>(frame.area.height * scale);
    surface.rgb.assign(static_cast<std::size_t>(surface.width) * surface.height * 3U, 0);
    const kit::Canvas canvas{&surface, {0, 0, scale, {}}, &drawn, {}};
    kit::draw_gallery(canvas, laid);
    return surface;
}

/// The gallery follows the look: in the modern look its header's title is
/// as wide as Barlow Condensed draws it at the case's class, and its text is
/// drawn in other pixels than the classic look's.
void the_gallery_follows_the_look(
    const Case& shown, const kit::Frame& frame, const kit::Fonts& classic, const kit::Fonts& modern
) {
    const kit::GalleryModel model;
    const kit::GalleryLayout in_classic = kit::gallery_layout(model, frame, classic);
    const kit::GalleryLayout in_modern = kit::gallery_layout(model, frame, modern);
    // The window's header: its title and the width laid out for it.
    const auto header_of = [](const kit::GalleryLayout& laid) -> const kit::HeaderLook* {
        for (const kit::Item& item : laid.list.items)
            if (item.role == kit::Role::header)
                if (const auto* look = std::get_if<kit::HeaderLook>(&item.look))
                    return look;
        return nullptr;
    };
    const kit::HeaderLook* classic_header = header_of(in_classic);
    const kit::HeaderLook* modern_header = header_of(in_modern);
    expect(classic_header != nullptr && modern_header != nullptr, shown, "no header");
    if (classic_header == nullptr || modern_header == nullptr)
        return;
    expect(
        modern_header->title_width ==
            kit::tracked_width(
                modern, kit::FontRole::regular, modern_header->title, modern_header->tracking
            ),
        shown,
        "the modern look's header title is not Barlow Condensed's width"
    );
    expect(
        modern_header->title_width ==
            kit::modern_width(
                *modern.modern,
                modern_header->title,
                kit::modern_type(kit::FontRole::regular, true, shown.size_class),
                modern_header->tracking
            ),
        shown,
        "the modern look's header title is not measured in its tracked type"
    );
    expect(
        classic_header->title_width != modern_header->title_width,
        shown,
        "the header title keeps the classic width"
    );
    expect(
        gallery_picture(frame, in_classic, classic).rgb !=
            gallery_picture(frame, in_modern, modern).rgb,
        shown,
        "the modern look draws the classic look's pixels"
    );
}

/// The gallery draws on a canvas of its frame at its scale.
void the_gallery_draws(const Case& shown, const kit::Frame& frame, const kit::GalleryLayout& laid) {
    const int32_t scale = std::max(frame.scale_percent / 100, int32_t{1});
    renderer::Surface surface;
    surface.width = static_cast<uint32_t>(frame.area.width * scale);
    surface.height = static_cast<uint32_t>(frame.area.height * scale);
    surface.rgb.assign(static_cast<std::size_t>(surface.width) * surface.height * 3U, 0);
    const kit::Canvas canvas{&surface, {0, 0, scale, {}}, &fonts(), {}};
    kit::draw_gallery(canvas, laid);
    // The face is the panel's colour where nothing else lies: its top-left
    // inner corner, under the header's band, is the band's.
    const std::size_t at = (static_cast<std::size_t>(2 * scale) * surface.width +
                            static_cast<std::size_t>(2 * scale)) *
                           3U;
    expect(
        surface.rgb[at] == kit::colour::band.r && surface.rgb[at + 1] == kit::colour::band.g &&
            surface.rgb[at + 2] == kit::colour::band.b,
        shown,
        "the header's band is not drawn"
    );
}

} // namespace

int main() {
    static const kit::Fonts classic = block_fonts();
    const std::unique_ptr<oa::platform::text_font::FontStack> stack =
        oa::platform::text_font::FontStack::open(oa::platform::text_font::bundled_font_directory());
    OA_CHECK(stack != nullptr);
    if (!stack)
        return oa::test::check_exit_status();
    kit::Fonts modern = classic;
    modern.modern = stack.get();
    for (const auto& [look, look_fonts] :
         {std::pair<std::string_view, const kit::Fonts*>{"classic", &classic},
          std::pair<std::string_view, const kit::Fonts*>{"modern", &modern}}) {
        checked_look = look;
        checked_fonts = look_fonts;
        for (const Case& shown : kCases) {
            const kit::Frame frame = frame_of(shown);
            expect(frame.size_class == shown.size_class, shown, "the frame is not of its class");
            // The modern look's type takes the case's class, as the OA
            // layer hands its screens their class's fonts.
            modern.size_class = shown.size_class;
            const kit::GalleryModel model;
            const kit::GalleryLayout laid = kit::gallery_layout(model, frame, fonts());
            expect(
                laid.columns == shown.columns, shown, "the sections do not flow in their columns"
            );
            every_role_shows(shown, laid);
            every_input_role_shows_five_states(shown, laid);
            every_part_lies_in_the_frame_or_its_scroll_area(shown, frame, laid);
            no_two_controls_overlap(shown, laid);
            tab_visits_every_control_once(shown, frame);
            down_moves_to_the_nearest_control_below(shown, frame);
            names_are_sound(shown, laid);
            no_text_shows_the_version(shown, laid);
            using_the_samples_changes_the_model_alone(shown, frame);
            the_gallery_draws(shown, frame, laid);
            if (shown.size_class != kit::SizeClass::compact) {
                const kit::Item* coach = nullptr;
                for (const kit::Item& item : laid.list.items)
                    if (item.role == kit::Role::coach_mark)
                        coach = &item;
                expect(
                    coach != nullptr && coach->rect.width >= 140,
                    shown,
                    "the coach mark is squeezed"
                );
            }
        }
    }
    for (const Case& shown : kCases) {
        modern.size_class = shown.size_class;
        the_gallery_follows_the_look(shown, frame_of(shown), classic, modern);
    }
    return oa::test::check_exit_status();
}
