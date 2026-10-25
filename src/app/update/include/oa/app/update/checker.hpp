// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One background check for a new release. The worker fetches the feed and
// its signature, and the main thread reads a snapshot. A check that is
// still running is cancelled, and a cancelled check is not a finished one.
#pragma once

#include "oa/app/update/build.hpp"
#include "oa/app/update/check.hpp"
#include "oa/base/threads.hpp"
#include "oa/data/engine_feed/feed.hpp"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

namespace oa::netgame::http {
class Client;
} // namespace oa::netgame::http

namespace oa::app::update {

/// Where a check stands. The main thread reads this from a snapshot.
enum class CheckState : uint8_t {
    idle,       ///< no check has run
    checking,   ///< a check is in progress
    up_to_date, ///< the feed verified and offers nothing newer
    offered,    ///< the feed verified and offers a newer release
    failed,     ///< the check failed, or the feed was refused
    cancelled,  ///< the check was cancelled before it finished
};

/// One published result. The offer's pointers address `feed`.
struct CheckSnapshot {
    CheckState state{CheckState::idle};                      ///< where the check stands
    std::shared_ptr<const oa::data::engine_feed::Feed> feed; ///< the verified feed; empty otherwise
    oa::data::engine_feed::Offer offer{}; ///< the offer; way is none unless offered
    std::optional<int64_t> checked_at;    ///< when the server answered, if it did
    std::string failure;                  ///< why a failed check failed
    uint64_t generation{};                ///< moves on for each published snapshot
};

/// Fetches one feed on a worker thread and keeps the latest snapshot.
///
/// `start` does nothing while a check is already running. The worker uses
/// the game's own user agent and sends nothing else. The verified bytes are
/// written under the updates folder only after they verify.
class Checker {
  public:

    /// Keeps the folder the verified feed is written into. The folder is
    /// created when a feed is stored.
    ///
    /// @param updates_folder `<data>/updates`
    explicit Checker(std::filesystem::path updates_folder);

    /// Cancels a running check and joins the worker.
    ~Checker();

    Checker(const Checker&) = delete;
    Checker& operator=(const Checker&) = delete;

    /// Starts one check, or does nothing while one is running.
    ///
    /// `asked` does not change the fetch. The caller decides whether a
    /// check is due. An empty key list uses the pinned keys.
    ///
    /// @param source the feed and the keys
    /// @param build the build the offer is chosen for
    /// @param asked true when the player asked for this check
    void start(const Source& source, const ThisBuild& build, bool asked);

    /// Asks a running check to stop. A check that has not finished does not
    /// count as done.
    void cancel() noexcept;

    /// Copies the latest snapshot.
    ///
    /// @return the snapshot
    [[nodiscard]] CheckSnapshot snapshot() const;

  private:

    /// One check the worker has been given.
    struct Job {
        Source source;   ///< the feed and the keys
        ThisBuild build; ///< the build the offer is chosen for
    };

    /// The worker's entry.
    ///
    /// @param argument the checker
    static void thread_main(void* argument);

    /// Fetches until the checker stops.
    void run();

    /// Fetches one job and publishes its result.
    ///
    /// @param client the worker's client
    /// @param job the check
    void run_job(oa::netgame::http::Client& client, const Job& job);

    /// Publishes a failed check. A cancel that won the race publishes cancelled.
    ///
    /// @param failure the line
    /// @param checked when the server answered, if it did
    void publish_failed(std::string failure, std::optional<int64_t> checked);

    /// Publishes a cancelled check, with no time and no offer.
    void publish_cancelled();

    /// Publishes a verified result, or cancelled when a cancel won the race.
    ///
    /// @param next the result, without a generation
    void publish_done(CheckSnapshot next);

    std::filesystem::path folder_;          ///< `<data>/updates`
    mutable base::threads::Mutex mutex_;    ///< guards the snapshot and the job
    base::threads::ConditionVariable wake_; ///< wakes the worker
    base::threads::Thread thread_{};        ///< the worker; empty until the first check
    bool thread_started_{false};            ///< a worker has been started
    bool stop_{false};                      ///< the destructor has asked the worker to leave
    bool busy_{false};                      ///< a check has been published and not finished
    bool has_job_{false};                   ///< `job_` is waiting for the worker
    Job job_{};                             ///< the waiting check
    std::atomic<bool> cancel_{false};       ///< set when the running check should stop
    CheckSnapshot snapshot_{};              ///< the latest snapshot
};

} // namespace oa::app::update
