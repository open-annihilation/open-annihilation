// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// PNG writer: per-row filter choice, Adam7 pass extraction and chunking of
// the deflated stream.

#include <cstring>
#include <string_view>
#include <vector>

#include <zlib.h>

#include "png_internal.hpp"

namespace oa::formats::png {

using namespace detail;

namespace {

// Deflated image data per IDAT chunk.
constexpr std::size_t k_idat_chunk_bytes = 0x2000;
// Largest filtered image the writer deflates in one call.
constexpr std::size_t k_max_filtered_bytes = 0x3fffffff;
// Bytes handed to one zlib crc32 call, which takes a 32-bit length.
constexpr std::size_t k_crc_piece_bytes = 0x40000000;

bool valid_depth(ColorType color_type, uint8_t depth) {
    switch (color_type) {
    case ColorType::grey:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8 || depth == 16;
    case ColorType::palette:
        return depth == 1 || depth == 2 || depth == 4 || depth == 8;
    case ColorType::rgb:
    case ColorType::grey_alpha:
    case ColorType::rgb_alpha:
        return depth == 8 || depth == 16;
    }
    return false;
}

bool valid_header(const Header& header) {
    return header.width >= 1 && header.width <= k_max_dimension && header.height >= 1 &&
           header.height <= k_max_dimension && valid_depth(header.color_type, header.bit_depth) &&
           (header.interlace == Interlace::none || header.interlace == Interlace::adam7);
}

void append_chunk(
    std::vector<uint8_t>& out, const char* type, const uint8_t* data, std::size_t size
) {
    const std::size_t start = out.size();
    out.resize(start + k_chunk_overhead + size);
    uint8_t* p = out.data() + start;
    write_be32(p, static_cast<uint32_t>(size));
    std::memcpy(p + k_chunk_length_bytes, type, k_chunk_type_bytes);
    if (size != 0)
        std::memcpy(p + k_chunk_length_bytes + k_chunk_type_bytes, data, size);
    write_be32(
        p + k_chunk_length_bytes + k_chunk_type_bytes + size,
        chunk_crc(p + k_chunk_length_bytes, k_chunk_type_bytes + size)
    );
}

// Printable Latin-1, as the PNG specification defines a text keyword:
// codes 32 to 126 and 161 to 255.
constexpr uint8_t k_keyword_space = 32;
constexpr uint8_t k_keyword_ascii_last = 126;
constexpr uint8_t k_keyword_latin1_first = 161;
constexpr std::size_t k_keyword_min_bytes = 1;
constexpr std::size_t k_keyword_max_bytes = 79;
// A chunk's data length is at most 2^31 - 1.
constexpr std::size_t k_max_chunk_bytes = 0x7fffffff;
// iTXt compression flag and method for text stored as UTF-8.
constexpr uint8_t k_text_uncompressed = 0;

/// Tells whether a byte may appear in a text keyword.
///
/// @param byte the byte
/// @return true for printable Latin-1
bool keyword_byte(uint8_t byte) {
    return (byte >= k_keyword_space && byte <= k_keyword_ascii_last) ||
           byte >= k_keyword_latin1_first;
}

/// Tells whether a keyword meets the PNG rules for tEXt and iTXt.
///
/// @param keyword the keyword
/// @return true when its length and bytes are allowed
bool keyword_allowed(std::string_view keyword) {
    if (keyword.size() < k_keyword_min_bytes || keyword.size() > k_keyword_max_bytes)
        return false;
    bool previous_space = false;
    for (std::size_t index = 0; index < keyword.size(); ++index) {
        const auto byte = static_cast<uint8_t>(keyword[index]);
        const bool space = byte == k_keyword_space;
        if (!keyword_byte(byte) || (space && (index == 0 || previous_space)))
            return false;
        previous_space = space;
    }
    return !previous_space;
}

/// Appends text bytes to a chunk body.
///
/// @param[in,out] body the chunk body
/// @param bytes the bytes
void append_bytes(std::vector<uint8_t>& body, std::string_view bytes) {
    body.reserve(body.size() + bytes.size());
    for (const char byte : bytes)
        body.push_back(static_cast<uint8_t>(byte));
}

/// Tells whether a text holds a NUL.
///
/// @param text the text
/// @return true when it does
bool holds_nul(std::string_view text) {
    return text.find('\0') != std::string_view::npos;
}

// A well-formed UTF-8 sequence, and nothing above U+10FFFF. A NUL is refused
// with the other bytes that are not a character the chunk may store.
constexpr uint32_t k_utf8_two_byte_minimum = 0x80;
constexpr uint32_t k_utf8_three_byte_minimum = 0x800;
constexpr uint32_t k_utf8_four_byte_minimum = 0x10000;
constexpr uint32_t k_last_code_point = 0x10FFFF;
constexpr uint32_t k_first_surrogate = 0xD800;
constexpr uint32_t k_last_surrogate = 0xDFFF;
constexpr uint8_t k_utf8_continuation_mask = 0xC0;
constexpr uint8_t k_utf8_continuation_bits = 0x80;
constexpr uint8_t k_utf8_payload_mask = 0x3F;

/// Measures one UTF-8 character, refusing a NUL, a cut-short or overlong
/// sequence, a surrogate and a code point past U+10FFFF.
///
/// @param text the text, from the character's first byte
/// @return the character's length in bytes, or 0 when it is not allowed
std::size_t utf8_character_length(std::string_view text) {
    if (text.empty())
        return 0;
    const auto first = static_cast<uint8_t>(text[0]);
    if (first == 0)
        return 0;
    if (first < k_utf8_two_byte_minimum)
        return 1;
    std::size_t length = 0;
    uint32_t code_point = 0;
    uint32_t smallest = 0;
    if ((first & 0xE0) == 0xC0) {
        length = 2;
        code_point = first & 0x1FU;
        smallest = k_utf8_two_byte_minimum;
    } else if ((first & 0xF0) == 0xE0) {
        length = 3;
        code_point = first & 0x0FU;
        smallest = k_utf8_three_byte_minimum;
    } else if ((first & 0xF8) == 0xF0) {
        length = 4;
        code_point = first & 0x07U;
        smallest = k_utf8_four_byte_minimum;
    } else {
        return 0;
    }
    if (text.size() < length)
        return 0;
    for (std::size_t index = 1; index < length; ++index) {
        const auto byte = static_cast<uint8_t>(text[index]);
        if ((byte & k_utf8_continuation_mask) != k_utf8_continuation_bits)
            return 0;
        code_point = (code_point << 6) | (byte & k_utf8_payload_mask);
    }
    if (code_point < smallest || code_point > k_last_code_point ||
        (code_point >= k_first_surrogate && code_point <= k_last_surrogate))
        return 0;
    return length;
}

/// Tells whether an iTXt text is UTF-8 with no NUL.
///
/// @param text the text
/// @return true when every byte belongs to such a character
bool utf8_text(std::string_view text) {
    for (std::size_t at = 0; at < text.size();) {
        const std::size_t length = utf8_character_length(text.substr(at));
        if (length == 0)
            return false;
        at += length;
    }
    return true;
}

// One text chunk ready to append: its type and its body.
struct PreparedChunk {
    const char* type = nullptr;
    std::vector<uint8_t> body;
};

/// Builds the body of one text chunk.
///
/// @param chunk the chunk
/// @param[out] prepared its type and body
/// @return false when the keyword or the text breaks a rule, or the body
///         exceeds a chunk's maximum length
bool prepare_one_text(const TextChunk& chunk, PreparedChunk* prepared) {
    if (!keyword_allowed(chunk.keyword))
        return false;
    PreparedChunk built;
    switch (chunk.kind) {
    case TextKind::latin1:
        if (holds_nul(chunk.text))
            return false;
        built.type = "tEXt";
        append_bytes(built.body, chunk.keyword);
        built.body.push_back(0);
        append_bytes(built.body, chunk.text);
        break;
    case TextKind::utf8:
        if (!utf8_text(chunk.text))
            return false;
        built.type = "iTXt";
        append_bytes(built.body, chunk.keyword);
        built.body.push_back(0);
        built.body.push_back(k_text_uncompressed);
        built.body.push_back(k_text_uncompressed);
        built.body.push_back(0);
        built.body.push_back(0);
        append_bytes(built.body, chunk.text);
        break;
    }
    if (built.type == nullptr || built.body.size() > k_max_chunk_bytes)
        return false;
    *prepared = std::move(built);
    return true;
}

/// Checks every text chunk and builds the bodies write_file appends.
///
/// @param texts the chunks, in the order they are written
/// @param[out] prepared one entry per chunk
/// @return false when any chunk breaks a rule
bool prepare_text_chunks(std::span<const TextChunk> texts, std::vector<PreparedChunk>* prepared) {
    std::vector<PreparedChunk> built;
    built.reserve(texts.size());
    for (const TextChunk& chunk : texts) {
        PreparedChunk one;
        if (!prepare_one_text(chunk, &one))
            return false;
        built.push_back(std::move(one));
    }
    *prepared = std::move(built);
    return true;
}

// Filters `row` against `previous` with one filter type into `out`.
void apply_filter(
    Filter filter,
    const uint8_t* row,
    const uint8_t* previous,
    std::size_t size,
    std::size_t stride,
    uint8_t* out
) {
    if (filter == Filter::none) {
        std::memcpy(out, row, size);
        return;
    }
    for (std::size_t i = 0; i < size; ++i) {
        const int32_t left = i >= stride ? row[i - stride] : 0;
        const int32_t up = previous[i];
        const int32_t up_left = i >= stride ? previous[i - stride] : 0;
        int32_t predictor = 0;
        switch (filter) {
        case Filter::none:
            break;
        case Filter::sub:
            predictor = left;
            break;
        case Filter::up:
            predictor = up;
            break;
        case Filter::average:
            predictor = (left + up) / 2;
            break;
        case Filter::paeth:
            predictor = paeth_predictor(left, up, up_left);
            break;
        }
        out[i] = static_cast<uint8_t>(row[i] - predictor);
    }
}

// Sum of the filtered bytes read as signed values; smaller usually deflates better.
uint64_t filter_cost(const uint8_t* filtered, std::size_t size) {
    uint64_t cost = 0;
    for (std::size_t i = 0; i < size; ++i)
        cost += filtered[i] < 0x80 ? filtered[i] : 0x100u - filtered[i];
    return cost;
}

// Appends one filtered scanline: palette and sub-byte images, and every row
// of a stored file, are left unfiltered, others take the cheapest filter.
void append_scanline(
    const Header& header,
    bool stored,
    const uint8_t* row,
    const uint8_t* previous,
    std::size_t size,
    std::vector<uint8_t>& candidate,
    std::vector<uint8_t>& out
) {
    const std::size_t stride = filter_stride(header);
    Filter best = Filter::none;
    if (!stored && header.color_type != ColorType::palette && header.bit_depth >= 8) {
        uint64_t best_cost = UINT64_MAX;
        for (uint8_t f = 0; f < k_filter_count; ++f) {
            apply_filter(static_cast<Filter>(f), row, previous, size, stride, candidate.data());
            const uint64_t cost = filter_cost(candidate.data(), size);
            if (cost < best_cost) {
                best_cost = cost;
                best = static_cast<Filter>(f);
            }
        }
    }
    out.push_back(static_cast<uint8_t>(best));
    const std::size_t start = out.size();
    out.resize(start + size);
    apply_filter(best, row, previous, size, stride, out.data() + start);
}

// Copies the pixels of one pass row out of the full image row.
void gather_row(
    const uint8_t* image_row, const Pass& pass, uint32_t count, uint32_t bits, uint8_t* out
) {
    if (pass.dx == 1) {
        std::memcpy(out, image_row, packed_bytes(count, bits));
        return;
    }
    if (bits >= 8) {
        const uint32_t bytes = bits / 8;
        for (uint32_t i = 0; i < count; ++i)
            std::memcpy(
                out + static_cast<std::size_t>(i) * bytes,
                image_row +
                    (static_cast<std::size_t>(pass.x0) + static_cast<std::size_t>(i) * pass.dx) *
                        bytes,
                bytes
            );
        return;
    }
    std::memset(out, 0, packed_bytes(count, bits));
    const uint32_t mask = (1u << bits) - 1;
    for (uint32_t i = 0; i < count; ++i) {
        const uint64_t from = (pass.x0 + static_cast<uint64_t>(i) * pass.dx) * bits;
        const uint32_t value = (image_row[from / 8] >> (8 - bits - from % 8)) & mask;
        const uint64_t to = static_cast<uint64_t>(i) * bits;
        out[to / 8] = static_cast<uint8_t>(out[to / 8] | (value << (8 - bits - to % 8)));
    }
}

// Encodes an image as a PNG file: filtered and compressed as tightly as zlib
// can, or, stored, unfiltered and in deflate's uncompressed blocks.
bool write_file(const Image& image, bool stored, std::vector<uint8_t>* out);

} // namespace

uint32_t detail::crc_update(uint32_t crc, const uint8_t* data, std::size_t size) {
    uLong value = crc;
    while (size > 0) {
        const auto piece = static_cast<uInt>(size > k_crc_piece_bytes ? k_crc_piece_bytes : size);
        value = crc32(value, data, piece);
        data += piece;
        size -= piece;
    }
    return static_cast<uint32_t>(value);
}

uint32_t detail::chunk_crc(const uint8_t* type_and_data, std::size_t size) {
    return crc_update(static_cast<uint32_t>(crc32(0L, Z_NULL, 0)), type_and_data, size);
}

bool write(const Image& image, std::vector<uint8_t>* out) {
    return write_file(image, false, out);
}

bool write_stored(const Image& image, std::vector<uint8_t>* out) {
    return write_file(image, true, out);
}

namespace {

bool write_file(const Image& image, bool stored, std::vector<uint8_t>* out) {
    const Header& header = image.header;
    if (!valid_header(header))
        return false;
    if (header.color_type == ColorType::palette &&
        (image.palette.empty() || image.palette.size() > k_max_palette_entries))
        return false;
    const std::size_t row = row_bytes(header, {});
    if (row == 0 || image.rows.size() / row != header.height || image.rows.size() % row != 0)
        return false;
    std::vector<PreparedChunk> text_chunks;
    if (!prepare_text_chunks(image.texts, &text_chunks))
        return false;

    const bool interlaced = header.interlace == Interlace::adam7;
    const uint32_t pass_count = interlaced ? k_adam7_pass_count : 1;
    const uint32_t bits = bits_per_pixel(header);
    std::vector<uint8_t> filtered;
    std::vector<uint8_t> current(row);
    std::vector<uint8_t> previous(row);
    std::vector<uint8_t> candidate(row);
    for (uint32_t p = 0; p < pass_count; ++p) {
        const Pass& pass = interlaced ? k_adam7_passes[p] : k_whole_image;
        const uint32_t pass_width = pass_extent(header.width, pass.x0, pass.dx);
        const uint32_t pass_height = pass_extent(header.height, pass.y0, pass.dy);
        if (pass_width == 0 || pass_height == 0)
            continue;
        const std::size_t pass_bytes = packed_bytes(pass_width, bits);
        if (filtered.size() + (pass_bytes + 1) * pass_height > k_max_filtered_bytes)
            return false;
        std::memset(previous.data(), 0, pass_bytes);
        for (uint32_t y = 0; y < pass_height; ++y) {
            const std::size_t image_y = pass.y0 + static_cast<std::size_t>(y) * pass.dy;
            gather_row(image.rows.data() + image_y * row, pass, pass_width, bits, current.data());
            append_scanline(
                header, stored, current.data(), previous.data(), pass_bytes, candidate, filtered
            );
            std::memcpy(previous.data(), current.data(), pass_bytes);
        }
    }

    uLongf packed_size = compressBound(static_cast<uLong>(filtered.size()));
    std::vector<uint8_t> packed(packed_size);
    if (compress2(
            packed.data(),
            &packed_size,
            filtered.data(),
            static_cast<uLong>(filtered.size()),
            stored ? Z_NO_COMPRESSION : Z_BEST_COMPRESSION
        ) != Z_OK)
        return false;

    out->assign(k_signature, k_signature + k_signature_bytes);
    uint8_t ihdr[k_ihdr_bytes] = {};
    write_be32(ihdr + k_ihdr_width, header.width);
    write_be32(ihdr + k_ihdr_height, header.height);
    ihdr[k_ihdr_bit_depth] = header.bit_depth;
    ihdr[k_ihdr_color_type] = static_cast<uint8_t>(header.color_type);
    ihdr[k_ihdr_compression] = k_compression_deflate;
    ihdr[k_ihdr_filter] = k_filter_method_adaptive;
    ihdr[k_ihdr_interlace] = static_cast<uint8_t>(header.interlace);
    append_chunk(*out, "IHDR", ihdr, sizeof ihdr);
    if (header.color_type == ColorType::palette) {
        std::vector<uint8_t> entries;
        for (const Rgb& entry : image.palette) {
            entries.push_back(entry.r);
            entries.push_back(entry.g);
            entries.push_back(entry.b);
        }
        append_chunk(*out, "PLTE", entries.data(), entries.size());
    }
    for (const PreparedChunk& text_chunk : text_chunks)
        append_chunk(*out, text_chunk.type, text_chunk.body.data(), text_chunk.body.size());
    for (std::size_t offset = 0; offset < packed_size; offset += k_idat_chunk_bytes) {
        const std::size_t size =
            packed_size - offset < k_idat_chunk_bytes ? packed_size - offset : k_idat_chunk_bytes;
        append_chunk(*out, "IDAT", packed.data() + offset, size);
    }
    append_chunk(*out, "IEND", nullptr, 0);
    return true;
}

} // namespace

} // namespace oa::formats::png
