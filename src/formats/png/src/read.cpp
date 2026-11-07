// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// PNG reader: the chunk grammar, the IDAT stream, row filters, Adam7
// placement and the strip-16 / unpack row transforms. read_texts walks the
// chunks on its own for tEXt and uncompressed iTXt; it does not use the
// checks or messages below.
//
// The file is read as a stream: a chunk's length and type, then its data in
// the pieces each handler asks for, then its CRC. The chunk rules, the order
// they are checked in and the reported text match 3.1c, so a file that is
// cut short or malformed stops at the same point with the same message. Rows
// are written as soon as they decode, so those decoded before an error remain.

#include <cstdio>
#include <cstring>
#include <string_view>
#include <vector>

#include <zlib.h>

#include "png_internal.hpp"

namespace oa::formats::png {

using namespace detail;

namespace {

// Compressed data is handed to zlib in pieces of at most this many bytes,
// which decides how many rows a truncated IDAT still yields.
constexpr std::size_t k_read_piece_bytes = 0x2000;
constexpr std::size_t k_formatted_message_bytes = 64;

// Reader::sequence bits: the chunks read so far.
constexpr uint8_t k_seen_header = 0x01;
constexpr uint8_t k_seen_palette = 0x02;
constexpr uint8_t k_seen_image_data = 0x04;
constexpr uint8_t k_after_image_data = 0x08;
constexpr uint8_t k_seen_end = 0x10;

// Record::accepted bits: chunks whose content a record holds.
constexpr uint32_t k_accepted_gamma = 0x001;
constexpr uint32_t k_accepted_significant_bits = 0x002;
constexpr uint32_t k_accepted_chromaticity = 0x004;
constexpr uint32_t k_accepted_palette = 0x008;
constexpr uint32_t k_accepted_transparency = 0x010;
constexpr uint32_t k_accepted_background = 0x020;
constexpr uint32_t k_accepted_histogram = 0x040;
constexpr uint32_t k_accepted_physical_size = 0x080;
constexpr uint32_t k_accepted_offset = 0x100;
constexpr uint32_t k_accepted_time = 0x200;
constexpr uint32_t k_accepted_calibration = 0x400;
constexpr uint32_t k_accepted_srgb = 0x800;

// Ancillary chunk layouts.
constexpr uint32_t k_palette_entry_bytes = 3;
constexpr uint32_t k_gamma_bytes = 4;
constexpr uint32_t k_chromaticity_bytes = 32;
constexpr uint32_t k_chromaticity_point_bytes = 8;
constexpr uint32_t k_srgb_bytes = 1;
constexpr uint8_t k_srgb_max_intent = 3;
constexpr uint32_t k_rgb_transparency_bytes = 6;
constexpr uint32_t k_grey_transparency_bytes = 2;
constexpr uint32_t k_physical_size_bytes = 9;
constexpr uint32_t k_offset_bytes = 9;
constexpr uint32_t k_time_bytes = 7;
constexpr uint32_t k_histogram_entry_bytes = 2;
constexpr uint32_t k_palette_significant_bits_bytes = 3;
constexpr uint32_t k_palette_background_bytes = 1;
constexpr uint32_t k_grey_background_bytes = 2;
constexpr uint32_t k_rgb_background_bytes = 6;
// pCAL, from the purpose's NUL: X0 at +1, X1 at +5, then the equation type,
// the parameter count and the units string. The chunk must run more than
// k_calibration_min_tail bytes past that NUL.
constexpr std::size_t k_calibration_equation = 9;
constexpr std::size_t k_calibration_parameters = 10;
constexpr std::size_t k_calibration_units = 11;
constexpr std::size_t k_calibration_min_tail = 12;
constexpr uint8_t k_calibration_equation_count = 4;
// Parameters each pCAL equation type takes.
constexpr uint8_t k_calibration_parameter_counts[k_calibration_equation_count] = {2, 3, 3, 4};
constexpr uint8_t k_text_compression_deflate = 0;

// Fixed-point chunk values are hundred-thousandths.
constexpr float k_fixed_point_scale = 1e-05f;
constexpr double k_fixed_point_units = 100000.0;
// sRGB's gamma as a gAMA value, and as set when sRGB is accepted.
constexpr uint32_t k_srgb_gamma_fixed = 45000;
constexpr float k_srgb_gamma = 0.45f;
// A chromaticity coordinate above this, or a point whose coordinates sum
// above one, is invalid.
constexpr double k_max_chromaticity = 0.8;
// Distance from the sRGB chromaticities below which a cHRM agrees with sRGB.
constexpr double k_chromaticity_tolerance = 0.001;

struct ChromaticityPoint {
    float x{};
    float y{};
};

struct Chromaticity {
    ChromaticityPoint white{};
    ChromaticityPoint red{};
    ChromaticityPoint green{};
    ChromaticityPoint blue{};
};

constexpr Chromaticity k_srgb_chromaticity = {
    {0.3127f, 0.329f}, {0.64f, 0.33f}, {0.3f, 0.6f}, {0.15f, 0.06f}
};

// What the reader keeps of the chunks it accepted into one image record: the
// one before the image data, or the fresh one read_image fills after it.
struct Record {
    uint32_t accepted{}; // k_accepted_* bits
    float gamma{};
    Chromaticity chromaticity{};
    uint16_t palette_entries{};
    Rgb palette[k_max_palette_entries]{};
};

struct Reader {
    std::span<const uint8_t> file;
    std::size_t position{};
    const Messages* messages{};
    uint8_t chunk_type[k_chunk_type_bytes]{};
    uint32_t crc{};
    uint8_t sequence{}; // k_seen_* bits
    Header header{};
    uint16_t palette_entries{};      // of the accepted PLTE
    uint16_t transparency_entries{}; // of the last tRNS read
    float gamma{};                   // of the last gAMA accepted
    uint32_t image_data_left{};      // bytes of the current IDAT not yet read
    z_stream z{};
    bool z_open{};
    bool stream_ended{};

    Reader() = default;
    Reader(const Reader&) = delete;
    Reader& operator=(const Reader&) = delete;

    ~Reader() {
        if (z_open)
            inflateEnd(&z);
    }
};

void warn(const Reader& r, const char* text) {
    if (r.messages->warning != nullptr)
        r.messages->warning(r.messages->user, text);
}

// Reports an error; returns false so callers can stop with `return fail(...)`.
bool fail(const Reader& r, const char* text) {
    if (r.messages->error != nullptr)
        r.messages->error(r.messages->user, text);
    return false;
}

void warn_chunk(const Reader& r, const char* text) {
    char message[k_chunk_message_bytes];
    format_chunk_message(r.chunk_type, text, message);
    warn(r, message);
}

bool fail_chunk(const Reader& r, const char* text) {
    char message[k_chunk_message_bytes];
    format_chunk_message(r.chunk_type, text, message);
    return fail(r, message);
}

bool printable_type_byte(uint8_t b) {
    return (b >= ')' && b <= 'Z') || (b >= 'a' && b <= 'z');
}

bool type_is(const Reader& r, const char* type) {
    return std::memcmp(r.chunk_type, type, k_chunk_type_bytes) == 0;
}

bool is_critical(const Reader& r) {
    return (r.chunk_type[0] & k_ancillary_bit) == 0;
}

// Takes the next `size` bytes of the file; a short file is a read error.
bool read_bytes(Reader& r, std::size_t size, const uint8_t** out) {
    if (r.position > r.file.size() || r.file.size() - r.position < size) {
        r.position = r.file.size();
        return fail(r, "Read Error");
    }
    *out = r.file.data() + r.position;
    r.position += size;
    return true;
}

// Takes chunk data, adding it to the chunk's CRC.
bool read_chunk_data(Reader& r, std::size_t size, const uint8_t** out) {
    if (!read_bytes(r, size, out))
        return false;
    r.crc = crc_update(r.crc, *out, size);
    return true;
}

// Reads a chunk's length and type and starts its CRC.
bool read_chunk_header(Reader& r, uint32_t* length) {
    const uint8_t* bytes = nullptr;
    if (!read_bytes(r, k_chunk_length_bytes, &bytes))
        return false;
    *length = read_be32(bytes);
    r.crc = static_cast<uint32_t>(crc32(0L, Z_NULL, 0));
    if (!read_chunk_data(r, k_chunk_type_bytes, &bytes))
        return false;
    std::memcpy(r.chunk_type, bytes, k_chunk_type_bytes);
    return true;
}

enum class ChunkEnd : uint8_t {
    kept,
    dropped, // an ancillary chunk whose CRC did not match
    failed,
};

// Reads the rest of the chunk's data (`skip` bytes) and its CRC. A mismatch
// stops reading on a critical chunk and drops an ancillary one.
ChunkEnd finish_chunk(Reader& r, std::size_t skip) {
    const uint8_t* bytes = nullptr;
    if (!read_chunk_data(r, skip, &bytes) || !read_bytes(r, k_chunk_crc_bytes, &bytes))
        return ChunkEnd::failed;
    if (read_be32(bytes) == r.crc)
        return ChunkEnd::kept;
    if (is_critical(r)) {
        fail_chunk(r, "CRC error");
        return ChunkEnd::failed;
    }
    warn_chunk(r, "CRC error");
    return ChunkEnd::dropped;
}

bool skip_chunk(Reader& r, std::size_t length) {
    return finish_chunk(r, length) != ChunkEnd::failed;
}

// Ignores a chunk with a warning.
bool ignore_chunk(Reader& r, uint32_t length, const char* warning) {
    warn(r, warning);
    return skip_chunk(r, length);
}

// Stores a finished chunk's content unless its CRC dropped it.
bool finish_and_accept(Reader& r, Record& record, uint32_t bit) {
    const ChunkEnd end = finish_chunk(r, 0);
    if (end == ChunkEnd::kept)
        record.accepted |= bit;
    return end != ChunkEnd::failed;
}

float fixed_point_value(uint32_t value) {
    return static_cast<float>(
        static_cast<double>(value) * static_cast<double>(k_fixed_point_scale)
    );
}

bool differs_from_srgb(const Chromaticity& c) {
    const ChromaticityPoint points[] = {c.white, c.red, c.green, c.blue};
    const ChromaticityPoint srgb[] = {
        k_srgb_chromaticity.white,
        k_srgb_chromaticity.red,
        k_srgb_chromaticity.green,
        k_srgb_chromaticity.blue,
    };
    for (std::size_t i = 0; i < 4; ++i) {
        const double dx = static_cast<double>(points[i].x) - static_cast<double>(srgb[i].x);
        const double dy = static_cast<double>(points[i].y) - static_cast<double>(srgb[i].y);
        if ((dx < 0 ? -dx : dx) > k_chromaticity_tolerance ||
            (dy < 0 ? -dy : dy) > k_chromaticity_tolerance)
            return true;
    }
    return false;
}

// ---- critical chunks --------------------------------------------------------

bool handle_header(Reader& r, uint32_t length) {
    if (r.sequence != 0)
        return fail(r, "Out of place IHDR");
    if (length != k_ihdr_bytes)
        return fail(r, "Invalid IHDR chunk");
    r.sequence |= k_seen_header;
    const uint8_t* d = nullptr;
    if (!read_chunk_data(r, k_ihdr_bytes, &d) || finish_chunk(r, 0) == ChunkEnd::failed)
        return false;
    const uint32_t width = read_be32(d + k_ihdr_width);
    const uint32_t height = read_be32(d + k_ihdr_height);
    const uint8_t depth = d[k_ihdr_bit_depth];
    const uint8_t color = d[k_ihdr_color_type];
    if (width == 0 || width > k_max_dimension || height == 0 || height > k_max_dimension)
        return fail(r, "Invalid image size in IHDR");
    if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16)
        return fail(r, "Invalid bit depth in IHDR");
    const auto type = static_cast<ColorType>(color);
    if (type != ColorType::grey && type != ColorType::rgb && type != ColorType::palette &&
        type != ColorType::grey_alpha && type != ColorType::rgb_alpha)
        return fail(r, "Invalid color type in IHDR");
    // A palette image of depth 16 passes.
    if ((type == ColorType::rgb || type == ColorType::grey_alpha || type == ColorType::rgb_alpha) &&
        depth < 8)
        return fail(r, "Invalid color type/bit depth combination in IHDR");
    if (d[k_ihdr_interlace] > static_cast<uint8_t>(Interlace::adam7))
        return fail(r, "Unknown interlace method in IHDR");
    if (d[k_ihdr_compression] != k_compression_deflate)
        return fail(r, "Unknown compression method in IHDR");
    if (d[k_ihdr_filter] != k_filter_method_adaptive)
        return fail(r, "Unknown filter method in IHDR");
    r.header = Header{width, height, depth, type, static_cast<Interlace>(d[k_ihdr_interlace])};
    const auto pixel_bits = static_cast<uint8_t>(bits_per_pixel(r.header));
    if (k_max_dimension / ((pixel_bits + 7u) >> 3) < width)
        warn(r, "Width too large to process image data; rowbytes will overflow.");
    return true;
}

bool handle_palette(Reader& r, Record& record, uint32_t length) {
    if ((r.sequence & k_seen_header) == 0)
        return fail(r, "Missing IHDR before PLTE");
    if ((r.sequence & k_seen_image_data) != 0)
        return ignore_chunk(r, length, "Invalid PLTE after IDAT");
    if ((r.sequence & k_seen_palette) != 0)
        return fail(r, "Duplicate PLTE chunk");
    // Seen even when the length is then refused.
    r.sequence |= k_seen_palette;
    if (length % k_palette_entry_bytes != 0) {
        if (r.header.color_type != ColorType::palette)
            return ignore_chunk(r, length, "Invalid palette chunk");
        return fail(r, "Invalid palette chunk");
    }
    const uint32_t entries = length / k_palette_entry_bytes;
    const uint8_t* data = nullptr;
    if (!read_chunk_data(r, length, &data) || finish_chunk(r, 0) == ChunkEnd::failed)
        return false;
    r.palette_entries = static_cast<uint16_t>(entries);
    record.palette_entries = r.palette_entries;
    for (uint32_t i = 0; i < entries && i < k_max_palette_entries; ++i) {
        const uint8_t* entry = data + i * k_palette_entry_bytes;
        record.palette[i] = Rgb{entry[0], entry[1], entry[2]};
    }
    record.accepted |= k_accepted_palette;
    if (r.header.color_type == ColorType::palette &&
        (record.accepted & k_accepted_transparency) != 0 &&
        r.palette_entries < r.transparency_entries) {
        warn(r, "Truncating incorrect tRNS chunk length");
        r.transparency_entries = r.palette_entries;
    }
    return true;
}

bool handle_end(Reader& r, uint32_t length) {
    if ((r.sequence & k_seen_header) == 0 || (r.sequence & k_seen_image_data) == 0)
        return fail(r, "No image in file");
    r.sequence |= k_after_image_data | k_seen_end;
    if (length != 0)
        warn(r, "Incorrect IEND chunk length");
    return skip_chunk(r, length);
}

// ---- ancillary chunks -------------------------------------------------------

bool handle_gamma(Reader& r, Record& record, uint32_t length) {
    if ((r.sequence & k_seen_header) == 0)
        return fail(r, "Missing IHDR before gAMA");
    if ((r.sequence & k_seen_image_data) != 0)
        return ignore_chunk(r, length, "Invalid gAMA after IDAT");
    if ((r.sequence & k_seen_palette) != 0)
        warn(r, "Out of place gAMA chunk");
    else if ((record.accepted & k_accepted_gamma) != 0 && (record.accepted & k_accepted_srgb) == 0)
        return ignore_chunk(r, length, "Duplicate gAMA chunk");
    if (length != k_gamma_bytes)
        return ignore_chunk(r, length, "Incorrect gAMA chunk length");
    const uint8_t* data = nullptr;
    if (!read_chunk_data(r, k_gamma_bytes, &data))
        return false;
    const ChunkEnd end = finish_chunk(r, 0);
    if (end != ChunkEnd::kept)
        return end != ChunkEnd::failed;
    const uint32_t value = read_be32(data);
    if (value == 0)
        return true;
    if ((record.accepted & k_accepted_srgb) != 0 && value != k_srgb_gamma_fixed) {
        warn(r, "Ignoring incorrect gAMA value when sRGB is also present");
        return true;
    }
    r.gamma = fixed_point_value(value);
    record.gamma = r.gamma;
    record.accepted |= k_accepted_gamma;
    return true;
}

bool handle_significant_bits(Reader& r, Record& record, uint32_t length) {
    if ((r.sequence & k_seen_header) == 0)
        return fail(r, "Missing IHDR before sBIT");
    if ((r.sequence & k_seen_image_data) != 0)
        return ignore_chunk(r, length, "Invalid sBIT after IDAT");
    if ((r.sequence & k_seen_palette) != 0)
        warn(r, "Out of place sBIT chunk");
    else if ((record.accepted & k_accepted_significant_bits) != 0)
        return ignore_chunk(r, length, "Duplicate sBIT chunk");
    const uint32_t expected = r.header.color_type == ColorType::palette
                                  ? k_palette_significant_bits_bytes
                                  : channel_count(r.header.color_type);
    if (length != expected)
        return ignore_chunk(r, length, "Incorrect sBIT chunk length");
    const uint8_t* data = nullptr;
    return read_chunk_data(r, expected, &data) &&
           finish_and_accept(r, record, k_accepted_significant_bits);
}

bool valid_point(const ChromaticityPoint& p) {
    const double x = p.x;
    const double y = p.y;
    return !(x < 0.0 || x > k_max_chromaticity || y < 0.0 || y > k_max_chromaticity || y + x > 1.0);
}

bool handle_chromaticity(Reader& r, Record& record, uint32_t length) {
    // A cHRM before IHDR is reported with the sBIT message, as 3.1c reports it.
    if ((r.sequence & k_seen_header) == 0)
        return fail(r, "Missing IHDR before sBIT");
    if ((r.sequence & k_seen_image_data) != 0)
        return ignore_chunk(r, length, "Invalid cHRM after IDAT");
    // Its warning for a cHRM that follows PLTE reads as if PLTE were missing.
    if ((r.sequence & k_seen_palette) != 0)
        warn(r, "Missing PLTE before cHRM");
    else if (
        (record.accepted & k_accepted_chromaticity) != 0 && (record.accepted & k_accepted_srgb) == 0
    )
        return ignore_chunk(r, length, "Duplicate cHRM chunk");
    if (length != k_chromaticity_bytes)
        return ignore_chunk(r, length, "Incorrect cHRM chunk length");
    Chromaticity c{};
    ChromaticityPoint* points[] = {&c.white, &c.red, &c.green, &c.blue};
    const char* invalid[] = {
        "Invalid cHRM white point",
        "Invalid cHRM red point",
        "Invalid cHRM green point",
        "Invalid cHRM blue point",
    };
    uint32_t left = k_chromaticity_bytes;
    for (std::size_t i = 0; i < 4; ++i) {
        const uint8_t* data = nullptr;
        if (!read_chunk_data(r, k_chromaticity_point_bytes, &data))
            return false;
        left -= k_chromaticity_point_bytes;
        *points[i] = ChromaticityPoint{
            fixed_point_value(read_be32(data)), fixed_point_value(read_be32(data + 4))
        };
        if (!valid_point(*points[i])) {
            warn(r, invalid[i]);
            return skip_chunk(r, left);
        }
    }
    const ChunkEnd end = finish_chunk(r, 0);
    if (end != ChunkEnd::kept)
        return end != ChunkEnd::failed;
    // Under sRGB the chunk is never stored.
    if ((record.accepted & k_accepted_srgb) != 0) {
        if (differs_from_srgb(c))
            warn(r, "Ignoring incorrect cHRM value when sRGB is also present");
        return true;
    }
    record.chromaticity = c;
    record.accepted |= k_accepted_chromaticity;
    return true;
}

bool handle_srgb(Reader& r, Record& record, uint32_t length) {
    if ((r.sequence & k_seen_header) == 0)
        return fail(r, "Missing IHDR before sRGB");
    if ((r.sequence & k_seen_image_data) != 0)
        return ignore_chunk(r, length, "Invalid sRGB after IDAT");
    if ((r.sequence & k_seen_palette) != 0)
        warn(r, "Out of place sRGB chunk");
    else if ((record.accepted & k_accepted_srgb) != 0)
        return ignore_chunk(r, length, "Duplicate sRGB chunk");
    if (length != k_srgb_bytes)
        return ignore_chunk(r, length, "Incorrect sRGB chunk length");
    const uint8_t* data = nullptr;
    if (!read_chunk_data(r, k_srgb_bytes, &data))
        return false;
    const ChunkEnd end = finish_chunk(r, 0);
    if (end != ChunkEnd::kept)
        return end != ChunkEnd::failed;
    if (data[0] > k_srgb_max_intent) {
        warn(r, "Unknown sRGB intent");
        return true;
    }
    if ((record.accepted & k_accepted_gamma) != 0) {
        const auto fixed =
            static_cast<int64_t>(static_cast<double>(r.gamma) * k_fixed_point_units + 0.5);
        if (fixed != k_srgb_gamma_fixed)
            warn(r, "Ignoring incorrect gAMA value when sRGB is also present");
    }
    if ((record.accepted & k_accepted_chromaticity) != 0 && differs_from_srgb(record.chromaticity))
        warn(r, "Ignoring incorrect cHRM value when sRGB is also present");
    // Accepting sRGB also sets its gamma and chromaticities.
    record.gamma = k_srgb_gamma;
    record.chromaticity = k_srgb_chromaticity;
    record.accepted |= k_accepted_srgb | k_accepted_gamma | k_accepted_chromaticity;
    return true;
}

bool handle_transparency(Reader& r, Record& record, uint32_t length) {
    if ((r.sequence & k_seen_header) == 0)
        return fail(r, "Missing IHDR before tRNS");
    if ((r.sequence & k_seen_image_data) != 0)
        return ignore_chunk(r, length, "Invalid tRNS after IDAT");
    if ((record.accepted & k_accepted_transparency) != 0)
        return ignore_chunk(r, length, "Duplicate tRNS chunk");
    const uint8_t* data = nullptr;
    switch (r.header.color_type) {
    case ColorType::palette:
        // Without a PLTE the entries are read regardless.
        if ((r.sequence & k_seen_palette) == 0)
            warn(r, "Missing PLTE before tRNS");
        else if (r.palette_entries < length)
            return ignore_chunk(r, length, "Incorrect tRNS chunk length");
        if (length == 0) {
            warn(r, "Zero length tRNS chunk");
            return skip_chunk(r, 0);
        }
        if (!read_chunk_data(r, length, &data))
            return false;
        r.transparency_entries = static_cast<uint16_t>(length);
        break;
    case ColorType::rgb:
    case ColorType::grey: {
        const uint32_t expected = r.header.color_type == ColorType::rgb ? k_rgb_transparency_bytes
                                                                        : k_grey_transparency_bytes;
        if (length != expected)
            return ignore_chunk(r, length, "Incorrect tRNS chunk length");
        if (!read_chunk_data(r, expected, &data))
            return false;
        r.transparency_entries = 1;
        break;
    }
    case ColorType::grey_alpha:
    case ColorType::rgb_alpha:
        return ignore_chunk(r, length, "tRNS chunk not allowed with alpha channel");
    }
    return finish_and_accept(r, record, k_accepted_transparency);
}

bool handle_background(Reader& r, Record& record, uint32_t length) {
    if ((r.sequence & k_seen_header) == 0)
        return fail(r, "Missing IHDR before bKGD");
    if ((r.sequence & k_seen_image_data) != 0)
        return ignore_chunk(r, length, "Invalid bKGD after IDAT");
    if (r.header.color_type == ColorType::palette && (r.sequence & k_seen_palette) == 0)
        return ignore_chunk(r, length, "Missing PLTE before bKGD");
    if ((record.accepted & k_accepted_background) != 0)
        return ignore_chunk(r, length, "Duplicate bKGD chunk");
    uint32_t expected = k_grey_background_bytes;
    if (r.header.color_type == ColorType::palette)
        expected = k_palette_background_bytes;
    else if (r.header.color_type == ColorType::rgb || r.header.color_type == ColorType::rgb_alpha)
        expected = k_rgb_background_bytes;
    if (length != expected)
        return ignore_chunk(r, length, "Incorrect bKGD chunk length");
    const uint8_t* data = nullptr;
    return read_chunk_data(r, expected, &data) &&
           finish_and_accept(r, record, k_accepted_background);
}

bool handle_histogram(Reader& r, Record& record, uint32_t length) {
    if ((r.sequence & k_seen_header) == 0)
        return fail(r, "Missing IHDR before hIST");
    if ((r.sequence & k_seen_image_data) != 0)
        return ignore_chunk(r, length, "Invalid hIST after IDAT");
    if ((r.sequence & k_seen_palette) == 0)
        return ignore_chunk(r, length, "Missing PLTE before hIST");
    if ((record.accepted & k_accepted_histogram) != 0)
        return ignore_chunk(r, length, "Duplicate hIST chunk");
    if (length != static_cast<uint32_t>(r.palette_entries) * k_histogram_entry_bytes)
        return ignore_chunk(r, length, "Incorrect hIST chunk length");
    const uint8_t* data = nullptr;
    return read_chunk_data(r, length, &data) && finish_and_accept(r, record, k_accepted_histogram);
}

// pHYs and oFFs: fixed length, before the image data, once.
bool handle_fixed_chunk(
    Reader& r,
    Record& record,
    uint32_t length,
    uint32_t expected,
    uint32_t bit,
    const char* missing_header,
    const char* after_image_data,
    const char* duplicate,
    const char* wrong_length
) {
    if ((r.sequence & k_seen_header) == 0)
        return fail(r, missing_header);
    if ((r.sequence & k_seen_image_data) != 0)
        return ignore_chunk(r, length, after_image_data);
    if ((record.accepted & bit) != 0)
        return ignore_chunk(r, length, duplicate);
    if (length != expected)
        return ignore_chunk(r, length, wrong_length);
    const uint8_t* data = nullptr;
    return read_chunk_data(r, length, &data) && finish_and_accept(r, record, bit);
}

// Index of the first NUL at or after `from` in `data`; the byte at `size`,
// just past the chunk, counts as a NUL.
std::size_t string_end(const uint8_t* data, std::size_t size, std::size_t from) {
    while (from < size && data[from] != 0)
        ++from;
    return from;
}

bool handle_calibration(Reader& r, Record& record, uint32_t length) {
    if ((r.sequence & k_seen_header) == 0)
        return fail(r, "Missing IHDR before pCAL");
    if ((r.sequence & k_seen_image_data) != 0)
        return ignore_chunk(r, length, "Invalid pCAL after IDAT");
    if ((record.accepted & k_accepted_calibration) != 0)
        return ignore_chunk(r, length, "Duplicate pCAL chunk");
    const uint8_t* data = nullptr;
    if (!read_chunk_data(r, length, &data))
        return false;
    const ChunkEnd end = finish_chunk(r, 0);
    if (end != ChunkEnd::kept)
        return end != ChunkEnd::failed;
    const std::size_t purpose_end = string_end(data, length, 0);
    if (length <= purpose_end + k_calibration_min_tail) {
        warn(r, "Invalid pCAL data");
        return true;
    }
    const uint8_t equation = data[purpose_end + k_calibration_equation];
    const uint8_t parameters = data[purpose_end + k_calibration_parameters];
    if (equation < k_calibration_equation_count &&
        parameters != k_calibration_parameter_counts[equation]) {
        warn(r, "Invalid pCAL parameters for equation type");
        return true;
    }
    if (equation >= k_calibration_equation_count)
        warn(r, "Unrecognized equation type for pCAL chunk");
    // Units, then one string per parameter; a parameter that starts past the
    // chunk makes the data invalid.
    std::size_t cursor = string_end(data, length, purpose_end + k_calibration_units);
    for (uint8_t i = 0; i < parameters; ++i) {
        ++cursor;
        if (cursor > length) {
            warn(r, "Invalid pCAL data");
            return true;
        }
        cursor = string_end(data, length, cursor);
    }
    record.accepted |= k_accepted_calibration;
    return true;
}

bool handle_time(Reader& r, Record& record, uint32_t length) {
    if ((r.sequence & k_seen_header) == 0)
        return fail(r, "Out of place tIME chunk");
    if ((record.accepted & k_accepted_time) != 0)
        return ignore_chunk(r, length, "Duplicate tIME chunk");
    if ((r.sequence & k_seen_image_data) != 0)
        r.sequence |= k_after_image_data;
    if (length != k_time_bytes)
        return ignore_chunk(r, length, "Incorrect tIME chunk length");
    const uint8_t* data = nullptr;
    return read_chunk_data(r, k_time_bytes, &data) && finish_and_accept(r, record, k_accepted_time);
}

bool handle_text(Reader& r, uint32_t length) {
    if ((r.sequence & k_seen_header) == 0)
        return fail(r, "Missing IHDR before tEXt");
    if ((r.sequence & k_seen_image_data) != 0)
        r.sequence |= k_after_image_data;
    return skip_chunk(r, length);
}

// Inflates a zTXt's text only to report its warnings and errors.
void check_compressed_text(Reader& r, const uint8_t* data, std::size_t size) {
    z_stream z{};
    if (inflateInit(&z) != Z_OK)
        return;
    uint8_t text[k_read_piece_bytes];
    z.next_in = const_cast<Bytef*>(data);
    z.avail_in = static_cast<uInt>(size);
    z.next_out = text;
    z.avail_out = sizeof text;
    while (z.avail_in != 0) {
        const int status = inflate(&z, Z_PARTIAL_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            warn(r, z.msg != nullptr ? z.msg : "Error decoding zTXt chunk");
            break;
        }
        if (status == Z_STREAM_END)
            break;
        if (z.avail_out == 0) {
            z.next_out = text;
            z.avail_out = sizeof text;
        }
    }
    inflateEnd(&z);
}

bool handle_compressed_text(Reader& r, uint32_t length) {
    if ((r.sequence & k_seen_header) == 0)
        return fail(r, "Missing IHDR before zTXt");
    if ((r.sequence & k_seen_image_data) != 0)
        r.sequence |= k_after_image_data;
    const uint8_t* data = nullptr;
    if (!read_chunk_data(r, length, &data))
        return false;
    const ChunkEnd end = finish_chunk(r, 0);
    if (end != ChunkEnd::kept)
        return end != ChunkEnd::failed;
    const std::size_t keyword_end = string_end(data, length, 0);
    if (keyword_end == length) {
        warn(r, "Zero length zTXt chunk");
        return true;
    }
    // The method byte reads as the terminating NUL when the keyword's NUL
    // ends the chunk, and the compressed text is then empty.
    const std::size_t method_at = keyword_end + 1;
    const auto method = static_cast<int8_t>(method_at < length ? data[method_at] : 0);
    if (method != k_text_compression_deflate) {
        char message[k_formatted_message_bytes];
        std::snprintf(message, sizeof message, "Unknown zTXt compression type %d", method);
        warn(r, message);
        return true;
    }
    const std::size_t text_at = method_at + 1;
    if (text_at < length)
        check_compressed_text(r, data + text_at, length - text_at);
    return true;
}

bool handle_unknown(Reader& r, uint32_t length) {
    for (uint8_t b : r.chunk_type)
        if (!printable_type_byte(b))
            return fail_chunk(r, "invalid chunk type");
    if (is_critical(r))
        return fail_chunk(r, "unknown critical chunk");
    if ((r.sequence & k_seen_image_data) != 0)
        r.sequence |= k_after_image_data;
    return skip_chunk(r, length);
}

// Chunks every stage handles alike. False after an error.
bool handle_chunk(Reader& r, Record& record, uint32_t length) {
    if (type_is(r, "IHDR"))
        return handle_header(r, length);
    if (type_is(r, "PLTE"))
        return handle_palette(r, record, length);
    if (type_is(r, "IEND"))
        return handle_end(r, length);
    if (type_is(r, "bKGD"))
        return handle_background(r, record, length);
    if (type_is(r, "cHRM"))
        return handle_chromaticity(r, record, length);
    if (type_is(r, "gAMA"))
        return handle_gamma(r, record, length);
    if (type_is(r, "hIST"))
        return handle_histogram(r, record, length);
    if (type_is(r, "oFFs"))
        return handle_fixed_chunk(
            r,
            record,
            length,
            k_offset_bytes,
            k_accepted_offset,
            "Missing IHDR before oFFs",
            "Invalid oFFs after IDAT",
            "Duplicate oFFs chunk",
            "Incorrect oFFs chunk length"
        );
    if (type_is(r, "pCAL"))
        return handle_calibration(r, record, length);
    if (type_is(r, "pHYs"))
        return handle_fixed_chunk(
            r,
            record,
            length,
            k_physical_size_bytes,
            k_accepted_physical_size,
            "Missing IHDR before pHYS",
            "Invalid pHYS after IDAT",
            "Duplicate pHYS chunk",
            "Incorrect pHYs chunk length"
        );
    if (type_is(r, "sBIT"))
        return handle_significant_bits(r, record, length);
    if (type_is(r, "sRGB"))
        return handle_srgb(r, record, length);
    if (type_is(r, "tEXt"))
        return handle_text(r, length);
    if (type_is(r, "tIME"))
        return handle_time(r, record, length);
    if (type_is(r, "tRNS"))
        return handle_transparency(r, record, length);
    if (type_is(r, "zTXt"))
        return handle_compressed_text(r, length);
    return handle_unknown(r, length);
}

// ---- image data -------------------------------------------------------------

// Hands zlib the next piece of image data, moving on to the next IDAT
// (after checking the used one's CRC) when the current one is used up.
bool refill(Reader& r) {
    while (r.image_data_left == 0) {
        uint32_t length = 0;
        if (finish_chunk(r, 0) == ChunkEnd::failed || !read_chunk_header(r, &length))
            return false;
        r.image_data_left = length;
        if (!type_is(r, "IDAT"))
            return fail(r, "Not enough image data");
    }
    const std::size_t piece =
        r.image_data_left < k_read_piece_bytes ? r.image_data_left : k_read_piece_bytes;
    const uint8_t* data = nullptr;
    if (!read_chunk_data(r, piece, &data))
        return false;
    r.z.next_in = const_cast<Bytef*>(data);
    r.z.avail_in = static_cast<uInt>(piece);
    r.image_data_left -= static_cast<uint32_t>(piece);
    return true;
}

// Inflates one encoded row (filter byte included).
bool inflate_row(Reader& r, uint8_t* out, std::size_t size) {
    r.z.next_out = out;
    r.z.avail_out = static_cast<uInt>(size);
    do {
        if (r.z.avail_in == 0 && !refill(r))
            return false;
        const int status = inflate(&r.z, Z_PARTIAL_FLUSH);
        if (status == Z_STREAM_END) {
            if (r.z.avail_out != 0 || r.z.avail_in != 0 || r.image_data_left != 0)
                return fail(r, "Extra compressed data");
            r.sequence |= k_after_image_data;
            r.stream_ended = true;
            break;
        }
        if (status != Z_OK)
            return fail(r, r.z.msg != nullptr ? r.z.msg : "Decompression error");
    } while (r.z.avail_out != 0);
    return true;
}

// After the last row the stream must end without another byte, leaving no
// image data behind.
bool finish_image_data(Reader& r) {
    if (!r.stream_ended) {
        uint8_t extra = 0;
        r.z.next_out = &extra;
        r.z.avail_out = 1;
        for (;;) {
            if (r.z.avail_in == 0 && !refill(r))
                return false;
            const int status = inflate(&r.z, Z_PARTIAL_FLUSH);
            if (status == Z_STREAM_END) {
                if (r.z.avail_out == 0 || r.z.avail_in != 0 || r.image_data_left != 0)
                    return fail(r, "Extra compressed data");
                r.stream_ended = true;
                break;
            }
            if (status != Z_OK)
                return fail(r, r.z.msg != nullptr ? r.z.msg : "Decompression Error");
            if (r.z.avail_out == 0)
                return fail(r, "Extra compressed data");
        }
    }
    if (r.image_data_left != 0 || r.z.avail_in != 0)
        return fail(r, "Extra compression data");
    r.sequence |= k_after_image_data;
    return true;
}

uint32_t output_bit_depth(const Header& header, const Transforms& transforms) {
    if (header.bit_depth == 16 && transforms.strip_16)
        return 8;
    if (header.bit_depth < 8 && transforms.unpack)
        return 8;
    return header.bit_depth;
}

// Applies the transforms to one decoded row of `pixels` pixels.
void transform_row(
    const uint8_t* raw,
    uint32_t pixels,
    const Header& header,
    const Transforms& transforms,
    uint8_t* out
) {
    const uint64_t samples = static_cast<uint64_t>(pixels) * channel_count(header.color_type);
    if (header.bit_depth == 16 && transforms.strip_16) {
        for (uint64_t i = 0; i < samples; ++i)
            out[i] = raw[i * 2];
        return;
    }
    if (header.bit_depth < 8 && transforms.unpack) {
        const uint32_t depth = header.bit_depth;
        const uint32_t per_byte = 8 / depth;
        const uint32_t mask = (1u << depth) - 1;
        for (uint64_t i = 0; i < samples; ++i) {
            const uint32_t shift = 8 - depth * static_cast<uint32_t>(i % per_byte + 1);
            out[i] = static_cast<uint8_t>((raw[i / per_byte] >> shift) & mask);
        }
        return;
    }
    std::memcpy(out, raw, packed_bytes(pixels, bits_per_pixel(header)));
}

uint32_t get_bits(const uint8_t* row, uint64_t pixel, uint32_t bits) {
    const uint64_t bit = pixel * bits;
    const uint32_t shift = 8 - bits - static_cast<uint32_t>(bit % 8);
    return (row[bit / 8] >> shift) & ((1u << bits) - 1);
}

void set_bits(uint8_t* row, uint64_t pixel, uint32_t bits, uint32_t value) {
    const uint64_t bit = pixel * bits;
    const uint32_t shift = 8 - bits - static_cast<uint32_t>(bit % 8);
    const auto mask = static_cast<uint8_t>(((1u << bits) - 1) << shift);
    uint8_t& byte = row[bit / 8];
    byte = static_cast<uint8_t>((byte & ~mask) | ((value << shift) & mask));
}

// Writes a pass row's pixels to their columns of the image row.
void place_row(
    const uint8_t* pixels, uint32_t count, uint32_t bits, const Pass& pass, uint8_t* image_row
) {
    if (pass.dx == 1) {
        std::memcpy(image_row, pixels, packed_bytes(count, bits));
        return;
    }
    if (bits >= 8) {
        const uint32_t bytes = bits / 8;
        for (uint32_t i = 0; i < count; ++i)
            std::memcpy(
                image_row +
                    (static_cast<std::size_t>(pass.x0) + static_cast<std::size_t>(i) * pass.dx) *
                        bytes,
                pixels + static_cast<std::size_t>(i) * bytes,
                bytes
            );
        return;
    }
    for (uint32_t i = 0; i < count; ++i)
        set_bits(
            image_row, pass.x0 + static_cast<uint64_t>(i) * pass.dx, bits, get_bits(pixels, i, bits)
        );
}

// Decodes every pass into `rows`. False after an error, with the rows
// decoded so far in place.
bool decode_rows(
    Reader& r, const Transforms& transforms, std::span<uint8_t> rows, std::size_t out_row_bytes
) {
    const Header& header = r.header;
    const bool interlaced = header.interlace == Interlace::adam7;
    const uint32_t pass_count = interlaced ? k_adam7_pass_count : 1;
    const uint32_t raw_bits = bits_per_pixel(header);
    const uint32_t out_bits =
        output_bit_depth(header, transforms) * channel_count(header.color_type);
    const std::size_t stride = filter_stride(header);
    const std::size_t full_raw_bytes = packed_bytes(header.width, raw_bits);
    std::vector<uint8_t> line(full_raw_bytes + 1);
    std::vector<uint8_t> previous(full_raw_bytes);
    std::vector<uint8_t> converted(out_row_bytes);
    for (uint32_t p = 0; p < pass_count; ++p) {
        const Pass& pass = interlaced ? k_adam7_passes[p] : k_whole_image;
        const uint32_t pass_width = pass_extent(header.width, pass.x0, pass.dx);
        const uint32_t pass_height = pass_extent(header.height, pass.y0, pass.dy);
        if (pass_width == 0 || pass_height == 0)
            continue;
        const std::size_t pass_bytes = packed_bytes(pass_width, raw_bits);
        std::memset(previous.data(), 0, pass_bytes);
        for (uint32_t y = 0; y < pass_height; ++y) {
            if (!inflate_row(r, line.data(), pass_bytes + 1))
                return false;
            uint8_t* data = line.data() + 1;
            if (!unfilter_row(line[0], data, previous.data(), pass_bytes, stride))
                return fail(r, "Bad adaptive filter type");
            std::memcpy(previous.data(), data, pass_bytes);
            transform_row(data, pass_width, header, transforms, converted.data());
            const std::size_t image_y = pass.y0 + static_cast<std::size_t>(y) * pass.dy;
            place_row(
                converted.data(), pass_width, out_bits, pass, rows.data() + image_y * out_row_bytes
            );
        }
    }
    return true;
}

// Chunks after the image data up to IEND, into a fresh record.
bool read_trailing_chunks(Reader& r) {
    Record record{};
    if (finish_chunk(r, 0) == ChunkEnd::failed)
        return false;
    do {
        uint32_t length = 0;
        if (!read_chunk_header(r, &length))
            return false;
        if (type_is(r, "IDAT")) {
            if (length != 0 || (r.sequence & k_after_image_data) != 0)
                return fail(r, "Too many IDAT's found");
            if (!skip_chunk(r, 0))
                return false;
        } else if (!handle_chunk(r, record, length)) {
            return false;
        }
    } while ((r.sequence & k_seen_end) == 0);
    return true;
}

} // namespace

void detail::format_chunk_message(const uint8_t* type, const char* text, char* out) {
    static constexpr char k_hex_digits[] = "0123456789ABCDEF";
    std::size_t used = 0;
    for (std::size_t i = 0; i < k_chunk_type_bytes; ++i) {
        const uint8_t b = type[i];
        if (printable_type_byte(b)) {
            out[used++] = static_cast<char>(b);
            continue;
        }
        out[used++] = '[';
        out[used++] = k_hex_digits[b >> 4];
        out[used++] = k_hex_digits[b & 0x0f];
        out[used++] = ']';
    }
    out[used++] = ':';
    out[used++] = ' ';
    std::size_t kept = 0;
    while (kept < k_chunk_message_text && text[kept] != '\0') {
        out[used++] = text[kept];
        ++kept;
    }
    out[used] = '\0';
}

bool detail::unfilter_row(
    uint8_t filter, uint8_t* row, const uint8_t* previous, std::size_t size, std::size_t stride
) {
    switch (static_cast<Filter>(filter)) {
    case Filter::none:
        return true;
    case Filter::sub:
        for (std::size_t i = stride; i < size; ++i)
            row[i] = static_cast<uint8_t>(row[i] + row[i - stride]);
        return true;
    case Filter::up:
        for (std::size_t i = 0; i < size; ++i)
            row[i] = static_cast<uint8_t>(row[i] + previous[i]);
        return true;
    case Filter::average:
        for (std::size_t i = 0; i < size; ++i) {
            const uint32_t left = i >= stride ? row[i - stride] : 0;
            row[i] = static_cast<uint8_t>(row[i] + (left + previous[i]) / 2);
        }
        return true;
    case Filter::paeth:
        for (std::size_t i = 0; i < size; ++i) {
            const int32_t left = i >= stride ? row[i - stride] : 0;
            const int32_t up_left = i >= stride ? previous[i - stride] : 0;
            row[i] = static_cast<uint8_t>(row[i] + paeth_predictor(left, previous[i], up_left));
        }
        return true;
    }
    return false;
}

namespace {

// How far an iTXt chunk was read. compressed is a chunk this reader skips.
enum class InternationalChunk : uint8_t {
    malformed,
    compressed,
    kept,
};

/// Finds the next NUL in a chunk body.
///
/// @param data the chunk body
/// @param length the body length
/// @param from the first byte to look at
/// @return the index of the NUL, or length when the body has none
std::size_t separator_at(const uint8_t* data, uint32_t length, std::size_t from) {
    std::size_t at = from;
    while (at < length && data[at] != 0)
        ++at;
    return at;
}

/// Splits a tEXt body into its keyword and its text.
///
/// @param data the chunk body
/// @param length the body length
/// @param[out] chunk the chunk; keyword and text point into data
/// @return false when the body has no keyword separator
bool take_latin1(const uint8_t* data, uint32_t length, TextChunk* chunk) {
    const std::size_t keyword_end = separator_at(data, length, 0);
    if (keyword_end >= length)
        return false;
    chunk->kind = TextKind::latin1;
    chunk->keyword = std::string_view(reinterpret_cast<const char*>(data), keyword_end);
    const std::size_t text_at = keyword_end + 1;
    chunk->text = std::string_view(reinterpret_cast<const char*>(data + text_at), length - text_at);
    return true;
}

/// Splits an iTXt body, or reports that it is compressed or malformed.
///
/// @param data the chunk body
/// @param length the body length
/// @param[out] chunk the chunk when the result is kept; keyword and text
///        point into data
/// @return malformed when the body is cut short of its method byte or of a
///         separator, compressed when the compression flag is not 0, kept
///         otherwise. The method byte is ignored when the flag is 0.
InternationalChunk take_international(const uint8_t* data, uint32_t length, TextChunk* chunk) {
    const std::size_t keyword_end = separator_at(data, length, 0);
    if (keyword_end >= length || keyword_end + 1 >= length)
        return InternationalChunk::malformed;
    if (data[keyword_end + 1] != 0)
        return InternationalChunk::compressed;
    // The method byte has to be present. Decoders ignore its value when the
    // flag is 0; encoders set it to 0.
    if (keyword_end + 2 >= length)
        return InternationalChunk::malformed;
    const std::size_t language_end = separator_at(data, length, keyword_end + 3);
    if (language_end >= length)
        return InternationalChunk::malformed;
    const std::size_t translated_end = separator_at(data, length, language_end + 1);
    if (translated_end >= length)
        return InternationalChunk::malformed;
    const std::size_t text_at = translated_end + 1;
    chunk->kind = TextKind::utf8;
    chunk->keyword = std::string_view(reinterpret_cast<const char*>(data), keyword_end);
    chunk->text = std::string_view(reinterpret_cast<const char*>(data + text_at), length - text_at);
    return InternationalChunk::kept;
}

} // namespace

uint32_t channel_count(ColorType color_type) {
    switch (color_type) {
    case ColorType::rgb:
        return 3;
    case ColorType::grey_alpha:
        return 2;
    case ColorType::rgb_alpha:
        return 4;
    case ColorType::grey:
    case ColorType::palette:
        break;
    }
    return 1;
}

bool has_signature(std::span<const uint8_t> file) {
    return file.size() >= k_signature_bytes &&
           std::memcmp(file.data(), k_signature, k_signature_bytes) == 0;
}

bool read_info(std::span<const uint8_t> file, const Messages& messages, Info* info) {
    *info = Info{};
    Reader r;
    r.file = file;
    r.messages = &messages;
    // The signature is checked before any chunk is read.
    if (!has_signature(file))
        return fail(r, "File is not in PNG format");
    r.position = k_signature_bytes;
    Record record{};
    for (;;) {
        uint32_t length = 0;
        if (!read_chunk_header(r, &length))
            return false;
        if (!type_is(r, "IDAT")) {
            if (!handle_chunk(r, record, length))
                return false;
            continue;
        }
        if ((r.sequence & k_seen_header) == 0)
            return fail(r, "Missing IHDR before IDAT");
        if (r.header.color_type == ColorType::palette && (r.sequence & k_seen_palette) == 0)
            return fail(r, "Missing PLTE before IDAT");
        info->header = r.header;
        info->has_palette = (record.accepted & k_accepted_palette) != 0;
        info->palette_entries = record.palette_entries;
        std::memcpy(info->palette, record.palette, sizeof info->palette);
        info->image_data = r.position;
        info->image_data_bytes = length;
        return true;
    }
}

Header header_of(const Info& info, const Messages& messages) {
    const uint32_t pixel_bits = bits_per_pixel(info.header);
    if (k_max_dimension / ((pixel_bits + 7u) >> 3) < info.header.width &&
        messages.warning != nullptr)
        messages.warning(messages.user, "Width too large for libpng to process image data.");
    return info.header;
}

std::size_t row_bytes(const Header& header, const Transforms& transforms) {
    return packed_bytes(
        header.width, output_bit_depth(header, transforms) * channel_count(header.color_type)
    );
}

Progress read_image(
    std::span<const uint8_t> file,
    const Info& info,
    const Transforms& transforms,
    const Messages& messages,
    std::span<uint8_t> rows
) {
    Reader r;
    r.file = file;
    r.messages = &messages;
    r.header = info.header;
    const std::size_t out_row_bytes = row_bytes(info.header, transforms);
    // Rows sized other than by row_bytes are a caller error, reported with the
    // message for misplaced row reads.
    // The raw and the converted rows are sized at 64 bits before anything
    // is sized from them.
    const uint32_t out_bits =
        output_bit_depth(info.header, transforms) * channel_count(info.header.color_type);
    if (!row_size_decodable(info.header.width, bits_per_pixel(info.header)) ||
        !row_size_decodable(info.header.width, out_bits) || out_row_bytes == 0 ||
        rows.size() / out_row_bytes < info.header.height) {
        fail(r, "Row read request is not permitted");
        return Progress::none;
    }
    // Resume just after the first IDAT's type, as read_info left the stream.
    std::memcpy(r.chunk_type, "IDAT", k_chunk_type_bytes);
    r.crc =
        crc_update(static_cast<uint32_t>(crc32(0L, Z_NULL, 0)), r.chunk_type, k_chunk_type_bytes);
    r.position = info.image_data;
    r.image_data_left = info.image_data_bytes;
    r.sequence = k_seen_header | k_seen_image_data;
    const int status = inflateInit(&r.z);
    if (status != Z_OK) {
        fail(
            r,
            status == Z_VERSION_ERROR                           ? "zlib version error"
            : status == Z_MEM_ERROR || status == Z_STREAM_ERROR ? "zlib memory error"
                                                                : "Unknown zlib error"
        );
        return Progress::none;
    }
    r.z_open = true;
    if (!decode_rows(r, transforms, rows, out_row_bytes) || !finish_image_data(r))
        return Progress::none;
    return read_trailing_chunks(r) ? Progress::end : Progress::rows;
}

bool read_texts(std::span<const uint8_t> file, std::vector<TextChunk>* texts) {
    if (texts == nullptr || !has_signature(file))
        return false;
    std::vector<TextChunk> found;
    std::size_t at = k_signature_bytes;
    for (;;) {
        if (file.size() - at < k_chunk_overhead)
            return false;
        const uint8_t* chunk = file.data() + at;
        const uint32_t length = read_be32(chunk);
        if (static_cast<std::size_t>(length) > file.size() - at - k_chunk_overhead)
            return false;
        const uint8_t* type = chunk + k_chunk_length_bytes;
        const uint8_t* data = type + k_chunk_type_bytes;
        const uint8_t* crc_bytes = data + length;
        if (chunk_crc(type, k_chunk_type_bytes + static_cast<std::size_t>(length)) !=
            read_be32(crc_bytes))
            return false;
        at += k_chunk_overhead + length;
        if (std::memcmp(type, "IEND", k_chunk_type_bytes) == 0) {
            *texts = std::move(found);
            return true;
        }
        if (std::memcmp(type, "tEXt", k_chunk_type_bytes) == 0) {
            TextChunk item;
            if (!take_latin1(data, length, &item))
                return false;
            found.push_back(item);
            continue;
        }
        if (std::memcmp(type, "zTXt", k_chunk_type_bytes) == 0)
            continue;
        if (std::memcmp(type, "iTXt", k_chunk_type_bytes) == 0) {
            TextChunk item;
            const InternationalChunk taken = take_international(data, length, &item);
            if (taken == InternationalChunk::malformed)
                return false;
            if (taken == InternationalChunk::kept)
                found.push_back(item);
        }
    }
}

} // namespace oa::formats::png
