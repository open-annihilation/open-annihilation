// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The brain's own random stream, and the rule that recognises Classic.
// The golden table is the stdout of tests/random_reference.py.

#include "match_fixture.hpp"

#include "classic_fingerprint.hpp"
#include "classic_sample_fingerprint.hpp"
#include "internal.hpp"

#include "oa/data/brain_facts.hpp"
#include "oa/sim/brains/check.hpp"
#include "oa/sim/brains/classic.hpp"
#include "oa/sim/match_runtime/script.hpp"

#include "lua.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

namespace brains = oa::sim::brains;
namespace host = oa::data::script_host;

struct Golden {
    uint32_t match_seed{};
    uint8_t player{};
    const char* id{};
    const char* version{};
    uint64_t seed{};
    int draws[10]{};
};

// clang-format off
const Golden golden[] = {
    {12345u, 1u, "steady-commander", "1.2", 0x5cdff17c53fadb5bULL, {54, 26, 75, 99, 92, 33, 83, 49, 12, 48}},
    {0u, 0u, "steady-commander", "1.2", 0x9708213a39e810c2ULL, {57, 59, 14, 97, 97, 32, 6, 23, 32, 88}},
    {12345u, 2u, "steady-commander", "1.2", 0x101f10c5c79f6d56ULL, {10, 44, 5, 21, 28, 8, 57, 38, 13, 30}},
    {12345u, 1u, "steady-commander", "1.3", 0xb625f82472af6f6bULL, {98, 48, 26, 22, 29, 46, 70, 47, 72, 14}},
};
// clang-format on

struct DrawRead {
    std::vector<lua_Integer> values;
    lua_Integer continued{};
    bool has_continued{};
    lua_Integer replaced{};
    bool has_replaced{};
    bool shared_nil{};
    bool shared_present{};
    lua_Integer shared{};
    bool shared_ran{};
};

void read_draws(void* context, lua_State* state) {
    auto* held = static_cast<DrawRead*>(context);
    lua_getglobal(state, "drawn");
    if (lua_istable(state, -1) != 0) {
        const int count = static_cast<int>(lua_rawlen(state, -1));
        for (int row = 1; row <= count; ++row) {
            lua_rawgeti(state, -1, row);
            if (lua_isinteger(state, -1) != 0)
                held->values.push_back(lua_tointeger(state, -1));
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);
    lua_getglobal(state, "continued");
    if (lua_isinteger(state, -1) != 0) {
        held->continued = lua_tointeger(state, -1);
        held->has_continued = true;
    }
    lua_pop(state, 1);
    lua_getglobal(state, "replaced");
    if (lua_isinteger(state, -1) != 0) {
        held->replaced = lua_tointeger(state, -1);
        held->has_replaced = true;
    }
    lua_pop(state, 1);
}

DrawRead read_sandbox(host::Sandbox& sandbox) {
    DrawRead held;
    host::ScriptError error;
    CHECK(sandbox.run_host("read", &held, read_draws, error));
    return held;
}

void expect_draws(const DrawRead& held, const int* draws, int count) {
    CHECK(static_cast<int>(held.values.size()) == count);
    for (int index = 0; index < count; ++index)
        CHECK(held.values[static_cast<std::size_t>(index)] == draws[index]);
}

struct Seeded {
    /// The sources and the seat stay put when this value is moved. The sandbox
    /// keeps their addresses, and it is destroyed first.
    std::string script;
    std::unique_ptr<host::MemorySources> sources = std::make_unique<host::MemorySources>();
    std::unique_ptr<brains::Seat> seat;
    std::optional<host::Sandbox> sandbox;
};

Seeded open_seeded(uint64_t seed, std::string script) {
    Seeded held;
    held.script = std::move(script);
    held.seat = std::make_unique<brains::Seat>();
    CHECK(held.sources->add(
        "brain.lua",
        std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(held.script.data()), held.script.size()
        )
    ));
    host::OpenOptions options;
    options.package_id = "steady-commander";
    options.profile = brains::brain_profile(
        *held.seat,
        host::Bank{
            host::default_tick_allowance, host::default_bank_ticks, host::default_call_time_limit
        }
    );
    options.sources = held.sources->hooks();
    options.random_seed = seed;
    host::ScriptError error;
    auto opened = host::Sandbox::open(options, error);
    CHECK(opened.has_value());
    held.sandbox = std::move(*opened);
    CHECK(held.sandbox->run_file("brain.lua", "load", nullptr, error));
    return held;
}

const char* ten_draws = R"(drawn = {}
for _ = 1, 10 do
  drawn[#drawn + 1] = math.random(1, 100)
end
)";

const char* continued_draws = R"(drawn = {}
for _ = 1, 10 do
  drawn[#drawn + 1] = math.random(1, 100)
end
math.random = function()
  return 7
end
continued = brain.random(100)
replaced = math.random()
)";

const char* eleven_draws = R"(drawn = {}
for _ = 1, 11 do
  drawn[#drawn + 1] = math.random(1, 100)
end
)";

void seeds_match_the_reference() {
    CHECK(brains::classic_fingerprint_builtin[0] == '\0');
    for (const Golden& row : golden) {
        const uint64_t seed =
            brains::brain_random_seed(row.match_seed, row.player, row.id, row.version);
        CHECK(seed == row.seed);
    }
    for (std::size_t left = 0; left < 4; ++left) {
        for (std::size_t right = left + 1; right < 4; ++right)
            CHECK(golden[left].seed != golden[right].seed);
    }
}

void draws_match_the_reference() {
    for (const Golden& row : golden) {
        Seeded seeded = open_seeded(row.seed, ten_draws);
        expect_draws(read_sandbox(*seeded.sandbox), row.draws, 10);
    }
}

void replacing_math_random_leaves_brain_random() {
    const Golden& row = golden[0];
    Seeded continued = open_seeded(row.seed, continued_draws);
    const DrawRead held = read_sandbox(*continued.sandbox);
    expect_draws(held, row.draws, 10);
    CHECK(held.has_continued);
    CHECK(held.has_replaced);
    CHECK(held.replaced == 7);
    Seeded eleven = open_seeded(row.seed, eleven_draws);
    const DrawRead longer = read_sandbox(*eleven.sandbox);
    CHECK(longer.values.size() == 11);
    CHECK(longer.values[10] == held.continued);
    CHECK(held.continued != 7);
}

struct Package {
    std::string id{"steady-commander"};
    std::string version{"1.2"};
    std::string entry{"brain.lua"};
    uint8_t player{computer};
    std::string script;
    std::string fingerprint;
};

brains::SeatOutcome seat_script(Fixture& fixture, Package& package) {
    std::vector<brains::PackageFile> files;
    files.push_back(
        {package.entry,
         std::span<const uint8_t>(
             reinterpret_cast<const uint8_t*>(package.script.data()), package.script.size()
         )}
    );
    brains::SeatSpec spec;
    spec.player = package.player;
    spec.id = package.id;
    spec.name = "Steady Commander";
    spec.version = package.version;
    spec.entry = package.entry;
    spec.level = "steady";
    spec.difficulty = OA_DIFFICULTY_MEDIUM;
    spec.files = files;
    spec.classic_fingerprint_for_tests = package.fingerprint;
    return brains::seat_brain(*fixture.match, spec);
}

const char* seated_draws = R"(local hooks = {}
drawn = {}
function hooks.fits()
  return true
end
function hooks.start()
  for _ = 1, 5 do
    drawn[#drawn + 1] = math.random(1, 100)
  end
  for _ = 1, 5 do
    drawn[#drawn + 1] = brain.random(100)
  end
end
return hooks
)";

void a_seated_brain_draws_the_match_seed() {
    Fixture fixture(0, true, {}, 0, 12345);
    CHECK(fixture.match->random_seed() == 12345u);
    Package package;
    package.script = seated_draws;
    const brains::SeatOutcome outcome = seat_script(fixture, package);
    CHECK(outcome.result == brains::SeatResult::seated);
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    expect_draws(read_sandbox(*sandbox), golden[0].draws, 10);
    CHECK(!brains::seat_is_classic(*fixture.match, computer));
}

const char* heavy_tick = R"(local hooks = {}
function hooks.fits()
  return true
end
function hooks.start()
  return true
end
function hooks.onTick()
  for _ = 1, 10000 do
    brain.random(1000)
  end
end
return hooks
)";

const char* quiet_tick = R"(local hooks = {}
function hooks.fits()
  return true
end
function hooks.start()
  return true
end
function hooks.onTick()
end
return hooks
)";

void own_draws_leave_the_shared_stream() {
    Fixture heavy;
    Fixture quiet;
    Package drawing;
    drawing.script = heavy_tick;
    Package still;
    still.script = quiet_tick;
    CHECK(seat_script(heavy, drawing).result == brains::SeatResult::seated);
    CHECK(seat_script(quiet, still).result == brains::SeatResult::seated);
    CHECK(heavy.match->random_state() == quiet.match->random_state());
    for (int tick = 0; tick < 4; ++tick) {
        ++heavy.match->state().game.tick;
        heavy.match->tick();
        ++quiet.match->state().game.tick;
        quiet.match->tick();
        CHECK(heavy.match->random_state() == quiet.match->random_state());
    }
}

struct Sample {
    std::vector<std::string> paths;
    std::vector<std::vector<uint8_t>> bytes;

    std::vector<brains::PackageFile> files() const {
        std::vector<brains::PackageFile> listed;
        listed.reserve(paths.size());
        for (std::size_t index = 0; index < paths.size(); ++index)
            listed.push_back({paths[index], bytes[index]});
        return listed;
    }
};

std::vector<uint8_t> read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error("cannot read " + path.string());
    input.seekg(0, std::ios::end);
    const auto length = input.tellg();
    input.seekg(0);
    if (length < 0)
        throw std::runtime_error("cannot read " + path.string());
    std::vector<uint8_t> bytes(static_cast<std::size_t>(length));
    if (!bytes.empty())
        input.read(
            reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
    if (!input)
        throw std::runtime_error("cannot read " + path.string());
    return bytes;
}

Sample load_sample() {
    const std::filesystem::path root(OA_CLASSIC_SAMPLE_DIR);
    const char* names[] = {"LICENSE", "README.md", "lua/a.lua", "oabrain.yaml"};
    std::vector<std::string> expected(std::begin(names), std::end(names));
    std::sort(expected.begin(), expected.end());
    std::vector<std::string> found;
    for (const std::filesystem::directory_entry& entry :
         std::filesystem::recursive_directory_iterator(root)) {
        if (!entry.is_regular_file())
            continue;
        found.push_back(entry.path().lexically_relative(root).generic_string());
    }
    std::sort(found.begin(), found.end());
    CHECK(found == expected);
    Sample sample;
    for (const char* name : names) {
        sample.paths.emplace_back(name);
        sample.bytes.push_back(read_file(root / name));
    }
    return sample;
}

std::string fingerprint_text(const Sample& sample) {
    const std::vector<brains::PackageFile> files = sample.files();
    const std::array<char, 65> text = brains::classic_fingerprint(files);
    return std::string(text.data());
}

brains::SeatOutcome
seat_sample(Fixture& fixture, const Sample& sample, std::string_view fingerprint) {
    const std::vector<brains::PackageFile> files = sample.files();
    brains::SeatSpec spec;
    spec.player = computer;
    spec.id = "steady-commander";
    spec.name = "Steady Commander";
    spec.version = "1.2";
    spec.entry = "lua/a.lua";
    spec.level = "steady";
    spec.difficulty = OA_DIFFICULTY_MEDIUM;
    spec.files = files;
    spec.classic_fingerprint_for_tests = fingerprint;
    return brains::seat_brain(*fixture.match, spec);
}

struct SharedCall {
    lua_Integer bound{};
    bool present{};
    lua_Integer value{};
    bool ran{};
};

void call_shared(void* context, lua_State* state) {
    auto* held = static_cast<SharedCall*>(context);
    lua_getglobal(state, "brain");
    lua_getfield(state, -1, "shared_random");
    held->present = lua_isnil(state, -1) == 0;
    if (!held->present) {
        lua_pop(state, 2);
        return;
    }
    lua_pushinteger(state, held->bound);
    lua_call(state, 1, 1);
    held->ran = lua_isinteger(state, -1) != 0;
    if (held->ran)
        held->value = lua_tointeger(state, -1);
    lua_pop(state, 2);
}

SharedCall draw_shared(host::Sandbox& sandbox, lua_Integer bound) {
    SharedCall held;
    held.bound = bound;
    host::ScriptError error;
    CHECK(sandbox.run_host("shared", &held, call_shared, error));
    return held;
}

void sample_fingerprint_matches_the_build() {
    const Sample sample = load_sample();
    CHECK(fingerprint_text(sample) == brains::classic_sample_fingerprint);
    CHECK(brains::classic_sample_fingerprint[0] != '\0');
    Sample with_origin = sample;
    with_origin.paths.emplace_back(".oa-origin.yaml");
    with_origin.bytes.push_back({'n', 'o', 't', 'e'});
    CHECK(fingerprint_text(with_origin) == brains::classic_sample_fingerprint);
}

void classic_sample_may_draw_the_shared_stream() {
    Fixture fixture;
    const Sample sample = load_sample();
    CHECK(
        seat_sample(fixture, sample, brains::classic_sample_fingerprint).result ==
        brains::SeatResult::seated
    );
    CHECK(brains::seat_is_classic(*fixture.match, computer));
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    // The simulation draws during a tick. This snapshot is the state the call
    // itself is about to consume, after seating.
    const uint32_t before = fixture.match->random_state();
    oa::sim::match_runtime::SharedRandom copy(0);
    copy.restore(before);
    const uint32_t expect_wide = copy.bounded(900);
    const uint32_t after_wide = copy.state();
    CHECK(copy.bounded(1) == 0u);
    CHECK(copy.state() == after_wide);
    const SharedCall wide = draw_shared(*sandbox, 900);
    CHECK(wide.present);
    CHECK(wide.ran);
    CHECK(wide.value == static_cast<lua_Integer>(expect_wide));
    CHECK(fixture.match->random_state() == after_wide);
    const uint32_t parked = fixture.match->random_state();
    const SharedCall narrow = draw_shared(*sandbox, 1);
    CHECK(narrow.present);
    CHECK(narrow.ran);
    CHECK(narrow.value == 0);
    CHECK(fixture.match->random_state() == parked);
}

void changed_samples_are_not_classic() {
    const Sample sample = load_sample();
    const std::string fingerprint = brains::classic_sample_fingerprint;
    Sample changed = sample;
    for (std::size_t index = 0; index < changed.paths.size(); ++index) {
        if (changed.paths[index] == "lua/a.lua" && !changed.bytes[index].empty())
            changed.bytes[index][0] = static_cast<uint8_t>(changed.bytes[index][0] ^ 0x1u);
    }
    Sample extra = sample;
    extra.paths.emplace_back("notes.txt");
    extra.bytes.push_back({'x'});
    Sample missing = sample;
    missing.paths.pop_back();
    missing.bytes.pop_back();
    const Sample* cases[] = {&changed, &extra, &missing};
    for (const Sample* one : cases) {
        CHECK(fingerprint_text(*one) != fingerprint);
        Fixture fixture;
        CHECK(seat_sample(fixture, *one, fingerprint).result == brains::SeatResult::seated);
        CHECK(!brains::seat_is_classic(*fixture.match, computer));
        host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
        CHECK(sandbox != nullptr);
        const SharedCall shared = draw_shared(*sandbox, 900);
        CHECK(!shared.present);
    }
}

void an_id_of_classic_is_not_classic() {
    Fixture fixture;
    Package package;
    package.id = "classic";
    package.script = "local hooks = {}\nreturn hooks\n";
    CHECK(seat_script(fixture, package).result == brains::SeatResult::seated);
    CHECK(!brains::seat_is_classic(*fixture.match, computer));
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    CHECK(!draw_shared(*sandbox, 900).present);
}

void empty_builtin_matches_nothing() {
    Fixture fixture;
    const Sample sample = load_sample();
    CHECK(seat_sample(fixture, sample, {}).result == brains::SeatResult::seated);
    CHECK(!brains::seat_is_classic(*fixture.match, computer));
    CHECK(!brains::seat_is_classic(*fixture.match, 0));
    CHECK(!brains::seat_is_classic(*fixture.match, 99));
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    CHECK(!draw_shared(*sandbox, 900).present);
}

const char* checked_draws = R"(local hooks = {}
function hooks.fits()
  if brain.shared_random ~= nil then
    return "shared"
  end
  local parts = {}
  for _ = 1, 10 do
    parts[#parts + 1] = tostring(brain.random(100))
  end
  return table.concat(parts, ",")
end
return hooks
)";

void check_brain_draws_from_match_seed_zero() {
    Package package;
    package.player = 0;
    package.script = checked_draws;
    std::vector<brains::PackageFile> files;
    files.push_back(
        {"brain.lua",
         std::span<const uint8_t>(
             reinterpret_cast<const uint8_t*>(package.script.data()), package.script.size()
         )}
    );
    const std::array<char, 65> fingerprint = brains::classic_fingerprint(files);
    const std::string classic = fingerprint.data();
    brains::SeatSpec spec;
    spec.player = package.player;
    spec.id = package.id;
    spec.name = "Steady Commander";
    spec.version = package.version;
    spec.entry = package.entry;
    spec.level = "steady";
    spec.difficulty = OA_DIFFICULTY_MEDIUM;
    spec.files = files;
    spec.classic_fingerprint_for_tests = classic;
    oa::data::brain_facts::GameFacts facts;
    const brains::SeatOutcome outcome = brains::check_brain(spec, facts);
    CHECK(outcome.result == brains::SeatResult::does_not_fit);
    std::string wanted;
    for (int index = 0; index < 10; ++index) {
        if (index != 0)
            wanted += ',';
        wanted += std::to_string(golden[1].draws[index]);
    }
    CHECK(std::string(outcome.reason.data()) == wanted);
}

} // namespace

int main() {
    try {
        seeds_match_the_reference();
        draws_match_the_reference();
        replacing_math_random_leaves_brain_random();
        a_seated_brain_draws_the_match_seed();
        own_draws_leave_the_shared_stream();
        sample_fingerprint_matches_the_build();
        classic_sample_may_draw_the_shared_stream();
        changed_samples_are_not_classic();
        an_id_of_classic_is_not_classic();
        empty_builtin_matches_nothing();
        check_brain_draws_from_match_seed_zero();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "sim-brains-random: %s\n", ex.what());
        return 1;
    }
    return 0;
}
