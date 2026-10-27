// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A computer seat's brain in the game's rooms. On SKIRMISH each computer
// row's own Player button names the seat's brain (the room's "Computer" for
// the built-in player) and has a notch at its right end. A press on the
// notch, or the right button anywhere on the Player button, opens the brain
// menu, drawn as the room's list box under the button; the left button
// elsewhere on it keeps 3.1c's own press. On NEWGAME and Any Mission the
// campaign's computer player has one button in the room's button art,
// under Difficulty, with the OA stamp and the notch; a press anywhere on it
// opens the menu. Tab, the arrows and the controller reach the notch and
// the campaign button; Up and Down on the campaign screens stay with the
// lists. Resting the pointer on a button, or the information key, shows
// the seat's card; while the menu is open, the card of the row selected.
// With no brain installed the screen is not there.
//
// The brain menu, the room's art and the drawing of room parts on the OA
// layer, which the battle room shares, are here too (brain_seats_room.hpp).

#include "brain_seats_room.hpp"
#include "engine_settings_state.hpp"
#include "library_host.hpp"
#include "oa_button_focus.hpp"
#include "oa_layer.hpp"

#include "oa/app/content/updates.hpp"
#include "oa/app/mod_summary.hpp"
#include "oa/app/runtime.hpp"
#include "oa/core/game_state.h"
#include "oa/data/brain_library/library.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/formats/oabrain/manifest.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/ui/brain_picker/model.hpp"
#include "oa/ui/brain_picker/text.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_multiplayer/presence_card.hpp"
#include "oa/ui/frontend_renderer/room_parts.hpp"
#include "oa/ui/gui_layout.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/pad_controls.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <exception>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {

namespace brain = oa::data::brain_library;
namespace picker = oa::ui::brain_picker;
namespace library = oa::ui::library;
namespace oabrain = oa::formats::oabrain;
namespace oamut = oa::formats::oamut;
namespace place = oa::ui::frontend_state;
namespace mp = oa::ui::frontend_multiplayer;
namespace pad = oa::ui::pad_controls;
namespace kit = oa::ui::kit;
namespace gui = oa::ui::gui_layout;
namespace room = oa::ui::frontend_renderer;

namespace {

constexpr std::string_view screen_name = "brain-seats";
/// The campaign's computer player. A mission seats that player in slot 1.
constexpr int32_t campaign_place = 1;
/// The least gap above and below the campaign button, in frame pixels.
constexpr int32_t column_gap = 2;
/// The gap place_block leaves when the column has no room, in frame pixels.
constexpr int32_t block_gap = 4;
/// The campaign button's height when place_block sizes it, in frame pixels.
constexpr int32_t campaign_height = 20;
/// The campaign button's automation name, without oa.
constexpr std::string_view campaign_button_name = "brain-seat.campaign";
/// The notch's width at a button's right end, in frame pixels.
constexpr int32_t notch_width = 14;
/// Frame pixels round a part that its focus marker lights.
constexpr int32_t focus_margin = room_focus_margin;
/// Points between the menu and the card beside it.
constexpr int32_t card_gap = 6;
/// Control ids start here, clear of a hover card's line ids.
constexpr int32_t id_base = 100;
/// The menu's control ids start here.
constexpr int32_t menu_id_base = 200;
/// The card's slot while the menu is open: the menu's selected row.
constexpr int32_t menu_card_slot = 1000;
/// The Library's tab the menu's last row opens.
constexpr library::Tab brains_tab = library::Tab::brains;

/// Returns one of the interface's words in the language shown.
///
/// @param context unused
/// @param english the word, in English
/// @return its translation, or the English
std::string_view shown_word(void* /*context*/, std::string_view english) {
    return oa::data::languages::interface_text(english);
}

/// The hooks a seat's label and card look words up through.
///
/// @return the hooks
library::TextHooks text_hooks() {
    return library::TextHooks{nullptr, shown_word};
}

/// Tells whether two language tags name one language.
///
/// ASCII case is ignored, and '_' is read as '-'.
///
/// @param left a tag
/// @param right a tag
/// @return true when they name one language
bool same_tag(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t at = 0; at < left.size(); ++at) {
        unsigned char first = static_cast<unsigned char>(left[at]);
        unsigned char second = static_cast<unsigned char>(right[at]);
        if (first == '_')
            first = '-';
        if (second == '_')
            second = '-';
        if (std::tolower(first) != std::tolower(second))
            return false;
    }
    return true;
}

/// The seat difficulty a preference difficulty names.
///
/// @param difficulty the preference, 0 easy, 2 hard, and medium otherwise
/// @return the difficulty
brain::Difficulty difficulty_of(uint32_t difficulty) noexcept {
    if (difficulty == static_cast<uint32_t>(OA_DIFFICULTY_EASY))
        return brain::Difficulty::easy;
    if (difficulty == static_cast<uint32_t>(OA_DIFFICULTY_HARD))
        return brain::Difficulty::hard;
    return brain::Difficulty::medium;
}

/// The level id a built-in seat stores for a difficulty.
///
/// @param difficulty the difficulty
/// @return easy, medium or hard
const char* level_id_of(brain::Difficulty difficulty) noexcept {
    switch (difficulty) {
    case brain::Difficulty::easy:
        return "easy";
    case brain::Difficulty::hard:
        return "hard";
    case brain::Difficulty::medium:
        break;
    }
    return "medium";
}

/// Maps a manifest difficulty onto the seat record's.
///
/// @param difficulty the manifest's difficulty
/// @return the seat difficulty
brain::Difficulty library_difficulty(oabrain::Difficulty difficulty) noexcept {
    switch (difficulty) {
    case oabrain::Difficulty::easy:
        return brain::Difficulty::easy;
    case oabrain::Difficulty::medium:
        return brain::Difficulty::medium;
    case oabrain::Difficulty::hard:
        return brain::Difficulty::hard;
    }
    return brain::Difficulty::medium;
}

/// The level id `default_level` chooses.
///
/// @param manifest the brain
/// @param wanted the setup's difficulty
/// @return the level id; empty when the brain has no level
std::string level_for(const oabrain::Manifest& manifest, brain::Difficulty wanted) {
    if (manifest.levels.empty())
        return {};
    std::vector<brain::Difficulty> levels;
    levels.reserve(manifest.levels.size());
    for (const oabrain::Level& level : manifest.levels)
        levels.push_back(library_difficulty(level.difficulty));
    const std::size_t chosen = brain::default_level(levels, wanted);
    if (chosen >= manifest.levels.size())
        return {};
    return manifest.levels[chosen].id;
}

/// The mod being played, for a brain's fit. Empty when the game plays no mod.
///
/// @param mods the mods the game lists
/// @return the mod's id, name and version; an empty id is the base game
template <typename Mods>
library::PlayedMod played_mod(const Mods& mods) {
    library::PlayedMod played;
    if (mods.playing_mod_folder.empty())
        return played;
    for (std::size_t at = 0;
         at < mods.mod_folders.size() && at < mods.mod_names.size() && at < mods.mod_details.size();
         ++at) {
        if (mods.mod_folders[at] != mods.playing_mod_folder)
            continue;
        const auto& details = mods.mod_details[at];
        played.id = details.profile_id;
        played.name = mods.mod_names[at];
        if (details.version != mod_summary_no_version)
            played.version = details.version;
        break;
    }
    return played;
}

place::PlaceRect as_place(const room::SourceRect& rect) noexcept {
    return {rect.x, rect.y, rect.width, rect.height};
}

room::SourceRect as_source(const place::PlaceRect& rect) noexcept {
    return {rect.x, rect.y, rect.width, rect.height};
}

/// Grows a rectangle of frame pixels on every side.
///
/// @param rect the rectangle
/// @param margin the pixels added on each side
/// @return the grown rectangle
room::SourceRect grown(const room::SourceRect& rect, int32_t margin) noexcept {
    return {rect.x - margin, rect.y - margin, rect.width + margin * 2, rect.height + margin * 2};
}

/// Tells whether a rectangle of frame pixels holds a pixel.
///
/// @param rect the rectangle
/// @param at the pixel
/// @return true when it does
bool holds_pixel(const room::SourceRect& rect, kit::Point at) noexcept {
    return at.x >= rect.x && at.y >= rect.y && at.x < rect.x + rect.width &&
           at.y < rect.y + rect.height;
}

/// Returns the union of two rectangles.
///
/// @param into the rectangle so far; empty when its width is not positive
/// @param extra the rectangle to include
/// @return the union
kit::Rect unite(const kit::Rect& into, const kit::Rect& extra) noexcept {
    if (into.width <= 0 || into.height <= 0)
        return extra;
    if (extra.width <= 0 || extra.height <= 0)
        return into;
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
/// @param gadget the gadget
/// @return true when Tab can stop on it
bool takes_focus(const gui::Gadget& gadget) noexcept {
    if (gadget.common.width <= 0 || gadget.common.height <= 0)
        return false;
    return gui::keyboard_focusable(gadget);
}

/// Returns where a hover card's arrow is drawn.
///
/// @param placed the card
/// @param look the card's look, its arrow pointed
/// @param arrow_depth how far the arrow reaches out of the card
/// @return the arrow's rectangle; empty without an arrow
kit::Rect arrow_rect(const kit::Rect& placed, const kit::HoverCardLook& look, int32_t arrow_depth) {
    const int32_t across = 2 * arrow_depth - 1;
    const int32_t left = placed.x + look.arrow_at - (arrow_depth - 1);
    switch (look.arrow) {
    case kit::Side::top:
        return {left, placed.y - arrow_depth, across, arrow_depth};
    case kit::Side::bottom:
        return {left, placed.y + placed.height, across, arrow_depth};
    case kit::Side::none:
    case kit::Side::left:
    case kit::Side::right:
        break;
    }
    return {};
}

/// Writes one computer seat. A slot that is not one is left alone.
///
/// @param seats the setup's seats
/// @param settings the skirmish slots
/// @param slot the slot
/// @param choice the choice
void write_one(
    brain::Seats& seats,
    const entry::SkirmishSettings& settings,
    int32_t slot,
    const picker::Choice& choice
) {
    if (slot < 0 || static_cast<std::size_t>(slot) >= settings.slots.size())
        return;
    if (slot >= settings.slot_count || slot >= static_cast<int32_t>(brain::most_seats))
        return;
    if (settings.slots[static_cast<std::size_t>(slot)].controller != entry::controller::computer)
        return;
    brain::Seat* own = nullptr;
    for (brain::Seat& seat : seats.seats) {
        if (seat.place == slot)
            own = &seat;
    }
    if (own == nullptr) {
        seats.seats.push_back({});
        own = &seats.seats.back();
    }
    own->place = slot;
    own->brain = choice.brain;
    own->level = choice.level;
    own->options = choice.options;
    own->version.clear();
    own->package_sha256.clear();
    own->name.clear();
}

/// A brain's name and one level's name, in the language shown.
struct NamedBrain {
    std::string name{};  ///< the brain's name
    std::string level{}; ///< the level's name
};

/// Resolves a brain's name and a level's name the way the picker does.
///
/// The manifest's own strings fill a name no translation gave. A brain with
/// no name uses its id.
///
/// @param manifest the brain
/// @param level_id the level
/// @return the names
NamedBrain names_of(const oabrain::Manifest& manifest, std::string_view level_id) {
    NamedBrain named;
    const oa::data::languages::FallbackChain chain =
        oa::data::languages::fallback_chain(oa::data::languages::interface_language());
    for (const oa::data::languages::Language* language : chain.view()) {
        if (language == nullptr)
            continue;
        for (const oabrain::Translation& translation : manifest.strings) {
            if (!same_tag(translation.words.language, language->tag))
                continue;
            if (named.name.empty() && !translation.words.name.empty())
                named.name = translation.words.name;
            if (named.level.empty()) {
                for (const oabrain::LevelName& level : translation.levels) {
                    if (level.id == level_id && !level.name.empty())
                        named.level = level.name;
                }
            }
        }
    }
    if (named.name.empty())
        named.name = manifest.name;
    if (named.name.empty())
        named.name = manifest.id;
    if (named.level.empty()) {
        if (const oabrain::Level* level = oabrain::find_level(manifest, level_id))
            named.level = level->name;
    }
    if (named.level.empty())
        named.level = std::string(level_id);
    return named;
}

/// An automation word from a brain's id: lower-case letters, digits and
/// hyphens, anything else a hyphen.
///
/// @param id the id
/// @return the word; "brain" for an empty id
std::string automation_word(std::string_view id) {
    std::string word;
    word.reserve(id.size());
    for (const char letter : id) {
        const auto byte = static_cast<unsigned char>(letter);
        if (std::isalnum(byte) != 0)
            word.push_back(static_cast<char>(std::tolower(byte)));
        else
            word.push_back('-');
    }
    if (word.empty())
        word = "brain";
    return word;
}

/// Tells whether a menu row can be chosen.
///
/// @param row the row
/// @return true for the built-in player, a brain that fits, the check box and the Library row
bool choosable(const BrainMenuRow& row) noexcept {
    if (row.kind == BrainMenuKind::heading)
        return false;
    return !(row.kind == BrainMenuKind::brain && row.unfit);
}

/// The choice a row plays.
///
/// @param menu the menu
/// @param row the row
/// @return the choice
picker::Choice choice_of(const BrainMenu& menu, const BrainMenuRow& row) {
    picker::Choice choice;
    if (row.kind == BrainMenuKind::brain) {
        choice.brain = row.id;
        choice.level = row.level_id;
        if (row.current)
            choice.options = menu.options;
        return choice;
    }
    choice.level = row.level_id;
    return choice;
}

/// The current row of the menu, or null.
///
/// @param menu the menu
/// @return the row the seat plays now
const BrainMenuRow* current_row(const BrainMenu& menu) noexcept {
    for (const BrainMenuRow& row : menu.rows)
        if (row.current)
            return &row;
    return nullptr;
}

/// Steps the current row's level one way, round its levels.
///
/// @param menu the menu
/// @param row the row
/// @param forward true for the level after
/// @return a level answer; none for a row without levels to step through
BrainMenuAnswer step_level(const BrainMenu& menu, const BrainMenuRow& row, bool forward) {
    BrainMenuAnswer answer;
    if ((row.kind != BrainMenuKind::brain && row.kind != BrainMenuKind::built_in) || !row.current ||
        row.level_ids.size() < 2)
        return answer;
    std::size_t at = 0;
    for (std::size_t index = 0; index < row.level_ids.size(); ++index)
        if (row.level_ids[index] == row.level_id)
            at = index;
    const std::size_t count = row.level_ids.size();
    at = forward ? (at + 1) % count : (at + count - 1) % count;
    answer.kind = BrainMenuAnswer::Kind::level;
    answer.choice = choice_of(menu, row);
    answer.choice.level = row.level_ids[at];
    answer.every = menu.every;
    return answer;
}

/// Answers a press on one row.
///
/// @param[in,out] menu the menu; the check box turns
/// @param index the row
/// @return what to do
BrainMenuAnswer press_row(BrainMenu& menu, int32_t index) {
    BrainMenuAnswer answer;
    if (index < 0 || static_cast<std::size_t>(index) >= menu.rows.size())
        return answer;
    const BrainMenuRow& row = menu.rows[static_cast<std::size_t>(index)];
    if (!choosable(row))
        return answer;
    switch (row.kind) {
    case BrainMenuKind::heading:
        break;
    case BrainMenuKind::built_in:
    case BrainMenuKind::brain:
        answer.kind = BrainMenuAnswer::Kind::choose;
        answer.choice = choice_of(menu, row);
        answer.every = menu.every;
        break;
    case BrainMenuKind::every_seat:
        menu.every = !menu.every;
        if (menu.every) {
            if (const BrainMenuRow* current = current_row(menu)) {
                answer.kind = BrainMenuAnswer::Kind::every;
                answer.choice = choice_of(menu, *current);
                answer.every = true;
            }
        }
        break;
    case BrainMenuKind::get_brains:
        answer.kind = BrainMenuAnswer::Kind::get_brains;
        break;
    }
    return answer;
}

/// The room's frame as a rectangle of frame pixels.
///
/// @param seen what the layer's screens are placed on
/// @return the frame
room::SourceRect room_of(const LayerView& seen) noexcept {
    return {0, 0, seen.picture_size.x, seen.picture_size.y};
}

} // namespace

// ---- Shared by the rooms (brain_seats_room.hpp) ----

kit::Point frame_point(const LayerView& seen, kit::Point layer_point) {
    if (seen.picture.width <= 0 || seen.picture.height <= 0 || seen.picture_size.x <= 0 ||
        seen.picture_size.y <= 0)
        return {-1, -1};
    const int64_t percent = std::max(seen.scale_percent, int32_t{100});
    const auto floor_divide = [](int64_t value, int64_t by) {
        return value >= 0 ? value / by : -((-value + by - 1) / by);
    };
    const int64_t window_x = floor_divide(static_cast<int64_t>(layer_point.x) * percent, 100);
    const int64_t window_y = floor_divide(static_cast<int64_t>(layer_point.y) * percent, 100);
    return {
        static_cast<int32_t>(
            floor_divide((window_x - seen.picture.x) * seen.picture_size.x, seen.picture.width)
        ),
        static_cast<int32_t>(
            floor_divide((window_y - seen.picture.y) * seen.picture_size.y, seen.picture.height)
        )
    };
}

kit::Rect frame_rect_points(const LayerView& seen, const room::SourceRect& frame) {
    return frame_to_points(seen, kit::Rect{frame.x, frame.y, frame.width, frame.height});
}

room::SourceRect points_rect_frame(const LayerView& seen, const kit::Rect& layer_points) {
    const kit::Point first = frame_point(seen, {layer_points.x, layer_points.y});
    const kit::Point last = frame_point(
        seen, {layer_points.x + layer_points.width - 1, layer_points.y + layer_points.height - 1}
    );
    return {first.x, first.y, last.x - first.x + 1, last.y - first.y + 1};
}

kit::Rect
frame_rect_local(const LayerView& seen, const kit::Rect& bounds, const room::SourceRect& frame) {
    return local_of(frame_rect_points(seen, frame), bounds);
}

void paint_room_parts(
    const kit::Canvas& canvas,
    const LayerView& seen,
    const kit::Rect& bounds,
    const room::RoomCanvas& parts
) {
    if (parts.area.width <= 0 || parts.area.height <= 0)
        return;
    std::vector<uint8_t> rgba;
    room::room_canvas_rgba(parts, rgba);
    const kit::Rect at = frame_rect_local(seen, bounds, parts.area);
    kit::draw_picture(
        canvas,
        at,
        room::RgbaPicture{
            static_cast<uint32_t>(parts.area.width), static_cast<uint32_t>(parts.area.height), rgba
        }
    );
}

std::vector<room::RoomListRow> brain_menu_list_rows(const BrainMenu& menu) {
    std::vector<room::RoomListRow> rows;
    rows.reserve(menu.rows.size());
    bool ruled = false;
    for (std::size_t index = 0; index < menu.rows.size(); ++index) {
        const BrainMenuRow& row = menu.rows[index];
        room::RoomListRow shown;
        shown.text = room::room_text(row.words);
        shown.selected = static_cast<int32_t>(index) == menu.selected;
        switch (row.kind) {
        case BrainMenuKind::heading:
            shown.heading = true;
            break;
        case BrainMenuKind::built_in:
        case BrainMenuKind::brain:
            shown.ticked = row.current;
            shown.grey = row.unfit;
            shown.value = row.current && row.level_ids.size() > 1
                              ? room::room_text("< " + row.level_words + " >")
                              : room::room_text(row.level_words);
            break;
        case BrainMenuKind::every_seat:
            shown.checkbox = true;
            shown.checked = menu.every;
            shown.rule_above = !ruled;
            ruled = true;
            break;
        case BrainMenuKind::get_brains:
            shown.rule_above = !ruled;
            ruled = true;
            break;
        }
        rows.push_back(std::move(shown));
    }
    return rows;
}

void place_brain_menu(
    const room::RoomArt& art,
    BrainMenu& menu,
    const room::SourceRect& button,
    const room::SourceRect& room_frame
) {
    const std::vector<room::RoomListRow> rows = brain_menu_list_rows(menu);
    const room::SourceRect size = room::room_list_size(art, rows, brain_menu_value_width);
    const int32_t width = std::min(std::max(size.width, button.width), room_frame.width);
    const int32_t height = std::min(size.height, room_frame.height);
    int32_t x = button.x;
    int32_t y = button.y + button.height + 1;
    if (y + height > room_frame.y + room_frame.height)
        y = button.y - height - 1;
    x = std::clamp(x, room_frame.x, room_frame.x + room_frame.width - width);
    y = std::clamp(y, room_frame.y, room_frame.y + room_frame.height - height);
    menu.rect = {x, y, width, height};
}

int32_t brain_menu_row_at(const room::RoomArt& art, const BrainMenu& menu, kit::Point at) {
    const std::vector<room::RoomListRow> rows = brain_menu_list_rows(menu);
    room::RoomListLook look;
    look.rect = menu.rect;
    look.rows = rows;
    look.value_width = brain_menu_value_width;
    const std::vector<room::SourceRect> rects = room::room_list_row_rects(art, look);
    for (std::size_t index = 0; index < rects.size(); ++index)
        if (holds_pixel(rects[index], at))
            return static_cast<int32_t>(index);
    return -1;
}

BrainMenuArrows brain_menu_arrows(const room::RoomArt& art, const BrainMenu& menu, int32_t row) {
    BrainMenuArrows arrows;
    if (row < 0 || static_cast<std::size_t>(row) >= menu.rows.size())
        return arrows;
    const BrainMenuRow& shown = menu.rows[static_cast<std::size_t>(row)];
    if ((shown.kind != BrainMenuKind::brain && shown.kind != BrainMenuKind::built_in) ||
        !shown.current || shown.level_ids.size() < 2)
        return arrows;
    const std::vector<room::RoomListRow> rows = brain_menu_list_rows(menu);
    room::RoomListLook look;
    look.rect = menu.rect;
    look.rows = rows;
    look.value_width = brain_menu_value_width;
    const std::vector<room::SourceRect> rects = room::room_list_row_rects(art, look);
    if (static_cast<std::size_t>(row) >= rects.size())
        return arrows;
    const room::SourceRect& at = rects[static_cast<std::size_t>(row)];
    const std::string value = room::fit_room_text(
        art,
        room::RoomFont::caption,
        rows[static_cast<std::size_t>(row)].value,
        brain_menu_value_width
    );
    const int32_t right = at.x + at.width - 3;
    const int32_t width = room::room_text_width(art, room::RoomFont::caption, value);
    // Each arrow takes its glyph and its space, and a little more to aim at.
    const int32_t arrow = std::max(
        room::room_text_width(art, room::RoomFont::caption, room::room_text("< ")) + 3, int32_t{10}
    );
    arrows.previous = {right - width - 2, at.y, arrow, at.height};
    arrows.next = {right - arrow + 2, at.y, arrow, at.height};
    return arrows;
}

BrainMenuAnswer brain_menu_press(const room::RoomArt& art, BrainMenu& menu, kit::Point at) {
    BrainMenuAnswer answer;
    if (!holds_pixel(menu.rect, at)) {
        answer.kind = BrainMenuAnswer::Kind::close;
        return answer;
    }
    const int32_t row = brain_menu_row_at(art, menu, at);
    if (row < 0)
        return answer;
    menu.selected = choosable(menu.rows[static_cast<std::size_t>(row)]) ? row : menu.selected;
    const BrainMenuArrows arrows = brain_menu_arrows(art, menu, row);
    if (arrows.previous.width > 0 && holds_pixel(arrows.previous, at))
        return step_level(menu, menu.rows[static_cast<std::size_t>(row)], false);
    if (arrows.next.width > 0 && holds_pixel(arrows.next, at))
        return step_level(menu, menu.rows[static_cast<std::size_t>(row)], true);
    return press_row(menu, row);
}

BrainMenuAnswer brain_menu_key(BrainMenu& menu, kit::Key pressed) {
    BrainMenuAnswer answer;
    const auto count = static_cast<int32_t>(menu.rows.size());
    switch (pressed) {
    case kit::Key::escape:
        answer.kind = BrainMenuAnswer::Kind::close;
        return answer;
    case kit::Key::up:
    case kit::Key::down: {
        const int32_t step = pressed == kit::Key::down ? 1 : -1;
        for (int32_t at = menu.selected + step; at >= 0 && at < count; at += step) {
            if (choosable(menu.rows[static_cast<std::size_t>(at)])) {
                menu.selected = at;
                break;
            }
        }
        return answer;
    }
    case kit::Key::left:
    case kit::Key::right:
        if (menu.selected >= 0 && menu.selected < count)
            return step_level(
                menu, menu.rows[static_cast<std::size_t>(menu.selected)], pressed == kit::Key::right
            );
        return answer;
    case kit::Key::enter:
    case kit::Key::space:
        return press_row(menu, menu.selected);
    default:
        break;
    }
    return answer;
}

void add_brain_menu_controls(
    kit::DisplayList& list,
    const room::RoomArt& art,
    const BrainMenu& menu,
    const LayerView& seen,
    const kit::Rect& bounds,
    kit::ControlId first_id
) {
    const std::vector<room::RoomListRow> rows = brain_menu_list_rows(menu);
    room::RoomListLook look;
    look.rect = menu.rect;
    look.rows = rows;
    look.value_width = brain_menu_value_width;
    const std::vector<room::SourceRect> rects = room::room_list_row_rects(art, look);
    kit::ControlId next_id = first_id;
    const auto add = [&](const room::SourceRect& rect,
                         std::string name,
                         kit::ControlKind kind,
                         std::string text,
                         bool enabled,
                         bool checked) {
        kit::Control control;
        control.id = next_id++;
        control.rect = frame_rect_local(seen, bounds, rect);
        control.enabled = enabled;
        control.name = std::move(name);
        control.kind = kind;
        control.focusable = false;
        control.checked = checked;
        control.text = std::move(text);
        list.controls.push_back(std::move(control));
    };
    for (std::size_t index = 0; index < menu.rows.size() && index < rects.size(); ++index) {
        const BrainMenuArrows arrows = brain_menu_arrows(art, menu, static_cast<int32_t>(index));
        if (arrows.previous.width > 0) {
            add(arrows.previous,
                "brain-menu.level.previous",
                kit::ControlKind::button,
                "<",
                true,
                false);
            add(arrows.next, "brain-menu.level.next", kit::ControlKind::button, ">", true, false);
        }
    }
    for (std::size_t index = 0; index < menu.rows.size() && index < rects.size(); ++index) {
        const BrainMenuRow& row = menu.rows[index];
        switch (row.kind) {
        case BrainMenuKind::heading:
            break;
        case BrainMenuKind::built_in:
        case BrainMenuKind::brain: {
            std::string text = row.words;
            if (!row.level_words.empty())
                text += " · " + row.level_words;
            add(rects[index],
                "brain-menu.row." + (row.kind == BrainMenuKind::built_in ? std::string("built-in")
                                                                         : automation_word(row.id)),
                kit::ControlKind::list_item,
                std::move(text),
                !row.unfit,
                row.current);
            break;
        }
        case BrainMenuKind::every_seat:
            add(rects[index],
                "brain-menu.every-seat",
                kit::ControlKind::toggle,
                row.words,
                true,
                menu.every);
            break;
        case BrainMenuKind::get_brains:
            add(rects[index],
                "brain-menu.get-brains",
                kit::ControlKind::button,
                row.words,
                true,
                false);
            break;
        }
    }
}

bool BrainSeatsAccess::shows(Runtime& runtime) {
    const bool campaign =
        runtime.screen_ == Screen::new_campaign || runtime.screen_ == Screen::any_mission;
    if (runtime.screen_ != Screen::skirmish && !campaign)
        return false;
    if (runtime.match_ != nullptr)
        return false;
    if (oa::ui::frontend_dialogs::dialog_count() != 0)
        return false;
    if (runtime.oa_layer().screen_fonts() == nullptr)
        return false;
    return !runtime.installed_brains().brains.empty();
}

void BrainSeatsAccess::write(
    Runtime& runtime, int32_t slot, const picker::Choice& choice, bool every
) {
    brain::Seats& seats = runtime.setup_brain_seats(brain::Setup::skirmish);
    const entry::SkirmishSettings& settings = runtime.skirmish_settings_;
    if (!every) {
        write_one(seats, settings, slot, choice);
        return;
    }
    const int32_t limit =
        std::min(settings.slot_count, static_cast<int32_t>(settings.slots.size()));
    for (int32_t at = 0; at < limit; ++at)
        write_one(seats, settings, at, choice);
}

void BrainSeatsAccess::write_campaign(Runtime& runtime, const picker::Choice& choice) {
    brain::Seats& seats = runtime.setup_brain_seats(brain::Setup::campaign);
    brain::Seat* own = nullptr;
    for (brain::Seat& seat : seats.seats) {
        if (seat.place == campaign_place)
            own = &seat;
    }
    if (own == nullptr) {
        seats.seats.push_back({});
        own = &seats.seats.back();
    }
    own->place = campaign_place;
    own->brain = choice.brain;
    own->level = choice.level;
    own->options = choice.options;
    own->version.clear();
    own->package_sha256.clear();
    own->name.clear();
}

room::RoomArt BrainSeatsAccess::screen_art(const Runtime& runtime) {
    return room::RoomArt{&runtime.resources_, room::room_palette(runtime.resources_)};
}

const room::Surface& BrainSeatsAccess::frame(const Runtime& runtime) {
    return runtime.surface_;
}

std::optional<room::ScreenResources>
BrainSeatsAccess::load_art(Runtime& runtime, const room::ScreenAssetNames& names) {
    try {
        return room::load_screen_art(runtime.assets_, names);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

void BrainSeatsAccess::get_more_brains(Runtime& runtime) {
    auto shown = std::make_shared<Runtime::LibraryReturn>();
    shown->tab = brains_tab;
    runtime.take_library_return(std::move(shown));
    runtime.pending_screen_ = screen_id(Screen::main_menu);
}

BrainMenu BrainSeatsAccess::menu(
    Runtime& runtime,
    int32_t seat,
    const picker::Choice& current,
    brain::Difficulty difficulty,
    std::string_view built_in_level,
    const std::vector<std::pair<std::string, std::string>>& built_in_levels,
    std::string_view heading,
    bool every_row,
    bool library_row
) {
    BrainMenu made;
    made.seat = seat;
    made.options = current.options;
    BrainMenuRow title;
    title.kind = BrainMenuKind::heading;
    title.words = std::string(oa::data::languages::interface_text(heading));
    made.rows.push_back(std::move(title));

    BrainMenuRow built_in;
    built_in.kind = BrainMenuKind::built_in;
    built_in.words = runtime.translate_ui("Computer");
    built_in.level_id =
        current.brain.empty() && !current.level.empty() ? current.level : level_id_of(difficulty);
    built_in.level_words = std::string(built_in_level);
    built_in.shown.level = std::string(built_in_level);
    for (const auto& level : built_in_levels) {
        built_in.level_ids.push_back(level.first);
        built_in.level_names.push_back(level.second);
    }
    made.rows.push_back(std::move(built_in));

    library::Inputs probe;
    probe.engine = content::this_engine_rules().engine;
    probe.played = played_mod(runtime.engine_settings_state());
    probe.content.enabled = runtime.content_choice().expansions;
    const brain::Listing& listing = runtime.installed_brains();
    std::vector<BrainMenuRow> brains;
    brains.reserve(listing.brains.size());
    bool current_found = current.brain.empty();
    for (const brain::InstalledBrain& installed : listing.brains) {
        const oabrain::Manifest& manifest = installed.manifest;
        BrainMenuRow row;
        row.kind = BrainMenuKind::brain;
        row.id = manifest.id;
        row.unfit =
            !library::brain_unfit_reason(probe, brain_facts_of(manifest, installed.network_bytes))
                 .empty();
        row.current = !row.unfit && manifest.id == current.brain;
        if (row.current)
            current_found = true;
        row.level_id = row.current && oabrain::find_level(manifest, current.level) != nullptr
                           ? current.level
                           : level_for(manifest, difficulty);
        for (const oabrain::Level& level : manifest.levels) {
            row.level_ids.push_back(level.id);
            row.level_names.push_back(names_of(manifest, level.id).level);
        }
        const NamedBrain names = names_of(manifest, row.level_id);
        row.words = names.name;
        row.level_words = row.level_id.empty() ? std::string() : names.level;
        row.shown.name = names.name;
        row.shown.version = oamut::version_text(manifest.version);
        row.shown.level = row.level_words;
        row.shown.full_awareness = manifest.full_awareness;
        brains.push_back(std::move(row));
    }
    std::stable_sort(
        brains.begin(), brains.end(), [](const BrainMenuRow& left, const BrainMenuRow& right) {
            return left.words < right.words;
        }
    );
    // A seat whose brain is missing or unfit plays the built-in player.
    made.rows[1].current = !current_found || current.brain.empty();
    for (BrainMenuRow& row : brains)
        made.rows.push_back(std::move(row));
    if (every_row) {
        BrainMenuRow every;
        every.kind = BrainMenuKind::every_seat;
        every.words =
            std::string(oa::data::languages::interface_text(brain_menu_words::every_seat));
        made.rows.push_back(std::move(every));
    }
    if (library_row) {
        BrainMenuRow more;
        more.kind = BrainMenuKind::get_brains;
        more.words = std::string(oa::data::languages::interface_text(brain_menu_words::get_brains));
        made.rows.push_back(std::move(more));
    }
    for (std::size_t index = 0; index < made.rows.size(); ++index)
        if (made.rows[index].current)
            made.selected = static_cast<int32_t>(index);
    return made;
}

/// The computer seats' brains over SKIRMISH, and the campaign's one button
/// over NEWGAME and Any Mission. Not modal, no backdrop, not opaque, so the
/// room shows round them. A notch, a campaign button and an open menu take
/// their own presses; the left button on the rest of a Player button goes
/// on to the room. Y stays Mutators'.
struct Runtime::BrainSeatsScreen final : LayerScreen {
    /// Makes the screen.
    ///
    /// @param runtime the runtime
    explicit BrainSeatsScreen(Runtime& runtime) : runtime_(runtime) {}

    /// Returns "brain-seats".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return screen_name; }

    /// Returns the buttons, their focus markers, the open menu and the open
    /// card, in the window.
    ///
    /// @param placed_on what the screen is placed on
    /// @return its place; empty while it does not show
    [[nodiscard]] LayerPlacement placement(const LayerView& placed_on) const override {
        const Geometry laid = geometry(placed_on);
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

    /// Tells that what the buttons do not take goes on.
    ///
    /// @return false
    [[nodiscard]] bool modal() const override { return false; }

    /// Tells that the buttons darken nothing.
    ///
    /// @return false
    [[nodiscard]] bool backdrop() const override { return false; }

    /// Tells that the buttons lie over the picture.
    ///
    /// @return false
    [[nodiscard]] bool opaque() const override { return false; }

    /// Follows the view the buttons are placed on.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override { view_ = placed_on; }

    /// Draws each seat's button in the room's art, the open menu as the
    /// room's list box, and the open card.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override {
        const Geometry laid = geometry(view_);
        if (!laid.ok)
            return;
        const room::RoomArt art = BrainSeatsAccess::screen_art(runtime_);
        const room::Surface& frame = BrainSeatsAccess::frame(runtime_);
        for (const SeatButton& button : laid.buttons) {
            room::RoomCanvas parts = room::room_canvas(grown(button.rect, focus_margin), &frame);
            draw_button(parts, art, button);
            paint_room_parts(canvas, view_, laid.bounds, parts);
        }
        if (menu_.seat >= 0) {
            room::RoomCanvas parts = room::room_canvas(menu_.rect, &frame);
            const std::vector<room::RoomListRow> rows = brain_menu_list_rows(menu_);
            room::RoomListLook look;
            look.rect = menu_.rect;
            look.rows = rows;
            look.value_width = brain_menu_value_width;
            room::draw_room_list(parts, art, look);
            paint_room_parts(canvas, view_, laid.bounds, parts);
        }
        if (laid.card) {
            kit::DisplayList card;
            kit::add_hover_card(card, *laid.card, laid.card_look);
            kit::paint(canvas, card);
        }
    }

    /// Takes a press on a notch, a campaign button, the open menu or the open
    /// card, and the right button on a computer's Player button. While the
    /// menu is open it takes every press: one outside it closes it.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the screen's points
    /// @param finger_reach a finger's reach; 0 for a mouse
    /// @return redraw or none when the screen takes it; pass otherwise
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        const Geometry laid = geometry(view_);
        if (!laid.ok)
            return LayerAnswer::pass;
        pointer_layer_ = {at.x + laid.bounds.x, at.y + laid.bounds.y};
        have_pointer_ = true;
        const kit::Point pixel = frame_point(view_, pointer_layer_);
        if (menu_.seat >= 0)
            return menu_pointer(kind, button, pixel);
        const int32_t over_notch = notch_at(laid, pixel);
        const int32_t over_button = button_at(laid, pixel);
        const int32_t over = kind != ScreenInputKind::pointer_move && finger_reach > 0
                                 ? finger_slot(laid, at, finger_reach)
                                 : over_notch;
        if (over_button != hovered_slot_) {
            hovered_slot_ = over_button;
            ++revision_;
        }
        const int32_t card_before = card_.open_slot();
        follow_card(laid);
        if (card_.open_slot() != card_before)
            ++revision_;
        const bool on_card = card_contains(laid, at);
        if (kind == ScreenInputKind::pointer_down && button == SDL_BUTTON_RIGHT &&
            over_button >= 0) {
            right_pressed_slot_ = over_button;
            return LayerAnswer::none;
        }
        if (kind == ScreenInputKind::pointer_up && button == SDL_BUTTON_RIGHT &&
            right_pressed_slot_ >= 0) {
            const int32_t pressed = right_pressed_slot_;
            right_pressed_slot_ = -1;
            if (pressed == over_button)
                open_menu(pressed);
            return LayerAnswer::redraw;
        }
        if (kind == ScreenInputKind::pointer_down && button == SDL_BUTTON_LEFT) {
            if (over >= 0) {
                pressed_slot_ = over;
                card_pressed_ = false;
                ++revision_;
                return LayerAnswer::redraw;
            }
            if (on_card) {
                pressed_slot_ = -1;
                card_pressed_ = true;
                return LayerAnswer::none;
            }
            pressed_slot_ = -1;
            card_pressed_ = false;
            if (focused_slot_ >= 0) {
                focused_slot_ = -1;
                ++revision_;
            }
            return LayerAnswer::pass;
        }
        if (kind == ScreenInputKind::pointer_up && button == SDL_BUTTON_LEFT) {
            if (card_pressed_) {
                card_pressed_ = false;
                return LayerAnswer::none;
            }
            if (on_card && pressed_slot_ < 0)
                return LayerAnswer::none;
            if (pressed_slot_ >= 0) {
                const int32_t pressed = pressed_slot_;
                pressed_slot_ = -1;
                ++revision_;
                if (over == pressed)
                    open_menu(pressed);
                return LayerAnswer::redraw;
            }
        }
        return LayerAnswer::pass;
    }

    /// Takes the open menu's keys, the information key, Escape when a card
    /// is open, and the keys a focused notch or campaign button holds.
    ///
    /// @param pressed the key
    /// @return none or redraw when the screen takes it; pass otherwise
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        if (menu_.seat >= 0) {
            if (pressed == kit::Key::tab || pressed == kit::Key::back_tab)
                return LayerAnswer::none;
            const BrainMenuAnswer answer = brain_menu_key(menu_, pressed);
            apply(answer);
            ++revision_;
            return LayerAnswer::redraw;
        }
        if (pressed == kit::Key::info) {
            const int32_t focused = focused_slot_ >= 0 ? focused_slot_ : mp::presence_card_no_slot;
            if (card_.key_info(focused)) {
                ++revision_;
                return LayerAnswer::none;
            }
            return LayerAnswer::pass;
        }
        if (pressed == kit::Key::escape) {
            if (card_.key_close()) {
                ++revision_;
                return LayerAnswer::none;
            }
            return LayerAnswer::pass;
        }
        return answer_key(pressed);
    }

    /// Lets every shortcut go on.
    ///
    /// @return pass
    LayerAnswer shortcut(uint32_t /*sdl_key*/, uint16_t /*modifiers*/) override {
        return LayerAnswer::pass;
    }

    /// Takes the focus keys and a press on the same terms as the keyboard.
    /// Y, View and Back go on: Y opens Mutators, and View and Back become
    /// the information key and Escape once.
    ///
    /// @param action the gamepad action
    /// @return none or redraw when the screen takes it; pass otherwise
    LayerAnswer pad_action(pad::Action action) override {
        if (action == pad::Action::press)
            return key(kit::Key::enter, 0);
        if (action == pad::Action::focus_next)
            return key(kit::Key::tab, 0);
        if (action == pad::Action::focus_previous)
            return key(kit::Key::back_tab, 0);
        if (action == pad::Action::focus_up)
            return key(kit::Key::up, 0);
        if (action == pad::Action::focus_down)
            return key(kit::Key::down, 0);
        if (action == pad::Action::focus_left)
            return key(kit::Key::left, 0);
        if (action == pad::Action::focus_right)
            return key(kit::Key::right, 0);
        return LayerAnswer::pass;
    }

    /// Lets the wheel go on, and takes it while the menu is open.
    ///
    /// @return pass, or none while the menu is open
    LayerAnswer wheel(kit::Point /*at*/, float /*notches*/) override {
        return menu_.seat >= 0 ? LayerAnswer::none : LayerAnswer::pass;
    }

    /// Returns the screen's controls for automation and the pointer: each
    /// notch, or the campaign's button, the open menu's rows and the open
    /// card's lines.
    ///
    /// A notch's automation name is brain-seat.<slot>, which the endpoint
    /// lists as oa.brain-seat.<slot>; the campaign button's is
    /// brain-seat.campaign. Their text is the seat's whole label. Tab's
    /// order holds them. The menu's names are add_brain_menu_controls'.
    ///
    /// @return the display list
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list;
        const Geometry laid = geometry(view_);
        if (!laid.ok)
            return list;
        if (menu_.seat >= 0)
            add_brain_menu_controls(
                list,
                BrainSeatsAccess::screen_art(runtime_),
                menu_,
                view_,
                laid.bounds,
                menu_id_base
            );
        std::vector<const SeatButton*> order;
        order.reserve(laid.buttons.size());
        for (const SeatButton& button : laid.buttons)
            order.push_back(&button);
        std::sort(order.begin(), order.end(), [](const SeatButton* left, const SeatButton* right) {
            return order_key(left->order_at) < order_key(right->order_at);
        });
        for (const SeatButton* button : order) {
            kit::Control control;
            control.id = button->id;
            control.rect = local_of(
                kit::Rect{
                    button->notch_at.x,
                    button->notch_at.y,
                    button->notch_at.width,
                    button->notch_at.height
                },
                laid.bounds
            );
            control.name = button->name;
            control.kind = kit::ControlKind::button;
            control.text = button->label;
            list.controls.push_back(std::move(control));
            list.tab_order.push_back(button->id);
        }
        if (laid.card) {
            kit::add_hover_card(list, *laid.card, laid.card_look);
            mp::presence_card_controls(list, *laid.card, laid.card_look);
        }
        return list;
    }

    /// Returns the notches' hover, press and focus.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override {
        kit::Interaction shown_state;
        shown_state.hovered = control_id(hovered_slot_);
        shown_state.pressed = control_id(pressed_slot_);
        shown_state.focused = control_id(focused_slot_);
        shown_state.focus_shown = focused_slot_ >= 0;
        return shown_state;
    }

    /// Closes once the setup is gone, a match has started, a dialog is up or
    /// no brain is installed. Closes the menu whose seat is gone. Gives up a
    /// button whose row is gone, and gives up the focus once the screen's
    /// own focus is on a record again, so the two markers never show at
    /// once. Follows the pointer for the card, and the room's own press of
    /// a Player button, which the seat's button draws.
    ///
    /// @return close when it should leave; otherwise none
    LayerAnswer tick() override {
        if (!BrainSeatsAccess::shows(runtime_))
            return LayerAnswer::close;
        Geometry laid = geometry(view_);
        if (menu_.seat >= 0 && !has_button(laid, menu_.seat)) {
            menu_ = {};
            ++revision_;
        }
        if (focused_slot_ >= 0 && !has_button(laid, focused_slot_)) {
            focused_slot_ = -1;
            pressed_slot_ = -1;
            ++revision_;
        }
        if (focused_slot_ >= 0 && runtime_.frontend_focus() >= 0) {
            focused_slot_ = -1;
            pressed_slot_ = -1;
            ++revision_;
        }
        if (card_.open_slot() != mp::presence_card_no_slot && card_.open_slot() != menu_card_slot &&
            !has_button(laid, card_.open_slot()))
            card_.close();
        const int32_t card_before = card_.open_slot();
        follow_card(laid);
        if (card_.open_slot() != card_before)
            ++revision_;
        laid = geometry(view_);
        const std::string now = seal_of(laid);
        if (now != seal_) {
            seal_ = std::move(now);
            ++revision_;
        }
        return LayerAnswer::none;
    }

    /// Returns the screen's revision.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Closes the menu, when it is open.
    void close(bool /*by_key*/) override { menu_ = {}; }

  private:

    /// One seat's button.
    struct SeatButton {
        int32_t slot{-1};            ///< the slot, from 0, or the campaign's place
        kit::ControlId id{};         ///< its control id
        std::string name{};          ///< its automation name, without oa.
        bool campaign{};             ///< the campaign's own button, pressed anywhere
        int32_t gadget{-1};          ///< the room's Player record; -1 for the campaign's
        room::SourceRect rect{};     ///< the button, in frame pixels
        room::SourceRect notch{};    ///< its notch, in frame pixels
        place::PlaceRect notch_at{}; ///< what a press reaches, in the layer's points
        /// Where Tab orders the notch: its left edge on the button's top
        /// row, so that it comes just after the button, in the layer's points.
        place::PlaceRect order_at{};
        std::string caption{};     ///< the words on the button, in the room's bytes
        std::string label{};       ///< the seat's whole label, for automation
        picker::SeatShown shown{}; ///< what the card reads
        picker::Choice choice{};   ///< the seat's choice
    };

    /// Where the buttons, the records, the menu and the open card lie.
    struct Geometry {
        bool ok{};                               ///< a button was placed
        kit::Rect bounds{};                      ///< the screen's place, in the layer's points
        std::vector<SeatButton> buttons{};       ///< the seats that have a button
        std::vector<FocusRecord> focusable{};    ///< the records Tab can stop on
        std::vector<place::PlaceRect> gadgets{}; ///< every gadget, in the screen's points
        std::optional<kit::Rect> card{};         ///< the open card, in the screen's points
        std::optional<kit::Rect> arrow{};        ///< its arrow, in the screen's points
        kit::HoverCardLook card_look{};          ///< the open card's look
    };

    /// One stop in the combined Tab order: a record, or a seat's notch.
    struct Stop {
        bool button{};  ///< true for a seat's notch
        int32_t id{-1}; ///< the gadget's index, or the slot
        int64_t key{};  ///< its Tab key
    };

    /// The control id of a slot, or none.
    ///
    /// @param slot the slot; negative is none
    /// @return the id
    [[nodiscard]] static kit::ControlId control_id(int32_t slot) noexcept {
        if (slot < 0)
            return kit::no_control;
        return id_base + slot;
    }

    /// Tells whether NEWGAME or Any Mission is showing.
    ///
    /// @return true on those screens
    [[nodiscard]] bool campaign_screen() const noexcept {
        return runtime_.screen_ == Screen::new_campaign || runtime_.screen_ == Screen::any_mission;
    }

    /// The difficulty the Difficulty button is showing, else the preference.
    ///
    /// @return 0, 1 or 2, as the preference stores them
    [[nodiscard]] uint32_t difficulty_stage() const {
        const auto found = runtime_.widget_text_stages_.find("Difficulty");
        if (found != runtime_.widget_text_stages_.end() && found->second <= 2)
            return static_cast<uint32_t>(found->second);
        const uint32_t stored = runtime_.preferences_.difficulty;
        return stored <= 2 ? stored : static_cast<uint32_t>(OA_DIFFICULTY_MEDIUM);
    }

    /// The Difficulty button's word, as the room shows it.
    ///
    /// @param stage the button's stage
    /// @return the word; empty when the button is not on the screen
    [[nodiscard]] std::string difficulty_word(std::size_t stage) const {
        const auto& gadgets = runtime_.resources_.layout.gadgets;
        for (std::size_t at = 1; at < gadgets.size(); ++at) {
            if (!Runtime::tdf_names_equal(gadgets[at].common.name, "Difficulty"))
                continue;
            const auto* fields = std::get_if<gui::ButtonFields>(&gadgets[at].fields);
            if (fields == nullptr)
                break;
            if (std::optional<std::string> translated =
                    renderer::translated_stage_caption(*fields, stage))
                return *translated;
            return std::string(renderer::staged_caption(fields->text, stage));
        }
        return {};
    }

    /// What a computer seat's button shows, and its choice.
    ///
    /// The same rules as the seats a start plays: an empty brain is the
    /// built-in player on purpose, and a brain that is missing or unfit is
    /// the built-in player too. Strings are copied out of the listing.
    ///
    /// @param setup the setup the seat belongs to
    /// @param slot the slot
    /// @param difficulty the setup's difficulty
    /// @param word the Difficulty button's word
    /// @return the face and the choice
    [[nodiscard]] std::pair<picker::SeatShown, picker::Choice> face_of(
        brain::Setup setup, int32_t slot, brain::Difficulty difficulty, const std::string& word
    ) const {
        const brain::Seats& seats = runtime_.setup_brain_seats(setup);
        const brain::Seat* own = nullptr;
        for (const brain::Seat& seat : seats.seats) {
            if (seat.place == slot)
                own = &seat;
        }
        picker::SeatShown shown;
        picker::Choice choice;
        choice.level = level_id_of(difficulty);
        const auto built_in = [&]() {
            shown.level = word;
            choice.brain.clear();
            choice.level = level_id_of(difficulty);
            choice.options.clear();
            return std::pair<picker::SeatShown, picker::Choice>{shown, choice};
        };
        if (own != nullptr && own->brain.empty())
            return built_in();
        const std::string id = own != nullptr ? own->brain : runtime_.default_brain();
        if (id.empty())
            return built_in();
        const brain::Listing& listing = runtime_.installed_brains();
        const brain::InstalledBrain* installed = brain::find(listing, id);
        if (installed == nullptr)
            return built_in();
        const oa::data::content_bundles::ContentChoice content = runtime_.content_choice();
        library::Inputs probe;
        probe.engine = content::this_engine_rules().engine;
        probe.played = played_mod(runtime_.engine_settings_state());
        probe.content.enabled = content.expansions;
        const std::string unfit = library::brain_unfit_reason(
            probe, brain_facts_of(installed->manifest, installed->network_bytes)
        );
        if (!unfit.empty())
            return built_in();
        std::string level_id;
        if (own != nullptr)
            level_id = own->level;
        if (oabrain::find_level(installed->manifest, level_id) == nullptr)
            level_id = level_for(installed->manifest, difficulty);
        if (oabrain::find_level(installed->manifest, level_id) == nullptr)
            return built_in();
        const NamedBrain names = names_of(installed->manifest, level_id);
        shown.name = names.name;
        shown.version = oamut::version_text(installed->manifest.version);
        shown.level = names.level;
        shown.full_awareness = installed->manifest.full_awareness;
        choice.brain = installed->manifest.id;
        choice.level = level_id;
        if (own != nullptr)
            choice.options = own->options;
        return {std::move(shown), std::move(choice)};
    }

    /// The frame rectangle of a gadget of the room.
    ///
    /// @param common the gadget's fields
    /// @return its rectangle, in frame pixels
    [[nodiscard]] room::SourceRect frame_rect(const gui::CommonFields& common) const {
        const auto origin = runtime_.panel_origin();
        return {
            static_cast<int32_t>(origin.x) + common.x,
            static_cast<int32_t>(origin.y) + common.y,
            static_cast<int32_t>(common.width),
            static_cast<int32_t>(common.height)
        };
    }

    /// The art the room draws a gadget with: the sequence its run-time
    /// image names, else its own by name; null takes BUTTONS0.
    ///
    /// @param gadget the gadget
    /// @return the sequence, or null
    [[nodiscard]] const formats::gaf::Sequence* art_of(const gui::Gadget& gadget) const {
        const renderer::ScreenResources& resources = runtime_.resources_;
        const auto by_name = [](const formats::gaf::Archive& archive, std::string_view name) {
            const formats::gaf::Sequence* found = nullptr;
            for (const formats::gaf::Sequence& sequence : archive.sequences)
                if (found == nullptr && Runtime::tdf_names_equal(sequence.name, name) &&
                    !sequence.frames.empty())
                    found = &sequence;
            return found;
        };
        const auto sprite = runtime_.widget_sprites_.find(gadget.common.name);
        if (sprite != runtime_.widget_sprites_.end()) {
            const formats::gaf::Archive* archive = &resources.sprites;
            if (sprite->second.archive == renderer::SpriteArchive::shared)
                archive = &resources.shared_sprites;
            else if (sprite->second.archive == renderer::SpriteArchive::global)
                archive = &resources.global_sprites;
            return by_name(*archive, sprite->second.sequence);
        }
        if (const formats::gaf::Sequence* own = by_name(resources.sprites, gadget.common.name))
            return own;
        return by_name(resources.shared_sprites, gadget.common.name);
    }

    /// Puts the buttons' bounds, the open card and every gadget's place on a geometry.
    ///
    /// @param laid the buttons; unchanged while it has none
    /// @param placed_on what the screen is placed on
    /// @param gadgets every gadget, in the layer's points
    void complete_geometry(
        Geometry& laid, const LayerView& placed_on, std::vector<place::PlaceRect> gadgets
    ) const {
        if (laid.buttons.empty())
            return;
        kit::Rect bounds{};
        for (SeatButton& button : laid.buttons) {
            bounds = unite(bounds, frame_rect_points(placed_on, grown(button.rect, focus_margin)));
            const kit::Rect notch = frame_rect_points(placed_on, button.notch);
            const kit::Rect whole = frame_rect_points(placed_on, button.rect);
            button.notch_at = {notch.x, notch.y, notch.width, notch.height};
            button.order_at = {notch.x, whole.y, notch.width, notch.height};
        }
        if (menu_.seat >= 0)
            bounds = unite(bounds, frame_rect_points(placed_on, menu_.rect));
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        const int32_t open = card_.open_slot();
        if (fonts != nullptr && open == menu_card_slot && menu_.seat >= 0 && menu_.selected >= 0 &&
            static_cast<std::size_t>(menu_.selected) < menu_.rows.size()) {
            // Beside the menu, at the selected row's height, with no arrow.
            const BrainMenuRow& row = menu_.rows[static_cast<std::size_t>(menu_.selected)];
            if (row.kind == BrainMenuKind::brain || row.kind == BrainMenuKind::built_in) {
                laid.card_look = picker::seat_card(row.shown, text_hooks());
                laid.card_look.arrow = kit::Side::none;
                const kit::Point size = kit::hover_card_size(*fonts, laid.card_look);
                const kit::Rect list = frame_rect_points(placed_on, menu_.rect);
                const kit::Rect area = placed_on.frame.area;
                int32_t x = list.x + list.width + card_gap;
                if (x + size.x > area.x + area.width)
                    x = list.x - card_gap - size.x;
                x = std::max(x, area.x);
                const int32_t y =
                    std::clamp(list.y, area.y, std::max(area.y, area.y + area.height - size.y));
                const kit::Rect card{x, y, size.x, size.y};
                bounds = unite(bounds, card);
                laid.card = card;
            }
        } else if (fonts != nullptr && open != mp::presence_card_no_slot && menu_.seat < 0) {
            const SeatButton* open_button = nullptr;
            for (const SeatButton& button : laid.buttons) {
                if (button.slot == open)
                    open_button = &button;
            }
            if (open_button != nullptr) {
                const int32_t arrow_depth =
                    kit::metrics_of(placed_on.frame.size_class).hover_card_arrow;
                laid.card_look = picker::seat_card(open_button->shown, text_hooks());
                const kit::Point size = kit::hover_card_size(*fonts, laid.card_look);
                const kit::Rect badge = frame_rect_points(placed_on, open_button->rect);
                const kit::Rect card =
                    mp::presence_card_place(badge, size, placed_on.frame.area, arrow_depth);
                mp::presence_card_point(laid.card_look, card, badge);
                const kit::Rect arrow = arrow_rect(card, laid.card_look, arrow_depth);
                bounds = unite(bounds, card);
                if (arrow.width > 0 && arrow.height > 0) {
                    bounds = unite(bounds, arrow);
                    laid.arrow = arrow;
                }
                laid.card = card;
            }
        }
        if (laid.card)
            laid.card = local_of(*laid.card, bounds);
        if (laid.arrow)
            laid.arrow = local_of(*laid.arrow, bounds);
        laid.gadgets.reserve(gadgets.size());
        for (const place::PlaceRect& rect : gadgets)
            laid.gadgets.push_back({rect.x - bounds.x, rect.y - bounds.y, rect.width, rect.height});
        laid.bounds = bounds;
        laid.ok = bounds.width > 0 && bounds.height > 0 && !laid.buttons.empty();
    }

    /// Lists every gadget's place and the records Tab can stop on.
    ///
    /// @param laid the geometry the records go on
    /// @param placed_on what the screen is placed on
    /// @return every gadget, in the layer's points
    [[nodiscard]] std::vector<place::PlaceRect>
    room_records(Geometry& laid, const LayerView& placed_on) const {
        const auto& gadgets = runtime_.resources_.layout.gadgets;
        std::vector<place::PlaceRect> everything;
        everything.reserve(gadgets.size());
        for (std::size_t at = 1; at < gadgets.size(); ++at) {
            const gui::CommonFields& common = gadgets[at].common;
            if (common.width <= 0 || common.height <= 0)
                continue;
            const room::SourceRect frame = frame_rect(common);
            const kit::Rect points = frame_rect_points(placed_on, frame);
            if (points.width <= 0 || points.height <= 0)
                continue;
            const place::PlaceRect rect{points.x, points.y, points.width, points.height};
            everything.push_back(rect);
            if (!takes_focus(gadgets[at]))
                continue;
            FocusRecord record;
            record.index = static_cast<int32_t>(at);
            record.at = rect;
            record.frame_x = frame.x;
            record.frame_y = frame.y;
            laid.focusable.push_back(record);
        }
        return everything;
    }

    /// The campaign's one button, under Difficulty, in frame pixels.
    ///
    /// The button is the size of the gadget under Difficulty, centred in
    /// that column with a gap of 2. When that column has no room,
    /// place_block puts a button of the words' own size against Difficulty.
    ///
    /// @param placed_on what the screen is placed on
    /// @return the geometry; not ok while the button is not placed
    [[nodiscard]] Geometry campaign_geometry(const LayerView& placed_on) const {
        Geometry laid;
        if (!BrainSeatsAccess::shows(runtime_) || placed_on.picture_size.x <= 0)
            return laid;
        const auto& gadgets = runtime_.resources_.layout.gadgets;
        if (gadgets.size() < 2)
            return laid;
        std::vector<place::PlaceRect> everything = room_records(laid, placed_on);
        std::vector<place::PlaceRect> frames;
        frames.reserve(gadgets.size());
        place::PlaceRect anchor{};
        bool found = false;
        for (std::size_t at = 1; at < gadgets.size(); ++at) {
            const gui::CommonFields& common = gadgets[at].common;
            if (common.width <= 0 || common.height <= 0)
                continue;
            const place::PlaceRect rect = as_place(frame_rect(common));
            if (!found && Runtime::tdf_names_equal(common.name, "Difficulty")) {
                anchor = rect;
                found = true;
                continue;
            }
            frames.push_back(rect);
        }
        if (!found)
            return laid;
        const place::PlaceRect room_rect = as_place(room_of(placed_on));
        const uint32_t stage = difficulty_stage();
        const brain::Difficulty difficulty = difficulty_of(stage);
        const std::string word = difficulty_word(stage);
        const std::pair<picker::SeatShown, picker::Choice> face =
            face_of(brain::Setup::campaign, campaign_place, difficulty, word);
        const std::string caption =
            face.first.name.empty() ? runtime_.translate_ui("Computer") : face.first.name;
        std::optional<place::PlaceRect> chosen =
            place::place_in_column(anchor, frames, room_rect, column_gap);
        if (!chosen) {
            const room::RoomArt art = BrainSeatsAccess::screen_art(runtime_);
            const room::SourceRect stamp = room::room_stamp_rect({0, 0, 0, campaign_height});
            const int32_t width =
                stamp.x + stamp.width + 6 +
                room::room_text_width(art, room::RoomFont::caption, room::room_text(caption)) + 8 +
                notch_width + 4;
            const place::BlockSizes sizes{width, campaign_height, 0, 0, 0, block_gap, 0, 0};
            chosen = place::place_block(anchor, frames, room_rect, sizes).button;
        }
        if (!chosen || chosen->width <= 0 || chosen->height <= 0)
            return laid;
        SeatButton button;
        button.slot = campaign_place;
        button.id = control_id(campaign_place);
        button.name = std::string(campaign_button_name);
        button.campaign = true;
        button.rect = as_source(*chosen);
        button.notch = button.rect;
        button.shown = face.first;
        button.choice = face.second;
        button.caption = room::room_text(caption);
        button.label = picker::seat_label(face.first, false, text_hooks());
        laid.buttons.push_back(std::move(button));
        complete_geometry(laid, placed_on, std::move(everything));
        return laid;
    }

    /// Where the buttons, the menu and the open card lie for a view.
    ///
    /// @param placed_on what the screen is placed on
    /// @return the geometry; not ok while no button is placed
    [[nodiscard]] Geometry geometry(const LayerView& placed_on) const {
        if (campaign_screen())
            return campaign_geometry(placed_on);
        Geometry laid;
        if (!BrainSeatsAccess::shows(runtime_) || placed_on.picture_size.x <= 0)
            return laid;
        const auto& gadgets = runtime_.resources_.layout.gadgets;
        if (gadgets.size() < 2)
            return laid;
        const entry::SkirmishSettings& settings = runtime_.skirmish_settings_;
        const int32_t slot_limit =
            std::min(settings.slot_count, static_cast<int32_t>(settings.slots.size()));
        std::vector<place::PlaceRect> everything = room_records(laid, placed_on);
        const uint32_t stage = difficulty_stage();
        const brain::Difficulty difficulty = difficulty_of(stage);
        const std::string word = difficulty_word(stage);
        const library::TextHooks hooks = text_hooks();
        for (int32_t slot = 0; slot < slot_limit; ++slot) {
            if (settings.slots[static_cast<std::size_t>(slot)].controller !=
                entry::controller::computer)
                continue;
            const std::string player_name = "Player" + std::to_string(slot);
            int32_t gadget = -1;
            for (std::size_t at = 1; at < gadgets.size() && gadget < 0; ++at) {
                const gui::CommonFields& common = gadgets[at].common;
                if (common.width > notch_width * 2 && common.height > 8 &&
                    Runtime::tdf_names_equal(common.name, player_name))
                    gadget = static_cast<int32_t>(at);
            }
            if (gadget < 0)
                continue;
            const gui::Gadget& player = gadgets[static_cast<std::size_t>(gadget)];
            SeatButton button;
            button.slot = slot;
            button.id = control_id(slot);
            button.name = "brain-seat." + std::to_string(slot);
            button.gadget = gadget;
            button.rect = frame_rect(player.common);
            button.notch = room::room_notch_rect(button.rect, notch_width);
            const std::pair<picker::SeatShown, picker::Choice> face =
                face_of(brain::Setup::skirmish, slot, difficulty, word);
            button.shown = face.first;
            button.choice = face.second;
            if (face.first.name.empty()) {
                // The built-in player keeps the room's own word.
                if (const auto* fields = std::get_if<gui::ButtonFields>(&player.fields))
                    button.caption = fields->text;
            } else {
                button.caption = room::room_text(face.first.name);
            }
            button.label = picker::seat_label(button.shown, false, hooks);
            laid.buttons.push_back(std::move(button));
        }
        complete_geometry(laid, placed_on, std::move(everything));
        return laid;
    }

    /// Draws one seat's button on a canvas of room parts: the plate in the
    /// room's art with the seat's words, the notch, the OA stamp on the
    /// campaign's button, and the room's focus marker while it holds the focus.
    ///
    /// @param parts the canvas
    /// @param art the room's art
    /// @param button the button
    void
    draw_button(room::RoomCanvas& parts, const room::RoomArt& art, const SeatButton& button) const {
        room::RoomButtonLook look;
        look.rect = button.rect;
        look.caption = button.caption;
        look.narrower_font = true;
        const room::SourceRect notch = room::room_notch_rect(button.rect, notch_width);
        look.inset_right = notch_width + 2;
        const bool held_here = pressed_slot_ == button.slot && hovered_slot_ == button.slot;
        if (button.campaign) {
            const room::SourceRect stamp = room::room_stamp_rect(button.rect);
            look.inset_left = stamp.x - button.rect.x + stamp.width + 2;
            look.condition =
                held_here ? room::ButtonCondition::pressed : room::ButtonCondition::normal;
            room::draw_room_button(parts, art, look);
            room::draw_room_stamp(parts, art, stamp);
        } else {
            const gui::Gadget& player =
                runtime_.resources_.layout.gadgets[static_cast<std::size_t>(button.gadget)];
            look.art = art_of(player);
            // The room's own press of the Player button shows on it.
            if (runtime_.selected_ == button.gadget &&
                runtime_.hovered_ == static_cast<std::size_t>(button.gadget))
                look.condition = room::ButtonCondition::pressed;
            room::draw_room_button(parts, art, look);
        }
        room::draw_room_notch(parts, art, notch, menu_.seat == button.slot, held_here);
        if (focused_slot_ == button.slot)
            room::draw_room_focus(parts, art, button.campaign ? button.rect : notch);
    }

    /// A text that changes when the buttons, the focus, the menu, the card
    /// or the room's own press of a Player button change.
    ///
    /// @param laid the geometry
    /// @return the text
    [[nodiscard]] std::string seal_of(const Geometry& laid) const {
        std::string text = std::to_string(focused_slot_);
        text += '/';
        text += std::to_string(card_.open_slot());
        text += '/';
        text += std::to_string(menu_.seat);
        text += ':';
        text += std::to_string(menu_.selected);
        text += menu_.every ? "e" : "-";
        for (const BrainMenuRow& row : menu_.rows) {
            text += row.level_id;
            text += row.current ? "*" : ".";
        }
        for (const SeatButton& button : laid.buttons) {
            text += '|';
            text += std::to_string(button.slot);
            text += button.label;
            text += button.caption;
            text += std::to_string(button.notch_at.x);
            text += std::to_string(button.notch_at.y);
            text += std::to_string(button.notch_at.width);
            text += std::to_string(button.notch_at.height);
            if (button.gadget >= 0 && runtime_.selected_ == button.gadget &&
                runtime_.hovered_ == static_cast<std::size_t>(button.gadget))
                text += 'p';
        }
        if (laid.card) {
            text += "#";
            text += std::to_string(laid.card->x);
            text += std::to_string(laid.card->y);
            text += std::to_string(laid.card->width);
            text += std::to_string(laid.card->height);
        }
        return text;
    }

    /// The slot whose notch, or campaign button, holds a frame pixel, or -1.
    ///
    /// @param laid the geometry
    /// @param at the pixel
    /// @return the slot
    [[nodiscard]] static int32_t notch_at(const Geometry& laid, kit::Point at) noexcept {
        for (const SeatButton& button : laid.buttons) {
            if (holds_pixel(button.notch, at))
                return button.slot;
        }
        return -1;
    }

    /// The slot whose button holds a frame pixel, or -1.
    ///
    /// @param laid the geometry
    /// @param at the pixel
    /// @return the slot
    [[nodiscard]] static int32_t button_at(const Geometry& laid, kit::Point at) noexcept {
        for (const SeatButton& button : laid.buttons) {
            if (holds_pixel(button.rect, at))
                return button.slot;
        }
        return -1;
    }

    /// Tells whether the open card or its arrow holds a point.
    ///
    /// @param laid the geometry
    /// @param at the point, in the screen's points
    /// @return true when it does
    [[nodiscard]] static bool card_contains(const Geometry& laid, kit::Point at) noexcept {
        if (laid.card && kit::contains(*laid.card, at))
            return true;
        return laid.arrow && kit::contains(*laid.arrow, at);
    }

    /// Tells whether a slot has a button.
    ///
    /// @param laid the geometry
    /// @param slot the slot
    /// @return true when it does
    [[nodiscard]] static bool has_button(const Geometry& laid, int32_t slot) noexcept {
        return button_of(laid, slot) != nullptr;
    }

    /// The button of a slot, or null.
    ///
    /// @param laid the geometry
    /// @param slot the slot
    /// @return the button
    [[nodiscard]] static const SeatButton* button_of(const Geometry& laid, int32_t slot) noexcept {
        for (const SeatButton& button : laid.buttons) {
            if (button.slot == slot)
                return &button;
        }
        return nullptr;
    }

    /// Tells whether a finger's reach lands on a notch.
    ///
    /// A gadget nearer the point, or one the point lies inside, keeps it.
    ///
    /// @param laid where the notches and the gadgets lie
    /// @param at the finger, in the screen's points
    /// @param finger_reach how far the finger reaches, in points
    /// @return the slot, or -1
    [[nodiscard]] int32_t
    finger_slot(const Geometry& laid, kit::Point at, int32_t finger_reach) const {
        if (finger_reach <= 0)
            return -1;
        const kit::DisplayList list = display_list();
        for (const SeatButton& button : laid.buttons) {
            const place::PlaceRect local{
                button.notch_at.x - laid.bounds.x,
                button.notch_at.y - laid.bounds.y,
                button.notch_at.width,
                button.notch_at.height
            };
            if (local.width <= 0 || local.height <= 0)
                continue;
            if (!button.campaign) {
                // Inside the Player button, the rest of it is the room's.
                const kit::Rect player =
                    local_of(frame_rect_points(view_, button.rect), laid.bounds);
                if (kit::contains(player, at) &&
                    !kit::contains(kit::Rect{local.x, local.y, local.width, local.height}, at))
                    continue;
            }
            if (place::nearer_gadget(at.x, at.y, local, laid.gadgets))
                continue;
            kit::Interaction trial = interaction();
            if (kit::finger_down(trial, list, at, finger_reach).control == button.id)
                return button.slot;
        }
        return -1;
    }

    /// Follows the pointer and the focus for the card. While the menu is
    /// open, the card is the selected row's, at once.
    ///
    /// The pointer is kept in the layer's points, so a card opening, which
    /// moves the screen's origin, still finds the button.
    ///
    /// @param laid the geometry
    void follow_card(const Geometry& laid) {
        if (menu_.seat >= 0) {
            const bool shows_card = menu_.selected >= 0 &&
                                    static_cast<std::size_t>(menu_.selected) < menu_.rows.size() &&
                                    (menu_.rows[static_cast<std::size_t>(menu_.selected)].kind ==
                                         BrainMenuKind::brain ||
                                     menu_.rows[static_cast<std::size_t>(menu_.selected)].kind ==
                                         BrainMenuKind::built_in);
            if (!shows_card)
                card_.close();
            else if (card_.open_slot() != menu_card_slot) {
                card_.close();
                (void)card_.key_info(menu_card_slot);
            }
            return;
        }
        if (card_.open_slot() == menu_card_slot)
            card_.close();
        int32_t slot = mp::presence_card_no_slot;
        bool over = false;
        if (laid.ok && have_pointer_) {
            const kit::Point local{
                pointer_layer_.x - laid.bounds.x, pointer_layer_.y - laid.bounds.y
            };
            slot = button_at(laid, frame_point(view_, pointer_layer_));
            over = card_contains(laid, local);
        }
        card_.over_card(over);
        (void)card_.pointer(slot, static_cast<uint64_t>(SDL_GetTicks()));
        const int32_t focused = focused_slot_ >= 0 ? focused_slot_ : mp::presence_card_no_slot;
        (void)card_.focus(focused);
    }

    /// The combined Tab order: records and notches, key ascending, a record
    /// before a notch that shares its key.
    ///
    /// @param laid the geometry
    /// @return the stops
    [[nodiscard]] static std::vector<Stop> stops_of(const Geometry& laid) {
        std::vector<Stop> stops;
        stops.reserve(laid.focusable.size() + laid.buttons.size());
        for (const FocusRecord& record : laid.focusable)
            stops.push_back(Stop{false, record.index, order_key(record.at)});
        for (const SeatButton& button : laid.buttons)
            stops.push_back(Stop{true, button.slot, order_key(button.order_at)});
        std::sort(stops.begin(), stops.end(), [](const Stop& left, const Stop& right) {
            if (left.key != right.key)
                return left.key < right.key;
            return !left.button && right.button;
        });
        return stops;
    }

    /// Takes the focus onto a notch and clears the screen's own marker.
    ///
    /// @param slot the slot
    /// @return redraw
    LayerAnswer take_focus(int32_t slot) {
        focused_slot_ = slot;
        pressed_slot_ = -1;
        if (runtime_.frontend_focus() != -1)
            runtime_.set_frontend_focus(-1);
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Hands the focus to a record and clears the notch's marker.
    ///
    /// @param gadget the record
    /// @return redraw
    LayerAnswer hand_focus(int32_t gadget) {
        focused_slot_ = -1;
        runtime_.set_frontend_focus(gadget);
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Hands the focus to one stop.
    ///
    /// @param stop the stop
    /// @return redraw, or none when a record cannot take it
    LayerAnswer take_stop(const Stop& stop) {
        if (stop.button)
            return take_focus(stop.id);
        if (stop.id > 0)
            return hand_focus(stop.id);
        return LayerAnswer::none;
    }

    /// The notch of the computer row above or below, when there is one.
    ///
    /// @param laid the geometry
    /// @param slot the focused slot
    /// @param down true for the row below
    /// @return its slot, or -1
    [[nodiscard]] static int32_t neighbour_slot(const Geometry& laid, int32_t slot, bool down) {
        const SeatButton* current = button_of(laid, slot);
        if (current == nullptr)
            return -1;
        int32_t found = -1;
        int32_t best_y = 0;
        for (const SeatButton& button : laid.buttons) {
            if (button.slot == slot)
                continue;
            const bool that_way = down ? button.notch_at.y > current->notch_at.y
                                       : button.notch_at.y < current->notch_at.y;
            if (!that_way)
                continue;
            const bool closer =
                found < 0 || (down ? button.notch_at.y < best_y : button.notch_at.y > best_y);
            if (!closer)
                continue;
            found = button.slot;
            best_y = button.notch_at.y;
        }
        return found;
    }

    /// Answers one key, for the keyboard and for the pad actions that become keys.
    ///
    /// @param pressed the key
    /// @return none or redraw when the screen takes it; pass otherwise
    LayerAnswer answer_key(kit::Key pressed) {
        const Geometry laid = geometry(view_);
        if (!laid.ok)
            return LayerAnswer::pass;
        const int32_t current = runtime_.frontend_focus();
        if (focused_slot_ < 0) {
            if (pressed == kit::Key::tab || pressed == kit::Key::back_tab) {
                for (const SeatButton& button : laid.buttons) {
                    if (pressed == kit::Key::tab) {
                        const int32_t before = record_before(laid.focusable, button.order_at);
                        if (current == before && before > 0)
                            return take_focus(button.slot);
                    }
                    if (pressed == kit::Key::back_tab) {
                        const int32_t after = record_after(laid.focusable, button.order_at);
                        if (current == after && after > 0)
                            return take_focus(button.slot);
                    }
                }
            }
            return LayerAnswer::pass;
        }
        if (pressed == kit::Key::enter || pressed == kit::Key::space) {
            open_menu(focused_slot_);
            return LayerAnswer::redraw;
        }
        const std::vector<Stop> stops = stops_of(laid);
        std::size_t at = stops.size();
        for (std::size_t found = 0; found < stops.size(); ++found) {
            if (stops[found].button && stops[found].id == focused_slot_)
                at = found;
        }
        if ((pressed == kit::Key::tab || pressed == kit::Key::back_tab) && at < stops.size() &&
            !stops.empty()) {
            const std::size_t next = pressed == kit::Key::tab
                                         ? (at + 1 < stops.size() ? at + 1 : 0)
                                         : (at > 0 ? at - 1 : stops.size() - 1);
            return take_stop(stops[next]);
        }
        const SeatButton* current_button = button_of(laid, focused_slot_);
        if (current_button == nullptr)
            return LayerAnswer::pass;
        // NEWGAME and Any Mission keep Up and Down for the campaign lists.
        if (campaign_screen() && (pressed == kit::Key::up || pressed == kit::Key::down))
            return LayerAnswer::pass;
        if (pressed == kit::Key::left && current_button->gadget > 0)
            return hand_focus(current_button->gadget);
        if (pressed == kit::Key::left || pressed == kit::Key::right) {
            const Aim aim = pressed == kit::Key::left ? Aim::left : Aim::right;
            const int32_t next = nearest_record(laid.focusable, current_button->notch_at, aim);
            return next > 0 ? hand_focus(next) : LayerAnswer::none;
        }
        if (pressed == kit::Key::up || pressed == kit::Key::down) {
            const int32_t neighbour =
                neighbour_slot(laid, focused_slot_, pressed == kit::Key::down);
            if (neighbour >= 0)
                return take_focus(neighbour);
            const Aim aim = pressed == kit::Key::down ? Aim::down : Aim::up;
            const int32_t next = nearest_record(laid.focusable, current_button->notch_at, aim);
            return next > 0 ? hand_focus(next) : LayerAnswer::none;
        }
        return LayerAnswer::pass;
    }

    /// Takes the pointer while the menu is open: a move selects the row
    /// under it, a press on a row answers it, and a press outside closes
    /// the menu. Every press is taken.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param pixel the pointer, in frame pixels
    /// @return redraw when it changed what is drawn; none otherwise
    LayerAnswer menu_pointer(ScreenInputKind kind, uint8_t button, kit::Point pixel) {
        const room::RoomArt art = BrainSeatsAccess::screen_art(runtime_);
        if (kind == ScreenInputKind::pointer_move) {
            const int32_t row = brain_menu_row_at(art, menu_, pixel);
            if (row >= 0 && row != menu_.selected &&
                choosable(menu_.rows[static_cast<std::size_t>(row)])) {
                menu_.selected = row;
                ++revision_;
                follow_card(geometry(view_));
                return LayerAnswer::redraw;
            }
            return LayerAnswer::none;
        }
        if (kind == ScreenInputKind::pointer_down) {
            menu_press_ = button;
            return LayerAnswer::none;
        }
        if (kind == ScreenInputKind::pointer_up && menu_press_ == button) {
            menu_press_ = 0;
            // A right press on the seat's own button only opened the menu.
            const BrainMenuAnswer answer = brain_menu_press(art, menu_, pixel);
            apply(answer);
            ++revision_;
            return LayerAnswer::redraw;
        }
        return LayerAnswer::none;
    }

    /// Opens the brain menu on one seat, under its button.
    ///
    /// @param slot the slot
    void open_menu(int32_t slot) {
        const Geometry laid = geometry(view_);
        const SeatButton* button = button_of(laid, slot);
        if (button == nullptr)
            return;
        const bool campaign = campaign_screen();
        const brain::Difficulty difficulty = difficulty_of(difficulty_stage());
        menu_ = BrainSeatsAccess::menu(
            runtime_,
            slot,
            button->choice,
            difficulty,
            difficulty_word(difficulty_stage()),
            {},
            campaign ? brain_menu_words::campaign_heading : brain_menu_words::seat_heading,
            !campaign,
            true
        );
        place_brain_menu(
            BrainSeatsAccess::screen_art(runtime_), menu_, button->rect, room_of(view_)
        );
        menu_press_ = 0;
        card_.close();
        ++revision_;
    }

    /// Does what the menu asked: writes the seat or every computer seat,
    /// closes, or leaves for the Library.
    ///
    /// @param answer the menu's answer
    void apply(const BrainMenuAnswer& answer) {
        const int32_t seat = menu_.seat;
        const bool campaign = campaign_screen();
        const auto write = [&](const picker::Choice& choice, bool every) {
            if (campaign)
                BrainSeatsAccess::write_campaign(runtime_, choice);
            else
                BrainSeatsAccess::write(runtime_, seat, choice, every);
        };
        switch (answer.kind) {
        case BrainMenuAnswer::Kind::none:
            return;
        case BrainMenuAnswer::Kind::close:
            menu_ = {};
            card_.close();
            return;
        case BrainMenuAnswer::Kind::choose:
            write(answer.choice, answer.every);
            menu_ = {};
            card_.close();
            return;
        case BrainMenuAnswer::Kind::level:
        case BrainMenuAnswer::Kind::every: {
            write(answer.choice, answer.every);
            // The menu reads the seat again and keeps its place.
            const int32_t selected = menu_.selected;
            const bool every = menu_.every;
            open_menu(seat);
            menu_.selected = selected;
            menu_.every = every;
            return;
        }
        case BrainMenuAnswer::Kind::get_brains:
            menu_ = {};
            card_.close();
            BrainSeatsAccess::get_more_brains(runtime_);
            return;
        }
    }

    Runtime& runtime_;               ///< the runtime whose setup it shows over
    LayerView view_{};               ///< the view it was last laid out for
    mp::PresenceCardTrigger card_{}; ///< when a seat's card opens and closes
    BrainMenu menu_{};               ///< the brain menu; closed while its seat is -1
    uint8_t menu_press_{};     ///< the button a press on the open menu is held with; 0 for none
    int32_t hovered_slot_{-1}; ///< the button the pointer is over; -1 for none
    int32_t pressed_slot_{-1}; ///< the notch a left press is held on; -1 for none
    int32_t right_pressed_slot_{-1}; ///< the button a right press is held on; -1 for none
    int32_t focused_slot_{-1};       ///< the notch the keyboard focus is on; -1 for none
    bool have_pointer_{};            ///< the pointer has been seen
    kit::Point pointer_layer_{};     ///< the pointer, in the layer's points
    bool card_pressed_{};            ///< a press started on the open card
    std::string seal_{};             ///< the buttons, the menu and the card last drawn
    uint64_t revision_{};            ///< counts the changes to how it looks
};

void Runtime::keep_brain_seats_screen() {
    OaLayer& layer = oa_layer();
    const bool library_open = layer.find(library_screen_name) != nullptr;
    if (library_was_open_ && !library_open)
        reload_installed_brains();
    library_was_open_ = library_open;
    if (!BrainSeatsAccess::shows(*this) || layer.find(screen_name) != nullptr)
        return;
    layer.push(std::make_unique<BrainSeatsScreen>(*this));
}

} // namespace oa::app
