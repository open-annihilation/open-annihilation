// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/fnt.hpp"
#include "oa/formats/gaf.hpp"
#include <algorithm>
#include <bit>
#include <cstdint>
#include <limits>
#include <stdexcept>
#include <string>

namespace oa::formats::fnt {
namespace {
constexpr std::size_t fnt_header_bytes = file_header_bytes;
constexpr std::size_t fnt_data_start = file_preamble_bytes;

struct FileHeader {
    uint16_t height = 0;
    uint16_t word_after_height = 0; // low byte: rows every glyph starts above the pen
    std::array<uint16_t, limit::glyph_count> offsets{};
};

constexpr uint8_t first_printable = 0x20;
constexpr uint8_t height_reference = 0x49;

using base::bytes::ByteReader;
using base::bytes::DecodeCode;
using base::bytes::DecodeError;
using base::bytes::Decoded;
} // namespace

Decoded<Font> parse_fnt(std::span<const uint8_t> b) {
    if (b.size() > limit::input_bytes)
        return DecodeError{
            DecodeCode::limit_exceeded,
            limit::input_bytes,
            "FNT font exceeds the 4 MiB safety limit"
        };
    if (b.size() < fnt_data_start)
        return DecodeError{
            DecodeCode::truncated, b.size(), "FNT is shorter than its 516-byte header"
        };
    ByteReader reader(b);
    FileHeader disk;
    disk.height = reader.u16();
    disk.word_after_height = reader.u16();
    for (auto& offset : disk.offsets)
        offset = reader.u16();
    Font out;
    out.nominal_height = disk.height;
    out.word_after_height = disk.word_after_height;
    if (out.nominal_height == 0 || out.nominal_height > limit::glyph_height)
        return DecodeError{DecodeCode::out_of_range, 0, "FNT glyph height is outside 1..128"};
    for (std::size_t c = 0; c < limit::glyph_count; ++c) {
        const std::size_t offset = disk.offsets[c];
        if (offset == 0)
            continue;
        const auto offset_at = fnt_header_bytes + c * sizeof(uint16_t);
        if (offset < fnt_data_start || offset >= b.size())
            return DecodeError{
                DecodeCode::out_of_range, offset_at, "FNT glyph offset lies outside glyph data"
            };
        const auto width = b[offset];
        if (width == 0 || width > limit::glyph_width)
            return DecodeError{
                DecodeCode::out_of_range, offset, "FNT glyph width is outside 1..128"
            };
        const std::size_t count = static_cast<std::size_t>(width) * out.nominal_height;
        const std::size_t packed = (count + 7U) / 8U;
        const auto bitmap = reader.bytes_at(offset + 1, packed);
        if (!reader.ok())
            return DecodeError{DecodeCode::truncated, offset + 1, "truncated FNT glyph bitmap"};
        Glyph glyph;
        glyph.width = width;
        glyph.height = out.nominal_height;
        // FNT is a monochrome mask. Index 255 is the explicit palette-raster
        // foreground; callers can remap it to the current GUI palette.
        glyph.pixels.assign(count, foreground_index);
        glyph.coverage.resize(count);
        for (std::size_t bit = 0; bit < count; ++bit)
            glyph.coverage[bit] = static_cast<uint8_t>((bitmap[bit / 8U] >> (7U - bit % 8U)) & 1U);
        out.glyphs[c] = std::move(glyph);
    }
    return out;
}

Decoded<Font> parse_gaf(std::span<const uint8_t> b) {
    if (b.size() > limit::input_bytes)
        return DecodeError{
            DecodeCode::limit_exceeded,
            limit::input_bytes,
            "GAF font exceeds the 4 MiB safety limit"
        };
    // A glyph's size is refused before any of its pixels are decoded.
    const auto parsed = formats::gaf::parse(
        b,
        formats::gaf::PixelData::decoded,
        std::max<uint16_t>(limit::glyph_width, limit::glyph_height)
    );
    if (!parsed.ok() && parsed.error && parsed.error->code == formats::gaf::ErrorCode::side_limit)
        return DecodeError{
            DecodeCode::out_of_range,
            parsed.error->offset,
            "GAF font glyph dimensions exceed 128 pixels"
        };
    if (!parsed.ok())
        return DecodeError{
            DecodeCode::malformed,
            parsed.error ? parsed.error->offset : 0,
            "cannot parse GAF font",
            parsed.error ? static_cast<uint16_t>(parsed.error->code) : uint16_t{}
        };
    if (parsed.archive->sequences.empty())
        return DecodeError{DecodeCode::malformed, 0, "GAF font contains no sequence"};
    const auto& frames = parsed.archive->sequences.front().frames;
    if (frames.size() > limit::glyph_count)
        return DecodeError{
            DecodeCode::limit_exceeded, 0, "GAF font has more than 256 glyph frames"
        };
    Font out;
    for (std::size_t c = 0; c < frames.size(); ++c) {
        const auto rendered = formats::gaf::render_normal(frames[c]);
        if (!rendered.ok())
            return DecodeError{
                DecodeCode::malformed,
                rendered.error ? rendered.error->offset : 0,
                "cannot render a GAF font glyph",
                static_cast<uint16_t>(c)
            };
        const auto& f = *rendered.frame;
        if (f.width > limit::glyph_width || f.height > limit::glyph_height)
            return DecodeError{
                DecodeCode::out_of_range,
                0,
                "GAF font glyph dimensions exceed 128 pixels",
                static_cast<uint16_t>(c)
            };
        out.glyphs[c] = Glyph{f.width, f.height, f.origin_x, f.origin_y, f.pixels, f.coverage};
    }
    if (out.glyphs[height_reference]) {
        out.nominal_height = out.glyphs[height_reference]->height;
        // The game normalizes every GUI-font frame after loading: it reads
        // frame 0x49's height, then subtracts that height from each frame's
        // signed origin_y field. Raw hattfont12 has origin_y=11,height=12;
        // the active GUI font therefore draws with origin_y=-1, placing its
        // bitmap one pixel below the centred pen coordinate the GUI passes.
        for (auto& glyph : out.glyphs) {
            if (!glyph)
                continue;
            const auto wrapped =
                static_cast<uint16_t>(static_cast<int32_t>(glyph->origin_y) - out.nominal_height);
            glyph->origin_y = std::bit_cast<int16_t>(wrapped);
        }
    }
    return out;
}

Decoded<Font> load_fnt(AssetStore& a, std::string_view p) {
    return parse_fnt(a.read(p).bytes);
}

Decoded<Font> load_gaf(AssetStore& a, std::string_view p) {
    return parse_gaf(a.read(p).bytes);
}

namespace {
constexpr std::string_view disk_font_extension = "FNT";
// Longest font path the game accepts, in bytes with the terminator.
constexpr std::size_t disk_font_path_bytes = 0x100;

/// Builds directory\name with its last dotted suffix replaced by ".FNT".
///
/// @return the path, or nullopt when it would not fit the font path limit
std::optional<std::string> disk_font_path(std::string_view directory, std::string_view name) {
    std::string path;
    path.reserve(directory.size() + name.size() + disk_font_extension.size() + 2);
    path.append(directory);
    path.push_back('\\');
    path.append(name);
    // The suffix dropped starts at the path's last dot, wherever it is. A dot
    // in the language directory counts: "fonts-en.gb\\smlfont" becomes
    // "fonts-en.FNT".
    if (const auto dot = path.rfind('.'); dot != std::string::npos)
        path.resize(dot);
    path.push_back('.');
    path.append(disk_font_extension);
    if (path.size() >= disk_font_path_bytes)
        return std::nullopt;
    return path;
}

struct LocatedFont {
    bool found = false;
    std::vector<uint8_t> bytes;
};

LocatedFont read_opened_font(AssetStore& assets, const std::string& path) {
    try {
        auto data = assets.read(path);
        return {true, std::move(data.bytes)};
    } catch (const std::runtime_error& error) {
        // The game treats a failed open as "absent". Other failures stay fatal.
        if (std::string_view(error.what()).starts_with("asset not found:"))
            return {};
        throw;
    }
}

Decoded<Font> font_from_opened_file(const LocatedFont& located) {
    // An empty or missing font file is fatal; no other path is tried.
    if (!located.found || located.bytes.empty())
        return DecodeError{DecodeCode::not_found, 0, "missing font file"};
    return parse_fnt(located.bytes);
}

const DecodeError font_path_too_long{
    DecodeCode::limit_exceeded, 0, "font path exceeds the 256-byte font path limit"
};
} // namespace

Decoded<Font> load_named_fnt(AssetStore& assets, std::string_view name, std::string_view language) {
    // The language comes from the game's language setting. The alternate path
    // is fonts-<language>\<name>, kept only when it opens.
    if (!language.empty()) {
        const auto alternate = disk_font_path(std::string("fonts-") + std::string(language), name);
        if (!alternate)
            return font_path_too_long;
        const auto located = read_opened_font(assets, *alternate);
        if (located.found)
            return font_from_opened_file(located);
    }
    const auto fallback = disk_font_path("fonts", name);
    if (!fallback)
        return font_path_too_long;
    return font_from_opened_file(read_opened_font(assets, *fallback));
}

uint32_t measure_text(const Font& f, std::string_view text) noexcept {
    uint32_t width = 0;
    for (const unsigned char c : text) {
        if (c < first_printable || !f.glyphs[c])
            continue;
        const auto add = f.glyphs[c]->width;
        width = add > std::numeric_limits<uint32_t>::max() - width
                    ? std::numeric_limits<uint32_t>::max()
                    : width + add;
    }
    return width;
}

std::string_view fit_text(const Font& f, std::string_view text, uint32_t width) noexcept {
    // Glyph widths are never negative, so the first character that would
    // take the width past `width` ends the longest start that fits.
    uint32_t used = 0;
    std::size_t kept = 0;
    for (const unsigned char c : text) {
        const uint32_t add = c < first_printable || !f.glyphs[c] ? 0U : f.glyphs[c]->width;
        if (add > width - used)
            break;
        used += add;
        ++kept;
    }
    return text.substr(0, kept);
}

uint16_t line_height(const Font& f) noexcept {
    const auto& g = f.glyphs[height_reference];
    const auto height = g ? g->height : f.nominal_height;
    return static_cast<uint16_t>(height + 2U);
}

int32_t row_lift(const Font& f) noexcept {
    return static_cast<int8_t>(static_cast<uint8_t>(f.word_after_height & 0xffU));
}

int32_t
raster_text(IndexedSurface dst, const Font& f, std::string_view text, int32_t x, int32_t y) {
    if (dst.stride < dst.width || (dst.height != 0 && dst.stride > dst.pixels.size() / dst.height))
        throw std::runtime_error("indexed destination has inconsistent bounds");
    if (!dst.coverage.empty() && (dst.height != 0 && dst.stride > dst.coverage.size() / dst.height))
        throw std::runtime_error("indexed destination coverage has inconsistent bounds");
    const int64_t lift = row_lift(f);
    for (const unsigned char c : text) {
        if (c < first_printable || !f.glyphs[c])
            continue;
        const auto& g = *f.glyphs[c];
        const auto count = static_cast<std::size_t>(g.width) * g.height;
        if (g.pixels.size() != count || g.coverage.size() != count)
            throw std::runtime_error("font glyph has inconsistent buffers");
        if (c != first_printable) {
            const auto left = static_cast<int64_t>(x) - g.origin_x;
            const auto top = static_cast<int64_t>(y) - g.origin_y - lift;
            for (uint32_t row = 0; row < g.height; ++row)
                for (uint32_t col = 0; col < g.width; ++col) {
                    const auto source = static_cast<std::size_t>(row) * g.width + col;
                    const auto dx = left + col;
                    const auto dy = top + row;
                    if (!g.coverage[source] || dx < 0 || dy < 0 ||
                        dx >= static_cast<int32_t>(dst.width) ||
                        dy >= static_cast<int32_t>(dst.height))
                        continue;
                    const auto target =
                        static_cast<std::size_t>(dy) * dst.stride + static_cast<std::size_t>(dx);
                    dst.pixels[target] = g.pixels[source];
                    if (!dst.coverage.empty())
                        dst.coverage[target] = 1;
                }
        }
        const auto next = static_cast<int64_t>(x) + g.width;
        x = static_cast<int32_t>(std::clamp<int64_t>(
            next, std::numeric_limits<int32_t>::min(), std::numeric_limits<int32_t>::max()
        ));
    }
    return x;
}
} // namespace oa::formats::fnt
