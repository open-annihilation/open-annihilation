// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Battleroom (LOUNGE2.GUI): slot rules, rows, options, start checks.
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/room_limits.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"

#include "oa/base/game_loop.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/map_pack/map_name.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/data/mod_profile/overrides.hpp"
#include "oa/netgame/player_slots.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/private_channel.hpp"
#include "oa/netgame/records.hpp"
#include "oa/netgame/recorder_messages.hpp"
#include "oa/netgame/unicode_chat.hpp"
#include "oa/ui/frontend_multiplayer/dialogs.hpp"
#include "oa/ui/frontend_multiplayer/get_map.hpp"
#include "oa/ui/frontend_multiplayer/team_rules.hpp"
#include "oa/sim/messages.hpp"
#include "oa/sim/mission_units/map_units.hpp"
#include "oa/base/text.hpp"
#include "oa/core/side.h"
#include "oa/data/languages/interface_text.hpp"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <iostream>
#include <iterator>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace oa::ui::frontend_multiplayer {

namespace {

// Where the accessors below find their Game and Player fields, which the
// packed records may leave unaligned.
constexpr std::size_t kChatRingOffset = offsetof(Game, chat_lines);
// Bytes of the longest UTF-8 character.
constexpr std::size_t kLongestCharacter = 4;
constexpr std::size_t kSessionPlayersOffset = offsetof(Game, session_player_limit);
constexpr std::size_t kPlayerCountOffset = offsetof(Game, player_count);
constexpr std::size_t kChatHeadOffset = offsetof(Game, chat_head);
constexpr std::size_t kChatTailOffset = offsetof(Game, chat_tail);
constexpr std::size_t kGameNameOffset = offsetof(Game, game_name);
constexpr std::size_t kNicknameOffset = offsetof(Game, nickname);
constexpr std::size_t kPasswordOffset = offsetof(Game, password);
constexpr std::size_t kLobbyOptionOffset = offsetof(Game, setup_options);
constexpr std::size_t kMaxUnitsOffset = offsetof(Game, max_units);
constexpr std::size_t kMaxUnitsDefaultOffset = offsetof(Game, max_units_setting);
constexpr std::size_t kScreenWidthOffset = offsetof(Game, screen_width);
constexpr std::size_t kScreenHeightOffset = offsetof(Game, screen_height);
constexpr std::size_t kProviderGuidOffset = offsetof(Game, provider_guid);
// Game.start_options, in order: commander rule, unmapped, LOS limited, LOS true.
constexpr std::size_t kCommanderOptionOffset = offsetof(Game, start_options);
constexpr std::size_t kUnmappedOptionOffset = kCommanderOptionOffset + sizeof(int32_t);
constexpr std::size_t kLosLimitedOptionOffset = kUnmappedOptionOffset + sizeof(int32_t);
constexpr std::size_t kLosTrueOptionOffset = kLosLimitedOptionOffset + sizeof(int32_t);
constexpr std::size_t kSideCountOffset = offsetof(Game, side_count);
constexpr std::size_t kPlayerPingOffset = offsetof(Player, latency);
constexpr std::size_t kPlayerJoinTickOffset = offsetof(Player, join_tick);
constexpr std::size_t kPlayerAlliedBackOffset = offsetof(Player, allied_by);
constexpr std::size_t kPlayerTeamOffset = offsetof(Player, team);

constexpr std::size_t kSessionRulesOffset = offsetof(Game, session_rules);

constexpr int32_t kMaxUnitsFloor = 20; // MAXUNITS knob value + 20
// While a launch is active the version byte in the session description is
// biased by this, as status::launch_only marks.
constexpr int32_t kLaunchVersionBias = 100;
// Game.setup_options bit a launch's lock sets: the host's options
// stay locked.
constexpr uint16_t kSetupOptionsLocked = 0x0001;
// Game.gui_flags bit of a game a lobby program launched.
constexpr uint8_t kGuiFlagLobbyLaunch = 0x10;
// The PREVMENU caption, and the longest provider label that takes its place.
constexpr const char* kPreviousMenuCaption = "Previous Menu";
constexpr std::size_t kProviderCaptionLimit = 11;
// LaunchBlock values the battle room applies.
constexpr int32_t kLaunchDeathFirst = 1; // continues; 2 ends, 3 deathmatch
constexpr int32_t kLaunchDeathLast = 3;
constexpr int32_t kLaunchLosTrue = 1;
constexpr int32_t kLaunchLosCircular = 2;
constexpr int32_t kLaunchLosPermanent = 3;
constexpr int32_t kLaunchCheatsAllowed = 2;
constexpr int32_t kLaunchLocationFixed = 1;
constexpr int32_t kLaunchMappingUnmapped = 1;
constexpr int32_t kLaunchWatchingAllowed = 2;
constexpr int32_t kResourceSliderMaximum = 0x2711;
constexpr int32_t kDefaultResource = 1000;
constexpr uint32_t kComputerRejectTicks = 0x1f;
constexpr uint32_t kStatsInterval = 0x3c;
constexpr int16_t kStartFrames = 9; // battlestart sequence length
constexpr int16_t kStartFrameStop = 8;
constexpr int16_t kStartFrameSound = 4;
constexpr uint32_t kStartFrameTicks = 4;
// While everyone is ready START's art is lit at the tick's low five bits,
// cycling through light levels 0..31.
constexpr uint32_t kStartLightMask = 0x1f;
constexpr uint8_t kRejectPlayer = 1;
constexpr uint8_t kRejectLeaving = 2;
constexpr uint8_t kRejectWatching = 9;
constexpr uint8_t kRejectCreatorLeft = 10;
constexpr uint8_t kRejectDeathmatch = 0xb;
constexpr uint8_t kRejectVersion = 8; // "You need a newer version of the game to enter"
constexpr uint32_t kTicksPerSecond = 30;
constexpr uint32_t kMillisecondsPerSecond = 1000;
constexpr std::size_t kComputerNameBytes = 0x10; // session name of a computer player
constexpr uint16_t kSeatedMaxUnits = 100;        // PlayerSetupInfo.max_units as a player is seated
// Player.machine_flags: the player joined without the battle room; cleared
// when a computer player is seated.
constexpr uint8_t kMachineFlagJoinedWithoutBattleroom = 0x02;
constexpr uint32_t kBroadcastId = 0;
constexpr uint32_t kNoPlayerId = 0xffffffffU;
constexpr uint32_t kHostMachineGroup = 1;
constexpr uint32_t kSessionFlagGame = 0x4;     // published on every session
constexpr uint32_t kSessionFlagStarted = 0x20; // the game started; stays set once published
constexpr uint32_t kMachineGroupCount = 10;    // groups 1..10
constexpr uint16_t kRunFlagPaused = 0x01;      // Game.sim_run_flags: the game is paused

// The ten row templates cloned per slot, in the game's table order.
constexpr const char* kRowKeys[] = {
    "PLAYERx", "READYx", "LOGOx", "SIDEx", "PINGx", "MEMx", "RESx", "ALLYx", "CDx", "TEAMICONSx"
};

enum RowKey {
    row_player,
    row_ready,
    row_logo,
    row_side,
    row_ping,
    row_mem,
    row_res,
    row_ally,
    row_cd,
    row_team
};

// Host-only option buttons grayed for clients.
/// The battle room buttons a mod's display rules may add, and what each
/// says when the host presses it, as if typed.
struct ModLobbyButton {
    uint8_t bit{};
    const char* name{};
    const char* line{};
};

constexpr ModLobbyButton kModLobbyButtons[] = {
    {lobby_button::autoteam, "AUTOTEAM", "+autoteam"},
    {lobby_button::autopause, "AUTOPAUSE", ".autopause"},
    {lobby_button::randomteam, "RANDOMTEAM", "+randomteam"},
    {lobby_button::crcreport, "CRCREPORT", ".crcreport"},
};

constexpr const char* kHostOptionButtons[] = {
    "COMMANDER", "MAPPING", "LOSTYPE", "WATCHING", "CHEATING", "FIXEDLOC", "GAMEOPEN"
};

uint8_t* game_bytes(Game& game, std::size_t offset) noexcept {
    return reinterpret_cast<uint8_t*>(&game) + offset;
}

uint8_t* player_bytes(Player& player, std::size_t offset) noexcept {
    return reinterpret_cast<uint8_t*>(&player) + offset;
}

PlayerSetupInfo& info_of(Lobby& lobby, const Player& player) noexcept {
    auto* info = player_info(lobby, player);
    return info != nullptr ? *info : lobby.infos[0];
}

void play(Lobby& lobby, const char* name) noexcept {
    if (lobby.services.play_sound != nullptr)
        lobby.services.play_sound(lobby.services.context, name);
}

void message(Lobby& lobby, const char* text) noexcept {
    if (lobby.services.message != nullptr)
        lobby.services.message(lobby.services.context, text);
}

void notify(Lobby& lobby, int32_t event) noexcept {
    if (lobby.services.notify != nullptr)
        lobby.services.notify(lobby.services.context, event);
}

uint32_t now(Lobby& lobby) noexcept {
    return lobby.services.tick != nullptr ? lobby.services.tick(lobby.services.context) : 0;
}

/// Marks the local player's block as OA's: it holds a game disc and carries
/// the engine signature. OA asks for no disc, but a 3.1c host still counts
/// them before START, so every OA player reports one. The signature is kept
/// in the block itself, so the blocks a match sends carry it too.
///
/// @param info the local player's block
void mark_local_block(PlayerSetupInfo& info) noexcept {
    info.status = static_cast<uint16_t>(info.status | status::has_disc);
    netgame::mark_engine_signature(reinterpret_cast<uint8_t*>(&info));
}

bool map_selected(Lobby& lobby) noexcept {
    return lobby.maps.selected != nullptr && lobby.maps.selected(lobby.maps.context);
}

const char* map_name(Lobby& lobby) noexcept {
    const char* name = lobby.maps.name != nullptr ? lobby.maps.name(lobby.maps.context) : nullptr;
    return name != nullptr ? name : "";
}

int32_t map_memory(Lobby& lobby) noexcept {
    return lobby.maps.memory_mb != nullptr ? lobby.maps.memory_mb(lobby.maps.context) : 0;
}

/// Queues a lobby record from a player of this machine, reaching each machine once.
///
/// While machines are shared a broadcast goes to one player of every
/// machine instead, and to Lobby::refused_joiner when one is set.
///
/// @param lobby Lobby state holding the network table.
/// @param from Id of the local or computer player the record is from.
/// @param to Destination player id; 0 broadcasts.
/// @param data Record bytes.
/// @param size Record length in bytes.
/// @param unguaranteed Send it at once without guaranteed delivery, as a ping is.
void send(
    Lobby& lobby,
    uint32_t from,
    uint32_t to,
    const uint8_t* data,
    std::size_t size,
    bool unguaranteed = false
) noexcept {
    const auto one = [&](uint32_t target) {
        if (lobby.net.send_from != nullptr)
            lobby.net.send_from(lobby.net.context, from, target, data, size, unguaranteed);
        else if (lobby.net.send != nullptr)
            lobby.net.send(lobby.net.context, target, data, size);
    };
    if (to != kBroadcastId || lobby.game->shared_machines == 0) {
        one(to);
        return;
    }
    uint32_t ids[kSlotCount];
    const auto count = machine_broadcast_targets(*lobby.game, ids);
    for (int32_t i = 0; i < count; ++i)
        one(ids[i]);
    // A refused joiner holds no slot, so no machine's target names it.
    if (lobby.refused_joiner != 0)
        one(lobby.refused_joiner);
}

/// Sends every queued record now.
///
/// @param lobby Lobby state holding the network table.
void flush(Lobby& lobby) noexcept {
    if (lobby.net.flush != nullptr)
        lobby.net.flush(lobby.net.context);
}

/// Returns this machine's millisecond clock, or the 30 Hz clock in milliseconds without one.
///
/// @param lobby Lobby state holding the services.
/// @return Milliseconds.
uint32_t milliseconds(Lobby& lobby) noexcept {
    if (lobby.services.milliseconds != nullptr)
        return lobby.services.milliseconds(lobby.services.context);
    return now(lobby) * kMillisecondsPerSecond / kTicksPerSecond;
}

bool occupied_by(const Player& player, uint8_t a, uint8_t b = 0xff) noexcept {
    return player.in_use != 0 && (player.status == a || player.status == b);
}

bool local_or_computer(const Player& player) noexcept {
    return occupied_by(player, kSlotLocal, kSlotComputer);
}

/// Returns the id the records that concern no one player leave from: the
/// first seated local or computer player in slot order.
///
/// @param lobby Lobby state.
/// @return The player id; the local player's when none is seated.
uint32_t primary_id(Lobby& lobby) noexcept {
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        const auto& player = slot_player(lobby, slot);
        if (local_or_computer(player))
            return player.player_id;
    }
    return local_player(lobby).player_id;
}

std::string_view text_view(const char* text, std::size_t bound) noexcept {
    return {text, ::strnlen(text, bound)};
}

/// Copies text into a fixed field, cut to fit and terminated.
///
/// @param[out] out Field receiving the text.
/// @param capacity Size of the field, terminator included; at least 1.
/// @param text Text to copy.
void copy_text(char* out, std::size_t capacity, std::string_view text) noexcept {
    const auto length = std::min(text.size(), capacity - 1);
    std::memmove(out, text.data(), length);
    out[length] = '\0';
}

/// Tells whether text begins with a prefix, compared without case.
///
/// @param text Text to test.
/// @param prefix Prefix to find.
/// @return True when text starts with prefix ignoring case.
bool starts_nocase(std::string_view text, std::string_view prefix) noexcept {
    if (text.size() < prefix.size())
        return false;
    for (std::size_t i = 0; i < prefix.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(text[i])) !=
            std::tolower(static_cast<unsigned char>(prefix[i])))
            return false;
    return true;
}

void format(char (&out)[64], const char* pattern, int32_t value) noexcept {
    std::snprintf(out, sizeof(out), pattern, static_cast<int>(value));
}

bool host_is_local(Lobby& lobby) noexcept {
    return (local_info(lobby).role & kRoleHost) != 0;
}

/// Counts the occupied remote slots.
///
/// @param lobby Lobby state.
/// @return The count.
int32_t remote_count(Lobby& lobby) noexcept {
    int32_t count = 0;
    for (int32_t slot = 0; slot < kSlotCount; ++slot)
        if (occupied_by(slot_player(lobby, slot), kSlotRemote))
            ++count;
    return count;
}

/// Counts the seated slots whose status, read as a signed byte, is below
/// a computer player's: the local humans, as the start gate counts them
/// when a game may start with one human and computer players.
///
/// @param lobby Lobby state.
/// @return The count.
int32_t local_human_count(Lobby& lobby) noexcept {
    int32_t count = 0;
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        const auto& player = slot_player(lobby, slot);
        if (player.in_use != 0 &&
            static_cast<int8_t>(player.status) < static_cast<int8_t>(kSlotComputer))
            ++count;
    }
    return count;
}

/// The setup rules of the lobby's profile; 3.1c's without one.
///
/// @param lobby Lobby state.
/// @return The rules.
const data::match_rules::SetupRules& setup_rules(const Lobby& lobby) noexcept {
    static constexpr data::match_rules::SetupRules base{};
    return lobby.rules != nullptr ? lobby.rules->setup : base;
}

/// The team rules of the lobby's profile; 3.1c's without one.
///
/// @param lobby Lobby state.
/// @return The rules.
const data::match_rules::TeamsRules& teams_rules(const Lobby& lobby) noexcept {
    static constexpr data::match_rules::TeamsRules base{};
    return lobby.rules != nullptr ? lobby.rules->teams : base;
}

/// Counts the occupied local slots and the remote slots whose info reports them playing.
///
/// @param lobby Lobby state.
/// @return The count.
int32_t playing_count(Lobby& lobby) noexcept {
    int32_t count = 0;
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        if (occupied_by(player, kSlotLocal) || slot_remote_playing(lobby, player))
            ++count;
    }
    return count;
}

/// Counts the occupied computer slots and the remote slots whose info reports them defeated.
///
/// @param lobby Lobby state.
/// @return The count.
int32_t idle_participant_count(Lobby& lobby) noexcept {
    int32_t count = 0;
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        if (occupied_by(player, kSlotComputer) || slot_remote_defeated(lobby, player))
            ++count;
    }
    return count;
}

void set_group(Panel& panel, const char* pattern, int32_t slot, int32_t value) {
    char name[64];
    format(name, pattern, slot);
    panel_set_value(panel, name, value);
}

Control* row_control(Panel& panel, const char* pattern, int32_t slot) noexcept {
    char name[64];
    format(name, pattern, slot);
    return panel_control(panel, name);
}

/// Sets or clears one bit of an option word.
///
/// @param[in,out] options option word
/// @param bit the bit
/// @param on whether it is set
void set_option_bit(uint16_t& options, uint16_t bit, bool on) noexcept {
    options = static_cast<uint16_t>(on ? options | bit : options & ~bit);
}

/// Tells whether two side names are the same, ignoring ASCII case.
///
/// @param left one name
/// @param right the other
/// @return true when every character matches, ignoring ASCII case
bool same_side_name(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto one = static_cast<unsigned char>(left[index]);
        const auto other = static_cast<unsigned char>(right[index]);
        if (std::tolower(one) != std::tolower(other))
            return false;
    }
    return true;
}

/// Posts that a launch named a side this game does not have.
///
/// @param[in,out] lobby Lobby state.
/// @param side The number or name as the launch wrote it.
void post_launch_side(Lobby& lobby, std::string_view side) {
    std::string line(
        oa::data::languages::interface_text(
            "*** The launch names side {side}, which this game does not have."
        )
    );
    const auto mark = line.find("{side}");
    if (mark != std::string::npos)
        line.replace(mark, std::string_view("{side}").size(), side);
    lobby_post_chat(lobby, line.c_str());
}

/// Posts that a launch gave a computer player a side it may not take.
///
/// @param[in,out] lobby Lobby state.
/// @param side The number or name as the launch wrote it.
void post_computer_launch_side(Lobby& lobby, std::string_view side) {
    std::string line(
        oa::data::languages::interface_text(
            "*** The launch gives a computer player side {side}, which computer players cannot "
            "take in this game."
        )
    );
    const auto mark = line.find("{side}");
    if (mark != std::string::npos)
        line.replace(mark, std::string_view("{side}").size(), side);
    lobby_post_chat(lobby, line.c_str());
}

/// Seats a launch faction or SIDEDATA name, or leaves the room's side.
///
/// A faction N from 1 is side N - 1 when the room has it. With no faction, a
/// name takes the side whose SIDEDATA name matches, ignoring ASCII case. A
/// number or name the room lacks leaves the side and posts the launch line
/// once. An empty name with no faction leaves the side.
///
/// @param[in,out] lobby Lobby state.
/// @param[in,out] info The player's lobby block.
/// @param faction The launch faction; none when the launch named a side.
/// @param faction_name The SIDEDATA name; empty when the launch gave a faction.
void seat_named_side(
    Lobby& lobby, PlayerSetupInfo& info, int32_t faction, std::string_view faction_name
) {
    namespace nl = oa::ui::frontend_multiplayer::launch;
    if (lobby.game == nullptr)
        return;
    const uint8_t count = lobby_side_count(*lobby.game);
    if (faction != nl::faction::none) {
        const int32_t side = faction - 1;
        if (side >= 0 && static_cast<uint32_t>(side) < count) {
            info.side = static_cast<uint8_t>(side);
            info.options = static_cast<uint16_t>(info.options & ~option::watcher);
        } else {
            post_launch_side(lobby, std::to_string(side));
        }
        return;
    }
    if (faction_name.empty())
        return;
    const auto limit = std::min(lobby.side_names.size(), static_cast<std::size_t>(count));
    for (std::size_t index = 0; index < limit; ++index) {
        if (!same_side_name(lobby.side_names[index], faction_name))
            continue;
        info.side = static_cast<uint8_t>(index);
        info.options = static_cast<uint16_t>(info.options & ~option::watcher);
        return;
    }
    post_launch_side(lobby, faction_name);
}

/// Seats the local player as an active launch says: side or watching, team and colour.
///
/// A faction N from 1 becomes side N - 1 when the room has it, and a faction
/// name the side whose SIDEDATA name matches, ignoring case. A side the room
/// lacks is left as the room chose it and named once in the chat. A watcher
/// watches instead. A team 1 to 5 becomes the player's team, which is sent
/// with the alliances refreshed; a colour 1 to 10 is asked for as a click on
/// the player's logo asks, and a colour another player has gives the next
/// free one. A field that is 0 leaves its setting alone.
///
/// @param[in,out] lobby Lobby state; its launch must be active.
/// @param[in,out] me the local player
/// @param[in,out] info the local player's lobby block
void apply_launch_seat(Lobby& lobby, Player& me, PlayerSetupInfo& info) noexcept {
    namespace nl = oa::ui::frontend_multiplayer::launch;
    const nl::LaunchBlock& block = *lobby.launch_link.block;
    if (block.watcher != 0) {
        info.side = 0;
        info.options |= option::watcher;
    } else {
        seat_named_side(
            lobby, info, block.faction, text_view(block.faction_name, sizeof block.faction_name)
        );
    }
    if (block.team >= nl::kLaunchTeamFirst && block.team <= nl::kLaunchTeamLast) {
        lobby_player_team(me) = static_cast<uint8_t>(block.team - nl::kLaunchTeamFirst);
        lobby_send_team(lobby, me);
        lobby_update_ally_matrix(lobby);
    }
    if (block.color >= nl::kLaunchColourFirst && block.color <= nl::kLaunchColourLast)
        lobby_request_color(lobby, static_cast<uint32_t>(block.color - nl::kLaunchColourFirst));
}

/// Applies an active launch's options to the local player as the battle room opens.
///
/// The host takes the unit limit, the lock, the commander rule, the resources,
/// line of sight, cheats, start positions, mapping and watching. A joiner
/// takes none of them; the host's rules stay the host's. Both then take the
/// seat (apply_launch_seat). Energy and metal become the sliders' first
/// values. A field that is 0 leaves its setting alone.
///
/// @param[in,out] lobby Lobby state; its launch must be active.
/// @param[in,out] me the local player
/// @param[in,out] info the local player's lobby block
/// @param[in,out] energy the energy slider's first value
/// @param[in,out] metal the metal slider's first value
/// @param host true when this machine hosts the game
void apply_launch_options(
    Lobby& lobby, Player& me, PlayerSetupInfo& info, int32_t& energy, int32_t& metal, bool host
) noexcept {
    const auto& block = *lobby.launch_link.block;
    if (!host) {
        apply_launch_seat(lobby, me, info);
        return;
    }
    auto& game = *lobby.game;
    if (block.max_units != 0) {
        info.max_units = static_cast<uint16_t>(block.max_units);
        lobby_max_units(game) = static_cast<uint16_t>(block.max_units);
    }
    uint16_t word = lobby_option_word(game);
    set_option_bit(word, kSetupOptionsLocked, block.lock_options != 0);
    lobby_option_word(game) = word;
    if (block.death_option >= kLaunchDeathFirst && block.death_option <= kLaunchDeathLast) {
        const auto rule = static_cast<uint32_t>(block.death_option - kLaunchDeathFirst);
        std::memcpy(game_bytes(game, kCommanderOptionOffset), &rule, sizeof(rule));
        std::memcpy(game_bytes(game, kSessionRulesOffset), &rule, sizeof(rule));
        info.options = static_cast<uint16_t>(
            (info.options & ~option::commander_mask) | ((rule << 11) & option::commander_mask)
        );
    }
    if (block.energy != 0)
        energy = block.energy;
    if (block.metal != 0)
        metal = block.metal;
    uint16_t options = info.options;
    if (block.los == kLaunchLosTrue) {
        options |= option::los_limited | option::los_true;
    } else if (block.los == kLaunchLosCircular) {
        options = static_cast<uint16_t>((options & ~option::los_true) | option::los_limited);
    } else if (block.los == kLaunchLosPermanent) {
        options = static_cast<uint16_t>(options & ~option::los_limited);
    }
    if (block.cheats != 0)
        set_option_bit(options, option::cheats_allowed, block.cheats == kLaunchCheatsAllowed);
    if (block.location != 0)
        set_option_bit(options, option::fixed_locations, block.location == kLaunchLocationFixed);
    if (block.mapping != 0)
        set_option_bit(options, option::unmapped, block.mapping == kLaunchMappingUnmapped);
    if (block.watching != 0)
        set_option_bit(options, option::watching_allowed, block.watching == kLaunchWatchingAllowed);
    info.options = options;
    apply_launch_seat(lobby, me, info);
}

void set_host_options(Game& game, uint16_t options) noexcept {
    const auto store = [&](std::size_t offset, uint32_t value) {
        std::memcpy(game_bytes(game, offset), &value, sizeof(value));
    };
    store(kLosLimitedOptionOffset, (options & option::los_limited) >> 9);
    store(kLosTrueOptionOffset, (options & option::los_true) >> 10);
    store(kCommanderOptionOffset, (options & option::commander_mask) >> 11);
    store(kUnmappedOptionOffset, (options & option::unmapped) >> 8);
}

// ---------------------------------------------------------------------------
// The recorder in the battle room (Lobby::wire_rules.recorder_protocol)

// A recorder sends the host's options to machines whose recorder is at least this version.
constexpr uint8_t kRecorderOptionsProtocol = 5;
// A chat line keeps a NUL in its last byte.
constexpr std::size_t kChatLineChars = sizeof(netgame::ChatRecord::text) - 1;

// Bytes past a cut that show whether a UTF-8 character spans it.
constexpr std::size_t kChatLookahead = 4;
// The most bytes of a line Unicode chat reads: more than four records hold.
constexpr std::size_t kChatReadBytes = 512;

/// Gives how much of a line fits a limit without cutting a character.
///
/// @param line The line, zero-terminated.
/// @param limit The most bytes kept.
/// @return The bytes kept.
std::size_t chat_bytes_within(const char* line, std::size_t limit) noexcept {
    return base::text::whole_characters(
        std::string_view(line, ::strnlen(line, limit + kChatLookahead)), limit
    );
}

/// Tells whether a slot's machine reads chat as UTF-8: its block says so.
///
/// @param lobby Lobby state.
/// @param slot The slot.
/// @return True when it does.
bool slot_reads_unicode_chat(Lobby& lobby, int32_t slot) noexcept {
    const auto* info = slot >= 0 ? slot_info(lobby, slot) : nullptr;
    return info != nullptr &&
           netgame::announces_unicode_chat(reinterpret_cast<const uint8_t*>(info));
}

/// Sends one chat record to everyone in the form each machine reads: once
/// when all read the same form, else one copy per machine.
///
/// @param lobby Lobby state.
/// @param from Sending player id.
/// @param utf8 The record in UTF-8.
/// @param code_page The record in the code page.
void send_chat_forms(
    Lobby& lobby,
    uint32_t from,
    const netgame::ChatRecord& utf8,
    const netgame::ChatRecord& code_page
) noexcept {
    uint8_t wide[80];
    uint8_t narrow[80];
    std::size_t wide_size = 0;
    std::size_t narrow_size = 0;
    if (netgame::encode_record(utf8, wide, sizeof(wide), &wide_size) != netgame::WireError::ok ||
        netgame::encode_record(code_page, narrow, sizeof(narrow), &narrow_size) !=
            netgame::WireError::ok)
        return;
    const auto reads = [&](uint32_t id) {
        return slot_reads_unicode_chat(lobby, slot_for_player_id(lobby, id));
    };
    uint32_t ids[kSlotCount];
    int32_t count = 0;
    const bool shared = lobby.game->shared_machines != 0;
    if (shared) {
        count = machine_broadcast_targets(*lobby.game, ids);
    } else {
        for (int32_t slot = 0; slot < kSlotCount; ++slot)
            if (occupied_by(slot_player(lobby, slot), kSlotRemote))
                ids[count++] = slot_player(lobby, slot).player_id;
    }
    int32_t readers = 0;
    for (int32_t i = 0; i < count; ++i)
        readers += reads(ids[i]) ? 1 : 0;
    if (readers == 0 || readers == count) {
        if (readers == 0)
            send(lobby, from, kBroadcastId, narrow, narrow_size);
        else
            send(lobby, from, kBroadcastId, wide, wide_size);
        return;
    }
    for (int32_t i = 0; i < count; ++i) {
        if (reads(ids[i]))
            send(lobby, from, ids[i], wide, wide_size);
        else
            send(lobby, from, ids[i], narrow, narrow_size);
    }
    // A refused joiner holds no slot, so no machine's target names it.
    if (shared && lobby.refused_joiner != 0)
        send(lobby, from, lobby.refused_joiner, narrow, narrow_size);
}

/// Sends a chat line to everyone while Unicode chat is on (Lobby::unicode_chat):
/// read as UTF-8, cut into records, each in the form each machine reads.
///
/// @param lobby Lobby state.
/// @param from Sending player id.
/// @param text The line, as this machine holds game text.
/// @param record_bytes The bytes one record keeps of the line.
/// @param split A line that is no command goes as up to netgame::chat_line_parts records.
/// @return The parts sent, in UTF-8.
std::vector<std::string> say_unicode(
    Lobby& lobby, uint32_t from, const char* text, std::size_t record_bytes, bool split
) noexcept {
    const auto line = netgame::chat_utf8(std::string_view(text, ::strnlen(text, kChatReadBytes)));
    std::vector<std::string> parts;
    if (split && !netgame::chat_command(line))
        parts = netgame::chat_parts(line, record_bytes, netgame::chat_line_parts);
    else
        parts.push_back(line.substr(0, base::text::whole_characters(line, record_bytes)));
    for (const auto& part : parts) {
        netgame::ChatRecord utf8{};
        std::memcpy(utf8.text, part.data(), std::min(part.size(), sizeof(utf8.text)));
        const auto narrow = netgame::chat_code_page(part);
        netgame::ChatRecord code_page{};
        std::memcpy(code_page.text, narrow.data(), std::min(narrow.size(), sizeof(code_page.text)));
        send_chat_forms(lobby, from, utf8, code_page);
    }
    return parts;
}

/// Posts a chat line a player said, or another machine sent, as this machine shows it.
///
/// The alliance lines and "does not have this map" go between machines in
/// English and show in the language shown
/// (oa::sim::messages::format_shown_chat_line), a deliberate difference
/// from 3.1c; any other line is posted as it came.
///
/// @param[in,out] lobby Lobby state.
/// @param line The chat line, "<Name> text".
void post_shown_chat(Lobby& lobby, const char* line) noexcept {
    char shown[sim::messages::most_chat_line_bytes];
    sim::messages::format_shown_chat_line(
        shown,
        sizeof shown,
        std::string_view(line, ::strnlen(line, sizeof shown)),
        lobby.services.translate,
        lobby.services.context
    );
    lobby_post_chat(lobby, shown);
}

/// Sends a plain chat line from the local player to everyone and shows it here.
///
/// @param lobby Lobby state.
/// @param line The line; cut at 63 bytes, between whole characters.
void recorder_say(Lobby& lobby, const char* line) noexcept {
    const auto from = local_player(lobby).player_id;
    if (lobby.unicode_chat) {
        (void)say_unicode(lobby, from, line, kChatLineChars, false);
    } else {
        netgame::ChatRecord record{};
        std::memcpy(record.text, line, chat_bytes_within(line, kChatLineChars));
        uint8_t wire[80];
        std::size_t written = 0;
        if (netgame::encode_record(record, wire, sizeof(wire), &written) == netgame::WireError::ok)
            send(lobby, from, kBroadcastId, wire, written);
    }
    flush(lobby);
    lobby_post_chat(lobby, line);
}

/// Tells whether the profile makes the recorder's commander warp available (setup.commander-warp).
///
/// @param lobby Lobby state.
/// @return True when the rule is on with its available parameter.
bool commander_warp_available(const Lobby& lobby) noexcept {
    const auto& warp = setup_rules(lobby).commander_warp;
    return warp.enabled && warp.available;
}

/// Tells whether the profile makes the recorder's prebuilt bases available (setup.recorder-prebuilt-base).
///
/// @param lobby Lobby state.
/// @return True when the rule is on with its available parameter.
bool prebuilt_base_available(const Lobby& lobby) noexcept {
    const auto& base = setup_rules(lobby).recorder_prebuilt_base;
    return base.enabled && base.available;
}

/// The most bytes of a base file .base reads.
constexpr std::size_t kBaseFileLimit = 64 * 1024;

/// Carries out the host's .base: loads the standard base, or the base file
/// the command names, and offers a base to every player seated.
///
/// @param lobby Lobby state; this machine hosts.
/// @param line The command.
/// @param[out] answer The line the host's recorder answers with.
/// @param capacity Its size.
void recorder_offer_base(
    Lobby& lobby, const netgame::RecorderCommandLine& line, char* answer, std::size_t capacity
) noexcept {
    auto& base = lobby.recorder.base;
    if (line.argument[0] == '\0') {
        netgame::recorder_standard_base(base);
        std::snprintf(answer, capacity, "Standard base initiated .baseoff to disable");
    } else {
        std::string text;
        if (lobby.services.read_file == nullptr ||
            !lobby.services.read_file(
                lobby.services.context, line.argument, kBaseFileLimit, &text
            )) {
            std::snprintf(answer, capacity, "Unable to open file %s", line.argument);
            return;
        }
        const auto outcome = netgame::recorder_read_base(text, base);
        if (outcome != netgame::RecorderBaseRead::read) {
            std::snprintf(answer, capacity, "%s", netgame::recorder_base_read_text(outcome));
            return;
        }
        // A long file name cuts the answer short at the chat line's end.
        const std::string reply =
            std::string("Fast base initiated from ") + line.argument + " .baseoff to disable";
        oa::base::text::copy_terminated({answer, capacity}, reply);
    }
    for (int32_t slot = 0; slot < kSlotCount; ++slot)
        base.available[slot] = slot_player(lobby, slot).in_use != 0;
}

/// Picks a map at random from the list and makes it the game's, as the host's map dialog does.
///
/// @param lobby Lobby state; this machine hosts.
void recorder_random_map(Lobby& lobby) noexcept {
    auto& maps = lobby.maps;
    if (maps.count == nullptr || maps.at == nullptr || maps.select == nullptr)
        return;
    const auto count = maps.count(maps.context);
    if (count <= 0)
        return;
    const auto pick = static_cast<int32_t>(milliseconds(lobby) % static_cast<uint32_t>(count));
    const char* name = maps.at(maps.context, pick);
    if (name == nullptr || !maps.select(maps.context, name))
        return;
    auto& mine = local_info(lobby);
    std::snprintf(mine.map_name, sizeof(mine.map_name), "%s", name);
    mine.map_hash = maps.content_hash != nullptr ? maps.content_hash(maps.context) : 0;
    lobby_send_player_info(lobby);
    lobby_publish_session(lobby);
}

// ---------------------------------------------------------------------------
// Presence records

/// Most bytes of a map pack's descriptor URL, name and version in its field.
constexpr std::size_t kPackUrlMostBytes = 255;
constexpr std::size_t kPackNameMostBytes = 64;
constexpr std::size_t kPackVersionMostBytes = 32;

/// Forgets a slot's presence entries: the record its player sent, whose
/// block it holds, and what was sent to it.
///
/// @param[in,out] records the battle room's presence records
/// @param slot the slot, 0..9
void presence_forget(PresenceRecords& records, int32_t slot) noexcept {
    records.peer_size[slot] = 0;
    records.peer_id[slot] = 0;
    records.block_player[slot] = 0;
    records.sent_generation[slot] = 0;
    records.sent_to[slot] = 0;
}

/// Returns the sim hash of 3.1c's own rules: the plain baseline's, the
/// profile a game without a mod resolves to.
///
/// It is resolved the first time it is asked for and kept for the run.
///
/// @return The hash; nothing when the baseline does not resolve.
const std::optional<netgame::PresenceDigest>& plain_baseline_sim_hash() noexcept {
    static const std::optional<netgame::PresenceDigest> hash =
        []() noexcept -> std::optional<netgame::PresenceDigest> {
        namespace profiles = oa::data::mod_profile;
        try {
            const std::string text = profiles::base_game_profile_text();
            const auto resolved = profiles::resolve_profile(
                std::span<const uint8_t>(
                    reinterpret_cast<const uint8_t*>(text.data()), text.size()
                ),
                profiles::base_game_id
            );
            if (resolved.resolution)
                return resolved.resolution->profile.sim_hash;
        } catch (const std::exception&) {
        }
        return std::nullopt;
    }();
    return hash;
}

/// Writes the value of a map pack's field into the presence records' answer.
///
/// A descriptor URL, name or version longer than the field allows is sent
/// empty. When the pack still breaks the field's rules its name, version
/// and URL are all sent empty; when it breaks them even so, it is left out.
///
/// @param[in,out] records the battle room's presence records
/// @param pack the pack LobbyMaps::pack named
void presence_take_pack(PresenceRecords& records, netgame::PresenceMapPack pack) noexcept {
    records.pack_size = 0;
    if (pack.descriptor_url.size() > kPackUrlMostBytes)
        pack.descriptor_url.clear();
    if (pack.name.size() > kPackNameMostBytes)
        pack.name.clear();
    if (pack.version.size() > kPackVersionMostBytes)
        pack.version.clear();
    std::size_t written = 0;
    if (netgame::encode_presence_map_pack(pack, records.pack, sizeof records.pack, &written) !=
        netgame::WireError::ok) {
        pack.descriptor_url.clear();
        pack.name.clear();
        pack.version.clear();
        if (netgame::encode_presence_map_pack(pack, records.pack, sizeof records.pack, &written) !=
            netgame::WireError::ok)
            return;
    }
    records.pack_size = static_cast<uint16_t>(written);
}

/// Asks LobbyMaps::pack for the selected map's pack, on the host, when the
/// map's name differs from the one last asked about or a binding has come
/// since; the answer is kept between.
///
/// @param[in,out] lobby Lobby state; this machine hosts.
void presence_ask_pack(Lobby& lobby) noexcept {
    auto& records = lobby.presence_records;
    // Unbound, nothing is asked: the first binding asks about the map.
    if (lobby.maps.pack == nullptr) {
        records.pack_map[0] = '\0';
        records.pack_size = 0;
        return;
    }
    // Cut to the size kept, so that a longer name is not asked about again.
    const auto name = text_view(map_name(lobby), sizeof records.pack_map - 1);
    if (!records.pack_rebound && name == text_view(records.pack_map, sizeof records.pack_map))
        return;
    records.pack_rebound = false;
    copy_text(records.pack_map, sizeof records.pack_map, name);
    records.pack_size = 0;
    if (name.empty())
        return;
    netgame::PresenceMapPack pack;
    if (lobby.maps.pack(lobby.maps.pack_context, &pack))
        presence_take_pack(records, std::move(pack));
}

/// Difficulty a computer player shows, matching the game's easy, medium and hard.
constexpr uint8_t kDifficultyEasy = 0;
constexpr uint8_t kDifficultyMedium = 1;
constexpr uint8_t kDifficultyHard = 2;
constexpr uint8_t kDifficultyCount = 3;
/// Kinds of a host's request to change another machine's computer player.
constexpr uint8_t kComputerEditSide = 1;
constexpr uint8_t kComputerEditColor = 2;
constexpr uint8_t kComputerEditTeam = 3;
constexpr uint8_t kComputerEditDifficulty = 4;

/// Stores a little-endian 32-bit value.
///
/// @param[out] out four bytes
/// @param value the value
void store_u32_le(uint8_t* out, uint32_t value) noexcept {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

/// Reads a little-endian 32-bit value.
///
/// @param in four bytes
/// @return the value
uint32_t load_u32_le(const uint8_t* in) noexcept {
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8) |
           (static_cast<uint32_t>(in[2]) << 16) | (static_cast<uint32_t>(in[3]) << 24);
}

/// Returns the English word a difficulty shows.
///
/// @param difficulty 0 easy, 1 medium, anything else hard
/// @return the word
const char* difficulty_word(uint8_t difficulty) noexcept {
    switch (difficulty) {
    case kDifficultyEasy:
        return "Easy";
    case kDifficultyMedium:
        return "Medium";
    default:
        return "Hard";
    }
}

/// Returns the difficulty a slot shows. Unset, or out of range, shows hard.
///
/// @param lobby Lobby state.
/// @param slot Player slot.
/// @return 0, 1 or 2.
uint8_t shown_difficulty(const Lobby& lobby, int32_t slot) noexcept {
    if (slot < 0 || slot >= kSlotCount || !lobby.computer_difficulty_set[slot])
        return kDifficultyHard;
    const auto noted = lobby.computer_difficulty[slot];
    return noted < kDifficultyCount ? noted : kDifficultyHard;
}

/// Notes a computer player's difficulty.
///
/// @param[in,out] lobby Lobby state.
/// @param slot Player slot.
/// @param difficulty 0 easy, 1 medium or 2 hard; anything else is ignored.
void note_computer_difficulty(Lobby& lobby, int32_t slot, uint8_t difficulty) noexcept {
    if (slot < 0 || slot >= kSlotCount || difficulty >= kDifficultyCount)
        return;
    lobby.computer_difficulty[slot] = difficulty;
    lobby.computer_difficulty_set[slot] = true;
}

/// Forgets a slot's difficulty and a host command that names its player.
///
/// @param[in,out] lobby Lobby state.
/// @param slot Player slot.
void clear_computer_difficulty(Lobby& lobby, int32_t slot) noexcept {
    if (slot < 0 || slot >= kSlotCount)
        return;
    const auto id = slot_player(lobby, slot).player_id;
    lobby.computer_difficulty[slot] = 0;
    lobby.computer_difficulty_set[slot] = false;
    lobby.computer_brain[slot].reset();
    lobby.computer_launch_brain[static_cast<std::size_t>(slot)].clear();
    if (lobby.computer_command_kind != 0 && lobby.computer_command_player == id)
        lobby.computer_command_kind = 0;
}

/// Tells whether a remote slot is a computer player before the start.
///
/// After the start a defeated human uses the same state, so it is not one.
///
/// @param lobby Lobby state.
/// @param slot Player slot.
/// @return True for a seated remote computer player.
bool remote_computer(Lobby& lobby, int32_t slot) noexcept {
    const auto& player = slot_player(lobby, slot);
    if (!occupied_by(player, kSlotRemote) || (lobby.game->session_flags & kNetFlagGameStarted) != 0)
        return false;
    const auto* info = slot_info(lobby, slot);
    return info != nullptr && info->state == kInfoStateDefeated;
}

/// Tells whether a row is a computer player.
///
/// @param lobby Lobby state.
/// @param slot Player slot.
/// @return True for this machine's computer player, or a remote one before the start.
bool computer_row(Lobby& lobby, int32_t slot) noexcept {
    if (slot < 0 || slot >= kSlotCount)
        return false;
    return occupied_by(slot_player(lobby, slot), kSlotComputer) || remote_computer(lobby, slot);
}

/// Returns the human who runs a computer player.
///
/// This machine's computer player is the local human's. A remote one shares
/// a machine group with a human still playing. A group of 0 names no one.
///
/// @param lobby Lobby state.
/// @param slot The computer player's slot.
/// @return The human's slot, or -1.
int32_t computer_owner_slot(Lobby& lobby, int32_t slot) noexcept {
    const auto& player = slot_player(lobby, slot);
    if (occupied_by(player, kSlotComputer))
        return lobby.game->local_player_index;
    if (!remote_computer(lobby, slot) || player.machine_group == 0)
        return -1;
    for (int32_t other = 0; other < kSlotCount; ++other) {
        if (other == slot)
            continue;
        const auto& mate = slot_player(lobby, other);
        if (mate.machine_group != player.machine_group)
            continue;
        if (!occupied_by(mate, kSlotRemote) && !occupied_by(mate, kSlotLocal))
            continue;
        const auto* info = slot_info(lobby, other);
        if (info != nullptr && info->state == kInfoStatePlaying)
            return other;
    }
    return -1;
}

/// Tells whether the human who runs a computer player is on OA.
///
/// The local human is. A remote human is when its block carries the signature.
///
/// @param lobby Lobby state.
/// @param owner The human's slot; negative is no.
/// @return True when that machine runs OA.
bool owner_runs_oa(Lobby& lobby, int32_t owner) noexcept {
    if (owner < 0 || owner >= kSlotCount)
        return false;
    if (occupied_by(slot_player(lobby, owner), kSlotLocal))
        return true;
    const auto* info = slot_info(lobby, owner);
    return info != nullptr &&
           netgame::sent_by_open_annihilation(reinterpret_cast<const uint8_t*>(info));
}

/// Tells whether the local host may change a remote computer player.
///
/// A computer player whose human is on 3.1c can be removed, not restyled.
///
/// @param lobby Lobby state.
/// @param slot The computer player's slot.
/// @return True when this machine hosts and the owner's machine runs OA.
bool host_edits_remote_computer(Lobby& lobby, int32_t slot) noexcept {
    if (slot < 0 || slot >= kSlotCount || lobby.computer_brain[slot])
        return false;
    if (!host_is_local(lobby) || !remote_computer(lobby, slot))
        return false;
    return owner_runs_oa(lobby, computer_owner_slot(lobby, slot));
}

/// Tells whether the viewer may change a computer player's row.
///
/// The local human may change this machine's. The host may change a remote
/// one whose human runs OA. Neither may while the launch is locked or the
/// local player is ready.
///
/// @param lobby Lobby state.
/// @param slot The computer player's slot.
/// @return True when a click on the row should change it.
bool edits_computer(Lobby& lobby, int32_t slot) noexcept {
    if (slot < 0 || slot >= kSlotCount || lobby.computer_brain[slot])
        return false;
    if (lobby_launch_locked(lobby))
        return false;
    if ((info_of(lobby, local_player(lobby)).options & option::ready) != 0)
        return false;
    if (occupied_by(slot_player(lobby, slot), kSlotComputer))
        return true;
    return host_edits_remote_computer(lobby, slot);
}

/// Asks the owner of a remote computer player to change it.
///
/// The command stays until that player's block, team or difficulty list
/// echoes the same kind and value. A newer command replaces an older one.
///
/// @param[in,out] lobby Lobby state.
/// @param player_id The computer player's session id.
/// @param kind Side, colour, team or difficulty.
/// @param value The new value.
void queue_computer_edit(Lobby& lobby, uint32_t player_id, uint8_t kind, uint8_t value) noexcept {
    lobby.computer_command_player = player_id;
    lobby.computer_command_kind = kind;
    lobby.computer_command_value = value;
}

/// Drops a host command once the owner's echo matches it.
///
/// An older echo does not retire a newer command.
///
/// @param[in,out] lobby Lobby state.
/// @param player_id The computer player's session id.
/// @param kind The kind the echo answers.
/// @param value The value the echo carries.
void note_computer_command_echo(
    Lobby& lobby, uint32_t player_id, uint8_t kind, uint8_t value
) noexcept {
    if (lobby.computer_command_kind == kind && lobby.computer_command_player == player_id &&
        lobby.computer_command_value == value)
        lobby.computer_command_kind = 0;
}

/// Applies a host's change to a computer player this machine runs.
///
/// Side and colour go out with the player's block, team with its team record.
/// Difficulty is published with this machine's presence record.
///
/// @param[in,out] lobby Lobby state.
/// @param player_id The computer player's session id.
/// @param kind Side, colour, team or difficulty.
/// @param value The new value.
void apply_host_computer_edit(
    Lobby& lobby, uint32_t player_id, uint8_t kind, uint8_t value
) noexcept {
    const auto slot = slot_for_player_id(lobby, player_id);
    if (slot < 0)
        return;
    auto& player = slot_player(lobby, slot);
    if (!occupied_by(player, kSlotComputer))
        return;
    auto* info = slot_info(lobby, slot);
    if (info == nullptr)
        return;
    switch (kind) {
    case kComputerEditSide:
        info->side = value;
        lobby_send_player_info(lobby);
        break;
    case kComputerEditColor:
        info->color = value;
        lobby_send_player_info(lobby);
        break;
    case kComputerEditTeam:
        lobby_player_team(player) = value;
        lobby_send_team(lobby, player);
        lobby_update_ally_matrix(lobby);
        break;
    case kComputerEditDifficulty:
        note_computer_difficulty(lobby, slot, value);
        break;
    default:
        break;
    }
}

/// Reads computer players and a host command from a presence record just kept.
///
/// @param[in,out] lobby Lobby state.
/// @param sender_slot Slot of the player who sent the record.
void take_computer_presence(Lobby& lobby, int32_t sender_slot) noexcept {
    const auto& records = lobby.presence_records;
    if (sender_slot < 0 || sender_slot >= kSlotCount || records.peer_size[sender_slot] == 0)
        return;
    const auto* bytes = records.peer[sender_slot];
    const auto size = records.peer_size[sender_slot];
    std::span<const uint8_t> listed;
    if (netgame::find_presence_field(bytes, size, netgame::PresenceTag::computers, &listed)) {
        for (std::size_t at = 0; at + 5 <= listed.size(); at += 5) {
            const auto id = load_u32_le(listed.data() + at);
            const auto difficulty = listed[at + 4];
            const auto seated = slot_for_player_id(lobby, id);
            if (seated >= 0)
                note_computer_difficulty(lobby, seated, difficulty);
            note_computer_command_echo(lobby, id, kComputerEditDifficulty, difficulty);
        }
    }
    std::span<const uint8_t> brains_value;
    const bool brains_found = netgame::find_presence_field(
        bytes, size, netgame::PresenceTag::computer_brains, &brains_value
    );
    const auto decoded =
        brains_found ? netgame::decode_presence_brains(brains_value) : std::nullopt;
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        if (computer_owner_slot(lobby, slot) != sender_slot)
            continue;
        const auto id = slot_player(lobby, slot).player_id;
        const netgame::PresenceBrain* match = nullptr;
        if (decoded) {
            for (const auto& item : *decoded) {
                if (item.player == id)
                    match = &item;
            }
        }
        if (match == nullptr) {
            lobby.computer_brain[slot].reset();
            continue;
        }
        ComputerBrain brain;
        brain.id = match->id;
        brain.name = match->name;
        brain.version = match->version;
        brain.level_id = match->level_id;
        brain.level = match->level;
        brain.full_awareness = match->full_awareness;
        lobby.computer_brain[slot] = std::move(brain);
    }
    const auto host = lobby_host_slot(lobby);
    if (host_is_local(lobby) || host == kNoSlot || sender_slot != static_cast<int32_t>(host))
        return;
    std::span<const uint8_t> command;
    if (!netgame::find_presence_field(bytes, size, netgame::PresenceTag::computer_edit, &command) ||
        command.size() < 6)
        return;
    apply_host_computer_edit(lobby, load_u32_le(command.data()), command[4], command[5]);
}

/// Appends this machine's computer players, then a host command, to a presence record.
///
/// A computer player is listed only once its difficulty has been chosen and
/// the session has seated it, so a room that never chose one sends nothing more.
///
/// @param lobby Lobby state.
/// @param[in,out] record Presence record being composed.
/// @param[in,out] size Its length so far.
/// @param capacity Bytes available at record.
void append_computer_presence(
    Lobby& lobby, uint8_t* record, std::size_t& size, std::size_t capacity
) noexcept {
    uint8_t listed[kSlotCount * 5];
    std::size_t count = 0;
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        const auto& player = slot_player(lobby, slot);
        if (!occupied_by(player, kSlotComputer) || !lobby.computer_difficulty_set[slot])
            continue;
        store_u32_le(listed + count, player.player_id);
        listed[count + 4] = lobby.computer_difficulty[slot];
        count += 5;
    }
    if (count != 0) {
        std::size_t written = 0;
        if (netgame::append_presence_field(
                record,
                size,
                capacity,
                netgame::PresenceTag::computers,
                std::span<const uint8_t>(listed, count),
                &written
            ) == netgame::WireError::ok)
            size = written;
    }
    std::vector<netgame::PresenceBrain> brains;
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        const auto& player = slot_player(lobby, slot);
        if (!occupied_by(player, kSlotComputer) || !lobby.computer_brain[slot])
            continue;
        const auto& seated = *lobby.computer_brain[slot];
        netgame::PresenceBrain item;
        item.player = player.player_id;
        item.id = seated.id;
        item.name = seated.name;
        item.version = seated.version;
        item.level_id = seated.level_id;
        item.level = seated.level;
        item.full_awareness = seated.full_awareness;
        brains.push_back(std::move(item));
    }
    if (!brains.empty()) {
        const auto limit = capacity < netgame::presence_record_max_bytes
                               ? capacity
                               : netgame::presence_record_max_bytes;
        if (limit > size + netgame::presence_field_header_bytes) {
            const auto room = limit - size - netgame::presence_field_header_bytes;
            uint8_t value[netgame::presence_record_max_bytes];
            std::size_t brain_written = 0;
            std::size_t sent = 0;
            if (netgame::encode_presence_brains(brains, value, room, &brain_written, &sent) ==
                    netgame::WireError::ok &&
                sent != 0) {
                std::size_t total = 0;
                if (netgame::append_presence_field(
                        record,
                        size,
                        capacity,
                        netgame::PresenceTag::computer_brains,
                        std::span<const uint8_t>(value, brain_written),
                        &total
                    ) == netgame::WireError::ok)
                    size = total;
            }
        }
    }
    if (lobby.computer_command_kind == 0)
        return;
    uint8_t body[6];
    store_u32_le(body, lobby.computer_command_player);
    body[4] = lobby.computer_command_kind;
    body[5] = lobby.computer_command_value;
    std::size_t written = 0;
    if (netgame::append_presence_field(
            record,
            size,
            capacity,
            netgame::PresenceTag::computer_edit,
            std::span<const uint8_t>(body, sizeof body),
            &written
        ) == netgame::WireError::ok)
        size = written;
}

/// Returns the next free colour after a computer player's current one.
///
/// @param lobby Lobby state.
/// @param player The computer player.
/// @param color Its current colour.
/// @return The next free colour, or the current one when none is free.
uint8_t next_computer_color(Lobby& lobby, const Player& player, uint8_t color) noexcept {
    auto next = static_cast<uint32_t>(color == kNoColor ? 0 : color + 1U);
    for (int32_t tries = 0; tries < kColorCount; ++tries) {
        if (next >= static_cast<uint32_t>(kColorCount))
            next = 0;
        if (lobby_color_available(lobby, player.player_id, static_cast<int32_t>(next)))
            return static_cast<uint8_t>(next);
        ++next;
    }
    return color;
}

/// Folds the stored host record into the guest's assembly once per change.
///
/// A record that arrives before this machine knows the host slot is folded
/// when the slot becomes known. The same bytes are not folded twice.
///
/// @param[in,out] lobby Lobby state.
void feed_host_assembly(Lobby& lobby) noexcept {
    if (host_is_local(lobby) || lobby.game == nullptr)
        return;
    const auto host = lobby_host_slot(lobby);
    if (host == kNoSlot)
        return;
    auto& records = lobby.presence_records;
    const auto slot = static_cast<int32_t>(host);
    if (records.peer_size[slot] == 0)
        return;
    uint64_t hash = 14695981039346656037ull;
    for (uint16_t index = 0; index < records.peer_size[slot]; ++index) {
        hash ^= records.peer[slot][index];
        hash *= 1099511628211ull;
    }
    if (records.host_fed_size == records.peer_size[slot] && records.host_fed_hash == hash)
        return;
    records.host_fed_size = records.peer_size[slot];
    records.host_fed_hash = hash;
    try {
        netgame::PresenceRecord decoded;
        if (netgame::decode_presence(records.peer[slot], records.peer_size[slot], &decoded) !=
            netgame::WireError::ok)
            return;
        if (netgame::presence_assembly_take(records.host_loadout, decoded))
            ++records.host_loadout_generation;
    } catch (const std::exception&) {
    }
}

/// Keeps a presence record a seated remote player sent as that player's.
///
/// @param[in,out] lobby Lobby state.
/// @param event the record's event; its first byte is the presence record's type
/// @return true when it was kept; false for an unseated sender, one of this
///         machine's players, or a length word other than the event's size
bool presence_keep(Lobby& lobby, const LobbyEvent& event) noexcept {
    auto& records = lobby.presence_records;
    const auto slot = slot_for_player_id(lobby, event.player_id);
    uint16_t length = 0;
    if (slot < 0 || !occupied_by(slot_player(lobby, slot), kSlotRemote) ||
        netgame::presence_record_length(event.data, event.size, &length) !=
            netgame::WireError::ok ||
        length != event.size || event.size > sizeof records.peer[slot])
        return false;
    std::memcpy(records.peer[slot], event.data, event.size);
    records.peer_size[slot] = event.size;
    records.peer_id[slot] = event.player_id;
    take_computer_presence(lobby, slot);
    // The host's record carries the room's list. A guest keeps it as far as
    // the records have brought it, including a part that continues the head.
    feed_host_assembly(lobby);
    lobby_note_classic_room(lobby);
    // An OA joiner is judged when its record arrives, not on its block. A
    // build that sends no record is never refused. A later record from a
    // player already named does not post the line again.
    if (host_is_local(lobby) && lobby_room_oa_only(lobby) && lobby_limits_remote_oa(lobby, slot) &&
        !lobby_limits_covered(lobby, slot)) {
        if (!lobby.limits_older_refused[slot]) {
            lobby.limits_older_refused[slot] = true;
            lobby_post_limits_line(lobby, lobby_limits_newer_line(), slot);
        }
        lobby_reject(lobby, event.player_id, kRejectVersion);
        flush(lobby);
    }
    return true;
}

} // namespace

std::span<const uint8_t> lobby_presence_record(Lobby& lobby, int32_t slot) noexcept {
    if (slot < 0 || slot >= kSlotCount)
        return {};
    const auto& player = slot_player(lobby, slot);
    const auto& records = lobby.presence_records;
    if (local_or_computer(player))
        return {records.local, records.local_size};
    if (!occupied_by(player, kSlotRemote) || records.peer_size[slot] == 0 ||
        records.peer_id[slot] != player.player_id)
        return {};
    return {records.peer[slot], records.peer_size[slot]};
}

namespace {

/// Returns what a row shows in its CD<slot> rectangle, the dot left aside (lobby_row_badge).
///
/// @param lobby Lobby state.
/// @param slot Player slot; outside 0..9 shows nothing.
/// @return The badge.
RowBadge row_badge(Lobby& lobby, int32_t slot) noexcept {
    if (lobby.game == nullptr || slot < 0 || slot >= kSlotCount)
        return RowBadge::none;
    const auto& player = slot_player(lobby, slot);
    const auto& info = info_of(lobby, player);
    const bool watcher = player.in_use != 0 && (info.options & option::watcher) != 0;
    if (!slot_active(player) && !watcher)
        return RowBadge::none;
    const bool present = occupied_by(player, kSlotLocal) || slot_remote_playing(lobby, player);
    if (!present)
        return RowBadge::none;
    if (occupied_by(player, kSlotLocal) ||
        netgame::sent_by_open_annihilation(reinterpret_cast<const uint8_t*>(&info)))
        return RowBadge::open_annihilation;
    return (info.status & status::has_disc) != 0 ? RowBadge::disc : RowBadge::none;
}

} // namespace

RowBadgeState lobby_row_badge(Lobby& lobby, int32_t slot) noexcept {
    return {row_badge(lobby, slot), lobby_rules_differ_from_host(lobby, slot)};
}

bool lobby_sim_hash_differs_from_host(Lobby& lobby, int32_t slot) noexcept {
    if (row_badge(lobby, slot) != RowBadge::open_annihilation)
        return false;
    const auto host = lobby_host_slot(lobby);
    if (host == kNoSlot || host == slot)
        return false;
    const auto row_record = lobby_presence_record(lobby, slot);
    const auto host_record = lobby_presence_record(lobby, host);
    const auto row_hash = netgame::presence_sim_hash(row_record.data(), row_record.size());
    const auto host_hash = netgame::presence_sim_hash(host_record.data(), host_record.size());
    if (row_hash && host_hash)
        return *row_hash != *host_hash;
    // A 3.1c host sends no record and plays 3.1c's own rules. An OA host
    // without a record (an OA 0.7 machine) tells nothing.
    const auto& host_player = slot_player(lobby, host);
    if (local_or_computer(host_player) ||
        netgame::sent_by_open_annihilation(
            reinterpret_cast<const uint8_t*>(&info_of(lobby, host_player))
        ))
        return false;
    const auto& plain = lobby.presence_records.plain_sim_hash;
    return row_hash && plain && *row_hash != *plain;
}

bool lobby_rules_differ_from_host(Lobby& lobby, int32_t slot) noexcept {
    if (lobby_sim_hash_differs_from_host(lobby, slot))
        return true;
    if (row_badge(lobby, slot) != RowBadge::open_annihilation)
        return false;
    const auto host = lobby_host_slot(lobby);
    if (host == kNoSlot || host == slot)
        return false;
    if (netgame::presence_content_match(
            lobby_presence_record(lobby, slot), lobby_presence_record(lobby, host)
        ) == netgame::PresenceContentMatch::differs)
        return true;
    return lobby_mutators_differ_from_host(lobby, slot);
}

bool lobby_content_allows_start(Lobby& lobby) noexcept {
    if (!host_is_local(lobby))
        return true;
    const auto host = lobby_host_slot(lobby);
    if (host == kNoSlot)
        return true;
    const auto host_record = lobby_presence_record(lobby, host);
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        if (slot == host)
            continue;
        if (row_badge(lobby, slot) != RowBadge::open_annihilation)
            continue;
        const auto& player = slot_player(lobby, slot);
        // This machine's players share its record, so they match themselves.
        if (local_or_computer(player))
            continue;
        if (netgame::presence_content_match(lobby_presence_record(lobby, slot), host_record) ==
            netgame::PresenceContentMatch::differs)
            return false;
    }
    return true;
}

namespace {

/// A seat that plays: in use, local, a computer, or a remote player still
/// playing, and not watching. A remote seat counts only once this machine
/// has that player's own block.
bool playing_seat(Lobby& lobby, int32_t player_slot) noexcept {
    if (lobby.game == nullptr || player_slot < 0 || player_slot >= kSlotCount)
        return false;
    const auto& player = slot_player(lobby, player_slot);
    if (player.in_use == 0)
        return false;
    const auto* info = player_info(lobby, player);
    if (info == nullptr || (info->options & option::watcher) != 0)
        return false;
    if (local_or_computer(player))
        return true;
    if (player.status != kSlotRemote)
        return false;
    if (lobby.presence_records.block_player[player_slot] != player.player_id)
        return false;
    return slot_remote_playing(lobby, player);
}

} // namespace

int32_t lobby_first_side_past_table(Lobby& lobby) noexcept {
    if (lobby.game == nullptr)
        return -1;
    const auto side_count = lobby_side_count(*lobby.game);
    if (side_count == 0)
        return -1;
    for (int32_t player_slot = 0; player_slot < kSlotCount; ++player_slot) {
        if (!playing_seat(lobby, player_slot))
            continue;
        const auto* info = player_info(lobby, slot_player(lobby, player_slot));
        if (info != nullptr && info->side >= side_count)
            return player_slot;
    }
    return -1;
}

bool lobby_sides_allow_start(Lobby& lobby) noexcept {
    if (lobby.game == nullptr || !host_is_local(lobby))
        return true;
    return lobby_first_side_past_table(lobby) < 0;
}

void lobby_note_sides_past_table(Lobby& lobby) noexcept {
    if (lobby.game == nullptr || !host_is_local(lobby))
        return;
    const auto side_count = lobby_side_count(*lobby.game);
    for (int32_t player_slot = 0; player_slot < kSlotCount; ++player_slot) {
        const auto* info = player_info(lobby, slot_player(lobby, player_slot));
        const bool past = side_count != 0 && playing_seat(lobby, player_slot) && info != nullptr &&
                          info->side >= side_count;
        if (!past) {
            lobby.side_past_table_posted[player_slot] = -1;
            continue;
        }
        if (lobby.side_past_table_posted[player_slot] == static_cast<int16_t>(info->side))
            continue;
        lobby_post_classic_line(
            lobby, "*** {player} chose a side this game does not have.", player_slot
        );
        lobby.side_past_table_posted[player_slot] = static_cast<int16_t>(info->side);
    }
}

namespace {

/// Tells whether a slot is an Open Annihilation player of another machine.
///
/// @param lobby Lobby state.
/// @param player_slot Player slot.
/// @return True when that player is remote, playing and carries the signature.
bool remote_oa_player(Lobby& lobby, int32_t player_slot) noexcept {
    return lobby_remote_oa_player(lobby, player_slot);
}

/// Appends one mutator part when at least one mutator fits.
///
/// @param[in,out] record Presence record being composed.
/// @param[in,out] size Its length so far.
/// @param capacity Bytes available at record.
/// @param loadout The full list the part is drawn from.
/// @param first The position of the first mutator.
/// @return True when a part was appended.
bool append_mutator_part(
    uint8_t* record,
    std::size_t& size,
    std::size_t capacity,
    const netgame::PresenceMutators& loadout,
    uint8_t first
) noexcept {
    if (size >= capacity || capacity - size <= netgame::presence_field_header_bytes)
        return false;
    const auto value_room = capacity - size - netgame::presence_field_header_bytes;
    uint8_t value[netgame::presence_record_max_bytes];
    std::size_t written = 0;
    uint8_t sent = 0;
    if (netgame::encode_presence_mutator_part(loadout, first, value, value_room, &written, &sent) !=
        netgame::WireError::ok)
        return false;
    std::size_t grown = 0;
    if (netgame::append_presence_field(
            record,
            size,
            capacity,
            netgame::PresenceTag::mutator_part,
            std::span<const uint8_t>(value, written),
            &grown
        ) != netgame::WireError::ok)
        return false;
    size = grown;
    return true;
}

/// The smallest held of an Open Annihilation peer still taking this list.
///
/// @param lobby Lobby state.
/// @param host_state The host source's mutator field.
/// @param[out] first That held.
/// @return True when some peer names this digest and holds less than total.
bool smallest_held(
    Lobby& lobby, const netgame::PresenceMutatorsState& host_state, uint8_t* first
) noexcept {
    const auto host = lobby_host_slot(lobby);
    bool behind = false;
    uint8_t smallest = 0;
    for (int32_t player_slot = 0; player_slot < kSlotCount; ++player_slot) {
        if (player_slot == static_cast<int32_t>(host) || !remote_oa_player(lobby, player_slot))
            continue;
        const auto row = lobby_presence_record(lobby, player_slot);
        const auto row_state = netgame::presence_mutators_state(row.data(), row.size());
        // A peer with no mutator field is not part of this exchange.
        if (!row_state || row_state->digest != host_state.digest ||
            row_state->held >= host_state.total)
            continue;
        if (!behind || row_state->held < smallest)
            smallest = row_state->held;
        behind = true;
    }
    if (behind)
        *first = smallest;
    return behind;
}

/// Rewrites the source smaller until the map pack and one part both fit.
///
/// The bound source is left as it is. The composed record is replaced.
///
/// @param lobby Lobby state.
/// @param[out] composed The record being composed.
/// @param[out] size Its length.
/// @param loadout The full list the part is drawn from.
/// @param first The position of the first mutator.
/// @return True when a part was appended.
bool shrink_source_for_part(
    Lobby& lobby,
    uint8_t* composed,
    std::size_t& size,
    const netgame::PresenceMutators& loadout,
    uint8_t first
) {
    auto& records = lobby.presence_records;
    netgame::PresenceRecord decoded;
    if (netgame::decode_presence(records.source, records.source_size, &decoded) !=
        netgame::WireError::ok)
        return false;
    // The smallest value that carries the mutator at `first`, then one head
    // that leaves room for the pack and that part.
    uint8_t probe[netgame::presence_record_max_bytes];
    std::size_t fit = 0;
    std::size_t low = 1;
    std::size_t high = netgame::presence_record_max_bytes;
    while (low <= high) {
        const std::size_t mid = low + (high - low) / 2;
        std::size_t written = 0;
        uint8_t sent = 0;
        if (netgame::encode_presence_mutator_part(loadout, first, probe, mid, &written, &sent) ==
                netgame::WireError::ok &&
            sent >= 1) {
            fit = written;
            if (mid == 0)
                break;
            high = mid - 1;
        } else {
            low = mid + 1;
        }
    }
    if (fit == 0)
        return false;
    const std::size_t pack_field =
        records.pack_size == 0 ? 0 : netgame::presence_field_header_bytes + records.pack_size;
    const std::size_t part_field = netgame::presence_field_header_bytes + fit;
    if (pack_field >= netgame::presence_record_max_bytes ||
        part_field >= netgame::presence_record_max_bytes - pack_field)
        return false;
    const std::size_t head_cap = netgame::presence_record_max_bytes - pack_field - part_field;
    uint8_t shrunk[netgame::presence_record_max_bytes];
    std::size_t shrunk_size = 0;
    if (netgame::encode_presence(decoded, shrunk, head_cap, &shrunk_size) != netgame::WireError::ok)
        return false;
    if (records.pack_size != 0) {
        std::size_t packed = 0;
        if (netgame::append_presence_field(
                shrunk,
                shrunk_size,
                sizeof shrunk,
                netgame::PresenceTag::map_pack,
                {records.pack, records.pack_size},
                &packed
            ) != netgame::WireError::ok)
            return false;
        shrunk_size = packed;
    }
    if (!append_mutator_part(shrunk, shrunk_size, sizeof shrunk, loadout, first))
        return false;
    std::memcpy(composed, shrunk, shrunk_size);
    size = shrunk_size;
    return true;
}

/// Appends one part of the host's list, shrinking the head when the pack leaves no room.
///
/// @param[in,out] lobby Lobby state.
/// @param[in,out] composed The record being composed, source and pack already copied.
/// @param[in,out] size Its length.
void append_room_mutator_part(Lobby& lobby, uint8_t* composed, std::size_t& size) noexcept {
    auto& records = lobby.presence_records;
    try {
        const auto host_state =
            netgame::presence_mutators_state(records.source, records.source_size);
        if (!host_state || !records.part_source ||
            records.part_source->digest != host_state->digest ||
            records.part_source->total != host_state->total)
            return;
        uint8_t first = 0;
        if (!smallest_held(lobby, *host_state, &first))
            return;
        const auto& loadout = *records.part_source;
        if (append_mutator_part(
                composed, size, netgame::presence_record_max_bytes, loadout, first
            ) ||
            shrink_source_for_part(lobby, composed, size, loadout, first)) {
            records.part_refused_digest.reset();
            return;
        }
        if (records.part_refused_digest && *records.part_refused_digest == host_state->digest)
            return;
        std::cerr << "multiplayer: no room for the mutator list after the map pack\n";
        std::cerr.flush();
        records.part_refused_digest = host_state->digest;
    } catch (const std::exception&) {
    }
}

/// Tells whether a guest has not built the host's list.
///
/// The host is never waiting on itself. No mutator field on the host means
/// the room has no mutators. The guest waits when its own record has no
/// field, names another list, or is not built.
///
/// @param lobby Lobby state.
/// @return True when READY stays off.
bool guest_awaits_mutators(Lobby& lobby) noexcept {
    if (host_is_local(lobby) || lobby.game == nullptr)
        return false;
    const auto host = lobby_host_slot(lobby);
    if (host == kNoSlot)
        return false;
    const auto host_record = lobby_presence_record(lobby, static_cast<int32_t>(host));
    const auto host_state =
        netgame::presence_mutators_state(host_record.data(), host_record.size());
    if (!host_state)
        return false;
    const auto own = lobby_presence_record(lobby, lobby.game->local_player_index);
    const auto own_state = netgame::presence_mutators_state(own.data(), own.size());
    if (!own_state || own_state->digest != host_state->digest)
        return true;
    return !own_state->built;
}

} // namespace

void presence_refresh(Lobby& lobby) noexcept {
    feed_host_assembly(lobby);
    auto& records = lobby.presence_records;
    const bool host = host_is_local(lobby);
    if (host)
        presence_ask_pack(lobby);
    uint8_t composed[netgame::presence_record_max_bytes];
    std::size_t size = 0;
    if (records.source_size != 0 && records.source_size <= sizeof composed) {
        std::memcpy(composed, records.source, records.source_size);
        size = records.source_size;
        // Only the host's record names a map pack.
        std::size_t written = 0;
        if (host && records.pack_size != 0 &&
            netgame::append_presence_field(
                composed,
                size,
                sizeof composed,
                netgame::PresenceTag::map_pack,
                {records.pack, records.pack_size},
                &written
            ) == netgame::WireError::ok)
            size = written;
        if (host)
            append_room_mutator_part(lobby, composed, size);
        append_computer_presence(lobby, composed, size, sizeof composed);
    }
    if (size == records.local_size && std::memcmp(composed, records.local, size) == 0) {
        if (host)
            lobby_note_classic_room(lobby);
        return;
    }
    std::memcpy(records.local, composed, size);
    records.local_size = static_cast<uint16_t>(size);
    ++records.local_generation;
    if (host)
        lobby_note_classic_room(lobby);
}

void presence_send_due(Lobby& lobby, int32_t only_slot) noexcept {
    auto& records = lobby.presence_records;
    if (records.local_size == 0 || (lobby.game->session_flags & kNetFlagLive) == 0 ||
        only_slot >= kSlotCount)
        return;
    const int32_t first = only_slot < 0 ? 0 : only_slot;
    const int32_t end = only_slot < 0 ? kSlotCount : only_slot + 1;
    const auto from = local_player(lobby).player_id;
    for (int32_t slot = first; slot < end; ++slot) {
        const auto& player = slot_player(lobby, slot);
        const auto* info = slot_info(lobby, slot);
        // A human of another machine, whose own block has come and says OA.
        if (!occupied_by(player, kSlotRemote) || player.player_id == kBroadcastId ||
            info == nullptr || records.block_player[slot] != player.player_id ||
            info->state != kInfoStatePlaying ||
            !netgame::presence_peer(reinterpret_cast<const uint8_t*>(info)))
            continue;
        if (records.sent_generation[slot] == records.local_generation &&
            records.sent_to[slot] == player.player_id)
            continue;
        // Alone in its frame: a machine that does not read the record loses
        // nothing else with it.
        flush(lobby);
        send(lobby, from, player.player_id, records.local, records.local_size);
        flush(lobby);
        records.sent_generation[slot] = records.local_generation;
        records.sent_to[slot] = player.player_id;
    }
}

bool recorder_chat_line(Lobby& lobby, int32_t sender_slot, const char* text) noexcept {
    const auto& rules = lobby.wire_rules;
    if (rules.recorder_protocol == netgame::recorder_protocol_plain || sender_slot < 0 ||
        sender_slot >= kSlotCount)
        return false;
    const auto line = netgame::parse_recorder_command(text);
    if (line.command == netgame::RecorderCommand::none)
        return false;
    if (netgame::recorder_session_command(line.command) && !rules.recorder_session_commands)
        return false;
    if (netgame::recorder_speed_command(line.command) && !rules.speed_lock)
        return false;
    const bool from_host = lobby_host_slot(lobby) == sender_slot;
    const bool from_here = local_or_computer(slot_player(lobby, sender_slot));
    auto& session = lobby.recorder;
    char answer[kChatLineBytes];
    answer[0] = '\0';
    switch (line.command) {
    case netgame::RecorderCommand::report:
    case netgame::RecorderCommand::report_mod:
        if (session.program[0] != '\0') {
            const auto& me = local_player(lobby);
            std::snprintf(
                answer,
                sizeof answer,
                "*** %s uses %s",
                std::string(text_view(me.name, sizeof(me.name))).c_str(),
                session.program
            );
        }
        break;
    case netgame::RecorderCommand::vote_go:
    case netgame::RecorderCommand::force_go:
        session.watchers_ready = true;
        break;
    case netgame::RecorderCommand::record:
        // The local player's .record names the game's recording.
        if (from_here && line.argument[0] != '\0') {
            base::text::copy_terminated(
                session.record_name,
                std::string_view(
                    line.argument, chat_bytes_within(line.argument, sizeof session.record_name - 1)
                )
            );
            std::snprintf(answer, sizeof answer, "Recording to %s", session.record_name);
        }
        break;
    case netgame::RecorderCommand::random_map:
    case netgame::RecorderCommand::random_map_ex:
        if (from_host && from_here)
            recorder_random_map(lobby);
        break;
    case netgame::RecorderCommand::base_file:
        if (!prebuilt_base_available(lobby) || !from_host)
            return false;
        if (from_here)
            recorder_offer_base(lobby, line, answer, sizeof answer);
        break;
    case netgame::RecorderCommand::base_off:
        if (!prebuilt_base_available(lobby) || !from_host)
            return false;
        session.base.enabled = false;
        std::fill(std::begin(session.base.available), std::end(session.base.available), false);
        if (from_here)
            std::snprintf(answer, sizeof answer, "Quick base disabled");
        break;
    default:
        if (!netgame::recorder_command_host_only(line.command) || !from_host)
            return false;
        if (line.command == netgame::RecorderCommand::commander_warp &&
            !commander_warp_available(lobby))
            return false;
        if (!netgame::recorder_apply_host_command(session, line) || !from_here)
            return true;
        // The host's recorder says what its command did.
        switch (line.command) {
        case netgame::RecorderCommand::speed_lock: {
            std::snprintf(
                answer,
                sizeof answer,
                "Speed locked between %d and %d",
                static_cast<int8_t>(session.options.speed_low),
                static_cast<int8_t>(session.options.speed_high)
            );
            break;
        }
        case netgame::RecorderCommand::speed_unlock:
            std::snprintf(answer, sizeof answer, "Speed unlocked");
            break;
        case netgame::RecorderCommand::autopause:
            std::snprintf(
                answer, sizeof answer, "Autopause enabled - At the start only the host can unpause"
            );
            break;
        case netgame::RecorderCommand::commander_warp:
            std::snprintf(
                answer,
                sizeof answer,
                session.options.commander_warp != 0 ? "Cmd warping enabled" : "Cmd warping disabled"
            );
            break;
        default:
            break;
        }
        break;
    }
    if (answer[0] != '\0')
        recorder_say(lobby, answer);
    return true;
}

void recorder_note_block(Lobby& lobby, int32_t slot) noexcept {
    if (lobby.wire_rules.recorder_protocol == netgame::recorder_protocol_plain || slot < 0 ||
        slot >= kSlotCount)
        return;
    auto& session = lobby.recorder;
    auto* bytes = reinterpret_cast<uint8_t*>(slot_info(lobby, slot));
    if (bytes == nullptr)
        return;
    const auto protocol = bytes[netgame::player_info_recorder_protocol_offset];
    session.peer_protocol[slot] = protocol;
    // After .votego or .forcego a watcher counts as ready.
    auto& info = *reinterpret_cast<PlayerSetupInfo*>(bytes);
    if (session.watchers_ready && (info.options & option::watcher) != 0)
        info.options |= option::ready;
    if (!host_is_local(lobby) || protocol < kRecorderOptionsProtocol ||
        session.host_options_sent[slot])
        return;
    session.host_options_sent[slot] = true;
    uint8_t payload[netgame::recorder_host_options_bytes];
    netgame::encode_recorder_host_options(session.options, payload);
    uint8_t record[netgame::recorder_message_header_bytes + sizeof payload];
    std::size_t written = 0;
    if (netgame::encode_recorder_message(
            netgame::RecorderMessageKind::host_options,
            payload,
            sizeof payload,
            record,
            sizeof record,
            &written
        ) != netgame::WireError::ok)
        return;
    flush(lobby);
    send(lobby, local_player(lobby).player_id, slot_player(lobby, slot).player_id, record, written);
    flush(lobby);
}

bool recorder_lobby_record(
    Lobby& lobby, int32_t sender_slot, const uint8_t* data, std::size_t size
) noexcept {
    if (sender_slot < 0 || sender_slot >= kSlotCount || size == 0)
        return false;
    netgame::RecorderMessage message{};
    if (data[0] != static_cast<uint8_t>(netgame::RecorderRecordType::message) ||
        netgame::decode_recorder_message(data, size, &message) != netgame::WireError::ok)
        return false;
    if (message.kind == netgame::RecorderMessageKind::host_options &&
        lobby_host_slot(lobby) == sender_slot) {
        netgame::RecorderHostOptions options{};
        if (netgame::decode_recorder_host_options(message.payload, message.size, &options) !=
            netgame::WireError::ok)
            return false;
        // Without the rules' speed lock the host's lock is not taken.
        if (!lobby.wire_rules.speed_lock)
            options.speed_lock = 0;
        lobby.recorder.options = options;
        return true;
    }
    if (message.kind == netgame::RecorderMessageKind::warp_done) {
        lobby.recorder.warp_done[sender_slot] = true;
        return true;
    }
    return false;
}

namespace {} // namespace

// ---------------------------------------------------------------------------
// Accessors

FieldRef<uint16_t> lobby_player_count(Game& game) noexcept {
    return {game_bytes(game, kPlayerCountOffset)};
}

FieldRef<uint16_t> lobby_chat_head(Game& game) noexcept {
    return {game_bytes(game, kChatHeadOffset)};
}

FieldRef<uint16_t> lobby_chat_tail(Game& game) noexcept {
    return {game_bytes(game, kChatTailOffset)};
}

char* lobby_chat_line(Game& game, std::size_t index) noexcept {
    return reinterpret_cast<char*>(
        game_bytes(game, kChatRingOffset + (index % kChatLines) * kChatLineBytes)
    );
}

char* lobby_game_name(Game& game) noexcept {
    return reinterpret_cast<char*>(game_bytes(game, kGameNameOffset));
}

char* lobby_nickname(Game& game) noexcept {
    return reinterpret_cast<char*>(game_bytes(game, kNicknameOffset));
}

char* lobby_password(Game& game) noexcept {
    return reinterpret_cast<char*>(game_bytes(game, kPasswordOffset));
}

FieldRef<uint16_t> lobby_option_word(Game& game) noexcept {
    return {game_bytes(game, kLobbyOptionOffset)};
}

FieldRef<uint16_t> lobby_max_units(Game& game) noexcept {
    return {game_bytes(game, kMaxUnitsOffset)};
}

FieldRef<uint16_t> lobby_max_units_default(Game& game) noexcept {
    return {game_bytes(game, kMaxUnitsDefaultOffset)};
}

FieldRef<int32_t> lobby_screen_width(Game& game) noexcept {
    return {game_bytes(game, kScreenWidthOffset)};
}

FieldRef<int32_t> lobby_screen_height(Game& game) noexcept {
    return {game_bytes(game, kScreenHeightOffset)};
}

uint8_t* lobby_provider_guid(Game& game) noexcept {
    return game_bytes(game, kProviderGuidOffset);
}

FieldRef<int32_t> lobby_session_players(Game& game) noexcept {
    return {game_bytes(game, kSessionPlayersOffset)};
}

FieldRef<int32_t> lobby_player_ping(Player& player) noexcept {
    return {player_bytes(player, kPlayerPingOffset)};
}

FieldRef<uint32_t> lobby_player_join_tick(Player& player) noexcept {
    return {player_bytes(player, kPlayerJoinTickOffset)};
}

uint8_t& lobby_player_team(Player& player) noexcept {
    return *player_bytes(player, kPlayerTeamOffset);
}

uint8_t* lobby_player_allied_back(Player& player) noexcept {
    return player_bytes(player, kPlayerAlliedBackOffset);
}

uint8_t lobby_side_count(const Game& game) noexcept {
    uint32_t count = 0;
    std::memcpy(&count, reinterpret_cast<const uint8_t*>(&game) + kSideCountOffset, sizeof(count));
    return static_cast<uint8_t>(std::min<uint32_t>(count, 0xff));
}

uint8_t lobby_offered_side(const Game& game, uint32_t side, uint32_t also_hidden) noexcept {
    const uint32_t count = lobby_side_count(game);
    const uint32_t hidden = oa::data::defs::battle_room_hidden_sides() | also_hidden;
    while (side < count && side < 32 && (hidden & (1U << side)) != 0)
        ++side;
    return static_cast<uint8_t>(std::min(side, count));
}

uint8_t lobby_offered_side_before(const Game& game, uint32_t side, uint32_t also_hidden) noexcept {
    const uint32_t count = lobby_side_count(game);
    const uint32_t hidden = oa::data::defs::battle_room_hidden_sides() | also_hidden;
    uint32_t at = side < count ? side : count;
    while (at > 0) {
        --at;
        if (at < 32 && (hidden & (1U << at)) != 0)
            continue;
        return static_cast<uint8_t>(at);
    }
    return static_cast<uint8_t>(count);
}

uint32_t lobby_computer_hidden_sides() noexcept {
    const uint32_t allowed = oa::data::defs::computer_player_sides();
    return allowed == 0 ? 0U : ~allowed;
}

StartedOptions lobby_started_options(Game& game) noexcept {
    const auto load = [&](std::size_t offset) {
        uint32_t value = 0;
        std::memcpy(&value, game_bytes(game, offset), sizeof(value));
        return value;
    };
    return {
        load(kCommanderOptionOffset),
        load(kUnmappedOptionOffset),
        load(kLosLimitedOptionOffset),
        load(kLosTrueOptionOffset),
        0
    };
}

PlayerSetupInfo* player_info(Lobby& lobby, const Player& player) noexcept {
    if (player.info == 0 || player.info > static_cast<uint32_t>(kSlotCount))
        return nullptr;
    return &lobby.infos[player.info - 1];
}

PlayerSetupInfo* slot_info(Lobby& lobby, int32_t slot) noexcept {
    if (slot < 0 || slot >= kSlotCount)
        return nullptr;
    return player_info(lobby, lobby.game->players[slot]);
}

Player& slot_player(Lobby& lobby, int32_t slot) noexcept {
    return lobby.game->players[std::clamp(slot, 0, kSlotCount - 1)];
}

Player& local_player(Lobby& lobby) noexcept {
    return slot_player(lobby, lobby.game->local_player_index);
}

PlayerSetupInfo& local_info(Lobby& lobby) noexcept {
    return info_of(lobby, local_player(lobby));
}

void lobby_reset(Lobby& lobby, Game& game) noexcept {
    lobby.game = &game;
    std::memset(static_cast<void*>(lobby.infos), 0, sizeof(lobby.infos));
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = game.players[slot];
        std::memset(&player, 0, sizeof(player));
        player.info = static_cast<oa_ref32>(slot + 1);
        player.index = kNoSlot;
        lobby_player_team(player) = kNoTeam;
        lobby.infos[slot].color = kNoColor;
    }
    game.local_player_index = 0;
    lobby_chat_head(game) = 0;
    lobby_chat_tail(game) = 0;
    lobby_player_count(game) = 1;
    lobby.rows_built = false;
    lobby.start_frame = 0;
    lobby.last_player_count = 0xffffffffU;
    lobby.confirm_slot = kNoSlot;
    lobby.timeout_player = 0;
    lobby.stalled_player = 0;
    lobby.timeout_baseline = 0;
    if (game.player_timeout_seconds <= 0)
        game.player_timeout_seconds = kDefaultPlayerTimeoutSeconds;
    lobby.row_base = 0;
    lobby.refused_joiner = 0;
    // The other players' presence records are forgotten; this machine's stays.
    for (int32_t slot = 0; slot < kSlotCount; ++slot)
        presence_forget(lobby.presence_records, slot);
    std::fill(std::begin(lobby.computer_difficulty), std::end(lobby.computer_difficulty), 0);
    std::fill(
        std::begin(lobby.computer_difficulty_set), std::end(lobby.computer_difficulty_set), false
    );
    lobby.computer_brain.fill(std::nullopt);
    lobby.computer_launch_brain.fill({});
    lobby.computer_command_player = 0;
    lobby.computer_command_kind = 0;
    lobby.computer_command_value = 0;
    lobby.presence_records.plain_sim_hash = plain_baseline_sim_hash();
    // A new game does not keep the previous host's list. This machine's
    // outgoing list stays, as the source record does. The generation moves
    // so a guest's copy empties even under a host that sends no list, and
    // the next host's record is folded even when its bytes are the same.
    lobby.presence_records.host_loadout = {};
    ++lobby.presence_records.host_loadout_generation;
    lobby.presence_records.host_fed_size = 0;
    lobby.presence_records.host_fed_hash = 0;
    lobby.presence_records.part_refused_digest.reset();
    lobby.mutators_hold_seen = false;
    lobby.classic_lines_posted = false;
    lobby.leave_line = nullptr;
    std::fill(
        std::begin(lobby.side_past_table_posted),
        std::end(lobby.side_past_table_posted),
        static_cast<int16_t>(-1)
    );
    std::fill(std::begin(lobby.limits_older_refused), std::end(lobby.limits_older_refused), false);
    // The recorder's session starts again; the line it answers with stays.
    char program[sizeof lobby.recorder.program];
    std::memcpy(program, lobby.recorder.program, sizeof program);
    lobby.recorder = netgame::RecorderSession{};
    std::memcpy(lobby.recorder.program, program, sizeof program);
}

void lobby_seat_local(Lobby& lobby, int32_t slot, bool host, const char* nickname) noexcept {
    auto& game = *lobby.game;
    game.local_player_index = static_cast<uint8_t>(slot);
    auto& player = slot_player(lobby, slot);
    player.in_use = 1;
    player.index = static_cast<uint8_t>(slot);
    copy_text(player.name, sizeof(player.name), nickname != nullptr ? nickname : "");
    copy_text(
        player.second_name, sizeof(player.second_name), text_view(player.name, sizeof(player.name))
    );
    slot_set_status(lobby, player, kSlotLocal);
    auto& info = info_of(lobby, player);
    info.role = host ? kRoleHost : 0;
    info.version_major = static_cast<uint8_t>(lobby.local_version_major);
    info.version_minor = static_cast<uint8_t>(lobby.local_version_minor);
    info.net_id = player.player_id;
    if (host && info.color == kNoColor)
        info.color = static_cast<uint8_t>(lobby_next_free_color(lobby));
}

// ---------------------------------------------------------------------------
// Slot predicates

bool slot_active(const Player& player) noexcept {
    return player.in_use != 0 &&
           (player.status == kSlotLocal || player.status == kSlotComputer ||
            player.status == kSlotRemote) &&
           player.index != kNoSlot;
}

bool slot_remote_playing(Lobby& lobby, const Player& player) noexcept {
    return occupied_by(player, kSlotRemote) && info_of(lobby, player).state == kInfoStatePlaying;
}

bool slot_remote_defeated(Lobby& lobby, const Player& player) noexcept {
    return occupied_by(player, kSlotRemote) && info_of(lobby, player).state == kInfoStateDefeated;
}

bool slot_participating(const Player& player) noexcept {
    return player.in_use != 0 && slot_active(player) &&
           (player.unit_count != 0 || player.units_created == 0) && player.index != kNoSlot;
}

uint8_t lobby_host_slot(Lobby& lobby) noexcept {
    for (uint8_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        if (player.status != kSlotOpen && (info_of(lobby, player).role & kRoleHost) != 0)
            return slot;
    }
    return kNoSlot;
}

void slot_set_status(Lobby& lobby, Player& player, uint8_t status) noexcept {
    player.status = status;
    if (status != kSlotRemote)
        info_of(lobby, player).state = status;
}

bool lobby_launch_locked(Lobby& lobby) noexcept {
    return lobby_options_locked(lobby) && lobby.option_4;
}

bool lobby_options_locked(Lobby& lobby) noexcept {
    return (lobby_option_word(*lobby.game) & kSetupOptionsLocked) != 0;
}

bool lobby_launch_active(const Lobby& lobby) noexcept {
    const auto& link = lobby.launch_link;
    return link.launch_active != nullptr && link.launch_active(link.context);
}

bool lobby_launch_block_active(const Lobby& lobby) noexcept {
    return lobby.launch_link.block != nullptr && lobby_launch_active(lobby);
}

bool info_version_compatible(
    const PlayerSetupInfo& info, uint32_t local_major, const netgame::WireRules& rules
) noexcept {
    int32_t version = info.version_major;
    if (rules.launch_version_bias && (info.status & status::launch_only) != 0)
        version -= 100;
    auto compared = rules;
    compared.version_major = static_cast<uint8_t>(local_major);
    return netgame::version_admits(compared, version);
}

char* local_password_field(Lobby& lobby) noexcept {
    return local_info(lobby).password;
}

bool lobby_has_host_map(Lobby& lobby) noexcept {
    if (!map_selected(lobby))
        return false;
    const auto host = lobby_host_slot(lobby);
    if (host != kNoSlot) {
        const auto& info = info_of(lobby, slot_player(lobby, host));
        if (info.version_major > 1 || (info.version_major == 1 && info.version_minor > 1)) {
            const auto hash = lobby.maps.content_hash != nullptr
                                  ? lobby.maps.content_hash(lobby.maps.context)
                                  : 0U;
            return hash == info.map_hash;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------
// Colours, teams and alliances

int32_t lobby_next_free_color(Lobby& lobby) noexcept {
    int32_t used[kColorCount] = {};
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        if (!slot_active(player))
            continue;
        const auto color = info_of(lobby, player).color;
        used[color < 9 ? color : 9] = 1;
    }
    for (int32_t color = 0; color < kColorCount; ++color)
        if (used[color] == 0)
            return color;
    return 0;
}

int32_t team_member_count(Lobby& lobby, uint32_t team) noexcept {
    if (team == kNoTeam)
        return 0;
    const bool watching_started = (lobby.game->session_flags & kNetFlagGameStarted) != 0;
    int32_t count = 0;
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        if (lobby_player_team(player) != team || !slot_active(player))
            continue;
        if (watching_started && !slot_participating(player))
            continue;
        ++count;
    }
    return count;
}

bool lobby_all_on_one_team(Lobby& lobby) noexcept {
    const auto total = idle_participant_count(lobby) + playing_count(lobby);
    for (uint32_t team = 0; team < kTeamCount; ++team)
        if (team_member_count(lobby, team) == total)
            return true;
    return false;
}

int32_t next_team_mate(Lobby& lobby, int32_t slot, int32_t start) noexcept {
    if (start >= kSlotCount || start < 0)
        return -1;
    const auto team = lobby_player_team(slot_player(lobby, slot));
    for (int32_t other = start; other < kSlotCount; ++other) {
        auto& player = slot_player(lobby, other);
        if (lobby_player_team(player) == team && player.status != kSlotOpen &&
            lobby_player_team(player) != kNoTeam)
            return other;
        if (other == slot)
            return other;
    }
    return -1;
}

void lobby_update_team_icons(Lobby& lobby, Panel& panel) noexcept {
    const bool watching_started = (lobby.game->session_flags & kNetFlagGameStarted) != 0;
    int32_t shown = 0;
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        if (!slot_active(player) || player.status == kSlotBlocked)
            continue;
        if (watching_started &&
            (!slot_participating(player) || info_of(lobby, player).color == kNoColor))
            continue;
        char name[64];
        format(name, "TEAMICONS%d", watching_started ? shown++ : slot);
        const auto team = lobby_player_team(player);
        const auto members = team_member_count(lobby, team);
        int32_t stage = 10;
        if (members != 0)
            stage = team * 2 + (members == 1 ? 1 : 0);
        panel_set_stage(panel, name, stage);
    }
    lobby.game->gui_flags |= 1U;
}

void lobby_update_ally_matrix(Lobby& lobby) noexcept {
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        if (!slot_active(player) || player.status == kSlotBlocked)
            continue;
        int32_t mate = 0;
        while ((mate = next_team_mate(lobby, slot, mate)) != -1) {
            lobby_player_allied_back(player)[mate] = 1;
            player.alliance[mate] = 1;
            info_of(lobby, slot_player(lobby, mate)).status |= status::allied_victory;
            info_of(lobby, player).status |= status::allied_victory;
            ++mate;
        }
        // teams.allied-victory-kept leaves the player's Allied Victory
        // setting as it is here; leaving a team still clears it.
        if (team_member_count(lobby, lobby_player_team(player)) < 2 &&
            !teams_rules(lobby).allied_victory_kept.enabled)
            info_of(lobby, player).status &= static_cast<uint16_t>(~status::allied_victory);
    }
}

void lobby_leave_team(Lobby& lobby, int32_t slot) noexcept {
    auto& player = slot_player(lobby, slot);
    const auto team = lobby_player_team(player);
    if (team == kNoTeam)
        return;
    for (int32_t other = 0; other < kSlotCount; ++other) {
        auto& mate = slot_player(lobby, other);
        if (!slot_active(mate) || mate.status == kSlotBlocked || lobby_player_team(mate) != team ||
            other == player.index)
            continue;
        lobby_set_alliance(lobby, player, mate, 0, true);
        info_of(lobby, player).status &= static_cast<uint16_t>(~status::allied_victory);
    }
}

void lobby_cycle_team(Lobby& lobby, Panel& panel, int32_t slot) noexcept {
    auto& player = slot_player(lobby, slot);
    const auto team = lobby_player_team(player);
    lobby_leave_team(lobby, slot);
    lobby_player_team(player) = static_cast<uint8_t>((team + 1U) % 6U);
    lobby_send_team(lobby, player);
    lobby_update_ally_matrix(lobby);
    lobby_update_team_icons(lobby, panel);
}

// ---------------------------------------------------------------------------
// Slot table maintenance

void lobby_swap_slots(Lobby& lobby, int32_t first, int32_t second) noexcept {
    auto& a = slot_player(lobby, first);
    auto& b = slot_player(lobby, second);
    const Player held = b;
    b = a;
    a = held;
    // The presence entries stay with their players.
    auto& records = lobby.presence_records;
    std::swap(records.peer[first], records.peer[second]);
    std::swap(records.peer_size[first], records.peer_size[second]);
    std::swap(records.peer_id[first], records.peer_id[second]);
    std::swap(records.block_player[first], records.block_player[second]);
    std::swap(records.sent_generation[first], records.sent_generation[second]);
    std::swap(records.sent_to[first], records.sent_to[second]);
    std::swap(lobby.computer_difficulty[first], lobby.computer_difficulty[second]);
    std::swap(lobby.computer_difficulty_set[first], lobby.computer_difficulty_set[second]);
    std::swap(lobby.computer_brain[first], lobby.computer_brain[second]);
    std::swap(
        lobby.computer_launch_brain[static_cast<std::size_t>(first)],
        lobby.computer_launch_brain[static_cast<std::size_t>(second)]
    );
    std::swap(lobby.side_past_table_posted[first], lobby.side_past_table_posted[second]);
    std::swap(lobby.limits_older_refused[first], lobby.limits_older_refused[second]);
    slot_set_status(lobby, a, kSlotOpen);
    a.in_use = 0;
    // The game renumbers eleven records; the eleventh lies past the table.
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        player.index = slot_active(player) ? static_cast<uint8_t>(slot) : kNoSlot;
    }
}

void lobby_compact_slots(Lobby& lobby) noexcept {
    const auto active_at = [&](int32_t slot) {
        return slot < kSlotCount && slot_active(slot_player(lobby, slot));
    };
    int32_t low = 0;
    int32_t high = 0;
    for (;;) {
        if (high >= kSlotCount && low >= kSlotCount)
            return;
        while ((active_at(low) ||
                (low < kSlotCount && slot_player(lobby, low).status == kSlotBlocked)) &&
               low < kSlotCount)
            ++low;
        high = low;
        for (;;) {
            ++high;
            if (active_at(high))
                break;
            if (high >= kSlotCount)
                return;
        }
        if (high >= kSlotCount || low >= kSlotCount)
            return;
        lobby_swap_slots(lobby, high, low);
    }
}

// ---------------------------------------------------------------------------
// Battleroom panel

void control_make_label(Control& control) noexcept {
    if (control.type != ControlType::button)
        return;
    control.x = static_cast<int16_t>(control.x + 2);
    control.type = ControlType::label;
    control.attributes |= kAttributeLabelText;
}

/// Restores a row label to the button it was cloned from.
///
/// @param[in,out] control A label made from a button; anything else is left.
void control_make_button(Control& control) noexcept {
    if (control.type != ControlType::label)
        return;
    control.x = static_cast<int16_t>(control.x - 2);
    control.type = ControlType::button;
    control.attributes &= ~kAttributeLabelText;
}

void lobby_build_rows(Lobby& lobby, Panel& panel) noexcept {
    const auto local = lobby.game->local_player_index;
    if (lobby.row_base > 0)
        panel.count = lobby.row_base;
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        for (int32_t key = 0; key < static_cast<int32_t>(std::size(kRowKeys)); ++key) {
            const auto source = panel_find(panel, kRowKeys[key]);
            if (source == kNoControl)
                continue;
            const auto index = panel_clone(
                panel, source, static_cast<char>('0' + slot), static_cast<int16_t>(slot * kRowPitch)
            );
            if (index == kNoControl)
                return;
            auto& row = panel.controls[static_cast<std::size_t>(index)];
            row.active = 1;
            if (row.type == ControlType::label)
                continue;
            switch (key) {
            case row_player:
                if (slot == local) {
                    control_make_label(row);
                    row.attributes = 1;
                } else {
                    row.attributes |= kAttributeRemoteRow;
                }
                break;
            case row_ready:
                if (slot != local) {
                    row.grayed = true;
                    row.value = 0;
                }
                break;
            case row_logo:
                if (!local_or_computer(player))
                    row.active = 0;
                break;
            case row_side:
                row.grayed = !local_or_computer(player);
                row.active = 0;
                break;
            case row_res:
                if (slot != local)
                    control_make_label(row);
                if (occupied_by(player, kSlotComputer))
                    row.active = 0;
                break;
            case row_ally:
                if (slot == local)
                    row.active = 0;
                break;
            case row_team:
                row.active = 0;
                row.stage = 10;
                break;
            default:
                control_make_label(row);
                break;
            }
        }
    }
    if (auto* own = row_control(panel, "PLAYER%d", local))
        control_make_label(*own);
    lobby.rows_built = true;
}

void lobby_update_option_buttons(Lobby& lobby, Panel& panel) noexcept {
    auto slot = lobby_host_slot(lobby);
    if (slot == kNoSlot)
        slot = lobby.game->local_player_index;
    const auto options = info_of(lobby, slot_player(lobby, slot)).options;
    panel_set_stage(panel, "COMMANDER", (options & option::commander_mask) >> 11);
    panel_set_stage(panel, "MAPPING", ~(options >> 8) & 1);
    const int32_t los = (options & option::los_limited) == 0 ? 2 : (~options >> 10) & 1;
    panel_set_stage(panel, "LOSTYPE", los);
    panel_set_stage(panel, "WATCHING", (options & 0xff) >> 7);
    panel_set_stage(panel, "CHEATING", (options & option::cheats_allowed) >> 13);
    panel_set_stage(panel, "FIXEDLOC", (options & option::fixed_locations) >> 14);
    panel_set_stage(panel, "GAMEOPEN", (options & option::game_closed) == 0 ? 1 : 0);
}

void lobby_update_side_button(Lobby& lobby, Panel& panel, int32_t slot) noexcept {
    auto& player = slot_player(lobby, slot);
    const auto& info = info_of(lobby, player);
    char name[64];
    format(name, "SIDE%d", slot);
    const bool watcher = player.in_use != 0 && (info.options & option::watcher) != 0;
    panel_set_stage(panel, name, watcher ? oa::data::defs::watcher_side_frame() : info.side);
}

void lobby_on_max_units(Panel& panel, void* user) noexcept {
    auto& lobby = *static_cast<Lobby*>(user);
    auto* slider = panel_control(panel, "MAXUNITS");
    if (slider == nullptr)
        return;
    const auto host = lobby_host_slot(lobby);
    int32_t value = 0;
    if (host == lobby.game->local_player_index || host == kNoSlot)
        value = oa::ui::gui_input::scroll_value(slider->scroll) + kMaxUnitsFloor;
    else
        value = info_of(lobby, slot_player(lobby, host)).max_units;
    char text[64];
    format(text, "%d", value);
    panel_set_text(panel, "MAXUNITSTEXT", text);
    auto& info = local_info(lobby);
    info.max_units = static_cast<uint16_t>(value);
    if ((info.role & kRoleHost) != 0)
        lobby_send_player_info(lobby);
}

void lobby_on_metal(Panel& panel, void* user) noexcept {
    auto& lobby = *static_cast<Lobby*>(user);
    auto* slider = panel_control(panel, "METAL");
    if (slider == nullptr)
        return;
    const auto value = oa::ui::gui_input::scroll_value(slider->scroll) / 100 * 100;
    char text[64];
    format(text, "%d", value);
    panel_set_text(panel, "METALTEXT", text);
    auto& info = local_info(lobby);
    info.metal_hundreds = static_cast<uint16_t>(value / 100);
    if ((info.role & kRoleHost) != 0) {
        lobby_send_player_info(lobby);
        lobby_publish_session(lobby);
    }
}

void lobby_on_energy(Panel& panel, void* user) noexcept {
    auto& lobby = *static_cast<Lobby*>(user);
    auto* slider = panel_control(panel, "ENERGY");
    if (slider == nullptr)
        return;
    const auto value = oa::ui::gui_input::scroll_value(slider->scroll) / 100 * 100;
    char text[64];
    format(text, "%d", value);
    panel_set_text(panel, "ENERGYTEXT", text);
    auto& info = local_info(lobby);
    info.energy_hundreds = static_cast<uint16_t>(value / 100);
    if ((info.role & kRoleHost) != 0) {
        lobby_send_player_info(lobby);
        lobby_publish_session(lobby);
    }
}

void lobby_bind_slider(
    Lobby& lobby,
    Panel& panel,
    const char* name,
    int32_t maximum,
    int32_t value,
    SliderHandler handler
) noexcept {
    if (auto* slider = panel_control(panel, name)) {
        slider->scroll.maximum = maximum;
        slider->on_change = handler;
        oa::ui::gui_input::scroll_set_value(slider->scroll, value);
    }
    if (handler != nullptr)
        handler(panel, &lobby);
    panel.dirty = true;
}

void lobby_cycle_resolution(Lobby& lobby, bool backwards) noexcept {
    if (lobby.services.display_modes == nullptr)
        return;
    DisplayMode modes[100];
    const auto count = lobby.services.display_modes(lobby.services.context, modes, 100);
    if (count <= 0)
        return;
    auto& info = local_info(lobby);
    for (int32_t index = 0; index < count; ++index) {
        if (modes[index].width != info.screen_width || modes[index].height != info.screen_height)
            continue;
        int32_t next = backwards ? index - 1 : index + 1;
        if (next < 0)
            next = count - 1;
        if (next >= count)
            next = 0;
        info.screen_width = static_cast<uint16_t>(modes[next].width);
        info.screen_height = static_cast<uint16_t>(modes[next].height);
        lobby_send_player_info(lobby);
        lobby_screen_width(*lobby.game) = modes[next].width;
        lobby_screen_height(*lobby.game) = modes[next].height;
        // The same preference the settings store, applied at once: RES has
        // no OK of its own.
        multiplayer_choose_screen_size(modes[next].width, modes[next].height);
        return;
    }
}

void lobby_update_status(Lobby& lobby, Panel& panel) noexcept {
    auto& game = *lobby.game;
    const auto local = game.local_player_index;
    auto& me = local_player(lobby);
    const int32_t my_ready = (info_of(lobby, me).options & option::ready) >> 5;
    uint32_t lowest_latency = 0xffffffffU;

    if (auto* output = panel_control(panel, "OUTPUT")) {
        uint32_t head = lobby_chat_head(game);
        const uint32_t tail = lobby_chat_tail(game);
        if (head < tail)
            head += kChatLines;
        const int32_t rows = output->height / (12 + 2);
        if (rows < static_cast<int32_t>(head - tail)) {
            auto next = static_cast<uint16_t>(lobby_chat_tail(game) + 1);
            if (next > kChatLines - 1)
                next = 0;
            lobby_chat_tail(game) = next;
        }
        std::vector<std::string> lines;
        for (uint32_t at = lobby_chat_tail(game); at != lobby_chat_head(game);
             at = (at + 1) % kChatLines)
            lines.emplace_back(text_view(lobby_chat_line(game, at), kChatLineBytes));
        panel_set_items(panel, "OUTPUT", std::move(lines));
    }
    lobby_update_option_buttons(lobby, panel);

    if (auto* map_label = panel_control(panel, "MAPNAME")) {
        if (!map_selected(lobby)) {
            map_label->color = kWarningColor;
            panel_set_text(panel, "MAPNAME", "NOT SELECTED");
        } else {
            const auto* context = lobby.maps.map_context != nullptr
                                      ? lobby.maps.map_context(lobby.maps.context)
                                      : nullptr;
            const char* localized = context != nullptr
                                        ? data::campaign::campaign_localized_name(context)
                                        : map_name(lobby);
            // A joiner who lacks the host's map sees the name the host sent.
            // A machine that has the map keeps the name the map file gives.
            std::string host_map;
            const char* name = localized;
            if (!host_is_local(lobby) && !lobby_has_host_map(lobby)) {
                const auto host = lobby_host_slot(lobby);
                if (host != kNoSlot) {
                    const auto& host_info = info_of(lobby, slot_player(lobby, host));
                    host_map.assign(
                        host_info.map_name, ::strnlen(host_info.map_name, sizeof host_info.map_name)
                    );
                    name = host_map.c_str();
                }
            }
            // The label shows the map's own name. The key on the wire stays
            // name@package, including the name a joiner is missing.
            const std::string_view carried(name != nullptr ? name : "");
            const bool packaged = oa::data::map_pack::split_pack_map_name(carried).has_value();
            const std::string own =
                packaged ? multiplayer_shown_map_name(name) : std::string(carried);
            const char* shown = own.c_str();
            const bool changed = control_text(*map_label) != std::string_view(shown);
            if (!lobby_has_host_map(lobby)) {
                map_label->color = ((now(lobby) / 30) & 1U) != 0 ? kWarningColor : 0;
                if (changed) {
                    // Said in English, as English 3.1c sends it; each machine
                    // shows it in its own language.
                    lobby_say(lobby, me, sim::messages::phrase_missing_map);
                    info_of(lobby, me).options &= static_cast<uint16_t>(~option::ready);
                    set_group(panel, "READY%d", local, 0);
                    lobby_send_player_info(lobby);
                }
                if (!host_is_local(lobby))
                    panel_set_grayed(panel, "MAP", true);
            } else {
                map_label->color = 0;
                panel_set_grayed(panel, "MAP", false);
            }
            set_control_text(*map_label, shown);
        }
    }

    // A guest that has not built the host's list is not ready, whether or
    // not the map label is on this panel. Said once, when the bit was on.
    if (guest_awaits_mutators(lobby) && (info_of(lobby, me).options & option::ready) != 0) {
        info_of(lobby, me).options =
            static_cast<uint16_t>(info_of(lobby, me).options & ~option::ready);
        set_group(panel, "READY%d", local, 0);
        lobby_send_player_info(lobby);
    }

    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        if (local_or_computer(player)) {
            auto& info = info_of(lobby, player);
            info.options = static_cast<uint16_t>(
                (info.options & ~option::ready) | (info_of(lobby, me).options & option::ready)
            );
        }
    }
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        if ((info_of(lobby, me).options & option::commander_mask) == option::commander_deathmatch &&
            (occupied_by(player, kSlotComputer) || slot_remote_defeated(lobby, player))) {
            lobby_reject(lobby, player.player_id, kRejectDeathmatch);
            lobby_send_player_info(lobby);
        }
        const auto host = lobby_host_slot(lobby);
        if (host != kNoSlot &&
            (info_of(lobby, slot_player(lobby, host)).options & option::watching_allowed) == 0 &&
            player.in_use != 0) {
            auto& info = info_of(lobby, player);
            if ((info.options & option::watcher) != 0) {
                info.options &= static_cast<uint16_t>(~option::watcher);
                info.side = 0;
                lobby_send_player_info(lobby);
            }
        }
    }
    lobby_update_ally_matrix(lobby);
    lobby_update_team_icons(lobby, panel);

    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        auto& info = info_of(lobby, player);
        char name[64];
        const bool watcher = player.in_use != 0 && (info.options & option::watcher) != 0;
        if (!slot_active(player) && !watcher) {
            format(name, "CD%d", slot);
            panel_set_active(panel, name, false);
            panel_set_grayed(panel, name, false);
            format(name, "PLAYER%d", slot);
            // In the language shown, as 3.1c's text setter translates it:
            // BLOCKED translated inside its brackets first.
            std::string unused = "UNUSED";
            if (player.status == kSlotBlocked)
                unused = std::string("[") + lobby_translated(lobby, "BLOCKED") + "]";
            panel_set_text(panel, name, lobby_translated(lobby, unused.c_str()));
            panel_set_grayed(panel, name, my_ready != 0);
            for (const char* pattern :
                 {"LOGO%d", "SIDE%d", "TEAMICONS%d", "RES%d", "PING%d", "MEM%d"})
                if (auto* control = row_control(panel, pattern, slot))
                    control->active = 0;
            if (slot != local)
                if (auto* ally = row_control(panel, "ALLY%d", slot))
                    ally->active = 0;
            if (auto* ready = row_control(panel, "READY%d", slot)) {
                ready->value = 0;
                ready->active = 0;
                ready->grayed = true;
            }
            continue;
        }
        const bool present = occupied_by(player, kSlotLocal) || slot_remote_playing(lobby, player);
        // OA asks for no game disc, so an OA player's row shows the OA badge
        // in place of the CD icon. A player on 3.1c still shows the disc it
        // has: a 3.1c host counts them before START.
        format(name, "CD%d", slot);
        panel_set_active(panel, name, lobby_row_badge(lobby, slot).badge == RowBadge::disc);
        panel_set_grayed(panel, name, false);
        if (auto* logo = row_control(panel, "LOGO%d", slot)) {
            logo->active = (info.color == kNoColor && my_ready == 0) ? 0 : 1;
            logo->stage = info.color;
            // A colour is not changed while the local player is ready.
            logo->hot = my_ready == 0;
        }
        format(name, "PLAYER%d", slot);
        panel_set_text(panel, name, text_view(player.name, 0x1e));
        panel_set_grayed(panel, name, my_ready != 0);
        if (auto* side = row_control(panel, "SIDE%d", slot)) {
            lobby_update_side_button(lobby, panel, slot);
            side->active = 1;
            side->grayed =
                (!local_or_computer(player) && !host_edits_remote_computer(lobby, slot)) ||
                my_ready != 0;
        }
        if (auto* ally = row_control(panel, "ALLY%d", slot)) {
            ally->stage =
                static_cast<uint8_t>(lobby_player_allied_back(me)[slot] << 1 | me.alliance[slot]);
            const bool live = player.in_use != 0 && (player.status == kSlotLocal || watcher ||
                                                     player.status == kSlotComputer);
            const bool me_watching =
                me.in_use != 0 && (info_of(lobby, me).options & option::watcher) != 0;
            ally->active = (!live && !slot_remote_defeated(lobby, player) && !me_watching) ? 1 : 0;
            ally->grayed = my_ready != 0;
        }
        if (auto* team = row_control(panel, "TEAMICONS%d", slot)) {
            team->active = watcher ? 0 : 1;
            team->grayed =
                (!local_or_computer(player) && !host_edits_remote_computer(lobby, slot)) ||
                my_ready != 0;
        }
        if (auto* res = row_control(panel, "RES%d", slot)) {
            char text[64] = "";
            if (present)
                std::snprintf(text, sizeof(text), "%dx%d", info.screen_width, info.screen_height);
            set_control_text(*res, text);
            if (occupied_by(player, kSlotLocal))
                res->grayed = my_ready != 0;
            else
                res->active = 1;
        }
        if (auto* ping = row_control(panel, "PING%d", slot)) {
            if (!slot_remote_playing(lobby, player)) {
                set_control_text(*ping, "n/a");
            } else {
                const auto value = static_cast<int32_t>(lobby_player_ping(player));
                char text[64];
                format(text, "%d", value);
                if (host_is_local(lobby) &&
                    unit_sync_peer_complete(
                        lobby, netgame::player_slot_id(*lobby.game, static_cast<uint8_t>(slot))
                    ))
                    oa::base::text::append_terminated(text, ":s");
                set_control_text(*ping, text);
                if (static_cast<uint32_t>(value) <= lowest_latency)
                    lowest_latency = static_cast<uint32_t>(value);
            }
            ping->active = 1;
        }
        if (auto* mem = row_control(panel, "MEM%d", slot)) {
            if (computer_row(lobby, slot)) {
                set_control_text(
                    *mem, lobby_translated(lobby, difficulty_word(shown_difficulty(lobby, slot)))
                );
                mem->color = 0;
                mem->active = 1;
                if (edits_computer(lobby, slot)) {
                    control_make_button(*mem);
                    mem->grayed = false;
                } else {
                    control_make_label(*mem);
                    mem->grayed = true;
                }
            } else {
                control_make_label(*mem);
                char text[64] = "";
                if (present)
                    format(text, "%d", info.memory_mb);
                set_control_text(*mem, text);
                mem->color = info.memory_mb < map_memory(lobby) ? kWarningColor : 0;
                mem->active = 1;
                mem->grayed = false;
            }
        }
        if (auto* ready = row_control(panel, "READY%d", slot)) {
            ready->active = 1;
            ready->value = static_cast<int16_t>((info.options >> 5) & 1);
            const bool own = occupied_by(player, kSlotLocal);
            ready->grayed = !own || guest_awaits_mutators(lobby);
        }
    }
    panel.dirty = true;
    auto& mine = info_of(lobby, me);
    if ((mine.role & kRoleHost) != 0 && lowest_latency < mine.lowest_latency) {
        mine.lowest_latency = static_cast<uint16_t>(lowest_latency);
        lobby_publish_session(lobby);
    }
}

bool lobby_ready_to_start(Lobby& lobby) noexcept {
    // With setup.allow-start-with-ai the gate asks for a local human
    // instead of a remote player, so one human and computer players start.
    const bool with_computers = setup_rules(lobby).allow_start_with_ai.enabled;
    if ((with_computers ? local_human_count(lobby) : remote_count(lobby)) == 0)
        return false;
    bool everyone_watching = true;
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        if (player.in_use == 0) {
            everyone_watching = false;
            continue;
        }
        const auto options = info_of(lobby, player).options;
        if ((player.status == kSlotLocal || player.status == kSlotComputer ||
             player.status == kSlotRemote) &&
            (options & option::ready) == 0)
            return false;
        if ((options & option::watcher) == 0)
            everyone_watching = false;
    }
    if (lobby_player_count(*lobby.game) != 1)
        return !everyone_watching;
    return false;
}

namespace {

/// Seats the computer players an active launch names, as clicks on open slots seat them.
///
/// Each takes the next open slot, then the launch's side, team, colour
/// (or the next free one) and difficulty. Nothing is seated in a deathmatch
/// or a tournament game, which seat no computer players, nor in a room where
/// this machine already runs one, so a launch seats them once.
///
/// @param[in,out] lobby Lobby state; its launch must be active.
/// @return true when a computer player was seated
bool seat_launch_computers(Lobby& lobby) noexcept {
    namespace nl = oa::ui::frontend_multiplayer::launch;
    const nl::LaunchBlock& block = *lobby.launch_link.block;
    if (block.computer_count <= 0)
        return false;
    const auto host = lobby_host_slot(lobby);
    const auto host_options =
        host == kNoSlot ? 0 : info_of(lobby, slot_player(lobby, host)).options;
    if ((host_options & option::commander_mask) == option::commander_deathmatch || lobby.option_4)
        return false;
    // A computer player counts from the click that seats it, before the
    // session announces it.
    for (int32_t slot = 0; slot < kSlotCount; ++slot)
        if (slot_player(lobby, slot).status == kSlotComputer)
            return false;
    const auto local = static_cast<int32_t>(lobby.game->local_player_index);
    const int32_t wanted = std::min(block.computer_count, nl::kLaunchComputerLimit);
    int32_t seated = 0;
    for (int32_t slot = 0; slot < kSlotCount && seated < wanted; ++slot) {
        auto& player = slot_player(lobby, slot);
        if (slot == local || player.status != kSlotOpen)
            continue;
        const nl::LaunchComputer& computer = block.computers[seated];
        lobby_add_computer(lobby, slot);
        if (player.status != kSlotComputer)
            break; // the session took no more players
        ++seated;
        auto& info = info_of(lobby, player);
        const int32_t colour = computer.color - nl::kLaunchColourFirst;
        info.color = computer.color >= nl::kLaunchColourFirst &&
                             computer.color <= nl::kLaunchColourLast &&
                             lobby_color_available(lobby, player.player_id, colour)
                         ? static_cast<uint8_t>(colour)
                         : static_cast<uint8_t>(lobby_next_free_color(lobby));
        seat_named_side(
            lobby,
            info,
            computer.faction,
            text_view(computer.faction_name, sizeof computer.faction_name)
        );
        const uint32_t computer_hidden = lobby_computer_hidden_sides();
        if (computer_hidden != 0 && info.side < 32 && (computer_hidden & (1U << info.side)) != 0) {
            const uint8_t count = lobby_side_count(*lobby.game);
            const uint8_t offered = lobby_offered_side(*lobby.game, 0, computer_hidden);
            if (offered < count)
                info.side = offered;
            if (computer.faction != nl::faction::none)
                post_computer_launch_side(lobby, std::to_string(computer.faction - 1));
            else
                post_computer_launch_side(
                    lobby, text_view(computer.faction_name, sizeof computer.faction_name)
                );
        }
        if (computer.team >= nl::kLaunchTeamFirst && computer.team <= nl::kLaunchTeamLast)
            lobby_player_team(player) = static_cast<uint8_t>(computer.team - nl::kLaunchTeamFirst);
        if (computer.difficulty >= nl::difficulty::easy &&
            computer.difficulty <= nl::difficulty::hard)
            note_computer_difficulty(
                lobby, slot, static_cast<uint8_t>(computer.difficulty - nl::difficulty::easy)
            );
        if (computer.brain[0] == '\0')
            lobby.computer_launch_brain[static_cast<std::size_t>(slot)].clear();
        else
            lobby.computer_launch_brain[static_cast<std::size_t>(slot)] = computer.brain;
        notify(lobby, 2);
    }
    if (seated == 0)
        return false;
    lobby_update_ally_matrix(lobby);
    lobby_publish_session(lobby);
    return true;
}

/// Sets the unit restrictions an active launch names, as the host's Restrictions sets them.
///
/// Each names a unit by its FBI UnitName, in any case; a unit this game's
/// content does not have is left out. A limit of 0 disables the unit.
///
/// @param[in,out] lobby Lobby state; its launch must be active and its unit sync made.
void apply_launch_restrictions(Lobby& lobby) noexcept {
    namespace nl = oa::ui::frontend_multiplayer::launch;
    const nl::LaunchBlock& block = *lobby.launch_link.block;
    const int32_t count = std::min(block.restriction_count, nl::kLaunchRestrictionLimit);
    for (int32_t at = 0; at < count; ++at) {
        const nl::LaunchRestriction& restriction = block.restrictions[at];
        const std::string_view unit = text_view(restriction.unit, sizeof restriction.unit);
        for (int32_t type = 1; type < lobby.unit_count; ++type) {
            const LobbyUnit& candidate = lobby.units[type];
            if (candidate.unit_name == nullptr ||
                std::string_view(candidate.unit_name).size() != unit.size() ||
                !starts_nocase(candidate.unit_name, unit))
                continue;
            unit_sync_set_limit(lobby, candidate.fbi_hash, restriction.limit);
            (void)unit_sync_set_enabled(lobby, candidate.fbi_hash, restriction.limit != 0);
            break;
        }
    }
}

} // namespace

void lobby_enter_battleroom(Lobby& lobby, Panel& panel) noexcept {
    auto& game = *lobby.game;
    lobby.last_player_count = 0xffffffffU;
    lobby.rows_built = false;
    lobby.start_frame = 0;
    game.gui_flags |= 1U;
    auto& me = local_player(lobby);
    auto& info = info_of(lobby, me);
    const bool host = (info.role & kRoleHost) != 0;
    if (host)
        lobby_max_units(game) = static_cast<uint16_t>(lobby_max_units_default(game));
    info.screen_width = static_cast<uint16_t>(static_cast<int32_t>(lobby_screen_width(game)));
    info.screen_height = static_cast<uint16_t>(static_cast<int32_t>(lobby_screen_height(game)));
    mark_local_block(info);
    if (auto* entry = panel_control(panel, "MESSAGE"))
        entry->value = 0x7f;
    int32_t energy = kDefaultResource;
    int32_t metal = kDefaultResource;
    const bool launch_applied = lobby_launch_block_active(lobby);
    if (launch_applied) {
        apply_launch_options(lobby, me, info, energy, metal, host);
    } else if (host) {
        const auto saved = lobby_started_options(game);
        auto options = static_cast<uint16_t>(info.options & 0xa0ff);
        options |= static_cast<uint16_t>((saved.unmapped & 1U) << 8);
        options |= static_cast<uint16_t>((saved.los_limited & 1U) << 9);
        options |= static_cast<uint16_t>((saved.los_true & 1U) << 10);
        options |= static_cast<uint16_t>((saved.fixed_locations & 1U) << 14);
        options |= static_cast<uint16_t>((saved.commander & 3U) << 11);
        info.options = options;
    }
    const bool host_seat = host && local_or_computer(me);
    if (!host_seat || lobby_launch_locked(lobby)) {
        if (auto* map = panel_control(panel, "MAP")) {
            map->attributes = 2;
            set_control_text(*map, lobby_translated(lobby, "View Map"));
        }
    }
    if (!host || lobby_options_locked(lobby))
        for (const char* name : kHostOptionButtons)
            panel_set_grayed(panel, name, true);
    // The buttons a mod's display rules add are the host's alone.
    for (const auto& button : kModLobbyButtons)
        if ((lobby.lobby_buttons & button.bit) != 0)
            panel_set_grayed(panel, button.name, !host);
    lobby_update_option_buttons(lobby, panel);
    if (auto* mem = panel_control(panel, "MEMx"))
        mem->color = info.memory_mb < map_memory(lobby) ? kWarningColor : 0;
    unit_sync_create(lobby, host);
    if (host && launch_applied)
        apply_launch_restrictions(lobby);
    const bool synced = unit_sync_complete(lobby);
    panel_set_active(panel, "START", synced);
    panel_set_grayed(
        panel,
        "START",
        !(host && lobby_ready_to_start(lobby) && lobby_content_allows_start(lobby) &&
          lobby_mutators_allow_start(lobby) && lobby_limits_allow_start(lobby) &&
          lobby_sides_allow_start(lobby) && synced)
    );
    panel_set_grayed(panel, "RESTRICTIONS", false);
    panel_set_text(panel, "METALTEXT", "0");
    panel_set_text(panel, "ENERGYTEXT", "0");
    // A game a lobby program started, or one with a launch active, shows the
    // launch block's provider label on PREVMENU when it is short enough.
    if ((game.gui_flags & kGuiFlagLobbyLaunch) != 0 || lobby_launch_active(lobby)) {
        const char* label =
            lobby.launch_link.block != nullptr ? lobby.launch_link.block->provider_label : "";
        const bool short_label =
            label[0] != '\0' && ::strnlen(label, sizeof lobby.launch_link.block->provider_label) <=
                                    kProviderCaptionLimit;
        panel_set_text(panel, "PREVMENU", short_label ? label : kPreviousMenuCaption);
    }
    lobby_bind_slider(lobby, panel, "METAL", kResourceSliderMaximum, metal, lobby_on_metal);
    const auto units = static_cast<int32_t>(lobby_max_units(game)) - kMaxUnitsFloor;
    lobby_bind_slider(lobby, panel, "MAXUNITS", units, units, lobby_on_max_units);
    if (!host || lobby_options_locked(lobby))
        for (const char* name : {"MAXUNITS", "ENERGY", "METAL", "CHEATING"})
            panel_set_grayed(panel, name, true);
    if (lobby_launch_locked(lobby))
        panel_set_grayed(panel, "RESTRICTIONS", true);
    if (lobby_launch_active(lobby))
        panel_set_grayed(panel, "COMMANDER", true);
    lobby_bind_slider(lobby, panel, "ENERGY", kResourceSliderMaximum, energy, lobby_on_energy);
    // The host plays the map the launch names. A name that is not installed
    // clears the map, so a map already selected is not kept in its place.
    bool named_map_missing = false;
    if (host && launch_applied && lobby.launch_link.block->mission[0] != '\0') {
        const bool chosen = lobby.maps.select != nullptr &&
                            lobby.maps.select(lobby.maps.context, lobby.launch_link.block->mission);
        if (!chosen) {
            named_map_missing = true;
            info.map_name[0] = '\0';
            message(lobby, "Could not find the multiplayer map!!");
        }
    }
    if (!named_map_missing) {
        if (!map_selected(lobby))
            message(lobby, "Could not find the multiplayer map!!");
        std::snprintf(info.map_name, sizeof(info.map_name), "%s", map_name(lobby));
    }
    info.map_hash =
        lobby.maps.content_hash != nullptr ? lobby.maps.content_hash(lobby.maps.context) : 0;
    if (launch_applied)
        (void)seat_launch_computers(lobby);
    // A launch that closes the game closes it once its own computer players
    // are seated, as the host's GAMEOPEN does.
    if (host && launch_applied && lobby.launch_link.block->closed != 0)
        info.options |= option::game_closed;
    presence_refresh(lobby);
    lobby_send_player_info(lobby);
    panel.focus = panel_find(panel, "MESSAGE");
    for (const char* key : kRowKeys)
        panel_set_active(panel, key, false);
    lobby.row_base = panel.count;
    lobby_build_rows(lobby, panel);
    lobby_update_team_icons(lobby, panel);
    lobby_update_status(lobby, panel);
    // The closed doors over START take its clicks until the host's lobby is
    // first ready to start.
    if (auto* start = panel_control(panel, "battlestart")) {
        start->stage = 0;
        start->active = 1;
        start->hot = true;
    }
}

namespace {

/// Tells whether a typed line is text, compared without case.
///
/// @param text Line as typed.
/// @param word Text to compare with.
/// @return True when they match ignoring case.
bool same_nocase(std::string_view text, std::string_view word) noexcept {
    return text.size() == word.size() && starts_nocase(text, word);
}

LobbyAction handle_panel_event(Lobby& lobby, Panel& panel) noexcept;

/// The clicks on an open slot's PLAYER control that seat a computer player:
/// the first blocks the slot, the second opens it with a computer player.
constexpr int32_t kNeutralSeatClicks = 2;

/// Seats a computer player for the selected map's neutral units as START is
/// pressed (setup.map-scripted-units).
///
/// With the rule's auto-neutral-ai it acts once per battle room, while
/// map-placed units are on, when the
/// selected map places units, some of them for the neutral player, and no
/// slot's block names a computer player: the first open slot's PLAYER
/// control is clicked as the host clicks it to seat one, and three notices
/// tell the host. START then does nothing else.
///
/// @param[in,out] lobby Lobby state.
/// @param[in,out] panel The battle room panel.
/// @return true when a slot was clicked and START stops there.
bool seat_neutral_computer(Lobby& lobby, Panel& panel) noexcept {
    const auto& units = setup_rules(lobby).map_scripted_units;
    if (!units.enabled || !units.auto_neutral_ai || !lobby.map_units_on ||
        lobby.neutral_computer_seated || lobby.maps.map_context == nullptr)
        return false;
    const auto* map = lobby.maps.map_context(lobby.maps.context);
    if (map == nullptr || map->unit_count <= 0 || map->units == nullptr)
        return false;
    if (!sim::mission_units::has_neutral_map_units(map->units, map->unit_count))
        return false;
    for (int32_t slot = 0; slot < kSlotCount; ++slot)
        if (info_of(lobby, slot_player(lobby, slot)).state == team_rules::setup_computer)
            return false;
    int32_t open = -1;
    for (int32_t slot = 0; slot < kSlotCount && open < 0; ++slot)
        if (slot_player(lobby, slot).status == kSlotOpen)
            open = slot;
    if (open < 0)
        return false;
    char name[64];
    format(name, "PLAYER%d", open);
    const auto control = panel_find(panel, name);
    for (int32_t click = 0; click < kNeutralSeatClicks; ++click) {
        panel.selected = control;
        if (control != kNoControl)
            (void)handle_panel_event(lobby, panel);
    }
    lobby_post_chat(lobby, "An AI has been added to accept neutral units for this map");
    lobby_post_chat(lobby, "Remove the AI now if you don't want it");
    lobby_post_chat(lobby, "Use +spawnoff to disable extra unit spawn in general");
    lobby.neutral_computer_seated = true;
    return true;
}

/// Handles a click on the battle room panel; lobby_handle_event flushes what it queued.
///
/// @param[in,out] lobby Lobby state.
/// @param[in,out] panel The battle room panel.
/// @return What the screen should do next.
LobbyAction handle_panel_event(Lobby& lobby, Panel& panel) noexcept {
    auto& game = *lobby.game;
    if (panel.selected == kNoControl)
        return LobbyAction::none;
    const auto local = game.local_player_index;
    auto& me = local_player(lobby);
    const bool hosting = [&] {
        const auto host = lobby_host_slot(lobby);
        return host != kNoSlot && local_or_computer(slot_player(lobby, host));
    }();
    auto action = LobbyAction::none;
    char name[64];

    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        auto& info = info_of(lobby, player);

        format(name, "LOGO%d", slot);
        if (panel_selected_is(panel, name) &&
            (local_or_computer(player) || host_edits_remote_computer(lobby, slot))) {
            play(lobby, "Multi");
            if (occupied_by(player, kSlotLocal)) {
                lobby_request_color(lobby, info.color + 1U);
            } else {
                info.color = next_computer_color(lobby, player, info.color);
                if (!local_or_computer(player))
                    queue_computer_edit(lobby, player.player_id, kComputerEditColor, info.color);
            }
            game.gui_flags |= 1U;
            if (local_or_computer(player))
                lobby_send_player_info(lobby);
        }

        format(name, "PLAYER%d", slot);
        if (panel_selected_is(panel, name) && slot != local) {
            play(lobby, "Multi");
            const auto current = player.status;
            bool publish = true;
            if (current == kSlotOpen && hosting) {
                slot_set_status(lobby, player, kSlotBlocked);
                player.player_id = 0xffffffffU;
                lobby_session_players(game) = lobby_session_players(game) - 1;
            } else if (current != kSlotOpen && current != kSlotBlocked) {
                if (player.in_use != 0 && current == kSlotComputer &&
                    base::game_loop::scaled_clock_elapsed(
                        now(lobby), lobby_player_join_tick(player)
                    ) >= kComputerRejectTicks) {
                    lobby_reject(lobby, player.player_id, kRejectPlayer);
                    slot_set_status(lobby, player, kSlotOpen);
                    player.in_use = 0;
                } else if (hosting && player.in_use != 0 && current == kSlotRemote) {
                    lobby.confirm_slot = static_cast<uint8_t>(slot);
                    action = LobbyAction::confirm_reject;
                }
            } else {
                if (current == kSlotBlocked) {
                    slot_set_status(lobby, player, kSlotOpen);
                    lobby_session_players(game) = lobby_session_players(game) + 1;
                    lobby_publish_session(lobby);
                }
                const auto host = lobby_host_slot(lobby);
                const auto host_options =
                    host == kNoSlot ? 0 : info_of(lobby, slot_player(lobby, host)).options;
                if ((host_options & option::game_closed) != 0) {
                    message(lobby, "Can't add another player when game is closed.");
                    slot_set_status(lobby, player, kSlotOpen);
                    game.gui_flags |= 1U;
                    publish = false;
                } else if (
                    (host_options & option::commander_mask) != option::commander_deathmatch &&
                    !lobby.option_4
                ) {
                    // The game colours the slot and notifies whether or
                    // not the session took the player.
                    lobby_add_computer(lobby, slot);
                    info.color = static_cast<uint8_t>(lobby_next_free_color(lobby));
                    notify(lobby, 2);
                }
            }
            if (!publish)
                break;
            game.gui_flags |= 1U;
            lobby_publish_session(lobby);
            lobby_send_player_info(lobby);
        }

        format(name, "SIDE%d", slot);
        if (panel_selected_is(panel, name)) {
            if (!lobby_launch_locked(lobby)) {
                play(lobby, "Multi");
                // The button steps past a side the mod keeps out of the
                // battle room, and wraps to the first side offered. Past
                // 3.1c's five sides a right click walks that cycle backwards.
                const uint8_t count = lobby_side_count(game);
                const uint32_t computer_hidden =
                    player.status == kSlotComputer ? lobby_computer_hidden_sides() : 0U;
                const auto first_offered = [&] {
                    const uint8_t first = lobby_offered_side(game, 0, computer_hidden);
                    return first < count ? first : uint8_t{0};
                };
                const auto host = lobby_host_slot(lobby);
                const auto host_options =
                    host == kNoSlot ? 0 : info_of(lobby, slot_player(lobby, host)).options;
                const bool may_watch = (host_options & option::watching_allowed) != 0 &&
                                       player.in_use != 0 && player.status == kSlotLocal;
                const bool watching = player.in_use != 0 && (info.options & option::watcher) != 0;
                if (panel.button == 2 && count > OA_SIDE_COUNT) {
                    const uint8_t earlier = lobby_offered_side_before(game, count, computer_hidden);
                    const uint8_t last = earlier < count ? earlier : uint8_t{0};
                    if (watching) {
                        info.options &= static_cast<uint16_t>(~option::watcher);
                        info.side = last;
                    } else {
                        const uint8_t previous =
                            lobby_offered_side_before(game, info.side, computer_hidden);
                        if (previous < count) {
                            info.side = previous;
                        } else if (may_watch) {
                            info.options |= option::watcher;
                        } else {
                            info.side = last;
                            panel_set_stage(panel, name, info.side);
                        }
                    }
                } else if (watching) {
                    info.options &= static_cast<uint16_t>(~option::watcher);
                    info.side = first_offered();
                } else {
                    info.side = lobby_offered_side(game, info.side + 1U, computer_hidden);
                    if (info.side >= count) {
                        info.side = first_offered();
                        if (!may_watch)
                            panel_set_stage(panel, name, info.side);
                        else
                            info.options |= option::watcher;
                    }
                }
                game.gui_flags |= 1U;
                notify(lobby, 4);
                if (host_edits_remote_computer(lobby, slot))
                    queue_computer_edit(lobby, player.player_id, kComputerEditSide, info.side);
                lobby_send_player_info(lobby);
            } else {
                play(lobby, "notoktobuild");
            }
        }

        format(name, "ALLY%d", slot);
        if (panel_selected_is(panel, name)) {
            if (!lobby_launch_locked(lobby)) {
                const auto value = static_cast<uint8_t>(me.alliance[slot] ^ 1U);
                lobby_set_alliance(lobby, me, player, value, false);
                me.alliance[slot] = value;
                if (lobby_player_team(me) != kNoTeam &&
                    lobby_player_team(me) == lobby_player_team(player)) {
                    lobby_leave_team(lobby, local);
                    lobby_player_team(me) = kNoTeam;
                    lobby_send_team(lobby, me);
                }
                const bool quiet = value == 0 && lobby_player_allied_back(me)[slot] * 2U != 3U;
                play(lobby, quiet ? "Multi" : "Ally");
                // Said in English, as English 3.1c sends it; each machine
                // shows it in its own language.
                char line[96];
                std::snprintf(
                    line,
                    sizeof(line),
                    " %s %s",
                    value == 0 ? sim::messages::phrase_broke_alliance_with
                               : sim::messages::phrase_allied_with,
                    std::string(text_view(player.name, sizeof(player.name))).c_str()
                );
                lobby_say(lobby, me, line);
                game.gui_flags |= 1U;
                lobby_send_player_info(lobby);
            } else {
                play(lobby, "notoktobuild");
            }
        }

        format(name, "TEAMICONS%d", slot);
        if (panel_selected_is(panel, name)) {
            if (!lobby_launch_locked(lobby)) {
                play(lobby, "Ally");
                lobby_cycle_team(lobby, panel, slot);
                lobby_send_team(lobby, player);
                if (host_edits_remote_computer(lobby, slot))
                    queue_computer_edit(
                        lobby, player.player_id, kComputerEditTeam, lobby_player_team(player)
                    );
            } else {
                play(lobby, "notoktobuild");
            }
        }

        format(name, "MEM%d", slot);
        if (panel_selected_is(panel, name) && edits_computer(lobby, slot)) {
            play(lobby, "Multi");
            const auto next =
                static_cast<uint8_t>((shown_difficulty(lobby, slot) + 1U) % kDifficultyCount);
            note_computer_difficulty(lobby, slot, next);
            if (!local_or_computer(player))
                queue_computer_edit(lobby, player.player_id, kComputerEditDifficulty, next);
            game.gui_flags |= 1U;
        }

        format(name, "RES%d", slot);
        if (panel_selected_is(panel, name) && occupied_by(player, kSlotLocal)) {
            play(lobby, "Multi");
            lobby_cycle_resolution(lobby, panel.button == 2);
            panel.selected = kNoControl;
            game.gui_flags |= 1U;
            return action;
        }

        format(name, "READY%d", slot);
        if (panel_selected_is(panel, name) && occupied_by(player, kSlotLocal)) {
            play(lobby, "Multi");
            if (!lobby_has_host_map(lobby) || guest_awaits_mutators(lobby)) {
                panel_set_value(panel, name, 0);
            } else {
                const auto* ready = panel_control(panel, name);
                const bool on = ready != nullptr && (ready->value & 1) != 0;
                info.options = static_cast<uint16_t>(
                    (info.options & ~option::ready) | (on ? option::ready : 0)
                );
                for (int32_t other = 0; other < kSlotCount; ++other) {
                    auto& mate = slot_player(lobby, other);
                    if (!local_or_computer(mate))
                        continue;
                    auto& mate_info = info_of(lobby, mate);
                    mate_info.options = static_cast<uint16_t>(
                        (mate_info.options & ~option::ready) |
                        (info_of(lobby, me).options & option::ready)
                    );
                }
                game.gui_flags |= 1U;
                lobby_send_player_info(lobby);
            }
        }
    }
    if (lobby_player_count(game) != kSlotCount)
        game.gui_flags |= 1U;

    auto& mine = info_of(lobby, me);
    const auto finish = [&](bool publish) {
        if (publish) {
            lobby_send_player_info(lobby);
            lobby_publish_session(lobby);
        }
        game.gui_flags |= 1U;
        panel.selected = kNoControl;
    };

    if (panel_selected_is(panel, "PREVMENU")) {
        play(lobby, "Previous");
        for (int32_t slot = 0; slot < kSlotCount; ++slot) {
            auto& player = slot_player(lobby, slot);
            if (local_or_computer(player))
                lobby_reject(lobby, player.player_id, kRejectLeaving);
        }
        game.frontend_pending_signal = 3;
        return LobbyAction::leave;
    }
    for (const auto& button : kModLobbyButtons) {
        if ((lobby.lobby_buttons & button.bit) == 0 || !panel_selected_is(panel, button.name))
            continue;
        const auto* control = panel_control(panel, button.name);
        if (control == nullptr || control->grayed)
            return action;
        play(lobby, "Multi");
        lobby_say(lobby, me, button.line);
        game.gui_flags |= 1U;
        panel.selected = kNoControl;
        return action;
    }
    if (panel_selected_is(panel, "MESSAGE")) {
        auto* entry = panel_control(panel, "MESSAGE");
        const auto text = std::string(control_text(*entry));
        if (!text.empty()) {
            // The battle room's commands are matched without case; paging
            // sends nothing here.
            const bool syncerr = same_nocase(text, "+syncerr");
            const bool command = syncerr || same_nocase(text, "+page") || same_nocase(text, "+p") ||
                                 starts_nocase(text, "+page ") || starts_nocase(text, "+p ");
            if (syncerr) {
                char diagnostic[kChatLineBytes];
                if (const char* line = unit_sync_diagnostic(lobby, diagnostic, sizeof diagnostic))
                    lobby_post_chat(lobby, line);
            } else if (!command) {
                // A profile's setup command runs here and still goes out
                // as a chat line.
                (void)lobby_run_setup_command(lobby, text);
                lobby_say(lobby, me, text.c_str());
            }
            game.gui_flags |= 1U;
            set_control_text(*entry, "");
        }
        panel.focus = panel_find(panel, "MESSAGE");
        panel.selected = kNoControl;
        return action;
    }
    if (panel_selected_is(panel, "COMMANDER")) {
        play(lobby, "Multi");
        auto options = mine.options;
        const auto stepped = ((options & 0xf800U) + option::commander_step) ^ options;
        options = static_cast<uint16_t>((stepped & option::commander_mask) ^ options);
        if ((options & option::commander_mask) > option::commander_deathmatch)
            options &= static_cast<uint16_t>(~option::commander_mask);
        mine.options = options;
        finish(true);
        return action;
    }
    if (panel_selected_is(panel, "LOSTYPE")) {
        play(lobby, "Multi");
        if ((mine.options & option::los_limited) == 0)
            mine.options |= option::los_limited | option::los_true;
        else if ((mine.options & option::los_true) != 0)
            mine.options &= static_cast<uint16_t>(~option::los_true);
        else
            mine.options &= static_cast<uint16_t>(~option::los_limited);
        finish(true);
        return action;
    }
    if (panel_selected_is(panel, "WATCHING")) {
        play(lobby, "Multi");
        mine.options ^= option::watching_allowed;
        if ((mine.options & option::watching_allowed) == 0 && me.in_use != 0 &&
            (mine.options & option::watcher) != 0)
            mine.options &= static_cast<uint16_t>(~option::watcher);
        finish(true);
        return action;
    }
    if (panel_selected_is(panel, "CHEATING") || panel_selected_is(panel, "FIXEDLOC")) {
        play(lobby, "Multi");
        mine.options ^=
            panel_selected_is(panel, "CHEATING") ? option::cheats_allowed : option::fixed_locations;
        lobby_send_player_info(lobby);
        finish(false);
        return action;
    }
    if (panel_selected_is(panel, "MAPPING")) {
        play(lobby, "Multi");
        const auto* mapping = panel_control(panel, "MAPPING");
        const bool unmapped = mapping != nullptr && mapping->stage == 0;
        mine.options = static_cast<uint16_t>(
            (mine.options & ~option::unmapped) | (unmapped ? option::unmapped : 0)
        );
        finish(true);
        return action;
    }
    if (panel_selected_is(panel, "START")) {
        const bool mutators_allow = lobby_mutators_allow_start(lobby);
        const bool limits_allow = lobby_limits_allow_start(lobby);
        const bool sides_allow = lobby_sides_allow_start(lobby);
        if (!lobby_content_allows_start(lobby) || !mutators_allow || !limits_allow ||
            !sides_allow) {
            if (!mutators_allow)
                lobby_note_refused_start(lobby_mutators_block(lobby));
            panel.selected = kNoControl;
            return action;
        }
        if (seat_neutral_computer(lobby, panel)) {
            panel.selected = kNoControl;
            return action;
        }
        play(lobby, "BigButton");
        if (lobby_all_on_one_team(lobby)) {
            panel.selected = kNoControl;
            message(lobby, "Can not start game with all players on the same team.");
            return action;
        }
        if (map_selected(lobby)) {
            if ((mine.options & option::watching_allowed) == 0)
                for (int32_t slot = 0; slot < kSlotCount; ++slot) {
                    auto& player = slot_player(lobby, slot);
                    if (occupied_by(player, kSlotRemote) &&
                        (info_of(lobby, player).options & option::watcher) != 0)
                        lobby_reject(lobby, player.player_id, kRejectWatching);
                }
            game.frontend_pending_signal = 0x11;
            mine.options |= option::started;
            lobby_publish_session(lobby);
            set_host_options(game, mine.options);
            notify(lobby, 10);
            return LobbyAction::start;
        }
        play(lobby, "Multi");
        panel.selected = kNoControl;
        return LobbyAction::select_map;
    }
    if (panel_selected_is(panel, "GAMEOPEN")) {
        play(lobby, "Multi");
        const auto* open = panel_control(panel, "GAMEOPEN");
        const bool closed = open != nullptr && open->stage == 0;
        mine.options = static_cast<uint16_t>(
            (mine.options & ~option::game_closed) | (closed ? option::game_closed : 0)
        );
        finish(true);
        return action;
    }
    if (panel_selected_is(panel, "RESTRICTIONS")) {
        play(lobby, "Options");
        panel.selected = kNoControl;
        return LobbyAction::restrictions;
    }
    if (panel_selected_is(panel, "MAP") || panel_selected_is(panel, "MAPNAME")) {
        play(lobby, "Multi");
        panel.selected = kNoControl;
        if ((mine.role & kRoleHost) == 0 || lobby_launch_locked(lobby))
            return LobbyAction::view_map;
        return LobbyAction::select_map;
    }
    panel.selected = kNoControl;
    return action;
}

/// Broadcasts a ping (0x02) from a player of this machine, stamped with this machine's millisecond clock.
///
/// It goes at once without guaranteed delivery; the echoes fill the Ping
/// column.
///
/// @param[in,out] lobby Lobby state.
/// @param from Local or computer player the ping is from.
void send_ping(Lobby& lobby, uint32_t from) noexcept {
    netgame::PingRecord ping{};
    ping.origin_tick_count = milliseconds(lobby);
    ping.origin_player_id = from;
    uint8_t wire[16];
    std::size_t written = 0;
    if (netgame::encode_record(ping, wire, sizeof(wire), &written) == netgame::WireError::ok)
        send(lobby, from, kBroadcastId, wire, written, true);
}

/// Broadcasts a probe (0x06) from a player of this machine.
///
/// @param[in,out] lobby Lobby state.
/// @param from Local or computer player the probe is from.
void send_probe(Lobby& lobby, uint32_t from) noexcept {
    const auto probe = static_cast<uint8_t>(netgame::RecordType::probe);
    send(lobby, from, kBroadcastId, &probe, 1);
}

/// Runs the battle room's periodic block, about every two seconds.
///
/// For each seated local or computer player in slot order, each record from
/// that player: whatever is queued is flushed, a ping goes out, then a
/// probe; while the player shows no colour the local player asks the host
/// for colour 0 (a host takes it itself when it is free); the host's own
/// player also sends the slot table. Then every such player's lobby block
/// and team, and the machine-group requests, go out as
/// lobby_send_player_info sends them.
///
/// @param[in,out] lobby Lobby state.
void send_periodic_status(Lobby& lobby) noexcept {
    if ((lobby.game->session_flags & kNetFlagLive) == 0)
        return;
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        if (!local_or_computer(player))
            continue;
        flush(lobby);
        send_ping(lobby, player.player_id);
        send_probe(lobby, player.player_id);
        const auto& info = info_of(lobby, player);
        if (info.color == kNoColor)
            lobby_request_color(lobby, 0);
        if ((info.role & kRoleHost) != 0)
            lobby_send_slot_table(lobby);
    }
    lobby_send_player_info(lobby);
}

} // namespace

LobbyAction lobby_handle_event(Lobby& lobby, Panel& panel) noexcept {
    const auto action = handle_panel_event(lobby, panel);
    flush(lobby);
    return action;
}

LobbyAction lobby_tick(Lobby& lobby, Panel& panel, LobbyFront front, Panel* view_map) noexcept {
    auto& game = *lobby.game;
    bool changed = false;
    LobbyEvent event{};
    while (lobby.net.receive != nullptr && lobby.net.receive(lobby.net.context, &event))
        changed = lobby_apply_event(lobby, event) || changed;
    lobby.stalled_player = lobby_check_timeouts(lobby);
    note_shared_machines(game);
    if (changed || !lobby_has_host_map(lobby))
        game.gui_flags |= 1U;

    auto& me = local_player(lobby);
    if (me.reject_reason != 0) {
        game.frontend_pending_signal = 3;
        return LobbyAction::leave;
    }
    // This machine's presence record as it is now, to each OA player who
    // has not had it.
    presence_refresh(lobby);
    presence_send_due(lobby);
    lobby_log_mutators_hold(lobby);
    lobby_note_sides_past_table(lobby);
    if ((game.gui_flags & 1U) != 0) {
        if (!lobby.rows_built)
            lobby_build_rows(lobby, panel);
        else
            lobby_compact_slots(lobby);
        if (lobby_player_count(game) != lobby.last_player_count) {
            lobby.last_player_count = lobby_player_count(game);
            lobby_publish_session(lobby);
        }
        const auto host = host_is_local(lobby) ? kNoSlot : lobby_host_slot(lobby);
        if (host != kNoSlot && front == LobbyFront::battleroom) {
            auto& host_info = info_of(lobby, slot_player(lobby, host));
            if (lobby.maps.select != nullptr)
                lobby.maps.select(lobby.maps.context, host_info.map_name);
            if (auto* units = panel_control(panel, "MAXUNITS"))
                oa::ui::gui_input::scroll_set_value(
                    units->scroll, host_info.max_units - kMaxUnitsFloor
                );
            if (auto* metal = panel_control(panel, "METAL"))
                oa::ui::gui_input::scroll_set_value(metal->scroll, host_info.metal_hundreds * 100);
            if (auto* energy = panel_control(panel, "ENERGY"))
                oa::ui::gui_input::scroll_set_value(
                    energy->scroll, host_info.energy_hundreds * 100
                );
            lobby_on_max_units(panel, &lobby);
            lobby_on_energy(panel, &lobby);
            lobby_on_metal(panel, &lobby);
        } else if (host != kNoSlot && front == LobbyFront::view_map && view_map != nullptr) {
            const auto& host_info = info_of(lobby, slot_player(lobby, host));
            (void)viewmap_follow_host(
                lobby,
                *view_map,
                std::string_view(
                    host_info.map_name, ::strnlen(host_info.map_name, sizeof(host_info.map_name))
                )
            );
        }
        game.gui_flags &= static_cast<uint8_t>(~1U);
        // The battle room's status and doors wait while a dialog is in front.
        if (front == LobbyFront::battleroom) {
            if (host_is_local(lobby)) {
                const bool synced = unit_sync_complete(lobby);
                const bool ready =
                    lobby_ready_to_start(lobby) && lobby_content_allows_start(lobby) &&
                    lobby_mutators_allow_start(lobby) && lobby_limits_allow_start(lobby) &&
                    lobby_sides_allow_start(lobby);
                const auto tick = now(lobby);
                panel_set_grayed(panel, "SYNCHING", true);
                if (lobby.start_frame > 0 && lobby_clock_passed(tick, lobby.start_frame_tick)) {
                    if (lobby.start_frame < kStartFrameStop) {
                        ++lobby.start_frame;
                        panel.dirty = true;
                        game.gui_flags |= 1U;
                    }
                    // A reading below the step's time has turned over to 0
                    // since it was set; the steps go on from that reading.
                    lobby.start_frame_tick =
                        std::min(tick, lobby.start_frame_tick) + kStartFrameTicks;
                    if (lobby.start_frame == kStartFrameSound)
                        play(lobby, "Panel");
                }
                if (lobby.start_frame != 0 && lobby.start_frame < kStartFrames - 1)
                    game.gui_flags |= 1U;
                if (ready) {
                    game.gui_flags |= 1U;
                    if (lobby.start_frame == 0) {
                        lobby.start_frame = 1;
                        lobby.start_frame_tick = tick;
                        play(lobby, "Options");
                    }
                    // The doors open for good and no longer take START's clicks.
                    if (auto* doors = panel_control(panel, "battlestart"))
                        doors->hot = false;
                    if (auto* start = panel_control(panel, "START"))
                        start->light_level = static_cast<uint8_t>(tick & kStartLightMask);
                }
                if (auto* sprite = panel_control(panel, "battlestart"))
                    sprite->stage = static_cast<uint8_t>(lobby.start_frame);
                panel_set_grayed(panel, "START", !ready);
                panel_set_active(panel, "START", synced);
                panel_set_active(panel, "SYNCHING", !synced);
            }
            lobby_update_status(lobby, panel);
            panel.dirty = true;
        }
    }
    unit_sync_tick(lobby);
    if (lobby_clock_passed(now(lobby), lobby.next_stats_tick)) {
        lobby.next_stats_tick = now(lobby) + kStatsInterval;
        mark_local_block(local_info(lobby));
        send_periodic_status(lobby);
    }
    flush(lobby);
    return LobbyAction::none;
}

bool lobby_clock_passed(uint32_t tick, uint32_t due) noexcept {
    return !base::game_loop::scaled_clock_before(tick, due + 1);
}

uint32_t lobby_check_timeouts(Lobby& lobby) noexcept {
    auto& game = *lobby.game;
    if ((game.console_flags & OA_CONSOLE_FLAG_NO_DROP) != 0)
        return lobby.stalled_player;
    const auto tick = now(lobby);
    if ((game.sim_run_flags & kRunFlagPaused) != 0) {
        lobby.timeout_baseline = tick;
        return lobby.stalled_player;
    }
    const auto limit = static_cast<uint32_t>(game.player_timeout_seconds) * kTicksPerSecond;
    // Silence counts from the later of the player's last word and the last
    // pause, counting a turn of the clock to 0 between.
    const auto stalled = [&](const Player& player) {
        const auto silent = std::min(
            base::game_loop::scaled_clock_elapsed(tick, player.last_update_time),
            base::game_loop::scaled_clock_elapsed(tick, lobby.timeout_baseline)
        );
        return occupied_by(player, kSlotRemote) && silent > limit;
    };
    int64_t group = -1;
    for (const auto& player : game.players) {
        if (!stalled(player))
            continue;
        if (group < 0)
            group = player.machine_group;
        else if (group != player.machine_group)
            return 0; // several machines are silent
    }
    for (const auto& player : game.players)
        if (stalled(player))
            return player.player_id;
    return 0;
}

void lobby_leave_battleroom(Lobby& lobby) noexcept {
    lobby_update_ally_matrix(lobby);
    unit_sync_destroy(lobby);
}

// ---------------------------------------------------------------------------
// Records

void lobby_send_player_info(Lobby& lobby) noexcept {
    if ((lobby.game->session_flags & kNetFlagLive) == 0)
        return;
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        if (!local_or_computer(player))
            continue;
        netgame::PlayerInfoRecord record{};
        const auto* bytes = reinterpret_cast<const uint8_t*>(&info_of(lobby, player));
        std::memcpy(record.info_head, bytes, sizeof(record.info_head));
        record.player_id = player.player_id;
        std::memcpy(
            record.info_tail, bytes + netgame::player_info_tail_offset, sizeof(record.info_tail)
        );
        // A recorder stamps its protocol on every battle-room block it sends.
        record.info_tail
            [netgame::player_info_recorder_protocol_offset - netgame::player_info_tail_offset] =
            lobby.wire_rules.recorder_protocol;
        netgame::announce_unicode_chat(record, lobby.unicode_chat);
        // Every block OA sends says so, a computer player's too, so another OA
        // machine can tell it from 3.1c's.
        netgame::stamp_engine_signature(record);
        uint8_t wire[kLobbyRecordBytes];
        std::size_t written = 0;
        if (netgame::encode_record(record, wire, sizeof(wire), &written) == netgame::WireError::ok)
            send(lobby, player.player_id, kBroadcastId, wire, written);
        const auto& teams = teams_rules(lobby).team_number_alliances;
        if (teams.enabled)
            lobby_apply_team_steps(
                lobby,
                team_rules::resend_alliances(
                    lobby_team_slots(lobby),
                    static_cast<uint8_t>(slot),
                    teams.lobby_rebroadcast ==
                            data::match_rules::TeamsTeamNumberAlliancesLobbyRebroadcast::
                                recompute_from_teams
                        ? team_rules::AllianceResend::from_teams
                        : team_rules::AllianceResend::stored
                )
            );
        lobby_send_team(lobby, player);
    }
    lobby_request_machine_groups(lobby);
    flush(lobby);
}

bool lobby_color_available(Lobby& lobby, uint32_t player_id, int32_t color) noexcept {
    if (color == kNoColor || color < 0 || color >= kColorCount)
        return false;
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        if (player.status != kSlotOpen && player.player_id != player_id &&
            info_of(lobby, player).color == static_cast<uint8_t>(color))
            return false;
    }
    return true;
}

bool lobby_assign_free_color(Lobby& lobby, uint32_t player_id, uint32_t color) noexcept {
    auto candidate = color;
    for (int32_t tries = 0;;) {
        if (candidate >= static_cast<uint32_t>(kColorCount))
            candidate = 0;
        bool used = false;
        for (int32_t slot = 0; slot < kSlotCount && !used; ++slot) {
            auto& player = slot_player(lobby, slot);
            used = player.status != kSlotOpen && player.status != kSlotBlocked &&
                   player.player_id != player_id && info_of(lobby, player).color == candidate;
        }
        if (!used)
            break;
        ++candidate;
        if (++tries >= kColorCount)
            break;
    }
    const auto value = static_cast<uint8_t>(candidate);
    if (player_id == local_player(lobby).player_id) {
        local_info(lobby).color = value;
        return true;
    }
    netgame::PlayerValueReplyRecord reply{};
    reply.value = value;
    uint8_t wire[4];
    std::size_t written = 0;
    if (netgame::encode_record(reply, wire, sizeof(wire), &written) != netgame::WireError::ok)
        return false;
    send(lobby, local_player(lobby).player_id, player_id, wire, written);
    flush(lobby);
    const auto slot = slot_for_player_id(lobby, player_id);
    if (slot >= 0)
        info_of(lobby, slot_player(lobby, slot)).color = value;
    return true;
}

void lobby_request_color(Lobby& lobby, uint32_t color) noexcept {
    const auto& me = local_player(lobby);
    const auto host = lobby_host_slot(lobby);
    if (host == lobby.game->local_player_index) {
        if (lobby_color_available(lobby, me.player_id, static_cast<int32_t>(color)))
            local_info(lobby).color = static_cast<uint8_t>(color);
        else
            (void)lobby_assign_free_color(lobby, me.player_id, color);
        return;
    }
    netgame::PlayerValueRequestRecord request{};
    request.value = static_cast<uint8_t>(color);
    uint8_t wire[4];
    std::size_t written = 0;
    if (netgame::encode_record(request, wire, sizeof(wire), &written) != netgame::WireError::ok)
        return;
    send(
        lobby,
        me.player_id,
        host == kNoSlot ? kBroadcastId : slot_player(lobby, host).player_id,
        wire,
        written
    );
    flush(lobby);
}

void lobby_send_team(Lobby& lobby, const Player& player) noexcept {
    if (!local_or_computer(player))
        return;
    netgame::PlayerTeamRecord record{};
    record.player_id = player.player_id;
    record.value = lobby_player_team(const_cast<Player&>(player));
    uint8_t wire[16];
    std::size_t written = 0;
    if (netgame::encode_record(record, wire, sizeof(wire), &written) == netgame::WireError::ok)
        send(lobby, player.player_id, kBroadcastId, wire, written);
    flush(lobby);
}

void lobby_set_alliance(
    Lobby& lobby, Player& from, Player& to, uint8_t value, bool both_sides
) noexcept {
    if (from.index >= kSlotCount || to.index >= kSlotCount)
        return;
    if (local_or_computer(from)) {
        from.alliance[to.index] = value;
        if (occupied_by(to, kSlotComputer) || slot_remote_defeated(lobby, to) || both_sides)
            lobby_player_allied_back(from)[to.index] = value;
    }
    if (local_or_computer(to)) {
        lobby_player_allied_back(to)[from.index] = value;
        if (occupied_by(to, kSlotComputer) || both_sides)
            to.alliance[from.index] = value;
    } else if (occupied_by(to, kSlotRemote)) {
        lobby_send_alliance(lobby, from, to, value, both_sides ? 1U : 0U);
    }
    // The game reports only in a multiplayer session, which the lobby and
    // the in-game allies panel always are.
    notify(lobby, 4);
}

team_rules::TeamSlots lobby_team_slots(Lobby& lobby) noexcept {
    team_rules::TeamSlots slots{};
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        const auto& info = info_of(lobby, player);
        auto& view = slots[static_cast<std::size_t>(slot)];
        view.in_use = player.in_use != 0;
        view.status = player.status;
        view.watcher = (info.options & option::watcher) != 0;
        view.setup_state = info.state;
        view.team = static_cast<int8_t>(lobby_player_team(player));
        std::memcpy(view.alliance.data(), player.alliance, view.alliance.size());
        static_assert(sizeof(player.name) <= sizeof(view.name));
        std::memcpy(view.name.data(), player.name, sizeof(player.name));
    }
    return slots;
}

void lobby_announce_alliance(Lobby& lobby, Player& from, Player& to, uint8_t value) noexcept {
    if (from.index >= kSlotCount || to.index >= kSlotCount)
        return;
    netgame::AllianceRecord record{};
    record.player_id_a = from.player_id;
    record.player_id_b = to.player_id;
    record.value = value;
    uint8_t wire[32];
    std::size_t written = 0;
    if (local_or_computer(from)) {
        from.alliance[to.index] = value;
        if (netgame::encode_record(record, wire, sizeof(wire), &written) == netgame::WireError::ok)
            send(lobby, from.player_id, kBroadcastId, wire, written);
        // A player on this machine takes it as it would from the record.
        if (local_or_computer(to))
            lobby_set_alliance(lobby, from, to, value, false);
    } else {
        record.both_sides = team_rules::alliance_request;
        if (netgame::encode_record(record, wire, sizeof(wire), &written) == netgame::WireError::ok)
            send(lobby, local_player(lobby).player_id, from.player_id, wire, written);
    }
    flush(lobby);
    lobby.game->gui_flags |= 1U;
}

void lobby_apply_team_steps(Lobby& lobby, const team_rules::TeamSteps& steps) noexcept {
    for (uint16_t i = 0; i < steps.count; ++i) {
        const auto& step = steps.items[i];
        auto& from = slot_player(lobby, step.from);
        if (step.kind == team_rules::TeamStep::Kind::alliance) {
            lobby_announce_alliance(lobby, from, slot_player(lobby, step.to), step.value);
            continue;
        }
        lobby_player_team(from) = step.value;
        netgame::PlayerTeamRecord record{};
        record.player_id = from.player_id;
        record.value = static_cast<uint8_t>(step.value | team_rules::team_keeps_alliances);
        uint8_t wire[16];
        std::size_t written = 0;
        if (netgame::encode_record(record, wire, sizeof(wire), &written) == netgame::WireError::ok)
            send(lobby, local_player(lobby).player_id, kBroadcastId, wire, written);
        flush(lobby);
    }
    lobby.game->gui_flags |= 1U;
}

namespace {

/// Draws for the team deal through the lobby's services.
///
/// @param context The lobby.
/// @param bound Exclusive upper limit.
/// @return The draw.
uint32_t lobby_random_below(void* context, uint32_t bound) {
    auto& lobby = *static_cast<Lobby*>(context);
    return lobby.services.random_below(lobby.services.context, bound);
}

/// Deals the counted players into teams in a random order (+autoteam, +randomteam).
///
/// @param[in,out] lobby Lobby state.
/// @param command The command's name, for its refusal.
/// @param argument The command's argument.
/// @param notice The notice posted as the teams are dealt.
void deal_random_teams(
    Lobby& lobby, std::string_view command, std::string_view argument, const char* notice
) noexcept {
    if (local_player(lobby).machine_group != kHostMachineGroup) {
        char line[kChatLineBytes];
        std::snprintf(
            line,
            sizeof line,
            "%.*s can only be used by host",
            static_cast<int>(command.size()),
            command.data()
        );
        lobby_post_chat(lobby, line);
        return;
    }
    const int32_t teams = team_rules::dealt_team_count(argument);
    lobby_post_chat(lobby, notice);
    std::array<int32_t, team_rules::slot_count> order{0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
    team_rules::ShuffleRandom random{};
    if (lobby.services.random_below != nullptr)
        random = {&lobby, lobby_random_below};
    team_rules::shuffle_slots(order, random);
    lobby_apply_team_steps(lobby, team_rules::deal_teams(lobby_team_slots(lobby), order, teams));
}

} // namespace

bool lobby_run_setup_command(Lobby& lobby, std::string_view text) noexcept {
    const auto space = text.find(' ');
    const auto command = text.substr(0, space);
    auto argument = space == std::string_view::npos ? std::string_view{} : text.substr(space + 1);
    while (!argument.empty() && argument.front() == ' ')
        argument.remove_prefix(1);
    const auto is = [&](std::string_view word) {
        return command.size() == word.size() && starts_nocase(command, word);
    };
    if (lobby.rules == nullptr)
        return false;
    if (lobby.rules->teams.team_number_alliances.enabled) {
        if (is("+autoteam")) {
            deal_random_teams(
                lobby, "+autoteam", argument, "Autobalance not available. Setting random teams"
            );
            return true;
        }
        if (is("+randomteam")) {
            deal_random_teams(lobby, "+randomteam", argument, "Setting random teams");
            return true;
        }
    }
    if (lobby.rules->setup.map_scripted_units.enabled) {
        if (is("+spawnoff")) {
            lobby.map_units_on = false;
            lobby_post_chat(lobby, "Unit spawn is disabled ...");
            return true;
        }
        if (is("+spawnon")) {
            lobby.map_units_on = true;
            lobby_post_chat(lobby, "Unit spawn is enabled ...");
            return true;
        }
    }
    return false;
}

void lobby_send_alliance(
    Lobby& lobby, const Player& from, const Player& to, uint8_t value, uint32_t both_sides
) noexcept {
    netgame::AllianceRecord record{};
    record.player_id_a = from.player_id;
    record.player_id_b = to.player_id;
    record.value = value;
    record.both_sides = both_sides;
    uint8_t wire[32];
    std::size_t written = 0;
    if (netgame::encode_record(record, wire, sizeof(wire), &written) == netgame::WireError::ok)
        send(lobby, from.player_id, to.player_id, wire, written);
    flush(lobby);
}

namespace {

/// Returns the first seated local or computer player in slot order, which
/// the records of a rejection leave from.
///
/// @param lobby Lobby state.
/// @param gone A player that has already left the game and no longer
///        counts; null for none.
/// @return The player, or null when none is seated.
const Player* first_seated_here(Lobby& lobby, const Player* gone) noexcept {
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        const auto& player = slot_player(lobby, slot);
        if (&player != gone && local_or_computer(player))
            return &player;
    }
    return nullptr;
}

/// Broadcasts 0x1b {id, reason} from a player of this machine.
///
/// Nothing is sent without a sender or from one already rejected, since a
/// rejected player sends nothing.
///
/// @param[in,out] lobby Lobby state.
/// @param from Sending player; null sends nothing.
/// @param player_id Player the record names.
/// @param reason RejectReason value.
void send_reject(Lobby& lobby, const Player* from, uint32_t player_id, uint8_t reason) noexcept {
    if (from == nullptr || from->reject_reason != 0)
        return;
    netgame::RejectRecord record{};
    record.player_id = player_id;
    record.reason = reason;
    uint8_t wire[16];
    std::size_t written = 0;
    if (netgame::encode_record(record, wire, sizeof(wire), &written) == netgame::WireError::ok)
        send(lobby, from->player_id, kBroadcastId, wire, written);
}

/// Takes a rejected player out of the game: it is no longer counted
/// (Game.player_count) and, unless it is this machine's own after the
/// start, its slot opens; before the start a computer player of this
/// machine also leaves the session.
///
/// @param[in,out] lobby Lobby state.
/// @param[in,out] player Player leaving.
void retire_slot(Lobby& lobby, Player& player) noexcept {
    auto& game = *lobby.game;
    const bool started = (game.session_flags & kNetFlagGameStarted) != 0;
    if (!started || !local_or_computer(player)) {
        if (!started && occupied_by(player, kSlotComputer) && lobby.net.remove_player != nullptr)
            lobby.net.remove_player(lobby.net.context, player.player_id);
        clear_computer_difficulty(lobby, static_cast<int32_t>(&player - game.players));
        slot_set_status(lobby, player, kSlotOpen);
        player.in_use = 0;
        player.machine_group = 0;
    }
    if (lobby_player_count(game) > 0)
        lobby_player_count(game) = static_cast<uint16_t>(lobby_player_count(game) - 1);
}

} // namespace

void lobby_reject(Lobby& lobby, uint32_t player_id, uint8_t reason) noexcept {
    const auto slot = slot_for_player_id(lobby, player_id);
    if (slot < 0)
        return;
    auto& player = slot_player(lobby, slot);
    if (player.reject_reason == 0) {
        if (occupied_by(player, kSlotLocal)) {
            // Each local and computer player of this machine is named in
            // slot order, from the first one still seated. Before the start
            // the local player leaves the game at the first of these, so
            // the next seated player sends the rest and the local player is
            // not named after a computer player seated ahead of it; after
            // the start it stays seated, and once rejected sends no more.
            const bool started = (lobby.game->session_flags & kNetFlagGameStarted) != 0;
            const Player* gone = nullptr;
            for (int32_t other = 0; other < kSlotCount; ++other) {
                auto& mate = slot_player(lobby, other);
                if (!local_or_computer(mate) || &mate == gone)
                    continue;
                send_reject(lobby, first_seated_here(lobby, gone), mate.player_id, reason);
                if (!started)
                    gone = &player;
                mate.reject_reason = reason;
            }
        } else if (occupied_by(player, kSlotComputer)) {
            send_reject(lobby, first_seated_here(lobby, nullptr), player_id, reason);
            retire_slot(lobby, player);
        } else if (occupied_by(player, kSlotRemote)) {
            send_reject(lobby, first_seated_here(lobby, nullptr), player_id, reason);
            if (slot_remote_playing(lobby, player) && player.machine_group != 0) {
                // A human still playing takes its machine's other players
                // with it: every player of its machine group. A player whose
                // machine group is not known yet takes only itself.
                const auto group = player.machine_group;
                for (int32_t other = 0; other < kSlotCount; ++other) {
                    auto& mate = slot_player(lobby, other);
                    if (!occupied_by(mate, kSlotRemote) || mate.machine_group != group)
                        continue;
                    retire_slot(lobby, mate);
                    mate.reject_reason = reason;
                }
            } else {
                retire_slot(lobby, player);
            }
        }
    }
    player.reject_reason = reason;
}

void lobby_post_chat(Lobby& lobby, const char* line) noexcept {
    auto& game = *lobby.game;
    const uint16_t head = lobby_chat_head(game);
    // A line keeps what fits, less a UTF-8 character the cut would split.
    const std::string_view text(line, ::strnlen(line, kChatLineBytes + kLongestCharacter));
    copy_text(
        lobby_chat_line(game, head),
        kChatLineBytes,
        text.substr(0, base::text::whole_characters(text, kChatLineBytes - 1))
    );
    auto next = static_cast<uint16_t>((head + 1U) % kChatLines);
    lobby_chat_head(game) = next;
    if (next == static_cast<uint16_t>(lobby_chat_tail(game)))
        lobby_chat_tail(game) = static_cast<uint16_t>((next + 1U) % kChatLines);
    game.gui_flags |= 1U;
}

void lobby_say(Lobby& lobby, const Player& speaker, const char* text) noexcept {
    std::string line = "<";
    line += text_view(speaker.name, sizeof(speaker.name));
    line += "> ";
    line += text;
    if (lobby.unicode_chat) {
        // Each part shows here as it shows on the other machines.
        const auto parts = say_unicode(
            lobby, speaker.player_id, line.c_str(), sizeof(netgame::ChatRecord::text), true
        );
        flush(lobby);
        for (const auto& part : parts)
            post_shown_chat(lobby, part.c_str());
    } else {
        // The record carries the line's first 64 bytes, the rest zero, with
        // no terminator when the line fills it; a UTF-8 character the 64th
        // byte would split is left out whole.
        netgame::ChatRecord record{};
        std::memcpy(
            record.text, line.data(), base::text::whole_characters(line, sizeof(record.text))
        );
        uint8_t wire[80];
        std::size_t written = 0;
        if (netgame::encode_record(record, wire, sizeof(wire), &written) == netgame::WireError::ok)
            send(lobby, speaker.player_id, kBroadcastId, wire, written);
        flush(lobby);
        post_shown_chat(lobby, line.c_str());
    }
    recorder_chat_line(lobby, slot_for_player_id(lobby, speaker.player_id), line.c_str());
}

void lobby_session_description(Lobby& lobby, char* name, uint8_t* user) noexcept {
    auto& game = *lobby.game;
    auto& info = local_info(lobby);
    // While a launch is active the session is marked launch-only and its
    // version biased.
    const bool launched = lobby_launch_active(lobby) && lobby.wire_rules.launch_version_bias;
    info.version_major = static_cast<uint8_t>(
        static_cast<int32_t>(lobby.local_version_major) + (launched ? kLaunchVersionBias : 0)
    );
    info.options =
        static_cast<uint16_t>((info.options & ~0xfU) | (lobby_player_count(game) & 0xfU));
    info.status = static_cast<uint16_t>(
        (info.status & ~status::launch_only) | (launched ? status::launch_only : 0)
    );
    std::memcpy(
        user, reinterpret_cast<const uint8_t*>(&info) + kSessionUserInfoOffset, kSessionUserBytes
    );
    std::memset(name, ' ', kSessionNameBytes);
    const auto game_name = text_view(lobby_game_name(game), kSessionGameNameBytes);
    std::memcpy(name, game_name.data(), game_name.size());
    const auto map = text_view(map_name(lobby), kSessionMapNameBytes);
    std::memcpy(name + kSessionGameNameBytes, map.data(), map.size());
    name[kSessionNameBytes - 1] = '\0';
}

void lobby_publish_session(Lobby& lobby) noexcept {
    if (lobby.net.describe == nullptr || !host_is_local(lobby))
        return;
    char name[kSessionNameBytes];
    uint8_t user[kSessionUserBytes];
    lobby_session_description(lobby, name, user);
    const auto& info = local_info(lobby);
    // A password is checked against the joiner's tag, never flagged on the
    // session.
    uint32_t flags = kSessionFlagGame;
    if ((info.options & option::started) != 0)
        flags |= kSessionFlagStarted;
    const auto players = static_cast<int32_t>(lobby_session_players(*lobby.game));
    lobby.net.describe(
        lobby.net.context, name, user, flags, static_cast<uint32_t>(std::max(players, 0))
    );
}

void lobby_send_slot_table(Lobby& lobby) noexcept {
    if ((lobby.game->session_flags & kNetFlagLive) == 0 || !host_is_local(lobby))
        return;
    auto& game = *lobby.game;
    netgame::SlotTableRecord record{};
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        const auto& player = slot_player(lobby, slot);
        game.slot_table[slot] = player.status == kSlotBlocked ? kNoPlayerId
                                : player.status == kSlotOpen  ? 0U
                                                              : player.player_id;
        record.slot_ids[slot] = game.slot_table[slot];
    }
    uint8_t wire[64];
    std::size_t written = 0;
    const auto host = lobby_host_slot(lobby);
    if (netgame::encode_record(record, wire, sizeof(wire), &written) == netgame::WireError::ok)
        send(lobby, slot_player(lobby, host).player_id, kBroadcastId, wire, written);
}

void lobby_set_computer_brain(
    Lobby& lobby, int32_t slot, std::optional<ComputerBrain> brain, uint8_t difficulty
) noexcept {
    if (slot < 0 || slot >= kSlotCount || lobby.game == nullptr)
        return;
    if (!occupied_by(slot_player(lobby, slot), kSlotComputer))
        return;
    lobby.computer_brain[slot] = std::move(brain);
    note_computer_difficulty(lobby, slot, difficulty);
}

const ComputerBrain* lobby_computer_brain(const Lobby& lobby, int32_t slot) noexcept {
    if (slot < 0 || slot >= kSlotCount || !lobby.computer_brain[slot])
        return nullptr;
    return &*lobby.computer_brain[slot];
}

void lobby_add_computer(Lobby& lobby, int32_t slot) noexcept {
    auto& game = *lobby.game;
    auto& player = slot_player(lobby, slot);
    slot_set_status(lobby, player, kSlotComputer);
    char name[kComputerNameBytes + 1]{};
    const auto& naming = setup_rules(lobby).ai_player_name_format;
    if (naming.enabled) {
        // setup.ai-player-name-format: the profile's format, with the local
        // player's name and this slot, cut to the field.
        static_assert(kComputerNameBytes == team_rules::computer_name_bytes);
        team_rules::format_computer_name(
            name, naming.format.view(), local_player(lobby).name, slot
        );
    } else if (std::snprintf(name, sizeof(name), "AI:%s", local_player(lobby).name) < 0) {
        // The name is cut to the field; a failed format leaves it empty.
        name[0] = '\0';
    }
    auto& info = info_of(lobby, player);
    const auto host_role = slot == lobby_host_slot(lobby) ? kRoleHost : 0;
    info.role = static_cast<uint8_t>((info.role & ~kRoleHost) | host_role);
    info.color = kNoColor;
    player.machine_flags =
        static_cast<uint8_t>(player.machine_flags & ~kMachineFlagJoinedWithoutBattleroom);
    lobby_player_join_tick(player) = now(lobby);
    const auto count = static_cast<uint16_t>(lobby_player_count(game) & option::player_count_mask);
    info.options = static_cast<uint16_t>(
        (info.options & ~(option::player_count_mask | option::game_closed)) | count
    );
    info.max_units = kSeatedMaxUnits;
    // A computer player reports its machine's memory, as the local player
    // does, so no machine marks it short of the map's memory.
    info.memory_mb = local_info(lobby).memory_mb;
    const auto password = lobby_password(game)[0] != '\0' ? status::password : 0;
    const auto launch_only = lobby_launch_active(lobby) ? status::launch_only : 0;
    info.status = static_cast<uint16_t>(
        (info.status & ~(status::password | status::launch_only)) | password | launch_only
    );
    info.screen_width = static_cast<uint16_t>(static_cast<int32_t>(lobby_screen_width(game)));
    info.screen_height = static_cast<uint16_t>(static_cast<int32_t>(lobby_screen_height(game)));
    info.version_major = static_cast<uint8_t>(lobby.local_version_major);
    info.version_minor = static_cast<uint8_t>(lobby.local_version_minor);
    uint32_t id = 0;
    if (lobby.net.add_player == nullptr ||
        lobby.net.add_player(lobby.net.context, name, lobby_password(game), &id) !=
            LobbyResult::ok) {
        slot_set_status(lobby, player, kSlotOpen);
        message(
            lobby,
            host_is_local(lobby) ? "Direct Play failed to add new player.\n\nRecommended you go to "
                                   "previous screen and re-create the game session.\n"
                                 : "Direct Play failed to add new player.\n\nRecommended you go to "
                                   "previous screen and re-join the game session.\n"
        );
        return;
    }
    player.in_use = 0;
    player.player_id = id;
    player.index = static_cast<uint8_t>(slot);
    player.machine_group = 0;
    // The row carries the session name, which names the human who runs it.
    std::snprintf(player.name, sizeof(player.name), "%s", name);
    copy_text(
        player.second_name, sizeof(player.second_name), text_view(player.name, sizeof(player.name))
    );
    note_computer_difficulty(lobby, slot, kDifficultyHard);
    const uint32_t computer_hidden = lobby_computer_hidden_sides();
    if (computer_hidden != 0 && info.side < 32 && (computer_hidden & (1U << info.side)) != 0) {
        const uint8_t offered = lobby_offered_side(game, 0, computer_hidden);
        if (offered < lobby_side_count(game))
            info.side = offered;
    }
}

// ---------------------------------------------------------------------------
// Machine groups

void note_shared_machines(Game& game) noexcept {
    uint32_t members[kMachineGroupCount + 1]{};
    game.shared_machines = 0;
    for (const auto& player : game.players) {
        const auto group = player.machine_group;
        if (!slot_active(player) || group == 0 || group > kMachineGroupCount)
            continue;
        if (++members[group] > 1) {
            game.shared_machines = 1;
            return;
        }
    }
}

uint32_t free_machine_group(const Game& game) noexcept {
    for (uint32_t group = 1; group <= kMachineGroupCount; ++group) {
        bool held = false;
        for (const auto& player : game.players)
            held = held || (slot_active(player) && player.machine_group == group);
        if (!held)
            return group;
    }
    return 0;
}

int32_t machine_broadcast_targets(const Game& game, uint32_t (&ids)[kSlotCount]) noexcept {
    bool reached[kMachineGroupCount + 1]{};
    int32_t count = 0;
    for (const auto& player : game.players) {
        const auto group = player.machine_group;
        if (!occupied_by(player, kSlotRemote) || (group <= kMachineGroupCount && reached[group]))
            continue;
        ids[count++] = player.player_id;
        if (group < kMachineGroupCount)
            reached[group] = true;
    }
    return count;
}

void lobby_request_machine_groups(Lobby& lobby) noexcept {
    const bool hosting = host_is_local(lobby);
    const auto host = lobby_host_slot(lobby);
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        if (!slot_active(player) || player.machine_group != 0)
            continue;
        netgame::MachineGroupRequestRecord request{};
        request.player_id = player.player_id;
        request.same_machine_id = kNoPlayerId;
        if (local_or_computer(player)) {
            if (hosting) {
                player.machine_group = kHostMachineGroup;
                continue;
            }
            request.assign = 1;
            if (player.status == kSlotComputer)
                request.same_machine_id = local_player(lobby).player_id;
        }
        // Only a remote host can be asked; the host's own requests go nowhere.
        if (host == kNoSlot || !occupied_by(slot_player(lobby, host), kSlotRemote))
            continue;
        uint8_t wire[16];
        std::size_t written = 0;
        if (netgame::encode_record(request, wire, sizeof(wire), &written) == netgame::WireError::ok)
            send(lobby, primary_id(lobby), slot_player(lobby, host).player_id, wire, written);
    }
}

namespace {

/// Answers a 0x21 request on the host, broadcasting the group as 0x22.
///
/// Assign 0 reports the asking player's group; assign 1 gives it the group
/// of the player named as sharing its machine, or a new one when none is
/// named and it has none yet.
///
/// @param[in,out] lobby Hosting lobby state.
/// @param request The received request.
/// @return False when not hosting, the player is unknown, the named machine mate is inactive or no group
///         could be given.
bool assign_machine_group(
    Lobby& lobby, const netgame::MachineGroupRequestRecord& request
) noexcept {
    const auto slot = slot_for_player_id(lobby, request.player_id);
    if (!host_is_local(lobby) || slot < 0)
        return false;
    auto& player = slot_player(lobby, slot);
    uint32_t group = 0;
    if (request.assign == 0) {
        group = player.machine_group;
    } else {
        const auto same = slot_for_player_id(lobby, request.same_machine_id);
        if (same < 0) {
            if (player.machine_group != 0)
                return false;
            group = free_machine_group(*lobby.game);
        } else {
            const auto& other = slot_player(lobby, same);
            if (!slot_active(other))
                return false;
            group = other.machine_group;
        }
        if (group != 0)
            player.machine_group = group;
    }
    if (group == 0)
        return false;
    netgame::MachineGroupReplyRecord reply{};
    reply.player_id = player.player_id;
    reply.machine_group = static_cast<uint8_t>(group);
    uint8_t wire[16];
    std::size_t written = 0;
    if (netgame::encode_record(reply, wire, sizeof(wire), &written) == netgame::WireError::ok)
        send(
            lobby, slot_player(lobby, lobby_host_slot(lobby)).player_id, kBroadcastId, wire, written
        );
    return true;
}

/// The line a room with mutators posts about a 3.1c player it refuses.
constexpr const char* kClassicRefusedLine =
    "*** {player} plays Total Annihilation 3.1c and cannot join a game with mutators.";

/// Tells whether a remote player's block lacks the engine signature while
/// the host's record carries mutators.
///
/// @param lobby Lobby state.
/// @param slot Player slot of a remote player.
/// @return True when the room's mutators cannot take that player.
bool classic_in_mutator_room(Lobby& lobby, int32_t slot) noexcept {
    const auto host = lobby_host_slot(lobby);
    if (host == kNoSlot || !occupied_by(slot_player(lobby, slot), kSlotRemote))
        return false;
    const auto host_record = lobby_presence_record(lobby, static_cast<int32_t>(host));
    const auto* info = slot_info(lobby, slot);
    return netgame::presence_mutators_state(host_record.data(), host_record.size()).has_value() &&
           info != nullptr &&
           !netgame::sent_by_open_annihilation(reinterpret_cast<const uint8_t*>(info));
}

} // namespace

int32_t slot_for_player_id(Lobby& lobby, uint32_t player_id) noexcept {
    for (int32_t slot = 0; slot < kSlotCount; ++slot) {
        auto& player = slot_player(lobby, slot);
        if (player.in_use != 0 && player.player_id == player_id)
            return slot;
    }
    return -1;
}

bool lobby_add_player(Lobby& lobby, uint32_t player_id, const char* name) noexcept {
    auto& game = *lobby.game;
    auto slot = netgame::player_slot_of(game, player_id);
    if (slot == netgame::no_player_slot) {
        slot = netgame::free_player_slot(game);
        if (slot == netgame::no_player_slot)
            return false;
        game.players[slot].status = kSlotRemote;
    } else {
        const auto& seated = game.players[slot];
        if ((seated.status != kSlotLocal && seated.status != kSlotComputer) || seated.in_use != 0)
            return false;
    }
    auto& player = game.players[slot];
    // Its block may still hold an earlier player's bytes until its own comes.
    presence_forget(lobby.presence_records, slot);
    lobby.limits_older_refused[slot] = false;
    copy_text(
        player.name,
        sizeof(player.name),
        text_view(name != nullptr ? name : "", sizeof(LobbyEvent{}.name))
    );
    // The session's short name goes to Player.second_name (the name chat lines
    // carry) and its long name to Player.name; the session has one name
    // for both.
    copy_text(
        player.second_name, sizeof(player.second_name), text_view(player.name, sizeof(player.name))
    );
    player.in_use = 1;
    player.index = slot;
    slot_set_status(lobby, player, player.status);
    auto& info = info_of(lobby, player);
    if (player.status == kSlotRemote) {
        info.state = kInfoStatePlaying;
        info.color = kNoColor;
    }
    player.machine_group = 0;
    info.options = static_cast<uint16_t>(info.options & ~option::ready);
    player.reject_reason = 0;
    player.player_id = player_id;
    player.last_update_time = now(lobby);
    lobby_player_count(game) = static_cast<uint16_t>(lobby_player_count(game) + 1);
    // A player joining takes back the prebuilt bases the host offered.
    auto& base = lobby.recorder.base;
    const auto local = game.local_player_index;
    if (player.status == kSlotRemote && local < kSlotCount && base.available[local]) {
        if (host_is_local(lobby))
            recorder_say(lobby, "New player. Quick base toggled off");
        std::fill(std::begin(base.available), std::end(base.available), false);
    }
    lobby_send_player_info(lobby);
    return true;
}

bool lobby_apply_event(Lobby& lobby, const LobbyEvent& event) noexcept {
    auto& game = *lobby.game;
    switch (event.kind) {
    case LobbyEventKind::player_joined:
        if (event.refuse_reason != 0) {
            // A refused joiner is told with this machine's lobby blocks, then
            // its rejection; it is not seated and leaves by itself. Records
            // that go out per machine reach it as well.
            lobby.refused_joiner = event.player_id;
            lobby_send_player_info(lobby);
            send_reject(
                lobby, first_seated_here(lobby, nullptr), event.player_id, event.refuse_reason
            );
            flush(lobby);
            lobby.refused_joiner = 0;
            return false;
        }
        return lobby_add_player(lobby, event.player_id, event.name);
    case LobbyEventKind::player_left: {
        const auto slot = slot_for_player_id(lobby, event.player_id);
        if (slot < 0)
            return false;
        presence_forget(lobby.presence_records, slot);
        lobby.limits_older_refused[slot] = false;
        auto& player = slot_player(lobby, slot);
        // The creator leaving before the start ends the game here: its
        // player and this machine's leave with reason 10.
        const bool creator_left = (game.session_flags & kNetFlagGameStarted) == 0 &&
                                  player.status == kSlotRemote &&
                                  (info_of(lobby, player).role & kRoleHost) != 0;
        lobby_reject(lobby, event.player_id, kRejectPlayer);
        if (creator_left)
            lobby_reject(lobby, local_player(lobby).player_id, kRejectCreatorLeft);
        flush(lobby);
        // A slot the rejection did not retire (one already rejected) is
        // retired here.
        if (player.in_use != 0) {
            clear_computer_difficulty(lobby, static_cast<int32_t>(&player - game.players));
            slot_set_status(lobby, player, kSlotOpen);
            player.in_use = 0;
            if (lobby_player_count(game) > 0)
                lobby_player_count(game) = static_cast<uint16_t>(lobby_player_count(game) - 1);
        }
        player.index = kNoSlot;
        player.machine_group = 0;
        return true;
    }
    case LobbyEventKind::session_lost:
        local_player(lobby).reject_reason = 6;
        return true;
    case LobbyEventKind::record:
        break;
    default:
        return false;
    }
    if (event.size == 0)
        return false;
    // Records from this machine's own players are not heard; any other
    // seated sender was heard from now.
    if (const auto sender = slot_for_player_id(lobby, event.player_id); sender >= 0) {
        auto& from = slot_player(lobby, sender);
        if (local_or_computer(from))
            return false;
        from.last_update_time = now(lobby);
    }
    const auto type = static_cast<netgame::RecordType>(event.data[0]);
    switch (type) {
    case netgame::RecordType::chat: {
        // Private messages are for the match; the battle room shows none.
        if (lobby.wire_rules.private_channel != netgame::PrivateChannel::none &&
            netgame::is_private_chat(event.data, event.size))
            return false;
        netgame::ChatRecord record{};
        if (netgame::decode_record(event.data, event.size, &record) != netgame::WireError::ok)
            return false;
        char line[sizeof(record.text) + 1];
        std::memcpy(line, record.text, sizeof(record.text));
        line[sizeof(record.text)] = '\0';
        // A line from a machine that sends UTF-8 is read strictly as UTF-8.
        const auto sender = slot_for_player_id(lobby, event.player_id);
        if (lobby.unicode_chat && slot_reads_unicode_chat(lobby, sender))
            post_shown_chat(lobby, netgame::chat_strict_utf8(line).c_str());
        else
            post_shown_chat(lobby, line);
        recorder_chat_line(lobby, sender, line);
        return true;
    }
    case netgame::RecordType::player_info: {
        netgame::PlayerInfoRecord record{};
        if (netgame::decode_record(event.data, event.size, &record) != netgame::WireError::ok)
            return false;
        const auto slot = slot_for_player_id(lobby, record.player_id);
        if (slot < 0 || slot_player(lobby, slot).status != kSlotRemote)
            return false;
        // Read before the block is copied: a later block may not repeat the
        // host bit, and the copy replaces the setup info.
        const auto known_host = lobby_host_slot(lobby);
        auto* bytes = reinterpret_cast<uint8_t*>(slot_info(lobby, slot));
        std::memcpy(bytes, record.info_head, sizeof(record.info_head));
        std::memcpy(
            bytes + offsetof(PlayerSetupInfo, net_id), &record.player_id, sizeof(record.player_id)
        );
        std::memcpy(
            bytes + netgame::player_info_tail_offset, record.info_tail, sizeof(record.info_tail)
        );
        // A block from a player this machine has had none from: the player arrives.
        const bool arriving = lobby.presence_records.block_player[slot] != record.player_id;
        lobby.presence_records.block_player[slot] = record.player_id;
        // A 3.1c joiner cannot play a mutator room. Reason 8 is the text
        // that machine already shows. The line stays on this machine.
        // A later block (ready, side, colour) is not an arrival. Only the
        // host judges: a guest's first block from a player may be the
        // greeting of one seated before the list, so a guest names the
        // joiner when the host's refusal comes (the reject case below).
        if (arriving && host_is_local(lobby) && classic_in_mutator_room(lobby, slot)) {
            lobby_post_classic_line(lobby, kClassicRefusedLine, slot);
            lobby_reject(lobby, record.player_id, kRejectVersion);
            flush(lobby);
            return true;
        }
        // A 3.1c joiner cannot play a mod past 3.1c's limits. The mutator
        // line keeps its place when both apply. An OA block is seated: its
        // record has not arrived, so it is not refused here.
        if (arriving && host_is_local(lobby) && lobby_room_oa_only(lobby)) {
            const auto* arrived = slot_info(lobby, slot);
            if (arrived != nullptr &&
                !netgame::sent_by_open_annihilation(reinterpret_cast<const uint8_t*>(arrived))) {
                lobby_post_limits_line(lobby, lobby_limits_classic_line(lobby), slot);
                lobby_reject(lobby, record.player_id, kRejectVersion);
                flush(lobby);
                return true;
            }
        }
        // This machine's mod is OA-only and the host plays 3.1c. Leave with
        // the ordinary reason, and send no presence record to that host.
        if (!host_is_local(lobby) && lobby_room_oa_only(lobby)) {
            const auto host_now = lobby_host_slot(lobby);
            const bool from_host =
                (known_host != kNoSlot && slot == static_cast<int32_t>(known_host)) ||
                (host_now != kNoSlot && slot == static_cast<int32_t>(host_now));
            const auto* host_info = slot_info(lobby, slot);
            if (from_host && host_info != nullptr &&
                !netgame::sent_by_open_annihilation(reinterpret_cast<const uint8_t*>(host_info))) {
                if (lobby.leave_line == nullptr) {
                    lobby.leave_line = lobby_limits_leave_line();
                    lobby_post_limits_line(lobby, lobby.leave_line, -1);
                }
                for (int32_t mine = 0; mine < kSlotCount; ++mine) {
                    auto& seated = slot_player(lobby, mine);
                    if (local_or_computer(seated))
                        lobby_reject(lobby, seated.player_id, kRejectLeaving);
                }
                flush(lobby);
                return true;
            }
        }
        if (const auto* echoed = slot_info(lobby, slot)) {
            note_computer_command_echo(lobby, record.player_id, kComputerEditSide, echoed->side);
            note_computer_command_echo(lobby, record.player_id, kComputerEditColor, echoed->color);
        }
        note_shared_machines(game);
        recorder_note_block(lobby, slot);
        // A player who arrived, or whose block now says OA, gets this
        // machine's presence record once.
        presence_send_due(lobby, slot);
        lobby_note_classic_room(lobby);
        return true;
    }
    case netgame::RecordType::machine_group_request: {
        netgame::MachineGroupRequestRecord record{};
        if (netgame::decode_record(event.data, event.size, &record) != netgame::WireError::ok)
            return false;
        return assign_machine_group(lobby, record);
    }
    case netgame::RecordType::slot_table: {
        netgame::SlotTableRecord record{};
        if (netgame::decode_record(event.data, event.size, &record) != netgame::WireError::ok)
            return false;
        std::memcpy(game.slot_table, record.slot_ids, sizeof(game.slot_table));
        return true;
    }
    case netgame::RecordType::machine_group_reply: {
        netgame::MachineGroupReplyRecord record{};
        if (netgame::decode_record(event.data, event.size, &record) != netgame::WireError::ok)
            return false;
        const auto slot = slot_for_player_id(lobby, record.player_id);
        if (slot < 0)
            return false;
        slot_player(lobby, slot).machine_group = record.machine_group;
        return true;
    }
    case netgame::RecordType::player_team: {
        netgame::PlayerTeamRecord record{};
        if (netgame::decode_record(event.data, event.size, &record) != netgame::WireError::ok)
            return false;
        const auto slot = slot_for_player_id(lobby, record.player_id);
        if (slot < 0)
            return false;
        const auto& teams = teams_rules(lobby).team_number_alliances;
        if (teams.enabled) {
            // teams.team-number-alliances: the team decides this machine's
            // players' alliances with the sender.
            const auto result = team_rules::receive_team_number(
                lobby_team_slots(lobby),
                static_cast<uint8_t>(slot),
                record.value,
                teams.bit7_keeps_alliances
            );
            if (result.store)
                lobby_player_team(slot_player(lobby, slot)) = static_cast<uint8_t>(result.team);
            lobby_apply_team_steps(lobby, result.steps);
            note_computer_command_echo(
                lobby,
                record.player_id,
                kComputerEditTeam,
                lobby_player_team(slot_player(lobby, slot))
            );
            game.gui_flags |= 1U;
            return true;
        }
        lobby_player_team(slot_player(lobby, slot)) = record.value;
        note_computer_command_echo(lobby, record.player_id, kComputerEditTeam, record.value);
        return true;
    }
    case netgame::RecordType::alliance: {
        netgame::AllianceRecord record{};
        if (netgame::decode_record(event.data, event.size, &record) != netgame::WireError::ok)
            return false;
        const auto from = slot_for_player_id(lobby, record.player_id_a);
        const auto to = slot_for_player_id(lobby, record.player_id_b);
        if (from < 0 || to < 0)
            return false;
        if (teams_rules(lobby).team_number_alliances.enabled &&
            team_rules::alliance_requested(
                lobby_team_slots(lobby)[static_cast<std::size_t>(from)], record.both_sides
            )) {
            // A request for one of this machine's players: it sets the
            // alliance and announces it.
            lobby_announce_alliance(
                lobby, slot_player(lobby, from), slot_player(lobby, to), record.value != 0 ? 1 : 0
            );
            return true;
        }
        if (record.value != 0)
            play(lobby, "Ally");
        auto& sender = slot_player(lobby, from);
        auto& target = slot_player(lobby, to);
        if (local_or_computer(target)) {
            lobby_set_alliance(lobby, sender, target, record.value, record.both_sides != 0);
            game.gui_flags |= 1U;
        }
        sender.alliance[to] = record.value;
        return true;
    }
    case netgame::RecordType::reject: {
        netgame::RejectRecord record{};
        if (netgame::decode_record(event.data, event.size, &record) != netgame::WireError::ok)
            return false;
        const auto slot = slot_for_player_id(lobby, record.player_id);
        if (slot < 0)
            return false;
        // The host refused a 3.1c joiner from its room with mutators: a
        // guest names it too, once, from the joiner's own block. A guest
        // never refuses anyone itself.
        bool mutator_named = false;
        if (record.reason == kRejectVersion && !host_is_local(lobby) &&
            lobby.presence_records.block_player[slot] == record.player_id &&
            classic_in_mutator_room(lobby, slot)) {
            lobby_post_classic_line(lobby, kClassicRefusedLine, slot);
            mutator_named = true;
        }
        // The host refused a player this mod cannot include. A guest names
        // that player and does not refuse anyone itself. The mutator line
        // keeps its place when both apply.
        if (!mutator_named && record.reason == kRejectVersion && !host_is_local(lobby) &&
            lobby.presence_records.block_player[slot] == record.player_id &&
            lobby_room_oa_only(lobby)) {
            const auto* named = slot_info(lobby, slot);
            if (named != nullptr &&
                !netgame::sent_by_open_annihilation(reinterpret_cast<const uint8_t*>(named)))
                lobby_post_limits_line(lobby, lobby_limits_classic_line(lobby), slot);
            else if (
                named != nullptr &&
                netgame::sent_by_open_annihilation(reinterpret_cast<const uint8_t*>(named)) &&
                !lobby_limits_covered(lobby, slot)
            )
                lobby_post_limits_line(lobby, lobby_limits_newer_line(), slot);
        }
        // Passed on once, as for a rejection made here.
        lobby_reject(lobby, record.player_id, record.reason);
        flush(lobby);
        return true;
    }
    case netgame::RecordType::ping: {
        netgame::PingRecord record{};
        if (netgame::decode_record(event.data, event.size, &record) != netgame::WireError::ok ||
            record.echo_tick_count == 0)
            return false;
        // An echo of a ping this machine sent: the echoing player's ping.
        const auto origin = slot_for_player_id(lobby, record.origin_player_id);
        const auto echoing = slot_for_player_id(lobby, event.player_id);
        if (origin < 0 || echoing < 0 || !local_or_computer(slot_player(lobby, origin)))
            return false;
        lobby_player_ping(slot_player(lobby, echoing)) =
            static_cast<int32_t>(milliseconds(lobby) - record.origin_tick_count);
        return true;
    }
    case netgame::RecordType::player_value_request: {
        netgame::PlayerValueRequestRecord record{};
        if (netgame::decode_record(event.data, event.size, &record) != netgame::WireError::ok ||
            !host_is_local(lobby))
            return false;
        const auto color = static_cast<int8_t>(record.value);
        if (!lobby_color_available(lobby, event.player_id, color)) {
            (void)lobby_assign_free_color(lobby, event.player_id, static_cast<uint32_t>(color));
            return true;
        }
        netgame::PlayerValueReplyRecord reply{};
        reply.value = record.value;
        uint8_t wire[4];
        std::size_t written = 0;
        if (netgame::encode_record(reply, wire, sizeof(wire), &written) == netgame::WireError::ok)
            send(lobby, local_player(lobby).player_id, event.player_id, wire, written);
        flush(lobby);
        return true;
    }
    case netgame::RecordType::player_value_reply: {
        netgame::PlayerValueReplyRecord record{};
        if (netgame::decode_record(event.data, event.size, &record) != netgame::WireError::ok)
            return false;
        local_info(lobby).color = record.value;
        lobby_send_player_info(lobby);
        return true;
    }
    case netgame::RecordType::unit_def_handshake:
        unit_sync_receive(
            lobby,
            {event.data, event.size},
            static_cast<uint8_t>(std::max(0, slot_for_player_id(lobby, event.player_id)))
        );
        return true;
    case netgame::RecordType::game_start:
        game.session_flags |= kNetFlagGameStarted;
        info_of(lobby, local_player(lobby)).options |= option::started;
        game.frontend_pending_signal = 0x11;
        return true;
    default:
        if (event.data[0] == netgame::presence_record_type)
            return presence_keep(lobby, event);
        if (lobby.wire_rules.recorder_protocol != netgame::recorder_protocol_plain &&
            netgame::is_recorder_record_type(event.data[0]))
            return recorder_lobby_record(
                lobby, slot_for_player_id(lobby, event.player_id), event.data, event.size
            );
        return false;
    }
}

int32_t map_memory_mb(int32_t terrain_bytes) noexcept {
    constexpr int32_t k16_bytes = 0x3e6666, k24_bytes = 0x600000, k32_bytes = 0x800000;
    constexpr int32_t k48_bytes = 0xa00000, k64_bytes = 0xc00000;
    if (terrain_bytes < k16_bytes)
        return 16;
    if (terrain_bytes < k24_bytes)
        return 24;
    if (terrain_bytes < k32_bytes)
        return 32;
    if (terrain_bytes < k48_bytes)
        return 48;
    return terrain_bytes < k64_bytes ? 64 : 128;
}

// ---------------------------------------------------------------------------
// GET MAP: the host's map as a joiner sees it

bool battle_room_map(
    Lobby& lobby, const Panel& panel, int32_t offset_x, int32_t offset_y, BattleRoomMap* out
) noexcept {
    if (out == nullptr)
        return false;
    try {
        *out = BattleRoomMap{};
        const Control* label = panel_control(panel, "MAPNAME");
        const auto host_slot = lobby.game != nullptr ? lobby_host_slot(lobby) : kNoSlot;
        if (label == nullptr || host_slot == kNoSlot)
            return false;
        const PlayerSetupInfo& host_block = info_of(lobby, slot_player(lobby, host_slot));
        // The name the host sent, read no further than its field.
        out->host_map = std::string(text_view(host_block.map_name, sizeof host_block.map_name));
        out->joiner = !host_is_local(lobby);
        const auto stored = lobby_presence_record(lobby, host_slot);
        netgame::PresenceRecord decoded;
        if (!stored.empty() &&
            netgame::decode_presence(stored.data(), stored.size(), &decoded) ==
                netgame::WireError::ok &&
            decoded.map_pack)
            out->host_pack = host_pack_of(*decoded.map_pack);
        out->label_x = label->x + offset_x;
        out->label_y = label->y + offset_y;
        out->label_width = label->width;
        out->label_height = label->height;
        return true;
    } catch (const std::exception&) {
        *out = BattleRoomMap{};
        return false;
    }
}

} // namespace oa::ui::frontend_multiplayer
