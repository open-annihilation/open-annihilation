// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/engine_feed/feed.hpp"

#include "oa/base/sha256.hpp"
#include "oa/base/signing/ed25519.hpp"
#include "oa/data/catalogue/check.hpp"

#include <memory>
#include <string>
#include <utility>

namespace oa::data::engine_feed {
namespace {

namespace catalogue = oa::data::catalogue;

} // namespace

const char* verdict_text(Verdict verdict) noexcept {
    switch (verdict) {
    case Verdict::accepted:
        return "accepted";
    case Verdict::too_large:
        return "too_large";
    case Verdict::no_signature:
        return "no_signature";
    case Verdict::bad_signature_file:
        return "bad_signature_file";
    case Verdict::unknown_key:
        return "unknown_key";
    case Verdict::bad_signature:
        return "bad_signature";
    case Verdict::malformed:
        return "malformed";
    case Verdict::wrong_format:
        return "wrong_format";
    case Verdict::wrong_channel:
        return "wrong_channel";
    }
    return "";
}

Checked check_feed(const CheckRequest& request) {
    Checked result;
    if (request.feed.size() > max_feed_bytes) {
        result.verdict = Verdict::too_large;
        result.detail = "the feed is larger than 256 KiB";
        return result;
    }
    if (request.signature.empty()) {
        result.verdict = Verdict::no_signature;
        result.detail = "the feed has no signature";
        return result;
    }

    std::string signature_error;
    const auto line = catalogue::parse_signature_file(request.signature, &signature_error);
    if (!line) {
        result.verdict = Verdict::bad_signature_file;
        result.detail = std::move(signature_error);
        return result;
    }

    const registry::PinnedKey* pin = nullptr;
    for (const registry::PinnedKey& key : request.keys) {
        if (key.key_id == line->key_id) {
            pin = &key;
            break;
        }
    }
    if (pin == nullptr) {
        result.verdict = Verdict::unknown_key;
        result.detail = "the signature names a key that is not pinned";
        return result;
    }

    const auto public_key = base::signing::parse_public_key(pin->public_key);
    if (!public_key || !base::signing::verify(*public_key, request.feed, line->signature)) {
        result.verdict = Verdict::bad_signature;
        result.detail = "the signature does not match the feed";
        return result;
    }

    result.signed_by = line->key_id;
    result.digest = base::sha256::digest_of(request.feed);

    // The bytes are not read until the signature has been verified.
    std::string read_error;
    auto loaded = read_feed(request.feed, &read_error);
    if (!loaded) {
        if (read_error == "feed is not 1")
            result.verdict = Verdict::wrong_format;
        else
            result.verdict = Verdict::malformed;
        result.detail = std::move(read_error);
        return result;
    }
    if (loaded->channel != request.channel) {
        result.verdict = Verdict::wrong_channel;
        result.detail = "the feed is for another channel";
        return result;
    }

    result.verdict = Verdict::accepted;
    result.feed = std::make_shared<const Feed>(std::move(*loaded));
    return result;
}

} // namespace oa::data::engine_feed
