// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Planner observation stays opt-in, publishes only explicitly finished frames,
// and merges sampled worker work without changing the caller's frame identity.
#include "plan_work_counts.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {

namespace plan = oa::app::plan_work;

/// Reads one named phase without relying on its enumeration position.
const plan::PhaseCounts& phase(const plan::PlanWorkCounts& counts, plan::Phase name) {
    return counts.phases[static_cast<std::size_t>(name)];
}

/// Compares every recorded phase field, including the CPU sample denominator.
bool same_phase(const plan::PhaseCounts& first, const plan::PhaseCounts& second) {
    return first.calls == second.calls && first.samples == second.samples &&
           first.wall_ns == second.wall_ns && first.cpu_ns == second.cpu_ns &&
           first.cpu_samples == second.cpu_samples;
}

/// Compares published counters and the metadata of the frame that owns them.
bool same_counts(const plan::PlanWorkCounts& first, const plan::PlanWorkCounts& second) {
    if (first.frame_id != second.frame_id || first.completed != second.completed ||
        first.sampling_stride != second.sampling_stride || first.work != second.work)
        return false;
    for (std::size_t index = 0; index < plan::phase_count; ++index)
        if (!same_phase(first.phases[index], second.phases[index]))
            return false;
    return true;
}

/// Keeps test changes to the opt-in controller and published result local.
struct RestoreObserver {
    bool enabled{plan::enabled};
    plan::PlanWorkCounts published{plan::frame_counts};

    ~RestoreObserver() {
        plan::enabled = enabled;
        plan::frame_counts = published;
    }
};

/// Disabled frames hide an enclosing collector and never publish its stale work.
void test_disabled_frame() {
    RestoreObserver restore;
    OA_CHECK(!plan::enabled);
    OA_CHECK(plan::current == nullptr);
    plan::Collector enclosing;
    enclosing.counts.frame_id = 90;
    plan::frame_counts.frame_id = 89;
    plan::frame_counts.completed = true;
    const auto accepted = plan::frame_counts;
    {
        plan::Binding caller(&enclosing);
        {
            plan::Frame disabled(false);
            OA_CHECK(plan::current == nullptr);
            OA_CHECK(!plan::recording());
            OA_CHECK(plan::source_frame() == 0);
            disabled.set_frame(91);
            plan::count(plan::Work::unit_plans, 7);
            {
                plan::Span ignored(plan::Phase::worker_plan, true);
            }
            disabled.finish(true);
            OA_CHECK(same_counts(plan::frame_counts, accepted));
        }
        OA_CHECK(plan::current == &enclosing);
        OA_CHECK(phase(enclosing.counts, plan::Phase::worker_plan).calls == 0);
        OA_CHECK(enclosing.counts.work[static_cast<std::size_t>(plan::Work::unit_plans)] == 0);
    }
    OA_CHECK(plan::current == nullptr);
    OA_CHECK(same_counts(plan::frame_counts, accepted));
}

/// A frame owns local counters until an explicit successful finish publishes them.
void test_explicit_publication() {
    RestoreObserver restore;
    plan::Collector enclosing;
    plan::Binding caller(&enclosing);
    plan::enabled = true;
    plan::frame_counts.frame_id = 400;
    plan::frame_counts.completed = true;
    plan::frame_counts.work.fill(17);
    plan::PlanWorkCounts accepted;
    {
        plan::Frame frame(plan::enabled);
        OA_CHECK(plan::recording());
        OA_CHECK(plan::current != nullptr && plan::current != &enclosing);
        OA_CHECK(plan::frame_counts.frame_id == 0 && !plan::frame_counts.completed);
        OA_CHECK(plan::frame_counts.work == plan::PlanWorkCounts{}.work);
        frame.set_frame(401);
        OA_CHECK(plan::current != nullptr && plan::current->counts.frame_id == 401);
        OA_CHECK(plan::source_frame() == 401);
        OA_CHECK(plan::frame_counts.frame_id == 0);
        plan::count(plan::Work::copy_plans, 3);
        {
            plan::Span observed(plan::Phase::run_prepare, true);
        }
        frame.finish(true);
        accepted = plan::frame_counts;
        OA_CHECK(accepted.frame_id == 401 && accepted.completed);
        OA_CHECK(accepted.work[static_cast<std::size_t>(plan::Work::copy_plans)] == 3);
        const auto& captured = phase(accepted, plan::Phase::run_prepare);
        OA_CHECK(captured.calls == 1 && captured.samples == 1);
        OA_CHECK(captured.cpu_samples <= captured.samples);
        if (captured.cpu_samples == 0)
            OA_CHECK(captured.cpu_ns == 0);
        plan::count(plan::Work::copy_plans, 5);
    }
    OA_CHECK(plan::current == &enclosing);
    OA_CHECK(same_counts(plan::frame_counts, accepted));
}

/// Nested observed frames publish their own work and restore the outer destination.
void test_nested_frames() {
    RestoreObserver restore;
    {
        plan::Frame outer(true);
        outer.set_frame(451);
        auto* outer_collector = plan::current;
        plan::count(plan::Work::unit_plans, 2);
        {
            plan::Frame inner(true);
            inner.set_frame(452);
            OA_CHECK(plan::current != outer_collector && plan::source_frame() == 452);
            OA_CHECK(!plan::frame_counts.completed && plan::frame_counts.frame_id == 0);
            plan::count(plan::Work::unit_plans, 7);
            inner.finish(true);
            OA_CHECK(plan::frame_counts.completed && plan::frame_counts.frame_id == 452);
            OA_CHECK(
                plan::frame_counts.work[static_cast<std::size_t>(plan::Work::unit_plans)] == 7
            );
        }
        OA_CHECK(plan::current == outer_collector && plan::source_frame() == 451);
        plan::count(plan::Work::unit_plans, 3);
        outer.finish(true);
        OA_CHECK(plan::frame_counts.completed && plan::frame_counts.frame_id == 451);
        OA_CHECK(plan::frame_counts.work[static_cast<std::size_t>(plan::Work::unit_plans)] == 5);
    }
    OA_CHECK(plan::current == nullptr);
}

/// Missing finish and exception unwinding leave the reset frame incomplete.
void test_unfinished_and_failed_frames() {
    RestoreObserver restore;
    plan::Collector enclosing;
    plan::Binding caller(&enclosing);
    {
        plan::Frame unfinished(true);
        unfinished.set_frame(501);
        plan::count(plan::Work::unit_plans);
    }
    OA_CHECK(plan::current == &enclosing);
    OA_CHECK(plan::frame_counts.frame_id == 0 && !plan::frame_counts.completed);
    OA_CHECK(plan::frame_counts.work == plan::PlanWorkCounts{}.work);
    try {
        plan::Frame failed(true);
        failed.set_frame(502);
        plan::Span interrupted(plan::Phase::unit_sites, true);
        throw std::runtime_error("leave the observed frame unfinished");
    } catch (const std::runtime_error&) {
    }
    OA_CHECK(plan::current == &enclosing);
    OA_CHECK(plan::frame_counts.frame_id == 0 && !plan::frame_counts.completed);
    OA_CHECK(phase(plan::frame_counts, plan::Phase::unit_sites).calls == 0);
    {
        plan::Frame failed(true);
        failed.set_frame(503);
        plan::count(plan::Work::unit_plans, 2);
        failed.finish(false);
    }
    OA_CHECK(plan::frame_counts.frame_id == 503 && !plan::frame_counts.completed);
    OA_CHECK(plan::frame_counts.work[static_cast<std::size_t>(plan::Work::unit_plans)] == 2);
}

/// Each slot is sampled once per stride while every logical phase call is counted.
void test_rotating_unit_sampling() {
    constexpr uint32_t stride = 64;
    std::array<uint32_t, stride> sampled_frames{};
    for (uint32_t frame_id = 0; frame_id < stride; ++frame_id) {
        plan::Collector collector;
        collector.counts.frame_id = frame_id;
        OA_CHECK(collector.counts.sampling_stride == stride);
        plan::Binding binding(&collector);
        for (uint32_t slot = 0; slot < stride; ++slot) {
            {
                plan::UnitSampling unit(slot);
                sampled_frames[slot] += collector.sample ? 1U : 0U;
                plan::count(plan::Work::unit_plans);
                plan::Span observed(plan::Phase::copy_place);
            }
            OA_CHECK(collector.sample);
        }
        const auto& captured = phase(collector.counts, plan::Phase::copy_place);
        OA_CHECK(captured.calls == stride && captured.samples == 1);
        OA_CHECK(captured.cpu_samples <= captured.samples);
        OA_CHECK(collector.counts.work[static_cast<std::size_t>(plan::Work::unit_plans)] == stride);
    }
    for (const auto frames : sampled_frames)
        OA_CHECK(frames == 1);

    plan::Collector collector;
    collector.counts.sampling_stride = 0;
    plan::Binding binding(&collector);
    for (uint32_t slot = 0; slot < stride; ++slot) {
        plan::UnitSampling unit(slot);
        OA_CHECK(!collector.sample);
        plan::Span unsampled(plan::Phase::copy_note);
    }
    const auto& unsampled = phase(collector.counts, plan::Phase::copy_note);
    OA_CHECK(unsampled.calls == stride && unsampled.samples == 0);
    OA_CHECK(unsampled.wall_ns == 0 && unsampled.cpu_ns == 0 && unsampled.cpu_samples == 0);
    {
        plan::UnitSampling unit(0);
        plan::Span whole(plan::Phase::battlefield_sort, true);
    }
    OA_CHECK(phase(collector.counts, plan::Phase::battlefield_sort).samples == 1);
}

/// Invalid phase and work sentinels leave all bounded arrays untouched.
void test_invalid_names() {
    plan::Collector collector;
    plan::Binding binding(&collector);
    {
        plan::Span invalid(plan::Phase::count, true);
        plan::count(plan::Work::count, 10);
        invalid.stop();
    }
    OA_CHECK(same_counts(collector.counts, {}));
}

/// Spans and sampling scopes restore their captured collector under nested bindings.
void test_nested_scope_ownership() {
    plan::Collector outer, inner;
    outer.counts.sampling_stride = 0;
    inner.sample = false;
    plan::Binding caller(&outer);
    plan::Span observed(plan::Phase::original_update, true);
    std::optional<plan::UnitSampling> sampling;
    sampling.emplace(0);
    OA_CHECK(!outer.sample);
    {
        plan::Binding nested(&inner);
        observed.stop();
        const auto stopped = phase(outer.counts, plan::Phase::original_update);
        observed.stop();
        OA_CHECK(same_phase(phase(outer.counts, plan::Phase::original_update), stopped));
        OA_CHECK(phase(inner.counts, plan::Phase::original_update).calls == 0);
        sampling.reset();
        OA_CHECK(outer.sample && !inner.sample);
        {
            plan::Binding absent(nullptr);
            OA_CHECK(plan::current == nullptr && !plan::recording());
        }
        OA_CHECK(plan::current == &inner);
    }
    OA_CHECK(plan::current == &outer);
    OA_CHECK(phase(outer.counts, plan::Phase::original_update).calls == 1);
    OA_CHECK(phase(outer.counts, plan::Phase::original_update).samples == 1);
    try {
        plan::Binding nested(&inner);
        throw std::runtime_error("restore the caller collector");
    } catch (const std::runtime_error&) {
    }
    OA_CHECK(plan::current == &outer);
}

/// Merging wall-only samples never manufactures thread CPU observations.
void test_merge_preserves_identity_and_valid_pairs() {
    plan::PlanWorkCounts target, source;
    target.frame_id = 61;
    target.completed = true;
    target.sampling_stride = 64;
    source.frame_id = 999;
    source.sampling_stride = 1;
    const auto index = static_cast<std::size_t>(plan::Phase::worker_plan);
    target.phases[index] = {7, 3, 90, 50, 2};
    source.phases[index] = {4, 2, 80, 0, 0};
    target.work[static_cast<std::size_t>(plan::Work::transform_rebuilds)] = 3;
    source.work[static_cast<std::size_t>(plan::Work::transform_rebuilds)] = 5;
    const auto source_before = source;
    plan::merge(target, source);
    OA_CHECK(target.frame_id == 61 && target.completed && target.sampling_stride == 64);
    const auto& merged = target.phases[index];
    OA_CHECK(merged.calls == 11 && merged.samples == 5 && merged.wall_ns == 170);
    OA_CHECK(merged.cpu_ns == 50 && merged.cpu_samples == 2);
    OA_CHECK(target.work[static_cast<std::size_t>(plan::Work::transform_rebuilds)] == 8);
    OA_CHECK(same_counts(source, source_before));
    plan::merge_active(source);
    OA_CHECK(plan::current == nullptr);
}

/// Worker bindings are thread-local and merge only after their work has joined.
void test_worker_tls_and_merge() {
    plan::Collector caller, worker;
    caller.counts.frame_id = 71;
    worker.counts.frame_id = 72;
    plan::Binding binding(&caller);
    bool started_without_collector{}, restored_without_collector{}, nested_restored{};
    std::thread thread([&] {
        started_without_collector = plan::current == nullptr && !plan::recording();
        {
            plan::Binding worker_binding(&worker);
            {
                plan::Span work(plan::Phase::worker_plan, true);
                plan::count(plan::Work::unit_plans, 4);
            }
            {
                plan::Binding absent(nullptr);
                plan::count(plan::Work::unit_plans, 10);
            }
            nested_restored = plan::current == &worker;
        }
        restored_without_collector = plan::current == nullptr;
    });
    plan::count(plan::Work::copy_plans, 2);
    thread.join();
    OA_CHECK(started_without_collector && restored_without_collector && nested_restored);
    OA_CHECK(plan::current == &caller);
    OA_CHECK(caller.counts.work[static_cast<std::size_t>(plan::Work::unit_plans)] == 0);
    plan::merge_active(worker.counts);
    OA_CHECK(caller.counts.frame_id == 71 && !caller.counts.completed);
    OA_CHECK(caller.counts.work[static_cast<std::size_t>(plan::Work::unit_plans)] == 4);
    OA_CHECK(caller.counts.work[static_cast<std::size_t>(plan::Work::copy_plans)] == 2);
    const auto& captured = phase(caller.counts, plan::Phase::worker_plan);
    OA_CHECK(captured.calls == 1 && captured.samples == 1);
    OA_CHECK(captured.cpu_samples <= captured.samples);
    if (captured.cpu_samples == 0)
        OA_CHECK(captured.cpu_ns == 0);
}

} // namespace

int main() {
    test_disabled_frame();
    test_explicit_publication();
    test_nested_frames();
    test_unfinished_and_failed_frames();
    test_rotating_unit_sampling();
    test_invalid_names();
    test_nested_scope_ownership();
    test_merge_preserves_identity_and_valid_pairs();
    test_worker_tls_and_merge();
    return oa::test::check_exit_status();
}
