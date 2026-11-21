// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Independent scalar captures and drawing-pool captures leave the same full
// history, including rates, across tick, generation and attachment changes.
#include "presentation_capture.hpp"
#include "oa/data/enhanced_mode.hpp"

#include "oa/base/float_precision.hpp"

#include <array>
#include <atomic>
#include <cfenv>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace {

namespace app = oa::app;
namespace runtime = oa::sim::match_runtime;
namespace vm = oa::sim::script_vm;
namespace fp = oa::base::float_precision;
namespace jobs = oa::platform::job_pool;

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

/// Counts host access separately from snapshot export, which reads model state.
struct Host final : vm::Host {
    mutable std::atomic<uint64_t> calls{};

    int32_t piece_position(uint32_t, uint32_t) const override {
        ++calls;
        return 0;
    }

    int32_t piece_angle(uint32_t, uint32_t) const override {
        ++calls;
        return 0;
    }

    uint32_t piece_visible(uint32_t) const override {
        ++calls;
        return 1;
    }

    uint32_t piece_cached(uint32_t) const override {
        ++calls;
        return 0;
    }

    uint32_t piece_shaded(uint32_t) const override {
        ++calls;
        return 1;
    }

    void set_piece_position(uint32_t, uint32_t, int32_t) override { ++calls; }

    void set_piece_angle(uint32_t, uint32_t, uint32_t) override { ++calls; }

    void set_piece_visible(uint32_t, uint32_t) override { ++calls; }

    void set_piece_cached(uint32_t, uint32_t) override { ++calls; }

    void set_piece_shaded(uint32_t, uint32_t) override { ++calls; }

    void emit_sfx(uint32_t, int32_t) override { ++calls; }

    void explode(uint32_t, int32_t) override { ++calls; }

    int32_t get_unit_value(int32_t, int32_t, int32_t, int32_t, int32_t) override {
        ++calls;
        return 0;
    }

    void set_unit_value(int32_t, int32_t) override { ++calls; }

    void attach_unit(int32_t, int32_t, int32_t) override { ++calls; }

    void drop_unit(int32_t) override { ++calls; }

    void ignored_piece_op(uint32_t, uint32_t, uint32_t) override { ++calls; }

    void dont_shadow(uint32_t) override { ++calls; }

    uint32_t is_carrying_unit(uint32_t) override {
        ++calls;
        return 0;
    }

    uint32_t carrier_unit_id() override {
        ++calls;
        return 0;
    }

    uint32_t random_bounded(uint32_t) override {
        ++calls;
        return 0;
    }
};

struct Program {
    std::shared_ptr<const oa::formats::cob::CobProgram> source;
    runtime::SharedScriptProgram shared;
};

Program make_program(std::size_t count) {
    auto source = std::make_shared<oa::formats::cob::CobProgram>();
    source->code = {vm::opcode::push_constant, 0, vm::opcode::stop_spin, 0, 1, vm::opcode::return_};
    source->entry_points = {0};
    source->scripts = {{"Stop", 0}};
    source->piece_names.resize(count);
    return {source, runtime::prepare_type_script(source)};
}

std::shared_ptr<const oa::formats::objects3d::Model> make_model(std::size_t count) {
    auto model = std::make_shared<oa::formats::objects3d::Model>();
    model->objects.resize(count);
    for (std::size_t piece = 0; piece < count; ++piece) {
        auto& object = model->objects[piece];
        object.name = "piece" + std::to_string(piece);
        if (piece != 0)
            object.parent = 0;
        if (piece == 0 && count > 1)
            object.first_child = 1;
        else if (piece + 1 < count)
            object.next_sibling = static_cast<uint32_t>(piece + 1);
    }
    return model;
}

/// One actor shared by the independent histories; the capture never writes it.
struct Actor {
    Host host;
    std::unique_ptr<runtime::ScriptInstance> script;
    oa::Unit record{};
    oa::sim::model_runtime::Instance model;
    std::array<app::UnitMotion, 5> histories;
};

bool same_point(const auto& a, const auto& b) {
    return a.x == b.x && a.y == b.y && a.z == b.z;
}

bool same_pose(const app::UnitPose& a, const app::UnitPose& b) {
    if (!same_point(a.position, b.position) || a.heading != b.heading || a.pitch != b.pitch ||
        a.bank != b.bank || a.attach_parent != b.attach_parent ||
        a.pieces.size() != b.pieces.size())
        return false;
    for (std::size_t index = 0; index < a.pieces.size(); ++index) {
        const auto& left = a.pieces[index];
        const auto& right = b.pieces[index];
        if (!same_point(left.translation, right.translation) ||
            left.rotation.xy != right.rotation.xy || left.rotation.xz != right.rotation.xz ||
            left.rotation.yz != right.rotation.yz || left.turn_per_tick != right.turn_per_tick)
            return false;
    }
    return true;
}

bool same_history(const app::UnitMotion& a, const app::UnitMotion& b) {
    return a.instance_generation == b.instance_generation && a.tick == b.tick && a.seen == b.seen &&
           a.continued == b.continued && a.moved == b.moved && a.pieces_moved == b.pieces_moved &&
           a.blended_draw == b.blended_draw && a.state.cache.draws == b.state.cache.draws &&
           a.state.image_builds == b.state.image_builds &&
           a.state.transforms_dirty == b.state.transforms_dirty &&
           same_pose(a.previous, b.previous) && same_pose(a.current, b.current);
}

/// Original capture expressed through scalar reads, independent of triplet packing.
app::UnitCaptureTotals scalar_capture(std::span<const app::UnitCaptureItem> items, uint32_t tick) {
    app::UnitCaptureTotals totals;
    std::vector<int32_t> rates;
    for (const auto& item : items) {
        const auto& history = *item.motion;
        if (history.seen && history.instance_generation == item.generation && history.tick == tick)
            continue;
        const auto count = item.instance->pieces().size();
        rates.assign(count * 3, 0);
        if (item.script != nullptr)
            for (uint32_t piece = 0; piece < count; ++piece)
                for (uint32_t axis = 0; axis < 3; ++axis)
                    rates[piece * 3 + axis] = item.script->piece_turn_speed(piece, 2 - axis);
        app::observe_unit(*item.motion, tick, item.generation, *item.unit, *item.instance, rates);
        ++totals.units;
        totals.pieces += count;
        totals.triplets += item.script != nullptr ? count : 0;
    }
    return totals;
}

bool same_totals(const app::UnitCaptureTotals& a, const app::UnitCaptureTotals& b) {
    return a.units == b.units && a.pieces == b.pieces && a.triplets == b.triplets;
}

/// Changes only the supported denormal controls, leaving all other settings alone.
/// @param enabled whether the supported units flush denormal values to zero
void set_flush_to_zero(bool enabled) {
    auto control = fp::save_float_control();
#if defined(__SSE__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 1)
    constexpr uint32_t flush = 0x8000U;
    control.vector_unit = (control.vector_unit & ~flush) | (enabled ? flush : 0);
#if defined(__x86_64__) || defined(_M_X64)
    // Every x86-64 processor supports DAZ; some 32-bit SSE processors do not.
    constexpr uint32_t denormals_are_zero = 0x0040U;
    control.vector_unit =
        (control.vector_unit & ~denormals_are_zero) | (enabled ? denormals_are_zero : 0);
#endif
#endif
#if defined(__aarch64__) || defined(_M_ARM64) || (defined(__arm__) && defined(__ARM_FP))
    constexpr uint64_t flush = 0x01000000U;
    control.arm_unit = (control.arm_unit & ~flush) | (enabled ? flush : 0);
#endif
    fp::restore_float_control(control);
}

void test_parallel_history() {
    constexpr std::size_t count = 1025; // Four bands and a remainder at the pool's threshold.
    constexpr uint32_t identity = 719;
    const std::array programs{make_program(2), make_program(5)};
    const std::array models{make_model(3), make_model(5)};
    std::vector<std::unique_ptr<Actor>> actors;
    actors.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto actor = std::make_unique<Actor>();
        actor->model = oa::sim::model_runtime::make_instance(models[index % 2]);
        if (index % 11 != 0) {
            const auto& program = programs[index % 3 == 0 ? 1 : 0];
            actor->script = std::make_unique<runtime::ScriptInstance>(
                program.source, program.shared, actor->host, 1000
            );
            auto state = actor->script->vm().export_state(identity);
            CHECK(state.ok());
            if (!state.ok())
                return;
            for (std::size_t piece = 0; piece < state.state->pieces.size(); ++piece)
                for (std::size_t axis = 0; axis < 3; ++axis)
                    state.state->pieces[piece].axes[axis].turn_speed_per_tick =
                        static_cast<int32_t>((index + 1) * (piece + 3) * (axis + 5)) *
                        (axis % 2 ? -1 : 1);
            CHECK(!actor->script->vm().import_state(*state.state));
        }
        actors.push_back(std::move(actor));
    }
    jobs::Pool one(1), two(2), four(4);
    const std::array<jobs::Pool*, 4> pools{nullptr, &one, &two, &four};
    CHECK(two.threads() == 2 && four.threads() == 4);
    std::array<app::UnitCaptureScratch, 4> scratch;
    std::array<std::vector<app::UnitCaptureItem>, 5> batches;
    for (auto& batch : batches)
        batch.reserve(count);

    struct Step {
        uint32_t tick, generation;
    };

    const std::array<Step, 15> steps{
        {{40, 1},
         {41, 1},
         {41, 1},
         {42, 1},
         {47, 1},
         {53, 1},
         {53, 2},
         {54, 2},
         {55, 2},
         {56, 2},
         {UINT32_MAX, 2},
         {0, 2},
         {1, 2},
         {2, 2},
         {3, 2}}
    };
    const auto original_control = fp::save_float_control();
    for (std::size_t step = 0; step < steps.size(); ++step) {
        const auto [tick, generation] = steps[step];
        for (auto& batch : batches)
            batch.clear();
        for (std::size_t index = 0; index < count; ++index) {
            auto& actor = *actors[index];
            if (step == 2 && actor.script) {
                std::array<int32_t, 4> arguments{};
                CHECK(actor.script->piece_turn_speed(0, 1) != 0);
                CHECK(actor.script->query("Stop", arguments));
                CHECK(actor.script->piece_turn_speed(0, 1) == 0);
            }
            if (step == 12)
                actor.model = oa::sim::model_runtime::make_instance(models[(index + 1) % 2]);
            else if (step == 13)
                actor.model = oa::sim::model_runtime::make_instance(nullptr);
            else if (step == 14)
                actor.model = oa::sim::model_runtime::make_instance(models[index % 2]);
            // The query and its following tick change only the rates. A later
            // attachment change also moves a piece, separating the two flags.
            const auto pose_step = step == 2 || step == 3 ? 1 : step;
            actor.record.position = {
                static_cast<int32_t>((index + pose_step) * 1000),
                static_cast<int32_t>(index * 3),
                static_cast<int32_t>((index + pose_step) * 700)
            };
            actor.record.heading = static_cast<uint16_t>(index * 79 + pose_step * 103);
            actor.record.attach_parent = step == 7 ? 23 : 0;
            if (step == 9)
                actor.record.position.x += 65 * 0x10000;
            for (std::size_t piece = 0; piece < actor.model.pieces().size(); ++piece) {
                auto& state = actor.model.pieces()[piece];
                const auto value = static_cast<int32_t>(index * 17 + piece * 23 + pose_step * 31);
                state.translation = {value, -value, value * 2};
                state.rotation = {
                    static_cast<int16_t>(value * 3),
                    static_cast<int16_t>(-value * 5),
                    static_cast<int16_t>(value * 7)
                };
            }
            for (std::size_t version = 0; version < batches.size(); ++version)
                batches[version].push_back(
                    {&actor.histories[version],
                     &actor.record,
                     &actor.model,
                     actor.script.get(),
                     generation}
                );
            if (step == 6)
                for (auto& history : actor.histories) {
                    history.blended_draw = 44;
                    history.state.cache.draws = 7;
                    history.state.image_builds = 9;
                }
        }
        std::vector<oa::sim::script_state::State> before;
        std::vector<uint64_t> host_calls;
        for (const auto& actor : actors) {
            if (actor->script)
                before.push_back(*actor->script->vm().export_state(identity).state);
            host_calls.push_back(actor->host.calls.load());
        }
        CHECK(std::fesetround(step % 2 ? FE_DOWNWARD : FE_UPWARD) == 0);
        set_flush_to_zero(step % 2 != 0);
        const auto caller_control = fp::save_float_control();
        const auto expected = scalar_capture(batches[0], tick);
        for (std::size_t version = 0; version < pools.size(); ++version) {
            // Clearing the bands exposes the cold-generation serial fallback.
            if (step == 6)
                scratch[version].bands.clear();
            const auto actual = app::capture_unit_poses(
                batches[version + 1], tick, scratch[version], pools[version]
            );
            CHECK(same_totals(expected, actual));
            CHECK(fp::float_control_matches(caller_control));
            if (step == 3 && version >= 2)
                CHECK(scratch[version].bands.size() == pools[version]->threads());
            if (step == 6)
                CHECK(scratch[version].bands.empty());
        }
        std::size_t saved = 0;
        for (std::size_t index = 0; index < actors.size(); ++index) {
            auto& actor = *actors[index];
            CHECK(actor.host.calls.load() == host_calls[index]);
            for (std::size_t version = 1; version < actor.histories.size(); ++version)
                CHECK(same_history(actor.histories[0], actor.histories[version]));
            const auto& history = actor.histories[0];
            if (step == 2 && actor.script)
                CHECK(history.current.pieces[0].turn_per_tick[1] != 0);
            if (step == 3) {
                CHECK(history.continued && !history.moved && !history.pieces_moved);
                if (actor.script)
                    CHECK(history.current.pieces[0].turn_per_tick[1] == 0);
            }
            if (step == 7)
                CHECK(!history.continued && !history.moved && history.pieces_moved);
            if (step == 6)
                CHECK(
                    history.blended_draw == 0 && history.state.cache.draws == 0 &&
                    history.state.image_builds == 0
                );
            if (actor.script)
                CHECK(before[saved++] == *actor.script->vm().export_state(identity).state);
        }
    }
    // At least two outer bands make the pool busy. Only one calls the
    // capture helper; its nested bands run synchronously on that thread.
    const auto expected_nested = scalar_capture(batches[0], 4);
    app::UnitCaptureTotals actual_nested;
    bool entry_control_preserved = false;
    const auto nested_caller_control = fp::save_float_control();
    jobs::run_bands(&four, 2, [&](uint32_t band) {
        if (band != 0)
            return;
        const auto original_entry_control = fp::save_float_control();
        set_flush_to_zero(true);
        const auto entry_control = fp::save_float_control();
        actual_nested = app::capture_unit_poses(batches[4], 4, scratch[3], &four);
        entry_control_preserved = fp::float_control_matches(entry_control);
        fp::restore_float_control(original_entry_control);
    });
    CHECK(same_totals(expected_nested, actual_nested));
    CHECK(entry_control_preserved && fp::float_control_matches(nested_caller_control));
    for (const auto& actor : actors)
        CHECK(same_history(actor->histories[0], actor->histories[4]));
    fp::restore_float_control(original_control);
    CHECK(fp::float_control_matches(original_control));
    app::UnitCaptureScratch empty_scratch;
    CHECK(same_totals({}, app::capture_unit_poses({}, 1, empty_scratch, &four)));
}

/// The engine hack follows exactly the opted-in preset and existing global policy.
void test_capture_policy() {
    oa::data::match_rules::MatchRules rules;
    for (const bool enabled : {false, true})
        for (int32_t version = 0; version <= 12; ++version) {
            oa::data::enhanced_mode::apply_alias(
                rules.setup.enhanced_mode, version > 0 ? static_cast<uint32_t>(version) : 0U
            );
            rules.setup.enhanced_mode.enabled = enabled && rules.setup.enhanced_mode.enabled;
            CHECK(app::unit_pose_capture(rules, false) == app::UnitPoseCapture::full);
            const auto expected =
                enabled && (version == 7 || version == 8 || version == 10 || version == 11)
                    ? app::UnitPoseCapture::root
                    : app::UnitPoseCapture::full;
            CHECK(app::unit_pose_capture(rules, true) == expected);
        }
}

/// Root capture does not touch scripts; a same-tick model request primes the actual rig.
void test_root_capture_and_query_transition() {
    constexpr uint32_t identity = 719;
    Host host;
    const auto program = make_program(2);
    runtime::ScriptInstance script(program.source, program.shared, host, 1000);
    auto saved = script.vm().export_state(identity);
    CHECK(saved.ok());
    if (!saved.ok())
        return;
    for (auto& piece : saved.state->pieces) {
        piece.axes[0].turn_speed_per_tick = 21;
        piece.axes[1].turn_speed_per_tick = -11;
        piece.axes[2].turn_speed_per_tick = 31;
    }
    CHECK(!script.vm().import_state(*saved.state));
    auto instance = oa::sim::model_runtime::make_instance(make_model(3));
    oa::Unit unit{};
    unit.position = {100 * 65536, 0, 100 * 65536};
    app::UnitMotion motion;
    app::UnitCaptureScratch scratch;
    app::UnitCaptureItem item{&motion, &unit, &instance, &script, 1};
    const std::span<const app::UnitCaptureItem> items(&item, 1);
    const auto before = *script.vm().export_state(identity).state;
    const auto host_calls = host.calls.load();
    auto total = app::capture_unit_poses(items, 10, scratch, nullptr, app::UnitPoseCapture::root);
    CHECK(same_totals(total, {1, 0, 0}));
    CHECK(!motion.rig_valid && !motion.rig_primed);
    CHECK(motion.current.pieces.empty() && motion.previous.pieces.empty());
    CHECK(scratch.serial_turns.empty() && scratch.bands.empty());
    unit.position.x += 4 * 65536;
    unit.heading = 1200;
    total = app::capture_unit_poses(items, 11, scratch, nullptr, app::UnitPoseCapture::root);
    CHECK(same_totals(total, {1, 0, 0}));
    CHECK(motion.continued && motion.moved && !motion.pieces_moved);
    CHECK(host.calls.load() == host_calls);
    CHECK(before == *script.vm().export_state(identity).state);
    const auto previous_root = motion.previous;
    const auto current_root = motion.current;
    // A real query changes a rate without advancing the simulation tick.
    std::array<int32_t, 4> arguments{};
    CHECK(script.query("Stop", arguments));
    CHECK(script.piece_turn_speed(0, 1) == 0);
    instance.pieces()[0].rotation.xy = 1234;
    instance.pieces()[0].translation.y = 456;
    const auto after_query = *script.vm().export_state(identity).state;
    const auto query_host_calls = host.calls.load();
    total = app::capture_unit_poses(items, 11, scratch);
    CHECK(same_totals(total, {0, 3, 3}));
    CHECK(motion.tick == 11 && motion.continued && motion.moved);
    CHECK(motion.rig_valid && motion.rig_primed && !motion.pieces_moved);
    CHECK(same_pose(previous_root, motion.previous));
    CHECK(same_point(current_root.position, motion.current.position));
    CHECK(current_root.heading == motion.current.heading);
    CHECK(motion.current.pieces[0].rotation.xy == 1234);
    CHECK(motion.current.pieces[0].translation.y == 456);
    CHECK(motion.current.pieces[0].turn_per_tick[0] == 31);
    CHECK(motion.current.pieces[0].turn_per_tick[1] == 0);
    CHECK(motion.current.pieces[0].turn_per_tick[2] == 21);
    CHECK(motion.current.pieces[2].turn_per_tick[0] == 0);
    CHECK(host.calls.load() == query_host_calls);
    CHECK(after_query == *script.vm().export_state(identity).state);
    // A paused, same-tick redraw keeps the prime rather than reading later
    // query changes or fabricating a previous spin endpoint.
    instance.pieces()[0].rotation.xy = 2345;
    total = app::capture_unit_poses(items, 11, scratch);
    CHECK(same_totals(total, {}));
    auto drawn = oa::sim::model_runtime::make_instance(make_model(3));
    auto drawn_unit = unit;
    app::blend_unit_root_pose(motion.previous, motion.current, app::whole_tick / 2, drawn_unit);
    app::place_current_unit_pieces(motion.current, drawn);
    CHECK(drawn_unit.position.x == 102 * 65536);
    CHECK(drawn.pieces()[0].rotation.xy == 1234);
    total = app::capture_unit_poses(items, 12, scratch);
    CHECK(same_totals(total, {1, 3, 3}));
    CHECK(motion.rig_valid && !motion.rig_primed);
    CHECK(motion.previous.pieces[0].rotation.xy == 1234);
    CHECK(motion.current.pieces[0].rotation.xy == 2345);
}

/// Warm Dots histories use workers without allocating rig buffers or rate scratch.
void test_parallel_roots_without_rig_storage() {
    constexpr std::size_t count = 1025;
    const auto model = make_model(3);
    std::vector<std::unique_ptr<Actor>> actors;
    actors.reserve(count);
    std::array<std::vector<app::UnitCaptureItem>, 2> batches;
    for (auto& batch : batches)
        batch.reserve(count);
    for (std::size_t index = 0; index < count; ++index) {
        auto actor = std::make_unique<Actor>();
        actor->model = oa::sim::model_runtime::make_instance(model);
        actor->record.position = {static_cast<int32_t>(index * 1200), 0, 4000};
        for (std::size_t version = 0; version < batches.size(); ++version)
            batches[version].push_back(
                {&actor->histories[version], &actor->record, &actor->model, nullptr, 1}
            );
        actors.push_back(std::move(actor));
    }
    jobs::Pool pool(4);
    std::array<app::UnitCaptureScratch, 2> scratch;
    for (const uint32_t tick : {1U, 2U}) {
        for (auto& actor : actors)
            actor->record.position.z += 700;
        const auto serial = app::capture_unit_poses(
            batches[0], tick, scratch[0], nullptr, app::UnitPoseCapture::root
        );
        const auto parallel = app::capture_unit_poses(
            batches[1], tick, scratch[1], &pool, app::UnitPoseCapture::root
        );
        CHECK(same_totals(serial, {count, 0, 0}));
        CHECK(same_totals(serial, parallel));
        for (const auto& actor : actors) {
            CHECK(same_history(actor->histories[0], actor->histories[1]));
            for (std::size_t version = 0; version < 2; ++version) {
                const auto& motion = actor->histories[version];
                CHECK(!motion.rig_valid && motion.root_piece_count == 3);
                CHECK(
                    motion.current.pieces.capacity() == 0 && motion.previous.pieces.capacity() == 0
                );
            }
        }
    }
    CHECK(scratch[0].serial_turns.capacity() == 0);
    CHECK(scratch[1].bands.size() == 4);
    for (const auto& band : scratch[1].bands)
        CHECK(band.turns.capacity() == 0);
    // Immediate model transition captures every rig, while root counters
    // remain zero because this simulation tick already has its endpoint.
    const auto prime = app::capture_unit_poses(batches[1], 2, scratch[1], &pool);
    CHECK(same_totals(prime, {0, count * 3, 0}));
    for (const auto& actor : actors)
        CHECK(actor->histories[1].rig_valid && actor->histories[1].rig_primed);
    const auto next = app::capture_unit_poses(batches[1], 3, scratch[1], &pool);
    CHECK(same_totals(next, {count, count * 3, 0}));
    for (const auto& actor : actors)
        CHECK(actor->histories[1].rig_valid && !actor->histories[1].rig_primed);
}

} // namespace

int main() {
    test_parallel_history();
    test_capture_policy();
    test_root_capture_and_query_transition();
    test_parallel_roots_without_rig_storage();
    return failures == 0 ? 0 : 1;
}
