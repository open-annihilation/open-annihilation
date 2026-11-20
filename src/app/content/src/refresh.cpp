// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One registry's refresh: each source in order, the check, the cache and a
// key rotation.
#include "reach.hpp"
#include "service_state.hpp"

#include <cstdint>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app::content {
namespace {

/// What a refresh needs that must not change while the fetch runs.
struct Plan {
    std::string id{};
    std::vector<formats::url::Url> sources{};
    std::vector<data::registry::RegistryKey> keys{};
    bool built_in = false;
    bool added = false;
    data::catalogue::Previous previous{};
    int64_t now = 0;
    bool player_writable = false;
    data::registry::PlayerRegistries player{};
    std::filesystem::path registries_file{};
};

/// A catalogue a source served that the check accepted.
struct Kept {
    bool ok = false;
    bool unchanged = false;
    std::vector<uint8_t> bytes{};
    std::vector<uint8_t> signature{};
    std::shared_ptr<const data::catalogue::Catalogue> catalogue{};
    base::sha256::Digest digest{};
    formats::url::Url served_from{};
    bool expired = false;
    std::optional<std::vector<data::registry::RegistryKey>> new_keys{};
};

/// Joins the reasons a refresh failed, for the registry's last error.
///
/// @param errors one reason per source that was tried
/// @return the reasons separated by a semicolon, or a short fallback
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

/// Logs one source's outcome.
///
/// @param state the service
/// @param id the registry id
/// @param url the source
/// @param outcome the outcome, after the colon
void log_outcome(
    ServiceState& state, std::string_view id, const formats::url::Url& url, std::string_view outcome
) {
    std::string line = "catalogue of ";
    line += id;
    line += " from ";
    line += formats::url::url_text(url);
    line += ": ";
    line += outcome;
    log_line(state, line);
}

/// Fetches one address into memory.
///
/// @param http the worker's client
/// @param url the address
/// @param gzip true to accept a gzip body
/// @param limit the most decoded body bytes
/// @param cancel the service's cancel flag
/// @param body the decoded body; cleared first
/// @return the client's outcome
netgame::http::Response fetch_bytes(
    netgame::http::Client& http,
    const formats::url::Url& url,
    bool gzip,
    uint64_t limit,
    const std::atomic<bool>* cancel,
    std::vector<uint8_t>& body
) {
    netgame::http::Request request;
    request.url = formats::url::url_text(url);
    request.accept_gzip = gzip;
    request.limits.max_body_bytes = limit;
    request.timeouts.connect_ms = 10000;
    request.timeouts.idle_ms = 30000;
    request.timeouts.total_ms = 120000;
    request.cancel = cancel;
    return http.fetch(request, body);
}

/// Copies the fields a refresh needs. The caller holds the mutex.
///
/// @param state the service
/// @param index the registry
/// @param plan filled on success
/// @return false when the registry is gone or the service is stopping
bool take_plan(ServiceState& state, std::size_t index, Plan& plan) {
    if (state.stop || index >= state.registries.size())
        return false;
    const RegistryRecord& record = state.registries[index];
    plan.id = record.registry.descriptor.id;
    plan.sources = record.sources;
    plan.keys = record.registry.descriptor.keys;
    plan.built_in = record.registry.origin == data::registry::Origin::built_in;
    plan.added = record.registry.origin == data::registry::Origin::added;
    if (record.catalogue && record.have_digest) {
        plan.previous.sequence = record.catalogue->sequence;
        plan.previous.digest = record.digest;
    }
    plan.now = now_seconds(state);
    plan.player_writable = state.player_load != data::registry::LoadResult::unreadable &&
                           !state.options.player_folder.empty();
    plan.player = state.player;
    plan.registries_file =
        state.options.player_folder / std::string(data::registry::player_registries_file);
    return true;
}

/// Tries each source until one catalogue is usable, or the service stops.
///
/// @param state the service
/// @param http the worker's client
/// @param plan the registry's sources and keys
/// @param kept set when a source was usable
/// @param errors one reason per source that was not usable
/// @return false when the fetch was cancelled
bool try_sources(
    ServiceState& state,
    netgame::http::Client& http,
    const Plan& plan,
    Kept& kept,
    std::vector<std::string>& errors
) {
    for (const formats::url::Url& source : plan.sources) {
        if (state.cancel.load())
            return false;
        std::vector<uint8_t> body;
        const netgame::http::Response catalogue = fetch_bytes(
            http, source, true, data::catalogue::max_catalogue_bytes, &state.cancel, body
        );
        if (catalogue.failure == netgame::http::Failure::cancelled || state.cancel.load())
            return false;
        if (catalogue.failure != netgame::http::Failure::none) {
            const std::string outcome = reach_detail(catalogue);
            log_outcome(state, plan.id, source, outcome);
            errors.push_back(outcome);
            continue;
        }
        if (catalogue.status != 200) {
            const std::string outcome =
                "refused, the server answered " + std::to_string(catalogue.status);
            log_outcome(state, plan.id, source, outcome);
            errors.push_back(outcome);
            continue;
        }

        // A missing signature is empty bytes. An unsigned registry stays
        // usable; a signed one is refused by the check and the next source
        // is tried.
        const std::optional<formats::url::Url> signature_url =
            formats::url::parse_http_url(formats::url::url_text(source) + ".sig");
        std::vector<uint8_t> signature;
        netgame::http::Response signature_response;
        if (signature_url)
            signature_response =
                fetch_bytes(http, *signature_url, false, 4 * 1024, &state.cancel, signature);
        if (signature_response.failure == netgame::http::Failure::cancelled || state.cancel.load())
            return false;
        if (signature_response.failure != netgame::http::Failure::none ||
            signature_response.status != 200)
            signature.clear();

        data::catalogue::CheckRequest request;
        request.catalogue = body;
        request.signature = signature;
        request.registry_id = plan.id;
        request.trusted_keys = plan.keys;
        request.built_in = plan.built_in;
        request.previous = plan.previous;
        request.now = plan.now;
        const data::catalogue::Checked checked = data::catalogue::check_catalogue(request);
        if (!checked.usable() || !checked.catalogue) {
            const std::string outcome =
                std::string("refused, ") + data::catalogue::verdict_text(checked.verdict);
            log_outcome(state, plan.id, source, outcome);
            errors.push_back(outcome);
            continue;
        }

        const char* const verb =
            checked.verdict == data::catalogue::Verdict::unchanged ? "unchanged" : "accepted";
        log_outcome(
            state,
            plan.id,
            source,
            std::string(verb) + ", sequence " + std::to_string(checked.catalogue->sequence)
        );
        for (const std::string& problem : checked.catalogue->problems) {
            if (problem.rfind("releases", 0) == 0)
                log_outcome(state, plan.id, source, "left out " + problem);
        }
        kept.ok = true;
        kept.unchanged = checked.verdict == data::catalogue::Verdict::unchanged;
        kept.bytes = std::move(body);
        kept.signature = std::move(signature);
        kept.digest = checked.digest;
        kept.served_from = source;
        kept.expired = checked.expired;
        kept.new_keys = checked.new_keys;
        kept.catalogue = checked.catalogue;
        return true;
    }
    return true;
}

/// Writes new trusted keys when the catalogue published them and the file can be written.
///
/// @param state the service
/// @param plan the player's registries as they were at the start of the fetch
/// @param kept the catalogue that was kept
/// @param updated the registries to store when the save succeeded
/// @return true when the file and `updated` hold the new keys
bool save_rotation(
    ServiceState& state,
    const Plan& plan,
    const Kept& kept,
    data::registry::PlayerRegistries& updated
) {
    if (!kept.new_keys || !plan.added)
        return false;
    if (!plan.player_writable) {
        log_line(state, "the trusted keys of " + plan.id + " were not saved");
        return false;
    }
    updated = plan.player;
    const data::registry::Refusal refusal =
        data::registry::replace_trusted_keys(updated, plan.id, *kept.new_keys);
    if (refusal != data::registry::Refusal::none) {
        log_line(
            state,
            "the trusted keys of " + plan.id +
                " were not saved: " + data::registry::refusal_text(refusal)
        );
        return false;
    }
    std::string error;
    if (!data::registry::save_player_registries(plan.registries_file, updated, &error)) {
        log_line(state, "the trusted keys of " + plan.id + " were not saved: " + error);
        return false;
    }
    return true;
}

/// Stores a usable catalogue and publishes. The caller does not hold the mutex.
///
/// @param state the service
/// @param index the registry
/// @param plan the clock and the player's list
/// @param kept the catalogue
/// @param saved_keys true when the trusted keys were written
/// @param updated the player's list after the rotation
/// @param http the worker's client, for the listeners
void publish_kept(
    ServiceState& state,
    std::size_t index,
    const Plan& plan,
    Kept& kept,
    bool saved_keys,
    data::registry::PlayerRegistries updated,
    netgame::http::Client& http
) {
    std::vector<RefreshListener> listeners;
    RegistryView view;
    {
        base::threads::LockGuard guard(state.mutex);
        if (state.stop || index >= state.registries.size())
            return;
        RegistryRecord& record = state.registries[index];
        record.bytes = std::move(kept.bytes);
        record.signature = std::move(kept.signature);
        record.digest = kept.digest;
        record.have_digest = true;
        record.catalogue = std::move(kept.catalogue);
        record.served_from = kept.served_from;
        record.checked = plan.now;
        record.expired = kept.expired;
        record.last_error.clear();
        record.refresh_failed = false;
        record.refreshing = false;
        if (saved_keys && kept.new_keys) {
            record.registry.descriptor.keys = *kept.new_keys;
            state.player = std::move(updated);
        }
        record.sources = sources_of(record);
        record.status = status_of(record, state.options.developer_mode);
        publish(state);
        listeners = state.listeners;
        view = view_of(record);
    }
    WorkerTools tools{http, &state.cancel, content_directory(state)};
    for (const RefreshListener& listener : listeners) {
        if (state.cancel.load())
            return;
        try {
            listener(view, tools);
        } catch (const std::exception& error) {
            log_line(state, std::string("a catalogue listener failed: ") + error.what());
        } catch (...) {
            log_line(state, "a catalogue listener failed");
        }
    }
}

/// Stores a failed refresh and publishes. The caller does not hold the mutex.
///
/// @param state the service
/// @param index the registry
/// @param errors the reasons the sources were not usable
void publish_failed(
    ServiceState& state, std::size_t index, const std::vector<std::string>& errors
) {
    base::threads::LockGuard guard(state.mutex);
    if (state.stop || index >= state.registries.size())
        return;
    RegistryRecord& record = state.registries[index];
    record.refreshing = false;
    record.refresh_failed = true;
    record.last_error = join_errors(errors);
    record.status = status_of(record, state.options.developer_mode);
    publish(state);
}

/// Fetches the releases list once per new digest and stores it on the registry.
///
/// A catalogue with no `releases` member drops the list that was held. A
/// digest already held is not fetched again. A failed or mismatched fetch
/// keeps the list that was held. The caller does not hold the mutex.
///
/// @param state the service
/// @param index the registry
/// @param http the worker's client
/// @param plan the registry's sources
/// @param kept the catalogue just accepted
void refresh_releases(
    ServiceState& state,
    std::size_t index,
    netgame::http::Client& http,
    const Plan& plan,
    const Kept& kept
) {
    data::catalogue::HeldReleases held;
    {
        base::threads::LockGuard guard(state.mutex);
        if (state.stop || index >= state.registries.size())
            return;
        const RegistryRecord& record = state.registries[index];
        held.digest = record.releases_digest;
        if (record.releases)
            held.list = *record.releases;
    }
    data::catalogue::HeldReleases next;
    std::vector<uint8_t> accepted;
    bool write = false;
    const bool named = kept.catalogue && kept.catalogue->releases.has_value();
    if (!named) {
        next = {};
    } else if (held.digest.has_value() && *held.digest == kept.catalogue->releases->sha256) {
        next = std::move(held);
    } else {
        const data::catalogue::ReleasesRef& member = *kept.catalogue->releases;
        RegistryView view;
        view.catalogue = kept.catalogue;
        view.served_from = kept.served_from;
        view.sources = plan.sources;
        const std::vector<formats::url::Url> urls = reference_urls(view, member.file);
        bool got = false;
        for (const formats::url::Url& url : urls) {
            if (state.cancel.load())
                break;
            std::vector<uint8_t> body;
            const netgame::http::Response response = fetch_bytes(
                http, url, true, data::catalogue::max_catalogue_bytes, &state.cancel, body
            );
            if (response.failure == netgame::http::Failure::cancelled || state.cancel.load())
                break;
            if (response.failure != netgame::http::Failure::none || response.status != 200) {
                log_outcome(state, plan.id, url, "the releases list was not fetched");
                continue;
            }
            data::catalogue::ReleasesTake how = data::catalogue::ReleasesTake::mismatch;
            data::catalogue::HeldReleases taken =
                data::catalogue::take_releases(member, body, held, &how);
            if (how == data::catalogue::ReleasesTake::taken) {
                next = std::move(taken);
                accepted = std::move(body);
                write = true;
                got = true;
                log_outcome(state, plan.id, url, "releases list accepted");
                break;
            }
            if (how == data::catalogue::ReleasesTake::ignored) {
                log_outcome(state, plan.id, url, "releases list ignored");
                next = held;
                got = true;
                break;
            }
            log_outcome(state, plan.id, url, "releases list did not match the catalogue");
        }
        if (!got)
            next = std::move(held);
    }
    if (write)
        write_releases_cache(state, plan.id, accepted);
    {
        base::threads::LockGuard guard(state.mutex);
        if (state.stop || index >= state.registries.size())
            return;
        RegistryRecord& record = state.registries[index];
        if (next.digest.has_value()) {
            record.releases_digest = next.digest;
            record.releases =
                std::make_shared<const data::catalogue::ReleasesList>(std::move(next.list));
        } else {
            record.releases.reset();
            record.releases_digest.reset();
        }
    }
}

} // namespace

void refresh_registry(ServiceState& state, std::size_t index, netgame::http::Client& http) {
    Plan plan;
    {
        base::threads::LockGuard guard(state.mutex);
        if (!take_plan(state, index, plan)) {
            if (index < state.registries.size())
                state.registries[index].refreshing = false;
            return;
        }
    }

    Kept kept;
    std::vector<std::string> errors;
    if (!try_sources(state, http, plan, kept, errors)) {
        clear_refreshing(state, index, false);
        return;
    }
    if (state.cancel.load()) {
        clear_refreshing(state, index, false);
        return;
    }

    if (kept.ok && kept.catalogue) {
        write_cache(
            state,
            plan.id,
            *kept.catalogue,
            kept.digest,
            plan.now,
            kept.served_from,
            kept.bytes,
            kept.signature,
            !kept.unchanged
        );
        data::registry::PlayerRegistries updated;
        const bool saved_keys = save_rotation(state, plan, kept, updated);
        refresh_releases(state, index, http, plan, kept);
        publish_kept(state, index, plan, kept, saved_keys, std::move(updated), http);
        return;
    }
    publish_failed(state, index, errors);
}

} // namespace oa::app::content
