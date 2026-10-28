// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A guest's Mutators in the battle room, on the OA layer: one screen,
// get-mutators, that is not modal, has no backdrop and is not opaque, drawn
// from the room's own art (brain_seats_room.hpp). It shows while the guest
// is in the battle room. The host's set is the room's own on and off button
// with the OA stamp and the word "Mutators", where the host's button stands,
// as large as Select Map and over it: the red lamp while the host plays none,
// the green lamp while this machine has every one and the red lamp blinking
// while it lacks one. A press opens the picker read-only on that set, and resting the
// pointer on it shows its card, which names them. To its left, GET MUTATORS, a button in the room's art,
// queues the releases the host named, through the fetch the room already
// owns; a bar takes its place while one downloads, and a line in the room's
// label font says why one cannot be fetched. Nothing readies the player.

#include "brain_seats_room.hpp"
#include "mod_install_state.hpp"
#include "oa_layer.hpp"

#include "oa/app/runtime.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/ui/frontend_multiplayer/get_map.hpp"
#include "oa/ui/frontend_multiplayer/mutator_room.hpp"
#include "oa/ui/frontend_multiplayer/presence_card.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/frontend_renderer/room_parts.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/frontend_state/oa_button_place.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/mutator_picker/model.hpp"
#include "oa/ui/pad_controls.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <tuple>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace picker = oa::ui::mutator_picker;
namespace place = oa::ui::frontend_state;
namespace mp = oa::ui::frontend_multiplayer;
namespace pad = oa::ui::pad_controls;
namespace kit = oa::ui::kit;
namespace room = oa::ui::frontend_renderer;

constexpr std::string_view screen_name = "get-mutators";
constexpr std::string_view view_name = "mutators-view";
constexpr std::string_view fetch_name = "get-mutators";
constexpr std::string_view progress_name = "get-mutators-progress";
constexpr std::string_view text_name = "get-mutators-text";
constexpr std::string_view ready_name = "get-mutators-ready";
constexpr std::string_view word_fetch = "GET MUTATORS";
constexpr std::string_view word_prefix = "Mutators:";
constexpr kit::ControlId view_control = 1;
constexpr kit::ControlId fetch_control = 2;
constexpr kit::ControlId text_control = 3;
constexpr kit::ControlId progress_control = 4;
constexpr kit::ControlId ready_control = 5;
constexpr int32_t summary_line_width = 200;
constexpr int32_t column_slack = 10;
/// The least gap above and below the button over Select Map, in frame pixels.
constexpr int32_t least_gap = 2;
/// Frame pixels between two of the block's parts, and round them.
constexpr int32_t part_gap = 4;
/// GET MUTATORS' padding round its words, in frame pixels.
constexpr int32_t fetch_padding = 14;
/// The bar's width, in frame pixels.
constexpr int32_t bar_width = 80;
/// The narrowest a part to the button's left may be, in frame pixels.
constexpr int32_t least_part_width = 30;
/// The card's slot: the button, the one thing it describes.
constexpr int32_t card_slot = 0;
constexpr int32_t bar_whole = 1000;
constexpr int32_t bar_per_percent = 10;

/// Which button of the row holds the keyboard focus.
enum class Held : uint8_t { none, view, fetch };

/// Where an arrow looks for the next record.
enum class Aim : uint8_t { up, down, left, right };

/// One focusable record of the battle room.
struct FocusNote {
    int32_t index{-1};       ///< the control
    place::PlaceRect at{};   ///< its rectangle, in points
    int32_t frame_x{};       ///< its left edge, in frame pixels
    int32_t frame_y{};       ///< its top edge, in frame pixels
    int32_t focus_place{-1}; ///< its place in the battle room's order
};

/// One mutator of the summary, and whether this machine lacks it.
struct Listed {
    std::string name{}; ///< the name, or the id
    bool lacking{};     ///< this machine does not have the host's release
};

/// Where the guest's button, the part to its left, the ready line and the
/// card lie.
struct Geometry {
    bool ok{};                          ///< the guest's block shows
    kit::Rect bounds{};                 ///< the screen's place, in the layer's points
    room::SourceRect view{};            ///< the room's Mutators button, in frame pixels
    room::SourceRect side{};            ///< GET MUTATORS, the bar or the line, in frame pixels
    bool fetch_button{};                ///< GET MUTATORS is a button
    bool bar{};                         ///< the bar shows in GET MUTATORS' place
    bool line_shown{};                  ///< the state line is drawn in GET MUTATORS' place
    int32_t bar_done{};                 ///< how much of the bar is filled, in thousandths
    std::string fetch_caption{};        ///< GET MUTATORS in the language shown
    bool summary{};                     ///< the line is the set, not a state line
    std::string line{};                 ///< the line, whole and uncut
    std::vector<Listed> listed{};       ///< the set, in the host's order
    std::vector<FocusNote> focusable{}; ///< the records Tab can stop on
    place::PlaceRect view_points{};     ///< the button, for the order
    place::PlaceRect fetch_points{};    ///< GET MUTATORS, for the order
    int32_t anchor_index{-1};           ///< Select Map's record
    int32_t before{-1};                 ///< the record just before the row
    int32_t after{-1};                  ///< the record just after the row
    int32_t first{-1};                  ///< the first record in the order
    int32_t last{-1};                   ///< the last record in the order
    int32_t below{-1};                  ///< the next record down Select Map's column
    std::optional<kit::Rect> ready{};   ///< the line in READY's place, in the layer's points
    std::string ready_text{};           ///< that line's words
    std::optional<kit::Rect> card{};    ///< the open card, in the layer's points
    std::optional<kit::Rect> arrow{};   ///< its arrow, in the layer's points
    kit::HoverCardLook card_look{};     ///< the open card's look, its arrow pointed
};

/// Returns one of the block's words in the language shown.
///
/// @param english the words, in English
/// @return their translation, or the English
std::string translated(std::string_view english) {
    return std::string(oa::data::languages::interface_text(english));
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
/// @param extra the rectangle to include; nothing when it is empty
/// @return the union
kit::Rect unite(const kit::Rect& into, const kit::Rect& extra) noexcept {
    if (into.width <= 0 || into.height <= 0)
        return extra;
    if (extra.width <= 0 || extra.height <= 0)
        return into;
    const int32_t span_left = std::min(into.x, extra.x);
    const int32_t top = std::min(into.y, extra.y);
    const int32_t right = std::max(into.x + into.width, extra.x + extra.width);
    const int32_t bottom = std::max(into.y + into.height, extra.y + extra.height);
    return {span_left, top, right - span_left, bottom - top};
}

/// Tells whether two rectangles of frame pixels share a pixel.
///
/// @param first a rectangle
/// @param second another
/// @return true when they do
bool meets(const room::SourceRect& first, const room::SourceRect& second) noexcept {
    return first.x < second.x + second.width && second.x < first.x + first.width &&
           first.y < second.y + second.height && second.y < first.y + first.height;
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

kit::Rect local_of(const kit::Rect& rect, const kit::Rect& origin) noexcept {
    return {rect.x - origin.x, rect.y - origin.y, rect.width, rect.height};
}

int64_t order_key(const place::PlaceRect& rect) noexcept {
    return static_cast<int64_t>(rect.y) * 5000 + rect.x;
}

/// The record just before the row, the first of a shared key.
///
/// @param records the focusable records
/// @param button the left button
/// @return its index, or -1 when none comes before
int32_t record_before(const std::vector<FocusNote>& records, const place::PlaceRect& button) {
    const int64_t key = order_key(button);
    int64_t best = 0;
    int32_t found = -1;
    for (const FocusNote& record : records) {
        const int64_t other = order_key(record.at);
        if (other <= key && (found < 0 || other > best)) {
            best = other;
            found = record.index;
        }
    }
    return found;
}

/// The next record down Select Map's column that sits wholly under the row.
///
/// @param records the focusable records
/// @param anchor_x Select Map's left edge, in frame pixels
/// @param button the row
/// @return its index, or -1 when the column has none under the row
int32_t column_below(
    const std::vector<FocusNote>& records, int32_t anchor_x, const place::PlaceRect& button
) {
    int32_t best_y = 0;
    int32_t found = -1;
    for (const FocusNote& record : records) {
        const int32_t delta = record.frame_x - anchor_x;
        if (delta <= -column_slack || delta >= column_slack)
            continue;
        if (record.at.y < button.y + button.height)
            continue;
        if (found < 0 || record.frame_y < best_y) {
            best_y = record.frame_y;
            found = record.index;
        }
    }
    return found;
}

/// The nearest focusable record in a direction, by the rectangles' centres.
///
/// @param records the focusable records
/// @param button the button the arrow leaves
/// @param aim the direction
/// @return its index, or -1 when none lies that way
int32_t
nearest_record(const std::vector<FocusNote>& records, const place::PlaceRect& button, Aim aim) {
    const int32_t origin_x = button.x + button.width / 2;
    const int32_t origin_y = button.y + button.height / 2;
    int64_t best = 0;
    int32_t found = -1;
    for (const FocusNote& record : records) {
        const int32_t centre_x = record.at.x + record.at.width / 2;
        const int32_t centre_y = record.at.y + record.at.height / 2;
        const int32_t dx = centre_x - origin_x;
        const int32_t dy = centre_y - origin_y;
        int32_t primary = 0;
        int32_t across = 0;
        switch (aim) {
        case Aim::up:
            primary = -dy;
            across = dx;
            break;
        case Aim::down:
            primary = dy;
            across = dx;
            break;
        case Aim::left:
            primary = -dx;
            across = dy;
            break;
        case Aim::right:
            primary = dx;
            across = dy;
            break;
        }
        const int32_t span = primary < 0 ? -primary : primary;
        const int32_t side = across < 0 ? -across : across;
        if (primary <= 0 || span < side)
            continue;
        const int64_t distance = static_cast<int64_t>(dx) * dx + static_cast<int64_t>(dy) * dy;
        if (found < 0 || distance < best) {
            best = distance;
            found = record.index;
        }
    }
    return found;
}

/// Tells whether READY stays off because this guest has not built the host's list.
///
/// The same rule as the battle room's guest_awaits_mutators. The host is
/// never waiting on itself. No mutator field on the host means the room has
/// no mutators. The guest waits when its own record has no field, names
/// another list, or is not built.
///
/// @param lobby the battle room
/// @return true when READY stays off
bool guest_awaits_mutators(mp::Lobby& lobby) noexcept {
    if (mp::lobby_hosts(lobby) || lobby.game == nullptr)
        return false;
    const uint8_t host_slot = mp::lobby_host_slot(lobby);
    if (host_slot == mp::kNoSlot)
        return false;
    const auto host_record = mp::lobby_presence_record(lobby, static_cast<int32_t>(host_slot));
    const auto host_state =
        oa::netgame::presence_mutators_state(host_record.data(), host_record.size());
    if (!host_state)
        return false;
    const auto own_record = mp::lobby_presence_record(lobby, lobby.game->local_player_index);
    const auto own_state =
        oa::netgame::presence_mutators_state(own_record.data(), own_record.size());
    if (!own_state || own_state->digest != host_state->digest)
        return true;
    return !own_state->built;
}

/// The name the summary and the ready line show for one fetched mutator.
///
/// @param item the fetch row
/// @return its offer's name, or its id
std::string item_name(const Runtime::HostMutatorFetch& item) {
    return item.offer.name.empty() ? item.mutator.id : item.offer.name;
}

/// The registry the chat and the downloads-off line name.
///
/// @param item the fetch row
/// @return the registry's name, or its id
std::string item_registry(const Runtime::HostMutatorFetch& item) {
    return item.offer.registry_name.empty() ? item.offer.registry_id : item.offer.registry_name;
}

} // namespace

/// A guest's Mutators as a screen of the OA layer.
///
/// Its points are the layer's. It shows the room's Mutators button over
/// Select Map and, to its left while a release can be fetched or the last
/// try failed, GET MUTATORS; the bar while a download runs; or the line that
/// says why none can be fetched.
struct Runtime::GetMutatorsScreen final : LayerScreen {
    /// Makes the screen.
    ///
    /// @param runtime the runtime, whose fetch and picker it uses
    explicit GetMutatorsScreen(Runtime& runtime) : runtime_(runtime) {}

    /// Returns "get-mutators".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return screen_name; }

    /// Returns the button, the part to its left, the ready line and the open
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

    /// Tells that what the block does not take goes on.
    ///
    /// @return false
    [[nodiscard]] bool modal() const override { return false; }

    /// Tells that the block darkens nothing.
    ///
    /// @return false
    [[nodiscard]] bool backdrop() const override { return false; }

    /// Tells that the block lies over the picture.
    ///
    /// @return false
    [[nodiscard]] bool opaque() const override { return false; }

    /// Follows the view the block is placed on.
    ///
    /// @param placed_on what the screen is placed on
    void lay_out(const LayerView& placed_on) override { view_ = placed_on; }

    /// Draws the button, GET MUTATORS or the bar or the line, the ready line
    /// and the open card.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override {
        const Geometry laid = geometry(view_);
        if (!laid.ok)
            return;
        const room::RoomArt art = room_art();
        const room::Surface& frame = BrainSeatsAccess::frame(runtime_);
        const int32_t margin = room_focus_margin;
        room::RoomCanvas parts = room::room_canvas(
            {laid.view.x - margin,
             laid.view.y - margin,
             laid.view.width + 2 * margin,
             laid.view.height + 2 * margin},
            &frame
        );
        draw_mutators_switch(
            parts,
            art,
            laid.view,
            room::room_lamp_shown(lamp(laid), static_cast<uint64_t>(SDL_GetTicks())),
            pressed_ == view_control && hovered_ == view_control ? room::ButtonCondition::pressed
                                                                 : room::ButtonCondition::normal,
            focused_ == Held::view
        );
        paint_room_parts(canvas, view_, laid.bounds, parts);
        if (laid.side.width > 0) {
            room::RoomCanvas side = room::room_canvas(
                {laid.side.x - margin,
                 laid.side.y - margin,
                 laid.side.width + 2 * margin,
                 laid.side.height + 2 * margin},
                &frame
            );
            if (laid.fetch_button) {
                const std::string caption = room::room_text(laid.fetch_caption);
                room::RoomButtonLook look;
                look.rect = laid.side;
                look.caption = caption;
                look.narrower_font = true;
                if (pressed_ == fetch_control && hovered_ == fetch_control)
                    look.condition = room::ButtonCondition::pressed;
                room::draw_room_button(side, art, look);
                if (focused_ == Held::fetch)
                    room::draw_room_focus(side, art, laid.side);
            } else if (laid.bar) {
                room::draw_room_bar(side, art, laid.side, laid.bar_done, bar_whole);
                const std::string percent =
                    room::room_text(std::to_string(laid.bar_done / bar_per_percent) + "%");
                draw_line(side, art, laid.side, percent, true);
            } else if (laid.line_shown) {
                // On the box's top rows, clear of the room's own lettering under it.
                const int32_t height = room::room_line_height(art, room::RoomFont::label);
                draw_line(
                    side,
                    art,
                    {laid.side.x, laid.side.y, laid.side.width, std::min(height, laid.side.height)},
                    room::room_text(laid.line),
                    false
                );
            }
            paint_room_parts(canvas, view_, laid.bounds, side);
        }
        if (laid.ready) {
            const room::SourceRect line = points_rect_frame(view_, *laid.ready);
            room::RoomCanvas ready = room::room_canvas(line, &frame);
            draw_line(ready, art, line, room::room_text(laid.ready_text), false);
            paint_room_parts(canvas, view_, laid.bounds, ready);
        }
        if (laid.card) {
            kit::DisplayList card;
            kit::add_hover_card(card, local_of(*laid.card, laid.bounds), laid.card_look);
            kit::paint(canvas, card);
        }
    }

    /// Takes a press on the button or GET MUTATORS. A press anywhere else goes on.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the screen's points
    /// @return redraw for the block's own press; pass otherwise
    LayerAnswer pointer(
        ScreenInputKind kind, uint8_t button, kit::Point at, int32_t /*finger_reach*/
    ) override {
        const Geometry laid = geometry(view_);
        if (laid.ok) {
            pointer_layer_ = {at.x + laid.bounds.x, at.y + laid.bounds.y};
            have_pointer_ = true;
        }
        const kit::ControlId over = hit(laid);
        if (over != hovered_) {
            hovered_ = over;
            ++revision_;
        }
        const int32_t card_before = card_.open_slot();
        follow_card(laid);
        if (card_.open_slot() != card_before)
            ++revision_;
        if (kind == ScreenInputKind::pointer_down && button == SDL_BUTTON_LEFT) {
            if (over == view_control || over == fetch_control) {
                pressed_ = over;
                card_.close();
                ++revision_;
                return LayerAnswer::redraw;
            }
            pressed_ = kit::no_control;
            if (focused_ != Held::none) {
                focused_ = Held::none;
                ++revision_;
            }
            return LayerAnswer::pass;
        }
        if (kind == ScreenInputKind::pointer_up && button == SDL_BUTTON_LEFT &&
            pressed_ != kit::no_control) {
            const kit::ControlId held = pressed_;
            pressed_ = kit::no_control;
            ++revision_;
            if (over == held) {
                if (held == view_control)
                    open_view();
                else
                    runtime_.fetch_host_mutators();
            }
            return LayerAnswer::redraw;
        }
        return LayerAnswer::pass;
    }

    /// Takes the keys the row holds the focus for, Y, which reads the set,
    /// the information key, which shows the card, and Escape while it is open.
    ///
    /// @param pressed the key
    /// @return none or redraw when the row takes it; pass otherwise
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        if (pressed == kit::Key::info && focused_ == Held::view && card_.key_info(card_slot)) {
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
    /// @return none or redraw when the row takes it; pass otherwise
    LayerAnswer pad_action(pad::Action action) override {
        if (action == pad::Action::library_search)
            return answer_key(kit::Key::yes);
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

    /// Returns the block's controls for automation and the pointer, and the
    /// open card.
    ///
    /// The button is mutators-view: its text counts the host's set,
    /// "Mutators · off" or "Mutators · 2 on" (mutators_automation_text),
    /// and it is checked while its lamp is green, not while it is red or
    /// blinks. GET MUTATORS is get-mutators. The bar is
    /// get-mutators-progress and its text is the percent. The line is
    /// get-mutators-text and stores the whole line, uncut: the set while
    /// nothing is being fetched, otherwise the state. The ready line is
    /// get-mutators-ready. The endpoint adds oa. to each name.
    ///
    /// @return the display list
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList list;
        const Geometry laid = geometry(view_);
        if (!laid.ok)
            return list;
        const auto area =
            [&](kit::ControlId id, const kit::Rect& rect, std::string_view name, std::string text) {
                kit::Control control;
                control.id = id;
                control.rect = rect;
                control.enabled = false;
                control.focusable = false;
                control.name = std::string(name);
                control.kind = kit::ControlKind::area;
                control.text = std::move(text);
                list.controls.push_back(std::move(control));
            };
        const room::RoomArt art = room_art();
        const kit::Rect view_rect = frame_rect_local(view_, laid.bounds, laid.view);
        if (laid.fetch_button) {
            kit::Control fetch;
            fetch.id = fetch_control;
            fetch.rect = frame_rect_local(view_, laid.bounds, laid.side);
            fetch.name = std::string(fetch_name);
            fetch.kind = kit::ControlKind::button;
            fetch.text = laid.fetch_caption;
            list.controls.push_back(std::move(fetch));
            list.tab_order.push_back(fetch_control);
        }
        kit::Control view_button;
        view_button.id = view_control;
        view_button.rect = view_rect;
        view_button.name = std::string(view_name);
        view_button.kind = kit::ControlKind::button;
        view_button.text = mutators_automation_text(art, laid.view, laid.listed.size());
        view_button.checked = lamp(laid) == room::RoomLamp::green;
        list.controls.push_back(std::move(view_button));
        list.tab_order.push_back(view_control);
        if (laid.bar)
            area(
                progress_control,
                frame_rect_local(view_, laid.bounds, laid.side),
                progress_name,
                std::to_string(laid.bar_done / bar_per_percent) + "%"
            );
        area(
            text_control,
            laid.line_shown ? frame_rect_local(view_, laid.bounds, laid.side) : view_rect,
            text_name,
            laid.line
        );
        if (laid.ready)
            area(ready_control, local_of(*laid.ready, laid.bounds), ready_name, laid.ready_text);
        if (laid.card) {
            const kit::Rect card = local_of(*laid.card, laid.bounds);
            kit::add_hover_card(list, card, laid.card_look);
            mp::presence_card_controls(list, card, laid.card_look);
        }
        return list;
    }

    /// Returns the row's hover, press and focus.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override {
        kit::Interaction shown_state;
        shown_state.hovered = hovered_;
        shown_state.pressed = pressed_;
        if (focused_ == Held::view)
            shown_state.focused = view_control;
        else if (focused_ == Held::fetch)
            shown_state.focused = fetch_control;
        shown_state.focus_shown = focused_ != Held::none;
        return shown_state;
    }

    /// Closes once the multiplayer screens go. Gives up the focus once the
    /// battle room's own focus is on a record again, and once GET MUTATORS
    /// is no longer a button. Follows the pointer for the card.
    ///
    /// @return close when it should leave; redraw when what it shows changed; otherwise none
    LayerAnswer tick() override {
        if (!mp::multiplayer_showing())
            return LayerAnswer::close;
        const uint64_t before = revision_;
        const int32_t card_before = card_.open_slot();
        follow_card(geometry(view_));
        if (card_.open_slot() != card_before)
            ++revision_;
        const Geometry laid = geometry(view_);
        const Held was = focused_;
        if (focused_ != Held::none && mp::multiplayer_battle_room_focus() > 0)
            focused_ = Held::none;
        if (focused_ == Held::fetch && !laid.fetch_button)
            focused_ = Held::none;
        const std::string stamp = stamp_of(laid);
        if (focused_ != was || stamp != stamp_) {
            stamp_ = std::move(stamp);
            ++revision_;
        }
        return revision_ != before ? LayerAnswer::redraw : LayerAnswer::none;
    }

    /// Returns the block's revision.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Has nothing of its own to close. A download goes on.
    void close(bool /*by_key*/) override {}

  private:

    /// The battle room's art, read once while the screen lives.
    ///
    /// @return the art; without resources when it could not be read
    [[nodiscard]] room::RoomArt room_art() const {
        if (!art_read_) {
            art_read_ = true;
            art_ = BrainSeatsAccess::load_art(runtime_, battle_room_art_names());
        }
        if (!art_)
            return {};
        return {&*art_, room::room_palette(*art_)};
    }

    /// The button's lamp: red while the host plays no mutator, blinking red
    /// while this machine lacks one (READY waits on it), otherwise green.
    ///
    /// @param laid the place
    /// @return the lamp
    [[nodiscard]] static room::RoomLamp lamp(const Geometry& laid) noexcept {
        if (laid.listed.empty())
            return room::RoomLamp::red;
        for (const Listed& one : laid.listed)
            if (one.lacking)
                return room::RoomLamp::red_blinking;
        return room::RoomLamp::green;
    }

    /// Draws a line of the room's label font in a box: right-aligned, or
    /// centred, and cut with "..." where it is too wide.
    ///
    /// @param parts the canvas
    /// @param art the art
    /// @param box the box, in frame pixels
    /// @param text the line, in the room's bytes
    /// @param centred centre it rather than align it right
    static void draw_line(
        room::RoomCanvas& parts,
        const room::RoomArt& art,
        const room::SourceRect& box,
        const std::string& text,
        bool centred
    ) {
        const std::string fitted =
            room::fit_room_text(art, room::RoomFont::label, text, box.width - 2);
        const int32_t width = room::room_text_width(art, room::RoomFont::label, fitted);
        const int32_t height = room::room_line_height(art, room::RoomFont::label);
        const int32_t x = centred ? box.x + (box.width - width) / 2 : box.x + box.width - width;
        std::ignore = room::draw_room_text(
            parts,
            art,
            room::RoomFont::label,
            fitted,
            x,
            box.y + (box.height - height) / 2,
            box.width
        );
    }

    /// Places the guest's button where the host's stands over Select Map,
    /// the part to its left, the ready line beside START and the open card.
    ///
    /// @param placed_on what the screen is placed on
    /// @return the place; not ok when the guest has nothing to show
    [[nodiscard]] Geometry geometry(const LayerView& placed_on) const {
        Geometry laid;
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        if (fonts == nullptr || placed_on.picture_size.x <= 0 || placed_on.match)
            return laid;
        mp::BattleRoomMap map;
        if (!mp::multiplayer_battle_room_map(&map) || !map.joiner)
            return laid;
        const std::vector<Runtime::HostMutatorFetch> fetch = runtime_.host_mutators_fetch();

        std::vector<mp::MutatorFetch> states;
        states.reserve(fetch.size());
        const Runtime::HostMutatorFetch* deciding = nullptr;
        laid.listed.reserve(fetch.size());
        for (const Runtime::HostMutatorFetch& item : fetch) {
            states.push_back(item.state);
            Listed one;
            one.name = item_name(item);
            one.lacking = item.state != mp::MutatorFetch::have;
            if (deciding == nullptr && one.lacking)
                deciding = &item;
            laid.listed.push_back(std::move(one));
        }
        // A guest that has built the host's list lacks nothing, whatever a
        // catalogue says of the releases: no part shows beside the button.
        const bool awaits = !fetch.empty() && guest_awaits_mutators(mp::multiplayer_lobby());
        if (!awaits) {
            deciding = nullptr;
            for (Listed& one : laid.listed)
                one.lacking = false;
        }
        const mp::GetMutators stage =
            fetch.empty() || !awaits ? mp::GetMutators::none : mp::get_mutators_state(states);
        laid.fetch_button = !fetch.empty() && mp::get_mutators_button_shown(stage);
        laid.bar = !fetch.empty() && stage == mp::GetMutators::downloading;
        std::string deciding_name;
        std::string deciding_registry;
        std::string problem;
        int32_t percent = 0;
        if (deciding != nullptr) {
            deciding_name = item_name(*deciding);
            deciding_registry = item_registry(*deciding);
            problem = deciding->failure;
            if (deciding->offer.bytes > 0) {
                const uint64_t bytes = deciding->offer.bytes;
                const uint64_t done = std::min(deciding->done, bytes);
                percent = static_cast<int32_t>(done * 100 / bytes);
                laid.bar_done = static_cast<int32_t>(done * bar_whole / bytes);
            }
        }
        laid.summary = stage == mp::GetMutators::none || stage == mp::GetMutators::get;
        if (laid.summary) {
            laid.line = translated(word_prefix);
            for (std::size_t at = 0; at < laid.listed.size(); ++at) {
                laid.line += at == 0 ? " " : " · ";
                laid.line += laid.listed[at].name;
            }
        } else {
            laid.line =
                mp::get_mutators_text(stage, deciding_name, percent, problem, deciding_registry);
        }
        laid.fetch_caption = translated(word_fetch);

        const std::vector<mp::BattleRoomControl> controls = mp::multiplayer_battle_room_controls();
        std::vector<room::SourceRect> frames;
        std::vector<place::PlaceRect> reason_others;
        room::SourceRect anchor{};
        place::PlaceRect start_rect{};
        bool found = false;
        bool have_start = false;
        int32_t anchor_frame_x = 0;
        frames.reserve(controls.size() + 1);
        for (const mp::BattleRoomControl& control : controls) {
            const kit::Rect frame{control.x, control.y, control.width, control.height};
            const kit::Rect points = frame_to_points(placed_on, frame);
            if (points.width <= 0 || points.height <= 0)
                continue;
            const bool map_control = Runtime::tdf_names_equal(control.name, "MAP");
            const bool start = Runtime::tdf_names_equal(control.name, "START");
            if (map_control && !found) {
                anchor = {control.x, control.y, control.width, control.height};
                anchor_frame_x = control.x;
                laid.anchor_index = control.index;
                found = true;
            } else {
                frames.push_back({control.x, control.y, control.width, control.height});
            }
            if (start && !have_start) {
                start_rect = as_place(points);
                have_start = true;
            } else {
                reason_others.push_back(as_place(points));
            }
            if (control.focus_place >= 0) {
                FocusNote record;
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
        const Runtime::ShownBox map_box = runtime_.get_map_box();
        if (map_box.width > 0 && map_box.height > 0) {
            const kit::Rect box{map_box.x, map_box.y, map_box.width, map_box.height};
            frames.push_back(points_rect_frame(placed_on, box));
            reason_others.push_back(as_place(box));
        }

        // The button stands where the host's stands: as large as Select Map
        // and over it, its edges Select Map's.
        const room::SourceRect room_frame{0, 0, placed_on.picture_size.x, placed_on.picture_size.y};
        std::vector<place::PlaceRect> others;
        others.reserve(frames.size());
        for (const room::SourceRect& rect : frames)
            others.push_back({rect.x, rect.y, rect.width, rect.height});
        const std::optional<place::PlaceRect> over = place::place_over(
            {anchor.x, anchor.y, anchor.width, anchor.height},
            others,
            {room_frame.x, room_frame.y, room_frame.width, room_frame.height},
            least_gap
        );
        laid.view =
            over ? room::SourceRect{over->x, over->y, over->width, over->height}
                 : room::SourceRect{
                       anchor.x, anchor.y - anchor.height - part_gap, anchor.width, anchor.height
                   };
        const room::RoomArt art = room_art();

        // GET MUTATORS, the bar or the line to its left, shortened to miss
        // every control of the room.
        laid.line_shown = !laid.summary && !laid.fetch_button && !laid.bar;
        int32_t wanted = 0;
        if (laid.fetch_button)
            wanted = room::room_text_width(
                         art, room::RoomFont::label, room::room_text(laid.fetch_caption)
                     ) +
                     fetch_padding;
        else if (laid.bar)
            wanted = bar_width;
        else if (laid.line_shown)
            wanted = std::min(
                room::room_text_width(art, room::RoomFont::label, room::room_text(laid.line)) + 4,
                summary_line_width
            );
        if (wanted > 0) {
            const int32_t right = laid.view.x - part_gap;
            room::SourceRect side{right - wanted, laid.view.y, wanted, laid.view.height};
            for (const room::SourceRect& other : frames) {
                if (!meets(side, other))
                    continue;
                const int32_t left = other.x + other.width + part_gap;
                side.width = right - left;
                side.x = left;
                if (side.width <= 0)
                    break;
            }
            side.x = std::max(side.x, room_frame.x);
            side.width = right - side.x;
            if (side.width >= least_part_width)
                laid.side = side;
        }
        if (laid.side.width <= 0) {
            laid.fetch_button = false;
            laid.bar = false;
            laid.line_shown = false;
        }

        laid.view_points = as_place(frame_rect_points(placed_on, laid.view));
        if (laid.fetch_button)
            laid.fetch_points = as_place(frame_rect_points(placed_on, laid.side));
        const place::PlaceRect leftmost = laid.fetch_button ? laid.fetch_points : laid.view_points;
        const room::SourceRect marked{
            laid.view.x - room_focus_margin,
            laid.view.y - room_focus_margin,
            laid.view.width + 2 * room_focus_margin,
            laid.view.height + 2 * room_focus_margin
        };
        kit::Rect bounds = frame_rect_points(placed_on, marked);
        if (laid.side.width > 0)
            bounds = unite(
                bounds,
                frame_rect_points(
                    placed_on,
                    {laid.side.x - room_focus_margin,
                     laid.side.y - room_focus_margin,
                     laid.side.width + 2 * room_focus_margin,
                     laid.side.height + 2 * room_focus_margin}
                )
            );
        laid.before = record_before(laid.focusable, leftmost);
        int32_t before_place = -1;
        for (const FocusNote& record : laid.focusable) {
            if (record.index == laid.before)
                before_place = record.focus_place;
        }
        if (before_place >= 0) {
            for (const FocusNote& record : laid.focusable) {
                if (record.focus_place == before_place + 1)
                    laid.after = record.index;
            }
        }
        int32_t earliest = 0;
        int32_t latest = -1;
        for (const FocusNote& record : laid.focusable) {
            if (laid.first < 0 || record.focus_place < earliest) {
                earliest = record.focus_place;
                laid.first = record.index;
            }
            if (record.focus_place > latest) {
                latest = record.focus_place;
                laid.last = record.index;
            }
        }
        laid.below = column_below(laid.focusable, anchor_frame_x, laid.view_points);
        const kit::Metrics& metrics = kit::compact_metrics;
        if (have_start && deciding != nullptr && guest_awaits_mutators(mp::multiplayer_lobby())) {
            place::PlaceRect box = mp::reason_box(
                start_rect, reason_others, metrics.small_line, summary_line_width * 2
            );
            // The line is right-aligned, and at 1280x720 its words meet the
            // chat entry's pipe and sit on the entry's frame. A few points
            // left and up clear that frame. The host's waiting line is left
            // where reason_box puts it.
            constexpr int32_t kClearEntry = 10;
            constexpr int32_t kLift = 6;
            box.width = std::max(box.width - kClearEntry, int32_t{1});
            box.y = std::max(box.y - kLift, int32_t{0});
            if (box.width > 0 && box.height > 0) {
                laid.ready = as_kit(box);
                laid.ready_text = mp::guest_ready_line(deciding_name);
                bounds = unite(bounds, *laid.ready);
            }
        }
        if (card_.open_slot() == card_slot) {
            std::vector<MutatorsCardItem> items;
            items.reserve(laid.listed.size());
            for (const Listed& one : laid.listed)
                items.push_back({one.name, one.lacking});
            laid.card_look = mutators_card(items, false);
            const kit::Point size = kit::hover_card_size(*fonts, laid.card_look);
            const kit::Rect badge = frame_rect_points(placed_on, laid.view);
            const int32_t arrow_depth =
                kit::metrics_of(placed_on.frame.size_class).hover_card_arrow;
            const kit::Rect card =
                mp::presence_card_place(badge, size, placed_on.frame.area, arrow_depth);
            mp::presence_card_point(laid.card_look, card, badge);
            laid.card = card;
            bounds = unite(bounds, card);
            const int32_t across = 2 * arrow_depth - 1;
            const int32_t left = card.x + laid.card_look.arrow_at - (arrow_depth - 1);
            if (laid.card_look.arrow == kit::Side::top)
                laid.arrow = kit::Rect{left, card.y - arrow_depth, across, arrow_depth};
            else if (laid.card_look.arrow == kit::Side::bottom)
                laid.arrow = kit::Rect{left, card.y + card.height, across, arrow_depth};
            if (laid.arrow)
                bounds = unite(bounds, *laid.arrow);
        }
        laid.bounds = bounds;
        laid.ok = bounds.width > 0 && bounds.height > 0;
        return laid;
    }

    /// The control under the pointer, or none.
    ///
    /// @param laid the place
    /// @return the control
    [[nodiscard]] kit::ControlId hit(const Geometry& laid) const {
        if (!laid.ok || !have_pointer_)
            return kit::no_control;
        const kit::Point pixel = frame_point(view_, pointer_layer_);
        if (holds_pixel(laid.view, pixel))
            return view_control;
        if (laid.fetch_button && holds_pixel(laid.side, pixel))
            return fetch_control;
        return kit::no_control;
    }

    /// Follows the pointer for the card: resting on the button opens it,
    /// and it stays while the pointer is on the button or the card.
    ///
    /// @param laid the place
    void follow_card(const Geometry& laid) {
        bool over_button = false;
        bool over_card = false;
        if (laid.ok && have_pointer_) {
            over_button = holds_pixel(laid.view, frame_point(view_, pointer_layer_));
            over_card = (laid.card && kit::contains(*laid.card, pointer_layer_)) ||
                        (laid.arrow && kit::contains(*laid.arrow, pointer_layer_));
        }
        card_.over_card(over_card);
        (void)card_.pointer(
            over_button ? card_slot : mp::presence_card_no_slot,
            static_cast<uint64_t>(SDL_GetTicks())
        );
    }

    /// Takes the focus onto a button and clears the battle room's own.
    ///
    /// @param which the button
    /// @return redraw
    LayerAnswer take_focus(Held which) {
        focused_ = which;
        pressed_ = kit::no_control;
        mp::multiplayer_set_battle_room_focus(mp::kNoControl);
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Hands the focus to a record and clears the row's.
    ///
    /// @param index the record
    /// @return redraw
    LayerAnswer hand_focus(int32_t index) {
        focused_ = Held::none;
        mp::multiplayer_set_battle_room_focus(index);
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Answers one key, for the keyboard and for the pad actions that become
    /// keys. GET MUTATORS, while it shows, comes before the button in the
    /// row, as it stands to its left.
    ///
    /// @param pressed the key
    /// @return none or redraw when the row takes it; pass otherwise
    LayerAnswer answer_key(kit::Key pressed) {
        const Geometry laid = geometry(view_);
        if (!laid.ok)
            return LayerAnswer::pass;
        if (pressed == kit::Key::yes) {
            open_view();
            return LayerAnswer::none;
        }
        const Held first = laid.fetch_button ? Held::fetch : Held::view;
        const int32_t current = mp::multiplayer_battle_room_focus();
        if (focused_ == Held::none) {
            if (pressed == kit::Key::tab && current == laid.before && laid.before > 0)
                return take_focus(first);
            if (pressed == kit::Key::back_tab && current == laid.after && laid.after > 0)
                return take_focus(Held::view);
            if (pressed == kit::Key::down && current == laid.anchor_index && laid.anchor_index > 0)
                return take_focus(Held::view);
            if (pressed == kit::Key::up && current == laid.below && laid.below > 0)
                return take_focus(Held::view);
            return LayerAnswer::pass;
        }
        if (pressed == kit::Key::enter || pressed == kit::Key::space) {
            if (focused_ == Held::fetch && laid.fetch_button)
                runtime_.fetch_host_mutators();
            else
                open_view();
            return LayerAnswer::none;
        }
        if (pressed == kit::Key::tab) {
            if (focused_ == Held::fetch)
                return take_focus(Held::view);
            const int32_t next = laid.after > 0 ? laid.after : laid.first;
            return next > 0 ? hand_focus(next) : LayerAnswer::none;
        }
        if (pressed == kit::Key::back_tab) {
            if (focused_ == Held::view && laid.fetch_button)
                return take_focus(Held::fetch);
            const int32_t next = laid.before > 0 ? laid.before : laid.last;
            return next > 0 ? hand_focus(next) : LayerAnswer::none;
        }
        const place::PlaceRect from =
            focused_ == Held::fetch && laid.fetch_button ? laid.fetch_points : laid.view_points;
        if (pressed == kit::Key::right) {
            if (focused_ == Held::fetch)
                return take_focus(Held::view);
            const int32_t next = nearest_record(laid.focusable, from, Aim::right);
            return next > 0 ? hand_focus(next) : LayerAnswer::none;
        }
        if (pressed == kit::Key::left) {
            if (focused_ == Held::view && laid.fetch_button)
                return take_focus(Held::fetch);
            const int32_t next = nearest_record(laid.focusable, from, Aim::left);
            return next > 0 ? hand_focus(next) : LayerAnswer::none;
        }
        if (pressed == kit::Key::up || pressed == kit::Key::down) {
            const int32_t next =
                nearest_record(laid.focusable, from, pressed == kit::Key::up ? Aim::up : Aim::down);
            return next > 0 ? hand_focus(next) : LayerAnswer::none;
        }
        return LayerAnswer::pass;
    }

    /// Opens the picker read-only on the host's set. USE THESE is not
    /// offered. With no set, nothing opens.
    void open_view() {
        oa::netgame::PresenceLoadoutAssembly assembly;
        if (!mp::multiplayer_room_mutators(&assembly))
            return;
        MutatorPickerRequest request;
        request.setup = picker::Setup::multiplayer;
        request.read_only = true;
        request.set.reserve(assembly.mutators.size());
        for (const oa::netgame::PresenceMutator& item : assembly.mutators) {
            picker::Chosen chosen;
            chosen.id = item.id;
            chosen.options.reserve(item.options.size());
            for (const oa::netgame::PresenceMutatorOption& option : item.options)
                chosen.options.emplace_back(option.key, option.value);
            request.set.push_back(std::move(chosen));
        }
        runtime_.open_mutator_picker(std::move(request));
    }

    /// A text that changes when what the block shows changes.
    ///
    /// @param laid the place
    /// @return the text
    [[nodiscard]] std::string stamp_of(const Geometry& laid) const {
        std::string stamp = laid.ok ? "1" : "0";
        stamp += laid.fetch_button ? "b" : "";
        stamp += laid.bar ? "p" : "";
        stamp += std::to_string(laid.bar_done);
        stamp += laid.line;
        stamp += laid.ready_text;
        // Each turn of a blinking lamp is a new picture; a steady lamp asks for none.
        stamp += std::to_string(
            static_cast<int32_t>(
                room::room_lamp_shown(lamp(laid), static_cast<uint64_t>(SDL_GetTicks()))
            )
        );
        stamp += std::to_string(laid.view.x) + ',' + std::to_string(laid.view.y) + ',' +
                 std::to_string(laid.view.width);
        stamp += std::to_string(laid.side.x) + ',' + std::to_string(laid.side.width);
        stamp += std::to_string(card_.open_slot());
        stamp += std::to_string(hovered_) + std::to_string(pressed_);
        return stamp;
    }

    Runtime& runtime_;                        ///< the runtime
    LayerView view_{};                        ///< the view it was last laid out for
    kit::ControlId hovered_{kit::no_control}; ///< the button under the pointer
    kit::ControlId pressed_{kit::no_control}; ///< the button a press is held on
    Held focused_{Held::none};                ///< the button the keyboard focus is on
    uint64_t revision_{};                     ///< counts the changes to how it looks
    std::string stamp_{};                     ///< what it last showed
    mp::PresenceCardTrigger card_{};          ///< when the card opens and closes
    bool have_pointer_{};                     ///< the pointer has been seen
    kit::Point pointer_layer_{};              ///< the pointer, in the layer's points
    /// The battle room's art, read on its first use; nothing when it could not be read.
    mutable std::optional<room::ScreenResources> art_{};
    mutable bool art_read_{}; ///< the battle room's art has been read
};

bool Runtime::ModInstallState::get_mutators_shows(const Runtime& runtime) {
    // The battle room shows with no dialog over it, the host plays a set, and
    // the guest's block has a place there.
    mp::BattleRoomMap room;
    if (!runtime.oa_layer_ || !mp::multiplayer_battle_room_map(&room) || !room.joiner)
        return false;
    oa::netgame::PresenceLoadoutAssembly assembly;
    if (!mp::multiplayer_room_mutators(&assembly) || assembly.mutators.empty())
        return false;
    const LayerPlacement placed = runtime.oa_layer_->placement_of(screen_name);
    return placed.shown.width > 0 && placed.shown.height > 0;
}

void Runtime::keep_get_mutators_screen() {
    if (screen_ == Screen::match || !mp::multiplayer_showing())
        return;
    OaLayer& layer = oa_layer();
    if (layer.find(screen_name) != nullptr)
        return;
    layer.push(std::make_unique<GetMutatorsScreen>(*this));
}

} // namespace oa::app
