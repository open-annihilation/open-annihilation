// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/update_helper/verify.hpp"

#include "oa/base/sha256.hpp"
#include "oa/base/signing/ed25519.hpp"
#include "oa/data/engine_feed/feed.hpp"
#include "oa/data/registry/descriptor.hpp"
#include "oa/data/registry/trust.hpp"
#include "oa/update_helper/paths.hpp"

#include <array>
#include <cstdint>
#include <fstream>
#include <span>
#include <utility>
#include <vector>

namespace oa::update_helper {
namespace {

namespace fs = std::filesystem;
namespace feed = oa::data::engine_feed;
namespace sha256 = oa::base::sha256;

constexpr std::size_t hash_piece = 64 * 1024;
constexpr std::size_t max_signature_bytes = 256;

void set_error(std::string* reason, std::string message) {
    if (reason != nullptr)
        *reason = std::move(message);
}

bool read_capped(
    const fs::path& path, std::size_t most, std::vector<uint8_t>& out, std::string* reason
) {
    std::error_code io;
    const auto size = fs::file_size(path, io);
    if (io) {
        set_error(reason, "a file could not be read");
        return false;
    }
    const auto take = size > most ? most : static_cast<std::size_t>(size);
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(reason, "a file could not be read");
        return false;
    }
    out.resize(take);
    if (take > 0)
        in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(take));
    if (!in && !in.eof()) {
        set_error(reason, "a file could not be read");
        out.clear();
        return false;
    }
    return true;
}

bool hash_file(const fs::path& path, sha256::Digest& digest, std::string* reason) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        set_error(reason, "the package could not be read");
        return false;
    }
    sha256::Hasher hasher;
    std::array<uint8_t, hash_piece> piece{};
    while (in) {
        in.read(reinterpret_cast<char*>(piece.data()), static_cast<std::streamsize>(piece.size()));
        const auto got = in.gcount();
        if (got > 0)
            sha256::update(
                hasher, std::span<const uint8_t>(piece.data(), static_cast<std::size_t>(got))
            );
    }
    if (in.bad()) {
        set_error(reason, "the package could not be read");
        return false;
    }
    digest = sha256::finish(hasher);
    return true;
}

} // namespace

bool parse_feed_key(std::string_view text, FeedKey& key, std::string* error) {
    const auto equals = text.find('=');
    if (equals == std::string_view::npos || equals == 0 || equals + 1 >= text.size()) {
        set_error(error, "a feed key must be an id, an equals sign and a key");
        return false;
    }
    FeedKey parsed;
    parsed.id = std::string(text.substr(0, equals));
    parsed.text = std::string(text.substr(equals + 1));
    if (!oa::data::registry::valid_key_id(parsed.id) ||
        !oa::base::signing::parse_public_key(parsed.text)) {
        set_error(error, "a feed key must be an id, an equals sign and a key");
        return false;
    }
    key = std::move(parsed);
    return true;
}

bool verify_package(const VerifyRequest& request, Verified& verified, std::string* error) {
    std::vector<uint8_t> feed_bytes;
    if (!read_capped(request.feed, feed::max_feed_bytes + 1, feed_bytes, error))
        return false;

    std::vector<uint8_t> signature_bytes;
    std::error_code signature_io;
    if (fs::exists(request.signature, signature_io) && !signature_io) {
        if (!read_capped(request.signature, max_signature_bytes + 1, signature_bytes, error))
            return false;
    }

    std::vector<oa::data::registry::PinnedKey> pins;
    pins.reserve(request.keys.size());
    for (const FeedKey& key : request.keys) {
        oa::data::registry::PinnedKey pin;
        pin.registry = "check";
        pin.key_id = key.id;
        pin.public_key = key.text;
        pins.push_back(pin);
    }

    feed::CheckRequest check;
    check.feed = feed_bytes;
    check.signature = signature_bytes;
    if (!pins.empty())
        check.keys = std::span<const oa::data::registry::PinnedKey>(pins.data(), pins.size());
    const feed::Checked checked = feed::check_feed(check);
    if (!checked.usable() || checked.feed == nullptr) {
        const char* verdict = feed::verdict_text(checked.verdict);
        std::string message = verdict != nullptr ? verdict : "refused";
        if (!checked.detail.empty())
            message += ": " + checked.detail;
        set_error(error, std::move(message));
        return false;
    }

    const feed::Release* release = nullptr;
    for (const feed::Release& candidate : checked.feed->releases) {
        if (candidate.release == request.release) {
            release = &candidate;
            break;
        }
    }
    if (release == nullptr) {
        set_error(error, "the feed does not name this release");
        return false;
    }

    const feed::Package* package = nullptr;
    for (const feed::Package& candidate : release->packages) {
        if (candidate.id == request.package) {
            package = &candidate;
            break;
        }
    }
    if (package == nullptr) {
        set_error(error, "the feed does not name this package");
        return false;
    }

    const feed::PackageFile* recorded = nullptr;
    if (request.installer) {
        if (!package->installer) {
            set_error(error, "the feed does not name an installer");
            return false;
        }
        recorded = &*package->installer;
    } else {
        recorded = &package->zip;
    }

    if (utf8_of(request.file.filename()) != recorded->file) {
        set_error(error, "the package's name is not the one the feed names");
        return false;
    }

    std::error_code size_io;
    const auto size = fs::file_size(request.file, size_io);
    if (size_io) {
        set_error(error, "the package could not be read");
        return false;
    }
    if (static_cast<uint64_t>(size) != recorded->size) {
        set_error(error, "the package's size does not match the feed");
        return false;
    }

    sha256::Digest digest{};
    if (!hash_file(request.file, digest, error))
        return false;
    if (digest != recorded->sha256) {
        set_error(error, "the package's digest does not match the feed");
        return false;
    }

    verified.version = release->version_text;
    verified.release = release->release;
    verified.package = package->id;
    return true;
}

} // namespace oa::update_helper
