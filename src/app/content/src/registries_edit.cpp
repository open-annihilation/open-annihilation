// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Adding, removing and turning off registries. A check fetches on the worker.
// It stops at its connect, idle and whole-fetch limits, so one address cannot
// hold the next check past those limits. An edit writes Registries.yaml and
// the cache on the calling thread, under the service's lock, and only when
// that file could be read.
#include "reach.hpp"
#include "service_state.hpp"

#include "oa/base/signing/ed25519.hpp"
#include "oa/data/catalogue/check.hpp"
#include "oa/formats/url.hpp"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app::content {
namespace {

namespace fs = std::filesystem;
namespace registry = data::registry;
namespace catalogue = data::catalogue;
namespace http = netgame::http;

/// What a check reads once, before any fetch, so the lock is not held while
/// the network runs.
struct CheckContext {
    std::vector<registry::Descriptor> builtins{};
    registry::PlayerRegistries player{};
    bool developer_mode = false;
    int64_t now = 0;
};

/// The registry a mirror is checked against, copied before the fetch.
struct MirrorTarget {
    bool found = false;
    registry::Descriptor descriptor{};
    catalogue::Previous previous{};
};

/// A catalogue a source served that the check accepted.
struct FetchedCatalogue {
    bool cancelled = false;
    bool ok = false;
    std::string detail{};
    std::vector<uint8_t> bytes{};
    std::vector<uint8_t> signature{};
    std::shared_ptr<const catalogue::Catalogue> catalogue{};
    base::sha256::Digest digest{};
    formats::url::Url served_from{};
    bool expired = false;
    /// A reached catalogue failed its signature, and none verified.
    bool signature_refused = false;
};

/// A signing key the player typed could not be read.
constexpr std::string_view kSigningKeyUnreadable = "The signing key could not be read.";
/// A typed key did not verify the catalogue.
constexpr std::string_view kCatalogueNotSigned = "The catalogue isn't signed by this key.";
/// The id stored for a typed key that the descriptor does not already name.
constexpr std::string_view kPlayerKeyId = "player";

/// Writes a refusal into `why` when the caller asked for one.
///
/// @param why the caller's sentence; null skips it
/// @param text the sentence
void set_why(std::string* why, std::string_view text) {
    if (why != nullptr)
        *why = std::string(text);
}

/// Reports whether two registry ids are the same, ASCII case ignored.
///
/// @param left one id
/// @param right the other id
/// @return true when they match
bool same_id(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const char first = left[index] >= 'A' && left[index] <= 'Z'
                               ? static_cast<char>(left[index] - 'A' + 'a')
                               : left[index];
        const char second = right[index] >= 'A' && right[index] <= 'Z'
                                ? static_cast<char>(right[index] - 'A' + 'a')
                                : right[index];
        if (first != second)
            return false;
    }
    return true;
}

/// Reports whether an id is one path segment a cache folder may use.
///
/// @param id the registry id
/// @return true when the id has no separator and is not a relative segment
bool one_segment(std::string_view id) noexcept {
    if (id.empty() || id == "." || id == "..")
        return false;
    for (char character : id) {
        if (character == '/' || character == '\\' || character == ':')
            return false;
    }
    return true;
}

/// The day `seconds` falls on, UTC, as YYYY-MM-DD.
///
/// @param seconds seconds since 1970-01-01T00:00:00Z
/// @return the day, or empty when `seconds` is negative
std::string utc_date(int64_t seconds) {
    if (seconds < 0)
        return {};
    int64_t days = seconds / 86400;
    days += 719468;
    const int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    const unsigned day_of_era = static_cast<unsigned>(days - era * 146097);
    const unsigned year_of_era =
        (day_of_era - day_of_era / 1460 + day_of_era / 36524 - day_of_era / 146096) / 365;
    int year = static_cast<int>(year_of_era) + static_cast<int>(era) * 400;
    const unsigned day_of_year =
        day_of_era - (365 * year_of_era + year_of_era / 4 - year_of_era / 100);
    const unsigned month_part = (5 * day_of_year + 2) / 153;
    const unsigned day = day_of_year - (153 * month_part + 2) / 5 + 1;
    const unsigned month = month_part < 10 ? month_part + 3 : month_part - 9;
    year += month <= 2 ? 1 : 0;
    std::string text;
    text.reserve(10);
    const std::string year_text = std::to_string(year);
    if (year_text.size() < 4)
        text.append(4 - year_text.size(), '0');
    text += year_text;
    text.push_back('-');
    text.push_back(static_cast<char>('0' + month / 10));
    text.push_back(static_cast<char>('0' + month % 10));
    text.push_back('-');
    text.push_back(static_cast<char>('0' + day / 10));
    text.push_back(static_cast<char>('0' + day % 10));
    return text;
}

/// Appends a URL that is not already listed.
///
/// @param urls the list
/// @param url the URL
void append_unique(std::vector<formats::url::Url>& urls, const formats::url::Url& url) {
    for (const formats::url::Url& have : urls) {
        if (have == url)
            return;
    }
    urls.push_back(url);
}

/// Joins the reasons a catalogue was not usable.
///
/// @param errors one reason per source that was tried
/// @return the reasons separated by a semicolon, or a short fallback
/// Drops leading and trailing spaces and tabs from a signing key.
///
/// @param text the field's text
/// @return the key, or empty when the field was blank
std::string trimmed_signing_key(std::string_view text) {
    std::size_t begin = 0;
    while (begin < text.size() && (text[begin] == ' ' || text[begin] == '\t'))
        ++begin;
    std::size_t end = text.size();
    while (end > begin && (text[end - 1] == ' ' || text[end - 1] == '\t'))
        --end;
    return std::string(text.substr(begin, end - begin));
}

/// Reports whether a catalogue verdict is a signature the typed key did not make.
///
/// @param verdict the verdict
/// @return true for a missing, unreadable, unknown or mismatched signature
bool signature_verdict(catalogue::Verdict verdict) noexcept {
    return verdict == catalogue::Verdict::no_signature ||
           verdict == catalogue::Verdict::bad_signature_file ||
           verdict == catalogue::Verdict::unknown_key ||
           verdict == catalogue::Verdict::bad_signature;
}

/// Replaces a descriptor's keys with the one public key the player typed.
///
/// The id stays the descriptor's when that key is already named there, so a
/// signature file that names it still verifies. Any other key is stored as
/// `player`. A built-in registry's pinned keys are not written.
///
/// @param[in,out] descriptor the descriptor that was read
/// @param public_key the key the player typed
void use_typed_key(registry::Descriptor& descriptor, const base::signing::PublicKey& public_key) {
    std::string kept_id(kPlayerKeyId);
    for (const registry::RegistryKey& listed : descriptor.keys) {
        if (listed.key == public_key) {
            kept_id = listed.id;
            break;
        }
    }
    registry::RegistryKey chosen;
    chosen.id = std::move(kept_id);
    chosen.key = public_key;
    descriptor.keys.clear();
    descriptor.keys.push_back(std::move(chosen));
}

std::string join_errors(const std::vector<std::string>& errors) {
    if (errors.empty())
        return "no usable catalogue";
    std::string joined;
    for (const std::string& error : errors) {
        if (!joined.empty())
            joined += "; ";
        joined += error;
    }
    return joined;
}

/// Counts packages by kind, leaving out a kind that has none.
///
/// @param check the check to fill
/// @param loaded the catalogue that verified
void set_counts(RegistryCheck& check, const catalogue::Catalogue& loaded) {
    check.packages = loaded.packages.size();
    std::size_t languages = 0;
    std::size_t mods = 0;
    std::size_t maps = 0;
    std::size_t expansions = 0;
    std::size_t mutators = 0;
    std::size_t brains = 0;
    for (const catalogue::Package& package : loaded.packages) {
        switch (package.kind) {
        case catalogue::Kind::oalang:
            ++languages;
            break;
        case catalogue::Kind::oamod:
            ++mods;
            break;
        case catalogue::Kind::oamap:
            ++maps;
            break;
        case catalogue::Kind::expansion:
            ++expansions;
            break;
        case catalogue::Kind::oamut:
            ++mutators;
            break;
        case catalogue::Kind::oabrain:
            ++brains;
            break;
        }
    }
    if (languages != 0)
        check.kinds.push_back({catalogue::Kind::oalang, languages});
    if (mods != 0)
        check.kinds.push_back({catalogue::Kind::oamod, mods});
    if (maps != 0)
        check.kinds.push_back({catalogue::Kind::oamap, maps});
    if (expansions != 0)
        check.kinds.push_back({catalogue::Kind::expansion, expansions});
    if (mutators != 0)
        check.kinds.push_back({catalogue::Kind::oamut, mutators});
    if (brains != 0)
        check.kinds.push_back({catalogue::Kind::oabrain, brains});
}

/// Fetches one address into memory.
///
/// @param client the worker's client
/// @param url the address
/// @param gzip true to accept a gzip body
/// @param limit the most decoded body bytes
/// @param cancel the service's cancel flag
/// @param total_ms the whole fetch, redirects included
/// @param body the decoded body; cleared by the client
/// @return the client's outcome
http::Response fetch_bytes(
    http::Client& client,
    const formats::url::Url& url,
    bool gzip,
    uint64_t limit,
    const std::atomic<bool>* cancel,
    uint32_t total_ms,
    std::vector<uint8_t>& body
) {
    http::Request request;
    request.url = formats::url::url_text(url);
    request.accept_gzip = gzip;
    request.limits.max_body_bytes = limit;
    request.timeouts.connect_ms = registry_check_connect_ms;
    request.timeouts.idle_ms = registry_check_idle_ms;
    request.timeouts.total_ms = total_ms;
    request.cancel = cancel;
    return client.fetch(request, body);
}

/// Copies the registries a check is judged against. Takes the service's lock.
///
/// @param state the service
/// @return the built-in registries, the player's list and Developer mode
CheckContext copy_context(ServiceState& state) {
    base::threads::LockGuard guard(state.mutex);
    CheckContext context;
    context.developer_mode = state.options.developer_mode;
    context.now = now_seconds(state);
    context.player = state.player;
    for (const RegistryRecord& record : state.registries) {
        if (record.retired || record.registry.origin != registry::Origin::built_in)
            continue;
        context.builtins.push_back(record.registry.descriptor);
    }
    return context;
}

/// Copies one registry for a mirror check. Takes the service's lock.
///
/// @param state the service
/// @param id the registry's id
/// @param context filled with the lists the rules use
/// @return the registry, or found false when it is not in effect
MirrorTarget copy_mirror_target(ServiceState& state, std::string_view id, CheckContext& context) {
    base::threads::LockGuard guard(state.mutex);
    context.developer_mode = state.options.developer_mode;
    context.now = now_seconds(state);
    context.player = state.player;
    MirrorTarget target;
    for (const RegistryRecord& record : state.registries) {
        if (record.retired)
            continue;
        if (record.registry.origin == registry::Origin::built_in)
            context.builtins.push_back(record.registry.descriptor);
        if (target.found || !same_id(record.registry.descriptor.id, id))
            continue;
        target.found = true;
        target.descriptor = record.registry.descriptor;
        if (record.catalogue && record.have_digest) {
            target.previous.sequence = record.catalogue->sequence;
            target.previous.digest = record.digest;
        }
    }
    return target;
}

/// The rules a new registry or mirror is checked against.
///
/// @param context the copied lists
/// @return the context the registry rules take
registry::AddContext add_context(const CheckContext& context) {
    registry::AddContext rules;
    rules.built_ins = context.builtins;
    rules.player = &context.player;
    rules.developer_mode = context.developer_mode;
    return rules;
}

/// Stores a worker's outcome. Takes the service's lock.
///
/// @param state the service
/// @param request the check
/// @param incoming the outcome; done is set here
void store_check(ServiceState& state, uint64_t request, StoredCheck incoming) {
    base::threads::LockGuard guard(state.mutex);
    for (StoredCheck& stored : state.checks) {
        if (stored.request != request)
            continue;
        const bool mirror = stored.mirror;
        const bool used = stored.used;
        incoming.request = request;
        incoming.done = true;
        incoming.mirror = mirror;
        incoming.used = used;
        incoming.result.request = request;
        stored = std::move(incoming);
        return;
    }
}

/// Stores a failure when the check threw before it could store one.
///
/// @param state the service
/// @param request the check
/// @param detail what failed
void store_failure(ServiceState& state, uint64_t request, std::string detail) {
    base::threads::LockGuard guard(state.mutex);
    for (StoredCheck& stored : state.checks) {
        if (stored.request != request || stored.done)
            continue;
        stored.done = true;
        stored.result.request = request;
        stored.result.refusal = registry::Refusal::unreadable;
        stored.result.detail = std::move(detail);
        return;
    }
}

/// Hands out a request id and remembers an unfinished check.
///
/// @param state the service
/// @param mirror true when this check is a mirror
/// @return the request id, or 0 when the service is stopping
uint64_t begin_check(ServiceState& state, bool mirror) {
    base::threads::LockGuard guard(state.mutex);
    if (state.stop)
        return 0;
    StoredCheck stored;
    stored.request = ++state.next_check;
    if (stored.request == 0)
        stored.request = ++state.next_check;
    stored.mirror = mirror;
    const uint64_t request = stored.request;
    state.checks.push_back(std::move(stored));
    return request;
}

/// Tries each catalogue address until one verifies, or the service stops.
///
/// @param id the registry the catalogue must name
/// @param keys the keys that may sign it
/// @param previous the catalogue already trusted, when there is one
/// @param now the service clock, seconds since 1970
/// @param sources the addresses, in the order they are tried
/// @param tools the worker's client and cancel flag
/// @return the catalogue that verified, or why none did
FetchedCatalogue fetch_catalogue(
    std::string_view id,
    std::span<const registry::RegistryKey> keys,
    const catalogue::Previous& previous,
    int64_t now,
    const std::vector<formats::url::Url>& sources,
    WorkerTools& tools
) {
    FetchedCatalogue fetched;
    std::vector<std::string> errors;
    bool saw_signature_refusal = false;
    for (const formats::url::Url& source : sources) {
        if (tools.cancel != nullptr && tools.cancel->load()) {
            fetched.cancelled = true;
            return fetched;
        }
        std::vector<uint8_t> body;
        const http::Response response = fetch_bytes(
            tools.http,
            source,
            true,
            catalogue::max_catalogue_bytes,
            tools.cancel,
            registry_check_catalogue_ms,
            body
        );
        if (response.failure == http::Failure::cancelled ||
            (tools.cancel != nullptr && tools.cancel->load())) {
            fetched.cancelled = true;
            return fetched;
        }
        if (response.failure != http::Failure::none) {
            errors.push_back(reach_detail(response));
            continue;
        }
        if (response.status != 200) {
            errors.push_back("the server answered " + std::to_string(response.status));
            continue;
        }

        const std::optional<formats::url::Url> signature_url =
            formats::url::parse_http_url(formats::url::url_text(source) + ".sig");
        std::vector<uint8_t> signature;
        http::Response signature_response;
        if (signature_url) {
            signature_response = fetch_bytes(
                tools.http,
                *signature_url,
                false,
                4 * 1024,
                tools.cancel,
                registry_check_catalogue_ms,
                signature
            );
        }
        if (signature_response.failure == http::Failure::cancelled ||
            (tools.cancel != nullptr && tools.cancel->load())) {
            fetched.cancelled = true;
            return fetched;
        }
        if (signature_response.failure != http::Failure::none || signature_response.status != 200)
            signature.clear();

        catalogue::CheckRequest request;
        request.catalogue = body;
        request.signature = signature;
        request.registry_id = id;
        request.trusted_keys = keys;
        request.built_in = false;
        request.previous = previous;
        request.now = now;
        const catalogue::Checked checked = catalogue::check_catalogue(request);
        if (!checked.usable() || !checked.catalogue) {
            if (signature_verdict(checked.verdict))
                saw_signature_refusal = true;
            errors.push_back(catalogue::verdict_text(checked.verdict));
            continue;
        }
        fetched.ok = true;
        fetched.bytes = std::move(body);
        fetched.signature = std::move(signature);
        fetched.catalogue = checked.catalogue;
        fetched.digest = checked.digest;
        fetched.served_from = source;
        fetched.expired = checked.expired;
        return fetched;
    }
    fetched.signature_refused = saw_signature_refusal;
    fetched.detail = join_errors(errors);
    return fetched;
}

/// Fills a check from a descriptor that was read, then fetches its catalogue.
///
/// @param state the service
/// @param request the check
/// @param context the rules' lists, copied before the fetch
/// @param descriptor the descriptor that was read
/// @param url the address it was read from; empty for a file
/// @param typed_key true when the player gave a signing key, so a signature
///        failure says that key did not sign the catalogue
/// @param tools the worker's client
void finish_registry(
    ServiceState& state,
    uint64_t request,
    const CheckContext& context,
    registry::Descriptor descriptor,
    std::string url,
    bool typed_key,
    WorkerTools& tools
) {
    StoredCheck stored;
    stored.result.url = std::move(url);
    stored.result.address = registry::address_text(descriptor);
    stored.result.fingerprints = registry::key_fingerprints(descriptor);
    stored.result.descriptor = descriptor;
    const registry::Refusal rules = registry::check_new_registry(descriptor, add_context(context));
    if (rules != registry::Refusal::none) {
        stored.result.refusal = rules;
        stored.result.detail = registry::refusal_text(rules);
        store_check(state, request, std::move(stored));
        return;
    }

    std::vector<formats::url::Url> sources;
    append_unique(sources, descriptor.catalogue);
    for (const formats::url::Url& mirror : descriptor.mirrors)
        append_unique(sources, mirror);
    const FetchedCatalogue fetched =
        fetch_catalogue(descriptor.id, descriptor.keys, {}, context.now, sources, tools);
    if (fetched.cancelled) {
        stored.result.refusal = registry::Refusal::unreadable;
        stored.result.detail = "the fetch was cancelled";
        store_check(state, request, std::move(stored));
        return;
    }
    if (!fetched.ok || !fetched.catalogue) {
        stored.result.refusal = registry::Refusal::unreadable;
        if (typed_key && fetched.signature_refused)
            stored.result.detail = std::string(kCatalogueNotSigned);
        else
            stored.result.detail = fetched.detail;
        store_check(state, request, std::move(stored));
        return;
    }
    stored.result.refusal = registry::Refusal::none;
    stored.result.catalogue = fetched.catalogue;
    set_counts(stored.result, *fetched.catalogue);
    stored.result.served_from = fetched.served_from;
    stored.catalogue_bytes = fetched.bytes;
    stored.signature = fetched.signature;
    stored.digest = fetched.digest;
    stored.have_digest = true;
    stored.served_from = fetched.served_from;
    stored.expired = fetched.expired;
    store_check(state, request, std::move(stored));
}

/// Fetches and checks a descriptor from an address.
///
/// @param state the service
/// @param request the check
/// @param url the address the player gave
/// @param signing_key a public key, or empty to keep the descriptor's keys
/// @param tools the worker's client
void complete_url_check(
    ServiceState& state,
    uint64_t request,
    const std::string& url,
    const std::string& signing_key,
    WorkerTools& tools
) {
    const std::string typed = trimmed_signing_key(signing_key);
    std::optional<base::signing::PublicKey> typed_key;
    if (!typed.empty()) {
        typed_key = base::signing::parse_public_key(typed);
        if (!typed_key) {
            StoredCheck stored;
            stored.result.refusal = registry::Refusal::unreadable;
            stored.result.detail = std::string(kSigningKeyUnreadable);
            store_check(state, request, std::move(stored));
            return;
        }
    }
    const CheckContext context = copy_context(state);
    formats::url::UrlError error = formats::url::UrlError::none;
    const std::optional<formats::url::Url> parsed = formats::url::parse_http_url(url, &error);
    if (!parsed) {
        StoredCheck stored;
        stored.result.refusal = registry::Refusal::unreadable;
        stored.result.detail = formats::url::url_error_text(error);
        store_check(state, request, std::move(stored));
        return;
    }
    std::vector<uint8_t> body;
    const http::Response response = fetch_bytes(
        tools.http,
        *parsed,
        false,
        registry::max_descriptor_bytes,
        tools.cancel,
        registry_check_descriptor_ms,
        body
    );
    if (response.failure == http::Failure::cancelled ||
        (tools.cancel != nullptr && tools.cancel->load())) {
        StoredCheck stored;
        stored.result.url = formats::url::url_text(*parsed);
        stored.result.refusal = registry::Refusal::unreadable;
        stored.result.detail = "the fetch was cancelled";
        store_check(state, request, std::move(stored));
        return;
    }
    if (response.failure != http::Failure::none || response.status != 200) {
        StoredCheck stored;
        stored.result.url = formats::url::url_text(*parsed);
        stored.result.refusal = registry::Refusal::unreadable;
        if (response.failure != http::Failure::none) {
            stored.result.detail = reach_detail(response);
        } else {
            stored.result.detail = "the server answered " + std::to_string(response.status);
        }
        store_check(state, request, std::move(stored));
        return;
    }
    registry::Descriptor descriptor;
    std::string read_error;
    if (!registry::read_descriptor(body, descriptor, &read_error)) {
        StoredCheck stored;
        stored.result.url = formats::url::url_text(*parsed);
        stored.result.refusal = registry::Refusal::unreadable;
        stored.result.detail = read_error.empty() ? std::string("The registry could not be read.")
                                                  : std::move(read_error);
        store_check(state, request, std::move(stored));
        return;
    }
    if (typed_key)
        use_typed_key(descriptor, *typed_key);
    finish_registry(
        state,
        request,
        context,
        std::move(descriptor),
        formats::url::url_text(*parsed),
        typed_key.has_value(),
        tools
    );
}

/// Checks a descriptor the player already has, then fetches its catalogue.
///
/// @param state the service
/// @param request the check
/// @param bytes the descriptor file
/// @param tools the worker's client
void complete_file_check(
    ServiceState& state, uint64_t request, const std::vector<uint8_t>& bytes, WorkerTools& tools
) {
    const CheckContext context = copy_context(state);
    registry::Descriptor descriptor;
    std::string read_error;
    if (!registry::read_descriptor(bytes, descriptor, &read_error)) {
        StoredCheck stored;
        stored.result.refusal = registry::Refusal::unreadable;
        stored.result.detail = read_error.empty() ? std::string("The registry could not be read.")
                                                  : std::move(read_error);
        store_check(state, request, std::move(stored));
        return;
    }
    finish_registry(state, request, context, std::move(descriptor), {}, false, tools);
}

/// Checks one mirror against the registry's trusted keys.
///
/// @param state the service
/// @param request the check
/// @param id the registry's id
/// @param url the mirror the player gave
/// @param tools the worker's client
void complete_mirror_check(
    ServiceState& state,
    uint64_t request,
    const std::string& id,
    const std::string& url,
    WorkerTools& tools
) {
    CheckContext context;
    const MirrorTarget target = copy_mirror_target(state, id, context);
    StoredCheck stored;
    stored.result.url = url;
    if (target.found) {
        stored.result.descriptor = target.descriptor;
        stored.result.fingerprints = registry::key_fingerprints(target.descriptor);
    }
    formats::url::UrlError error = formats::url::UrlError::none;
    const std::optional<formats::url::Url> parsed = formats::url::parse_http_url(url, &error);
    if (!parsed) {
        stored.result.refusal = registry::Refusal::unreadable;
        stored.result.detail = formats::url::url_error_text(error);
        store_check(state, request, std::move(stored));
        return;
    }
    stored.mirror_url = *parsed;
    stored.result.url = formats::url::url_text(*parsed);
    stored.result.address = formats::url::host_header(*parsed);
    const registry::Refusal rules = registry::check_new_mirror(id, *parsed, add_context(context));
    if (rules != registry::Refusal::none) {
        stored.result.refusal = rules;
        stored.result.detail = registry::refusal_text(rules);
        store_check(state, request, std::move(stored));
        return;
    }

    const std::vector<formats::url::Url> sources{*parsed};
    const FetchedCatalogue fetched = fetch_catalogue(
        target.descriptor.id, target.descriptor.keys, target.previous, context.now, sources, tools
    );
    if (fetched.cancelled) {
        stored.result.refusal = registry::Refusal::unreadable;
        stored.result.detail = "the fetch was cancelled";
        store_check(state, request, std::move(stored));
        return;
    }
    if (!fetched.ok || !fetched.catalogue) {
        stored.result.refusal = registry::Refusal::unreadable;
        stored.result.detail = fetched.detail;
        store_check(state, request, std::move(stored));
        return;
    }
    stored.result.refusal = registry::Refusal::none;
    stored.result.catalogue = fetched.catalogue;
    set_counts(stored.result, *fetched.catalogue);
    stored.result.served_from = fetched.served_from;
    stored.catalogue_bytes = fetched.bytes;
    stored.signature = fetched.signature;
    stored.digest = fetched.digest;
    stored.have_digest = true;
    stored.served_from = fetched.served_from;
    stored.expired = fetched.expired;
    store_check(state, request, std::move(stored));
}

/// Runs one check and stores a failure when it throws.
///
/// The check runs on the worker, behind work already queued. The fetch stops
/// at the connect limit while the address is opened, the idle limit while a
/// byte is waited for, and the whole-fetch limit, so this check cannot hold
/// a later one for longer than those clocks.
///
/// @param service the service
/// @param state the service's state
/// @param request the check
/// @param job the check
void run_check(Service& service, ServiceState& state, uint64_t request, WorkerJob job) {
    service.run_on_worker([&state, request, job = std::move(job)](WorkerTools& tools) {
        try {
            job(tools);
        } catch (const std::exception& error) {
            store_failure(state, request, std::string("the check failed: ") + error.what());
        } catch (...) {
            store_failure(state, request, "the check failed");
        }
    });
}

/// Reports whether an edit may write. The caller holds the service's lock.
///
/// @param state the service
/// @param[out] why why it may not; may be null
/// @return true when Registries.yaml was read and the service has started
bool edits_open(const ServiceState& state, std::string* why) {
    if (state.stop || !state.started) {
        set_why(why, "The content service has not started.");
        return false;
    }
    if (state.options.player_folder.empty() ||
        state.player_load == registry::LoadResult::unreadable) {
        const std::string& error = state.registries_file_error;
        set_why(
            why,
            error.empty() ? std::string_view("Registries.yaml could not be read.")
                          : std::string_view(error)
        );
        return false;
    }
    return true;
}

/// Replaces Registries.yaml. The caller holds the service's lock.
///
/// @param state the service
/// @param player the registries to store
/// @param[out] why why the file was not replaced; may be null
/// @return true when the file holds these registries
bool save_player(
    const ServiceState& state, const registry::PlayerRegistries& player, std::string* why
) {
    std::string error;
    const fs::path file =
        state.options.player_folder / std::string(registry::player_registries_file);
    if (!registry::save_player_registries(file, player, &error)) {
        set_why(
            why,
            error.empty() ? std::string_view("Registries.yaml could not be written.")
                          : std::string_view(error)
        );
        return false;
    }
    return true;
}

/// The built-in descriptors still in effect. The caller holds the lock.
///
/// @param state the service
/// @return the descriptors, in list order
std::vector<registry::Descriptor> builtins_of(const ServiceState& state) {
    std::vector<registry::Descriptor> builtins;
    for (const RegistryRecord& record : state.registries) {
        if (record.retired || record.registry.origin != registry::Origin::built_in)
            continue;
        builtins.push_back(record.registry.descriptor);
    }
    return builtins;
}

/// Finds a registry that has not been removed. The caller holds the lock.
///
/// @param state the service
/// @param id the registry's id
/// @return its place in the list, or nothing
std::optional<std::size_t> find_registry(const ServiceState& state, std::string_view id) {
    for (std::size_t index = 0; index < state.registries.size(); ++index) {
        const RegistryRecord& record = state.registries[index];
        if (record.retired || !same_id(record.registry.descriptor.id, id))
            continue;
        return index;
    }
    return std::nullopt;
}

/// Finds a finished or running check. The caller holds the lock.
///
/// @param state the service
/// @param request the check
/// @return the check, or null
StoredCheck* find_check(ServiceState& state, uint64_t request) {
    for (StoredCheck& stored : state.checks) {
        if (stored.request == request)
            return &stored;
    }
    return nullptr;
}

/// Copies the player's flags onto the registries in memory.
///
/// The caller holds the lock. Catalogues already kept are left as they are.
///
/// @param state the service
void apply_player(ServiceState& state) {
    for (RegistryRecord& record : state.registries) {
        if (record.retired)
            continue;
        if (record.registry.origin == registry::Origin::built_in) {
            bool enabled = true;
            for (const std::string& disabled : state.player.disabled) {
                if (same_id(disabled, record.registry.descriptor.id))
                    enabled = false;
            }
            record.registry.enabled = enabled;
        } else {
            for (const registry::AddedRegistry& added : state.player.registries) {
                if (!same_id(added.descriptor.id, record.registry.descriptor.id))
                    continue;
                record.registry.descriptor = added.descriptor;
                record.registry.enabled = added.enabled;
                record.registry.player_mirrors = added.player_mirrors;
                record.registry.url = added.url;
                record.registry.conflicting = false;
                for (const RegistryRecord& other : state.registries) {
                    if (other.retired || other.registry.origin != registry::Origin::built_in)
                        continue;
                    if (same_id(other.registry.descriptor.id, added.descriptor.id))
                        record.registry.conflicting = true;
                }
            }
        }
        record.sources = sources_of(record);
        record.status = status_of(record, state.options.developer_mode);
    }
}

/// Appends the registry just added, with the catalogue the check accepted.
///
/// @param state the service
/// @param stored the check that was added
void append_added(ServiceState& state, const StoredCheck& stored) {
    if (state.player.registries.empty())
        return;
    const registry::AddedRegistry& added = state.player.registries.back();
    for (const RegistryRecord& record : state.registries) {
        if (!record.retired && record.registry.origin == registry::Origin::added &&
            same_id(record.registry.descriptor.id, added.descriptor.id))
            return;
    }
    RegistryRecord record;
    record.registry.descriptor = added.descriptor;
    record.registry.origin = registry::Origin::added;
    record.registry.enabled = added.enabled;
    record.registry.player_mirrors = added.player_mirrors;
    record.registry.url = added.url;
    for (const RegistryRecord& other : state.registries) {
        if (other.retired || other.registry.origin != registry::Origin::built_in)
            continue;
        if (same_id(other.registry.descriptor.id, added.descriptor.id))
            record.registry.conflicting = true;
    }
    record.catalogue = stored.result.catalogue;
    record.bytes = stored.catalogue_bytes;
    record.signature = stored.signature;
    record.digest = stored.digest;
    record.have_digest = stored.have_digest;
    record.served_from = stored.served_from;
    record.checked = now_seconds(state);
    record.expired = stored.expired;
    record.sources = sources_of(record);
    record.status = status_of(record, state.options.developer_mode);
    state.registries.push_back(std::move(record));
}

/// Drops a queued refresh of one registry. The caller holds the lock.
///
/// @param state the service
/// @param index the registry
void drop_queued_refresh(ServiceState& state, std::size_t index) {
    std::vector<WorkItem> kept;
    kept.reserve(state.queue.size());
    for (WorkItem& item : state.queue) {
        if (item.kind == WorkItem::Kind::refresh && item.index == index)
            continue;
        kept.push_back(std::move(item));
    }
    state.queue = std::move(kept);
}

/// Deletes one registry's cache folder, and nothing outside catalogues/.
///
/// @param state the service
/// @param id the registry id, which is the folder's name
void delete_one_registry_cache(const ServiceState& state, std::string_view id) {
    if (!one_segment(id))
        return;
    const fs::path content = content_directory(state);
    if (content.empty())
        return;
    const fs::path catalogues = content / std::string(catalogues_folder_name);
    const fs::path registry_cache = catalogues / std::string(id);
    if (registry_cache.parent_path() != catalogues)
        return;
    std::error_code error;
    fs::remove_all(registry_cache, error);
    if (error)
        log_line(state, "cache of " + std::string(id) + " was not removed: " + error.message());
}

/// Refuses a check that cannot be added. The caller holds the lock.
///
/// @param stored the check; null when the request is unknown
/// @param mirror true when a mirror is being added
/// @param[out] why why it was refused; may be null
/// @return the refusal, or none when the check may be added
registry::Refusal check_usable(const StoredCheck* stored, bool mirror, std::string* why) {
    if (stored == nullptr) {
        set_why(why, "This check is not known.");
        return registry::Refusal::unreadable;
    }
    if (!stored->done) {
        set_why(why, "This check has not finished.");
        return registry::Refusal::unreadable;
    }
    if (stored->used) {
        set_why(why, "This check was already used.");
        return registry::Refusal::unreadable;
    }
    if (stored->mirror != mirror) {
        set_why(why, mirror ? "This check is not a mirror." : "This check is not a registry.");
        return registry::Refusal::unreadable;
    }
    if (stored->result.refusal != registry::Refusal::none) {
        set_why(
            why,
            stored->result.detail.empty()
                ? std::string_view(registry::refusal_text(stored->result.refusal))
                : std::string_view(stored->result.detail)
        );
        return stored->result.refusal;
    }
    return registry::Refusal::none;
}

} // namespace

bool https_read_over_http() noexcept {
    return !netgame::http::tls_available();
}

uint64_t Service::check_registry_url(std::string url, std::string signing_key) {
    ServiceState& state = *impl_;
    const uint64_t request = begin_check(state, false);
    if (request == 0)
        return 0;
    run_check(
        *this,
        state,
        request,
        [&state, request, url = std::move(url), signing_key = std::move(signing_key)](
            WorkerTools& tools
        ) { complete_url_check(state, request, url, signing_key, tools); }
    );
    return request;
}

uint64_t Service::check_registry_file(std::vector<uint8_t> bytes) {
    ServiceState& state = *impl_;
    const uint64_t request = begin_check(state, false);
    if (request == 0)
        return 0;
    run_check(
        *this, state, request, [&state, request, bytes = std::move(bytes)](WorkerTools& tools) {
            complete_file_check(state, request, bytes, tools);
        }
    );
    return request;
}

std::optional<RegistryCheck> Service::registry_check(uint64_t request) const {
    base::threads::LockGuard guard(impl_->mutex);
    for (const StoredCheck& stored : impl_->checks) {
        if (stored.request == request && stored.done)
            return stored.result;
    }
    return std::nullopt;
}

uint64_t Service::check_mirror(std::string_view id, std::string url) {
    ServiceState& state = *impl_;
    const uint64_t request = begin_check(state, true);
    if (request == 0)
        return 0;
    run_check(
        *this,
        state,
        request,
        [&state, request, id = std::string(id), url = std::move(url)](WorkerTools& tools) {
            complete_mirror_check(state, request, id, url, tools);
        }
    );
    return request;
}

registry::Refusal Service::add_checked_registry(uint64_t request, std::string* why) {
    base::threads::LockGuard guard(impl_->mutex);
    if (!edits_open(*impl_, why))
        return registry::Refusal::unreadable;
    StoredCheck* stored = find_check(*impl_, request);
    const registry::Refusal usable = check_usable(stored, false, why);
    if (usable != registry::Refusal::none)
        return usable;
    if (!stored->result.descriptor || !stored->result.catalogue || !stored->served_from) {
        set_why(why, "This check has no catalogue.");
        return registry::Refusal::unreadable;
    }

    const std::string date = utc_date(now_seconds(*impl_));
    if (date.empty()) {
        set_why(why, "The date could not be stored.");
        return registry::Refusal::unreadable;
    }
    const std::vector<registry::Descriptor> builtins = builtins_of(*impl_);
    registry::PlayerRegistries updated = impl_->player;
    registry::AddContext rules;
    rules.built_ins = builtins;
    rules.player = &updated;
    rules.developer_mode = impl_->options.developer_mode;
    const registry::Refusal again = registry::check_new_registry(*stored->result.descriptor, rules);
    if (again != registry::Refusal::none) {
        set_why(why, registry::refusal_text(again));
        return again;
    }
    const registry::Refusal added = registry::add_registry(
        updated, *stored->result.descriptor, stored->result.url, date, rules
    );
    if (added != registry::Refusal::none) {
        set_why(why, registry::refusal_text(added));
        return added;
    }
    if (!save_player(*impl_, updated, why))
        return registry::Refusal::unreadable;

    impl_->player = std::move(updated);
    stored->used = true;
    const registry::Descriptor descriptor = *stored->result.descriptor;
    append_added(*impl_, *stored);
    write_cache(
        *impl_,
        descriptor.id,
        *stored->result.catalogue,
        stored->digest,
        now_seconds(*impl_),
        *stored->served_from,
        stored->catalogue_bytes,
        stored->signature,
        true
    );
    publish(*impl_);
    return registry::Refusal::none;
}

registry::Refusal Service::remove_registry(std::string_view id, std::string* why) {
    base::threads::LockGuard guard(impl_->mutex);
    if (!edits_open(*impl_, why))
        return registry::Refusal::unreadable;
    const std::optional<std::size_t> index = find_registry(*impl_, id);
    if (index && impl_->registries[*index].registry.origin == registry::Origin::built_in) {
        set_why(why, "can be turned off, not removed");
        return registry::Refusal::not_added;
    }

    std::string cache_id(id);
    for (const registry::AddedRegistry& added : impl_->player.registries) {
        if (same_id(added.descriptor.id, id))
            cache_id = added.descriptor.id;
    }
    registry::PlayerRegistries updated = impl_->player;
    const registry::Refusal removed = registry::remove_registry(updated, id);
    if (removed != registry::Refusal::none) {
        set_why(why, registry::refusal_text(removed));
        return removed;
    }
    if (!save_player(*impl_, updated, why))
        return registry::Refusal::unreadable;

    impl_->player = std::move(updated);
    if (index) {
        drop_queued_refresh(*impl_, *index);
        RegistryRecord& record = impl_->registries[*index];
        record.retired = true;
        record.refreshing = false;
        cache_id = record.registry.descriptor.id;
    }
    delete_one_registry_cache(*impl_, cache_id);
    publish(*impl_);
    return registry::Refusal::none;
}

registry::Refusal Service::set_registry_enabled(std::string_view id, bool on, std::string* why) {
    bool queued = false;
    bool worker_started = true;
    registry::Refusal refusal = registry::Refusal::none;
    {
        base::threads::LockGuard guard(impl_->mutex);
        if (!edits_open(*impl_, why))
            return registry::Refusal::unreadable;
        const std::vector<registry::Descriptor> builtins = builtins_of(*impl_);
        registry::PlayerRegistries updated = impl_->player;
        refusal = registry::set_enabled(updated, builtins, id, on);
        if (refusal != registry::Refusal::none) {
            set_why(why, registry::refusal_text(refusal));
            return refusal;
        }
        if (!save_player(*impl_, updated, why))
            return registry::Refusal::unreadable;
        impl_->player = std::move(updated);
        apply_player(*impl_);
        if (on) {
            const std::optional<std::size_t> index = find_registry(*impl_, id);
            if (index)
                queued =
                    queue_refresh(*impl_, *index, RefreshReason::check_now, now_seconds(*impl_));
        }
        if (queued)
            worker_started = ensure_worker(*impl_);
        publish(*impl_);
    }
    if (queued && !worker_started)
        log_line(*impl_, "the catalogue worker could not be started");
    return refusal;
}

registry::Refusal Service::add_checked_mirror(uint64_t request, std::string* why) {
    base::threads::LockGuard guard(impl_->mutex);
    if (!edits_open(*impl_, why))
        return registry::Refusal::unreadable;
    StoredCheck* stored = find_check(*impl_, request);
    const registry::Refusal usable = check_usable(stored, true, why);
    if (usable != registry::Refusal::none)
        return usable;
    if (!stored->result.descriptor || stored->mirror_url.host.empty()) {
        set_why(why, "This check has no mirror.");
        return registry::Refusal::unreadable;
    }

    const std::vector<registry::Descriptor> builtins = builtins_of(*impl_);
    registry::PlayerRegistries updated = impl_->player;
    registry::AddContext rules;
    rules.built_ins = builtins;
    rules.player = &updated;
    rules.developer_mode = impl_->options.developer_mode;
    const std::string id = stored->result.descriptor->id;
    const registry::Refusal again = registry::check_new_mirror(id, stored->mirror_url, rules);
    if (again != registry::Refusal::none) {
        set_why(why, registry::refusal_text(again));
        return again;
    }
    const registry::Refusal added =
        registry::add_player_mirror(updated, id, stored->mirror_url, rules);
    if (added != registry::Refusal::none) {
        set_why(why, registry::refusal_text(added));
        return added;
    }
    if (!save_player(*impl_, updated, why))
        return registry::Refusal::unreadable;

    impl_->player = std::move(updated);
    stored->used = true;
    apply_player(*impl_);
    publish(*impl_);
    return registry::Refusal::none;
}

} // namespace oa::app::content
