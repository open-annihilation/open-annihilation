// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Characterisation of Vm::tick_context, which wakes and runs one script
// context on its own: an index past the pool, stopped and sleeping
// contexts, running one context while another waits, the instruction budget,
// and a seeded schedule of single-context ticks whose digest pins the
// contexts and statics the VM leaves today.

#include "oa/sim/script_vm.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <vector>

namespace {

namespace vm = oa::sim::script_vm;
namespace op = oa::sim::script_vm::opcode;

int failures = 0;

/// Records a failed check.
///
/// @param line source line of the check
/// @param what text of the failed condition
void report_failure(int line, const char* what) {
    std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, line, what);
    ++failures;
}

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            report_failure(__LINE__, #condition);                                                  \
        }                                                                                          \
    } while (false)

// SLEEP waits time_scale * ms / 1000 ticks: 2000 ms at scale 10 is 20 ticks.
constexpr int32_t time_scale = 10;
constexpr uint32_t sleep_ms = 2000;
constexpr int32_t sleep_ticks = 20;

// Script entry points in the program below.
constexpr uint32_t store_eleven = 0;
constexpr uint32_t store_twenty_two = 1;
constexpr uint32_t sleep_then_store = 2;
constexpr uint32_t sum_three = 3;
constexpr std::size_t static_count = 4;

// 32-bit FNV-1a parameters.
constexpr uint32_t fnv_offset_basis = 0x811C9DC5u;
constexpr uint32_t fnv_prime = 0x01000193u;
// Marsaglia's xorshift32 shift triple.
constexpr uint32_t xorshift_left_first = 13;
constexpr uint32_t xorshift_right = 17;
constexpr uint32_t xorshift_left_second = 5;
constexpr uint32_t seed_schedule = 0x082EFA98u;
constexpr int32_t sweep_steps = 600;
constexpr uint32_t sweep_digest = 0xAB8C2D17u;

/// Builds a VM over four scripts: two single stores, a sleep then a store, and a three-term sum.
///
/// @return the VM, every context stopped
vm::Vm make_vm() {
    std::vector<uint32_t> code = {
        // 0: static 0 = 11.
        op::push_constant,
        11,
        op::pop_static,
        0,
        op::return_,
        // 5: static 1 = 22.
        op::push_constant,
        22,
        op::pop_static,
        1,
        op::return_,
        // 10: sleep, then static 2 = 33.
        op::push_constant,
        sleep_ms,
        op::sleep,
        op::push_constant,
        33,
        op::pop_static,
        2,
        op::return_,
        // 18: static 3 = 1 + 2 + 3.
        op::push_constant,
        1,
        op::push_constant,
        2,
        op::add,
        op::push_constant,
        3,
        op::add,
        op::pop_static,
        3,
        op::return_,
    };
    std::vector<uint32_t> entries = {0, 5, 10, 18};

    struct HeldWords {
        std::vector<uint32_t> code;
        std::vector<uint32_t> entry_points;
    };

    const auto held = std::make_shared<HeldWords>();
    held->code = std::move(code);
    held->entry_points = std::move(entries);
    vm::Program program;
    program.code = held->code;
    program.entry_points = held->entry_points;
    program.static_count = static_count;
    program.owner = held;
    return vm::Vm(std::move(program), time_scale);
}

/// Checks the index bound and a stopped context.
void test_idle_contexts() {
    auto machine = make_vm();
    const auto outside = machine.tick_context(vm::context_count, 0);
    CHECK(!outside.ok() && outside.error->code == vm::ErrorCode::invalid_state);
    CHECK(outside.instructions == 0);

    // A stopped context runs nothing, even with no budget.
    const auto stopped = machine.tick_context(0, 0, 0);
    CHECK(stopped.ok() && stopped.instructions == 0);
}

/// Checks that one context runs while the others keep waiting.
void test_single_context() {
    auto machine = make_vm();
    const auto first = machine.start(store_eleven);
    const auto second = machine.start(store_twenty_two);
    CHECK(first.ok() && second.ok() && first.context == 0 && second.context == 1);

    const auto run = machine.tick_context(second.context, 0);
    CHECK(run.ok() && run.instructions == 3);
    CHECK(machine.static_value(1) == 22 && machine.static_value(0) == 0);
    CHECK(machine.context(first.context).state == vm::ContextState::active);
    CHECK(machine.context(second.context).state == vm::ContextState::stopped);
    CHECK(machine.active_count() == 1);

    const auto rest = machine.tick_context(first.context, 0);
    CHECK(rest.ok() && rest.instructions == 3 && machine.static_value(0) == 11);
    CHECK(machine.active_count() == 0);
}

/// Checks that a sleeping context counts down by the elapsed ticks and resumes at zero.
void test_sleep() {
    auto machine = make_vm();
    const auto started = machine.start(sleep_then_store);
    const auto slept = machine.tick_context(started.context, 0);
    CHECK(slept.ok() && slept.instructions == 2);
    CHECK(machine.context(started.context).state == vm::ContextState::sleeping);
    CHECK(machine.context(started.context).sleep_remaining == sleep_ticks);

    const auto counting = machine.tick_context(started.context, 5);
    CHECK(counting.ok() && counting.instructions == 0);
    CHECK(machine.context(started.context).sleep_remaining == sleep_ticks - 5);

    // More elapsed ticks than remain: the count goes negative and the script resumes.
    const auto resumed = machine.tick_context(started.context, 40);
    CHECK(resumed.ok() && resumed.instructions == 3);
    CHECK(machine.context(started.context).sleep_remaining == sleep_ticks - 5 - 40);
    CHECK(machine.static_value(2) == 33 && machine.active_count() == 0);
}

/// Checks the instruction budget of a single-context tick.
void test_budget() {
    auto machine = make_vm();
    const auto started = machine.start(sum_three);
    // No budget stops an active context before its first instruction.
    const auto none = machine.tick_context(started.context, 0, 0);
    CHECK(
        !none.ok() && none.error->code == vm::ErrorCode::instruction_limit && none.instructions == 0
    );

    const auto partial = machine.tick_context(started.context, 0, 3);
    CHECK(!partial.ok() && partial.error->code == vm::ErrorCode::instruction_limit);
    CHECK(partial.instructions == 3 && partial.error->pc == 23);
    CHECK(machine.context(started.context).state == vm::ContextState::active);

    // The next tick continues where the budget ran out.
    const auto finished = machine.tick_context(started.context, 0);
    CHECK(finished.ok() && finished.instructions == 4 && machine.static_value(3) == 6);
}

/// Advances an xorshift32 generator.
///
/// @param[in,out] state generator state, never 0
/// @return the next value
uint32_t next(uint32_t& state) {
    state ^= state << xorshift_left_first;
    state ^= state >> xorshift_right;
    state ^= state << xorshift_left_second;
    return state;
}

/// Extends an FNV-1a digest over a 32-bit value, low byte first.
///
/// @param digest running digest
/// @param value value to add
/// @return the extended digest
uint32_t digest_value(uint32_t digest, uint32_t value) {
    for (uint32_t shift = 0; shift < 32; shift += 8) {
        digest = (digest ^ ((value >> shift) & 0xFFu)) * fnv_prime;
    }
    return digest;
}

/// Starts, sleeps and ticks seeded contexts one at a time and pins the digest of what they leave.
void test_schedule() {
    uint32_t state = seed_schedule;
    uint32_t digest = fnv_offset_basis;
    auto machine = make_vm();
    for (int32_t step = 0; step < sweep_steps; ++step) {
        if (next(state) % 3 == 0) {
            const auto started = machine.start(next(state) % 4);
            digest = digest_value(digest, static_cast<uint32_t>(started.context));
        }
        const auto index = static_cast<std::size_t>(next(state) % vm::context_count);
        // Drawn one per statement: a call's arguments are evaluated in an unspecified order.
        const auto elapsed = next(state) % 8;
        const auto instruction_budget = 1 + next(state) % 6;
        const auto ran = machine.tick_context(index, elapsed, instruction_budget);
        digest = digest_value(digest, static_cast<uint32_t>(ran.instructions));
        digest = digest_value(digest, ran.error ? static_cast<uint32_t>(ran.error->code) : 0u);
        const auto context = machine.context(index);
        digest = digest_value(digest, static_cast<uint32_t>(context.state));
        digest = digest_value(digest, context.pc);
        digest = digest_value(digest, static_cast<uint32_t>(context.stack_pointer));
        digest = digest_value(digest, static_cast<uint32_t>(context.sleep_remaining));
    }
    for (std::size_t i = 0; i < static_count; ++i) {
        digest = digest_value(digest, static_cast<uint32_t>(machine.static_value(i).value_or(-1)));
    }
    digest = digest_value(digest, static_cast<uint32_t>(machine.active_count()));
    if (digest != sweep_digest) {
        std::fprintf(
            stderr,
            "%s:%d: schedule digest is 0x%08X, expected 0x%08X\n",
            __FILE__,
            __LINE__,
            digest,
            sweep_digest
        );
        ++failures;
    }
}

} // namespace

int main() {
    test_idle_contexts();
    test_single_context();
    test_sleep();
    test_budget();
    test_schedule();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}
