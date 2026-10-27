// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The package download. One file at a time, on a worker of its own, written
// in steps and resumed after a cut, a match or the player's pause. The main
// thread starts it and reads a snapshot. Where a download stands, why it
// stopped and how fast it moves are named for every build; the worker is
// compiled only where the game may replace itself.
#pragma once

#include <array>
#include <cstdint>
#include <filesystem>
#include <string>

#ifndef OA_SELF_UPDATE
#error "OA_SELF_UPDATE (0 or 1) says whether the game may replace itself"
#endif

namespace oa::app::update {

/// Where a download stands. The main thread reads this from a snapshot.
enum class DownloadState : uint8_t {
    waiting,          ///< nothing is being fetched
    downloading,      ///< a mirror is being read
    paused_by_player, ///< the player paused it; it carries on when they resume
    paused_for_match, ///< a match is running, so the fetch has stopped
    verifying,        ///< the file is being checked against the feed
    ready,            ///< the size and the SHA-256 match the feed
    failed,           ///< the download failed
};

/// Why a download stopped, as the player is told.
enum class DownloadFailure : uint8_t {
    none,       ///< it did not stop
    connect,    ///< no mirror could be reached or kept answering
    wrong_file, ///< the file was not the one the signed feed names; it was deleted
    storage,    ///< the file could not be written or kept
    unusable,   ///< the feed's entry for this build cannot be downloaded
    cancelled,  ///< it was cancelled before it finished
};

/// How fast a download moves, over its last few seconds.
///
/// Each sample is a time in milliseconds and the bytes received by then. The
/// rate is the bytes gained over the samples kept, which span at most
/// rate_window_ms. Fewer bytes than the last sample starts the meter again.
class RateMeter {
  public:

    /// The longest span of samples the rate is taken over, in milliseconds.
    static constexpr uint64_t rate_window_ms = 8000;
    /// The shortest span a rate is given for, in milliseconds.
    static constexpr uint64_t rate_least_ms = 500;

    /// Adds one sample.
    ///
    /// @param at_ms a steady clock, in milliseconds
    /// @param bytes the bytes received by then
    void sample(uint64_t at_ms, uint64_t bytes) noexcept;

    /// Forgets every sample.
    void reset() noexcept;

    /// The rate over the samples kept.
    ///
    /// @return bytes a second; 0 until the samples span rate_least_ms
    [[nodiscard]] uint64_t bytes_per_second() const noexcept;

  private:

    /// One sample: when, and how many bytes had arrived.
    struct Sample {
        uint64_t at_ms{}; ///< the steady clock, in milliseconds
        uint64_t bytes{}; ///< bytes received by then
    };

    static constexpr std::size_t most_samples = 32; ///< samples kept at once
    std::array<Sample, most_samples> samples_{};    ///< oldest first
    std::size_t count_{};                           ///< samples held
};

} // namespace oa::app::update

#if OA_SELF_UPDATE

#include "oa/base/threads.hpp"
#include "oa/data/engine_feed/feed.hpp"

#include <atomic>

namespace oa::app::update {

/// Bytes written and flushed together. A resume keeps a whole number of these.
inline constexpr uint32_t download_step_bytes = 1u << 20;

/// The suffix on a package file that is still being written.
inline constexpr std::string_view part_suffix = ".part";

/// Why a finished file was refused: its size or its SHA-256 was not the feed's.
inline constexpr std::string_view wrong_file_failure =
    "the download was not the file the feed promised";

/// One published point in a download.
struct DownloadSnapshot {
    DownloadState state{DownloadState::waiting}; ///< where the download stands
    uint64_t received{};                         ///< bytes committed to the part
    uint64_t total{};                            ///< bytes the feed promised
    std::string failure;                         ///< why a failed download failed
    DownloadFailure failure_kind{DownloadFailure::none}; ///< why it failed, as the player is told
    uint64_t bytes_per_second{}; ///< how fast it moved over its last few seconds; 0 when unknown
    std::filesystem::path path;  ///< the part, or the file once it is ready
    uint64_t generation{};       ///< moves on for each published snapshot
};

/// Fetches one package on a worker thread and keeps the latest snapshot.
///
/// `start` does nothing while a download is already running. A match, or the
/// player's pause, stops the fetch at the next step, and a download does not
/// start while either holds it. The part is renamed only after its size and
/// SHA-256 match. A package already downloaded and checked is checked again
/// and used, without a fetch.
class PackageDownload {
  public:

    /// Makes a download that has not started.
    PackageDownload();

    /// Cancels a running download and joins the worker.
    ~PackageDownload();

    PackageDownload(const PackageDownload&) = delete;
    PackageDownload& operator=(const PackageDownload&) = delete;

    /// Starts one download, or does nothing while one is running.
    ///
    /// `folder` is `<data>/updates`. The file is written under
    /// `<folder>/<release>/`. An older numbered folder there is removed.
    /// `installer` selects the package's installer; otherwise the zip is
    /// fetched. A file name that is not a plain name is refused.
    ///
    /// @param release the release the package belongs to
    /// @param package the package for this build
    /// @param installer true to fetch the installer instead of the zip
    /// @param folder `<data>/updates`
    void start(
        const oa::data::engine_feed::Release& release,
        const oa::data::engine_feed::Package& package,
        bool installer,
        std::filesystem::path folder
    );

    /// Tells the worker whether a match is running.
    ///
    /// A fetch does not start while this is set. One that has started stops
    /// once the current step is on disk.
    ///
    /// @param running true while a match is on screen
    void set_match_running(bool running) noexcept;

    /// Asks a running download to stop. The bytes already stored stay, so a
    /// later start carries on from them.
    void cancel() noexcept;

    /// Pauses the download for the player.
    ///
    /// The fetch stops once the current step is on disk, and the snapshot
    /// says paused at once. The bytes stay. A download started while paused
    /// waits.
    void pause() noexcept;

    /// Carries on after the player's pause. A match still holds it.
    void resume() noexcept;

    /// Tells whether the player has paused the download.
    ///
    /// @return true between pause and resume
    [[nodiscard]] bool paused() const noexcept;

    /// Stops the download and deletes what it stored.
    ///
    /// The part and a finished file of the last download are removed, and the
    /// snapshot goes back to waiting: nothing half-downloaded is kept. A
    /// running download removes them itself once it has stopped.
    void discard() noexcept;

    /// Copies the latest snapshot.
    ///
    /// @return the snapshot
    [[nodiscard]] DownloadSnapshot snapshot() const;

    /// Forces the answer `tls_available` would give, for a test.
    ///
    /// An https mirror is not contacted when this is false. The game leaves
    /// it unset, which asks the system.
    ///
    /// @param available true when https mirrors may be fetched
    void set_tls_available(bool available) noexcept;

  private:

    /// One download the worker has been given.
    struct Job {
        oa::data::engine_feed::Release release;  ///< the release
        oa::data::engine_feed::Package package;  ///< the package
        oa::data::engine_feed::PackageFile file; ///< the zip, or the installer
        std::filesystem::path folder;            ///< `<data>/updates`
        std::filesystem::path directory;         ///< `<folder>/<release>`
        std::filesystem::path part;              ///< the file while it is written
        std::filesystem::path final_path;        ///< the file once it has been checked
    };

    /// The worker's entry.
    ///
    /// @param argument the download
    static void thread_main(void* argument);

    /// Fetches until the download stops.
    void run();

    /// Fetches one job and publishes its result.
    ///
    /// @param job the download
    void run_job(const Job& job);

    /// Publishes a snapshot. `finished` clears the busy flag.
    ///
    /// @param next the snapshot, without a generation
    /// @param finished true when the download has ended
    void publish(DownloadSnapshot next, bool finished);

    /// Waits while a match or the player holds the download, publishing which
    /// one does. Returns false when the download should stop.
    ///
    /// @param received bytes already committed, for the paused snapshot
    /// @param total bytes the feed promised
    /// @param part the part
    /// @return false when the download was cancelled or the worker is stopping
    [[nodiscard]] bool
    wait_while_held(uint64_t received, uint64_t total, const std::filesystem::path& part);

    /// The state a held or moving download shows: the player's pause first, then a match.
    ///
    /// @return paused_by_player, paused_for_match or downloading
    [[nodiscard]] DownloadState moving_state() const noexcept;

    /// Publishes the bytes one step committed, with the rate. Called by the
    /// part's writer on the worker.
    ///
    /// @param context the download
    /// @param done bytes committed to the part
    static void note_step(void* context, uint64_t done);

    /// Removes the files of the download that stopped, when a discard asked.
    ///
    /// @param part the part
    /// @param final_path the finished file
    /// @return true when a discard asked and the files were removed
    bool discard_files(const std::filesystem::path& part, const std::filesystem::path& final_path);

    /// True when https mirrors may be contacted.
    ///
    /// @return the test's answer, or the system's when the test set none
    [[nodiscard]] bool tls_on() const noexcept;

    mutable base::threads::Mutex mutex_;    ///< guards the snapshot and the job
    base::threads::ConditionVariable wake_; ///< wakes the worker
    base::threads::Thread thread_{};        ///< the worker; empty until the first download
    bool thread_started_{false};            ///< a worker has been started
    bool stop_{false};                      ///< the destructor has asked the worker to leave
    bool busy_{false};                      ///< a download has been published and not finished
    bool has_job_{false};                   ///< `job_` is waiting for the worker
    Job job_{};                             ///< the waiting download
    std::atomic<bool> cancel_{false};       ///< set when the running download should stop
    std::atomic<bool> match_{false};        ///< set while a match is running
    std::atomic<bool> held_{false};         ///< set while the player has paused it
    std::atomic<bool> discard_{false};      ///< set when the stopped download's files go
    uint64_t step_total_{};                 ///< the running file's size, for note_step
    std::filesystem::path step_part_;       ///< the running part, for note_step
    RateMeter rate_{};                      ///< the worker's rate; touched only by the worker
    std::filesystem::path last_part_;       ///< the last download's part
    std::filesystem::path last_final_;      ///< the last download's finished file
    std::atomic<int> tls_mode_{-1};         ///< -1 asks the system; 0 skips https; 1 allows it
    DownloadSnapshot snapshot_{};           ///< the latest snapshot
};

} // namespace oa::app::update

#endif
