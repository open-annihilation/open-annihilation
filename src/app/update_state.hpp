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
};

} // namespace oa::app
