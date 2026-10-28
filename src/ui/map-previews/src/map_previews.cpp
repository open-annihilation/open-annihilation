// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Thumbnails for the rows on screen: a few decodes a frame, from the bytes
// each kind of map already has.
#include "oa/ui/map_previews/map_previews.hpp"

#include "oa/formats/png.hpp"
#include "oa/formats/tnt.hpp"
#include "oa/ui/frontend_multiplayer/dialogs.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace oa::ui::map_previews {
namespace {

enum class Outcome : uint8_t { decoded, failed, waiting };

/// Copies one terrain range. False when the host cannot.
bool read_terrain(
    const PreviewHost& host, const char* path, uint32_t offset, void* out, uint32_t size
) {
    return host.read_map != nullptr && host.read_map(host.context, path, offset, out, size);
}

/// Centres a picture in a black square of `side` pixels.
///
/// The picture already fits the square on its longer side. The bars are
/// opaque black, so a square card shows the map's shape and nothing else.
///
/// @param picture the picture, no larger than the square on either side
/// @param side the square's side in pixels
/// @return the square
Thumbnail in_square(Thumbnail picture, int32_t side) {
    if (picture.width == side && picture.height == side)
        return picture;
    Thumbnail square;
    square.width = side;
    square.height = side;
    const auto count = static_cast<std::size_t>(side) * static_cast<std::size_t>(side);
    square.rgba.assign(count * 4U, 0);
    for (std::size_t index = 0; index < count; ++index)
        square.rgba[index * 4U + 3U] = 255;
    if (picture.width <= 0 || picture.height <= 0 || picture.width > side || picture.height > side)
        return square;
    const int32_t left = (side - picture.width) / 2;
    const int32_t top = (side - picture.height) / 2;
    for (int32_t row = 0; row < picture.height; ++row) {
        std::memcpy(
            square.rgba.data() +
                (static_cast<std::size_t>(top + row) * static_cast<std::size_t>(side) +
                 static_cast<std::size_t>(left)) *
                    4U,
            picture.rgba.data() +
                static_cast<std::size_t>(row) * static_cast<std::size_t>(picture.width) * 4U,
            static_cast<std::size_t>(picture.width) * 4U
        );
    }
    return square;
}

/// The longer side of a picture fitted to `side`, and the other side at `across`:`along`.
///
/// @param along the map's longer count of squares
/// @param across the map's shorter count of squares
/// @param side the square's side in pixels
/// @return the shorter side in pixels, at least 1
int32_t fitted_across(int32_t along, int32_t across, int32_t side) {
    return static_cast<int32_t>(std::max<int64_t>(1, static_cast<int64_t>(side) * across / along));
}

/// Decodes a base map's minimap into a square thumbnail.
///
/// Reads the terrain file's header and minimap. The picture keeps the map's
/// own shape, its width and height in map squares, as large as fits the
/// square, centred, and the rest of the square is black. The minimap's
/// pixels are coloured with the game palette.
///
/// @param request the base map
/// @param host the terrain reader and the game palette
/// @param side the square's side in pixels
/// @param[out] thumbnail receives the square; unchanged on failure
/// @return decoded, or failed when the minimap or the palette is missing
Outcome decode_base(
    const PreviewRequest& request, const PreviewHost& host, int32_t side, Thumbnail* thumbnail
) {
    if (request.map.empty() || host.palette == nullptr || host.read_map == nullptr)
        return Outcome::failed;
    const std::string path = "maps/" + request.map + ".tnt";

    struct Reader {
        const PreviewHost* host;
        const char* path;
    } reader{&host, path.c_str()};

    const oa::formats::tnt::MapFileReader file{
        &reader, [](void* context, uint32_t offset, void* out, uint32_t size) {
            const auto& open = *static_cast<const Reader*>(context);
            return read_terrain(*open.host, open.path, offset, out, size);
        }
    };
    oa::formats::tnt::RadarPicture radar;
    int32_t cells_wide = 0;
    int32_t cells_high = 0;
    if (!oa::formats::tnt::load_radar_picture(file, radar, &cells_wide, &cells_high))
        return Outcome::failed;
    if (cells_wide <= 0 || cells_high <= 0 || radar.width <= 0 || radar.height <= 0)
        return Outcome::failed;
    int32_t dst_w = side;
    int32_t dst_h = side;
    if (cells_wide >= cells_high)
        dst_h = fitted_across(cells_wide, cells_high, side);
    else
        dst_w = fitted_across(cells_high, cells_wide, side);
    Thumbnail decoded;
    decoded.width = dst_w;
    decoded.height = dst_h;
    decoded.rgba.resize(static_cast<std::size_t>(dst_w) * static_cast<std::size_t>(dst_h) * 4U);
    const auto& palette = *host.palette;
    for (int32_t y = 0; y < dst_h; ++y) {
        const auto src_y = static_cast<int32_t>(static_cast<int64_t>(y) * radar.height / dst_h);
        for (int32_t x = 0; x < dst_w; ++x) {
            const auto src_x = static_cast<int32_t>(static_cast<int64_t>(x) * radar.width / dst_w);
            const auto index = static_cast<std::size_t>(
                radar.pixels
                    [static_cast<std::size_t>(src_y) * static_cast<std::size_t>(radar.width) +
                     static_cast<std::size_t>(src_x)]
            );
            auto* pixel = decoded.rgba.data() +
                          (static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_w) +
                           static_cast<std::size_t>(x)) *
                              4U;
            pixel[0] = palette[index * 4U];
            pixel[1] = palette[index * 4U + 1U];
            pixel[2] = palette[index * 4U + 2U];
            pixel[3] = 255;
        }
    }
    *thumbnail = in_square(std::move(decoded), side);
    return Outcome::decoded;
}

/// Writes one source pixel as opaque or translucent red, green, blue and alpha.
void paint(
    const oa::formats::png::Info& info,
    std::span<const uint8_t> rows,
    std::size_t stride,
    int32_t x,
    int32_t y,
    uint8_t* pixel
) {
    const auto channels = oa::formats::png::channel_count(info.header.color_type);
    const auto* sample =
        rows.data() + static_cast<std::size_t>(y) * stride + static_cast<std::size_t>(x) * channels;
    switch (info.header.color_type) {
    case oa::formats::png::ColorType::grey:
        pixel[0] = pixel[1] = pixel[2] = sample[0];
        pixel[3] = 255;
        break;
    case oa::formats::png::ColorType::grey_alpha:
        pixel[0] = pixel[1] = pixel[2] = sample[0];
        pixel[3] = sample[1];
        break;
    case oa::formats::png::ColorType::rgb:
        pixel[0] = sample[0];
        pixel[1] = sample[1];
        pixel[2] = sample[2];
        pixel[3] = 255;
        break;
    case oa::formats::png::ColorType::rgb_alpha:
        std::memcpy(pixel, sample, 4);
        break;
    case oa::formats::png::ColorType::palette: {
        const auto index = sample[0];
        const oa::formats::png::Rgb colour =
            index < info.palette_entries ? info.palette[index] : oa::formats::png::Rgb{};
        pixel[0] = colour.r;
        pixel[1] = colour.g;
        pixel[2] = colour.b;
        pixel[3] = 255;
        break;
    }
    }
}

/// Scales a picture so its longer side is `side`, by nearest neighbour,
/// then centres it on a black square of that side.
///
/// @param file the picture's bytes
/// @param side the longer side of the result, in pixels
/// @param[out] thumbnail receives the scaled picture; unchanged on failure
/// @return true when the picture decoded
bool decode_png(std::span<const uint8_t> file, int32_t side, Thumbnail* thumbnail) {
    oa::formats::png::Messages messages;
    oa::formats::png::Info info;
    if (!oa::formats::png::read_info(file, messages, &info))
        return false;
    const auto& header = info.header;
    if (header.width == 0 || header.height == 0 || header.width > 0x7fffffffU ||
        header.height > 0x7fffffffU)
        return false;
    oa::formats::png::Transforms transforms;
    transforms.strip_16 = header.bit_depth == 16;
    transforms.unpack = header.bit_depth < 8;
    const auto stride = oa::formats::png::row_bytes(header, transforms);
    if (stride == 0 || header.height > std::numeric_limits<std::size_t>::max() / stride)
        return false;
    std::vector<uint8_t> rows(static_cast<std::size_t>(header.height) * stride);
    const auto progress = oa::formats::png::read_image(file, info, transforms, messages, rows);
    if (progress != oa::formats::png::Progress::rows && progress != oa::formats::png::Progress::end)
        return false;
    const auto src_w = static_cast<int32_t>(header.width);
    const auto src_h = static_cast<int32_t>(header.height);
    int32_t dst_w = side;
    int32_t dst_h = side;
    if (src_w >= src_h)
        dst_h =
            static_cast<int32_t>(std::max<int64_t>(1, static_cast<int64_t>(src_h) * side / src_w));
    else
        dst_w =
            static_cast<int32_t>(std::max<int64_t>(1, static_cast<int64_t>(src_w) * side / src_h));
    Thumbnail decoded;
    decoded.width = dst_w;
    decoded.height = dst_h;
    decoded.rgba.resize(static_cast<std::size_t>(dst_w) * static_cast<std::size_t>(dst_h) * 4U);
    for (int32_t y = 0; y < dst_h; ++y) {
        const auto src_y = static_cast<int32_t>(static_cast<int64_t>(y) * src_h / dst_h);
        for (int32_t x = 0; x < dst_w; ++x) {
            const auto src_x = static_cast<int32_t>(static_cast<int64_t>(x) * src_w / dst_w);
            paint(
                info,
                rows,
                stride,
                src_x,
                src_y,
                decoded.rgba.data() +
                    (static_cast<std::size_t>(y) * static_cast<std::size_t>(dst_w) +
                     static_cast<std::size_t>(x)) *
                        4U
            );
        }
    }
    *thumbnail = in_square(std::move(decoded), side);
    return true;
}

/// Decodes one waiting request.
///
/// @param request the row that asked
/// @param host the bytes and the game palette
/// @param side the square a thumbnail fits into, in pixels
/// @param[out] thumbnail receives a decoded picture
/// @return waiting when an online picture's bytes are not ready yet
Outcome decode_one(
    const PreviewRequest& request, const PreviewHost& host, int32_t side, Thumbnail* thumbnail
) {
    if (request.source == PreviewSource::base)
        return decode_base(request, host, side, thumbnail);
    std::vector<uint8_t> bytes;
    if (request.source == PreviewSource::online) {
        if (host.online_bytes == nullptr || !host.online_bytes(host.context, request.key, &bytes))
            return Outcome::waiting;
    } else if (host.read_file == nullptr || !host.read_file(host.context, request.file, &bytes)) {
        return Outcome::failed;
    }
    return decode_png(bytes, side, thumbnail) ? Outcome::decoded : Outcome::failed;
}

} // namespace

PreviewCache::PreviewCache(std::size_t most_entries, int32_t side)
    : most_entries_(most_entries),
      side_(std::clamp(side, int32_t{1}, oa::ui::frontend_multiplayer::kMapPictureMaxSide)) {
}

const Thumbnail* PreviewCache::ask(const PreviewRequest& request) {
    ++stamp_;
    if (const auto found = by_key_.find(request.key); found != by_key_.end()) {
        found->second->asked = stamp_;
        return &found->second->thumbnail;
    }
    if (failed_.find(request.key) != failed_.end())
        return nullptr;
    for (auto& pending : waiting_) {
        if (pending.request.key == request.key) {
            pending.request = request;
            pending.asked = stamp_;
            return nullptr;
        }
    }
    waiting_.push_back(Waiting{request, stamp_});
    return nullptr;
}

bool PreviewCache::failed(std::string_view key) const {
    return std::any_of(failed_.begin(), failed_.end(), [&](const std::string& failed) {
        return failed == key;
    });
}

void PreviewCache::drop_oldest() {
    auto oldest = cached_.begin();
    for (auto it = cached_.begin(); it != cached_.end(); ++it)
        if (it->asked < oldest->asked)
            oldest = it;
    by_key_.erase(oldest->key);
    cached_.erase(oldest);
}

void PreviewCache::store(std::string key, Thumbnail thumbnail, uint64_t asked) {
    if (most_entries_ == 0)
        return;
    if (const auto found = by_key_.find(key); found != by_key_.end()) {
        found->second->thumbnail = std::move(thumbnail);
        found->second->asked = asked;
        return;
    }
    while (cached_.size() >= most_entries_)
        drop_oldest();
    cached_.push_front(Cached{std::move(key), std::move(thumbnail), asked});
    by_key_.emplace(cached_.front().key, cached_.begin());
}

void PreviewCache::pump(const PreviewHost& host, int32_t most_decodes) {
    if (most_decodes <= 0 || waiting_.empty())
        return;
    std::vector<std::size_t> order(waiting_.size());
    for (std::size_t index = 0; index < order.size(); ++index)
        order[index] = index;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        return waiting_[left].asked > waiting_[right].asked;
    });
    std::vector<char> done(waiting_.size(), 0);
    int32_t decoded = 0;
    for (const std::size_t index : order) {
        if (decoded >= most_decodes)
            break;
        Thumbnail thumbnail;
        Outcome outcome = Outcome::failed;
        try {
            outcome = decode_one(waiting_[index].request, host, side_, &thumbnail);
        } catch (const std::exception&) {
            outcome = Outcome::failed;
        }
        if (outcome == Outcome::waiting)
            continue;
        ++decoded;
        done[index] = 1;
        if (outcome == Outcome::decoded)
            store(waiting_[index].request.key, std::move(thumbnail), waiting_[index].asked);
        else
            failed_.insert(waiting_[index].request.key);
    }
    for (std::size_t index = waiting_.size(); index > 0; --index)
        if (done[index - 1] != 0)
            waiting_.erase(waiting_.begin() + static_cast<std::ptrdiff_t>(index - 1));
}

void PreviewCache::new_frame() {
    waiting_.clear();
}

} // namespace oa::ui::map_previews
