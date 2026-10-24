// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A brain log and brain.record. Two runs of one match write the same lines.
// A built-in seat's lines are the shadow call log's lines. A record is
// checked and charged whether or not a log is open.

#include "match_fixture.hpp"

#include "oa/base/game_loop.hpp"
#include "oa/sim/brains/check.hpp"
#include "oa/sim/brains/classic.hpp"
#include "oa/sim/brains/logs.hpp"
#include "oa/sim/brains/seat.hpp"
#include "oa/sim/brains/shadow.hpp"
#include "oa/sim/state_hash.hpp"

#include "lua.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace brains = oa::sim::brains;
namespace host = oa::data::script_host;

constexpr char play_script[] = R"(local hooks = {}
function hooks.start()
  brain.every(100, function() end)
end
function hooks.onTick(tick)
  brain.random(8)
  math.random(1, 4)
  math.random()
  brain.shared_random(3)
  local units = me:units()
  if units[1] ~= nil then
    units[1]:move({x = 8, z = 8})
  end
  if tick == 1 then
    brain.record{
      features = {
        zeta = brain.tensor({1, 2}, {2}),
        alpha = brain.tensor({3}, {1})
      },
      decision = 17
    }
  end
end
return hooks
)";

constexpr char quiet_script[] = R"(local hooks = {}
function hooks.onTick() end
return hooks
)";

struct KeptSample {
    bool got{};
    uint8_t player{};
    uint32_t tick{};
    uint8_t feature_count{};
    bool decision_tensor{};
    int64_t decision{};
    std::string names[brains::record_feature_cap];
    uint8_t ranks[brains::record_feature_cap]{};
    uint32_t dims[brains::record_feature_cap][4]{};
    std::vector<int32_t> values[brains::record_feature_cap];
};

struct Tape {
    std::string text;
    std::vector<std::string> chunks;
    KeptSample sample;
};

void take_lines(void* context, std::string_view text) {
    auto* tape = static_cast<Tape*>(context);
    tape->chunks.emplace_back(text);
    tape->text.append(text.data(), text.size());
}

void take_sample(void* context, const brains::RecordSample& sample) {
    auto* tape = static_cast<Tape*>(context);
    KeptSample& kept = tape->sample;
    kept.got = true;
    kept.player = sample.player;
    kept.tick = sample.tick;
    kept.feature_count = sample.feature_count;
    kept.decision_tensor = sample.decision.tensor;
    kept.decision = sample.decision.whole;
    const uint8_t count = sample.feature_count < brains::record_feature_cap
                              ? sample.feature_count
                              : brains::record_feature_cap;
    for (uint8_t index = 0; index < count; ++index) {
        const brains::FeatureView& feature = sample.features[index];
        kept.names[index] = feature.name;
        kept.ranks[index] = feature.rank;
        for (uint8_t axis = 0; axis < 4; ++axis)
            kept.dims[index][axis] = feature.dims[axis];
        kept.values[index].assign(feature.values.begin(), feature.values.end());
    }
}

void ready_lists(Fixture& fixture) {
    CHECK(sim::ai::configure_match_computer_players(*fixture.match, profile, build_lists, false));
    sim::ai::prepare_match_computer_players(*fixture.match);
}

void seat_script(Fixture& fixture, const std::string& script, bool classic) {
    const auto bytes =
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(script.data()), script.size()};
    const brains::PackageFile file{"brain.lua", bytes};
    const std::span<const brains::PackageFile> files(&file, 1);
    const std::array<char, 65> fingerprint = brains::classic_fingerprint(files);
    brains::SeatSpec spec;
    spec.player = computer;
    spec.id = "order-giver";
    spec.name = "Order Giver";
    spec.version = "1";
    spec.entry = "brain.lua";
    spec.level = "hard";
    spec.difficulty = OA_DIFFICULTY_HARD;
    spec.files = files;
    if (classic)
        spec.classic_fingerprint_for_tests = fingerprint.data();
    const brains::SeatOutcome outcome = brains::seat_brain(*fixture.match, spec);
    if (outcome.result != brains::SeatResult::seated)
        throw std::runtime_error(std::string("seat refused: ") + outcome.reason.data());
}

void arm_log(Fixture& fixture, Tape& tape, bool samples) {
    brains::LogHooks hooks;
    hooks.context = &tape;
    hooks.lines = take_lines;
    if (samples)
        hooks.sample = take_sample;
    brains::set_log_hooks(*fixture.match, hooks);
}

void play(Fixture& fixture, uint32_t ticks) {
    for (uint32_t index = 0; index < ticks; ++index) {
        ++fixture.match->state().game.tick;
        fixture.match->tick();
    }
}

bool contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

std::string logged_match(Tape* tape, uint32_t ticks) {
    Fixture fixture;
    ready_lists(fixture);
    fixture.spawn(computer, vehicle, 10, 10);
    arm_log(fixture, *tape, true);
    seat_script(fixture, play_script, true);
    play(fixture, ticks);
    return tape->text;
}

void two_runs_write_the_same_lines() {
    Tape first;
    Tape second;
    const std::string left = logged_match(&first, 600);
    const std::string right = logged_match(&second, 600);
    CHECK(left == right);
    CHECK(contains(left, "brain log 1\n"));
    CHECK(contains(left, "tick "));
    CHECK(contains(left, "call onTick "));
    CHECK(contains(left, "call timer\n"));
    CHECK(contains(left, "move "));
    CHECK(contains(left, "own-draw "));
    CHECK(contains(left, "own-draw float "));
    CHECK(contains(left, "\ndraw "));
    CHECK(first.sample.got);
    CHECK(first.sample.player == computer);
    CHECK(first.sample.tick == 1);
    CHECK(first.sample.feature_count == 2);
    CHECK(!first.sample.decision_tensor);
    CHECK(first.sample.decision == 17);
    CHECK(first.sample.names[0] == "alpha");
    CHECK(first.sample.names[1] == "zeta");
    CHECK(first.sample.ranks[0] == 1);
    CHECK(first.sample.ranks[1] == 1);
    CHECK(first.sample.dims[0][0] == 1);
    CHECK(first.sample.dims[1][0] == 2);
    CHECK(first.sample.values[0].size() == 1);
    CHECK(first.sample.values[0][0] == 3 * 65536);
    CHECK(first.sample.values[1].size() == 2);
    CHECK(first.sample.values[1][0] == 1 * 65536);
    CHECK(first.sample.values[1][1] == 2 * 65536);
}

uint32_t match_stream(void* context) noexcept {
    return static_cast<sim::match_runtime::Match*>(context)->random_state();
}

std::string builtin_manual(Fixture& fixture, uint8_t player) {
    brains::CallLog log;
    brains::LoggingHost session;
    brains::logging_arm(
        session,
        sim::ai::match_computer_host(*fixture.match),
        log,
        player,
        match_stream,
        fixture.match.get()
    );
    sim::ai::computer_players_adopt(
        sim::ai::match_computer_players(*fixture.match), session.host, player
    );
    sim::ai::computer_player_tick_orders(
        sim::ai::match_computer_players(*fixture.match), session.host, player
    );
    brains::logging_note_stances(session);
    char* text = brains::render_call_log(log, fixture.match->state().game.tick, player, false);
    CHECK(text != nullptr);
    const std::string kept(text);
    delete[] text;
    return kept;
}

void builtin_lines_match_the_logging_host() {
    Fixture logged;
    Fixture manual;
    ready_lists(logged);
    ready_lists(manual);
    Tape tape;
    arm_log(logged, tape, false);
    const std::size_t banner = tape.chunks.size();
    CHECK(banner == 1);
    CHECK(tape.chunks[0] == "brain log 1\n");
    for (uint32_t index = 0; index < 120; ++index) {
        ++logged.match->state().game.tick;
        ++manual.match->state().game.tick;
        sim::ai::tick_match_computer_orders(*logged.match, computer);
        const std::string expected = builtin_manual(manual, computer);
        CHECK(tape.chunks.size() == banner + index + 1);
        CHECK(tape.chunks[banner + index] == expected);
    }
}

uint64_t state_hash(Fixture& fixture) {
    const oa::base::game_loop::Timing timing{};
    return oa::sim::trace::match_state_hash(*fixture.match, timing, 0, 0, nullptr);
}

void hooks_do_not_change_the_match() {
    Fixture logged;
    Fixture plain;
    ready_lists(logged);
    ready_lists(plain);
    logged.spawn(computer, vehicle, 10, 10);
    plain.spawn(computer, vehicle, 10, 10);
    Tape tape;
    arm_log(logged, tape, true);
    seat_script(logged, play_script, true);
    seat_script(plain, play_script, true);
    CHECK(
        brains::seat_instructions(*logged.match, computer) ==
        brains::seat_instructions(*plain.match, computer)
    );
    for (uint32_t index = 0; index < 600; ++index) {
        ++logged.match->state().game.tick;
        ++plain.match->state().game.tick;
        logged.match->tick();
        plain.match->tick();
        CHECK(logged.match->random_state() == plain.match->random_state());
        CHECK(state_hash(logged) == state_hash(plain));
        CHECK(
            brains::seat_instructions(*logged.match, computer) ==
            brains::seat_instructions(*plain.match, computer)
        );
    }
}

struct Chunk {
    const char* source{};
};

int push_chunk(void* context, lua_State* state) {
    const auto* chunk = static_cast<const Chunk*>(context);
    if (chunk == nullptr || chunk->source == nullptr)
        return luaL_error(state, "missing probe");
    if (luaL_loadstring(state, chunk->source) != 0) {
        const char* message = lua_tostring(state, -1);
        if (message == nullptr)
            message = "probe failed to load";
        return luaL_error(state, "%s", message);
    }
    return 0;
}

bool probe_fails(host::Sandbox& sandbox, const char* source, const char* sentence) {
    Chunk chunk{source};
    host::CallHooks hooks;
    hooks.context = &chunk;
    hooks.push = push_chunk;
    host::ScriptError error;
    if (sandbox.call("probe", hooks, error))
        return false;
    return error.message.find(sentence) != std::string::npos;
}

void record_refusals_and_the_same_charge() {
    Fixture logged;
    Fixture plain;
    ready_lists(logged);
    ready_lists(plain);
    Tape tape;
    arm_log(logged, tape, true);
    seat_script(logged, quiet_script, false);
    seat_script(plain, quiet_script, false);
    host::Sandbox* sandbox = brains::seat_sandbox(*logged.match, computer);
    CHECK(sandbox != nullptr);
    CHECK(probe_fails(*sandbox, "brain.record(1)\n", "brain.record: expects one table"));
    CHECK(probe_fails(
        *sandbox, "brain.record{}\n", "brain.record: features must be a table of 1 to 16 tensors"
    ));
    CHECK(probe_fails(
        *sandbox,
        "brain.record{features = {['Bad'] = brain.tensor({1}, {1})}}\n",
        "brain.record: a feature name is 1 to 31 letters, digits or underscores"
    ));
    CHECK(probe_fails(
        *sandbox,
        "brain.record{features = {alpha = 1}}\n",
        "brain.record: feature 'alpha' is not a tensor"
    ));
    CHECK(probe_fails(
        *sandbox,
        "brain.record{features = {alpha = brain.tensor({1}, {1})}, decision = 1.5}\n",
        "brain.record: decision must be an integer or a tensor"
    ));
    host::Sandbox* plain_box = brains::seat_sandbox(*plain.match, computer);
    CHECK(plain_box != nullptr);
    const char* good = "brain.record{features = {alpha = brain.tensor({1}, {1})}, decision = 4}\n";
    Chunk logged_chunk{good};
    Chunk plain_chunk{good};
    host::CallHooks logged_hooks;
    logged_hooks.context = &logged_chunk;
    logged_hooks.push = push_chunk;
    host::CallHooks plain_hooks;
    plain_hooks.context = &plain_chunk;
    plain_hooks.push = push_chunk;
    host::ScriptError logged_error;
    host::ScriptError plain_error;
    CHECK(sandbox->call("probe", logged_hooks, logged_error));
    CHECK(plain_box->call("probe", plain_hooks, plain_error));
    CHECK(sandbox->instructions_used() == plain_box->instructions_used());
    CHECK(sandbox->instructions_used() >= 64);
}

brains::SeatOutcome check_script(const char* script) {
    const std::string text(script);
    const auto bytes =
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(text.data()), text.size()};
    const brains::PackageFile file{"brain.lua", bytes};
    brains::SeatSpec spec;
    spec.player = 0;
    spec.id = "order-giver";
    spec.name = "Order Giver";
    spec.version = "1";
    spec.entry = "brain.lua";
    spec.level = "hard";
    spec.difficulty = OA_DIFFICULTY_HARD;
    spec.files = std::span<const brains::PackageFile>(&file, 1);
    return brains::check_brain(spec, oa::data::brain_facts::GameFacts{});
}

void record_outside_a_match_is_charged() {
    const brains::SeatOutcome plain =
        check_script("local hooks = {}\nfunction hooks.fits() return true end\nreturn hooks\n");
    const brains::SeatOutcome charged = check_script(
        "local hooks = {}\n"
        "function hooks.fits()\n"
        "  brain.record{features = {alpha = brain.tensor({1}, {1})}, decision = 1}\n"
        "  return true\n"
        "end\n"
        "return hooks\n"
    );
    CHECK(plain.result == brains::SeatResult::seated);
    CHECK(charged.result == brains::SeatResult::seated);
    CHECK(charged.fits_instructions >= plain.fits_instructions + 64);
    const brains::SeatOutcome refused = check_script(
        "local hooks = {}\nfunction hooks.fits()\n  brain.record(1)\n  return true\nend\nreturn "
        "hooks\n"
    );
    CHECK(refused.result == brains::SeatResult::failed);
    CHECK(refused.error.message.find("brain.record: expects one table") != std::string::npos);
}

} // namespace

int main() {
    try {
        two_runs_write_the_same_lines();
        builtin_lines_match_the_logging_host();
        hooks_do_not_change_the_match();
        record_refusals_and_the_same_charge();
        record_outside_a_match_is_charged();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "sim-brains-logs: %s\n", error.what());
        return 1;
    }
    return 0;
}
