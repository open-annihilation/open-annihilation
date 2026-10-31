// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The game's two dialog fonts, the modern faces that draw what they lack,
// the characters drawn in place of the ones those fonts do not hold, and
// the modern look's lines in Barlow.

#include "oa/ui/kit/text.hpp"

#include "oa/base/text/line_break.hpp"
#include "oa/formats/fnt.hpp"
#include "oa/ui/decoded.hpp"
#include "oa/ui/frontend_renderer/game_text.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oa::ui::kit {

namespace {

namespace renderer = oa::ui::frontend_renderer;
namespace present = oa::present;
namespace text_font = oa::platform::text_font;

/// The most a percent scale is: a point drawn at 64 pixels.
constexpr int32_t most_scale_percent = 6400;

/// Returns a length in points at a scale in percent, to the nearest pixel.
///
/// @param points the length, in points
/// @param scale_percent pixels a point, in percent
/// @return the pixels
int32_t scaled_pixels(int32_t points, int32_t scale_percent) noexcept {
    const int64_t product = static_cast<int64_t>(points) * scale_percent;
    // A half away from zero, as the canvas rounds its points.
    return static_cast<int32_t>(product >= 0 ? (product + 50) / 100 : (product - 50) / 100);
}

/// Returns the style a modern type is drawn in at a scale.
///
/// @param type the type
/// @param scale_percent pixels a point, in percent
/// @return the style, antialiased
text_font::Style modern_style(const ModernType& type, int32_t scale_percent) {
    text_font::Style style;
    style.pixel_size = std::clamp(
        scaled_pixels(type.points, scale_percent), int32_t{1}, text_font::max_pixel_size
    );
    style.weight = type.weight;
    style.family = type.family;
    style.rendering = text_font::Rendering::antialiased;
    return style;
}

/// Lays a modern line's characters into a surface at a whole scale, each
/// pixel blended with the colour by its coverage, within the placement's
/// clip.
///
/// @param[in,out] target the surface
/// @param placement where the picture lands, and its clip
/// @param line the line, drawn at the placement's scale
/// @param pen the pen's column, in the surface's pixels
/// @param baseline the baseline's row, in the surface's pixels
/// @param colour the text's colour
void lay_modern_line(
    renderer::Surface& target,
    const renderer::Placement& placement,
    const ModernLine& line,
    int32_t pen,
    int32_t baseline,
    std::array<uint8_t, 3> colour
) {
    const auto width = static_cast<int32_t>(target.width);
    const auto height = static_cast<int32_t>(target.height);
    if (target.rgb.size() < static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 3U)
        return;
    int32_t clip_left = 0;
    int32_t clip_top = 0;
    int32_t clip_right = width;
    int32_t clip_bottom = height;
    const int32_t scale = std::max(placement.scale, 1);
    if (placement.clip.width > 0 && placement.clip.height > 0) {
        clip_left = std::max(clip_left, placement.x + placement.clip.x * scale);
        clip_top = std::max(clip_top, placement.y + placement.clip.y * scale);
        clip_right =
            std::min(clip_right, placement.x + (placement.clip.x + placement.clip.width) * scale);
        clip_bottom =
            std::min(clip_bottom, placement.y + (placement.clip.y + placement.clip.height) * scale);
    }
    for (const ModernGlyph& glyph : line.glyphs) {
        const text_font::Coverage& drawn = glyph.coverage;
        const int32_t left = pen + glyph.pen - drawn.origin;
        const int32_t top = baseline - drawn.baseline;
        for (int32_t row = 0; row < drawn.height; ++row) {
            const int32_t y = top + row;
            if (y < clip_top || y >= clip_bottom)
                continue;
            for (int32_t column = 0; column < drawn.width; ++column) {
                const int32_t x = left + column;
                if (x < clip_left || x >= clip_right)
                    continue;
                const uint32_t cover =
                    drawn.alpha[static_cast<std::size_t>(row * drawn.width + column)];
                if (cover == 0)
                    continue;
                const std::size_t at =
                    (static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(x)) *
                    3U;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const uint32_t under = target.rgb[at + channel];
                    const uint32_t over = colour[channel];
                    target.rgb[at + channel] =
                        static_cast<uint8_t>((under * (255U - cover) + over * cover + 127U) / 255U);
                }
            }
        }
    }
}

/// A font with the characters it draws and the modern face that draws the others.
struct FaceFont {
    const renderer::TextFont& font;                     ///< the game font
    const present::FontCharacters& characters;          ///< what it draws
    present::TextFace face{present::TextFace::message}; ///< the modern face for the rest
    /// The game font holds no glyphs, as before the game's files are
    /// installed: the modern face draws every text.
    bool modern_only{};
};

/// Tells whether a font holds any glyph.
///
/// @param font the font
/// @return false for a font with no glyph at all
bool holds_glyphs(const oa::formats::fnt::Font& font) noexcept {
    return std::any_of(font.glyphs.begin(), font.glyphs.end(), [](const auto& glyph) {
        return glyph.has_value();
    });
}

/// Returns one of the two fonts.
///
/// @param fonts the fonts
/// @param role which of them
/// @return the font, the message face for the regular one and the status face for the small one
FaceFont face_of(const Fonts& fonts, FontRole role) noexcept {
    const bool small = role == FontRole::small;
    const renderer::TextFont& font = small ? fonts.small : fonts.regular;
    const present::FontCharacters& characters =
        small ? fonts.small_characters : fonts.regular_characters;
    return {font, characters, modern_face(role), !holds_glyphs(font.font)};
}

/// Tells whether a text is all ASCII, which a game font draws byte for byte.
///
/// @param text the text
/// @return true without a byte from 0x80 up
bool plain_ascii(std::string_view text) noexcept {
    return std::all_of(text.begin(), text.end(), [](char letter) {
        return static_cast<unsigned char>(letter) < 0x80;
    });
}

/// A character a game font may lack, and the characters it draws in its place.
struct StandIn {
    char32_t character{};        ///< the character
    std::string_view utf8{};     ///< the character in UTF-8
    std::string_view in_place{}; ///< what the game font draws instead
};

/// The ellipsis as three full stops, the mark between a location's folders
/// as a greater-than sign, and a no-break space as the font's own space, so
/// that it is drawn and measured as wide as a space. The wrap reads the text
/// before these stand in, so a line never breaks at a no-break space.
constexpr std::array<StandIn, 3> stand_ins{{
    {U'\u2026', "\xE2\x80\xA6", "..."},
    {U'\u203a', "\xE2\x80\xBA", ">"},
    {U'\u00a0', "\xC2\xA0", " "},
}};

/// The game's palette, which the GUI fonts' glyph pixels index.
constexpr std::string_view game_palette_path = "palettes/palette.pal";

/// Returns a UTF-8 text's width: the characters the font draws at its glyphs'
/// widths, the others as the modern fonts draw them.
///
/// @param font the font
/// @param shown the text, already stood in
/// @return the width, in points
int32_t width_of(const FaceFont& font, std::string_view shown) {
    if (font.modern_only) {
        return present::modern_text_advance(shown, font.face, 1, present::game_font_text_size)
            .value_or(0);
    }
    if (plain_ascii(shown))
        return renderer::text_width(font.font, shown);
    int32_t width = 0;
    for (const auto& run : present::split_text(shown, font.characters)) {
        if (!run.modern)
            width += renderer::text_width(font.font, run.text);
        else
            width += present::modern_text_advance(run.text, font.face, 1, run.size).value_or(0);
    }
    return width;
}

} // namespace

std::size_t character_bytes(std::string_view text) noexcept {
    if (text.empty())
        return 0;
    const auto sequence = present::utf8_sequence(text);
    return sequence.bytes != 0 ? sequence.bytes : 1;
}

bool continuation_byte(char byte) noexcept {
    return (static_cast<unsigned char>(byte) & 0xC0U) == 0x80U;
}

std::size_t character_count(std::string_view text) noexcept {
    std::size_t count = 0;
    for (const char byte : text)
        if (!continuation_byte(byte))
            ++count;
    return count;
}

int32_t estimated_width(std::string_view text) {
    return static_cast<int32_t>(oa::base::text::text_columns(text)) * estimated_character_width;
}

std::string fit(std::string_view text, int32_t width, const Measure& measure) {
    if (measure(text) <= width)
        return std::string(text);
    if (measure(ellipsis) > width)
        return {};
    std::vector<std::size_t> ends;
    for (std::size_t at = 0; at < text.size();) {
        at += std::max<std::size_t>(character_bytes(text.substr(at)), 1);
        ends.push_back(std::min(at, text.size()));
    }
    std::size_t low = 0;
    std::size_t high = ends.size();
    std::string best(ellipsis);
    while (low < high) {
        const std::size_t middle = (low + high + 1) / 2;
        std::string candidate(text.substr(0, ends[middle - 1]));
        while (!candidate.empty() && candidate.back() == ' ')
            candidate.pop_back();
        candidate += ellipsis;
        if (measure(candidate) <= width) {
            best = std::move(candidate);
            low = middle;
        } else {
            high = middle - 1;
        }
    }
    return best;
}

int32_t measured_width(
    const TextMeasureHooks& hooks, std::string_view text, int32_t pixel_size, bool bold
) {
    if (text.empty())
        return 0;
    if (hooks.width != nullptr)
        return hooks.width(hooks.context, text, pixel_size, bold);
    return static_cast<int32_t>(
        std::ceil(0.55 * pixel_size * static_cast<double>(character_count(text)))
    );
}

int32_t measured_line(const TextMeasureHooks& hooks, int32_t pixel_size, bool bold) {
    if (hooks.line_height != nullptr)
        return std::max(1, hooks.line_height(hooks.context, pixel_size, bold));
    return static_cast<int32_t>(std::ceil(1.25 * pixel_size));
}

int32_t modern_width(
    text_font::FontStack& fonts, std::string_view text, const ModernType& type, int32_t tracking
) {
    if (text.empty())
        return 0;
    const auto placed = fonts.layout(text, modern_style(type, 100));
    if (!placed || placed->empty())
        return 0;
    const auto gaps = static_cast<int32_t>(placed->size()) - 1;
    return placed->back().pen + placed->back().advance + tracking * gaps;
}

ModernLine modern_line(
    text_font::FontStack& fonts,
    std::string_view text,
    const ModernType& type,
    int32_t tracking,
    int32_t scale_percent
) {
    ModernLine line;
    if (text.empty())
        return line;
    const int32_t percent = std::clamp(scale_percent, int32_t{1}, most_scale_percent);
    const text_font::Style measured = modern_style(type, 100);
    const auto placed = fonts.layout(text, measured);
    if (!placed || placed->empty())
        return line;
    const auto gaps = static_cast<int32_t>(placed->size()) - 1;
    line.width = placed->back().pen + placed->back().advance + tracking * gaps;
    // At a scale of 1 without tracking the line's own pens are its glyphs'.
    if (percent == 100 && tracking == 0) {
        if (auto whole = fonts.draw(text, measured))
            line.glyphs.push_back({std::move(*whole), 0});
        return line;
    }
    text_font::Style drawn = modern_style(type, percent);
    // Each character alone is drawn in the family the whole line takes.
    drawn.family = fonts.line_family(text, measured);
    line.glyphs.reserve(placed->size());
    for (std::size_t index = 0; index < placed->size(); ++index) {
        const text_font::Placement& placement = (*placed)[index];
        std::string character;
        present::append_utf8(character, placement.character);
        auto coverage = fonts.draw(character, drawn);
        if (!coverage)
            continue;
        const int32_t pen = placement.pen + tracking * static_cast<int32_t>(index);
        line.glyphs.push_back({std::move(*coverage), scaled_pixels(pen, percent)});
    }
    return line;
}

int32_t
modern_capitals(text_font::FontStack& fonts, const ModernType& type, int32_t scale_percent) {
    const auto drawn = fonts.draw(
        "H", modern_style(type, std::clamp(scale_percent, int32_t{1}, most_scale_percent))
    );
    if (!drawn || drawn->width <= 0)
        return 0;
    constexpr uint8_t half_covered = 128;
    for (int32_t row = 0; row < drawn->height && row < drawn->baseline; ++row)
        for (int32_t column = 0; column < drawn->width; ++column)
            if (drawn->alpha[static_cast<std::size_t>(row * drawn->width + column)] >= half_covered)
                return drawn->baseline - row;
    return 0;
}

std::string with_stand_ins(const Fonts& fonts, FontRole role, std::string_view text) {
    // Barlow and the faces after it draw these characters themselves.
    if (modern_look(fonts))
        return std::string(text);
    const FaceFont font = face_of(fonts, role);
    std::string shown(text);
    // A line the modern fonts draw whole draws these characters itself.
    if (font.modern_only || present::drawn_whole_in_modern_fonts(text, font.characters))
        return shown;
    for (const StandIn& stand_in : stand_ins) {
        if (font.characters.byte_for(stand_in.character))
            continue;
        for (std::size_t at = shown.find(stand_in.utf8); at != std::string::npos;
             at = shown.find(stand_in.utf8, at + stand_in.in_place.size()))
            shown.replace(at, stand_in.utf8.size(), stand_in.in_place);
    }
    return shown;
}

int32_t text_width(const Fonts& fonts, FontRole role, std::string_view text) {
    if (modern_look(fonts))
        return modern_width(*fonts.modern, text, modern_type(role, false, fonts.size_class), 0);
    const std::string drawn = with_stand_ins(fonts, role, text);
    return width_of(face_of(fonts, role), drawn);
}

int32_t tracked_width(const Fonts& fonts, FontRole role, std::string_view text, int32_t tracking) {
    if (text.empty())
        return 0;
    if (modern_look(fonts))
        return modern_width(
            *fonts.modern, text, modern_type(role, tracking > 0, fonts.size_class), tracking
        );
    int32_t characters = 0;
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t bytes = character_bytes(text.substr(at));
        if (bytes == 0)
            break;
        ++characters;
        at += bytes;
    }
    return text_width(fonts, role, text) + tracking * (characters - 1);
}

int32_t modern_tracked_width(
    const Fonts& fonts, FontRole role, std::string_view text, int32_t tracking, int32_t scale
) {
    if (modern_look(fonts))
        return tracked_width(fonts, role, text, tracking);
    const FaceFont font = face_of(fonts, role);
    const int32_t used = std::max(scale, 1);
    int32_t width = 0;
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t bytes = character_bytes(text.substr(at));
        if (bytes == 0)
            break;
        if (at > 0)
            width += tracking;
        if (const auto advance = present::modern_text_advance(
                text.substr(at, bytes), font.face, used, present::game_font_text_size
            ))
            width += (*advance + used - 1) / used;
        at += bytes;
    }
    return width;
}

int32_t capitals(const Fonts& fonts, FontRole role) {
    if (modern_look(fonts))
        return modern_capitals(*fonts.modern, modern_type(role, false, fonts.size_class), 100);
    const FaceFont font = face_of(fonts, role);
    if (!font.modern_only)
        return font.font.font.nominal_height;
    const auto layers =
        present::modern_text("H", font.face, 1, present::game_font_text_size, false);
    if (!layers || layers->width <= 0)
        return 0;
    for (int32_t row = 0; row < layers->height && row < layers->baseline; ++row)
        for (int32_t column = 0; column < layers->width; ++column)
            if (layers->fill[static_cast<std::size_t>(row * layers->width + column)] != 0)
                return layers->baseline - row;
    return 0;
}

int32_t draw_text(
    renderer::Surface& target,
    const renderer::Placement& placement,
    const Fonts& fonts,
    FontRole role,
    std::string_view text,
    int32_t pen,
    int32_t pen_row,
    std::array<uint8_t, 3> colour
) {
    if (modern_look(fonts)) {
        const ModernType type = modern_type(role, false, fonts.size_class);
        const int32_t scale = placement.scale;
        const ModernLine line = modern_line(*fonts.modern, text, type, 0, std::max(scale, 1) * 100);
        if (scale >= 1) {
            // A game font's capitals stand on the row under its nominal
            // height; the modern line's stand there too.
            const int32_t baseline = pen_row + 1 + modern_capitals(*fonts.modern, type, 100);
            lay_modern_line(
                target,
                placement,
                line,
                placement.x + pen * scale,
                placement.y + baseline * scale,
                colour
            );
        }
        return pen + line.width;
    }
    const FaceFont font = face_of(fonts, role);
    if (!font.modern_only && plain_ascii(text))
        return renderer::draw_text(target, placement, font.font, text, pen, pen_row, colour);
    const int32_t scale = std::max(placement.scale, 1);
    // A font the modern face stands in for wholly draws the whole text in it.
    std::vector<present::TextRun> runs;
    if (font.modern_only)
        runs.push_back(present::TextRun{true, std::string(text), present::game_font_text_size});
    else
        runs = present::split_text(text, font.characters);
    const int32_t capital_rows = capitals(fonts, role);
    for (const auto& run : runs) {
        if (!run.modern) {
            pen = renderer::draw_text(target, placement, font.font, run.text, pen, pen_row, colour);
            continue;
        }
        auto layers = present::modern_text(run.text, font.face, scale, run.size, false);
        if (!layers || placement.scale < 1)
            continue;
        // Letters no lighter than their outline, as on the accent's light
        // face, are drawn bare: the outline and the shadow would only
        // thicken them into a blot there.
        if (std::equal(
                colour.begin(),
                colour.end(),
                present::text_outline_color.begin(),
                [](uint8_t letter, uint8_t outline) { return letter <= outline; }
            )) {
            std::fill(layers->outline.begin(), layers->outline.end(), uint8_t{0});
            std::fill(layers->shadow.begin(), layers->shadow.end(), uint8_t{0});
        }
        auto canvas = present::rgb_canvas(
            target.rgb, static_cast<int32_t>(target.width), static_cast<int32_t>(target.height), {}
        );
        if (placement.clip.width > 0 && placement.clip.height > 0) {
            canvas.clip_left = std::max(canvas.clip_left, placement.x + placement.clip.x * scale);
            canvas.clip_top = std::max(canvas.clip_top, placement.y + placement.clip.y * scale);
            canvas.clip_right = std::min(
                canvas.clip_right,
                placement.x + (placement.clip.x + placement.clip.width) * scale - 1
            );
            canvas.clip_bottom = std::min(
                canvas.clip_bottom,
                placement.y + (placement.clip.y + placement.clip.height) * scale - 1
            );
        }
        // A game font's capitals stand on the row under its nominal height.
        const int32_t baseline = pen_row + 1 + capital_rows;
        present::lay_text(
            canvas, *layers, placement.x + pen * scale, placement.y + baseline * scale, colour
        );
        pen += (layers->advance + scale - 1) / scale;
    }
    return pen;
}

void draw_boxed_text(
    renderer::Surface& target,
    const renderer::Placement& placement,
    const Fonts& fonts,
    FontRole role,
    std::string_view shown,
    const renderer::SourceRect& box,
    Align align,
    std::array<uint8_t, 3> colour,
    int32_t tracking
) {
    const std::string drawn = with_stand_ins(fonts, role, shown);
    shown = drawn;
    const int32_t width = tracked_width(fonts, role, shown, tracking);
    int32_t pen = box.x;
    if (align == Align::centre)
        pen = box.x + (box.width - width) / 2;
    else if (align == Align::right)
        pen = box.x + box.width - width;
    // The modern look's line takes the tracked type when it has tracking.
    const ModernType type = modern_type(role, tracking > 0, fonts.size_class);
    // A game font's capitals start one row under the pen row. The ink is that
    // band, not the whole line box: a title's box and the line under it share
    // two rows, and their glyphs meet at the edge.
    const int32_t capital_rows =
        modern_look(fonts) ? modern_capitals(*fonts.modern, type, 100) : capitals(fonts, role);
    const int32_t pen_row = box.y + (box.height - capital_rows) / 2 - 1;
    int32_t ink_y = box.y;
    int32_t ink_height = box.height;
    if (capital_rows > 0) {
        ink_y = pen_row + 1;
        ink_height = capital_rows;
    }
    // A canvas moved with placed_at draws in its own points. The record is in
    // the root canvas's points, so texts on different canvases can be compared.
    const int32_t origin_x = placement.scale > 0 ? placement.x / placement.scale : 0;
    const int32_t origin_y = placement.scale > 0 ? placement.y / placement.scale : 0;
    const int32_t note_x = box.x + origin_x;
    const int32_t note_y = ink_y + origin_y;
    const int32_t note_pen = pen + origin_x;
    if (placement.scale == 0)
        note_text_fit(shown, width, note_x, note_y, box.width, ink_height, note_pen, 0, 0, -1, 0);
    else if (placement.clip.width > 0 && placement.clip.height > 0)
        note_text_fit(
            shown,
            width,
            note_x,
            note_y,
            box.width,
            ink_height,
            note_pen,
            placement.clip.x + origin_x,
            placement.clip.y + origin_y,
            placement.clip.width,
            placement.clip.height
        );
    else
        note_text_fit(shown, width, note_x, note_y, box.width, ink_height, note_pen);
    if (modern_look(fonts)) {
        const int32_t scale = placement.scale;
        if (scale < 1)
            return;
        const ModernLine line = modern_line(*fonts.modern, shown, type, tracking, scale * 100);
        lay_modern_line(
            target,
            placement,
            line,
            placement.x + pen * scale,
            placement.y + (pen_row + 1 + capital_rows) * scale,
            colour
        );
        return;
    }
    if (tracking == 0) {
        draw_text(target, placement, fonts, role, shown, pen, pen_row, colour);
        return;
    }
    for (std::size_t at = 0; at < shown.size();) {
        const std::size_t bytes = character_bytes(shown.substr(at));
        if (bytes == 0)
            break;
        pen = draw_text(
                  target, placement, fonts, role, shown.substr(at, bytes), pen, pen_row, colour
              ) +
              tracking;
        at += bytes;
    }
}

Fonts load_game_fonts(oa::AssetStore& assets) {
    const std::vector<uint8_t> bytes = assets.read(game_palette_path).bytes;
    oa::PaletteBytes palette{};
    if (bytes.size() < palette.size())
        throw std::runtime_error("the game's palette is short: " + std::string(game_palette_path));
    std::copy_n(bytes.begin(), palette.size(), palette.begin());
    Fonts fonts;
    constexpr std::string_view regular_font = "anims/hattfont12.gaf";
    constexpr std::string_view small_font = "anims/hattfont11.gaf";
    fonts.regular = renderer::text_font(
        oa::ui::decoded::require(oa::formats::fnt::load_gaf(assets, regular_font), regular_font),
        palette
    );
    fonts.small = renderer::text_font(
        oa::ui::decoded::require(oa::formats::fnt::load_gaf(assets, small_font), small_font),
        palette
    );
    fonts.regular_characters = renderer::fnt_font_characters(fonts.regular.font);
    fonts.small_characters = renderer::fnt_font_characters(fonts.small.font);
    return fonts;
}

#if !defined(NDEBUG)

namespace {

/// One text's ink in the units of the draw that made it.
struct FitInk {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
};

/// The records and the ink of the paint in progress.
struct FitState {
    std::string screen{};
    std::vector<TextFitRecord> records{};
    std::vector<FitInk> ink{};
    /// Notes drawn into a buffer that is copied onto the screen, shifted so
    /// they are not compared with the screen's own text.
    int32_t space{};
};

FitState& fit_state() {
    static FitState state;
    return state;
}

/// Tells whether two inks cross, edges that only meet counting as clear.
bool ink_crosses(const FitInk& left, const FitInk& right) noexcept {
    return left.x < right.x + right.width && right.x < left.x + left.width &&
           left.y < right.y + right.height && right.y < left.y + left.height;
}

/// Keeps a piece of ink that still has area.
void keep_ink(std::vector<FitInk>& kept, int32_t x, int32_t y, int32_t width, int32_t height) {
    if (width > 0 && height > 0)
        kept.push_back(FitInk{x, y, width, height});
}

} // namespace

void set_text_fit_screen(std::string_view screen) {
    fit_state().screen = std::string(screen);
}

void begin_text_fit_frame() {
    FitState& state = fit_state();
    state.records.clear();
    state.ink.clear();
    state.space = 0;
}

void begin_text_fit_space() {
    fit_state().space += 100000;
}

void end_text_fit_space() {
    FitState& state = fit_state();
    state.space = std::max(state.space - 100000, int32_t{0});
}

void text_fit_begin_paint() {
    fit_state().ink.clear();
}

void note_text_fit(
    std::string_view text,
    int32_t text_width,
    int32_t box_x,
    int32_t box_y,
    int32_t box_width,
    int32_t box_height,
    int32_t ink_x,
    int32_t clip_x,
    int32_t clip_y,
    int32_t clip_width,
    int32_t clip_height
) {
    if (text.empty())
        return;
    FitState& state = fit_state();
    const int32_t space = state.space;
    FitInk ink{
        ink_x + space, box_y, std::max(text_width, int32_t{0}), std::max(box_height, int32_t{0})
    };
    // A negative clip hides the ink. A positive one keeps the part inside it.
    if (clip_width < 0 || clip_height < 0) {
        ink.width = 0;
        ink.height = 0;
    } else if (clip_width > 0 && clip_height > 0 && ink.width > 0 && ink.height > 0) {
        const int32_t left = std::max(ink.x, clip_x + space);
        const int32_t top = std::max(ink.y, clip_y);
        const int32_t right = std::min(ink.x + ink.width, clip_x + space + clip_width);
        const int32_t bottom = std::min(ink.y + ink.height, clip_y + clip_height);
        ink.x = left;
        ink.y = top;
        ink.width = std::max(right - left, int32_t{0});
        ink.height = std::max(bottom - top, int32_t{0});
    }
    bool across = false;
    if (ink.width > 0 && ink.height > 0) {
        for (const FitInk& earlier : state.ink) {
            if (ink_crosses(ink, earlier)) {
                across = true;
                break;
            }
        }
        state.ink.push_back(ink);
    }
    const bool wider = box_width < text_width;
    if (!wider && !across)
        return;
    TextFitRecord record;
    record.screen = state.screen;
    record.text = std::string(text);
    record.text_width = text_width;
    record.box_x = box_x;
    record.box_y = box_y;
    record.box_width = box_width;
    record.wider = wider;
    record.across = across;
    state.records.push_back(std::move(record));
}

void cover_text_fit(int32_t x, int32_t y, int32_t width, int32_t height) {
    if (width <= 0 || height <= 0)
        return;
    x += fit_state().space;
    const int32_t cover_right = x + width;
    const int32_t cover_bottom = y + height;
    FitState& state = fit_state();
    std::vector<FitInk> kept;
    kept.reserve(state.ink.size());
    for (const FitInk& ink : state.ink) {
        const int32_t ink_right = ink.x + ink.width;
        const int32_t ink_bottom = ink.y + ink.height;
        if (ink.x >= cover_right || x >= ink_right || ink.y >= cover_bottom || y >= ink_bottom) {
            kept.push_back(ink);
            continue;
        }
        keep_ink(kept, ink.x, ink.y, ink.width, y - ink.y);
        keep_ink(kept, ink.x, cover_bottom, ink.width, ink_bottom - cover_bottom);
        const int32_t band_top = std::max(ink.y, y);
        const int32_t band_bottom = std::min(ink_bottom, cover_bottom);
        const int32_t band_height = band_bottom - band_top;
        keep_ink(kept, ink.x, band_top, x - ink.x, band_height);
        keep_ink(kept, cover_right, band_top, ink_right - cover_right, band_height);
    }
    state.ink = std::move(kept);
}

const std::vector<TextFitRecord>& text_fit_records() {
    return fit_state().records;
}

#else

void set_text_fit_screen(std::string_view) {
}

void begin_text_fit_frame() {
}

void begin_text_fit_space() {
}

void end_text_fit_space() {
}

void text_fit_begin_paint() {
}

void note_text_fit(
    std::string_view,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t,
    int32_t
) {
}

void cover_text_fit(int32_t, int32_t, int32_t, int32_t) {
}

const std::vector<TextFitRecord>& text_fit_records() {
    static const std::vector<TextFitRecord> none;
    return none;
}

#endif

} // namespace oa::ui::kit
