// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A D-gun kill shared between two machines, over the installed game's ARMCOM
// and ARMSOLAR (FBI, 3DO and COB), every weapon TDF and the armsolar corpse
// chain: two match runtimes joined through the match binding over an
// in-memory wire, at 250 and at 10 units a player.
//
// The host's commander D-guns the joiner's solars. The joiner simulates the
// solars, so it kills each at percentage 100, where the solar's own Killed
// script steps past the heap, and shares the death as a 0x0c record; the
// host's copies die with that outcome, so neither machine shows a wreck. A
// kill at a low percentage leaves the same armsolar_dead on both machines,
// a received 0x0c is applied as sent (its wreck, Killed pieces and credit),
// and a 0x09 into a slot still holding a copy kills that copy by its own
// health first.

#include "oa/netgame/condenser.hpp"
#include "oa/netgame/match/match_binding.hpp"
#include "oa/netgame/records.hpp"

#include "oa/data/defs/asset_files.hpp"
#include "oa/data/defs/unit_catalog.hpp"
#include "oa/data/defs/unit_def_loader.hpp"
#include "oa/data/defs/unit_header.hpp"
#include "oa/data/mission_types.hpp"
#include "oa/data/unit_definitions.hpp"
#include "oa/sim/combat_state.hpp"
#include "oa/sim/map_runtime.hpp"
#include "oa/sim/map_runtime/feature_defs.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/match_runtime/script.hpp"
#include "oa/sim/match_runtime/unit.hpp"
#include "oa/sim/unit_spawn/spawn_runtime.hpp"
#include "oa/test/game_assets.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
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

constexpr uint32_t kIds[2] = {0x2001, 0x1003}; // host, joiner; the joiner's id sorts first
constexpr uint8_t kHostSlot = 0;
constexpr uint8_t kJoinerSlot = 1;
constexpr uint16_t kMapCells = 64;
constexpr uint16_t kCommanderType = 1;
constexpr uint16_t kSolarType = 2;
constexpr uint8_t kWeaponKind = 1; // Unit.damage_kind of a weapon hit
constexpr int32_t kCommanderX = 320;
constexpr int32_t kRowZ = 512;
constexpr std::array<int32_t, 3> kSolarX{464, 544, 624};
// A solar collector's footprint reaches two plots either side of its cell.
constexpr int32_t kFootprintReach = 2;
constexpr float kFunds = 5000.0F;

// ------------------------------------------------------------------ game data

struct Reader : sim::unit_spawn::AssetReader {
    const AssetStore& store;

    explicit Reader(const AssetStore& s) : store(s) {}

    std::optional<std::vector<uint8_t>> read(std::string_view path) override {
        try {
            return store.read(path).bytes;
        } catch (const std::exception&) {
            return std::nullopt;
        }
    }
};

struct FeatureReader final : sim::map_runtime::FeatureAssetReader {
    const AssetStore& store;

    explicit FeatureReader(const AssetStore& s) : store(s) {}

    data::unit_definitions::Result<std::vector<std::string>> list_effective_recursive(
        std::string_view directory, std::string_view extension
    ) const override {
        try {
            return {store.list_effective_recursive(directory, extension), {}};
        } catch (const std::exception& error) {
            return {{}, {data::unit_definitions::ErrorCode::io, 0, error.what()}};
        }
    }

    data::unit_definitions::Result<std::string> read(std::string_view path) const override {
        try {
            const auto bytes = store.read(path).bytes;
            return {std::string(bytes.begin(), bytes.end()), {}};
        } catch (const std::exception& error) {
            return {{}, {data::unit_definitions::ErrorCode::io, 0, error.what()}};
        }
    }
};

struct LoadedUnit {
    data::unit_definitions::UnitDefinition def;
    data::unit_definitions::RuntimeDefinitionMetadata meta;
    sim::unit_spawn::LoadedType loaded;
    int16_t corpse = -1;
};

// The two unit types, every weapon and the features the solar's corpse chain
// needs, loaded once for every machine.
struct Catalog {
    AssetStore store = oa::test::require_game_assets("the D-gun kill shared between two machines");
    sim::combat_state::WeaponRegistry weapons;
    std::vector<formats::tdf::OwnedDocument> feature_docs;
    sim::map_runtime::FeatureDefTable features;
    std::vector<LoadedUnit> units; // index 0 reserved; kCommanderType ARMCOM; kSolarType ARMSOLAR
    data::unit_definitions::UnitTargetCategoryMasks target_masks;
    uint16_t solar_dead{};
    uint16_t solar_heap{};

    Catalog() {
        const auto files = data::defs::asset_store_files(&store);
        (void)sim::combat_state::install_weapon_files(weapons, files);
        auto docs = sim::map_runtime::load_feature_documents(FeatureReader(store));
        if (!docs)
            throw std::runtime_error("feature documents: " + docs.error.message);
        feature_docs = std::move(docs.value);
        const auto movement = std::make_unique<data::defs::MoveClassTable>();
        if (!data::defs::load_move_classes(&files, movement.get(), nullptr, nullptr))
            throw std::runtime_error("moveinfo does not load");
        // Each FBI as a match loads it: its header, then the whole file into a
        // UnitDef record, which the typed fields and metadata come from.
        data::defs::SoundCategoryTable sounds{};
        data::defs::WeaponTdfSet weapon_files{};
        data::defs::weapon_tdf_set_init(&weapon_files);
        data::defs::UnitDefTables tables{};
        data::defs::unit_def_tables_init(&tables);
        if (!data::defs::load_sound_categories(&files, &sounds, nullptr) ||
            !data::defs::load_weapon_tdf_set(&files, nullptr, false, &weapon_files) ||
            !data::defs::unit_def_tables_allocate(&tables, 3))
            throw std::runtime_error("the unit tables do not load");

        struct Corpses {
            sim::map_runtime::FeatureDefTable& features;
            std::vector<formats::tdf::OwnedDocument>& documents;
            std::string error;

            static int16_t load(void* context, const char* name) {
                auto& self = *static_cast<Corpses*>(context);
                const auto found = sim::map_runtime::find_or_load_feature(
                    self.features, self.documents, name, nullptr
                );
                if (found.ok())
                    return static_cast<int16_t>(found.index);
                self.error = std::string(name) + ": " + found.error->message;
                return -1;
            }
        } corpses{features, feature_docs, {}};

        const data::defs::UnitDefLoadHost corpse_host{&corpses, Corpses::load};
        const data::defs::UnitHeaderSources header_sources{"", &weapon_files, 3, 1, false, false};
        const data::defs::UnitDefSources unit_sources{
            "",
            movement.get(),
            weapons.records(),
            &sounds,
            &tables.categories,
            &tables.blocks,
            &corpse_host
        };
        units.resize(1);
        Reader reader(store);
        uint16_t type = 0;
        for (const char* name : {"ARMCOM", "ARMSOLAR"}) {
            auto& record = tables.records[++type];
            record.type_id = type;
            char fbi[data::defs::path_capacity];
            data::defs::build_variant_path(&files, fbi, sizeof fbi, "units", name, "FBI", nullptr);
            bool refused = false;
            if (!data::defs::load_unit_header(&files, fbi, record, header_sources, &refused) ||
                !data::defs::load_unit_def(&files, fbi, record, unit_sources) ||
                !corpses.error.empty())
                throw std::runtime_error(std::string("fbi ") + fbi + " " + corpses.error);
            LoadedUnit unit;
            unit.def = data::unit_definitions::unit_definition_from(
                record, {movement.get(), weapons.records(), &sounds, &tables.categories}
            );
            auto meta =
                data::unit_definitions::resolve_runtime_metadata(record, *movement, tables.blocks);
            if (!meta)
                throw std::runtime_error(
                    std::string("metadata ") + fbi + ": " + meta.error.message
                );
            unit.meta = meta.value;
            sim::unit_spawn::RuntimeBindings bindings;
            bindings.enabled = true;
            const auto binding = sim::combat_state::bind_unit_weapons(
                weapons, {unit.def.weapon1, unit.def.weapon2, unit.def.weapon3}
            );
            bindings.resolved_weapon_present = binding.resolved_nondefault_weapon;
            bindings.default_mission = static_cast<uint8_t>(record.default_mission_type);
            bindings.movement_footprint =
                std::array<int16_t, 2>{unit.meta.footprint_x, unit.meta.footprint_z};
            unit.loaded = sim::unit_spawn::load_runtime_type(unit.def, bindings, reader);
            unit.corpse = record.corpse;
            units.push_back(std::move(unit));
        }
        data::defs::sound_category_table_free(&sounds);
        data::defs::weapon_tdf_set_free(&weapon_files);
        data::defs::unit_def_tables_free(&tables);
        if (const auto error =
                sim::map_runtime::load_feature_links(features, feature_docs, nullptr))
            throw std::runtime_error("feature links: " + error->message);
        if (units[kSolarType].corpse < 0)
            throw std::runtime_error("ARMSOLAR leaves no corpse");
        solar_dead = static_cast<uint16_t>(units[kSolarType].corpse);
        solar_heap = features.defs[solar_dead].dead_feature;
        if (solar_heap >= features.defs.size() ||
            features.defs[solar_heap].dead_feature < features.defs.size())
            throw std::runtime_error(
                "the armsolar corpse chain is not a corpse, a heap and nothing"
            );
        if (weapons.find("ARM_DISINTEGRATOR") == nullptr)
            throw std::runtime_error("no ARM_DISINTEGRATOR");
    }

    int32_t solar_maximum_health() const {
        return units[kSolarType].loaded.type.simulation.maximum_health;
    }
};

// ------------------------------------------------------------------ the wire

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

// The records of one type among the datagrams a machine sent, unwrapped as
// the receiver's condenser and frame layer unwrap them.
std::vector<SentRecord> sent_of(const Inbox& inbox, RecordType type) {
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
            if (record_wire_length(frame.data() + at, size - at, &length) != WireError::ok ||
                length == 0)
                break;
            if (frame[at] == static_cast<uint8_t>(type))
                out.push_back(
                    {from, to, std::vector<uint8_t>(frame.data() + at, frame.data() + at + length)}
                );
            at += length;
        }
    }
    return out;
}

// The 0x0c record bytes a unit's death goes out as.
std::vector<uint8_t> killed_bytes(
    uint16_t unit,
    uint32_t attacker_owner_id,
    uint16_t attacker,
    uint8_t percent,
    uint8_t kind_and_wreck_level
) {
    return {
        0x0c,
        static_cast<uint8_t>(unit),
        static_cast<uint8_t>(unit >> 8),
        static_cast<uint8_t>(attacker_owner_id),
        static_cast<uint8_t>(attacker_owner_id >> 8),
        static_cast<uint8_t>(attacker_owner_id >> 16),
        static_cast<uint8_t>(attacker_owner_id >> 24),
        static_cast<uint8_t>(attacker),
        static_cast<uint8_t>(attacker >> 8),
        percent,
        kind_and_wreck_level
    };
}

// ------------------------------------------------------------------ machines

// One machine over the catalog: a flat 64-cell map, both unit types, and the
// network side, playing one of the two player slots.
struct Machine {
    const Catalog& catalog;
    formats::tnt::Map map;
    std::vector<sim::visibility_state::TerrainCell> terrain_values;
    std::array<sim::visibility_state::SightMask, 1> masks{};
    std::vector<sim::unit_spawn::LoadedType> loaded;
    std::vector<sim::unit_spawn::Type> types;
    std::vector<data::unit_definitions::UnitDefinition> defs;
    std::vector<data::unit_definitions::RuntimeDefinitionMetadata> metas;
    std::vector<sim::match_runtime::RuntimeTypeFields> fields;
    std::vector<sim::spatial_state::Plot> collision_plots;
    std::vector<FeatureDef> feature_defs;
    Services services;
    Scenario scenario;
    std::unique_ptr<sim::match_runtime::Match> match;
    Inbox inbox;
    NetConnection connection{};
    std::unique_ptr<NetMatch> net = std::make_unique<NetMatch>();
    MatchBinding binding;

    explicit Machine(const Catalog& c)
        : catalog(c), terrain_values(std::size_t{kMapCells} * kMapCells), loaded(c.units.size()),
          types(c.units.size()), defs(c.units.size()), metas(c.units.size()),
          fields(c.units.size()), collision_plots(terrain_values.size()),
          feature_defs(c.features.defs) {
        map.attribute_width = map.attribute_height = kMapCells;
        map.attributes.resize(terrain_values.size());
        masks[0].width = masks[0].height = 1;
        masks[0].pixels = {1};
        for (std::size_t i = 1; i < c.units.size(); ++i) {
            loaded[i] = c.units[i].loaded;
            types[i] = c.units[i].loaded.type;
            defs[i] = c.units[i].def;
            metas[i] = c.units[i].meta;
        }
        for (std::size_t i = 1; i < c.units.size(); ++i) {
            auto& f = fields[i];
            f.definition = &defs[i];
            f.yard_mask = metas[i].yard_cells;
            f.runtime_metadata = &metas[i];
            f.target_masks = &c.target_masks;
            f.movement_class =
                metas[i].movement_class_handle
                    ? static_cast<sim::unit_spawn::AssetHandle>(*metas[i].movement_class_handle) +
                          1U
                    : sim::unit_spawn::AssetHandle{0};
            f.corpse_feature = c.units[i].corpse;
        }
    }

    void start(uint8_t local_slot, uint16_t units_per_player) {
        const int32_t sight_cells = kMapCells / 2;
        sim::match_runtime::OfflineInputs input{
            map,
            loaded,
            types,
            fields,
            catalog.weapons,
            terrain_values,
            masks,
            sight_cells,
            sight_cells,
            units_per_player,
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
        match = std::make_unique<sim::match_runtime::Match>(input, services);
        match->configure_strategic_environment({0, 0.5F, 0});
        auto& world = match->state();
        world.game.map_pixel_width = world.game.map_pixel_height = kMapCells * 16;
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

    World& world() { return match->state(); }

    Unit& unit(uint16_t index) { return world().units[index]; }

    uint32_t tick() { return world().game.tick; }

    bool live(uint16_t index) { return (unit(index).flags & OA_UNIT_FLAG_LIVE) != 0; }

    // The EXPLODEs one unit's scripts ran here, in order.
    std::vector<PieceExplosion> explosions_of(uint16_t index) const {
        std::vector<PieceExplosion> out;
        for (const auto& explosion : services.explosions)
            if (explosion.unit == index)
                out.push_back(explosion);
        return out;
    }

    // How many plots under a solar's footprint around a world point hold a feature.
    uint32_t features_around(int32_t x, int32_t z) {
        uint32_t found = 0;
        const int32_t cell_x = x / 16;
        const int32_t cell_z = z / 16;
        for (int32_t row = cell_z - kFootprintReach; row <= cell_z + kFootprintReach; ++row)
            for (int32_t column = cell_x - kFootprintReach; column <= cell_x + kFootprintReach;
                 ++column)
                if (const auto* plot = world_plot(&world(), column, row);
                    plot != nullptr && plot->feature < world().feature_def_count)
                    ++found;
        return found;
    }

    // The feature word of a plot, where a corpse lies on its unit's cell.
    uint16_t feature_on(std::array<int32_t, 2> cell) {
        const auto* plot = world_plot(&world(), cell[0], cell[1]);
        return plot != nullptr ? plot->feature : sim::feature_runtime::no_feature;
    }

    // The cell a unit stands on, where its corpse will lie.
    std::array<int32_t, 2> cell_of(uint16_t index) {
        return {unit(index).cell_x, unit(index).cell_z};
    }
};

void step(Machine& m) {
    try {
        match_binding_tick(&m.binding);
    } catch (const std::exception& error) {
        ++failures;
        std::fprintf(stderr, "tick %u: %s\n", m.tick(), error.what());
    }
    packet_layer_flush(m.connection.packets, 0, true);
}

void fund(Machine& m, uint8_t slot) {
    auto& player = m.world().game.players[slot];
    player.energy = player.energy_storage = kFunds;
    player.metal = player.metal_storage = kFunds;
}

// The host's commander and a row of the joiner's solars, each with its copy
// on the other machine once a full-record cycle has passed.
struct Scene {
    Machine host;
    Machine joiner;
    uint16_t commander{};
    std::vector<uint16_t> solars;

    Scene(const Catalog& catalog, uint16_t per_player, std::size_t solar_count)
        : host(catalog), joiner(catalog) {
        host.inbox.peer = &joiner.inbox;
        joiner.inbox.peer = &host.inbox;
        host.start(kHostSlot, per_player);
        joiner.start(kJoinerSlot, per_player);
        auto* placed = spawn(host, kHostSlot, kCommanderType, kCommanderX, kRowZ);
        if (placed == nullptr)
            throw std::runtime_error("the commander was not placed");
        commander = placed->unit_index;
        // Hold fire, and the laser (weapon slot 0) off, so only the D-gun kills.
        placed->unit->flags &= ~OA_UNIT_FLAG_FIRE_ORDER_MASK;
        placed->record.weapons[0].flags =
            static_cast<uint8_t>(placed->record.weapons[0].flags & ~OA_UNIT_WEAPON_ENABLED);
        for (std::size_t i = 0; i < solar_count; ++i) {
            auto* solar = spawn(joiner, kJoinerSlot, kSolarType, kSolarX[i], kRowZ);
            if (solar == nullptr)
                throw std::runtime_error("a solar was not placed");
            solars.push_back(solar->unit_index);
        }
        const uint32_t warm = std::max<uint32_t>(70u, per_player + 20u);
        for (uint32_t t = 0; t < warm; ++t)
            both();
        for (const auto solar : solars)
            CHECK(host.live(solar) && host.unit(solar).type_index == kSolarType);
        CHECK(joiner.live(commander));
        forget();
    }

    static sim::unit_spawn::Slot*
    spawn(Machine& m, uint8_t player, uint16_t type, int32_t x, int32_t z) {
        return m.match->create(
            {player,
             type,
             {static_cast<uint32_t>(x) << 16, 0, static_cast<uint32_t>(z) << 16},
             true,
             1,
             0}
        );
    }

    void both() {
        fund(host, kHostSlot);
        step(host);
        step(joiner);
    }

    // Starts the checks' record: nothing sent, nothing exploded.
    void forget() {
        host.inbox.sent.clear();
        joiner.inbox.sent.clear();
        host.services.explosions.clear();
        joiner.services.explosions.clear();
    }

    // The 0x0c bytes of a unit's death when the commander last damaged it.
    std::vector<uint8_t>
    killed(uint16_t unit, uint8_t percent, uint8_t kind_and_wreck_level) const {
        return killed_bytes(unit, kIds[kHostSlot], commander, percent, kind_and_wreck_level);
    }
};

// The packed kind and wreck level byte of a weapon death's 0x0c record.
constexpr uint8_t weapon_death(uint8_t wreck_level) {
    return static_cast<uint8_t>((kWeaponKind << unit_killed_kind_shift) | wreck_level);
}

// ------------------------------------------------------------------ cases

// The host's commander D-guns the joiner's first solar; the ball carries on
// through the second. The joiner kills each at percentage 100, where the
// solar's Killed script picks wreck level 3 (past the heap), and sends one
// 0x0c per solar from its player id to every player. The host's copies die
// with that outcome on the next tick: the same Killed pieces, no wreck, and
// the kill credited as on the joiner. The third solar survives with the same
// health on both machines, and a further full-record cycle changes nothing.
void dgun_leaves_no_wreck_on_either_machine(const Catalog& catalog, uint16_t units_per_player) {
    Scene scene(catalog, units_per_player, kSolarX.size());
    auto& host = scene.host;
    auto& joiner = scene.joiner;
    const auto first = scene.solars[0];
    const auto& target = host.unit(first).position;
    (void)host.match->issue_attack_special(
        scene.commander, {target.x, target.y, target.z}, false, first
    );
    std::array<uint32_t, 2> joiner_death{};
    std::array<uint32_t, 2> host_death{};
    const auto all_gone = [&] {
        return std::all_of(host_death.begin(), host_death.end(), [](uint32_t tick) {
            return tick != 0;
        });
    };
    for (uint32_t t = 0; t < 600 && !all_gone(); ++t) {
        scene.both();
        for (std::size_t i = 0; i < joiner_death.size(); ++i) {
            const auto solar = scene.solars[i];
            if (joiner_death[i] == 0 && !joiner.live(solar))
                joiner_death[i] = joiner.tick();
            if (host_death[i] == 0 && !host.live(solar))
                host_death[i] = host.tick();
        }
    }
    CHECK(all_gone());
    for (std::size_t i = 0; i < joiner_death.size(); ++i) {
        const auto solar = scene.solars[i];
        CHECK(
            joiner_death[i] != 0 && host_death[i] >= joiner_death[i] &&
            host_death[i] - joiner_death[i] <= 2
        );
        CHECK(
            joiner.features_around(kSolarX[i], kRowZ) == 0 &&
            host.features_around(kSolarX[i], kRowZ) == 0
        );
        const auto owner_pieces = joiner.explosions_of(solar);
        CHECK(!owner_pieces.empty() && owner_pieces == host.explosions_of(solar));
    }
    CHECK(joiner.match->wrecks().empty() && host.match->wrecks().empty());

    // One 0x0c per dead solar, and none from the machine that does not
    // simulate them.
    const auto kills = sent_of(joiner.inbox, RecordType::unit_killed);
    CHECK(kills.size() == 2);
    for (std::size_t i = 0; i < kills.size() && i < 2; ++i) {
        CHECK(kills[i].from == kIds[kJoinerSlot] && kills[i].to == broadcast_destination_id);
        CHECK(kills[i].bytes == scene.killed(scene.solars[i], 100, weapon_death(3)));
    }
    CHECK(sent_of(host.inbox, RecordType::unit_killed).empty());

    for (auto* m : {&host, &joiner}) {
        const auto& players = m->world().game.players;
        CHECK(players[kHostSlot].kills == 2 && players[kJoinerSlot].losses == 2);
        CHECK(m->unit(scene.commander).veteran_level == 2);
    }

    // The survivor's copy follows its owner's full record.
    const auto survivor = scene.solars[2];
    bool agreed = false;
    for (uint32_t t = 0; t < units_per_player + 20u && !agreed; ++t) {
        scene.both();
        agreed = host.unit(survivor).health == joiner.unit(survivor).health;
    }
    CHECK(agreed && host.live(survivor) && joiner.live(survivor));
    const auto health = joiner.unit(survivor).health;
    for (uint32_t t = 0; t < units_per_player + 10u; ++t)
        scene.both();
    CHECK(host.unit(survivor).health == health && joiner.unit(survivor).health == health);
    CHECK(joiner.match->wrecks().empty() && host.match->wrecks().empty());
    for (std::size_t i = 0; i < 2; ++i) {
        CHECK(!host.live(scene.solars[i]) && !joiner.live(scene.solars[i]));
        CHECK(
            joiner.features_around(kSolarX[i], kRowZ) == 0 &&
            host.features_around(kSolarX[i], kRowZ) == 0
        );
    }
    CHECK(sent_of(joiner.inbox, RecordType::unit_killed).size() == 2);
    CHECK(host.net->record_errors == 0 && joiner.net->record_errors == 0);
}

// A solar left at 5 health for two 30-tick samples takes a 25-point hit from
// the host's commander (a 0x0b). The joiner kills it at percentage
// (20 * 100 / 326 + 1) / 2 = 3, where Killed picks the plain corpse, and
// shares that; both machines then show armsolar_dead on the solar's cell,
// although the host's copy never went below 5 health.
void low_severity_kill_leaves_the_same_wreck(const Catalog& catalog, uint16_t units_per_player) {
    Scene scene(catalog, units_per_player, 1);
    auto& host = scene.host;
    auto& joiner = scene.joiner;
    const auto solar = scene.solars[0];
    const auto cell = joiner.cell_of(solar);
    CHECK(host.cell_of(solar) == cell);
    joiner.unit(solar).health = 5;
    for (uint32_t t = 0; t < 60; ++t)
        scene.both();
    CHECK(joiner.unit(solar).health == 5);
    CHECK(
        joiner.unit(solar).health_percent == 1 && joiner.unit(solar).previous_health_percent == 1
    );
    scene.forget();
    UnitDamageRecord hit{};
    hit.target_unit_index = solar;
    hit.source_unit_index = scene.commander;
    hit.amount = 25;
    hit.kind = kWeaponKind;
    net_match_send_damage(host.net.get(), kIds[kHostSlot], hit);
    packet_layer_flush(host.connection.packets, 0, true);
    uint32_t joiner_death = 0;
    uint32_t host_death = 0;
    for (uint32_t t = 0; t < 10 && host_death == 0; ++t) {
        step(joiner);
        if (joiner_death == 0 && !joiner.live(solar))
            joiner_death = joiner.tick();
        fund(host, kHostSlot);
        step(host);
        if (joiner_death != 0 && !host.live(solar))
            host_death = host.tick();
    }
    CHECK(joiner_death != 0 && host_death != 0 && host_death - joiner_death <= 2);
    const auto percent = static_cast<uint8_t>((20 * 100 / catalog.solar_maximum_health() + 1) / 2);
    CHECK(percent == 3);
    const auto kills = sent_of(joiner.inbox, RecordType::unit_killed);
    CHECK(kills.size() == 1 && kills[0].bytes == scene.killed(solar, percent, weapon_death(1)));
    for (auto* m : {&host, &joiner}) {
        CHECK(m->match->wrecks().size() == 1);
        CHECK(m->feature_on(cell) == catalog.solar_dead);
        if (!m->match->wrecks().empty()) {
            const auto& wreck = m->match->wrecks()[0];
            CHECK(
                wreck.feature == catalog.solar_dead && wreck.cell_x == cell[0] &&
                wreck.cell_z == cell[1]
            );
        }
    }
    CHECK(
        !joiner.explosions_of(solar).empty() &&
        joiner.explosions_of(solar) == host.explosions_of(solar)
    );
    CHECK(sent_of(host.inbox, RecordType::unit_killed).empty());
}

// The joiner sends the 0x0c a weapon kill at percentage 20 would, wreck
// level 1, for a solar it still has; the host applies it as sent: the
// solar's Killed script plays its pieces for 20, armsolar_dead lies on its
// cell, and the host's player gains the kill, the joiner's the loss and the
// commander a veteran level.
void received_record_is_applied_as_sent(const Catalog& catalog, uint16_t units_per_player) {
    Scene scene(catalog, units_per_player, 1);
    auto& host = scene.host;
    auto& joiner = scene.joiner;
    const auto solar = scene.solars[0];
    const auto cell = host.cell_of(solar);
    constexpr uint8_t percent = 20;
    // The pieces the solar's own Killed script throws at 20.
    auto* instance = joiner.match->instance(solar);
    CHECK(instance != nullptr && instance->script() != nullptr);
    if (instance == nullptr || instance->script() == nullptr)
        return;
    std::array<int32_t, 4> args{percent, 0, 0, 0};
    CHECK(instance->script()->query("Killed", args));
    CHECK(args[1] == 1);
    std::vector<PieceExplosion> expected = joiner.explosions_of(solar);
    CHECK(expected.size() == 5);
    const auto& players = host.world().game.players;
    const auto kills = players[kHostSlot].kills;
    const auto losses = players[kJoinerSlot].losses;
    const auto veteran = host.unit(scene.commander).veteran_level;
    const auto record = scene.killed(solar, percent, weapon_death(1));
    CHECK(net_match_send(
        joiner.net.get(), kIds[kJoinerSlot], broadcast_destination_id, record.data(), record.size()
    ));
    packet_layer_flush(joiner.connection.packets, 0, true);
    (void)net_match_pump(host.net.get());
    CHECK(!host.live(solar));
    CHECK(host.feature_on(cell) == catalog.solar_dead);
    CHECK(
        host.match->wrecks().size() == 1 && host.match->wrecks()[0].feature == catalog.solar_dead
    );
    CHECK(host.explosions_of(solar) == expected);
    CHECK(players[kHostSlot].kills == kills + 1 && players[kJoinerSlot].losses == losses + 1);
    CHECK(host.unit(scene.commander).veteran_level == veteran + 1);
    CHECK(host.net->record_errors == 0);
}

// A 0x09 for a slot whose copy is still there at 0 health, its previous
// 30-tick health percentage 100: the copy dies first by its own health, at
// (0 + 100) / 2 = 50, where Killed picks the heap, and then the new solar
// takes the slot. The copy is simulated elsewhere, so no 0x0c goes out.
void slot_reuse_kills_the_stale_copy(const Catalog& catalog, uint16_t units_per_player) {
    Scene scene(catalog, units_per_player, 1);
    auto& host = scene.host;
    const auto solar = scene.solars[0];
    auto& copy = host.unit(solar);
    const auto cell = host.cell_of(solar);
    copy.health = 0;
    copy.health_percent = 100;
    copy.previous_health_percent = 100;
    const auto created = host.binding.created_remote;
    UnitCreatedRecord record{};
    record.unit_def_index = kSolarType;
    record.unit_index = solar;
    record.position[0] = kSolarX[0] << 16;
    record.position[2] = (kRowZ + 192) << 16;
    auto sim = match_binding_sim(&host.binding);
    sim.create_unit(sim.context, &host.world(), kJoinerSlot, record);
    CHECK(host.feature_on(cell) == catalog.solar_heap);
    CHECK(
        host.match->wrecks().size() == 1 && host.match->wrecks()[0].feature == catalog.solar_heap
    );
    CHECK(!host.explosions_of(solar).empty());
    CHECK(host.binding.created_remote == created + 1);
    CHECK(
        host.live(solar) && copy.type_index == kSolarType && (copy.position.z >> 16) == kRowZ + 192
    );
    packet_layer_flush(host.connection.packets, 0, true);
    CHECK(sent_of(host.inbox, RecordType::unit_killed).empty());
}

} // namespace

int main() {
    try {
        const Catalog catalog;
        // At 250 units a player the commander is unit 251; at 10, unit 11.
        for (const uint16_t units_per_player : {uint16_t{250}, uint16_t{10}}) {
            const auto before = failures;
            dgun_leaves_no_wreck_on_either_machine(catalog, units_per_player);
            low_severity_kill_leaves_the_same_wreck(catalog, units_per_player);
            received_record_is_applied_as_sent(catalog, units_per_player);
            slot_reuse_kills_the_stale_copy(catalog, units_per_player);
            if (failures != before)
                std::fprintf(
                    stderr,
                    "at %u units a player: %d failure(s)\n",
                    units_per_player,
                    failures - before
                );
        }
    } catch (const std::exception& error) {
        std::fprintf(stderr, "D-gun kill: %s\n", error.what());
        return 1;
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("D-gun kill shared between two machines: all tests passed");
    return 0;
}
