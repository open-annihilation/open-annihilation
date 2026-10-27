// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Arrange overlays: where OA's overlays over a running match show, and the
// screen that moves them. It opens from the in-game menu's Overlays button,
// drawn in the menu's own art, over the overlays' area: a veil and a grid
// over the battlefield, a handle on the keys overlay, the chat line and the
// latch chips, each where the overlay shows, and a card with Reset and Done
// where no handle lies, stepping aside as one is dragged under it. A handle
// drags on an 8-point grid (overlay_placement), an arrow moves the focused
// one, a double-click puts one back where it lays itself out, and Reset puts
// every one back. Each place is kept for the size class in effect.

#include "oa_layer.hpp"

#include "oa/app/runtime.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/formats/fnt.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace kit = oa::ui::kit;
namespace layout = oa::ui::display_layout;
namespace places = overlay_placement;

/// Arrange overlays' name on the layer; automation names its dialog
/// oa.arrange-overlays.
constexpr std::string_view kScreenName = "arrange-overlays";

/// The card's title.
constexpr std::string_view kTitleWords = "Arrange Overlays";
/// The card's hint.
constexpr std::string_view kHintWords =
    "Drag an overlay where it suits you. Each screen size keeps its own places.";
/// The button that puts every overlay back.
constexpr std::string_view kResetWords = "Reset";
/// The button that closes the screen.
constexpr std::string_view kDoneWords = "Done";
/// Each overlay's handle's words, in overlay_placement::overlays' order.
constexpr std::array<std::string_view, 3> kHandleWords{"Keys Overlay", "Chat Line", "Latch Chips"};

/// The first handle's control number; the others follow in order.
constexpr kit::ControlId kFirstHandle = 1;
/// Reset's control number.
constexpr kit::ControlId kResetId = 10;
/// Done's control number.
constexpr kit::ControlId kDoneId = 11;

/// The share of each pixel of the overlays' area the veil darkens, in 256ths.
constexpr uint32_t kVeilOpacity = 96;
/// The grid's lines are this many points apart: every second grid step.
constexpr int32_t kGridLinePoints = 2 * places::grid_points;
/// The share of each pixel a grid line lightens, in 256ths.
constexpr uint32_t kGridOpacity = 18;
/// The share of a handle the accent tints, in 256ths, at rest and held.
constexpr uint32_t kHandleOpacity = 36;
constexpr uint32_t kHeldOpacity = 80;
/// A handle's dashes and the gaps between them, in points.
constexpr int32_t kDashPoints = 4;
constexpr int32_t kDashGapPoints = 3;
/// A handle's grip: a square of this many points beside its words.
constexpr int32_t kGripPoints = 10;
/// The grip's corners' radius, in points.
constexpr int32_t kGripRadius = 3;
/// The latch chips lay themselves out this many points under the overlays'
/// area's top (Runtime::draw_key_latch_chips).
constexpr int32_t kChipsTopPoints = 6;
/// The columns between two latch chips (Runtime::draw_key_latch_chips).
constexpr int32_t kChipGapPoints = 4;
/// The latch chips' handle is at least this wide while no chip is on, in
/// the chips' points: about one chip.
constexpr int32_t kChipsLeastPoints = 96;
/// The rows the chat line keeps above and below its text, in the
/// battlefield's text points (Runtime::draw_risen_chat_line).
constexpr int32_t kChatInsetPoints = 2;

/// Returns a length in canvas pixels as points at a scale, rounded down.
///
/// @param pixels the length, in pixels; 0 or more
/// @param scale_percent the scale, in percent, above 0
/// @return the points
[[nodiscard]] int32_t points_down(int32_t pixels, int32_t scale_percent) noexcept {
    const int64_t scaled = int64_t{pixels} * 100;
    return static_cast<int32_t>(
        scaled >= 0 ? scaled / scale_percent : -((-scaled + scale_percent - 1) / scale_percent)
    );
}

/// Returns a length in canvas pixels as points at a scale, rounded up.
///
/// @param pixels the length, in pixels; 0 or more
/// @param scale_percent the scale, in percent, above 0
/// @return the points
[[nodiscard]] int32_t points_up(int32_t pixels, int32_t scale_percent) noexcept {
    return static_cast<int32_t>(
        (int64_t{std::max(pixels, 0)} * 100 + scale_percent - 1) / scale_percent
    );
}

/// Returns a rectangle of canvas pixels as points of the overlays' area,
/// from its top left corner: the corner rounded down, the size up.
///
/// @param room the overlays' room
/// @param pixels the rectangle, in canvas pixels
/// @return the rectangle, in points
[[nodiscard]] kit::Rect
points_in(const OaLayer::OverlayRoom& room, const layout::Rect& pixels) noexcept {
    return {
        points_down(pixels.x - room.area.x, room.scale_percent),
        points_down(pixels.y - room.area.y, room.scale_percent),
        points_up(pixels.width, room.scale_percent),
        points_up(pixels.height, room.scale_percent)
    };
}

/// Returns a word in the language shown.
///
/// @param english the word, in English
/// @return its translation, or the English
[[nodiscard]] std::string shown(std::string_view english) {
    return std::string(oa::data::languages::interface_text(english));
}

/// Draws a rectangle's rounded outline in dashes: whole quarter circles at
/// its corners and dashes along its sides between them.
///
/// @param canvas where it is drawn
/// @param rect the rectangle, in points
/// @param radius its corners' radius, in points
/// @param ink the colour
void draw_dashed_outline(
    const kit::Canvas& canvas, const kit::Rect& rect, int32_t radius, kit::Colour ink
) {
    if (rect.width <= 0 || rect.height <= 0)
        return;
    const int32_t corner = std::clamp(radius, int32_t{0}, std::min(rect.width, rect.height) / 2);
    const int32_t right = rect.x + rect.width;
    const int32_t bottom = rect.y + rect.height;
    if (corner > 0) {
        const std::array<kit::Rect, 4> squares{{
            {rect.x, rect.y, corner, corner},
            {right - corner, rect.y, corner, corner},
            {rect.x, bottom - corner, corner, corner},
            {right - corner, bottom - corner, corner, corner},
        }};
        for (const kit::Rect& square : squares)
            kit::outline_corner_rect(kit::clipped(canvas, square), rect, corner, ink);
    }
    const int32_t step = kDashPoints + kDashGapPoints;
    for (int32_t x = rect.x + corner; x < right - corner; x += step) {
        const int32_t length = std::min(kDashPoints, right - corner - x);
        kit::fill_rect(canvas, {x, rect.y, length, 1}, ink);
        kit::fill_rect(canvas, {x, bottom - 1, length, 1}, ink);
    }
    for (int32_t y = rect.y + corner; y < bottom - corner; y += step) {
        const int32_t length = std::min(kDashPoints, bottom - corner - y);
        kit::fill_rect(canvas, {rect.x, y, 1, length}, ink);
        kit::fill_rect(canvas, {right - 1, y, 1, length}, ink);
    }
}

/// Returns how many points two rectangles share.
///
/// @param first a rectangle
/// @param second another
/// @return the area they share, in square points; 0 when they do not meet
[[nodiscard]] int64_t shared_area(const kit::Rect& first, const kit::Rect& second) noexcept {
    const int64_t across =
        std::min(first.x + first.width, second.x + second.width) - std::max(first.x, second.x);
    const int64_t down =
        std::min(first.y + first.height, second.y + second.height) - std::max(first.y, second.y);
    return across > 0 && down > 0 ? across * down : 0;
}

/// Returns where Arrange Overlays' card goes: where it covers the least of
/// the handles, none where it can, on the grid a margin in from the area's
/// edges, and of those the place nearest the middle of the area's bottom,
/// then of its top.
///
/// @param whole the overlays' area, in points from its corner
/// @param size the card's width and height, in points
/// @param handles the handles, in the same points
/// @param margin the points kept clear of the area's edges
/// @return the card
[[nodiscard]] kit::Rect card_place(
    const kit::Rect& whole, kit::Point size, const std::vector<kit::Rect>& handles, int32_t margin
) {
    const int32_t left = margin;
    const int32_t right = std::max(margin, whole.width - size.x - margin);
    const int32_t top = margin;
    const int32_t bottom = std::max(margin, whole.height - size.y - margin);
    const int32_t middle = std::max((whole.width - size.x) / 2, 0);
    // Each axis's places: the grid from the near margin, the middle, and the
    // far margin.
    const auto places_along = [](int32_t first, int32_t last, int32_t also) {
        std::vector<int32_t> along;
        for (int32_t at = first; at < last; at += places::grid_points)
            along.push_back(at);
        along.push_back(last);
        along.push_back(std::clamp(also, first, last));
        return along;
    };
    kit::Rect best{middle, bottom, size.x, size.y};
    int64_t best_covered = -1;
    int64_t best_distance = 0;
    for (const int32_t y : places_along(top, bottom, bottom))
        for (const int32_t x : places_along(left, right, middle)) {
            const kit::Rect card{x, y, size.x, size.y};
            int64_t covered = 0;
            for (const kit::Rect& handle : handles)
                covered += shared_area(card, handle);
            // Nearest the bottom's middle, then the top's: a step further
            // from the bottom counts a point more.
            const int64_t across = std::abs(int64_t{x} - middle);
            const int64_t distance = std::min(
                across + std::abs(int64_t{y} - bottom), across + std::abs(int64_t{y} - top) + 1
            );
            if (best_covered < 0 || covered < best_covered ||
                (covered == best_covered && distance < best_distance)) {
                best = card;
                best_covered = covered;
                best_distance = distance;
            }
        }
    return best;
}

} // namespace

kit::Rect OaLayer::OverlayRoom::points() const noexcept {
    const int32_t percent = scale_percent > 0 ? scale_percent : 100;
    return {0, 0, points_down(area.width, percent), points_down(area.height, percent)};
}

std::optional<OaLayer::OverlayRoom> OaLayer::overlay_room() const {
    if (runtime_.screen_ != Screen::match || runtime_.match_ == nullptr)
        return std::nullopt;
    const LayerView seen = view();
    if (!seen.match || seen.match_layout == nullptr)
        return std::nullopt;
    const MatchDialogPlace place = match_dialog(
        *seen.match_layout, seen.interface_percent, seen.fit_safe_area, seen.window_size
    );
    OverlayRoom room;
    room.area = runtime_.overlay_area();
    room.scale_percent = place.scale_percent > 0 ? place.scale_percent : 100;
    room.size_class = place.size_class;
    if (room.area.width <= 0 || room.area.height <= 0)
        return std::nullopt;
    return room;
}

layout::Rect OaLayer::overlay_shown(std::string_view overlay, const layout::Rect& unplaced) const {
    if (unplaced.width <= 0 || unplaced.height <= 0)
        return unplaced;
    const auto room = overlay_room();
    if (!room)
        return unplaced;
    const auto place = places::stored_place(runtime_.preference_values_, overlay, room->size_class);
    if (!place)
        return unplaced;
    const kit::Rect size = points_in(*room, unplaced);
    const kit::Rect at = places::rect_at(room->points(), {size.width, size.height}, *place);
    const layout::Rect& area = room->area;
    // As large as it is, at the place, kept in the overlays' area.
    const auto along = [&room](int32_t points, int32_t start, int32_t length, int32_t room_length) {
        const int32_t pixel = start + window_placement::pixels_of(points, room->scale_percent);
        return std::clamp(pixel, start, start + std::max(room_length - length, int32_t{0}));
    };
    return {
        along(at.x, area.x, unplaced.width, area.width),
        along(at.y, area.y, unplaced.height, area.height),
        unplaced.width,
        unplaced.height
    };
}

std::vector<OaLayer::ArrangedOverlay> OaLayer::arranged_overlays() const {
    std::vector<ArrangedOverlay> arranged;
    const auto room = overlay_room();
    if (!room)
        return arranged;
    const layout::Rect& area = room->area;
    const int scale = std::max(1, runtime_.hud_text_scale());
    // The keys overlay's panel, as it lays itself out.
    arranged.push_back({places::keys_overlay, keys_overlay_unplaced()});
    // The chat line across the area, standing on its bottom edge, as tall as
    // a line of the battlefield's text and the rows round it.
    {
        const oa::formats::fnt::Font* font = runtime_.match_label_font();
        const int text_height =
            (font != nullptr ? static_cast<int>(oa::formats::fnt::line_height(*font)) : 12) * scale;
        const int height = std::min(text_height + 2 * kChatInsetPoints * scale, area.height);
        arranged.push_back(
            {places::chat_line, {area.x, area.y + area.height - height, area.width, height}}
        );
    }
    // The latch chips centred under the area's top, as wide as the chips on
    // now, or about one chip while none is.
    {
        const kit::Fonts* fonts = runtime_.engine_settings_fonts();
        std::vector<Runtime::KeyLatchChip> chips;
        runtime_.gather_key_latch_chips(chips);
        int32_t total = 0;
        for (const Runtime::KeyLatchChip& chip : chips) {
            kit::ChipLook look;
            look.text = chip.text;
            look.state = chip.filled ? kit::ChipState::playing : kit::ChipState::installed;
            const int32_t width = fonts != nullptr ? kit::chip_width(*fonts, look)
                                                   : 2 * kit::compact_metrics.chip_padding;
            total += width + (total > 0 ? kChipGapPoints : 0);
        }
        const int32_t span = area.width / scale;
        total = std::min(std::max(total, kChipsLeastPoints), span);
        arranged.push_back(
            {places::latch_chips,
             {area.x + (span - total) / 2 * scale,
              area.y + kChipsTopPoints * scale,
              total * scale,
              kit::compact_metrics.chip_height * scale}}
        );
    }
    return arranged;
}

std::optional<places::OverlayPlace> OaLayer::overlay_place(std::string_view overlay) const {
    const auto room = overlay_room();
    if (!room)
        return std::nullopt;
    return places::stored_place(runtime_.preference_values_, overlay, room->size_class);
}

bool OaLayer::place_overlay(std::string_view overlay, const places::OverlayPlace& place) {
    const auto room = overlay_room();
    if (!room || !places::arranged(overlay))
        return false;
    const bool changed =
        places::store_place(runtime_.preference_values_, overlay, room->size_class, place);
    if (changed)
        write_window_preferences();
    ++revision_;
    lay_out_screens();
    return changed;
}

bool OaLayer::forget_overlay_place(std::string_view overlay) {
    const auto room = overlay_room();
    if (!room)
        return false;
    const bool forgotten =
        places::forget_place(runtime_.preference_values_, overlay, room->size_class);
    if (forgotten)
        write_window_preferences();
    ++revision_;
    lay_out_screens();
    return forgotten;
}

bool OaLayer::reset_overlay_places() {
    const bool forgotten = places::forget_places(runtime_.preference_values_);
    if (forgotten)
        write_window_preferences();
    ++revision_;
    lay_out_screens();
    return forgotten;
}

namespace {

/// Arrange overlays as a screen of the OA layer: modal over the overlays'
/// area, not opaque, with no backdrop of the layer's own (it veils the area
/// itself), its points the overlays' places' points (OaLayer::overlay_room).
class ArrangeScreen final : public LayerScreen {
  public:

    /// Makes the screen.
    ///
    /// @param layer the layer it shows on, which keeps the places
    explicit ArrangeScreen(OaLayer& layer) : layer_(layer) {}

    /// Returns "arrange-overlays".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return kScreenName; }

    /// Returns the overlays' area at the places' scale.
    ///
    /// @param view what the screen is placed on
    /// @return its place; empty off a match or before it is laid out
    [[nodiscard]] LayerPlacement placement(const LayerView& view) const override {
        if (!laid_ || !view.match)
            return {};
        const kit::Rect whole = room_.points();
        const kit::Rect drawn = kit::to_canvas(whole, room_.scale_percent);
        return {
            {room_.area.x, room_.area.y, drawn.width, drawn.height},
            whole.width,
            whole.height,
            room_.scale_percent
        };
    }

    /// Tells that the screen takes every input.
    ///
    /// @return true
    [[nodiscard]] bool modal() const override { return true; }

    /// Tells that the layer darkens nothing under it: the screen veils the
    /// overlays' area itself, and the side column stays as it is.
    ///
    /// @return false
    [[nodiscard]] bool backdrop() const override { return false; }

    /// Tells that the screen is laid over the battlefield by its opacity.
    ///
    /// @return false
    [[nodiscard]] bool opaque() const override { return false; }

    /// Tells that what the screen draws changes only with its revision.
    ///
    /// @return true
    [[nodiscard]] bool revision_covers_drawing() const override { return true; }

    /// Lays the handles and the card out for the overlays' area now.
    ///
    /// @param view what the screen is placed on
    void lay_out(const LayerView& view) override {
        const auto room = view.match ? layer_.overlay_room() : std::nullopt;
        const kit::Fonts* fonts = layer_.screen_fonts();
        if (!room || fonts == nullptr) {
            laid_ = false;
            return;
        }
        const bool moved =
            !laid_ || room->area.x != room_.area.x || room->area.y != room_.area.y ||
            room->area.width != room_.area.width || room->area.height != room_.area.height ||
            room->scale_percent != room_.scale_percent || room->size_class != room_.size_class;
        room_ = *room;
        if (moved || overlays_.empty())
            overlays_ = layer_.arranged_overlays();
        place_handles(*fonts);
        laid_ = true;
    }

    /// Draws the veil and the grid over the overlays' area, the card, and a
    /// handle on each overlay.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override {
        if (!laid_ || !kit::can_draw(canvas))
            return;
        const kit::Metrics& metrics = kit::metrics_of(room_.size_class);
        const kit::Rect whole = room_.points();
        kit::blend_rect(canvas, whole, kit::colour::band, kVeilOpacity);
        for (int32_t x = kGridLinePoints; x < whole.width; x += kGridLinePoints)
            kit::blend_rect(canvas, {x, 0, 1, whole.height}, kit::colour::text, kGridOpacity);
        for (int32_t y = kGridLinePoints; y < whole.height; y += kGridLinePoints)
            kit::blend_rect(canvas, {0, y, whole.width, 1}, kit::colour::text, kGridOpacity);

        // A handle on each overlay, the one held over the rest.
        for (std::size_t index = 0; index < handles_.size(); ++index)
            if (!drag_ || drag_->index != index)
                draw_handle(canvas, index);
        if (drag_)
            draw_handle(canvas, drag_->index);

        // The card over them: its title, its hint, Reset and Done.
        kit::fill_corner_rect(canvas, card_, metrics.card_radius, kit::colour::panel);
        kit::outline_corner_rect(canvas, card_, metrics.card_radius, kit::colour::strong_edge);
        kit::box_text(
            canvas, kit::FontRole::regular, title_, title_box_, kit::Align::left, kit::colour::text
        );
        for (std::size_t line = 0; line < hint_lines_.size() && line < hint_boxes_.size(); ++line)
            kit::box_text(
                canvas,
                kit::FontRole::small,
                hint_lines_[line],
                hint_boxes_[line],
                kit::Align::left,
                kit::colour::text_secondary
            );
        draw_card_button(canvas, kResetId, reset_box_, reset_words_, kit::ButtonStyle::plain);
        draw_card_button(canvas, kDoneId, done_box_, done_words_, kit::ButtonStyle::accent);
    }

    /// Takes a pointer: a press on a handle drags it, a double-click puts it
    /// back where it lays itself out, and Reset and Done act on their
    /// release.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button, 1 for the left
    /// @param at the pointer, in the screen's points
    /// @param finger_reach how far a finger may lie from a control; 0 for a mouse
    /// @param clicks how many presses landed as one
    /// @return what the screen did
    LayerAnswer pointer_with_clicks(
        ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach, uint8_t clicks
    ) override {
        if (!laid_)
            return LayerAnswer::none;
        if (kind == ScreenInputKind::pointer_move) {
            if (drag_) {
                const kit::Point travel{at.x - drag_->grab.x, at.y - drag_->grab.y};
                const places::OverlayPlace place =
                    places::dragged_place(room_.points(), drag_->start, travel);
                drag_->moved = drag_->moved || travel.x != 0 || travel.y != 0;
                if (place == drag_->place)
                    return LayerAnswer::none;
                drag_->place = place;
                relay_out();
                return changed();
            }
            const kit::PointerOutcome moved = kit::pointer_move(interaction_, list_, at);
            return moved.result != kit::PointerResult::none ? changed() : LayerAnswer::none;
        }
        if (kind == ScreenInputKind::pointer_down) {
            if (button != SDL_BUTTON_LEFT)
                return LayerAnswer::none;
            const kit::ControlId pressed = finger_reach > 0
                                               ? kit::reach(list_, at, finger_reach).control
                                               : kit::hit(list_, at);
            // The card lies over the handles: a press on it reaches its
            // buttons alone.
            if (kit::contains(card_, at) && handle_of(pressed))
                return LayerAnswer::none;
            if (const auto index = handle_of(pressed)) {
                interaction_.focused = pressed;
                if (clicks >= 2) {
                    // A double-click puts the overlay back where it lays
                    // itself out.
                    drag_.reset();
                    layer_.forget_overlay_place(handles_[*index].name);
                    relay_out();
                    return changed();
                }
                Drag drag;
                drag.index = *index;
                drag.grab = at;
                drag.start = handles_[*index].shown;
                drag.place = places::place_at(room_.points(), handles_[*index].shown);
                drag_ = drag;
                interaction_.pressed = pressed;
                return changed();
            }
            const kit::PointerOutcome down =
                finger_reach > 0 ? kit::finger_down(interaction_, list_, at, finger_reach)
                                 : kit::pointer_down(interaction_, list_, at);
            return down.result != kit::PointerResult::none ? changed() : LayerAnswer::none;
        }
        if (kind != ScreenInputKind::pointer_up || button != SDL_BUTTON_LEFT)
            return LayerAnswer::none;
        if (drag_) {
            const Drag ended = *drag_;
            drag_.reset();
            interaction_.pressed = kit::no_control;
            if (ended.moved)
                layer_.place_overlay(handles_[ended.index].name, ended.place);
            relay_out();
            return changed();
        }
        const kit::PointerOutcome released = kit::pointer_up(interaction_, list_, at);
        if (released.result == kit::PointerResult::activated)
            return act(released.control);
        return released.result != kit::PointerResult::none ? changed() : LayerAnswer::none;
    }

    /// Takes a pointer with no count of its presses, as one press.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the screen's points
    /// @param finger_reach a finger's reach, in the screen's points; 0 for a mouse
    /// @return what the screen did
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        return pointer_with_clicks(kind, button, at, finger_reach, 1);
    }

    /// Takes a key: Escape closes the screen, an arrow moves the focused
    /// handle a grid step (Shift a larger one), Tab and Shift+Tab move the
    /// focus, and Enter and Space press the focused button.
    ///
    /// @param pressed the key
    /// @return what the screen did
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        if (!laid_)
            return pressed == kit::Key::escape ? LayerAnswer::close : LayerAnswer::none;
        if (pressed == kit::Key::escape)
            return LayerAnswer::close;
        const auto focused = handle_of(interaction_.focused);
        if (focused && interaction_.focus_shown) {
            int32_t across = 0;
            int32_t down = 0;
            if (pressed == kit::Key::left)
                across = -1;
            else if (pressed == kit::Key::right)
                across = 1;
            else if (pressed == kit::Key::up)
                down = -1;
            else if (pressed == kit::Key::down)
                down = 1;
            if (across != 0 || down != 0) {
                const bool large = (SDL_GetModState() & SDL_KMOD_SHIFT) != 0;
                layer_.place_overlay(
                    handles_[*focused].name,
                    places::moved_place(
                        room_.points(), handles_[*focused].shown, across, down, large
                    )
                );
                relay_out();
                return changed();
            }
        }
        const kit::KeyOutcome outcome = kit::key(interaction_, list_, pressed);
        switch (outcome.result) {
        case kit::KeyResult::to_control:
            if (pressed == kit::Key::enter || pressed == kit::Key::space)
                return act(outcome.control);
            return LayerAnswer::none;
        case kit::KeyResult::accept:
            return LayerAnswer::close;
        case kit::KeyResult::cancel:
            return LayerAnswer::close;
        case kit::KeyResult::redraw:
            return changed();
        default:
            return LayerAnswer::none;
        }
    }

    /// Takes a turn of the wheel, which does nothing here.
    ///
    /// @return none
    LayerAnswer wheel(kit::Point /*at*/, float /*notches*/) override { return LayerAnswer::none; }

    /// Returns the handles, Reset and Done, as automation lists them.
    ///
    /// @return the list
    [[nodiscard]] kit::DisplayList display_list() const override { return list_; }

    /// Returns the hover, the press and the focus.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override { return interaction_; }

    /// Closes the screen once its match is no longer shown.
    ///
    /// @return close then; otherwise none
    LayerAnswer tick() override {
        return layer_.overlay_room() ? LayerAnswer::none : LayerAnswer::close;
    }

    /// Returns the revision.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Ends a drag under way; its place is not kept.
    void close(bool /*by_key*/) override { drag_.reset(); }

  private:

    /// One overlay's handle.
    struct Handle {
        std::string_view name{}; ///< the overlay's name
        std::string words{};     ///< its words, in the language shown
        kit::Rect shown{};       ///< the handle, in the screen's points
    };

    /// A handle being dragged.
    struct Drag {
        std::size_t index{};          ///< the handle
        kit::Point grab{};            ///< where the press landed, in the screen's points
        kit::Rect start{};            ///< where the handle showed when pressed
        places::OverlayPlace place{}; ///< the place the drag gives the overlay now
        bool moved{};                 ///< the pointer has moved since the press
    };

    /// Counts a change to what the screen draws.
    ///
    /// @return redraw
    LayerAnswer changed() noexcept {
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Lays the handles out again for the places now.
    void relay_out() {
        if (const kit::Fonts* fonts = layer_.screen_fonts())
            place_handles(*fonts);
    }

    /// Returns the handle a control is.
    ///
    /// @param control the control's number
    /// @return the handle's index; nothing for another control
    [[nodiscard]] std::optional<std::size_t> handle_of(kit::ControlId control) const noexcept {
        if (control < kFirstHandle)
            return std::nullopt;
        const auto index = static_cast<std::size_t>(control - kFirstHandle);
        if (index >= handles_.size())
            return std::nullopt;
        return index;
    }

    /// Carries out a button's press.
    ///
    /// @param control the button
    /// @return close for Done; redraw for Reset; otherwise none
    LayerAnswer act(kit::ControlId control) {
        if (control == kDoneId)
            return LayerAnswer::close;
        if (control == kResetId) {
            drag_.reset();
            layer_.reset_overlay_places();
            relay_out();
            return changed();
        }
        return LayerAnswer::none;
    }

    /// Places each handle where its overlay shows, the card where it covers
    /// the least of them (card_place), and lists the controls.
    ///
    /// @param fonts the fonts the words are measured in
    void place_handles(const kit::Fonts& fonts) {
        const kit::Metrics& metrics = kit::metrics_of(room_.size_class);
        const kit::Rect whole = room_.points();
        handles_.clear();
        for (std::size_t index = 0; index < overlays_.size(); ++index) {
            const OaLayer::ArrangedOverlay& overlay = overlays_[index];
            Handle handle;
            handle.name = overlay.name;
            const auto word =
                std::find(places::overlays.begin(), places::overlays.end(), overlay.name);
            handle.words = shown(
                kHandleWords
                    [static_cast<std::size_t>(std::distance(places::overlays.begin(), word))]
            );
            const kit::Rect unplaced = points_in(room_, overlay.unplaced);
            const kit::Point least{
                kit::text_width(fonts, kit::FontRole::small, handle.words) + kGripPoints +
                    metrics.space * 4,
                metrics.control_height
            };
            const kit::Point size{
                std::max(unplaced.width, least.x), std::max(unplaced.height, least.y)
            };
            std::optional<places::OverlayPlace> place = layer_.overlay_place(overlay.name);
            if (drag_ && drag_->index == index)
                place = drag_->place;
            handle.shown = place ? places::rect_at(whole, size, *place)
                                 : places::handle_rect(whole, unplaced, least);
            handles_.push_back(std::move(handle));
        }

        // The card: the title, the hint wrapped to the card, Reset and Done.
        title_ = shown(kTitleWords);
        reset_words_ = shown(kResetWords);
        done_words_ = shown(kDoneWords);
        const int32_t pad = metrics.padding;
        const int32_t gap = metrics.space * 2;
        const auto button_width = [&](const std::string& words) {
            return std::max(
                metrics.button_width,
                kit::text_width(fonts, kit::FontRole::small, words) + 2 * metrics.chip_padding
            );
        };
        const int32_t reset_width = button_width(reset_words_);
        const int32_t done_width = button_width(done_words_);
        const int32_t widest = std::max(
            {kit::text_width(fonts, kit::FontRole::regular, title_),
             reset_width + gap + done_width,
             metrics.notice_width - 2 * pad}
        );
        const int32_t inner = std::max(std::min(widest, whole.width - 2 * pad - 2 * gap), 1);
        const kit::Measure measure = [&fonts](std::string_view text) {
            return kit::text_width(fonts, kit::FontRole::small, text);
        };
        hint_lines_ = kit::wrap(shown(kHintWords), inner, measure);
        const int32_t height = pad + metrics.regular_line + metrics.space +
                               static_cast<int32_t>(hint_lines_.size()) * metrics.small_line + gap +
                               metrics.button_height + pad;
        const int32_t width = inner + 2 * pad;
        std::vector<kit::Rect> handle_rects;
        for (const Handle& placed : handles_)
            handle_rects.push_back(placed.shown);
        card_ = card_place(whole, {width, height}, handle_rects, gap);
        int32_t cursor = card_.y + pad;
        title_box_ = {card_.x + pad, cursor, inner, metrics.regular_line};
        cursor += metrics.regular_line + metrics.space;
        hint_boxes_.clear();
        for (std::size_t line = 0; line < hint_lines_.size(); ++line) {
            hint_boxes_.push_back({card_.x + pad, cursor, inner, metrics.small_line});
            cursor += metrics.small_line;
        }
        cursor += gap;
        done_box_ = {
            card_.x + card_.width - pad - done_width, cursor, done_width, metrics.button_height
        };
        reset_box_ = {done_box_.x - gap - reset_width, cursor, reset_width, metrics.button_height};

        // The controls: the card's buttons first, as the card lies over the
        // handles, then the handles, the last drawn first.
        list_ = {};
        const auto add_button = [this](
                                    kit::ControlId id,
                                    const kit::Rect& box,
                                    std::string_view word,
                                    const std::string& words
                                ) {
            kit::Control control;
            control.id = id;
            control.rect = box;
            control.name = std::string(kScreenName) + '.' + std::string(word);
            control.kind = kit::ControlKind::button;
            control.text = words;
            list_.controls.push_back(std::move(control));
        };
        add_button(kResetId, reset_box_, "reset", reset_words_);
        add_button(kDoneId, done_box_, "done", done_words_);
        for (std::size_t index = handles_.size(); index > 0; --index) {
            const Handle& handle = handles_[index - 1];
            kit::Control control;
            control.id = kFirstHandle + static_cast<kit::ControlId>(index - 1);
            control.rect = handle.shown;
            control.name = std::string(kScreenName) + '.' + std::string(handle.name);
            control.kind = kit::ControlKind::button;
            control.text = handle.words;
            list_.controls.push_back(std::move(control));
        }
        for (std::size_t index = 0; index < handles_.size(); ++index)
            list_.tab_order.push_back(kFirstHandle + static_cast<kit::ControlId>(index));
        list_.tab_order.push_back(kResetId);
        list_.tab_order.push_back(kDoneId);
    }

    /// Draws one of the card's buttons, its hover, press and focus.
    ///
    /// @param canvas where it draws
    /// @param id the button's control number
    /// @param box the button
    /// @param words its caption
    /// @param style its look
    void draw_card_button(
        const kit::Canvas& canvas,
        kit::ControlId id,
        const kit::Rect& box,
        const std::string& words,
        kit::ButtonStyle style
    ) const {
        kit::ButtonLook look;
        look.caption = words;
        look.style = style;
        look.hovered = interaction_.hovered == id;
        look.held = interaction_.pressed == id && interaction_.hovered == id;
        kit::draw_button(canvas, box, look);
        if (interaction_.focus_shown && interaction_.focused == id)
            kit::draw_focus_ring(canvas, box, false);
    }

    /// Draws one handle: the accent's tint, a dashed rounded outline, the
    /// grip and the overlay's words, and its focus.
    ///
    /// @param canvas where it draws
    /// @param index the handle
    void draw_handle(const kit::Canvas& canvas, std::size_t index) const {
        const Handle& handle = handles_[index];
        const kit::Metrics& metrics = kit::metrics_of(room_.size_class);
        const kit::ControlId id = kFirstHandle + static_cast<kit::ControlId>(index);
        const bool held = drag_ && drag_->index == index;
        const bool hovered = interaction_.hovered == id;
        const kit::Rect& box = handle.shown;
        const int32_t radius = metrics.control_radius;
        kit::blend_corner_rect(
            canvas,
            box,
            radius,
            kit::colour::accent,
            held || hovered ? kHeldOpacity : kHandleOpacity
        );
        draw_dashed_outline(canvas, box, radius, kit::colour::accent);
        // The grip and the words at the handle's left, down its middle, or
        // along its top on a handle taller than two rows: the card in the
        // area's middle leaves them showing.
        const int32_t gap = metrics.space;
        const int32_t left = box.x + 2 * metrics.space;
        const int32_t row = metrics.control_height;
        const int32_t middle =
            box.height > 2 * row ? box.y + row / 2 + metrics.space : box.y + box.height / 2;
        kit::fill_corner_rect(
            canvas,
            {left, middle - kGripPoints / 2, kGripPoints, kGripPoints},
            kGripRadius,
            kit::colour::accent
        );
        const int32_t words_left = left + kGripPoints + gap;
        kit::box_text(
            kit::clipped(canvas, box),
            kit::FontRole::small,
            handle.words,
            {words_left,
             middle - metrics.small_line / 2,
             std::max(box.x + box.width - words_left - metrics.space, int32_t{0}),
             metrics.small_line},
            kit::Align::left,
            kit::colour::text
        );
        if (interaction_.focus_shown && interaction_.focused == id)
            kit::draw_focus_ring(canvas, box, false);
    }

    OaLayer& layer_;                                   ///< the layer it shows on
    OaLayer::OverlayRoom room_{};                      ///< the overlays' area as last laid out
    std::vector<OaLayer::ArrangedOverlay> overlays_{}; ///< the overlays, as the room last gave them
    std::vector<Handle> handles_{};                    ///< a handle on each overlay
    std::optional<Drag> drag_{};            ///< the handle being dragged; none between drags
    kit::Rect card_{};                      ///< the card
    std::string title_{};                   ///< the card's title, in the language shown
    kit::Rect title_box_{};                 ///< the title's box
    std::vector<std::string> hint_lines_{}; ///< the hint's lines
    std::vector<kit::Rect> hint_boxes_{};   ///< each hint line's box
    std::string reset_words_{};             ///< Reset's caption
    kit::Rect reset_box_{};                 ///< Reset
    std::string done_words_{};              ///< Done's caption
    kit::Rect done_box_{};                  ///< Done
    kit::DisplayList list_{};               ///< the controls
    kit::Interaction interaction_{};        ///< the hover, the press and the focus
    uint64_t revision_{1};                  ///< changes with what the screen draws
    bool laid_{};                           ///< laid out over a match
};

} // namespace

bool OaLayer::open_arrange_overlays() {
    if (find(kScreenName) != nullptr || screen_fonts() == nullptr || !overlay_room())
        return false;
    push(std::make_unique<ArrangeScreen>(*this));
    lay_out_screens();
    return true;
}

} // namespace oa::app
