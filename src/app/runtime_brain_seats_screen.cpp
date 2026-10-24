// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One brain button on each computer row of SKIRMISH, to the right of that
// row's Energy button. The button reads the seat's label, cut with … to the
// room it has, and opens that seat's picker. Tab, the arrows and the
// controller reach it. Resting the pointer, or the information key, shows
// the seat's card. With no brain installed the screen is not there.

#include "oa_button_focus.hpp"
#include "oa_layer.hpp"
#include "library_host.hpp"
#include "engine_settings_state.hpp"

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
#include "oa/ui/gui_layout.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/pad_controls.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstdint>
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

/// Reads the runtime fields the seat screen's helpers need, and writes a
/// choice onto computer seats. The helpers are not members.
struct BrainSeatsAccess {
    /// Tells whether the seat buttons show.
    ///
    /// @param runtime the runtime
    /// @return true on SKIRMISH, with no match, no dialog, fonts, and a brain installed
    static bool shows(Runtime& runtime) {
        if (runtime.screen_ != Screen::skirmish)
            return false;
        if (runtime.match_ != nullptr)
            return false;
        if (oa::ui::frontend_dialogs::dialog_count() != 0)
            return false;
        if (runtime.oa_layer().screen_fonts() == nullptr)
            return false;
        return !runtime.installed_brains().brains.empty();
    }

    /// Writes a choice onto one computer seat, or onto every computer seat.
    ///
    /// A slot that is not a computer seat is left as it is, including a seat
    /// stored for a slot that is no longer a computer.
    ///
    /// @param runtime the runtime
    /// @param slot the seat the picker was opened on
    /// @param choice the choice USE THIS handed
    /// @param every true when the choice is for every computer seat
    static void write(Runtime& runtime, int32_t slot, const picker::Choice& choice, bool every);
};

namespace {

constexpr std::string_view screen_name = "brain-seats";
constexpr std::string_view word_seat = "Seat {n}";
/// The gap after Energy, and before the rectangle that limits the button, in points.
constexpr int32_t beside_gap = 4;
/// A button narrower than this is left out, in points.
constexpr int32_t least_width = 48;
/// The button's width when nothing limits it, in points.
constexpr int32_t most_width = 160;
/// Control ids start here, clear of a hover card's line ids.
constexpr int32_t id_base = 100;

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
    for (std::size_t at = 0; at < mods.mod_folders.size() && at < mods.mod_names.size() &&
                             at < mods.mod_details.size();
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

/// The seat's name as a player reads it: "Seat 2" for slot 1.
///
/// @param slot the slot, from 0
/// @return the name, in the language shown
std::string seat_title(int32_t slot) {
    std::string text(oa::data::languages::interface_text(word_seat));
    constexpr std::string_view holder = "{n}";
    if (const std::size_t at = text.find(holder); at != std::string::npos)
        text.replace(at, holder.size(), std::to_string(slot + 1));
    return text;
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
/// The manifest's own strings fill a name no translation gave.
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
    if (named.level.empty()) {
        if (const oabrain::Level* level = oabrain::find_level(manifest, level_id))
            named.level = level->name;
    }
    return named;
}

} // namespace

void BrainSeatsAccess::write(
    Runtime& runtime, int32_t slot, const picker::Choice& choice, bool every
) {
    brain::Seats& seats = runtime.setup_brain_seats(brain::Setup::skirmish);
    const entry::SkirmishSettings& settings = runtime.skirmish_settings_;
    if (!every) {
        write_one(seats, settings, slot, choice);
        return;
    }
    const int32_t limit = std::min(settings.slot_count, static_cast<int32_t>(settings.slots.size()));
    for (int32_t at = 0; at < limit; ++at)
        write_one(seats, settings, at, choice);
}

/// The computer-seat buttons over SKIRMISH. Not modal, no backdrop, not
/// opaque, so the picture shows round them. A button takes its own press
/// and the focus handed to it. Y stays MUTATORS…'s.
struct Runtime::BrainSeatsScreen final : LayerScreen {
    /// Makes the screen.
    ///
    /// @param runtime the runtime
    explicit BrainSeatsScreen(Runtime& runtime) : runtime_(runtime) {}

    /// Returns "brain-seats".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return screen_name; }

    /// Returns the buttons, the focus ring's margin and the open card, in the window.
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

    /// Draws the buttons, the open card and, while a button holds the focus, its ring.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override { kit::paint(canvas, display_list()); }

    /// Takes a press on a button or the open card. A press anywhere else
    /// clears the focus and goes on.
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
        const int32_t over_mouse = button_at(laid, at);
        const int32_t over = kind != ScreenInputKind::pointer_move && finger_reach > 0
                                 ? finger_slot(laid, at, finger_reach)
                                 : over_mouse;
        if (over_mouse != hovered_slot_) {
            hovered_slot_ = over_mouse;
            ++revision_;
        }
        const int32_t card_before = card_.open_slot();
        follow_card(laid);
        if (card_.open_slot() != card_before)
            ++revision_;
        const bool on_card = card_contains(laid, at);
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
                    open_picker(pressed);
                return LayerAnswer::redraw;
            }
        }
        return LayerAnswer::pass;
    }

    /// Takes the information key, Escape when a card is open, and the keys
    /// a focused button holds.
    ///
    /// @param pressed the key
    /// @return none or redraw when the screen takes it; pass otherwise
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        if (pressed == kit::Key::info) {
            const int32_t focused =
                focused_slot_ >= 0 ? focused_slot_ : mp::presence_card_no_slot;
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

    /// Returns the buttons, the open card and the ring.
    ///
    /// A button's automation name is brain-seat.<slot>, which the endpoint
    /// lists as oa.brain-seat.<slot>, and its text is the whole label. The
    /// drawn caption is that label cut to the button. Tab's order holds the
    /// buttons. The card's lines are not focusable.
    ///
    /// @return the display list
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list;
        const Geometry laid = geometry(view_);
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        if (!laid.ok || fonts == nullptr)
            return list;
        std::vector<const SeatButton*> order;
        order.reserve(laid.buttons.size());
        for (const SeatButton& button : laid.buttons)
            order.push_back(&button);
        std::sort(order.begin(), order.end(), [](const SeatButton* left, const SeatButton* right) {
            return order_key(left->at) < order_key(right->at);
        });
        for (const SeatButton* button : order) {
            kit::ButtonLook look;
            look.style = kit::ButtonStyle::filled;
            look.enabled = true;
            look.hovered = hovered_slot_ == button->slot;
            look.held = pressed_slot_ == button->slot && hovered_slot_ == button->slot;
            look.oa_mark = true;
            fit_caption(look, *fonts, button->local, button->label);
            kit::add_button(list, button->local, look, button->id, button->name);
            if (!list.controls.empty())
                list.controls.back().text = button->label;
            list.tab_order.push_back(button->id);
            if (focused_slot_ == button->slot)
                kit::add_focus_ring(list, button->local, true);
        }
        if (laid.card) {
            kit::add_hover_card(list, *laid.card, laid.card_look);
            mp::presence_card_controls(list, *laid.card, laid.card_look);
        }
        return list;
    }

    /// Returns the buttons' hover, press and focus.
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

    /// Closes once SKIRMISH is gone, a match has started, a dialog is up or
    /// no brain is installed. Gives up a button whose row is gone, and gives
    /// up the focus once the screen's own focus is on a record again, so the
    /// two rings never show at once. Follows the pointer for the card.
    ///
    /// @return close when it should leave; otherwise none
    LayerAnswer tick() override {
        if (!BrainSeatsAccess::shows(runtime_))
            return LayerAnswer::close;
        Geometry laid = geometry(view_);
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
        if (card_.open_slot() != mp::presence_card_no_slot && !has_button(laid, card_.open_slot()))
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

    /// Has nothing of its own to close. The picker, when it is open, is its own screen.
    void close(bool /*by_key*/) override {}

  private:

    /// One computer row's button, in the layer's points and the screen's.
    struct SeatButton {
        int32_t slot{-1};          ///< the slot, from 0
        kit::ControlId id{};       ///< its control id
        std::string name{};        ///< its automation name, without oa.
        place::PlaceRect at{};    ///< the button, in the layer's points
        kit::Rect local{};         ///< the button, in the screen's points
        int32_t energy{-1};        ///< its row's Energy gadget
        std::string label{};       ///< the whole label, uncut
        picker::SeatShown shown{}; ///< what the card reads
        picker::Choice choice{};   ///< the choice the picker opens on
    };

    /// Where the buttons, the records and the open card lie.
    struct Geometry {
        bool ok{};                              ///< a button was placed
        kit::Rect bounds{};                     ///< the screen's place, in the layer's points
        std::vector<SeatButton> buttons{};     ///< the computer rows that have a button
        std::vector<FocusRecord> focusable{};  ///< the records Tab can stop on
        std::vector<place::PlaceRect> gadgets{}; ///< every gadget, in the screen's points
        std::optional<kit::Rect> card{};        ///< the open card, in the screen's points
        std::optional<kit::Rect> arrow{};       ///< its arrow, in the screen's points
        kit::HoverCardLook card_look{};         ///< the open card's look
    };

    /// One stop in the combined Tab order: a record, or a seat button.
    struct Stop {
        bool button{};  ///< true for a seat button
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

    /// The Difficulty button's word, as SKIRMISH shows it.
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
            if (std::optional<std::string> translated = renderer::translated_stage_caption(*fields, stage))
                return *translated;
            return std::string(renderer::staged_caption(fields->text, stage));
        }
        return {};
    }

    /// What a computer seat's button shows, and the choice the picker opens on.
    ///
    /// The same rules as the seats a start plays: an empty brain is the
    /// built-in player on purpose, and a brain that is missing or unfit is
    /// the built-in player too. Strings are copied out of the listing.
    ///
    /// @param slot the slot
    /// @param difficulty the setup's difficulty
    /// @param word the Difficulty button's word
    /// @return the face and the choice
    [[nodiscard]] std::pair<picker::SeatShown, picker::Choice>
    face_of(int32_t slot, brain::Difficulty difficulty, const std::string& word) const {
        const brain::Seats& seats = runtime_.setup_brain_seats(brain::Setup::skirmish);
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

    /// Where the buttons and the open card lie for a view.
    ///
    /// @param placed_on what the screen is placed on
    /// @return the geometry; not ok while no button is placed
    [[nodiscard]] Geometry geometry(const LayerView& placed_on) const {
        Geometry laid;
        if (!BrainSeatsAccess::shows(runtime_) || placed_on.picture_size.x <= 0)
            return laid;
        const auto& gadgets = runtime_.resources_.layout.gadgets;
        if (gadgets.size() < 2)
            return laid;
        const auto origin = runtime_.panel_origin();
        const entry::SkirmishSettings& settings = runtime_.skirmish_settings_;
        const int32_t slot_limit =
            std::min(settings.slot_count, static_cast<int32_t>(settings.slots.size()));
        std::vector<place::PlaceRect> everything;
        everything.reserve(gadgets.size());
        for (std::size_t at = 1; at < gadgets.size(); ++at) {
            const gui::CommonFields& common = gadgets[at].common;
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
            const place::PlaceRect rect = as_place(points);
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
        const kit::Rect room_frame{0, 0, placed_on.picture_size.x, placed_on.picture_size.y};
        const kit::Rect room = frame_to_points(placed_on, room_frame);
        const uint32_t stage = difficulty_stage();
        const brain::Difficulty difficulty = difficulty_of(stage);
        const std::string word = difficulty_word(stage);
        const library::TextHooks hooks = text_hooks();
        const int32_t height = kit::compact_metrics.button_height;
        for (int32_t slot = 0; slot < slot_limit; ++slot) {
            if (settings.slots[static_cast<std::size_t>(slot)].controller !=
                entry::controller::computer)
                continue;
            const std::string energy_name = "Energy" + std::to_string(slot);
            place::PlaceRect anchor{};
            int32_t energy = -1;
            bool found = false;
            for (std::size_t at = 1; at < gadgets.size(); ++at) {
                const gui::CommonFields& common = gadgets[at].common;
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
                if (!found && Runtime::tdf_names_equal(common.name, energy_name)) {
                    anchor = as_place(points);
                    energy = static_cast<int32_t>(at);
                    found = true;
                    break;
                }
            }
            if (!found)
                continue;
            std::vector<place::PlaceRect> others;
            others.reserve(everything.size());
            for (const place::PlaceRect& rect : everything) {
                if (rect.x == anchor.x && rect.y == anchor.y && rect.width == anchor.width &&
                    rect.height == anchor.height)
                    continue;
                others.push_back(rect);
            }
            const std::optional<place::PlaceRect> beside = place::place_beside(
                anchor, others, as_place(room), beside_gap, height, least_width, most_width
            );
            if (!beside)
                continue;
            SeatButton button;
            button.slot = slot;
            button.id = control_id(slot);
            button.name = "brain-seat." + std::to_string(slot);
            button.at = *beside;
            button.energy = energy;
            const std::pair<picker::SeatShown, picker::Choice> face =
                face_of(slot, difficulty, word);
            button.shown = face.first;
            button.choice = face.second;
            button.label = picker::seat_label(button.shown, false, hooks);
            laid.buttons.push_back(std::move(button));
        }
        if (laid.buttons.empty())
            return laid;
        kit::Rect bounds{};
        for (const SeatButton& button : laid.buttons)
            bounds = unite(bounds, outset(as_kit(button.at), kit::compact_metrics.focus_inset));
        const int32_t open = card_.open_slot();
        const SeatButton* open_button = nullptr;
        for (const SeatButton& button : laid.buttons) {
            if (button.slot == open)
                open_button = &button;
        }
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        if (open_button != nullptr && fonts != nullptr) {
            const int32_t arrow_depth = kit::metrics_of(placed_on.frame.size_class).hover_card_arrow;
            laid.card_look = picker::seat_card(open_button->shown, hooks);
            const kit::Point size = kit::hover_card_size(*fonts, laid.card_look);
            const kit::Rect badge = as_kit(open_button->at);
            const kit::Rect card =
                mp::presence_card_place(badge, size, placed_on.frame.area, arrow_depth);
            mp::presence_card_point(laid.card_look, card, badge);
            const kit::Rect arrow = arrow_rect(card, laid.card_look, arrow_depth);
            bounds = unite(bounds, card);
            if (arrow.width > 0 && arrow.height > 0)
                bounds = unite(bounds, arrow);
            laid.card = local_of(card, bounds);
            if (arrow.width > 0 && arrow.height > 0)
                laid.arrow = local_of(arrow, bounds);
        }
        for (SeatButton& button : laid.buttons)
            button.local = local_of(as_kit(button.at), bounds);
        laid.gadgets.reserve(everything.size());
        for (const place::PlaceRect& rect : everything)
            laid.gadgets.push_back(as_place(local_of(as_kit(rect), bounds)));
        laid.bounds = bounds;
        laid.ok = bounds.width > 0 && bounds.height > 0 && !laid.buttons.empty();
        return laid;
    }

    /// A text that changes when the buttons, the focus or the card change.
    ///
    /// @param laid the geometry
    /// @return the text
    [[nodiscard]] std::string seal_of(const Geometry& laid) const {
        std::string text = std::to_string(focused_slot_);
        text += '/';
        text += std::to_string(card_.open_slot());
        for (const SeatButton& button : laid.buttons) {
            text += '|';
            text += std::to_string(button.slot);
            text += button.label;
            text += std::to_string(button.at.x);
            text += std::to_string(button.at.y);
            text += std::to_string(button.at.width);
            text += std::to_string(button.at.height);
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

    /// The slot whose button holds a point, or -1.
    ///
    /// @param laid the geometry
    /// @param at the point, in the screen's points
    /// @return the slot
    [[nodiscard]] static int32_t button_at(const Geometry& laid, kit::Point at) noexcept {
        for (const SeatButton& button : laid.buttons) {
            if (kit::contains(button.local, at))
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
        for (const SeatButton& button : laid.buttons) {
            if (button.slot == slot)
                return true;
        }
        return false;
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

    /// Puts the label in the look, in the regular font where it fits after
    /// the badge, else the small font, cut with … only when even that does
    /// not fit.
    ///
    /// @param look the look the words go in
    /// @param fonts the fonts
    /// @param button the button, in the screen's points
    /// @param label the whole label
    void fit_caption(
        kit::ButtonLook& look,
        const kit::Fonts& fonts,
        const kit::Rect& button,
        const std::string& label
    ) const {
        const kit::ButtonMarkParts parts = kit::button_mark_parts(button);
        const int32_t room = std::max(parts.caption.width, int32_t{0});
        look.font = kit::FontRole::regular;
        if (kit::text_width(fonts, kit::FontRole::regular, label) <= room) {
            look.caption = label;
            return;
        }
        look.font = kit::FontRole::small;
        const auto measure = [&fonts](std::string_view text) {
            return kit::text_width(fonts, kit::FontRole::small, text);
        };
        look.caption = measure(label) <= room ? label : kit::fit(label, room, measure);
    }

    /// Tells whether a finger's reach lands on a button.
    ///
    /// A gadget nearer the point, or one the point lies inside, keeps it.
    ///
    /// @param laid where the buttons and the gadgets lie
    /// @param at the finger, in the screen's points
    /// @param finger_reach how far the finger reaches, in points
    /// @return the slot, or -1
    [[nodiscard]] int32_t
    finger_slot(const Geometry& laid, kit::Point at, int32_t finger_reach) const {
        if (finger_reach <= 0)
            return -1;
        for (const SeatButton& button : laid.buttons) {
            if (button.local.width <= 0 || button.local.height <= 0)
                continue;
            const place::PlaceRect button_place = as_place(button.local);
            if (place::nearer_gadget(at.x, at.y, button_place, laid.gadgets))
                continue;
            kit::DisplayList list = display_list();
            for (kit::Control& control : list.controls) {
                if (control.id == button.id)
                    control.enabled = true;
            }
            kit::Interaction trial = interaction();
            if (kit::finger_down(trial, list, at, finger_reach).control == button.id)
                return button.slot;
        }
        return -1;
    }

    /// Follows the pointer and the focus for the card.
    ///
    /// The pointer is kept in the layer's points, so a card opening, which
    /// moves the screen's origin, still finds the button.
    ///
    /// @param laid the geometry
    void follow_card(const Geometry& laid) {
        int32_t slot = mp::presence_card_no_slot;
        bool over = false;
        if (laid.ok && have_pointer_) {
            const kit::Point local{
                pointer_layer_.x - laid.bounds.x, pointer_layer_.y - laid.bounds.y
            };
            slot = button_at(laid, local);
            over = card_contains(laid, local);
        }
        card_.over_card(over);
        (void)card_.pointer(slot, static_cast<uint64_t>(SDL_GetTicks()));
        const int32_t focused = focused_slot_ >= 0 ? focused_slot_ : mp::presence_card_no_slot;
        (void)card_.focus(focused);
    }

    /// The combined Tab order: records and seat buttons, key ascending, a
    /// record before a button that shares its key.
    ///
    /// @param laid the geometry
    /// @return the stops
    [[nodiscard]] static std::vector<Stop> stops_of(const Geometry& laid) {
        std::vector<Stop> stops;
        stops.reserve(laid.focusable.size() + laid.buttons.size());
        for (const FocusRecord& record : laid.focusable)
            stops.push_back(Stop{false, record.index, order_key(record.at)});
        for (const SeatButton& button : laid.buttons)
            stops.push_back(Stop{true, button.slot, order_key(button.at)});
        std::sort(stops.begin(), stops.end(), [](const Stop& left, const Stop& right) {
            if (left.key != right.key)
                return left.key < right.key;
            return !left.button && right.button;
        });
        return stops;
    }

    /// Takes the focus onto a button and clears the screen's own ring.
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

    /// Hands the focus to a record and clears the button's ring.
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

    /// The seat button of the computer row above or below, when there is one.
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
        int32_t best_x = 0;
        for (const SeatButton& button : laid.buttons) {
            if (button.slot == slot)
                continue;
            const bool that_way = down ? button.at.y > current->at.y : button.at.y < current->at.y;
            if (!that_way)
                continue;
            const bool closer = found < 0 || (down ? button.at.y < best_y : button.at.y > best_y) ||
                                (button.at.y == best_y && button.at.x < best_x);
            if (!closer)
                continue;
            found = button.slot;
            best_y = button.at.y;
            best_x = button.at.x;
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
                    if (pressed == kit::Key::tab && current == button.energy && button.energy > 0)
                        return take_focus(button.slot);
                    if (pressed == kit::Key::tab) {
                        const int32_t before = record_before(laid.focusable, button.at);
                        if (current == before && before > 0)
                            return take_focus(button.slot);
                    }
                    if (pressed == kit::Key::back_tab) {
                        const int32_t after = record_after(laid.focusable, button.at);
                        if (current == after && after > 0)
                            return take_focus(button.slot);
                    }
                }
            }
            return LayerAnswer::pass;
        }
        if (pressed == kit::Key::enter || pressed == kit::Key::space) {
            open_picker(focused_slot_);
            return LayerAnswer::none;
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
        if (pressed == kit::Key::left)
            return current_button->energy > 0 ? hand_focus(current_button->energy) : LayerAnswer::none;
        if (pressed == kit::Key::right) {
            const int32_t next = nearest_record(laid.focusable, current_button->at, Aim::right);
            return next > 0 ? hand_focus(next) : LayerAnswer::none;
        }
        if (pressed == kit::Key::up || pressed == kit::Key::down) {
            const int32_t neighbour =
                neighbour_slot(laid, focused_slot_, pressed == kit::Key::down);
            if (neighbour >= 0)
                return take_focus(neighbour);
            const Aim aim = pressed == kit::Key::down ? Aim::down : Aim::up;
            const int32_t next = nearest_record(laid.focusable, current_button->at, aim);
            return next > 0 ? hand_focus(next) : LayerAnswer::none;
        }
        return LayerAnswer::pass;
    }

    /// Opens the picker on one seat. USE THIS writes that seat, or every
    /// computer seat. CANCEL changes nothing.
    ///
    /// @param slot the slot
    void open_picker(int32_t slot) {
        const Geometry laid = geometry(view_);
        const SeatButton* button = button_of(laid, slot);
        if (button == nullptr)
            return;
        BrainPickerRequest request;
        request.setup = brain::Setup::skirmish;
        request.seat_name = seat_title(slot);
        request.current = button->choice;
        request.setup_difficulty = difficulty_of(difficulty_stage());
        request.built_in_level_fixed = true;
        request.offer_every_seat = true;
        Runtime* runtime = &runtime_;
        request.used = [runtime, slot](picker::Choice choice, bool every) {
            BrainSeatsAccess::write(*runtime, slot, choice, every);
        };
        request.cancelled = [] {};
        runtime_.open_brain_picker(std::move(request));
    }

    Runtime& runtime_;                 ///< the runtime whose setup it shows over
    LayerView view_{};                 ///< the view it was last laid out for
    mp::PresenceCardTrigger card_{};   ///< when a seat's card opens and closes
    int32_t hovered_slot_{-1};         ///< the button the pointer is over; -1 for none
    int32_t pressed_slot_{-1};         ///< the button a press is held on; -1 for none
    int32_t focused_slot_{-1};         ///< the button the keyboard focus is on; -1 for none
    bool have_pointer_{};              ///< the pointer has been seen
    kit::Point pointer_layer_{};       ///< the pointer, in the layer's points
    bool card_pressed_{};              ///< a press started on the open card
    std::string seal_{};               ///< the buttons and the card last drawn
    uint64_t revision_{};              ///< counts the changes to how it looks
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
