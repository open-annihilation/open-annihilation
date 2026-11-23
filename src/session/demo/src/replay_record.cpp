// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/session/demo/replay_record.hpp"

#include "oa/base/sha256.hpp"
#include "oa/base/zstd/zstd.hpp"
#include "oa/core/player_setup.h"
#include "oa/data/brain_library/seats.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/formats/oademo/interface_words.hpp"
#include "oa/formats/tad.hpp"
#include "oa/session/demo/place_file.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <ios>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::session::demo {

namespace {

namespace oademo = formats::oademo;
namespace tad = formats::tad;
namespace fs = std::filesystem;

/// The smallest chunk that holds a packet: the length word, the delay and the sender.
constexpr std::size_t smallest_packet_chunk =
    tad::layout::chunk_length_bytes + tad::layout::packet_fixed_bytes;

uint16_t load16(const uint8_t* at) noexcept {
    return static_cast<uint16_t>(at[0] | (at[1] << 8));
}

uint32_t load32(const uint8_t* at) noexcept {
    return static_cast<uint32_t>(at[0]) | (static_cast<uint32_t>(at[1]) << 8) |
           (static_cast<uint32_t>(at[2]) << 16) | (static_cast<uint32_t>(at[3]) << 24);
}

std::string side_name(uint8_t side) {
    if (side == static_cast<uint8_t>(tad::RecordedSide::arm))
        return "arm";
    if (side == static_cast<uint8_t>(tad::RecordedSide::core))
        return "core";
    return std::to_string(side);
}

/// Reads a file whole.
///
/// @param path the file
/// @param[out] bytes its bytes
/// @param[out] error why it could not be read
/// @param[out] transient true when the disk refused the read, false when the file is too long
/// @return false when it cannot be opened, is longer than a recording may be, or is not read whole
bool read_file(
    const fs::path& path, std::vector<uint8_t>& bytes, std::string& error, bool& transient
) {
    transient = true;
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "the file cannot be opened";
        return false;
    }
    in.seekg(0, std::ios::end);
    const std::streamoff end = in.tellg();
    if (!in || end < 0) {
        error = "the file cannot be read";
        return false;
    }
    if (static_cast<uint64_t>(end) > tad::limit::input_bytes) {
        error = "the recording is too long to finish";
        transient = false;
        return false;
    }
    in.seekg(0);
    bytes.resize(static_cast<std::size_t>(end));
    if (end > 0)
        in.read(reinterpret_cast<char*>(bytes.data()), end);
    if (!in) {
        error = "the file cannot be read";
        return false;
    }
    return true;
}

/// Writes a file whole.
///
/// @param path the file; replaced
/// @param bytes what it holds
/// @return false when it cannot be written
bool write_file(const fs::path& path, std::span<const uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    if (!bytes.empty())
        out.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
    out.flush();
    return static_cast<bool>(out);
}

/// The file a finished file is written to, beside its stream, before it is placed.
fs::path work_file(const fs::path& stream_file) {
    fs::path made = stream_file;
    made += ".writing";
    return made;
}

/// The bytes a stream is read in, and the most a chunk can be (its length is a 16-bit word).
constexpr std::size_t stream_piece_bytes = std::size_t{1} << 20;

/// The room a frame's compressed bytes gather in before they are passed on.
constexpr std::size_t frame_room_bytes = std::size_t{256} << 10;

/// A file read in order through one fixed buffer: a chunk at a time, or in pieces.
class StreamFile {
  public:

    /// Opens a file.
    ///
    /// @param path the file
    /// @return false when it cannot be opened or its size cannot be read
    bool open(const fs::path& path) {
        in_.open(path, std::ios::binary);
        if (!in_)
            return false;
        in_.seekg(0, std::ios::end);
        const std::streamoff end = in_.tellg();
        in_.seekg(0);
        if (!in_ || end < 0)
            return false;
        size_ = static_cast<uint64_t>(end);
        buffer_.resize(stream_piece_bytes);
        return true;
    }

    /// Returns the file's size.
    ///
    /// @return the bytes
    [[nodiscard]] uint64_t size() const noexcept { return size_; }

    /// Returns the offset of the next byte not yet taken.
    ///
    /// @return the offset
    [[nodiscard]] uint64_t offset() const noexcept { return taken_ + at_; }

    /// Tells whether a read of the file failed.
    ///
    /// @return true after a read the disk refused
    [[nodiscard]] bool failed() const noexcept { return failed_; }

    /// Returns the next `count` bytes without taking them.
    ///
    /// @param count at most stream_piece_bytes
    /// @return the bytes; shorter when the file ends first
    std::span<const uint8_t> peek(std::size_t count) {
        if (end_ - at_ < count)
            fill();
        return {buffer_.data() + at_, std::min(count, end_ - at_)};
    }

    /// Takes bytes peek returned.
    ///
    /// @param count how many
    void take(std::size_t count) noexcept { at_ += std::min(count, end_ - at_); }

  private:

    /// Moves what is left to the front of the buffer and reads on after it.
    void fill() {
        if (at_ != 0) {
            std::memmove(buffer_.data(), buffer_.data() + at_, end_ - at_);
            taken_ += at_;
            end_ -= at_;
            at_ = 0;
        }
        const uint64_t left = size_ - taken_ - end_;
        const std::size_t room = buffer_.size() - end_;
        const std::size_t want = static_cast<std::size_t>(std::min<uint64_t>(left, room));
        if (want == 0)
            return;
        in_.read(
            reinterpret_cast<char*>(buffer_.data() + end_), static_cast<std::streamsize>(want)
        );
        const auto got = static_cast<std::size_t>(std::max<std::streamsize>(in_.gcount(), 0));
        end_ += got;
        if (got != want)
            failed_ = true;
    }

    std::ifstream in_{};
    std::vector<uint8_t> buffer_{};
    uint64_t size_{};
    uint64_t taken_{}; ///< file offset of buffer_[0]
    std::size_t at_{};
    std::size_t end_{};
    bool failed_{};
};

/// What a walk over a recording stream found.
struct StreamWalk {
    /// The bytes before the packets: the header, sectors, players, statuses and unit checks.
    std::vector<uint8_t> head{};
    /// The length of the stream up to its last whole packet; 0 when the part before the packets
    /// is not whole.
    uint64_t whole{};
    /// Whole packets.
    uint64_t packets{};
    /// The recorder's clock at the last whole packet: the sum of every delay, in milliseconds.
    uint64_t time_ms{};
    /// True when every whole packet's payload starts with the plain marker.
    bool plain{true};
    /// The digest of the stream up to its last whole packet.
    base::sha256::Hasher hasher{};
    /// The latest tick a plain packet's tick base names; 0 for none.
    uint32_t last_tick{};
};

/// The tick a plain packet's payload starts with, when it starts with a tick base.
///
/// @param payload the packet's payload, its marker first
/// @return the tick; 0 when the payload is packed or starts with another record
uint32_t tick_base_of(std::span<const uint8_t> payload) noexcept {
    constexpr std::size_t tick_base_bytes = 5;
    if (payload.size() < 1 + tick_base_bytes || payload[0] != tad::payload_marker ||
        payload[1] != static_cast<uint8_t>(tad::RecordType::tick_base))
        return 0;
    return load32(payload.data() + 2);
}

/// Walks a stream's chunks from its start, as whole_packets_end walks bytes held, a chunk at a
/// time: the header, the sector count, the sectors, the players, their statuses and the unit
/// checks, then each packet, up to the first chunk that is not all there or is too short to
/// hold a packet.
///
/// @param file the stream, at its start
/// @param digest whether the walk takes the digest of the stream up to its last whole packet
/// @param[out] walk what it found
/// @return false when the disk refused a read
bool walk_stream(StreamFile& file, bool digest, StreamWalk& walk) {
    // The length of the whole chunk next; 0 when it is not all there.
    const auto chunk_next = [&file]() -> std::size_t {
        const std::span<const uint8_t> word = file.peek(tad::layout::chunk_length_bytes);
        if (word.size() < tad::layout::chunk_length_bytes)
            return 0;
        const std::size_t length = load16(word.data());
        if (length < tad::layout::chunk_length_bytes || file.peek(length).size() < length)
            return 0;
        return length;
    };
    // Keeps a chunk of the part before the packets.
    const auto keep = [&](std::size_t length) {
        const std::span<const uint8_t> chunk = file.peek(length);
        walk.head.insert(walk.head.end(), chunk.begin(), chunk.end());
        file.take(length);
    };
    const std::size_t header = chunk_next();
    if (header < tad::layout::chunk_length_bytes + tad::layout::header_fixed_bytes)
        return !file.failed();
    const std::size_t player_count =
        file.peek(header)[tad::layout::chunk_length_bytes + tad::layout::magic_bytes + 2];
    keep(header);
    const std::size_t counted = chunk_next();
    if (counted != tad::layout::chunk_length_bytes + tad::layout::sector_count_bytes)
        return !file.failed();
    const uint32_t sectors = load32(file.peek(counted).data() + tad::layout::chunk_length_bytes);
    if (sectors > tad::limit::sectors)
        return !file.failed();
    keep(counted);
    // The sectors, the players, their statuses, and the unit checks.
    const std::size_t before_packets = sectors + player_count * 2 + 1;
    for (std::size_t index = 0; index < before_packets; ++index) {
        const std::size_t length = chunk_next();
        if (length == 0)
            return !file.failed();
        keep(length);
    }
    if (digest)
        base::sha256::update(walk.hasher, walk.head);
    walk.whole = walk.head.size();
    for (;;) {
        const std::size_t length = chunk_next();
        if (length < smallest_packet_chunk)
            break;
        const std::span<const uint8_t> chunk = file.peek(length);
        const std::span<const uint8_t> payload = chunk.subspan(smallest_packet_chunk);
        if (payload.empty() || payload[0] != tad::payload_marker)
            walk.plain = false;
        walk.last_tick = std::max(walk.last_tick, tick_base_of(payload));
        walk.time_ms += load16(chunk.data() + tad::layout::chunk_length_bytes);
        if (digest)
            base::sha256::update(walk.hasher, chunk);
        file.take(length);
        walk.whole += length;
        ++walk.packets;
    }
    return !file.failed();
}

/// Copies the first `count` bytes of a stream through `take`, a piece at a time.
///
/// @param file the stream, at its start
/// @param count how many bytes
/// @param take called with each piece in order; false stops the copy
/// @return false when the disk refused a read or `take` stopped
template <class Take>
bool copy_stream(StreamFile& file, uint64_t count, Take&& take) {
    while (count != 0) {
        const auto want = static_cast<std::size_t>(std::min<uint64_t>(count, stream_piece_bytes));
        const std::span<const uint8_t> piece = file.peek(want);
        if (piece.size() != want || !take(piece))
            return false;
        file.take(want);
        count -= want;
    }
    return !file.failed();
}

/// Counts a frame's bytes and drops them.
bool count_frame(void*, std::span<const uint8_t>) {
    return true;
}

/// Makes a zstd frame of a stream's first bytes a piece at a time, for write_framed to store:
/// each read compresses as much of the stream as the bytes asked for need.
struct FramePump {
    /// The stream, read from its start.
    StreamFile file{};
    /// The bytes still to compress.
    uint64_t left{};
    /// The compressor.
    base::zstd::FrameWriter writer{};
    /// Where its bytes gather.
    std::vector<uint8_t> room{};
    /// Frame bytes made and not yet read.
    std::vector<uint8_t> ready{};
    /// How many of `ready` have been read.
    std::size_t ready_at{};
    /// True once the frame has ended.
    bool ended{};

    FramePump() = default;
    FramePump(const FramePump&) = delete;
    FramePump& operator=(const FramePump&) = delete;

    /// Frees the compressor.
    ~FramePump() { base::zstd::frame_writer_free(writer); }
};

/// Keeps a frame's bytes for a FramePump.
bool keep_frame(void* context, std::span<const uint8_t> bytes) {
    auto& pump = *static_cast<FramePump*>(context);
    pump.ready.insert(pump.ready.end(), bytes.begin(), bytes.end());
    return true;
}

/// Fills `into` with the frame's next bytes, compressing more of the stream as needed.
bool read_pumped_frame(void* context, std::span<uint8_t> into) {
    auto& pump = *static_cast<FramePump*>(context);
    std::size_t filled = 0;
    while (filled < into.size()) {
        if (pump.ready_at == pump.ready.size()) {
            pump.ready.clear();
            pump.ready_at = 0;
            if (pump.ended)
                return false;
            if (pump.left == 0) {
                if (base::zstd::frame_writer_end(pump.writer) != base::zstd::Error::none)
                    return false;
                pump.ended = true;
                continue;
            }
            const auto want =
                static_cast<std::size_t>(std::min<uint64_t>(pump.left, stream_piece_bytes));
            const std::span<const uint8_t> piece = pump.file.peek(want);
            if (piece.size() != want ||
                base::zstd::frame_writer_write(pump.writer, piece) != base::zstd::Error::none)
                return false;
            pump.file.take(want);
            pump.left -= want;
            continue;
        }
        const std::size_t take = std::min(into.size() - filled, pump.ready.size() - pump.ready_at);
        std::memcpy(into.data() + filled, pump.ready.data() + pump.ready_at, take);
        pump.ready_at += take;
        filled += take;
    }
    return true;
}

/// Writes bytes into a file at an offset, for write_framed.
bool write_file_at(void* context, uint64_t offset, std::span<const uint8_t> bytes) {
    auto& out = *static_cast<std::ofstream*>(context);
    if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()))
        return false;
    out.seekp(static_cast<std::streamoff>(offset));
    if (!bytes.empty())
        out.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
    return static_cast<bool>(out);
}

/// Finishes a stream too long to finish in one piece: its frame is made and stored a piece at a
/// time, so only a window of it is ever held.
///
/// Three passes over the stream: the first finds its last whole packet, checks every packet is
/// plain and takes its digest; the second compresses it to learn the frame's size, which the
/// zip entry names before its bytes; the third compresses it again into the replay.
///
/// @param stream_file the recording stream
/// @param replay the replay replay_begin made
/// @param end how the recording ended
/// @param destination the file to make
/// @param replace whether a file that is there is replaced
/// @return what finishing did
FinishResult finish_in_pieces(
    const fs::path& stream_file,
    oademo::Replay replay,
    const ReplayEnd& end,
    const fs::path& destination,
    bool replace
) {
    FinishResult result;
    result.transient = true;
    StreamWalk walk;
    {
        StreamFile file;
        if (!file.open(stream_file)) {
            result.error = "the file cannot be opened";
            return result;
        }
        if (!walk_stream(file, true, walk)) {
            result.error = "the file cannot be read";
            return result;
        }
    }
    result.transient = false;
    if (walk.whole == 0) {
        result.error = "the recording does not hold its header";
        return result;
    }
    if (walk.packets == 0) {
        result.empty = true;
        result.error = "the recording holds no packet";
        return result;
    }
    // The part before the packets parses as the whole would: a recording with no packet.
    const tad::ParseResult parsed = tad::parse(walk.head);
    if (!parsed.ok()) {
        result.error = "the recording cannot be read: " + parsed.error->message;
        return result;
    }
    if (!walk.plain) {
        result.error = "the recording holds a packet that is not plain";
        return result;
    }
    if (tad::find_sector(*parsed.demo, tad::SectorType::player_address) != nullptr) {
        result.error = std::string(oademo::recording_holds_addresses);
        return result;
    }
    replay.meta.recording.packets = walk.packets;
    replay.meta.recording.tad_version = parsed.demo->version;
    replay.meta.recording.addresses_removed = true;
    replay.meta.recorded.after_crash = end.after_crash;
    replay.meta.game.ended = end.ended;
    replay.meta.game.length_ticks = end.length_ticks;
    replay.meta.game.length_ms = walk.time_ms;
    const auto hex = base::sha256::to_hex(base::sha256::finish(walk.hasher));
    replay.meta.recording.sha256.assign(hex.begin(), hex.end());
    replay.meta.recording.bytes = walk.whole;
    replay.recording.clear();

    result.transient = true;
    // The frame's size, which its zip entry names before its bytes.
    std::vector<uint8_t> room(frame_room_bytes);
    uint64_t frame_bytes = 0;
    {
        StreamFile file;
        base::zstd::FrameWriter counter;
        bool counted = file.open(stream_file) && base::zstd::frame_writer_begin(
                                                     counter,
                                                     oademo::level_when_a_game_ends,
                                                     walk.whole,
                                                     room,
                                                     base::zstd::FrameSink{nullptr, count_frame}
                                                 ) == base::zstd::Error::none;
        counted =
            counted && copy_stream(file, walk.whole, [&counter](std::span<const uint8_t> piece) {
                return base::zstd::frame_writer_write(counter, piece) == base::zstd::Error::none;
            });
        counted = counted && base::zstd::frame_writer_end(counter) == base::zstd::Error::none;
        frame_bytes = counter.written;
        base::zstd::frame_writer_free(counter);
        if (!counted) {
            result.error = "the recording cannot be compressed";
            return result;
        }
    }
    std::error_code failure;
    fs::create_directories(destination.parent_path(), failure);
    const fs::path made = work_file(stream_file);
    bool written = false;
    std::string why_not;
    {
        FramePump pump;
        pump.left = walk.whole;
        pump.room.resize(frame_room_bytes);
        std::ofstream out(made, std::ios::binary | std::ios::trunc);
        if (pump.file.open(stream_file) && out &&
            base::zstd::frame_writer_begin(
                pump.writer,
                oademo::level_when_a_game_ends,
                walk.whole,
                pump.room,
                base::zstd::FrameSink{&pump, keep_frame}
            ) == base::zstd::Error::none) {
            why_not = oademo::write_framed(
                replay,
                oademo::FrameSource{&pump, read_pumped_frame, frame_bytes},
                oademo::ArchiveOutput{&out, write_file_at}
            );
            out.flush();
            // The frame ends where its size said: nothing is left over once it was stored.
            if (!pump.ended && pump.ready_at == pump.ready.size() &&
                base::zstd::frame_writer_end(pump.writer) == base::zstd::Error::none)
                pump.ended = true;
            written = why_not.empty() && pump.ended && pump.ready_at == pump.ready.size() &&
                      static_cast<bool>(out);
        }
    }
    if (!written) {
        fs::remove(made, failure);
        result.error = "the replay cannot be written";
        return result;
    }
    if (!place_file(made, destination, replace, result.placed, result.error)) {
        fs::remove(made, failure);
        return result;
    }
    result.transient = false;
    fs::remove(stream_file, failure);
    result.ok = true;
    result.packets = walk.packets;
    result.last_tick = walk.last_tick;
    return result;
}

/// Names the mutators of a loadout in the description, and asks for each one
/// by package in the manifest.
void add_mutators(const RecordingSetup& setup, oademo::Replay& replay) {
    if (setup.loadout_text.empty())
        return;
    const std::optional<oa::data::mutators::Loadout> loadout =
        oa::data::mutators::parse_loadout(setup.loadout_text, nullptr);
    if (!loadout.has_value())
        return;
    for (const oa::data::mutators::LoadoutEntry& entry : loadout->entries) {
        replay.meta.content.mutators.push_back(entry.id);
        oademo::Need need;
        need.kind = "oamut";
        need.id = entry.id;
        need.version = std::optional<std::string>(entry.version);
        const std::array<char, oa::base::sha256::hex_size> hex =
            oa::base::sha256::to_hex(entry.package);
        need.sha256 = std::optional<std::string>(std::in_place, hex.data(), hex.size());
        replay.manifest.needs.push_back(std::move(need));
    }
}

/// Names the brains of a seats text in the description, each once, in the
/// order of the places they played. A brain is never a need: playback seats none.
void add_brains(const RecordingSetup& setup, oademo::Replay& replay) {
    if (setup.brains_text.empty())
        return;
    const std::optional<oa::data::brain_library::Seats> seats =
        oa::data::brain_library::parse_seats(setup.brains_text);
    if (!seats.has_value())
        return;
    std::vector<const oa::data::brain_library::Seat*> by_place;
    by_place.reserve(seats->seats.size());
    for (const oa::data::brain_library::Seat& seat : seats->seats)
        by_place.push_back(&seat);
    std::stable_sort(
        by_place.begin(),
        by_place.end(),
        [](const oa::data::brain_library::Seat* left, const oa::data::brain_library::Seat* right) {
            return left->place < right->place;
        }
    );
    for (const oa::data::brain_library::Seat* seat : by_place) {
        if (seat->brain.empty())
            continue;
        std::vector<std::string>& brains = replay.meta.content.brains;
        if (std::find(brains.begin(), brains.end(), seat->brain) == brains.end())
            brains.push_back(seat->brain);
    }
}

} // namespace

oademo::Replay replay_begin(const RecordingSetup& setup, const ReplayFacts& facts) {
    oademo::Replay replay;
    const auto header = recording_header(setup);
    if (header.ok())
        replay.recording = header.bytes;
    oademo::set_recording_identity(replay);
    auto& meta = replay.meta;
    meta.recording.tad_version = tad::supported_version;
    meta.recording.packets = 0;
    meta.recording.addresses_removed = true;
    if (!facts.recorded_at.empty())
        meta.recorded.at = facts.recorded_at;
    meta.recorded.local = setup.date_text;
    if (!facts.recorded_by.empty())
        meta.recorded.by = facts.recorded_by;
    if (!facts.platform.empty())
        meta.recorded.platform = facts.platform;
    meta.recorded.how = facts.how;
    meta.recorded.after_crash = false;
    meta.game.kind = facts.game_kind;
    if (!facts.game_kind_object.empty())
        meta.game.kind_object = facts.game_kind_object;
    meta.map.name = facts.map_name;
    meta.content.base.id = facts.base_game;
    meta.content.base.version = facts.base_version;
    meta.content.expansions = facts.expansions;
    meta.content.mod = facts.mod;
    if (setup.limits) {
        meta.content.limits.weapon_types = setup.limits->weapon_types;
        meta.content.limits.sides = setup.limits->sides;
    } else {
        meta.content.limits.weapon_types = tad::recorded_base_weapon_types;
        meta.content.limits.sides = tad::recorded_base_sides;
    }
    std::vector<uint8_t> checks;
    for (const auto& check : setup.unit_checks)
        checks.insert(checks.end(), check.begin(), check.end());
    const oademo::UnitTableDigest digest = oademo::unit_table_digest(checks);
    meta.units.count = digest.count;
    meta.units.table_sha256 = digest.sha256;
    meta.units.index_bits = digest.index_bits;
    for (std::size_t index = 0; index < setup.players.size(); ++index) {
        const RecordingPlayer& player = setup.players[index];
        const ReplayPlayerFacts* known =
            index < facts.players.size() ? &facts.players[index] : nullptr;
        const bool watcher = (player.info.options & OA_SETUP_OPTION_WATCHER) != 0;
        oademo::PlayerInfo info;
        info.number = static_cast<int>(index + 1);
        info.name = known != nullptr ? known->name : player.name;
        info.side = watcher ? "watcher" : side_name(player.info.side);
        info.colour = player.info.color;
        info.team = static_cast<int>(player.team);
        info.kind = known != nullptr && !known->kind.empty() ? known->kind
                                                             : (watcher ? "watcher" : "human");
        if (known != nullptr)
            info.recorded_here = known->recorded_here;
        meta.players.push_back(std::move(info));
    }

    replay.manifest.engine.recorded_with = facts.engine_version;
    replay.manifest.engine.needs = std::string(">=") + std::string(first_release_reading_replays);
    oademo::Need base;
    base.kind = "base";
    base.id = facts.base_game;
    base.version = std::optional<std::string>(facts.base_version);
    replay.manifest.needs.push_back(std::move(base));
    for (const std::string& expansion : facts.expansions) {
        oademo::Need need;
        need.kind = "expansion";
        need.id = expansion;
        replay.manifest.needs.push_back(std::move(need));
    }
    for (oademo::Need need : facts.packages) {
        if (need.kind == "oamod" && !need.units_table_sha256.has_value() && digest.count != 0)
            need.units_table_sha256 = digest.sha256;
        replay.manifest.needs.push_back(std::move(need));
    }
    add_mutators(setup, replay);
    add_brains(setup, replay);
    return replay;
}

std::size_t whole_packets_end(std::span<const uint8_t> stream, uint64_t* packets) {
    if (packets != nullptr)
        *packets = 0;
    std::size_t at = 0;
    // The length of the whole chunk at a position; 0 when it is not all there.
    const auto chunk_at = [&stream](std::size_t position) -> std::size_t {
        if (position + tad::layout::chunk_length_bytes > stream.size())
            return 0;
        const std::size_t length = load16(stream.data() + position);
        if (length < tad::layout::chunk_length_bytes || position + length > stream.size())
            return 0;
        return length;
    };
    const std::size_t header = chunk_at(at);
    if (header < tad::layout::chunk_length_bytes + tad::layout::header_fixed_bytes)
        return 0;
    const std::size_t player_count =
        stream[at + tad::layout::chunk_length_bytes + tad::layout::magic_bytes + 2];
    at += header;
    const std::size_t counted = chunk_at(at);
    if (counted != tad::layout::chunk_length_bytes + tad::layout::sector_count_bytes)
        return 0;
    const uint32_t sectors = load32(stream.data() + at + tad::layout::chunk_length_bytes);
    at += counted;
    if (sectors > tad::limit::sectors)
        return 0;
    // The sectors, the players, their statuses, and the unit checks.
    const std::size_t before_packets = sectors + player_count * 2 + 1;
    for (std::size_t index = 0; index < before_packets; ++index) {
        const std::size_t length = chunk_at(at);
        if (length == 0)
            return 0;
        at += length;
    }
    uint64_t whole = 0;
    for (;;) {
        const std::size_t length = chunk_at(at);
        if (length < smallest_packet_chunk)
            break;
        at += length;
        ++whole;
    }
    if (packets != nullptr)
        *packets = whole;
    return at;
}

FinishResult finish_as_replay(
    const fs::path& stream_file,
    oademo::Replay replay,
    const ReplayEnd& end,
    const fs::path& destination,
    bool replace,
    uint64_t whole_most_bytes
) {
    FinishResult result;
    // A stream past what is finished in one piece is finished in pieces.
    std::error_code size_failure;
    const uintmax_t stream_bytes = fs::file_size(stream_file, size_failure);
    if (!size_failure &&
        stream_bytes > std::min<uint64_t>(whole_most_bytes, tad::limit::input_bytes))
        return finish_in_pieces(stream_file, std::move(replay), end, destination, replace);
    std::vector<uint8_t> bytes;
    if (!read_file(stream_file, bytes, result.error, result.transient))
        return result;
    result.transient = false;
    uint64_t packets = 0;
    const std::size_t whole = whole_packets_end(bytes, &packets);
    if (whole == 0) {
        result.error = "the recording does not hold its header";
        return result;
    }
    if (packets == 0) {
        result.empty = true;
        result.error = "the recording holds no packet";
        return result;
    }
    // More packets than a recording held whole may hold: in pieces, which holds none of them.
    if (packets > tad::limit::packets) {
        bytes = {};
        return finish_in_pieces(stream_file, std::move(replay), end, destination, replace);
    }
    bytes.resize(whole);
    const tad::ParseResult parsed = tad::parse(bytes);
    if (!parsed.ok()) {
        result.error = "the recording cannot be read: " + parsed.error->message;
        return result;
    }
    uint32_t last_tick = 0;
    for (const tad::Packet& packet : parsed.demo->packets) {
        if (packet.payload.empty() || packet.payload[0] != tad::payload_marker) {
            result.error = "the recording holds a packet that is not plain";
            return result;
        }
        last_tick = std::max(last_tick, tick_base_of(packet.payload));
    }
    replay.meta.recording.packets = packets;
    replay.meta.recording.tad_version = parsed.demo->version;
    replay.meta.recording.addresses_removed = true;
    replay.meta.recorded.after_crash = end.after_crash;
    replay.meta.game.ended = end.ended;
    replay.meta.game.length_ticks = end.length_ticks;
    if (parsed.demo->packets.empty())
        replay.meta.game.length_ms.reset();
    else
        replay.meta.game.length_ms = parsed.demo->packets.back().time_ms;
    replay.recording = std::move(bytes);
    oademo::set_recording_identity(replay);
    const oademo::WriteResult written =
        oademo::write(replay, oademo::WriteOptions{oademo::level_when_a_game_ends, false});
    if (!written.ok()) {
        result.error = written.error;
        return result;
    }
    result.transient = true;
    std::error_code failure;
    fs::create_directories(destination.parent_path(), failure);
    const fs::path made = work_file(stream_file);
    if (!write_file(made, written.bytes)) {
        fs::remove(made, failure);
        result.error = "the replay cannot be written";
        return result;
    }
    if (!place_file(made, destination, replace, result.placed, result.error)) {
        fs::remove(made, failure);
        return result;
    }
    result.transient = false;
    fs::remove(stream_file, failure);
    result.ok = true;
    result.packets = packets;
    result.last_tick = last_tick;
    return result;
}

FinishResult finish_as_tad(
    const fs::path& stream_file, const fs::path& destination, bool keep_empty, bool replace
) {
    FinishResult result;
    result.transient = true;
    StreamWalk walk;
    StreamFile file;
    if (!file.open(stream_file)) {
        result.error = "the file cannot be opened";
        return result;
    }
    if (!walk_stream(file, false, walk)) {
        result.error = "the file cannot be read";
        return result;
    }
    result.transient = false;
    if (walk.whole == 0) {
        result.error = "the recording does not hold its header";
        return result;
    }
    if (walk.packets == 0 && !keep_empty) {
        result.empty = true;
        result.error = "the recording holds no packet";
        return result;
    }
    const uint64_t stream_bytes = file.size();
    file = StreamFile{};
    std::error_code failure;
    result.transient = true;
    if (walk.whole < stream_bytes) {
        // Cut part way through a packet: keep the packets that are whole, copied a piece at a time.
        const fs::path made = work_file(stream_file);
        fs::create_directories(destination.parent_path(), failure);
        bool copied = false;
        {
            StreamFile from;
            std::ofstream out(made, std::ios::binary | std::ios::trunc);
            copied = from.open(stream_file) && out &&
                     copy_stream(from, walk.whole, [&out](std::span<const uint8_t> piece) {
                         out.write(
                             reinterpret_cast<const char*>(piece.data()),
                             static_cast<std::streamsize>(piece.size())
                         );
                         return static_cast<bool>(out);
                     });
            out.flush();
            copied = copied && static_cast<bool>(out);
        }
        if (!copied) {
            fs::remove(made, failure);
            result.error = "the recording cannot be written";
            return result;
        }
        if (!place_file(made, destination, replace, result.placed, result.error)) {
            fs::remove(made, failure);
            return result;
        }
        fs::remove(stream_file, failure);
    } else if (!place_file(stream_file, destination, replace, result.placed, result.error)) {
        return result;
    }
    result.transient = false;
    result.ok = true;
    result.packets = walk.packets;
    result.last_tick = walk.last_tick;
    return result;
}

std::string recording_stop_line(const FinishResult& result, std::string_view why) {
    std::string line;
    if (result.last_tick == 0) {
        line = "the recording stopped part way";
    } else {
        const uint32_t seconds = result.last_tick / recorded_ticks_per_second;
        const uint32_t minutes = seconds / 60;
        std::string second_text = std::to_string(seconds % 60);
        if (second_text.size() < 2)
            second_text.insert(second_text.begin(), '0');
        line = "the recording stopped at " + std::to_string(minutes) + ":" + second_text +
               " of the game (tick " + std::to_string(result.last_tick) + ")";
    }
    line += ": ";
    line += why;
    line += "; the replay keeps the game up to there";
    return line;
}

} // namespace oa::session::demo
