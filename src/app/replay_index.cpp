// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The index pass's parts that need no running match: the notes a recording's
// stream holds (read_stream_notes) and the index built from them and the
// replayed timeline (build_index). See replay_index.hpp.

#include "oa/app/replay_index.hpp"

#include "oa/core/player.h"
#include "oa/core/player_setup.h"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/tad.hpp"
#include "oa/media/director/planner.hpp"
#include "oa/netgame/private_channel.hpp"
#include "oa/netgame/records.hpp"
#include "oa/netgame/unicode_chat.hpp"
#include "oa/present/game_text.hpp"
#include "oa/session/demo.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app::replay_index {
namespace {

namespace director = oa::media::director;
namespace oademo = oa::formats::oademo;
namespace tad = oa::formats::tad;

static_assert(director_min_cut_ticks == director::min_cut_ticks);
static_assert(director_switch_numerator == director::switch_ratio_numerator);
static_assert(director_switch_denominator == director::switch_ratio_denominator);
static_assert(director_pan_reach_percent == director::pan_reach_percent);

/// The side value of a recorded player who only watches.
constexpr uint8_t watcher_side = 2;

/// The bit of a pause record's value that says the game is paused.
constexpr uint8_t paused_bit = 1;

/// Health event kinds of the match (EventHooks::unit_damaged): healing and
/// paralysis take no health off a unit, every other kind does.
constexpr uint8_t damage_kind_paralyze = 2;
constexpr uint8_t damage_kind_healing = 10;

/// A recorded player as the index reads the stream.
struct Seat {
    int number{};
    uint32_t id{}; ///< the transport id the records name the player by
    bool unicode_chat{};
};

/// Returns the number of the player a transport id names.
///
/// @param seats the recording's players
/// @param id the id a record names
/// @return the player's number, or 0 when no player has the id
int number_of(const std::vector<Seat>& seats, uint32_t id) noexcept {
    for (const Seat& seat : seats)
        if (seat.id == id)
            return seat.number;
    return 0;
}

/// Returns a chat record's line.
///
/// @param record the record, type byte first
/// @param unicode_chat the sender's lines are UTF-8
/// @return the line as UTF-8, up to the first NUL byte
std::string chat_line(std::span<const uint8_t> record, bool unicode_chat) {
    const std::size_t room = std::min(record.size() - 1, sizeof(netgame::ChatRecord::text));
    const char* text = reinterpret_cast<const char*>(record.data() + 1);
    const void* end = std::memchr(text, 0, room);
    const std::size_t length =
        end != nullptr ? static_cast<std::size_t>(static_cast<const char*>(end) - text) : room;
    return oa::present::decode_game_text(std::string_view(text, length), unicode_chat);
}

/// Adds a note for a record of the stream the index notes.
///
/// @param seats the recording's players
/// @param sender the number of the player the packet is from
/// @param tick the packet's tick
/// @param record the record, type byte first
/// @param[in,out] notes the notes so far
void note_record(
    const std::vector<Seat>& seats,
    int sender,
    uint32_t tick,
    std::span<const uint8_t> record,
    std::vector<Note>& notes
) {
    if (record.empty())
        return;
    Note note;
    note.tick = tick;
    note.player = sender;
    switch (static_cast<netgame::RecordType>(record[0])) {
    case netgame::RecordType::chat: {
        if (netgame::is_private_chat(record.data(), record.size()))
            return;
        bool unicode_chat = false;
        for (const Seat& seat : seats)
            if (seat.number == sender)
                unicode_chat = seat.unicode_chat;
        note.kind = NoteKind::chat;
        note.text = chat_line(record, unicode_chat);
        break;
    }
    case netgame::RecordType::pause_speed: {
        netgame::PauseSpeedRecord decoded{};
        if (netgame::decode_record(record.data(), record.size(), &decoded) !=
                netgame::WireError::ok ||
            decoded.kind != netgame::pause_speed_kind_pause)
            return;
        note.kind = NoteKind::pause;
        note.value = static_cast<uint8_t>(decoded.value & paused_bit);
        break;
    }
    case netgame::RecordType::reject: {
        netgame::RejectRecord decoded{};
        if (netgame::decode_record(record.data(), record.size(), &decoded) !=
            netgame::WireError::ok)
            return;
        note.kind = NoteKind::rejected;
        note.other = number_of(seats, decoded.player_id);
        note.value = decoded.reason;
        break;
    }
    case netgame::RecordType::disconnect_notice: {
        netgame::DisconnectNoticeRecord decoded{};
        if (netgame::decode_record(record.data(), record.size(), &decoded) !=
            netgame::WireError::ok)
            return;
        note.kind = NoteKind::disconnected;
        note.other = number_of(seats, decoded.player_id);
        break;
    }
    case netgame::RecordType::alliance: {
        netgame::AllianceRecord decoded{};
        if (netgame::decode_record(record.data(), record.size(), &decoded) !=
            netgame::WireError::ok)
            return;
        note.kind = NoteKind::alliance;
        note.player = number_of(seats, decoded.player_id_a);
        note.other = number_of(seats, decoded.player_id_b);
        // The record sets the first player's side of the pair, as playback
        // applies it; its last word, whatever it asks of the other machine,
        // changes nothing here.
        note.value = decoded.value != 0 ? uint8_t{1} : uint8_t{0};
        break;
    }
    case netgame::RecordType::economy: {
        netgame::EconomyRecord decoded{};
        if (netgame::decode_record(record.data(), record.size(), &decoded) !=
            netgame::WireError::ok)
            return;
        // The machines send their economy every ten seconds without asking
        // for a reply; only the end of a game asks for the others'.
        if (decoded.want_reply == 0)
            return;
        note.kind = NoteKind::end_of_game;
        note.value = 1;
        break;
    }
    default:
        return;
    }
    notes.push_back(std::move(note));
}

/// A marker with the order it was made in, to keep a stable order within a tick.
struct Ranked {
    oademo::IndexMarker marker{};
    uint32_t rank{}; ///< 0 for the start, 2 for the end, 1 for the rest
};

/// Returns the number of the player seated in a slot.
///
/// @param stream what the recording's stream holds
/// @param slot a player index of the replay; the viewer's own slot has no number
/// @return the player's number, or 0 when the slot holds no recorded player
int number_in_slot(const StreamNotes& stream, uint8_t slot) noexcept {
    return slot < stream.players.size() ? stream.players[slot].number : 0;
}

/// Returns the marker a departure's reject reason makes.
///
/// @param reason a netgame::RejectReason value
/// @param[out] kind the marker's kind
/// @return false for a reason that is no departure from the game
bool departure_kind(uint8_t reason, oademo::MarkerKind& kind) noexcept {
    switch (static_cast<netgame::RejectReason>(reason)) {
    case netgame::RejectReason::leaving:
    case netgame::RejectReason::game_closed:
    case netgame::RejectReason::creator_left:
        kind = oademo::MarkerKind::player_left;
        return true;
    case netgame::RejectReason::connection_lost:
        kind = oademo::MarkerKind::player_dropped;
        return true;
    case netgame::RejectReason::deathmatch:
        kind = oademo::MarkerKind::player_defeated;
        return true;
    default:
        return false;
    }
}

/// Returns one player's totals as the index holds them.
///
/// @param stats the player's stats
/// @return the totals
oademo::IndexTotals totals_of(const director::PlayerStats& stats) noexcept {
    oademo::IndexTotals totals;
    totals.kills = stats.kills;
    totals.losses = stats.losses;
    totals.commanders_lost = stats.commanders_lost;
    totals.units = stats.unit_count;
    totals.metal = stats.metal;
    totals.energy = stats.energy;
    return totals;
}

/// Returns the totals of a player who has left: what the player had
/// scored, and no units or stores.
///
/// @param last the player's last totals
/// @return the totals
oademo::IndexTotals totals_after_leaving(oademo::IndexTotals last) noexcept {
    last.units = 0;
    last.metal = 0;
    last.energy = 0;
    return last;
}

/// Fills the index's series and each player's last totals from the
/// timeline's stats.
///
/// @param source what the index is built from
/// @param samples how many samples the series hold
/// @param[in,out] index the index; its players are set
void fill_totals(const Source& source, std::size_t samples, oademo::TimelineIndex& index) {
    const director::Timeline& timeline = *source.timeline;
    const StreamNotes& stream = *source.stream;
    const uint32_t first = index.first_tick;
    const std::size_t seats = stream.players.size();
    std::vector<std::vector<std::optional<oademo::IndexTotals>>> rows(
        seats, std::vector<std::optional<oademo::IndexTotals>>(samples)
    );
    std::vector<std::optional<oademo::IndexTotals>> last_seen(seats);
    for (const director::PlayerStats& stats : timeline.stats) {
        if (stats.player >= seats || stats.tick < first)
            continue;
        const oademo::IndexTotals totals = totals_of(stats);
        last_seen[stats.player] = totals;
        const uint32_t since = stats.tick - first;
        if (since % index.period_ticks != 0)
            continue;
        const std::size_t at = since / index.period_ticks;
        if (at < samples)
            rows[stats.player][at] = totals;
    }
    for (const director::PlayerStats& stats : source.final_stats)
        if (stats.player < seats)
            last_seen[stats.player] = totals_of(stats);
    for (std::size_t seat = 0; seat < seats; ++seat) {
        const StreamPlayer& player = stream.players[seat];
        oademo::IndexPlayer listed;
        listed.number = player.number;
        listed.name = player.name;
        listed.watcher = player.watcher;
        const bool still_there = std::any_of(
            source.final_stats.begin(),
            source.final_stats.end(),
            [seat](const director::PlayerStats& stats) { return stats.player == seat; }
        );
        if (last_seen[seat].has_value())
            listed.final = still_there ? *last_seen[seat] : totals_after_leaving(*last_seen[seat]);
        index.players.push_back(std::move(listed));
        if (player.watcher)
            continue;
        oademo::IndexSeries series;
        series.player = player.number;
        oademo::IndexTotals carried{};
        bool gone = false;
        for (std::size_t at = 0; at < samples; ++at) {
            if (rows[seat][at].has_value()) {
                carried = *rows[seat][at];
                gone = false;
            } else if (at != 0 || last_seen[seat].has_value()) {
                gone = true;
            }
            const oademo::IndexTotals shown = gone ? totals_after_leaving(carried) : carried;
            series.kills.push_back(shown.kills);
            series.losses.push_back(shown.losses);
            series.commanders_lost.push_back(shown.commanders_lost);
            series.units.push_back(shown.units);
            series.metal.push_back(shown.metal);
            series.energy.push_back(shown.energy);
        }
        index.totals.push_back(std::move(series));
    }
}

/// Fills the index's battle intensity from the timeline's health events.
///
/// @param timeline the replayed game's timeline
/// @param samples how many stretches the intensity holds
/// @param[in,out] index the index; its damage is set
void fill_damage(
    const director::Timeline& timeline, std::size_t samples, oademo::TimelineIndex& index
) {
    index.damage.assign(samples, 0);
    for (const director::TimelineEvent& event : timeline.events) {
        if (event.kind != director::EventKind::damage || event.tick < index.first_tick ||
            event.amount <= 0 || event.damage_kind == damage_kind_healing ||
            event.damage_kind == damage_kind_paralyze)
            continue;
        const std::size_t at = std::min<std::size_t>(
            (event.tick - index.first_tick) / index.period_ticks, samples - 1
        );
        index.damage[at] += static_cast<uint64_t>(event.amount);
    }
}

/// The number of marker kinds.
constexpr std::size_t marker_kind_count = 16;

/// Tells whether a chat record's line is one a player typed: it starts with
/// the player's name between `<` and `>` and a space. The recorders' notices
/// and their replies to commands are carried as chat records too, and do not.
///
/// @param line the line, as UTF-8
/// @return true when the line starts `<name> `
bool typed_by_a_player(std::string_view line) noexcept {
    return line.starts_with('<') && line.find("> ", 1) != std::string_view::npos;
}

/// Pause records called in the opening of a game are left out: the players'
/// machines hold the start of a game with pauses of their own.
constexpr uint32_t opening_ticks = 3 * oademo::game_ticks_per_second;

/// The kind a death names when the unit's slot is taken by another unit.
constexpr uint8_t displaced_death_kind = 0;

/// Deaths that are not a unit destroyed or a building lost: captured,
/// reclaimed, dismissed or cancelled.
constexpr uint8_t death_kind_captured = 4;
constexpr uint8_t death_kind_reclaim = 5;
constexpr uint8_t death_kind_dismissed = 7;
constexpr uint8_t death_kind_cancelled = 9;

/// Ticks gathered into one engagement: ten seconds.
constexpr uint32_t engagement_period_ticks = 300;

/// The side of the square a large engagement is gathered in, in map pixels.
constexpr int32_t engagement_cell_pixels = 512;

/// Health points that make a cluster of hits a large engagement.
constexpr int64_t engagement_damage = 2000;

/// Hits that make a cluster a large engagement when the damage is smaller.
constexpr int engagement_hits = 8;

/// Who is allied with whom: each record sets one player's side of a pair.
class Alliances {
  public:

    /// Sets one player's alliance with another.
    ///
    /// @param player the player whose alliance it is
    /// @param other the other player
    /// @param allied the player allies with the other
    /// @return true when the pair's mutual alliance, both sides allied,
    ///         began or ended; a record naming no player, or a player itself,
    ///         changes nothing
    bool set(int player, int other, bool allied) {
        if (player == 0 || other == 0 || player == other)
            return false;
        const bool before = mutual(player, other);
        if (allied)
            sides_.insert({player, other});
        else
            sides_.erase({player, other});
        return before != mutual(player, other);
    }

    /// Tells whether two players are allied with each other.
    ///
    /// @param a one player
    /// @param b the other
    /// @return true when each has set its alliance with the other
    [[nodiscard]] bool mutual(int a, int b) const {
        return sides_.contains({a, b}) && sides_.contains({b, a});
    }

    /// Names the team of each player: the smallest number among the players
    /// it is allied with, directly or through others, each pair both ways.
    ///
    /// @param players the numbers of the players to place
    /// @return each player's team, in the order of `players`
    [[nodiscard]] std::vector<int> teams(const std::vector<int>& players) const {
        std::vector<int> team(players);
        for (bool changed = true; changed;) {
            changed = false;
            for (std::size_t a = 0; a < players.size(); ++a)
                for (std::size_t b = 0; b < players.size(); ++b) {
                    if (!mutual(players[a], players[b]) || team[a] == team[b])
                        continue;
                    team[a] = team[b] = std::min(team[a], team[b]);
                    changed = true;
                }
        }
        return team;
    }

  private:

    std::set<std::pair<int, int>> sides_{};
};

/// What the commanders of the replay decided.
struct Commanders {
    /// A marker for each commander lost.
    std::vector<oademo::IndexMarker> deaths{};
    /// A commander lost left at most one team with a commander.
    bool decided{};
    /// The players of that team; empty when no team was left.
    std::vector<int> winners{};
};

/// Follows the commanders through the replay's events.
///
/// A commander lost is a death with the commander flag of a player the
/// recording seats who plays; a commander displaced from its slot and
/// replaced by another of its player's on the same tick is no loss. The
/// replay is decided by the first commander lost after which at most one
/// team has a commander, teams being the mutual alliances at that tick; the
/// deaths of that tick all count first.
///
/// @param source what the index is built from
/// @return the markers, and whether and how the commanders decided the replay
Commanders follow_commanders(const Source& source) {
    const director::Timeline& timeline = *source.timeline;
    const StreamNotes& stream = *source.stream;
    Commanders found;
    std::set<std::tuple<uint32_t, uint16_t, uint8_t>> replacements;
    std::array<uint32_t, OA_PLAYER_COUNT> live{};
    std::array<bool, OA_PLAYER_COUNT> combatant{};
    const auto commander = [](const director::TimelineEvent& event) {
        return (event.flags & director::event_flag::commander) != 0 &&
               event.owner < OA_PLAYER_COUNT;
    };
    for (const director::TimelineEvent& event : timeline.events)
        if (commander(event) && event.kind == director::EventKind::created) {
            replacements.insert({event.tick, event.unit, event.owner});
            if (event.owner < stream.players.size() && !stream.players[event.owner].watcher)
                combatant[event.owner] = true;
        }
    std::vector<uint8_t> seats;
    std::vector<int> numbers;
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot)
        if (combatant[slot]) {
            seats.push_back(slot);
            numbers.push_back(stream.players[slot].number);
        }

    // The deaths of one tick are settled together once the tick is over.
    const auto settle = [&](uint32_t tick) {
        Alliances alliances;
        for (const Note& note : stream.notes)
            if (note.kind == NoteKind::alliance && note.tick <= tick)
                alliances.set(note.player, note.other, note.value != 0);
        const std::vector<int> team = alliances.teams(numbers);
        std::set<int> alive;
        for (std::size_t seat = 0; seat < seats.size(); ++seat)
            if (live[seats[seat]] != 0)
                alive.insert(team[seat]);
        found.decided = alive.size() <= 1;
        if (alive.size() == 1)
            for (std::size_t seat = 0; seat < seats.size(); ++seat)
                if (team[seat] == *alive.begin())
                    found.winners.push_back(numbers[seat]);
    };
    bool lost_this_tick = false;
    uint32_t tick = 0;
    for (const director::TimelineEvent& event : timeline.events) {
        if (lost_this_tick && event.tick != tick && !found.decided)
            settle(tick);
        if (event.tick != tick)
            lost_this_tick = false;
        tick = event.tick;
        if (!commander(event))
            continue;
        if (event.kind == director::EventKind::created) {
            ++live[event.owner];
        } else if (event.kind == director::EventKind::death) {
            if (live[event.owner] != 0)
                --live[event.owner];
            if (event.damage_kind == displaced_death_kind &&
                replacements.contains({event.tick, event.unit, event.owner}))
                continue;
            const int dead = number_in_slot(stream, event.owner);
            if (dead == 0)
                continue;
            oademo::IndexMarker marker;
            marker.tick = event.tick;
            marker.kind = oademo::MarkerKind::commander_death;
            marker.player = dead;
            marker.other =
                event.other_owner < OA_PLAYER_COUNT ? number_in_slot(stream, event.other_owner) : 0;
            found.deaths.push_back(marker);
            if (combatant[event.owner])
                lost_this_tick = true;
        }
    }
    if (lost_this_tick && !found.decided)
        settle(tick);
    return found;
}

/// Returns the unit type a timeline recorded for an index.
///
/// @param timeline the timeline
/// @param index the type's index
/// @return the type, or null when the timeline did not note it
const director::TimelineUnitType* type_named(const director::Timeline& timeline, uint16_t index) {
    const std::vector<director::TimelineUnitType>& types = timeline.header.unit_types;
    const auto at = std::lower_bound(
        types.begin(),
        types.end(),
        index,
        [](const director::TimelineUnitType& type, uint16_t wanted) {
            return type.type_index < wanted;
        }
    );
    if (at == types.end() || at->type_index != index)
        return nullptr;
    return &*at;
}

/// Tells whether a death is a unit destroyed or a building lost.
///
/// @param kind the death's kind
/// @return false for a capture, a reclaim, a dismissal or a cancelled build
bool destroyed_in_battle(uint8_t kind) noexcept {
    return kind != death_kind_captured && kind != death_kind_reclaim &&
           kind != death_kind_dismissed && kind != death_kind_cancelled;
}

/// Keeps at most `most` events, spread evenly across the ones given, which
/// are already in order.
///
/// @param[in,out] events the events
/// @param most how many to keep
void keep_spread(std::vector<oademo::BattleEvent>& events, std::size_t most) {
    if (events.size() <= most)
        return;
    std::vector<oademo::BattleEvent> kept;
    kept.reserve(most);
    const std::size_t count = events.size();
    for (std::size_t place = 0; place < most; ++place)
        kept.push_back(events[place * count / most]);
    events = std::move(kept);
}

/// The weight of a fight: an engagement's health, or what a loss cost.
///
/// @param event a fight
/// @return the weight, at least 1
uint32_t fight_weight(const oademo::BattleEvent& event) noexcept {
    if (event.kind == oademo::BattleKind::engagement && event.amount > 0)
        return static_cast<uint32_t>(event.amount);
    if (event.metal != 0)
        return event.metal;
    if (event.health != 0)
        return event.health;
    return 1;
}

/// Tells whether one fight should be shown instead of another.
///
/// @param event the candidate
/// @param best the one held so far
/// @return true when the candidate is heavier, or as heavy and earlier
bool heavier_fight(const oademo::BattleEvent& event, const oademo::BattleEvent& best) noexcept {
    const uint32_t weight = fight_weight(event);
    const uint32_t held = fight_weight(best);
    if (weight != held)
        return weight > held;
    if (event.tick != best.tick)
        return event.tick < best.tick;
    if (event.x != best.x)
        return event.x < best.x;
    return event.z < best.z;
}

/// The column or row of a map pixel. A negative place shares the first cell.
///
/// @param place the pixel
/// @return the cell
int32_t battle_cell(int32_t place) noexcept {
    if (place <= 0)
        return 0;
    return place / engagement_cell_pixels;
}

/// Keeps the heaviest fight in each square of engagement_cell_pixels and each
/// stretch of engagement_period_ticks.
///
/// @param[in,out] fights the fights, in any order
void keep_heaviest_cell(std::vector<oademo::BattleEvent>& fights) {
    struct Key {
        uint32_t period{};
        int32_t column{};
        int32_t row{};

        bool operator<(const Key& other) const noexcept {
            return std::tie(period, column, row) < std::tie(other.period, other.column, other.row);
        }
    };

    std::map<Key, oademo::BattleEvent> best;
    for (const oademo::BattleEvent& event : fights) {
        const Key key{
            event.tick / engagement_period_ticks, battle_cell(event.x), battle_cell(event.z)
        };
        const auto found = best.find(key);
        if (found == best.end() || heavier_fight(event, found->second))
            best.insert_or_assign(key, event);
    }
    fights.clear();
    fights.reserve(best.size());
    for (const auto& [key, event] : best) {
        (void)key;
        fights.push_back(event);
    }
}

/// Tells whether a commander creation is the one a replacement displaced.
///
/// The creation is left out when a displaced death of the same tick, unit
/// and owner is recorded, and either another commander of that player was
/// created on that tick or a later creation uses the same unit.
///
/// @param timeline the timeline
/// @param event the creation
/// @param displaced displaced deaths, as tick, unit and owner
/// @return true when the creation is left out
bool displaced_creation(
    const director::Timeline& timeline,
    const director::TimelineEvent& event,
    const std::set<std::tuple<uint32_t, uint16_t, uint8_t>>& displaced
) {
    if (!displaced.contains({event.tick, event.unit, event.owner}))
        return false;
    for (const director::TimelineEvent& other : timeline.events) {
        if (other.kind != director::EventKind::created ||
            (other.flags & director::event_flag::commander) == 0 || other.owner != event.owner)
            continue;
        if (other.tick == event.tick && other.unit != event.unit)
            return true;
        if (other.unit == event.unit && &other > &event)
            return true;
    }
    return false;
}

/// Orders battle events by tick, then kind, then place.
///
/// @param left one event
/// @param right the other
/// @return true when left comes first
bool battle_before(const oademo::BattleEvent& left, const oademo::BattleEvent& right) noexcept {
    if (left.tick != right.tick)
        return left.tick < right.tick;
    if (left.kind != right.kind)
        return static_cast<uint8_t>(left.kind) < static_cast<uint8_t>(right.kind);
    if (left.x != right.x)
        return left.x < right.x;
    return left.z < right.z;
}

/// Fills the index's battle events from the timeline.
///
/// A commander displaced and replaced on the same tick is left out, its
/// creation and its loss. Hits are gathered into a square of
/// engagement_cell_pixels over engagement_period_ticks, and a square becomes
/// one engagement when the hits take engagement_damage health or there are
/// engagement_hits of them. The heaviest fight in each of those squares and
/// stretches is kept. Commanders are kept, and what remains is spread across
/// the game so the file stays within most_battle_events.
///
/// @param source what the index is built from
/// @param[in,out] index the index; its events are set
void fill_battle_events(const Source& source, oademo::TimelineIndex& index) {
    const director::Timeline& timeline = *source.timeline;
    const StreamNotes& stream = *source.stream;
    std::set<std::tuple<uint32_t, uint16_t, uint8_t>> replacements;
    std::set<std::tuple<uint32_t, uint16_t, uint8_t>> displaced;
    for (const director::TimelineEvent& event : timeline.events) {
        if (event.kind == director::EventKind::created &&
            (event.flags & director::event_flag::commander) != 0)
            replacements.insert({event.tick, event.unit, event.owner});
        if (event.kind == director::EventKind::death &&
            (event.flags & director::event_flag::commander) != 0 &&
            event.damage_kind == displaced_death_kind)
            displaced.insert({event.tick, event.unit, event.owner});
    }

    std::vector<oademo::BattleEvent> commanders;
    std::vector<oademo::BattleEvent> fights;

    struct Cluster {
        uint32_t period{};
        int32_t column{};
        int32_t row{};

        bool operator<(const Cluster& other) const noexcept {
            return std::tie(period, column, row) < std::tie(other.period, other.column, other.row);
        }
    };

    struct Pile {
        int64_t damage{};
        int64_t across{};
        int64_t down{};
        int hits{};
        uint32_t tick{};
    };

    std::map<Cluster, Pile> piles;
    for (const director::TimelineEvent& event : timeline.events) {
        if (event.kind == director::EventKind::created &&
            (event.flags & director::event_flag::commander) != 0) {
            if (displaced_creation(timeline, event, displaced))
                continue;
            oademo::BattleEvent placed;
            placed.tick = event.tick;
            placed.kind = oademo::BattleKind::commander;
            placed.x = event.at.x;
            placed.z = director::ground_row(event.at);
            placed.player = number_in_slot(stream, event.owner);
            commanders.push_back(placed);
            continue;
        }
        if (event.kind == director::EventKind::death && destroyed_in_battle(event.damage_kind)) {
            const bool commander = (event.flags & director::event_flag::commander) != 0;
            if (commander && event.damage_kind == displaced_death_kind &&
                replacements.contains({event.tick, event.unit, event.owner}))
                continue;
            oademo::BattleEvent placed;
            placed.tick = event.tick;
            placed.x = event.at.x;
            placed.z = director::ground_row(event.at);
            placed.player = number_in_slot(stream, event.owner);
            if (commander) {
                placed.kind = oademo::BattleKind::commander_lost;
                commanders.push_back(placed);
            } else {
                const director::TimelineUnitType* type = type_named(timeline, event.unit_type);
                const bool mobile = type != nullptr && type->mobile;
                placed.kind =
                    mobile ? oademo::BattleKind::unit_lost : oademo::BattleKind::building_lost;
                if (type != nullptr) {
                    placed.metal = type->metal_cost;
                    placed.health = type->max_damage;
                }
                fights.push_back(placed);
            }
            continue;
        }
        if (event.kind != director::EventKind::damage || event.amount <= 0 ||
            event.damage_kind == damage_kind_healing || event.damage_kind == damage_kind_paralyze)
            continue;
        const int32_t row = director::ground_row(event.at);
        Cluster cluster;
        cluster.period = event.tick / engagement_period_ticks;
        cluster.column = event.at.x / engagement_cell_pixels;
        cluster.row = row / engagement_cell_pixels;
        Pile& pile = piles[cluster];
        if (pile.hits == 0)
            pile.tick = event.tick;
        pile.damage += event.amount;
        pile.across += static_cast<int64_t>(event.at.x) * event.amount;
        pile.down += static_cast<int64_t>(row) * event.amount;
        ++pile.hits;
    }
    for (const auto& [cluster, pile] : piles) {
        (void)cluster;
        if (pile.damage < engagement_damage && pile.hits < engagement_hits)
            continue;
        if (pile.damage <= 0)
            continue;
        oademo::BattleEvent placed;
        placed.tick = pile.tick;
        placed.kind = oademo::BattleKind::engagement;
        const int64_t across = pile.across / pile.damage;
        const int64_t down = pile.down / pile.damage;
        placed.x = static_cast<int32_t>(std::clamp(
            across,
            static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
            static_cast<int64_t>(std::numeric_limits<int32_t>::max())
        ));
        placed.z = static_cast<int32_t>(std::clamp(
            down,
            static_cast<int64_t>(std::numeric_limits<int32_t>::min()),
            static_cast<int64_t>(std::numeric_limits<int32_t>::max())
        ));
        placed.amount = pile.damage > std::numeric_limits<int32_t>::max()
                            ? std::numeric_limits<int32_t>::max()
                            : static_cast<int32_t>(pile.damage);
        fights.push_back(placed);
    }

    std::stable_sort(commanders.begin(), commanders.end(), battle_before);
    keep_heaviest_cell(fights);
    std::stable_sort(fights.begin(), fights.end(), battle_before);
    std::size_t room = oademo::most_battle_events;
    if (commanders.size() >= room) {
        keep_spread(commanders, room);
        fights.clear();
    } else {
        room -= commanders.size();
        keep_spread(fights, room);
    }
    index.events = std::move(commanders);
    index.events.insert(index.events.end(), fights.begin(), fights.end());
    std::stable_sort(index.events.begin(), index.events.end(), battle_before);
}

} // namespace

std::optional<StreamNotes> read_stream_notes(std::span<const uint8_t> file, std::string& error) {
    std::vector<uint8_t> unpacked;
    std::span<const uint8_t> recording = file;
    if (oademo::recognised(file)) {
        oademo::ReadResult replay = oademo::read(file);
        if (!replay.ok()) {
            error = replay.error;
            return std::nullopt;
        }
        unpacked = std::move(replay.replay->recording);
        recording = unpacked;
    }
    const tad::ParseResult parsed = tad::parse(recording);
    if (!parsed.ok()) {
        error = "the recording does not parse: " + parsed.error->message;
        return std::nullopt;
    }
    const tad::Demo& demo = *parsed.demo;
    tad::RecordForms forms = tad::RecordForms::narrow;
    if (const tad::Sector* sector = tad::find_sector(demo, tad::SectorType::limits);
        sector != nullptr)
        if (const std::optional<tad::RecordedLimits> limits = tad::decode_limits_sector(*sector);
            limits.has_value() && limits->weapon_types > tad::recorded_base_weapon_types)
            forms = tad::RecordForms::wide;

    StreamNotes notes;
    std::vector<Seat> seats;
    std::array<bool, 256> listed{};
    for (const tad::Player& player : demo.players) {
        const oa::session::demo::RecordedPlayer recorded =
            oa::session::demo::demo_recorded_player(demo, player);
        Seat seat;
        seat.number = player.number;
        seat.id = recorded.from_status ? recorded.info.player_id : player.number;
        seat.unicode_chat =
            recorded.from_status &&
            oa::netgame::announces_unicode_chat(reinterpret_cast<const uint8_t*>(&recorded.info));
        seats.push_back(seat);
        listed[player.number] = true;
        StreamPlayer seated;
        seated.number = player.number;
        seated.name = oa::present::decode_game_text(player.name, false);
        seated.watcher = recorded.from_status
                             ? (recorded.info.options & OA_SETUP_OPTION_WATCHER) != 0
                             : player.side == watcher_side;
        notes.players.push_back(std::move(seated));
    }

    // A packet is due on the last tick its unit states carry, or the tick of
    // the packet before it when it carries none; a packet with no game
    // record is skipped (demo_load).
    uint32_t due_tick = 0;
    for (const tad::Packet& packet : demo.packets) {
        if (!listed[packet.sender])
            continue;
        const tad::DecodedPayload decoded = tad::decode_payload(packet.payload);
        if (!decoded.ok())
            continue;
        const tad::RecordSplit split = tad::split_records(decoded.bytes, forms);
        uint32_t last_tick = 0;
        uint32_t game_records = 0;
        for (const tad::Record& record : split.records) {
            const bool wide =
                forms == tad::RecordForms::wide && netgame::is_wide_record_type(record.type);
            if (netgame::is_record_type(record.type) || wide) {
                ++game_records;
            } else if (
                (record.type == static_cast<uint8_t>(tad::RecordType::elided_unit_state) ||
                 record.type == static_cast<uint8_t>(tad::RecordType::empty_tick)) &&
                record.tick.has_value()
            ) {
                ++game_records;
            } else {
                continue;
            }
            if (record.tick.has_value())
                last_tick = std::max(last_tick, *record.tick);
        }
        if (game_records == 0)
            continue;
        if (last_tick != 0)
            due_tick = last_tick;
        for (const tad::Record& record : split.records)
            if (netgame::is_record_type(record.type))
                note_record(seats, packet.sender, due_tick, record.bytes, notes.notes);
    }
    return notes;
}

oademo::TimelineIndex build_index(const Source& source) {
    const director::Timeline& timeline = *source.timeline;
    const StreamNotes& stream = *source.stream;
    oademo::TimelineIndex index;
    index.first_tick = timeline.header.first_tick;
    index.length_ticks = timeline.header.last_tick;
    index.ended_tick = timeline.header.last_tick;
    if (source.length_ms != 0)
        index.length_ms = source.length_ms;
    index.verdict.finished = timeline.verdict.finished;
    index.verdict.clean = timeline.verdict.clean;
    index.verdict.paced = timeline.verdict.paced;
    index.verdict.content_differs = timeline.verdict.content_differs;
    index.verdict.errors = timeline.verdict.errors;
    index.verdict.first_error_tick = timeline.verdict.first_error_tick;
    index.verdict.last_error = timeline.verdict.last_error;

    const std::size_t samples = (index.length_ticks > index.first_tick
                                     ? (index.length_ticks - index.first_tick) / index.period_ticks
                                     : 0) +
                                1;
    fill_totals(source, samples, index);
    fill_damage(timeline, samples, index);

    std::vector<Ranked> ranked;
    const auto add = [&ranked](oademo::IndexMarker marker, uint32_t rank = 1) {
        ranked.push_back(Ranked{std::move(marker), rank});
    };
    oademo::IndexMarker start;
    start.tick = index.first_tick;
    start.kind = oademo::MarkerKind::start;
    add(std::move(start), 0);

    const Commanders commanders = follow_commanders(source);
    for (const oademo::IndexMarker& marker : commanders.deaths)
        add(marker);
    if (commanders.decided) {
        index.result.winners = commanders.winners;
        if (!commanders.winners.empty())
            index.result.by = oademo::ResultBy::commanders;
    }
    // A campaign mission's own end names the recording player's result. It
    // replaces whatever the commanders decided, and only once the replay
    // reached the end of the recording. A game that is not a campaign never
    // sets Source::mission, so its result stays with the commanders.
    if (timeline.verdict.finished && source.mission != Source::Mission::none &&
        source.mission_player > 0) {
        index.result.winners.clear();
        index.result.by = oademo::ResultBy::mission;
        if (source.mission == Source::Mission::won) {
            index.result.winners.push_back(source.mission_player);
        } else {
            for (const StreamPlayer& player : stream.players)
                if (!player.watcher && player.number != source.mission_player && player.number > 0)
                    index.result.winners.push_back(player.number);
            // Nobody else plays, and an empty list would show no result.
            if (index.result.winners.empty())
                index.result.winners.push_back(0);
        }
    }

    bool paused = false;
    Alliances alliances;
    std::array<bool, 256> departed{};
    bool end_of_game = false;
    bool departures = false;
    for (const Note& note : stream.notes) {
        oademo::IndexMarker marker;
        marker.tick = note.tick;
        marker.player = note.player;
        switch (note.kind) {
        case NoteKind::chat:
            if (!typed_by_a_player(note.text) || oa::netgame::chat_command(note.text))
                continue;
            marker.kind = oademo::MarkerKind::chat;
            marker.text = note.text;
            break;
        case NoteKind::pause:
            if (note.tick < opening_ticks || (note.value != 0) == paused)
                continue;
            paused = note.value != 0;
            marker.kind = oademo::MarkerKind::pause;
            marker.flag = paused;
            break;
        case NoteKind::rejected:
        case NoteKind::disconnected: {
            oademo::MarkerKind kind = oademo::MarkerKind::player_left;
            if (note.kind == NoteKind::rejected && !departure_kind(note.value, kind))
                continue;
            if (note.other == 0 || departed[static_cast<std::size_t>(note.other)])
                continue;
            departed[static_cast<std::size_t>(note.other)] = true;
            departures = true;
            marker.kind = kind;
            marker.player = note.other;
            break;
        }
        case NoteKind::alliance: {
            if (!alliances.set(note.player, note.other, note.value != 0))
                continue;
            marker.kind = oademo::MarkerKind::alliance;
            marker.other = note.other;
            marker.flag = alliances.mutual(note.player, note.other);
            break;
        }
        case NoteKind::end_of_game:
            end_of_game = true;
            continue;
        }
        add(std::move(marker));
    }

    oademo::IndexMarker end;
    end.tick = index.length_ticks;
    end.kind = oademo::MarkerKind::end;
    add(std::move(end), 2);
    std::stable_sort(ranked.begin(), ranked.end(), [](const Ranked& a, const Ranked& b) {
        return a.marker.tick != b.marker.tick ? a.marker.tick < b.marker.tick : a.rank < b.rank;
    });
    std::array<std::size_t, marker_kind_count> kept{};
    for (Ranked& item : ranked) {
        const auto kind = static_cast<std::size_t>(item.marker.kind);
        if (kept[kind] >= oademo::most_markers_of_a_kind) {
            ++index.markers_omitted;
            continue;
        }
        ++kept[kind];
        index.markers.push_back(std::move(item.marker));
    }

    if (!timeline.verdict.finished)
        index.ended = oademo::IndexEnd::cut;
    else if (commanders.decided)
        index.ended = oademo::IndexEnd::finished;
    else if (departures)
        index.ended = oademo::IndexEnd::left;
    else if (end_of_game)
        index.ended = oademo::IndexEnd::finished;
    else if (source.recorded_end.has_value())
        index.ended = *source.recorded_end;
    else
        index.ended = oademo::IndexEnd::cut;
    index.version = oademo::timeline_version;
    fill_battle_events(source, index);
    return index;
}

BattleAim battle_aim(std::span<const oademo::BattleEvent> events, uint32_t tick) {
    const auto in_stretch = [&](uint32_t from, uint32_t to) -> const oademo::BattleEvent* {
        const oademo::BattleEvent* best = nullptr;
        for (const oademo::BattleEvent& event : events) {
            if (!oademo::battle_fight(event.kind) || event.tick < from || event.tick > to)
                continue;
            if (best == nullptr || heavier_fight(event, *best))
                best = &event;
        }
        return best;
    };
    const auto later = [](uint32_t from, uint32_t by) {
        return from > std::numeric_limits<uint32_t>::max() - by
                   ? std::numeric_limits<uint32_t>::max()
                   : from + by;
    };
    // The nearest fight in a stretch, by how far its tick is from the moment.
    // An equal tick keeps the heavier fight.
    const auto nearest = [&](uint32_t from, uint32_t to) -> const oademo::BattleEvent* {
        const oademo::BattleEvent* best = nullptr;
        uint32_t best_gap = 0;
        for (const oademo::BattleEvent& event : events) {
            if (!oademo::battle_fight(event.kind) || event.tick < from || event.tick > to)
                continue;
            const uint32_t gap = event.tick >= tick ? event.tick - tick : tick - event.tick;
            if (best == nullptr || gap < best_gap ||
                (gap == best_gap && heavier_fight(event, *best))) {
                best = &event;
                best_gap = gap;
            }
        }
        return best;
    };
    // The latest fight in a stretch. The same tick keeps the heavier fight.
    const auto most_recent = [&](uint32_t from, uint32_t to) -> const oademo::BattleEvent* {
        const oademo::BattleEvent* best = nullptr;
        for (const oademo::BattleEvent& event : events) {
            if (!oademo::battle_fight(event.kind) || event.tick < from || event.tick > to)
                continue;
            if (best == nullptr || event.tick > best->tick ||
                (event.tick == best->tick && heavier_fight(event, *best)))
                best = &event;
        }
        return best;
    };
    const uint32_t ahead = later(tick, battle_look_ahead_ticks);
    const oademo::BattleEvent* chosen = in_stretch(tick, ahead);
    if (chosen == nullptr && tick > 0) {
        const uint32_t trail = tick > battle_trail_ticks ? tick - battle_trail_ticks : 0;
        chosen = most_recent(trail, tick - 1);
    }
    // A minute with no fight in the short windows still shows the battle,
    // not the place the commanders started.
    if (chosen == nullptr) {
        const uint32_t ahead_from =
            ahead == std::numeric_limits<uint32_t>::max() ? ahead : ahead + 1;
        const oademo::BattleEvent* forward = nearest(ahead_from, later(tick, battle_lull_ticks));
        const oademo::BattleEvent* back = nullptr;
        if (tick > battle_trail_ticks) {
            const uint32_t behind = tick > battle_lull_ticks ? tick - battle_lull_ticks : 0;
            back = nearest(behind, tick - battle_trail_ticks - 1);
        }
        if (forward == nullptr)
            chosen = back;
        else if (back == nullptr)
            chosen = forward;
        else {
            const uint32_t forward_gap = forward->tick - tick;
            const uint32_t back_gap = tick - back->tick;
            chosen = forward_gap <= back_gap ? forward : back;
        }
    }
    BattleAim aim;
    if (chosen == nullptr)
        return aim;
    aim.found = true;
    aim.x = chosen->x;
    aim.z = chosen->z;
    aim.tick = chosen->tick;
    return aim;
}

namespace {

/// Tells whether two places are within `pixels` of each other.
///
/// @param ax one place, across the map
/// @param az one place, down the map
/// @param bx the other, across the map
/// @param bz the other, down the map
/// @param pixels the reach
/// @return true when the distance is within the reach
bool near_place(int32_t ax, int32_t az, int32_t bx, int32_t bz, int32_t pixels) noexcept {
    const int64_t across = static_cast<int64_t>(ax) - bx;
    const int64_t down = static_cast<int64_t>(az) - bz;
    const int64_t reach = pixels;
    return across * across + down * down <= reach * reach;
}

/// Tells whether the fight on show is still the fight of the moment.
///
/// A fight older than the trail stretch no longer holds the picture against
/// a newer one, however heavy it was. The quiet minute only finds a place to
/// go when nothing nearer in time is fighting.
///
/// @param view the view
/// @param tick the moment on show
/// @return true while that fight's tick is in the look-ahead or the trail stretch
bool subject_in_range(const DirectorView& view, uint32_t tick) noexcept {
    if (view.subject_tick >= tick)
        return view.subject_tick - tick <= battle_look_ahead_ticks;
    return tick - view.subject_tick <= battle_trail_ticks;
}

/// The fight battle_aim chose, or none.
///
/// @param events the events
/// @param tick the moment on show
/// @return the fight; null when battle_aim finds none
const oademo::BattleEvent*
fight_chosen(std::span<const oademo::BattleEvent> events, uint32_t tick) {
    const BattleAim aim = battle_aim(events, tick);
    if (!aim.found)
        return nullptr;
    const oademo::BattleEvent* chosen = nullptr;
    for (const oademo::BattleEvent& event : events) {
        if (!oademo::battle_fight(event.kind) || event.tick != aim.tick || event.x != aim.x ||
            event.z != aim.z)
            continue;
        if (chosen == nullptr || heavier_fight(event, *chosen))
            chosen = &event;
    }
    return chosen;
}

/// Moves the picture from where the pan started toward the fight.
///
/// The ease is a smoothstep. Its fastest moment stays under one screen a
/// second at 1×. A cut, whose start is the fight, stays there.
///
/// @param[in,out] view the view
/// @param since_cut_ms real time since the pan or cut started
/// @param view_width the picture's width, in map pixels
void ease_pan(DirectorView& view, uint32_t since_cut_ms, int32_t view_width) noexcept {
    if (!view.has_subject)
        return;
    const double across =
        static_cast<double>(view.subject_x) - static_cast<double>(view.pan_from_x);
    const double down = static_cast<double>(view.subject_z) - static_cast<double>(view.pan_from_z);
    const double distance = std::sqrt(across * across + down * down);
    if (distance <= 0.5 || view_width <= 0) {
        view.x = view.subject_x;
        view.z = view.subject_z;
        return;
    }
    // Smoothstep peaks at 1.5 times the average speed. The duration keeps
    // that peak under 0.85 of a screen a second.
    const double screen = static_cast<double>(view_width);
    const double duration_ms = std::max(1.0, 1.5 * distance / (screen * 0.85) * 1000.0);
    const uint32_t elapsed =
        since_cut_ms >= view.pan_origin_ms ? since_cut_ms - view.pan_origin_ms : 0;
    const double along = std::min(1.0, static_cast<double>(elapsed) / duration_ms);
    const double eased = along * along * (3.0 - 2.0 * along);
    view.x =
        static_cast<int32_t>(std::lround(static_cast<double>(view.pan_from_x) + across * eased));
    view.z = static_cast<int32_t>(std::lround(static_cast<double>(view.pan_from_z) + down * eased));
}

} // namespace

BattleAim director_opening(std::span<const oademo::BattleEvent> events) {
    const oademo::BattleEvent* first = nullptr;
    const auto rank = [](const oademo::BattleEvent& event) {
        return std::tuple(
            event.tick,
            event.player > 0 ? event.player : std::numeric_limits<int>::max(),
            event.x,
            event.z
        );
    };
    for (const oademo::BattleEvent& event : events) {
        if (event.kind != oademo::BattleKind::commander)
            continue;
        if (first == nullptr || rank(event) < rank(*first))
            first = &event;
    }
    BattleAim aim;
    if (first == nullptr)
        return aim;
    aim.found = true;
    aim.x = first->x;
    aim.z = first->z;
    aim.tick = first->tick;
    return aim;
}

DirectorView director_view(
    std::span<const oademo::BattleEvent> events,
    uint32_t tick,
    uint32_t since_cut_ms,
    const DirectorView& previous,
    int32_t view_width,
    int32_t view_height
) {
    DirectorView view = previous;
    view.changed = false;
    view.cut = false;
    const oademo::BattleEvent* candidate = fight_chosen(events, tick);
    if (candidate == nullptr) {
        ease_pan(view, since_cut_ms, view_width);
        return view;
    }
    const uint32_t weight = fight_weight(*candidate);
    const bool same =
        view.has_subject &&
        near_place(
            view.subject_x, view.subject_z, candidate->x, candidate->z, director_same_place_pixels
        );
    if (same) {
        // The fight continued nearby. Ease across from the picture as it is.
        // The hold clock stays, so a fast playback cannot strobe on the slide.
        const bool moved = view.subject_x != candidate->x || view.subject_z != candidate->z;
        view.subject_x = candidate->x;
        view.subject_z = candidate->z;
        view.subject_tick = candidate->tick;
        view.subject_weight = weight;
        if (moved) {
            view.pan_from_x = view.x;
            view.pan_from_z = view.z;
            view.pan_origin_ms = since_cut_ms;
        }
        ease_pan(view, since_cut_ms, view_width);
        return view;
    }
    const bool in_range = view.has_subject && subject_in_range(view, tick);
    const bool outweighs =
        !view.has_subject ||
        static_cast<uint64_t>(weight) * director_switch_denominator >=
            static_cast<uint64_t>(view.subject_weight) * director_switch_numerator;
    const bool game_ready = !view.has_subject || (tick >= view.cut_tick &&
                                                  tick - view.cut_tick >= director_min_cut_ticks);
    const bool wall_ready = !view.has_subject || since_cut_ms >= director_hold_ms;
    bool take = !view.has_subject || (game_ready && wall_ready && (!in_range || outweighs));
    if (take && view.has_subject && view.left && tick >= view.left_tick &&
        tick - view.left_tick <= director_return_ticks &&
        near_place(
            candidate->x, candidate->z, view.left_x, view.left_z, director_same_place_pixels
        ))
        take = false;
    if (!take) {
        ease_pan(view, since_cut_ms, view_width);
        return view;
    }
    const bool first = !view.has_subject;
    const int32_t from_x = view.x;
    const int32_t from_z = view.z;
    const double across = static_cast<double>(candidate->x) - static_cast<double>(from_x);
    const double down = static_cast<double>(candidate->z) - static_cast<double>(from_z);
    const double distance = std::sqrt(across * across + down * down);
    const double pan_limit =
        view_width > 0 ? static_cast<double>(view_width) * director_pan_reach_percent / 100.0 : 0.0;
    const int32_t screen = std::max(view_width, view_height);
    const double two_screens = screen > 0 ? static_cast<double>(screen) * 2.0 : 0.0;
    const bool pan = !first && view_width > 0 && distance <= pan_limit && distance <= two_screens;
    if (!first) {
        view.left = true;
        view.left_x = from_x;
        view.left_z = from_z;
        view.left_tick = tick;
    }
    view.has_subject = true;
    view.subject_x = candidate->x;
    view.subject_z = candidate->z;
    view.subject_tick = candidate->tick;
    view.subject_weight = weight;
    view.cut_tick = tick;
    view.changed = true;
    view.cut = !pan;
    view.pan_from_x = pan ? from_x : candidate->x;
    view.pan_from_z = pan ? from_z : candidate->z;
    view.pan_origin_ms = 0;
    view.x = pan ? from_x : candidate->x;
    view.z = pan ? from_z : candidate->z;
    return view;
}

} // namespace oa::app::replay_index
