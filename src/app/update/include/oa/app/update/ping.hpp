// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Reporting one check for a new release to the address its signed feed
// names. The report is sent on a worker thread and nothing waits for it: a
// report that fails changes nothing and is only logged. Whether a check is
// reported is the caller's choice, at most once a check and once an hour.
#pragma once

#include "oa/app/update/build.hpp"
#include "oa/base/threads.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace oa::app::update {

/// What the report of one check says, and nothing else.
struct PingReport {
    std::string version;    ///< this build's version
    std::string platform;   ///< this build's package id, the one `choose` uses
    std::string channel;    ///< `stable` or `testing`
    std::string install_id; ///< the feed's registry's install ID; empty when off or absent
    bool asked{};           ///< the player asked for the check (`manual`); else `automatic`
};

/// The package ids a report may name: those the releases are built for.
inline constexpr std::array<std::string_view, 9> reported_platforms{
    "linux-arm64",
    "linux-armhf",
    "linux-x86_64",
    "macos-universal",
    "steam-deck",
    "windows-95",
    "windows-arm64",
    "windows-x64",
    "windows-x86",
};

/// Tells whether this build reports its checks.
///
/// A build reports only when its version has no label and its package id
/// is one of `reported_platforms`. A labelled build, such as a development
/// one, and a build no release package is made for, such as iOS, never
/// reports.
///
/// @param build this build
/// @return true when its checks are reported
[[nodiscard]] bool build_reports(const ThisBuild& build) noexcept;

/// The report as the body of its request.
///
/// One JSON object on one line, of `version`, `platform`, `channel`,
/// `install_id` and `trigger` in that order. `trigger` is `manual` for a
/// check the player asked for and `automatic` otherwise.
///
/// @param report the report
/// @return the JSON text
[[nodiscard]] std::string ping_body(const PingReport& report);

/// Tells whether an address names this machine: `localhost` or an IPv4
/// address that starts with 127. An unattended run and a test feed report
/// only there.
///
/// @param url the address
/// @return true for an http or https address on this machine
[[nodiscard]] bool local_address(std::string_view url);

/// Where a report stands. The main thread reads this from a snapshot.
enum class PingState : uint8_t {
    idle,    ///< nothing has been sent
    sending, ///< a report is on its way
    sent,    ///< the server took the report
    failed,  ///< the report did not reach the server, or was refused
};

/// One published result.
struct PingSnapshot {
    PingState state{PingState::idle}; ///< where the report stands
    int status{};                     ///< the server's answer; 0 when none was read
    std::string failure;              ///< why a failed report failed
    uint64_t generation{};            ///< moves on for each published snapshot
};

/// Sends one report at a time on a worker thread, with the game's own user
/// agent and no other header than its type.
class Pinger {
  public:

    Pinger() = default;

    /// Stops a report that is still being sent and joins the worker.
    ~Pinger();

    Pinger(const Pinger&) = delete;
    Pinger& operator=(const Pinger&) = delete;

    /// Hands one report to the worker, or does nothing while one is being sent.
    ///
    /// @param url the address the feed names
    /// @param report what the report says
    /// @return true when the report was handed over
    bool send(std::string url, const PingReport& report);

    /// Copies the latest snapshot.
    ///
    /// @return the snapshot
    [[nodiscard]] PingSnapshot snapshot() const;

  private:

    /// The worker's entry.
    ///
    /// @param argument the pinger
    static void thread_main(void* argument);

    /// Sends reports until the pinger stops.
    void run();

    mutable base::threads::Mutex mutex_;    ///< guards the snapshot and the waiting report
    base::threads::ConditionVariable wake_; ///< wakes the worker
    base::threads::Thread thread_{};        ///< the worker; empty until the first report
    bool thread_started_{false};            ///< a worker has been started
    bool stop_{false};                      ///< the destructor has asked the worker to leave
    bool busy_{false};                      ///< a report has been handed over and not finished
    bool has_job_{false};                   ///< `url_` and `body_` wait for the worker
    std::string url_;                       ///< the waiting report's address
    std::string body_;                      ///< the waiting report's body
    std::atomic<bool> cancel_{false};       ///< set when the report should stop
    PingSnapshot snapshot_{};               ///< the latest snapshot
};

} // namespace oa::app::update
