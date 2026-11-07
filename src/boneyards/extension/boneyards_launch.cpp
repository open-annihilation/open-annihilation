// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Boneyards' part of a network game (boneyards_launch.hpp).
#include "boneyards_launch.hpp"

#include "boneyards_data.hpp"

#include "oa/boneyards/overlay_launch.hpp"
#include "oa/boneyards/rover_calls.hpp"
#include "oa/data/replays/replays.hpp"
#include "oa/netgame/frontend/multiplayer_states.hpp"

#include <cstddef>
#include <cstdio>
#include <cstring>
#include <string>

namespace oa::app {

// The Boneyards console's own entries (console_screens.cpp).
void boneyards_console_show_notice(const char* text);
void boneyards_console_stop_sounds();

namespace {

namespace api = oa::app::netgame::extension_api;
namespace nl = oa::ui::frontend_multiplayer::launch;
namespace reporter = oa::boneyards::reporter;
namespace frontend = oa::netgame::frontend;

BoneyardsLaunch g_launch{};

// Set when a launch is applied, and left set if a later reply is refused,
// so the match already started keeps its report.
bool g_launch_locked_allowed = false;

/// Copies text into a field, keeping at most `limit` characters and terminating it.
///
/// @param[out] field field receiving the text
/// @param capacity size of the field in bytes; more than `limit`
/// @param limit most characters kept
/// @param text text to copy; null stores an empty text
void copy_bounded(char* field, std::size_t capacity, std::size_t limit, const char* text) noexcept {
    std::memset(field, 0, capacity);
    if (text == nullptr)
        return;
    std::snprintf(field, limit + 1, "%s", text);
    field[limit] = '\0';
}

/// Reads the planet of the launched battle and its game id.
///
/// The planet is the one the launch reply named.
///
/// @param[out] area_id the launch reply's planet
/// @param[out] game_id the launch reply's game id
/// @return false without the Boneyards
bool battle_ids(uint32_t* area_id, const char** game_id) noexcept {
    if (g_launch.battle_ids != nullptr)
        return g_launch.battle_ids(area_id, game_id);
    return oa::boneyards::console::boneyards_status_widget_battle_ids(
        boneyards_console_status_widget(), area_id, game_id
    );
}

/// Reads the planet's name and the sector's name of the launched battle.
///
/// @param[out] planet the planet's name
/// @param[out] sector the sector's name
/// @return false without the Boneyards
bool battle_place(const char** planet, const char** sector) noexcept {
    if (g_launch.battle_place != nullptr)
        return g_launch.battle_place(planet, sector);
    return oa::boneyards::console::boneyards_status_widget_battle_place(
        boneyards_console_status_widget(), planet, sector
    );
}

/// Adds a member to a recorded game's object unless its text is empty.
///
/// @param[in,out] game the recorded game
/// @param name the member's name
/// @param text the member's text; null or empty adds nothing
void add_recorded_member(api::RecordedGame& game, const char* name, const char* text) noexcept {
    if (text == nullptr || text[0] == '\0' || game.member_count >= api::recorded_game_member_limit)
        return;
    api::RecordedGameMember& member = game.members[game.member_count++];
    copy_bounded(member.name, sizeof member.name, sizeof member.name - 1, name);
    copy_bounded(member.text, sizeof member.text, sizeof member.text - 1, text);
}

/// Leaves the game and ends the program with the disconnect reason, as YES in "Exit to the system?" does.
///
/// The battle tunnel is dropped first: quitting is a way out of the battle.
///
/// @param context unused
void leave_game_from_boneyards(void*) noexcept {
    boneyards_remove_overlay_route();
    api::leave_game(true);
}

/// Posts a line the Boneyards sends to the running match, or into the frontend Game's chat ring.
///
/// @param context unused
/// @param text the line
void post_battle_message(void*, const char* text) noexcept {
    api::post_battle_line(text);
}

/// Empties the frontend Game's chat ring.
///
/// @param context unused
void empty_battle_chat(void*) noexcept {
    api::empty_chat_ring();
}

/// Ends network play's launch as the Boneyards' launched battle ends.
///
/// @param context unused
void end_battle_launch(void*) noexcept {
    boneyards_remove_overlay_route();
    api::end_launch();
}

/// Answers context_flags: 1 while a launch is active.
///
/// @param ctx unused
/// @param state unused
/// @return 1 or 0
uint32_t query_context_flags(ScreenContext*, void*) noexcept {
    return api::launch_active() ? 1U : 0U;
}

/// Answers adapter_lookup_needed: 1 while the network the Boneyards needs is there.
///
/// @param ctx unused
/// @param state unused
/// @return 1 or 0
uint32_t query_adapter_lookup_needed(ScreenContext*, void*) noexcept {
    return oa::boneyards::console::boneyards_status_widget_network_available(
               boneyards_console_status_widget()
           )
               ? 1U
               : 0U;
}

/// Answers generate_default_game_name: 0, as no lobby program launches the native game.
///
/// @param ctx unused
/// @param state unused
/// @return 0
uint32_t query_default_game_name(ScreenContext*, void*) noexcept {
    return 0;
}

/// Uploads the game's settings (Rover slot 26).
///
/// @param context unused
/// @param upload the settings
void upload_settings(void*, const reporter::SettingsUpload& upload) noexcept {
    uint32_t area_id = 0;
    const char* game_id = nullptr;
    if (!battle_ids(&area_id, &game_id))
        return;
    (void)oa::boneyards::rover_call_send_game_settings(
        area_id,
        game_id,
        upload.lock_options,
        upload.mission,
        upload.player_limit,
        upload.death_option,
        upload.energy,
        upload.metal,
        upload.location,
        upload.los,
        upload.map_mode,
        upload.max_units,
        upload.watching,
        upload.cheats,
        upload.tournament
    );
}

/// Uploads one player's entry (Rover slot 28).
///
/// @param context unused
/// @param upload the player
void upload_player(void*, const reporter::PlayerUpload& upload) noexcept {
    uint32_t area_id = 0;
    const char* game_id = nullptr;
    if (!battle_ids(&area_id, &game_id))
        return;
    (void)oa::boneyards::rover_call_send_player_info(
        area_id, game_id, upload.net_id, upload.name, upload.side, upload.team
    );
}

/// Uploads one player's score (Rover slot 30).
///
/// @param context unused
/// @param upload the score
void upload_score(void*, const reporter::ScoreUpload& upload) noexcept {
    uint32_t area_id = 0;
    const char* game_id = nullptr;
    if (!battle_ids(&area_id, &game_id))
        return;
    namespace line = reporter::score_line;
    (void)oa::boneyards::rover_call_send_score_report(
        area_id,
        game_id,
        upload.net_id,
        upload.lines[line::kills],
        upload.lines[line::losses],
        upload.lines[line::energy_produced],
        upload.lines[line::metal_produced],
        upload.lines[line::excess_energy],
        upload.lines[line::excess_metal],
        upload.lines[line::commanders_killed],
        upload.lines[line::commanders_lost],
        upload.score,
        upload.winner,
        upload.final_report
    );
}

/// Uploads a chat line with the score reports (Rover slot 31).
///
/// @param context unused
/// @param net_id the local player's session id
/// @param line the chat line
void upload_chat(void*, uint32_t net_id, const char* line) noexcept {
    uint32_t area_id = 0;
    const char* game_id = nullptr;
    if (battle_ids(&area_id, &game_id))
        (void)oa::boneyards::rover_call_send_score_report_message(area_id, game_id, net_id, line);
}

/// Returns the launch block's connection type for the Boneyards console.
///
/// @param user unused
/// @return the type
int32_t bridge_connection_type(void*) noexcept {
    return api::connection_type();
}

/// Tells the console whether this simulation may be launched.
///
/// @param user unused
/// @param[out] text the refusal, when the launch is refused and `text` is non-null
/// @return false when the simulation is not one the Boneyards launches
bool bridge_launch_permitted(void*, const char** text) noexcept {
    const DataVerdict verdict = boneyards_current_data_verdict();
    if (verdict.allowed)
        return true;
    if (text != nullptr)
        *text = verdict.message != nullptr ? verdict.message : kBoneyardsDataRefusal;
    return false;
}

} // namespace

int32_t boneyards_faction_of_allegiance(const char* allegiance) noexcept {
    if (allegiance == nullptr)
        return nl::faction::none;
    if (allegiance[0] == 'A')
        return nl::faction::arm;
    if (allegiance[0] == 'C')
        return nl::faction::core;
    return nl::faction::none;
}

void online_launch_fill_boneyards(
    nl::LaunchBlock& block, const oa::boneyards::LaunchReport& launch, const char* user_name
) noexcept {
    block.time_limit = 0;
    block.hosting = launch.creator ? 1U : 0U;
    block.player_limit = launch.playerlimit;
    block.connection_type = nl::connection_type::tcpip;
    block.launched_connection_type = nl::connection_type::tcpip;
    block.lock_options = launch.lock_options ? 1U : 0U;
    // The picker stays locked and the room starts with no mutators, whether
    // or not the server locked the room's other options.
    block.bar_mutators = 1;
    block.max_units = launch.maxunits;
    block.energy = launch.energy;
    block.metal = launch.metal;
    block.death_option = launch.deathopt;
    block.los = launch.los;
    block.cheats = launch.cheats;
    block.location = launch.location;
    block.mapping = launch.mapping;
    block.watching = launch.watching;
    block.tournament = launch.tournament;
    block.team = launch.team;
    block.pfs = 0;
    block.faction = boneyards_faction_of_allegiance(launch.allegiance);
    copy_bounded(
        block.provider_label,
        sizeof block.provider_label,
        sizeof block.provider_label - 1,
        kBoneyardsProviderLabel
    );
    copy_bounded(
        block.session_name, sizeof block.session_name, sizeof block.session_name - 1, launch.session
    );
    copy_bounded(block.mission, sizeof block.mission, sizeof block.mission - 1, launch.mission);
    copy_bounded(block.password, sizeof block.password, sizeof block.password - 1, launch.password);
    copy_bounded(block.user_name, sizeof block.user_name, sizeof block.user_name - 1, user_name);
    copy_bounded(block.address, sizeof block.address, sizeof block.address - 1, launch.address);
}

void bind_boneyards_launch(const BoneyardsLaunch& launch) noexcept {
    g_launch = launch;
}

void on_boneyards_launch(
    void*, const oa::boneyards::LaunchReport* launch, const char* player_name
) noexcept {
    boneyards_remove_overlay_route();
    const DataVerdict verdict = boneyards_current_data_verdict();
    if (!verdict.allowed) {
        if (launch != nullptr)
            (void)oa::boneyards::rover_call_leave_battles(launch->area_id, nullptr);
        boneyards_console_show_notice(
            verdict.message != nullptr ? verdict.message : kBoneyardsDataRefusal
        );
        return;
    }
    if (const oa::boneyards::OverlayLaunch* overlay = oa::boneyards::current_overlay_launch())
        boneyards_install_overlay_route(*overlay);
    g_launch_locked_allowed = true;
    nl::LaunchBlock& block = api::launch_block();
    if (launch != nullptr)
        online_launch_fill_boneyards(block, *launch, player_name);
    // A launch with no reply still plays with no mutators.
    block.bar_mutators = 1;
    api::apply_launch();
    if (g_launch.exit_prompt != nullptr)
        exit_prompt_launch_applied(*g_launch.exit_prompt);
}

oa::boneyards::console::LaunchBridge boneyards_launch_bridge() noexcept {
    oa::boneyards::console::LaunchBridge bridge{};
    bridge.launched = on_boneyards_launch;
    bridge.launch_permitted = bridge_launch_permitted;
    bridge.launch_ended = end_battle_launch;
    bridge.connection_type = bridge_connection_type;
    bridge.empty_chat_ring = empty_battle_chat;
    bridge.battle_message = post_battle_message;
    bridge.leave_game = leave_game_from_boneyards;
    return bridge;
}

void boneyards_join_failed(void*, const char* text) noexcept {
    // The player is still on the game list, still joining. The leave goes out
    // once they are back in the Boneyards, the same leave the battle room's
    // back button sends on that return.
    boneyards_remove_overlay_route();
    boneyards_console_show_notice(text);
    boneyards_console_stop_sounds();
}

void boneyards_page(void*, const char* user, const char* text) noexcept {
    // Rover slot 41, a no-op without the online client module.
    oa::boneyards::rover_call_send_page_by_name(user, text);
}

void register_boneyards_queries(ScreenRegistry* registry) noexcept {
    (void)query_register(registry, frontend::query::context_flags, query_context_flags, nullptr);
    (void)query_register(
        registry, frontend::query::adapter_lookup_needed, query_adapter_lookup_needed, nullptr
    );
    (void)query_register(
        registry, frontend::query::generate_default_game_name, query_default_game_name, nullptr
    );
}

void boneyards_report_hooks(void*, reporter::Hooks& hooks) noexcept {
    hooks.upload_settings = upload_settings;
    hooks.upload_player = upload_player;
    hooks.upload_score = upload_score;
    hooks.upload_chat = upload_chat;
}

bool boneyards_report_simulation_allowed() noexcept {
    if (api::launch_active() && g_launch_locked_allowed)
        return true;
    return boneyards_current_data_verdict().allowed;
}

bool boneyards_launched_game() noexcept {
    return oa::boneyards::console::boneyards_status_widget_launched_game(
        boneyards_console_status_widget()
    );
}

bool boneyards_recorded_game(void*, api::RecordedGame* game) noexcept {
    if (game == nullptr || !api::launch_active())
        return false;
    uint32_t area_id = 0;
    const char* game_id = nullptr;
    if (!battle_ids(&area_id, &game_id) || game_id == nullptr || game_id[0] == '\0')
        return false;
    *game = api::RecordedGame{};
    copy_bounded(game->kind, sizeof game->kind, sizeof game->kind - 1, kBoneyardsReplayKind.data());
    add_recorded_member(*game, "game_id", game_id);
    const char* planet = nullptr;
    const char* sector = nullptr;
    if (battle_place(&planet, &sector)) {
        add_recorded_member(*game, "planet", planet);
        add_recorded_member(*game, "sector", sector);
    }
    return true;
}

void register_boneyards_replay_kind() {
    oa::data::replays::register_kind(
        {std::string(kBoneyardsReplayKind), std::string(kBoneyardsReplayTitle)}
    );
}

const char* boneyards_return_label() noexcept {
    if (!boneyards_launched_game())
        return nullptr;
    return api::launch_active() ? api::launch_block().provider_label : nullptr;
}

} // namespace oa::app
