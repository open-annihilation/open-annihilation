// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The FNT parser on a small font and on each malformed input, the GAF font
// parser on bytes that are no GAF, and the named-font search through
// language folders, including the empty and missing files that end it.

#include "oa/formats/fnt.hpp"
#include "oa/test/scratch_directory.hpp"
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace {

using oa::base::bytes::DecodeCode;

int failures = 0;

/// Records a failed check.
///
/// @param line source line of the check
/// @param what text of the failed condition
void report_failure(int line, const char* what) {
    std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, line, what);
    ++failures;
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            report_failure(__LINE__, #condition);                                                  \
        }                                                                                          \
    } while (false)

void put16(std::vector<uint8_t>& b, std::size_t o, uint16_t v) {
    b[o] = static_cast<uint8_t>(v);
    b[o + 1] = static_cast<uint8_t>(v >> 8U);
}

void put32(std::vector<uint8_t>& b, std::size_t o, uint32_t v) {
    put16(b, o, static_cast<uint16_t>(v));
    put16(b, o + 2, static_cast<uint16_t>(v >> 16U));
}

std::vector<uint8_t> fnt_with_width(uint8_t width) {
    const std::size_t packed = (static_cast<std::size_t>(width) + 7U) / 8U;
    std::vector<uint8_t> bytes(516 + 1 + packed);
    bytes[0] = 1;
    constexpr std::size_t glyph = 516;
    bytes[4 + static_cast<std::size_t>('A') * 2] = static_cast<uint8_t>(glyph);
    bytes[5 + static_cast<std::size_t>('A') * 2] = static_cast<uint8_t>(glyph >> 8U);
    bytes[glyph] = width;
    return bytes;
}

void write_bytes(const std::filesystem::path& path, std::span<const uint8_t> bytes) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    CHECK(out.good());
}

uint32_t named_width(oa::AssetStore& assets, std::string_view name, std::string_view language) {
    const auto font = oa::formats::fnt::load_named_fnt(assets, name, language);
    CHECK(font.ok());
    return font.ok() ? oa::formats::fnt::measure_text(*font.value, "A") : 0;
}

/// Reports whether parse_fnt fails with a code at an offset.
///
/// @param bytes the file
/// @param code the error code expected
/// @param offset the file offset expected
/// @return true when the parse fails so
bool fnt_fails_with(const std::vector<uint8_t>& bytes, DecodeCode code, uint64_t offset) {
    const auto parsed = oa::formats::fnt::parse_fnt(bytes);
    return !parsed.ok() && parsed.error.code == code && parsed.error.offset == offset &&
           parsed.error.message != nullptr;
}

void test_parse_fnt() {
    std::vector<uint8_t> b(522);
    put16(b, 0, 2);
    put16(b, 2, 0x1201);
    put16(b, 4 + static_cast<std::size_t>(' ') * 2, 516);
    put16(b, 4 + static_cast<std::size_t>('A') * 2, 519);
    b[516] = 3;
    b[519] = 3;
    b[520] = 0xA8; // continuous bits: 101 / 010
    const auto parsed = oa::formats::fnt::parse_fnt(b);
    CHECK(parsed.ok());
    if (!parsed.ok())
        return;
    const auto& f = *parsed.value;
    CHECK(f.word_after_height == 0x1201);
    // The lift is the second word's low byte, signed.
    CHECK(oa::formats::fnt::row_lift(f) == 1);
    oa::formats::fnt::Font lowered;
    lowered.word_after_height = 0x01ff;
    CHECK(oa::formats::fnt::row_lift(lowered) == -1);
    CHECK(oa::formats::fnt::measure_text(f, " A\nA") == 9);
    // A text too wide loses its last characters until it fits; the
    // newline, which adds nothing, stays after the 'A' that fits.
    CHECK(oa::formats::fnt::fit_text(f, " A\nA", 9) == " A\nA");
    CHECK(oa::formats::fnt::fit_text(f, " A\nA", 8) == " A\n");
    CHECK(oa::formats::fnt::fit_text(f, " A\nA", 5) == " ");
    CHECK(oa::formats::fnt::fit_text(f, " A\nA", 2).empty());
    CHECK(oa::formats::fnt::fit_text(f, "", 0).empty());
    CHECK(oa::formats::fnt::line_height(f) == 4);
    // The glyph rows start the lift above the pen row: 'A' with its pen on
    // row 2 has its top row on row 1 and its bottom row on row 2.
    std::vector<uint8_t> pixels(12, 7);
    std::vector<uint8_t> coverage(12);
    CHECK(oa::formats::fnt::raster_text({4, 3, 4, pixels, coverage}, f, " A", 0, 2) == 6);
    CHECK(pixels[7] == 255);
    CHECK(coverage[7] == 1);
    CHECK(coverage[11] == 0);
    CHECK(oa::formats::fnt::raster_text({4, 3, 4, pixels, coverage}, f, "A", -2, 0) == 1);
    // A font with no lift draws from the pen row itself, and a negative
    // lift draws below it.
    auto level = f;
    level.word_after_height = 0x1200;
    std::vector<uint8_t> level_pixels(12, 7);
    std::vector<uint8_t> level_coverage(12);
    CHECK(
        oa::formats::fnt::raster_text({4, 3, 4, level_pixels, level_coverage}, level, "A", 0, 1) ==
        3
    );
    CHECK(level_coverage[4] == 1 && level_coverage[0] == 0);
    auto sunk = f;
    sunk.word_after_height = 0x00ff;
    std::vector<uint8_t> sunk_pixels(12, 7);
    std::vector<uint8_t> sunk_coverage(12);
    CHECK(
        oa::formats::fnt::raster_text({4, 3, 4, sunk_pixels, sunk_coverage}, sunk, "A", 0, 0) == 3
    );
    CHECK(sunk_coverage[4] == 1 && sunk_coverage[0] == 0);

    // The 'A' bitmap needs the byte at 520, which is cut off.
    auto cut = b;
    cut.resize(520);
    CHECK(fnt_fails_with(cut, DecodeCode::truncated, 520));
    // A header shorter than 516 bytes.
    CHECK(fnt_fails_with(std::vector<uint8_t>(515), DecodeCode::truncated, 515));
    // A glyph height of 0 or over 128.
    auto flat = b;
    put16(flat, 0, 0);
    CHECK(fnt_fails_with(flat, DecodeCode::out_of_range, 0));
    auto tall = b;
    put16(tall, 0, 129);
    CHECK(fnt_fails_with(tall, DecodeCode::out_of_range, 0));
    // A glyph offset inside the header, or past the end of the file; the
    // offset reported is the table entry's.
    const std::size_t entry = 4 + static_cast<std::size_t>('A') * 2;
    auto inside = b;
    put16(inside, entry, 100);
    CHECK(fnt_fails_with(inside, DecodeCode::out_of_range, entry));
    auto past = b;
    put16(past, entry, 600);
    CHECK(fnt_fails_with(past, DecodeCode::out_of_range, entry));
    // A glyph width of 0 or over 128, at the glyph.
    auto narrow = b;
    narrow[519] = 0;
    CHECK(fnt_fails_with(narrow, DecodeCode::out_of_range, 519));
    auto wide = b;
    wide[519] = 129;
    CHECK(fnt_fails_with(wide, DecodeCode::out_of_range, 519));
    // An input over the size limit.
    CHECK(fnt_fails_with(
        std::vector<uint8_t>(oa::formats::fnt::limit::input_bytes + 1),
        DecodeCode::limit_exceeded,
        oa::formats::fnt::limit::input_bytes
    ));
}

/// Builds a GAF font of one sequence holding one raw glyph frame.
///
/// @param width the glyph's width in pixels
/// @return the file
std::vector<uint8_t> one_glyph_gaf(uint16_t width) {
    constexpr std::size_t frame_at = 64;
    constexpr std::size_t pixels_at = frame_at + 24;
    std::vector<uint8_t> b(pixels_at + width, 1);
    std::fill(b.begin(), b.begin() + static_cast<std::ptrdiff_t>(pixels_at), uint8_t{0});
    put32(b, 0, 0x00010100U);
    put32(b, 4, 1);
    put32(b, 12, 16);
    put16(b, 16, 1);
    put32(b, 56, static_cast<uint32_t>(frame_at));
    put16(b, frame_at, width);
    put16(b, frame_at + 2, 1);
    put32(b, frame_at + 16, static_cast<uint32_t>(pixels_at));
    return b;
}

void test_parse_gaf() {
    const auto parsed = oa::formats::fnt::parse_gaf(std::vector<uint8_t>{1, 2, 3, 4});
    CHECK(!parsed.ok() && parsed.error.code == DecodeCode::malformed);
    CHECK(!oa::formats::fnt::parse_gaf({}).ok());
    // A glyph over 128 pixels is refused before it is decoded.
    CHECK(oa::formats::fnt::parse_gaf(one_glyph_gaf(128)).ok());
    const auto wide = oa::formats::fnt::parse_gaf(one_glyph_gaf(129));
    CHECK(!wide.ok() && wide.error.code == DecodeCode::out_of_range && wide.error.offset == 64);
}

void test_named_fonts() {
    const auto root = oa::test::make_scratch_directory("oa-font-format-named-fnt");
    write_bytes(root / "fonts" / "smlfont.FNT", fnt_with_width(3));
    oa::AssetStore assets(root);
    CHECK(named_width(assets, "smlfont.old", "") == 3);
    CHECK(named_width(assets, "smlfont", "english") == 3);
    write_bytes(root / "fonts-english" / "smlfont.FNT", fnt_with_width(5));
    // The store lists each loose folder once; a rescan picks up the new folder.
    assets.mark_loose_shadows();
    CHECK(named_width(assets, "smlfont", "english") == 5);
    CHECK(named_width(assets, "smlfont", "") == 3);
    write_bytes(root / "fonts-en.FNT", fnt_with_width(7));
    write_bytes(root / "fonts-en.gb" / "smlfont.FNT", fnt_with_width(9));
    assets.mark_loose_shadows();
    CHECK(named_width(assets, "smlfont", "en.gb") == 7);

    // An empty file in the language folder ends the search: the valid font
    // in the plain folder is not tried.
    write_bytes(root / "fonts-english" / "blank.FNT", {});
    write_bytes(root / "fonts" / "blank.FNT", fnt_with_width(4));
    assets.mark_loose_shadows();
    const auto blank = oa::formats::fnt::load_named_fnt(assets, "blank", "english");
    CHECK(!blank.ok() && blank.error.code == DecodeCode::not_found);
    // So does a malformed one, with its own error.
    write_bytes(root / "fonts-english" / "bad.FNT", std::vector<uint8_t>{1, 2, 3, 4});
    write_bytes(root / "fonts" / "bad.FNT", fnt_with_width(4));
    assets.mark_loose_shadows();
    const auto bad = oa::formats::fnt::load_named_fnt(assets, "bad", "english");
    CHECK(
        !bad.ok() && bad.error.code == DecodeCode::truncated &&
        std::string_view(bad.error.message) == "FNT is shorter than its 516-byte header"
    );
    // A font in neither folder.
    const auto absent = oa::formats::fnt::load_named_fnt(assets, "missing", "english");
    CHECK(!absent.ok() && absent.error.code == DecodeCode::not_found);
    // A path over the game's 255-byte limit.
    const auto long_name = oa::formats::fnt::load_named_fnt(assets, std::string(300, 'n'), "");
    CHECK(!long_name.ok() && long_name.error.code == DecodeCode::limit_exceeded);
    std::filesystem::remove_all(root);
}

} // namespace

int main() {
    test_parse_fnt();
    test_parse_gaf();
    test_named_fonts();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
