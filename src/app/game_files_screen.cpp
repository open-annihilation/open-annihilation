// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Game files screen's loop on the game's window (game_files_screen.hpp):
// events mapped to presses and keys, the import's snapshots polled into the
// model, the layout painted and presented, and the controller that carries
// out what presses ask: the system's picker, the look at a source, the copy,
// the commit, the management state's removals and additions, and the
// Language dialog.
#include "game_files_screen.hpp"

#include "game_files_paint.hpp"
#include "render_host.hpp"
#include "render_state.hpp"
#include "window_pixels.hpp"

#include "oa/app/content_copy.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/game_files_hooks.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/ui/engine_settings.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <exception>
#include <iostream>
#include <memory>
#include <string>
#include <system_error>
#include <utility>

#ifndef OA_ENGINE_VERSION
#define OA_ENGINE_VERSION "0.0.0"
#endif

namespace oa::app {

// Named once, whichever app header that draws with the painter is included first.
#ifndef OA_APP_UI_PAINT
#define OA_APP_UI_PAINT
namespace paint = oa::ui::paint;
#endif

namespace view = oa::ui::game_files;
namespace text_font = oa::platform::text_font;
using game_files::SourceKind;

std::string game_files_version_text() {
    return std::string("v") + OA_ENGINE_VERSION;
}

namespace {

/// How long the loop waits for an event while a worker runs, so that its progress is
/// polled about four times a second, in milliseconds.
constexpr int32_t worker_wait_ms = 250;
/// How long the loop waits while a picker's answer is awaited, in milliseconds.
constexpr int32_t picker_wait_ms = 100;
/// How long the loop waits while the check drives it, in milliseconds.
constexpr int32_t check_wait_ms = 16;
/// The shortest time between two repaints for progress alone, in nanoseconds.
constexpr uint64_t progress_repaint_ns = 250'000'000;
/// The copy's time left is shown once it has run this long, in nanoseconds.
constexpr uint64_t time_left_after_ns = 5'000'000'000;
/// The time left comes from the rate over this window, in nanoseconds.
constexpr uint64_t rate_window_ns = 10'000'000'000;
/// Nanoseconds in a second.
constexpr double ns_per_second = 1.0e9;

/// Writes one line about the import to the game's log (standard error, which the log holds).
///
/// @param line what happened, without the leading "game files: "
void log_import(const std::string& line) {
    std::cerr << "open-annihilation: game files: " << line << '\n';
}

/// The parts Ready to copy shows in order, before the mods.
constexpr std::array<game_files::Part, 7> shown_parts{
    game_files::Part::game_archives,
    game_files::Part::update_31c,
    game_files::Part::core_contingency,
    game_files::Part::battle_tactics,
    game_files::Part::extra,
    game_files::Part::music,
    game_files::Part::movies,
};

/// Returns the screen's row kind for one of the game's parts.
///
/// @param part the part
/// @return its row kind; PartKind::demo for the demo's installer
[[nodiscard]] view::PartKind part_kind(game_files::Part part) noexcept {
    switch (part) {
    case game_files::Part::game_archives:
        return view::PartKind::game_archives;
    case game_files::Part::update_31c:
        return view::PartKind::update_31c;
    case game_files::Part::core_contingency:
        return view::PartKind::core_contingency;
    case game_files::Part::battle_tactics:
        return view::PartKind::battle_tactics;
    case game_files::Part::extra:
        return view::PartKind::extra;
    case game_files::Part::music:
        return view::PartKind::music;
    case game_files::Part::movies:
        return view::PartKind::movies;
    case game_files::Part::mods:
        return view::PartKind::mod;
    case game_files::Part::other:
        return view::PartKind::left_out;
    case game_files::Part::demo:
        return view::PartKind::demo;
    }
    return view::PartKind::game_archives;
}

/// Joins a part's names for its row: "totala1.hpi, totala2.hpi, …".
///
/// @param summary the part's summary
/// @return the names, with an ellipsis when more files are in it than names
[[nodiscard]] std::string joined_names(const game_files::PartSummary& summary) {
    std::string text;
    for (const std::string& name : summary.names) {
        if (!text.empty())
            text += ", ";
        text += name;
    }
    if (summary.files > summary.names.size() && !text.empty())
        text += ", \xE2\x80\xA6";
    return text;
}

/// Turns a relative '/'-separated folder into its display form: "Games › Total Annihilation".
///
/// @param relative the folder
/// @return its display form
[[nodiscard]] std::string display_folder(std::string_view relative) {
    std::string text;
    for (const char character : relative) {
        if (character == '/')
            text += " \xE2\x80\xBA ";
        else
            text += character;
    }
    return text;
}

/// Returns the bytes free on the volume of a folder, or of its nearest existing parent.
///
/// @param hooks the platform's hooks
/// @param folder the folder
/// @return the bytes free; nothing when they cannot be read
[[nodiscard]] std::optional<uint64_t> free_space_at(const GameFilesHooks& hooks, fs::path folder) {
    std::error_code error;
    while (!folder.empty() && !fs::exists(folder, error)) {
        fs::path parent = folder.parent_path();
        if (parent == folder)
            break;
        folder = std::move(parent);
    }
    if (folder.empty())
        return std::nullopt;
    if (hooks.free_space != nullptr) {
        uint64_t bytes = 0;
        if (hooks.free_space(hooks.context, path_to_utf8(folder).c_str(), &bytes))
            return bytes;
        return std::nullopt;
    }
    const auto info = fs::space(folder, error);
    if (error)
        return std::nullopt;
    return static_cast<uint64_t>(info.available);
}

/// Sums the sizes of the files under a folder.
///
/// @param folder the folder
/// @return bytes; 0 when it cannot be read
[[nodiscard]] uint64_t folder_bytes(const fs::path& folder) {
    std::error_code error;
    if (folder.empty() || !fs::is_directory(folder, error))
        return 0;
    uint64_t total = 0;
    for (fs::recursive_directory_iterator
             entry(folder, fs::directory_options::skip_permission_denied, error),
         end;
         !error && entry != end;
         entry.increment(error)) {
        std::error_code entry_error;
        if (entry->is_regular_file(entry_error) && !entry_error) {
            const auto size = entry->file_size(entry_error);
            if (!entry_error)
                total += static_cast<uint64_t>(size);
        }
    }
    return total;
}

/// Reads the backups setting from the preferences file.
///
/// @param file the preferences file
/// @return true when the game files go into the device's backups
[[nodiscard]] bool read_backed_up(const fs::path& file) {
    try {
        const auto values = oa::platform::preferences::load(file);
        const auto found =
            values.find(std::string(oa::ui::engine_settings::key::game_files_backed_up));
        return found != values.end() && found->second == "1";
    } catch (const std::exception&) {
        return false;
    }
}

/// Where a row of the screen comes from, so that a press on it reaches the import.
struct RowSource {
    /// What the row shows.
    enum class Kind : uint8_t {
        part,      ///< one of the game's parts
        mod,       ///< a mod
        demo,      ///< the demo's installer
        demo_data, ///< the demo's unpacked data
    };
    Kind kind{Kind::part};                          ///< what the row shows
    game_files::Part part{game_files::Part::other}; ///< the part, for Kind::part
    uint32_t mod{};                                 ///< the mod's place, for Kind::mod
    std::string mod_id{};                           ///< the mod's id, in the management state
};

} // namespace

/// What the loop keeps between passes, and the controller between the screen's model and the
/// import.
struct GameFilesScreen::State {
    view::Model model{};                         ///< the screen's model
    view::Layout layout{};                       ///< the layout last painted
    view::Viewport viewport{};                   ///< the viewport last laid out
    paint::Canvas canvas{};                      ///< the canvas last painted
    oa::platform::text_font::FontStack* fonts{}; ///< the bundled fonts, when open
    game_files::RunSnapshot run{};               ///< the copy's latest snapshot
    game_files::ScanSnapshot scan{};             ///< the scan's latest snapshot
    bool quit{};                                 ///< the loop should end as a quit
    /// The family the text is drawn in: Barlow in the modern look (game_files_text_family).
    oa::platform::text_font::Family text_family{oa::platform::text_font::Family::sans};

    /// Makes the loop's state for a request.
    ///
    /// @param request what the screen was opened with
    explicit State(const GameFilesScreenRequest& request)
        : request_(request), hooks_(game_files_hooks()),
          frame_(request.renderer, "the Game files screen") {}

    /// Ends the workers and lets go of what the screen holds.
    ~State() {
        if (watching_)
            SDL_RemoveEventWatch(lifecycle_watch, this);
        if (scanner_)
            scanner_->cancel();
        if (runner_) {
            runner_->stop();
            runner_->join();
        }
        release_sources();
    }

    State(const State&) = delete;
    State& operator=(const State&) = delete;

    /// Opens the fonts and fills the first model.
    void start();
    /// Runs one pass of the loop.
    ///
    /// @param screen the check's view of the screen
    void pass(GameFilesScreen& screen);

    /// Tells whether the loop has ended, and how.
    ///
    /// @return the end; nothing while it runs
    [[nodiscard]] const std::optional<GameFilesEnd>& end() const noexcept { return end_; }

    /// Pushes a tap at a control's centre.
    ///
    /// @param control the control
    /// @return false when it is not on the layout
    bool tap(view::Control control);
    /// Takes a picker's answer.
    ///
    /// @param paths the chosen paths
    /// @param movable whether the engine may move them
    /// @param error why the picker failed; null when it did not
    void take_picker_answer(const std::vector<std::string>& paths, bool movable, const char* error);

    GameFilesScreen* check_screen_{}; ///< the check's view of the screen
    bool language_open{};             ///< the Language dialog is up
    bool picker_waiting{};            ///< a picker's answer is awaited

    std::size_t held_count() const noexcept { return picked_.size(); } ///< sources held

  private:

    // The loop.
    static bool SDLCALL lifecycle_watch(void* userdata, SDL_Event* event);
    void come_back() noexcept;
    void take_return();
    void leave_screen() noexcept;
    void handle_event(const SDL_Event& event);
    void refresh_viewport();
    void repaint();
    void present();
    void poll_workers();
    void poll_scan();
    void poll_run();
    void deliver_picker_answer();
    [[nodiscard]] int32_t wait_ms() const noexcept;

  public:

    /// How long the loop waits for an event in a player's run; -1 waits for an event alone.
    ///
    /// @return milliseconds, or -1
    [[nodiscard]] int32_t player_wait_ms() const noexcept;

  private:

    // Input.
    [[nodiscard]] bool accepted_touch(SDL_TouchID device) const noexcept;
    void take_pointer(const PointerEvent& pointer);
    void press_at(float x, float y, bool finger, SDL_TouchID touch, SDL_FingerID id);
    void move_to(float x, float y);
    void release_at(float x, float y);
    void take_key(const SDL_KeyboardEvent& key);
    void take_outcome(view::Outcome outcome);

    // The controller.
    void act(view::Outcome outcome);
    void show_picker(PickKind kind);
    void start_scan(SourceKind kind, const std::vector<std::string>& paths, bool movable);
    void take_plan(const game_files::ScanSnapshot& snapshot);
    void show_plan();
    void fill_plan_rows();
    void read_switches();
    void update_space();
    bool start_run();
    void take_checked(const game_files::RunSnapshot& snapshot);
    void take_run_failure(const game_files::RunSnapshot& snapshot);
    void take_stop();
    void check_copied(bool adopted);
    void manage_check();
    void go_home();
    void fill_first_run();
    void fill_manage();
    void fill_ready(const game_files::RunSnapshot& snapshot);
    void mirror_committed_game();
    void set_step(view::Step step);
    void show_problem(view::Problem problem, std::string detail = {}, std::string file = {});
    void release_sources();
    void open_language();
    void update_progress(const game_files::RunSnapshot& snapshot);
    [[nodiscard]] game_files::ImportMode run_mode() const noexcept;

    const GameFilesScreenRequest& request_; ///< what the screen was opened with
    GameFilesHooks hooks_{};                ///< the platform's hooks
    std::unique_ptr<text_font::FontStack> font_owner_{};
    view::Interaction interaction_{};
    std::optional<GameFilesEnd> end_{};
    std::optional<RenderState> render_state_{};
    WindowFrame frame_; ///< the frame shown on the window
    bool dirty_{true};
    bool progress_dirty_{};
    bool painted_{};
    bool presenting_{true};
    uint64_t last_paint_ns_{};
    uint32_t window_id_{};
    bool watching_{};

    // The press under way, and the control it went down on.
    WindowPress press_{};
    view::Control press_control_{};

    // The import.
    std::unique_ptr<game_files::SourceScan> scanner_{};
    std::unique_ptr<game_files::ImportRun> runner_{};
    game_files::ScanStage scan_stage_{game_files::ScanStage::listing};
    game_files::RunStage run_stage_{game_files::RunStage::idle};
    bool scan_taken_{true};
    bool run_taken_{true};
    std::shared_ptr<const game_files::ImportPlan> plan_{};
    game_files::Switches switches_{};
    std::vector<RowSource> rows_{};
    SourceKind scan_kind_{SourceKind::game_folder};
    PickKind pick_kind_{PickKind::game_folder};
    bool manage_replace_{};
    bool backed_up_{};
    fs::path old_folder_{};
    std::vector<std::string> picked_{};
    bool picked_movable_{};

    struct PickerAnswer {
        bool arrived{};
        std::vector<std::string> paths{};
        bool movable{};
        std::string error{};
    } answer_{};
    enum class Retry : uint8_t { none, picker, run, commit } retry_{Retry::none};
    game_files::CopiedFiles copied_{};
    std::optional<game_files::ImportState> waiting_{};
    bool resuming_{};
    bool stop_keep_{};
    bool expired_{};
    bool away_while_copying_{};
    bool came_back_{}; ///< the game came back to the screen; take_return has yet to run
    bool source_failed_check_{};
    std::string source_problem_{};
    std::deque<std::pair<uint64_t, uint64_t>> rate_{};
    uint64_t copy_started_ns_{};
    uint32_t wake_event_{};
};

// ---------------------------------------------------------------------------------------------
// The loop

void GameFilesScreen::State::start() {
    render_state_.emplace(request_.renderer);
    render_state_->use_window_pixels();
    window_id_ = SDL_GetWindowID(request_.window);
    // The system may suspend the game as soon as it says the game leaves the screen, so
    // presenting stops as the event is queued, not when the loop reads it.
    watching_ = SDL_AddEventWatch(lifecycle_watch, this);
    // One event type wakes the loop when a picker answers, registered once for every opening.
    static const uint32_t wake_event = SDL_RegisterEvents(1);
    wake_event_ = wake_event;
    try {
        font_owner_ = text_font::FontStack::open(text_font::bundled_font_directory());
    } catch (const std::exception& error) {
        std::cerr << "open-annihilation: the Game files screen's fonts could not be opened: "
                  << error.what() << '\n';
    }
    if (!font_owner_)
        std::cerr << "open-annihilation: the Game files screen shows no text: the bundled fonts "
                     "are missing\n";
    fonts = font_owner_.get();
    text_family = game_files_text_family(request_.preferences_file);
    backed_up_ = read_backed_up(
        request_.preferences_file.value_or(oa::platform::preferences::default_file())
    );
    if (request_.entry == GameFilesEntry::first_run)
        install_game_files_language(request_.preferences_file, request_.user_folder);
    else
        read_game_files_language_packs(request_.user_folder, request_.preferences_file);
    if (fonts != nullptr)
        use_game_files_language_fonts(*fonts);
    model.version = request_.version;
    model.management = request_.entry == GameFilesEntry::manage;
    const uint32_t capabilities =
        hooks_.capabilities != nullptr ? hooks_.capabilities(hooks_.context) : 0U;
    model.offers_pick_folder = has_capability(capabilities, GameFilesCapability::pick_folder);
    model.offers_pick_files = has_capability(capabilities, GameFilesCapability::pick_files);
    model.offers_copy_yourself =
        has_capability(capabilities, GameFilesCapability::shared_documents);
    for (std::size_t which = 0; which < model.words.size(); ++which) {
        const char* word = hooks_.text != nullptr
                               ? hooks_.text(hooks_.context, static_cast<GameFilesText>(which))
                               : nullptr;
        model.words[which] = word != nullptr ? word : "";
    }
    model.backed_up = backed_up_;
    if (request_.recovery.outcome == game_files::Recovery::copy_waiting && request_.recovery.state)
        waiting_ = request_.recovery.state;
    if (model.management) {
        fill_manage();
        set_step(view::Step::manage);
    } else {
        fill_first_run();
        if (model.banner == view::Banner::none && request_.needed.needed &&
            !request_.needed.problem.empty()) {
            model.banner = view::Banner::folder_refused;
            model.detail = request_.needed.problem;
        }
        if (request_.recovery.outcome == game_files::Recovery::unreadable) {
            model.banner = view::Banner::continue_copy;
            model.detail = request_.recovery.error;
            model.location.clear();
            model.stopped_bytes = game_files::staged_bytes(request_.paths.staging);
            model.stopped_total = 0;
        }
        set_step(view::Step::first_run);
    }
    refresh_viewport();
}

int32_t GameFilesScreen::State::wait_ms() const noexcept {
    if (request_.check.pass != nullptr)
        return check_wait_ms;
    return player_wait_ms();
}

int32_t GameFilesScreen::State::player_wait_ms() const noexcept {
    // A worker that has not reached a stage where it waits for the player is polled: it may
    // have finished after the last poll, with no event to wake the loop.
    const bool scan_pending = scanner_ && (scanner_->busy() || !scan_taken_ ||
                                           (scan_stage_ != game_files::ScanStage::nested &&
                                            scan_stage_ != game_files::ScanStage::already_there &&
                                            scan_stage_ != game_files::ScanStage::planned &&
                                            scan_stage_ != game_files::ScanStage::failed));
    const bool run_pending = runner_ && (runner_->busy() || !run_taken_ ||
                                         (run_stage_ != game_files::RunStage::checked &&
                                          run_stage_ != game_files::RunStage::stopped &&
                                          run_stage_ != game_files::RunStage::failed));
    if (scan_pending || run_pending || progress_dirty_)
        return worker_wait_ms;
    if (picker_waiting)
        return picker_wait_ms;
    // The renderer's first frames are counted until its start-up stage passes, so the screen
    // keeps presenting while it stands.
    if (request_.host != nullptr && request_.host->start_stage_open())
        return check_wait_ms;
    return -1;
}

void GameFilesScreen::State::pass(GameFilesScreen& screen) {
    SDL_Event event{};
    if (SDL_WaitEventTimeout(&event, dirty_ ? 0 : wait_ms())) {
        handle_event(event);
        while (!end_ && SDL_PollEvent(&event))
            handle_event(event);
    }
    if (end_)
        return;
    if (came_back_) {
        came_back_ = false;
        take_return();
    }
    deliver_picker_answer();
    poll_workers();
    refresh_viewport();
    repaint();
    present();
    if (request_.check.pass != nullptr && !end_)
        request_.check.pass(request_.check.context, screen);
    if (quit && !end_) {
        if (scanner_)
            scanner_->cancel();
        if (runner_ && runner_->busy()) {
            runner_->stop();
            runner_->join();
        }
        end_ = GameFilesEnd::quit;
    }
}

void GameFilesScreen::State::handle_event(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_QUIT:
    case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
        // A running copy is stopped and kept, as by an interruption: the next start offers
        // to continue it.
        quit = true;
        if (scanner_)
            scanner_->cancel();
        if (runner_ && runner_->busy()) {
            runner_->stop();
            runner_->join();
        }
        end_ = GameFilesEnd::quit;
        return;
    // Leaving and coming back arrive through lifecycle_watch only: SDL hands those events
    // to the watches as they happen and never queues them.
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED:
    case SDL_EVENT_WINDOW_SAFE_AREA_CHANGED:
    case SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED:
    case SDL_EVENT_WINDOW_EXPOSED:
    case SDL_EVENT_WINDOW_SHOWN:
    case SDL_EVENT_WINDOW_RESTORED:
        dirty_ = true;
        return;
    case SDL_EVENT_RENDER_DEVICE_RESET:
    case SDL_EVENT_RENDER_TARGETS_RESET:
        if (event.type == SDL_EVENT_RENDER_DEVICE_RESET)
            frame_.drop_texture();
        render_state_->use_window_pixels();
        dirty_ = true;
        return;
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_CANCELED:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
        take_pointer(read_pointer(event, viewport, press_));
        return;
    case SDL_EVENT_KEY_DOWN:
        take_key(event.key);
        return;
    default:
        if (event.type == wake_event_ && wake_event_ != 0)
            dirty_ = true;
        return;
    }
}

bool SDLCALL GameFilesScreen::State::lifecycle_watch(void* userdata, SDL_Event* event) {
    if (userdata == nullptr || event == nullptr || !SDL_IsMainThread())
        return true;
    if (event->type == SDL_EVENT_WILL_ENTER_BACKGROUND ||
        event->type == SDL_EVENT_DID_ENTER_BACKGROUND)
        static_cast<State*>(userdata)->leave_screen();
    else if (event->type == SDL_EVENT_DID_ENTER_FOREGROUND)
        static_cast<State*>(userdata)->come_back();
    return true;
}

void GameFilesScreen::State::come_back() noexcept {
    if (presenting_)
        return;
    // Drawing starts again at once, so that the Language dialog's own loop presents
    // too; the copy's banners wait for the screen's next pass.
    presenting_ = true;
    dirty_ = true;
    came_back_ = true;
    // The loop may be waiting with no timeout: wake it.
    if (wake_event_ != 0) {
        SDL_Event wake{};
        wake.type = wake_event_;
        SDL_PushEvent(&wake);
    }
}

void GameFilesScreen::State::take_return() {
    poll_workers();
    if (expired_) {
        expired_ = false;
        if (start_run())
            model.banner = view::Banner::copy_resumed;
    } else if (away_while_copying_ && model.step == view::Step::copying) {
        model.banner = view::Banner::copy_went_on;
    }
    away_while_copying_ = false;
}

void GameFilesScreen::State::leave_screen() noexcept {
    if (!presenting_)
        return;
    presenting_ = false;
    away_while_copying_ = runner_ && run_stage_ == game_files::RunStage::copying;
}

bool GameFilesScreen::State::accepted_touch(SDL_TouchID device) const noexcept {
    return device == game_files_check_touch_id || touch_screen(device);
}

void GameFilesScreen::State::take_pointer(const PointerEvent& pointer) {
    switch (pointer.step) {
    case PointerStep::none:
        return;
    case PointerStep::down:
        if (!pointer.finger || accepted_touch(pointer.touch))
            press_at(pointer.x, pointer.y, pointer.finger, pointer.touch, pointer.id);
        return;
    case PointerStep::move:
        move_to(pointer.x, pointer.y);
        return;
    case PointerStep::up:
        release_at(pointer.x, pointer.y);
        return;
    case PointerStep::cancel:
        take_outcome(view::press_up(model, interaction_, layout, view::Control{}));
        press_ = {};
        press_control_ = {};
        return;
    case PointerStep::wheel:
        take_outcome(view::scroll(model, layout, pointer.wheel_points));
        return;
    }
}

void GameFilesScreen::State::press_at(
    float x, float y, bool finger, SDL_TouchID touch, SDL_FingerID id
) {
    if (press_.active)
        return;
    const float reach = view::pick_reach_points * viewport.px_per_point;
    const view::Control control = view::hit_test(layout, canvas_point(x, y), reach);
    press_ = WindowPress{true, finger, touch, id, x, y, y, false};
    press_control_ = control;
    take_outcome(view::press_down(model, interaction_, layout, control));
    dirty_ = true;
}

void GameFilesScreen::State::move_to(float x, float y) {
    static_cast<void>(x);
    if (!press_.active)
        return;
    if (!press_.scrolling) {
        if (!press_scrolls(press_, layout.rows, y, viewport.px_per_point))
            return;
        // The press becomes a scroll: what it held is released as slid off.
        press_.scrolling = true;
        take_outcome(view::press_up(model, interaction_, layout, view::Control{}));
        press_.last_y = press_.start_y;
    }
    const float points = (press_.last_y - y) / std::max(viewport.px_per_point, 0.01F);
    press_.last_y = y;
    take_outcome(view::scroll(model, layout, points));
}

void GameFilesScreen::State::release_at(float x, float y) {
    if (!press_.active)
        return;
    const WindowPress finished = press_;
    const view::Control held = press_control_;
    press_ = {};
    press_control_ = {};
    if (finished.scrolling)
        return;
    const float reach = view::pick_reach_points * viewport.px_per_point;
    const view::Control under = view::hit_test(layout, canvas_point(x, y), reach);
    take_outcome(
        view::press_up(model, interaction_, layout, under == held ? under : view::Control{})
    );
    dirty_ = true;
}

void GameFilesScreen::State::take_key(const SDL_KeyboardEvent& key) {
    const bool shift = (key.mod & SDL_KMOD_SHIFT) != 0;
    const bool command = (key.mod & SDL_KMOD_GUI) != 0;
    std::optional<view::Key> meaning;
    switch (key.key) {
    case SDLK_TAB:
        meaning = shift ? view::Key::back_tab : view::Key::tab;
        break;
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        meaning = view::Key::enter;
        break;
    case SDLK_SPACE:
        meaning = view::Key::space;
        break;
    case SDLK_ESCAPE:
        meaning = view::Key::escape;
        break;
    case SDLK_PERIOD:
        if (command)
            meaning = view::Key::escape;
        break;
    case SDLK_UP:
        meaning = view::Key::up;
        break;
    case SDLK_DOWN:
        meaning = view::Key::down;
        break;
    case SDLK_PAGEUP:
        meaning = view::Key::page_up;
        break;
    case SDLK_PAGEDOWN:
        meaning = view::Key::page_down;
        break;
    default:
        break;
    }
    if (!meaning)
        return;
    take_outcome(view::key(model, interaction_, layout, *meaning));
    dirty_ = true;
}

void GameFilesScreen::State::take_outcome(view::Outcome outcome) {
    if (outcome.command == view::Command::none)
        return;
    dirty_ = true;
    // A switch flipped by the press is read back before anything acts on the plan.
    if (model.step == view::Step::ready_to_copy)
        read_switches();
    if (outcome.command != view::Command::redraw)
        act(outcome);
}

void GameFilesScreen::State::refresh_viewport() {
    if (read_window_viewport(request_.window, request_.renderer, viewport))
        dirty_ = true;
}

void GameFilesScreen::State::repaint() {
    painted_ = false;
    const uint64_t now = SDL_GetTicksNS();
    if (!dirty_ && !(progress_dirty_ && now - last_paint_ns_ >= progress_repaint_ns))
        return;
    if (viewport.width <= 0 || viewport.height <= 0)
        return;
    layout = view::lay_out(model, viewport, game_files_measure(fonts, text_family));
    // A shown focus that left the layout moves to its first control.
    const std::vector<oa::ui::kit::ControlId>& order = layout.list.tab_order;
    if (interaction_.focus_shown && !order.empty() &&
        std::find(order.begin(), order.end(), interaction_.focused) == order.end())
        interaction_.focused = order.front();
    oa::ui::kit::mark_states(layout.list, interaction_);
    if (canvas.width != viewport.width || canvas.height != viewport.height)
        canvas = paint::make_canvas(viewport.width, viewport.height);
    paint_game_files(canvas, layout, fonts, viewport.px_per_point, game_files_icon(), text_family);
    dirty_ = false;
    progress_dirty_ = false;
    painted_ = true;
    last_paint_ns_ = now;
}

void GameFilesScreen::State::present() {
    // While the renderer's start-up stage stands, the last frame is shown again each pass.
    const bool counting = request_.host != nullptr && request_.host->start_stage_open();
    if (!presenting_ || (!painted_ && !counting))
        return;
    static_cast<void>(frame_.show(canvas, *render_state_, request_.host));
}

// ---------------------------------------------------------------------------------------------
// The workers

void GameFilesScreen::State::poll_workers() {
    poll_scan();
    poll_run();
}

void GameFilesScreen::State::poll_scan() {
    if (!scanner_)
        return;
    game_files::ScanSnapshot snapshot = scanner_->snapshot();
    const bool moved = snapshot.stage != scan_stage_ || snapshot.files != scan.files ||
                       snapshot.bytes != scan.bytes;
    scan = snapshot;
    if (!moved && scan_taken_)
        return;
    const bool new_stage = snapshot.stage != scan_stage_ || !scan_taken_;
    scan_stage_ = snapshot.stage;
    scan_taken_ = true;
    switch (snapshot.stage) {
    case game_files::ScanStage::listing:
        if (model.step == view::Step::looking) {
            model.listed_files = snapshot.files;
            model.listed_bytes = snapshot.bytes;
            progress_dirty_ = true;
        }
        return;
    case game_files::ScanStage::nested:
        if (!new_stage)
            return;
        model.nested = snapshot.names.nested;
        set_step(view::Step::nested_offer);
        return;
    case game_files::ScanStage::already_there:
        if (new_stage)
            set_step(view::Step::already_there);
        return;
    case game_files::ScanStage::planned:
        if (new_stage)
            take_plan(snapshot);
        return;
    case game_files::ScanStage::failed:
        if (!new_stage)
            return;
        switch (snapshot.failure) {
        case game_files::ScanSnapshot::Failure::none:
            if (snapshot.error.empty()) {
                // Cancelled.
                release_sources();
                go_home();
                return;
            }
            show_problem(view::Problem::access_withdrawn, snapshot.error);
            return;
        case game_files::ScanSnapshot::Failure::unreadable:
            show_problem(view::Problem::access_withdrawn, snapshot.error);
            return;
        case game_files::ScanSnapshot::Failure::not_a_game:
            show_problem(
                view::Problem::not_a_game,
                snapshot.error.empty() ? describe_archive_problem(DemoSetup{}) : snapshot.error
            );
            return;
        case game_files::ScanSnapshot::Failure::not_demo_installer: {
            std::string file;
            if (!picked_.empty())
                file = path_to_utf8(path_from_utf8(picked_.front()).filename());
            show_problem(view::Problem::not_demo_installer, snapshot.error, file);
            return;
        }
        case game_files::ScanSnapshot::Failure::too_large:
            model.copy_bytes = snapshot.bytes;
            show_problem(view::Problem::too_large, snapshot.error);
            return;
        }
        return;
    }
}

void GameFilesScreen::State::poll_run() {
    if (!runner_)
        return;
    game_files::RunSnapshot snapshot = runner_->snapshot();
    const bool new_stage = snapshot.stage != run_stage_ || !run_taken_;
    run_stage_ = snapshot.stage;
    run_taken_ = true;
    run = snapshot;
    switch (snapshot.stage) {
    case game_files::RunStage::idle:
        return;
    case game_files::RunStage::copying:
        if (model.step != view::Step::copying && model.step != view::Step::problem)
            set_step(view::Step::copying);
        update_progress(snapshot);
        return;
    case game_files::RunStage::checking:
        if (new_stage) {
            update_progress(snapshot);
            set_step(view::Step::checking);
        }
        return;
    case game_files::RunStage::checked:
        if (new_stage)
            take_checked(snapshot);
        return;
    case game_files::RunStage::stopped:
        if (new_stage)
            take_stop();
        return;
    case game_files::RunStage::failed:
        if (new_stage)
            take_run_failure(snapshot);
        return;
    }
}

void GameFilesScreen::State::update_progress(const game_files::RunSnapshot& snapshot) {
    const uint64_t now = SDL_GetTicksNS();
    auto& progress = model.progress;
    const bool moved = progress.done_bytes != snapshot.bytes_done ||
                       progress.total_bytes != snapshot.bytes_total ||
                       progress.current != snapshot.current ||
                       progress.downloading != snapshot.current_remote;
    progress.done_bytes = snapshot.bytes_done;
    progress.total_bytes = snapshot.bytes_total;
    progress.current = snapshot.current;
    progress.downloading = snapshot.current_remote;
    if (rate_.empty() || rate_.back().second != snapshot.bytes_done)
        rate_.emplace_back(now, snapshot.bytes_done);
    while (rate_.size() > 2 && now - rate_.front().first > rate_window_ns)
        rate_.pop_front();
    // The time left: after 5 s, from the rate over the last 10 s; none while a download
    // is waited on with nothing moving.
    std::optional<uint32_t> left;
    if (now - copy_started_ns_ >= time_left_after_ns && rate_.size() >= 2) {
        const auto& [first_ns, first_bytes] = rate_.front();
        const double seconds = static_cast<double>(now - first_ns) / ns_per_second;
        const double rate =
            seconds > 0.0 ? static_cast<double>(snapshot.bytes_done - first_bytes) / seconds : 0.0;
        const bool waiting_on_download =
            snapshot.current_remote && snapshot.bytes_done == first_bytes;
        if (rate > 0.0 && !waiting_on_download && snapshot.bytes_total >= snapshot.bytes_done)
            left = static_cast<uint32_t>(std::min(
                static_cast<double>(snapshot.bytes_total - snapshot.bytes_done) / rate, 359999.0
            ));
    }
    if (left != progress.seconds_left) {
        progress.seconds_left = left;
        progress_dirty_ = true;
    }
    // The parts tick off as their files finish.
    for (std::size_t row = 0; row < rows_.size() && row < model.parts.size(); ++row) {
        const RowSource& source = rows_[row];
        auto& part = model.parts[row];
        uint8_t done = 0;
        if (source.kind == RowSource::Kind::part)
            done = snapshot.parts_done[static_cast<std::size_t>(source.part)];
        else if (source.kind == RowSource::Kind::mod && source.mod < snapshot.mods_done.size())
            done = snapshot.mods_done[source.mod];
        else if (source.kind == RowSource::Kind::demo)
            done = snapshot.parts_done[static_cast<std::size_t>(game_files::Part::demo)];
        view::Mark mark = done == 2   ? view::Mark::done
                          : done == 1 ? view::Mark::copying
                                      : view::Mark::waiting;
        if (!part.on || part.mark == view::Mark::missing)
            mark = part.mark == view::Mark::missing ? view::Mark::missing : view::Mark::none;
        if (part.mark != mark) {
            part.mark = mark;
            progress_dirty_ = true;
        }
    }
    if (moved)
        progress_dirty_ = true;
}

// ---------------------------------------------------------------------------------------------
// The picker

void GameFilesScreen::State::show_picker(PickKind kind) {
    pick_kind_ = kind;
    retry_ = Retry::picker;
    if (hooks_.show_picker == nullptr) {
        show_problem(view::Problem::picker_failed, "this system offers no file picker");
        return;
    }
    // A source held from before is let go: the new pick replaces it.
    if (!(runner_ && runner_->busy()))
        release_sources();
    picker_waiting = true;
    answer_ = {};
    hooks_.show_picker(
        hooks_.context,
        kind,
        [](void* userdata, const std::vector<std::string>& paths, bool movable, const char* error) {
            static_cast<State*>(userdata)->take_picker_answer(paths, movable, error);
        },
        this
    );
}

void GameFilesScreen::State::take_picker_answer(
    const std::vector<std::string>& paths, bool movable, const char* error
) {
    answer_.arrived = true;
    answer_.paths = paths;
    answer_.movable = movable;
    answer_.error = error != nullptr ? error : "";
    // The loop may be waiting with no timeout: wake it.
    if (wake_event_ != 0) {
        SDL_Event wake{};
        wake.type = wake_event_;
        SDL_PushEvent(&wake);
    }
}

void GameFilesScreen::State::deliver_picker_answer() {
    if (!answer_.arrived)
        return;
    PickerAnswer answer = std::move(answer_);
    answer_ = {};
    picker_waiting = false;
    dirty_ = true;
    if (!answer.error.empty()) {
        show_problem(view::Problem::picker_failed, answer.error);
        return;
    }
    if (answer.paths.empty())
        return; // Cancelled: nothing changes and nothing is said.
    picked_ = answer.paths;
    picked_movable_ = answer.movable;
    SourceKind kind = SourceKind::game_folder;
    switch (pick_kind_) {
    case PickKind::game_folder:
        kind = SourceKind::game_folder;
        break;
    case PickKind::demo_installer:
        kind = SourceKind::demo_installer;
        break;
    case PickKind::additions_folder:
        kind = SourceKind::additions_folder;
        break;
    case PickKind::archives:
        kind = SourceKind::archives;
        break;
    }
    start_scan(kind, answer.paths, answer.movable);
}

void GameFilesScreen::State::release_sources() {
    if (hooks_.release_source != nullptr)
        for (const std::string& path : picked_)
            hooks_.release_source(hooks_.context, path.c_str());
    picked_.clear();
}

// ---------------------------------------------------------------------------------------------
// The scan and the plan

void GameFilesScreen::State::start_scan(
    SourceKind kind, const std::vector<std::string>& paths, bool movable
) {
    scan_kind_ = kind;
    game_files::ScanRequest scan_request;
    scan_request.kind = kind;
    scan_request.paths = paths;
    scan_request.movable = movable;
    // Additions are checked layered over the game folder once copied, not on their own.
    scan_request.inspect_local =
        kind == SourceKind::game_folder || kind == SourceKind::demo_installer;
    scan_request.mod = request_.mod;
    scanner_ = std::make_unique<game_files::SourceScan>();
    std::string error;
    if (!scanner_->start(hooks_, request_.paths, std::move(scan_request), &error)) {
        scanner_.reset();
        show_problem(view::Problem::picker_failed, error);
        return;
    }
    scan = {};
    scan_stage_ = game_files::ScanStage::listing;
    scan_taken_ = false;
    plan_.reset();
    // Looking at the demo's installer names a file, not a folder.
    model.demo = kind == SourceKind::demo_installer;
    model.listed_files = 0;
    model.listed_bytes = 0;
    model.location =
        paths.empty() ? std::string{} : path_to_utf8(path_from_utf8(paths.front()).filename());
    set_step(view::Step::looking);
}

void GameFilesScreen::State::take_plan(const game_files::ScanSnapshot& snapshot) {
    plan_ = snapshot.plan;
    if (!plan_) {
        show_problem(view::Problem::access_withdrawn, snapshot.error);
        return;
    }
    switches_ = game_files::Switches{};
    switches_.mods.assign(plan_->mods.size(), 1);
    if (!plan_->location.empty())
        model.location = plan_->location;
    source_failed_check_ = snapshot.source_check && !usable(*snapshot.source_check);
    source_problem_ =
        source_failed_check_ ? describe_install_problem(*snapshot.source_check) : std::string{};
    model.source_checked = snapshot.source_check && usable(*snapshot.source_check);
    model.source_check_skipped = snapshot.source_check_skipped;
    // A copy that waits from an earlier start continues when the same folder was chosen.
    if (resuming_ && waiting_) {
        resuming_ = false;
        const game_files::ImportState& waiting = *waiting_;
        switches_.parts = waiting.parts;
        for (std::size_t mod = 0; mod < plan_->mods.size(); ++mod)
            if (std::find(
                    waiting.mods_off.begin(), waiting.mods_off.end(), plan_->mods[mod].folder
                ) != waiting.mods_off.end())
                switches_.mods[mod] = 0;
        uint64_t switched_bytes = 0;
        uint32_t switched_files = 0;
        for (const game_files::PlannedFile& file : plan_->files) {
            const bool on =
                file.part == game_files::Part::mods
                    ? (file.mod < switches_.mods.size() && switches_.mods[file.mod] != 0)
                    : switches_.parts[static_cast<std::size_t>(file.part)];
            if (!on)
                continue;
            ++switched_files;
            switched_bytes += file.size;
        }
        const bool same =
            (switched_files == waiting.files && switched_bytes == waiting.bytes) ||
            (plan_->files.size() == waiting.files && plan_->total_bytes == waiting.bytes);
        fill_plan_rows();
        update_space();
        if (same) {
            start_run();
            return;
        }
        show_problem(view::Problem::source_changed);
        return;
    }
    if (plan_->total_bytes > game_files::far_too_large_bytes) {
        fill_plan_rows();
        update_space();
        model.copy_bytes = plan_->total_bytes;
        show_problem(view::Problem::too_large);
        return;
    }
    show_plan();
}

void GameFilesScreen::State::show_plan() {
    if (!plan_)
        return;
    if (source_failed_check_) {
        show_problem(view::Problem::cannot_play, source_problem_);
        return;
    }
    fill_plan_rows();
    model.left_out.clear();
    model.left_out_bytes = 0;
    for (const game_files::LeftOutFile& file : plan_->left_out) {
        model.left_out.push_back(
            view::LeftOutRow{file.path, file.size, static_cast<uint8_t>(file.reason)}
        );
        model.left_out_bytes += file.size;
    }
    model.warnings = plan_->warnings;
    model.remote_bytes = plan_->remote_bytes;
    model.sizes_unknown = plan_->sizes_unknown;
    model.demo = plan_->demo;
    std::error_code error;
    const bool installed = fs::is_directory(request_.paths.game_folder, error);
    model.replace =
        (scan_kind_ == SourceKind::game_folder || scan_kind_ == SourceKind::demo_installer) &&
        installed && (plan_->has_archives || plan_->demo);
    model.replace_bytes = model.replace ? folder_bytes(request_.paths.game_folder) : 0;
    update_space();
    set_step(view::Step::ready_to_copy);
}

void GameFilesScreen::State::fill_plan_rows() {
    model.parts.clear();
    rows_.clear();
    if (!plan_)
        return;
    if (plan_->demo) {
        view::PartRow row;
        row.kind = view::PartKind::demo;
        row.bytes = plan_->total_bytes;
        row.count = static_cast<uint32_t>(plan_->files.size());
        row.mark = view::Mark::found;
        model.parts.push_back(row);
        rows_.push_back(RowSource{RowSource::Kind::demo, game_files::Part::demo, 0, {}});
        return;
    }
    const bool additions =
        scan_kind_ == SourceKind::additions_folder || scan_kind_ == SourceKind::archives;
    for (const game_files::Part part : shown_parts) {
        const game_files::PartSummary& summary = plan_->parts[static_cast<std::size_t>(part)];
        // An addition shows only what it holds; a game folder shows the game's parts, and the
        // extra archives only when there are some.
        if (!summary.found && (additions || part == game_files::Part::extra))
            continue;
        view::PartRow row;
        row.kind = part_kind(part);
        row.files = part == game_files::Part::music ? std::string{} : joined_names(summary);
        row.bytes = summary.bytes;
        row.bytes_known = summary.bytes_known;
        row.count = part == game_files::Part::music ? summary.music_tracks : summary.files;
        row.mark = summary.found ? view::Mark::found : view::Mark::missing;
        row.has_switch = summary.found && game_files::part_switchable(part);
        row.on = switches_.parts[static_cast<std::size_t>(part)];
        model.parts.push_back(row);
        rows_.push_back(RowSource{RowSource::Kind::part, part, 0, {}});
    }
    for (std::size_t mod = 0; mod < plan_->mods.size(); ++mod) {
        const game_files::ModFound& found = plan_->mods[mod];
        view::PartRow row;
        row.kind = view::PartKind::mod;
        row.name = !found.name.empty() ? found.name : !found.id.empty() ? found.id : found.folder;
        row.files = found.folder;
        row.errors = found.errors;
        row.mark = found.errors.empty() ? view::Mark::found : view::Mark::refused;
        row.has_switch = true;
        row.on = mod < switches_.mods.size() && switches_.mods[mod] != 0;
        for (const game_files::PlannedFile& file : plan_->files)
            if (file.part == game_files::Part::mods && file.mod == mod) {
                row.bytes += file.size;
                ++row.count;
                if (!file.size_known)
                    row.bytes_known = false;
            }
        model.parts.push_back(row);
        rows_.push_back(
            RowSource{
                RowSource::Kind::mod, game_files::Part::mods, static_cast<uint32_t>(mod), found.id
            }
        );
    }
}

void GameFilesScreen::State::read_switches() {
    bool changed = false;
    for (std::size_t row = 0; row < rows_.size() && row < model.parts.size(); ++row) {
        const view::PartRow& part = model.parts[row];
        if (!part.has_switch)
            continue;
        const RowSource& source = rows_[row];
        if (source.kind == RowSource::Kind::part) {
            bool& on = switches_.parts[static_cast<std::size_t>(source.part)];
            changed = changed || on != part.on;
            on = part.on;
        } else if (source.kind == RowSource::Kind::mod && source.mod < switches_.mods.size()) {
            const uint8_t on = part.on ? 1 : 0;
            changed = changed || switches_.mods[source.mod] != on;
            switches_.mods[source.mod] = on;
        }
    }
    if (changed)
        update_space();
}

void GameFilesScreen::State::update_space() {
    if (!plan_)
        return;
    const game_files::SpaceNeed need =
        game_files::space_need(hooks_, request_.paths, *plan_, switches_);
    model.copy_bytes = need.copy_bytes;
    model.need_bytes = need.need_bytes;
    model.free_bytes = need.free_bytes;
    model.free_known = need.free_known;
    model.space_short = need.free_known && !need.fits;
    model.fitting_off.clear();
    for (const game_files::Part part : need.fitting_off)
        model.fitting_off.push_back(part_kind(part));
    dirty_ = true;
}

game_files::ImportMode GameFilesScreen::State::run_mode() const noexcept {
    // The demo's installer chosen with Add files… joins the game folder as an addition.
    if (scan_kind_ == SourceKind::demo_installer && model.management && !manage_replace_)
        return game_files::ImportMode::add;
    if (plan_)
        return game_files::import_mode_of(*plan_);
    return scan_kind_ == SourceKind::additions_folder || scan_kind_ == SourceKind::archives
               ? game_files::ImportMode::add
               : game_files::ImportMode::replace;
}

bool GameFilesScreen::State::start_run() {
    if (!plan_)
        return false;
    retry_ = Retry::run;
    game_files::RunRequest run_request;
    run_request.mode = run_mode();
    run_request.mod = request_.mod;
    run_request.check = true;
    // A mod added is checked as the mod folder itself: the run finds its staged folder.
    if (run_request.mode == game_files::ImportMode::mod)
        run_request.mod.folder.clear();
    runner_ = std::make_unique<game_files::ImportRun>();
    std::string error;
    if (!runner_->start(hooks_, request_.paths, plan_, switches_, run_request, &error)) {
        runner_.reset();
        show_problem(view::Problem::staging_unwritable, error);
        return false;
    }
    run = {};
    run_stage_ = game_files::RunStage::idle;
    run_taken_ = false;
    stop_keep_ = false;
    expired_ = false;
    rate_.clear();
    copy_started_ns_ = SDL_GetTicksNS();
    model.progress = {};
    // A copy that continues starts from what is already staged; the space's copy bytes
    // leave the staged files out.
    model.progress.done_bytes = game_files::staged_bytes(request_.paths.staging);
    model.progress.total_bytes = model.progress.done_bytes + model.copy_bytes;
    log_import(
        "copying " + std::to_string(plan_->files.size()) + " files (" +
        std::to_string(model.copy_bytes) + " bytes) from " + plan_->location
    );
    if (model.progress.done_bytes > 0)
        log_import(
            "continuing: " + std::to_string(model.progress.done_bytes) +
            " bytes already copied are kept"
        );
    model.banner = view::Banner::none;
    for (auto& part : model.parts)
        if (part.on && part.mark != view::Mark::missing)
            part.mark = view::Mark::waiting;
    set_step(view::Step::copying);
    return true;
}

void GameFilesScreen::State::take_checked(const game_files::RunSnapshot& snapshot) {
    if (snapshot.files_skipped > 0)
        log_import(std::to_string(snapshot.files_skipped) + " files already copied were kept");
    if (!snapshot.check || !usable(*snapshot.check)) {
        log_import(
            "checked: cannot be played: " +
            (snapshot.check ? describe_install_problem(*snapshot.check) : snapshot.error)
        );
        show_problem(
            view::Problem::cannot_play,
            snapshot.check ? describe_install_problem(*snapshot.check) : snapshot.error
        );
        return;
    }
    log_import("checked: the copy can be played");
    const game_files::ImportMode mode = run_mode();
    if (mode == game_files::ImportMode::replace && manage_replace_) {
        // A replacement in the management state switches at the next start.
        std::string error;
        if (!game_files::schedule_replacement(request_.paths, &error)) {
            retry_ = Retry::commit;
            show_problem(view::Problem::staging_unwritable, error);
            return;
        }
        log_import("the copy replaces the game folder at the next start");
        manage_replace_ = false;
        release_sources();
        fill_manage();
        model.banner = view::Banner::next_start;
        set_step(view::Step::manage);
        return;
    }
    const game_files::CommitResult commit =
        game_files::commit_import(hooks_, request_.paths, mode, backed_up_);
    if (!commit.ok) {
        log_import("not committed: " + commit.error);
        retry_ = Retry::commit;
        show_problem(view::Problem::staging_unwritable, commit.error);
        return;
    }
    log_import(
        "committed to " + path_to_utf8(request_.paths.game_folder) +
        (commit.old_folder.empty()
             ? std::string{}
             : " (the earlier folder is kept as " + path_to_utf8(commit.old_folder) + ")")
    );
    release_sources();
    if (mode == game_files::ImportMode::add || mode == game_files::ImportMode::mod) {
        const uint32_t added =
            snapshot.files_total > 0 ? snapshot.files_total : snapshot.files_done;
        const std::string from = plan_ ? plan_->location : std::string{};
        const std::vector<std::string> kept = plan_ ? plan_->kept : std::vector<std::string>{};
        fill_manage();
        model.added_files = added;
        model.added_from = from;
        model.added_kept = kept;
        model.banner = view::Banner::added;
        set_step(view::Step::manage);
        return;
    }
    old_folder_ = commit.old_folder;
    fill_ready(snapshot);
    mirror_committed_game();
    model.banner = view::Banner::none;
    set_step(view::Step::ready_to_play);
}

void GameFilesScreen::State::mirror_committed_game() {
    // The check's own copy stays in the staged folder. A player's import is
    // written into the managed folders, and the staged folder is removed
    // only after that copy finishes, so the game is not kept twice.
    if (request_.check.pass != nullptr)
        return;
    fs::path user;
    try {
        const fs::path preferences = preference_file(request_.preferences_file);
        std::error_code error;
        const oa::platform::preferences::Values values =
            fs::exists(preferences, error) ? oa::platform::preferences::load(preferences)
                                           : oa::platform::preferences::Values{};
        std::string note;
        user = own_user_folder(
            request_.user_folder, request_.preferences_file, preferences, values, note
        );
    } catch (const std::exception& error) {
        log_import(std::string("the managed copy was not written: ") + error.what());
        return;
    }
    if (user.empty())
        return;
    try {
        const auto table = oa::data::content_bundles::bundles();
        FolderContentSource source(request_.paths.game_folder, table);
        CopyRequest copy;
        copy.user_folder = std::move(user);
        copy.bundles = table;
        copy.news = &std::cerr;
        const CopyResult result = copy_content(source, copy);
        if (result.status != CopyStatus::finished) {
            log_import("the managed copy was not written: " + result.error);
            return;
        }
        std::error_code failure;
        fs::remove_all(request_.paths.game_folder, failure);
        if (failure)
            log_import("the staged game folder could not be removed: " + failure.message());
        else
            log_import("the staged game folder was removed after the managed copy");
    } catch (const std::exception& error) {
        log_import(std::string("the managed copy was not written: ") + error.what());
    }
}

void GameFilesScreen::State::fill_ready(const game_files::RunSnapshot& snapshot) {
    const game_files::InstalledSummary installed =
        game_files::summarize_installed(hooks_, request_.paths);
    model.ready_parts = {};
    for (const game_files::Part part : shown_parts)
        model.ready_parts[static_cast<std::size_t>(part_kind(part))] =
            installed.parts[static_cast<std::size_t>(part)].found;
    model.ready_parts[static_cast<std::size_t>(view::PartKind::mod)] = !installed.mods.empty();
    model.ready_parts[static_cast<std::size_t>(view::PartKind::demo)] = installed.demo;
    model.demo = installed.demo;
    model.ready_summary = view::ready_text(model.ready_parts, installed.demo);
    model.uses_bytes = installed.bytes;
    const auto free = free_space_at(hooks_, request_.paths.game_folder);
    model.free_known = free.has_value();
    model.free_bytes = free.value_or(0);
    model.skipped_archives.clear();
    if (snapshot.check)
        for (const SkippedArchive& skipped : snapshot.check->skipped)
            model.skipped_archives.push_back(path_to_utf8(skipped.path.filename()));
    model.backed_up = backed_up_;
    model.old_folder_bytes = folder_bytes(old_folder_);
}

void GameFilesScreen::State::take_run_failure(const game_files::RunSnapshot& snapshot) {
    model.stopped_bytes = game_files::staged_bytes(request_.paths.staging);
    model.stopped_total = snapshot.bytes_total;
    const std::string file = snapshot.file.empty() ? snapshot.current : snapshot.file;
    log_import(
        "the copy stopped after " + std::to_string(model.stopped_bytes) +
        " bytes: " + snapshot.error + (file.empty() ? std::string{} : " (" + file + ")")
    );
    switch (snapshot.failure) {
    case game_files::RunFailure::none:
    case game_files::RunFailure::unreadable:
    case game_files::RunFailure::gone:
        show_problem(view::Problem::source_unreadable, snapshot.error, file);
        return;
    case game_files::RunFailure::no_space:
        show_problem(view::Problem::disk_full, snapshot.error, file);
        return;
    case game_files::RunFailure::offline:
        show_problem(view::Problem::download_failed, snapshot.error, file);
        return;
    case game_files::RunFailure::denied:
        show_problem(view::Problem::access_withdrawn, snapshot.error, file);
        return;
    case game_files::RunFailure::changed:
        show_problem(view::Problem::source_changed, snapshot.error, file);
        return;
    case game_files::RunFailure::staging_unwritable:
        show_problem(view::Problem::staging_unwritable, snapshot.error, file);
        return;
    case game_files::RunFailure::not_usable:
        show_problem(
            view::Problem::cannot_play,
            snapshot.check ? describe_install_problem(*snapshot.check) : snapshot.error
        );
        return;
    }
}

void GameFilesScreen::State::take_stop() {
    if (run.stopped_by_expiry && !stop_keep_) {
        // The system's time away ran out: the copy continues on return.
        log_import(
            "the copy stopped after " +
            std::to_string(game_files::staged_bytes(request_.paths.staging)) +
            " bytes when the time away ran out; it goes on when the game is back"
        );
        expired_ = true;
        return;
    }
    stop_keep_ = false;
    release_sources();
    const uint64_t staged = game_files::staged_bytes(request_.paths.staging);
    const uint64_t total = run.bytes_total;
    const std::string location = plan_ ? plan_->location : model.location;
    std::string error;
    const auto state = game_files::read_import_state(request_.paths.state_file, &error);
    waiting_ = state;
    log_import("the copy stopped after " + std::to_string(staged) + " bytes (kept)");
    go_home();
    model.banner = view::Banner::continue_copy;
    model.location = state && !state->location.empty() ? state->location : location;
    model.stopped_bytes = staged;
    model.stopped_total = state ? state->bytes : total;
}

// ---------------------------------------------------------------------------------------------
// Steps

void GameFilesScreen::State::set_step(view::Step step) {
    if (model.step != step) {
        model.scroll_points = 0;
        interaction_.pressed = oa::ui::kit::no_control;
    }
    model.step = step;
    model.sheet = view::Sheet::none;
    dirty_ = true;
}

void GameFilesScreen::State::show_problem(
    view::Problem problem, std::string detail, std::string file
) {
    model.problem = problem;
    model.detail = std::move(detail);
    model.file = std::move(file);
    set_step(view::Step::problem);
}

void GameFilesScreen::State::go_home() {
    if (model.management) {
        fill_manage();
        set_step(view::Step::manage);
    } else {
        fill_first_run();
        set_step(view::Step::first_run);
    }
}

void GameFilesScreen::State::fill_first_run() {
    model.banner = view::Banner::none;
    model.detail.clear();
    model.parts.clear();
    rows_.clear();
    const auto free = free_space_at(hooks_, request_.paths.game_folder);
    model.free_known = free.has_value();
    model.free_bytes = free.value_or(0);
    // A copy that was stopped, by the player or by an interruption, offers to continue.
    std::string error;
    const auto state = game_files::read_import_state(request_.paths.state_file, &error);
    if (state && state->phase == game_files::ImportPhase::copying) {
        waiting_ = state;
        model.banner = view::Banner::continue_copy;
        model.location = state->location;
        model.stopped_bytes = game_files::staged_bytes(request_.paths.staging);
        model.stopped_total = state->bytes;
    } else if (!state) {
        waiting_.reset();
    }
}

void GameFilesScreen::State::fill_manage() {
    const game_files::InstalledSummary installed =
        game_files::summarize_installed(hooks_, request_.paths);
    model.banner = view::Banner::none;
    model.parts.clear();
    rows_.clear();
    for (const game_files::Part part : shown_parts) {
        const game_files::PartSummary& summary = installed.parts[static_cast<std::size_t>(part)];
        if (!summary.found)
            continue;
        view::PartRow row;
        row.kind = part_kind(part);
        row.files = part == game_files::Part::music ? std::string{} : joined_names(summary);
        row.bytes = summary.bytes;
        row.bytes_known = summary.bytes_known;
        row.count = part == game_files::Part::music ? summary.music_tracks : summary.files;
        row.mark = view::Mark::found;
        // The game archives go only with everything (Remove all GAME FILES).
        row.removable = part != game_files::Part::game_archives;
        model.parts.push_back(row);
        rows_.push_back(RowSource{RowSource::Kind::part, part, 0, {}});
    }
    for (std::size_t mod = 0; mod < installed.mods.size(); ++mod) {
        const game_files::ModFound& found = installed.mods[mod];
        view::PartRow row;
        row.kind = view::PartKind::mod;
        row.name = !found.name.empty() ? found.name : found.id;
        row.files = "mods/" + found.id;
        row.bytes = folder_bytes(request_.paths.game_folder / "mods" / path_from_utf8(found.id));
        row.errors = found.errors;
        row.mark = found.errors.empty() ? view::Mark::found : view::Mark::refused;
        row.removable = true;
        model.parts.push_back(row);
        rows_.push_back(
            RowSource{
                RowSource::Kind::mod, game_files::Part::mods, static_cast<uint32_t>(mod), found.id
            }
        );
    }
    if (installed.demo) {
        view::PartRow row;
        row.kind = view::PartKind::demo;
        row.mark = view::Mark::found;
        model.parts.push_back(row);
        rows_.push_back(RowSource{RowSource::Kind::demo, game_files::Part::demo, 0, {}});
    }
    if (installed.demo_data_unused && installed.demo_data_bytes > 0) {
        view::PartRow row;
        row.kind = view::PartKind::demo_data;
        row.bytes = installed.demo_data_bytes;
        row.mark = view::Mark::warning;
        row.removable = true;
        model.parts.push_back(row);
        rows_.push_back(RowSource{RowSource::Kind::demo_data, game_files::Part::demo, 0, {}});
    }
    model.installed_bytes = installed.bytes;
    model.old_folder_bytes = installed.old_folder_bytes;
    const auto free = free_space_at(hooks_, request_.paths.game_folder);
    model.free_known = free.has_value();
    model.free_bytes = free.value_or(0);
    model.pending_replacement = false;
    model.pending_removals.clear();
    std::string error;
    const auto state = game_files::read_import_state(request_.paths.state_file, &error);
    if (state) {
        if (state->mode == game_files::ImportMode::remove) {
            model.pending_removals = state->removals;
            model.banner = view::Banner::next_start;
        } else if (
            state->mode == game_files::ImportMode::replace &&
            state->phase == game_files::ImportPhase::checked
        ) {
            model.pending_replacement = true;
            model.banner = view::Banner::next_start;
        } else if (state->phase == game_files::ImportPhase::copying) {
            waiting_ = state;
            model.banner = view::Banner::continue_copy;
            model.location = state->location;
            model.stopped_bytes = game_files::staged_bytes(request_.paths.staging);
            model.stopped_total = state->bytes;
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Commands

void GameFilesScreen::State::act(view::Outcome outcome) {
    switch (outcome.command) {
    case view::Command::none:
    case view::Command::redraw:
        return;
    case view::Command::pick_game_folder:
        // From the continue banner, the same folder continues the copy that waits.
        resuming_ = waiting_.has_value() && model.banner == view::Banner::continue_copy &&
                    (model.step == view::Step::first_run || model.step == view::Step::manage);
        show_picker(PickKind::game_folder);
        return;
    case view::Command::pick_installer:
        resuming_ = false;
        show_picker(PickKind::demo_installer);
        return;
    case view::Command::pick_additions_folder:
        resuming_ = false;
        manage_replace_ = false;
        show_picker(PickKind::additions_folder);
        return;
    case view::Command::pick_archives:
        resuming_ = false;
        manage_replace_ = false;
        show_picker(PickKind::archives);
        return;
    case view::Command::check_copied:
        check_copied(false);
        return;
    case view::Command::cancel_scan:
        if (scanner_)
            scanner_->cancel();
        scanner_.reset();
        scan_taken_ = true;
        release_sources();
        go_home();
        return;
    case view::Command::use_nested: {
        std::string error;
        if (!scanner_ || !scanner_->use_nested(outcome.index, &error)) {
            show_problem(view::Problem::access_withdrawn, error);
            return;
        }
        if (outcome.index < model.nested.size())
            model.location = display_folder(model.nested[outcome.index]);
        // The scan starts over below: its snapshot is a fresh listing.
        scan_stage_ = game_files::ScanStage::listing;
        scan_taken_ = false;
        set_step(view::Step::looking);
        return;
    }
    case view::Command::check_in_place:
        if (model.management) {
            manage_check();
            return;
        }
        scanner_.reset();
        release_sources();
        end_ = GameFilesEnd::play;
        return;
    case view::Command::recheck_space:
        update_space();
        if (model.step == view::Step::problem && model.problem == view::Problem::short_space &&
            !model.space_short)
            set_step(view::Step::ready_to_copy);
        return;
    case view::Command::start_copy:
        if (model.space_short) {
            show_problem(view::Problem::short_space);
            return;
        }
        start_run();
        return;
    case view::Command::copy_anyway:
        show_plan();
        return;
    case view::Command::stop_keep:
        if (runner_) {
            stop_keep_ = true;
            runner_->stop();
            runner_->join();
            poll_run();
            // A copy that finished before the stop arrived goes on as finished.
            if (run.stage == game_files::RunStage::stopped && model.step != view::Step::first_run &&
                model.step != view::Step::manage)
                take_stop();
        }
        return;
    case view::Command::stop_discard: {
        if (runner_) {
            runner_->stop();
            runner_->join();
        }
        runner_.reset();
        run_taken_ = true;
        std::string error;
        static_cast<void>(game_files::discard_import(request_.paths, &error));
        log_import("the copy stopped (discarded)");
        release_sources();
        waiting_.reset();
        go_home();
        model.banner = view::Banner::nothing_added;
        return;
    }
    case view::Command::continue_copy:
        start_run();
        return;
    case view::Command::start_again: {
        std::string error;
        static_cast<void>(game_files::discard_import(request_.paths, &error));
        waiting_.reset();
        start_run();
        return;
    }
    case view::Command::discard: {
        if (runner_ && runner_->busy()) {
            runner_->stop();
            runner_->join();
        }
        runner_.reset();
        run_taken_ = true;
        std::string error;
        static_cast<void>(game_files::discard_import(request_.paths, &error));
        release_sources();
        waiting_.reset();
        go_home();
        model.banner = view::Banner::none;
        return;
    }
    case view::Command::adopt: {
        std::string error;
        if (!game_files::adopt_copied_files(request_.paths, copied_, &error)) {
            show_problem(view::Problem::staging_unwritable, error);
            return;
        }
        check_copied(true);
        return;
    }
    case view::Command::play:
        if (model.management) {
            go_home();
            return;
        }
        end_ = GameFilesEnd::play;
        return;
    case view::Command::remove_old: {
        std::string error;
        if (!old_folder_.empty() && game_files::remove_folder_now(old_folder_, &error)) {
            old_folder_.clear();
            model.old_folder_bytes = 0;
        } else if (!error.empty()) {
            std::cerr << "open-annihilation: the earlier game folder was not removed: " << error
                      << '\n';
        }
        dirty_ = true;
        return;
    }
    case view::Command::back:
        if (scanner_)
            scanner_->cancel();
        scanner_.reset();
        scan_taken_ = true;
        if (!(runner_ && runner_->busy()))
            release_sources();
        resuming_ = false;
        go_home();
        return;
    case view::Command::open_language:
        open_language();
        return;
    case view::Command::manage_check:
        manage_check();
        return;
    case view::Command::manage_remove: {
        if (outcome.index >= rows_.size())
            return;
        const RowSource& source = rows_[outcome.index];
        if (source.kind == RowSource::Kind::demo_data) {
            act(view::Outcome{view::Command::remove_demo_data, outcome.index});
            return;
        }
        std::vector<std::string> files;
        if (source.kind == RowSource::Kind::mod)
            files = game_files::part_files(
                hooks_, request_.paths, game_files::Part::mods, source.mod_id
            );
        else
            files = game_files::part_files(hooks_, request_.paths, source.part);
        std::string error;
        if (files.empty() || !game_files::schedule_removal(request_.paths, files, &error)) {
            if (!error.empty())
                show_problem(view::Problem::staging_unwritable, error);
            return;
        }
        fill_manage();
        model.banner = view::Banner::next_start;
        set_step(view::Step::manage);
        return;
    }
    case view::Command::manage_remove_all: {
        const std::array<std::string, 1> everything{"*"};
        std::string error;
        if (!game_files::schedule_removal(request_.paths, everything, &error)) {
            show_problem(view::Problem::staging_unwritable, error);
            return;
        }
        fill_manage();
        model.banner = view::Banner::next_start;
        set_step(view::Step::manage);
        return;
    }
    case view::Command::manage_replace:
        manage_replace_ = true;
        resuming_ = false;
        show_picker(PickKind::game_folder);
        return;
    case view::Command::manage_cancel_pending: {
        std::string error;
        if (!game_files::cancel_scheduled(request_.paths, &error))
            std::cerr << "open-annihilation: the waiting change was not cancelled: " << error
                      << '\n';
        fill_manage();
        set_step(view::Step::manage);
        return;
    }
    case view::Command::remove_demo_data: {
        std::string error;
        const fs::path demo_data = demo_bundle_directory(request_.paths.data_folder);
        if (!game_files::remove_folder_now(demo_data, &error))
            std::cerr << "open-annihilation: the demo's data was not removed: " << error << '\n';
        fill_manage();
        set_step(view::Step::manage);
        return;
    }
    case view::Command::retry:
        switch (retry_) {
        case Retry::picker:
        case Retry::none:
            show_picker(pick_kind_);
            return;
        case Retry::run:
            start_run();
            return;
        case Retry::commit:
            take_checked(run);
            return;
        }
        return;
    case view::Command::done:
        end_ = GameFilesEnd::done;
        return;
    }
}

void GameFilesScreen::State::check_copied(bool adopted) {
    copied_ = game_files::find_copied_files(request_.paths);
    switch (copied_.find) {
    case game_files::CopiedFind::game_folder:
        // Resolution checks the folder; a refusal comes back as S1's banner.
        end_ = GameFilesEnd::play;
        return;
    case game_files::CopiedFind::misnamed_folder:
        if (adopted) {
            show_problem(view::Problem::no_game_folder_yet);
            return;
        }
        show_problem(view::Problem::found_misnamed, {}, copied_.folder);
        return;
    case game_files::CopiedFind::loose_archives: {
        if (adopted) {
            show_problem(view::Problem::no_game_folder_yet);
            return;
        }
        std::string names;
        for (const std::string& name : copied_.archives) {
            if (!names.empty())
                names += ", ";
            names += name;
        }
        show_problem(view::Problem::found_loose, {}, names);
        return;
    }
    case game_files::CopiedFind::nothing:
        show_problem(view::Problem::no_game_folder_yet);
        return;
    }
}

void GameFilesScreen::State::manage_check() {
    const GameInstall install = inspect_game_install(
        request_.paths.game_folder, request_.paths.data_folder, demo_1997, request_.mod
    );
    if (!usable(install)) {
        show_problem(view::Problem::cannot_play, describe_install_problem(install));
        return;
    }
    game_files::RunSnapshot checked;
    checked.check = install;
    old_folder_.clear();
    fill_ready(checked);
    model.old_folder_bytes = 0;
    model.banner = view::Banner::none;
    set_step(view::Step::ready_to_play);
}

void GameFilesScreen::State::open_language() {
    language_open = true;
    GameFilesLanguageRequest dialog;
    dialog.under = &canvas;
    dialog.fonts = fonts;
    dialog.version = request_.version;
    dialog.preferences_file = request_.preferences_file;
    dialog.user_folder = request_.user_folder;
    dialog.hooks.context = this;
    dialog.hooks.present = [](void* context, const paint::Canvas& frame) {
        auto& state = *static_cast<State*>(context);
        state.canvas = frame;
        state.painted_ = true;
        state.present();
    };
    dialog.hooks.viewport = [](void* context) {
        auto& state = *static_cast<State*>(context);
        state.refresh_viewport();
        return state.viewport;
    };
    if (request_.check.pass != nullptr)
        dialog.hooks.pass = [](void* context) {
            auto& state = *static_cast<State*>(context);
            if (state.check_screen_ != nullptr)
                state.request_.check.pass(state.request_.check.context, *state.check_screen_);
        };
    // The screen's own frame lies under the dialog.
    repaint();
    static_cast<void>(run_game_files_language_dialog(dialog));
    language_open = false;
    dirty_ = true;
    // The words may be in another language now.
    for (std::size_t which = 0; which < model.words.size(); ++which) {
        const char* word = hooks_.text != nullptr
                               ? hooks_.text(hooks_.context, static_cast<GameFilesText>(which))
                               : nullptr;
        model.words[which] = word != nullptr ? word : "";
    }
}

bool GameFilesScreen::State::tap(view::Control control) {
    // The topmost part of the control: a sheet's lies above what it covers.
    const oa::ui::kit::Item* found = nullptr;
    for (auto part = layout.list.items.rbegin(); part != layout.list.items.rend(); ++part)
        if (view::screen_control(part->control) == control) {
            found = &*part;
            break;
        }
    if (found == nullptr || viewport.width <= 0 || viewport.height <= 0)
        return false;
    const float x =
        (static_cast<float>(found->rect.x) + static_cast<float>(found->rect.width) * 0.5F) /
        static_cast<float>(viewport.width);
    const float y =
        (static_cast<float>(found->rect.y) + static_cast<float>(found->rect.height) * 0.5F) /
        static_cast<float>(viewport.height);
    for (const SDL_EventType type : {SDL_EVENT_FINGER_DOWN, SDL_EVENT_FINGER_UP}) {
        SDL_Event event{};
        event.type = type;
        event.tfinger.timestamp = SDL_GetTicksNS();
        event.tfinger.touchID = game_files_check_touch_id;
        event.tfinger.fingerID = game_files_check_finger_id;
        event.tfinger.x = x;
        event.tfinger.y = y;
        event.tfinger.pressure = type == SDL_EVENT_FINGER_UP ? 0.0F : 1.0F;
        event.tfinger.windowID = window_id_;
        SDL_PushEvent(&event);
    }
    return true;
}

GameFilesEnd run_game_files_screen(const GameFilesScreenRequest& request) {
    if (request.window == nullptr || request.renderer == nullptr)
        return GameFilesEnd::quit;
    GameFilesScreen::State state(request);
    GameFilesScreen screen(state);
    state.check_screen_ = &screen;
    state.start();
    while (!state.end())
        state.pass(screen);
    return *state.end();
}

GameFilesScreen::GameFilesScreen(State& state) noexcept : state_(&state) {
}

const oa::ui::game_files::Model& GameFilesScreen::model() const noexcept {
    return state_->model;
}

const oa::ui::game_files::Layout& GameFilesScreen::layout() const noexcept {
    return state_->layout;
}

const oa::ui::game_files::Viewport& GameFilesScreen::viewport() const noexcept {
    return state_->viewport;
}

const paint::Canvas& GameFilesScreen::canvas() const noexcept {
    return state_->canvas;
}

oa::platform::text_font::FontStack* GameFilesScreen::fonts() const noexcept {
    return state_->fonts;
}

bool GameFilesScreen::tap(oa::ui::game_files::Control control) {
    return state_->tap(control);
}

void GameFilesScreen::press_key(SDL_Keycode key, SDL_Keymod modifiers) {
    for (const SDL_EventType type : {SDL_EVENT_KEY_DOWN, SDL_EVENT_KEY_UP}) {
        SDL_Event event{};
        event.type = type;
        event.key.timestamp = SDL_GetTicksNS();
        event.key.key = key;
        event.key.mod = modifiers;
        event.key.scancode = SDL_GetScancodeFromKey(key, nullptr);
        event.key.down = type == SDL_EVENT_KEY_DOWN;
        SDL_PushEvent(&event);
    }
}

const game_files::RunSnapshot& GameFilesScreen::run() const noexcept {
    return state_->run;
}

const game_files::ScanSnapshot& GameFilesScreen::scan() const noexcept {
    return state_->scan;
}

void GameFilesScreen::request_quit() noexcept {
    state_->quit = true;
}

bool GameFilesScreen::language_open() const noexcept {
    return state_->language_open;
}

void GameFilesScreen::answer_picker(
    const std::vector<std::string>& paths, bool movable, const char* error
) {
    state_->take_picker_answer(paths, movable, error);
}

bool GameFilesScreen::picker_open() const noexcept {
    return state_->picker_waiting;
}

std::size_t GameFilesScreen::held_sources() const noexcept {
    return state_->held_count();
}

bool GameFilesScreen::waits_for_events_only() const noexcept {
    return state_->player_wait_ms() < 0;
}

} // namespace oa::app
