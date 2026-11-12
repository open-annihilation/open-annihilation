// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "presentation_capture.hpp"

#include "oa/base/float_precision.hpp"
#include "oa/data/enhanced_mode.hpp"

#include <algorithm>
#include <cstddef>
#include <new>

namespace oa::app {
namespace {

/// Fewest units assigned to a drawing band before capture uses workers.
constexpr std::size_t units_per_capture_band = 256;

/// Adds completed capture work without touching the global frame timers.
void add(UnitCaptureTotals& total, const UnitCaptureTotals& more) noexcept {
    total.units += more.units;
    total.pieces += more.pieces;
    total.triplets += more.triplets;
}

/// Takes the original signed turn triplets and captures one unit's history.
UnitCaptureTotals capture_one(
    const UnitCaptureItem& item, uint32_t tick, std::vector<int32_t>& turns, UnitPoseCapture fields
) {
    auto& motion = *item.motion;
    const bool same_root =
        motion.seen && motion.instance_generation == item.generation && motion.tick == tick;
    if (same_root && (fields == UnitPoseCapture::root || motion.rig_valid))
        return {};
    if (fields == UnitPoseCapture::root) {
        observe_unit_root(motion, tick, item.generation, *item.unit, *item.instance);
        return {1, 0, 0};
    }
    const auto count = item.instance->pieces().size();
    turns.resize(count * 3);
    if (item.script == nullptr)
        std::fill(turns.begin(), turns.end(), 0);
    else
        for (std::size_t piece = 0; piece < count; ++piece) {
            const auto rates = item.script->piece_turn_speeds(static_cast<uint32_t>(piece));
            turns[piece * 3] = rates[2];
            turns[piece * 3 + 1] = rates[1];
            turns[piece * 3 + 2] = rates[0];
        }
    observe_unit(motion, tick, item.generation, *item.unit, *item.instance, turns);
    // A same-tick model transition captures a rig, not a second root endpoint.
    return {same_root ? 0U : 1U, count, item.script != nullptr ? count : 0};
}

/// Restores the worker's settings after using the capture caller's settings.
struct CaptureFloatControl {
    oa::base::float_precision::FloatControl previous;

    explicit CaptureFloatControl(const oa::base::float_precision::FloatControl& wanted)
        : previous(oa::base::float_precision::save_float_control()) {
        oa::base::float_precision::restore_float_control(wanted);
    }

    ~CaptureFloatControl() { oa::base::float_precision::restore_float_control(previous); }
};

} // namespace

UnitPoseCapture
unit_pose_capture(const oa::data::match_rules::MatchRules& rules, bool global_dots) noexcept {
    const auto& enhanced = rules.setup.enhanced_mode;
    return global_dots && enhanced.enabled &&
                   oa::data::enhanced_mode::uses_distributed_battle_flow(enhanced.version)
               ? UnitPoseCapture::root
               : UnitPoseCapture::full;
}

UnitCaptureTotals capture_unit_poses(
    std::span<const UnitCaptureItem> items,
    uint32_t tick,
    UnitCaptureScratch& scratch,
    oa::platform::job_pool::Pool* pool,
    UnitPoseCapture fields
) {
    const auto serial = [&] {
        UnitCaptureTotals totals;
        for (const auto& item : items)
            add(totals, capture_one(item, tick, scratch.serial_turns, fields));
        return totals;
    };
    const auto wanted = std::min<std::size_t>(
        pool != nullptr ? pool->threads() : 1, items.size() / units_per_capture_band
    );
    if (wanted < 2)
        return serial();
    std::size_t most_pieces = 0;
    for (const auto& item : items) {
        const auto& motion = *item.motion;
        const auto count = item.instance->pieces().size();
        // Only allocation-free, already initialized histories go to workers.
        // Cold captures retain the serial order and warm both alternating buffers.
        if (!motion.seen || motion.instance_generation != item.generation ||
            (fields == UnitPoseCapture::full && (motion.current.pieces.capacity() < count ||
                                                 motion.previous.pieces.capacity() < count)))
            return serial();
        if (fields == UnitPoseCapture::full)
            most_pieces = std::max(most_pieces, count);
    }
    try {
        scratch.bands.resize(wanted);
        for (auto& band : scratch.bands)
            band.turns.reserve(most_pieces * 3);
    } catch (const std::bad_alloc&) {
        return serial();
    }
    const auto control = oa::base::float_precision::save_float_control();
    for (auto& band : scratch.bands) {
        band.totals = {};
        band.failure = {};
    }
    oa::platform::job_pool::run_bands(
        pool, static_cast<uint32_t>(wanted), [&](uint32_t index) noexcept {
            const CaptureFloatControl rounding(control);
            auto& band = scratch.bands[index];
            const auto begin = items.size() * index / wanted;
            const auto end = items.size() * (index + 1) / wanted;
            for (auto at = begin; at < end; ++at)
                try {
                    add(band.totals, capture_one(items[at], tick, band.turns, fields));
                } catch (...) {
                    band.failure = std::current_exception();
                    band.failed_item = at;
                    break;
                }
        }
    );
    UnitCaptureTotals totals;
    const UnitCaptureBand* failed = nullptr;
    for (const auto& band : scratch.bands) {
        add(totals, band.totals);
        if (band.failure && (failed == nullptr || band.failed_item < failed->failed_item))
            failed = &band;
    }
    if (failed != nullptr)
        std::rethrow_exception(failed->failure);
    return totals;
}

} // namespace oa::app
