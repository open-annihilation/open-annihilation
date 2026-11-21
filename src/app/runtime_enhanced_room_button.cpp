// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle room's Enhanced Mode. Once a frame the room resolves the
// revision it plays (resolve_enhanced_room): the host from its switch, the
// launch's lock and every seated machine's highest revision, a guest from
// the host's record. Each change holds that revision's rules for the
// room's next match and posts one line. The switch is drawn as the room's
// settings column draws an option, a label "Enhanced Mode" over a switch
// with the OA stamp reading the room's revision or "Off", in the strip right
// of BATTLE ROOM, its right edge the player list's; the player who keeps
// the room Off or lower is named in up to three rows left of it. The host
// turns it with a press, Return, Space or the pad's A; a guest sees it
// grayed, and a press or those keys show its card. The host's waiting line
// beside START names a machine whose rules are not the room's yet.

#include "brain_seats_room.hpp"
#include "engine_settings_state.hpp"
#include "oa_button_focus.hpp"
#include "oa_layer.hpp"

#include "oa/app/runtime.hpp"
#include "oa/data/enhanced_mode.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/netgame/unicode_chat.hpp"
#include "oa/present/game_text.hpp"
#include "oa/ui/frontend_multiplayer/get_map.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/mutator_room.hpp"
#include "oa/ui/frontend_multiplayer/presence_card.hpp"
#include "oa/ui/frontend_multiplayer/room_enhanced.hpp"
#include "oa/ui/frontend_multiplayer/room_mutators.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/frontend_renderer/room_parts.hpp"
#include "oa/ui/frontend_state/oa_button_place.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/pad_controls.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace mp = oa::ui::frontend_multiplayer;
namespace kit = oa::ui::kit;
namespace pad = oa::ui::pad_controls;
namespace place = oa::ui::frontend_state;
namespace room = oa::ui::frontend_renderer;
namespace mode = oa::data::enhanced_mode;

constexpr std::string_view screen_name = "enhanced-room";
constexpr std::string_view switch_name = "enhanced-mode";
constexpr std::string_view label_name = "enhanced-mode.label";
constexpr std::string_view reason_name = "enhanced-mode.reason";
constexpr std::string_view waiting_name = "enhanced-mode.waiting";
constexpr std::string_view word_label = "Enhanced Mode";
constexpr std::string_view word_automation_on = "Enhanced Mode · Revision {n}";
constexpr std::string_view word_automation_off = "Enhanced Mode · off";
constexpr std::string_view word_card_change = "Click to change.";
constexpr kit::ControlId switch_id = 1;
constexpr kit::ControlId label_id = 2;
constexpr kit::ControlId reason_id = 3;
constexpr kit::ControlId waiting_id = 4;
/// The card's slot: the switch, the one thing it describes.
constexpr int32_t card_slot = 0;

/// The label over the switch, in the battle room's frame pixels: as the
/// settings column labels an option, at the top of the strip right of
/// BATTLE ROOM (frame columns 214 to 494, rows 6 to 43).
constexpr room::SourceRect label_frame{374, 8, 120, 11};
/// The switch, as large as the settings column's, its right edge the
/// player list's.
constexpr room::SourceRect switch_frame{374, 21, 120, 20};
/// The reason's box left of the switch: up to three rows, right-aligned,
/// nearest the switch at its foot.
constexpr room::SourceRect reason_frame{222, 8, 144, 35};
/// The rows the reason may take.
constexpr int32_t reason_rows = 3;
/// The frame rows one line of the room's label font takes.
constexpr int32_t label_rows = 11;
/// The pitch of the reason's rows.
constexpr int32_t row_pitch = 12;
/// The waiting line beside START, at its widest, in points.
constexpr int32_t waiting_line_width = 400;

/// Returns one of the switch's words in the language shown.
///
/// @param english the words, in English
/// @return their translation, or the English
std::string shown(std::string_view english) {
    return std::string(oa::data::languages::interface_text(english));
}

/// The switch's text for automation: "Enhanced Mode · Revision 1", or
/// "Enhanced Mode · off".
///
/// @param revision the room's revision; 0 is Off
/// @return the text, in the language shown
std::string automation_text(uint32_t revision) {
    if (revision == 0)
        return shown(word_automation_off);
    std::string text = shown(word_automation_on);
    constexpr std::string_view holder = "{n}";
    if (const std::size_t at = text.find(holder); at != std::string::npos)
        text.replace(at, holder.size(), std::to_string(revision));
    return text;
}

kit::Rect local_of(const kit::Rect& rect, const kit::Rect& origin) noexcept {
    return {rect.x - origin.x, rect.y - origin.y, rect.width, rect.height};
}

place::PlaceRect as_place(const kit::Rect& rect) noexcept {
    return {rect.x, rect.y, rect.width, rect.height};
}

kit::Rect as_kit(const place::PlaceRect& rect) noexcept {
    return {rect.x, rect.y, rect.width, rect.height};
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

/// The name a player record shows, within its field.
///
/// @param name the field
/// @param bytes how many bytes the field holds
/// @return the name, read as UTF-8
std::string name_of(const char* name, std::size_t bytes) {
    if (name == nullptr || bytes == 0)
        return {};
    return oa::netgame::chat_utf8(std::string_view(name, ::strnlen(name, bytes)));
}

/// Tells whether Mutators' waiting line shows beside START: a seated player
/// lacks a mutator of the room's list. That line names what is missing, so
/// it wins over the Enhanced Mode waiting line in the same place.
///
/// @param mutators how many mutators the room's list holds
/// @param lobby the battle room's lobby
/// @return true while Mutators' line shows
bool mutators_line_shows(std::size_t mutators, mp::Lobby& lobby) {
    if (mutators == 0 || lobby.game == nullptr || !mp::lobby_hosts(lobby))
        return false;
    if (mp::lobby_mutators_block(lobby).hold == mp::MutatorsHold::none)
        return false;
    const std::size_t count = std::min(mutators, std::size_t{32});
    for (int32_t slot = 0; slot < mp::kSlotCount; ++slot) {
        if (!mp::slot_active(mp::slot_player(lobby, slot)))
            continue;
        for (std::size_t position = 0; position < count; ++position) {
            const auto have = mp::lobby_mutator_have(lobby, slot, static_cast<uint8_t>(position));
            if (have == mp::MutatorHave::missing || have == mp::MutatorHave::other_release)
                return true;
        }
    }
    return false;
}

/// Breaks words into rows of a width in the room's label font, at spaces;
/// what does not fit the last row allowed is kept on it, to be cut there.
///
/// @param art the room's art
/// @param words the words, in the room's bytes
/// @param width the room a row has, in frame pixels
/// @param rows the most rows
/// @return the rows, in the room's bytes
std::vector<std::string>
wrap_rows(const room::RoomArt& art, std::string_view words, int32_t width, int32_t rows) {
    std::vector<std::string> lines;
    std::string current;
    std::size_t at = 0;
    while (at < words.size()) {
        std::size_t space = words.find(' ', at);
        if (space == std::string_view::npos)
            space = words.size();
        const std::string word(words.substr(at, space - at));
        const std::string tried = current.empty() ? word : current + ' ' + word;
        if (!current.empty() && room::room_text_width(art, room::RoomFont::label, tried) > width) {
            lines.push_back(current);
            current = word;
        } else {
            current = tried;
        }
        at = space + 1;
    }
    if (!current.empty())
        lines.push_back(current);
    if (rows > 0 && static_cast<int32_t>(lines.size()) > rows) {
        std::string rest = lines[static_cast<std::size_t>(rows - 1)];
        for (std::size_t more = static_cast<std::size_t>(rows); more < lines.size(); ++more)
            rest += ' ' + lines[more];
        lines.resize(static_cast<std::size_t>(rows));
        lines.back() = rest;
    }
    return lines;
}

/// Reads room text back as UTF-8, a cut ended with "…".
///
/// @param bytes the room's bytes
/// @return the text
std::string room_text_back(std::string_view bytes) {
    std::string text =
        oa::present::decode_game_text(std::string(bytes), oa::present::game_text_settings().utf8);
    constexpr std::string_view cut = "...";
    if (text.size() >= cut.size() && text.compare(text.size() - cut.size(), cut.size(), cut) == 0)
        text.replace(text.size() - cut.size(), cut.size(), "…");
    return text;
}

} // namespace

// --- The room's resolution -------------------------------------------------

std::optional<uint32_t> Runtime::announced_enhanced_revision() const noexcept {
    if (options_.presence_without_enhanced_mode)
        return std::nullopt;
    return mode::highest_revision;
}

std::optional<uint32_t> Runtime::room_enhanced_announcement() const noexcept {
    const RoomEnhanced& state = room_enhanced_;
    if (!state.entered || !state.hosting || !state.held || state.held_revision == 0)
        return std::nullopt;
    return state.held_revision;
}

std::optional<mode::Choice> Runtime::room_enhanced_choice() const {
    const RoomEnhanced& state = room_enhanced_;
    if (!state.entered || !state.held)
        return std::nullopt;
    if (state.held_revision == 0)
        return mode::Choice{};
    return mode::Choice{state.held_revision, mode::revision_features(state.held_revision)};
}

std::optional<mp::EnhancedRoom> Runtime::room_enhanced_state() const {
    if (!room_enhanced_.entered)
        return std::nullopt;
    return room_enhanced_.room;
}

bool Runtime::room_enhanced_switch() const noexcept {
    return room_enhanced_.entered && room_enhanced_.hosting && room_enhanced_.switch_on;
}

bool Runtime::set_room_enhanced_switch(bool on) {
    RoomEnhanced& state = room_enhanced_;
    if (!state.entered || !state.hosting)
        return false;
    mp::Lobby& lobby = mp::multiplayer_lobby();
    if (on && lobby.game != nullptr && mp::lobby_options_locked(lobby))
        return false;
    state.switch_on = on;
    return true;
}

void Runtime::follow_room_enhanced() {
    keep_enhanced_room_screen();
    RoomEnhanced& state = room_enhanced_;
    mp::Lobby& lobby = mp::multiplayer_lobby();
    if (!mp::multiplayer_battle_room_open() || lobby.game == nullptr) {
        if (!state.entered)
            return;
        // A match that started from the room took the hold, and its end
        // releases it. A room left without a match plays nothing more.
        const bool match_took =
            match_ != nullptr || (engine_settings_ && engine_settings_->restored_enhanced_started);
        if (state.held && !match_took)
            release_enhanced_rules_once(true);
        state = {};
        return;
    }
    const bool hosting = mp::lobby_hosts(lobby);
    const bool locked = hosting && mp::lobby_options_locked(lobby);
    if (!state.entered || state.hosting != hosting) {
        if (state.entered && state.held)
            release_enhanced_rules_once(true);
        state = {};
        state.entered = true;
        state.hosting = hosting;
        const bool settings_on = engine_settings_ && engine_settings_->current.enhanced_mode;
        state.switch_on =
            hosting &&
            mp::enhanced_room_switch_start(settings_on, mp::lobby_stock_files_room(lobby), locked);
    }
    // A launch that locks the room's options keeps the switch Off.
    if (locked)
        state.switch_on = false;

    const std::vector<mp::EnhancedSeat> seats =
        mp::lobby_enhanced_seats(lobby, announced_enhanced_revision());
    mp::EnhancedRoomInputs inputs;
    inputs.hosting = hosting;
    inputs.switch_on = state.switch_on;
    inputs.locked = locked;
    if (!hosting)
        inputs.announced = mp::lobby_enhanced_announced(lobby);
    inputs.seats = seats;
    mp::EnhancedRoomChange change = mp::resolve_enhanced_room(inputs, state.room);
    state.room = change.room;
    for (const std::string& line : change.lines)
        mp::lobby_post_enhanced_line(lobby, line);

    // Every machine, the host included, holds the room's choice, never its
    // own Settings', from the room's first frame. A guest holds Off until
    // the host says otherwise.
    const uint32_t revision = change.room.revision;
    if (state.held && state.held_revision == revision)
        return;
    if (state.held) {
        release_enhanced_rules_once(true);
        state.held = false;
    }
    const mode::Choice choice =
        revision == 0 ? mode::Choice{} : mode::Choice{revision, mode::revision_features(revision)};
    if (hold_enhanced_rules_once(choice)) {
        state.held = true;
        state.held_revision = revision;
        state.hold_refused = false;
        std::cerr << "multiplayer: Enhanced Mode: the room plays "
                  << (revision == 0 ? std::string("Off") : "Revision " + std::to_string(revision))
                  << '\n';
        return;
    }
    if (!state.hold_refused) {
        state.hold_refused = true;
        std::cerr << "multiplayer: Enhanced Mode: this game cannot play "
                  << (revision == 0 ? std::string("Off") : "Revision " + std::to_string(revision))
                  << " with its rules\n";
    }
    // A revision the rules cannot play leaves the room Off on this machine.
    if (revision != 0 && hold_enhanced_rules_once(mode::Choice{})) {
        state.held = true;
        state.held_revision = 0;
    }
}

// --- The switch ------------------------------------------------------------

/// The switch as a screen of the OA layer: not modal, no backdrop, not
/// opaque, so the room shows round it. It takes the switch, the focus
/// handed to it, and the keys and the pad's A while it holds the focus.
struct Runtime::EnhancedRoomScreen final : LayerScreen {
    /// Makes the switch.
    ///
    /// @param runtime the runtime
    explicit EnhancedRoomScreen(Runtime& runtime) : runtime_(runtime) {
        runtime_.enhanced_room_screen_ = this;
    }

    /// Clears the runtime's pointer when it still names this switch.
    ~EnhancedRoomScreen() override {
        if (runtime_.enhanced_room_screen_ == this)
            runtime_.enhanced_room_screen_ = nullptr;
    }

    /// Where the switch and its parts lie, and what they show.
    struct Geometry {
        bool ok{};      ///< the room shows and the parts were placed
        bool hosting{}; ///< this machine hosts the room
        /// A launch locks the room's options, and the switch with them.
        bool launch_locked{};
        kit::Rect bounds{};   ///< the screen's place, in the layer's points
        kit::Rect button{};   ///< the switch, in the layer's points
        kit::Rect label{};    ///< the label over it, in the layer's points
        uint32_t revision{};  ///< the room's revision; 0 is Off
        bool switch_on{};     ///< the host's switch; on a guest, the room plays a revision
        std::string reason{}; ///< who keeps the room Off or lower; empty for none
        std::optional<kit::Rect> reason_box{};   ///< the reason's rows, in the layer's points
        std::optional<kit::Rect> waiting{};      ///< the line beside START
        std::string waiting_text{};              ///< that line's words
        std::vector<FocusRecord> focusable{};    ///< the room's records Tab can stop on
        std::vector<place::PlaceRect> gadgets{}; ///< every room control, in the screen's points
        place::PlaceRect button_points{};        ///< the switch, for the order
        int32_t before{-1};               ///< the record just before the switch in Tab's order
        int32_t after{-1};                ///< the record just after it
        int32_t first{-1};                ///< the first record in the order
        int32_t last{-1};                 ///< the last record in the order
        std::optional<kit::Rect> card{};  ///< the open card, in the layer's points
        std::optional<kit::Rect> arrow{}; ///< its arrow
        kit::HoverCardLook card_look{};   ///< the open card's look
    };

    /// Returns "enhanced-room".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return screen_name; }

    /// Returns the parts and the focus margin, in the window.
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

    /// Tells that what the switch does not take goes on.
    ///
    /// @return false
    [[nodiscard]] bool modal() const override { return false; }

    /// Tells that the switch darkens nothing.
    ///
    /// @return false
    [[nodiscard]] bool backdrop() const override { return false; }

    /// Tells that the room shows round the parts.
    ///
    /// @return false
    [[nodiscard]] bool opaque() const override { return false; }

    /// Tells that the switch draws the room's marker, not the kit's ring.
    ///
    /// @return false
    [[nodiscard]] bool draws_kit_focus_ring() const override { return false; }

    /// Follows the view the switch is placed on.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override { view_ = placed_on; }

    /// Draws the label, the switch with the OA stamp, its value and lamp,
    /// the reason's rows and the waiting line, in the room's art and fonts.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override {
        const Geometry laid = laid_out(view_);
        if (!laid.ok)
            return;
        const room::RoomArt art = room_art();
        const room::Surface& frame = BrainSeatsAccess::frame(runtime_);
        room::RoomCanvas parts = room::room_canvas(
            {switch_frame.x - room_focus_margin,
             switch_frame.y - room_focus_margin,
             switch_frame.width + 2 * room_focus_margin,
             switch_frame.height + 2 * room_focus_margin},
            &frame
        );
        const std::string value = room::room_text(mp::enhanced_revision_word(laid.revision));
        room::RoomButtonLook look = switch_look();
        look.caption = value;
        look.condition = room::ButtonCondition::normal;
        if (!laid.hosting || laid.launch_locked)
            look.condition = room::ButtonCondition::disabled;
        else if (pressed_ && hovered_)
            look.condition = room::ButtonCondition::pressed;
        room::draw_room_switch(parts, art, look, lamp_shown(laid));
        room::draw_room_stamp(parts, art, room::room_stamp_rect(switch_frame), room_stamp_icon());
        if (focus_ring_)
            room::draw_room_focus(parts, art, switch_frame);
        paint_room_parts(canvas, view_, laid.bounds, parts);

        {
            room::RoomCanvas label = room::room_canvas(label_frame, &frame);
            const std::string text = room::fit_room_text(
                art, room::RoomFont::label, room::room_text(shown(word_label)), label_frame.width
            );
            const int32_t height = room::room_line_height(art, room::RoomFont::label);
            std::ignore = room::draw_room_text(
                label,
                art,
                room::RoomFont::label,
                text,
                label_frame.x,
                label_frame.y + (label_frame.height - height) / 2,
                label_frame.width
            );
            paint_room_parts(canvas, view_, laid.bounds, label);
        }

        if (laid.reason_box) {
            const std::vector<std::string> rows =
                wrap_rows(art, room::room_text(laid.reason), reason_frame.width, reason_rows);
            // The rows sit at the box's foot, nearest the switch, right-aligned.
            const int32_t first = reason_frame.y + reason_frame.height -
                                  static_cast<int32_t>(rows.size()) * row_pitch + 1;
            for (std::size_t row = 0; row < rows.size(); ++row) {
                const room::SourceRect box{
                    reason_frame.x,
                    first + static_cast<int32_t>(row) * row_pitch,
                    reason_frame.width,
                    label_rows
                };
                room::RoomCanvas line = room::room_canvas(box, &frame);
                const std::string text =
                    room::fit_room_text(art, room::RoomFont::label, rows[row], box.width);
                const int32_t width = room::room_text_width(art, room::RoomFont::label, text);
                std::ignore = room::draw_room_text(
                    line,
                    art,
                    room::RoomFont::label,
                    text,
                    box.x + box.width - width,
                    box.y,
                    box.width
                );
                paint_room_parts(canvas, view_, laid.bounds, line);
            }
        }

        if (laid.waiting) {
            const room::SourceRect line = points_rect_frame(view_, *laid.waiting);
            room::RoomCanvas waiting = room::room_canvas(line, &frame);
            const std::string fitted = room::fit_room_text(
                art, room::RoomFont::label, room::room_text(laid.waiting_text), line.width
            );
            const int32_t width = room::room_text_width(art, room::RoomFont::label, fitted);
            const int32_t height = room::room_line_height(art, room::RoomFont::label);
            std::ignore = room::draw_room_text(
                waiting,
                art,
                room::RoomFont::label,
                fitted,
                line.x + line.width - width,
                line.y + (line.height - height) / 2,
                line.width
            );
            paint_room_parts(canvas, view_, laid.bounds, waiting);
        }

        if (laid.card) {
            kit::DisplayList card;
            kit::add_hover_card(card, local_of(*laid.card, laid.bounds), laid.card_look);
            kit::paint(canvas, card);
        }
    }

    /// Takes a press on the switch. The host's press turns it; a guest's,
    /// and one on a switch a launch locks, show its card. A press anywhere
    /// else clears the focus and goes on.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the screen's points
    /// @param finger_reach a finger's reach; 0 for a mouse
    /// @return redraw for the switch's own press; pass otherwise
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
                              ? finger_reaches_switch(laid, at, finger_reach)
                              : inside;
        if (kind == ScreenInputKind::pointer_down && button == SDL_BUTTON_LEFT) {
            if (over) {
                if (!turns(laid)) {
                    // Read-only: the press shows the card instead.
                    std::ignore = card_.tap(card_slot);
                    ++revision_;
                    return LayerAnswer::redraw;
                }
                card_.close();
                pressed_ = true;
                ++revision_;
                return LayerAnswer::redraw;
            }
            pressed_ = false;
            if (focused_) {
                focused_ = false;
                focus_ring_ = false;
                ++revision_;
            }
            return LayerAnswer::pass;
        }
        if (kind == ScreenInputKind::pointer_up && button == SDL_BUTTON_LEFT && pressed_) {
            pressed_ = false;
            ++revision_;
            if (over && turns(laid))
                turn(laid);
            return LayerAnswer::redraw;
        }
        if (kind == ScreenInputKind::pointer_up && button == SDL_BUTTON_LEFT && over)
            return LayerAnswer::redraw;
        return LayerAnswer::pass;
    }

    /// Takes the keys the switch holds the focus for, the information key,
    /// which shows the card, and Escape while the card is open.
    ///
    /// @param pressed the key
    /// @param sdl_key the platform key; 0 for a pad action
    /// @return none or redraw when the switch takes it; pass otherwise
    LayerAnswer key(kit::Key pressed, uint32_t sdl_key) override {
        // Tab and Shift+Tab reach the layer's screens as Next control and
        // Previous control; the room's own order follows Next button and
        // Previous button (shortcut), so these go on.
        if ((pressed == kit::Key::tab || pressed == kit::Key::back_tab) && sdl_key != 0)
            return LayerAnswer::pass;
        if (pressed == kit::Key::info && focused_ && focus_ring_ && card_.key_info(card_slot)) {
            ++revision_;
            return LayerAnswer::none;
        }
        if (pressed == kit::Key::escape && card_.key_close()) {
            ++revision_;
            return LayerAnswer::none;
        }
        return answer_key(pressed);
    }

    /// Takes Next button and Previous button as Tab's steps.
    ///
    /// @return none or redraw when the switch takes the step; pass otherwise
    LayerAnswer shortcut(uint32_t /*sdl_key*/, uint16_t /*modifiers*/) override {
        const MenuMove move = runtime_.menu_move(MenuMoveCount::down).move;
        if (move == MenuMove::next)
            return answer_key(kit::Key::tab);
        if (move == MenuMove::previous)
            return answer_key(kit::Key::back_tab);
        return LayerAnswer::pass;
    }

    /// Takes the pad's A and its focus moves on the same terms as the keyboard.
    ///
    /// @param action the gamepad action
    /// @return none or redraw when the switch takes it; pass otherwise
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

    /// Returns the switch, its label, the reason and the waiting line, for
    /// automation, the screen reader and the pointer.
    ///
    /// The switch is enhanced-mode, which the endpoint lists as
    /// oa.enhanced-mode: a switch named "Enhanced Mode", checked while the
    /// host's switch is on (on a guest, while the room plays a revision),
    /// its text "Enhanced Mode · Revision 1" or "Enhanced Mode · off", its
    /// description the reason that keeps the room Off or lower, or the lock
    /// reason. The label is enhanced-mode.label, the reason
    /// enhanced-mode.reason and the line beside START enhanced-mode.waiting,
    /// each an area Tab passes over. The open card's lines are
    /// battleroom.tip.line.<n>.
    ///
    /// @return the display list
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list;
        const Geometry laid = laid_out(view_);
        if (!laid.ok)
            return list;
        kit::Control control;
        control.id = switch_id;
        control.rect = local_of(laid.button, laid.bounds);
        control.enabled = turns(laid);
        control.name = std::string(switch_name);
        control.kind = kit::ControlKind::toggle;
        control.label = shown(word_label);
        control.text = automation_text(laid.revision);
        control.checked = laid.switch_on;
        control.description = description(laid);
        list.controls.push_back(std::move(control));
        list.tab_order.push_back(switch_id);
        kit::Control label;
        label.id = label_id;
        label.rect = local_of(laid.label, laid.bounds);
        label.enabled = false;
        label.name = std::string(label_name);
        label.kind = kit::ControlKind::area;
        label.focusable = false;
        label.text = shown(word_label);
        list.controls.push_back(std::move(label));
        if (laid.reason_box) {
            kit::Control reason;
            reason.id = reason_id;
            reason.rect = local_of(*laid.reason_box, laid.bounds);
            reason.enabled = false;
            reason.name = std::string(reason_name);
            reason.kind = kit::ControlKind::area;
            reason.focusable = false;
            reason.text = laid.reason;
            list.controls.push_back(std::move(reason));
        }
        if (laid.waiting) {
            kit::Control line;
            line.id = waiting_id;
            line.rect = local_of(*laid.waiting, laid.bounds);
            line.enabled = false;
            line.name = std::string(waiting_name);
            line.kind = kit::ControlKind::area;
            line.focusable = false;
            line.text = shown_waiting(laid);
            list.controls.push_back(std::move(line));
        }
        if (laid.card) {
            const kit::Rect card = local_of(*laid.card, laid.bounds);
            kit::add_hover_card(list, card, laid.card_look);
            mp::presence_card_controls(list, card, laid.card_look);
        }
        return list;
    }

    /// Returns the switch's hover, press and focus.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override {
        kit::Interaction shown_state;
        shown_state.hovered = hovered_ ? switch_id : kit::no_control;
        shown_state.pressed = pressed_ ? switch_id : kit::no_control;
        shown_state.focused = focused_ ? switch_id : kit::no_control;
        shown_state.focus_shown = focus_ring_;
        return shown_state;
    }

    /// Closes once the battle room is gone. Gives up the focus once the
    /// room's own focus is on a record again. Follows the words, the lamp,
    /// the reason and the waiting line.
    ///
    /// @return close when it should leave; otherwise none
    LayerAnswer tick() override {
        if (!mp::multiplayer_battle_room_open())
            return LayerAnswer::close;
        if (focused_ && mp::multiplayer_battle_room_focus() > 0) {
            focused_ = false;
            focus_ring_ = false;
            ++revision_;
        }
        const int32_t card_before = card_.open_slot();
        follow_card(laid_out(view_));
        if (card_.open_slot() != card_before)
            ++revision_;
        const Geometry laid = laid_out(view_);
        std::string now = automation_text(laid.revision);
        now += '/';
        now += std::to_string(static_cast<int32_t>(lamp_shown(laid)));
        now += laid.hosting ? "h" : "g";
        now += laid.launch_locked ? "l" : "-";
        now += laid.switch_on ? "1" : "0";
        now += laid.reason;
        now += '/';
        now += laid.waiting_text;
        if (now != seal_) {
            seal_ = std::move(now);
            ++revision_;
        }
        return LayerAnswer::none;
    }

    /// Returns the switch's revision.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Has nothing of its own to close.
    void close(bool /*by_key*/) override {}

  private:

    /// The switch's look, its words aside: its place and the room kept for the stamp.
    ///
    /// @return the look
    static room::RoomButtonLook switch_look() {
        room::RoomButtonLook look;
        look.rect = switch_frame;
        const room::SourceRect stamp = room::room_stamp_rect(switch_frame);
        look.inset_left = stamp.x - switch_frame.x + stamp.width + 2;
        look.narrower_font = true;
        return look;
    }

    /// Tells whether a press turns the switch: the host's, while no launch locks it.
    ///
    /// @param laid the geometry
    /// @return true when it turns
    [[nodiscard]] static bool turns(const Geometry& laid) noexcept {
        return laid.hosting && !laid.launch_locked;
    }

    /// Turns the host's switch.
    ///
    /// @param laid the geometry
    void turn(const Geometry& laid) {
        if (!turns(laid))
            return;
        std::ignore = runtime_.set_room_enhanced_switch(!runtime_.room_enhanced_switch());
        // The room resolves again at once, so the next frame draws it.
        runtime_.follow_room_enhanced();
        ++revision_;
    }

    /// The words a screen reader reads after the switch's value: the reason
    /// that keeps the room Off or lower, then the lock reason.
    ///
    /// @param laid the geometry
    /// @return the words; empty for none
    [[nodiscard]] static std::string description(const Geometry& laid) {
        std::string words = laid.reason;
        std::string lock;
        if (!laid.hosting)
            lock = mp::enhanced_room_guest_lock_text();
        else if (laid.launch_locked)
            lock = mp::enhanced_room_launch_lock_text();
        if (!lock.empty())
            words += (words.empty() ? "" : " ") + lock;
        return words;
    }

    /// The card: "Enhanced Mode", the room's revision, the reason, and what
    /// a press does or why it does nothing.
    ///
    /// @param laid the geometry
    /// @return the card, its arrow not yet pointed
    [[nodiscard]] static kit::HoverCardLook card_of(const Geometry& laid) {
        kit::HoverCardLook look;
        look.arrow = kit::Side::none;
        look.title = shown(word_label);
        kit::HoverCardRow row;
        row.value = mp::enhanced_revision_word(laid.revision);
        look.rows.push_back(std::move(row));
        if (!laid.reason.empty())
            look.block.push_back(laid.reason);
        if (!laid.hosting)
            look.block.push_back(mp::enhanced_room_guest_lock_text());
        else if (laid.launch_locked)
            look.block.push_back(mp::enhanced_room_launch_lock_text());
        else
            look.block.push_back(shown(word_card_change));
        return look;
    }

    /// Places the parts, from the room that shows and the room's state.
    ///
    /// @param placed_on what the screen is placed on
    /// @param with_card place the open card too
    /// @return the geometry; not ok while the room or its fonts are absent
    [[nodiscard]] Geometry geometry_of(const LayerView& placed_on, bool with_card) const {
        Geometry laid;
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        if (fonts == nullptr || placed_on.picture_size.x <= 0)
            return laid;
        mp::BattleRoomMap map;
        if (!mp::multiplayer_battle_room_map(&map))
            return laid;
        const std::optional<mp::EnhancedRoom> state = runtime_.room_enhanced_state();
        if (!state)
            return laid;
        mp::Lobby& lobby = mp::multiplayer_lobby();
        if (lobby.game == nullptr)
            return laid;
        laid.hosting = !map.joiner;
        laid.launch_locked = laid.hosting && mp::lobby_options_locked(lobby);
        laid.revision = state->revision;
        laid.switch_on = laid.hosting ? runtime_.room_enhanced_switch() : state->revision != 0;
        // The host names who keeps its switch from playing; a guest, who
        // cannot see the host's switch, names only a seat with no later revision.
        if (laid.hosting || state->reason == mp::EnhancedRoomReason::lower)
            laid.reason = mp::enhanced_room_reason_text(*state);
        laid.button = frame_rect_points(placed_on, switch_frame);
        laid.label = frame_rect_points(placed_on, label_frame);
        laid.button_points = as_place(laid.button);
        kit::Rect bounds = frame_rect_points(
            placed_on,
            {switch_frame.x - room_focus_margin,
             switch_frame.y - room_focus_margin,
             switch_frame.width + 2 * room_focus_margin,
             switch_frame.height + 2 * room_focus_margin}
        );
        bounds = unite(bounds, laid.label);
        if (!laid.reason.empty()) {
            laid.reason_box = frame_rect_points(placed_on, reason_frame);
            bounds = unite(bounds, *laid.reason_box);
        }

        const std::vector<mp::BattleRoomControl> controls = mp::multiplayer_battle_room_controls();
        place::PlaceRect start_rect{};
        bool have_start = false;
        std::vector<place::PlaceRect> reason_others;
        const auto local = static_cast<int32_t>(lobby.game->local_player_index);
        const std::string team_name = "TEAMICONS" + std::to_string(local);
        int32_t local_place = -1;
        for (const mp::BattleRoomControl& control : controls) {
            const kit::Rect points =
                frame_to_points(placed_on, {control.x, control.y, control.width, control.height});
            if (points.width <= 0 || points.height <= 0)
                continue;
            laid.gadgets.push_back(as_place(points));
            if (!have_start && Runtime::tdf_names_equal(control.name, "START")) {
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
                // The switch heads the settings column in Tab's order: after
                // this machine's own row, before Dead Commander.
                const bool own_row =
                    Runtime::tdf_names_equal(control.name, team_name) ||
                    Runtime::tdf_names_equal(control.name, "LOGO" + std::to_string(local)) ||
                    Runtime::tdf_names_equal(control.name, "SIDE" + std::to_string(local)) ||
                    Runtime::tdf_names_equal(control.name, "READY" + std::to_string(local));
                if (own_row && control.focus_place > local_place) {
                    local_place = control.focus_place;
                    laid.before = control.index;
                }
            }
        }
        for (const FocusRecord& record : laid.focusable)
            if (local_place >= 0 && record.focus_place == local_place + 1)
                laid.after = record.index;
        laid.first = first_record(laid.focusable);
        laid.last = last_record(laid.focusable);

        if (laid.hosting && have_start &&
            !mutators_line_shows(runtime_.room_mutators().size(), lobby)) {
            const mp::EnhancedBlock block = mp::lobby_enhanced_block(lobby);
            if (block.hold != mp::EnhancedHold::none && block.slot >= 0 && block.revision != 0) {
                const kit::Metrics& metrics = kit::compact_metrics;
                const place::PlaceRect box = mp::reason_box(
                    start_rect, reason_others, metrics.small_line, waiting_line_width
                );
                if (box.width > 0 && box.height > 0) {
                    const Player& player = mp::slot_player(lobby, block.slot);
                    laid.waiting = as_kit(box);
                    laid.waiting_text = mp::enhanced_waiting_line(
                        name_of(player.name, sizeof player.name), block.revision
                    );
                    bounds = unite(bounds, *laid.waiting);
                }
            }
        }

        if (with_card) {
            const int32_t arrow_depth =
                kit::metrics_of(placed_on.frame.size_class).hover_card_arrow;
            laid.card_look = card_of(laid);
            const kit::Point size = kit::hover_card_size(*fonts, laid.card_look);
            const kit::Rect placed =
                mp::presence_card_place(laid.button, size, placed_on.frame.area, arrow_depth);
            mp::presence_card_point(laid.card_look, placed, laid.button);
            laid.card = placed;
            bounds = unite(bounds, placed);
            const int32_t across = 2 * arrow_depth - 1;
            const int32_t left = placed.x + laid.card_look.arrow_at - (arrow_depth - 1);
            if (laid.card_look.arrow == kit::Side::top)
                laid.arrow = kit::Rect{left, placed.y - arrow_depth, across, arrow_depth};
            else if (laid.card_look.arrow == kit::Side::bottom)
                laid.arrow = kit::Rect{left, placed.y + placed.height, across, arrow_depth};
            if (laid.arrow)
                bounds = unite(bounds, *laid.arrow);
        }
        laid.bounds = bounds;
        for (place::PlaceRect& gadget : laid.gadgets) {
            gadget.x -= laid.bounds.x;
            gadget.y -= laid.bounds.y;
        }
        laid.ok = bounds.width > 0 && bounds.height > 0;
        return laid;
    }

    /// The geometry for a view, the card placed while it is open.
    ///
    /// @param placed_on what the screen is placed on
    /// @return the geometry
    [[nodiscard]] Geometry laid_out(const LayerView& placed_on) const {
        return geometry_of(placed_on, card_.open_slot() == card_slot);
    }

    /// The lamp, as the room's on and off buttons light theirs: red while
    /// the room is Off by the switch or a launch, green while it plays a
    /// revision, and blinking red while the host's switch is on and a
    /// machine keeps the room Off.
    ///
    /// @param laid the geometry
    /// @return the lamp
    [[nodiscard]] static room::RoomLamp lamp(const Geometry& laid) noexcept {
        if (laid.revision != 0)
            return room::RoomLamp::green;
        if (laid.hosting && laid.switch_on && !laid.launch_locked)
            return room::RoomLamp::red_blinking;
        return room::RoomLamp::red;
    }

    /// The lamp drawn now: a blinking lamp is red or unlit by the clock.
    ///
    /// @param laid the geometry
    /// @return the lamp
    [[nodiscard]] static room::RoomLamp lamp_shown(const Geometry& laid) {
        return room::room_lamp_shown(lamp(laid), static_cast<uint64_t>(SDL_GetTicks()));
    }

    /// The waiting line as the room draws it, in UTF-8: whole where it fits,
    /// else cut as the room cuts it.
    ///
    /// @param laid the geometry, its line placed
    /// @return the line
    [[nodiscard]] std::string shown_waiting(const Geometry& laid) const {
        if (!laid.waiting)
            return {};
        const room::RoomArt art = room_art();
        const std::string text = room::room_text(laid.waiting_text);
        const std::string fitted = room::fit_room_text(
            art, room::RoomFont::label, text, points_rect_frame(view_, *laid.waiting).width
        );
        if (fitted == text)
            return laid.waiting_text;
        return room_text_back(fitted);
    }

    /// Tells whether a finger's reach lands on the switch. A room control
    /// nearer the point, or one the point lies inside, keeps it.
    ///
    /// @param laid where the switch and the room's controls lie
    /// @param at the finger, in the screen's points
    /// @param finger_reach how far the finger reaches, in points
    /// @return true when the finger presses the switch
    [[nodiscard]] bool
    finger_reaches_switch(const Geometry& laid, kit::Point at, int32_t finger_reach) const {
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
            if (control.id == switch_id)
                control.enabled = true;
        kit::Interaction trial = interaction();
        return kit::finger_down(trial, list, at, finger_reach).control == switch_id;
    }

    /// Follows the pointer for the card: resting on the switch opens it, and
    /// it stays while the pointer is on the switch or the card.
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
        std::ignore = card_.pointer(
            over_button ? card_slot : mp::presence_card_no_slot,
            static_cast<uint64_t>(SDL_GetTicks())
        );
    }

    /// Takes the focus onto the switch and clears the room's own.
    ///
    /// @return redraw
    LayerAnswer take_focus() {
        focused_ = true;
        pressed_ = false;
        focus_ring_ = true;
        mp::multiplayer_set_battle_room_focus(mp::kNoControl);
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Hands the focus to a room record and clears the switch's ring.
    ///
    /// @param index the record
    /// @return redraw
    LayerAnswer hand_focus(int32_t index) {
        focused_ = false;
        focus_ring_ = false;
        mp::multiplayer_set_battle_room_focus(index);
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Answers one key, for the keyboard and for the pad actions that become keys.
    ///
    /// @param pressed the key
    /// @return none or redraw when the switch takes it; pass otherwise
    LayerAnswer answer_key(kit::Key pressed) {
        const Geometry laid = laid_out(view_);
        if (!laid.ok)
            return LayerAnswer::pass;
        const int32_t current = mp::multiplayer_battle_room_focus();
        if (!focused_) {
            if (pressed == kit::Key::tab && current == laid.before && laid.before > 0)
                return take_focus();
            if (pressed == kit::Key::back_tab && current == laid.after && laid.after > 0)
                return take_focus();
            return LayerAnswer::pass;
        }
        if (pressed == kit::Key::enter || pressed == kit::Key::space) {
            if (turns(laid)) {
                turn(laid);
            } else {
                std::ignore = card_.key_info(card_slot);
                ++revision_;
            }
            return LayerAnswer::redraw;
        }
        if (pressed == kit::Key::tab) {
            const int32_t next = laid.after > 0 ? laid.after : laid.first;
            return next > 0 ? hand_focus(next) : LayerAnswer::none;
        }
        if (pressed == kit::Key::back_tab) {
            const int32_t next = laid.before > 0 ? laid.before : laid.last;
            return next > 0 ? hand_focus(next) : LayerAnswer::none;
        }
        if (pressed == kit::Key::up || pressed == kit::Key::down || pressed == kit::Key::left ||
            pressed == kit::Key::right) {
            Aim aim = Aim::up;
            if (pressed == kit::Key::down)
                aim = Aim::down;
            else if (pressed == kit::Key::left)
                aim = Aim::left;
            else if (pressed == kit::Key::right)
                aim = Aim::right;
            const int32_t next = nearest_record(laid.focusable, laid.button_points, aim);
            return next > 0 ? hand_focus(next) : LayerAnswer::none;
        }
        return LayerAnswer::pass;
    }

    /// The battle room's art, read once while the screen lives.
    ///
    /// @return the art
    [[nodiscard]] room::RoomArt room_art() const {
        if (!art_read_) {
            art_read_ = true;
            art_ = BrainSeatsAccess::load_art(runtime_, battle_room_art_names());
        }
        if (!art_)
            return {};
        return {&*art_, room::room_palette(*art_)};
    }

    Runtime& runtime_;               ///< the runtime whose battle room it shows over
    LayerView view_{};               ///< the view it was last laid out for
    bool hovered_{};                 ///< the pointer is over the switch
    bool pressed_{};                 ///< a press on the switch is held
    bool focused_{};                 ///< the keyboard focus is on the switch
    bool focus_ring_{};              ///< the room's focus marker is drawn on it
    uint64_t revision_{};            ///< counts the changes to how it looks
    std::string seal_{};             ///< the words, lamp and lines last drawn
    mp::PresenceCardTrigger card_{}; ///< when the card opens and closes
    bool have_pointer_{};            ///< the pointer has been seen
    kit::Point pointer_layer_{};     ///< the pointer, in the layer's points
    /// The battle room's art, read on its first draw; nothing when it could not be read.
    mutable std::optional<room::ScreenResources> art_{};
    mutable bool art_read_{}; ///< the art has been read
};

void Runtime::keep_enhanced_room_screen() {
    if (!mp::multiplayer_battle_room_open() || oa_layer().screen_fonts() == nullptr)
        return;
    OaLayer& layer = oa_layer();
    if (layer.find(screen_name) != nullptr)
        return;
    layer.push(std::make_unique<EnhancedRoomScreen>(*this));
}

} // namespace oa::app
