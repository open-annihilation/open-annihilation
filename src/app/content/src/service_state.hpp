// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The service's private state, shared by the snapshot, the worker, the
// refresh and the cache. Not a public header.
#pragma once

#include "oa/app/content/service.hpp"

#include "oa/base/sha256.hpp"
#include "oa/base/threads.hpp"
#include "oa/data/catalogue/check.hpp"
#include "oa/data/catalogue/releases.hpp"
#include "oa/data/registry/player_registries.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::content {

/// One registry the service keeps, including bytes the snapshot does not show.
struct RegistryRecord {
    data::registry::RegistryInEffect registry{};
    RegistryStatus status = RegistryStatus::never_fetched;
    std::shared_ptr<const data::catalogue::Catalogue> catalogue{};
    std::vector<uint8_t> bytes{};
    std::vector<uint8_t> signature{};
    base::sha256::Digest digest{};
    bool have_digest = false;
    std::optional<formats::url::Url> served_from{};
    std::vector<formats::url::Url> sources{};
    std::optional<int64_t> checked{};
    std::optional<int64_t> last_attempt{};
    bool expired = false;
    bool refreshing = false;
    bool refresh_failed = false;
    std::string last_error{};
    bool retired = false; ///< removed from the list; the slot stays so a refresh keeps its index
    /// The releases list accepted for `releases_digest`. Null when none is held.
    std::shared_ptr<const data::catalogue::ReleasesList> releases{};
    /// The catalogue member digest `releases` was accepted against. Absent when none is held.
    std::optional<base::sha256::Digest> releases_digest{};
};

/// One registry or mirror check kept until the player adds it or the service stops.
struct StoredCheck {
    uint64_t request = 0; ///< the id returned to the caller
    bool done = false;    ///< true once the worker has stored the outcome
    bool used = false;    ///< true once an add consumed this check
    bool mirror = false;  ///< true when check_mirror queued it
    bool expired = false; ///< the catalogue is past expires; it was still usable
    RegistryCheck result{};
    std::vector<uint8_t> catalogue_bytes{}; ///< the exact bytes the check accepted
    std::vector<uint8_t> signature{};       ///< the signature file; empty when there was none
    base::sha256::Digest digest{};          ///< the SHA-256 of the catalogue bytes
    bool have_digest = false;               ///< true when digest was computed
    std::optional<formats::url::Url> served_from{}; ///< the catalogue address that verified
    formats::url::Url mirror_url{};                 ///< the mirror, when this check is one
};

/// A refresh or a posted job, in the order it was queued.
struct WorkItem {
    enum class Kind : uint8_t { refresh, job };

    Kind kind = Kind::job;
    std::size_t index = 0; ///< the registry, when kind is refresh
    WorkerJob job{};       ///< the posted job, when kind is job
};

/// The service's state. The mutex guards every field a thread shares,
/// except `cancel`, `options.data_folder` and the function pointers, which
/// do not change after the service is made (`developer_mode` and `check` do).
struct ServiceState {
    /// Stores the options and an empty snapshot of generation 0.
    ///
    /// @param options_in the folders, the setting and the clock
    explicit ServiceState(ServiceOptions options_in);

    ServiceOptions options;
    std::atomic<bool> cancel{false};
    mutable base::threads::Mutex mutex{};
    base::threads::ConditionVariable wake{};
    base::threads::Thread thread{};
    bool thread_started = false;
    bool started = false;
    bool stop = false;
    std::vector<RegistryRecord> registries{};
    data::registry::PlayerRegistries player{};
    data::registry::LoadResult player_load = data::registry::LoadResult::missing;
    std::string registries_file_error{};
    std::vector<WorkItem> queue{};
    std::vector<RefreshListener> listeners{};
    std::shared_ptr<const Snapshot> snapshot{};
    uint64_t generation = 0;
    uint64_t next_check = 0;           ///< the last request id handed out
    std::vector<StoredCheck> checks{}; ///< checks, including ones still running
};

/// Returns the clock the service was given, or the system clock.
///
/// @param state the service
/// @return seconds since 1970
[[nodiscard]] int64_t now_seconds(const ServiceState& state) noexcept;

/// Writes one line through the service's log, when it has one.
///
/// @param state the service
/// @param line the line, without a line end
void log_line(const ServiceState& state, std::string_view line);

/// Returns `<data>/content`, or an empty path when there is no data folder.
///
/// @param state the service
/// @return the content folder
[[nodiscard]] std::filesystem::path content_directory(const ServiceState& state);

/// Reports whether a registry may be fetched in this Developer mode.
///
/// @param record the registry
/// @param developer_mode true when Developer mode is on
/// @return true when a refresh may try it
[[nodiscard]] bool can_fetch(const RegistryRecord& record, bool developer_mode);

/// The status a record shows for this Developer mode.
///
/// @param record the registry
/// @param developer_mode true when Developer mode is on
/// @return the status
[[nodiscard]] RegistryStatus status_of(const RegistryRecord& record, bool developer_mode);

/// The catalogue URLs a refresh tries, duplicates dropped.
///
/// @param record the registry, including a cached catalogue when it has one
/// @return the URLs, in the order a refresh tries them
[[nodiscard]] std::vector<formats::url::Url> sources_of(const RegistryRecord& record);

/// Copies the fields a snapshot and a listener show.
///
/// @param record the registry
/// @return the view
[[nodiscard]] RegistryView view_of(const RegistryRecord& record);

/// Publishes a new snapshot. The caller holds the service's mutex.
///
/// A registry that has been removed is left out.
///
/// @param state the service
void publish(ServiceState& state);

/// Queues one registry when the rules say so. The caller holds the mutex.
///
/// A removed registry is not queued.
///
/// @param state the service
/// @param index the registry
/// @param reason why the refresh was asked for
/// @param now the clock, seconds since 1970
/// @return true when a refresh was queued
bool queue_refresh(ServiceState& state, std::size_t index, RefreshReason reason, int64_t now);

/// Reads one registry's cache into the record when the cached bytes are usable.
///
/// A cache that fails the check is left unused and is not deleted.
///
/// @param state the service
/// @param record the registry; its catalogue is set only when the cache is usable
void read_cache(ServiceState& state, RegistryRecord& record);

/// Writes an accepted catalogue, or only the state file when the bytes did not change.
///
/// @param state the service
/// @param id the registry id
/// @param catalogue the catalogue that was kept
/// @param digest the SHA-256 of the catalogue bytes
/// @param checked when the catalogue was confirmed, seconds since 1970
/// @param source the URL that served it
/// @param catalogue_bytes the catalogue bytes
/// @param signature the signature bytes; empty when there are none
/// @param write_catalogue true to replace the catalogue and signature files too
void write_cache(
    const ServiceState& state,
    std::string_view id,
    const data::catalogue::Catalogue& catalogue,
    const base::sha256::Digest& digest,
    int64_t checked,
    const formats::url::Url& source,
    std::span<const uint8_t> catalogue_bytes,
    std::span<const uint8_t> signature,
    bool write_catalogue
);

/// Writes a releases list beside the cached catalogue.
///
/// The file is replaced only when a fetch was accepted. A failed fetch leaves
/// the file that is already there.
///
/// @param state the service
/// @param id the registry id
/// @param bytes the list file's bytes
void write_releases_cache(
    const ServiceState& state, std::string_view id, std::span<const uint8_t> bytes
);

/// Fetches one registry and publishes the snapshot. Runs on the worker.
///
/// @param state the service
/// @param index the registry's place in the list
/// @param http the worker's client
void refresh_registry(ServiceState& state, std::size_t index, netgame::http::Client& http);

/// Clears the refreshing flag, and publishes unless the service is stopping.
///
/// @param state the service
/// @param index the registry's place in the list
/// @param publish_change true to publish when the service is not stopping
void clear_refreshing(ServiceState& state, std::size_t index, bool publish_change);

/// Starts the worker for the first queued job. The caller holds the mutex.
///
/// On failure the queue is dropped and refreshing flags are cleared.
///
/// @param state the service
/// @return false when a thread was needed and could not be started
[[nodiscard]] bool ensure_worker(ServiceState& state);

/// The worker's thread entry.
///
/// @param argument the service state
void worker_entry(void* argument);

/// The service object's state. Defined here so each part of the service can
/// reach the lock, the registries and the cache.
struct Service::Impl : ServiceState {
    using ServiceState::ServiceState;
};

} // namespace oa::app::content
