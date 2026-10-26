// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The background check of one run (Runtime::UpdateState): the worker, the
// snapshot the main thread last read, and whether this run has already
// looked. runtime_update.cpp starts it and reads it.
#pragma once

#include "oa/app/runtime.hpp"
#include "oa/app/update/checker.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#if defined(OA_SELF_UPDATE) && OA_SELF_UPDATE
#include "oa/app/update/download.hpp"
#include "oa/app/update/route.hpp"
#endif

namespace oa::app {

struct Runtime::UpdateState {
    /// The worker. Empty until a check has somewhere to keep the feed.
    std::unique_ptr<update::Checker> checker;
    /// The snapshot last copied for the main thread.
    update::CheckSnapshot held{};
    /// The generation already copied into `held`.
    uint64_t seen_generation = 0;
    /// An automatic check has been started this run.
    bool ran = false;
    /// Settled main-menu frames in a row.
    uint32_t menu_frames = 0;
    /// The release number of the build the running check asked about.
    int64_t release = 0;
    /// A missing data folder has already been logged.
    bool logged_folder = false;
    /// The release the question was queued for this run. Empty when it has
    /// not been asked, or a match closed it before an answer, so the next
    /// settled menu asks again. Later, Skip and Update leave it set. A
    /// higher release still asks.
    std::optional<int64_t> offered_release;
    /// The page that question opens. Empty when the feed named none. Set
    /// with `offered_release`, so a later check cannot change the page.
    std::optional<std::string> offered_page;
    /// The Updated notice has been queued this run.
    bool updated_notice_shown = false;
#if defined(OA_SELF_UPDATE) && OA_SELF_UPDATE
    /// The package download. Empty until one is started.
    std::unique_ptr<update::PackageDownload> download;
    /// The release a download was started for. 0 until then.
    int64_t download_release = 0;
    /// That release's version, as the feed shows it.
    std::string download_version;
    /// The package id the download is fetching.
    std::string download_package;
    /// True when the file is the package's installer.
    bool download_installer = false;
    /// A download of `download_release` has been started, including one that failed.
    bool download_attempted = false;
    /// The route the download is using. Meaningful when `route_known`.
    update::Where route{};
    /// `route` has been filled for the offer now held.
    bool route_known = false;
    /// A missing route has already been logged.
    bool logged_route = false;
    /// The version that is running, for the plan's kept-version line.
    std::string from_version;
    /// The ready prompt was queued for this release. A match clears it.
    std::optional<int64_t> ready_prompt_release;
    /// Starts the helper. Null records the start instead of making it.
    bool (*start_helper)(
        void* context,
        const std::filesystem::path& helper,
        const std::vector<std::string>& arguments,
        std::string& why
    ){};
    /// What `start_helper` is called with. The state itself, for a recorded start.
    void* helper_context{};
    /// The helper start has been chosen for this run.
    bool helper_chosen = false;
    /// The helper a hand-off asked to start, and the arguments it was given.
    std::string helper;
    std::vector<std::string> helper_arguments;
    /// A hand-off asked the hooks to start the helper.
    bool handed = false;
#endif
};

} // namespace oa::app
