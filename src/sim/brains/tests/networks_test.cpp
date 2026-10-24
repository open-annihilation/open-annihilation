// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Networks and grids. steady-commander is the made-up brain. ridge is the
// made-up mod. The fixture map is the computer-build map.

#include "match_fixture.hpp"

#include "internal.hpp"

#include "oa/base/sha256.hpp"
#include "oa/core/map_plot.h"
#include "oa/core/player.h"
#include "oa/core/unit.h"
#include "oa/core/unit_def.h"
#include "oa/core/world.h"
#include "oa/data/neural/network.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/formats/oann/oann.hpp"
#include "oa/sim/brains/check.hpp"
#include "oa/sim/brains/seat.hpp"
#include "oa/sim/detection.hpp"
#include "oa/sim/visibility_state.hpp"

#include "lua.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace brains = oa::sim::brains;
namespace host = oa::data::script_host;
namespace neural = oa::data::neural;
namespace oann = oa::formats::oann;

constexpr uint8_t ally_player = 2;
constexpr int32_t radar_reach = 400;
constexpr int32_t one_unit = static_cast<int32_t>(1u << 16);
constexpr uint16_t mapping_and_line_of_sight = static_cast<uint16_t>(
    sim::visibility_state::terrain_mapping | sim::visibility_state::update_sight_grid
);
constexpr const char* idle_script = "return { fits = function() return true end }\n";
constexpr const char* brain_id = "steady-commander";

struct Chunk {
    const char* source{};
};

struct IntGlobal {
    const char* name{};
    lua_Integer value{};
    bool found{};
};

struct TextGlobal {
    const char* name{};
    std::string value;
    bool found{};
};

struct Packed {
    std::string script{idle_script};
    std::vector<std::string> ids;
    std::vector<std::string> paths;
    std::vector<std::vector<uint8_t>> bytes;
};

oann::Tensor valued(
    std::string name,
    oann::Role role,
    int8_t exponent,
    std::initializer_list<uint32_t> dims,
    oann::ElementType type = oann::ElementType::int16
) {
    oann::Tensor tensor;
    tensor.name = std::move(name);
    tensor.role = role;
    tensor.type = type;
    tensor.exponent = exponent;
    tensor.rank = static_cast<uint8_t>(dims.size());
    uint8_t axis = 0;
    for (const uint32_t dim : dims) {
        tensor.dims[axis] = dim;
        ++axis;
    }
    return tensor;
}

void push_i8(std::vector<uint8_t>& data, int value) {
    data.push_back(static_cast<uint8_t>(static_cast<int8_t>(value)));
}

oann::Tensor constant_i8(
    std::string name, std::initializer_list<uint32_t> dims, std::initializer_list<int> values
) {
    oann::Tensor tensor =
        valued(std::move(name), oann::Role::constant, 0, dims, oann::ElementType::int8);
    for (const int value : values)
        push_i8(tensor.data, value);
    return tensor;
}

oann::Tensor ones_i8(std::string name, std::initializer_list<uint32_t> dims) {
    oann::Tensor tensor =
        valued(std::move(name), oann::Role::constant, 0, dims, oann::ElementType::int8);
    uint64_t count = 1;
    for (const uint32_t dim : dims)
        count *= dim;
    tensor.data.assign(static_cast<std::size_t>(count), 1);
    return tensor;
}

oann::Layer make_layer(
    oann::LayerKind kind,
    std::initializer_list<uint16_t> inputs,
    uint16_t output,
    uint16_t weights = oann::no_tensor,
    uint16_t bias = oann::no_tensor
) {
    oann::Layer layer;
    layer.kind = kind;
    layer.input_count = static_cast<uint8_t>(inputs.size());
    layer.inputs.fill(oann::no_tensor);
    uint8_t slot = 0;
    for (const uint16_t input : inputs) {
        layer.inputs[slot] = input;
        ++slot;
    }
    layer.output = output;
    layer.weights = weights;
    layer.bias = bias;
    return layer;
}

oann::Layer spatial(
    oann::LayerKind kind,
    std::initializer_list<uint16_t> inputs,
    uint16_t output,
    uint8_t kernel_h,
    uint8_t kernel_w,
    uint8_t stride_h,
    uint8_t stride_w,
    uint8_t pad_h,
    uint8_t pad_w,
    uint16_t weights
) {
    oann::Layer layer = make_layer(kind, inputs, output, weights);
    layer.kernel_h = kernel_h;
    layer.kernel_w = kernel_w;
    layer.stride_h = stride_h;
    layer.stride_w = stride_w;
    layer.pad_h = pad_h;
    layer.pad_w = pad_w;
    return layer;
}

std::vector<uint8_t> file_of(const oann::Model& model) {
    oann::Problem problem;
    const std::optional<std::vector<uint8_t>> bytes = oann::write(model, problem);
    if (!bytes)
        throw std::runtime_error("write: " + problem.message);
    const std::optional<oann::Model> back = oann::read(*bytes, problem);
    if (!back)
        throw std::runtime_error("read: " + problem.message);
    return *bytes;
}

void add_model(Packed& packed, const char* id, const char* path, const oann::Model& model) {
    packed.ids.emplace_back(id);
    packed.paths.emplace_back(path);
    packed.bytes.push_back(file_of(model));
}

void add_raw(Packed& packed, const char* id, const char* path, std::vector<uint8_t> bytes) {
    packed.ids.emplace_back(id);
    packed.paths.emplace_back(path);
    packed.bytes.push_back(std::move(bytes));
}

oann::Model scout_model() {
    oann::Model model;
    model.tensors.push_back(valued("signal", oann::Role::input, 0, {2}));
    model.tensors.push_back(constant_i8("weights", {2, 2}, {1, 0, 4, -1}));
    model.tensors.push_back(valued("scores", oann::Role::output, 0, {2}));
    model.tensors.push_back(valued("pick", oann::Role::output, 0, {1}, oann::ElementType::int32));
    model.layers.push_back(make_layer(oann::LayerKind::dense, {0}, 2, 1));
    model.layers.push_back(make_layer(oann::LayerKind::argmax, {2}, 3));
    return model;
}

oann::Model patch_model() {
    oann::Model model;
    model.tensors.push_back(valued("signal", oann::Role::input, 0, {1, 2, 2}));
    model.tensors.push_back(constant_i8("weights", {1, 1, 2, 2}, {1, 0, 0, 1}));
    model.tensors.push_back(valued("seen", oann::Role::output, 0, {1, 1, 1}));
    model.layers.push_back(spatial(oann::LayerKind::conv2d, {0}, 2, 2, 2, 1, 1, 0, 0, 1));
    return model;
}

oann::Model small_model() {
    oann::Model model;
    model.tensors.push_back(valued("signal", oann::Role::input, 0, {1}));
    model.tensors.push_back(valued("kept", oann::Role::output, 0, {1}));
    model.layers.push_back(make_layer(oann::LayerKind::relu, {0}, 1));
    return model;
}

oann::Model heavy_model() {
    oann::Model model;
    model.tensors.push_back(valued("signal", oann::Role::input, 0, {1}));
    model.tensors.push_back(ones_i8("weights", {65535, 1}));
    model.tensors.push_back(valued("scores", oann::Role::output, 0, {65535}));
    model.layers.push_back(make_layer(oann::LayerKind::dense, {0}, 2, 1));
    return model;
}

oann::Model threat_model() {
    oann::Model model;
    model.tensors.push_back(valued("signal", oann::Role::input, 0, {1}));
    model.tensors.push_back(valued("act0", oann::Role::activation, 0, {1}));
    model.tensors.push_back(valued("act1", oann::Role::activation, 0, {1}));
    model.tensors.push_back(valued("act2", oann::Role::activation, 0, {1}));
    model.tensors.push_back(valued("act3", oann::Role::activation, 0, {1}));
    model.tensors.push_back(constant_i8("rows", {4, 1}, {1, 2, 3, 4}));
    model.tensors.push_back(valued("emb", oann::Role::output, 0, {1, 1}));
    model.layers.push_back(make_layer(oann::LayerKind::relu, {0}, 1));
    model.layers.push_back(make_layer(oann::LayerKind::relu, {1}, 2));
    model.layers.push_back(make_layer(oann::LayerKind::relu, {2}, 3));
    model.layers.push_back(make_layer(oann::LayerKind::relu, {3}, 4));
    model.layers.push_back(make_layer(oann::LayerKind::embedding, {4}, 6, 5));
    return model;
}

oann::Model huge_model() {
    // A flatten of this cube is illegal: its rank-1 length would pass
    // max_dimension. Two relus keep the same three int16 tensors.
    oann::Model model;
    model.tensors.push_back(valued("pic", oann::Role::input, 0, {1, 4096, 4096}));
    model.tensors.push_back(valued("mid", oann::Role::activation, 0, {1, 4096, 4096}));
    model.tensors.push_back(valued("out", oann::Role::output, 0, {1, 4096, 4096}));
    model.layers.push_back(make_layer(oann::LayerKind::relu, {0}, 1));
    model.layers.push_back(make_layer(oann::LayerKind::relu, {1}, 2));
    return model;
}

oann::Model wide_model() {
    oann::Model model;
    model.tensors.push_back(valued("pic", oann::Role::input, 0, {1, 2014, 2014}));
    model.tensors.push_back(ones_i8("weights", {1, 1, 15, 15}));
    model.tensors.push_back(valued("out", oann::Role::output, 0, {1, 2000, 2000}));
    model.layers.push_back(spatial(oann::LayerKind::conv2d, {0}, 2, 15, 15, 1, 1, 0, 0, 1));
    return model;
}

void fill(neural::Network& network, std::initializer_list<int16_t> values) {
    const std::span<int16_t> samples = network.input(0);
    CHECK(samples.size() == values.size());
    std::size_t element = 0;
    for (const int16_t value : values)
        samples[element++] = value;
}

neural::Network built(const oann::Model& model) {
    oann::Problem problem;
    std::optional<neural::Network> network = neural::Network::build(model, problem);
    if (!network)
        throw std::runtime_error("build: " + problem.message);
    return std::move(*network);
}

void require_seated(const brains::SeatOutcome& outcome) {
    if (outcome.result == brains::SeatResult::seated)
        return;
    throw std::runtime_error(
        std::string("seat failed: ") + outcome.reason.data() + " " + outcome.error.message
    );
}

void expect_reason(const brains::SeatOutcome& outcome, const char* text) {
    if (outcome.result != brains::SeatResult::refused ||
        std::string(outcome.reason.data()) != text) {
        throw std::runtime_error(
            std::string("reason: ") + outcome.reason.data() + " " + outcome.error.message
        );
    }
}

Fixture mapped_fixture() {
    return Fixture(0, true, {}, 0, 1, {}, mapping_and_line_of_sight);
}

void use_ridge(Fixture& fixture) {
    oa::data::brain_facts::GameFacts facts;
    facts.mod.id = "ridge";
    facts.mod.name = "Ridge";
    facts.mod.version = "28";
    brains::set_match_game_facts(*fixture.match, std::move(facts));
}

brains::SeatOutcome
load_packed(Fixture& fixture, Packed& packed, bool full, const host::Bank* bank) {
    use_ridge(fixture);
    std::vector<brains::PackageFile> files;
    files.push_back(
        brains::PackageFile{
            "brain.lua",
            std::span<const uint8_t>{
                reinterpret_cast<const uint8_t*>(packed.script.data()), packed.script.size()
            }
        }
    );
    std::vector<brains::NetworkFile> networks;
    networks.reserve(packed.ids.size());
    for (std::size_t index = 0; index < packed.ids.size(); ++index) {
        files.push_back(brains::PackageFile{packed.paths[index], packed.bytes[index]});
        brains::NetworkFile network;
        network.id = packed.ids[index];
        network.path = packed.paths[index];
        network.sha256 = oa::base::sha256::digest_of(packed.bytes[index]);
        networks.push_back(network);
    }
    brains::SeatSpec spec;
    spec.player = computer;
    spec.id = brain_id;
    spec.name = "Steady Commander";
    spec.version = "1.0";
    spec.entry = "brain.lua";
    spec.level = "steady";
    spec.difficulty = OA_DIFFICULTY_MEDIUM;
    spec.full_awareness = full;
    spec.files = files;
    spec.networks = networks;
    if (bank != nullptr)
        spec.bank = *bank;
    return brains::seat_brain(*fixture.match, spec);
}

brains::Seat* require_seat(Fixture& fixture, Packed& packed, bool full, const host::Bank* bank) {
    require_seated(load_packed(fixture, packed, full, bank));
    const auto& held = fixture.match->seat_state();
    CHECK(static_cast<bool>(held));
    auto* state = static_cast<brains::Brains*>(held.get());
    CHECK(state != nullptr);
    CHECK(state->seats[computer] != nullptr);
    return state->seats[computer].get();
}

void require_closed(Fixture& fixture) {
    CHECK(!brains::brain_seated(*fixture.match, computer));
    const auto& held = fixture.match->seat_state();
    if (!held)
        return;
    auto* state = static_cast<brains::Brains*>(held.get());
    CHECK(state != nullptr);
    CHECK(state->seats[computer] == nullptr);
}

int push_chunk(void* context, lua_State* lua) {
    const auto* chunk = static_cast<const Chunk*>(context);
    if (chunk == nullptr || chunk->source == nullptr)
        return luaL_error(lua, "missing probe");
    if (luaL_loadstring(lua, chunk->source) != 0) {
        const char* message = lua_tostring(lua, -1);
        if (message == nullptr)
            message = "probe failed to load";
        return luaL_error(lua, "%s", message);
    }
    return 0;
}

void probe(host::Sandbox& sandbox, const char* source) {
    Chunk chunk{source};
    host::CallHooks hooks;
    hooks.context = &chunk;
    hooks.push = push_chunk;
    host::ScriptError error;
    if (!sandbox.call("probe", hooks, error))
        throw std::runtime_error(error.message.empty() ? "probe failed" : error.message);
}

bool banked(host::Sandbox& sandbox, const char* source, host::ScriptError& error) {
    Chunk chunk{source};
    host::CallHooks hooks;
    hooks.context = &chunk;
    hooks.push = push_chunk;
    return sandbox.call_banked("probe", hooks, error);
}

void read_int(void* context, lua_State* lua) {
    auto* box = static_cast<IntGlobal*>(context);
    lua_getglobal(lua, box->name);
    box->found = lua_isinteger(lua, -1) != 0;
    if (box->found)
        box->value = lua_tointeger(lua, -1);
    lua_pop(lua, 1);
}

void read_text(void* context, lua_State* lua) {
    auto* box = static_cast<TextGlobal*>(context);
    lua_getglobal(lua, box->name);
    box->found = lua_type(lua, -1) == LUA_TSTRING;
    if (box->found) {
        std::size_t length = 0;
        const char* text = lua_tolstring(lua, -1, &length);
        box->value.assign(text, length);
    }
    lua_pop(lua, 1);
}

lua_Integer global_int(host::Sandbox& sandbox, const char* name) {
    IntGlobal box{name, 0, false};
    host::ScriptError error;
    if (!sandbox.run_host("read-int", &box, read_int, error))
        throw std::runtime_error(error.message.empty() ? "read failed" : error.message);
    CHECK(box.found);
    return box.value;
}

std::string global_text(host::Sandbox& sandbox, const char* name) {
    TextGlobal box{name, {}, false};
    host::ScriptError error;
    if (!sandbox.run_host("read-text", &box, read_text, error))
        throw std::runtime_error(error.message.empty() ? "read failed" : error.message);
    CHECK(box.found);
    return box.value;
}

void expect_int(host::Sandbox& sandbox, const char* name, lua_Integer want) {
    const lua_Integer got = global_int(sandbox, name);
    if (got != want)
        throw std::runtime_error(std::string(name) + " got " + std::to_string(got));
}

void expect_text(host::Sandbox& sandbox, const char* name, const char* want) {
    const std::string got = global_text(sandbox, name);
    if (got != want)
        throw std::runtime_error(std::string(name) + " got " + got);
}

void expect_has(host::Sandbox& sandbox, const char* name, const char* part) {
    const std::string got = global_text(sandbox, name);
    if (got.find(part) == std::string::npos)
        throw std::runtime_error(got + " missing " + part);
}

void advance(Fixture& fixture) {
    ++fixture.match->state().game.tick;
    fixture.match->tick();
}

void lift(Fixture& fixture, sim::unit_spawn::Slot& slot, int32_t cell_x, int32_t cell_z) {
    const auto centre = [](int32_t cell) {
        return static_cast<int32_t>(static_cast<uint32_t>(cell * 16 + 8) << 16);
    };
    fixture.match->place_unit_at(slot.unit_index, centre(cell_x), one_unit, centre(cell_z), 1);
}

void give_radar(Fixture& fixture, sim::unit_spawn::Slot& slot) {
    auto& table = fixture.match->state().unit_defs[vehicle];
    table.radar_distance = static_cast<int16_t>(radar_reach);
    table.sonar_distance = 0;
    oa::UnitDef* live = oa::world_unit_def_of(&fixture.match->state(), &slot.record);
    CHECK(live != nullptr);
    live->radar_distance = static_cast<int16_t>(radar_reach);
    live->sonar_distance = 0;
    slot.state_flags = static_cast<uint8_t>(slot.state_flags | OA_UNIT_STATE_ACTIVE);
}

void arm(Fixture& fixture, sim::unit_spawn::Slot& slot) {
    oa::UnitDef* live = oa::world_unit_def_of(&fixture.match->state(), &slot.record);
    CHECK(live != nullptr);
    live->flags |= OA_UNIT_DEF_FLAG_HAS_WEAPONS;
}

void ally_with(Fixture& fixture) {
    fixture.match->simulation().players[ally_player].present = true;
    fixture.match->simulation().players[ally_player].status = OA_PLAYER_STATUS_LOCAL;
    std::array<uint8_t, 10> allies{};
    allies[computer] = 1;
    allies[ally_player] = 1;
    fixture.match->configure_player_alliances(computer, allies);
    CHECK(fixture.match->allied(computer, ally_player));
    CHECK(!fixture.match->allied(computer, 0));
}

void set_plot(Fixture& fixture, int32_t x, int32_t z, uint8_t height, uint8_t metal) {
    oa::MapPlot* plot = oa::world_plot(&fixture.match->state(), x, z);
    CHECK(plot != nullptr);
    plot->height = height;
    plot->metal = metal;
}

void fill_land(Fixture& fixture, uint8_t height, uint8_t metal) {
    for (int32_t z = 0; z < map_cells; ++z) {
        for (int32_t x = 0; x < map_cells; ++x)
            set_plot(fixture, x, z, height, metal);
    }
}

void test_direct() {
    neural::Network scout = built(scout_model());
    fill(scout, {2, 3});
    const neural::RunError scout_run = scout.run();
    CHECK(!scout_run.failed);
    CHECK(scout.output(0).size() == 2);
    CHECK(scout.output(0)[0] == 2);
    CHECK(scout.output(0)[1] == 5);
    CHECK(scout.output_index(1) == 1);
    CHECK(scout.charge_units() == 1);

    neural::Network patch = built(patch_model());
    fill(patch, {1, 2, 3, 4});
    CHECK(!patch.run().failed);
    CHECK(patch.output(0).size() == 1);
    CHECK(patch.output(0)[0] == 5);

    neural::Network threat = built(threat_model());
    fill(threat, {70});
    const neural::RunError missed = threat.run();
    CHECK(missed.failed);
    CHECK(missed.layer == 4);
    CHECK(missed.row == 70);

    const oann::Model huge = huge_model();
    CHECK(neural::Network::memory_bytes_of(huge) == 100663296);
    const std::vector<uint8_t> huge_file = file_of(huge);
    CHECK(huge_file.size() < 4096);
    neural::Network held = built(huge);
    CHECK(held.memory_bytes() == 100663296);

    oann::Problem problem;
    const std::optional<neural::Network> wide = neural::Network::build(wide_model(), problem);
    CHECK(!wide);
    CHECK(problem.message == "a run would take 900000000 operations; a run may take 268435456");
    CHECK(neural::Network::memory_bytes_of(wide_model()) < neural::max_brain_network_bytes);

    std::vector<uint8_t> junk(oann::header_bytes, 0xFF);
    const std::optional<oann::Model> broken = oann::read(junk, problem);
    CHECK(!broken);
    CHECK(problem.message == "the magic is not OANN");
}

void test_refusals() {
    {
        Fixture fixture;
        Packed packed;
        add_raw(
            packed, "broken", "nets/broken.oann", std::vector<uint8_t>(oann::header_bytes, 0xFF)
        );
        expect_reason(
            load_packed(fixture, packed, false, nullptr), "network broken: the magic is not OANN"
        );
        require_closed(fixture);
    }
    {
        Fixture fixture;
        Packed packed;
        add_model(packed, "wide", "nets/wide.oann", wide_model());
        expect_reason(
            load_packed(fixture, packed, false, nullptr),
            "network wide: a run would take 900000000 operations; a run may take 268435456"
        );
        require_closed(fixture);
    }
    {
        Fixture fixture;
        Packed packed;
        add_model(packed, "huge", "nets/huge.oann", huge_model());
        expect_reason(
            load_packed(fixture, packed, false, nullptr),
            "network huge: networks would take 100663296 bytes; a brain may hold 67108864"
        );
        require_closed(fixture);
    }
}

void test_lua_networks() {
    Fixture fixture = mapped_fixture();
    Packed packed;
    add_model(packed, "scout", "nets/scout.oann", scout_model());
    add_model(packed, "patch", "nets/seen.oann", patch_model());
    brains::Seat* seat = require_seat(fixture, packed, false, nullptr);
    CHECK(seat->networks != nullptr);
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    probe(
        *sandbox,
        "local net = brain.network('scout')\n"
        "local again = brain.network('scout')\n"
        "same = rawequal(net, again) and 1 or 0\n"
        "cost = net:cost()\n"
        "local inputs = net:inputs()\n"
        "local outputs = net:outputs()\n"
        "port_n = #inputs * 10 + #outputs\n"
        "port_text = inputs[1] .. ' ' .. outputs[1] .. ' ' .. outputs[2]\n"
        "local out = net:run({signal = brain.tensor({2, 3})})\n"
        "raw0 = out.scores:get(1):raw()\n"
        "raw1 = out.scores:get(2):raw()\n"
        "pick = out.pick\n"
        "score_text = tostring(out.scores)\n"
        "local seen = brain.network('patch'):run({signal = brain.tensor({1, 2, 3, 4})})\n"
        "seen_raw = seen.seen:get(1):raw()\n"
        "seen_text = tostring(seen.seen)\n"
    );
    expect_int(*sandbox, "same", 1);
    expect_int(*sandbox, "cost", 1);
    expect_int(*sandbox, "port_n", 12);
    expect_text(*sandbox, "port_text", "signal scores pick");
    expect_int(*sandbox, "raw0", 2 * 65536);
    expect_int(*sandbox, "raw1", 5 * 65536);
    expect_int(*sandbox, "pick", 2);
    expect_text(*sandbox, "score_text", "tensor: 2");
    expect_int(*sandbox, "seen_raw", 5 * 65536);
    expect_text(*sandbox, "seen_text", "tensor: 1x1x1");
}

void test_tensors() {
    Fixture fixture;
    Packed packed;
    brains::Seat* seat = require_seat(fixture, packed, false, nullptr);
    CHECK(seat->networks == nullptr);
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    probe(
        *sandbox,
        "local function refused(body)\n"
        "  local ok, err = pcall(body)\n"
        "  if ok then return 'unexpected success' end\n"
        "  return tostring(err)\n"
        "end\n"
        "local t = brain.tensor({1, 4, 4, -1}, {2, 2})\n"
        "if t:argmax() ~= 2 then error('argmax') end\n"
        "if t:size() ~= 4 or #t ~= 4 then error('size') end\n"
        "if t:get(1):raw() ~= 65536 then error('get') end\n"
        "if t:totable()[2]:raw() ~= 262144 then error('totable') end\n"
        "local shape = t:shape()\n"
        "if #shape ~= 2 or shape[1] ~= 2 or shape[2] ~= 2 then error('shape') end\n"
        "text = tostring(t)\n"
        "local half = brain.tensor({1.5, -1.5, 1.9, -1.9})\n"
        "if half:get(1):raw() ~= 98304 then error('1.5') end\n"
        "if half:get(2):raw() ~= -98304 then error('-1.5') end\n"
        "if half:get(3):raw() ~= 124518 then error('1.9') end\n"
        "if half:get(4):raw() ~= -124518 then error('-1.9') end\n"
        "local kept = brain.tensor({fixed(1.5)})\n"
        "if kept:get(1):raw() ~= fixed(1.5):raw() then error('fixed') end\n"
        "if brain.tensor({32767}):get(1):raw() ~= 32767 * 65536 then error('hi') end\n"
        "if brain.tensor({-32768}):get(1):raw() ~= -2147483648 then error('lo') end\n"
        "whole = brain.tensor({2}):get(1):raw()\n"
        "hi_note = refused(function() brain.tensor({32768}) end)\n"
        "lo_note = refused(function() brain.tensor({-32769}) end)\n"
        "nan_note = refused(function() brain.tensor({0/0}) end)\n"
        "inf_note = refused(function() brain.tensor({1/0}) end)\n"
        "shape_note = refused(function() brain.tensor({1, 2, 3}, {2, 2}) end)\n"
        "zero_note = refused(function() brain.tensor({1}, {0}) end)\n"
        "empty_note = refused(function() brain.tensor({}) end)\n"
        "text_note = refused(function() brain.tensor({'a'}) end)\n"
        "index_note = refused(function() t:get(0) end)\n"
        "frozen = refused(function() t[1] = 1 end)\n"
        "miss = refused(function() brain.network('missing') end)\n"
        "if brain.tensor({1}):size() ~= 1 then error('after miss') end\n"
    );
    expect_text(*sandbox, "text", "tensor: 2x2");
    expect_int(*sandbox, "whole", 2 * 65536);
    expect_has(*sandbox, "hi_note", "brain.tensor: value 1 is outside -32768 to 32767");
    expect_has(*sandbox, "lo_note", "brain.tensor: value 1 is outside -32768 to 32767");
    expect_has(*sandbox, "nan_note", "brain.tensor: value 1 is not a finite number");
    expect_has(*sandbox, "inf_note", "brain.tensor: value 1 is not a finite number");
    expect_has(*sandbox, "shape_note", "brain.tensor: the shape covers 4 values, not 3");
    expect_has(*sandbox, "zero_note", "brain.tensor: the shape must be 1 to 4 positive integers");
    expect_has(*sandbox, "empty_note", "brain.tensor: expects 1 to 4194304 numbers");
    expect_has(*sandbox, "text_note", "brain.tensor: value 1 is not a number");
    expect_has(*sandbox, "index_note", "brain.tensor: index 0 is outside 1 to 4");
    expect_has(*sandbox, "frozen", "a tensor cannot be changed");
    expect_has(*sandbox, "miss", "brain.network: this brain has no network 'missing'");
}

void test_run_errors() {
    Fixture fixture;
    Packed packed;
    add_model(packed, "scout", "nets/scout.oann", scout_model());
    require_seat(fixture, packed, false, nullptr);
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    probe(
        *sandbox,
        "local function refused(body)\n"
        "  local ok, err = pcall(body)\n"
        "  if ok then return 'unexpected success' end\n"
        "  return tostring(err)\n"
        "end\n"
        "local net = brain.network('scout')\n"
        "local signal = brain.tensor({2, 3})\n"
        "unknown = refused(function() net:run({zebra = signal, apple = signal}) end)\n"
        "keys = refused(function() net:run({signal = signal, [true] = 1}) end)\n"
        "missing = refused(function() net:run({}) end)\n"
        "kind = refused(function() net:run({signal = 1}) end)\n"
        "size_note = refused(function() net:run({signal = brain.tensor({1})}) end)\n"
        "table_note = refused(function() net:run(1) end)\n"
        "local run = net.run\n"
        "bare = refused(function() run({}) end)\n"
        "frozen = refused(function() net.extra = 1 end)\n"
    );
    expect_has(*sandbox, "unknown", "network scout: unknown input 'apple'");
    expect_has(*sandbox, "keys", "network scout: input names must be strings");
    expect_has(*sandbox, "missing", "network scout: missing input 'signal'");
    expect_has(*sandbox, "kind", "network scout: input 'signal' must be a tensor");
    expect_has(*sandbox, "size_note", "network scout: input 'signal' has 1 elements, not 2");
    expect_has(*sandbox, "table_note", "network scout: run expects a table of inputs");
    expect_has(*sandbox, "bare", "run expects a network");
    expect_has(*sandbox, "frozen", "a network cannot be changed");

    Fixture threat_fixture;
    Packed threat;
    add_model(threat, "threat", "nets/threat.oann", threat_model());
    require_seat(threat_fixture, threat, false, nullptr);
    host::Sandbox* threat_box = brains::seat_sandbox(*threat_fixture.match, computer);
    CHECK(threat_box != nullptr);
    probe(
        *threat_box,
        "local ok, err = pcall(function()\n"
        "  brain.network('threat'):run({signal = brain.tensor({70})})\n"
        "  done = 1\n"
        "end)\n"
        "if ok then error('embedding should fail') end\n"
        "note = tostring(err)\n"
        "if done ~= nil then error('result existed') end\n"
    );
    expect_has(*threat_box, "note", "network threat: layer 4 (embedding) has no row 70");
}

void test_charges() {
    const host::Bank tight{5500, 1, host::default_call_time_limit};
    const host::Bank roomy{20000, 1, host::default_call_time_limit};
    {
        Fixture fixture;
        Packed packed;
        add_model(packed, "small", "nets/small.oann", small_model());
        require_seat(fixture, packed, false, &tight);
        host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
        CHECK(sandbox != nullptr);
        host::ScriptError error;
        const bool ran = banked(
            *sandbox,
            "local out = brain.network('small'):run({signal = brain.tensor({3})})\n"
            "done = out.kept:get(1):raw()\n",
            error
        );
        if (!ran)
            throw std::runtime_error(error.message.empty() ? "small run failed" : error.message);
        expect_int(*sandbox, "done", 3 * 65536);
    }
    {
        Fixture fixture;
        Packed packed;
        add_model(packed, "heavy", "nets/heavy.oann", heavy_model());
        require_seat(fixture, packed, false, &tight);
        host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
        CHECK(sandbox != nullptr);
        host::ScriptError error;
        const bool ran = banked(
            *sandbox,
            "local out = brain.network('heavy'):run({signal = brain.tensor({1})})\n"
            "done = 1\n",
            error
        );
        CHECK(!ran);
        CHECK(error.kind == host::ErrorKind::instruction_budget);
        CHECK(error.message.find("allowance") != std::string::npos);
        CHECK(sandbox->failed());
    }
    {
        Fixture fixture;
        Packed packed;
        require_seat(fixture, packed, false, &roomy);
        host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
        CHECK(sandbox != nullptr);
        host::ScriptError error;
        // At most 40 bytes, so the chunk name is a short string. A longer
        // source is charged once, when the string cache first keeps it, and
        // the second call would then spend less.
        const char* script = "return brain.tensor{1,2,3}:argmax()";
        CHECK(banked(*sandbox, script, error));
        const uint64_t first = sandbox->instructions_used();
        CHECK(banked(*sandbox, script, error));
        const uint64_t second = sandbox->instructions_used();
        CHECK(first > 0);
        CHECK(first == second);
    }
}

void place_battle(Fixture& fixture) {
    ally_with(fixture);
    auto& own = fixture.spawn(computer, vehicle, 4, 4);
    auto& frame = fixture.spawn(computer, vehicle, 4, 6);
    auto& ally = fixture.spawn(ally_player, vehicle, 5, 4);
    auto& enemy = fixture.spawn(0, vehicle, 4, 5);
    auto& armed = fixture.spawn(0, silo, 6, 4);
    auto& far = fixture.spawn(0, vehicle, 40, 8);
    auto& tower = fixture.spawn(computer, vehicle, 8, 8);
    auto& blip = fixture.spawn(0, vehicle, 32, 8);
    lift(fixture, own, 4, 4);
    lift(fixture, frame, 4, 6);
    lift(fixture, ally, 5, 4);
    lift(fixture, enemy, 4, 5);
    lift(fixture, armed, 6, 4);
    lift(fixture, far, 40, 8);
    lift(fixture, tower, 8, 8);
    lift(fixture, blip, 32, 8);
    give_radar(fixture, tower);
    arm(fixture, armed);
    oa::UnitDef* vehicle_def = oa::world_unit_def_of(&fixture.match->state(), &enemy.record);
    CHECK(vehicle_def != nullptr);
    CHECK((vehicle_def->flags & OA_UNIT_DEF_FLAG_HAS_WEAPONS) == 0);
}

void test_grids() {
    const char* layers =
        "local g = brain.grid({'own', 'allies', 'enemies', 'threat', 'radar'}, 64)\n"
        "local function at(layer, row, col)\n"
        "  return g:get(1 + layer * 64 * 64 + row * 64 + col):raw()\n"
        "end\n"
        "own_a = at(0, 4, 4)\n"
        "own_b = at(0, 6, 4)\n"
        "ally_self = at(1, 4, 4)\n"
        "ally_n = at(1, 4, 5)\n"
        "enemy_n = at(2, 5, 4)\n"
        "enemy_silo = at(2, 4, 6)\n"
        "enemy_far = at(2, 8, 40)\n"
        "enemy_blip = at(2, 8, 32)\n"
        "threat_silo = at(3, 4, 6)\n"
        "threat_vehicle = at(3, 5, 4)\n"
        "radar_blip = at(4, 8, 32)\n"
        "radar_seen = at(4, 5, 4)\n"
        "radar_far = at(4, 8, 40)\n";
    {
        Fixture fixture = mapped_fixture();
        place_battle(fixture);
        Packed packed;
        require_seat(fixture, packed, false, nullptr);
        advance(fixture);
        bool found = false;
        const oa::World& world = fixture.match->state();
        for (uint32_t slot = 0; slot < world.unit_slot_count; ++slot) {
            oa::Unit& unit = world.units[slot];
            if (unit.owner_index == computer && unit.type_index == vehicle) {
                const int32_t x = unit.position.x >> 16;
                const int32_t z = unit.position.z >> 16;
                if (x == 4 * 16 + 8 && z == 6 * 16 + 8) {
                    unit.build_remaining = 1.0F;
                    CHECK(unit.type_index != 0);
                    CHECK(oa::unit_is_live_target(unit.flags));
                    found = true;
                }
            }
        }
        CHECK(found);
        const auto picture = fixture.match->player_contact_bits(computer);
        uint16_t blip = 0;
        uint16_t seen = 0;
        bool have_blip = false;
        bool have_seen = false;
        for (uint32_t slot = 0; slot < world.unit_slot_count && slot < picture.size(); ++slot) {
            const oa::Unit& unit = world.units[slot];
            if (unit.type_index != vehicle || unit.owner_index != 0)
                continue;
            const int32_t x = unit.position.x >> 16;
            const int32_t z = unit.position.z >> 16;
            if (x == 32 * 16 + 8 && z == 8 * 16 + 8) {
                blip = static_cast<uint16_t>(slot);
                have_blip = true;
            }
            if (x == 4 * 16 + 8 && z == 5 * 16 + 8) {
                seen = static_cast<uint16_t>(slot);
                have_seen = true;
            }
        }
        CHECK(have_blip && have_seen);
        CHECK((picture[blip] & sim::detection::picture_radar) != 0);
        CHECK(!fixture.match->unit_visible(computer, blip));
        CHECK(fixture.match->unit_visible(computer, seen));
        host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
        CHECK(sandbox != nullptr);
        probe(*sandbox, layers);
        expect_int(*sandbox, "own_a", 65536);
        expect_int(*sandbox, "own_b", 65536);
        expect_int(*sandbox, "ally_self", 0);
        expect_int(*sandbox, "ally_n", 65536);
        expect_int(*sandbox, "enemy_n", 65536);
        expect_int(*sandbox, "enemy_silo", 65536);
        expect_int(*sandbox, "enemy_far", 0);
        expect_int(*sandbox, "enemy_blip", 0);
        expect_int(*sandbox, "threat_silo", 65536);
        expect_int(*sandbox, "threat_vehicle", 0);
        expect_int(*sandbox, "radar_blip", 65536);
        expect_int(*sandbox, "radar_seen", 0);
        expect_int(*sandbox, "radar_far", 0);

        probe(
            *sandbox,
            "local g = brain.grid({'sight', 'explored'}, 32)\n"
            "sight_bad = 0\n"
            "explored_bad = 0\n"
            "seen_on = 0\n"
            "for z = 0, 31 do\n"
            "  for x = 0, 31 do\n"
            "    local s = g:get(1 + z * 32 + x):raw()\n"
            "    local e = g:get(1 + 32 * 32 + z * 32 + x):raw()\n"
            "    local want_s = me.sight:at(x, z) and 65536 or 0\n"
            "    local want_e = me.explored:at(x, z) and 65536 or 0\n"
            "    if s ~= want_s then sight_bad = sight_bad + 1 end\n"
            "    if e ~= want_e then explored_bad = explored_bad + 1 end\n"
            "    if s == 65536 then seen_on = seen_on + 1 end\n"
            "  end\n"
            "end\n"
            "anchor = me.sight:at(2, 2) and 1 or 0\n"
        );
        expect_int(*sandbox, "sight_bad", 0);
        expect_int(*sandbox, "explored_bad", 0);
        CHECK(global_int(*sandbox, "seen_on") > 0);
        expect_int(*sandbox, "anchor", 1);
    }
    {
        Fixture fixture = mapped_fixture();
        place_battle(fixture);
        Packed packed;
        require_seat(fixture, packed, true, nullptr);
        advance(fixture);
        host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
        CHECK(sandbox != nullptr);
        probe(*sandbox, layers);
        expect_int(*sandbox, "enemy_far", 65536);
        expect_int(*sandbox, "enemy_blip", 65536);
        expect_int(*sandbox, "radar_blip", 65536);
        expect_int(*sandbox, "radar_far", 0);
        expect_int(*sandbox, "threat_vehicle", 0);
        expect_int(*sandbox, "ally_self", 0);
    }
}

void test_terrain() {
    Fixture fixture;
    Packed packed;
    require_seat(fixture, packed, false, nullptr);
    fixture.match->state().game.sea_level = 5;
    fill_land(fixture, 0, 0);
    set_plot(fixture, 3, 5, 9, 4);
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    probe(
        *sandbox,
        "local g = brain.grid({'water', 'height', 'metal'})\n"
        "label = tostring(g)\n"
        "local function at(layer, row, col)\n"
        "  return g:get(1 + layer * 64 * 64 + row * 64 + col):raw()\n"
        "end\n"
        "water0 = at(0, 0, 0)\n"
        "height0 = at(1, 0, 0)\n"
        "metal0 = at(2, 0, 0)\n"
        "water1 = at(0, 5, 3)\n"
        "height1 = at(1, 5, 3)\n"
        "metal1 = at(2, 5, 3)\n"
        "local function refused(body)\n"
        "  local ok, err = pcall(body)\n"
        "  if ok then return 'unexpected success' end\n"
        "  return tostring(err)\n"
        "end\n"
        "unknown = refused(function() brain.grid({'foo'}) end)\n"
        "repeated = refused(function() brain.grid({'sight', 'sight'}) end)\n"
        "sized = refused(function() brain.grid({'sight'}, 48) end)\n"
        "empty = refused(function() brain.grid({}) end)\n"
    );
    expect_text(*sandbox, "label", "tensor: 3x64x64");
    expect_int(*sandbox, "water0", 65536);
    expect_int(*sandbox, "height0", 0);
    expect_int(*sandbox, "metal0", 0);
    expect_int(*sandbox, "water1", 0);
    expect_int(*sandbox, "height1", 9 * 65536);
    expect_int(*sandbox, "metal1", 4 * 65536);
    expect_has(*sandbox, "unknown", "brain.grid: unknown layer 'foo'");
    expect_has(*sandbox, "repeated", "brain.grid: layer 'sight' is repeated");
    expect_has(*sandbox, "sized", "brain.grid: size must be 8, 16, 32, 64 or 128");
    expect_has(*sandbox, "empty", "brain.grid: expects 1 to 10 layer names");

    fill_land(fixture, 1, 0);
    set_plot(fixture, 0, 0, 9, 4);
    probe(
        *sandbox,
        "local function cell(size, row, col, layer)\n"
        "  local g = brain.grid({'height', 'metal'}, size)\n"
        "  return g:get(1 + layer * size * size + row * size + col):raw()\n"
        "end\n"
        "h00 = cell(8, 0, 0, 0)\n"
        "m00 = cell(8, 0, 0, 1)\n"
        "h11 = cell(8, 1, 1, 0)\n"
        "m11 = cell(8, 1, 1, 1)\n"
        "a00 = cell(128, 0, 0, 0)\n"
        "a11 = cell(128, 1, 1, 0)\n"
        "a22 = cell(128, 2, 2, 0)\n"
        "b00 = cell(128, 0, 0, 1)\n"
        "b22 = cell(128, 2, 2, 1)\n"
    );
    expect_int(*sandbox, "h00", 73728);
    expect_int(*sandbox, "m00", 4096);
    expect_int(*sandbox, "h11", 65536);
    expect_int(*sandbox, "m11", 0);
    expect_int(*sandbox, "a00", 9 * 65536);
    expect_int(*sandbox, "a11", 9 * 65536);
    expect_int(*sandbox, "a22", 65536);
    expect_int(*sandbox, "b00", 4 * 65536);
    expect_int(*sandbox, "b22", 0);
}

void test_check() {
    Packed packed;
    packed.script = "return { fits = function()\n"
                    "  local out = brain.network('scout'):run({signal = brain.tensor({2, 3})})\n"
                    "  if out.scores:get(1):raw() ~= 131072 then return 'scores0' end\n"
                    "  if out.scores:get(2):raw() ~= 327680 then return 'scores1' end\n"
                    "  if out.pick ~= 2 then return 'pick' end\n"
                    "  if brain.grid({'no-such'}, 48) ~= nil then return 'grid' end\n"
                    "  if game.mod.id ~= 'ridge' then return 'mod' end\n"
                    "  if game.in_match ~= false then return 'match' end\n"
                    "  return true\n"
                    "end }\n";
    add_model(packed, "scout", "nets/scout.oann", scout_model());
    std::vector<brains::PackageFile> files;
    files.push_back(
        brains::PackageFile{
            "brain.lua",
            std::span<const uint8_t>{
                reinterpret_cast<const uint8_t*>(packed.script.data()), packed.script.size()
            }
        }
    );
    std::vector<brains::NetworkFile> networks;
    for (std::size_t index = 0; index < packed.ids.size(); ++index) {
        files.push_back(brains::PackageFile{packed.paths[index], packed.bytes[index]});
        brains::NetworkFile network;
        network.id = packed.ids[index];
        network.path = packed.paths[index];
        network.sha256 = oa::base::sha256::digest_of(packed.bytes[index]);
        networks.push_back(network);
    }
    brains::SeatSpec spec;
    spec.player = computer;
    spec.id = brain_id;
    spec.name = "Steady Commander";
    spec.version = "1.0";
    spec.entry = "brain.lua";
    spec.level = "steady";
    spec.difficulty = OA_DIFFICULTY_MEDIUM;
    spec.files = files;
    spec.networks = networks;
    oa::data::brain_facts::GameFacts facts;
    facts.mod.id = "ridge";
    facts.mod.name = "Ridge";
    facts.mod.version = "28";
    require_seated(brains::check_brain(spec, facts));
}

} // namespace

int main() {
    try {
        test_direct();
        test_refusals();
        test_lua_networks();
        test_tensors();
        test_run_errors();
        test_charges();
        test_grids();
        test_terrain();
        test_check();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
    return 0;
}
