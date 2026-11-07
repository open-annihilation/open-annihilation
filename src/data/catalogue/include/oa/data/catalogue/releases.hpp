// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The releases list a catalogue points at: older releases of the packages
// the catalogue offers, named by their SHA-256. Nothing here fetches. The
// catalogue's signature covers the list through the digest on `releases`.
#pragma once

#include "oa/data/catalogue/catalogue.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace oa::data::catalogue {

/// The releases-list format this build reads.
inline constexpr int64_t releases_version = 1;

/// How taking a fetched list against the catalogue member went.
enum class ReleasesTake : uint8_t {
    taken,    ///< the bytes match the member and the list was read
    mismatch, ///< the size or the digest differs, or the bytes are not a list
    ignored,  ///< the bytes match, and `releases` is not 1
};

/// One older release the list offers. A skipped entry is not stored here.
struct ReleaseEntry {
    Kind kind = Kind::oamod;
    std::string id;   ///< the package key
    std::string name; ///< the name a player sees, 1 to 64 bytes
    /// 1 to 32 bytes. Empty when this kind names none.
    std::string version;
    /// The packaging revision, when the entry names one. Absent otherwise.
    std::optional<int32_t> revision;
    int64_t release{};             ///< the release number, 1 to 2^31-1
    uint64_t size{};               ///< the package's size in bytes, 1 to 2^32
    base::sha256::Digest sha256{}; ///< the package's SHA-256
    std::string file;              ///< the reference whose last segment is `<sha256>.<kind>`
    /// The day or time this release was published, as written. Empty when absent.
    std::string published;
    /// The engine requirement as written. Empty when the entry names none.
    std::string requires_engine_text;
    /// The engine requirement, read with the shared range parser. Absent when
    /// the entry names none.
    std::optional<formats::oamod::EngineRange> requires_engine;
};

/// One releases list that was read. `problems` holds why entries were skipped.
struct ReleasesList {
    std::string registry;               ///< the registry id the list names
    int64_t sequence{};                 ///< the publisher's sequence, at least 0
    std::string generated_text;         ///< `generated`, as written
    int64_t generated{};                ///< `generated`, seconds since 1970
    std::vector<ReleaseEntry> packages; ///< the entries that passed their rules
    std::vector<std::string> problems;  ///< why entries were skipped, at most max_problems
};

/// A releases list this build is holding, and the digest it was accepted for.
struct HeldReleases {
    /// The catalogue member's digest these bytes were accepted against.
    /// Absent when nothing is held.
    std::optional<base::sha256::Digest> digest;
    ReleasesList list{}; ///< the entries; empty when nothing is held
};

/// Reads a releases list when its bytes are the file the catalogue member names.
///
/// The size and the SHA-256 are compared with `member` before the bytes are
/// read. A difference, or bytes that are not a releases list, returns `held`
/// unchanged and reports `mismatch`. A list whose `releases` is not 1 returns
/// `held` unchanged and reports `ignored`. A list that reads has each entry
/// checked on its own: one that breaks a rule is left out, and the reason is
/// kept when fewer than max_problems reasons are kept already. The accepted
/// list replaces `held`, and its digest is `member.sha256`.
///
/// @param member the catalogue's pointer at this file
/// @param bytes the file's bytes
/// @param held the list already held; returned unchanged unless the bytes are taken
/// @param[out] how `taken`, `mismatch` or `ignored`; may be null
/// @return the list to hold
[[nodiscard]] HeldReleases take_releases(
    const ReleasesRef& member, std::span<const uint8_t> bytes, HeldReleases held, ReleasesTake* how
);

} // namespace oa::data::catalogue
