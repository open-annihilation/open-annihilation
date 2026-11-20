// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Every language's own name draws from the fonts that travel with the game,
// and so does every line of the endonym lists, which are the notice shown
// before a language pack is installed. Every Bengali letter of the Bengali
// list is in the Bengali endonym face, and, with --shape, a Bengali line
// shapes with that face to the glyphs HarfBuzz gives with the full Noto Sans
// Bengali Bold. --shape skips when this build does not shape, and when the
// full face is absent.

#include "oa/data/languages.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/test/check.hpp"

#if defined(OA_TEXT_SHAPING)
#include <hb-ft.h>
#include <hb.h>

#include <ft2build.h>
#include FT_FREETYPE_H
#endif

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace languages = oa::data::languages;
namespace text_font = oa::platform::text_font;

/// Requires that a line of the endonym list draws in both weights.
void require_line(text_font::FontStack& stack, std::string_view line) {
    OA_CHECK(stack.draws(line, text_font::Weight::bold));
    OA_CHECK(stack.draws(line, text_font::Weight::regular));
}

/// Returns a notice line with the list's sample size in place of "%s".
///
/// @param text a notice line
/// @return the line the endonym list holds
std::string notice_line(std::string_view text) {
    std::string line(text);
    const auto size = line.find("%s");
    if (size != std::string::npos)
        line.replace(size, 2, "4.0 MB");
    return line;
}

/// Requires every line of one notice, and that each is a line of the lists.
///
/// @param stack the fonts that travel with the game
/// @param lines the lists' lines
/// @param notice the notice
void require_notice(
    text_font::FontStack& stack,
    const std::vector<std::string>& lines,
    const languages::PackNotice& notice
) {
    const std::string_view texts[] = {
        notice.body_sized,
        notice.body,
        notice.download,
        notice.failed,
        notice.retry,
        notice.unavailable,
    };
    for (const std::string_view text : texts) {
        const std::string line = notice_line(text);
        require_line(stack, line);
        bool listed = false;
        for (const std::string& known : lines)
            if (known == line)
                listed = true;
        OA_CHECK(listed);
    }
}

/// Reads the lines of an endonym list: comments and blank lines left out.
///
/// @param path the list's path
/// @param[out] lines receives the lines
/// @return false when the list cannot be read
bool read_list(const char* path, std::vector<std::string>& lines) {
    std::ifstream list(path, std::ios::binary);
    if (!list) {
        std::cerr << "data-languages-endonyms: cannot read " << path << '\n';
        return false;
    }
    std::string line;
    while (std::getline(list, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line.front() == '#')
            continue;
        lines.push_back(line);
    }
    return true;
}

/// Tells whether a character is one the Bengali endonym face is for: a letter
/// of the Bengali block, or the danda, which Bengali ends its sentences with.
///
/// @param character the code point
/// @return true for such a character
bool is_bengali(char32_t character) noexcept {
    return (character >= 0x0980 && character <= 0x09FF) || character == 0x0964 ||
           character == 0x0965;
}

/// Requires that every Bengali character of a line is in the Bengali endonym
/// face, in both weights, and so is drawn by it with no pack installed.
///
/// @param stack the fonts that travel with the game
/// @param line a line of the Bengali list
/// @return how many Bengali characters the line holds
std::size_t require_bengali_letters(text_font::FontStack& stack, std::string_view line) {
    const auto characters = text_font::decode_utf8(line);
    OA_CHECK(characters.has_value());
    if (!characters)
        return 0;
    std::size_t found = 0;
    for (const char32_t character : *characters) {
        if (!is_bengali(character))
            continue;
        ++found;
        OA_CHECK(
            stack.face_for(character, text_font::Weight::bold) == text_font::Face::endonyms_bengali
        );
        OA_CHECK(
            stack.face_for(character, text_font::Weight::regular) ==
            text_font::Face::endonyms_bengali
        );
    }
    return found;
}

/// Checks both lists, every language's own name and every notice.
///
/// @param stack the fonts that travel with the game
/// @param cjk_path the Chinese, Japanese and Korean list
/// @param bengali_path the Bengali list
/// @return the exit status of the check: 1 when a list cannot be read
int check_lists(text_font::FontStack& stack, const char* cjk_path, const char* bengali_path) {
    OA_CHECK(stack.has_face(text_font::Face::endonyms));
    OA_CHECK(stack.has_face(text_font::Face::endonyms_bengali));
    for (const languages::Language* language : languages::known_languages()) {
        OA_CHECK(language != nullptr);
        if (language != nullptr)
            require_line(stack, language->endonym);
    }
    std::vector<std::string> lines;
    if (!read_list(cjk_path, lines))
        return 1;
    const std::size_t cjk_lines = lines.size();
    if (!read_list(bengali_path, lines))
        return 1;
    for (const std::string& line : lines)
        require_line(stack, line);
    // The Bengali list's characters are in the Bengali endonym face.
    std::size_t bengali_letters = 0;
    for (std::size_t at = cjk_lines; at < lines.size(); ++at)
        bengali_letters += require_bengali_letters(stack, lines[at]);
    OA_CHECK(bengali_letters > 0);
    // The Chinese list holds no Bengali letter, and the Bengali list no ideograph.
    for (std::size_t at = 0; at < lines.size(); ++at) {
        const auto characters = text_font::decode_utf8(lines[at]);
        OA_CHECK(characters.has_value());
        if (!characters)
            continue;
        for (const char32_t character : *characters) {
            const bool ideograph = character >= 0x4E00 && character <= 0x9FFF;
            OA_CHECK(at < cjk_lines ? !is_bengali(character) : !ideograph);
        }
    }
    OA_CHECK(languages::pack_notice("en") == nullptr);
    OA_CHECK(languages::pack_notice("zh_Hans") != nullptr);
    OA_CHECK(languages::pack_notice("bn") != nullptr);
    for (const languages::Language* language : languages::known_languages()) {
        if (language == nullptr)
            continue;
        if (const languages::PackNotice* notice = languages::pack_notice(language->tag))
            require_notice(stack, lines, *notice);
    }
    // Bengali has no entry of its own: its pack's entry comes from a catalogue,
    // so its notice is checked by its tag.
    if (const languages::PackNotice* notice = languages::pack_notice("bn"))
        require_notice(stack, lines, *notice);
    return 0;
}

#if defined(OA_TEXT_SHAPING)

/// Appends a code point to UTF-8 text.
///
/// @param[out] text receives the bytes
/// @param character a code point, not a surrogate, at most U+10FFFF
void append_utf8(std::string& text, char32_t character) {
    if (character < 0x80) {
        text.push_back(static_cast<char>(character));
    } else if (character < 0x800) {
        text.push_back(static_cast<char>(0xC0U | (character >> 6)));
        text.push_back(static_cast<char>(0x80U | (character & 0x3FU)));
    } else if (character < 0x10000) {
        text.push_back(static_cast<char>(0xE0U | (character >> 12)));
        text.push_back(static_cast<char>(0x80U | ((character >> 6) & 0x3FU)));
        text.push_back(static_cast<char>(0x80U | (character & 0x3FU)));
    } else {
        text.push_back(static_cast<char>(0xF0U | (character >> 18)));
        text.push_back(static_cast<char>(0x80U | ((character >> 12) & 0x3FU)));
        text.push_back(static_cast<char>(0x80U | ((character >> 6) & 0x3FU)));
        text.push_back(static_cast<char>(0x80U | (character & 0x3FU)));
    }
}

/// A run of one shaping script: Bengali letters, or the danda, which the
/// engine shapes as Devanagari.
struct Run {
    std::string text;
    bool danda{};
};

/// Splits a line into the runs the engine shapes: a stretch of the Bengali
/// block, and a stretch of the danda marks, which stand in the Devanagari
/// block. Every other character separates runs and is not shaped.
///
/// @param line a line of the Bengali list
/// @return the runs, in order
std::vector<Run> shaped_runs(std::string_view line) {
    std::vector<Run> runs;
    const auto characters = text_font::decode_utf8(line);
    if (!characters)
        return runs;
    bool open = false;
    for (const char32_t character : *characters) {
        if (!is_bengali(character)) {
            open = false;
            continue;
        }
        const bool danda = character == 0x0964 || character == 0x0965;
        if (!open || runs.back().danda != danda) {
            runs.push_back(Run{std::string(), danda});
            open = true;
        }
        append_utf8(runs.back().text, character);
    }
    return runs;
}

/// A FreeType face held open, closed when this dies.
struct HeldFace {
    FT_Library library{};
    FT_Face face{};

    HeldFace() = default;

    ~HeldFace() {
        if (face != nullptr)
            FT_Done_Face(face);
        if (library != nullptr)
            FT_Done_FreeType(library);
    }

    HeldFace(const HeldFace&) = delete;
    HeldFace& operator=(const HeldFace&) = delete;
};

/// Shapes the Bengali lines with the endonym face and with the full face,
/// and requires the same glyphs, clusters, advances and offsets.
///
/// The endonym face is drawn at the sans faces' size, so both are shaped at
/// 14 px, mono, as the engine shapes a run: HarfBuzz's FreeType font with the
/// style's load flags, left to right, with the run's script.
///
/// @param stack the fonts that travel with the game
/// @param bengali_path the Bengali list
/// @param full_face the full Noto Sans Bengali Bold
/// @return the exit status of the check
int check_shaping(text_font::FontStack& stack, const char* bengali_path, const char* full_face) {
    std::vector<std::string> lines;
    if (!read_list(bengali_path, lines))
        return 1;
    HeldFace held;
    OA_CHECK(FT_Init_FreeType(&held.library) == 0);
    if (held.library == nullptr)
        return oa::test::check_exit_status();
    OA_CHECK(FT_New_Face(held.library, full_face, 0, &held.face) == 0);
    if (held.face == nullptr)
        return oa::test::check_exit_status();
    constexpr int32_t pixel_size = 14;
    OA_CHECK(FT_Set_Pixel_Sizes(held.face, 0, pixel_size) == 0);
    hb_font_t* shaped_font = hb_ft_font_create(held.face, nullptr);
    OA_CHECK(shaped_font != nullptr);
    if (shaped_font == nullptr)
        return oa::test::check_exit_status();
    const FT_Int32 load = FT_LOAD_NO_BITMAP | FT_LOAD_TARGET_MONO;
    hb_ft_font_set_load_flags(shaped_font, static_cast<int>(load));

    text_font::Style style;
    style.pixel_size = pixel_size;
    style.weight = text_font::Weight::bold;
    style.rendering = text_font::Rendering::mono;

    std::size_t compared = 0;
    for (const std::string& line : lines) {
        for (const Run& run : shaped_runs(line)) {
            hb_buffer_t* buffer = hb_buffer_create();
            OA_CHECK(buffer != nullptr);
            if (buffer == nullptr)
                continue;
            hb_buffer_add_utf8(
                buffer,
                run.text.data(),
                static_cast<int>(run.text.size()),
                0,
                static_cast<int>(run.text.size())
            );
            hb_buffer_set_direction(buffer, HB_DIRECTION_LTR);
            hb_buffer_set_script(buffer, run.danda ? HB_SCRIPT_DEVANAGARI : HB_SCRIPT_BENGALI);
            OA_CHECK(hb_buffer_allocation_successful(buffer));
            hb_shape(shaped_font, buffer, nullptr, 0);
            unsigned int glyph_count = 0;
            const hb_glyph_info_t* infos = hb_buffer_get_glyph_infos(buffer, &glyph_count);
            const hb_glyph_position_t* positions =
                hb_buffer_get_glyph_positions(buffer, &glyph_count);
            OA_CHECK(infos != nullptr && positions != nullptr && glyph_count > 0);
            const auto shaped = stack.shape(run.text, style);
            OA_CHECK(shaped.has_value());
            if (shaped && infos != nullptr && positions != nullptr) {
                OA_CHECK(shaped->size() == glyph_count);
                const std::size_t count =
                    std::min(shaped->size(), static_cast<std::size_t>(glyph_count));
                for (std::size_t glyph_at = 0; glyph_at < count; ++glyph_at) {
                    const text_font::ShapedGlyph& one = (*shaped)[glyph_at];
                    OA_CHECK(one.face == text_font::Face::endonyms_bengali);
                    // Glyph 0 is the missing-glyph box: the cut lacks a glyph
                    // the list needs.
                    OA_CHECK(one.glyph != 0);
                    OA_CHECK(one.glyph == infos[glyph_at].codepoint);
                    OA_CHECK(one.cluster == infos[glyph_at].cluster);
                    OA_CHECK(one.x_advance == static_cast<int32_t>(positions[glyph_at].x_advance));
                    OA_CHECK(one.y_advance == static_cast<int32_t>(positions[glyph_at].y_advance));
                    OA_CHECK(one.x_offset == static_cast<int32_t>(positions[glyph_at].x_offset));
                    OA_CHECK(one.y_offset == static_cast<int32_t>(positions[glyph_at].y_offset));
                }
                ++compared;
            }
            hb_buffer_destroy(buffer);
        }
    }
    // Every run of the list was compared, the name among them.
    OA_CHECK(compared > 0);
    hb_font_destroy(shaped_font);
    return oa::test::check_exit_status();
}

#endif

} // namespace

int main(int argc, char** argv) {
    const bool shape = argc >= 2 && std::string_view(argv[1]) == "--shape";
    if ((!shape && argc != 3) || (shape && argc != 4)) {
        std::cerr << "data-languages-endonyms: the endonym lists' paths are required, "
                     "or --shape, the Bengali list's path and the full Bengali face\n";
        return 1;
    }
    auto stack = text_font::FontStack::open(text_font::bundled_font_directory());
    OA_CHECK(stack != nullptr);
    if (!stack)
        return oa::test::check_exit_status();
    if (!shape) {
        const int status = check_lists(*stack, argv[1], argv[2]);
        return status != 0 ? status : oa::test::check_exit_status();
    }
#if !defined(OA_TEXT_SHAPING)
    std::cout << "data-languages-endonyms --shape: skipped, this build does not shape\n";
    return 77;
#else
    if (!std::filesystem::is_regular_file(argv[3])) {
        std::cout << "data-languages-endonyms --shape: skipped, " << argv[3] << " is absent\n";
        return 77;
    }
    return check_shaping(*stack, argv[2], argv[3]);
#endif
}
