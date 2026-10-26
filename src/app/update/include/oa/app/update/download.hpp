// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The package download. One file at a time, on a worker of its own, written
// in steps and resumed after a cut or a match. The main thread starts it
// and reads a snapshot. It does not fetch, write or hash.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

#ifndef OA_SELF_UPDATE
#error "OA_SELF_UPDATE (0 or 1) says whether the game may replace itself"
#endif

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

/// Where a download stands. The main thread reads this from a snapshot.
enum class DownloadState : uint8_t {
    waiting,          ///< nothing is being fetched
    downloading,      ///< a mirror is being read
    paused_for_match, ///< a match is running, so the fetch has stopped
    verifying,        ///< the file is being checked against the feed
    ready,            ///< the size and the SHA-256 match the feed
    failed,           ///< the download failed
};

/// One published point in a download.
struct DownloadSnapshot {
    DownloadState state{DownloadState::waiting}; ///< where the download stands
    uint64_t received{};                         ///< bytes committed to the part
    uint64_t total{};                            ///< bytes the feed promised
    std::string failure;                         ///< why a failed download failed
    std::filesystem::path path;                  ///< the part, or the file once it is ready
    uint64_t generation{};                       ///< moves on for each published snapshot
};

/// Fetches one package on a worker thread and keeps the latest snapshot.
///
/// `start` does nothing while a download is already running. A match stops
/// the fetch at the next step, and a download does not start while a match
/// is running. The part is renamed only after its size and SHA-256 match.
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

    /// Asks a running download to stop.
    void cancel() noexcept;

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

    /// Waits while a match is running. Returns false when the download should stop.
    ///
    /// @return false when the download was cancelled or the worker is stopping
    [[nodiscard]] bool wait_out_of_match();

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
    std::atomic<int> tls_mode_{-1};         ///< -1 asks the system; 0 skips https; 1 allows it
    DownloadSnapshot snapshot_{};           ///< the latest snapshot
};

} // namespace oa::app::update

#endif
