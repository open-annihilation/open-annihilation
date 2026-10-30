// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Unit types of the installed game for match tests: their FBI definitions over
// the installed movement classes, weapons, categories, corpses, models and
// scripts, laid out as a Match takes them, and a map of open sea, a shallow
// shelf and land to put them on.
#pragma once

#include "oa/data/defs/asset_files.hpp"
#include "oa/data/defs/unit_catalog.hpp"
#include "oa/data/defs/unit_def_loader.hpp"
#include "oa/data/defs/unit_header.hpp"
#include "oa/sim/map_runtime.hpp"
#include "oa/sim/map_runtime/feature_defs.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/test/game_assets.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oa::test {

/// Reads the installed game's unit models and scripts for a runtime type.
class InstalledReader final : public sim::unit_spawn::AssetReader {
  public:

    /// Binds the reader to a mounted store.
    ///
    /// @param store the installed game's store; must outlive the reader
    explicit InstalledReader(const AssetStore& store) : store_(store) {}

    /// Reads a file of the installed game.
    ///
    /// @param path path inside the game data
    /// @return the bytes, or nothing when no archive provides the file
    std::optional<std::vector<uint8_t>> read(std::string_view path) override {
        auto bytes = read_game_file(store_, path);
        if (bytes.empty())
            return std::nullopt;
        return bytes;
    }

  private:

    const AssetStore& store_;
};

/// Lists and reads the installed game's feature TDFs.
class InstalledFeatureReader final : public sim::map_runtime::FeatureAssetReader {
  public:

    /// Binds the reader to a mounted store.
    ///
    /// @param store the installed game's store; must outlive the reader
    explicit InstalledFeatureReader(const AssetStore& store) : store_(store) {}

    /// Lists every effective file under a directory, recursively.
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
    /// @return the text
    data::unit_definitions::Result<std::string> read(std::string_view path) const override {
        const auto bytes = read_game_file(store_, path);
        return {std::string(bytes.begin(), bytes.end()), {}};
    }

  private:

    const AssetStore& store_;
};

/// Named unit types of the installed game, as a Match's offline inputs take them.
///
/// Each FBI loads as a match loads it: its header, then the whole file into
/// a UnitDef record, from which the typed fields and the runtime metadata
/// come. Type 0 is reserved; the named types follow in the order given.
/// Weapons are every installed weapon, and the category masks are resolved
/// over the named types alone, which the categories they name need.
struct InstalledUnits {
    // The tables the unit files load against, released once the types are built.
    struct Tables {
        data::defs::SoundCategoryTable sounds{};
        data::defs::WeaponTdfSet weapon_files{};
        data::defs::UnitDefTables units{};

        /// Empties the tables.
        Tables() {
            data::defs::weapon_tdf_set_init(&weapon_files);
            data::defs::unit_def_tables_init(&units);
        }

        /// Frees what the tables hold.
        ~Tables() {
            data::defs::sound_category_table_free(&sounds);
            data::defs::weapon_tdf_set_free(&weapon_files);
            data::defs::unit_def_tables_free(&units);
        }

        Tables(const Tables&) = delete;
        Tables& operator=(const Tables&) = delete;
    };

    // Each unit's corpse, loaded into the feature table as the FBI names it.
    struct Corpses {
        sim::map_runtime::FeatureDefTable& features;
        std::vector<formats::tdf::OwnedDocument>& documents;
        std::string error;

        /// Finds or loads a corpse feature (UnitDefLoadHost::corpse).
        ///
        /// @param context the Corpses
        /// @param name feature name
        /// @return its FeatureDef index; -1 when it does not load
        static int16_t load(void* context, const char* name) {
            auto& self = *static_cast<Corpses*>(context);
            const auto found = sim::map_runtime::find_or_load_feature(
                self.features, self.documents, name, nullptr
            );
            if (!found.ok()) {
                self.error = found.error->message;
                return -1;
            }
            return static_cast<int16_t>(found.index);
        }
    };

    std::vector<sim::unit_spawn::LoadedType> loaded;
    std::vector<sim::unit_spawn::Type> types;
    std::vector<data::unit_definitions::UnitDefinition> definitions;
    std::vector<data::unit_definitions::RuntimeDefinitionMetadata> metadata;
    std::vector<sim::match_runtime::RuntimeTypeFields> fields;
    std::vector<data::unit_definitions::UnitTargetCategoryMasks> target_masks;
    sim::combat_state::WeaponRegistry weapons;
    std::vector<formats::tdf::OwnedDocument> feature_documents;
    sim::map_runtime::FeatureDefTable features;

    /// Loads the named types.
    ///
    /// Throws std::runtime_error when a file is missing or does not load.
    ///
    /// @param store the installed game's store
    /// @param names unit names, the FBI file names without extension
    InstalledUnits(const AssetStore& store, std::initializer_list<std::string_view> names) {
        const auto files = data::defs::asset_store_files(&store);
        (void)sim::combat_state::install_weapon_files(weapons, files);
        auto documents = sim::map_runtime::load_feature_documents(InstalledFeatureReader(store));
        if (!documents)
            throw std::runtime_error("feature documents: " + documents.error.message);
        feature_documents = std::move(documents.value);
        const auto movement = std::make_unique<data::defs::MoveClassTable>();
        if (!data::defs::load_move_classes(&files, movement.get(), nullptr, nullptr))
            throw std::runtime_error("moveinfo.tdf does not load");
        Tables tables;
        if (!data::defs::load_sound_categories(&files, &tables.sounds, nullptr))
            throw std::runtime_error("sound.tdf does not load");
        if (!data::defs::load_weapon_tdf_set(&files, nullptr, false, &tables.weapon_files))
            throw std::runtime_error("the weapon files do not list");
        const auto count = names.size() + 1;
        if (!data::defs::unit_def_tables_allocate(&tables.units, static_cast<uint32_t>(count)))
            throw std::runtime_error("the unit table does not allocate");
        loaded.resize(count);
        types.resize(count);
        definitions.resize(count);
        metadata.resize(count);
        fields.resize(count);
        target_masks.resize(count);
        InstalledReader reader(store);
        Corpses corpses{features, feature_documents, {}};
        const data::defs::UnitDefLoadHost corpse_host{&corpses, Corpses::load};
        const data::defs::UnitHeaderSources header_sources{
            "", &tables.weapon_files, 3, 1, false, false
        };
        const data::defs::UnitDefSources unit_sources{
            "",
            movement.get(),
            weapons.records(),
            &tables.sounds,
            &tables.units.categories,
            &tables.units.blocks,
            &corpse_host
        };
        const data::unit_definitions::UnitDefinitionSources definition_sources{
            movement.get(), weapons.records(), &tables.sounds, &tables.units.categories
        };
        uint16_t index = 0;
        for (const auto name : names) {
            ++index;
            auto& record = tables.units.records[index];
            record.type_id = index;
            char fbi[data::defs::path_capacity];
            data::defs::build_variant_path(
                &files, fbi, sizeof fbi, "units", std::string(name).c_str(), "FBI", nullptr
            );
            bool refused = false;
            if (!data::defs::load_unit_header(&files, fbi, record, header_sources, &refused) ||
                !data::defs::load_unit_def(&files, fbi, record, unit_sources))
                throw std::runtime_error(std::string(fbi) + " does not load");
            if (!corpses.error.empty())
                throw std::runtime_error(std::string(fbi) + " corpse: " + corpses.error);
            auto definition =
                data::unit_definitions::unit_definition_from(record, definition_sources);
            auto resolved = data::unit_definitions::resolve_runtime_metadata(
                record, *movement, tables.units.blocks
            );
            if (!resolved)
                throw std::runtime_error(std::string(fbi) + ": " + resolved.error.message);
            sim::unit_spawn::RuntimeBindings bindings;
            bindings.enabled = true;
            bindings.resolved_weapon_present =
                sim::combat_state::bind_unit_weapons(
                    weapons, {definition.weapon1, definition.weapon2, definition.weapon3}
                )
                    .resolved_nondefault_weapon;
            bindings.default_mission = static_cast<uint8_t>(record.default_mission_type);
            bindings.movement_footprint =
                std::array<int16_t, 2>{resolved.value.footprint_x, resolved.value.footprint_z};
            loaded[index] = sim::unit_spawn::load_runtime_type(definition, bindings, reader);
            types[index] = loaded[index].type;
            definitions[index] = std::move(definition);
            metadata[index] = std::move(resolved.value);
        }
        if (const auto error =
                sim::map_runtime::load_feature_links(features, feature_documents, nullptr))
            throw std::runtime_error("feature links: " + error->message);
        for (std::size_t type = 1; type < count; ++type) {
            target_masks[type] = data::unit_definitions::target_category_masks(
                tables.units.records[type], tables.units.categories
            );
            auto& field = fields[type];
            field.definition = &definitions[type];
            field.yard_mask = metadata[type].yard_cells;
            field.runtime_metadata = &metadata[type];
            field.target_masks = &target_masks[type];
            // A movement class's handle is its slot + 1; 0 means none.
            field.movement_class = metadata[type].movement_class_handle
                                       ? static_cast<sim::unit_spawn::AssetHandle>(
                                             *metadata[type].movement_class_handle
                                         ) + 1U
                                       : sim::unit_spawn::AssetHandle{0};
            field.corpse_feature = tables.units.records[type].corpse;
        }
    }

    /// Returns a named type's index.
    ///
    /// Throws std::out_of_range for a name that was not loaded.
    ///
    /// @param name unit name, matched ignoring case
    /// @return the type index, 1 or more
    [[nodiscard]] uint16_t type(std::string_view name) const {
        for (std::size_t index = 1; index < definitions.size(); ++index) {
            const auto& unit = definitions[index].unit_name;
            if (unit.size() == name.size() &&
                std::equal(unit.begin(), unit.end(), name.begin(), [](char a, char b) {
                    return std::tolower(static_cast<unsigned char>(a)) ==
                           std::tolower(static_cast<unsigned char>(b));
                }))
                return static_cast<uint16_t>(index);
        }
        throw std::out_of_range("no installed type " + std::string(name));
    }
};

/// A map of open sea, a shallow shelf and land, in 16-unit cells: columns
/// under `shelf_column` lie `deep_floor` high, those under `land_column`
/// `shelf_floor`, and the rest `land_height`, with the sea at `sea_level`.
struct Seascape {
    formats::tnt::Map map;
    std::vector<sim::visibility_state::TerrainCell> terrain_values;
    std::vector<sim::spatial_state::Plot> plots;
    std::array<sim::visibility_state::SightMask, 1> masks{};

    /// Builds the map.
    ///
    /// @param width columns
    /// @param height rows
    /// @param sea_level sea level in whole world units
    /// @param shelf_column first column of the shelf
    /// @param land_column first column of land
    /// @param deep_floor height of the open sea's floor
    /// @param shelf_floor height of the shelf
    /// @param land_height height of the land
    Seascape(
        int32_t width,
        int32_t height,
        uint8_t sea_level,
        int32_t shelf_column,
        int32_t land_column,
        uint8_t deep_floor,
        uint8_t shelf_floor,
        uint8_t land_height
    ) {
        const auto floor_at = [&](int32_t column) {
            if (column >= land_column)
                return land_height;
            return column >= shelf_column ? shelf_floor : deep_floor;
        };
        map.attribute_width = static_cast<uint32_t>(width);
        map.attribute_height = static_cast<uint32_t>(height);
        map.sea_level = sea_level;
        const auto cells = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
        map.attributes.resize(cells);
        plots.resize(cells);
        terrain_values.resize(cells);
        for (int32_t z = 0; z < height; ++z)
            for (int32_t x = 0; x < width; ++x) {
                const auto index = static_cast<std::size_t>(z * width + x);
                map.attributes[index].height = floor_at(x);
                const auto next = floor_at(std::min(x + 1, width - 1));
                plots[index].low_height = std::min(floor_at(x), next);
                plots[index].high_height = std::max(floor_at(x), next);
            }
        // Sight reaches seven 32-unit cells around a unit.
        masks[0].width = masks[0].height = 15;
        masks[0].offset_x = masks[0].offset_z = 7;
        masks[0].pixels.assign(15 * 15, 1);
    }
};

} // namespace oa::test
