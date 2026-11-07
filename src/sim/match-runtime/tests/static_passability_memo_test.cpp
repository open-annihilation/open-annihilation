// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "static_passability_memo.hpp"
#include "oa/sim/ground_orders/shared_routes.hpp"
#include "oa/sim/spatial_state/cell_classifier.hpp"
#include "oa/sim/spatial_state/spatial.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <chrono>
#include <cstdio>
#include <limits>
#include <memory>
#include <string_view>

namespace {
namespace runtime = oa::sim::match_runtime;
namespace spatial = oa::sim::spatial_state;

struct Fixture {
    std::unique_ptr<runtime::StaticPassabilityMemo> memo{
        std::make_unique<runtime::StaticPassabilityMemo>()
    };
    spatial::World world{};
    std::array<spatial::CellClassifier, 2> classes{};
    std::array<bool, 2> known{true, true};
    uint64_t physical_calls{};

    explicit Fixture(uint32_t width = 16, uint32_t height = 16) {
        world.terrain_width = width;
        world.terrain_height = height;
        world.plots.resize(size_t(width) * height);
        world.sea_level = 20;
        for (auto& plot : world.plots)
            plot.low_height = plot.high_height = 20;
        classes[0] = {1, 1, 10, -10, 6, 3, 8, 4, 0};
        classes[1] = {2, 2, 10, -10, 6, 3, 8, 4, 0};
    }

    uint8_t physical(uint32_t klass, int32_t x, int32_t z) {
        ++physical_calls;
        return oa::sim::ground_orders::static_flow_passable(classes[klass], x, z, world) ? 1 : 0;
    }

    uint8_t sample(uint32_t klass, uint32_t player, int32_t x, int32_t z) {
        // The knowledge guard belongs to every sampler call, before memo lookup.
        if (!known[player])
            return 2;
        uint8_t value{};
        if (!memo->find(klass, x, z, value)) {
            value = physical(klass, x, z);
            memo->remember(klass, x, z, value);
        }
        return value;
    }
};

void repeated_checks_keep_both_physical_results_and_complete_keys() {
    Fixture fixture;
    uint8_t unchanged = 7;
    OA_CHECK(!fixture.memo->find(0, 0, 0, unchanged));
    OA_CHECK(unchanged == 7);
    fixture.memo->remember(0, 0, 0, 1);
    OA_CHECK(!fixture.memo->find(0, 0, 0, unchanged));
    fixture.memo->begin_pass();
    fixture.world.plots[3 * 16 + 4].blocking_feature = 1;
    for (unsigned repeat = 0; repeat < 100; ++repeat) {
        OA_CHECK(fixture.sample(0, 0, 3, 3) == 1);
        OA_CHECK(fixture.sample(1, 0, 3, 3) == 0);
        OA_CHECK(fixture.sample(0, 1, 3, 3) == 1);
        OA_CHECK(fixture.sample(1, 1, 3, 3) == 0);
    }
    OA_CHECK(fixture.physical_calls == 2);
    OA_CHECK(fixture.sample(0, 0, 4, 3) == 0);
    OA_CHECK(fixture.sample(0, 0, 3, 4) == 1);
    OA_CHECK(fixture.physical_calls == 4);
}

void same_tick_new_invocations_recheck_new_barriers() {
    Fixture fixture;
    constexpr uint32_t game_tick = 42;
    const auto pass = [&](uint32_t tick) {
        OA_CHECK(tick == game_tick);
        fixture.memo->begin_pass();
        return fixture.sample(1, 0, 3, 3);
    };
    OA_CHECK(pass(game_tick) == 1);
    fixture.world.plots[3 * 16 + 4].blocking_feature = 1;
    OA_CHECK(pass(game_tick) == 0);
    fixture.world.plots[3 * 16 + 4].blocking_feature = 0;
    OA_CHECK(pass(game_tick) == 1);
    OA_CHECK(fixture.physical_calls == 3);
}

void fog_is_checked_before_every_shared_physical_lookup() {
    Fixture fixture;
    fixture.memo->begin_pass();
    fixture.known[0] = false;
    fixture.world.plots[3 * 16 + 4].blocking_feature = 1;
    OA_CHECK(fixture.sample(1, 0, 3, 3) == 2);
    OA_CHECK(fixture.physical_calls == 0);
    OA_CHECK(fixture.sample(1, 1, 3, 3) == 0);
    OA_CHECK(fixture.physical_calls == 1);
    OA_CHECK(fixture.sample(1, 0, 3, 3) == 2);
    OA_CHECK(fixture.physical_calls == 1);
    fixture.known[0] = true;
    OA_CHECK(fixture.sample(1, 0, 3, 3) == 0);
    OA_CHECK(fixture.physical_calls == 1);
    fixture.known[1] = false;
    OA_CHECK(fixture.sample(1, 1, 3, 3) == 2);
    fixture.memo->remember(0, 8, 8, 2);
    uint8_t unchanged = 7;
    OA_CHECK(!fixture.memo->find(0, 8, 8, unchanged));
    OA_CHECK(unchanged == 7);
}

void colliding_keys_replace_storage_without_reusing_another_cell() {
    auto memo = std::make_unique<runtime::StaticPassabilityMemo>();
    auto first_columns =
        std::make_unique<std::array<int32_t, runtime::static_passability_entry_count>>();
    first_columns->fill(-1);
    int32_t first = -1, second = -1;
    for (int32_t x = 0; x <= int32_t(runtime::static_passability_entry_count); ++x) {
        const auto slot = runtime::StaticPassabilityMemo::slot(0, x, 0);
        if ((*first_columns)[slot] >= 0) {
            first = (*first_columns)[slot];
            second = x;
            break;
        }
        (*first_columns)[slot] = x;
    }
    OA_CHECK(first >= 0 && second > first);
    memo->begin_pass();
    memo->remember(0, first, 0, 1);
    uint8_t value{};
    OA_CHECK(memo->find(0, first, 0, value) && value == 1);
    memo->remember(0, second, 0, 0);
    OA_CHECK(!memo->find(0, first, 0, value));
    OA_CHECK(memo->find(0, second, 0, value) && value == 0);
    const auto low = std::numeric_limits<int32_t>::min();
    const auto high = std::numeric_limits<int32_t>::max();
    memo->remember(std::numeric_limits<uint32_t>::max(), low, high, 1);
    OA_CHECK(memo->find(std::numeric_limits<uint32_t>::max(), low, high, value) && value == 1);
    OA_CHECK(!memo->find(std::numeric_limits<uint32_t>::max() - 1, low, high, value));
}

void wrapped_epochs_clear_old_stamps_before_reuse() {
    std::array<runtime::StaticPassabilityEntry, 3> entries{};
    entries[0].epoch = 1;
    entries[1].epoch = 17;
    entries[2].epoch = std::numeric_limits<uint32_t>::max();
    uint32_t epoch = std::numeric_limits<uint32_t>::max();
    runtime::advance_static_passability_epoch(epoch, entries);
    OA_CHECK(epoch == 1);
    for (const auto& entry : entries)
        OA_CHECK(entry.epoch == 0);
    entries[1].epoch = 1;
    runtime::advance_static_passability_epoch(epoch, entries);
    OA_CHECK(epoch == 2 && entries[1].epoch == 1);
}

void benchmark_repeated_physical_footprints() {
    Fixture fixture(256, 256);
    constexpr unsigned passes = 16, fields = 64, columns = 64, rows = 64;
    const auto run = [&](bool memoized) {
        fixture.physical_calls = 0;
        uint64_t checksum = 0;
        const auto start = std::chrono::steady_clock::now();
        for (unsigned pass = 0; pass < passes; ++pass) {
            fixture.memo->begin_pass();
            for (unsigned field = 0; field < fields; ++field)
                for (unsigned z = 0; z < rows; ++z)
                    for (unsigned x = 0; x < columns; ++x)
                        checksum +=
                            memoized ? fixture.sample(field % 2, 0, int32_t(x * 3), int32_t(z * 3))
                                     : fixture.physical(field % 2, int32_t(x * 3), int32_t(z * 3));
        }
        const auto elapsed =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                .count();
        std::printf(
            "%s checksum %llu, physical calls %llu, %.3f ms\n",
            memoized ? "memo" : "direct",
            static_cast<unsigned long long>(checksum),
            static_cast<unsigned long long>(fixture.physical_calls),
            elapsed
        );
        return std::array<uint64_t, 2>{checksum, fixture.physical_calls};
    };
    const auto direct = run(false), memo = run(true);
    OA_CHECK(direct[0] == memo[0]);
    OA_CHECK(memo[1] < direct[1] / 4);
    std::printf("memo storage %zu bytes\n", sizeof(runtime::StaticPassabilityMemo));
}
} // namespace

int main(int argc, char** argv) {
    repeated_checks_keep_both_physical_results_and_complete_keys();
    same_tick_new_invocations_recheck_new_barriers();
    fog_is_checked_before_every_shared_physical_lookup();
    colliding_keys_replace_storage_without_reusing_another_cell();
    wrapped_epochs_clear_old_stamps_before_reuse();
    if (argc > 1 && std::string_view(argv[1]) == "--benchmark")
        benchmark_repeated_physical_footprints();
    return oa::test::check_exit_status();
}
