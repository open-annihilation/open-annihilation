// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#include "renderer_water.hpp"
#include "renderer_water_appearance.hpp"
#include <numbers>
#include <algorithm>
#include <cmath>

namespace oa::app::renderer_water {
void WindState::reset(const formats::tnt::Map& map) noexcept {
    // Hash at most 1024 heights, keeping map-load work bounded even on huge maps.
    constexpr std::size_t maximum_samples = 1024;
    constexpr uint32_t hash_prime = 16777619U;
    uint32_t hash = 2166136261U;
    const auto append = [&](uint32_t word) {
        for (unsigned byte = 0; byte < 4; ++byte) {
            hash ^= (word >> (byte * 8)) & 255U;
            hash *= hash_prime;
        }
    };
    append(map.attribute_width);
    append(map.attribute_height);
    append(map.sea_level);
    const auto stride =
        std::max<std::size_t>(1, (map.attributes.size() + maximum_samples - 1) / maximum_samples);
    for (std::size_t index = 0; index < map.attributes.size(); index += stride)
        append(map.attributes[index].height);
    constexpr uint32_t direction_steps = 65536;
    const float angle =
        float(hash % direction_steps) * (2 * std::numbers::pi_v<float> / float(direction_steps));
    value = {std::cos(angle), std::sin(angle), .55F, 0, 0};
}

void WindState::advance(uint32_t now) noexcept {
    constexpr double ticks_per_second = 30;
    constexpr double maximum_pixels_per_second = 24;
    const double travel =
        double(now) * maximum_pixels_per_second * value.strength / ticks_per_second;
    value.drift_x = float(travel * value.x);
    value.drift_z = float(travel * value.z);
}

Levels shown_levels(const Sea& sea, const Levels& chosen) noexcept {
    return sea.lava ? Levels{} : chosen;
}

bool stirred_by_shots(const Sea& sea) noexcept {
    return !sea.lava && !sea.swallows_shots;
}

std::array<float, 3> foam_tint(const std::array<float, 3>& ground) noexcept {
    using namespace renderer_water_appearance;
    // Lightness as the eye weighs the three colours.
    const float lightness =
        std::clamp(0.2126F * ground[0] + 0.7152F * ground[1] + 0.0722F * ground[2], 0.0F, 1.0F);
    const float foam = lightness + (1 - lightness) * foam_lift;
    std::array<float, 3> colour{};
    for (std::size_t channel = 0; channel < colour.size(); ++channel)
        colour[channel] =
            std::clamp(foam + (ground[channel] - lightness) * foam_chroma, 0.0F, 1.0F);
    return colour;
}

float surface_strength(
    Kind kind, float height, float sea, bool water_weapon, bool crossed_surface
) noexcept {
    if (!std::isfinite(height) || !std::isfinite(sea))
        return 0;
    if (kind == Kind::water_impact && crossed_surface && height < sea)
        return 1;
    if (water_weapon)
        return 0;
    constexpr float maximum_deck_height = 64;
    constexpr float surface_tolerance = 4;
    if (kind == Kind::firing || kind == Kind::hit)
        return height >= sea && height <= sea + maximum_deck_height
                   ? (kind == Kind::firing ? .45F : 1.0F)
                   : 0;
    return std::abs(height - sea) <= surface_tolerance ? 1.0F : 0;
}

void History::advance(uint32_t now) noexcept {
    if (seen && now < tick)
        count = 0;
    tick = now;
    seen = true;
    std::size_t retained = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (entries[i].birth <= now && now - entries[i].birth < lifetime_ticks)
            entries[retained++] = entries[i];
    count = retained;
}

void History::add(Disturbance event, bool visible) noexcept {
    if (!visible || !seen || event.birth > tick || tick - event.birth >= lifetime_ticks ||
        !std::isfinite(event.x) || !std::isfinite(event.z) || !std::isfinite(event.strength) ||
        event.strength <= 0)
        return;
    event.strength = std::min(event.strength, 1.0F);
    for (std::size_t i = count; i > 0; --i) {
        auto& prior = entries[i - 1];
        const float dx = prior.x - event.x, dz = prior.z - event.z;
        if (prior.kind == event.kind && event.birth >= prior.birth &&
            event.birth - prior.birth <= 3 && dx * dx + dz * dz <= 144) {
            prior.strength = std::min(1.0F, prior.strength + event.strength * .25F);
            return;
        }
    }
    if (count == capacity) {
        std::move(entries.begin() + 1, entries.end(), entries.begin());
        --count;
    }
    entries[count++] = event;
}

std::span<const Disturbance> History::active() const noexcept {
    return {entries.data(), count};
}

void SurfaceCache::reset(const formats::tnt::Map& map, std::span<const uint8_t> palette) {
    reset_foam(map, palette);
    constexpr uint32_t maximum_edge = 256;
    constexpr float cell = 16;
    stride_ = std::max(
        1U, (std::max(map.attribute_width, map.attribute_height) + maximum_edge - 1) / maximum_edge
    );
    width_ = (map.attribute_width + stride_ - 1) / stride_;
    height_ = (map.attribute_height + stride_ - 1) / stride_;
    world_width_ = map.attribute_width * cell;
    world_height_ = map.attribute_height * cell;
    fetch_.assign(std::size_t(width_) * height_, 0);
    std::vector<uint8_t> wet(fetch_.size());
    for (uint32_t z = 0; z < height_; ++z)
        for (uint32_t x = 0; x < width_; ++x) {
            const auto mx = std::min(map.attribute_width - 1, x * stride_ + stride_ / 2);
            const auto mz = std::min(map.attribute_height - 1, z * stride_ + stride_ / 2);
            const auto index = std::size_t(mz) * map.attribute_width + mx;
            wet[std::size_t(z) * width_ + x] =
                index < map.attributes.size() && map.attributes[index].height < map.sea_level;
        }
    std::vector<std::size_t> body;
    body.reserve(fetch_.size());
    for (std::size_t start = 0; start < wet.size(); ++start) {
        if (wet[start] != 1)
            continue;
        body.clear();
        body.push_back(start);
        wet[start] = 2;
        for (std::size_t n = 0; n < body.size(); ++n) {
            const auto index = body[n];
            const auto x = index % width_, z = index / width_;
            const auto visit = [&](std::size_t next) {
                if (wet[next] == 1) {
                    wet[next] = 2;
                    body.push_back(next);
                }
            };
            if (x > 0)
                visit(index - 1);
            if (x + 1 < width_)
                visit(index + 1);
            if (z > 0)
                visit(index - width_);
            if (z + 1 < height_)
                visit(index + width_);
        }
        const float body_factor =
            std::clamp(std::sqrt(float(body.size())) * float(stride_) / 32.0F, .08F, 1.0F);
        for (const auto index : body) {
            const int x = int(index % width_), z = int(index / width_);
            int open = 0;
            constexpr int reach = 8;
            for (const auto direction : {std::array<int, 2>{1, 0}, {-1, 0}, {0, 1}, {0, -1}})
                for (int step = 1; step <= reach; ++step) {
                    const int sx = x + direction[0] * step, sz = z + direction[1] * step;
                    if (sx < 0 || sz < 0 || sx >= int(width_) || sz >= int(height_) ||
                        wet[std::size_t(sz) * width_ + sx] == 0)
                        break;
                    ++open;
                }
            fetch_[index] = body_factor * (.2F + .8F * float(open) / float(4 * reach));
        }
    }
}

float SurfaceCache::fetch(float x, float z) const noexcept {
    if (!std::isfinite(x) || !std::isfinite(z) || x < 0 || z < 0 || x >= world_width_ ||
        z >= world_height_)
        return 0;
    const auto index =
        std::size_t(z / (16.0F * stride_)) * width_ + std::size_t(x / (16.0F * stride_));
    return index < fetch_.size() ? fetch_[index] : 0;
}

float SurfaceCache::smooth_fetch(float x, float z) const noexcept {
    if (!std::isfinite(x) || !std::isfinite(z) || x < 0 || z < 0 || x >= world_width_ ||
        z >= world_height_ || width_ == 0 || height_ == 0 ||
        fetch_.size() < std::size_t(width_) * height_)
        return 0;
    const float edge = 16.0F * float(stride_);
    const float across = x / edge - 0.5F, down = z / edge - 0.5F;
    const int column = int(std::floor(across)), row = int(std::floor(down));
    const float tx = across - float(column), tz = down - float(row);
    const auto at = [&](int c, int r) {
        c = std::clamp(c, 0, int(width_) - 1);
        r = std::clamp(r, 0, int(height_) - 1);
        return fetch_[std::size_t(r) * width_ + std::size_t(c)];
    };
    const float upper = at(column, row) + (at(column + 1, row) - at(column, row)) * tx;
    const float lower = at(column, row + 1) + (at(column + 1, row + 1) - at(column, row + 1)) * tx;
    return upper + (lower - upper) * tz;
}

void SurfaceCache::reset_foam(const formats::tnt::Map& map, std::span<const uint8_t> palette) {
    namespace tnt_layout = formats::tnt::layout;
    foam_.clear();
    foam_width_ = foam_height_ = 0;
    constexpr std::size_t palette_bytes = 256 * 3;
    const auto tile_edge = uint32_t(tnt_layout::tile_edge_pixels);
    if (palette.size() < palette_bytes || map.tile_width == 0 || map.tile_height == 0 ||
        map.tile_indices.size() < std::size_t(map.tile_width) * map.tile_height)
        return;
    // Cells of 16 picture pixels, or of a multiple of 16 that keeps the
    // grid at most 512 cells a side; each averages 4 by 4 of its pixels.
    constexpr uint32_t maximum_edge = 512;
    constexpr uint32_t samples = 4;
    const uint32_t picture_width = map.tile_width * tile_edge;
    const uint32_t picture_height = map.tile_height * tile_edge;
    const uint32_t cells_across = map.tile_width * tnt_layout::attribute_cells_per_tile_edge;
    const uint32_t cells_down = map.tile_height * tnt_layout::attribute_cells_per_tile_edge;
    const uint32_t stride =
        std::max(1U, (std::max(cells_across, cells_down) + maximum_edge - 1) / maximum_edge);
    const uint32_t span = 16 * stride;
    const uint32_t width = (cells_across + stride - 1) / stride;
    const uint32_t height = (cells_down + stride - 1) / stride;
    const std::array<float, 3> plain{
        renderer_water_appearance::foam_red,
        renderer_water_appearance::foam_green,
        renderer_water_appearance::foam_blue
    };
    std::vector<std::array<float, 3>> ground(std::size_t(width) * height, plain);
    for (uint32_t row = 0; row < height; ++row)
        for (uint32_t column = 0; column < width; ++column) {
            std::array<float, 3> sum{};
            uint32_t count = 0;
            for (uint32_t down = 0; down < samples; ++down)
                for (uint32_t along = 0; along < samples; ++along) {
                    const uint32_t x = column * span + (2 * along + 1) * span / (2 * samples);
                    const uint32_t y = row * span + (2 * down + 1) * span / (2 * samples);
                    if (x >= picture_width || y >= picture_height)
                        continue;
                    const auto tile =
                        map.tile_indices
                            [std::size_t(y / tile_edge) * map.tile_width + x / tile_edge];
                    const auto pixel = std::size_t(tile) * tnt_layout::tile_bytes +
                                       std::size_t(y % tile_edge) * tile_edge + x % tile_edge;
                    if (pixel >= map.tile_palette_indices.size())
                        continue;
                    const auto entry = std::size_t(map.tile_palette_indices[pixel]) * 3;
                    for (std::size_t channel = 0; channel < sum.size(); ++channel)
                        sum[channel] += palette[entry + channel];
                    ++count;
                }
            if (count != 0)
                for (std::size_t channel = 0; channel < sum.size(); ++channel)
                    ground[std::size_t(row) * width + column][channel] =
                        sum[channel] / (255.0F * float(count));
        }
    // Each cell's foam from the ground of the three by three cells around
    // it, so that the foam takes the shore's colour rather than one cell's.
    foam_.resize(ground.size());
    for (uint32_t row = 0; row < height; ++row)
        for (uint32_t column = 0; column < width; ++column) {
            std::array<float, 3> sum{};
            float count = 0;
            for (int dr = -1; dr <= 1; ++dr)
                for (int dc = -1; dc <= 1; ++dc) {
                    const int r = int(row) + dr, c = int(column) + dc;
                    if (r < 0 || c < 0 || r >= int(height) || c >= int(width))
                        continue;
                    const auto& colour = ground[std::size_t(r) * width + std::size_t(c)];
                    for (std::size_t channel = 0; channel < sum.size(); ++channel)
                        sum[channel] += colour[channel];
                    ++count;
                }
            for (auto& channel : sum)
                channel /= count;
            const auto foam = foam_tint(sum);
            auto& cell = foam_[std::size_t(row) * width + column];
            for (std::size_t channel = 0; channel < cell.size(); ++channel)
                cell[channel] = uint8_t(std::lround(foam[channel] * 255.0F));
        }
    foam_width_ = width;
    foam_height_ = height;
    foam_cell_ = float(span);
}

std::array<float, 3> SurfaceCache::foam_colour(float x, float y) const noexcept {
    if (foam_width_ == 0 || foam_height_ == 0 ||
        foam_.size() < std::size_t(foam_width_) * foam_height_ || !std::isfinite(x) ||
        !std::isfinite(y))
        return {
            renderer_water_appearance::foam_red,
            renderer_water_appearance::foam_green,
            renderer_water_appearance::foam_blue
        };
    const float across = x / foam_cell_ - 0.5F, down = y / foam_cell_ - 0.5F;
    const float left = std::floor(across), top = std::floor(down);
    const float tx = across - left, ty = down - top;
    // Far outside the map a point takes its nearest edge cell.
    const auto clamped = [](float value, uint32_t count) {
        return std::size_t(std::clamp(value, 0.0F, float(count - 1)));
    };
    const auto at = [&](float c, float r, std::size_t channel) {
        return float(
                   foam_[clamped(r, foam_height_) * foam_width_ + clamped(c, foam_width_)][channel]
               ) /
               255.0F;
    };
    std::array<float, 3> colour{};
    for (std::size_t channel = 0; channel < colour.size(); ++channel) {
        const float upper =
            at(left, top, channel) + (at(left + 1, top, channel) - at(left, top, channel)) * tx;
        const float lower = at(left, top + 1, channel) +
                            (at(left + 1, top + 1, channel) - at(left, top + 1, channel)) * tx;
        colour[channel] = upper + (lower - upper) * ty;
    }
    return colour;
}
} // namespace oa::app::renderer_water
