// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Two offline match runtimes joined through the match binding over an
// in-memory wire: each creates its own commander (sent as 0x09), plays 300
// networked ticks with a move order, and the owner's 0x2c stream steps the
// other machine's copy through the owner's own positions; a relayed health
// event reaches the owner. The remote driver alone brakes without a route
// and is held in its cell while blocked, a remote aircraft flies its
// owner's goal and layer, and a carried copy keeps its driver. A unit dies
// on the other machine as its owner killed it (0x0c: its wreck, Killed
// pieces and credit), a 0x09 into a slot still holding a copy kills that
// copy by its own health, and a weapon hit on a feature goes to the host,
// which applies it and has every machine start the die sequence.

#include "oa/netgame/condenser.hpp"
#include "oa/netgame/match/match_binding.hpp"
#include "oa/sim/air/driver.hpp"
#include "oa/sim/air/goal.hpp"
#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/match_runtime/construction_orders.hpp"
#include "oa/sim/weapon_execution/interceptor.hpp"
#include "oa/sim/weapon_execution/projectile_pool.hpp"
#include "oa/sim/weapon_execution/weapon_launch.hpp"
#include "oa/sim/world_environment/wind.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <initializer_list>
#include <memory>
#include <vector>
#include "oa/test/match_services.hpp"

using namespace oa;
using namespace oa::netgame;
using namespace oa::netgame::match;

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            ++failures;                                                                            \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
        }                                                                                          \
    } while (0)

constexpr uint16_t kUnitsPerPlayer = 4;
constexpr uint8_t kGun = 1;  // a line-of-sight turret weapon
constexpr uint8_t kRock = 2; // a meteor weapon
constexpr uint16_t kAircraftType = 2;
// The naval machines' hovercraft and floating types, and their sea.
constexpr uint16_t kHoverType = 3;
constexpr uint16_t kFloaterType = 4;
constexpr uint32_t kNavalSeaLevel = 60;
constexpr int8_t kFloaterWaterline = 4;
constexpr uint32_t kUptimeMilliseconds = 1000;
// The transport machines' air transport, air pad and cargo types. The cargo
// stands kCargoHeight units high; the pad's landing piece sits on its deck.
constexpr uint16_t kTransportType = 5;
constexpr uint16_t kPadType = 6;
constexpr uint16_t kCargoType = 7;
constexpr int32_t kCargoHeight = 10;
constexpr int32_t kDeckHeight = 12;
constexpr int32_t kLinkPiece = 1;
constexpr uint8_t kVtolPickupKind = 57; // VTOL_Pickup, the mission index a flying pickup runs
constexpr uint16_t kMapCells = 16;      // attribute cells along each side
constexpr uint32_t kIds[2] = {0x2001, 0x1003}; // the joiner's id sorts first
constexpr uint8_t kWeaponKind = 1;             // Unit.damage_kind of a weapon hit
constexpr uint8_t kCarriedDeathKind = 6;       // the death kind of cargo whose carrier died
// The killable type's corpse chain: its corpse steps along featuredead to
// the heap, and the heap to nothing, as a solar collector's does.
constexpr uint16_t kDeadFeature = 0;
constexpr uint16_t kHeapFeature = 1;
constexpr uint16_t kCorpseHitPoints = 100;
constexpr uint32_t kRootPiece = 0;

struct Datagram {
    uint32_t from{};
    uint32_t to{};
    std::vector<uint8_t> bytes;
};

// One record a machine sent, with its datagram's addresses.
struct SentRecord {
    uint32_t from{};
    uint32_t to{};
    std::vector<uint8_t> bytes; // type byte first
};

struct Inbox {
    std::deque<Datagram> queue;
    Inbox* peer{};
    std::vector<Datagram> sent; // every datagram this machine sent, oldest first
};

uint32_t inbox_send(
    void* context, uint32_t from, uint32_t to, uint32_t, const uint8_t* data, uint32_t size
) {
    auto* inbox = static_cast<Inbox*>(context);
    Datagram datagram{from, to, std::vector<uint8_t>(data, data + size)};
    inbox->sent.push_back(datagram);
    inbox->peer->queue.push_back(std::move(datagram));
    return transport_result::ok;
}

// The records of the datagrams a machine sent, unwrapped as the receiver's
// condenser and frame layer unwrap them.
std::vector<SentRecord> sent_records(const Inbox& inbox) {
    struct One {
        const Datagram* datagram{};
        bool done{};
    };

    std::vector<SentRecord> out;
    auto condenser = std::make_unique<Condenser>();
    std::vector<uint8_t> frame(condenser_decoded_bytes);
    for (const auto& datagram : inbox.sent) {
        One one{&datagram, false};
        NetTransport transport{};
        transport.context = &one;
        transport.receive =
            [](void* c, uint32_t* from, uint32_t* to, uint8_t* buffer, uint32_t* size) -> uint32_t {
            auto* o = static_cast<One*>(c);
            if (o->done)
                return transport_result::no_messages;
            if (o->datagram->bytes.size() > *size)
                return transport_result::buffer_too_small;
            std::memcpy(buffer, o->datagram->bytes.data(), o->datagram->bytes.size());
            *size = static_cast<uint32_t>(o->datagram->bytes.size());
            *from = o->datagram->from;
            *to = o->datagram->to;
            o->done = true;
            return transport_result::ok;
        };
        uint32_t from = 0;
        uint32_t to = 0;
        auto size = static_cast<uint32_t>(frame.size());
        if (condenser_receive(
                condenser.get(), transport, nullptr, &from, &to, frame.data(), &size
            ) != transport_result::ok) {
            ++failures;
            std::fprintf(stderr, "a sent datagram does not unwrap\n");
            continue;
        }
        for (std::size_t at = frame_header_bytes; at < size;) {
            uint16_t length = 0;
            const auto error = record_wire_length(frame.data() + at, size - at, &length);
            if (error == WireError::invalid_type) {
                if (wide_record_wire_length(frame.data() + at, size - at, &length) !=
                        WireError::ok ||
                    length == 0)
                    break;
            } else if (error != WireError::ok || length == 0) {
                break;
            }
            out.push_back(
                {from, to, std::vector<uint8_t>(frame.data() + at, frame.data() + at + length)}
            );
            at += length;
        }
    }
    return out;
}

std::size_t count_sent_type(const Inbox& inbox, uint8_t type) {
    std::size_t count = 0;
    for (const auto& record : sent_records(inbox))
        if (!record.bytes.empty() && record.bytes[0] == type)
            ++count;
    return count;
}

// The records of one type a machine sent.
std::vector<SentRecord> sent_of(const Inbox& inbox, RecordType type) {
    std::vector<SentRecord> out;
    for (auto& record : sent_records(inbox))
        if (record.bytes[0] == static_cast<uint8_t>(type))
            out.push_back(std::move(record));
    return out;
}

uint32_t
inbox_receive(void* context, uint32_t* from, uint32_t* to, uint8_t* buffer, uint32_t* size) {
    auto* inbox = static_cast<Inbox*>(context);
    if (inbox->queue.empty())
        return transport_result::no_messages;
    const auto& d = inbox->queue.front();
    if (d.bytes.size() > *size)
        return transport_result::buffer_too_small;
    std::memcpy(buffer, d.bytes.data(), d.bytes.size());
    *size = static_cast<uint32_t>(d.bytes.size());
    *from = d.from;
    *to = d.to;
    inbox->queue.pop_front();
    return transport_result::ok;
}

// One EXPLODE a unit's script ran: the unit, the piece and the flags.
struct PieceExplosion {
    uint16_t unit{};
    uint32_t piece{};
    int32_t flags{};

    bool operator==(const PieceExplosion&) const = default;
};

struct Services : oa::test::QuietServices {
    std::vector<PieceExplosion> explosions;

    void explode_piece(sim::unit_spawn::Slot& slot, uint32_t piece, int32_t flags) override {
        explosions.push_back({slot.unit_index, piece, flags});
    }
};

using Scenario = oa::test::EmptyScenario;

/// Appends Killed(severity, corpsetype) with a solar collector's thresholds:
/// corpsetype 1 up to severity 25, 2 up to 50 and 3 above. It explodes the
/// root piece once, with the corpsetype as the flags.
///
/// @param[in,out] script Program that gains the function.
void add_killed_script(formats::cob::CobProgram& script) {
    using namespace sim::script_vm;
    const auto killed = static_cast<uint32_t>(script.code.size());
    const uint32_t over_25 = killed + 11;
    const uint32_t over_50 = killed + 22;
    const uint32_t chosen = killed + 24;
    script.code.insert(
        script.code.end(),
        {opcode::push_local,
         0,
         opcode::push_constant,
         25,
         opcode::less_equal,
         opcode::jump_if_false,
         over_25,
         opcode::push_constant,
         1,
         opcode::jump,
         chosen,
         opcode::push_local,
         0,
         opcode::push_constant,
         50,
         opcode::less_equal,
         opcode::jump_if_false,
         over_50,
         opcode::push_constant,
         2,
         opcode::jump,
         chosen,
         opcode::push_constant,
         3,
         opcode::pop_local,
         1,
         opcode::push_local,
         1,
         opcode::explode,
         kRootPiece,
         opcode::return_}
    );
    script.scripts.push_back({"Killed", killed});
    script.entry_points.push_back(killed);
}

/// Builds a unit script of the named functions, each starting at its code's
/// first word; the pieces are the transport models' base and link.
///
/// @param functions each function's name and code
/// @return the program
std::shared_ptr<formats::cob::CobProgram>
assemble(std::initializer_list<std::pair<const char*, std::vector<uint32_t>>> functions) {
    auto cob = std::make_shared<formats::cob::CobProgram>();
    for (const auto& [name, code] : functions) {
        const auto entry = static_cast<uint32_t>(cob->code.size());
        cob->scripts.push_back({name, entry});
        cob->entry_points.push_back(entry);
        cob->code.insert(cob->code.end(), code.begin(), code.end());
    }
    cob->header.script_count = static_cast<uint32_t>(cob->scripts.size());
    cob->header.static_variable_count = 2;
    cob->piece_names = {"base", "link"};
    return cob;
}

// One machine: a square map, a ground and a flying unit type, optionally a
// structure type, and the network side. A killable machine's types carry the
// Killed script and leave the corpse chain.
struct Machine {
    uint16_t cells;
    formats::tnt::Map map;
    std::vector<sim::visibility_state::TerrainCell> terrain_values;
    std::array<sim::visibility_state::SightMask, 1> masks{};
    std::shared_ptr<formats::objects3d::Model> model =
        std::make_shared<formats::objects3d::Model>();
    // The hovercraft's model: the root with a selection square, which the
    // ground fit reads.
    std::shared_ptr<formats::objects3d::Model> hover_model =
        std::make_shared<formats::objects3d::Model>();
    std::shared_ptr<formats::cob::CobProgram> script = std::make_shared<formats::cob::CobProgram>();
    // The transport machines' models: a base with a link piece under it, and
    // an air pad's deck with its landing piece on top.
    std::shared_ptr<formats::objects3d::Model> carrier_model =
        std::make_shared<formats::objects3d::Model>();
    std::shared_ptr<formats::objects3d::Model> deck_model =
        std::make_shared<formats::objects3d::Model>();
    std::shared_ptr<formats::cob::CobProgram> carrier_script, pad_script, cargo_script;
    std::vector<sim::unit_spawn::LoadedType> loaded;
    std::vector<sim::unit_spawn::Type> types;
    data::unit_definitions::UnitDefinition def;
    data::unit_definitions::UnitDefinition air_def;
    data::unit_definitions::UnitDefinition structure_def;
    data::unit_definitions::UnitDefinition hover_def;
    data::unit_definitions::UnitDefinition floater_def;
    data::unit_definitions::UnitDefinition transport_def;
    data::unit_definitions::UnitDefinition pad_def;
    data::unit_definitions::UnitDefinition cargo_def;
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    data::unit_definitions::RuntimeDefinitionMetadata metadata;
    std::vector<sim::spatial_state::Plot> collision_plots;
    std::vector<sim::match_runtime::RuntimeTypeFields> fields;
    std::array<uint8_t, 1> yard{4};
    // The rules a mod's profile sets, and each unit type's own; 3.1c's
    // unless a test sets them before start.
    data::match_rules::MatchRules rules{};
    std::vector<data::match_rules::UnitTypeRules> unit_type_rules;
    sim::combat_state::WeaponRegistry weapons;
    std::vector<FeatureDef> feature_defs;
    Services services;
    Scenario scenario;
    std::unique_ptr<sim::match_runtime::Match> match;
    Inbox inbox;
    NetConnection connection{};
    std::unique_ptr<NetMatch> net = std::make_unique<NetMatch>();
    MatchBinding binding;

    explicit Machine(
        uint16_t structure_type = 0,
        uint16_t side_cells = kMapCells,
        bool armed = false,
        bool killable = false,
        bool naval = false,
        bool transports = false
    )
        : cells(side_cells), terrain_values(std::size_t{side_cells} * side_cells),
          loaded(
              std::max<std::size_t>(
                  transports ? kCargoType + 1u : (naval ? kFloaterType + 1u : 3u),
                  structure_type + 1u
              )
          ),
          types(loaded.size()), collision_plots(terrain_values.size()), fields(loaded.size()) {
        map.attribute_width = map.attribute_height = cells;
        map.attributes.resize(terrain_values.size());
        masks[0].width = masks[0].height = 1;
        masks[0].pixels = {1};
        model->objects.resize(1);
        model->objects[0].name = "root";
        script->code = {sim::script_vm::opcode::return_, sim::script_vm::opcode::return_};
        script->scripts = {{"Create", 0}, {"StartBuilding", 1}};
        script->entry_points = {0, 1};
        script->piece_names = {"root"};
        if (killable) {
            add_killed_script(*script);
            feature_defs.resize(2);
            for (auto& feature : feature_defs) {
                feature.footprint_x = feature.footprint_z = 1;
                feature.dead_feature = sim::feature_runtime::no_feature;
                feature.burnt_feature = sim::feature_runtime::no_feature;
                feature.reclamate_feature = sim::feature_runtime::no_feature;
            }
            feature_defs[kDeadFeature].dead_feature = kHeapFeature;
            feature_defs[kDeadFeature].damage = kCorpseHitPoints;
            fields[1].corpse_feature = static_cast<int16_t>(kDeadFeature);
        }
        loaded[1].model = model;
        loaded[1].script = script;
        types[1].simulation.flags = 0x800000;
        types[1].simulation.maximum_health = 100;
        types[1].footprint_x = types[1].footprint_z = 1;
        types[1].model = reinterpret_cast<uintptr_t>(model.get());
        types[1].cob = reinterpret_cast<uintptr_t>(script.get());
        types[1].bm_code = 1;
        loaded[1].type = types[1];
        def.sight_distance = 160;
        def.acceleration_fixed = 65536;
        def.brake_rate_fixed = 65536;
        def.max_velocity_fixed = 2 * 65536;
        def.turn_rate = 1024;
        def.energy_storage = 1000.0F;
        def.metal_storage = 1000.0F;
        if (armed) {
            (void)sim::combat_state::install_weapon_text(
                weapons,
                std::string(
                    "[TESTGUN]{id=" + std::to_string(kGun) +
                    "; reloadtime=0.1; range=400; lineofsight=1; weaponvelocity=100; turret=1; "
                    "[DAMAGE]{default=10;}}"
                )
            );
            (void)sim::combat_state::install_weapon_text(
                weapons, "[TESTROCK]{id=" + std::to_string(kRock) + "; reloadtime=1; meteor=1;}"
            );
            def.weapon1 = "TESTGUN";
        }
        fields[1].definition = &def;
        fields[1].yard_mask = yard;
        fields[1].runtime_metadata = &metadata;
        fields[1].target_masks = &target_masks;
        fields[1].movement_class = 0;
        types[kAircraftType] = types[1];
        types[kAircraftType].simulation.flags |= OA_UNIT_DEF_FLAG_CAN_FLY;
        loaded[kAircraftType] = loaded[1];
        loaded[kAircraftType].type = types[kAircraftType];
        air_def = def;
        air_def.can_fly = true;
        air_def.cruise_altitude = 60;
        fields[kAircraftType] = fields[1];
        fields[kAircraftType].definition = &air_def;
        if (naval) {
            // Every cell lies under the sea: a hovercraft rides its surface
            // and a floater sits its waterline below it.
            map.sea_level = kNavalSeaLevel;
            *hover_model = *model;
            auto& hover_root = hover_model->objects[0];
            constexpr int32_t kHalfSide = 8 << 16;
            hover_root.vertices = {
                {-kHalfSide, 0, -kHalfSide},
                {kHalfSide, 0, -kHalfSide},
                {kHalfSide, 0, kHalfSide},
                {-kHalfSide, 0, kHalfSide},
            };
            hover_root.primitives.resize(1);
            hover_root.primitives[0].vertex_indices = {0, 1, 2, 3};
            hover_root.selection_primitive = 0;
            types[kHoverType] = types[1];
            types[kHoverType].simulation.flags |= OA_UNIT_DEF_FLAG_CAN_HOVER;
            types[kHoverType].model = reinterpret_cast<uintptr_t>(hover_model.get());
            loaded[kHoverType] = loaded[1];
            loaded[kHoverType].model = hover_model;
            loaded[kHoverType].type = types[kHoverType];
            hover_def = def;
            hover_def.can_hover = true;
            fields[kHoverType] = fields[1];
            fields[kHoverType].definition = &hover_def;
            types[kFloaterType] = types[1];
            types[kFloaterType].simulation.flags |= OA_UNIT_DEF_FLAG_FLOATER;
            types[kFloaterType].simulation.waterline_offset =
                static_cast<uint8_t>(kFloaterWaterline);
            loaded[kFloaterType] = loaded[1];
            loaded[kFloaterType].type = types[kFloaterType];
            floater_def = def;
            floater_def.floater = true;
            floater_def.waterline = kFloaterWaterline;
            fields[kFloaterType] = fields[1];
            fields[kFloaterType].definition = &floater_def;
        }
        if (transports)
            add_transport_types();
        if (structure_type == 0)
            return;
        types[structure_type] = types[1];
        types[structure_type].bm_code = 0;
        loaded[structure_type] = loaded[1];
        loaded[structure_type].type = types[structure_type];
        structure_def = def;
        structure_def.acceleration_fixed = 0;
        structure_def.brake_rate_fixed = 0;
        structure_def.max_velocity_fixed = 0;
        structure_def.turn_rate = 0;
        fields[structure_type] = fields[1];
        fields[structure_type].definition = &structure_def;
        fields[structure_type].movement_class.reset();
    }

    /// Adds an air transport, an air pad and a cargo type, each with the
    /// scripts its missions call: QueryTransport answers the link piece and
    /// BeginTransport keeps its argument in static 0; QueryLandingPad offers
    /// the landing piece.
    void add_transport_types() {
        namespace op = sim::script_vm::opcode;
        const std::vector<uint32_t> finish{op::push_constant, 0, op::return_};
        carrier_model->objects.resize(2);
        carrier_model->objects[0].name = "base";
        carrier_model->objects[0].vertices = {{0, 0, 0}, {0, kCargoHeight << 16, 0}};
        carrier_model->objects[0].first_child = 1;
        carrier_model->objects[1].name = "link";
        carrier_model->objects[1].parent = 0;
        carrier_model->objects[1].offset_from_parent = {0, -(8 << 16), 0};
        *deck_model = *carrier_model;
        deck_model->objects[0].vertices = {{0, 0, 0}, {0, kDeckHeight << 16, 0}};
        deck_model->objects[1].offset_from_parent = {0, kDeckHeight << 16, 0};
        // A query answering statics 0 and 1 in locals 0 and 1.
        const std::vector<uint32_t> statics = [&] {
            std::vector<uint32_t> code{
                op::push_static, 0u, op::pop_local, 0u, op::push_static, 1u, op::pop_local, 1u
            };
            const std::size_t queries = code.size();
            code.resize(queries + finish.size());
            std::copy(
                finish.begin(), finish.end(), code.begin() + static_cast<std::ptrdiff_t>(queries)
            );
            return code;
        }();
        cargo_script = assemble({{"Create", finish}});
        carrier_script = assemble({
            {"Create", finish},
            {"QueryTransport",
             {op::push_constant, kLinkPiece, op::pop_local, 0, op::push_constant, 0, op::return_}},
            {"BeginTransport",
             {op::push_local, 0, op::pop_static, 0, op::push_constant, 0, op::return_}},
            {"EndTransport", finish},
            {"Statics", statics},
        });
        pad_script = assemble({
            {"Create", finish},
            {"QueryLandingPad",
             {op::push_constant, kLinkPiece, op::pop_local, 0, op::push_constant, 0, op::return_}},
        });
        const auto add = [&](uint16_t type,
                             const std::shared_ptr<formats::objects3d::Model>& shape,
                             const std::shared_ptr<formats::cob::CobProgram>& code,
                             data::unit_definitions::UnitDefinition& definition) {
            types[type] = types[1];
            types[type].footprint_x = types[type].footprint_z = 2;
            types[type].model = reinterpret_cast<uintptr_t>(shape.get());
            types[type].cob = reinterpret_cast<uintptr_t>(code.get());
            loaded[type] = loaded[1];
            loaded[type].model = shape;
            loaded[type].script = code;
            definition = def;
            definition.footprint_x = definition.footprint_z = 2;
            fields[type] = fields[1];
            fields[type].definition = &definition;
        };
        add(kCargoType, carrier_model, cargo_script, cargo_def);
        add(kTransportType, carrier_model, carrier_script, transport_def);
        types[kTransportType].simulation.flags |= OA_UNIT_DEF_FLAG_CAN_FLY;
        types[kTransportType].simulation.abilities |= OA_UNIT_DEF_ABILITY_CAN_LOAD;
        transport_def.can_fly = true;
        transport_def.can_load = true;
        transport_def.cant_be_transported = true;
        transport_def.cruise_altitude = 60;
        transport_def.max_velocity_fixed = 4 * 65536;
        transport_def.transport_size = 3;
        transport_def.transport_capacity = 1;
        add(kPadType, deck_model, pad_script, pad_def);
        types[kPadType].simulation.flags |= OA_UNIT_DEF_FLAG_IS_AIRBASE;
        pad_def.is_airbase = true;
        pad_def.max_velocity_fixed = 0;
        for (const uint16_t type : {kCargoType, kTransportType, kPadType})
            loaded[type].type = types[type];
    }

    void start(uint8_t local_slot) {
        const int32_t sight_cells = cells / 2;
        sim::match_runtime::OfflineInputs input{
            map,
            loaded,
            types,
            fields,
            weapons,
            terrain_values,
            masks,
            sight_cells,
            sight_cells,
            kUnitsPerPlayer,
            2,
            local_slot,
            30,
            1,
            &scenario,
            {},
            collision_plots,
            {}
        };
        input.feature_defs = feature_defs;
        input.rules = rules;
        input.unit_type_rules = unit_type_rules;
        // Both machines read the same uptime, so a hovercraft's bob agrees.
        input.uptime_milliseconds = [] { return kUptimeMilliseconds; };
        match = std::make_unique<sim::match_runtime::Match>(input, services);
        match->configure_strategic_environment({0, 0.5f, 0});
        auto& world = match->state();
        world.game.map_pixel_width = world.game.map_pixel_height = cells * 16;
        for (auto& player : world.game.players)
            player.player_id = no_player_id; // free slots, as a launch leaves them
        for (uint8_t slot = 0; slot < 2; ++slot) {
            auto& player = world.game.players[slot];
            player.in_use = 1;
            player.index = slot;
            player.player_id = kIds[slot];
            player.status = slot == local_slot ? OA_PLAYER_STATUS_LOCAL : OA_PLAYER_STATUS_MIRRORED;
            player.info = oa_ref_from_index(slot);
        }
        world.game.local_player_index = local_slot;
        world.game.unit_def_id_bits = unit_def_id_bits_for_count(world.unit_def_count);
        world.game.session_flags |= kNetFlagLive | kNetFlagGameStarted;
        std::array<uint8_t, 10> allies{};
        allies[local_slot] = 1;
        match->configure_outcomes(local_slot, allies, false);
        for (uint8_t slot = 0; slot < 2; ++slot) {
            std::array<uint8_t, 10> own{};
            own[slot] = 1;
            match->configure_player_alliances(slot, own);
        }
        connection.packets = new PacketLayer();
        packet_layer_create(connection.packets);
        packet_layer_start(connection.packets, NetTransport{&inbox, inbox_send, inbox_receive});
        match_binding_init(&binding, match.get(), net.get());
        net_match_begin(
            net.get(),
            &connection,
            &world,
            match_binding_sim(&binding),
            match_binding_hooks(&binding)
        );
        match_binding_install(&binding);
        net_match_enter_game(net.get());
    }

    ~Machine() { net_connection_destroy(&connection); }
};

void step(Machine& m) {
    try {
        match_binding_tick(&m.binding);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "tick %u: %s\n", m.match->state().game.tick, error.what());
    }
    packet_layer_flush(m.connection.packets, 0, true);
}

// A unit's movement as a 0x2c record for one tick leaves it.
struct Pose {
    int32_t x{};
    int32_t y{};
    int32_t z{};
    uint16_t heading{};
    int32_t speed{};

    bool operator==(const Pose&) const = default;
};

Pose pose_of(Machine& m, uint16_t index) {
    const auto& unit = m.match->state().units[index];
    const auto* ground = m.match->ground_runtime(index);
    return {
        unit.position.x,
        unit.position.y,
        unit.position.z,
        unit.heading,
        ground != nullptr ? ground->movement.speed : 0
    };
}

// The owner's pose of one unit at each of its ticks, and how far the other
// machine's copy strays from the pose of the last record it applied.
struct Divergence {
    std::vector<Pose> owner; // by owner tick
    uint32_t compared{};
    uint32_t moving{};
    uint32_t differing{};

    void note_owner(Machine& m, uint16_t index) {
        const auto tick = m.match->state().game.tick;
        if (owner.size() <= tick)
            owner.resize(tick + 1);
        owner[tick] = pose_of(m, index);
    }

    void compare(Machine& copy, uint8_t owner_slot, uint16_t index) {
        if (copy.match->ground_runtime(index) == nullptr)
            return;
        const auto tick = copy.match->state().game.players[owner_slot].last_sim_tick;
        if (tick <= 0 || static_cast<std::size_t>(tick) >= owner.size())
            return;
        const auto& expected = owner[static_cast<std::size_t>(tick)];
        const auto seen = pose_of(copy, index);
        ++compared;
        moving += expected.speed != 0 ? 1u : 0u;
        if (seen == expected)
            return;
        if (differing++ == 0)
            std::fprintf(
                stderr,
                "tick %d: copy (%d, %d, %d) heading %u speed %d, "
                "owner (%d, %d, %d) heading %u speed %d\n",
                tick,
                seen.x,
                seen.y,
                seen.z,
                seen.heading,
                seen.speed,
                expected.x,
                expected.y,
                expected.z,
                expected.heading,
                expected.speed
            );
    }
};

bool same_units(Machine& a, Machine& b, uint32_t* live) {
    auto& wa = a.match->state();
    auto& wb = b.match->state();
    bool same = true;
    *live = 0;
    for (uint32_t i = 1; i < wa.unit_slot_count; ++i) {
        const auto& x = wa.units[i];
        const auto& y = wb.units[i];
        if (x.type_index == 0 && y.type_index == 0)
            continue;
        ++*live;
        same = same && x.type_index == y.type_index && x.position.x == y.position.x &&
               x.position.z == y.position.z && x.health == y.health;
    }
    return same;
}

struct Pair {
    std::unique_ptr<Machine> host;
    std::unique_ptr<Machine> joiner;

    explicit Pair(
        uint16_t structure_type = 0,
        uint16_t cells = kMapCells,
        bool armed = false,
        bool killable = false,
        bool naval = false,
        bool transports = false
    )
        : host(
              std::make_unique<Machine>(structure_type, cells, armed, killable, naval, transports)
          ),
          joiner(
              std::make_unique<Machine>(structure_type, cells, armed, killable, naval, transports)
          ) {
        host->inbox.peer = &joiner->inbox;
        joiner->inbox.peer = &host->inbox;
        host->start(0);
        joiner->start(1);
    }
};

void replicated_play() {
    Pair pair;
    auto& host = pair.host;
    auto& joiner = pair.joiner;

    // Ranges follow the player ids: the joiner (lower id) numbers first.
    CHECK(host->match->state().game.players[1].first_unit == oa_ref_from_index(1));
    CHECK(
        host->match->state().game.players[0].first_unit == oa_ref_from_index(kUnitsPerPlayer + 1)
    );
    CHECK(
        joiner->match->state().game.players[1].first_unit ==
        host->match->state().game.players[1].first_unit
    );

    auto* host_unit = host->match->create({0, 1, {48u << 16, 0, 48u << 16}, true, 1, 0});
    auto* joiner_unit = joiner->match->create({1, 1, {176u << 16, 0, 176u << 16}, true, 1, 0});
    CHECK(host_unit != nullptr && joiner_unit != nullptr);
    if (host_unit == nullptr || joiner_unit == nullptr)
        return;
    const auto host_index = host_unit->unit_index;
    const auto joiner_index = joiner_unit->unit_index;

    bool saw_route = false;
    bool saw_speed = false;
    Divergence divergence;
    for (uint32_t t = 0; t < 300; ++t) {
        if (t == 30) {
            try {
                (void)host->match->issue_ground_move(
                    host_index, {48 * 65536, 0, 96 * 65536}, false
                );
            } catch (const std::exception& error) {
                ++failures;
                std::fprintf(stderr, "order: %s\n", error.what());
            }
        }
        step(*host);
        divergence.note_owner(*host, host_index);
        step(*joiner);
        divergence.compare(*joiner, 0, host_index);
        // The owner's route head reaches the joiner's remote navigator, and
        // each full record the copy's speed.
        const auto* copy = joiner->match->ground_runtime(host_index);
        const auto* owner = host->match->ground_runtime(host_index);
        if (copy != nullptr && owner != nullptr) {
            saw_route = saw_route || copy->mirrored_navigation.count > 1;
            saw_speed = saw_speed || (copy->movement.speed != 0 &&
                                      copy->movement.speed == owner->movement.speed);
        }
    }
    CHECK(saw_route && saw_speed);
    CHECK(divergence.compared >= 290 && divergence.moving > 10);
    CHECK(divergence.differing == 0);
    CHECK(host->binding.created_remote == 1 && joiner->binding.created_remote == 1);
    CHECK(host->net->record_errors == 0 && joiner->net->record_errors == 0);
    CHECK(host->net->records_applied >= 290 && joiner->net->records_applied >= 290);
    uint32_t live = 0;
    bool agreed = false;
    for (uint32_t t = 0; t < 40 && !agreed; ++t) {
        step(*host);
        step(*joiner);
        agreed = same_units(*host, *joiner, &live);
    }
    CHECK(agreed && live == 2);
    const auto& moved = host->match->state().units[host_index];
    CHECK(moved.position.z != static_cast<int32_t>(48u << 16));
    CHECK(joiner->match->state().units[host_index].position.z == moved.position.z);

    // Damage to the joiner's unit on the host's machine is relayed (0x0b);
    // the owner applies it and its full record brings the host's copy along.
    auto& host_world = host->match->state();
    sim::unit_health::HealthEvent event{};
    event.target = joiner_index;
    event.source = host_index;
    event.amount = 9;
    event.kind = 1;
    auto& multiplayer = host->match->multiplayer;
    multiplayer.health_shared(
        multiplayer.context, multiplayer.health_route(multiplayer.context, host_index), event
    );
    bool relayed = false;
    for (uint32_t t = 0; t < 40 && !relayed; ++t) {
        step(*host);
        step(*joiner);
        relayed = joiner->match->state().units[joiner_index].health == 91 &&
                  host_world.units[joiner_index].health == 91;
    }
    CHECK(relayed);

    // Resources given to the remote player are credited on arrival.
    auto& joiner_world = joiner->match->state();
    auto* staging = world_player_economy(&joiner_world, &joiner_world.game.players[1]);
    const float before = staging != nullptr ? staging->metal.produced : 0.0F;
    host_world.game.players[0].metal = 500.0F;
    net_match_give(host->net.get(), 0, 1, true, 50.0F);
    for (uint32_t t = 0; t < 4; ++t) {
        packet_layer_flush(host->connection.packets, 0, true);
        (void)net_match_pump(joiner->net.get());
    }
    CHECK(staging != nullptr && staging->metal.produced >= before + 50.0F - 0.01F);
    CHECK(host_world.game.players[0].metal == 450.0F);
}

// The joiner's copy of the host's unit, driven through the joiner's
// replication hooks as a received record would.
struct RemoteCopy {
    Machine& machine;
    ReplicationSim sim;
    Unit* unit;
    const sim::ground_orders::GroundRuntime* ground;

    RemoteCopy(Machine& m, uint16_t index)
        : machine(m), sim(match_binding_sim(&m.binding)), unit(&m.match->state().units[index]),
          ground(m.match->ground_runtime(index)) {}

    void route(bool blocked, std::initializer_list<std::array<int16_t, 2>> points) {
        WaypointDelta delta{};
        delta.flag = blocked;
        for (const auto& point : points) {
            delta.points[delta.count][0] = point[0];
            delta.points[delta.count][1] = point[1];
            ++delta.count;
        }
        sim.apply_ground_delta(sim.context, &machine.match->state(), unit, delta);
    }

    void record() {
        sim.movement_tick(sim.context, &machine.match->state(), unit);
        sim.sight_update(sim.context, &machine.match->state(), unit);
    }
};

void remote_driver_brakes_and_blocks() {
    Pair pair;
    auto* owned = pair.host->match->create({0, 1, {40u << 16, 0, 40u << 16}, true, 1, 0});
    CHECK(owned != nullptr);
    if (owned == nullptr)
        return;
    const auto index = owned->unit_index;
    for (uint32_t t = 0; t < 4; ++t) {
        step(*pair.host);
        step(*pair.joiner);
    }
    RemoteCopy copy(*pair.joiner, index);
    CHECK(copy.ground != nullptr && copy.ground->mirrored_driver);
    if (copy.ground == nullptr)
        return;
    const auto top_speed = pair.joiner->def.max_velocity_fixed;

    // A route head turns the copy onto it and gets it up to speed.
    copy.route(false, {{40, 40}, {40, 220}});
    for (uint32_t t = 0; t < 60; ++t)
        copy.record();
    CHECK(copy.ground->movement.speed == top_speed && copy.unit->position.z > (100 << 16));

    // No route: it brakes to a stop and stays there.
    copy.route(false, {});
    for (uint32_t t = 0; t < 4; ++t)
        copy.record();
    CHECK(copy.ground->movement.speed == 0);
    const auto stopped = copy.unit->position;
    copy.record();
    CHECK(copy.unit->position.x == stopped.x && copy.unit->position.z == stopped.z);

    // Blocked: pressing on toward the next cell holds it inside its own,
    // cut to half its top speed each time it reaches the edge.
    const auto cell_x = copy.unit->cell_x;
    const auto cell_z = copy.unit->cell_z;
    const auto x = static_cast<int16_t>(stopped.x >> 16);
    const auto z = static_cast<int16_t>(stopped.z >> 16);
    copy.route(true, {{x, z}, {x, static_cast<int16_t>(z + 60)}});
    bool held = true;
    for (uint32_t t = 0; t < 24; ++t) {
        copy.record();
        held = held && copy.unit->cell_x == cell_x && copy.unit->cell_z == cell_z;
    }
    CHECK(held && copy.ground->movement.speed == top_speed / 2);
    CHECK(copy.unit->position.z > stopped.z);
}

// The owner flies its aircraft to a point; the joiner's copy takes off with
// it and follows its path record by record.
void remote_aircraft_follow_their_owner() {
    Pair pair;
    auto* owned =
        pair.host->match->create({0, kAircraftType, {60u << 16, 0, 60u << 16}, true, 1, 0});
    CHECK(owned != nullptr);
    if (owned == nullptr)
        return;
    const auto index = owned->unit_index;
    Divergence divergence;
    bool copy_flew = false;
    for (uint32_t t = 0; t < 240; ++t) {
        if (t == 10) {
            try {
                (void)pair.host->match->issue_ground_move(index, {60 << 16, 0, 200 << 16}, false);
            } catch (const std::exception& error) {
                ++failures;
                std::fprintf(stderr, "aircraft order: %s\n", error.what());
            }
        }
        step(*pair.host);
        divergence.note_owner(*pair.host, index);
        step(*pair.joiner);
        divergence.compare(*pair.joiner, 0, index);
        if (const auto* copy = pair.joiner->match->ground_runtime(index))
            copy_flew =
                copy_flew || ((copy->movement.flags & 3) == 2 && copy->movement.speed != 0 &&
                              pair.joiner->match->state().units[index].position.y > 0);
    }
    const auto* driver = pair.joiner->match->air_driver(index);
    CHECK(driver != nullptr && !driver->local);
    CHECK(copy_flew);
    CHECK(pair.host->match->state().units[index].position.z > (150 << 16));
    CHECK(divergence.compared >= 230 && divergence.moving > 20);
    CHECK(divergence.differing == 0);
}

// An aircraft that reaches a goal with an arrival radius drops it inside
// its movement step, after the step has steered toward it. The copy
// tracks the same goal and drops it at the same step, so its steering
// heading stays its owner's when the aircraft arrives turning, flying
// faster than its brake rate (the excess speed goes along its heading).
// So does a goal installed and reached in one step, which the owner's
// record still carries.
void aircraft_arriving_inside_a_radius_keep_their_copy() {
    Pair pair;
    auto* owned =
        pair.host->match->create({0, kAircraftType, {60u << 16, 0, 40u << 16}, true, 1, 0});
    CHECK(owned != nullptr);
    if (owned == nullptr)
        return;
    const auto index = owned->unit_index;
    const auto fast = [index](Machine& m) {
        auto& world = m.match->state();
        if (auto* type = world_unit_def_of(&world, &world.units[index])) {
            type->max_velocity = 4 << 16;
            type->brake_rate = 2 << 16;
            type->acceleration = 1 << 15;
        }
    };
    fast(*pair.host);
    uint32_t away_events = 0;
    uint32_t aside_events = 0;
    uint32_t swerve_events = 0;
    sim::air::AirGoal away{};
    sim::air::AirGoal aside{};
    sim::air::AirGoal swerve{};
    Divergence divergence;
    uint16_t fastest = 0;
    for (uint32_t t = 0; t < 240; ++t) {
        if (t == 5)
            fast(*pair.joiner);
        auto* driver = pair.host->match->air_driver(index);
        auto& unit = pair.host->match->state().units[index];
        if (t == 10) {
            // Up and away along z, with no order behind the goal.
            pair.host->match->set_movement_layer(index, sim::air::layer_air);
            away = sim::air::air_goal_at_point(&away_events, &unit, {60 << 16, 0, 240 << 16});
            sim::air::air_driver_set_goal(driver, &away);
        }
        const auto* ground = pair.host->match->ground_runtime(index);
        if (t > 10 && aside.kind == sim::air::AirGoalKind::none && ground != nullptr &&
            ground->movement.speed > (3 << 16) && unit.position.z > (120 << 16)) {
            // A point off to the side, reached 60 units short while turning.
            aside = sim::air::air_goal_at_point(&aside_events, &unit, {200 << 16, 0, 200 << 16});
            sim::air::air_goal_set_arrival_radius(&aside, 60);
            sim::air::air_driver_set_goal(driver, &aside);
            fastest = static_cast<uint16_t>(ground->movement.speed >> 16);
        } else if (
            swerve.kind == sim::air::AirGoalKind::none &&
            (aside_events & sim::ground_orders::arrived_event) != 0 && ground != nullptr &&
            ground->movement.speed > (3 << 16)
        ) {
            // A point 40 units to the left of its path, inside a 60 radius:
            // reached at the step that first steers toward it.
            const int32_t left_x = unit.position.x - (ground->movement.velocity[2] * 10);
            const int32_t left_z = unit.position.z + (ground->movement.velocity[0] * 10);
            swerve = sim::air::air_goal_at_point(&swerve_events, &unit, {left_x, 0, left_z});
            sim::air::air_goal_set_arrival_radius(&swerve, 60);
            sim::air::air_driver_set_goal(driver, &swerve);
        }
        step(*pair.host);
        divergence.note_owner(*pair.host, index);
        step(*pair.joiner);
        divergence.compare(*pair.joiner, 0, index);
    }
    CHECK(fastest >= 3);
    CHECK((aside_events & sim::ground_orders::arrived_event) != 0);
    CHECK((swerve_events & sim::ground_orders::arrived_event) != 0);
    CHECK(divergence.compared >= 230 && divergence.moving > 100);
    CHECK(divergence.differing == 0);
}

// A carried copy still runs its driver and steering on every record, as the
// game calls them before the carried branch: an aircraft follows its
// received goal and a ground unit turns toward its route head.
void carried_copies_keep_their_drivers() {
    Pair pair;
    auto* carrier = pair.host->match->create({0, 1, {200u << 16, 0, 200u << 16}, true, 1, 0});
    auto* flier =
        pair.host->match->create({0, kAircraftType, {60u << 16, 0, 60u << 16}, true, 1, 0});
    auto* walker = pair.host->match->create({0, 1, {140u << 16, 0, 60u << 16}, true, 1, 0});
    CHECK(carrier != nullptr && flier != nullptr && walker != nullptr);
    if (carrier == nullptr || flier == nullptr || walker == nullptr)
        return;
    const auto carrier_index = carrier->unit_index;
    const auto flier_index = flier->unit_index;
    const auto walker_index = walker->unit_index;
    for (uint32_t t = 0; t < 4; ++t) {
        step(*pair.host);
        step(*pair.joiner);
    }
    RemoteCopy air(*pair.joiner, flier_index);
    RemoteCopy ground(*pair.joiner, walker_index);
    CHECK(ground.ground != nullptr && ground.ground->mirrored_driver);
    if (ground.ground == nullptr)
        return;
    auto& joiner = *pair.joiner->match;
    joiner.set_carry_link(flier_index, carrier_index, -1, 0);
    joiner.set_carry_link(walker_index, carrier_index, -1, 0);
    CHECK(air.unit->attach_parent != 0 && ground.unit->attach_parent != 0);

    // A seek goal slides its point one world unit along z each tick; the
    // driver keeps that displacement as the point's velocity.
    AirDelta seek{};
    seek.goal_tag = air_goal_tag_seek;
    seek.seek.point[0] = 20 << 16;
    seek.seek.point[2] = 20 << 16;
    seek.seek.step[2] = 1 << 16;
    air.sim.set_air_goal(air.sim.context, &joiner.state(), air.unit, seek);
    const auto* driver = joiner.air_driver(flier_index);
    CHECK(driver != nullptr && driver->goal != nullptr);
    air.record();
    air.record();
    CHECK(driver != nullptr && driver->velocity.x == 0 && driver->velocity.z == (1 << 16));

    // A route head off to the side turns the carried unit.
    ground.route(false, {{140, 60}, {240, 60}});
    ground.record();
    CHECK(ground.ground->movement.turn != 0);

    // The copy's death drops its remote driver with the goal it owns.
    joiner.teardown_dead_unit(joiner.world().slots.at(flier_index));
    CHECK(joiner.air_driver(flier_index) == nullptr);
}

// The host's unit carries the joiner's unit, whose own machine simulates
// it: the 0x0a link reaches the cargo's owner, which alone gives it
// BeCarried; once the carrier stops the cargo sits on it on both machines;
// and the carrier's death kills the cargo as kind 6, which its owner sends,
// on both machines, with no record refused.
void foreign_cargo_dies_with_its_carrier() {
    Pair pair{0, kMapCells, false, true};
    auto& host = *pair.host;
    auto& joiner = *pair.joiner;
    auto* carrier_slot = host.match->create({0, 1, {48u << 16, 0, 48u << 16}, true, 1, 0});
    auto* cargo_slot = joiner.match->create({1, 1, {96u << 16, 0, 64u << 16}, true, 1, 0});
    CHECK(carrier_slot != nullptr && cargo_slot != nullptr);
    if (carrier_slot == nullptr || cargo_slot == nullptr)
        return;
    const auto carrier = carrier_slot->unit_index;
    const auto cargo = cargo_slot->unit_index;
    for (uint32_t t = 0; t < 2 * kUnitsPerPlayer; ++t) {
        step(host);
        step(joiner);
    }
    host.inbox.sent.clear();
    joiner.inbox.sent.clear();
    host.match->script_attach_unit(carrier, cargo, -1, 0);
    for (uint32_t t = 0; t < 3; ++t) {
        step(host);
        step(joiner);
    }
    const auto& owner_cargo = joiner.match->state().units[cargo];
    const auto& host_copy = host.match->state().units[cargo];
    CHECK(sent_of(host.inbox, RecordType::unit_link).size() == 1);
    CHECK(oa_unit_slot_from_ref(owner_cargo.attach_parent) == carrier);
    const auto* owner_head = joiner.match->world().slots.at(cargo).unit->primary;
    const auto* copy_head = host.match->world().slots.at(cargo).unit->primary;
    CHECK(owner_head != nullptr && owner_head->kind == oa::sim::match_runtime::be_carried_kind);
    CHECK(copy_head == nullptr);

    (void)host.match->issue_ground_move(carrier, {48 * 65536, 0, 160 * 65536}, false);
    for (uint32_t t = 0; t < 250; ++t) {
        step(host);
        step(joiner);
    }
    const auto& host_carrier = host.match->state().units[carrier];
    const auto& joiner_carrier = joiner.match->state().units[carrier];
    CHECK(
        oa_unit_slot_from_ref(host_copy.attach_parent) == carrier &&
        oa_unit_slot_from_ref(owner_cargo.attach_parent) == carrier
    );
    CHECK(
        host_copy.position.x == owner_cargo.position.x &&
        host_copy.position.z == owner_cargo.position.z &&
        owner_cargo.position.x == joiner_carrier.position.x &&
        owner_cargo.position.z == joiner_carrier.position.z &&
        host_copy.position.z == host_carrier.position.z
    );

    host.inbox.sent.clear();
    joiner.inbox.sent.clear();
    auto& doomed = host.match->state().units[carrier];
    doomed.health = 0;
    doomed.health_percent = 40;
    doomed.previous_health_percent = 40;
    doomed.damage_kind = kWeaponKind;
    doomed.flags |= OA_UNIT_FLAG_DEATH_PENDING;
    for (uint32_t t = 0; t < 4 * kUnitsPerPlayer; ++t) {
        step(host);
        step(joiner);
    }
    const auto live = [](Machine& m, uint16_t index) {
        return (m.match->state().units[index].flags & OA_UNIT_FLAG_LIVE) != 0;
    };
    CHECK(
        !live(host, carrier) && !live(joiner, carrier) && !live(host, cargo) && !live(joiner, cargo)
    );
    const auto cargo_kills = sent_of(joiner.inbox, RecordType::unit_killed);
    CHECK(cargo_kills.size() == 1);
    if (cargo_kills.size() == 1) {
        UnitKilledRecord killed{};
        CHECK(
            decode_record(cargo_kills[0].bytes.data(), cargo_kills[0].bytes.size(), &killed) ==
            WireError::ok
        );
        CHECK(
            killed.unit_index == cargo &&
            (killed.kind_and_wreck_level >> unit_killed_kind_shift) == kCarriedDeathKind
        );
    }
    for (const auto& kill : sent_of(host.inbox, RecordType::unit_killed)) {
        UnitKilledRecord killed{};
        (void)decode_record(kill.bytes.data(), kill.bytes.size(), &killed);
        CHECK(killed.unit_index != cargo);
    }
    CHECK(host.net->record_errors == 0 && joiner.net->record_errors == 0);
}
} // namespace

// A 0x0c death record naming unit 0 does nothing at all
// (units.ignore-null-death-record, every machine's behaviour with or without
// the rule): no unit dies and the record is not an error.
void death_record_for_unit_zero_changes_nothing() {
    Pair pair;
    auto* owned = pair.host->match->create({0, 1, {40u << 16, 0, 40u << 16}, true, 1, 0});
    CHECK(owned != nullptr);
    for (uint32_t t = 0; t < 4; ++t) {
        step(*pair.host);
        step(*pair.joiner);
    }
    auto& world = pair.joiner->match->state();
    const std::vector<Unit> before(world.units, world.units + world.unit_slot_count);
    UnitKilledRecord killed{};
    killed.unit_index = 0;
    killed.killed_percent = 100;
    killed.kind_and_wreck_level = static_cast<uint8_t>(kWeaponKind << unit_killed_kind_shift) | 1;
    uint8_t bytes[64];
    std::size_t written = 0;
    CHECK(encode_record(killed, bytes, sizeof bytes, &written) == WireError::ok);
    CHECK(net_match_send(pair.host->net.get(), kIds[0], broadcast_destination_id, bytes, written));
    packet_layer_flush(pair.host->connection.packets, 0, true);
    (void)net_match_pump(pair.joiner->net.get());
    CHECK(std::memcmp(before.data(), world.units, before.size() * sizeof(Unit)) == 0);
    CHECK(pair.joiner->net->record_errors == 0 && pair.joiner->match->fault() == nullptr);
}

// Records the owner sends as they are made reach the other machine's copy:
// 0x10 starts a script function by its COB index with its four locals, the
// count of them as arguments, and 0x0e sets off the shot aimed at its point
// with its weapon.
void remote_events_reach_the_copy() {
    Pair pair;
    auto* owned = pair.host->match->create({0, 1, {40u << 16, 0, 40u << 16}, true, 1, 0});
    CHECK(owned != nullptr);
    if (owned == nullptr)
        return;
    const auto index = owned->unit_index;
    for (uint32_t t = 0; t < 4; ++t) {
        step(*pair.host);
        step(*pair.joiner);
    }
    auto* copy = pair.joiner->match->instance(index);
    auto* script = copy != nullptr ? copy->script() : nullptr;
    CHECK(script != nullptr);
    if (script == nullptr)
        return;
    const auto send = [&](const auto& record) {
        uint8_t bytes[64];
        std::size_t written = 0;
        CHECK(encode_record(record, bytes, sizeof bytes, &written) == WireError::ok);
        CHECK(
            net_match_send(pair.host->net.get(), kIds[0], broadcast_destination_id, bytes, written)
        );
        packet_layer_flush(pair.host->connection.packets, 0, true);
        (void)net_match_pump(pair.joiner->net.get());
    };

    CobStartRecord start{};
    start.unit_index = index;
    start.function_index = 0;
    start.argument_count = 2;
    start.args[0] = 7;
    start.args[1] = 8;
    start.args[2] = 9;
    start.args[3] = 10;
    const auto running = script->vm().active_count();
    send(start);
    CHECK(script->vm().active_count() == running + 1);
    bool found = false;
    for (std::size_t i = 0; i < sim::script_vm::context_count && !found; ++i) {
        const auto context = script->vm().context(i);
        found = context.state != sim::script_vm::ContextState::stopped &&
                context.stack_pointer == 1 && context.slots[0] == 7 && context.slots[1] == 8 &&
                context.slots[2] == 9 && context.slots[3] == 10;
    }
    CHECK(found);

    auto& world = pair.joiner->match->state();
    constexpr uint8_t kWeapon = 3;
    world.game.weapon_defs[kWeapon].weapon_id = kWeapon;
    world.game.weapon_defs[kWeapon + 1].weapon_id = kWeapon + 1;
    world.game.projectile_count = 2;
    for (int32_t i = 0; i < 2; ++i) {
        auto& shot = world.projectiles[i];
        shot = Projectile{};
        shot.def = oa_ref_from_index(kWeapon + static_cast<uint32_t>(i));
        shot.target = {100 << 16, 5 << 16, 120 << 16};
    }
    ProjectileInterceptedRecord intercepted{};
    intercepted.target[0] = 100 << 16;
    intercepted.target[1] = 5 << 16;
    intercepted.target[2] = 120 << 16;
    intercepted.weapon_id = kWeapon + 1;
    send(intercepted);
    CHECK((world.projectiles[0].flags & OA_PROJECTILE_FLAG_RETIRED) == 0);
    CHECK((world.projectiles[1].flags & OA_PROJECTILE_FLAG_RETIRED) != 0);
    CHECK(pair.joiner->net->record_errors == 0);
}

// The owner finds the script by name and sends it as 0x10 from the unit's
// owner, so the copy starts the same script with the same locals.
// A machine that does not own the unit sends nothing.
void owner_script_starts_reach_the_copy() {
    Pair pair;
    auto* owned = pair.host->match->create({0, 1, {40u << 16, 0, 40u << 16}, true, 1, 0});
    CHECK(owned != nullptr);
    if (owned == nullptr)
        return;
    const auto index = owned->unit_index;
    for (uint32_t t = 0; t < 4; ++t) {
        step(*pair.host);
        step(*pair.joiner);
    }
    auto* copy = pair.joiner->match->instance(index);
    auto* copy_script = copy != nullptr ? copy->script() : nullptr;
    auto* original = pair.host->match->instance(index);
    auto* owner_script = original != nullptr ? original->script() : nullptr;
    CHECK(copy_script != nullptr && owner_script != nullptr);
    if (copy_script == nullptr || owner_script == nullptr)
        return;

    const auto running = copy_script->vm().active_count();
    pair.host->match->share_named_script_start(index, "Create", 2, {7, 8, 0, 0});
    packet_layer_flush(pair.host->connection.packets, 0, true);
    (void)net_match_pump(pair.joiner->net.get());
    CHECK(copy_script->vm().active_count() == running + 1);
    bool found = false;
    for (std::size_t i = 0; i < sim::script_vm::context_count && !found; ++i) {
        const auto context = copy_script->vm().context(i);
        found = context.state != sim::script_vm::ContextState::stopped &&
                context.stack_pointer == 1 && context.slots[0] == 7 && context.slots[1] == 8;
    }
    CHECK(found);

    // No such script: index -1 goes out and the copy starts nothing.
    pair.host->match->share_named_script_start(index, "StopBuilding", 0, {});
    packet_layer_flush(pair.host->connection.packets, 0, true);
    (void)net_match_pump(pair.joiner->net.get());
    CHECK(copy_script->vm().active_count() == running + 1);

    // A builder's StartBuilding goes out as its build orders share it: one
    // argument, the heading zero-extended from 16 bits.
    pair.host->match->share_named_script_start(index, "StartBuilding", 1, {0xc000, 0, 0, 0});
    packet_layer_flush(pair.host->connection.packets, 0, true);
    const auto starts = sent_of(pair.host->inbox, RecordType::cob_start);
    CHECK(
        !starts.empty() && starts.back().bytes[3] == 1 && starts.back().bytes[4] == 0 &&
        starts.back().bytes[5] == 1 && starts.back().bytes[6] == 0x00 &&
        starts.back().bytes[7] == 0xc0 && starts.back().bytes[8] == 0 && starts.back().bytes[9] == 0
    );
    (void)net_match_pump(pair.joiner->net.get());
    CHECK(copy_script->vm().active_count() == running + 2);
    bool heading_found = false;
    for (std::size_t i = 0; i < sim::script_vm::context_count && !heading_found; ++i) {
        const auto context = copy_script->vm().context(i);
        heading_found =
            context.state != sim::script_vm::ContextState::stopped && context.slots[0] == 0xc000;
    }
    CHECK(heading_found);

    const auto owner_running = owner_script->vm().active_count();
    pair.joiner->match->share_script_start(index, 0);
    packet_layer_flush(pair.joiner->connection.packets, 0, true);
    (void)net_match_pump(pair.host->net.get());
    CHECK(owner_script->vm().active_count() == owner_running);
    CHECK(pair.joiner->net->record_errors == 0 && pair.host->net->record_errors == 0);
}

// A shot fired on the owner's machine arrives as 0x0d: the copy's slot
// takes the carried angles and the constructor the carried weapon picks
// (here the line one) launches the slot's weapon from the carried start at
// the carried target for the unit's player. A shot from no live unit, or
// from a fourth slot, launches nothing. A meteor the host announces falls
// on the other machine from the same point at the same velocity, owned by
// no player.
void remote_shots_reach_the_copy() {
    Pair pair(0, kMapCells, true);
    auto* owned = pair.host->match->create({0, 1, {40u << 16, 0, 40u << 16}, true, 1, 0});
    CHECK(owned != nullptr);
    if (owned == nullptr)
        return;
    const auto index = owned->unit_index;
    for (uint32_t t = 0; t < 4; ++t) {
        step(*pair.host);
        step(*pair.joiner);
    }
    auto& world = pair.joiner->match->state();
    CHECK(
        world.units[index].type_index == 1 &&
        world.units[index].weapons[0].def == oa_ref_from_index(kGun)
    );
    const auto deliver = [&] {
        packet_layer_flush(pair.host->connection.packets, 0, true);
        (void)net_match_pump(pair.joiner->net.get());
    };
    const auto send = [&](const WeaponFireRecord& record) {
        uint8_t bytes[64];
        std::size_t written = 0;
        CHECK(
            encode_record(record, bytes, sizeof bytes, &written) == WireError::ok && written == 36
        );
        CHECK(
            net_match_send(pair.host->net.get(), kIds[0], broadcast_destination_id, bytes, written)
        );
        deliver();
    };

    WeaponFireRecord fire{};
    fire.start[0] = 41 << 16;
    fire.start[1] = 12 << 16;
    fire.start[2] = 40 << 16;
    fire.target[0] = 140 << 16;
    fire.target[1] = 0;
    fire.target[2] = 90 << 16;
    fire.weapon_id = kGun;
    fire.aim_heading = 0x1234;
    fire.aim_pitch = -0x100;
    fire.source_unit_index = index;
    fire.slot = 0;
    const auto before = world.game.projectile_count;
    send(fire);
    CHECK(world.game.projectile_count == before + 1);
    const auto& shot = world.projectiles[before];
    const sim::weapon_execution::FixedVector start{fire.start[0], fire.start[1], fire.start[2]};
    const sim::weapon_execution::FixedVector target{fire.target[0], fire.target[1], fire.target[2]};
    const auto expected = sim::weapon_execution::launch_line_projectile(
        world.game.weapon_defs[kGun], start, target, world.game.tick
    );
    CHECK(
        shot.def == oa_ref_from_index(kGun) && shot.source == oa_unit_ref_from_slot(index) &&
        shot.owner_index == 0
    );
    CHECK(
        shot.position.x == start[0] && shot.position.y == start[1] && shot.position.z == start[2]
    );
    CHECK(shot.target.x == target[0] && shot.target.y == target[1] && shot.target.z == target[2]);
    CHECK(
        shot.heading == expected.heading && shot.pitch == expected.pitch &&
        shot.speed == expected.speed
    );
    CHECK(
        shot.velocity.x == expected.velocity[0] && shot.velocity.y == expected.velocity[1] &&
        shot.velocity.z == expected.velocity[2] && shot.lifetime_tick == expected.lifetime_tick
    );
    CHECK(
        world.units[index].weapons[0].aim_heading == 0x1234 &&
        world.units[index].weapons[0].aim_pitch == -0x100
    );

    fire.slot = 3;
    send(fire);
    fire.slot = 0;
    fire.source_unit_index = 0;
    send(fire);
    CHECK(world.game.projectile_count == before + 1);

    const auto host_before = pair.host->match->state().game.projectile_count;
    const FixedVec3 fall_from{10 << 16, 500 << 16, 20 << 16};
    const FixedVec3 fall{0, -(15 << 16), 0};
    CHECK(pair.host->match->launch_meteor(oa_ref_from_index(kRock), fall_from, fall, true));
    deliver();
    CHECK(pair.host->match->state().game.projectile_count == host_before + 1);
    CHECK(world.game.projectile_count == before + 2);
    const auto& rock = world.projectiles[before + 1];
    CHECK(rock.def == oa_ref_from_index(kRock) && rock.source == 0 && rock.owner_index == 10);
    CHECK(
        rock.position.x == fall_from.x && rock.position.y == fall_from.y &&
        rock.position.z == fall_from.z
    );
    CHECK(rock.velocity.x == fall.x && rock.velocity.y == fall.y && rock.velocity.z == fall.z);
    CHECK(pair.joiner->net->record_errors == 0);
}

// A killable pair after two full-record cycles: the host's unit, and the
// joiner's unit it will be credited with killing, each with its copy. Both
// stand inside the view's edges, where wrecks can lie.
struct KillScene {
    Pair pair{0, kMapCells, false, true};
    uint16_t attacker{};
    uint16_t victim{};

    KillScene() {
        auto* attacker_slot =
            pair.host->match->create({0, 1, {48u << 16, 0, 48u << 16}, true, 1, 0});
        auto* victim_slot =
            pair.joiner->match->create({1, 1, {96u << 16, 0, 64u << 16}, true, 1, 0});
        CHECK(attacker_slot != nullptr && victim_slot != nullptr);
        if (attacker_slot == nullptr || victim_slot == nullptr)
            return;
        attacker = attacker_slot->unit_index;
        victim = victim_slot->unit_index;
        for (uint32_t t = 0; t < 2 * kUnitsPerPlayer; ++t)
            both();
        CHECK(live(*pair.host, victim) && live(*pair.joiner, attacker));
        forget_sent();
    }

    void both() {
        step(*pair.host);
        step(*pair.joiner);
    }

    void forget_sent() {
        pair.host->inbox.sent.clear();
        pair.joiner->inbox.sent.clear();
        pair.host->services.explosions.clear();
        pair.joiner->services.explosions.clear();
    }

    static bool live(Machine& m, uint16_t index) {
        return (m.match->state().units[index].flags & OA_UNIT_FLAG_LIVE) != 0;
    }

    // The joiner's unit is left dying by the host's unit at a health and
    // previous 30-tick health percentage (maximum health 100).
    void doom(int16_t health, uint8_t previous_percent) {
        auto& unit = pair.joiner->match->state().units[victim];
        unit.health = health;
        unit.health_percent = previous_percent;
        unit.previous_health_percent = previous_percent;
        unit.damage_kind = kWeaponKind;
        unit.last_attacker_id = attacker;
        unit.last_attacker_owner = 0;
        unit.flags |= OA_UNIT_FLAG_DEATH_PENDING;
    }

    // Steps the joiner, then the host, until the victim is gone on both;
    // returns how many ticks the host's copy outlives the joiner's kill: 0
    // when the host's next tick, which here shares the joiner's tick number,
    // takes it away.
    uint32_t run_until_gone() {
        uint32_t joiner_tick = 0;
        for (uint32_t t = 0; t < 4 * kUnitsPerPlayer; ++t) {
            step(*pair.joiner);
            if (joiner_tick == 0 && !live(*pair.joiner, victim))
                joiner_tick = pair.joiner->match->state().game.tick;
            step(*pair.host);
            if (joiner_tick != 0 && !live(*pair.host, victim))
                return pair.host->match->state().game.tick - joiner_tick;
        }
        return ~0u;
    }

    // The EXPLODEs the victim's scripts ran on a machine, in order.
    std::vector<PieceExplosion> explosions(Machine& m) const {
        std::vector<PieceExplosion> out;
        for (const auto& explosion : m.services.explosions)
            if (explosion.unit == victim)
                out.push_back(explosion);
        return out;
    }
};

// The feature word on the plot where a wreck lies.
uint16_t feature_under(Machine& m, const sim::match_runtime::Match::Wreck& wreck) {
    const auto* plot = world_plot(&m.match->state(), wreck.cell_x, wreck.cell_z);
    return plot != nullptr ? plot->feature : sim::feature_runtime::no_feature;
}

// The owner's kill at a low percentage reaches the other machine as a 0x0c
// record on the next tick, from the owner's player id to every player, and
// the copy leaves the same corpse at the same cell, playing the same Killed
// pieces, although the copy itself was never damaged there. A kill at a high
// percentage steps past the heap and leaves nothing on either machine. The
// machine that does not simulate the unit sends no 0x0c.
void owner_kill_is_shared_with_its_wreck() {
    {
        KillScene scene;
        auto& host = *scene.pair.host;
        auto& joiner = *scene.pair.joiner;
        // (0 * 100 / 100 + 40) / 2 = 20: corpsetype 1, the corpse itself.
        scene.doom(0, 40);
        CHECK(scene.run_until_gone() == 0);
        const auto kills = sent_of(joiner.inbox, RecordType::unit_killed);
        CHECK(kills.size() == 1);
        if (kills.size() == 1) {
            const auto& kill = kills[0];
            CHECK(kill.from == kIds[1] && kill.to == broadcast_destination_id);
            const std::vector<uint8_t> expected{
                0x0c,
                static_cast<uint8_t>(scene.victim),
                static_cast<uint8_t>(scene.victim >> 8),
                0x01,
                0x20,
                0x00,
                0x00,
                static_cast<uint8_t>(scene.attacker),
                static_cast<uint8_t>(scene.attacker >> 8),
                20,
                0x11
            };
            CHECK(kill.bytes == expected);
        }
        CHECK(sent_of(host.inbox, RecordType::unit_killed).empty());
        CHECK(joiner.match->wrecks().size() == 1 && host.match->wrecks().size() == 1);
        if (joiner.match->wrecks().size() == 1 && host.match->wrecks().size() == 1) {
            const auto& owner_wreck = joiner.match->wrecks()[0];
            const auto& copy_wreck = host.match->wrecks()[0];
            CHECK(owner_wreck.feature == kDeadFeature && copy_wreck.feature == kDeadFeature);
            CHECK(
                owner_wreck.cell_x == copy_wreck.cell_x && owner_wreck.cell_z == copy_wreck.cell_z
            );
            CHECK(
                feature_under(host, copy_wreck) == kDeadFeature &&
                feature_under(joiner, owner_wreck) == kDeadFeature
            );
        }
        const auto owner_pieces = scene.explosions(joiner);
        CHECK(owner_pieces.size() == 1 && owner_pieces == scene.explosions(host));
        CHECK(!owner_pieces.empty() && owner_pieces[0].flags == 1);
    }
    {
        KillScene scene;
        auto& host = *scene.pair.host;
        auto& joiner = *scene.pair.joiner;
        // (1000 * 100 / 100 + 100) / 2, clamped to 100: corpsetype 3, past the heap.
        scene.doom(-1000, 100);
        CHECK(scene.run_until_gone() == 0);
        const auto kills = sent_of(joiner.inbox, RecordType::unit_killed);
        CHECK(
            kills.size() == 1 && kills[0].bytes.size() == unit_killed_record_bytes &&
            kills[0].bytes[9] == 100 && kills[0].bytes[10] == 0x13
        );
        CHECK(sent_of(host.inbox, RecordType::unit_killed).empty());
        CHECK(joiner.match->wrecks().empty() && host.match->wrecks().empty());
        const auto owner_pieces = scene.explosions(joiner);
        CHECK(owner_pieces.size() == 1 && owner_pieces == scene.explosions(host));
        // A further full-record cycle leaves both as they are.
        for (uint32_t t = 0; t < 2 * kUnitsPerPlayer; ++t)
            scene.both();
        CHECK(!KillScene::live(host, scene.victim) && host.match->wrecks().empty());
    }
}

// The kill is credited on both machines as the record names it: the
// attacker's player a kill, the victim's player a loss, and the attacker a
// veteran level.
void received_kill_credits_the_record() {
    KillScene scene;
    // (0 + 60) / 2 = 30: corpsetype 2, the heap.
    scene.doom(0, 60);
    CHECK(scene.run_until_gone() == 0);
    for (auto* m : {scene.pair.host.get(), scene.pair.joiner.get()}) {
        const auto& world = m->match->state();
        CHECK(world.game.players[0].kills == 1 && world.game.players[0].losses == 0);
        CHECK(world.game.players[1].kills == 0 && world.game.players[1].losses == 1);
        CHECK(world.units[scene.attacker].veteran_level == 1);
        CHECK(m->match->wrecks().size() == 1 && m->match->wrecks()[0].feature == kHeapFeature);
    }
}

// A 0x09 record into a slot whose copy is still there kills that copy first
// as the kill handler does, by its own health: at 0 health and a previous
// 100% its Killed picks the heap at (0 + 100) / 2 = 50. Then the new unit
// takes the slot. The copy is simulated elsewhere, so no 0x0c goes out.
void create_into_live_slot_runs_the_kill() {
    KillScene scene;
    auto& host = *scene.pair.host;
    auto& world = host.match->state();
    auto& copy = world.units[scene.victim];
    copy.health = 0;
    copy.health_percent = 100;
    copy.previous_health_percent = 100;
    const auto created = host.binding.created_remote;
    UnitCreatedRecord record{};
    record.unit_def_index = 1;
    record.unit_index = scene.victim;
    record.position[0] = 96 << 16;
    record.position[2] = 96 << 16;
    auto sim = match_binding_sim(&host.binding);
    sim.create_unit(sim.context, &world, 1, record);
    CHECK(host.match->wrecks().size() == 1);
    if (host.match->wrecks().size() == 1)
        CHECK(
            host.match->wrecks()[0].feature == kHeapFeature &&
            feature_under(host, host.match->wrecks()[0]) == kHeapFeature
        );
    const auto pieces = scene.explosions(host);
    CHECK(pieces.size() == 1 && pieces[0].flags == 2);
    CHECK(host.binding.created_remote == created + 1);
    CHECK(KillScene::live(host, scene.victim) && copy.type_index == 1);
    CHECK((copy.position.x >> 16) == 96 && (copy.position.z >> 16) == 96);
    packet_layer_flush(host.connection.packets, 0, true);
    CHECK(sent_of(host.inbox, RecordType::unit_killed).empty());
}

// Under the recorder's rules a 0x09 that repeats the copy already in its
// slot (same owner, type and ground position) is dropped: the copy stays
// and nothing is created. Without them the same record replaces the copy,
// as above.
void repeated_create_under_recorder_rules_is_dropped() {
    for (const bool recorder : {true, false}) {
        KillScene scene;
        auto& host = *scene.pair.host;
        auto& world = host.match->state();
        auto& copy = world.units[scene.victim];
        host.binding.net->rules.recorder_protocol =
            recorder ? recorder_protocol_current : recorder_protocol_plain;
        const auto created = host.binding.created_remote;
        UnitCreatedRecord record{};
        record.unit_def_index = copy.type_index;
        record.unit_index = scene.victim;
        record.position[0] = copy.position.x;
        record.position[1] = copy.position.y;
        record.position[2] = copy.position.z;
        auto sim = match_binding_sim(&host.binding);
        sim.create_unit(sim.context, &world, copy.owner_index, record);
        CHECK(KillScene::live(host, scene.victim) && copy.type_index == record.unit_def_index);
        CHECK(host.binding.created_remote == created + (recorder ? 0u : 1u));
    }
}

// A 0x09 naming a definition past this machine's unit table creates nothing
// and is counted apart from refused creates.
void create_past_the_table_is_counted_apart() {
    KillScene scene;
    auto& host = *scene.pair.host;
    auto& world = host.match->state();
    const auto slot = static_cast<uint16_t>(scene.victim + 1);
    const auto created = host.binding.created_remote;
    const auto refused = host.binding.refused_creates;
    UnitCreatedRecord record{};
    record.unit_def_index = static_cast<uint16_t>(world.unit_def_count);
    record.unit_index = slot;
    record.position[0] = 96 << 16;
    record.position[2] = 96 << 16;
    auto sim = match_binding_sim(&host.binding);
    sim.create_unit(sim.context, &world, 1, record);
    CHECK(host.binding.creates_past_table == 1);
    CHECK(host.binding.created_remote == created && host.binding.refused_creates == refused);
    CHECK(!KillScene::live(host, slot));
}

// A machine whose player lacks the host role hands a weapon hit on a feature
// to the host (0x0f with the weapon id, addressed to the host's id) and
// leaves the feature as it is; the host applies it, destroys the corpse and
// broadcasts its die sequence (0xfd), which both machines then play.
void feature_hits_go_to_the_host() {
    Pair pair(0, kMapCells, false, true);
    auto& host = *pair.host;
    auto& joiner = *pair.joiner;
    constexpr uint8_t kHitWeapon = 7;
    constexpr int32_t x = 5;
    constexpr int32_t z = 6;
    for (auto* m : {&host, &joiner}) {
        auto& world = m->match->state();
        world.game.console_flags |= OA_CONSOLE_FLAG_TREE_DEATH;
        world.player_info[0].role |= lobby_role_feature_authority;
        world.game.weapon_defs[kHitWeapon].weapon_id = kHitWeapon;
        world.game.weapon_defs[kHitWeapon].damage_default = 2 * kCorpseHitPoints;
        const auto plot = static_cast<std::size_t>(z) * world.game.map_width + x;
        CHECK(
            sim::feature_runtime::place_feature(
                world,
                m->match->feature_host(),
                plot,
                kDeadFeature,
                nullptr,
                nullptr,
                sim::feature_runtime::no_player
            ) != nullptr ||
            world.plots[plot].feature == kDeadFeature
        );
        CHECK(world.plots[plot].feature == kDeadFeature);
    }
    for (uint32_t t = 0; t < 2; ++t) {
        step(host);
        step(joiner);
    }
    host.inbox.sent.clear();
    joiner.inbox.sent.clear();
    auto& joiner_world = joiner.match->state();
    auto& host_world = host.match->state();
    const auto plot = static_cast<std::size_t>(z) * joiner_world.game.map_width + x;
    sim::feature_runtime::damage_feature(
        joiner_world,
        joiner.match->feature_host(),
        plot,
        x,
        z,
        joiner_world.game.weapon_defs[kHitWeapon]
    );
    CHECK(joiner_world.plots[plot].feature == kDeadFeature);
    packet_layer_flush(joiner.connection.packets, 0, true);
    const auto hits = sent_of(joiner.inbox, RecordType::feature_event);
    CHECK(hits.size() == 1);
    if (hits.size() == 1) {
        CHECK(hits[0].from == kIds[1] && hits[0].to == kIds[0]);
        CHECK((hits[0].bytes == std::vector<uint8_t>{0x0f, kHitWeapon, x, 0, z, 0}));
    }
    (void)net_match_pump(host.net.get());
    CHECK(host_world.plots[plot].feature == kHeapFeature);
    packet_layer_flush(host.connection.packets, 0, true);
    const auto sequences = sent_of(host.inbox, RecordType::feature_event);
    CHECK(sequences.size() == 1);
    if (sequences.size() == 1) {
        CHECK(sequences[0].from == kIds[0]);
        CHECK((
            sequences[0].bytes == std::vector<uint8_t>{0x0f, feature_action_queue_event, x, 0, z, 0}
        ));
    }
    (void)net_match_pump(joiner.net.get());
    CHECK(joiner_world.plots[plot].feature == kHeapFeature);
    // Neither machine sends anything back for a sequence it was told of.
    packet_layer_flush(joiner.connection.packets, 0, true);
    CHECK(sent_of(joiner.inbox, RecordType::feature_event).size() == 1);
    CHECK(host.net->record_errors == 0 && joiner.net->record_errors == 0);
}

// A finished reclaim goes out to every player as the reclaim sequence (0xff)
// from the reclaiming unit's owner, with or without the host role, and the
// other machine plays it. When a machine also runs a computer player (here
// the host, which takes the joiner's player over for the purpose), that
// player's reclaim goes out from its own id, not from the first player there.
void reclaims_go_out_from_the_reclaiming_unit_s_owner() {
    KillScene scene;
    auto& host = *scene.pair.host;
    auto& joiner = *scene.pair.joiner;
    constexpr int32_t x = 5;
    constexpr int32_t z = 6;
    constexpr int32_t other_x = 9;
    for (auto* m : {&host, &joiner}) {
        auto& world = m->match->state();
        world.player_info[0].role |= lobby_role_feature_authority;
        for (const auto cell_x : {x, other_x}) {
            const auto plot = static_cast<std::size_t>(z) * world.game.map_width + cell_x;
            (void)sim::feature_runtime::place_feature(
                world,
                m->match->feature_host(),
                plot,
                kDeadFeature,
                nullptr,
                nullptr,
                sim::feature_runtime::no_player
            );
            CHECK(world.plots[plot].feature == kDeadFeature);
        }
    }
    auto& joiner_world = joiner.match->state();
    auto& host_world = host.match->state();
    const auto plot = static_cast<std::size_t>(z) * joiner_world.game.map_width + x;
    const auto point = [](int32_t cell_x, int32_t cell_z) {
        return FixedVec3{(cell_x * 16 + 8) << 16, 0, (cell_z * 16 + 8) << 16};
    };
    CHECK(joiner.match->reclaim_feature(joiner_world.units[scene.victim], point(x, z)));
    CHECK(joiner_world.plots[plot].feature != kDeadFeature);
    packet_layer_flush(joiner.connection.packets, 0, true);
    const auto reclaims = sent_of(joiner.inbox, RecordType::feature_event);
    CHECK(reclaims.size() == 1);
    if (reclaims.size() == 1) {
        CHECK(reclaims[0].from == kIds[1] && reclaims[0].to == broadcast_destination_id);
        CHECK(
            (reclaims[0].bytes ==
             std::vector<uint8_t>{0x0f, feature_action_queue_event_flagged, x, 0, z, 0})
        );
    }
    (void)net_match_pump(host.net.get());
    CHECK(host_world.plots[plot].feature == joiner_world.plots[plot].feature);
    packet_layer_flush(host.connection.packets, 0, true);
    CHECK(sent_of(host.inbox, RecordType::feature_event).empty());
    CHECK(host.net->record_errors == 0 && joiner.net->record_errors == 0);

    auto& computer = host_world.game.players[1];
    computer.status = OA_PLAYER_STATUS_COMPUTER;
    CHECK(host.match->reclaim_feature(host_world.units[scene.victim], point(other_x, z)));
    computer.status = OA_PLAYER_STATUS_MIRRORED;
    packet_layer_flush(host.connection.packets, 0, true);
    const auto computer_reclaims = sent_of(host.inbox, RecordType::feature_event);
    CHECK(computer_reclaims.size() == 1);
    if (computer_reclaims.size() == 1) {
        CHECK(
            computer_reclaims[0].from == kIds[1] &&
            computer_reclaims[0].to == broadcast_destination_id
        );
        CHECK(
            computer_reclaims[0].bytes[1] == feature_action_queue_event_flagged &&
            computer_reclaims[0].bytes[2] == other_x
        );
    }
}

// An anti-missile weapon whose 96-wide blast sets other shots off, and the
// missile it stops, installed on a machine.
constexpr uint8_t kInterceptor = 3;
constexpr uint8_t kMissile = 4;

void install_interceptors(Machine& m) {
    (void)sim::combat_state::install_weapon_text(
        m.weapons,
        std::string(
            "[TESTAMD]{id=" + std::to_string(kInterceptor) +
            "; reloadtime=1; range=1000; lineofsight=1; weaponvelocity=100; areaofeffect=96; "
            "interceptor=1; [DAMAGE]{default=10;}}"
        )
    );
    (void)sim::combat_state::install_weapon_text(
        m.weapons,
        std::string(
            "[TESTMISSILE]{id=" + std::to_string(kMissile) +
            "; reloadtime=1; range=1000; lineofsight=1; weaponvelocity=100; areaofeffect=32; "
            "[DAMAGE]{default=10;}}"
        )
    );
    std::copy(
        m.weapons.records().begin(),
        m.weapons.records().end(),
        std::begin(m.match->state().game.weapon_defs)
    );
}

// A live shot of a weapon at a point, aimed at that point, fired by a unit of
// a player, as a weapon constructor leaves it.
Projectile&
launch_shot(Machine& m, uint8_t weapon, int32_t x, int32_t z, uint8_t owner, uint16_t source) {
    auto& world = m.match->state();
    auto* shot = sim::weapon_execution::allocate_projectile(world);
    CHECK(shot != nullptr);
    const FixedVec3 at{x << 16, 60 << 16, z << 16};
    sim::weapon_execution::init_projectile_record(
        world,
        *shot,
        oa_ref_from_index(weapon),
        at,
        &at,
        world.game.tick,
        world_unit_at(&world, source),
        0
    );
    shot->owner_index = owner;
    shot->lifetime_tick = world.game.tick + 200;
    return *shot;
}

// Pumps a machine at its next tick, without simulating it, so the next
// record of a frame received this tick is handed out.
void pump_next_tick(Machine& m) {
    ++m.match->state().game.tick;
    (void)net_match_pump(m.net.get());
}

bool retired(const Projectile& shot) {
    return (shot.flags & OA_PROJECTILE_FLAG_RETIRED) != 0;
}

// The host's anti-missile shot sets off the joiner's missile on the host's
// machine: the host sends two 0x0e records, the missile's target point and
// weapon id and then the interceptor's, both to every player from the
// interceptor's owner. On the joiner's machine, which simulates the missile,
// only those records can stop it: its missile and its copy of the
// interceptor go off, and the joiner shares nothing back.
void interceptions_reach_the_shot_s_owner() {
    Pair pair(0, kMapCells, true);
    auto& host = *pair.host;
    auto& joiner = *pair.joiner;
    install_interceptors(host);
    install_interceptors(joiner);
    auto* defender = host.match->create({0, 1, {40u << 16, 0, 200u << 16}, true, 1, 0});
    auto* launcher = joiner.match->create({1, 1, {200u << 16, 0, 40u << 16}, true, 1, 0});
    CHECK(defender != nullptr && launcher != nullptr);
    if (defender == nullptr || launcher == nullptr)
        return;
    const auto defender_id = defender->unit_index;
    const auto launcher_id = launcher->unit_index;
    for (uint32_t t = 0; t < 4; ++t) {
        step(host);
        step(joiner);
    }
    host.inbox.sent.clear();
    joiner.inbox.sent.clear();
    auto& interceptor = launch_shot(host, kInterceptor, 128, 128, 0, defender_id);
    auto& missile_copy = launch_shot(host, kMissile, 150, 128, 1, launcher_id);
    auto& missile = launch_shot(joiner, kMissile, 150, 128, 1, launcher_id);
    auto& interceptor_copy = launch_shot(joiner, kInterceptor, 128, 128, 0, defender_id);
    try {
        host.match->detonate(interceptor, nullptr);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "detonate: %s\n", error.what());
    }
    CHECK(retired(missile_copy));
    packet_layer_flush(host.connection.packets, 0, true);
    const auto sent = sent_of(host.inbox, RecordType::projectile_intercepted);
    CHECK(sent.size() == 2);
    if (sent.size() == 2) {
        const auto expected = [](int32_t x, int32_t z, uint8_t weapon) {
            std::vector<uint8_t> bytes{0x0e};
            for (const int32_t value : {x << 16, 60 << 16, z << 16})
                for (int shift = 0; shift < 32; shift += 8)
                    bytes.push_back(static_cast<uint8_t>(static_cast<uint32_t>(value) >> shift));
            bytes.push_back(weapon);
            return bytes;
        };
        CHECK(
            sent[0].bytes == expected(150, 128, kMissile) &&
            sent[1].bytes == expected(128, 128, kInterceptor)
        );
        for (const auto& record : sent)
            CHECK(record.from == kIds[0] && record.to == broadcast_destination_id);
    }
    CHECK(!retired(missile) && !retired(interceptor_copy));
    // The frame hands out one record a tick: the missile goes off as the
    // first arrives, the interceptor's copy a tick later.
    (void)net_match_pump(joiner.net.get());
    CHECK(retired(missile) && !retired(interceptor_copy));
    pump_next_tick(joiner);
    CHECK(retired(interceptor_copy));
    packet_layer_flush(joiner.connection.packets, 0, true);
    CHECK(sent_of(joiner.inbox, RecordType::projectile_intercepted).empty());
    CHECK(joiner.net->record_errors == 0);
}

// A carry link the host's match accepts goes to every player as a 0x0a
// record {unit, carrier, piece, mode} from the first player on the host's
// machine; the joiner's copy is carried on the next pump, before any full
// record, and the joiner sends nothing back. A drop sets the copy down.
void carry_links_reach_the_copy() {
    Pair pair;
    auto& host = *pair.host;
    auto& joiner = *pair.joiner;
    auto* transport = host.match->create({0, 1, {60u << 16, 0, 60u << 16}, true, 1, 0});
    auto* cargo = host.match->create({0, 1, {80u << 16, 0, 60u << 16}, true, 1, 0});
    CHECK(transport != nullptr && cargo != nullptr);
    if (transport == nullptr || cargo == nullptr)
        return;
    const auto transport_id = transport->unit_index;
    const auto cargo_id = cargo->unit_index;
    for (uint32_t t = 0; t < 4; ++t) {
        step(host);
        step(joiner);
    }
    host.inbox.sent.clear();
    joiner.inbox.sent.clear();
    auto& copy = joiner.match->state().units[cargo_id];
    CHECK(oa_unit_slot_from_ref(copy.attach_parent) == 0);
    host.match->script_attach_unit(transport_id, cargo_id, 0, 2);
    packet_layer_flush(host.connection.packets, 0, true);
    auto links = sent_of(host.inbox, RecordType::unit_link);
    CHECK(links.size() == 1);
    if (links.size() == 1) {
        CHECK(links[0].from == kIds[0] && links[0].to == broadcast_destination_id);
        CHECK((
            links[0].bytes ==
            std::vector<uint8_t>{
                0x0a, static_cast<uint8_t>(cargo_id), 0, static_cast<uint8_t>(transport_id), 0, 0, 2
            }
        ));
    }
    (void)net_match_pump(joiner.net.get());
    CHECK(oa_unit_slot_from_ref(copy.attach_parent) == transport_id);
    packet_layer_flush(joiner.connection.packets, 0, true);
    CHECK(sent_of(joiner.inbox, RecordType::unit_link).empty());

    host.inbox.sent.clear();
    host.match->script_drop_unit(transport_id, cargo_id);
    packet_layer_flush(host.connection.packets, 0, true);
    links = sent_of(host.inbox, RecordType::unit_link);
    CHECK(
        links.size() == 1 && links[0].bytes[3] == 0 && links[0].bytes[4] == 0 &&
        links[0].bytes[5] == 0xff
    );
    (void)net_match_pump(joiner.net.get());
    CHECK(oa_unit_slot_from_ref(copy.attach_parent) == 0);
    packet_layer_flush(joiner.connection.packets, 0, true);
    CHECK(sent_of(joiner.inbox, RecordType::unit_link).empty());
    CHECK(joiner.net->record_errors == 0);
}

// A unit the host finishes goes out as a 0x12 record {unit, builder} from the
// builder's owner, and the joiner's copy, created unfinished, is finished on
// the next pump, before the full record comes round; a copy's machine sends
// no 0x12. A building created finished names itself as its builder, right
// after its 0x09.
void finished_units_reach_the_copy() {
    constexpr uint16_t kStructure = 3;
    Pair pair(kStructure);
    auto& host = *pair.host;
    auto& joiner = *pair.joiner;
    auto* builder = host.match->create({0, 1, {60u << 16, 0, 60u << 16}, true, 1, 0});
    auto* frame = host.match->create({0, 1, {100u << 16, 0, 60u << 16}, false, 1, 0});
    CHECK(builder != nullptr && frame != nullptr);
    if (builder == nullptr || frame == nullptr)
        return;
    const auto builder_id = builder->unit_index;
    const auto frame_id = frame->unit_index;
    for (uint32_t t = 0; t < 2; ++t) {
        step(host);
        step(joiner);
    }
    auto& copy = joiner.match->state().units[frame_id];
    CHECK(copy.type_index == 1 && copy.build_remaining != 0.0F);
    host.inbox.sent.clear();
    joiner.inbox.sent.clear();
    host.match->finish_unit(frame_id, builder_id);
    CHECK(host.match->state().units[frame_id].build_remaining == 0.0F);
    packet_layer_flush(host.connection.packets, 0, true);
    auto links = sent_of(host.inbox, RecordType::builder_link);
    CHECK(links.size() == 1);
    if (links.size() == 1) {
        CHECK(links[0].from == kIds[0] && links[0].to == broadcast_destination_id);
        CHECK(
            (links[0].bytes ==
             std::vector<uint8_t>{
                 0x12, static_cast<uint8_t>(frame_id), 0, static_cast<uint8_t>(builder_id), 0
             })
        );
    }
    (void)net_match_pump(joiner.net.get());
    CHECK(copy.build_remaining == 0.0F);
    packet_layer_flush(joiner.connection.packets, 0, true);
    CHECK(sent_of(joiner.inbox, RecordType::builder_link).empty());

    host.inbox.sent.clear();
    auto* building = host.match->create({0, kStructure, {140u << 16, 0, 140u << 16}, true, 1, 0});
    CHECK(building != nullptr);
    if (building == nullptr)
        return;
    const auto building_id = building->unit_index;
    packet_layer_flush(host.connection.packets, 0, true);
    const auto records = sent_records(host.inbox);
    CHECK(records.size() == 2 && records[0].bytes[0] == 0x09 && records[1].bytes[0] == 0x12);
    if (records.size() == 2)
        CHECK(
            (records[1].bytes ==
             std::vector<uint8_t>{
                 0x12, static_cast<uint8_t>(building_id), 0, static_cast<uint8_t>(building_id), 0
             })
        );
    (void)net_match_pump(joiner.net.get());
    // The copy is created unfinished, and the 0x12 a tick later finishes it.
    const auto& building_copy = joiner.match->state().units[building_id];
    CHECK(building_copy.type_index == kStructure && building_copy.build_remaining != 0.0F);
    pump_next_tick(joiner);
    CHECK(building_copy.build_remaining == 0.0F);
    CHECK(joiner.net->record_errors == 0);
}

// units.build-rotation over the wire: a building the host places facing
// east carries the facing in the heading of its 0x09 record, and the
// joiner's copy stands turned, its footprint's width and depth swapped.
// Without the rule both machines place it facing south.
void turned_buildings_reach_the_copy() {
    constexpr uint16_t kStructure = 3;
    // Two cells wide, one deep; turned east, one wide and two deep.
    static constexpr std::array<uint8_t, 2> kLongYard{4, 4};
    for (const bool rule : {true, false}) {
        Machine host(kStructure);
        Machine joiner(kStructure);
        host.inbox.peer = &joiner.inbox;
        joiner.inbox.peer = &host.inbox;
        for (Machine* m : {&host, &joiner}) {
            m->types[kStructure].footprint_x = 2;
            m->types[kStructure].footprint_z = 1;
            m->loaded[kStructure].type = m->types[kStructure];
            m->structure_def.footprint_x = 2;
            m->structure_def.footprint_z = 1;
            m->fields[kStructure].yard_mask = kLongYard;
            m->rules.units.build_rotation.enabled = rule;
            m->unit_type_rules.resize(m->types.size());
            m->unit_type_rules[kStructure].build_facings =
                data::match_rules::build_facing::south | data::match_rules::build_facing::east;
        }
        host.start(0);
        joiner.start(1);
        const auto facing = host.match->build_facing(kStructure, sim::unit_spawn::facing_east);
        CHECK(facing == (rule ? sim::unit_spawn::facing_east : sim::unit_spawn::facing_south));
        auto* building =
            host.match->create({0, kStructure, {128u << 16, 0, 128u << 16}, true, 1, 0, facing});
        CHECK(building != nullptr);
        if (building == nullptr)
            continue;
        const auto building_id = building->unit_index;
        const auto& original = host.match->state().units[building_id];
        CHECK(original.footprint_x == (rule ? 1 : 2) && original.footprint_z == (rule ? 2 : 1));
        packet_layer_flush(host.connection.packets, 0, true);
        (void)net_match_pump(joiner.net.get());
        const auto& copy = joiner.match->state().units[building_id];
        CHECK(copy.type_index == kStructure && copy.heading == original.heading);
        CHECK(copy.footprint_x == original.footprint_x && copy.footprint_z == original.footprint_z);
        CHECK(joiner.match->unit_build_facing(copy) == facing);
        CHECK(host.match->unit_build_facing(original) == facing);
        CHECK(joiner.net->record_errors == 0);
    }
}

// A wreck a unit of the host raised back into a unit goes out as the
// reclaim sequence (0x0f 0xff) at the wreck's origin plot, from that unit's
// owner, and the joiner's wreck is taken away.
void resurrected_wrecks_leave_every_machine() {
    KillScene scene;
    auto& host = *scene.pair.host;
    auto& joiner = *scene.pair.joiner;
    constexpr int32_t x = 5;
    constexpr int32_t z = 6;
    for (auto* m : {&host, &joiner}) {
        auto& world = m->match->state();
        const auto plot = static_cast<std::size_t>(z) * world.game.map_width + x;
        (void)sim::feature_runtime::place_feature(
            world,
            m->match->feature_host(),
            plot,
            kDeadFeature,
            nullptr,
            nullptr,
            sim::feature_runtime::no_player
        );
        CHECK(world.plots[plot].feature == kDeadFeature);
    }
    auto& multiplayer = host.match->multiplayer;
    multiplayer.feature_changed(
        multiplayer.context, sim::feature_runtime::FeatureChange::resurrected, x, z, scene.attacker
    );
    packet_layer_flush(host.connection.packets, 0, true);
    const auto sent = sent_of(host.inbox, RecordType::feature_event);
    CHECK(sent.size() == 1);
    if (sent.size() == 1) {
        CHECK(sent[0].from == kIds[0] && sent[0].to == broadcast_destination_id);
        CHECK((sent[0].bytes == std::vector<uint8_t>{0x0f, 0xff, x, 0, z, 0}));
    }
    (void)net_match_pump(joiner.net.get());
    auto& joiner_world = joiner.match->state();
    const auto plot = static_cast<std::size_t>(z) * joiner_world.game.map_width + x;
    CHECK(joiner_world.plots[plot].feature != kDeadFeature);
    CHECK(joiner.net->record_errors == 0);
}

// A departed player's units are destroyed on every machine: the joiner's
// player is reported gone (0x1c), and the host's copies of its units blow up
// and die there with no 0x0c from the host. The host passes the notice on,
// and the joiner, told that its own player is gone, loses at once.
void departed_players_units_are_destroyed() {
    Pair pair;
    auto& host = *pair.host;
    auto& joiner = *pair.joiner;
    auto* owned = joiner.match->create({1, 1, {160u << 16, 0, 160u << 16}, true, 1, 0});
    CHECK(owned != nullptr);
    if (owned == nullptr)
        return;
    const auto index = owned->unit_index;
    for (uint32_t t = 0; t < 4; ++t) {
        step(host);
        step(joiner);
    }
    auto& copy = host.match->state().units[index];
    CHECK((copy.flags & OA_UNIT_FLAG_LIVE) != 0);
    host.inbox.sent.clear();
    DisconnectNoticeRecord gone{};
    gone.player_id = kIds[1];
    uint8_t bytes[5];
    CHECK(encode_record(gone, bytes, sizeof bytes, nullptr) == WireError::ok);
    CHECK(net_match_send(joiner.net.get(), kIds[1], broadcast_destination_id, bytes, sizeof bytes));
    packet_layer_flush(joiner.connection.packets, 0, true);
    (void)net_match_pump(host.net.get());
    CHECK(host.match->state().game.players[1].in_use == 0);
    CHECK((copy.flags & OA_UNIT_FLAG_LIVE) == 0 || (copy.flags & OA_UNIT_FLAG_DEATH_PENDING) != 0);
    for (uint32_t t = 0; t < 4; ++t)
        step(host);
    CHECK((copy.flags & OA_UNIT_FLAG_LIVE) == 0);
    CHECK(sent_of(host.inbox, RecordType::unit_killed).empty());
    const auto relayed = sent_of(host.inbox, RecordType::disconnect_notice);
    CHECK(relayed.size() == 1 && relayed[0].from == kIds[0]);
    CHECK(joiner.match->outcome() == sim::scenario::Outcome::ongoing);
    (void)net_match_pump(joiner.net.get());
    CHECK(joiner.match->outcome() == sim::scenario::Outcome::defeat);
}

// A pause from the joiner stops the host's tick: the step that hears it
// runs to its end, then the host's tick, its 0x2c records and so the full
// record rotation hold until the resume, and go on with the next tick.
void a_pause_from_elsewhere_holds_the_tick() {
    Pair pair;
    auto& host = *pair.host;
    auto& joiner = *pair.joiner;
    auto* owned = host.match->create({0, 1, {60u << 16, 0, 60u << 16}, true, 1, 0});
    CHECK(owned != nullptr);
    for (uint32_t t = 0; t < 4; ++t) {
        step(host);
        step(joiner);
    }
    host.inbox.sent.clear();
    net_match_set_pause(joiner.net.get(), true);
    packet_layer_flush(joiner.connection.packets, 0, true);
    step(host); // hears the pause after its tick moved on
    auto& host_game = host.match->state().game;
    CHECK((host_game.sim_run_flags & run_flag_paused) != 0);
    const auto held = host_game.tick;
    for (uint32_t t = 0; t < 10; ++t) {
        step(host);
        step(joiner);
    }
    CHECK(host_game.tick == held);
    const auto before_resume = sent_of(host.inbox, RecordType::unit_state).size();
    net_match_set_pause(joiner.net.get(), false);
    packet_layer_flush(joiner.connection.packets, 0, true);
    for (uint32_t t = 0; t < 6; ++t) {
        step(host);
        step(joiner);
    }
    CHECK(host_game.tick > held && (host_game.sim_run_flags & run_flag_paused) == 0);
    const auto states = sent_of(host.inbox, RecordType::unit_state);
    CHECK(states.size() > before_resume);
    // Every 0x2c carries its tick at +3: one per tick, none skipped.
    uint32_t previous = 0;
    bool consecutive = true;
    for (const auto& record : states) {
        const auto tick = load_u32(record.bytes.data() + 3);
        consecutive = consecutive && (previous == 0 || tick == previous + 1);
        previous = tick;
    }
    CHECK(consecutive && previous == host_game.tick);
    CHECK(host.net->record_errors == 0 && joiner.net->record_errors == 0);
}

// A unit the host gives to the joiner's player goes out from the unit's
// owner as a 0x14 record {unit, new owner's id, build progress truncated,
// health, bank and heading, pitch, the three stockpiles by the first
// slot's weapon}, followed by the unit's death as captured (0x0c, kind 4).
// The joiner creates the unit for its own player, finished, with the
// host's health and orientation, and announces it (0x09 from the joiner's
// player); the given unit dies on both machines. A 0x14 naming a player the
// receiving machine does not simulate changes nothing.
void given_units_reach_their_new_owner() {
    constexpr uint8_t kCaptured = 4; // the death kind of a unit handed over
    Pair pair(0, kMapCells, true);
    auto& host = *pair.host;
    auto& joiner = *pair.joiner;
    auto* gift = host.match->create({0, 1, {100u << 16, 0, 100u << 16}, true, 1, 0});
    CHECK(gift != nullptr);
    if (gift == nullptr)
        return;
    const auto gift_id = gift->unit_index;
    for (uint32_t t = 0; t < 4; ++t) {
        step(host);
        step(joiner);
    }
    auto& original = host.match->state().units[gift_id];
    auto& copy = joiner.match->state().units[gift_id];
    CHECK((copy.flags & OA_UNIT_FLAG_LIVE) != 0);
    original.health = 60;
    original.bank = 3;
    original.heading = 0x4000;
    original.pitch = -2;
    const bool armed = (original.weapons[0].flags & OA_UNIT_WEAPON_ENABLED) != 0;
    CHECK(armed);
    original.weapons[0].stockpile = 2;
    const auto owned_by_joiner = [](Machine& m) {
        std::vector<uint16_t> units;
        const auto& world = m.match->state();
        for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot)
            if (world.units[slot].type_index != 0 && world.units[slot].owner_index == 1 &&
                (world.units[slot].flags & OA_UNIT_FLAG_LIVE) != 0)
                units.push_back(static_cast<uint16_t>(slot));
        return units;
    };
    CHECK(owned_by_joiner(joiner).empty());
    host.inbox.sent.clear();
    joiner.inbox.sent.clear();
    host.match->transfer_unit(gift_id, 1);
    // The unit dies as its next tick settles deaths, which shares it.
    step(host);
    const auto records = sent_records(host.inbox);
    std::size_t transfer_at = records.size();
    std::size_t kill_at = records.size();
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (records[i].bytes[0] == static_cast<uint8_t>(RecordType::unit_transfer) &&
            transfer_at == records.size())
            transfer_at = i;
        if (records[i].bytes[0] == static_cast<uint8_t>(RecordType::unit_killed) &&
            kill_at == records.size())
            kill_at = i;
    }
    CHECK(transfer_at < kill_at && kill_at < records.size());
    if (kill_at < records.size()) {
        const auto& transfer = records[transfer_at];
        CHECK(transfer.from == kIds[0] && transfer.to == broadcast_destination_id);
        const std::vector<uint8_t> expected{
            0x14,
            static_cast<uint8_t>(gift_id),
            0, // unit
            0x03,
            0x10,
            0,
            0, // new owner's id (kIds[1])
            0,
            0,
            0,
            0, // build progress 0
            60,
            0,
            0,
            0, // health
            3,
            0,
            0x00,
            0x40, // bank, heading
            0xfe,
            0xff, // pitch
            2,
            original.weapons[1].stockpile,
            original.weapons[2].stockpile
        };
        CHECK(transfer.bytes == expected);
        const auto& kill = records[kill_at];
        CHECK(kill.from == kIds[0] && kill.bytes[1] == static_cast<uint8_t>(gift_id));
        CHECK((kill.bytes[10] >> unit_killed_kind_shift) == kCaptured);
    }
    for (uint32_t t = 0; t < 4; ++t) {
        step(joiner);
        step(host);
    }
    const auto joiner_units = owned_by_joiner(joiner);
    CHECK(joiner_units.size() == 1);
    if (joiner_units.size() == 1) {
        const auto& received = joiner.match->state().units[joiner_units[0]];
        CHECK(joiner_units[0] != gift_id && received.type_index == 1);
        CHECK(received.health == 60 && received.build_remaining == 0.0F);
        CHECK(received.bank == 3 && received.heading == 0x4000 && received.pitch == -2);
        CHECK(received.weapons[0].stockpile == 2);
        const auto created = sent_of(joiner.inbox, RecordType::unit_created);
        CHECK(created.size() == 1 && created[0].from == kIds[1]);
        // The host holds the joiner's new unit as the joiner's.
        CHECK((owned_by_joiner(host) == std::vector<uint16_t>{joiner_units[0]}));
    }
    CHECK((original.flags & OA_UNIT_FLAG_LIVE) == 0 && (copy.flags & OA_UNIT_FLAG_LIVE) == 0);
    CHECK(sent_of(joiner.inbox, RecordType::unit_transfer).empty());

    // A 0x14 handing a host unit to the host's own player, which the joiner
    // does not simulate, changes nothing there.
    auto* kept = host.match->create({0, 1, {140u << 16, 0, 100u << 16}, true, 1, 0});
    CHECK(kept != nullptr);
    if (kept == nullptr)
        return;
    for (uint32_t t = 0; t < 4; ++t) {
        step(host);
        step(joiner);
    }
    auto transfer = unit_transfer_record(host.match->state().units[kept->unit_index], kIds[0]);
    uint8_t bytes[24];
    CHECK(encode_record(transfer, bytes, sizeof bytes, nullptr) == WireError::ok);
    const auto before = owned_by_joiner(joiner).size();
    CHECK(net_match_send(host.net.get(), kIds[0], broadcast_destination_id, bytes, sizeof bytes));
    packet_layer_flush(host.connection.packets, 0, true);
    pump_next_tick(joiner);
    CHECK(owned_by_joiner(joiner).size() == before);
    CHECK((joiner.match->state().units[kept->unit_index].flags & OA_UNIT_FLAG_LIVE) != 0);
    CHECK(joiner.net->record_errors == 0 && host.net->record_errors == 0);
}

// A hovercraft and a floater at sea take the same height on the machine
// that simulates them and on the other, where they are copies driven by
// their owner's records: the hovercraft on the surface, the floater its
// waterline below it.
// Runs both machines until `done` holds or `limit` rounds have passed.
template <typename Done>
bool run_pair_until(Pair& pair, uint32_t limit, Done done) {
    for (uint32_t t = 0; t < limit; ++t) {
        if (done())
            return true;
        step(*pair.host);
        step(*pair.joiner);
    }
    return done();
}

// A flying transport's pickup starts BeginTransport with the cargo's height,
// and the 0x10 record carries it: the copy's script keeps the same argument.
void flying_carrier_shares_the_cargo_height() {
    Pair pair{0, kMapCells, false, false, false, true};
    auto& host = *pair.host;
    auto& joiner = *pair.joiner;
    auto* transport =
        host.match->create({0, kTransportType, {64u << 16, 0, 64u << 16}, true, 1, 0});
    auto* cargo = host.match->create({0, kCargoType, {160u << 16, 0, 96u << 16}, true, 1, 0});
    CHECK(transport != nullptr && cargo != nullptr);
    if (transport == nullptr || cargo == nullptr)
        return;
    const auto carrier = transport->unit_index;
    const auto load = cargo->unit_index;
    (void)run_pair_until(pair, 2 * kUnitsPerPlayer, [] { return false; });
    host.inbox.sent.clear();
    joiner.inbox.sent.clear();
    host.match->issue_load(carrier, load, false);
    const auto& owner_cargo = host.match->state().units[load];
    CHECK(run_pair_until(pair, 1200, [&] {
        return oa_unit_slot_from_ref(owner_cargo.attach_parent) == carrier;
    }));
    const auto height =
        static_cast<uint32_t>(host.match->state().unit_defs[kCargoType].model_height);
    CHECK(height == static_cast<uint32_t>(kCargoHeight) << 16);
    int32_t begin_transport = -1;
    const auto& names = host.carrier_script->scripts;
    for (std::size_t i = 0; i < names.size(); ++i)
        if (names[i].name == "BeginTransport")
            begin_transport = static_cast<int32_t>(i);
    bool shared = false;
    for (const auto& start : sent_of(host.inbox, RecordType::cob_start)) {
        CobStartRecord record{};
        if (decode_record(start.bytes.data(), start.bytes.size(), &record) != WireError::ok)
            continue;
        shared =
            shared || (record.unit_index == carrier && record.function_index == begin_transport &&
                       record.argument_count == 1 && record.args[0] == height);
    }
    CHECK(shared);
    // The copy ran BeginTransport with the owner's argument.
    std::array<int32_t, 4> owner_statics{};
    std::array<int32_t, 4> copy_statics{};
    auto* owner_object = host.match->instance(carrier);
    auto* copy_object = joiner.match->instance(carrier);
    CHECK(owner_object != nullptr && copy_object != nullptr);
    if (owner_object == nullptr || copy_object == nullptr)
        return;
    CHECK(owner_object->script()->query("Statics", owner_statics));
    CHECK(copy_object->script()->query("Statics", copy_statics));
    CHECK(static_cast<uint32_t>(owner_statics[0]) == height && copy_statics[0] == owner_statics[0]);
    // The carry link reaches the copy.
    (void)run_pair_until(pair, 2 * kUnitsPerPlayer, [] { return false; });
    CHECK(oa_unit_slot_from_ref(joiner.match->state().units[load].attach_parent) == carrier);
    CHECK(host.net->record_errors == 0 && joiner.net->record_errors == 0);
}

// A loaded transport lands on another player's allied pad and hands its cargo
// to the pad's landing piece: the cargo ends where the pad's piece holds it on
// both machines, the transport free of it.
void cargo_handed_to_another_players_pad() {
    Pair pair{0, kMapCells, false, false, false, true};
    auto& host = *pair.host;
    auto& joiner = *pair.joiner;
    for (auto* machine : {&host, &joiner})
        for (uint8_t slot = 0; slot < 2; ++slot)
            machine->match->configure_player_alliances(slot, {1, 1});
    auto* transport =
        host.match->create({0, kTransportType, {48u << 16, 0, 48u << 16}, true, 1, 0});
    auto* cargo = host.match->create({0, kCargoType, {112u << 16, 0, 48u << 16}, true, 1, 0});
    auto* pad = joiner.match->create({1, kPadType, {176u << 16, 0, 176u << 16}, true, 1, 0});
    CHECK(transport != nullptr && cargo != nullptr && pad != nullptr);
    if (transport == nullptr || cargo == nullptr || pad == nullptr)
        return;
    const auto carrier = transport->unit_index;
    const auto load = cargo->unit_index;
    const auto deck = pad->unit_index;
    (void)run_pair_until(pair, 4 * kUnitsPerPlayer, [] { return false; });
    CHECK((host.match->state().units[deck].flags & OA_UNIT_FLAG_LIVE) != 0);
    host.match->issue_load(carrier, load, false);
    const auto& owner_cargo = host.match->state().units[load];
    CHECK(run_pair_until(pair, 1200, [&] {
        return oa_unit_slot_from_ref(owner_cargo.attach_parent) == carrier;
    }));
    (void)run_pair_until(pair, 60, [&] {
        const auto* head = host.match->world().slots.at(carrier).unit->primary;
        return head == nullptr || head->kind != kVtolPickupKind;
    });
    host.match->issue_order(
        carrier, sim::ground_orders::vtol_landing_kind, false, deck, nullptr, 0, 0
    );
    CHECK(run_pair_until(pair, 2400, [&] {
        return oa_unit_slot_from_ref(owner_cargo.attach_parent) == deck;
    }));
    (void)run_pair_until(pair, 4 * kUnitsPerPlayer, [] { return false; });
    const auto& copy_cargo = joiner.match->state().units[load];
    CHECK(oa_unit_slot_from_ref(copy_cargo.attach_parent) == deck);
    CHECK(static_cast<int8_t>(owner_cargo.attach_piece) == kLinkPiece);
    CHECK(host.match->state().units[carrier].attach_first_child == 0);
    CHECK(
        owner_cargo.position.x == copy_cargo.position.x &&
        owner_cargo.position.y == copy_cargo.position.y &&
        owner_cargo.position.z == copy_cargo.position.z
    );
    CHECK(host.net->record_errors == 0 && joiner.net->record_errors == 0);
}

void naval_units_agree_on_their_height() {
    Pair pair{0, kMapCells, false, false, true};
    auto& host = *pair.host;
    auto& joiner = *pair.joiner;
    auto* hover = joiner.match->create({1, kHoverType, {64u << 16, 0, 64u << 16}, true, 1, 0});
    auto* floater = joiner.match->create({1, kFloaterType, {128u << 16, 0, 96u << 16}, true, 1, 0});
    // A ground unit under the sea, as a submarine, rests on the sea floor.
    auto* diver = joiner.match->create({1, 1, {96u << 16, 0, 128u << 16}, true, 1, 0});
    CHECK(hover != nullptr && floater != nullptr && diver != nullptr);
    if (hover == nullptr || floater == nullptr || diver == nullptr)
        return;
    const auto hover_id = hover->unit_index;
    const auto floater_id = floater->unit_index;
    const auto diver_id = diver->unit_index;
    // A copy's height is refreshed after its movement step, so its sea
    // occupy code follows its owner's one record list later: the steps run
    // on well past the hovercraft settling.
    for (uint32_t t = 0; t < 16 * kUnitsPerPlayer; ++t) {
        step(joiner);
        step(host);
    }
    const auto& owner_hover = joiner.match->state().units[hover_id];
    const auto& copy_hover = host.match->state().units[hover_id];
    const auto& owner_floater = joiner.match->state().units[floater_id];
    const auto& copy_floater = host.match->state().units[floater_id];
    CHECK(
        (copy_hover.flags & OA_UNIT_FLAG_LIVE) != 0 && (copy_floater.flags & OA_UNIT_FLAG_LIVE) != 0
    );
    CHECK(
        (owner_floater.position.y >> 16) == static_cast<int32_t>(kNavalSeaLevel) - kFloaterWaterline
    );
    CHECK(
        (owner_hover.position.y >> 16) >= static_cast<int32_t>(kNavalSeaLevel) - 1 &&
        (owner_hover.position.y >> 16) <= static_cast<int32_t>(kNavalSeaLevel) + 1
    );
    CHECK((copy_floater.position.y >> 16) == (owner_floater.position.y >> 16));
    CHECK((copy_hover.position.y >> 16) == (owner_hover.position.y >> 16));
    // Each machine keeps the sea occupy code its own movement tick finds,
    // and the copies agree with their owners.
    const auto occupy = [](const Unit& unit) { return sim::world_environment::sea_occupy(unit); };
    CHECK(occupy(owner_floater) != sim::world_environment::sea_occupy_none);
    CHECK(occupy(owner_hover) != sim::world_environment::sea_occupy_none);
    CHECK(occupy(copy_floater) == occupy(owner_floater));
    CHECK(occupy(copy_hover) == occupy(owner_hover));
    const auto& owner_diver = joiner.match->state().units[diver_id];
    const auto& copy_diver = host.match->state().units[diver_id];
    CHECK((owner_diver.position.y >> 16) == 0 && (copy_diver.position.y >> 16) == 0);
    CHECK(
        occupy(owner_diver) == sim::world_environment::sea_occupy_submerged &&
        occupy(copy_diver) == sim::world_environment::sea_occupy_submerged
    );
    CHECK(host.net->record_errors == 0 && joiner.net->record_errors == 0);
}

// A profile that raises weapon types sends shots as 0xe0. The joiner
// launches a unit's shot from the weapon in the named slot: id 300 is that
// slot's weapon, so the projectile's def is its ref, and an id under 256 in
// the next slot launches that slot's weapon. A meteor the host shares falls
// on the joiner, including one past 255. A line-of-sight weapon shared the
// same way does not: a shot with no source unit is relaunched only for a
// meteor. 3.1c rules keep the shared shot as 0x0d.
void remote_wide_shots_reach_the_copy() {
    Machine host_machine(0, kMapCells, true);
    Machine joiner_machine(0, kMapCells, true);
    const char* wide_gun =
        "[W300]{id=300; reloadtime=0.1; range=400; lineofsight=1; weaponvelocity=100; turret=1; "
        "[DAMAGE]{default=10;}}";
    const char* wide_rock = "[ROCK301]{id=301; reloadtime=1; meteor=1;}";
    CHECK(sim::combat_state::install_weapon_text(host_machine.weapons, wide_gun, false, 1024) == 1);
    CHECK(
        sim::combat_state::install_weapon_text(joiner_machine.weapons, wide_gun, false, 1024) == 1
    );
    CHECK(
        sim::combat_state::install_weapon_text(host_machine.weapons, wide_rock, false, 1024) == 1
    );
    CHECK(
        sim::combat_state::install_weapon_text(joiner_machine.weapons, wide_rock, false, 1024) == 1
    );
    // The unit's first slot is W300. Its second stays TESTGUN, the weapon
    // the constructor armed it with, so an id under 256 still has a slot.
    host_machine.def.weapon1 = "W300";
    host_machine.def.weapon2 = "TESTGUN";
    joiner_machine.def.weapon1 = "W300";
    joiner_machine.def.weapon2 = "TESTGUN";
    host_machine.inbox.peer = &joiner_machine.inbox;
    joiner_machine.inbox.peer = &host_machine.inbox;
    host_machine.start(0);
    joiner_machine.start(1);
    host_machine.net->rules.weapon_types = 1024;
    joiner_machine.net->rules.weapon_types = 1024;
    host_machine.connection.packets->receiver.wide_records = true;
    joiner_machine.connection.packets->receiver.wide_records = true;

    auto* owned = host_machine.match->create({0, 1, {40u << 16, 0, 40u << 16}, true, 1, 0});
    CHECK(owned != nullptr);
    if (owned == nullptr)
        return;
    const auto index = owned->unit_index;
    for (uint32_t t = 0; t < 4; ++t) {
        step(host_machine);
        step(joiner_machine);
    }
    host_machine.inbox.sent.clear();
    joiner_machine.inbox.sent.clear();
    auto& world = joiner_machine.match->state();
    const auto deliver = [&] {
        packet_layer_flush(host_machine.connection.packets, 0, true);
        (void)net_match_pump(joiner_machine.net.get());
    };
    const auto send_wide = [&](const WideWeaponFireRecord& record) {
        uint8_t bytes[64];
        std::size_t written = 0;
        CHECK(encode_record(record, bytes, sizeof bytes, &written) == WireError::ok);
        CHECK(written == wide_record_length_table[0]);
        CHECK(net_match_send(
            host_machine.net.get(), kIds[0], broadcast_destination_id, bytes, written
        ));
        deliver();
    };
    const auto sent_weapon_id = [&] {
        WideWeaponFireRecord decoded{};
        bool found = false;
        for (const auto& record : sent_records(host_machine.inbox)) {
            if (record.bytes.empty() || record.bytes[0] != 0xe0)
                continue;
            CHECK(
                decode_record(record.bytes.data(), record.bytes.size(), &decoded) == WireError::ok
            );
            found = true;
        }
        CHECK(found);
        return decoded.weapon_id;
    };

    CHECK(world.units[index].weapons[0].def == oa_ref_from_index(300));
    CHECK(world.units[index].weapons[1].def == oa_ref_from_index(kGun));
    const auto* wide_def = world_weapon_def(&world, oa_ref_from_index(300));
    CHECK(wide_def != nullptr && oa_weapon_def_id(wide_def) == 300);
    CHECK(world_weapon_ref(&world, wide_def) == oa_ref_from_index(300));
    const auto* wide_meteor = world_weapon_def(&world, oa_ref_from_index(301));
    CHECK(wide_meteor != nullptr && oa_weapon_def_id(wide_meteor) == 301);
    CHECK(world_weapon_ref(&world, wide_meteor) == oa_ref_from_index(301));

    WideWeaponFireRecord fire{};
    fire.start[0] = 41 << 16;
    fire.start[1] = 12 << 16;
    fire.start[2] = 40 << 16;
    fire.target[0] = 140 << 16;
    fire.target[1] = 0;
    fire.target[2] = 90 << 16;
    fire.weapon_id = 300;
    fire.aim_heading = 0x1234;
    fire.aim_pitch = -0x100;
    fire.source_unit_index = index;
    fire.slot = 0;
    const auto before = world.game.projectile_count;
    send_wide(fire);
    CHECK(world.game.projectile_count == before + 1);
    const auto& wide_shot = world.projectiles[before];
    CHECK(wide_shot.def == oa_ref_from_index(300));
    CHECK(wide_shot.source == oa_unit_ref_from_slot(index) && wide_shot.owner_index == 0);
    CHECK(wide_shot.position.x == fire.start[0] && wide_shot.target.x == fire.target[0]);
    CHECK(
        world.units[index].weapons[0].aim_heading == 0x1234 &&
        world.units[index].weapons[0].aim_pitch == -0x100
    );
    CHECK(joiner_machine.net->wide_records_applied == 1);

    fire.weapon_id = kGun;
    fire.slot = 1;
    send_wide(fire);
    CHECK(world.game.projectile_count == before + 2);
    CHECK(world.projectiles[before + 1].def == oa_ref_from_index(kGun));
    CHECK(world.projectiles[before + 1].source == oa_unit_ref_from_slot(index));
    CHECK(
        world.units[index].weapons[1].aim_heading == 0x1234 &&
        world.units[index].weapons[1].aim_pitch == -0x100
    );

    host_machine.inbox.sent.clear();
    auto applied = joiner_machine.net->wide_records_applied;
    auto shots_before = world.game.projectile_count;
    // launch_meteor is the public path into the binding's shot_fired.
    const FixedVec3 fall_from{fire.start[0], fire.start[1], fire.start[2]};
    const FixedVec3 fall{0, -(8 << 16), 0};
    CHECK(host_machine.match->launch_meteor(oa_ref_from_index(kGun), fall_from, fall, true));
    deliver();
    CHECK(count_sent_type(host_machine.inbox, 0xe0) == 1);
    CHECK(sent_weapon_id() == kGun);
    CHECK(sent_of(host_machine.inbox, RecordType::weapon_fire).empty());
    CHECK(joiner_machine.net->wide_records_applied == applied + 1);
    CHECK(world.game.projectile_count == shots_before);

    host_machine.inbox.sent.clear();
    applied = joiner_machine.net->wide_records_applied;
    shots_before = world.game.projectile_count;
    CHECK(host_machine.match->launch_meteor(oa_ref_from_index(kRock), fall_from, fall, true));
    deliver();
    CHECK(count_sent_type(host_machine.inbox, 0xe0) == 1);
    CHECK(sent_weapon_id() == kRock);
    CHECK(sent_of(host_machine.inbox, RecordType::weapon_fire).empty());
    CHECK(joiner_machine.net->wide_records_applied == applied + 1);
    CHECK(world.game.projectile_count == shots_before + 1);
    const auto& rock = world.projectiles[world.game.projectile_count - 1];
    CHECK(rock.def == oa_ref_from_index(kRock) && rock.source == 0 && rock.owner_index == 10);
    CHECK(
        rock.position.x == fall_from.x && rock.position.y == fall_from.y &&
        rock.position.z == fall_from.z
    );
    CHECK(rock.velocity.x == fall.x && rock.velocity.y == fall.y && rock.velocity.z == fall.z);

    host_machine.inbox.sent.clear();
    applied = joiner_machine.net->wide_records_applied;
    shots_before = world.game.projectile_count;
    CHECK(host_machine.match->launch_meteor(oa_ref_from_index(301), fall_from, fall, true));
    deliver();
    CHECK(count_sent_type(host_machine.inbox, 0xe0) == 1);
    CHECK(sent_weapon_id() == 301);
    CHECK(sent_of(host_machine.inbox, RecordType::weapon_fire).empty());
    CHECK(joiner_machine.net->wide_records_applied == applied + 1);
    CHECK(world.game.projectile_count == shots_before + 1);
    const auto& far_rock = world.projectiles[world.game.projectile_count - 1];
    CHECK(
        far_rock.def == oa_ref_from_index(301) && far_rock.source == 0 && far_rock.owner_index == 10
    );
    CHECK(
        joiner_machine.connection.packets->traffic
            .record_count[static_cast<uint8_t>(RecordType::weapon_fire)]
                         [static_cast<uint8_t>(TrafficChannel::received)] == 0
    );
    CHECK(joiner_machine.net->record_errors == 0);

    Pair stock(0, kMapCells, true);
    auto* stock_unit = stock.host->match->create({0, 1, {40u << 16, 0, 40u << 16}, true, 1, 0});
    CHECK(stock_unit != nullptr);
    if (stock_unit == nullptr)
        return;
    for (uint32_t t = 0; t < 4; ++t) {
        step(*stock.host);
        step(*stock.joiner);
    }
    stock.host->inbox.sent.clear();
    const FixedVec3 stock_from{10 << 16, 40 << 16, 20 << 16};
    const FixedVec3 stock_fall{0, -(8 << 16), 0};
    CHECK(stock.host->match->launch_meteor(oa_ref_from_index(kGun), stock_from, stock_fall, true));
    packet_layer_flush(stock.host->connection.packets, 0, true);
    (void)net_match_pump(stock.joiner->net.get());
    CHECK(sent_of(stock.host->inbox, RecordType::weapon_fire).size() == 1);
    CHECK(count_sent_type(stock.host->inbox, 0xe0) == 0);
    CHECK(stock.joiner->net->wide_records_applied == 0);
    CHECK(stock.host->net->wide_records_applied == 0);
}

// The same interception under wide rules arrives as two 0xe1 records.
void wide_interceptions_reach_the_shot_s_owner() {
    Pair pair(0, kMapCells, true);
    auto& host = *pair.host;
    auto& joiner = *pair.joiner;
    install_interceptors(host);
    install_interceptors(joiner);
    auto* defender = host.match->create({0, 1, {40u << 16, 0, 200u << 16}, true, 1, 0});
    auto* launcher = joiner.match->create({1, 1, {200u << 16, 0, 40u << 16}, true, 1, 0});
    CHECK(defender != nullptr && launcher != nullptr);
    if (defender == nullptr || launcher == nullptr)
        return;
    const auto defender_id = defender->unit_index;
    const auto launcher_id = launcher->unit_index;
    for (uint32_t t = 0; t < 4; ++t) {
        step(host);
        step(joiner);
    }
    host.net->rules.weapon_types = 1024;
    joiner.net->rules.weapon_types = 1024;
    host.connection.packets->receiver.wide_records = true;
    joiner.connection.packets->receiver.wide_records = true;
    host.inbox.sent.clear();
    joiner.inbox.sent.clear();
    auto& interceptor = launch_shot(host, kInterceptor, 128, 128, 0, defender_id);
    auto& missile_copy = launch_shot(host, kMissile, 150, 128, 1, launcher_id);
    auto& missile = launch_shot(joiner, kMissile, 150, 128, 1, launcher_id);
    auto& interceptor_copy = launch_shot(joiner, kInterceptor, 128, 128, 0, defender_id);
    try {
        host.match->detonate(interceptor, nullptr);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "detonate: %s\n", error.what());
    }
    CHECK(retired(missile_copy));
    packet_layer_flush(host.connection.packets, 0, true);
    CHECK(count_sent_type(host.inbox, 0xe1) == 2);
    CHECK(sent_of(host.inbox, RecordType::projectile_intercepted).empty());
    const auto sent = sent_records(host.inbox);
    std::size_t wide_seen = 0;
    for (const auto& record : sent) {
        if (record.bytes.empty() || record.bytes[0] != 0xe1)
            continue;
        CHECK(record.bytes.size() == wide_record_length_table[1]);
        CHECK(record.from == kIds[0] && record.to == broadcast_destination_id);
        ++wide_seen;
    }
    CHECK(wide_seen == 2);
    CHECK(!retired(missile) && !retired(interceptor_copy));
    (void)net_match_pump(joiner.net.get());
    CHECK(retired(missile) && !retired(interceptor_copy));
    pump_next_tick(joiner);
    CHECK(retired(interceptor_copy));
    CHECK(joiner.net->wide_records_applied == 2);
    CHECK(joiner.net->record_errors == 0);
}

int main() {
    replicated_play();
    remote_driver_brakes_and_blocks();
    remote_aircraft_follow_their_owner();
    aircraft_arriving_inside_a_radius_keep_their_copy();
    carried_copies_keep_their_drivers();
    remote_events_reach_the_copy();
    death_record_for_unit_zero_changes_nothing();
    owner_script_starts_reach_the_copy();
    remote_shots_reach_the_copy();
    remote_wide_shots_reach_the_copy();
    owner_kill_is_shared_with_its_wreck();
    received_kill_credits_the_record();
    create_into_live_slot_runs_the_kill();
    repeated_create_under_recorder_rules_is_dropped();
    create_past_the_table_is_counted_apart();
    feature_hits_go_to_the_host();
    reclaims_go_out_from_the_reclaiming_unit_s_owner();
    interceptions_reach_the_shot_s_owner();
    wide_interceptions_reach_the_shot_s_owner();
    carry_links_reach_the_copy();
    finished_units_reach_the_copy();
    turned_buildings_reach_the_copy();
    resurrected_wrecks_leave_every_machine();
    departed_players_units_are_destroyed();
    a_pause_from_elsewhere_holds_the_tick();
    foreign_cargo_dies_with_its_carrier();
    naval_units_agree_on_their_height();
    flying_carrier_shares_the_cargo_height();
    cargo_handed_to_another_players_pad();
    given_units_reach_their_new_owner();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("net match runtime: all tests passed");
    return 0;
}
