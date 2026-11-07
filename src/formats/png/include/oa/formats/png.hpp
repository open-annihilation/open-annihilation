// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// PNG codec over the project's zlib.
//
// The game reads PNG only for the IMG tag of its markup pages. This reader
// reads every standard colour type, bit depth and Adam7 interlacing, and
// applies the two row transforms those pages use: 16-bit samples cut to their
// high byte and 1/2/4-bit samples widened to one unscaled byte each. The
// writer turns packed rows back into a file, which the tools use for image
// output; its stored form, which the automation endpoint uses for the frames
// it hands out, trades the file's size for the time it takes to write. A
// picture may also carry tEXt and uncompressed iTXt chunks. read_texts reads
// those back; read_info and read_image do not, and their checks and messages
// stay as they are.
//
// Reading is split in two so that a caller can act between the steps:
// read_info takes the chunks before the image data, read_image decodes the
// rows and then the chunks after them. The chunk rules, the points where
// reading stops and the fault each warning and error reports match 3.1c.
// Every message a markup page can raise has 3.1c's text; the two it never
// raises, for a file without the signature (the pages skip such a file
// unread) and for rows not sized by row_bytes, are worded for developers.
// Errors go to Messages::error and stop reading; warnings go to
// Messages::warning.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace oa::formats::png {

inline constexpr std::size_t k_signature_bytes = 8;
inline constexpr uint8_t k_signature[k_signature_bytes] = {
    0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'
};
// IHDR width and height must lie in 1..k_max_dimension.
inline constexpr uint32_t k_max_dimension = 0x7fffffff;
inline constexpr std::size_t k_max_palette_entries = 256;

enum class ColorType : uint8_t {
    grey = 0,
    rgb = 2,
    palette = 3,
    grey_alpha = 4,
    rgb_alpha = 6,
};

enum class Interlace : uint8_t {
    none = 0,
    adam7 = 1,
};

struct Rgb {
    uint8_t r{};
    uint8_t g{};
    uint8_t b{};
};

struct Header {
    uint32_t width{};
    uint32_t height{};
    uint8_t bit_depth{}; // bits per sample: 1, 2, 4, 8 or 16
    ColorType color_type{};
    Interlace interlace{};
};

struct Messages {
    void* user{};
    void (*warning)(void* user, const char* text){};
    void (*error)(void* user, const char* text){};
};

// What precedes the image data.
struct Info {
    Header header{};
    bool has_palette{};                   // a PLTE chunk was accepted, whatever the colour type
    uint16_t palette_entries{};           // its entry count, cut to 16 bits
    Rgb palette[k_max_palette_entries]{}; // its first entries
    std::size_t image_data{};             // file offset of the first IDAT chunk's data
    uint32_t image_data_bytes{};          // that chunk's length
};

struct Transforms {
    bool strip_16{}; // keep the high byte of each 16-bit sample
    bool unpack{};   // one byte per 1/2/4-bit sample, value unscaled
};

enum class Progress : uint8_t {
    none, // stopped inside the image data
    rows, // every row decoded; stopped in the chunks after the image data
    end,  // IEND reached
};

/// Returns the number of samples per pixel of a colour type.
///
/// @param color_type PNG colour type
/// @return 1 for grey and palette, 2 for grey with alpha, 3 for RGB, 4 for RGB with alpha
uint32_t channel_count(ColorType color_type);

/// Tests whether a file starts with the PNG signature.
///
/// @param file file bytes
/// @return true when the first eight bytes are the signature
bool has_signature(std::span<const uint8_t> file);

/// Reads the chunks after the signature up to the first IDAT's length and type.
///
/// @param file the whole file
/// @param messages warning and error sinks
/// @param[out] info header, palette and position of the first IDAT chunk
/// @return false after reporting an error
bool read_info(std::span<const uint8_t> file, const Messages& messages, Info* info);

/// Returns the IHDR fields of an Info.
///
/// Warns when a row of that width cannot be sized.
///
/// @param info result of read_info
/// @param messages warning sink
/// @return info.header
Header header_of(const Info& info, const Messages& messages);

/// Returns the bytes of one image row after the transforms.
///
/// @param header image header
/// @param transforms row transforms applied
/// @return packed bytes per row
std::size_t row_bytes(const Header& header, const Transforms& transforms);

/// Decodes the image data, then reads the chunks up to IEND.
///
/// @param file the whole file
/// @param info result of read_info
/// @param transforms row transforms applied
/// @param messages warning and error sinks
/// @param[out] rows height rows of row_bytes each, top row first; rows the
///        data did not reach are left as they were
/// @return how far reading got before stopping
Progress read_image(
    std::span<const uint8_t> file,
    const Info& info,
    const Transforms& transforms,
    const Messages& messages,
    std::span<uint8_t> rows
);

/// A tEXt chunk, whose text is Latin-1, or an uncompressed iTXt chunk, whose
/// text is UTF-8.
enum class TextKind : uint8_t {
    /// Latin-1 text, written as a tEXt chunk.
    latin1,
    /// UTF-8 text, written as an uncompressed iTXt chunk with an empty
    /// language tag and an empty translated keyword.
    utf8,
};

/// One text chunk stored in a PNG file.
struct TextChunk {
    TextKind kind{};
    /// 1 to 79 bytes of printable Latin-1, with no leading space, no trailing
    /// space and no two spaces together.
    std::string_view keyword{};
    /// The chunk's text: Latin-1 with no NUL for tEXt, UTF-8 with no NUL for iTXt.
    std::string_view text{};
};

/// Reads the file's tEXt chunks and its uncompressed iTXt chunks.
///
/// A zTXt chunk is skipped, and so is an iTXt chunk whose compression flag
/// is not 0. Keyword and text point into file.
///
/// @param file the whole file
/// @param[out] texts the chunks in file order; left unchanged when reading fails
/// @return false when file is not a whole PNG, a chunk's checksum does not
///         match, or a tEXt or uncompressed iTXt chunk has no keyword separator
[[nodiscard]] bool read_texts(std::span<const uint8_t> file, std::vector<TextChunk>* texts);

struct Image {
    Header header{};                 // interlace selects Adam7 output
    std::span<const Rgb> palette{};  // required for ColorType::palette
    std::span<const uint8_t> rows{}; // height rows of row_bytes(header, {}) bytes
    /// Text chunks written after the palette and before the image data.
    /// Empty leaves the file byte for byte as a picture with no text.
    std::span<const TextChunk> texts{};
};

/// Encodes an image as a PNG file.
///
/// Text chunks are written after the header and the palette, before the
/// image data, in the order given. A text chunk that breaks the keyword or
/// text rules refuses the file.
///
/// @param image header, palette, packed rows and text chunks
/// @param[out] out receives the file bytes; left unchanged when writing is refused
/// @return false when the header, palette, row data or a text chunk is invalid
bool write(const Image& image, std::vector<uint8_t>* out);

/// Encodes an image as a PNG file without filtering its rows or compressing
/// them: the image data is stored as deflate's uncompressed blocks, so the
/// file takes little more than one pass over the pixels to write and is a
/// little larger than they are. Text chunks follow the same rules, and sit
/// in the same place in the file, as for write.
///
/// @param image header, palette, packed rows and text chunks
/// @param[out] out receives the file bytes; left unchanged when writing is refused
/// @return false when the header, palette, row data or a text chunk is invalid
bool write_stored(const Image& image, std::vector<uint8_t>* out);

} // namespace oa::formats::png
