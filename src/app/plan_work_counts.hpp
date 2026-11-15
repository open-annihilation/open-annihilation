// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include "benchmark_thread_cpu.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>

namespace oa::app::plan_work {

/// Planner boundaries observed only by an explicitly enabled benchmark.
enum class Phase : uint8_t {
    pose_capture,
    list_prepare,
    unit_sites,
    battlefield_sort,
    run_prepare,
    run_wait,
    worker_plan,
    copy_place,
    original_note,
    original_update,
    copy_note,
    copy_update,
    omission_proof,
    model_plan_metadata,
    model_plan_pixels,
    count
};

/// Number of named planner boundaries.
inline constexpr size_t phase_count = static_cast<size_t>(Phase::count);
/// Stable column names for planner observations.
inline constexpr std::array<std::string_view, phase_count> phase_names{
    "pose_capture",
    "list_prepare",
    "unit_sites",
    "battlefield_sort",
    "run_prepare",
    "run_wait",
    "worker_plan",
    "copy_place",
    "original_note",
    "original_update",
    "copy_note",
    "copy_update",
    "omission_proof",
    "model_plan_metadata",
    "model_plan_pixels"
};

/// Actual planner calls and completed work, independent of timing samples.
enum class Work : uint8_t {
    unit_plans,
    copy_plans,
    linked_plans,
    omitted_plans,
    original_updates_dirty, ///< original dirty on update entry, before applying the root threshold
    copy_updates_dirty,     ///< copy dirty on update entry, before applying the root threshold
    transform_cache_hits,   ///< whole-pose hits in the direct cached-update branch
    root_prefix_hits,       ///< root-prefix hits in the direct cached-update branch
    transform_rebuilds,     ///< direct rebuild calls; linked native updates remain opaque
    metadata_image_builds,
    pixel_image_builds,
    count
};

/// Number of named planner work counters.
inline constexpr size_t work_count = static_cast<size_t>(Work::count);
/// Stable column names for completed planner work.
inline constexpr std::array<std::string_view, work_count> work_names{
    "unit_plans",
    "copy_plans",
    "linked_plans",
    "omitted_plans",
    "original_updates_dirty",
    "copy_updates_dirty",
    "transform_cache_hits",
    "root_prefix_hits",
    "transform_rebuilds",
    "metadata_image_builds",
    "pixel_image_builds"
};

/// Raw observations of one boundary; durations cover only its timed samples.
/// CPU samples require two successful, nondecreasing reads of the same thread.
/// A zero CPU duration with no CPU samples means unavailable, not idle work.
struct PhaseCounts {
    uint64_t calls{};
    uint64_t samples{};
    uint64_t wall_ns{};
    uint64_t cpu_ns{};
    uint64_t cpu_samples{};
};

/// One benchmark frame's exact work and raw sampled planner durations.
/// Worker durations include nested unit spans and overlap the caller's run_wait.
/// Sums of worker wall time are not frame elapsed time. Sampled durations are
/// never extrapolated to unsampled units or treated as full-population latency.
struct PlanWorkCounts {
    uint64_t frame_id{};
    bool completed{};
    uint32_t sampling_stride{64};
    std::array<PhaseCounts, phase_count> phases{};
    std::array<uint64_t, work_count> work{};
};

/// One thread's stack-owned counters and current unit sampling decision.
struct Collector {
    PlanWorkCounts counts{};
    bool sample{true};
};

/// Ordinary gameplay leaves planner observation disabled.
inline bool enabled{};
/// The last explicitly published benchmark frame, incomplete until finish().
inline PlanWorkCounts frame_counts{};
/// The calling thread's active destination; workers never share a collector.
inline thread_local Collector* current{};

/// Temporarily binds an independent collector, preserving nested callers.
class Binding {
  public:

    /// Binds the collector; null temporarily disables all observation.
    /// @param collector the calling thread's destination, which outlives this binding
    explicit Binding(Collector* collector) noexcept : previous_(current) { current = collector; }

    Binding(const Binding&) = delete;
    Binding& operator=(const Binding&) = delete;

    /// Restores the collector active before this binding.
    ~Binding() { current = previous_; }

  private:

    Collector* previous_{};
};

/// Returns whether this thread currently has an observation destination.
/// @return true inside a nonnull binding
[[nodiscard]] inline bool recording() noexcept {
    return current != nullptr;
}

/// Returns the active frame identity without reading a clock.
/// @return the collector's frame, or zero without a binding
[[nodiscard]] inline uint64_t source_frame() noexcept {
    return current != nullptr ? current->counts.frame_id : 0;
}

/// Counts every reached boundary and times selected calls on their own thread.
/// A span retains its original destination across nested collector bindings.
/// Its wall interval encloses both reads of the paired thread CPU clock.
class Span {
  public:

    /// Records a call, sampling it when the current unit or whole boundary asks.
    /// Disabled and unsampled calls read neither the wall nor the CPU clock.
    /// @param phase the boundary reached
    /// @param whole true to time every call regardless of the unit sample
    explicit Span(Phase phase, bool whole = false) noexcept : collector_(current), phase_(phase) {
        const auto index = static_cast<size_t>(phase_);
        if (collector_ == nullptr || index >= phase_count)
            return;
        ++collector_->counts.phases[index].calls;
        if (!whole && !collector_->sample)
            return;
        wall_since_ = Clock::now();
        cpu_since_ = benchmark_timing::thread_cpu_nanoseconds();
        running_ = true;
    }

    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;

    /// Stops a running sample on every scope exit.
    ~Span() { stop(); }

    /// Records this sample once; repeated stops and unsampled calls do nothing.
    void stop() noexcept {
        if (!running_)
            return;
        running_ = false;
        const auto cpu_until = benchmark_timing::thread_cpu_nanoseconds();
        const auto wall_until = Clock::now();
        auto& into = collector_->counts.phases[static_cast<size_t>(phase_)];
        ++into.samples;
        into.wall_ns += static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(wall_until - wall_since_).count()
        );
        if (cpu_since_ && cpu_until && *cpu_until >= *cpu_since_) {
            into.cpu_ns += *cpu_until - *cpu_since_;
            ++into.cpu_samples;
        }
    }

  private:

    using Clock = std::chrono::steady_clock;
    Collector* collector_{};
    Phase phase_{};
    Clock::time_point wall_since_{};
    std::optional<uint64_t> cpu_since_{};
    bool running_{};
};

/// Adds actual completed work to the calling thread's destination.
/// @param work the named work performed
/// @param amount completed calls or units, independent of timing sampling
inline void count(Work work, uint64_t amount = 1) noexcept {
    const auto index = static_cast<size_t>(work);
    if (current != nullptr && index < work_count)
        current->counts.work[index] += amount;
}

/// Adds phase and work totals, preserving the destination's frame metadata.
/// @param[in,out] into the caller-owned aggregate
/// @param more the joined thread's independent observations
inline void merge(PlanWorkCounts& into, const PlanWorkCounts& more) noexcept {
    for (size_t index = 0; index < phase_count; ++index) {
        auto& target = into.phases[index];
        const auto& source = more.phases[index];
        target.calls += source.calls;
        target.samples += source.samples;
        target.wall_ns += source.wall_ns;
        target.cpu_ns += source.cpu_ns;
        target.cpu_samples += source.cpu_samples;
    }
    for (size_t index = 0; index < work_count; ++index)
        into.work[index] += more.work[index];
}

/// Merges joined observations into the current destination, when bound.
/// @param more a collector no worker is still writing
inline void merge_active(const PlanWorkCounts& more) noexcept {
    if (current != nullptr)
        merge(current->counts, more);
}

/// Selects one rotating unit-slot residue and restores the previous decision.
class UnitSampling {
  public:

    /// Selects slot modulo stride equal to frame modulo stride, normally one in 64.
    /// A zero stride samples no units. No random stream or clock is read.
    /// @param slot the canonical unit slot whose subspans follow
    explicit UnitSampling(uint64_t slot) noexcept : collector_(current) {
        if (collector_ == nullptr)
            return;
        previous_ = collector_->sample;
        const auto stride = collector_->counts.sampling_stride;
        collector_->sample = stride != 0 && slot % stride == collector_->counts.frame_id % stride;
    }

    UnitSampling(const UnitSampling&) = delete;
    UnitSampling& operator=(const UnitSampling&) = delete;

    /// Restores the sampling decision of this scope's original collector.
    ~UnitSampling() {
        if (collector_ != nullptr)
            collector_->sample = previous_;
    }

  private:

    Collector* collector_{};
    bool previous_{};
};

/// Owns one caller's observations; only an explicit finish publishes them.
/// The collector and binding outlive worker joins and all measured spans.
class Frame {
  public:

    /// Resets published counters only when this frame records; false binds null.
    /// An exceptional exit before finish leaves the published frame incomplete.
    /// @param observe true for an explicitly requested benchmark observation
    explicit Frame(bool observe) noexcept
        : binding_(observe ? &collector_ : nullptr), active_(observe) {
        if (active_)
            frame_counts = {};
    }

    Frame(const Frame&) = delete;
    Frame& operator=(const Frame&) = delete;

    /// Restores the previous binding without publishing unfinished observations.
    ~Frame() = default;

    /// Sets only this frame's identity, used by rotating unit samples.
    /// @param frame_id the caller's benchmark frame identity
    void set_frame(uint64_t frame_id) noexcept {
        if (active_)
            collector_.counts.frame_id = frame_id;
    }

    /// Publishes this frame after its spans stopped and workers joined.
    /// @param success whether the complete requested frame was drawn
    void finish(bool success) noexcept {
        if (!active_)
            return;
        collector_.counts.completed = success;
        frame_counts = collector_.counts;
    }

  private:

    Collector collector_{};
    Binding binding_;
    bool active_{};
};

} // namespace oa::app::plan_work
