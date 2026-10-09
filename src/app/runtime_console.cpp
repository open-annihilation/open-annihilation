// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// In-match chat line and "+command" console, bound to the canonical World of
// the running match. The console reaches the systems the runtime owns
// (audio, files, savegames, units, features, AI profiles) through the
// ConsoleHost and HotkeyHost callbacks bound here.
#include "oa/app/runtime.hpp"
#include "engine_settings_state.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/app/match_console.hpp"
#include "oa/app/asset_files.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/base/text.hpp"
#include "oa/formats/cob.hpp"
#include "oa/formats/fnt.hpp"

#include "oa/sim/ai.hpp"
#include "oa/data/campaign/campaign_file.hpp"
#include "oa/data/defs/categories.hpp"
#include "oa/sim/feature_runtime.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/match_runtime/mission_unit_binding.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/data/persist/save_sections.hpp"
#include "oa/sim/mission_units.hpp"
#include "oa/ui/console/console.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/console/hotkeys.hpp"
#include "oa/ui/frontend/savegame_dialogs.hpp"
#include "oa/ui/hud/chat_panel.hpp"
#include "oa/ui/hud/game_clock.hpp"
#include "oa/ui/hud/status_panel.hpp"
#include "oa/ui/hud/share_panel.hpp"
#include "oa/platform/files.hpp"
#include "oa/present/game_text.hpp"
#include "match_fault.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>
#include <tuple>

namespace oa::app {

namespace console = oa::ui::console;

namespace {

/// The rows the chat line keeps clear above and below its text where it
/// stands over the battlefield, at the HUD's text scale.
constexpr int chat_field_margin_rows = 2;
/// The rows a line of HUD text takes, at the HUD's text scale, where the
/// game's label font has not loaded.
constexpr int chat_field_fallback_rows = 12;

constexpr uint8_t kConsolePlacer = 10; // placing player "Feature" records: none
constexpr uint32_t kCellShift = 20;    // 16.16 world units to 16-pixel map cells
static_assert(console::kMessageNoSender == oa::sim::messages::sender_none);

Runtime* runtime_of(void* context) noexcept {
    return static_cast<Runtime*>(context);
}

// Resource paths the console reads use '\' separators.
std::string resource_path(const char* path) {
    std::string relative(path != nullptr ? path : "");
    std::replace(relative.begin(), relative.end(), '\\', '/');
    return relative;
}

/// Returns the origin plot of the feature covering a plot.
///
/// A footprint cell (feature_continuation, 0xfffe) steps back by the z and x
/// distances the two bytes of its MapPlot.feature_record hold (feature_back_z,
/// feature_back_x).
///
/// @param plots the match's plots
/// @param index plot to resolve
/// @param width map width in plots
/// @return the index of the plot that holds the feature's origin, or index
///         when the plot is not a footprint cell or the step back would pass
///         the first plot
std::size_t feature_origin(
    std::span<const oa::sim::spatial_state::Plot> plots, std::size_t index, std::size_t width
) noexcept {
    const auto& plot = plots[index];
    if (plot.feature_word != oa::sim::spatial_state::feature_continuation)
        return index;
    const auto back = static_cast<std::size_t>(plot.feature_back_z) * width + plot.feature_back_x;
    return back <= index ? index - back : index;
}

// "SFX" flips the flag the emitter pool checks
// before every particle emitter it hands out.
void sync_effects_toggle(oa::sim::match_runtime::Match* match, const console::Console& console) {
    if (match != nullptr)
        match->effects().emitters_refused = console.sfx_flag;
}

} // namespace

void Runtime::console_post_message(std::string_view text, uint8_t kind, uint8_t sender) {
    post_match_message(text, kind, 0, sender);
    status_ = std::string(text);
}

void Runtime::take_console_capture_options(oa::Game& game) {
    if (game.output_directory_changed != 0) {
        const char* directory = game.output_directory;
        preferences_.image_output_directory =
            std::string(directory, strnlen(directory, sizeof game.output_directory - 1));
        preferences_.image_output_directory_changed = 1;
        game.output_directory_changed = 0U;
    }
    if (game.capture_rate_changed != 0) {
        preferences_.movie_output_rate = static_cast<uint32_t>(game.capture_rate);
        preferences_.movie_output_rate_changed = 1;
        game.capture_rate_changed = 0U;
    }
}

console::Console* Runtime::match_console() {
    if (!match_)
        return nullptr;
    oa::World* world = &match_->state();
    if (!console_)
        console_ = std::make_shared<MatchConsole>();
    if (console_->bound_world != world) {
        auto& host = console_->host;
        host = {};
        host.context = this;
        host.difficulty_names = difficulty_names();
        // The options save reads the Game block: the match's option fields go
        // into the preferences first, and Film and FilmSpeed flag theirs for
        // this save.
        host.save_game_options = [](void* context) {
            auto* runtime = runtime_of(context);
            if (runtime->match_)
                runtime->take_match_options(runtime->match_->state().game);
            runtime->save_preferences();
        };
        host.post_message = [](void* context, const char* text, uint8_t kind, uint8_t sender) {
            runtime_of(context)->console_post_message(text, kind, sender);
        };
        host.compact_render_cache = [](void* context) {
            runtime_of(context)->release_model_images();
        };
        host.set_gamma = [](void* context, float gamma) {
            runtime_of(context)->set_display_gamma(gamma);
        };
        host.set_lighting = [](void* context, int32_t x, int32_t y, int32_t z) {
            runtime_of(context)->set_match_model_light(x, y, z);
        };
        host.logo_count = [](void* context) -> uint16_t {
            return runtime_of(context)->color_frame_count();
        };
        host.play_cd_track = [](void* context, int32_t track) {
            runtime_of(context)->music_cd_play(track);
        };
        host.stop_cd = [](void* context) { runtime_of(context)->music_cd_stop(); };
        host.set_music_mode = [](void* context, int32_t kind) {
            runtime_of(context)->music_mode(kind);
        };
        host.transfer_metal = [](void* context, uint8_t from, uint8_t to, float amount) {
            runtime_of(context)->console_give(from, to, amount, true);
        };
        host.transfer_energy = [](void* context, uint8_t from, uint8_t to, float amount) {
            runtime_of(context)->console_give(from, to, amount, false);
        };
        host.kill_player_units = [](void* context, uint8_t player) {
            if (auto& match = runtime_of(context)->match_)
                match->destroy_player_units(player);
        };
        host.kill_all_units = [](void* context) { runtime_of(context)->console_kill_all_units(); };
        host.disable_mission_conditions = [](void* context) {
            if (auto& match = runtime_of(context)->match_)
                match->disable_scenario();
        };
        host.kill_units_of_type = [](void* context, uint16_t type) {
            runtime_of(context)->console_kill_units_of_type(type);
        };
        // Unit creation as the spawn command calls it: finished, on the ground.
        host.create_unit =
            [](void* context, uint8_t player, uint16_t type, const FixedVec3* position) {
                auto* runtime = runtime_of(context);
                oa::sim::unit_spawn::Request request;
                request.player = player;
                request.type = type;
                request.finished = true;
                request.state = kGroundOccupancyState;
                request.position = {
                    static_cast<uint32_t>(position->x),
                    static_cast<uint32_t>(position->y),
                    static_cast<uint32_t>(position->z)
                };
                // A unit the spawn refuses, for a full pool or a blocked
                // place, is not created and nothing more happens.
                try {
                    std::ignore = runtime->match_->create(request);
                } catch (const std::exception& error) {
                    runtime->console_post_message(std::string("spawn: ") + error.what());
                }
            };
        host.reload_unit_type = [](void* context, uint16_t type) {
            runtime_of(context)->console_reload_unit_type(type);
        };
        host.find_unit_type = [](void* context, const char* name) -> uint16_t {
            return oa::sim::unit_spawn::find_type_index(
                runtime_of(context)->spawn_type_names_, name
            );
        };
        host.start_meteor_storm = [](void* context) { runtime_of(context)->start_meteor_strike(); };
        host.set_meteor_enabled = [](void* context, bool enabled) {
            runtime_of(context)->set_meteor_enabled(enabled);
        };
        host.clear_all_features = [](void* context) {
            runtime_of(context)->console_burn_all_features();
        };
        // Clears the cursor's feature with force; a cell off the map or
        // without a feature is left as it is.
        host.clear_feature_at = [](void* context, int16_t cell_x, int16_t cell_z) {
            std::ignore = runtime_of(context)->console_burn_feature(cell_x, cell_z, true);
        };
        host.place_feature_at =
            [](void* context, const char* feature, int16_t cell_x, int16_t cell_z) {
                return runtime_of(context)->console_place_feature(feature, cell_x, cell_z);
            };
        host.apply_ai_weight = [](void* context, uint8_t player, const char* type, float percent) {
            auto* players = oa::sim::ai::match_computer_players(*runtime_of(context)->match_);
            oa::sim::ai::computer_apply_weight(players, player, type, percent);
        };
        host.apply_ai_limit = [](void* context, uint8_t player, const char* type, int32_t limit) {
            auto* players = oa::sim::ai::match_computer_players(*runtime_of(context)->match_);
            oa::sim::ai::computer_apply_limit(players, player, type, limit);
        };
        host.reload_ai_profiles = [](void* context) {
            runtime_of(context)->reload_computer_profiles();
        };
        // The report opens the named file "w+b" relative to the working
        // directory; the report prints the map context's terrain and AI
        // profile path slots.
        host.write_ai_weights = [](void* context, uint8_t player, const char* path) {
            auto* runtime = runtime_of(context);
            if (!runtime->match_)
                return;
            std::FILE* file = oa::platform::open_file(path, "w+b");
            if (file == nullptr)
                return;
            const auto* map = runtime->match_map_context();
            namespace missions = oa::data::campaign;
            oa::sim::ai::write_match_computer_report(
                *runtime->match_,
                player,
                {map != nullptr ? missions::campaign_path(map, missions::CampaignPath::mission)
                                : nullptr,
                 map != nullptr ? missions::campaign_path(map, missions::CampaignPath::ai_profile)
                                : nullptr},
                file
            );
            std::fclose(file);
        };
        host.read_text_file = [](void* context, const char* path, int32_t* length) -> char* {
            return runtime_of(context)->console_read_text_file(path, length);
        };
        host.free_text_file = [](void*, char* text) { std::free(text); };
        // Files 3.1c writes in the game directory go to the save root, saved
        // games and captures to the player's own folder.
        host.create_directories = [](void* context, const char* path) {
            std::error_code error;
            fs::create_directories(
                runtime_of(context)->game_file_path(path, ui::frontend::SavePathUse::write), error
            );
        };
        // Creates or truncates the file.
        host.touch_file = [](void* context, const char* path) {
            const auto target =
                runtime_of(context)->game_file_path(path, ui::frontend::SavePathUse::write);
            std::ofstream(target, std::ios::binary | std::ios::trunc).flush();
        };
        host.save_game =
            [](void* context, const char* path, const char* description, int32_t game_id) {
                auto* runtime = runtime_of(context);
                // The status line holds why a save failed; it goes to the log,
                // as the save dialog's failures do.
                try {
                    if (!runtime->save_match_game(
                            runtime->game_file_path(path, ui::frontend::SavePathUse::write),
                            description,
                            game_id
                        ))
                        std::cerr << "open-annihilation: " << runtime->status_ << '\n';
                } catch (const std::exception& failure) {
                    runtime->status_ = std::string("Save: ") + failure.what();
                    std::cerr << "open-annihilation: " << runtime->status_ << '\n';
                }
            };
        host.now_ms = [](void* context) -> uint32_t {
            return runtime_of(context)->clock_milliseconds();
        };
        host.reset_sight_buffers = [](void* context, bool refill_mapped) {
            runtime_of(context)->reset_match_sight(refill_mapped);
        };
        host.crash_test = [](void* context, console::CrashTest test) {
            runtime_of(context)->run_console_crash_test(test);
        };
        host.toggle_sound_3d = [](void* context) {
            auto& spatial = runtime_of(context)->sound_spatial_;
            spatial = spatial == 0 ? 1 : 0;
        };
        host.toggle_novelty_voice = [](void* context) {
            runtime_of(context)->toggle_novelty_voice();
        };
        call_hook_or_raise<&Extension::console_host>(extension_, *this, host);
        // The team panels' host starts empty for each match; the extension
        // fills what reaches the other players' machines.
        team_panel_host_ = {};
        team_panel_host_.context = this;
        call_hook_or_raise<&Extension::team_panel_host>(extension_, *this, team_panel_host_);
        // The match's menus and end-of-game screen take the return label.
        take_return_label();
        host.issue_group_mission =
            [](void* context, uint8_t kind, int32_t parameter_1, int32_t parameter_2) {
                runtime_of(context)->issue_group_mission(kind, parameter_1, parameter_2);
            };
        host.set_search_node_credit = [](void* context, int32_t nodes) {
            if (auto& match = runtime_of(context)->match_)
                match->path_search_jobs().tick_credit = nodes;
        };
        host.set_search_heuristic = [](void* context, int32_t weight) {
            if (auto& match = runtime_of(context)->match_)
                match->path_search_jobs().base_heuristic = weight;
        };
        host.render_poster = [](void* context,
                                const char* directory,
                                const char* prefix,
                                int32_t x,
                                int32_t y,
                                int32_t width,
                                int32_t height) {
            runtime_of(context)->render_poster(directory, prefix, x, y, width, height);
        };
        // The posters' folder when it is too long for Game.output_directory.
        host.output_directory = [](void* context) -> const char* {
            return runtime_of(context)->preferences_.image_output_directory.c_str();
        };
        // Not bound: build snapping, which only changes its own state.
        // "Contour" keeps its spacing from game to game, as in 3.1c.
        int32_t contour[2];
        std::copy(
            std::begin(console_->state.contour_values),
            std::end(console_->state.contour_values),
            contour
        );
        if (!console::console_init(&console_->state, world, &console_->host))
            throw std::runtime_error("the console's command table refused its own commands");
        std::copy(std::begin(contour), std::end(contour), console_->state.contour_values);
        // "+stats" shows or hides the frame statistics over the battlefield
        // (draw_frame_stats) and saves the choice, as the Show performance
        // statistics setting does; "+stats 1" and "+stats 0" show and hide
        // them for this run without saving.
        // 3.1c has no command that shows these times; its frame rate shows
        // on the debug keys' line (draw_debug_status_line). It is an option:
        // it needs no passphrase and echoes to this machine alone.
        if (!oa::ui::services::command_table_set(
                &console_->state.commands,
                "Stats",
                [](oa::ui::services::TokenLine* line) {
                    const console::Console* active = console::console_active();
                    if (active == nullptr || active->host == nullptr)
                        return;
                    auto* runtime = runtime_of(active->host->context);
                    const int32_t asked = oa::ui::services::token_line_get_int(line, 1, -1);
                    if (asked < 0)
                        EngineSettingsState::save_frame_stats(
                            *runtime, !runtime->frame_stats_shown_
                        );
                    else
                        runtime->show_frame_stats(asked != 0);
                },
                console::command_class::option | console::command_class::private_echo
            ))
            throw std::runtime_error("the console's command table refused Stats");
        // A mod's rules move commands between classes and change ATM and
        // the console keys.
        console::console_apply_rules(
            &console_->state, match_ ? match_->rules() : oa::data::match_rules::MatchRules{}
        );
        console_->bound_world = world;
        restore_console_carry();
    }
    // The chat line adds the cheat class while the session's flag is set.
    console_->state.cheats_enabled = session_cheats_allowed_;
    return &console_->state;
}

void Runtime::take_return_label() {
    return_label_.fill('\0');
    const char* label = call_hook_or_raise<&Extension::return_label>(extension_);
    if (label != nullptr)
        std::copy_n(label, ::strnlen(label, return_label_.size() - 1U), return_label_.begin());
}

oa::data::campaign::SessionKind Runtime::match_session_kind() const {
    if (campaign_mission_)
        return oa::data::campaign::SessionKind::campaign;
    constexpr uint32_t kShared = extension_state::shared_match | extension_state::replay;
    return (current_extension_state() & kShared) != 0 ? oa::data::campaign::SessionKind::multiplayer
                                                      : oa::data::campaign::SessionKind::skirmish;
}

bool Runtime::local_player_watches() const {
    if (!match_)
        return false;
    // A replay that leaves the viewer no slot of its own seats it in a
    // recorded player's slot, but it only watches.
    if (match_->slotless_viewer() != oa::sim::match_runtime::SlotlessViewer::none)
        return true;
    const oa::World& world = match_->state();
    const auto* local = oa::world_player_record(&world, world.game.local_player_index);
    const auto* info = local != nullptr ? oa::world_player_info(&world, local) : nullptr;
    return info != nullptr && (info->options & OA_SETUP_OPTION_WATCHER) != 0;
}

void Runtime::console_give(uint8_t from, uint8_t to, float amount, bool metal) {
    if (!match_ ||
        call_hook_or_raise<&Extension::give_resources>(extension_, *this, from, to, amount, metal))
        return;
    oa::World& world = match_->state();
    std::array<oa::UnitEconomy*, OA_PLAYER_COUNT> economies{};
    std::array<uint8_t, OA_PLAYER_COUNT> info_options{};
    for (std::size_t i = 0; i < OA_PLAYER_COUNT; ++i) {
        oa::Player& player = world.game.players[i];
        economies[i] = oa::world_player_economy(&world, &player);
        if (const auto* info = oa::world_player_info(&world, &player); info != nullptr)
            info_options[i] = static_cast<uint8_t>(info->options);
    }
    oa::ui::hud::ShareWorld share{
        world.game.players,
        economies.data(),
        info_options.data(),
        match_local_player_,
        world.game.difficulty
    };
    const oa::ui::hud::ShareHost offline{};
    if (metal)
        oa::ui::hud::transfer_metal(share, from, to, amount, true, offline);
    else
        oa::ui::hud::transfer_energy(share, from, to, amount, true, offline);
}

void Runtime::console_kill_all_units() {
    if (!match_)
        return;
    oa::sim::match_runtime::MissionUnitBinding binding{*match_, {}};
    oa::sim::mission_units::kill_all_units(
        match_->state(), oa::sim::match_runtime::mission_unit_hooks(binding)
    );
}

void Runtime::console_kill_units_of_type(uint16_t type) {
    if (!match_)
        return;
    oa::sim::match_runtime::MissionUnitBinding binding{*match_, {}};
    oa::sim::mission_units::kill_units_of_type(
        match_->state(), type, oa::sim::match_runtime::mission_unit_hooks(binding)
    );
}

void Runtime::console_reload_unit_type(uint16_t type) {
    if (!match_)
        return;
    const oa::data::defs::Files files = asset_files(assets_);
    const oa::data::defs::UnitDefLoadHost corpses{
        &feature_table_, [](void* context, const char* name) {
            const auto& table = *static_cast<const oa::sim::map_runtime::FeatureDefTable*>(context);
            return static_cast<int16_t>(oa::sim::map_runtime::find_feature_index(table, name));
        }
    };
    const oa::data::defs::UnitDefSources sources{
        "",
        &unit_table_.move_classes,
        match_->state().game.weapon_defs,
        &unit_table_.sound_categories,
        &unit_table_.tables.categories,
        &unit_table_.tables.blocks,
        &corpses,
        mod_profile() != nullptr ? oa::data::defs::yard_map_rules(mod_profile()->rules.units)
                                 : oa::data::defs::YardMapRules{},
        unit_text_sink()
    };
    const oa::data::defs::UnitScriptLoader scripts{
        this, [](void* context, uint16_t reloaded, const char* path) {
            auto& runtime = *runtime_of(context);
            auto& loaded = runtime.loaded_commander_types_.at(reloaded);
            loaded.script.reset();
            if (const auto bytes = runtime.read(path)) {
                auto parsed = oa::formats::cob::parse_cob(*bytes);
                if (parsed.ok())
                    loaded.script = std::make_shared<const oa::formats::cob::CobProgram>(
                        std::move(*parsed.value)
                    );
                else
                    runtime.console_post_message(std::string(path) + ": " + parsed.error.message);
            }
            loaded.type.cob =
                reinterpret_cast<oa::sim::unit_spawn::AssetHandle>(loaded.script.get());
            runtime.spawn_types_.at(reloaded).cob = loaded.type.cob;
        }
    };
    if (oa::data::defs::update_unit_def(&files, &unit_table_.tables, type, sources, scripts))
        match_->replace_unit_def(type, unit_table_.tables.records[type]);
}

bool Runtime::console_burn_feature(int32_t cell_x, int32_t cell_z, bool force) {
    if (!match_)
        return false;
    auto& world = match_->state();
    const auto* plot = oa::world_plot(&world, cell_x, cell_z);
    if (plot == nullptr)
        return false;
    return oa::sim::feature_runtime::clear_plot_feature(
        world, match_->feature_host(), static_cast<std::size_t>(plot - world.plots), force
    );
}

void Runtime::console_burn_all_features() {
    if (!match_)
        return;
    oa::sim::feature_runtime::clear_all_features(match_->state(), match_->feature_host());
}

bool Runtime::console_place_feature(const char* name, int32_t cell_x, int32_t cell_z) {
    if (!match_ || name == nullptr)
        return false;
    const uint16_t index = oa::sim::map_runtime::find_feature_index(feature_table_, name);
    auto& world = match_->state();
    const auto* plot = oa::world_plot(&world, cell_x, cell_z);
    if (index == oa::sim::map_runtime::no_feature_index || plot == nullptr)
        return false;
    const auto at = static_cast<std::size_t>(plot - world.plots);
    // Whether the feature was placed is read back from the plot below.
    std::ignore = oa::sim::feature_runtime::place_feature(
        world, match_->feature_host(), at, index, nullptr, nullptr, kConsolePlacer
    );
    if (world.plots[at].feature != index)
        return false;
    place_catalog_feature_draw(cell_x, cell_z, index);
    return true;
}

void Runtime::store_cursor_cell(const oa::sim::ground_orders::Point& ground) {
    oa::Game& game = match_->state().game;
    const auto cell_x = static_cast<int16_t>(static_cast<uint32_t>(ground[0]) >> kCellShift);
    const auto cell_z = static_cast<int16_t>(static_cast<uint32_t>(ground[2]) >> kCellShift);
    game.cursor_cell_x = cell_x;
    game.cursor_cell_z = cell_z;
    uint16_t word = oa::sim::spatial_state::no_feature;
    const auto& plots = match_->spatial().plots;
    const auto width = static_cast<std::size_t>(selected_tnt_ ? selected_tnt_->attribute_width : 0);
    const auto index = static_cast<std::size_t>(cell_z) * width + static_cast<std::size_t>(cell_x);
    if (cell_x >= 0 && cell_z >= 0 && static_cast<std::size_t>(cell_x) < width &&
        index < plots.size()) {
        word = plots[index].feature_word;
        if (word == oa::sim::spatial_state::feature_continuation)
            word = plots[feature_origin(plots, index, width)].feature_word;
        else if (word >= oa::sim::spatial_state::first_reserved_feature)
            word = oa::sim::spatial_state::no_feature;
    }
    game.cursor_feature = word;
}

char* Runtime::console_read_text_file(const char* path, int32_t* length) {
    const auto bytes = assets_.load_file_contents(resource_path(path));
    if (!bytes)
        return nullptr;
    auto* copy = static_cast<char*>(std::malloc(bytes->size() + 1));
    if (copy == nullptr)
        return nullptr;
    std::memcpy(copy, bytes->data(), bytes->size());
    copy[bytes->size()] = '\0';
    if (length != nullptr)
        *length = static_cast<int32_t>(bytes->size());
    return copy;
}

void Runtime::open_chat_line() {
    if (local_player_watches())
        return;
    chat_composing_ = true;
    chat_buffer_.clear();
    chat_composition_.clear();
    status_ = "Message";
    // The chat line's place: the band at the foot of the overlays' area
    // where a line rises over the battlefield, one line of HUD text with its
    // rows above and below, down to the TALK field's foot in the bottom bar
    // where the line is typed while it fits there.
    namespace layout = oa::ui::display_layout;
    const auto area = overlay_area();
    const int scale = std::max(hud_text_scale(), 1);
    const oa::formats::fnt::Font* font = match_label_font();
    const int text_rows = font != nullptr ? static_cast<int>(oa::formats::fnt::line_height(*font))
                                          : chat_field_fallback_rows;
    const int band = (text_rows + 2 * chat_field_margin_rows) * scale;
    const int area_bottom = area.y + area.height;
    int bottom = area_bottom;
    if (!touch_controls_active())
        if (const auto box = chat_text_box()) {
            const auto foot = layout::source_to_canvas(match_layout_, box->x, box->y + box->height);
            bottom = std::max(bottom, foot.y);
        }
    const int top = std::max(area_bottom - band, area.y);
    start_text_input(layout::Rect{area.x, top, area.width, bottom - top});
}

void Runtime::close_chat_line() {
    chat_composing_ = false;
    chat_buffer_.clear();
    chat_composition_.clear();
    stop_text_input();
}

void Runtime::submit_chat_line() {
    // The typed line goes out as game text, cut where the chat line's
    // buffer ends without splitting a character.
    std::string line = typed_game_text(chat_buffer_);
    line.resize(oa::base::text::whole_characters(line, oa::ui::hud::typed_text_bytes - 1));
    close_chat_line();
    if (!match_)
        return;
    oa::ui::hud::ChatHost host{};
    host.user = this;
    host.submit_command = [](void* user, const char* text, uint8_t mode) -> uint8_t {
        auto* runtime = runtime_of(user);
        console::Console* con = runtime->match_console();
        if (con == nullptr)
            return mode;
        mode = console::console_submit_chat_line(con, text, mode, nullptr);
        sync_effects_toggle(runtime->match_.get(), *con);
        runtime->keep_console_carry();
        return mode;
    };
    host.post_chat = [](void* user,
                        const oa::Player& speaker,
                        const char* text,
                        uint8_t kind,
                        const char* target) {
        auto* runtime = runtime_of(user);
        if (!runtime->match_ || runtime->refuse_take_line(text))
            return;
        const auto hooks = runtime->message_hooks();
        oa::sim::messages::post_chat(runtime->match_->state(), speaker, text, kind, target, hooks);
        runtime->status_ = text;
        runtime->read_speed_lock_line(text);
    };
    oa::ui::hud::send_chat_line(match_->state(), line.c_str(), host);
}

bool Runtime::refuse_take_line(const char* text) {
    if (!match_ || !match_->rules().sharing.take_requires_live_commander.enabled ||
        !oa::ui::hud::is_take_command(text))
        return false;
    auto& world = match_->state();
    const auto* commanders =
        oa::data::defs::category_registry_find(&unit_table_.tables.categories, "Commander");
    const uint8_t fallen = oa::ui::hud::commander_destroyed_elsewhere(
        world, world.game.local_player_index, commanders != nullptr ? commanders->words : nullptr
    );
    if (fallen >= OA_PLAYER_COUNT)
        return false;
    char notice[96];
    oa::ui::hud::format_take_refusal(notice, sizeof notice, world.game.players[fallen]);
    oa::sim::messages::post_message(
        world,
        notice,
        oa::sim::messages::kind_unit_report,
        0,
        oa::sim::messages::sender_none,
        message_hooks()
    );
    status_ = notice;
    return true;
}

void Runtime::list_save_files(
    const char* pattern, void (*visit)(void* user, const char* name), void* user
) const {
    const auto path = game_file_path(pattern, ui::frontend::SavePathUse::write);
    const auto wildcard = path_to_utf8(path.filename());
    const auto star = wildcard.find('*');
    const auto prefix = wildcard.substr(0, star);
    const auto suffix = star == std::string::npos ? std::string() : wildcard.substr(star + 1);
    const auto same = [](char a, char b) {
        return std::toupper(static_cast<unsigned char>(a)) ==
               std::toupper(static_cast<unsigned char>(b));
    };
    // The saved games' folder, then the folders that held them before, whose
    // names are taken too.
    std::vector<fs::path> folders{path.parent_path()};
    if (path.parent_path() == saves_folder()) {
        const auto earlier = save_roots().earlier;
        folders.insert(folders.end(), earlier.begin(), earlier.end());
    }
    for (const auto& folder : folders) {
        std::error_code error;
        for (const auto& entry : fs::directory_iterator(folder, error)) {
            const auto name = path_to_utf8(entry.path().filename());
            if (name.size() >= prefix.size() + suffix.size() &&
                std::equal(prefix.begin(), prefix.end(), name.begin(), same) &&
                std::equal(suffix.rbegin(), suffix.rend(), name.rbegin(), same))
                visit(user, name.c_str());
        }
    }
}

bool Runtime::handle_console_hotkey(const SDL_KeyboardEvent& key) {
    console::Console* con = match_console();
    if (con == nullptr)
        return false;
    const oa::Game& game = con->world->game;
    const bool developer = (console::console_flags(game) & console::console_flag::developer) != 0;
    const bool debug_keys = (game.outcome_flags & console::outcome_flag::debug_keys) != 0;
    // A multiplayer game's team menu (Tab) and share panel ('h'); a mod's
    // rules may open the team menu in every game type.
    const bool multiplayer = multiplayer_session();
    // A mod's key remaps leave '\' to other uses, re-run the last line on
    // Insert and toggle the debug keys on F10 as well.
    const bool remaps = con->key_remaps;
    const bool f10 = key.key == SDLK_F10 || key.scancode == SDL_SCANCODE_F10;
    uint32_t code = 0;
    if (key.key == SDLK_F4 || key.scancode == SDL_SCANCODE_F4)
        code = console::hotkey::f1 + 3;
    else if (key.key == SDLK_PAUSE || key.scancode == SDL_SCANCODE_PAUSE)
        code = console::hotkey::pause;
    else if ((multiplayer || con->team_menu_every_game) && key.key == SDLK_TAB)
        code = console::hotkey::tab;
    else if (multiplayer && key.key == SDLK_H)
        code = 'h';
    // The key below Escape is '`' on every layout, so that a layout with no
    // '`' key, such as the Italian one, where it types '\', has the key too.
    else if (key.key == SDLK_GRAVE || key.scancode == SDL_SCANCODE_GRAVE)
        code = '`';
    else if (developer && !remaps && key.key == SDLK_BACKSLASH)
        code = console::hotkey::repeat_command;
    else if (developer && remaps && key.key == SDLK_INSERT)
        code = console::hotkey::insert;
    else if (developer && key.key == SDLK_F11)
        code = console::hotkey::f11;
    else if ((input_modifiers(ModifierUse::keyboard) & SDL_KMOD_CTRL) != 0 && f10)
        code = console::hotkey::control_f10;
    else if (developer && remaps && f10)
        code = console::hotkey::f10;
    else if (debug_keys && key.key == SDLK_EQUALS)
        code = '=';
    else if (debug_keys && key.key == SDLK_RIGHTBRACKET)
        code = ']';
    else if (debug_keys && key.key == SDLK_I)
        code = 'i';
    else if (debug_keys && key.key == SDLK_M)
        code = 'm';
    if (code == 0)
        return false;
    // Only the keys above reach the dispatcher; the rest are the runtime's own
    // hotkeys. Callbacks the runtime does not provide stay unbound.
    console::HotkeyHost host{};
    host.context = this;
    host.shift_down = [](void* context) {
        return runtime_of(context)->control_key_down(oa::ui::gui_input::ControlKey::shift);
    };
    host.alt_down = [](void* context) {
        return runtime_of(context)->control_key_down(oa::ui::gui_input::ControlKey::alt);
    };
    host.play_sound = [](void* context, const char* name) {
        runtime_of(context)->play_match_interface_sound(name);
    };
    host.open_chat = [](void* context) { runtime_of(context)->open_chat_line(); };
    host.change_game_speed = [](void* context, int32_t direction) {
        runtime_of(context)->adjust_game_speed(direction);
    };
    host.clear_messages = [](void* context) {
        oa::sim::messages::clear_messages(runtime_of(context)->match_->state().game);
    };
    host.open_options_panel = [](void* context) { runtime_of(context)->show_match_pause_menu(); };
    host.session_kind = [](void* context) -> oa::data::campaign::SessionKind {
        return runtime_of(context)->multiplayer_session()
                   ? oa::data::campaign::SessionKind::multiplayer
                   : oa::data::campaign::SessionKind::none;
    };
    host.open_team_menu = [](void* context) { runtime_of(context)->toggle_team_menu(); };
    host.open_share_panel = [](void* context) { runtime_of(context)->open_team_share_panel(); };
    host.send_pause = [](void* context, bool paused) {
        auto& runtime = *runtime_of(context);
        runtime.status_ = paused ? "Game paused" : "Resumed";
        call_hook_or_report<&Extension::pause_changed>(
            runtime.extension_, runtime.hook_error_report(), runtime, paused
        );
    };
    host.list_files = [](void* context,
                         const char* pattern,
                         void (*visit)(void* user, const char* name),
                         void* user) {
        runtime_of(context)->list_save_files(pattern, visit, user);
    };
    host.begin_movie_capture = [](void* context, const char* path) {
        runtime_of(context)->begin_film_capture(path);
    };
    console::hotkey_dispatch(con, &host, code);
    sync_effects_toggle(match_.get(), *con);
    keep_console_carry();
    if (code == console::hotkey::f11)
        status_ = (game.outcome_flags & console::outcome_flag::debug_keys) != 0 ? "Debug keys on"
                                                                                : "Debug keys off";
    return true;
}

const oa::formats::fnt::Font* Runtime::console_clock_font() {
    if (!match_)
        return nullptr;
    if (oa::ui::hud::clock_font(match_->state().game) == oa::ui::hud::ClockFont::message_log)
        return &message_font();
    return match_label_font();
}

std::optional<int> Runtime::console_clock_pen_row() {
    namespace hud = oa::ui::hud;
    namespace layout = oa::ui::display_layout;
    if (!match_)
        return std::nullopt;
    const oa::Game& game = match_->state().game;
    if ((console::console_flags(game) & console::console_flag::clock) == 0)
        return std::nullopt;
    const oa::formats::fnt::Font* font = console_clock_font();
    if (font == nullptr)
        return std::nullopt;
    // The pen's rows above the bottom bar; the glyphs start the font's lift
    // above the pen. Text larger than the font rises by what it adds.
    const auto height = static_cast<uint8_t>(std::min(
        oa::present::sized_length(
            static_cast<uint8_t>(font->nominal_height), oa::present::game_text_size()
        ),
        int32_t{UINT8_MAX}
    ));
    const int pen_rise =
        layout::kSourceBottomBarY - hud::clock_pen_row(layout::kSourceHeight, height);
    // Above the bottom of the overlays' area: the bottom bar's top, or with
    // the touch controls on, the top of what they lay along the bottom.
    const auto area = overlay_area();
    return area.y + area.height - pen_rise * hud_text_scale();
}

void Runtime::draw_console_clock() {
    namespace hud = oa::ui::hud;
    namespace layout = oa::ui::display_layout;
    const auto pen_row = console_clock_pen_row();
    if (!pen_row || console_clock_opacity_ == 0)
        return;
    const oa::Game& game = match_->state().game;
    const oa::formats::fnt::Font* font = console_clock_font();

    struct Label {
        Runtime* runtime;
        std::string text;
    } label{this, {}};

    char text[64];
    hud::format_game_time(
        game,
        [](void* context, const char* line) -> const char* {
            auto& translated = *static_cast<Label*>(context);
            translated.text = translated.runtime->translate_ui(line);
            return translated.text.c_str();
        },
        &label,
        text,
        sizeof text
    );
    ensure_ui_colors();
    const auto scale = hud_text_scale();
    const auto at =
        canvas_paint(overlay_area().x + (hud::kClockLeft - layout::kSourceLeft) * scale, *pen_row);
    const auto paint = [&] {
        draw_match_text(font, at.x, at.y, text, ui_colors_[hud::kClockColorSlot], scale);
    };
    if (console_clock_opacity_ >= hud::kClockOpaque) {
        paint();
        return;
    }
    // Fading in: the clock and all it paints around its letters, which may
    // lie a line above and below the pen and a few columns either side.
    const int line =
        std::max(
            static_cast<int>(oa::formats::fnt::line_height(*font)),
            static_cast<int>(oa::present::sized_length(
                static_cast<uint8_t>(font->nominal_height), oa::present::game_text_size()
            ))
        ) *
        scale;
    const int margin = 8 * scale;
    paint_faded(
        at.x - margin,
        at.y - 2 * line,
        match_text_width(*font, text, scale) + 2 * margin,
        5 * line,
        console_clock_opacity_,
        paint
    );
}

void Runtime::check_console_commands() {
    if (screen_ != Screen::match || !match_)
        throw std::runtime_error("console check needs a running match");
    bool running = true;
    const auto key = [&](SDL_Keycode code, SDL_Scancode scancode) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = code;
        event.key.scancode = scancode;
        handle_sdl_event(event, running);
    };
    const auto type = [&](const char* text) {
        SDL_Event event{};
        event.type = SDL_EVENT_TEXT_INPUT;
        event.text.text = text;
        handle_sdl_event(event, running);
    };
    const auto enter_line = [&](const char* text) {
        key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        if (!chat_composing_)
            throw std::runtime_error("console check: Enter did not open the chat line");
        type(text);
        key(SDLK_RETURN, SDL_SCANCODE_RETURN);
        if (chat_composing_)
            throw std::runtime_error("console check: Enter did not submit the chat line");
    };
    const oa::Game& game = match_->state().game;
    const auto clock_before = console::console_flags(game) & console::console_flag::clock;
    enter_line("+clock");
    if ((console::console_flags(game) & console::console_flag::clock) == clock_before)
        throw std::runtime_error("console check: +clock did not toggle the clock option");
    const auto& speaker = game.players[game.local_player_index].second_name;
    const std::string echo =
        "<" + std::string(speaker, strnlen(speaker, sizeof speaker)) + "> +clock";
    if (const auto lines = match_message_lines(); lines.empty() || lines.back() != echo)
        throw std::runtime_error("console check: +clock was not echoed as " + echo);
    render_match_surface();
    const auto player = match_->state().game.viewpoint_player;
    const float metal_before = match_->state().game.players[player].metal;
    enter_line("+atm");
    const float metal_after = match_->state().game.players[player].metal;
    // The console adds its ATM amount: 1000, or a mod profile's.
    const auto* con = match_console();
    if (con == nullptr || metal_after != metal_before + con->atm_amount)
        throw std::runtime_error("console check: +atm did not add the ATM amount of metal");
    oa::World& world = match_->state();
    const uint8_t giver = game.local_player_index;
    uint8_t other = OA_PLAYER_COUNT;
    for (uint8_t i = OA_PLAYER_COUNT; i-- > 0;)
        if (i != giver && game.players[i].status == OA_PLAYER_STATUS_COMPUTER)
            other = i;
    if (other == OA_PLAYER_COUNT)
        throw std::runtime_error("console check needs a computer player");
    const float giver_before = world.game.players[giver].metal;
    const auto* credit = oa::world_player_economy(&world, &world.game.players[other]);
    const float credit_before = credit != nullptr ? credit->metal.produced : 0.0F;
    char line[32];
    std::snprintf(line, sizeof line, "+give %u 100 metal", static_cast<unsigned>(other));
    enter_line(line);
    if (world.game.players[giver].metal != giver_before - 100.0F || credit == nullptr ||
        !(credit->metal.produced > credit_before))
        throw std::runtime_error("console check: +give did not move 100 metal to the other player");
    check_console_skirmish_cheats(enter_line);
    enter_line("+clock");
    enter_line("+Now Film Chris Include Reload Assert");
    key(SDLK_F11, SDL_SCANCODE_F11);
    if ((game.outcome_flags & console::outcome_flag::debug_keys) == 0)
        throw std::runtime_error(
            "console check: F11 after the passphrase did not enable debug keys"
        );
    check_debug_status_line();
    key(SDLK_F11, SDL_SCANCODE_F11);
    // "+reload <unit>" kills that type with outcome 8 before reloading it:
    // a solar collector placed for the computer player must be gone after
    // the next update.
    const auto solar = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMSOLAR");
    const auto solars = [&] {
        std::size_t count = 0;
        for (const auto& slot : match_->world().slots)
            count += slot.unit != nullptr && slot.record.type_index == solar &&
                             (slot.record.flags & OA_UNIT_FLAG_LIVE) != 0
                         ? 1
                         : 0;
        return count;
    };
    const oa::Unit* anchor = nullptr;
    for (const auto& slot : match_->world().slots)
        if (anchor == nullptr && slot.unit != nullptr && slot.record.type_index != 0 &&
            slot.record.owner_index == other)
            anchor = &slot.record;
    if (anchor == nullptr || solar == 0 || console_->host.create_unit == nullptr)
        throw std::runtime_error("console check cannot place a unit for the computer player");
    constexpr int32_t kPlacementOffset = 128 << 16;
    const FixedVec3 position{
        anchor->position.x - kPlacementOffset, anchor->position.y, anchor->position.z
    };
    console_->host.create_unit(this, other, solar, &position);
    if (solars() == 0)
        throw std::runtime_error("console check: the spawn callback created no unit");
    enter_line("+reload ARMSOLAR");
    ++match_timing_.tick;
    match_->simulation().tick = match_timing_.tick;
    tick_or_raise(*match_);
    if (solars() != 0)
        throw std::runtime_error("console check: +reload left the placed unit alive");
    check_console_option_commands(enter_line);
    check_console_cursor_commands(enter_line);
    check_console_debug_commands(enter_line);
    check_console_sound_commands(enter_line);
    check_console_display_commands(enter_line);
    check_console_stats(enter_line);
    if (extension_.check_console != nullptr) {
        std::function<void(const char*)> line_entry = enter_line;
        call_hook_or_raise<&Extension::check_console>(
            extension_,
            *this,
            [](void* user, const char* text) {
                (*static_cast<std::function<void(const char*)>*>(user))(text);
            },
            &line_entry
        );
    }
    check_console_unit_commands(enter_line);
    check_console_poster_command(enter_line);
    check_console_range_overlays(enter_line);
    check_game_settings_sheet();
    std::cout << "console check: +clock toggled the clock, +atm added 1000 metal, +give moved "
                 "metal, +reload removed a unit, a spawn by name and +kill (which turned victory "
                 "and defeat off), +feature and +burnone at the cursor, passphrase and F11 debug "
                 "keys with their FRATE line, +contour lines, +profile bars, +stats and "
                 "DebugBreak's gate work\n";
}

void Runtime::enter_console_check_line(const char* text) {
    bool running = true;
    const auto key = [&](SDL_Keycode code, SDL_Scancode scancode) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = code;
        event.key.scancode = scancode;
        handle_sdl_event(event, running);
    };
    key(SDLK_RETURN, SDL_SCANCODE_RETURN);
    if (!chat_composing_)
        throw std::runtime_error("console check: Enter did not open the chat line");
    SDL_Event event{};
    event.type = SDL_EVENT_TEXT_INPUT;
    event.text.text = text;
    handle_sdl_event(event, running);
    key(SDLK_RETURN, SDL_SCANCODE_RETURN);
    if (chat_composing_)
        throw std::runtime_error("console check: Enter did not submit the chat line");
}

void Runtime::check_console_cursor_commands(const std::function<void(const char*)>& enter_line) {
    namespace input = oa::sim::gameplay_input;
    oa::World& world = match_->state();
    uint8_t spare = OA_PLAYER_COUNT;
    for (uint8_t i = OA_PLAYER_COUNT; i-- > 0;)
        if (world.game.players[i].status == OA_PLAYER_STATUS_FREE)
            spare = i;
    const oa::Unit* anchor = nullptr;
    for (const auto& slot : match_->world().slots)
        if (anchor == nullptr && slot.unit != nullptr && slot.record.type_index != 0 &&
            slot.record.owner_index == match_local_player_)
            anchor = &slot.record;
    if (spare == OA_PLAYER_COUNT || anchor == nullptr || !selected_tnt_ ||
        selected_tnt_->features.empty())
        throw std::runtime_error("console check needs a free slot, a commander and map features");
    const auto* feature = find_catalog_feature(selected_tnt_->features.front().name);
    if (feature == nullptr)
        throw std::runtime_error("console check: the map's first feature has no definition");
    // A cell left of the commander whose footprint holds no feature.
    constexpr int32_t kCellUnits = 1 << kCellShift;
    const auto& plots = match_->spatial().plots;
    const auto width = static_cast<int32_t>(selected_tnt_->attribute_width);
    const auto clear = [&](int32_t x, int32_t z) {
        for (int32_t row = 0; row < std::max<int16_t>(1, feature->terrain.footprint_z); ++row)
            for (int32_t column = 0; column < std::max<int16_t>(1, feature->terrain.footprint_x);
                 ++column) {
                const auto index = static_cast<std::size_t>(z + row) * width + (x + column);
                if (x < 0 || x + column >= width || index >= plots.size() ||
                    plots[index].feature_word != oa::sim::spatial_state::no_feature)
                    return false;
            }
        return true;
    };
    oa::sim::ground_orders::Point ground{};
    int32_t cell_x = -1, cell_z = -1;
    for (int32_t step = 4; step < 64 && cell_x < 0; ++step) {
        ground = {anchor->position.x - step * kCellUnits, anchor->position.y, anchor->position.z};
        const int32_t x = static_cast<int32_t>(static_cast<uint32_t>(ground[0]) >> kCellShift);
        const int32_t z = static_cast<int32_t>(static_cast<uint32_t>(ground[2]) >> kCellShift);
        if (clear(x, z)) {
            cell_x = x;
            cell_z = z;
        }
    }
    if (cell_x < 0)
        throw std::runtime_error("console check found no clear cell beside the commander");
    input::set_pointer_position(world.game, {ground[0], ground[1], ground[2]});
    store_cursor_cell(ground);
    const auto spare_units = [&] {
        std::size_t count = 0;
        for (const auto& slot : match_->world().slots)
            count += slot.unit != nullptr && slot.record.type_index != 0 &&
                             slot.record.owner_index == spare &&
                             (slot.record.flags & OA_UNIT_FLAG_LIVE) != 0
                         ? 1
                         : 0;
        return count;
    };
    char line[64];
    std::snprintf(line, sizeof line, "+ARMSOLAR %u", static_cast<unsigned>(spare));
    enter_line(line);
    if (spare_units() == 0)
        throw std::runtime_error("console check: the spawn by name created no unit");
    // Units of players not simulated here wait on the other branch of the
    // match's commander-death queue.
    oa::Player& stand_in = world.game.players[spare];
    const auto free_in_use = stand_in.in_use;
    stand_in.in_use = 1;
    stand_in.status = OA_PLAYER_STATUS_COMPUTER;
    std::snprintf(line, sizeof line, "+kill %u", static_cast<unsigned>(spare));
    enter_line(line);
    ++match_timing_.tick;
    match_->simulation().tick = match_timing_.tick;
    tick_or_raise(*match_);
    const auto survivors = spare_units();
    stand_in.in_use = free_in_use;
    stand_in.status = OA_PLAYER_STATUS_FREE;
    if (survivors != 0)
        throw std::runtime_error("console check: +kill left a simulated player's unit alive");
    // Kill also leaves the game with no victory or defeat; nothing turns
    // them back on before the next game.
    if (match_->scenario_controller().enabled != 0)
        throw std::runtime_error("console check: +kill left the victory and defeat tests on");
    const auto origin = static_cast<std::size_t>(cell_z) * width + cell_x;
    std::snprintf(line, sizeof line, "+feature %s", selected_tnt_->features.front().name.c_str());
    enter_line(line);
    if (plots[origin].feature_word != 0 ||
        (plots[origin].flags & oa::data::persist::plot_flags_player_features) == 0)
        throw std::runtime_error("console check: +feature did not place at the cursor cell");
    store_cursor_cell(ground);
    enter_line("+burnone");
    if (plots[origin].feature_word != oa::sim::spatial_state::no_feature)
        throw std::runtime_error("console check: +burnone left the feature at the cursor");
}

} // namespace oa::app
