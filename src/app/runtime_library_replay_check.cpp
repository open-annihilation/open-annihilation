// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-library-replay PATH: one replay already in the player's Recordings
// folder, walked through the Library as a player would, whatever its length.
// The Library lists it, its own index pass gives it a timeline, Watch plays it
// to the middle, Watch from jumps back there by the replay's own jump and
// plays on to the end with no error, and Export writes it as a .oademo and as
// a .tad. Each step prints a `library replay:` line, and the run's peak
// memory is printed after the replay has played. tools/check_native_library_replays.py
// places the replay and reads the lines.

#include "library_host.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"

#include "oa/app/automation_host.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/data/replays/long_replay.hpp"
#include "oa/data/replays/replays.hpp"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/oademo/timeline_index.hpp"
#include "oa/platform/files.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/library/export.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/replays.hpp"

#ifndef _WIN32
#include <sys/resource.h>
#endif

#include <SDL3/SDL.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace oa::app {

namespace {

namespace fs = std::filesystem;
namespace library = oa::ui::library;
namespace oademo = oa::formats::oademo;
namespace replay_data = oa::data::replays;

/// The longest any one step may take: a replay of hours plays for many minutes.
constexpr std::chrono::minutes kMostStepTime{90};

/// Ticks granted on each frame while the replay plays on the fixed clock.
constexpr uint32_t kTicksAFrame = 64;

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("library replay check: " + std::string(what));
}

/// Returns the process's peak memory, in bytes; 0 where the system does not say.
///
/// @return the bytes
uint64_t peak_memory_bytes() {
#ifdef _WIN32
    return 0;
#else
    rusage usage{};
    if (getrusage(RUSAGE_SELF, &usage) != 0)
        return 0;
#ifdef __APPLE__
    return static_cast<uint64_t>(usage.ru_maxrss);
#else
    return static_cast<uint64_t>(usage.ru_maxrss) * 1024U;
#endif
#endif
}

/// Returns the regular files of a folder whose names end with `ending`.
///
/// @param folder the folder
/// @param ending the ending, such as `.tad`
/// @return the files
std::vector<fs::path> files_ending(const fs::path& folder, std::string_view ending) {
    std::vector<fs::path> found;
    std::error_code failed;
    for (fs::directory_iterator entry(folder, failed), end; !failed && entry != end;
         entry.increment(failed)) {
        const std::string name = platform::path_to_utf8(entry->path().filename());
        if (name.size() >= ending.size() &&
            name.compare(name.size() - ending.size(), ending.size(), ending) == 0)
            found.push_back(entry->path());
    }
    return found;
}

} // namespace

void Runtime::check_library_replay() {
    require(sdl_.window != nullptr, "the check needs the SDL presenter");
    require(!user_folder_.empty(), "the run has no folder of the player's own");
    const std::string path = options_.check_library_replay;
    const fs::path recordings = user_folder_ / std::string(recordings_folder_name);
    std::optional<fs::path> file;
    {
        fs::path below = recordings;
        std::string_view rest = path;
        while (!rest.empty()) {
            const std::size_t slash = rest.find('/');
            below /= platform::path_from_utf8(std::string(rest.substr(0, slash)));
            if (slash == std::string_view::npos)
                break;
            rest = rest.substr(slash + 1);
        }
        file = below;
    }
    std::error_code sized;
    const uint64_t file_bytes = fs::file_size(*file, sized);
    require(!sized, path + " is not in the player's Recordings folder");
    const fs::path exported = user_folder_ / "Exported replays";

    struct CleanUp {
        fs::path exported;

        ~CleanUp() {
            std::error_code removed;
            fs::remove_all(exported, removed);
        }
    } clean_up{exported};

    {
        std::error_code removed;
        fs::remove_all(exported, removed);
        fs::create_directories(exported, removed);
    }
    // The check and the pass it starts ask for no update and no catalogue.
    preference_values_["open-annihilation.update-check"] = "never";
    preference_values_["open-annihilation.content-updates"] = "never";
    preferences_dirty_ = true;
    flush_preferences();

    bool running = true;
    LibraryHost& host = library_host();
    const auto frames = [&](int count) {
        for (int frame = 0; frame < count; ++frame) {
            run_frame(running);
            require(running, "a frame ended the run");
        }
    };
    const auto deadline = [] { return std::chrono::steady_clock::now() + kMostStepTime; };
    const auto seconds_since = [](std::chrono::steady_clock::time_point started) {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    const auto top_is = [this](std::string_view name) {
        const LayerScreen* top = oa_layer().top();
        return top != nullptr && top->name() == name;
    };
    const auto row = [&]() { return library::find_replay(host.model, path); };
    // Clicks a control the Library shows, as a player does.
    const auto click_named = [&](std::string_view name) {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        const auto shown = std::find_if(controls.begin(), controls.end(), [&](const auto& one) {
            return one.name == name && one.visible && one.window_width > 0 && one.window_height > 0;
        });
        require(shown != controls.end(), std::string(name) + " is not shown");
        require(shown->enabled, std::string(name) + " is not enabled");
        const oa::ui::display_layout::Point pixel{
            shown->window_x + shown->window_width / 2, shown->window_y + shown->window_height / 2
        };
        for (const SDL_EventType type :
             {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event = window_pointer_event(
                sdl_.window,
                sdl_.renderer,
                type,
                pixel,
                type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT
            );
            bool going = true;
            dispatch_event(event, going);
            require(going, "a click ended the run");
        }
        frames(1);
    };
    const auto back_to_library = [&] {
        leave_match();
        load(Screen::main_menu);
        for (int frame = 0; frame < 600 && !top_is(library_screen_name); ++frame)
            frames(1);
        frames(2);
        require(top_is(library_screen_name), "leaving the replay did not open the Library again");
    };

    load(Screen::main_menu);
    require(screen_ == Screen::main_menu, "the main menu did not open");
    require(
        SDL_SetWindowSize(sdl_.window, 1280, 720) && SDL_SyncWindow(sdl_.window),
        "the window did not take 1280x720"
    );
    apply_output_mode();
    frames(4);

    // The list: the folder is read, and the replay is listed with what it says.
    open_library(library::Tab::replays);
    for (const auto until = deadline();
         host.replays_reading() && std::chrono::steady_clock::now() < until;)
        frames(1);
    frames(2);
    require(top_is(library_screen_name), "the Library did not open");
    require(!host.replays_reading(), "the Recordings folder was not read");
    require(row() != nullptr, path + " is not listed");
    {
        const replay_data::Replay& listed = row()->replay;
        require(!replay_data::damaged(listed), path + " is listed damaged: " + listed.damage);
        require(listed.size == file_bytes, path + " is listed at another size");
        std::cout << "library replay: listed " << path << ": " << listed.size << " bytes, map "
                  << listed.map << ", " << listed.players.size() << " players, " << listed.length_ms
                  << " ms, recording " << listed.sha256 << '\n';
        for (const replay_data::ContentGap& gap : row()->gaps)
            std::cout << "library replay: needs " << gap.id << ' ' << gap.name << '\n';
        require(row()->gaps.empty(), path + " still needs content to be watched");
    }
    host.model.replays.selected = path;
    ++host.model_revision;
    frames(2);

    // The timeline: the Library's own index pass reads the replay to its end.
    const auto pass_started = std::chrono::steady_clock::now();
    for (const auto until = deadline();
         std::chrono::steady_clock::now() < until &&
         !(row() != nullptr && row()->replay.timeline.has_value() &&
           !row()->replay.timeline->empty()) &&
         !(row() != nullptr && row()->replay.read_stamp.has_value());) {
        frames(1);
    }
    require(
        row() != nullptr && row()->replay.timeline.has_value() && !row()->replay.timeline->empty(),
        "the index pass stored no timeline for " + path
    );
    const std::optional<oademo::TimelineIndex> timeline =
        oademo::parse_timeline_index(*row()->replay.timeline);
    require(timeline.has_value(), "the stored timeline does not read");
    std::cout << "library replay: timeline of " << timeline->length_ticks << " ticks, ended "
              << oademo::index_end_name(timeline->ended) << ", " << timeline->markers.size()
              << " markers, " << timeline->events.size() << " battle events, clean "
              << (timeline->verdict.clean ? "yes" : "no") << ", after "
              << static_cast<int64_t>(seconds_since(pass_started)) << " s\n";
    require(timeline->verdict.clean, "the index pass did not replay " + path + " cleanly");
    require(row()->length_ticks == timeline->length_ticks, "the card shows another length");
    const uint32_t middle = timeline->length_ticks / 2;

    // Watch to the middle, and back to the Library.
    require(host.start_replay_watch(*this, path, std::nullopt), "Watch did not open " + path);
    for (int frame = 0; frame < 600 && screen_ != Screen::match; ++frame)
        frames(1);
    require(screen_ == Screen::match && match_ != nullptr, "Watch did not start the replay");
    const uint32_t first_tick = match_->state().game.tick;
    for (const auto until = deadline(); std::chrono::steady_clock::now() < until &&
                                        match_ != nullptr && match_->state().game.tick < middle;) {
        static_cast<void>(
            run_granted_ticks(std::min(kTicksAFrame, middle - match_->state().game.tick))
        );
        frames(1);
    }
    require(
        match_ != nullptr && match_->state().game.tick >= middle, "Watch did not reach the middle"
    );
    std::cout << "library replay: Watch played ticks " << first_tick << " to "
              << match_->state().game.tick << '\n';
    back_to_library();
    require(
        row() != nullptr && row()->replay.watched_tick.has_value(),
        "leaving the replay did not remember where it was"
    );
    const uint32_t remembered = *row()->replay.watched_tick;

    // Watch from: the replay's own jump to where it was left, then on to the end.
    const auto jump_started = std::chrono::steady_clock::now();
    require(host.start_replay_watch(*this, path, remembered), "Watch from did not open " + path);
    for (int frame = 0; frame < 600 && screen_ != Screen::match; ++frame)
        frames(1);
    require(screen_ == Screen::match && match_ != nullptr, "Watch from did not start the replay");
    bool saw_skipping = false;
    bool arrived = false;
    for (const auto until = deadline();
         std::chrono::steady_clock::now() < until && match_ != nullptr;) {
        frames(1);
        ReplayPlaybackView view{};
        const bool have = call_hook_or_report<&Extension::replay_state>(
            extension_, hook_error_report(), *this, &view
        );
        if (have && view.skipping)
            saw_skipping = true;
        if (have && !view.skipping && match_ != nullptr &&
            match_->state().game.tick == remembered && (saw_skipping || remembered == 0)) {
            arrived = true;
            break;
        }
    }
    require(arrived, "Watch from did not jump to the tick it was left at");
    std::cout << "library replay: Watch from jumped to tick " << match_->state().game.tick << " in "
              << static_cast<int64_t>(seconds_since(jump_started)) << " s\n";
    for (const auto until = deadline(); std::chrono::steady_clock::now() < until &&
                                        match_ != nullptr && !host.watched_replay_finished();) {
        static_cast<void>(run_granted_ticks(kTicksAFrame));
        frames(1);
    }
    require(match_ != nullptr && host.watched_replay_finished(), "the replay did not finish");
    const std::optional<RecordingStatus> status = host.watched_replay_status();
    require(status.has_value(), "the replay's status cannot be read");
    std::cout << "library replay: played to tick " << status->tick << ", finished "
              << (status->finished ? "yes" : "no") << ", errors " << status->errors << ", clean "
              << (status->clean ? "yes" : "no") << '\n';
    std::cout << "library replay: peak memory " << peak_memory_bytes() << " bytes\n";
    require(status->errors == 0, "the replay played with errors");
    back_to_library();

    // Export: measured, then written as a .oademo and as a .tad.
    const auto export_as = [&](library::ExportFormat format, std::string_view ending) {
        host.open_replay_export(*this, path);
        require(host.model.export_dialog.open, "Export did not open for " + path);
        host.model.export_dialog.folder = platform::path_to_utf8(exported);
        host.model.export_dialog.folder_is_downloads = false;
        host.model.export_dialog.folder_label = "Exported replays";
        ++host.model_revision;
        const auto measuring = std::chrono::steady_clock::now();
        for (const auto until = deadline(); std::chrono::steady_clock::now() < until &&
                                            (host.model.export_dialog.files.empty() ||
                                             !host.model.export_dialog.files.front().ready) &&
                                            !(!host.model.export_dialog.files.empty() &&
                                              host.model.export_dialog.files.front().unreadable);)
            frames(1);
        require(
            !host.model.export_dialog.files.empty() && host.model.export_dialog.files.front().ready,
            "the export of " + path + " was not measured"
        );
        const library::ExportFile measured = host.model.export_dialog.files.front();
        const double measure_seconds = seconds_since(measuring);
        host.model.export_dialog.format = format;
        host.model.export_dialog.keep_oa_sectors = true;
        ++host.model_revision;
        frames(1);
        const auto writing = std::chrono::steady_clock::now();
        click_named("oa.library.export.export");
        for (const auto until = deadline();
             std::chrono::steady_clock::now() < until && host.model.export_dialog.open &&
             !host.model.export_dialog.done && host.model.export_dialog.problem.empty();)
            frames(1);
        require(
            host.model.export_dialog.done,
            host.model.export_dialog.problem.empty() ? "the export did not finish"
                                                     : host.model.export_dialog.problem
        );
        const std::vector<fs::path> written = files_ending(exported, ending);
        require(written.size() == 1, "the export did not write one " + std::string(ending));
        std::error_code failed;
        const uint64_t bytes = fs::file_size(written.front(), failed);
        const uint64_t expected =
            format == library::ExportFormat::oademo ? measured.oademo_bytes : measured.tad_bytes;
        std::cout << "library replay: exported " << ending << ' ' << bytes << " bytes (measured "
                  << expected << " in " << static_cast<int64_t>(measure_seconds)
                  << " s, written in " << static_cast<int64_t>(seconds_since(writing)) << " s)\n";
        require(bytes == expected, "the exported file is not the size measured");
        require(files_ending(exported, ".part").empty(), "a partial export was left");
        library::close_export(host.model);
        ++host.model_revision;
        frames(2);
        return written.front();
    };
    const fs::path as_oademo = export_as(library::ExportFormat::oademo, ".oademo");
    const oademo::DescriptionResult described = replay_data::read_description_file(as_oademo);
    require(described.ok(), "the exported .oademo is not described");
    std::cout << "library replay: the exported .oademo holds recording "
              << described.meta->recording.sha256 << '\n';
    require(
        described.meta->recording.sha256 == row()->replay.sha256,
        "the exported .oademo holds another recording"
    );
    {
        std::error_code removed;
        fs::remove(as_oademo, removed);
    }
    const fs::path as_tad = export_as(library::ExportFormat::tad, ".tad");
    {
        std::ifstream in(as_tad, std::ios::binary);
        std::string start(10, '\0');
        in.read(start.data(), static_cast<std::streamsize>(start.size()));
        require(
            start.compare(2, 7, "TA Demo") == 0, "the exported .tad is not a TA Demo recording"
        );
    }
    close_library();
    frames(2);
    std::cout << "library replay: done\n";
}

} // namespace oa::app
