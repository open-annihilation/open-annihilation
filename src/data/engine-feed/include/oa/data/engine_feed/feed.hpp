// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The signed engine feed: the releases one channel offers, and the check
// that the bytes are the ones a pinned key signed. The release number
// orders releases; the version text is only shown. Nothing here fetches
// or opens a file.
#pragma once

#include "oa/base/sha256.hpp"
#include "oa/data/registry/trust.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/formats/url.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::engine_feed {

/// The feed format this build reads.
inline constexpr int64_t feed_format = 1;

/// The most bytes of a feed this build reads.
inline constexpr std::size_t max_feed_bytes = 256 * 1024;

/// The channel a release build asks for.
inline constexpr std::string_view stable_channel = "stable";

/// One file a release offers: the zip, or the installer beside it.
struct PackageFile {
    std::string file;              ///< a plain file name, never a path
    uint64_t size{};               ///< bytes, from 1 to 2^40
    base::sha256::Digest sha256{}; ///< the file's SHA-256
};

/// One package of a release, named by the build it is for.
struct Package {
    std::string id;                         ///< the package id, such as windows-x64
    PackageFile zip;                        ///< the zip a download saves
    std::optional<PackageFile> installer;   ///< the Windows setup or the macOS package
    std::vector<formats::url::Url> mirrors; ///< addresses ending in /, with no query
};

/// The release notes in one language.
struct Notes {
    std::string language;           ///< the language tag the notes were written under
    std::vector<std::string> lines; ///< the lines, in order
};

/// One release the feed offers.
struct Release {
    int64_t release{};                       ///< the number that orders releases
    formats::oamod::EngineVersion version{}; ///< the version, parsed
    std::string version_text;                ///< the version as the feed shows it
    std::string date;                        ///< the day, YYYY-MM-DD
    formats::oamod::EngineVersion from{};    ///< the oldest version updated in place
    int32_t rollout{100};                    ///< the percentage; 100 when the feed omits it
    std::vector<Notes> notes;                ///< notes by language; empty when omitted
    std::string page;                        ///< the release page; empty when omitted
    std::vector<Package> packages;           ///< the packages, in the feed's order
};

/// One channel's feed. Releases are newest first.
struct Feed {
    std::string channel; ///< the channel the file names
    /// The oldest version that may play online. Read and kept; not acted on.
    std::optional<formats::oamod::EngineVersion> online_min;
    std::vector<Release> releases; ///< newest release number first
};

/// Reads a feed's structure.
///
/// The bytes are one JSON object of format 1. A signature is not checked.
/// A member this build does not know is ignored. A member named twice
/// refuses the feed. Releases come back newest first.
///
/// @param bytes the feed's bytes
/// @param[out] error why the bytes were refused; may be null
/// @return the feed, or nothing when the bytes are not one
[[nodiscard]] std::optional<Feed> read_feed(std::span<const uint8_t> bytes, std::string* error);

/// Why a feed was kept or refused.
enum class Verdict : uint8_t {
    accepted,           ///< the feed is the one to keep
    too_large,          ///< more than max_feed_bytes
    no_signature,       ///< the signature file is empty
    bad_signature_file, ///< the signature file is not the one line
    unknown_key,        ///< the signature names a key that is not pinned
    bad_signature,      ///< the signature does not match the bytes
    malformed,          ///< the bytes are not a feed
    wrong_format,       ///< feed is not 1
    wrong_channel,      ///< the feed names another channel
};

/// The verdict as a short name.
///
/// @param verdict the verdict
/// @return a stable name; never null
[[nodiscard]] const char* verdict_text(Verdict verdict) noexcept;

/// What the caller knows when it asks for a feed to be checked.
struct CheckRequest {
    std::span<const uint8_t> feed;            ///< the feed's exact bytes
    std::span<const uint8_t> signature;       ///< the signature file; empty when there is none
    std::string_view channel{stable_channel}; ///< the channel the bytes were fetched for
    /// The keys that may sign it. The engine's pinned keys when the caller passes none.
    std::span<const registry::PinnedKey> keys{registry::pinned_keys()};
};

/// The result of checking one feed.
struct Checked {
    Verdict verdict{Verdict::malformed}; ///< why the bytes were kept or refused
    std::string detail;                  ///< the particular reason, when there is one
    /// The feed. Set only when the bytes are usable.
    std::shared_ptr<const Feed> feed;
    std::string signed_by;         ///< the pinned key the signature named
    base::sha256::Digest digest{}; ///< the SHA-256 of the exact bytes, after the signature

    /// Reports whether the feed may be offered.
    ///
    /// @return true when the verdict is accepted
    [[nodiscard]] bool usable() const noexcept { return verdict == Verdict::accepted; }
};

/// Checks a fetched feed.
///
/// The size is checked first, then the signature file, then the pinned key
/// it names. The signature is verified over the exact bytes before those
/// bytes are parsed. The format and the channel come next.
///
/// @param request the bytes, the channel and the keys
/// @return the verdict, and the feed when it was accepted
[[nodiscard]] Checked check_feed(const CheckRequest& request);

/// The release number of a version.
///
/// major × 10000 + minor × 100 + patch. Defined only while minor and patch
/// are at most 99, so 0.8.7 is 807. A feed may still number a release
/// however the maintainer chooses.
///
/// @param version the version
/// @return the number, or nothing when minor or patch is above 99
[[nodiscard]] std::optional<int64_t> release_number(formats::oamod::EngineVersion version) noexcept;

/// The build that is asking which release to offer.
struct Build {
    formats::oamod::EngineVersion version{}; ///< the build's version
    int64_t release{};                       ///< the build's release number
    std::string_view package;                ///< the build's package id
};

/// How an offered release is taken.
enum class Way : uint8_t {
    none,      ///< nothing to offer
    in_place,  ///< the build is new enough to take the package
    page_only, ///< the build is older than from, so the page is offered
};

/// The release choose picked, and the package for this build.
///
/// The pointers address the feed that was passed to choose, and are null
/// when way is none.
struct Offer {
    const Release* release{}; ///< the release, or null
    const Package* package{}; ///< that release's package for the build, or null
    Way way{Way::none};       ///< how the release is offered
};

/// Picks the release to offer a build.
///
/// Walks the releases newest first. A release at or below the build's
/// number is skipped, and so is one with no package for the build. The
/// first that remains is in place when the build's version is at or after
/// from, and page only otherwise. rollout and online_min are not read.
///
/// @param feed the feed, newest release first
/// @param build the build that is asking
/// @return the offer; way is none when nothing is left
[[nodiscard]] Offer choose(const Feed& feed, const Build& build) noexcept;

/// The notes for a language tag.
///
/// The exact tag is taken first, then its first subtag, then en. The match
/// does not care about ASCII case. The pointer addresses `release`.
///
/// @param release the release
/// @param language_tag the tag, such as de-AT
/// @return the notes, or null when the release has none of those
[[nodiscard]] const Notes*
notes_for(const Release& release, std::string_view language_tag) noexcept;

/// Writes a feed in the canonical form.
///
/// Members are in the order the format shows them, with two-space indents
/// and a final line end. Reading the text back yields the same feed.
///
/// @param feed the feed
/// @return the JSON text
[[nodiscard]] std::string feed_text(const Feed& feed);

} // namespace oa::data::engine_feed
