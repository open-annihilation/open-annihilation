// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Savegames of the running match: the Summary, Players and unit sections of
// src/data/persist and the HUD over the canonical World, the start of a skirmish
// or campaign mission from a savegame, and the match's meteor-storm state
// they carry.
#include "engine_settings_state.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/core/map_plot.h"
#include "oa/app/runtime.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/mod_profile_loader.hpp"
#include "oa/base/bytes.hpp"

#include "oa/data/campaign/campaign_file.hpp"
#include "oa/formats/cob.hpp"
#include "oa/sim/ground_orders/ground_runtime.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/world_environment/meteor.hpp"
#include "oa/data/mission_types.hpp"
#include "oa/sim/scenario/condition_persist.hpp"
#include "oa/data/persist/hapibank.hpp"
#include "oa/data/mod_profile/registry.hpp"
#include "oa/data/persist/save_profile.hpp"
#include "oa/data/persist/save_sections.hpp"
#include "oa/sim/feature_runtime.hpp"
#include "oa/sim/script_state.hpp"
#include "oa/sim/session.hpp"
#include "oa/sim/state_hash.hpp"
#include "oa/sim/unit_health/veterancy.hpp"
#include "oa/sim/unit_spawn/spawn.hpp"
#include "oa/sim/trace.hpp"
#include "oa/ui/hud/order_panel.hpp"
#include "oa/ui/hud/player_records.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/frontend/savegame_dialogs.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace oa::app {

namespace persist = oa::data::persist;
namespace missions = oa::data::campaign;
namespace environment = oa::sim::world_environment;
namespace hud = oa::ui::hud;
namespace save_key = oa::data::persist::save_key;

namespace {
using base::bytes::load_le16;
using base::bytes::load_le32;

// The 3.1c build stamp, which fills two Summary keys ("BUILD DATE: %s",
// "BUILD TIME: %s").
constexpr const char* kGameBuildDate = "Nov 17 1999";
constexpr const char* kGameBuildTime = "11:45:48";

constexpr std::size_t kSightWordBytes = sizeof(uint16_t);
constexpr const char* kHeadlessSaveName = "headless.sav";

// World units along one side of a map cell.

// Units a save/load run's transport ship starts with in its hold. A unit
// starting aboard, as a mission's can, hangs from no piece of its carrier and
// takes movement layer 0.
constexpr int32_t kSaveloadShipCargo = 3;
constexpr int8_t kNoCarryPiece = -1;
constexpr uint8_t kStartAboardLayer = 0;

// gamedata\meteor.tdf [Default] and the map keys it stands in for.
constexpr const char* kMeteorDefaults = "meteor.tdf";
constexpr const char* kMeteorDefaultSection = "Default";
constexpr const char* kMeteorWeapon = "MeteorWeapon";
constexpr const char* kMeteorRadius = "MeteorRadius";
constexpr const char* kMeteorDensity = "MeteorDensity";
constexpr const char* kMeteorDuration = "MeteorDuration";
constexpr const char* kMeteorInterval = "MeteorInterval";

// Movement image handed to the mobility writer: the 0x23-byte block the
// ground runtime saves sits at the movement object's saved offset, its last
// byte in the flags byte.
constexpr std::size_t kMovementImageBytes = persist::movement_flags + 1;
static_assert(persist::movement_saved_bytes + 1 == oa::sim::ground_orders::mobility_record_size);

// The Features section reads and restores the feature runtime's placed-feature
// records, whose fields sit where the section's names say.
namespace placed = oa::sim::feature_runtime;
static_assert(persist::feature_record_bytes == sizeof(placed::PlacedFeature));
static_assert(persist::feature_record_bytes == OA_PLACED_FEATURE_BYTES);
static_assert(
    persist::feature_record::frame == offsetof(placed::PlacedFeature, sprite) +
                                          offsetof(placed::PlacedFeatureSprite, animation) +
                                          offsetof(placed::FeatureCursor, frame)
);
static_assert(
    persist::feature_record::sequence == offsetof(placed::PlacedFeature, sprite) +
                                             offsetof(placed::PlacedFeatureSprite, animation) +
                                             offsetof(placed::FeatureCursor, sequence)
);
static_assert(
    persist::feature_record::position ==
    offsetof(placed::PlacedFeature, model) + offsetof(placed::PlacedFeatureModel, position)
);
static_assert(persist::feature_record::position_bytes == sizeof(FixedVec3));
static_assert(persist::feature_record::orientation == offsetof(placed::PlacedFeature, orientation));
static_assert(
    persist::feature_record::orientation_bytes == sizeof(placed::PlacedFeature::orientation)
);
static_assert(persist::feature_record::damage == offsetof(placed::PlacedFeature, damage));
static_assert(
    persist::feature_record::spread_countdown == offsetof(placed::PlacedFeature, spread_countdown)
);

void store_le16(uint8_t* out, uint16_t value) noexcept {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
}

void store_le32(uint8_t* out, uint32_t value) noexcept {
    store_le16(out, static_cast<uint16_t>(value));
    store_le16(out + 2, static_cast<uint16_t>(value >> 16));
}

float load_le_float(const uint8_t* in) noexcept {
    return std::bit_cast<float>(load_le32(in));
}

/// Marks the plots a load of a save of the match hides under the map's edges
/// before it places the save's features: the edges laid over the map with its
/// blocking markers alone.
///
/// @param world the match's world, for its plots and map size
/// @param lava_world the map is a lava world
/// @return one entry per plot, nonzero where the load hides it
std::vector<uint8_t> plots_hidden_on_load(const oa::World& world, bool lava_world) {
    namespace features = oa::sim::feature_runtime;
    const int32_t map_width = world.game.map_width;
    const int32_t map_height = world.game.map_height;
    const auto cells = static_cast<std::size_t>(std::max(map_width, 0)) *
                       static_cast<std::size_t>(std::max(map_height, 0));
    std::vector<oa::MapPlot> plots(world.plots, world.plots + cells);
    for (auto& plot : plots)
        if (plot.feature != features::feature_marker)
            plot.feature = features::no_feature;
    const auto scratch = std::make_unique<oa::World>();
    scratch->game = world.game;
    scratch->plots = plots.data();
    features::void_hidden_edges(*scratch, lava_world);
    std::vector<uint8_t> hidden(cells);
    for (std::size_t index = 0; index < cells; ++index)
        hidden[index] = plots[index].feature == features::hidden_edge ? 1 : 0;
    return hidden;
}

struct BankGuard {
    persist::Bank bank{};

    BankGuard() { persist::bank_init(&bank); }

    ~BankGuard() { persist::bank_destroy(&bank); }

    BankGuard(const BankGuard&) = delete;
    BankGuard& operator=(const BankGuard&) = delete;
};

/// Reads every account of a save file into the guard's bank.
///
/// @param[out] guard bank the accounts are read into
/// @param path savegame file
/// @param[out] error why the read failed
/// @return false when the file is missing or not a save
bool read_whole_save(BankGuard& guard, const fs::path& path, persist::BankError* error) {
    const persist::FileSource source = persist::stdio_file_source();
    return persist::bank_read_file(
        &guard.bank,
        path_to_utf8(path).c_str(),
        persist::savegame_description,
        nullptr,
        &source,
        error
    );
}

// The offline match keeps the map size, camera, player count and in-match
// mode in the runtime rather than in its Game block, which the save
// sections read. This bounded scaffolding writes them
// into the Game block for one save or load and puts the previous values back;
// it goes away once the runtime keeps them in the Game block itself.
class GameBinding {
  public:

    GameBinding(
        oa::Game& game,
        const oa::formats::tnt::Map& map,
        int32_t camera_x,
        int32_t camera_z,
        uint16_t player_count
    )
        : game_(game), map_width_(game.map_width), map_height_(game.map_height),
          camera_x_(game.camera_x), camera_y_(game.camera_y), player_count_(game.player_count),
          mode_(game.mode) {
        game.map_width = static_cast<int32_t>(map.attribute_width);
        game.map_height = static_cast<int32_t>(map.attribute_height);
        game.camera_x = static_cast<uint32_t>(camera_x);
        game.camera_y = static_cast<uint32_t>(camera_z);
        game.player_count = player_count;
        game.mode = persist::game_mode_in_match;
    }

    ~GameBinding() {
        game_.map_width = map_width_;
        game_.map_height = map_height_;
        game_.camera_x = camera_x_;
        game_.camera_y = camera_y_;
        game_.player_count = player_count_;
        game_.mode = mode_;
    }

    GameBinding(const GameBinding&) = delete;
    GameBinding& operator=(const GameBinding&) = delete;

  private:

    oa::Game& game_;
    int32_t map_width_;
    int32_t map_height_;
    uint32_t camera_x_;
    uint32_t camera_y_;
    uint16_t player_count_;
    int32_t mode_;
};

// The frontend keeps the campaign's mission results and mission index
// (Game.mission_results, Game.mission_index) outside the match's Game block;
// the same bounded scaffolding binds them for one save.
class SessionRecordBinding {
  public:

    SessionRecordBinding(
        oa::Game& game,
        const std::array<uint8_t, oa::ui::frontend_state::start_pattern_bytes + 1>& results,
        std::size_t mission_index
    )
        : game_(game), mission_index_(game.mission_index) {
        std::memcpy(results_, game.mission_results, sizeof results_);
        std::memcpy(game.mission_results, results.data(), sizeof game.mission_results);
        game.mission_index = static_cast<int32_t>(mission_index);
    }

    ~SessionRecordBinding() {
        std::memcpy(game_.mission_results, results_, sizeof results_);
        game_.mission_index = mission_index_;
    }

    SessionRecordBinding(const SessionRecordBinding&) = delete;
    SessionRecordBinding& operator=(const SessionRecordBinding&) = delete;

  private:

    oa::Game& game_;
    char results_[sizeof oa::Game::mission_results];
    int32_t mission_index_;
};

static_assert(sizeof oa::Game::mission_results == oa::ui::frontend_state::start_pattern_bytes + 1);

// The coordinator clock the Players section keeps (Game.last_frame_time
// through Game.sim_run_flags)
// lives in the runtime's base::game_loop::Timing; the same bounded scaffolding.
void store_timing(oa::Game& game, const oa::base::game_loop::Timing& timing) noexcept {
    game.last_frame_time = timing.previous_clock;
    game.pending_ticks = timing.pending_steps;
    game.frame_elapsed = timing.elapsed_bits;
    game.tick_remainder = timing.remainder;
    game.tick = timing.tick;
    game.requested_speed = timing.requested_rate;
    game.current_speed = timing.actual_rate;
    game.speed_adaptation = timing.adaptation;
    game.sim_run_flags = timing.flags;
}

void load_timing(const oa::Game& game, oa::base::game_loop::Timing& timing) noexcept {
    timing.previous_clock = game.last_frame_time;
    timing.pending_steps = game.pending_ticks;
    timing.elapsed_bits = game.frame_elapsed;
    timing.remainder = game.tick_remainder;
    timing.tick = game.tick;
    timing.requested_rate = game.requested_speed;
    timing.actual_rate = game.current_speed;
    timing.adaptation = game.speed_adaptation;
    timing.flags = game.sim_run_flags;
}

// The local player and clock bound for one save, as GameBinding does.
class LocalClockBinding {
  public:

    LocalClockBinding(
        oa::Game& game, uint8_t local_player, const oa::base::game_loop::Timing& timing
    )
        : game_(game), local_player_(game.local_player_index) {
        load_timing(game, saved_);
        game.local_player_index = local_player;
        store_timing(game, timing);
    }

    ~LocalClockBinding() {
        game_.local_player_index = local_player_;
        store_timing(game_, saved_);
    }

    LocalClockBinding(const LocalClockBinding&) = delete;
    LocalClockBinding& operator=(const LocalClockBinding&) = delete;

  private:

    oa::Game& game_;
    uint8_t local_player_;
    oa::base::game_loop::Timing saved_{};
};

// Game.saved_game for the length of one mission start.
struct ResumedSave {
    persist::Bank*& slot;

    ResumedSave(persist::Bank*& resumed, persist::Bank* bank) : slot(resumed) { slot = bank; }

    ~ResumedSave() { slot = nullptr; }

    ResumedSave(const ResumedSave&) = delete;
    ResumedSave& operator=(const ResumedSave&) = delete;
};

/// Returns a Summary reader over an open bank, for reading a save's Summary.
///
/// @return the reader
ui::frontend::SaveSummaryReader bank_summary_reader() {
    ui::frontend::SaveSummaryReader reader;
    reader.get_int = [](void*, void* bank, const char* field, int32_t fallback) {
        return persist::bank_get_int(static_cast<persist::Bank*>(bank), field, fallback);
    };
    reader.get_string = [](void*, void* bank, const char* field, char* out, std::size_t capacity) {
        const char* text =
            persist::bank_get_text(static_cast<persist::Bank*>(bank), field, nullptr);
        if (text == nullptr || capacity == 0)
            return false;
        std::snprintf(out, capacity, "%s", text);
        return true;
    };
    reader.has_field = [](void*, void* bank, const char* field) {
        return persist::bank_has_field(static_cast<persist::Bank*>(bank), field);
    };
    return reader;
}

// A TDF section as the meteor-defaults reader sees it.
persist::MeteorTdf meteor_tdf(const oa::formats::tdf::Block* section) {
    persist::MeteorTdf tdf{};
    tdf.context = const_cast<oa::formats::tdf::Block*>(section);
    tdf.text = [](void* context, const char* key, char* out, std::size_t out_bytes) {
        return oa::formats::tdf::get_string(
            static_cast<const oa::formats::tdf::Block*>(context), key, out, out_bytes, nullptr
        );
    };
    tdf.integer = [](void* context, const char* key, int32_t fallback) {
        return oa::formats::tdf::get_int(
            static_cast<const oa::formats::tdf::Block*>(context), key, fallback
        );
    };
    tdf.real = [](void* context, const char* key, double fallback) {
        return oa::formats::tdf::get_double(
            static_cast<const oa::formats::tdf::Block*>(context), key, fallback
        );
    };
    return tdf;
}

/// Returns the bytes of a named blob of a bank's open account.
///
/// @param[in,out] bank bank whose account holds the blob; the blob is left open
/// @param name blob name
/// @return the blob's bytes, empty when the account has no such blob
std::vector<uint8_t> blob_bytes(persist::Bank& bank, const char* name) {
    std::vector<uint8_t> bytes;
    if (!persist::bank_open_blob_name(&bank, name))
        return bytes;
    bytes.resize(static_cast<std::size_t>(std::max(persist::bank_blob_size(&bank), 0)));
    persist::bank_blob_seek(&bank, 0);
    bytes.resize(persist::bank_blob_read(&bank, bytes.data(), static_cast<uint32_t>(bytes.size())));
    return bytes;
}

/// Digests the Features section a bank holds by feature type name.
///
/// A load builds the feature type table in an order of its own: the map's
/// types, then its units' remnants and what those turn into, then the types
/// the save names that the table still lacks, such as those a mission's
/// schema places. The same features saved before and after a load can thus
/// list their type names in another order and carry other type words. The
/// digest takes the type names in byte order, then the records of "Normal
/// Features", "Animating Features" and "3D Features" in the order each blob
/// holds them, each with its type word replaced by the name it indexes (a
/// word past the names keeps its own two bytes): 64-bit FNV-1a from
/// digest_basis.
///
/// @param[in,out] bank bank with the Features account open; its open blob changes
/// @return the digest
uint64_t features_digest_by_name(persist::Bank& bank) {
    namespace layout = persist::feature_section;
    constexpr std::size_t type_word_bytes = sizeof(uint16_t);
    uint64_t digest = oa::sim::trace::digest_basis;
    const auto add = [&digest](std::span<const uint8_t> bytes) {
        for (const uint8_t byte : bytes) {
            digest ^= byte;
            digest *= oa::sim::trace::digest_prime;
        }
    };
    const std::vector<uint8_t> names = blob_bytes(bank, save_key::feature_type_names);
    const std::size_t name_count = names.size() / layout::type_name_bytes;
    const auto name_at = [&names](std::size_t index) {
        return std::span<const uint8_t>(names).subspan(
            index * layout::type_name_bytes, layout::type_name_bytes
        );
    };
    std::vector<std::span<const uint8_t>> sorted;
    sorted.reserve(name_count);
    for (std::size_t index = 0; index < name_count; ++index)
        sorted.push_back(name_at(index));
    std::sort(sorted.begin(), sorted.end(), [](auto left, auto right) {
        return std::ranges::lexicographical_compare(left, right);
    });
    for (const auto name : sorted)
        add(name);

    struct RecordBlob {
        const char* name{};
        uint32_t record_bytes{};
    };

    for (const RecordBlob blob :
         {RecordBlob{save_key::normal_features, layout::normal_record_bytes},
          RecordBlob{save_key::animating_features, layout::animating_record_bytes},
          RecordBlob{save_key::object_features, layout::object_record_bytes}}) {
        const std::vector<uint8_t> records = blob_bytes(bank, blob.name);
        for (std::size_t at = 0; at + blob.record_bytes <= records.size();
             at += blob.record_bytes) {
            const auto record = std::span<const uint8_t>(records).subspan(at, blob.record_bytes);
            const std::size_t type = load_le16(record.data() + layout::record_type);
            add(record.first(layout::record_type));
            add(type < name_count ? name_at(type)
                                  : record.subspan(layout::record_type, type_word_bytes));
            add(record.subspan(layout::record_type + type_word_bytes));
        }
    }
    return digest;
}

// Rings of 16-pixel cells searched around a unit for a finished structure's
// site.
constexpr int32_t kSiteNearest = 8;
constexpr int32_t kSiteFarthest = 40;

} // namespace

// Match state the sections read and write that lives outside the World.
struct Runtime::SaveLoadState {
    environment::MeteorState meteor{};
    std::array<uint8_t, oa::sim::session::kSkirmishInfoBytes> skirmish_info{};
    std::vector<uint8_t> mapping; // the sight words, little-endian
    bool mapping_valid{};
    std::array<uint8_t, kMovementImageBytes> movement_image{};
    uint16_t current_unit{};
    // Game.saved_game: the savegame a starting skirmish or campaign mission
    // resumes, and whether its Players section restored.
    persist::Bank* resumed_save{};
    bool resumed_players{};
    // The feature TDF set a load reads the types its save names from, held
    // from the Features section's set load to its link pass.
    std::vector<oa::formats::tdf::OwnedDocument> feature_documents;
    // Script, movement and economy state the last save could not write or the
    // last load could not restore, and feature types the last load could not
    // load.
    std::size_t save_failures{};
    std::size_t restore_failures{};

    struct Bindings {
        Runtime* runtime{};
        SaveLoadState* state{};
        oa::World* world{}; // the Game block the sections read
    };

    static void stage_sight(Runtime& runtime, SaveLoadState& state);
    static void apply_plots(Runtime& runtime, const SaveLoadState& state);
    static void stage_rules(const init::Preferences& preferences, SaveLoadState& state);
    static persist::SaveHooks make_hooks(Bindings* bindings);
    static persist::SaveContext match_context(
        oa::World& world, SaveLoadState& state, uint8_t* mapping, const persist::SaveHooks& hooks
    );
    static uint16_t run_unit_limit(Runtime& runtime);
    static std::optional<uint16_t> buildable_unit_limit(int32_t limit);
    static std::optional<uint16_t> saved_unit_limit(Runtime& runtime, persist::Bank* bank);
    static void adopt_saved_unit_limit(Runtime& runtime, oa::World& world, persist::Bank* bank);
};

void Runtime::destroy_saveload_state(SaveLoadState* state) noexcept {
    delete state;
}

Runtime::SaveLoadState& Runtime::saveload_state() {
    if (!saveload_)
        saveload_.reset(new SaveLoadState());
    return *saveload_;
}

fs::path Runtime::save_game_root() const {
    // Each mod keeps its own saved games, in a folder named after its id.
    if (options_.mod_profile)
        return preference_path_.parent_path() / std::string(mods_folder_name) /
               path_from_utf8(options_.mod_profile->id);
    return preference_path_.parent_path();
}

fs::path save_relative_path(std::string_view path) {
    std::string relative(path);
    std::replace(relative.begin(), relative.end(), '\\', '/');
    const auto first = relative.find('/');
    const std::string_view head = std::string_view(relative).substr(0, first);
    const auto same = [](char a, char b) {
        return std::toupper(static_cast<unsigned char>(a)) ==
               std::toupper(static_cast<unsigned char>(b));
    };
    if (head.size() == ui::frontend::kSaveDirectory.size() &&
        std::equal(head.begin(), head.end(), ui::frontend::kSaveDirectory.begin(), same))
        relative.replace(0, head.size(), ui::frontend::kSaveDirectory);
    return fs::path(relative);
}

void Runtime::set_meteor_enabled(bool enabled) {
    if (enabled)
        environment::enable_meteors(saveload_state().meteor);
    else
        environment::disable_meteors(saveload_state().meteor);
}

bool Runtime::meteor_enabled() {
    return saveload_state().meteor.enabled != 0;
}

void Runtime::start_meteor_strike() {
    if (!match_ || !selected_tnt_)
        return;
    oa::World& world = match_->state();
    // The camera held on the map as the game holds it: a view past the
    // map's edges reaches no save and no strike.
    const auto camera = on_map_camera();
    const GameBinding binding(
        world.game, *selected_tnt_, camera[0], camera[1], state_.player_count
    );
    environment::MeteorHost host{};
    host.context = this;
    host.lcg_random = [](void* context) -> int32_t {
        return static_cast<Runtime*>(context)->match_->lcg_rand();
    };
    environment::start_meteor_strike(saveload_state().meteor, world, host);
}

void Runtime::reset_meteors() {
    auto& meteor = saveload_state().meteor;
    environment::MeteorSettings settings{};
    const auto load_defaults = [&] {
        const auto bytes = assets_.load_file_contents(
            oa::data::defs::data_path(oa::data::defs::DataDirectory::gamedata, kMeteorDefaults)
        );
        if (!bytes)
            return;
        const std::string_view text(reinterpret_cast<const char*>(bytes->data()), bytes->size());
        oa::formats::tdf::OwnedDocument document;
        if (!document.parse(text))
            throw std::runtime_error(
                "cannot parse " + std::string(
                                      oa::data::defs::data_path(
                                          oa::data::defs::DataDirectory::gamedata, kMeteorDefaults
                                      )
                                  )
            );
        const auto* section = oa::formats::tdf::find_child(document.root(), kMeteorDefaultSection);
        if (section == nullptr)
            return;
        const auto tdf = meteor_tdf(section);
        if (persist::load_meteor_config(&tdf, &settings) == persist::MeteorConfigResult::bogus)
            throw std::runtime_error(
                "bogus meteor defaults in " +
                std::string(
                    oa::data::defs::data_path(
                        oa::data::defs::DataDirectory::gamedata, kMeteorDefaults
                    )
                )
            );
    };
    const auto real = [&](const char* key) {
        return static_cast<float>(oa::formats::tdf::get_double(session_schema_section(), key, 0.0));
    };
    const auto weapon = schema_text(kMeteorWeapon).value_or("");
    if (weapon.empty()) {
        environment::disable_meteors(meteor);
        load_defaults();
    } else {
        std::snprintf(settings.weapon, sizeof settings.weapon, "%s", weapon.c_str());
        settings.radius = schema_integer(kMeteorRadius, 0);
        settings.density = real(kMeteorDensity);
        settings.duration = real(kMeteorDuration);
        settings.interval = real(kMeteorInterval);
        if (settings.radius == 0 || settings.density == 0.0F || settings.duration == 0.0F ||
            settings.interval == 0.0F)
            load_defaults();
        environment::enable_meteors(meteor);
    }
    environment::configure_meteors(meteor, settings);
    environment::MeteorHost host{};
    host.context = this;
    host.find_weapon = [](void* context, const char* name) -> oa_ref32 {
        const auto* definition = static_cast<Runtime*>(context)->weapon_registry_.find(name);
        return definition != nullptr ? oa::oa_ref_from_index(definition->registry_index) : 0u;
    };
    environment::reset_meteors(meteor, match_->state(), host);
}

void Runtime::step_meteors() {
    if (!match_)
        return;
    environment::MeteorHost host{};
    host.context = this;
    host.lcg_random = [](void* context) -> int32_t {
        return static_cast<Runtime*>(context)->match_->lcg_rand();
    };
    host.spawn_projectile =
        [](void* context, oa_ref32 weapon, const FixedVec3* position, const FixedVec3* velocity) {
            // A meteor that finds the projectile pool full does not fall.
            std::ignore = static_cast<Runtime*>(context)->match_->launch_meteor(
                weapon, *position, *velocity, true
            );
        };
    environment::step_meteors(saveload_state().meteor, match_->state(), host);
}

// Stages the sight words the Mapping section reads; the other map sections
// read and restore the match's canonical plots.
void Runtime::SaveLoadState::stage_sight(Runtime& runtime, SaveLoadState& state) {
    const oa::formats::tnt::Map& map = *runtime.selected_tnt_;
    const auto cells = static_cast<std::size_t>(map.attribute_width) * map.attribute_height;
    const auto& sight = runtime.match_->sight();
    // One sight word per two cells.
    state.mapping.assign(cells / 2, 0);
    state.mapping_valid = sight.player_bits.size() * kSightWordBytes == state.mapping.size();
    if (state.mapping_valid)
        for (std::size_t i = 0; i < sight.player_bits.size(); ++i)
            store_le16(state.mapping.data() + i * kSightWordBytes, sight.player_bits[i]);
}

// Takes the metal and placing-player bits a load restored into the canonical
// plots, and the restored sight words, into the match.
void Runtime::SaveLoadState::apply_plots(Runtime& runtime, const SaveLoadState& state) {
    runtime.match_->adopt_plot_metal();
    const oa::World& world = runtime.match_->state();
    auto& plots = runtime.match_->spatial().plots;
    const int32_t map_width = world.game.map_width;
    const int32_t map_height = world.game.map_height;
    const auto cells = static_cast<std::size_t>(std::max(map_width, 0)) *
                       static_cast<std::size_t>(std::max(map_height, 0));
    for (std::size_t i = 0; i < cells && i < plots.size(); ++i) {
        const oa::MapPlot& plot = world.plots[i];
        plots[i].flags = static_cast<uint8_t>(
            (plots[i].flags & ~persist::plot_flags_player_features) |
            (plot.flags & persist::plot_flags_player_features)
        );
    }
    if (!state.mapping_valid)
        return;
    auto& sight = runtime.match_->sight_mutable();
    for (std::size_t i = 0; i < sight.player_bits.size(); ++i)
        sight.player_bits[i] = load_le16(state.mapping.data() + i * kSightWordBytes);
}

// The sections over a match: its canonical plots and placed-feature records,
// the sight words staged at `mapping` and the meteor state.
persist::SaveContext Runtime::SaveLoadState::match_context(
    oa::World& world, SaveLoadState& state, uint8_t* mapping, const persist::SaveHooks& hooks
) {
    return {
        &world,
        reinterpret_cast<uint8_t*>(world.plots),
        mapping,
        world.placed_features,
        &state.meteor,
        &hooks,
        world.placed_features != nullptr ? world.placed_feature_count : 0u
    };
}

/// Returns the run-wide unit limit a new skirmish plays at: the frontend Game
/// block's max_units_setting, or the Unit limit setting while nothing has set
/// it (EngineSettingsState::run_unit_limit).
///
/// @param runtime the runtime whose frontend Game block holds the setting
/// @return units per player
uint16_t Runtime::SaveLoadState::run_unit_limit(Runtime& runtime) {
    return EngineSettingsState::run_unit_limit(runtime);
}

/// Returns a unit limit as units per player when a unit pool can be built for it.
///
/// @param limit units per player, as a save holds it
/// @return the limit, or nullopt when it is not positive or its pool would pass
///     65535 slots
std::optional<uint16_t> Runtime::SaveLoadState::buildable_unit_limit(int32_t limit) {
    if (limit <= 0 || limit > std::numeric_limits<uint16_t>::max())
        return std::nullopt;
    // Only whether the pool can be built matters, not its size.
    if (oa::sim::unit_spawn::unit_pool_size(static_cast<uint16_t>(limit)) == 0)
        return std::nullopt;
    return static_cast<uint16_t>(limit);
}

/// Returns the unit limit a skirmish save's world is built at: its Summary
/// "maxunits" as the save gives it, or the run-wide limit when the save has
/// none.
///
/// @param runtime the runtime whose run-wide limit stands in for a missing field
/// @param[in,out] bank the save, with its Summary account open
/// @return units per player, or nullopt when the save's value cannot size a unit pool
std::optional<uint16_t>
Runtime::SaveLoadState::saved_unit_limit(Runtime& runtime, persist::Bank* bank) {
    if (!persist::bank_has_field(bank, save_key::max_units))
        return run_unit_limit(runtime);
    return buildable_unit_limit(persist::bank_get_int(bank, save_key::max_units, 0));
}

/// Reads a save's Summary "maxunits" into the match's unit-limit setting and the
/// run-wide one; a save without the field leaves both alone.
///
/// The match keeps playing at the limit its world was built at. The match's
/// setting takes the value as given, for the next save to write; the run-wide
/// setting takes it only when a unit pool can be built for it.
///
/// @param runtime the runtime whose frontend Game block holds the run-wide setting
/// @param[in,out] world the loaded match, whose Game.max_units_setting receives the value
/// @param[in,out] bank the save, with its Summary account open
/// @quirk The loaded game's limit becomes the run-wide setting, so the next new
///        skirmish of the run plays at it. The preferences file is not written.
void Runtime::SaveLoadState::adopt_saved_unit_limit(
    Runtime& runtime, oa::World& world, persist::Bank* bank
) {
    if (!persist::bank_has_field(bank, save_key::max_units))
        return;
    const int32_t saved = persist::bank_get_int(bank, save_key::max_units, 0);
    world.game.max_units_setting = static_cast<uint16_t>(saved);
    if (const std::optional<uint16_t> limit = buildable_unit_limit(saved))
        runtime.frontend_game().max_units_setting = *limit;
}

// The skirmish rule words the Summary reads through Game.skirmish_info.
void Runtime::SaveLoadState::stage_rules(
    const init::Preferences& preferences, SaveLoadState& state
) {
    namespace rules = data::persist::skirmish_rules;
    uint8_t* info = state.skirmish_info.data();
    state.skirmish_info.fill(0);
    store_le32(info + rules::commander_death, preferences.skirmish.commander_death);
    store_le32(info + rules::mapping, preferences.skirmish.mapping);
    store_le32(info + rules::line_of_sight, preferences.skirmish.line_of_sight);
    store_le32(info + rules::line_of_sight_type, preferences.skirmish.los_type);
    store_le32(info + rules::location, preferences.skirmish_location);
}

persist::SaveHooks Runtime::SaveLoadState::make_hooks(Bindings* bindings) {
    persist::SaveHooks hooks{};
    hooks.context = bindings;
    hooks.resolve = [](void* context, persist::SaveRef kind, oa_ref32 ref) -> void* {
        auto* b = static_cast<Bindings*>(context);
        oa::World& world = *b->world;
        switch (kind) {
        case persist::SaveRef::player_info:
            return ref != 0 && ref <= OA_PLAYER_COUNT ? &world.player_info[ref - 1] : nullptr;
        case persist::SaveRef::mission_rules:
            return b->state->skirmish_info.data();
        case persist::SaveRef::movement: {
            // Unit.movement only flags the native movement object, which only
            // mobile units have; the unit is the one whose script the writer
            // stored just before.
            auto* ground = b->runtime->match_->ground_runtime(b->state->current_unit);
            if (ground == nullptr)
                return nullptr;
            const auto saved = ground->save_mobility();
            auto& image = b->state->movement_image;
            image.fill(0);
            std::copy_n(
                saved.begin(),
                persist::movement_saved_bytes,
                image.begin() + persist::movement_saved_offset
            );
            image[persist::movement_flags] = saved[persist::movement_saved_bytes];
            return image.data();
        }
        }
        return nullptr;
    };
    // The runtime's camera placement.
    hooks.set_camera = [](void* context, int32_t x, int32_t z) {
        auto* runtime = static_cast<Bindings*>(context)->runtime;
        runtime->match_camera_x_ = x;
        runtime->match_camera_z_ = z;
        runtime->match_camera_flags_ = 0;
    };
    hooks.write_script = [](void* context, oa::Unit* unit, persist::Bank* bank) {
        auto* b = static_cast<Bindings*>(context);
        b->state->current_unit = unit->id;
        auto* instance = b->runtime->match_->instance(unit->id);
        if (instance == nullptr || instance->script() == nullptr)
            return;
        const auto exported = instance->script()->vm().export_state(
            oa::formats::cob::script_identity(instance->script()->program())
        );
        std::vector<uint8_t> bytes;
        try {
            if (exported.ok())
                bytes = oa::sim::script_state::encode(*exported.state);
        } catch (const std::exception&) {
            bytes.clear();
        }
        if (bytes.empty()) {
            ++b->state->save_failures;
            return;
        }
        persist::bank_blob_seek(bank, 0);
        persist::bank_blob_write(bank, bytes.data(), static_cast<uint32_t>(bytes.size()));
    };
    hooks.visit_orders = [](void* context,
                            const oa::Unit* unit,
                            persist::SavedOrderVisit visit,
                            void* walk) {
        static_cast<Bindings*>(context)->runtime->match_->visit_saved_orders(unit->id, visit, walk);
    };
    // restore_saved_unit counts a unit it cannot create as a restore
    // failure; an id the save holds no record for restores nothing.
    hooks.restore_unit = [](void* context, uint16_t id, persist::Bank* bank) {
        std::ignore = static_cast<Bindings*>(context)->runtime->restore_saved_unit(id, bank);
    };
    // The Features section's load reads the feature TDF set again, loads the
    // types its save names that the table lacks, links their remnants and
    // places the save's features on the match's canonical plots. The match
    // takes each extended table, so every placed type is in it.
    hooks.load_feature_set = [](void* context) {
        auto* b = static_cast<Bindings*>(context);
        auto documents = b->runtime->load_feature_tdf_set();
        if (!documents) {
            std::cerr << "saved features: " << documents.error.message << '\n';
            ++b->state->restore_failures;
            b->state->feature_documents.clear();
            return;
        }
        b->state->feature_documents = std::move(documents.value);
    };
    hooks.find_or_load_feature = [](void* context, const char* name) -> int16_t {
        auto* b = static_cast<Bindings*>(context);
        auto& runtime = *b->runtime;
        const auto host = runtime.feature_def_host();
        const auto loaded = oa::sim::map_runtime::find_or_load_feature(
            runtime.feature_table_, b->state->feature_documents, name, &host
        );
        runtime.match_->adopt_feature_defs(runtime.feature_table_.defs);
        if (!loaded.ok()) {
            std::cerr << "saved feature '" << name << "': " << loaded.error->message << '\n';
            ++b->state->restore_failures;
            return static_cast<int16_t>(oa::sim::feature_runtime::no_feature);
        }
        return static_cast<int16_t>(loaded.index);
    };
    hooks.link_feature_set = [](void* context) {
        auto* b = static_cast<Bindings*>(context);
        auto& runtime = *b->runtime;
        const auto host = runtime.feature_def_host();
        if (const auto error = oa::sim::map_runtime::load_feature_links(
                runtime.feature_table_, b->state->feature_documents, &host
            )) {
            std::cerr << "saved features: " << error->message << '\n';
            ++b->state->restore_failures;
        }
        runtime.match_->adopt_feature_defs(runtime.feature_table_.defs);
        b->state->feature_documents.clear();
    };
    hooks.place_feature = [](void* context,
                             uint8_t* plot,
                             uint16_t type,
                             const uint8_t* position,
                             const uint8_t* orientation) {
        static_cast<Bindings*>(context)->runtime->match_->place_saved_feature(
            plot, type, position, orientation
        );
    };
    hooks.burn_feature = [](void* context, uint16_t x, uint16_t z) {
        static_cast<Bindings*>(context)->runtime->match_->ignite_saved_feature(x, z);
    };
    hooks.queue_feature_event = [](void* context, uint16_t x, uint16_t z, int32_t kind) {
        static_cast<Bindings*>(context)->runtime->match_->restart_saved_feature_sequence(
            x, z, kind
        );
    };
    return hooks;
}

bool Runtime::save_match_game(const fs::path& path, const char* description, int32_t game_id) {
    if (!match_ || !selected_tnt_) {
        status_ = "There is no match to save";
        return false;
    }
    auto& state = saveload_state();
    state.save_failures = 0;
    oa::World& world = match_->state();
    // The camera held on the map as the game holds it: a view past the
    // map's edges reaches no save and no strike.
    const auto camera = on_map_camera();
    const GameBinding binding(
        world.game, *selected_tnt_, camera[0], camera[1], state_.player_count
    );
    const LocalClockBinding clock(world.game, match_local_player_, saved_match_timing());
    const SessionRecordBinding session(world.game, state_.mission_results, selected_mission_index_);
    SaveLoadState::stage_sight(*this, state);
    SaveLoadState::stage_rules(preferences_, state);
    SaveLoadState::Bindings bindings{this, &state, &world};
    const persist::SaveHooks hooks = SaveLoadState::make_hooks(&bindings);
    persist::SaveContext save = SaveLoadState::match_context(
        world, state, state.mapping_valid ? state.mapping.data() : nullptr, hooks
    );
    return write_saved_game(save, campaign_mission_, path, description, game_id);
}

bool Runtime::save_between_missions(
    const fs::path& path, const char* description, int32_t game_id
) {
    oa::World* world = endgame_world();
    const auto* options = endgame_game_options();
    if (world == nullptr || options == nullptr ||
        options->kind != missions::SessionKind::campaign) {
        status_ = "There is no finished campaign mission to save";
        return false;
    }
    auto& state = saveload_state();
    state.save_failures = 0;
    SaveLoadState::Bindings bindings{this, &state, world};
    const persist::SaveHooks hooks = SaveLoadState::make_hooks(&bindings);
    persist::SaveContext save{world, nullptr, nullptr, nullptr, &state.meteor, &hooks};
    return write_saved_game(save, true, path, description, game_id);
}

bool Runtime::save_dialog_game(std::string_view dialog_path, const char* description) {
    const auto path = game_file_path(dialog_path, ui::frontend::SavePathUse::write);
    const auto game_id = static_cast<int32_t>(std::time(nullptr));
    try {
        if (match_)
            return save_match_game(path, description, game_id);
        return save_between_missions(path, description, game_id);
    } catch (const std::exception& failure) {
        status_ = std::string("Save: ") + failure.what();
        std::cerr << "unsupported operation: " << status_ << '\n';
        return false;
    }
}

bool Runtime::write_saved_game(
    persist::SaveContext& save,
    bool campaign,
    const fs::path& path,
    const char* description,
    int32_t game_id
) {
    struct SummaryBindings {
        Runtime* runtime{};
        oa::World* world{};
        missions::CampaignFile* campaign{}; // null for a skirmish
        missions::CampaignEnv env{};
        persist::ImageRows radar{}; // Game.radar_final_surface's rows
    } summary_bindings{
        this, save.world, campaign ? &campaign_object() : nullptr, campaign_dialog_env()
    };

    persist::SummaryHooks summary{};
    summary.context = &summary_bindings;
    summary.build_date = kGameBuildDate;
    summary.build_time = kGameBuildTime;
    summary.campaign_name = [](void* context) -> const char* {
        const auto* campaign = static_cast<SummaryBindings*>(context)->campaign;
        return campaign != nullptr ? missions::campaign_name_if_loaded(campaign) : nullptr;
    };
    // After the last mission there is no next one, and the Summary names the
    // finished mission. A mission that does not load leaves the save as it
    // is (write_saved_game).
    summary.advance_next_mission = [](void* context) {
        auto* b = static_cast<SummaryBindings*>(context);
        if (b->campaign != nullptr)
            std::ignore = missions::campaign_advance_next_mission(b->campaign, &b->env);
    };
    summary.mission_name = [](void* context) -> const char* {
        const auto* b = static_cast<SummaryBindings*>(context);
        return b->campaign != nullptr ? missions::campaign_mission_name(b->campaign)
                                      : b->runtime->selected_map_name_runtime_.c_str();
    };
    summary.game_type = [](void* context) -> int32_t {
        return static_cast<SummaryBindings*>(context)->campaign != nullptr
                   ? persist::game_type_campaign
                   : persist::game_type_skirmish;
    };
    summary.bind_mission_info = [](void* context) {
        auto* b = static_cast<SummaryBindings*>(context);
        if (b->campaign != nullptr)
            std::ignore =
                missions::campaign_bind_mission(b->campaign, &b->env, b->world->game.mission_index);
    };
    // The radar image the match shows (Game.radar_final_surface), which the
    // load and save dialogs show for the save; none until the match is drawn.
    summary.radar_image = [](void* context) -> const persist::ImageRows* {
        auto* b = static_cast<SummaryBindings*>(context);
        const auto& radar = b->runtime->radar_state_;
        const oa::Surface* image = radar.surfaces.final_image;
        if (radar.built_for != b->world || image == nullptr || image->pixels == nullptr ||
            image->width <= 0 || image->height <= 0)
            return nullptr;
        b->radar = {
            static_cast<uint32_t>(image->width),
            static_cast<uint32_t>(image->height),
            static_cast<uint32_t>(image->pitch),
            image->pixels
        };
        return &b->radar;
    };
    summary.write_stats_panel = [](void* context, persist::Bank* bank) {
        hud::save_players_section(*static_cast<SummaryBindings*>(context)->world, *bank);
    };
    summary.save_profile = [](void* context, persist::Bank* bank) {
        static_cast<SummaryBindings*>(context)->runtime->save_mod_profile(bank);
    };
    summary.save_conditions = [](void* context, persist::Bank* bank) {
        auto* runtime = static_cast<SummaryBindings*>(context)->runtime;
        if (!sim::scenario::save_conditions(
                runtime->match_->scenario_controller(), bank, runtime->scenario_map_kind()
            ))
            throw std::runtime_error("the campaign map's victory conditions cannot be saved");
    };
    // A name that would leave the saved games' folder has no host path.
    if (path.empty()) {
        status_ = "Could not write the saved game: its name leaves the saved games folder";
        return false;
    }
    std::error_code error;
    fs::create_directories(path.parent_path(), error);
    const persist::FileSink sink = persist::stdio_file_sink();
    const bool written = persist::save_write_game(
        &save, &summary, path_to_utf8(path).c_str(), description, game_id, &sink
    );
    // Between missions the save loads the next mission's information to name
    // it, then binds the finished mission again. The file holds none of that
    // information, so a mission that does not load leaves the save as it is:
    // the player sees only the mission loader's message boxes, as in 3.1c.
    status_ = written ? "Saved " + path_to_utf8(path.filename())
                      : "Could not write " + path_to_utf8(path);
    return written;
}

oa::Unit* Runtime::restore_saved_unit(uint16_t id, persist::Bank* bank) {
    namespace r = data::persist::unit_record;
    namespace f = data::persist::unit_record_flags;
    auto& state = saveload_state();
    auto& slots = match_->world().slots;
    if (id == 0 || id >= slots.size())
        return nullptr;
    if (auto& existing = slots[id];
        existing.unit != nullptr && (existing.record.flags & OA_UNIT_FLAG_LIVE) != 0)
        return &existing.record;
    // The ids that hold records; the walk stops at the first id below the
    // count without one, as the game's does.
    int32_t* stored_ids = nullptr;
    const int32_t stored = persist::save_unit_record_ids(
        bank, persist::bank_get_int(bank, save_key::unit_count, 0), &stored_ids
    );
    const std::unique_ptr<int32_t, void (*)(void*)> ids(stored_ids, std::free);
    for (int32_t index = 0; index < stored && ids.get()[index] == index; ++index) {
        if (!persist::bank_open_blob_id(bank, index))
            return nullptr;
        persist::bank_blob_seek(bank, 0);
        uint8_t record[persist::unit_record_bytes];
        if (persist::bank_blob_read(bank, record, persist::unit_record_bytes) !=
            persist::unit_record_bytes)
            return nullptr;
        if (load_le16(record + r::id) != id)
            continue;
        char type_name[r::type_name_bytes + 1] = {};
        std::memcpy(type_name, record + r::type_name, r::type_name_bytes);
        const uint32_t flags = load_le32(record + r::flags);
        const uint32_t creation_state = (flags >> f::low_shift) & f::state;
        oa::sim::unit_spawn::Request request;
        request.player = record[r::owner];
        request.type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, type_name);
        request.position = {
            load_le32(record + r::position + offsetof(FixedVec3, x)),
            load_le32(record + r::position + offsetof(FixedVec3, y)),
            load_le32(record + r::position + offsetof(FixedVec3, z))
        };
        request.finished = true;
        request.state = creation_state;
        request.requested_slot = id;
        // A building is placed facing as its saved heading turns it (units.build-rotation).
        request.facing =
            match_->build_facing_of_heading(request.type, load_le16(record + r::heading));
        oa::sim::unit_spawn::Slot* slot = nullptr;
        try {
            slot = match_->create(request);
        } catch (const std::exception& failure) {
            std::cerr << "saved unit " << id << " (" << type_name << "): " << failure.what()
                      << '\n';
        }
        if (slot == nullptr || slot->unit == nullptr) {
            ++state.restore_failures;
            return nullptr;
        }
        oa::Unit& unit = slot->record;
        unit.bank = static_cast<int16_t>(load_le16(record + r::bank));
        unit.heading = load_le16(record + r::heading);
        unit.pitch = static_cast<int16_t>(load_le16(record + r::pitch));
        unit.health = static_cast<int16_t>(load_le16(record + r::health));
        unit.veteran_level = load_le16(record + r::veteran_level);
        unit.position.y =
            static_cast<oa_fixed>(load_le32(record + r::position + offsetof(FixedVec3, y)));
        if (const uint16_t carrier = load_le16(record + r::carrier_id);
            carrier != 0 && restore_saved_unit(carrier, bank) != nullptr)
            match_->set_carry_link(
                id,
                carrier,
                static_cast<int8_t>(record[r::carrier_slot]),
                static_cast<uint8_t>(creation_state)
            );
        const oa::Unit* linked = restore_saved_unit(load_le16(record + r::linked_id), bank);
        unit.last_attacker_id = linked != nullptr ? linked->id : 0u;
        unit.attach_piece = record[r::carrier_slot];
        unit.last_attacker_owner = record[r::last_attacker_owner];
        unit.extracted_metal = load_le_float(record + r::extracted_metal);
        unit.cell_x = static_cast<int16_t>(load_le16(record + r::cell_x));
        unit.cell_z = static_cast<int16_t>(load_le16(record + r::cell_z));
        unit.sight_center_x = load_le16(record + r::sight_center_x);
        unit.sight_center_z = load_le16(record + r::sight_center_z);
        unit.footprint_x = static_cast<int16_t>(load_le16(record + r::footprint_x));
        unit.footprint_z = static_cast<int16_t>(load_le16(record + r::footprint_z));
        unit.squad = static_cast<int32_t>(load_le32(record + r::squad));
        match_->set_unit_squad(id, static_cast<uint32_t>(unit.squad));
        unit.build_remaining = load_le_float(record + r::build_remaining);
        unit.damage_kind = record[r::damage_kind];
        unit.health_percent = record[r::health_percent];
        unit.previous_health_percent = record[r::previous_health_percent];
        unit.events = load_le16(record + r::events);
        unit.sight_band = record[r::sight_band];
        unit.damage_countdown = record[r::damage_countdown];
        unit.state_flags = record[r::state_flags];
        unit.build_flags =
            static_cast<uint8_t>((unit.build_flags & ~f::build_nibble) | (flags & f::build_nibble));
        const uint32_t restored = ((flags >> f::low_shift) & (f::low & ~f::state)) |
                                  ((flags >> f::construction_dirty_shift) & f::construction_dirty) |
                                  ((flags >> f::high_shift) & f::high_kept);
        unit.flags = (unit.flags & ~f::restored) | restored;
        unit.decloak_until_tick = load_le32(record + r::decloak_until_tick);
        // Unit.movement keeps what the creation set: only mobile units have
        // the movement object the record's flag and mobility blob describe.
        const bool has_movement = load_le32(record + r::has_movement) != 0;
        char blob[persist::save_name_bytes];
        if (!persist::save_read_unit_economy(&unit, bank))
            ++state.restore_failures;
        const auto read_blob = [bank](const char* name, void* out, uint32_t bytes) {
            if (!persist::bank_open_blob_name(bank, name))
                return false;
            persist::bank_blob_seek(bank, 0);
            return persist::bank_blob_read(bank, out, bytes) == bytes;
        };
        if (has_movement) {
            std::snprintf(blob, sizeof blob, save_key::mobility_format, static_cast<unsigned>(id));
            std::array<uint8_t, oa::sim::ground_orders::mobility_record_size> saved{};
            auto* ground = match_->ground_runtime(id);
            if (ground != nullptr &&
                read_blob(blob, saved.data(), static_cast<uint32_t>(saved.size())))
                ground->load_mobility(saved);
            else
                ++state.restore_failures;
        }
        restore_saved_orders(unit, load_le32(record + r::order_count), bank);
        if (auto* instance = match_->instance(id);
            instance != nullptr && instance->script() != nullptr) {
            std::snprintf(blob, sizeof blob, save_key::script_format, static_cast<int>(index));
            bool imported = false;
            if (persist::bank_open_blob_name(bank, blob)) {
                std::vector<uint8_t> bytes(static_cast<std::size_t>(persist::bank_blob_size(bank)));
                persist::bank_blob_seek(bank, 0);
                const auto& header = instance->script()->program().header;
                if (persist::bank_blob_read(
                        bank, bytes.data(), static_cast<uint32_t>(bytes.size())
                    ) == bytes.size()) {
                    const auto decoded = oa::sim::script_state::decode(
                        bytes,
                        oa::formats::cob::script_identity(instance->script()->program()),
                        header.static_variable_count,
                        header.piece_count
                    );
                    imported = decoded.ok() &&
                               !instance->script()->vm().import_state(*decoded.state).has_value();
                }
            }
            if (!imported)
                ++state.restore_failures;
        }
        for (std::size_t w = 0; w < OA_UNIT_WEAPON_COUNT; ++w)
            persist::save_restore_unit_weapon(
                &match_->state(), record + r::weapons + w * r::weapon_bytes, unit.weapons[w]
            );
        // An open yard (build flags bit 2) settles its footprint and its
        // neighbours' through the footprint clear.
        constexpr uint8_t yard_open_flag = 0x04;
        if ((unit.build_flags & yard_open_flag) != 0)
            match_->refresh_restored_footprint(id);
        return &unit;
    }
    return nullptr;
}

void Runtime::restore_saved_orders(
    const oa::Unit& unit, uint32_t order_count, persist::Bank* bank
) {
    auto& state = saveload_state();
    const persist::BankAccounts* accounts = bank->accounts;
    const int32_t stored =
        accounts != nullptr && accounts->open >= 0 && accounts->open < accounts->count
            ? accounts->items[accounts->open].blob_count
            : 0;
    const uint32_t count = std::min(order_count, static_cast<uint32_t>(std::max(stored, 0)));
    auto tails = match_->saved_order_tails(unit.id);
    const auto restored = [&](uint16_t id) -> uint16_t {
        return id != 0 && restore_saved_unit(id, bank) != nullptr ? id : uint16_t{0};
    };
    for (uint32_t index = 0; index < count; ++index) {
        char blob[persist::save_name_bytes];
        std::snprintf(
            blob, sizeof blob, save_key::order_format, static_cast<unsigned>(unit.id), index
        );
        persist::SavedOrder order;
        persist::SavedGoal goal;
        if (persist::bank_find_blob_name(bank, blob, false) < 0 ||
            !persist::save_read_order(&match_->state(), &unit, bank, blob, &order, &goal)) {
            ++state.restore_failures;
            continue;
        }
        order.target_id = restored(order.target_id);
        goal.air_target.unit_id = restored(goal.air_target.unit_id);
        goal.air_target.target_id = restored(goal.air_target.target_id);
        goal.air_seek.unit_id = restored(goal.air_seek.unit_id);
        match_->restore_saved_order(unit.id, order, goal, tails);
    }
    match_->install_head_goal(unit.id);
}

bool Runtime::start_saved_game(std::string_view dialog_path) {
    // A mod that cannot start a game says so, and the dialog stays.
    if (refuse_incomplete_mod_start())
        return false;
    try {
        return load_saved_game(game_file_path(dialog_path, ui::frontend::SavePathUse::read));
    } catch (const std::exception& failure) {
        status_ = std::string("Saved game start: ") + failure.what();
        std::cerr << "unsupported operation: " << status_ << '\n';
        return false;
    }
}

int32_t Runtime::scenario_map_kind() const noexcept {
    return campaign_mission_ ? init::map_list_kind::selection_setup : init::map_list_kind::skirmish;
}

bool Runtime::restore_saved_session(persist::Bank* bank) {
    auto& state = saveload_state();
    state.restore_failures = 0;
    oa::World& world = match_->state();
    // The camera held on the map as the game holds it: a view past the
    // map's edges reaches no save and no strike.
    const auto camera = on_map_camera();
    const GameBinding binding(
        world.game, *selected_tnt_, camera[0], camera[1], state_.player_count
    );
    SaveLoadState::stage_sight(*this, state);
    SaveLoadState::Bindings bindings{this, &state, &world};
    const persist::SaveHooks hooks = SaveLoadState::make_hooks(&bindings);
    persist::SaveContext save =
        SaveLoadState::match_context(world, state, state.mapping.data(), hooks);
    persist::bank_open_account(bank, save_key::summary);
    SaveLoadState::adopt_saved_unit_limit(*this, world, bank);
    const bool players = hud::load_players_section(world, *bank);
    persist::save_read_camera(&save, bank);
    persist::save_read_features(&save, bank);
    persist::save_read_metal_plotmap(&save, bank);
    persist::save_read_player_features(&save, bank);
    persist::save_read_terrain_mapping(&save, bank);
    persist::save_read_units(&save, bank);
    persist::save_read_meteor(&state.meteor, bank);
    if (!sim::scenario::load_conditions(match_->scenario_controller(), bank, scenario_map_kind()))
        throw std::runtime_error("the campaign map's victory conditions cannot be restored");
    restore_saved_rule_state(bank);
    match_->selection().frame_flags |= hud::kFrameRedrawBuildMenu;
    SaveLoadState::apply_plots(*this, state);
    rebuild_feature_draws();
    for (std::size_t i = 0; i < OA_PLAYER_COUNT; ++i) {
        skirmish_settings_.slots[i].side = world.player_info[i].side;
        skirmish_settings_.slots[i].color = world.player_info[i].color;
    }
    return players;
}

void Runtime::save_mod_profile(persist::Bank* bank) const {
    namespace profiles = oa::data::mod_profile;
    const auto* profile = mod_profile();
    if (profile == nullptr)
        return;
    const persist::SavedProfile saved{
        profile->id,
        profile->version,
        profiles::registry::table().catalogue,
        profiles::digest_text(profile->sim_hash),
        profiles::digest_text(profile->full_hash)
    };
    std::vector<persist::SavedRuleState> tables;
    if (match_ && match_->state().game.mode == persist::game_mode_in_match) {
        const auto& state = match_->rule_state();
        for (uint8_t at = 0; at < state.count; ++at) {
            const auto& table = state.tables[at];
            tables.push_back(
                {table.name,
                 table.bytes != nullptr ? table.bytes(table.context) : std::span<const uint8_t>{}}
            );
        }
    }
    if (!persist::save_write_profile(bank, saved, tables))
        throw std::runtime_error("the mod profile's account cannot be saved");
}

bool Runtime::check_saved_mod_profile(persist::Bank* bank) {
    namespace profiles = oa::data::mod_profile;
    const auto saved = persist::save_read_profile(bank);
    if (!saved)
        return true;
    // The save is loaded into a new match, which plays the profile with the
    // overrides in effect now.
    const auto* profile = next_match_profile();
    const std::string saved_name = saved->id + " " + saved->version;
    if (profile == nullptr) {
        status_ = "Savegame belongs to mod profile " + saved_name + "; this game plays base 3.1c";
        return false;
    }
    if (saved->sim_hash != profiles::digest_text(profile->sim_hash)) {
        status_ = "Savegame belongs to mod profile " + saved_name + " (sim hash " +
                  saved->sim_hash + "); this game plays " + profile->id + " " + profile->version +
                  " (sim hash " + profiles::digest_text(profile->sim_hash) + ")";
        return false;
    }
    return true;
}

void Runtime::restore_saved_rule_state(persist::Bank* bank) {
    const auto& state = match_->rule_state();
    if (state.count == 0 || !persist::save_read_profile(bank))
        return;
    for (uint8_t at = 0; at < state.count; ++at) {
        const auto& table = state.tables[at];
        const auto bytes = persist::save_read_rule_state(bank, table.name);
        if (!bytes || table.restore == nullptr)
            continue;
        if (!table.restore(table.context, *bytes))
            throw std::runtime_error(
                std::string("the saved state of the mod rule table ") + table.name +
                " does not fit this game"
            );
    }
}

bool Runtime::resume_saved_mission() {
    auto& state = saveload_state();
    if (state.resumed_save == nullptr)
        return false;
    state.resumed_players = restore_saved_session(state.resumed_save);
    return true;
}

bool Runtime::resuming_saved_game() const noexcept {
    return saveload_ && saveload_->resumed_save != nullptr;
}

bool Runtime::read_save_summary(const fs::path& path, ui::frontend::LoadSummary& summary) {
    BankGuard guard;
    persist::BankError error{};
    if (!read_whole_save(guard, path, &error))
        return false;
    persist::bank_open_account(&guard.bank, save_key::summary);
    return ui::frontend::savegame_read_load_summary(bank_summary_reader(), &guard.bank, summary);
}

bool Runtime::load_saved_game(const fs::path& path) {
    BankGuard guard;
    persist::Bank* bank = &guard.bank;
    persist::BankError error{};
    if (!read_whole_save(guard, path, &error)) {
        status_ = std::string("Invalid savegame file: ") + error.message;
        return false;
    }
    if (!check_saved_mod_profile(bank))
        return false;
    persist::bank_open_account(bank, save_key::summary);
    ui::frontend::LoadSummary summary;
    if (!ui::frontend::savegame_read_load_summary(bank_summary_reader(), bank, summary)) {
        status_ = "Savegame names no mission";
        return false;
    }
    if (summary.game_type == persist::game_type_campaign)
        return load_saved_campaign(path, bank, summary);
    if (summary.game_type != persist::game_type_skirmish) {
        status_ = "Invalid savegame file";
        return false;
    }
    // A skirmish's world is built at the limit it was saved at, so each
    // player's units come back into that player's own slots. A campaign
    // mission plays at its mission's limit instead.
    const std::optional<uint16_t> units_per_player = SaveLoadState::saved_unit_limit(*this, bank);
    if (!units_per_player) {
        status_ = "Invalid savegame file: unit limit out of range";
        return false;
    }
    // The controllers come from a scratch game block: the match is not built yet.
    std::array<hud::SkirmishSlot, OA_PLAYER_COUNT> controllers{};
    {
        const auto scratch = std::make_unique<oa::World>();
        hud::load_player_controllers(*scratch, *bank, controllers.data());
    }
    persist::bank_open_account(bank, save_key::players);
    const auto human =
        persist::bank_get_int(bank, save_key::human_player, persist::no_human_player);
    if (human < 0 || human >= persist::no_human_player ||
        controllers[static_cast<std::size_t>(human)].controller != entry::controller::human) {
        status_ = "Savegame has no human player";
        return false;
    }

    if (match_)
        leave_match();
    campaign_mission_ = false;
    preferences_.difficulty = static_cast<uint32_t>(summary.difficulty);
    preferences_.skirmish.commander_death = static_cast<uint32_t>(summary.commander_death);
    preferences_.skirmish.mapping = static_cast<uint32_t>(summary.mapping);
    preferences_.skirmish.line_of_sight = static_cast<uint32_t>(summary.line_of_sight);
    preferences_.skirmish.los_type = static_cast<uint32_t>(summary.line_of_sight_type);
    preferences_.skirmish_location = static_cast<uint32_t>(summary.location);
    state_.player_count = static_cast<uint16_t>(summary.players);
    const std::string map(summary.mission.data());
    keep_player_skirmish_settings();
    skirmish_settings_.map_name = map;
    if (select_map(map) == 0 || map_player_capacity() == 0) {
        status_ = "Saved map '" + map + "' is not available";
        return false;
    }
    std::copy_n(
        summary.start_pattern.begin(), state_.mission_results.size(), state_.mission_results.begin()
    );
    // Side, logo and alliances are the saved players' own and follow with
    // the Players section; each player starts alone. The slot count covers
    // the highest active slot, not the number of players.
    int32_t active_slots = 0;
    for (std::size_t i = 0; i < skirmish_settings_.slots.size(); ++i) {
        auto& slot = skirmish_settings_.slots[i];
        slot = {};
        slot.alliance = entry::unassigned_alliance;
        if (i >= controllers.size() || (controllers[i].controller != entry::controller::human &&
                                        controllers[i].controller != entry::controller::computer))
            continue;
        slot.controller = controllers[i].controller;
        active_slots = static_cast<int32_t>(i) + 1;
    }
    skirmish_settings_.slot_count = std::max(skirmish_settings_.slot_count, active_slots);
    {
        // The match is built for the save, which places its features.
        const ResumedSave resumed(saveload_state().resumed_save, bank);
        bootstrap_match(
            {.units_per_player = *units_per_player, .place_commanders = false, .seat_roster = true}
        );
    }
    if (!match_ || altitude_sight_blocked_) {
        status_ = "Saved game start was blocked";
        return false;
    }
    // Mission start rebuilds the sight grids before the saved session loads.
    reset_match_sight(true);
    finish_saved_game_start(restore_saved_session(bank));
    enter_match_view();
    status_ = "Loaded " + path_to_utf8(path.filename()) + " at tick " +
              std::to_string(match_timing_.tick);
    return true;
}

bool Runtime::load_saved_campaign(
    const fs::path& path, persist::Bank* bank, const ui::frontend::LoadSummary& summary
) {
    if (match_)
        leave_match();
    preferences_.side = static_cast<uint32_t>(summary.side);
    preferences_.difficulty = static_cast<uint32_t>(summary.difficulty);
    // The side's campaign lists, bound to the saved campaign.
    discover_campaigns();
    const std::string_view campaign(summary.campaign.data());
    const auto named = std::find_if(
        campaign_labels_.begin(), campaign_labels_.end(), [&](const std::string& label) {
            return tdf_names_equal(label, campaign);
        }
    );
    if (!summary.has_campaign || named == campaign_labels_.end()) {
        status_ = "Invalid savegame file: no campaign '" + std::string(campaign) + "'";
        return false;
    }
    selected_campaign_index_ = static_cast<std::size_t>(named - campaign_labels_.begin());
    load_campaign_missions(selected_campaign_index_);
    missions::CampaignFile& file = campaign_object();
    const missions::CampaignEnv env = campaign_dialog_env();
    if (!missions::campaign_select_mission(&file, &env, summary.mission.data()) ||
        static_cast<std::size_t>(file.mission_index) >= campaign_mission_files_.size()) {
        // Over the mission loader's message, as 3.1c shows a saved mission
        // that does not load.
        show_frontend_message(
            translate_ui(ui::frontend::kInvalidSaveMessage),
            ui::frontend::kInvalidSaveMessageWidth,
            entry::message_show_ok,
            entry::message_fit_width
        );
        status_ = "Invalid savegame file: no mission '" + std::string(summary.mission.data()) + "'";
        return false;
    }
    selected_mission_index_ = static_cast<std::size_t>(file.mission_index);
    std::copy_n(
        summary.start_pattern.begin(), state_.mission_results.size(), state_.mission_results.begin()
    );
    if (summary.between_missions) {
        // The briefing keeps the bound lists as it does from Any Mission.
        screen_ = Screen::any_mission;
        show_mission_briefing();
        if (screen_ != Screen::briefing) {
            const auto failure = status_;
            load(Screen::single_player);
            status_ = failure;
            return false;
        }
        briefing_parent_ = Screen::single_player;
        status_ = "Loaded " + path_to_utf8(path.filename()) + " before " + file.mission_name;
        return true;
    }
    auto& state = saveload_state();
    state.resumed_players = false;
    briefing_parent_ = Screen::single_player;
    {
        const ResumedSave resumed(state.resumed_save, bank);
        start_campaign_mission();
    }
    if (!match_ || !campaign_mission_ || screen_ != Screen::match) {
        status_ = "Saved campaign start failed: " + status_;
        return false;
    }
    finish_saved_game_start(state.resumed_players);
    status_ = "Loaded " + path_to_utf8(path.filename()) + " at tick " +
              std::to_string(match_timing_.tick);
    return true;
}

void Runtime::finish_saved_game_start(bool restored_players) {
    oa::World& world = match_->state();
    if (restored_players)
        load_timing(world.game, match_timing_);
    // A loaded game starts running, whatever the save holds in the pause bit
    // of Game.sim_run_flags; the file keeps the bit as it was written. Only
    // the Pause key lifts that bit, and closing the in-game menu does not,
    // so a game loaded with it set would stay held after its menu closed.
    world.game.sim_run_flags =
        static_cast<uint16_t>(world.game.sim_run_flags & ~oa::ui::console::kSimRunPaused);
    match_timing_.flags =
        static_cast<uint16_t>(match_timing_.flags & ~oa::ui::console::kSimRunPaused);
    for (std::size_t i = 0; i < OA_PLAYER_COUNT; ++i) {
        if (skirmish_settings_.slots[i].controller == entry::controller::disabled)
            continue;
        std::array<uint8_t, OA_PLAYER_COUNT> allies{};
        std::copy_n(world.game.players[i].alliance, allies.size(), allies.begin());
        match_->configure_player_alliances(static_cast<uint8_t>(i), allies);
    }
    radar_explored_.clear();
    radar_state_.release();
    // The saved host clock belongs to the session that wrote it.
    match_timing_.previous_clock =
        oa::base::game_loop::scaled_clock(clock_milliseconds(), match_clock_scale());
    match_tick_blocked_ = false;
}

uint64_t Runtime::match_world_digest() const {
    if (!match_)
        return oa::sim::trace::digest_basis;
    // The camera as saves hold it, on the map.
    const auto camera = on_map_camera();
    return oa::sim::trace::match_state_hash(
        *match_, match_timing_, camera[0], camera[1], saveload_ ? &saveload_->meteor : nullptr
    );
}

oa::sim::unit_spawn::Slot* Runtime::place_finished_structure(uint16_t type, uint16_t near) {
    const auto& slots = match_->world().slots;
    if (type == 0 || type >= spawn_types_.size() || near >= slots.size() ||
        slots[near].unit == nullptr)
        return nullptr;
    const auto cell_x = static_cast<int32_t>(slots[near].unit->position[0] >> 20);
    const auto cell_z = static_cast<int32_t>(slots[near].unit->position[2] >> 20);
    std::optional<std::pair<int32_t, int32_t>> site;
    for (int32_t ring = kSiteNearest; ring < kSiteFarthest && !site; ++ring)
        for (int32_t dz = -ring; dz <= ring && !site; dz += 2)
            for (int32_t dx = -ring; dx <= ring && !site; dx += 2)
                if ((dx == -ring || dx == ring || dz == -ring || dz == ring) &&
                    match_->building_site(type, cell_x + dx, cell_z + dz, 0))
                    site = std::pair{cell_x + dx, cell_z + dz};
    if (!site)
        return nullptr;
    const auto& placed_type = spawn_types_[type];
    const auto x = static_cast<uint32_t>((site->first * 2 + placed_type.footprint_x) * 8);
    const auto z = static_cast<uint32_t>((site->second * 2 + placed_type.footprint_z) * 8);
    oa::sim::unit_spawn::Request request;
    request.player = match_local_player_;
    request.type = type;
    request.finished = true;
    request.state = kGroundOccupancyState;
    request.position = {
        x << 16, static_cast<uint32_t>(match_->map_height(x << 16, z << 16)) << 16, z << 16
    };
    auto* placed = match_->create(request);
    return placed != nullptr && placed->unit != nullptr ? placed : nullptr;
}

void Runtime::give_saveload_orders() {
    const auto type = [&](std::string_view name) {
        const auto index = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
        if (index == 0)
            throw std::runtime_error("saveload orders lack " + std::string(name));
        return index;
    };
    const auto lab = type("ARMLAB");
    const auto peewee = type("ARMPW");
    const auto solar = type("ARMSOLAR");
    const auto& slots = match_->world().slots;
    uint16_t commander = 0;
    for (const auto& slot : slots)
        if (slot.unit != nullptr && slot.record.type_index != 0 &&
            slot.record.owner_index == match_local_player_) {
            commander = slot.unit_index;
            break;
        }
    if (commander == 0)
        throw std::runtime_error("saveload orders need the local commander");
    const auto* factory = place_finished_structure(lab, commander);
    if (factory == nullptr)
        throw std::runtime_error("saveload orders found no site for ARMLAB");
    match_->queue_factory_build(factory->unit_index, peewee, 3);
    const auto map_w = static_cast<int32_t>(selected_tnt_->tile_width * 32U);
    const auto map_h = static_cast<int32_t>(selected_tnt_->tile_height * 32U);
    const auto at = [&](int32_t x, int32_t z) {
        x = std::clamp(x, 32, map_w - 32);
        z = std::clamp(z, 32, map_h - 32);
        return oa::sim::ground_orders::Point{
            x * 65536,
            match_->map_height(static_cast<uint32_t>(x) << 16, static_cast<uint32_t>(z) << 16) *
                65536,
            z * 65536
        };
    };
    const auto x = static_cast<int32_t>(slots[commander].unit->position[0] >> 16);
    const auto z = static_cast<int32_t>(slots[commander].unit->position[2] >> 16);
    match_->issue_mobile_build(commander, solar, at(x, z - 64), false);
    const auto spawn = [&](uint16_t spawned, const oa::sim::ground_orders::Point& point) {
        oa::sim::unit_spawn::Request request;
        request.player = match_local_player_;
        request.type = spawned;
        request.finished = true;
        request.state = kGroundOccupancyState;
        request.position = {
            std::bit_cast<uint32_t>(point[0]),
            std::bit_cast<uint32_t>(point[1]),
            std::bit_cast<uint32_t>(point[2])
        };
        auto* created = match_->create(request);
        if (created == nullptr || created->unit == nullptr)
            throw std::runtime_error(
                "saveload orders could not place " + std::string(spawn_type_names_[spawned])
            );
        return created->unit_index;
    };
    // The centre of the site nearest a point where a type can stand, searched
    // ring by ring outwards over the whole map.
    const auto nearest_site = [&](uint16_t placed, int32_t near_x, int32_t near_z) {
        const auto& footprint = spawn_types_[placed];
        const int32_t cells_x = map_w / OA_MAP_CELL_PIXELS, cells_z = map_h / OA_MAP_CELL_PIXELS;
        const int32_t cell_x = near_x / OA_MAP_CELL_PIXELS, cell_z = near_z / OA_MAP_CELL_PIXELS;
        for (int32_t ring = 0; ring < std::max(cells_x, cells_z); ++ring)
            for (int32_t dz = -ring; dz <= ring; ++dz)
                for (int32_t dx = -ring; dx <= ring; ++dx)
                    if ((std::abs(dx) == ring || std::abs(dz) == ring) &&
                        match_->building_site(placed, cell_x + dx, cell_z + dz, 0))
                        return at(
                            (cell_x + dx) * OA_MAP_CELL_PIXELS +
                                footprint.footprint_x * OA_MAP_CELL_PIXELS / 2,
                            (cell_z + dz) * OA_MAP_CELL_PIXELS +
                                footprint.footprint_z * OA_MAP_CELL_PIXELS / 2
                        );
        throw std::runtime_error(
            "saveload orders found no site for " + std::string(spawn_type_names_[placed])
        );
    };
    std::array<uint16_t, 3> walkers{};
    for (std::size_t i = 0; i < walkers.size(); ++i)
        walkers[i] = spawn(peewee, at(x - 24 + static_cast<int32_t>(i) * 24, z + 100));
    const auto far_side = at(x < map_w / 2 ? map_w : 0, z < map_h / 2 ? map_h : 0);
    match_->issue_patrol(walkers[0], at(x, z + 300), false);
    match_->issue_guard(walkers[1], commander, false);
    match_->issue_ground_move(walkers[2], far_side, false);
    // An Atlas lifts a Peewee onto its link piece, then carries it towards
    // the far side; a transport ship in the water nearest the commander
    // starts with Peewees in its hold.
    const auto atlas = spawn(type("ARMATLAS"), at(x - 160, z - 120));
    match_->issue_load(atlas, spawn(peewee, at(x - 200, z - 120)), false);
    match_->issue_unload(atlas, far_side, true);
    const auto transport_ship = type("ARMTSHIP");
    const auto ship = spawn(transport_ship, nearest_site(transport_ship, x, z));
    for (int32_t i = 0; i < kSaveloadShipCargo; ++i)
        match_->set_carry_link(
            spawn(peewee, at(x + 24 * i, z - 140)), ship, kNoCarryPiece, kStartAboardLayer
        );
}

void Runtime::print_saved_orders() const {
    std::array<uint32_t, 256> counts{};
    uint32_t total = 0;

    struct Tally {
        std::array<uint32_t, 256>& counts;
        uint32_t& total;
    } tally{counts, total};

    for (const auto& slot : match_->world().slots) {
        if (slot.unit == nullptr || slot.record.type_index == 0 ||
            (slot.record.flags & OA_UNIT_FLAG_LIVE) == 0)
            continue;
        match_->visit_saved_orders(
            slot.unit_index,
            [](void* walk, const persist::SavedOrder* order, const persist::SavedGoal*) {
                auto& t = *static_cast<Tally*>(walk);
                ++t.counts[order->kind];
                ++t.total;
            },
            &tally
        );
    }
    std::string line = "saveload: orders " + std::to_string(total);
    const auto names = oa::data::mission_types::registered_names();
    for (std::size_t kind = 0; kind < counts.size(); ++kind)
        if (counts[kind] != 0)
            line += ' ' + (kind < names.size() ? std::string(names[kind]) : std::to_string(kind)) +
                    '=' + std::to_string(counts[kind]);
    std::printf("%s\n", line.c_str());
}

void Runtime::print_saved_units() const {
    const oa::World& world = match_->state();
    const auto rules = match_->rules_view();
    std::map<std::string, uint32_t> types;
    std::map<uint32_t, uint32_t> levels;
    uint32_t veterans = 0;
    uint32_t most_kills = 0;
    uint32_t stockpile_weapons = 0;
    uint32_t stocked_shots = 0;
    std::array<uint32_t, 4> facings{};
    uint32_t buildings = 0;
    std::array<int64_t, OA_PLAYER_COUNT> health{};
    for (const auto& slot : match_->world().slots) {
        if (slot.unit == nullptr || slot.record.type_index == 0 ||
            (slot.record.flags & OA_UNIT_FLAG_LIVE) == 0)
            continue;
        const oa::Unit& unit = world.units[slot.unit_index];
        const auto type = slot.record.type_index;
        ++types[type < spawn_type_names_.size() ? spawn_type_names_[type] : std::to_string(type)];
        if (unit.veteran_level != 0) {
            ++veterans;
            ++levels[sim::unit_health::veterancy_level(rules, type, unit.veteran_level)];
            most_kills = std::max<uint32_t>(most_kills, unit.veteran_level);
        }
        for (const auto& weapon : unit.weapons) {
            const auto* definition = oa::world_weapon_def(&world, weapon.def);
            if (definition == nullptr || (definition->flags & OA_WEAPON_FLAG_STOCKPILE) == 0)
                continue;
            ++stockpile_weapons;
            stocked_shots += weapon.stockpile;
        }
        if (unit.movement == 0) {
            ++buildings;
            ++facings[match_->unit_build_facing(unit) & 3U];
        }
        if (unit.owner_index < health.size())
            health[unit.owner_index] += unit.health;
    }
    std::string line = "saveload: unit types " + std::to_string(types.size());
    for (const auto& [name, count] : types)
        line += ' ' + name + '=' + std::to_string(count);
    std::printf("%s\n", line.c_str());
    line = "saveload: veterans " + std::to_string(veterans) + ", levels";
    for (const auto& [level, count] : levels)
        line += ' ' + std::to_string(level) + '=' + std::to_string(count);
    line += ", most kills " + std::to_string(most_kills);
    std::printf("%s\n", line.c_str());
    std::printf(
        "saveload: stockpile weapons %u, shots held %u\n", stockpile_weapons, stocked_shots
    );
    std::printf(
        "saveload: buildings %u, facing south %u east %u north %u west %u\n",
        buildings,
        facings[0],
        facings[1],
        facings[2],
        facings[3]
    );
    line = "saveload: health by player";
    for (std::size_t player = 0; player < health.size(); ++player)
        if (health[player] != 0)
            line += ' ' + std::to_string(player) + '=' + std::to_string(health[player]);
    std::printf("%s\n", line.c_str());
}

void Runtime::give_saveload_feature_events() {
    namespace features = oa::sim::feature_runtime;
    oa::World& world = match_->state();
    const auto host = match_->feature_host();
    const auto width = world.game.map_width;
    const auto height = world.game.map_height;
    bool burning = false, dying = false, reclaimed = false, cleared = false;
    // Features in the middle half of the map, away from the edges a load
    // hides.
    for (int32_t z = height / 4; z < height - height / 4; ++z) {
        for (int32_t x = width / 4; x < width - width / 4; ++x) {
            const auto index = static_cast<std::size_t>(z) * static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(x);
            const oa::MapPlot& plot = world.plots[index];
            if (plot.feature >= world.feature_def_count ||
                (plot.flags & OA_PLOT_FLAG_ANIMATING_FEATURE) != 0)
                continue;
            const oa::FeatureDef& def = world.feature_defs[plot.feature];
            const bool sprite = (def.flags & OA_FEATURE_FLAG_SPRITE) != 0;
            if (!burning && sprite && def.seq_name_burn != 0 &&
                (def.flags & OA_FEATURE_FLAG_FLAMABLE) != 0) {
                features::ignite_feature(world, host, x, z, false);
                burning = true;
            } else if (!dying && sprite && def.seq_name_die != 0) {
                features::start_feature_sequence(world, host, x, z, false);
                dying = true;
            } else if (!reclaimed && sprite && def.seq_name_reclamate != 0) {
                features::start_feature_sequence(world, host, x, z, true);
                reclaimed = true;
            } else if (!cleared && (def.flags & OA_FEATURE_FLAG_INDESTRUCTIBLE) == 0) {
                cleared = features::clear_plot_feature(world, host, index, false);
            }
        }
    }
    if (!burning || !dying || !reclaimed || !cleared)
        throw std::runtime_error(
            "saveload features: the map lacks a feature to burn, kill, reclaim or clear"
        );
}

void Runtime::print_saved_features() {
    namespace features = oa::sim::feature_runtime;
    oa::World& world = match_->state();
    // The camera held on the map as the game holds it: a view past the
    // map's edges reaches no save and no strike.
    const auto camera = on_map_camera();
    const GameBinding binding(
        world.game, *selected_tnt_, camera[0], camera[1], state_.player_count
    );
    const auto width = world.game.map_width;
    const auto height = world.game.map_height;
    const auto cells = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    // A load hides the plots under the map's edges before it places the
    // saved features, so a feature whose footprint reaches one does not come
    // back; the line leaves those out.
    const auto hidden = plots_hidden_on_load(world, match_->effects().lava_world);
    std::vector<oa::MapPlot> plots(world.plots, world.plots + cells);
    std::size_t burning = 0, dying = 0, reclaimed = 0;
    for (std::size_t index = 0; index < cells; ++index) {
        oa::MapPlot& plot = plots[index];
        if (plot.feature >= world.feature_def_count)
            continue;
        const oa::FeatureDef& def = world.feature_defs[plot.feature];
        const auto x = static_cast<int32_t>(index % static_cast<std::size_t>(width));
        const auto z = static_cast<int32_t>(index / static_cast<std::size_t>(width));
        bool restored = x + def.footprint_x <= width && z + def.footprint_z <= height;
        for (int32_t row = 0; restored && row < def.footprint_z; ++row)
            for (int32_t column = 0; restored && column < def.footprint_x; ++column)
                restored = hidden[index + static_cast<std::size_t>(row * width + column)] == 0;
        if (!restored) {
            plot.feature = features::no_feature;
            continue;
        }
        const auto* record = (plot.flags & OA_PLOT_FLAG_ANIMATING_FEATURE) != 0
                                 ? features::feature_record(world, plot.feature_record)
                                 : nullptr;
        if ((def.flags & OA_FEATURE_FLAG_SPRITE) == 0 || record == nullptr)
            continue;
        // The sequences as the Features section tells them apart.
        const auto sequence = record->sprite.animation.sequence;
        if (sequence == def.seq_name_burn)
            ++burning;
        else if (sequence == def.seq_name_die)
            ++dying;
        else if (sequence == def.seq_name_reclamate)
            ++reclaimed;
    }
    auto& state = saveload_state();
    SaveLoadState::Bindings bindings{this, &state, &world};
    const persist::SaveHooks hooks = SaveLoadState::make_hooks(&bindings);
    persist::SaveContext save = SaveLoadState::match_context(world, state, nullptr, hooks);
    save.plots = reinterpret_cast<uint8_t*>(plots.data());
    BankGuard guard;
    if (!persist::bank_reset(&guard.bank))
        throw std::runtime_error("saveload features: no bank to write to");
    persist::save_write_features(&save, &guard.bank);
    persist::bank_open_account(&guard.bank, save_key::features);
    const uint64_t digest = features_digest_by_name(guard.bank);
    std::printf(
        "saveload: features normal %d 3d %d animating %d burn %zu die %zu reclaim %zu digest "
        "%016llx\n",
        persist::bank_get_int(&guard.bank, save_key::normal_feature_count, 0),
        persist::bank_get_int(&guard.bank, save_key::object_feature_count, 0),
        persist::bank_get_int(&guard.bank, save_key::animating_feature_count, 0),
        burning,
        dying,
        reclaimed,
        static_cast<unsigned long long>(digest)
    );
}

void Runtime::run_headless_saveload() {
    if (!options_.load_file.empty()) {
        if (!load_saved_game(options_.load_file))
            throw std::runtime_error("saved game did not load: " + status_);
        if (!match_) {
            std::printf(
                "saveload: %s; %s mission %zu\n",
                status_.c_str(),
                screen_ == Screen::briefing ? "briefing" : "no",
                selected_mission_index_
            );
            std::fflush(stdout);
            return;
        }
        std::cout << "saveload: " << status_ << "; restore failures "
                  << saveload_state().restore_failures << '\n';
    } else if (options_.campaign_mission) {
        // A mission that does not start throws; its file name is not needed.
        std::ignore = start_headless_campaign_mission();
        if (options_.give_orders)
            give_mission_orders();
    } else {
        start_benchmark_skirmish();
        if (options_.combat_units != 0)
            spawn_combat_armies(options_.combat_units);
        if (options_.give_orders)
            give_saveload_orders();
        if (!options_.stage_file.empty())
            apply_stage();
    }
    match_layout_ = lay_out_match(options_.match_width, options_.match_height);
    // The headless camera placement applies to a loaded game as to a start.
    if (options_.camera) {
        match_camera_x_ = options_.camera->first;
        match_camera_z_ = options_.camera->second;
    }
    bool saved = false;
    const auto save_if_due = [&] {
        if (saved || !options_.save_after || match_timing_.tick < *options_.save_after)
            return;
        const fs::path target =
            options_.save_file.empty() ? saves_folder() / kHeadlessSaveName : options_.save_file;
        if (!save_match_game(
                target, persist::command_line_description, persist::command_line_game_id
            ))
            throw std::runtime_error("save failed: " + status_);
        std::cout << "saveload: saved " << path_to_utf8(target) << " at tick " << match_timing_.tick
                  << "; save failures " << saveload_state().save_failures << '\n';
        saved = true;
    };
    save_if_due();
    // Shortly before the save, features start burning, dying and being
    // reclaimed, so the save holds them while they play.
    constexpr uint32_t feature_event_lead = 2;
    const std::size_t ticks = options_.match_ticks.value_or(0);
    auto outcome = match_->outcome();
    for (std::size_t step = 0; step < ticks; ++step) {
        if (options_.give_orders && options_.save_after &&
            match_timing_.tick + feature_event_lead == *options_.save_after)
            give_saveload_feature_events();
        try {
            step_match_simulation();
        } catch (const std::exception& failure) {
            report_match_tick_error(failure.what());
        }
        save_if_due();
        // The first victory or defeat the ticks decide.
        const auto decided = match_->outcome();
        if (outcome == sim::scenario::Outcome::ongoing &&
            (decided == sim::scenario::Outcome::victory ||
             decided == sim::scenario::Outcome::defeat)) {
            outcome = decided;
            std::printf(
                "saveload: outcome %s at tick %u\n",
                outcome == sim::scenario::Outcome::victory ? "victory" : "defeat",
                match_timing_.tick
            );
        }
    }
    std::size_t live = 0;
    for (const auto& slot : match_->world().slots)
        live += slot.unit != nullptr && slot.record.type_index != 0 ? 1 : 0;
    print_saved_orders();
    print_saved_units();
    print_saved_features();
    // The limit the match plays at, its setting as a save writes it, and the
    // run-wide limit a new skirmish would take.
    const oa::Game& game = match_->state().game;
    std::printf(
        "saveload: units per player %u setting %u run-wide %u\n",
        static_cast<unsigned>(game.units_per_player),
        static_cast<unsigned>(game.max_units_setting),
        static_cast<unsigned>(SaveLoadState::run_unit_limit(*this))
    );
    std::printf(
        "saveload: tick %u units %zu digest %016llx\n",
        match_timing_.tick,
        live,
        static_cast<unsigned long long>(match_world_digest())
    );
    std::fflush(stdout);
    if (!options_.snapshot.empty()) {
        rebuild_surface();
        write_ppm(options_.snapshot, surface_);
    }
    print_memory_status();
}

} // namespace oa::app
