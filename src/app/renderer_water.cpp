// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#include "renderer_water.hpp"
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

void SurfaceCache::reset(const formats::tnt::Map& map) {
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
} // namespace oa::app::renderer_water
