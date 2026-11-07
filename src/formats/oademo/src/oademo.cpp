// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "detail.hpp"

#include "oa/formats/oademo/interface_words.hpp"

#include "oa/base/sha256.hpp"
#include "oa/base/zstd/zstd.hpp"
#include "oa/formats/tad.hpp"
#include "oa/formats/zip.hpp"
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
namespace {

constexpr std::string_view signature_name = "oademo";
constexpr std::string_view meta_name = "meta.json";
constexpr std::string_view manifest_name = "manifest.json";
constexpr std::string_view timeline_name = "timeline.json";
constexpr std::string_view zstd_name = "recording.tad.zst";
constexpr std::string_view deflate_name = "recording.tad";
constexpr uint32_t signature_cap = 32;
constexpr uint32_t description_cap = 16U * 1024U * 1024U;

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

bool signature_text(std::string_view text) noexcept {
    return text == "oademo 1" || text == "oademo 1\n" || text == "oademo 1\r\n";
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

std::string issue_sentence(detail::Issue issue) {
    if (issue == detail::Issue::version)
        return sentence(not_this_version);
    return sentence(details_unreadable);
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

const zip::Entry* recording_entry(const zip::CentralDirectory& directory, int& count) noexcept {
    count = 0;
    const zip::Entry* found = nullptr;
    for (const zip::Entry& entry : directory.entries) {
        if (entry.name != zstd_name && entry.name != deflate_name)
            continue;
        ++count;
        found = &entry;
    }
    return found;
}

/// A replay whose layout was checked, with its description and manifest read.
struct Described {
    /// The archive's directory.
    zip::CentralDirectory directory{};
    /// True when the fourth entry is `timeline.json` and the recording is the fifth.
    bool with_timeline{};
    /// True when the recording is the stored zstd frame, false when it is deflated.
    bool zstd_entry{};
    /// The description.
    Meta meta{};
    /// The manifest.
    Manifest manifest{};
};

/// Checks a replay's layout and reads its description and manifest.
///
/// Checks the zip directory, the stored `oademo` entry and its line, that one
/// recording entry follows the 4 or 5 entries in order with their methods,
/// and the size a deflated recording names, then reads `meta.json` and
/// `manifest.json`. Neither the timeline nor the recording is read.
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
    // A zip that does not open with the stored `oademo` entry is some other zip.
    if (directory.entries.empty() || directory.entries[0].name != signature_name ||
        directory.entries[0].method != zip::Method::stored)
        return sentence(not_a_replay);

    std::vector<uint8_t> signature;
    std::string problem;
    if (!load_capped(
            bytes, directory.entries[0], signature_cap, not_this_version, signature, problem
        ))
        return problem;
    if (!signature_text(as_text(signature)))
        return sentence(not_this_version);

    int recordings = 0;
    const zip::Entry* recording = recording_entry(directory, recordings);
    if (recordings != 1 || recording == nullptr)
        return sentence(not_one_recording);

    const bool with_timeline = directory.entries.size() == 5;
    if (directory.entries.size() != 4 && !with_timeline)
        return sentence(not_this_version);
    const zip::Entry& meta_entry = directory.entries[1];
    const zip::Entry& manifest_entry = directory.entries[2];
    if (meta_entry.name != meta_name || meta_entry.method != zip::Method::deflated ||
        manifest_entry.name != manifest_name || manifest_entry.method != zip::Method::deflated)
        return sentence(not_this_version);
    const zip::Entry* recording_at = &directory.entries[3];
    if (with_timeline) {
        const zip::Entry& timeline_entry = directory.entries[3];
        recording_at = &directory.entries[4];
        if (timeline_entry.name != timeline_name || timeline_entry.method != zip::Method::deflated)
            return sentence(not_this_version);
    }
    if (recording_at != recording)
        return sentence(not_this_version);
    const bool zstd_entry = recording->name == zstd_name;
    if (zstd_entry) {
        if (recording->method != zip::Method::stored)
            return sentence(not_this_version);
    } else if (recording->method != zip::Method::deflated) {
        return sentence(not_this_version);
    }
    if (!zstd_entry && static_cast<uint64_t>(recording->bytes) > tad::limit::input_bytes)
        return sentence(recording_too_long);

    std::vector<uint8_t> meta_bytes;
    std::vector<uint8_t> manifest_bytes;
    if (!load_capped(bytes, meta_entry, description_cap, details_unreadable, meta_bytes, problem) ||
        !load_capped(
            bytes, manifest_entry, description_cap, details_unreadable, manifest_bytes, problem
        ))
        return problem;

    detail::Issue issue = detail::Issue::none;
    if (!detail::parse_meta(as_text(meta_bytes), described.meta, issue) ||
        !detail::parse_manifest(as_text(manifest_bytes), described.manifest, issue))
        return issue_sentence(issue);
    described.with_timeline = with_timeline;
    described.zstd_entry = zstd_entry;
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
    Described described;
    DescriptionResult result;
    result.error = describe(bytes, described);
    if (!result.error.empty())
        return result;
    result.meta = std::move(described.meta);
    result.manifest = std::move(described.manifest);
    return result;
}

ReadResult read(std::span<const uint8_t> bytes) {
    Described described;
    const std::string why_not_described = describe(bytes, described);
    if (!why_not_described.empty())
        return refuse(why_not_described);
    const std::vector<zip::Entry>& entries = described.directory.entries;
    const zip::Entry* recording = described.with_timeline ? &entries[4] : &entries[3];

    Replay replay;
    replay.meta = std::move(described.meta);
    replay.manifest = std::move(described.manifest);
    if (described.with_timeline) {
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
    if (described.zstd_entry) {
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

    const std::string meta = meta_text(replay.meta);
    const std::string manifest = manifest_text(replay.manifest);
    MemoryOut out;
    zip::WriterOptions zip_options;
    zip_options.deflate_level = deflate_level;
    zip::StreamWriter writer(zip::OutputHooks{&out, write_memory}, zip_options);
    zip::ZipError problem;
    if (!put_entry(
            writer, signature_name, as_bytes(signature_line), zip::Method::stored, problem
        ) ||
        !put_entry(writer, meta_name, as_bytes(meta), zip::Method::deflated, problem) ||
        !put_entry(writer, manifest_name, as_bytes(manifest), zip::Method::deflated, problem))
        return refuse_write(replay_unwritable);
    if (timeline.has_value() &&
        !put_entry(writer, timeline_name, as_bytes(*timeline), zip::Method::deflated, problem))
        return refuse_write(replay_unwritable);
    if (!put_entry(writer, recording_entry_name, recording_bytes, recording_method, problem) ||
        !writer.finish(problem))
        return refuse_write(replay_unwritable);
    WriteResult result;
    result.bytes = std::move(out.bytes);
    return result;
}

void set_recording_identity(Replay& replay) {
    const auto digest = base::sha256::digest_of(replay.recording);
    const auto hex = base::sha256::to_hex(digest);
    replay.meta.recording.sha256.assign(hex.begin(), hex.end());
    replay.meta.recording.bytes = replay.recording.size();
}

} // namespace oa::formats::oademo
