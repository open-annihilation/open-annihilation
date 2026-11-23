// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "detail.hpp"

#include "oa/formats/oademo/interface_words.hpp"
#include "oa/formats/oademo/recording_reader.hpp"

#include "oa/base/sha256.hpp"
#include "oa/base/zstd/zstd.hpp"
#include "oa/formats/tad.hpp"
#include "oa/formats/zip.hpp"
#include "oa/formats/zip/stream.hpp"
#include "oa/formats/zip/writer.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::formats::oademo {

namespace detail {

bool signature_text(std::string_view text) noexcept {
    return text == "oademo 1" || text == "oademo 1\n" || text == "oademo 1\r\n";
}

} // namespace detail

namespace {

using detail::deflate_name;
using detail::description_cap;
using detail::signature_name;
using detail::zstd_name;

std::string sentence(std::string_view text) {
    return std::string(text);
}

ReadResult refuse(std::string_view text) {
    ReadResult result;
    result.error = sentence(text);
    return result;
}

WriteResult refuse_write(std::string_view text) {
    WriteResult result;
    result.error = sentence(text);
    return result;
}

bool same_bytes(std::span<const uint8_t> bytes, std::size_t at, std::string_view text) noexcept {
    if (at > bytes.size() || bytes.size() - at < text.size())
        return false;
    for (std::size_t index = 0; index < text.size(); ++index)
        if (bytes[at + index] != static_cast<uint8_t>(text[index]))
            return false;
    return true;
}

uint16_t load_u16(std::span<const uint8_t> bytes, std::size_t at) noexcept {
    return static_cast<uint16_t>(bytes[at]) |
           static_cast<uint16_t>(static_cast<uint16_t>(bytes[at + 1]) << 8U);
}

std::string as_text(const std::vector<uint8_t>& bytes) {
    return std::string(reinterpret_cast<const char*>(bytes.data()), bytes.size());
}

std::span<const uint8_t> as_bytes(std::string_view text) noexcept {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

bool hex_equal(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        unsigned char a = static_cast<unsigned char>(left[index]);
        unsigned char b = static_cast<unsigned char>(right[index]);
        if (a >= 'A' && a <= 'Z')
            a = static_cast<unsigned char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = static_cast<unsigned char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

struct MemoryOut {
    std::vector<uint8_t> bytes{};
};

bool write_memory(void* context, uint64_t offset, std::span<const uint8_t> bytes) {
    auto& out = *static_cast<MemoryOut*>(context);
    if (offset > out.bytes.size())
        return false;
    const std::size_t at = static_cast<std::size_t>(offset);
    if (bytes.size() > out.bytes.max_size() - at)
        return false;
    if (out.bytes.size() < at + bytes.size())
        out.bytes.resize(at + bytes.size());
    for (std::size_t index = 0; index < bytes.size(); ++index)
        out.bytes[at + index] = bytes[index];
    return true;
}

bool put_entry(
    zip::StreamWriter& writer,
    std::string_view name,
    std::span<const uint8_t> bytes,
    zip::Method method,
    zip::ZipError& problem
) {
    if (!writer.begin_entry(name, bytes.size(), method, problem))
        return false;
    if (!writer.write(bytes, problem))
        return false;
    return writer.end_entry(problem);
}

bool load_capped(
    std::span<const uint8_t> archive,
    const zip::Entry& entry,
    uint32_t cap,
    std::string_view oversize,
    std::vector<uint8_t>& bytes,
    std::string& problem
) {
    if (entry.bytes > cap) {
        problem = sentence(oversize);
        return false;
    }
    zip::ZipError zip_error;
    if (!zip::read_entry(archive, entry, bytes, zip_error)) {
        problem = sentence(not_a_replay);
        return false;
    }
    return true;
}

/// How a frame that names no size was read.
enum class UnsizedRead : uint8_t {
    read,       ///< the frame ended inside the room and `out` holds it
    too_long,   ///< the frame needs more than the recording format allows
    unreadable, ///< the frame is damaged
};

/// Reads a frame whose header names no size, into a buffer that grows.
///
/// The buffer starts at four times the frame, and doubles when the frame has
/// not ended, up to the recording format's limit. Each try decodes from the
/// start, so the work is at most twice the frame's own.
UnsizedRead read_unsized_frame(std::span<const uint8_t> frame, std::vector<uint8_t>& out) {
    constexpr std::size_t limit = tad::limit::input_bytes;
    std::size_t room = std::min(limit, std::max<std::size_t>(frame.size() * 4, 1U << 20U));
    for (;;) {
        try {
            out.resize(room);
        } catch (const std::bad_alloc&) {
            return UnsizedRead::too_long;
        }
        const base::zstd::Sized got = base::zstd::decompress(frame, out);
        if (got.ok()) {
            out.resize(got.bytes);
            return UnsizedRead::read;
        }
        if (got.error != base::zstd::Error::buffer_too_small)
            return UnsizedRead::unreadable;
        if (room >= limit)
            return UnsizedRead::too_long;
        room = room > limit / 2 ? limit : room * 2;
    }
}

/// Reads one frame into `out`, after checking the size it names.
///
/// @return the sentence that says why not, or an empty text when `out` holds the recording
std::string_view read_frame(std::span<const uint8_t> frame, std::vector<uint8_t>& out) {
    const base::zstd::Sized named = base::zstd::content_size(frame, tad::limit::input_bytes);
    if (named.error == base::zstd::Error::too_large)
        return recording_too_long;
    if (named.error == base::zstd::Error::size_unknown) {
        switch (read_unsized_frame(frame, out)) {
        case UnsizedRead::read:
            return {};
        case UnsizedRead::too_long:
            return recording_too_long;
        case UnsizedRead::unreadable:
            return recording_unreadable;
        }
        return recording_unreadable;
    }
    if (!named.ok())
        return recording_unreadable;
    // The size is within the limit, so the buffer is made only now.
    try {
        out.resize(named.bytes);
    } catch (const std::bad_alloc&) {
        return recording_too_long;
    }
    const base::zstd::Sized got = base::zstd::decompress(frame, out);
    if (!got.ok() || got.bytes != named.bytes)
        return recording_unreadable;
    return {};
}

/// A replay whose layout was checked, with its description and manifest read.
struct Described {
    /// The archive's directory.
    zip::CentralDirectory directory{};
    /// Where the recording is.
    detail::Layout layout{};
    /// The description.
    Meta meta{};
    /// The manifest.
    Manifest manifest{};
};

/// Checks a replay's layout and reads its description and manifest.
///
/// Reads the zip directory, then makes the checks of detail::describe_entries.
/// Neither the timeline nor the recording is read.
///
/// @param bytes the whole file
/// @param[out] described the layout, the description and the manifest; whole
///        only when the result is empty
/// @return a sentence from interface_words.hpp, or empty when both were read
std::string describe(std::span<const uint8_t> bytes, Described& described) {
    zip::CentralDirectory& directory = described.directory;
    zip::ZipError zip_error;
    if (!zip::read_directory(bytes, directory, zip_error)) {
        if (zip_error.status == zip::ZipStatus::duplicate_name)
            return sentence(not_one_recording);
        return sentence(not_a_replay);
    }
    const auto load = [&](std::size_t index,
                          uint32_t cap,
                          std::string_view oversize,
                          std::vector<uint8_t>& loaded) {
        std::string problem;
        if (!load_capped(bytes, directory.entries[index], cap, oversize, loaded, problem))
            return problem;
        return std::string();
    };
    return detail::describe_entries(
        std::span<const zip::Entry>(directory.entries),
        tad::limit::input_bytes,
        load,
        described.layout,
        described.meta,
        described.manifest
    );
}

/// Bytes of a file held in memory, read at an offset as a streamed zip reads them.
struct HeldBytes {
    /// The file.
    std::span<const uint8_t> bytes{};
};

bool read_held(void* context, uint64_t offset, std::span<uint8_t> into) {
    const auto& held = *static_cast<const HeldBytes*>(context);
    if (offset > held.bytes.size() || into.size() > held.bytes.size() - offset)
        return false;
    std::copy_n(
        held.bytes.begin() + static_cast<std::ptrdiff_t>(offset), into.size(), into.begin()
    );
    return true;
}

/// Reads the description and the manifest of a replay through the zip reader that takes an
/// archive in pieces.
///
/// @param source where the replay's bytes come from
/// @param archive_bytes the replay's size
/// @param[out] result the description and the manifest, or why not
void describe_in_pieces(
    const zip::SourceHooks& source, uint64_t archive_bytes, DescriptionResult& result
) {
    zip::StreamDirectory directory;
    zip::ZipError zip_error;
    if (!zip::read_stream_directory(
            source, archive_bytes, detail::archive_limits(), directory, zip_error
        )) {
        result.error = sentence(
            zip_error.status == zip::ZipStatus::duplicate_name ? not_one_recording : not_a_replay
        );
        return;
    }
    const auto load = [&](std::size_t index,
                          uint32_t cap,
                          std::string_view oversize,
                          std::vector<uint8_t>& loaded) {
        return detail::load_stream_entry(
            source, archive_bytes, directory.entries[index], cap, oversize, loaded
        );
    };
    detail::Layout layout;
    Meta meta;
    Manifest manifest;
    result.error = detail::describe_entries(
        std::span<const zip::StreamEntry>(directory.entries),
        UINT64_MAX,
        load,
        layout,
        meta,
        manifest
    );
    if (!result.error.empty())
        return;
    result.meta = std::move(meta);
    result.manifest = std::move(manifest);
}

/// Writes the entries of a replay in order: the signature, the description, the
/// manifest, the timeline when there is one, and the recording entry, whose
/// bytes `put_recording` hands the writer.
///
/// @param meta the description as compact JSON
/// @param manifest the manifest as compact JSON
/// @param timeline the timeline as compact JSON, when there is one
/// @param recording_name the recording entry's name
/// @param recording_method how the recording entry is stored
/// @param recording_bytes the recording entry's size, uncompressed
/// @param put_recording writes the entry's bytes, called as `put_recording(writer, problem)`
/// @param output where the archive's bytes go
/// @return empty, or replay_unwritable
template <class PutRecording>
std::string assemble(
    std::string_view meta,
    std::string_view manifest,
    const std::optional<std::string>& timeline,
    std::string_view recording_name,
    zip::Method recording_method,
    uint64_t recording_bytes,
    PutRecording&& put_recording,
    const zip::OutputHooks& output
) {
    zip::WriterOptions zip_options;
    zip_options.deflate_level = deflate_level;
    zip::StreamWriter writer(output, zip_options);
    zip::ZipError problem;
    if (!put_entry(
            writer, signature_name, as_bytes(signature_line), zip::Method::stored, problem
        ) ||
        !put_entry(writer, detail::meta_name, as_bytes(meta), zip::Method::deflated, problem) ||
        !put_entry(
            writer, detail::manifest_name, as_bytes(manifest), zip::Method::deflated, problem
        ))
        return sentence(replay_unwritable);
    if (timeline.has_value() &&
        !put_entry(
            writer, detail::timeline_name, as_bytes(*timeline), zip::Method::deflated, problem
        ))
        return sentence(replay_unwritable);
    if (!writer.begin_entry(recording_name, recording_bytes, recording_method, problem) ||
        !put_recording(writer, problem) || !writer.end_entry(problem) || !writer.finish(problem))
        return sentence(replay_unwritable);
    return {};
}

} // namespace

bool recognised(std::span<const uint8_t> bytes) noexcept {
    // The stored name at byte 30 and the start of the line, `oademo `, at byte 36.
    // The version after it is `read`'s to judge.
    constexpr std::string_view line_start = "oademo ";
    constexpr std::size_t prefix = 36 + line_start.size();
    if (bytes.size() < prefix)
        return false;
    if (!same_bytes(bytes, 0, "PK") || bytes[2] != 3 || bytes[3] != 4)
        return false;
    if (load_u16(bytes, 8) != 0)
        return false;
    if (load_u16(bytes, 26) != signature_name.size() || load_u16(bytes, 28) != 0)
        return false;
    if (!same_bytes(bytes, 30, signature_name))
        return false;
    return same_bytes(bytes, 36, line_start);
}

DescriptionResult read_description(std::span<const uint8_t> bytes) {
    DescriptionResult result;
    // A file past what the whole-archive reader takes is read in pieces; its recording is never
    // touched here, so its size costs nothing.
    if (bytes.size() > zip::max_archive_bytes) {
        HeldBytes held{bytes};
        describe_in_pieces(zip::SourceHooks{&held, read_held}, bytes.size(), result);
        return result;
    }
    Described described;
    result.error = describe(bytes, described);
    if (!result.error.empty())
        return result;
    result.meta = std::move(described.meta);
    result.manifest = std::move(described.manifest);
    return result;
}

DescriptionResult read_description(const ArchiveSource& source) {
    DescriptionResult result;
    if (source.read_at == nullptr) {
        result.error = sentence(not_a_replay);
        return result;
    }
    describe_in_pieces(zip::SourceHooks{source.context, source.read_at}, source.bytes, result);
    return result;
}

ReadResult read(std::span<const uint8_t> bytes) {
    Described described;
    const std::string why_not_described = describe(bytes, described);
    if (!why_not_described.empty())
        return refuse(why_not_described);
    const std::vector<zip::Entry>& entries = described.directory.entries;
    const zip::Entry* recording = &entries[described.layout.recording];

    Replay replay;
    replay.meta = std::move(described.meta);
    replay.manifest = std::move(described.manifest);
    if (described.layout.with_timeline) {
        std::vector<uint8_t> timeline_bytes;
        std::string problem;
        if (!load_capped(
                bytes, entries[3], description_cap, details_unreadable, timeline_bytes, problem
            ))
            return refuse(problem);
        const std::optional<std::string> timeline = detail::canonical_json(as_text(timeline_bytes));
        if (!timeline.has_value())
            return refuse(details_unreadable);
        replay.timeline = *timeline;
    }

    std::vector<uint8_t> stored;
    zip::ZipError read_error;
    if (!zip::read_entry(bytes, *recording, stored, read_error))
        return refuse(not_a_replay);
    if (described.layout.zstd_entry) {
        const std::string_view why_not = read_frame(stored, replay.recording);
        if (!why_not.empty())
            return refuse(why_not);
    } else {
        replay.recording = std::move(stored);
    }

    const auto digest = base::sha256::digest_of(replay.recording);
    const auto hex = base::sha256::to_hex(digest);
    const std::string_view hashed(hex.data(), hex.size());
    if (replay.meta.recording.bytes != replay.recording.size() ||
        !hex_equal(replay.meta.recording.sha256, hashed))
        return refuse(recording_mismatch);
    ReadResult result;
    result.replay = std::move(replay);
    return result;
}

WriteResult write(const Replay& replay, WriteOptions options) {
    std::optional<std::string> timeline;
    if (replay.timeline.has_value()) {
        timeline = detail::canonical_json(*replay.timeline);
        if (!timeline.has_value())
            return refuse_write(timeline_unwritable);
    }
    // The recording must be one this format holds: a TA Demo recording of a
    // size the reader accepts, with no sector of network addresses.
    if (replay.recording.size() > tad::limit::input_bytes)
        return refuse_write(recording_too_long);
    const tad::ParseResult parsed = tad::parse(replay.recording);
    if (!parsed.ok())
        return refuse_write(recording_not_a_recording);
    if (tad::find_sector(*parsed.demo, tad::SectorType::player_address) != nullptr)
        return refuse_write(recording_holds_addresses);

    std::vector<uint8_t> frame;
    std::span<const uint8_t> recording_bytes = replay.recording;
    std::string_view recording_entry_name = zstd_name;
    zip::Method recording_method = zip::Method::stored;
    if (options.deflate) {
        recording_entry_name = deflate_name;
        recording_method = zip::Method::deflated;
    } else {
        if (options.level < 1 || options.level > base::zstd::highest_level())
            return refuse_write(level_refused);
        const std::size_t bound = base::zstd::compress_bound(replay.recording.size());
        if (bound == 0)
            return refuse_write(recording_unwritable);
        try {
            frame.resize(bound);
        } catch (const std::bad_alloc&) {
            return refuse_write(recording_unwritable);
        }
        const bool watched = options.watch.report != nullptr || options.watch.stop != nullptr;
        base::zstd::Sized written;
        if (watched) {
            base::zstd::CompressWatch watch;
            watch.report = options.watch.report;
            watch.stop = options.watch.stop;
            watch.context = options.watch.context;
            written = base::zstd::compress_watched(replay.recording, options.level, frame, watch);
        } else {
            written = base::zstd::compress(replay.recording, options.level, frame);
        }
        if (written.error == base::zstd::Error::bad_level)
            return refuse_write(level_refused);
        if (!written.ok())
            return refuse_write(recording_unwritable);
        frame.resize(written.bytes);
        recording_bytes = frame;
    }

    MemoryOut out;
    const std::string why_not = assemble(
        meta_text(replay.meta),
        manifest_text(replay.manifest),
        timeline,
        recording_entry_name,
        recording_method,
        recording_bytes.size(),
        [&](zip::StreamWriter& writer, zip::ZipError& problem) {
            return writer.write(recording_bytes, problem);
        },
        zip::OutputHooks{&out, write_memory}
    );
    if (!why_not.empty())
        return refuse_write(why_not);
    WriteResult result;
    result.bytes = std::move(out.bytes);
    return result;
}

std::string write_framed(const Replay& replay, const FrameSource& frame, const ArchiveOutput& out) {
    std::optional<std::string> timeline;
    if (replay.timeline.has_value()) {
        timeline = detail::canonical_json(*replay.timeline);
        if (!timeline.has_value())
            return sentence(timeline_unwritable);
    }
    if (frame.read == nullptr || out.write_at == nullptr)
        return sentence(replay_unwritable);
    // The frame passes through one fixed piece, however large it is.
    constexpr std::size_t piece_bytes = 256U * 1024U;
    std::vector<uint8_t> piece(
        static_cast<std::size_t>(std::min<uint64_t>(piece_bytes, frame.bytes))
    );
    return assemble(
        meta_text(replay.meta),
        manifest_text(replay.manifest),
        timeline,
        zstd_name,
        zip::Method::stored,
        frame.bytes,
        [&](zip::StreamWriter& writer, zip::ZipError& problem) {
            for (uint64_t left = frame.bytes; left != 0;) {
                const std::span<uint8_t> next(
                    piece.data(), static_cast<std::size_t>(std::min<uint64_t>(left, piece.size()))
                );
                if (!frame.read(frame.context, next) || !writer.write(next, problem))
                    return false;
                left -= next.size();
            }
            return true;
        },
        zip::OutputHooks{out.context, out.write_at}
    );
}

void set_recording_identity(Replay& replay) {
    const auto digest = base::sha256::digest_of(replay.recording);
    const auto hex = base::sha256::to_hex(digest);
    replay.meta.recording.sha256.assign(hex.begin(), hex.end());
    replay.meta.recording.bytes = replay.recording.size();
}

} // namespace oa::formats::oademo
