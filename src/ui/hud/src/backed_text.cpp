// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/hud/backed_text.hpp"

#include <algorithm>
#include <optional>
#include <utility>

namespace oa::ui::hud {
namespace {

/// Bytes of a palette entry: red, green, blue and one more.
constexpr std::size_t kPaletteEntryBytes = 4;
/// Coverage of a pixel a letter covers whole.
constexpr uint8_t kWholeCoverage = 255;

/// Gives the colour of a palette entry; black past the palette's end.
std::array<uint8_t, 3> entry_colour(std::span<const uint8_t> palette, uint8_t index) noexcept {
    const std::size_t at = static_cast<std::size_t>(index) * kPaletteEntryBytes;
    if (at + 2 >= palette.size())
        return {};
    return {palette[at], palette[at + 1], palette[at + 2]};
}

/// Gives the colour a colour is drawn in on RGB pixels reduced to a palette.
std::array<uint8_t, 3>
reduced_colour(std::span<const uint8_t> palette, std::array<uint8_t, 3> colour) noexcept {
    if (palette.empty())
        return colour;
    return entry_colour(
        palette, present::nearest_palette_index(palette, kPaletteEntryBytes, colour)
    );
}

/// Where a laid line's layers lie on the pixels.
struct Placed {
    const LaidLine* line{};
    int32_t left{}; ///< the column of the layers' first column
    int32_t top{};  ///< the row of the layers' first row

    /// Gives the index into the layers of a pixel.
    ///
    /// @param x column
    /// @param y row
    /// @return the index; none outside the layers
    [[nodiscard]] std::optional<std::size_t> at(int32_t x, int32_t y) const noexcept {
        const auto& layers = line->layers;
        const int32_t column = x - left;
        const int32_t row = y - top;
        if (column < 0 || row < 0 || column >= layers.width || row >= layers.height)
            return std::nullopt;
        const auto index = static_cast<std::size_t>(row) * static_cast<std::size_t>(layers.width) +
                           static_cast<std::size_t>(column);
        if (index >= layers.fill.size() || index >= layers.outline.size() ||
            index >= layers.shadow.size())
            return std::nullopt;
        return index;
    }

    /// Tells whether the line's letters, outline or shadow lie on a pixel.
    ///
    /// @param x column
    /// @param y row
    /// @return true where any of them does
    [[nodiscard]] bool inked(int32_t x, int32_t y) const noexcept {
        const auto index = at(x, y);
        if (!index)
            return false;
        const auto& layers = line->layers;
        return layers.fill[*index] != 0 || layers.outline[*index] != 0 ||
               layers.shadow[*index] != 0;
    }

    /// Tells whether the line's backing lies on a pixel.
    ///
    /// @param x column
    /// @param y row
    /// @return true inside a backed line's box
    [[nodiscard]] bool backs(int32_t x, int32_t y) const noexcept {
        const auto& layers = line->layers;
        const int32_t column = x - left;
        const int32_t row = y - top;
        return line->backed && column >= 0 && column < layers.width &&
               row >= layers.background_top && row <= layers.background_bottom;
    }
};

} // namespace

void lay_text_backing(
    present::TextCanvas& canvas, const present::TextLayers& layers, int32_t x, int32_t baseline_y
) {
    if (!layers.background || canvas.pixels == nullptr || layers.width <= 0 || layers.height <= 0)
        return;
    if (canvas.indexed && canvas.palette.size() < kPaletteEntryBytes)
        return;
    const int32_t left = x - layers.pen;
    const int32_t top = baseline_y - layers.baseline;
    const int32_t first_row = std::max({top + layers.background_top, top, canvas.clip_top});
    const int32_t last_row =
        std::min({top + layers.background_bottom, top + layers.height - 1, canvas.clip_bottom});
    const int32_t first_column = std::max(left, canvas.clip_left);
    const int32_t last_column = std::min(left + layers.width - 1, canvas.clip_right);
    // The box's colour, reduced to the palette as lay_text reduces it.
    const uint8_t index = canvas.palette.empty()
                              ? uint8_t{0}
                              : present::nearest_palette_index(
                                    canvas.palette, kPaletteEntryBytes, present::text_shade_color
                                );
    const std::array<uint8_t, 3> colour =
        canvas.palette.empty() ? present::text_shade_color : entry_colour(canvas.palette, index);
    for (int32_t row = first_row; row <= last_row; ++row) {
        uint8_t* line = canvas.pixels + static_cast<std::ptrdiff_t>(row) * canvas.pitch;
        for (int32_t column = first_column; column <= last_column; ++column) {
            if (canvas.indexed) {
                line[column] = index;
                continue;
            }
            uint8_t* pixel = line + static_cast<std::ptrdiff_t>(column) * 3;
            std::copy(colour.begin(), colour.end(), pixel);
        }
    }
}

present::TextLayers without_backing(present::TextLayers layers) {
    layers.background = false;
    return layers;
}

TextHold::Pause::Pause(TextHold& hold) noexcept : hold_(&hold) {
    ++hold_->paused_;
}

TextHold::Pause::~Pause() {
    --hold_->paused_;
}

void TextHold::open(bool holds) {
    if (depth_++ == 0)
        holds_ = holds;
}

void TextHold::close() {
    if (depth_ == 0 || --depth_ != 0)
        return;
    // The lines are taken first: a line laid now is laid at once, and one
    // that opens a group of its own holds nothing of these.
    std::vector<std::function<void()>> lines = std::move(lines_);
    lines_.clear();
    holds_ = false;
    for (const auto& lay : lines)
        lay();
}

void TextHold::drop() noexcept {
    lines_.clear();
    depth_ = 0;
    holds_ = false;
}

bool TextHold::holding() const noexcept {
    return depth_ > 0 && holds_ && paused_ == 0;
}

void TextHold::hold(std::function<void()> lay) {
    if (!holding()) {
        lay();
        return;
    }
    lines_.push_back(std::move(lay));
}

std::size_t TextHold::held() const noexcept {
    return lines_.size();
}

LaidLinesFound find_laid_lines(
    std::span<const uint8_t> rgb,
    int32_t width,
    int32_t height,
    std::span<const uint8_t> palette,
    std::span<const LaidLine> lines
) {
    LaidLinesFound found;
    if (width <= 0 || height <= 0 ||
        rgb.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U)
        return found;
    std::vector<Placed> placed;
    placed.reserve(lines.size());
    for (const LaidLine& line : lines)
        placed.push_back({&line, line.x - line.layers.pen, line.baseline_y - line.layers.baseline});
    const auto pixel = [&](int32_t x, int32_t y) {
        const auto at = (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                         static_cast<std::size_t>(x)) *
                        3U;
        return std::array<uint8_t, 3>{rgb[at], rgb[at + 1], rgb[at + 2]};
    };
    const auto inside = [&](int32_t x, int32_t y) {
        return x >= 0 && y >= 0 && x < width && y < height;
    };
    const auto inked_by_any = [&](int32_t x, int32_t y) {
        return std::any_of(placed.begin(), placed.end(), [&](const Placed& other) {
            return other.inked(x, y);
        });
    };
    // The letters: whole wherever no later line has ink.
    for (std::size_t index = 0; index < placed.size(); ++index) {
        const Placed& line = placed[index];
        const auto& layers = line.line->layers;
        const auto colour = reduced_colour(palette, line.line->color);
        const auto later = std::span<const Placed>(placed).subspan(index + 1);
        for (int32_t row = 0; row < layers.height; ++row)
            for (int32_t column = 0; column < layers.width; ++column) {
                const int32_t x = line.left + column;
                const int32_t y = line.top + row;
                const auto at = line.at(x, y);
                if (!inside(x, y) || !at || layers.fill[*at] != kWholeCoverage)
                    continue;
                ++found.letter_pixels;
                if (std::any_of(later.begin(), later.end(), [&](const Placed& other) {
                        return other.backs(x, y);
                    }))
                    ++found.letters_under_later_backings;
                if (std::any_of(later.begin(), later.end(), [&](const Placed& other) {
                        return other.inked(x, y);
                    }))
                    continue;
                if (pixel(x, y) != colour)
                    ++found.covered_letters;
            }
    }
    // The backings: their colour wherever no line has ink.
    const auto box_colour = reduced_colour(palette, present::text_shade_color);
    for (const Placed& line : placed) {
        if (!line.line->backed)
            continue;
        const auto& layers = line.line->layers;
        std::size_t pixels = 0;
        std::size_t shown = 0;
        for (int32_t row = layers.background_top; row <= layers.background_bottom; ++row)
            for (int32_t column = 0; column < layers.width; ++column) {
                const int32_t x = line.left + column;
                const int32_t y = line.top + row;
                if (!inside(x, y) || inked_by_any(x, y))
                    continue;
                ++pixels;
                if (pixel(x, y) == box_colour)
                    ++shown;
            }
        found.backing_pixels += pixels;
        found.backing_pixels_shown += shown;
        if (shown * 2 < pixels)
            ++found.bare_backings;
    }
    return found;
}

} // namespace oa::ui::hud
