// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The SIDEDATA side table, the player records of an offline match, and the
// viewpoint side of the deathmatch commander respawn.
#include "oa/app/runtime.hpp"

#include "oa/app/asset_files.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/app/mod_profile_loader.hpp"
#include "oa/data/defs/files.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/sim/scenario/commander_rules.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/sim/selection.hpp"
#include "oa/present/world_renderer/world_camera.hpp"
#include "oa/ui/campaign/single_player.hpp"
#include "match_fault.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace oa::app {
namespace {

// Message box width the watch-mode notice opens with.
constexpr int32_t kWatchMessageWidth = 500;

#if OA_SELF_CHECKS
constexpr uint32_t kRespawnTickLimit = 30 * 20;
// Start storage the game grants the respawned commander's player from the
// no-player record's player-info block, which holds no resources.
constexpr float kRespawnStorageFloor = 200.0F;

// The player's first live unit of its side's commander type, or null.
const oa::Unit* live_side_commander(oa::World& world, uint8_t index) {
    oa::Player* player = oa::world_player(&world, index);
    const oa::PlayerSetupInfo* info =
        player != nullptr ? oa::world_player_info(&world, player) : nullptr;
    if (info == nullptr || info->side >= OA_SIDE_COUNT)
        return nullptr;
    uint32_t count = 0;
    const oa::Unit* units = oa::world_player_units(&world, player, &count);
    for (uint32_t i = 0; i < count; ++i) {
        const oa::UnitDef* def = oa::world_unit_def_of(&world, &units[i]);
        if ((units[i].flags & OA_UNIT_FLAG_LIVE) != 0 && def != nullptr &&
            std::strcmp(def->unit_name, world.game.sides[info->side].commander) == 0)
            return &units[i];
    }
    return nullptr;
}
#endif

/// Returns the report that ends a start whose sides name a file the game's
/// files lack.
///
/// @param table the side table
/// @param missing the first missing file
/// @return the file's path and the side that names it
std::string side_file_report(
    const oa::data::defs::SideTable& table, const oa::data::defs::SideMissingFile& missing
) {
    const auto& name = table.sides[missing.side].name;
    return std::string(missing.path) + ", which GAMEDATA/SIDEDATA.TDF names for the " +
           std::string(name, strnlen(name, sizeof name)) + " side, is missing";
}

/// Returns what ends a start whose side table loaded so: the first section a
/// side lacks, or no side at all.
///
/// @param loaded the file loaded and every side was complete
/// @param table the side table
/// @return the report; empty when the table can be played
std::string side_table_problem(bool loaded, const oa::data::defs::SideTable& table) {
    if (!loaded && table.error[0] != '\0')
        return table.error;
    if (table.count == 0)
        return "gamedata/sidedata.tdf contains no side definitions";
    return {};
}

/// Returns the first file a side table's sides name that the files lack.
///
/// @param files the game's files
/// @param table the side table
/// @param language the language word whose folders are looked in first
/// @return the report; empty when every file is there
std::string first_side_file_gap(
    const oa::data::defs::Files& files, const oa::data::defs::SideTable& table, const char* language
) {
    std::array<oa::data::defs::SideMissingFile, oa::data::defs::side_missing_file_capacity>
        missing{};
    if (oa::data::defs::side_missing_files(&files, table, language, missing) == 0)
        return {};
    return side_file_report(table, missing.front());
}

} // namespace

void Runtime::load_side_table() {
    // The language the game starts in, whose folders are looked in first for
    // SIDEDATA and the files its sides name, all read once for the run.
    const char* language = game_language();
    side_files_language_ = language != nullptr ? language : "";
    const oa::data::defs::Files files = asset_files(assets_);
    // Side.font stays null: the match HUD loads the viewed side's font by its
    // name and nothing reads the handle.
    const bool loaded =
        oa::data::defs::load_side_data(&files, &side_table_, side_files_language_.c_str(), nullptr);
    if (auto problem = side_table_problem(loaded, side_table_); !problem.empty())
        throw std::runtime_error(problem);
    skirmish_ui_.side_count = static_cast<int32_t>(side_table_.count);
}

std::string Runtime::side_data_path() const {
    const oa::data::defs::Files files = asset_files(assets_);
    char path[oa::data::defs::path_capacity];
    oa::data::defs::build_variant_path(
        &files,
        path,
        sizeof path,
        oa::data::defs::directory_name(oa::data::defs::DataDirectory::gamedata),
        "sidedata",
        "tdf",
        side_files_language_.c_str()
    );
    return path;
}

std::vector<oa::data::defs::SideMissingFile> Runtime::missing_side_files() const {
    const oa::data::defs::Files files = asset_files(assets_);
    std::array<oa::data::defs::SideMissingFile, oa::data::defs::side_missing_file_capacity>
        missing{};
    const auto count = oa::data::defs::side_missing_files(
        &files, side_table_, side_files_language_.c_str(), missing
    );
    return {missing.begin(), missing.begin() + std::min<std::size_t>(count, missing.size())};
}

void Runtime::require_side_files() {
    // A mod's games start without them, and it warns of them.
    if (plays_mod())
        return;
    const oa::data::defs::Files files = asset_files(assets_);
    if (auto gap = first_side_file_gap(files, side_table_, side_files_language_.c_str());
        !gap.empty())
        throw std::runtime_error(gap);
}

std::string Runtime::side_data_problem_over(
    const fs::path& folder, const fs::path& game_folder, const char* language
) const {
    // The mod folder over the game folder, with the archives discovery mounts
    // from both, as that start mounts them.
    oa::AssetStore probe(std::vector<fs::path>{folder, game_folder});
    std::ignore = probe.discover(discovery_plan_of(nullptr));
    const oa::data::defs::Files files = asset_files(probe);
    // Without a profile the base game's layout names the data's folders.
    const oa::data::defs::DataLayout base_layout;
    char path[oa::data::defs::path_capacity];
    oa::data::defs::build_variant_path(
        &files,
        path,
        sizeof path,
        base_layout.directories[static_cast<std::size_t>(oa::data::defs::DataDirectory::gamedata)]
            .c_str(),
        "sidedata",
        "tdf",
        language
    );
    oa::formats::tdf::Document document;
    oa::formats::tdf::document_init(&document);
    // As load_side_data reads it.
    const bool read = oa::data::defs::load_tdf_file(&files, path, &document, nullptr);
    const auto table = std::make_unique<oa::data::defs::SideTable>();
    const bool complete = oa::data::defs::side_table_load(&document, table.get(), nullptr);
    oa::formats::tdf::document_free(&document);
    return side_table_problem(read && complete, *table);
}

std::vector<std::string> Runtime::saved_game_side_names() const {
    namespace campaign = oa::ui::campaign;
    static_assert(sizeof(oa::Side::name) == campaign::kSideNameBytes);
    char names[OA_SIDE_COUNT][campaign::kSideNameBytes] = {};
    const auto count = std::min<uint32_t>(side_table_.count, OA_SIDE_COUNT);
    for (uint32_t side = 0; side < count; ++side)
        std::memcpy(names[side], side_table_.sides[side].name, campaign::kSideNameBytes);
    // The dialogs' side list: the names one after another, each ended by a
    // NUL, and one more NUL after the last.
    // The list is read to its closing NUL, so its length is not needed.
    char list[OA_SIDE_COUNT * campaign::kSideNameBytes + 1] = {};
    std::ignore = campaign::build_side_name_list(names, count, list, sizeof list);
    std::vector<std::string> shown;
    for (const char* name = list; *name != '\0'; name += std::strlen(name) + 1)
        shown.emplace_back(name);
    return shown;
}

void Runtime::bind_player_records(oa::World& world) {
    std::copy(std::begin(side_table_.sides), std::end(side_table_.sides), world.game.sides);
    world.game.side_count = side_table_.count;
    for (uint32_t slot = 0; slot < OA_PLAYER_RECORD_COUNT; ++slot) {
        oa::PlayerSetupInfo& info = world.player_info[slot];
        info = {};
        info.color = static_cast<uint8_t>(slot);
        oa::world_player_record(&world, slot)->info = oa::oa_ref_from_index(slot);
        if (slot == OA_PLAYER_COUNT)
            continue;
        const auto& setup = skirmish_settings_.slots[slot];
        if (setup.controller == entry::controller::disabled)
            continue;
        info.side = static_cast<uint8_t>(setup.side);
        info.color = static_cast<uint8_t>(setup.color);
        info.state = static_cast<uint8_t>(setup.controller);
    }
    world.player_info[match_local_player_].options |= OA_SETUP_OPTION_STARTED;
}

void Runtime::bind_respawn_view() {
    match_->respawn = {
        this,
        [](void* context) { static_cast<Runtime*>(context)->reset_sight_presentation(true); },
        [](void* context) { static_cast<Runtime*>(context)->select_side_commander(); },
    };
    match_->watch = {
        this, [](void* context, oa::sim::match_runtime::Match::WatchNotice notice) {
            auto& self = *static_cast<Runtime*>(context);
            self.reset_sight_presentation(true);
            if (notice == oa::sim::match_runtime::Match::WatchNotice::continue_prompt) {
                auto ctx = self.screen_context();
                oa::ui::frontend_dialogs::open_continue_watching(
                    &ctx, &self, [](void* context, bool keep) {
                        auto& runtime = *static_cast<Runtime*>(context);
                        if (runtime.match_ != nullptr) {
                            runtime.match_->choose_continue_watching(keep);
                            if (keep)
                                call_hook_or_report<&Extension::match_event>(
                                    runtime.extension_,
                                    runtime.hook_error_report(),
                                    runtime,
                                    MatchEvent::watching_kept
                                );
                        }
                    }
                );
            }
            if (notice == oa::sim::match_runtime::Match::WatchNotice::host_watching)
                self.show_frontend_message(
                    "You have been defeated.  You stay in the game as a watcher so that it goes "
                    "on for the other players.",
                    kWatchMessageWidth,
                    1,
                    1
                );
            if (notice == oa::sim::match_runtime::Match::WatchNotice::hosting_computers)
                self.show_frontend_message(
                    "You are placed in watch mode because you are hosting AI players which are "
                    "still alive.  If you exit, they will be terminated.",
                    kWatchMessageWidth,
                    1,
                    1
                );
        }
    };
}

void Runtime::reset_sight_presentation(bool refill_mapped) {
    if (refill_mapped)
        radar_explored_.clear();
    radar_state_.reset_sight = true;
}

void Runtime::reset_match_sight(bool refill_mapped) {
    if (!match_ || altitude_sight_blocked_)
        return;
    match_->reset_sight_buffers(refill_mapped);
    reset_sight_presentation(refill_mapped);
}

void Runtime::select_side_commander() {
    if (!match_)
        return;

    struct Finder {
        Runtime* runtime{};
        bool cleared{};
    } finder{this};

    oa::sim::selection::Hooks hooks{};
    hooks.context = &finder;
    hooks.stop_follow = [](void* context) {
        Runtime& runtime = *static_cast<Finder*>(context)->runtime;
        oa::present::world_renderer::camera_stop_follow(runtime.match_->state().game);
        runtime.stop_match_tracking();
    };
    // The camera has no glide target and moves at once.
    hooks.center_camera = [](void* context, const FixedVec3& position, bool) {
        Runtime& runtime = *static_cast<Finder*>(context)->runtime;
        runtime.match_camera_x_ =
            oa::present::world_renderer::world_screen_x(position) - runtime.visible_map_width() / 2;
        runtime.match_camera_z_ = oa::present::world_renderer::world_screen_y(position) -
                                  runtime.visible_map_height() / 2;
    };
    hooks.reset_command = [](void* context) {
        static_cast<Finder*>(context)->runtime->reset_match_command();
    };
    hooks.selection_cleared = [](void* context) {
        auto& finder = *static_cast<Finder*>(context);
        finder.cleared = true;
        finder.runtime->selected_match_unit_ = 0;
    };
    oa::World& world = match_->state();
    oa::sim::selection::find_commander(world, true, hooks);
    if (!finder.cleared)
        return;
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot)
        if ((world.units[slot].flags & OA_UNIT_FLAG_SELECTED) != 0)
            adopt_selection(static_cast<uint16_t>(slot));
    apply_match_hud_for_selection();
}

void Runtime::select_and_follow_commander(bool add) {
    if (!match_)
        return;
    oa::World& world = match_->state();
    auto& categories = unit_table_.tables.categories;
    const auto* mask = oa::data::defs::category_registry_find_or_add(
        &categories, oa::sim::selection::ctrl_c_category
    );
    if (mask == nullptr)
        return;
    oa::sim::selection::Hooks hooks{};
    hooks.context = this;
    hooks.reset_command = [](void* context) {
        static_cast<Runtime*>(context)->reset_match_command();
    };
    oa::sim::selection::apply_type_mask_selection(world, *mask, add, hooks);
    world.game.follow_unit = match_tracking_ ? oa::oa_unit_ref_from_slot(tracked_match_unit_) : 0u;
    oa::sim::selection::follow_commander(world, categories);
    if (world.game.follow_unit != 0u) {
        tracked_match_unit_ =
            static_cast<uint16_t>(oa::oa_unit_slot_from_ref(world.game.follow_unit));
        match_tracking_ = true;
    }
    selected_match_unit_ = 0;
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot)
        if ((world.units[slot].flags & OA_UNIT_FLAG_SELECTED) != 0)
            adopt_selection(static_cast<uint16_t>(slot));
    apply_match_hud_for_selection();
}

void Runtime::check_player_records(std::string_view context) {
    const std::string label(context);
    if (!match_)
        throw std::runtime_error(label + " player check needs a match");
    oa::World& world = match_->state();
    // Game.sides holds each SIDEDATA side, with the commander it names.
    if (world.game.side_count < 2 || world.game.side_count != side_table_.count)
        throw std::runtime_error(label + " player check: Game.sides lacks the SIDEDATA sides");
    for (uint32_t side = 0; side < world.game.side_count; ++side)
        if (std::strcmp(world.game.sides[side].commander, side_table_.sides[side].commander) != 0)
            throw std::runtime_error(
                label + " player check: Game.sides " + std::to_string(side) +
                " does not name its SIDEDATA commander"
            );
    uint32_t bound = 0;
    for (uint32_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        const auto& setup = skirmish_settings_.slots[slot];
        if (setup.controller == entry::controller::disabled)
            continue;
        const oa::PlayerSetupInfo* info = oa::world_player_info(&world, &world.game.players[slot]);
        if (info == nullptr || info->side != setup.side || info->color != setup.color ||
            info->state != setup.controller)
            throw std::runtime_error(
                label + " player check: Player.info of player " + std::to_string(slot) +
                " does not carry its slot"
            );
        ++bound;
    }
    if (bound < 2)
        throw std::runtime_error(label + " player check: fewer than two players are bound");
    std::cout << label << " player check: Game.sides " << world.game.sides[0].commander << "/"
              << world.game.sides[1].commander << ", Player.info bound for " << bound
              << " players\n";
}

// A step of the self-checks (runtime_checks.cpp), which a build without them
// leaves out.
#if OA_SELF_CHECKS
void Runtime::check_deathmatch_respawn() {
    exercise_click(skirmish::resource_name(skirmish::Button::start));
    if (screen_ != Screen::match || !match_)
        throw std::runtime_error("respawn check: Start did not enter a match");
    check_player_records("skirmish");
    oa::World& world = match_->state();
    const uint8_t local = match_local_player_;
    oa::Player& player = world.game.players[local];
    if (sim::scenario::host_player_index(world) != OA_PLAYER_COUNT)
        throw std::runtime_error("respawn check: the skirmish seats a host");
    world.game.session_rules = static_cast<int32_t>(sim::scenario::CommanderRule::deathmatch);
    clear_local_selection();
    match_->destroy_player_units(local);
    bool defeated = false;
    const oa::Unit* commander = nullptr;
    for (uint32_t step = 0; step < kRespawnTickLimit && commander == nullptr; ++step) {
        ++match_timing_.tick;
        match_->simulation().tick = match_timing_.tick;
        tick_or_raise(*match_);
        if (player.unit_count == 0)
            defeated = true;
        else if (defeated)
            commander = live_side_commander(world, local);
    }
    if (!defeated)
        throw std::runtime_error("respawn check: the local units survived the sweep");
    if (commander == nullptr || match_->outcome() != sim::scenario::Outcome::ongoing)
        throw std::runtime_error("respawn check: no commander respawned under the deathmatch rule");
    if (selected_match_unit_ != commander->id || (commander->flags & OA_UNIT_FLAG_SELECTED) == 0)
        throw std::runtime_error("respawn check: the respawned commander is not selected");
    const int32_t centre_x = oa::present::world_renderer::world_screen_x(commander->position);
    const int32_t centre_z = oa::present::world_renderer::world_screen_y(commander->position);
    if (match_camera_x_ != centre_x - visible_map_width() / 2 ||
        match_camera_z_ != centre_z - visible_map_height() / 2)
        throw std::runtime_error("respawn check: the camera is not centred on the new commander");
    if (player.shared_metal_storage != kRespawnStorageFloor ||
        player.shared_energy_storage != kRespawnStorageFloor)
        throw std::runtime_error(
            "respawn check: the start storage did not come from the no-player record"
        );
    std::cout << "respawn check: " << oa::world_unit_def_of(&world, commander)->unit_name
              << " respawned at tick " << match_timing_.tick << " (" << centre_x << ", " << centre_z
              << "), selected and centred, start storage " << player.shared_metal_storage << "\n";
    return_to_skirmish_menu();
}
#endif

} // namespace oa::app
