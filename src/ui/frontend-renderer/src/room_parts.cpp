// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/frontend_renderer/room_parts.hpp"

#include "oa/present/game_text.hpp"
#include "oa/ui/frontend_renderer/gadget_draw.hpp"
#include "oa/ui/frontend_renderer/game_text.hpp"
#include "oa/ui/frontend_renderer/scroll_bars.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace oa::ui::frontend_renderer {
namespace {

// Entries of the GUI palette the game's plain parts are drawn in, mapped
// into the room's palette as the room maps them.
constexpr uint8_t gui_black = 0;
constexpr uint8_t gui_light = 17; // the raised bevel's top and left edges
constexpr uint8_t gui_disabled_fill = 19;
constexpr uint8_t gui_button_fill = 20;
// A caption shows no wider than its room less this many pixels.
constexpr int32_t caption_margin = 6;
// The light-table row the room lights a list's selected row through.
constexpr int32_t selection_level = 0x1E;
// The shade-table row a grayed button is darkened through.
constexpr std::size_t grayed_shade_row = 0x20 - 0x14;
constexpr std::size_t table_rows = 32;
// Pixels a button's plate keeps whole at each end when it is drawn wider or
// narrower than its art.
constexpr int32_t plate_end = 6;
constexpr std::string_view buttons_sequence = "BUTTONS0";
constexpr std::string_view switch_sequence = "stagebuttn1";
constexpr std::string_view checkbox_sequence = "CHECKBOX";
constexpr std::string_view cut_mark = "...";
// The colours the stamp and a lamp are drawn in, each taken as the
// palette's nearest entry so that they keep to the room's palette.
constexpr Rgb stamp_plate{30, 32, 28};
constexpr Rgb stamp_gold{214, 167, 71};
constexpr Rgb stamp_cut{92, 70, 30};
constexpr Rgb lamp_unlit_top{44, 44, 36};
constexpr Rgb lamp_unlit_foot{16, 16, 12};
constexpr Rgb lamp_green_top{166, 255, 150};
constexpr Rgb lamp_green_foot{31, 159, 31};
constexpr Rgb lamp_red_top{255, 171, 156};
constexpr Rgb lamp_red_foot{179, 38, 26};
constexpr Rgb notch_ink{239, 231, 168};
// How far, on average over a lamp's pixels, one lamp's glow must stand
// above the other's for room_lamp_seen to read it as lit.
constexpr int32_t lamp_seen_margin = 24;
/// How bright a notch triangle's slanted sides are, in percent of its face.
constexpr int32_t notch_shade_percent = 45;

[[nodiscard]] Rgb entry_rgb(const PaletteBytes& palette, uint8_t index) noexcept {
    const std::size_t at = static_cast<std::size_t>(index) * palette_entry_bytes;
    return {palette[at], palette[at + 1], palette[at + 2]};
}

/// The palette entry nearest a colour: the smallest summed channel
/// difference, the first entry winning a tie.
[[nodiscard]] uint8_t nearest_entry(const PaletteBytes& palette, Rgb colour) noexcept {
    uint8_t best = 0;
    int32_t best_distance = INT32_MAX;
    for (std::size_t index = 0; index < palette_color_count && best_distance != 0; ++index) {
        const Rgb entry = entry_rgb(palette, static_cast<uint8_t>(index));
        int32_t distance = 0;
        for (std::size_t channel = 0; channel < 3; ++channel)
            distance += std::abs(static_cast<int32_t>(entry[channel]) - colour[channel]);
        if (distance < best_distance) {
            best_distance = distance;
            best = static_cast<uint8_t>(index);
        }
    }
    return best;
}

/// The room's colour of a GUI palette entry: the room's nearest entry to it.
[[nodiscard]] Rgb gui_rgb(const RoomArt& art, uint8_t gui_index) noexcept {
    if (art.resources == nullptr)
        return entry_rgb(art.palette, gui_index);
    return entry_rgb(
        art.palette, nearest_entry(art.palette, entry_rgb(art.resources->gui_palette, gui_index))
    );
}

/// A colour as the room's palette shows it.
[[nodiscard]] Rgb palette_colour(const RoomArt& art, Rgb wanted) noexcept {
    return entry_rgb(art.palette, nearest_entry(art.palette, wanted));
}

[[nodiscard]] bool holds(const RoomCanvas& canvas, int32_t x, int32_t y) noexcept {
    return x >= canvas.area.x && y >= canvas.area.y && x < canvas.area.x + canvas.area.width &&
           y < canvas.area.y + canvas.area.height &&
           canvas.surface.rgb.size() >= static_cast<std::size_t>(canvas.area.width) *
                                            static_cast<std::size_t>(canvas.area.height) * 3U &&
           canvas.covered.size() >= static_cast<std::size_t>(canvas.area.width) *
                                        static_cast<std::size_t>(canvas.area.height);
}

[[nodiscard]] std::size_t pixel_at(const RoomCanvas& canvas, int32_t x, int32_t y) noexcept {
    return static_cast<std::size_t>(y - canvas.area.y) *
               static_cast<std::size_t>(canvas.area.width) +
           static_cast<std::size_t>(x - canvas.area.x);
}

void put(RoomCanvas& canvas, int32_t x, int32_t y, Rgb colour) noexcept {
    if (!holds(canvas, x, y))
        return;
    const std::size_t at = pixel_at(canvas, x, y);
    canvas.surface.rgb[at * 3] = colour[0];
    canvas.surface.rgb[at * 3 + 1] = colour[1];
    canvas.surface.rgb[at * 3 + 2] = colour[2];
    canvas.covered[at] = 1;
}

[[nodiscard]] Rgb get(const RoomCanvas& canvas, int32_t x, int32_t y) noexcept {
    if (!holds(canvas, x, y))
        return {0, 0, 0};
    const std::size_t at = pixel_at(canvas, x, y) * 3;
    return {canvas.surface.rgb[at], canvas.surface.rgb[at + 1], canvas.surface.rgb[at + 2]};
}

void fill(RoomCanvas& canvas, SourceRect rect, Rgb colour) noexcept {
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y)
        for (int32_t x = rect.x; x < rect.x + rect.width; ++x)
            put(canvas, x, y, colour);
}

void row_line(RoomCanvas& canvas, int32_t left, int32_t right, int32_t y, Rgb colour) noexcept {
    for (int32_t x = left; x <= right; ++x)
        put(canvas, x, y, colour);
}

void column_line(RoomCanvas& canvas, int32_t x, int32_t top, int32_t bottom, Rgb colour) noexcept {
    for (int32_t y = top; y <= bottom; ++y)
        put(canvas, x, y, colour);
}

/// The game's two-pixel bevel round the inside of a rectangle.
void bevel(RoomCanvas& canvas, SourceRect rect, Rgb top_left, Rgb bottom_right) noexcept {
    const int32_t left = rect.x;
    const int32_t top = rect.y;
    const int32_t right = rect.x + rect.width - 1;
    const int32_t bottom = rect.y + rect.height - 1;
    row_line(canvas, left, right, top, top_left);
    row_line(canvas, left, right - 1, top + 1, top_left);
    column_line(canvas, left, top, bottom, top_left);
    column_line(canvas, left + 1, top, bottom - 1, top_left);
    column_line(canvas, right, top + 1, bottom, bottom_right);
    column_line(canvas, right - 1, top + 2, bottom, bottom_right);
    row_line(canvas, left + 1, right, bottom, bottom_right);
    row_line(canvas, left + 2, right, bottom - 1, bottom_right);
}

/// A one-pixel frame round the inside of a rectangle.
void frame_line(RoomCanvas& canvas, SourceRect rect, Rgb top_left, Rgb bottom_right) noexcept {
    const int32_t right = rect.x + rect.width - 1;
    const int32_t bottom = rect.y + rect.height - 1;
    row_line(canvas, rect.x, right, rect.y, top_left);
    column_line(canvas, rect.x, rect.y, bottom, top_left);
    row_line(canvas, rect.x + 1, right, bottom, bottom_right);
    column_line(canvas, right, rect.y + 1, bottom, bottom_right);
}

/// A GAF frame's palette indices and coverage.
struct FramePixels {
    int32_t width{};
    int32_t height{};
    std::span<const uint8_t> pixels{};
    std::span<const uint8_t> coverage{};
};

/// Gives a frame's pixels as the room draws them, drawing a frame with
/// layers into `drawn`. None when the frame cannot be drawn.
[[nodiscard]] std::optional<FramePixels>
frame_pixels(const formats::gaf::Frame& frame, formats::gaf::RenderResult& drawn) {
    const std::size_t count = static_cast<std::size_t>(frame.width) * frame.height;
    if (frame.layers.empty() && frame.layer_count == 0 && count != 0 &&
        frame.pixels.size() == count && frame.coverage.size() == count)
        return FramePixels{frame.width, frame.height, frame.pixels, frame.coverage};
    drawn = formats::gaf::render_normal(frame);
    if (!drawn.ok())
        return std::nullopt;
    const formats::gaf::RenderedFrame& made = *drawn.frame;
    const std::size_t made_count = static_cast<std::size_t>(made.width) * made.height;
    if (made_count == 0 || made.pixels.size() != made_count || made.coverage.size() != made_count)
        return std::nullopt;
    return FramePixels{made.width, made.height, made.pixels, made.coverage};
}

[[nodiscard]] bool same_name(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t at = 0; at < left.size(); ++at) {
        const auto lower = [](char value) {
            return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
        };
        if (lower(left[at]) != lower(right[at]))
            return false;
    }
    return true;
}

/// The room's sequence of a name: its own archive's first, then the shared one's.
[[nodiscard]] const formats::gaf::Sequence*
find_sequence(const RoomArt& art, std::string_view name) noexcept {
    if (art.resources == nullptr)
        return nullptr;
    for (const formats::gaf::Archive* archive :
         {&art.resources->sprites, &art.resources->shared_sprites}) {
        for (const formats::gaf::Sequence& sequence : archive->sequences)
            if (same_name(sequence.name, name) && !sequence.frames.empty())
                return &sequence;
    }
    return nullptr;
}

/// Draws a frame's covered pixels at its own size, through a light row when one is given.
void blit(
    RoomCanvas& canvas,
    const PaletteBytes& palette,
    const FramePixels& frame,
    int32_t x,
    int32_t y,
    const uint8_t* light_row = nullptr
) {
    for (int32_t row = 0; row < frame.height; ++row)
        for (int32_t column = 0; column < frame.width; ++column) {
            const std::size_t at = static_cast<std::size_t>(row) * frame.width + column;
            if (frame.coverage[at] == 0)
                continue;
            const uint8_t index =
                light_row != nullptr ? light_row[frame.pixels[at]] : frame.pixels[at];
            put(canvas, x + column, y + row, entry_rgb(palette, index));
        }
}

/// The source column or row a destination one shows when a frame is drawn
/// larger or smaller: its first `head` and last `tail` kept, the middle
/// repeated or cut.
[[nodiscard]] int32_t
sliced_source(int32_t at, int32_t destination, int32_t source, int32_t head, int32_t tail) {
    if (destination == source)
        return at;
    head = std::min(head, source / 2);
    tail = std::min(tail, source - head);
    if (at < head)
        return at;
    if (at >= destination - tail)
        return source - (destination - at);
    const int32_t middle = std::max(source - head - tail, int32_t{1});
    return head + (at - head) % middle;
}

/// Draws a frame stretched to a rectangle by keeping its ends and repeating its middle.
void blit_sliced(
    RoomCanvas& canvas,
    const PaletteBytes& palette,
    const FramePixels& frame,
    SourceRect dest,
    int32_t left,
    int32_t right,
    int32_t top,
    int32_t bottom
) {
    if (frame.width <= 0 || frame.height <= 0)
        return;
    for (int32_t row = 0; row < dest.height; ++row) {
        const int32_t source_row = sliced_source(row, dest.height, frame.height, top, bottom);
        if (source_row < 0 || source_row >= frame.height)
            continue;
        for (int32_t column = 0; column < dest.width; ++column) {
            const int32_t source_column =
                sliced_source(column, dest.width, frame.width, left, right);
            if (source_column < 0 || source_column >= frame.width)
                continue;
            const std::size_t at = static_cast<std::size_t>(source_row) * frame.width +
                                   static_cast<std::size_t>(source_column);
            if (frame.coverage[at] == 0)
                continue;
            put(canvas, dest.x + column, dest.y + row, entry_rgb(palette, frame.pixels[at]));
        }
    }
}

/// The palette entries drawn colours stand for, each colour searched for
/// once: the entry that holds it, else the nearest.
class EntryLookup {
  public:

    /// Makes the lookup for a palette.
    ///
    /// @param palette the palette
    explicit EntryLookup(const PaletteBytes& palette) : palette_(palette) {}

    /// Returns the entry a colour stands for.
    ///
    /// @param colour the colour
    /// @return the entry
    [[nodiscard]] uint8_t operator()(Rgb colour) {
        const uint32_t key = static_cast<uint32_t>(colour[0]) << 16 |
                             static_cast<uint32_t>(colour[1]) << 8 | colour[2];
        for (const Found& found : found_)
            if (found.key == key)
                return found.entry;
        const uint8_t entry = nearest_entry(palette_, colour);
        found_.push_back({key, entry});
        return entry;
    }

  private:

    struct Found {
        uint32_t key{};
        uint8_t entry{};
    };

    const PaletteBytes& palette_;
    std::vector<Found> found_{};
};

/// Lights every pixel of a rectangle through a light-table row and covers it.
void light_rect(RoomCanvas& canvas, const RoomArt& art, SourceRect rect, int32_t level) {
    if (art.resources == nullptr || level <= 0 || level >= static_cast<int32_t>(table_rows) ||
        art.resources->light_table.size() != table_rows * palette_color_count)
        return;
    const uint8_t* row =
        art.resources->light_table.data() + static_cast<std::size_t>(level) * palette_color_count;
    EntryLookup entry_of(art.palette);
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y)
        for (int32_t x = rect.x; x < rect.x + rect.width; ++x) {
            if (!holds(canvas, x, y))
                continue;
            const uint8_t index = entry_of(get(canvas, x, y));
            put(canvas, x, y, entry_rgb(art.palette, row[index]));
        }
}

/// Darkens the covered pixels of a rectangle as the room darkens a grayed
/// button: through the palette's gray, then one row of the shade table.
void darken_rect(RoomCanvas& canvas, const RoomArt& art, SourceRect rect) {
    if (art.resources == nullptr ||
        art.resources->shade_table.size() != table_rows * palette_color_count)
        return;
    const std::vector<uint8_t> gray = build_gray_table(art.palette);
    EntryLookup entry_of(art.palette);
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y)
        for (int32_t x = rect.x; x < rect.x + rect.width; ++x) {
            if (!holds(canvas, x, y) || canvas.covered[pixel_at(canvas, x, y)] == 0)
                continue;
            const uint8_t index = entry_of(get(canvas, x, y));
            const std::size_t grey = gray.size() >= palette_color_count ? gray[index] : index;
            // Entries from 0x80 index the row as signed bytes, and so read the row before.
            const std::size_t row = grey >= 0x80 ? grayed_shade_row - 1 : grayed_shade_row;
            put(
                canvas,
                x,
                y,
                entry_rgb(art.palette, art.resources->shade_table[row * palette_color_count + grey])
            );
        }
}

[[nodiscard]] const formats::fnt::Font* font_of(const RoomArt& art, RoomFont font) noexcept {
    if (art.resources == nullptr)
        return nullptr;
    const formats::fnt::Font& chosen =
        font == RoomFont::label ? art.resources->label_font : art.resources->font;
    if (chosen.nominal_height == 0 && !chosen.glyphs['I'])
        return nullptr;
    return &chosen;
}

/// Cuts text, a whole character at a time, until it fits a width.
[[nodiscard]] std::string_view
cut_to(const formats::fnt::Font& font, std::string_view text, int32_t width) {
    const bool utf8 = present::game_text_settings().utf8;
    while (!text.empty() && measure_fnt_game_text(font, text, false) > width)
        text = text.substr(0, utf8 ? present::last_character_start(text) : text.size() - 1);
    return text;
}

/// The room's caption ink: the font's lightest letter colour.
[[nodiscard]] Rgb ink_of(const RoomArt& art, const formats::fnt::Font* font) {
    if (font == nullptr)
        return palette_colour(art, notch_ink);
    return entry_rgb(art.palette, fnt_font_ink(*font, art.palette));
}

/// A caption fitted to its box: the font it is drawn in and the bytes drawn.
struct FittedCaption {
    RoomFont font{RoomFont::caption}; ///< the font
    std::string text{};               ///< the bytes drawn
};

/// Fits a caption to a box as a room's button does.
FittedCaption fitted_caption(
    const RoomArt& art, std::string_view caption, int32_t box_width, bool narrower_font
) {
    FittedCaption fitted;
    const formats::fnt::Font* font = font_of(art, RoomFont::caption);
    if (font == nullptr || caption.empty() || box_width <= 0)
        return fitted;
    const int32_t limit = box_width - caption_margin;
    if (narrower_font && measure_fnt_game_text(*font, caption, false) > limit &&
        font_of(art, RoomFont::label) != nullptr)
        fitted.font = RoomFont::label;
    fitted.text = fit_room_text(art, fitted.font, caption, limit);
    return fitted;
}

/// Draws a caption centred in a box, as the room draws a button's.
void draw_caption(
    RoomCanvas& canvas,
    const RoomArt& art,
    std::string_view caption,
    SourceRect box,
    bool pressed,
    bool narrower_font
) {
    const FittedCaption fitted = fitted_caption(art, caption, box.width, narrower_font);
    const formats::fnt::Font* font = font_of(art, fitted.font);
    if (font == nullptr || fitted.text.empty())
        return;
    const std::string& shown = fitted.text;
    const int32_t limit = box.width - caption_margin;
    const int32_t most_size = fitted_fnt_text_size(*font, shown, limit, false);
    const int32_t width = measure_fnt_game_text(*font, shown, false, most_size);
    const int32_t height = formats::fnt::line_height(*font);
    const int32_t offset = pressed ? 1 : 0;
    const int32_t x = box.x + (box.width - 1 - width) / 2 + offset + 1;
    const int32_t y = box.y + (box.height - 1 - height) / 2 + offset;
    const TextClip clip{0, 0, canvas.area.width - 1, canvas.area.height - 1};
    const int32_t local_x = x - canvas.area.x;
    const int32_t local_y = y - canvas.area.y;
    if (needs_text_runs(shown, false)) {
        std::ignore = draw_fnt_game_text(
            canvas.surface,
            *font,
            shown,
            local_x,
            local_y,
            ink_of(art, font),
            art.palette,
            clip,
            false,
            most_size
        );
        return;
    }
    draw_fnt_glyphs(canvas.surface, *font, shown, local_x, local_y, art.palette, clip);
}

/// The frames of the on and off art, and where its lamps lie.
struct SwitchArt {
    const formats::gaf::Sequence* sequence{}; ///< stagebuttn1
    int32_t lamp_left{};                      ///< the lamps' first column
    int32_t lamp_right{};                     ///< their last column
    int32_t lamp_top{};                       ///< their first row
    int32_t lamp_bottom{};                    ///< their last row
};

// The frames of the on and off art: off lights the red lamp, on the green
// one, and the third lights neither.
constexpr std::size_t switch_red_frame = 0;
constexpr std::size_t switch_green_frame = 1;
constexpr std::size_t switch_unlit_frame = 2;

/// Finds the on and off art's lamps: the pixels its red and green frames do
/// not share.
[[nodiscard]] std::optional<SwitchArt> switch_art(const RoomArt& art) {
    const formats::gaf::Sequence* sequence = find_sequence(art, switch_sequence);
    if (sequence == nullptr || sequence->frames.size() <= switch_unlit_frame)
        return std::nullopt;
    formats::gaf::RenderResult red_drawn;
    formats::gaf::RenderResult green_drawn;
    const std::optional<FramePixels> red =
        frame_pixels(sequence->frames[switch_red_frame], red_drawn);
    const std::optional<FramePixels> green =
        frame_pixels(sequence->frames[switch_green_frame], green_drawn);
    if (!red || !green || red->width != green->width || red->height != green->height)
        return std::nullopt;
    SwitchArt found;
    found.sequence = sequence;
    found.lamp_left = red->width;
    found.lamp_top = red->height;
    found.lamp_right = -1;
    found.lamp_bottom = -1;
    for (int32_t row = 0; row < red->height; ++row)
        for (int32_t column = 0; column < red->width; ++column) {
            const std::size_t at = static_cast<std::size_t>(row) * red->width + column;
            if (red->pixels[at] == green->pixels[at] && red->coverage[at] == green->coverage[at])
                continue;
            found.lamp_left = std::min(found.lamp_left, column);
            found.lamp_right = std::max(found.lamp_right, column);
            found.lamp_top = std::min(found.lamp_top, row);
            found.lamp_bottom = std::max(found.lamp_bottom, row);
        }
    if (found.lamp_right < found.lamp_left)
        return std::nullopt;
    return found;
}

} // namespace

PaletteBytes room_palette(const ScreenResources& resources) noexcept {
    return resources.background.palette.has_value() ? *resources.background.palette
                                                    : resources.gui_palette;
}

RoomCanvas room_canvas(SourceRect area, const Surface* frame) {
    RoomCanvas canvas;
    if (area.width <= 0 || area.height <= 0)
        return canvas;
    canvas.area = area;
    canvas.surface.width = static_cast<uint32_t>(area.width);
    canvas.surface.height = static_cast<uint32_t>(area.height);
    const std::size_t count =
        static_cast<std::size_t>(area.width) * static_cast<std::size_t>(area.height);
    canvas.surface.rgb.assign(count * 3U, 0);
    canvas.covered.assign(count, 0);
    if (frame == nullptr ||
        frame->rgb.size() < static_cast<std::size_t>(frame->width) * frame->height * 3U)
        return canvas;
    for (int32_t row = 0; row < area.height; ++row) {
        const int32_t y = area.y + row;
        if (y < 0 || y >= static_cast<int32_t>(frame->height))
            continue;
        for (int32_t column = 0; column < area.width; ++column) {
            const int32_t x = area.x + column;
            if (x < 0 || x >= static_cast<int32_t>(frame->width))
                continue;
            const std::size_t from =
                (static_cast<std::size_t>(y) * frame->width + static_cast<std::size_t>(x)) * 3U;
            const std::size_t to =
                (static_cast<std::size_t>(row) * area.width + static_cast<std::size_t>(column)) *
                3U;
            canvas.surface.rgb[to] = frame->rgb[from];
            canvas.surface.rgb[to + 1] = frame->rgb[from + 1];
            canvas.surface.rgb[to + 2] = frame->rgb[from + 2];
        }
    }
    return canvas;
}

void room_canvas_rgba(const RoomCanvas& canvas, std::vector<uint8_t>& rgba) {
    const std::size_t count = static_cast<std::size_t>(std::max(canvas.area.width, 0)) *
                              static_cast<std::size_t>(std::max(canvas.area.height, 0));
    rgba.assign(count * 4U, 0);
    if (canvas.surface.rgb.size() < count * 3U || canvas.covered.size() < count)
        return;
    for (std::size_t at = 0; at < count; ++at) {
        if (canvas.covered[at] == 0)
            continue;
        rgba[at * 4] = canvas.surface.rgb[at * 3];
        rgba[at * 4 + 1] = canvas.surface.rgb[at * 3 + 1];
        rgba[at * 4 + 2] = canvas.surface.rgb[at * 3 + 2];
        rgba[at * 4 + 3] = 255;
    }
}

std::string room_text(std::string_view words) {
    return present::encode_game_text(words, present::game_text_settings().utf8);
}

int32_t room_line_height(const RoomArt& art, RoomFont font) noexcept {
    const formats::fnt::Font* chosen = font_of(art, font);
    return chosen != nullptr ? formats::fnt::line_height(*chosen) : 0;
}

int32_t room_text_width(const RoomArt& art, RoomFont font, std::string_view text) {
    const formats::fnt::Font* chosen = font_of(art, font);
    return chosen != nullptr ? measure_fnt_game_text(*chosen, text, false) : 0;
}

std::string fit_room_text(const RoomArt& art, RoomFont font, std::string_view text, int32_t width) {
    const formats::fnt::Font* chosen = font_of(art, font);
    if (chosen == nullptr || width <= 0)
        return {};
    if (measure_fnt_game_text(*chosen, text, false) <= width)
        return std::string(text);
    const int32_t mark = measure_fnt_game_text(*chosen, cut_mark, false);
    std::string_view kept = cut_to(*chosen, text, width - mark);
    while (!kept.empty() && kept.back() == ' ')
        kept.remove_suffix(1);
    if (kept.empty())
        return std::string(cut_to(*chosen, text, width));
    return std::string(kept) + std::string(cut_mark);
}

int32_t draw_room_text(
    RoomCanvas& canvas,
    const RoomArt& art,
    RoomFont font,
    std::string_view text,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t light_level
) {
    const formats::fnt::Font* chosen = font_of(art, font);
    if (chosen == nullptr || text.empty() || width <= 0 || canvas.area.width <= 0)
        return x;
    const int32_t height = formats::fnt::line_height(*chosen);
    // The rows a glyph may reach, kept to compare once the text is drawn.
    const int32_t top = std::max(y - height, canvas.area.y);
    const int32_t bottom = std::min(y + 2 * height, canvas.area.y + canvas.area.height);
    const int32_t left = std::max(x, canvas.area.x);
    const int32_t right = std::min(x + width, canvas.area.x + canvas.area.width);
    if (top >= bottom || left >= right)
        return x;
    std::vector<uint8_t> before;
    before.reserve(static_cast<std::size_t>(bottom - top) * (right - left) * 3U);
    for (int32_t row = top; row < bottom; ++row)
        for (int32_t column = left; column < right; ++column) {
            const Rgb was = get(canvas, column, row);
            before.insert(before.end(), was.begin(), was.end());
        }
    const TextClip clip{
        left - canvas.area.x,
        top - canvas.area.y,
        right - 1 - canvas.area.x,
        bottom - 1 - canvas.area.y
    };
    const uint8_t* light_row = nullptr;
    if (light_level > 0 && light_level < static_cast<int32_t>(table_rows) &&
        art.resources->light_table.size() == table_rows * palette_color_count)
        light_row = art.resources->light_table.data() +
                    static_cast<std::size_t>(light_level) * palette_color_count;
    int32_t end = x;
    const std::string_view shown = cut_to(*chosen, text, width);
    if (needs_text_runs(shown, false)) {
        Rgb ink = ink_of(art, chosen);
        if (light_row != nullptr)
            ink = entry_rgb(art.palette, light_row[nearest_entry(art.palette, ink)]);
        end = draw_fnt_game_text(
                  canvas.surface,
                  *chosen,
                  shown,
                  x - canvas.area.x,
                  y - canvas.area.y,
                  ink,
                  art.palette,
                  clip,
                  false
              ) +
              canvas.area.x;
    } else {
        draw_fnt_glyphs(
            canvas.surface,
            *chosen,
            shown,
            x - canvas.area.x,
            y - canvas.area.y,
            art.palette,
            clip,
            light_row
        );
        end = x + static_cast<int32_t>(formats::fnt::measure_text(*chosen, shown));
    }
    std::size_t at = 0;
    for (int32_t row = top; row < bottom; ++row)
        for (int32_t column = left; column < right; ++column, at += 3) {
            const Rgb now = get(canvas, column, row);
            if (now[0] != before[at] || now[1] != before[at + 1] || now[2] != before[at + 2])
                canvas.covered[pixel_at(canvas, column, row)] = 1;
        }
    return end;
}

void cover_room_area(
    RoomCanvas& canvas, SourceRect rect, const Image& picture, int32_t picture_x, int32_t picture_y
) {
    if (picture.rgb.size() < static_cast<std::size_t>(picture.width) * picture.height * 3U)
        return;
    for (int32_t y = rect.y; y < rect.y + rect.height; ++y)
        for (int32_t x = rect.x; x < rect.x + rect.width; ++x) {
            const int32_t column = x - picture_x;
            const int32_t row = y - picture_y;
            if (column < 0 || row < 0 || column >= static_cast<int32_t>(picture.width) ||
                row >= static_cast<int32_t>(picture.height))
                continue;
            const std::size_t at =
                (static_cast<std::size_t>(row) * picture.width + static_cast<std::size_t>(column)) *
                3U;
            put(canvas, x, y, {picture.rgb[at], picture.rgb[at + 1], picture.rgb[at + 2]});
        }
}

std::string room_caption_shown(
    const RoomArt& art, std::string_view caption, int32_t box_width, bool narrower_font
) {
    return fitted_caption(art, caption, box_width, narrower_font).text;
}

int32_t room_switch_caption_width(const RoomArt& art, const RoomButtonLook& look) {
    int32_t right_end = 0;
    if (const std::optional<SwitchArt> found = switch_art(art)) {
        const int32_t width =
            static_cast<int32_t>(found->sequence->frames[switch_green_frame].width);
        right_end = width - found->lamp_left + 2;
    } else {
        right_end = std::max(std::max(look.rect.height - 8, 4) / 3, 3) + 6;
    }
    return look.rect.width - look.inset_left - look.inset_right - right_end;
}

std::size_t
room_button_group(const formats::gaf::Sequence& sequence, int32_t width, int32_t height) noexcept {
    std::size_t best = 0;
    // The best difference starts at 1000, so that a declaration far from
    // every frame keeps the first group, as the room's own buttons do.
    int32_t best_difference = 1000;
    for (std::size_t index = 0; index < sequence.frames.size(); index += 4) {
        const formats::gaf::Frame& frame = sequence.frames[index];
        const int32_t difference = std::abs(width - static_cast<int32_t>(frame.width)) +
                                   std::abs(height - static_cast<int32_t>(frame.height));
        if (difference < best_difference) {
            best = index;
            best_difference = difference;
        }
    }
    return best;
}

void draw_room_button(RoomCanvas& canvas, const RoomArt& art, const RoomButtonLook& look) {
    const SourceRect rect = look.rect;
    if (rect.width <= 0 || rect.height <= 0)
        return;
    const bool pressed = look.condition == ButtonCondition::pressed;
    const bool disabled = look.condition == ButtonCondition::disabled;
    bool plate = false;
    if (look.art != nullptr && !look.art->frames.empty()) {
        // A button's own art keeps its own size at the button's corner.
        std::size_t index = 0;
        if (pressed)
            index = std::min<std::size_t>(1, look.art->frames.size() - 1);
        else if (disabled)
            index = std::min<std::size_t>(2, look.art->frames.size() - 1);
        formats::gaf::RenderResult drawn;
        if (const std::optional<FramePixels> pixels =
                frame_pixels(look.art->frames[index], drawn)) {
            blit(canvas, art.palette, *pixels, rect.x, rect.y);
            plate = true;
        }
    } else if (const formats::gaf::Sequence* buttons = find_sequence(art, buttons_sequence)) {
        const std::size_t group = room_button_group(*buttons, rect.width, rect.height);
        std::size_t index = group;
        if (pressed)
            index = std::min(group + 1, buttons->frames.size() - 1);
        else if (disabled)
            index = std::min(group + 2, buttons->frames.size() - 1);
        formats::gaf::RenderResult drawn;
        if (const std::optional<FramePixels> pixels = frame_pixels(buttons->frames[index], drawn)) {
            blit_sliced(
                canvas, art.palette, *pixels, rect, plate_end, plate_end, plate_end, plate_end
            );
            plate = true;
        }
    }
    if (!plate) {
        // The plain face: filled, then given its raised or sunken bevel.
        const bool sunken = pressed || disabled;
        fill(canvas, rect, gui_rgb(art, disabled ? gui_disabled_fill : gui_button_fill));
        bevel(
            canvas,
            rect,
            gui_rgb(art, sunken ? gui_black : gui_light),
            gui_rgb(art, disabled ? gui_disabled_fill : (sunken ? gui_light : gui_black))
        );
    }
    const SourceRect box{
        rect.x + look.inset_left,
        rect.y,
        rect.width - look.inset_left - look.inset_right,
        rect.height
    };
    draw_caption(canvas, art, look.caption, box, pressed, look.narrower_font);
    if (disabled)
        darken_rect(canvas, art, rect);
}

RoomLampParts room_lamp_parts(const RoomArt& art, SourceRect rect) {
    RoomLampParts parts;
    const std::optional<SwitchArt> found = switch_art(art);
    if (!found)
        return parts;
    const formats::gaf::Frame& frame = found->sequence->frames[switch_green_frame];
    const int32_t from_right = static_cast<int32_t>(frame.width) - found->lamp_left;
    const int32_t lamp_height = found->lamp_bottom - found->lamp_top + 1;
    parts.inset_right = from_right;
    parts.lamp = {
        rect.x + rect.width - from_right,
        rect.y + sliced_source(found->lamp_top, rect.height, frame.height, plate_end, plate_end),
        found->lamp_right - found->lamp_left + 1,
        lamp_height
    };
    if (rect.height != static_cast<int32_t>(frame.height)) {
        // A taller or shorter plate keeps the lamp centred where the art has it.
        const int32_t centre = found->lamp_top + lamp_height / 2;
        parts.lamp.y =
            rect.y + centre * rect.height / std::max<int32_t>(frame.height, 1) - lamp_height / 2;
    }
    return parts;
}

RoomLamp room_lamp_shown(RoomLamp lamp, uint64_t milliseconds) noexcept {
    if (lamp != RoomLamp::red_blinking)
        return lamp;
    return (milliseconds / room_lamp_blink_ms) % 2 == 0 ? RoomLamp::red : RoomLamp::unlit;
}

RoomLamp room_lamp_seen(std::span<const uint8_t> rgb, int32_t width, SourceRect lamp) noexcept {
    if (width <= 0 || lamp.width < 2 || lamp.height <= 0 || lamp.x < 0 || lamp.y < 0 ||
        lamp.x + lamp.width > width)
        return RoomLamp::unlit;
    const std::size_t stride = static_cast<std::size_t>(width);
    if (static_cast<std::size_t>(lamp.y) + static_cast<std::size_t>(lamp.height) >
        rgb.size() / (stride * 3U))
        return RoomLamp::unlit;
    // The red lamp is the left one and the green the right: each half adds
    // how far its own colour stands above the other two.
    const int32_t half = lamp.width / 2;
    int64_t red_glow = 0;
    int64_t green_glow = 0;
    for (int32_t row = lamp.y; row < lamp.y + lamp.height; ++row)
        for (int32_t step = 0; step < half; ++step) {
            const std::size_t line = static_cast<std::size_t>(row) * stride;
            const std::size_t left = (line + static_cast<std::size_t>(lamp.x + step)) * 3U;
            const std::size_t right =
                (line + static_cast<std::size_t>(lamp.x + lamp.width - 1 - step)) * 3U;
            red_glow += std::max(
                0, int32_t{rgb[left]} - std::max(int32_t{rgb[left + 1]}, int32_t{rgb[left + 2]})
            );
            green_glow += std::max(
                0, int32_t{rgb[right + 1]} - std::max(int32_t{rgb[right]}, int32_t{rgb[right + 2]})
            );
        }
    const int64_t margin = int64_t{lamp_seen_margin} * half * lamp.height;
    if (red_glow > green_glow + margin)
        return RoomLamp::red;
    if (green_glow > red_glow + margin)
        return RoomLamp::green;
    return RoomLamp::unlit;
}

void draw_room_switch(
    RoomCanvas& canvas, const RoomArt& art, const RoomButtonLook& look, RoomLamp lamp
) {
    const SourceRect rect = look.rect;
    if (rect.width <= 0 || rect.height <= 0)
        return;
    const std::optional<SwitchArt> found = switch_art(art);
    if (!found) {
        // Without the art the lamp sits in the plate's right end.
        const int32_t lamp_height = std::max(rect.height - 8, 4);
        const int32_t lamp_width = std::max(lamp_height / 3, 3);
        RoomButtonLook plain = look;
        plain.art = nullptr;
        plain.inset_right = look.inset_right + lamp_width + 6;
        draw_room_button(canvas, art, plain);
        draw_room_lamp(
            canvas,
            art,
            {rect.x + rect.width - 5 - lamp_width,
             rect.y + (rect.height - lamp_height) / 2,
             lamp_width,
             lamp_height},
            lamp
        );
        return;
    }
    const bool pressed = look.condition == ButtonCondition::pressed;
    const bool disabled = look.condition == ButtonCondition::disabled;
    std::size_t index = switch_unlit_frame;
    if (lamp == RoomLamp::green)
        index = switch_green_frame;
    else if (lamp == RoomLamp::red || lamp == RoomLamp::red_blinking)
        index = switch_red_frame;
    formats::gaf::RenderResult drawn;
    const std::optional<FramePixels> pixels = frame_pixels(found->sequence->frames[index], drawn);
    if (!pixels)
        return;
    const int32_t right_end = pixels->width - found->lamp_left + 2;
    blit_sliced(canvas, art.palette, *pixels, rect, plate_end, right_end, plate_end, plate_end);
    const SourceRect box{
        rect.x + look.inset_left,
        rect.y,
        rect.width - look.inset_left - look.inset_right - right_end,
        rect.height
    };
    draw_caption(canvas, art, look.caption, box, pressed, look.narrower_font);
    if (disabled)
        darken_rect(canvas, art, rect);
}

int32_t room_switch_width(const RoomArt& art, std::string_view caption, int32_t inset_left) {
    // The caption's own width, the margin a caption keeps and a pixel of
    // centring on each side.
    const int32_t words = room_text_width(art, RoomFont::caption, caption) + caption_margin + 2;
    if (const std::optional<SwitchArt> found = switch_art(art)) {
        const int32_t width =
            static_cast<int32_t>(found->sequence->frames[switch_green_frame].width);
        return inset_left + words + (width - found->lamp_left + 2);
    }
    return inset_left + words + 12;
}

void draw_room_lamp(RoomCanvas& canvas, const RoomArt& art, SourceRect rect, RoomLamp lamp) {
    if (rect.width <= 0 || rect.height <= 0)
        return;
    frame_line(canvas, rect, gui_rgb(art, gui_black), gui_rgb(art, gui_light));
    Rgb top = lamp_unlit_top;
    Rgb foot = lamp_unlit_foot;
    if (lamp == RoomLamp::green) {
        top = lamp_green_top;
        foot = lamp_green_foot;
    } else if (lamp == RoomLamp::red || lamp == RoomLamp::red_blinking) {
        top = lamp_red_top;
        foot = lamp_red_foot;
    }
    const int32_t inner = std::max(rect.height - 2, 1);
    for (int32_t row = 0; row < inner; ++row) {
        Rgb mixed{};
        for (std::size_t channel = 0; channel < 3; ++channel)
            mixed[channel] = static_cast<uint8_t>(
                top[channel] +
                (static_cast<int32_t>(foot[channel]) - top[channel]) * row / std::max(inner - 1, 1)
            );
        row_line(
            canvas,
            rect.x + 1,
            rect.x + rect.width - 2,
            rect.y + 1 + row,
            palette_colour(art, mixed)
        );
    }
}

void draw_room_bar(
    RoomCanvas& canvas, const RoomArt& art, SourceRect rect, int32_t done, int32_t whole
) {
    if (rect.width < 4 || rect.height < 4)
        return;
    fill(canvas, rect, gui_rgb(art, gui_black));
    frame_line(canvas, rect, gui_rgb(art, gui_black), gui_rgb(art, gui_light));
    if (whole <= 0 || done <= 0)
        return;
    const int32_t inner = rect.width - 2;
    const int32_t filled =
        static_cast<int32_t>(static_cast<int64_t>(inner) * std::min(done, whole) / whole);
    if (filled <= 0)
        return;
    const int32_t rows = rect.height - 2;
    for (int32_t row = 0; row < rows; ++row) {
        Rgb mixed{};
        for (std::size_t channel = 0; channel < 3; ++channel)
            mixed[channel] = static_cast<uint8_t>(
                lamp_green_top[channel] +
                (static_cast<int32_t>(lamp_green_foot[channel]) - lamp_green_top[channel]) * row /
                    std::max(rows - 1, 1)
            );
        row_line(canvas, rect.x + 1, rect.x + filled, rect.y + 1 + row, palette_colour(art, mixed));
    }
}

SourceRect room_notch_rect(SourceRect button, int32_t width) noexcept {
    const int32_t edge = 2;
    return {
        button.x + button.width - edge - width, button.y + edge, width, button.height - 2 * edge
    };
}

void draw_room_notch(
    RoomCanvas& canvas, const RoomArt& art, SourceRect notch, bool open, bool held
) {
    if (notch.width < 4 || notch.height < 4)
        return;
    const Rgb dark = gui_rgb(art, gui_black);
    const Rgb light = gui_rgb(art, gui_light);
    column_line(canvas, notch.x, notch.y, notch.y + notch.height - 1, held ? light : dark);
    column_line(canvas, notch.x + 1, notch.y, notch.y + notch.height - 1, held ? dark : light);
    int32_t base = std::min(notch.width - 5, 9);
    if (base % 2 == 0)
        --base;
    if (base < 3)
        return;
    const int32_t rows = (base + 1) / 2;
    const int32_t drop = held ? 1 : 0;
    const int32_t left = notch.x + 2 + (notch.width - 2 - base) / 2 + drop;
    const int32_t top = notch.y + (notch.height - rows) / 2 + drop;
    const Rgb ink = ink_of(art, font_of(art, RoomFont::caption));
    // The triangle in the caption's colour with no black edge, shaded as
    // the room's own arrows are: its slanted sides and its point a shade
    // darker than its face.
    Rgb shaded{};
    for (std::size_t channel = 0; channel < shaded.size(); ++channel)
        shaded[channel] = static_cast<uint8_t>(ink[channel] * notch_shade_percent / 100);
    const Rgb edge = palette_colour(art, shaded);
    const auto inside = [&](int32_t x, int32_t y) {
        const int32_t row = y - top;
        if (row < 0 || row >= rows)
            return false;
        const int32_t inset = open ? rows - 1 - row : row;
        return x >= left + inset && x <= left + base - 1 - inset;
    };
    // The base row is the face's; the other rows' ends are the slanted sides.
    const int32_t base_row = open ? rows - 1 : 0;
    for (int32_t y = top; y < top + rows; ++y)
        for (int32_t x = left; x < left + base; ++x) {
            if (!inside(x, y))
                continue;
            const bool side = y - top != base_row && (!inside(x - 1, y) || !inside(x + 1, y));
            put(canvas, x, y, side ? edge : ink);
        }
}

namespace {

/// One pixel remembered under a stamp, so a clear part of the icon does not
/// cover what it did not change.
struct StampPixel {
    int32_t x{};
    int32_t y{};
    Rgb colour{};
};

/// Draws the icon into the largest square of a stamp, centred. Each canvas
/// pixel is the mean of the icon pixels that land on it. A pixel the icon
/// leaves unchanged stays uncovered when it was.
void draw_stamp_icon(RoomCanvas& canvas, SourceRect rect, const RgbaPicture& icon) {
    const int32_t side = std::min(rect.width, rect.height);
    if (side <= 0)
        return;
    const SourceRect square{
        rect.x + (rect.width - side) / 2, rect.y + (rect.height - side) / 2, side, side
    };
    std::vector<StampPixel> under;
    under.reserve(static_cast<std::size_t>(side) * static_cast<std::size_t>(side));
    for (int32_t y = square.y; y < square.y + side; ++y)
        for (int32_t x = square.x; x < square.x + side; ++x)
            if (holds(canvas, x, y))
                under.push_back({x, y, get(canvas, x, y)});
    draw_picture(canvas.surface, Placement{-canvas.area.x, -canvas.area.y, 1}, square, icon);
    for (const StampPixel& pixel : under) {
        if (get(canvas, pixel.x, pixel.y) == pixel.colour)
            continue;
        canvas.covered[pixel_at(canvas, pixel.x, pixel.y)] = 1;
    }
}

} // namespace

void draw_room_stamp(
    RoomCanvas& canvas, const RoomArt& art, SourceRect rect, const RgbaPicture& icon
) {
    if (rect.width < 6 || rect.height < 6)
        return;
    if (picture_drawable(icon)) {
        draw_stamp_icon(canvas, rect, icon);
        return;
    }
    fill(canvas, rect, palette_colour(art, stamp_plate));
    frame_line(canvas, rect, gui_rgb(art, gui_black), gui_rgb(art, gui_light));
    const Mark& letters = rect.height >= 14 ? oa_mark_bold : oa_mark_thin;
    const int32_t x = rect.x + (rect.width - letters.width) / 2;
    const int32_t y = rect.y + (rect.height - letters.height) / 2;
    const Rgb cut = palette_colour(art, stamp_cut);
    const Rgb gold = palette_colour(art, stamp_gold);
    for (int32_t row = 0; row < letters.height; ++row)
        for (int32_t column = 0; column < letters.width; ++column)
            if (letters.bits[static_cast<std::size_t>(row) * letters.width + column] != 0)
                put(canvas, x + column + 1, y + row + 1, cut);
    for (int32_t row = 0; row < letters.height; ++row)
        for (int32_t column = 0; column < letters.width; ++column)
            if (letters.bits[static_cast<std::size_t>(row) * letters.width + column] != 0)
                put(canvas, x + column, y + row, gold);
}

SourceRect room_stamp_rect(SourceRect button) noexcept {
    // The width stays the thin letters' width. Captions start after it, and
    // a taller mark must not push them along.
    const int32_t height = std::clamp(button.height - 8, 9, 14);
    const int32_t width = oa_mark_thin.width + 6;
    return {button.x + 5, button.y + (button.height - height) / 2, width, height};
}

void draw_room_focus(RoomCanvas& canvas, const RoomArt& art, SourceRect record) {
    if (art.resources == nullptr ||
        art.resources->light_table.size() != table_rows * palette_color_count)
        return;
    const ui::gui_layout::GadgetRect inclusive{
        record.x, record.y, record.x + record.width - 1, record.y + record.height - 1
    };
    EntryLookup entry_of(art.palette);
    const auto light = [&](int32_t x, int32_t y, int32_t level) {
        if (!holds(canvas, x, y) || level < 0 || level >= static_cast<int32_t>(table_rows))
            return;
        const uint8_t index = entry_of(get(canvas, x, y));
        put(canvas,
            x,
            y,
            entry_rgb(
                art.palette,
                art.resources
                    ->light_table[static_cast<std::size_t>(level) * palette_color_count + index]
            ));
    };
    for (const FocusRing& ring : focus_rings(inclusive)) {
        const Rect32& r = ring.rect;
        for (int32_t x = r.x1; x <= r.x2; ++x)
            light(x, r.y1, ring.level);
        for (int32_t y = r.y1; y <= r.y2; ++y)
            light(r.x2, y, ring.level);
        for (int32_t x = r.x1; x <= r.x2; ++x)
            light(x, r.y2, ring.level);
        for (int32_t y = r.y1; y <= r.y2; ++y)
            light(r.x1, y, ring.level);
    }
}

int32_t room_list_row_height(const RoomArt& art) noexcept {
    return std::max(room_line_height(art, RoomFont::caption) + 1, int32_t{12});
}

std::vector<SourceRect> room_list_row_rects(const RoomArt& art, const RoomListLook& look) {
    std::vector<SourceRect> rects;
    rects.reserve(look.rows.size());
    const int32_t height = room_list_row_height(art);
    int32_t y = look.rect.y + room_list_border;
    for (const RoomListRow& row : look.rows) {
        if (row.rule_above)
            y += room_list_rule;
        rects.push_back(
            {look.rect.x + room_list_border, y, look.rect.width - 2 * room_list_border, height}
        );
        y += height;
    }
    return rects;
}

SourceRect
room_list_size(const RoomArt& art, std::span<const RoomListRow> rows, int32_t value_width) {
    int32_t widest = 0;
    int32_t height = 0;
    const int32_t row_height = room_list_row_height(art);
    for (const RoomListRow& row : rows) {
        const RoomFont font = row.heading ? RoomFont::label : RoomFont::caption;
        const int32_t words = room_text_width(art, font, row.text) + 6;
        widest = std::max(widest, row.heading ? words : room_list_indent + words + value_width + 8);
        height += row_height + (row.rule_above ? room_list_rule : 0);
    }
    return {0, 0, widest + 2 * room_list_border, height + 2 * room_list_border};
}

void draw_room_list(RoomCanvas& canvas, const RoomArt& art, const RoomListLook& look) {
    const SourceRect rect = look.rect;
    if (rect.width < 2 * room_list_border || rect.height < 2 * room_list_border)
        return;
    // The frame: the room's button face with its raised bevel, round a
    // dark well whose own edge is sunk.
    fill(canvas, rect, gui_rgb(art, gui_button_fill));
    bevel(canvas, rect, gui_rgb(art, gui_light), gui_rgb(art, gui_black));
    const SourceRect well{rect.x + 3, rect.y + 3, rect.width - 6, rect.height - 6};
    fill(canvas, well, gui_rgb(art, gui_black));
    frame_line(canvas, well, gui_rgb(art, gui_black), gui_rgb(art, gui_light));
    const std::vector<SourceRect> rects = room_list_row_rects(art, look);
    const formats::gaf::Sequence* checkbox = find_sequence(art, checkbox_sequence);
    for (std::size_t index = 0; index < rects.size() && index < look.rows.size(); ++index) {
        const RoomListRow& row = look.rows[index];
        const SourceRect& at = rects[index];
        if (row.rule_above) {
            const int32_t y = at.y - room_list_rule / 2 - 1;
            row_line(canvas, at.x + 2, at.x + at.width - 3, y, gui_rgb(art, gui_black));
            row_line(canvas, at.x + 2, at.x + at.width - 3, y + 1, gui_rgb(art, gui_light));
        }
        const RoomFont font = row.heading ? RoomFont::label : RoomFont::caption;
        const int32_t line = room_line_height(art, font);
        const int32_t text_y = at.y + (at.height - line) / 2;
        const int32_t value_left = at.x + at.width - 3 - look.value_width;
        const int32_t words_left = at.x + (row.heading ? 3 : room_list_indent);
        const int32_t words_width =
            row.heading ? at.width - 6 : std::max(value_left - 4 - words_left, int32_t{0});
        std::ignore = draw_room_text(
            canvas,
            art,
            font,
            fit_room_text(art, font, row.text, words_width),
            words_left,
            text_y,
            words_width
        );
        if (row.ticked && !row.heading) {
            // A tick two pixels thick: down from its left end, then up to its right.
            const Rgb ink = ink_of(art, font_of(art, RoomFont::caption));
            const int32_t foot = at.y + at.height / 2 + 2;
            const int32_t left = at.x + 2;
            for (int32_t step = 0; step < 3; ++step) {
                put(canvas, left + step, foot - 2 + step, ink);
                put(canvas, left + step, foot - 1 + step, ink);
            }
            for (int32_t step = 0; step < 5; ++step) {
                put(canvas, left + 3 + step, foot - step, ink);
                put(canvas, left + 3 + step, foot + 1 - step, ink);
            }
        }
        if (row.checkbox) {
            bool drawn = false;
            if (checkbox != nullptr) {
                const std::size_t frame =
                    std::min<std::size_t>(row.checked ? 1 : 0, checkbox->frames.size() - 1);
                formats::gaf::RenderResult rendered;
                if (const std::optional<FramePixels> pixels =
                        frame_pixels(checkbox->frames[frame], rendered)) {
                    blit(
                        canvas,
                        art.palette,
                        *pixels,
                        at.x + at.width - 3 - pixels->width,
                        at.y + (at.height - pixels->height) / 2
                    );
                    drawn = true;
                }
            }
            if (!drawn) {
                const int32_t side = std::min(at.height - 2, 11);
                const SourceRect box{
                    at.x + at.width - 3 - side, at.y + (at.height - side) / 2, side, side
                };
                frame_line(canvas, box, gui_rgb(art, gui_light), gui_rgb(art, gui_black));
                if (row.checked)
                    for (int32_t step = 2; step < side - 2; ++step) {
                        put(canvas, box.x + step, box.y + step, ink_of(art, nullptr));
                        put(canvas, box.x + side - 1 - step, box.y + step, ink_of(art, nullptr));
                    }
            }
        } else if (!row.value.empty() && look.value_width > 0) {
            const std::string value =
                fit_room_text(art, RoomFont::caption, row.value, look.value_width);
            const int32_t width = room_text_width(art, RoomFont::caption, value);
            std::ignore = draw_room_text(
                canvas,
                art,
                RoomFont::caption,
                value,
                at.x + at.width - 3 - width,
                text_y,
                look.value_width
            );
        }
        if (row.grey)
            darken_rect(canvas, art, at);
        if (row.selected && !row.heading)
            light_rect(canvas, art, at, selection_level);
    }
}

} // namespace oa::ui::frontend_renderer
