// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Arrange overlays: where OA's overlays over a running match show, and the
// screen that places them. While Settings' Move overlays is on, the keys
// overlay, the chat line and the latch chips show at the places the player
// gave them; while it is off they show where they lay themselves out, and
// the places stay kept. Accessibility's Arrange… opens the screen over
// Settings: a picture of the match in the kit's look at the window's
// proportions (the side panel, the top bar, the bottom bar and the
// battlefield), the three overlays drawn with sample content where each
// shows, a handle on each, and a card with Reset, Cancel and Done where no
// handle lies, stepping aside as one is dragged under it. A handle drags on
// an 8-point grid (overlay_placement), an arrow moves the focused one, a
// double-click puts one back where it lays itself out, and Reset puts every
// one back. Each place is kept for the size class a match in the window
// takes; Cancel puts every place back as it was when the screen opened.

#include "oa_layer.hpp"

#include "oa/app/runtime.hpp"
#include "oa/data/input_actions/actions.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/formats/fnt.hpp"
#include "oa/ui/key_editor/overlay.hpp"
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
#include <map>
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
/// The button that closes the screen and keeps the places.
constexpr std::string_view kDoneWords = "Done";
/// The button that closes the screen and puts the places back.
constexpr std::string_view kCancelWords = "Cancel";
/// The picture's parts' words.
constexpr std::string_view kSidePanelWords = "Side panel";
constexpr std::string_view kTopBarWords = "Top bar";
constexpr std::string_view kBottomBarWords = "Bottom bar";
/// The chat line's sample: a line being typed.
constexpr std::string_view kChatSampleWords = "Ready when you are";
/// The latch chip's sample: Queue latched, as the match's chip reads it.
constexpr std::string_view kLatchedWords = "{name} · latched";
constexpr std::string_view kQueueWords = "Queue";
/// The keys overlay's sample: its first lines while playing.
constexpr std::size_t kSampleLines = 4;
/// Each overlay's handle's words, in overlay_placement::overlays' order.
constexpr std::array<std::string_view, 3> kHandleWords{"Keys Overlay", "Chat Line", "Latch Chips"};

/// The least width of the card's words, in points: the hint wraps to it.
constexpr int32_t kCardLeastPoints = 240;

/// The first handle's control number; the others follow in order.
constexpr kit::ControlId kFirstHandle = 1;
/// Reset's control number.
constexpr kit::ControlId kResetId = 10;
/// Done's control number.
constexpr kit::ControlId kDoneId = 11;
/// Cancel's control number.
constexpr kit::ControlId kCancelId = 12;

/// The share of the chat line's sample the band behind its words covers, in 256ths.
constexpr uint32_t kChatBandOpacity = 160;
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

/// Returns the latch chip's sample words: Queue latched, as the match's
/// chip reads it.
///
/// @return the words, in the language shown
[[nodiscard]] std::string sample_chip_words() {
    std::string words = shown(kLatchedWords);
    std::string name = shown(kQueueWords);
    for (char& letter : name)
        if (letter >= 'a' && letter <= 'z')
            letter = static_cast<char>(letter - 'a' + 'A');
    constexpr std::string_view holder = "{name}";
    if (const std::size_t at = words.find(holder); at != std::string::npos)
        words.replace(at, holder.size(), name);
    return words;
}

/// Returns the bit of a key context, or 0 when the registry has none of that name.
///
/// @param name the context's id
/// @return the bit
[[nodiscard]] uint32_t context_bit(std::string_view name) {
    const auto table = oa::data::input_actions::contexts();
    for (std::size_t index = 0; index < table.size() && index < 32; ++index)
        if (table[index].name == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    return 0;
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
    room.window = {0, 0, seen.match_layout->width, seen.match_layout->height};
    room.area = runtime_.overlay_area();
    room.scale_percent = place.scale_percent > 0 ? place.scale_percent : 100;
    room.size_class = place.size_class;
    if (room.area.width <= 0 || room.area.height <= 0)
        return std::nullopt;
    return room;
}

std::optional<OaLayer::OverlayRoom> OaLayer::arranging_room() const {
    if (runtime_.screen_ == Screen::match)
        return overlay_room();
    // A match laid out at the window's canvas: its battlefield, and the
    // in-match windows' class and scale there.
    const SDL_Point canvas = runtime_.match_canvas_point();
    if (canvas.x <= 0 || canvas.y <= 0)
        return std::nullopt;
    const layout::MatchLayout laid =
        layout::make_match_layout(canvas.x, canvas.y, runtime_.match_chrome_most_scale());
    const LayerView seen = view();
    const MatchDialogPlace place =
        match_dialog(laid, seen.interface_percent, false, seen.window_size);
    OverlayRoom room;
    room.window = {0, 0, laid.width, laid.height};
    room.area = {
        laid.battlefield_x(),
        laid.battlefield_y(),
        laid.battlefield_width(),
        laid.battlefield_height()
    };
    room.scale_percent = place.scale_percent > 0 ? place.scale_percent : 100;
    room.size_class = place.size_class;
    if (room.area.width <= 0 || room.area.height <= 0 || room.window.width <= 0 ||
        room.window.height <= 0)
        return std::nullopt;
    return room;
}

bool OaLayer::overlays_move() const {
    return runtime_.engine_settings().move_overlays;
}

layout::Rect OaLayer::overlay_shown(std::string_view overlay, const layout::Rect& unplaced) const {
    if (unplaced.width <= 0 || unplaced.height <= 0 || !overlays_move())
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
    const auto room = overlay_room();
    if (!room)
        return {};
    return arranged_overlays_in(*room, false);
}

std::vector<OaLayer::ArrangedOverlay>
OaLayer::arranged_overlays_in(const OverlayRoom& room, bool samples) const {
    std::vector<ArrangedOverlay> arranged;
    const layout::Rect& area = room.area;
    const int scale = std::max(1, runtime_.hud_text_scale());
    // The keys overlay's panel, as it lays itself out: the sample's, or the
    // match's own.
    if (samples) {
        const oa::ui::key_editor::OverlayLayout sample = keys_overlay_sample(room);
        const int32_t percent = room.scale_percent;
        arranged.push_back(
            {places::keys_overlay,
             {area.x + window_placement::pixels_of(sample.panel.x, percent),
              area.y + window_placement::pixels_of(sample.panel.y, percent),
              window_placement::pixels_of(sample.panel.width, percent),
              window_placement::pixels_of(sample.panel.height, percent)}}
        );
    } else {
        arranged.push_back({places::keys_overlay, keys_overlay_unplaced()});
    }
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
    // now (the sample's one chip), or about one chip while none is.
    {
        const kit::Fonts* fonts = runtime_.engine_settings_fonts();
        std::vector<Runtime::KeyLatchChip> chips;
        if (samples) {
            Runtime::KeyLatchChip chip;
            chip.text = sample_chip_words();
            chip.filled = true;
            chips.push_back(std::move(chip));
        } else {
            runtime_.gather_key_latch_chips(chips);
        }
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

oa::ui::key_editor::OverlayLayout OaLayer::keys_overlay_sample(const OverlayRoom& room) const {
    namespace editor = oa::ui::key_editor;
    const kit::Fonts* fonts = screen_fonts();
    if (fonts == nullptr)
        return {};
    // The keys of a match being played, the first few of them.
    editor::OverlayInputs inputs;
    runtime_.fill_keys_overlay(inputs);
    inputs.contexts = {};
    inputs.contexts.active = context_bit("global") | context_bit("match") | context_bit("play");
    inputs.on.clear();
    std::vector<editor::OverlayGroup> groups = editor::overlay_groups(inputs);
    std::size_t left = kSampleLines;
    std::vector<editor::OverlayGroup> kept;
    for (editor::OverlayGroup& group : groups) {
        if (left == 0)
            break;
        if (group.lines.size() > left)
            group.lines.resize(left);
        left -= group.lines.size();
        kept.push_back(std::move(group));
    }
    // Beside the side column, at the in-match windows' scale, as the match
    // lays the panel out.
    const int32_t percent = room.scale_percent > 0 ? room.scale_percent : 100;
    editor::OverlayRoom placed;
    const int32_t left_edge = points_down(room.area.x, percent);
    const int32_t top_edge = points_down(room.area.y, percent);
    placed.battlefield = {
        left_edge,
        top_edge,
        points_down(room.area.x + room.area.width, percent) - left_edge,
        points_down(room.area.y + room.area.height, percent) - top_edge
    };
    placed.column_right = left_edge;
    placed.size_class = room.size_class;
    editor::OverlayLayout laid = editor::overlay_layout(inputs, kept, placed, *fonts, 0);
    // From the area's top left corner.
    laid.panel.x -= left_edge;
    laid.panel.y -= top_edge;
    return laid;
}

std::optional<places::OverlayPlace>
OaLayer::overlay_place(std::string_view overlay, kit::SizeClass size_class) const {
    return places::stored_place(runtime_.preference_values_, overlay, size_class);
}

bool OaLayer::place_overlay(
    std::string_view overlay, kit::SizeClass size_class, const places::OverlayPlace& place
) {
    if (!places::arranged(overlay))
        return false;
    const bool changed =
        places::store_place(runtime_.preference_values_, overlay, size_class, place);
    if (changed)
        write_window_preferences();
    ++revision_;
    lay_out_screens();
    return changed;
}

bool OaLayer::forget_overlay_place(std::string_view overlay, kit::SizeClass size_class) {
    const bool forgotten = places::forget_place(runtime_.preference_values_, overlay, size_class);
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

std::map<std::string, std::string> OaLayer::kept_overlay_places() const {
    std::map<std::string, std::string> kept;
    for (const auto& [key, value] : runtime_.preference_values_)
        if (key.starts_with(places::place_key_start))
            kept.emplace(key, value);
    return kept;
}

void OaLayer::restore_overlay_places(const std::map<std::string, std::string>& kept) {
    const bool forgotten = places::forget_places(runtime_.preference_values_);
    for (const auto& [key, value] : kept)
        runtime_.preference_values_[key] = value;
    if (forgotten || !kept.empty())
        write_window_preferences();
    ++revision_;
    lay_out_screens();
}

namespace {

/// Arrange overlays as a screen of the OA layer: modal over Settings, a
/// picture of the match as large as the window, its points the places'
/// points (OaLayer::arranging_room). The overlays' area, the handles and the
/// card are laid out in points from the area's top left corner; the screen
/// moves them to the area's place when it draws and lists them.
class ArrangeScreen final : public LayerScreen {
  public:

    /// Makes the screen.
    ///
    /// @param layer the layer it shows on, which keeps the places
    /// @param kept every overlay's place as the screen opened, which Cancel
    ///     puts back
    ArrangeScreen(OaLayer& layer, std::map<std::string, std::string> kept)
        : layer_(layer), kept_(std::move(kept)) {}

    /// Returns "arrange-overlays".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return kScreenName; }

    /// Returns the whole window at the places' scale: the match's canvas in
    /// a match, the room on the front end.
    ///
    /// @param view what the screen is placed on
    /// @return its place; empty before it is laid out
    [[nodiscard]] LayerPlacement placement(const LayerView& view) const override {
        if (!laid_)
            return {};
        int32_t percent = room_.scale_percent;
        layout::Rect into = room_.window;
        if (!view.match && view.room.width > 0 && room_.window.width > 0) {
            // The front end's window pixels for the match canvas's.
            percent = static_cast<int32_t>(
                (int64_t{room_.scale_percent} * view.room.width + room_.window.width / 2) /
                room_.window.width
            );
            into = view.room;
        }
        percent = std::max(percent, int32_t{1});
        const kit::Rect drawn = kit::to_canvas(whole_, percent);
        return {{into.x, into.y, drawn.width, drawn.height}, whole_.width, whole_.height, percent};
    }

    /// Tells that the screen takes every input.
    ///
    /// @return true
    [[nodiscard]] bool modal() const override { return true; }

    /// Tells that the layer darkens nothing under it: the screen covers it.
    ///
    /// @return false
    [[nodiscard]] bool backdrop() const override { return false; }

    /// Tells that the screen covers what lies under it.
    ///
    /// @return true
    [[nodiscard]] bool opaque() const override { return true; }

    /// Tells that what the screen draws changes only with its revision.
    ///
    /// @return true
    [[nodiscard]] bool revision_covers_drawing() const override { return true; }

    /// Lays the picture, the samples, the handles and the card out for the
    /// window now.
    ///
    /// @param view what the screen is placed on
    void lay_out(const LayerView& /*view*/) override {
        const auto room = layer_.arranging_room();
        const kit::Fonts* fonts = layer_.screen_fonts();
        if (!room || fonts == nullptr) {
            laid_ = false;
            return;
        }
        room_ = *room;
        const int32_t percent = room_.scale_percent;
        whole_ = {
            0, 0, points_up(room_.window.width, percent), points_up(room_.window.height, percent)
        };
        const kit::Rect area = room_.points();
        field_ = {
            points_down(room_.area.x, percent),
            points_down(room_.area.y, percent),
            area.width,
            area.height
        };
        overlays_ = layer_.arranged_overlays_in(room_, true);
        keys_ = layer_.keys_overlay_sample(room_);
        chat_words_ = shown(kChatSampleWords);
        chip_words_ = sample_chip_words();
        place_handles(*fonts);
        laid_ = true;
        ++revision_;
    }

    /// Draws the picture of the match, the overlays' samples where they
    /// show, a handle on each and the card.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override {
        if (!laid_ || !kit::can_draw(canvas))
            return;
        draw_picture(canvas);
        const kit::Canvas area = kit::placed_at(kit::clipped(canvas, field_), {field_.x, field_.y});
        draw_samples(area);
        // A handle on each overlay, the one held over the rest.
        for (std::size_t index = 0; index < handles_.size(); ++index)
            if (!drag_ || drag_->index != index)
                draw_handle(area, index);
        if (drag_)
            draw_handle(area, drag_->index);
        draw_card(area);
    }

    /// Takes a pointer: a press on a handle drags it, a double-click puts it
    /// back where it lays itself out, and Reset, Cancel and Done act on their
    /// release.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button, 1 for the left
    /// @param point the pointer, in the screen's points
    /// @param finger_reach how far a finger may lie from a control; 0 for a mouse
    /// @param clicks how many presses landed as one
    /// @return what the screen did
    LayerAnswer pointer_with_clicks(
        ScreenInputKind kind, uint8_t button, kit::Point point, int32_t finger_reach, uint8_t clicks
    ) override {
        if (!laid_)
            return LayerAnswer::none;
        // In points from the overlays' area's top left corner.
        const kit::Point at{point.x - field_.x, point.y - field_.y};
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
            std::optional<std::size_t> index = handle_of(pressed);
            if (!index && pressed == kit::no_control)
                index = tab_at(at);
            if (index) {
                const kit::ControlId held = kFirstHandle + static_cast<kit::ControlId>(*index);
                interaction_.focused = held;
                if (clicks >= 2) {
                    // A double-click puts the overlay back where it lays
                    // itself out.
                    drag_.reset();
                    layer_.forget_overlay_place(handles_[*index].name, room_.size_class);
                    relay_out();
                    return changed();
                }
                Drag drag;
                drag.index = *index;
                drag.grab = at;
                drag.start = handles_[*index].shown;
                drag.place = places::place_at(room_.points(), handles_[*index].shown);
                drag_ = drag;
                interaction_.pressed = held;
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
                layer_.place_overlay(handles_[ended.index].name, room_.size_class, ended.place);
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

    /// Takes a key: Escape puts the places back and closes the screen, an
    /// arrow moves the focused handle a grid step (Shift a larger one), Tab
    /// and Shift+Tab move the focus, and Enter and Space press the focused
    /// button.
    ///
    /// @param pressed the key
    /// @return what the screen did
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        if (pressed == kit::Key::escape)
            return act(kCancelId);
        if (!laid_)
            return LayerAnswer::none;
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
                    room_.size_class,
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
            return act(kDoneId);
        case kit::KeyResult::cancel:
            return act(kCancelId);
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

    /// Returns the handles, Reset, Cancel and Done, as automation lists
    /// them, in the screen's points.
    ///
    /// @return the list
    [[nodiscard]] kit::DisplayList display_list() const override {
        kit::DisplayList moved = list_;
        for (kit::Control& control : moved.controls) {
            control.rect.x += field_.x;
            control.rect.y += field_.y;
        }
        return moved;
    }

    /// Returns the hover, the press and the focus.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override { return interaction_; }

    /// Closes the screen once no match can be pictured: the match it shows
    /// over has ended.
    ///
    /// @return close then; otherwise none
    LayerAnswer tick() override {
        return layer_.arranging_room() ? LayerAnswer::none : LayerAnswer::close;
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
        kit::Rect shown{};       ///< the handle, in the area's points
        kit::Rect overlay{};     ///< where the overlay itself shows, in the area's points
        /// The tab with the grip and the words, on the handle's top edge
        /// outside it, or under it where the area's top leaves no room, so
        /// that it covers none of the sample; a press on it takes the handle.
        kit::Rect tab{};
    };

    /// A handle being dragged.
    struct Drag {
        std::size_t index{};          ///< the handle
        kit::Point grab{};            ///< where the press landed, in the area's points
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

    /// Returns the handle whose tab lies under a point, the one drawn last
    /// first.
    ///
    /// @param at the point, in the area's points
    /// @return the handle's index; nothing off every tab
    [[nodiscard]] std::optional<std::size_t> tab_at(kit::Point at) const noexcept {
        for (std::size_t index = handles_.size(); index > 0; --index)
            if (kit::contains(handles_[index - 1].tab, at))
                return index - 1;
        return std::nullopt;
    }

    /// Carries out a button's press.
    ///
    /// @param control the button
    /// @return close for Done and Cancel; redraw for Reset; otherwise none
    LayerAnswer act(kit::ControlId control) {
        if (control == kDoneId)
            return LayerAnswer::close;
        if (control == kCancelId) {
            drag_.reset();
            layer_.restore_overlay_places(kept_);
            return LayerAnswer::close;
        }
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
            std::optional<places::OverlayPlace> place =
                layer_.overlay_place(overlay.name, room_.size_class);
            if (drag_ && drag_->index == index)
                place = drag_->place;
            handle.shown = place ? places::rect_at(whole, size, *place)
                                 : places::handle_rect(whole, unplaced, least);
            handle.overlay = place
                                 ? places::rect_at(whole, {unplaced.width, unplaced.height}, *place)
                                 : places::kept_in(whole, unplaced);
            const int32_t tab_height = metrics.small_line + metrics.space;
            const int32_t tab_width = std::min(least.x, std::max(whole.width, int32_t{1}));
            int32_t tab_y = handle.shown.y - tab_height;
            if (tab_y < 0)
                tab_y = handle.shown.y + handle.shown.height;
            handle.tab = {
                std::clamp(
                    handle.shown.x, int32_t{0}, std::max(whole.width - tab_width, int32_t{0})
                ),
                std::clamp(tab_y, int32_t{0}, std::max(whole.height - tab_height, int32_t{0})),
                tab_width,
                tab_height
            };
            handles_.push_back(std::move(handle));
        }

        // The card: the title, the hint wrapped to the card, Reset, Cancel and Done.
        title_ = shown(kTitleWords);
        reset_words_ = shown(kResetWords);
        cancel_words_ = shown(kCancelWords);
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
        const int32_t cancel_width = button_width(cancel_words_);
        const int32_t done_width = button_width(done_words_);
        // Narrow, so that it finds room beside the handles: its title, its
        // buttons, and the hint wrapped to them.
        const int32_t widest = std::max(
            {kit::text_width(fonts, kit::FontRole::regular, title_),
             reset_width + gap + cancel_width + gap + done_width,
             kCardLeastPoints}
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
        for (const Handle& placed : handles_) {
            handle_rects.push_back(placed.shown);
            handle_rects.push_back(placed.tab);
        }
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
        cancel_box_ = {
            done_box_.x - gap - cancel_width, cursor, cancel_width, metrics.button_height
        };
        reset_box_ = {card_.x + pad, cursor, reset_width, metrics.button_height};

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
        add_button(kCancelId, cancel_box_, "cancel", cancel_words_);
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
        list_.tab_order.push_back(kCancelId);
        list_.tab_order.push_back(kDoneId);
    }

    /// Draws the picture of the match in the kit's look: the side panel
    /// down the window's left, the top bar and the bottom bar beside it,
    /// each named, and the battlefield between them with the grid.
    ///
    /// @param canvas where it draws, in the screen's points
    void draw_picture(const kit::Canvas& canvas) const {
        kit::fill_rect(canvas, whole_, kit::colour::band);
        const int32_t right = field_.x + field_.width;
        const int32_t bottom = field_.y + field_.height;
        const kit::Rect side{0, 0, field_.x, whole_.height};
        const kit::Rect top{field_.x, 0, whole_.width - field_.x, field_.y};
        const kit::Rect foot{field_.x, bottom, whole_.width - field_.x, whole_.height - bottom};
        const kit::Rect beyond{right, field_.y, whole_.width - right, field_.height};
        kit::fill_rect(canvas, side, kit::colour::panel);
        kit::fill_rect(canvas, top, kit::colour::list);
        kit::fill_rect(canvas, foot, kit::colour::list);
        kit::fill_rect(canvas, beyond, kit::colour::list);
        kit::fill_rect(canvas, field_, kit::colour::well);
        for (int32_t x = kGridLinePoints; x < field_.width; x += kGridLinePoints)
            kit::blend_rect(
                canvas, {field_.x + x, field_.y, 1, field_.height}, kit::colour::text, kGridOpacity
            );
        for (int32_t y = kGridLinePoints; y < field_.height; y += kGridLinePoints)
            kit::blend_rect(
                canvas, {field_.x, field_.y + y, field_.width, 1}, kit::colour::text, kGridOpacity
            );
        // The rules between the parts.
        kit::fill_rect(canvas, {field_.x - 1, 0, 1, whole_.height}, kit::colour::strong_edge);
        kit::fill_rect(canvas, {field_.x, field_.y - 1, field_.width, 1}, kit::colour::rule);
        kit::fill_rect(canvas, {field_.x, bottom, field_.width, 1}, kit::colour::rule);
        // Each part's name, where it has the room.
        const kit::Metrics& metrics = kit::metrics_of(room_.size_class);
        const auto name = [&](const kit::Rect& part, std::string_view words, bool at_top) {
            if (part.width <= 2 * metrics.space || part.height < metrics.small_line)
                return;
            const int32_t y =
                at_top ? part.y + metrics.space : part.y + (part.height - metrics.small_line) / 2;
            kit::box_text(
                kit::clipped(canvas, part),
                kit::FontRole::small,
                shown(words),
                {part.x + metrics.space, y, part.width - 2 * metrics.space, metrics.small_line},
                kit::Align::centre,
                kit::colour::text_tertiary
            );
        };
        name(side, kSidePanelWords, true);
        name(top, kTopBarWords, false);
        name(foot, kBottomBarWords, false);
    }

    /// Draws each overlay's sample where it shows: the keys overlay's short
    /// list, the chat line being typed and the latch chip.
    ///
    /// @param area where it draws, in the area's points
    void draw_samples(const kit::Canvas& area) const {
        const kit::Metrics& metrics = kit::metrics_of(room_.size_class);
        for (const Handle& handle : handles_) {
            const kit::Rect& at = handle.overlay;
            if (handle.name == places::keys_overlay) {
                oa::ui::key_editor::OverlayLayout drawn = keys_;
                drawn.panel.x = 0;
                drawn.panel.y = 0;
                oa::ui::key_editor::draw_overlay(
                    kit::placed_at(kit::clipped(area, at), {at.x, at.y}), drawn
                );
            } else if (handle.name == places::chat_line) {
                kit::blend_rect(area, at, kit::colour::band, kChatBandOpacity);
                kit::box_text(
                    kit::clipped(area, at),
                    kit::FontRole::small,
                    chat_words_,
                    {at.x + metrics.space,
                     at.y + (at.height - metrics.small_line) / 2,
                     std::max(at.width - 2 * metrics.space, int32_t{0}),
                     metrics.small_line},
                    kit::Align::left,
                    kit::colour::text
                );
            } else if (handle.name == places::latch_chips) {
                kit::ChipLook look;
                look.text = chip_words_;
                look.state = kit::ChipState::playing;
                const int32_t height = kit::compact_metrics.chip_height;
                kit::draw_chip(
                    area, {at.x, at.y + (at.height - height) / 2, at.width, height}, look
                );
            }
        }
    }

    /// Draws the card: its title, its hint, Reset, Cancel and Done.
    ///
    /// @param area where it draws, in the area's points
    void draw_card(const kit::Canvas& area) const {
        const kit::Metrics& metrics = kit::metrics_of(room_.size_class);
        kit::fill_corner_rect(area, card_, metrics.card_radius, kit::colour::panel);
        kit::outline_corner_rect(area, card_, metrics.card_radius, kit::colour::strong_edge);
        kit::box_text(
            area, kit::FontRole::regular, title_, title_box_, kit::Align::left, kit::colour::text
        );
        for (std::size_t line = 0; line < hint_lines_.size() && line < hint_boxes_.size(); ++line)
            kit::box_text(
                area,
                kit::FontRole::small,
                hint_lines_[line],
                hint_boxes_[line],
                kit::Align::left,
                kit::colour::text_secondary
            );
        draw_card_button(area, kResetId, reset_box_, reset_words_, kit::ButtonStyle::plain);
        draw_card_button(area, kCancelId, cancel_box_, cancel_words_, kit::ButtonStyle::plain);
        draw_card_button(area, kDoneId, done_box_, done_words_, kit::ButtonStyle::accent);
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
        // The tab: the accent's face with the grip and the words, outside
        // the handle, so the sample shows whole.
        const kit::Rect& tab = handle.tab;
        kit::fill_corner_rect(canvas, tab, kGripRadius, kit::colour::accent);
        const int32_t gap = metrics.space;
        const int32_t left = tab.x + gap;
        const int32_t middle = tab.y + tab.height / 2;
        kit::fill_corner_rect(
            canvas,
            {left, middle - kGripPoints / 2, kGripPoints, kGripPoints},
            kGripRadius,
            kit::colour::on_accent
        );
        const int32_t words_left = left + kGripPoints + gap;
        kit::box_text(
            kit::clipped(canvas, tab),
            kit::FontRole::small,
            handle.words,
            {words_left,
             middle - metrics.small_line / 2,
             std::max(tab.x + tab.width - words_left - gap, int32_t{0}),
             metrics.small_line},
            kit::Align::left,
            kit::colour::on_accent
        );
        if (interaction_.focus_shown && interaction_.focused == id)
            kit::draw_focus_ring(canvas, box, false);
    }

    OaLayer& layer_;                            ///< the layer it shows on
    std::map<std::string, std::string> kept_{}; ///< the places as the screen opened
    OaLayer::OverlayRoom room_{};               ///< the room a match takes in the window
    kit::Rect whole_{};                         ///< the window, in the screen's points
    kit::Rect field_{};                         ///< the overlays' area, in the screen's points
    std::vector<OaLayer::ArrangedOverlay>
        overlays_{};                           ///< the samples where they lay themselves out
    oa::ui::key_editor::OverlayLayout keys_{}; ///< the keys overlay's sample
    std::string chat_words_{};                 ///< the chat line's sample
    std::string chip_words_{};                 ///< the latch chip's sample
    std::vector<Handle> handles_{};            ///< a handle on each overlay
    std::optional<Drag> drag_{};               ///< the handle being dragged; none between drags
    kit::Rect card_{};                         ///< the card
    std::string title_{};                      ///< the card's title, in the language shown
    kit::Rect title_box_{};                    ///< the title's box
    std::vector<std::string> hint_lines_{};    ///< the hint's lines
    std::vector<kit::Rect> hint_boxes_{};      ///< each hint line's box
    std::string reset_words_{};                ///< Reset's caption
    kit::Rect reset_box_{};                    ///< Reset
    std::string cancel_words_{};               ///< Cancel's caption
    kit::Rect cancel_box_{};                   ///< Cancel
    std::string done_words_{};                 ///< Done's caption
    kit::Rect done_box_{};                     ///< Done
    kit::DisplayList list_{};                  ///< the controls, in the area's points
    kit::Interaction interaction_{};           ///< the hover, the press and the focus
    uint64_t revision_{1};                     ///< changes with what the screen draws
    bool laid_{};                              ///< laid out for a room
};

} // namespace

bool OaLayer::open_arrange_overlays() {
    if (find(kScreenName) != nullptr || screen_fonts() == nullptr || !arranging_room())
        return false;
    push(std::make_unique<ArrangeScreen>(*this, kept_overlay_places()));
    lay_out_screens();
    return true;
}

} // namespace oa::app
