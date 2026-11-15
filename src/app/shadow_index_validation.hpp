// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "renderer_live_gpu.hpp"

#include <array>
#include <cstddef>
#include <cstdint>

namespace oa::app::renderer_live_gpu::shadow_index_detail {

inline constexpr uint32_t local_corner_mask = (uint32_t{1} << model_corner_bits) - 1;

/// Tests a half-open range without adding either untrusted end.
[[nodiscard]] inline bool within(std::size_t first, std::size_t count, std::size_t size) noexcept {
    return first <= size && count <= size - first;
}

/// Checks one immutable local sequence before any fallback or admission reads it.
[[nodiscard]] inline bool
valid_pattern_base(const ModelDraws& draws, const ShadowIndexPattern& pattern) noexcept {
    if (pattern.indices_per_cast == 0 || pattern.indices_per_cast % 3 != 0 ||
        pattern.indices_per_cast >
            shadow_template_byte_limit / sizeof(uint32_t) / model_run_instances ||
        pattern.type_corner_count == 0 || pattern.type_corner_count > local_corner_mask + 1 ||
        pattern.selected_mask == 0 ||
        !within(pattern.type_corners, pattern.type_corner_count, draws.corners.size() / 2) ||
        !within(
            pattern.first_index,
            std::size_t{pattern.indices_per_cast} * model_run_instances,
            draws.shadow_pattern_indices.size()
        ))
        return false;
    for (const auto corner :
         draws.shadow_pattern_indices.subspan(pattern.first_index, pattern.indices_per_cast)) {
        if (corner >= pattern.type_corner_count)
            return false;
        const auto piece =
            draws.corners[2 * (std::size_t{pattern.type_corners} + corner)] >> model_piece_shift;
        if (piece >= 64 || (pattern.selected_mask & (uint64_t{1} << piece)) == 0)
            return false;
    }
    return true;
}

/// Checks all 256 repetitions before an immutable template is admitted to the GPU.
[[nodiscard]] inline bool
valid_pattern_repetitions(const ModelDraws& draws, const ShadowIndexPattern& pattern) noexcept {
    if (!valid_pattern_base(draws, pattern))
        return false;
    for (uint32_t cast = 1; cast < model_run_instances; ++cast)
        for (uint32_t at = 0; at < pattern.indices_per_cast; ++at) {
            const auto base = draws.shadow_pattern_indices[pattern.first_index + at];
            const auto expanded =
                pattern.first_index + std::size_t{cast} * pattern.indices_per_cast + at;
            if (draws.shadow_pattern_indices[expanded] != (cast << model_corner_bits | base))
                return false;
        }
    return true;
}

/// Checks compact ranges and every current cast binding, independently of residency.
/// Unadmitted templates use only their checked local sequence; their expanded
/// repetitions are checked separately before a persistent buffer may read them.
[[nodiscard]] inline bool valid_compact_draws(const ModelDraws& draws) noexcept {
    if (!draws.cast_indices.empty() || !draws.cast_runs.empty() ||
        !draws.shadow_index_casts.empty() ||
        draws.logical_shadow_index_count > largest_vertex_count ||
        draws.shadow_dynamic_indices.size() > largest_vertex_count ||
        draws.shadow_patterns.size() > shadow_template_pattern_limit ||
        draws.shadow_pattern_indices.size() > shadow_template_byte_limit / sizeof(uint32_t) ||
        draws.shadow_pattern_indices.size() % 3 != 0 ||
        (!draws.shadow_patterns.empty() &&
         (draws.shadow_patterns_revision == 0 ||
          draws.shadow_patterns_type_revision != draws.corners_revision)))
        return false;
    std::size_t pattern_end{};
    for (const auto& pattern : draws.shadow_patterns) {
        if (pattern.first_index != pattern_end || !valid_pattern_base(draws, pattern))
            return false;
        pattern_end += std::size_t{pattern.indices_per_cast} * model_run_instances;
    }
    if (pattern_end != draws.shadow_pattern_indices.size())
        return false;
    std::size_t cast_end{}, dynamic_end{}, logical{};
    for (const auto& run : draws.shadow_index_runs) {
        if (run.first_cast != cast_end || run.cast_count == 0 ||
            run.cast_count > model_run_instances ||
            !within(run.first_cast, run.cast_count, draws.casts.size()) ||
            run.index_count % 3 != 0 || run.index_count > largest_vertex_count - logical)
            return false;
        cast_end += run.cast_count;
        logical += run.index_count;
        if (run.pattern == 0) {
            if (run.first_index != dynamic_end ||
                !within(run.first_index, run.index_count, draws.shadow_dynamic_indices.size()))
                return false;
            for (const auto index :
                 draws.shadow_dynamic_indices.subspan(run.first_index, run.index_count)) {
                const auto relative = index >> model_corner_bits;
                if (relative >= run.cast_count)
                    return false;
                const auto instance = draws.casts[run.first_cast + relative].instance;
                if (instance >= draws.instances.size() ||
                    (index & local_corner_mask) >= draws.instances[instance].corner_count)
                    return false;
            }
            dynamic_end += run.index_count;
        } else {
            if (run.pattern > draws.shadow_patterns.size() || run.first_index != 0 ||
                run.cast_count < shadow_template_minimum_casts)
                return false;
            const auto& pattern = draws.shadow_patterns[run.pattern - 1];
            if (run.index_count != std::size_t{pattern.indices_per_cast} * run.cast_count)
                return false;
            for (uint32_t cast = run.first_cast; cast < run.first_cast + run.cast_count; ++cast) {
                const auto& shadow = draws.casts[cast];
                if (shadow.kind != shadow_cast_picture || shadow.instance >= draws.instances.size())
                    return false;
                const auto& instance = draws.instances[shadow.instance];
                if (instance.corners != pattern.type_corners ||
                    instance.corner_count != pattern.type_corner_count ||
                    instance.piece_count > 64 ||
                    (instance.piece_count < 64 &&
                     pattern.selected_mask >> instance.piece_count != 0))
                    return false;
            }
        }
    }
    return cast_end == draws.casts.size() && dynamic_end == draws.shadow_dynamic_indices.size() &&
           logical == draws.logical_shadow_index_count;
}

/// Checks local sources and each cast binding before a procedural draw or fallback.
///
/// Dynamic words contain only local corners. A retained source always supplies
/// its complete one-cast sequence; run boundaries retain the original order.
///
/// @param draws the frame's cast sources and immutable pattern snapshot
/// @return true when every source and complete run is within its current binding
[[nodiscard]] inline bool valid_procedural_draws(const ModelDraws& draws) noexcept {
    if (!draws.cast_indices.empty() || !draws.cast_runs.empty() ||
        draws.shadow_index_casts.size() != draws.casts.size() ||
        draws.logical_shadow_index_count > largest_vertex_count ||
        draws.shadow_dynamic_indices.size() > largest_vertex_count ||
        draws.shadow_patterns.size() > shadow_template_pattern_limit ||
        draws.shadow_pattern_indices.size() > shadow_template_byte_limit / sizeof(uint32_t) ||
        draws.shadow_pattern_indices.size() % 3 != 0 ||
        (!draws.shadow_patterns.empty() &&
         (draws.shadow_patterns_revision == 0 ||
          draws.shadow_patterns_type_revision != draws.corners_revision)))
        return false;
    std::size_t pattern_end{};
    for (const auto& pattern : draws.shadow_patterns) {
        if (pattern.first_index != pattern_end || !valid_pattern_base(draws, pattern))
            return false;
        pattern_end += std::size_t{pattern.indices_per_cast} * model_run_instances;
    }
    if (pattern_end != draws.shadow_pattern_indices.size())
        return false;
    std::size_t cast_end{}, dynamic_end{}, logical{};
    for (const auto& run : draws.shadow_index_runs) {
        if (run.first_cast != cast_end || run.cast_count == 0 ||
            run.cast_count > model_run_instances || run.first_index != 0 || run.pattern != 0 ||
            !within(run.first_cast, run.cast_count, draws.casts.size()) ||
            run.index_count % 3 != 0 || run.index_count > largest_vertex_count - logical)
            return false;
        std::size_t run_count{};
        for (uint32_t cast = run.first_cast; cast < run.first_cast + run.cast_count; ++cast) {
            const auto& source = draws.shadow_index_casts[cast];
            const auto& shadow = draws.casts[cast];
            if (shadow.instance >= draws.instances.size() || source.index_count % 3 != 0 ||
                source.index_count > run.index_count - run_count)
                return false;
            const auto& instance = draws.instances[shadow.instance];
            if (instance.corner_count > local_corner_mask + 1)
                return false;
            if (source.pattern == 0) {
                if (source.first_index != dynamic_end ||
                    !within(
                        source.first_index, source.index_count, draws.shadow_dynamic_indices.size()
                    ))
                    return false;
                for (const auto corner :
                     draws.shadow_dynamic_indices.subspan(source.first_index, source.index_count))
                    if (corner > local_corner_mask || corner >= instance.corner_count)
                        return false;
                dynamic_end += source.index_count;
            } else {
                if (source.pattern > draws.shadow_patterns.size() || source.first_index != 0 ||
                    shadow.kind != shadow_cast_picture)
                    return false;
                const auto& pattern = draws.shadow_patterns[source.pattern - 1];
                if (source.index_count != pattern.indices_per_cast ||
                    instance.corners != pattern.type_corners ||
                    instance.corner_count != pattern.type_corner_count ||
                    instance.piece_count > 64 ||
                    (instance.piece_count < 64 &&
                     pattern.selected_mask >> instance.piece_count != 0))
                    return false;
            }
            run_count += source.index_count;
        }
        if (run_count != run.index_count)
            return false;
        cast_end += run.cast_count;
        logical += run.index_count;
    }
    return cast_end == draws.casts.size() && dynamic_end == draws.shadow_dynamic_indices.size() &&
           logical == draws.logical_shadow_index_count;
}

/// Counts mixed runs from their actual retained and dynamic corner sources.
///
/// The independently checked ranges are required. A hit describes the requested
/// patterns before this frame; the sixteen-cast threshold may still select an
/// indexed fallback without changing that residency observation.
///
/// @param draws the accepted procedural cast sources
/// @param resident the patterns resident before the current frame
/// @param admitted the patterns available from this frame's successful upload
/// @param procedural_runs the final procedural selection for each original run
/// @return logical corner counts and requests for the chosen complete runs
[[nodiscard]] inline oa::app::ShadowIndexCounts procedural_draw_counts(
    const ModelDraws& draws,
    const std::array<bool, shadow_template_pattern_limit>& resident,
    const std::array<bool, shadow_template_pattern_limit>& admitted,
    std::span<const uint8_t> procedural_runs
) noexcept {
    oa::app::ShadowIndexCounts counts;
    if (procedural_runs.size() != draws.shadow_index_runs.size())
        return counts;
    counts.logical_indices = draws.logical_shadow_index_count;
    for (std::size_t at = 0; at < draws.shadow_index_runs.size(); ++at) {
        const auto& run = draws.shadow_index_runs[at];
        bool requested{}, all_resident = true;
        uint64_t cached{};
        for (uint32_t cast = run.first_cast; cast < run.first_cast + run.cast_count; ++cast) {
            const auto& source = draws.shadow_index_casts[cast];
            if (source.pattern == 0)
                continue;
            requested = true;
            const auto pattern = source.pattern - 1;
            all_resident &= resident[pattern];
            if (procedural_runs[at] != 0 && (resident[pattern] || admitted[pattern]))
                cached += source.index_count;
        }
        counts.cached_indices += cached;
        counts.dynamic_indices += run.index_count - cached;
        if (requested) {
            ++counts.requested_runs;
            counts.cache_hits += all_resident;
            counts.cache_misses += !all_resident;
            counts.cached_runs += cached != 0;
            counts.fallback_runs += cached == 0;
        }
    }
    return counts;
}

/// Counts the chosen final draw path after valid_compact_draws accepted its ranges.
/// A fallback after an earlier resident request is counted from its final choice.
[[nodiscard]] inline oa::app::ShadowIndexCounts draw_counts(
    const ModelDraws& draws,
    const std::array<bool, shadow_template_pattern_limit>& resident,
    const std::array<bool, shadow_template_pattern_limit>& admitted,
    bool templates_available
) noexcept {
    oa::app::ShadowIndexCounts counts;
    counts.logical_indices = draws.logical_shadow_index_count;
    for (const auto& run : draws.shadow_index_runs) {
        const bool requested = run.pattern != 0;
        const bool cached = requested && templates_available &&
                            (resident[run.pattern - 1] || admitted[run.pattern - 1]);
        if (requested) {
            ++counts.requested_runs;
            if (cached && resident[run.pattern - 1])
                ++counts.cache_hits;
            else
                ++counts.cache_misses;
        }
        if (cached) {
            ++counts.cached_runs;
            counts.cached_indices += run.index_count;
        } else {
            counts.dynamic_indices += run.index_count;
            counts.fallback_runs += requested;
        }
    }
    return counts;
}

} // namespace oa::app::renderer_live_gpu::shadow_index_detail
