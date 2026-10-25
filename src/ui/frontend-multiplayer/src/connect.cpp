// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Connection screens: SELPROV, TCP, SELGAME and NEWMULTI.
#include "oa/ui/frontend_multiplayer/connect.hpp"
#include "oa/formats/tdf.hpp"

#include "oa/data/languages/interface_text.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace oa::ui::frontend_multiplayer {

namespace {

constexpr std::size_t kNameEntryBytes = 0x10;   // GAMENAME/NICKNAME accept 16 characters
constexpr std::size_t kPasswordCopyBytes = 0xb; // copy bound: 10 characters and a terminator
constexpr uint32_t kMaxSessionPlayers = 10;
constexpr uint8_t kRejectLeaving = 2; // Player.reject_reason of a player who left by itself
constexpr uint8_t kSignalInitialize = 0;
constexpr uint8_t kSignalProviderChosen = 2;
constexpr uint8_t kSignalBack = 3;
constexpr uint8_t kSignalTcpBack = 0x10;
constexpr uint8_t kSignalHost = 0x11;
constexpr uint8_t kSignalJoin = 0x12;
constexpr uint8_t kSignalWatch = 0x13;
// Game.gui_flags: a lobby program launched the game.
constexpr uint8_t kGuiFlagLobbyLaunch = 0x10;
constexpr std::size_t kNicknameCopyBytes = 0x11;   // 16 characters and a terminator
constexpr std::size_t kLaunchNicknameLimit = 0x10; // characters of the launch's user name kept
constexpr uint32_t kMillisecondsPerSecond = 1000;
constexpr uint32_t kWaitSecondsLimit = 1000; // a count above this shows 0

void play(Lobby& lobby, const char* name) noexcept {
    if (lobby.services.play_sound != nullptr)
        lobby.services.play_sound(lobby.services.context, name);
}

void message(Lobby& lobby, const char* text) noexcept {
    if (lobby.services.message != nullptr)
        lobby.services.message(lobby.services.context, text);
}

std::string bounded(const char* text, std::size_t bound) {
    return {text, ::strnlen(text, bound)};
}

void copy_field(char* out, std::size_t capacity, std::string_view text) noexcept {
    const auto length = std::min(text.size(), capacity - 1);
    std::memcpy(out, text.data(), length);
    out[length] = '\0';
}

const PlayerSetupInfo*
session_info(const SessionEntry& session, PlayerSetupInfo& scratch) noexcept {
    std::memset(static_cast<void*>(&scratch), 0, sizeof(scratch));
    std::memcpy(
        reinterpret_cast<uint8_t*>(&scratch) + kSessionUserInfoOffset,
        session.user,
        sizeof(session.user)
    );
    return &scratch;
}

/// Returns the game's launch block, an empty one when the lobby has none.
///
/// @param lobby Lobby state and its launch link.
/// @return The block.
oa::ui::frontend_multiplayer::launch::LaunchBlock& launch_block(Lobby& lobby) noexcept {
    static oa::ui::frontend_multiplayer::launch::LaunchBlock empty{};
    if (lobby.launch_link.block != nullptr)
        return *lobby.launch_link.block;
    empty = oa::ui::frontend_multiplayer::launch::LaunchBlock{};
    return empty;
}

/// Reads the lobby services' millisecond clock.
///
/// @param lobby Lobby state and its services.
/// @return Milliseconds; the 30 Hz tick in milliseconds without the clock, 0 without either.
uint32_t milliseconds(Lobby& lobby) noexcept {
    if (lobby.services.milliseconds != nullptr)
        return lobby.services.milliseconds(lobby.services.context);
    if (lobby.services.tick != nullptr)
        return lobby.services.tick(lobby.services.context) * kMillisecondsPerSecond / 30U;
    return 0;
}

/// Plays BigButton unless a launch is active.
///
/// @param[in,out] lobby Lobby state, its services and launch link.
void play_big_button(Lobby& lobby) noexcept {
    if (!lobby_launch_active(lobby))
        play(lobby, "BigButton");
}

int32_t selected_session(const Panel& panel, const ConnectState& state) noexcept {
    const auto* list = panel_control(panel, "GAMENAME");
    if (list == nullptr || state.session_count == 0)
        return -1;
    return std::clamp<int32_t>(list->list_selection, 0, state.session_count - 1);
}

} // namespace

ProviderKind provider_kind(const uint8_t guid[16]) noexcept {
    if (std::memcmp(guid, kProviderGuidModem, 16) == 0)
        return ProviderKind::modem;
    if (std::memcmp(guid, kProviderGuidTcpip, 16) == 0)
        return ProviderKind::tcpip;
    if (std::memcmp(guid, kProviderGuidIpx, 16) == 0)
        return ProviderKind::ipx;
    return std::memcmp(guid, kProviderGuidSerial, 16) == 0 ? ProviderKind::serial
                                                           : ProviderKind::other;
}

const char* reject_reason_text(uint8_t reason) noexcept {
    switch (reason) {
    case 3:
        return "The game is closed";
    case 4:
        return "You did not have the correct password";
    case 5:
        return "The game is full";
    case 6:
        return "You have lost connection with the game";
    case 7:
        return "You need a unit you don't have for this game";
    case 8:
        return "You need a newer version of the game to enter";
    case 9:
        return "No watching is allowed for this game";
    case 10:
        return "The creator has left the game";
    default:
        return "You were rejected from the game";
    }
}

// ---------------------------------------------------------------------------
// SELPROV

void providers_open(Lobby& lobby, ConnectState& state, Panel& panel) noexcept {
    state.provider_count = 0;
    state.provider = -1;
    if (lobby.net.close != nullptr)
        lobby.net.close(lobby.net.context);
    if (lobby.net.providers != nullptr)
        state.provider_count = std::clamp(
            lobby.net.providers(
                lobby.net.context, state.providers, static_cast<int32_t>(kMaxProviders)
            ),
            0,
            static_cast<int32_t>(kMaxProviders)
        );
    std::vector<std::string> names;
    for (int32_t index = 0; index < state.provider_count; ++index)
        names.emplace_back(bounded(state.providers[index].name, kProviderNameBytes));
    panel_fill_list(panel, "DPLAY", std::move(names));
    // No provider rows come from the SERVICEx template, which stays hidden.
    panel_set_active(panel, "SERVICEx", false);
    panel.focus = panel_find(panel, "SELECT");
}

bool providers_take_launch(Lobby& lobby, ConnectState& state) noexcept {
    namespace nl = oa::ui::frontend_multiplayer::launch;
    auto& block = launch_block(lobby);
    ProviderKind wanted = ProviderKind::other;
    bool named = true;
    switch (block.connection_type) {
    case nl::connection_type::tcpip:
        wanted = ProviderKind::tcpip;
        break;
    case nl::connection_type::ipx:
        wanted = ProviderKind::ipx;
        break;
    case nl::connection_type::modem:
        wanted = ProviderKind::modem;
        break;
    case nl::connection_type::serial:
        wanted = ProviderKind::serial;
        break;
    default:
        named = false;
        break;
    }
    block.connection_type = nl::connection_type::none;
    if (!named)
        return false;
    int32_t found = -1;
    for (int32_t index = 0; index < state.provider_count && found < 0; ++index)
        if (provider_kind(state.providers[index].guid) == wanted)
            found = index;
    if (found < 0)
        return false;
    state.provider = found;
    std::memcpy(lobby_provider_guid(*lobby.game), state.providers[found].guid, 16);
    if (lobby_launch_block_active(lobby) && block.password[0] != '\0')
        copy_field(
            lobby_password(*lobby.game),
            kPasswordCopyBytes,
            bounded(block.password, sizeof block.password)
        );
    return true;
}

ConnectAction providers_handle_event(Lobby& lobby, ConnectState& state, Panel& panel) noexcept {
    auto& game = *lobby.game;
    if (panel.selected == kNoControl)
        return ConnectAction::none;
    if (panel_selected_is(panel, "DPLAY") || panel_selected_is(panel, "SELECT")) {
        const auto* list = panel_control(panel, "DPLAY");
        const auto row = list != nullptr ? list->list_selection : 0;
        panel.selected = kNoControl;
        if (row < 0 || row >= state.provider_count)
            return ConnectAction::none;
        state.provider = row;
        std::memcpy(lobby_provider_guid(game), state.providers[row].guid, 16);
        game.frontend_pending_signal = kSignalProviderChosen;
        play(lobby, "BigButton");
        return ConnectAction::provider;
    }
    if (panel_selected_is(panel, "PREVMENU")) {
        panel.selected = kNoControl;
        game.frontend_pending_signal = kSignalBack;
        play(lobby, "Previous");
        return ConnectAction::main_menu;
    }
    if (panel_selected_is(panel, "SETTINGS")) {
        play(lobby, "Options");
        panel.selected = kNoControl;
        return ConnectAction::options;
    }
    panel.selected = kNoControl;
    return ConnectAction::none;
}

int32_t panel_add_entry(
    Panel& panel, int32_t source, int16_t y, const char* text, const char* name
) noexcept {
    if (source <= 0 || source >= panel.count || panel.count >= static_cast<int32_t>(kPanelControls))
        return kNoControl;
    const auto index = panel.count++;
    auto& entry = panel.controls[static_cast<std::size_t>(index)];
    entry = panel.controls[static_cast<std::size_t>(source)];
    set_control_text(entry, text);
    set_control_name(entry, name);
    entry.y = y;
    entry.active = 1;
    entry.color = 0;
    return index;
}

// ---------------------------------------------------------------------------
// TCP

void tcp_open(Lobby& lobby, ConnectState& state, Panel& panel) noexcept {
    char address[128] = "";
    const auto& block = launch_block(lobby);
    state.launch_address_used = block.address[0] != '\0';
    if (state.launch_address_used) {
        copy_field(address, sizeof(address), bounded(block.address, sizeof block.address));
    } else if (
        state.settings.read_address == nullptr ||
        !state.settings.read_address(state.settings.context, address, sizeof(address))
    ) {
        address[0] = '\0';
    }
    panel_set_text(panel, "ADDRESS", address);
    panel.focus = panel_find(panel, "ADDRESS");
}

ConnectAction tcp_accept_launch_address(Lobby& lobby, ConnectState& state, Panel& panel) noexcept {
    auto& game = *lobby.game;
    state.launch_address_used = false;
    game.connection_flags |= kConnectFromOk | kConnectAddressSet;
    copy_field(state.address, sizeof(state.address), panel_text(panel, "ADDRESS"));
    const bool launched = lobby_launch_active(lobby);
    if (!launched) {
        play(lobby, "SmlButton");
        if (state.settings.write_address != nullptr)
            state.settings.write_address(state.settings.context, state.address);
    }
    game.connection_flags = static_cast<uint8_t>(
        (game.connection_flags & ~kConnectFromOk) |
        (launch_block(lobby).hosting != 0 ? kConnectFromOk : 0)
    );
    return connect_open_service(lobby, state, state.address) ? ConnectAction::game_list
                                                             : ConnectAction::providers;
}

ConnectAction tcp_accept_direct(Lobby& lobby, ConnectState& state, Panel& panel) noexcept {
    auto& game = *lobby.game;
    game.connection_flags |= kConnectFromOk | kConnectAddressSet;
    copy_field(state.address, sizeof(state.address), panel_text(panel, "ADDRESS"));
    play(lobby, "SmlButton");
    return connect_open_service(lobby, state, state.address) ? ConnectAction::game_list
                                                             : ConnectAction::providers;
}

ConnectAction tcp_handle_event(Lobby& lobby, ConnectState& state, Panel& panel) noexcept {
    auto& game = *lobby.game;
    if (panel.selected == kNoControl)
        return ConnectAction::none;
    const bool accept = panel_selected_is(panel, "OK") || panel_selected_is(panel, "ADDRESS");
    if (!accept && panel_selected_is(panel, "JOIN")) {
        game.connection_flags =
            static_cast<uint8_t>((game.connection_flags | kConnectAddressSet) & ~kConnectFromOk);
    } else if (accept) {
        game.connection_flags |= kConnectFromOk | kConnectAddressSet;
    } else if (panel_selected_is(panel, "PREV")) {
        panel.selected = kNoControl;
        game.frontend_pending_signal = kSignalTcpBack;
        play(lobby, "Previous");
        return ConnectAction::providers;
    } else {
        panel.selected = kNoControl;
        return ConnectAction::none;
    }
    panel.selected = kNoControl;
    copy_field(state.address, sizeof(state.address), panel_text(panel, "ADDRESS"));
    if (!lobby_launch_active(lobby)) {
        play(lobby, "SmlButton");
        if (state.settings.write_address != nullptr)
            state.settings.write_address(state.settings.context, state.address);
    }
    return connect_open_service(lobby, state, state.address) ? ConnectAction::game_list
                                                             : ConnectAction::providers;
}

bool connect_open_service(Lobby& lobby, ConnectState& state, const char* address) noexcept {
    auto& game = *lobby.game;
    const Provider* provider = state.provider >= 0 ? &state.providers[state.provider] : nullptr;
    if (lobby.net.open != nullptr && lobby.net.open(lobby.net.context, provider, address)) {
        game.session_flags |= kNetFlagLive;
        return true;
    }
    copy_field(state.error_text, sizeof(state.error_text), kServiceErrorText);
    game.frontend_state = kStateConnectionSelection;
    game.frontend_signal = kSignalInitialize;
    game.frontend_pending_signal = kSignalInitialize;
    return false;
}

void connect_show_error_text(Lobby& lobby, ConnectState& state) noexcept {
    if (state.error_text[0] == '\0')
        return;
    message(lobby, state.error_text);
    state.error_text[0] = '\0';
}

// ---------------------------------------------------------------------------
// SELGAME

bool game_list_update(Lobby& lobby, ConnectState& state, Panel& panel) noexcept {
    int32_t count = -1;
    if (lobby.net.enumerate != nullptr)
        count = lobby.net.enumerate(
            lobby.net.context, state.sessions, static_cast<int32_t>(kMaxSessions)
        );
    if (count < 0) {
        state.session_count = 0;
        return false;
    }
    state.session_count = std::min(count, static_cast<int32_t>(kMaxSessions));
    // The game list's values in the language shown, as 3.1c's list
    // translates them.
    const char* language =
        lobby.services.files != nullptr ? lobby.services.files->language : nullptr;
    const bool translates_map_names = language != nullptr && language[0] != '\0' &&
                                      formats::tdf::compare_nocase(language, "english") != 0;
    std::vector<std::string> columns[11];
    for (int32_t index = 0; index < state.session_count; ++index) {
        const auto& session = state.sessions[index];
        PlayerSetupInfo scratch{};
        const auto* info = session_info(session, scratch);
        char text[64];
        columns[0].emplace_back(bounded(session.name, 0x10));
        std::snprintf(
            text, sizeof(text), "%d/%d", info->options & 0xf, static_cast<int>(session.max_players)
        );
        columns[1].emplace_back(text);
        std::string map = bounded(session.name + 0x10, 0xf);
        while (!map.empty() && map.back() == ' ')
            map.pop_back();
        // In a language other than English the map's name is lowered and
        // looked up, as 3.1c's game list does: a name gamedata\translate.tdf
        // does not translate shows in lower case.
        if (translates_map_names) {
            std::transform(map.begin(), map.end(), map.begin(), [](char c) {
                return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            });
            map = lobby_translated(lobby, map.c_str());
        }
        columns[2].push_back(map);
        const uint32_t flags =
            static_cast<uint32_t>(info->options) | static_cast<uint32_t>(info->status) << 16;
        const char* status_text = "Open";
        if (!info_version_compatible(*info, lobby.local_version_major, lobby.wire_rules))
            status_text = "VER!";
        else if ((flags & option::game_closed) != 0)
            status_text = "Lock";
        else if ((flags & option::started) != 0)
            status_text = "Play";
        else if ((flags & (static_cast<uint32_t>(status::launch_only) << 16)) != 0)
            status_text = "BY";
        columns[3].emplace_back(lobby_translated(lobby, status_text));
        std::snprintf(text, sizeof(text), "%d", info->memory_mb);
        columns[4].emplace_back(text);
        std::snprintf(text, sizeof(text), "%d", info->metal_hundreds * 100);
        columns[5].emplace_back(text);
        std::snprintf(text, sizeof(text), "%d", info->energy_hundreds * 100);
        columns[6].emplace_back(text);
        std::snprintf(text, sizeof(text), "%d", info->lowest_latency);
        columns[7].emplace_back(text);
        const auto commander = flags & option::commander_mask;
        columns[8].emplace_back(lobby_translated(
            lobby,
            commander == 0                        ? "No"
            : commander == option::commander_step ? "Yes"
                                                  : "DM"
        ));
        columns[9].emplace_back(
            lobby_translated(lobby, (flags & option::unmapped) != 0 ? "Blk" : "Gray")
        );
        columns[10].emplace_back(
            lobby_translated(lobby, (flags & option::los_limited) != 0 ? "No" : "Yes")
        );
    }
    const char* lists[11] = {
        "GAMENAME",
        "PLAYERS",
        "MAPNAME",
        "STATUS",
        nullptr,
        "METAL",
        "ENERGY",
        "PING",
        "COMMANDER",
        "FULLMAP",
        "LOS"
    };
    for (std::size_t column = 0; column < 11; ++column)
        if (lists[column] != nullptr)
            panel_fill_list(panel, lists[column], std::move(columns[column]));
    game_list_refresh_join(lobby, state, panel);
    return true;
}

void game_list_refresh_join(Lobby& lobby, ConnectState& state, Panel& panel) noexcept {
    const auto row = selected_session(panel, state);
    PlayerSetupInfo scratch{};
    const bool empty = row < 0;
    const auto* info = empty ? &scratch : session_info(state.sessions[row], scratch);
    const bool compatible =
        !empty && info_version_compatible(*info, lobby.local_version_major, lobby.wire_rules);
    const uint32_t options = info->options;
    if (auto* watch = panel_control(panel, "WATCH")) {
        const auto high = ((~options & option::watching_allowed) | (options >> 8)) >> 3;
        const auto bits = (high | (options & option::started)) >> 4;
        watch->grayed = ((bits & 1U) != 0) || empty || !compatible;
    }
    if (auto* join = panel_control(panel, "JOINGAME")) {
        const auto bits = ((options & option::started) | (options >> 11)) >> 4;
        join->grayed = ((bits & 1U) != 0) || empty || !compatible;
    }
    const bool password = (info->status & status::password) != 0;
    panel_set_active(panel, "PASSWORDTEXT", password);
    panel_set_active(panel, "PASSWORD", password);
    panel.dirty = true;
}

bool game_list_open(Lobby& lobby, ConnectState& state, Panel& panel) noexcept {
    auto& game = *lobby.game;
    if ((game.gui_flags & kGuiFlagLobbyLaunch) == 0) {
        auto& block = launch_block(lobby);
        const bool address = block.address[0] != '\0';
        state.update_requested = address && block.hosting != 0;
        state.join_pending = lobby_launch_active(lobby) && address && block.hosting == 0;
        block.address[0] = '\0';
    }
    state.host_waiting = false;
    state.host_not_found_exiting = false;
    panel_set_text(panel, "PASSWORD", bounded(lobby_password(game), 10));
    panel_set_text(panel, "NICKNAME", bounded(lobby_nickname(game), 10));
    panel_set_grayed(panel, "JOIN", true);
    panel_set_grayed(panel, "WATCH", true);
    if (!state.update_requested && !game_list_update(lobby, state, panel)) {
        message(lobby, "Invalid TCP/IP Address");
        game.frontend_pending_signal = kSignalBack;
        return false;
    }
    panel.focus = panel_find(panel, "GAMENAME");
    // A player who left by itself (reason 2) is told nothing, unless this
    // machine left a room a 3.1c host was playing. That line is shown once.
    auto& me = local_player(lobby);
    if (lobby.leave_line != nullptr) {
        const std::string left(oa::data::languages::interface_text(lobby.leave_line));
        message(lobby, left.c_str());
        lobby.leave_line = nullptr;
    } else if (me.reject_reason != 0 && me.reject_reason != kRejectLeaving)
        message(lobby, lobby_translated(lobby, reject_reason_text(me.reject_reason)));
    me.reject_reason = 0;
    return true;
}

namespace {

/// Leaves the game list for NEWMULTI, as STARTNEW does.
///
/// @param[in,out] lobby Lobby state, its game block and services.
/// @param[in,out] panel The game list's panel.
/// @return ConnectAction::new_game.
ConnectAction start_new_game(Lobby& lobby, Panel& panel) noexcept {
    local_info(lobby).options &= static_cast<uint16_t>(~option::watcher);
    play_big_button(lobby);
    copy_field(lobby_nickname(*lobby.game), kNicknameCopyBytes, panel_text(panel, "NICKNAME"));
    panel.selected = kNoControl;
    return ConnectAction::new_game;
}

/// Chooses a listed game to join or watch once it passes the checks.
///
/// @param[in,out] lobby Lobby state, its game block and services.
/// @param[in,out] state Connection screens' state; chosen is set.
/// @param[in,out] panel The game list's panel.
/// @param row the game's row
/// @param watch whether to watch rather than play
/// @return join or watch, or none when a check refused it.
ConnectAction choose_listed_game(
    Lobby& lobby, ConnectState& state, Panel& panel, int32_t row, bool watch
) noexcept {
    auto& game = *lobby.game;
    PlayerSetupInfo scratch{};
    const auto* info = session_info(state.sessions[row], scratch);
    if ((info->status & status::launch_only) != 0 && !lobby_launch_active(lobby)) {
        message(lobby, "You must join this game via the Boneyards.");
        return ConnectAction::none;
    }
    if (!info_version_compatible(*info, lobby.local_version_major, lobby.wire_rules)) {
        message(lobby, "You do not have a compatible version for this game.");
        return ConnectAction::none;
    }
    if ((info->options & option::game_closed) != 0 || (info->options & option::started) != 0) {
        play(lobby, "Previous");
        return ConnectAction::none;
    }
    copy_field(lobby_nickname(game), kNicknameCopyBytes, panel_text(panel, "NICKNAME"));
    if (lobby_nickname(game)[0] == '\0') {
        panel.focus = panel_find(panel, "NICKNAME");
        message(lobby, "You must enter your name");
        return ConnectAction::none;
    }
    copy_field(lobby_password(game), kPasswordCopyBytes, panel_text(panel, "PASSWORD"));
    copy_field(local_password_field(lobby), kPasswordCopyBytes, panel_text(panel, "PASSWORD"));
    const auto host = lobby_host_slot(lobby);
    if (host != kNoSlot && (slot_info(lobby, host)->options & option::started) == option::started)
        game.session_flags |= kNetFlagGameStarted;
    state.chosen = state.sessions[row];
    if (watch) {
        local_info(lobby).options |= option::watcher;
        play(lobby, "Multi");
        game.frontend_pending_signal = kSignalWatch;
        return ConnectAction::watch;
    }
    local_info(lobby).options &= static_cast<uint16_t>(~option::watcher);
    play_big_button(lobby);
    game.frontend_pending_signal = kSignalJoin;
    return ConnectAction::join;
}

/// Shows the wait for the host with the seconds left at the previous frame.
///
/// @param[in,out] lobby Lobby state and its services.
/// @param[in,out] state Connection screens' state; host_wait_last_ms becomes `now`.
/// @param now the clock this frame, in milliseconds
/// @param first whether this is the wait's first frame, which shows 0
void show_host_wait(Lobby& lobby, ConnectState& state, uint32_t now, bool first) noexcept {
    uint32_t seconds = 0;
    if (!first) {
        const auto left =
            static_cast<int32_t>(state.host_wait_deadline_ms - state.host_wait_last_ms);
        const auto whole = left / static_cast<int32_t>(kMillisecondsPerSecond);
        seconds = whole < 0 || static_cast<uint32_t>(whole) > kWaitSecondsLimit
                      ? 0
                      : static_cast<uint32_t>(whole) / 2U * 2U;
    }
    char text[64];
    std::snprintf(
        text, sizeof(text), "%s (%u)", kWaitingForHostText, static_cast<unsigned>(seconds)
    );
    message(lobby, text);
    state.host_wait_last_ms = now;
}

/// Finds the listed game a pending join waits for.
///
/// @param state Connection screens' state, its session list polled.
/// @return the first listed game's row, or with join_game_name set the row
///         of the first game of that name; -1 when none is listed
int32_t awaited_session(const ConnectState& state) noexcept {
    if (state.join_game_name[0] == '\0')
        return state.session_count > 0 ? 0 : -1;
    const std::string wanted = bounded(state.join_game_name, kSessionGameNameBytes);
    for (int32_t row = 0; row < state.session_count; ++row) {
        std::string name = bounded(state.sessions[row].name, kSessionGameNameBytes);
        while (!name.empty() && name.back() == ' ')
            name.pop_back();
        if (name == wanted)
            return row;
    }
    return -1;
}

/// Polls the session list while waiting for the host and joins the game the join waits for.
///
/// @param[in,out] lobby Lobby state, its game block, network table and services.
/// @param[in,out] state Connection screens' state.
/// @param[in,out] panel The game list's panel.
/// @return join when the game was listed and chosen, else none.
ConnectAction poll_for_host(Lobby& lobby, ConnectState& state, Panel& panel) noexcept {
    if (!game_list_update(lobby, state, panel))
        return ConnectAction::none;
    const auto row = awaited_session(state);
    if (row < 0)
        return ConnectAction::none;
    state.host_waiting = false;
    message(lobby, "");
    if (auto* list = panel_control(panel, "GAMENAME"))
        list->list_selection = static_cast<int16_t>(row);
    return choose_listed_game(lobby, state, panel, row, false);
}

} // namespace

ConnectAction game_list_tick(Lobby& lobby, ConnectState& state, Panel& panel) noexcept {
    const uint32_t now = milliseconds(lobby);
    if (state.host_not_found_exiting) {
        if (static_cast<int32_t>(now - state.host_not_found_exit_ms) < 0)
            return ConnectAction::none;
        state.host_not_found_exiting = false;
        return ConnectAction::leave_game;
    }
    if (state.join_pending) {
        state.join_pending = false;
        state.host_waiting = true;
        state.host_wait_launch = lobby_launch_active(lobby);
        state.host_wait_deadline_ms = now + kHostWaitMilliseconds;
        state.host_wait_next_poll_ms = now + kHostPollMilliseconds;
        state.host_wait_last_ms = 0;
        const auto joined = poll_for_host(lobby, state, panel);
        if (!state.host_waiting)
            return joined;
        show_host_wait(lobby, state, now, true);
        return ConnectAction::none;
    }
    if (state.host_waiting) {
        show_host_wait(lobby, state, now, false);
        if (static_cast<int32_t>(now - state.host_wait_next_poll_ms) < 0)
            return ConnectAction::none;
        if (static_cast<int32_t>(now - state.host_wait_deadline_ms) >= 0) {
            state.host_waiting = false;
            message(lobby, "");
            if (state.host_wait_launch) {
                const auto& link = lobby.launch_link;
                if (link.join_failed != nullptr)
                    link.join_failed(link.context, kHostNotFoundText);
                lobby.game->frontend_state = kStateMainMenu;
                panel.selected = kNoControl;
                return ConnectAction::main_menu;
            }
            if (state.join_direct) {
                state.join_direct = false;
                message(lobby, kHostNotFoundText);
                return ConnectAction::none;
            }
            message(lobby, kHostNotFoundExitingText);
            state.host_not_found_exiting = true;
            state.host_not_found_exit_ms = now + kHostNotFoundExitMilliseconds;
            return ConnectAction::none;
        }
        state.host_wait_next_poll_ms = now + kHostPollMilliseconds;
        return poll_for_host(lobby, state, panel);
    }
    if (state.update_requested) {
        state.update_requested = false;
        return start_new_game(lobby, panel);
    }
    return ConnectAction::none;
}

ConnectAction game_list_handle_event(Lobby& lobby, ConnectState& state, Panel& panel) noexcept {
    auto& game = *lobby.game;
    if (panel.selected == kNoControl)
        return ConnectAction::none;
    if (state.update_requested) {
        state.update_requested = false;
        return start_new_game(lobby, panel);
    }
    if (panel_selected_is(panel, "UPDATE")) {
        copy_field(local_password_field(lobby), kPasswordCopyBytes, panel_text(panel, "PASSWORD"));
        play(lobby, "Multi");
        (void)game_list_update(lobby, state, panel);
        panel.selected = kNoControl;
        return ConnectAction::none;
    }
    if (panel_selected_is(panel, "PREVMENU")) {
        panel.selected = kNoControl;
        // A launched game goes back to the main menu.
        if (lobby_launch_active(lobby)) {
            game.frontend_state = kStateMainMenu;
            return ConnectAction::main_menu;
        }
        game.frontend_pending_signal = kSignalBack;
        play(lobby, "Previous");
        return ConnectAction::providers;
    }
    if (panel_selected_is(panel, "STARTNEW"))
        return start_new_game(lobby, panel);
    const auto& control = panel.controls[static_cast<std::size_t>(panel.selected)];
    const bool watch = panel_selected_is(panel, "WATCH");
    const bool join =
        watch || panel_selected_is(panel, "JOINGAME") || control.type == ControlType::list_box;
    if (!join) {
        panel.selected = kNoControl;
        return ConnectAction::none;
    }
    if (control.type == ControlType::list_box)
        game_list_refresh_join(lobby, state, panel);
    const auto row = selected_session(panel, state);
    panel.selected = kNoControl;
    if (row < 0)
        return ConnectAction::none;
    return choose_listed_game(lobby, state, panel, row, watch);
}

// ---------------------------------------------------------------------------
// NEWMULTI

void new_game_open(Lobby& lobby, ConnectState& state, Panel& panel) noexcept {
    auto& game = *lobby.game;
    state.host_at_once = false;
    // An active launch names the player and the game. The game name is what the
    // battle room reports; the entry keeps 16 characters, as GAMENAME does.
    if (lobby_launch_block_active(lobby)) {
        const auto& launched = launch_block(lobby);
        if (launched.user_name[0] != '\0') {
            copy_field(
                lobby_nickname(game),
                kNicknameCopyBytes,
                bounded(launched.user_name, kLaunchNicknameLimit)
            );
        }
        if (launched.session_name[0] != '\0') {
            copy_field(
                lobby_game_name(game), 0x11, bounded(launched.session_name, kNameEntryBytes)
            );
        }
    }
    if (lobby_nickname(game)[0] == '\0' && state.settings.user_name != nullptr) {
        char name[0x11] = "";
        if (state.settings.user_name(state.settings.context, name, sizeof(name)))
            copy_field(lobby_nickname(game), 0x11, name);
    }
    panel_set_text(panel, "GAMENAME", bounded(lobby_game_name(game), kNameEntryBytes));
    if (auto* entry = panel_control(panel, "GAMENAME"))
        entry->value = static_cast<int16_t>(kNameEntryBytes);
    panel_set_text(panel, "NICKNAME", bounded(lobby_nickname(game), kNameEntryBytes));
    if (auto* entry = panel_control(panel, "NICKNAME"))
        entry->value = static_cast<int16_t>(kNameEntryBytes);
    panel.focus = panel_find(panel, "GAMENAME");
    // A launched game goes on to host without waiting for OK.
    if (lobby_launch_active(lobby)) {
        state.host_at_once = true;
        game.frontend_pending_signal = kSignalHost;
        return;
    }
    const char* password = local_password_field(lobby);
    if (password[0] == '\0')
        password = lobby_password(game);
    panel_set_text(panel, "PASSWORD", bounded(password, 10));
}

ConnectAction new_game_handle_event(Lobby& lobby, ConnectState&, Panel& panel) noexcept {
    auto& game = *lobby.game;
    if (panel.selected == kNoControl)
        return ConnectAction::none;
    if (panel_selected_is(panel, "GAMENAME")) {
        panel.focus = panel_find(panel, "NICKNAME");
    } else if (panel_selected_is(panel, "NICKNAME")) {
        panel.focus = panel_find(panel, "PASSWORD");
    } else if (panel_selected_is(panel, "PASSWORD")) {
        panel.focus = panel_find(panel, "OK");
    } else if (panel_selected_is(panel, "OK")) {
        play(lobby, "BigButton");
        panel.selected = kNoControl;
        copy_field(lobby_password(game), kPasswordCopyBytes, panel_text(panel, "PASSWORD"));
        const auto name = std::string(panel_text(panel, "GAMENAME"));
        if (name.empty()) {
            panel.focus = panel_find(panel, "GAMENAME");
            message(lobby, "You must enter a game name");
            return ConnectAction::none;
        }
        const auto nickname = std::string(panel_text(panel, "NICKNAME"));
        if (nickname.empty()) {
            panel.focus = panel_find(panel, "NICKNAME");
            message(lobby, "You must enter your name");
            return ConnectAction::none;
        }
        copy_field(lobby_game_name(game), 0x11, name);
        copy_field(lobby_nickname(game), 0x11, nickname);
        game.frontend_pending_signal = kSignalHost;
        return ConnectAction::host;
    } else if (panel_selected_is(panel, "CANCEL")) {
        play(lobby, "Previous");
        panel.selected = kNoControl;
        return ConnectAction::game_list;
    }
    panel.selected = kNoControl;
    panel.dirty = true;
    return ConnectAction::none;
}

// ---------------------------------------------------------------------------
// Session creation and joining

bool connect_host(Lobby& lobby, ConnectState&) noexcept {
    auto& game = *lobby.game;
    if (lobby.net.host == nullptr)
        return false;
    // The host's lobby block is ready first: the session is created with its
    // full name and user bytes, so the first search already shows them.
    lobby_reset(lobby, game);
    lobby_player_count(game) = 1;
    lobby_seat_local(lobby, 0, true, lobby_nickname(game));
    auto& info = local_info(lobby);
    std::snprintf(info.password, sizeof(info.password), "%s", lobby_password(game));
    if (info.password[0] != '\0')
        info.status |= status::password;
    // An active launch's player limit, when above 1, caps the session.
    uint32_t max_players = kMaxSessionPlayers;
    if (lobby_launch_block_active(lobby) && launch_block(lobby).player_limit > 1)
        max_players = std::min<uint32_t>(
            static_cast<uint32_t>(launch_block(lobby).player_limit), kMaxSessionPlayers
        );
    lobby_session_players(game) = static_cast<int32_t>(max_players);
    char name[kSessionNameBytes];
    uint8_t user[kSessionUserBytes];
    lobby_session_description(lobby, name, user);
    HostRequest request{
        lobby_game_name(game),
        lobby_nickname(game),
        lobby_password(game),
        max_players,
        game.players,
        name,
        user
    };
    uint32_t id = 0;
    if (lobby.net.host(lobby.net.context, &request, &id) != LobbyResult::ok)
        return false;
    game.session_flags |= kNetFlagLive;
    local_player(lobby).player_id = id;
    info.net_id = id;
    return true;
}

bool connect_join(Lobby& lobby, ConnectState& state, bool watch) noexcept {
    auto& game = *lobby.game;
    if (lobby.net.join == nullptr)
        return false;
    JoinRequest request{
        &state.chosen, lobby_nickname(game), lobby_password(game), watch, game.players
    };
    uint32_t id = 0;
    const auto result = lobby.net.join(lobby.net.context, &request, &id);
    if (result != LobbyResult::ok) {
        message(lobby, lobby_translated(lobby, reject_reason_text(static_cast<uint8_t>(result))));
        return false;
    }
    const auto watcher = local_info(lobby).options & option::watcher;
    lobby_reset(lobby, game);
    game.session_flags |= kNetFlagLive;
    local_player(lobby).player_id = id;
    lobby_seat_local(lobby, 0, false, lobby_nickname(game));
    local_info(lobby).options |= static_cast<uint16_t>(watcher);
    std::snprintf(
        local_info(lobby).password, sizeof(local_info(lobby).password), "%s", lobby_password(game)
    );
    lobby_send_player_info(lobby);
    lobby_request_color(lobby, 0);
    return true;
}

} // namespace oa::ui::frontend_multiplayer
