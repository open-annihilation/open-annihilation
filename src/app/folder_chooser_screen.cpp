// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The in-engine game folder chooser's loop, its listing of folders and its
// painting (folder_chooser_screen.hpp): events from fingers, the pointer,
// the wheel, the keys and gamepads mapped to the chooser's presses and keys
// (src/ui/folder-chooser), the folders found and the browsed folder checked
// with the engine's own inspection, the layout painted in the Game files
// screen's look and presented on the game's window. The window's canvas, the
// frame shown on it and the pointer read in canvas pixels are the Game files
// screen's too (window_pixels.hpp); the gamepads are the chooser's own.
#include "folder_chooser_screen.hpp"

#include "game_files_paint.hpp"
#include "game_files_screen.hpp"
#include "render_host.hpp"
#include "render_state.hpp"
#include "window_pixels.hpp"

#include "oa/app/demo_installer.hpp"
#include "oa/app/game_files_hooks.hpp"
#include "oa/app/game_files_import.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/platform/game_installs.hpp"
#include "oa/platform/machine.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/ui/folder_chooser.hpp"
#include "oa/ui/game_files.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/pad_controls.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace paint = oa::ui::paint;
namespace kit = oa::ui::kit;
namespace chooser = oa::ui::folder_chooser;
namespace view = oa::ui::game_files;
namespace installs = oa::platform::game_installs;
namespace text_font = oa::platform::text_font;

/// How long the loop waits for an event while a held direction or a stick needs its timer,
/// or while the renderer's start-up frames are counted, in milliseconds.
constexpr int32_t frame_wait_ms = 16;
/// The most entries of a folder the browser reads.
constexpr std::size_t browse_listing_limit = 8192;
/// The most files and folders the browser lists.
constexpr std::size_t most_browse_entries = 1000;
/// The most files read inside one folder while saying what it holds.
constexpr std::size_t most_child_files = 64;
/// The largest value of a gamepad's stick axis.
constexpr float stick_axis_range = 32767.0F;
/// The share of the left stick's travel past which it moves the focus.
constexpr float stick_on_share = 0.5F;
/// The share of the left stick's travel under which a held direction ends.
constexpr float stick_off_share = 0.3F;
/// The share of the right stick's travel it rests within without scrolling.
constexpr float scroll_dead_share = 0.2F;
/// How fast the right stick pushed fully scrolls the rows, in points a second.
constexpr float stick_scroll_points_per_second = 1200.0F;
/// Milliseconds in a second.
constexpr float ms_per_second = 1000.0F;
/// The most characters the filter keeps.
constexpr std::size_t most_filter_characters = 64;

/// A direction the focus moves in.
enum class Direction : uint8_t {
    none,  ///< no direction
    up,    ///< up
    down,  ///< down
    left,  ///< left
    right, ///< right
};

/// What holds a direction down.
enum class HoldSource : uint8_t {
    dpad,  ///< the D-pad
    stick, ///< the left stick
};

/// Returns the chooser's key for a direction.
///
/// @param direction the direction, not none
/// @return the key
chooser::Key key_of(Direction direction) noexcept {
    switch (direction) {
    case Direction::up:
        return chooser::Key::up;
    case Direction::down:
        return chooser::Key::down;
    case Direction::left:
        return chooser::Key::left;
    case Direction::right:
    case Direction::none:
        break;
    }
    return chooser::Key::right;
}

/// Returns the direction a D-pad button stands for.
///
/// @param button the button
/// @return the direction; none for another button
Direction direction_of(uint8_t button) noexcept {
    switch (static_cast<SDL_GamepadButton>(button)) {
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return Direction::up;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return Direction::down;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return Direction::left;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return Direction::right;
    default:
        break;
    }
    return Direction::none;
}

/// Returns one of the locator's words in the installed language.
///
/// @param english the words as the source writes them
/// @return their translation, or the words themselves
std::string tr(std::string_view english) {
    return std::string(oa::data::languages::interface_text(english));
}

/// Tells whether nobody watches a run: --unattended and the scripted runs, a headless check,
/// CI, or a dummy or offscreen video driver.
///
/// @param options the parsed command line
/// @return true when nobody can answer the chooser
bool nobody_watches(const Options& options) {
    const char* ci = SDL_getenv("CI");
    const char* driver = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    return options.unattended || options.headless_check ||
           unattended_environment(
               ci == nullptr ? std::string_view{} : std::string_view(ci),
               driver == nullptr ? std::string_view{} : std::string_view(driver)
           );
}

/// A folder as an absolute path with a trailing separator removed.
///
/// @param folder the folder
/// @return the absolute folder
fs::path absolute_folder(const fs::path& folder) {
    std::error_code error;
    fs::path shown = fs::absolute(folder, error).lexically_normal();
    if (error)
        shown = folder;
    if (!shown.has_filename() && shown.has_relative_path())
        shown = shown.parent_path();
    return shown;
}

/// Makes an empty file of a size, sparse where the file system allows it.
///
/// @param path the file
/// @param size its size, in bytes
/// @return true when the file has that size
bool sparse_file(const fs::path& path, uint64_t size) {
    std::error_code error;
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path(), error);
    if (error)
        return false;
    {
        std::ofstream created(path, std::ios::binary | std::ios::trunc);
        if (!created)
            return false;
    }
    fs::resize_file(path, static_cast<uintmax_t>(size), error);
    return !error;
}

/// The scratch home the picture check browses, and the places it shows. The places are the
/// fixed ones the tests use. Nothing outside `root` is read or created.
struct PictureScene {
    fs::path root{};                  ///< removed when the check ends
    fs::path home{};                  ///< Desktop, Documents, Downloads and Games
    fs::path install{};               ///< Games/Total Annihilation
    installs::LocatorFacts desktop{}; ///< the desktop sidebar
    installs::LocatorFacts deck{};    ///< one card and one Steam prefix
};

/// Builds the scratch home: the three usual folders, a Total Annihilation of empty files
/// named and sized from the built-in table, and the Windows installer beside it.
///
/// @param root where the home is made; replaced if it is already there
/// @param[out] scene the home, the install and the sidebars
/// @return true when the files were made
bool build_picture_scene(const fs::path& root, PictureScene& scene) {
    namespace bundles = oa::data::content_bundles;
    std::error_code error;
    fs::remove_all(root, error);
    error.clear();
    scene.root = root;
    const fs::path home = root / "home";
    fs::create_directories(home / "Desktop", error);
    if (error)
        return false;
    fs::create_directories(home / "Documents", error);
    if (error)
        return false;
    fs::create_directories(home / "Downloads", error);
    if (error)
        return false;
    const fs::path games = home / "Games";
    const fs::path install = games / "Total Annihilation";
    fs::create_directories(install, error);
    if (error)
        return false;
    for (const bundles::Bundle& bundle : bundles::bundles()) {
        for (const bundles::ContentFile& file : bundle.files) {
            if (!sparse_file(install / std::string(file.copy_name), file.size))
                return false;
        }
    }
    const bundles::Installer* windows = nullptr;
    for (const bundles::Installer& installer : bundles::installers()) {
        if (installer.kind == bundles::InstallerKind::windows)
            windows = &installer;
    }
    if (windows == nullptr || !sparse_file(games / std::string(windows->file_name), windows->size))
        return false;
    scene.home = absolute_folder(home);
    scene.install = absolute_folder(install);
    scene.desktop.home = scene.home;
    scene.desktop.desktop = scene.home / "Desktop";
    scene.desktop.documents = scene.home / "Documents";
    scene.desktop.downloads = scene.home / "Downloads";
    scene.desktop.applications = "/Applications";
    scene.desktop.volumes = {"/", "/Volumes/TOTALA"};
    scene.desktop.steam = "/home/play/.local/share/Steam/steamapps/compatdata/298030/pfx";
    scene.desktop.heroic = "/home/play/Games/Heroic";
    scene.deck.deck = true;
    scene.deck.home = scene.home;
    scene.deck.sd_cards = {"/run/media/mmcblk0p1"};
    scene.deck.steam = scene.desktop.steam;
    return true;
}

/// Removes a picture scene. Failure to remove it does not change the check's result.
///
/// @param root the scene's root
void remove_picture_scene(const fs::path& root) {
    std::error_code error;
    fs::remove_all(root, error);
}

/// The chooser on the game's window: its loop, its model and what its commands do.
class ChooserScreen {
  public:

    /// Makes the chooser for the folders resolution found.
    ///
    /// @param options the parsed command line
    /// @param host what made the renderer, whose start-up frames are counted; may be null
    /// @param window the game's window
    /// @param renderer its renderer
    /// @param needed why there is no folder, and the folders found
    ChooserScreen(
        const Options& options,
        RendererHost* host,
        SDL_Window* window,
        SDL_Renderer* renderer,
        const GameFilesNeeded& needed
    )
        : options_(options), host_(host), window_(window), renderer_(renderer), needed_(needed),
          render_state_(renderer), frame_(renderer, "the folder chooser") {}

    /// Closes the gamepads it opened.
    ~ChooserScreen() {
        for (const OpenPad& pad : pads_)
            SDL_CloseGamepad(pad.pad);
    }

    ChooserScreen(const ChooserScreen&) = delete;
    ChooserScreen& operator=(const ChooserScreen&) = delete;

    /// Runs the chooser until a folder is chosen, the player quits or the window closes.
    ///
    /// @param[out] game_directory the folder chosen
    /// @return false when the player quit or the window closed
    bool run(std::optional<GameDirectory>& game_directory) {
        start();
        while (!done_)
            pass();
        if (chosen_)
            game_directory = std::move(chosen_);
        return kept_;
    }

  private:

    /// A gamepad the chooser opened.
    struct OpenPad {
        SDL_JoystickID id{}; ///< its instance
        SDL_Gamepad* pad{};  ///< its handle
    };

    /// Opens the fonts, the gamepads already there, the list and the first frame.
    void start() {
        render_state_.use_window_pixels();
        install_game_files_language(options_.preferences_file, options_.user_folder);
        try {
            fonts_ = text_font::FontStack::open(text_font::bundled_font_directory());
        } catch (const std::exception& error) {
            std::cerr << "open-annihilation: the folder chooser's fonts could not be opened: "
                      << error.what() << '\n';
        }
        if (!fonts_)
            std::cerr << "open-annihilation: the folder chooser shows no text: the bundled fonts "
                         "are missing\n";
        else
            use_game_files_language_fonts(*fonts_);
        text_family_ = game_files_text_family(options_.preferences_file);
        int count = 0;
        if (SDL_JoystickID* ids = SDL_GetGamepads(&count); ids != nullptr) {
            for (int index = 0; index < count; ++index)
                open_pad(ids[index]);
            SDL_free(ids);
        }
        model_.version = game_files_version_text();
        model_.deck = oa::platform::running_in_steam_game_mode();
        model_.offers_dialog = needed_.dialog_offered && !model_.deck;
#if defined(__APPLE__)
        model_.permission_note = model_.offers_dialog;
#endif
        std::string notice;
        if (!needed_.stored_folder.empty())
            notice = "The Total Annihilation folder chosen earlier can no longer be used:\n" +
                     path_to_utf8(needed_.stored_folder) + "\n" + needed_.stored_problem;
        if (!needed_.dialog_problem.empty()) {
            if (!notice.empty())
                notice += "\n\n";
            notice += "The system's folder dialog did not open: " + needed_.dialog_problem;
        }
        model_.notice = std::move(notice);
        model_.choose_label = tr("Use this folder");
        if (home_.empty())
            home_ = installs::home_folder();
        refresh_viewport();
        std::error_code error;
        const fs::path start =
            !home_.empty() && fs::is_directory(home_, error) ? home_ : fs::path{};
        browse_to(start.empty() ? start_folder() : start);
        dirty_ = true;
    }

    /// Runs one pass of the loop: events, the gamepads' timers, the layout, the frame.
    void pass() {
        SDL_Event event{};
        if (SDL_WaitEventTimeout(&event, dirty_ ? 0 : wait_ms())) {
            handle_event(event);
            while (!done_ && SDL_PollEvent(&event))
                handle_event(event);
        }
        if (done_)
            return;
        tick_pads();
        refresh_viewport();
        if (options_.check_text_fit)
            kit::set_text_fit_screen("folder-chooser/locator");
        repaint();
        present();
        // The text-fit check paints the locator once and leaves. A quit
        // queued before the first paint would skip the frame.
        if (options_.check_text_fit)
            finish(false);
    }

    /// Returns how long the loop may wait for an event.
    ///
    /// @return milliseconds; -1 waits for an event alone
    [[nodiscard]] int32_t wait_ms() const noexcept {
        const bool timers =
            held_ != Direction::none || std::fabs(scroll_stick_) > scroll_dead_share;
        if (timers || (host_ != nullptr && host_->start_stage_open()))
            return frame_wait_ms;
        return -1;
    }

    /// Takes one event.
    ///
    /// @param event the event
    void handle_event(const SDL_Event& event) {
        switch (event.type) {
        case SDL_EVENT_QUIT:
        case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
            finish(false);
            return;
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
            frame_.drop_texture();
            render_state_.use_window_pixels();
            dirty_ = true;
            return;
        case SDL_EVENT_RENDER_TARGETS_RESET:
            render_state_.use_window_pixels();
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
            take_pointer(read_pointer(event, viewport_, press_));
            return;
        case SDL_EVENT_KEY_DOWN:
            take_key(event.key);
            return;
        case SDL_EVENT_TEXT_INPUT:
            take_text(event.text.text);
            return;
        case SDL_EVENT_GAMEPAD_ADDED:
            open_pad(event.gdevice.which);
            return;
        case SDL_EVENT_GAMEPAD_REMOVED:
            close_pad(event.gdevice.which);
            return;
        case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            take_pad_button(event.gbutton.button, true);
            return;
        case SDL_EVENT_GAMEPAD_BUTTON_UP:
            take_pad_button(event.gbutton.button, false);
            return;
        case SDL_EVENT_GAMEPAD_AXIS_MOTION:
            take_pad_axis(event.gaxis.axis, event.gaxis.value);
            return;
        default:
            return;
        }
    }

    /// Takes what an event does to the press: a touch screen's finger (touch_screen) or the
    /// pointer's button going down presses, a move may scroll the rows, coming up carries out
    /// the target's command, a cancelled finger lets go, and the wheel scrolls the rows.
    ///
    /// @param pointer the event, read in canvas pixels
    void take_pointer(const PointerEvent& pointer) {
        switch (pointer.step) {
        case PointerStep::none:
            return;
        case PointerStep::down:
            if (!pointer.finger || touch_screen(pointer.touch))
                press_at(pointer.x, pointer.y, pointer.finger, pointer.touch, pointer.id);
            return;
        case PointerStep::move:
            move_to(pointer.y);
            return;
        case PointerStep::up:
            release_at(pointer.x, pointer.y);
            return;
        case PointerStep::cancel:
            ui_.pressed = -1;
            press_ = {};
            dirty_ = true;
            return;
        case PointerStep::wheel:
            chooser::scroll(layout_, ui_, static_cast<int32_t>(std::lround(pointer.wheel_points)));
            dirty_ = true;
            return;
        }
    }

    /// Starts a press.
    ///
    /// @param x where, canvas pixels
    /// @param y where, canvas pixels
    /// @param finger it is a finger's
    /// @param touch the finger's device
    /// @param id the finger
    void press_at(float x, float y, bool finger, SDL_TouchID touch, SDL_FingerID id) {
        if (press_.active)
            return;
        lay_out_if_stale();
        press_ = WindowPress{true, finger, touch, id, x, y, y, false};
        chooser::press_down(layout_, ui_, canvas_point(x, y));
        dirty_ = true;
    }

    /// Moves a press: over the rows, past drag_threshold_points it scrolls them instead.
    ///
    /// @param y where it is, canvas pixels
    void move_to(float y) {
        if (!press_.active)
            return;
        const float scale = std::max(viewport_.px_per_point, 0.01F);
        if (!press_.scrolling) {
            if (!press_scrolls(press_, layout_.paint.rows, y, scale))
                return;
            // The press becomes a scroll: what it held is let go.
            press_.scrolling = true;
            ui_.pressed = -1;
            press_.last_y = press_.start_y;
        }
        const float points = (press_.last_y - y) / scale;
        const auto whole = static_cast<int32_t>(points);
        if (whole == 0)
            return;
        press_.last_y -= static_cast<float>(whole) * scale;
        chooser::scroll(layout_, ui_, whole);
        dirty_ = true;
    }

    /// Ends a press: on the target it went down on, the target's command is carried out.
    ///
    /// @param x where, canvas pixels
    /// @param y where, canvas pixels
    void release_at(float x, float y) {
        if (!press_.active)
            return;
        const WindowPress finished = press_;
        press_ = {};
        dirty_ = true;
        if (finished.scrolling) {
            ui_.pressed = -1;
            return;
        }
        act(chooser::press_up(layout_, ui_, canvas_point(x, y)));
    }

    /// Takes a key.
    ///
    /// @param key the key's event
    void take_key(const SDL_KeyboardEvent& key) {
        const bool shift = (key.mod & SDL_KMOD_SHIFT) != 0;
        switch (key.key) {
        case SDLK_TAB:
            press_key(shift ? chooser::Key::back_tab : chooser::Key::tab);
            return;
        case SDLK_RETURN:
        case SDLK_KP_ENTER:
        case SDLK_SPACE:
            press_key(chooser::Key::enter);
            return;
        case SDLK_ESCAPE:
            go_back();
            return;
        case SDLK_BACKSPACE:
            if (filter_focused_ && !model_.filter.empty()) {
                model_.filter.pop_back();
                show_filtered();
                return;
            }
            if (model_.has_parent)
                act({chooser::Command::parent_folder, 0});
            return;
        case SDLK_X:
            if (!filter_focused_)
                press_key(chooser::Key::choose);
            return;
        case SDLK_Y:
            if (!filter_focused_)
                press_key(chooser::Key::filter);
            return;
        case SDLK_UP:
            press_key(chooser::Key::up);
            return;
        case SDLK_DOWN:
            press_key(chooser::Key::down);
            return;
        case SDLK_LEFT:
            press_key(chooser::Key::left);
            return;
        case SDLK_RIGHT:
            press_key(chooser::Key::right);
            return;
        case SDLK_PAGEUP:
            press_key(chooser::Key::page_up);
            return;
        case SDLK_PAGEDOWN:
            press_key(chooser::Key::page_down);
            return;
        default:
            return;
        }
    }

    /// Gives the chooser a key and carries out what it asks.
    ///
    /// @param key the key
    void press_key(chooser::Key key) {
        lay_out_if_stale();
        const chooser::Outcome outcome = chooser::key(layout_, ui_, key);
        dirty_ = true;
        stale_ = true;
        act(outcome);
    }

    /// Opens a gamepad, once.
    ///
    /// @param id its instance
    void open_pad(SDL_JoystickID id) {
        if (std::any_of(pads_.begin(), pads_.end(), [id](const OpenPad& pad) {
                return pad.id == id;
            }))
            return;
        if (SDL_Gamepad* pad = SDL_OpenGamepad(id))
            pads_.push_back({id, pad});
    }

    /// Closes a gamepad that went away.
    ///
    /// @param id its instance
    void close_pad(SDL_JoystickID id) {
        const auto found = std::find_if(pads_.begin(), pads_.end(), [id](const OpenPad& pad) {
            return pad.id == id;
        });
        if (found == pads_.end())
            return;
        SDL_CloseGamepad(found->pad);
        pads_.erase(found);
        release_direction();
        stick_direction_ = Direction::none;
        scroll_stick_ = 0.0F;
    }

    /// Takes a gamepad's button: the D-pad moves the focus (held, it repeats), A presses, B
    /// goes back, the shoulder buttons turn a page.
    ///
    /// @param button the button
    /// @param down it went down
    void take_pad_button(uint8_t button, bool down) {
        const Direction direction = direction_of(button);
        if (direction != Direction::none) {
            if (down)
                hold_direction(direction, HoldSource::dpad);
            else if (held_ == direction && hold_source_ == HoldSource::dpad)
                release_direction();
            return;
        }
        if (!down)
            return;
        switch (static_cast<SDL_GamepadButton>(button)) {
        case SDL_GAMEPAD_BUTTON_SOUTH:
            press_key(chooser::Key::enter);
            return;
        case SDL_GAMEPAD_BUTTON_EAST:
            go_back();
            return;
        case SDL_GAMEPAD_BUTTON_WEST:
            press_key(chooser::Key::choose);
            return;
        case SDL_GAMEPAD_BUTTON_NORTH:
            press_key(chooser::Key::filter);
            return;
        case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
            press_key(chooser::Key::page_up);
            return;
        case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
            press_key(chooser::Key::page_down);
            return;
        default:
            return;
        }
    }

    /// Takes a gamepad's axis: the left stick moves the focus like the D-pad, the right stick
    /// scrolls the rows.
    ///
    /// @param axis the axis
    /// @param value its value
    void take_pad_axis(uint8_t axis, int16_t value) {
        const float share = std::clamp(static_cast<float>(value) / stick_axis_range, -1.0F, 1.0F);
        switch (static_cast<SDL_GamepadAxis>(axis)) {
        case SDL_GAMEPAD_AXIS_LEFTX:
            stick_x_ = share;
            break;
        case SDL_GAMEPAD_AXIS_LEFTY:
            stick_y_ = share;
            break;
        case SDL_GAMEPAD_AXIS_RIGHTY:
            scroll_stick_ = share;
            last_tick_ms_ = SDL_GetTicks();
            return;
        default:
            return;
        }
        // A held direction lasts while the stick stays out past stick_off_share along it; a new
        // one needs stick_on_share along the axis the stick leans on most.
        Direction next = Direction::none;
        const float along_y = std::fabs(stick_y_);
        const float along_x = std::fabs(stick_x_);
        const bool keeps = (stick_direction_ == Direction::up && stick_y_ <= -stick_off_share) ||
                           (stick_direction_ == Direction::down && stick_y_ >= stick_off_share) ||
                           (stick_direction_ == Direction::left && stick_x_ <= -stick_off_share) ||
                           (stick_direction_ == Direction::right && stick_x_ >= stick_off_share);
        if (keeps)
            next = stick_direction_;
        else if (std::max(along_x, along_y) >= stick_on_share)
            next = along_y >= along_x ? (stick_y_ < 0.0F ? Direction::up : Direction::down)
                                      : (stick_x_ < 0.0F ? Direction::left : Direction::right);
        if (next == stick_direction_)
            return;
        stick_direction_ = next;
        if (next != Direction::none)
            hold_direction(next, HoldSource::stick);
        else if (hold_source_ == HoldSource::stick)
            release_direction();
    }

    /// Starts a held direction: one step at once, then the menu repeat.
    ///
    /// @param direction the direction
    /// @param source what holds it
    void hold_direction(Direction direction, HoldSource source) {
        held_ = direction;
        hold_source_ = source;
        const uint32_t steps = repeat_.press(SDL_GetTicks());
        for (uint32_t step = 0; step < steps; ++step)
            press_key(key_of(direction));
    }

    /// Ends the held direction.
    void release_direction() noexcept {
        held_ = Direction::none;
        repeat_.release();
    }

    /// Moves the gamepads' timers on: a held direction's repeats and the right stick's scroll.
    void tick_pads() {
        const uint64_t now = SDL_GetTicks();
        if (held_ != Direction::none) {
            const uint32_t steps = repeat_.advance(now);
            for (uint32_t step = 0; step < steps; ++step)
                press_key(key_of(held_));
        }
        const float lean = std::fabs(scroll_stick_);
        if (lean > scroll_dead_share && now > last_tick_ms_) {
            const float push = (lean - scroll_dead_share) / (1.0F - scroll_dead_share);
            const float seconds = static_cast<float>(now - last_tick_ms_) / ms_per_second;
            scroll_carry_ += (scroll_stick_ < 0.0F ? -1.0F : 1.0F) * push * push *
                             stick_scroll_points_per_second * seconds;
            const auto whole = static_cast<int32_t>(scroll_carry_);
            if (whole != 0) {
                scroll_carry_ -= static_cast<float>(whole);
                chooser::scroll(layout_, ui_, whole);
                dirty_ = true;
            }
        } else {
            scroll_carry_ = 0.0F;
        }
        last_tick_ms_ = now;
    }

    /// Clears a filter that holds text. Otherwise goes up a folder, or cancels.
    void go_back() {
        if (!model_.filter.empty()) {
            model_.filter.clear();
            filter_focused_ = false;
            SDL_StopTextInput(window_);
            show_filtered();
            return;
        }
        filter_focused_ = false;
        press_key(chooser::Key::escape);
    }

    /// Appends typed text to the filter while it is taking text.
    ///
    /// @param text the characters
    void take_text(const char* text) {
        if (!filter_focused_ || text == nullptr || text[0] == '\0')
            return;
        if (model_.filter.size() >= most_filter_characters)
            return;
        model_.filter += text;
        if (model_.filter.size() > most_filter_characters)
            model_.filter.resize(most_filter_characters);
        show_filtered();
    }

    /// Carries out what a press or a key asks.
    ///
    /// @param outcome the command
    void act(chooser::Outcome outcome) {
        switch (outcome.command) {
        case chooser::Command::none:
            return;
        case chooser::Command::open_place:
            if (outcome.index < places_.size())
                browse_to(places_[outcome.index]);
            return;
        case chooser::Command::parent_folder:
            if (model_.has_parent)
                browse_to(browsed_.parent_path());
            return;
        case chooser::Command::activate_entry:
            if (outcome.index < shown_paths_.size()) {
                if (model_.entries[outcome.index].directory)
                    browse_to(shown_paths_[outcome.index]);
                else
                    select_row(outcome.index);
            }
            return;
        case chooser::Command::select_entry:
            select_row(outcome.index);
            return;
        case chooser::Command::choose:
            choose_current();
            return;
        case chooser::Command::use_dialog:
            use_dialog();
            return;
        case chooser::Command::cancel:
            finish(false);
            return;
        case chooser::Command::toggle_filter:
            filter_focused_ = !filter_focused_;
            if (filter_focused_)
                SDL_StartTextInput(window_);
            else
                SDL_StopTextInput(window_);
            dirty_ = true;
            return;
        }
    }

    /// Points the side panel at a row and says whether it can be used.
    ///
    /// @param index the row
    void select_row(uint16_t index) {
        if (index >= model_.entries.size())
            return;
        model_.selected = static_cast<int32_t>(index);
        sync_choice();
        stale_ = true;
        dirty_ = true;
    }

    /// Uses the selected installer or folder, or the folder being shown when that is the game.
    void choose_current() {
        if (model_.selected >= 0 &&
            static_cast<std::size_t>(model_.selected) < model_.entries.size() &&
            static_cast<std::size_t>(model_.selected) < shown_paths_.size() &&
            model_.entries[static_cast<std::size_t>(model_.selected)].choosable) {
            const chooser::Entry& entry = model_.entries[static_cast<std::size_t>(model_.selected)];
            if (entry.directory)
                pick(shown_paths_[static_cast<std::size_t>(model_.selected)]);
            else
                pick(browsed_);
            return;
        }
        if (folder_choosable_)
            pick(browsed_);
    }

    /// Returns the folder the browser opens at first: the home folder, else the folder the game
    /// runs in.
    ///
    /// @return the folder
    [[nodiscard]] fs::path start_folder() const {
        std::error_code error;
        if (!home_.empty() && fs::is_directory(home_, error))
            return home_;
        fs::path current = fs::current_path(error);
        return error ? fs::path("/") : current;
    }

    /// Plays a folder when its check passes, else says why it cannot be played.
    ///
    /// @param folder the folder
    void pick(const fs::path& folder) {
        last_asked_ = folder;
        // The picture check's files are empty. Choosing records the folder and does not read them.
        if (picture_check_)
            return;
        std::string problem;
        if (auto taken = take_chosen_folder(options_, folder, &problem)) {
            chosen_ = std::move(taken);
            finish(true);
            return;
        }
        model_.notice = "This folder cannot be played:\n" + path_to_utf8(folder) + "\n" + problem;
        model_.can_choose = folder_choosable_;
        stale_ = true;
        dirty_ = true;
    }

    /// Opens the system's folder dialog and plays the folder picked. Game Mode never opens it.
    /// A dialog that cannot open is said in the notice and no longer offered.
    void use_dialog() {
        if (!model_.offers_dialog || model_.deck)
            return;
        fs::path chosen;
        std::string error;
        const fs::path start = browsed_.empty() ? start_folder() : browsed_;
        const FolderPick picked = pick_game_folder_with_dialog(start, &chosen, &error);
        dirty_ = true;
        stale_ = true;
        switch (picked) {
        case FolderPick::chosen:
            pick(chosen);
            return;
        case FolderPick::cancelled:
            return;
        case FolderPick::unavailable:
            model_.notice = "The system's folder dialog did not open: " + error;
            model_.offers_dialog = false;
            model_.permission_note = false;
            return;
        }
    }

    /// Shows a folder: its places, its files and folders, and what each one is.
    ///
    /// @param folder the folder
    void browse_to(const fs::path& folder) {
        std::error_code error;
        fs::path shown = fs::absolute(folder, error).lexically_normal();
        if (error)
            shown = folder;
        if (!shown.has_filename() && shown.has_relative_path())
            shown = shown.parent_path();
        browsed_ = shown;
        model_.path = installs::path_under_home(home_, shown);
        model_.has_parent = shown.has_relative_path() && shown.parent_path() != shown;
        model_.filter.clear();
        // Typing filters the folder just opened, without a press on Filter first.
        filter_focused_ = true;
        if (window_ != nullptr)
            SDL_StartTextInput(window_);
        fill_places();
        fill_entries();
        show_filtered();
        ui_.scroll_points = 0;
        ui_.pressed = -1;
        ui_.focus_shown = false;
        lay_out_now();
        ui_.focus = 0;
        for (std::size_t index = 0; index < layout_.focus_order.size(); ++index) {
            const chooser::Target target = layout_.focus_order[index];
            if (target.kind == chooser::TargetKind::entry &&
                target.index == static_cast<uint16_t>(std::max(model_.selected, 0))) {
                ui_.focus = static_cast<int32_t>(index);
                break;
            }
        }
        stale_ = true;
        dirty_ = true;
    }

    /// Fills the sidebar: the machine's places, then the installs already found, in the
    /// found band between the places and the volumes.
    void fill_places() {
        places_.clear();
        model_.places.clear();
        const installs::LocatorFacts facts =
            staged_facts_ ? *staged_facts_ : installs::locator_place_facts(model_.deck);
        const std::vector<installs::LocatorPlace> listed = installs::locator_places(facts);
        bool found_added = false;
        const auto add_found = [this, &found_added]() {
            if (found_added)
                return;
            found_added = true;
            for (const FoundInstall& install : needed_.found) {
                places_.push_back(install.folder);
                chooser::Place place;
                place.label = path_to_utf8(install.folder.filename());
                if (place.label.empty())
                    place.label = path_to_utf8(install.folder);
                place.group = chooser::PlaceGroup::found;
                model_.places.push_back(std::move(place));
            }
        };
        for (const installs::LocatorPlace& fact : listed) {
            if (fact.group != installs::LocatorGroup::places)
                add_found();
            places_.push_back(fact.folder);
            chooser::Place place;
            place.label = tr(fact.label);
            switch (fact.group) {
            case installs::LocatorGroup::places:
                place.group = chooser::PlaceGroup::places;
                break;
            case installs::LocatorGroup::volumes:
                place.group = chooser::PlaceGroup::volumes;
                break;
            case installs::LocatorGroup::deck_games:
                place.group = chooser::PlaceGroup::deck_games;
                break;
            }
            model_.places.push_back(std::move(place));
        }
        add_found();
        model_.place = -1;
        for (std::size_t index = 0; index < places_.size(); ++index) {
            if (places_[index] == browsed_)
                model_.place = static_cast<int32_t>(index);
        }
    }

    /// Reads up to most_child_files files in a folder, and the files in its music folder.
    ///
    /// @param folder the folder
    /// @return the files, by name and size
    [[nodiscard]] std::vector<chooser::ChildFile> child_files(const fs::path& folder) const {
        std::vector<chooser::ChildFile> files;
        const auto take = [&files](const fs::path& directory, std::string prefix) {
            std::error_code error;
            fs::directory_iterator entry(
                directory, fs::directory_options::skip_permission_denied, error
            );
            for (; !error && entry != fs::directory_iterator() && files.size() < most_child_files;
                 entry.increment(error)) {
                std::error_code kind_error;
                if (!entry->is_regular_file(kind_error) || kind_error)
                    continue;
                const std::string name = path_to_utf8(entry->path().filename());
                if (name.empty() || name.front() == '.')
                    continue;
                std::error_code size_error;
                const auto size = entry->file_size(size_error);
                if (size_error)
                    continue;
                chooser::ChildFile child;
                child.name = prefix + name;
                child.size = static_cast<uint64_t>(size);
                files.push_back(std::move(child));
            }
        };
        take(folder, {});
        take(folder / "music", "music/");
        return files;
    }

    /// One file or folder read from the folder being shown.
    struct Listed {
        fs::path path{};        ///< where it is
        chooser::Entry shown{}; ///< what the row says
        bool directory{};       ///< it is a folder, for sorting
    };

    /// Fills the folder's rows, files and folders, hidden names left out.
    void fill_entries() {
        listed_.clear();
        std::error_code error;
        fs::directory_iterator entry(
            browsed_, fs::directory_options::skip_permission_denied, error
        );
        std::size_t seen = 0;
        std::vector<chooser::ChildFile> here;
        for (; !error && entry != fs::directory_iterator(); entry.increment(error)) {
            if (++seen > browse_listing_limit || listed_.size() >= most_browse_entries)
                break;
            const fs::path path = entry->path();
            const std::string name = path_to_utf8(path.filename());
            if (name.empty() || name.front() == '.')
                continue;
            std::error_code kind_error;
            const bool directory = entry->is_directory(kind_error) && !kind_error;
            const bool file = !directory && entry->is_regular_file(kind_error) && !kind_error;
            if (!directory && !file)
                continue;
            uint64_t size = 0;
            std::vector<chooser::ChildFile> children;
            if (file) {
                std::error_code size_error;
                const auto bytes = entry->file_size(size_error);
                if (!size_error)
                    size = static_cast<uint64_t>(bytes);
                if (here.size() < most_child_files)
                    here.push_back(chooser::ChildFile{name, size});
            } else {
                children = child_files(path);
            }
            const chooser::SourceAccount account =
                chooser::describe_source(name, size, directory, children, demo_1997.installer_size);
            Listed row;
            row.path = path;
            row.directory = directory;
            row.shown.name = name;
            row.shown.what = account.what;
            row.shown.title = account.title.empty() ? name : account.title;
            row.shown.account = account.account;
            row.shown.bundles = account.bundles;
            row.shown.directory = directory;
            row.shown.recognised = account.recognised;
            row.shown.choosable = account.choosable;
            if (file && size > 0)
                row.shown.size_label = view::size_text(size);
            listed_.push_back(std::move(row));
        }
        std::stable_sort(listed_.begin(), listed_.end(), [](const Listed& a, const Listed& b) {
            return chooser::entry_before(a.shown, b.shown);
        });
        const chooser::SourceAccount folder_account = chooser::describe_source(
            path_to_utf8(browsed_.filename()), 0, true, here, demo_1997.installer_size
        );
        folder_choosable_ = folder_account.choosable;
    }

    /// Applies the filter and points the side panel at the first row that can be used.
    void show_filtered() {
        model_.entries.clear();
        shown_paths_.clear();
        for (const Listed& row : listed_) {
            if (!chooser::name_matches(row.shown.name, model_.filter))
                continue;
            model_.entries.push_back(row.shown);
            shown_paths_.push_back(row.path);
        }
        model_.selected = model_.entries.empty() ? -1 : 0;
        for (std::size_t index = 0; index < model_.entries.size(); ++index) {
            if (model_.entries[index].choosable) {
                model_.selected = static_cast<int32_t>(index);
                break;
            }
        }
        sync_choice();
        stale_ = true;
        dirty_ = true;
    }

    /// Sets the accent button from the selection, or from the folder being shown.
    void sync_choice() {
        const bool row_ok = model_.selected >= 0 &&
                            static_cast<std::size_t>(model_.selected) < model_.entries.size() &&
                            model_.entries[static_cast<std::size_t>(model_.selected)].choosable;
        model_.can_choose = row_ok || folder_choosable_;
        if (row_ok && !model_.entries[static_cast<std::size_t>(model_.selected)].directory)
            model_.choose_label = tr("Use this installer");
        else
            model_.choose_label = tr("Use this folder");
    }

    /// Ends the chooser.
    ///
    /// @param kept true when a folder was chosen; false when the player quit
    void finish(bool kept) noexcept {
        done_ = true;
        kept_ = kept;
    }

    /// Reads the window's size, density and safe area into the viewport.
    void refresh_viewport() {
        if (read_window_viewport(window_, renderer_, viewport_)) {
            dirty_ = true;
            stale_ = true;
        }
    }

    /// Lays the chooser out from the model and the state now.
    void lay_out_now() {
        layout_ = chooser::lay_out(
            model_, ui_, viewport_, game_files_measure(fonts_.get(), text_family_)
        );
        // A focus past the end of a new layout moves to its first target.
        if (ui_.focus >= static_cast<int32_t>(layout_.focus_order.size()))
            ui_.focus = layout_.focus_order.empty() ? -1 : 0;
        ui_.scroll_points =
            std::clamp(ui_.scroll_points, 0, std::max(0, layout_.scroll_max_points));
        stale_ = false;
    }

    /// Lays the chooser out again when the model, the state or the scroll changed since, so
    /// that input reads the boxes as they now are.
    void lay_out_if_stale() {
        if (stale_ || dirty_)
            lay_out_now();
    }

    /// Lays out and paints the frame when anything changed.
    void repaint() {
        painted_ = false;
        if (!dirty_ || viewport_.width <= 0 || viewport_.height <= 0)
            return;
        lay_out_now();
        if (canvas_.width != viewport_.width || canvas_.height != viewport_.height)
            canvas_ = paint::make_canvas(viewport_.width, viewport_.height);
        paint_game_files(
            canvas_,
            layout_.paint,
            fonts_.get(),
            viewport_.px_per_point,
            game_files_icon(),
            text_family_
        );
        dirty_ = false;
        painted_ = true;
    }

    /// Shows the frame on the window.
    void present() {
        // While the renderer's start-up stage stands, the last frame is shown again each pass.
        const bool counting = host_ != nullptr && host_->start_stage_open();
        if (!painted_ && !counting)
            return;
        static_cast<void>(frame_.show(canvas_, render_state_, host_));
    }

    /// The family the text is drawn in: Barlow in the modern look (game_files_text_family).
    text_font::Family text_family_{text_font::Family::sans};
    const Options& options_;   ///< the parsed command line
    RendererHost* host_{};     ///< what made the renderer; may be null
    SDL_Window* window_{};     ///< the game's window
    SDL_Renderer* renderer_{}; ///< its renderer
    GameFilesNeeded needed_{}; ///< why there is no folder, and the folders found
    RenderState render_state_; ///< the renderer's state to put back
    WindowFrame frame_;        ///< the frame shown on the window
    std::unique_ptr<text_font::FontStack> fonts_{}; ///< the bundled fonts, when open
    chooser::Model model_{};                        ///< what the chooser shows
    chooser::UiState ui_{};                         ///< what the chooser keeps between frames
    chooser::Layout layout_{};                      ///< the layout last made
    view::Viewport viewport_{};                     ///< the window's canvas
    paint::Canvas canvas_{};                        ///< the frame last painted
    bool dirty_{true};                              ///< the frame must be painted again
    bool stale_{true};                      ///< the layout must be made again before input reads it
    bool painted_{};                        ///< this pass painted a frame
    bool done_{};                           ///< the chooser has ended
    bool kept_{};                           ///< it ended with a folder chosen
    std::optional<GameDirectory> chosen_{}; ///< the folder chosen
    WindowPress press_{};                   ///< the press under way
    std::vector<OpenPad> pads_{};           ///< the gamepads opened
    Direction held_{Direction::none};       ///< the direction held down
    HoldSource hold_source_{HoldSource::dpad};   ///< what holds it
    oa::ui::pad_controls::MenuRepeat repeat_{};  ///< its repeats
    Direction stick_direction_{Direction::none}; ///< the left stick's direction
    float stick_x_{};                            ///< the left stick across, -1 to 1
    float stick_y_{};                            ///< the left stick down, -1 to 1
    float scroll_stick_{};                       ///< the right stick down, -1 to 1
    float scroll_carry_{};                ///< the part of a point the right stick has scrolled
    uint64_t last_tick_ms_{};             ///< when the gamepads' timers last moved on
    fs::path home_{};                     ///< the player's home folder
    fs::path browsed_{};                  ///< the folder being shown
    std::vector<fs::path> places_{};      ///< the sidebar, by Model::places
    std::vector<Listed> listed_{};        ///< every row of the folder, before the filter
    std::vector<fs::path> shown_paths_{}; ///< the rows on screen, by Model::entries
    bool folder_choosable_{};             ///< the folder being shown is itself a game
    bool filter_focused_{};               ///< typed text goes into the filter
    /// Places the picture check shows instead of this machine's. Empty in ordinary play.
    std::optional<installs::LocatorFacts> staged_facts_{};
    fs::path last_asked_{}; ///< the folder the choose key last asked to use
    bool picture_check_{};  ///< choosing records the folder and does not read its files

  public:

    /// Builds a scratch home, drives the locator with Tab onto its install, writes the
    /// desktop, Deck and phone pictures when asked, then chooses that folder with the choose key.
    ///
    /// @param desktop_picture the desktop picture; empty writes none
    /// @param deck_picture the Deck picture; empty writes none
    /// @param phone_picture the phone picture; empty writes none
    /// @return true when the choose key asked for the staged install
    bool drive(
        const fs::path& desktop_picture, const fs::path& deck_picture, const fs::path& phone_picture
    ) {
        const fs::path root = !options_.user_folder || options_.user_folder->empty()
                                  ? fs::temp_directory_path() / "oa-folder-chooser-scene"
                                  : *options_.user_folder / "locator-scene";

        struct Wiper {
            fs::path folder;

            ~Wiper() { remove_picture_scene(folder); }
        } wiper{root};

        PictureScene scene;
        if (!build_picture_scene(root, scene)) {
            std::cerr << "open-annihilation: folder chooser check: the staged folder could not "
                         "be made\n";
            return false;
        }
        picture_check_ = true;
        home_ = scene.home;
        staged_facts_ = scene.desktop;
        start();
        model_.deck = false;
        model_.offers_dialog = true;
#if defined(__APPLE__)
        model_.permission_note = true;
#endif
        if (window_ != nullptr) {
            SDL_SetWindowFullscreen(window_, false);
            SDL_SetWindowSize(window_, 1280, 720);
        }
        refresh_viewport();
        browse_to(scene.home / "Games");
        ui_.focus = 0;
        ui_.focus_shown = false;
        const fs::path fixture = scene.install;
        int target = -1;
        for (std::size_t index = 0; index < shown_paths_.size(); ++index) {
            std::error_code same;
            if (shown_paths_[index] == fixture ||
                fs::equivalent(shown_paths_[index], fixture, same))
                target = static_cast<int32_t>(index);
        }
        if (target < 0) {
            std::cerr << "open-annihilation: folder chooser check: the staged install is not in "
                      << model_.path << '\n';
            return false;
        }
        const int limit = static_cast<int>(layout_.focus_order.size()) + 3;
        for (int step = 0; step < limit; ++step) {
            press_key(chooser::Key::tab);
            if (ui_.focus < 0 || static_cast<std::size_t>(ui_.focus) >= layout_.focus_order.size())
                continue;
            const chooser::Target focused =
                layout_.focus_order[static_cast<std::size_t>(ui_.focus)];
            if (focused.kind == chooser::TargetKind::entry &&
                focused.index == static_cast<uint16_t>(target))
                break;
        }
        if (model_.selected != target) {
            std::cerr << "open-annihilation: folder chooser check: the keys did not reach the "
                         "staged install\n";
            return false;
        }
        if (!desktop_picture.empty() && !write_picture(desktop_picture))
            return false;
        const int32_t kept_row = model_.selected;
        model_.deck = true;
        model_.offers_dialog = false;
        model_.permission_note = false;
        staged_facts_ = scene.deck;
        fill_places();
        model_.selected = kept_row;
        sync_choice();
        if (window_ != nullptr)
            SDL_SetWindowSize(window_, 1280, 800);
        refresh_viewport();
        focus_row(kept_row);
        if (!deck_picture.empty() && !write_picture(deck_picture))
            return false;
        model_.deck = false;
        model_.offers_dialog = true;
#if defined(__APPLE__)
        model_.permission_note = true;
#else
        model_.permission_note = false;
#endif
        staged_facts_ = scene.desktop;
        fill_places();
        model_.selected = kept_row;
        sync_choice();
        if (window_ != nullptr)
            SDL_SetWindowSize(window_, 852, 393);
        refresh_viewport();
        focus_row(kept_row);
        if (!phone_picture.empty() && !write_picture(phone_picture))
            return false;
        press_key(chooser::Key::choose);
        std::error_code same;
        const bool matched = last_asked_ == fixture || fs::equivalent(last_asked_, fixture, same);
        if (!matched) {
            std::cerr << "open-annihilation: folder chooser check: the choose key did not ask for "
                         "the staged install\n";
            return false;
        }
        std::cout << "folder chooser check: passed\n";
        return true;
    }

    /// Points the shown focus at a row, after the window's size has changed.
    ///
    /// @param row the row
    void focus_row(int32_t row) {
        lay_out_now();
        ui_.focus_shown = true;
        for (std::size_t index = 0; index < layout_.focus_order.size(); ++index) {
            const chooser::Target target = layout_.focus_order[index];
            if (target.kind == chooser::TargetKind::entry &&
                target.index == static_cast<uint16_t>(std::max(row, 0))) {
                ui_.focus = static_cast<int32_t>(index);
                break;
            }
        }
        stale_ = true;
        dirty_ = true;
    }

    /// Paints the locator and writes it as a PNG.
    ///
    /// @param file the picture
    /// @return true when it was written
    bool write_picture(const fs::path& file) {
        dirty_ = true;
        repaint();
        if (canvas_.width <= 0 || canvas_.height <= 0) {
            std::cerr << "open-annihilation: folder chooser check: the window has no picture\n";
            return false;
        }
        std::string error;
        if (!write_game_files_png(file, canvas_, &error)) {
            std::cerr << "open-annihilation: folder chooser check: " << error << '\n';
            return false;
        }
        return true;
    }
};

} // namespace

bool folder_chooser_offered(const Options& options) {
    return !nobody_watches(options) && !game_files_import_offered(game_files_hooks());
}

bool run_folder_chooser_key_check(
    const Options& options, const FolderChooserDisplayHooks& display, const fs::path& picture
) {
    SDL_Window* window = display.window != nullptr ? display.window(display.context) : nullptr;
    SDL_Renderer* renderer = window != nullptr && display.renderer != nullptr
                                 ? display.renderer(display.context)
                                 : nullptr;
    if (window == nullptr || renderer == nullptr) {
        std::cerr << "open-annihilation: folder chooser check: the window cannot open\n";
        return false;
    }
    RendererHost* host = display.host != nullptr ? display.host(display.context) : nullptr;
    fs::path deck;
    fs::path phone;
    if (!picture.empty()) {
        const std::string name = picture.stem().string() + "-deck" + picture.extension().string();
        deck = picture.parent_path() / name;
        const std::string phone_name =
            picture.stem().string() + "-phone" + picture.extension().string();
        phone = picture.parent_path() / phone_name;
    }
    ChooserScreen screen(options, host, window, renderer, GameFilesNeeded{});
    return screen.drive(picture, deck, phone);
}

bool run_folder_chooser_until_resolved(
    const Options& options,
    const FolderChooserDisplayHooks& display,
    GameFilesNeeded& needed,
    std::optional<GameDirectory>& game_directory
) {
    if (game_directory || !needed.needed || !needed.chooser)
        return true;
    SDL_Window* window = display.window != nullptr ? display.window(display.context) : nullptr;
    SDL_Renderer* renderer = window != nullptr && display.renderer != nullptr
                                 ? display.renderer(display.context)
                                 : nullptr;
    if (window == nullptr || renderer == nullptr)
        throw std::runtime_error(
            "the game's window cannot open to choose the Total Annihilation folder; name it with "
            "--game-dir PATH"
        );
    RendererHost* host = display.host != nullptr ? display.host(display.context) : nullptr;
    bool kept = false;
    {
        ChooserScreen screen(options, host, window, renderer, needed);
        kept = screen.run(game_directory);
    }
    if (game_directory)
        needed = GameFilesNeeded{};
    return kept;
}

} // namespace oa::app
