// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/catalogue/check.hpp"
#include "catalogue_document.hpp"

#include "oa/base/sha256.hpp"
#include "oa/base/signing/ed25519.hpp"
#include "oa/formats/json.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::catalogue {
namespace {

namespace json = formats::json;

/// Reports whether the two key sets are the same, whatever their order.
bool same_keys(
    std::span<const registry::RegistryKey> trusted,
    const std::vector<registry::RegistryKey>& published
) {
    if (trusted.size() != published.size())
        return false;
    for (const registry::RegistryKey& key : published) {
        bool found = false;
        for (const registry::RegistryKey& old : trusted) {
            if (old.id == key.id && old.key == key.key) {
                found = true;
                break;
            }
        }
        if (!found)
            return false;
    }
    return true;
}

/// The keys an added registry published, when the caller should store them.
std::optional<std::vector<registry::RegistryKey>>
rotation(const CheckRequest& request, const Catalogue& loaded) {
    if (request.built_in || loaded.keys.empty())
        return std::nullopt;
    if (same_keys(request.trusted_keys, loaded.keys))
        return std::nullopt;
    return loaded.keys;
}

} // namespace

Checked check_catalogue(const CheckRequest& request) {
    Checked result;
    if (request.catalogue.size() > max_catalogue_bytes) {
        result.verdict = Verdict::too_large;
        result.detail = "the catalogue is larger than 16 MiB";
        return result;
    }

    // A signed registry is verified here, before any byte is read as JSON.
    // An unsigned registry names no key, so there is nothing to verify.
    if (!request.trusted_keys.empty()) {
        if (request.signature.empty()) {
            result.verdict = Verdict::no_signature;
            result.detail = "the catalogue has no signature";
            return result;
        }
        std::string error;
        const auto line = parse_signature_file(request.signature, &error);
        if (!line) {
            result.verdict = Verdict::bad_signature_file;
            result.detail = std::move(error);
            return result;
        }
        const registry::RegistryKey* key = nullptr;
        for (const registry::RegistryKey& trusted : request.trusted_keys) {
            if (trusted.id == line->key_id) {
                key = &trusted;
                break;
            }
        }
        if (!key) {
            result.verdict = Verdict::unknown_key;
            result.detail = "the signature names a key this registry does not trust";
            return result;
        }
        if (!base::signing::verify(key->key, request.catalogue, line->signature)) {
            result.verdict = Verdict::bad_signature;
            result.detail = "the signature does not match the catalogue";
            return result;
        }
        result.signed_by = line->key_id;
    }

    result.digest = base::sha256::digest_of(request.catalogue);

    // The bytes are parsed once, for the checks below and for the entries.
    const auto root = parse_catalogue_json(request.catalogue);
    if (root && root->type() == json::JsonType::object) {
        const json::Json* version = root->find("catalogue");
        const auto version_number = version ? version->integer() : std::nullopt;
        if (version_number && *version_number != catalogue_version) {
            result.verdict = Verdict::wrong_version;
            result.detail = "catalogue is not 1";
            return result;
        }
        if (version_number && *version_number == catalogue_version) {
            const json::Json* registry = root->find("registry");
            const std::string* registry_text = registry ? registry->string() : nullptr;
            if (registry_text && *registry_text != request.registry_id) {
                result.verdict = Verdict::wrong_registry;
                result.detail = "the catalogue names another registry";
                return result;
            }
        }
    }

    Catalogue loaded;
    std::string error;
    if (!read_parsed_catalogue(root, loaded, &error)) {
        result.verdict = Verdict::malformed;
        result.detail = std::move(error);
        return result;
    }
    if (loaded.registry != request.registry_id) {
        result.verdict = Verdict::wrong_registry;
        result.detail = "the catalogue names another registry";
        return result;
    }

    Verdict verdict = Verdict::accepted;
    if (request.previous.sequence) {
        const int64_t previous = *request.previous.sequence;
        if (loaded.sequence < previous) {
            result.verdict = Verdict::sequence_went_back;
            result.detail = "the sequence went backwards";
            return result;
        }
        if (loaded.sequence == previous) {
            if (request.previous.digest && *request.previous.digest == result.digest) {
                verdict = Verdict::unchanged;
            } else {
                result.verdict = Verdict::sequence_not_advanced;
                result.detail = "the sequence did not advance";
                return result;
            }
        }
    }

    result.verdict = verdict;
    result.expired = request.now >= loaded.expires;
    result.new_keys = rotation(request, loaded);
    result.catalogue = std::make_shared<const Catalogue>(std::move(loaded));
    return result;
}

} // namespace oa::data::catalogue
