// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/replays/long_replay.hpp"

#include "oa/base/sha256.hpp"
#include "oa/base/zstd/zstd.hpp"
#include "oa/formats/oademo/interface_words.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <ios>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::data::replays {

namespace {

namespace fs = std::filesystem;
namespace oademo = oa::formats::oademo;
namespace tad = oa::formats::tad;
namespace zstd = oa::base::zstd;

/// What a stopped export says.
constexpr std::string_view export_stopped = "the export was stopped";

/// What a file that cannot be opened or read says.
constexpr std::string_view file_not_read = "the file could not be read";

/// Packets between two asks whether an export should stop.
constexpr uint64_t packets_between_stops = 256;

/// The bytes a stored frame is copied through.
constexpr std::size_t frame_piece_bytes = std::size_t{1} << 20;

/// Says why a walk stopped, as the reader or the parse says it.
///
/// @param failure the walk's failure
/// @return the sentence, or the parse's words
std::string failure_text(const oademo::WalkFailure& failure) {
    if (!failure.sentence.empty())
        return failure.sentence;
    if (failure.parse.has_value())
        return failure.parse->message;
    return std::string(file_not_read);
}

/// Tells whether a decoded payload holds a wide weapon record (types 0xE0 to 0xE2).
///
/// @param payload the plain payload, marker first
/// @return true when one of its records is wide
bool holds_wide_record(std::span<const uint8_t> payload) {
    const tad::RecordSplit split = tad::split_records(payload, tad::RecordForms::wide);
    for (const tad::Record& record : split.records) {
        if (record.type >= tad::first_wide_recorded_type &&
            static_cast<std::size_t>(record.type - tad::first_wide_recorded_type) <
                tad::wide_record_lengths.size())
            return true;
    }
    return false;
}

/// Opens a replay or a `.tad` file's recording for a walk.
///
/// @param file the opened file
/// @param[out] walk the walk
/// @return empty, or why it did not open
std::string open_walk(ReplayFile& file, oademo::RecordingWalk& walk) {
    const std::vector<uint8_t> first = file.first_bytes(64);
    const bool opened = oademo::recognised(first) ? walk.open_replay(file.archive())
                                                  : walk.open_tad(file.recording());
    if (!opened)
        return failure_text(walk.failure());
    return {};
}

/// A frame read in order from a file, for write_framed.
struct FrameFile {
    std::ifstream in{};
};

bool read_frame_file(void* context, std::span<uint8_t> into) {
    auto& file = *static_cast<FrameFile*>(context);
    file.in.read(reinterpret_cast<char*>(into.data()), static_cast<std::streamsize>(into.size()));
    return file.in.gcount() == static_cast<std::streamsize>(into.size());
}

/// A replay's stored frame read in order, for write_framed.
struct StoredFrame {
    oademo::RecordingReader* reader{};
    const ExportProgress* progress{};
    uint64_t taken{};
    uint64_t whole{};
};

bool read_stored(void* context, std::span<uint8_t> into) {
    auto& frame = *static_cast<StoredFrame*>(context);
    if (frame.progress != nullptr && frame.progress->stop != nullptr &&
        frame.progress->stop(frame.progress->context))
        return false;
    std::size_t got = 0;
    if (!frame.reader->read_stored_frame(into, got).empty() || got != into.size())
        return false;
    frame.taken += got;
    if (frame.progress != nullptr && frame.progress->report != nullptr)
        frame.progress->report(frame.progress->context, frame.taken, frame.whole);
    return true;
}

/// A frame written in pieces to a file.
struct FrameOut {
    std::ofstream out{};
    uint64_t bytes{};
};

bool write_frame_out(void* context, std::span<const uint8_t> bytes) {
    auto& frame = *static_cast<FrameOut*>(context);
    frame.out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    frame.bytes += bytes.size();
    return static_cast<bool>(frame.out);
}

/// Hands a cleaned recording to a frame written in pieces.
struct FrameFeed {
    zstd::FrameWriter* writer{};
};

bool feed_frame(void* context, std::span<const uint8_t> bytes) {
    auto& feed = *static_cast<FrameFeed*>(context);
    return zstd::frame_writer_write(*feed.writer, bytes) == zstd::Error::none;
}

/// Compresses the cleaned recording of `file` into `frame_file` and stores it in a replay.
///
/// @param file the `.tad` or `.oademo`
/// @param replay the description, the manifest and the timeline; its
///        recording identity and packets are set from `cleaned`
/// @param cleaned the cleaned recording's measure
/// @param level the zstd level
/// @param frame_file the scratch file for the frame; removed afterwards
/// @param out where the archive goes
/// @param progress progress and a way to stop; may be null
/// @return empty, or why not
std::string compress_and_store(
    const fs::path& file,
    oademo::Replay replay,
    const CleanedRecording& cleaned,
    int level,
    const fs::path& frame_file,
    const oademo::ArchiveOutput& out,
    const ExportProgress* progress
) {
    std::error_code removed;
    std::string problem;
    {
        FrameOut frame;
        frame.out.open(frame_file, std::ios::binary | std::ios::trunc);
        if (!frame.out)
            return std::string(oademo::recording_unwritable);
        std::vector<uint8_t> room(frame_piece_bytes);
        zstd::FrameWriter writer;
        if (zstd::frame_writer_begin(
                writer, level, cleaned.bytes, room, zstd::FrameSink{&frame, write_frame_out}
            ) != zstd::Error::none) {
            zstd::frame_writer_free(writer);
            frame.out.close();
            fs::remove(frame_file, removed);
            return std::string(oademo::recording_unwritable);
        }
        tad::ExportOptions plain;
        plain.keep_oa_sectors = true;
        plain.fit_tadr_sizes = false;
        FrameFeed feed{&writer};
        const CleanedRecording again =
            write_cleaned_recording(file, plain, ExportSink{&feed, feed_frame}, progress);
        if (!again.ok())
            problem = again.error;
        else if (again.bytes != cleaned.bytes || again.sha256 != cleaned.sha256)
            problem = std::string(file_not_read);
        else if (zstd::frame_writer_end(writer) != zstd::Error::none)
            problem = std::string(oademo::recording_unwritable);
        zstd::frame_writer_free(writer);
        frame.out.close();
        if (problem.empty() && frame.out.fail())
            problem = std::string(oademo::recording_unwritable);
        if (problem.empty()) {
            replay.recording.clear();
            replay.meta.recording.sha256 = cleaned.sha256;
            replay.meta.recording.bytes = cleaned.bytes;
            replay.meta.recording.packets = cleaned.packets;
            replay.meta.recording.tad_version = tad::supported_version;
            replay.meta.recording.addresses_removed = true;
            FrameFile frame_in;
            frame_in.in.open(frame_file, std::ios::binary);
            if (!frame_in.in)
                problem = std::string(oademo::recording_unwritable);
            else
                problem = oademo::write_framed(
                    replay, oademo::FrameSource{&frame_in, read_frame_file, frame.bytes}, out
                );
        }
    }
    fs::remove(frame_file, removed);
    return problem;
}

} // namespace

bool ReplayFile::open(const fs::path& file) {
    in_.open(file, std::ios::binary);
    if (!in_)
        return false;
    in_.seekg(0, std::ios::end);
    const std::streamoff end = in_.tellg();
    if (!in_ || end < 0)
        return false;
    size_ = static_cast<uint64_t>(end);
    next_ = 0;
    return true;
}

oademo::ArchiveSource ReplayFile::archive() noexcept {
    return oademo::ArchiveSource{this, read_at, size_};
}

oademo::RecordingSource ReplayFile::recording() noexcept {
    next_ = 0;
    return oademo::RecordingSource{this, read_on};
}

std::vector<uint8_t> ReplayFile::first_bytes(std::size_t count) {
    std::vector<uint8_t> bytes(static_cast<std::size_t>(std::min<uint64_t>(count, size_)));
    in_.clear();
    in_.seekg(0);
    in_.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    bytes.resize(static_cast<std::size_t>(std::max<std::streamsize>(in_.gcount(), 0)));
    return bytes;
}

bool ReplayFile::read_at(void* context, uint64_t offset, std::span<uint8_t> into) {
    auto& file = *static_cast<ReplayFile*>(context);
    if (offset > file.size_ || into.size() > file.size_ - offset)
        return false;
    file.in_.clear();
    file.in_.seekg(static_cast<std::streamoff>(offset));
    file.in_.read(reinterpret_cast<char*>(into.data()), static_cast<std::streamsize>(into.size()));
    return file.in_.gcount() == static_cast<std::streamsize>(into.size());
}

bool ReplayFile::read_on(void* context, std::span<uint8_t> into, std::size_t& got) {
    auto& file = *static_cast<ReplayFile*>(context);
    got = 0;
    file.in_.clear();
    file.in_.seekg(static_cast<std::streamoff>(file.next_));
    file.in_.read(reinterpret_cast<char*>(into.data()), static_cast<std::streamsize>(into.size()));
    got = static_cast<std::size_t>(std::max<std::streamsize>(file.in_.gcount(), 0));
    file.next_ += got;
    return !file.in_.bad();
}

bool ReplayOutput::open(const fs::path& file) {
    out_.open(file, std::ios::binary | std::ios::in | std::ios::out | std::ios::trunc);
    bytes_ = 0;
    counting_ = false;
    return static_cast<bool>(out_);
}

void ReplayOutput::count_only() noexcept {
    bytes_ = 0;
    counting_ = true;
}

oademo::ArchiveOutput ReplayOutput::archive() noexcept {
    return oademo::ArchiveOutput{this, write_at};
}

bool ReplayOutput::close() {
    if (counting_)
        return true;
    out_.flush();
    const bool written = static_cast<bool>(out_);
    out_.close();
    return written && !out_.fail();
}

bool ReplayOutput::write_at(void* context, uint64_t offset, std::span<const uint8_t> bytes) {
    auto& output = *static_cast<ReplayOutput*>(context);
    if (offset > static_cast<uint64_t>(std::numeric_limits<std::streamoff>::max()) ||
        bytes.size() > std::numeric_limits<uint64_t>::max() - offset)
        return false;
    output.bytes_ = std::max(output.bytes_, offset + bytes.size());
    if (output.counting_)
        return true;
    output.out_.seekp(static_cast<std::streamoff>(offset));
    output.out_.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    return static_cast<bool>(output.out_);
}

oademo::DescriptionResult read_description_file(const fs::path& file) {
    ReplayFile opened;
    if (!opened.open(file)) {
        oademo::DescriptionResult refused;
        refused.error = std::string(oademo::not_a_replay);
        return refused;
    }
    return oademo::read_description(opened.archive());
}

bool long_replay(const fs::path& file) {
    ReplayFile opened;
    if (!opened.open(file))
        return false;
    if (opened.size() > whole_read_most_bytes)
        return true;
    if (!oademo::recognised(opened.first_bytes(64)))
        return false;
    const oademo::DescriptionResult described = oademo::read_description(opened.archive());
    return described.ok() && described.meta->recording.bytes > whole_read_most_bytes;
}

CleanedRecording write_cleaned_recording(
    const fs::path& file,
    const tad::ExportOptions& options,
    const ExportSink& sink,
    const ExportProgress* progress
) {
    CleanedRecording result;
    ReplayFile opened;
    if (!opened.open(file)) {
        result.error = std::string(file_not_read);
        return result;
    }
    oademo::RecordingWalk walk;
    if (std::string why_not = open_walk(opened, walk); !why_not.empty()) {
        result.error = std::move(why_not);
        return result;
    }
    const uint64_t whole =
        walk.replay() != nullptr ? walk.replay()->meta().recording.bytes : opened.size();
    // The part before the packets is exported as a recording with no packet,
    // which tad::export_recording writes as it writes the same part of the whole.
    const tad::ExportResult head = tad::export_recording(walk.head_bytes(), options);
    if (!head.ok()) {
        result.error = head.error->message;
        return result;
    }
    base::sha256::Hasher hasher;
    const auto put = [&](std::span<const uint8_t> bytes) {
        base::sha256::update(hasher, bytes);
        result.bytes += bytes.size();
        return sink.write == nullptr || sink.write(sink.context, bytes);
    };
    const auto stopped = [&]() {
        return progress != nullptr && progress->stop != nullptr &&
               progress->stop(progress->context);
    };
    if (!put(head.bytes)) {
        result.error = std::string(oademo::recording_unwritable);
        return result;
    }
    bool clean = tad::find_sector(walk.head(), tad::SectorType::player_address) == nullptr;
    bool wide = false;
    std::vector<uint8_t> chunk;
    tad::Packet packet;
    for (;;) {
        bool ended = false;
        if (!walk.next(packet, ended)) {
            result.error = failure_text(walk.failure());
            return result;
        }
        if (ended)
            break;
        if (result.packets % packets_between_stops == 0) {
            if (stopped()) {
                result.error = std::string(export_stopped);
                return result;
            }
            if (progress != nullptr && progress->report != nullptr)
                progress->report(progress->context, walk.bytes_read(), whole);
        }
        if (packet.payload.empty() || packet.payload[0] != tad::payload_marker)
            clean = false;
        const tad::DecodedPayload decoded = tad::decode_payload(packet.payload);
        if (!decoded.ok()) {
            result.error = decoded.error->message;
            return result;
        }
        if (!wide && holds_wide_record(decoded.bytes))
            wide = true;
        const std::size_t length = tad::layout::chunk_length_bytes +
                                   tad::layout::packet_fixed_bytes + decoded.bytes.size();
        if (length > tad::limit::chunk_bytes) {
            result.error = "packet does not fit its chunk";
            return result;
        }
        chunk.resize(length);
        chunk[0] = static_cast<uint8_t>(length);
        chunk[1] = static_cast<uint8_t>(length >> 8U);
        chunk[2] = static_cast<uint8_t>(packet.delay_ms);
        chunk[3] = static_cast<uint8_t>(packet.delay_ms >> 8U);
        chunk[4] = packet.sender;
        std::copy(decoded.bytes.begin(), decoded.bytes.end(), chunk.begin() + 5);
        if (!put(chunk)) {
            result.error = std::string(oademo::recording_unwritable);
            return result;
        }
        ++result.packets;
        result.last_time_ms = packet.time_ms;
    }
    if (progress != nullptr && progress->report != nullptr)
        progress->report(progress->context, whole, whole);
    // The export's own order: names, statuses, wide records, then mutators.
    for (const tad::ExportNotice& notice : head.notices) {
        if (notice.kind == tad::ExportNoticeKind::mutators)
            continue;
        result.notices.push_back(notice);
    }
    if (wide)
        result.notices.push_back(tad::ExportNotice{tad::ExportNoticeKind::wide_records, {}});
    for (const tad::ExportNotice& notice : head.notices) {
        if (notice.kind == tad::ExportNoticeKind::mutators)
            result.notices.push_back(notice);
    }
    const auto hex = base::sha256::to_hex(base::sha256::finish(hasher));
    result.sha256.assign(hex.begin(), hex.end());
    result.clean = clean;
    return result;
}

std::string export_long_replay(
    const fs::path& file,
    const std::optional<std::string>& timeline,
    const fs::path& frame_file,
    const oademo::ArchiveOutput& out,
    const ExportProgress* progress
) {
    ReplayFile opened;
    if (!opened.open(file))
        return std::string(file_not_read);
    oademo::RecordingReader reader;
    if (std::string why_not = reader.open(opened.archive()); !why_not.empty())
        return why_not;
    oademo::Replay replay;
    replay.meta = reader.meta();
    replay.manifest = reader.manifest();
    if (timeline.has_value()) {
        replay.timeline = timeline;
    } else if (std::string why_not = reader.read_timeline(replay.timeline); !why_not.empty()) {
        return why_not;
    }
    // A measure first: whether the recording is already clean, which a
    // whole export also asks, and that it reads to its end.
    tad::ExportOptions plain;
    plain.keep_oa_sectors = true;
    plain.fit_tadr_sizes = false;
    const CleanedRecording cleaned = write_cleaned_recording(file, plain, ExportSink{}, progress);
    if (!cleaned.ok())
        return cleaned.error;
    if (!cleaned.clean)
        return compress_and_store(
            file,
            std::move(replay),
            cleaned,
            oademo::level_when_a_game_ends,
            frame_file,
            out,
            progress
        );
    if (reader.stored_frame_bytes() == 0)
        return compress_and_store(
            file,
            std::move(replay),
            cleaned,
            oademo::level_when_a_game_ends,
            frame_file,
            out,
            progress
        );
    StoredFrame frame{&reader, progress, 0, reader.stored_frame_bytes()};
    return oademo::write_framed(
        replay, oademo::FrameSource{&frame, read_stored, reader.stored_frame_bytes()}, out
    );
}

std::string write_replay_in_pieces(
    const fs::path& file,
    oademo::Replay replay,
    int level,
    const fs::path& frame_file,
    const oademo::ArchiveOutput& out,
    const ExportProgress* progress
) {
    tad::ExportOptions plain;
    plain.keep_oa_sectors = true;
    plain.fit_tadr_sizes = false;
    const CleanedRecording cleaned = write_cleaned_recording(file, plain, ExportSink{}, progress);
    if (!cleaned.ok())
        return cleaned.error;
    if (!replay.meta.game.length_ms.has_value() && cleaned.packets != 0)
        replay.meta.game.length_ms = cleaned.last_time_ms;
    return compress_and_store(file, std::move(replay), cleaned, level, frame_file, out, progress);
}

} // namespace oa::data::replays
