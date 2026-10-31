// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Check of the battlefield behind a dialog over a paused match: in the
// standard, Basic and Full tiers, at zoom 1, 1.5, 2 and 0.5, the frame presented
// with the in-game briefing, the save dialog or the help dialog over the
// match shows the battlefield outside the dialog's panel exactly as the frame
// presented without it, the units' pixels among them.
#include "oa/app/runtime.hpp"

#include "render_host.hpp"
#include "render_run.hpp"

#include "oa/formats/png.hpp"
#include "oa/platform/files.hpp"
#include "oa/sim/unit_movement/terrain.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/hud/resource_bar.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

/// The run's exit status when the machine cannot run the accelerated tiers.
constexpr int skipped_exit_code = 77;
/// The window the match is drawn in: the layout's own size, so that every
/// tier presents the battlefield 1:1.
constexpr int backdrop_window_width = 1280;
constexpr int backdrop_window_height = 800;
/// The CORE campaign's nineteenth mission, which opens on a fleet at sea:
/// ships and submarines, partly under the water, beside the commander.
constexpr std::size_t backdrop_mission = 18;
/// The zooms each tier is checked at: whole, magnified between whole
/// numbers and by a whole number, and reduced.
constexpr std::array<float, 4> backdrop_zooms{1.0F, 1.5F, 2.0F, 0.5F};
/// Where the view puts the unit it is aimed at, as shares of the
/// battlefield across and down: clear of the briefing at the top left and
/// of the dialogs centred on the window.
constexpr float aimed_share = 0.75F;
/// Steps of the resource readout's easing that settle it, so that the
/// frames compared show the same numbers.
constexpr int readout_settle_steps = 256;
/// A display gamma above the identity, which the game's gamma setting
/// reaches at its eighteenth step, for the case of a battlefield taken back
/// through the gamma.
constexpr float raised_gamma = 1.25F;

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("dialog backdrop check: " + what);
}

void require(bool ok, const std::string& what) {
    if (!ok)
        fail(what);
}

/// A rectangle of a frame, in canvas pixels.
struct Area {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
};

/// Returns whether a pixel lies in an area.
///
/// @param area the area
/// @param x the pixel's column
/// @param y the pixel's row
/// @return true inside it
bool inside(const Area& area, int32_t x, int32_t y) {
    return x >= area.x && y >= area.y && x < area.x + area.width && y < area.y + area.height;
}

/// What two frames show of an area outside the areas left out.
struct Comparison {
    std::size_t compared{};  ///< pixels compared
    std::size_t differing{}; ///< of them, pixels that differ
    int largest{};           ///< the largest difference of a channel, in levels
};

/// Compares two frames over an area outside the areas left out.
///
/// @param a one frame
/// @param b the other, of the same size
/// @param area the area compared
/// @param left_out the areas not compared
/// @return how many pixels were compared, how many of them differ and by
///     how much at most
Comparison compare_outside(
    const renderer::Surface& a,
    const renderer::Surface& b,
    const Area& area,
    const std::vector<Area>& left_out
) {
    Comparison comparison;
    for (int32_t y = std::max(area.y, 0);
         y < std::min(area.y + area.height, static_cast<int32_t>(a.height));
         ++y)
        for (int32_t x = std::max(area.x, 0);
             x < std::min(area.x + area.width, static_cast<int32_t>(a.width));
             ++x) {
            if (std::any_of(left_out.begin(), left_out.end(), [x, y](const Area& out) {
                    return inside(out, x, y);
                }))
                continue;
            ++comparison.compared;
            const auto at =
                (static_cast<std::size_t>(y) * a.width + static_cast<std::size_t>(x)) * 3U;
            int largest = 0;
            for (std::size_t channel = 0; channel < 3; ++channel)
                largest =
                    std::max(largest, std::abs(int{a.rgb[at + channel]} - b.rgb[at + channel]));
            if (largest != 0)
                ++comparison.differing;
            comparison.largest = std::max(comparison.largest, largest);
        }
    return comparison;
}

/// Writes a frame as a PNG file.
///
/// @param path the file
/// @param frame the frame
void write_png(const fs::path& path, const renderer::Surface& frame) {
    std::vector<uint8_t> file;
    const oa::formats::png::Header header{
        frame.width, frame.height, 8, oa::formats::png::ColorType::rgb, {}
    };
    if (!oa::formats::png::write(oa::formats::png::Image{header, {}, frame.rgb}, &file))
        fail("cannot encode " + oa::platform::path_to_utf8(path));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size())
    );
    if (!output)
        fail("cannot write " + oa::platform::path_to_utf8(path));
}

/// Returns a zoom's text for the check's report and file names: "0.5", "1", "1.5", "2".
///
/// @param zoom the zoom
/// @return its text
std::string zoom_name(float zoom) {
    std::string text = std::to_string(zoom);
    text.erase(text.find_last_not_of('0') + 1);
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text;
}

/// The dialogs opened over the paused match.
enum class Backdrop { briefing, save, help };

/// Returns a dialog's name in the check's report and file names.
///
/// @param dialog the dialog
/// @return its name
std::string_view backdrop_name(Backdrop dialog) {
    switch (dialog) {
    case Backdrop::briefing:
        return "briefing";
    case Backdrop::save:
        return "save";
    case Backdrop::help:
        return "help";
    }
    return "dialog";
}

} // namespace

int Runtime::check_dialog_backdrop() {
    using oa::ui::engine_settings::HardwareAcceleration;
    namespace dialogs = oa::ui::frontend_dialogs;
    if (sdl_.renderer == nullptr || sdl_.window == nullptr || !render_run_ ||
        render_run_->host == nullptr)
        fail("needs the SDL renderer and the renderer host that made it");
    const auto& inputs = render_run_->host->tier_inputs();
    if (inputs.memory < render_policy::smallest_accelerated_memory) {
        std::cout << "dialog backdrop check: skipped: the machine reports less than the 2 GiB "
                     "threshold of memory\n";
        return skipped_exit_code;
    }
    if (inputs.capability != render_policy::Capability::capable && !options_.force_capable) {
        std::cout << "dialog backdrop check: skipped: the renderer is not capable of the "
                     "accelerated tiers\n";
        return skipped_exit_code;
    }
    if (!options_.hardware_acceleration ||
        *options_.hardware_acceleration != HardwareAcceleration::full)
        fail("needs --hardware-acceleration=full, which lets the run switch to every tier");
    const auto set_level = [&](HardwareAcceleration level) {
        options_.hardware_acceleration = level;
        update_render_tier();
        if ((level == HardwareAcceleration::full) != full_presentation() ||
            (level != HardwareAcceleration::off) != accelerated_presentation())
            fail("the tier did not switch");
    };
    const char* renderer_name = SDL_GetRendererName(sdl_.renderer);
    std::cout << "dialog backdrop check: on the " << (renderer_name ? renderer_name : "unknown")
              << " renderer\n";

    // Frames are presented without the cursor, and the window is the
    // layout's size, as the check ends however it ends.
    struct Restore {
        Runtime& runtime;
        bool cursor{};

        ~Restore() {
            runtime.frame_without_cursor_ = cursor;
            if (runtime.sdl_.window != nullptr) {
                std::ignore = SDL_SetWindowSize(
                    runtime.sdl_.window, kDefaultWindowWidth, kDefaultWindowHeight
                );
                std::ignore = SDL_SyncWindow(runtime.sdl_.window);
            }
        }
    } restore{*this, frame_without_cursor_};

    frame_without_cursor_ = true;
    if (!SDL_SetWindowSize(sdl_.window, backdrop_window_width, backdrop_window_height) ||
        !SDL_SyncWindow(sdl_.window))
        fail(std::string("SDL_SetWindowSize: ") + SDL_GetError());

    // The mission loads in Full, so that its loading screen makes the Full
    // tier's pages.
    set_level(HardwareAcceleration::full);
    preferences_.side = 1;
    discover_campaigns();
    require(
        campaign_mission_files_.size() > backdrop_mission,
        "the CORE campaign has no mission " + std::to_string(backdrop_mission + 1)
    );
    selected_mission_index_ = backdrop_mission;
    show_mission_briefing();
    require(screen_ == Screen::briefing, "could not open the mission's briefing");
    start_campaign_mission();
    require(
        screen_ == Screen::match && match_ && campaign_mission_,
        "could not start the mission: " + status_
    );
    render();
    require(
        match_layout_.width == backdrop_window_width &&
            match_layout_.height == backdrop_window_height,
        "the match is not laid out at the window's size"
    );

    // The local player's unit on the water that lies farthest south, with
    // the fleet beside it in view; else the local player's first unit.
    const oa::sim::unit_movement::Terrain terrain(*selected_tnt_);
    uint16_t aimed = 0;
    bool aimed_at_sea = false;
    int32_t aimed_z = 0;
    for (const auto& slot : match_->world().slots) {
        if (slot.unit_index == 0 || slot.unit == nullptr || slot.record.type_index == 0 ||
            slot.record.owner_index != match_local_player_)
            continue;
        const auto& position = slot.unit->position;
        const bool at_sea = terrain.height(position[0], position[2]) < terrain.sea_level();
        const int32_t z = position[2] >> 16;
        if (aimed == 0 || (at_sea && (!aimed_at_sea || z > aimed_z))) {
            aimed = slot.unit_index;
            aimed_at_sea = at_sea;
            aimed_z = z;
        }
    }
    require(aimed != 0, "found no unit of the local player");
    const auto& aimed_position = match_->world().slots[aimed].unit->position;
    const int32_t aimed_x = aimed_position[0] >> 16;

    const fs::path report_directory = "local/reports";
    fs::create_directories(report_directory);
    std::size_t failures = 0;
    std::string first_failure;
    const auto presented = [&] {
        renderer::Surface frame;
        capture_frame_ = &frame;
        render();
        capture_frame_ = nullptr;
        return frame;
    };
    const std::array<std::pair<HardwareAcceleration, std::string_view>, 3> tiers{{
        {HardwareAcceleration::off, "standard"},
        {HardwareAcceleration::basic, "basic"},
        {HardwareAcceleration::full, "full"},
    }};
    for (const auto& [level, tier] : tiers) {
        set_level(level);
        for (const float zoom : backdrop_zooms) {
            match_zoom_ = match_zoom_target_ = zoom;
            show_match_pause_menu();
            set_camera_position(
                aimed_x -
                    static_cast<int32_t>(static_cast<float>(visible_map_width()) * aimed_share),
                aimed_z -
                    static_cast<int32_t>(static_cast<float>(visible_map_height()) * aimed_share),
                0
            );
            update_pointer(
                static_cast<float>(backdrop_window_width - 1),
                static_cast<float>(backdrop_window_height - 1)
            );
            // Settled readouts leave the paused frame as each dialog finds it.
            if (const auto viewer = match_view_player(); viewer < OA_PLAYER_COUNT) {
                auto& game = match_->state().game;
                for (int step = 0; step < readout_settle_steps; ++step)
                    oa::ui::hud::update_resource_readout(
                        game.resource_readout, game.players[viewer], game.tick
                    );
            }
            render();
            const Area battlefield{
                match_layout_.left,
                match_layout_.top,
                match_layout_.battlefield_width(),
                match_layout_.battlefield_height()
            };
            // The in-game menu's panel, which a dialog may darken.
            Area menu{};
            if (match_hud_ && !match_hud_->layout.gadgets.empty()) {
                const auto& root = match_hud_->layout.gadgets.front().common;
                const auto canvas = oa::ui::display_layout::source_rect_to_canvas(
                    match_layout_, root.x, root.y, root.width, root.height
                );
                menu = {canvas.x, canvas.y, canvas.width, canvas.height};
            }
            const std::string drawn =
                full_frame_drawn()                                  ? "the card"
                : accelerated_.frame.method == SceneMethod::magnify ? "the card, magnified"
                : accelerated_.frame.method == SceneMethod::area    ? "the area pass"
                                                                    : "the processor";
            const auto shown = presented();
            require(
                shown.width == static_cast<uint32_t>(backdrop_window_width) &&
                    shown.height == static_cast<uint32_t>(backdrop_window_height),
                "the match frame was not read back at the window's size"
            );
            for (const Backdrop dialog : {Backdrop::briefing, Backdrop::save, Backdrop::help}) {
                const std::string where = std::string(backdrop_name(dialog)) + " in the " +
                                          std::string(tier) + " tier at zoom " + zoom_name(zoom);
                Area panel{};
                switch (dialog) {
                case Backdrop::briefing:
                    activate_pause_gadget("MISSION");
                    require(screen_ == Screen::briefing, "MISSION did not open the briefing");
                    break;
                case Backdrop::save:
                    activate_pause_gadget("SAVEGAME");
                    require(screen_ == Screen::load_game, "SAVEGAME did not open the save page");
                    break;
                case Backdrop::help:
                    activate_pause_gadget("HELP");
                    require(
                        dialogs::dialog_kind() == dialogs::DialogKind::help,
                        "HELP did not open the help dialog"
                    );
                    break;
                }
                if (dialog == Backdrop::help) {
                    const auto& root = dialogs::dialog_resources()->layout.gadgets.front().common;
                    panel = {root.x, root.y, root.width, root.height};
                } else {
                    const auto& root = resources_.layout.gadgets.front().common;
                    panel = {root.x, root.y, root.width, root.height};
                }
                const auto over = presented();
                require(
                    over.width == shown.width && over.height == shown.height,
                    "the " + where + " is not drawn over the match's frame"
                );
                const auto comparison = compare_outside(shown, over, battlefield, {panel, menu});
                std::cout << "dialog backdrop check: " << where << " (the battlefield drawn by "
                          << drawn << "): " << comparison.differing << " of " << comparison.compared
                          << " battlefield pixels outside the panel differ from the match's "
                             "frame\n";
                const std::string stem = "native-dialog-backdrop-" + std::string(tier) + "-zoom" +
                                         zoom_name(zoom) + '-' + std::string(backdrop_name(dialog));
                if (comparison.differing != 0 || zoom == 1.0F) {
                    write_png(report_directory / (stem + "-closed.png"), shown);
                    write_png(report_directory / (stem + "-open.png"), over);
                }
                require(
                    comparison.compared * 2 >
                        static_cast<std::size_t>(battlefield.width) * battlefield.height,
                    "the " + where + " leaves less than half the battlefield to compare"
                );
                if (comparison.differing != 0) {
                    if (failures == 0)
                        first_failure = "the " + where + " changes " +
                                        std::to_string(comparison.differing) +
                                        " pixels of the battlefield outside its panel";
                    ++failures;
                }
                switch (dialog) {
                case Backdrop::briefing:
                    click_briefing_gadget("OK");
                    break;
                case Backdrop::save:
                    exercise_click("CANCEL");
                    break;
                case Backdrop::help:
                    dialogs::close_dialog();
                    break;
                }
                require(
                    screen_ == Screen::match && match_paused_ && dialogs::dialog_count() == 0,
                    "the " + where + " did not close back to the paused match"
                );
                show_match_pause_menu();
            }
        }
    }

    // With the display gamma raised, the card's battlefield is taken back
    // through the gamma, which the dialog's presentation applies again: no
    // level may move by more than half the widest step between the levels
    // the gamma gives, where the battlefield taken through it twice would be
    // brightened again.
    {
        const float kept_gamma = display_gamma_;
        set_display_gamma(raised_gamma);
        int widest_step = 0;
        for (std::size_t level = 1; level < gamma_table_.size(); ++level)
            widest_step =
                std::max(widest_step, int{gamma_table_[level]} - int{gamma_table_[level - 1]});
        const int allowed = (widest_step + 1) / 2;
        set_level(HardwareAcceleration::full);
        match_zoom_ = match_zoom_target_ = 1.0F;
        show_match_pause_menu();
        set_camera_position(
            aimed_x - static_cast<int32_t>(static_cast<float>(visible_map_width()) * aimed_share),
            aimed_z - static_cast<int32_t>(static_cast<float>(visible_map_height()) * aimed_share),
            0
        );
        render();
        const Area battlefield{
            match_layout_.left,
            match_layout_.top,
            match_layout_.battlefield_width(),
            match_layout_.battlefield_height()
        };
        const auto shown = presented();
        activate_pause_gadget("MISSION");
        require(screen_ == Screen::briefing, "MISSION did not open the briefing");
        const auto& root = resources_.layout.gadgets.front().common;
        const Area panel{root.x, root.y, root.width, root.height};
        const auto over = presented();
        const auto comparison = compare_outside(shown, over, battlefield, {panel});
        std::cout << "dialog backdrop check: briefing in the full tier at zoom 1 at gamma "
                  << raised_gamma << ": " << comparison.differing << " of " << comparison.compared
                  << " battlefield pixels outside the panel differ from the match's frame, by "
                  << comparison.largest << " levels at most, of the " << allowed << " allowed\n";
        if (comparison.largest > allowed) {
            write_png(
                report_directory / "native-dialog-backdrop-full-gamma-briefing-closed.png", shown
            );
            write_png(
                report_directory / "native-dialog-backdrop-full-gamma-briefing-open.png", over
            );
            if (failures == 0)
                first_failure = "the briefing at gamma " + zoom_name(raised_gamma) +
                                " moves the battlefield outside its panel by " +
                                std::to_string(comparison.largest) + " levels";
            ++failures;
        }
        click_briefing_gadget("OK");
        require(
            screen_ == Screen::match && match_paused_,
            "the briefing at a raised gamma did not close back to the paused match"
        );
        set_display_gamma(kept_gamma);
    }
    leave_match();
    if (failures != 0)
        fail(
            first_failure + " (" + std::to_string(failures) +
            " cases in all; the pictures are in " + oa::platform::path_to_utf8(report_directory) +
            ")"
        );
    std::cout << "dialog backdrop check: every dialog left the battlefield as the match drew it\n";
    return 0;
}

} // namespace oa::app
