// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The download queue. One package at a time, on a worker of its own, written
// in steps and resumed after a cut, a quit or a match. The main thread queues
// and reads; it does not fetch, write or hash.
#pragma once

#include "oa/app/content/service.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/catalogue/catalogue.hpp"
#include "oa/data/registry/descriptor.hpp"
#include "oa/formats/url.hpp"
#include "oa/netgame/http/client.hpp"

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::content {

/// Bytes written and flushed together. A resume keeps a whole number of these.
inline constexpr uint32_t download_step_bytes = 1u << 20;

/// The suffix on a package file that is still being written.
inline constexpr std::string_view part_suffix = ".part";

/// The name of the saved queue, in the downloads folder.
inline constexpr std::string_view queue_file_name = "queue.yaml";

/// How often a challenge is polled when the registry does not say, in milliseconds.
inline constexpr uint32_t challenge_poll_ms = 3000;

/// Why the player asked for a package.
enum class DownloadReason : uint8_t {
    install,          ///< the player is getting it for the first time
    update,           ///< a newer release than the one installed
    repair,           ///< the installed copy is being replaced
    offered_in_lobby, ///< offered while setting up a shared game
    mirror,           ///< a registry is being copied
};

/// Where one queued package stands.
enum class DownloadState : uint8_t {
    waiting,          ///< queued, not started
    asking,           ///< asking the registry for a key
    challenge,        ///< the registry asked the player to confirm they are a person
    downloading,      ///< the package file is being fetched
    paused_for_match, ///< held while a match runs; the part is kept
    verifying,        ///< the file is being checked against the catalogue
    handing_over,     ///< the file is being given to the installer
    installing,       ///< the installer has the file
    done,             ///< installed
    failed,           ///< stopped, and will not resume until the player retries
    cancelled,        ///< the player cancelled, or set the install question aside
};

/// Why a download failed. `none` is not a failure.
enum class DownloadFailure : uint8_t {
    none,                ///< no failure
    unreachable,         ///< the registry and its mirrors could not be reached
    refused,             ///< the registry refused the request
    not_offered,         ///< the registry no longer offers this release
    install_id_off,      ///< downloads are off for this registry
    no_space,            ///< the disk has no room for the rest of the file
    hash_mismatch,       ///< the file was not the one the catalogue named, twice
    server_file_differs, ///< the server's file is not the size the catalogue names
    challenge_expired,   ///< the check expired before it was passed
    install_refused,     ///< the installer refused the package
    install_failed,      ///< the package could not be installed
    disk_error,          ///< the file could not be written
    server_check,        ///< the server asked for a check meant for a web browser
};

/// The sentence a log uses for a failure.
///
/// @param failure the failure, or none
/// @return a stable English phrase; never null
[[nodiscard]] inline const char* failure_text(DownloadFailure failure) noexcept {
    switch (failure) {
    case DownloadFailure::none:
        return "";
    case DownloadFailure::unreachable:
        return "the server could not be reached";
    case DownloadFailure::refused:
        return "the registry refused the download";
    case DownloadFailure::not_offered:
        return "the registry no longer offers this release";
    case DownloadFailure::install_id_off:
        return "downloads are off for this registry";
    case DownloadFailure::no_space:
        return "there is not enough free space";
    case DownloadFailure::hash_mismatch:
        return "the download was not the file the catalogue promised";
    case DownloadFailure::server_file_differs:
        return "the server's file is not the size the catalogue promised";
    case DownloadFailure::challenge_expired:
        return "the check expired";
    case DownloadFailure::install_refused:
        return "the package was refused";
    case DownloadFailure::install_failed:
        return "the package could not be installed";
    case DownloadFailure::disk_error:
        return "the download could not be written";
    case DownloadFailure::server_check:
        return "the server asked for a check the game cannot do";
    }
    return "";
}

/// What the player is told when a registry's server asked for a check meant
/// for a web browser, which the game never answers. {registry} is the
/// registry's name.
inline constexpr std::string_view server_check_words =
    "{registry} asked for a check the game cannot do. Try again later.";

/// The word the download API uses for a reason.
///
/// @param reason why the player asked
/// @return `install`, `update`, `repair`, `offered-in-lobby` or `mirror`; empty when unknown
[[nodiscard]] std::string_view reason_text(DownloadReason reason) noexcept;

/// One package to download, fixed when it is queued.
struct DownloadTarget {
    std::string registry;      ///< the registry id
    std::string registry_name; ///< the descriptor's name, for the texts that name the registry
    data::catalogue::Kind kind = data::catalogue::Kind::oamod;
    std::string key;                      ///< the package key in that registry
    std::string name;                     ///< the name a player sees
    int64_t release = 0;                  ///< the catalogue release
    uint64_t size = 0;                    ///< the catalogue's size, in bytes
    base::sha256::Digest sha256{};        ///< the catalogue's SHA-256
    std::vector<formats::url::Url> files; ///< the package file, one address per source
    data::registry::DownloadMode mode = data::registry::DownloadMode::direct;
    std::optional<formats::url::Url> api; ///< the download API, when the registry uses keys
    data::registry::InstallIdUse install_id = data::registry::InstallIdUse::none;
};

/// Resolves a catalogue entry into a download.
///
/// The file addresses are reference_urls of the entry's file. The registry's
/// name, download mode, API and install-id use come from its descriptor.
/// Nothing is fetched.
///
/// @param snapshot the catalogues in effect
/// @param registry the registry id
/// @param key the package key
/// @param[out] why why there is no target; may be null
/// @return the target, or nothing when the snapshot has no such package
[[nodiscard]] std::optional<DownloadTarget> download_target(
    const Snapshot& snapshot, std::string_view registry, std::string_view key, std::string* why
);

/// Why the player asked, carried to the installer with the file.
struct InstallOrigin {
    std::string registry; ///< the registry id
    data::catalogue::Kind kind = data::catalogue::Kind::oamod;
    std::string key;                                 ///< the package key
    int64_t release = 0;                             ///< the catalogue release
    base::sha256::Digest sha256{};                   ///< the catalogue's SHA-256
    DownloadReason reason = DownloadReason::install; ///< why the player asked
};

/// Gives a finished file to the installer.
///
/// Called on the download worker. It must not call back into the queue.
/// True means the installer accepted the file and will report an outcome.
///
/// @param context the pointer given in the options
/// @param item the queue item
/// @param file the package file, named `<sha256>.<kind>`
/// @param origin where it came from, and why the player asked
/// @return true when the installer took the file
using Handoff = bool (*)(
    void* context, uint64_t item, const std::filesystem::path& file, const InstallOrigin& origin
);

/// What the installer reported for one file.
enum class InstallOutcome : uint8_t {
    installed, ///< the package is in place
    refused,   ///< refused before it was installed
    failed,    ///< the install did not finish
    set_aside, ///< the player set the question aside
};

/// The check a registry asked the player to pass.
struct ChallengeView {
    std::string code;       ///< the code the player enters
    std::string verify_url; ///< the page, opened in a browser
    std::string address;    ///< the page without its scheme and query
    int64_t expires = 0;    ///< when the check expires, seconds since 1970
};

/// One queued package, as the main thread reads it.
struct DownloadView {
    uint64_t item = 0; ///< the id queue returned
    DownloadTarget target;
    DownloadReason reason = DownloadReason::install;
    DownloadState state = DownloadState::waiting;
    uint64_t done = 0; ///< bytes held in the part, or the whole file once it is complete
    DownloadFailure failure = DownloadFailure::none;
    /// What the failure's own text does not say: the registry's message or
    /// the installer's reason. Empty when the failure's text says it all; it
    /// never repeats that text.
    std::string detail;
    std::optional<ChallengeView> challenge; ///< set while a check is waiting
};

/// Looks up one of the interface's own words in the language the game shows.
///
/// @param english the word, in English
/// @return the word to show
using LookUp = std::string_view (*)(std::string_view english);

/// Writes why an item failed, naming its reason once, as a line says it
/// after a colon.
///
/// A check the server asked for names the registry and asks the player to
/// try again later (server_check_words). An installer's refusal or failure
/// with a reason of its own is that reason alone. Any other failure is its
/// text, then the detail in brackets when there is one. The words are left
/// as they are written: a failure's own text starts with a small letter.
///
/// @param item the item
/// @param look_up looks the words up; null keeps them in English
/// @return the reason; empty when the item has neither a failure nor a detail
[[nodiscard]] std::string failure_reason(const DownloadView& item, LookUp look_up);

/// Writes what the player is told of a failed item, naming its reason once:
/// failure_reason, with a capital first letter.
///
/// @param item the item
/// @param look_up looks the words up; null keeps them in English
/// @return the message; empty when the item has neither a failure nor a detail
[[nodiscard]] std::string failure_message(const DownloadView& item, LookUp look_up);

/// The platform this build names in a key request.
///
/// @return `windows`, `macos`, `linux`, `ios` or `android`
[[nodiscard]] std::string_view platform_name() noexcept;

/// The architecture this build names in a key request.
///
/// @return `x64`, `x86`, `arm64` or `arm`
[[nodiscard]] std::string_view arch_name() noexcept;

/// What the queue is given when it is made.
struct DownloadsOptions {
    std::filesystem::path data_folder{};  ///< empty turns downloads off
    std::string language;                 ///< the tag of the language the game shows
    Handoff handoff = nullptr;            ///< null means a finished file cannot be installed
    void* handoff_context = nullptr;      ///< passed back to handoff
    netgame::http::ClientOptions http{};  ///< the worker's client
    LogSink log = nullptr;                ///< null discards lines
    void* log_context = nullptr;          ///< passed back to log
    Clock clock = nullptr;                ///< null uses the system clock; seconds since 1970
    uint32_t retry_base_ms = 2000;        ///< the first wait; the later waits scale from it
    uint32_t poll_ms = challenge_poll_ms; ///< how often a check is polled; tests shorten it
};

/// The queue and the worker that runs it.
///
/// Every method is for the main thread. None of them fetches, writes a part,
/// reads or writes the saved queue, or hashes a file. start hands the snapshot
/// to the worker, which reads the saved queue. The destructor stops the worker
/// within a second and keeps the parts and the saved queue.
///
/// Every failure writes one log line, once, from the worker: the package, the
/// failure's text and its detail, and what the server last sent for the key
/// request and for the package (answered_text: the address without its
/// query, the status, the content type and the length, or the network
/// failure). A check meant for a web browser fails the item at once as
/// server_check; the game never answers it, asks again by itself or tries
/// another address around it.
class Downloads {
  public:

    /// Stores the options and starts the worker. Nothing is fetched yet.
    ///
    /// @param options the folder, the language and the hand-off
    explicit Downloads(DownloadsOptions options);

    /// Stops the worker and keeps every part and the saved queue.
    ~Downloads();

    Downloads(const Downloads&) = delete;
    Downloads& operator=(const Downloads&) = delete;

    /// Hands the snapshot to the worker, which reads the saved queue.
    ///
    /// An item whose catalogue entry still has the saved release and SHA-256
    /// is queued again and resumes its part. The rest are dropped, with a log
    /// line. A second call does nothing.
    ///
    /// @param snapshot the catalogues in effect
    void start(const Snapshot& snapshot);

    /// Queues one package, at the back.
    ///
    /// The target is kept as given. A registry whose install id is required,
    /// and whose id is off, is refused. A package that already has an
    /// unfinished item is refused. Nothing is fetched on this thread.
    ///
    /// @param target the package, fixed for this item
    /// @param reason why the player asked
    /// @param[out] why why it was not queued; may be null
    /// @return the item id, or nothing when it was refused
    [[nodiscard]] std::optional<uint64_t>
    queue(DownloadTarget target, DownloadReason reason, std::string* why);

    /// Cancels one item and keeps its part, so queueing the package again resumes it.
    ///
    /// An item that has already ended is left as it is.
    ///
    /// @param item the id queue returned
    void cancel(uint64_t item);

    /// Cancels every unfinished item of one registry. Other registries are left.
    ///
    /// @param registry the registry id
    void cancel_registry(std::string_view registry);

    /// Queues a failed or cancelled item again, keeping its part.
    ///
    /// An item that is still running, or that finished installed, is left as it is.
    ///
    /// @param item the id queue returned
    void retry(uint64_t item);

    /// Remembers one registry's install id. An empty id means downloads are off.
    ///
    /// An unfinished item of a registry that requires an id fails as
    /// install_id_off before its next key request. Nothing is written.
    ///
    /// @param registry the registry id
    /// @param id the id, or nothing when it is off
    void set_install_id(std::string_view registry, std::optional<std::string> id);

    /// Says whether a match is running.
    ///
    /// While it is, no item starts, and a fetch stops at its next step.
    /// When it is not, a paused item resumes.
    ///
    /// @param running true while a match runs
    void set_match_running(bool running) noexcept;

    /// Remembers the language the game shows. The next key request sends it.
    ///
    /// @param tag the language tag
    void set_language(std::string tag);

    /// Records what the installer did with a file the queue handed over.
    ///
    /// A refusal's or a failure's reason becomes the item's detail and is in
    /// the log line the failure writes.
    ///
    /// @param item the id that was handed over
    /// @param outcome what the installer reported
    /// @param reason why it refused or failed, in the player's words; empty when it gave none
    void report_install(uint64_t item, InstallOutcome outcome, std::string reason = {});

    /// Returns every item, oldest first.
    ///
    /// @return the items, copied
    [[nodiscard]] std::vector<DownloadView> view() const;

    /// Returns a count that moves on whenever view would change.
    ///
    /// @return the count, from 0
    [[nodiscard]] uint64_t generation() const noexcept;

    /// Tells whether any item has not ended.
    ///
    /// @return true while an item is not done, failed or cancelled
    [[nodiscard]] bool busy() const noexcept;

    /// Names every queued or running item's part and final file.
    ///
    /// A failed or cancelled item is not included: emptying the folder may
    /// remove its part. The files need not exist.
    ///
    /// @return the paths
    [[nodiscard]] std::vector<std::filesystem::path> files_in_use() const;

    /// The worker's state. Its definition stays with the worker.
    struct State;

  private:

    std::unique_ptr<State> state_;
};

} // namespace oa::app::content
