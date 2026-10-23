// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A per-tick bank. The balance moves by the instruction count alone, and a
// banked call's real-time limit is a backstop with its own sentence. The
// clock in this file measures the test; the balance does not read one.

#include "lua.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/test/check.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace host = oa::data::script_host;

namespace {

struct Package {
    std::map<std::string, std::vector<uint8_t>> files;
};

struct Counted {
    uint64_t iterations = 0;
};

struct KeptLines {
    std::vector<std::string> lines;
};

const char* definitions = "function spin(n)\n"
                          "  local i = 0\n"
                          "  while i < n do\n"
                          "    i = i + 1\n"
                          "  end\n"
                          "  return i\n"
                          "end\n"
                          "function empty()\n"
                          "end\n"
                          "function forever()\n"
                          "  while true do end\n"
                          "end\n"
                          "function caught()\n"
                          "  while true do\n"
                          "    pcall(function()\n"
                          "      while true do end\n"
                          "    end)\n"
                          "  end\n"
                          "end\n"
                          "function handled()\n"
                          "  while true do\n"
                          "    xpcall(function()\n"
                          "      while true do end\n"
                          "    end, function()\n"
                          "      while true do end\n"
                          "    end)\n"
                          "  end\n"
                          "end\n"
                          "function dump()\n"
                          "  for i = 1, 300 do\n"
                          "    print(i)\n"
                          "  end\n"
                          "end\n"
                          "function few()\n"
                          "  print(\"again\")\n"
                          "  print(\"kept\")\n"
                          "end\n"
                          "function once()\n"
                          "  print(\"one\")\n"
                          "end\n";

const char* tight_loop = "while true do end\n";

bool read_package(void* context, std::string_view path, std::vector<uint8_t>& bytes) {
    auto* package = static_cast<Package*>(context);
    const auto found = package->files.find(std::string(path));
    if (found == package->files.end())
        return false;
    bytes = found->second;
    return true;
}

void put(Package& package, std::string path, std::string_view source) {
    package.files[std::move(path)] = std::vector<uint8_t>(source.begin(), source.end());
}

// Instruction and bank cases count instructions. Sixty seconds is past any
// build's speed, so the count stops the call. The clock case below keeps a
// short limit and is the one that proves the backstop.
constexpr std::chrono::milliseconds instruction_call_limit{60'000};

host::Profile bank_profile() {
    return host::Profile{
        "bank-test",
        host::Budget{50'000'000, std::size_t{8} << 20},
        nullptr,
        nullptr,
        host::Bank{2'000'000, 30, instruction_call_limit},
    };
}

std::optional<host::Sandbox> open_with(
    Package& package,
    host::ScriptError& error,
    host::Profile profile,
    std::chrono::milliseconds limit = host::default_time_limit
) {
    host::OpenOptions options;
    options.package_id = "sample";
    options.profile = profile;
    options.time_limit = limit;
    options.sources.context = &package;
    options.sources.read = read_package;
    auto sandbox = host::Sandbox::open(options, error);
    OA_CHECK(sandbox.has_value());
    return sandbox;
}

bool define(host::Sandbox& sandbox, Package& package, host::ScriptError& error) {
    put(package, "mutator.lua", definitions);
    return sandbox.run_file("mutator.lua", "load", nullptr, error);
}

int push_spin(void* context, lua_State* state) {
    const auto* counted = static_cast<const Counted*>(context);
    lua_getglobal(state, "spin");
    lua_pushinteger(state, static_cast<lua_Integer>(counted->iterations));
    return 1;
}

int push_named(void* context, lua_State* state) {
    lua_getglobal(state, static_cast<const char*>(context));
    return 0;
}

void keep_line(void* context, std::string_view text) {
    static_cast<KeptLines*>(context)->lines.emplace_back(text);
}

void host_touch(void* /*context*/, lua_State* state) {
    lua_pushinteger(state, 1);
    lua_setglobal(state, "touched");
}

void host_forever(void* /*context*/, lua_State* state) {
    if (luaL_loadstring(state, tight_loop) != LUA_OK)
        return;
    lua_call(state, 0, 0);
}

std::string budget_message(uint64_t allowance) {
    return "the script ran past its budget of " + std::to_string(allowance) + " instructions";
}

std::string banked_message(uint64_t balance) {
    return "the script ran past its allowance of " + std::to_string(balance) +
           " instructions (2000000 a tick, banked up to 60000000)";
}

host::CallHooks spin_hooks(Counted& counted) {
    host::CallHooks hooks;
    hooks.context = &counted;
    hooks.push = push_spin;
    return hooks;
}

host::CallHooks named_hooks(const char* name) {
    host::CallHooks hooks;
    hooks.context = const_cast<char*>(name);
    hooks.push = push_named;
    return hooks;
}

bool measure_spin(uint64_t iterations, uint64_t* used, bool* ran) {
    if (iterations > static_cast<uint64_t>(std::numeric_limits<lua_Integer>::max()))
        return false;
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, bank_profile());
    if (!sandbox || !define(*sandbox, package, error))
        return false;
    Counted counted;
    counted.iterations = iterations;
    const host::CallHooks hooks = spin_hooks(counted);
    *ran = sandbox->call_banked("tick", hooks, error);
    *used = sandbox->instructions_used();
    return true;
}

struct Measurement {
    uint64_t iterations = 0;
    uint64_t used = 0;
    bool found = false;
};

Measurement find_in_range(uint64_t low, uint64_t high) {
    Measurement result;
    const uint64_t target = low + (high - low) / 2;
    uint64_t count = 200000;
    uint64_t used = 0;
    bool ran = false;
    if (!measure_spin(count, &used, &ran) || !ran || used == 0)
        return result;
    count = count * target / used;
    if (count == 0)
        count = 1;
    for (int attempt = 0; attempt < 8; ++attempt) {
        if (count > 80'000'000)
            return result;
        if (!measure_spin(count, &used, &ran))
            return result;
        if (ran && used >= low && used <= high) {
            result.iterations = count;
            result.used = used;
            result.found = true;
            std::printf(
                "bank range %llu..%llu iterations %llu used %llu\n",
                static_cast<unsigned long long>(low),
                static_cast<unsigned long long>(high),
                static_cast<unsigned long long>(count),
                static_cast<unsigned long long>(used)
            );
            return result;
        }
        uint64_t basis = used;
        if (!ran)
            basis = used > 0 ? used : 60'000'000;
        if (basis == 0)
            return result;
        uint64_t scaled = count * target / basis;
        if (scaled == count) {
            if (!ran || used > high)
                scaled = count > 1000 ? count - 1000 : 1;
            else
                scaled = count + 1000;
        }
        count = scaled == 0 ? 1 : scaled;
    }
    return result;
}

void bank_opens_full_and_refill_stops_at_the_limit() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, bank_profile());
    if (!sandbox)
        return;
    OA_CHECK(sandbox->bank_balance() == 60'000'000);
    OA_CHECK(sandbox->bank_limit() == 60'000'000);
    sandbox->refill_bank();
    OA_CHECK(sandbox->bank_balance() == 60'000'000);

    OA_CHECK(define(*sandbox, package, error));
    OA_CHECK(sandbox->bank_balance() == 60'000'000);
    Counted counted;
    counted.iterations = 200000;
    const host::CallHooks hooks = spin_hooks(counted);
    const uint64_t before = sandbox->bank_balance();
    OA_CHECK(sandbox->call_banked("tick", hooks, error));
    const uint64_t used = sandbox->instructions_used();
    OA_CHECK(used > 0);
    OA_CHECK(sandbox->bank_balance() == before - used);
    OA_CHECK(sandbox->bank_balance() < sandbox->bank_limit());

    while (sandbox->bank_balance() <= sandbox->bank_limit() - host::default_tick_allowance)
        sandbox->refill_bank();
    sandbox->refill_bank();
    OA_CHECK(sandbox->bank_balance() == 60'000'000);
    sandbox->refill_bank();
    OA_CHECK(sandbox->bank_balance() == 60'000'000);
    OA_CHECK(sandbox->bank_balance() <= sandbox->bank_limit());

    auto moved = std::move(*sandbox);
    OA_CHECK(sandbox->bank_balance() == 0);
    OA_CHECK(sandbox->bank_limit() == 0);
    sandbox->refill_bank();
    OA_CHECK(sandbox->bank_balance() == 0);
    OA_CHECK(moved.bank_balance() == 60'000'000);
    OA_CHECK(moved.bank_limit() == 60'000'000);
}

void a_profile_without_a_bank_stays_usable() {
    Package package;
    host::ScriptError error;
    const host::Profile plain{
        "plain", host::Budget{50'000'000, std::size_t{8} << 20}, nullptr, nullptr
    };
    auto sandbox = open_with(package, error, plain);
    if (!sandbox)
        return;
    OA_CHECK(sandbox->bank_balance() == 0);
    OA_CHECK(sandbox->bank_limit() == 0);
    sandbox->refill_bank();
    OA_CHECK(sandbox->bank_balance() == 0);

    OA_CHECK(define(*sandbox, package, error));
    host::ScriptError banked;
    const host::CallHooks hooks = named_hooks("empty");
    OA_CHECK(!sandbox->call_banked("tick", hooks, banked));
    OA_CHECK(banked.kind == host::ErrorKind::runtime);
    OA_CHECK(banked.message == "this sandbox's profile has no bank");
    OA_CHECK(!sandbox->failed());

    host::ScriptError called;
    OA_CHECK(sandbox->call("plan", hooks, called));
    OA_CHECK(!sandbox->failed());
}

void ticks_below_one_count_as_one() {
    Package package;
    host::ScriptError error;
    host::Profile profile{
        "short-bank",
        host::Budget{50'000'000, std::size_t{8} << 20},
        nullptr,
        nullptr,
        host::Bank{2'000'000, 0, instruction_call_limit},
    };
    auto sandbox = open_with(package, error, profile);
    if (!sandbox)
        return;
    OA_CHECK(sandbox->bank_limit() == 2'000'000);
    OA_CHECK(sandbox->bank_balance() == 2'000'000);
    sandbox->refill_bank();
    OA_CHECK(sandbox->bank_balance() == 2'000'000);
}

void the_limit_saturates() {
    Package package;
    host::ScriptError error;
    host::Profile profile{
        "wide-bank",
        host::Budget{50'000'000, std::size_t{8} << 20},
        nullptr,
        nullptr,
        host::Bank{UINT64_MAX, 30, instruction_call_limit},
    };
    auto sandbox = open_with(package, error, profile);
    if (!sandbox)
        return;
    OA_CHECK(sandbox->bank_limit() == UINT64_MAX);
    OA_CHECK(sandbox->bank_balance() == UINT64_MAX);
    sandbox->refill_bank();
    OA_CHECK(sandbox->bank_balance() == UINT64_MAX);
}

void a_banked_call_spends_what_it_used() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, bank_profile());
    if (!sandbox || !define(*sandbox, package, error))
        return;

    const host::CallHooks empty = named_hooks("empty");
    const uint64_t full = sandbox->bank_balance();
    OA_CHECK(sandbox->call_banked("tick", empty, error));
    OA_CHECK(sandbox->bank_balance() == full - sandbox->instructions_used());

    Counted counted;
    counted.iterations = 200000;
    const host::CallHooks hooks = spin_hooks(counted);
    const uint64_t before = sandbox->bank_balance();
    OA_CHECK(sandbox->call_banked("tick", hooks, error));
    const uint64_t used = sandbox->instructions_used();
    OA_CHECK(used > 0);
    OA_CHECK(sandbox->bank_balance() == before - used);
}

void two_sandboxes_spend_the_same() {
    Package first_package;
    Package second_package;
    host::ScriptError first_error;
    host::ScriptError second_error;
    auto first = open_with(first_package, first_error, bank_profile());
    auto second = open_with(second_package, second_error, bank_profile());
    if (!first || !second)
        return;
    OA_CHECK(define(*first, first_package, first_error));
    OA_CHECK(define(*second, second_package, second_error));

    const uint64_t counts[] = {100000, 200000, 150000};
    for (uint64_t iterations : counts) {
        Counted counted;
        counted.iterations = iterations;
        const host::CallHooks hooks = spin_hooks(counted);
        const uint64_t before = first->bank_balance();
        OA_CHECK(first->call_banked("tick", hooks, first_error));
        const uint64_t used = first->instructions_used();
        OA_CHECK(first->bank_balance() == before - used);
        OA_CHECK(second->call_banked("tick", hooks, second_error));
        OA_CHECK(second->instructions_used() == used);
        OA_CHECK(second->bank_balance() == first->bank_balance());
        first->refill_bank();
        second->refill_bank();
        OA_CHECK(second->bank_balance() == first->bank_balance());
    }
}

void spending_down_and_refilling() {
    const Measurement heavy = find_in_range(58'800'000, 59'200'000);
    const Measurement pass = find_in_range(20'300'000, 20'700'000);
    OA_CHECK(heavy.found);
    OA_CHECK(pass.found);
    if (!heavy.found || !pass.found)
        return;

    uint64_t fail_count = pass.iterations * 22'000'000 / pass.used;
    uint64_t fail_used = 0;
    bool fail_ran = false;
    bool fail_sized = false;
    for (int attempt = 0; attempt < 6; ++attempt) {
        if (!measure_spin(fail_count, &fail_used, &fail_ran))
            return;
        if (fail_ran && fail_used >= 21'500'000 && fail_used <= 22'500'000) {
            fail_sized = true;
            break;
        }
        if (fail_used == 0)
            return;
        const uint64_t basis = fail_ran ? fail_used : 60'000'000;
        uint64_t scaled = fail_count * 22'000'000 / basis;
        if (scaled == fail_count)
            scaled = fail_used > 22'500'000 || !fail_ran ? fail_count - fail_count / 20
                                                         : fail_count + fail_count / 20;
        if (scaled == 0)
            return;
        fail_count = scaled;
    }
    OA_CHECK(fail_sized);
    if (!fail_sized)
        return;
    std::printf(
        "bank fail-sized iterations %llu used %llu\n",
        static_cast<unsigned long long>(fail_count),
        static_cast<unsigned long long>(fail_used)
    );

    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, bank_profile());
    if (!sandbox || !define(*sandbox, package, error))
        return;

    Counted heavy_count;
    heavy_count.iterations = heavy.iterations;
    OA_CHECK(sandbox->call_banked("tick", spin_hooks(heavy_count), error));
    const uint64_t left = sandbox->bank_balance();
    OA_CHECK(sandbox->instructions_used() == heavy.used);
    OA_CHECK(left == 60'000'000 - heavy.used);
    OA_CHECK(left >= 800'000 && left <= 1'200'000);

    for (int refill = 0; refill < 10; ++refill)
        sandbox->refill_bank();
    OA_CHECK(sandbox->bank_balance() == left + 20'000'000);

    Counted pass_count;
    pass_count.iterations = pass.iterations;
    const uint64_t before_pass = sandbox->bank_balance();
    OA_CHECK(before_pass > pass.used);
    OA_CHECK(sandbox->call_banked("tick", spin_hooks(pass_count), error));
    OA_CHECK(sandbox->instructions_used() == pass.used);
    OA_CHECK(sandbox->bank_balance() == before_pass - pass.used);

    const uint64_t had = sandbox->bank_balance();
    OA_CHECK(had < 22'000'000);
    Counted too_much;
    too_much.iterations = fail_count;
    host::ScriptError spent;
    OA_CHECK(!sandbox->call_banked("heavy", spin_hooks(too_much), spent));
    OA_CHECK(spent.kind == host::ErrorKind::instruction_budget);
    OA_CHECK(spent.phase == "heavy");
    OA_CHECK(spent.message == banked_message(had));
    OA_CHECK(sandbox->bank_balance() == 0);
}

void a_banked_loop_stops_and_sticks(const char* function_name) {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, bank_profile());
    if (!sandbox || !define(*sandbox, package, error))
        return;
    const host::CallHooks hooks = named_hooks(function_name);
    OA_CHECK(!sandbox->call_banked("tick", hooks, error));
    OA_CHECK(error.kind == host::ErrorKind::instruction_budget);
    OA_CHECK(error.phase == "tick");
    OA_CHECK(error.message == banked_message(60'000'000));
    OA_CHECK(sandbox->failed());
    OA_CHECK(sandbox->instructions_used() >= 60'000'000);
    OA_CHECK(
        sandbox->instructions_used() <=
        60'000'000 + static_cast<uint64_t>(host::budget_check_interval)
    );

    host::ScriptError again;
    OA_CHECK(!sandbox->call("later", hooks, again));
    OA_CHECK(again.kind == error.kind);
    OA_CHECK(again.phase == error.phase);
    OA_CHECK(again.message == error.message);
}

std::string timed_message(const char* phase, std::chrono::milliseconds limit, bool banked) {
    const auto length = limit.count();
    long long number = length;
    const char* unit = "milliseconds";
    if (length > 0 && length % 1000 == 0) {
        number = length / 1000;
        unit = number == 1 ? "second" : "seconds";
    } else if (length == 1) {
        unit = "millisecond";
    }
    std::string text =
        std::string(phase) + " ran for more than " + std::to_string(number) + " " + unit;
    if (banked)
        text += " on this computer; the stop is for the computer's speed, not the script's "
                "instructions";
    else
        text += "; its work is too large or never ends";
    return text;
}

void a_banked_call_stops_on_the_clock() {
    host::Budget budget;
    budget.instructions = std::numeric_limits<uint64_t>::max();
    budget.memory_bytes = std::size_t{8} << 20;
    // Short on purpose: this case proves the real-time backstop. The
    // instruction cases use instruction_call_limit instead.
    const host::Profile profile{
        "timed",
        budget,
        nullptr,
        nullptr,
        host::Bank{1'000'000'000'000, 1, std::chrono::milliseconds{50}},
    };
    const auto open_limit = std::chrono::milliseconds{200};

    Package banked_package;
    host::ScriptError banked_error;
    auto banked = open_with(banked_package, banked_error, profile, open_limit);
    if (!banked || !define(*banked, banked_package, banked_error))
        return;
    const auto banked_started = std::chrono::steady_clock::now();
    const bool banked_ran = banked->call_banked("tick", named_hooks("forever"), banked_error);
    const auto banked_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - banked_started
    );
    std::printf(
        "time-limit banked elapsed %lld ms\n", static_cast<long long>(banked_elapsed.count())
    );
    OA_CHECK(!banked_ran);
    OA_CHECK(banked_error.kind == host::ErrorKind::time_limit);
    OA_CHECK(banked_error.phase == "tick");
    OA_CHECK(banked_error.message == timed_message("tick", std::chrono::milliseconds{50}, true));
    OA_CHECK(banked_elapsed >= std::chrono::milliseconds{50});
    OA_CHECK(banked_elapsed < std::chrono::milliseconds{50} + std::chrono::seconds{1});
    OA_CHECK(banked->instructions_used() < profile.bank.per_tick);

    Package plain_package;
    host::ScriptError plain_error;
    auto plain = open_with(plain_package, plain_error, profile, open_limit);
    if (!plain)
        return;
    put(plain_package, "mutator.lua", tight_loop);
    const auto plain_started = std::chrono::steady_clock::now();
    const bool plain_ran = plain->run_file("mutator.lua", "plan", nullptr, plain_error);
    const auto plain_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - plain_started
    );
    std::printf(
        "time-limit plain elapsed %lld ms\n", static_cast<long long>(plain_elapsed.count())
    );
    OA_CHECK(!plain_ran);
    OA_CHECK(plain_error.kind == host::ErrorKind::time_limit);
    OA_CHECK(plain_error.phase == "plan");
    OA_CHECK(plain_error.message == timed_message("plan", open_limit, false));
    OA_CHECK(plain_elapsed >= open_limit);
    OA_CHECK(plain_elapsed < open_limit + std::chrono::seconds{1});
    OA_CHECK(plain->bank_balance() == plain->bank_limit());
}

void expect_profile_budget(
    const char* phase, bool (*run)(host::Sandbox&, Package&, host::ScriptError&)
) {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, bank_profile());
    if (!sandbox)
        return;
    OA_CHECK(sandbox->bank_balance() == 60'000'000);
    OA_CHECK(!run(*sandbox, package, error));
    OA_CHECK(error.kind == host::ErrorKind::instruction_budget);
    OA_CHECK(error.phase == phase);
    OA_CHECK(error.message == budget_message(50'000'000));
    OA_CHECK(sandbox->bank_balance() == 60'000'000);
}

bool exhaust_by_file(host::Sandbox& sandbox, Package& package, host::ScriptError& error) {
    put(package, "mutator.lua", tight_loop);
    return sandbox.run_file("mutator.lua", "file", nullptr, error);
}

bool exhaust_by_call(host::Sandbox& sandbox, Package& package, host::ScriptError& error) {
    if (!define(sandbox, package, error))
        return true;
    return sandbox.call("call", named_hooks("forever"), error);
}

bool exhaust_by_host(host::Sandbox& sandbox, Package& /*package*/, host::ScriptError& error) {
    return sandbox.run_host("host", nullptr, host_forever, error);
}

void other_phases_keep_the_profile_budget() {
    expect_profile_budget("file", exhaust_by_file);
    expect_profile_budget("call", exhaust_by_call);
    expect_profile_budget("host", exhaust_by_host);

    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, bank_profile());
    if (!sandbox || !define(*sandbox, package, error))
        return;
    const uint64_t opened = sandbox->bank_balance();

    OA_CHECK(sandbox->call("plan", named_hooks("empty"), error));
    OA_CHECK(sandbox->bank_balance() == opened);
    OA_CHECK(sandbox->run_host("host", nullptr, host_touch, error));
    OA_CHECK(sandbox->bank_balance() == opened);

    Counted counted;
    counted.iterations = 100000;
    OA_CHECK(sandbox->call_banked("tick", spin_hooks(counted), error));
    const uint64_t after_bank = sandbox->bank_balance();
    OA_CHECK(after_bank == opened - sandbox->instructions_used());
    OA_CHECK(after_bank < opened);

    host::ScriptError spent;
    OA_CHECK(!sandbox->call("again", named_hooks("forever"), spent));
    OA_CHECK(spent.kind == host::ErrorKind::instruction_budget);
    OA_CHECK(spent.message == budget_message(50'000'000));
    OA_CHECK(sandbox->bank_balance() == after_bank);
}

void drain_printed_hands_lines_out_in_order() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, bank_profile());
    if (!sandbox || !define(*sandbox, package, error))
        return;

    OA_CHECK(sandbox->call_banked("tick", named_hooks("dump"), error));
    KeptLines kept;
    sandbox->drain_printed(&kept, keep_line);
    OA_CHECK(sandbox->printed().empty());
    bool ordered = kept.lines.size() == 256;
    for (std::size_t line_index = 0; line_index < 255 && ordered; ++line_index)
        ordered = kept.lines[line_index] == std::to_string(line_index + 1);
    if (!kept.lines.empty() && kept.lines.back() != "45 lines left out") {
        std::fprintf(stderr, "omission note: %s\n", kept.lines.back().c_str());
    }
    ordered = ordered && !kept.lines.empty() && kept.lines.back() == "45 lines left out";
    OA_CHECK(ordered);

    OA_CHECK(sandbox->call_banked("tick", named_hooks("few"), error));
    KeptLines again;
    sandbox->drain_printed(&again, keep_line);
    OA_CHECK(again.lines.size() == 2);
    OA_CHECK(again.lines[0] == "again");
    OA_CHECK(again.lines[1] == "kept");
    OA_CHECK(sandbox->printed().empty());

    OA_CHECK(sandbox->call_banked("tick", named_hooks("once"), error));
    OA_CHECK(sandbox->printed().size() == 1);
    OA_CHECK(sandbox->printed()[0] == "one");
    sandbox->drain_printed(nullptr, nullptr);
    OA_CHECK(sandbox->printed().empty());

    OA_CHECK(sandbox->call_banked("tick", named_hooks("few"), error));
    const auto lines = sandbox->printed();
    OA_CHECK(lines.size() == 2);
    OA_CHECK(lines[0] == "again");
    OA_CHECK(lines[1] == "kept");
}

} // namespace

int main() {
    bank_opens_full_and_refill_stops_at_the_limit();
    a_profile_without_a_bank_stays_usable();
    ticks_below_one_count_as_one();
    the_limit_saturates();
    a_banked_call_spends_what_it_used();
    two_sandboxes_spend_the_same();
    spending_down_and_refilling();
    a_banked_loop_stops_and_sticks("forever");
    a_banked_loop_stops_and_sticks("caught");
    a_banked_loop_stops_and_sticks("handled");
    a_banked_call_stops_on_the_clock();
    other_phases_keep_the_profile_budget();
    drain_printed_hands_lines_out_in_order();
    return oa::test::check_exit_status();
}
