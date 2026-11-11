// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-replay-viewer: a skirmish of three players, at least 2,400 ticks, is
// recorded and watched. Two computers take one side. One computer leaves as
// the recording ends. The bar is drawn at 640x480, 1280x720 and 1920x1080,
// and in German at 640x480. Each press is checked against the replay: pause,
// seeks, markers, speed, a timeline drag, the pin, the shortcut card, the
// controller and a tap. The camera modes are Free, Follow their camera,
// Follow a unit and, once the index holds a fight, Director. A second play
// switches view and camera, seeks nowhere after that, and must digest the
// same as the straight play. Markers come from the recording's index pass.

#include "library_host.hpp"
#include "net_options.hpp"
#include "network_play.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "pad_state.hpp"
#include "touch_dispatch.hpp"

#include "oa/app/expansion_entry.hpp"
#include "oa/app/frame_pacing.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/base/game_loop.hpp"
#include "oa/core/player.h"
#include "oa/core/world.h"
#include "oa/platform/files.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/unit_spawn/spawn.hpp"
#include "oa/sim/unit_spawn/spawn_runtime.hpp"
#include "oa/ui/first_run/first_run.hpp"
#include "oa/ui/library/export.hpp"
#include "oa/ui/library/replays.hpp"
#include "oa/ui/replay_viewer/replay_viewer.hpp"

#include <SDL3/SDL.h>

#include <algorithm>

#ifndef _WIN32
#include <cerrno>
#include <sys/resource.h>
#include <unistd.h>
#endif

#include <array>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace layout = oa::ui::display_layout;
namespace library = oa::ui::library;
namespace first_run = oa::ui::first_run;
namespace pad = oa::ui::pad_controls;

/// Milliseconds one check frame moves the clock. Fifty is one tick at the
/// match clock's 30 units a second.
constexpr uint32_t kFrameMs = 50;
/// Nanoseconds in a millisecond.
constexpr uint64_t kNsPerMs = 1'000'000;
/// A minute at the match clock, the short jump.
constexpr uint32_t kShortTicks = 300;
/// Six minutes at the match clock, the long jump.
constexpr uint32_t kLongTicks = 1800;

/// One window the check opens the bar in.
struct CheckWindow {
    int32_t width{};  ///< the window's width, in pixels
    int32_t height{}; ///< its height
};

/// The windows, in the order the check opens them.
constexpr std::array<CheckWindow, 3> kWindows{{
    {640, 480},
    {1280, 720},
    {1920, 1080},
}};

/// One row of the speed menu and the speed it asks for.
struct SpeedStep {
    std::string_view name{}; ///< the control
    uint32_t milli{};        ///< thousandths of 1x
};

/// The speed menu, slowest first.
constexpr std::array<SpeedStep, 6> kSpeeds{{
    {"oa.replay-viewer.speed-500", 500},
    {"oa.replay-viewer.speed-1000", 1000},
    {"oa.replay-viewer.speed-2000", 2000},
    {"oa.replay-viewer.speed-4000", 4000},
    {"oa.replay-viewer.speed-8000", 8000},
    {"oa.replay-viewer.speed-16000", 16000},
}};

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("replay viewer check: " + std::string(what));
}

/// The path of one step's snapshot: <stem>-<step>.ppm beside --snapshot.
///
/// @param snapshot the --snapshot path
/// @param step the step's name
/// @return the step's snapshot path
fs::path step_snapshot(const fs::path& snapshot, std::string_view step) {
    fs::path file = snapshot.parent_path() / snapshot.stem();
    file += '-';
    file += std::string(step);
    file += ".ppm";
    return file;
}

/// The digest as sixteen hex digits.
///
/// @param digest the match digest
/// @return the text
std::string digest_text(uint64_t digest) {
    std::ostringstream line;
    line << std::hex << std::setfill('0') << std::setw(16) << digest;
    return line.str();
}

/// Tells whether a folder holds a finished export, not a partial file.
///
/// @param folder the folder
/// @return true when a regular file there has bytes
bool exported_file(const fs::path& folder) {
    std::error_code failed;
    if (!fs::is_directory(folder, failed))
        return false;
    const fs::directory_iterator listing(folder, failed);
    if (failed)
        return false;
    for (const fs::directory_entry& entry : listing) {
        const std::string name = oa::platform::path_to_utf8(entry.path().filename());
        if (name.size() >= 5 && name.ends_with(".part"))
            continue;
        std::error_code size_failed;
        if (entry.is_regular_file(size_failed) && !size_failed &&
            entry.file_size(size_failed) > 0 && !size_failed)
            return true;
    }
    return false;
}

} // namespace

void Runtime::check_replay_viewer() {
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "needs the SDL presenter");
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder_.empty(),
        "the run has no folder of the player's own (--user-folder)"
    );

    preference_values_[std::string(first_run::preference_key)] =
        std::string(first_run::preference_done);
    preference_values_[std::string(kUpdateCheckKey)] = std::string(kCheckOffWord);
    preference_values_[std::string(kCatalogueCheckKey)] = std::string(kCheckOffWord);
    preferences_dirty_ = true;
    save_preferences();
    require(!preference_path_.empty(), "preferences have no path");
    const auto file_bytes = [](const fs::path& path) {
        std::ifstream input(path, std::ios::binary);
        if (!input)
            return std::vector<uint8_t>{};
        return std::vector<uint8_t>(
            std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()
        );
    };
    std::vector<uint8_t> preferences_before = file_bytes(preference_path_);
    require(!preferences_before.empty(), "preferences were not written");

    bool running = true;
    const auto frames = [&](int32_t count) {
        for (int32_t frame = 0; frame < count; ++frame) {
            // The fixed clock stays on the tick it has reached, so a frame
            // takes no step until this check carries a clock of its own.
            if (frame_run_clock_ns_)
                *frame_run_clock_ns_ += static_cast<uint64_t>(kFrameMs) * kNsPerMs;
            run_frame(running);
            require(running, "a frame ended the run");
        }
    };
    const auto named = [this](std::string_view name) -> std::optional<AutomationControl> {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls)
            if (control.name == name && control.visible && control.window_width > 0 &&
                control.window_height > 0)
                return control;
        return std::nullopt;
    };
    const auto require_named = [&](std::string_view name, std::string_view why) {
        const std::optional<AutomationControl> control = named(name);
        require(control.has_value(), why);
        return *control;
    };
    const auto pointer_at = [&](layout::Point pixel, SDL_EventType type, uint8_t button) {
        SDL_Event event = window_pointer_event(sdl_.window, sdl_.renderer, type, pixel, button);
        dispatch_event(event, running);
        require(running, "a pointer event ended the run");
    };
    const auto click_named = [&](std::string_view name) {
        const AutomationControl control = require_named(name, std::string(name) + " is not shown");
        const layout::Point pixel{
            control.window_x + control.window_width / 2,
            control.window_y + control.window_height / 2
        };
        pointer_at(pixel, SDL_EVENT_MOUSE_MOTION, 0);
        pointer_at(pixel, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
        pointer_at(pixel, SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
        frames(1);
    };
    const auto snapshot = [this](std::string_view step) {
        if (options_.snapshot.empty())
            return;
        oa_layer().read_whole_window(true);
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        oa_layer().read_whole_window(false);
        write_ppm(step_snapshot(options_.snapshot, step), presented);
    };
    const auto dismiss_welcome = [&] {
        if (LayerScreen* welcome = oa_layer().find("first-run")) {
            oa_layer().close(welcome);
            frames(1);
        }
    };
    const auto resize = [&](const CheckWindow& window) {
        require(
            SDL_SetWindowSize(sdl_.window, window.width, window.height) &&
                SDL_SyncWindow(sdl_.window),
            std::string("the window did not take ") + std::to_string(window.width) + 'x' +
                std::to_string(window.height)
        );
        apply_output_mode();
    };
    const auto wait_menu = [&] {
        for (int frame_index = 0; frame_index < 500; ++frame_index) {
            frames(1);
            if (screen_ == Screen::main_menu && match_ == nullptr)
                return;
        }
        require(false, "the match did not leave");
    };
    const auto wait_settled = [&](bool need_gap) {
        int stable = 0;
        uint32_t last_tick = 0;
        bool have_tick = false;
        bool saw_gap = !need_gap;
        for (int frame_index = 0; frame_index < 8000 && stable < 15; ++frame_index) {
            frames(1);
            require(match_ != nullptr, "the replay ended before its end card");
            const bool card = named("oa.replay-viewer.watch-again").has_value();
            if (!card)
                saw_gap = true;
            const uint32_t now_tick = match_->state().game.tick;
            if (card && saw_gap && have_tick && now_tick == last_tick)
                ++stable;
            else
                stable = 0;
            last_tick = now_tick;
            have_tick = true;
        }
        require(stable >= 15, "the end card did not settle");
    };
    const auto playback = [&] {
        ReplayPlaybackView view{};
        const bool filled = call_hook_or_report<&Extension::replay_state>(
            extension_, hook_error_report(), *this, &view
        );
        require(filled, "replay_state did not answer");
        return view;
    };
    const auto wait_tick = [&](uint32_t want, std::string_view why) {
        ReplayPlaybackView view{};
        for (int frame_index = 0; frame_index < 4000; ++frame_index) {
            view = playback();
            if (!view.skipping && view.catch_up_percent < 0 && view.tick == want)
                return;
            frames(1);
        }
        require(
            false,
            std::string(why) + " stayed at tick " + std::to_string(view.tick) + " catch-up " +
                std::to_string(view.catch_up_percent)
        );
    };
    const auto wait_asked = [&](uint32_t milli) {
        for (int frame_index = 0; frame_index < 60; ++frame_index) {
            const ReplayPlaybackView view = playback();
            if (!view.skipping && view.asked_milli == milli)
                return;
            frames(1);
        }
        require(false, "the speed did not become " + std::to_string(milli));
    };
    const auto wait_length = [&] {
        for (int frame_index = 0; frame_index < 200; ++frame_index) {
            if (playback().length_known >= 2400)
                return;
            frames(1);
        }
        require(false, "the recording's length never reached 2,400 ticks");
    };
    const auto key = [&](SDL_Keycode code, SDL_Keymod mods = SDL_KMOD_NONE) {
        PadAccess::send_key(*this, code, mods);
        frames(1);
    };
    const auto pad_down = [&](pad::PadButton button) {
        PadAccess::press(*this, button, frame_pacing::steady_now_ns());
        frames(1);
    };

    load(Screen::main_menu);
    require(screen_ == Screen::main_menu, "the main menu did not open");
    frames(2);
    dismiss_welcome();

    const fs::path recorded = recordings_folder(user_folder_, files_mod_id()) / "R08-check.tad";
    std::error_code made;
    fs::create_directories(recorded.parent_path(), made);
    require(!made, "the recordings folder was not made");
    netgame_context().options.net_record = recorded;
    // Two computers of one side: the second seat copies the first computer,
    // then takes a colour of its own. They share a side name and not a number.
    exercise_click(menu::resource_name(menu::Button::single_player));
    exercise_click(entry::resource_name(entry::Button::skirmish));
    state_.player_count = 3;
    require(map_player_capacity() >= 3, "the skirmish map lacks three start positions");
    auto& seats = skirmish_settings_.slots;
    seats[2] = seats[1];
    seats[2].controller = entry::controller::computer;
    for (int32_t color = 0; color < 10; ++color) {
        if (color != seats[0].color && color != seats[1].color) {
            seats[2].color = color;
            break;
        }
    }
    skirmish_settings_.slot_count = std::max(skirmish_settings_.slot_count, 3);
    exercise_click(skirmish::resource_name(skirmish::Button::start));
    require(screen_ == Screen::match && match_ != nullptr, "the skirmish did not start");
    // A check's fixed clock reads the tick it is already on, so the frames
    // below would draw and never step, and the skirmish would write nothing.
    const uint64_t clock_start_ns = frame_pacing::steady_now_ns();
    frame_run_clock_ns_ = clock_start_ns;
    match_timing_.previous_clock = oa::base::game_loop::scaled_clock(
        static_cast<uint32_t>(clock_start_ns / kNsPerMs), match_clock_scale()
    );
    match_timing_.remainder = 0.0F;
    uint32_t recorded_tick = 0;
    bool fight_planted = false;
    bool fight_struck = false;
    int32_t fight_x = 0;
    int32_t fight_z = 0;
    std::vector<uint16_t> fight_units;
    for (int frame_index = 0; frame_index < 8000; ++frame_index) {
        frames(1);
        if (match_ == nullptr || screen_ != Screen::match)
            break;
        recorded_tick = match_->state().game.tick;
        // A fight far from the commanders, so the index has a battle to follow
        // and the camera check can tell a start base from the fighting. One
        // frame passes between the units appearing and their destruction, so
        // the recording holds both.
        if (!fight_planted && recorded_tick >= 1650) {
            const oa::Game& game = match_->state().game;
            const int32_t map_w = game.map_pixel_width;
            const int32_t map_h = game.map_pixel_height;
            int32_t margin = 400;
            if (map_w < margin * 2 + 64)
                margin = std::max(map_w / 8, int32_t{16});
            if (map_h < margin * 2 + 64)
                margin = std::min(margin, std::max(map_h / 8, int32_t{16}));
            const int32_t left = std::min(margin, std::max(map_w - margin, int32_t{0}));
            const int32_t right = std::max(left, map_w - margin);
            const int32_t top = std::min(margin, std::max(map_h - margin, int32_t{0}));
            const int32_t bottom = std::max(top, map_h - margin);

            struct Spot {
                int32_t x{};
                int32_t z{};
                int64_t gap{};
            };

            std::array<Spot, 5> spots{{
                {left, top, 0},
                {right, top, 0},
                {left, bottom, 0},
                {right, bottom, 0},
                {(left + right) / 2, (top + bottom) / 2, 0},
            }};
            std::vector<std::pair<int32_t, int32_t>> commanders;
            for (const auto& slot : match_->world().slots) {
                if (slot.unit == nullptr)
                    continue;
                if (!oa::sim::match_runtime::side_commander(match_->state(), slot.record))
                    continue;
                commanders.emplace_back(
                    static_cast<int32_t>(slot.unit->position[0] >> 16),
                    static_cast<int32_t>(slot.unit->position[2] >> 16)
                );
            }
            for (Spot& spot : spots) {
                int64_t gap = std::numeric_limits<int64_t>::max();
                for (const auto& commander : commanders) {
                    const int64_t across = static_cast<int64_t>(spot.x) - commander.first;
                    const int64_t down = static_cast<int64_t>(spot.z) - commander.second;
                    gap = std::min(gap, across * across + down * down);
                }
                if (commanders.empty())
                    gap = 0;
                spot.gap = gap;
            }
            const auto farther = [](const Spot& left_spot, const Spot& right_spot) {
                return left_spot.gap > right_spot.gap;
            };
            std::sort(spots.begin(), spots.end(), farther);
            std::array<uint8_t, 2> players{{0, 1}};
            int seated = 0;
            for (int slot = 0; slot < OA_PLAYER_COUNT && seated < 2; ++slot) {
                if (game.players[slot].in_use == 0)
                    continue;
                players[static_cast<std::size_t>(seated)] = static_cast<uint8_t>(slot);
                ++seated;
            }
            if (seated == 1)
                players[1] = players[0];
            const std::array<std::string_view, 2> names{{"ARMPW", "CORAK"}};
            for (const Spot& spot : spots) {
                fight_units.clear();
                for (int side = 0; side < 2 && seated > 0; ++side) {
                    const uint16_t type = oa::sim::unit_spawn::find_type_index(
                        spawn_type_names_, names[static_cast<std::size_t>(side)]
                    );
                    if (type == 0)
                        continue;
                    for (int index = 0; index < 2; ++index) {
                        const int32_t x = std::clamp(
                            spot.x + (side == 0 ? -40 : 40), int32_t{0}, std::max(map_w - 1, 0)
                        );
                        const int32_t z = std::clamp(
                            spot.z + (index == 0 ? -24 : 24), int32_t{0}, std::max(map_h - 1, 0)
                        );
                        const auto across = static_cast<uint32_t>(x);
                        const auto down = static_cast<uint32_t>(z);
                        oa::sim::unit_spawn::Request request;
                        request.player = players[static_cast<std::size_t>(side)];
                        request.type = type;
                        request.finished = true;
                        request.state = kGroundOccupancyState;
                        request.position = {
                            across << 16,
                            static_cast<uint32_t>(match_->map_height(across << 16, down << 16))
                                << 16,
                            down << 16,
                        };
                        const auto* placed = match_->create(request);
                        if (placed == nullptr || placed->unit == nullptr)
                            continue;
                        fight_units.push_back(placed->unit_index);
                    }
                }
                if (!fight_units.empty()) {
                    fight_x = spot.x;
                    fight_z = spot.z;
                    fight_planted = true;
                    break;
                }
            }
        } else if (fight_planted && !fight_struck) {
            for (uint16_t unit : fight_units)
                match_->kill_unit(
                    unit, static_cast<uint8_t>(oa::sim::match_runtime::DeathKind::weapon)
                );
            fight_struck = true;
        }
        if (recorded_tick >= 2400)
            break;
    }
    {
        NetworkPlay* play = NetworkPlay::find(*this);
        require(play != nullptr, "the skirmish has no recorder");
        play->note_skirmish_departure();
    }
    require(
        recorded_tick >= 2400,
        "the skirmish reached tick " + std::to_string(recorded_tick) + ", under 2,400"
    );
    require(fight_planted && fight_struck, "the recording has no fight for the Director");
    std::cout << "replay viewer: recorded " << recorded_tick << " ticks, fight at " << fight_x
              << ", " << fight_z << '\n';
    leave_match();
    // leave_match drops the match and leaves the screen on it. The menu is
    // loaded before the next frame, which would otherwise draw no match.
    load(Screen::main_menu);
    uint64_t last_size = 0;
    int size_stable = 0;
    for (int frame_index = 0; frame_index < 500 && size_stable < 5; ++frame_index) {
        frames(1);
        std::error_code size_failed;
        const bool present = fs::is_regular_file(recorded, size_failed);
        const uint64_t size = present && !size_failed
                                  ? static_cast<uint64_t>(fs::file_size(recorded, size_failed))
                                  : 0;
        if (!size_failed && size > 0 && size == last_size && screen_ == Screen::main_menu &&
            match_ == nullptr)
            ++size_stable;
        else
            size_stable = 0;
        last_size = size;
    }
    require(size_stable >= 5, "the skirmish was not recorded");
    netgame_context().options.net_record.clear();

    const auto open_playback = [&](bool pin) {
        std::ifstream stream(recorded, std::ios::binary);
        require(static_cast<bool>(stream), "the recording did not open");
        const std::vector<uint8_t> bytes{
            std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()
        };
        require(!bytes.empty(), "the recording is empty");
        replay_recording_path_ = recorded;
        const std::string name = "R08-check.tad";
        const RecordingInput input{name.c_str(), bytes.data(), bytes.size(), true};
        ReplayHooks hooks{};
        RecordingInfo info{};
        bool opened = false;
        try {
            opened = call_hook_or_raise<&Extension::open_recording>(
                extension_, *this, input, hooks, info
            );
        } catch (const std::exception& open_error) {
            throw std::runtime_error(
                std::string("replay viewer check: the recording did not play: ") + open_error.what()
            );
        }
        require(opened, "no extension played the recording");
        for (int frame_index = 0; frame_index < 400; ++frame_index) {
            frames(1);
            if (screen_ == Screen::match && match_ != nullptr &&
                (current_extension_state() & extension_state::replay) != 0) {
                if (pin)
                    require(replay_viewer_command("watch.replay-bar"), "the bar did not pin");
                return;
            }
        }
        require(false, "the replay did not start");
    };

    open_playback(false);
    {
        bool saw_pass = false;
        bool preferences_at_pass = false;
        const auto sample_pass = [&] {
            const int pid = replay_index_pid();
            if (pid <= 0)
                return;
            saw_pass = true;
            // The recording and the screen change write the player's file
            // before the pass is up. From the first moment the pass is
            // running, that file must stay as it is.
            if (!preferences_at_pass) {
                preferences_before = file_bytes(preference_path_);
                preferences_at_pass = true;
            }
#ifndef _WIN32
            errno = 0;
            const int nice = getpriority(PRIO_PROCESS, static_cast<id_t>(pid));
            require(
                errno == 0 && nice == 10,
                "the index pass is not at nice 10 (nice " + std::to_string(nice) + ")"
            );
#endif
            require(
                file_bytes(preference_path_) == preferences_before,
                "the index pass changed the player's preferences"
            );
        };
        sample_pass();
        const uint64_t started_by = frame_pacing::steady_now_ns() + 3'000'000'000ULL;
        while (!saw_pass && frame_pacing::steady_now_ns() < started_by) {
            frames(1);
            sample_pass();
        }
        require(saw_pass, "the index pass did not start");
        const uint64_t finished_by = frame_pacing::steady_now_ns() + 15'000'000'000ULL;
        while (replay_index_pid() != 0 && frame_pacing::steady_now_ns() < finished_by)
            frames(1);
        require(replay_index_pid() == 0, "the index pass did not finish");
        // The pass writes a line each thousand ticks, counted against the
        // recording's own length. A count against anything longer stays at 0.
        require(
            replay_finding_percent() > 0,
            "the pass's progress stayed at " + std::to_string(replay_finding_percent()) + " %"
        );
        require(
            file_bytes(preference_path_) == preferences_before,
            "the index pass changed the player's preferences"
        );
    }
    wait_settled(false);
    const int32_t straight_camera_x = match_camera_x_;
    const int32_t straight_camera_z = match_camera_z_;
    // The picture's place is part of the digest. Both plays are read from
    // Free at the map's corner, with no frame between the move and the read.
    set_camera_position(0, 0, 0);
    const uint64_t straight = match_world_digest();
    std::cout << "replay viewer straight " << digest_text(straight) << '\n';
    leave_match();
    load(Screen::main_menu);
    wait_menu();

    open_playback(true);
    require(oa_layer().find("replay-viewer") != nullptr, "the bar did not open");
    std::string colour_name;
    int colour_number = -1;
    for (int frame_index = 0; frame_index < 8 && colour_name.empty() && match_ != nullptr;
         ++frame_index) {
        const oa::World& world = match_->state();
        for (const oa::Player& player : world.game.players) {
            if (player.in_use == 0)
                continue;
            const oa::PlayerSetupInfo* info = oa::world_player_info(&world, &player);
            if (info == nullptr)
                continue;
            std::size_t length = 0;
            while (length < sizeof(player.name) && player.name[length] != '\0')
                ++length;
            if (length == 0)
                continue;
            colour_name.assign(player.name, length);
            colour_number = static_cast<int>(info->color);
            break;
        }
        if (colour_name.empty())
            frames(1);
    }
    require(!colour_name.empty(), "the match named no player colour");
    for (const CheckWindow& window : kWindows) {
        resize(window);
        frames(2);
        std::ostringstream step;
        step << window.width << 'x' << window.height;
        snapshot(step.str());
    }
    require(
        file_bytes(preference_path_) == preferences_before,
        "the index pass changed the player's preferences"
    );
    set_language_choice("de");
    resize(CheckWindow{640, 480});
    frames(2);
    snapshot("640x480-de");
    set_language_choice("en");
    resize(CheckWindow{1280, 720});
    frames(2);

    wait_length();
    {
        const bool paused = playback().paused || match_paused_;
        const AutomationControl play = require_named("oa.replay-viewer.play", "Play is not shown");
        require(play.text == (paused ? "Play" : "Pause"), "Play is not named from the catalogue");
        require(play.label == play.text, "Play's label differs from its text");
        const AutomationControl back = require_named("oa.replay-viewer.back", "Back is not shown");
        require(
            back.text == "Back 10 seconds" && back.label == back.text,
            "Back is not named from the catalogue"
        );
        const AutomationControl forward =
            require_named("oa.replay-viewer.forward", "Forward is not shown");
        require(
            forward.text == "Forward 10 seconds" && forward.label == forward.text,
            "Forward is not named from the catalogue"
        );
    }

    if (playback().paused)
        key(SDLK_PAUSE);
    require(!playback().paused, "the replay did not start playing");
    {
        const uint32_t before = playback().tick;
        key(SDLK_PAUSE);
        const ReplayPlaybackView held = playback();
        require(held.paused, "Pause did not pause");
        frames(3);
        const ReplayPlaybackView still = playback();
        require(still.paused && still.tick == held.tick, "Pause did not hold the tick");
        require(still.tick == before || still.tick == held.tick, "Pause moved the tick");
        key(SDLK_PAUSE);
        require(!playback().paused, "Pause did not play again");
        key(SDLK_K);
        require(playback().paused, "K did not pause");
        frames(2);
        require(playback().paused, "K did not hold the pause");
        key(SDLK_K);
        require(!playback().paused, "K did not play again");
    }

    click_named("oa.replay-viewer.leave");
    frames(2);
    require(pause_menu_shown(), "Leave did not open the in-game menu");
    {
        const AutomationControl play =
            require_named("oa.replay-viewer.play", "Play hid in the menu");
        require(play.text == "Play" && !play.checked, "the menu did not show Play");
    }
    resume_match_pause();
    frames(2);
    require(!pause_menu_shown(), "the in-game menu stayed open");

    wait_tick(playback().tick, "a settle");
    key(SDLK_HOME, SDL_KMOD_CTRL);
    wait_tick(0, "Ctrl+Home");
    key(SDLK_L);
    wait_tick(kShortTicks, "L");
    key(SDLK_L, SDL_KMOD_SHIFT);
    wait_tick(kShortTicks + kLongTicks, "Shift+L");
    key(SDLK_J);
    wait_tick(kLongTicks, "J");
    key(SDLK_J, SDL_KMOD_SHIFT);
    wait_tick(0, "Shift+J");

    pad_down(pad::PadButton::r1);
    wait_tick(kShortTicks, "R1");
    pad_down(pad::PadButton::l1);
    wait_tick(0, "L1");
    {
        const uint32_t before = playback().tick;
        pad_down(pad::PadButton::a);
        const ReplayPlaybackView held = playback();
        require(held.paused, "pad A did not pause");
        frames(3);
        require(playback().paused && playback().tick == held.tick, "pad A did not hold the tick");
        require(held.tick == before || held.tick == playback().tick, "pad A moved the tick");
        pad_down(pad::PadButton::a);
        require(!playback().paused, "pad A did not play");
    }
    pad_down(pad::PadButton::r2);
    wait_asked(2000);
    pad_down(pad::PadButton::l2);
    wait_asked(1000);
    pad_down(pad::PadButton::l2);
    wait_asked(500);
    pad_down(pad::PadButton::r2);
    wait_asked(1000);

    for (const SpeedStep& speed : kSpeeds) {
        if (!named(speed.name))
            click_named("oa.replay-viewer.speed");
        click_named(speed.name);
        wait_asked(speed.milli);
    }
    if (!named("oa.replay-viewer.speed-1000"))
        click_named("oa.replay-viewer.speed");
    click_named("oa.replay-viewer.speed-1000");
    wait_asked(1000);

    key(SDLK_HOME, SDL_KMOD_CTRL);
    wait_tick(0, "Ctrl+Home before the drag");
    {
        const AutomationControl timeline =
            require_named("oa.replay-viewer.timeline", "the timeline is not shown");
        require(timeline.window_width > 1, "the timeline has no width");
        const uint32_t length = playback().length_known;
        const int32_t along_y = timeline.window_y + timeline.window_height / 2;
        const int32_t from_x = timeline.window_x;
        const int32_t to_x = timeline.window_x + timeline.window_width * 2 / 5;
        const int32_t local = to_x - timeline.window_x;
        const uint32_t want = static_cast<uint32_t>(
            (static_cast<uint64_t>(local) * length) /
            static_cast<uint64_t>(timeline.window_width - 1)
        );
        require(want > 0 && want < length, "the drag's tick is not inside the recording");
        const layout::Point from_point{from_x, along_y};
        const layout::Point to_point{to_x, along_y};
        pointer_at(from_point, SDL_EVENT_MOUSE_MOTION, 0);
        pointer_at(from_point, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT);
        pointer_at(to_point, SDL_EVENT_MOUSE_MOTION, 0);
        ReplayPlaybackView mid = playback();
        bool saw_catch_up = mid.catch_up_percent >= 0;
        if (!saw_catch_up && mid.tick != want) {
            frames(1);
            mid = playback();
            saw_catch_up = mid.catch_up_percent >= 0;
        }
        pointer_at(to_point, SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT);
        require(saw_catch_up || mid.tick == want, "the drag showed no catch-up");
        wait_tick(want, "the drag");
    }

    const layout::Point above{640, 80};
    pad_down(pad::PadButton::b);
    pointer_at(above, SDL_EVENT_MOUSE_MOTION, 0);
    SDL_Delay(3200);
    frames(1);
    require(!named("oa.replay-viewer.play"), "the bar did not hide after 3 seconds");
    pad_down(pad::PadButton::b);
    SDL_Delay(4000);
    frames(1);
    require(named("oa.replay-viewer.play").has_value(), "B did not keep the bar");
    pad_down(pad::PadButton::b);
    pointer_at(above, SDL_EVENT_MOUSE_MOTION, 0);
    SDL_Delay(3200);
    frames(1);
    require(!named("oa.replay-viewer.play"), "the bar stayed after the pin came off");
    TouchDispatchAccess::battlefield_tap(*this, 640.0F, 200.0F, 1);
    frames(1);
    require(named("oa.replay-viewer.play").has_value(), "a tap did not show the bar");

    key(SDLK_SLASH, SDL_KMOD_SHIFT);
    {
        const AutomationControl card =
            require_named("oa.replay-viewer.keys-card", "? did not open the shortcut card");
        require(card.text.find("F4") != std::string::npos, "the shortcut card does not list F4");
    }
    key(SDLK_SLASH, SDL_KMOD_SHIFT);
    require(!named("oa.replay-viewer.keys-card"), "? did not close the shortcut card");

    if (!playback().paused)
        key(SDLK_PAUSE);
    require(playback().paused, "the replay did not pause for the index");
    bool index_ready = false;
    for (int frame_index = 0; frame_index < 4000 && !index_ready; ++frame_index) {
        frames(1);
        index_ready = replay_index_arrived();
    }
    require(index_ready, "the index pass did not arrive");
    {
        // A paused frame, then the same frame after Tab. The view is
        // presentation: the world's bytes, including the recorded viewpoint,
        // stay as they were.
        const auto sample_world = [&] {
            const oa::World& world = match_->state();
            const oa::Game& game = world.game;
            struct Sample {
                uint8_t viewpoint{};
                bool radar{};
                uint32_t flags{};
                uint32_t sight{};
                std::vector<uint8_t> players{};
            };
            Sample taken;
            taken.viewpoint = game.viewpoint_player;
            taken.radar = match_->slotless_full_radar();
            taken.flags = 2166136261u;
            if (world.units != nullptr) {
                // The low twelve bits are the unit flags a tick writes.
                constexpr uint32_t unit_flag_low = 0xfffu;
                for (uint32_t index = 0; index < world.unit_slot_count; ++index) {
                    taken.flags ^= world.units[index].flags & unit_flag_low;
                    taken.flags *= 16777619u;
                }
            }
            taken.sight = 2166136261u;
            const auto& bits = match_->sight().player_bits;
            const auto* raw = reinterpret_cast<const uint8_t*>(bits.data());
            const std::size_t bytes = bits.size() * sizeof(bits[0]);
            for (std::size_t index = 0; index < bytes; ++index) {
                taken.sight ^= raw[index];
                taken.sight *= 16777619u;
            }
            taken.players.resize(sizeof game.players);
            std::memcpy(taken.players.data(), &game.players, sizeof game.players);
            return taken;
        };
        const auto world_change = [](const auto& left, const auto& right) -> std::string {
            if (left.viewpoint != right.viewpoint)
                return "viewpoint";
            if (left.radar != right.radar)
                return "radar";
            if (left.flags != right.flags)
                return "unit flags";
            if (left.sight != right.sight)
                return "sight";
            if (left.players != right.players) {
                std::size_t at = 0;
                const std::size_t count = std::min(left.players.size(), right.players.size());
                while (at < count && left.players[at] == right.players[at])
                    ++at;
                return "players at offset " + std::to_string(at);
            }
            return {};
        };
        const auto held = sample_world();
        frames(1);
        {
            const std::string change = world_change(sample_world(), held);
            require(change.empty(), "a paused frame changed the replay (" + change + ")");
        }
        key(SDLK_TAB);
        {
            const std::string change = world_change(sample_world(), held);
            require(change.empty(), "a view switch changed the replay (" + change + ")");
        }
        key(SDLK_Q);
        {
            const std::string change = world_change(sample_world(), held);
            require(
                change.empty(), "returning to every player changed the replay (" + change + ")"
            );
        }
    }
    key(SDLK_PAUSE);
    require(!playback().paused, "the replay stayed paused after the index");

    const int32_t kept_camera_x = match_camera_x_;
    const int32_t kept_camera_z = match_camera_z_;
    if (!named("oa.replay-viewer.view")) {
        TouchDispatchAccess::battlefield_tap(*this, 640.0F, 200.0F, 1);
        frames(1);
    }
    call_hook_or_report<&Extension::replay_set_speed>(
        extension_, hook_error_report(), *this, int32_t{16000}
    );
    wait_asked(16000);
    key(SDLK_HOME, SDL_KMOD_CTRL);
    wait_tick(0, "home before the views");
    frames(2);
    key(SDLK_TAB);
    frames(2);
    require(
        require_named("oa.replay-viewer.view", "View is not shown").text != "All players",
        "Tab stayed on every player"
    );
    for (int frame_index = 0; frame_index < 8000 && playback().tick < 1800; ++frame_index)
        frames(1);
    require(playback().tick >= 1800, "a simulated minute did not pass");
    // 16× spends the fight in a few frames. Hold the moment the minute
    // lands, back on that minute, and read the Director there. Past it the
    // picture follows a later fight, or a quiet stretch.
    if (!playback().paused)
        key(SDLK_PAUSE);
    require(playback().paused, "the replay did not pause on the fight");
    if (playback().tick != kLongTicks) {
        call_hook_or_report<&Extension::replay_seek>(
            extension_, hook_error_report(), *this, kLongTicks
        );
        wait_tick(kLongTicks, "the fight");
    }
    key(SDLK_TAB);
    frames(2);
    key(SDLK_Q);
    frames(2);
    require(
        require_named("oa.replay-viewer.view", "View is not shown").text == "All players",
        "Q did not show every player"
    );
    click_named("oa.replay-viewer.camera");
    frames(1);
    bool saw_their_camera = false;
    bool saw_director = false;
    bool saw_follow_unit = false;
    for (int item = 0; item < 4; ++item) {
        const std::optional<AutomationControl> row =
            named("oa.replay-viewer.camera-item." + std::to_string(item));
        if (!row)
            continue;
        if (row->text == "Follow their camera")
            saw_their_camera = true;
        if (row->text == "Director")
            saw_director = true;
        if (row->text == "Follow a unit")
            saw_follow_unit = true;
    }
    require(!saw_their_camera, "Follow their camera was offered for a skirmish recording");
    require(saw_director, "Director was not offered");
    require(!saw_follow_unit, "Follow a unit was offered in the menu");
    const auto choose_camera = [&](std::string_view caption) {
        bool open = false;
        for (int item = 0; item < 4; ++item) {
            if (named("oa.replay-viewer.camera-item." + std::to_string(item)))
                open = true;
        }
        if (!open)
            click_named("oa.replay-viewer.camera");
        for (int item = 0; item < 4; ++item) {
            const std::string name = "oa.replay-viewer.camera-item." + std::to_string(item);
            const std::optional<AutomationControl> row = named(name);
            if (!row || row->text != caption)
                continue;
            click_named(name);
            frames(2);
            require(
                require_named("oa.replay-viewer.camera", "Camera is not shown").text == caption,
                std::string(caption) + " did not become the camera"
            );
            return;
        }
        require(false, std::string(caption) + " was not in the camera menu");
    };
    choose_camera("Director");
    frames(4);
    {
        const int64_t centre_x = static_cast<int64_t>(match_camera_x_) + visible_map_width() / 2;
        const int64_t centre_z = static_cast<int64_t>(match_camera_z_) + visible_map_height() / 2;
        const int64_t across = centre_x - fight_x;
        const int64_t down = centre_z - fight_z;
        const int64_t reach = std::max(visible_map_width(), visible_map_height());
        require(
            across * across + down * down <= reach * reach,
            "Director was not on the fight at tick " + std::to_string(playback().tick)
        );
    }
    choose_camera("Free");
    key(SDLK_C);
    frames(2);
    require(
        require_named("oa.replay-viewer.camera", "Camera is not shown").text == "Director",
        "C did not leave Free for Director"
    );
    key(SDLK_C);
    frames(2);
    require(
        require_named("oa.replay-viewer.camera", "Camera is not shown").text == "Free",
        "C did not leave Director for Free"
    );
    if (playback().paused)
        key(SDLK_PAUSE);
    require(!playback().paused, "the replay stayed paused after the cameras");
    uint16_t followed = 0;
    if (match_ != nullptr) {
        const auto& unit_slots = match_->world().slots;
        for (std::size_t index = 1; index < unit_slots.size(); ++index) {
            const auto id = static_cast<uint16_t>(index);
            if (!match_unit_present(id))
                continue;
            followed = id;
            break;
        }
    }
    require(followed != 0, "the replay had no unit to follow");
    replay_follow_unit(followed);
    frames(2);
    require(
        require_named("oa.replay-viewer.camera", "Camera is not shown").text == "Follow a unit",
        "the followed unit did not take the camera"
    );
    frame_run_clock_ns_ = frame_pacing::steady_now_ns();
    frame_run_scroll_ = 1;
    frames(2);
    frame_run_scroll_ = 0;
    frames(1);
    require(
        require_named("oa.replay-viewer.camera", "Camera is not shown").text == "Free",
        "panning did not leave the followed unit"
    );
    key(SDLK_F4);
    frames(1);
    require(named("oa.replay-viewer.watch-chip.0").has_value(), "F4 did not fold the player list");
    key(SDLK_F4);
    frames(1);
    require(named("oa.replay-viewer.watch-row.0").has_value(), "F4 did not open the player list");
    {
        // Each row is its own seat. Two computers of one side share a name.
        // Tab reaches every one of them, and only the row on show has the eye.
        const auto row_at = [&](int index) {
            return named("oa.replay-viewer.watch-row." + std::to_string(index));
        };
        const auto eyes_of = [&] {
            int rows = 0;
            int eyes = 0;
            for (int index = 0; index < OA_PLAYER_COUNT + 1; ++index) {
                const std::optional<AutomationControl> row = row_at(index);
                if (!row)
                    continue;
                ++rows;
                if (row->checked)
                    ++eyes;
            }
            return std::pair<int, int>{rows, eyes};
        };
        const auto counted = eyes_of();
        require(counted.first >= 4, "the list has fewer than four rows");
        require(counted.second == 1, "the open list does not have one eye");

        struct Seen {
            std::string name;
            uint8_t slot{};
            uint8_t colour{};
            uint16_t units{};
            bool watcher{};
        };

        std::vector<Seen> seen;
        for (int index = 0; index < counted.first; ++index) {
            const std::string control = "oa.replay-viewer.watch-row." + std::to_string(index);
            const AutomationControl before = require_named(control, "a watch row vanished");
            click_named(control);
            const std::optional<AutomationControl> after = named(control);
            require(after.has_value() && after->checked, "the clicked row is not the eye");
            const auto again = eyes_of();
            require(again.second == 1, "a row click left more than one eye");
            const uint8_t slot = match_view_player();
            require(slot < OA_PLAYER_COUNT, "the view names no player");
            const oa::World& world = match_->state();
            const oa::Player& player = world.game.players[slot];
            const oa::PlayerSetupInfo* info = oa::world_player_info(&world, &player);
            require(info != nullptr, "the viewed player has no setup");
            Seen one;
            one.name = before.text;
            const std::string::size_type left = one.name.find(" Left");
            const std::string::size_type defeated = one.name.find(" Defeated");
            const std::string::size_type cut = std::min(left, defeated);
            if (cut != std::string::npos)
                one.name.resize(cut);
            one.slot = slot;
            one.colour = info->color;
            one.units = player.unit_count;
            one.watcher = (info->options & OA_SETUP_OPTION_WATCHER) != 0;
            seen.push_back(std::move(one));
        }
        int seated = 0;
        for (const Seen& one : seen) {
            if (one.watcher || one.name == "All players")
                continue;
            ++seated;
            require(one.units >= 1, one.name + " shows no units");
        }
        require(seated >= 3, "fewer than three players took a view");
        bool shared_name = false;
        for (std::size_t left = 0; left < seen.size(); ++left) {
            if (seen[left].watcher || seen[left].name == "All players")
                continue;
            for (std::size_t right = left + 1; right < seen.size(); ++right) {
                if (seen[right].watcher || seen[left].name != seen[right].name)
                    continue;
                shared_name = true;
                require(seen[left].slot != seen[right].slot, "two rows of one name share a view");
                require(
                    seen[left].colour != seen[right].colour, "two rows of one name share a colour"
                );
            }
        }
        require(shared_name, "no two computers of one side share a row name");
        key(SDLK_Q);
        frames(1);
        std::array<bool, OA_PLAYER_COUNT> visited{};
        int visited_seated = 0;
        for (int step = 0; step < counted.first; ++step) {
            key(SDLK_TAB);
            frames(1);
            require(eyes_of().second == 1, "Tab left more than one eye");
            const uint8_t slot = match_view_player();
            if (slot >= OA_PLAYER_COUNT || visited[slot])
                continue;
            const oa::World& world = match_->state();
            const oa::Player& player = world.game.players[slot];
            const oa::PlayerSetupInfo* info = oa::world_player_info(&world, &player);
            if (info == nullptr || (info->options & OA_SETUP_OPTION_WATCHER) != 0)
                continue;
            visited[slot] = true;
            ++visited_seated;
        }
        require(visited_seated >= seated, "Tab did not reach every player");
    }
    replay_reset_watch();
    set_camera_position(kept_camera_x, kept_camera_z, 0);
    call_hook_or_report<&Extension::replay_set_speed>(
        extension_, hook_error_report(), *this, int32_t{1000}
    );
    wait_asked(1000);
    key(SDLK_HOME, SDL_KMOD_CTRL);
    wait_tick(0, "home after the views");
    set_camera_position(kept_camera_x, kept_camera_z, 0);
    frames(1);

    key(SDLK_HOME, SDL_KMOD_CTRL);
    wait_tick(0, "Ctrl+Home before the markers");
    const std::optional<uint32_t> next_marker = replay_marker_after(0);
    require(next_marker.has_value(), "the timeline has no marker after the start");
    key(SDLK_PERIOD);
    wait_tick(*next_marker, "period");
    std::cout << "replay viewer: period landed on marker " << *next_marker << '\n';
    if (const std::optional<uint32_t> previous = replay_marker_before(*next_marker)) {
        key(SDLK_COMMA);
        wait_tick(*previous, "comma");
        std::cout << "replay viewer: comma landed on marker " << *previous << '\n';
        pad_down(pad::PadButton::dpad_right);
        wait_tick(*next_marker, "the d-pad right");
        pad_down(pad::PadButton::dpad_left);
        wait_tick(*previous, "the d-pad left");
    } else {
        std::cout << "replay viewer: one marker, comma has none before it\n";
        key(SDLK_HOME, SDL_KMOD_CTRL);
        wait_tick(0, "Ctrl+Home before the d-pad");
        pad_down(pad::PadButton::dpad_right);
        wait_tick(*next_marker, "the d-pad right");
    }

    if (playback().tick != playback().length_known) {
        key(SDLK_END, SDL_KMOD_CTRL);
        wait_tick(playback().length_known, "Ctrl+End");
    }
    wait_settled(false);
    require(
        replay_named_colour(colour_name) == colour_number,
        "the end card lost " + colour_name + "'s colour"
    );
    // A jump back from the end lands on the tick asked for, not on the
    // keyframe before it, and the bar's time shows where it landed.
    {
        const uint32_t end_tick = playback().tick;
        if (!playback().paused)
            key(SDLK_PAUSE);
        require(playback().paused, "the replay did not pause at its end");
        key(SDLK_J);
        wait_tick(end_tick - kShortTicks, "J from the end");
        frames(2);
        const AutomationControl timeline =
            require_named("oa.replay-viewer.timeline", "the timeline is not shown");
        const std::string landed =
            oa::ui::replay_viewer::time_text(end_tick - kShortTicks, end_tick);
        require(
            timeline.text.substr(0, timeline.text.find(" / ")) ==
                landed.substr(0, landed.find(" / ")),
            "the bar's time reads " + timeline.text + " after J from the end"
        );
        key(SDLK_PAUSE);
        require(!playback().paused, "the replay stayed paused after J from the end");
        key(SDLK_END, SDL_KMOD_CTRL);
        wait_tick(playback().length_known, "Ctrl+End after J");
        wait_settled(false);
    }

    click_named("oa.replay-viewer.export");
    frames(2);
    require(library_host().model.export_dialog.open, "Export did not open");
    const fs::path exported = user_folder_ / "R08-export";
    std::error_code export_made;
    fs::create_directories(exported, export_made);
    require(!export_made, "the export folder was not made");
    library::set_export_folder(
        library_host().model, oa::platform::path_to_utf8(exported), "R08-export", false
    );
    library::set_export_format(library_host().model, library::ExportFormat::tad);
    for (int frame_index = 0;
         frame_index < 400 && !library::export_can_run(library_host().model.export_dialog);
         ++frame_index)
        frames(1);
    require(library::export_can_run(library_host().model.export_dialog), "Export was not ready");
    click_named("oa.library.export.format.tad");
    click_named("oa.library.export.export");
    bool wrote = false;
    for (int frame_index = 0; frame_index < 600 && !wrote; ++frame_index) {
        frames(1);
        wrote = exported_file(exported);
    }
    require(wrote, "Export wrote no file");
    for (int frame_index = 0; frame_index < 200 && !named("oa.library.export.close"); ++frame_index)
        frames(1);
    click_named("oa.library.export.close");
    frames(1);

    // The compared run starts where the straight one did: every player, Free,
    // 1x, and that run's camera. Watch again keeps the view, so nothing here
    // calls replay_reset_watch. The switches below are only on the way
    // forward. Q and the straight camera go back once the end card has
    // settled, with no further frame, and the digests must match.
    call_hook_or_report<&Extension::replay_set_speed>(
        extension_, hook_error_report(), *this, int32_t{1000}
    );
    wait_asked(1000);
    key(SDLK_Q);
    choose_camera("Free");
    // The pin toggles. Whichever way it was, one press and a look after the
    // hide delay leaves it holding the bar for the compared run.
    require(replay_viewer_command("watch.replay-bar"), "the bar did not answer");
    SDL_Delay(3200);
    frames(1);
    if (!named("oa.replay-viewer.play"))
        require(replay_viewer_command("watch.replay-bar"), "the bar did not pin");
    set_camera_position(straight_camera_x, straight_camera_z, 0);
    click_named("oa.replay-viewer.watch-again");

    bool tabbed = false;
    bool shifted = false;
    bool whole = false;
    bool stepped_view = false;
    bool unit_clicked = false;
    bool tabbed_minute = false;
    uint32_t next_minute = kLongTicks;
    int stable = 0;
    uint32_t last_tick = 0;
    bool have_tick = false;
    bool saw_gap = false;
    for (int frame_index = 0; frame_index < 12000 && stable < 15; ++frame_index) {
        const ReplayPlaybackView view = playback();
        const bool card = named("oa.replay-viewer.watch-again").has_value();
        const bool playing =
            !view.skipping && view.catch_up_percent < 0 && !card && match_ != nullptr;
        if (playing) {
            const uint32_t tick = view.tick;
            if (!tabbed) {
                key(SDLK_TAB);
                tabbed = true;
                require(
                    require_named("oa.replay-viewer.view", "View is not shown").text !=
                        "All players",
                    "Tab at the start stayed on every player"
                );
                continue;
            }
            if (!shifted && tick >= 120) {
                key(SDLK_TAB, SDL_KMOD_SHIFT);
                shifted = true;
                continue;
            }
            if (!whole && tick >= 240) {
                key(SDLK_Q);
                whole = true;
                require(
                    require_named("oa.replay-viewer.view", "View is not shown").text ==
                        "All players",
                    "Q did not show every player on the compared run"
                );
                require(
                    require_named("oa.replay-viewer.camera", "Camera is not shown").text == "Free",
                    "Q moved the camera off Free"
                );
                continue;
            }
            if (!stepped_view && tick >= 400) {
                pad_down(pad::PadButton::y);
                stepped_view = true;
                continue;
            }
            if (!unit_clicked && tick >= 1120) {
                bool clicked = false;
                const auto& unit_slots = match_->world().slots;
                for (std::size_t index = 1; index < unit_slots.size(); ++index) {
                    const auto id = static_cast<uint16_t>(index);
                    if (!match_unit_present(id) || unit_slots[id].unit == nullptr)
                        continue;
                    const auto* unit = unit_slots[id].unit;
                    const int32_t map_x = static_cast<int32_t>(unit->position[0] >> 16);
                    const int32_t map_z = static_cast<int32_t>(unit->position[2] >> 16);
                    set_camera_position(
                        map_x - visible_map_width() / 2, map_z - visible_map_height() / 2, 0
                    );
                    const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
                    const auto point = project_match_point(viewport, unit->position);
                    const float pixel_x = static_cast<float>(point.x);
                    const float pixel_y = static_cast<float>(point.y);
                    if (!battlefield_contains(pixel_x, pixel_y))
                        continue;
                    update_pointer(pixel_x, pixel_y);
                    if (hovered_match_unit_ != id)
                        continue;
                    // A real double-click is the first press, then the second.
                    handle_match_left_click(pixel_x, pixel_y, 1);
                    handle_match_left_click(pixel_x, pixel_y, 2);
                    clicked = true;
                    break;
                }
                require(clicked, "no unit took the double-click");
                unit_clicked = true;
                frames(2);
                require(
                    require_named("oa.replay-viewer.camera", "Camera is not shown").text ==
                        "Follow a unit",
                    "the double-click did not follow a unit"
                );
                continue;
            }
            if (tick >= next_minute) {
                key(SDLK_TAB);
                tabbed_minute = true;
                next_minute += kLongTicks;
                continue;
            }
        }
        frames(1);
        require(match_ != nullptr, "the replay ended before its end card");
        const bool settled_card = named("oa.replay-viewer.watch-again").has_value();
        if (!settled_card)
            saw_gap = true;
        const uint32_t now_tick = match_->state().game.tick;
        if (settled_card && saw_gap && have_tick && now_tick == last_tick)
            ++stable;
        else
            stable = 0;
        last_tick = now_tick;
        have_tick = true;
    }
    require(tabbed, "the compared run did not press Tab at the start");
    require(shifted, "the compared run did not press Shift+Tab");
    require(whole, "the compared run did not press Q");
    require(stepped_view, "the compared run did not press pad Y");
    require(unit_clicked, "the compared run did not double-click a unit");
    require(tabbed_minute, "the compared run did not press Tab at a simulated minute");
    require(stable >= 15, "the switched play did not settle");
    require(
        replay_named_colour(colour_name) == colour_number,
        "watch again lost " + colour_name + "'s colour"
    );
    require(replay_viewer_command("watch.whole-view"), "Q did not return every player");
    choose_camera("Free");
    set_camera_position(0, 0, 0);
    const uint64_t again = match_world_digest();
    std::cout << "replay viewer straight " << digest_text(straight) << '\n';
    std::cout << "replay viewer switched " << digest_text(again) << '\n';
    require(
        again == straight,
        "the digest changed: " + digest_text(straight) + " then " + digest_text(again)
    );
    bool saw_left = false;
    for (int row = 0; row < OA_PLAYER_COUNT + 1; ++row) {
        const std::string index_text = std::to_string(row);
        for (const char* kind : {"watch-row.", "watch-chip."}) {
            const std::optional<AutomationControl> control =
                named(std::string("oa.replay-viewer.") + kind + index_text);
            if (control && control->text.find("Left") != std::string::npos)
                saw_left = true;
        }
    }
    require(saw_left, "no row says the player left");

    leave_match();
    load(Screen::main_menu);
    wait_menu();
    dismiss_welcome();
    open_library(library::Tab::replays);
    const std::string relative = mod_subfolder_name(files_mod_id()) + "/R08-check.tad";
    std::optional<std::size_t> place;
    for (int frame_index = 0; frame_index < 800 && !place; ++frame_index) {
        frames(1);
        place = library::replay_place(library_host().model, relative);
    }
    require(place.has_value(), relative + " is not in the Library");
    library_host().state.reveal_row = static_cast<int32_t>(*place);
    ++library_host().model_revision;
    frames(1);
    click_named("oa.library.row.replay." + std::to_string(*place + 1));
    click_named("oa.library.action.watch");
    bool watching = false;
    bool saw_index_process = false;
    bool markers_ready = false;
    int watched_frames = 0;
    for (int frame_index = 0; frame_index < 400 && watched_frames < 30; ++frame_index) {
        frames(1);
        const bool now = screen_ == Screen::match && match_ != nullptr &&
                         (current_extension_state() & extension_state::replay) != 0;
        if (!now)
            continue;
        watching = true;
        ++watched_frames;
        if (replay_index_pid() != 0)
            saw_index_process = true;
        if (replay_index_arrived() && replay_marker_after(0).has_value())
            markers_ready = true;
    }
    require(watching, "WATCH did not start the replay");
    require(!saw_index_process, "WATCH started an index pass for a timeline the Library holds");
    require(markers_ready, "WATCH did not show the Library's markers on its first frames");
    require(library_return_ && library_return_->replay, "WATCH did not keep the row");
    const std::string kept = *library_return_->replay;
    wait_length();
    key(SDLK_END, SDL_KMOD_CTRL);
    wait_tick(playback().length_known, "Ctrl+End from the Library");
    for (int frame_index = 0; frame_index < 400 && !named("oa.replay-viewer.library");
         ++frame_index)
        frames(1);
    click_named("oa.replay-viewer.library");
    bool returned = false;
    for (int frame_index = 0; frame_index < 300 && !returned; ++frame_index) {
        frames(1);
        returned = screen_ == Screen::main_menu && match_ == nullptr &&
                   oa_layer().find(std::string(library_screen_name)) != nullptr &&
                   library_host().model.tab == library::Tab::replays &&
                   library_host().model.replays.selected == std::optional<std::string>(kept);
        if (frame_index == 40 && !returned)
            SDL_Delay(5200);
    }
    require(returned, "Back to the Library did not return to " + kept);
    std::cout << "replay viewer: library shows " << kept << '\n';
}

} // namespace oa::app
