// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The game's two dialog fonts, the modern faces that draw what they lack,
// and the characters drawn in place of the ones those fonts do not hold.

#include "oa/ui/kit/text.hpp"

#include "oa/base/text/line_break.hpp"
#include "oa/formats/fnt.hpp"
#include "oa/ui/decoded.hpp"

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

/// Returns the characters a GUI font draws: every glyph that is not the
/// picture of glyph 0, the font's box for a missing character.
///
/// @param font the font
/// @return the characters
present::FontCharacters gui_characters(const oa::formats::fnt::Font& font) {
    const auto& box = font.glyphs[0];
    return present::FontCharacters::gui_font([&font, &box](uint8_t byte) {
        const auto& glyph = font.glyphs[byte];
        if (!glyph || glyph->width == 0 || glyph->height == 0)
            return false;
        return byte == 0 || !box || box->width != glyph->width || box->height != glyph->height ||
               box->pixels != glyph->pixels;
    });
}

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

std::string with_stand_ins(const Fonts& fonts, FontRole role, std::string_view text) {
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
    const std::string drawn = with_stand_ins(fonts, role, text);
    return width_of(face_of(fonts, role), drawn);
}

int32_t tracked_width(const Fonts& fonts, FontRole role, std::string_view text, int32_t tracking) {
    if (text.empty())
        return 0;
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
    // A game font's capitals start one row under the pen row. The ink is that
    // band, not the whole line box: a title's box and the line under it share
    // two rows, and their glyphs meet at the edge.
    const int32_t capital_rows = capitals(fonts, role);
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
    fonts.regular_characters = gui_characters(fonts.regular.font);
    fonts.small_characters = gui_characters(fonts.small.font);
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
