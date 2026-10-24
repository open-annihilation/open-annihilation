// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The shadow's call log: the logging host over the computer-player fake,
// the recording squad view, stream re-alignment, and the comparison.

#include "oa/sim/ai.hpp"
#include "oa/sim/brains/seat_calls.hpp"
#include "oa/sim/brains/shadow.hpp"
#include "oa/sim/match_runtime/script.hpp"
#include "oa/test/computer_fake.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

using namespace oa::sim::ai;
using namespace oa::test::computer_fake;

namespace {

namespace brains = oa::sim::brains;

int failures = 0;

void check(bool ok, const char* what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void check_at(bool ok, const char* what, uint32_t tick) {
    if (ok)
        return;
    std::fprintf(stderr, "FAIL: %s at tick %u\n", what, tick);
    ++failures;
}

struct ProbedAssign {
    uint16_t unit{};
    int squad{};
};

std::vector<ProbedAssign> probed_assigns;

void probe_set_squad(void* context, uint16_t unit, Squad squad) {
    const int squad_index = static_cast<int>(static_cast<uint32_t>(squad));
    probed_assigns.push_back({unit, squad_index});
    fake_of(context)->set_squad(unit, squad_index);
}

bool note_attack_point(void* context, uint16_t unit, const oa::FixedVec3* at) {
    oa::FixedVec3 point{};
    if (at != nullptr)
        point = *at;
    fake_of(context)->orders.push_back({"attack_point", unit, 0, point, false});
    return true;
}

void put_name(char* out, const char* text) {
    std::snprintf(out, 16, "%s", text != nullptr ? text : "");
}

bool same_text(const char* left, const char* right) {
    if (left == nullptr || right == nullptr)
        return left == right;
    return std::strcmp(left, right) == 0;
}

bool same_point(const oa::FixedVec3& left, int32_t x, int32_t y, int32_t z) {
    return left.x == x && left.y == y && left.z == z;
}

struct DueTask {
    const char* name{};
    uint32_t next{};
    uint32_t gap{};
    uint32_t bound{};
};

// The section markers test_tick's units produce: sort every 30 ticks, then
// each task whose next tick has arrived. Air and siege add one roll of 7.
std::vector<std::pair<uint32_t, const char*>> expected_markers() {
    DueTask tasks[] = {
        {"task1", 0, 30, 0},
        {"task2", 0, 300, 0},
        {"task3", 0, 150, 0},
        {"task4", 0, 90, 0},
        {"task6", 0, 300, 0},
        {"task7", 0, 150, 0},
        {"task8", 0, 30, 900},
        {"task9", 0, 30, 150},
    };
    std::vector<std::pair<uint32_t, const char*>> markers;
    for (uint32_t tick = 1; tick <= 120; ++tick) {
        if (tick % 30 == 0)
            markers.push_back({tick, "sort"});
        for (DueTask& task : tasks) {
            if (task.next > tick)
                continue;
            markers.push_back({tick, task.name});
            uint32_t extra = 0;
            if (task.bound != 0)
                extra = 7u % task.bound;
            task.next = tick + task.gap + extra;
        }
    }
    return markers;
}

struct SeenMarker {
    uint32_t tick{};
    char name[16]{};
};

// (a) The logging host records the orders, assigns and draws the fake saw.
void test_logging_host() {
    probed_assigns.clear();
    Fake fake;
    fake.player(0, OA_PLAYER_STATUS_LOCAL, 1, 9);
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 10, 40);
    auto host = host_for(fake);
    host.order_attack_point = note_attack_point;
    ComputerPlayers state{};
    describe_catalog(state);
    check(computer_players_configure(&state, "", kBuildLists), "configure");
    fake.spawn(1, 0, ARMLLT, 900, 900, OA_UNIT_FLAG_BUILDING | OA_UNIT_FLAG_HAS_WEAPONS);
    fake.spawn(10, 1, ARMCOM, 200, 200, 0);
    fake.spawn(11, 1, ARMLAB, 240, 200, OA_UNIT_FLAG_BUILDING);
    for (uint32_t slot = 12; slot < 16; ++slot)
        fake.spawn(slot, 1, ARMPW, 300 + static_cast<int32_t>(slot), 300, OA_UNIT_FLAG_HAS_WEAPONS);
    fake.rolls.assign(4096, 7);
    check(computer_players_initialize(&state, host), "initialize");
    const std::size_t roll_after_setup = fake.roll;
    host.set_squad = probe_set_squad;

    brains::CallLog log;
    brains::LoggingHost session;
    brains::logging_arm(session, host, log, 1, nullptr, nullptr);

    std::vector<SeenMarker> seen;
    for (uint32_t tick = 1; tick <= 120; ++tick) {
        fake.world.game.tick = tick;
        const uint32_t before = log.count;
        computer_player_tick_orders(&state, session.host, 1);
        brains::logging_note_stances(session);
        uint32_t added_stances = 0;
        for (uint32_t index = before; index < log.count; ++index) {
            const brains::LogEntry& entry = log.items[index];
            if (entry.kind == brains::LogKind::section) {
                SeenMarker marker;
                marker.tick = tick;
                put_name(marker.name, entry.name);
                seen.push_back(marker);
            } else if (entry.kind == brains::LogKind::stance) {
                ++added_stances;
            }
        }
        if (tick % 30 == 0) {
            check_at(added_stances == 6, "sort stance lines", tick);
        } else {
            check_at(added_stances == 0, "stance lines only after sort", tick);
        }
    }

    const auto expected = expected_markers();
    check(seen.size() == expected.size(), "section marker count");
    const std::size_t marker_count = seen.size() < expected.size() ? seen.size() : expected.size();
    for (std::size_t index = 0; index < marker_count; ++index) {
        check_at(seen[index].tick == expected[index].first, "section tick", expected[index].first);
        check_at(
            same_text(seen[index].name, expected[index].second), "section name", seen[index].tick
        );
    }
    for (const SeenMarker& marker : seen)
        check(!same_text(marker.name, "task5"), "no task5 marker");

    std::vector<ProbedAssign> logged_assigns;
    std::vector<Order> logged_orders;
    std::size_t replay = roll_after_setup;
    for (uint32_t index = 0; index < log.count; ++index) {
        const brains::LogEntry& entry = log.items[index];
        if (entry.kind == brains::LogKind::draw) {
            uint32_t expect = 0;
            if (entry.bound >= 2) {
                const uint32_t roll = replay < fake.rolls.size() ? fake.rolls[replay] : 0u;
                expect = roll % entry.bound;
                ++replay;
            }
            check(entry.value == expect, "logged draw");
            continue;
        }
        if (entry.kind == brains::LogKind::assign) {
            logged_assigns.push_back({entry.slot, static_cast<int>(entry.extra)});
            continue;
        }
        if (entry.kind == brains::LogKind::section || entry.kind == brains::LogKind::stance ||
            entry.kind == brains::LogKind::resync)
            continue;
        Order order;
        order.unit = entry.slot;
        order.at = {entry.x, entry.y, entry.z};
        if (entry.kind == brains::LogKind::move) {
            order.kind = "move";
            order.queue = entry.flag != 0;
        } else if (entry.kind == brains::LogKind::patrol) {
            order.kind = "patrol";
            order.queue = entry.flag != 0;
        } else if (entry.kind == brains::LogKind::attack) {
            order.kind = "attack";
            order.arg = entry.extra;
        } else if (entry.kind == brains::LogKind::build) {
            order.kind = "build";
            order.arg = entry.extra;
        } else if (entry.kind == brains::LogKind::queue_order) {
            order.kind = "factory";
            order.arg = entry.extra;
        } else if (entry.kind == brains::LogKind::on) {
            order.kind = entry.flag != 0 ? "on" : "off";
        } else if (entry.kind == brains::LogKind::attack_point) {
            order.kind = "attack_point";
        } else {
            check(false, "logged line is an order");
            continue;
        }
        logged_orders.push_back(order);
    }
    check(replay == fake.roll, "draws cover the fake's rolls");
    check(logged_assigns.size() == probed_assigns.size(), "assign count");
    const std::size_t assign_count = logged_assigns.size() < probed_assigns.size()
                                         ? logged_assigns.size()
                                         : probed_assigns.size();
    for (std::size_t index = 0; index < assign_count; ++index) {
        check(
            logged_assigns[index].unit == probed_assigns[index].unit &&
                logged_assigns[index].squad == probed_assigns[index].squad,
            "assign"
        );
    }
    check(logged_orders.size() == fake.orders.size(), "order count");
    const std::size_t order_count =
        logged_orders.size() < fake.orders.size() ? logged_orders.size() : fake.orders.size();
    for (std::size_t index = 0; index < order_count; ++index) {
        const Order& logged = logged_orders[index];
        const Order& saw = fake.orders[index];
        check(
            logged.kind == saw.kind && logged.unit == saw.unit && logged.arg == saw.arg &&
                logged.queue == saw.queue && same_point(logged.at, saw.at.x, saw.at.y, saw.at.z),
            "order"
        );
    }
    computer_players_release(&state);
}

std::vector<uint16_t> members_of(brains::Recording& recorded, uint32_t squad) {
    std::vector<uint16_t> members;
    const auto squad_id = static_cast<Squad>(squad);
    const uint32_t size = recorded.calls.host.squad_size(recorded.calls.host.context, 1, squad_id);
    for (uint32_t index = 0; index < size; ++index)
        members.push_back(
            recorded.calls.host.squad_member(recorded.calls.host.context, 1, squad_id, index)
        );
    return members;
}

void expect_members(
    brains::Recording& recorded, uint32_t squad, const std::vector<uint16_t>& want
) {
    const std::vector<uint16_t> got = members_of(recorded, squad);
    check(got == want, "squad members");
}

void push_section(brains::CallLog& log, const char* name, uint32_t state) {
    brains::LogEntry entry{};
    entry.kind = brains::LogKind::section;
    put_name(entry.name, name);
    entry.state_before = state;
    check(brains::log_push(log, entry), "push section");
}

void push_assign(brains::CallLog& log, uint16_t slot, uint16_t squad) {
    brains::LogEntry entry{};
    entry.kind = brains::LogKind::assign;
    entry.slot = slot;
    entry.extra = squad;
    check(brains::log_push(log, entry), "push assign");
}

void push_draw(
    brains::CallLog& log, uint32_t bound, uint32_t value, uint32_t before, uint32_t after
) {
    brains::LogEntry entry{};
    entry.kind = brains::LogKind::draw;
    entry.bound = bound;
    entry.value = value;
    entry.state_before = before;
    entry.state_after = after;
    check(brains::log_push(log, entry), "push draw");
}

brains::SeatCalls live_calls(Fake& fake) {
    return brains::seat_calls_from_computer_host(host_for(fake));
}

// (b) Assigns follow the match's swap-and-push, and a section restores the
// earlier view. A unit ordered this tick reads the live head.
void test_recording_view() {
    Fake fake;
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 1, 9);
    for (uint16_t slot = 1; slot <= 4; ++slot)
        fake.spawn(slot, 1, ARMCOM, 100 + static_cast<int32_t>(slot), 100, 0);
    fake.set_squad(1, 1);
    fake.set_squad(2, 1);
    fake.set_squad(3, 1);
    brains::Recording recorded;
    const brains::SeatCalls live = live_calls(fake);
    brains::recording_bind(recorded, live, 1);
    brains::CallLog brain;
    brains::recording_begin(recorded, brain, nullptr, 1);
    recorded.calls.host.set_squad(recorded.calls.host.context, 4, static_cast<Squad>(1));
    expect_members(recorded, 1, {1, 2, 3, 4});
    recorded.calls.host.set_squad(recorded.calls.host.context, 1, static_cast<Squad>(2));
    expect_members(recorded, 1, {4, 2, 3});
    expect_members(recorded, 2, {1});
    recorded.calls.host.set_squad(recorded.calls.host.context, 2, static_cast<Squad>(1));
    expect_members(recorded, 1, {4, 3, 2});
    expect_members(recorded, 2, {1});
    check(fake.orders.empty(), "an assign leaves the fake's orders");

    Fake reset_fake;
    reset_fake.player(1, OA_PLAYER_STATUS_COMPUTER, 1, 9);
    for (uint16_t slot = 1; slot <= 4; ++slot)
        reset_fake.spawn(slot, 1, ARMCOM, 100, 100, 0);
    reset_fake.set_squad(1, 1);
    reset_fake.set_squad(2, 1);
    brains::CallLog builtin;
    push_section(builtin, "task1", 1);
    push_assign(builtin, 3, 1);
    push_section(builtin, "task2", 1);
    push_assign(builtin, 4, 1);
    brains::Recording reset;
    const brains::SeatCalls reset_live = live_calls(reset_fake);
    brains::recording_bind(reset, reset_live, 1);
    brains::CallLog reset_brain;
    brains::recording_begin(reset, reset_brain, &builtin, 1);
    reset.calls.section(&reset.calls.host, "task2");
    expect_members(reset, 1, {1, 2, 3});
    expect_members(reset, 2, {});

    Fake head_fake;
    head_fake.player(1, OA_PLAYER_STATUS_COMPUTER, 1, 9);
    head_fake.spawn(5, 1, ARMPW, 200, 200, OA_UNIT_FLAG_HAS_WEAPONS);
    head_fake.spawn(6, 1, ARMPW, 220, 200, OA_UNIT_FLAG_HAS_WEAPONS);
    brains::Recording heads;
    const brains::SeatCalls head_live = live_calls(head_fake);
    brains::recording_bind(heads, head_live, 1);
    brains::CallLog head_brain;
    brains::recording_begin(heads, head_brain, nullptr, 1);
    uint8_t preserve = 0;
    uint8_t command = 0;
    check(
        !heads.calls.host.primary_order(heads.calls.host.context, 5, &preserve, &command),
        "snapshot head is empty"
    );
    head_fake.holds_order[5] = true;
    const oa::FixedVec3 at{1, 2, 3};
    check(heads.calls.host.order_move(heads.calls.host.context, 5, &at, false), "recorded move");
    check(
        heads.calls.host.primary_order(heads.calls.host.context, 5, &preserve, &command),
        "a unit ordered this tick reads the live head"
    );
    check(
        !heads.calls.host.primary_order(heads.calls.host.context, 6, &preserve, &command),
        "another unit keeps the snapshot head"
    );
    check(head_fake.orders.empty(), "a recorded move leaves the fake");
}

uint32_t count_kind(const brains::CallLog& log, brains::LogKind kind) {
    uint32_t count = 0;
    for (uint32_t index = 0; index < log.count; ++index)
        count += log.items[index].kind == kind ? 1u : 0u;
    return count;
}

const brains::LogEntry* nth_kind(const brains::CallLog& log, brains::LogKind kind, uint32_t nth) {
    uint32_t seen = 0;
    for (uint32_t index = 0; index < log.count; ++index) {
        if (log.items[index].kind != kind)
            continue;
        if (seen == nth)
            return &log.items[index];
        ++seen;
    }
    return nullptr;
}

// (c) A gap in the built-in stream puts the copy back and logs one resync.
void test_realign() {
    oa::sim::match_runtime::SharedRandom steady{1};
    brains::CallLog steady_log;
    const uint32_t marker_state = steady.state();
    push_section(steady_log, "task1", marker_state);
    const uint32_t first_before = steady.state();
    const uint32_t first_value = steady.bounded(150);
    const uint32_t first_after = steady.state();
    push_draw(steady_log, 150, first_value, first_before, first_after);
    const uint32_t second_before = steady.state();
    const uint32_t second_value = steady.bounded(150);
    const uint32_t second_after = steady.state();
    push_draw(steady_log, 150, second_value, second_before, second_after);
    check(second_before == first_after, "a following draw has no gap");

    Fake fake;
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 1, 9);
    brains::Recording steady_rec;
    const brains::SeatCalls live = live_calls(fake);
    brains::recording_bind(steady_rec, live, 1);
    brains::CallLog steady_brain;
    brains::recording_begin(steady_rec, steady_brain, &steady_log, marker_state);
    steady_rec.calls.section(&steady_rec.calls.host, "task1");
    const uint32_t steady_first = steady_rec.calls.host.random(steady_rec.calls.host.context, 150);
    const uint32_t steady_second = steady_rec.calls.host.random(steady_rec.calls.host.context, 150);
    check(steady_first == first_value && steady_second == second_value, "draws with no gap");
    check(count_kind(steady_brain, brains::LogKind::resync) == 0, "no resync without a gap");

    oa::sim::match_runtime::SharedRandom gapped{1};
    brains::CallLog gapped_log;
    const uint32_t gapped_marker = gapped.state();
    push_section(gapped_log, "task1", gapped_marker);
    const uint32_t gap_first_before = gapped.state();
    const uint32_t gap_first_value = gapped.bounded(150);
    const uint32_t gap_first_after = gapped.state();
    push_draw(gapped_log, 150, gap_first_value, gap_first_before, gap_first_after);
    (void)gapped.bounded(3);
    const uint32_t gap_second_before = gapped.state();
    const uint32_t gap_second_value = gapped.bounded(150);
    const uint32_t gap_second_after = gapped.state();
    push_draw(gapped_log, 150, gap_second_value, gap_second_before, gap_second_after);
    check(gap_second_before != gap_first_after, "the extra draw opens a gap");

    brains::Recording gapped_rec;
    brains::recording_bind(gapped_rec, live, 1);
    brains::CallLog gapped_brain;
    brains::recording_begin(gapped_rec, gapped_brain, &gapped_log, gapped_marker);
    gapped_rec.calls.section(&gapped_rec.calls.host, "task1");
    const uint32_t gapped_first = gapped_rec.calls.host.random(gapped_rec.calls.host.context, 150);
    const uint32_t gapped_second = gapped_rec.calls.host.random(gapped_rec.calls.host.context, 150);
    check(gapped_first == gap_first_value, "the first draw matches");
    check(gapped_second == gap_second_value, "the second draw matches after resync");
    check(count_kind(gapped_brain, brains::LogKind::resync) == 1, "one resync");
    const brains::LogEntry* resync = nth_kind(gapped_brain, brains::LogKind::resync, 0);
    check(resync != nullptr && resync->state_before == gap_second_before, "resync names the state");
}

void expect_difference(
    const brains::TickDifference& got,
    bool different,
    const char* section,
    uint32_t tick,
    const char* label
) {
    check_at(got.different == different, label, tick);
    if (!different) {
        check_at(got.section[0] == '\0', "no section", tick);
        return;
    }
    check_at(same_text(got.section, section), label, tick);
}

void push_move(brains::CallLog& log, uint16_t slot) {
    brains::LogEntry entry{};
    entry.kind = brains::LogKind::move;
    entry.slot = slot;
    entry.x = 1;
    entry.z = 2;
    check(brains::log_push(log, entry), "push move");
}

void push_stance(brains::CallLog& log, uint16_t slot, int32_t move, int32_t fire) {
    brains::LogEntry entry{};
    entry.kind = brains::LogKind::stance;
    entry.slot = slot;
    entry.count = move;
    entry.fire = fire;
    check(brains::log_push(log, entry), "push stance");
}

// (d) Comparison names the section, and a section left out of the mask is skipped.
void test_compare() {
    brains::CallLog left;
    brains::CallLog right;
    push_section(left, "task1", 1);
    push_draw(left, 150, 7, 1, 2);
    push_section(right, "task1", 1);
    push_draw(right, 150, 7, 1, 2);
    expect_difference(
        brains::compare_logs(left, right, brains::shadow_mask_all), false, "", 11, "equal logs"
    );

    brains::CallLog bound_brain;
    push_section(bound_brain, "task1", 1);
    push_draw(bound_brain, 90, 7, 1, 2);
    expect_difference(
        brains::compare_logs(left, bound_brain, brains::shadow_mask_all),
        true,
        "task1",
        12,
        "changed bound"
    );

    brains::CallLog extra;
    push_section(extra, "task1", 1);
    push_draw(extra, 150, 7, 1, 2);
    push_move(extra, 4);
    expect_difference(
        brains::compare_logs(left, extra, brains::shadow_mask_all), true, "task1", 40, "extra entry"
    );

    brains::CallLog marked;
    brains::CallLog shorter;
    push_section(marked, "task1", 1);
    push_section(marked, "task2", 1);
    push_section(shorter, "task1", 1);
    expect_difference(
        brains::compare_logs(marked, shorter, brains::shadow_mask_all),
        true,
        "schedule",
        30,
        "changed marker list"
    );

    brains::CallLog stance_left;
    brains::CallLog stance_right;
    push_section(stance_left, "sort", 1);
    push_stance(stance_left, 10, 2, 2);
    push_section(stance_right, "sort", 1);
    push_stance(stance_right, 10, 2, 1);
    expect_difference(
        brains::compare_logs(stance_left, stance_right, brains::shadow_bit_sort),
        true,
        "sort",
        30,
        "changed stance"
    );

    brains::CallLog sort_only;
    brains::CallLog no_sort;
    push_section(sort_only, "sort", 1);
    push_section(no_sort, "task9", 1);
    expect_difference(
        brains::compare_logs(sort_only, no_sort, brains::shadow_bit_sort),
        true,
        "sort",
        30,
        "sort marker only on the built-in side"
    );

    brains::CallLog task_left;
    brains::CallLog task_right;
    push_section(task_left, "task1", 1);
    push_draw(task_left, 150, 7, 1, 2);
    push_section(task_left, "task9", 1);
    push_draw(task_left, 150, 1, 2, 3);
    push_section(task_right, "task1", 1);
    push_draw(task_right, 150, 7, 1, 2);
    push_section(task_right, "task9", 1);
    push_draw(task_right, 150, 9, 2, 3);
    expect_difference(
        brains::compare_logs(task_left, task_right, brains::shadow_bit_task(1)),
        false,
        "",
        75,
        "a section left out"
    );
}

// (e) A refused order is logged and does not reach the fake.
void test_refused() {
    Fake fake;
    fake.player(1, OA_PLAYER_STATUS_COMPUTER, 1, 9);
    fake.spawn(3, 1, ARMPW, 100, 100, OA_UNIT_FLAG_HAS_WEAPONS);
    brains::Recording recorded;
    const brains::SeatCalls live = live_calls(fake);
    brains::recording_bind(recorded, live, 1);
    brains::CallLog brain;
    brains::recording_begin(recorded, brain, nullptr, 1);
    check(recorded.calls.attack_queued(&recorded.calls.host, 3, 1), "refused attack");
    check(fake.orders.empty(), "a refused order leaves the fake");
    check(count_kind(brain, brains::LogKind::refused) == 1, "one refused line");
    const brains::LogEntry* refused = nth_kind(brain, brains::LogKind::refused, 0);
    check(
        refused != nullptr && same_text(refused->name, "attack") && refused->slot == 3,
        "refused attack 3"
    );
}

} // namespace

int main() {
    test_logging_host();
    test_recording_view();
    test_realign();
    test_compare();
    test_refused();
    failures += oa::test::computer_fake::fixture_failures;
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::printf("shadow tests passed\n");
    return 0;
}
