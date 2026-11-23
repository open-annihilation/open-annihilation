// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/session/demo.hpp"

#include "demo_source.hpp"

#include "oa/formats/oademo/oademo.hpp"
#include "oa/netgame/match/launch.hpp"
#include "oa/netgame/private_channel.hpp"
#include "oa/netgame/records.hpp"
#include "oa/netgame/recorder_messages.hpp"
#include "oa/netgame/route_batch.hpp"
#include "oa/netgame/unit_state.hpp"
#include "oa/netgame/network.hpp"
#include "oa/base/text.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstring>
#include <exception>
#include <fstream>
#include <iterator>
#include <span>
#include <unordered_set>

namespace oa::session::demo {
namespace {

constexpr std::size_t player_name_capacity = sizeof(Player::name) - 1;
constexpr double fixed_one = 65536.0;
constexpr uint64_t fnv_offset = 0xcbf29ce484222325ull;
constexpr uint64_t fnv_prime = 0x100000001b3ull;

bool fail(std::string* error, std::string message) {
    if (error != nullptr)
        *error = std::move(message);
    return false;
}

void put_u32(uint8_t* out, uint32_t value) {
    out[0] = static_cast<uint8_t>(value);
    out[1] = static_cast<uint8_t>(value >> 8);
    out[2] = static_cast<uint8_t>(value >> 16);
    out[3] = static_cast<uint8_t>(value >> 24);
}

// An empty tick (0xff) stands for that tick's 0x2c with no unit entry and an
// empty full-record slot: every recorded packet holds one unit state or
// empty tick per sender tick, and no stored 0xfd has that minimal form.
bool append_empty_unit_state(uint32_t tick, unsigned def_bits, std::vector<uint8_t>* frame) {
    constexpr std::size_t words = 4;
    uint8_t storage[words * netgame::bit_stream_word_bytes]{};
    netgame::BitWriter writer;
    netgame::bit_writer_init(&writer, storage, words);
    netgame::unit_state_begin(&writer, tick);
    uint16_t length = 0;
    if (netgame::unit_state_finish(&writer, netgame::FullUnitRecord{}, def_bits, &length) !=
        netgame::WireError::ok)
        return false;
    frame->insert(frame->end(), storage, storage + length);
    return true;
}

/// Appends the route batch records of a recorded packet to a rebuilt frame.
///
/// A recording keeps each route batch record (0xe4, 0xe5) in a packet of its
/// own, as it keeps every record but a 0x2c, and each is sized by the length
/// it carries.
///
/// @param records the packet's records, after its payload marker
/// @param[in,out] frame the frame being rebuilt
/// @param[out] last_tick the latest tick a part names, 0 for none
/// @param[in,out] stats counts the records kept
/// @return false, appending nothing, when the packet holds anything but whole route batch records
bool append_route_batch_records(
    std::span<const uint8_t> records,
    std::vector<uint8_t>* frame,
    uint32_t* last_tick,
    PlaybackStats* stats
) {
    uint32_t tick = 0;
    uint32_t count = 0;
    for (std::size_t at = 0; at < records.size();) {
        uint16_t length = 0;
        if (netgame::route_batch_record_length(records.data() + at, records.size() - at, &length) !=
            netgame::WireError::ok)
            return false;
        netgame::RouteBatchPartHeader header{};
        if (records[at] == static_cast<uint8_t>(netgame::RouteBatchRecordType::part) &&
            netgame::decode_route_batch_part_header(records.data() + at, length, &header) ==
                netgame::WireError::ok)
            tick = std::max(tick, header.tick);
        at += length;
        ++count;
    }
    frame->insert(frame->end(), records.begin(), records.end());
    *last_tick = tick;
    stats->route_batch_records += count;
    return count != 0;
}

// A recorded packet as the frame the packet layer would have received:
// sequence dword, then the game records with unit states restored. Reports
// the last sender tick among its unit states, restored or stored whole, or
// among its route batch parts (0 when none).
bool rebuild_frame(
    const formats::tad::Packet& packet,
    unsigned def_bits,
    formats::tad::RecordForms forms,
    std::vector<uint8_t>* frame,
    uint32_t* last_tick,
    PlaybackStats* stats
) {
    frame->assign(netgame::frame_header_bytes, 0);
    *last_tick = 0;
    uint32_t game_records = 0;
    const auto decoded = formats::tad::decode_payload(packet.payload);
    if (!decoded.ok()) {
        ++stats->bad_packets;
        return false;
    }
    if (decoded.bytes.size() > 1 && netgame::is_route_batch_record_type(decoded.bytes[1])) {
        const std::span<const uint8_t> records(decoded.bytes.data() + 1, decoded.bytes.size() - 1);
        if (append_route_batch_records(records, frame, last_tick, stats))
            return true;
        ++stats->bad_packets;
        return false;
    }
    const auto split = formats::tad::split_records(decoded.bytes, forms);
    if (split.error)
        ++stats->bad_packets;
    for (const auto& record : split.records) {
        const bool wide =
            forms == formats::tad::RecordForms::wide && netgame::is_wide_record_type(record.type);
        if (netgame::is_record_type(record.type) || wide) {
            frame->insert(frame->end(), record.bytes.begin(), record.bytes.end());
            if (wide)
                ++stats->wide_records;
            if (record.tick)
                *last_tick = std::max(*last_tick, *record.tick);
            ++game_records;
            continue;
        }
        const bool elided =
            record.type == static_cast<uint8_t>(formats::tad::RecordType::elided_unit_state);
        const bool empty =
            record.type == static_cast<uint8_t>(formats::tad::RecordType::empty_tick);
        if (!elided && !empty) {
            ++stats->recorder_records;
            continue;
        }
        if (!record.tick) {
            ++stats->untimed_unit_states;
            continue;
        }
        if (elided) {
            const auto state = formats::tad::expand_unit_state(record);
            if (state.empty()) {
                ++stats->bad_packets;
                continue;
            }
            frame->insert(frame->end(), state.begin(), state.end());
        } else {
            if (!append_empty_unit_state(*record.tick, def_bits, frame)) {
                ++stats->bad_packets;
                continue;
            }
            ++stats->empty_ticks;
        }
        *last_tick = std::max(*last_tick, *record.tick);
        ++game_records;
    }
    return game_records != 0;
}

// The lobby block of the 0x20 record in a player's status datagram.
bool status_player_info(const formats::tad::PlayerStatus& status, PlayerSetupInfo* info) {
    const auto unwrapped = oa::netgame::network::unwrap_frame(status.datagram);
    if (!unwrapped.ok())
        return false;
    const auto& frame = *unwrapped.value;
    for (std::size_t offset = netgame::frame_header_bytes; offset < frame.size();) {
        const auto length =
            formats::tad::record_length({frame.data() + offset, frame.size() - offset});
        if (length == 0)
            return false;
        if (frame[offset] == static_cast<uint8_t>(netgame::RecordType::player_info)) {
            static_assert(sizeof(PlayerSetupInfo) == netgame::player_info_block_bytes);
            std::memcpy(info, frame.data() + offset + 1, sizeof(PlayerSetupInfo));
            return true;
        }
        offset += length;
    }
    return false;
}

// The recorded 0x1a records reach the watcher's table as they reach a
// joined client, which seeds nothing that could keep a type: its own records
// start with the remote side clear.
void receive_unit_checks(DemoPlayback* playback) {
    playback->battleroom = std::make_unique<ui::frontend_multiplayer::Lobby>();
    auto& battleroom = *playback->battleroom;
    ui::frontend_multiplayer::unit_sync_create(battleroom, false);
    for (const auto& bytes : formats::tad::unit_check_records(playback->demo)) {
        netgame::UnitDefHandshakeRecord record{};
        if (netgame::decode_record(bytes.data(), bytes.size(), &record) != netgame::WireError::ok)
            continue;
        if (record.subtype == static_cast<uint8_t>(netgame::HandshakeSubtype::verdict))
            ++playback->recorded_verdicts;
        ui::frontend_multiplayer::unit_sync_receive(battleroom, bytes, 0);
    }
}

// The FBI hashes of the recorded game's unit table: the types the verdicts
// gave both sides, as every machine of the game kept them; without verdicts
// the recorder's own subtype-2 list, else every key the records name.
std::unordered_set<uint32_t> recorded_unit_table(const DemoPlayback& playback) {
    std::unordered_set<uint32_t> recorded;
    if (const auto* sync = demo_unit_sync(playback)) {
        for (int32_t index = 0; index < sync->record_count; ++index) {
            const auto& record = sync->records[index];
            if (record.local != 0 && record.remote != 0)
                recorded.insert(record.key);
        }
        return recorded;
    }
    std::unordered_set<uint32_t> any_subtype;
    for (const auto& bytes : formats::tad::unit_check_records(playback.demo)) {
        netgame::UnitDefHandshakeRecord record{};
        if (netgame::decode_record(bytes.data(), bytes.size(), &record) != netgame::WireError::ok)
            continue;
        any_subtype.insert(record.key);
        if (record.subtype == static_cast<uint8_t>(netgame::HandshakeSubtype::def_checksum))
            recorded.insert(record.key);
    }
    return recorded.empty() ? any_subtype : recorded;
}

// Each recorded player's lobby block and net id; the watcher's id sorts
// after all of them, as free slots' -1 does after the watcher.
bool bind_recorded_players(DemoPlayback* playback, std::string* error) {
    std::array<bool, sender_count> seen{};
    uint32_t highest = 0;
    for (const auto& player : playback->demo.players) {
        if (player.number == 0 || seen[player.number])
            return fail(
                error,
                "demo player number " + std::to_string(player.number) +
                    " cannot address a transport"
            );
        seen[player.number] = true;
        const RecordedPlayer recorded = demo_recorded_player(playback->demo, player);
        const uint32_t id = recorded.from_status ? recorded.info.player_id : player.number;
        if (id == 0 || id >= netgame::match::no_player_id - 1)
            return fail(
                error, "demo player " + std::to_string(player.number) + " has no usable id"
            );
        for (const auto& other : playback->demo.players)
            if (other.number != player.number && playback->sender_ids[other.number] == id)
                return fail(error, "demo players share id " + std::to_string(id));
        playback->sender_ids[player.number] = id;
        highest = std::max(highest, id);
        playback->players.push_back(recorded);
    }
    playback->watcher_id = highest + 1u;
    return true;
}

const DemoFrame* pending_frame(DemoPlayback* playback);
void take_pending_frame(DemoPlayback* playback);

uint32_t transport_send(void* context, uint32_t, uint32_t, uint32_t, const uint8_t*, uint32_t) {
    ++static_cast<DemoPlayback*>(context)->stats.sends_dropped;
    return netgame::transport_result::ok;
}

uint32_t
transport_receive(void* context, uint32_t* from, uint32_t* to, uint8_t* buffer, uint32_t* size) {
    auto* playback = static_cast<DemoPlayback*>(context);
    while (!demo_exhausted(*playback)) {
        const DemoFrame* frame = pending_frame(playback);
        if (frame == nullptr)
            return netgame::transport_result::no_messages;
        if (frame->due_tick > playback->tick)
            return netgame::transport_result::no_messages;
        if (frame->datagram.size() > *size) {
            ++playback->stats.frames_oversized;
            take_pending_frame(playback);
            continue;
        }
        std::memcpy(buffer, frame->datagram.data(), frame->datagram.size());
        *size = static_cast<uint32_t>(frame->datagram.size());
        *from = playback->sender_ids[frame->sender];
        *to = netgame::broadcast_destination_id;
        take_pending_frame(playback);
        ++playback->stats.frames_delivered;
        return netgame::transport_result::ok;
    }
    return netgame::transport_result::no_messages;
}

DemoSession& session_of(void* context) {
    return *static_cast<DemoSession*>(context);
}

// Chat text arrives with its "<name> " prefix.
void note_chat(void* context, uint8_t, const char* text) {
    static_cast<DemoSession*>(context)->lines.emplace_back(text != nullptr ? text : "");
}

void note_notice(void* context, const char* text) {
    static_cast<DemoSession*>(context)->lines.emplace_back(text != nullptr ? text : "");
}

// The recorded player a record came from: a remote slot under its recorded id.
Player* recorded_sender(World& world, uint32_t sender_id) {
    for (auto& player : world.game.players)
        if (player.in_use != 0 && player.status == OA_PLAYER_STATUS_MIRRORED &&
            player.player_id == sender_id)
            return &player;
    return nullptr;
}

// Records the match pump admitted, before they are dispatched. A viewer
// without a slot of its own is shown each recorded chat line, which the
// pump shows only to a local slot; under watcher-view each recorded player's
// economy records give it income figures.
void note_record(void* context, uint32_t sender_id, const uint8_t* record, std::size_t size) {
    auto& session = *static_cast<DemoSession*>(context);
    if (session.match == nullptr || record == nullptr || size == 0)
        return;
    auto& world = session.match->state();
    auto* sender = recorded_sender(world, sender_id);
    if (sender == nullptr || sender->index >= OA_PLAYER_COUNT)
        return;
    if (demo_note_shown_camera(&session.shown_camera[sender->index], record, size)) {
        if (session.shown_camera[sender->index].shared)
            session.recording_shares_camera = true;
        return;
    }
    const auto type = static_cast<netgame::RecordType>(record[0]);
    if (type == netgame::RecordType::chat && demo_watches_without_slot(session.playback)) {
        if (session.net.rules.private_channel != netgame::PrivateChannel::none &&
            netgame::is_private_chat(record, size))
            return;
        char text[sizeof(netgame::ChatRecord::text) + 1]{};
        std::memcpy(text, record + 1, std::min(size - 1, sizeof(netgame::ChatRecord::text)));
        session.lines.push_back(netgame::match::net_match_chat_text(&session.net, *sender, text));
        return;
    }
    if (type == netgame::RecordType::economy) {
        netgame::EconomyRecord economy{};
        if (netgame::decode_record(record, size, &economy) != netgame::WireError::ok)
            return;
        demo_income_for_display(
            &session.shown_samples[sender->index],
            &session.shown_income[sender->index],
            economy,
            session.playback.tick
        );
        if (session.playback.ten_player_replay == netgame::TenPlayerReplay::watcher_view)
            demo_follow_economy(
                &session.economy_samples[sender->index], sender, economy, session.playback.tick
            );
    }
}

uint32_t horizontal_distance(const FixedVec3& a, const FixedVec3& b) {
    const double dx = (static_cast<double>(a.x) - b.x) / fixed_one;
    const double dz = (static_cast<double>(a.z) - b.z) / fixed_one;
    return static_cast<uint32_t>(std::sqrt(dx * dx + dz * dz));
}

/// Returns the tick of the record that carries a full record being applied.
///
/// A unit state and a route batch set their sender's last_sim_tick to the
/// tick they carry before their full record applies; the sender is the
/// player whose range of units holds the slot.
///
/// @param world the replay's world
/// @param slot the unit slot the full record is for
/// @return the sender's tick, or 0 when no player's range holds the slot
uint32_t sender_tick_of(const World& world, uint32_t slot) {
    for (const auto& player : world.game.players) {
        if (player.in_use == 0 || player.first_unit == 0 || player.last_unit == 0)
            continue;
        if (slot >= oa_unit_slot_from_ref(player.first_unit) &&
            slot <= oa_unit_slot_from_ref(player.last_unit))
            return static_cast<uint32_t>(player.last_sim_tick);
    }
    return 0;
}

void note_full_record(
    void* context, const Unit& unit, const FixedVec3& before, const FixedVec3& to
) {
    auto* session = static_cast<DemoSession*>(context);
    const auto* world = session->net.world;
    if (world == nullptr || unit.type_index == 0)
        return;
    const auto slot = world_unit_slot(world, &unit);
    if (slot >= session->placed_type.size())
        return;
    ++session->full_records.placements;
    if (session->placed_type[slot] == unit.type_index) {
        const auto* def = world_unit_def_of(world, &unit);
        const bool flier = def != nullptr && (def->flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0;
        auto& stats = flier ? session->full_records.air : session->full_records.ground;
        const auto& previous = session->placed_at[slot];
        const auto drift = horizontal_distance(before, to);
        ++stats.measured;
        stats.moved += previous.x != to.x || previous.z != to.z ? 1u : 0u;
        stats.drift_sum += drift;
        stats.drift_max = std::max(stats.drift_max, drift);
    }
    session->placed_type[slot] = unit.type_index;
    session->placed_at[slot] = to;
    if (slot == session->tracked_unit)
        session->tracked.push_back({world->game.tick, sender_tick_of(*world, slot), before, to});
}

// A slot emptied since its last full record starts a new unit.
void forget_empty_slots(DemoSession* session) {
    const auto* world = session->net.world;
    if (world == nullptr)
        return;
    const auto count = std::min<std::size_t>(session->placed_type.size(), world->unit_slot_count);
    for (std::size_t slot = 1; slot < count; ++slot)
        if (world->units[slot].type_index == 0)
            session->placed_type[slot] = 0;
}

void mix(uint64_t* hash, const void* bytes, std::size_t size) {
    const auto* p = static_cast<const uint8_t*>(bytes);
    for (std::size_t i = 0; i < size; ++i) {
        *hash ^= p[i];
        *hash *= fnv_prime;
    }
}

// Takes a copy: the fields hashed sit in packed records, where a reference
// to one may be misaligned.
template <class T>
void mix_value(uint64_t* hash, T value) {
    mix(hash, &value, sizeof value);
}

// The file's player order is the playback slot. -1 when the number is none of them.
int recorded_player_slot(const formats::tad::Demo& demo, uint8_t sender) {
    const auto& players = demo.players;
    for (std::size_t index = 0; index < players.size() && index < OA_PLAYER_COUNT; ++index)
        if (players[index].number == sender)
            return static_cast<int>(index);
    return -1;
}

// Camera records ride in the file and are left out of the rebuilt game frame.
void keep_packet_cameras(
    const formats::tad::Demo& demo,
    const formats::tad::Packet& packet,
    uint32_t tick,
    std::vector<RecordedCamera>& cameras
) {
    const int player_slot = recorded_player_slot(demo, packet.sender);
    if (player_slot < 0)
        return;
    const auto decoded = formats::tad::decode_payload(packet.payload);
    if (!decoded.ok())
        return;
    const auto split = formats::tad::split_records(decoded.bytes);
    for (const auto& record : split.records) {
        ShownCamera shown{};
        if (!demo_note_shown_camera(&shown, record.bytes.data(), record.bytes.size()) ||
            !shown.seen)
            continue;
        RecordedCamera kept{};
        kept.slot = static_cast<uint8_t>(player_slot);
        kept.tick = tick;
        kept.x = shown.x;
        kept.y = shown.y;
        kept.shared = shown.shared;
        cameras.push_back(kept);
    }
}

// The latest camera at or before the match's tick, for each recorded player.
void apply_recorded_cameras(DemoSession* session) {
    if (session->match == nullptr)
        return;
    const uint32_t played_tick = session->match->state().game.tick;
    for (const RecordedCamera& camera : session->playback.cameras) {
        if (camera.tick > played_tick || camera.slot >= OA_PLAYER_COUNT)
            continue;
        ShownCamera& shown = session->shown_camera[camera.slot];
        shown.seen = true;
        shown.x = camera.x;
        shown.y = camera.y;
        shown.shared = camera.shared;
        if (camera.shared)
            session->recording_shares_camera = true;
    }
}

/// The most bytes before a recording's first packet: the header, the sector
/// count, 256 sectors, ten players and their statuses, and the unit checks,
/// each a chunk of at most 65,535 bytes.
constexpr std::size_t most_head_bytes =
    (2 + formats::tad::limit::sectors + 2 * formats::tad::limit::players + 1) *
    formats::tad::limit::chunk_bytes;

uint16_t load_u16(const uint8_t* at) noexcept {
    return static_cast<uint16_t>(at[0] | (at[1] << 8));
}

/// Finds where a recording's packets start, walking its chunks as a whole
/// parse reads them: the header, the sector count, the sectors, the players,
/// their statuses and the unit checks.
///
/// @param bytes the recording's first bytes, its part before the packets whole
/// @return the offset of the first packet's chunk; 0 when the chunks before
///         it are not whole or do not read, which a parse then says why
std::size_t packets_start(std::span<const uint8_t> bytes) {
    namespace layout = formats::tad::layout;
    std::size_t at = 0;
    // The length of the whole chunk at a position; 0 when it is not all there.
    const auto chunk_at = [&bytes](std::size_t position) -> std::size_t {
        if (bytes.size() - position < layout::chunk_length_bytes)
            return 0;
        const std::size_t length = load_u16(bytes.data() + position);
        if (length < layout::chunk_length_bytes || length > bytes.size() - position)
            return 0;
        return length;
    };
    const std::size_t header = chunk_at(at);
    if (header < layout::chunk_length_bytes + layout::header_fixed_bytes)
        return 0;
    const std::size_t player_count =
        bytes[at + layout::chunk_length_bytes + layout::magic_bytes + 2];
    if (player_count > formats::tad::limit::players)
        return 0;
    at += header;
    const std::size_t counted = chunk_at(at);
    if (counted != layout::chunk_length_bytes + layout::sector_count_bytes)
        return 0;
    const std::size_t sectors = static_cast<std::size_t>(load_u16(bytes.data() + at + 2)) |
                                (static_cast<std::size_t>(load_u16(bytes.data() + at + 4)) << 16);
    if (sectors > formats::tad::limit::sectors)
        return 0;
    at += counted;
    for (std::size_t index = 0; index < sectors + player_count * 2 + 1; ++index) {
        const std::size_t length = chunk_at(at);
        if (length == 0)
            return 0;
        at += length;
    }
    return at;
}

/// What a recorded packet gave when its frame was made.
enum class Made : uint8_t {
    frame,          ///< a frame
    unknown_sender, ///< a number no player chunk names; dropped
    empty,          ///< no game record
    bad,            ///< a frame that does not encode
};

/// The record forms a playback's packets are split with.
formats::tad::RecordForms forms_of(const DemoPlayback& playback) noexcept {
    return playback.limits &&
                   playback.limits->weapon_types > formats::tad::recorded_base_weapon_types
               ? formats::tad::RecordForms::wide
               : formats::tad::RecordForms::narrow;
}

/// Makes the frame a recorded packet gives, as the packet layer would have
/// received it, and moves on what carries to the next packet.
///
/// A frame is due once the local tick reaches the last sender tick it
/// carries; frames without unit states follow the one before them.
///
/// @param playback the playback, its players bound
/// @param packet the packet
/// @param[in,out] making the due tick and sequences that carry from packet to packet
/// @param[in,out] stats counts what the packet held
/// @param[in,out] frame room the frame is rebuilt in
/// @param[out] out the frame, when one is made
/// @param cameras where the packet's cameras are kept; null keeps none
/// @return what the packet gave
Made make_frame(
    const DemoPlayback& playback,
    const formats::tad::Packet& packet,
    FrameMaking& making,
    PlaybackStats& stats,
    std::vector<uint8_t>& frame,
    DemoFrame& out,
    std::vector<RecordedCamera>* cameras
) {
    // Transport id 0 would make the frame a system message.
    if (playback.sender_ids[packet.sender] == 0) {
        ++stats.unknown_senders;
        return Made::unknown_sender;
    }
    uint32_t last_tick = 0;
    const auto route_batch_records = stats.route_batch_records;
    const bool built = rebuild_frame(
        packet, playback.unit_def_bits, forms_of(playback), &frame, &last_tick, &stats
    );
    // A part sent again carries the tick of its batch, which can lie
    // before the frames already due: it is due with them.
    if (last_tick != 0)
        making.due_tick = stats.route_batch_records != route_batch_records
                              ? std::max(making.due_tick, last_tick)
                              : last_tick;
    if (cameras != nullptr)
        keep_packet_cameras(playback.demo, packet, making.due_tick, *cameras);
    if (!built) {
        ++stats.empty_packets;
        return Made::empty;
    }
    auto& next = making.sequence[packet.sender];
    put_u32(frame.data(), static_cast<uint32_t>(next));
    out.time_ms = packet.time_ms;
    out.sender = packet.sender;
    out.due_tick = making.due_tick;
    try {
        out.datagram = oa::netgame::network::encode_frame(frame, false);
    } catch (const std::exception&) {
        ++stats.bad_packets;
        return Made::bad;
    }
    next = netgame::next_frame_sequence(next);
    return Made::frame;
}

/// The carry a recording's first packet starts from.
FrameMaking first_making() {
    FrameMaking making;
    making.sequence.fill(netgame::first_broadcast_frame_sequence);
    return making;
}

/// Makes the delivery's next frame, reading on until a packet gives one.
///
/// @param[in,out] playback the playback
/// @param[out] out the frame
/// @return false when no frame is left, or the recording cannot be read
bool make_next_frame(DemoPlayback* playback, DemoFrame& out) {
    PlaybackSource& source = *playback->source;
    if (!source.failure.empty())
        return false;
    if (source.reader == nullptr) {
        std::unique_ptr<RecordingBytes> bytes;
        if (std::string why_not = source.open_bytes(bytes); !why_not.empty()) {
            source.failure = why_not;
            return false;
        }
        source.reader = std::make_unique<PacketReader>(std::move(bytes));
        if (std::string why_not = source.reader->start_at(source.packets_at, 0); !why_not.empty()) {
            source.failure = why_not;
            return false;
        }
        source.making = first_making();
        source.next_frame = 0;
    }
    PlaybackStats counted;
    std::vector<uint8_t> frame;
    for (;;) {
        formats::tad::Packet packet{};
        bool ended = false;
        if (std::string why_not = source.reader->next(packet, ended); !why_not.empty()) {
            source.failure = why_not;
            return false;
        }
        if (ended) {
            // The load counted more: the recording changed under the playback.
            if (source.next_frame < playback->frame_count)
                source.failure = "the recording ends before its last frame";
            return false;
        }
        if (make_frame(*playback, packet, source.making, counted, frame, out, nullptr) ==
            Made::frame) {
            ++source.next_frame;
            return true;
        }
    }
}

/// Returns the frame the delivery takes next, making it when it is not made yet.
///
/// @param[in,out] playback the playback
/// @return the frame; null when none is left or the recording cannot be read
const DemoFrame* pending_frame(DemoPlayback* playback) {
    if (playback->source == nullptr)
        return nullptr;
    PlaybackSource& source = *playback->source;
    if (!source.pending) {
        DemoFrame frame;
        if (!make_next_frame(playback, frame))
            return nullptr;
        source.pending = std::move(frame);
    }
    return &*source.pending;
}

/// Takes the pending frame: it was delivered, or dropped.
///
/// @param[in,out] playback the playback
void take_pending_frame(DemoPlayback* playback) {
    if (playback->source != nullptr)
        playback->source->pending.reset();
    ++playback->cursor;
}

/// Tells whether a playback's recording is a replay, read through its frame.
bool from_replay(const PlaybackSource& source) noexcept {
    return source.origin == PlaybackSource::Origin::held_replay ||
           source.origin == PlaybackSource::Origin::file_replay;
}

/// Reads the recording's packets once: counts the frames they make and what
/// they hold, keeps their cameras, and keeps a place to make frames again
/// from every seek_point_frames frames.
///
/// @param[in,out] playback the playback, its players bound
/// @param[in,out] reader the packets, from the first
/// @param[out] error why the packets do not read
/// @return false when they do not
bool count_frames(DemoPlayback* playback, PacketReader& reader, std::string* error) {
    PlaybackSource& source = *playback->source;
    FrameMaking making = first_making();
    std::vector<uint8_t> frame;
    DemoFrame made;
    uint64_t frames = 0;
    source.seek_points.clear();
    for (;;) {
        if (frames % seek_point_frames == 0 &&
            (source.seek_points.empty() || source.seek_points.back().frames != frames)) {
            SeekPoint point;
            point.frames = frames;
            point.offset = reader.offset();
            point.time_ms = reader.time_ms();
            point.due_tick = making.due_tick;
            for (std::size_t index = 0;
                 index < playback->demo.players.size() && index < point.sequence.size();
                 ++index)
                point.sequence[index] = making.sequence[playback->demo.players[index].number];
            source.seek_points.push_back(point);
        }
        formats::tad::Packet packet{};
        bool ended = false;
        if (std::string why_not = reader.next(packet, ended); !why_not.empty()) {
            // A replay whose digest is not its description's says so first, as a whole read does.
            if (from_replay(source)) {
                if (std::string at_end = reader.drain(); !at_end.empty())
                    return fail(error, at_end);
            }
            return fail(error, why_not);
        }
        if (ended)
            break;
        playback->duration_ms = packet.time_ms;
        if (make_frame(
                *playback, packet, making, playback->stats, frame, made, &playback->cameras
            ) != Made::frame)
            continue;
        ++frames;
        ++playback->stats.frames;
        playback->last_due_tick = made.due_tick;
    }
    playback->frame_count = static_cast<std::size_t>(frames);
    return true;
}

/// Puts the delivery where frame `target` is made next.
///
/// Forward, it reads on; backward, it reads again from the last seek point
/// at or before the frame.
///
/// @param[in,out] playback the playback
/// @param target the frame
/// @return false when the recording cannot be read to it
bool deliver_from(DemoPlayback* playback, uint64_t target) {
    if (playback->source == nullptr)
        return target == 0;
    PlaybackSource& source = *playback->source;
    if (source.pending && source.next_frame == target + 1)
        return true;
    source.pending.reset();
    source.failure.clear();
    if (source.reader == nullptr || source.next_frame > target) {
        std::unique_ptr<RecordingBytes> bytes;
        if (std::string why_not = source.open_bytes(bytes); !why_not.empty()) {
            source.failure = why_not;
            return false;
        }
        const SeekPoint* from = nullptr;
        for (const SeekPoint& point : source.seek_points)
            if (point.frames <= target)
                from = &point;
        source.reader = std::make_unique<PacketReader>(std::move(bytes));
        source.making = first_making();
        source.next_frame = 0;
        std::string why_not;
        if (from != nullptr) {
            why_not = source.reader->start_at(from->offset, from->time_ms);
            source.making.due_tick = from->due_tick;
            for (std::size_t index = 0;
                 index < playback->demo.players.size() && index < from->sequence.size();
                 ++index)
                source.making.sequence[playback->demo.players[index].number] =
                    from->sequence[index];
            source.next_frame = from->frames;
        } else {
            why_not = source.reader->start_at(source.packets_at, 0);
        }
        if (!why_not.empty()) {
            source.failure = why_not;
            return false;
        }
    }
    DemoFrame dropped;
    while (source.next_frame < target)
        if (!make_next_frame(playback, dropped))
            return false;
    return true;
}

/// Refuses a recording, after reading its packets through: a recording whose
/// packets do not read, or a replay whose digest is not its description's,
/// says so first, as a whole parse and a whole read would.
///
/// @param[in,out] reader the packets, from the first
/// @param[out] error why the recording cannot be played
/// @param message the refusal when the packets read
/// @return false
bool fail_after_packets(PacketReader& reader, std::string* error, std::string message) {
    for (;;) {
        formats::tad::Packet packet{};
        bool ended = false;
        if (std::string why_not = reader.next(packet, ended); !why_not.empty()) {
            if (std::string at_end = reader.drain(); !at_end.empty())
                return fail(error, at_end);
            return fail(error, why_not);
        }
        if (ended)
            return fail(error, std::move(message));
    }
}

/// Reads a playback's recording from where its origin says.
///
/// @param[in,out] playback the playback; emptied first
/// @param source where the recording is
/// @param[out] error why it cannot be played
/// @return false when it cannot
bool load_source(
    DemoPlayback* playback, std::shared_ptr<PlaybackSource> source, std::string* error
) {
    *playback = DemoPlayback{};
    playback->source = std::move(source);
    const bool replay = from_replay(*playback->source);
    std::unique_ptr<RecordingBytes> bytes;
    if (std::string why_not = playback->source->open_bytes(bytes); !why_not.empty())
        return fail(error, why_not);
    // The part before the packets, read until it is whole: it is never more than most_head_bytes.
    std::vector<uint8_t> first;
    std::size_t packets_at = 0;
    bool bytes_ended = false;
    for (std::size_t want = std::size_t{64} << 10; packets_at == 0 && !bytes_ended;
         want = std::min(want * 2, most_head_bytes)) {
        const std::size_t have = first.size();
        first.resize(want);
        std::size_t filled = have;
        while (filled < want) {
            std::size_t got = 0;
            if (std::string why_not = bytes->read(std::span(first).subspan(filled), got);
                !why_not.empty())
                return fail(error, why_not);
            if (got == 0) {
                bytes_ended = true;
                break;
            }
            filled += got;
        }
        first.resize(filled);
        packets_at = packets_start(first);
        if (want == most_head_bytes)
            break;
    }
    PacketReader reader(std::move(bytes));
    reader.start_with(packets_at, 0, std::span(first).subspan(packets_at));
    const auto refuse_parse = [&](const formats::tad::ParseResult& parsed) {
        // A replay whose digest is not its description's says so first, as a whole read does.
        if (replay) {
            if (std::string at_end = reader.drain(); !at_end.empty())
                return fail(error, at_end);
        }
        if (parsed.ok())
            return fail(
                error, "demo parse failed at offset 0: the part before the packets does not read"
            );
        return fail(
            error,
            "demo parse failed at offset " + std::to_string(parsed.error->offset) + ": " +
                parsed.error->message
        );
    };
    if (packets_at == 0)
        return refuse_parse(formats::tad::parse(first));
    playback->file.assign(first.begin(), first.begin() + static_cast<std::ptrdiff_t>(packets_at));
    first = {};
    playback->source->packets_at = packets_at;
    auto parsed = formats::tad::parse(playback->file);
    if (!parsed.ok())
        return refuse_parse(parsed);
    playback->demo = std::move(*parsed.demo);
    if (const auto* sector =
            formats::tad::find_sector(playback->demo, formats::tad::SectorType::limits)) {
        if (const auto decoded = formats::tad::decode_limits_sector(*sector))
            playback->limits = *decoded;
        else
            playback->limits_unreadable = true;
    }
    // A whole parse read the packets before the players were judged: a recording whose packets
    // do not read still says so first.
    if (playback->demo.players.empty())
        return fail_after_packets(reader, error, "demo has no players");
    std::string unbound;
    if (!bind_recorded_players(playback, &unbound))
        return fail_after_packets(reader, error, std::move(unbound));
    receive_unit_checks(playback);
    const auto recorded = recorded_unit_table(*playback);
    if (recorded.empty())
        return fail_after_packets(reader, error, "demo announces no unit definitions");
    playback->recorded_definitions = static_cast<uint32_t>(recorded.size());
    // The game sizes the 0x2c index from the compacted table, slot 0 included.
    playback->unit_def_bits = static_cast<unsigned>(
        netgame::match::unit_def_id_bits_for_count(playback->recorded_definitions + 1u)
    );
    return count_frames(playback, reader, error);
}

} // namespace

RecordedPlayer
demo_recorded_player(const formats::tad::Demo& demo, const formats::tad::Player& player) {
    RecordedPlayer recorded;
    recorded.info.side = player.side;
    recorded.info.color = player.color;
    for (const auto& status : demo.statuses)
        if (status.number == player.number && status_player_info(status, &recorded.info)) {
            recorded.from_status = true;
            break;
        }
    return recorded;
}

bool demo_recognised(std::span<const uint8_t> bytes) noexcept {
    if (formats::oademo::recognised(bytes))
        return true;
    // The header is the first chunk: its length field, then the magic.
    constexpr std::size_t magic_at = formats::tad::layout::chunk_length_bytes;
    if (bytes.size() < magic_at + formats::tad::layout::magic_bytes)
        return false;
    for (std::size_t i = 0; i < formats::tad::layout::magic_bytes; ++i)
        if (bytes[magic_at + i] != static_cast<uint8_t>(formats::tad::magic[i]))
            return false;
    return true;
}

bool demo_open(DemoPlayback* playback, const std::filesystem::path& path, std::string* error) {
    // The first bytes say whether the file is a replay; the recording is read from the file as
    // playback reaches it, never whole.
    std::array<uint8_t, 64> start{};
    std::size_t got = 0;
    {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            // In UTF-8: a recording named outside the system's code page has no
            // narrow spelling on Windows.
            const auto name = path.u8string();
            return fail(error, "cannot open demo: " + std::string(name.begin(), name.end()));
        }
        in.read(reinterpret_cast<char*>(start.data()), static_cast<std::streamsize>(start.size()));
        got = static_cast<std::size_t>(std::max<std::streamsize>(in.gcount(), 0));
    }
    auto source = std::make_shared<PlaybackSource>();
    source->path = path;
    source->origin = formats::oademo::recognised(std::span(start).first(got))
                         ? PlaybackSource::Origin::file_replay
                         : PlaybackSource::Origin::file_tad;
    return load_source(playback, std::move(source), error);
}

bool demo_load(DemoPlayback* playback, std::vector<uint8_t> bytes, std::string* error) {
    auto source = std::make_shared<PlaybackSource>();
    // The mission's unit table is decided when playback loads the map the
    // stream names, not from a replay's description. A .tad has none, and a
    // recording packed back from one says it was imported.
    source->origin = formats::oademo::recognised(bytes) ? PlaybackSource::Origin::held_replay
                                                        : PlaybackSource::Origin::held_tad;
    source->held = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    return load_source(playback, std::move(source), error);
}

std::vector<DemoFrame> demo_frames(const DemoPlayback& playback) {
    std::vector<DemoFrame> frames;
    if (playback.source == nullptr)
        return frames;
    // A playback of its own over the same recording, so this one's delivery is not moved.
    DemoPlayback reading;
    reading.sender_ids = playback.sender_ids;
    reading.unit_def_bits = playback.unit_def_bits;
    reading.limits = playback.limits;
    reading.demo.players = playback.demo.players;
    auto source = std::make_shared<PlaybackSource>();
    source->origin = playback.source->origin;
    source->path = playback.source->path;
    source->packets_at = playback.source->packets_at;
    source->held = playback.source->held;
    reading.source = std::move(source);
    reading.frame_count = playback.frame_count;
    frames.reserve(playback.frame_count);
    while (frames.size() < playback.frame_count) {
        const DemoFrame* frame = pending_frame(&reading);
        if (frame == nullptr)
            break;
        frames.push_back(*frame);
        take_pending_frame(&reading);
    }
    return frames;
}

DemoLimitsVerdict
demo_limits_verdict(const DemoPlayback& playback, uint32_t weapon_types, uint32_t sides) noexcept {
    if (playback.limits_unreadable)
        return DemoLimitsVerdict::unreadable;
    if (playback.limits) {
        return playback.limits->weapon_types == weapon_types && playback.limits->sides == sides
                   ? DemoLimitsVerdict::plays
                   : DemoLimitsVerdict::other_limits;
    }
    if (weapon_types == formats::tad::recorded_base_weapon_types &&
        sides == formats::tad::recorded_base_sides)
        return DemoLimitsVerdict::plays;
    return DemoLimitsVerdict::base_limits;
}

netgame::NetTransport demo_transport(DemoPlayback* playback) noexcept {
    return netgame::NetTransport{playback, transport_send, transport_receive};
}

bool demo_exhausted(const DemoPlayback& playback) noexcept {
    return playback.cursor >= playback.frame_count ||
           (playback.source != nullptr && !playback.source->failure.empty());
}

uint64_t demo_duration_ms(const DemoPlayback& playback) noexcept {
    return playback.duration_ms;
}

bool demo_deliver_from(DemoPlayback* playback, std::size_t cursor) {
    if (playback == nullptr || cursor > playback->frame_count)
        return false;
    if (!deliver_from(playback, cursor))
        return false;
    playback->cursor = cursor;
    return true;
}

std::string demo_read_failure(const DemoPlayback& playback) {
    return playback.source != nullptr ? playback.source->failure : std::string();
}

const ui::frontend_multiplayer::UnitSync* demo_unit_sync(const DemoPlayback& playback) noexcept {
    if (playback.recorded_verdicts == 0 || !playback.battleroom)
        return nullptr;
    return &playback.battleroom->sync;
}

UnitTableCheck demo_check_unit_table(
    const DemoPlayback& playback,
    std::span<const uint32_t> local_keys,
    std::vector<std::size_t>* extra_indices
) {
    const auto recorded = recorded_unit_table(playback);
    UnitTableCheck check;
    check.recorded = static_cast<uint32_t>(recorded.size());
    std::unordered_set<uint32_t> local(local_keys.begin(), local_keys.end());
    for (const auto key : recorded)
        if (local.contains(key))
            ++check.matched;
    check.missing = check.recorded - check.matched;
    for (std::size_t i = 0; i < local_keys.size(); ++i) {
        if (recorded.contains(local_keys[i]))
            continue;
        ++check.extra;
        if (extra_indices != nullptr)
            extra_indices->push_back(i);
    }
    check.identical =
        check.missing == 0 && check.extra == 0 && local_keys.size() == recorded.size();
    return check;
}

bool demo_watches_without_slot(const DemoPlayback& playback) noexcept {
    return playback.demo.players.size() == OA_PLAYER_COUNT &&
           playback.ten_player_replay != netgame::TenPlayerReplay::off;
}

sim::match_runtime::SlotlessViewer demo_slotless_viewer(const DemoPlayback& playback) noexcept {
    using sim::match_runtime::SlotlessViewer;
    if (!demo_watches_without_slot(playback))
        return SlotlessViewer::none;
    return playback.ten_player_replay == netgame::TenPlayerReplay::allied_fake_player
               ? SlotlessViewer::ally_of_every_player
               : SlotlessViewer::watcher;
}

void demo_follow_economy(
    EconomySample* sample, Player* player, const netgame::EconomyRecord& record, uint32_t tick
) noexcept {
    if (sample == nullptr || player == nullptr)
        return;
    if (sample->taken) {
        if (tick <= sample->tick)
            return;
        const auto per_settlement =
            static_cast<float>(economy_settlement_ticks) / static_cast<float>(tick - sample->tick);
        player->energy_produced =
            (record.energy_produced_total - sample->energy_produced_total) * per_settlement;
        player->energy_requested =
            (record.energy_requested_total - sample->energy_requested_total) * per_settlement;
        player->metal_produced =
            (record.metal_produced_total - sample->metal_produced_total) * per_settlement;
        player->metal_requested =
            (record.metal_requested_total - sample->metal_requested_total) * per_settlement;
    }
    sample->taken = true;
    sample->tick = tick;
    sample->energy_produced_total = record.energy_produced_total;
    sample->energy_requested_total = record.energy_requested_total;
    sample->metal_produced_total = record.metal_produced_total;
    sample->metal_requested_total = record.metal_requested_total;
}

void demo_income_for_display(
    EconomySample* sample, ShownIncome* shown, const netgame::EconomyRecord& record, uint32_t tick
) noexcept {
    if (sample == nullptr || shown == nullptr)
        return;
    if (sample->taken) {
        if (tick <= sample->tick)
            return;
        const auto per_settlement =
            static_cast<float>(economy_settlement_ticks) / static_cast<float>(tick - sample->tick);
        shown->metal =
            (record.metal_produced_total - sample->metal_produced_total) * per_settlement;
        shown->energy =
            (record.energy_produced_total - sample->energy_produced_total) * per_settlement;
        shown->metal_used =
            (record.metal_requested_total - sample->metal_requested_total) * per_settlement;
        shown->energy_used =
            (record.energy_requested_total - sample->energy_requested_total) * per_settlement;
        shown->taken = true;
    }
    sample->taken = true;
    sample->tick = tick;
    sample->energy_produced_total = record.energy_produced_total;
    sample->energy_requested_total = record.energy_requested_total;
    sample->metal_produced_total = record.metal_produced_total;
    sample->metal_requested_total = record.metal_requested_total;
}

bool demo_note_shown_camera(ShownCamera* camera, const uint8_t* record, std::size_t size) noexcept {
    if (camera == nullptr || record == nullptr || size < netgame::recorder_camera_bytes)
        return false;
    if (record[0] != static_cast<uint8_t>(netgame::RecorderRecordType::camera))
        return false;
    const auto word = [](const uint8_t* bytes) {
        return static_cast<uint16_t>(bytes[0] | (static_cast<uint16_t>(bytes[1]) << 8));
    };
    const uint16_t x = word(record + 1);
    const uint16_t y = word(record + 3);
    camera->seen = true;
    camera->x = x;
    camera->y = y;
    camera->shared = x != netgame::recorder_camera_off || y != netgame::recorder_camera_off;
    return true;
}

bool demo_bind_players(const DemoPlayback& playback, World* world, uint8_t* watcher_slot) noexcept {
    const auto count = playback.demo.players.size();
    const bool slotless = demo_watches_without_slot(playback);
    if (world == nullptr || count == 0 || (count >= OA_PLAYER_COUNT && !slotless) ||
        playback.players.size() != count)
        return false;
    auto& game = world->game;
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        auto& player = game.players[slot];
        auto& info = world->player_info[slot];
        player.in_use = 0;
        player.player_id = netgame::match::no_player_id;
        player.status = OA_PLAYER_STATUS_FREE;
        player.index = slot;
        player.info = oa_ref_from_index(slot);
        player.reject_reason = 0;
        player.machine_group = 0;
        player.team = OA_PLAYER_NO_TEAM;
        std::memset(player.name, 0, sizeof player.name);
        std::memset(player.alliance, 0, sizeof player.alliance);
        std::memset(player.allied_by, 0, sizeof player.allied_by);
        info = PlayerSetupInfo{};
        if (slot < count) {
            const auto& recorded = playback.demo.players[slot];
            info = playback.players[slot].info;
            player.in_use = 1;
            player.player_id = playback.sender_ids[recorded.number];
            player.status = OA_PLAYER_STATUS_MIRRORED;
            oa::base::text::copy_padded(player.name, recorded.name.c_str(), player_name_capacity);
        } else if (slot == count) {
            player.in_use = 1;
            player.player_id = playback.watcher_id;
            player.status = OA_PLAYER_STATUS_LOCAL;
            oa::base::text::copy_padded(player.name, "Watcher", player_name_capacity);
            info.player_id = playback.watcher_id;
            info.options = OA_SETUP_OPTION_WATCHER;
        }
        player.alliance[slot] = 1;
        player.allied_by[slot] = 1;
    }
    *watcher_slot = static_cast<uint8_t>(count);
    // Without a slot of its own the viewer looks through the first recorded
    // player's, which stays remote: nothing is simulated or sent for it here.
    const uint8_t view = slotless ? 0 : *watcher_slot;
    game.local_player_index = view;
    game.viewpoint_player = view;
    game.player_count = static_cast<uint16_t>(slotless ? count : count + 1);
    if (const auto* host = netgame::match::launch_host_info(world))
        netgame::match::match_apply_host_options(world, *host);
    // The viewer watches, seated or not, and sees the whole map.
    if (slotless)
        netgame::match::match_show_whole_map(world);
    else
        netgame::match::match_apply_watcher_view(world);
    game.unit_def_id_bits = netgame::match::unit_def_id_bits_for_count(world->unit_def_count);
    game.session_flags = static_cast<uint8_t>(
        game.session_flags | netgame::match::kNetFlagLive | netgame::match::kNetFlagGameStarted
    );
    return true;
}

bool demo_session_begin(
    DemoSession* session, sim::match_runtime::Match* match, std::string* error
) {
    if (session == nullptr || match == nullptr)
        return fail(error, "demo session needs a match");
    demo_session_end(session);
    auto* world = &match->state();
    const auto limit = session->playback.demo.max_units;
    if (world->game.units_per_player != limit)
        return fail(
            error,
            "match unit limit " + std::to_string(world->game.units_per_player) +
                " differs from the recording's " + std::to_string(limit)
        );
    if (netgame::match::unit_def_id_bits_for_count(world->unit_def_count) !=
        static_cast<int32_t>(session->playback.unit_def_bits))
        return fail(
            error,
            "the recording's " + std::to_string(session->playback.recorded_definitions) +
                " unit definitions need a different index width than this match's " +
                std::to_string(world->unit_def_count - 1)
        );
    if (!demo_bind_players(session->playback, world, &session->watcher_slot))
        return fail(
            error,
            "recording has " + std::to_string(session->playback.demo.players.size()) +
                " players; playback needs a free watcher slot"
        );
    for (uint8_t slot = 0; slot <= session->watcher_slot && slot < OA_PLAYER_COUNT; ++slot) {
        std::array<uint8_t, OA_PLAYER_COUNT> own{};
        own[slot] = 1;
        match->configure_player_alliances(slot, own);
    }
    match->set_slotless_viewer(demo_slotless_viewer(session->playback));
    match->set_slotless_view_switched(false);
    session->economy_samples = {};
    session->shown_samples = {};
    session->shown_income = {};
    session->shown_camera = {};
    session->recording_shares_camera = false;
    for (const RecordedCamera& camera : session->playback.cameras)
        if (camera.shared)
            session->recording_shares_camera = true;
    session->match = match;
    session->connection = netgame::match::NetConnection{};
    session->connection.packets = new netgame::match::PacketLayer();
    netgame::match::packet_layer_create(session->connection.packets);
    netgame::match::packet_layer_start(
        session->connection.packets, demo_transport(&session->playback)
    );
    const bool play_wide =
        session->playback.limits.has_value() &&
        session->playback.limits->weapon_types > formats::tad::recorded_base_weapon_types;
    session->connection.packets->receiver.wide_records = play_wide;
    // A recording that holds route batch records plays them as the machine
    // that recorded it did; one without them splits as before.
    session->connection.packets->receiver.route_batches =
        session->playback.stats.route_batch_records != 0;
    netgame::match::match_binding_init(&session->binding, match, &session->net);
    session->binding.full_record_probe = {session, note_full_record};
    session->full_records = {};
    session->placed_type.assign(world->unit_slot_count, 0);
    session->placed_at.assign(world->unit_slot_count, FixedVec3{});
    session->tracked_unit =
        static_cast<uint16_t>(oa_unit_slot_from_ref(world->game.players[0].first_unit));
    session->tracked.clear();
    // The hooks' context is the session, for chat and notices; the
    // binding's own hooks are reached through it.
    auto hooks = netgame::match::match_binding_hooks(&session->binding);
    hooks.context = session;
    hooks.chat = note_chat;
    hooks.notice = note_notice;
    if (session->translate_game_text != nullptr)
        hooks.translate_game_text = [](void* context, const char* english) {
            const auto& viewer = session_of(context);
            return viewer.translate_game_text(viewer.game_text_context, english);
        };
    hooks.record_seen = note_record;
    hooks.destroy_player_units = [](void* context, World*, uint8_t slot) {
        netgame::match::match_binding_destroy_player_units(&session_of(context).binding, slot);
    };
    hooks.end_local_game = [](void* context) {
        netgame::match::match_binding_end_local_game(&session_of(context).binding);
    };
    hooks.local_player_won = [](void* context) {
        return netgame::match::match_binding_local_player_won(&session_of(context).binding);
    };
    hooks.alliance_changed = [](void* context, World*, uint8_t slot) {
        netgame::match::match_binding_follow_alliances(&session_of(context).binding, slot);
    };
    hooks.share_sight = [](void* context, World*, uint8_t from, uint8_t to) {
        netgame::match::match_binding_share_sight(&session_of(context).binding, from, to);
    };
    netgame::match::net_match_begin(
        &session->net,
        &session->connection,
        world,
        netgame::match::match_binding_sim(&session->binding),
        hooks
    );
    // A recording keeps each line as its machine heard it; a player whose
    // block says UTF-8 chat was heard in UTF-8.
    session->net.recorded_chat = true;
    session->net.unicode_chat = session->unicode_chat;
    netgame::match::match_binding_install(&session->binding);
    netgame::match::net_match_enter_game(&session->net);
    apply_recorded_cameras(session);
    session->tick_errors = 0;
    session->last_error.clear();
    session->lines.clear();
    // A new binding is a new game as far as keyframes go.
    session->keyframe_base.clear();
    session->keyframe_expected.reset();
    session->keyframe_faults.clear();
    return true;
}

void demo_session_frame(DemoSession* session) {
    if (session == nullptr || session->match == nullptr)
        return;
    // The binding advances the tick before it pumps the packet layer.
    session->playback.tick = session->match->state().game.tick + 1u;
    try {
        netgame::match::match_binding_tick(&session->binding);
    } catch (const std::exception& error) {
        ++session->tick_errors;
        session->last_error = error.what();
    }
    // A recorded pause does not hold playback, which would stop the frames
    // that carry its end from being delivered; the viewer's own Pause key
    // holds it instead.
    auto& game = session->match->state().game;
    game.sim_run_flags =
        static_cast<uint16_t>(game.sim_run_flags & ~netgame::match::run_flag_paused);
    forget_empty_slots(session);
    apply_recorded_cameras(session);
    // A recording that stopped reading part way, as when its file changed, ends the playback
    // there, and the session says so once.
    if (auto* source = session->playback.source.get();
        source != nullptr && !source->failure.empty() && !source->failure_told) {
        source->failure_told = true;
        ++session->tick_errors;
        session->last_error = "the recording cannot be read on: " + source->failure;
    }
    // A replay that began at a restore meets the digest the keyframe of this tick holds.
    if (session->keyframe_expected && game.tick >= session->keyframe_expected->tick) {
        const auto expected = *session->keyframe_expected;
        session->keyframe_expected.reset();
        const auto found = demo_world_digest(&session->match->state());
        if (game.tick == expected.tick && found != expected.digest)
            session->keyframe_faults.push_back({expected.tick, expected.digest, found});
    }
}

void demo_session_end(DemoSession* session) noexcept {
    if (session == nullptr)
        return;
    if (session->connection.packets != nullptr)
        netgame::match::net_connection_destroy(&session->connection);
    session->connection = netgame::match::NetConnection{};
    session->net = netgame::match::NetMatch{};
    session->binding = netgame::match::MatchBinding{};
    session->match = nullptr;
}

bool demo_session_finished(const DemoSession& session) noexcept {
    if (!demo_exhausted(session.playback))
        return false;
    const auto* packets = session.connection.packets;
    if (packets == nullptr)
        return true;
    return netgame::match::packet_layer_idle(packets);
}

DemoVerdict demo_session_verdict(const DemoSession& session, bool unit_table_differs) noexcept {
    DemoVerdict verdict;
    verdict.clean = (session.net.record_errors == 0 || unit_table_differs) &&
                    (session.binding.creates_past_table == 0 || unit_table_differs) &&
                    session.tick_errors == 0 && session.binding.refused_creates == 0;
    // The tracked unit's full records ride on every units_per_player-th
    // sender tick, counted on the sender's clock.
    const auto period = static_cast<int64_t>(
        session.match != nullptr ? session.match->state().game.units_per_player
                                 : session.playback.demo.max_units
    );
    verdict.paced = true;
    for (std::size_t i = 1; i < session.tracked.size(); ++i) {
        const auto spacing = static_cast<int64_t>(session.tracked[i].sender_tick) -
                             session.tracked[i - 1].sender_tick;
        verdict.paced = verdict.paced && spacing == period;
    }
    return verdict;
}

uint64_t demo_world_digest(const World* world) noexcept {
    uint64_t hash = fnv_offset;
    if (world == nullptr)
        return hash;
    mix_value(&hash, world->game.tick);
    for (uint32_t slot = 1; slot < world->unit_slot_count; ++slot) {
        const auto& unit = world->units[slot];
        if (unit.type_index == 0)
            continue;
        mix_value(&hash, unit.id);
        mix_value(&hash, unit.type_index);
        mix_value(&hash, unit.owner_index);
        mix_value(&hash, unit.position.x);
        mix_value(&hash, unit.position.y);
        mix_value(&hash, unit.position.z);
        mix_value(&hash, unit.heading);
        mix_value(&hash, unit.health);
        mix_value(&hash, unit.state_flags);
        const float build_remaining = unit.build_remaining;
        mix_value(&hash, std::bit_cast<uint32_t>(build_remaining));
    }
    for (const auto& player : world->game.players) {
        if (player.in_use == 0)
            continue;
        mix_value(&hash, player.index);
        const float metal = player.metal;
        const float energy = player.energy;
        mix_value(&hash, std::bit_cast<uint32_t>(metal));
        mix_value(&hash, std::bit_cast<uint32_t>(energy));
        mix_value(&hash, player.unit_count);
        mix_value(&hash, player.kills);
        mix_value(&hash, player.losses);
    }
    return hash;
}

uint32_t demo_live_units(const World* world) noexcept {
    uint32_t live = 0;
    if (world == nullptr)
        return live;
    for (uint32_t slot = 1; slot < world->unit_slot_count; ++slot)
        live += world->units[slot].type_index != 0 ? 1u : 0u;
    return live;
}

} // namespace oa::session::demo
