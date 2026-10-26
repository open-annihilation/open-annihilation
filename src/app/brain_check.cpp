// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The check match: the mounted game's definitions, a flat stand-in map, and
// one seated brain. The match is never ticked.

#include "oa/app/brain_check.hpp"

#include "oa/app/game_directory.hpp"

#include "oa/core/player.h"
#include "oa/core/side.h"
#include "oa/data/brain_facts.hpp"
#include "oa/data/limits.hpp"
#include "oa/data/defs/asset_files.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/defs/move_classes.hpp"
#include "oa/data/defs/sides.hpp"
#include "oa/data/defs/sound_categories.hpp"
#include "oa/data/defs/unit_catalog.hpp"
#include "oa/data/defs/unit_def_loader.hpp"
#include "oa/data/defs/unit_header.hpp"
#include "oa/data/defs/unit_records.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/data/unit_definitions.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/tnt.hpp"
#include "oa/sim/ai.hpp"
#include "oa/sim/combat_state.hpp"
#include "oa/sim/map_runtime.hpp"
#include "oa/sim/map_runtime/feature_defs.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/scenario/state.hpp"
#include "oa/sim/spatial_state/spatial.hpp"
#include "oa/sim/unit_spawn/spawn.hpp"
#include "oa/sim/unit_spawn/spawn_runtime.hpp"
#include "oa/sim/visibility_state.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef OA_ENGINE_VERSION
#define OA_ENGINE_VERSION "0.0.0"
#endif

namespace oa::app {
namespace {

namespace defs = oa::data::defs;
namespace facts = oa::data::brain_facts;
namespace oamut = oa::formats::oamut;
namespace script = oa::data::script_host;

/// The brain's seat. Player 0 is the local player and holds no units.
constexpr uint8_t brain_player = 1;
/// Cells on each side of the stand-in map. Each cell is 16 world units.
constexpr int32_t map_cells = 64;
/// World units from the map's origin to its centre.
constexpr uint32_t map_centre = 512u << 16;
/// The base game a works-with entry names when no mod is mounted.
constexpr std::string_view base_game_id = "ta-3.1c";
constexpr std::string_view base_game_version = "3.1c";

/// Puts the previous data layout back when the check returns.
struct LayoutRestore {
    defs::DataLayout previous{};
    bool armed = false;

    LayoutRestore() = default;
    LayoutRestore(const LayoutRestore&) = delete;
    LayoutRestore& operator=(const LayoutRestore&) = delete;

    /// Remembers the layout in use. Call it before replacing that layout.
    void arm() {
        previous = defs::data_layout();
        armed = true;
    }

    ~LayoutRestore() {
        if (armed)
            defs::use_data_layout(previous);
    }
};

/// Services the check match asks nothing visible of.
struct QuietServices : sim::match_runtime::OfflineServices {
    /// Plays no command speech.
    void command_sound(sim::unit_spawn::Slot&, uint32_t) override {}

    /// Plays no activation sound.
    void activation_sound(sim::unit_spawn::Slot&, sim::unit_activation::Sound) override {}

    /// Tells no attachment interface.
    void attachment_notification(sim::unit_spawn::Slot&, uint32_t) override {}

    /// Redraws no order panel.
    void refresh_selected_unit(sim::unit_spawn::Slot&) override {}

    /// Shows no EMIT-SFX effect.
    void emit_sfx(sim::unit_spawn::Slot&, uint32_t, int32_t) override {}

    /// Explodes no piece.
    void explode_piece(sim::unit_spawn::Slot&, uint32_t, int32_t) override {}

    /// Attaches no unit.
    void attach_unit(sim::unit_spawn::Slot&, int32_t, int32_t, int32_t) override {}

    /// Drops no unit.
    void drop_unit(sim::unit_spawn::Slot&, int32_t) override {}

    /// Recomputes no plot heights.
    void refresh_plot_height_range(std::array<int16_t, 2>, std::array<int16_t, 2>) override {}

    /// Refreshes no movement map.
    void notify_object_footprint_removed(sim::spatial_state::Unit&, uint32_t) override {}

    /// Refreshes no movement map.
    void notify_footprint_changed(std::array<int16_t, 2>, std::array<int16_t, 2>) override {}
};

/// Scenario definitions with no keys.
struct EmptyScenario : sim::scenario::DefinitionHost {
    /// Reads no key.
    ///
    /// @return the fallback
    int32_t integer(std::string_view, int32_t fallback) override { return fallback; }

    /// Reads no key.
    ///
    /// @return nothing
    std::optional<std::string> text(std::string_view) override { return std::nullopt; }
};

/// Reads unit models and scripts from the mounted store.
class StoreReader final : public sim::unit_spawn::AssetReader {
  public:

    /// Binds the reader to a mounted store.
    ///
    /// @param store the store; it must outlive the reader
    explicit StoreReader(const AssetStore& store) : store_(store) {}

    /// Reads one file.
    ///
    /// @param path path inside the game data
    /// @return the bytes, or nothing when the file is missing or empty
    std::optional<std::vector<uint8_t>> read(std::string_view path) override {
        return store_.load_file_contents(path);
    }

  private:

    const AssetStore& store_;
};

/// Lists and reads feature documents from the mounted store.
class StoreFeatureReader final : public sim::map_runtime::FeatureAssetReader {
  public:

    /// Binds the reader to a mounted store.
    ///
    /// @param store the store; it must outlive the reader
    explicit StoreFeatureReader(const AssetStore& store) : store_(store) {}

    /// Lists every effective file under a directory.
    ///
    /// @param directory logical directory
    /// @param extension extension kept
    /// @return the logical paths
    data::unit_definitions::Result<std::vector<std::string>> list_effective_recursive(
        std::string_view directory, std::string_view extension
    ) const override {
        return {store_.list_effective_recursive(directory, extension), {}};
    }

    /// Reads a file's text.
    ///
    /// @param path logical path
    /// @return the text, or an error when the file is missing
    data::unit_definitions::Result<std::string> read(std::string_view path) const override {
        const std::optional<std::vector<uint8_t>> bytes = store_.load_file_contents(path);
        if (!bytes) {
            return {
                {}, {data::unit_definitions::ErrorCode::io, 0, std::string(path) + " does not load"}
            };
        }
        return {std::string(bytes->begin(), bytes->end()), {}};
    }

  private:

    const AssetStore& store_;
};

/// Unit tables released once the match no longer needs them.
struct UnitTables {
    defs::SoundCategoryTable sounds{};
    defs::WeaponTdfSet weapon_files{};
    defs::UnitDefTables units{};

    /// Empties the tables.
    UnitTables() {
        defs::weapon_tdf_set_init(&weapon_files);
        defs::unit_def_tables_init(&units);
    }

    /// Frees what the tables hold.
    ~UnitTables() {
        defs::sound_category_table_free(&sounds);
        defs::weapon_tdf_set_free(&weapon_files);
        defs::unit_def_tables_free(&units);
    }

    UnitTables(const UnitTables&) = delete;
    UnitTables& operator=(const UnitTables&) = delete;
};

/// Loads a corpse feature while a unit file is read.
struct Corpses {
    sim::map_runtime::FeatureDefTable& features;
    std::vector<formats::tdf::OwnedDocument>& documents;
    std::string failure;

    /// Finds or loads a corpse (UnitDefLoadHost::corpse).
    ///
    /// @param context the Corpses
    /// @param name feature name
    /// @return its index, or -1 when it does not load
    static int16_t load(void* context, const char* name) {
        auto& self = *static_cast<Corpses*>(context);
        const auto found =
            sim::map_runtime::find_or_load_feature(self.features, self.documents, name, nullptr);
        if (!found.ok()) {
            self.failure = found.error->message;
            return -1;
        }
        return static_cast<int16_t>(found.index);
    }
};

/// A flat land map: 64 by 64 cells, sea level 0, no features and no metal.
struct FlatMap {
    formats::tnt::Map map;
    std::vector<sim::visibility_state::TerrainCell> terrain;
    std::vector<sim::spatial_state::Plot> plots;
    std::array<sim::visibility_state::SightMask, 1> masks{};

    /// Builds the map.
    FlatMap() {
        map.attribute_width = static_cast<uint32_t>(map_cells);
        map.attribute_height = static_cast<uint32_t>(map_cells);
        map.sea_level = 0;
        const auto cells =
            static_cast<std::size_t>(map_cells) * static_cast<std::size_t>(map_cells);
        map.attributes.resize(cells);
        plots.resize(cells);
        terrain.resize(cells);
        for (std::size_t index = 0; index < cells; ++index) {
            map.attributes[index].height = 1;
            plots[index].low_height = 1;
            plots[index].high_height = 1;
        }
        masks[0].width = masks[0].height = 15;
        masks[0].offset_x = masks[0].offset_z = 7;
        masks[0].pixels.assign(15 * 15, 1);
    }
};

/// Bytes of a C string held in a fixed buffer.
///
/// @param bytes the buffer
/// @param room its size
/// @return the text up to the first NUL
std::string text_of(const char* bytes, std::size_t room) {
    std::size_t size = 0;
    while (size < room && bytes[size] != '\0')
        ++size;
    return std::string(bytes, size);
}

/// The seating reason, cut where the seat cut it.
///
/// @param reason the seat's reason buffer
/// @return its text
std::string reason_text(const std::array<char, 256>& reason) {
    return text_of(reason.data(), reason.size());
}

/// Bytes of the network files the manifest names.
///
/// @param brain the brain
/// @return the sizes added together
std::size_t network_file_bytes(const InstalledBrain& brain) {
    std::size_t total = 0;
    for (const auto& network : brain.manifest.networks) {
        for (const auto& file : brain.files) {
            if (file.first == network.file) {
                total += file.second.size();
                break;
            }
        }
    }
    return total;
}

/// One works-with entry as a report names it.
///
/// @param condition the entry
/// @return the id, or the id and its range
std::string condition_text(const oamut::Condition& condition) {
    if (condition.range_text.empty())
        return condition.id;
    return condition.id + " " + condition.range_text;
}

/// Joins conditions the way a does-not-fit line names them.
///
/// @param conditions the entries
/// @return the joined text, or `nothing` when there are none
std::string joined_conditions(const std::vector<oamut::Condition>& conditions) {
    std::string text;
    for (const oamut::Condition& condition : conditions) {
        if (!text.empty())
            text += ", ";
        text += condition_text(condition);
    }
    if (text.empty())
        text = "nothing";
    return text;
}

/// Joins expansion ids.
///
/// @param ids the ids
/// @return the joined text
std::string joined_ids(const std::vector<std::string>& ids) {
    std::string text;
    for (const std::string& id : ids) {
        if (!text.empty())
            text += ", ";
        text += id;
    }
    return text;
}

/// Tells whether one works-with entry holds this game.
///
/// `*` matches any mounted mod and the base game played alone, as the
/// works-with grammar has it.
///
/// @param condition the entry
/// @param request the check
/// @param profile the mounted mod, or null for the base game
/// @return true when the entry holds
bool condition_holds(
    const oamut::Condition& condition,
    const BrainCheckRequest& request,
    const data::mod_profile::ModProfile* profile
) {
    if (condition.id == "*")
        return true;
    if (request.mod_folder.empty())
        return oamut::condition_met(condition, base_game_id, base_game_version);
    return oamut::condition_met(condition, profile->id, profile->version);
}

/// The manifest's own fit, before any Lua. Empty when the brain fits.
///
/// @param request the check
/// @param profile the mounted mod, or null for the base game
/// @return the reason it does not fit, without the `does not fit: ` prefix
std::string
manifest_fit(const BrainCheckRequest& request, const data::mod_profile::ModProfile* profile) {
    const std::string game =
        request.mod_folder.empty()
            ? std::string(base_game_id)
            : (profile->version.empty() ? profile->id : profile->id + " " + profile->version);
    bool holds = false;
    for (const oamut::Condition& condition : request.brain.manifest.works_with) {
        if (condition_holds(condition, request, profile)) {
            holds = true;
            break;
        }
    }
    if (!holds) {
        return "works-with names " + joined_conditions(request.brain.manifest.works_with) +
               "; this game is " + game;
    }
    if (!request.brain.manifest.requires_expansions.empty()) {
        return "requires.expansions names " +
               joined_ids(request.brain.manifest.requires_expansions) + "; this game has none";
    }
    return {};
}

/// Fills the three phase lines from a seating.
///
/// A phase that was not entered is `not run`, including load. A short phase
/// that ran can still report 0 instructions, so the ran flags decide.
///
/// @param outcome the seating
/// @param[out] report the report being filled
void fill_phases(const sim::brains::SeatOutcome& outcome, BrainCheckReport& report) {
    report.load.instructions = outcome.load_instructions;
    report.fits.instructions = outcome.fits_instructions;
    report.start.instructions = outcome.start_instructions;
    const std::string reason = reason_text(outcome.reason);

    if (!outcome.load_ran) {
        report.load.result = "not run";
        report.fits.result = "not run";
        report.start.result = "not run";
    } else if (
        outcome.result == sim::brains::SeatResult::failed && !outcome.fits_ran && !outcome.start_ran
    ) {
        report.load.result = "failed";
        report.fits.result = "not run";
        report.start.result = "not run";
    } else {
        report.load.result = "ok";
        if (!outcome.fits_ran) {
            report.fits.result = "none";
        } else if (outcome.result == sim::brains::SeatResult::does_not_fit) {
            report.fits.text = reason;
            report.fits.result = "does not fit: " + reason;
        } else if (outcome.result == sim::brains::SeatResult::failed && !outcome.start_ran) {
            report.fits.result = "failed";
        } else {
            report.fits.result = "ok";
        }

        if (!outcome.start_ran)
            report.start.result =
                outcome.result == sim::brains::SeatResult::seated ? "none" : "not run";
        else if (outcome.result == sim::brains::SeatResult::failed)
            report.start.result = "failed";
        else
            report.start.result = "ok";
    }

    report.seated = outcome.result == sim::brains::SeatResult::seated;
    if (outcome.result == sim::brains::SeatResult::does_not_fit) {
        report.error.clear();
    } else if (outcome.result == sim::brains::SeatResult::failed) {
        if (!outcome.error.message.empty() || !outcome.error.phase.empty())
            report.error = script::describe(outcome.error);
        else
            report.error = reason;
    } else if (outcome.result == sim::brains::SeatResult::refused) {
        report.error = reason;
    }
}

/// Reads one AI profile. Empty text when the file is missing.
///
/// @param store the mounted store
/// @param path the profile's path
/// @return the path and the text; the path is empty when nothing was read
facts::ProfileFile read_profile(const AssetStore& store, const std::string& path) {
    facts::ProfileFile file;
    const std::optional<std::vector<uint8_t>> bytes = store.load_file_contents(path);
    if (!bytes)
        return file;
    file.path = path;
    file.text.assign(bytes->begin(), bytes->end());
    return file;
}

/// The facts the brain reads. The base bundle is `totala`.
///
/// @param store the mounted store
/// @param profile the mounted mod, or null
/// @param sides the sides this match loaded
/// @param difficulty the chosen level's difficulty
/// @return the facts
facts::GameFacts make_facts(
    const AssetStore& store,
    const data::mod_profile::ModProfile* profile,
    const defs::SideTable& sides,
    int32_t difficulty
) {
    facts::GameFacts game;
    game.base = "totala";
    game.engine = OA_ENGINE_VERSION;
    game.mode = facts::GameMode::skirmish;
    game.difficulty = difficulty;
    game.mod = facts::mod_facts_of(profile);
    game.mod.sides.clear();
    game.mod.sides.reserve(sides.count);
    for (uint32_t index = 0; index < sides.count; ++index) {
        const Side& side = *defs::side_at(sides, index);
        facts::SideFacts stored;
        stored.name = text_of(side.name, sizeof side.name);
        stored.prefix = text_of(side.name_prefix, sizeof side.name_prefix);
        stored.commander = text_of(side.commander, sizeof side.commander);
        game.mod.sides.push_back(std::move(stored));
    }
    const std::string profile_path = defs::data_path(defs::DataDirectory::ai, "default.txt");
    game.ai_profile = read_profile(store, profile_path);
    const char* folder = defs::directory_name(defs::DataDirectory::ai);
    if (folder == nullptr)
        return game;
    const std::vector<std::string> names = store.list_effective(folder, ".txt");
    constexpr std::size_t profile_cap = 256;
    const std::size_t kept = names.size() < profile_cap ? names.size() : profile_cap;
    game.ai_profiles.reserve(kept);
    for (std::size_t index = 0; index < kept; ++index) {
        facts::ProfileFile stored = read_profile(store, names[index]);
        if (stored.path.empty())
            continue;
        game.ai_profiles.push_back(std::move(stored));
    }
    return game;
}

/// Sorts the kept unit records by name and numbers them from 0.
///
/// Unavailable records stay. A copyright the header refused is still a unit.
///
/// @param[in,out] tables the unit tables
void number_catalog(defs::UnitDefTables& tables) {
    defs::unit_defs_sort(
        tables.records + 1, tables.records + tables.count, defs::unit_def_name_less
    );
    for (uint32_t index = 0; index < tables.count; ++index)
        tables.records[index].type_id = static_cast<uint16_t>(index);
}

/// Drops records `drop` marks and closes the table over the ones that remain.
///
/// @param[in,out] tables the unit tables
/// @param drop one byte per record; nonzero records are dropped
void compact_catalog(defs::UnitDefTables& tables, const std::vector<char>& drop) {
    uint32_t write = 1;
    for (uint32_t read = 1; read < tables.count; ++read) {
        if (drop[read] != 0)
            continue;
        if (write != read)
            defs::unit_def_assign(&tables.records[write], &tables.records[read]);
        ++write;
    }
    tables.count = write;
}

/// Loads one unit's definition and runtime type into `index`.
///
/// @param files the mounted files
/// @param tables the unit tables
/// @param movement the movement classes
/// @param weapons the weapon registry
/// @param corpses corpse loading
/// @param sources the definition sources
/// @param definition_sources the typed-field sources
/// @param yard_maps which units get a yard map
/// @param reader the model reader
/// @param index the type index
/// @param[out] loaded the runtime type
/// @param[out] type the spawn type
/// @param[out] definition the typed fields
/// @param[out] metadata the runtime metadata
/// @param[out] masks the category masks
/// @param[out] field the match's field record
/// @param[out] warning set when the unit does not load
/// @return false when the unit does not load
bool load_one_unit(
    const defs::Files& files,
    UnitTables& tables,
    const defs::MoveClassTable& movement,
    sim::combat_state::WeaponRegistry& weapons,
    Corpses& corpses,
    const defs::UnitDefSources& sources,
    const data::unit_definitions::UnitDefinitionSources& definition_sources,
    const defs::YardMapRules& yard_maps,
    StoreReader& reader,
    uint32_t index,
    sim::unit_spawn::LoadedType& loaded,
    sim::unit_spawn::Type& type,
    data::unit_definitions::UnitDefinition& definition,
    data::unit_definitions::RuntimeDefinitionMetadata& metadata,
    data::unit_definitions::UnitTargetCategoryMasks& masks,
    sim::match_runtime::RuntimeTypeFields& field,
    std::string& warning
) {
    oa::UnitDef& record = tables.units.records[index];
    char fbi_path[defs::path_capacity];
    defs::build_variant_path(
        &files,
        fbi_path,
        sizeof fbi_path,
        defs::directory_name(defs::DataDirectory::units),
        record.unit_name,
        defs::unit_extension(),
        nullptr
    );
    const auto fail = [&](std::string text) {
        warning = std::move(text);
        return false;
    };
    corpses.failure.clear();
    try {
        if (!defs::load_unit_def(&files, fbi_path, record, sources))
            return fail(std::string(fbi_path) + " does not load");
        if (!corpses.failure.empty())
            return fail(std::string(fbi_path) + " does not load");
        auto made = data::unit_definitions::unit_definition_from(record, definition_sources);
        auto resolved = data::unit_definitions::resolve_runtime_metadata(
            record, movement, tables.units.blocks, yard_maps
        );
        if (!resolved)
            return fail(std::string(fbi_path) + " does not load");
        sim::unit_spawn::RuntimeBindings bindings;
        bindings.enabled = true;
        bindings.resolved_weapon_present = sim::combat_state::bind_unit_weapons(
                                               weapons, {made.weapon1, made.weapon2, made.weapon3}
        )
                                               .resolved_nondefault_weapon;
        bindings.default_mission = static_cast<uint8_t>(record.default_mission_type);
        bindings.movement_footprint =
            std::array<int16_t, 2>{resolved.value.footprint_x, resolved.value.footprint_z};
        auto runtime = sim::unit_spawn::load_runtime_type(made, bindings, reader);
        if (!runtime.load_error.empty())
            return fail(std::string(fbi_path) + " does not load");
        masks = data::unit_definitions::target_category_masks(record, tables.units.categories);
        definition = std::move(made);
        metadata = std::move(resolved.value);
        loaded = std::move(runtime);
        type = loaded.type;
        field.definition = &definition;
        field.yard_mask = metadata.yard_cells;
        field.runtime_metadata = &metadata;
        field.target_masks = &masks;
        field.movement_class =
            metadata.movement_class_handle
                ? static_cast<sim::unit_spawn::AssetHandle>(*metadata.movement_class_handle) + 1U
                : sim::unit_spawn::AssetHandle{0};
        field.corpse_feature = record.corpse;
        return true;
    } catch (const std::exception&) {
        return fail(std::string(fbi_path) + " does not load");
    }
}

/// Loads every unit file. A file that does not load is one warning.
///
/// @param store the mounted store
/// @param files the file boundary over `store`
/// @param profile the mounted mod, or null
/// @param[out] tables unit tables; count is the kept types including slot 0
/// @param[out] weapons the weapon registry
/// @param[out] features corpse features
/// @param[out] loaded runtime types, index 0 reserved
/// @param[out] types spawn types
/// @param[out] definitions typed fields
/// @param[out] metadata runtime metadata
/// @param[out] masks category masks
/// @param[out] fields match field records
/// @param[out] warnings one line per file that did not load
/// @param[out] problem set when the tables themselves cannot be loaded
/// @return false when `problem` was set
bool load_units(
    const AssetStore& store,
    const defs::Files& files,
    const data::mod_profile::ModProfile* profile,
    UnitTables& tables,
    sim::combat_state::WeaponRegistry& weapons,
    sim::map_runtime::FeatureDefTable& features,
    std::vector<sim::unit_spawn::LoadedType>& loaded,
    std::vector<sim::unit_spawn::Type>& types,
    std::vector<data::unit_definitions::UnitDefinition>& definitions,
    std::vector<data::unit_definitions::RuntimeDefinitionMetadata>& metadata,
    std::vector<data::unit_definitions::UnitTargetCategoryMasks>& masks,
    std::vector<sim::match_runtime::RuntimeTypeFields>& fields,
    std::vector<std::string>& warnings,
    std::string& problem
) {
    auto documents = sim::map_runtime::load_feature_documents(StoreFeatureReader(store));
    if (!documents) {
        problem = "feature documents: " + documents.error.message;
        return false;
    }
    const auto movement = std::make_unique<defs::MoveClassTable>();
    if (!defs::load_move_classes(&files, movement.get(), nullptr, nullptr)) {
        problem = "moveinfo.tdf does not load";
        return false;
    }
    if (!defs::load_sound_categories(&files, &tables.sounds, nullptr)) {
        problem = "sound.tdf does not load";
        return false;
    }
    if (!defs::load_weapon_tdf_set(&files, nullptr, false, &tables.weapon_files)) {
        problem = "the weapon files do not list";
        return false;
    }
    const uint32_t weapon_types = profile != nullptr ? profile->limits.weapon_types.types
                                                     : data::limits::Limits{}.weapon_types.types;
    static_cast<void>(sim::combat_state::install_weapon_files(weapons, files, false, weapon_types));

    const char* units_directory = defs::directory_name(defs::DataDirectory::units);
    std::vector<std::string> paths =
        store.list_effective(units_directory, defs::unit_file_suffix());
    std::sort(paths.begin(), paths.end());
    if (paths.empty()) {
        problem = "the units folder holds no unit";
        return false;
    }
    if (paths.size() + 1 > defs::max_unit_defs) {
        problem = "the unit catalog exceeds the unit table";
        return false;
    }
    if (!defs::unit_def_tables_allocate(&tables.units, static_cast<uint32_t>(paths.size() + 1))) {
        problem = "the unit table does not allocate";
        return false;
    }
    const uint32_t mask_types =
        profile != nullptr ? profile->limits.category_masks.types : data::limits::base_type_bits;
    if (!defs::category_registry_set_mask_types(&tables.units.categories, mask_types)) {
        problem = "the category masks cannot hold the game's unit types";
        return false;
    }

    const defs::UnitHeaderSources header_sources{
        "",
        &tables.weapon_files,
        defs::data_layout().build_version[0],
        defs::data_layout().build_version[1],
        false,
        false
    };
    uint32_t next = 1;
    for (const std::string& path : paths) {
        oa::UnitDef& record = tables.units.records[next];
        record = {};
        record.type_id = static_cast<uint16_t>(next);
        bool refused = false;
        if (!defs::load_unit_header(&files, path.c_str(), record, header_sources, &refused)) {
            warnings.push_back(path + " does not load");
            record = {};
            continue;
        }
        ++next;
    }
    tables.units.count = next;

    Corpses corpses{features, documents.value, {}};
    const defs::UnitDefLoadHost corpse_host{&corpses, Corpses::load};
    const defs::YardMapRules yard_maps =
        profile != nullptr ? defs::yard_map_rules(profile->rules.units) : defs::YardMapRules{};
    const defs::UnitDefSources unit_sources{
        "",
        movement.get(),
        weapons.records(),
        &tables.sounds,
        &tables.units.categories,
        &tables.units.blocks,
        &corpse_host,
        yard_maps
    };
    const data::unit_definitions::UnitDefinitionSources definition_sources{
        movement.get(), weapons.records(), &tables.sounds, &tables.units.categories
    };
    StoreReader reader(store);

    bool clean = false;
    for (int pass = 0; pass < 2 && !clean; ++pass) {
        number_catalog(tables.units);
        if (pass > 0)
            defs::category_registry_clear(&tables.units.categories);
        const uint32_t count = tables.units.count;
        loaded.assign(count, {});
        types.assign(count, {});
        definitions.assign(count, {});
        metadata.assign(count, {});
        masks.assign(count, {});
        fields.assign(count, {});
        std::vector<char> drop(count, 0);
        bool dropped = false;
        for (uint32_t index = 1; index < count; ++index) {
            std::string warning;
            if (!load_one_unit(
                    files,
                    tables,
                    *movement,
                    weapons,
                    corpses,
                    unit_sources,
                    definition_sources,
                    yard_maps,
                    reader,
                    index,
                    loaded[index],
                    types[index],
                    definitions[index],
                    metadata[index],
                    masks[index],
                    fields[index],
                    warning
                )) {
                if (pass == 0)
                    warnings.push_back(std::move(warning));
                drop[index] = 1;
                dropped = true;
            }
        }
        if (!dropped) {
            clean = true;
            break;
        }
        compact_catalog(tables.units, drop);
    }
    if (!clean) {
        problem = "a unit definition does not load";
        return false;
    }

    // Field pointers address the vectors, which are not grown again.
    for (std::size_t index = 1; index < fields.size(); ++index) {
        fields[index].definition = &definitions[index];
        fields[index].yard_mask = metadata[index].yard_cells;
        fields[index].runtime_metadata = &metadata[index];
        fields[index].target_masks = &masks[index];
    }

    if (!defs::load_build_lists(&files, nullptr, &tables.units) ||
        !defs::load_download_menu(&files, nullptr, &tables.units)) {
        problem = "the build lists do not load";
        return false;
    }
    if (const auto linked =
            sim::map_runtime::load_feature_links(features, documents.value, nullptr)) {
        problem = "feature links: " + linked->message;
        return false;
    }
    return true;
}

/// Packs every type's build list the way the game hands it to the computer players.
///
/// @param tables the unit tables, after the lists are loaded
/// @return a count then its ids, for each type from 0
std::vector<uint16_t> pack_build_lists(const defs::UnitDefTables& tables) {
    std::vector<uint16_t> lists;
    for (uint32_t type = 0; type < tables.count; ++type) {
        const oa::UnitDef& unit = tables.records[type];
        const uint16_t* ids = defs::unit_def_build_ids(&tables, unit);
        const uint32_t count = ids != nullptr ? unit.build_id_count : 0;
        lists.push_back(static_cast<uint16_t>(count));
        lists.insert(lists.end(), ids, ids + count);
    }
    return lists;
}

/// The check's report for a brain that does not fit, with nothing run.
///
/// @param request the check
/// @param profile the mounted mod, or null
/// @param level the resolved level
/// @param reason the fit failure, without the prefix
/// @return the report
BrainCheckReport unfit_report(
    const BrainCheckRequest& request,
    const data::mod_profile::ModProfile* profile,
    const std::string& level,
    const std::string& reason
) {
    BrainCheckReport report;
    report.game = "totala";
    report.level = level;
    report.network_bytes = network_file_bytes(request.brain);
    if (!request.mod_folder.empty())
        report.mod = std::make_pair(profile->id, profile->version);
    report.load.result = "not run";
    report.fits.text = reason;
    report.fits.result = "does not fit: " + reason;
    report.start.result = "not run";
    return report;
}

/// Mounts, fits, builds the match and seats the brain.
///
/// @param request the check
/// @param[out] problem set when nothing is returned
/// @param inspect called on the live match; may be null
/// @param context passed to `inspect`
/// @return the report, or nothing when the match cannot be built
std::optional<BrainCheckReport> check_seated(
    const BrainCheckRequest& request,
    std::string& problem,
    void (*inspect)(sim::match_runtime::Match& match, void* context),
    void* context
) {
    ResolvedSeat resolved;
    if (!resolve_seat(request.brain, request.choice, resolved, problem))
        return std::nullopt;

    ModChoice choice;
    choice.folder = request.mod_folder;
    const GameInstall install = inspect_game_install(request.game_dir, {}, demo_1997, choice);
    const auto unusable = !install.profile_errors.empty() || !install.problem.empty() ||
                          !install.folder || install.archives.empty() || install.folders.empty();
    if (unusable) {
        problem = path_to_utf8(request.game_dir) + ": " + describe_install_problem(install);
        return std::nullopt;
    }
    if (!request.mod_folder.empty() && install.profile == nullptr) {
        problem = path_to_utf8(request.game_dir) + ": the mod has no profile";
        return std::nullopt;
    }
    const data::mod_profile::ModProfile* profile = install.profile.get();
    const std::string fit = manifest_fit(request, profile);
    if (!fit.empty())
        return unfit_report(request, profile, resolved.level, fit);

    LayoutRestore layout;
    layout.arm();
    defs::use_data_layout(data_layout_of(profile));

    AssetStore store(install.folders);
    for (const std::filesystem::path& candidate : install.archives) {
        const std::filesystem::path archive =
            candidate.is_absolute() ? candidate : request.game_dir / candidate;
        store.mount(archive);
    }
    const defs::Files files = defs::asset_store_files(&store);

    // Declared before the match so they outlive it.
    QuietServices services;
    EmptyScenario scenario;
    FlatMap flat;
    sim::combat_state::WeaponRegistry weapons;
    sim::map_runtime::FeatureDefTable features;
    UnitTables tables;
    std::vector<sim::unit_spawn::LoadedType> loaded;
    std::vector<sim::unit_spawn::Type> types;
    std::vector<data::unit_definitions::UnitDefinition> definitions;
    std::vector<data::unit_definitions::RuntimeDefinitionMetadata> metadata;
    std::vector<data::unit_definitions::UnitTargetCategoryMasks> masks;
    std::vector<sim::match_runtime::RuntimeTypeFields> fields;
    std::vector<std::string> warnings;
    if (!load_units(
            store,
            files,
            profile,
            tables,
            weapons,
            features,
            loaded,
            types,
            definitions,
            metadata,
            masks,
            fields,
            warnings,
            problem
        ))
        return std::nullopt;

    defs::SideTable side_table;
    const uint32_t side_limit =
        profile != nullptr ? profile->limits.sides.count : data::limits::base_sides;
    if (!defs::load_side_data(&files, &side_table, nullptr, nullptr, side_limit) ||
        side_table.count == 0) {
        problem = side_table.error[0] != '\0' ? std::string(side_table.error)
                                              : "the side data does not load";
        return std::nullopt;
    }
    if (request.side >= side_table.count) {
        problem = "side " + std::to_string(request.side) + " is not one of this game's sides";
        return std::nullopt;
    }
    const Side* chosen = defs::side_at(side_table, request.side);
    const std::string commander = text_of(chosen->commander, sizeof chosen->commander);
    if (commander.empty()) {
        problem = "side " + std::to_string(request.side) + " names no commander";
        return std::nullopt;
    }
    const uint16_t commander_type =
        defs::unit_defs_type_id(tables.units.records, tables.units.count, commander.c_str());
    if (commander_type == 0) {
        problem =
            "side " + std::to_string(request.side) + "'s commander " + commander + " is not a unit";
        return std::nullopt;
    }

    const std::vector<uint16_t> build_lists = pack_build_lists(tables.units);
    sim::match_runtime::OfflineInputs input{
        .map = flat.map,
        .loaded = loaded,
        .types = types,
        .fields = fields,
        .weapons = weapons,
        .terrain_values = flat.terrain,
        .sight_masks = flat.masks,
        .sight_width = 32,
        .sight_height = 32,
        .per_player_limit = 32,
        .visibility_flags = 2,
        .viewpoint_player = 0,
        .clock_scale = 30,
        .random_seed = 1,
        .scenario_definitions = &scenario,
        .uptime_milliseconds = [] { return 1000u; },
        .collision_plots = flat.plots,
        .altitude_sight = std::nullopt,
        .minimum_wind = 0,
        .maximum_wind = 0,
        .tidal_strength = 0.0F,
        .feature_defs = features.defs,
        .unit_defs = std::span<const oa::UnitDef>(tables.units.records, tables.units.count),
    };
    if (profile != nullptr) {
        input.rules = profile->rules;
        input.limits = profile->limits;
        input.profile_sim_hash = profile->sim_hash;
    }
    if (const char* refusal = sim::match_runtime::Match::input_error(input)) {
        problem = refusal;
        return std::nullopt;
    }
    auto match = std::make_unique<sim::match_runtime::Match>(input, services);
    if (const char* fault = match->fault()) {
        problem = std::string("the check match is not usable: ") + fault;
        return std::nullopt;
    }

    for (uint8_t player = 0; player < 2; ++player) {
        match->simulation().players[player].present = true;
        match->simulation().players[player].status =
            player == 0 ? OA_PLAYER_STATUS_LOCAL : OA_PLAYER_STATUS_COMPUTER;
        std::array<uint8_t, 10> allies{};
        allies[player] = 1;
        match->configure_player_alliances(player, allies);
        auto& record = match->state().game.players[player];
        record.energy = 900.0F;
        record.metal = 900.0F;
    }
    std::array<uint8_t, 10> local_allies{};
    local_allies[0] = 1;
    match->configure_outcomes(0, local_allies, false);

    oa::World& world = match->state();
    const std::vector<oa::Side> records = defs::side_records(side_table);
    match->bind_sides(records, side_table.count);
    world.player_info[brain_player].side = request.side;

    sim::unit_spawn::Slot* slot = match->create(
        {brain_player,
         commander_type,
         {map_centre, 0, map_centre},
         true,
         sim::unit_spawn::ground_occupancy_state,
         0}
    );
    if (slot == nullptr || slot->unit == nullptr) {
        problem = "the commander was not placed";
        return std::nullopt;
    }
    slot->unit->object_present = true;

    facts::GameFacts game_facts = make_facts(store, profile, side_table, resolved.difficulty);
    const std::string profile_text = game_facts.ai_profile.text;
    sim::brains::set_match_game_facts(*match, std::move(game_facts));

    if (!sim::ai::configure_match_computer_players(*match, profile_text, build_lists, false)) {
        problem = "cannot store the computer players' profile and build lists";
        return std::nullopt;
    }

    std::vector<sim::brains::PackageFile> package_files;
    std::vector<sim::brains::OptionValue> option_values;
    std::vector<sim::brains::NetworkFile> network_files;
    const sim::brains::SeatSpec spec = seat_spec(
        brain_player, request.brain, resolved, package_files, option_values, network_files
    );
    const sim::brains::SeatOutcome outcome = sim::brains::seat_brain(*match, spec);

    BrainCheckReport report;
    report.game = "totala";
    report.level = resolved.level;
    report.network_bytes = network_file_bytes(request.brain);
    report.warnings = std::move(warnings);
    if (!request.mod_folder.empty())
        report.mod = std::make_pair(profile->id, profile->version);
    fill_phases(outcome, report);
    if (report.seated) {
        if (script::Sandbox* sandbox = sim::brains::seat_sandbox(*match, brain_player))
            report.lua_bytes = sandbox->memory_in_use();
    }
    if (inspect != nullptr)
        inspect(*match, context);
    return report;
}

} // namespace

std::optional<BrainCheckReport> check_brain_against_game(
    const BrainCheckRequest& request,
    std::string& problem,
    void (*inspect)(sim::match_runtime::Match& match, void* context),
    void* context
) {
    try {
        return check_seated(request, problem, inspect, context);
    } catch (const std::bad_alloc&) {
        problem = "the check ran out of memory";
        return std::nullopt;
    } catch (const std::exception& ex) {
        problem = ex.what();
        return std::nullopt;
    }
}

std::optional<BrainCheckReport>
check_brain_against_game(const BrainCheckRequest& request, std::string& problem) {
    return check_brain_against_game(request, problem, nullptr, nullptr);
}

} // namespace oa::app
