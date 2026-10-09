// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Skirmish match bootstrap from the selected map and players.
#include "oa/app/runtime.hpp"
#include "oa/ui/hud/unit_labels.hpp"
#include "engine_settings_state.hpp"
#include "oa/app/asset_files.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/app/match_console.hpp"
#include "oa/app/view_rules.hpp"
#include "oa/data/campaign/campaign_file.hpp"
#include "oa/data/defs/gamedata_tables.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/defs/rule_keys.hpp"
#include "oa/data/defs/unit_def_loader.hpp"
#include "oa/data/defs/unit_header.hpp"
#include "oa/data/defs/unit_records.hpp"
#include "oa/data/mission_types.hpp"
#include "oa/sim/session.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/decoded.hpp"
#include "oa/ui/hud/player_records.hpp"
#include "oa/ui/hud/status_panel.hpp"
#include "match_fault.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

// A seated player reports the machine's memory; without a measurement the
// match reports the player setup's default, 256 MB.
constexpr int32_t kStandInPhysicalMemory = 256 * 0x100000;
// The player timeout of a match no launch switch changed: seconds a silent
// player is waited for.
constexpr int32_t kPlayerTimeoutSeconds = 30;
// The most of a map's undefined features a failed start names.
constexpr std::size_t kMostMissingFeaturesNamed = 4;

/// Returns why a map naming features that no feature file defines cannot
/// start: those features, the first few of them by name, as maps made for
/// other game data name them. 3.1c cannot start such a map either.
///
/// @param map the map
/// @param documents the feature files of the game and its mods
/// @return the reason, for the failed start's message
std::string missing_features_reason(
    const oa::formats::tnt::Map& map, std::span<const oa::formats::tdf::OwnedDocument> documents
) {
    std::vector<std::string_view> missing;
    for (const auto& feature : map.features)
        if (oa::sim::map_runtime::find_feature_section(documents, feature.name) == nullptr &&
            std::ranges::find(missing, std::string_view(feature.name)) == missing.end())
            missing.push_back(feature.name);
    std::string reason = "the map needs features the game and its mods do not define: ";
    for (std::size_t at = 0; at < missing.size() && at < kMostMissingFeaturesNamed; ++at)
        reason += (at == 0 ? "" : ", ") + std::string(missing[at]);
    if (missing.size() > kMostMissingFeaturesNamed)
        reason += " and " + std::to_string(missing.size() - kMostMissingFeaturesNamed) + " more";
    reason += "; it was made for other game data";
    return reason;
}

class FeatureCatalogReader final : public oa::sim::map_runtime::FeatureAssetReader {
  public:

    explicit FeatureCatalogReader(const oa::AssetStore& assets) : assets_(assets) {}

    oa::data::unit_definitions::Result<std::vector<std::string>> list_effective_recursive(
        std::string_view directory, std::string_view extension
    ) const override {
        try {
            return {assets_.list_effective_recursive(directory, extension), {}};
        } catch (const std::exception& error) {
            return {{}, {oa::data::unit_definitions::ErrorCode::io, 0, error.what()}};
        }
    }

    oa::data::unit_definitions::Result<std::string> read(std::string_view path) const override {
        try {
            const auto bytes = assets_.read(path).bytes;
            return {std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size()), {}};
        } catch (const std::exception& error) {
            return {{}, {oa::data::unit_definitions::ErrorCode::io, 0, error.what()}};
        }
    }

  private:

    const oa::AssetStore& assets_;
};

// A feature one-shot sequence (burn, die, reclamate) plays once: the loader
// clears the repeat byte of the loaded GAF entry.
constexpr uint16_t gaf_repeat_byte = 0x00ff;

template <typename T>
oa_ref32 table_ref(const std::vector<T>& table) {
    return static_cast<oa_ref32>(table.size());
}

// Option bits the match loader clears: the radar, double-shot
// and half-shot console toggles last only for the match they were set in.
constexpr uint16_t match_cleared_console_flags = oa::ui::console::console_flag::full_radar |
                                                 oa::ui::console::console_flag::double_shot |
                                                 oa::ui::console::console_flag::half_shot;

// The unit loader loads a corpse= feature the table lacks; the first failure is kept.
struct CorpseFeatures {
    oa::sim::map_runtime::FeatureDefTable& table;
    std::span<const oa::formats::tdf::OwnedDocument> documents;
    const oa::sim::map_runtime::FeatureDefHost* host{};
    std::string error;

    static int16_t load(void* context, const char* name) {
        auto& self = *static_cast<CorpseFeatures*>(context);
        const auto corpse =
            oa::sim::map_runtime::find_or_load_feature(self.table, self.documents, name, self.host);
        if (corpse.ok())
            return static_cast<int16_t>(corpse.index);
        if (self.error.empty())
            self.error =
                "cannot load corpse feature '" + std::string(name) + "': " + corpse.error->message;
        return -1;
    }
};

// The rays per TABLEn: line1..lineN, each read into a 0x200-byte
// buffer (a missing line reads empty), become one altitude sight pattern.
struct LosTables {
    std::vector<oa::sim::visibility_state::AltitudeSightPattern>& patterns;
    std::string error;

    static bool resize(void* context, int16_t table_count) {
        auto& self = *static_cast<LosTables*>(context);
        try {
            self.patterns.assign(static_cast<std::size_t>(table_count), {});
            return true;
        } catch (const std::exception& failure) {
            self.error = failure.what();
            return false;
        }
    }

    static bool table(
        void* context, int16_t index, const oa::formats::tdf::Block* section, int16_t line_count
    ) {
        auto& self = *static_cast<LosTables*>(context);
        try {
            std::vector<std::string> texts;
            texts.reserve(static_cast<std::size_t>(line_count));
            for (int16_t line = 0; line < line_count; ++line) {
                char key[32];
                char text[0x200];
                std::snprintf(key, sizeof key, "line%d", line + 1);
                oa::formats::tdf::get_string(section, key, text, sizeof text, "");
                texts.emplace_back(text);
            }
            const std::vector<std::string_view> lines(texts.begin(), texts.end());
            auto pattern = oa::sim::visibility_state::build_altitude_pattern(lines);
            if (pattern.error != nullptr) {
                self.error =
                    "gamedata/los.tdf TABLE" + std::to_string(index + 1) + ": " + pattern.error;
                return false;
            }
            self.patterns[static_cast<std::size_t>(index)] = std::move(pattern);
            return true;
        } catch (const std::exception& failure) {
            self.error =
                "gamedata/los.tdf TABLE" + std::to_string(index + 1) + ": " + failure.what();
            return false;
        }
    }
};

/// Returns a name with its ASCII letters lowered.
///
/// @param name the name
/// @return the lowered name
std::string lowered_name(std::string_view name) {
    std::string lowered(name);
    for (auto& character : lowered)
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    return lowered;
}

/// Returns a bound key's name for the loaders: null when nothing is bound.
///
/// @param name the key's name in the files, empty when unbound
/// @return the name, or null
const char* bound_key(const std::string& name) {
    return name.empty() ? nullptr : name.c_str();
}

/// Returns the weapon-file keys a mod profile binds.
///
/// @param keys the profile's bindings; must outlive the result
/// @return the names, null where nothing is bound
oa::data::defs::WeaponDataKeys
bound_weapon_keys(const oa::data::mod_profile::DataKeyBindings& keys) {
    return {
        bound_key(keys.weapons_not_to_air),
        bound_key(keys.weapons_surface_fire),
        bound_key(keys.weapons_not_to_underwater),
        bound_key(keys.weapons_no_map_alert)
    };
}

/// Returns the data keys a mod profile binds.
///
/// @param profile the profile; null for base 3.1c
/// @return its bindings, or bindings of nothing without a profile
const oa::data::mod_profile::DataKeyBindings&
data_key_bindings(const oa::data::mod_profile::ModProfile* profile) {
    static const oa::data::mod_profile::DataKeyBindings none{};
    return profile != nullptr ? profile->data_keys : none;
}

/// Returns the unit-file rule keys a mod profile binds.
///
/// @param keys the profile's bindings; must outlive the result
/// @return the names, null where nothing is bound
oa::data::defs::UnitDataKeys bound_unit_keys(const oa::data::mod_profile::DataKeyBindings& keys) {
    return {
        bound_key(keys.veterancy_thresholds),
        bound_key(keys.veterancy_accuracy_rate),
        bound_key(keys.units_build_facings)
    };
}

/// Returns the unit-file preview keys a mod profile binds.
///
/// @param keys the profile's bindings; must outlive the result
/// @return the names, null where nothing is bound
oa::data::defs::UnitPreviewDataKeys
bound_unit_preview_keys(const oa::data::mod_profile::DataKeyBindings& keys) {
    return {
        bound_key(keys.ui_preview_pieces),
        bound_key(keys.ui_preview_pieces_by_facing),
        bound_key(keys.ui_preview_object),
        bound_key(keys.ui_preview_face_opponent)
    };
}

/// Reports the bound unit keys of one file whose values could not be used;
/// the type then takes the profile's defaults for them.
///
/// @param path the FBI file's path
/// @param keys the names the profile binds
/// @param issues what was wrong with each
void report_rule_key_issues(
    const char* path,
    const oa::data::defs::UnitDataKeys& keys,
    const oa::data::defs::RuleKeyIssues& issues
) {
    const auto report = [path](const char* key, oa::data::defs::RuleKeyProblem problem) {
        if (problem != oa::data::defs::RuleKeyProblem::none)
            std::cerr << path << ": " << key << ": "
                      << oa::data::defs::rule_key_problem_text(problem)
                      << "; the mod profile's default applies\n";
    };
    report(keys.veterancy_thresholds, issues.veterancy_thresholds);
    report(keys.veterancy_accuracy_rate, issues.veterancy_accuracy_rate);
    report(keys.build_facings, issues.build_facings);
}

} // namespace

oa::data::unit_definitions::Result<std::vector<oa::formats::tdf::OwnedDocument>>
Runtime::load_feature_tdf_set() const {
    return oa::sim::map_runtime::load_feature_documents(FeatureCatalogReader(assets_));
}

oa::sim::map_runtime::FeatureDefHost Runtime::feature_def_host() {
    oa::sim::map_runtime::FeatureDefHost host{};
    host.context = this;
    host.load_animation = [](void* context, const char* gaf_name) -> oa_ref32 {
        auto& self = *static_cast<Runtime*>(context);
        try {
            auto file = self.assets_.read("anims/" + std::string(gaf_name) + ".gaf").bytes;
            auto parsed = oa::formats::gaf::parse(file, oa::formats::gaf::PixelData::checked);
            if (!parsed.ok())
                return 0;
            self.feature_assets_.archives.push_back(
                std::make_unique<oa::formats::gaf::Archive>(std::move(*parsed.archive))
            );
            self.feature_assets_.archive_files.push_back(std::move(file));
            self.feature_assets_.archive_names.push_back(lowered_name(gaf_name));
        } catch (const std::exception&) {
            return 0;
        }
        return table_ref(self.feature_assets_.archives);
    };
    host.find_sequence =
        [](void* context, oa_ref32 animation, const char* name, bool one_shot) -> oa_ref32 {
        auto& self = *static_cast<Runtime*>(context);
        auto& assets = self.feature_assets_;
        if (animation == 0 || animation > assets.archives.size())
            return 0;
        oa::formats::gaf::Sequence* sequence = nullptr;
        auto& archive_sequences = assets.archives[animation - 1]->sequences;
        std::size_t place = 0;
        for (; place < archive_sequences.size(); ++place)
            if (tdf_names_equal(archive_sequences[place].name, name)) {
                sequence = &archive_sequences[place];
                break;
            }
        if (sequence == nullptr)
            return 0;
        if (one_shot)
            sequence->repeat_flags =
                static_cast<uint16_t>(sequence->repeat_flags & ~gaf_repeat_byte);
        for (std::size_t index = 0; index < assets.sequences.size(); ++index)
            if (assets.sequences[index] == sequence)
                return static_cast<oa_ref32>(index + 1);
        assets.sequences.push_back(sequence);
        assets.sequence_places.push_back({animation - 1U, place});
        assets.rendered.emplace_back();
        return table_ref(assets.sequences);
    };
    host.load_object = [](void* context, const char* object_name) -> oa_ref32 {
        auto& self = *static_cast<Runtime*>(context);
        try {
            const auto path = "objects3d/" + std::string(object_name) + ".3DO";
            const auto bytes = self.assets_.read(path).bytes;
            self.feature_assets_.models.push_back(
                std::make_shared<const oa::formats::objects3d::Model>(oa::ui::decoded::require(
                    oa::formats::objects3d::load_3do(std::as_bytes(std::span(bytes))), path
                ))
            );
        } catch (const std::exception&) {
            return 0;
        }
        return table_ref(self.feature_assets_.models);
    };
    host.find_weapon = [](void* context, const char* weapon_name) -> oa_ref32 {
        const auto* definition = static_cast<Runtime*>(context)->weapon_registry_.find(weapon_name);
        return definition != nullptr ? oa::oa_ref_from_index(definition->registry_index) : 0u;
    };
    host.sequence_frame = [](void* context,
                             oa_ref32 sequence,
                             uint16_t frame,
                             uint16_t* frame_count,
                             uint8_t* repeat,
                             uint16_t* duration) {
        // Without a sequence, or past its last frame, the frame stays empty,
        // and its zero frame count tells the feature runtime so.
        oa::sim::feature_runtime::FeatureSequenceFrame out{};
        std::ignore = static_cast<Runtime*>(context)->feature_sequence_frame(sequence, frame, out);
        *frame_count = out.frame_count;
        *repeat = out.repeat;
        *duration = out.duration;
    };
    return host;
}

Runtime::FeatureGafView Runtime::feature_gaf_file(const std::string& filename, std::string& error) {
    error.clear();
    const auto key = lowered_name(filename);
    const auto& names = feature_assets_.archive_names;
    if (const auto loaded = std::find(names.begin(), names.end(), key); loaded != names.end()) {
        const auto index = static_cast<std::size_t>(loaded - names.begin());
        return {feature_assets_.archive_files[index], feature_assets_.archives[index].get()};
    }
    auto found = feature_gaf_files_.find(key);
    if (found == feature_gaf_files_.end()) {
        FeatureGafFile read;
        read.file = assets_.read("anims/" + filename + ".gaf").bytes;
        auto parsed = oa::formats::gaf::parse(read.file, oa::formats::gaf::PixelData::checked);
        if (parsed.ok())
            read.archive = std::move(*parsed.archive);
        else
            read.error = parsed.error->message;
        found = feature_gaf_files_.emplace(key, std::move(read)).first;
    }
    error = found->second.error;
    return {found->second.file, error.empty() ? &found->second.archive : nullptr};
}

std::optional<oa::formats::gaf::Sequence>
Runtime::decode_feature_sequence(oa_ref32 sequence) const {
    if (sequence == 0 || sequence > feature_assets_.sequences.size())
        return std::nullopt;
    const auto& place = feature_assets_.sequence_places[sequence - 1];
    auto decoded =
        oa::formats::gaf::parse_sequence(feature_assets_.archive_files[place.archive], place.index);
    if (!decoded.ok())
        return std::nullopt;
    decoded.sequence->repeat_flags = feature_assets_.sequences[sequence - 1]->repeat_flags;
    return std::move(decoded.sequence);
}

bool Runtime::feature_sequence_frame(
    oa_ref32 sequence, uint16_t frame, oa::sim::feature_runtime::FeatureSequenceFrame& out
) const {
    out = {};
    if (sequence == 0 || sequence > feature_assets_.sequences.size())
        return false;
    const auto& frames = feature_assets_.sequences[sequence - 1]->frames;
    out.frame_count = static_cast<uint16_t>(frames.size());
    out.repeat = static_cast<uint8_t>(
        feature_assets_.sequences[sequence - 1]->repeat_flags & gaf_repeat_byte
    );
    if (frame >= frames.size())
        return false;
    const auto& image = frames[frame];
    out.duration = image.duration;
    out.width = static_cast<int16_t>(image.width);
    out.height = static_cast<int16_t>(image.height);
    out.origin_x = image.origin_x;
    out.origin_y = image.origin_y;
    return true;
}

void Runtime::apply_skirmish_players() {
    // A new skirmish plays at the run's unit limit.
    EngineSettingsState::start_skirmish(*this, EngineSettingsState::run_unit_limit(*this), true);
}

void Runtime::seat_skirmish_roster(oa::World& world) {
    std::array<oa::ui::hud::SkirmishSlot, entry::skirmish_slot_capacity> roster{};
    for (std::size_t index = 0; index < roster.size(); ++index) {
        const auto& slot = skirmish_settings_.slots[index];
        roster[index] = {
            slot.controller, slot.side, slot.alliance, slot.metal, slot.energy, slot.color
        };
    }
    oa::ui::hud::init_player_slots_from_roster(
        world,
        roster.data(),
        skirmish_settings_.slot_count,
        oa::data::campaign::SessionKind::skirmish,
        kStandInPhysicalMemory
    );
}

void Runtime::seat_campaign_players(oa::World& world) {
    oa::ui::hud::init_player_slot(
        world,
        0,
        OA_PLAYER_STATUS_LOCAL,
        oa::data::campaign::SessionKind::campaign,
        kStandInPhysicalMemory
    );
    oa::ui::hud::init_player_slot(
        world,
        1,
        OA_PLAYER_STATUS_COMPUTER,
        oa::data::campaign::SessionKind::campaign,
        kStandInPhysicalMemory
    );
    if (auto* info = oa::world_player_info(&world, &world.game.players[1]))
        info->color = 1;
}

void Runtime::bind_match_speech() {
    // Captions come in the game's English wording and keep it in the queue,
    // which keeps its own copy; the message log shows them in the player's
    // language (post_unit_report).
    oa::sim::match_runtime::SpeechHooks hooks;
    hooks.context = this;
    hooks.speak =
        [](void* context, oa::sim::unit_spawn::Slot& slot, uint32_t category, const char* caption) {
            auto& self = *static_cast<Runtime*>(context);
            const auto& fixes = self.ui_rules().interface_fixes;
            caption = oa::ui::hud::shown_unit_caption(
                caption,
                fixes.enabled &&
                    fixes.fixes.contains(
                        oa::data::mod_profile::UiInterfaceFixesFixes::resurrect_spelling
                    )
            );
            self.offline_services_.command_speech(
                slot, category, caption != nullptr ? std::string_view(caption) : std::string_view{}
            );
        };
    match_->set_speech_hooks(hooks);
}

void Runtime::bootstrap_match(const MatchBootstrap& bootstrap) {
    // A shared game whose mod cannot start one is refused before anything is
    // dropped or loaded; its warning shows once the main menu does, where
    // the abandoned launch returns.
    if (bootstrap.multiplayer && refuse_incomplete_mod_start(true))
        throw std::runtime_error("the mod's files are missing, so its games cannot start");
    // A new Start attempt owns a new world.  Do not let a failed bootstrap
    // expose commanders, timing state, or a renderable match from an older
    // map selection, nor the last game's end screen.
    release_endgame();
    teardown_match();
    // From its loading screen a shared game or a replay keeps the tier it
    // has.
    begin_render_tier_match(
        bootstrap.multiplayer ? render_policy::MatchKind::shared_game
        : bootstrap.replay    ? render_policy::MatchKind::replay
                              : render_policy::MatchKind::none
    );
    altitude_sight_blocked_ = false;
    match_tick_blocked_ = false;
    match_timing_ = {};
    load_progress_.fill(0);
    begin_loading_screen();
    // A shared game or a replay makes Full's pages and targets now, before
    // the world is built and before the machines wait for each other, and
    // nothing of them afterwards.
    preallocate_full_match_textures();
    set_load_progress(0, 10);
    if (bootstrap.place_commanders && selected_start_markers_.empty() && !campaign_mission_)
        throw std::runtime_error("selected map has no start-position schema");
    session_schema_ = session_schema();
    // The weapons, every WEAPONS\*.TDF section into its ID's slot. A lava
    // world loads the lava explosions into the water explosion slot.
    weapon_registry_ = oa::sim::combat_state::WeaponRegistry{};
    weapon_model_names_.clear();
    const oa::data::defs::Files files = asset_files(assets_);
    std::size_t installed_weapons = 0;
    {
        const auto weapon_table = std::make_unique<oa::data::defs::WeaponTable>();
        const oa::data::defs::WeaponDataKeys weapon_keys =
            bound_weapon_keys(data_key_bindings(mod_profile()));
        const oa::data::defs::WeaponLoadOptions weapon_options{
            nullptr, nullptr, integer("lavaworld", 0) != 0, false, &weapon_keys
        };
        oa::data::defs::load_weapon_defs(&files, weapon_table.get(), &weapon_options);
        installed_weapons =
            oa::sim::combat_state::install_weapon_table(weapon_registry_, *weapon_table);
        // Each weapon's own rules, by weapon ID, when the profile binds a weapon key.
        weapon_rules_.assign(weapon_keys.any() ? OA_WEAPON_DEF_COUNT : 0U, {});
        for (std::size_t slot = 0; slot < weapon_rules_.size(); ++slot) {
            const WeaponDef& weapon = weapon_table->defs[slot];
            if (weapon.key[0] != '\0')
                weapon_rules_[weapon.weapon_id] = weapon_table->rule_data[slot];
        }
        for (std::size_t slot = 0; slot < OA_WEAPON_DEF_COUNT; ++slot) {
            const char* model = weapon_table->assets[slot].model;
            if (weapon_table->defs[slot].key[0] != '\0' && model[0] != '\0')
                weapon_model_names_.emplace(static_cast<uint8_t>(slot), model);
        }
        oa::data::defs::weapon_table_free(weapon_table.get());
    }
    if (installed_weapons == 0)
        throw std::runtime_error("no base weapon definitions were installed");
    set_load_progress(0, 100);

    // Mission state set-up: Terrain before Units.
    set_load_progress(1, 15);
    if (!selected_tnt_)
        throw std::runtime_error("selected map terrain is not loaded");
    // The feature TDF set is parsed once; the map's
    // feature table, the unit corpses and the link pass all search it.
    const auto feature_documents = load_feature_tdf_set();
    if (!feature_documents)
        throw std::runtime_error(
            "cannot load feature definitions: " + feature_documents.error.message
        );
    const auto feature_terrain =
        oa::sim::map_runtime::resolve_feature_terrain(*selected_tnt_, feature_documents.value);
    if (!feature_terrain.ok()) {
        if (feature_terrain.error->code ==
            oa::sim::map_runtime::ErrorCode::missing_feature_definition)
            throw std::runtime_error(
                missing_features_reason(*selected_tnt_, feature_documents.value)
            );
        throw std::runtime_error("cannot resolve map features: " + feature_terrain.error->message);
    }
    // The map's features fill the table in TNT order, so
    // the plot feature words index it directly. The table references the
    // GAF sequences, 3DO models and burn weapons through feature_assets_.
    feature_assets_ = {};
    feature_gaf_files_.clear();
    const auto feature_host = feature_def_host();
    if (const auto error = oa::sim::map_runtime::init_feature_table(
            feature_table_, *selected_tnt_, feature_documents.value, &feature_host
        ))
        throw std::runtime_error("cannot load map feature table: " + error->message);
    load_mission_features(feature_documents.value, feature_host);
    set_load_progress(1, 45);
    const auto mask_bytes = assets_.read(oa::sim::map_runtime::sight_mask_archive).bytes;
    const auto mask_archive = oa::formats::gaf::parse(mask_bytes);
    if (!mask_archive.ok())
        throw std::runtime_error("cannot parse visibility masks: " + mask_archive.error->message);
    // The terrain load seeds every plot's metal from the session object's SurfaceMetal.
    configured_map_metal_ = schema_integer("SurfaceMetal", 0);
    auto prepared = oa::sim::map_runtime::prepare(
        *selected_tnt_, configured_map_metal_, *feature_terrain.value, *mask_archive.archive
    );
    if (!prepared.ok())
        throw std::runtime_error("cannot prepare map runtime: " + prepared.error->message);
    prepared_map_ = std::move(*prepared.value);
    set_load_progress(1, 100);
    // The Full tier's terrain pages, with the terrain: made here, before the
    // world is built and before a shared game's load barrier, so that the
    // match's first frame finds them and no page is made at a frame.
    make_full_match_pages();

    loaded_commander_types_.clear();
    unit_definitions_.clear();
    runtime_definition_metadata_.clear();
    offline_type_fields_.clear();
    spawn_types_.clear();
    spawn_type_names_.clear();
    spawn_types_.push_back({});
    spawn_type_names_.emplace_back();
    loaded_commander_types_.push_back({});
    // The unit headers, one per units/*.FBI. A mission keeps the types its
    // use-only file lists and a session those its players agreed on; the
    // table then drops the rest and sorts the kept ones by unit name, which
    // numbers them.
    if (!oa::data::defs::load_move_classes(&files, &unit_table_.move_classes, nullptr, nullptr))
        throw std::runtime_error("Can't load MOVEINFO.TDF");
    const auto& weapon_defs = weapon_registry_.records();
    oa::data::defs::WeaponTdfSet weapon_files{};
    if (!oa::data::defs::load_weapon_tdf_set(&files, nullptr, false, &weapon_files))
        throw std::runtime_error("cannot list the weapon files");
    const std::
        unique_ptr<oa::data::defs::WeaponTdfSet, void (*)(oa::data::defs::WeaponTdfSet*) noexcept>
            weapon_files_owner(&weapon_files, oa::data::defs::weapon_tdf_set_free);
    const oa::data::defs::UnitHeaderSources header_sources{
        // The build version unit files are checked against: the game's
        // network version, kept in the data layout rather than in
        // Game.version_block.
        "",
        &weapon_files,
        oa::data::defs::data_layout().build_version[0],
        oa::data::defs::data_layout().build_version[1],
        false,
        false,
        // Each unit's name and description in other languages, for what
        // players see; the records keep Name and Description.
        unit_text_sink()
    };
    std::vector<std::string> unit_files;
    files.list(
        files.context,
        oa::data::defs::directory_name(oa::data::defs::DataDirectory::units),
        oa::data::defs::unit_extension(),
        [](void* user, const char* name) {
            static_cast<std::vector<std::string>*>(user)->emplace_back(name);
        },
        &unit_files
    );
    auto& unit_table = unit_table_.tables;
    if (!oa::data::defs::unit_def_tables_allocate(
            &unit_table, static_cast<uint32_t>(unit_files.size() + 1U)
        ))
        throw std::runtime_error("unit catalog exceeds the unit table");
    // The category masks hold as many type ids as the game's limits say.
    if (!oa::data::defs::category_registry_set_mask_types(
            &unit_table.categories, limits_.category_masks.types
        ))
        throw std::runtime_error("the category masks cannot hold the game's unit types");
    for (std::size_t index = 0; index < unit_files.size(); ++index) {
        char fbi_path[oa::data::defs::path_capacity];
        oa::data::defs::build_variant_path(
            &files,
            fbi_path,
            sizeof fbi_path,
            oa::data::defs::directory_name(oa::data::defs::DataDirectory::units),
            unit_files[index].c_str(),
            oa::data::defs::unit_extension(),
            nullptr
        );
        bool refused = false;
        if (!oa::data::defs::load_unit_header(
                &files, fbi_path, unit_table.records[index + 1U], header_sources, &refused
            ))
            throw std::runtime_error("cannot load unit header " + std::string(fbi_path));
    }
    if (campaign_mission_)
        mark_campaign_units(unit_table.records, unit_table.count);
    if (bootstrap.unit_filter.mark_units != nullptr)
        bootstrap.unit_filter.mark_units(
            bootstrap.unit_filter.context, unit_table.records, unit_table.count
        );
    unit_table.count =
        oa::data::defs::unit_defs_finalize_catalog(unit_table.records, unit_table.count);
    const std::size_t unit_count = unit_table.count != 0 ? unit_table.count - 1U : 0U;
    // Each kept unit's FBI, by unit name, against Game.weapon_defs, the
    // movement classes and the sound categories.
    CorpseFeatures corpses{feature_table_, feature_documents.value, &feature_host, {}};
    const oa::data::defs::UnitDefLoadHost corpse_host{&corpses, CorpseFeatures::load};
    // Which units get a yard map follows the mod profile's unit rules.
    const auto yard_maps = mod_profile() != nullptr
                               ? oa::data::defs::yard_map_rules(mod_profile()->rules.units)
                               : oa::data::defs::YardMapRules{};
    const oa::data::defs::UnitDefSources unit_sources{
        "",
        &unit_table_.move_classes,
        weapon_defs.data(),
        &unit_table_.sound_categories,
        &unit_table.categories,
        &unit_table.blocks,
        &corpse_host,
        yard_maps,
        unit_text_sink()
    };
    const oa::data::unit_definitions::UnitDefinitionSources definition_sources{
        &unit_table_.move_classes,
        weapon_defs.data(),
        &unit_table_.sound_categories,
        &unit_table.categories
    };
    // The rule and preview keys the mod profile binds, read from each kept
    // unit's FBI; nothing is read when it binds none.
    const auto& data_keys = data_key_bindings(mod_profile());
    const oa::data::defs::UnitDataKeys unit_keys = bound_unit_keys(data_keys);
    const oa::data::defs::UnitPreviewDataKeys preview_keys = bound_unit_preview_keys(data_keys);
    unit_type_rules_.assign(unit_keys.any() ? unit_count + 1U : 0U, {});
    unit_preview_keys_.assign(preview_keys.any() ? unit_count + 1U : 0U, {});
    loaded_commander_types_.reserve(unit_count + 1U);
    unit_definitions_.reserve(unit_count);
    runtime_definition_metadata_.reserve(unit_count);
    spawn_types_.reserve(unit_count + 1);
    spawn_type_names_.reserve(unit_count + 1);
    set_load_progress(2, 5);
    for (std::size_t type_id = 1; type_id <= unit_count; ++type_id) {
        oa::UnitDef& record = unit_table.records[type_id];
        char fbi_path[oa::data::defs::path_capacity];
        oa::data::defs::build_variant_path(
            &files,
            fbi_path,
            sizeof fbi_path,
            oa::data::defs::directory_name(oa::data::defs::DataDirectory::units),
            record.unit_name,
            oa::data::defs::unit_extension(),
            nullptr
        );
        if (!oa::data::defs::load_unit_def(&files, fbi_path, record, unit_sources))
            throw std::runtime_error("cannot load unit definition " + std::string(fbi_path));
        if (!corpses.error.empty())
            throw std::runtime_error(corpses.error);
        if (!unit_type_rules_.empty() || !unit_preview_keys_.empty()) {
            oa::data::match_rules::UnitTypeRules rule_data{};
            oa::data::defs::UnitPreviewKeys preview{};
            oa::data::defs::RuleKeyIssues issues{};
            if (!oa::data::defs::load_unit_rule_keys(
                    &files, fbi_path, unit_keys, preview_keys, rule_data, &preview, &issues
                ))
                throw std::runtime_error("cannot load unit definition " + std::string(fbi_path));
            report_rule_key_issues(fbi_path, unit_keys, issues);
            if (!unit_type_rules_.empty())
                unit_type_rules_[type_id] = rule_data;
            if (!unit_preview_keys_.empty())
                unit_preview_keys_[type_id] = preview;
        }
        auto definition =
            oa::data::unit_definitions::unit_definition_from(record, definition_sources);
        auto metadata = oa::data::unit_definitions::resolve_runtime_metadata(
            record, unit_table_.move_classes, unit_table.blocks, yard_maps
        );
        if (!metadata)
            throw std::runtime_error(
                "cannot resolve runtime metadata for '" + definition.unit_name +
                "': " + metadata.error.message
            );
        oa::sim::unit_spawn::RuntimeBindings bindings;
        bindings.enabled = true;
        const auto weapon_binding = oa::sim::combat_state::bind_unit_weapons(
            weapon_registry_, {definition.weapon1, definition.weapon2, definition.weapon3}
        );
        bindings.resolved_weapon_present = weapon_binding.resolved_nondefault_weapon;
        bindings.default_mission = static_cast<uint8_t>(record.default_mission_type);
        bindings.movement_footprint =
            std::array<int16_t, 2>{metadata.value.footprint_x, metadata.value.footprint_z};
        auto loaded = oa::sim::unit_spawn::load_runtime_type(definition, bindings, *this);
        if (!loaded.load_error.empty())
            throw std::runtime_error(loaded.load_error);
        // A script file that cannot be read stops neither its type nor the
        // load: the unit plays without a script, as one whose file is absent.
        if (loaded.script_error.code != oa::base::bytes::DecodeCode::none)
            std::cerr << "open-annihilation: invalid unit script " << loaded.script_path << ": "
                      << loaded.script_error.message << "; " << loaded.unit_name
                      << " plays without a script\n";
        loaded_commander_types_.push_back(std::move(loaded));
        // After the FBI: the model's height, the GUI page count and
        // the page-zero bit; the COB stays with the runtime type.
        const auto& loaded_type = loaded_commander_types_.back();
        record.bounds_min_y = 0;
        record.model_height = oa::ui::decoded::require(
            oa::formats::objects3d::maximum_height_fixed(*loaded_type.model), loaded_type.model_path
        );
        record.size_y = record.model_height - record.bounds_min_y;
        record.flags = (record.flags & ~OA_UNIT_DEF_FLAG_BUILD_MENU_DEFAULT) |
                       (loaded_type.type.simulation.flags & OA_UNIT_DEF_FLAG_BUILD_MENU_DEFAULT);
        record.gui_page_count = loaded_type.type.gui_page_count;
        unit_definitions_.push_back(std::move(definition));
        runtime_definition_metadata_.push_back(std::move(metadata.value));
        spawn_types_.push_back(loaded_commander_types_.back().type);
        spawn_type_names_.push_back(loaded_commander_types_.back().unit_name);
        if (type_id % 20 == 0)
            set_load_progress(2, static_cast<uint8_t>(type_id * 100 / unit_count));
    }
    // The headers loaded above are the unit headers; the kept types take their
    // agreed limits (UnitDef.player_limit) from the verdicts.
    if (bootstrap.unit_filter.mark_units != nullptr)
        bootstrap.unit_filter.mark_units(
            bootstrap.unit_filter.context, unit_table.records, unit_table.count
        );
    // The CANBUILD pass, then the download menus.
    if (!oa::data::defs::load_build_lists(&files, nullptr, &unit_table, limits_.build_lists))
        throw std::runtime_error("Can't load GAMEDATA.TDF");
    if (!oa::data::defs::load_download_menu(&files, nullptr, &unit_table, limits_.build_lists))
        throw std::runtime_error("cannot load the download menus");
    // The header table is marked stale once the full load is done; the
    // frontend reads its unit headers again when it next runs.
    frontend_game().unit_defs_stale = 1;
    set_load_progress(2, 100);
    set_load_progress(4, 20);
    // The target-category masks, once every type has joined its categories.
    unit_target_masks_.assign(loaded_commander_types_.size(), {});
    offline_type_fields_.resize(loaded_commander_types_.size());
    for (std::size_t index = 1; index < loaded_commander_types_.size(); ++index) {
        const auto& metadata = runtime_definition_metadata_[index - 1U];
        auto& fields = offline_type_fields_[index];
        unit_target_masks_[index] = oa::data::unit_definitions::target_category_masks(
            unit_table.records[index], unit_table.categories
        );
        fields.definition = &unit_definitions_[index - 1U];
        fields.yard_mask = metadata.yard_cells;
        fields.runtime_metadata = &metadata;
        fields.target_masks = &unit_target_masks_[index];
        // UnitDef.move_class refers to the movement class, so class 0 (KBOTSS2)
        // is nonzero too; the handle is the class index + 1 and 0 means none.
        if (metadata.movement_class_handle)
            fields.movement_class =
                static_cast<oa::sim::unit_spawn::AssetHandle>(*metadata.movement_class_handle) + 1U;
        else
            fields.movement_class = oa::sim::unit_spawn::AssetHandle{0};
        fields.corpse_feature = unit_table.records[index].corpse;
    }
    // Featuredead/reclamate/burnt links resolve once the units have
    // added their corpses, appending any remnant the table still lacks.
    if (const auto error = oa::sim::map_runtime::load_feature_links(
            feature_table_, feature_documents.value, &feature_host
        ))
        throw std::runtime_error("cannot link feature definitions: " + error->message);
    set_load_progress(3, 20);
    match_features_.clear();
    match_gaf_features_.clear();
    match_gaf_anims_.clear();
    match_gaf_anim_index_.clear();
    gaf_feature_anim_tick_ = 0;
    wrecks_drawn_ = 0;
    {
        auto catalog = oa::sim::map_runtime::load_feature_catalog(feature_documents.value);
        if (!catalog.ok())
            throw std::runtime_error("cannot load feature catalog: " + catalog.error->message);
        feature_catalog_ = std::move(*catalog.value);
    }

    // A sprite feature's sequence's pixels are decoded from its GAF file
    // (feature_gaf_file) once, for the animation the first feature showing it
    // adds.
    // Each 3DO model the map's object features name, loaded once and shared.
    std::unordered_map<std::string, std::shared_ptr<const oa::formats::objects3d::Model>>
        feature_models;
    for (const auto& placed : prepared_map_->placed_features) {
        const auto world_x = (placed.cell_x * 16 + static_cast<int32_t>(placed.footprint_x) * 8)
                             << 16;
        const auto world_z = (placed.cell_z * 16 + static_cast<int32_t>(placed.footprint_z) * 8)
                             << 16;
        const oa::formats::objects3d::FixedVector3 position{
            world_x, static_cast<int32_t>(placed.height) << 16, world_z
        };
        uint16_t feature_word = 0xffff;
        if (placed.cell_x >= 0 && placed.cell_z >= 0) {
            const auto plot =
                static_cast<std::size_t>(placed.cell_z) * selected_tnt_->attribute_width +
                static_cast<std::size_t>(placed.cell_x);
            if (plot < prepared_map_->collision_plots.size())
                feature_word = prepared_map_->collision_plots[plot].feature_word;
        }
        if (!placed.object.empty()) {
            try {
                auto& model = feature_models[placed.object];
                if (!model) {
                    const auto path = "objects3d/" + placed.object + ".3DO";
                    const auto bytes = assets_.read(path).bytes;
                    model = std::make_shared<const oa::formats::objects3d::Model>(
                        oa::ui::decoded::require(
                            oa::formats::objects3d::load_3do(std::as_bytes(std::span(bytes))), path
                        )
                    );
                }
                MatchFeatureDraw draw;
                draw.instance = oa::sim::model_runtime::make_instance(model);
                draw.position = position;
                draw.cell_x = placed.cell_x;
                draw.cell_z = placed.cell_z;
                draw.feature_index = feature_word;
                match_features_.push_back(std::move(draw));
            } catch (const std::exception& error) {
                std::cerr << "feature model '" << placed.object << "' unavailable: " << error.what()
                          << '\n';
            }
            continue;
        }
        if (placed.filename.empty() || placed.seqname.empty())
            continue;
        try {
            std::string parse_error;
            const auto gaf = feature_gaf_file(placed.filename, parse_error);
            if (gaf.archive == nullptr) {
                std::cerr << "feature GAF 'anims/" << placed.filename
                          << ".gaf' parse failed: " << parse_error << '\n';
                continue;
            }
            const auto* sequence = gaf_sequence(*gaf.archive, placed.seqname);
            if (sequence == nullptr || sequence->frames.empty())
                continue;
            auto anim = static_cast<std::size_t>(-1);
            if (const auto found =
                    match_gaf_anim_index_.find(placed.filename + "/" + placed.seqname);
                found != match_gaf_anim_index_.end())
                anim = found->second;
            else {
                auto decoded = oa::formats::gaf::parse_sequence(
                    gaf.file, static_cast<std::size_t>(sequence - gaf.archive->sequences.data())
                );
                if (!decoded.ok())
                    continue;
                anim = intern_gaf_feature_anim(
                    placed.filename, placed.seqname, *decoded.sequence, placed.animating
                );
            }
            if (anim == static_cast<std::size_t>(-1))
                continue;
            match_gaf_features_.push_back(
                {anim,
                 position,
                 placed.cell_x,
                 placed.cell_z,
                 feature_word,
                 intern_feature_shadow_anim(feature_word, placed.filename, placed.animating)}
            );
        } catch (const std::exception& error) {
            std::cerr << "feature sprite '" << placed.filename << "/" << placed.seqname
                      << "' unavailable: " << error.what() << '\n';
        }
    }
    if (match_)
        gaf_feature_anim_tick_ = match_->simulation().tick;
    std::vector<uint8_t> terrain_heights;
    terrain_heights.reserve(selected_tnt_->attributes.size());
    for (const auto& attribute : selected_tnt_->attributes)
        terrain_heights.push_back(attribute.height);
    altitude_cells_ = oa::sim::visibility_state::build_altitude_cells(
        terrain_heights,
        static_cast<int32_t>(selected_tnt_->attribute_width),
        static_cast<int32_t>(selected_tnt_->attribute_height),
        static_cast<uint8_t>(selected_tnt_->sea_level)
    );

    // The terrain load reads the line-of-sight ray tables.
    LosTables los_tables{altitude_patterns_, {}};
    if (!oa::data::defs::load_gamedata_tables(
            &files, nullptr, {&los_tables, LosTables::resize, LosTables::table}
        ))
        throw std::runtime_error(
            los_tables.error.empty() ? std::string("cannot load gamedata/los.tdf")
                                     : los_tables.error
        );
    // The prepared map's collision plots are let go once copied here, and
    // this copy once the match, which copies them while it is constructed,
    // holds its own.
    std::vector<oa::sim::spatial_state::Plot> collision_plots(
        prepared_map_->collision_plots.size()
    );
    for (std::size_t index = 0; index < collision_plots.size(); ++index) {
        const auto& source = prepared_map_->collision_plots[index];
        collision_plots[index].high_height = source.high_height;
        collision_plots[index].low_height = source.low_height;
        collision_plots[index].blocking_feature = source.blocking_feature;
        collision_plots[index].metal_feature = source.metal_feature;
        collision_plots[index].geo_feature = source.geo_feature;
        collision_plots[index].indestructible_feature = source.indestructible_feature;
        collision_plots[index].metal = source.metal;
        collision_plots[index].feature_word = source.feature_word;
        collision_plots[index].feature_back_x = source.feature_back_x;
        collision_plots[index].feature_back_z = source.feature_back_z;
        collision_plots[index].feature_height = source.feature_height;
        collision_plots[index].feature_footprint_x = source.feature_footprint_x;
        collision_plots[index].feature_footprint_z = source.feature_footprint_z;
    }
    std::vector<oa::sim::map_runtime::CollisionPlot>().swap(prepared_map_->collision_plots);
    const auto counter =
        static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
    const auto random_seed = options_.seed.value_or(
        options_.fixed_clock
            ? kFixedRandomSeed
            : static_cast<uint32_t>(counter) + static_cast<uint32_t>(counter >> 32U)
    );
    // A campaign plays under the rules block its mission loader wrote, a
    // skirmish under its own settings; the session flags apply either.
    int32_t session_record[4] = {
        static_cast<int32_t>(preferences_.skirmish.commander_death),
        static_cast<int32_t>(preferences_.skirmish.mapping),
        static_cast<int32_t>(preferences_.skirmish.line_of_sight),
        static_cast<int32_t>(preferences_.skirmish.los_type)
    };
    if (campaign_mission_)
        campaign_session_rules(session_record);
    const auto visibility_flags = static_cast<uint16_t>(
        (session_record[1] & 1) | ((session_record[2] & 1) << 1) | ((session_record[3] & 1) << 2)
    );
    uint8_t local_player = 0;
    for (std::size_t index = 0; index < skirmish_settings_.slots.size(); ++index)
        if (skirmish_settings_.slots[index].controller == entry::controller::human)
            local_player = static_cast<uint8_t>(index);
    match_local_player_ = local_player;
    oa::sim::match_runtime::OfflineInputs inputs{
        *selected_tnt_,
        loaded_commander_types_,
        spawn_types_,
        offline_type_fields_,
        weapon_registry_,
        prepared_map_->terrain_values,
        prepared_map_->sight_masks,
        prepared_map_->sight_width,
        prepared_map_->sight_height,
        bootstrap.units_per_player,
        visibility_flags,
        local_player,
        30,
        random_seed,
        this,
        [this] { return clock_milliseconds(); },
        collision_plots,
        oa::sim::visibility_state::AltitudeSightData{
            prepared_map_->sight_width,
            prepared_map_->sight_height,
            altitude_cells_,
            altitude_patterns_
        },
        integer("MinWindSpeed", 0),
        integer("MaxWindSpeed", 0),
        static_cast<float>(integer("TidalStrength", 0)),
        feature_table_.defs
    };
    inputs.unit_defs = {unit_table.records, unit_table.count};
    inputs.limits = limits_;
    // The display rules as they are now; Developer Mode may change them
    // while the match runs.
    inputs.display = view_rules::match_display_rules(ui_rules());
    if (const auto* profile = mod_profile()) {
        inputs.rules = profile->rules;
        // The player's own options for patrolling and guarding builders.
        view_rules::apply_builder_options(
            view_settings_, inputs.rules.orders.con_patrol_guard_options
        );
        inputs.profile_sim_hash = profile->sim_hash;
    }
    // Records for another type table than this one are not this match's.
    if (unit_type_rules_.size() == spawn_types_.size())
        inputs.unit_type_rules = unit_type_rules_;
    inputs.weapon_rules = weapon_rules_;
    inputs.mission_features = mission_features_;
    inputs.resuming_saved_game = resuming_saved_game();
    inputs.effect_sequence = [this](std::string_view archive, std::string_view entry) {
        return explosion_sequence(archive, entry);
    };
    inputs.feature_sequence_frame =
        [this](
            oa_ref32 sequence, uint16_t frame, oa::sim::feature_runtime::FeatureSequenceFrame& out
        ) { return feature_sequence_frame(sequence, frame, out); };
    inputs.loaded_primitives = [this](
                                   const oa::formats::objects3d::Model& model,
                                   uint32_t object,
                                   std::vector<oa::sim::effect_particles::PiecePrimitive>& out
                               ) { return loaded_match_primitives(model, object, out); };
    // The session start loads FX.GAF before the mission places
    // the map's features, so a geothermal vent smokes from the first tick.
    if (match_fx_.sequences.empty())
        append_gaf_file(match_fx_, "anims/FX.GAF");
    // The explosion files the weapons name are read and checked before the
    // match starts, so that the first of each explosion does not wait for
    // its file and a damaged one is reported now; their sequences are
    // decoded as explosions first ask for them.
    for (std::size_t index = 0; index < oa::sim::combat_state::weapon_registry_capacity; ++index) {
        const auto& weapon = weapon_registry_.definition(static_cast<uint8_t>(index));
        for (const auto* name : {&weapon.explosion_gaf, &weapon.water_explosion_gaf})
            if (!name->empty())
                load_explosion_gaf(*name);
    }
    try {
        if (const char* refused = oa::sim::match_runtime::Match::input_error(inputs))
            throw std::runtime_error(std::string("cannot start the match: ") + refused);
        match_ = std::make_unique<oa::sim::match_runtime::Match>(inputs, offline_services_);
        // A next-game unit limit an extension set belongs to the next new
        // game: a match at the run's limit, or a multiplayer game, which a
        // joined one ends though it plays at its host's limit. A match that
        // brings its own limit leaves it for the game that asked for it.
        const bool new_game = bootstrap.run_unit_limit || bootstrap.multiplayer;
        unsaved_unit_limit_playing_ = new_game && unsaved_unit_limit_pending_;
        if (new_game)
            unsaved_unit_limit_pending_ = false;
        game_speed_lock_.reset();
        raise_match_fault(*match_);
        std::vector<oa::sim::spatial_state::Plot>().swap(collision_plots);
        bind_match_speech();
        match_->set_difficulty(static_cast<int32_t>(preferences_.difficulty));
        // The mission starts with the top bar's shown stores and the space-bar
        // strip cleared; the strip's LIGHTBAR picture is bound again.
        oa::ui::hud::reset_status_panel(match_->state().game);
        status_lightbar_.reset();
        status_lightbar_loaded_ = false;
        // A campaign's block stays in Game.session_record where its loader
        // wrote it, and so does the mission's nomovie flag.
        if (campaign_mission_) {
            static_assert(sizeof session_record == sizeof match_->state().game.session_record);
            std::memcpy(match_->state().game.session_record, session_record, sizeof session_record);
            match_->state().game.no_movie = campaign_object().no_movie;
        }
        oa::sim::session::apply_session_flags(&match_->state().game, session_record);
        // The option word the settings load filled persists into the match,
        // less the per-match toggles the match loader clears; its tree-death bit lets weapons
        // damage features.
        oa::ui::console::set_console_flags(
            match_->state().game,
            static_cast<uint16_t>(preferences_.display_flags & ~match_cleared_console_flags)
        );
        seed_match_options(match_->state().game);
    } catch (const std::exception& error) {
        if (std::string_view(error.what()) != "altitude sight data is required")
            throw;
        altitude_sight_blocked_ = true;
        status_ = std::string("Offline match setup blocked: ") + error.what();
        std::cerr << "unsupported operation: " << status_ << '\n';
        return;
    }
    {
        // UnitDef.unit_name is the FBI UnitName that savegame unit
        // records and the console's spawn patterns name a type by; the spawn
        // command walks Game.unit_def_count types.
        oa::World& world = match_->state();
        for (std::size_t type = 1; type < world.unit_def_count && type < spawn_type_names_.size();
             ++type)
            std::snprintf(
                world.unit_defs[type].unit_name,
                sizeof world.unit_defs[type].unit_name,
                "%s",
                spawn_type_names_[type].c_str()
            );
        world.game.unit_def_count = static_cast<int32_t>(world.unit_def_count);
        // A skirmish starts at the configured unit limit (Game.max_units_setting); a
        // save's Summary writes it back as "maxunits". A campaign plays at
        // its mission's limit and keeps the run's limit in the setting.
        world.game.max_units_setting = campaign_mission_
                                           ? EngineSettingsState::run_unit_limit(*this)
                                           : bootstrap.units_per_player;
        bind_session_options();
        // Game.player_count, the players Start counted (two in a campaign): the
        // kills board has a row for each.
        world.game.player_count = state_.player_count;
        // The command-line fields of the Game block;
        // the extension writes the ones its launch switches set.
        world.game.player_timeout_seconds = kPlayerTimeoutSeconds;
        world.game.send_error_percent = 0;
        call_hook_or_raise<&Extension::match_game>(extension_, world.game);
        world.game.setup_options |= options_.launch.game_options;
        bind_player_records(world);
        if (bootstrap.seat_roster)
            seat_skirmish_roster(world);
        else if (campaign_mission_)
            seat_campaign_players(world);
    }
    place_mission_feature_draws();
    match_->point_sound = {
        this,
        [](void* context) { return static_cast<Runtime*>(context)->sound_spatial_ != 0; },
        [](
            void* context, const char* name, const oa::sim::match_runtime::Match::PointSound& sound
        ) { static_cast<Runtime*>(context)->play_point_sound(name, sound); }
    };
    // The panel dropped to its root page is reloaded for the selection by
    // the next panel update; the runtime rebuilds it at once.
    match_->order_panel = {this, [](void* context) {
                               static_cast<Runtime*>(context)->apply_match_hud_for_selection();
                           }};
    // Sound-table names the match plays unplaced, like the frontend's, and
    // the files they name for the ones it plays at a point.
    match_->named_sound = {
        this,
        [](void* context, const char* name) {
            static_cast<Runtime*>(context)->play_ui_sound(name, 0);
        },
        [](void* context, const char* name) -> const char* {
            const auto& registry = static_cast<Runtime*>(context)->audio_registry_;
            const auto* sound = registry.get(registry.find(name));
            return sound != nullptr ? sound->file.c_str() : nullptr;
        }
    };
    bind_respawn_view();
    // The observer pulse pauses while shift is held (asked of the keyboard);
    // the selection it drops takes the runtime's primary unit
    // with it.
    match_->observer = {
        this,
        [](void* context) {
            return static_cast<Runtime*>(context)->control_key_down(
                oa::ui::gui_input::ControlKey::shift
            );
        },
        [](void* context) {
            auto& self = *static_cast<Runtime*>(context);
            self.selected_match_unit_ = 0;
            self.apply_match_hud_for_selection();
        },
    };
    bind_message_log();
    reset_meteors();
    match_->meteor = {this, [](void* context) { static_cast<Runtime*>(context)->step_meteors(); }};
    match_->profile.context = this;
    match_->profile.mark = [](void* context, int32_t category) {
        static_cast<Runtime*>(context)->mark_profile(category);
    };
    // The console binds to the new match here, even over a world at the old
    // one's address, so its host (with the extension's part) is filled before
    // the match posts its first line; the binding restores the carried
    // console values.
    if (console_)
        console_->bound_world = nullptr;
    // Only the binding is wanted here; the console is not used yet.
    std::ignore = match_console();
    effect_boundary_.clock = &match_->state().game;
    set_load_progress(3, 100);
    set_load_progress(4, 70);
    offline_effects_.bind(*match_);
    offline_services_.bind_effects(offline_effects_);
    // The novelty voice plays the mod profile's two sounds
    // (strings.cheat.sing-sounds), or 3.1c's.
    const auto* profile = mod_profile();
    novelty_sounds_ = profile != nullptr ? profile->strings.cheat.sing_sounds
                                         : oa::data::mod_profile::StringsCheat{}.sing_sounds;
    offline_services_.bind_announcements(
        unit_sound_catalog_,
        *match_,
        spawn_types_,
        unit_definitions_,
        {preferences_.unit_chat,
         preferences_.unit_chat_text,
         !options_.mute,
         (preferences_.sound_flags & init::preference_flags::speech_fx) != 0,
         true,
         novelty_voice_ != 0,
         {novelty_sounds_[0], novelty_sounds_[1]}}
    );
    for (std::size_t player = 0; player < skirmish_settings_.slots.size(); ++player) {
        const auto controller = skirmish_settings_.slots[player].controller;
        if (controller != entry::controller::disabled && controller != entry::controller::human &&
            controller != entry::controller::computer)
            throw std::runtime_error("skirmish player has invalid controller state");
        // The settings block's eleventh slot record has no player of the
        // ten to describe.
        if (bootstrap.seat_roster || player >= match_->simulation().players.size())
            continue;
        auto& simulation_player = match_->simulation().players[player];
        simulation_player.present = controller != entry::controller::disabled;
        simulation_player.status = static_cast<uint8_t>(controller);
    }
    std::array<uint8_t, 10> local_allies{};
    const auto local_alliance = skirmish_settings_.slots[local_player].alliance;
    for (std::size_t index = 0; index < local_allies.size(); ++index) {
        const auto& candidate = skirmish_settings_.slots[index];
        local_allies[index] = static_cast<uint8_t>(
            index == local_player ||
            (candidate.controller != entry::controller::disabled &&
             candidate.alliance == local_alliance && candidate.alliance != 5)
        );
    }
    match_->configure_outcomes(
        local_player,
        local_allies,
        bootstrap.defeat_allowed,
        campaign_mission_,
        bootstrap.multiplayer
    );
    // Only the ten players have alliances; the eleventh slot record has no
    // player, even when a restored or received block enables it.
    for (std::size_t player = 0; player < std::size(match_->state().game.players); ++player) {
        const auto& owner = skirmish_settings_.slots[player];
        if (owner.controller == entry::controller::disabled)
            continue;
        std::array<uint8_t, 10> allies{};
        for (std::size_t candidate_index = 0; candidate_index < allies.size(); ++candidate_index) {
            const auto& candidate = skirmish_settings_.slots[candidate_index];
            allies[candidate_index] = static_cast<uint8_t>(
                candidate_index == player ||
                (candidate.controller != entry::controller::disabled &&
                 candidate.alliance == owner.alliance && owner.alliance != 5)
            );
        }
        match_->configure_player_alliances(static_cast<uint8_t>(player), allies);
        std::copy(allies.begin(), allies.end(), match_->state().game.players[player].alliance);
    }
    match_timing_ = {};
    match_timing_.requested_rate = preferences_.game_speed;
    match_timing_.actual_rate = preferences_.current_game_speed;
    // The match start resets a multiplayer session to the normal
    // speed in the preference words themselves.
    oa::base::game_loop::reset_mode_timing(match_timing_, bootstrap.multiplayer);
    preferences_.game_speed = match_timing_.requested_rate;
    preferences_.current_game_speed = match_timing_.actual_rate;
    // The speed words are Game.requested_speed and Game.current_speed; a
    // speed change compares against them before it posts its line.
    match_->state().game.requested_speed = match_timing_.requested_rate;
    match_->state().game.current_speed = match_timing_.actual_rate;
    match_timing_.previous_clock =
        oa::base::game_loop::scaled_clock(clock_milliseconds(), match_clock_scale());
    match_tick_blocked_ = false;
    configure_computer_players();
    begin_map_units(resuming_saved_game());
    std::size_t started_players = 0;
    if (campaign_mission_) {
        // Mission start rebuilds the sight grids before the mission's units stand.
        reset_match_sight(true);
        // A mission resumed from a savegame restores it instead.
        if (!resume_saved_mission())
            spawn_campaign_units();
        started_players = 0;
        for (const auto& slot : match_->world().slots)
            if (slot.unit != nullptr && slot.unit->type_index)
                ++started_players;
    } else if (bootstrap.place_commanders) {
        // The start position of each player, as the commanders are placed.
        std::array<int32_t, 10> start_positions{};
        start_positions.fill(-1);
        for (std::size_t player = 0;
             player < skirmish_settings_.slots.size() && player < start_positions.size();
             ++player)
            if (skirmish_settings_.slots[player].controller != 0)
                start_positions[player] = static_cast<int32_t>(player);
        for (std::size_t player = 0; player < skirmish_settings_.slots.size(); ++player) {
            const auto& slot = skirmish_settings_.slots[player];
            if (slot.controller == 0)
                continue;
            // A position an earlier move swapped is read from the table.
            int32_t start_index = player < start_positions.size() ? start_positions[player]
                                                                  : static_cast<int32_t>(player);
            // setup.map-scripted-units: the map's units for this start
            // position take the commander's place; before the commander is
            // placed the computer player moves last on a neutral map.
            if (place_map_units(static_cast<uint8_t>(player), start_index)) {
                ++started_players;
                continue;
            }
            if (player < start_positions.size()) {
                move_map_unit_computer_last(start_positions, start_index);
                if (place_map_units(static_cast<uint8_t>(player), start_index)) {
                    ++started_players;
                    continue;
                }
            }
            oa::sim::unit_spawn::PlayerSetup setup{
                static_cast<uint8_t>(slot.side),
                static_cast<uint8_t>(slot.color),
                slot.metal,
                slot.energy
            };
            oa::sim::unit_spawn::StartResult started;
            try {
                started = match_->start_player(
                    static_cast<uint8_t>(player),
                    setup,
                    selected_start_markers_,
                    start_index,
                    kBattlefieldWidth,
                    kBattlefieldHeight,
                    *this
                );
            } catch (const std::exception& error) {
                const std::string_view message(error.what());
                if (message != "the alternate altitude sight algorithm is not implemented" &&
                    message != "altitude sight data is required")
                    throw;
                altitude_sight_blocked_ = true;
                std::cerr << "unsupported operation: " << error.what() << '\n';
                break;
            }
            if (!started.position_found || started.unit == nullptr)
                throw std::runtime_error("offline commander start failed");
            ++started_players;
        }
        // Mission start seeds the stores once every commander is placed.
        std::array<oa::ui::hud::SkirmishSlot, OA_PLAYER_COUNT> seeds{};
        for (std::size_t player = 0;
             player < seeds.size() && player < skirmish_settings_.slots.size();
             ++player) {
            seeds[player].metal = skirmish_settings_.slots[player].metal;
            seeds[player].energy = skirmish_settings_.slots[player].energy;
        }
        oa::ui::hud::set_starting_resources(
            match_->state(), oa::data::campaign::SessionKind::skirmish, nullptr, seeds.data()
        );
        // Mission start rebuilds the sight grids once every commander stands.
        reset_match_sight(true);
    }
    // No commander is required by name: each player's is the one SIDEDATA
    // names for its side (commander_type_for_side).
    set_load_progress(4, 100);
    set_load_progress(5, 100);
    if (!options_.trace_digest.empty() &&
        !match_->record_trace(options_.trace_digest.string(), options_.trace_units.string()))
        throw std::runtime_error(
            "cannot create the trace stream " + options_.trace_digest.string() +
            (options_.trace_units.empty() ? "" : " or " + options_.trace_units.string())
        );
    // The path search's credit: the setting's in a game played alone; a
    // shared game or a replay plays at the base credit on every machine.
    EngineSettingsState::start_path_credit(
        *this,
        bootstrap.multiplayer || (current_extension_state() &
                                  (extension_state::shared_match | extension_state::replay)) != 0
    );
    status_ = "Offline match world prepared for " + selected_map_name_runtime_ + " with " +
              std::to_string(unit_definitions_.size()) + " unit runtimes, " +
              std::to_string(feature_table_.defs.size()) + " feature definitions and " +
              std::to_string(started_players) +
              (campaign_mission_ ? " placed units" : " commanders") +
              (altitude_sight_blocked_ ? "; altitude sight blocks match entry." : ".");
    std::cerr << status_ << '\n';
}

} // namespace oa::app
