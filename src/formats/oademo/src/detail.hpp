// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Shared by the description parser, the archive reader and the recording read
// in pieces. Not a public header.

#include "oa/formats/oademo/interface_words.hpp"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/zip.hpp"
#include "oa/formats/zip/stream.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::formats::oademo {
namespace detail {

/// The stored first entry, which holds the signature line.
inline constexpr std::string_view signature_name = "oademo";
/// The description.
inline constexpr std::string_view meta_name = "meta.json";
/// The manifest.
inline constexpr std::string_view manifest_name = "manifest.json";
/// The optional timeline.
inline constexpr std::string_view timeline_name = "timeline.json";
/// The recording stored as one zstd frame.
inline constexpr std::string_view zstd_name = "recording.tad.zst";
/// The recording deflated.
inline constexpr std::string_view deflate_name = "recording.tad";
/// The most bytes the signature entry may hold.
inline constexpr uint32_t signature_cap = 32;
/// The most bytes the description, the manifest or the timeline may hold.
inline constexpr uint32_t description_cap = 16U * 1024U * 1024U;

/// Why a description or a manifest was refused.
enum class Issue : uint8_t {
    none,    ///< the text was read
    version, ///< the version is not 1
    details, ///< the text is missing a member or is not JSON
};

/// Reads a description.
///
/// @param text the JSON text
/// @param[out] meta the description, when the text was read
/// @param[out] issue why the text was refused
/// @return true when the text was read
[[nodiscard]] bool parse_meta(std::string_view text, Meta& meta, Issue& issue);

/// Reads a manifest.
///
/// @param text the JSON text
/// @param[out] manifest the manifest, when the text was read
/// @param[out] issue why the text was refused
/// @return true when the text was read
[[nodiscard]] bool parse_manifest(std::string_view text, Manifest& manifest, Issue& issue);

/// Rewrites one JSON value as compact JSON.
///
/// @param text one JSON value
/// @return the compact text, or nothing when the text is not one value
[[nodiscard]] std::optional<std::string> canonical_json(std::string_view text);

/// The bounds a replay read in pieces keeps: its archive and its stored
/// recording may be as large as `most_streamed_archive_bytes`, and its
/// directory holds a replay's few entries.
///
/// @return the limits
[[nodiscard]] zip::StreamLimits archive_limits() noexcept;

/// Reads one entry of a replay read in pieces whole, as describe_entries' `load` does.
///
/// @param source where the replay's bytes come from
/// @param archive_bytes the replay's size
/// @param entry the entry
/// @param cap the most bytes the entry may hold
/// @param oversize the sentence for an entry larger than `cap`
/// @param[out] bytes the entry's bytes
/// @return `oversize`, `not_a_replay` for an entry that does not read, or empty
[[nodiscard]] std::string load_stream_entry(
    const zip::SourceHooks& source,
    uint64_t archive_bytes,
    const zip::StreamEntry& entry,
    uint32_t cap,
    std::string_view oversize,
    std::vector<uint8_t>& bytes
);

/// Tells whether the signature entry holds the line this version writes.
///
/// @param text the entry's bytes as text
/// @return true for `oademo 1` with no line end, a newline, or a carriage return and newline
[[nodiscard]] bool signature_text(std::string_view text) noexcept;

/// Where a replay's recording is, once its layout was checked.
struct Layout {
    /// True when the fourth entry is `timeline.json` and the recording is the fifth.
    bool with_timeline{};
    /// True when the recording is the stored zstd frame, false when it is deflated.
    bool zstd_entry{};
    /// The recording entry's place in the directory.
    std::size_t recording{};
};

/// Checks a replay's entries and reads its description and manifest, whichever
/// zip reader listed the entries.
///
/// Checks the stored `oademo` entry and its line, that one recording entry
/// follows the 4 or 5 entries in order with their methods, and the size a
/// deflated recording names, then reads `meta.json` and `manifest.json`.
/// Neither the timeline nor the recording is read.
///
/// @param entries the archive's directory, in order
/// @param most_deflated_recording the most bytes a deflated recording may name
/// @param load reads entry `index` whole, at most `cap` bytes; called as
///        `load(index, cap, oversize, bytes)`, it returns `oversize` for a
///        larger entry, `not_a_replay` for one that does not read, or empty
/// @param[out] layout where the recording is
/// @param[out] meta the description
/// @param[out] manifest the manifest
/// @return a sentence from interface_words.hpp, or empty when both were read
template <class Entry, class Load>
std::string describe_entries(
    std::span<const Entry> entries,
    uint64_t most_deflated_recording,
    Load&& load,
    Layout& layout,
    Meta& meta,
    Manifest& manifest
) {
    // A zip that does not open with the stored `oademo` entry is some other zip.
    if (entries.empty() || entries[0].name != signature_name ||
        entries[0].method != zip::Method::stored)
        return std::string(not_a_replay);
    std::vector<uint8_t> signature;
    if (std::string problem = load(std::size_t{0}, signature_cap, not_this_version, signature);
        !problem.empty())
        return problem;
    if (!signature_text(
            std::string_view(reinterpret_cast<const char*>(signature.data()), signature.size())
        ))
        return std::string(not_this_version);

    int recordings = 0;
    std::size_t recording = 0;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (entries[index].name != zstd_name && entries[index].name != deflate_name)
            continue;
        ++recordings;
        recording = index;
    }
    if (recordings != 1)
        return std::string(not_one_recording);

    const bool with_timeline = entries.size() == 5;
    if (entries.size() != 4 && !with_timeline)
        return std::string(not_this_version);
    const Entry& meta_entry = entries[1];
    const Entry& manifest_entry = entries[2];
    if (meta_entry.name != meta_name || meta_entry.method != zip::Method::deflated ||
        manifest_entry.name != manifest_name || manifest_entry.method != zip::Method::deflated)
        return std::string(not_this_version);
    std::size_t recording_at = 3;
    if (with_timeline) {
        const Entry& timeline_entry = entries[3];
        recording_at = 4;
        if (timeline_entry.name != timeline_name || timeline_entry.method != zip::Method::deflated)
            return std::string(not_this_version);
    }
    if (recording_at != recording)
        return std::string(not_this_version);
    const Entry& recording_entry = entries[recording];
    const bool zstd_entry = recording_entry.name == zstd_name;
    if (zstd_entry) {
        if (recording_entry.method != zip::Method::stored)
            return std::string(not_this_version);
    } else if (recording_entry.method != zip::Method::deflated) {
        return std::string(not_this_version);
    }
    if (!zstd_entry && static_cast<uint64_t>(recording_entry.bytes) > most_deflated_recording)
        return std::string(recording_too_long);

    std::vector<uint8_t> meta_bytes;
    std::vector<uint8_t> manifest_bytes;
    if (std::string problem = load(std::size_t{1}, description_cap, details_unreadable, meta_bytes);
        !problem.empty())
        return problem;
    if (std::string problem =
            load(std::size_t{2}, description_cap, details_unreadable, manifest_bytes);
        !problem.empty())
        return problem;

    Issue issue = Issue::none;
    const std::string_view meta_text(
        reinterpret_cast<const char*>(meta_bytes.data()), meta_bytes.size()
    );
    const std::string_view manifest_text(
        reinterpret_cast<const char*>(manifest_bytes.data()), manifest_bytes.size()
    );
    if (!parse_meta(meta_text, meta, issue) || !parse_manifest(manifest_text, manifest, issue))
        return std::string(issue == Issue::version ? not_this_version : details_unreadable);
    layout.with_timeline = with_timeline;
    layout.zstd_entry = zstd_entry;
    layout.recording = recording;
    return {};
}

} // namespace detail
} // namespace oa::formats::oademo
