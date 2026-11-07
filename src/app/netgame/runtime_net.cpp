// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Networked match integration: the session behind the multiplayer
// screens, the launch from the battle room through the netgame path (the
// start record, the loading screen's work at its pace and the end of
// loading), the per-tick match binding, and the in-match routing of chat,
// pause, speed and leave. --net-loopback-check hosts and joins in one
// process over 127.0.0.1, runs the in-game team panels over the session
// and compares both worlds after a settled run.
#include "oa/app/asset_files.hpp"
#include "oa/app/mutators_memory.hpp"
#include "oa/app/pack_map_source.hpp"
#include "oa/app/presence_facts.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/version_line.hpp"
#include "oa/app/view_rules.hpp"
#include "network_play.hpp"
#include "oa/app/check_host.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/user_folder.hpp"
#include "demo_state.hpp"
#include "launch_binding.hpp"
#include "oa/session/demo.hpp"
#include "oa/session/demo/recording_stream.hpp"
#include "oa/session/demo/replay_record.hpp"
#include "net_options.hpp"
#include "net_state.hpp"
#include "recording_partials.hpp"
#include "traffic_overlay.hpp"
#include "wire_rules_binding.hpp"

#include "oa/netgame/match/launch.hpp"
#include "oa/netgame/match/match_binding.hpp"
#include "oa/netgame/match/net_match.hpp"
#include "oa/netgame/match/session_lobby.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"
#include "oa/netgame/unicode_chat.hpp"
#include "oa/netgame/unit_state.hpp"
#include "oa/app/netgame/extension_api.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/campaign/campaign_file.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/defs/unit_header.hpp"
#include "oa/data/limits.hpp"
#include "oa/data/map_pack/map_name.hpp"
#include "oa/data/saves/content_record.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/base/text.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/netgame/sync/unit_checksum.hpp"
#include "oa/present/game_text.hpp"
#include "oa/sim/ai.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/messages.hpp"
#include "oa/ui/console/console.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/hud/chat_panel.hpp"
#include "oa/ui/hud/order_panel.hpp"
#include "oa/ui/hud/player_records.hpp"
#include "oa/ui/hud/share_panel.hpp"
#include "oa/ui/hud/shared_views.hpp"
#include "oa/ui/hud/team_panels.hpp"
#include "oa/ui/hud/whiteboard.hpp"
#include "oa/ui/frontend_multiplayer/connect.hpp"
#include "oa/ui/frontend_multiplayer/room_limits.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace oa::app {

namespace mp = oa::ui::frontend_multiplayer;
namespace nm = oa::netgame::match;

namespace {

constexpr uint16_t kLoopbackUnitsPerPlayer = 20;
// With --net-loopback-computer: room for the computer player's base and
// army, on a small map of the base game whose bases lie close together; an
// install without it plays its first map.
constexpr uint16_t kLoopbackComputerUnitsPerPlayer = 100;
constexpr const char* kLoopbackComputerMap = "Ashap Plateau";
// Units the computer player has, its commander among them, once it has
// built for the check's ticks.
constexpr uint32_t kComputerBuiltUnits = 5;
// Most ticks the computer player's strike and the fight may take.
constexpr uint32_t kComputerFightTicks = 9000;
// The computer player's losses the fight must reach.
constexpr int16_t kComputerFightLosses = 2;
// The squad the host's player gives the computer player, enough for its
// land strike to set out: Arm kbots made beside the host's commander, this
// far to its side and this far apart, in world units. The joiner's player
// gives one more across the session.
constexpr int32_t kComputerSquadGifts = 6;
constexpr const char* kComputerSquadType = "ARMHAM";
constexpr const char* kComputerAcrossType = "ARMPW";
constexpr int32_t kComputerGiftAside = 96;
constexpr int32_t kComputerGiftSpacing = 40;
// Ticks the owner's poses are kept for comparing the joiner's copies, and
// how far a copy may stand from its owner: a pixel, in 16.16.
constexpr std::size_t kComputerPoseTicks = 128;
constexpr uint32_t kCopyOffsetLimit = 1u << 16;
// The loopback checks wait on the other machine in rounds, each of which
// serves both machines once: a pump, a frame or a tick. A wait gives up
// only once it has gone kLoopbackWaitRounds rounds and kLoopbackWaitMs of
// the wall clock without progress. A machine under load runs the same
// rounds more slowly and waits for them, and on a quick machine a step the
// wall clock paces, such as the load barrier, still has its time. A wait
// that shows progress, such as the unit sync's records, counts again from
// each step. kLoopbackBackstopMs of the wall clock (ten minutes) ends any
// wait.
constexpr uint32_t kLoopbackWaitRounds = 200;
constexpr uint32_t kLoopbackWaitMs = 15000;
constexpr uint32_t kLoopbackBackstopMs = 10 * 60 * 1000;
// Milliseconds a networked tick of the loopback checks waits for the other
// machine's records. None: the checks settle until both machines agree
// instead of waiting on the clock. The computer match, whose fight has to
// play the same way every run, reads every record sent before the next
// step instead (loopback_deliver), pausing this long between looks.
constexpr uint32_t kTickPumpWaitMs = 0;
constexpr std::chrono::microseconds kLoopbackDeliverPause{50};
constexpr int32_t kLoopbackMoveDistance = 64 << 16;
// The host commander fires at the ground this far ahead of it, within its
// laser's range of 200, from tick 200 until tick 260.
constexpr int32_t kLoopbackShotDistance = 100 << 16;
// Beside the host commander, in world units. The check's shooter carries a
// weapon whose range is about 180. After the commander walks 64 and aims
// 100 ahead, 32 aside keeps that aim point in range.
constexpr int32_t kLoopbackShooterAside = 32;
constexpr std::size_t kLoopbackFireFrom = 200;
constexpr std::size_t kLoopbackFireUntil = 260;
// The tick the host's player turns its commander's build page with the
// next-page key.
constexpr std::size_t kLoopbackPageTurn = 40;
// Ticks for the client's commander to self-destruct and its defeat countdown
// to run out: the five-second countdown, then six 30-tick outcome checks.
constexpr uint32_t kLoopbackDefeatTicks = 900;
// Ticks for the host's victory to follow once its enemy watches: at most the
// five-call outcome countdown, one call every 30 ticks, and the check that
// starts it.
constexpr uint32_t kLoopbackVictoryTicks = 180;
// The battlefield and font height the check's recording painter reports to
// the traffic readout: the battlefield's height at 640x480, and the side
// panel font's.
constexpr int kLoopbackOverlayBottom = 480 - 32 - 32;
constexpr uint8_t kLoopbackOverlayFontHeight = 11;

// A loopback check's wait on the other machine, counted in rounds and in
// the wall clock since its last progress (kLoopbackWaitRounds).
class LoopbackWait {
  public:

    /// Counts one round of the wait.
    ///
    /// @param progress a figure that grows only while the wait gets somewhere, such as the
    ///        unit sync's records; 0 for a wait that shows none
    /// @return whether the wait may go on
    bool next_round(uint64_t progress = 0) {
        const auto now = Clock::now();
        if (progress > most_progress_) {
            most_progress_ = progress;
            rounds_since_progress_ = 0;
            progressed_at_ = now;
        }
        ++rounds_since_progress_;
        const auto since = [now](Clock::time_point from) {
            return std::chrono::duration_cast<std::chrono::milliseconds>(now - from).count();
        };
        return since(started_at_) < kLoopbackBackstopMs &&
               (rounds_since_progress_ <= kLoopbackWaitRounds ||
                since(progressed_at_) < kLoopbackWaitMs);
    }

  private:

    using Clock = std::chrono::steady_clock;
    Clock::time_point started_at_{Clock::now()};   ///< when the wait began
    Clock::time_point progressed_at_{started_at_}; ///< when its progress last grew
    uint64_t most_progress_{};                     ///< the highest progress figure seen
    uint32_t rounds_since_progress_{};             ///< rounds since its progress last grew
};

// A unit's movement as its owner's record for one tick leaves it.
struct UnitPose {
    int32_t x{};
    int32_t y{};
    int32_t z{};
    int32_t heading{};
    int32_t speed{};
};

// The lobby block of the player with this session id, or null.
const PlayerSetupInfo* info_for_id(const World& world, uint32_t net_id) {
    for (const auto& player : world.game.players)
        if (player.in_use != 0 && player.player_id == net_id)
            return world_player_info(&world, &player);
    return nullptr;
}

bool watches(const PlayerSetupInfo* info) {
    return info != nullptr && (info->options & OA_SETUP_OPTION_WATCHER) != 0;
}

// Largest difference of any pose component, in its raw units.
uint32_t pose_divergence(const UnitPose& a, const UnitPose& b) {
    uint32_t largest = 0;
    for (const auto& [p, q] :
         {std::pair{a.x, b.x},
          std::pair{a.y, b.y},
          std::pair{a.z, b.z},
          std::pair{a.heading, b.heading},
          std::pair{a.speed, b.speed}})
        largest = std::max(largest, static_cast<uint32_t>(std::abs(static_cast<int64_t>(p) - q)));
    return largest;
}

std::string_view field_text(const char* field, std::size_t capacity) {
    return {field, ::strnlen(field, capacity)};
}

// The line of a match's message log at a place of its lines listed oldest
// first, as match_message_lines lists them; nothing past the newest.
std::optional<oa::sim::messages::MessageLine> logged_line(const World& world, std::size_t place) {
    const auto& game = world.game;
    const auto count = static_cast<std::size_t>(
        (game.chat_head + OA_CHAT_LINE_COUNT - game.chat_tail) % OA_CHAT_LINE_COUNT
    );
    if (place >= count)
        return std::nullopt;
    oa::sim::messages::MessageLine line{};
    std::memcpy(&line, game.chat_lines[(game.chat_tail + place) % OA_CHAT_LINE_COUNT], sizeof line);
    return line;
}

bool slot_live(const Player& player) {
    return player.in_use != 0 &&
           (player.status == OA_PLAYER_STATUS_LOCAL || player.status == OA_PLAYER_STATUS_COMPUTER ||
            player.status == OA_PLAYER_STATUS_MIRRORED);
}

std::string player_name(const World& world, uint32_t net_id) {
    for (const auto& player : world.game.players)
        if (player.in_use != 0 && player.player_id == net_id)
            return std::string(field_text(player.name, sizeof player.name));
    return "player " + std::to_string(net_id);
}

// FNV-1a over the tick and every live unit; owners are named by player id
// because slot orders differ between machines.
uint64_t world_digest(const World& world) {
    constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
    constexpr uint64_t kFnvPrime = 0x100000001b3ull;
    uint64_t hash = kFnvOffsetBasis;
    const auto mix = [&hash](uint64_t value) {
        hash ^= value;
        hash *= kFnvPrime;
    };
    mix(world.game.tick);
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
        const auto& unit = world.units[slot];
        if (unit.type_index == 0)
            continue;
        mix(slot);
        mix(unit.type_index);
        mix(unit.owner_index < OA_PLAYER_COUNT ? world.game.players[unit.owner_index].player_id
                                               : 0u);
        mix(static_cast<uint32_t>(unit.position.x));
        mix(static_cast<uint32_t>(unit.position.y));
        mix(static_cast<uint32_t>(unit.position.z));
        mix(static_cast<uint16_t>(unit.health));
    }
    return hash;
}

/// Returns the slot of the in-use player with a session id.
///
/// @param world match world
/// @param net_id session id
/// @return the slot, or OA_PLAYER_COUNT
uint8_t player_slot(const World& world, uint32_t net_id) {
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot)
        if (world.game.players[slot].in_use != 0 && world.game.players[slot].player_id == net_id)
            return slot;
    return OA_PLAYER_COUNT;
}

/// Tells whether a unit slot holds a live unit.
///
/// @param unit unit record
/// @return true for a unit of some type with its live flag
bool unit_live(const Unit& unit) {
    return unit.type_index != 0 && (unit.flags & OA_UNIT_FLAG_LIVE) != 0;
}

/// Counts the live units a player owns.
///
/// @param world match world
/// @param slot player slot
/// @return the count
uint32_t live_units(const World& world, uint8_t slot) {
    uint32_t count = 0;
    for (uint32_t index = 1; index < world.unit_slot_count; ++index)
        count += unit_live(world.units[index]) && world.units[index].owner_index == slot ? 1 : 0;
    return count;
}

/// Counts the live units of one type a player owns.
///
/// @param world match world
/// @param slot player slot
/// @param type UnitDef index
/// @return the count
uint32_t units_of_type(const World& world, uint8_t slot, uint16_t type) {
    uint32_t count = 0;
    for (uint32_t index = 1; index < world.unit_slot_count; ++index) {
        const auto& unit = world.units[index];
        count += unit_live(unit) && unit.owner_index == slot && unit.type_index == type ? 1 : 0;
    }
    return count;
}

/// Finds a player's live commander: a unit of the type its side names in
/// SIDEDATA, as the game's commander rule counts it.
///
/// @param world match world
/// @param slot player slot
/// @return its unit slot, or 0
uint16_t commander_of(const World& world, uint8_t slot) {
    for (uint32_t index = 1; index < world.unit_slot_count; ++index) {
        const auto& unit = world.units[index];
        if (unit_live(unit) && unit.owner_index == slot &&
            sim::match_runtime::side_commander(world, unit))
            return static_cast<uint16_t>(index);
    }
    return 0;
}

/// Finds a unit type by its unit name, ignoring case.
///
/// @param world match world
/// @param name unit name, such as ARMPW
/// @return its UnitDef index, or 0
uint16_t unit_type_named(const World& world, std::string_view name) {
    for (uint32_t type = 1; type < world.unit_def_count; ++type) {
        const auto held =
            field_text(world.unit_defs[type].unit_name, sizeof world.unit_defs[type].unit_name);
        if (held.size() == name.size() &&
            std::equal(held.begin(), held.end(), name.begin(), [](char a, char b) {
                return std::tolower(static_cast<unsigned char>(a)) ==
                       std::tolower(static_cast<unsigned char>(b));
            }))
            return static_cast<uint16_t>(type);
    }
    return 0;
}

/// Makes a finished unit for a player, standing beside another unit.
///
/// @param match the player's match
/// @param world that match's world
/// @param player the owner's slot
/// @param type the unit type
/// @param beside the unit it stands beside
/// @param aside how many world units to the side, flipped when that would leave the map
/// @return the new unit's slot, or 0 when it could not be made
uint16_t make_unit(
    sim::match_runtime::Match& match,
    World& world,
    uint8_t player,
    uint16_t type,
    uint16_t beside,
    int32_t aside
) {
    const auto& at = world.units[beside];
    const int32_t width = static_cast<int32_t>(world.game.map_pixel_width) << 16;
    const int32_t offset = at.position.x + (aside << 16) < width ? aside << 16 : -(aside << 16);
    oa::sim::unit_spawn::Request request{};
    request.player = player;
    request.type = type;
    request.position = {
        static_cast<uint32_t>(at.position.x + offset),
        static_cast<uint32_t>(at.position.y),
        static_cast<uint32_t>(at.position.z)
    };
    request.finished = true;
    request.state = 1;
    auto* made = match.create(request);
    return made != nullptr && made->unit != nullptr ? static_cast<uint16_t>(made->unit_index) : 0;
}

/// Hashes which units a world holds: slot, type and owner by player id (FNV-1a).
///
/// @param world match world
/// @return the hash
uint64_t unit_roster(const World& world) {
    constexpr uint64_t kFnvOffsetBasis = 0xcbf29ce484222325ull;
    constexpr uint64_t kFnvPrime = 0x100000001b3ull;
    uint64_t hash = kFnvOffsetBasis;
    const auto mix = [&hash](uint64_t value) {
        hash ^= value;
        hash *= kFnvPrime;
    };
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
        const auto& unit = world.units[slot];
        if (!unit_live(unit))
            continue;
        mix(slot);
        mix(unit.type_index);
        mix(unit.owner_index < OA_PLAYER_COUNT ? world.game.players[unit.owner_index].player_id
                                               : 0u);
    }
    return hash;
}

/// The shots one player's units launch in a match, counted as each is placed.
///
/// A shot is counted when it enters the projectile pool, not looked for in
/// the pool after the tick: a shot that strikes in the tick it is fired is
/// gone by then, and whether it does can differ between the machine that
/// fires it and one that launches it from the 0x0d a tick later.
struct LaunchedShots {
    uint8_t owner{};  ///< player slot whose units' shots count
    uint32_t count{}; ///< shots launched so far
};

/// Counts a shot a unit's weapon launched, on this machine or from its
/// owner's 0x0d, when the unit belongs to the counted player
/// (EventHooks::shot_placed).
///
/// A burst's later shots, which every machine makes from the first one, and
/// meteors are not counted.
///
/// @param context the LaunchedShots
/// @param world match world
/// @param shot the shot placed
/// @param source how the shot came to be
void count_launched_shot(
    void* context,
    const World& world,
    const oa::Projectile& shot,
    sim::match_runtime::ShotSource source,
    const FixedVec3* /*aim*/,
    uint16_t /*target_unit*/
) {
    auto& shots = *static_cast<LaunchedShots*>(context);
    const auto unit = oa::oa_unit_slot_from_ref(shot.source);
    if (source == sim::match_runtime::ShotSource::weapon && unit != 0 &&
        unit < world.unit_slot_count && world.units[unit].owner_index == shots.owner)
        ++shots.count;
}

/// Counts a player's launched shots through a match's event hooks while it
/// lives, then gives the match back the hooks it had.
class LaunchedShotCounter {
  public:

    /// Points the match's event hooks at the count.
    ///
    /// @param[in,out] match match whose shots are counted
    /// @param[in,out] shots count to add to; outlives the counter
    LaunchedShotCounter(sim::match_runtime::Match& match, LaunchedShots& shots)
        : counted_(match), saved_hooks_(match.event_hooks) {
        counted_.event_hooks = {};
        counted_.event_hooks.context = &shots;
        counted_.event_hooks.shot_placed = &count_launched_shot;
    }

    /// Gives the match back the hooks it had.
    ~LaunchedShotCounter() { counted_.event_hooks = saved_hooks_; }

    LaunchedShotCounter(const LaunchedShotCounter&) = delete;
    LaunchedShotCounter& operator=(const LaunchedShotCounter&) = delete;

  private:

    sim::match_runtime::Match& counted_;         ///< match whose shots are counted
    sim::match_runtime::EventHooks saved_hooks_; ///< the hooks the match had
};

/// The joiner's copies of one player's units that a pad has carried since
/// their last full record.
///
/// A unit on its factory's pad turns with the pad. The joiner spins its
/// copy of the pad from the factory's records, at the ticks its record
/// pacing applies them; the copy of the unit takes its pad's turn at each
/// of its owner's 0x2c, and leaves the pad when the 0x0a arrives, which
/// comes from the first player on the owner's machine rather than from the
/// owner. So the copy leaves its pad some turn steps off its owner, more
/// when records arrive late, and on a route that turns it about it may turn
/// the other way. Only its next full record puts it back on its owner's
/// path, as in 3.1c.
struct CarriedCopies {
    const World* world{};         ///< the joiner's world
    std::vector<uint8_t> carried; ///< by unit slot: nonzero while waiting for a full record
    uint32_t placed{};            ///< copies a full record placed after a pad carried them
};

/// Takes a copy's carried mark away when a full record places it
/// (FullRecordProbe::placed).
///
/// @param context the CarriedCopies
/// @param unit the unit placed
void note_placed_copy(
    void* context, const Unit& unit, const FixedVec3& /*before*/, const FixedVec3& /*to*/
) {
    auto& copies = *static_cast<CarriedCopies*>(context);
    const auto slot = world_unit_slot(copies.world, &unit);
    if (slot < copies.carried.size() && copies.carried[slot] != 0) {
        copies.carried[slot] = 0;
        ++copies.placed;
    }
}

/// Watches the full records a binding applies while it lives, then gives
/// the binding back the probe it had.
class PlacedCopyWatch {
  public:

    /// Points the binding's full record probe at the copies.
    ///
    /// @param[in,out] binding binding whose full records are watched
    /// @param[in,out] copies carried copies to update; outlives the watch
    PlacedCopyWatch(nm::MatchBinding& binding, CarriedCopies& copies)
        : watched_(binding), saved_probe_(binding.full_record_probe) {
        watched_.full_record_probe = {&copies, &note_placed_copy};
    }

    /// Gives the binding back the probe it had.
    ~PlacedCopyWatch() { watched_.full_record_probe = saved_probe_; }

    PlacedCopyWatch(const PlacedCopyWatch&) = delete;
    PlacedCopyWatch& operator=(const PlacedCopyWatch&) = delete;

  private:

    nm::MatchBinding& watched_;       ///< binding whose full records are watched
    nm::FullRecordProbe saved_probe_; ///< the probe the binding had
};

/// Marks the unit types the battle room's verdicts keep and gives them their agreed build limits.
///
/// @param context the battle room's mp::UnitSync
/// @param[in,out] headers unit headers, index 0 empty
/// @param count number of headers
void mark_agreed_units(void* context, UnitDef* headers, uint32_t count) {
    mp::unit_sync_mark_units(*static_cast<const mp::UnitSync*>(context), headers, count);
}

#ifndef OA_ENGINE_VERSION
#error "OA_ENGINE_VERSION names the engine's version for the recorder's report line"
#endif
#ifndef OA_ENGINE_VERSION_LABEL
#error "OA_ENGINE_VERSION_LABEL names the engine's version label, empty for a release build"
#endif

/// The line this machine's recorder answers .report with: the program and its version.
constexpr const char* kProgramLine = "Open Annihilation " OA_ENGINE_VERSION;
/// The engine's version as the engine shows it, which the battle room's
/// line names.
constexpr const char* kEngineVersionText = "v" OA_ENGINE_VERSION;

/// Returns the engine's version as a presence record names it: the version,
/// and a development build's label after a '-' ("0.8.0-dev").
///
/// @return the version text
std::string presence_engine_text() {
    std::string version = OA_ENGINE_VERSION;
    constexpr std::string_view label = OA_ENGINE_VERSION_LABEL;
    if (!label.empty())
        version.append("-").append(label);
    return version;
}

/// The facts a presence record left out, each reported once (one bit each).
namespace presence_problem {
inline constexpr uint32_t engine = 0x001;       ///< the version text breaks its rule
inline constexpr uint32_t platform = 0x002;     ///< the platform's name breaks its rule
inline constexpr uint32_t architecture = 0x004; ///< the architecture's name breaks its rule
inline constexpr uint32_t mod = 0x008;          ///< the mod's id, name or revision breaks its rule
inline constexpr uint32_t mod_version = 0x010;  ///< the mod's version breaks its rule
inline constexpr uint32_t mod_origin = 0x020;   ///< the mod's origin breaks its rule
inline constexpr uint32_t game_hack = 0x040;    ///< a game hack's id breaks its rule
inline constexpr uint32_t view_hack = 0x080;    ///< a view hack's id breaks its rule
inline constexpr uint32_t override_id = 0x100;  ///< an override's hack id breaks its rule
inline constexpr uint32_t no_reserve = 0x200;   ///< the record leaves no room for a map pack
inline constexpr uint32_t bundles = 0x400;      ///< the base game or an expansion breaks its rule
inline constexpr uint32_t mutators = 0x800;     ///< the mutator list breaks its rule
inline constexpr uint32_t mutator_modes = 0x1000; ///< the clash-mode mask breaks its rule
} // namespace presence_problem

/// The content and language a presence record adds to the facts.
struct PresencePlayed {
    bool announce_bundles = false;       ///< the managed copy is mounted and the choice is usable
    std::string base_game;               ///< the base game's id
    std::vector<std::string> expansions; ///< rule-changing expansions, music left out
    bool differs_from_known = false;     ///< a chosen bundle's file is not a known hash
    std::string language;                ///< the shown language's English name
};

/// Reads the content and language the presence record carries.
///
/// The runtime's public announcement already leaves music out, and names
/// bundles only for a managed copy.
///
/// @param runtime the runtime
/// @return what to add to the record
PresencePlayed played_content(const Runtime& runtime) {
    const Runtime::PresenceAnnouncement announced = runtime.presence_announcement();
    PresencePlayed played;
    played.announce_bundles = announced.announce_bundles;
    played.base_game = announced.base_game;
    played.expansions = announced.expansions;
    played.differs_from_known = announced.differs_from_known;
    played.language = announced.language;
    return played;
}

/// The bytes follow_presence compares so a content or language change refreshes the record.
///
/// @param played the content and language
/// @return the key
std::string played_content_key(const PresencePlayed& played) {
    std::string key;
    key.push_back(played.announce_bundles ? '1' : '0');
    key.push_back('\n');
    key += played.base_game;
    key.push_back('\n');
    for (const auto& id : played.expansions) {
        key += id;
        key.push_back('\n');
    }
    key.push_back(played.differs_from_known ? '1' : '0');
    key.push_back('\n');
    key += played.language;
    return key;
}

/// Tells whether every field of a record keeps the record's rules.
///
/// @param record the record
/// @return true when it encodes, or is refused only for its size
bool presence_keeps_rules(const oa::netgame::PresenceRecord& record) {
    uint8_t bytes[oa::netgame::presence_record_max_bytes];
    std::size_t written = 0;
    return oa::netgame::encode_presence(record, bytes, sizeof bytes, &written) !=
           oa::netgame::WireError::bad_argument;
}

/// Writes this machine's presence record from its facts, without a map pack.
///
/// A fact that breaks the record's rules is left out: a platform or
/// architecture name, or a mod's version, is sent empty; an engine, a mod,
/// an origin, a hack, an override or the mutator list is left out. Each
/// kind is reported on the error stream the first time. The mutator list
/// keeps the room left for the host's map pack: the record is still written
/// within presence_record_max_bytes less presence_map_pack_field_max_bytes.
///
/// @param facts what this machine is playing
/// @param[in,out] reported the presence_problem bits already reported
/// @param played the base game, expansions and language
/// @param mutators the mutators field, or null when this machine plays none
/// @param mutator_modes the host's clash-mode mask, sent with the mutator
///        field; nothing when this machine sends none
/// @return the record; empty only when nothing could be written
std::vector<uint8_t> write_presence_record(
    const PresenceFacts& facts,
    uint32_t& reported,
    const PresencePlayed& played,
    const oa::netgame::PresenceMutators* mutators,
    std::optional<uint32_t> mutator_modes
) {
    namespace ng = oa::netgame;
    const auto report = [&](uint32_t problem, const char* what) {
        if ((reported & problem) != 0)
            return;
        reported |= problem;
        std::cerr << "multiplayer: the presence record leaves out " << what << '\n';
    };
    ng::PresenceRecord record;
    ng::PresenceRecord probe;

    ng::PresenceEngine engine{presence_engine_text(), facts.platform, facts.architecture};
    probe.engine = ng::PresenceEngine{engine.version, engine.platform, ""};
    if (!presence_keeps_rules(probe)) {
        report(presence_problem::platform, "the platform's name");
        engine.platform.clear();
    }
    probe.engine = ng::PresenceEngine{engine.version, "", engine.architecture};
    if (!presence_keeps_rules(probe)) {
        report(presence_problem::architecture, "the architecture's name");
        engine.architecture.clear();
    }
    probe = {};
    probe.engine = engine;
    if (presence_keeps_rules(probe))
        record.engine = std::move(engine);
    else
        report(presence_problem::engine, "the engine's version");
    // Whether this machine can fetch content, so that the host's card and
    // a joiner's GET MAP can tell.
    record.content = ng::PresenceContent{facts.fetches_content};

    if (played.announce_bundles) {
        probe = {};
        probe.bundles =
            ng::PresenceBundles{played.base_game, played.expansions, played.differs_from_known};
        if (presence_keeps_rules(probe))
            record.bundles = std::move(probe.bundles);
        else
            report(presence_problem::bundles, "the base game or an expansion");
    }
    if (!played.language.empty()) {
        probe = {};
        probe.language = played.language;
        if (presence_keeps_rules(probe))
            record.language = std::move(probe.language);
    }

    if (facts.mod) {
        const auto& mod = *facts.mod;
        probe = {};
        if (mod.packaging_revision >= 0 &&
            mod.packaging_revision <= std::numeric_limits<uint16_t>::max()) {
            probe.mod = ng::PresenceMod{
                mod.id, mod.name, mod.version, static_cast<uint16_t>(mod.packaging_revision)
            };
            if (!presence_keeps_rules(probe)) {
                probe.mod->version.clear();
                if (presence_keeps_rules(probe))
                    report(presence_problem::mod_version, "the mod's version");
            }
        }
        if (probe.mod && presence_keeps_rules(probe))
            record.mod = std::move(probe.mod);
        else
            report(presence_problem::mod, "the mod: its id, name or packaging revision");
    }
    // The origin is the mod's, so it goes only with the mod. A descriptor
    // URL over 255 bytes is sent empty, as for a registry not in effect.
    constexpr std::size_t url_most_bytes = 255;
    if (record.mod && facts.mod_origin) {
        const auto& origin = *facts.mod_origin;
        probe = {};
        if (origin.catalogue_release >= 1 &&
            origin.catalogue_release <= std::numeric_limits<uint32_t>::max()) {
            probe.mod_origin = ng::PresenceOrigin{
                origin.registry,
                origin.descriptor_url.size() > url_most_bytes ? std::string{}
                                                              : origin.descriptor_url,
                static_cast<uint32_t>(origin.catalogue_release),
                origin.sha256
            };
            if (!presence_keeps_rules(probe))
                probe.mod_origin->descriptor_url.clear();
        }
        if (probe.mod_origin && presence_keeps_rules(probe))
            record.mod_origin = std::move(probe.mod_origin);
        else
            report(presence_problem::mod_origin, "where the mod was published");
    }
    record.sim_hash = facts.sim_hash;

    if (mutators != nullptr) {
        probe = {};
        probe.mutators = *mutators;
        if (presence_keeps_rules(probe))
            record.mutators = std::move(probe.mutators);
        else
            report(presence_problem::mutators, "the mutator list");
    }
    // The host sends each mutator's mode whenever it sends the list. A mask
    // the record refuses is left out, and a guest then builds every mode
    // as Replace.
    if (record.mutators && mutator_modes) {
        probe = {};
        probe.mutators = *record.mutators;
        probe.mutator_modes = *mutator_modes;
        if (presence_keeps_rules(probe))
            record.mutator_modes = *mutator_modes;
        else
            report(presence_problem::mutator_modes, "the mutator modes");
    }

    const auto hacks =
        [&](const std::vector<std::string>& ids, uint32_t problem, const char* what) {
            ng::PresenceHacks list;
            list.total = static_cast<uint16_t>(
                std::min<std::size_t>(ids.size(), std::numeric_limits<uint16_t>::max())
            );
            for (const auto& id : ids) {
                probe = {};
                probe.game_hacks = ng::PresenceHacks{1, {id}};
                if (presence_keeps_rules(probe))
                    list.ids.push_back(id);
                else
                    report(problem, what);
            }
            return list;
        };
    record.game_hacks = hacks(facts.game_hacks, presence_problem::game_hack, "a game hack's id");
    record.view_hacks = hacks(facts.view_hacks, presence_problem::view_hack, "a view hack's id");

    ng::PresenceDeveloper developer;
    developer.on = facts.developer_mode;
    developer.total = static_cast<uint16_t>(
        std::min<std::size_t>(facts.overrides.size(), std::numeric_limits<uint16_t>::max())
    );
    for (const auto& override : facts.overrides) {
        probe = {};
        probe.developer = ng::PresenceDeveloper{false, 1, {{override.hack, override.on}}};
        if (presence_keeps_rules(probe))
            developer.overrides.push_back(ng::PresenceOverride{override.hack, override.on});
        else
            report(presence_problem::override_id, "an override's hack id");
    }
    record.developer = std::move(developer);

    // The host's map pack always fits beside the record, unless the
    // record's fixed fields alone need more.
    constexpr std::size_t reserved_capacity =
        ng::presence_record_max_bytes - ng::presence_map_pack_field_max_bytes;
    record.limits = ng::PresenceLimits{
        oa::data::limits::highest_weapon_types,
        static_cast<uint8_t>(oa::data::limits::highest_sides),
    };
    uint8_t bytes[ng::presence_record_max_bytes];
    std::size_t written = 0;
    auto error = ng::encode_presence(record, bytes, reserved_capacity, &written);
    if (error == ng::WireError::buffer_too_small) {
        report(
            presence_problem::no_reserve,
            "the room for the host's map pack, which may then not fit beside it"
        );
        error = ng::encode_presence(record, bytes, sizeof bytes, &written);
    }
    if (error != ng::WireError::ok)
        return {};
    return std::vector<uint8_t>(bytes, bytes + written);
}

/// The network rules of a runtime's profile.
///
/// @param runtime the runtime
/// @return its rules; 3.1c's without a profile
oa::netgame::WireRules runtime_wire_rules(const Runtime& runtime) {
    auto rules = oa::app::netgame::wire_rules_of(runtime.mod_profile());
    // A game that joins a replayer to watch a recording presents the
    // replay version.
    if (net_options().replay_viewer) {
        rules.version_major = rules.replay_version_major;
        rules.version_minor = rules.replay_version_minor;
    }
    return rules;
}

/// The wall clock a recording counts its delays on.
///
/// @return milliseconds on the steady clock
uint64_t recording_clock_ms() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()
    )
                                     .count());
}

/// Formats the local date and time for a recording.
///
/// @param pattern strftime pattern
/// @return the text
std::string local_time_text(const char* pattern) {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif
    char formatted[64];
    const auto length = std::strftime(formatted, sizeof formatted, pattern, &parts);
    return std::string(formatted, length);
}

/// Formats the clock's time in UTC as a replay writes an instant.
///
/// @return the time as `YYYY-MM-DDTHH:MM:SSZ`
std::string utc_time_text() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    char formatted[32];
    const auto length = std::strftime(formatted, sizeof formatted, "%Y-%m-%dT%H:%M:%SZ", &parts);
    return std::string(formatted, length);
}

/// Names the program that records, as a replay's description names it.
///
/// @return `open-annihilation ` and the version, with the label when the build has one
std::string recording_program() {
    std::string program = "open-annihilation " OA_ENGINE_VERSION;
#ifdef OA_ENGINE_VERSION_LABEL
    constexpr std::string_view label = OA_ENGINE_VERSION_LABEL;
    if (!label.empty()) {
        program.push_back('-');
        program.append(label);
    }
#endif
    return program;
}

/// Names the machine's platform as a replay's description names it.
///
/// A universal macOS build names the processor it runs on, as in `macos-arm64`.
///
/// @return the platform and the processor
std::string recording_platform() {
    std::string platform(release_platform_name());
    constexpr std::string_view universal = "-universal";
    if (platform.size() >= universal.size() &&
        platform.compare(platform.size() - universal.size(), universal.size(), universal) == 0) {
        platform.resize(platform.size() - universal.size());
        platform.push_back('-');
        platform.append(running_architecture());
    }
    return platform;
}

/// Fills the integrity check's answers about this machine: a digest of the
/// program line, and one of the profile's sim hash with the map, so two
/// machines agree exactly when they run the same program on the same rules and
/// map. While this match holds a mutator digest, the game-data digest starts
/// from played_sim_hash, which folds that digest into the profile hash (or
/// into the baseline a game without a profile plays). Without a mutator
/// digest the bytes stay the profile hash and the map, as they did before
/// mutators. A managed copy appends the canonical bytes of its base game and
/// rule-changing expansions (presence_content_identity). With no bundles
/// field and no mutator digest the digest stays the sim hash and the map.
///
/// @param[out] identity the identity
/// @param runtime the runtime whose profile is read
/// @param map the match's map name
void fill_integrity_identity(
    oa::netgame::match::IntegrityIdentity& identity, const Runtime& runtime, const std::string& map
) {
    const std::string_view program{kProgramLine};
    const auto program_digest = oa::base::sha256::digest_of(
        {reinterpret_cast<const uint8_t*>(program.data()), program.size()}
    );
    std::memcpy(identity.program, program_digest.data(), sizeof identity.program);
    oa::base::sha256::Hasher data{};
    if (runtime.match_mutator_digest()) {
        const oa::base::sha256::Digest played_hash = played_sim_hash(runtime);
        oa::base::sha256::update(data, played_hash);
    } else if (const auto* profile = runtime.mod_profile()) {
        oa::base::sha256::update(data, profile->sim_hash);
    }
    oa::base::sha256::update(data, {reinterpret_cast<const uint8_t*>(map.data()), map.size()});
    const auto played = played_content(runtime);
    if (played.announce_bundles) {
        oa::netgame::PresenceRecord content;
        content.bundles = oa::netgame::PresenceBundles{
            played.base_game, played.expansions, played.differs_from_known
        };
        uint8_t canonical[oa::netgame::presence_content_identity_max_bytes];
        std::size_t canonical_bytes = 0;
        if (oa::netgame::presence_content_identity(
                content, canonical, sizeof canonical, &canonical_bytes
            ) == oa::netgame::WireError::ok &&
            canonical_bytes != 0)
            oa::base::sha256::update(data, {canonical, canonical_bytes});
    }
    const auto data_digest = oa::base::sha256::finish(data);
    std::memcpy(identity.game_data, data_digest.data(), sizeof identity.game_data);
    const auto hex = oa::base::sha256::to_hex(data_digest);
    std::cerr << "multiplayer: integrity game data ";
    std::cerr.write(hex.data(), 16);
    std::cerr << '\n';
    std::cerr.flush();
}

/// One unit header the store serves: its unit name and the bytes' hash.
struct ServedHeader {
    std::string name;
    uint32_t hash = 0;
};

/// Reads the unit headers the store serves now.
///
/// The same files the match start loads, each one's unit name and the hash
/// of its bytes. A file that does not parse is left out.
///
/// @param assets the store, mutator layer included when one is mounted
/// @return those headers
std::vector<ServedHeader> served_unit_headers(const oa::AssetStore& assets) {
    struct Weapons {
        oa::data::defs::WeaponTdfSet set{};

        Weapons() { oa::data::defs::weapon_tdf_set_init(&set); }

        ~Weapons() { oa::data::defs::weapon_tdf_set_free(&set); }
    } weapons;

    const oa::data::defs::Files files = asset_files(assets);
    const auto& layout = oa::data::defs::data_layout();
    const oa::data::defs::UnitHeaderSources sources{
        "", &weapons.set, layout.build_version[0], layout.build_version[1], false, false, nullptr
    };
    std::vector<std::string> names;
    if (files.list != nullptr) {
        files.list(
            files.context,
            oa::data::defs::directory_name(oa::data::defs::DataDirectory::units),
            oa::data::defs::unit_extension(),
            [](void* user, const char* name) {
                static_cast<std::vector<std::string>*>(user)->emplace_back(name);
            },
            &names
        );
    }
    std::vector<ServedHeader> headers;
    headers.reserve(names.size());
    for (const std::string& file_name : names) {
        char path[oa::data::defs::path_capacity];
        oa::data::defs::build_variant_path(
            &files,
            path,
            sizeof path,
            oa::data::defs::directory_name(oa::data::defs::DataDirectory::units),
            file_name.c_str(),
            oa::data::defs::unit_extension(),
            nullptr
        );
        oa::UnitDef unit{};
        bool refused = false;
        if (!oa::data::defs::load_unit_header(&files, path, unit, sources, &refused))
            continue;
        const char* label = unit.unit_name[0] != '\0' ? unit.unit_name : unit.name;
        headers.push_back(
            {std::string(label, ::strnlen(label, sizeof unit.unit_name)), unit.fbi_hash}
        );
    }
    return headers;
}

/// Computes the content checksum of every unit of the battle room's table,
/// as LobbyServices::unit_checksum gives it, listing the GUI pages once.
///
/// @param assets the store the unit files are read from, or null
/// @param units the unit table
/// @param unit_count entries in the table
/// @return one checksum per entry; the stored content checksum where the
///         files cannot be read, and for entry 0, the reserved empty type,
///         which names no unit's files
std::vector<uint32_t> unit_content_checksums(
    const oa::AssetStore* assets, const mp::LobbyUnit* units, int32_t unit_count
) {
    std::vector<uint32_t> checksums;
    for (int32_t index = 0; units != nullptr && index < unit_count; ++index)
        checksums.push_back(units[index].content_checksum);
    if (assets == nullptr)
        return checksums;
    const oa::netgame::sync::StoreUnitFiles files(*assets);
    const auto pages = oa::netgame::sync::list_unit_gui_pages(files);
    for (int32_t index = 1; units != nullptr && index < unit_count; ++index) {
        const auto& unit = units[index];
        // A failed listing leaves every stored checksum, as each unit's own
        // listing would.
        if (unit.unit_name == nullptr || !pages)
            continue;
        try {
            const auto mixed = oa::netgame::sync::mix_unit_file_checksum(
                files, unit.unit_name, unit.content_checksum, unit.weapon_checksum, pages.value
            );
            if (mixed)
                checksums[static_cast<std::size_t>(index)] = mixed.value;
        } catch (const std::exception&) {
        }
    }
    return checksums;
}

struct LoopbackSide {
    NetworkPlay* play{};
    const char* nickname{};
    std::unique_ptr<Game> game = std::make_unique<Game>();
    std::unique_ptr<mp::Lobby> lobby = std::make_unique<mp::Lobby>();
    std::unique_ptr<mp::ConnectState> connect = std::make_unique<mp::ConnectState>();
    mp::LobbyNet net{};
};

} // namespace

const char* NetworkPlay::translate_text(void* play, const char* english) noexcept {
    auto& self = *static_cast<NetworkPlay*>(play);
    try {
        self.translated_text_ = self.runtime_.translate_ui(english);
    } catch (...) {
        return nullptr;
    }
    return self.translated_text_.c_str();
}

// C-style callbacks handed to the multiplayer screens and the net match, and
// the launch steps they drive.
struct NetworkPlay::NetHost {
    static NetworkPlay& self(void* context) { return *static_cast<NetworkPlay*>(context); }

    static void on_start(void* context, mp::Lobby& lobby) { launch(self(context), lobby); }

    // A chat line another player sent is posted as their chat, with their
    // logo and the arrival sound, through the console's host, which the
    // engine fills as the match starts.
    static void chat(void* context, uint8_t sender, const char* text) {
        const auto* host = self(context).net_->console_host;
        if (host == nullptr)
            return;
        if (host->post_message != nullptr)
            host->post_message(host->context, text, oa::sim::messages::kind_player_chat, sender);
        // The sender already ran the line. The host hook runs the commands
        // that have to agree on every machine.
        if (host->apply_remote_line != nullptr)
            host->apply_remote_line(host->context, text, sender);
    }

    static void notice(void* context, const char* text) {
        self(context).runtime_.console_post_message(text);
    }

    static int32_t rand15(void*) { return std::rand() & 0x7fff; }

    // Services the other in-process machine while this one waits on a reply.
    static void pump_peer(void* context, uint32_t wait_ms) {
        auto* peer = static_cast<NetState*>(context);
        if (peer->connection.opened)
            oa::netgame::sock::host_pump(peer->connection.host, wait_ms);
    }

    // The binding's hooks, reached through the runtime's session: its
    // chat, notice and random hooks take network play's state as their context.
    static nm::NetMatchHooks hooks(NetworkPlay& play) {
        nm::NetMatchHooks hooks{};
        hooks.context = &play;
        hooks.chat = chat;
        hooks.seat_chat =
            [](void* context, uint8_t sender, uint8_t addressee, bool broadcast, const char* line) {
                hear_remote_seat_chat(self(context).runtime_, sender, addressee, broadcast, line);
            };
        hooks.notice = notice;
        hooks.translate_game_text = translate_text;
        hooks.rand15 = rand15;
        hooks.credit = [](void*, World* world, uint8_t to, bool metal, float amount) {
            nm::credit_player_resource(world, to, metal, amount);
        };
        hooks.debit = [](void*, World* world, uint8_t from, bool metal, float amount) {
            return nm::debit_player_resource(world, from, metal, amount);
        };
        hooks.destroy_player_units = [](void* context, World*, uint8_t slot) {
            nm::match_binding_destroy_player_units(&self(context).net_->binding, slot);
        };
        hooks.end_local_game = [](void* context) {
            nm::match_binding_end_local_game(&self(context).net_->binding);
        };
        hooks.local_player_won = [](void* context) {
            return nm::match_binding_local_player_won(&self(context).net_->binding);
        };
        hooks.alliance_changed = [](void* context, World*, uint8_t slot) {
            nm::match_binding_follow_alliances(&self(context).net_->binding, slot);
        };
        hooks.share_sight = [](void* context, World*, uint8_t from, uint8_t to) {
            nm::match_binding_share_sight(&self(context).net_->binding, from, to);
        };
        hooks.record_seen =
            [](void* context, uint32_t sender, const uint8_t* record, std::size_t size) {
                auto& recording = self(context).net_->recording;
                if (recording.started)
                    oa::session::demo::recording_add(
                        &recording, sender, recording_clock_ms(), {record, size}
                    );
            };
        // ui.whiteboard: marks an ally's recorder sent join this machine's whiteboard.
        hooks.whiteboard_marks =
            [](void* context, uint8_t, const uint8_t* batch, std::size_t size) {
                self(context).runtime_.receive_whiteboard_batch({batch, size});
            };
        hooks.vote_shown =
            [](void* context, const nm::Vote& vote, uint8_t target, uint8_t, uint8_t) {
                auto& play = self(context);
                play.net_->vote_target = vote.target_id;
                if (target < OA_PLAYER_COUNT)
                    notice(
                        context,
                        std::string(
                            oa::data::languages::interface_text(
                                "Vote Yes or Vote No in the console answers the vote"
                            )
                        )
                            .c_str()
                    );
            };
        hooks.speed_lock_changed =
            [](void* context, bool locked, uint8_t slowest, uint8_t fastest) {
                share_speed_lock(self(context).runtime_, locked, slowest, fastest);
            };
        return hooks;
    }

    // The host's speed lock reaches the runtime, whose speed keys and GAME
    // slider then keep to it as the match does.
    static void share_speed_lock(Runtime& runtime, bool locked, uint8_t slowest, uint8_t fastest) {
        if (locked)
            runtime.lock_game_speed({slowest, fastest});
        else
            runtime.unlock_game_speed();
    }

    // The battle room hands over its lobby on the host's START or on the
    // host's 0x08 at a client. The host queues its 0x08 as the battle room
    // closes; the loading screen's first frame sends it, so clients load
    // alongside the host. While the world is built no frame runs: the
    // engine's load_progress hook keeps the connection alive meanwhile.
    static void launch(NetworkPlay& play, mp::Lobby& lobby) {
        auto& state = *play.net_;
        if (!state.connected || !state.connection.in_session || lobby.game == nullptr ||
            lobby.game->local_player_index >= OA_PLAYER_COUNT) {
            abort(play, "the battle room has no live session");
            return;
        }
        const auto local_slot = lobby.game->local_player_index;
        const auto host_slot = mp::lobby_host_slot(lobby);
        const auto* host_info =
            host_slot != mp::kNoSlot ? mp::slot_info(lobby, host_slot) : nullptr;
        if (host_info == nullptr) {
            abort(play, "the battle room has no host");
            return;
        }
        state.hosting = host_slot == local_slot;
        // A joiner that has already seen the start no longer treats a remote
        // computer as a computer, so this machine's brains go out first. The
        // joiner then still has them when it records the game.
        if (state.hosting) {
            play.runtime_.note_room_computers(lobby);
            nm::net_match_queue_game_start(&state.connection, *lobby.game);
        }
        state.building_game = lobby.game;
        std::copy(
            std::begin(lobby.game->slot_table),
            std::end(lobby.game->slot_table),
            state.host_slot_ids.begin()
        );
        state.loading_pace = {};
        state.load_rows = {};
        try {
            build(play, lobby, *host_info, local_slot);
        } catch (const std::exception& error) {
            abort(play, error.what());
        }
        state.building_game = nullptr;
    }

    /// Builds the network match from the battle room and starts it loading.
    ///
    /// Selects the host's map, fills the skirmish slots from the battle
    /// room's players and loads an empty world at the host's unit limit. The
    /// battle room's unit-sync verdicts decide which unit types load and
    /// their limits on every machine alike, and the verdicts are dropped
    /// afterwards. The launch then applies the lobby's slots and alliances,
    /// starts the score report and binds the match to the session. Throws
    /// std::runtime_error when the map is not installed, has too few start
    /// positions, or the lobby holds no local slot.
    ///
    /// @param play network play's state for the runtime that loads the match
    /// @param lobby battle room the match starts from; its unit-sync table is dropped
    /// @param host_info the host's setup block, which names the map and the unit limit
    /// @param local_slot this machine's player slot
    static void build(
        NetworkPlay& play,
        mp::Lobby& lobby,
        const mp::PlayerSetupInfo& host_info,
        uint8_t local_slot
    ) {
        Runtime& runtime = play.runtime_;
        auto& state = *play.net_;
        const std::string map(field_text(host_info.map_name, sizeof host_info.map_name));
        if (!play.select_map_named(map))
            throw std::runtime_error("the multiplayer map is not installed: " + map);
        // Watchers take no start position.
        uint16_t live = 0;
        for (int32_t slot = 0; slot < mp::kSlotCount; ++slot) {
            const auto* info = mp::slot_info(lobby, slot);
            const bool watching = info != nullptr && (info->options & OA_SETUP_OPTION_WATCHER) != 0;
            live += slot_live(mp::slot_player(lobby, slot)) && !watching ? 1 : 0;
        }
        // The start positions come from the schema for the host's slot table.
        runtime.state_.player_count =
            static_cast<uint16_t>(nm::match_layout_player_count(*lobby.game));
        if (runtime.map_player_capacity() < static_cast<int32_t>(live))
            throw std::runtime_error("the map has too few start positions for the lobby");
        const auto* local_info = mp::slot_info(lobby, local_slot);
        const bool local_watcher =
            local_info != nullptr && (local_info->options & OA_SETUP_OPTION_WATCHER) != 0;
        // The shared skirmish bootstrap builds the world for the local
        // viewpoint with the lobby's teams. Remote humans are entered as
        // computer slots so the bootstrap never takes one of them as local;
        // match_launch_apply restores every status afterwards.
        runtime.keep_player_skirmish_settings();
        for (int32_t slot = 0; slot < mp::kSlotCount; ++slot) {
            auto& player = mp::slot_player(lobby, slot);
            const auto* info = mp::slot_info(lobby, slot);
            auto& target = runtime.skirmish_settings_.slots[static_cast<std::size_t>(slot)];
            target = {};
            target.controller = slot == local_slot  ? entry::controller::human
                                : slot_live(player) ? entry::controller::computer
                                                    : entry::controller::disabled;
            target.alliance = mp::lobby_player_team(player);
            if (info != nullptr) {
                target.side = info->side;
                target.color = info->color;
                target.metal = info->metal_hundreds * 100;
                target.energy = info->energy_hundreds * 100;
            }
        }
        // The world starts empty at the host's unit limit; the local commander
        // is placed on the start position the load barrier assigns.
        const uint16_t limit =
            host_info.max_units != 0 ? host_info.max_units : kSkirmishUnitsPerPlayer;
        // The battle room's unit verdicts decide which unit types the match
        // loads and their build limits, on every machine alike; the table is
        // then dropped, so the match handles no 0x1a. A battle room that
        // never built its table keeps every type. The player slot tick never
        // tests a watcher for defeat.
        const bool verdicts = lobby.sync.record_count > 0;
        // A recording of the game keeps the verdicts the launch applies.
        state.unit_verdicts.clear();
        for (int32_t index = 0; index < lobby.sync.record_count; ++index) {
            const auto& record = lobby.sync.records[index];
            oa::netgame::UnitDefHandshakeRecord verdict{};
            verdict.subtype = static_cast<uint8_t>(oa::netgame::HandshakeSubtype::verdict);
            verdict.key = record.key;
            verdict.value = static_cast<uint32_t>(record.local) |
                            static_cast<uint32_t>(record.remote) << 8U |
                            static_cast<uint32_t>(static_cast<uint16_t>(record.limit)) << 16U;
            std::array<uint8_t, oa::formats::tad::layout::unit_check_record_bytes> bytes{};
            std::size_t written = 0;
            if (oa::netgame::encode_record(verdict, bytes.data(), bytes.size(), &written) ==
                oa::netgame::WireError::ok)
                state.unit_verdicts.push_back(bytes);
        }
        // The room's layer comes off before the match mounts its own, map
        // included. The list is the one this room built. The run's own
        // choice is put back afterwards, including when the start throws,
        // so a later skirmish does not play this room's list. A match that
        // never showed the battle room, such as the loopback check, has no
        // room list, so it plays the run's own, as a single-player start does.
        // A launch that bars mutators plays none instead, even then.
        const bool followed = runtime.room_followed();
        if (followed && runtime.room_match_unbuilt()) {
            std::cerr << "open-annihilation: mutators: room failed: the battle room's "
                         "list is not built\n";
            throw std::runtime_error("the battle room's list is not built");
        }
        std::vector<oa::data::mutators::ResolvedMutator> saved_choice(
            runtime.chosen_match_mutators().begin(), runtime.chosen_match_mutators().end()
        );
        std::vector<oa::data::mutators::ResolvedMutator> room_chosen;
        if (mp::lobby_mutators_barred(lobby)) {
            // The run's own pick stays in the restore below. This start plays none.
            room_chosen.clear();
        } else if (followed) {
            room_chosen.assign(
                runtime.room_match_mutators().begin(), runtime.room_match_mutators().end()
            );
        } else {
            room_chosen.assign(
                runtime.chosen_match_mutators().begin(), runtime.chosen_match_mutators().end()
            );
        }

        struct RunChoiceRestore {
            Runtime& runtime;
            std::vector<oa::data::mutators::ResolvedMutator> saved;

            ~RunChoiceRestore() {
                try {
                    runtime.choose_match_mutators(std::move(saved));
                } catch (...) {
                }
            }
        } run_choice_restore{runtime, std::move(saved_choice)};

        runtime.release_room_mutators();
        runtime.choose_match_mutators(std::move(room_chosen));
        // This machine's computers take their brains before the match's first
        // tick. Another machine's computers are not in the list. The seats
        // remembered afterwards are in the order this machine seated them.
        runtime.note_room_computers(lobby);
        const oa::data::brain_library::Seats room_seats = runtime.room_brain_seats();
        std::vector<uint8_t> room_computers;
        room_computers.reserve(room_seats.seats.size());
        for (const oa::data::brain_library::Seat& room_seat : room_seats.seats) {
            if (room_seat.place < 0 || room_seat.place > 9)
                continue;
            room_computers.push_back(static_cast<uint8_t>(room_seat.place));
        }
        runtime.seat_brains_once(runtime.resolve_brain_seats(
            room_seats, room_computers, oa::data::brain_library::Difficulty::hard
        ));
        if (!room_seats.seats.empty()) {
            oa::data::brain_library::Seats memory_seats = room_seats;
            for (std::size_t seat_index = 0; seat_index < memory_seats.seats.size(); ++seat_index)
                memory_seats.seats[seat_index].place = static_cast<int32_t>(seat_index);
            runtime.remember_brain_seats(oa::data::brain_library::Setup::multiplayer, memory_seats);
        }
        runtime.bootstrap_match(
            {.units_per_player = limit,
             .place_commanders = false,
             .defeat_allowed = !local_watcher,
             .unit_filter = {&lobby.sync, verdicts ? mark_agreed_units : nullptr},
             .multiplayer = true,
             // The host's room starts from the run's limit; a joiner plays
             // at the host's.
             .run_unit_limit = state.hosting}
        );
        // Other machines' computers, as the room showed them. This machine's
        // own are the brains the match just seated.
        runtime.keep_remote_played_brains(lobby);
        mp::unit_sync_destroy(lobby);
        if (!runtime.match_ || runtime.altitude_sight_blocked_)
            throw std::runtime_error(
                "the multiplayer bootstrap did not leave an empty match world"
            );
        // The host's START plays at hard difficulty, after the preferences
        // are saved. Skirmish notes none, so it keeps the game's difficulty.
        if (state.hosting)
            runtime.match_->set_difficulty(OA_DIFFICULTY_HARD);

        auto& world = runtime.match_->state();
        if (!nm::match_launch_apply(lobby, &world))
            throw std::runtime_error("the lobby has no local slot");
        // A seat this machine places on a side the match does not have ends
        // the start here, before the machines wait for each other to load.
        if (const auto past = nm::match_first_side_past_table(&world); past < OA_PLAYER_COUNT) {
            const auto& seated = world.game.players[past];
            const auto* info = world_player_info(&world, &seated);
            const auto side = info != nullptr ? static_cast<unsigned>(info->side) : 0U;
            abort(
                play,
                std::string(field_text(seated.name, sizeof seated.name)) + ": side " +
                    std::to_string(side) + " is not one of this game's sides"
            );
            return;
        }
        play.start_reporter(world, lobby.game);
        if (state.hosting)
            world.game.session_flags =
                static_cast<uint8_t>(world.game.session_flags | nm::kNetFlagGameStarted);
        nm::match_binding_init(&state.binding, runtime.match_.get(), state.net.get());
        // A difficulty chosen in the battle room replaces hard for the
        // computer player it names; one never chosen stays hard. The match
        // is the one the binding holds, and the first tick has not run.
        if (state.binding.match != nullptr) {
            auto* ais = oa::sim::ai::match_computer_players(*state.binding.match);
            for (int32_t slot = 0; slot < mp::kSlotCount; ++slot) {
                const auto& seated = mp::slot_player(lobby, slot);
                if (seated.status != mp::kSlotComputer || seated.in_use == 0)
                    continue;
                const auto noted = lobby.computer_difficulty[slot];
                const auto chosen =
                    lobby.computer_difficulty_set[slot] && noted <= OA_DIFFICULTY_HARD
                        ? static_cast<int32_t>(noted)
                        : OA_DIFFICULTY_HARD;
                oa::sim::ai::computer_players_set_difficulty(
                    ais, static_cast<uint8_t>(slot), chosen
                );
            }
        }
        nm::net_match_begin(
            state.net.get(),
            &state.connection,
            &world,
            nm::match_binding_sim(&state.binding),
            hooks(play)
        );
        if (const auto* profile = runtime.mod_profile())
            nm::net_match_bind_rules(
                state.net.get(), &profile->rules, runtime.map_places_neutral_units()
            );
        // The battle room's recorder session goes on in the match, its speed
        // lock with it, and the machine's chat with it.
        state.net->recorder = lobby.recorder;
        state.net->unicode_chat = play.unicode_chat();
        uint8_t slowest = 0;
        uint8_t fastest = 0;
        const bool locked = nm::net_match_speed_range(state.net.get(), &slowest, &fastest);
        share_speed_lock(runtime, locked, slowest, fastest);
        fill_integrity_identity(state.net->integrity.identity, runtime, map);
        state.binding.match->set_host_stays_watching(state.net->rules.host_stays_watching);
        state.lag_guard = {};
        state.vote_target = nm::no_player_id;
        nm::match_binding_install(&state.binding);
        for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
            const auto& player = world.game.players[slot];
            if (player.in_use == 0)
                continue;
            std::array<uint8_t, 10> allies{};
            for (std::size_t other = 0; other < allies.size(); ++other)
                allies[other] = other == slot || player.alliance[other] != 0 ? 1 : 0;
            runtime.match_->configure_player_alliances(slot, allies);
        }
        runtime.match_local_player_ = local_slot;
        nm::net_match_loader_waiting(state.net.get());
        state.local_slot = local_slot;
        state.loading = true;
        state.active = true;
        state.pause_seen = false;
        state.finish_pending = false;
        state.stalled_player = nm::no_player_id;
        runtime.status_ = "The other players are still loading";
        // The host's START is the real start. The set this room built is the
        // multiplayer set the next battle room offers. A guest stores
        // nothing, a start that threw never reaches here, and an empty list
        // stores nothing.
        if (state.hosting && followed) {
            const auto played = runtime.chosen_match_mutators();
            runtime.remember_mutators(
                mutators_memory::Setup::multiplayer, oa::data::mutators::loadout_of(played)
            );
        }
    }

    // One loading-screen frame, at most every 200 ms as the loading screen
    // paces it; true once the start positions are final and the other
    // machines have had time to pass their own barriers, so the commanders
    // can be placed (nm::commander_wait_frames). Each frame before the
    // barrier passes also reports the local players' load progress from the
    // rows the engine last reported.
    static bool loading_frame(NetworkPlay& play) {
        auto& state = *play.net_;
        const bool ready = nm::net_match_paced_loading_frame(
            state.net.get(), &state.loading_pace, state.load_rows.data()
        );
        watch_timeouts(play);
        return ready;
    }

    /// Places the local commanders once the load barrier passed, announces them and enters the
    /// match.
    ///
    /// It runs nm::commander_wait_frames loading frames after the barrier
    /// passed, so the other machines take the commanders' 0x09 and their
    /// copies stand on the start positions from it.
    ///
    /// Every slot simulated here (local or computer) that plays gets its
    /// commander on the start position the barrier assigned, with the host's
    /// starting resources; a watcher gets none, sees the whole map (mapping and
    /// line of sight off) and has its camera at half the view. The sight grids
    /// are rebuilt once the commanders stand. Loading then ends as the loader
    /// ends it (net_match_end_loading): the local player's block says the game
    /// has started and every local player's block and team go out, the host
    /// publishes its session again, and one economy period is counted. Throws
    /// std::runtime_error when the match has no host block, a local player has
    /// no start position or a commander cannot be placed.
    ///
    /// @param play network play's state for the runtime whose network match loaded
    static void finish(NetworkPlay& play) {
        Runtime& runtime = play.runtime_;
        auto& state = *play.net_;
        auto& world = runtime.match_->state();
        const auto* host = nm::launch_host_info(&world);
        if (host == nullptr)
            throw std::runtime_error("the match has no host block");
        std::vector<std::pair<uint8_t, uint8_t>> starts;
        for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
            const auto& player = world.game.players[slot];
            const bool simulated_here = player.status == OA_PLAYER_STATUS_LOCAL ||
                                        player.status == OA_PLAYER_STATUS_COMPUTER;
            if (player.in_use == 0 || !simulated_here || nm::match_slot_watcher(&world, slot))
                continue;
            const auto start = nm::net_match_start_position(state.net.get(), slot);
            if (start == nm::no_start_position)
                throw std::runtime_error("no start position was assigned to a local player");
            starts.emplace_back(slot, start);
        }
        nm::net_match_enter_game(state.net.get());
        // setup.map-scripted-units: each start position this machine places
        // is noted, the computer player moves last on a neutral map, and a
        // player the map lists units for takes them in place of its commander.
        std::array<int32_t, 10> start_positions{};
        start_positions.fill(-1);
        for (const auto& [slot, start] : starts)
            start_positions[slot] = start;
        const bool fixed_positions = (host->options & OA_SETUP_OPTION_FIXED_LOCATIONS) != 0;
        runtime.note_brain_map_starts();
        for (const auto& placed : starts) {
            const uint8_t slot = placed.first;
            // A position an earlier move swapped is read from the table.
            int32_t start = start_positions[slot];
            runtime.move_map_unit_computer_last(start_positions, start, fixed_positions);
            if (runtime.place_map_units(slot, start)) {
                runtime.remember_brain_player_start(slot, start, true, true);
                continue;
            }
            const auto& info = world.player_info[slot];
            const oa::sim::unit_spawn::PlayerSetup setup{
                info.side, info.color, host->metal_hundreds * 100, host->energy_hundreds * 100
            };
            const auto started = runtime.match_->start_player(
                slot,
                setup,
                runtime.selected_start_markers_,
                start,
                kBattlefieldWidth,
                kBattlefieldHeight,
                runtime
            );
            if (!started.position_found || started.unit == nullptr)
                throw std::runtime_error("a local commander could not be placed");
            runtime.remember_brain_player_start(slot, start, false, true);
        }
        // Another machine's player is noted at the start the host dealt it,
        // the same on every machine. The host sends each machine only its
        // own players' starts, so with fixed starts the others are worked
        // out as the host deals them, in the host's slot order; nothing
        // moves a computer player last then. Shuffled starts are the host's
        // alone, and a machine may then have moved its computer player last,
        // so another machine's player has none, and a brain reads only its
        // own start.
        std::array<int32_t, OA_PLAYER_COUNT> dealt{};
        dealt.fill(-1);
        if (fixed_positions)
            (void)nm::net_match_fixed_start_positions(state.net.get(), state.host_slot_ids, dealt);
        for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
            const auto& player = world.game.players[slot];
            if (player.in_use == 0 || nm::match_slot_watcher(&world, slot))
                continue;
            runtime.remember_brain_player_start(slot, dealt[slot], false, false);
        }
        runtime.publish_brain_starts(fixed_positions);
        // Every store starts at the host's resources.
        oa::ui::hud::set_starting_resources(
            world, oa::data::campaign::SessionKind::multiplayer, nullptr, nullptr
        );
        nm::match_apply_watcher_view(&world);
        if (nm::match_slot_watcher(&world, state.local_slot)) {
            runtime.match_camera_x_ = kBattlefieldWidth / 2;
            runtime.match_camera_z_ = kBattlefieldHeight / 2;
            runtime.match_camera_flags_ = 0;
        }
        runtime.reset_match_sight(true);
        // The network game is loaded here, before it records and tells the
        // other machines it is ready; its players' pass waits for the first
        // tick.
        state.binding.match->finish_load();
        state.recording_refused = false;
        start_recording(play);
        nm::net_match_end_loading(state.net.get());
        // The recorder's start: a pause under autopause or commander warp.
        // Under the warp the local player places its commander and says it
        // is done (setup.commander-warp); a watcher, or a profile without
        // the rule, keeps its start position and is done at once.
        nm::net_match_recorder_start(state.net.get());
        if (state.net->recorder.options.commander_warp != 0) {
            const auto* profile = runtime.mod_profile();
            const bool placing = profile != nullptr &&
                                 profile->rules.setup.commander_warp.enabled &&
                                 profile->rules.setup.commander_warp.available &&
                                 !nm::match_slot_watcher(&world, state.local_slot);
            if (placing)
                state.binding.match->begin_commander_placement();
            if (state.binding.match->commander_placement() ==
                oa::sim::match_runtime::CommanderPlacement::none)
                nm::net_match_warp_done(state.net.get());
        }
        nm::net_match_set_speed(state.net.get(), runtime.preferences_.current_game_speed, false);
        state.speed_seen = runtime.preferences_.current_game_speed;
        state.loading = false;
        play.report_game_event(oa::app::netgame::extension_api::report_event::game_started);
        runtime.enter_match_view();
    }

    /// Starts the game's recording: under the recorder's rules, when the player's setting keeps
    /// every game, or when --net-record or --record-game names a file.
    ///
    /// The recording machine's players come first, then the others in slot
    /// order; the unit checks are this machine's unit checksums and the
    /// battle room's verdicts. The recorder hack's layout (a `.tad`) is written under the
    /// recorder's rules and by --net-record, and a `.oademo` by the setting and by
    /// --record-game, unless its file name ends in `.tad`; never both. A game in a headless run
    /// or a check is recorded only when --net-record or --record-game names a file. Nothing is sent
    /// to the other machines on account of the setting. The recording is streamed to
    /// Recordings/.replays/partial from here on.
    ///
    /// @param play network play's state for the runtime whose match starts
    static void start_recording(NetworkPlay& play) {
        Runtime& runtime = play.runtime_;
        auto& state = *play.net_;
        reap_finishing(state);
        state.recording = {};
        state.partial.reset();
        state.recording_is_replay = false;
        state.recording_named = false;
        state.recording_start = {};
        state.game_finished = false;
        const bool recorder =
            state.net->rules.recorder_protocol != oa::netgame::recorder_protocol_plain;
        const auto& net_record = net_options().net_record;
        const auto& record_game = net_options().record_game;
        const bool by_setting = runtime.records_games_by_setting();
        // .record's name is game text, which the file's name holds as the
        // characters it stands for.
        const std::string asked = oa::present::decode_game_text(
            state.net->recorder.record_name, runtime.game_text_utf8()
        );
        // Which layout the game is written in, and whether it is recorded at all.
        const RecordingPlan plan = plan_recording(recorder, net_record, record_game, by_setting);
        if (plan.layout == RecordingLayout::none)
            return;
        const bool replay_layout = plan.layout == RecordingLayout::replay;
        state.recording_named = plan.named;
        auto& world = *state.net->world;
        const auto* profile = runtime.mod_profile();
        const auto* host = nm::launch_host_info(&world);
        const auto map = host != nullptr
                             ? std::string(field_text(host->map_name, sizeof host->map_name))
                             : std::string();
        oa::session::demo::RecordingSetup setup{};
        setup.max_units = static_cast<uint16_t>(world.game.units_per_player);
        setup.map_name = map;
        setup.recorder_text = kProgramLine;
        setup.date_text = local_time_text("%Y-%m-%d %H:%M");
        // A replay stores every packet plain.
        setup.compress = !replay_layout && profile != nullptr &&
                         profile->recorder.ta_demo_recorder.compress_files;
        // Empty text writes no sector, so a game without mutators is unchanged.
        const std::string loadout_text = runtime.played_loadout_text();
        if (loadout_text.size() > oa::session::demo::recording_loadout_most_bytes) {
            std::cerr
                << "demo mutators: the loadout is too long for the recording and is left out\n";
        } else {
            setup.loadout_text = loadout_text;
        }
        // Empty text writes no sector, so a game without brains is unchanged.
        const std::string brains_text = runtime.played_brains_text();
        if (brains_text.size() > oa::session::demo::recording_brains_most_bytes) {
            std::cerr << "demo brains: the seats are too long for the recording and are left out\n";
        } else {
            setup.brains_text = brains_text;
        }
        // A match only OA machines can play records both limits. Any other
        // match writes no sector, so its recording stays as it is.
        if (oa::netgame::oa_only(state.net->rules)) {
            oa::formats::tad::RecordedLimits recorded;
            recorded.weapon_types = state.net->rules.weapon_types;
            recorded.sides = state.net->rules.sides;
            setup.limits = recorded;
        }
        const bool utf8_text = runtime.game_text_utf8();
        std::vector<oa::session::demo::ReplayPlayerFacts> player_facts;
        const auto add_player = [&](uint8_t slot) {
            const auto& player = world.game.players[slot];
            oa::session::demo::RecordingPlayer entry{};
            entry.player_id = player.player_id;
            entry.name = std::string(field_text(player.name, sizeof player.name));
            entry.info = world.player_info[slot];
            // The recording keeps a player's lines in UTF-8 when this
            // machine sent or heard them so (net_match_say).
            const bool here = player.status != OA_PLAYER_STATUS_MIRRORED;
            auto* block = reinterpret_cast<uint8_t*>(&entry.info);
            oa::netgame::mark_unicode_chat(
                block,
                state.net->unicode_chat && (here || oa::netgame::announces_unicode_chat(block))
            );
            entry.team = player.team;
            oa::session::demo::ReplayPlayerFacts facts;
            facts.name = oa::present::decode_game_text(entry.name, utf8_text);
            const bool watcher = (entry.info.options & OA_SETUP_OPTION_WATCHER) != 0;
            facts.kind = watcher                                      ? "watcher"
                         : player.status == OA_PLAYER_STATUS_COMPUTER ? "computer"
                                                                      : "human";
            facts.recorded_here = player.status == OA_PLAYER_STATUS_LOCAL;
            player_facts.push_back(std::move(facts));
            setup.players.push_back(std::move(entry));
        };
        for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
            const auto& player = world.game.players[slot];
            if (player.in_use != 0 && (player.status == OA_PLAYER_STATUS_LOCAL ||
                                       player.status == OA_PLAYER_STATUS_COMPUTER))
                add_player(slot);
        }
        for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot)
            if (world.game.players[slot].in_use != 0 &&
                world.game.players[slot].status == OA_PLAYER_STATUS_MIRRORED)
                add_player(slot);
        // This machine's unit checksums as its battle room sent them, then
        // the verdicts its launch applied.
        int32_t unit_count = 0;
        const CheckHost host_check = check_host(runtime);
        const oa::AssetStore* unit_files = host_check.assets(host_check.context);
        const mp::LobbyUnit* units = mp::multiplayer_units(*unit_files, &unit_count);
        const auto checksums = unit_content_checksums(unit_files, units, unit_count);
        for (int32_t index = 1; units != nullptr && index < unit_count; ++index) {
            oa::netgame::UnitDefHandshakeRecord check{};
            check.subtype = static_cast<uint8_t>(oa::netgame::HandshakeSubtype::def_checksum);
            check.key = units[index].fbi_hash;
            check.value = checksums[static_cast<std::size_t>(index)];
            std::array<uint8_t, oa::formats::tad::layout::unit_check_record_bytes> bytes{};
            std::size_t written = 0;
            if (oa::netgame::encode_record(check, bytes.data(), bytes.size(), &written) ==
                oa::netgame::WireError::ok)
                setup.unit_checks.push_back(bytes);
        }
        setup.unit_checks.insert(
            setup.unit_checks.end(), state.unit_verdicts.begin(), state.unit_verdicts.end()
        );
        // The file the recording becomes.
        std::string file_name;
        if (replay_layout) {
            file_name = oa::session::demo::recording_file_name(
                local_time_text("%Y-%m-%d %H%M"), map, ".oademo"
            );
        } else if (!asked.empty()) {
            // .record names the file; it takes the recorder's manual extension.
            const std::string extension =
                profile != nullptr ? profile->recorder.ta_demo_recorder.manual_extension : ".tad";
            file_name = oa::session::demo::recording_file_name("", asked, extension);
        } else {
            const std::string extension =
                profile != nullptr ? profile->recorder.ta_demo_recorder.auto_extension : ".tad";
            file_name = oa::session::demo::recording_file_name(
                local_time_text("%Y-%m-%d %H%M"), map, extension
            );
        }
        // --record-game's and --net-record's file as it is given, else the mod's folder in
        // Recordings.
        state.recording_path = recording_file(
            !record_game.empty() ? record_game : net_record,
            runtime.user_folder(),
            runtime.files_mod_id(),
            path_from_utf8(file_name)
        );
        state.recording_is_replay = replay_layout;
        // What the game knows that the recording does not hold: the replay's description.
        if (replay_layout)
            state.recording_start = oa::session::demo::replay_begin(
                setup, replay_facts(runtime, std::move(player_facts), map, utf8_text)
            );
        std::string problem;
        state.partial = partial_make(
            runtime.user_folder(),
            runtime.files_mod_id(),
            state.recording_path.filename(),
            plan.layout,
            &problem
        );
        if (state.partial == nullptr) {
            std::cerr << "multiplayer: the recording could not be started: " << problem << '\n';
            state.recording_refused = true;
            return;
        }
        // A replay's description is kept before its stream is made, so that no game holds a
        // replay stream that a crash would leave with no description: a game whose description
        // cannot be kept is not recorded.
        if (replay_layout && !partial_write_start(state.partial->files, state.recording_start)) {
            std::cerr << "multiplayer: the recording could not be started: its description cannot "
                         "be kept\n";
            partial_discard(*state.partial);
            state.partial.reset();
            state.recording_refused = true;
            return;
        }
        oa::session::demo::recording_begin(&state.recording, std::move(setup));
        if (!oa::session::demo::recording_stream_to(
                &state.recording, state.partial->files.stream, &problem
            )) {
            std::cerr << "multiplayer: the recording could not be started: " << problem << '\n';
            state.recording = {};
            partial_discard(*state.partial);
            state.partial.reset();
            state.recording_refused = true;
            return;
        }
        if (!partial_write_note(state.partial->files, runtime.content_note_text()))
            std::cerr << "multiplayer: the recording's content note could not be kept\n";
    }

    /// Reads what the installed-package records hold of a package; nothing is known of one
    /// whose folder cannot be read.
    ///
    /// @param runtime the runtime
    /// @param kind the package's kind
    /// @param id the package's id
    /// @param version the version it must have; empty takes any
    /// @return what is known
    static Runtime::InstalledPackageFacts installed_package(
        const Runtime& runtime, std::string_view kind, std::string_view id, std::string_view version
    ) {
        try {
            return runtime.installed_package(kind, id, version);
        } catch (const std::exception& error) {
            std::cerr << "multiplayer: the installed packages could not be read for the recording: "
                      << error.what() << '\n';
            return {};
        }
    }

    /// Reads a package's digest from the game, which may throw, as an optional text.
    ///
    /// @param fetch reads the digest from the runtime
    /// @return the digest; empty text when it could not be read
    template <typename Fetch>
    static std::string digest_text_of(Fetch fetch) {
        try {
            return fetch();
        } catch (const std::exception& error) {
            std::cerr << "multiplayer: a package's digest could not be read for the recording: "
                      << error.what() << '\n';
            return {};
        }
    }

    /// Describes the game being recorded as a replay's description and manifest need it.
    ///
    /// @param runtime the runtime whose match starts
    /// @param players what is known of each recorded player, in the recording's order
    /// @param map the map's name as the lobby has it, game text
    /// @param utf8_text whether game text is UTF-8
    /// @return the facts
    static oa::session::demo::ReplayFacts replay_facts(
        const Runtime& runtime,
        std::vector<oa::session::demo::ReplayPlayerFacts> players,
        const std::string& map,
        bool utf8_text
    ) {
        oa::session::demo::ReplayFacts facts;
        facts.recorded_at = utc_time_text();
        facts.recorded_by = recording_program();
        facts.platform = recording_platform();
        facts.how = "multiplayer";
        facts.engine_version = OA_ENGINE_VERSION;
        facts.game_kind = "multiplayer";
        // A game an extension launched names its own kind and what it knows of the game.
        if (const auto extension = recorded_game_facts()) {
            facts.game_kind = extension->kind;
            facts.game_kind_object = extension->object_json;
        }
        facts.map_name = oa::present::decode_game_text(map, utf8_text);
        const oa::data::saves::ContentRecord content = runtime.played_content_record();
        if (content.present) {
            facts.base_game = content.base_game;
            facts.expansions = content.expansions;
        }
        facts.players = std::move(players);
        const auto* profile = runtime.mod_profile();
        if (profile != nullptr && !profile->id.empty()) {
            oa::formats::oademo::ModRef mod;
            mod.id = profile->id;
            mod.name = profile->name;
            mod.version = profile->version;
            if (profile->packaging.revision != 0)
                mod.revision = profile->packaging.revision;
            mod.sim_hash = oa::data::mod_profile::digest_text(profile->sim_hash);
            facts.mod = mod;
            oa::formats::oademo::Need need;
            need.kind = "oamod";
            need.id = profile->id;
            need.version = profile->version;
            if (profile->packaging.revision != 0)
                need.revision = profile->packaging.revision;
            need.sim_hash = mod.sim_hash;
            // Every package need carries a digest: the installed package's, else the mod folder's.
            if (const std::string digest =
                    digest_text_of([&runtime] { return runtime.played_mod_digest(); });
                !digest.empty())
                need.sha256 = digest;
            facts.packages.push_back(std::move(need));
        }
        // A map from a map pack names the pack, by its package's digest or else its folder's. A
        // pack that is not installed has no digest to give, so no need is written for it.
        if (const auto pack = oa::data::map_pack::split_pack_map_name(map)) {
            oa::formats::oademo::Need need;
            need.kind = "oamap";
            need.id = std::string(pack->id);
            const auto installed = installed_package(runtime, "oamap", need.id, {});
            if (installed.found) {
                need.version = installed.version;
                const std::string digest =
                    !installed.sha256.empty()
                        ? installed.sha256
                        : digest_text_of([&runtime, &installed] {
                              return runtime.folder_digest_text(installed.folder);
                          });
                if (!digest.empty()) {
                    need.sha256 = digest;
                    facts.packages.push_back(std::move(need));
                }
            }
        }
        return facts;
    }

    /// Says how the last game's recording went, once the finish has run.
    ///
    /// The frame calls this, so that the log names the file soon after the game is left, not at
    /// the next game or the end of the session.
    ///
    /// @param state network play's state
    static void reap_if_finished(NetState& state) {
        if (state.finishing != nullptr && static_cast<bool>(state.finishing->ran))
            reap_finishing(state);
    }

    /// Waits for the last game's recording to be in place and says how it went.
    ///
    /// @param state network play's state
    static void reap_finishing(NetState& state) {
        if (state.finishing == nullptr)
            return;
        auto finishing = std::move(state.finishing);
        finishing->join();
        if (!finishing->stream_problem.empty())
            std::cerr << "multiplayer: the recording is not whole: " << finishing->stream_problem
                      << '\n';
        const auto& result = finishing->result;
        if (result.ok) {
            // The file may have taken another name than the one wanted.
            state.recording_path = result.placed;
            std::cout << "multiplayer: recorded the game to " << path_to_utf8(result.placed)
                      << '\n';
        } else if (result.empty) {
            std::cout << "multiplayer: nothing was recorded\n";
        } else {
            std::cerr << "multiplayer: the recording was not written: " << result.error << '\n';
        }
    }

    /// Finishes a recording on the thread that runs it, or the thread that draws.
    ///
    /// @param argument the RecordingFinishing
    static void run_finishing(void* argument) {
        auto& finishing = *static_cast<RecordingFinishing*>(argument);
        try {
            // What the stream still holds is written, and the file closed, here.
            const bool whole =
                finishing.stream == nullptr || finishing.stream->close(&finishing.stream_problem);
            const uint64_t dropped =
                finishing.stream != nullptr ? finishing.stream->dropped() : uint64_t{0};
            finishing.stream.reset();
            if (!whole) {
                finishing.finish.cut = true;
                finishing.finish.end.ended = "cut";
            }
            // A packet that did not fit a chunk leaves a hole in the recording: it says so.
            if (dropped != 0) {
                finishing.finish.end.ended = "cut";
                if (!finishing.stream_problem.empty())
                    finishing.stream_problem += "; ";
                finishing.stream_problem +=
                    std::to_string(dropped) + " packets did not fit the file and are missing";
            }
            finishing.result = finish_partial(finishing.finish);
        } catch (const std::exception& error) {
            finishing.result = {};
            finishing.result.error = error.what();
        }
        // The game's hold on the recording's files ends with the finish.
        std::error_code failure;
        const auto lock_file = finishing.finish.files.lock_file;
        if (finishing.partial != nullptr)
            finishing.partial->lock.reset();
        std::filesystem::remove(lock_file, failure);
        finishing.ran = true;
    }

    /// Finishes the running game's recording, if one is being made, and stops it.
    ///
    /// The stream is closed and the recording finished into its place: compressed into a
    /// `.oademo`, or moved as the recorder hack's `.tad`. A run nobody watches does so before this
    /// returns, so that a check reads the file; a game a person plays does so on a thread of its
    /// own, which the next game or the end of the session waits for, so that no frame waits for
    /// the disk.
    ///
    /// @param play network play's state
    static void finish_recording(NetworkPlay& play) {
        auto& state = *play.net_;
        if (!state.recording.started)
            return;
        const uint32_t length_ticks = state.net->world != nullptr ? state.net->world->game.tick : 0;
        auto finishing = std::make_unique<RecordingFinishing>();
        // The stream goes to the finish, which closes it: what it still holds is written there,
        // off the thread that draws.
        finishing->stream = std::move(state.recording.stream);
        state.recording = {};
        finishing->partial = std::move(state.partial);
        if (finishing->partial == nullptr)
            return;
        finishing->finish.files = finishing->partial->files;
        finishing->finish.replay_layout = state.recording_is_replay;
        finishing->finish.start = std::move(state.recording_start);
        finishing->finish.end.ended = state.game_finished ? "finished" : "left";
        if (length_ticks != 0)
            finishing->finish.end.length_ticks = length_ticks;
        // An automatic recording never replaces another: the second of one minute takes " (2)",
        // and so on, where the file is placed. A file that was named is replaced.
        finishing->finish.destination = state.recording_path;
        finishing->finish.replace = state.recording_named;
        state.recording_start = {};
        // The finish may run beside a frame, so it holds copies of everything it reads.
        RecordingFinishing* running = finishing.get();
        state.finishing = std::move(finishing);
        if (!play.runtime_.person_plays() ||
            !oa::base::threads::start_thread(running->thread, &run_finishing, running))
            run_finishing(running);
        else
            running->started = true;
        if (!running->started)
            reap_finishing(state);
    }

    static void abort(NetworkPlay& play, std::string_view reason) {
        Runtime& runtime = play.runtime_;
        runtime.status_ = "Multiplayer start failed: " + std::string(reason);
        std::cerr << "multiplayer: " << reason << '\n';
        play.net_leave();
        if (runtime.match_)
            runtime.leave_match();
        runtime.pending_screen_ = screen_id(Screen::main_menu);
    }

    static void watch_timeouts(NetworkPlay& play) {
        Runtime& runtime = play.runtime_;
        auto& state = *play.net_;
        const auto stalled = state.net->timeout_player;
        if (stalled != state.stalled_player) {
            state.stalled_player = stalled;
            if (stalled != nm::no_player_id)
                runtime.status_ = "Waiting for " + player_name(*state.net->world, stalled) + "...";
        }
        if (stalled != nm::no_player_id && nm::net_match_timeout_expired(state.net.get(), stalled))
            state.stalled_player = nm::no_player_id;
    }

    // Loopback check: ephemeral ports on 127.0.0.1, the peer serviced while
    // this machine waits. Both machines count the bytes they send each other
    // and read in one counter (loopback_deliver); the clock and the waits
    // stay real.
    static bool
    loopback_configure(NetworkPlay& play, uint32_t seed, NetState* peer, int64_t* in_flight) {
        auto& state = *play.net_;
        if (state.connected)
            nm::net_connection_destroy(&state.connection);
        nm::SessionLobbyConfig config{};
        const uint8_t loopback[4] = {127, 0, 0, 1};
        std::memcpy(config.host.bind_ip, loopback, sizeof loopback);
        std::memcpy(config.host.enum_target, loopback, sizeof loopback);
        config.host.stream_port_first = 0;
        config.host.datagram_port_first = 0;
        config.host.enum_port = 0;
        config.host.seed = seed;
        config.host.clock.in_flight = in_flight;
        state.connected = nm::net_connection_create(&state.connection, config);
        if (state.connected) {
            state.connection.pump_context = peer;
            state.connection.pump_other = pump_peer;
        }
        return state.connected;
    }

    static void loopback_pump(NetworkPlay& play, uint32_t wait_ms) {
        if (play.net_->connection.opened)
            oa::netgame::sock::host_pump(play.net_->connection.host, wait_ms);
    }

    // A networked tick pushed out at once; the paced flush of real play
    // would hold records for the wall clock the check does not wait on.
    static void loopback_step(NetworkPlay& play) {
        auto& connection = play.net_->connection;
        play.net_simulation_step();
        nm::packet_layer_flush(connection.packets, nm::net_connection_time(&connection), true);
    }

    // Services both machines until each has read every byte the other sent
    // it. The system hands a loopback datagram over a little after it is
    // sent, later on a loaded machine; read when it happens to be there, a
    // record would reach the other machine's match a tick early or late, and
    // the receiver spreads a frame's records over the ticks from the one it
    // arrives at, so its units, and the fight, would play otherwise from run
    // to run. Read here, each machine applies the other's records of a tick
    // at the same tick of every run.
    static void loopback_deliver(NetworkPlay& host, NetworkPlay& joiner, const int64_t& in_flight) {
        LoopbackWait wait;
        for (;;) {
            loopback_pump(host, 0);
            loopback_pump(joiner, 0);
            if (in_flight <= 0)
                return;
            if (!wait.next_round())
                throw std::runtime_error(
                    "net loopback check: bytes one machine sent the other did not arrive"
                );
            std::this_thread::sleep_for(kLoopbackDeliverPause);
        }
    }

    static UnitPose unit_pose(NetworkPlay& play, uint16_t unit) {
        Runtime& runtime = play.runtime_;
        const auto& record = runtime.match_->state().units[unit];
        const auto* ground = runtime.match_->ground_runtime(unit);
        return {
            record.position.x,
            record.position.y,
            record.position.z,
            record.heading,
            ground != nullptr ? ground->movement.speed : 0
        };
    }

    static uint16_t local_commander(NetworkPlay& play) {
        Runtime& runtime = play.runtime_;
        const auto& world = runtime.match_->state();
        for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
            const auto& unit = world.units[slot];
            if (unit.type_index != 0 && unit.owner_index == runtime.match_local_player_)
                return static_cast<uint16_t>(slot);
        }
        return 0;
    }

    /// A unit as its owner's machine held it at one tick.
    struct OwnerPose {
        UnitPose pose{};
        uint16_t type{}; ///< UnitDef index; 0 for an empty slot
        bool finished{}; ///< Unit.build_remaining was 0
    };

    /// Returns a unit's position, heading and ground speed in a match.
    ///
    /// @param match match holding the unit
    /// @param unit unit slot
    /// @return the pose; speed 0 for a unit without a ground driver
    static UnitPose match_pose(const sim::match_runtime::Match& match, uint16_t unit) {
        const auto& record = match.state().units[unit];
        const auto* ground = match.ground_runtime(unit);
        return {
            record.position.x,
            record.position.y,
            record.position.z,
            record.heading,
            ground != nullptr ? ground->movement.speed : 0
        };
    }

    /// Plays the loopback match with the computer player the host seated.
    ///
    /// The computer player plays on the host's machine at hard difficulty;
    /// the joiner holds its slot as simulated elsewhere, in the host's
    /// machine group, and copies its units from the records its player
    /// sends. It builds its base for `build_ticks`. The host's player then
    /// gives it a squad of kbots on the host's machine, and the joiner's
    /// player gives it one more across the session. With the squad its land
    /// strike sets out against its enemy's commander (the joiner's; the
    /// host's when the joiner watches) and fights until its units have fired,
    /// killed, and lost two; whatever is left of that commander then
    /// self-destructs, and its player watches. When the joiner played and
    /// seated no computer of its own, the host's side has won. A computer
    /// the joiner seated stays in the game, so that seating does not wait
    /// for a winner. When the joiner played, the final economy settles
    /// either way, the computer player's among it. The host then leaves,
    /// and the joiner retires it and the computer player, keeping their
    /// final figures. The joiner's own computer player stays. A recording
    /// of that match plays back when the check kept one.
    ///
    /// Throughout, the joiner's copy of every finished unit the computer
    /// player moves stays within a pixel of where its owner had it at the
    /// tick of the last 0x2c the joiner applied from the computer player,
    /// once a full record has placed it if a pad carried it (CarriedCopies);
    /// after each part of the match both machines hold the same units, the
    /// computer player's shots reach the joiner one for one, and the joiner
    /// counts the computer player's kills and losses as the host does.
    /// Progress goes to stdout; a failure throws std::runtime_error.
    ///
    /// @param host network play's state for the hosting runtime, in its running match
    /// @param joiner network play's state for the joining runtime, in its running match
    /// @param build_ticks ticks the computer player builds before the strike
    /// @param computer_id session id of the computer player
    /// @param watching whether the joiner only watches
    /// @param joiner_computer session id of the joiner's computer player, or none
    /// @param in_flight the bytes the machines have sent each other and not yet read
    static void computer_match(
        NetworkPlay& host,
        NetworkPlay& joiner,
        std::size_t build_ticks,
        uint32_t computer_id,
        bool watching,
        uint32_t joiner_computer,
        const int64_t& in_flight
    ) {
        const auto require = [](bool ok, const char* what) {
            if (!ok)
                throw std::runtime_error(std::string("net loopback check: ") + what);
        };
        auto& host_world = *host.net_->net->world;
        auto& client_world = *joiner.net_->net->world;
        auto& host_match = *host.net_->binding.match;
        auto& client_match = *joiner.net_->binding.match;
        const auto host_local = host_world.game.local_player_index;
        const auto client_local = client_world.game.local_player_index;
        const auto host_id = host_world.game.players[host_local].player_id;
        const auto client_id = client_world.game.players[client_local].player_id;
        const auto host_ai = player_slot(host_world, computer_id);
        const auto client_ai = player_slot(client_world, computer_id);
        const auto host_on_client = player_slot(client_world, host_id);
        const auto client_on_host = player_slot(host_world, client_id);
        require(
            host_ai < OA_PLAYER_COUNT && client_ai < OA_PLAYER_COUNT &&
                host_on_client < OA_PLAYER_COUNT && client_on_host < OA_PLAYER_COUNT,
            "a machine has no slot for a player"
        );
        const auto& computer_here = host_world.game.players[host_ai];
        const auto& computer_copy = client_world.game.players[client_ai];
        require(
            computer_here.status == OA_PLAYER_STATUS_COMPUTER &&
                computer_copy.status == OA_PLAYER_STATUS_MIRRORED &&
                computer_copy.machine_group != 0 &&
                computer_copy.machine_group ==
                    client_world.game.players[host_on_client].machine_group,
            "the computer player is not played on the host's machine alone"
        );
        require(
            host_world.game.difficulty == OA_DIFFICULTY_HARD,
            "the host does not play its computer player at hard difficulty"
        );
        const bool allied =
            computer_here.alliance[host_local] != 0 && computer_copy.alliance[host_on_client] != 0;
        require(allied != watching, "the computer player's team did not reach the match");

        // The joiner's computer player is allied with everyone, and they with
        // it, before any tick. It builds, and it is not the host computer
        // player's nearest enemy, so the strike still meets the joiner's
        // commander. Both matches agree, and a later follow reads the same
        // rows from the player records.
        if (joiner_computer != nm::no_player_id) {
            const auto ally_with_all = [&](sim::match_runtime::Match& match, World& world) {
                const auto allied_slot = player_slot(world, joiner_computer);
                require(allied_slot < OA_PLAYER_COUNT, "the joiner's computer player has no slot");
                std::array<uint8_t, 10> row{};
                row.fill(1);
                match.configure_player_alliances(allied_slot, row);
                for (uint8_t other = 0; other < OA_PLAYER_COUNT; ++other) {
                    if (other == allied_slot || world.game.players[other].in_use == 0)
                        continue;
                    std::array<uint8_t, 10> allies{};
                    for (std::size_t index = 0; index < allies.size(); ++index)
                        allies[index] = world.game.players[other].alliance[index];
                    allies[allied_slot] = 1;
                    match.configure_player_alliances(other, allies);
                }
            };
            ally_with_all(host_match, host_world);
            ally_with_all(client_match, client_world);
        }

        // The owner's pose of every unit slot at each of the last ticks.
        std::vector<std::vector<OwnerPose>> owner_poses(
            kComputerPoseTicks, std::vector<OwnerPose>(host_world.unit_slot_count)
        );
        uint32_t copies_compared = 0;
        uint32_t copies_exact = 0;
        uint32_t copies_worst_offset = 0;
        CarriedCopies carried{&client_world, std::vector<uint8_t>(client_world.unit_slot_count)};
        const PlacedCopyWatch placed_watch(joiner.net_->binding, carried);
        // The computer player's shots: the host's weapons launch them and
        // share each as a 0x0d, from which the joiner launches its own.
        LaunchedShots host_shots{host_ai};
        LaunchedShots client_shots{client_ai};
        const LaunchedShotCounter host_counter(host_match, host_shots);
        const LaunchedShotCounter client_counter(client_match, client_shots);
        const auto note_owner = [&] {
            auto& poses = owner_poses[host_world.game.tick % kComputerPoseTicks];
            for (uint16_t slot = 1; slot < host_world.unit_slot_count; ++slot) {
                const auto& unit = host_world.units[slot];
                poses[slot] =
                    unit_live(unit)
                        ? OwnerPose{match_pose(host_match, slot), unit.type_index, unit.build_remaining == 0.0F}
                        : OwnerPose{};
            }
        };
        // A copy moved from records matches its owner at the tick of the
        // record the joiner last applied from the computer player. A copy
        // a pad has carried leaves it off its owner's path until its next
        // full record (CarriedCopies), so it is not compared until then;
        // nor are units still being built.
        const auto compare_copies = [&] {
            for (uint16_t slot = 1; slot < client_world.unit_slot_count; ++slot) {
                const auto& unit = client_world.units[slot];
                const bool waiting = carried.carried[slot] != 0 || unit.attach_parent != 0;
                carried.carried[slot] = unit_live(unit) && unit.owner_index == client_ai && waiting;
            }
            const auto tick = computer_copy.last_sim_tick;
            if (tick <= 0 || static_cast<uint32_t>(tick) > host_world.game.tick ||
                host_world.game.tick - static_cast<uint32_t>(tick) >= kComputerPoseTicks)
                return;
            const auto& poses = owner_poses[static_cast<uint32_t>(tick) % kComputerPoseTicks];
            for (uint16_t slot = 1; slot < client_world.unit_slot_count; ++slot) {
                const auto& unit = client_world.units[slot];
                const auto& owner = poses[slot];
                if (!unit_live(unit) || unit.owner_index != client_ai ||
                    unit.build_remaining != 0.0F || owner.type != unit.type_index ||
                    !owner.finished || client_match.ground_runtime(slot) == nullptr ||
                    carried.carried[slot] != 0)
                    continue;
                const auto copy = match_pose(client_match, slot);
                ++copies_compared;
                copies_exact += pose_divergence(copy, owner.pose) == 0 ? 1 : 0;
                const UnitPose ground{
                    copy.x, owner.pose.y, copy.z, owner.pose.heading, owner.pose.speed
                };
                copies_worst_offset =
                    std::max(copies_worst_offset, pose_divergence(ground, owner.pose));
            }
        };
        // The joiner applies the host's records of a tick at that tick, and
        // the host the joiner's at the next (loopback_deliver).
        loopback_deliver(host, joiner, in_flight);
        const auto play_tick = [&] {
            host.net_frame();
            joiner.net_frame();
            loopback_step(host);
            note_owner();
            loopback_deliver(host, joiner, in_flight);
            loopback_step(joiner);
            compare_copies();
            loopback_deliver(host, joiner, in_flight);
        };
        // Units are created and die on the joiner a few ticks after the host;
        // both hold the same ones once none is on its way.
        const auto settle_limit = 3u * host_world.game.units_per_player;
        const auto settle = [&](const char* what) {
            for (uint32_t extra = 0; extra < settle_limit; ++extra) {
                play_tick();
                if (unit_roster(host_world) == unit_roster(client_world))
                    return;
            }
            require(false, what);
        };
        const auto score_line = [&] {
            std::cout << "kills " << computer_here.kills << " / " << computer_copy.kills
                      << ", losses " << computer_here.losses << " / " << computer_copy.losses
                      << ", shots " << host_shots.count << " / " << client_shots.count;
        };

        // The computer player builds its base. Its commander stands on the
        // joiner from its 0x09 on.
        for (std::size_t tick = 0; tick < build_ticks; ++tick)
            play_tick();
        settle("the joiner does not hold the computer player's base");
        const auto built = live_units(host_world, host_ai);
        require(built >= kComputerBuiltUnits, "the computer player built too little");
        require(
            commander_of(client_world, client_ai) != 0 &&
                commander_of(client_world, client_ai) == commander_of(host_world, host_ai),
            "the joiner has no copy of the computer player's commander"
        );
        std::cout << "net loopback check: the computer player built " << built - 1
                  << " units by tick " << host_world.game.tick << "; both machines hold them\n";
        if (joiner_computer != nm::no_player_id) {
            const auto joiner_ai = player_slot(client_world, joiner_computer);
            const auto joiner_copy = player_slot(host_world, joiner_computer);
            const auto joiner_on_host = player_slot(host_world, client_id);
            require(
                joiner_ai < OA_PLAYER_COUNT && joiner_copy < OA_PLAYER_COUNT &&
                    client_world.game.players[joiner_ai].status == OA_PLAYER_STATUS_COMPUTER &&
                    host_world.game.players[joiner_copy].status == OA_PLAYER_STATUS_MIRRORED &&
                    host_world.game.players[joiner_copy].machine_group != 0 &&
                    joiner_on_host < OA_PLAYER_COUNT &&
                    host_world.game.players[joiner_copy].machine_group ==
                        host_world.game.players[joiner_on_host].machine_group,
                "the joiner's computer player is not played on the joiner's machine alone"
            );
            const auto joiner_built = live_units(client_world, joiner_ai);
            require(
                joiner_built >= kComputerBuiltUnits &&
                    live_units(host_world, joiner_copy) == joiner_built,
                "the joiner's computer player did not build on both machines"
            );
            std::cout << "net loopback check: the joiner's computer player built "
                      << joiner_built - 1 << " units; both machines hold them\n";
        }

        // A player makes a unit beside its commander and gives it to the
        // computer player; the giver's machine hands it over.
        const auto give = [&](sim::match_runtime::Match& giver_match,
                              World& giver_world,
                              uint8_t giver,
                              uint8_t recipient,
                              const char* type_name,
                              int32_t aside) {
            const auto type = unit_type_named(giver_world, type_name);
            const auto commander = commander_of(giver_world, giver);
            require(type != 0 && commander != 0, "a giver has no commander or unit type to give");
            const auto gift = make_unit(giver_match, giver_world, giver, type, commander, aside);
            require(gift != 0, "a giver could not make the unit to give");
            for (int frame = 0; frame < 4; ++frame)
                play_tick();
            require(
                unit_live(host_world.units[gift]) && unit_live(client_world.units[gift]),
                "a machine has no copy of the unit to be given"
            );
            const auto held_before = units_of_type(host_world, host_ai, type);
            giver_match.transfer_unit(gift, recipient);
            settle("the machines disagree on a given unit");
            require(
                !unit_live(host_world.units[gift]) && !unit_live(client_world.units[gift]) &&
                    units_of_type(host_world, host_ai, type) == held_before + 1 &&
                    units_of_type(client_world, client_ai, type) == held_before + 1,
                "the computer player was not given the unit on both machines"
            );
        };
        // The host's player gives the computer player a squad of Arm kbots on
        // the host's machine: each new unit of the computer player reaches the
        // joiner from its player, each old one dies on both. The joiner's
        // player gives it one more across the session (0x14): the host makes
        // the unit for its computer player. The computer player, playing
        // Core, builds neither type itself.
        for (int32_t gift = 0; gift < kComputerSquadGifts; ++gift)
            give(
                host_match,
                host_world,
                host_local,
                host_ai,
                kComputerSquadType,
                kComputerGiftAside + gift * kComputerGiftSpacing
            );
        if (!watching)
            give(
                client_match,
                client_world,
                client_local,
                client_ai,
                kComputerAcrossType,
                kComputerGiftAside
            );
        std::cout << "net loopback check: the host's player gave the computer player "
                  << kComputerSquadGifts << " x " << kComputerSquadType
                  << (watching ? "" : ", the joiner's player one ")
                  << (watching ? "" : kComputerAcrossType)
                  << "; both machines hold them as the computer player's\n";

        // With its squad the computer player strikes the nearest enemy unit:
        // its enemy's commander (the joiner's; the host's when the joiner
        // watches), which fights back. The strike runs until the computer
        // player's units have fired, killed and lost units.
        const auto* computer_players = sim::ai::match_computer_players(host_match);
        const auto strike = std::find_if(
            std::begin(computer_players->players[host_ai].tasks),
            std::end(computer_players->players[host_ai].tasks),
            [](const sim::ai::ComputerTask& task) {
                return task.kind == sim::ai::TaskKind::strike &&
                       task.squad == sim::ai::Squad::land_strike;
            }
        );
        require(
            strike != std::end(computer_players->players[host_ai].tasks),
            "the computer player has no land strike"
        );
        auto& enemy_world = watching ? host_world : client_world;
        const auto enemy_slot = enemy_world.game.local_player_index;
        const auto enemy_commander = commander_of(enemy_world, enemy_slot);
        require(enemy_commander != 0, "the computer player's enemy has no commander");
        const auto enemy_health = enemy_world.units[enemy_commander].health;
        const auto kills_before = computer_here.kills;
        const auto losses_before = computer_here.losses;
        bool struck = false;
        const auto fought = [&] {
            return struck && host_shots.count != 0 && computer_here.kills > kills_before &&
                   computer_here.losses - losses_before >= kComputerFightLosses;
        };
        uint32_t fight_ticks = 0;
        for (; fight_ticks < kComputerFightTicks && !fought(); ++fight_ticks) {
            play_tick();
            struck = struck || strike->attacking != 0;
        }
        std::cout << "net loopback check: the computer player struck and fought for " << fight_ticks
                  << " ticks: ";
        score_line();
        std::cout << "\n";
        require(struck, "the computer player did not strike");
        require(
            host_shots.count != 0 && computer_here.kills > kills_before,
            "the computer player's units killed nothing"
        );
        require(
            computer_here.losses - losses_before >= kComputerFightLosses,
            "the computer player lost too little in the fight"
        );
        require(
            !unit_live(enemy_world.units[enemy_commander]) ||
                enemy_world.units[enemy_commander].health < enemy_health,
            "the computer player did the enemy commander no harm"
        );

        // Whatever of its enemy's commander is left self-destructs; its player
        // watches once its defeat countdown runs out, and no one fights any
        // more.
        if (unit_live(enemy_world.units[enemy_commander]))
            (watching ? host_match : client_match)
                .toggle_self_destruct(std::span<const uint16_t>(&enemy_commander, 1));
        const auto enemy_id = enemy_world.game.players[enemy_slot].player_id;
        const auto defeated = [&] {
            return watches(info_for_id(client_world, enemy_id)) &&
                   watches(info_for_id(host_world, enemy_id));
        };
        for (uint32_t tick = 0; tick < kLoopbackDefeatTicks && !defeated(); ++tick)
            play_tick();
        require(defeated(), "the computer player's enemy was not defeated to watching");
        settle("the machines disagree on the units after the fight");
        std::cout << "net loopback check: tick " << host_world.game.tick << " / "
                  << client_world.game.tick << ", computer player's units "
                  << live_units(host_world, host_ai) << " / " << live_units(client_world, client_ai)
                  << ", ";
        score_line();
        std::cout << ", copies compared " << copies_compared << ", exact " << copies_exact
                  << ", farthest off " << copies_worst_offset << " (16.16), placed off pads "
                  << carried.placed << "\n";
        require(
            client_shots.count == host_shots.count,
            "the computer player's shots did not all reach the joiner"
        );
        require(
            computer_copy.kills == computer_here.kills &&
                computer_copy.losses == computer_here.losses,
            "the joiner counts the computer player's kills or losses otherwise"
        );
        require(
            copies_compared != 0 && copies_worst_offset < kCopyOffsetLimit,
            "a copy left the computer player's unit's path"
        );
        require(
            host.net_->net->record_errors == 0 && joiner.net_->net->record_errors == 0 &&
                joiner.net_->binding.refused_creates == 0,
            "record errors or refused creates"
        );

        // The host's final economy, the computer player's among it, settles
        // before it leaves. Until then the joiner holds no economy figures
        // for the computer player, which sends no periodic 0x28 (only the
        // viewpoint player does). One computer on the host's team wins once
        // the joiner's human watches. A computer the joiner seated stays
        // alive and taking part, and a multiplayer win needs every other
        // side out or sharing allied victory, which this seating does not
        // set, so that check does not wait for a winner it cannot get.
        if (!watching) {
            if (joiner_computer == nm::no_player_id) {
                // The host's victory comes at one of its 30-tick outcome checks
                // after its enemy watches; play on until the outcome is decided.
                for (uint32_t tick = 0; tick < kLoopbackVictoryTicks &&
                                        host_match.outcome() == sim::scenario::Outcome::ongoing;
                     ++tick)
                    play_tick();
                require(
                    host_match.outcome() == sim::scenario::Outcome::victory,
                    "the host's side did not win"
                );
                std::cout << "net loopback check: the host's side won at tick "
                          << host_world.game.tick << "\n";
            } else {
                std::cout << "net loopback check: both computer players stay in play\n";
            }
            require(
                computer_copy.energy_produced_total == 0.0 &&
                    computer_copy.metal_produced_total == 0.0,
                "the joiner had economy figures for the computer player before its final economy"
            );
            LoopbackWait settle_wait;
            while (!host.net_final_economy_settled()) {
                require(settle_wait.next_round(), "the host's final economy did not settle");
                joiner.net_frame();
                loopback_step(joiner);
                loopback_pump(joiner, 1);
            }
        }
        // The host leaves; the joiner retires its machine group, the host's
        // player and its computer player, as departed.
        host.net_leave();
        LoopbackWait retire_wait;
        while (computer_copy.in_use != 0 || client_world.game.players[host_on_client].in_use != 0) {
            require(retire_wait.next_round(), "the joiner did not retire the host's machine");
            joiner.net_frame();
            loopback_step(joiner);
            loopback_pump(joiner, 1);
            loopback_pump(host, 1);
        }
        std::cout
            << "net loopback check: the host left; the joiner retired it and its computer player"
            << " with reasons "
            << static_cast<int>(client_world.game.players[host_on_client].reject_reason) << " / "
            << static_cast<int>(computer_copy.reject_reason) << "; the computer player's figures "
            << computer_here.kills << "/" << computer_here.losses << " kills/losses, "
            << computer_here.energy_produced_total << " energy and "
            << computer_here.metal_produced_total << " metal produced; the joiner's copy "
            << computer_copy.kills << "/" << computer_copy.losses << ", "
            << computer_copy.energy_produced_total << ", " << computer_copy.metal_produced_total
            << "\n";
        require(
            computer_copy.reject_reason == 1 &&
                client_world.game.players[host_on_client].reject_reason == 1,
            "the joiner did not retire the host's machine as departed"
        );
        if (!watching)
            require(
                computer_copy.kills == computer_here.kills &&
                    computer_copy.losses == computer_here.losses &&
                    static_cast<float>(computer_copy.energy_produced_total) ==
                        static_cast<float>(computer_here.energy_produced_total) &&
                    static_cast<float>(computer_copy.metal_produced_total) ==
                        static_cast<float>(computer_here.metal_produced_total),
                "the joiner's figures for the computer player differ from its final ones"
            );
        if (joiner_computer != nm::no_player_id) {
            const auto kept = player_slot(client_world, joiner_computer);
            require(
                kept < OA_PLAYER_COUNT && client_world.game.players[kept].in_use != 0 &&
                    client_world.game.players[kept].reject_reason == 0,
                "the joiner retired its own computer player with the host"
            );
            std::cout << "net loopback check: the joiner's computer player stayed\n";
            if (!net_options().net_record.empty()) {
                oa::session::demo::DemoPlayback playback;
                std::string error;
                require(
                    oa::session::demo::demo_open(&playback, host.net_->recording_path, &error) &&
                        !playback.frames.empty(),
                    "the recording did not open"
                );
                int32_t named = 0;
                for (const auto& recorded : playback.demo.players)
                    if (recorded.name.rfind("AI:", 0) == 0)
                        ++named;
                require(named >= 2, "the recording does not name both computer players");
                playback.tick = 0xffffffffu;
                const auto transport = oa::session::demo::demo_transport(&playback);
                std::vector<uint8_t> buffer(65536);
                int32_t delivered = 0;
                for (;;) {
                    uint32_t from_id = 0;
                    uint32_t to_id = 0;
                    auto size = static_cast<uint32_t>(buffer.size());
                    const auto result = transport.receive(
                        transport.context, &from_id, &to_id, buffer.data(), &size
                    );
                    if (result != oa::netgame::transport_result::ok)
                        break;
                    ++delivered;
                }
                require(delivered != 0, "the recording played back no frames");
                std::cout << "net loopback check: recording played back " << delivered
                          << " frames\n";
            }
        }
        joiner.net_leave();
    }
};

void NetworkPlay::destroy_net_state(NetState* state) noexcept {
    // A recording on its way into place is finished before the session goes.
    if (state != nullptr)
        NetHost::reap_finishing(*state);
    if (state != nullptr && state->connected)
        nm::net_connection_destroy(&state->connection);
    delete state;
}

// A skirmish recording. The packet layer's transport sends nowhere: every
// record is kept by the recording as it is handed to the match, and the
// socket host stays null. The match's unit ranges are not moved.
struct SkirmishRecord {
    std::unique_ptr<nm::PacketLayer> packets{};
    nm::NetConnection connection{};
    std::unique_ptr<nm::NetMatch> net{};
    nm::MatchBinding binding{};
    oa::session::demo::DemoRecording recording{};
    std::filesystem::path recording_path{};
    std::unique_ptr<PartialRecording> partial{};
    bool recording_is_replay{};
    bool recording_named{};
    oa::formats::oademo::Replay recording_start{};
    std::unique_ptr<RecordingFinishing> finishing{};
    /// The id the recording stores for each slot. 0 means the slot is not a sender.
    std::array<uint32_t, OA_PLAYER_COUNT> sender_ids{};
    /// True when this recording set the live-game bit and must clear it.
    bool live_raised{};
    /// 1 once a create for that slot has been recorded and its death has not.
    std::vector<uint8_t> announced{};
    /// The owner slot of an announced unit, so a death between ticks can be sent.
    std::vector<uint8_t> announced_owner{};
    /// Economy periods so far. A 0x28 goes out every fourth, as a network game sends it.
    uint32_t economy_periods{};
    /// True when the match being recorded is a campaign mission.
    bool campaign{};
};

namespace {

/// A route bit and an air-driver flag word, put back after a unit state is sent.
struct RouteSnap {
    uint16_t slot{};
    uint8_t navigation{};
    bool has_navigation{};
    uint8_t air{};
    bool has_air{};
};

/// Feature changes are recorded. A hit is still applied on this machine.
bool skirmish_records_features(void*) {
    return true;
}

/// A feature hit is applied here and not sent. This machine is the only one.
bool skirmish_feature_hit_here(void*, uint16_t, int32_t, int32_t) {
    return false;
}

/// The players' setup blocks are already in the recording's header. Sending
/// them again would store the live id 0, which a recording cannot name.
void skirmish_status_unchanged(void*) {
}

/// Sends one local player's unit state, then puts the route bits and the
/// player's last tick back so the rest of the tick matches a skirmish that
/// is not recording.
void skirmish_local_ticked(void* context, Player& player) {
    auto& binding = *static_cast<nm::MatchBinding*>(context);
    auto* match = binding.match;
    auto* world = binding.net != nullptr ? binding.net->world : nullptr;
    std::vector<RouteSnap> snaps;
    const int32_t last_tick = player.last_sim_tick;
    if (match != nullptr && world != nullptr && world->units != nullptr) {
        uint32_t unit_count = 0;
        Unit* first = world_player_units(world, &player, &unit_count);
        const uint32_t slot_count = world->unit_slot_count;
        snaps.reserve(unit_count);
        for (uint32_t unit_index = 0; unit_index < unit_count; ++unit_index) {
            const auto& unit = first[unit_index];
            if (unit.type_index == 0 || unit.movement == 0 || unit.id >= slot_count)
                continue;
            RouteSnap snap{};
            snap.slot = unit.id;
            if (auto* ground = match->ground_runtime(snap.slot)) {
                snap.has_navigation = true;
                snap.navigation = ground->navigation.flags;
            }
            if (auto* driver = match->air_driver(snap.slot)) {
                snap.has_air = true;
                snap.air = driver->flags;
            }
            snaps.push_back(snap);
        }
    }
    // A skirmish never runs the network launch, which is what writes
    // Game.unit_def_id_bits. A unit state needs that width, and playback
    // sizes the same width from the loaded table. The field is put on for
    // the send and the skirmish's own value is put back.
    int32_t saved_def_bits = 0;
    bool replaced_def_bits = false;
    if (world != nullptr) {
        saved_def_bits = world->game.unit_def_id_bits;
        const auto current_bits = static_cast<unsigned>(saved_def_bits);
        if (current_bits < 1u || current_bits > oa::netgame::unit_state_max_def_index_bits) {
            const int32_t sized_def_bits = nm::unit_def_id_bits_for_count(world->unit_def_count);
            const auto sized_width = static_cast<unsigned>(sized_def_bits);
            if (sized_width >= 1u && sized_width <= oa::netgame::unit_state_max_def_index_bits) {
                world->game.unit_def_id_bits = sized_def_bits;
                replaced_def_bits = true;
            }
        }
    }
    nm::match_binding_send_local_tick(context, player);
    if (replaced_def_bits && world != nullptr)
        world->game.unit_def_id_bits = saved_def_bits;
    if (match != nullptr) {
        for (const RouteSnap& snap : snaps) {
            if (snap.has_navigation) {
                if (auto* ground = match->ground_runtime(snap.slot))
                    ground->navigation.flags = snap.navigation;
            }
            if (snap.has_air) {
                if (auto* driver = match->air_driver(snap.slot))
                    driver->flags = snap.air;
            }
        }
    }
    player.last_sim_tick = last_tick;
    if (binding.net != nullptr && binding.net->connection != nullptr &&
        binding.net->connection->packets != nullptr)
        nm::packet_layer_flush(binding.net->connection->packets, 0, true);
}

/// Stores one record under the sender's recording id. The live id can be 0,
/// which a recording rejects, so the slot's stored id is used instead.
void skirmish_record_seen(void* context, uint32_t sender, const uint8_t* bytes, std::size_t size) {
    auto& binding = *static_cast<nm::MatchBinding*>(context);
    auto* held = static_cast<SkirmishRecord*>(binding.full_record_probe.context);
    if (held == nullptr || !held->recording.started || bytes == nullptr || size == 0)
        return;
    uint32_t stored = 0;
    if (binding.net != nullptr && binding.net->world != nullptr) {
        const auto& players = binding.net->world->game.players;
        for (uint8_t player_slot = 0; player_slot < OA_PLAYER_COUNT; ++player_slot) {
            if (players[player_slot].in_use != 0 && players[player_slot].player_id == sender) {
                stored = held->sender_ids[player_slot];
                break;
            }
        }
    }
    if (stored == 0)
        return;
    oa::session::demo::recording_add(&held->recording, stored, recording_clock_ms(), {bytes, size});
}

/// Sends one record from a player's live id. The recording stores the slot's id.
template <class Record>
bool send_from_player(nm::NetMatch* net, uint32_t from_id, const Record& record) {
    constexpr auto length = oa::netgame::record_table_length(static_cast<uint8_t>(Record::type));
    std::array<uint8_t, length> bytes{};
    std::size_t written = 0;
    if (oa::netgame::encode_record(record, bytes.data(), bytes.size(), &written) !=
        oa::netgame::WireError::ok)
        return false;
    return nm::net_match_send(
        net, from_id, oa::netgame::broadcast_destination_id, bytes.data(), written
    );
}

/// True for a player whose records this machine sends.
bool sends_here(const Player& player) {
    return player.in_use != 0 &&
           (player.status == OA_PLAYER_STATUS_LOCAL || player.status == OA_PLAYER_STATUS_COMPUTER);
}

/// Remembers that a create for this slot is in the recording, or that it is not.
void mark_announced(SkirmishRecord& record, uint16_t index, uint8_t owner, bool live) {
    if (record.announced.size() <= index) {
        record.announced.resize(static_cast<std::size_t>(index) + 1u);
        record.announced_owner.resize(record.announced.size());
    }
    record.announced[index] = live ? 1u : 0u;
    record.announced_owner[index] = owner;
}

/// Announces a unit the tick created, and remembers it so a later pass does not.
void skirmish_unit_created(void* context, uint16_t index) {
    auto& binding = *static_cast<nm::MatchBinding*>(context);
    auto* held = static_cast<SkirmishRecord*>(binding.full_record_probe.context);
    if (index < binding.movement.size())
        binding.movement[index] = oa::netgame::MovementRecord{};
    auto* world = binding.net != nullptr ? binding.net->world : nullptr;
    auto* unit = world != nullptr ? world_unit_at(world, index) : nullptr;
    const auto* owner = unit != nullptr ? world_player_ref(world, unit->owner) : nullptr;
    if (unit != nullptr && owner != nullptr && sends_here(*owner))
        nm::net_match_send_unit_created(binding.net, unit);
    if (held != nullptr && owner != nullptr)
        mark_announced(*held, index, static_cast<uint8_t>(owner->index), true);
}

/// Announces a unit the tick killed, and forgets it.
void skirmish_unit_killed(
    void* context, uint16_t index, const sim::match_runtime::KillOutcome& outcome
) {
    auto& binding = *static_cast<nm::MatchBinding*>(context);
    auto* held = static_cast<SkirmishRecord*>(binding.full_record_probe.context);
    auto* world = binding.net != nullptr ? binding.net->world : nullptr;
    const auto* unit = world != nullptr ? world_unit_at(world, index) : nullptr;
    const auto* owner = unit != nullptr ? world_player_ref(world, unit->owner) : nullptr;
    if (owner != nullptr && sends_here(*owner)) {
        const auto record = nm::encode_unit_killed_record(
            *world,
            index,
            static_cast<uint8_t>(outcome.kind),
            outcome.killed_percent,
            outcome.wreck_level
        );
        (void)nm::net_match_send(
            binding.net,
            owner->player_id,
            oa::netgame::broadcast_destination_id,
            record.data(),
            record.size()
        );
    }
    if (held != nullptr)
        mark_announced(*held, index, 0, false);
}

/// Sets the live-game bit when it is clear, and remembers that this recording set it.
void raise_skirmish_live(SkirmishRecord& record) {
    if (record.net == nullptr || record.net->world == nullptr)
        return;
    auto& flags = record.net->world->game.session_flags;
    if ((flags & nm::kNetFlagLive) != 0)
        return;
    flags = static_cast<uint8_t>(flags | nm::kNetFlagLive);
    record.live_raised = true;
}

/// Clears the live-game bit when this recording set it. A bit set by anything else stays.
void lower_skirmish_live(SkirmishRecord& record) {
    if (!record.live_raised)
        return;
    record.live_raised = false;
    if (record.net == nullptr || record.net->world == nullptr)
        return;
    auto& flags = record.net->world->game.session_flags;
    flags = static_cast<uint8_t>(flags & ~nm::kNetFlagLive);
}

/// Records a create for a unit that appeared while the bit was down, and a
/// death for one that vanished then. The tick's own hooks cover the rest.
void catch_up_units(SkirmishRecord& record) {
    auto* net = record.net.get();
    auto* world = net != nullptr ? net->world : nullptr;
    if (world == nullptr || world->units == nullptr)
        return;
    for (uint32_t unit_slot = 1; unit_slot < world->unit_slot_count; ++unit_slot) {
        const Unit& unit = world->units[unit_slot];
        const bool live = unit.type_index != 0;
        const bool known = unit_slot < record.announced.size() && record.announced[unit_slot] != 0;
        if (live && !known) {
            const auto* owner = world_player_ref(world, unit.owner);
            if (owner != nullptr && sends_here(*owner)) {
                nm::net_match_send_unit_created(net, &unit);
                mark_announced(
                    record,
                    static_cast<uint16_t>(unit_slot),
                    static_cast<uint8_t>(owner->index),
                    true
                );
            }
            continue;
        }
        if (!live && known) {
            const uint8_t owner_slot = record.announced_owner[unit_slot];
            if (owner_slot < OA_PLAYER_COUNT) {
                const auto& owner = world->game.players[owner_slot];
                if (sends_here(owner)) {
                    oa::netgame::UnitKilledRecord killed{};
                    killed.unit_index = static_cast<uint16_t>(unit_slot);
                    killed.attacker_owner_id = nm::no_player_id;
                    (void)send_from_player(net, owner.player_id, killed);
                }
            }
            mark_announced(record, static_cast<uint16_t>(unit_slot), 0, false);
        }
    }
}

/// Records one 0x28 for every sender, from that player's own totals.
void send_skirmish_economies(SkirmishRecord& record) {
    auto* net = record.net.get();
    auto* world = net != nullptr ? net->world : nullptr;
    if (world == nullptr)
        return;
    for (uint8_t player_slot = 0; player_slot < OA_PLAYER_COUNT; ++player_slot) {
        auto& player = world->game.players[player_slot];
        if (!sends_here(player) || record.sender_ids[player_slot] == 0)
            continue;
        oa::netgame::EconomyRecord economy{};
        economy.kills = player.kills;
        economy.losses = player.losses;
        economy.commanders_killed = player.commanders_killed;
        economy.commanders_lost = player.commanders_lost;
        economy.metal = player.metal;
        economy.energy = player.energy;
        economy.metal_storage = player.metal_storage;
        economy.energy_storage = player.energy_storage;
        economy.energy_produced_total = static_cast<float>(player.energy_produced_total);
        economy.energy_requested_total = static_cast<float>(player.energy_requested_total);
        economy.energy_wasted_total = static_cast<float>(player.energy_wasted_total);
        economy.metal_produced_total = static_cast<float>(player.metal_produced_total);
        economy.metal_requested_total = static_cast<float>(player.metal_requested_total);
        economy.metal_wasted_total = static_cast<float>(player.metal_wasted_total);
        (void)send_from_player(net, player.player_id, economy);
    }
    if (record.connection.packets != nullptr)
        nm::packet_layer_flush(record.connection.packets, 0, true);
}

/// A 0x28 every fourth economy period, the pace net_match_economy_period sends at.
///
/// The period is 30 ticks. The call sees the tick that has just finished.
void send_skirmish_economy_period(SkirmishRecord& record) {
    auto* world = record.net != nullptr ? record.net->world : nullptr;
    if (world == nullptr)
        return;
    const uint32_t tick = world->game.tick;
    if (tick == 0 || tick % 30u != 0)
        return;
    const uint32_t periods = ++record.economy_periods;
    if ((periods & 3u) != 0)
        return;
    send_skirmish_economies(record);
}

/// One line per sender: the units still standing and the totals a 0x28 carries.
///
/// @param world the world the recording is finishing
/// @param word `skirmish` or `campaign`, the line's first word
void print_skirmish_players(const World& world, const char* word) {
    std::array<uint32_t, OA_PLAYER_COUNT> live_by_owner{};
    if (world.units != nullptr) {
        for (uint32_t unit_slot = 1; unit_slot < world.unit_slot_count; ++unit_slot) {
            const auto& unit = world.units[unit_slot];
            if (unit.type_index == 0 || unit.owner_index >= OA_PLAYER_COUNT)
                continue;
            ++live_by_owner[unit.owner_index];
        }
    }
    for (uint8_t player_slot = 0; player_slot < OA_PLAYER_COUNT; ++player_slot) {
        const auto& player = world.game.players[player_slot];
        if (!sends_here(player))
            continue;
        std::printf(
            "%s player: slot %u units %u kills %d losses %d metal %.9g energy %.9g\n",
            word != nullptr ? word : "skirmish",
            static_cast<unsigned>(player_slot),
            live_by_owner[player_slot],
            static_cast<int>(player.kills),
            static_cast<int>(player.losses),
            static_cast<double>(player.metal),
            static_cast<double>(player.energy)
        );
    }
}

/// Writes one JSON string, quotes included, with the controls a JSON string escapes.
void append_json_string(std::string& out, std::string_view json_text) {
    out.push_back('"');
    for (const char character : json_text) {
        const auto byte = static_cast<unsigned char>(character);
        switch (byte) {
        case '"':
            out += "\\\"";
            break;
        case '\\':
            out += "\\\\";
            break;
        case '\b':
            out += "\\b";
            break;
        case '\f':
            out += "\\f";
            break;
        case '\n':
            out += "\\n";
            break;
        case '\r':
            out += "\\r";
            break;
        case '\t':
            out += "\\t";
            break;
        default:
            if (byte < 0x20) {
                constexpr char kDigits[] = "0123456789abcdef";
                out += "\\u00";
                out.push_back(kDigits[byte >> 4]);
                out.push_back(kDigits[byte & 0x0f]);
            } else {
                out.push_back(static_cast<char>(byte));
            }
            break;
        }
    }
    out.push_back('"');
}

/// The `game.campaign` object: the campaign's name, the mission's number and its name.
///
/// @param name the campaign's name
/// @param mission the mission's number in the campaign's list
/// @param mission_name the mission's name
/// @return compact JSON
std::string
campaign_kind_json(std::string_view name, int32_t mission, std::string_view mission_name) {
    std::string object;
    object.reserve(name.size() + mission_name.size() + 48);
    object += "{\"name\":";
    append_json_string(object, name);
    object += ",\"mission\":";
    object += std::to_string(mission);
    object += ",\"mission_name\":";
    append_json_string(object, mission_name);
    object += '}';
    return object;
}

/// Takes the match's network hooks down and clears the live-game bit.
void detach_skirmish(SkirmishRecord& record) {
    if (record.binding.match != nullptr) {
        record.binding.match->multiplayer = {};
        record.binding.match = nullptr;
    }
    record.binding.features = {};
    if (record.net != nullptr && record.net->world != nullptr) {
        auto& flags = record.net->world->game.session_flags;
        flags = static_cast<uint8_t>(flags & ~nm::kNetFlagLive);
        record.net->world = nullptr;
    }
    record.binding.net = nullptr;
}

/// Says how a skirmish recording finished, once its thread has ended.
void reap_skirmish(SkirmishRecord& record) {
    if (record.finishing == nullptr)
        return;
    auto finishing = std::move(record.finishing);
    finishing->join();
    const char* word = record.campaign ? "campaign" : "skirmish";
    if (!finishing->stream_problem.empty())
        std::cerr << word << ": the recording is not whole: " << finishing->stream_problem << '\n';
    const auto& result = finishing->result;
    if (result.ok) {
        record.recording_path = result.placed;
        std::cout << word << ": recorded the game to " << path_to_utf8(result.placed) << '\n';
    } else if (result.empty) {
        std::cout << word << ": nothing was recorded\n";
    } else {
        std::cerr << word << ": the recording was not written: " << result.error << '\n';
    }
}

/// Gives up a recording that never opened its stream.
void discard_skirmish_partial(SkirmishRecord& record) {
    record.recording = {};
    if (record.partial == nullptr)
        return;
    partial_discard(*record.partial);
    record.partial.reset();
}

} // namespace

void NetworkPlay::advance_skirmish_recording() {
    if (skirmish_ == nullptr || !skirmish_->recording.started)
        return;
    // The bit is up for this step only: the tick, its feature changes and a
    // unit a command made while it was down are recorded, and it is cleared
    // again once the frame has presented.
    raise_skirmish_live(*skirmish_);
    catch_up_units(*skirmish_);
    send_skirmish_economy_period(*skirmish_);
}

void NetworkPlay::hide_skirmish_live_bit() {
    if (skirmish_ == nullptr)
        return;
    lower_skirmish_live(*skirmish_);
}

void NetworkPlay::print_feature_cells(const char* label, const oa::World& world) {
    uint32_t cells = 0;
    // FNV-1a over each occupied cell's column, row and feature, in that order.
    uint32_t digest = 2166136261u;
    const int32_t width = world.game.map_width;
    const int32_t height = world.game.map_height;
    if (world.plots != nullptr && width > 0 && height > 0) {
        for (int32_t row = 0; row < height; ++row) {
            for (int32_t column = 0; column < width; ++column) {
                const uint16_t feature =
                    world
                        .plots
                            [static_cast<std::size_t>(row) * static_cast<std::size_t>(width) +
                             static_cast<std::size_t>(column)]
                        .feature;
                if (feature >= OA_PLOT_FEATURE_RESERVED)
                    continue;
                ++cells;
                const uint32_t parts[3] = {
                    static_cast<uint32_t>(column),
                    static_cast<uint32_t>(row),
                    feature,
                };
                for (const uint32_t part : parts) {
                    digest ^= part;
                    digest *= 16777619u;
                }
            }
        }
    }
    std::printf("%s: cells %u digest %08x\n", label != nullptr ? label : "", cells, digest);
}

void NetworkPlay::destroy_skirmish_record(SkirmishRecord* record) noexcept {
    if (record == nullptr)
        return;
    // The packet layer is owned by the record. The connection must not free it.
    record->connection.packets = nullptr;
    record->connection.host = nullptr;
    delete record;
}

NetworkPlay::~NetworkPlay() {
    const char* word = skirmish_ != nullptr && skirmish_->campaign ? "campaign" : "skirmish";
    try {
        finish_skirmish_recording();
    } catch (const std::exception& error) {
        std::cerr << word << ": the recording could not be finished: " << error.what() << '\n';
    }
}

void NetworkPlay::consider_skirmish_recording() {
    if (skirmish_decided_)
        return;
    if (runtime_.running_match() == nullptr)
        return;
    const auto session_kind = runtime_.running_session_kind();
    if (session_kind != oa::data::campaign::SessionKind::skirmish &&
        session_kind != oa::data::campaign::SessionKind::campaign)
        return;
    // A network match that is still loading does not take the tick, and a
    // replay takes it above. Neither is recorded here.
    if (demo_ || (net_ && (net_->active || net_->loading)))
        return;
    const bool campaign_match = session_kind == oa::data::campaign::SessionKind::campaign;
    const char* word = campaign_match ? "campaign" : "skirmish";
    skirmish_decided_ = true;
    if (skirmish_ != nullptr) {
        reap_skirmish(*skirmish_);
        detach_skirmish(*skirmish_);
        skirmish_.reset();
    }
    const bool recorder =
        runtime_wire_rules(runtime_).recorder_protocol != oa::netgame::recorder_protocol_plain;
    const RecordingPlan plan = plan_recording(
        recorder,
        net_options().net_record,
        net_options().record_game,
        runtime_.records_games_by_setting()
    );
    if (plan.layout == RecordingLayout::none)
        return;
    auto* world = &runtime_.running_match()->state();
    if (world->units == nullptr)
        return;
    try {
        auto held = std::unique_ptr<SkirmishRecord, void (*)(SkirmishRecord*) noexcept>(
            new (std::nothrow) SkirmishRecord(), destroy_skirmish_record
        );
        if (held == nullptr) {
            std::cerr << word << ": the recording could not be started: no storage\n";
            return;
        }
        held->campaign = campaign_match;

        // A failure before the recording is kept takes the hooks down. The
        // guard dies before `held`, so the record is still there to detach.
        struct ArmGuard {
            SkirmishRecord* record{};
            bool keep{};

            ~ArmGuard() {
                if (!keep && record != nullptr) {
                    detach_skirmish(*record);
                    discard_skirmish_partial(*record);
                }
            }
        } guard{held.get()};

        held->packets.reset(new (std::nothrow) nm::PacketLayer());
        if (held->packets == nullptr) {
            std::cerr << word << ": the recording could not be started: no packet storage\n";
            return;
        }
        nm::packet_layer_create(held->packets.get());
        oa::netgame::NetTransport transport{};
        nm::packet_layer_start(held->packets.get(), transport);
        held->connection.packets = held->packets.get();
        held->connection.rules = runtime_wire_rules(runtime_);
        held->net = std::make_unique<nm::NetMatch>();
        nm::match_binding_prepare(&held->binding, runtime_.running_match(), held->net.get());
        // The record hook's context is the binding. The probe names this
        // recording, and nothing calls the probe: its placed hook stays null.
        held->binding.full_record_probe.context = held.get();
        auto hooks = nm::match_binding_hooks(&held->binding);
        hooks.record_seen = skirmish_record_seen;
        const uint16_t load_flags = world->game.load_flags;
        const int32_t timeout_seconds = world->game.player_timeout_seconds;
        nm::net_match_begin(
            held->net.get(), &held->connection, world, nm::match_binding_sim(&held->binding), hooks
        );
        // The begin marks the game live and, when no timeout is set, writes
        // the network default. Put the match's own words back, the live
        // bit included. The recording raises that bit only while it sends,
        // so the rest of the game still reads as the match it is.
        world->game.load_flags = load_flags;
        world->game.player_timeout_seconds = timeout_seconds;
        world->game.session_flags =
            static_cast<uint8_t>(world->game.session_flags & ~nm::kNetFlagLive);
        held->net->unicode_chat = unicode_chat();
        nm::match_binding_install(&held->binding);
        // Hits stay on this machine. Destructions, reclaims and resurrections
        // are recorded, as a host records them.
        held->binding.features.networked = skirmish_records_features;
        runtime_.running_match()->multiplayer.local_player_ticked = skirmish_local_ticked;
        runtime_.running_match()->multiplayer.player_status_changed = skirmish_status_unchanged;
        runtime_.running_match()->multiplayer.unit_created = skirmish_unit_created;
        runtime_.running_match()->multiplayer.unit_killed = skirmish_unit_killed;
        runtime_.running_match()->multiplayer.feature_hit_elsewhere = skirmish_feature_hit_here;
        raise_skirmish_live(*held);

        const bool replay_layout = plan.layout == RecordingLayout::replay;
        held->recording_named = plan.named;
        const auto* profile = runtime_.mod_profile();
        // A mission's map is named as its OTA names it, which is how playback finds the file.
        std::string map = runtime_.played_map_name();
        Runtime::RunningMissionFacts mission_facts;
        if (campaign_match) {
            mission_facts = runtime_.running_mission_facts();
            if (!mission_facts.map_title.empty())
                map = mission_facts.map_title;
        }
        const bool utf8_text = runtime_.game_text_utf8();
        oa::session::demo::RecordingSetup setup{};
        setup.max_units = static_cast<uint16_t>(world->game.units_per_player);
        setup.map_name = map;
        setup.recorder_text = kProgramLine;
        setup.date_text = local_time_text("%Y-%m-%d %H:%M");
        setup.compress = !replay_layout && profile != nullptr &&
                         profile->recorder.ta_demo_recorder.compress_files;
        const std::string loadout_text = runtime_.played_loadout_text();
        if (loadout_text.size() > oa::session::demo::recording_loadout_most_bytes) {
            std::cerr
                << "demo mutators: the loadout is too long for the recording and is left out\n";
        } else {
            setup.loadout_text = loadout_text;
        }
        const std::string brains_text = runtime_.played_brains_text();
        if (brains_text.size() > oa::session::demo::recording_brains_most_bytes) {
            std::cerr << "demo brains: the seats are too long for the recording and are left out\n";
        } else {
            setup.brains_text = brains_text;
        }
        if (oa::netgame::oa_only(held->connection.rules)) {
            oa::formats::tad::RecordedLimits recorded;
            recorded.weapon_types = held->connection.rules.weapon_types;
            recorded.sides = held->connection.rules.sides;
            setup.limits = recorded;
        }
        std::vector<oa::session::demo::ReplayPlayerFacts> player_facts;
        const auto add_player = [&](uint8_t player_slot) {
            const auto& player = world->game.players[player_slot];
            oa::session::demo::RecordingPlayer entry{};
            // The live id of the first slot is 0. A recording numbers players
            // from 1, and playback seats them in this order.
            entry.player_id = static_cast<uint32_t>(player_slot) + 1u;
            held->sender_ids[player_slot] = entry.player_id;
            entry.name = std::string(field_text(player.name, sizeof player.name));
            entry.info = world->player_info[player_slot];
            const bool here = player.status != OA_PLAYER_STATUS_MIRRORED;
            auto* block = reinterpret_cast<uint8_t*>(&entry.info);
            oa::netgame::mark_unicode_chat(
                block,
                held->net->unicode_chat && (here || oa::netgame::announces_unicode_chat(block))
            );
            entry.team = player.team;
            oa::session::demo::ReplayPlayerFacts facts;
            facts.name = oa::present::decode_game_text(entry.name, utf8_text);
            const bool watcher = (entry.info.options & OA_SETUP_OPTION_WATCHER) != 0;
            facts.kind = watcher                                      ? "watcher"
                         : player.status == OA_PLAYER_STATUS_COMPUTER ? "computer"
                                                                      : "human";
            facts.recorded_here = player.status == OA_PLAYER_STATUS_LOCAL;
            player_facts.push_back(std::move(facts));
            setup.players.push_back(std::move(entry));
        };
        for (uint8_t player_slot = 0; player_slot < OA_PLAYER_COUNT; ++player_slot) {
            const auto& player = world->game.players[player_slot];
            if (player.in_use != 0 && (player.status == OA_PLAYER_STATUS_LOCAL ||
                                       player.status == OA_PLAYER_STATUS_COMPUTER))
                add_player(player_slot);
        }
        for (uint8_t player_slot = 0; player_slot < OA_PLAYER_COUNT; ++player_slot) {
            const auto& player = world->game.players[player_slot];
            if (player.in_use != 0 && player.status == OA_PLAYER_STATUS_MIRRORED)
                add_player(player_slot);
        }
        if (setup.players.empty()) {
            std::cerr << word << ": the recording could not be started: the match has no players\n";
            return;
        }
        // The table the match loaded, mutators included. Playback builds the
        // same table from the recording's loadout and compares these keys.
        if (world->unit_defs != nullptr) {
            for (uint32_t index = 1; index < world->unit_def_count; ++index) {
                const auto& def = world->unit_defs[index];
                oa::netgame::UnitDefHandshakeRecord check{};
                check.subtype = static_cast<uint8_t>(oa::netgame::HandshakeSubtype::def_checksum);
                check.key = def.fbi_hash;
                check.value = def.content_checksum;
                std::array<uint8_t, oa::formats::tad::layout::unit_check_record_bytes> bytes{};
                std::size_t written = 0;
                if (oa::netgame::encode_record(check, bytes.data(), bytes.size(), &written) ==
                    oa::netgame::WireError::ok)
                    setup.unit_checks.push_back(bytes);
            }
        }
        std::string file_name;
        if (replay_layout) {
            file_name = oa::session::demo::recording_file_name(
                local_time_text("%Y-%m-%d %H%M"), map, ".oademo"
            );
        } else {
            const std::string extension =
                profile != nullptr ? profile->recorder.ta_demo_recorder.auto_extension : ".tad";
            file_name = oa::session::demo::recording_file_name(
                local_time_text("%Y-%m-%d %H%M"), map, extension
            );
        }
        const auto& record_game = net_options().record_game;
        const auto& net_record = net_options().net_record;
        held->recording_path = recording_file(
            !record_game.empty() ? record_game : net_record,
            runtime_.user_folder(),
            runtime_.files_mod_id(),
            path_from_utf8(file_name)
        );
        held->recording_is_replay = replay_layout;
        if (replay_layout) {
            auto facts = NetHost::replay_facts(runtime_, std::move(player_facts), map, utf8_text);
            if (campaign_match) {
                facts.how = "campaign";
                facts.game_kind = "campaign";
                facts.game_kind_object = campaign_kind_json(
                    mission_facts.campaign_name,
                    mission_facts.mission_number,
                    mission_facts.mission_name
                );
                if (!mission_facts.source_expansion.empty() && std::find(
                                                                   facts.expansions.begin(),
                                                                   facts.expansions.end(),
                                                                   mission_facts.source_expansion
                                                               ) == facts.expansions.end())
                    facts.expansions.push_back(mission_facts.source_expansion);
            } else {
                facts.how = "skirmish";
                facts.game_kind = "skirmish";
                facts.game_kind_object.clear();
            }
            held->recording_start = oa::session::demo::replay_begin(setup, facts);
        }
        std::string problem;
        held->partial = partial_make(
            runtime_.user_folder(),
            runtime_.files_mod_id(),
            held->recording_path.filename(),
            plan.layout,
            &problem
        );
        if (held->partial == nullptr) {
            std::cerr << word << ": the recording could not be started: " << problem << '\n';
            return;
        }
        if (replay_layout && !partial_write_start(held->partial->files, held->recording_start)) {
            std::cerr << word
                      << ": the recording could not be started: its description cannot "
                         "be kept\n";
            return;
        }
        oa::session::demo::recording_begin(&held->recording, std::move(setup));
        if (!oa::session::demo::recording_stream_to(
                &held->recording, held->partial->files.stream, &problem
            )) {
            std::cerr << word << ": the recording could not be started: " << problem << '\n';
            return;
        }
        if (!partial_write_note(held->partial->files, runtime_.content_note_text()))
            std::cerr << word << ": the recording's content note could not be kept\n";
        // Commanders and, in a game loaded from a save, every unit already
        // standing were placed before this bit was set, so they sent nothing.
        // One create each lets a playback build the same world. Later creates
        // go out through the hooks.
        for (uint32_t unit_slot = 1; unit_slot < world->unit_slot_count; ++unit_slot) {
            const Unit& unit = world->units[unit_slot];
            if (unit.type_index == 0)
                continue;
            const auto* owner = world_player_ref(world, unit.owner);
            if (owner == nullptr || !sends_here(*owner))
                continue;
            nm::net_match_send_unit_created(held->net.get(), &unit);
            mark_announced(
                *held, static_cast<uint16_t>(unit_slot), static_cast<uint8_t>(owner->index), true
            );
        }
        nm::packet_layer_flush(held->connection.packets, 0, true);
        print_feature_cells(
            campaign_match ? "campaign features at start" : "skirmish features at start", *world
        );
        guard.keep = true;
        skirmish_ = std::move(held);
    } catch (const std::exception& error) {
        std::cerr << word << ": the recording could not be started: " << error.what() << '\n';
    }
}

bool NetworkPlay::skirmish_packing() const noexcept {
    return skirmish_ != nullptr && skirmish_->finishing != nullptr && skirmish_->finishing->started;
}

void NetworkPlay::finish_skirmish_recording() {
    if (skirmish_ == nullptr) {
        skirmish_decided_ = false;
        return;
    }
    auto& record = *skirmish_;
    try {
        if (record.recording.started) {
            uint32_t length_ticks = 0;
            bool match_finished = false;
            const char* mission_word = nullptr;
            if (runtime_.running_match() != nullptr) {
                length_ticks = runtime_.running_match()->state().game.tick;
                const auto outcome = runtime_.running_match()->outcome();
                match_finished = outcome == oa::sim::scenario::Outcome::victory ||
                                 outcome == oa::sim::scenario::Outcome::defeat;
                // meta.json keeps the recording player's result of a campaign
                // mission. A skirmish and a network game record no such word.
                if (record.campaign && outcome == oa::sim::scenario::Outcome::victory)
                    mission_word = "won";
                else if (record.campaign && outcome == oa::sim::scenario::Outcome::defeat)
                    mission_word = "lost";
            }
            // One last 0x28 per sender, as a network game sends when it ends,
            // while the world and the send path are still there.
            raise_skirmish_live(record);
            send_skirmish_economies(record);
            const char* word = record.campaign ? "campaign" : "skirmish";
            if (record.net != nullptr && record.net->world != nullptr) {
                print_skirmish_players(*record.net->world, word);
                print_feature_cells(
                    record.campaign ? "campaign features" : "skirmish features", *record.net->world
                );
            }
            detach_skirmish(record);
            auto finishing = std::make_unique<RecordingFinishing>();
            finishing->stream = std::move(record.recording.stream);
            record.recording = {};
            finishing->partial = std::move(record.partial);
            if (finishing->partial == nullptr) {
                std::cerr << word << ": the recording could not be finished: it has no files\n";
            } else {
                finishing->finish.files = finishing->partial->files;
                finishing->finish.replay_layout = record.recording_is_replay;
                finishing->finish.start = std::move(record.recording_start);
                finishing->finish.end.ended = match_finished ? "finished" : "left";
                if (mission_word != nullptr)
                    for (oa::formats::oademo::PlayerInfo& recorded :
                         finishing->finish.start.meta.players)
                        if (recorded.recorded_here.value_or(false))
                            recorded.result = mission_word;
                if (length_ticks != 0)
                    finishing->finish.end.length_ticks = length_ticks;
                finishing->finish.destination = record.recording_path;
                finishing->finish.replace = record.recording_named;
                record.recording_start = {};
                RecordingFinishing* running = finishing.get();
                record.finishing = std::move(finishing);
                if (!runtime_.person_plays() ||
                    !oa::base::threads::start_thread(
                        running->thread, &NetHost::run_finishing, running
                    ))
                    NetHost::run_finishing(running);
                else
                    running->started = true;
                if (!running->started)
                    reap_skirmish(record);
            }
        } else if (record.finishing != nullptr) {
            reap_skirmish(record);
        } else {
            // Already closed, or never started. The end and its length stay as written.
            detach_skirmish(record);
            discard_skirmish_partial(record);
        }
    } catch (const std::exception& error) {
        const char* word = record.campaign ? "campaign" : "skirmish";
        std::cerr << word << ": the recording could not be finished: " << error.what() << '\n';
    }
    skirmish_decided_ = false;
}

void NetworkPlay::net_bind_multiplayer() {
    release_match_report();
    if (!net_) {
        net_.reset(new NetState());
        nm::SessionLobbyConfig config{};
        if (net_launch_switches().send_pacing_set != 0)
            config.sends_per_second = net_launch_switches().send_pacing;
        if (net_options().check_host_not_found)
            config.host.clock = host_not_found_clock();
        if (const auto port = net_options().dplay_port) {
            config.host.enum_port = *port;
            config.host.stream_port_first = 0;
            config.host.datagram_port_first = 0;
        }
        if (const auto& guid = net_options().dplay_guid) {
            config.application_named = true;
            std::memcpy(config.application.bytes, guid->data(), guid->size());
        }
        net_->connected = nm::net_connection_create(&net_->connection, config);
        if (!net_->connected)
            std::cerr << "multiplayer: session storage is unavailable\n";
    }
    if (const auto* profile = runtime_.mod_profile();
        profile != nullptr && profile->recorder.ta_demo_recorder.weapon_id_patch)
        std::cerr << "multiplayer: the recorder's wider weapon ids are not supported; it keeps "
                     "protocol "
                  << static_cast<int>(oa::netgame::recorder_protocol_current) << '\n';
    bind_profile_rules();
    if (net_->connected)
        mp::multiplayer_bind_net(nm::session_lobby_net(&net_->connection));
    // The pack maps, and the pack of the host's selected map, which the
    // battle room names in the host's presence record (LobbyMaps::pack).
    const mp::LobbyMapSource maps_source = oa::app::pack_map_source(runtime_);
    mp::multiplayer_bind_map_source(maps_source);
    mp::multiplayer_bind_map_pack(maps_source.context, maps_source.pack);
    if (net_options().check_host_not_found)
        mp::multiplayer_bind_clock({nullptr, [](void*) { return host_not_found_clock_ms(); }});
    mp::multiplayer_bind_player_timeout(net_launch_switches().net_timeout_seconds);
    mp::multiplayer_bind_start(NetHost::on_start, this);
    mp::multiplayer_bind_translation(
        {&runtime_, [](void* context, std::string_view interface_text) {
             return static_cast<Runtime*>(context)->translate_ui(interface_text);
         }}
    );
    // The chat list's measure, so a line about a 3.1c player goes on in a
    // further line where the list would clip it.
    mp::multiplayer_bind_chat_measure(
        {&runtime_,
         [](void* context) { return static_cast<Runtime*>(context)->chat_list_room(); },
         [](void* context, std::string_view line) {
             return static_cast<Runtime*>(context)->chat_list_width(line);
         }}
    );
    // Every battle room is told which engine this machine runs, whether
    // Developer Mode is on, and whether a program on this machine may
    // control the game.
    mp::multiplayer_bind_engine_banner({&runtime_, [](void* context) {
                                            const auto& runtime =
                                                *static_cast<const Runtime*>(context);
                                            return mp::engine_banner_line(
                                                kEngineVersionText,
                                                runtime.developer_mode(),
                                                runtime_options(runtime).remote_controlled
                                            );
                                        }});
}

void NetworkPlay::bind_profile_rules() {
    const auto rules = runtime_wire_rules(runtime_);
    if (net_ && net_->connected)
        nm::net_connection_set_rules(&net_->connection, rules);
    mp::multiplayer_bind_wire_rules(rules, kProgramLine);
    const uint8_t buttons = view_rules::lobby_button_bits(runtime_.ui_rules());
    mp::multiplayer_bind_lobby_buttons(buttons);
    const auto* profile = runtime_.mod_profile();
    mp::multiplayer_bind_rules(profile != nullptr ? &profile->rules : nullptr);
    bound_profile_ = profile;
    bound_sim_hash_ = profile != nullptr ? profile->sim_hash : oa::base::sha256::Digest{};
    bound_lobby_buttons_ = buttons;
}

void NetworkPlay::follow_profile_rules() {
    const auto* profile = runtime_.mod_profile();
    const auto sim_hash = profile != nullptr ? profile->sim_hash : oa::base::sha256::Digest{};
    if (profile == bound_profile_ && sim_hash == bound_sim_hash_ &&
        view_rules::lobby_button_bits(runtime_.ui_rules()) == bound_lobby_buttons_)
        return;
    bind_profile_rules();
}

bool NetworkPlay::unicode_chat() const {
    return runtime_.unicode_chat_on();
}

void NetworkPlay::follow_unicode_chat() {
    const bool on = unicode_chat();
    mp::multiplayer_bind_unicode_chat(on);
    if (net_ && net_->net)
        net_->net->unicode_chat = on;
    if (demo_) {
        demo_->unicode_chat = on;
        demo_->net.unicode_chat = on;
    }
}

void NetworkPlay::follow_presence() {
    runtime_.follow_room_mutators();
    // A change to the mutator field while the screens are hidden, such as
    // the release at a match's start, stays pending until they show again:
    // taking its generation here would skip writing it into the record.
    if (presence_taken_ && !mp::multiplayer_showing())
        return;
    const uint32_t generation = runtime_.room_mutators_generation();
    const bool mutators_changed =
        !presence_mutators_seen_ || generation != presence_mutator_generation_;
    if (mutators_changed) {
        presence_mutators_seen_ = true;
        presence_mutator_generation_ = generation;
        const oa::netgame::PresenceMutators* field = runtime_.room_mutators_presence();
        if (field == nullptr) {
            presence_mutators_held_ = false;
            presence_mutators_ = {};
        } else {
            presence_mutators_ = *field;
            presence_mutators_held_ = true;
        }
        // The full list, even when the bytes below are cut to the same record.
        mp::multiplayer_bind_mutator_loadout(
            presence_mutators_held_ ? &presence_mutators_ : nullptr
        );
    }
    const bool first = !presence_taken_;
    presence_taken_ = true;
    auto facts = presence_facts(runtime_);
    const auto played = played_content(runtime_);
    const auto key = played_content_key(played);
    if (!first && facts == presence_facts_ && key == presence_content_key_ && !mutators_changed)
        return;
    presence_facts_ = std::move(facts);
    presence_content_key_ = key;
    auto record = write_presence_record(
        presence_facts_,
        presence_reported_,
        played,
        presence_mutators_held_ ? &presence_mutators_ : nullptr,
        presence_mutators_held_ ? runtime_.room_mutator_modes() : std::nullopt
    );
    if (!first && record == bound_presence_record_)
        return;
    bound_presence_record_ = std::move(record);
    mp::multiplayer_bind_presence_record(
        bound_presence_record_.data(), bound_presence_record_.size()
    );
}

// Per frame: the load barrier while loading; in the match a pause set
// elsewhere announced, a speed set elsewhere taken into the speed
// preferences and the match clock, the paused-frame pump while the pause bit
// is set, the peer ticks for the lag throttle and the remote-player timeout.
// The Pause key's 0x19 goes out as the key is pressed (the extension's
// pause_changed), and the speed keys' and the GAME slider's as they set the
// speed (speed_changed); an in-game menu sends nothing and holds nothing.
//
// A finished game holds on its outcome until its final economy settles
// (net_final_economy_settled). That hold is not a pause: no 0x19 goes out
// and the match clock keeps stepping it, so it keeps sending its per-tick
// records, and the other players play on. The timeout scan keeps running,
// so a player who stops answering is dropped and no longer holds the gate.
void NetworkPlay::net_frame() {
    // A recording finished off this thread is named in the log as soon as it is in place.
    if (net_)
        NetHost::reap_if_finished(*net_);
    if (!net_ || !net_->active)
        return;
    auto& state = *net_;
    if (state.loading) {
        try {
            if (NetHost::loading_frame(*this))
                NetHost::finish(*this);
        } catch (const std::exception& error) {
            NetHost::abort(*this, error.what());
        }
        return;
    }
    // A shared match goes on beneath the preferences its menu opens.
    if (!runtime_.match_running())
        return;
    auto& game = runtime_.match_->state().game;
    const bool sim_paused = (game.sim_run_flags & nm::run_flag_paused) != 0;
    if (sim_paused != state.pause_seen) {
        state.pause_seen = sim_paused;
        runtime_.status_ = sim_paused ? "Game paused" : "Game resumed";
    }
    // A speed set elsewhere: one another machine sent, or one this machine's
    // preferences put back (Cancel, UNDO, RESTORE), which stays here.
    if (game.requested_speed != state.speed_seen &&
        game.requested_speed >= state.net->rules.speed_min &&
        game.requested_speed <= state.net->rules.speed_max) {
        runtime_.preferences_.current_game_speed = game.requested_speed;
        runtime_.match_timing_.actual_rate = game.requested_speed;
        runtime_.match_timing_.requested_rate = game.requested_speed;
        state.speed_seen = game.requested_speed;
    }
    share_cameras();
    if (sim_paused)
        nm::net_match_paused_frame(state.net.get());
    // ui.whiteboard: the marks drawn here since the last frame go to the
    // other players' recorders, batch by batch.
    for (auto batch = runtime_.take_whiteboard_batch(); !batch.empty();
         batch = runtime_.take_whiteboard_batch())
        (void)nm::net_match_send_whiteboard(state.net.get(), batch.data(), batch.size());
    // A .record typed during the game starts its recording now.
    if (state.net->recorder.record_name[0] != '\0' && !state.recording.started &&
        !state.recording_refused)
        NetHost::start_recording(*this);
    nm::net_match_sync_timing(&runtime_.match_->state(), &runtime_.match_timing_);
    NetHost::watch_timeouts(*this);
    step_reporter_frame();
}

// This machine's camera goes to the match, which sends it with the
// recorder's traffic; the cameras the other machines sent go to the view.
void NetworkPlay::share_cameras() {
    auto& match = *net_->net;
    if (const auto centre = runtime_.camera_centre())
        nm::net_match_note_camera(&match, (*centre)[0], (*centre)[1]);
    std::array<oa::ui::hud::ReportedCamera, OA_PLAYER_COUNT> reported{};
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot)
        reported[slot] = {
            match.recorder.camera_shared[slot],
            match.recorder.camera_x[slot],
            match.recorder.camera_y[slot],
        };
    runtime_.take_reported_cameras(reported);
}

bool NetworkPlay::net_match_active() const {
    return net_ && net_->active;
}

const NetState* NetworkPlay::loading_net_match() const {
    return net_ && net_->active && net_->loading ? net_.get() : nullptr;
}

const NetState* NetworkPlay::session_net_match() const {
    return net_ && net_->active ? net_.get() : nullptr;
}

bool NetworkPlay::net_local_watcher() const {
    return net_ && net_->active && runtime_.match_ &&
           nm::match_slot_watcher(
               &runtime_.match_->state(), runtime_.match_->state().game.local_player_index
           );
}

bool NetworkPlay::net_session_open() const {
    return net_ && net_->connected && net_->connection.in_session;
}

bool NetworkPlay::net_simulation_step() {
    if (!net_ || !net_->active || net_->loading || !runtime_.match_)
        return false;
    if (const auto gap = net_->net->rules.lag_guard_ms; gap != 0) {
        const auto verdict = oa::base::game_loop::lag_guard_step(
            net_->lag_guard, gap, lag_guard_clock_ms(), net_remote_silence_ms()
        );
        if (verdict == oa::base::game_loop::LagGuardStep::held)
            return true;
        if (verdict == oa::base::game_loop::LagGuardStep::closing)
            NetHost::notice(this, "Network gap detected - simulation paused");
        if (verdict == oa::base::game_loop::LagGuardStep::opening)
            NetHost::notice(
                this,
                ("Network resumed after " +
                 std::to_string(lag_guard_clock_ms() - net_->lag_guard.closed_at_ms) + " ms freeze")
                    .c_str()
            );
    }
    nm::match_binding_tick(&net_->binding);
    runtime_.match_timing_.tick = runtime_.match_->state().game.tick;
    return true;
}

uint32_t NetworkPlay::lag_guard_clock_ms() {
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()
    )
                                     .count());
}

uint32_t NetworkPlay::net_remote_silence_ms() const {
    if (!net_ || !net_->active || net_->net->world == nullptr)
        return 0;
    const auto& world = *net_->net->world;
    const auto now = nm::net_connection_time(&net_->connection);
    bool heard = false;
    uint32_t newest = 0;
    for (const auto& player : world.game.players) {
        if (player.in_use == 0 || player.status != OA_PLAYER_STATUS_MIRRORED ||
            player.reject_reason != 0)
            continue;
        const auto silent = now - player.last_update_time;
        if (!heard || silent < newest)
            newest = silent;
        heard = true;
    }
    // Connection time counts 1/30 s.
    constexpr uint32_t ms_per_second = 1000;
    constexpr uint32_t ticks_per_second = 30;
    return heard ? newest * ms_per_second / ticks_per_second : 0;
}

void NetworkPlay::net_pause_key(bool paused) {
    auto& game = net_->net->world->game;
    if (net_lag_guard_closed()) {
        game.sim_run_flags = static_cast<uint16_t>(
            paused ? game.sim_run_flags & ~nm::run_flag_paused
                   : game.sim_run_flags | nm::run_flag_paused
        );
        return;
    }
    nm::net_match_set_pause(net_->net.get(), paused);
    net_->pause_seen = (game.sim_run_flags & nm::run_flag_paused) != 0;
}

bool NetworkPlay::net_lag_guard_closed() const {
    return net_ && net_->active && net_->net->rules.lag_guard_ms != 0 && net_->lag_guard.closed;
}

// A finished network game holds on its frame until the final
// economy is settled, then tears the game down.
bool NetworkPlay::net_final_economy_settled() {
    if (!net_ || !net_->active || net_->loading || !runtime_.match_)
        return true;
    auto& state = *net_;
    (void)nm::net_match_pump(state.net.get());
    state.finish_pending = !nm::net_match_final_economy(state.net.get());
    return !state.finish_pending;
}

// The statistics live with the connection's packet queue; without one
// there is nothing to reset.
void NetworkPlay::net_reset_traffic_stats() {
    if (!net_ || !net_->connected || net_->connection.packets == nullptr)
        return;
    const uint32_t tick = runtime_.match_ ? runtime_.match_->state().game.tick : 0;
    oa::netgame::traffic_stats_reset(&net_->connection.packets->traffic, tick);
}

void NetworkPlay::net_send_player_status() {
    if (net_ && net_->active)
        nm::net_match_send_player_status(net_->net.get(), false);
}

// Leaving sends no record of its own: closing the session destroys the local
// players, and every other machine retires their slots as departed, with no
// chat line.
void NetworkPlay::net_leave() {
    if (!net_ || !net_->active)
        return;
    NetHost::finish_recording(*this);
    auto& state = *net_;
    if (runtime_.match_)
        nm::net_connection_finish(&state.connection, &runtime_.match_->state().game);
    state.active = false;
    state.loading = false;
    state.finish_pending = false;
    state.binding = {};
    *state.net = nm::NetMatch{};
    const auto net = nm::session_lobby_net(&state.connection);
    net.close(net.context);
    mp::multiplayer_reset();
}

// "Give" while a network match runs: net_match_give debits, credits and
// sends the resource record, as the share transfers' network branch does.
// Nothing is given while the match loads.
bool NetworkPlay::net_give(uint8_t from, uint8_t to, bool metal, float amount) {
    if (!net_ || !net_->active)
        return false;
    if (!net_->loading)
        nm::net_match_give(net_->net.get(), from, to, metal, amount);
    return true;
}

// The text message send as the chat line calls it: the formatted line to every player.
void NetworkPlay::net_send_chat(const char* line) {
    if (!net_ || !net_->active || net_->loading || line == nullptr || *line == '\0')
        return;
    nm::net_match_say(net_->net.get(), line);
}

void NetworkPlay::net_send_seat_chat(
    uint8_t seat, const char* line, uint8_t mode, const uint8_t* targets
) {
    if (!net_ || !net_->active || net_->loading || line == nullptr || *line == '\0')
        return;
    nm::net_match_say_from(net_->net.get(), seat, line, mode, targets);
}

const char* NetworkPlay::shared_chat_line(const char* line, std::size_t index) const {
    if (!net_ || !net_->active || net_->loading || line == nullptr)
        return nullptr;
    const auto& parts = net_->net->said_parts;
    // The parts are of this line: its first part starts it.
    if (index >= parts.size() || std::string_view(line).rfind(parts.front(), 0) != 0)
        return nullptr;
    return parts[index].c_str();
}

std::size_t NetworkPlay::shared_chat_line_bytes() const {
    if (!net_ || !net_->active || net_->loading || !net_->net->unicode_chat)
        return 0;
    return oa::netgame::chat_line_parts * oa::netgame::chat_record_text_bytes;
}

void NetworkPlay::net_say(std::string_view text) {
    if (!net_ || !net_->active || net_->loading || !runtime_.match_ || text.empty())
        return;
    const auto& local = runtime_.match_->state().game.players[net_->local_slot];
    std::string line = "<";
    line += field_text(local.name, sizeof local.name);
    line += "> ";
    line += text;
    // Without Unicode chat one record carries the line, cut between whole
    // characters; with it the match cuts the line into its records.
    if (!net_->net->unicode_chat)
        line.resize(oa::base::text::whole_characters(line, sizeof(oa::netgame::ChatRecord::text)));
    nm::net_match_say(net_->net.get(), line.c_str());
    // The chat formatter also reports lines that reach everyone.
    const uint8_t mode = runtime_.match_->state().game.chat_mode;
    if (mode != OA_CHAT_MODE_CHOSEN && mode != OA_CHAT_MODE_ALLIES &&
        mode != oa::sim::messages::chat_mode_local_only)
        report_chat_line(line.c_str());
}

int NetworkPlay::run_net_loopback_check(std::size_t ticks) {
    if (!runtime_options(runtime_).headless_check)
        throw std::runtime_error("--net-loopback-check needs --headless-check");
    std::cout << "net loopback check: hosting and joining in-process over 127.0.0.1\n";
    // The joiner's match writes its own trace files beside the host's.
    auto joiner_options = runtime_options(runtime_);
    for (auto* path : {&joiner_options.trace_digest, &joiner_options.trace_units})
        if (!path->empty())
            *path += ".joiner";
    // Both machines run the same extension table; the check presses Pause
    // through it as the engine's hotkeys do.
    const Extension& extension = runtime_.extension_;
    // The bytes the machines send each other and have not yet read
    // (loopback_configure). It outlives the joiner.
    int64_t in_flight = 0;
    // The joining machine is a whole second runtime, so it lives on the heap
    // beside this one rather than on the thread's stack.
    const auto joiner_runtime =
        std::make_unique<Runtime>(std::move(joiner_options), runtime_.assets_, extension);
    Runtime& joiner = *joiner_runtime;
    NetworkPlay& joiner_play = NetworkPlay::of(joiner);

    // The host services the joiner while it waits, and counts its bytes in
    // flight here (loopback_configure). Both stop before the joiner is
    // destroyed, however the check ends, so the host's later teardown never
    // services a freed machine or counts into this frame.
    struct JoinerUnlink {
        NetworkPlay& host;

        ~JoinerUnlink() {
            if (host.net_) {
                auto& connection = host.net_->connection;
                connection.pump_context = nullptr;
                connection.pump_other = nullptr;
                connection.host_config.clock.in_flight = nullptr;
                if (connection.host != nullptr)
                    connection.host->config.clock.in_flight = nullptr;
            }
        }
    };

    const JoinerUnlink unlink_joiner{*this};
    LoopbackSide host{this, "hoster"};
    LoopbackSide client{&joiner_play, "joiner"};
    const auto require = [](bool ok, const char* what) {
        if (!ok)
            throw std::runtime_error(std::string("net loopback check: ") + what);
    };
    const auto on_screen = [](Runtime& side, Screen screen) {
        const CheckHost side_host = check_host(side);
        return side_host.screen(side_host.context) == screen_id(screen);
    };
    require(
        NetHost::loopback_configure(*this, 0x51, joiner_play.net_.get(), &in_flight) &&
            NetHost::loopback_configure(joiner_play, 0x77, net_.get(), &in_flight),
        "loopback session storage"
    );
    for (auto* side : {&host, &client}) {
        side->net = nm::session_lobby_net(&side->play->net_->connection);
        mp::lobby_reset(*side->lobby, *side->game);
        side->lobby->net = side->net;
        side->lobby->wire_rules = runtime_wire_rules(side->play->runtime_);
        side->lobby->unicode_chat = side->play->unicode_chat();
        // Each machine's presence record, as its battle room would hold it.
        side->play->follow_presence();
        const auto& presence = side->play->bound_presence_record_;
        auto& records = side->lobby->presence_records;
        records.source_size =
            static_cast<uint16_t>(std::min(presence.size(), sizeof records.source));
        std::copy_n(presence.begin(), records.source_size, records.source);
        mp::presence_refresh(*side->lobby);
        side->lobby->local_version_major = side->lobby->wire_rules.version_major;
        side->lobby->local_version_minor = side->lobby->wire_rules.version_minor;
        nm::net_connection_set_rules(&side->play->net_->connection, side->lobby->wire_rules);
        std::snprintf(mp::lobby_nickname(*side->game), 17, "%s", side->nickname);
        std::snprintf(mp::lobby_game_name(*side->game), 17, "%s", "Loopback Game");
    }
    const auto pump_both = [&](uint32_t wait_ms) {
        NetHost::loopback_pump(*this, wait_ms);
        NetHost::loopback_pump(joiner_play, wait_ms);
    };
    // Pumps both machines until done() holds; progress() is the wait's
    // progress figure (LoopbackWait::next_round).
    const auto wait_progressing = [&](auto done, auto progress, const char* what) {
        LoopbackWait wait;
        while (!done()) {
            require(wait.next_round(progress()), what);
            pump_both(2);
        }
    };
    const auto wait_until = [&](auto done, const char* what) {
        wait_progressing(done, [] { return uint64_t{0}; }, what);
    };
    const auto drain = [](LoopbackSide& side) {
        mp::LobbyEvent event{};
        while (side.net.receive(side.net.context, &event))
            (void)mp::lobby_apply_event(*side.lobby, event);
    };

    mp::Provider providers[mp::kMaxProviders]{};
    require(
        host.net.providers(host.net.context, providers, mp::kMaxProviders) >= 1,
        "no TCP/IP provider"
    );
    require(host.net.open(host.net.context, &providers[0], "127.0.0.1"), "host open");
    require(mp::connect_host(*host.lobby, *host.connect), "host session");
    // The host may seat a computer player, which then plays the joiner, or
    // the host while the joiner watches. --net-loopback-each-ai seats one
    // on the joiner as well.
    const bool each_ai = net_options().net_loopback_each_ai;
    const bool computer = net_options().net_loopback_computer || each_ai;
    auto& host_info = mp::local_info(*host.lobby);
    host_info.max_units = computer ? kLoopbackComputerUnitsPerPlayer : kLoopbackUnitsPerPlayer;
    host_info.metal_hundreds = 10;
    host_info.energy_hundreds = 10;
    host_info.side = 0;
    host_info.color = 0;
    const bool small_map = computer && select_map_named(kLoopbackComputerMap);
    std::snprintf(
        host_info.map_name,
        sizeof host_info.map_name,
        "%s",
        small_map ? kLoopbackComputerMap : runtime_.first_map_name_.c_str()
    );
    mp::lobby_publish_session(*host.lobby);

    require(client.net.open(client.net.context, &providers[0], "127.0.0.1"), "client open");
    joiner_play.net_->connection.host->engine.config.enum_port =
        net_->connection.host->enum_port_bound;
    mp::SessionEntry sessions[mp::kMaxSessions]{};
    require(
        client.net.enumerate(client.net.context, sessions, mp::kMaxSessions) >= 1,
        "session enumeration"
    );
    client.connect->chosen = sessions[0];
    require(mp::connect_join(*client.lobby, *client.connect, false), "client join");
    const auto host_id = net_->connection.local_id;
    const auto client_id = joiner_play.net_->connection.local_id;
    wait_until(
        [&] {
            drain(host);
            drain(client);
            return mp::slot_for_player_id(*host.lobby, client_id) >= 0 &&
                   mp::slot_for_player_id(*client.lobby, host_id) >= 0;
        },
        "rosters did not meet"
    );
    const bool watching = net_options().net_loopback_watcher;
    auto& client_info = mp::local_info(*client.lobby);
    client_info.side = 1;
    client_info.color = 1;
    if (watching)
        client_info.options |= OA_SETUP_OPTION_WATCHER;
    // A hosted game starts with watching disallowed; the host's WATCHING
    // button allows it, for the watcher and for a defeated player who keeps
    // watching. Its CHEATING button opens cheats to both players; the
    // watcher's game leaves them closed.
    require(
        (host_info.options & mp::option::watching_allowed) == 0,
        "the hosted game started with watching allowed"
    );
    host_info.options ^= mp::option::watching_allowed;
    if (!watching)
        host_info.options ^= mp::option::cheats_allowed;
    // --net-loopback-fixed-starts: the host's block asks for fixed starts.
    if (net_options().net_loopback_fixed_starts)
        host_info.options |= mp::option::fixed_locations;
    mp::lobby_send_player_info(*host.lobby);
    mp::lobby_send_player_info(*client.lobby);
    wait_until(
        [&] {
            drain(host);
            drain(client);
            const auto host_slot = mp::slot_for_player_id(*client.lobby, host_id);
            const auto client_slot = mp::slot_for_player_id(*host.lobby, client_id);
            return host_slot >= 0 && client_slot >= 0 &&
                   (mp::slot_info(*client.lobby, host_slot)->role & mp::kRoleHost) != 0 &&
                   mp::slot_info(*client.lobby, host_slot)->max_units == host_info.max_units &&
                   mp::slot_info(*client.lobby, host_slot)->options == host_info.options &&
                   mp::slot_info(*host.lobby, client_slot)->side == 1;
        },
        "player info did not arrive"
    );
    // Each machine sent the other its presence record, alone in its frame,
    // as the other's block arrived with the 'OA' signature, and keeps the
    // one the other sent.
    const auto presence_of = [](LoopbackSide& side, uint32_t player_id) {
        return mp::lobby_presence_record(
            *side.lobby, mp::slot_for_player_id(*side.lobby, player_id)
        );
    };
    wait_until(
        [&] {
            drain(host);
            drain(client);
            return !presence_of(host, client_id).empty() && !presence_of(client, host_id).empty();
        },
        "a presence record did not arrive"
    );
    const auto presence_engine = presence_engine_text();
    for (const auto record : {presence_of(host, client_id), presence_of(client, host_id)}) {
        oa::netgame::PresenceRecord decoded;
        require(
            oa::netgame::decode_presence(record.data(), record.size(), &decoded) ==
                    oa::netgame::WireError::ok &&
                decoded.engine && decoded.engine->version == presence_engine,
            "a presence record did not name this build's engine"
        );
        require(
            decoded.limits &&
                decoded.limits->weapon_types == oa::data::limits::highest_weapon_types &&
                decoded.limits->sides == static_cast<uint8_t>(oa::data::limits::highest_sides),
            "a presence record did not name this build's limits"
        );
    }
    require(mp::lobby_limits_allow_start(*host.lobby), "the host's room held START for limits");
    std::cout << "net loopback check: limits allow START\n";
    std::cout << "net loopback check: presence records exchanged\n";
    // The host seats a computer player in the first open slot, as a click on
    // a closed slot does: a player of the host's session, playing Core. It
    // joins the host's team, unless the joiner only watches: then it plays
    // the host.
    uint32_t computer_id = nm::no_player_id;
    if (computer) {
        int32_t seat = -1;
        for (int32_t slot = 0; slot < mp::kSlotCount && seat < 0; ++slot) {
            const auto& player = mp::slot_player(*host.lobby, slot);
            if (player.in_use == 0 && player.status == mp::kSlotOpen)
                seat = slot;
        }
        require(seat >= 0, "no open slot for the computer player");
        mp::lobby_add_computer(*host.lobby, seat);
        auto* seat_info = mp::slot_info(*host.lobby, seat);
        require(seat_info != nullptr, "the computer player has no setup block");
        seat_info->color = static_cast<uint8_t>(mp::lobby_next_free_color(*host.lobby));
        seat_info->side = 1;
        computer_id = mp::slot_player(*host.lobby, seat).player_id;
        mp::lobby_send_player_info(*host.lobby);
        wait_until(
            [&] {
                drain(host);
                drain(client);
                return mp::slot_for_player_id(*host.lobby, computer_id) == seat &&
                       mp::slot_for_player_id(*client.lobby, computer_id) >= 0;
            },
            "the joiner did not seat the host's computer player"
        );
        if (!watching) {
            for (const auto slot :
                 {static_cast<int32_t>(host.lobby->game->local_player_index), seat}) {
                auto& player = mp::slot_player(*host.lobby, slot);
                mp::lobby_player_team(player) = 0;
                mp::lobby_send_team(*host.lobby, player);
            }
        }
        // The periodic block of the host's battle room sends every player's
        // blocks again, which lets through a frame of the computer player
        // that the joiner holds while the host's player numbers the shared
        // broadcast channel in between.
        const uint8_t team = watching ? mp::kNoTeam : 0;
        uint32_t rounds = 0;
        wait_until(
            [&] {
                if (++rounds % 50 == 0)
                    mp::lobby_send_player_info(*host.lobby);
                drain(host);
                drain(client);
                const auto seen = mp::slot_for_player_id(*client.lobby, computer_id);
                const auto host_seen = mp::slot_for_player_id(*client.lobby, host_id);
                return seen >= 0 && host_seen >= 0 &&
                       mp::slot_info(*client.lobby, seen)->color == seat_info->color &&
                       mp::lobby_player_team(mp::slot_player(*client.lobby, seen)) == team &&
                       mp::slot_player(*client.lobby, seen).machine_group != 0 &&
                       mp::slot_player(*client.lobby, seen).machine_group ==
                           mp::slot_player(*client.lobby, host_seen).machine_group;
            },
            "the computer player's block, team or machine group did not reach the joiner"
        );
        // Each battle room's refresh settles the alliances from the teams.
        for (auto* side : {&host, &client})
            mp::lobby_update_ally_matrix(*side->lobby);
        std::cout << "net loopback check: the host seated computer player "
                  << field_text(mp::slot_player(*host.lobby, seat).name, sizeof(Player::name))
                  << " in slot " << seat << (watching ? " against itself" : " on its team")
                  << ", on " << host_info.map_name << "\n";
    }
    uint32_t joiner_computer_id = nm::no_player_id;
    if (each_ai) {
        int32_t joiner_seat = -1;
        for (int32_t slot = 0; slot < mp::kSlotCount && joiner_seat < 0; ++slot) {
            const auto& player = mp::slot_player(*client.lobby, slot);
            if (player.in_use == 0 && player.status == mp::kSlotOpen)
                joiner_seat = slot;
        }
        require(joiner_seat >= 0, "no open slot for the joiner's computer player");
        mp::lobby_add_computer(*client.lobby, joiner_seat);
        auto* joiner_info = mp::slot_info(*client.lobby, joiner_seat);
        require(joiner_info != nullptr, "the joiner's computer player has no setup block");
        joiner_info->color = static_cast<uint8_t>(mp::lobby_next_free_color(*client.lobby));
        joiner_info->side = 0;
        joiner_computer_id = mp::slot_player(*client.lobby, joiner_seat).player_id;
        mp::lobby_send_player_info(*client.lobby);
        uint32_t group_rounds = 0;
        wait_until(
            [&] {
                if (++group_rounds % 50 == 0)
                    mp::lobby_send_player_info(*client.lobby);
                drain(host);
                drain(client);
                const auto seen = mp::slot_for_player_id(*host.lobby, joiner_computer_id);
                const auto client_seen = mp::slot_for_player_id(*host.lobby, client_id);
                return seen >= 0 && client_seen >= 0 &&
                       mp::slot_player(*host.lobby, seen).machine_group != 0 &&
                       mp::slot_player(*host.lobby, seen).machine_group ==
                           mp::slot_player(*host.lobby, client_seen).machine_group;
            },
            "the joiner's computer player did not join its machine"
        );
        // Allied with everyone before the match copies the rows, so the
        // strike still finds the joiner's commander.
        const auto ally_lobby = [](mp::Lobby& lobby, uint32_t id) {
            const auto slot = mp::slot_for_player_id(lobby, id);
            if (slot < 0)
                return;
            auto& player = mp::slot_player(lobby, slot);
            for (int32_t other = 0; other < mp::kSlotCount; ++other) {
                if (other == slot)
                    continue;
                auto& mate = mp::slot_player(lobby, other);
                if (mate.in_use == 0)
                    continue;
                player.alliance[other] = 1;
                mate.alliance[slot] = 1;
            }
        };
        ally_lobby(*host.lobby, joiner_computer_id);
        ally_lobby(*client.lobby, joiner_computer_id);
        std::cout << "net loopback check: the joiner seated computer player "
                  << field_text(
                         mp::slot_player(*client.lobby, joiner_seat).name, sizeof(Player::name)
                     )
                  << " in slot " << joiner_seat << "\n";
    }

    // The battle room's unit sync. With a chosen list, the shared mutator
    // layer is mounted first and the headers are read over it; the layer
    // comes off before the launch, which mounts the same bytes again. The
    // check creates no battle-room state, so the launch still plays the
    // run's own list. The client's copy of one unit carries a content
    // checksum from other files, so the host's verdict takes that unit away
    // in both battle rooms and its restriction row greys out; every other
    // unit stays. The verdicts stay with each battle room until its launch
    // applies them. The cached headers stay as the layer served them: the
    // recording's unit table reads that cache, so they are not read again
    // after the layer comes off.
    uint32_t taken_away_key = 0;
    int32_t synced_units = 0;
    {
        std::string layer_failure;
        const bool layered = runtime_.mount_chosen_mutator_layer(layer_failure);
        require(layer_failure.empty(), layer_failure.c_str());
        if (layered) {
            mp::multiplayer_units_changed();
            std::cout << "net loopback check: unit sync over the mutators' layer\n";
        }
        int32_t unit_count = 0;
        const CheckHost host_check = check_host(runtime_);
        mp::LobbyUnit* host_units =
            mp::multiplayer_units(*host_check.assets(host_check.context), &unit_count);
        require(unit_count > 100, "the install has no unit table");
        std::vector<mp::LobbyUnit> client_units(host_units, host_units + unit_count);
        int32_t altered = 1;
        while (altered < unit_count && (client_units[altered].abilities & mp::kUnitNoRestrict) != 0)
            ++altered;
        require(altered + 1 < unit_count, "no restrictable unit to alter");
        const int32_t kept = altered + 1;
        client_units[altered].content_checksum =
            mp::multiplayer_unit_checksum(nullptr, host_units[altered]) ^ 1u;
        host.lobby->units = host_units;
        client.lobby->units = client_units.data();
        for (auto* side : {&host, &client}) {
            side->lobby->unit_count = unit_count;
            side->lobby->services.unit_checksum = mp::multiplayer_unit_checksum;
        }
        mp::unit_sync_create(*host.lobby, true);
        mp::unit_sync_create(*client.lobby, false);
        // The sync's progress: the records the joiner handled, and the unit
        // count, checksums and acknowledgements the host has from it. The
        // joiner works out and sends four checksums a round, so a loaded
        // machine takes longer over them but keeps showing progress.
        const auto sync_progress = [&] {
            uint64_t figure = client.lobby->sync.records_handled;
            const auto& host_sync = host.lobby->sync;
            for (int32_t index = 0; index < host_sync.peer_count; ++index) {
                const auto& peer = host_sync.peers[index];
                figure +=
                    uint64_t{peer.received} + peer.acknowledged + (peer.expected != 0 ? 1 : 0);
            }
            return figure;
        };
        wait_progressing(
            [&] {
                mp::unit_sync_tick(*host.lobby);
                mp::unit_sync_tick(*client.lobby);
                drain(host);
                drain(client);
                return mp::unit_sync_complete(*host.lobby);
            },
            sync_progress,
            "the unit sync did not finish"
        );
        mp::UnitSyncRecord record{};
        const auto shared = [&](LoopbackSide& side, int32_t type) {
            return mp::unit_sync_lookup(*side.lobby, host_units[type].fbi_hash, &record);
        };
        require(
            !shared(host, altered) && !shared(client, altered) && shared(host, kept) &&
                shared(client, kept),
            "the unit with other files was not taken away, or another unit was"
        );
        auto restrict = std::make_unique<mp::RestrictPanel>();
        mp::Panel panel;
        mp::restrict_open(*client.lobby, * restrict, panel);
        const auto row_flags = [&](int32_t type) {
            for (int32_t row = 0; row < restrict->count; ++row)
                if (restrict->entries[row].unit == type)
                    return static_cast<int32_t>(restrict->flags[row]);
            return -1;
        };
        require(
            row_flags(altered) >= 0 && (row_flags(altered) & mp::kRestrictRowUnavailable) != 0 &&
                row_flags(kept) >= 0 && (row_flags(kept) & mp::kRestrictRowUnavailable) == 0,
            "the client's restriction row for the unit with other files is not greyed"
        );
        std::cout << "net loopback check: unit sync over " << unit_count - 1 << " units took "
                  << host_units[altered].name << " away\n";
        taken_away_key = host_units[altered].fbi_hash;
        synced_units = unit_count - 1;
        for (auto* side : {&host, &client}) {
            side->lobby->units = nullptr;
            side->lobby->unit_count = 0;
        }
        if (layered) {
            require(
                runtime_.release_chosen_mutator_layer(),
                "the mutators' layer could not be released before launch"
            );
        }
    }

    // Each machine left its single-player speed elsewhere; the match start
    // (through its timing reset) runs a network game at the normal
    // speed and keeps it in the speed preferences.
    constexpr uint16_t kHostLeftSpeed = 4;
    constexpr uint16_t kClientLeftSpeed = 17;
    // Writes a machine's speed into its speed preferences, as the
    // preferences' Cancel, UNDO and RESTORE write back a speed.
    const auto hold_speed = [](Runtime& side, uint16_t speed) {
        side.preferences_.game_speed = side.preferences_.current_game_speed = speed;
    };
    hold_speed(runtime_, kHostLeftSpeed);
    hold_speed(joiner, kClientLeftSpeed);

    // The host's battle room sends its slot table every few seconds, and the
    // joiner launches with it, as from a room that has been open that long.
    mp::lobby_send_slot_table(*host.lobby);
    wait_until(
        [&] {
            drain(host);
            drain(client);
            return std::equal(
                std::begin(host.lobby->game->slot_table),
                std::end(host.lobby->game->slot_table),
                std::begin(client.lobby->game->slot_table)
            );
        },
        "the host's slot table did not reach the joiner"
    );

    // START: the host launches and its 0x08 launches the client. No frame
    // runs while the host builds its world; the loading screen's work goes on
    // through the engine's load progress hook, which sends the 0x08, a probe
    // and the host's rising load progress (0x2a) to the joiner's battle room.
    const auto joiner_received = [&](oa::netgame::RecordType type) {
        return joiner_play.net_->connection.packets->traffic
            .record_count[static_cast<uint8_t>(type)]
                         [static_cast<uint8_t>(oa::netgame::TrafficChannel::received)];
    };
    const auto probes_before_build = joiner_received(oa::netgame::RecordType::probe);
    std::vector<uint8_t> build_progress;
    NetHost::launch(*this, *host.lobby);
    require(net_->active && net_->loading, "host launch");
    wait_until(
        [&] {
            mp::LobbyEvent event{};
            while (client.net.receive(client.net.context, &event)) {
                if (event.kind == mp::LobbyEventKind::record && event.size >= 2 &&
                    event.data[0] == static_cast<uint8_t>(oa::netgame::RecordType::load_progress))
                    build_progress.push_back(event.data[1]);
                (void)mp::lobby_apply_event(*client.lobby, event);
            }
            return (mp::local_info(*client.lobby).options & mp::option::started) != 0;
        },
        "start record did not reach the client"
    );
    require(
        joiner_received(oa::netgame::RecordType::probe) > probes_before_build &&
            !build_progress.empty() && build_progress.front() < nm::load_progress_complete,
        "the host's build sent no probe or load progress to the joiner"
    );
    NetHost::launch(joiner_play, *client.lobby);
    require(joiner_play.net_->active && joiner_play.net_->loading, "client launch");
    // The load barrier and the match entry run through the per-frame path.
    wait_until(
        [&] {
            net_frame();
            joiner_play.net_frame();
            return !net_->loading && !joiner_play.net_->loading;
        },
        "load barrier did not pass"
    );
    require(
        net_->active && joiner_play.net_->active && on_screen(runtime_, Screen::match) &&
            on_screen(joiner, Screen::match),
        "match entry"
    );
    // Joining waits for no match report: each machine's report starts as
    // its match launches.
    require(
        net_->report_started && joiner_play.net_->report_started,
        "a machine's score report did not start as its match launched"
    );
    std::cout << "net loopback check: the host's build sent the joiner load progress";
    for (const auto percent : build_progress)
        std::cout << ' ' << static_cast<int>(percent);
    std::cout << '\n';
    // Each launch applied its battle room's verdicts to the unit table it
    // loaded and dropped them: neither match has the unit taken away, and
    // every other header the store serves now is in the match, by name.
    const CheckHost launched_check = check_host(runtime_);
    const std::vector<ServedHeader> served =
        served_unit_headers(*launched_check.assets(launched_check.context));
    for (const auto* side : {&host, &client}) {
        const auto& world = *side->play->net_->net->world;
        bool kept = false;
        for (uint32_t type = 1; type < world.unit_def_count; ++type)
            kept = kept || world.unit_defs[type].fbi_hash == taken_away_key;
        std::string absent;
        for (const ServedHeader& header : served) {
            if (header.hash == taken_away_key)
                continue;
            bool found = false;
            for (uint32_t slot = 1; slot < world.unit_def_count; ++slot) {
                if (world.unit_defs[slot].fbi_hash == header.hash) {
                    found = true;
                    break;
                }
            }
            if (!found) {
                absent = header.name;
                break;
            }
        }
        const std::string reason =
            absent.empty() ? std::string("a launch did not apply its battle room's unit verdicts")
                           : absent + " is absent from the match";
        require(
            !kept && absent.empty() && static_cast<int32_t>(world.unit_def_count) == synced_units &&
                side->lobby->sync.record_count == 0,
            reason.c_str()
        );
    }
    for (const Runtime* side :
         {static_cast<const Runtime*>(&runtime_), static_cast<const Runtime*>(&joiner)}) {
        const auto& timing = side->match_timing_;
        const auto& game = side->match_->state().game;
        require(
            timing.requested_rate == oa::base::game_loop::normal_game_speed &&
                timing.actual_rate == oa::base::game_loop::normal_game_speed &&
                game.requested_speed == oa::base::game_loop::normal_game_speed &&
                game.current_speed == oa::base::game_loop::normal_game_speed &&
                side->preferences_.game_speed == oa::base::game_loop::normal_game_speed &&
                side->preferences_.current_game_speed == oa::base::game_loop::normal_game_speed,
            "a machine did not start the network game at the normal speed"
        );
    }
    const auto& host_world = runtime_.match_->state();
    const auto& client_world = joiner.match_->state();
    // The match the binding holds is the host's. The shooter is made and
    // ordered through it, on the world this check already reads.
    auto& host_match = *net_->binding.match;
    // The setup blocks each machine holds of the other machine's players, a
    // computer player's among them, came from that machine and carry the
    // five bytes after the engine signature as zero, as 0.7 sent them:
    // presence travels only in the presence record. Counted are the remote
    // blocks seen, and those whose five bytes are all zero.
    int32_t remote_blocks = 0;
    int32_t blocks_without_presence = 0;
    int32_t blocks_with_recorder = 0;
    for (const auto* world : {&host_world, &client_world})
        for (const auto& player : world->game.players) {
            if (player.in_use == 0 || player.status != OA_PLAYER_STATUS_MIRRORED)
                continue;
            const auto* info = world_player_info(world, &player);
            if (info == nullptr)
                continue;
            ++remote_blocks;
            if (info->recorder_protocol != oa::netgame::recorder_protocol_plain)
                ++blocks_with_recorder;
            if (std::all_of(
                    std::begin(info->reserved_after_signature),
                    std::end(info->reserved_after_signature),
                    [](uint8_t at) { return at == 0; }
                ))
                ++blocks_without_presence;
        }
    std::cout << "net loopback check: setup blocks carry no presence bytes: "
              << blocks_without_presence << " / " << remote_blocks << '\n';
    // A game recorded for the player's own sake presents no recorder: the
    // blocks say what the profile says, whether the game is recorded or not.
    std::cout << "net loopback check: setup blocks presenting a recorder: " << blocks_with_recorder
              << " / " << remote_blocks << '\n';
    require(remote_blocks > 0, "no remote setup block reached either machine");
    require(
        blocks_without_presence == remote_blocks,
        "a remote setup block carried bytes after the engine signature"
    );
    if (computer) {
        NetHost::computer_match(
            *this, joiner_play, ticks, computer_id, watching, joiner_computer_id, in_flight
        );
        return 0;
    }
    const auto host_commander = NetHost::local_commander(*this);
    require(host_commander != 0, "host commander");
    const auto commander_z = host_world.units[host_commander].position.z;
    const auto speed_before = runtime_.preferences_.current_game_speed;
    // The host commander's pose at each host tick; the client's copy is
    // compared with the pose of the last record it applied. Slot orders
    // differ between machines, so the client's host slot is found by id.
    const auto client_host_slot = static_cast<std::size_t>(
        std::find_if(
            std::begin(client_world.game.players),
            std::end(client_world.game.players),
            [host_id](const Player& player) {
                return player.in_use != 0 && player.player_id == host_id;
            }
        ) -
        std::begin(client_world.game.players)
    );
    require(client_host_slot < OA_PLAYER_COUNT, "the client has no slot for the host");
    require(
        client_world.game.players[client_host_slot].load_progress == nm::load_progress_complete,
        "the host's load progress did not reach 100 at the joiner"
    );
    // The setup blocks each machine holds say UTF-8 chat only while Unicode
    // chat is on; with it off they stay as 3.1c sends them.
    const auto utf8_blocks = [](const World& world) {
        int32_t count = 0;
        for (const auto& player : world.game.players)
            if (const auto* info =
                    player.in_use != 0 ? world_player_info(&world, &player) : nullptr;
                info != nullptr &&
                oa::netgame::announces_unicode_chat(reinterpret_cast<const uint8_t*>(info)))
                ++count;
        return count;
    };
    std::cout << "net loopback check: Unicode chat " << (unicode_chat() ? "on" : "off")
              << ", setup blocks saying UTF-8 " << utf8_blocks(host_world) << " / "
              << utf8_blocks(client_world) << '\n';
    require(
        unicode_chat() || (utf8_blocks(host_world) == 0 && utf8_blocks(client_world) == 0),
        "a setup block said UTF-8 chat with Unicode chat off"
    );
    std::vector<UnitPose> host_poses;
    uint32_t copy_compared = 0;
    uint32_t copy_divergence = 0;
    // The host's 0x09 for its commander goes out two loading frames after
    // its barrier passed, once the joiner has passed its own and takes it:
    // the joiner's copy stands on the host's start position from that 0x09,
    // so it is on the host's path from the first unit state record it
    // applies. The copy is compared from then on.
    uint32_t copy_placed_tick = 0;
    uint32_t first_applied_tick = 0; // host tick of the first record the client applied
    const auto note_host_pose = [&] {
        const auto tick = host_world.game.tick;
        if (host_poses.size() <= tick)
            host_poses.resize(tick + 1);
        host_poses[tick] = NetHost::unit_pose(*this, host_commander);
    };
    const auto compare_copy = [&] {
        const auto tick = client_world.game.players[client_host_slot].last_sim_tick;
        if (tick <= 0 || static_cast<std::size_t>(tick) >= host_poses.size())
            return;
        if (first_applied_tick == 0)
            first_applied_tick = static_cast<uint32_t>(tick);
        if (joiner.match_->ground_runtime(host_commander) == nullptr)
            return;
        const auto& owner = host_poses[static_cast<std::size_t>(tick)];
        const auto divergence =
            pose_divergence(owner, NetHost::unit_pose(joiner_play, host_commander));
        if (copy_placed_tick == 0) {
            if (divergence != 0)
                return;
            copy_placed_tick = static_cast<uint32_t>(tick);
        }
        ++copy_compared;
        copy_divergence = std::max(copy_divergence, divergence);
    };

    // Every shot the host commander fires: each machine's records for them,
    // the client's arriving as 0x0d. The first of each must leave from the
    // same point for the same point.
    struct Shots {
        uint32_t count{};
        FixedVec3 origin{};
        FixedVec3 target{};
    };

    Shots host_shots{};
    Shots client_shots{};
    Shots shooter_host{};
    Shots shooter_client{};
    uint16_t shooter_slot = 0;
    const std::string& shooter_name = net_options().net_loopback_shooter;
    const auto host_commander_ref = oa::oa_unit_ref_from_slot(host_commander);
    const auto count_from =
        [](const World& world, decltype(host_commander_ref) source, Shots& shots) {
            const int32_t projectile_count = world.game.projectile_count;
            const auto live = std::clamp<int32_t>(projectile_count, 0, OA_PROJECTILE_CAPACITY);
            for (int32_t i = 0; i < live; ++i) {
                const auto& shot = world.projectiles[i];
                if (shot.source != source || shot.created_tick != world.game.tick)
                    continue;
                if (shots.count++ == 0) {
                    shots.origin = shot.origin;
                    shots.target = shot.target;
                }
            }
        };
    // The shooter's new shots are counted with the commander's, on the
    // machine whose world was passed.
    const auto count_shots = [&](const World& world, Shots& shots) {
        count_from(world, host_commander_ref, shots);
        if (shooter_slot == 0)
            return;
        auto& shooter_shots = &world == &host_world ? shooter_host : shooter_client;
        count_from(world, oa::oa_unit_ref_from_slot(shooter_slot), shooter_shots);
    };
    const auto same_point = [](const FixedVec3& a, const FixedVec3& b) {
        return a.x == b.x && a.y == b.y && a.z == b.z;
    };

    // Orders, chat, a speed change, the menu, a pause and a volley at the
    // ground from the host; the client follows each through the records.
    // Typed text reaches everyone, an option command's echo stays on the host
    // (the chat line's local-only mode).
    const auto type_line = [&](const char* text) {
        runtime_.chat_buffer_ = text;
        runtime_.submit_chat_line();
    };
    // The Pause key as the engine's hotkeys run it: the pause bit flips, then
    // the extension hears of it.
    auto& host_game = net_->net->world->game;
    const auto press_pause = [&] {
        host_game.sim_run_flags =
            static_cast<uint16_t>(host_game.sim_run_flags ^ nm::run_flag_paused);
        extension.pause_changed(
            extension.context, runtime_, (host_game.sim_run_flags & nm::run_flag_paused) != 0
        );
    };
    const auto pauses_received = [&] {
        const auto& traffic = joiner_play.net_->connection.packets->traffic;
        return traffic.record_count[static_cast<uint8_t>(oa::netgame::RecordType::pause_speed)]
                                   [static_cast<uint8_t>(oa::netgame::TrafficChannel::received)];
    };
    const auto both_paused = [&] {
        return (host_world.game.sim_run_flags & nm::run_flag_paused) != 0 &&
               (client_world.game.sim_run_flags & nm::run_flag_paused) != 0;
    };
    // The speed keys as the engine's hotkeys run them on the host: the
    // engine reports each speed they set to the extension, which sends it.
    const auto raise_host_speed = [&] { runtime_.adjust_game_speed(1); };
    // A machine's preferences put a speed back straight into its match, as
    // their Cancel and UNDO put back the speed they opened with and RESTORE
    // the normal speed; the engine reports none of them, and the other
    // machine hears nothing of it.
    const auto put_back_speed = [&](Runtime& side, uint16_t speed) {
        hold_speed(side, speed);
        auto& side_game = NetworkPlay::of(side).net_->net->world->game;
        side_game.requested_speed = side_game.current_speed = speed;
    };
    const auto host_speeds_received = [&] {
        const auto& traffic = net_->connection.packets->traffic;
        return traffic.record_count[static_cast<uint8_t>(oa::netgame::RecordType::pause_speed)]
                                   [static_cast<uint8_t>(oa::netgame::TrafficChannel::received)];
    };
    uint32_t host_speeds_before_cancel = 0;
    uint16_t host_speed_before_cancel = 0;
    uint32_t joiner_speeds_before_put_back = 0;
    uint16_t joiner_speed_before_put_back = 0;
    // Remembers what the joiner has heard as the host's preferences put
    // back a speed, and puts it back.
    const auto host_puts_back = [&](uint16_t speed) {
        joiner_speeds_before_put_back = pauses_received();
        joiner_speed_before_put_back = client_world.game.requested_speed;
        put_back_speed(runtime_, speed);
    };
    // Checks that the joiner heard nothing of the speed the host put back.
    const auto require_put_back_kept = [&](const char* what, uint16_t speed) {
        const std::string failure =
            std::string("the host's ") + what + " sent the speed it put back";
        require(
            pauses_received() == joiner_speeds_before_put_back &&
                client_world.game.requested_speed == joiner_speed_before_put_back &&
                host_world.game.requested_speed == speed &&
                runtime_.preferences_.current_game_speed == speed,
            failure.c_str()
        );
        std::cout << "net loopback check: the host's " << what << " put back speed " << speed
                  << " and sent nothing; the joiner kept " << joiner_speed_before_put_back << "\n";
    };
    // The host's player turns its commander's build page with the
    // next-page key, a choice the host keeps to itself: the joiner's copy of
    // the commander keeps the page it was created with, and the worlds
    // agree as they would without it.
    const CheckHost host_keys = check_host(runtime_);
    const auto press_host_key = [&](SDL_Keycode code) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.windowID = host_keys.window_id(host_keys.context);
        event.key.key = code;
        event.key.down = true;
        require(host_keys.dispatch(host_keys.context, &event), "a key ended the host's run");
    };
    // The host's window losing or gaining the focus, as the window system
    // sends it.
    const auto host_window_event = [&](SDL_EventType type) {
        SDL_Event event{};
        event.type = type;
        event.window.windowID = host_keys.window_id(host_keys.context);
        require(
            host_keys.dispatch(host_keys.context, &event), "a window event ended the host's run"
        );
    };
    // The build page a world's copy of the host commander shows; 0 for its
    // order page.
    const auto commander_page = [host_commander](const World& world) {
        const auto flags = world.units[host_commander].flags;
        return (flags & OA_UNIT_FLAG_BUILD_MENU) != 0 ? oa::ui::hud::build_page(flags) : 0u;
    };
    uint32_t host_page_before = 0;
    uint32_t host_page_turned = 0;
    uint32_t joiner_page_before = 0;
    uint32_t pauses_before_menu = 0;
    uint32_t client_tick_before_menu = 0;
    uint32_t held_host_tick = 0;
    uint32_t held_client_tick = 0;
    uint32_t pauses_before_focus = 0;
    uint32_t host_tick_before_focus = 0;
    uint32_t client_tick_before_focus = 0;
    uint32_t netstats_tick = 0;
    // One exchange of both machines: each steps, the host commander's pose is
    // kept, and both machines' new shots are counted.
    const auto exchange = [&] {
        net_frame();
        joiner_play.net_frame();
        NetHost::loopback_step(*this);
        note_host_pose();
        count_shots(host_world, host_shots);
        NetHost::loopback_step(joiner_play);
        compare_copy();
        count_shots(client_world, client_shots);
        pump_both(kTickPumpWaitMs);
    };
    // The host makes the named unit beside its commander and plays until the
    // joiner holds that copy. Its shots are counted from here on.
    if (!shooter_name.empty()) {
        const auto shooter_type = unit_type_named(host_world, shooter_name);
        require(shooter_type != 0, "the loopback shooter type is not in the match");
        shooter_slot = make_unit(
            host_match,
            host_match.state(),
            host_world.units[host_commander].owner_index,
            shooter_type,
            host_commander,
            kLoopbackShooterAside
        );
        require(shooter_slot != 0, "the host could not make the loopback shooter");
        const auto copy_limit = 3u * host_world.game.units_per_player;
        bool copy_held = unit_live(host_world.units[shooter_slot]) &&
                         unit_live(client_world.units[shooter_slot]);
        for (uint32_t waited = 0; waited < copy_limit && !copy_held; ++waited) {
            exchange();
            copy_held = unit_live(host_world.units[shooter_slot]) &&
                        unit_live(client_world.units[shooter_slot]);
        }
        require(copy_held, "the joiner has no copy of the loopback shooter");
    }
    for (std::size_t tick = 0; tick < ticks; ++tick) {
        if (tick == 30) {
            const auto& unit = host_world.units[host_commander];
            runtime_.match_->issue_ground_move(
                host_commander,
                {unit.position.x, unit.position.y, unit.position.z + kLoopbackMoveDistance},
                false
            );
        }
        if (tick == kLoopbackPageTurn) {
            const auto& commander = host_world.units[host_commander];
            host_page_before = commander_page(host_world);
            joiner_page_before = commander_page(client_world);
            // The page after the key: the next one among the type's pages, or
            // the order page after the last.
            const auto turned = oa::ui::hud::build_menu_forward(
                commander.flags, host_world.unit_defs[commander.type_index].gui_page_count, true
            );
            const auto expected_page =
                (turned & OA_UNIT_FLAG_BUILD_MENU) != 0 ? oa::ui::hud::build_page(turned) : 0u;
            // Space selects the commander when nothing is selected, and the
            // commander is all the host's player has.
            press_host_key(SDLK_SPACE);
            press_host_key(SDLK_PERIOD);
            host_page_turned = commander_page(host_world);
            require(
                host_page_before != 0 && host_page_turned == expected_page,
                "the host's next-page key did not turn its commander's build page"
            );
        }
        if (tick == 60)
            net_say("gg");
        if (tick == 61)
            type_line("glhf");
        if (tick == 62 || tick == 63)
            type_line("+clock");
        if (tick == 64)
            type_line("+bps");
        if (tick == 65) {
            type_line("+netstats");
            netstats_tick = host_world.game.tick;
        }
        // A speed key on the host reaches the joiner.
        if (tick == 100)
            raise_host_speed();
        // Photo mode on the host, after the speed key. The time commands are
        // refused, and the camera moves before the picture is held.
        if (net_options().net_loopback_photo && tick == 100)
            runtime_.exercise_shared_photo(static_cast<uint32_t>(tick));
        // The host's preferences send nothing: its RESTORE puts back the
        // normal speed, then its Cancel or UNDO the speed they opened with,
        // and the joiner keeps the speed the key set.
        if (tick == 110) {
            require(
                client_world.game.requested_speed == host_world.game.requested_speed &&
                    host_world.game.requested_speed == speed_before + 1,
                "the host's speed key did not reach the joiner"
            );
            host_puts_back(oa::base::game_loop::normal_game_speed);
        }
        if (tick == 116) {
            require_put_back_kept("RESTORE", oa::base::game_loop::normal_game_speed);
            host_puts_back(static_cast<uint16_t>(speed_before + 1));
        }
        if (tick == 122)
            require_put_back_kept("Cancel or UNDO", static_cast<uint16_t>(speed_before + 1));
        // A watcher's preferences put back the speed they opened with
        // straight into its match (their Cancel, UNDO or RESTORE); the host
        // hears nothing of it, and its next speed key reaches the watcher.
        if (watching && tick == 122) {
            host_speeds_before_cancel = host_speeds_received();
            host_speed_before_cancel = host_world.game.requested_speed;
            put_back_speed(joiner, speed_before);
        }
        if (watching && tick == 130) {
            require(
                host_speeds_received() == host_speeds_before_cancel &&
                    host_world.game.requested_speed == host_speed_before_cancel &&
                    client_world.game.requested_speed == speed_before,
                "the watcher's preferences sent the speed they wrote back"
            );
            std::cout << "net loopback check: the watcher's preferences wrote back speed "
                      << speed_before << " and sent nothing\n";
            raise_host_speed();
        }
        // The host's in-game menu holds nothing in a shared match and sends no
        // pause: the client keeps ticking and hears no 0x19.
        if (tick == 150) {
            runtime_.match_paused_ = true;
            pauses_before_menu = pauses_received();
            client_tick_before_menu = client_world.game.tick;
        }
        if (tick == 170) {
            require(
                pauses_received() == pauses_before_menu &&
                    (client_world.game.sim_run_flags & nm::run_flag_paused) == 0 &&
                    client_world.game.tick > client_tick_before_menu,
                "the host's menu paused the client"
            );
            runtime_.match_paused_ = false;
        }
        // The held picture covers the next 30 exchanges. The pause key below
        // still answers, and the camera no longer moves.
        if (net_options().net_loopback_photo && tick == 170)
            runtime_.exercise_shared_photo(static_cast<uint32_t>(tick));
        if (net_options().net_loopback_photo && tick == 200)
            runtime_.exercise_shared_photo(static_cast<uint32_t>(tick));
        // The Pause key pauses both machines, whose ticks then hold, and
        // pressed again resumes both.
        if (tick == 172)
            press_pause();
        if (tick == 176) {
            require(both_paused(), "the Pause key did not pause both machines");
            held_host_tick = host_world.game.tick;
            held_client_tick = client_world.game.tick;
        }
        if (tick == 184) {
            require(
                both_paused() && host_world.game.tick == held_host_tick &&
                    client_world.game.tick == held_client_tick,
                "a paused machine's tick moved"
            );
            press_pause();
        }
        if (tick == 190) {
            require(
                (host_world.game.sim_run_flags & nm::run_flag_paused) == 0 &&
                    (client_world.game.sim_run_flags & nm::run_flag_paused) == 0 &&
                    host_world.game.tick > held_host_tick &&
                    client_world.game.tick > held_client_tick,
                "the second Pause did not resume both machines"
            );
            // A machine that runs hears a pause only in a step, after its
            // tick has moved on, while a paused one hears the resume before
            // it steps, so the two machines may hold for a different number
            // of ticks. The machine behind catches up before the worlds are
            // compared at equal ticks.
            const uint32_t host_tick = host_world.game.tick;
            const uint32_t client_tick = client_world.game.tick;
            require(
                std::max(host_tick, client_tick) - std::min(host_tick, client_tick) <= 2,
                "the pause left the machines' ticks far apart"
            );
            std::cout
                << "net loopback check: the host's menu paused nothing; its Pause key held both "
                << "machines at ticks " << held_host_tick << " / " << held_client_tick << "\n";
            while (host_world.game.tick < client_world.game.tick) {
                NetHost::loopback_step(*this);
                note_host_pose();
                count_shots(host_world, host_shots);
                pump_both(kTickPumpWaitMs);
            }
            while (client_world.game.tick < host_world.game.tick) {
                NetHost::loopback_step(joiner_play);
                compare_copy();
                count_shots(client_world, client_shots);
                pump_both(1);
            }
        }
        // The host's window losing the focus pauses nothing in a network
        // match, though When the window loses focus pauses a game played
        // alone by default (the check's fresh preferences keep the default):
        // both machines keep ticking and the joiner hears no 0x19.
        if (tick == 192) {
            pauses_before_focus = pauses_received();
            host_tick_before_focus = host_world.game.tick;
            client_tick_before_focus = client_world.game.tick;
            host_window_event(SDL_EVENT_WINDOW_FOCUS_LOST);
        }
        if (tick == 198) {
            require(
                pauses_received() == pauses_before_focus &&
                    (host_world.game.sim_run_flags & nm::run_flag_paused) == 0 &&
                    (client_world.game.sim_run_flags & nm::run_flag_paused) == 0 &&
                    host_world.game.tick > host_tick_before_focus &&
                    client_world.game.tick > client_tick_before_focus,
                "losing the host window's focus paused the network match"
            );
            host_window_event(SDL_EVENT_WINDOW_FOCUS_GAINED);
            std::cout << "net loopback check: the host's window lost the focus and both machines "
                      << "ran on\n";
        }
        if (tick == kLoopbackFireFrom) {
            const auto& unit = host_world.units[host_commander];
            const auto map_depth = static_cast<int32_t>(host_world.game.map_pixel_height) << 16;
            const int32_t ahead = unit.position.z + kLoopbackShotDistance < map_depth
                                      ? kLoopbackShotDistance
                                      : -kLoopbackShotDistance;
            (void)runtime_.match_->issue_attack_ground(
                host_commander, {unit.position.x, unit.position.y, unit.position.z + ahead}, false
            );
            if (shooter_slot != 0) {
                const int32_t map_width = static_cast<int32_t>(host_world.game.map_pixel_width)
                                          << 16;
                const int32_t beside = unit.position.x + (1 << 16) < map_width
                                           ? static_cast<int32_t>(1 << 16)
                                           : -(1 << 16);
                (void)host_match.issue_attack_ground(
                    shooter_slot,
                    {unit.position.x + beside, unit.position.y, unit.position.z + ahead},
                    false
                );
            }
        }
        if (tick == kLoopbackFireUntil) {
            (void)runtime_.match_->issue_stop(host_commander);
            if (shooter_slot != 0)
                host_match.kill_unit(shooter_slot, 0);
        }
        net_frame();
        joiner_play.net_frame();
        NetHost::loopback_step(*this);
        note_host_pose();
        count_shots(host_world, host_shots);
        NetHost::loopback_step(joiner_play);
        compare_copy();
        count_shots(client_world, client_shots);
        pump_both(kTickPumpWaitMs);
    }
    require((client_world.game.sim_run_flags & nm::run_flag_paused) == 0, "client stayed paused");
    require(
        runtime_.preferences_.current_game_speed != speed_before &&
            joiner.preferences_.current_game_speed == runtime_.preferences_.current_game_speed,
        "speed change did not reach the client"
    );
    // The shooter's death (0x0c) has to reach the joiner before the settle
    // counts live units. A run that ends at or before the fire window never
    // kills it.
    if (shooter_slot != 0 && ticks > kLoopbackFireUntil) {
        const auto gone_limit = 3u * host_world.game.units_per_player;
        bool shooter_gone = !unit_live(host_world.units[shooter_slot]) &&
                            !unit_live(client_world.units[shooter_slot]);
        for (uint32_t waited = 0; waited < gone_limit && !shooter_gone; ++waited) {
            exchange();
            shooter_gone = !unit_live(host_world.units[shooter_slot]) &&
                           !unit_live(client_world.units[shooter_slot]);
        }
        require(shooter_gone, "the loopback shooter is still live");
    }
    // Settle: the owner's full record refreshes every slot once per
    // units_per_player ticks.
    const auto settle_limit = 3u * host_world.game.units_per_player;
    uint64_t host_digest = 0;
    uint64_t client_digest = 0;
    uint32_t settled_after = 0;
    bool agreed = false;
    for (uint32_t extra = 0; extra < settle_limit && !agreed; ++extra) {
        NetHost::loopback_step(*this);
        note_host_pose();
        count_shots(host_world, host_shots);
        NetHost::loopback_step(joiner_play);
        compare_copy();
        count_shots(client_world, client_shots);
        pump_both(kTickPumpWaitMs);
        host_digest = world_digest(host_world);
        client_digest = world_digest(client_world);
        agreed = host_digest == client_digest;
        settled_after = extra + 1;
    }
    uint32_t live = 0;
    for (uint32_t slot = 1; slot < host_world.unit_slot_count; ++slot)
        live += host_world.units[slot].type_index != 0 ? 1 : 0;
    const auto& host_net = *net_->net;
    const auto& client_net = *joiner_play.net_->net;
    char digests[64];
    std::snprintf(
        digests,
        sizeof digests,
        "%016llx / %016llx",
        static_cast<unsigned long long>(host_digest),
        static_cast<unsigned long long>(client_digest)
    );
    std::cout << "net loopback check: " << ticks << " ticks + " << settled_after << " settle, tick "
              << host_world.game.tick << " / " << client_world.game.tick << ", digests " << digests
              << ", live units " << live << ", records applied " << host_net.records_applied
              << " / " << client_net.records_applied << ", refused " << host_net.records_refused
              << " / " << client_net.records_refused << ", errors " << host_net.record_errors
              << " / " << client_net.record_errors << ", remote creates "
              << net_->binding.created_remote << " / " << joiner_play.net_->binding.created_remote
              << ", host commander copy divergence " << copy_divergence << " over " << copy_compared
              << " records, host commander shots " << host_shots.count << " / "
              << client_shots.count << "\n";
    require(agreed, "world digests differ after settling");
    require(
        commander_page(host_world) == host_page_turned &&
            commander_page(client_world) == joiner_page_before,
        "the host's turn of its commander's build page reached the joiner"
    );
    std::cout << "net loopback check: the host turned its commander's build page from "
              << host_page_before << " to " << host_page_turned << "; the joiner's copy kept page "
              << joiner_page_before << " and the worlds agree\n";
    require(
        copy_placed_tick != 0 && copy_placed_tick == first_applied_tick,
        "the client's copy of the host commander did not stand on its start position from its 0x09"
    );
    // Under the commander start sync the record each machine sends for its
    // commander is decoded as an ordinary one too: the copy heads straight
    // for the commander's goal from it and leaves the owner's path until the
    // owner's next records, so only the sync and the settled worlds are held.
    if (host_net.rules.commander_sync_tick != 0) {
        require(host_net.commander_syncs_sent > 0, "the host sent no commander start sync");
        std::cout << "net loopback check: the host sent " << host_net.commander_syncs_sent
                  << " commander start sync records; the client placed the commander from "
                  << client_net.commander_syncs_applied << "\n";
    } else {
        require(
            copy_compared > ticks / 2 && copy_divergence == 0,
            "the client's copy left the host commander's path"
        );
    }
    const uint32_t joiner_creates = 1u + (shooter_slot != 0 ? 1u : 0u);
    if (watching) {
        require(
            live == 1 && NetHost::local_commander(joiner_play) == 0,
            "the watcher was given a commander"
        );
        require(
            net_->binding.created_remote == 0 &&
                joiner_play.net_->binding.created_remote == joiner_creates,
            "the host commander did not reach the watcher"
        );
        require(
            joiner.match_->outcome() == sim::scenario::Outcome::ongoing, "the watcher was defeated"
        );
    } else {
        require(live == 2, "both commanders are not present");
        require(
            net_->binding.created_remote == 1 &&
                joiner_play.net_->binding.created_remote == joiner_creates,
            "remote commanders were not created"
        );
    }
    require(host_net.record_errors == 0 && client_net.record_errors == 0, "record errors");
    require(
        host_shots.count != 0 && client_shots.count == host_shots.count,
        "the host commander's shots did not all reach the client"
    );
    require(
        same_point(client_shots.origin, host_shots.origin) &&
            same_point(client_shots.target, host_shots.target),
        "the client's copy of the first shot left from or for another point"
    );
    require(
        host_world.units[host_commander].position.z != commander_z, "host commander did not move"
    );
    if (shooter_slot != 0) {
        std::cout << "net loopback check: shooter " << shooter_name << " fired "
                  << shooter_host.count << " / " << shooter_client.count << " shots\n";
        require(
            shooter_host.count > 0 && shooter_host.count == shooter_client.count,
            "the loopback shooter's shots did not all reach the client"
        );
    }
    const auto received_channel = static_cast<uint8_t>(oa::netgame::TrafficChannel::received);
    const auto& joiner_traffic = joiner_play.net_->connection.packets->traffic;
    const auto received_of = [&](oa::netgame::RecordType type) {
        return joiner_traffic.record_count[static_cast<uint8_t>(type)][received_channel];
    };
    const auto narrow_fire = received_of(oa::netgame::RecordType::weapon_fire);
    const auto narrow_intercepted = received_of(oa::netgame::RecordType::projectile_intercepted);
    const auto narrow_feature = received_of(oa::netgame::RecordType::feature_event);
    std::cout << "net loopback check: wide records applied " << host_net.wide_records_applied
              << " / " << client_net.wide_records_applied << "; weapon records received 0x0d "
              << narrow_fire << ", 0x0e " << narrow_intercepted << ", 0x0f " << narrow_feature
              << "\n";
    if (oa::netgame::wide_weapon_records(host_net.rules)) {
        const uint32_t least = shooter_slot != 0 ? shooter_host.count : 1u;
        require(
            client_net.wide_records_applied >= least && narrow_fire == 0 &&
                narrow_intercepted == 0 && narrow_feature == 0,
            "a wide game sent a narrow weapon record or the joiner applied no wide record"
        );
    } else {
        require(
            host_net.wide_records_applied == 0 && client_net.wide_records_applied == 0 &&
                narrow_fire > 0,
            "a narrow game applied a wide record or the joiner received no 0x0d"
        );
    }
    // The in-game team panels over the session, run as the host's panels
    // run them, through the host the extension fills for them. ALLIES.GUI
    // allies the joiner and breaks the alliance again: each change reaches
    // the joiner's alliance tables (0x23) and its line the joiner's log.
    // SHARE.GUI gives the joiner 100 metal (0x16), which the joiner is
    // credited, then a selected unit (0x14) and the map: the joiner's player
    // owns the unit, the host's is gone on both machines. CONTROL.GUI turns
    // watching off, which the joiner's copy of the host's block shows, and
    // on again, and closes, removing nobody.
    bool whiteboard_drawn = false; // the host's whiteboard marks reached the joiner
    if (!watching) {
        auto& panel_world = *net_->net->world;
        const auto panel_host_slot = panel_world.game.local_player_index;
        const auto panel_client_slot = static_cast<uint8_t>(
            std::find_if(
                std::begin(panel_world.game.players),
                std::end(panel_world.game.players),
                [client_id](const Player& player) {
                    return player.in_use != 0 && player.player_id == client_id;
                }
            ) -
            std::begin(panel_world.game.players)
        );
        require(panel_client_slot < OA_PLAYER_COUNT, "the host has no slot for the client");
        oa::ui::hud::TeamPanelHost panels{};
        panels.context = &runtime_;
        extension.team_panel_host(extension.context, runtime_, panels);
        // The chat line goes out as the match's chat lines do (net_send_chat).
        oa::sim::messages::Hooks chat_hooks{};
        chat_hooks.context = this;
        chat_hooks.share_chat = [](void* context, const char* line) {
            static_cast<NetworkPlay*>(context)->net_send_chat(line);
        };
        const oa::ui::hud::PanelControls no_controls{
            nullptr,
            [](void*, const char*) -> int32_t { return -1; },
            [](void*, int32_t, int32_t) {},
            [](void*, int32_t, int32_t) {},
            [](void*, int32_t) {},
            [](void*, int32_t, int32_t) {},
            [](void*, int32_t) -> int32_t { return 0; },
            [](void*, int32_t) -> const char* { return ""; },
            [](void*, int32_t, const char*) {},
            [](void*, int32_t) {},
            [](void*, int32_t, bool) {},
            [](void*, int32_t, bool) {},
            [](void*, int32_t, const char*) {},
        };
        const oa::ui::hud::HudEvents events{
            nullptr, [](void*, const char*) {}, [](void*, const char*, int32_t) {}
        };
        const auto run_both = [&](int frames) {
            for (int frame = 0; frame < frames; ++frame) {
                NetHost::loopback_step(*this);
                NetHost::loopback_step(joiner_play);
                pump_both(kTickPumpWaitMs);
            }
        };
        panel_world.game.chat_mode = OA_CHAT_MODE_EVERYONE;
        const auto& joiner_local = client_world.game.players[client_world.game.local_player_index];
        char ally_control[16];
        std::snprintf(
            ally_control,
            sizeof ally_control,
            "LIVEALLY%u",
            static_cast<unsigned>(panel_client_slot)
        );
        // ui.whiteboard under the recorder: while the host's player allies
        // the joiner's, a line and a marker the host draws reach the joiner's
        // whiteboard (recorder message kind 0), and the marker is echoed to
        // its log with its label (checked with the other lines below); once
        // the alliance is broken, the next ones do not.
        const auto draws = [](const Runtime& side) { return side.ui_rules().whiteboard.enabled; };
        const bool whiteboard =
            draws(runtime_) && draws(joiner) &&
            net_->net->rules.recorder_protocol != oa::netgame::recorder_protocol_plain;
        uint32_t whiteboard_batches = 0;
        const auto draw_on_whiteboard = [&](uint8_t allied) {
            const auto& board = joiner.match_whiteboard();
            const auto lines_before = board.lines.size();
            const auto markers_before = board.markers.size();
            const int32_t x = allied != 0 ? 320 : 640;
            const int32_t y = 288;
            const auto color = oa::ui::hud::stock_dot_color(panel_world, panel_host_slot);
            const std::string label = allied != 0 ? "allied mark" : "unallied mark";
            auto& drawn = runtime_.match_whiteboard();
            oa::ui::hud::whiteboard_draw_line(drawn, x, y, x + 64, y + 32, color);
            oa::ui::hud::whiteboard_place_marker(drawn, x, y, color, label);
            net_frame();
            run_both(4);
            if (allied == 0) {
                require(
                    board.lines.size() == lines_before && board.markers.size() == markers_before,
                    "the joiner showed whiteboard marks from a player not allied with it"
                );
                return;
            }
            require(
                board.lines.size() == lines_before + 1 &&
                    board.markers.size() == markers_before + 1,
                "the host's whiteboard marks did not reach the joiner"
            );
            const auto& line = board.lines.back();
            const auto& marker = board.markers.back();
            require(
                line.x1 == x && line.y1 == y && line.x2 == x + 64 && line.y2 == y + 32 &&
                    line.color == color && marker.x == x && marker.y == y &&
                    marker.color == color && board.received_marker && board.received_x == x &&
                    board.received_y == y,
                "the joiner's whiteboard holds other marks than the host drew"
            );
            whiteboard_batches = joiner_play.net_->net->whiteboard_batches;
            whiteboard_drawn = true;
        };
        for (const uint8_t allied : {uint8_t{1}, uint8_t{0}}) {
            const auto result = oa::ui::hud::allies_panel_click(
                panel_world, ally_control, no_controls, events, panels
            );
            require(result.announcement[0] != '\0', "ALLIES.GUI had no line to say");
            oa::sim::messages::post_chat(
                panel_world,
                panel_world.game.players[panel_host_slot],
                result.announcement,
                oa::ui::hud::kMessageKindChat,
                nullptr,
                chat_hooks
            );
            run_both(4);
            require(
                panel_world.game.players[panel_host_slot].alliance[panel_client_slot] == allied &&
                    joiner_local.allied_by[client_host_slot] == allied,
                "the host's alliance change did not reach the joiner"
            );
            require(
                joiner_play.net_->binding.match->allied(
                    static_cast<uint8_t>(client_host_slot), joiner_local.index
                ) == (allied != 0),
                "the joiner's simulation did not follow the host's alliance"
            );
            if (whiteboard)
                draw_on_whiteboard(allied);
        }
        if (whiteboard)
            std::cout << "net loopback check: the host's whiteboard marks reached the allied "
                         "joiner in "
                      << whiteboard_batches << " batches and stayed away once unallied\n";

        // SHARE.GUI's view of the host's players.
        std::array<oa::UnitEconomy*, OA_PLAYER_COUNT> economies{};
        std::array<uint8_t, OA_PLAYER_COUNT> setup_flags{};
        for (std::size_t index = 0; index < OA_PLAYER_COUNT; ++index) {
            auto& player = panel_world.game.players[index];
            economies[index] = oa::world_player_economy(&panel_world, &player);
            if (const auto* info = oa::world_player_info(&panel_world, &player))
                setup_flags[index] = static_cast<uint8_t>(info->options);
        }
        oa::ui::hud::ShareWorld share_world{
            panel_world.game.players,
            economies.data(),
            setup_flags.data(),
            panel_host_slot,
            panel_world.game.difficulty
        };
        oa::ui::hud::SharePanel share{};
        uint16_t share_flags = panel_world.game.frame_flags;
        require(
            oa::ui::hud::open_share_panel(share_world, share, share_flags), "SHARE.GUI did not open"
        );
        int32_t share_row = -1;
        for (int32_t row = 0; row < share.recipient_count; ++row)
            if (share.recipients[row] == panel_client_slot)
                share_row = row;
        require(share_row >= 0, "SHARE.GUI does not list the joiner");
        oa::ui::hud::ShareHost share_host{};
        share_host.user = &panels;
        share_host.send_metal = [](void* user, uint8_t from, uint8_t to, float amount) {
            const auto& host = *static_cast<const oa::ui::hud::TeamPanelHost*>(user);
            host.resources_given(host.context, from, to, true, amount);
        };
        share_host.send_energy = [](void* user, uint8_t from, uint8_t to, float amount) {
            const auto& host = *static_cast<const oa::ui::hud::TeamPanelHost*>(user);
            host.resources_given(host.context, from, to, false, amount);
        };
        share_host.share_map = [](void* user, uint8_t from, uint8_t to) {
            const auto& host = *static_cast<const oa::ui::hud::TeamPanelHost*>(user);
            host.sight_shared(host.context, from, to);
        };
        auto& joiner_world = *joiner_play.net_->net->world;
        const auto* joiner_economy = oa::world_player_economy(
            &joiner_world, &joiner_world.game.players[joiner_world.game.local_player_index]
        );
        require(joiner_economy != nullptr, "the joiner has no economy");
        (void)oa::ui::hud::share_panel_click(
            share_world,
            share,
            "OK",
            share_row,
            100,
            0,
            false,
            false,
            share_flags,
            events,
            share_host
        );
        nm::packet_layer_flush(
            net_->connection.packets, nm::net_connection_time(&net_->connection), true
        );
        pump_both(1);
        const auto credited_before = joiner_economy->metal.produced;
        (void)nm::net_match_pump(joiner_play.net_->net.get());
        require(
            joiner_economy->metal.produced - credited_before == 100.0F,
            "the joiner was not credited the metal the host gave"
        );

        // A kbot the host builds and the host's commander, every unit the
        // host's player has, are selected and given with the map through
        // SHARE.GUI's own path (Runtime::give_selected_units_to). The kbot
        // goes. The commander stays when its type is in the Commander
        // category and goes too when it is not: the check says which, and
        // the tests that run it hold it to the data they give it.
        const auto named = [](const char* field, std::size_t capacity, std::string_view name) {
            const auto held = field_text(field, capacity);
            return held.size() == name.size() &&
                   std::equal(held.begin(), held.end(), name.begin(), [](char a, char b) {
                       return std::tolower(static_cast<unsigned char>(a)) ==
                              std::tolower(static_cast<unsigned char>(b));
                   });
        };
        uint16_t kbot_type = 0;
        for (uint32_t type = 1; type < panel_world.unit_def_count && kbot_type == 0; ++type)
            if (named(
                    panel_world.unit_defs[type].unit_name,
                    sizeof panel_world.unit_defs[type].unit_name,
                    "ARMPW"
                ))
                kbot_type = static_cast<uint16_t>(type);
        require(kbot_type != 0, "the install has no ARMPW to give");
        const auto& commander_record = panel_world.units[host_commander];
        const int32_t aside = commander_record.position.x + (96 << 16) <
                                      (static_cast<int32_t>(panel_world.game.map_pixel_width) << 16)
                                  ? (96 << 16)
                                  : -(96 << 16);
        oa::sim::unit_spawn::Request request{};
        request.player = panel_host_slot;
        request.type = kbot_type;
        request.position = {
            static_cast<uint32_t>(commander_record.position.x + aside),
            static_cast<uint32_t>(commander_record.position.y),
            static_cast<uint32_t>(commander_record.position.z)
        };
        request.finished = true;
        request.state = 1;
        auto* made = net_->binding.match->create(request);
        require(
            made != nullptr && made->unit != nullptr, "the host could not build a unit to give"
        );
        const auto gift = made->unit_index;
        run_both(4);
        require(
            (client_world.units[gift].flags & OA_UNIT_FLAG_LIVE) != 0,
            "the joiner has no copy of the unit to be given"
        );
        uint32_t own_count = 0;
        oa::Unit* own = oa::world_player_units(
            &panel_world, &panel_world.game.players[panel_host_slot], &own_count
        );
        uint32_t host_units = 0;
        for (uint32_t index = 0; index < own_count; ++index)
            if (unit_live(own[index])) {
                own[index].flags |= OA_UNIT_FLAG_SELECTED;
                ++host_units;
            }
        require(
            host_units == 2 &&
                (panel_world.units[host_commander].flags & OA_UNIT_FLAG_SELECTED) != 0,
            "the host's player has more or fewer units than its commander and the kbot"
        );
        const auto commander_type = panel_world.units[host_commander].type_index;
        share_host.user = this;
        share_host.give_units = [](void* user, uint8_t, uint8_t to) {
            static_cast<NetworkPlay*>(user)->runtime_.give_selected_units_to(to);
        };
        share_host.share_map = [](void* user, uint8_t from, uint8_t to) {
            auto& self = *static_cast<NetworkPlay*>(user);
            nm::net_match_share_sight(self.net_->net.get(), from, to);
        };
        share_host.send_metal = nullptr;
        share_host.send_energy = nullptr;
        const auto counted = [](NetworkPlay& play, oa::netgame::RecordType type) {
            return play.net_->connection.packets->traffic
                .record_count[static_cast<uint8_t>(type)]
                             [static_cast<uint8_t>(oa::netgame::TrafficChannel::received)];
        };
        const auto received = [&](oa::netgame::RecordType type) {
            return counted(joiner_play, type);
        };
        const auto gifts_before = received(oa::netgame::RecordType::unit_transfer);
        const auto maps_before = received(oa::netgame::RecordType::resource_give);
        // Cells the host's player has mapped on the joiner's machine and the
        // joiner's player has not; the first rows are made so.
        const auto host_bit = static_cast<uint16_t>(1U << client_host_slot);
        const auto joiner_bit = static_cast<uint16_t>(1U << joiner_local.index);
        auto& joiner_mapped = joiner_play.net_->binding.match->sight_mutable().player_bits;
        constexpr std::size_t marked_cells = 256;
        for (std::size_t cell = 0; cell < std::min(joiner_mapped.size(), marked_cells); ++cell)
            joiner_mapped[cell] =
                static_cast<uint16_t>((joiner_mapped[cell] | host_bit) & ~joiner_bit);
        (void)oa::ui::hud::share_panel_click(
            share_world, share, "OK", share_row, 0, 0, true, true, share_flags, events, share_host
        );
        run_both(8);
        const auto joiner_host_slot = static_cast<uint8_t>(client_host_slot);
        // The host's commander as both machines hold it: live under the
        // host's player, or gone from its slot.
        const bool commander_kept =
            unit_live(host_world.units[host_commander]) &&
            host_world.units[host_commander].owner_index == panel_host_slot &&
            unit_live(client_world.units[host_commander]) &&
            client_world.units[host_commander].owner_index == joiner_host_slot;
        // One 0x14 for each unit that went.
        require(
            received(oa::netgame::RecordType::unit_transfer) - gifts_before ==
                    (commander_kept ? 1u : 2u) &&
                received(oa::netgame::RecordType::resource_give) - maps_before == 1,
            "the joiner did not receive the units and the map the host gave"
        );
        require(
            std::none_of(
                joiner_mapped.begin(),
                joiner_mapped.end(),
                [&](uint16_t bits) { return (bits & host_bit) != 0 && (bits & joiner_bit) == 0; }
            ),
            "the joiner's player did not take the map the host's player gave"
        );
        // A unit of a type the joiner's player owns on the joiner's machine.
        const auto joiner_owned = [&](uint16_t type) {
            uint16_t found = 0;
            for (uint32_t slot = 1; slot < client_world.unit_slot_count && found == 0; ++slot) {
                const auto& unit = client_world.units[slot];
                if (unit.type_index == type && unit_live(unit) &&
                    unit.owner_index == client_world.game.local_player_index)
                    found = static_cast<uint16_t>(slot);
            }
            return found;
        };
        const uint16_t given = joiner_owned(kbot_type);
        require(given != 0 && given != gift, "the joiner's player does not own the given unit");
        require(
            (host_world.units[gift].flags & OA_UNIT_FLAG_LIVE) == 0 &&
                (client_world.units[gift].flags & OA_UNIT_FLAG_LIVE) == 0,
            "the given unit is still on a machine"
        );
        require(
            (host_world.units[given].flags & OA_UNIT_FLAG_LIVE) != 0 &&
                host_world.units[given].owner_index == panel_client_slot,
            "the host does not hold the given unit as the joiner's"
        );
        if (!commander_kept) {
            // The commander went too; the joiner's player gives it back
            // through the same path, and the host's player owns it again.
            const uint16_t taken = joiner_owned(commander_type);
            require(
                taken != 0 && !unit_live(host_world.units[host_commander]) &&
                    !unit_live(client_world.units[host_commander]) &&
                    host_world.units[taken].owner_index == panel_client_slot,
                "the host's commander neither stayed nor went to the joiner on both machines"
            );
            uint32_t joiner_count = 0;
            oa::Unit* joiner_units = oa::world_player_units(
                &joiner_world, &joiner_world.game.players[joiner_local.index], &joiner_count
            );
            for (uint32_t index = 0; index < joiner_count; ++index)
                joiner_units[index].flags &= ~OA_UNIT_FLAG_SELECTED;
            joiner_world.units[taken].flags |= OA_UNIT_FLAG_SELECTED;
            const auto returns_before = counted(*this, oa::netgame::RecordType::unit_transfer);
            joiner.give_selected_units_to(joiner_host_slot);
            run_both(8);
            uint16_t returned = 0;
            for (uint32_t slot = 1; slot < host_world.unit_slot_count && returned == 0; ++slot) {
                const auto& unit = host_world.units[slot];
                if (unit.type_index == commander_type && unit_live(unit) &&
                    unit.owner_index == panel_host_slot)
                    returned = static_cast<uint16_t>(slot);
            }
            require(
                counted(*this, oa::netgame::RecordType::unit_transfer) - returns_before == 1 &&
                    returned != 0 && !unit_live(client_world.units[taken]) &&
                    unit_live(client_world.units[returned]) &&
                    client_world.units[returned].owner_index == joiner_host_slot,
                "the joiner's player did not give the host's commander back"
            );
        }
        // The joiner's gift goes again, so its commander is its last unit.
        joiner_play.net_->binding.match->kill_unit(given, 0);
        run_both(8);
        require(
            (host_world.units[given].flags & OA_UNIT_FLAG_LIVE) == 0 &&
                (client_world.units[given].flags & OA_UNIT_FLAG_LIVE) == 0,
            "the given unit did not die on both machines"
        );
        bool gifts_agreed = false;
        for (uint32_t extra = 0; extra < settle_limit && !gifts_agreed; ++extra) {
            run_both(1);
            gifts_agreed = world_digest(host_world) == world_digest(client_world);
        }
        require(gifts_agreed, "the machines disagree after the host gave its units");
        std::cout << "net loopback check: the host's player gave every unit it had; the kbot went "
                     "and its commander "
                  << (commander_kept ? "stayed" : "went too, then came back from the joiner")
                  << ", and the machines agree\n";

        auto* panel_info =
            oa::world_player_info(&panel_world, &panel_world.game.players[panel_host_slot]);
        require(panel_info != nullptr, "the host has no setup block");
        const auto allowed_before = panel_info->options & OA_SETUP_OPTION_WATCHING_ALLOWED;
        (void)oa::ui::hud::control_panel_click(
            panel_world, "WATCHING", no_controls, events, panels
        );
        run_both(4);
        const auto* host_copy_info = info_for_id(client_world, host_id);
        require(
            host_copy_info != nullptr &&
                (host_copy_info->options & OA_SETUP_OPTION_WATCHING_ALLOWED) ==
                    (allowed_before ^ OA_SETUP_OPTION_WATCHING_ALLOWED),
            "the joiner's copy of the host's block did not follow WATCHING"
        );
        // OK publishes the session again from the host's block as it now
        // is: the joiner's view of it follows WATCHING, and keeps the name
        // in full.
        (void)oa::ui::hud::control_panel_click(panel_world, "OK", no_controls, events, panels);
        run_both(4);
        const auto& joiner_view = joiner_play.net_->connection.host->engine;
        const auto published_options = static_cast<uint16_t>(joiner_view.desc.user[0] >> 16);
        require(
            (published_options & OA_SETUP_OPTION_WATCHING_ALLOWED) ==
                    (allowed_before ^ OA_SETUP_OPTION_WATCHING_ALLOWED) &&
                std::strlen(joiner_view.session_name) > 16 &&
                std::strcmp(joiner_view.session_name, net_->connection.host->engine.session_name) ==
                    0,
            "CONTROL.GUI's OK did not publish the host's block and the full name again"
        );
        (void)oa::ui::hud::control_panel_click(
            panel_world, "WATCHING", no_controls, events, panels
        );
        const auto closed =
            oa::ui::hud::control_panel_click(panel_world, "OK", no_controls, events, panels);
        run_both(4);
        require(
            closed.click == oa::ui::hud::TeamPanelClick::closed &&
                (host_copy_info->options & OA_SETUP_OPTION_WATCHING_ALLOWED) == allowed_before &&
                panel_world.game.players[panel_client_slot].in_use != 0 &&
                panel_world.game.players[panel_client_slot].reject_reason == 0,
            "CONTROL.GUI's OK removed a player or left watching changed"
        );
        std::cout << "net loopback check: the host's team panels allied the joiner and broke it "
                     "off, gave it "
                  << "100 metal, unit " << gift << " (now its unit " << given
                  << ") and the map, and "
                  << "turned watching off and on\n";
    }
    const auto client_lines = joiner.match_message_lines();
    const auto host_chat = runtime_.match_message_lines();
    const auto shown = [](const std::vector<std::string>& lines, const char* line) {
        return std::find(lines.begin(), lines.end(), line) != lines.end();
    };
    require(shown(client_lines, "<hoster> gg"), "chat did not reach the client");
    require(
        watching || (shown(client_lines, "<hoster>  allied with joiner") &&
                     shown(client_lines, "<hoster>  broke alliance with joiner")),
        "the lines ALLIES.GUI said did not reach the client"
    );
    require(
        !whiteboard_drawn || (shown(client_lines, "*hoster: allied mark") &&
                              !shown(client_lines, "*hoster: unallied mark")),
        "the joiner's log did not echo only the allied whiteboard marker"
    );
    require(
        shown(client_lines, "<hoster> glhf") && shown(host_chat, "<hoster> glhf"),
        "the typed chat line did not reach both players"
    );
    // The client stores the line as another player's chat from the host's
    // slot, which starts it with the host's logo; the host's own echo comes
    // from no player.
    namespace messages = oa::sim::messages;
    const auto place = [](const std::vector<std::string>& lines, const char* line) {
        return static_cast<std::size_t>(
            std::find(lines.begin(), lines.end(), line) - lines.begin()
        );
    };
    const auto received = logged_line(client_world, place(client_lines, "<hoster> glhf"));
    require(
        received && (received->kind & messages::kind_mask) == messages::kind_player_chat &&
            received->sender == client_host_slot,
        "the client did not store the host's chat line as the host's chat"
    );
    const auto echoed = logged_line(host_world, place(host_chat, "<hoster> glhf"));
    require(
        echoed && (echoed->kind & messages::kind_mask) == oa::ui::hud::kMessageKindChat &&
            echoed->sender == messages::sender_none,
        "the host's own chat line was not stored as a line from no player"
    );
    require(
        shown(host_chat, "<hoster> +clock") && !shown(client_lines, "<hoster> +clock"),
        "an option command's echo was not local to the host"
    );
    // "BPS" turns the host's traffic readout on and "NetStats" resets its
    // counters, their echoes local too. The readout then draws its two rates
    // over their bars from the connection's counters, and the next "BPS"
    // takes it away.
    auto& traffic = net_->connection.packets->traffic;
    require(
        host_world.game.show_bandwidth == 1 && client_world.game.show_bandwidth == 0,
        "+bps did not turn on the host's traffic readout alone"
    );
    require(
        traffic.reset_tick == netstats_tick && netstats_tick != 0,
        "+netstats did not reset the traffic counters"
    );
    require(
        shown(host_chat, "<hoster> +bps") && !shown(client_lines, "<hoster> +bps") &&
            shown(host_chat, "<hoster> +netstats") && !shown(client_lines, "<hoster> +netstats"),
        "the echo of +bps or +netstats was not local to the host"
    );

    struct Readout {
        std::vector<std::string> labels;
        int fills{};
    };

    const auto draw_readout = [&] {
        Readout readout;
        MatchOverlay overlay{};
        overlay.painter = &readout;
        overlay.game = &host_world.game;
        overlay.bottom = kLoopbackOverlayBottom;
        overlay.scale = 1;
        overlay.font_height = [](void*, OverlayFont) -> uint8_t {
            return kLoopbackOverlayFontHeight;
        };
        overlay.draw_text = [](void* painter, OverlayFont, int, int, const char* written, uint8_t) {
            static_cast<Readout*>(painter)->labels.emplace_back(written);
        };
        overlay.fill_rect = [](void* painter, int, int, int, int, uint8_t) {
            ++static_cast<Readout*>(painter)->fills;
        };
        draw_traffic_overlay(overlay, &traffic, nm::net_connection_time(&net_->connection));
        return readout;
    };
    const auto readout = draw_readout();
    const auto labelled = [](const std::string& label, std::string_view start) {
        return label.starts_with(start) && label.ends_with(" K/s");
    };
    require(
        readout.labels.size() == 2 && labelled(readout.labels[0], "Send - ") &&
            labelled(readout.labels[1], "Receive - ") && readout.fills >= 8,
        "the traffic readout did not draw both rates over their bars"
    );
    type_line("+bps");
    const auto hidden = draw_readout();
    require(
        host_world.game.show_bandwidth == 0 && hidden.labels.empty() && hidden.fills == 0,
        "+bps did not take the traffic readout away"
    );
    std::cout << "net loopback check: +bps showed \"" << readout.labels[0] << "\" and \""
              << readout.labels[1] << "\" and hid them again, +netstats reset the counters at tick "
              << netstats_tick << "\n";
    const auto session_step = [&](bool peer_runs) {
        NetHost::loopback_step(*this);
        if (peer_runs)
            NetHost::loopback_step(joiner_play);
        pump_both(kTickPumpWaitMs);
    };
    check_console_session_cheats(joiner, session_step);
    runtime_.check_console_network_takeover(joiner, session_step);
    check_console_network_session(joiner, session_step);
    if (watching) {
        // The opponent check passes over a watcher, so the host wins at once.
        require(
            runtime_.match_->outcome() == sim::scenario::Outcome::victory,
            "the host did not win against a watcher"
        );
    } else {
        // The client's commander self-destructs. The host allows watching, so
        // when the client's defeat countdown runs out it watches instead of
        // losing and sends its lobby block again; the host, its
        // only opponent gone, wins.
        const uint16_t commander = NetHost::local_commander(joiner_play);
        joiner.match_->toggle_self_destruct(std::span<const uint16_t>(&commander, 1));
        const auto defeated = [&] {
            return watches(info_for_id(client_world, client_id)) &&
                   watches(info_for_id(host_world, client_id)) &&
                   runtime_.match_->outcome() == sim::scenario::Outcome::victory;
        };
        for (uint32_t tick = 0; tick < kLoopbackDefeatTicks && !defeated(); ++tick) {
            NetHost::loopback_step(*this);
            NetHost::loopback_step(joiner_play);
            pump_both(kTickPumpWaitMs);
        }
        std::cout << "net loopback check: the defeated client watches ("
                  << (watches(info_for_id(client_world, client_id)) ? "yes" : "no") << " / "
                  << (watches(info_for_id(host_world, client_id)) ? "yes" : "no")
                  << " on the host), tick " << client_world.game.tick << "\n";
        require(
            NetHost::local_commander(joiner_play) == 0,
            "the client's commander did not self-destruct"
        );
        require(
            watches(info_for_id(client_world, client_id)), "the defeated client does not watch"
        );
        require(
            joiner.match_->outcome() == sim::scenario::Outcome::ongoing,
            "the defeated client lost instead of watching"
        );
        require(
            watches(info_for_id(host_world, client_id)), "the host was not told the client watches"
        );
        require(
            ui::frontend_dialogs::dialog_kind() ==
                ui::frontend_dialogs::DialogKind::continue_watching,
            "the defeated client did not open the continue-watching prompt"
        );
        require(!joiner.match_paused_, "the watcher prompt paused the network match");
        auto context = joiner.screen_context();
        require(ui::frontend_dialogs::dialog_click(&context, "CHOICE1"), "cannot accept watching");
        require(
            ui::frontend_dialogs::dialog_kind() == ui::frontend_dialogs::DialogKind::none &&
                joiner.match_->outcome() == sim::scenario::Outcome::ongoing,
            "accepting watching did not return to the running match"
        );
        require(
            runtime_.match_->outcome() == sim::scenario::Outcome::victory, "the host did not win"
        );
    }
    // The host's game is over and ends as a finished game does. The outcome
    // shows, but the clock of a shared match keeps stepping it; the end of
    // each frame retries the end of the match, which holds until the final
    // economy settles, and the next frame's network frame and clock step run,
    // until the host leaves the session and its end screen opens. The hold is
    // no pause: the host's tick moves on and it sends no probe. The joiner's
    // side runs between the host's frames.
    const auto finish_host_game = [&](auto&& between_frames, const char* what) {
        runtime_.match_paused_ = true;
        uint32_t held = 0;
        const auto probes_sent = [&] {
            return net_->connection.packets->traffic
                .record_count[static_cast<uint8_t>(oa::netgame::RecordType::probe)]
                             [static_cast<uint8_t>(oa::netgame::TrafficChannel::sent)];
        };
        const auto probes_before = probes_sent();
        const auto tick_before = host_world.game.tick;
        uint32_t probes_held = 0;
        uint32_t tick_held = 0;
        LoopbackWait wait;
        for (;;) {
            runtime_.finish_match_outcome();
            if (!net_->active)
                break;
            require(wait.next_round(), what);
            ++held;
            between_frames();
            net_frame();
            NetHost::loopback_step(*this);
            probes_held = probes_sent() - probes_before;
            tick_held = host_world.game.tick - tick_before;
        }
        require(held > 0, "the final economy settled before the other machine answered");
        require(tick_held == held, "the host's tick did not move on while it held on its outcome");
        require(probes_held == 0, "the host sent probes while it held on its outcome");
        require(
            on_screen(runtime_, Screen::campaign_end),
            "the host's finished game did not open its end screen"
        );
        require(
            reporter_world() == &host_world,
            "the end screen does not keep the host's finished match"
        );
        std::cout << "net loopback check: the host held on its outcome for " << held << " frames\n";
    };
    const auto client_slot_on_host = static_cast<std::size_t>(
        std::find_if(
            std::begin(host_world.game.players),
            std::end(host_world.game.players),
            [client_id](const Player& player) {
                return player.in_use != 0 && player.player_id == client_id;
            }
        ) -
        std::begin(host_world.game.players)
    );
    require(client_slot_on_host < OA_PLAYER_COUNT, "the host has no slot for the client");
    if (watching) {
        // The watcher stops answering once the host's game is over: it runs
        // no more frames. Before each of the host's frames, the time the host
        // last heard from it and the timeout scan's baseline move back past
        // the timeout and the drop time, standing in for that long a silence.
        // The host's hold pauses nothing, so its timeout scan names the
        // watcher and drops it as a lost connection (reason 6), and the final
        // economy stops waiting on it.
        constexpr uint32_t kTimeTicksPerSecond = 30; // the connection clock's rate
        auto& silent = net_->net->world->game.players[client_slot_on_host];
        finish_host_game(
            [&] {
                const auto silence =
                    (host_world.game.player_timeout_seconds + nm::timeout_drop_extra_seconds + 1) *
                    kTimeTicksPerSecond;
                const auto now = nm::net_connection_time(&net_->connection);
                silent.last_update_time = now - silence;
                net_->net->timeout_baseline = now - silence;
                NetHost::loopback_pump(*this, 1);
            },
            "the silent watcher held the host on its outcome"
        );
        require(
            silent.reject_reason == 6,
            "the host did not drop the silent watcher as a lost connection"
        );
    } else {
        // The joiner, watching since its defeat, plays on and answers the
        // host's final economy. The host's hold never pauses it. The host
        // leaves with no chat line: the joiner retires its slot as departed
        // (reason 1) and keeps the figures the host's final economy gave,
        // which are the host's own. Leaving changes nothing in the host's
        // finished world.
        bool paused_by_host = false;
        const auto joiner_frame = [&] {
            joiner_play.net_frame();
            NetHost::loopback_step(joiner_play);
            paused_by_host =
                paused_by_host || (client_world.game.sim_run_flags & nm::run_flag_paused) != 0;
            pump_both(kTickPumpWaitMs);
        };
        const auto& host_player = host_world.game.players[host_world.game.local_player_index];
        const auto host_player_count = host_world.game.player_count;
        std::array<uint8_t, sizeof host_player.alliance> host_alliances{};
        std::copy_n(host_player.alliance, host_alliances.size(), host_alliances.begin());
        finish_host_game(joiner_frame, "the host's final economy did not settle");
        const auto& host_copy = client_world.game.players[client_host_slot];
        wait_until(
            [&] {
                joiner_frame();
                return host_copy.in_use == 0;
            },
            "the joiner did not retire the host's slot"
        );
        const auto client_lines_after = joiner.match_message_lines();
        std::cout << "net loopback check: the host left; the joiner retired it with reason "
                  << static_cast<int>(host_copy.reject_reason) << ", host figures "
                  << host_player.kills << "/" << host_player.losses << " kills/losses, "
                  << host_player.energy_produced_total << " energy and "
                  << host_player.metal_produced_total << " metal produced; the joiner's copy "
                  << host_copy.kills << "/" << host_copy.losses << ", "
                  << host_copy.energy_produced_total << ", " << host_copy.metal_produced_total
                  << "\n";
        require(!paused_by_host, "the host's finished game paused the joiner");
        require(host_copy.reject_reason == 1, "the joiner did not retire the host as departed");
        require(
            !shown(client_lines_after, "Player hoster has disconnected"),
            "the host's leave showed a disconnect line"
        );
        require(
            host_copy.kills == host_player.kills && host_copy.losses == host_player.losses &&
                static_cast<float>(host_copy.energy_produced_total) ==
                    static_cast<float>(host_player.energy_produced_total) &&
                static_cast<float>(host_copy.metal_produced_total) ==
                    static_cast<float>(host_player.metal_produced_total),
            "the joiner's figures for the host differ from the host's final ones"
        );
        require(
            host_world.game.player_count == host_player_count &&
                std::equal(
                    host_alliances.begin(), host_alliances.end(), std::begin(host_player.alliance)
                ),
            "leaving changed the host's finished world"
        );
    }
    joiner.leave_match();
    net_bind_multiplayer();
    return 0;
}

} // namespace oa::app
