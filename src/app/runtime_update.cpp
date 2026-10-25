// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One check for a new release. The main menu starts it when it is due, a
// match stops it, and a result that reached the server is remembered.
#include "oa/app/runtime.hpp"

#include "oa/app/game_directory.hpp"
#include "update_state.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/frontend_dialogs.hpp"

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace update = oa::app::update;

/// Frames in a row the main menu shows before an automatic check.
constexpr uint32_t kUpdateMenuFrames = 2;

/// Seconds since 1970.
int64_t wall_seconds() {
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(now).count();
}

/// The feed this run checks. A named feed is a test feed.
update::Source source_for(const Options& options) {
    update::Source source;
    if (!options.update_feed.has_value())
        return source;
    source.feed_url = *options.update_feed;
    source.test = true;
    source.keys.reserve(options.update_keys.size());
    for (const std::string& key : options.update_keys) {
        const std::size_t split = key.find('=');
        if (split == std::string::npos || split == 0)
            continue;
        update::FeedKey parsed;
        parsed.id = key.substr(0, split);
        parsed.text = key.substr(split + 1);
        source.keys.push_back(std::move(parsed));
    }
    return source;
}

/// This program's build. The base path is cached for the process.
update::ThisBuild program_build() {
    // SDL caches this string for the process. The caller does not free it.
    const char* const base = SDL_GetBasePath();
    if (base == nullptr)
        return update::this_build({});
    return update::this_build(path_from_utf8(base));
}

/// `<data>/updates`, or nothing when the data folder cannot be named.
std::optional<fs::path> updates_folder(const Options& options, std::string& failure) {
    try {
        const fs::path data =
            options.data_dir ? *options.data_dir : oa::platform::preferences::data_directory();
        return data / "updates";
    } catch (const std::exception& thrown) {
        failure = thrown.what();
        return std::nullopt;
    }
}

/// One line for a finished check. Checking and idle say nothing.
void log_result(int64_t release, const update::CheckSnapshot& latest) {
    if (latest.state == update::CheckState::up_to_date) {
        std::cerr << "open-annihilation: update check: up to date (release " << release << ")\n";
        return;
    }
    if (latest.state == update::CheckState::offered && latest.offer.release != nullptr) {
        std::cerr << "open-annihilation: update check: offered "
                  << latest.offer.release->version_text << " (release "
                  << latest.offer.release->release << ")\n";
        return;
    }
    if (latest.state == update::CheckState::failed) {
        std::cerr << "open-annihilation: update check: failed: " << latest.failure << '\n';
        return;
    }
    if (latest.state == update::CheckState::cancelled)
        std::cerr << "open-annihilation: update check: cancelled\n";
}

} // namespace

void Runtime::destroy_update_state(UpdateState* state) noexcept {
    delete state;
}

void Runtime::cancel_update_check() noexcept {
    if (update_ != nullptr && update_->checker != nullptr)
        update_->checker->cancel();
}

const update::CheckSnapshot& Runtime::update_snapshot() const {
    if (update_ == nullptr) {
        static const update::CheckSnapshot empty;
        return empty;
    }
    return update_->held;
}

void Runtime::start_update_check(bool asked) {
    if (match_ != nullptr || screen_ == Screen::match)
        return;
    const auto found = preference_values_.find(std::string(oa::ui::first_run::preference_key));
    const bool setup_done = found != preference_values_.end() &&
                            oa::ui::engine_settings::language_notice_trim(found->second) ==
                                oa::ui::first_run::preference_done &&
                            !first_run_blocks_language_notice();
    if (!setup_done)
        return;
    // An unattended or headless run stays quiet unless this run named a feed.
    if (!options_.update_feed.has_value() && (options_.unattended || options_.headless_check))
        return;
    if (update_ == nullptr)
        update_.reset(new UpdateState());
    const update::CheckMode mode = update::read_check_mode(preference_values_);
    if (mode == update::CheckMode::never)
        return;
    if (!asked && mode != update::CheckMode::automatically)
        return;
    if (!asked && update_->ran)
        return;
    if (!asked &&
        !update::check_due(mode, update::read_checked(preference_values_), wall_seconds()))
        return;
    if (update_->checker == nullptr) {
        std::string failure;
        const std::optional<fs::path> folder = updates_folder(options_, failure);
        if (!folder) {
            if (!update_->logged_folder) {
                update_->logged_folder = true;
                std::cerr << "open-annihilation: update check: failed: " << failure << '\n';
            }
            return;
        }
        update_->checker = std::make_unique<update::Checker>(*folder);
    }
    const update::ThisBuild built = program_build();
    update_->release = built.release;
    update_->checker->start(source_for(options_), built, asked);
    if (!asked)
        update_->ran = true;
}

void Runtime::tick_update_check() {
    if (update_ == nullptr)
        update_.reset(new UpdateState());
    if (match_ != nullptr || screen_ == Screen::match)
        cancel_update_check();
    if (update_->checker != nullptr) {
        const update::CheckSnapshot latest = update_->checker->snapshot();
        if (latest.generation != update_->seen_generation) {
            update_->seen_generation = latest.generation;
            update_->held = latest;
            log_result(update_->release, latest);
            if (latest.checked_at.has_value() && (latest.state == update::CheckState::up_to_date ||
                                                  latest.state == update::CheckState::offered ||
                                                  latest.state == update::CheckState::failed)) {
                update::write_checked(preference_values_, *latest.checked_at);
                preferences_dirty_ = true;
                try {
                    flush_preferences();
                } catch (const std::exception& failure) {
                    std::cerr << "open-annihilation: update check: failed: " << failure.what()
                              << '\n';
                }
            }
        }
    }
    // Ask while the main menu is up, including while a dialog or Settings
    // keeps the question waiting. A match took the branch above and cancels
    // there; the question is not offered over it.
    offer_update();
    // The menu on its own, with no dialog and no settings window over it.
    const bool settled = screen_ == Screen::main_menu && !frame_owned_by_package() &&
                         state_.state == frontend::state_id::main_menu &&
                         state_.pending_signal != frontend::signal_id::multiplayer &&
                         oa::ui::frontend_dialogs::dialog_count() == 0 &&
                         engine_settings_dialog() == nullptr;
    if (!settled) {
        update_->menu_frames = 0;
        return;
    }
    if (update_->menu_frames < kUpdateMenuFrames)
        ++update_->menu_frames;
    if (update_->menu_frames >= kUpdateMenuFrames)
        start_update_check(false);
}

} // namespace oa::app
