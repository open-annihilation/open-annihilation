// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/frontend_renderer/game_text.hpp"

#include "oa/present/display.hpp"
#include "oa/base/threads.hpp"

#include <algorithm>
#include <cstddef>
#include <limits>
#include <map>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::ui::frontend_renderer {

namespace {

/// The glyph whose height is a GUI font's line, and whose rows give an FNT
/// font's baseline.
constexpr unsigned char gui_height_glyph = 'I';
constexpr unsigned char fnt_baseline_glyph = 'H';
/// The first byte above ASCII.
constexpr unsigned char first_high_byte = 0x80;
/// The first byte raster_text takes: the space, which it draws nothing for.
constexpr unsigned char fnt_first_printable = ' ';
/// The smallest 'I' of a GUI font drawn in the message face: hattfont12's.
constexpr int32_t message_face_height = 12;
/// The smallest 'H' of an FNT font drawn in the message face.
constexpr int32_t message_face_capitals = 9;
/// Rows of the line height under an FNT font's baseline when it has no 'H'.
constexpr int32_t fnt_rows_under_baseline = 2;
/// Bytes of one light-table row, one per palette entry.
constexpr std::size_t light_row_bytes = 256;

/// The characters of each GUI font worked out so far, by its glyphs.
struct KeptCharacters {
    base::threads::Mutex mutex{};
    std::map<std::pair<const void*, uint16_t>, present::FontCharacters> fonts{};
};

KeptCharacters& kept_characters() {
    static KeptCharacters kept;
    return kept;
}

/// The rows an FNT glyph covers below the pen row, one past its last.
std::optional<int32_t> glyph_bottom(const formats::fnt::Font& font, unsigned char byte) {
    const auto& glyph = font.glyphs[byte];
    if (!glyph || glyph->width == 0)
        return std::nullopt;
    int32_t last = -1;
    for (int32_t row = 0; row < glyph->height; ++row)
        for (int32_t column = 0; column < glyph->width; ++column) {
            const auto at = static_cast<std::size_t>(row) * glyph->width + column;
            if (at < glyph->coverage.size() && glyph->coverage[at] != 0)
                last = row;
        }
    if (last < 0)
        return std::nullopt;
    return -glyph->origin_y - formats::fnt::row_lift(font) + last + 1;
}

} // namespace

present::FontCharacters gui_font_characters(const present::GafSprites& font) {
    if (font.sequences.empty())
        return present::FontCharacters::gui_font([](uint8_t) { return false; });
    const present::GafSequence* glyphs = &font.sequences.front();
    auto& kept = kept_characters();
    const base::threads::LockGuard lock(kept.mutex);
    const auto key = std::pair<const void*, uint16_t>{glyphs->frames, glyphs->frame_count};
    if (const auto found = kept.fonts.find(key); found != kept.fonts.end())
        return found->second;
    const oa::Sprite* box = present::gaf_frame(glyphs, 0);
    auto characters = present::FontCharacters::gui_font([&](uint8_t byte) {
        const oa::Sprite* glyph = present::gaf_frame(glyphs, byte);
        return glyph != nullptr && glyph->width != 0 && !present::same_glyph(glyph, box);
    });
    return kept.fonts.emplace(key, std::move(characters)).first->second;
}

void forget_gui_font_characters() {
    auto& kept = kept_characters();
    const base::threads::LockGuard lock(kept.mutex);
    kept.fonts.clear();
}

int32_t gui_font_baseline(const present::GafSprites& font) {
    const oa::Sprite* tall = font.sequences.empty()
                                 ? nullptr
                                 : present::gaf_frame(&font.sequences.front(), gui_height_glyph);
    return tall != nullptr ? tall->height : 0;
}

present::TextFace gui_font_face(const present::GafSprites& font) {
    return gui_font_baseline(font) >= message_face_height ? present::TextFace::message
                                                          : present::TextFace::status;
}

present::FontCharacters fnt_font_characters(const formats::fnt::Font& font) {
    return present::FontCharacters::fnt_font([&font](uint8_t byte) {
        return font.glyphs[byte].has_value() && font.glyphs[byte]->width != 0;
    });
}

int32_t fnt_font_baseline(const formats::fnt::Font& font) {
    if (const auto bottom = glyph_bottom(font, fnt_baseline_glyph))
        return *bottom;
    return static_cast<int32_t>(formats::fnt::line_height(font)) - fnt_rows_under_baseline -
           formats::fnt::row_lift(font);
}

uint8_t fnt_font_ink(const formats::fnt::Font& font, const PaletteBytes& palette) {
    uint8_t ink = formats::fnt::foreground_index;
    const auto& glyph = font.glyphs[fnt_baseline_glyph];
    if (!glyph)
        return ink;
    // Lightness as the sum of the three channels.
    const auto lightness = [&palette](uint8_t index) {
        const std::size_t at = static_cast<std::size_t>(index) * palette_entry_bytes;
        return palette[at] + palette[at + 1] + palette[at + 2];
    };
    int32_t lightest = -1;
    for (std::size_t at = 0; at < glyph->coverage.size() && at < glyph->pixels.size(); ++at)
        if (glyph->coverage[at] != 0 && lightness(glyph->pixels[at]) > lightest) {
            ink = glyph->pixels[at];
            lightest = lightness(ink);
        }
    return ink;
}

present::TextFace fnt_font_face(const formats::fnt::Font& font) {
    const auto& glyph = font.glyphs[fnt_baseline_glyph];
    int32_t capitals = 0;
    if (glyph) {
        int32_t first = -1;
        int32_t last = -1;
        for (int32_t row = 0; row < glyph->height; ++row)
            for (int32_t column = 0; column < glyph->width; ++column) {
                const auto at = static_cast<std::size_t>(row) * glyph->width + column;
                if (at < glyph->coverage.size() && glyph->coverage[at] != 0) {
                    first = first < 0 ? row : first;
                    last = row;
                }
            }
        capitals = first < 0 ? 0 : last - first + 1;
    }
    return capitals >= message_face_capitals ? present::TextFace::message
                                             : present::TextFace::label;
}

bool needs_text_runs(std::string_view text, bool game_text) {
    if (present::game_text_hooks().draw == nullptr || text.empty())
        return false;
    if (game_text && present::game_text_settings().style.modern_fonts)
        return true;
    return std::any_of(text.begin(), text.end(), [](char byte) {
        return static_cast<unsigned char>(byte) >= first_high_byte;
    });
}

std::vector<present::TextRun>
split_game_text(std::string_view text, const present::FontCharacters& font, bool game_text) {
    if (present::game_text_hooks().draw == nullptr) {
        if (text.empty())
            return {};
        return {{false, std::string(text), present::game_font_text_size}};
    }
    const auto settings = present::game_text_settings();
    const std::string utf8 = present::decode_game_text(text, settings.utf8);
    if (game_text && settings.style.modern_fonts) {
        if (utf8.empty())
            return {};
        return {{true, utf8, present::held_text_size(settings.style.size)}};
    }
    return present::split_text(utf8, font);
}

int32_t screen_text_size(const present::TextRun& run) noexcept {
    return std::min(run.size, present::game_font_text_size);
}

std::array<uint8_t, 3>
lit_text_color(std::array<uint8_t, 3> color, std::span<const uint8_t> palette, int32_t level) {
    const present::DisplayContext* display = present::display_context();
    if (level <= 0 || display == nullptr || display->light_table == nullptr ||
        (display->flags & present::display_flag_light_table) == 0 ||
        palette.size() < palette_color_count * palette_entry_bytes)
        return color;
    const uint8_t index = present::nearest_palette_index(palette, palette_entry_bytes, color);
    const uint8_t lit =
        display->light_table[static_cast<std::size_t>(level) * light_row_bytes + index];
    const std::size_t at = static_cast<std::size_t>(lit) * palette_entry_bytes;
    return {palette[at], palette[at + 1], palette[at + 2]};
}

void draw_fnt_glyphs(
    Surface& surface,
    const formats::fnt::Font& font,
    std::string_view text,
    int32_t x,
    int32_t y,
    const PaletteBytes& palette,
    const TextClip& clip,
    const uint8_t* light_row
) {
    // Bound the glyphs' pixels as raster_text places them, then keep the
    // part inside the clip and the surface.
    int64_t drawn_left = std::numeric_limits<int64_t>::max();
    int64_t drawn_top = std::numeric_limits<int64_t>::max();
    int64_t drawn_right = std::numeric_limits<int64_t>::min();
    int64_t drawn_bottom = std::numeric_limits<int64_t>::min();
    const int64_t lift = formats::fnt::row_lift(font);
    int64_t pen = x;
    for (const unsigned char byte : text) {
        if (byte < fnt_first_printable || !font.glyphs[byte])
            continue;
        const auto& glyph = *font.glyphs[byte];
        if (byte != fnt_first_printable) {
            const int64_t left = pen - glyph.origin_x;
            const int64_t top = int64_t{y} - glyph.origin_y - lift;
            drawn_left = std::min(drawn_left, left);
            drawn_top = std::min(drawn_top, top);
            drawn_right = std::max(drawn_right, left + glyph.width);
            drawn_bottom = std::max(drawn_bottom, top + glyph.height);
        }
        pen = std::clamp<int64_t>(
            pen + glyph.width,
            std::numeric_limits<int32_t>::min(),
            std::numeric_limits<int32_t>::max()
        );
    }
    int64_t left = std::max({drawn_left, int64_t{clip.left}, int64_t{0}});
    int64_t top = std::max({drawn_top, int64_t{clip.top}, int64_t{0}});
    int64_t right =
        std::min({drawn_right, int64_t{clip.right} + 1, static_cast<int64_t>(surface.width)});
    int64_t bottom =
        std::min({drawn_bottom, int64_t{clip.bottom} + 1, static_cast<int64_t>(surface.height)});
    if (left >= right || top >= bottom) {
        left = 0;
        top = 0;
        right = 0;
        bottom = 0;
    }
    const auto columns = static_cast<uint32_t>(right - left);
    const auto rows = static_cast<uint32_t>(bottom - top);
    const std::size_t count = static_cast<std::size_t>(columns) * rows;
    std::vector<uint8_t> indices(count);
    std::vector<uint8_t> coverage(count);
    const formats::fnt::IndexedSurface scratch{columns, rows, columns, indices, coverage};
    std::ignore = formats::fnt::raster_text(
        scratch,
        font,
        text,
        static_cast<int32_t>(std::clamp<int64_t>(
            int64_t{x} - left,
            std::numeric_limits<int32_t>::min(),
            std::numeric_limits<int32_t>::max()
        )),
        static_cast<int32_t>(std::clamp<int64_t>(
            int64_t{y} - top,
            std::numeric_limits<int32_t>::min(),
            std::numeric_limits<int32_t>::max()
        ))
    );
    for (uint32_t row = 0; row < rows; ++row) {
        const std::size_t from = static_cast<std::size_t>(row) * columns;
        uint8_t* out = surface.rgb.data() + (static_cast<std::size_t>(top + row) * surface.width +
                                             static_cast<std::size_t>(left)) *
                                                3U;
        for (uint32_t column = 0; column < columns; ++column, out += 3) {
            if (coverage[from + column] == 0)
                continue;
            const uint8_t index =
                light_row != nullptr ? light_row[indices[from + column]] : indices[from + column];
            const std::size_t entry = static_cast<std::size_t>(index) * palette_entry_bytes;
            std::copy_n(palette.begin() + static_cast<std::ptrdiff_t>(entry), 3, out);
        }
    }
}

int32_t draw_fnt_game_text(
    Surface& surface,
    const formats::fnt::Font& font,
    std::string_view text,
    int32_t x,
    int32_t y,
    std::array<uint8_t, 3> color,
    const PaletteBytes& palette,
    const TextClip& clip,
    bool game_text,
    int32_t most_size
) {
    const int32_t clip_right = clip.right;
    if (text.empty())
        return x;
    const auto runs = needs_text_runs(text, game_text)
                          ? split_game_text(text, fnt_font_characters(font), game_text)
                          : std::vector<present::TextRun>{
                                {false, std::string(text), present::game_font_text_size}
                            };
    // The screens are laid out for the game's fonts: modern text keeps the
    // font's baseline at any size.
    const int32_t baseline = y + fnt_font_baseline(font);
    const auto face = fnt_font_face(font);
    auto canvas = present::rgb_canvas(
        surface.rgb,
        static_cast<int32_t>(surface.width),
        static_cast<int32_t>(surface.height),
        palette
    );
    canvas.clip_left = std::max(canvas.clip_left, clip.left);
    canvas.clip_top = std::max(canvas.clip_top, clip.top);
    canvas.clip_right = std::min(canvas.clip_right, clip.right);
    canvas.clip_bottom = std::min(canvas.clip_bottom, clip.bottom);
    for (const auto& run : runs) {
        const int32_t size = std::min(screen_text_size(run), most_size);
        std::optional<present::TextLayers> layers;
        if (run.modern)
            layers = present::modern_text(run.text, face, 1, size);
        if (!layers) {
            const std::string bytes =
                run.modern ? present::encode_game_text(run.text, false) : run.text;
            // Glyphs up to the first that does not wholly fit.
            std::size_t fitted = 0;
            int32_t pen = x;
            for (const unsigned char byte : bytes) {
                const auto& glyph = font.glyphs[byte];
                const int32_t width = byte >= ' ' && glyph ? glyph->width : 0;
                if (pen + width - 1 > clip_right && width != 0)
                    break;
                pen += width;
                ++fitted;
            }
            draw_fnt_glyphs(
                surface, font, std::string_view(bytes).substr(0, fitted), x, y, palette, clip
            );
            x = pen;
            if (fitted < bytes.size())
                return x;
            continue;
        }
        if (x + layers->advance - 1 > clip_right) {
            const std::size_t fitted =
                present::modern_text_fit(run.text, face, 1, size, clip_right - x + 1);
            if (fitted != 0)
                if (const auto part =
                        present::modern_text(run.text.substr(0, fitted), face, 1, size))
                    present::lay_text(canvas, *part, x, baseline, color);
            return x;
        }
        present::lay_text(canvas, *layers, x, baseline, color);
        x += layers->advance;
    }
    return x;
}

int32_t draw_fnt_composition(
    Surface& surface,
    const formats::fnt::Font& font,
    std::string_view composition,
    int32_t x,
    int32_t y,
    std::array<uint8_t, 3> color,
    const PaletteBytes& palette,
    const TextClip& clip
) {
    if (composition.empty())
        return x;
    const auto underline = [&](int32_t left, int32_t row, int32_t width, int32_t rows) {
        for (int32_t py = std::max(row, clip.top); py < row + rows && py <= clip.bottom; ++py)
            for (int32_t px = std::max(left, clip.left); px < left + width && px <= clip.right;
                 ++px) {
                if (px < 0 || py < 0 || px >= static_cast<int32_t>(surface.width) ||
                    py >= static_cast<int32_t>(surface.height))
                    continue;
                const auto at =
                    (static_cast<std::size_t>(py) * surface.width + static_cast<std::size_t>(px)) *
                    3U;
                std::copy(
                    color.begin(),
                    color.end(),
                    surface.rgb.begin() + static_cast<std::ptrdiff_t>(at)
                );
            }
    };
    const std::string text =
        present::decode_game_text(composition, present::game_text_settings().utf8);
    const auto face = fnt_font_face(font);
    constexpr int32_t size = present::game_font_text_size;
    std::size_t shown = text.size();
    std::optional<present::TextLayers> layers = present::modern_text(text, face, 1, size);
    if (layers && x + layers->advance - 1 > clip.right) {
        shown = present::modern_text_fit(text, face, 1, size, clip.right - x + 1);
        layers = shown != 0
                     ? present::modern_text(std::string_view(text).substr(0, shown), face, 1, size)
                     : std::nullopt;
        if (!layers)
            return x;
    }
    if (!layers) {
        const int32_t pen =
            draw_fnt_game_text(surface, font, composition, x, y, color, palette, clip, false);
        underline(
            x,
            y + static_cast<int32_t>(formats::fnt::line_height(font)) - 1 -
                formats::fnt::row_lift(font),
            pen - x,
            1
        );
        return pen;
    }
    // The modern line keeps the font's baseline, as the line's own modern
    // characters do.
    const int32_t baseline = y + fnt_font_baseline(font);
    auto canvas = present::rgb_canvas(
        surface.rgb,
        static_cast<int32_t>(surface.width),
        static_cast<int32_t>(surface.height),
        palette
    );
    canvas.clip_left = std::max(canvas.clip_left, clip.left);
    canvas.clip_top = std::max(canvas.clip_top, clip.top);
    canvas.clip_right = std::min(canvas.clip_right, clip.right);
    canvas.clip_bottom = std::min(canvas.clip_bottom, clip.bottom);
    present::lay_text(canvas, *layers, x, baseline, color);
    if (const auto marked = present::modern_text_underline(
            std::string_view(text).substr(0, shown), face, 1, size, 0, shown
        ))
        underline(x + marked->left, baseline + marked->row, marked->width, marked->thickness);
    return x + layers->advance;
}

int32_t measure_fnt_game_text(
    const formats::fnt::Font& font, std::string_view text, bool game_text, int32_t most_size
) {
    if (!needs_text_runs(text, game_text))
        return static_cast<int32_t>(formats::fnt::measure_text(font, text));
    int32_t width = 0;
    for (const auto& run : split_game_text(text, fnt_font_characters(font), game_text)) {
        if (run.modern)
            if (const auto layers = present::modern_text(
                    run.text, fnt_font_face(font), 1, std::min(screen_text_size(run), most_size)
                )) {
                width += layers->advance;
                continue;
            }
        const std::string bytes =
            run.modern ? present::encode_game_text(run.text, false) : run.text;
        width += static_cast<int32_t>(formats::fnt::measure_text(font, bytes));
    }
    return width;
}

int32_t fitted_fnt_text_size(
    const formats::fnt::Font& font, std::string_view text, int32_t room, bool game_text
) {
    if (!needs_text_runs(text, game_text))
        return present::game_font_text_size;
    return present::fitted_text_size(
        [&](int32_t most_size) { return measure_fnt_game_text(font, text, game_text, most_size); },
        room
    );
}

std::optional<LineRows> fnt_game_text_modern_rows(
    const formats::fnt::Font& font, std::string_view text, bool game_text, int32_t most_size
) {
    if (!needs_text_runs(text, game_text))
        return std::nullopt;
    // Each run lies with its baseline on the font's.
    const int32_t baseline = fnt_font_baseline(font);
    std::optional<LineRows> rows;
    for (const auto& run : split_game_text(text, fnt_font_characters(font), game_text)) {
        if (!run.modern)
            continue;
        const auto layers = present::modern_text(
            run.text, fnt_font_face(font), 1, std::min(screen_text_size(run), most_size)
        );
        if (!layers || layers->drawn_top < 0)
            continue;
        const int32_t top = baseline - layers->baseline + layers->drawn_top;
        const int32_t bottom = baseline - layers->baseline + layers->drawn_bottom;
        rows = rows ? LineRows{std::min(rows->top, top), std::max(rows->bottom, bottom)}
                    : LineRows{top, bottom};
    }
    return rows;
}

CaptionRows fnt_caption_rows(
    const formats::fnt::Font& font,
    std::string_view text,
    bool game_text,
    int32_t top,
    int32_t height
) {
    CaptionRows placed{top, present::game_font_text_size};
    if (!fnt_game_text_modern_rows(font, text, game_text))
        return placed;
    placed.most_size = present::fitted_text_size(
        [&](int32_t most_size) {
            const auto rows = fnt_game_text_modern_rows(font, text, game_text, most_size);
            return rows ? rows->bottom - rows->top + 1 : 0;
        },
        height
    );
    if (const auto rows = fnt_game_text_modern_rows(font, text, game_text, placed.most_size)) {
        placed.pen_row = std::min(placed.pen_row, top + height - 1 - rows->bottom);
        placed.pen_row = std::max(placed.pen_row, top - rows->top);
    }
    return placed;
}

} // namespace oa::ui::frontend_renderer
