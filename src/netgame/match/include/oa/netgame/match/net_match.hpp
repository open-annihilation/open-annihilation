// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// A running multiplayer match over the lobby's connection: the start record,
// the loading screen's work at its pace (from the building of the world on),
// the load barrier (start positions and "loaded" records) and the end of
// loading, the packet pump that admits and dispatches every in-game record,
// the per-tick unit-state broadcast, the resource and economy records,
// pause/speed, chat, the team panels' records, disconnects and the
// remote-player timeout. Simulation effects of received unit records go
// through ReplicationSim; everything else acts on the canonical World.

#include "oa/base/game_loop.hpp"
#include "oa/netgame/match/integrity_check.hpp"
#include "oa/netgame/match/launch.hpp"
#include "oa/netgame/match/session_lobby.hpp"
#include "oa/netgame/match/vote_reject.hpp"
#include "oa/netgame/recorder_session.hpp"
#include "oa/netgame/replication.hpp"
#include "oa/netgame/wire_rules.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::netgame::match {

// Admission bits of record_phase_table.
enum class NetPhase : uint8_t { lobby = 1, loading = 2, in_game = 4 };

// Game.load_flags bits.
inline constexpr uint16_t load_flag_started = 0x01;
inline constexpr uint16_t load_flag_loader_done = 0x02;
inline constexpr uint16_t load_flag_barrier = 0x04;
inline constexpr uint16_t load_flag_barrier_passed = 0x08;

inline constexpr uint16_t run_flag_paused = 0x01; // Game.sim_run_flags bit 0
// Game.session_flags bits: the session is open, and the game has started.
using oa::ui::frontend_multiplayer::kNetFlagGameStarted;
using oa::ui::frontend_multiplayer::kNetFlagLive;
inline constexpr uint32_t default_timeout_seconds = 0x1e;
inline constexpr uint32_t timeout_drop_extra_seconds = 0x78;
inline constexpr uint32_t keepalive_interval = 0x3c; // time ticks between paused probes
inline constexpr uint8_t min_game_speed = 1;
inline constexpr uint8_t max_game_speed = 20;
inline constexpr uint32_t no_player_id = 0xffffffffu;
inline constexpr uint8_t no_start_position = 0xff;

// Load barrier tables: per player whether its load finished, whether it
// acknowledged the start and its start assignment, and whether the positions
// are assigned. The canonical record holds them in Game.load_barrier, which
// is left zero; the match keeps them here.
struct LoadBarrier {
    uint32_t loaded[OA_PLAYER_COUNT]{};
    uint32_t start_acked[OA_PLAYER_COUNT]{};
    int32_t start_assignment[OA_PLAYER_COUNT]{};
    bool assigned{};
};

struct NetMatchHooks {
    void* context{};

    /// Shows a 0x05 chat line addressed to a local human player.
    ///
    /// @param context NetMatchHooks.context.
    /// @param sender Player slot of the sender.
    /// @param text NUL-terminated text of at most 64 characters.
    void (*chat)(void* context, uint8_t sender, const char* text){};

    /// Shows a status line: disconnects, speed changes, integrity notices.
    ///
    /// @param context NetMatchHooks.context.
    /// @param text NUL-terminated line.
    void (*notice)(void* context, const char* text){};

    /// Translates one of the game's own texts into the language shown, as
    /// the game's translate.tdf keys it: by its English exactly. The match
    /// builds each notice about another machine from such texts as it shows
    /// the notice, as 3.1c does. Null, or a null return, shows the English.
    ///
    /// @param context NetMatchHooks.context.
    /// @param english NUL-terminated English text.
    /// @return The translation, valid until the next call; or null.
    const char* (*translate_game_text)(void* context, const char* english){};

    /// Credits a receiving player's economy for 0x16 subtypes 1 (energy) and 2 (metal) and local gives.
    ///
    /// @param context NetMatchHooks.context.
    /// @param world Match world.
    /// @param to Receiving player slot.
    /// @param metal True for metal, false for energy.
    /// @param amount Amount received.
    void (*credit)(void* context, World* world, uint8_t to, bool metal, float amount){};

    /// Debits the sender's store for a local give.
    ///
    /// @param context NetMatchHooks.context.
    /// @param world Match world.
    /// @param from Giving player slot.
    /// @param metal True for metal, false for energy.
    /// @param amount Amount given.
    /// @return False when the store cannot cover the amount; the give still goes on.
    bool (*debit)(void* context, World* world, uint8_t from, bool metal, float amount){};

    /// Shares sight from one player to another for 0x16 subtype 3: the
    /// receiver takes every cell the sharer has mapped. Null shares nothing.
    ///
    /// @param context NetMatchHooks.context.
    /// @param world Match world.
    /// @param from Player slot sharing its sight.
    /// @param to Player slot receiving it.
    void (*share_sight)(void* context, World* world, uint8_t from, uint8_t to){};

    /// Draws the 15-bit rand() value the start-position shuffle uses.
    ///
    /// @param context NetMatchHooks.context.
    /// @return A value in 0..0x7fff.
    int32_t (*rand15)(void* context){};

    /// Destroys a departing player's units, as the commander rule's sweep does.
    ///
    /// Called as a slot departs, after the local players' alliances with it
    /// are cleared and before the slot is freed. Units simulated here blow
    /// themselves up and are shared as usual; copies of units simulated
    /// elsewhere blow up and die here at once. Null destroys nothing.
    ///
    /// @param context NetMatchHooks.context.
    /// @param world Match world.
    /// @param slot Player slot of the departing player, 0..9.
    void (*destroy_player_units)(void* context, World* world, uint8_t slot){};

    /// Ends the local player's game as a defeat, when a 0x1c disconnect notice names this machine's first
    /// local player. Null keeps the game running.
    ///
    /// @param context NetMatchHooks.context.
    void (*end_local_game)(void* context){};

    /// Tells whether the local player has already won.
    ///
    /// A 0x28 economy record that asks for a reply still gets the 0x29 answer,
    /// but a winner sends no economy of its own back. Null counts as not won.
    ///
    /// @param context NetMatchHooks.context.
    /// @return True once the local player's game is won.
    bool (*local_player_won)(void* context){};

    /// Lets the simulation follow a player's alliance row (Player.alliance)
    /// once a received 0x23 or a departure changed it. Null leaves the
    /// simulation's alliances as they were.
    ///
    /// @param context NetMatchHooks.context.
    /// @param world Match world.
    /// @param slot Player slot whose row changed, 0..9.
    void (*alliance_changed)(void* context, World* world, uint8_t slot){};

    /// Shows the vote on a player as it opens or changes (network.vote-reject). Null shows it as a
    /// notice line only.
    ///
    /// @param context NetMatchHooks.context.
    /// @param vote The vote.
    /// @param target_slot The slot the vote would reject.
    /// @param needed Yes votes it needs.
    /// @param electorate Players taking part.
    void (*vote_shown)(
        void* context, const Vote& vote, uint8_t target_slot, uint8_t needed, uint8_t electorate
    ){};

    /// Sees every record a remote player's machine sent that the pump admits, the recorder's
    /// included, and every record a local player sends, once per send, as a recording keeps
    /// them. Null sees nothing.
    ///
    /// @param context NetMatchHooks.context.
    /// @param sender_id Transport id of the player the record is from.
    /// @param record The record, type byte first.
    /// @param size Its length.
    void (*record_seen)(
        void* context, uint32_t sender_id, const uint8_t* record, std::size_t size
    ){};

    /// Hears that the host locked or unlocked the game speed (.syncon,
    /// .syncoff) under WireRules::speed_lock, after the match applied it.
    /// Null hears nothing.
    ///
    /// @param context NetMatchHooks.context.
    /// @param locked The lock is on.
    /// @param slowest The lowest speed now allowed (net_match_speed_range).
    /// @param fastest The highest speed now allowed.
    void (*speed_lock_changed)(void* context, bool locked, uint8_t slowest, uint8_t fastest){};

    /// Shows a batch of whiteboard marks another player's recorder sent
    /// (recorder message kind 0), once the match has admitted it: the sender
    /// allies this machine's player. Null drops every batch.
    ///
    /// @param context NetMatchHooks.context.
    /// @param sender_slot Slot of the player whose marks they are.
    /// @param batch The batch, its record count first.
    /// @param size Its length, at most recorder_whiteboard_max_payload.
    void (*whiteboard_marks)(
        void* context, uint8_t sender_slot, const uint8_t* batch, std::size_t size
    ){};

    /// Hands a chat line to a seat of this machine, a human or a computer
    /// player, as this machine shows it. A broadcast was addressed to every
    /// player. A private-channel record is not handed on. Null hears nothing.
    ///
    /// @param context NetMatchHooks.context.
    /// @param sender Player slot of the sender.
    /// @param addressee Player slot the record was addressed to; the local
    ///        player for a broadcast.
    /// @param broadcast The record went to every player.
    /// @param text The line as this machine shows it, NUL-terminated.
    void (*seat_chat)(
        void* context, uint8_t sender, uint8_t addressee, bool broadcast, const char* text
    ){};
};

/// The health a unit has in the recorder's eyes before any full record of it was seen.
inline constexpr int16_t recorder_unit_unseen_health = 42;
/// RecorderUnitView::state of a slot the recorder knows no unit in.
inline constexpr uint16_t recorder_unit_state_empty = 1000;
/// RecorderUnitView::state of a unit the recorder saw created and no full record of since.
inline constexpr uint16_t recorder_unit_state_created = 0xff;

/// A unit as the recorder last saw it (sharing.recorder-take-give).
struct RecorderUnitView {
    int16_t health{recorder_unit_unseen_health}; ///< as the unit's last full record gave it
    /// The build byte of the unit's last full record, 0 for a finished unit;
    /// recorder_unit_state_created after its creation record and
    /// recorder_unit_state_empty while the slot holds none.
    uint16_t state{recorder_unit_state_empty};
};

struct NetMatch {
    NetConnection* connection{};
    World* world{};
    ReplicationSim sim{};
    NetMatchHooks hooks{};
    NetPhase phase{NetPhase::loading};
    LoadBarrier barrier{};
    uint32_t frames_since_barrier{};       // loading frames due since the barrier passed
    uint32_t timeout_baseline{};           // time the stall scan counts from
    uint32_t timeout_player{no_player_id}; // player named by the timeout dialog
    uint32_t keepalive_time{};             // next paused probe
    uint32_t records_applied{};
    uint32_t records_refused{}; // gated, unknown sender or not handled
    uint32_t record_errors{};   // malformed or rejected by the replication layer
    /// The mod profile's rules the match keeps (net_match_bind_rules); null keeps 3.1c's.
    const data::match_rules::MatchRules* match_rules{};
    /// Whether the map places units for the neutral player, which the
    /// team start positions read.
    bool map_neutral_units{};
    /// The host worked out the team start positions (team_positions).
    bool team_positions_assigned{};
    /// The start position the team start positions gave each slot, -1 for none.
    std::array<int32_t, OA_PLAYER_COUNT> team_positions{};
    /// The game's network rules, copied from the connection by net_match_begin.
    WireRules rules{};
    /// The recorder's session (rules.recorder_protocol), from the battle room.
    RecorderSession recorder{};
    VoteBoard votes{};                  ///< votes to reject (rules.vote_reject)
    IntegrityCheckState integrity{};    ///< the integrity check (rules.integrity_check)
    uint32_t private_records{};         ///< private chat records read
    uint32_t recorder_records{};        ///< recorder records read
    uint32_t whiteboard_batches{};      ///< whiteboard batches shown (hooks.whiteboard_marks)
    uint32_t commander_syncs_applied{}; ///< commander positions taken from a start sync
    uint32_t commander_syncs_sent{};    ///< start sync records sent
    bool sending_copies{};              ///< a broadcast goes out as one copy per machine
    /// This machine sends and reads chat as UTF-8: its setup blocks say so
    /// (oa::netgame::announce_unicode_chat) and each machine gets a line in
    /// the form it reads (net_match_say). The app sets it; net_match_begin
    /// clears it.
    bool unicode_chat{};
    /// The match plays a recording, whose lines from a player whose block
    /// says UTF-8 chat are UTF-8 whatever this machine reads
    /// (net_match_chat_text).
    bool recorded_chat{};
    /// The records the last line net_match_say sent went out as, in UTF-8,
    /// when it went out as more than one; empty otherwise.
    std::vector<std::string> said_parts;
    /// The recorder's claim on a silent player's units (sharing.recorder-take-give).
    RecorderTake take{};
    /// By unit slot, while sharing.recorder-take-give is on: each unit as the
    /// recorder last saw it.
    std::vector<RecorderUnitView> recorder_units;
    uint32_t taken_units{};    ///< units a take handed over to this machine's player
    uint32_t base_buildings{}; ///< buildings of prebuilt bases this machine handed over
};

/// Takes over the lobby's connection for the match.
///
/// Binds the packet layer to the match's player table and send options,
/// sets the live-game bit and the default 30 s timeout when none is set,
/// marks local players loaded and watchers acknowledged, and sets the
/// load-started flag.
///
/// @param[out] match Match state to initialise.
/// @param connection The lobby's connection.
/// @param world Match world; must already hold the launch (match_launch_apply) and its unit tables.
/// @param sim Replication hooks for received unit records.
/// @param hooks Chat, notice, resource and random hooks.
void net_match_begin(
    NetMatch* match,
    NetConnection* connection,
    World* world,
    const ReplicationSim& sim,
    const NetMatchHooks& hooks
) noexcept;

/// Binds a mod profile's setup and team rules to a begun match.
///
/// With setup.team-start-positions at team-adjacent the host's start
/// positions keep team-mates together (team_rules::team_start_positions);
/// with teams.team-number-alliances a team record moves this machine's
/// players' alliances with its sender and an alliance request for one of
/// them is carried out and announced.
///
/// @param[in,out] match Match begun with net_match_begin.
/// @param rules The rules, or null for 3.1c's.
/// @param map_neutral_units Whether the map places units for the neutral player.
void net_match_bind_rules(
    NetMatch* match, const data::match_rules::MatchRules* rules, bool map_neutral_units
) noexcept;

/// Deals teams by start position in the game (the console's "autoteam" under
/// teams.team-number-alliances).
///
/// Only the host that worked out team start positions deals; it needs two
/// players with a start position and no player on a team. Each such player
/// allies the players whose position equals its own modulo `teams` and
/// unallies the rest, announcing each alliance as the team rules do.
///
/// @param[in,out] match Running match.
/// @param teams the number of teams, 2..5
/// @param[out] notice the line that reports the outcome
/// @param capacity bytes at notice
void net_match_deal_teams(
    NetMatch* match, int32_t teams, char* notice, std::size_t capacity
) noexcept;

/// Queues the 0x08 start record from the local player as the battle room closes; only the host sends it.
///
/// Nothing is flushed: the loading screen's first frame sends it.
///
/// @param[in,out] match Running match.
void net_match_send_game_start(NetMatch* match) noexcept;

/// Queues the 0x08 start record from the battle room's local player as the host's battle room closes.
///
/// It goes to every player, or, while machines are shared
/// (Game.shared_machines), to one player of each other machine, as every
/// broadcast then does. Nothing is flushed: the first frame of the loading
/// screen (net_match_building_frame) sends it with that frame's records.
/// Does nothing outside a session.
///
/// @param[in,out] connection The battle room's connection.
/// @param game The battle room's players.
void net_match_queue_game_start(NetConnection* connection, const Game& game) noexcept;

/// Notes that the loader has finished loading and waits on the barrier.
///
/// @param[in,out] match Running match whose Game.load_flags gains the barrier bit.
void net_match_loader_waiting(NetMatch* match) noexcept;

/// Runs one loading-screen frame: probe records, pump, barrier.
///
/// Once the barrier passes, guaranteed delivery is turned off.
///
/// @param[in,out] match Running match.
/// @return True once the barrier has passed; the start positions are then final.
bool net_match_loading_frame(NetMatch* match) noexcept;

/// Returns the start position index of a slot once the barrier passed (Player.start_position).
///
/// The host sends each machine the start positions of its own players
/// only, so another machine's player keeps whatever its record held before;
/// net_match_fixed_start_positions gives its start when the starts are
/// fixed.
///
/// @param match Running match.
/// @param slot Player slot, 0..9.
/// @return The start position, or no_start_position for a slot out of range.
[[nodiscard]] uint8_t net_match_start_position(const NetMatch* match, uint8_t slot) noexcept;

/// Works out the start position the host gives every slot when the starts
/// are fixed, the same on every machine of the match.
///
/// The host deals fixed starts in its own slot order: every active slot
/// that is not watching takes the next position, or with
/// setup.team-start-positions at team-adjacent the team rules place them
/// (team_rules::team_start_positions). Another machine seats the players in
/// slots of its own, so it reads the host's order from the host's slot
/// table, and on a map with neutral units reads the computer players of the
/// host's machine group as the host's own. Shuffled starts are the host's
/// alone, so this does not give them.
///
/// @param match Running match.
/// @param host_slots The host's slot table (Game.slot_table, record 0x26) as
///        the battle room last had it: each of the host's slots' player id,
///        0 for a free slot, no_player_id for a closed one. The host's
///        machine reads its own slots instead.
/// @param[out] positions each of this machine's slots' start position, -1 for none
/// @return False, with every position -1, when the table does not list this
///         machine's players as playing once each, or the team rules would
///         read the host's computer players and the host has no machine group
[[nodiscard]] bool net_match_fixed_start_positions(
    const NetMatch* match,
    std::span<const uint32_t, OA_PLAYER_COUNT> host_slots,
    std::array<int32_t, OA_PLAYER_COUNT>& positions
) noexcept;

// Rows of the loading screen (Game.load_progress), each 0..100.
inline constexpr std::size_t load_progress_rows = 6;
inline constexpr uint8_t load_progress_complete = 100;

/// Connection time ticks between two frames of the loading screen's work: it
/// runs at most every 200 ms, as the loading screen waits that long after
/// each frame.
inline constexpr uint32_t loading_frame_ticks = 6;

/// Loading frames that come due after the one in which the load barrier
/// passed before the commanders are placed and announced (0x09).
///
/// Another machine passes its own barrier at the first loading frame of its
/// own that reads this machine's 0x15, and a machine still on its loading
/// screen refuses a 0x09, as 3.1c does. A 0x09 that reaches it together with
/// the 0x15 is lost there: it makes its copy of the commander from the first
/// unit state record (0x2c) that names the commander, where its empty slot
/// stands, and puts it on the start position only when the commander's full
/// record comes round, units_per_player ticks later. Two loading frames
/// (400 ms) outlast a loading frame of the other machine (200 ms and its
/// drawing), so the 0x09 arrives after its barrier has passed and the
/// commander stands on its start position from its first record. The
/// records are those 3.1c sends, in its order, in one flush: the last load
/// progress (0x2a), the commanders' 0x09 and 0x11, the setup blocks and
/// teams.
inline constexpr uint32_t commander_wait_frames = 2;

/// When the loading screen's work last ran.
struct LoadingPace {
    bool ran{};      // it has run at least once
    uint32_t time{}; // connection time (net_connection_time) of the last run
};

/// Tells whether the loading screen's work is due and, when it is, notes it as run now.
///
/// It is due the first time and then once loading_frame_ticks have passed
/// since it last ran.
///
/// @param[in,out] pace When it last ran.
/// @param now Connection time (net_connection_time).
/// @return True when the work runs now.
[[nodiscard]] bool loading_frame_due(LoadingPace* pace, uint32_t now) noexcept;

/// Keeps the battle room's connection alive while a match's world is built, before net_match_begin.
///
/// Services the transport without waiting; each local or computer player
/// of the battle room still in the game sends a 0x06 probe, flushed at once,
/// then its load progress, the mean of the rows, as 0x2a; all is flushed.
/// Both go to every player, or while machines are shared to one player of
/// each other machine. Received frames stay queued for the match's pump.
/// The caller paces it (loading_frame_due). Does nothing outside a session.
///
/// @param[in,out] connection The battle room's connection.
/// @param game The battle room's players.
/// @param rows The six loading-screen category percentages, 0..100 each.
void net_match_building_frame(
    NetConnection* connection, const Game& game, const uint8_t rows[load_progress_rows]
) noexcept;

/// Sends each local and computer player's load progress, the mean of the rows, as 0x2a to all.
///
/// The mean is also stored as the player's own load progress (Player.load_progress).
///
/// @param[in,out] match Running match.
/// @param rows The six loading-screen category percentages, 0..100 each.
void net_match_send_load_progress(NetMatch* match, const uint8_t rows[load_progress_rows]) noexcept;

/// Runs one loading-screen frame at the loading screen's pace.
///
/// When loading_frame_due says so: net_match_loading_frame (probes, pump,
/// barrier, flush), then net_match_send_load_progress, whose records the
/// next frame's flush sends. Otherwise nothing is sent or read. Once the
/// barrier has passed, a frame that comes due only services the transport
/// and counts toward commander_wait_frames: nothing is sent, and what
/// arrives stays queued for the match's pump.
///
/// @param[in,out] match Running match.
/// @param[in,out] pace When the loading screen's work last ran.
/// @param rows The six loading-screen category percentages, 0..100 each.
/// @return True once the barrier has passed and commander_wait_frames more frames have come due: the
///         start positions are final and the commanders can be placed.
bool net_match_paced_loading_frame(
    NetMatch* match, LoadingPace* pace, const uint8_t rows[load_progress_rows]
) noexcept;

/// Ends the loading of a match once its barrier has passed and the local commanders stand.
///
/// The local player's setup block gains OA_SETUP_OPTION_STARTED; the local
/// and computer players send their lobby blocks and teams with the
/// machine-group requests (net_match_send_player_status); the host publishes
/// its session again (net_match_republish), flag 0x20 (the game has
/// started) set; and one economy period is counted
/// (net_match_economy_period).
///
/// @param[in,out] match Running match.
void net_match_end_loading(NetMatch* match) noexcept;

/// Publishes the hosted session again from the local player's setup block, as the battle room does.
///
/// The block's options take the current player count (Game.player_count)
/// in their low four bits, as the battle room's description does; its
/// bytes from 0x99 become the description's user bytes, the launch-only
/// status bit left out. With the started option set, flag 0x20 is set. The
/// name last published in full and the player limit stay. Does nothing
/// unless this machine hosts the session.
///
/// @param[in,out] match Running match.
void net_match_republish(NetMatch* match) noexcept;

// Player bars of the multiplayer loading screen: the active players share
// a 0x26c-pixel strip from x 0xb, each bar two pixels short of its share.
inline constexpr int32_t loading_bars_left = 0xb;
inline constexpr int32_t loading_bars_width = 0x26c;
inline constexpr int32_t loading_bar_gap = 2;
inline constexpr int32_t loading_bar_top = 0x1a4;
inline constexpr int32_t loading_bar_bottom = 0x1b3;
inline constexpr int32_t loading_status_x = 10;
inline constexpr int32_t loading_status_y = 400;
inline constexpr std::size_t loading_status_bytes = 128;

struct LoadingBar {
    const Player* player{}; // its name is drawn over the bar
    int32_t left{};
    int32_t right{};  // the bar spans left..right on rows loading_bar_top..loading_bar_bottom
    int32_t filled{}; // the progress covers left..filled
};

struct LoadingScreenStatus {
    LoadingBar bars[OA_PLAYER_COUNT]{};
    int32_t bar_count{};               // none once the barrier passed
    int32_t ready{};                   // players at 100 whose "loaded" record arrived
    char text[loading_status_bytes]{}; // drawn at loading_status_x, loading_status_y
};

/// Lays out the multiplayer loading screen's player bars and status line.
///
/// The line's texts are the game's own, in the language shown
/// (NetMatchHooks::translate_game_text).
///
/// @param match Running match.
/// @param[out] out Bars for every active player in slot order and the line counting the players ready;
///        "Synchronization complete" alone once the barrier passed.
void net_match_loading_screen_status(const NetMatch* match, LoadingScreenStatus* out) noexcept;

/// Ends loading: in-game records are admitted from now on and the timeout baseline restarts.
///
/// @param[in,out] match Running match.
void net_match_enter_game(NetMatch* match) noexcept;

/// Drains and dispatches every deliverable packet, then rescans for stalled players.
///
/// Records are admitted by phase and sender; a record from a slot that is
/// neither local nor a seated remote player rejects that sender. A 0x08
/// start record ends the drain. In the game, while this machine's player
/// takes a silent player's units (sharing.recorder-take-give), a step of the
/// take goes before each packet read, and the drain goes on while steps
/// hand units over.
///
/// @param[in,out] match Running match.
/// @return How many packets were read; 0 outside a live game.
uint32_t net_match_pump(NetMatch* match) noexcept;

/// Sends one record from a local player id, suppressed for rejected slots.
///
/// While machines are shared a broadcast goes to one player of each machine.
///
/// @param[in,out] match Running match.
/// @param from_id Transport id of a local player that has not been rejected.
/// @param to_id Destination transport id: 0 broadcasts, otherwise a remote player not rejected.
/// @param record Record bytes, type byte first.
/// @param size Record length in bytes.
/// @return True when the record was queued (always true for a shared-machine broadcast).
bool net_match_send(
    NetMatch* match, uint32_t from_id, uint32_t to_id, const uint8_t* record, std::size_t size
) noexcept;

/// Packs and broadcasts this tick's 0x2c record of a local player.
///
/// @param[in,out] match Running match; a packing error is counted in record_errors.
/// @param player Local player whose units are packed; others are ignored.
void net_match_send_player_state(NetMatch* match, Player* player) noexcept;

/// Sends every local and computer player's lobby block (0x20) and team (0x24) to all players, pushed out at once.
///
/// Each player's 0x20 goes out from that player, followed by its 0x24,
/// which is flushed at once. With machine_groups, the machine-group requests
/// the battle room sends follow (0x21, to the host, from the first player
/// on this machine): for each active player still in group 0, a local or
/// computer player asks for a group (a computer player for its human's), and
/// a player simulated elsewhere asks for the group the host gave it; on the
/// host's own machine its players take group 1 and nothing is sent. The
/// running game's status asks for none. Everything is flushed at the end.
///
/// @param[in,out] match Running match.
/// @param machine_groups Also send the machine-group requests, as at the end of loading.
void net_match_send_player_status(NetMatch* match, bool machine_groups) noexcept;

/// Tells another player's machine that a local player's alliance with its player changed (in-game ALLIES.GUI).
///
/// Sends 0x23 {from's id, to's id, allied, both sides 0} from the local
/// player to that player alone, pushed out at once. The local alliance
/// tables already hold the change. Nothing goes to a player simulated here,
/// or from or to a rejected one.
///
/// @param[in,out] match Running match.
/// @param from Local player slot whose alliance changed, 0..9.
/// @param to The other player's slot, 0..9.
/// @param allied 1 allied, 0 not.
void net_match_send_alliance(NetMatch* match, uint8_t from, uint8_t to, uint8_t allied) noexcept;

/// Removes a player from the game on every machine (in-game CONTROL.GUI).
///
/// Broadcasts 0x1b {id, reason} and retires the slot or slots as a
/// received reject does: a human simulated elsewhere that is still playing
/// takes its whole machine with it.
///
/// @param[in,out] match Running match.
/// @param slot Player slot, 0..9; a free slot is ignored.
/// @param reason RejectReason it leaves with: 1 removed by the host, 9 watching no longer allowed.
void net_match_remove_player(NetMatch* match, uint8_t slot, uint8_t reason) noexcept;

/// Broadcasts a unit created locally as a 0x09 record from its owner.
///
/// @param[in,out] match Running match.
/// @param unit New unit; ignored unless its owner plays on this machine.
void net_match_send_unit_created(NetMatch* match, const Unit* unit) noexcept;

/// Broadcasts a builder link or completion as a 0x12 record from the builder's owner.
///
/// @param[in,out] match Running match.
/// @param source Building unit; ignored unless its owner plays on this machine.
/// @param subject Unit being built, or null (unit index 0).
void net_match_send_builder_link(NetMatch* match, const Unit* source, const Unit* subject) noexcept;

/// Broadcasts a health event on a unit owned elsewhere as a 0x0b record.
///
/// @param[in,out] match Running match.
/// @param route Player id the record goes out from.
/// @param record The damage event.
void net_match_send_damage(
    NetMatch* match, uint32_t route, const UnitDamageRecord& record
) noexcept;

/// Returns the default route of a relayed health event.
///
/// @param match Running match.
/// @return The first local player's id, or no_player_id.
[[nodiscard]] uint32_t net_match_local_route(const NetMatch* match) noexcept;

/// Returns the player id of the game's host: the first in-use slot whose setup block has the host role bit.
///
/// @param match Running match.
/// @return That player's id, or no_player_id when no slot has the role.
[[nodiscard]] uint32_t net_match_host_id(const NetMatch* match) noexcept;

/// Ends one simulation tick: allied resource and sight sharing for the local player, then the paced flush.
///
/// Every 60 ticks, with the player's share options on, the metal above
/// Player.metal_share_threshold (a third of it) and the energy above
/// Player.energy_share_threshold (half of it) go to another player, capped at
/// that player's free storage. Every 450 ticks, with its sight option on, the
/// local player shares its sight with each player it is allied with. Both
/// reach only players simulated elsewhere that are humans still playing,
/// taking part in the game and set in the local player's alliance row.
///
/// @param[in,out] match Running match.
/// @quirk The resources go to the last such player in slot order whose store
///        is below the local player's, not to the poorest.
void net_match_after_tick(NetMatch* match) noexcept;

/// Runs a frame with no tick while the pause bit of Game.sim_run_flags is set: flush, pump, and a probe
/// every 0x3c time ticks.
///
/// A menu or a finished game held on its outcome is not a pause and runs no
/// such frame.
///
/// @param[in,out] match Running match.
void net_match_paused_frame(NetMatch* match) noexcept;

/// Scans remote players for a stall past Game.player_timeout_seconds.
///
/// While paused the baseline moves to now. With the console's no-drop
/// option the scan does not run and the dialog's player stays as it was.
///
/// @param[in,out] match Running match.
/// @return The id to name in the timeout dialog: the first stalled player when all stalled players share a
///         machine group, else no_player_id.
[[nodiscard]] uint32_t net_match_check_timeouts(NetMatch* match) noexcept;

/// Drops a remote player stalled past the timeout plus 0x78 s.
///
/// @param[in,out] match Running match.
/// @param player_id Transport id of the stalled player.
/// @return True when the player was rejected with reason connection lost.
bool net_match_timeout_expired(NetMatch* match, uint32_t player_id) noexcept;

/// Hands the host role on after the host left a started game.
///
/// The slot answering to the highest transport id (unsigned) among the local
/// and remote players takes the host role.
///
/// @param[in,out] world Match world whose successor's info block gains the host role bit.
/// @quirk Computer players' ids are not counted; with no local or remote player the search stays at id 0,
///        which is looked up like any other.
void net_match_designate_host(World* world) noexcept;

/// Copies every player's presence, status, unit count and last tick into the timing coordinator's peer slots.
///
/// The coordinator uses them for the lag throttle (900/3600 ticks).
///
/// @param world Match world.
/// @param[in,out] timing Timing coordinator whose peer slots are updated.
void net_match_sync_timing(const World* world, base::game_loop::Timing* timing) noexcept;

/// Sets the pause bit of Game.sim_run_flags and broadcasts it as 0x19 {kind 0, value} from the first
/// player on this machine.
///
/// Only the Pause key pauses a network game; the in-game menus never do. The
/// engine flips the bit as the key is pressed, so this writes the same value.
/// While the recorder's autopause holds the game, an unpause on any machine
/// but the host's sets the bit again, sends nothing and says why.
///
/// @param[in,out] match Running match.
/// @param paused New pause state.
void net_match_set_pause(NetMatch* match, bool paused) noexcept;

/// Returns the speeds the match may be set to: the rules' range
/// (WireRules::speed_min to speed_max), narrowed by the host's speed lock
/// while WireRules::speed_lock is on, the recorder protocol is presented
/// and the host has locked the speed (recorder_speed_range).
///
/// @param match Running match.
/// @param[out] slowest The lowest speed; may be null.
/// @param[out] fastest The highest speed; may be null.
/// @return True while the host's lock narrows the range.
bool net_match_speed_range(const NetMatch* match, uint8_t* slowest, uint8_t* fastest) noexcept;

/// Changes the game speed, shows a notice when it changed and optionally broadcasts it as 0x19.
///
/// The speed is clamped to the rules' range (1..20 in 3.1c). A local change
/// outside the host's speed lock (net_match_speed_range) is not made. The
/// notice is the speed keys' own line (sim::speed::format_message), in the
/// language shown (NetMatchHooks::translate_game_text), for a received speed as for
/// a local one, as in 3.1c.
///
/// @param[in,out] match Running match.
/// @param speed Requested speed, clamped first to WireRules::speed_max and then to
///        WireRules::speed_min (1..20 in 3.1c); 10 is normal.
/// @param broadcast True for a local change, false when applying a received one.
void net_match_set_speed(NetMatch* match, int32_t speed, bool broadcast) noexcept;

/// Sends a chat line from the first local human player as 0x05, routed by Game.chat_mode.
///
/// A line starting with '+' and a line in mode 0 (everyone) go to every
/// player. Mode 3 (chosen players) sends one copy to each player whose
/// Game.chat_targets entry is set. Modes 1 (allies) and 2 (enemies) send one
/// copy to each player simulated elsewhere whose entry in the local player's
/// alliance row is set (1) or clear (2). Mode 4 (this machine only) sends
/// nothing. The recorder then reads the line for its commands, so an answer
/// follows the line that asked for it.
///
/// With NetMatch::unicode_chat off the record carries the line's first 64
/// bytes, cut between whole UTF-8 characters, as it always has. With it on
/// the line is read as UTF-8 (oa::netgame::chat_utf8) and each machine gets
/// it in the form it reads: UTF-8 where its setup block says so
/// (oa::netgame::announces_unicode_chat), the code page with '?' elsewhere.
/// A command ('+' or '.' first) goes as one record; another line longer
/// than a record goes as up to four, each "<Name> " and a part
/// (oa::netgame::chat_parts), which NetMatch::said_parts keeps. When the
/// forms differ, the record_seen hook sees the UTF-8 form once and the
/// copies go out with NetMatch::sending_copies set; a record all in ASCII
/// reads the same in both forms and goes once, so a line all in ASCII that
/// fits one record goes exactly as with the setting off.
///
/// @param[in,out] match Running match.
/// @param text Line to send, as this machine holds game text; null sends an
///        empty line.
void net_match_say(NetMatch* match, const char* text) noexcept;

/// Sends a chat line from a computer seat of this machine, from that seat's player id.
///
/// Sends nothing unless the seat is in use, a computer player, with a
/// player id, and the line is not a command ('+' or '.' first, after a
/// "<Name> " head). Everyone is one broadcast. Chosen players are the
/// slots `targets` names whose player id is not 0. Allies and enemies are
/// the remote players by the seat's own alliance row. Any other mode sends
/// nothing. With Unicode chat the line goes out as the parts say_unicode
/// makes. NetMatch::said_parts and the recorder are left as they were.
///
/// @param[in,out] match Running match; null sends nothing.
/// @param seat Player slot of the computer seat.
/// @param line The line, speaker prefix included; null sends nothing.
/// @param mode Chat mode (OA_CHAT_MODE_*).
/// @param targets Chosen-player slots; null when the mode names none.
void net_match_say_from(
    NetMatch* match, uint8_t seat, const char* line, uint8_t mode, const uint8_t* targets
) noexcept;

/// Gives a received chat line as this machine shows it, in the form it holds game text.
///
/// A line from a player whose setup block says UTF-8 chat is read strictly
/// as UTF-8 (oa::netgame::chat_strict_utf8) while NetMatch::unicode_chat is
/// on, and, from a recording (NetMatch::recorded_chat), written in the code
/// page while it is off. Every other line is kept as it came, to be read in
/// the code page, or as well-formed UTF-8 where it is that, while game text
/// may hold UTF-8. The alliance lines, which go between machines in
/// English, show their phrase through NetMatchHooks::translate_game_text
/// (oa::sim::messages::format_shown_chat_line), a deliberate difference
/// from 3.1c; the line is cut to 255 bytes.
///
/// @param match The match the line reached.
/// @param from The player it came from.
/// @param bytes The line, up to its first zero byte.
/// @return The line to show.
[[nodiscard]] std::string
net_match_chat_text(const NetMatch* match, const Player& from, std::string_view bytes);

/// Tells the receiver's machine about energy or metal a local player gave, which has already moved here.
///
/// Sends 0x16 subtype 1 (energy) or 2 (metal) from the giver to the
/// receiver when both still take part in the game: active, and holding units
/// or never having built any. Nothing is debited or credited.
///
/// @param[in,out] match Running match.
/// @param from Giving player slot, 0..9.
/// @param to Receiving player slot, 0..9.
/// @param metal True for metal, false for energy.
/// @param amount Amount given.
void net_match_send_give(
    NetMatch* match, uint8_t from, uint8_t to, bool metal, float amount
) noexcept;

/// Gives energy or metal from a local slot to another player.
///
/// The amount is capped at the sender's store; nothing happens for none. It
/// is debited from the sender through the hooks (a debit the store cannot
/// cover is ignored) and credited to the receiver, then sent as 0x16 subtype
/// 1 (energy) or 2 (metal) from the giver to the receiver when both still
/// take part in the game: active, and holding units or never having built
/// any.
///
/// @param[in,out] match Running match.
/// @param from Giving player slot, 0..9.
/// @param to Receiving player slot, 0..9.
/// @param metal True for metal, false for energy.
/// @param amount Amount to give.
void net_match_give(NetMatch* match, uint8_t from, uint8_t to, bool metal, float amount) noexcept;

/// Shares sight with another player by sending 0x16 subtype 3.
///
/// @param[in,out] match Running match.
/// @param from Player slot sharing its sight, 0..9.
/// @param to Player slot receiving it, 0..9.
void net_match_share_sight(NetMatch* match, uint8_t from, uint8_t to) noexcept;

/// Counts one 30-tick economy period; every fourth sends the viewpoint player's economy (0x28).
///
/// It goes to each player simulated elsewhere that is a human still playing
/// (not a computer player) and has not been rejected. The count lives on
/// the connection (NetConnection::economy_periods) and is never reset: the
/// end of loading counts one period, so the first game sends its first 0x28
/// at tick 90, and a later game on the same connection goes on from where
/// the last one left the count.
///
/// @param[in,out] match Running match.
void net_match_economy_period(NetMatch* match) noexcept;

/// Runs the network rules' work at the end of a simulation tick.
///
/// With the rules on: the integrity check's challenges at their ticks and its
/// report at tick 600, the tally of every open vote (a vote that passes
/// rejects its player on this machine, as every machine does), and the
/// recorder's release of a commander warp once every player's warp is done.
/// Does nothing with 3.1c's rules.
///
/// @param[in,out] match Running match.
void net_match_rules_tick(NetMatch* match) noexcept;

/// Votes on a player (network.vote-reject): the local human's yes or no to an open vote.
///
/// The vote goes to every player and counts here at once.
///
/// @param[in,out] match Running match.
/// @param target_id Transport id of the player the vote would reject.
/// @param yes true for yes
/// @return false when no vote on that player is open or the rule is off
bool net_match_cast_vote(NetMatch* match, uint32_t target_id, bool yes) noexcept;

/// Asks every machine to vote on rejecting a player (network.vote-reject).
///
/// The local human proposes, which counts as its yes.
///
/// @param[in,out] match Running match.
/// @param slot Player slot, 0..9.
/// @return false when the rule is off, the slot is no remote player, or a vote on it is open or
///         cooling down
bool net_match_propose_reject(NetMatch* match, uint8_t slot) noexcept;

/// Notes a full unit record a remote player's machine sent, as the recorder keeps it (sharing.recorder-take-give).
///
/// The record's health and build byte become the unit's in RecorderUnitView;
/// a record of an empty slot empties it. Does nothing while the rule is off.
///
/// @param[in,out] match Running match.
/// @param unit_slot The unit's slot.
/// @param record The record.
void net_match_note_full_record(
    NetMatch* match, uint32_t unit_slot, const FullUnitRecord& record
) noexcept;

/// Tells every recorder that the local player's commander warp is done (recorder message kind 1).
///
/// Sent alone, once, to each player whose machine runs the recorder. The
/// local players count as done here.
///
/// @param[in,out] match Running match.
void net_match_warp_done(NetMatch* match) noexcept;

/// Starts the game paused when the host's recorder options ask for it.
///
/// With autopause or commander warp set, every recorder sends a pause of its
/// own as the game starts; this machine does too and pauses.
///
/// @param[in,out] match Running match.
void net_match_recorder_start(NetMatch* match) noexcept;

/// Sends a batch of whiteboard marks to every other recorder (recorder message kind 0).
///
/// The batch goes as the whole payload, alone in its own frame, once to
/// each player still in the game whose machine runs recorder protocol 2 or
/// later. A machine that presents no recorder sends nothing.
///
/// @param[in,out] match Running match.
/// @param batch The batch, its record count first.
/// @param size Its length, 1 to recorder_whiteboard_max_payload.
/// @return The number of players it went to; 0 when nothing was sent.
uint32_t
net_match_send_whiteboard(NetMatch* match, const uint8_t* batch, std::size_t size) noexcept;

/// Sends the camera position to every recorder (record 0xfc), appended to the next frame.
///
/// @param[in,out] match Running match.
/// @param x Camera x, or recorder_camera_off.
/// @param y Camera y, or recorder_camera_off.
void net_match_send_camera(NetMatch* match, uint16_t x, uint16_t y) noexcept;

/// Notes where this machine's camera is, for the recorder to share.
///
/// While the recorder is on and the local player has not turned sharing off
/// (.sharemappos), the camera goes to every player with the frames this
/// machine sends: when it has moved since it last went out, at most once
/// each send interval.
///
/// @param[in,out] match Running match.
/// @param x The camera's centre in map pixels, clamped to 0..0xfffe.
/// @param y The camera's centre in map pixels, clamped to 0..0xfffe.
void net_match_note_camera(NetMatch* match, int32_t x, int32_t y) noexcept;

/// Settles the economy exchange of a finished game.
///
/// Resends each local player's economy (wanting a reply) to every player it
/// has not settled with, then services the transport for 250 ms.
///
/// @param[in,out] match Running match.
/// @return True once nothing was left to send.
[[nodiscard]] bool net_match_final_economy(NetMatch* match) noexcept;

} // namespace oa::netgame::match
