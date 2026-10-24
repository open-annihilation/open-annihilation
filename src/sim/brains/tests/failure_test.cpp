// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A seated brain that fails is handed back on the next tick. The made-up
// brain is steady-commander. The built-in player resumes the controller it
// already had.

#include "match_fixture.hpp"

#include "oa/core/player.h"
#include "oa/sim/ai.hpp"
#include "oa/sim/brains/seat.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace brains = oa::sim::brains;
namespace host = oa::data::script_host;

struct Package {
    std::string id{"steady-commander"};
    std::string name{"Steady Commander"};
    std::string version{"1"};
    std::string entry{"brain.lua"};
    std::string level{"steady"};
    int32_t difficulty{OA_DIFFICULTY_MEDIUM};
    uint8_t player{computer};
    host::Bank bank{
        host::default_tick_allowance, host::default_bank_ticks, host::default_call_time_limit
    };
    std::vector<std::pair<std::string, std::string>> files;
};

struct Report {
    struct Line {
        uint32_t tick{};
        std::string text;
    };

    std::vector<Line> lines;
    int stops{};
    uint8_t player{};
    uint32_t tick{};
    host::ScriptError error{};
};

void add_file(Package& package, std::string path, std::string text) {
    package.files.emplace_back(std::move(path), std::move(text));
}

brains::SeatOutcome seat_it(Fixture& fixture, Package& package) {
    std::vector<brains::PackageFile> files;
    files.reserve(package.files.size());
    for (const auto& held : package.files)
        files.push_back(
            {held.first,
             std::span<const uint8_t>{
                 reinterpret_cast<const uint8_t*>(held.second.data()), held.second.size()
             }}
        );
    brains::SeatSpec spec;
    spec.player = package.player;
    spec.id = package.id;
    spec.name = package.name;
    spec.version = package.version;
    spec.entry = package.entry;
    spec.level = package.level;
    spec.difficulty = package.difficulty;
    spec.files = files;
    spec.bank = package.bank;
    return brains::seat_brain(*fixture.match, spec);
}

void printed(void* context, uint8_t, uint32_t tick, std::string_view line) {
    auto* report = static_cast<Report*>(context);
    report->lines.push_back(Report::Line{tick, std::string(line)});
}

void stopped(void* context, uint8_t player, uint32_t tick, const host::ScriptError& error) {
    auto* report = static_cast<Report*>(context);
    report->stops += 1;
    report->player = player;
    report->tick = tick;
    report->error = error;
}

void watch(Fixture& fixture, Report& report) {
    brains::ReportHooks hooks;
    hooks.context = &report;
    hooks.printed = &printed;
    hooks.stopped = &stopped;
    brains::set_report_hooks(*fixture.match, hooks);
}

void advance(Fixture& fixture) {
    ++fixture.match->state().game.tick;
    fixture.match->tick();
}

void advance_n(Fixture& fixture, uint32_t ticks) {
    for (uint32_t passed = 0; passed < ticks; ++passed)
        advance(fixture);
}

void arm(Fixture& fixture) {
    fixture.spawn(0, vehicle, 8, 8);
    fixture.spawn(computer, vehicle, 40, 40);
    CHECK(sim::ai::configure_match_computer_players(*fixture.match, profile, build_lists, false));
}

int32_t countdown_of(Fixture& fixture) {
    auto* players = sim::ai::match_computer_players(*fixture.match);
    CHECK(players != nullptr);
    return players->players[computer].sort_countdown;
}

void expect_still_seated(Fixture& fixture, const Report& report) {
    CHECK(report.stops == 0);
    CHECK(brains::brain_seated(*fixture.match, computer));
    CHECK(brains::seat_sandbox(*fixture.match, computer) != nullptr);
    CHECK(countdown_of(fixture) == static_cast<int32_t>(sim::ai::sort_interval_ticks));
}

void expect_handed_back(Fixture& fixture, const Report& report, uint32_t tick) {
    CHECK(report.stops == 1);
    CHECK(report.player == computer);
    CHECK(report.tick == tick);
    CHECK(fixture.match->state().game.tick == tick);
    CHECK(!brains::brain_seated(*fixture.match, computer));
    CHECK(brains::seat_sandbox(*fixture.match, computer) == nullptr);
    CHECK(fixture.match->seat_hooks(computer).play == nullptr);
    CHECK(countdown_of(fixture) == static_cast<int32_t>(sim::ai::sort_interval_ticks));
    advance(fixture);
    CHECK(countdown_of(fixture) == static_cast<int32_t>(sim::ai::sort_interval_ticks) - 1);
    CHECK(report.stops == 1);
}

void error_at_tick_200_hands_the_seat_back() {
    Fixture fixture;
    arm(fixture);
    Report report;
    watch(fixture, report);
    Package package;
    add_file(
        package,
        "brain.lua",
        "return {\n"
        "  onTick = function(tick)\n"
        "    if tick >= 200 then\n"
        "      local missing = nil\n"
        "      return missing.field\n"
        "    end\n"
        "  end\n"
        "}\n"
    );
    CHECK(seat_it(fixture, package).result == brains::SeatResult::seated);
    advance_n(fixture, 200);
    expect_handed_back(fixture, report, 200);
    CHECK(report.error.kind == host::ErrorKind::runtime);
    CHECK(report.error.chunk == "brain.lua");
    CHECK(report.error.line != 0);
    CHECK(report.error.message.find("attempt to index a nil value") != std::string::npos);
    advance_n(fixture, run_ticks - 201);
    CHECK(fixture.match->state().game.tick == run_ticks);
    CHECK(fixture.count(computer, collector) != 0);
}

void stop_on_first_tick(Package& package, host::ErrorKind kind, std::string_view message) {
    Fixture fixture;
    arm(fixture);
    Report report;
    watch(fixture, report);
    CHECK(seat_it(fixture, package).result == brains::SeatResult::seated);
    advance(fixture);
    expect_handed_back(fixture, report, 1);
    CHECK(report.error.kind == kind);
    CHECK(report.error.message.find(message) != std::string::npos);
}

void bank_spent_hands_the_seat_back() {
    Package package;
    // The default bank is 60 million instructions. A loaded or slow build can
    // take longer than the 250 ms real-time limit to spend it, so the clock
    // would stop the call first; this case is about the instructions.
    package.bank.call_time_limit = std::chrono::milliseconds(60'000);
    add_file(package, "brain.lua", "return { onTick = function() while true do end end }\n");
    stop_on_first_tick(package, host::ErrorKind::instruction_budget, "allowance of");
}

void time_limit_hands_the_seat_back() {
    Package package;
    package.bank.per_tick = 50'000'000'000ULL;
    package.bank.ticks = 1;
    package.bank.call_time_limit = std::chrono::milliseconds(50);
    add_file(package, "brain.lua", "return { onTick = function() while true do end end }\n");
    stop_on_first_tick(package, host::ErrorKind::time_limit, "on this computer");
}

void memory_full_hands_the_seat_back() {
    Package package;
    package.bank.per_tick = 50'000'000'000ULL;
    package.bank.ticks = 1;
    package.bank.call_time_limit = std::chrono::milliseconds(5000);
    add_file(
        package,
        "brain.lua",
        "return {\n"
        "  onTick = function()\n"
        "    local held = {}\n"
        "    for i = 1, 2000 do\n"
        "      held[i] = string.rep(\"x\", 65536)\n"
        "    end\n"
        "  end\n"
        "}\n"
    );
    stop_on_first_tick(package, host::ErrorKind::memory_budget, "MiB of memory");
}

void long_string_hands_the_seat_back() {
    Package package;
    add_file(
        package, "brain.lua", "return { onTick = function() return string.rep(\"a\", 65537) end }\n"
    );
    stop_on_first_tick(package, host::ErrorKind::string_length, "65536");
}

void printed_lines_keep_their_ticks() {
    Fixture fixture;
    Report report;
    watch(fixture, report);
    Package package;
    add_file(
        package,
        "brain.lua",
        "return {\n"
        "  start = function() print(\"from-start\") end,\n"
        "  onTick = function(tick)\n"
        "    if tick == 1 then print(\"turn-one\") end\n"
        "    if tick == 2 then print(\"turn-two\") end\n"
        "  end\n"
        "}\n"
    );
    CHECK(seat_it(fixture, package).result == brains::SeatResult::seated);
    CHECK(report.lines.size() == 1);
    CHECK(report.lines[0].tick == 0);
    CHECK(report.lines[0].text == "from-start");
    advance(fixture);
    advance(fixture);
    CHECK(report.lines.size() == 3);
    CHECK(report.lines[1].tick == 1);
    CHECK(report.lines[1].text == "turn-one");
    CHECK(report.lines[2].tick == 2);
    CHECK(report.lines[2].text == "turn-two");
    CHECK(report.stops == 0);
    CHECK(brains::brain_seated(*fixture.match, computer));
}

void a_long_print_keeps_256_lines_and_the_omission() {
    Fixture fixture;
    Report report;
    watch(fixture, report);
    Package package;
    add_file(
        package,
        "brain.lua",
        "return {\n"
        "  onTick = function()\n"
        "    for i = 1, 300 do\n"
        "      print(\"row\")\n"
        "    end\n"
        "  end\n"
        "}\n"
    );
    CHECK(seat_it(fixture, package).result == brains::SeatResult::seated);
    advance(fixture);
    CHECK(report.lines.size() == 256);
    CHECK(report.lines.front().tick == 1);
    CHECK(report.lines.front().text == "row");
    CHECK(report.lines.back().text == "45 lines left out");
    CHECK(report.stops == 0);
}

void a_brain_that_never_fails_is_not_handed_back() {
    Fixture fixture;
    arm(fixture);
    Report report;
    watch(fixture, report);
    Package package;
    add_file(package, "brain.lua", "return { onTick = function() end }\n");
    CHECK(seat_it(fixture, package).result == brains::SeatResult::seated);
    advance_n(fixture, 30);
    expect_still_seated(fixture, report);
    CHECK(fixture.count(computer, collector) == 0);
}

void fits_and_start_do_not_report_a_stop() {
    Fixture fixture;
    Report report;
    watch(fixture, report);
    Package unfit;
    add_file(
        unfit,
        "brain.lua",
        "return { fits = function() print(\"fitting\") return \"no room\" end }\n"
    );
    const brains::SeatOutcome declined = seat_it(fixture, unfit);
    CHECK(declined.result == brains::SeatResult::does_not_fit);
    CHECK(report.stops == 0);
    CHECK(report.lines.size() == 1);
    CHECK(report.lines[0].tick == 0);
    CHECK(report.lines[0].text == "fitting");

    Report again;
    watch(fixture, again);
    Package broken;
    add_file(
        broken, "brain.lua", "return { start = function() print(\"before\") error(\"nope\") end }\n"
    );
    const brains::SeatOutcome failed = seat_it(fixture, broken);
    CHECK(failed.result == brains::SeatResult::failed);
    CHECK(again.stops == 0);
    CHECK(again.lines.size() == 1);
    CHECK(again.lines[0].text == "before");
    CHECK(!brains::brain_seated(*fixture.match, computer));
}

} // namespace

int main() {
    try {
        error_at_tick_200_hands_the_seat_back();
        bank_spent_hands_the_seat_back();
        time_limit_hands_the_seat_back();
        memory_full_hands_the_seat_back();
        long_string_hands_the_seat_back();
        printed_lines_keep_their_ticks();
        a_long_print_keeps_256_lines_and_the_omission();
        a_brain_that_never_fails_is_not_handed_back();
        fits_and_start_do_not_report_a_stop();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "sim-brains-failure: %s\n", ex.what());
        return 1;
    }
    return 0;
}
