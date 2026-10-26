// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Multiplayer lobby state: the lobby player-info block, the per-slot rules of
// the battleroom (LOUNGE2.GUI) and the unit-content sync table.
//
// Slots are the ten canonical Player records of oa::Game. Player.info holds a
// 1-based handle into Lobby::infos. Game and Player fields the lobby reads
// and writes are reached through the unaligned accessors below.
#pragma once

#include "oa/data/campaign/campaign_file.hpp"
#include "oa/data/match_rules.hpp"
#include "oa/core/game_state.h"
#include "oa/core/types.h"
#include "oa/core/unit_def.h"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/recorder_session.hpp"
#include "oa/netgame/wire_rules.hpp"
#include "oa/ui/frontend_multiplayer/launch_block.hpp"
#include "oa/ui/frontend_multiplayer/lobby_net.hpp"
#include "oa/ui/frontend_multiplayer/panel.hpp"
#include "oa/ui/frontend_multiplayer/room_mutators.hpp"
#include "oa/ui/frontend_multiplayer/team_rules.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::frontend_multiplayer {

inline constexpr int32_t kSlotCount = OA_PLAYER_COUNT;
inline constexpr uint8_t kNoSlot = 10;
inline constexpr uint8_t kNoTeam = 5; // Player.team of a player on no team
inline constexpr int32_t kTeamCount = 5;
inline constexpr uint8_t kNoColor = 0xff; // PlayerSetupInfo.color before a colour is chosen
inline constexpr int32_t kColorCount = 10;
inline constexpr int16_t kRowPitch = 0x14; // cloned row spacing in pixels
/// The player timeout of a game that names none, in seconds; the -t switch
/// sets 30 to 300.
inline constexpr int32_t kDefaultPlayerTimeoutSeconds = 30;

// Player.status values used by the lobby.
inline constexpr uint8_t kSlotOpen = OA_PLAYER_STATUS_FREE;
inline constexpr uint8_t kSlotLocal = OA_PLAYER_STATUS_LOCAL;
inline constexpr uint8_t kSlotComputer = OA_PLAYER_STATUS_COMPUTER;
inline constexpr uint8_t kSlotRemote = OA_PLAYER_STATUS_MIRRORED;
inline constexpr uint8_t kSlotBlocked = OA_PLAYER_STATUS_CLOSED;

// PlayerSetupInfo.state of a remote slot: 1 playing, 2 defeated.
inline constexpr uint8_t kInfoStatePlaying = 1;
inline constexpr uint8_t kInfoStateDefeated = 2;

// PlayerSetupInfo.role: the player hosts the session.
inline constexpr uint8_t kRoleHost = 0x01;

/// Game.session_flags bit set while a multiplayer session is open.
inline constexpr uint8_t kNetFlagLive = 0x01;
/// Game.session_flags bit set when the host's 0x08 start record arrives, and
/// when a watcher joins a game already started.
inline constexpr uint8_t kNetFlagGameStarted = 0x04;
/// Game.outcome_flags bit set when the player leaves the game.
inline constexpr uint16_t kOutcomeLeaving = 0x04;

// PlayerSetupInfo.options: the host's copy is the game setting.
namespace option {
inline constexpr uint16_t started = 0x0010; // START pressed; also session flag 0x20
inline constexpr uint16_t ready = 0x0020;   // READYx
inline constexpr uint16_t watcher = 0x0040; // slot is spectating
inline constexpr uint16_t watching_allowed = 0x0080;
inline constexpr uint16_t unmapped = 0x0100;       // MAPPING stage 0
inline constexpr uint16_t los_limited = 0x0200;    // LOSTYPE not "Permanent"
inline constexpr uint16_t los_true = 0x0400;       // with los_limited: "True", else "Circular"
inline constexpr uint16_t commander_mask = 0x1800; // COMMANDER stage << 11
inline constexpr uint16_t commander_step = 0x0800;
inline constexpr uint16_t commander_deathmatch = 0x1000;
inline constexpr uint16_t cheats_allowed = 0x2000;
inline constexpr uint16_t fixed_locations = 0x4000;
inline constexpr uint16_t game_closed = 0x8000;
inline constexpr uint16_t player_count_mask = 0x000f; // Game.player_count as a player is seated
} // namespace option

// PlayerSetupInfo.status.
namespace status {
inline constexpr uint16_t password = 0x0001; // host requires a password
inline constexpr uint16_t allied_victory = 0x0002;
inline constexpr uint16_t has_disc = 0x0004;
// Only a launched game may join; the version byte is biased by 100.
inline constexpr uint16_t launch_only = 0x0008;
} // namespace status

#pragma pack(push, 1)

// The 0xB9-byte lobby block sent as record 0x20 and copied by DirectPlay
// system message 0x102.
struct PlayerSetupInfo {
    char map_name[0x80]{};
    char password[0x0b]{};
    uint16_t screen_width{};
    uint16_t screen_height{};
    uint8_t reserved_after_screen_height{}; // copied with the block; never read
    uint32_t net_id{};                      // sender id overlay in record 0x20
    uint8_t state{};                        // mirrors Player.status except remote
    uint8_t side{};
    uint8_t color{};
    uint8_t role{};
    uint8_t reserved_after_role{}; // copied with the block; never read
    uint16_t memory_mb{};
    uint16_t options{};
    uint16_t status{};
    uint16_t lowest_latency{};
    uint16_t energy_hundreds{};
    uint16_t metal_hundreds{};
    uint16_t max_units{};
    uint8_t version_major{};
    uint8_t version_minor{};
    uint32_t map_hash{};
    uint8_t engine_signature[2]{};            // 'O', 'A' when OA sent the block
    uint8_t reserved_after_signature[0x05]{}; // copied with the block; never read
    uint8_t recorder_protocol{};              // the sender's recorder version, 0 for none
    uint8_t chat_signature[2]{};              // 'U', '8' when chat_flags holds the sender's chat
    uint8_t chat_flags{};                     // netgame::chat_flag_utf8, read after chat_signature
    uint8_t reserved_after_chat_flags{};      // copied with the block; never read
};

#pragma pack(pop)
static_assert(sizeof(PlayerSetupInfo) == kPlayerInfoBytes);
OA_ASSERT_OFFSET(PlayerSetupInfo, map_name, 0x00);
OA_ASSERT_OFFSET(PlayerSetupInfo, password, 0x80);
OA_ASSERT_OFFSET(PlayerSetupInfo, screen_width, 0x8b);
OA_ASSERT_OFFSET(PlayerSetupInfo, screen_height, 0x8d);
OA_ASSERT_OFFSET(PlayerSetupInfo, reserved_after_screen_height, 0x8f);
OA_ASSERT_OFFSET(PlayerSetupInfo, net_id, 0x90);
OA_ASSERT_OFFSET(PlayerSetupInfo, state, 0x94);
OA_ASSERT_OFFSET(PlayerSetupInfo, side, 0x95);
OA_ASSERT_OFFSET(PlayerSetupInfo, color, 0x96);
OA_ASSERT_OFFSET(PlayerSetupInfo, role, 0x97);
OA_ASSERT_OFFSET(PlayerSetupInfo, reserved_after_role, 0x98);
OA_ASSERT_OFFSET(PlayerSetupInfo, memory_mb, 0x99);
OA_ASSERT_OFFSET(PlayerSetupInfo, options, 0x9b);
OA_ASSERT_OFFSET(PlayerSetupInfo, status, 0x9d);
OA_ASSERT_OFFSET(PlayerSetupInfo, lowest_latency, 0x9f);
OA_ASSERT_OFFSET(PlayerSetupInfo, energy_hundreds, 0xa1);
OA_ASSERT_OFFSET(PlayerSetupInfo, metal_hundreds, 0xa3);
OA_ASSERT_OFFSET(PlayerSetupInfo, max_units, 0xa5);
OA_ASSERT_OFFSET(PlayerSetupInfo, version_major, 0xa7);
OA_ASSERT_OFFSET(PlayerSetupInfo, version_minor, 0xa8);
OA_ASSERT_OFFSET(PlayerSetupInfo, map_hash, 0xa9);
OA_ASSERT_OFFSET(PlayerSetupInfo, engine_signature, 0xad);
OA_ASSERT_OFFSET(PlayerSetupInfo, reserved_after_signature, 0xaf);
OA_ASSERT_OFFSET(PlayerSetupInfo, recorder_protocol, 0xb4);
OA_ASSERT_OFFSET(PlayerSetupInfo, chat_signature, 0xb5);
OA_ASSERT_OFFSET(PlayerSetupInfo, chat_flags, 0xb7);
OA_ASSERT_OFFSET(PlayerSetupInfo, reserved_after_chat_flags, 0xb8);

// One unit-content record keyed by FBI hash.
struct UnitSyncRecord {
    uint32_t key{};
    uint32_t checksum{}; // host: the unit's content checksum, 0 until a peer's is compared
    uint8_t local{};     // the unit exists here
    uint8_t local_high{};
    uint8_t remote{}; // the host reported it
    uint8_t remote_high{};
    int32_t limit{}; // build limit, -1 unlimited
};

inline constexpr std::size_t kMaxSyncUnits = 1024;

// Handshake progress of one peer as seen by the host.
struct UnitSyncPeer {
    uint32_t player_id{};
    uint32_t expected{};                 // subtype 1: def count
    uint32_t received{};                 // subtype 2 entries accepted
    uint32_t sent{};                     // greeting and verdicts sent to this peer
    uint32_t acknowledged{};             // subtype 4: highest received count reported
    uint32_t keys[kMaxSyncUnits]{};      // subtype 2 keys in arrival order, received of them
    uint32_t checksums[kMaxSyncUnits]{}; // subtype 2 values, parallel to keys
};

// The battle room's unit-content handshake.
struct UnitSync {
    bool host{};
    bool finished{};
    uint32_t records_handled{};
    int32_t next_unit{}; // client: unit types reported so far
    UnitSyncRecord records[kMaxSyncUnits]{};
    int32_t record_count{};
    uint32_t pending[kMaxSyncUnits]{}; // keys updated since the restrict panel looked
    int32_t pending_count{};
    UnitSyncPeer peers[kSlotCount]{};
    int32_t peer_count{};
};

// A unit type as the lobby sees it (a projection of UnitDef).
struct LobbyUnit {
    const char* name{};          // UnitDef.name
    const char* side{};          // UnitDef.side
    float cost_energy{};         // UnitDef.build_cost_energy
    float cost_metal{};          // UnitDef.build_cost_metal
    uint32_t abilities{};        // UnitDef.abilities; bit 15 norestrict, bit 16 disabled by default
    uint32_t fbi_hash{};         // UnitDef.fbi_hash
    const char* unit_name{};     // UnitDef.unit_name; names its script, GUI and download files
    uint32_t content_checksum{}; // UnitDef.content_checksum, 0 until worked out
    uint32_t weapon_checksum{};  // UnitDef.weapon_checksum
};

inline constexpr uint32_t kUnitNoRestrict = 1U << 15;
inline constexpr uint32_t kUnitDisabledDefault = 1U << 16;

struct DisplayMode {
    int32_t width{};
    int32_t height{};
    int32_t depth{};
};

// Services other than the network used by the lobby rules.
struct LobbyServices {
    void* context{};
    void (*play_sound)(void* context, const char* name){}; // interface sound by name
    void (*message)(void* context, const char* text){};    // modal message box
    // 30 Hz game clock. It reads as base::game_loop::scaled_clock does and
    // turns over to 0 past base::game_loop::scaled_clock_turn; the lobby's
    // timers compare its readings with lobby_clock_passed.
    uint32_t (*tick)(void* context){};
    int32_t (*display_modes)(void* context, DisplayMode* out, int32_t capacity){};
    // Lobby notifications the game reacts to (sound cues): 2 player added,
    // 4 side or alliance changed, 5 map changed, 10 game starting.
    void (*notify)(void* context, int32_t event){};
    // A unit's content checksum from its installed files and its weapon
    // checksum; one already worked out is returned as it is.
    uint32_t (*unit_checksum)(void* context, const LobbyUnit& unit){};
    // A PCX as a picture; 0 when it cannot be read.
    oa_ref32 (*load_picture)(void* context, const char* path, int32_t* width, int32_t* height){};
    void (*free_picture)(void* context, oa_ref32 picture){};
    // Language variant lookups of picture paths.
    const data::campaign::CampaignFiles* files{};
    /// A millisecond clock, which stamps the battle room's pings; null
    /// reads the 30 Hz clock in milliseconds.
    uint32_t (*milliseconds)(void* context){};
    /// Translates interface text into the game's language, as the game's
    /// translate.tdf table gives it; the result stays valid until the next
    /// call. Null, or a null result, leaves the text as it is.
    const char* (*translate)(void* context, const char* text){};
    /// Draws a value from 0 to bound - 1 for the teams +autoteam and
    /// +randomteam deal; null deals in slot order.
    uint32_t (*random_below)(void* context, uint32_t bound){};
    /// Reads the file the host's .base names: an absolute host path when one
    /// is there, or else a file of the game's folders. Null, or false back,
    /// reads nothing.
    ///
    /// @param context LobbyServices.context
    /// @param name the file's name as typed
    /// @param limit the most bytes read; a longer file is not read
    /// @param[out] contents the file's bytes
    /// @return true when the file was read
    bool (*read_file)(void* context, const char* name, std::size_t limit, std::string* contents){};
};

// The multiplayer map context (Game.game_options).
struct LobbyMaps {
    void* context{};
    bool (*selected)(void* context){};
    const char* (*name)(void* context){};
    bool (*select)(void* context, const char* name){};
    uint32_t (*content_hash)(void* context){};
    // Memory the selected map needs, in MB.
    int32_t (*memory_mb)(void* context){};
    const char* (*description)(void* context){};
    const char* (*size_text)(void* context){};
    int32_t (*count)(void* context){};
    const char* (*at)(void* context, int32_t index){};
    // The map context itself, bound to the selected map; null without one.
    const data::campaign::CampaignFile* (*map_context)(void* context){};
    // Bytes of a map file at an offset; false when fewer are there.
    bool (*read_map)(void* context, const char* path, uint32_t offset, void* out, uint32_t size){};
    /// Why the named map was refused the last time it was chosen.
    ///
    /// Null, or a null or empty result, means the map was not refused. Set
    /// this by member name after the positional binding: later members are
    /// appended here, and a positional list would assign them.
    ///
    /// @param context LobbyMaps.context
    /// @param name the map's name
    /// @return the reason, valid until the next choice; null when there is none
    const char* (*refusal)(void* context, const char* name){};
    /// The context pack is called with; the map packs' own, apart from context.
    void* pack_context{};
    /// On the host: the map pack of the selected map, which this machine's
    /// presence record names. The battle room asks it once each time the
    /// selected map's name changes and once after each binding
    /// (multiplayer_bind_map_pack), never every frame, and keeps the answer
    /// between. False back, or null, for a map of no pack.
    ///
    /// @param context pack_context
    /// @param[out] out the selected map's pack
    /// @return true when the selected map comes from a pack
    bool (*pack)(void* context, netgame::PresenceMapPack* out){};
};

/// Returns the memory, in MB, a map needs by the size of its terrain file.
///
/// @param terrain_bytes Size of the map's TNT file in bytes.
/// @return 16, 24, 32, 48, 64 or 128.
[[nodiscard]] int32_t map_memory_mb(int32_t terrain_bytes) noexcept;

// The launch the connection screens, the battle room and the
// session read, and what they ask of the launch that made it.
struct LaunchLink {
    /// The game's launch block; null reads as an empty block.
    oa::ui::frontend_multiplayer::launch::LaunchBlock* block{};
    void* context{};
    /// Tells whether a launch is active: a launch written into the block
    /// started the running game; null answers false.
    bool (*launch_active)(void* context){};
    /// A join the active launch asked for failed (its host was not found):
    /// shows `text` and leaves the launched battle; null does neither.
    void (*join_failed)(void* context, const char* text){};
    /// Asks the next frontend pass to set application mode `mode`, as the
    /// returns from a launched game set mode 1; null asks nothing.
    void (*request_app_mode)(void* context, int32_t mode){};
    /// Leaves the game and ends the program with exit status 0: the session
    /// closes, and with `with_reason` the local player's disconnect reason,
    /// when there is one, shows first; without it an open session hears
    /// outcome event 8 as it closes. Null leaves nothing.
    void (*leave_game)(void* context, bool with_reason){};
};

inline constexpr std::size_t kChatLines = 30;
inline constexpr std::size_t kChatLineBytes = 0x48;

/// The battle room buttons a mod's display rules may add (ui.share-dialog-
/// and-lobby-buttons), as bits of Lobby::lobby_buttons; each works only when
/// the battle room's GUI has a gadget of its name.
namespace lobby_button {
inline constexpr uint8_t autoteam = 0x01;   ///< AUTOTEAM says "+autoteam"
inline constexpr uint8_t autopause = 0x02;  ///< AUTOPAUSE says ".autopause"
inline constexpr uint8_t randomteam = 0x04; ///< RANDOMTEAM says "+randomteam"
inline constexpr uint8_t crcreport = 0x08;  ///< CRCREPORT says ".crcreport"
} // namespace lobby_button

/// The battle room's presence records (oa/netgame/presence.hpp): the one
/// this machine sends and the one each OA player of another machine sent.
///
/// This machine's record goes to every human player of another machine
/// whose own setup block carries the 'OA' signature (netgame::presence_peer),
/// OA 0.7 machines included, which ignore it; never to a computer player,
/// a machine without the signature or everyone at once. It goes alone in
/// its frame, once each time it changes and once to each such player who
/// arrives later. The setup blocks carry no presence. Each slot's entries
/// stay with its player when the battle room moves players between slots.
struct PresenceRecords {
    /// This machine's record as the app binds it, without a map pack
    /// (multiplayer_bind_presence_record); source_size bytes of it are set.
    uint8_t source[netgame::presence_record_max_bytes]{};
    uint16_t source_size{}; ///< 0 until a record is bound
    /// The record this machine sends: the source and, on the host, the map
    /// pack of its selected map (presence_refresh); local_size bytes are set.
    uint8_t local[netgame::presence_record_max_bytes]{};
    uint16_t local_size{}; ///< 0 while no record is bound: nothing is sent
    /// Counts the changes of local, from 1; 0 while it never held a record.
    uint32_t local_generation{};
    /// The record each slot's player sent; peer_size[slot] bytes are set.
    uint8_t peer[kSlotCount][netgame::presence_record_max_bytes]{};
    uint16_t peer_size[kSlotCount]{}; ///< 0 for no record
    uint32_t peer_id[kSlotCount]{};   ///< the player the stored record came from
    /// The player whose own setup block each slot holds: set as its 0x20 is
    /// applied, cleared as a player is seated in the slot or leaves it. Until
    /// then the slot's block may still hold an earlier player's bytes, so
    /// presence_peer reads it only for this player.
    uint32_t block_player[kSlotCount]{};
    /// The generation of local last sent to each slot's player.
    uint32_t sent_generation[kSlotCount]{};
    uint32_t sent_to[kSlotCount]{}; ///< the player it was sent to; 0 for none
    /// On the host: the selected map's name when LobbyMaps::pack was last
    /// asked, cut to fit.
    char pack_map[sizeof(PlayerSetupInfo::map_name)]{};
    /// The answer: the value of the map-pack field (tag 0x40), pack_size
    /// bytes of it set; 0 for a map of no pack.
    uint8_t pack[netgame::presence_map_pack_field_max_bytes]{};
    uint16_t pack_size{}; ///< 0 for no pack
    /// A map-pack binding has come since pack was last asked: the next
    /// presence_refresh asks it again.
    bool pack_rebound{};
    /// The sim hash of 3.1c's own rules: the plain baseline's, the profile
    /// a game without a mod resolves to (base_game_profile_text in
    /// oa/data/mod_profile/overrides.hpp), as this machine's presence
    /// record carries it when it plays 3.1c. lobby_reset sets it; nothing
    /// while the baseline does not resolve. Under a 3.1c host the rules dot
    /// compares a player's record with it (lobby_rules_differ_from_host).
    std::optional<netgame::PresenceDigest> plain_sim_hash{};
    /// The host's loadout as this machine holds it (presence_assembly_take).
    /// A guest's copy of the list the host's records have carried. Empty
    /// until a host record with a mutator field is kept, and emptied when a
    /// host record carries none.
    netgame::PresenceLoadoutAssembly host_loadout{};
    /// The host record last folded into host_loadout, so a record that
    /// arrived before the host slot was known is folded when it is.
    uint16_t host_fed_size{};
    uint64_t host_fed_hash{};
    /// Counts the changes of host_loadout, from 0, lobby_reset's emptying
    /// among them. A guest copies the assembly when this changes.
    uint32_t host_loadout_generation{};
    /// The full mutator field this machine sends parts from. The bound
    /// source may be cut to leave room for a map pack; a part is drawn from
    /// this, which keeps every mutator. Empty when this machine sends none.
    std::optional<netgame::PresenceMutators> part_source{};
    /// The loadout digest last named when a mutator part would not fit
    /// beside the map pack. One line is logged for each digest.
    std::optional<netgame::PresenceDigest> part_refused_digest{};
};

/// How the battle room's chat list measures a line it is to show
/// (multiplayer_bind_chat_measure). The list draws one stored line on one
/// row and clips the rest, so a line the room posts is fitted to it first.
struct ChatMeasure {
    void* context{};
    /// The room the list has for a line, in the units width gives; 0 or
    /// less while it is not known.
    int32_t (*room)(void* context){};
    /// The width a UTF-8 line takes in the list once posted: with Unicode
    /// chat off, in the code page it is stored in.
    int32_t (*width)(void* context, std::string_view line){};
};

/// The brain one computer player plays, as the battle room shows it.
///
/// Absent on a slot means the built-in player. Another machine does not need
/// the brain installed: the name, version and level travel in the presence
/// record.
struct ComputerBrain {
    std::string id;        ///< the brain's id
    std::string name;      ///< the brain's name
    std::string version;   ///< the brain's version; may be empty
    std::string level_id;  ///< the level's id
    std::string level;     ///< the level's name
    bool full_awareness{}; ///< the brain sees every unit
};

struct Lobby {
    Game* game{};
    PlayerSetupInfo infos[kSlotCount];
    LobbyNet net;
    LobbyServices services;
    LobbyMaps maps;
    UnitSync sync;
    LobbyUnit* units{}; // index 0 is the reserved empty type
    int32_t unit_count{};
    bool rows_built{}; // rows cloned for all slots
    bool option_4{};   // a tournament game: the launch's tournament field is set
    uint32_t last_player_count{};
    int16_t start_frame{}; // battlestart animation frame
    uint32_t start_frame_tick{};
    uint32_t next_stats_tick{};
    uint8_t confirm_slot{};          // YesOrNo target
    uint32_t timeout_player{};       // TIMEOUT.GUI target player id
    uint32_t timeout_refresh_tick{}; // the tick TIMEOUT.GUI is next redrawn after
    /// The player the stall scan at the end of the last pump found
    /// (lobby_check_timeouts), which TIMEOUT.GUI follows; 0 for none.
    uint32_t stalled_player{};
    /// The 30 Hz time the stall scan last found the game paused; a player
    /// heard from before it counts as silent from it instead.
    uint32_t timeout_baseline{};
    uint32_t local_version_major{}; // the build version, as Game.version_block holds it
    uint32_t local_version_minor{};
    int32_t row_base{}; // panel count before the rows were cloned
    /// A joiner being refused, which is not seated: while it is set, a
    /// record to everyone that goes out per machine goes to it too. 0 for
    /// none.
    uint32_t refused_joiner{};
    /// The launch and its service.
    LaunchLink launch_link;
    /// SIDEDATA names by side number. A side past the battle room's art shows its name.
    std::vector<std::string> side_names;
    /// The mod profile's rules the battle room keeps; null keeps 3.1c's.
    const data::match_rules::MatchRules* rules{};
    /// Whether map-placed units are on (+spawnon, +spawnoff) while the
    /// profile places them.
    bool map_units_on{true};
    /// The battle room already seated a computer player for a map's neutral units.
    bool neutral_computer_seated{};
    /// The battle room buttons a mod's display rules add, as lobby_button
    /// bits; none in 3.1c.
    uint8_t lobby_buttons{};
    /// The game's network rules: the version bytes and how they compare, the
    /// private chat channel and the recorder. Value-initialised: 3.1c.
    netgame::WireRules wire_rules{};
    /// The recorder's session, which the match takes over at the start.
    netgame::RecorderSession recorder{};
    /// This machine sends and reads chat as UTF-8: the blocks it sends say
    /// so (netgame::announce_unicode_chat) and each machine gets a line in
    /// the form it reads (lobby_say). Not one of the game's rules: players
    /// with it on and off play together.
    bool unicode_chat{};
    /// The presence record this machine sends to the OA players of other
    /// machines, and the ones they sent (PresenceRecords).
    PresenceRecords presence_records{};
    /// The mutator hold last logged for this host, and whether one was logged.
    MutatorsBlock mutators_hold_logged{};
    bool mutators_hold_seen{};
    /// The room has posted its lines for the host's current mutator field.
    /// Cleared when that field goes, so the next one posts once.
    bool classic_lines_posted{};
    /// How the chat list measures the lines about a 3.1c player
    /// (lobby_post_classic_line); unbound, they are cut by bytes alone.
    ChatMeasure chat_measure{};
    /// Difficulty of a computer player in this slot, 0 easy, 1 medium, 2 hard.
    /// Shown before the start. Zero, and computer_difficulty_set false, until
    /// this machine seats the player or a presence record names it.
    uint8_t computer_difficulty[kSlotCount]{};
    bool computer_difficulty_set[kSlotCount]{};
    /// The brain a computer player in this slot plays. Empty is the built-in
    /// player. This machine sets its own; a peer's presence record sets the
    /// computers that peer runs. A slot swap carries the brain with the player.
    std::array<std::optional<ComputerBrain>, kSlotCount> computer_brain{};
    /// The brain id a launch named for the computer player in this slot.
    /// Empty leaves the remembered seat and the default. A slot swap carries
    /// it with the player, and clearing the slot forgets it.
    std::array<std::string, kSlotCount> computer_launch_brain{};
    /// A change the host asked of another machine's computer player, kept
    /// until that machine echoes it. Kind 0 is none. The player id names the
    /// computer player, so a slot swap does not move the command.
    uint32_t computer_command_player{};
    uint8_t computer_command_kind{};
    uint8_t computer_command_value{};
    /// The side last named for this slot in the line about a side this game
    /// does not have, or -1 when nothing was posted.
    int16_t side_past_table_posted[kSlotCount]{-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
    /// A catalogue line the game list shows once after this machine leaves a
    /// room by itself. Empty when it did not.
    const char* leave_line{};
    /// Set once this host has posted the newer-build line for the player in
    /// this slot. A later record from that player does not post it again.
    bool limits_older_refused[kSlotCount]{};
};

// ---------------------------------------------------------------------------
// Game-state accessors: unaligned views of fields in the packed Game and
// Player records.

// Unaligned little-endian view of one field inside a packed record.
template <class T>
struct FieldRef {
    uint8_t* at{};

    /// Reads the field.
    ///
    /// @return The field's value.
    operator T() const noexcept {
        T value{};
        std::memcpy(&value, at, sizeof(T));
        return value;
    }

    /// Writes the field.
    ///
    /// @param value Value to store.
    /// @return This view.
    FieldRef& operator=(T value) noexcept {
        std::memcpy(at, &value, sizeof(T));
        return *this;
    }
};

/// Returns the count of seated players (Game.player_count).
[[nodiscard]] FieldRef<uint16_t> lobby_player_count(Game& game) noexcept;
/// Returns the chat ring's head index (Game.chat_head).
[[nodiscard]] FieldRef<uint16_t> lobby_chat_head(Game& game) noexcept;
/// Returns the chat ring's tail index (Game.chat_tail).
[[nodiscard]] FieldRef<uint16_t> lobby_chat_tail(Game& game) noexcept;
/// Returns one line of the 30-line chat ring (Game.chat_lines); the index wraps.
[[nodiscard]] char* lobby_chat_line(Game& game, std::size_t index) noexcept;
/// Returns the game name (Game.game_name, 17 bytes).
[[nodiscard]] char* lobby_game_name(Game& game) noexcept;
/// Returns the local nickname (Game.nickname, 17 bytes).
[[nodiscard]] char* lobby_nickname(Game& game) noexcept;
/// Returns the game password (Game.password, 11 bytes).
[[nodiscard]] char* lobby_password(Game& game) noexcept;
/// Returns the lobby option word (Game.setup_options; bit 0 locks the host's options).
[[nodiscard]] FieldRef<uint16_t> lobby_option_word(Game& game) noexcept;
/// Returns the unit limit of the game (Game.max_units).
[[nodiscard]] FieldRef<uint16_t> lobby_max_units(Game& game) noexcept;
/// Returns the configured unit limit (Game.max_units_setting).
[[nodiscard]] FieldRef<uint16_t> lobby_max_units_default(Game& game) noexcept;
/// Returns the local screen width (Game.screen_width).
[[nodiscard]] FieldRef<int32_t> lobby_screen_width(Game& game) noexcept;
/// Returns the local screen height (Game.screen_height).
[[nodiscard]] FieldRef<int32_t> lobby_screen_height(Game& game) noexcept;
/// Returns the stored provider GUID (Game.provider_guid, 16 bytes).
[[nodiscard]] uint8_t* lobby_provider_guid(Game& game) noexcept;
/// Returns the session's player limit (Game.session_player_limit).
[[nodiscard]] FieldRef<int32_t> lobby_session_players(Game& game) noexcept;
/// Returns a player's ping (Player.latency).
[[nodiscard]] FieldRef<int32_t> lobby_player_ping(Player& player) noexcept;
/// Returns the tick a player was seated at (Player.join_tick).
[[nodiscard]] FieldRef<uint32_t> lobby_player_join_tick(Player& player) noexcept;
/// Returns a player's team, 0..4 or kNoTeam (Player.team).
[[nodiscard]] uint8_t& lobby_player_team(Player& player) noexcept;
/// Returns a player's allied-back table, one byte per slot (Player.allied_by).
[[nodiscard]] uint8_t* lobby_player_allied_back(Player& player) noexcept;
/// Returns the number of playable sides (Game.side_count), capped at 255.
[[nodiscard]] uint8_t lobby_side_count(const Game& game) noexcept;

/// Returns the first side the SIDE button offers at or after `side`: a side
/// the mod's profile keeps out of the battle room is passed over.
/// `also_hidden` passes over more sides the same way.
///
/// @param game Game whose side count bounds the search.
/// @param side The side to start from.
/// @param also_hidden Extra sides to pass over, bit n for side n.
/// @return the side offered; the side count when none is left after `side`
[[nodiscard]] uint8_t
lobby_offered_side(const Game& game, uint32_t side, uint32_t also_hidden = 0) noexcept;

/// Returns the nearest side the SIDE button offers below `side`.
///
/// A side the mod's profile keeps out of the battle room is passed over.
/// `also_hidden` passes over more sides the same way.
///
/// @param game Game whose side count bounds the search.
/// @param side The side to step back from.
/// @param also_hidden Extra sides to pass over, bit n for side n.
/// @return the side offered below `side`; the side count when none is
uint8_t
lobby_offered_side_before(const Game& game, uint32_t side, uint32_t also_hidden = 0) noexcept;

/// Sides this machine's computer players may not take.
///
/// Bit n is set when side n is outside identity.computer-player-sides.
/// 0 means every side, as when that key is absent or empty.
///
/// @return the mask, or 0 when computer players may take every side
[[nodiscard]] uint32_t lobby_computer_hidden_sides() noexcept;

// Game options copied from the host's option word when a game starts.
struct StartedOptions {
    uint32_t commander{};   // Game.start_options[0]
    uint32_t unmapped{};    // Game.start_options[1]
    uint32_t los_limited{}; // Game.start_options[2]
    uint32_t los_true{};    // Game.start_options[3]
    uint32_t fixed_locations{};
};

/// Returns the game options stored when the last game started; fixed_locations is always 0.
[[nodiscard]] StartedOptions lobby_started_options(Game& game) noexcept;

/// Resolves a slot's Player.info handle.
///
/// @param lobby Lobby state.
/// @param slot Player slot, 0..9.
/// @return The slot's lobby block, or null for a slot out of range or an unset handle.
[[nodiscard]] PlayerSetupInfo* slot_info(Lobby& lobby, int32_t slot) noexcept;

/// Resolves a player's Player.info handle.
///
/// @param lobby Lobby state.
/// @param player Player record.
/// @return The player's lobby block, or null when the handle is unset or out of range.
[[nodiscard]] PlayerSetupInfo* player_info(Lobby& lobby, const Player& player) noexcept;

/// Returns the player record of a slot, clamped to 0..9.
[[nodiscard]] Player& slot_player(Lobby& lobby, int32_t slot) noexcept;
/// Returns the local player's record.
[[nodiscard]] Player& local_player(Lobby& lobby) noexcept;
/// Returns the local player's lobby block (block 0 when its handle is unset).
[[nodiscard]] PlayerSetupInfo& local_info(Lobby& lobby) noexcept;

/// Prepares an empty lobby: every slot open and cleared, infos bound, no colours or teams, local slot 0.
///
/// No player is stalled, and a game with no player timeout gets
/// kDefaultPlayerTimeoutSeconds. The presence records other players sent
/// and what was sent to them are forgotten; this machine's own record stays.
/// The previous host's mutator list is emptied as a change of it, and the
/// next host's record is folded even when its bytes are the same.
/// The plain baseline's sim hash is set (PresenceRecords::plain_sim_hash),
/// resolved the first time a lobby is reset and kept for the run.
///
/// @param[out] lobby Lobby to reset; it is bound to game.
/// @param[in,out] game Game block whose player records and chat ring are cleared.
void lobby_reset(Lobby& lobby, Game& game) noexcept;

/// Puts the local player in a slot as host or client with its nickname.
///
/// The host takes the first free colour; a client waits for the host's.
///
/// @param[in,out] lobby Lobby state.
/// @param slot Player slot, 0..9.
/// @param host True to take the host role.
/// @param nickname Player name, or null for none.
void lobby_seat_local(Lobby& lobby, int32_t slot, bool host, const char* nickname) noexcept;

// ---------------------------------------------------------------------------
// Slot predicates

/// Tells whether a slot is live: status local, computer or remote, and it has a player index.
///
/// @param player Player record.
/// @return True for a live slot.
[[nodiscard]] bool slot_active(const Player& player) noexcept;

/// Tells whether a remote slot's info reports it still playing.
///
/// @param lobby Lobby state.
/// @param player Player record.
/// @return True for an occupied remote slot in info state 1.
[[nodiscard]] bool slot_remote_playing(Lobby& lobby, const Player& player) noexcept;

/// Tells whether a remote slot's info reports it defeated.
///
/// @param lobby Lobby state.
/// @param player Player record.
/// @return True for an occupied remote slot in info state 2.
[[nodiscard]] bool slot_remote_defeated(Lobby& lobby, const Player& player) noexcept;

/// Tells whether an active slot still has units or never built any.
///
/// @param player Player record.
/// @return True for a participating slot.
[[nodiscard]] bool slot_participating(const Player& player) noexcept;

/// Finds the first non-open slot flagged host in its info.
///
/// @param lobby Lobby state.
/// @return The slot, or kNoSlot.
[[nodiscard]] uint8_t lobby_host_slot(Lobby& lobby) noexcept;

/// Sets Player.status, mirroring it into PlayerSetupInfo.state unless remote.
///
/// @param[in,out] lobby Lobby state.
/// @param[in,out] player Player record to change.
/// @param status New status.
void slot_set_status(Lobby& lobby, Player& player, uint8_t status) noexcept;

/// Tells whether a launch arranged the game: option word bit 0 and the second gate both set.
///
/// The second gate is the launch's tournament field (Lobby::option_4).
///
/// @param lobby Lobby state.
/// @return True for a game a launch arranged.
[[nodiscard]] bool lobby_launch_locked(Lobby& lobby) noexcept;

/// Tells whether the game's options are locked: option word bit 0, which a launch's lock sets.
///
/// @param lobby Lobby state.
/// @return True while the host's option controls stay locked.
[[nodiscard]] bool lobby_options_locked(Lobby& lobby) noexcept;

/// Tells whether a launch is active (LaunchLink::launch_active).
///
/// @param lobby Lobby state and its launch link.
/// @return The link's answer; false without one.
[[nodiscard]] bool lobby_launch_active(const Lobby& lobby) noexcept;

/// Tells whether a launch is active over a launch block: the lobby has the block and lobby_launch_active.
///
/// @param lobby Lobby state and its launch link.
/// @return False without a launch block.
[[nodiscard]] bool lobby_launch_block_active(const Lobby& lobby) noexcept;

/// Tells whether a host's version byte lets the local game join.
///
/// By 3.1c's rules the host's major must be at least the local one, and a
/// launch-only session's major is biased by 100. A game whose rules compare
/// for equality needs the same major, and without the launch bias reads the
/// byte as it is.
///
/// @param info The host's lobby block.
/// @param local_major Local major version, compared as a signed byte.
/// @param rules The local game's network rules; 3.1c's by default.
/// @return True when the local game may join.
[[nodiscard]] bool info_version_compatible(
    const PlayerSetupInfo& info, uint32_t local_major, const netgame::WireRules& rules = {}
) noexcept;

/// Reads a battle-room chat line for the recorder's commands, as every recorder reads each line it
/// sends or hears.
///
/// Only with the recorder presented (Lobby::wire_rules). Host commands
/// (.syncon, .syncoff, .autopause, .cmdwarp, .f1off) change the session's
/// options when the host sent them, and the host's own machine answers with
/// a chat line saying so; .cmdwarp turns the warp on or off, and only under
/// setup.commander-warp. Under setup.recorder-prebuilt-base the host's .base
/// offers every seated player the standard base, or the base file it names
/// (LobbyServices::read_file), and .baseoff takes every base back for the
/// session. .report is answered by this machine for its player; .votego and
/// .forcego make watchers count as ready; the host's .randmap picks a map
/// from the list at random. The opt-in session commands need
/// network.recorder-session-commands, and .syncon and .syncoff need
/// WireRules::speed_lock.
///
/// @param[in,out] lobby Lobby state.
/// @param sender_slot Slot of the player who sent the line; -1 reads nothing.
/// @param text The line, as shown ("<Name> .cmd args").
/// @return True when the line held a command this machine read.
bool recorder_chat_line(Lobby& lobby, int32_t sender_slot, const char* text) noexcept;

/// Notes a player's battle-room setup block for the recorder: the player's recorder protocol, and on
/// the host's machine the host's options sent once to a player whose recorder takes them.
///
/// @param[in,out] lobby Lobby state.
/// @param slot The player's slot.
void recorder_note_block(Lobby& lobby, int32_t slot) noexcept;

/// Reads a recorder record another machine sent to the battle room: the host's options and warp-done.
///
/// The host's speed lock among its options is taken only under WireRules::speed_lock.
///
/// @param[in,out] lobby Lobby state.
/// @param sender_slot Slot of the sender; -1 reads nothing.
/// @param data The record.
/// @param size Its length.
/// @return True when the record changed the session.
bool recorder_lobby_record(
    Lobby& lobby, int32_t sender_slot, const uint8_t* data, std::size_t size
) noexcept;

/// Returns the password field of the local lobby block.
///
/// @param lobby Lobby state.
/// @return The 11-byte field.
[[nodiscard]] char* local_password_field(Lobby& lobby) noexcept;

/// Tells whether this machine has the host's map.
///
/// @param lobby Lobby state.
/// @return False when no map is selected; true when the host predates map hashes (before 1.2) or the
///         local map hash matches the host's.
[[nodiscard]] bool lobby_has_host_map(Lobby& lobby) noexcept;

// ---------------------------------------------------------------------------
// Colours, teams and alliances

/// Returns the first colour no active slot uses.
///
/// @param lobby Lobby state.
/// @return The colour, 0..9; colours above 9 count as 9, and 0 when all are used.
[[nodiscard]] int32_t lobby_next_free_color(Lobby& lobby) noexcept;

/// Counts the active slots on a team; once a watched game started only participating slots count.
///
/// @param lobby Lobby state.
/// @param team Team 0..4; team 5 (none) counts zero.
/// @return The member count.
[[nodiscard]] int32_t team_member_count(Lobby& lobby, uint32_t team) noexcept;

/// Tells whether some team holds every playing slot (players, computers and defeated remotes).
///
/// @param lobby Lobby state.
/// @return True when one team has them all.
[[nodiscard]] bool lobby_all_on_one_team(Lobby& lobby) noexcept;

/// Finds the next slot from a start that shares a slot's team, reaching the slot itself last.
///
/// @param lobby Lobby state.
/// @param slot Slot whose team is searched.
/// @param start First slot to look at.
/// @return The mate's slot, slot itself when it is reached first, or -1.
[[nodiscard]] int32_t next_team_mate(Lobby& lobby, int32_t slot, int32_t start) noexcept;

/// Sets the stage of every TEAMICONSn control: team * 2 (+1 when alone), 10 without a team.
///
/// @param[in,out] lobby Lobby state.
/// @param[in,out] panel The battle room panel.
void lobby_update_team_icons(Lobby& lobby, Panel& panel) noexcept;

/// Marks mutual alliance and allied victory between team mates; clears allied victory for a player alone.
///
/// @param[in,out] lobby Lobby state.
void lobby_update_ally_matrix(Lobby& lobby) noexcept;

/// Breaks the alliances a slot held through its current team, on both sides.
///
/// @param[in,out] lobby Lobby state.
/// @param slot Slot leaving its team.
void lobby_leave_team(Lobby& lobby, int32_t slot) noexcept;

/// Advances a slot's team (0..4, then none), republishes it and refreshes the alliances and icons.
///
/// @param[in,out] lobby Lobby state.
/// @param[in,out] panel The battle room panel.
/// @param slot Slot whose team changes.
void lobby_cycle_team(Lobby& lobby, Panel& panel, int32_t slot) noexcept;

// ---------------------------------------------------------------------------
// Slot table maintenance

/// Swaps two player records, opens the first, and renumbers every slot's index.
///
/// The game renumbers eleven records; the eleventh lies past the table and
/// is not touched here. The slots' presence entries are swapped with them.
///
/// @param[in,out] lobby Lobby state.
/// @param first Slot that ends up open, holding the second's old record.
/// @param second Slot that receives the first's record.
void lobby_swap_slots(Lobby& lobby, int32_t first, int32_t second) noexcept;

/// Moves active slots in front of open ones, leaving closed slots in place.
///
/// @param[in,out] lobby Lobby state.
void lobby_compact_slots(Lobby& lobby) noexcept;

// ---------------------------------------------------------------------------
// Battleroom panel

/// Clones the PLAYERx..TEAMICONSx template row for all ten slots, shaping each row for its slot.
///
/// @param[in,out] lobby Lobby state.
/// @param[in,out] panel The battle room panel.
void lobby_build_rows(Lobby& lobby, Panel& panel) noexcept;

/// Converts a button into a label showing its caption, two pixels to the right.
///
/// @param[in,out] control Control to convert; other types are left alone.
void control_make_label(Control& control) noexcept;

/// Sets the COMMANDER..GAMEOPEN stages from the host's option word (the local one without a host).
///
/// @param lobby Lobby state.
/// @param[in,out] panel The battle room panel.
void lobby_update_option_buttons(Lobby& lobby, Panel& panel) noexcept;

/// Sets a slot's SIDEn stage: the slot's side, or 2 for a watcher.
///
/// @param lobby Lobby state.
/// @param[in,out] panel The battle room panel.
/// @param slot Player slot.
void lobby_update_side_button(Lobby& lobby, Panel& panel, int32_t slot) noexcept;

/// Handles the MAXUNITS slider: shows the host's value, or the local knob + 20 for the host, and stores it.
///
/// The host republishes its lobby block.
///
/// @param[in,out] panel The battle room panel.
/// @param lobby The Lobby, as the slider's user pointer.
void lobby_on_max_units(Panel& panel, void* lobby) noexcept;

/// Handles the METAL slider: rounds down to hundreds, shows and stores it; the host republishes.
///
/// @param[in,out] panel The battle room panel.
/// @param lobby The Lobby, as the slider's user pointer.
void lobby_on_metal(Panel& panel, void* lobby) noexcept;

/// Handles the ENERGY slider: rounds down to hundreds, shows and stores it; the host republishes.
///
/// @param[in,out] panel The battle room panel.
/// @param lobby The Lobby, as the slider's user pointer.
void lobby_on_energy(Panel& panel, void* lobby) noexcept;

/// Binds a slider's maximum, knob value and handler, then runs the handler.
///
/// @param[in,out] lobby Lobby state.
/// @param[in,out] panel The battle room panel.
/// @param name Slider control name.
/// @param maximum Largest value.
/// @param value Initial value.
/// @param handler Change handler, called with the Lobby as its user pointer; may be null.
void lobby_bind_slider(
    Lobby& lobby,
    Panel& panel,
    const char* name,
    int32_t maximum,
    int32_t value,
    SliderHandler handler
) noexcept;

/// Cycles the local screen resolution through the display modes and announces it.
///
/// @param[in,out] lobby Lobby state.
/// @param backwards Step to the previous mode (right click) instead of the next.
void lobby_cycle_resolution(Lobby& lobby, bool backwards) noexcept;

/// Refreshes every row, the chat output, the option buttons and the map name.
///
/// Also enforces the host's rules: deathmatch rejects computer and defeated
/// players, and watchers are dropped when watching is off. A missing host map
/// blinks the name and clears the local ready mark.
///
/// @param[in,out] lobby Lobby state.
/// @param[in,out] panel The battle room panel.
void lobby_update_status(Lobby& lobby, Panel& panel) noexcept;

/// Tells whether every seated slot is READY and not everyone is watching.
///
/// @param lobby Lobby state.
/// @return False without a remote player or with only one seated player.
[[nodiscard]] bool lobby_ready_to_start(Lobby& lobby) noexcept;

/// Sets up LOUNGE2.GUI for the local slot: options, sliders, unit sync, rows and the player info broadcast.
///
/// @param[in,out] lobby Lobby state.
/// @param[in,out] panel The battle room panel.
void lobby_enter_battleroom(Lobby& lobby, Panel& panel) noexcept;

enum class LobbyAction : uint8_t {
    none,
    leave,          // PREVMENU: back to the game list
    start,          // host pressed START with every check passed
    select_map,     // host opens SELMAP.GUI
    view_map,       // client opens VIEWMAP.GUI
    restrictions,   // open RESTRICT2.GUI
    confirm_reject, // open YesOrNo.gui for Lobby::confirm_slot
};

/// Handles a click on the battle room panel (panel.selected is the control).
///
/// Covers the per-slot logo, player, side, ally, team, resolution and ready
/// controls, chat entry, the host's option buttons, START (CD, team and map
/// checks), GAMEOPEN, RESTRICTIONS and the map buttons. The chat line's
/// commands (+syncerr, +page and +p) are matched without case and send no
/// chat. What the click queued is flushed before it returns.
///
/// @param[in,out] lobby Lobby state.
/// @param[in,out] panel The battle room panel.
/// @return What the screen should do next.
LobbyAction lobby_handle_event(Lobby& lobby, Panel& panel) noexcept;

/// The panel in front of the battle room while it ticks.
enum class LobbyFront : uint8_t {
    battleroom, ///< the battle room itself
    view_map,   ///< VIEWMAP.GUI, over the battle room
    dialog,     ///< any other dialog over the battle room
};

/// Runs the per-frame battle room work: incoming records, the stall scan, refresh, START animation, unit
/// sync and the periodic block, then flushes what the frame queued.
///
/// Once the incoming records are applied, the stall scan
/// (lobby_check_timeouts) sets Lobby::stalled_player, whatever is in front,
/// and this machine's presence record is composed again (presence_refresh)
/// and sent to each OA player who has not had it as it is now
/// (presence_send_due).
///
/// About every two seconds, for each seated local or computer player in slot
/// order and from that player: what is queued is flushed, a ping (0x02) goes
/// out at once without guaranteed delivery, then a probe (0x06); while the
/// player shows no colour the local player asks the host for colour 0; the
/// host's own player sends the slot table (0x26). Then every such player's
/// lobby block and team go out (lobby_send_player_info). Each echo of a ping
/// becomes the echoing player's Ping column entry.
///
/// The rows and the session's player count are kept up to date whatever is
/// in front. While a dialog is in front, the battle room's status, its START
/// doors and a client's copy of the host's map and resources wait until the
/// battle room is in front again, as in 3.1c, so a host browsing SELMAP's
/// maps tells the others nothing. A client's open VIEWMAP follows the host's
/// map instead (viewmap_follow_host).
///
/// @param[in,out] lobby Lobby state.
/// @param[in,out] panel The battle room panel.
/// @param front the panel in front of the battle room
/// @param[in,out] view_map VIEWMAP's panel while front is view_map; null otherwise
/// @return leave once the local player was rejected, else none.
LobbyAction lobby_tick(
    Lobby& lobby, Panel& panel, LobbyFront front = LobbyFront::battleroom, Panel* view_map = nullptr
) noexcept;

/// Tells whether the lobby's clock has passed the time a timer waits for.
///
/// The clock (LobbyServices::tick) runs from 0 through
/// base::game_loop::scaled_clock_turn and then turns over to 0, about every
/// 39.8 hours. A reading that has turned over since the time was set has
/// passed it, so a timer set just before the turn runs out at the first
/// reading after it.
///
/// @param tick the clock, in 30 Hz ticks
/// @param due the time waited for: a reading, or a reading plus a wait shorter than half a turn
/// @return true once `tick` lies after `due`
[[nodiscard]] bool lobby_clock_passed(uint32_t tick, uint32_t due) noexcept;

/// Scans the remote players for a stall, as each battle-room pump ends.
///
/// A remote player is stalled once more than Game.player_timeout_seconds
/// have passed since it was last heard from, or since the scan last found
/// the game paused when that is later, counting a turn of the clock to 0
/// between. This machine's own players, its computer players among them,
/// are never stalled. While the game is paused the scan only moves
/// Lobby::timeout_baseline to now, and with the console's "Drop 0" it does
/// not run; either way the player TIMEOUT.GUI names stays as it was.
///
/// @param[in,out] lobby Lobby state.
/// @return The player TIMEOUT.GUI names: the first stalled player in slot order when every stalled
///         player shares one machine group, 0 when none is stalled or they are of several groups;
///         Lobby::stalled_player when the scan does not run.
[[nodiscard]] uint32_t lobby_check_timeouts(Lobby& lobby) noexcept;

/// Settles alliances and drops the unit sync table when the panel closes.
///
/// @param[in,out] lobby Lobby state.
void lobby_leave_battleroom(Lobby& lobby) noexcept;

// ---------------------------------------------------------------------------
// Records the lobby sends and receives

/// Translates interface text through LobbyServices::translate, as 3.1c's
/// gadget text setter translates the texts it sets.
///
/// @param lobby lobby whose services translate
/// @param text text to translate
/// @return its translation, valid until the next call, or the text itself
///     without one
[[nodiscard]] const char* lobby_translated(const Lobby& lobby, const char* text) noexcept;

/// Sends every local and computer slot's lobby block (0x20) and team (0x24) to all players, then the
/// machine-group requests.
///
/// Each slot's records leave from that slot's own player; each team is
/// flushed as it goes, and the requests are flushed at the end. Does
/// nothing outside a live session.
///
/// @param[in,out] lobby Lobby state.
void lobby_send_player_info(Lobby& lobby) noexcept;

/// Broadcasts a local or computer player's team as 0x24 from that player and flushes it.
///
/// @param[in,out] lobby Lobby state.
/// @param player Player whose team is sent; others are ignored.
void lobby_send_team(Lobby& lobby, const Player& player) noexcept;

/// Tells whether no other seated player shows a colour.
///
/// @param lobby Lobby state.
/// @param player_id Player asking; its own colour does not count.
/// @param color Colour 0..9; 0xff and colours outside 0..9 are never available.
/// @return True when the colour is free.
[[nodiscard]] bool lobby_color_available(Lobby& lobby, uint32_t player_id, int32_t color) noexcept;

/// Gives a player the first colour from a start on that no other local, computer or remote slot shows.
///
/// Past 9 the search wraps to 0. The local player takes the colour; anyone
/// else is told with 0x18.
///
/// @param[in,out] lobby Lobby state.
/// @param player_id Player to colour.
/// @param color First colour to try.
/// @return False when the reply cannot be encoded.
/// @quirk After ten taken colours the last candidate, possibly 10, is given anyway.
bool lobby_assign_free_color(Lobby& lobby, uint32_t player_id, uint32_t color) noexcept;

/// Makes the local player's colour choice.
///
/// The host takes it, or the next free one; a client asks the host with
/// 0x17 from the local player, flushed at once, and waits for its 0x18. A
/// click on a computer slot's logo also lands here, and so does the
/// periodic block while a local or computer slot shows no colour.
///
/// @param[in,out] lobby Lobby state.
/// @param color Colour wanted.
void lobby_request_color(Lobby& lobby, uint32_t color) noexcept;

/// Broadcasts the host's slot table (Game.slot_table) as 0x26: each slot's player id, 0 for an open slot
/// and -1 for a closed one.
///
/// Only the host in a live session sends it, from its own player; clients
/// keep the copy they receive.
///
/// @param[in,out] lobby Lobby state.
void lobby_send_slot_table(Lobby& lobby) noexcept;

/// Sends an alliance change as 0x23 from the changing player to the other player and flushes it.
///
/// @param[in,out] lobby Lobby state.
/// @param from Player whose alliance changed.
/// @param to Player the alliance is with; the record goes to it.
/// @param value New alliance value.
/// @param both_sides Nonzero asks the receiver to set its own alliance with from too.
void lobby_send_alliance(
    Lobby& lobby, const Player& from, const Player& to, uint8_t value, uint32_t both_sides
) noexcept;

/// Sets the alliance of one player with another.
///
/// A local or computer side updates its own tables; a remote target is told
/// through record 0x23. A computer or defeated remote target, or both_sides,
/// has the mirror entries set too.
///
/// @param[in,out] lobby Lobby state.
/// @param[in,out] from Player whose alliance changes.
/// @param[in,out] to Player the alliance is with.
/// @param value New alliance value.
/// @param both_sides Set the mirror entries as well.
void lobby_set_alliance(
    Lobby& lobby, Player& from, Player& to, uint8_t value, bool both_sides
) noexcept;

/// Rejects a seated player once: broadcasts 0x1b and retires the slot.
///
/// Records leave from the first seated local or computer player, and none
/// from one already rejected. Nothing is sent for a player no slot answers
/// to or one already rejected, whose reason is only replaced. A computer
/// slot, or a remote one, is named, no longer counted (Game.player_count)
/// and opened; before the start a computer player of this machine also
/// leaves the session, and after it this machine's own slots stay seated. A
/// remote human still playing takes every remote player of its machine
/// group with it. Rejecting the local player names each local and computer
/// player of this machine in slot order and leaves the battle room: before
/// the start the local player leaves at the first record, so the next
/// seated player sends the rest and a computer player seated ahead of it is
/// named instead of it; after the start only the first record goes out.
///
/// @param[in,out] lobby Lobby state.
/// @param player_id Rejected player.
/// @param reason RejectReason value, stored as the slot's reject reason.
void lobby_reject(Lobby& lobby, uint32_t player_id, uint8_t reason) noexcept;

/// Broadcasts a chat line "<name> text" as 0x05 from the speaker, flushes it and posts it locally.
///
/// With Lobby::unicode_chat on the line goes as net_match_say sends one:
/// read as UTF-8, up to four records unless it is a command, and to each
/// machine in the form it reads.
///
/// @param[in,out] lobby Lobby state.
/// @param speaker Player speaking.
/// @param text Line text, as this machine holds game text.
void lobby_say(Lobby& lobby, const Player& speaker, const char* text) noexcept;

/// Appends a line to the chat ring, dropping the oldest when full.
///
/// @param[in,out] lobby Lobby state.
/// @param line Line to post, cut to 71 characters.
void lobby_post_chat(Lobby& lobby, const char* line) noexcept;

/// Builds the session description after stamping the version and player count into the local info.
///
/// A game started while a launch is active sets the launch-only status bit
/// and biases the version by 100; any other clears the bit and leaves the
/// version unbiased.
///
/// @param[in,out] lobby Lobby state.
/// @param[out] name Session name: game name, then the map name at 16, space padded to 31 characters.
/// @param[out] user The 16 user bytes: the local PlayerSetupInfo from memory_mb through version_minor.
void lobby_session_description(Lobby& lobby, char* name, uint8_t* user) noexcept;

/// Publishes the session description with flags 4 and 0x20 once started, and the player limit.
///
/// Only the host's session takes it. A password is never flagged on the
/// session: joiners present it in their join block. The limit is the
/// session's player limit (lobby_session_players), which closing a slot
/// lowers and reopening it raises.
///
/// @param[in,out] lobby Lobby state.
void lobby_publish_session(Lobby& lobby) noexcept;

/// Seats a session player.
///
/// A slot played here that already answers to the id and is not in use is
/// taken over, otherwise the first slot neither in use nor closed becomes
/// remote. The seated player's reject reason clears, its arrival time is
/// stamped, it is counted, and the local players' info goes out again. The
/// slot's own reset (Player and info defaults) follows the lobby
/// rules. Players already in a joined session arrive the same way. A
/// player seated from another machine takes back the prebuilt bases the
/// host offered, which the host's recorder announces. The slot's presence
/// entries are cleared: the player gets this machine's record once its own
/// setup block says it is an OA player.
///
/// @param[in,out] lobby Lobby state.
/// @param player_id Session player id.
/// @param name Player name, or null for none.
/// @return False when no slot can take the player.
bool lobby_add_player(Lobby& lobby, uint32_t player_id, const char* name) noexcept;

/// Keeps the brain a computer player of this machine plays, and its difficulty.
///
/// Only a computer player this machine runs is changed. A remote slot, an
/// empty slot and a slot outside the table are left as they are. An empty
/// brain is the built-in player. The difficulty is noted as a presence
/// record's computer list notes it: 0 easy, 1 medium, 2 hard.
///
/// @param[in,out] lobby Lobby state.
/// @param slot Player slot.
/// @param brain The brain, or nothing for the built-in player.
/// @param difficulty 0 easy, 1 medium or 2 hard.
void lobby_set_computer_brain(
    Lobby& lobby, int32_t slot, std::optional<ComputerBrain> brain, uint8_t difficulty
) noexcept;

/// Returns the brain a computer player in a slot plays.
///
/// @param lobby Lobby state.
/// @param slot Player slot.
/// @return The brain, or null for the built-in player and for a slot outside the table.
[[nodiscard]] const ComputerBrain* lobby_computer_brain(const Lobby& lobby, int32_t slot) noexcept;

/// Seats a computer player in an open slot as a session player of this machine.
///
/// Its session name is "AI:" and the local player's name, sixteen characters
/// at most. Its lobby block is prepared as for any seated player: no colour,
/// the host role only in the host's slot, the player count in the option
/// word's low bits and the game-closed bit off, a unit limit of 100, the
/// password bit, the launch-only status bit while a launch is active (the
/// version stays unbiased), this machine's screen mode and version; the
/// block keeps the rest of the slot's last occupant's options, a watcher bit
/// included. When the session refuses the player the slot is open again and
/// a message advises joining or creating the game again.
///
/// @param[in,out] lobby Lobby state.
/// @param slot Open slot to seat the computer in.
void lobby_add_computer(Lobby& lobby, int32_t slot) noexcept;

/// Applies one received event: arrivals, departures, a lost session or a lobby record.
///
/// A joiner the host refuses is not seated: this machine's lobby blocks go
/// out, then 0x1b with the refusal. A departed player is rejected with
/// reason 1; when it is the host's player before the start, the local
/// player is rejected with reason 10 too and leaves ("The creator has left
/// the game"). A record from one of this machine's own players is ignored;
/// any other record, of whatever type, stamps its seated sender's
/// Player.last_update_time, which the stall scan reads. A received
/// 0x1b naming a seated player rejects it as lobby_reject does, passing it
/// on once. A player's setup block (0x20) sends it this machine's presence
/// record when it is due (presence_send_due). A presence record (0xf0) from
/// a seated remote player whose length word is the event's size is kept as
/// that player's (lobby_presence_record); any other is ignored. A departed
/// player's record is forgotten.
///
/// @param[in,out] lobby Lobby state.
/// @param event Event from LobbyNet::receive.
/// @return True when it changed the lobby.
bool lobby_apply_event(Lobby& lobby, const LobbyEvent& event) noexcept;

/// Finds the in-use slot answering to a player id.
///
/// @param lobby Lobby state.
/// @param player_id Session player id.
/// @return The slot, or -1.
[[nodiscard]] int32_t slot_for_player_id(Lobby& lobby, uint32_t player_id) noexcept;

// ---------------------------------------------------------------------------
// Presence records (Lobby::presence_records)

/// Returns the presence record of the player in a slot.
///
/// @param lobby Lobby state.
/// @param slot Player slot, 0..9.
/// @return For a slot this machine plays, its own or a computer player's, the record this machine
///         sends; for a remote slot in use, the record the player seated there now sent; else empty.
[[nodiscard]] std::span<const uint8_t> lobby_presence_record(Lobby& lobby, int32_t slot) noexcept;

/// Composes the record this machine sends (PresenceRecords::local).
///
/// The record is the bound source and, when this machine hosts and
/// LobbyMaps::pack names a pack for the selected map, a map-pack field
/// (tag 0x40). The pack is asked only when the selected map's name differs
/// from the one it was last asked for, or once a binding has come since,
/// and its answer is kept between. A descriptor URL over 255 bytes, a name
/// over 64 bytes or a version over 32 bytes is sent empty; a pack that still
/// does not fit beside the source is left out. When the record differs from
/// the one held, it is taken and the generation counts on. An empty source
/// leaves the record empty.
///
/// @param[in,out] lobby Lobby state.
void presence_refresh(Lobby& lobby) noexcept;

/// Sends this machine's record to each OA player who has not had it as it is now.
///
/// A player gets it when its remote slot is in use, its own setup block has
/// arrived with state 1 (a human: watchers too, never a computer player) and
/// carries the 'OA' signature (netgame::presence_peer), and the generation
/// and player last sent to the slot are not the record's and this player.
/// Each record goes from the local player to that player alone, in a frame
/// of its own: what is queued goes out first, then the record. Nothing is
/// sent while the record is empty or outside a live session.
///
/// @param[in,out] lobby Lobby state.
/// @param only_slot The one slot to look at; -1 looks at every slot.
void presence_send_due(Lobby& lobby, int32_t only_slot = -1) noexcept;

// ---------------------------------------------------------------------------
// The OA badge and the rules dot: what a battle room row shows in its CD<slot>
// rectangle.

/// What a battle room row shows in its CD<slot> rectangle.
enum class RowBadge : uint8_t {
    none,              ///< nothing
    disc,              ///< the CD icon: a player on 3.1c who has the game disc
    open_annihilation, ///< the OA badge: a player on OA
};

/// What a battle room row shows in its CD<slot> rectangle, and whether its badge carries the dot.
struct RowBadgeState {
    RowBadge badge{};    ///< what the rectangle shows
    bool rules_differ{}; ///< the OA badge carries the dot (lobby_rules_differ_from_host)
};

/// Returns what a battle room row shows in its CD<slot> rectangle, and whether its badge carries the dot.
///
/// A row shows something only while it shows a player (an active slot or a
/// watcher) who is present: this machine's own player, or a remote player
/// whose block says it is playing. Such a player has the OA badge when it is
/// this machine's own player, which is always OA's, or its setup block
/// carries the 'OA' signature; else the CD icon when its block says it has
/// the game disc (status::has_disc); else nothing. OA asks for no disc, so
/// an OA player never shows the CD icon, though its block reports one for a
/// 3.1c host's count before START. The dot is lobby_rules_differ_from_host:
/// the sim hashes, or the base game and rule-changing expansions when both
/// records name them. It allocates nothing.
///
/// @param lobby Lobby state.
/// @param slot Player slot, 0..9; any other shows nothing.
/// @return The row's badge and its dot.
[[nodiscard]] RowBadgeState lobby_row_badge(Lobby& lobby, int32_t slot) noexcept;

/// Tells whether a row's sim hash differs from the host's.
///
/// This is the sim-hash half of the rules dot. The Rules line of the OA tip
/// uses it on its own. Never for the host's own row, a row without the OA
/// badge, or when no host slot is found (lobby_host_slot). Otherwise:
/// 1. when the row's record and the host's each carry a sim hash, they
///    differ exactly when the two hashes do;
/// 2. else, when the host is a 3.1c machine (not this machine, and its block
///    without the 'OA' signature), the host plays 3.1c's own rules, and the
///    row's differ when its record carries a sim hash other than the plain
///    baseline's (PresenceRecords::plain_sim_hash);
/// 3. else no record tells, and they do not differ.
///
/// Reads the records' sim hashes only (netgame::presence_sim_hash) and
/// allocates nothing.
///
/// @param lobby Lobby state.
/// @param slot Player slot, 0..9; any other gives false.
/// @return True when the sim hashes differ.
[[nodiscard]] bool lobby_sim_hash_differs_from_host(Lobby& lobby, int32_t slot) noexcept;

/// Tells whether a row's player plays by rules other than the host's, as the dot on its OA badge says.
///
/// The sim-hash half is lobby_sim_hash_differs_from_host. The content half
/// is the bundles field (netgame::presence_content_match): the same base
/// game and the same rule-changing expansions, and neither side's
/// differs-from-known flag. Music is not compared. A record that omits the
/// field does not differ for that reason, so an OA 0.7 machine and a 3.1c
/// player are unchanged. A setup block carries no presence and is never
/// read for rules. Allocates nothing.
///
/// @param lobby Lobby state.
/// @param slot Player slot, 0..9; any other gives false.
/// @return True when the row's OA badge carries the dot.
[[nodiscard]] bool lobby_rules_differ_from_host(Lobby& lobby, int32_t slot) noexcept;

/// Tells whether this host may start, from the OA players' content.
///
/// True when this machine is not the host. Otherwise every remote OA player
/// whose record names bundles must match the host's (presence_content_match).
/// A player without the field, a 3.1c player, and this machine's own players
/// do not refuse the start. Unit sync is a separate gate. Allocates nothing.
///
/// @param lobby Lobby state.
/// @return True when content does not refuse the start.
[[nodiscard]] bool lobby_content_allows_start(Lobby& lobby) noexcept;

/// Finds the first playing seat whose side is past the room's side count.
///
/// Slot order. A seat plays when it is in use and local, a computer, or a
/// remote player still playing, and it is not watching. A remote seat counts
/// only once this machine has that player's own block. Nothing is found when
/// the lobby has no game, when the side count is 0, or when every playing
/// seat's side is one the room has.
///
/// @param lobby Lobby state.
/// @return The slot, or -1 when there is none.
[[nodiscard]] int32_t lobby_first_side_past_table(Lobby& lobby) noexcept;

/// Tells whether this host may start, from the seated sides.
///
/// True when this machine is not the host. Otherwise no playing seat has a
/// side past the room's side count. A side count of 0 refuses nothing.
///
/// @param lobby Lobby state.
/// @return True when sides do not refuse the start.
[[nodiscard]] bool lobby_sides_allow_start(Lobby& lobby) noexcept;

/// Posts the host's line for each playing seat on a side the game does not have.
///
/// The host only. For each such seat whose side differs from the one last
/// posted for that slot, posts "*** {player} chose a side this game does not
/// have." and remembers the side. A seat back inside the table, emptied or
/// watching, forgets it, so the next side past the table posts once. A
/// guest's machine posts nothing. The lines are not sent.
///
/// @param[in,out] lobby Lobby state.
void lobby_note_sides_past_table(Lobby& lobby) noexcept;

// ---------------------------------------------------------------------------
// Machine groups: the players one machine seats share Player.machine_group
// (1..10), which the host hands out; its own machine is group 1. The lobby
// settles them and the match keeps them.

/// Sets Game.shared_machines when an active player shares its group with another; broadcasts then go once
/// per machine.
///
/// @param[in,out] game Game whose players are examined.
void note_shared_machines(Game& game) noexcept;

/// Returns the first group 1..10 no active player holds.
///
/// @param game Game whose players are examined.
/// @return The group, or 0 when all are taken.
[[nodiscard]] uint32_t free_machine_group(const Game& game) noexcept;

/// Lists who one broadcast goes to while machines are shared: in slot order the first remote player of
/// each group.
///
/// @param game Game whose players are examined.
/// @param[out] ids Player ids to send to.
/// @return The number of ids.
/// @quirk Only groups 0..9 are marked as reached, so every group-10 player gets its own copy.
[[nodiscard]] int32_t
machine_broadcast_targets(const Game& game, uint32_t (&ids)[kSlotCount]) noexcept;

/// Asks the host (0x21) for the group of every active slot without one.
///
/// The local player asks for a new group, a computer player for its human's
/// group, a remote player for the group the host gave it. Every request
/// leaves from the primary player. The host puts its own slots in group 1
/// and asks nothing.
///
/// @param[in,out] lobby Lobby state.
void lobby_request_machine_groups(Lobby& lobby) noexcept;

// ---------------------------------------------------------------------------
// Unit-content sync table

/// Clears the table and seeds one record per unit type.
///
/// Every record exists here; on the host it is also marked reported, and
/// units disabled by default start with limit 0, others unlimited.
///
/// @param[in,out] lobby Lobby state.
/// @param host Whether this machine hosts the game.
void unit_sync_create(Lobby& lobby, bool host) noexcept;

/// Starts the handshake again from the lobby's current unit table.
///
/// A guest drops its table and seeds it again. The host refills the records
/// from the current table, keeps a peer whose setup block lacks the Open
/// Annihilation signature with what that peer reported, drops every other
/// peer, and re-evaluates every record. A peer with no setup block is
/// dropped. The next tick greets a dropped peer again.
///
/// @param[in,out] lobby Lobby state.
void unit_sync_rebuild(Lobby& lobby) noexcept;

/// Drops one peer so the host greets it again.
///
/// The host only. A player the host is not waiting on is left as it is.
/// The next tick sends the greeting.
///
/// @param[in,out] lobby Lobby state.
/// @param player_id The peer to greet again.
void unit_sync_regreet(Lobby& lobby, uint32_t player_id) noexcept;

/// Drops the table.
///
/// @param[in,out] lobby Lobby state.
void unit_sync_destroy(Lobby& lobby) noexcept;

/// Sends a 0x1a record to one player, or to the host when not hosting, with bytes 2..5 cleared.
///
/// @param[in,out] lobby Lobby state.
/// @param player_id Destination when hosting.
/// @param record The 14-byte record.
void unit_sync_send(Lobby& lobby, uint32_t player_id, const uint8_t* record) noexcept;

/// Handles a received 0x1a record.
///
/// A client stores the host's verdicts (subtype 3). The host records a
/// peer's unit count (1), checksums (2) and received count (4).
///
/// @param[in,out] lobby Lobby state.
/// @param record The record as received; one that is not a whole 14-byte 0x1a record is dropped, as
///        are subtypes of 100 and above.
/// @param from_slot Sender's slot.
void unit_sync_receive(Lobby& lobby, std::span<const uint8_t> record, uint8_t from_slot) noexcept;

/// Sends one record's verdict (subtype 3: local, remote and limit) to a peer.
///
/// @param[in,out] lobby Lobby state.
/// @param[in,out] peer Peer to send to; its sent count grows.
/// @param record Record whose verdict is sent.
void unit_sync_send_verdict(
    Lobby& lobby, UnitSyncPeer& peer, const UnitSyncRecord& record
) noexcept;

/// Queues a changed key for the restrict panel and, when hosting, sends its verdict to every peer.
///
/// @param[in,out] lobby Lobby state.
/// @param key FBI hash of the unit.
void unit_sync_relay(Lobby& lobby, uint32_t key) noexcept;

/// Pops the oldest changed record.
///
/// @param[in,out] lobby Lobby state.
/// @param[out] out The record, or an empty one for a key no longer in the table.
/// @return False when none is queued.
bool unit_sync_pop_changed(Lobby& lobby, UnitSyncRecord* out) noexcept;

/// Looks up a record.
///
/// @param lobby Lobby state.
/// @param key FBI hash of the unit.
/// @param[out] out The record, or an empty one for an unknown key.
/// @return True when both sides have the unit.
bool unit_sync_lookup(Lobby& lobby, uint32_t key, UnitSyncRecord* out) noexcept;

/// Marks a unit enabled or disabled by the host's restrictions and relays the record.
///
/// @param[in,out] lobby Lobby state.
/// @param key FBI hash of the unit.
/// @param enabled Whether the unit may be built.
/// @return True when it is enabled and every peer has it; false for an unknown key.
bool unit_sync_set_enabled(Lobby& lobby, uint32_t key, bool enabled) noexcept;

/// Works out on the host whether every peer has a unit, and relays the record either way.
///
/// A non-zero checksum that differs from the host's own content checksum of
/// the unit (worked out the first time one is compared) clears it; so does a
/// peer that has not reported the key or, for a non-zero checksum, a peer
/// from version 1.2 on that reported another one. A key the host lacks is
/// ignored.
///
/// @param[in,out] lobby Lobby state.
/// @param key FBI hash of the unit.
/// @param checksum Checksum a peer reported, or 0 to recheck without one.
void unit_sync_update_record(Lobby& lobby, uint32_t key, uint32_t checksum) noexcept;

/// Runs the unit sync for one battle room frame.
///
/// A client, once the host has greeted it, reports its unit count, then
/// four units a frame with their content checksums, then how many records it
/// has handled. The host drops peers no longer playing and greets new ones
/// (subtype 0); when either changed the roster, every record is updated
/// without a checksum (in table order, not key order).
///
/// @param[in,out] lobby Lobby state.
void unit_sync_tick(Lobby& lobby) noexcept;

/// Returns a unit's content checksum, worked out through the host's files the first time and kept in the
/// unit (UnitDef.content_checksum).
///
/// @param lobby Lobby state.
/// @param[in,out] unit Unit whose checksum is wanted.
/// @return The checksum; 0 when it cannot be worked out.
[[nodiscard]] uint32_t unit_sync_content_checksum(Lobby& lobby, LobbyUnit& unit) noexcept;

/// Sets a unit's build limit and relays the record (the host sends it to every peer; the restrict panel
/// sees it change).
///
/// @param[in,out] lobby Lobby state.
/// @param key FBI hash of the unit; unknown keys are ignored.
/// @param limit Build limit, -1 for unlimited.
void unit_sync_set_limit(Lobby& lobby, uint32_t key, int32_t limit) noexcept;

/// Marks the unit catalog from the sync table.
///
/// Leaves each type of records[1..count) available only when the record for
/// its FBI hash has both sides set, and gives it that record's limit; a type
/// with no record gets limit 0. No-op once the sync finished.
///
/// @param sync The settled sync table.
/// @param[in,out] records Unit definitions, index 0 being the empty type.
/// @param count Number of definitions.
void unit_sync_mark_units(const UnitSync& sync, UnitDef* records, uint32_t count) noexcept;

/// Tells whether the host's sync is complete: every peer still playing has reported its unit count, sent a
/// checksum per unit and acknowledged every verdict.
///
/// @param lobby Lobby state.
/// @return The result; true for a client or once finished.
[[nodiscard]] bool unit_sync_complete(Lobby& lobby) noexcept;

/// Tells whether the host's sync with one player is complete.
///
/// @param lobby Lobby state.
/// @param player_id Peer to test.
/// @return False for a client; true for a peer not waited on (defeated or computer) or once finished.
[[nodiscard]] bool unit_sync_peer_complete(Lobby& lobby, uint32_t player_id) noexcept;

/// Answers "+syncerr": the first peer's shortfall.
///
/// Unlike the checks above it does not skip defeated or computer peers.
///
/// @param lobby Lobby state.
/// @param[out] out Buffer for a formatted shortfall.
/// @param capacity Bytes available at out.
/// @return The shortfall (out or a static message), "OK", or null for a client.
[[nodiscard]] const char*
unit_sync_diagnostic(Lobby& lobby, char* out, std::size_t capacity) noexcept;

// ---------------------------------------------------------------------------
// Setup and team rules of a mod profile (Lobby::rules)

/// Returns the battle room's slots as the team rules read them.
///
/// @param lobby Lobby state.
/// @return Each slot's seat, status, watching, setup state, team, alliances and name.
[[nodiscard]] team_rules::TeamSlots lobby_team_slots(Lobby& lobby) noexcept;

/// Sets a player's alliance with another and announces it, as the team rules do.
///
/// A player this machine runs takes the alliance and sends it to everyone
/// (both-sides word 0); a player on this machine at the other end takes it
/// as it would from the record. A player of another machine is asked by
/// its machine to set it: the record goes to that player with the
/// both-sides word team_rules::alliance_request.
///
/// @param[in,out] lobby Lobby state.
/// @param[in,out] from The player whose alliance changes.
/// @param[in,out] to The other player.
/// @param value 1 allied, 0 not.
void lobby_announce_alliance(Lobby& lobby, Player& from, Player& to, uint8_t value) noexcept;

/// Carries out team steps in order: alliance steps through
/// lobby_announce_alliance, team steps by storing the team and sending it
/// from the local player with team_rules::team_keeps_alliances set.
///
/// @param[in,out] lobby Lobby state.
/// @param steps The steps.
void lobby_apply_team_steps(Lobby& lobby, const team_rules::TeamSteps& steps) noexcept;

/// Runs a battle-room chat line that is one of the profile's setup commands.
///
/// With teams.team-number-alliances, "+autoteam [N]" and "+randomteam [N]"
/// deal the counted players into N teams (2 to 5, 2 by default) in a
/// random order, on the host's machine only; elsewhere they answer that
/// only the host can use them. With setup.map-scripted-units, "+spawnoff"
/// and "+spawnon" turn map-placed units off and on. Commands match without
/// case; each posts its notice as a local chat line.
///
/// @param[in,out] lobby Lobby state.
/// @param text The typed line.
/// @return true when the line was such a command.
bool lobby_run_setup_command(Lobby& lobby, std::string_view text) noexcept;

} // namespace oa::ui::frontend_multiplayer
