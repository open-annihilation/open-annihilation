// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Game text: UTF-8 sequences and the erase key's character, the game's code
// page read and written, which bytes a game font draws characters with, the
// runs a line splits into, the borders of modern text and how a line is
// laid on 8-bit and RGB pixels, and over pixels a picture beneath shows
// through; the text size: its range, the lengths,
// pixel sizes, borders and the message log's line steps at a few sizes,
// and a line broken into rows or showing its end.

#include "oa/present/game_text.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <string>
#include <tuple>
#include <vector>

namespace {

namespace present = oa::present;

/// U+00E9, U+65E5 and U+1F600 in UTF-8.
constexpr std::string_view e_acute = "\xC3\xA9";
constexpr std::string_view sun = "\xE6\x97\xA5";
constexpr std::string_view grin = "\xF0\x9F\x98\x80";

void reads_utf8_sequences() {
    OA_CHECK(present::utf8_sequence(sun).bytes == 3);
    OA_CHECK(present::utf8_sequence(sun).code_point == 0x65E5);
    OA_CHECK(present::utf8_sequence(grin).bytes == 4);
    OA_CHECK(present::utf8_sequence(e_acute).code_point == 0xE9);
    // ASCII, a lone 0xE9, a truncated sequence, an overlong 'A', a
    // surrogate and a code point past U+10FFFF start no sequence.
    OA_CHECK(present::utf8_sequence("a").bytes == 0);
    OA_CHECK(present::utf8_sequence("\xE9t").bytes == 0);
    OA_CHECK(present::utf8_sequence("\xE6\x97").bytes == 0);
    OA_CHECK(present::utf8_sequence("\xC1\x81").bytes == 0);
    OA_CHECK(present::utf8_sequence("\xED\xA0\x80").bytes == 0);
    OA_CHECK(present::utf8_sequence("\xF4\x90\x80\x80").bytes == 0);
    OA_CHECK(present::utf8_sequence("").bytes == 0);
}

void erases_whole_characters() {
    OA_CHECK(present::last_character_start("") == 0);
    OA_CHECK(present::last_character_start("ab") == 1);
    OA_CHECK(present::last_character_start(std::string("a") + std::string(sun)) == 1);
    OA_CHECK(present::last_character_start(std::string("a") + std::string(grin)) == 1);
    OA_CHECK(present::last_character_start(std::string(e_acute)) == 0);
    // A truncated sequence goes a byte at a time.
    OA_CHECK(present::last_character_start("a\xE6\x97") == 2);
}

void reads_and_writes_the_code_page() {
    OA_CHECK(present::code_page_character('A') == U'A');
    OA_CHECK(present::code_page_character(0xE9) == 0xE9);
    OA_CHECK(present::code_page_character(0x80) == 0x20AC);
    OA_CHECK(present::code_page_character(0x92) == 0x2019);
    OA_CHECK(present::code_page_character(0x81) == 0x81);
    OA_CHECK(present::code_page_byte(0x20AC) == uint8_t{0x80});
    OA_CHECK(present::code_page_byte(0xE9) == uint8_t{0xE9});
    OA_CHECK(present::code_page_byte(0x65E5) == std::nullopt);
    // Without UTF-8 every high byte is a code-page character.
    OA_CHECK(
        present::decode_game_text("caf\xE9", false) == std::string("caf") + std::string(e_acute)
    );
    OA_CHECK(present::decode_game_text("\xC3\xA9", false) == std::string("\xC3\x83\xC2\xA9"));
    // With it a well-formed sequence is its character and a stray byte is
    // still read in the code page.
    OA_CHECK(present::decode_game_text("\xC3\xA9", true) == e_acute);
    OA_CHECK(
        present::decode_game_text("\xE9 \xE6\x97\xA5", true) ==
        std::string(e_acute) + " " + std::string(sun)
    );
    OA_CHECK(present::decode_game_text("\x80", true) == "\xE2\x82\xAC");
    // Writing keeps UTF-8 when it may, else the code page with '?' for
    // what it lacks and for bytes that are not UTF-8.
    const std::string typed = std::string("caf") + std::string(e_acute) + std::string(sun) + "!";
    OA_CHECK(present::encode_game_text(typed, true) == typed);
    OA_CHECK(present::encode_game_text(typed, false) == "caf\xE9?!");
    OA_CHECK(present::encode_game_text("\xE2\x82\xAC\xFF", false) == "\x80?");
    OA_CHECK(present::encode_game_text("", false).empty());
    std::string appended;
    present::append_utf8(appended, 0x1F600);
    present::append_utf8(appended, 0xD800);
    present::append_utf8(appended, 0x110000);
    OA_CHECK(appended == grin);
}

void maps_characters_to_font_bytes() {
    const auto every = [](uint8_t) { return true; };
    const auto gui = present::FontCharacters::gui_font(every);
    OA_CHECK(gui.byte_for(U'A') == uint8_t{'A'});
    OA_CHECK(gui.byte_for(U' ') == uint8_t{' '});
    OA_CHECK(gui.byte_for(0x1F) == std::nullopt);
    // The game's code-page slot comes first, then the DOS letters.
    OA_CHECK(gui.byte_for(0xE9) == uint8_t{0xE9});
    OA_CHECK(gui.byte_for(0xC7) == uint8_t{0xC7});
    OA_CHECK(gui.byte_for(0xC1) == uint8_t{0xC1});
    OA_CHECK(gui.byte_for(0xB0) == uint8_t{0xB0});
    OA_CHECK(gui.byte_for(0xBF) == uint8_t{0xBF});
    OA_CHECK(gui.byte_for(0x0192) == uint8_t{0x9F});
    // Missing: the inverted exclamation mark, ý, and anything past the
    // DOS letters.
    OA_CHECK(gui.byte_for(0xA1) == std::nullopt);
    OA_CHECK(gui.byte_for(0xFD) == std::nullopt);
    OA_CHECK(gui.byte_for(0x65E5) == std::nullopt);
    // A slot holding the box is not a character.
    const auto boxes = present::FontCharacters::gui_font([](uint8_t byte) {
        return byte != 0xE9 && byte != 0xC7;
    });
    OA_CHECK(boxes.byte_for(0xE9) == uint8_t{0x82});
    OA_CHECK(boxes.byte_for(0xC7) == uint8_t{0x80});
    const auto fnt = present::FontCharacters::fnt_font(every);
    OA_CHECK(fnt.byte_for(0xA1) == uint8_t{0xA1});
    OA_CHECK(fnt.byte_for(0xFE) == uint8_t{0xFE});
    OA_CHECK(fnt.byte_for(0xFF) == std::nullopt);
    OA_CHECK(fnt.byte_for(0x2019) == uint8_t{0x92});
    OA_CHECK(fnt.byte_for(0x20AC) == std::nullopt);
    const auto ascii = present::FontCharacters::fnt_font([](uint8_t) { return false; });
    OA_CHECK(ascii.byte_for(0xE9) == std::nullopt && ascii.byte_for(U'z') == uint8_t{'z'});
}

void splits_runs_between_fonts() {
    const auto gui = present::FontCharacters::gui_font([](uint8_t) { return true; });
    const std::string line =
        std::string("caf") + std::string(e_acute) + " " + std::string(sun) + std::string(sun) + "!";
    const auto runs = present::split_text(line, gui);
    OA_CHECK(runs.size() == 3);
    OA_CHECK(!runs[0].modern && runs[0].text == "caf\xE9 ");
    OA_CHECK(runs[1].modern && runs[1].text == std::string(sun) + std::string(sun));
    OA_CHECK(!runs[2].modern && runs[2].text == "!");
    // The characters a game font lacks are drawn at its own size.
    OA_CHECK(runs[1].size == present::game_font_text_size);
    OA_CHECK(present::TextRun{}.size == present::game_font_text_size);
    OA_CHECK(present::split_text("", gui).empty());
}

/// Returns the runs of a text as "game:" or "modern:" and their text.
///
/// @param runs the runs
/// @return one line per run
std::vector<std::string> run_lines(const std::vector<present::TextRun>& runs) {
    std::vector<std::string> lines;
    for (const present::TextRun& run : runs)
        lines.push_back((run.modern ? "modern:" : "game:") + run.text);
    return lines;
}

void keeps_a_word_in_one_face() {
    // A GUI font of ASCII and the code page's letters, as hattfont12 is.
    const auto gui = present::FontCharacters::gui_font([](uint8_t) { return true; });
    using Lines = std::vector<std::string>;
    // A text with letters the font lacks is drawn whole in the modern fonts,
    // the hyphen in a word and the Latin words and digits beside it with it.
    OA_CHECK(run_lines(present::split_text("будь-якого", gui)) == Lines({"modern:будь-якого"}));
    OA_CHECK(
        run_lines(present::split_text("Набори тайлів Cavedog (16)", gui)) ==
        Lines({"modern:Набори тайлів Cavedog (16)"})
    );
    OA_CHECK(run_lines(present::split_text("Mapa1 \u0103", gui)) == Lines({"modern:Mapa1 \u0103"}));
    // English, and the letters the font has, keep the game font.
    OA_CHECK(
        run_lines(present::split_text("Copy again from... caf\u00e9", gui)) ==
        Lines({"game:Copy again from... caf\xE9"})
    );
    // Beside a symbol the font lacks, or a Chinese character, the rest keeps
    // the game font, as before.
    OA_CHECK(
        run_lines(present::split_text("16\u219216", gui)) ==
        Lines({"game:16", "modern:\u2192", "game:16"})
    );
    OA_CHECK(
        run_lines(present::split_text("共16张", gui)) ==
        Lines({"modern:共", "game:16", "modern:张"})
    );
}

void same_glyph_compares_pictures() {
    std::array<uint8_t, 4> one{1, 2, 3, 4};
    std::array<uint8_t, 4> other{1, 2, 3, 5};
    oa::Sprite first{};
    first.width = 2;
    first.height = 2;
    first.encoding = OA_SPRITE_RAW;
    first.key = 0xEE;
    first.data = one.data();
    oa::Sprite second = first;
    OA_CHECK(present::same_glyph(&first, &second));
    second.data = other.data();
    OA_CHECK(!present::same_glyph(&first, &second));
    second.data = one.data();
    second.origin_y = 1;
    OA_CHECK(!present::same_glyph(&first, &second));
    OA_CHECK(!present::same_glyph(nullptr, &first));
}

/// A mask of one pixel with a baseline two rows below it.
present::TextMask dot() {
    present::TextMask mask;
    mask.width = 1;
    mask.height = 3;
    mask.baseline = 2;
    mask.advance = 2;
    mask.alpha = {0, 255, 0};
    mask.character_ends = {2};
    return mask;
}

void builds_borders() {
    present::TextStyle bare{true, false, false, false, present::default_text_size};
    const auto plain = present::build_text_layers(dot(), bare, 1);
    OA_CHECK(plain.width == 1 && plain.height == 3 && plain.advance == 2);
    OA_CHECK(std::count(plain.outline.begin(), plain.outline.end(), 1) == 0);

    present::TextStyle outlined{true, true, false, false, present::default_text_size};
    const auto ring = present::build_text_layers(dot(), outlined, 1);
    OA_CHECK(ring.width == 3 && ring.height == 5 && ring.baseline == 3 && ring.pen == 1);
    OA_CHECK(ring.advance == 3);
    OA_CHECK(ring.fill[2 * 3 + 1] == 255);
    // All eight neighbours, and not the letter's own pixel.
    OA_CHECK(std::count(ring.outline.begin(), ring.outline.end(), 1) == 8);
    OA_CHECK(ring.outline[2 * 3 + 1] == 0);

    present::TextStyle both{true, true, true, true, present::default_text_size};
    const auto shaded = present::build_text_layers(dot(), both, 1);
    OA_CHECK(shaded.width == 4 && shaded.height == 6 && shaded.background);
    // The shadow is the outlined dot moved one down and right.
    OA_CHECK(std::count(shaded.shadow.begin(), shaded.shadow.end(), 1) == 9);
    OA_CHECK(shaded.shadow[2 * 4 + 1] == 1 && shaded.shadow[1 * 4 + 1] == 0);
    OA_CHECK(shaded.shadow[4 * 4 + 3] == 1);

    // Twice the scale: a two-pixel ring.
    const auto thick = present::build_text_layers(dot(), outlined, 2);
    OA_CHECK(thick.width == 5 && thick.advance == 4);
    OA_CHECK(std::count(thick.outline.begin(), thick.outline.end(), 1) == 24);
}

/// Black, the outline grey, mid grey, white and a red, in four-byte entries.
const std::vector<uint8_t> test_palette{0,   0, 0,   0,   43,  43, 43,  0, 128, 128,
                                        128, 0, 255, 255, 255, 0,  200, 0, 0,   0};

void lays_text_on_pixels() {
    present::TextStyle outlined{true, true, false, false, present::default_text_size};
    const auto ring = present::build_text_layers(dot(), outlined, 1);
    // Over white, the outline is grey and the letter red.
    auto buffer = present::create_surface(5, 6);
    std::fill(buffer.pixels.begin(), buffer.pixels.end(), uint8_t{3});
    auto canvas = present::indexed_canvas(buffer.surface, test_palette);
    present::lay_text(canvas, ring, 1, 4, {200, 0, 0});
    const auto pixel = [&](int32_t x, int32_t y) {
        return buffer.pixels[static_cast<std::size_t>(y * buffer.surface.pitch + x)];
    };
    OA_CHECK(pixel(1, 3) == 4);
    OA_CHECK(pixel(0, 2) == 1 && pixel(2, 4) == 1 && pixel(3, 3) == 3);
    // Over black, the outline is black.
    std::fill(buffer.pixels.begin(), buffer.pixels.end(), uint8_t{0});
    present::lay_text(canvas, ring, 1, 4, {200, 0, 0});
    OA_CHECK(pixel(1, 3) == 4 && pixel(0, 2) == 0);
    // The clip keeps what is outside it.
    std::fill(buffer.pixels.begin(), buffer.pixels.end(), uint8_t{3});
    canvas.clip_right = 0;
    present::lay_text(canvas, ring, 1, 4, {200, 0, 0});
    OA_CHECK(pixel(0, 2) == 1 && pixel(1, 3) == 3);

    // A shadow over white is half black, mid grey here; the background
    // box is black.
    present::TextStyle shadowed{true, false, true, false, present::default_text_size};
    const auto shadow = present::build_text_layers(dot(), shadowed, 1);
    std::fill(buffer.pixels.begin(), buffer.pixels.end(), uint8_t{3});
    canvas = present::indexed_canvas(buffer.surface, test_palette);
    present::lay_text(canvas, shadow, 0, 2, {255, 255, 255});
    OA_CHECK(pixel(0, 1) == 3 && pixel(1, 2) == 2 && pixel(0, 2) == 3);
    // The rows anything is drawn on: the letter's and its shadow's, with or
    // without a box; none for a line with nothing drawn.
    OA_CHECK(shadow.drawn_top == 1 && shadow.drawn_bottom == 2);
    present::TextMask blank;
    blank.width = 2;
    blank.height = 3;
    blank.baseline = 2;
    blank.alpha.assign(6, 0);
    const auto nothing = present::build_text_layers(blank, shadowed, 1);
    OA_CHECK(nothing.drawn_top == -1 && nothing.drawn_bottom == -1);
    // The box spans the rows the letter and its shadow take, across the line.
    present::TextStyle boxed{true, false, true, true, present::default_text_size};
    const auto box = present::build_text_layers(dot(), boxed, 1);
    OA_CHECK(box.background && box.background_top == 1 && box.background_bottom == 2);
    std::fill(buffer.pixels.begin(), buffer.pixels.end(), uint8_t{3});
    present::lay_text(canvas, box, 0, 2, {255, 255, 255});
    OA_CHECK(pixel(0, 0) == 3 && pixel(0, 1) == 3 && pixel(1, 1) == 0 && pixel(0, 2) == 0);
    OA_CHECK(pixel(1, 2) == 0 && pixel(0, 3) == 3);

    // RGB pixels without a palette keep the colours; with one they are
    // reduced to it.
    std::vector<uint8_t> rgb(3 * 3 * 5, 250);
    auto plain = present::rgb_canvas(rgb, 3, 5, {});
    present::lay_text(plain, ring, 1, 3, {10, 20, 30});
    OA_CHECK(rgb[(2 * 3 + 1) * 3] == 10 && rgb[(2 * 3 + 1) * 3 + 2] == 30);
    OA_CHECK(rgb[(1 * 3) * 3] == 43 && rgb[0] == 250);
    std::fill(rgb.begin(), rgb.end(), uint8_t{250});
    auto reduced = present::rgb_canvas(rgb, 3, 5, test_palette);
    present::lay_text(reduced, ring, 1, 3, {190, 10, 10});
    OA_CHECK(rgb[(2 * 3 + 1) * 3] == 200 && rgb[(2 * 3 + 1) * 3 + 1] == 0);
}

/// The runs a line hands to the picture beneath, in the order handed.
std::vector<present::TextWorldRun>& handed(void* user) {
    return *static_cast<std::vector<present::TextWorldRun>*>(user);
}

void leaves_the_layers_to_the_picture_beneath() {
    // The dot with its outline and shadow: the letter at (1, 2), the
    // outline round it, and the shadow alone at (3, 2), (3, 3) and (1, 4) to
    // (3, 4).
    present::TextStyle bordered{true, true, true, false, present::default_text_size};
    const auto layers = present::build_text_layers(dot(), bordered, 1);
    OA_CHECK(layers.width == 4 && layers.height == 6);
    constexpr std::array<uint8_t, 3> key{1, 2, 3};
    std::vector<uint8_t> rgb(static_cast<std::size_t>(layers.width * layers.height) * 3U);
    const auto clear = [&]() {
        for (std::size_t at = 0; at < rgb.size(); at += 3)
            std::copy(key.begin(), key.end(), rgb.begin() + static_cast<std::ptrdiff_t>(at));
    };
    clear();
    const auto pixel = [&](int32_t x, int32_t y) {
        const auto at = static_cast<std::size_t>(y * layers.width + x) * 3U;
        return std::array<uint8_t, 3>{rgb[at], rgb[at + 1], rgb[at + 2]};
    };
    // A pixel of the canvas's own under the shadow, which it shades itself.
    const auto own = static_cast<std::size_t>(4 * layers.width + 1) * 3U;
    std::fill_n(rgb.begin() + static_cast<std::ptrdiff_t>(own), 3, uint8_t{250});
    std::vector<present::TextWorldRun> runs;
    auto canvas = present::rgb_canvas(rgb, layers.width, layers.height, {});
    canvas.see_through = key;
    canvas.world_user = &runs;
    canvas.world_run = [](void* user, const present::TextWorldRun& run) {
        handed(user).push_back(run);
    };
    present::lay_text(canvas, layers, layers.pen, layers.baseline, {200, 0, 0});
    // The letter covering its pixel whole in its colour; the outline over
    // the key left to the picture beneath, which alone knows how dark it is.
    OA_CHECK((pixel(1, 2) == std::array<uint8_t, 3>{200, 0, 0}));
    OA_CHECK(pixel(0, 1) == key && pixel(2, 3) == key);
    // The shadow alone leaves the key too, and shades the canvas's own
    // pixel half black.
    OA_CHECK(pixel(3, 2) == key && pixel(3, 3) == key && pixel(2, 4) == key);
    OA_CHECK((pixel(1, 4) == std::array<uint8_t, 3>{125, 125, 125}));
    // Every shadow run, then every outline run: the shadow's the outlined
    // dot moved one down and right, the letter and the canvas's own pixel
    // left out, under the outline where they meet; the outline's the ring
    // round the letter.
    using Layer = present::TextWorldLayer;
    OA_CHECK(
        (runs == std::vector<present::TextWorldRun>{
                     {Layer::shadow, 2, 2, 2},
                     {Layer::shadow, 1, 3, 3},
                     {Layer::shadow, 2, 4, 2},
                     {Layer::outline, 0, 1, 3},
                     {Layer::outline, 0, 2, 1},
                     {Layer::outline, 2, 2, 1},
                     {Layer::outline, 0, 3, 3},
                 })
    );
    // Untouched pixels stay the key.
    OA_CHECK(pixel(0, 0) == key && pixel(3, 5) == key);

    // A letter covering a pixel in part leaves it, with its coverage, after
    // the outline under it.
    present::TextMask soft = dot();
    soft.alpha = {0, 160, 0};
    const auto soft_layers = present::build_text_layers(soft, bordered, 1);
    clear();
    runs.clear();
    present::lay_text(canvas, soft_layers, soft_layers.pen, soft_layers.baseline, {200, 0, 0});
    OA_CHECK(pixel(1, 2) == key);
    OA_CHECK(!runs.empty() && runs.back().layer == Layer::letter);
    OA_CHECK((runs.back() == present::TextWorldRun{Layer::letter, 1, 2, 1, {200, 0, 0}, 160}));
    for (std::size_t at = 1; at < runs.size(); ++at)
        OA_CHECK(runs[at - 1].layer <= runs[at].layer);

    // Without see-through pixels every layer is laid on the canvas.
    runs.clear();
    clear();
    canvas.see_through.reset();
    present::lay_text(canvas, layers, layers.pen, layers.baseline, {200, 0, 0});
    // The key is darker than the outline grey, so the outline over it is
    // the dark one.
    OA_CHECK(runs.empty() && pixel(3, 2) != key);
    OA_CHECK(pixel(0, 1) == present::text_dark_outline_color);
}

struct Drawn {
    int32_t calls{};
    present::TextFace face{};
    int32_t scale{};
    int32_t size{};
    present::TextSettings settings{};
};

/// Hooks whose fonts draw each character three pixels wide, recording what
/// they are asked to draw.
present::GameTextHooks three_pixel_hooks(Drawn& drawn) {
    present::GameTextHooks hooks{};
    hooks.context = &drawn;
    hooks.settings = [](void* context) { return static_cast<Drawn*>(context)->settings; };
    hooks.draw = [](void* context,
                    std::string_view text,
                    present::TextFace face,
                    int32_t scale,
                    int32_t size) -> std::shared_ptr<const present::TextMask> {
        auto& seen = *static_cast<Drawn*>(context);
        ++seen.calls;
        seen.face = face;
        seen.scale = scale;
        seen.size = size;
        auto mask = std::make_shared<present::TextMask>(dot());
        mask->character_ends.clear();
        int32_t pen = 0;
        for (std::size_t at = 0; at < text.size();) {
            const auto sequence = present::utf8_sequence(text.substr(at));
            at += sequence.bytes != 0 ? sequence.bytes : 1;
            pen += 3;
            mask->character_ends.push_back(pen);
        }
        mask->advance = pen;
        return mask;
    };
    return hooks;
}

void draws_through_the_hooks() {
    OA_CHECK(!present::modern_text("x", present::TextFace::label, 1, present::game_font_text_size));
    OA_CHECK(
        !present::game_text_settings().style.modern_fonts && !present::game_text_settings().utf8
    );
    Drawn drawn{};
    drawn.settings.style = {true, true, false, false, present::default_text_size};
    drawn.settings.utf8 = true;
    present::set_game_text_hooks(three_pixel_hooks(drawn));
    OA_CHECK(present::game_text_settings().utf8);
    constexpr int32_t full = present::game_font_text_size;
    const auto layers = present::modern_text("ab", present::TextFace::message, 2, full);
    OA_CHECK(layers && drawn.face == present::TextFace::message && drawn.scale == 2);
    OA_CHECK(drawn.size == full);
    OA_CHECK(layers->advance == 8 && !layers->background);
    drawn.settings.style.background = true;
    const auto boxed = present::modern_text("ab", present::TextFace::label, 1, full);
    OA_CHECK(boxed && boxed->background && boxed->background_top == 1);
    OA_CHECK(boxed->background_bottom == 3);
    OA_CHECK(!present::modern_text("ab", present::TextFace::label, 1, full, false)->background);
    drawn.settings.style.background = false;
    const std::string mixed = "a" + std::string(sun) + "b";
    OA_CHECK(present::modern_text_fit(mixed, present::TextFace::label, 1, full, 9) == mixed.size());
    OA_CHECK(present::modern_text_fit(mixed, present::TextFace::label, 1, full, 8) == 4);
    OA_CHECK(present::modern_text_fit(mixed, present::TextFace::label, 1, full, 2) == 0);
    // A size past the setting's range reaches the fonts held to it.
    std::ignore = present::modern_text("ab", present::TextFace::label, 1, 1000);
    OA_CHECK(drawn.size == present::highest_text_size);
    std::ignore = present::modern_text("ab", present::TextFace::label, 1, 10);
    OA_CHECK(drawn.size == present::lowest_text_size);
    present::set_game_text_hooks({});
    OA_CHECK(present::game_text_hooks().draw == nullptr);
}

/// Hooks that draw every line as one mask the test may change in place.
struct SharedMask {
    std::shared_ptr<present::TextMask> mask{std::make_shared<present::TextMask>(dot())};
    present::TextSettings settings{};
    int32_t calls{};
};

/// Returns hooks that draw every line as a SharedMask's mask.
///
/// @param shared the mask and the settings
/// @return the hooks
present::GameTextHooks shared_mask_hooks(SharedMask& shared) {
    present::GameTextHooks hooks{};
    hooks.context = &shared;
    hooks.settings = [](void* context) { return static_cast<SharedMask*>(context)->settings; };
    hooks.draw = [](
                     void* context, std::string_view, present::TextFace, int32_t, int32_t
                 ) -> std::shared_ptr<const present::TextMask> {
        auto& seen = *static_cast<SharedMask*>(context);
        ++seen.calls;
        return seen.mask;
    };
    return hooks;
}

/// Tells whether two lines' layers are the same in every part.
///
/// @param layers a line's layers
/// @param other another's
/// @return true when they are the same
bool same_layers(const present::TextLayers& layers, const present::TextLayers& other) {
    return layers.width == other.width && layers.height == other.height &&
           layers.baseline == other.baseline && layers.pen == other.pen &&
           layers.advance == other.advance && layers.fill == other.fill &&
           layers.outline == other.outline && layers.shadow == other.shadow &&
           layers.background == other.background && layers.background_top == other.background_top &&
           layers.background_bottom == other.background_bottom &&
           layers.drawn_top == other.drawn_top && layers.drawn_bottom == other.drawn_bottom;
}

void keeps_finished_layers() {
    SharedMask shared{};
    shared.settings.style = {true, true, true, true, present::default_text_size};
    present::set_game_text_hooks(shared_mask_hooks(shared));
    constexpr int32_t full = present::game_font_text_size;
    const auto built = [&shared](int32_t scale, bool background) {
        present::TextStyle style = shared.settings.style;
        style.background = style.background && background;
        return present::build_text_layers(*shared.mask, style, present::text_border(scale, full));
    };
    // A line drawn again is laid as it was first built, and as the borders
    // the settings choose would build it now.
    const auto first = present::modern_text("ab", present::TextFace::label, 2, full);
    const auto again = present::modern_text("ab", present::TextFace::label, 2, full);
    OA_CHECK(first && again && same_layers(*first, *again) && same_layers(*again, built(2, true)));
    const auto unboxed = present::modern_text("ab", present::TextFace::label, 2, full, false);
    OA_CHECK(unboxed && !unboxed->background && same_layers(*unboxed, built(2, false)));
    // Letters changed in place are built anew.
    shared.mask->alpha = {255, 255, 0};
    const auto changed = present::modern_text("ab", present::TextFace::label, 2, full);
    OA_CHECK(changed && same_layers(*changed, built(2, true)) && !same_layers(*changed, *first));
    // So are lines whose borders the settings change.
    shared.settings.style.outline = false;
    const auto bare = present::modern_text("ab", present::TextFace::label, 2, full);
    OA_CHECK(bare && same_layers(*bare, built(2, true)) && !same_layers(*bare, *changed));
    // The advance alone is the advance of the layers, for every border and
    // scale.
    for (const bool outline : {false, true})
        for (const bool shadow : {false, true})
            for (const int32_t scale : {1, 2, 3})
                for (const int32_t size : {present::lowest_text_size, full, 200}) {
                    shared.settings.style.outline = outline;
                    shared.settings.style.shadow = shadow;
                    const auto advance =
                        present::modern_text_advance("ab", present::TextFace::label, scale, size);
                    const auto layers =
                        present::modern_text("ab", present::TextFace::label, scale, size);
                    OA_CHECK(advance && layers && *advance == layers->advance);
                }
    present::forget_text_layers();
    present::set_game_text_hooks({});
    OA_CHECK(!present::modern_text_advance("ab", present::TextFace::label, 1, full));
}

void sizes_text_from_the_setting() {
    // The setting's range, its default a fifth below the game fonts' size.
    OA_CHECK(present::lowest_text_size == 50 && present::highest_text_size == 300);
    OA_CHECK(present::default_text_size == 80 && present::game_font_text_size == 100);
    OA_CHECK(present::TextStyle{}.size == present::default_text_size);
    OA_CHECK(present::held_text_size(10) == 50 && present::held_text_size(80) == 80);
    OA_CHECK(present::held_text_size(400) == 300);
    // Lengths at a size, to the nearest pixel, a half up: the message log's
    // step from COMIX's 14 rows, and hattfont12's baseline 12 rows under
    // the pen.
    const std::array<int32_t, 6> sizes{50, 80, 100, 150, 200, 300};
    const std::array<int32_t, 6> steps{7, 11, 14, 21, 28, 42};
    const std::array<int32_t, 6> baselines{6, 10, 12, 18, 24, 36};
    for (std::size_t at = 0; at < sizes.size(); ++at) {
        OA_CHECK(present::sized_length(14, sizes[at]) == steps[at]);
        OA_CHECK(present::sized_length(12, sizes[at]) == baselines[at]);
    }
    // The faces' pixel sizes: DejaVu Sans Bold 14 px beside hattfont12 and
    // 11 px beside the smaller fonts, at the size and times the scale, and
    // never below 7 px.
    const std::array<int32_t, 6> message{7, 11, 14, 21, 28, 42};
    const std::array<int32_t, 6> label{7, 9, 11, 17, 22, 33};
    for (std::size_t at = 0; at < sizes.size(); ++at) {
        OA_CHECK(present::face_pixel_size(14, 1, sizes[at]) == message[at]);
        OA_CHECK(present::face_pixel_size(11, 1, sizes[at]) == label[at]);
        OA_CHECK(present::face_pixel_size(14, 2, sizes[at]) == 2 * message[at]);
    }
    OA_CHECK(present::face_pixel_size(11, 2, 50) == 12);
    OA_CHECK(present::face_pixel_size(14, 1, 1000) == 42);
    // The borders grow with the size, a half down, one pixel at the least.
    OA_CHECK(present::text_border(1, 50) == 1 && present::text_border(1, 100) == 1);
    OA_CHECK(present::text_border(1, 150) == 1 && present::text_border(1, 160) == 2);
    OA_CHECK(present::text_border(1, 200) == 2 && present::text_border(1, 300) == 3);
    OA_CHECK(present::text_border(2, 80) == 2 && present::text_border(2, 100) == 2);
    OA_CHECK(present::text_border(3, 100) == 3 && present::text_border(2, 300) == 6);

    // The size in effect: the settings' while the modern fonts draw game
    // text, held to the range; the game fonts' own otherwise.
    OA_CHECK(present::game_text_size() == present::game_font_text_size);
    Drawn drawn{};
    drawn.settings.style = {true, false, false, false, 150};
    present::set_game_text_hooks(three_pixel_hooks(drawn));
    OA_CHECK(present::game_text_size() == 150);
    drawn.settings.style.size = 900;
    OA_CHECK(present::game_text_size() == present::highest_text_size);
    drawn.settings.style.modern_fonts = false;
    OA_CHECK(present::game_text_size() == present::game_font_text_size);
    // At a larger size the borders thicken: two pixels at 200 percent.
    drawn.settings.style = {true, true, false, false, 200};
    const auto large = present::modern_text("ab", present::TextFace::message, 1, 200);
    OA_CHECK(large && large->width == 1 + 2 * 2 && large->advance == 6 + 2);
    present::set_game_text_hooks({});
}

/// The rows a line breaks into, as text.
std::vector<std::string> rows_of(std::string_view text, int32_t width) {
    std::vector<std::string> rows;
    for (const auto& row : present::modern_text_rows(
             text, present::TextFace::message, 1, present::game_font_text_size, width
         ))
        rows.emplace_back(text.substr(row.offset, row.bytes));
    return rows;
}

void breaks_lines_into_rows() {
    // Without the hooks a line is one row; an empty one none.
    OA_CHECK(rows_of("ab cd", 3) == std::vector<std::string>{"ab cd"});
    OA_CHECK(rows_of("", 3).empty());
    Drawn drawn{};
    drawn.settings.style = {true, false, false, false, present::default_text_size};
    present::set_game_text_hooks(three_pixel_hooks(drawn));
    // Each character three pixels wide: rows break at their last space,
    // the space in neither row.
    OA_CHECK((rows_of("ab cd ef", 12) == std::vector<std::string>{"ab", "cd", "ef"}));
    OA_CHECK((rows_of("ab cd ef", 15) == std::vector<std::string>{"ab cd", "ef"}));
    OA_CHECK(rows_of("ab cd ef", 24) == std::vector<std::string>{"ab cd ef"});
    // A space that does not fit breaks the row before it; the spaces after
    // a break start no row.
    OA_CHECK((rows_of("abc  def", 9) == std::vector<std::string>{"abc", "def"}));
    // A word wider than a row breaks after its last character that fits,
    // and characters of several bytes stay whole.
    OA_CHECK((rows_of("abcdefgh", 9) == std::vector<std::string>{"abc", "def", "gh"}));
    const std::string suns = std::string(sun) + std::string(sun) + std::string(sun);
    OA_CHECK(
        (rows_of(suns, 6) ==
         std::vector<std::string>{std::string(sun) + std::string(sun), std::string(sun)})
    );
    // Every row holds a character, however narrow the room.
    OA_CHECK((rows_of("ab", 1) == std::vector<std::string>{"a", "b"}));
    // Chinese breaks between any two characters after a space early in the
    // row, and keeps the full-width comma off a row's start.
    OA_CHECK((rows_of("ab 指挥官已阵亡", 15) == std::vector<std::string>{"ab 指挥", "官已阵亡"}));
    OA_CHECK((rows_of("建造完成，单位", 12) == std::vector<std::string>{"建造完", "成，单位"}));
    // The borders take room: an outline and a shadow one pixel each.
    drawn.settings.style.outline = true;
    drawn.settings.style.shadow = true;
    OA_CHECK((rows_of("ab cd", 15) == std::vector<std::string>{"ab", "cd"}));
    OA_CHECK(rows_of("ab cd", 17) == std::vector<std::string>{"ab cd"});

    // A line being typed shows the end that fits.
    constexpr int32_t full = present::game_font_text_size;
    const auto tail = [](std::string_view text, int32_t width) {
        return present::modern_text_tail(text, present::TextFace::label, 1, full, width);
    };
    OA_CHECK(tail("abcdef", 9) == 3 && tail("abcdef", 18) == 0 && tail("abcdef", 100) == 0);
    OA_CHECK(tail("abcdef", 2) == 5);
    OA_CHECK(tail("a" + suns, 6) == 4 && tail("", 6) == 0);
    present::set_game_text_hooks({});
    OA_CHECK(tail("abcdef", 2) == 0);
}

void underlines_a_stretch() {
    Drawn drawn{};
    constexpr int32_t full = present::game_font_text_size;
    const auto underline = [](std::string_view text, std::size_t from, std::size_t to) {
        return present::modern_text_underline(text, present::TextFace::label, 1, full, from, to);
    };
    OA_CHECK(!underline("abcd", 1, 3));
    present::set_game_text_hooks(three_pixel_hooks(drawn));
    // Under the characters from the pen after the one before, on the
    // line's lowest row: the dot's line holds three rows over a baseline at
    // its third.
    const auto middle = underline("abcd", 1, 3);
    OA_CHECK(middle && middle->left == 3 && middle->width == 6);
    OA_CHECK(middle && middle->row == 0 && middle->thickness == 1);
    const std::string mixed = "a" + std::string(sun) + "b";
    const auto hanzi = underline(mixed, 1, 1 + sun.size());
    OA_CHECK(hanzi && hanzi->left == 3 && hanzi->width == 3);
    const auto end = underline(mixed, 1, mixed.size());
    OA_CHECK(end && end->left == 3 && end->width == 6);
    // Twice the scale, twice the thickness.
    const auto scaled =
        present::modern_text_underline("abcd", present::TextFace::label, 2, full, 0, 1);
    OA_CHECK(scaled && scaled->thickness == 2);
    OA_CHECK(!underline("abcd", 2, 2) && !underline("abcd", 4, 6) && !underline("", 0, 1));
    present::set_game_text_hooks({});
}

void reduces_colours_to_the_palette() {
    OA_CHECK(
        (present::blend_color({0, 0, 0}, {255, 255, 255}, 0) == std::array<uint8_t, 3>{0, 0, 0})
    );
    OA_CHECK(
        (present::blend_color({0, 100, 200}, {255, 255, 255}, 128) ==
         std::array<uint8_t, 3>{128, 178, 228})
    );
    OA_CHECK(present::nearest_palette_index(test_palette, 4, {200, 200, 200}) == 3);
    OA_CHECK(present::nearest_palette_index(test_palette, 4, {10, 0, 0}) == 0);
    OA_CHECK(present::nearest_palette_index({}, 4, {10, 0, 0}) == 0);
}

} // namespace

int main() {
    reads_utf8_sequences();
    erases_whole_characters();
    reads_and_writes_the_code_page();
    maps_characters_to_font_bytes();
    splits_runs_between_fonts();
    keeps_a_word_in_one_face();
    same_glyph_compares_pictures();
    builds_borders();
    lays_text_on_pixels();
    leaves_the_layers_to_the_picture_beneath();
    draws_through_the_hooks();
    keeps_finished_layers();
    sizes_text_from_the_setting();
    breaks_lines_into_rows();
    underlines_a_stretch();
    reduces_colours_to_the_palette();
    return oa::test::check_exit_status();
}
