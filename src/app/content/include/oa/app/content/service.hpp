// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The registries in effect and the catalogues cached for them. Fetches run
// on one worker thread. The main thread reads an immutable snapshot.
#pragma once

#include "oa/data/catalogue/catalogue.hpp"
#include "oa/data/catalogue/releases.hpp"
#include "oa/data/registry/trust.hpp"
#include "oa/formats/url.hpp"
#include "oa/netgame/http/client.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app::content {

/// The folder under the data folder that holds cached catalogues.
inline constexpr std::string_view content_folder_name = "content";

/// True when this system reads an https registry address as http.
///
/// Settings › Downloads says so once when this is true. The game still
/// starts where the system has no TLS library.
///
/// @return true when the HTTP client has no TLS
[[nodiscard]] bool https_read_over_http() noexcept;

/// The folder under the content folder, one directory per registry.
inline constexpr std::string_view catalogues_folder_name = "catalogues";

/// The folder of built-in registry descriptors, beside the program.
inline constexpr std::string_view builtin_registries_folder_name = "registries";

/// How old a confirmed catalogue may be before a start refreshes it, in seconds.
inline constexpr int64_t start_refresh_age_seconds = 6 * 60 * 60;

/// The least time between two refreshes opened with the Library, in seconds.
inline constexpr int64_t library_refresh_gap_seconds = 60;

/// Milliseconds a registry check may spend resolving a name and opening the connection.
inline constexpr uint32_t registry_check_connect_ms = 10000;

/// Milliseconds a registry check may wait for the next byte to be sent or to arrive.
inline constexpr uint32_t registry_check_idle_ms = 30000;

/// Milliseconds a registry-address check may spend fetching the descriptor.
inline constexpr uint32_t registry_check_descriptor_ms = 60000;

/// Milliseconds a check may spend fetching one catalogue.
inline constexpr uint32_t registry_check_catalogue_ms = 120000;

/// When the player wants catalogues refreshed.
enum class CheckForUpdates : uint8_t {
    automatically, ///< at start, when the Library opens, and on Check now
    library_only,  ///< when the Library opens, and on Check now
    never,         ///< only on Check now
};

/// Why a refresh was asked for.
enum class RefreshReason : uint8_t {
    start,          ///< the game is starting
    library_opened, ///< the player opened the Library
    check_now,      ///< the player chose Check now
};

/// Reports whether a refresh of one registry is due.
///
/// A start is due only when `check` is automatically and the registry has no
/// checked time, or that time is more than start_refresh_age_seconds before
/// `now`. A gap of exactly that age is not due. Opening the Library is due
/// unless `check` is never, and not again until library_refresh_gap_seconds
/// have passed since `last_attempt`; a gap of exactly that long is due.
/// Check now is always due. A clock that moved backwards is not due. A
/// headless or unattended run is the caller's to refuse: this function does
/// not know it. Disabled, conflicting and unsigned registries are too.
///
/// @param reason why the refresh was asked for
/// @param check the player's setting
/// @param now the caller's clock, seconds since 1970
/// @param checked when a refresh last found a usable catalogue; empty if never
/// @param last_attempt when a refresh was last queued; empty if never
/// @return true when the rules say to fetch
[[nodiscard]] bool refresh_due(
    RefreshReason reason,
    CheckForUpdates check,
    int64_t now,
    std::optional<int64_t> checked,
    std::optional<int64_t> last_attempt
) noexcept;

/// A function that receives one log line, without a line end.
using LogSink = void (*)(void* context, std::string_view line);

/// Seconds since 1970. A null clock uses the system clock.
using Clock = int64_t (*)();

/// What the game knows when it makes the content service.
struct ServiceOptions {
    std::filesystem::path data_folder{};    ///< empty: catalogues stay in memory
    std::filesystem::path player_folder{};  ///< holds Registries.yaml
    std::filesystem::path builtin_folder{}; ///< descriptors shipped with the game
    bool developer_mode = false;            ///< unsigned loopback registries may be fetched
    bool automatic = true;                  ///< false skips the refresh at start
    CheckForUpdates check = CheckForUpdates::automatically; ///< the player's setting
    netgame::http::ClientOptions http{};                    ///< the worker's client
    LogSink log = nullptr;                                  ///< null discards lines
    void* log_context = nullptr;                            ///< passed back to log
    Clock clock = nullptr;                                  ///< null uses the system clock
};

/// Where one registry's catalogue stands.
enum class RegistryStatus : uint8_t {
    never_fetched,        ///< no usable catalogue yet
    fresh,                ///< a catalogue that has not expired
    out_of_date,          ///< a usable catalogue past its expires
    failed,               ///< the last refresh found nothing usable
    needs_developer_mode, ///< unsigned while Developer mode is off
    conflicting,          ///< an added id a built-in registry has taken
    disabled,             ///< the player turned the registry off
};

/// One registry in effect, and the catalogue kept for it.
struct RegistryView {
    data::registry::RegistryInEffect registry{}; ///< the descriptor and where it comes from
    RegistryStatus status = RegistryStatus::never_fetched;         ///< where the catalogue stands
    std::shared_ptr<const data::catalogue::Catalogue> catalogue{}; ///< null until one is usable
    std::optional<formats::url::Url> served_from{}; ///< the host that served the catalogue
    std::vector<formats::url::Url> sources{}; ///< catalogue URLs, in the order a refresh tries
    std::optional<int64_t> checked{};         ///< when a refresh last found a usable catalogue
    bool expired = false;                     ///< the catalogue is past expires; it still serves
    bool refreshing = false;                  ///< a fetch is queued or running
    std::string last_error{}; ///< why the last refresh found nothing; empty after a good one
    std::vector<std::string> fingerprints{}; ///< the trusted keys, as a player compares them
    /// Older releases named by the catalogue's `releases` member. Null when
    /// the catalogue names none, or the list has not been fetched yet. A
    /// failed fetch keeps the list that was already here.
    std::shared_ptr<const data::catalogue::ReleasesList> releases{};
};

/// One package in the merged snapshot.
struct Entry {
    std::string registry{}; ///< the registry id the package was listed under
    const data::catalogue::Package* package = nullptr; ///< into a catalogue the snapshot keeps
    bool reviewed = false; ///< true when the package comes from a built-in registry
};

/// The lists the main thread reads. Entries point into the catalogues the
/// views keep alive.
struct Snapshot {
    uint64_t generation = 0;                ///< moves on with every new snapshot
    std::vector<RegistryView> registries{}; ///< every registry in effect
    std::vector<Entry> entries{};           ///< packages from registries that may give them
    std::string registries_file_error{};    ///< set when Registries.yaml could not be read
};

/// Builds the merged snapshot from registry views.
///
/// Entries come only from a registry that is enabled, not conflicting,
/// allowed by Developer mode and holding a catalogue, in registry order and
/// then catalogue order. `reviewed` is set for a built-in registry.
/// `generation` is left 0; the service sets it when it publishes.
///
/// @param registries the views, in the order they are listed
/// @param registries_file_error the load error, or empty
/// @return the snapshot; entries point into the catalogues the views keep
[[nodiscard]] Snapshot
make_snapshot(std::vector<RegistryView> registries, std::string registries_file_error = {});

/// Resolves a catalogue reference against the hosts that serve the registry.
///
/// `served_from` is tried first, then each URL of `sources`, and a URL
/// already listed is dropped. A base that does not resolve the reference is
/// skipped. Nothing is returned when the registry has no catalogue.
///
/// @param view the registry, including the catalogue and its hosts
/// @param reference a catalogue reference, root or relative
/// @return the addresses a download may try, in that order
[[nodiscard]] std::vector<formats::url::Url>
reference_urls(const RegistryView& view, std::string_view reference);

/// What a job on the worker may use. The client is the worker's own.
struct WorkerTools {
    netgame::http::Client& http;               ///< the worker's client; one thread uses it
    const std::atomic<bool>* cancel = nullptr; ///< set when the service is stopping
    std::filesystem::path content_folder{};    ///< `<data>/content`; empty without a data folder
};

/// A job the worker runs, in the order it was queued.
using WorkerJob = std::function<void(WorkerTools&)>;

/// Called on the worker after a refresh that found a usable catalogue.
using RefreshListener = std::function<void(const RegistryView& view, WorkerTools& tools)>;

/// The outcome of checking a registry or a mirror before it is trusted.
///
/// `registry_check` returns nothing while the check is still running, and
/// the same shape for an address, a descriptor file and a mirror. A refused
/// check still carries the descriptor and the address once a descriptor was
/// read. A file has no `url`.
struct RegistryCheck {
    uint64_t request = 0; ///< the id the check call returned
    /// none when the registry or the mirror may be added
    data::registry::Refusal refusal = data::registry::Refusal::none;
    /// The catalogue's verdict, or why the fetch failed. Empty when neither applies.
    std::string detail;
    /// The descriptor that was read. Empty only when no descriptor was read.
    std::optional<data::registry::Descriptor> descriptor;
    std::string url;     ///< the address that was checked; empty for a file
    std::string address; ///< the host a player is shown, and the port when it is not 80
    std::vector<std::string> fingerprints; ///< the keys, as a player compares them
    std::size_t packages = 0;              ///< packages in a catalogue that verified
    /// How many packages of each kind the catalogue holds. Kinds with none are left out.
    std::vector<std::pair<data::catalogue::Kind, std::size_t>> kinds;
    /// The catalogue. Set only when it verified with the keys being trusted.
    std::shared_ptr<const data::catalogue::Catalogue> catalogue;
    /// The catalogue address that verified, so a picture reference resolves
    /// against the host that served it. Empty when no catalogue verified.
    std::optional<formats::url::Url> served_from;
};

/// Holds the registries, the cache and the worker that refreshes them.
///
/// Every method may be called from the main thread. None waits on the
/// network. `snapshot` copies a pointer and returns.
class Service {
  public:

    /// Stores the options. No thread runs and no file is read yet.
    ///
    /// @param options the folders, the setting and the clock
    explicit Service(ServiceOptions options);

    /// Stops the worker.
    ///
    /// Sets the cancel flag the client's fetches read, drops the jobs that
    /// have not started and the listeners, wakes the worker and joins it.
    ~Service();

    Service(const Service&) = delete;
    Service& operator=(const Service&) = delete;
    Service(Service&&) = delete;
    Service& operator=(Service&&) = delete;

    /// Reads the registries and the cache, and queues a start refresh where one is due.
    ///
    /// The files are small and are read on the calling thread. A second call
    /// does nothing. The first snapshot is published before this returns,
    /// including when no registry is in effect.
    void start();

    /// Queues a refresh of one registry, or of every registry the rules allow.
    ///
    /// An empty `registry` refreshes each registry the rules allow. An id
    /// refreshes that registry only. A registry already queued or being
    /// fetched is not queued again. A call before start does nothing. The
    /// call returns without waiting for the fetch.
    ///
    /// @param reason why the refresh was asked for
    /// @param registry a registry id, or empty for every registry the rules allow
    void refresh(RefreshReason reason, std::string_view registry = {});

    /// Remembers Developer mode and refreshes unsigned registries that may now be fetched.
    ///
    /// Turning it off publishes again so those registries' packages leave the
    /// snapshot. A registry that becomes fetchable and has no catalogue is
    /// queued even when a start refresh would not be due.
    ///
    /// @param on true when Developer mode is on
    void set_developer_mode(bool on);

    /// Stores the player's setting. Nothing is fetched.
    ///
    /// @param check the setting
    void set_check_for_updates(CheckForUpdates check);

    /// Queues a job behind the jobs already queued. It runs on the worker only.
    ///
    /// The worker is started for the first job. The job may call snapshot
    /// and run_on_worker. A job that has not started when the service is
    /// destroyed is dropped.
    ///
    /// @param job the job; empty is ignored
    void run_on_worker(WorkerJob job);

    /// Adds a listener called on the worker after a refresh found a usable catalogue.
    ///
    /// The listener runs after the snapshot is published, and not while the
    /// service's lock is held, so it may call snapshot and run_on_worker. A
    /// refresh that found nothing usable does not call it. Listeners still
    /// waiting when the service is destroyed are dropped.
    ///
    /// @param listener the listener; empty is ignored
    void on_refreshed(RefreshListener listener);

    /// Returns the content folder, or an empty path when there is no data folder.
    ///
    /// @return `<data folder>/content`, or empty
    [[nodiscard]] std::filesystem::path content_folder() const;

    /// Returns the snapshot the main thread may read.
    ///
    /// Does not wait on a fetch. The snapshot stays valid for as long as the
    /// caller holds the pointer.
    ///
    /// @return the latest snapshot; never null
    [[nodiscard]] std::shared_ptr<const Snapshot> snapshot() const;

    /// Returns the generation of the latest snapshot.
    ///
    /// It moves on with every new snapshot and not otherwise. Before start
    /// it is 0.
    ///
    /// @return the generation
    [[nodiscard]] uint64_t generation() const noexcept;

    /// Checks a registry at an address, on the worker.
    ///
    /// The descriptor is fetched and read, every rule for a new registry is
    /// applied, and the catalogue is fetched and checked with the keys about
    /// to be trusted. A signing key replaces the descriptor's keys: the
    /// catalogue must verify with that key alone, and it is the only key
    /// stored. An empty signing key keeps the descriptor's keys. A signing
    /// key that cannot be read is refused before any fetch. Nothing is
    /// written. An https address is fetched with the system's TLS where this
    /// system has it, and read as http where it does not. The fetch stops at
    /// registry_check_connect_ms while the connection opens, at
    /// registry_check_idle_ms while a byte is waited for, and at
    /// registry_check_descriptor_ms or registry_check_catalogue_ms for the
    /// whole fetch, so one check cannot hold the next past those limits.
    /// The result is read with registry_check.
    ///
    /// @param url the address of the descriptor
    /// @param signing_key a public key, `ed25519:` and its base64; empty keeps the descriptor's keys
    /// @return the request id
    uint64_t check_registry_url(std::string url, std::string signing_key = {});

    /// Checks a registry from the bytes of a descriptor file, on the worker.
    ///
    /// The same checks as check_registry_url. The result's url is empty,
    /// because a file has no address. Nothing is written.
    ///
    /// @param bytes the descriptor's bytes
    /// @return the request id
    uint64_t check_registry_file(std::vector<uint8_t> bytes);

    /// Returns a finished check, or nothing while it is still running.
    ///
    /// Answers check_registry_url, check_registry_file and check_mirror.
    /// An unknown request is nothing as well.
    ///
    /// @param request the id a check call returned
    /// @return the outcome, or nothing when that check has not finished
    [[nodiscard]] std::optional<RegistryCheck> registry_check(uint64_t request) const;

    /// Adds a registry a check has accepted.
    ///
    /// The rules are applied again, the registry is stored with the service
    /// clock's date, and the checked catalogue is written into the cache.
    /// A check that failed, or a request already used, is refused. Nothing
    /// is written while Registries.yaml could not be read.
    ///
    /// @param request the id check_registry_url or check_registry_file returned
    /// @param[out] why why it was refused; may be null
    /// @return none when the registry was added
    [[nodiscard]] data::registry::Refusal add_checked_registry(uint64_t request, std::string* why);

    /// Removes an added registry and deletes only that registry's cache folder.
    ///
    /// A registry that ships with the game is refused: it can be turned off,
    /// not removed, and its cache is left in place. Nothing is written while
    /// Registries.yaml could not be read.
    ///
    /// @param id the registry's id
    /// @param[out] why why it was refused; may be null
    /// @return none when it was removed
    [[nodiscard]] data::registry::Refusal remove_registry(std::string_view id, std::string* why);

    /// Turns a registry on or off and stores that choice.
    ///
    /// A registry that ships with the game goes in or out of the disabled
    /// list. An added registry's own flag is set. Turning one on queues a
    /// refresh of that registry. Nothing is written while Registries.yaml
    /// could not be read.
    ///
    /// @param id the registry's id
    /// @param on true to turn it on, false to turn it off
    /// @param[out] why why it was refused; may be null
    /// @return none when the flag was set
    [[nodiscard]] data::registry::Refusal
    set_registry_enabled(std::string_view id, bool on, std::string* why);

    /// Checks a mirror for one registry, on the worker.
    ///
    /// The mirror is refused for a registry that ships with the game. Otherwise
    /// its catalogue and signature are fetched and checked with the keys
    /// already trusted for that registry, and with that registry's current
    /// sequence. The result is read with registry_check: `url` and `address`
    /// are the mirror's, and `descriptor` is the registry's own.
    ///
    /// @param id the registry's id
    /// @param url the mirror's address
    /// @return the request id
    uint64_t check_mirror(std::string_view id, std::string url);

    /// Adds a mirror a check has accepted.
    ///
    /// The rules are applied again. A mirror is never added to a registry
    /// that ships with the game, and never without its catalogue having
    /// verified. Nothing is written while Registries.yaml could not be read.
    ///
    /// @param request the id check_mirror returned
    /// @param[out] why why it was refused; may be null
    /// @return none when the mirror was added
    [[nodiscard]] data::registry::Refusal add_checked_mirror(uint64_t request, std::string* why);

  private:

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace oa::app::content
