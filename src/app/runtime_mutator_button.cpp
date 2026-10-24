// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Mutators on the skirmish setup, the campaign launch and the battle room's
// host. One filled kit button wearing the OA badge, its words "Mutators" or
// "Mutators (4)" for the set chosen, keeping to the left: on the skirmish
// setup as large as Previous Menu, in its column, centred between Difficulty
// and Previous Menu; on NEWGAME as large as the SELECT SIDE middle of the
// side bar, under it; in the battle room as large as Select Map, over it.
// Each place is worked out from the 3.1c gadgets on every window, and falls
// back to MORE MAPS…'s rule when it would meet one. The button joins that
// screen's Tab order. Y opens the picker. In the battle room a launch that
// locks the host's options draws the button disabled.

#include "oa_button_focus.hpp"
#include "oa_layer.hpp"
#include "touch_state.hpp"
#include "oa/app/mutators_memory.hpp"
#include "oa/app/runtime.hpp"

#include "oa/data/languages/interface_text.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_multiplayer/get_map.hpp"
#include "oa/ui/frontend_multiplayer/mutator_room.hpp"
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

    static bool pad_badges(Runtime& runtime) { return runtime.touch_state().hud.pad.badges; }

    static oa::ui::pad_controls::GlyphStyle pad_glyphs(Runtime& runtime) {
        return runtime.touch_state().hud.pad.glyphs;
    }
};

namespace {

namespace picker = oa::ui::mutator_picker;
namespace place = oa::ui::frontend_state;
namespace mp = oa::ui::frontend_multiplayer;
namespace pad = oa::ui::pad_controls;
namespace kit = oa::ui::kit;
namespace gui = oa::ui::gui_layout;

constexpr std::string_view screen_name = "mutators-button";
constexpr std::string_view button_name = "mutators";
constexpr std::string_view word_button = "Mutators";
constexpr std::string_view word_counted = "Mutators ({count})";
constexpr kit::ControlId button_id = 1;
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

/// The button's words for a set of a count: "Mutators", or "Mutators (4)".
///
/// @param count how many mutators the set holds
/// @return the words, in the language shown
std::string caption_for(std::size_t count) {
    if (count == 0)
        return shown(word_button);
    std::string text = shown(word_counted);
    constexpr std::string_view place_holder = "{count}";
    if (const std::size_t at = text.find(place_holder); at != std::string::npos)
        text.replace(at, place_holder.size(), std::to_string(count));
    return text;
}

void add_text(
    kit::DisplayList& list,
    const kit::Rect& box,
    std::string text,
    kit::Colour ink,
    kit::Align align
) {
    kit::Item item;
    item.role = kit::Role::text;
    item.rect = box;
    item.text = std::move(text);
    item.font = kit::FontRole::small;
    item.align = align;
    item.colour = ink;
    list.items.push_back(std::move(item));
}

/// Where the button and its chip lie, and which records neighbour it.
struct Geometry {
    bool ok{};                       ///< the anchor was found and the button was placed
    kit::Rect bounds{};              ///< the screen's place, in points, including the focus margin
    kit::Rect button{};              ///< the button, in points
    std::optional<kit::Rect> chip{}; ///< the Y chip, inside the button's right end, when it fits
    std::string glyph{};             ///< the chip's letters
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
};

/// The width a button needs for the OA badge and its words, where it falls
/// back to MORE MAPS…'s rule and is not sized by a gadget.
///
/// @param fonts the fonts
/// @param height the button's height
/// @param words the longest words it shows
/// @return the width, in points
int32_t natural_width(const kit::Fonts& fonts, int32_t height, std::string_view words) {
    const int32_t pad = std::max(height / 5, int32_t{2});
    const int32_t side = height - 2 * pad;
    return pad + side + pad + kit::text_width(fonts, kit::FontRole::small, words) + pad;
}

/// Puts the Y chip inside the button's right end, when the pad shows badges
/// and the button has room for it beside the badge and a few letters.
///
/// @param runtime the runtime
/// @param fonts the fonts
/// @param laid the geometry, its button placed
void place_chip(Runtime& runtime, const kit::Fonts& fonts, Geometry& laid) {
    if (!MutatorButtonAccess::pad_badges(runtime))
        return;
    const pad::GlyphSpec spec =
        pad::glyph_spec(pad::PadButton::y, MutatorButtonAccess::pad_glyphs(runtime));
    if (spec.text.empty())
        return;
    const kit::Metrics& metrics = kit::compact_metrics;
    kit::ChipLook look;
    look.text = std::string(spec.text);
    const int32_t width = kit::chip_width(fonts, look);
    const kit::ButtonMarkParts parts = kit::button_mark_parts(laid.button);
    const int32_t pad = std::max(laid.button.height / 5, int32_t{2});
    const int32_t left = laid.button.x + laid.button.width - pad - width;
    // The badge and a few letters stay clear of the chip.
    if (parts.caption.width <= 0 || left - pad < parts.caption.x + metrics.button_width / 2)
        return;
    laid.chip = kit::Rect{
        left,
        laid.button.y + (laid.button.height - metrics.chip_height) / 2,
        width,
        metrics.chip_height
    };
    laid.glyph = std::move(look.text);
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
Geometry battle_geometry(Runtime& runtime, const LayerView& placed_on) {
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
    // As large as Select Map, over it from its left edge. Where that meets a
    // control, MORE MAPS…'s rule at the button's own size.
    if (const std::optional<place::PlaceRect> over =
            place::place_over(anchor, others, as_place(room), least_gap)) {
        laid.button_points = *over;
    } else {
        place::BlockSizes sizes{
            natural_width(*fonts, metrics.button_height, caption_for(0)),
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
    place_chip(runtime, *fonts, laid);
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
    for (place::PlaceRect& gadget : laid.gadgets) {
        gadget.x -= laid.bounds.x;
        gadget.y -= laid.bounds.y;
    }
    laid.ok = bounds.width > 0 && bounds.height > 0;
    return laid;
}

Geometry geometry_of(Runtime& runtime, const LayerView& placed_on) {
    if (room_host_shows(runtime))
        return battle_geometry(runtime, placed_on);
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
    // NEWGAME: as large as the SELECT SIDE middle of the Arm | Core bar, under
    // it. Elsewhere, or when that meets a gadget, as large as the gadget under
    // Difficulty (Previous Menu), in Difficulty's column. When neither is
    // clear, MORE MAPS…'s rule at the button's own size.
    std::optional<place::PlaceRect> chosen;
    if (arm && core) {
        const bool arm_left = arm->x <= core->x;
        chosen = place::place_under_middle(
            arm_left ? *arm : *core, arm_left ? *core : *arm, everything, as_place(room)
        );
    }
    if (!chosen)
        chosen = place::place_in_column(anchor, others, as_place(room), least_gap);
    if (!chosen) {
        place::BlockSizes sizes{
            natural_width(*fonts, metrics.button_height, caption_for(0)),
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
    place_chip(runtime, *fonts, laid);
    const kit::Rect bounds = outset(laid.button, metrics.focus_inset);
    laid.bounds = bounds;
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
        const Geometry laid = geometry_of(runtime_, placed_on);
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

    /// Draws the button and, while it holds the focus, the ring.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override { kit::paint(canvas, display_list()); }

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
        const Geometry laid = geometry_of(runtime_, view_);
        const bool inside = laid.ok && kit::contains(local_of(laid.button, laid.bounds), at);
        if (inside != hovered_) {
            hovered_ = inside;
            ++revision_;
        }
        const bool over = kind != ScreenInputKind::pointer_move && finger_reach > 0
                              ? finger_reaches_button(laid, at, finger_reach)
                              : inside;
        if (kind == ScreenInputKind::pointer_down && button == SDL_BUTTON_LEFT) {
            if (over) {
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

    /// Takes the keys the button holds the focus for. Up and Down on the campaign screens go on.
    ///
    /// @param pressed the key
    /// @return none or redraw when the button takes it; pass otherwise
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override { return answer_key(pressed); }

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
            const Geometry laid = geometry_of(runtime_, view_);
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

    /// Returns the button, the waiting line beside START, the chip and the ring.
    ///
    /// The button's automation name is mutators, which the endpoint lists as
    /// oa.mutators, and its text is its words: "Mutators", or "Mutators (4)"
    /// for the set chosen. Tab's order holds the button.
    ///
    /// @return the display list
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list;
        const Geometry laid = geometry_of(runtime_, view_);
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        if (!laid.ok || fonts == nullptr)
            return list;
        const kit::Rect button = local_of(laid.button, laid.bounds);
        kit::ButtonLook look;
        look.style = kit::ButtonStyle::filled;
        look.enabled = !laid.locked;
        look.hovered = hovered_ && !laid.locked;
        look.held = pressed_ && hovered_ && !laid.locked;
        look.oa_mark = true;
        fit_caption(look, *fonts, button, laid.chip ? local_of(*laid.chip, laid.bounds).x : 0);
        kit::add_button(list, button, look, button_id, std::string(button_name));
        list.tab_order.push_back(button_id);
        if (laid.reason) {
            const kit::Rect box = local_of(*laid.reason, laid.bounds);
            const auto measure = [&fonts](std::string_view text) {
                return kit::text_width(*fonts, kit::FontRole::small, text);
            };
            add_text(
                list,
                box,
                kit::fit(laid.reason_text, box.width, measure),
                kit::colour::lock,
                kit::Align::right
            );
        }
        if (laid.chip) {
            kit::ChipLook chip;
            chip.text = laid.glyph;
            kit::add_chip(list, local_of(*laid.chip, laid.bounds), chip, kit::no_control, {});
        }
        if (focused_)
            kit::add_focus_ring(list, button, true);
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
    /// so the two rings never show at once.
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
        const Geometry laid = geometry_of(runtime_, view_);
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

    /// Puts the button's words in the look, in the regular font where they
    /// fit after the badge, else the small font, cut with … only when even
    /// that does not fit. A chip inside the button's right end keeps the words
    /// a padding clear of it.
    ///
    /// @param look the look the words go in
    /// @param fonts the fonts
    /// @param button the button, in the screen's points
    /// @param chip_left the chip's left edge; 0 when there is no chip
    void fit_caption(
        kit::ButtonLook& look, const kit::Fonts& fonts, const kit::Rect& button, int32_t chip_left
    ) const {
        const std::string words = caption_for(chosen_count());
        const kit::ButtonMarkParts parts = kit::button_mark_parts(button);
        int32_t room = parts.caption.width;
        if (chip_left > 0) {
            const int32_t pad = std::max(button.height / 5, int32_t{2});
            room = std::min(room, chip_left - pad - parts.caption.x);
        }
        look.font = kit::FontRole::regular;
        if (kit::text_width(fonts, kit::FontRole::regular, words) <= room) {
            look.caption = words;
            return;
        }
        look.font = kit::FontRole::small;
        const auto measure = [&fonts](std::string_view text) {
            return kit::text_width(fonts, kit::FontRole::small, text);
        };
        look.caption = measure(words) <= room ? words : kit::fit(words, std::max(room, 0), measure);
    }

    Runtime& runtime_;    ///< the runtime whose setups it shows over
    LayerView view_{};    ///< the view it was last laid out for
    bool hovered_{};      ///< the pointer is over the button
    bool pressed_{};      ///< a press on the button is held
    bool focused_{};      ///< the keyboard focus is on the button
    uint64_t revision_{}; ///< counts the changes to how it looks
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
