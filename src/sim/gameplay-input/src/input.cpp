// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/gameplay_input/input.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>

namespace oa::sim::gameplay_input {
namespace {
int32_t wrap_add(int32_t a, int32_t b) noexcept {
    return std::bit_cast<int32_t>(std::bit_cast<uint32_t>(a) + std::bit_cast<uint32_t>(b));
}

int32_t wrap_sub(int32_t a, int32_t b) noexcept {
    return std::bit_cast<int32_t>(std::bit_cast<uint32_t>(a) - std::bit_cast<uint32_t>(b));
}

int32_t wrap_mul(int32_t a, int32_t b) noexcept {
    return std::bit_cast<int32_t>(std::bit_cast<uint32_t>(a) * std::bit_cast<uint32_t>(b));
}

int32_t high(int32_t v) noexcept {
    return static_cast<int16_t>(std::bit_cast<uint32_t>(v) >> 16U);
}

int32_t fixed(int32_t v) noexcept {
    return std::bit_cast<int32_t>(std::bit_cast<uint32_t>(v) << 16U);
}

} // namespace

ScreenPoint project(
    const formats::objects3d::FixedVector3& local,
    const formats::objects3d::FixedVector3& world,
    const Camera& camera
) noexcept {
    const auto x = wrap_sub(wrap_add(local.x, world.x), fixed(camera.pixel_x));
    const auto z = wrap_sub(wrap_sub(world.z, fixed(camera.pixel_z)), local.z);
    const auto y = wrap_add(local.y, world.y);
    return {high(x) + 128, high(z) - (high(y) >> 1) + 32};
}

bool inside_clockwise(std::span<const ScreenPoint> polygon, ScreenPoint point) noexcept {
    const auto count = static_cast<int32_t>(polygon.size());
    if (count < 3)
        return false;
    for (int32_t i = 0; i < count; ++i) {
        const auto& a = polygon[i];
        const auto& b = polygon[(i + 1) % count];
        const auto left = wrap_mul(wrap_sub(b.y, a.y), wrap_sub(point.x, a.x));
        const auto right = wrap_mul(wrap_sub(point.y, a.y), wrap_sub(b.x, a.x));
        if (left <= right)
            return false;
    }
    return true;
}

bool hits_root_bounds(const PickUnit& unit, const Camera& camera, ScreenPoint point) {
    if (!unit.model || unit.model->objects.empty())
        return false;
    const auto [minimum, maximum] = formats::objects3d::object_bounds(unit.model->objects.front());
    std::array<formats::objects3d::FixedVector3, 4> corners{
        {{minimum.x, minimum.y, minimum.z},
         {maximum.x, minimum.y, minimum.z},
         {maximum.x, minimum.y, maximum.z},
         {minimum.x, minimum.y, maximum.z}}
    };
    std::array<ScreenPoint, 4> polygon{};
    for (std::size_t i = 0; i < 4; ++i) {
        corners[i] = sim::model_runtime::rotate_vector(corners[i], unit.rotation);
        polygon[i] = project(corners[i], unit.position, camera);
    }
    return inside_clockwise(polygon, point);
}

std::vector<uint16_t>
hit_candidates(std::span<const PickUnit> units, const Camera& camera, ScreenPoint point) {
    std::vector<uint16_t> result;
    result.reserve(units.size());
    for (const auto& unit : units)
        if (hits_root_bounds(unit, camera, point))
            result.push_back(unit.id);
    return result;
}

formats::objects3d::FixedVector3 terrain_intersection(
    const sim::unit_movement::Terrain& terrain,
    int32_t map_x,
    int32_t projected_z,
    int32_t map_width,
    int32_t map_height
) {
    if (map_width <= 0 || map_height <= 0)
        return {};
    map_x = std::clamp(map_x, 0, map_width - 1);
    projected_z = std::clamp(projected_z, 0, map_height - 1);
    const auto fixed_x = std::bit_cast<int32_t>(static_cast<uint32_t>(map_x) << 16U);
    auto sample = [&](int32_t fixed_z) {
        return std::max<int32_t>(terrain.sea_level(), terrain.height(fixed_x, fixed_z));
    };
    auto near_z = std::bit_cast<int32_t>(static_cast<uint32_t>((projected_z & ~15) + 128) << 16U);
    // The last row sampled stands when no row projects in front of the point.
    auto fixed_z = near_z;
    int32_t height = terrain.sea_level();
    for (int32_t offset = 128; offset >= 0; offset -= 16, near_z = wrap_sub(near_z, 0x100000)) {
        fixed_z = near_z;
        height = sample(fixed_z);
        const auto near_projected = high(fixed_z) - (height >> 1);
        if (near_projected > projected_z)
            continue;
        const auto far_z = wrap_add(fixed_z, 0x100000);
        const auto far_height = sample(far_z);
        const auto far_projected = high(far_z) - (far_height >> 1);
        if ((near_projected < far_projected && near_projected <= projected_z) ||
            projected_z <= far_projected) {
            const auto denominator = far_projected - near_projected;
            if (denominator == 0)
                break;
            // The multiply, add and shift of the numerator wrap at 32 bits.
            const auto numerator_bits = (static_cast<uint32_t>(near_projected) * 0xfffU +
                                         static_cast<uint32_t>(projected_z))
                                        << 20U;
            const auto numerator = std::bit_cast<int32_t>(numerator_bits);
            fixed_z = wrap_add(fixed_z, numerator / denominator);
            height = sample(fixed_z);
        }
        break;
    }
    return {fixed_x, std::bit_cast<int32_t>(static_cast<uint32_t>(height) << 16U), fixed_z};
}

formats::objects3d::FixedVector3 drag_box_corner(
    const sim::unit_movement::Terrain& terrain,
    int32_t map_x,
    int32_t projected_z,
    int32_t map_width,
    int32_t map_height,
    bool past_edges
) {
    const bool on_map =
        map_x >= 0 && map_x < map_width && projected_z >= 0 && projected_z < map_height;
    if (!past_edges || on_map || map_width <= 0 || map_height <= 0)
        return terrain_intersection(terrain, map_x, projected_z, map_width, map_height);
    return {fixed(map_x), 0, fixed(projected_z)};
}

bool command_binds_cursor_unit(uint8_t command) noexcept {
    return command != unload_command && command != stop_command && command != build_command;
}

} // namespace oa::sim::gameplay_input
