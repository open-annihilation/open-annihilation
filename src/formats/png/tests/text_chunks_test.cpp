// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Text chunks in a PNG file: both kinds round-trip, a keyword or a text that
// breaks the PNG rules is refused, the chunks sit between the header and the
// image data, and the bytes of one small picture are pinned.

#include "oa/base/sha256.hpp"
#include "oa/formats/png.hpp"
#include "png_internal.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using namespace oa::formats::png;
namespace sha256 = oa::base::sha256;

int g_failures = 0;

/// Records a failed check.
///
/// @param line source line of the check
/// @param what text of the failed condition
void report_failure(int line, const char* what) {
    std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, line, what);
    ++g_failures;
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            report_failure(__LINE__, #condition);                                                  \
        }                                                                                          \
    } while (false)

// "Café · 3:41" in UTF-8.
constexpr std::string_view k_caption = "Caf\xC3\xA9 \xC2\xB7 3:41";

// Digests of the 2 by 2 grey picture below. The plain pair is what write and
// write_stored produced for that picture with no text chunks.
constexpr std::string_view k_write_plain =
    "10db1cb4e3733eef478f564d5b88fb76f07d869a2908bfc1067442b97ae0b7b6";
constexpr std::string_view k_stored_plain =
    "255b8fb4123baf2dc00f48bfc01115dbef4b83b0210402da08e288a550323aca";
constexpr std::string_view k_write_texts =
    "7faa557ebd71affad65e11b988f2c594dc3343fef604b0aa3b885ff179a87c1a";
constexpr std::string_view k_stored_texts =
    "10fe6f0c7a279c1a6f09b5466aef3e74b9e3dc1766a0f3d8d04cfa55ed219e17";

struct Counts {
    int warnings = 0;
    int errors = 0;
};

void count_warning(void* user, const char*) {
    ++static_cast<Counts*>(user)->warnings;
}

void count_error(void* user, const char*) {
    ++static_cast<Counts*>(user)->errors;
}

Messages counting(Counts* counts) {
    return Messages{counts, count_warning, count_error};
}

Header grey_picture() {
    return Header{2, 2, 8, ColorType::grey, Interlace::none};
}

std::vector<uint8_t> grey_rows() {
    return {0x10, 0x20, 0x30, 0x40};
}

/// Writes a picture with one writer.
///
/// @param image the picture
/// @param stored true for the stored form
/// @param[out] file the file, when writing succeeds
/// @return false when the writer refuses the picture
bool encode(const Image& image, bool stored, std::vector<uint8_t>* file) {
    return stored ? write_stored(image, file) : write(image, file);
}

/// The chunk types of a file, in order, through the end chunk.
///
/// @param file the file
/// @return the four-byte types; empty when the file is shorter than one chunk
std::vector<std::string> chunk_types(std::span<const uint8_t> file) {
    std::vector<std::string> types;
    std::size_t at = k_signature_bytes;
    while (file.size() >= at && file.size() - at >= detail::k_chunk_overhead) {
        const uint32_t length = detail::read_be32(file.data() + at);
        if (static_cast<std::size_t>(length) > file.size() - at - detail::k_chunk_overhead)
            break;
        types.emplace_back(reinterpret_cast<const char*>(file.data() + at + 4), 4);
        at += detail::k_chunk_overhead + length;
        if (types.back() == "IEND")
            break;
    }
    return types;
}

/// Appends one chunk, with its checksum, to a file being built by hand.
///
/// @param[in,out] out the file bytes
/// @param type the four-byte chunk type
/// @param data the chunk body
void append_raw_chunk(std::vector<uint8_t>& out, const char* type, std::span<const uint8_t> data) {
    const std::size_t start = out.size();
    out.resize(start + detail::k_chunk_overhead + data.size());
    uint8_t* cursor = out.data() + start;
    detail::write_be32(cursor, static_cast<uint32_t>(data.size()));
    std::memcpy(cursor + 4, type, 4);
    if (!data.empty())
        std::memcpy(cursor + 8, data.data(), data.size());
    detail::write_be32(cursor + 8 + data.size(), detail::chunk_crc(cursor + 4, 4 + data.size()));
}

/// Copies text into bytes, one byte per character.
///
/// @param text the characters
/// @return the bytes
std::vector<uint8_t> bytes_of(std::string_view text) {
    std::vector<uint8_t> out(text.size());
    for (std::size_t index = 0; index < text.size(); ++index)
        out[index] = static_cast<uint8_t>(text[index]);
    return out;
}

/// Inserts chunks immediately before the first chunk of one type.
///
/// @param file a file this writer produced
/// @param stop the four-byte type to stop at
/// @param extra the chunks to insert, each type then body
/// @return the rebuilt file
std::vector<uint8_t> insert_before_type(
    const std::vector<uint8_t>& file,
    std::string_view stop,
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& extra
) {
    std::size_t at = k_signature_bytes;
    while (at < file.size() && file.size() - at >= detail::k_chunk_overhead) {
        const std::string_view here(reinterpret_cast<const char*>(file.data() + at + 4), 4);
        if (here == stop)
            break;
        const uint32_t length = detail::read_be32(file.data() + at);
        if (static_cast<std::size_t>(length) > file.size() - at - detail::k_chunk_overhead)
            break;
        at += detail::k_chunk_overhead + length;
    }
    std::vector<uint8_t> rebuilt(file.begin(), file.begin() + static_cast<std::ptrdiff_t>(at));
    for (const auto& chunk : extra)
        append_raw_chunk(rebuilt, chunk.first.c_str(), chunk.second);
    rebuilt.insert(rebuilt.end(), file.begin() + static_cast<std::ptrdiff_t>(at), file.end());
    return rebuilt;
}

/// Inserts chunks immediately before the end chunk.
///
/// @param file a file this writer produced
/// @param extra the chunks to insert, each type then body
/// @return the rebuilt file
std::vector<uint8_t> insert_before_end(
    const std::vector<uint8_t>& file,
    const std::vector<std::pair<std::string, std::vector<uint8_t>>>& extra
) {
    return insert_before_type(file, "IEND", extra);
}

bool same_chunk(const TextChunk& left, const TextChunk& right) {
    return left.kind == right.kind && left.keyword == right.keyword && left.text == right.text;
}

/// Round-trips a list of chunks through both writers.
///
/// @param chunks the chunks
/// @param what names the case when it fails
void round_trip(std::span<const TextChunk> chunks, const char* what) {
    const std::vector<uint8_t> rows = grey_rows();
    const Image image{grey_picture(), {}, rows, chunks};
    const bool forms[] = {false, true};
    for (const bool stored : forms) {
        std::vector<uint8_t> file;
        if (!encode(image, stored, &file)) {
            std::fprintf(stderr, "round trip refused: %s stored=%d\n", what, stored ? 1 : 0);
        }
        CHECK(encode(image, stored, &file));
        std::vector<TextChunk> read;
        CHECK(read_texts(file, &read));
        CHECK(read.size() == chunks.size());
        if (read.size() == chunks.size()) {
            for (std::size_t index = 0; index < chunks.size(); ++index)
                CHECK(same_chunk(read[index], chunks[index]));
        }
        Counts counts;
        Info info;
        CHECK(read_info(file, counting(&counts), &info));
        std::vector<uint8_t> decoded(rows.size());
        CHECK(read_image(file, info, {}, counting(&counts), decoded) == Progress::end);
        CHECK(decoded == rows);
        CHECK(counts.warnings == 0);
        CHECK(counts.errors == 0);
    }
}

void test_round_trip() {
    const std::string short_keyword(1, 'Q');
    const std::string long_keyword = std::string(78, 'k') + '\xF1';
    const char latin1_cafe[] = {'C', 'a', 'f', '\xE9'};
    const TextChunk chunks[] = {
        {TextKind::latin1, "Comment", "ridge"},
        {TextKind::utf8, "Title", k_caption},
        {TextKind::latin1, short_keyword, "n"},
        {TextKind::utf8, long_keyword, "players"},
        {TextKind::latin1, "Description", std::string_view(latin1_cafe, 4)},
        {TextKind::latin1, "Software", ""},
        {TextKind::utf8, "Author", ""},
        {TextKind::latin1, "A B", "kept"},
    };
    round_trip(chunks, "both kinds");
    round_trip({}, "no text");
}

/// Both writers refuse the chunk and leave the output as it was.
///
/// @param chunk the chunk
/// @param what names the case
void expect_refused(const TextChunk& chunk, const char* what) {
    const uint8_t pixel = 0;
    const Image image{
        Header{1, 1, 8, ColorType::grey, Interlace::none},
        {},
        std::span(&pixel, 1),
        std::span(&chunk, 1)
    };
    std::vector<uint8_t> compressed{0x5a};
    std::vector<uint8_t> stored{0x5a};
    const bool wrote = write(image, &compressed);
    const bool wrote_stored = write_stored(image, &stored);
    if (wrote || wrote_stored)
        std::fprintf(stderr, "accepted a chunk that breaks a rule: %s\n", what);
    CHECK(!wrote);
    CHECK(!wrote_stored);
    CHECK(compressed.size() == 1 && compressed[0] == 0x5a);
    CHECK(stored.size() == 1 && stored[0] == 0x5a);
}

void test_refusals() {
    const std::string eighty(80, 'a');
    const char nul_text[] = {'a', '\0', 'b'};
    const std::string_view nul(nul_text, 3);
    expect_refused({TextKind::latin1, "", "x"}, "empty keyword");
    expect_refused({TextKind::utf8, eighty, "x"}, "80-byte keyword");
    expect_refused({TextKind::latin1, " Title", "x"}, "leading space");
    expect_refused({TextKind::latin1, "Title ", "x"}, "trailing space");
    expect_refused({TextKind::utf8, "Ti  tle", "x"}, "double space");
    expect_refused({TextKind::latin1, "\x01", "x"}, "control byte");
    expect_refused({TextKind::latin1, "A\x7F", "x"}, "delete");
    expect_refused({TextKind::latin1, "\xA0", "x"}, "byte 160");
    expect_refused({TextKind::latin1, "Comment", nul}, "NUL in tEXt");
    expect_refused({TextKind::utf8, "Title", nul}, "NUL in iTXt");
    expect_refused({TextKind::utf8, "Title", "\xC3\x28"}, "invalid UTF-8");
    expect_refused({TextKind::utf8, "Title", "Caf\xE9"}, "Latin-1 as UTF-8");
    expect_refused({TextKind::utf8, "Title", "\xED\xA0\x80"}, "surrogate");
}

void expect_types(
    const std::vector<std::string>& types, std::span<const std::string_view> expected
) {
    CHECK(types.size() == expected.size());
    if (types.size() != expected.size())
        return;
    for (std::size_t index = 0; index < types.size(); ++index)
        CHECK(types[index] == expected[index]);
}

void test_order() {
    const std::vector<uint8_t> rows = grey_rows();
    const TextChunk chunks[] = {
        {TextKind::latin1, "Comment", "ridge"},
        {TextKind::utf8, "Title", k_caption},
    };
    const Image image{grey_picture(), {}, rows, chunks};
    const std::string_view expected[] = {"IHDR", "tEXt", "iTXt", "IDAT", "IEND"};
    const bool forms[] = {false, true};
    for (const bool stored : forms) {
        std::vector<uint8_t> file;
        CHECK(encode(image, stored, &file));
        expect_types(chunk_types(file), expected);
    }

    const Rgb entry{9, 8, 7};
    const uint8_t index = 0;
    const TextChunk label{TextKind::latin1, "Source", "Skirmish"};
    const Image indexed{
        Header{1, 1, 8, ColorType::palette, Interlace::none},
        std::span(&entry, 1),
        std::span(&index, 1),
        std::span(&label, 1),
    };
    std::vector<uint8_t> indexed_file;
    CHECK(write(indexed, &indexed_file));
    const std::string_view indexed_types[] = {"IHDR", "PLTE", "tEXt", "IDAT", "IEND"};
    expect_types(chunk_types(indexed_file), indexed_types);

    std::vector<uint8_t> plain;
    CHECK(write(Image{grey_picture(), {}, rows}, &plain));
    const std::string_view plain_types[] = {"IHDR", "IDAT", "IEND"};
    expect_types(chunk_types(plain), plain_types);
}

void expect_digest(const char* name, std::span<const uint8_t> file, std::string_view expected) {
    const auto hex = sha256::to_hex(sha256::digest_of(file));
    const std::string_view got(hex.data(), hex.size());
    if (got != expected)
        std::fprintf(stderr, "%s digest %.*s\n", name, static_cast<int>(got.size()), got.data());
    CHECK(got == expected);
}

void test_digests() {
    const std::vector<uint8_t> rows = grey_rows();
    const TextChunk chunks[] = {
        {TextKind::latin1, "Comment", "ridge"},
        {TextKind::utf8, "Title", k_caption},
    };
    const Image plain{grey_picture(), {}, rows};
    const Image with_text{grey_picture(), {}, rows, chunks};
    std::vector<uint8_t> write_plain;
    std::vector<uint8_t> stored_plain;
    std::vector<uint8_t> write_texts;
    std::vector<uint8_t> stored_texts;
    CHECK(write(plain, &write_plain));
    CHECK(write_stored(plain, &stored_plain));
    CHECK(write(with_text, &write_texts));
    CHECK(write_stored(with_text, &stored_texts));
    expect_digest("write plain", write_plain, k_write_plain);
    expect_digest("stored plain", stored_plain, k_stored_plain);
    expect_digest("write texts", write_texts, k_write_texts);
    expect_digest("stored texts", stored_texts, k_stored_texts);
    CHECK(write_plain != write_texts);
    CHECK(stored_plain != stored_texts);
}

void test_skips_compressed() {
    const std::vector<uint8_t> rows = grey_rows();
    const TextChunk chunks[] = {
        {TextKind::latin1, "Comment", "ridge"},
        {TextKind::utf8, "Title", k_caption},
    };
    std::vector<uint8_t> file;
    CHECK(write(Image{grey_picture(), {}, rows, chunks}, &file));
    const std::vector<std::pair<std::string, std::vector<uint8_t>>> extra = {
        {"zTXt",
         {static_cast<uint8_t>('N'),
          static_cast<uint8_t>('o'),
          static_cast<uint8_t>('t'),
          static_cast<uint8_t>('e'),
          0,
          0,
          static_cast<uint8_t>('x')}},
        {"iTXt",
         {static_cast<uint8_t>('N'),
          static_cast<uint8_t>('o'),
          static_cast<uint8_t>('t'),
          static_cast<uint8_t>('e'),
          0,
          1,
          0,
          0,
          0,
          static_cast<uint8_t>('h'),
          static_cast<uint8_t>('i'),
          static_cast<uint8_t>('d'),
          static_cast<uint8_t>('d'),
          static_cast<uint8_t>('e'),
          static_cast<uint8_t>('n')}},
        {"iTXt",
         {static_cast<uint8_t>('T'),
          static_cast<uint8_t>('i'),
          static_cast<uint8_t>('t'),
          static_cast<uint8_t>('l'),
          static_cast<uint8_t>('e'),
          0,
          0,
          0,
          static_cast<uint8_t>('e'),
          static_cast<uint8_t>('n'),
          0,
          static_cast<uint8_t>('T'),
          static_cast<uint8_t>('i'),
          static_cast<uint8_t>('t'),
          static_cast<uint8_t>('r'),
          static_cast<uint8_t>('e'),
          0,
          static_cast<uint8_t>('B'),
          static_cast<uint8_t>('o'),
          static_cast<uint8_t>('n'),
          static_cast<uint8_t>('j'),
          static_cast<uint8_t>('o'),
          static_cast<uint8_t>('u'),
          static_cast<uint8_t>('r')}},
        {"iTXt",
         {static_cast<uint8_t>('S'),
          static_cast<uint8_t>('k'),
          static_cast<uint8_t>('i'),
          static_cast<uint8_t>('p'),
          0,
          0,
          1,
          0,
          0,
          static_cast<uint8_t>('n'),
          static_cast<uint8_t>('o'),
          static_cast<uint8_t>('p'),
          static_cast<uint8_t>('e')}},
    };
    const std::vector<uint8_t> mixed = insert_before_end(file, extra);
    std::vector<TextChunk> read;
    CHECK(read_texts(mixed, &read));
    // Flag 0 is uncompressed whatever the method byte is, so "Skip" is kept.
    // The flag-1 "Note" chunk and the zTXt chunk stay skipped.
    CHECK(read.size() == 4);
    if (read.size() == 4) {
        CHECK(same_chunk(read[0], chunks[0]));
        CHECK(same_chunk(read[1], chunks[1]));
        CHECK(read[2].kind == TextKind::utf8);
        CHECK(read[2].keyword == "Title");
        CHECK(read[2].text == "Bonjour");
        CHECK(read[3].kind == TextKind::utf8);
        CHECK(read[3].keyword == "Skip");
        CHECK(read[3].text == "nope");
    }

    std::vector<TextChunk> kept{{TextKind::latin1, "Keep", "me"}};
    const std::vector<uint8_t> broken = insert_before_end(file, {{"iTXt", {1, 2, 3, 4}}});
    CHECK(!read_texts(broken, &kept));
    CHECK(kept.size() == 1);
    CHECK(kept[0].keyword == "Keep");
    CHECK(kept[0].text == "me");

    std::vector<uint8_t> flipped = file;
    CHECK(flipped.size() > 40);
    flipped[40] = static_cast<uint8_t>(flipped[40] ^ 0xFF);
    CHECK(!read_texts(flipped, &kept));
    CHECK(kept.size() == 1);

    const std::vector<uint8_t> signature_only(k_signature, k_signature + k_signature_bytes);
    CHECK(!read_texts(signature_only, &kept));
    CHECK(!read_texts({}, &kept));
    CHECK(kept[0].text == "me");
}

/// A chunk header after IHDR that declares more bytes than the file holds.
void test_length_past_end() {
    const std::vector<uint8_t> rows = grey_rows();
    std::vector<uint8_t> file;
    CHECK(write(Image{grey_picture(), {}, rows}, &file));
    const uint32_t header_length = detail::read_be32(file.data() + k_signature_bytes);
    const std::size_t after_header = k_signature_bytes + detail::k_chunk_overhead + header_length;
    std::vector<uint8_t> huge(
        file.begin(), file.begin() + static_cast<std::ptrdiff_t>(after_header)
    );
    const std::size_t start = huge.size();
    huge.resize(start + detail::k_chunk_overhead);
    detail::write_be32(huge.data() + start, 0xFFFFFFF0u);
    std::memcpy(huge.data() + start + 4, "tEXt", 4);
    huge[start + 8] = static_cast<uint8_t>('a');
    huge[start + 9] = 0;
    huge[start + 10] = static_cast<uint8_t>('b');
    huge[start + 11] = 0;

    std::vector<TextChunk> kept{{TextKind::latin1, "Keep", "me"}};
    CHECK(!read_texts(huge, &kept));
    CHECK(kept.size() == 1);
    CHECK(kept[0].keyword == "Keep");
    CHECK(kept[0].text == "me");
}

/// Every prefix of the two-chunk picture, including a file with no end chunk.
void test_cut_short() {
    const std::vector<uint8_t> rows = grey_rows();
    const TextChunk chunks[] = {
        {TextKind::latin1, "Comment", "ridge"},
        {TextKind::utf8, "Title", k_caption},
    };
    std::vector<uint8_t> file;
    CHECK(write(Image{grey_picture(), {}, rows, chunks}, &file));
    CHECK(file.size() > 1);
    for (std::size_t prefix = 0; prefix < file.size(); ++prefix) {
        std::vector<TextChunk> kept{{TextKind::latin1, "Keep", "me"}};
        CHECK(!read_texts(std::span<const uint8_t>(file.data(), prefix), &kept));
        CHECK(kept.size() == 1);
        CHECK(kept[0].keyword == "Keep");
        CHECK(kept[0].text == "me");
    }
}

/// An unknown ancillary chunk and a tIME chunk between the two texts.
void test_unknown_chunks() {
    const std::vector<uint8_t> rows = grey_rows();
    const TextChunk chunks[] = {
        {TextKind::latin1, "Comment", "ridge"},
        {TextKind::utf8, "Title", k_caption},
    };
    std::vector<uint8_t> file;
    CHECK(write(Image{grey_picture(), {}, rows, chunks}, &file));
    const std::vector<uint8_t> with_unknown = insert_before_type(
        file,
        "iTXt",
        {
            {"ridG", {1, 2, 3}},
            {"tIME", {0x07, 0xEA, 11, 6, 12, 0, 0}},
        }
    );
    const std::string_view expected[] = {"IHDR", "tEXt", "ridG", "tIME", "iTXt", "IDAT", "IEND"};
    expect_types(chunk_types(with_unknown), expected);
    std::vector<TextChunk> read;
    CHECK(read_texts(with_unknown, &read));
    CHECK(read.size() == 2);
    if (read.size() == 2) {
        CHECK(same_chunk(read[0], chunks[0]));
        CHECK(same_chunk(read[1], chunks[1]));
    }
}

/// A tEXt chunk after the image data is returned after the earlier texts.
void test_text_after_image() {
    const std::vector<uint8_t> rows = grey_rows();
    const TextChunk chunks[] = {
        {TextKind::latin1, "Comment", "ridge"},
        {TextKind::utf8, "Title", k_caption},
    };
    std::vector<uint8_t> file;
    CHECK(write(Image{grey_picture(), {}, rows, chunks}, &file));
    std::vector<uint8_t> body = bytes_of("Source");
    body.push_back(0);
    const std::vector<uint8_t> text = bytes_of("after");
    body.insert(body.end(), text.begin(), text.end());
    const std::vector<uint8_t> late = insert_before_end(file, {{"tEXt", body}});
    const std::string_view expected[] = {"IHDR", "tEXt", "iTXt", "IDAT", "tEXt", "IEND"};
    expect_types(chunk_types(late), expected);
    std::vector<TextChunk> read;
    CHECK(read_texts(late, &read));
    CHECK(read.size() == 3);
    if (read.size() == 3) {
        CHECK(same_chunk(read[0], chunks[0]));
        CHECK(same_chunk(read[1], chunks[1]));
        CHECK(read[2].kind == TextKind::latin1);
        CHECK(read[2].keyword == "Source");
        CHECK(read[2].text == "after");
    }
}

} // namespace

int main() {
    test_round_trip();
    test_refusals();
    test_order();
    test_digests();
    test_skips_compressed();
    test_length_past_end();
    test_cut_short();
    test_unknown_chunks();
    test_text_after_image();
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("png-text-chunks: ok");
    return 0;
}
