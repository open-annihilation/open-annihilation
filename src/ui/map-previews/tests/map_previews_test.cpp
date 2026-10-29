// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Thumbnails decode only for the rows asked for this frame, a few at a time.
#include "oa/ui/map_previews/map_previews.hpp"

#include "oa/data/campaign/campaign_assets.hpp"
#include "oa/data/campaign/map_catalog.hpp"
#include "oa/formats/png.hpp"
#include "oa/formats/tnt.hpp"
#include "oa/test/game_assets.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using oa::ui::map_previews::PreviewCache;
using oa::ui::map_previews::PreviewHost;
using oa::ui::map_previews::PreviewRequest;
using oa::ui::map_previews::PreviewSource;
using oa::ui::map_previews::Thumbnail;

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::cerr << "FAIL: " << what << '\n';
        ++failures;
    }
}

/// Bytes a stand-in host can hand to the cache.
struct StandIn {
    std::map<std::string, std::vector<uint8_t>> files;
    std::map<std::string, std::vector<uint8_t>> online;
    bool online_ready = true;
    int map_reads = 0;
    int file_reads = 0;
    int online_reads = 0;
    uint64_t map_bytes = 0;
};

bool read_map(void* context, const char* path, uint32_t offset, void* out, uint32_t size) {
    auto* stand = static_cast<StandIn*>(context);
    ++stand->map_reads;
    const auto found = stand->files.find(path != nullptr ? path : "");
    if (found == stand->files.end() || static_cast<uint64_t>(offset) + size > found->second.size())
        return false;
    std::memcpy(out, found->second.data() + offset, size);
    stand->map_bytes += size;
    return true;
}

bool read_file(void* context, const std::filesystem::path& file, std::vector<uint8_t>* out) {
    auto* stand = static_cast<StandIn*>(context);
    ++stand->file_reads;
    const auto found = stand->files.find(file.generic_string());
    if (found == stand->files.end() || out == nullptr)
        return false;
    *out = found->second;
    return true;
}

bool online_bytes(void* context, const std::string& key, std::vector<uint8_t>* out) {
    auto* stand = static_cast<StandIn*>(context);
    ++stand->online_reads;
    if (!stand->online_ready || out == nullptr)
        return false;
    const auto found = stand->online.find(key);
    if (found == stand->online.end())
        return false;
    *out = found->second;
    return true;
}

PreviewHost host_of(StandIn& stand, const oa::PaletteBytes* palette) {
    PreviewHost host;
    host.context = &stand;
    host.read_map = read_map;
    host.read_file = read_file;
    host.online_bytes = online_bytes;
    host.palette = palette;
    return host;
}

PreviewRequest
request_of(std::string key, PreviewSource source, std::string map, std::filesystem::path file) {
    PreviewRequest request;
    request.key = std::move(key);
    request.source = source;
    request.map = std::move(map);
    request.file = std::move(file);
    return request;
}

/// A terrain file of `cells_wide` by `cells_high` map squares, with a four-pixel
/// minimap (indices 1, 2, 3 and 4) and a padded body.
std::vector<uint8_t> stand_in_terrain(uint32_t cells_wide = 16, uint32_t cells_high = 16) {
    std::vector<uint8_t> terrain(4096, 0);
    const auto put = [&](std::size_t offset, uint32_t value) {
        terrain[offset] = static_cast<uint8_t>(value);
        terrain[offset + 1] = static_cast<uint8_t>(value >> 8);
        terrain[offset + 2] = static_cast<uint8_t>(value >> 16);
        terrain[offset + 3] = static_cast<uint8_t>(value >> 24);
    };
    using oa::formats::tnt::layout::header_bytes;
    using oa::formats::tnt::layout::minimap_header_bytes;
    put(0, static_cast<uint32_t>(oa::formats::tnt::Version::total_annihilation));
    put(4, cells_wide);
    put(8, cells_high);
    put(0x28, static_cast<uint32_t>(header_bytes));
    put(0x2c, oa::formats::tnt::layout::minimap_present_flag);
    put(header_bytes, 2);
    put(header_bytes + 4, 2);
    terrain[header_bytes + minimap_header_bytes + 0] = 1;
    terrain[header_bytes + minimap_header_bytes + 1] = 2;
    terrain[header_bytes + minimap_header_bytes + 2] = 3;
    terrain[header_bytes + minimap_header_bytes + 3] = 4;
    return terrain;
}

std::vector<uint8_t> encode_png(
    uint32_t width,
    uint32_t height,
    oa::formats::png::ColorType color,
    std::span<const uint8_t> rows,
    std::span<const oa::formats::png::Rgb> palette
) {
    oa::formats::png::Image image;
    image.header.width = width;
    image.header.height = height;
    image.header.bit_depth = 8;
    image.header.color_type = color;
    image.palette = palette;
    image.rows = rows;
    std::vector<uint8_t> file;
    expect(oa::formats::png::write(image, &file), "a preview picture encodes");
    return file;
}

/// True when the pixel is opaque black.
bool black_pixel(const Thumbnail& thumb, int32_t x, int32_t y) {
    const uint8_t* pixel =
        thumb.rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(thumb.width) +
                             static_cast<std::size_t>(x)) *
                                4U;
    return pixel[0] == 0 && pixel[1] == 0 && pixel[2] == 0 && pixel[3] == 255;
}

/// The minimap buffer's meaningful top-left, as a thumbnail keeps it.
///
/// @param squares_wide the map's width in squares
/// @param squares_high the map's height in squares
/// @param buffer_wide the buffer's width
/// @param buffer_high the buffer's height
/// @return the meaningful width and height
std::pair<int32_t, int32_t>
meaningful(int32_t squares_wide, int32_t squares_high, int32_t buffer_wide, int32_t buffer_high) {
    constexpr int32_t kRight = 32;
    constexpr int32_t kBottom = 128;
    constexpr int32_t kPixels = 16;
    const int32_t playable_wide = squares_wide * kPixels - kRight;
    const int32_t playable_high = squares_high * kPixels - kBottom;
    int32_t used_wide = buffer_wide;
    int32_t used_high = buffer_high;
    if (playable_wide > 0 && playable_high > 0) {
        if (playable_wide >= playable_high)
            used_high = static_cast<int32_t>(
                static_cast<int64_t>(buffer_high) * playable_high / playable_wide
            );
        else
            used_wide = static_cast<int32_t>(
                static_cast<int64_t>(buffer_wide) * playable_wide / playable_high
            );
    }
    if (used_wide < 1)
        used_wide = buffer_wide;
    if (used_high < 1)
        used_high = buffer_high;
    return {std::min(used_wide, buffer_wide), std::min(used_high, buffer_high)};
}

/// A map of `cells_wide` by `cells_high` squares keeps that shape in the
/// square: centred, with black bars, and the meaningful minimap's four
/// corners at the picture's four corners.
void expect_shape(int32_t cells_wide, int32_t cells_high, int32_t side) {
    StandIn stand;
    stand.files["maps/ridge.tnt"] =
        stand_in_terrain(static_cast<uint32_t>(cells_wide), static_cast<uint32_t>(cells_high));
    oa::PaletteBytes palette{};
    palette[1 * 4 + 0] = 10;
    palette[2 * 4 + 0] = 20;
    palette[3 * 4 + 1] = 30;
    palette[4 * 4 + 2] = 40;
    const PreviewHost host = host_of(stand, &palette);
    PreviewCache cache(4, side);
    const auto request = request_of("ridge", PreviewSource::base, "ridge", {});
    cache.ask(request);
    cache.pump(host, 1);
    const Thumbnail* thumb = cache.ask(request);
    expect(
        thumb != nullptr && thumb->width == side && thumb->height == side, "the picture is square"
    );
    if (thumb == nullptr || thumb->width != side || thumb->height != side)
        return;
    const int32_t picture_w =
        cells_wide >= cells_high ? side : std::max(1, side * cells_wide / cells_high);
    const int32_t picture_h =
        cells_wide >= cells_high ? std::max(1, side * cells_high / cells_wide) : side;
    const int32_t left = (side - picture_w) / 2;
    const int32_t top = (side - picture_h) / 2;
    const auto [used_wide, used_high] = meaningful(cells_wide, cells_high, 2, 2);
    const auto source = [&](int32_t x, int32_t y) {
        const int32_t column = x * used_wide / picture_w;
        const int32_t row = y * used_high / picture_h;
        return row * 2 + column;
    };
    const auto at = [&](int32_t x, int32_t y) {
        return thumb->rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(side) +
                                     static_cast<std::size_t>(x)) *
                                        4U;
    };
    const uint8_t corners[] = {10, 20, 30, 40};
    const int channel[] = {0, 0, 1, 2};
    const int32_t places[][2] = {
        {0, 0}, {picture_w - 1, 0}, {0, picture_h - 1}, {picture_w - 1, picture_h - 1}
    };
    for (int place = 0; place < 4; ++place) {
        const int32_t x = places[place][0];
        const int32_t y = places[place][1];
        expect(
            at(left + x, top + y)[channel[source(x, y)]] == corners[source(x, y)],
            "a meaningful corner is the picture's corner"
        );
    }
    bool bars = true;
    if (top > 0)
        bars = black_pixel(*thumb, side / 2, 0);
    if (left > 0)
        bars = bars && black_pixel(*thumb, 0, side / 2);
    expect(bars, "the rest of the square is black");
    expect(
        !black_pixel(*thumb, left + picture_w / 2, top + picture_h / 2),
        "the picture itself is not a bar"
    );
}

/// A terrain file whose minimap buffer is larger than the map's picture.
/// The picture sits in the top-left; the rest of the buffer is one void colour.
std::vector<uint8_t> padded_minimap() {
    constexpr uint32_t kCellsWide = 32;
    constexpr uint32_t kCellsHigh = 16;
    constexpr uint32_t kBuffer = 16;
    // The meaningful part is the top 4 rows of the 16-row buffer.
    constexpr int32_t kPictureRows = 4;
    constexpr uint8_t kVoid = 9;
    std::vector<uint8_t> terrain(4096, 0);
    const auto put = [&](std::size_t offset, uint32_t value) {
        terrain[offset] = static_cast<uint8_t>(value);
        terrain[offset + 1] = static_cast<uint8_t>(value >> 8);
        terrain[offset + 2] = static_cast<uint8_t>(value >> 16);
        terrain[offset + 3] = static_cast<uint8_t>(value >> 24);
    };
    using oa::formats::tnt::layout::header_bytes;
    using oa::formats::tnt::layout::minimap_header_bytes;
    put(0, static_cast<uint32_t>(oa::formats::tnt::Version::total_annihilation));
    put(4, kCellsWide);
    put(8, kCellsHigh);
    put(0x28, static_cast<uint32_t>(header_bytes));
    put(0x2c, oa::formats::tnt::layout::minimap_present_flag);
    put(header_bytes, kBuffer);
    put(header_bytes + 4, kBuffer);
    uint8_t* pixels = terrain.data() + header_bytes + minimap_header_bytes;
    std::fill_n(pixels, kBuffer * kBuffer, kVoid);
    for (uint32_t row = 0; row < kPictureRows; ++row)
        std::fill_n(pixels + row * kBuffer, kBuffer, static_cast<uint8_t>(5));
    pixels[0] = 1;
    pixels[kBuffer - 1] = 2;
    pixels[(kPictureRows - 1) * kBuffer] = 3;
    pixels[(kPictureRows - 1) * kBuffer + kBuffer - 1] = 4;
    return terrain;
}

/// A padded minimap keeps the picture and drops the void colour. The
/// picture's corners land on the fitted rectangle's corners.
void expect_no_void() {
    constexpr int32_t kSide = 32;
    constexpr int32_t kCellsWide = 32;
    constexpr int32_t kCellsHigh = 16;
    constexpr uint8_t kVoidRed = 80;
    constexpr uint8_t kVoidGreen = 160;
    constexpr uint8_t kVoidBlue = 255;
    StandIn stand;
    stand.files["maps/ridge.tnt"] = padded_minimap();
    oa::PaletteBytes palette{};
    palette[1 * 4 + 0] = 10;
    palette[2 * 4 + 0] = 20;
    palette[3 * 4 + 1] = 30;
    palette[4 * 4 + 2] = 40;
    palette[5 * 4 + 0] = 50;
    palette[9 * 4 + 0] = kVoidRed;
    palette[9 * 4 + 1] = kVoidGreen;
    palette[9 * 4 + 2] = kVoidBlue;
    const PreviewHost host = host_of(stand, &palette);
    PreviewCache cache(4, kSide);
    const auto request = request_of("ridge", PreviewSource::base, "ridge", {});
    cache.ask(request);
    cache.pump(host, 1);
    const Thumbnail* thumb = cache.ask(request);
    expect(
        thumb != nullptr && thumb->width == kSide && thumb->height == kSide,
        "a padded minimap decodes"
    );
    if (thumb == nullptr)
        return;
    bool void_colour = false;
    for (std::size_t index = 0; index + 3 < thumb->rgba.size(); index += 4) {
        if (thumb->rgba[index] == kVoidRed && thumb->rgba[index + 1] == kVoidGreen &&
            thumb->rgba[index + 2] == kVoidBlue)
            void_colour = true;
    }
    expect(!void_colour, "a thumbnail holds no void colour");
    const int32_t picture_w = kSide;
    const int32_t picture_h = kSide * kCellsHigh / kCellsWide;
    const int32_t left = 0;
    const int32_t top = (kSide - picture_h) / 2;
    const auto [used_wide, used_high] = meaningful(kCellsWide, kCellsHigh, 16, 16);
    const auto at = [&](int32_t x, int32_t y) {
        return thumb->rgba.data() + (static_cast<std::size_t>(y) * static_cast<std::size_t>(kSide) +
                                     static_cast<std::size_t>(x)) *
                                        4U;
    };
    const int32_t src_right = (picture_w - 1) * used_wide / picture_w;
    const int32_t src_bottom = (picture_h - 1) * used_high / picture_h;
    expect(
        src_right == used_wide - 1 && src_bottom == used_high - 1,
        "the fitted corners reach the picture"
    );
    expect(at(left, top)[0] == 10, "the picture's top left is the rectangle's");
    expect(at(left + picture_w - 1, top)[0] == 20, "the picture's top right is the rectangle's");
    expect(at(left, top + picture_h - 1)[1] == 30, "the picture's bottom left is the rectangle's");
    expect(
        at(left + picture_w - 1, top + picture_h - 1)[2] == 40,
        "the picture's bottom right is the rectangle's"
    );
    expect(black_pixel(*thumb, kSide / 2, 0), "the square above the picture is black");
}

void synthetic_tests() {
    expect_shape(18, 6, 18);
    expect_shape(6, 18, 18);
    expect_shape(16, 16, 16);
    expect_no_void();
    constexpr int32_t kSide = 4;
    StandIn stand;
    stand.files["maps/ridge.tnt"] = stand_in_terrain();
    const uint8_t rgb_rows[] = {255, 0, 0, 0, 0, 255};
    stand.files["packs/ridge.png"] =
        encode_png(2, 1, oa::formats::png::ColorType::rgb, rgb_rows, {});
    const oa::formats::png::Rgb colours[] = {{0, 255, 0}, {0, 0, 255}};
    const uint8_t index_rows[] = {0, 1};
    stand.online["catalogue/ridge.png"] =
        encode_png(2, 1, oa::formats::png::ColorType::palette, index_rows, colours);
    stand.files["packs/broken.png"] = {1, 2, 3, 4};

    oa::PaletteBytes palette{};
    palette[4] = 255;
    palette[5] = 16;
    palette[6] = 16;
    const PreviewHost host = host_of(stand, &palette);

    PreviewCache cache(8, kSide);
    const auto terrain = request_of("ridge", PreviewSource::base, "ridge", {});
    expect(cache.ask(terrain) == nullptr, "a row asked for is not decoded yet");
    expect(stand.map_reads == 0, "asking does not read the terrain");
    cache.pump(host, 4);
    const Thumbnail* decoded = cache.ask(terrain);
    expect(
        decoded != nullptr && decoded->width == kSide && decoded->height == kSide,
        "pump decodes the row"
    );
    const uint64_t header_and_minimap =
        oa::formats::tnt::layout::header_bytes + oa::formats::tnt::layout::minimap_header_bytes + 4;
    expect(
        stand.map_bytes == header_and_minimap &&
            stand.map_bytes < stand.files["maps/ridge.tnt"].size(),
        "a base thumbnail reads the header and the minimap"
    );
    expect(!cache.failed("ridge"), "a decoded row has not failed");

    const auto picture = request_of("pack-ridge", PreviewSource::pack, "ridge", "packs/ridge.png");
    const int map_reads = stand.map_reads;
    expect(cache.ask(picture) == nullptr, "a pack preview is not decoded until pump");
    cache.pump(host, 4);
    const Thumbnail* pack = cache.ask(picture);
    expect(
        pack != nullptr && pack->width == kSide && pack->height == kSide && pack->rgba[0] == 0 &&
            pack->rgba[(static_cast<std::size_t>(kSide) + 0) * 4U] == 255,
        "a pack preview keeps its shape, centred on black"
    );
    expect(stand.map_reads == map_reads, "a pack preview does not read a terrain file");

    stand.online_ready = false;
    const auto online = request_of("catalogue/ridge.png", PreviewSource::online, {}, {});
    const auto later = request_of("later", PreviewSource::pack, {}, "packs/ridge.png");
    PreviewCache waiting(8, kSide);
    expect(waiting.ask(later) == nullptr, "a ready row is asked for");
    expect(waiting.ask(online) == nullptr, "an online picture is asked for after it");
    const int files_before = stand.file_reads;
    waiting.pump(host, 1);
    expect(
        waiting.ask(online) == nullptr && !waiting.failed(online.key),
        "bytes that are not ready stay waiting"
    );
    expect(waiting.ask(later) != nullptr, "a picture that is not ready does not take the decode");
    expect(stand.file_reads == files_before + 1, "the ready row was the one decoded");
    stand.online_ready = true;
    waiting.pump(host, 1);
    const Thumbnail* fetched = waiting.ask(online);
    expect(
        fetched != nullptr && fetched->width == kSide && fetched->height == kSide &&
            fetched->rgba[(static_cast<std::size_t>(kSide) + 0) * 4U + 1U] == 255,
        "an online picture decodes once its bytes are ready"
    );

    PreviewCache dropped(8, kSide);
    expect(dropped.ask(picture) == nullptr, "a row is waiting");
    dropped.new_frame();
    const int files_after_drop = stand.file_reads;
    dropped.pump(host, 4);
    expect(stand.file_reads == files_after_drop, "a row not asked for again is not decoded");
    expect(
        dropped.ask(picture) == nullptr && !dropped.failed(picture.key),
        "a forgotten row is still waiting to be asked for"
    );

    PreviewCache few(8, kSide);
    const auto first = request_of("one", PreviewSource::pack, {}, "packs/ridge.png");
    const auto second = request_of("two", PreviewSource::pack, {}, "packs/ridge.png");
    const auto third = request_of("three", PreviewSource::pack, {}, "packs/ridge.png");
    few.ask(first);
    few.ask(second);
    few.ask(third);
    const int before_two = stand.file_reads;
    few.pump(host, 2);
    expect(stand.file_reads == before_two + 2, "pump decodes the two most recent rows");
    expect(
        few.ask(third) != nullptr && few.ask(second) != nullptr,
        "the two most recent rows are decoded"
    );
    expect(few.ask(first) == nullptr && !few.failed(first.key), "an older row is still waiting");

    PreviewCache broken(8, kSide);
    const auto bad = request_of("bad", PreviewSource::pack, {}, "packs/broken.png");
    broken.ask(bad);
    const int before_bad = stand.file_reads;
    broken.pump(host, 4);
    expect(broken.failed("bad") && broken.ask(bad) == nullptr, "a broken picture fails");
    broken.pump(host, 4);
    broken.ask(bad);
    broken.pump(host, 4);
    expect(stand.file_reads == before_bad + 1, "a failed picture is not decoded again");

    PreviewCache bounded(2, kSide);
    const auto alpha = request_of("alpha", PreviewSource::pack, {}, "packs/ridge.png");
    const auto beta = request_of("beta", PreviewSource::pack, {}, "packs/ridge.png");
    const auto gamma = request_of("gamma", PreviewSource::pack, {}, "packs/ridge.png");
    bounded.ask(alpha);
    bounded.ask(beta);
    bounded.pump(host, 4);
    bounded.ask(gamma);
    bounded.pump(host, 4);
    expect(bounded.ask(alpha) == nullptr, "the least recently asked thumbnail is dropped");
    expect(
        bounded.ask(beta) != nullptr && bounded.ask(gamma) != nullptr,
        "the cache keeps the newer thumbnails"
    );
}

std::vector<std::string> unpack(const char* names, int32_t count) {
    std::vector<std::string> out;
    if (names == nullptr)
        return out;
    for (int32_t index = 0; index < count; ++index) {
        out.emplace_back(names);
        names += out.back().size() + 1;
    }
    return out;
}

/// Every base map of the installed game, from its header and minimap only.
void installed_tests(const oa::AssetStore& assets) {
    auto files = oa::data::campaign::campaign_asset_files(assets);
    files.translate = nullptr;
    oa::data::campaign::MapList list{};
    char* names = nullptr;
    const auto count =
        oa::data::campaign::map_build_multiplayer_list(list, files, {}, &names, false, false);
    const std::vector<std::string> maps = unpack(names, count);
    std::free(names);
    oa::data::campaign::map_clear_list_cache(list, nullptr);
    expect(!maps.empty(), "the installed game lists base maps");
    if (maps.empty())
        return;

    struct Reader {
        const oa::AssetStore* assets{};
        uint64_t handed = 0;
    } reader{&assets};

    oa::PaletteBytes palette{};
    try {
        const auto bytes = assets.read("palettes/palette.pal").bytes;
        expect(bytes.size() == palette.size(), "the game palette is 256 colours");
        if (bytes.size() == palette.size())
            std::copy(bytes.begin(), bytes.end(), palette.begin());
    } catch (const std::exception&) {
        expect(false, "the game palette is readable");
        return;
    }

    PreviewHost host;
    host.context = &reader;
    host.palette = &palette;
    host.read_map = [](void* context, const char* path, uint32_t offset, void* out, uint32_t size) {
        auto* open = static_cast<Reader*>(context);
        try {
            const bool ok = open->assets->read_chunk(
                path != nullptr ? path : "", offset, std::span(static_cast<uint8_t*>(out), size)
            );
            if (ok)
                open->handed += size;
            return ok;
        } catch (const std::exception&) {
            return false;
        }
    };

    PreviewCache one(4, 32);
    const auto& sample = maps.front();
    one.ask(request_of(sample, PreviewSource::base, sample, {}));
    one.pump(host, 4);
    const Thumbnail* sample_thumb = one.ask(request_of(sample, PreviewSource::base, sample, {}));
    const auto sample_size = assets.file_size("maps/" + sample + ".tnt");
    expect(sample_thumb != nullptr && !one.failed(sample), "one base map's thumbnail decodes");
    expect(
        reader.handed > oa::formats::tnt::layout::header_bytes && reader.handed < sample_size,
        "reading one base map reads its header and minimap, not the whole terrain file"
    );

    PreviewCache cache(maps.size() + 8, 32);
    bool pending = true;
    for (int round = 0; pending && round < static_cast<int>(maps.size()) + 2; ++round) {
        pending = false;
        for (const auto& name : maps) {
            if (cache.ask(request_of(name, PreviewSource::base, name, {})) == nullptr &&
                !cache.failed(name))
                pending = true;
        }
        if (pending)
            cache.pump(host, 4);
    }
    for (const auto& name : maps)
        expect(
            cache.ask(request_of(name, PreviewSource::base, name, {})) != nullptr &&
                !cache.failed(name),
            "every base map's thumbnail decodes"
        );
}

} // namespace

int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv))
        installed_tests(oa::test::require_game_assets("map preview thumbnails"));
    else
        synthetic_tests();
    if (failures != 0) {
        std::cerr << failures << " failure(s)\n";
        return 1;
    }
    std::cout << "map preview tests passed\n";
    return 0;
}
