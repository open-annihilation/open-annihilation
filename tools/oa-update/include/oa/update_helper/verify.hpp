// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Checking a downloaded package against the saved feed. The feed's signature
// is checked first. The package is then the file that feed names, of the
// size and digest the feed records. Nothing here unpacks or moves a file.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace oa::update_helper {

/// One key that may sign the feed, in place of the keys the build pins.
struct FeedKey {
    std::string id;   ///< the key id
    std::string text; ///< `ed25519:` and the base64 of the public key
};

/// What the caller knows about the package it wants checked.
struct VerifyRequest {
    std::filesystem::path feed;      ///< the saved feed
    std::filesystem::path signature; ///< the saved signature; missing is no signature
    std::filesystem::path file;      ///< the downloaded package
    int64_t release{};               ///< the release number to find
    std::string package;             ///< the package id to find
    bool installer{};                ///< true to check the package's installer
    /// Keys in place of the pinned ones. Empty uses the keys the build pins.
    std::vector<FeedKey> keys;
};

/// The release the feed actually named, once the package matched.
struct Verified {
    std::string version; ///< the release's version text
    int64_t release{};   ///< the release's number
    std::string package; ///< the package id
};

/// Reads one `--feed-key` value, `ID=ed25519:` and the base64.
///
/// @param text the value
/// @param[out] key the key
/// @param[out] error why it was refused; may be null
/// @return true when the value is one key
[[nodiscard]] bool parse_feed_key(std::string_view text, FeedKey& key, std::string* error);

/// Checks the feed and the package.
///
/// The signature is checked with the request's keys, or the pinned keys when
/// none were passed. The release and the package are then taken from the
/// feed. The file's name, size and SHA-256, read in 64 KiB pieces, must be
/// the ones that package records. A failure leaves the disk alone.
///
/// @param request the feed, the signature and the package
/// @param[out] verified the release the feed named; unchanged on failure
/// @param[out] error why the package was refused; may be null
/// @return true when the package is the one the feed names
[[nodiscard]] bool
verify_package(const VerifyRequest& request, Verified& verified, std::string* error);

} // namespace oa::update_helper
