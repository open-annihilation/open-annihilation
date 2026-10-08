// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The --check-game-files check (game_files_check.hpp): platform and game
// files hooks scripted over a work folder, each route of the Game files
// screen driven step by step through taps and keys, pictures of every state
// for people (the window's own frame, and the same state painted at a tablet's
// and a phone's size), and one verdict line.
#include "game_files_check.hpp"

#include "game_files_paint.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/game_files_hooks.hpp"
#include "oa/app/platform_hooks.hpp"
#include "oa/base/threads.hpp"
#include "oa/platform/preferences.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <iostream>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace oa::app {
namespace {

namespace view = oa::ui::game_files;
using Clock = std::chrono::steady_clock;

/// The work folder's game folder parent and the game folder's name in it.
constexpr std::string_view documents_name = "Documents";
/// The folder the copy-yourself route copies the files into, under another name.
constexpr std::string_view misnamed_folder = "TA Commander Pack";
/// The source folders the driver prepares, when --game-files-source names none.
constexpr std::string_view default_source = "source";
/// How long a step of a route may take before the check fails, in seconds.
constexpr double step_seconds = 180.0;
/// The passes a route lets go by after a tap whose result shows nothing new.
constexpr int settle_passes = 4;
/// How far one notch of the wheel scrolls the screen's rows, in points (as the screen takes it).
constexpr float wheel_notch_points = 40.0F;
/// A finger's drag over the rows: how many moves, and how far each goes, in points.
constexpr int drag_moves = 6;
constexpr float drag_step_points = 12.0F;
/// How long the throttled copy sleeps between looks at its allowance.
constexpr uint32_t throttle_nap = 10;
/// The review sizes, in points, and their density: an 11-inch tablet and a current phone in
/// landscape, the phone with the parts of its screen kept clear.
constexpr int tablet_width_points = 1194;
constexpr int tablet_height_points = 834;
constexpr int phone_width_points = 852;
constexpr int phone_height_points = 393;
constexpr int phone_side_inset_points = 59;
constexpr int phone_bottom_inset_points = 21;
constexpr float review_px_per_point = 2.0F;
/// The files left out of the copy that the driver puts in the folder source.
constexpr std::array<std::string_view, 5> left_out_fixtures{
    "setup.exe", "manual.pdf", "Thumbs.db", ".DS_Store", "unins000.dat"
};
/// The files the subset holds, which the game folder holds after the copy.
constexpr std::array<std::string_view, 3> subset_archives{
    "totala1.hpi", "rev31.gp3", "tactics1.hpi"
};

/// One step of a route: when it is ready, and what it does then.
struct RouteStep {
    std::string what{};                            ///< what it waits for, for the verdict
    std::function<bool(GameFilesScreen&)> ready{}; ///< the state it waits for
    std::function<void(GameFilesScreen&)> act{};   ///< what it does then
    /// The control the step taps that must be in view first, when it lies among rows that
    /// scroll; null or nothing: none.
    std::function<std::optional<view::Control>(GameFilesScreen&)> shows{};
};

/// What the check keeps for the whole run: the options, the work folder, what the scripted
/// hooks saw, and where the route is.
struct CheckRun {
    Options options{};      ///< the parsed command line
    std::string variant{};  ///< the variant's name in the verdict and the pictures
    fs::path work{};        ///< the working directory
    fs::path documents{};   ///< <work>/Documents, the game folder's parent
    fs::path game_folder{}; ///< <work>/Documents/Total Annihilation
    fs::path source{};      ///< what the picker answers
    base::threads::Mutex mutex{};     ///< guards what the worker thread's hooks write
    std::vector<std::pair<std::string, bool>> backed_up{}; ///< set_backed_up's calls
    uint32_t keep_running_on{};                            ///< keep_running(true) calls
    uint32_t keep_running_off{};                           ///< keep_running(false) calls
    void (*expiring)(void*){}; ///< what ends the copy's time away, while it has some
    void* expiring_userdata{}; ///< and its userdata
    uint32_t released{};       ///< release_source calls
    uint64_t copied_bytes{};   ///< bytes the scripted copy finished
    std::optional<Clock::time_point> copy_started{}; ///< when the scripted copy began
    // The picker the screen is waiting on.
    bool picker_pending{};    ///< a picker's answer is held back
    PickKind picker_kind{};   ///< what it was asked to choose
    PickerDone picker_done{}; ///< its answer's callback
    void* picker_userdata{};  ///< and its userdata
    // The route.
    std::vector<RouteStep> route{};    ///< the steps
    std::size_t stage{};               ///< the step waited for
    Clock::time_point stage_started{}; ///< since when
    int passes_since_act{};            ///< passes since the last step acted
    bool route_done{};                 ///< every step ran
    std::string failure{};             ///< why the check failed; empty while it has not
    int pictures{};                    ///< pictures written so far
    std::vector<view::Step> seen{};    ///< steps seen, in order, for the opportunistic pictures
    uint32_t most_skipped{};           ///< the most files a copy skipped as already staged
    std::size_t switch_row{};          ///< the row whose switch the route turned off and on
    std::string mod_id{};              ///< the mod the manage route added
    bool screen_ran{};                 ///< the screen's loop ran a pass
    std::size_t held_sources{};        ///< the picked paths the screen held at its last pass
};

/// The check's state.
CheckRun& check_run() {
    static CheckRun run;
    return run;
}

/// Returns a step's name in a picture's name.
///
/// @param step the step
/// @return its slug
[[nodiscard]] std::string step_slug(view::Step step) {
    switch (step) {
    case view::Step::first_run:
        return "first-run";
    case view::Step::looking:
        return "looking";
    case view::Step::nested_offer:
        return "nested-offer";
    case view::Step::already_there:
        return "already-there";
    case view::Step::ready_to_copy:
        return "ready-to-copy";
    case view::Step::copying:
        return "copying";
    case view::Step::checking:
        return "checking";
    case view::Step::ready_to_play:
        return "ready-to-play";
    case view::Step::problem:
        return "problem";
    case view::Step::manage:
        return "manage";
    }
    return "step";
}

/// Returns a sheet's name in a picture's name.
///
/// @param sheet the sheet
/// @return its slug; empty for none
[[nodiscard]] std::string sheet_slug(view::Sheet sheet) {
    switch (sheet) {
    case view::Sheet::none:
        return {};
    case view::Sheet::stop:
        return "stop";
    case view::Sheet::replace_confirm:
        return "replace-confirm";
    case view::Sheet::left_out_list:
        return "left-out-list";
    case view::Sheet::mod_errors:
        return "mod-errors";
    case view::Sheet::remove_old_confirm:
        return "remove-old-confirm";
    case view::Sheet::remove_part_confirm:
        return "remove-part-confirm";
    case view::Sheet::remove_all_confirm:
        return "remove-all-confirm";
    case view::Sheet::add_files:
        return "add-files";
    case view::Sheet::scheduled_note:
        return "scheduled-note";
    }
    return {};
}

/// Returns a problem's name in a picture's name.
///
/// @param problem the problem
/// @return its slug
[[nodiscard]] std::string problem_slug(view::Problem problem) {
    switch (problem) {
    case view::Problem::not_a_game:
        return "not-a-game";
    case view::Problem::cannot_play:
        return "cannot-play";
    case view::Problem::not_demo_installer:
        return "not-demo-installer";
    case view::Problem::short_space:
        return "short-space";
    case view::Problem::disk_full:
        return "disk-full";
    case view::Problem::source_unreadable:
        return "source-unreadable";
    case view::Problem::download_failed:
        return "download-failed";
    case view::Problem::access_withdrawn:
        return "access-withdrawn";
    case view::Problem::source_changed:
        return "source-changed";
    case view::Problem::too_large:
        return "too-large";
    case view::Problem::no_game_folder_yet:
        return "no-game-folder-yet";
    case view::Problem::found_misnamed:
        return "found-misnamed";
    case view::Problem::found_loose:
        return "found-loose";
    case view::Problem::picker_failed:
        return "picker-failed";
    case view::Problem::staging_unwritable:
        return "staging-unwritable";
    }
    return "problem";
}

/// Returns the variant a run checks, from its route, its expectation and its window.
///
/// @param options the parsed command line
/// @return the variant's name
[[nodiscard]] std::string variant_name(const Options& options) {
    switch (options.game_files_route) {
    case GameFilesRoute::folder:
        break;
    case GameFilesRoute::demo:
        return "demo";
    case GameFilesRoute::copy_yourself:
        return "copy-yourself";
    case GameFilesRoute::manage:
        return options.game_files_expect == GameFilesExpect::next_start ? "manage-next-start"
                                                                        : "manage";
    }
    switch (options.game_files_expect) {
    case GameFilesExpect::main_menu:
        break;
    case GameFilesExpect::stopped_kept:
        return "stop";
    case GameFilesExpect::resumed:
        return "resume";
    case GameFilesExpect::not_a_game:
        return "not-game";
    case GameFilesExpect::short_space:
        return "short-space";
    case GameFilesExpect::next_start:
        return "next-start";
    }
    // The folder route in a window whose shorter side is a phone's.
    if (options.window_resolution && std::min(options.match_width, options.match_height) <
                                         oa::ui::touch_hud::phone_short_side_points)
        return "phone";
    return "folder";
}

/// Fails the check with a reason, keeping the first.
///
/// @param why the reason
void fail(std::string why) {
    auto& run = check_run();
    if (run.failure.empty()) {
        run.failure = std::move(why);
        std::cerr << "game-files check: " << run.failure << '\n';
    }
}

/// Writes one picture.
///
/// @param file the picture
/// @param canvas the canvas
void write_picture(const fs::path& file, const touch_paint::Canvas& canvas) {
    std::string error;
    if (!write_game_files_png(file, canvas, &error))
        fail("the picture " + path_to_utf8(file.filename()) + " was not written: " + error);
}

/// Writes the pictures of the screen's state: the window's frame, and the same model laid
/// out and painted at the tablet's and the phone's size.
///
/// @param screen the screen
/// @param extra a word added after the step and sheet; empty for none
void snap(GameFilesScreen& screen, std::string_view extra = {}) {
    auto& run = check_run();
    const view::Model& model = screen.model();
    std::string slug = step_slug(model.step);
    if (model.step == view::Step::problem)
        slug += "-" + problem_slug(model.problem);
    if (const std::string sheet = sheet_slug(model.sheet); !sheet.empty())
        slug += "-" + sheet;
    if (!extra.empty())
        slug += "-" + std::string(extra);
    ++run.pictures;
    // Room for any int the count can hold, its sign included.
    char number[12];
    std::snprintf(number, sizeof number, "%02d", run.pictures);
    const std::string base = "game-files-" + run.variant + "-" + number + "-" + slug;
    write_picture(run.work / (base + "-window.png"), screen.canvas());
    if (screen.language_open())
        return; // The dialog is the window's alone.
    const auto measure = game_files_measure(screen.fonts());
    for (const bool phone : {false, true}) {
        view::Viewport viewport;
        viewport.px_per_point = review_px_per_point;
        viewport.width = static_cast<int>(
            static_cast<float>(phone ? phone_width_points : tablet_width_points) *
            review_px_per_point
        );
        viewport.height = static_cast<int>(
            static_cast<float>(phone ? phone_height_points : tablet_height_points) *
            review_px_per_point
        );
        if (phone) {
            const int side =
                static_cast<int>(static_cast<float>(phone_side_inset_points) * review_px_per_point);
            viewport.safe.left = side;
            viewport.safe.right = side;
            viewport.safe.bottom = static_cast<int>(
                static_cast<float>(phone_bottom_inset_points) * review_px_per_point
            );
        }
        const view::Layout layout = view::lay_out(model, viewport, measure);
        touch_paint::Canvas canvas = touch_paint::make_canvas(viewport.width, viewport.height);
        paint_game_files(canvas, layout, screen.fonts(), viewport.px_per_point);
        write_picture(run.work / (base + (phone ? "-phone.png" : "-tablet.png")), canvas);
    }
}

/// Tells whether the layout holds a control.
///
/// @param screen the screen
/// @param control the control
/// @return true when an item has it
[[nodiscard]] bool has_control(const GameFilesScreen& screen, view::Control control) {
    const auto& items = screen.layout().items;
    return std::any_of(items.begin(), items.end(), [&](const view::Item& item) {
        return item.control == control;
    });
}

/// Tells whether a control is on the layout and can be pressed.
///
/// @param screen the screen
/// @param control the control
/// @return true when its topmost item is enabled
[[nodiscard]] bool control_enabled(const GameFilesScreen& screen, view::Control control) {
    const auto& items = screen.layout().items;
    for (auto item = items.rbegin(); item != items.rend(); ++item)
        if (item->control == control)
            return item->enabled;
    return false;
}

/// Taps a control, failing the check when it is not on the layout.
///
/// @param screen the screen
/// @param control the control
/// @param name its name, for the verdict
void tap(GameFilesScreen& screen, view::Control control, std::string_view name) {
    if (!screen.tap(control))
        fail("the screen shows no " + std::string(name) + " on " + step_slug(screen.model().step));
}

/// Returns a control of a kind with its index.
///
/// @param kind the kind
/// @param index its index
/// @return the control
[[nodiscard]] view::Control control(view::ControlKind kind, uint16_t index = 0) noexcept {
    return view::Control{kind, index};
}

/// Tells whether the screen shows a step, with no sheet.
///
/// @param screen the screen
/// @param step the step
/// @return true when it does
[[nodiscard]] bool showing(const GameFilesScreen& screen, view::Step step) {
    return screen.model().step == step && screen.model().sheet == view::Sheet::none &&
           !screen.language_open();
}

/// Tells whether the screen shows a problem.
///
/// @param screen the screen
/// @param problem the problem
/// @return true when it does
[[nodiscard]] bool showing_problem(const GameFilesScreen& screen, view::Problem problem) {
    return showing(screen, view::Step::problem) && screen.model().problem == problem;
}

/// Fails the check when the screen shows a problem the route does not expect.
///
/// @param screen the screen
/// @param expected the problem the route expects next; nothing when it expects none
void watch_problems(const GameFilesScreen& screen, std::optional<view::Problem> expected) {
    const view::Model& model = screen.model();
    if (model.step != view::Step::problem || (expected && model.problem == *expected))
        return;
    fail(
        "the screen shows the problem " + problem_slug(model.problem) +
        (model.detail.empty() ? std::string{} : ": " + model.detail)
    );
}

/// Links or copies the files of a folder into another, for the copy-yourself route.
///
/// @param from the folder
/// @param to where its files go
/// @return false with the check failed when they could not be put there
bool place_files(const fs::path& from, const fs::path& to) {
    std::error_code error;
    for (fs::recursive_directory_iterator entry(from, error), end; !error && entry != end;
         entry.increment(error)) {
        const fs::path target = to / fs::relative(entry->path(), from, error);
        if (error)
            break;
        if (entry->is_directory(error)) {
            fs::create_directories(target, error);
            continue;
        }
        fs::create_directories(target.parent_path(), error);
        error.clear();
        fs::create_hard_link(entry->path(), target, error);
        if (error) {
            error.clear();
            fs::copy_file(entry->path(), target, fs::copy_options::overwrite_existing, error);
        }
        if (error)
            break;
    }
    if (error) {
        fail("the files could not be placed in " + path_to_utf8(to) + ": " + error.message());
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// The scripted hooks

/// The default game folder: <work>/Documents/Total Annihilation when it exists.
bool check_default_game_folder(void*, std::string* folder) {
    const auto& run = check_run();
    std::error_code error;
    if (!fs::is_directory(run.game_folder, error))
        return false;
    *folder = path_to_utf8(run.game_folder);
    return true;
}

/// Every capability the import knows.
uint32_t check_capabilities(void*) {
    return static_cast<uint32_t>(GameFilesCapability::pick_folder) |
           static_cast<uint32_t>(GameFilesCapability::pick_files) |
           static_cast<uint32_t>(GameFilesCapability::shared_documents) |
           static_cast<uint32_t>(GameFilesCapability::remote_files) |
           static_cast<uint32_t>(GameFilesCapability::background_time);
}

/// The game folder: <work>/Documents/Total Annihilation, whether or not it exists.
bool check_game_folder(void*, std::string* folder) {
    *folder = path_to_utf8(check_run().game_folder);
    return true;
}

/// The picker: its answer is held back for the route's next pass, as a platform's arrives
/// later on the main thread.
void check_show_picker(void*, PickKind kind, PickerDone done, void* userdata) {
    auto& run = check_run();
    run.picker_pending = true;
    run.picker_kind = kind;
    run.picker_done = done;
    run.picker_userdata = userdata;
}

/// Counts the sources let go.
void check_release_source(void*, const char*) {
    auto& run = check_run();
    const base::threads::LockGuard lock(run.mutex);
    ++run.released;
}

/// Copies one file through the engine's copy, held to --game-files-copy-rate when it is given:
/// a file starts only once the bytes copied before it fit the rate since the first began.
/// Once --game-files-stop-after bytes are copied, the next file waits until the copy is
/// stopped, so the route's STOP always finds the copy running, however busy the machine is.
CopyOutcome check_copy_file(void*, const FileCopy& file, std::string* error) {
    auto& run = check_run();
    if (run.options.game_files_stop_after) {
        while (true) {
            uint64_t copied = 0;
            {
                const base::threads::LockGuard lock(run.mutex);
                copied = run.copied_bytes;
            }
            if (copied < *run.options.game_files_stop_after)
                break;
            if (file.stop != nullptr && file.stop->load())
                return CopyOutcome::stopped;
            base::threads::sleep_ms(throttle_nap);
        }
    }
    if (run.options.game_files_copy_rate && *run.options.game_files_copy_rate > 0) {
        const double rate = static_cast<double>(*run.options.game_files_copy_rate);
        while (true) {
            uint64_t copied = 0;
            Clock::time_point started;
            {
                const base::threads::LockGuard lock(run.mutex);
                if (!run.copy_started)
                    run.copy_started = Clock::now();
                started = *run.copy_started;
                copied = run.copied_bytes;
            }
            const double allowed =
                std::chrono::duration<double>(Clock::now() - started).count() * rate;
            if (static_cast<double>(copied) <= allowed)
                break;
            if (file.stop != nullptr && file.stop->load())
                return CopyOutcome::stopped;
            base::threads::sleep_ms(throttle_nap);
        }
    }
    const ChunkedCopy copy = file.copy != nullptr ? file.copy : game_files::chunked_copy;
    const CopyOutcome outcome = copy(file.source, file, error);
    if (outcome == CopyOutcome::copied) {
        const base::threads::LockGuard lock(run.mutex);
        run.copied_bytes += file.size;
    }
    return outcome;
}

/// The free space --game-files-free-bytes gives.
bool check_free_space(void*, const char*, uint64_t* bytes) {
    *bytes = check_run().options.game_files_free_bytes.value_or(0);
    return true;
}

/// Records the requests for time away from the screen; the time never runs out.
void check_keep_running(void*, bool running, void (*expiring)(void*), void* userdata) {
    auto& run = check_run();
    const base::threads::LockGuard lock(run.mutex);
    if (running) {
        ++run.keep_running_on;
        run.expiring = expiring;
        run.expiring_userdata = userdata;
    } else {
        ++run.keep_running_off;
        run.expiring = nullptr;
        run.expiring_userdata = nullptr;
    }
}

/// Ends the time away from the screen the copy asked for, as the system does when it runs out.
void expire_time_away() {
    auto& run = check_run();
    void (*expiring)(void*) = nullptr;
    void* userdata = nullptr;
    {
        const base::threads::LockGuard lock(run.mutex);
        expiring = run.expiring;
        userdata = run.expiring_userdata;
    }
    if (expiring == nullptr) {
        fail("the copy asked for no time away from the screen");
        return;
    }
    expiring(userdata);
}

/// Pushes one of the app lifecycle events, as the system sends it.
///
/// @param type the event's type
void push_lifecycle(SDL_EventType type) {
    SDL_Event event{};
    event.type = type;
    event.common.timestamp = SDL_GetTicksNS();
    SDL_PushEvent(&event);
}

/// Records the backup setting given to each folder.
bool check_set_backed_up(void*, const char* path, bool backed_up) {
    auto& run = check_run();
    const base::threads::LockGuard lock(run.mutex);
    run.backed_up.emplace_back(path != nullptr ? path : "", backed_up);
    return true;
}

/// Delivers a held-back picker answer: the variant's source, or a cancel for archives.
void deliver_picker() {
    auto& run = check_run();
    if (!run.picker_pending)
        return;
    run.picker_pending = false;
    std::vector<std::string> paths;
    bool movable = false;
    if (run.picker_kind != PickKind::archives)
        paths.push_back(path_to_utf8(run.source));
    // The installer is a copy made for the game when it lies in the work folder, as the
    // system's picker makes one; the driver links it there.
    if (run.picker_kind == PickKind::demo_installer) {
        std::error_code error;
        const auto relative = fs::relative(run.source, run.work, error);
        movable = !error && !relative.empty() && relative.native()[0] != '.';
    }
    if (run.picker_done != nullptr)
        run.picker_done(run.picker_userdata, paths, movable, nullptr);
}

// ---------------------------------------------------------------------------------------------
// The routes

/// Tells whether the layout shows the focus ring on a control.
///
/// @param screen the screen
/// @return true when an item is focused
[[nodiscard]] bool any_focused(const GameFilesScreen& screen) {
    const auto& items = screen.layout().items;
    return std::any_of(items.begin(), items.end(), [](const view::Item& item) {
        return item.focused;
    });
}

/// Pushes one finger event of the check's touch device at a canvas point.
///
/// @param screen the screen
/// @param type down, motion or up
/// @param x the canvas column
/// @param y the canvas row
void push_finger(const GameFilesScreen& screen, SDL_EventType type, float x, float y) {
    const view::Viewport& viewport = screen.viewport();
    if (viewport.width <= 0 || viewport.height <= 0)
        return;
    SDL_Event event{};
    event.type = type;
    event.tfinger.timestamp = SDL_GetTicksNS();
    event.tfinger.touchID = game_files_check_touch_id;
    event.tfinger.fingerID = game_files_check_finger_id;
    event.tfinger.x = x / static_cast<float>(viewport.width);
    event.tfinger.y = y / static_cast<float>(viewport.height);
    event.tfinger.pressure = type == SDL_EVENT_FINGER_UP ? 0.0F : 1.0F;
    SDL_PushEvent(&event);
}

/// Drags the rows up by a finger, from their middle, far enough to scroll them.
///
/// @param screen the screen
void drag_rows(const GameFilesScreen& screen) {
    const view::Rect& rows = screen.layout().rows;
    if (rows.width <= 0 || rows.height <= 0)
        return;
    const float x = static_cast<float>(rows.x) + static_cast<float>(rows.width) * 0.75F;
    const float y = static_cast<float>(rows.y) + static_cast<float>(rows.height) * 0.75F;
    const float step = drag_step_points * screen.viewport().px_per_point;
    push_finger(screen, SDL_EVENT_FINGER_DOWN, x, y);
    for (int moves = 1; moves <= drag_moves; ++moves)
        push_finger(screen, SDL_EVENT_FINGER_MOTION, x, y - step * static_cast<float>(moves));
    push_finger(screen, SDL_EVENT_FINGER_UP, x, y - step * static_cast<float>(drag_moves));
}

/// Clicks a control with the mouse's left button, failing the check when it is not on the
/// layout.
///
/// @param screen the screen
/// @param wanted the control
/// @param name its name, for the verdict
void click(const GameFilesScreen& screen, view::Control wanted, std::string_view name) {
    const auto& items = screen.layout().items;
    const view::Item* found = nullptr;
    for (auto item = items.rbegin(); item != items.rend(); ++item)
        if (item->control == wanted) {
            found = &*item;
            break;
        }
    const float px_per_point = std::max(screen.viewport().px_per_point, 0.01F);
    if (found == nullptr) {
        fail("the screen shows no " + std::string(name) + " on " + step_slug(screen.model().step));
        return;
    }
    const float x =
        (static_cast<float>(found->box.x) + static_cast<float>(found->box.width) * 0.5F) /
        px_per_point;
    const float y =
        (static_cast<float>(found->box.y) + static_cast<float>(found->box.height) * 0.5F) /
        px_per_point;
    for (const SDL_EventType type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
        SDL_Event event{};
        event.type = type;
        event.button.timestamp = SDL_GetTicksNS();
        event.button.which = 1;
        event.button.button = SDL_BUTTON_LEFT;
        event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.clicks = 1;
        event.button.x = x;
        event.button.y = y;
        SDL_PushEvent(&event);
    }
}

/// Notes the steps the screen shows, and pictures the passing ones a route does not wait for
/// (Looking, Copying, Checking), once each.
///
/// @param screen the screen
void notice(GameFilesScreen& screen) {
    auto& run = check_run();
    const view::Step step = screen.model().step;
    // While the import works, a player's loop must keep polling it: a screen that waited for an
    // event alone would show a stale step until the next touch.
    const bool working =
        step == view::Step::looking || step == view::Step::copying || step == view::Step::checking;
    const bool paused = screen.run().stage == game_files::RunStage::stopped;
    if (working && !paused && screen.model().sheet == view::Sheet::none &&
        screen.waits_for_events_only())
        fail(
            "the screen shows " + step_slug(step) +
            " but would wait for an event alone, with nothing left to poll"
        );
    if (std::find(run.seen.begin(), run.seen.end(), step) == run.seen.end()) {
        run.seen.push_back(step);
        if ((step == view::Step::looking || step == view::Step::copying ||
             step == view::Step::checking) &&
            screen.model().sheet == view::Sheet::none && !screen.language_open())
            snap(screen);
    }
    run.most_skipped = std::max(run.most_skipped, screen.run().files_skipped);
}

/// The folder route's steps, from S1 to PLAY, for the folder and phone variants.
///
/// @return the steps
[[nodiscard]] std::vector<RouteStep> folder_route() {
    std::vector<RouteStep> steps;
    steps.push_back(
        {"the Game files screen (S1)",
         [](GameFilesScreen& s) { return showing(s, view::Step::first_run); },
         [](GameFilesScreen& s) {
             snap(s);
             s.press_key(SDLK_TAB);
         }}
    );
    steps.push_back(
        {"the focus ring after Tab",
         [](GameFilesScreen& s) { return any_focused(s); },
         [](GameFilesScreen& s) {
             snap(s, "focus");
             tap(s, control(view::ControlKind::language), "OA · Aa");
         }}
    );
    steps.push_back(
        {"the Language dialog",
         [](GameFilesScreen& s) { return s.language_open(); },
         [](GameFilesScreen& s) {
             snap(s, "language");
             // The folder variant leaves the dialog with Escape (Cancel), the phone variant
             // with Return (OK, which writes the preferences file).
             s.press_key(check_run().variant == "phone" ? SDLK_RETURN : SDLK_ESCAPE);
         }}
    );
    steps.push_back(
        {"S1 again after the dialog",
         [](GameFilesScreen& s) { return showing(s, view::Step::first_run); },
         [](GameFilesScreen& s) {
             tap(s, control(view::ControlKind::choose_folder), "CHOOSE FOLDER");
         }}
    );
    steps.push_back(
        {"Ready to copy (S3)",
         [](GameFilesScreen& s) {
             watch_problems(s, std::nullopt);
             return showing(s, view::Step::ready_to_copy);
         },
         [](GameFilesScreen& s) {
             snap(s);
             const auto& model = s.model();
             if (model.left_out.empty())
                 fail("Ready to copy lists no file left out");
             if (model.parts.empty() || model.parts.front().kind != view::PartKind::game_archives ||
                 model.parts.front().mark != view::Mark::found)
                 fail("Ready to copy does not show the game archives found");
             // Rows that scroll are dragged up by a finger.
             if (s.layout().scroll_max_points > 0)
                 drag_rows(s);
         }}
    );
    steps.push_back(
        {"the rows dragged",
         [](GameFilesScreen& s) {
             return check_run().passes_since_act >= settle_passes &&
                    (s.layout().scroll_max_points <= 0 || s.model().scroll_points > 0);
         },
         [](GameFilesScreen& s) {
             if (s.model().scroll_points > 0)
                 snap(s, "dragged");
             tap(s, control(view::ControlKind::show_left_out), "SHOW");
         },
         [](GameFilesScreen&) -> std::optional<view::Control> {
             return control(view::ControlKind::show_left_out);
         }}
    );
    steps.push_back(
        {"the list of files left out",
         [](GameFilesScreen& s) { return s.model().sheet == view::Sheet::left_out_list; },
         [](GameFilesScreen& s) {
             snap(s);
             s.press_key(SDLK_ESCAPE);
         }}
    );
    steps.push_back(
        {"Ready to copy with the sheet closed",
         [](GameFilesScreen& s) { return showing(s, view::Step::ready_to_copy); },
         [](GameFilesScreen& s) {
             auto& run = check_run();
             if (run.switch_row >= s.model().parts.size()) {
                 fail("Ready to copy shows no switch to turn off");
                 return;
             }
             tap(s,
                 control(view::ControlKind::part_switch, static_cast<uint16_t>(run.switch_row)),
                 "a part's switch");
         },
         [](GameFilesScreen& s) -> std::optional<view::Control> {
             auto& run = check_run();
             const auto& parts = s.model().parts;
             const auto found =
                 std::find_if(parts.begin(), parts.end(), [](const view::PartRow& row) {
                     return row.has_switch && row.on;
                 });
             run.switch_row = static_cast<std::size_t>(found - parts.begin());
             if (found == parts.end())
                 return std::nullopt;
             return control(view::ControlKind::part_switch, static_cast<uint16_t>(run.switch_row));
         }}
    );
    steps.push_back(
        {"a part switched off",
         [](GameFilesScreen& s) {
             const auto& parts = s.model().parts;
             const std::size_t row = check_run().switch_row;
             return row < parts.size() && !parts[row].on;
         },
         [](GameFilesScreen& s) {
             snap(s, "switch-off");
             tap(s,
                 control(
                     view::ControlKind::part_switch, static_cast<uint16_t>(check_run().switch_row)
                 ),
                 "a part's switch");
         },
         [](GameFilesScreen&) -> std::optional<view::Control> {
             return control(
                 view::ControlKind::part_switch, static_cast<uint16_t>(check_run().switch_row)
             );
         }}
    );
    steps.push_back(
        {"the part switched on again",
         [](GameFilesScreen& s) {
             const auto& parts = s.model().parts;
             const std::size_t row = check_run().switch_row;
             return row < parts.size() && parts[row].on;
         },
         [](GameFilesScreen& s) {
             if (!control_enabled(s, control(view::ControlKind::copy)))
                 fail("COPY cannot be pressed on Ready to copy");
             tap(s, control(view::ControlKind::copy), "COPY");
         }}
    );
    steps.push_back(
        {"Ready to play (S5)",
         [](GameFilesScreen& s) {
             watch_problems(s, std::nullopt);
             return showing(s, view::Step::ready_to_play);
         },
         [](GameFilesScreen& s) {
             snap(s);
             tap(s, control(view::ControlKind::play), "PLAY");
         }}
    );
    return steps;
}

/// The demo route's steps.
///
/// @return the steps
[[nodiscard]] std::vector<RouteStep> demo_route() {
    std::vector<RouteStep> steps;
    steps.push_back(
        {"the Game files screen (S1)",
         [](GameFilesScreen& s) { return showing(s, view::Step::first_run); },
         [](GameFilesScreen& s) {
             snap(s);
             tap(s, control(view::ControlKind::choose_installer), "CHOOSE INSTALLER");
         }}
    );
    steps.push_back(
        {"Ready to copy with the demo's row",
         [](GameFilesScreen& s) {
             watch_problems(s, std::nullopt);
             return showing(s, view::Step::ready_to_copy);
         },
         [](GameFilesScreen& s) {
             snap(s);
             if (!s.model().demo)
                 fail("Ready to copy does not show the demo's installer");
             tap(s, control(view::ControlKind::copy), "COPY");
         }}
    );
    steps.push_back(
        {"Ready to play (S5)",
         [](GameFilesScreen& s) {
             watch_problems(s, std::nullopt);
             return showing(s, view::Step::ready_to_play);
         },
         [](GameFilesScreen& s) {
             snap(s);
             tap(s, control(view::ControlKind::play), "PLAY");
         }}
    );
    return steps;
}

/// The copy-yourself route's steps: I HAVE COPIED IT with nothing there, the files copied
/// under another name, CHECK AGAIN, USE IT.
///
/// @return the steps
[[nodiscard]] std::vector<RouteStep> copy_yourself_route() {
    std::vector<RouteStep> steps;
    steps.push_back(
        {"the Game files screen (S1)",
         [](GameFilesScreen& s) { return showing(s, view::Step::first_run); },
         [](GameFilesScreen& s) {
             snap(s);
             tap(s, control(view::ControlKind::i_have_copied), "I HAVE COPIED IT");
         }}
    );
    steps.push_back(
        {"the problem No game folder yet",
         [](GameFilesScreen& s) {
             watch_problems(s, view::Problem::no_game_folder_yet);
             return showing_problem(s, view::Problem::no_game_folder_yet);
         },
         [](GameFilesScreen& s) {
             snap(s);
             auto& run = check_run();
             if (place_files(run.source, run.documents / std::string(misnamed_folder)))
                 tap(s, control(view::ControlKind::problem_action, 0), "CHECK AGAIN");
         }}
    );
    steps.push_back(
        {"the problem Found your files",
         [](GameFilesScreen& s) {
             watch_problems(s, view::Problem::found_misnamed);
             return showing_problem(s, view::Problem::found_misnamed);
         },
         [](GameFilesScreen& s) {
             snap(s);
             if (s.model().file != misnamed_folder)
                 fail("Found your files names " + s.model().file + ", not the folder copied");
             tap(s, control(view::ControlKind::problem_action, 0), "USE IT");
         }}
    );
    return steps;
}

/// The stop route's steps: a slow copy stopped part way and kept.
///
/// @return the steps
[[nodiscard]] std::vector<RouteStep> stop_route() {
    std::vector<RouteStep> steps;
    steps.push_back(
        {"the Game files screen (S1)",
         [](GameFilesScreen& s) { return showing(s, view::Step::first_run); },
         [](GameFilesScreen& s) {
             tap(s, control(view::ControlKind::choose_folder), "CHOOSE FOLDER");
         }}
    );
    steps.push_back(
        {"Ready to copy (S3)",
         [](GameFilesScreen& s) {
             watch_problems(s, std::nullopt);
             return showing(s, view::Step::ready_to_copy);
         },
         [](GameFilesScreen& s) { tap(s, control(view::ControlKind::copy), "COPY"); }}
    );
    steps.push_back(
        {"the copy past --game-files-stop-after",
         [](GameFilesScreen& s) {
             watch_problems(s, std::nullopt);
             const uint64_t after = check_run().options.game_files_stop_after.value_or(1);
             return showing(s, view::Step::copying) && s.run().bytes_done >= after;
         },
         [](GameFilesScreen& s) {
             snap(s);
             // The game leaves the screen and comes back while the copy goes on.
             push_lifecycle(SDL_EVENT_WILL_ENTER_BACKGROUND);
             push_lifecycle(SDL_EVENT_DID_ENTER_BACKGROUND);
             push_lifecycle(SDL_EVENT_WILL_ENTER_FOREGROUND);
             push_lifecycle(SDL_EVENT_DID_ENTER_FOREGROUND);
         }}
    );
    steps.push_back(
        {"the banner that copying went on while away",
         [](GameFilesScreen& s) {
             watch_problems(s, std::nullopt);
             return showing(s, view::Step::copying) &&
                    s.model().banner == view::Banner::copy_went_on;
         },
         [](GameFilesScreen& s) {
             snap(s, "went-on");
             // Away again, and this time the system's time runs out.
             push_lifecycle(SDL_EVENT_WILL_ENTER_BACKGROUND);
             push_lifecycle(SDL_EVENT_DID_ENTER_BACKGROUND);
             expire_time_away();
         }}
    );
    steps.push_back(
        {"the copy paused when the time away ran out",
         [](GameFilesScreen& s) {
             watch_problems(s, std::nullopt);
             return s.run().stage == game_files::RunStage::stopped && s.run().stopped_by_expiry;
         },
         [](GameFilesScreen&) {
             push_lifecycle(SDL_EVENT_WILL_ENTER_FOREGROUND);
             push_lifecycle(SDL_EVENT_DID_ENTER_FOREGROUND);
         }}
    );
    steps.push_back(
        {"the copy started again with its banner",
         [](GameFilesScreen& s) {
             watch_problems(s, std::nullopt);
             return showing(s, view::Step::copying) &&
                    s.model().banner == view::Banner::copy_resumed;
         },
         [](GameFilesScreen& s) {
             snap(s, "resumed");
             tap(s, control(view::ControlKind::stop), "STOP");
         }}
    );
    steps.push_back(
        {"the Stop copying? sheet",
         [](GameFilesScreen& s) { return s.model().sheet == view::Sheet::stop; },
         [](GameFilesScreen& s) {
             snap(s);
             tap(s, control(view::ControlKind::sheet_option, 0), "KEEP WHAT WAS COPIED");
         }}
    );
    steps.push_back(
        {"S1 with the banner to continue",
         [](GameFilesScreen& s) {
             return showing(s, view::Step::first_run) &&
                    s.model().banner == view::Banner::continue_copy;
         },
         [](GameFilesScreen& s) {
             snap(s);
             s.request_quit();
         }}
    );
    return steps;
}

/// The resume route's steps: the banner, the same folder, staged files skipped.
///
/// @return the steps
[[nodiscard]] std::vector<RouteStep> resume_route() {
    std::vector<RouteStep> steps;
    steps.push_back(
        {"S1 with the banner to continue",
         [](GameFilesScreen& s) {
             return showing(s, view::Step::first_run) &&
                    s.model().banner == view::Banner::continue_copy;
         },
         [](GameFilesScreen& s) {
             snap(s);
             tap(s, control(view::ControlKind::choose_folder), "CHOOSE FOLDER");
         }}
    );
    steps.push_back(
        {"Ready to play (S5) after the copy continued",
         [](GameFilesScreen& s) {
             watch_problems(s, std::nullopt);
             return showing(s, view::Step::ready_to_play);
         },
         [](GameFilesScreen& s) {
             snap(s);
             if (check_run().most_skipped == 0)
                 fail("the copy skipped no file already copied");
             tap(s, control(view::ControlKind::play), "PLAY");
         }}
    );
    return steps;
}

/// The not-a-game route's steps.
///
/// @return the steps
[[nodiscard]] std::vector<RouteStep> not_game_route() {
    std::vector<RouteStep> steps;
    steps.push_back(
        {"the Game files screen (S1)",
         [](GameFilesScreen& s) { return showing(s, view::Step::first_run); },
         [](GameFilesScreen& s) {
             // A mouse's click, not a finger's tap.
             click(s, control(view::ControlKind::choose_folder), "CHOOSE FOLDER");
         }}
    );
    steps.push_back(
        {"the problem This folder does not hold a Total Annihilation installation",
         [](GameFilesScreen& s) {
             watch_problems(s, view::Problem::not_a_game);
             return showing_problem(s, view::Problem::not_a_game);
         },
         [](GameFilesScreen& s) {
             snap(s);
             if (!has_control(s, control(view::ControlKind::back)))
                 fail("the problem shows no BACK");
             // Escape goes back, as BACK does.
             s.press_key(SDLK_ESCAPE);
         }}
    );
    steps.push_back(
        {"S1 after Escape",
         [](GameFilesScreen& s) { return showing(s, view::Step::first_run); },
         [](GameFilesScreen& s) {
             snap(s);
             s.request_quit();
         }}
    );
    return steps;
}

/// Tells whether the screen shows the shortage of space, on Ready to copy or as a problem.
///
/// @param screen the screen
/// @return true when it does
[[nodiscard]] bool showing_short_space(const GameFilesScreen& screen) {
    return (showing(screen, view::Step::ready_to_copy) && screen.model().space_short) ||
           showing_problem(screen, view::Problem::short_space);
}

/// The short-space route's steps.
///
/// @return the steps
[[nodiscard]] std::vector<RouteStep> short_space_route() {
    std::vector<RouteStep> steps;
    steps.push_back(
        {"the Game files screen (S1)",
         [](GameFilesScreen& s) { return showing(s, view::Step::first_run); },
         [](GameFilesScreen& s) {
             tap(s, control(view::ControlKind::choose_folder), "CHOOSE FOLDER");
         }}
    );
    steps.push_back(
        {"the shortage of space",
         [](GameFilesScreen& s) {
             watch_problems(s, view::Problem::short_space);
             return showing_short_space(s);
         },
         [](GameFilesScreen& s) {
             snap(s);
             if (s.model().step == view::Step::ready_to_copy) {
                 if (control_enabled(s, control(view::ControlKind::copy)))
                     fail("COPY can be pressed although the space is short");
                 tap(s, control(view::ControlKind::check_space), "CHECK AGAIN");
             } else {
                 tap(s, control(view::ControlKind::problem_action, 0), "CHECK AGAIN");
             }
         }}
    );
    steps.push_back(
        {"the shortage still shown after CHECK AGAIN",
         [](GameFilesScreen& s) {
             return check_run().passes_since_act >= settle_passes && showing_short_space(s);
         },
         [](GameFilesScreen& s) {
             snap(s, "checked-again");
             s.request_quit();
         }}
    );
    return steps;
}

/// Returns the row of a part on the management state.
///
/// @param screen the screen
/// @param kind the part
/// @return its row; nothing when it is not shown
[[nodiscard]] std::optional<uint16_t> row_of(const GameFilesScreen& screen, view::PartKind kind) {
    const auto& parts = screen.model().parts;
    for (std::size_t row = 0; row < parts.size(); ++row)
        if (parts[row].kind == kind)
            return static_cast<uint16_t>(row);
    return std::nullopt;
}

/// The management route's steps: ADD FILES… with a mod, CHECK AGAIN, REMOVE Music, DONE.
///
/// @return the steps
[[nodiscard]] std::vector<RouteStep> manage_route() {
    std::vector<RouteStep> steps;
    steps.push_back(
        {"the management state (S8)",
         [](GameFilesScreen& s) { return showing(s, view::Step::manage); },
         [](GameFilesScreen& s) {
             snap(s);
             tap(s, control(view::ControlKind::manage_add), "ADD FILES…");
         }}
    );
    steps.push_back(
        {"the Add files sheet",
         [](GameFilesScreen& s) { return s.model().sheet == view::Sheet::add_files; },
         [](GameFilesScreen& s) {
             snap(s);
             tap(s, control(view::ControlKind::sheet_option, 0), "Choose a folder");
         }}
    );
    steps.push_back(
        {"Ready to copy the mod",
         [](GameFilesScreen& s) {
             watch_problems(s, std::nullopt);
             return showing(s, view::Step::ready_to_copy);
         },
         [](GameFilesScreen& s) {
             snap(s);
             const auto& parts = s.model().parts;
             const auto mod =
                 std::find_if(parts.begin(), parts.end(), [](const view::PartRow& row) {
                     return row.kind == view::PartKind::mod;
                 });
             if (mod == parts.end())
                 fail("Ready to copy does not show the mod");
             tap(s, control(view::ControlKind::copy), "COPY");
         }}
    );
    steps.push_back(
        {"S8 with the files added",
         [](GameFilesScreen& s) {
             watch_problems(s, std::nullopt);
             return showing(s, view::Step::manage) && s.model().banner == view::Banner::added;
         },
         [](GameFilesScreen& s) {
             snap(s);
             tap(s, control(view::ControlKind::manage_check), "CHECK AGAIN");
         }}
    );
    steps.push_back(
        {"the check's result",
         [](GameFilesScreen& s) {
             watch_problems(s, std::nullopt);
             return showing(s, view::Step::ready_to_play);
         },
         [](GameFilesScreen& s) {
             snap(s);
             if (has_control(s, control(view::ControlKind::back)))
                 tap(s, control(view::ControlKind::back), "BACK");
             else
                 tap(s, control(view::ControlKind::play), "PLAY");
         }}
    );
    steps.push_back(
        {"S8 after the check",
         [](GameFilesScreen& s) { return showing(s, view::Step::manage); },
         [](GameFilesScreen& s) {
             const auto music = row_of(s, view::PartKind::music);
             if (!music) {
                 fail("the management state shows no Music row");
                 return;
             }
             tap(s, control(view::ControlKind::manage_remove, *music), "REMOVE (Music)");
         },
         [](GameFilesScreen& s) -> std::optional<view::Control> {
             const auto music = row_of(s, view::PartKind::music);
             if (!music)
                 return std::nullopt;
             return control(view::ControlKind::manage_remove, *music);
         }}
    );
    steps.push_back(
        {"the sheet to remove Music",
         [](GameFilesScreen& s) { return s.model().sheet == view::Sheet::remove_part_confirm; },
         [](GameFilesScreen& s) {
             snap(s);
             tap(s, control(view::ControlKind::sheet_option, 0), "REMOVE");
         }}
    );
    steps.push_back(
        {"S8 with the change for the next start",
         [](GameFilesScreen& s) {
             watch_problems(s, std::nullopt);
             return showing(s, view::Step::manage) && s.model().banner == view::Banner::next_start;
         },
         [](GameFilesScreen& s) {
             snap(s);
             tap(s, control(view::ControlKind::manage_done), "DONE");
         }}
    );
    steps.push_back(
        {"the note that changes wait for the next start",
         [](GameFilesScreen& s) { return s.model().sheet == view::Sheet::scheduled_note; },
         [](GameFilesScreen& s) {
             snap(s);
             tap(s, control(view::ControlKind::sheet_option, 0), "OK");
         }}
    );
    return steps;
}

/// The second start's management steps: what waited applied, then DONE.
///
/// @return the steps
[[nodiscard]] std::vector<RouteStep> next_start_route() {
    std::vector<RouteStep> steps;
    steps.push_back(
        {"the management state (S8) at the next start",
         [](GameFilesScreen& s) { return showing(s, view::Step::manage); },
         [](GameFilesScreen& s) {
             snap(s);
             if (row_of(s, view::PartKind::music))
                 fail("Music is still shown after the next start");
             if (!row_of(s, view::PartKind::mod))
                 fail("the mod added is not shown at the next start");
             tap(s, control(view::ControlKind::manage_done), "DONE");
         }}
    );
    return steps;
}

/// Scrolls the rows so that a control among them can be tapped: when a tap at its centre
/// would not reach it, the rows are turned by the wheel towards it.
///
/// @param screen the screen
/// @param wanted the control
/// @return true when a tap at its centre reaches it, or scrolling cannot help
bool bring_into_view(GameFilesScreen& screen, view::Control wanted) {
    const view::Layout& layout = screen.layout();
    const view::Item* found = nullptr;
    for (auto item = layout.items.rbegin(); item != layout.items.rend(); ++item)
        if (item->control == wanted) {
            found = &*item;
            break;
        }
    if (found == nullptr || found->clip.width <= 0 || found->clip.height <= 0)
        return true;
    const view::Point centre{
        found->box.x + found->box.width / 2, found->box.y + found->box.height / 2
    };
    const float px_per_point = std::max(screen.viewport().px_per_point, 0.01F);
    if (view::hit_test(layout, centre, view::pick_reach_points * px_per_point) == wanted)
        return true;
    // Turn the wheel by the distance from the middle of the rows to the control.
    const float offset_points =
        static_cast<float>(centre.y - (found->clip.y + found->clip.height / 2)) / px_per_point;
    if (offset_points == 0.0F)
        return true;
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.timestamp = SDL_GetTicksNS();
    wheel.wheel.which = 0;
    wheel.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
    wheel.wheel.y = -offset_points / wheel_notch_points;
    SDL_PushEvent(&wheel);
    return false;
}

/// Starts a route's steps.
///
/// @param steps the steps
void start_route(std::vector<RouteStep> steps) {
    auto& run = check_run();
    run.route = std::move(steps);
    run.stage = 0;
    run.stage_started = Clock::now();
    run.passes_since_act = 0;
    run.route_done = run.route.empty();
}

/// Runs the route one pass: the held-back picker's answer, then the step waited for.
void route_pass(void*, GameFilesScreen& screen) {
    auto& run = check_run();
    run.screen_ran = true;
    run.held_sources = screen.held_sources();
    notice(screen);
    if (!run.failure.empty()) {
        screen.request_quit();
        return;
    }
    if (run.picker_pending) {
        deliver_picker();
        return;
    }
    ++run.passes_since_act;
    if (run.route_done || run.stage >= run.route.size())
        return;
    RouteStep& step = run.route[run.stage];
    if (step.ready(screen)) {
        if (step.shows) {
            if (const auto shown = step.shows(screen); shown && !bring_into_view(screen, *shown))
                return;
        }
        step.act(screen);
        ++run.stage;
        run.stage_started = Clock::now();
        run.passes_since_act = 0;
        run.route_done = run.stage >= run.route.size();
    } else if (
        std::chrono::duration<double>(Clock::now() - run.stage_started).count() > step_seconds
    ) {
        fail(
            "timed out waiting for " + step.what + " (the screen shows " +
            step_slug(screen.model().step) +
            (screen.model().step == view::Step::problem ? " " + problem_slug(screen.model().problem)
                                                        : "") +
            ")"
        );
    }
    if (!run.failure.empty())
        screen.request_quit();
}

/// Tells whether a file lies in a folder, its name matched without regard to case.
///
/// @param folder the folder
/// @param name the name
/// @return true when it is there
[[nodiscard]] bool holds_file(const fs::path& folder, std::string_view name) {
    std::error_code error;
    for (fs::directory_iterator entry(folder, error), end; !error && entry != end;
         entry.increment(error)) {
        std::string found = path_to_utf8(entry->path().filename());
        std::string wanted(name);
        std::transform(found.begin(), found.end(), found.begin(), [](unsigned char c) {
            return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        });
        std::transform(wanted.begin(), wanted.end(), wanted.begin(), [](unsigned char c) {
            return static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        });
        if (found == wanted)
            return true;
    }
    return false;
}

/// Finds the mods a folder holds: each folder of its mods folder with a profile.
///
/// @param game_folder the game folder
/// @return the mods' folder names
[[nodiscard]] std::vector<std::string> mods_in(const fs::path& game_folder) {
    std::vector<std::string> mods;
    std::error_code error;
    for (fs::directory_iterator entry(game_folder / "mods", error), end; !error && entry != end;
         entry.increment(error))
        if (entry->is_directory(error) && holds_file(entry->path(), "oamod.yaml"))
            mods.push_back(path_to_utf8(entry->path().filename()));
    return mods;
}

/// Checks what the folder route left in the game folder: the subset, without the files left
/// out, kept out of the backups.
void verify_folder_copy() {
    auto& run = check_run();
    for (const std::string_view archive : subset_archives)
        if (!holds_file(run.game_folder, archive))
            fail("the game folder lacks " + std::string(archive) + " after the copy");
    for (const std::string_view left_out : left_out_fixtures)
        if (holds_file(run.game_folder, left_out))
            fail("the game folder holds " + std::string(left_out) + ", which is left out");
    if (run.held_sources != 0)
        fail("the folder picked was not let go after the copy");
    const base::threads::LockGuard lock(run.mutex);
    if (run.keep_running_on == 0 || run.keep_running_off < run.keep_running_on)
        fail("the copy did not ask for time away from the screen and give it back");
    if (run.released == 0)
        fail("the folder picked was never let go");
    const std::string folder = path_to_utf8(run.game_folder);
    const bool recorded =
        std::any_of(run.backed_up.begin(), run.backed_up.end(), [&](const auto& call) {
            return call.first == folder && !call.second;
        });
    if (!recorded)
        fail("the game folder was not kept out of the device's backups");
}

} // namespace

void install_game_files_check(const Options& options) {
    auto& run = check_run();
    run.options = options;
    run.variant = variant_name(options);
    std::error_code error;
    run.work = fs::current_path(error);
    run.documents = run.work / std::string(documents_name);
    run.game_folder = run.documents / std::string(game_files::game_folder_name);
    run.source = options.game_files_source.empty() ? run.work / std::string(default_source)
                                                   : options.game_files_source;
    if (run.source.is_relative())
        run.source = run.work / run.source;
    // The game folder's parent is always there, as a platform's documents folder is.
    fs::create_directories(run.documents, error);

    PlatformHooks platform{};
    platform.default_game_folder = check_default_game_folder;
    set_platform_hooks(platform);

    GameFilesHooks hooks{};
    hooks.context = &run;
    hooks.capabilities = check_capabilities;
    hooks.game_folder = check_game_folder;
    hooks.show_picker = check_show_picker;
    hooks.release_source = check_release_source;
    hooks.copy_file = check_copy_file;
    if (options.game_files_free_bytes)
        hooks.free_space = check_free_space;
    hooks.keep_running = check_keep_running;
    hooks.set_backed_up = check_set_backed_up;
    set_game_files_hooks(hooks);

    switch (options.game_files_route) {
    case GameFilesRoute::folder:
        switch (options.game_files_expect) {
        case GameFilesExpect::stopped_kept:
            start_route(stop_route());
            break;
        case GameFilesExpect::resumed:
            start_route(resume_route());
            break;
        case GameFilesExpect::not_a_game:
            start_route(not_game_route());
            break;
        case GameFilesExpect::short_space:
            start_route(short_space_route());
            break;
        case GameFilesExpect::main_menu:
        case GameFilesExpect::next_start:
            start_route(folder_route());
            break;
        }
        break;
    case GameFilesRoute::demo:
        start_route(demo_route());
        break;
    case GameFilesRoute::copy_yourself:
        start_route(copy_yourself_route());
        break;
    case GameFilesRoute::manage:
        start_route(
            options.game_files_expect == GameFilesExpect::next_start ? next_start_route()
                                                                     : manage_route()
        );
        break;
    }
}

GameFilesScreenCheckHooks game_files_check_hooks(const Options& options) {
    static_cast<void>(options);
    GameFilesScreenCheckHooks hooks;
    hooks.context = &check_run();
    hooks.pass = route_pass;
    return hooks;
}

bool run_game_files_manage_check(
    const Options& options, SDL_Window* window, SDL_Renderer* renderer
) {
    auto& run = check_run();
    const fs::path data_folder =
        options.data_dir.value_or(oa::platform::preferences::data_directory());
    if (options.game_files_expect == GameFilesExpect::next_start) {
        // What waited for this start was applied before resolution.
        std::error_code error;
        if (fs::exists(run.game_folder / "music", error) &&
            !fs::is_empty(run.game_folder / "music", error))
            fail("the music folder is still there after the next start");
        if (mods_in(run.game_folder).empty())
            fail("the game folder holds no mod after the next start");
        if (fs::exists(
                data_folder / std::string(game_files::import_folder_name) /
                    std::string(game_files::state_file_name),
                error
            ))
            fail("a change still waits after the next start");
    }
    GameFilesScreenRequest request;
    request.window = window;
    request.renderer = renderer;
    request.entry = GameFilesEntry::manage;
    request.paths = game_files::import_paths(run.game_folder, data_folder);
    request.mod =
        ModChoice{options.mod_dir, options.mod_file, options.accept_unimplemented_hacks, nullptr};
    request.version = game_files_version_text();
    request.preferences_file = options.preferences_file;
    request.players_own_profile = !options.preferences_file.has_value();
    request.check = game_files_check_hooks(options);
    const GameFilesEnd end = run_game_files_screen(request);
    if (end != GameFilesEnd::done && run.failure.empty())
        fail("the management state ended without DONE");
    if (!run.route_done && run.failure.empty())
        fail(
            "the management route stopped at " +
            (run.stage < run.route.size() ? run.route[run.stage].what : std::string("its end"))
        );
    if (run.failure.empty() && options.game_files_expect != GameFilesExpect::next_start) {
        const auto mods = mods_in(run.game_folder);
        if (mods.empty())
            fail("the mod added is not in the game folder's mods folder");
        std::string error;
        const auto state = game_files::read_import_state(request.paths.state_file, &error);
        if (!state || state->mode != game_files::ImportMode::remove || state->removals.empty())
            fail("no removal waits for the next start");
    }
    return run.failure.empty();
}

int finish_game_files_check(const Options& options, int status) {
    auto& run = check_run();
    if (run.failure.empty()) {
        if (!run.screen_ran)
            fail("the Game files screen never opened");
        else if (!run.route_done)
            fail(
                "the route stopped at " +
                (run.stage < run.route.size() ? run.route[run.stage].what : std::string("its end"))
            );
        else if (status != 0)
            fail("the game ended with status " + std::to_string(status));
    }
    if (run.failure.empty()) {
        const fs::path data_folder =
            options.data_dir.value_or(oa::platform::preferences::data_directory());
        const auto paths = game_files::import_paths(run.game_folder, data_folder);
        std::error_code error;
        switch (options.game_files_route) {
        case GameFilesRoute::folder:
            if (options.game_files_expect == GameFilesExpect::main_menu ||
                options.game_files_expect == GameFilesExpect::resumed)
                verify_folder_copy();
            if (options.game_files_expect == GameFilesExpect::stopped_kept) {
                std::string why;
                const auto state = game_files::read_import_state(paths.state_file, &why);
                if (!state || state->phase != game_files::ImportPhase::copying)
                    fail("no copy waits to be continued after KEEP WHAT WAS COPIED");
                if (game_files::staged_bytes(paths.staging) == 0)
                    fail("nothing was kept of the copy");
                if (fs::exists(run.game_folder, error))
                    fail("the game folder exists after a copy that was stopped");
            }
            if (options.game_files_expect == GameFilesExpect::resumed && run.most_skipped == 0)
                fail("the copy skipped no file already copied");
            break;
        case GameFilesRoute::demo:
            if (!fs::is_regular_file(
                    data_folder / std::string(demo_1997.folder_name) /
                        std::string(demo_1997.archive_name),
                    error
                ))
                fail("the demo's game data was not unpacked");
            if (!fs::is_directory(run.game_folder, error))
                fail("the game folder was not made");
            break;
        case GameFilesRoute::copy_yourself:
            if (!holds_file(run.game_folder, "totala1.hpi"))
                fail("the game folder lacks totala1.hpi after USE IT");
            if (fs::exists(run.documents / std::string(misnamed_folder), error))
                fail("the folder copied under another name is still there after USE IT");
            break;
        case GameFilesRoute::manage:
            break;
        }
    }
    if (run.failure.empty())
        std::printf("game-files check: %s: passed\n", run.variant.c_str());
    else
        std::printf("game-files check: %s: failed: %s\n", run.variant.c_str(), run.failure.c_str());
    std::fflush(stdout);
    return run.failure.empty() ? status : 1;
}

} // namespace oa::app
