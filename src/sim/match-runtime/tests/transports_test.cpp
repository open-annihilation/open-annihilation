// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/air/flight.hpp"
#include "oa/base/game_math.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/sim/unit_script.hpp"
#include "oa/test/game_assets.hpp"
#include "installed_units.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>
#include "oa/test/match_services.hpp"
using namespace oa;
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(std::string("line ") + std::to_string(__LINE__) + ": " #x);   \
    } while (false)

// Transports: an air transport picks a unit up, carries it on its link piece
// and sets it down at an unload point; a hover transport does the same through
// its TransportPickup and TransportDrop scripts. Run with --data, the same
// runs repeat with the installed game's ARMATLAS, ARMTHOVR and ARMPW models
// and scripts, and the installed transport ships, Valkyrie, Bear and Atlas
// load, carry and lose tanks on a map of sea, shelf and land.
namespace {
namespace op = sim::script_vm::opcode;
constexpr uint8_t vtol_pickup = 57, vtol_unload = 65, be_carried = 11, ground_pickup = 20,
                  ground_unload = 21;
constexpr uint16_t air_transport_type = 1, cargo_type = 2, heavy_type = 3, hover_transport_type = 4,
                   gun_type = 5, flak_type = 6, pad_type = 7;
constexpr size_t type_count = 8;
constexpr uint8_t vtol_landing = sim::ground_orders::vtol_landing_kind;
constexpr uint8_t move_ground = sim::ground_orders::move_ground_kind;
constexpr uint32_t speech_acknowledged = 5;
constexpr int32_t map_cells = 64;
constexpr uint16_t sight_cells = 5; // 32-unit sight cells mapped around a unit
constexpr uint32_t speech_failed = 7;
constexpr uint32_t speech_loading = 0x0c;
constexpr uint32_t speech_unloaded = 0x0d;
constexpr uint8_t order_retry = 0x80; // order flags, set when a mission returns 9
constexpr int32_t one = 1 << 16;      // one world unit, 16.16

// One speech a unit made with its order's own caption.
struct Caption {
    uint16_t unit{};
    uint32_t category{};
    std::string text;
};

struct Services : oa::test::QuietServices {
    std::vector<std::pair<uint16_t, uint32_t>> speech;
    std::vector<Caption> captions;
    sim::match_runtime::Match* match{};

    void command_sound(sim::unit_spawn::Slot& slot, uint32_t category) override {
        speech.emplace_back(slot.unit_index, category);
    }

    /// The match's speech hook: records the caption and counts the speech.
    static void
    speak(void* context, sim::unit_spawn::Slot& slot, uint32_t category, const char* caption) {
        auto& services = *static_cast<Services*>(context);
        services.captions.push_back({slot.unit_index, category, caption});
        services.speech.emplace_back(slot.unit_index, category);
    }

    size_t captioned(uint16_t unit, uint32_t category, std::string_view text) const {
        size_t count = 0;
        for (const auto& caption : captions)
            count += caption.unit == unit && caption.category == category && caption.text == text;
        return count;
    }

    size_t spoken(uint16_t unit, uint32_t category) const {
        size_t count = 0;
        for (const auto& [who, what] : speech)
            count += who == unit && what == category;
        return count;
    }

    bool spoke(uint16_t unit, uint32_t category) const { return spoken(unit, category) != 0; }

    void
    attach_unit(sim::unit_spawn::Slot& slot, int32_t target, int32_t piece, int32_t mode) override {
        match->script_attach_unit(slot.unit_index, target, piece, mode);
    }

    void drop_unit(sim::unit_spawn::Slot& slot, int32_t target) override {
        match->script_drop_unit(slot.unit_index, target);
    }
};

using Scenario = oa::test::EmptyScenario;

struct Function {
    const char* name{};
    std::vector<uint32_t> code;
};

// Function bodies may jump to `end_label`, the offset of their own final word
// pair; assemble resolves it once the function's entry is known.
constexpr uint32_t end_label = 0xffffffffu;

std::shared_ptr<formats::cob::CobProgram> assemble(const std::vector<Function>& functions) {
    auto cob = std::make_shared<formats::cob::CobProgram>();
    for (const auto& function : functions) {
        const auto entry = static_cast<uint32_t>(cob->code.size());
        cob->scripts.push_back({function.name, entry});
        cob->entry_points.push_back(entry);
        for (const auto word : function.code)
            cob->code.push_back(
                word == end_label ? entry + static_cast<uint32_t>(function.code.size()) - 3 : word
            );
    }
    cob->header.script_count = static_cast<uint32_t>(functions.size());
    cob->header.static_variable_count = 2;
    cob->piece_names = {"base", "link"};
    return cob;
}

const std::vector<uint32_t> finish{op::push_constant, 0, op::return_};

std::vector<uint32_t> join(std::initializer_list<std::vector<uint32_t>> parts) {
    std::vector<uint32_t> code;
    for (const auto& part : parts)
        code.insert(code.end(), part.begin(), part.end());
    return code;
}

using Value = sim::unit_script::UnitValue;

uint32_t value(Value selector) {
    return static_cast<uint32_t>(selector);
}

// get <selector>(<operand pushed by `argument`>, 0, 0, 0)
std::vector<uint32_t> get(Value selector, std::vector<uint32_t> argument) {
    return join(
        {{op::push_constant, value(selector)},
         argument,
         {op::push_constant, 0, op::push_constant, 0, op::push_constant, 0, op::get}}
    );
}

std::vector<uint32_t> set_busy(uint32_t on) {
    return {op::push_constant, value(Value::busy), op::push_constant, on, op::set_unit_value};
}

std::vector<uint32_t> sleep_ms(uint32_t milliseconds) {
    return {op::push_constant, milliseconds, op::sleep};
}

std::vector<uint32_t> count_in_static(uint32_t index) {
    return {op::push_static, index, op::push_constant, 1, op::add, op::pop_static, index};
}

// attach-unit of the unit id argument to the link piece.
const std::vector<uint32_t> attach_argument{
    op::push_local, 0, op::push_constant, 1, op::push_constant, 0, op::attach_unit
};

// A query answering statics 0 and 1 in locals 0 and 1.
const std::vector<uint32_t> answer_statics{
    op::push_static,
    0,
    op::pop_local,
    0,
    op::push_static,
    1,
    op::pop_local,
    1,
    op::push_constant,
    0,
    op::return_
};

// Skips to the end unless the XZ distance between the two packed points is
// below `reach` world units.
std::vector<uint32_t>
unless_within(std::vector<uint32_t> first, std::vector<uint32_t> second, uint32_t reach) {
    return join(
        {{op::push_constant, value(Value::xz_hypot)},
         first,
         second,
         {op::subtract, op::push_constant, 0, op::push_constant, 0, op::push_constant, 0, op::get},
         {op::push_constant, reach << 16, op::less, op::jump_if_false, end_label}}
    );
}

std::array<uint32_t, 3> at(int32_t x, int32_t z) {
    return {static_cast<uint32_t>(x) << 16, 0, static_cast<uint32_t>(z) << 16};
}

sim::ground_orders::Point point(int32_t x, int32_t z) {
    return {x << 16, 0, z << 16};
}

int32_t world_x(const oa::Unit& unit) {
    return unit.position.x >> 16;
}

int32_t world_z(const oa::Unit& unit) {
    return unit.position.z >> 16;
}

// XZ distance in world units.
double planar(const oa::Unit& unit, int32_t x, int32_t z) {
    return std::hypot(
        (unit.position.x - (x << 16)) / 65536.0, (unit.position.z - (z << 16)) / 65536.0
    );
}

// Statics 0 and 1 of a unit's script, through its answer_statics query.
std::array<int32_t, 4> statics_of(sim::match_runtime::Match& match, uint16_t unit) {
    std::array<int32_t, 4> values{};
    CHECK(match.instance(unit)->script()->query("Statics", values));
    return values;
}

uint8_t kind_of(const sim::unit_spawn::Slot& slot) {
    return slot.unit->primary ? slot.unit->primary->kind : 0;
}

// Whether the unit is on a spatial bucket chain, where area searches find it.
bool in_buckets(sim::match_runtime::Match& match, uint16_t index) {
    match.prepare_spatial_state();
    const auto& world = match.spatial();
    const auto chained = [&](const sim::spatial_state::Bucket& bucket) {
        size_t steps = 0;
        for (auto id = bucket.head;
             id != sim::spatial_state::no_unit && steps++ < world.units.size();
             id = world.units[id].next_in_bucket)
            if (id == index)
                return true;
        return false;
    };
    for (const auto& bucket : world.buckets)
        if (chained(bucket))
            return true;
    return chained(world.outside_bucket);
}

// The map cell a footprint centred on the unit covers first.
int32_t cell_of(int32_t position, int16_t footprint) {
    return (position - footprint * 0x80000 + 0x80000) >> 20;
}

// Whether a hover transport with a two-cell footprint stands where its
// Ground_Unload close-in goal ends: 1.5 times its Z extent of 32 is 48, so its
// footprint origin cell is within three cells of the point's.
bool within_reach_cells(const oa::Unit& unit, int32_t x, int32_t z) {
    const auto dx = cell_of(unit.position.x, 2) - cell_of(x << 16, 2);
    const auto dz = cell_of(unit.position.z, 2) - cell_of(z << 16, 2);
    return dx * dx + dz * dz <= 3 * 3;
}

/// Loads a 3DO model of the installed game.
///
/// Throws through CHECK when nothing provides the file.
///
/// @param assets the installed game's store
/// @param path '/'-separated path of the model
/// @return the model
std::shared_ptr<formats::objects3d::Model>
installed_model(const AssetStore& assets, const std::string& path) {
    const auto bytes = test::read_game_file(assets, path);
    CHECK(!bytes.empty());
    return std::make_shared<formats::objects3d::Model>(
        formats::objects3d::load_3do(std::as_bytes(std::span(bytes))).value.value()
    );
}

/// Loads a COB unit script of the installed game.
///
/// Throws through CHECK when the file is missing or does not parse.
///
/// @param assets the installed game's store
/// @param path '/'-separated path of the script
/// @return the parsed program
std::shared_ptr<formats::cob::CobProgram>
installed_script(const AssetStore& assets, const std::string& path) {
    auto parsed = formats::cob::parse_cob(test::read_game_file(assets, path));
    CHECK(parsed.ok());
    return std::make_shared<formats::cob::CobProgram>(std::move(*parsed.value));
}

struct Fixture {
    formats::tnt::Map map;
    std::vector<sim::visibility_state::TerrainCell> terrain_values;
    std::array<sim::visibility_state::SightMask, 1> masks{};
    std::shared_ptr<formats::objects3d::Model> model =
        std::make_shared<formats::objects3d::Model>();
    std::shared_ptr<formats::objects3d::Model> crane =
        std::make_shared<formats::objects3d::Model>();
    std::shared_ptr<formats::objects3d::Model> deck = std::make_shared<formats::objects3d::Model>();
    std::shared_ptr<formats::cob::CobProgram> plain, carrier, loader, pad;
    std::array<sim::unit_spawn::LoadedType, type_count> loaded;
    std::array<sim::unit_spawn::Type, type_count> types;
    std::array<data::unit_definitions::UnitDefinition, type_count> defs;
    std::array<sim::match_runtime::RuntimeTypeFields, type_count> fields{};
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    data::unit_definitions::RuntimeDefinitionMetadata metadata;
    std::array<uint8_t, 1> yard{4};
    std::vector<sim::spatial_state::Plot> collision_plots;
    sim::combat_state::WeaponRegistry weapons;
    Services services;
    Scenario scenario;
    std::unique_ptr<sim::match_runtime::Match> match;
    uint32_t clock = 1;
    bool speech = true; ///< the match's speech with captions reaches Services::speak

    /// Builds the match.
    ///
    /// @param installed the installed game's store for its models and
    ///        scripts, or null for the made-up ones
    /// @param speech_hooks whether the match's speech with captions reaches
    ///        Services::speak rather than command_sound
    explicit Fixture(const AssetStore* installed = nullptr, bool speech_hooks = true) {
        map.attribute_width = map.attribute_height = map_cells;
        map.attributes.resize(map_cells * map_cells);
        terrain_values.resize(map_cells * map_cells);
        // Every unit maps the 32-unit sight cells within 160 of it for its
        // owner. The path search takes cells its player has not mapped as
        // open, so a one-cell sight would let routes run through
        // standing units.
        masks[0].width = masks[0].height = 2 * sight_cells + 1;
        masks[0].offset_x = masks[0].offset_z = sight_cells;
        masks[0].pixels.assign(masks[0].width * masks[0].height, 1);
        // A base 10 units high; the link piece hangs 8 units under it.
        model->objects.resize(2);
        model->objects[0].name = "base";
        model->objects[0].vertices = {{0, 0, 0}, {0, 10 << 16, 0}};
        model->objects[0].first_child = 1;
        model->objects[1].name = "link";
        model->objects[1].parent = 0;
        model->objects[1].offset_from_parent = {0, -(8 << 16), 0};
        // The hover transport's link hangs 40 units ahead of it.
        *crane = *model;
        crane->objects[1].offset_from_parent = {0, 0, 40 << 16};
        // An air pad's landing piece sits on its deck, 12 units up.
        *deck = *model;
        deck->objects[0].vertices = {{0, 0, 0}, {0, 12 << 16, 0}};
        deck->objects[1].offset_from_parent = {0, 12 << 16, 0};
        plain = assemble({{"Create", finish}});
        // Lift a unit within 100 units onto the link; drop it when the
        // transport stands within 80 units of the point.
        const std::vector<uint32_t> unit_xz = get(Value::unit_xz, {op::push_local, 0});
        const std::vector<uint32_t> base_xz = get(Value::piece_xz, {op::push_constant, 0});
        // Both stay BUSY for a second after moving the unit.
        loader = assemble({
            {"Create", finish},
            {"TransportPickup",
             join(
                 {unless_within(unit_xz, base_xz, 100),
                  set_busy(1),
                  attach_argument,
                  sleep_ms(1000),
                  set_busy(0),
                  finish}
             )},
            // Only the unit id is counted as an argument, so the first push
            // lands on the point's local: keep the point in a static first.
            {"TransportDrop",
             join(
                 {{op::push_local, 1, op::pop_static, 0},
                  unless_within(base_xz, {op::push_static, 0}, 80),
                  set_busy(1),
                  {op::push_local, 0, op::drop_unit},
                  sleep_ms(1000),
                  set_busy(0),
                  finish}
             )},
        });
        // QueryTransport answers the link piece; BeginTransport keeps its
        // argument in static 0 and EndTransport counts its calls in static 1.
        carrier = assemble({
            {"Create", finish},
            {"QueryTransport",
             {op::push_constant, 1, op::pop_local, 0, op::push_constant, 0, op::return_}},
            {"BeginTransport", join({{op::push_local, 0, op::pop_static, 0}, finish})},
            {"EndTransport", join({count_in_static(1), finish})},
            {"Statics", answer_statics},
        });
        // QueryLandingPad offers the landing piece.
        pad = assemble({
            {"Create", finish},
            {"QueryLandingPad",
             {op::push_constant, 1, op::pop_local, 0, op::push_constant, 0, op::return_}},
        });
        for (size_t i = 1; i < type_count; ++i) {
            types[i].simulation.flags = OA_UNIT_DEF_FLAG_AVAILABLE;
            types[i].simulation.maximum_health = 100;
            types[i].footprint_x = types[i].footprint_z = 2;
            types[i].bm_code = 1;
            types[i].model = reinterpret_cast<uintptr_t>(model.get());
            loaded[i].model = model;
            loaded[i].script = plain;
            defs[i].sight_distance = 160;
            defs[i].acceleration_fixed = 65536;
            defs[i].brake_rate_fixed = 65536;
            defs[i].max_velocity_fixed = 2 * 65536;
            defs[i].turn_rate = 1024;
            defs[i].footprint_x = defs[i].footprint_z = 2;
            defs[i].max_damage = 100;
            defs[i].energy_storage = defs[i].metal_storage = 1000.0F;
            fields[i].definition = &defs[i];
            fields[i].yard_mask = yard;
            fields[i].runtime_metadata = &metadata;
            fields[i].target_masks = &target_masks;
            fields[i].movement_class = 0;
        }
        loaded[air_transport_type].script = carrier;
        types[air_transport_type].simulation.flags |= OA_UNIT_DEF_FLAG_CAN_FLY;
        types[air_transport_type].simulation.abilities |= OA_UNIT_DEF_ABILITY_CAN_LOAD;
        defs[air_transport_type].can_fly = true;
        defs[air_transport_type].can_load = true;
        defs[air_transport_type].cant_be_transported = true;
        defs[air_transport_type].cruise_altitude = 60;
        defs[air_transport_type].max_velocity_fixed = 4 * 65536;
        defs[air_transport_type].transport_size = 3;
        defs[air_transport_type].transport_capacity = 1;
        types[heavy_type].footprint_x = types[heavy_type].footprint_z = 4;
        defs[heavy_type].footprint_x = defs[heavy_type].footprint_z = 4;
        loaded[hover_transport_type].model = crane;
        loaded[hover_transport_type].script = loader;
        types[hover_transport_type].model = reinterpret_cast<uintptr_t>(crane.get());
        types[hover_transport_type].simulation.flags |= OA_UNIT_DEF_FLAG_CAN_HOVER;
        types[hover_transport_type].simulation.abilities |= OA_UNIT_DEF_ABILITY_CAN_LOAD;
        defs[hover_transport_type].can_hover = true;
        defs[hover_transport_type].can_load = true;
        defs[hover_transport_type].cant_be_transported = true;
        defs[hover_transport_type].transport_size = 3;
        defs[hover_transport_type].transport_capacity = 1;
        fields[hover_transport_type].movement_class = 1;
        // The hovercraft's movement class record: its persistent map takes
        // the class footprint, which the class's types share.
        metadata.footprint_x = metadata.footprint_z = 2;
        // A ground gun and an anti-air gun, both reaching 300.
        (void)sim::combat_state::install_weapon_text(
            weapons,
            "[TESTGUN]{id=1; reloadtime=1; range=300; lineofsight=1; weaponvelocity=100; "
            "[DAMAGE]{default=10;}}"
        );
        (void)sim::combat_state::install_weapon_text(
            weapons,
            "[TESTFLAK]{id=2; reloadtime=1; range=300; lineofsight=1; toairweapon=1; "
            "weaponvelocity=100; [DAMAGE]{default=10;}}"
        );
        loaded[pad_type].model = deck;
        loaded[pad_type].script = pad;
        types[pad_type].model = reinterpret_cast<uintptr_t>(deck.get());
        types[pad_type].simulation.flags |= OA_UNIT_DEF_FLAG_IS_AIRBASE;
        defs[pad_type].is_airbase = true;
        defs[pad_type].footprint_x = defs[pad_type].footprint_z = 4;
        types[pad_type].footprint_x = types[pad_type].footprint_z = 4;
        // A pad that moves, as a carrier's deck does, at half a unit a tick.
        defs[pad_type].max_velocity_fixed = 65536 / 2;
        defs[gun_type].weapon1 = "TESTGUN";
        defs[flak_type].weapon1 = "TESTFLAK";
        for (const auto armed : {gun_type, flak_type}) {
            types[armed].simulation.flags |= OA_UNIT_DEF_FLAG_HAS_WEAPONS;
            defs[armed].can_attack = true;
        }
        if (installed != nullptr) {
            const std::array<std::pair<uint16_t, const char*>, 4> units{
                {{air_transport_type, "armatlas"},
                 {cargo_type, "armpw"},
                 {heavy_type, "armpw"},
                 {hover_transport_type, "armthovr"}}
            };
            for (const auto& [type, name] : units) {
                loaded[type].model =
                    installed_model(*installed, "objects3d/" + std::string(name) + ".3do");
                types[type].model = reinterpret_cast<uintptr_t>(loaded[type].model.get());
                if (type != cargo_type && type != heavy_type)
                    loaded[type].script =
                        installed_script(*installed, "scripts/" + std::string(name) + ".cob");
            }
        }
        for (size_t i = 1; i < type_count; ++i) {
            types[i].cob = reinterpret_cast<uintptr_t>(loaded[i].script.get());
            loaded[i].type = types[i];
        }
        collision_plots.resize(map_cells * map_cells);
        speech = speech_hooks;
        build_match();
    }

    /// Builds the match from the types as they are now.
    void build_match() {
        match.reset();
        sim::match_runtime::OfflineInputs input{
            map,  loaded, types, fields, weapons,   terrain_values,    masks,           32, 32, 16,
            2,    0,      30,    1,      &scenario, [] { return 0u; }, collision_plots, {}, 0,  0,
            0.0F, {}
        };
        match = std::make_unique<sim::match_runtime::Match>(input, services);
        services.match = match.get();
        if (speech)
            match->set_speech_hooks({&services, &Services::speak});
        match->configure_strategic_environment({0, 0.5F, 0});
        match->simulation().players[0].present = true;
        match->simulation().players[0].status = 1;
        std::array<uint8_t, 10> allies{};
        allies[0] = 1;
        match->configure_player_alliances(0, allies);
        match->configure_outcomes(0, allies, false);
    }

    /// Gives the type a script before any unit is spawned.
    ///
    /// A match prepares each type's script program as it is built, so the
    /// match is built again with the new script.
    void use_script(uint16_t type, std::shared_ptr<formats::cob::CobProgram> script) {
        loaded[type].script = std::move(script);
        types[type].cob = reinterpret_cast<uintptr_t>(loaded[type].script.get());
        loaded[type].type = types[type];
        build_match();
    }

    sim::unit_spawn::Slot& spawn(uint16_t type, int32_t x, int32_t z) {
        auto* slot = match->create({0, type, at(x, z), true, 1, 0});
        CHECK(slot && slot->movement_object);
        return *slot;
    }

    void run(uint32_t ticks) {
        for (uint32_t i = 0; i < ticks; ++i) {
            match->simulation().tick = clock++;
            match->tick();
        }
    }

    template <typename Done>
    bool run_until(uint32_t limit, Done done) {
        for (uint32_t i = 0; i < limit; ++i) {
            if (done())
                return true;
            run(1);
        }
        return done();
    }
};

void air_transport_carries_a_unit_to_the_unload_point(const AssetStore* installed) {
    Fixture f(installed);
    auto& atlas = f.spawn(air_transport_type, 200, 200);
    auto& tank = f.spawn(cargo_type, 360, 200);
    const auto transport = atlas.unit_index;
    const auto cargo = tank.unit_index;

    auto& pickup = f.match->issue_load(transport, cargo, false);
    CHECK(pickup.kind == vtol_pickup);
    CHECK(f.run_until(600, [&] { return tank.record.attach_parent != 0; }));
    CHECK(oa::oa_unit_slot_from_ref(tank.record.attach_parent) == transport);
    // The transport hovered with its link piece on the cargo, so attaching
    // barely moves it; the installed link hangs further below a hull that may
    // still be banked.
    const int32_t reach = installed == nullptr ? 4 : 24;
    CHECK(
        std::abs(world_x(tank.record) - 360) <= reach &&
        std::abs(world_z(tank.record) - 200) <= reach
    );
    CHECK(std::abs(tank.record.position.y >> 16) <= reach);
    // Carried: out of the buckets, unselectable, running BeCarried.
    CHECK(!in_buckets(*f.match, cargo));
    CHECK(!f.match->selectable(cargo));
    CHECK(tank.unit->primary && tank.unit->primary->kind == be_carried);
    CHECK(static_cast<int8_t>(tank.record.attach_piece) == 1);

    auto& unload = f.match->issue_unload(transport, point(200, 480), true);
    CHECK(unload.kind == vtol_unload);
    uint32_t followed = 0;
    int32_t hung = 0;
    CHECK(f.run_until(1500, [&] {
        if (tank.record.attach_parent) {
            hung = tank.record.position.y;
            const auto link = f.match->instance(transport)->piece_world(1);
            CHECK(tank.record.position.x == static_cast<int32_t>(link[0]));
            CHECK(tank.record.position.z == static_cast<int32_t>(link[2]));
            CHECK(tank.record.cell_x == cell_of(tank.record.position.x, tank.record.footprint_x));
            CHECK(tank.record.cell_z == cell_of(tank.record.position.z, tank.record.footprint_z));
            CHECK(!in_buckets(*f.match, cargo));
            // The carry link's mode 0 is the unit's movement layer while it
            // hangs (the carry link, then the occupancy removal of the movement
            // step).
            CHECK((tank.record.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) == 0);
            ++followed;
        }
        return tank.record.attach_parent == 0;
    }));
    CHECK(followed > 20);
    // The base hovers within 0.5 of the point with the hull level again; the
    // made-up link hangs straight under it, the installed one about a unit off.
    CHECK(planar(tank.record, 200, 480) <= (installed == nullptr ? 1 : 2));
    CHECK(in_buckets(*f.match, cargo));
    CHECK(f.match->selectable(cargo));
    // Released into the ground layer. The release snaps no height: the
    // cargo's movement step on the same tick finds its layer changed and
    // rewrites the position as moved, so the height fit puts it on the ground
    // through its model's ground plate.
    // That fit writes only the whole-unit word of the height, the flat
    // ground's 0, and keeps the fraction the unit hung at. The made-up model
    // has no ground plate, so it keeps the whole height it hung at.
    CHECK((tank.record.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) == 1);
    const auto released = tank.record.position.y;
    CHECK(released == (installed == nullptr ? hung : (hung & 0xffff)));
    CHECK(f.run_until(300, [&] {
        CHECK(tank.record.position.y == released);
        return atlas.unit->primary == nullptr || atlas.unit->primary->kind != vtol_unload;
    }));
}

// Hovering loaded over the pickup point, the transport's velocity no longer
// changes, so its bank eases back to level by the flight step's decay alone:
// each tick keeps 0xf333/0x10000 of the movement object's filtered velocity
// change, and the bank is the angle of its negated X in the heading frame
// (turned by the heading rotation) against gravity * 0x10000 / 0xccd. Whatever bank
// the arrival left at the attach fades within a few seconds.
void loaded_air_transport_levels_out_while_hovering(const AssetStore* installed) {
    Fixture f(installed);
    auto& atlas = f.spawn(air_transport_type, 200, 200);
    auto& tank = f.spawn(cargo_type, 360, 200);
    // The pickup hover matches the cargo's heading; off the axes the turn
    // into the heading frame rounds.
    tank.record.heading = 0x2345;
    const auto transport = atlas.unit_index;
    f.match->issue_load(transport, tank.unit_index, false);
    CHECK(f.run_until(600, [&] { return tank.record.attach_parent != 0; }));
    CHECK(atlas.record.heading == 0x2345);
    auto& movement = f.match->ground_runtime(transport)->movement;
    auto& filtered = f.match->ground_runtime(transport)->previous_vector;
    const std::array<int32_t, 3> still{};
    CHECK(f.run_until(60, [&] { return movement.velocity == still; }));
    const auto& def = *oa::world_unit_def_of(&f.match->state(), &atlas.record);
    const auto gravity = f.match->state().game.gravity != 0 ? f.match->state().game.gravity
                                                            : sim::ground_orders::default_gravity;
    const auto vertical = static_cast<int32_t>((static_cast<int64_t>(gravity) << 16) / 0xccd);
    const auto level = [&] {
        int32_t x = filtered[0], z = filtered[2];
        sim::air::rotate_xz(static_cast<int16_t>(atlas.record.heading), &x, &z);
        const auto roll =
            static_cast<int32_t>((static_cast<int64_t>(def.bank_scale) * -int64_t{x}) >> 16);
        return static_cast<int16_t>(base::game_math::direction(roll, vertical));
    };
    const auto first = static_cast<int16_t>(atlas.record.bank);
    CHECK(first == level());
    for (int tick = 0; tick < 90; ++tick) {
        const auto before = filtered;
        f.run(1);
        CHECK(movement.velocity == still);
        for (size_t axis = 0; axis < 3; ++axis)
            CHECK(
                filtered[axis] ==
                static_cast<int32_t>((static_cast<int64_t>(before[axis]) * 0xf333) >> 16)
            );
        CHECK(static_cast<int16_t>(atlas.record.bank) == level());
    }
    CHECK(std::abs(static_cast<int16_t>(atlas.record.bank)) * 10 <= std::abs(first) + 10);
}

// Nothing in automatic targeting looks at a carry link: the target pick draws
// its candidates from the seen enemies the knowledge walk lists, and neither
// that list nor the visibility test checks Unit.attach_parent. What a carry does
// change is the unit's movement layer, which takes the link's mode 0 (the
// Atlas's VTOL_Pickup and the Bear's attach-unit both pass 0). The range test
// wants an anti-air weapon's target in the air layer, so a
// carried unit is out of an anti-air gun's reach, but not out of a ground
// gun's.
void carried_units_stay_in_reach_of_ground_guns(const AssetStore* installed) {
    Fixture f(installed);
    auto& atlas = f.spawn(air_transport_type, 200, 200);
    auto& bear = f.spawn(hover_transport_type, 600, 200);
    auto& flown = f.spawn(cargo_type, 360, 200);
    auto& shipped = f.spawn(cargo_type, 600, 280);
    auto& gun = f.spawn(gun_type, 480, 360);
    auto& flak = f.spawn(flak_type, 480, 40);
    f.match->issue_load(atlas.unit_index, flown.unit_index, false);
    f.match->issue_load(bear.unit_index, shipped.unit_index, false);
    CHECK(f.run_until(900, [&] {
        return flown.record.attach_parent && shipped.record.attach_parent;
    }));
    f.run(2);
    for (const auto* cargo : {&flown, &shipped}) {
        CHECK((cargo->record.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) == 0);
        CHECK(f.match->weapon_can_reach(gun.unit_index, cargo->unit_index, 0));
        CHECK(!f.match->weapon_can_reach(flak.unit_index, cargo->unit_index, 0));
    }
    // The flying carrier stays in the anti-air gun's reach; the hovering one
    // is a ground target.
    CHECK((atlas.record.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) == 2);
    CHECK(f.match->weapon_can_reach(flak.unit_index, atlas.unit_index, 0));
    CHECK(!f.match->weapon_can_reach(flak.unit_index, bear.unit_index, 0));
    CHECK(f.match->weapon_can_reach(gun.unit_index, bear.unit_index, 0));
}

void air_transport_capacity_is_enforced(const AssetStore* installed) {
    Fixture f(installed);
    auto& atlas = f.spawn(air_transport_type, 200, 200);
    auto& heavy = f.spawn(heavy_type, 300, 200);
    f.match->issue_load(atlas.unit_index, heavy.unit_index, false);
    f.run(2);
    CHECK(heavy.record.attach_parent == 0);
    CHECK(f.services.spoke(atlas.unit_index, speech_failed));
    CHECK(f.services.captioned(atlas.unit_index, speech_failed, "Unit is too heavy to transport"));
    CHECK(atlas.unit->primary == nullptr || atlas.unit->primary->kind != vtol_pickup);

    auto& first = f.spawn(cargo_type, 360, 200);
    auto& second = f.spawn(cargo_type, 200, 360);
    f.match->issue_load(atlas.unit_index, first.unit_index, false);
    CHECK(f.run_until(600, [&] { return first.record.attach_parent != 0; }));
    CHECK(f.match->loaded_child_count(atlas.unit_index) == 1);
    f.match->issue_load(atlas.unit_index, second.unit_index, false);
    f.run(30);
    CHECK(second.record.attach_parent == 0);
    CHECK(f.match->loaded_child_count(atlas.unit_index) == 1);
}

// VTOL_Pickup and VTOL_Unload goal by goal with the made-up units: cruise
// altitude 60, the link 8 under the base, a cargo 10 high on flat ground.
void air_transport_flies_the_game_goals() {
    Fixture f;
    auto& atlas = f.spawn(air_transport_type, 200, 200);
    auto& tank = f.spawn(cargo_type, 360, 200);
    const auto transport = atlas.unit_index;
    const auto phase = [&] { return atlas.unit->primary ? atlas.unit->primary->phase : 0; };

    f.match->issue_load(transport, tank.unit_index, false);
    // Phase 1 follows the cargo; its 0x30 arrival radius wakes phase 2, and
    // phase 3 starts BeginTransport with the cargo height and hovers over it.
    CHECK(f.run_until(600, [&] { return kind_of(atlas) == vtol_pickup && phase() >= 4; }));
    CHECK(planar(atlas.record, 360, 200) < 0x30);
    CHECK(statics_of(*f.match, transport)[0] == 10 * one);
    // The hover goal arrives with the base over the cargo and 8 above it, so
    // the link meets the cargo, and phase 4 attaches it there.
    CHECK(f.run_until(600, [&] { return tank.record.attach_parent != 0; }));
    CHECK(planar(atlas.record, 360, 200) <= 0.5);
    CHECK(std::abs(atlas.record.position.y - 8 * one) < one);
    // Nothing replaces the hover goal: the loaded transport ends the order on
    // the goal's next event, still at the pickup height.
    CHECK(f.run_until(30, [&] { return kind_of(atlas) != vtol_pickup; }));
    CHECK(std::abs(atlas.record.position.y - 8 * one) < one);

    // Phase 0 flies at cruise altitude until within 0x140 of the point, where
    // phase 1 takes the goal down to the cargo height over the point.
    f.match->issue_unload(transport, point(200, 800), false);
    int32_t highest = 0;
    CHECK(f.run_until(900, [&] {
        const oa_fixed height = atlas.record.position.y;
        highest = std::max(highest, height);
        return kind_of(atlas) == vtol_unload && phase() >= 2;
    }));
    CHECK(std::abs(highest - 60 * one) < one);
    CHECK(planar(atlas.record, 200, 800) < 0x140);
    // Phase 2 starts EndTransport and sets the cargo down where it hangs, the
    // link 8 under a base 10 above the point.
    CHECK(f.run_until(900, [&] { return tank.record.attach_parent == 0; }));
    CHECK(planar(tank.record, 200, 800) <= 1);
    CHECK(std::abs(tank.record.position.y - 2 * one) < one);
    // Phase 2's goal climbs back to cruise altitude where the unit was set
    // down. Its arrival wakes the order, which ends as the transport is empty
    // before phase 3 could speak.
    const auto release = atlas.record.position;
    CHECK(f.run_until(300, [&] { return kind_of(atlas) != vtol_unload; }));
    CHECK(std::abs(atlas.record.position.y - 60 * one) < one);
    CHECK(planar(atlas.record, release.x >> 16, release.z >> 16) <= 1);
    CHECK(!f.services.spoke(transport, speech_unloaded));
    CHECK(statics_of(*f.match, transport)[1] == 1);
}

// VTOL_Unload refuses a point the cargo cannot stand on: it speaks the failure
// and returns 9, which with nothing queued restarts it after at least 30
// ticks, so it never descends until the point is clear.
void air_transport_waits_for_a_clear_unload_point() {
    constexpr auto weapon = static_cast<uint8_t>(sim::match_runtime::DeathKind::weapon);
    Fixture f;
    auto& atlas = f.spawn(air_transport_type, 200, 200);
    auto& tank = f.spawn(cargo_type, 360, 200);
    auto& blocker = f.spawn(cargo_type, 200, 800);
    const auto transport = atlas.unit_index;
    f.match->issue_load(transport, tank.unit_index, false);
    CHECK(f.run_until(600, [&] { return tank.record.attach_parent != 0; }));

    f.match->issue_unload(transport, point(200, 800), false);
    // Only phase 1 would install the descent over the point; phase 0's goal
    // holds cruise altitude.
    int32_t lowest = 60 * one;
    size_t failures = 0;
    uint32_t last_failure = 0;
    for (uint32_t tick = 0; tick < 400; ++tick) {
        f.run(1);
        if (failures != 0) {
            const oa_fixed height = atlas.record.position.y;
            lowest = std::min(lowest, height);
        }
        if (const auto spoken = f.services.spoken(transport, speech_failed); spoken != failures) {
            CHECK(spoken == failures + 1);
            CHECK(failures == 0 || tick - last_failure >= 30);
            failures = spoken;
            last_failure = tick;
        }
    }
    CHECK(failures >= 400 / 60);
    CHECK(f.services.captioned(transport, speech_failed, "Unable to unload unit") == failures);
    CHECK(lowest > 59 * one);
    CHECK(tank.record.attach_parent != 0);
    CHECK(kind_of(atlas) == vtol_unload && (atlas.unit->primary->flags & order_retry));

    f.match->apply_damage_event(blocker, nullptr, 30000, weapon, 0);
    CHECK(f.run_until(900, [&] { return tank.record.attach_parent == 0; }));
    CHECK(planar(tank.record, 200, 800) <= 1);
}

bool dead(const sim::unit_spawn::Slot& slot) {
    return slot.record.type_index == 0;
}

void carried_units_leave_a_dead_carrier(const AssetStore* installed) {
    constexpr auto weapon = static_cast<uint8_t>(sim::match_runtime::DeathKind::weapon);
    // A carried unit that dies leaves its carrier free to load again.
    Fixture f(installed);
    auto& atlas = f.spawn(air_transport_type, 200, 200);
    auto& tank = f.spawn(cargo_type, 360, 200);
    f.match->issue_load(atlas.unit_index, tank.unit_index, false);
    CHECK(f.run_until(600, [&] { return tank.record.attach_parent != 0; }));
    f.match->apply_damage_event(tank, nullptr, 30000, weapon, 0);
    CHECK(f.run_until(60, [&] { return dead(tank); }));
    CHECK(f.match->loaded_child_count(atlas.unit_index) == 0 && !atlas.record.attach_first_child);

    // A carrier that dies takes its cargo with it and sets it down first.
    auto& second = f.spawn(cargo_type, 200, 360);
    f.match->issue_load(atlas.unit_index, second.unit_index, false);
    CHECK(f.run_until(600, [&] { return second.record.attach_parent != 0; }));
    f.match->apply_damage_event(atlas, nullptr, 30000, weapon, 0);
    CHECK(f.run_until(60, [&] { return dead(atlas) && dead(second); }));
    CHECK(!second.record.attach_parent && !atlas.record.attach_first_child);
}

// Ticks a transport's `kind` order spent with its script BUSY, and ticks it
// outlived BUSY by, running until the order ends.
struct BusyWait {
    uint32_t busy{};
    uint32_t after{};
};

BusyWait wait_out_busy(Fixture& f, const sim::unit_spawn::Slot& transport, uint8_t kind) {
    constexpr uint8_t busy_flag = 0x02; // unit build_flags, COB BUSY
    BusyWait wait;
    CHECK(f.run_until(600, [&] {
        if (kind_of(transport) != kind)
            return true;
        if (transport.record.build_flags & busy_flag)
            ++wait.busy;
        else if (wait.busy)
            ++wait.after;
        return false;
    }));
    return wait;
}

void hover_transport_loads_and_unloads_through_its_scripts(const AssetStore* installed) {
    Fixture f(installed);
    auto& bear = f.spawn(hover_transport_type, 200, 200);
    auto& tank = f.spawn(cargo_type, 200, 280);
    const auto transport = bear.unit_index;
    const auto cargo = tank.unit_index;
    CHECK(sim::gameplay_input::can_load_unit(f.match->state(), bear.record, tank.record));

    auto& pickup = f.match->issue_load(transport, cargo, false);
    CHECK(pickup.kind == ground_pickup);
    CHECK(f.run_until(900, [&] { return tank.record.attach_parent != 0; }));
    CHECK(oa::oa_unit_slot_from_ref(tank.record.attach_parent) == transport);
    CHECK(tank.unit->primary && tank.unit->primary->kind == be_carried);
    CHECK(!in_buckets(*f.match, cargo) && !f.match->selectable(cargo));
    // The crane still swings the unit aboard before it clears BUSY; the order
    // waits for that, then finds the unit carried.
    const auto boarding = wait_out_busy(f, bear, ground_pickup);
    if (installed == nullptr)
        CHECK(boarding.busy >= 25 && boarding.after <= 1);
    // Full: capacity 1 refuses a second passenger.
    auto& other = f.spawn(cargo_type, 500, 200);
    CHECK(!sim::gameplay_input::can_load_unit(f.match->state(), bear.record, other.record));

    // The order's acknowledgement speaks once, on its first run.
    CHECK(f.services.captioned(transport, speech_acknowledged, "Loading unit") == 1);
    auto& unload = f.match->issue_unload(transport, point(560, 560), false);
    CHECK(unload.kind == ground_unload);
    CHECK(f.run_until(2000, [&] {
        if (tank.record.attach_parent) {
            const auto piece = static_cast<uint32_t>(static_cast<int8_t>(tank.record.attach_piece));
            const auto link = f.match->instance(transport)->piece_world(piece);
            CHECK(tank.record.position.x == static_cast<int32_t>(link[0]));
            CHECK(tank.record.position.z == static_cast<int32_t>(link[2]));
        }
        return tank.record.attach_parent == 0;
    }));
    // The first TransportDrop found the point out of reach; the second ran
    // where the close-in goal ended, and the unit stays where it hung.
    CHECK(within_reach_cells(bear.record, 560, 560));
    const double hung = std::hypot(
        (tank.record.position.x - bear.record.position.x) / 65536.0,
        (tank.record.position.z - bear.record.position.z) / 65536.0
    );
    if (installed == nullptr)
        CHECK(std::abs(hung - 40) < 1);
    else
        CHECK(planar(tank.record, 560, 560) <= 48);
    CHECK((tank.record.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) == 1);
    CHECK(in_buckets(*f.match, cargo) && f.match->selectable(cargo));
    const auto setting_down = wait_out_busy(f, bear, ground_unload);
    if (installed == nullptr)
        CHECK(setting_down.busy >= 25 && setting_down.after <= 1);
    CHECK(f.services.spoke(transport, speech_unloaded));
    CHECK(f.services.captioned(transport, speech_acknowledged, "Unloading") == 1);
    CHECK(sim::gameplay_input::can_load_unit(f.match->state(), bear.record, other.record));
}

// Out of the script's reach, Ground_Pickup closes in with a goal of radius 0
// on the cargo's own cell, which the transport can never stand on. The
// search routes it to the nearest open cell (it takes cells within the
// wall-follow cost as goal cells); the search from there fails, and that
// failure (event 0x40) wakes phase 5, which restarts the
// order for a second TransportPickup with the cargo in reach. The installed
// script reaches 125 from its turret piece: 37.5 plus 87.5 in BoomCalc.
void hover_transport_closes_in_on_a_unit_out_of_reach(const AssetStore* installed) {
    Fixture f(installed);
    auto& bear = f.spawn(hover_transport_type, 200, 200);
    auto& tank = f.spawn(cargo_type, 200, 420);
    const auto transport = bear.unit_index;
    const int32_t cargo_x = cell_of(tank.record.position.x, 2),
                  cargo_z = cell_of(tank.record.position.z, 2);
    const double reach = installed == nullptr ? 100 : 125;
    f.match->issue_load(transport, tank.unit_index, false);
    bool closed_in = false;
    std::vector<double> attempts;
    CHECK(f.run_until(900, [&] {
        const auto* goal = f.match->ground_runtime(transport)->navigation.goal;
        if (goal && kind_of(bear) == ground_pickup) {
            CHECK(goal->tolerance == 0 && goal->cell[0] == cargo_x && goal->cell[1] == cargo_z);
            closed_in = true;
        }
        // Both footprints are two cells wide: the transport never overlaps
        // the cargo, so it never stands on the goal cell.
        CHECK(
            std::abs(bear.record.cell_x - cargo_x) >= 2 ||
            std::abs(bear.record.cell_z - cargo_z) >= 2
        );
        if (f.services.spoken(transport, speech_loading) > attempts.size())
            attempts.push_back(planar(bear.record, 200, 420));
        return tank.record.attach_parent != 0;
    }));
    CHECK(closed_in);
    CHECK(attempts.size() == 2 && attempts[0] > reach && attempts[1] < reach);
    CHECK(!f.services.spoke(transport, speech_failed));
    CHECK(oa::oa_unit_slot_from_ref(tank.record.attach_parent) == transport);
}

void hover_transport_refuses_a_unit_too_large() {
    Fixture f;
    auto& bear = f.spawn(hover_transport_type, 200, 200);
    auto& heavy = f.spawn(heavy_type, 200, 280);
    auto& pickup = f.match->issue_load(bear.unit_index, heavy.unit_index, false);
    CHECK(pickup.kind == ground_pickup);
    f.run(2);
    CHECK(kind_of(bear) != ground_pickup);
    CHECK(f.services.spoke(bear.unit_index, speech_failed));
    CHECK(f.services.captioned(bear.unit_index, speech_failed, "Unit is too large to transport"));
    CHECK(!f.services.spoke(bear.unit_index, speech_loading));
    CHECK(heavy.record.attach_parent == 0);
}

// Ground_Pickup and Ground_Unload give the script three attempts, closing in
// between them, then return 9: with nothing queued the order stays, marked
// for retry, and restarts after at least 30 ticks.
void hover_transport_gives_up_after_three_attempts() {
    {
        // TransportPickup stays BUSY for 300 ms and never attaches. Standing
        // on the cargo, each close-in goal arrives at once.
        Fixture f;
        f.use_script(
            hover_transport_type,
            assemble(
                {{"Create", finish},
                 {"TransportPickup", join({set_busy(1), sleep_ms(300), set_busy(0), finish})}}
            )
        );
        auto& bear = f.spawn(hover_transport_type, 200, 200);
        auto& tank = f.spawn(cargo_type, 200, 200);
        const auto transport = bear.unit_index;
        f.match->issue_load(transport, tank.unit_index, false);
        CHECK(f.run_until(600, [&] {
            return bear.unit->primary && (bear.unit->primary->flags & order_retry);
        }));
        CHECK(f.services.spoken(transport, speech_loading) == 3);
        CHECK(kind_of(bear) == ground_pickup && tank.record.attach_parent == 0);
        f.run(29);
        CHECK(f.services.spoken(transport, speech_loading) == 3);
    }
    {
        // TransportDrop counts its calls in static 1, stays BUSY for 300 ms
        // and never drops.
        Fixture f;
        f.use_script(
            hover_transport_type,
            assemble(
                {{"Create", finish},
                 {"TransportPickup", join({set_busy(1), attach_argument, set_busy(0), finish})},
                 {"TransportDrop",
                  join({count_in_static(1), set_busy(1), sleep_ms(300), set_busy(0), finish})},
                 {"Statics", answer_statics}}
            )
        );
        auto& bear = f.spawn(hover_transport_type, 200, 200);
        auto& tank = f.spawn(cargo_type, 200, 280);
        const auto transport = bear.unit_index;
        f.match->issue_load(transport, tank.unit_index, false);
        CHECK(f.run_until(600, [&] { return tank.record.attach_parent != 0; }));
        CHECK(f.run_until(600, [&] { return kind_of(bear) != ground_pickup; }));
        f.match->issue_unload(transport, point(560, 560), false);
        CHECK(f.run_until(2000, [&] {
            return bear.unit->primary && (bear.unit->primary->flags & order_retry);
        }));
        CHECK(statics_of(*f.match, transport)[1] == 3);
        CHECK(kind_of(bear) == ground_unload && tank.record.attach_parent != 0);
        CHECK(within_reach_cells(bear.record, 560, 560));
        f.run(29);
        CHECK(statics_of(*f.match, transport)[1] == 3);
    }
}

// Without the application's speech hook a failure still speaks its category,
// through command_sound and with no caption.
void speech_without_hooks_plays_the_category() {
    Fixture f(nullptr, false);
    auto& atlas = f.spawn(air_transport_type, 200, 200);
    auto& heavy = f.spawn(heavy_type, 300, 200);
    f.match->issue_load(atlas.unit_index, heavy.unit_index, false);
    f.run(2);
    CHECK(f.services.spoke(atlas.unit_index, speech_failed));
    CHECK(f.services.captions.empty());
}

// A landing aircraft's position before it attaches, and its largest height
// change from one tick to the next.
struct Approach {
    oa::FixedVec3 last{};
    uint16_t heading{};
    int32_t steepest{};
    uint32_t ticks{};
};

/// Runs until `done`, following the aircraft until it attaches to anything.
///
/// @param f the fixture
/// @param aircraft the landing aircraft
/// @param done condition that ends the run
/// @return the approach, or a run past its limit through CHECK
template <typename Done>
Approach land(Fixture& f, const sim::unit_spawn::Slot& aircraft, Done done) {
    Approach approach;
    approach.last = aircraft.record.position;
    CHECK(f.run_until(2000, [&] {
        if (done())
            return true;
        if (aircraft.record.attach_parent == 0) {
            approach.steepest =
                std::max(approach.steepest, std::abs(aircraft.record.position.y - approach.last.y));
            approach.last = aircraft.record.position;
            approach.heading = aircraft.record.heading;
        }
        ++approach.ticks;
        return false;
    }));
    return approach;
}

// VTOL_Landing settles over the pad's landing piece with the pad's heading
// before it attaches: the descent follows the piece rather than a point at
// cruise altitude, so the aircraft does not drop onto the pad from on high.
void air_transport_settles_on_the_pad_piece() {
    Fixture f;
    auto& atlas = f.spawn(air_transport_type, 200, 200);
    auto& pad = f.spawn(pad_type, 400, 400);
    pad.record.heading = 0x4000;
    f.match->issue_order(atlas.unit_index, vtol_landing, false, pad.unit_index, nullptr, 0, 0);
    CHECK(kind_of(atlas) == vtol_landing);
    const auto approach = land(f, atlas, [&] { return atlas.record.attach_parent != 0; });
    CHECK(oa::oa_unit_slot_from_ref(atlas.record.attach_parent) == pad.unit_index);
    CHECK(f.services.captioned(atlas.unit_index, speech_acknowledged, "Landing") == 1);
    const auto piece = f.match->instance(pad.unit_index)->piece_world(1);
    const auto seat = static_cast<int32_t>(piece[1]);
    CHECK(planar(atlas.record, static_cast<int32_t>(piece[0]) >> 16, piece[2] >> 16) <= 0.5);
    CHECK(approach.heading == pad.record.heading);
    // Settled within a unit of the piece, and never more than a few units a
    // tick on the way down from cruise altitude.
    CHECK(std::abs(approach.last.y - seat) < one);
    CHECK(approach.steepest < 4 * one);
    CHECK(std::abs(atlas.record.position.y - approach.last.y) < one);
}

// A loaded transport settles its cargo's height above the piece, then hands
// the cargo to the pad: the cargo hangs there selectable, still running the
// BeCarried its pickup gave it (it ends only once the unit is set down), and
// a move order it is given ends at once while it hangs.
void loaded_air_transport_hands_its_cargo_to_the_pad() {
    Fixture f;
    auto& atlas = f.spawn(air_transport_type, 200, 200);
    auto& tank = f.spawn(cargo_type, 260, 200);
    auto& pad = f.spawn(pad_type, 400, 400);
    pad.record.heading = 0x4000;
    f.match->issue_load(atlas.unit_index, tank.unit_index, false);
    CHECK(f.run_until(800, [&] { return tank.record.attach_parent != 0; }));
    CHECK(f.run_until(60, [&] { return kind_of(atlas) != vtol_pickup; }));
    f.match->issue_order(atlas.unit_index, vtol_landing, false, pad.unit_index, nullptr, 0, 0);
    const auto approach = land(f, atlas, [&] {
        return oa::oa_unit_slot_from_ref(tank.record.attach_parent) == pad.unit_index;
    });
    const auto seat = static_cast<int32_t>(f.match->instance(pad.unit_index)->piece_world(1)[1]);
    CHECK(std::abs(approach.last.y - (seat + 10 * one)) < one);
    CHECK(approach.steepest < 4 * one);
    CHECK(atlas.record.attach_parent == 0 && !atlas.record.attach_first_child);
    CHECK(static_cast<int8_t>(tank.record.attach_piece) == 1);
    CHECK(f.match->selectable(tank.unit_index));
    CHECK(kind_of(tank) == be_carried);
    f.match->issue_ground_move(tank.unit_index, point(600, 600), false);
    f.run(3);
    CHECK(oa::oa_unit_slot_from_ref(tank.record.attach_parent) == pad.unit_index);
    CHECK(kind_of(tank) != move_ground);
}

// The approach follows a pad that moves, as a carrier's deck does.
void air_transport_follows_a_moving_pad() {
    Fixture f;
    auto& atlas = f.spawn(air_transport_type, 200, 200);
    auto& pad = f.spawn(pad_type, 500, 300);
    f.match->issue_ground_move(pad.unit_index, point(500, 1500), false);
    f.run(30);
    f.match->issue_order(atlas.unit_index, vtol_landing, false, pad.unit_index, nullptr, 0, 0);
    oa::FixedVec3 piece_before_attach{};
    oa::FixedVec3 pad_start = pad.record.position;
    const auto approach = land(f, atlas, [&] {
        if (atlas.record.attach_parent != 0)
            return true;
        const auto piece = f.match->instance(pad.unit_index)->piece_world(1);
        piece_before_attach = {
            static_cast<int32_t>(piece[0]),
            static_cast<int32_t>(piece[1]),
            static_cast<int32_t>(piece[2])
        };
        return false;
    });
    CHECK(oa::oa_unit_slot_from_ref(atlas.record.attach_parent) == pad.unit_index);
    // The pad moved while the aircraft came in, and the aircraft settled on
    // where its piece had gone.
    CHECK(std::abs(pad.record.position.z - pad_start.z) > 16 * one);
    CHECK(
        std::hypot(
            (approach.last.x - piece_before_attach.x) / 65536.0,
            (approach.last.z - piece_before_attach.z) / 65536.0
        ) <= 1.0
    );
}

// A landing that finds its piece taken says so: in phase 3 as "Landing
// failed", and with nothing to land on it starts over.
void landing_says_why_it_failed() {
    Fixture f;
    auto& atlas = f.spawn(air_transport_type, 200, 200);
    auto& pad = f.spawn(pad_type, 400, 400);
    auto& squatter = f.spawn(cargo_type, 600, 600);
    f.match->issue_order(atlas.unit_index, vtol_landing, false, pad.unit_index, nullptr, 0, 0);
    // Phase 2 has sent it at the pad; phase 3 looks for a free piece next.
    CHECK(f.run_until(600, [&] {
        return kind_of(atlas) == vtol_landing && atlas.unit->primary->phase == 3;
    }));
    f.match->set_carry_link(squatter.unit_index, pad.unit_index, 1, 0);
    CHECK(f.run_until(600, [&] {
        return f.services.captioned(atlas.unit_index, speech_failed, "Landing failed") != 0;
    }));
    CHECK(kind_of(atlas) == vtol_landing && atlas.record.attach_parent == 0);
    // With its pad gone the order gives up.
    constexpr auto weapon = static_cast<uint8_t>(sim::match_runtime::DeathKind::weapon);
    f.match->apply_damage_event(pad, nullptr, 30000, weapon, 0);
    CHECK(f.run_until(600, [&] {
        return f.services.captioned(atlas.unit_index, speech_failed, "Landing aborted") != 0;
    }));
}

// ---------------------------------------------------------------------------
// The installed game's transports, with their own definitions and scripts, on
// a map of open sea 60 deep, a shelf 8 deep from column 40 and land from 46.

constexpr uint8_t installed_sea = 100;
constexpr int32_t installed_shelf = 40;
constexpr int32_t installed_land = 46;
constexpr uint8_t installed_deep_floor = 40;
constexpr uint8_t installed_shelf_floor = 92;
constexpr uint8_t installed_land_height = 120;

struct InstalledFixture {
    test::InstalledUnits& units;
    test::Seascape sea{
        64,
        64,
        installed_sea,
        installed_shelf,
        installed_land,
        installed_deep_floor,
        installed_shelf_floor,
        installed_land_height
    };
    Services services;
    Scenario scenario;
    std::unique_ptr<sim::match_runtime::Match> match;
    uint32_t clock = 1;

    /// Builds a match over the loaded types.
    ///
    /// @param loaded the installed types; each fixture's match uses them in turn
    explicit InstalledFixture(test::InstalledUnits& loaded) : units(loaded) {
        sim::match_runtime::OfflineInputs input{
            sea.map,
            units.loaded,
            units.types,
            units.fields,
            units.weapons,
            sea.terrain_values,
            sea.masks,
            32,
            32,
            32,
            2,
            0,
            30,
            1,
            &scenario,
            [] { return 1000u; },
            sea.plots,
            {},
            0,
            0,
            0.0F,
            units.features.defs
        };
        match = std::make_unique<sim::match_runtime::Match>(input, services);
        services.match = match.get();
        match->set_speech_hooks({&services, &Services::speak});
        match->configure_strategic_environment({0, 0.5F, 0});
        match->simulation().players[0].present = true;
        match->simulation().players[0].status = 1;
        std::array<uint8_t, 10> allies{};
        allies[0] = 1;
        match->configure_player_alliances(0, allies);
        match->configure_outcomes(0, allies, false);
    }

    sim::unit_spawn::Slot& spawn(std::string_view name, int32_t cell_x, int32_t cell_z) {
        auto* slot =
            match->create({0, units.type(name), at(cell_x * 16 + 8, cell_z * 16 + 8), true, 1, 0});
        CHECK(slot && slot->movement_object);
        return *slot;
    }

    void run(uint32_t ticks) {
        for (uint32_t i = 0; i < ticks; ++i) {
            match->simulation().tick = clock++;
            match->tick();
        }
    }

    template <typename Done>
    bool run_until(uint32_t limit, Done done) {
        for (uint32_t i = 0; i < limit; ++i) {
            if (done())
                return true;
            run(1);
        }
        return done();
    }

    bool can_load(const sim::unit_spawn::Slot& transport, const sim::unit_spawn::Slot& cargo) {
        return sim::gameplay_input::can_load_unit(match->state(), transport.record, cargo.record);
    }
};

bool carried_by(const sim::unit_spawn::Slot& cargo, const sim::unit_spawn::Slot& carrier) {
    return cargo.record.attach_parent != 0 &&
           oa::oa_unit_slot_from_ref(cargo.record.attach_parent) == carrier.unit_index;
}

// The transport ships lift a tank from the shelf into the hold with their own
// cranes, which reach it from the ship's turret.
void installed_ships_load_from_the_shore(test::InstalledUnits& units) {
    for (const auto* name : {"ARMTSHIP", "CORTSHIP"}) {
        InstalledFixture f(units);
        auto& ship = f.spawn(name, 34, 10);
        auto& tank = f.spawn("ARMSTUMP", 42, 10);
        CHECK(f.can_load(ship, tank));
        f.match->issue_load(ship.unit_index, tank.unit_index, false);
        CHECK(f.run_until(1500, [&] { return carried_by(tank, ship); }));
        CHECK(kind_of(tank) == be_carried);
        // In the hold the tank hangs from no piece.
        CHECK(f.run_until(600, [&] { return kind_of(ship) != ground_pickup; }));
        CHECK(static_cast<int8_t>(tank.record.attach_piece) == -1);
    }
}

// A drop where the cargo cannot stand, deep water for a tank, leaves it
// aboard: after three tries the order waits to retry.
void installed_ship_keeps_cargo_it_cannot_set_down(test::InstalledUnits& units) {
    InstalledFixture f(units);
    auto& ship = f.spawn("ARMTSHIP", 34, 10);
    auto& tank = f.spawn("ARMSTUMP", 42, 10);
    f.match->set_carry_link(tank.unit_index, ship.unit_index, -1, 0);
    f.match->issue_unload(ship.unit_index, point(32 * 16, 10 * 16), false);
    CHECK(f.run_until(3000, [&] {
        return ship.unit->primary && (ship.unit->primary->flags & order_retry);
    }));
    CHECK(kind_of(ship) == ground_unload && carried_by(tank, ship));
}

// A ship's capacity is its transportcapacity (the Envoy's 5, not the
// transportmaxunits key the game ignores); the check is made as the order is
// given, and queued pickups load past it.
void installed_capacity_is_checked_as_the_order_is_given(test::InstalledUnits& units) {
    InstalledFixture f(units);
    auto& ship = f.spawn("CORTSHIP", 34, 10);
    CHECK(f.units.definitions[ship.record.type_index].transport_capacity == 5);
    std::vector<sim::unit_spawn::Slot*> tanks;
    for (int32_t row = 0; row < 6; ++row)
        tanks.push_back(&f.spawn("ARMSTUMP", 42 + (row % 2) * 2, 6 + row * 2));
    for (auto* tank : tanks)
        CHECK(f.can_load(ship, *tank));
    for (auto* tank : tanks)
        f.match->issue_load(ship.unit_index, tank->unit_index, true);
    // With five aboard a sixth could not be ordered aboard; the pickup queued
    // before goes on.
    bool refused_sixth = false;
    CHECK(f.run_until(12000, [&] {
        if (f.match->loaded_child_count(ship.unit_index) == 5 && !carried_by(*tanks[5], ship))
            refused_sixth = refused_sixth || !f.can_load(ship, *tanks[5]);
        return std::all_of(tanks.begin(), tanks.end(), [&](const sim::unit_spawn::Slot* tank) {
            return carried_by(*tank, ship);
        });
    }));
    CHECK(refused_sixth && f.match->loaded_child_count(ship.unit_index) == 6);
    auto& seventh = f.spawn("ARMSTUMP", 44, 20);
    CHECK(!f.can_load(ship, seventh));
}

// Each UNLOAD sets down one unit, the last one loaded.
void installed_ship_sets_down_the_last_unit_loaded(test::InstalledUnits& units) {
    InstalledFixture f(units);
    auto& ship = f.spawn("ARMTSHIP", 34, 10);
    auto& first = f.spawn("ARMSTUMP", 42, 6);
    auto& last = f.spawn("ARMSTUMP", 42, 14);
    f.match->set_carry_link(first.unit_index, ship.unit_index, -1, 0);
    f.match->set_carry_link(last.unit_index, ship.unit_index, -1, 0);
    f.match->issue_unload(ship.unit_index, point(42 * 16, 10 * 16), false);
    CHECK(f.run_until(3000, [&] { return kind_of(ship) != ground_unload; }));
    CHECK(!carried_by(last, ship) && carried_by(first, ship));
}

// The Valkyrie carries one unit at a time.
void installed_valkyrie_carries_one_unit(test::InstalledUnits& units) {
    InstalledFixture f(units);
    auto& valkyrie = f.spawn("CORVALK", 50, 10);
    auto& first = f.spawn("ARMSTUMP", 54, 10);
    auto& second = f.spawn("ARMSTUMP", 50, 16);
    f.match->issue_load(valkyrie.unit_index, first.unit_index, false);
    CHECK(f.run_until(900, [&] { return carried_by(first, valkyrie); }));
    CHECK(!f.can_load(valkyrie, second));
    f.match->issue_load(valkyrie.unit_index, second.unit_index, false);
    f.run(30);
    CHECK(!carried_by(second, valkyrie) && carried_by(first, valkyrie));
}

// An air transport cannot lift a unit under the sea: the order is not given
// for one, and a pickup whose cargo is under the sea, as a sunk unit is, ends
// at once.
void installed_air_transport_leaves_sunk_units(test::InstalledUnits& units) {
    InstalledFixture f(units);
    auto& atlas = f.spawn("ARMATLAS", 50, 10);
    auto& sub = f.spawn("ARMSUB", 20, 10);
    f.run(2);
    CHECK(!f.can_load(atlas, sub));
    f.match->issue_load(atlas.unit_index, sub.unit_index, false);
    f.run(2);
    CHECK(kind_of(atlas) != vtol_pickup && !carried_by(sub, atlas));
    CHECK(f.services.captioned(atlas.unit_index, speech_failed, "Transport mission failed"));
}

// A carrier's death kills everything aboard as cargo (kind 6); a carrier
// that destroys itself kills its cargo with its own kind (3).
void installed_cargo_dies_with_its_carrier(test::InstalledUnits& units) {
    constexpr auto weapon = static_cast<uint8_t>(sim::match_runtime::DeathKind::weapon);
    constexpr auto self_destruct =
        static_cast<uint8_t>(sim::match_runtime::DeathKind::self_destruct);
    constexpr auto cargo = static_cast<uint8_t>(sim::match_runtime::DeathKind::cargo);
    for (const auto kind : {weapon, self_destruct}) {
        InstalledFixture f(units);
        auto& ship = f.spawn("ARMTSHIP", 34, 10);
        std::vector<sim::unit_spawn::Slot*> aboard;
        for (int32_t row = 0; row < 3; ++row) {
            aboard.push_back(&f.spawn("ARMSTUMP", 44, 6 + row * 4));
            f.match->set_carry_link(aboard.back()->unit_index, ship.unit_index, -1, 0);
        }
        f.match->apply_damage_event(ship, kind == self_destruct ? &ship : nullptr, 30000, kind, 0);
        // The kind each cargo unit was dealt, as its carrier's death dealt it.
        std::vector<uint8_t> kinds(aboard.size());
        CHECK(f.run_until(120, [&] {
            for (std::size_t index = 0; index < aboard.size(); ++index)
                if (kinds[index] == 0)
                    kinds[index] = aboard[index]->record.damage_kind;
            return std::all_of(aboard.begin(), aboard.end(), [](const sim::unit_spawn::Slot* unit) {
                return unit->record.type_index == 0;
            });
        }));
        for (const auto dealt : kinds)
            CHECK(dealt == (kind == self_destruct ? self_destruct : cargo));
    }
}

void installed_transports(const AssetStore& store) {
    test::InstalledUnits units(
        store, {"ARMTSHIP", "CORTSHIP", "CORVALK", "ARMTHOVR", "ARMATLAS", "ARMSTUMP", "ARMSUB"}
    );
    installed_ships_load_from_the_shore(units);
    installed_ship_keeps_cargo_it_cannot_set_down(units);
    installed_ship_sets_down_the_last_unit_loaded(units);
    installed_capacity_is_checked_as_the_order_is_given(units);
    installed_valkyrie_carries_one_unit(units);
    installed_air_transport_leaves_sunk_units(units);
    installed_cargo_dies_with_its_carrier(units);
}
} // namespace

int main(int argc, char** argv) {
    const bool installed_data = test::game_data_requested(argc, argv);
    const auto assets =
        installed_data
            ? std::optional<AssetStore>(test::require_game_assets("the installed transports"))
            : std::nullopt;
    try {
        const AssetStore* installed = assets ? &*assets : nullptr;
        air_transport_carries_a_unit_to_the_unload_point(installed);
        if (installed == nullptr) {
            air_transport_flies_the_game_goals();
            air_transport_waits_for_a_clear_unload_point();
            hover_transport_refuses_a_unit_too_large();
            hover_transport_gives_up_after_three_attempts();
            speech_without_hooks_plays_the_category();
            air_transport_settles_on_the_pad_piece();
            loaded_air_transport_hands_its_cargo_to_the_pad();
            air_transport_follows_a_moving_pad();
            landing_says_why_it_failed();
        }
        air_transport_capacity_is_enforced(installed);
        loaded_air_transport_levels_out_while_hovering(installed);
        carried_units_stay_in_reach_of_ground_guns(installed);
        carried_units_leave_a_dead_carrier(installed);
        hover_transport_loads_and_unloads_through_its_scripts(installed);
        hover_transport_closes_in_on_a_unit_out_of_reach(installed);
        if (installed != nullptr)
            installed_transports(*installed);
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    std::cout << (installed_data ? "installed transports ok\n" : "transports ok\n");
    return 0;
}
