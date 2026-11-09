// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Per-frame unit bounds match triangle and card accumulation in a map,
// forget missing slots, grow within the unit-ID range and survive frame wrap.
#include "unit_bounds.hpp"

#include "oa/test/check.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <unordered_map>

namespace {

using oa::app::UnitBounds;
using Reference = std::unordered_map<uint16_t, oa::Rect32>;
using Point = std::array<float, 2>;

constexpr int32_t layout_left = 11;
constexpr int32_t layout_top = -7;
constexpr uint16_t highest_id = std::numeric_limits<uint16_t>::max();

oa::Rect32 empty_bounds() {
    return {
        std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int32_t>::max(),
        std::numeric_limits<int32_t>::min(),
        std::numeric_limits<int32_t>::min()
    };
}

bool same(const oa::Rect32& a, const oa::Rect32& b) {
    return a.x1 == b.x1 && a.y1 == b.y1 && a.x2 == b.x2 && a.y2 == b.y2;
}

void include_points(oa::Rect32& bounds, std::span<const Point> points) {
    for (const auto& point : points) {
        bounds.x1 = std::min(bounds.x1, static_cast<int32_t>(std::floor(point[0])) + layout_left);
        bounds.y1 = std::min(bounds.y1, static_cast<int32_t>(std::floor(point[1])) + layout_top);
        bounds.x2 = std::max(bounds.x2, static_cast<int32_t>(std::ceil(point[0])) + layout_left);
        bounds.y2 = std::max(bounds.y2, static_cast<int32_t>(std::ceil(point[1])) + layout_top);
    }
}

void add_triangle(
    UnitBounds& actual, Reference& reference, uint16_t id, const std::array<Point, 3>& triangle
) {
    include_points(actual.touch(id), triangle);
    const auto entry = reference.try_emplace(id, empty_bounds()).first;
    include_points(entry->second, triangle);
}

void merge(oa::Rect32& target, const oa::Rect32& bounds) {
    target.x1 = std::min(target.x1, bounds.x1);
    target.y1 = std::min(target.y1, bounds.y1);
    target.x2 = std::max(target.x2, bounds.x2);
    target.y2 = std::max(target.y2, bounds.y2);
}

void add_card(
    UnitBounds& actual,
    Reference& reference,
    uint16_t id,
    float left,
    float top,
    float right,
    float bottom
) {
    const oa::Rect32 bounds{
        static_cast<int32_t>(std::floor(left)) + layout_left,
        static_cast<int32_t>(std::floor(top)) + layout_top,
        static_cast<int32_t>(std::ceil(right)) + layout_left,
        static_cast<int32_t>(std::ceil(bottom)) + layout_top
    };
    merge(actual.touch(id), bounds);
    const auto [entry, inserted] = reference.try_emplace(id, bounds);
    if (!inserted)
        merge(entry->second, bounds);
}

void check_reference(const UnitBounds& actual, const Reference& reference) {
    const auto capacity = actual.storage_slots();
    for (uint32_t id = 0; id < UnitBounds::max_slots; ++id) {
        const auto* found = actual.find(static_cast<uint16_t>(id));
        const auto expected = reference.find(static_cast<uint16_t>(id));
        OA_CHECK((found != nullptr) == (expected != reference.end()));
        if (found != nullptr && expected != reference.end())
            OA_CHECK(same(*found, expected->second));
    }
    OA_CHECK(actual.storage_slots() == capacity);
    OA_CHECK(capacity <= UnitBounds::max_slots);
}

/// A slot starts empty once each frame; touching it again retains its bounds.
void first_touch_initializes_and_repeated_touch_preserves() {
    UnitBounds bounds;
    OA_CHECK(UnitBounds::max_slots == size_t{highest_id} + 1);
    const auto capacity = bounds.storage_slots();
    OA_CHECK(bounds.find(0) == nullptr);
    OA_CHECK(bounds.find(17) == nullptr);
    OA_CHECK(bounds.find(highest_id) == nullptr);
    OA_CHECK(bounds.storage_slots() == capacity);
    auto& first = bounds.touch(17);
    OA_CHECK(same(first, empty_bounds()));
    first = {-13, -19, 42, 81};
    OA_CHECK(&bounds.touch(17) == &first);
    OA_CHECK(same(bounds.touch(17), {-13, -19, 42, 81}));
    OA_CHECK(bounds.find(17) != nullptr);
    bounds.begin_frame();
    OA_CHECK(bounds.find(17) == nullptr);
    OA_CHECK(same(bounds.touch(17), empty_bounds()));
}

/// Repeated triangles and distinct ranges for one ID keep the same floor/ceil
/// extents as map accumulation, including negative fractional coordinates.
void triangle_ranges_match_map_accumulation() {
    UnitBounds bounds;
    Reference reference;
    constexpr std::array<Point, 3> first{{{-0.25F, 2.75F}, {3.0F, -4.125F}, {1.5F, 1.25F}}};
    constexpr std::array<Point, 3> second{{{-1.75F, 9.125F}, {8.25F, -2.0F}, {3.5F, 4.5F}}};
    add_triangle(bounds, reference, 17, first);
    OA_CHECK(same(bounds.touch(17), {10, -12, 14, -4}));
    add_triangle(bounds, reference, 17, first);
    OA_CHECK(same(bounds.touch(17), {10, -12, 14, -4}));
    add_triangle(bounds, reference, 17, second);
    OA_CHECK(same(bounds.touch(17), {9, -12, 20, 3}));
    add_triangle(bounds, reference, 99, second);
    check_reference(bounds, reference);
}

/// Cards extend triangle bounds and merge with other cards of the same ID;
/// a card-only ID begins with its own rectangle.
void triangle_and_card_bounds_match_map_accumulation() {
    UnitBounds bounds;
    Reference reference;
    constexpr std::array<Point, 3> triangle{{{-0.25F, 2.75F}, {3.0F, -4.125F}, {1.5F, 1.25F}}};
    add_triangle(bounds, reference, 17, triangle);
    add_card(bounds, reference, 17, -10.25F, -12.5F, 10.125F, 8.875F);
    OA_CHECK(same(bounds.touch(17), {0, -20, 22, 2}));
    add_card(bounds, reference, 200, 1.1F, 2.2F, 4.4F, 8.8F);
    OA_CHECK(same(bounds.touch(200), {12, -5, 16, 2}));
    add_card(bounds, reference, 200, -4.01F, -1.01F, 2.0F, 10.01F);
    add_card(bounds, reference, 200, 1.1F, 2.2F, 4.4F, 8.8F);
    OA_CHECK(same(bounds.touch(200), {6, -9, 16, 4}));
    check_reference(bounds, reference);
}

/// Empty frames forget previous IDs, and a reused slot starts with its new
/// bounds. Growing through high IDs never retains more than the ID range.
void frames_slot_reuse_and_high_ids_stay_bounded() {
    UnitBounds bounds;
    Reference reference;
    add_card(bounds, reference, 7, -100.5F, -200.5F, 300.5F, 400.5F);
    add_card(bounds, reference, 11, 0.0F, 0.0F, 1.0F, 1.0F);
    check_reference(bounds, reference);
    bounds.begin_frame();
    reference.clear();
    check_reference(bounds, reference);
    add_card(bounds, reference, 7, 1.0F, 2.0F, 3.0F, 4.0F);
    OA_CHECK(same(bounds.touch(7), {12, -5, 14, -3}));
    OA_CHECK(bounds.find(11) == nullptr);
    for (const uint16_t id : std::array<uint16_t, 5>{0, 16384, 32768, 65534, highest_id}) {
        add_card(bounds, reference, id, -0.5F, -1.5F, 2.5F, 3.5F);
        OA_CHECK(bounds.storage_slots() <= UnitBounds::max_slots);
    }
    add_card(bounds, reference, highest_id, -2.5F, -3.5F, 4.5F, 5.5F);
    check_reference(bounds, reference);
    const auto capacity = bounds.storage_slots();
    bounds.begin_frame();
    reference.clear();
    bounds.begin_frame();
    check_reference(bounds, reference);
    OA_CHECK(bounds.storage_slots() == capacity);
}

/// A complete natural frame-stamp cycle must not revive a slot from the
/// original frame or the frame immediately before the wrap.
void frame_stamp_wrap_keeps_old_slots_missing() {
    UnitBounds bounds;
    bounds.touch(11) = {-19, -31, 57, 81};
    bounds.touch(highest_id) = {-7, -5, 3, 9};
    constexpr uint32_t frame_cycle = std::numeric_limits<uint16_t>::max();
    for (uint32_t frame = 1; frame < frame_cycle; ++frame)
        bounds.begin_frame();
    OA_CHECK(bounds.find(11) == nullptr);
    OA_CHECK(bounds.find(highest_id) == nullptr);
    bounds.touch(12) = {1, 2, 3, 4};
    bounds.begin_frame();
    OA_CHECK(bounds.find(11) == nullptr);
    OA_CHECK(bounds.find(12) == nullptr);
    OA_CHECK(bounds.find(highest_id) == nullptr);
    OA_CHECK(same(bounds.touch(11), empty_bounds()));
    bounds.touch(11) = {5, 7, 9, 11};
    const auto* found = bounds.find(11);
    OA_CHECK(found != nullptr);
    if (found != nullptr)
        OA_CHECK(same(*found, {5, 7, 9, 11}));
    OA_CHECK(bounds.storage_slots() <= UnitBounds::max_slots);
}

} // namespace

int main() {
    first_touch_initializes_and_repeated_touch_preserves();
    triangle_ranges_match_map_accumulation();
    triangle_and_card_bounds_match_map_accumulation();
    frames_slot_reuse_and_high_ids_stay_bounded();
    frame_stamp_wrap_keeps_old_slots_missing();
    return oa::test::check_exit_status();
}
