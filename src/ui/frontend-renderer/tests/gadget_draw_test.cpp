// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/frontend_renderer/gadget_draw.hpp"
#include "oa/ui/frontend_renderer/game_text.hpp"

#include "oa/present/display.hpp"
#include "oa/present/palette_tables.hpp"
#include "oa/present/rle.hpp"
#include "oa/present/surface.hpp"
#include "oa/present/game_text.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace {

int failures = 0;

void require(bool condition, const char* message) {
    if (!condition) {
        std::cerr << message << '\n';
        ++failures;
    }
}

uint8_t at(const oa::present::SurfaceBuffer& buffer, int x, int y) {
    return buffer.pixels[static_cast<std::size_t>(y * buffer.surface.pitch + x)];
}

std::string rows(const oa::present::SurfaceBuffer& buffer) {
    std::string out;
    for (int y = 0; y < buffer.surface.height; ++y) {
        for (int x = 0; x < buffer.surface.width; ++x)
            out += static_cast<char>('0' + at(buffer, x, y));
        out += '\n';
    }
    return out;
}

// A GAF font whose first sequence has glyphs 'A' (2x2 row-RLE, key 0,
// hotspot (0,1)), ' ' (3 wide) and 'B' (1x1 raw).
struct TestFont {
    oa::present::GafSprites gaf;
    std::vector<uint8_t> a_pixels{5, 0, 5, 6};
    std::vector<uint8_t> a_stream;
    std::vector<uint8_t> b_pixels{7};
};

void build_font(TestFont& font) {
    constexpr int glyph_count = 128;
    oa::Sprite a{};
    a.width = 2;
    a.height = 2;
    a.origin_y = 1;
    a.data = font.a_pixels.data();
    oa::present::RleEncoder encoder;
    font.a_stream.resize(
        static_cast<std::size_t>(oa::present::encode_rle_sprite(encoder, nullptr, a))
    );
    oa::present::encode_rle_sprite(encoder, font.a_stream.data(), a);
    a.encoding = OA_SPRITE_ROW_RLE;
    a.data = font.a_stream.data();
    oa::Sprite space{};
    space.width = 3;
    space.height = 1;
    oa::Sprite b{};
    b.width = 1;
    b.height = 1;
    b.key = 0xFF;
    b.data = font.b_pixels.data();
    font.gaf.sprites = {a, space, b};
    font.gaf.slots.assign(glyph_count, oa::present::GafFrameSlot{});
    font.gaf.slots['A'].frame = &font.gaf.sprites[0];
    font.gaf.slots[' '].frame = &font.gaf.sprites[1];
    font.gaf.slots['B'].frame = &font.gaf.sprites[2];
    oa::present::GafSequence glyphs{};
    glyphs.frame_count = glyph_count;
    glyphs.frames = font.gaf.slots.data();
    font.gaf.sequences = {glyphs};
}

void put_le(std::vector<uint8_t>& bytes, std::size_t at, uint32_t value, std::size_t size) {
    for (std::size_t i = 0; i < size; ++i)
        bytes[at + i] = static_cast<uint8_t>(value >> (8 * i));
}

// One sequence of glyphs 0..'I', each a raw 1x1 frame with hotspot y 3,
// except 'I', which is 1x5.
std::vector<uint8_t> font_file() {
    constexpr std::size_t glyphs = 'I' + 1;
    constexpr std::size_t sequence = 0x10;
    constexpr std::size_t slots = sequence + 0x28;
    constexpr std::size_t headers = slots + glyphs * 8;
    constexpr std::size_t pixels = headers + glyphs * 0x18;
    std::vector<uint8_t> bytes(pixels + glyphs * 5, 0);
    put_le(bytes, 0, 0x00010100, 4);
    put_le(bytes, 4, 1, 4);
    put_le(bytes, 0x0C, sequence, 4);
    put_le(bytes, sequence, glyphs, 2);
    for (std::size_t glyph = 0; glyph < glyphs; ++glyph) {
        const std::size_t header = headers + glyph * 0x18;
        put_le(bytes, slots + glyph * 8, static_cast<uint32_t>(header), 4);
        put_le(bytes, header, 1, 2);
        put_le(bytes, header + 2, glyph == 'I' ? 5 : 1, 2);
        put_le(bytes, header + 6, 3, 2);
        put_le(bytes, header + 0x10, static_cast<uint32_t>(pixels + glyph * 5), 4);
    }
    return bytes;
}

void test_gui_font() {
    namespace draw = oa::ui::frontend_renderer;
    const auto file = font_file();
    oa::present::GafSprites font;
    require(draw::load_gui_font(file, font) == oa::present::GafStatus::ok, "gui font: relocates");
    const auto* glyphs = &font.sequences.front();
    require(
        oa::present::gaf_frame(glyphs, 'A')->origin_y == -2 &&
            oa::present::gaf_frame(glyphs, 'I')->origin_y == -2 &&
            oa::present::gaf_frame(glyphs, 0)->origin_y == -2,
        "gui font: every hotspot moves up by the height of 'I'"
    );
    oa::present::GafSprites broken;
    require(
        draw::load_gui_font(std::span(file).first(0x20), broken) != oa::present::GafStatus::ok,
        "gui font: a truncated file is rejected"
    );
}

// The characters a GUI font draws are kept by the address of its glyphs.
// Once they are forgotten, as a switch of mod forgets them, a font loaded at
// the same address is read afresh.
void test_forget_gui_font_characters() {
    namespace draw = oa::ui::frontend_renderer;
    constexpr char32_t e_acute = 0x00E9;
    constexpr uint8_t dos_e_acute = 0x82;
    constexpr uint16_t glyph_count = 256;
    TestFont font;
    build_font(font);
    font.gaf.slots.resize(glyph_count);
    font.gaf.sequences.front().frames = font.gaf.slots.data();
    font.gaf.sequences.front().frame_count = glyph_count;
    font.gaf.slots[dos_e_acute].frame = &font.gaf.sprites[2];
    require(
        draw::gui_font_characters(font.gaf).byte_for(e_acute) == dos_e_acute,
        "gui font characters: the DOS code page's letter is drawn by its glyph"
    );
    font.gaf.slots[dos_e_acute].frame = nullptr;
    draw::forget_gui_font_characters();
    require(
        !draw::gui_font_characters(font.gaf).byte_for(e_acute),
        "gui font characters: once forgotten, a font at the same address is read afresh"
    );
}

// A GUI font read as an FNT font, as the 3.1c screens read hattfont12 and
// hattfont11, holds its box, the picture of glyph 0, in the code page slots
// of letters it lacks. A caption with such a letter is drawn whole in the
// modern fonts, and a letter whose slot holds the box but which the font
// holds in the DOS code page's slot is drawn from there.
void test_gaf_backed_fnt_font_characters() {
    namespace draw = oa::ui::frontend_renderer;
    constexpr char32_t c_cedilla = 0x00C7;
    constexpr char32_t a_tilde_capital = 0x00C3;
    constexpr char32_t a_tilde = 0x00E3;
    constexpr char32_t o_tilde = 0x00F5;
    constexpr char32_t a_circumflex = 0x00E2;
    constexpr uint8_t dos_c_cedilla = 0x80;
    // A Latin-1 letter's slot in the game's code page is its own number.
    const auto slot = [](char32_t letter) { return static_cast<std::size_t>(letter); };
    // A box with its last pixel clear, and a letter of another picture.
    const oa::formats::fnt::Glyph box{2, 2, 0, -1, {6, 6, 6, 0}, {1, 1, 1, 0}};
    const oa::formats::fnt::Glyph letter{2, 2, 0, -1, {7, 0, 7, 7}, {1, 0, 1, 1}};
    oa::formats::fnt::Font font;
    font.gaf_backed = true;
    font.glyphs[0] = box;
    for (const char32_t lacked : {c_cedilla, a_tilde, o_tilde})
        font.glyphs[slot(lacked)] = box;
    // The box again, with another value under its clear pixel.
    font.glyphs[slot(a_tilde_capital)] = box;
    font.glyphs[slot(a_tilde_capital)]->pixels[3] = 9;
    font.glyphs[dos_c_cedilla] = letter;
    font.glyphs[slot(a_circumflex)] = letter;
    const auto characters = draw::fnt_font_characters(font);
    require(
        characters.byte_for(c_cedilla) == dos_c_cedilla,
        "gaf-backed font characters: Ç is drawn from the DOS slot, not its box"
    );
    require(
        !characters.byte_for(a_tilde) && !characters.byte_for(o_tilde),
        "gaf-backed font characters: a letter whose slot holds the box is not the font's"
    );
    require(
        !characters.byte_for(a_tilde_capital),
        "gaf-backed font characters: what lies under the box's clear pixels does not matter"
    );
    require(
        characters.byte_for(a_circumflex) == slot(a_circumflex),
        "gaf-backed font characters: a letter of its own picture is drawn by its glyph"
    );
    const std::string_view options = "Op\xC3\xA7\xC3\xB5"
                                     "es"; // Opções
    const auto runs = oa::present::split_text(options, characters);
    require(
        runs.size() == 1 && runs.front().modern && runs.front().text == options,
        "gaf-backed font characters: a caption with a letter the font lacks is drawn whole in "
        "the modern fonts"
    );
    font.gaf_backed = false;
    require(
        draw::fnt_font_characters(font).byte_for(o_tilde) == slot(o_tilde),
        "fnt font characters: an FNT font's glyph is its letter, whatever it shows"
    );
}

void test_gadget_text() {
    namespace draw = oa::ui::frontend_renderer;
    TestFont font;
    build_font(font);
    auto buffer = oa::present::create_surface(12, 3);
    std::fill(buffer.pixels.begin(), buffer.pixels.end(), uint8_t{9});
    draw::draw_gadget_text(
        &buffer.surface,
        &font.gaf,
        "A B\x01"
        "AC",
        1,
        2,
        draw::gadget_text_unbounded,
        0
    );
    // Glyphs sit at the pen less their hotspot; the space advances 3 without
    // drawing, the control byte and the glyphless 'C' are skipped.
    require(
        at(buffer, 1, 1) == 5 && at(buffer, 2, 1) == 9 && at(buffer, 1, 2) == 5 &&
            at(buffer, 2, 2) == 6,
        "gadget text: 'A' at the pen less its hotspot, key pixels skipped"
    );
    require(at(buffer, 3, 1) == 9 && at(buffer, 5, 1) == 9, "gadget text: the space draws nothing");
    require(at(buffer, 6, 2) == 7, "gadget text: 'B' after the space's advance");
    require(
        at(buffer, 7, 1) == 5 && at(buffer, 8, 2) == 6, "gadget text: the control byte is skipped"
    );
    require(
        at(buffer, 9, 1) == 9 && at(buffer, 9, 2) == 9,
        "gadget text: a byte without a glyph draws nothing"
    );

    // A width budget stops before the first glyph wider than what is left.
    std::fill(buffer.pixels.begin(), buffer.pixels.end(), uint8_t{9});
    draw::draw_gadget_text(&buffer.surface, &font.gaf, "A B", 0, 1, 5, 0);
    require(at(buffer, 0, 0) == 5 && at(buffer, 5, 1) == 9, "gadget text: the budget stops at 'B'");

    // A light level draws through that row of the light table.
    oa::present::DisplayContext display{};
    display.flags = oa::present::display_flag_light_table;
    oa::present::load_light_table(display);
    for (int row = 0; row < oa::present::ramp_table_rows; ++row)
        for (int index = 0; index < 256; ++index)
            display.light_table[row * 256 + index] = static_cast<uint8_t>(index + row);
    oa::present::bind_display(&display);
    std::fill(buffer.pixels.begin(), buffer.pixels.end(), uint8_t{9});
    draw::draw_gadget_text(&buffer.surface, &font.gaf, "A", 0, 1, draw::gadget_text_unbounded, 30);
    require(
        at(buffer, 0, 0) == 35 && at(buffer, 1, 0) == 9 && at(buffer, 1, 1) == 36,
        "gadget text: a light level remaps the glyph through its row"
    );
    oa::present::bind_display(nullptr);
    oa::present::free_light_table(display);
}

// What the game-text hooks answer: the settings, and whether a line is drawn.
struct GameTextAnswers {
    oa::present::TextSettings settings{};
    bool draws{true};
    std::vector<uint8_t> palette{};
    int32_t size{}; ///< the size the last line was drawn at, in percent
};

/// A line of solid characters, 3 pixels wide and 2 rows above the baseline.
std::shared_ptr<const oa::present::TextMask> solid_line(std::string_view text) {
    auto mask = std::make_shared<oa::present::TextMask>();
    int32_t pen = 0;
    for (std::size_t at = 0; at < text.size();) {
        const auto sequence = oa::present::utf8_sequence(text.substr(at));
        at += sequence.bytes != 0 ? sequence.bytes : 1;
        pen += 3;
        mask->character_ends.push_back(pen);
    }
    mask->width = pen;
    mask->height = 2;
    mask->baseline = 2;
    mask->advance = pen;
    mask->alpha.assign(static_cast<std::size_t>(pen) * 2U, 255);
    return mask;
}

void test_gadget_text_game_runs() {
    namespace draw = oa::ui::frontend_renderer;
    TestFont font;
    build_font(font);
    GameTextAnswers answers;
    answers.settings.style = {false, false, false, false, oa::present::default_text_size};
    answers.settings.utf8 = true;
    // Entry 4 is hattfont12's colour, which the modern runs are drawn in.
    answers.palette.assign(8 * 4, 0);
    answers.palette[4 * 4] = 195;
    answers.palette[4 * 4 + 1] = 195;
    answers.palette[4 * 4 + 2] = 155;
    oa::present::GameTextHooks hooks{};
    hooks.context = &answers;
    hooks.settings = [](void* context) { return static_cast<GameTextAnswers*>(context)->settings; };
    hooks.draw = [](void* context,
                    std::string_view text,
                    oa::present::TextFace,
                    int32_t,
                    int32_t size) -> std::shared_ptr<const oa::present::TextMask> {
        auto& answers = *static_cast<GameTextAnswers*>(context);
        answers.size = size;
        return answers.draws ? solid_line(text) : nullptr;
    };
    hooks.palette = [](void* context) -> std::span<const uint8_t> {
        return static_cast<GameTextAnswers*>(context)->palette;
    };
    oa::present::set_game_text_hooks(hooks);
    const std::string sun = "\xE6\x97\xA5";
    const std::string text = "A" + sun + "B";
    auto buffer = oa::present::create_surface(12, 4);
    std::fill(buffer.pixels.begin(), buffer.pixels.end(), uint8_t{9});
    draw::draw_gadget_text(
        &buffer.surface, &font.gaf, text.c_str(), 1, 2, draw::gadget_text_unbounded, 0
    );
    // The font has no 'I': the baseline is the pen row, and the run stands
    // on it after 'A'.
    require(
        at(buffer, 3, 0) == 4 && at(buffer, 5, 1) == 4 && at(buffer, 3, 2) == 9,
        "gadget text: the font's missing character is drawn in the modern fonts after 'A'"
    );
    require(at(buffer, 6, 2) == 7, "gadget text: 'B' follows the run's width");
    require(
        draw::measure_gadget_text(&font.gaf, text.c_str()) == 2 + 3 + 1,
        "gadget text: the run measures as drawn"
    );
    // A width budget stops before a run wider than what is left.
    std::fill(buffer.pixels.begin(), buffer.pixels.end(), uint8_t{9});
    draw::draw_gadget_text(&buffer.surface, &font.gaf, text.c_str(), 1, 2, 4, 0);
    require(
        at(buffer, 3, 0) == 9 && at(buffer, 6, 2) == 9, "gadget text: the budget stops at the run"
    );
    // A run the modern fonts cannot draw is the code page's '?', which the
    // font has no glyph for.
    answers.draws = false;
    std::fill(buffer.pixels.begin(), buffer.pixels.end(), uint8_t{9});
    draw::draw_gadget_text(
        &buffer.surface, &font.gaf, text.c_str(), 1, 2, draw::gadget_text_unbounded, 0
    );
    require(
        at(buffer, 3, 0) == 9 && at(buffer, 3, 2) == 7,
        "gadget text: a run the modern fonts cannot draw takes no room"
    );
    // Game text is drawn whole in the modern fonts while the settings choose
    // them; interface text keeps the font.
    answers.draws = true;
    answers.settings.style.modern_fonts = true;
    std::fill(buffer.pixels.begin(), buffer.pixels.end(), uint8_t{9});
    draw::draw_gadget_text(
        &buffer.surface, &font.gaf, "AB", 1, 2, draw::gadget_text_unbounded, 0, true
    );
    require(
        at(buffer, 1, 0) == 4 && at(buffer, 6, 1) == 4 && at(buffer, 1, 2) == 9,
        "gadget text: game text is drawn in the modern fonts"
    );
    require(
        answers.size == oa::present::default_text_size,
        "gadget text: game text is drawn at the text size"
    );
    // Gadgets are laid out for the game's fonts: game text larger than them
    // is drawn at their size, on the same baseline; smaller is drawn smaller.
    answers.settings.style.size = oa::present::highest_text_size;
    std::fill(buffer.pixels.begin(), buffer.pixels.end(), uint8_t{9});
    draw::draw_gadget_text(
        &buffer.surface, &font.gaf, "AB", 1, 2, draw::gadget_text_unbounded, 0, true
    );
    require(
        answers.size == oa::present::game_font_text_size && at(buffer, 1, 0) == 4 &&
            at(buffer, 1, 2) == 9,
        "gadget text: game text keeps to the game fonts' size"
    );
    answers.settings.style.size = oa::present::lowest_text_size;
    draw::draw_gadget_text(
        &buffer.surface, &font.gaf, "AB", 1, 2, draw::gadget_text_unbounded, 0, true
    );
    require(
        answers.size == oa::present::lowest_text_size, "gadget text: smaller text is drawn smaller"
    );
    require(
        draw::screen_text_size({true, "AB", oa::present::highest_text_size}) ==
                oa::present::game_font_text_size &&
            draw::screen_text_size({true, "AB", oa::present::lowest_text_size}) ==
                oa::present::lowest_text_size,
        "gadget text: the screens hold text to the game fonts' size"
    );
    // A label's missing character is drawn at the font's own size, whatever
    // the setting.
    draw::draw_gadget_text(
        &buffer.surface, &font.gaf, text.c_str(), 1, 2, draw::gadget_text_unbounded, 0
    );
    require(
        answers.size == oa::present::game_font_text_size,
        "gadget text: a missing character keeps the font's size"
    );
    answers.settings.style.size = oa::present::default_text_size;
    std::fill(buffer.pixels.begin(), buffer.pixels.end(), uint8_t{9});
    draw::draw_gadget_text(&buffer.surface, &font.gaf, "AB", 1, 2, draw::gadget_text_unbounded, 0);
    require(at(buffer, 1, 2) == 5 && at(buffer, 3, 2) == 7, "gadget text: a label keeps the font");
    oa::present::set_game_text_hooks({});
}

/// A line of characters 3 pixels wide, each 2 rows above the baseline,
/// over a line one row deeper that the letters leave clear.
std::shared_ptr<const oa::present::TextMask> letters_over_a_clear_row(std::string_view text) {
    auto mask = std::make_shared<oa::present::TextMask>();
    int32_t pen = 0;
    for (std::size_t at = 0; at < text.size();) {
        const auto sequence = oa::present::utf8_sequence(text.substr(at));
        at += sequence.bytes != 0 ? sequence.bytes : 1;
        pen += 3;
        mask->character_ends.push_back(pen);
    }
    mask->width = pen;
    mask->height = 3;
    mask->baseline = 2;
    mask->advance = pen;
    mask->alpha.assign(static_cast<std::size_t>(pen) * 3U, 0);
    std::fill(mask->alpha.begin(), mask->alpha.begin() + pen * 2, uint8_t{255});
    return mask;
}

// The input method's composition after a line is drawn wholly in the modern
// fonts, its spelling's Latin letters among them, and underlined in its
// colour on the line's lowest row; as much of it as fits the clip is drawn.
void test_composition_is_underlined() {
    namespace draw = oa::ui::frontend_renderer;
    GameTextAnswers answers;
    answers.settings.style = {false, false, false, false, oa::present::default_text_size};
    answers.settings.utf8 = true;
    oa::present::GameTextHooks hooks{};
    hooks.context = &answers;
    hooks.settings = [](void* context) { return static_cast<GameTextAnswers*>(context)->settings; };
    hooks.draw = [](void*, std::string_view text, oa::present::TextFace, int32_t, int32_t)
        -> std::shared_ptr<const oa::present::TextMask> { return letters_over_a_clear_row(text); };
    oa::present::set_game_text_hooks(hooks);
    const std::array<uint8_t, 3> ink{200, 100, 50};
    oa::PaletteBytes palette{};
    std::copy(ink.begin(), ink.end(), palette.begin() + 4);
    // A font with no glyphs: its baseline is the pen row.
    const oa::formats::fnt::Font font{};
    draw::Surface surface;
    surface.width = 20;
    surface.height = 6;
    const auto pixel = [&surface](int32_t x, int32_t y) {
        const auto at = (static_cast<std::size_t>(y) * surface.width + x) * 3U;
        return std::array<uint8_t, 3>{surface.rgb[at], surface.rgb[at + 1], surface.rgb[at + 2]};
    };
    surface.rgb.assign(20U * 6U * 3U, 0);
    const int32_t pen =
        draw::draw_fnt_composition(surface, font, "ab", 2, 3, ink, palette, {0, 0, 19, 5});
    require(pen == 8, "composition: the pen moves past it");
    require(pixel(2, 1) == ink && pixel(7, 2) == ink, "composition: its letters are drawn");
    require(
        pixel(2, 3) == ink && pixel(7, 3) == ink && pixel(1, 3) != ink && pixel(8, 3) != ink,
        "composition: it is underlined under its letters on the line's lowest row"
    );
    surface.rgb.assign(20U * 6U * 3U, 0);
    require(
        draw::draw_fnt_composition(surface, font, "ab", 2, 3, ink, palette, {0, 0, 5, 5}) == 5,
        "composition: as much as fits is drawn"
    );
    require(pixel(4, 3) == ink && pixel(5, 3) != ink, "composition: its underline keeps to it");
    oa::present::set_game_text_hooks({});
}

// The rows the modern fonts draw on in a line, counted from its pen row on
// an FNT font's baseline: placed by them, the line keeps to the rows it is
// given. Text the font draws itself has none.
void test_fnt_game_text_modern_rows() {
    namespace draw = oa::ui::frontend_renderer;
    GameTextAnswers answers;
    answers.settings.style = {false, false, false, false, oa::present::default_text_size};
    answers.settings.utf8 = true;
    oa::present::GameTextHooks hooks{};
    hooks.context = &answers;
    hooks.settings = [](void* context) { return static_cast<GameTextAnswers*>(context)->settings; };
    hooks.draw = [](void*, std::string_view text, oa::present::TextFace, int32_t, int32_t)
        -> std::shared_ptr<const oa::present::TextMask> { return letters_over_a_clear_row(text); };
    oa::present::set_game_text_hooks(hooks);
    const std::array<uint8_t, 3> ink{200, 100, 50};
    oa::PaletteBytes palette{};
    std::copy(ink.begin(), ink.end(), palette.begin() + 4);
    // A font with no glyphs: its baseline is the pen row. The letters stand
    // on the two rows above it, and the clear row under them draws nothing.
    const oa::formats::fnt::Font font{};
    const std::string_view hanzi = "\xE4\xB8\xAD";
    const auto rows = draw::fnt_game_text_modern_rows(font, hanzi, true);
    require(
        rows.has_value() && rows->top == -2 && rows->bottom == -1,
        "modern rows: the two rows the letters stand on above the baseline"
    );
    require(
        !draw::fnt_game_text_modern_rows(font, "ab", false).has_value(),
        "modern rows: none for the font's own text"
    );
    require(!draw::fnt_game_text_modern_rows(font, "", true).has_value(), "modern rows: no text");
    draw::Surface surface;
    surface.width = 6;
    surface.height = 6;
    surface.rgb.assign(6U * 6U * 3U, 0);
    const auto row_inked = [&surface, &ink](int32_t y) {
        for (int32_t x = 0; x < 6; ++x) {
            const auto at = (static_cast<std::size_t>(y) * surface.width + x) * 3U;
            if (std::array<uint8_t, 3>{surface.rgb[at], surface.rgb[at + 1], surface.rgb[at + 2]} ==
                ink)
                return true;
        }
        return false;
    };
    const int32_t first_row = 1;
    std::ignore = draw::draw_fnt_game_text(
        surface,
        font,
        hanzi,
        1,
        first_row - (rows ? rows->top : 0),
        ink,
        palette,
        {0, 0, 5, 5},
        true
    );
    require(
        !row_inked(first_row - 1) && row_inked(first_row) && row_inked(first_row + 1) &&
            !row_inked(first_row + 2),
        "modern rows: placed by them, the letters keep to their rows"
    );
    oa::present::set_game_text_hooks({});
}

// A line whose letters stand on as many rows above the baseline as its
// size has whole quarters of the game font's (4 at 100%, 2 at 70%), with
// one row below it, each character 3 columns wide.
std::shared_ptr<const oa::present::TextMask> rows_by_size(std::string_view text, int32_t size) {
    auto mask = std::make_shared<oa::present::TextMask>();
    const int32_t above = size / 25;
    mask->width = mask->advance = static_cast<int32_t>(text.size()) * 3;
    mask->height = above + 1;
    mask->baseline = above;
    mask->alpha.assign(static_cast<std::size_t>(mask->width * mask->height), 255);
    return mask;
}

// A caption's modern letters keep to a box's rows: lowered by what they
// would draw above it, raised by what they would draw below it, and drawn
// smaller while they stand taller than it. The font's own caption stands
// on the box's top row at the font's size.
void test_fnt_caption_rows() {
    namespace draw = oa::ui::frontend_renderer;
    GameTextAnswers answers;
    answers.settings.style = {false, false, false, false, oa::present::default_text_size};
    answers.settings.utf8 = true;
    oa::present::GameTextHooks hooks{};
    hooks.context = &answers;
    hooks.settings = [](void* context) { return static_cast<GameTextAnswers*>(context)->settings; };
    hooks.draw = [](void*, std::string_view text, oa::present::TextFace, int32_t, int32_t size)
        -> std::shared_ptr<const oa::present::TextMask> { return rows_by_size(text, size); };
    oa::present::set_game_text_hooks(hooks);
    // A font with no glyphs: its baseline is the pen row.
    const oa::formats::fnt::Font font{};
    const std::string_view hanzi = "\xE4\xB8\xAD";
    // A character the font lacks is drawn at the font's own size, 100%:
    // 4 rows above the baseline and 1 below, rows -4 to 0 from the pen row.
    const auto rows = draw::fnt_game_text_modern_rows(font, hanzi, true);
    require(rows && rows->top == -4 && rows->bottom == 0, "caption rows: the letters' rows");
    const auto roomy = draw::fnt_caption_rows(font, hanzi, true, 10, 6);
    require(
        roomy.pen_row == 14 && roomy.most_size == oa::present::game_font_text_size,
        "caption rows: lowered by the rows they stand above the pen row"
    );
    const auto exact = draw::fnt_caption_rows(font, hanzi, true, 10, 5);
    require(exact.pen_row == 14, "caption rows: five rows fill a box of five");
    // In a box of three rows the letters are drawn at 70%, 2 rows above the
    // baseline and 1 below.
    const auto short_box = draw::fnt_caption_rows(font, hanzi, true, 10, 3);
    require(
        short_box.most_size == 70 && short_box.pen_row == 12,
        "caption rows: drawn smaller while taller than the box"
    );
    const auto own = draw::fnt_caption_rows(font, "ab", false, 10, 3);
    require(
        own.pen_row == 10 && own.most_size == oa::present::game_font_text_size,
        "caption rows: the font's own caption stands on the box's top row"
    );
    oa::present::set_game_text_hooks({});
}

} // namespace

int main() {
    namespace draw = oa::ui::frontend_renderer;
    {
        auto buffer = oa::present::create_surface(6, 6);
        draw::fill_box_raised(&buffer.surface, oa::Rect32{0, 0, 5, 5}, 1, 2, 3);
        const std::string expected = "111111\n"
                                     "111112\n"
                                     "113322\n"
                                     "113322\n"
                                     "112222\n"
                                     "122222\n";
        require(rows(buffer) == expected, "raised box: two-pixel bevel over the fill");
    }

    {
        auto buffer = oa::present::create_surface(6, 6);
        draw::fill_box_sunken(&buffer.surface, oa::Rect32{0, 0, 5, 5}, 1, 2, 3);
        require(
            at(buffer, 0, 0) == 2 && at(buffer, 5, 5) == 1 && at(buffer, 2, 2) == 3,
            "sunken box swaps the edge colours"
        );
    }
    {
        auto buffer = oa::present::create_surface(5, 5);
        draw::fill_frame_raised(&buffer.surface, oa::Rect32{0, 0, 4, 4}, 1, 2, 3);
        const std::string expected = "11111\n"
                                     "13332\n"
                                     "13332\n"
                                     "13332\n"
                                     "12222\n";
        require(rows(buffer) == expected, "raised frame: one-pixel edges");
        draw::fill_frame_sunken(&buffer.surface, oa::Rect32{0, 0, 4, 4}, 1, 2, 3);
        require(
            at(buffer, 0, 0) == 2 && at(buffer, 4, 4) == 1, "sunken frame swaps the edge colours"
        );
    }
    {
        auto buffer = oa::present::create_surface(4, 4);
        const oa::ui::gui_layout::GadgetRect rect{0, 0, 3, 3};
        draw::draw_value_marker(&buffer.surface, rect, 1, 7, 1);
        require(at(buffer, 3, 0) == 7 && at(buffer, 0, 3) == 0, "attribute 1 draws the top edge");
        draw::draw_value_marker(&buffer.surface, rect, 2, 5, 1);
        require(at(buffer, 0, 3) == 5, "attribute 2 draws the left edge");
        draw::draw_value_marker(&buffer.surface, rect, 4, 6, 0);
        require(at(buffer, 2, 2) == 0, "flag bit 0 clear draws nothing");
        draw::draw_value_marker(&buffer.surface, rect, 4, 6, 1);
        require(at(buffer, 2, 2) == 6, "attribute 4 draws the diagonal");
    }
    {
        // The key is found among whole characters: a byte of a Hangul
        // syllable, which takes three, is never the key.
        const std::string_view korean_ok = "\xED\x99\x95\xEC\x9D\xB8"; // 확인
        require(
            draw::quick_key_offset(korean_ok, '\xED') == std::string_view::npos,
            "quick key: a byte within a character is not the key"
        );
        require(
            draw::quick_key_offset("\xED\x99\x95\xEC\x9D\xB8 OK", 'O') == 7,
            "quick key: a letter after a syllable"
        );
        require(draw::quick_key_offset("Cancel", 'n') == 2, "quick key: an English caption's key");
        const std::string code_page_caption = std::string(1, '\xDC') + "ber";
        require(
            draw::quick_key_offset(code_page_caption, '\xDC') == 0,
            "quick key: a letter of the one-byte code page"
        );
        require(draw::quick_key_offset("OK", '\0') == std::string_view::npos, "quick key: none");
    }
    test_gadget_text();
    test_gadget_text_game_runs();
    test_composition_is_underlined();
    test_fnt_game_text_modern_rows();
    test_fnt_caption_rows();
    test_gui_font();
    test_forget_gui_font_characters();
    test_gaf_backed_fnt_font_characters();
    return failures == 0 ? 0 : 1;
}
