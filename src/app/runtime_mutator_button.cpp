// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Mutators on the skirmish setup, the campaign launch and the battle room's
// host: one of the room's own on and off buttons, drawn from the room's art
// (brain_seats_room.hpp), with the OA stamp, the word "Mutators" and its
// lamp, as 3.1c's on and off buttons light theirs: green while mutators are
// on, red while none is on, and blinking red while a player in the battle
// room lacks one. Its card names the mutators on,
// and its text for automation counts them. It stands in line with the
// room's own buttons, as large as they are: on the skirmish setup as large
// as Previous Menu, in its column, centred between Difficulty and Previous
// Menu; on NEWGAME and Any Mission as large as Previous Menu, at the head of
// Difficulty's column, over Set Difficulty and under the side bar; in the
// battle room as large as Select Map, over it. Each place is worked out
// from the 3.1c gadgets on every window, and falls back to MORE MAPS…'s
// rule when it would meet one. The button joins that screen's Tab order,
// where the room's own focus marker shows on it. Y opens the picker. In the
// battle room a launch that locks the host's options draws the button
// grayed, and the line beside START that names a player who lacks a
// mutator is in the room's label font.

#include "brain_seats_room.hpp"
#include "oa_button_focus.hpp"
#include "oa_layer.hpp"
#include "oa/app/mutators_memory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/window_icon.hpp"

#include "oa/data/languages/interface_text.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/present/game_text.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_multiplayer/get_map.hpp"
#include "oa/ui/frontend_multiplayer/mutator_room.hpp"
#include "oa/ui/frontend_multiplayer/presence_card.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/frontend_state/oa_button_place.hpp"
#include "oa/ui/gui_layout.hpp"
#include "oa/ui/gui_layout/gui_gadget.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/mutator_picker/model.hpp"
#include "oa/ui/frontend_renderer/room_parts.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/pad_controls.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

/// Reads the runtime fields the button's helpers need. The helpers are not
/// members; the screen and the start path are.
struct MutatorButtonAccess {
    static Screen screen(const Runtime& runtime) noexcept { return runtime.screen_; }

    static bool match_running(const Runtime& runtime) noexcept { return runtime.match_ != nullptr; }

    static const renderer::ScreenResources& resources(const Runtime& runtime) noexcept {
        return runtime.resources_;
    }

    static const oa::ui::kit::Fonts* screen_fonts(Runtime& runtime) {
        return runtime.oa_layer().screen_fonts();
    }

    static bool names_equal(std::string_view left, std::string_view right) {
        return Runtime::tdf_names_equal(left, right);
    }
};

namespace {

namespace picker = oa::ui::mutator_picker;
namespace place = oa::ui::frontend_state;
namespace mp = oa::ui::frontend_multiplayer;
namespace pad = oa::ui::pad_controls;
namespace kit = oa::ui::kit;
namespace gui = oa::ui::gui_layout;

namespace room = oa::ui::frontend_renderer;

constexpr std::string_view screen_name = "mutators-button";
constexpr std::string_view button_name = "mutators";
constexpr std::string_view reason_name = "mutators.waiting";
constexpr std::string_view word_caption = "Mutators";
constexpr std::string_view word_off = "Mutators · off";
constexpr std::string_view word_counted = "Mutators · {count} on";
constexpr std::string_view word_card_off = "No mutators on";
constexpr std::string_view word_card_one = "1 mutator on";
constexpr std::string_view word_card_count = "{count} mutators on";
constexpr std::string_view word_card_more = "and {count} more";
constexpr std::string_view word_card_lacking = "not installed";
constexpr std::string_view word_card_order = "Applied in this order.";
constexpr std::string_view word_card_change = "Click to change.";
constexpr std::string_view word_card_read = "Click to read.";
constexpr kit::ControlId button_id = 1;
constexpr kit::ControlId reason_id = 2;
/// The card's slot: the button, the one thing it describes.
constexpr int32_t card_slot = 0;
/// The least gap above and below the button where it is centred in a space.
constexpr int32_t least_gap = 2;
/// The gap MORE MAPS…'s rule leaves, where the button falls back to it.
constexpr int32_t anchor_gap = 4;
/// The waiting line beside START, at its widest, in points.
constexpr int32_t reason_line_width = 400;

/// Returns one of the button's words in the language shown.
///
/// @param english the words, in English
/// @return their translation, or the English
std::string shown(std::string_view english) {
    return std::string(oa::data::languages::interface_text(english));
}

place::PlaceRect as_place(const kit::Rect& rect) noexcept {
    return {rect.x, rect.y, rect.width, rect.height};
}

kit::Rect as_kit(const place::PlaceRect& rect) noexcept {
    return {rect.x, rect.y, rect.width, rect.height};
}

/// Grows a rectangle by the focus ring's margin.
///
/// @param rect the rectangle
/// @param margin the margin, in points
/// @return the grown rectangle
kit::Rect outset(const kit::Rect& rect, int32_t margin) noexcept {
    return {rect.x - margin, rect.y - margin, rect.width + margin * 2, rect.height + margin * 2};
}

/// Returns the union of two rectangles.
///
/// @param into the rectangle so far; empty when its width is not positive
/// @param extra the rectangle to include
/// @return the union
kit::Rect unite(const kit::Rect& into, const kit::Rect& extra) noexcept {
    if (into.width <= 0 || into.height <= 0)
        return extra;
    const int32_t left = std::min(into.x, extra.x);
    const int32_t top = std::min(into.y, extra.y);
    const int32_t right = std::max(into.x + into.width, extra.x + extra.width);
    const int32_t bottom = std::max(into.y + into.height, extra.y + extra.height);
    return {left, top, right - left, bottom - top};
}

kit::Rect local_of(const kit::Rect& rect, const kit::Rect& origin) noexcept {
    return {rect.x - origin.x, rect.y - origin.y, rect.width, rect.height};
}

/// Tells whether a gadget takes the keyboard focus on this screen.
///
/// The gadget engine's own rule says which records Tab stops on. A record
/// with no area is not drawn, so this screen leaves it out even when that
/// rule would count it.
///
/// @param gadget the gadget
/// @return true when Tab can stop on it
bool takes_focus(const gui::Gadget& gadget) noexcept {
    if (gadget.common.width <= 0 || gadget.common.height <= 0)
        return false;
    return gui::keyboard_focusable(gadget);
}

/// Tells whether Mutators shows over this screen.
///
/// @param runtime the runtime
/// @return true on the skirmish setup, NEWGAME and Any Mission, with no match and no dialog
bool button_shows(Runtime& runtime) {
    const Screen screen = MutatorButtonAccess::screen(runtime);
    if (screen != Screen::skirmish && screen != Screen::new_campaign &&
        screen != Screen::any_mission)
        return false;
    if (MutatorButtonAccess::match_running(runtime))
        return false;
    if (oa::ui::frontend_dialogs::dialog_count() != 0)
        return false;
    return MutatorButtonAccess::screen_fonts(runtime) != nullptr;
}

mutators_memory::Setup memory_setup(const Runtime& runtime) noexcept {
    const Screen screen = MutatorButtonAccess::screen(runtime);
    if (screen == Screen::new_campaign || screen == Screen::any_mission)
        return mutators_memory::Setup::campaign;
    return mutators_memory::Setup::skirmish;
}

picker::Setup picker_setup(const Runtime& runtime) noexcept {
    const Screen screen = MutatorButtonAccess::screen(runtime);
    if (screen == Screen::new_campaign || screen == Screen::any_mission)
        return picker::Setup::campaign;
    return picker::Setup::skirmish;
}

/// Reads a remembered loadout as the set the picker opens on.
///
/// Clash is left unset: the picker fills it from the manifest.
///
/// @param loadout the remembered set
/// @return the picker's chosen records
std::vector<picker::Chosen> chosen_from_loadout(const oa::data::mutators::Loadout& loadout) {
    std::vector<picker::Chosen> chosen;
    chosen.reserve(loadout.entries.size());
    for (const oa::data::mutators::LoadoutEntry& entry : loadout.entries) {
        picker::Chosen one;
        one.id = entry.id;
        one.options.reserve(entry.options.size());
        for (const oa::data::mutators::LoadoutOption& option : entry.options)
            one.options.emplace_back(option.id, oa::data::mutators::option_text(option));
        chosen.push_back(std::move(one));
    }
    return chosen;
}

/// The button's word, "Mutators", in the language shown.
///
/// @return the word
std::string caption() {
    return shown(word_caption);
}

/// The button's text for automation for a set of a count, its word whole:
/// "Mutators · off", or "Mutators · 4 on".
///
/// @param count how many mutators the set holds
/// @return the text, in the language shown
std::string count_text(std::size_t count) {
    if (count == 0)
        return shown(word_off);
    std::string text = shown(word_counted);
    constexpr std::string_view place_holder = "{count}";
    if (const std::size_t at = text.find(place_holder); at != std::string::npos)
        text.replace(at, place_holder.size(), std::to_string(count));
    return text;
}

/// Where the button lies, and which records neighbour it.
struct Geometry {
    bool ok{};          ///< the anchor was found and the button was placed
    kit::Rect bounds{}; ///< the screen's place, in points, including the focus margin
    kit::Rect button{}; ///< the button, in points
    std::vector<FocusRecord> focusable{}; ///< the records Tab can stop on
    place::PlaceRect button_points{};     ///< the button, for the order
    /// Every gadget with an area, the anchor included, in the screen's points.
    std::vector<place::PlaceRect> gadgets{};
    int32_t anchor_index{-1};          ///< the anchor's record
    int32_t before{-1};                ///< the record just before the button
    int32_t after{-1};                 ///< the record just after the button
    int32_t first{-1};                 ///< the first record in the order
    int32_t last{-1};                  ///< the last record in the order
    int32_t below{-1};                 ///< the next record down the anchor's column
    bool battle{};                     ///< the battle room's host block
    bool locked{};                     ///< a launch locks the host's options
    std::optional<kit::Rect> reason{}; ///< the waiting line beside START
    std::string reason_text{};         ///< that line's words
    room::SourceRect frame{};          ///< the button, in frame pixels
    std::optional<kit::Rect> card{};   ///< the open card, in the layer's points
    std::optional<kit::Rect> arrow{};  ///< its arrow, in the layer's points
    kit::HoverCardLook card_look{};    ///< the open card's look, its arrow pointed
};

/// Puts the button's frame pixels on a geometry, and grows its bounds by
/// the room's focus marker round them and by the open card.
///
/// @param laid the geometry, its button and bounds placed
/// @param placed_on what the screen is placed on
/// @param card the open card's look; null while it is closed
/// @param fonts the fonts the card is measured in
void add_frame(
    Geometry& laid,
    const LayerView& placed_on,
    const kit::HoverCardLook* card,
    const kit::Fonts& fonts
) {
    laid.frame = points_rect_frame(placed_on, laid.button);
    const room::SourceRect marked{
        laid.frame.x - room_focus_margin,
        laid.frame.y - room_focus_margin,
        laid.frame.width + 2 * room_focus_margin,
        laid.frame.height + 2 * room_focus_margin
    };
    laid.bounds = unite(laid.bounds, frame_rect_points(placed_on, marked));
    if (card == nullptr)
        return;
    const int32_t arrow_depth = kit::metrics_of(placed_on.frame.size_class).hover_card_arrow;
    laid.card_look = *card;
    const kit::Point size = kit::hover_card_size(fonts, laid.card_look);
    const kit::Rect placed =
        mp::presence_card_place(laid.button, size, placed_on.frame.area, arrow_depth);
    mp::presence_card_point(laid.card_look, placed, laid.button);
    laid.card = placed;
    laid.bounds = unite(laid.bounds, placed);
    const int32_t across = 2 * arrow_depth - 1;
    const int32_t left = placed.x + laid.card_look.arrow_at - (arrow_depth - 1);
    if (laid.card_look.arrow == kit::Side::top)
        laid.arrow = kit::Rect{left, placed.y - arrow_depth, across, arrow_depth};
    else if (laid.card_look.arrow == kit::Side::bottom)
        laid.arrow = kit::Rect{left, placed.y + placed.height, across, arrow_depth};
    if (laid.arrow)
        laid.bounds = unite(laid.bounds, *laid.arrow);
}

/// The width a button needs for the OA badge and its word, where it falls
/// back to MORE MAPS…'s rule and is not sized by a gadget.
///
/// @param fonts the fonts
/// @param height the button's height
/// @param words the word it shows
/// @return the width, in points
int32_t natural_width(const kit::Fonts& fonts, int32_t height, std::string_view words) {
    const int32_t pad = std::max(height / 5, int32_t{2});
    const int32_t side = height - 2 * pad;
    return pad + side + pad + kit::text_width(fonts, kit::FontRole::small, words) + pad;
}

/// The name a player record shows, within its field.
///
/// @param name the field
/// @param bytes how many bytes the field holds
/// @return the name
std::string name_of(const char* name, std::size_t bytes) {
    if (name == nullptr || bytes == 0)
        return {};
    return std::string(name, ::strnlen(name, bytes));
}

/// Reads a resolved room list as the set the picker opens on.
///
/// @param resolved the room's list
/// @return the picker's chosen records
std::vector<picker::Chosen>
chosen_from_resolved(std::span<const oa::data::mutators::ResolvedMutator> resolved) {
    std::vector<picker::Chosen> chosen;
    chosen.reserve(resolved.size());
    for (const oa::data::mutators::ResolvedMutator& one : resolved) {
        picker::Chosen item;
        item.id = one.manifest.id;
        item.options.reserve(one.options.size());
        for (const oa::data::mutators::LoadoutOption& option : one.options)
            item.options.emplace_back(option.id, oa::data::mutators::option_text(option));
        if (one.clashes) {
            item.clash = *one.clashes == oa::formats::oamut::ClashMode::add
                             ? picker::Clash::add
                             : picker::Clash::replace;
        }
        chosen.push_back(std::move(item));
    }
    return chosen;
}

/// Tells whether the host's Mutators shows over the battle room.
///
/// @param runtime the runtime
/// @return true while the host's battle room is the screen in front
bool room_host_shows(Runtime& runtime) {
    if (MutatorButtonAccess::screen_fonts(runtime) == nullptr)
        return false;
    mp::BattleRoomMap map;
    if (!mp::multiplayer_battle_room_map(&map))
        return false;
    return !map.joiner;
}

/// The first seated player who lacks a mutator, while START waits on mutators.
///
/// @param runtime the runtime
/// @return that player and mutator, or nothing
std::optional<mp::PlayerLack> first_room_lack(Runtime& runtime) {
    const auto held = runtime.room_mutators();
    std::vector<std::string> names;
    names.reserve(held.size());
    for (const oa::data::mutators::ResolvedMutator& one : held)
        names.push_back(one.manifest.name.empty() ? one.manifest.id : one.manifest.name);
    if (names.empty())
        return std::nullopt;
    mp::Lobby& lobby = mp::multiplayer_lobby();
    if (lobby.game == nullptr || !mp::lobby_hosts(lobby))
        return std::nullopt;
    if (mp::lobby_mutators_block(lobby).hold == mp::MutatorsHold::none)
        return std::nullopt;
    const std::size_t count = std::min(names.size(), std::size_t{32});
    for (int32_t slot = 0; slot < mp::kSlotCount; ++slot) {
        const Player& player = mp::slot_player(lobby, slot);
        if (!mp::slot_active(player))
            continue;
        for (std::size_t position = 0; position < count; ++position) {
            const auto have = mp::lobby_mutator_have(lobby, slot, static_cast<uint8_t>(position));
            if (have != mp::MutatorHave::missing && have != mp::MutatorHave::other_release)
                continue;
            mp::PlayerLack lack;
            lack.player = name_of(player.name, sizeof player.name);
            lack.name = names[position];
            return lack;
        }
    }
    return std::nullopt;
}

/// Places the host's block above Select Map.
///
/// @param runtime the runtime
/// @param placed_on what the screen is placed on
/// @return the place; not ok when the room or MAP is absent
Geometry
battle_geometry(Runtime& runtime, const LayerView& placed_on, const kit::HoverCardLook* card) {
    Geometry laid;
    laid.battle = true;
    const kit::Fonts* fonts = MutatorButtonAccess::screen_fonts(runtime);
    if (fonts == nullptr || placed_on.picture_size.x <= 0 || !room_host_shows(runtime))
        return laid;
    const std::vector<mp::BattleRoomControl> controls = mp::multiplayer_battle_room_controls();
    std::vector<place::PlaceRect> others;
    std::vector<place::PlaceRect> reason_others;
    place::PlaceRect anchor{};
    place::PlaceRect start_rect{};
    bool found = false;
    bool have_start = false;
    int32_t anchor_frame_x = 0;
    others.reserve(controls.size());
    for (const mp::BattleRoomControl& control : controls) {
        const kit::Rect frame{control.x, control.y, control.width, control.height};
        const kit::Rect points = frame_to_points(placed_on, frame);
        if (points.width <= 0 || points.height <= 0)
            continue;
        laid.gadgets.push_back(as_place(points));
        const bool map = MutatorButtonAccess::names_equal(control.name, "MAP");
        const bool start = MutatorButtonAccess::names_equal(control.name, "START");
        if (map && !found) {
            anchor = as_place(points);
            anchor_frame_x = control.x;
            laid.anchor_index = control.index;
            found = true;
        } else {
            others.push_back(as_place(points));
        }
        if (start && !have_start) {
            start_rect = as_place(points);
            have_start = true;
        } else {
            reason_others.push_back(as_place(points));
        }
        if (control.focus_place >= 0) {
            FocusRecord record;
            record.index = control.index;
            record.at = as_place(points);
            record.frame_x = control.x;
            record.frame_y = control.y;
            record.focus_place = control.focus_place;
            laid.focusable.push_back(record);
        }
    }
    if (!found)
        return laid;
    const kit::Metrics& metrics = kit::compact_metrics;
    const kit::Rect room_frame{0, 0, placed_on.picture_size.x, placed_on.picture_size.y};
    const kit::Rect room = frame_to_points(placed_on, room_frame);
    // As large as Select Map, over it, its edges Select Map's. Where that
    // meets a control, MORE MAPS…'s rule at the button's own size.
    if (const std::optional<place::PlaceRect> over =
            place::place_over(anchor, others, as_place(room), least_gap)) {
        laid.button_points = *over;
    } else {
        place::BlockSizes sizes{
            natural_width(*fonts, metrics.button_height, caption()),
            metrics.button_height,
            metrics.small_line,
            0,
            0,
            anchor_gap,
            0,
            0
        };
        laid.button_points = mp::place_over_anchor(anchor, others, as_place(room), sizes).button;
    }
    laid.button = as_kit(laid.button_points);
    kit::Rect bounds = outset(laid.button, metrics.focus_inset);
    laid.before = record_before(laid.focusable, laid.button_points);
    int32_t before_place = -1;
    for (const FocusRecord& record : laid.focusable) {
        if (record.index == laid.before)
            before_place = record.focus_place;
    }
    if (before_place >= 0) {
        for (const FocusRecord& record : laid.focusable) {
            if (record.focus_place == before_place + 1)
                laid.after = record.index;
        }
    }
    int32_t earliest = 0;
    int32_t latest = -1;
    for (const FocusRecord& record : laid.focusable) {
        if (laid.first < 0 || record.focus_place < earliest) {
            earliest = record.focus_place;
            laid.first = record.index;
        }
        if (record.focus_place > latest) {
            latest = record.focus_place;
            laid.last = record.index;
        }
    }
    laid.below = column_below(laid.focusable, anchor_frame_x, laid.button_points);
    if (have_start) {
        if (const std::optional<mp::PlayerLack> lack = first_room_lack(runtime)) {
            const place::PlaceRect box =
                mp::reason_box(start_rect, reason_others, metrics.small_line, reason_line_width);
            if (box.width > 0 && box.height > 0) {
                laid.reason = as_kit(box);
                laid.reason_text = mp::start_reason(*lack);
                bounds = unite(bounds, *laid.reason);
            }
        }
    }
    mp::Lobby& lobby = mp::multiplayer_lobby();
    laid.locked = lobby.game != nullptr && mp::lobby_options_locked(lobby);
    laid.bounds = bounds;
    add_frame(laid, placed_on, card, *fonts);
    for (place::PlaceRect& gadget : laid.gadgets) {
        gadget.x -= laid.bounds.x;
        gadget.y -= laid.bounds.y;
    }
    laid.ok = bounds.width > 0 && bounds.height > 0;
    return laid;
}

/// Places a button at the head of an anchor's column: as large as the
/// gadget under the anchor, its left edge, and over the anchor's label (a
/// rectangle just over the anchor, nearer it than that gadget is tall),
/// centred down the space between the last rectangle over the label and
/// the label.
///
/// @param anchor the gadget whose column it heads
/// @param others every other rectangle, the gadget under the anchor and the label among them
/// @param room the frame the button must stay inside
/// @return the button, or nothing when no gadget is under the anchor, or the
///     space does not hold the button with the gap, or it meets a rectangle
///     or leaves the room
std::optional<place::PlaceRect> place_at_column_head(
    const place::PlaceRect& anchor,
    std::span<const place::PlaceRect> others,
    const place::PlaceRect& room
) {
    const std::optional<place::PlaceRect> model = place::next_below(anchor, others);
    if (!model)
        return std::nullopt;
    int32_t head = anchor.y;
    for (const place::PlaceRect& other : others) {
        const bool shares = other.x < anchor.x + anchor.width && anchor.x < other.x + other.width;
        const int32_t bottom = other.y + other.height;
        if (shares && other.width > 0 && other.height > 0 && bottom <= anchor.y &&
            anchor.y - bottom < model->height)
            head = std::min(head, other.y);
    }
    return place::place_over(
        {model->x, head, model->width, model->height}, others, room, least_gap
    );
}

Geometry geometry_of(
    Runtime& runtime, const LayerView& placed_on, const kit::HoverCardLook* card = nullptr
) {
    if (room_host_shows(runtime))
        return battle_geometry(runtime, placed_on, card);
    Geometry laid;
    const kit::Fonts* fonts = MutatorButtonAccess::screen_fonts(runtime);
    if (fonts == nullptr || !button_shows(runtime))
        return laid;
    const auto& gadgets = MutatorButtonAccess::resources(runtime).layout.gadgets;
    if (gadgets.size() < 2 || placed_on.picture_size.x <= 0)
        return laid;
    const auto origin = runtime.panel_origin();
    std::vector<place::PlaceRect> others;
    std::vector<place::PlaceRect> everything;
    place::PlaceRect anchor{};
    bool found = false;
    int32_t anchor_frame_x = 0;
    std::optional<place::PlaceRect> arm;
    std::optional<place::PlaceRect> core;
    const Screen screen = MutatorButtonAccess::screen(runtime);
    const bool campaign = screen == Screen::new_campaign || screen == Screen::any_mission;
    others.reserve(gadgets.size());
    everything.reserve(gadgets.size());
    for (std::size_t index = 1; index < gadgets.size(); ++index) {
        const gui::CommonFields& common = gadgets[index].common;
        if (common.width <= 0 || common.height <= 0)
            continue;
        const kit::Rect frame{
            static_cast<int32_t>(origin.x) + common.x,
            static_cast<int32_t>(origin.y) + common.y,
            static_cast<int32_t>(common.width),
            static_cast<int32_t>(common.height)
        };
        const kit::Rect points = frame_to_points(placed_on, frame);
        if (points.width <= 0 || points.height <= 0)
            continue;
        laid.gadgets.push_back(as_place(points));
        everything.push_back(as_place(points));
        if (campaign && !arm && MutatorButtonAccess::names_equal(common.name, "Arm"))
            arm = as_place(points);
        if (campaign && !core && MutatorButtonAccess::names_equal(common.name, "Core"))
            core = as_place(points);
        if (!found && MutatorButtonAccess::names_equal(common.name, "Difficulty")) {
            anchor = as_place(points);
            anchor_frame_x = frame.x;
            laid.anchor_index = static_cast<int32_t>(index);
            found = true;
        } else {
            others.push_back(as_place(points));
        }
        if (takes_focus(gadgets[index])) {
            FocusRecord record;
            record.index = static_cast<int32_t>(index);
            record.at = as_place(points);
            record.frame_x = frame.x;
            record.frame_y = frame.y;
            laid.focusable.push_back(record);
        }
    }
    if (!found)
        return laid;
    const kit::Metrics& metrics = kit::compact_metrics;
    const kit::Rect room_frame{0, 0, placed_on.picture_size.x, placed_on.picture_size.y};
    const kit::Rect room = frame_to_points(placed_on, room_frame);
    // NEWGAME and Any Mission: as large as the gadget under Difficulty
    // (Previous Menu), at the head of Difficulty's column, over Set
    // Difficulty; the place between Difficulty and Previous Menu is the
    // campaign's computer player's. Where that meets a gadget, as large as
    // the SELECT SIDE middle of the Arm | Core bar, under it. The skirmish
    // setup, or when neither is clear: as large as the gadget under
    // Difficulty, in Difficulty's column between them. When none is clear,
    // MORE MAPS…'s rule at the button's own size.
    std::optional<place::PlaceRect> chosen;
    if (campaign)
        chosen = place_at_column_head(anchor, others, as_place(room));
    if (!chosen && arm && core) {
        const bool arm_left = arm->x <= core->x;
        chosen = place::place_under_middle(
            arm_left ? *arm : *core, arm_left ? *core : *arm, everything, as_place(room)
        );
    }
    if (!chosen)
        chosen = place::place_in_column(anchor, others, as_place(room), least_gap);
    if (!chosen) {
        place::BlockSizes sizes{
            natural_width(*fonts, metrics.button_height, caption()),
            metrics.button_height,
            metrics.small_line,
            0,
            0,
            anchor_gap,
            0,
            0
        };
        chosen = place::place_block(anchor, others, as_place(room), sizes).button;
    }
    laid.button_points = *chosen;
    laid.button = as_kit(laid.button_points);
    laid.bounds = outset(laid.button, metrics.focus_inset);
    add_frame(laid, placed_on, card, *fonts);
    const kit::Rect bounds = laid.bounds;
    for (place::PlaceRect& gadget : laid.gadgets) {
        gadget.x -= laid.bounds.x;
        gadget.y -= laid.bounds.y;
    }
    laid.before = record_before(laid.focusable, laid.button_points);
    laid.after = record_after(laid.focusable, laid.button_points);
    laid.first = first_record(laid.focusable);
    laid.last = last_record(laid.focusable);
    laid.below = column_below(laid.focusable, anchor_frame_x, laid.button_points);
    laid.ok = bounds.width > 0 && bounds.height > 0;
    return laid;
}

} // namespace

kit::HoverCardLook mutators_card(std::span<const MutatorsCardItem> items, bool host) {
    kit::HoverCardLook look;
    look.arrow = kit::Side::none;
    if (items.empty()) {
        look.title = shown(word_card_off);
    } else {
        look.title = shown(items.size() == 1 ? word_card_one : word_card_count);
        constexpr std::string_view count_holder = "{count}";
        if (const std::size_t at = look.title.find(count_holder); at != std::string::npos)
            look.title.replace(at, count_holder.size(), std::to_string(items.size()));
    }
    const std::size_t named = std::min(items.size(), mutators_card_rows);
    for (std::size_t at = 0; at < named; ++at) {
        kit::HoverCardRow row;
        row.label = std::to_string(at + 1);
        row.value = items[at].name;
        if (items[at].lacking) {
            row.value += ": " + shown(word_card_lacking);
            row.value_colour = kit::colour::lock;
        }
        look.rows.push_back(std::move(row));
    }
    if (items.size() > named) {
        kit::HoverCardRow more;
        more.value = shown(word_card_more);
        constexpr std::string_view count_holder = "{count}";
        if (const std::size_t at = more.value.find(count_holder); at != std::string::npos)
            more.value.replace(at, count_holder.size(), std::to_string(items.size() - named));
        look.rows.push_back(std::move(more));
    }
    if (!items.empty())
        look.block.push_back(shown(word_card_order));
    look.block.push_back(shown(host ? word_card_change : word_card_read));
    return look;
}

namespace {

/// A Mutators button's look, its words aside: its place, the room kept for
/// the stamp, and words that may be drawn narrower.
///
/// @param frame the button, in frame pixels
/// @return the look
room::RoomButtonLook switch_look(const room::SourceRect& frame) {
    room::RoomButtonLook look;
    look.rect = frame;
    const room::SourceRect stamp = room::room_stamp_rect(frame);
    look.inset_left = stamp.x - frame.x + stamp.width + 2;
    look.narrower_font = true;
    return look;
}

} // namespace

const room::RgbaPicture& room_stamp_icon() {
    static const WindowIcon kept = [] {
        WindowIcon decoded;
        std::string error;
        if (!decode_window_icon(window_icon_png(), decoded, error))
            return WindowIcon{};
        return visible_part(decoded);
    }();
    static const room::RgbaPicture picture{kept.width, kept.height, kept.pixels};
    return picture;
}

void draw_mutators_switch(
    room::RoomCanvas& parts,
    const room::RoomArt& art,
    const room::SourceRect& frame,
    room::RoomLamp lamp,
    room::ButtonCondition condition,
    bool focused
) {
    const std::string bytes = room::room_text(caption());
    room::RoomButtonLook look = switch_look(frame);
    look.caption = bytes;
    look.condition = condition;
    room::draw_room_switch(parts, art, look, lamp);
    room::draw_room_stamp(parts, art, room::room_stamp_rect(frame), room_stamp_icon());
    if (focused)
        room::draw_room_focus(parts, art, frame);
}

room::RoomLamp mutators_lamp_seen(
    Runtime& runtime,
    const LayerView& seen,
    const kit::Rect& button,
    bool battle,
    const room::Surface& picture
) {
    const int32_t percent = std::max(seen.scale_percent, int32_t{100});
    const kit::Point first = frame_point(seen, kit::to_points({button.x, button.y}, percent));
    const kit::Point last = frame_point(
        seen, kit::to_points({button.x + button.width - 1, button.y + button.height - 1}, percent)
    );
    const room::SourceRect frame{first.x, first.y, last.x - first.x + 1, last.y - first.y + 1};
    const std::optional<room::ScreenResources> loaded =
        battle ? BrainSeatsAccess::load_art(runtime, battle_room_art_names()) : std::nullopt;
    room::RoomArt art;
    if (!battle)
        art = BrainSeatsAccess::screen_art(runtime);
    else if (loaded)
        art = {&*loaded, room::room_palette(*loaded)};
    const room::RoomLampParts parts = room::room_lamp_parts(art, frame);
    const kit::Rect lamp = kit::to_canvas(frame_rect_points(seen, parts.lamp), percent);
    // The lamps' edges are rounded outward: a pixel in from each is the lamps'.
    return room::room_lamp_seen(
        picture.rgb,
        static_cast<int32_t>(picture.width),
        {lamp.x + 1, lamp.y + 1, lamp.width - 2, lamp.height - 2}
    );
}

namespace {

/// The button's word as draw_mutators_switch draws it, in UTF-8: whole
/// where it fits, else cut and ended with "…".
///
/// @param art the room's art
/// @param frame the button, in frame pixels
/// @return the word
std::string caption_shown(const room::RoomArt& art, const room::SourceRect& frame) {
    const std::string words = caption();
    const std::string bytes = room::room_text(words);
    const std::string drawn = room::room_caption_shown(
        art, bytes, room::room_switch_caption_width(art, switch_look(frame)), true
    );
    if (drawn.empty() || drawn == bytes)
        return words;
    std::string text = oa::present::decode_game_text(drawn, oa::present::game_text_settings().utf8);
    constexpr std::string_view cut = "...";
    if (text.size() >= cut.size() && text.compare(text.size() - cut.size(), cut.size(), cut) == 0)
        text.replace(text.size() - cut.size(), cut.size(), "\u2026");
    return text;
}

} // namespace

std::string mutators_automation_text(
    const room::RoomArt& art, const room::SourceRect& frame, std::size_t count
) {
    std::string text = count_text(count);
    const std::string word = caption();
    const std::string drawn = caption_shown(art, frame);
    if (drawn != word && text.compare(0, word.size(), word) == 0)
        text.replace(0, word.size(), drawn);
    return text;
}

/// Mutators as a screen of the OA layer: not modal, no backdrop, not opaque,
/// so the picture shows round the button. It takes the button, the focus handed
/// to it, and Y. Up and Down on NEWGAME and Any Mission go on to the lists.
struct Runtime::MutatorButtonScreen final : LayerScreen {
    /// Makes the button.
    ///
    /// @param runtime the runtime
    explicit MutatorButtonScreen(Runtime& runtime) : runtime_(runtime) {}

    /// Returns "mutators-button".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return screen_name; }

    /// Returns the block and the focus ring's margin, in the window.
    ///
    /// @param placed_on what the screen is placed on
    /// @return its place; empty while it does not show
    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        const Geometry laid = laid_out(placed_on);
        if (!laid.ok)
            return {};
        const int32_t percent = std::max(placed_on.scale_percent, int32_t{100});
        const kit::Rect shown_at = kit::to_canvas(laid.bounds, percent);
        return {
            {shown_at.x, shown_at.y, shown_at.width, shown_at.height},
            laid.bounds.width,
            laid.bounds.height,
            percent
        };
    }

    /// Tells that what the button does not take goes on.
    ///
    /// @return false
    [[nodiscard]] bool modal() const override { return false; }

    /// Tells that the button darkens nothing.
    ///
    /// @return false
    [[nodiscard]] bool backdrop() const override { return false; }

    /// Tells that the line lies over the picture.
    ///
    /// @return false
    [[nodiscard]] bool opaque() const override { return false; }

    /// Follows the view the button is placed on.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override { view_ = placed_on; }

    /// Draws the button in the room's art, with the OA stamp, its lamp and,
    /// while it holds the focus, the room's focus marker; and the waiting
    /// line beside START in the room's label font.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override {
        const Geometry laid = laid_out(view_);
        if (!laid.ok)
            return;
        const room::RoomArt art = room_art(laid.battle);
        const room::Surface& frame = BrainSeatsAccess::frame(runtime_);
        room::RoomCanvas parts = room::room_canvas(
            {laid.frame.x - room_focus_margin,
             laid.frame.y - room_focus_margin,
             laid.frame.width + 2 * room_focus_margin,
             laid.frame.height + 2 * room_focus_margin},
            &frame
        );
        room::ButtonCondition condition = room::ButtonCondition::normal;
        if (laid.locked)
            condition = room::ButtonCondition::disabled;
        else if (pressed_ && hovered_)
            condition = room::ButtonCondition::pressed;
        draw_mutators_switch(parts, art, laid.frame, lamp_shown(laid), condition, focused_);
        paint_room_parts(canvas, view_, laid.bounds, parts);
        if (laid.reason) {
            const room::SourceRect line = points_rect_frame(view_, *laid.reason);
            room::RoomCanvas reason = room::room_canvas(line, &frame);
            const std::string text = room::room_text(laid.reason_text);
            const std::string fitted =
                room::fit_room_text(art, room::RoomFont::label, text, line.width);
            const int32_t width = room::room_text_width(art, room::RoomFont::label, fitted);
            const int32_t height = room::room_line_height(art, room::RoomFont::label);
            std::ignore = room::draw_room_text(
                reason,
                art,
                room::RoomFont::label,
                fitted,
                line.x + line.width - width,
                line.y + (line.height - height) / 2,
                line.width
            );
            paint_room_parts(canvas, view_, laid.bounds, reason);
        }
        if (laid.card) {
            kit::DisplayList card;
            kit::add_hover_card(card, local_of(*laid.card, laid.bounds), laid.card_look);
            kit::paint(canvas, card);
        }
    }

    /// Takes a press on the button. A press anywhere else clears the focus and goes on.
    ///
    /// A mouse presses the drawn rectangle. A finger presses by its reach
    /// when no gadget of the screen under the button, the anchor included,
    /// is nearer the point. A point inside a gadget stays that gadget's.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the screen's points
    /// @param finger_reach a finger's reach; 0 for a mouse
    /// @return redraw for the button's own press; pass otherwise
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        const Geometry laid = laid_out(view_);
        const bool inside = laid.ok && kit::contains(local_of(laid.button, laid.bounds), at);
        if (inside != hovered_) {
            hovered_ = inside;
            ++revision_;
        }
        if (laid.ok) {
            pointer_layer_ = {at.x + laid.bounds.x, at.y + laid.bounds.y};
            have_pointer_ = true;
            const int32_t card_before = card_.open_slot();
            follow_card(laid);
            if (card_.open_slot() != card_before)
                ++revision_;
        }
        const bool over = kind != ScreenInputKind::pointer_move && finger_reach > 0
                              ? finger_reaches_button(laid, at, finger_reach)
                              : inside;
        if (kind == ScreenInputKind::pointer_down && button == SDL_BUTTON_LEFT) {
            if (over) {
                card_.close();
                // A locked button takes the press and opens nothing.
                if (!laid.locked) {
                    pressed_ = true;
                    ++revision_;
                }
                return LayerAnswer::redraw;
            }
            pressed_ = false;
            if (focused_) {
                focused_ = false;
                ++revision_;
            }
            return LayerAnswer::pass;
        }
        if (kind == ScreenInputKind::pointer_up && button == SDL_BUTTON_LEFT && laid.locked &&
            over) {
            pressed_ = false;
            return LayerAnswer::redraw;
        }
        if (kind == ScreenInputKind::pointer_up && button == SDL_BUTTON_LEFT && pressed_) {
            pressed_ = false;
            ++revision_;
            if (over && !laid.locked)
                open_picker();
            return LayerAnswer::redraw;
        }
        return LayerAnswer::pass;
    }

    /// Takes the keys the button holds the focus for, the information key,
    /// which shows the card, and Escape while the card is open. Up and Down
    /// on the campaign screens go on.
    ///
    /// @param pressed the key
    /// @return none or redraw when the button takes it; pass otherwise
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        if (pressed == kit::Key::info && focused_ && card_.key_info(card_slot)) {
            ++revision_;
            return LayerAnswer::none;
        }
        if (pressed == kit::Key::escape && card_.key_close()) {
            ++revision_;
            return LayerAnswer::none;
        }
        return answer_key(pressed);
    }

    /// Lets every shortcut go on.
    ///
    /// @return pass
    LayerAnswer shortcut(uint32_t /*sdl_key*/, uint16_t /*modifiers*/) override {
        return LayerAnswer::pass;
    }

    /// Takes Y, and the focus keys, on the same terms as the keyboard.
    ///
    /// @param action the gamepad action
    /// @return none or redraw when the button takes it; pass otherwise
    LayerAnswer pad_action(pad::Action action) override {
        if (action == pad::Action::library_search) {
            const Geometry laid = laid_out(view_);
            if (!laid.ok)
                return LayerAnswer::pass;
            if (!laid.locked)
                open_picker();
            return LayerAnswer::none;
        }
        if (action == pad::Action::press)
            return answer_key(kit::Key::enter);
        if (action == pad::Action::focus_next)
            return answer_key(kit::Key::tab);
        if (action == pad::Action::focus_previous)
            return answer_key(kit::Key::back_tab);
        if (action == pad::Action::focus_up)
            return answer_key(kit::Key::up);
        if (action == pad::Action::focus_down)
            return answer_key(kit::Key::down);
        if (action == pad::Action::focus_left)
            return answer_key(kit::Key::left);
        if (action == pad::Action::focus_right)
            return answer_key(kit::Key::right);
        return LayerAnswer::pass;
    }

    /// Lets the wheel go on.
    ///
    /// @return pass
    LayerAnswer wheel(kit::Point /*at*/, float /*notches*/) override { return LayerAnswer::pass; }

    /// Returns the button and the waiting line beside START, for
    /// automation and the pointer.
    ///
    /// The button's automation name is mutators, which the endpoint lists as
    /// oa.mutators, and its text counts the set chosen: "Mutators · off", or
    /// "Mutators · 4 on" (mutators_automation_text); it is checked while its
    /// lamp is lit green, not while it is red or blinks. The waiting line is
    /// mutators.waiting, a label. The open card's lines are
    /// battleroom.tip.line.<n>. Tab's order holds the button.
    ///
    /// @return the display list
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list;
        const Geometry laid = laid_out(view_);
        if (!laid.ok)
            return list;
        kit::Control control;
        control.id = button_id;
        control.rect = local_of(laid.button, laid.bounds);
        control.enabled = !laid.locked;
        control.name = std::string(button_name);
        control.kind = kit::ControlKind::button;
        control.text = mutators_automation_text(room_art(laid.battle), laid.frame, chosen_count());
        control.checked = lamp(laid) == room::RoomLamp::green;
        list.controls.push_back(std::move(control));
        list.tab_order.push_back(button_id);
        if (laid.reason) {
            kit::Control line;
            line.id = reason_id;
            line.rect = local_of(*laid.reason, laid.bounds);
            line.enabled = false;
            line.name = std::string(reason_name);
            line.kind = kit::ControlKind::area;
            line.focusable = false;
            line.text = shown_reason(laid);
            list.controls.push_back(std::move(line));
        }
        if (laid.card) {
            const kit::Rect card = local_of(*laid.card, laid.bounds);
            kit::add_hover_card(list, card, laid.card_look);
            mp::presence_card_controls(list, card, laid.card_look);
        }
        return list;
    }

    /// Returns the button's hover, press and focus.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override {
        kit::Interaction shown_state;
        shown_state.hovered = hovered_ ? button_id : kit::no_control;
        shown_state.pressed = pressed_ ? button_id : kit::no_control;
        shown_state.focused = focused_ ? button_id : kit::no_control;
        shown_state.focus_shown = focused_;
        return shown_state;
    }

    /// Closes once the setup is gone, a match has started, or a dialog is up.
    /// Gives up the focus once the screen's own focus is on a record again,
    /// so the two focus markers never show at once. Follows the words, the
    /// lamp and the place.
    ///
    /// @return close when it should leave; otherwise none
    LayerAnswer tick() override {
        const bool battle = room_host_shows(runtime_);
        if (!button_shows(runtime_) && !battle)
            return LayerAnswer::close;
        if (focused_) {
            const bool room_focus = battle && mp::multiplayer_battle_room_focus() > 0;
            const bool gadget_focus = !battle && runtime_.frontend_focus() >= 0;
            if (room_focus || gadget_focus) {
                focused_ = false;
                ++revision_;
            }
        }
        // The words, the lamp and the place are drawn from the room's art,
        // so a change of any of them is a new picture, and so is each turn
        // of a blinking lamp; a steady lamp asks for none.
        const int32_t card_before = card_.open_slot();
        follow_card(laid_out(view_));
        if (card_.open_slot() != card_before)
            ++revision_;
        const Geometry laid = laid_out(view_);
        std::string now = count_text(chosen_count());
        now += '/';
        now += std::to_string(static_cast<int32_t>(lamp_shown(laid)));
        now += laid.locked ? "l" : "-";
        now += laid.reason_text;
        now += std::to_string(laid.frame.x) + ',' + std::to_string(laid.frame.y) + ',' +
               std::to_string(laid.frame.width) + ',' + std::to_string(laid.frame.height);
        if (now != seal_) {
            seal_ = std::move(now);
            ++revision_;
        }
        return LayerAnswer::none;
    }

    /// Returns the button's revision.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Has nothing of its own to close.
    void close(bool /*by_key*/) override {}

  private:

    /// Tells whether a finger's reach lands on the button.
    ///
    /// A gadget nearer the point, or one the point lies inside, keeps it.
    /// A locked button is still the one the finger lands on: the press is
    /// taken and opens nothing. The hover stays the drawn rectangle.
    ///
    /// @param laid where the button and the gadgets lie
    /// @param at the finger, in the screen's points
    /// @param finger_reach how far the finger reaches, in points
    /// @return true when the finger presses the button
    [[nodiscard]] bool
    finger_reaches_button(const Geometry& laid, kit::Point at, int32_t finger_reach) const {
        if (!laid.ok || finger_reach <= 0)
            return false;
        const kit::Rect button = local_of(laid.button, laid.bounds);
        if (button.width <= 0 || button.height <= 0)
            return false;
        const place::PlaceRect button_place{button.x, button.y, button.width, button.height};
        if (place::nearer_gadget(at.x, at.y, button_place, laid.gadgets))
            return false;
        kit::DisplayList list = display_list();
        for (kit::Control& control : list.controls)
            if (control.id == button_id)
                control.enabled = true;
        kit::Interaction trial = interaction();
        return kit::finger_down(trial, list, at, finger_reach).control == button_id;
    }

    /// Tells whether the campaign's lists take Up and Down.
    ///
    /// @return true on NEWGAME and Any Mission
    [[nodiscard]] bool campaign() const noexcept {
        return runtime_.screen_ == Screen::new_campaign || runtime_.screen_ == Screen::any_mission;
    }

    /// Takes the focus onto the button and clears the screen's own ring.
    ///
    /// @param battle true in the battle room, where the panel's focus is cleared
    /// @return redraw
    LayerAnswer take_focus(bool battle) {
        focused_ = true;
        pressed_ = false;
        if (battle)
            mp::multiplayer_set_battle_room_focus(mp::kNoControl);
        else
            runtime_.set_frontend_focus(-1);
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Hands the focus to a record and clears the button's ring.
    ///
    /// @param index the record
    /// @param battle true in the battle room, where the panel's focus is set
    /// @return redraw
    LayerAnswer hand_focus(int32_t index, bool battle) {
        focused_ = false;
        if (battle)
            mp::multiplayer_set_battle_room_focus(index);
        else
            runtime_.set_frontend_focus(index);
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Answers one key, for the keyboard and for the pad actions that become keys.
    ///
    /// @param pressed the key
    /// @return none or redraw when the button takes it; pass otherwise
    LayerAnswer answer_key(kit::Key pressed) {
        const Geometry laid = laid_out(view_);
        if (!laid.ok)
            return LayerAnswer::pass;
        const int32_t current =
            laid.battle ? mp::multiplayer_battle_room_focus() : runtime_.frontend_focus();
        if (!focused_) {
            if (pressed == kit::Key::tab && current == laid.before && laid.before > 0)
                return take_focus(laid.battle);
            if (pressed == kit::Key::back_tab && current == laid.after && laid.after > 0)
                return take_focus(laid.battle);
            if (!campaign() && pressed == kit::Key::down && current == laid.anchor_index &&
                laid.anchor_index > 0)
                return take_focus(laid.battle);
            if (!campaign() && pressed == kit::Key::up && current == laid.below && laid.below > 0)
                return take_focus(laid.battle);
            return LayerAnswer::pass;
        }
        if (pressed == kit::Key::enter || pressed == kit::Key::space) {
            if (!laid.locked)
                open_picker();
            return LayerAnswer::none;
        }
        if (pressed == kit::Key::tab) {
            const int32_t next = laid.after > 0 ? laid.after : laid.first;
            return next > 0 ? hand_focus(next, laid.battle) : LayerAnswer::none;
        }
        if (pressed == kit::Key::back_tab) {
            const int32_t next = laid.before > 0 ? laid.before : laid.last;
            return next > 0 ? hand_focus(next, laid.battle) : LayerAnswer::none;
        }
        if (campaign() && (pressed == kit::Key::up || pressed == kit::Key::down))
            return LayerAnswer::pass;
        if (campaign() && (pressed == kit::Key::left || pressed == kit::Key::right))
            return LayerAnswer::none;
        if (!campaign() && (pressed == kit::Key::up || pressed == kit::Key::down ||
                            pressed == kit::Key::left || pressed == kit::Key::right)) {
            Aim aim = Aim::up;
            if (pressed == kit::Key::down)
                aim = Aim::down;
            else if (pressed == kit::Key::left)
                aim = Aim::left;
            else if (pressed == kit::Key::right)
                aim = Aim::right;
            const int32_t next = nearest_record(laid.focusable, laid.button_points, aim);
            return next > 0 ? hand_focus(next, laid.battle) : LayerAnswer::none;
        }
        return LayerAnswer::pass;
    }

    /// The set the setup that is showing will start with.
    ///
    /// @return the campaign's set on NEWGAME and Any Mission, otherwise the skirmish's
    [[nodiscard]] const std::vector<picker::Chosen>& chosen_set() const {
        if (campaign())
            return runtime_.campaign_mutators_;
        return runtime_.skirmish_mutators_;
    }

    /// Opens the battle room's picker on the room's list, or the remembered
    /// multiplayer set. USE THESE hands the set to the room. CANCEL changes nothing.
    void open_room_picker() {
        mp::Lobby& lobby = mp::multiplayer_lobby();
        if (lobby.game != nullptr && mp::lobby_options_locked(lobby))
            return;
        const auto held = runtime_.room_mutators();
        std::vector<oa::data::mutators::ResolvedMutator> current(held.begin(), held.end());
        MutatorPickerRequest request;
        request.setup = picker::Setup::multiplayer;
        if (!current.empty()) {
            request.set = chosen_from_resolved(current);
            request.remembered = false;
        } else if (
            const std::optional<oa::data::mutators::Loadout> remembered =
                runtime_.remembered_mutators(mutators_memory::Setup::multiplayer)
        ) {
            request.set = chosen_from_loadout(*remembered);
            request.remembered = !request.set.empty();
        }
        Runtime* runtime = &runtime_;
        request.used = [runtime](std::vector<picker::Chosen> set) {
            runtime->apply_room_mutators(std::move(set));
        };
        request.cancelled = [] {};
        runtime_.open_mutator_picker(std::move(request));
    }

    /// Opens the picker on the setup's chosen set, or on the set it remembered.
    void open_picker() {
        if (room_host_shows(runtime_)) {
            open_room_picker();
            return;
        }
        const mutators_memory::Setup setup = memory_setup(runtime_);
        const picker::Setup which = picker_setup(runtime_);
        const std::vector<picker::Chosen>& chosen = chosen_set();
        MutatorPickerRequest request;
        request.setup = which;
        if (!chosen.empty()) {
            request.set = chosen;
            request.remembered = false;
        } else if (
            const std::optional<oa::data::mutators::Loadout> remembered =
                runtime_.remembered_mutators(setup)
        ) {
            request.set = chosen_from_loadout(*remembered);
            request.remembered = !request.set.empty();
        }
        Runtime* runtime = &runtime_;
        request.used = [runtime, setup](std::vector<picker::Chosen> set) {
            runtime->replace_setup_mutators(setup, std::move(set));
        };
        request.cancelled = [] {};
        runtime_.open_mutator_picker(std::move(request));
    }

    /// How many mutators the button counts: the room's list in the battle
    /// room, otherwise the set the setup that is showing will start with.
    ///
    /// @return the count
    [[nodiscard]] std::size_t chosen_count() const {
        if (room_host_shows(runtime_))
            return runtime_.room_mutators().size();
        return chosen_set().size();
    }

    /// The geometry for a view, the card placed while it is open.
    ///
    /// @param placed_on what the screen is placed on
    /// @return the geometry
    [[nodiscard]] Geometry laid_out(const LayerView& placed_on) const {
        if (card_.open_slot() != card_slot)
            return geometry_of(runtime_, placed_on);
        const std::vector<MutatorsCardItem> items = card_items();
        const kit::HoverCardLook look = mutators_card(items, true);
        return geometry_of(runtime_, placed_on, &look);
    }

    /// The mutators the card names, in the order they apply: the room's list
    /// in the battle room, otherwise the setup's set, each named from its
    /// installed manifest.
    ///
    /// @return the mutators
    [[nodiscard]] std::vector<MutatorsCardItem> card_items() const {
        std::vector<MutatorsCardItem> items;
        if (room_host_shows(runtime_)) {
            for (const oa::data::mutators::ResolvedMutator& one : runtime_.room_mutators())
                items.push_back(
                    {one.manifest.name.empty() ? one.manifest.id : one.manifest.name, false}
                );
            return items;
        }
        const std::vector<picker::Chosen>& chosen = chosen_set();
        std::string key;
        for (const picker::Chosen& one : chosen)
            key += one.id + '\n';
        if (key != names_key_) {
            // The Mutators folder is read once a set, not on every frame.
            names_key_ = key;
            names_.clear();
            const oa::data::mutator_library::Listing listing =
                oa::data::mutator_library::list_installed(
                    runtime_.user_folder_ /
                    std::string(oa::data::mutator_library::mutators_folder_name)
                );
            for (const picker::Chosen& one : chosen) {
                const oa::data::mutator_library::InstalledMutator* installed =
                    oa::data::mutator_library::find(listing, one.id);
                names_.push_back(
                    installed != nullptr && !installed->manifest.name.empty()
                        ? installed->manifest.name
                        : one.id
                );
            }
        }
        for (const std::string& name : names_)
            items.push_back({name, false});
        return items;
    }

    /// Follows the pointer for the card: resting on the button opens it,
    /// and it stays while the pointer is on the button or the card.
    ///
    /// @param laid the geometry
    void follow_card(const Geometry& laid) {
        bool over_button = false;
        bool over_card = false;
        if (laid.ok && have_pointer_) {
            const kit::Point local{
                pointer_layer_.x - laid.bounds.x, pointer_layer_.y - laid.bounds.y
            };
            over_button = kit::contains(local_of(laid.button, laid.bounds), local);
            over_card = (laid.card && kit::contains(local_of(*laid.card, laid.bounds), local)) ||
                        (laid.arrow && kit::contains(local_of(*laid.arrow, laid.bounds), local));
        }
        card_.over_card(over_card);
        (void)card_.pointer(
            over_button ? card_slot : mp::presence_card_no_slot,
            static_cast<uint64_t>(SDL_GetTicks())
        );
    }

    /// The waiting line as the room draws it, in UTF-8: whole where it fits,
    /// else cut as the room cuts it.
    ///
    /// @param laid the geometry, its line placed
    /// @return the line
    [[nodiscard]] std::string shown_reason(const Geometry& laid) const {
        if (!laid.reason)
            return {};
        const room::RoomArt art = room_art(laid.battle);
        const std::string text = room::room_text(laid.reason_text);
        const std::string fitted = room::fit_room_text(
            art, room::RoomFont::label, text, points_rect_frame(view_, *laid.reason).width
        );
        if (fitted == text)
            return laid.reason_text;
        return oa::present::decode_game_text(fitted, oa::present::game_text_settings().utf8);
    }

    /// The button's lamp, as 3.1c's on and off buttons light theirs: red
    /// while no mutator is on; in the battle room, blinking red while a
    /// seated player lacks one; otherwise green.
    ///
    /// @param laid the geometry
    /// @return the lamp
    [[nodiscard]] room::RoomLamp lamp(const Geometry& laid) const {
        if (chosen_count() == 0)
            return room::RoomLamp::red;
        if (laid.battle) {
            mp::Lobby& lobby = mp::multiplayer_lobby();
            if (lobby.game != nullptr &&
                mp::lobby_mutators_block(lobby).hold != mp::MutatorsHold::none)
                return room::RoomLamp::red_blinking;
        }
        return room::RoomLamp::green;
    }

    /// The lamp drawn now: a blinking lamp is red or unlit by the clock.
    ///
    /// @param laid the geometry
    /// @return the lamp
    [[nodiscard]] room::RoomLamp lamp_shown(const Geometry& laid) const {
        return room::room_lamp_shown(lamp(laid), static_cast<uint64_t>(SDL_GetTicks()));
    }

    /// The art the button is drawn with: the setup's own, or the battle
    /// room's, which is read once while the screen lives.
    ///
    /// @param battle true in the battle room
    /// @return the art
    [[nodiscard]] room::RoomArt room_art(bool battle) const {
        if (!battle)
            return BrainSeatsAccess::screen_art(runtime_);
        if (!battle_art_read_) {
            battle_art_read_ = true;
            battle_art_ = BrainSeatsAccess::load_art(runtime_, battle_room_art_names());
        }
        if (!battle_art_)
            return {};
        return {&*battle_art_, room::room_palette(*battle_art_)};
    }

    Runtime& runtime_;               ///< the runtime whose setups it shows over
    LayerView view_{};               ///< the view it was last laid out for
    bool hovered_{};                 ///< the pointer is over the button
    bool pressed_{};                 ///< a press on the button is held
    bool focused_{};                 ///< the keyboard focus is on the button
    uint64_t revision_{};            ///< counts the changes to how it looks
    std::string seal_{};             ///< the words and the lamp last drawn
    mp::PresenceCardTrigger card_{}; ///< when the card opens and closes
    bool have_pointer_{};            ///< the pointer has been seen
    kit::Point pointer_layer_{};     ///< the pointer, in the layer's points
    /// The set the card's names were read for, its ids one a line.
    mutable std::string names_key_{};
    mutable std::vector<std::string> names_{}; ///< those names, in the set's order
    /// The battle room's art, read on its first draw; nothing when it could not be read.
    mutable std::optional<room::ScreenResources> battle_art_{};
    mutable bool battle_art_read_{}; ///< the battle room's art has been read
};

void Runtime::keep_mutator_button_screen() {
    OaLayer& layer = oa_layer();
    if ((!button_shows(*this) && !room_host_shows(*this)) || layer.find(screen_name) != nullptr)
        return;
    layer.push(std::make_unique<MutatorButtonScreen>(*this));
}

void Runtime::set_frontend_focus(int32_t index) {
    frontend_focus_ = index;
    if (screen_ == Screen::new_campaign || screen_ == Screen::any_mission) {
        if (index > 0 && static_cast<std::size_t>(index) < resources_.layout.gadgets.size())
            campaign_setup_focus_ =
                resources_.layout.gadgets[static_cast<std::size_t>(index)].common.name;
        else
            campaign_setup_focus_.clear();
    }
    rebuild_surface();
}

void Runtime::replace_setup_mutators(
    mutators_memory::Setup setup, std::vector<picker::Chosen> set
) {
    if (setup == mutators_memory::Setup::campaign)
        campaign_mutators_ = std::move(set);
    else if (setup == mutators_memory::Setup::skirmish)
        skirmish_mutators_ = std::move(set);
}

void Runtime::hand_chosen_mutators(mutators_memory::Setup setup) {
    cancel_loadout_once();
    const std::vector<picker::Chosen>* chosen = nullptr;
    if (setup == mutators_memory::Setup::skirmish)
        chosen = &skirmish_mutators_;
    else if (setup == mutators_memory::Setup::campaign)
        chosen = &campaign_mutators_;
    if (chosen == nullptr || chosen->empty()) {
        start_with_mutators(setup, {});
        return;
    }
    // One listing of the Mutators folder serves the whole set.
    const oa::data::mutator_library::Listing listing = oa::data::mutator_library::list_installed(
        user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name)
    );
    std::vector<oa::data::mutators::ResolvedMutator> resolved;
    resolved.reserve(chosen->size());
    for (const picker::Chosen& one : *chosen) {
        oa::data::mutators::MutatorChoice choice;
        choice.target = one.id;
        choice.options = one.options;
        std::string error;
        std::optional<oa::data::mutators::ResolvedMutator> match =
            oa::data::mutators::resolve_choice(choice, listing, &error);
        if (!match) {
            throw std::runtime_error(
                error.empty() ? "the mutators could not be read" : std::move(error)
            );
        }
        if (one.clash) {
            match->clashes = *one.clash == picker::Clash::add
                                 ? oa::formats::oamut::ClashMode::add
                                 : oa::formats::oamut::ClashMode::replace;
        }
        resolved.push_back(std::move(*match));
    }
    start_with_mutators(setup, std::move(resolved));
}

} // namespace oa::app
