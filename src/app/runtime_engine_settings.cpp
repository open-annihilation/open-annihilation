// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Open Annihilation settings: read at start, put in effect, saved, and
// the dialog's session, which the OA layer's settings screen shows on the
// main menu and in a match.

#include "engine_settings_state.hpp"
#include "library_host.hpp"
#include "mod_install_state.hpp"
#include "oa_layer.hpp"
#include "oa/app/package_install.hpp"
#include "oa/app/package_install/oamod.hpp"
#include "oa/app/package_install/prompts.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/game_files_hooks.hpp"
#include "oa/app/mod_profile_loader.hpp"
#include "oa/app/mod_summary.hpp"
#include "oa/app/user_folder.hpp"

#include "oa/app/acceleration_status.hpp"
#include "oa/app/content/downloads.hpp"
#include "oa/app/content/service.hpp"
#include "oa/app/content/settings.hpp"
#include "oa/app/content_copy.hpp"
#include "oa/app/setup_before_game.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/update/checker.hpp"
#include "oa/app/update/build.hpp"
#include "oa/ui/engine_settings/file_types.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/app/view_rules.hpp"
#include "oa/data/registry/descriptor.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/mod_profile/overrides.hpp"
#include "render_host.hpp"
#include "render_run.hpp"
#include "screen_size.hpp"
#include "oa/platform/file_types.hpp"
#include "oa/platform/machine.hpp"
#include "oa/platform/system.hpp"
#include "oa/platform/render_probe.hpp"
#include "oa/app/window_icon.hpp"
#include "oa/sim/ground_orders/search_worker.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/frontend/savegame_dialogs.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/touch_hud.hpp"

#include <SDL3/SDL.h>

#include <cstddef>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#ifndef OA_ENGINE_VERSION
#error "OA_ENGINE_VERSION names the engine's version for the dialog's header"
#endif

namespace oa::app {

namespace settings = oa::ui::engine_settings;

static_assert(
    settings::default_skirmish_players == init::default_skirmish_player_count,
    "the slider's default and a new install's skirmish rows are the same count"
);
namespace kit = oa::ui::kit;
namespace render_probe = oa::platform::render_probe;

static_assert(settings::base_path_search_nodes == oa::sim::ground_orders::search_tick_credit);
static_assert(settings::lowest_frame_rate == kLowestMaxFramesPerSecond);
static_assert(settings::highest_frame_rate == kDefaultMaxFramesPerSecond);
static_assert(settings::default_unit_limit == kSkirmishUnitsPerPlayer);
static_assert(
    static_cast<uint32_t>(settings::AntiAliasing::x16) ==
    oa::present::model::supersampling_factor(oa::present::model::UnitSupersampling::x16)
);

// The settings dialog keeps the mod folder the game starts with.
static_assert(mod_directory_preference == settings::key::mod_directory);

namespace {

namespace content = oa::app::content;

/// What Downloads shows, from the registries in effect and the stored IDs.
/// No ID is made here.
///
/// @param values the preference values
/// @param snapshot the registries in effect; null lists no install ID
/// @param folder the downloads folder
/// @param busy a download is queued, running or paused
/// @return the offer
[[nodiscard]] settings::DownloadsOffer downloads_offer_of(
    const std::map<std::string, std::string>& values,
    const std::shared_ptr<const content::Snapshot>& snapshot,
    const std::filesystem::path& folder,
    bool busy
) {
    settings::DownloadsOffer offer;
    if (snapshot) {
        for (const content::RegistryView& listed : snapshot->registries) {
            if (listed.registry.descriptor.install_id != oa::data::registry::InstallIdUse::required)
                continue;
            const content::InstallId stored =
                content::read_install_id(values, listed.registry.descriptor.id);
            settings::InstallIdRow row;
            row.registry_id = listed.registry.descriptor.id;
            row.registry_name = listed.registry.descriptor.name;
            row.on = stored.state != content::InstallIdState::off;
            row.made = stored.state == content::InstallIdState::on;
            row.id_text = content::install_id_shown(stored.text);
            offer.install_ids.push_back(std::move(row));
        }
    }
    offer.kept_bytes = content::downloaded_bytes(folder);
    offer.downloads_busy = busy;
    offer.https_over_http = content::https_read_over_http();
    return offer;
}

/// Gives the dialog the offer Downloads shows now.
///
/// @param[in,out] dialog the dialog
/// @param values the preference values
/// @param service the content service, for its registries
/// @param downloads the queue, for whether it is busy
/// @param folder the downloads folder
void give_downloads(
    settings::Dialog& dialog,
    const std::map<std::string, std::string>& values,
    content::Service& service,
    content::Downloads& downloads,
    const std::filesystem::path& folder
) {
    settings::set_downloads_offer(
        dialog, downloads_offer_of(values, service.snapshot(), folder, downloads.busy())
    );
}

/// Stores each install-ID switch the player moved. A switch turned on makes
/// no ID; the next download asks for one.
///
/// @param[in,out] values the preference values
/// @param opened the switches as the dialog opened
/// @param chosen the switches the player leaves
void store_install_switches(
    std::map<std::string, std::string>& values,
    const std::vector<settings::InstallIdChoice>& opened,
    const std::vector<settings::InstallIdChoice>& chosen
) {
    const std::size_t count = std::min(opened.size(), chosen.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (opened[index].on == chosen[index].on)
            continue;
        if (chosen[index].on)
            content::turn_install_id_on(values, chosen[index].registry_id);
        else
            content::turn_install_id_off(values, chosen[index].registry_id);
    }
}

/// Tells the queue each install-ID switch the player moved: nothing, so a
/// switch turned off stops that registry's downloads, and one turned on
/// waits until the next download asks.
///
/// @param downloads the queue
/// @param opened the switches as the dialog opened
/// @param chosen the switches the player leaves
void tell_install_switches(
    content::Downloads& downloads,
    const std::vector<settings::InstallIdChoice>& opened,
    const std::vector<settings::InstallIdChoice>& chosen
) {
    const std::size_t count = std::min(opened.size(), chosen.size());
    for (std::size_t index = 0; index < count; ++index) {
        if (opened[index].on == chosen[index].on)
            continue;
        downloads.set_install_id(chosen[index].registry_id, std::nullopt);
    }
}

/// The content service's check that matches the dialog's.
///
/// @param check the dialog's choice
/// @return the service's
[[nodiscard]] content::CheckForUpdates check_mode(settings::ContentCheck check) noexcept {
    switch (check) {
    case settings::ContentCheck::automatically:
        return content::CheckForUpdates::automatically;
    case settings::ContentCheck::when_library_opens:
        return content::CheckForUpdates::library_only;
    case settings::ContentCheck::never:
        return content::CheckForUpdates::never;
    }
    return content::CheckForUpdates::automatically;
}

/// Replaces `{token}` once.
void replace_token(std::string& text, std::string_view token, std::string_view value) {
    const auto at = text.find(token);
    if (at != std::string::npos)
        text.replace(at, token.size(), value);
}

/// "Checking… N%". The check's thread only counts files; this writes the line.
std::string checking_line(uint32_t done, uint32_t total) {
    std::string text{oa::data::languages::interface_text("Checking… {percent}%")};
    const uint32_t percent =
        total == 0 ? 0 : static_cast<uint32_t>((static_cast<uint64_t>(done) * 100ULL) / total);
    replace_token(text, "{percent}", std::to_string(percent));
    return text;
}

/// "{done} of {total} · N%", the copy's file line.
std::string files_progress_line(uint32_t done, uint32_t total) {
    std::string text{oa::data::languages::interface_text("{done} of {total} · {percent}%")};
    const uint32_t percent =
        total == 0 ? 0 : static_cast<uint32_t>((static_cast<uint64_t>(done) * 100ULL) / total);
    replace_token(text, "{done}", std::to_string(done));
    replace_token(text, "{total}", std::to_string(total));
    replace_token(text, "{percent}", std::to_string(percent));
    return text;
}

/// The check Game Content starts. It hashes off the UI thread.
struct ContentCheckJob {
    ~ContentCheckJob() {
        stop.store(true);
        if (worker.joinable())
            worker.join();
    }

    std::mutex mutex{};
    std::atomic<bool> stop{false};
    std::atomic<bool> exited{true};
    std::thread worker{};
    uint64_t generation{0};        ///< bumped to discard a check that was stopped
    uint64_t worker_generation{0}; ///< the generation the running thread serves
    bool running{false};
    bool restart{false}; ///< start again once the thread has left
    bool changed{false};
    bool result_ready{false};
    uint32_t done{0};
    uint32_t total{0};
    ContentCheck result{};
    fs::path folder{};
};

ContentCheckJob& content_check_job() {
    static ContentCheckJob job;
    return job;
}

/// A copy Game Content started. The copying step's worker does the bytes.
struct ContentCopyWatch {
    bool active{false}; ///< a copy this dialog started has not finished
    bool awake{false};  ///< the screen saver was turned off for that copy
    fs::path folder{};
    std::vector<settings::GameContentRow> before{}; ///< the rows before the bytes moved
};

ContentCopyWatch& content_copy_watch() {
    static ContentCopyWatch watch;
    return watch;
}

/// The check's progress. It runs on the check's thread and only stores counts.
bool on_settings_check(uint32_t done, uint32_t total, void* context) {
    if (context == nullptr)
        return false;
    auto* job = static_cast<ContentCheckJob*>(context);
    const std::lock_guard<std::mutex> lock(job->mutex);
    if (job->stop.load() || job->generation != job->worker_generation)
        return false;
    job->done = done;
    job->total = total;
    job->changed = true;
    return true;
}

/// Starts a check. The caller has joined any thread that was still running.
void launch_content_check(const fs::path& folder) {
    ContentCheckJob& job = content_check_job();
    if (folder.empty() || job.worker.joinable())
        return;
    job.stop.store(false);
    job.exited.store(false);
    uint64_t generation = 0;
    {
        const std::lock_guard<std::mutex> lock(job.mutex);
        ++job.generation;
        generation = job.generation;
        job.worker_generation = generation;
        job.running = true;
        job.restart = false;
        job.result_ready = false;
        job.changed = true;
        job.done = 0;
        job.total = 0;
        job.folder = folder;
        job.result = {};
    }
    job.worker = std::thread([generation, folder] {
        ContentCheckJob& held = content_check_job();
        ContentCheck check;
        bool threw = false;
        try {
            check = check_managed_content(
                folder, oa::data::content_bundles::bundles(), on_settings_check, &held, &held.stop
            );
        } catch (const std::exception&) {
            threw = true;
        }
        {
            const std::lock_guard<std::mutex> lock(held.mutex);
            if (!threw && held.generation == generation && !held.stop.load()) {
                held.result = std::move(check);
                held.result_ready = true;
            }
            held.running = false;
            held.changed = true;
        }
        held.exited.store(true);
    });
}

/// Asks a running check to stop. A restart waits until that thread has left,
/// so this does not join it.
void halt_content_check(bool restart_after, const fs::path& folder) {
    ContentCheckJob& job = content_check_job();
    job.stop.store(true);
    const std::lock_guard<std::mutex> lock(job.mutex);
    job.restart = restart_after && !folder.empty();
    if (!folder.empty())
        job.folder = folder;
    ++job.generation;
    job.result_ready = false;
    job.result = {};
    job.done = 0;
    job.total = 0;
    job.changed = true;
}

/// Joins a check that has left, then starts one that was asked for again.
void service_content_check() {
    ContentCheckJob& job = content_check_job();
    if (job.exited.load() && job.worker.joinable())
        job.worker.join();
    bool start = false;
    fs::path folder;
    {
        const std::lock_guard<std::mutex> lock(job.mutex);
        if (!job.worker.joinable() && job.restart && !content_copy_watch().active) {
            start = true;
            folder = job.folder;
            job.restart = false;
        }
    }
    if (start)
        launch_content_check(folder);
}

/// Starts a check, or asks the running one to start again when it leaves.
void request_content_check(const fs::path& folder) {
    if (folder.empty() || content_copy_watch().active)
        return;
    ContentCheckJob& job = content_check_job();
    if (job.worker.joinable() && !job.exited.load()) {
        halt_content_check(true, folder);
        return;
    }
    if (job.worker.joinable())
        job.worker.join();
    launch_content_check(folder);
}

/// Stops a copy and a check and waits for both. The dialog is closing, so the
/// wait is the time until the next chunk.
void finish_content_workers() {
    setup_request_cancel();
    ContentCheckJob& job = content_check_job();
    job.stop.store(true);
    {
        const std::lock_guard<std::mutex> lock(job.mutex);
        job.restart = false;
        ++job.generation;
        job.result_ready = false;
    }
    setup_stop_join();
    if (job.worker.joinable())
        job.worker.join();
    setup_release_located();
    ContentCopyWatch& watch = content_copy_watch();
    if (watch.awake)
        SDL_EnableScreenSaver();
    watch = {};
}

/// Lists every bundle with its size. An empty folder says nothing is copied.
/// A folder that has a copy says the check is starting, and does not hash.
void lay_game_content(settings::Dialog& dialog, const fs::path& user_folder) {
    const auto table = oa::data::content_bundles::bundles();
    const bool look = !user_folder.empty();
    const std::string waiting = checking_line(0, 0);
    dialog.game_content.clear();
    dialog.game_content.reserve(table.size());
    for (const auto& bundle : table) {
        settings::GameContentRow row;
        row.id = std::string(bundle.id);
        row.name = std::string(oa::data::languages::interface_text(bundle.english_name));
        uint64_t bytes = 0;
        for (const auto& file : bundle.files)
            bytes += file.size;
        row.size = settings::download_size_text(bytes);
        row.checking = look;
        row.check = look ? waiting : std::string(oa::data::languages::interface_text("not added"));
        dialog.game_content.push_back(std::move(row));
    }
}

/// Writes a finished check onto the rows. `found` is not kept.
void paint_check_result(settings::Dialog& dialog, const ContentCheck& check) {
    for (settings::GameContentRow& row : dialog.game_content) {
        const ContentBundleCheck* found = nullptr;
        for (const ContentBundleCheck& checked : check.bundles) {
            if (checked.id == row.id) {
                found = &checked;
                break;
            }
        }
        row.checking = false;
        row.copying = false;
        row.bar_done = -1;
        row.bar_whole = 1;
        if (found == nullptr) {
            row.check = std::string(oa::data::languages::interface_text("not added"));
            row.warn = false;
        } else {
            row.check = content_bundle_line(*found);
            row.warn = found->files_differ > 0;
        }
    }
}

/// Writes the check's percent onto every row.
void paint_checking(settings::Dialog& dialog, uint32_t done, uint32_t total) {
    const std::string line = checking_line(done, total);
    for (settings::GameContentRow& row : dialog.game_content) {
        row.checking = true;
        row.copying = false;
        row.warn = false;
        row.bar_done = -1;
        row.bar_whole = 1;
        row.check = line;
    }
}

/// The bundle of this id, or -1.
int find_copy_bundle(
    const std::vector<oa::ui::first_run::CopyBundle>& bundles, std::string_view id
) {
    for (int index = 0; index < static_cast<int>(bundles.size()); ++index) {
        if (bundles[static_cast<std::size_t>(index)].id == id)
            return index;
    }
    return -1;
}

/// Writes a copy's progress, its check, or "Copied" after a check that was
/// stopped once the bytes had landed.
void paint_copy(
    settings::Dialog& dialog,
    const oa::ui::first_run::CopyView& view,
    const std::vector<settings::GameContentRow>& before
) {
    dialog.game_content = before;
    if (view.hashing) {
        paint_checking(dialog, view.check_done, view.check_total);
        // The bytes have landed. Cancel stays on the row that was copying, so
        // the check can be stopped without closing Settings.
        for (settings::GameContentRow& row : dialog.game_content) {
            const int index = find_copy_bundle(view.bundles, row.id);
            if (index < 0)
                continue;
            if (view.bundles[static_cast<std::size_t>(index)].phase ==
                oa::ui::first_run::CopyPhase::copying)
                row.copying = true;
        }
        return;
    }
    const bool landed = view.bytes_landed && !view.running && !view.checked;
    for (settings::GameContentRow& row : dialog.game_content) {
        const int index = find_copy_bundle(view.bundles, row.id);
        row.copying = false;
        row.checking = false;
        row.bar_done = -1;
        row.bar_whole = 1;
        if (index < 0)
            continue;
        const oa::ui::first_run::CopyBundle& bundle = view.bundles[static_cast<std::size_t>(index)];
        if (view.checked) {
            if (bundle.phase == oa::ui::first_run::CopyPhase::waiting) {
                row.check = std::string(oa::data::languages::interface_text("not added"));
                row.warn = false;
            } else {
                row.check = bundle.amount.empty()
                                ? std::string(oa::data::languages::interface_text("Copied"))
                                : bundle.amount;
                row.warn = bundle.phase == oa::ui::first_run::CopyPhase::differ;
            }
            continue;
        }
        if (landed && bundle.phase != oa::ui::first_run::CopyPhase::waiting) {
            row.check = view.verdict.empty()
                            ? std::string(oa::data::languages::interface_text("Copied"))
                            : view.verdict;
            row.warn = !view.verdict.empty();
            continue;
        }
        if (bundle.phase != oa::ui::first_run::CopyPhase::copying)
            continue;
        row.copying = true;
        row.warn = false;
        row.bar_done = bundle.done;
        row.bar_whole = bundle.whole > 0 ? bundle.whole : 1;
        row.check = view.overall.empty() ? files_progress_line(0, 0) : view.overall;
    }
}

/// True when two row lists would draw differently.
bool content_rows_differ(
    const std::vector<settings::GameContentRow>& left,
    const std::vector<settings::GameContentRow>& right
) {
    if (left.size() != right.size())
        return true;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const settings::GameContentRow& a = left[index];
        const settings::GameContentRow& b = right[index];
        if (a.check != b.check || a.warn != b.warn || a.copying != b.copying ||
            a.checking != b.checking || a.bar_done != b.bar_done || a.bar_whole != b.bar_whole)
            return true;
    }
    return false;
}

/// True when a row was still being checked.
bool rows_checking(const std::vector<settings::GameContentRow>& rows) {
    for (const settings::GameContentRow& row : rows)
        if (row.checking)
            return true;
    return false;
}

/// Asks for a place and starts the copy on the copying step's thread.
/// The dialog keeps drawing. A second copy while one is running is ignored.
///
/// @param[in,out] dialog the dialog
/// @param window the window the picker is attached to
/// @param user_folder the player's folder
void start_game_content_copy(
    settings::Dialog& dialog, SDL_Window* window, const fs::path& user_folder
) {
    ContentCopyWatch& watch = content_copy_watch();
    if (watch.active || user_folder.empty())
        return;
    const bool was_checking = rows_checking(dialog.game_content);
    halt_content_check(false, user_folder);
    oa::ui::first_run::Model model;
    if (!setup_locate_source(window, user_folder, model)) {
        setup_release_located();
        if (was_checking)
            halt_content_check(true, user_folder);
        return;
    }
    watch.before = dialog.game_content;
    for (settings::GameContentRow& row : watch.before) {
        row.copying = false;
        row.bar_done = -1;
    }
    watch.folder = user_folder;
    if (!setup_begin_copy(user_folder, model)) {
        setup_release_located();
        watch.before.clear();
        if (was_checking)
            halt_content_check(true, user_folder);
        return;
    }
    watch.active = true;
    if (!watch.awake) {
        SDL_DisableScreenSaver();
        watch.awake = true;
    }
}

} // namespace

namespace {

/// Returns the size of the desktop of the display a window is on, as a
/// screen size keeps it.
///
/// @param options the parsed command line (Options::display_modes)
/// @param window the game's window; null for none
/// @param start_desktop the primary display's desktop as read at start
/// @return the desktop's size; start_desktop without a window, or where the
///     display does not report one a screen size can keep
settings::ScreenSize
window_desktop(const Options& options, SDL_Window* window, settings::ScreenSize start_desktop) {
    const SDL_DisplayID display = window != nullptr ? SDL_GetDisplayForWindow(window) : 0;
    if (display == 0)
        return start_desktop;
    // A screen size keeps each side in 16 bits.
    constexpr int32_t widest = std::numeric_limits<uint16_t>::max();
    const auto desktop = display_report(options, display).desktop.size;
    if (desktop.width <= 0 || desktop.height <= 0 || desktop.width > widest ||
        desktop.height > widest)
        return start_desktop;
    return {static_cast<uint16_t>(desktop.width), static_cast<uint16_t>(desktop.height)};
}

/// The version the dialog's header shows.
constexpr const char* kVersionText = "v" OA_ENGINE_VERSION;

/// The generation of the update snapshot Updates last painted. A check that
/// finishes on another section is painted the next time Updates is open.
uint64_t& updates_generation_seen() {
    static uint64_t seen = 0;
    return seen;
}

/// Returns a version without a leading v, as the status line writes it.
///
/// @param header the header's version text
/// @return the version
std::string_view release_version(std::string_view header) noexcept {
    if (!header.empty() && (header.front() == 'v' || header.front() == 'V'))
        header.remove_prefix(1);
    return header;
}

/// Returns the word the update preference stores for a way.
///
/// @param mode the way
/// @return automatically, when-asked or never
std::string_view update_mode_word(settings::UpdateCheck mode) noexcept {
    switch (mode) {
    case settings::UpdateCheck::when_asked:
        return "when-asked";
    case settings::UpdateCheck::never:
        return "never";
    case settings::UpdateCheck::automatically:
        return "automatically";
    }
    return "automatically";
}

/// Returns where the host's snapshot stands, as the page's lines name it.
///
/// @param state the snapshot's state
/// @return the stand
settings::UpdateStand update_stand(update::CheckState state) noexcept {
    switch (state) {
    case update::CheckState::idle:
        return settings::UpdateStand::idle;
    case update::CheckState::checking:
        return settings::UpdateStand::checking;
    case update::CheckState::up_to_date:
        return settings::UpdateStand::up_to_date;
    case update::CheckState::offered:
        return settings::UpdateStand::offered;
    case update::CheckState::failed:
        return settings::UpdateStand::failed;
    case update::CheckState::cancelled:
        return settings::UpdateStand::cancelled;
    }
    return settings::UpdateStand::idle;
}

/// Fills Updates from a snapshot. The way is the one the dialog shows, so an
/// unsaved change is not put back.
///
/// @param[in,out] dialog the dialog
/// @param snapshot the check
/// @param version this build's version, without a leading v
void paint_updates(
    settings::Dialog& dialog, const update::CheckSnapshot& snapshot, std::string_view version
) {
    settings::UpdateReading reading;
    reading.version = version;
    reading.stand = update_stand(snapshot.state);
    reading.never = dialog.chosen.update_check == settings::UpdateCheck::never;
    reading.checked_at = snapshot.checked_at;
    reading.now = std::chrono::duration_cast<std::chrono::seconds>(
                      std::chrono::system_clock::now().time_since_epoch()
    )
                      .count();
    if (snapshot.offer.release != nullptr)
        reading.offer_version = snapshot.offer.release->version_text;
    reading.replaces_itself = update::replaces_itself();
    if (!reading.replaces_itself)
        reading.packager = update::packager_name();
    const settings::UpdateLines lines = settings::update_lines(reading);
    dialog.updates.version_line = lines.version_line;
    dialog.updates.status = lines.status;
    dialog.updates.can_check = lines.can_check;
    dialog.updates.offered = lines.offered;
    dialog.updates.updated_by = lines.updated_by;
    dialog.updates.mode = static_cast<int32_t>(dialog.chosen.update_check);
}

/// Notes the generation Updates was just painted from.
///
/// @param snapshot the check
void note_updates_generation(const update::CheckSnapshot& snapshot) noexcept {
    updates_generation_seen() = snapshot.generation;
}

/// Paints Updates when the check has moved and that section is open.
///
/// @param snapshot the check
/// @param[in,out] dialog the dialog
/// @return true when the lines changed
bool refill_updates(const update::CheckSnapshot& snapshot, settings::Dialog& dialog) {
    if (dialog.page != settings::Page::updates)
        return false;
    if (snapshot.generation == updates_generation_seen())
        return false;
    paint_updates(dialog, snapshot, release_version(dialog.version));
    note_updates_generation(snapshot);
    return true;
}

/// What the player is told when the preferences file could not be written.
constexpr std::string_view kSaveFailedText = "Settings were not saved.";

/// The widest the main menu's message box about a failed save is, in source pixels.
constexpr int32_t kSaveFailedMessageWidth = 300;

/// The installation's options file, whose UnitLimit the unit limit defaults to.
constexpr std::string_view kInstallationIniName = "totala.ini";

/// SDL's names of the video drivers that draw no window.
constexpr std::array<std::string_view, 2> kWindowlessVideoDrivers{"dummy", "offscreen"};

/// Returns a text in lower case, ASCII letters only.
///
/// @param text the text
/// @return the text with A to Z lowered
std::string ascii_lower(std::string_view text) {
    std::string lowered(text);
    for (auto& character : lowered)
        character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
    return lowered;
}

} // namespace

settings::Inputs Runtime::EngineSettingsState::inputs(const Runtime& runtime) {
    settings::Inputs inputs{};
    inputs.players_own_profile = !runtime.options_.preferences_file.has_value();
#if defined(SDL_PLATFORM_MACOS)
    inputs.macos = true;
#endif
    if (runtime.engine_settings_) {
        inputs.installation_ini = runtime.engine_settings_->installation_ini;
        inputs.raspberry_pi = runtime.engine_settings_->raspberry_pi;
        inputs.light_machine = runtime.engine_settings_->light_machine;
        inputs.desktop = runtime.engine_settings_->desktop;
        inputs.steam_deck_panel_hz = runtime.engine_settings_->steam_deck_panel_hz;
    }
    inputs.units_per_player = runtime.limits_.units_per_player;
    inputs.native_density_windows = runtime.options_.native_density_windows;
    if (runtime.engine_settings_) {
        inputs.mod_folders = runtime.engine_settings_->mod_folders;
        inputs.profile_id = runtime.engine_settings_->profile_id;
    }
    return inputs;
}

std::string Runtime::EngineSettingsState::read_installation_ini(const fs::path& game_directory) {
    std::error_code error;
    for (fs::directory_iterator entry(game_directory, error), end; !error && entry != end;
         entry.increment(error)) {
        if (ascii_lower(path_to_utf8(entry->path().filename())) != kInstallationIniName ||
            !entry->is_regular_file(error))
            continue;
        std::ifstream input(entry->path(), std::ios::binary);
        std::string text(settings::installation_ini_limit, '\0');
        input.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(std::max<std::streamsize>(input.gcount(), 0)));
        return text;
    }
    return {};
}

bool Runtime::EngineSettingsState::live_switch_alt(const Runtime& runtime) {
    const uint16_t flags = runtime.match_ ? runtime.match_->state().game.graphics_flags
                                          : runtime.preferences_.graphics_flags;
    return (flags & init::preference_flags::switch_alt) != 0;
}

void Runtime::EngineSettingsState::take_live_settings(Runtime& runtime) {
    auto& current = runtime.engine_settings_state().current;
    current.switch_alt = live_switch_alt(runtime);
    current.frame_stats = runtime.frame_stats_shown_;
}

std::optional<std::string> Runtime::EngineSettingsState::flush(Runtime& runtime) {
    try {
        runtime.flush_preferences();
    } catch (const std::exception& error) {
        return std::string(error.what());
    }
    return std::nullopt;
}

std::vector<settings::ScreenSize>
Runtime::EngineSettingsState::offered_screen_sizes(const Runtime& runtime) {
    const int32_t minimum_height = view_rules::minimum_mode_height(runtime.ui_rules());
    if (runtime.sdl_.window == nullptr)
        return oa::app::offered_screen_sizes(
            runtime.options_, SDL_GetPrimaryDisplay(), start_use(runtime.options_), minimum_height
        );
    SDL_DisplayID display = SDL_GetDisplayForWindow(runtime.sdl_.window);
    if (display == 0)
        display = SDL_GetPrimaryDisplay();
    // Full screen lists the display's modes, whether or not the output can
    // switch to one of them. A window lists the sizes that fit the desktop.
    const bool full_screen = (SDL_GetWindowFlags(runtime.sdl_.window) & SDL_WINDOW_FULLSCREEN) != 0;
    const auto use = full_screen ? oa::platform::display_modes::Use::full_screen
                                 : oa::platform::display_modes::Use::window;
    return oa::app::offered_screen_sizes(runtime.options_, display, use, minimum_height);
}

settings::ScreenSize Runtime::EngineSettingsState::screen_size_now(const Runtime& runtime) {
    // A screen size keeps each side in 16 bits.
    constexpr int widest = std::numeric_limits<uint16_t>::max();
    const SDL_Point now = runtime.screen_size_now();
    if (now.x <= 0 || now.y <= 0 || now.x > widest || now.y > widest)
        return settings::desktop_screen_size;
    return {static_cast<uint16_t>(now.x), static_cast<uint16_t>(now.y)};
}

settings::ScreenSize Runtime::EngineSettingsState::screen_size_after(
    const Runtime& runtime, settings::ScreenSize setting
) {
    if (setting != settings::desktop_screen_size)
        return setting;
    // Desktop leaves a window as it is, and shows full screen at the
    // desktop's own size.
    if (runtime.sdl_.window != nullptr &&
        (SDL_GetWindowFlags(runtime.sdl_.window) & SDL_WINDOW_FULLSCREEN) == 0)
        return screen_size_now(runtime);
    const settings::ScreenSize desktop = window_desktop(
        runtime.options_,
        runtime.sdl_.window,
        runtime.engine_settings_ ? runtime.engine_settings_->desktop : settings::desktop_screen_size
    );
    return desktop != settings::desktop_screen_size ? desktop : screen_size_now(runtime);
}

settings::ScreenSize Runtime::EngineSettingsState::screen_size_in_effect(
    const Runtime& runtime, const std::vector<settings::ScreenSize>& offered
) {
    return screen_size_shown(
        runtime,
        runtime.engine_settings_ ? runtime.engine_settings_->current.screen_size
                                 : settings::desktop_screen_size,
        offered
    );
}

settings::ScreenSize Runtime::EngineSettingsState::screen_size_shown(
    const Runtime& runtime,
    settings::ScreenSize setting,
    const std::vector<settings::ScreenSize>& offered
) {
    settings::ScreenSize size = setting;
    // A screen size keeps each side in 16 bits.
    constexpr int widest = std::numeric_limits<uint16_t>::max();
    int width = 0;
    int height = 0;
    if (size == settings::desktop_screen_size && runtime.sdl_.window != nullptr &&
        SDL_GetWindowSize(runtime.sdl_.window, &width, &height) && width > 0 && height > 0 &&
        width <= widest && height <= widest)
        size = {static_cast<uint16_t>(width), static_cast<uint16_t>(height)};
    if (size == settings::desktop_screen_size && runtime.engine_settings_)
        size = runtime.engine_settings_->desktop;
    std::vector<oa::platform::display_modes::Size> sizes;
    sizes.reserve(offered.size());
    for (const settings::ScreenSize each : offered)
        sizes.push_back({each.width, each.height});
    const auto nearest =
        oa::platform::display_modes::nearest_offered(sizes, {size.width, size.height});
    return nearest ? offered[*nearest] : size;
}

settings::ScreenSize Runtime::EngineSettingsState::stored_screen_size(
    const Runtime& runtime, settings::ScreenSize size
) {
    const bool full_screen =
        runtime.sdl_.window != nullptr
            ? (SDL_GetWindowFlags(runtime.sdl_.window) & SDL_WINDOW_FULLSCREEN) != 0
            : start_use(runtime.options_) == oa::platform::display_modes::Use::full_screen;
    const settings::ScreenSize desktop = window_desktop(
        runtime.options_,
        runtime.sdl_.window,
        runtime.engine_settings_ ? runtime.engine_settings_->desktop : settings::desktop_screen_size
    );
    return full_screen && size == desktop ? settings::desktop_screen_size : size;
}

void Runtime::EngineSettingsState::choose_screen_size(Runtime& runtime, settings::ScreenSize size) {
    auto& state = runtime.engine_settings_state();
    const settings::ScreenSize chosen = stored_screen_size(runtime, size);
    state.current.screen_size = chosen;
    runtime.preference_values_[std::string(settings::key::screen_size)] =
        settings::screen_size_text(chosen);
    runtime.preferences_dirty_ = true;
}

void Runtime::EngineSettingsState::save_frame_stats(Runtime& runtime, bool shown) {
    runtime.show_frame_stats(shown);
    runtime.engine_settings_state().current.frame_stats = shown;
    runtime.preference_values_[std::string(settings::key::frame_stats)] =
        std::string(shown ? "1" : "0");
    runtime.preferences_dirty_ = true;
    if (const auto failure = flush(runtime))
        report_failed_save(runtime, *failure);
}

void Runtime::EngineSettingsState::report_failed_save(Runtime& runtime, const std::string& reason) {
    std::cerr << "open-annihilation: settings were not saved: " << reason << '\n';
    // The engine's own words, in the player's language when the interface
    // catalogue has them.
    const std::string_view text = oa::data::languages::interface_text(kSaveFailedText);
    if (runtime.screen_ == Screen::match && runtime.match_)
        runtime.post_match_message(text, oa::sim::messages::kind_status);
    else if (runtime.screen_ == Screen::main_menu)
        runtime.show_frontend_message(
            std::string(text),
            kSaveFailedMessageWidth,
            entry::message_show_ok,
            entry::message_fit_width
        );
}

uint16_t Runtime::EngineSettingsState::run_unit_limit(Runtime& runtime) {
    const uint16_t run = runtime.frontend_game().max_units_setting;
    return run != 0 ? run : runtime.engine_settings_state().current.unit_limit;
}

void Runtime::EngineSettingsState::keep_run_unit_limit(Runtime& runtime) {
    auto& game = runtime.frontend_game();
    if (game.max_units_setting == 0)
        game.max_units_setting = runtime.engine_settings_state().current.unit_limit;
}

void Runtime::EngineSettingsState::start_skirmish(
    Runtime& runtime, uint16_t units_per_player, bool run_unit_limit
) {
    runtime.bootstrap_match(
        {.units_per_player = units_per_player,
         .seat_roster = true,
         .run_unit_limit = run_unit_limit}
    );
}

uint16_t Runtime::EngineSettingsState::restart_unit_limit(Runtime& runtime) {
    if (runtime.match_ && runtime.match_->state().game.max_units_setting != 0)
        return runtime.match_->state().game.max_units_setting;
    return run_unit_limit(runtime);
}

void Runtime::EngineSettingsState::start_path_credit(Runtime& runtime, bool shared_or_replay) {
    if (!runtime.match_)
        return;
    auto& state = runtime.engine_settings_state();
    state.path_credit_held = shared_or_replay;
    runtime.match_->path_search_jobs().tick_credit = settings::match_path_search_nodes(
        state.current, shared_or_replay, runtime.limits_.path_search.nodes
    );
}

void Runtime::EngineSettingsState::hold_path_credit(Runtime& runtime, uint32_t extension_bits) {
    auto& state = runtime.engine_settings_state();
    if (!runtime.match_ || state.path_credit_held ||
        (extension_bits & (extension_state::shared_match | extension_state::replay)) == 0)
        return;
    state.path_credit_held = true;
    runtime.match_->path_search_jobs().tick_credit = runtime.limits_.path_search.nodes;
}

bool Runtime::EngineSettingsState::escape_opens_menu(Runtime& runtime) {
    return runtime.engine_settings_state().current.escape_opens_menu;
}

void Runtime::EngineSettingsState::note_pointer_source(Runtime& runtime, const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        runtime.engine_settings_state().finger_pointer = event.motion.which == SDL_TOUCH_MOUSEID;
        return;
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
        runtime.engine_settings_state().finger_pointer = event.button.which == SDL_TOUCH_MOUSEID;
        return;
    case SDL_EVENT_MOUSE_WHEEL:
        runtime.engine_settings_state().finger_pointer = false;
        return;
    default:
        return;
    }
}

int32_t
Runtime::EngineSettingsState::finger_reach(const Runtime& runtime, double canvas_per_pixel) {
    // The pick distance in window points, as canvas pixels of the screen
    // shown, then as pixels of what is drawn at the scale.
    const double canvas = static_cast<double>(oa::ui::touch_hud::gadget_pick_points) *
                          static_cast<double>(runtime.touch_px_per_point());
    const double per_pixel = canvas_per_pixel > 0.0 ? canvas_per_pixel : 1.0;
    return std::max(int32_t{1}, static_cast<int32_t>(std::lround(canvas / per_pixel)));
}

void Runtime::destroy_engine_settings_state(EngineSettingsState* state) noexcept {
    // A retry the player never confirmed is put back before the run ends,
    // so that its clean exit writes the records the dialog opened with.
    if (state != nullptr && state->records_host != nullptr)
        state->records_host->restore_records();
    delete state;
}

Runtime::EngineSettingsState& Runtime::engine_settings_state() {
    if (!engine_settings_)
        engine_settings_.reset(new EngineSettingsState{});
    return *engine_settings_;
}

void Runtime::load_engine_settings() {
    auto& state = engine_settings_state();
    // An explicit preferences file plays as the game does everywhere: the
    // installation's options are not read for it.
    state.installation_ini = options_.preferences_file
                                 ? std::string{}
                                 : EngineSettingsState::read_installation_ini(options_.game_dir);
    // The machine as the window was opened for it (starting_screen_size).
    const auto start = start_inputs(options_, desktop_size(options_));
    state.raspberry_pi = start.raspberry_pi;
    state.light_machine = start.light_machine;
    state.desktop = start.desktop;
    state.steam_deck_panel_hz = start.steam_deck_panel_hz;
    // A picked folder that is no longer the mod stored is forgotten; the
    // file loses its key with the next save.
    if (settings::remembered_picked_folder(preference_values_).empty() &&
        preference_values_.erase(std::string(settings::key::picked_mod_directory)) > 0)
        preferences_dirty_ = true;
    list_offered_mods();
    state.physical_memory = oa::platform::read_machine_traits().memory;
    // The overrides are read under the profile's id, and laid over it as
    // the settings are put in effect.
    load_profile_layers();
    const bool switch_alt = (preferences_.graphics_flags & init::preference_flags::switch_alt) != 0;
    auto read =
        settings::read_settings(preference_values_, EngineSettingsState::inputs(*this), switch_alt);
    state.dropped_mod_folder.clear();
    if (!options_.mod_dir.empty() || options_.base_game) {
        // --mod-dir and --base-game set the Mod setting aside for the run,
        // which plays it again only from a start without them.
        state.playing_mod_folder = read.mod_folder;
    } else if (!read.mod_folder.empty() && read.mod_folder != state.playing_mod_folder) {
        // The start dropped a stored mod folder that is gone and plays the
        // game folder as it is: the setting shows what plays.
        std::error_code missing;
        if (!fs::is_directory(path_from_utf8(read.mod_folder), missing)) {
            state.dropped_mod_folder = std::move(read.mod_folder);
            read.mod_folder.clear();
        }
    }
    apply_engine_settings(read);
    // The layers start afresh: the overrides read are laid over the profile
    // even when the settings in effect held them already.
    apply_hack_overrides();
    // The run's unit limit starts at the setting.
    frontend_game().max_units_setting = read.unit_limit;
    show_frame_stats(read.frame_stats);
    // The battle room's RES column offers the display's sizes and starts
    // at the size the game plays at; a 3.1c machine shows either as sent.
    namespace mp = oa::ui::frontend_multiplayer;
    constexpr int32_t game_bits = 8;
    mp::multiplayer_bind_display_modes(
        {this,
         [](void* context, mp::DisplayMode* out, int32_t capacity) {
             const auto sizes =
                 EngineSettingsState::offered_screen_sizes(*static_cast<const Runtime*>(context));
             int32_t count = 0;
             for (const auto size : sizes) {
                 if (count >= capacity)
                     break;
                 out[count++] = {size.width, size.height, game_bits};
             }
             return count;
         },
         [](void* context) {
             const auto& runtime = *static_cast<const Runtime*>(context);
             const auto size = EngineSettingsState::screen_size_in_effect(
                 runtime, EngineSettingsState::offered_screen_sizes(runtime)
             );
             return mp::DisplayMode{size.width, size.height, game_bits};
         },
         [](void* context, int32_t width, int32_t height) {
             // A screen size keeps each side in 16 bits.
             constexpr int32_t widest = std::numeric_limits<uint16_t>::max();
             if (width <= 0 || height <= 0 || width > widest || height > widest)
                 return;
             EngineSettingsState::take_screen_size(
                 *static_cast<Runtime*>(context),
                 {static_cast<uint16_t>(width), static_cast<uint16_t>(height)}
             );
         }}
    );
}

void Runtime::list_offered_mods() {
    auto& state = engine_settings_state();
    // The game folder is the last of the folders, below any mod folder
    // layered over it.
    state.mod_folders.clear();
    state.mod_names.clear();
    state.mod_details.clear();
    const auto& game_folder =
        options_.game_folders.empty() ? options_.game_dir : options_.game_folders.back();
    std::vector<fs::path> offered = list_mod_folders(game_folder);
    if (!user_folder_.empty()) {
        const auto own = list_mods_in(user_folder_ / std::string(user_mods_folder_name));
        offered.insert(offered.end(), own.begin(), own.end());
    }
    // The picked folder is listed only while it is the mod stored.
    if (const std::string picked = settings::remembered_picked_folder(preference_values_);
        !picked.empty()) {
        std::error_code missing;
        const fs::path folder = path_from_utf8(picked);
        if (fs::is_directory(folder, missing))
            offered.push_back(folder);
    }
    // The mod folder played is the one this start layered. An expansion
    // bundle is another game folder and is not offered as that mod.
    state.playing_mod_folder.clear();
    if (!options_.layered_mod_folder.empty()) {
        std::error_code absolute_error;
        const fs::path absolute = fs::absolute(options_.layered_mod_folder, absolute_error);
        state.playing_mod_folder = path_to_utf8(
            (absolute_error ? options_.layered_mod_folder : absolute).lexically_normal()
        );
        offered.push_back(options_.layered_mod_folder);
    }
    // A folder of the player's own Mods folder offers ROLL BACK while its
    // .backup keeps an earlier version of its mod that the game can play,
    // checked as a pick is.
    std::error_code own_error;
    const fs::path own_mods =
        user_folder_.empty()
            ? fs::path()
            : fs::absolute(user_folder_ / std::string(user_mods_folder_name), own_error)
                  .lexically_normal();
    const auto playable = [&](const fs::path& kept) {
        const ModChoice choice{{}, {}, options_.accept_unimplemented_hacks, &preference_values_};
        return check_picked_mod_folder(kept, game_folder, choice).refusal.empty();
    };
    for (const auto& folder : offered) {
        std::error_code error;
        const fs::path absolute = fs::absolute(folder, error);
        const std::string path = path_to_utf8((error ? folder : absolute).lexically_normal());
        // A folder offered twice, as when the Mods folder is the game
        // folder's, is listed once.
        if (std::find(state.mod_folders.begin(), state.mod_folders.end(), path) !=
            state.mod_folders.end())
            continue;
        ModSummary summary = read_mod_summary(folder);
        EngineSettingsState::OfferedMod details;
        details.version = std::move(summary.version);
        details.description = std::move(summary.description);
        details.has_profile = summary.has_profile;
        details.profile_id = std::move(summary.id);
        details.badge = std::move(summary.badge);
        if (!own_mods.empty() && path_from_utf8(path).parent_path() == own_mods)
            if (const auto backup = package_install::oamod::read_backup(folder);
                backup && backup->kind == package_install::FolderKind::package &&
                playable(
                    entry_without_case(folder, package_install::backup_folder_name)
                        .value_or(folder / std::string(package_install::backup_folder_name))
                )) {
                details.roll_back = true;
                details.kept_version = backup->version;
                details.kept_revision = backup->revision;
            }
        state.mod_folders.push_back(path);
        state.mod_names.push_back(std::move(summary.title));
        state.mod_details.push_back(std::move(details));
    }
    ++state.offered_generation;
}

void Runtime::request_soft_restart() {
    soft_restart_requested_ = true;
    exit_requested_ = true;
}

oa::present::TextStyle Runtime::text_style() const {
    oa::present::TextStyle style =
        engine_settings_
            ? settings::text_style(engine_settings_->current)
            : settings::text_style(settings::default_settings(EngineSettingsState::inputs(*this)));
    // A language drawn in the modern fonts has them on whatever the setting.
    if (language_needs_modern_fonts())
        style.modern_fonts = true;
    return style;
}

GameTextPreferences game_text_preferences(const Runtime& runtime) noexcept {
    // The settings may not exist yet, and building their defaults allocates:
    // a failure gives the defaults rather than ending the program, and says
    // why once a run, so that a broken setting is not silent.
    static std::atomic<bool> reported{false};
    const auto report = [](const char* why) noexcept {
        if (!reported.exchange(true))
            std::fprintf(
                stderr,
                "open-annihilation: the text settings could not be read, so extensions draw "
                "at the defaults: %s\n",
                why
            );
    };
    try {
        const oa::present::TextStyle style = runtime.text_style();
        return game_text_preferences_of(style.modern_fonts, style.size);
    } catch (const std::exception& error) {
        report(error.what());
    } catch (...) {
        report("an unknown error");
    }
    return GameTextPreferences{};
}

bool Runtime::unicode_chat_on() const {
    // A mod profile's Unicode text, and a language shown in UTF-8 or whose
    // pack asks for it, turn it on whatever the setting.
    const auto& text = ui_rules().text_rendering;
    if ((text.enabled && text.unicode) || oa::data::languages::turns_unicode_chat_on(
                                              shown_language(), language_unicode_chat() != nullptr
                                          ))
        return true;
    return engine_settings_ && engine_settings_->current.unicode_chat;
}

render_policy::MenuScaling Runtime::menu_scaling() const noexcept {
    return engine_settings_ ? engine_settings_->current.menu_scaling
                            : render_policy::MenuScaling::sharp;
}

settings::ExplosionFlash Runtime::explosion_flash() const noexcept {
    return engine_settings_ ? engine_settings_->current.explosion_flash
                            : settings::ExplosionFlash::full;
}

settings::FogOfWar Runtime::fog_of_war() const noexcept {
    return engine_settings_ ? engine_settings_->current.fog_of_war : settings::FogOfWar::smooth;
}

bool Runtime::smooth_motion() const noexcept {
    return !engine_settings_ || engine_settings_->current.smooth_motion;
}

double Runtime::past_map_edge_share() const noexcept {
    return settings::past_map_edge_share(
        engine_settings_ ? engine_settings_->current.view_past_map_edge
                         : settings::ViewPastMapEdge::one_half
    );
}

double Runtime::match_chrome_most_scale() const noexcept {
    namespace layout = oa::ui::display_layout;
    return !engine_settings_ || engine_settings_->current.hud_scaling ? layout::kMaxChromeScale
                                                                      : 1.0;
}

const settings::EngineSettings& Runtime::engine_settings() {
    EngineSettingsState::take_live_settings(*this);
    return engine_settings_state().current;
}

void Runtime::apply_engine_settings(const settings::EngineSettings& chosen) {
    auto& state = engine_settings_state();
    const settings::EngineSettings before = state.current;
    state.current = chosen;
    if (before.allow_full_3d != chosen.allow_full_3d ||
        before.unlocked_camera != chosen.unlocked_camera)
        apply_live_camera_settings(chosen.allow_full_3d, chosen.unlocked_camera);
    // Turning the wheel zoom off brings the battlefield back to its own scale.
    if (before.wheel_zoom && !chosen.wheel_zoom && match_)
        EngineSettingsState::ease_zoom_about_centre(*this, kDefaultBattlefieldZoom);
    // A view past the zoom's new limits comes back within them about the
    // battlefield's centre, at the next frame (step_match_zoom).
    else if (
        (before.max_zoom_out != chosen.max_zoom_out || before.max_zoom_in != chosen.max_zoom_in) &&
        match_
    ) {
        const float held = std::clamp(match_zoom_target_, least_match_zoom(), most_match_zoom());
        if (held != match_zoom_target_ || match_zoom_ < least_match_zoom() ||
            match_zoom_ > most_match_zoom())
            EngineSettingsState::ease_zoom_about_centre(*this, held);
    }
    // A view past the new limits of View past the map's edge is held within
    // them from the next frame, not from where it lies now.
    if (before.view_past_map_edge != chosen.view_past_map_edge)
        view_hold_ = {};
    // Turning middle-click panning off stops a pan that is already running.
    if (before.middle_click_pan && !chosen.middle_click_pan) {
        middle_pan_ = {};
        middle_pan_swallow_button_ = 0;
    }
    // ui.megamap acts only while the wheel zoom is off.
    if (before.wheel_zoom != chosen.wheel_zoom && match_)
        megamap_wheel_zoom_changed();
    // SwitchAlt, where the keys read it: the match's options and the
    // frontend's preferences, which the match's options go back into.
    const auto with_switch_alt = [&](uint16_t flags) {
        return static_cast<uint16_t>(
            (flags & ~init::preference_flags::switch_alt) |
            (chosen.switch_alt ? init::preference_flags::switch_alt : 0)
        );
    };
    preferences_.graphics_flags = with_switch_alt(preferences_.graphics_flags);
    if (match_)
        match_->state().game.graphics_flags = with_switch_alt(match_->state().game.graphics_flags);
    // The next new game plays at a changed limit; a running game keeps its own.
    if (chosen.unit_limit != before.unit_limit)
        frontend_game().max_units_setting = chosen.unit_limit;
    // --max-fps holds for the run.
    if (!options_.max_frames_per_second_given)
        options_.max_frames_per_second = chosen.max_frame_rate;
    unit_supersampling_ = oa::present::model::unit_supersampling_from_factor(
                              static_cast<uint32_t>(chosen.anti_aliasing)
    )
                              .value_or(oa::present::model::UnitSupersampling::off);
    // In Full the row's level asks the graphics card's world target for its
    // factor instead, which the next Full frame takes (ensure_full_world_target).
    if (chosen.frame_stats != before.frame_stats)
        show_frame_stats(chosen.frame_stats);
    // Off applies at once; so do Basic and Full, but in a shared game or a
    // replay, which keeps the tier it began with until it ends.
    if (chosen.hardware_acceleration != before.hardware_acceleration) {
        update_render_tier();
        if (!full_presentation() && !options_.headless_check)
            reset_live_renderer();
    }
    apply_vertical_sync();
    // Menu scaling applies at once to a screen drawn as one frame, and to a
    // match drawn at a screen size and scaled to the screen: whole steps
    // set the window's presentation, and every frame takes its filter.
    if (chosen.menu_scaling != before.menu_scaling &&
        (screen_ != Screen::match || scaled_frame_width_ > 0))
        apply_output_mode();
    // HUD scaling lays a game out again at once.
    else if (chosen.hud_scaling != before.hud_scaling && screen_ == Screen::match)
        apply_output_mode();
    if (chosen.developer_mode != before.developer_mode ||
        chosen.hack_overrides != before.hack_overrides)
        apply_hack_overrides();
    // The language shows at once, on the screen under the settings too.
    set_language_choice(chosen.language);
    // The Touch section takes effect at once too: the touch controls read
    // the settings in effect at every finger and every frame.
    // So does Include in device backups, on the game files themselves.
    if (chosen.game_files_backed_up != before.game_files_backed_up)
        EngineSettingsState::apply_game_files_backups(*this, chosen.game_files_backed_up);
}

const oa::data::mod_profile::ModProfile* Runtime::mod_profile() const noexcept {
    if (engine_settings_ && engine_settings_->layered)
        return engine_settings_->plays_base_rules ? nullptr : engine_settings_->played.get();
    return options_.mod_profile.get();
}

const oa::data::mod_profile::ModProfile* Runtime::next_match_profile() const noexcept {
    if (!engine_settings_ || !engine_settings_->layered)
        return options_.mod_profile.get();
    const auto& state = *engine_settings_;
    // A game without a mod plays 3.1c's rules, and no profile, while its
    // overrides change none of them.
    if (!options_.mod_profile && state.latest && state.latest->sim_hash == state.base_sim_hash)
        return nullptr;
    return state.latest.get();
}

void Runtime::play_latest_profile() noexcept {
    if (!engine_settings_ || !engine_settings_->layered || !engine_settings_->played)
        return;
    auto& state = *engine_settings_;
    const auto* next = next_match_profile();
    state.plays_base_rules = next == nullptr;
    // Copied into the one object the run plays by, which keeps its place.
    if (next != nullptr && next != state.played.get()) {
        try {
            *state.played = *next;
        } catch (const std::exception& error) {
            std::cerr << "open-annihilation: developer mode: the rules cannot be put in play: "
                      << error.what() << '\n';
        }
    }
}

const oa::data::mod_profile::UiRules& Runtime::ui_rules() const noexcept {
    static const oa::data::mod_profile::UiRules base{};
    if (engine_settings_ && engine_settings_->layered)
        return engine_settings_->latest ? engine_settings_->latest->ui : base;
    const auto* profile = options_.mod_profile.get();
    return profile != nullptr ? profile->ui : base;
}

bool Runtime::developer_mode() const noexcept {
    return engine_settings_ && engine_settings_->current.developer_mode;
}

void Runtime::load_profile_layers() {
    namespace profiles = oa::data::mod_profile;
    auto& state = engine_settings_state();
    state.layered = true;
    state.latest = options_.mod_profile;
    state.played = std::make_shared<profiles::ModProfile>(
        options_.mod_profile ? *options_.mod_profile : profiles::ModProfile{}
    );
    state.plays_base_rules = !options_.mod_profile;
    state.refused.clear();
    // A mod folder without a profile keeps its overrides under its own id,
    // apart from the game's without a mod.
    if (options_.mod_profile)
        state.profile_id = options_.mod_profile->id;
    else if (!options_.layered_mod_folder.empty())
        state.profile_id = folder_overrides_id(options_.layered_mod_folder);
    else
        state.profile_id = std::string(profiles::base_game_id);
    state.profile_hacks = profiles::base_hack_states();
    // The plain 3.1c baseline, which a game without a mod lays its
    // overrides over; its sim hash is the one 3.1c's rules give.
    ProfileSource base{};
    const std::string base_text = profiles::base_game_profile_text();
    base.text.assign(base_text.begin(), base_text.end());
    base.name = std::string(profiles::base_game_id);
    base.options.accept_unimplemented_hacks = options_.accept_unimplemented_hacks;
    const auto base_result = profiles::resolve_profile(base.text, base.name, base.options);
    if (base_result.resolution)
        state.base_sim_hash = base_result.resolution->profile.sim_hash;
    if (!options_.mod_profile) {
        state.profile_source = std::move(base);
        return;
    }
    // A mod's profile is read again as the game folder's was, with the
    // settings it binds.
    state.profile_source = folder_profile_source(
        options_.game_folders.empty() ? std::vector<fs::path>{options_.game_dir}
                                      : options_.game_folders,
        // The layered mod folder, so --mod-dir and a folder chosen in
        // Settings are read again. An empty folder still uses the game
        // install, which is what a run without a mod does.
        ModChoice{
            options_.layered_mod_folder,
            options_.mod_file,
            options_.accept_unimplemented_hacks,
            &preference_values_
        }
    );
    if (!state.profile_source)
        return;
    const auto shipped = profiles::resolve_profile(
        state.profile_source->text, state.profile_source->name, state.profile_source->options
    );
    if (shipped.resolution)
        state.profile_hacks = profiles::hack_states(shipped.resolution->effective);
    else
        state.profile_source.reset();
}

void Runtime::apply_hack_overrides() {
    namespace profiles = oa::data::mod_profile;
    if (!engine_settings_ || !engine_settings_->layered)
        return;
    auto& state = *engine_settings_;
    const auto& settings = state.current;
    std::shared_ptr<const profiles::ModProfile> latest = options_.mod_profile;
    std::vector<std::string> refused;
    if (settings.developer_mode && !settings.hack_overrides.empty()) {
        if (!state.profile_source) {
            refused.emplace_back("the profile cannot be read again; no override applies");
        } else {
            auto options = state.profile_source->options;
            options.overrides = settings.hack_overrides;
            auto result = profiles::resolve_profile(
                state.profile_source->text, state.profile_source->name, options
            );
            // The profile's own warnings were reported as the game started;
            // those of the overrides are reported here.
            constexpr std::string_view left_out = "the override is left out";
            for (const auto& warning : result.warnings)
                if (warning.message.find(left_out) != std::string::npos)
                    refused.push_back(profiles::format_diagnostic(warning));
            for (const auto& error : result.errors)
                refused.push_back(profiles::format_diagnostic(error));
            if (result.resolution)
                latest = std::make_shared<const profiles::ModProfile>(
                    std::move(result.resolution->profile)
                );
        }
    }
    if (refused != state.refused) {
        for (const auto& line : refused)
            std::cerr << "open-annihilation: developer mode: " << line << '\n';
        state.refused = std::move(refused);
    }
    static const profiles::UiRules base_rules{};
    const auto& display_before = state.latest ? state.latest->ui : base_rules;
    const auto& display_after = latest ? latest->ui : base_rules;
    const bool display_changed = !(display_before == display_after);
    state.latest = std::move(latest);
    // The rules wait for the running match to end; without one they apply now.
    if (!match_)
        play_latest_profile();
    if (!display_changed)
        return;
    // The display rules apply at once, a running match's voices and
    // explosions and the player's view settings included.
    if (match_)
        match_->set_display_rules(view_rules::match_display_rules(ui_rules()));
    load_view_settings();
}

AccelerationFacts Runtime::acceleration_facts() const {
    AccelerationFacts facts{};
    const auto setting = engine_settings_ ? engine_settings_->current.hardware_acceleration
                                          : oa::ui::engine_settings::HardwareAcceleration::off;
    if (render_run_ && render_run_->host != nullptr) {
        // The facts the tier is decided from, the function test's result
        // and the presentation drawing now among them.
        const bool drawing = accelerated_presentation();
        facts = tier_acceleration_facts(
            render_run_->host->tier_inputs(),
            drawing ? accelerated_.rung : render_tier_rung(),
            full_presentation() ? render_policy::RenderTier::full
            : drawing           ? render_policy::RenderTier::accelerated
                                : render_policy::RenderTier::standard
        );
        facts.asked = hardware_acceleration_asked(options_, setting);
        facts.flag = options_.hardware_acceleration;
        facts.force_capable = options_.force_capable;
        render_run_->host->fill_record_facts(facts);
        const std::string_view renderer_name = render_run_->host->facts().renderer;
        facts.vertical_sync_resets_device =
            render_probe::vertical_sync_resets_device(renderer_name);
        facts.vertical_sync_refused = vertical_sync_refused_;
        facts.full_supersample_drawn = static_cast<uint8_t>(full_supersample());
        facts.full_supersample = std::max<uint8_t>(1, facts.full_supersample_drawn);
        if (match_) {
            const uint32_t extension = current_extension_state();
            facts.shared_game =
                facts.shared_game || (extension & extension_state::shared_match) != 0;
            facts.replay = facts.replay || (extension & extension_state::replay) != 0;
        }
        return facts;
    }
    facts.asked = hardware_acceleration_asked(options_, setting);
    facts.flag = options_.hardware_acceleration;
    facts.force_capable = options_.force_capable;
    const char* named_driver = SDL_GetHint(SDL_HINT_RENDER_DRIVER);
    const char* video_driver = SDL_GetCurrentVideoDriver();
    facts.environment_driver =
        (named_driver != nullptr && *named_driver != '\0') ||
        (video_driver != nullptr &&
         std::find(kWindowlessVideoDrivers.begin(), kWindowlessVideoDrivers.end(), video_driver) !=
             kWindowlessVideoDrivers.end());
    facts.physical_memory = engine_settings_ ? engine_settings_->physical_memory : 0;
    // A renderer the runtime made itself is not looked at, so whether it is
    // able stays unknown (renderer_capable empty); only SDL's software
    // renderer is known unable.
    const char* renderer = sdl_.renderer != nullptr ? SDL_GetRendererName(sdl_.renderer) : nullptr;
    const std::string_view renderer_name = renderer != nullptr ? renderer : "";
    facts.software_renderer = renderer_name == SDL_SOFTWARE_RENDERER;
    facts.vertical_sync_resets_device = render_probe::vertical_sync_resets_device(renderer_name);
    facts.vertical_sync_refused = vertical_sync_refused_;
    if (match_) {
        const uint32_t extension = current_extension_state();
        facts.shared_game = (extension & extension_state::shared_match) != 0;
        facts.replay = (extension & extension_state::replay) != 0;
    }
    return facts;
}

void Runtime::take_renderer_retry(const settings::Dialog& dialog) {
    auto& state = engine_settings_state();
    if (dialog.forget_renderer_failures == state.retries_taken)
        return;
    state.retries_taken = dialog.forget_renderer_failures;
    if (render_run_ && render_run_->host != nullptr) {
        // The host keeps what the dialog opened with for Cancel, not what an
        // earlier retry in it left.
        state.records_host = render_run_->host;
        std::ignore = state.records_host->clear_records();
    }
    forget_render_failures();
}

void Runtime::keep_renderer_records() {
    auto& state = engine_settings_state();
    if (state.records_host != nullptr)
        state.records_host->keep_cleared_records();
    state.records_host = nullptr;
}

void Runtime::restore_renderer_records() {
    auto& state = engine_settings_state();
    if (state.records_host != nullptr)
        state.records_host->restore_records();
    state.records_host = nullptr;
}

AccelerationReport Runtime::acceleration_report() const {
    return report_acceleration(acceleration_facts());
}

void Runtime::apply_vertical_sync() {
    if (sdl_.renderer == nullptr || !engine_settings_)
        return;
    const bool wanted =
        engine_settings_->current.vertical_sync && !acceleration_report().vertical_sync_unavailable;
    // SDL makes every renderer without it, so while the setting stays Off
    // the renderer is never asked.
    if (wanted == vertical_sync_in_effect_)
        return;
    if (!SDL_SetRenderVSync(sdl_.renderer, wanted ? 1 : 0)) {
        std::cerr << "open-annihilation: the renderer does not wait for the display: "
                  << SDL_GetError() << '\n';
        if (wanted)
            vertical_sync_refused_ = true;
    }
    int vsync = 0;
    vertical_sync_in_effect_ = SDL_GetRenderVSync(sdl_.renderer, &vsync) && vsync != 0;
}

float Runtime::display_refresh_rate() const {
    if (sdl_.window == nullptr)
        return 0.0F;
    const SDL_DisplayMode* mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(sdl_.window));
    return mode != nullptr ? mode->refresh_rate : 0.0F;
}

std::optional<std::string> Runtime::save_engine_settings(
    const settings::EngineSettings& opened, const settings::EngineSettings& chosen, bool restored
) {
    // A mod folder the start dropped is still stored: what is chosen now
    // replaces it, No Mod included.
    auto& state = engine_settings_state();
    settings::EngineSettings stored = opened;
    if (!state.dropped_mod_folder.empty()) {
        stored.mod_folder = std::move(state.dropped_mod_folder);
        state.dropped_mod_folder.clear();
    }
    settings::write_settings(
        preference_values_,
        stored,
        chosen,
        settings::default_settings(EngineSettingsState::inputs(*this)),
        restored,
        engine_settings_state().profile_id
    );
    // SwitchAlt keeps 3.1c's own key, written only when it changed.
    if (chosen.switch_alt != opened.switch_alt || restored)
        write_number(init::general_section, "SwitchAlt", chosen.switch_alt ? 1U : 0U);
    preferences_dirty_ = true;
    return EngineSettingsState::flush(*this);
}

std::optional<oa::platform::file_types::OpenerReport> show_system_openers(settings::Dialog& dialog);

std::string kept_opener_hint_for(
    oa::platform::file_types::TypeSet asked, oa::platform::file_types::TypeSet open
);

settings::Dialog& Runtime::open_engine_settings_dialog(settings::DialogKind kind) {
    EngineSettingsState::take_live_settings(*this);
    auto& state = engine_settings_state();
    if (kind == settings::DialogKind::mod_options) {
        const auto& ui = ui_rules();
        settings::EngineSettings current = state.current;
        current.mod_options = view_rules::dialog_options(view_settings_, ui);
        settings::EngineSettings defaults = current;
        const auto* profile = mod_profile();
        const oa::data::match_rules::OrdersConPatrolGuardOptions base_builders{};
        const auto& builders =
            profile != nullptr ? profile->rules.orders.con_patrol_guard_options : base_builders;
        defaults.mod_options =
            view_rules::dialog_options(view_rules::read_view_settings(ui, builders, {}), ui);
        auto& dialog = state.dialog.emplace();
        settings::open_mod_options_dialog(
            dialog, current, defaults, view_rules::dialog_option_locks(ui), kVersionText
        );
        return dialog;
    }
    state.opened_zoom_target = match_zoom_target_;
    state.opened_run_unit_limit = frontend_game().max_units_setting;
    auto& dialog = state.dialog.emplace();
    state.retries_taken = 0;
    state.records_host = nullptr;
    // The Game files section is the main menu's: where the platform brings
    // game files in, Manage… runs over the menu, never over a match.
    const bool game_files =
        screen_ == Screen::main_menu && game_files_import_offered(game_files_hooks());
    // The slider shows the count the setup has. Zero means the setup is not
    // loaded yet, so the dialog keeps the new-install default and OK writes
    // nothing until the player moves the slider.
    const int32_t shown_players = skirmish_settings_.slot_count;
    if (shown_players > 0 &&
        static_cast<std::size_t>(shown_players) <= skirmish_settings_.slots.size())
        state.current.skirmish_players = shown_players;
    settings::open_dialog(
        dialog,
        state.current,
        settings::default_settings(EngineSettingsState::inputs(*this)),
        engine_settings_locks(),
        kVersionText,
        state.last_page,
        acceleration_report().status,
        settings::highest_offered_unit_limit(limits_.units_per_player),
        state.profile_hacks,
        &system_language(),
        touch_controls_active(),
        game_files,
        pad_used()
    );
    // Screen size offers Desktop and the display's own sizes. In a window it
    // opens at the window's own size, shown as Custom where the display
    // offers no such size; in full screen, and without a window, at the
    // setting, listed where the display does not offer it.
    dialog.offered_screen_sizes.assign(1, settings::desktop_screen_size);
    for (const settings::ScreenSize size : EngineSettingsState::offered_screen_sizes(*this))
        dialog.offered_screen_sizes.push_back(size);
    dialog.window_screen_size.reset();
    dialog.custom_screen_size.reset();
    settings::ScreenSize shown = state.current.screen_size;
    if (sdl_.window != nullptr && (SDL_GetWindowFlags(sdl_.window) & SDL_WINDOW_FULLSCREEN) == 0)
        if (const auto now = EngineSettingsState::screen_size_now(*this);
            now != settings::desktop_screen_size) {
            shown = now;
            dialog.window_screen_size = now;
        }
    state.window_screen_size_shown = dialog.window_screen_size.has_value();
    if (std::find(dialog.offered_screen_sizes.begin(), dialog.offered_screen_sizes.end(), shown) ==
        dialog.offered_screen_sizes.end()) {
        const auto later = std::find_if(
            dialog.offered_screen_sizes.begin() + 1,
            dialog.offered_screen_sizes.end(),
            [shown](settings::ScreenSize size) {
                return size.width > shown.width ||
                       (size.width == shown.width && size.height > shown.height);
            }
        );
        dialog.offered_screen_sizes.insert(later, shown);
        if (dialog.window_screen_size)
            dialog.custom_screen_size = shown;
    }
    // Controller's Steam Input notice, and Maximum frame rate's line naming
    // a Steam Deck's screen rate.
    dialog.steam_input = pad_steam_input();
    dialog.steam_deck_panel_hz = state.steam_deck_panel_hz;
    fill_game_files_rows(dialog);
    // Downloads: the registries that use an install ID, none made here, and
    // the downloaded files. Opening settings starts the content service.
    {
        content::Downloads& downloads = content_downloads();
        give_downloads(
            dialog, preference_values_, content_service(), downloads, content_downloads_folder()
        );
    }
    lay_game_content(dialog, user_folder_);
    request_content_check(user_folder_);
    paint_updates(dialog, update_snapshot(), release_version(kVersionText));
    apply_update_download(dialog);
    note_updates_generation(update_snapshot());
    // Your files shows the player's own folder.
    dialog.user_folder = path_to_utf8(user_folder_);
    // The languages whose packs turn Unicode chat on.
    dialog.unicode_chat_languages = unicode_chat_language_tags();
    // Languages a catalogue offers that are not installed, and where a
    // download of one of them stands.
    std::ignore = refresh_settings_languages(dialog);
    // On the main menu Language links to the Library, which opens over the
    // dialog; never in a match.
    dialog.library_link = screen_ == Screen::main_menu;
    // Developer Mode opens as it was left: its open areas and hacks and its filter.
    if (state.last_developer_list) {
        dialog.developer.areas_open = state.last_developer_list->areas_open;
        dialog.developer.hacks_open = state.last_developer_list->hacks_open;
        dialog.developer.active_only = state.last_developer_list->active_only;
    }
    // Registered pages join the engine's dialog after it is open, so open
    // does not wipe them. A mod's options never lists them. The rows stay
    // the registry's caller.
    if (oa_layer_ != nullptr) {
        const SettingsRegistry& registry = oa_layer_->registered_settings();
        std::vector<settings::RegisteredPage> pages;
        pages.reserve(registry.page_count);
        for (uint32_t index = 0; index < registry.page_count; ++index) {
            const SettingsPageDesc& desc = registry.pages[index];
            settings::RegisteredPage page;
            if (desc.id != nullptr)
                page.id = desc.id;
            if (desc.title != nullptr)
                page.title = desc.title;
            if (desc.owner != nullptr)
                page.owner = desc.owner;
            page.rows = desc.rows;
            page.row_count = desc.row_count;
            page.applied = desc.applied;
            page.state = desc.state;
            pages.push_back(std::move(page));
        }
        settings::set_registered_pages(dialog, pages, preference_values_);
    }
    // On macOS the six switches show what the system reports now, not the
    // stored preference. A start does not change an opener; only a switch
    // that moves, at OK, does.
    if (const std::optional<oa::platform::file_types::OpenerReport> report =
            show_system_openers(dialog)) {
        if (state.file_type_asked && report->error.empty())
            state.file_type_kept_hint = kept_opener_hint_for(*state.file_type_asked, report->open);
        dialog.file_type_kept_hint = state.file_type_kept_hint;
    }
    // Weapon types and sides: the profile's limits, and, in a match, how
    // many of each the match loaded. No profile is the base game.
    settings::ModLimitsShown mod_shown;
    mod_shown.mod = mod_profile() != nullptr;
    mod_shown.weapon_types = limits_.weapon_types.types;
    mod_shown.sides = limits_.sides.count;
    if (match_ != nullptr) {
        mod_shown.loaded_weapon_types = match_weapon_types_;
        mod_shown.loaded_sides = side_table_.count;
    }
    dialog.mod_limits = std::move(mod_shown);
    return dialog;
}

/// Writes what a file-type change did.
///
/// @param result what the change did
void log_file_type_change(const oa::platform::file_types::Registration& result) {
    for (const auto& line : result.lines)
        std::cerr << "open-annihilation: file types: " << line << '\n';
    if (!result.error.empty())
        std::cerr << "open-annihilation: file types: " << result.error << '\n';
}

/// The sentence for types OK asked off that this copy still opens. Empty when
/// none stayed.
///
/// @param asked what OK asked
/// @param open what the system reports now
/// @return the hint; empty when nothing stayed
std::string kept_opener_hint_for(
    const oa::platform::file_types::TypeSet asked, const oa::platform::file_types::TypeSet open
) {
    const std::vector<std::string> stayed =
        oa::platform::file_types::extensions_still_opened(asked, open);
    std::vector<std::string_view> dotted;
    dotted.reserve(stayed.size());
    for (const std::string& extension : stayed)
        dotted.emplace_back(extension);
    return settings::kept_opener_hint(dotted);
}

/// Sets the six switches from what the system reports, where a switch is the
/// default opener. A question that fails leaves that switch as the dialog
/// already shows it and is logged. The hint under File Type Registration
/// names a type this copy still opens that OK asked off, and clears when the
/// read-back matches what was asked.
///
/// @param dialog the dialog just opened
/// @return what the system reported; nullopt where a switch is not the opener
std::optional<oa::platform::file_types::OpenerReport>
show_system_openers(settings::Dialog& dialog) {
    namespace file_types = oa::platform::file_types;
    if (!file_types::default_opener_is_a_setting() || !dialog.file_type_switches)
        return std::nullopt;
    const file_types::Handler application = file_types::running_application();
    if (application.bundle_id.empty())
        return std::nullopt;
    const file_types::OpenerReport report = file_types::reported_openers(
        file_types::system_handler_calls(),
        application.bundle_id,
        settings::file_type_set(dialog.chosen)
    );
    if (!report.error.empty())
        std::cerr << "open-annihilation: file types: " << report.error << '\n';
    settings::set_file_type_switches(dialog.opened, report.open);
    settings::set_file_type_switches(dialog.chosen, report.open);
    return report;
}

/// On macOS, a check, a snapshot and any other run that is not someone
/// playing must not change the opener a player chose in Finder.
///
/// @param options the run's command line
/// @return false when this run must leave every opener as it is
bool openers_follow_ok(const Options& options) {
    namespace file_types = oa::platform::file_types;
    if (!file_types::default_opener_is_a_setting())
        return true;
    if (options.headless_check || options.unattended || options.preferences_file)
        return false;
    if (const auto driver = oa::platform::environment_value("SDL_VIDEO_DRIVER");
        driver && (*driver == "dummy" || *driver == "offscreen"))
        return false;
    return true;
}

/// Registers the file types `chosen` leaves on and withdraws the rest, where
/// this system takes a registration at run time. On macOS, asks the system to
/// change only the switches that moved. A registration that skips or fails
/// withdraws nothing.
///
/// @param opened the settings the dialog opened with
/// @param chosen the settings OK is keeping
/// @return true when macOS was asked and this copy is an application
bool apply_chosen_file_types(
    const settings::EngineSettings& opened, const settings::EngineSettings& chosen
) {
    namespace file_types = oa::platform::file_types;
    if (file_types::default_opener_is_a_setting()) {
        const std::vector<file_types::OpenerChange> changes =
            settings::moved_file_type_switches(opened, chosen);
        if (changes.empty())
            return false;
        const file_types::Registration done = file_types::apply_opener_changes(
            file_types::system_handler_calls(), file_types::running_application(), changes
        );
        log_file_type_change(done);
        // A run that is not an application never asked the system, so it
        // does not claim the opener stayed.
        bool skipped = !done.supported;
        for (const std::string& line : done.lines)
            skipped = skipped || line.rfind("skipped:", 0) == 0;
        return !skipped;
    }
    const auto executable = file_types::running_executable();
    if (!executable)
        return false;
    const file_types::TypeSet enabled = settings::file_type_set(chosen);
    const auto done = file_types::register_file_types(*executable, window_icon_png(), {}, enabled);
    log_file_type_change(done);
    if (!done.supported || !done.error.empty())
        return false;
    for (const auto& line : done.lines)
        if (line.rfind("skipped:", 0) == 0)
            return false;
    log_file_type_change(file_types::withdraw_file_types(enabled.complement()));
    return false;
}

settings::Dialog* Runtime::engine_settings_dialog() {
    if (!engine_settings_ || !engine_settings_->dialog)
        return nullptr;
    return &*engine_settings_->dialog;
}

bool Runtime::refresh_game_content(settings::Dialog& dialog) {
    const bool updates_moved = refill_updates(update_snapshot(), dialog);
    const bool download_moved = apply_update_download(dialog);
    service_content_check();
    ContentCopyWatch& watch = content_copy_watch();
    if (watch.active) {
        oa::ui::first_run::CopyView view;
        static_cast<void>(setup_pull_copy(view));
        const bool cancelled_bytes =
            !view.running && !view.hashing && !view.bytes_landed && !view.checked;
        if (cancelled_bytes) {
            if (!view.verdict.empty())
                std::ignore = settings::set_folder_notice(dialog, view.verdict);
            dialog.game_content = watch.before;
        } else {
            paint_copy(dialog, view, watch.before);
        }
        if (!view.running && !view.hashing) {
            const bool resume = cancelled_bytes && rows_checking(watch.before);
            const fs::path folder = watch.folder;
            setup_stop_join();
            setup_release_located();
            if (watch.awake)
                SDL_EnableScreenSaver();
            watch = {};
            if (resume)
                request_content_check(folder);
        }
        // A match draws the dialog only when this says so, so the bar moves
        // on the frames the copy is running.
        return true;
    }
    ContentCheckJob& job = content_check_job();
    bool apply = false;
    bool running = false;
    uint32_t done = 0;
    uint32_t total = 0;
    ContentCheck result;
    {
        const std::lock_guard<std::mutex> lock(job.mutex);
        running = job.running || !job.exited.load();
        done = job.done;
        total = job.total;
        if (job.result_ready) {
            apply = true;
            result = std::move(job.result);
            job.result_ready = false;
            running = false;
        }
    }
    if (apply) {
        const std::vector<settings::GameContentRow> saved = dialog.game_content;
        paint_check_result(dialog, result);
        return updates_moved || download_moved || content_rows_differ(saved, dialog.game_content);
    }
    if (running) {
        paint_checking(dialog, done, total);
        return true;
    }
    return updates_moved || download_moved;
}

bool Runtime::take_engine_settings_action(settings::DialogAction action) {
    auto* dialog = engine_settings_dialog();
    if (dialog == nullptr)
        return false;
    auto& state = engine_settings_state();
    switch (action) {
    case settings::DialogAction::none:
    case settings::DialogAction::redraw:
        return false;
    case settings::DialogAction::open_folder: {
        // Your files: Saves, which holds each mod's saved games; where
        // screenshots go, Screenshots, which holds each mod's, while no Image
        // Output Directory is set; or the player's Mods folder. The dialog
        // stays open.
        fs::path folder;
        switch (dialog->folder_to_open) {
        case settings::FolderButton::saves:
            folder = user_folder_ / std::string(saves_folder_name);
            break;
        case settings::FolderButton::screenshots:
            folder = game_file_path(
                preferences_.image_output_directory + "\\screenshots",
                ui::frontend::SavePathUse::write
            );
            if (folder == oa::app::screenshots_folder(user_folder_, files_mod_id()))
                folder = folder.parent_path();
            break;
        case settings::FolderButton::mods:
            folder = user_folder_ / std::string(user_mods_folder_name);
            break;
        }
        const FolderOpening opening = open_player_folder(folder);
        std::ignore = settings::set_folder_notice(*dialog, opening.opened ? "" : opening.reason);
        return false;
    }
    case settings::DialogAction::changed:
        if (dialog->kind == settings::DialogKind::mod_options) {
            view_rules::apply_dialog_options(
                dialog->chosen.mod_options, ui_rules(), view_settings_
            );
            return false;
        }
        take_renderer_retry(*dialog);
        {
            // A Screen size moved to applies when OK is pressed; until then
            // it stays as it was. The check for content updates does too,
            // and so does which file types the game opens. An Interface size
            // applies as it is chosen, and the open dialog and every OA
            // screen are laid out at it. Cancel puts back the size the
            // dialog opened with.
            settings::EngineSettings live = dialog->chosen;
            live.screen_size = dialog->opened.screen_size;
            live.content_check = dialog->opened.content_check;
            live.update_check = dialog->opened.update_check;
            live.update_download = dialog->opened.update_download;
            apply_engine_settings(live);
            if (dialog->page == settings::Page::updates)
                paint_updates(*dialog, update_snapshot(), release_version(dialog->version));
            apply_update_download(*dialog);
            oa_layer().lay_out_for_size();
        }
        return false;
    case settings::DialogAction::accepted: {
        finish_content_workers();
        if (dialog->kind == settings::DialogKind::mod_options) {
            view_rules::apply_dialog_options(
                dialog->chosen.mod_options, ui_rules(), view_settings_
            );
            save_view_settings();
            state.dialog.reset();
            return true;
        }
        take_renderer_retry(*dialog);
        keep_renderer_records();
        // A Screen size moved to is stored (as Desktop for the desktop's own
        // in full screen) and applied at once, also where it is the setting
        // already, as a window sized by hand snaps back to it.
        settings::EngineSettings chosen = dialog->chosen;
        const bool screen_size_moved =
            chosen.screen_size != dialog->opened.screen_size ||
            (state.window_screen_size_shown && !dialog->window_screen_size);
        if (screen_size_moved)
            chosen.screen_size = EngineSettingsState::stored_screen_size(*this, chosen.screen_size);
        const bool check_changed = chosen.content_check != dialog->opened.content_check;
        const bool updates_never = chosen.update_check == settings::UpdateCheck::never;
        const settings::ContentCheck chosen_check = chosen.content_check;
        const bool file_types_changed =
            !settings::moved_file_type_switches(dialog->opened, chosen).empty();
        const std::vector<settings::InstallIdChoice> switches_opened = dialog->install_id_opened;
        const std::vector<settings::InstallIdChoice> switches_chosen = dialog->install_id_chosen;
        // The switches are stored before the settings, so one save writes both.
        store_install_switches(preference_values_, switches_opened, switches_chosen);
        apply_engine_settings(chosen);
        // The registration follows the settings OK kept, at once. Cancel
        // never reaches here, and a switch moved before OK does not either.
        if (file_types_changed && openers_follow_ok(options_) &&
            apply_chosen_file_types(dialog->opened, chosen)) {
            const oa::platform::file_types::TypeSet asked = settings::file_type_set(chosen);
            state.file_type_asked = asked;
            const oa::platform::file_types::Handler application =
                oa::platform::file_types::running_application();
            const oa::platform::file_types::OpenerReport report =
                oa::platform::file_types::reported_openers(
                    oa::platform::file_types::system_handler_calls(), application.bundle_id, asked
                );
            if (report.error.empty())
                state.file_type_kept_hint = kept_opener_hint_for(asked, report.open);
        }
        // Changed registered keys join the values write_settings keeps, and
        // their owners are told once the flush succeeds.
        const std::vector<settings::RegisteredWrite> registered =
            settings::registered_writes(*dialog);
        for (const settings::RegisteredWrite& write : registered)
            preference_values_[write.key] = write.value;
        // SHOW AGAIN is kept only when OK is pressed. The key is erased
        // before the settings are written, so the same flush records it.
        if (dialog->welcome_again)
            forget_first_run();
        // The same NumSkirmishPlayers the setup's key codes store. A slider
        // that did not move leaves a stored count, including one the stops
        // do not offer, where it was.
        if (chosen.skirmish_players != dialog->opened.skirmish_players)
            init::set_skirmish_player_count(skirmish_settings_, chosen.skirmish_players, *this);
        const auto failure = save_engine_settings(dialog->opened, chosen, dialog->restored);
        tell_install_switches(content_downloads(), switches_opened, switches_chosen);
        if (check_changed)
            content_service().set_check_for_updates(check_mode(chosen_check));
        if (updates_never)
            cancel_update_check();
        state.last_page = dialog->page;
        state.last_developer_list = dialog->developer;
        state.dialog.reset();
        if (screen_size_moved)
            apply_screen_size(chosen.screen_size);
        if (failure) {
            // A mod that was not stored would not be the one the restart
            // plays: the game stays as it is and says so.
            EngineSettingsState::report_failed_save(*this, *failure);
            return true;
        }
        for (const settings::RegisteredWrite& write : registered) {
            if (write.applied != nullptr)
                write.applied(write.state, write.key.c_str(), write.value.c_str());
        }
        return true;
    }
    case settings::DialogAction::cancelled: {
        finish_content_workers();
        if (dialog->kind == settings::DialogKind::mod_options) {
            view_rules::apply_dialog_options(
                dialog->opened.mod_options, ui_rules(), view_settings_
            );
            state.dialog.reset();
            return true;
        }
        const bool zoom_changed = match_ && match_zoom_target_ != state.opened_zoom_target;
        restore_renderer_records();
        apply_engine_settings(dialog->opened);
        // The battlefield's zoom and the next game's unit limit go back to
        // what they were, a loaded game's limit included.
        if (zoom_changed)
            EngineSettingsState::ease_zoom_about_centre(*this, state.opened_zoom_target);
        frontend_game().max_units_setting = state.opened_run_unit_limit;
        state.last_page = dialog->page;
        state.last_developer_list = dialog->developer;
        state.dialog.reset();
        return true;
    }
    case settings::DialogAction::manage_game_files:
        // The dialog stays open under the Game files screen.
        open_game_files_manage();
        return false;
    case settings::DialogAction::show_gallery:
        // The gallery covers the window over the dialog, which stays open
        // under it; Escape and its CLOSE come back to Developer.
        oa_layer().open_gallery();
        return false;
    case settings::DialogAction::check_content:
        // A copy is already reading the files. The check waits for it.
        if (content_copy_watch().active)
            return false;
        paint_checking(*dialog, 0, 0);
        request_content_check(user_folder_);
        return false;
    case settings::DialogAction::copy_content_again:
        start_game_content_copy(*dialog, sdl_.window, user_folder_);
        return false;
    case settings::DialogAction::cancel_content_copy:
        // Stop between chunks. The dialog keeps drawing, and the bytes already
        // in the managed folders stay.
        if (content_copy_watch().active)
            setup_request_cancel();
        return false;
    case settings::DialogAction::open_content_folder: {
        const fs::path folder = user_folder_ / std::string(managed_games_folder);
        const FolderOpening opening = open_player_folder(folder);
        std::ignore = settings::set_folder_notice(*dialog, opening.opened ? "" : opening.reason);
        return false;
    }
    case settings::DialogAction::reset_install_id: {
        // A new ID is stored at once. Cancel does not put the old one back.
        // Nothing is made when the generator cannot be read, and the dialog
        // stays open either way. The ID is not written to the log.
        const std::string registry = dialog->install_id_registry;
        if (!content::reset_install_id(preference_values_, registry))
            return false;
        preferences_dirty_ = true;
        flush_preferences();
        const content::InstallId stored = content::read_install_id(preference_values_, registry);
        content::Downloads& downloads = content_downloads();
        if (stored.state == content::InstallIdState::on)
            downloads.set_install_id(registry, stored.text);
        give_downloads(
            *dialog, preference_values_, content_service(), downloads, content_downloads_folder()
        );
        return false;
    }
    case settings::DialogAction::empty_downloads:
        // Files a download is using stay. The dialog stays open, and Cancel
        // does not put the removed files back.
        static_cast<void>(empty_content_downloads());
        give_downloads(
            *dialog,
            preference_values_,
            content_service(),
            content_downloads(),
            content_downloads_folder()
        );
        return false;
    case settings::DialogAction::add_registry:
    case settings::DialogAction::add_mirror:
        // The Library's Registries tab asks for these itself.
        return false;
    case settings::DialogAction::open_library:
        // MORE IN THE LIBRARY…: the Library opens over the dialog, which
        // stays open under it; where the Library is under the dialog
        // already, the dialog closes back to it.
        open_library(library_tab_of(dialog->library_tab));
        return false;
    case settings::DialogAction::check_updates: {
        // Never offers no button. A check the dialog shows, but has not
        // stored yet, is lent to the preference for this one call.
        if (dialog->chosen.update_check == settings::UpdateCheck::never)
            return false;
        const std::string key{settings::key::update_check};
        const auto found = preference_values_.find(key);
        const bool saved_never = found != preference_values_.end() && found->second == "never";
        std::string saved_word;
        if (saved_never)
            saved_word = found->second;
        if (saved_never)
            preference_values_[key] = std::string(update_mode_word(dialog->chosen.update_check));
        start_update_check(true);
        if (saved_never)
            preference_values_[key] = std::move(saved_word);
        return false;
    }
    case settings::DialogAction::open_release:
        if (!start_update_download())
            open_release_page();
        return false;
    case settings::DialogAction::restart_update:
        quit_requested_ = true;
        exit_status_ = 0;
        return false;
    case settings::DialogAction::retry_update:
        retry_update_download();
        return false;
    case settings::DialogAction::download_language: {
        // The dialog stays open. The language applies when the pack arrives.
        std::string why;
        const bool queued = download_language(dialog->download_request, why);
        for (settings::AvailableLanguage& pack : dialog->available_languages) {
            if (pack.tag != dialog->download_request)
                continue;
            if (queued) {
                pack.state = settings::AvailableLanguage::State::waiting;
                pack.percent = 0;
                pack.failure.clear();
            } else {
                pack.state = settings::AvailableLanguage::State::failed;
                pack.failure = std::move(why);
            }
            break;
        }
        return false;
    }
    }
    return false;
}

const settings::DialogFonts* Runtime::engine_settings_fonts() {
    auto& state = engine_settings_state();
    if (!state.fonts && !state.fonts_missing) {
        try {
            state.fonts = settings::load_dialog_fonts(assets_);
        } catch (const std::exception& error) {
            state.fonts_missing = true;
            std::cerr << "open-annihilation: the settings' fonts are missing: " << error.what()
                      << '\n';
        }
    }
    return state.fonts ? &*state.fonts : nullptr;
}

oa::ui::frontend_renderer::RgbaPicture Runtime::engine_settings_icon() {
    auto& state = engine_settings_state();
    if (!state.icon && !state.icon_missing) {
        WindowIcon decoded;
        std::string error;
        if (decode_window_icon(window_icon_png(), decoded, error))
            state.icon = visible_part(decoded);
        if (!state.icon || state.icon->pixels.empty()) {
            state.icon.reset();
            state.icon_missing = true;
            std::cerr << "open-annihilation: the app icon is missing"
                      << (error.empty() ? std::string{} : ": " + error) << '\n';
        } else if (!state.maker_mark_set) {
            const WindowIcon& full = *state.icon;
            if (full.width <= maker_mark_prepared_side && full.height <= maker_mark_prepared_side)
                state.maker_mark = full;
            else
                state.maker_mark = area_scaled(full, maker_mark_prepared_side);
            kit::OpenAnnihilationMark mark;
            mark.full = {full.width, full.height, full.pixels};
            if (state.maker_mark && !state.maker_mark->pixels.empty())
                mark.prepared = {
                    state.maker_mark->width, state.maker_mark->height, state.maker_mark->pixels
                };
            kit::set_open_annihilation_mark(mark);
            state.maker_mark_set = true;
        }
    }
    if (!state.icon)
        return {};
    return {state.icon->width, state.icon->height, state.icon->pixels};
}

void Runtime::release_unsaved_unit_limit() {
    if (!unsaved_unit_limit_playing_)
        return;
    unsaved_unit_limit_playing_ = false;
    if (unsaved_unit_limit_pending_)
        return;
    frontend_game().max_units_setting = engine_settings_state().current.unit_limit;
}

void set_unit_limit(Runtime& runtime, int32_t units_per_player, SettingScope scope) {
    const auto& units = runtime.limits_.units_per_player;
    const int64_t lowest = units.minimum;
    const int64_t highest = settings::highest_offered_unit_limit(units);
    const int64_t clamped = std::clamp(static_cast<int64_t>(units_per_player), lowest, highest);
    const auto kept = static_cast<uint16_t>(clamped);
    switch (scope) {
    case SettingScope::next_game:
        // The next new game plays at it, and that match's teardown puts the
        // player's setting back.
        runtime.frontend_game().max_units_setting = kept;
        runtime.unsaved_unit_limit_pending_ = true;
        return;
    case SettingScope::immediate:
        runtime.frontend_game().max_units_setting = kept;
        runtime.unsaved_unit_limit_pending_ = false;
        runtime.engine_settings_state().current.unit_limit = kept;
        break;
    case SettingScope::next_restart:
        // The setting, as Settings shows it, and the preferences take it; the
        // run's limit stays as it is until the game next starts.
        runtime.engine_settings_state().current.unit_limit = kept;
        break;
    }
    runtime.preference_values_[std::string(settings::key::unit_limit)] = std::to_string(kept);
    runtime.preferences_dirty_ = true;
    if (const auto failure = Runtime::EngineSettingsState::flush(runtime))
        Runtime::EngineSettingsState::report_failed_save(runtime, *failure);
}

namespace {

/// Reports whether a mod folder is remembered.
///
/// @param values the preferences
/// @return true when the mod key names a folder
[[nodiscard]] bool mod_is_remembered(const oa::platform::preferences::Values& values) {
    const auto found = values.find(std::string(settings::key::mod_directory));
    return found != values.end() && !found->second.empty();
}

/// Views of ids owned by `ids`.
///
/// @param ids the ids
/// @return views of them, in the same order
[[nodiscard]] std::vector<std::string_view> id_views(const std::vector<std::string>& ids) {
    std::vector<std::string_view> views;
    views.reserve(ids.size());
    for (const std::string& id : ids)
        views.emplace_back(id);
    return views;
}

} // namespace

oa::data::content_bundles::ContentChoice Runtime::content_choice() const {
    const std::vector<std::string> copied = copied_content_bundles(options_.game_dir);
    const std::vector<std::string_view> copied_ids = id_views(copied);
    return content_choice_from_preferences(preference_values_, copied_ids, options_.content);
}

bool Runtime::managed_content_mounted() const noexcept {
    return options_.managed_content;
}

Runtime::PresenceAnnouncement Runtime::presence_announcement() const {
    PresenceAnnouncement played;
    played.language = std::string(shown_language().english_name);
    if (!managed_content_mounted())
        return played;
    const oa::data::content_bundles::ContentChoice choice = content_choice();
    if (!choice.error.empty() || choice.base_game.empty())
        return played;
    played.announce_bundles = true;
    played.base_game = choice.base_game;
    for (const std::string& expansion_id : choice.expansions) {
        if (expansion_id != oa::netgame::presence_uncompared_expansion)
            played.expansions.push_back(expansion_id);
    }
    const ContentCheck& known = content_check();
    for (const ContentBundleCheck& bundle : known.bundles) {
        if (bundle.files_differ == 0 || bundle.id == oa::netgame::presence_uncompared_expansion)
            continue;
        if (bundle.id == played.base_game) {
            played.differs_from_known = true;
            break;
        }
        for (const std::string& expansion_id : played.expansions) {
            if (expansion_id == bundle.id) {
                played.differs_from_known = true;
                break;
            }
        }
        if (played.differs_from_known)
            break;
    }
    return played;
}

oa::data::content_bundles::ContentChangeResult Runtime::turn_content_expansion(
    std::string_view bundle_id, bool on, std::span<const std::string_view> required
) {
    namespace bundles = oa::data::content_bundles;
    const std::vector<std::string> copied = copied_content_bundles(options_.game_dir);
    const std::vector<std::string_view> views = id_views(copied);
    const bundles::ContentChoice now =
        content_choice_from_preferences(preference_values_, views, options_.content);
    const bundles::ContentChangeResult result = bundles::turn_content_expansion(
        now, bundle_id, on, views, match_running(), mod_is_remembered(preference_values_), required
    );
    if (!store_content_change(preference_values_, result))
        return result;
    if (result.mod_cleared != bundles::ModClearReason::none && engine_settings_) {
        engine_settings_->current.mod_folder.clear();
        engine_settings_->dropped_mod_folder.clear();
    }
    preferences_dirty_ = true;
    if (const auto failure = EngineSettingsState::flush(*this))
        EngineSettingsState::report_failed_save(*this, *failure);
    return result;
}

oa::data::content_bundles::ContentChangeResult
Runtime::choose_content_base_game(std::string_view bundle_id) {
    namespace bundles = oa::data::content_bundles;
    const std::vector<std::string> copied = copied_content_bundles(options_.game_dir);
    const std::vector<std::string_view> views = id_views(copied);
    const bundles::ContentChoice now =
        content_choice_from_preferences(preference_values_, views, options_.content);
    const bundles::ContentChangeResult result = bundles::choose_content_base_game(
        now, bundle_id, views, match_running(), mod_is_remembered(preference_values_)
    );
    if (!store_content_change(preference_values_, result))
        return result;
    if (result.mod_cleared != bundles::ModClearReason::none && engine_settings_) {
        engine_settings_->current.mod_folder.clear();
        engine_settings_->dropped_mod_folder.clear();
    }
    preferences_dirty_ = true;
    if (const auto failure = EngineSettingsState::flush(*this))
        EngineSettingsState::report_failed_save(*this, *failure);
    return result;
}

} // namespace oa::app
