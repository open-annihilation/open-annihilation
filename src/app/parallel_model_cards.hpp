// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "renderer_live_models.hpp"

#include <algorithm>
#include <limits>
#include <span>

namespace oa::app::renderer_live_scene {
namespace parallel_model_cards_detail {

inline constexpr uint32_t first_shadow_depth = 65534;
inline constexpr uint32_t shadow_depth_divisor = 65535;
inline constexpr uint32_t maximum_shadow_depths = 65000;
inline constexpr uint32_t corner_mask = (uint32_t{1} << renderer_live_gpu::model_corner_bits) - 1;

/// Returns one shadow's depth, using the ordinary cast's expression order.
[[nodiscard]] inline float shadow_depth(uint32_t index) noexcept {
    return static_cast<float>(first_shadow_depth - index) / float{shadow_depth_divisor};
}

/// Tests a range without overflowing either end.
[[nodiscard]] inline bool within(std::size_t first, std::size_t count, std::size_t size) noexcept {
    return first <= size && count <= size - first;
}

/// Checks the records a model run's relative indices name.
[[nodiscard]] inline bool runs_valid(const ModelCards& cards, bool shadow) noexcept {
    const auto& runs = shadow ? cards.cast_runs : cards.runs;
    const auto& indices = shadow ? cards.cast_indices : cards.indices;
    const auto instances = shadow ? cards.casts.size() : cards.instances.size();
    for (const auto& run : runs) {
        if (!within(run.first_index, run.index_count, indices.size()) || run.index_count % 3 != 0 ||
            run.first_instance > instances)
            return false;
        for (std::size_t at = run.first_index; at < std::size_t{run.first_index} + run.index_count;
             ++at) {
            const auto encoded = indices[at];
            const auto relative = encoded >> renderer_live_gpu::model_corner_bits;
            if (relative >= renderer_live_gpu::model_run_instances ||
                relative >= instances - run.first_instance)
                return false;
            const auto index = std::size_t{run.first_instance} + relative;
            const auto instance = shadow ? cards.casts[index].instance : index;
            if (instance >= cards.instances.size() ||
                (encoded & corner_mask) >= cards.instances[instance].corner_count)
                return false;
        }
    }
    return true;
}

/// Checks a pass's owned ranges and its ordinary consecutive shadow depths.
[[nodiscard]] inline bool layout_valid(const ModelCards& cards) noexcept {
    namespace gpu = renderer_live_gpu;
    if (cards.positions.size() % 3 != 0 || cards.textures.size() % 4 != 0 ||
        cards.slot_states.size() != cards.textures.size() / 4 ||
        cards.instances.size() > gpu::largest_model_instance_count ||
        cards.positions.size() / 3 > gpu::largest_vertex_count ||
        cards.indices.size() > gpu::largest_vertex_count ||
        cards.cast_indices.size() > gpu::largest_vertex_count ||
        cards.casts.size() > gpu::largest_model_instance_count * 2 ||
        cards.cast_depths > maximum_shadow_depths)
        return false;
    for (const auto& instance : cards.instances)
        if (!within(instance.positions, instance.position_count, cards.positions.size() / 3) ||
            !within(instance.pieces, instance.piece_count, cards.piece_frames.size()) ||
            !within(instance.textures, instance.texture_count, cards.slot_states.size()) ||
            instance.corner_count > corner_mask + 1 || instance.position_count > corner_mask + 1 ||
            instance.piece_count > (uint32_t{1} << gpu::model_piece_shift) ||
            instance.texture_count > (uint32_t{1} << gpu::model_texture_bits))
            return false;
    for (const auto& table : cards.tables)
        if (table.first > cards.slot_states.size())
            return false;
    for (const auto& unit : cards.units)
        if (!within(unit.first_index, unit.index_count, cards.indices.size()) ||
            unit.index_count % 3 != 0 ||
            (unit.marker ? unit.instance >= cards.markers.size()
                         : unit.instance >= cards.instances.size()))
            return false;
    uint32_t depths{};
    const renderer_live_gpu::ShadowCast* previous = nullptr;
    for (const auto& cast : cards.casts) {
        if (cast.instance >= cards.instances.size() || cast.kind > gpu::shadow_cast_cut)
            return false;
        if (previous != nullptr && cast.depth == previous->depth) {
            if (previous->kind != gpu::shadow_cast_cut || cast.kind != gpu::shadow_cast_sheared ||
                previous->instance != cast.instance)
                return false;
        } else {
            if (depths >= cards.cast_depths || cast.depth != shadow_depth(depths))
                return false;
            ++depths;
        }
        previous = &cast;
    }
    return depths == cards.cast_depths && runs_valid(cards, false) && runs_valid(cards, true);
}

/// Tests an addition against the limit before performing it.
[[nodiscard]] inline bool fits(std::size_t first, std::size_t count, std::size_t limit) noexcept {
    return first <= limit && count <= limit - first;
}

} // namespace parallel_model_cards_detail

/// Tests whether an independent, marker-free model band can be joined.
///
/// Both lists must have their ordinary local shadow depths, before a scene
/// supplies the shadow map. A destination may already contain serial markers;
/// their shared fan and all its ranges stay where they were. Source lookup
/// caches are temporary and are not needed by the joined drawing or picking.
///
/// @param into the records already listed in drawing order
/// @param from the following independent band
/// @param vertex_base its first ordinary vertex in the destination pass
/// @return whether all layout checks and offset bounds pass, without mutation
[[nodiscard]] inline bool can_append_parallel_model_cards(
    const ModelCards& into, const ModelCards& from, std::size_t vertex_base
) noexcept {
    namespace detail = parallel_model_cards_detail;
    namespace gpu = renderer_live_gpu;
    constexpr auto word_limit = std::numeric_limits<uint32_t>::max();
    constexpr auto size_limit = std::numeric_limits<std::size_t>::max();
    if (&into == &from || !from.markers.empty() || !from.marker_runs.empty() ||
        std::any_of(
            from.units.begin(), from.units.end(), [](const auto& unit) { return unit.marker; }
        ) ||
        !detail::layout_valid(into) || !detail::layout_valid(from) ||
        !detail::fits(
            into.instances.size(), from.instances.size(), gpu::largest_model_instance_count
        ) ||
        !detail::fits(
            into.positions.size() / 3, from.positions.size() / 3, gpu::largest_vertex_count
        ) ||
        !detail::fits(into.indices.size(), from.indices.size(), gpu::largest_vertex_count) ||
        !detail::fits(
            into.cast_indices.size(), from.cast_indices.size(), gpu::largest_vertex_count
        ) ||
        !detail::fits(
            into.casts.size(), from.casts.size(), gpu::largest_model_instance_count * 2
        ) ||
        !detail::fits(into.cast_depths, from.cast_depths, detail::maximum_shadow_depths) ||
        !detail::fits(into.piece_frames.size(), from.piece_frames.size(), word_limit) ||
        !detail::fits(into.textures.size(), from.textures.size(), word_limit) ||
        !detail::fits(into.vertices, from.vertices, gpu::largest_vertex_count) ||
        !detail::fits(into.units.size(), from.units.size(), size_limit) ||
        !detail::fits(into.tables.size(), from.tables.size(), size_limit) ||
        !detail::fits(into.runs.size(), from.runs.size(), size_limit) ||
        !detail::fits(into.cast_runs.size(), from.cast_runs.size(), size_limit))
        return false;
    for (const auto& run : from.runs)
        if (!detail::fits(vertex_base, run.vertex, size_limit))
            return false;
    for (const auto& run : from.cast_runs)
        if (!detail::fits(vertex_base, run.vertex, size_limit))
            return false;
    for (const auto& unit : from.units)
        if (!detail::fits(vertex_base, unit.vertex, size_limit))
            return false;
    return true;
}

/// Joins one marker-free model band while preserving triangle and pick order.
///
/// Relative indices stay relative to their own runs. Every absolute range is
/// rebased, and shadow depths are formed again from their global integer
/// sequence, preserving a building's shared picture/cut depth exactly. The
/// caller keeps the same floating-point control as the drawing workers.
/// Structural rejection changes neither list. Allocation failure may throw
/// while reserving; sizes and records are still unchanged at that point.
///
/// @param[in,out] into the earlier records and destination
/// @param from the following independent band, retained unchanged
/// @param vertex_base its first ordinary vertex in the destination pass
/// @return false for an unsafe join; true after appending every record
[[nodiscard]] inline bool
append_parallel_model_cards(ModelCards& into, const ModelCards& from, std::size_t vertex_base) {
    if (!can_append_parallel_model_cards(into, from, vertex_base))
        return false;
    const auto instances = static_cast<uint32_t>(into.instances.size());
    const auto positions = static_cast<uint32_t>(into.positions.size() / 3);
    const auto pieces = static_cast<uint32_t>(into.piece_frames.size());
    const auto textures = static_cast<uint32_t>(into.slot_states.size());
    const auto indices = static_cast<uint32_t>(into.indices.size());
    const auto casts = static_cast<uint32_t>(into.casts.size());
    const auto cast_indices = static_cast<uint32_t>(into.cast_indices.size());
    const auto depths = into.cast_depths;
    // Every allocation precedes the first record change.
    into.instances.reserve(into.instances.size() + from.instances.size());
    into.positions.reserve(into.positions.size() + from.positions.size());
    into.piece_frames.reserve(into.piece_frames.size() + from.piece_frames.size());
    into.textures.reserve(into.textures.size() + from.textures.size());
    into.slot_states.reserve(into.slot_states.size() + from.slot_states.size());
    into.tables.reserve(into.tables.size() + from.tables.size());
    into.indices.reserve(into.indices.size() + from.indices.size());
    into.runs.reserve(into.runs.size() + from.runs.size());
    into.units.reserve(into.units.size() + from.units.size());
    into.casts.reserve(into.casts.size() + from.casts.size());
    into.cast_indices.reserve(into.cast_indices.size() + from.cast_indices.size());
    into.cast_runs.reserve(into.cast_runs.size() + from.cast_runs.size());
    into.positions.insert(into.positions.end(), from.positions.begin(), from.positions.end());
    into.piece_frames.insert(
        into.piece_frames.end(), from.piece_frames.begin(), from.piece_frames.end()
    );
    into.textures.insert(into.textures.end(), from.textures.begin(), from.textures.end());
    into.slot_states.insert(
        into.slot_states.end(), from.slot_states.begin(), from.slot_states.end()
    );
    into.indices.insert(into.indices.end(), from.indices.begin(), from.indices.end());
    into.cast_indices.insert(
        into.cast_indices.end(), from.cast_indices.begin(), from.cast_indices.end()
    );
    for (auto instance : from.instances) {
        instance.positions += positions;
        instance.pieces += pieces;
        instance.textures += textures;
        into.instances.push_back(instance);
    }
    for (auto table : from.tables) {
        table.first += textures;
        into.tables.push_back(table);
    }
    for (auto run : from.runs) {
        run.vertex += vertex_base;
        run.first_index += indices;
        run.first_instance += instances;
        into.runs.push_back(run);
    }
    for (auto unit : from.units) {
        unit.vertex += vertex_base;
        unit.instance += instances;
        unit.first_index += indices;
        into.units.push_back(unit);
    }
    uint32_t depth{};
    for (std::size_t index = 0; index < from.casts.size(); ++index) {
        auto cast = from.casts[index];
        if (index != 0 && cast.depth != from.casts[index - 1].depth)
            ++depth;
        cast.instance += instances;
        cast.depth = parallel_model_cards_detail::shadow_depth(depths + depth);
        into.casts.push_back(cast);
    }
    for (auto run : from.cast_runs) {
        run.vertex += vertex_base;
        run.first_index += cast_indices;
        run.first_instance += casts;
        into.cast_runs.push_back(run);
    }
    into.vertices += from.vertices;
    into.cast_depths += from.cast_depths;
    into.draw_instance = -1;
    return true;
}

namespace parallel_model_cards_detail {

/// Counts the records retained after the complete ordered batch.
struct BatchSize {
    std::size_t instances{}, positions{}, pieces{}, textures{}, tables{}, indices{}, runs{};
    std::size_t units{}, casts{}, cast_indices{}, cast_runs{}, vertices{}, depths{};
};

/// Reads the destination sizes before considering any following bands.
[[nodiscard]] inline BatchSize batch_size(const ModelCards& cards) noexcept {
    return {
        cards.instances.size(),
        cards.positions.size() / 3,
        cards.piece_frames.size(),
        cards.textures.size(),
        cards.tables.size(),
        cards.indices.size(),
        cards.runs.size(),
        cards.units.size(),
        cards.casts.size(),
        cards.cast_indices.size(),
        cards.cast_runs.size(),
        cards.vertices,
        cards.cast_depths
    };
}

/// Checks cumulative ranges and advances only the private scalar totals.
[[nodiscard]] inline bool
include_band(BatchSize& size, const ModelCards& from, std::size_t vertex_base) noexcept {
    namespace gpu = renderer_live_gpu;
    constexpr auto word_limit = std::numeric_limits<uint32_t>::max();
    constexpr auto size_limit = std::numeric_limits<std::size_t>::max();
    if (!fits(size.instances, from.instances.size(), gpu::largest_model_instance_count) ||
        !fits(size.positions, from.positions.size() / 3, gpu::largest_vertex_count) ||
        !fits(size.indices, from.indices.size(), gpu::largest_vertex_count) ||
        !fits(size.cast_indices, from.cast_indices.size(), gpu::largest_vertex_count) ||
        !fits(size.casts, from.casts.size(), gpu::largest_model_instance_count * 2) ||
        !fits(size.depths, from.cast_depths, maximum_shadow_depths) ||
        !fits(size.pieces, from.piece_frames.size(), word_limit) ||
        !fits(size.textures, from.textures.size(), word_limit) ||
        !fits(size.vertices, from.vertices, gpu::largest_vertex_count) ||
        !fits(size.units, from.units.size(), size_limit) ||
        !fits(size.tables, from.tables.size(), size_limit) ||
        !fits(size.runs, from.runs.size(), size_limit) ||
        !fits(size.cast_runs, from.cast_runs.size(), size_limit))
        return false;
    for (const auto& run : from.runs)
        if (!fits(vertex_base, run.vertex, size_limit))
            return false;
    for (const auto& run : from.cast_runs)
        if (!fits(vertex_base, run.vertex, size_limit))
            return false;
    for (const auto& unit : from.units)
        if (!fits(vertex_base, unit.vertex, size_limit))
            return false;
    size.instances += from.instances.size();
    size.positions += from.positions.size() / 3;
    size.pieces += from.piece_frames.size();
    size.textures += from.textures.size();
    size.tables += from.tables.size();
    size.indices += from.indices.size();
    size.runs += from.runs.size();
    size.units += from.units.size();
    size.casts += from.casts.size();
    size.cast_indices += from.cast_indices.size();
    size.cast_runs += from.cast_runs.size();
    size.vertices += from.vertices;
    size.depths += from.cast_depths;
    return true;
}

/// Appends already checked records to capacities reserved for the entire batch.
inline void
append_prevalidated_band(ModelCards& into, const ModelCards& from, std::size_t vertex_base) {
    const auto instances = static_cast<uint32_t>(into.instances.size());
    const auto positions = static_cast<uint32_t>(into.positions.size() / 3);
    const auto pieces = static_cast<uint32_t>(into.piece_frames.size());
    const auto textures = static_cast<uint32_t>(into.slot_states.size());
    const auto indices = static_cast<uint32_t>(into.indices.size());
    const auto casts = static_cast<uint32_t>(into.casts.size());
    const auto cast_indices = static_cast<uint32_t>(into.cast_indices.size());
    const auto depths = into.cast_depths;
    into.positions.insert(into.positions.end(), from.positions.begin(), from.positions.end());
    into.piece_frames.insert(
        into.piece_frames.end(), from.piece_frames.begin(), from.piece_frames.end()
    );
    into.textures.insert(into.textures.end(), from.textures.begin(), from.textures.end());
    into.slot_states.insert(
        into.slot_states.end(), from.slot_states.begin(), from.slot_states.end()
    );
    into.indices.insert(into.indices.end(), from.indices.begin(), from.indices.end());
    into.cast_indices.insert(
        into.cast_indices.end(), from.cast_indices.begin(), from.cast_indices.end()
    );
    for (auto instance : from.instances) {
        instance.positions += positions;
        instance.pieces += pieces;
        instance.textures += textures;
        into.instances.push_back(instance);
    }
    for (auto table : from.tables) {
        table.first += textures;
        into.tables.push_back(table);
    }
    for (auto run : from.runs) {
        run.vertex += vertex_base;
        run.first_index += indices;
        run.first_instance += instances;
        into.runs.push_back(run);
    }
    for (auto unit : from.units) {
        unit.vertex += vertex_base;
        unit.instance += instances;
        unit.first_index += indices;
        into.units.push_back(unit);
    }
    uint32_t depth{};
    for (std::size_t index = 0; index < from.casts.size(); ++index) {
        auto cast = from.casts[index];
        if (index != 0 && cast.depth != from.casts[index - 1].depth)
            ++depth;
        cast.instance += instances;
        cast.depth = shadow_depth(depths + depth);
        into.casts.push_back(cast);
    }
    for (auto run : from.cast_runs) {
        run.vertex += vertex_base;
        run.first_index += cast_indices;
        run.first_instance += casts;
        into.cast_runs.push_back(run);
    }
    into.vertices += from.vertices;
    into.cast_depths += from.cast_depths;
    into.draw_instance = -1;
}

} // namespace parallel_model_cards_detail

/// Joins up to four immutable model bands after validating every layout once.
///
/// The destination and each following band receive the same structural checks
/// as the single-band join. All cumulative offsets and limits are proved before
/// mutation; required capacities are reserved once for the complete batch.
/// Allocation failure may change capacities but leaves sizes and records intact.
/// Source aliases of the destination and null pointers are refused; a source
/// may occur more than once. An empty batch succeeds without changing anything.
///
/// @param[in,out] into the earlier records and destination
/// @param bands the independent marker-free bands, in exact drawing order
/// @param vertex_base their common first ordinary vertex in the destination pass
/// @return false without record changes for an unsafe batch; true after every band
[[nodiscard]] inline bool append_parallel_model_card_bands(
    ModelCards& into, std::span<const ModelCards* const> bands, std::size_t vertex_base
) {
    namespace detail = parallel_model_cards_detail;
    constexpr std::size_t maximum_bands = 4;
    if (bands.empty())
        return true;
    if (bands.size() > maximum_bands || !detail::layout_valid(into))
        return false;
    auto size = detail::batch_size(into);
    for (const auto* from : bands) {
        if (from == nullptr || from == &into || !from->markers.empty() ||
            !from->marker_runs.empty() ||
            std::any_of(
                from->units.begin(), from->units.end(), [](const auto& unit) { return unit.marker; }
            ) ||
            !detail::layout_valid(*from) || !detail::include_band(size, *from, vertex_base))
            return false;
    }
    // Every possible allocation precedes the first record change.
    into.instances.reserve(size.instances);
    into.positions.reserve(size.positions * 3);
    into.piece_frames.reserve(size.pieces);
    into.textures.reserve(size.textures);
    into.slot_states.reserve(size.textures / 4);
    into.tables.reserve(size.tables);
    into.indices.reserve(size.indices);
    into.runs.reserve(size.runs);
    into.units.reserve(size.units);
    into.casts.reserve(size.casts);
    into.cast_indices.reserve(size.cast_indices);
    into.cast_runs.reserve(size.cast_runs);
    for (const auto* from : bands)
        detail::append_prevalidated_band(into, *from, vertex_base);
    return true;
}

} // namespace oa::app::renderer_live_scene
