// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-map-browser: the map browser over the skirmish setup's map picker.
// The check writes the map pack check's two packs into the player's own
// Maps folder, with every built-in registry turned off so that nothing is
// fetched, and at 640x480, 1280x720, 1920x1080 and 2560x1440 opens SELMAP and
// presses MORE MAPS…. It requires the size class and scale the window gives
// and the grid's columns there; MORE MAPS… over none of SELMAP's gadgets,
// in the Classic and the Modern look the kit's lime accent button at a
// footer button's size under the list's frame, clear of the title and,
// where it grows, of the description box's border;
// every card and control inside the browser's window; Downloads showing
// the two check maps alone; the search "isle" leaving one; the clashing
// map's card greyed with "New names" in the footer's reason and the primary
// button disabled; and Select on the fitting map closing the browser with
// SELMAP's selection on it. Each size's frame is written as
// map-browser-<w>x<h>.png in the working folder, for review. The run is one
// nobody watches: last, the two packs are posted again under ids of their
// own, as the download queue hands a catalogue's over, and the fitting one
// must wait in the inbox with the browser closed and install with no prompt
// while it is the top screen, the clashing one be refused there. Then it
// hosts a battle room alone, opens Select Map, and at 640x480, 1280x720 and
// 1920x1080 requires MORE MAPS… over none of the map dialog's gadgets and
// with the same look and place. Each picker's frame is written in each look
// as more-maps-<look>-selmap-<w>x<h>.png and more-maps-<look>-room-<w>x<h>.png.
// MORE MAPS…'s look and place are told for every window, look and picker at
// the end, so one run names each place they are wrong.

#include "map_pack_check_packs.hpp"
#include "map_packs.hpp"
#include "mod_install_state.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "touch_state.hpp"
#include "user_folder_state.hpp"

#include "oa/app/content/service.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/package_install/inbox.hpp"
#include "oa/app/package_install/oamap.hpp"
#include "oa/app/package_install/origin.hpp"
#include "oa/app/runtime.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/map_pack/manifest.hpp"
#include "oa/data/map_pack/map_name.hpp"
#include "oa/data/registry/descriptor.hpp"
#include "oa/data/registry/player_registries.hpp"
#include "oa/data/registry/trust.hpp"
#include "oa/formats/png.hpp"
#include "oa/formats/zip.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_multiplayer/get_map.hpp"
#include "oa/ui/frontend_multiplayer/launch_block.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/panel.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/map_browser/screen.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ios>
#include <iostream>
#include <iterator>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

namespace oa::app {

namespace {

namespace kit = oa::ui::kit;
namespace mb = oa::ui::map_browser;
namespace layout = oa::ui::display_layout;
namespace registry = oa::data::registry;
namespace map_pack = oa::data::map_pack;
namespace mp = oa::ui::frontend_multiplayer;
namespace dialogs = oa::ui::frontend_dialogs;
namespace settings = oa::ui::engine_settings;

/// One window the check opens the browser in, and what the OA layer gives
/// there: the design's table of windows.
struct CheckWindow {
    int32_t width{};             ///< the window's width, in pixels
    int32_t height{};            ///< its height
    kit::SizeClass size_class{}; ///< the size class
    int32_t scale{};             ///< window pixels a point
    std::size_t columns{};       ///< the grid's columns
};

/// The windows, in the order the check opens them.
constexpr std::array<CheckWindow, 4> kWindows{{
    {640, 480, kit::SizeClass::compact, 1, 3},
    {1280, 720, kit::SizeClass::large, 1, 6},
    {1920, 1080, kit::SizeClass::large, 1, 6},
    {2560, 1440, kit::SizeClass::large, 2, 6},
}};

/// How far above the description box under the map list its drawn top
/// border begins, a dark row over a metal bar, in frame pixels.
constexpr int32_t kDescriptionBorderRise = 11;

/// The frames a step lets pass, enough for the pictures in view to be decoded.
constexpr int32_t kSettleFrames = 16;

/// The words the clashing map's reason holds.
constexpr std::string_view kNewNames = "New names";

/// The search the check types.
constexpr std::string_view kSearch = "isle";

/// Frames a package is given to wait before the check looks at the inbox.
constexpr int32_t kWaitingFrames = 30;

/// The most frames the check waits for what came of a package.
constexpr int32_t kMostFrames = 600;

/// The registry the check's packages come from, as the download queue's
/// hand-off names it. No registry of the run has this id.
constexpr std::string_view kCheckRegistry = "map-browser-check";

/// The packs the check posts as a catalogue's: the map pack check's two
/// packs under ids of their own, which no folder of Maps holds until the
/// check installs them.
constexpr std::string_view kOnlineIsles = "oa-check-isles-online";
constexpr std::string_view kOnlineClash = "oa-check-clash-online";

/// Stops the check.
///
/// @param ok the step held
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("map browser check: " + std::string(what));
}

/// Writes a file whole.
///
/// @param file the file
/// @param bytes its bytes
void write_text(const fs::path& file, std::string_view bytes) {
    if (!file.parent_path().empty())
        fs::create_directories(file.parent_path());
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    require(static_cast<bool>(out), "could not write " + path_to_utf8(file));
}

/// Reads a file whole.
///
/// @param file the file
/// @return its bytes
std::vector<uint8_t> read_whole(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    require(static_cast<bool>(in), "could not read " + path_to_utf8(file));
    const std::vector<uint8_t> bytes{std::istreambuf_iterator<char>{in}, {}};
    return bytes;
}

/// Writes one of the map pack check's packs, as its folder holds it, into a
/// map pack package of another id: its manifest with that id and its name
/// marked online, then every file its maps list, stored.
///
/// @param folder the pack's folder
/// @param id the package's pack id
/// @param file the package
void write_online_pack(const fs::path& folder, std::string_view id, const fs::path& file) {
    const std::vector<uint8_t> manifest_read =
        read_whole(folder / std::string(map_pack::manifest_file));
    map_pack::Manifest manifest;
    std::vector<map_pack::Problem> problems;
    require(
        map_pack::read_manifest(manifest_read, map_pack::ManifestUse::package, manifest, problems),
        "the manifest of " + path_to_utf8(folder) + " cannot be read"
    );
    manifest.id = std::string(id);
    manifest.name += " Online";
    const std::string manifest_text = map_pack::write_manifest(manifest);
    const std::vector<uint8_t> manifest_written(manifest_text.begin(), manifest_text.end());
    std::vector<std::string> listed_files;
    for (const map_pack::MapEntry& map : manifest.maps)
        for (const std::string& listed : map.files)
            if (std::find(listed_files.begin(), listed_files.end(), listed) == listed_files.end())
                listed_files.push_back(listed);
    std::vector<std::vector<uint8_t>> contents;
    contents.reserve(listed_files.size());
    for (const std::string& listed : listed_files)
        contents.push_back(read_whole(folder / path_from_utf8(listed)));
    // The entries point into the manifest's bytes and the contents, both named above.
    std::vector<oa::formats::zip::NewEntry> entries{{map_pack::manifest_file, manifest_written}};
    for (std::size_t at = 0; at < listed_files.size(); ++at)
        entries.push_back({listed_files[at], contents[at]});
    std::vector<uint8_t> archive;
    oa::formats::zip::ZipError error{};
    require(
        oa::formats::zip::write_archive(entries, archive, error),
        "the package " + path_to_utf8(file) + " could not be made"
    );
    write_text(
        file, std::string_view(reinterpret_cast<const char*>(archive.data()), archive.size())
    );
}

/// Writes a frame as a PNG file.
///
/// @param file the file
/// @param frame the frame
void write_png(const fs::path& file, const renderer::Surface& frame) {
    std::vector<uint8_t> bytes;
    const oa::formats::png::Header header{
        frame.width, frame.height, 8, oa::formats::png::ColorType::rgb, {}
    };
    require(
        oa::formats::png::write(oa::formats::png::Image{header, {}, frame.rgb}, &bytes),
        "could not encode " + path_to_utf8(file)
    );
    write_text(file, std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size()));
}

/// Maps a rectangle of the 640 by 480 frame to window pixels through the
/// picture's place in the window, the edges outward.
///
/// @param seen what the layer's screens are placed on
/// @param frame_rect the rectangle, in the frame's pixels
/// @return the rectangle, in window pixels
kit::Rect frame_to_window(const LayerView& seen, const kit::Rect& frame_rect) {
    const auto& picture = seen.picture;
    const auto floor_at = [](int64_t numerator, int64_t denominator) {
        return static_cast<int32_t>(numerator / denominator);
    };
    const auto ceil_at = [](int64_t numerator, int64_t denominator) {
        return static_cast<int32_t>((numerator + denominator - 1) / denominator);
    };
    const int64_t across = std::max(seen.picture_size.x, int32_t{1});
    const int64_t down = std::max(seen.picture_size.y, int32_t{1});
    const int32_t left =
        picture.x + floor_at(static_cast<int64_t>(frame_rect.x) * picture.width, across);
    const int32_t top =
        picture.y + floor_at(static_cast<int64_t>(frame_rect.y) * picture.height, down);
    const int32_t right =
        picture.x +
        ceil_at(static_cast<int64_t>(frame_rect.x + frame_rect.width) * picture.width, across);
    const int32_t bottom =
        picture.y +
        ceil_at(static_cast<int64_t>(frame_rect.y + frame_rect.height) * picture.height, down);
    return {left, top, right - left, bottom - top};
}

/// Returns what of a rectangle a clip leaves; the rectangle when no clip.
///
/// @param rect the rectangle
/// @param clip the clip; empty for none
/// @return the part that can show
kit::Rect visible_part(const kit::Rect& rect, const kit::Rect& clip) {
    if (clip.width <= 0 || clip.height <= 0)
        return rect;
    return kit::intersect(rect, clip);
}

/// Returns the game names of the cards a display list shows controls for,
/// in grid order.
///
/// @param list the display list
/// @return the names
std::vector<std::string> card_names(const kit::DisplayList& list) {
    std::vector<std::string> names;
    for (const kit::Control& control : list.controls)
        if (control.name.starts_with(mb::name_card_prefix))
            names.push_back(control.text);
    return names;
}

} // namespace

void Runtime::prepare_map_browser_check() {
    // Written before the content service reads the player's folder: every
    // built-in registry beside the program is off, so the check fetches no
    // catalogue and offers no map online.
    registry::PlayerRegistries list;
    if (const char* base = SDL_GetBasePath(); base != nullptr) {
        const fs::path folder =
            path_from_utf8(base) / path_from_utf8(content::builtin_registries_folder_name);
        std::error_code error;
        if (fs::is_directory(folder, error))
            for (const registry::Descriptor& built_in :
                 registry::read_builtin_registries(folder, nullptr))
                list.disabled.push_back(built_in.id);
    }
    write_text(
        user_folder() / std::string(registry::player_registries_file),
        registry::player_registries_text(list)
    );
    std::cout << "map browser check: " << list.disabled.size()
              << " built-in registries turned off\n";
}

void Runtime::check_map_browser() {
    require(sdl_.window != nullptr && sdl_.renderer != nullptr, "needs the SDL presenter");
    require(
        options_.user_folder && !options_.user_folder->empty() && !user_folder_.empty(),
        "the run has no folder of the player's own (--user-folder)"
    );
    // The map pack check's two packs, in the player's own Maps folder.
    const fs::path maps = user_folder_ / std::string(package_install::oamap::maps_folder_name);
    const auto models = assets_.list_effective("objects3d", ".3do", false);
    require(!models.empty(), "the game has no model for the check maps' feature");
    const std::string model_stem = path_to_utf8(path_from_utf8(models.front()).stem());
    // The packs an earlier run installed as a catalogue's leave first.
    for (const std::string_view online : {kOnlineIsles, kOnlineClash}) {
        std::error_code removed;
        fs::remove_all(maps / std::string(online), removed);
        require(!removed, "could not remove an earlier " + std::string(online));
    }
    write_map_pack_check_packs(maps, model_stem);
    map_packs().refresh();
    const std::string isle(map_pack_check_fitting_map);
    const std::string clash(map_pack_check_clashing_map);
    require(
        map_packs().find(isle) != nullptr && map_packs().find(clash) != nullptr,
        "the check packs are not installed"
    );
    eligible_map_names_.clear();
    first_map_name_.clear();
    discover_first_map();
    if (const std::shared_ptr<const content::Snapshot> snapshot = content_service().snapshot())
        require(snapshot->entries.empty(), "a catalogue offers packages with every registry off");

    bool running = true;
    const auto frames = [&](int32_t count) {
        for (int32_t frame = 0; frame < count; ++frame) {
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
    const auto click_named = [&](std::string_view name) {
        const std::optional<AutomationControl> control = named(name);
        require(control.has_value(), std::string(name) + " is not shown");
        const layout::Point pixel{
            control->window_x + control->window_width / 2,
            control->window_y + control->window_height / 2
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
            dispatch_event(event, running);
            require(running, "a click ended the run");
        }
        frames(1);
    };
    const auto browser = [this]() -> LayerScreen* {
        LayerScreen* top = oa_layer().top();
        return top != nullptr && top->name() == mb::screen_name ? top : nullptr;
    };
    const auto shown_cards = [&]() {
        LayerScreen* screen = browser();
        require(screen != nullptr, "the map browser is not on top");
        const kit::DisplayList list = screen->display_list();
        return card_names(list);
    };
    // Writes the window's frame, for review.
    const auto write_picker_frame =
        [&](const std::string& file, const CheckWindow& window, const std::string& on) {
            oa_layer().read_whole_window(true);
            renderer::Surface presented;
            capture_frame_ = &presented;
            render();
            capture_frame_ = nullptr;
            oa_layer().read_whole_window(false);
            require(
                presented.width == static_cast<uint32_t>(window.width) &&
                    presented.height == static_cast<uint32_t>(window.height),
                "the frame is not the window's size" + on
            );
            write_png(fs::path(file), presented);
        };
    // MORE MAPS… is the kit's primary button, lime with dark text, its
    // caption in the small font of the kit's compact buttons with a button's
    // padding, a footer button's 17 points tall, or up to the compact control
    // height where the rows down to the description box's border are clear.
    // It lies under the list's frame with its right edge on the list's,
    // clear of every gadget, the frame and the title's band (every row of the
    // picture above the frame), and on the description box's drawn border
    // only at 17 points, where the clear rows are fewer. The list and the
    // gadgets are in the 640 by 480 frame.
    std::vector<std::string> look_faults;
    const auto more_maps_look = [&](std::string_view picker,
                                    const kit::Rect& list_in_frame,
                                    const std::vector<kit::Rect>& gadgets_in_frame,
                                    const std::string& on) {
        const std::string where = " over " + std::string(picker) + on;
        LayerScreen* screen = oa_layer().find("more-maps");
        const kit::Fonts* fonts = oa_layer().screen_fonts();
        require(screen != nullptr && fonts != nullptr, "MORE MAPS… is not on the layer" + where);
        const kit::DisplayList list = screen->display_list();
        const kit::ButtonLook* look = nullptr;
        for (const kit::Item& item : list.items)
            if (const auto* button = std::get_if<kit::ButtonLook>(&item.look))
                look = button;
        if (look == nullptr) {
            look_faults.push_back("MORE MAPS… draws no button" + where);
            return;
        }
        const std::size_t faults_before = look_faults.size();
        if (look->style != kit::ButtonStyle::accent || look->font != kit::FontRole::small)
            look_faults.push_back(
                "MORE MAPS… is not the accent button with its caption in the small font" + where
            );
        const kit::Metrics& compact = kit::compact_metrics;
        const int32_t width = std::max(
            compact.button_width,
            kit::text_width(*fonts, kit::FontRole::small, look->caption) +
                compact.prompt_button_padding
        );
        const LayerPlacement placed = oa_layer().placement_of("more-maps");
        const std::string sized =
            std::to_string(placed.points_width) + " by " + std::to_string(placed.points_height);
        if (placed.points_height < compact.button_height ||
            placed.points_height > compact.control_height || placed.points_width != width)
            look_faults.push_back(
                "MORE MAPS… is " + sized + " points, not " + std::to_string(width) + " by " +
                std::to_string(compact.button_height) + " to " +
                std::to_string(compact.control_height) + where
            );
        const kit::Rect shown_at{
            placed.shown.x, placed.shown.y, placed.shown.width, placed.shown.height
        };
        const auto meets = [&](const kit::Rect& other) {
            const kit::Rect shared = kit::intersect(shown_at, other);
            return shared.width > 0 && shared.height > 0;
        };
        const LayerView seen = oa_layer().view();
        const kit::Rect list_window = frame_to_window(seen, list_in_frame);
        // The list draws its frame three frame pixels outside the gadget.
        const kit::Rect frame_window = frame_to_window(
            seen,
            {list_in_frame.x - 3,
             list_in_frame.y - 3,
             list_in_frame.width + 6,
             list_in_frame.height + 6}
        );
        const int32_t slack = std::max(seen.scale_percent, int32_t{100}) / 100;
        if (shown_at.y < frame_window.y + frame_window.height ||
            std::abs((shown_at.x + shown_at.width) - (list_window.x + list_window.width)) > slack)
            look_faults.push_back(
                "MORE MAPS… is not under the list's frame with its right edge on the list's" + where
            );
        if (meets(frame_window))
            look_faults.push_back("MORE MAPS… meets the list's frame" + where);
        if (meets(
                {seen.picture.x,
                 seen.picture.y,
                 seen.picture.width,
                 frame_window.y - seen.picture.y}
            ))
            look_faults.push_back("MORE MAPS… meets the title's band" + where);
        for (const kit::Rect& gadget : gadgets_in_frame) {
            if (meets(frame_to_window(seen, gadget)))
                look_faults.push_back("MORE MAPS… meets a gadget of the picker" + where);
            const bool under = gadget.y >= list_in_frame.y + list_in_frame.height &&
                               gadget.x < list_in_frame.x + list_in_frame.width &&
                               gadget.x + gadget.width > list_in_frame.x;
            const kit::Rect border = frame_to_window(
                seen,
                {gadget.x, gadget.y - kDescriptionBorderRise, gadget.width, kDescriptionBorderRise}
            );
            if (under && meets(border) && placed.points_height > compact.button_height)
                look_faults.push_back("MORE MAPS… grew onto the description box's border" + where);
        }
        if (look_faults.size() == faults_before)
            std::cout << "map browser check: MORE MAPS… is the accent button, " << sized
                      << " points, under the list's frame" << where << '\n';
    };
    // Checks MORE MAPS… in the Classic and the Modern look, and writes the
    // window's frame in each as more-maps-<look>-<picker>-<w>x<h>.png. The
    // look the run had is put back.
    const auto more_maps_in_both_looks = [&](std::string_view picker,
                                             std::string_view picture_word,
                                             const CheckWindow& window,
                                             const kit::Rect& list_in_frame,
                                             const std::vector<kit::Rect>& gadgets_in_frame,
                                             const std::string& on) {
        const settings::EngineSettings kept = engine_settings();
        for (const settings::Look shown_look : {settings::Look::classic, settings::Look::modern}) {
            settings::EngineSettings chosen = kept;
            chosen.look = shown_look;
            apply_engine_settings(chosen);
            frames(2);
            const std::string look_word =
                shown_look == settings::Look::modern ? "modern" : "classic";
            write_picker_frame(
                "more-maps-" + look_word + "-" + std::string(picture_word) + "-" +
                    std::to_string(window.width) + 'x' + std::to_string(window.height) + ".png",
                window,
                on
            );
            more_maps_look(
                std::string(picker) + " in " + look_word, list_in_frame, gadgets_in_frame, on
            );
        }
        apply_engine_settings(kept);
        frames(2);
    };

    for (const CheckWindow& window : kWindows) {
        const std::string size = std::to_string(window.width) + 'x' + std::to_string(window.height);
        const std::string on = " on the " + size + " window";
        require(
            SDL_SetWindowSize(sdl_.window, window.width, window.height) &&
                SDL_SyncWindow(sdl_.window),
            "the window did not take " + size
        );
        apply_output_mode();
        load(Screen::main_menu);
        require(screen_ == Screen::main_menu, "the main menu did not open" + on);
        frames(2);
        exercise_click(menu::resource_name(menu::Button::single_player));
        exercise_click(entry::resource_name(entry::Button::skirmish));
        require(screen_ == Screen::skirmish, "Skirmish did not open its setup" + on);
        exercise_click(skirmish::resource_name(skirmish::Button::select_map));
        require(screen_ == Screen::map_selection, "the map picker did not open" + on);
        frames(2);

        // MORE MAPS… shows under the map list, over none of SELMAP's gadgets.
        OaLayer& layer = oa_layer();
        require(
            layer.screen_class() == window.size_class && layer.screen_scale() == window.scale,
            "the layer is not " + std::string(size_class_name(window.size_class)) + " at " +
                std::to_string(window.scale) + "x" + on
        );
        require(named("oa.more-maps").has_value(), "MORE MAPS… is not listed as oa.more-maps" + on);
        const LayerPlacement button = layer.placement_of("more-maps");
        const kit::Rect button_rect{
            button.shown.x, button.shown.y, button.shown.width, button.shown.height
        };
        require(button_rect.width > 0 && button_rect.height > 0, "MORE MAPS… does not show" + on);
        const LayerView seen = layer.view();
        const auto& gadgets = resources_.layout.gadgets;
        require(!gadgets.empty(), "SELMAP has no gadgets" + on);
        kit::Rect map_list{};
        kit::Rect map_list_in_frame{};
        std::vector<kit::Rect> gadgets_in_frame;
        bool have_map_list = false;
        const auto& root = gadgets.front().common;
        const auto origin = panel_origin();
        const int32_t left = origin.x + (kCanvasWidth - static_cast<int32_t>(root.width)) / 2;
        const int32_t top = origin.y + (kCanvasHeight - static_cast<int32_t>(root.height)) / 2;
        for (std::size_t index = 1; index < gadgets.size(); ++index) {
            const auto& common = gadgets[index].common;
            const kit::Rect in_frame{
                left + common.x,
                top + common.y,
                static_cast<int32_t>(common.width),
                static_cast<int32_t>(common.height)
            };
            const kit::Rect gadget = frame_to_window(seen, in_frame);
            const kit::Rect shared = kit::intersect(button_rect, gadget);
            require(
                shared.width <= 0 || shared.height <= 0,
                "MORE MAPS… overlaps SELMAP's " + std::string(common.name) + on
            );
            if (!tdf_names_equal(common.name, "MAPNAMES")) {
                if (in_frame.width > 0 && in_frame.height > 0)
                    gadgets_in_frame.push_back(in_frame);
                continue;
            }
            map_list = gadget;
            map_list_in_frame = in_frame;
            have_map_list = true;
            // The list draws its frame three frame pixels outside the gadget.
            const kit::Rect frame = frame_to_window(
                seen,
                {left + common.x - 3,
                 top + common.y - 3,
                 static_cast<int32_t>(common.width) + 6,
                 static_cast<int32_t>(common.height) + 6}
            );
            const kit::Rect on_frame = kit::intersect(button_rect, frame);
            require(
                on_frame.width <= 0 || on_frame.height <= 0,
                "MORE MAPS… overlaps the map list's frame" + on
            );
        }
        require(have_map_list, "SELMAP has no map list" + on);
        more_maps_in_both_looks(
            "SELMAP", "selmap", window, map_list_in_frame, gadgets_in_frame, on
        );

        // A finger on MORE MAPS… opens the browser. A finger in the middle
        // of the map list does not. The mouse click below still opens it.
        if (window.width == 640 || window.width == 1280) {
            require(have_map_list, "SELMAP has no map list" + on);
            const bool unregistered = touch_state().dispatch.accept_unregistered_touch;
            touch_state().dispatch.accept_unregistered_touch = true;
            constexpr SDL_TouchID kFingerDevice = 0x7518;
            const auto finger_at = [&](layout::Point pixel) {
                for (const SDL_EventType type : {SDL_EVENT_FINGER_DOWN, SDL_EVENT_FINGER_UP}) {
                    SDL_Event event = window_finger_event(
                        sdl_.window, sdl_.renderer, type, pixel, kFingerDevice, 1
                    );
                    dispatch_event(event, running);
                    require(running, "a finger ended the run" + on);
                }
                frames(1);
            };
            finger_at(
                {button_rect.x + button_rect.width / 2, button_rect.y + button_rect.height / 2}
            );
            require(browser() != nullptr, "a finger on MORE MAPS… did not open the browser" + on);
            oa_layer().close(browser());
            frames(1);
            require(browser() == nullptr, "the map browser stayed open" + on);
            finger_at({map_list.x + map_list.width / 2, map_list.y + map_list.height / 2});
            require(
                browser() == nullptr,
                "a finger in the middle of the map list opened the browser" + on
            );
            touch_state().dispatch.accept_unregistered_touch = unregistered;
        }

        // The browser: its class's window, every card and control inside it,
        // and its columns.
        click_named("oa.more-maps");
        frames(kSettleFrames);
        LayerScreen* shown = browser();
        require(shown != nullptr, "MORE MAPS… did not open the map browser" + on);
        const LayerPlacement placed = layer.placement_of(mb::screen_name);
        const kit::Point expected = mb::window_size(seen.frame);
        require(
            placed.points_width == expected.x && placed.points_height == expected.y &&
                placed.shown.width == expected.x * window.scale &&
                placed.shown.height == expected.y * window.scale,
            "the map browser is not its class's window" + on
        );
        {
            const kit::DisplayList list = shown->display_list();
            require(kit::name_problem(list).empty(), kit::name_problem(list) + on);
            const kit::Rect whole{0, 0, expected.x, expected.y};
            for (const kit::Control& control : list.controls) {
                const kit::Rect reach = visible_part(control.rect, control.clip);
                if (reach.width > 0 && reach.height > 0)
                    require(
                        kit::wholly_in(reach, whole), control.name + " lies outside the window" + on
                    );
            }
            for (const kit::Item& item : list.items) {
                const kit::Rect drawn = visible_part(item.rect, item.clip);
                if (drawn.width > 0 && drawn.height > 0)
                    require(kit::wholly_in(drawn, whole), "a part lies outside the window" + on);
            }
            std::vector<int32_t> first_row;
            for (const kit::Control& control : list.controls)
                if (control.name.starts_with(mb::name_card_prefix) &&
                    (first_row.empty() || control.rect.y == first_row.front()))
                    first_row.push_back(control.rect.y);
            require(
                first_row.size() == window.columns,
                "the grid has " + std::to_string(first_row.size()) + " columns, not " +
                    std::to_string(window.columns) + on
            );
            constexpr std::string_view listed_names[] = {
                mb::name_filter_all, mb::name_filter_game,
                mb::name_filter_mod, mb::name_filter_downloads,
                mb::name_players,    "maps-players.any",
                "maps-players.2",    "maps-players.3",
                "maps-players.4",    "maps-players.5-10",
                mb::name_sizes,      "maps-size.any",
                "maps-size.s",       "maps-size.m",
                "maps-size.l",       mb::name_search,
                mb::name_cancel,     mb::name_select,
                mb::name_close,
            };
            for (const std::string_view listed : listed_names)
                require(
                    named("oa." + std::string(listed)).has_value(),
                    "oa." + std::string(listed) + " is not listed" + on
                );
        }

        // Downloads: the two check maps, and nothing offered online.
        click_named("oa.maps-filter-downloads");
        {
            std::vector<std::string> names = shown_cards();
            std::sort(names.begin(), names.end());
            require(
                names == std::vector<std::string>{clash, isle},
                "Downloads does not show the two check maps alone" + on
            );
        }

        // The clashing map: greyed, its reason in the footer, Select disabled.
        click_named("oa.maps-card-" + mb::card_word(clash));
        frames(kSettleFrames);
        {
            const kit::DisplayList list = shown->display_list();
            bool greyed = false;
            bool reason = false;
            for (const kit::Item& item : list.items) {
                if (const auto* card = std::get_if<kit::CardLook>(&item.look);
                    card != nullptr && card->selected)
                    greyed = card->greyed;
                if (item.role == kit::Role::text && item.text.find(kNewNames) != std::string::npos)
                    reason = true;
            }
            require(greyed, "the clashing map's card is not greyed" + on);
            require(reason, "the footer does not give the clashing map's reason" + on);
            const std::optional<AutomationControl> select_button = named("oa.maps-select");
            require(
                select_button && !select_button->enabled,
                "Select is not disabled for the clashing map" + on
            );
        }

        // The frame for review: every map, the clashing map selected.
        click_named("oa.maps-filter-all");
        frames(kSettleFrames);
        oa_layer().read_whole_window(true);
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        oa_layer().read_whole_window(false);
        require(
            presented.width == static_cast<uint32_t>(window.width) &&
                presented.height == static_cast<uint32_t>(window.height),
            "the frame is not the window's size" + on
        );
        write_png(fs::path("map-browser-" + size + ".png"), presented);

        // Acid Foursome is for 2 and 4 players. Its card reads 4, chip 4
        // lists it and chip 3 does not.
        constexpr std::string_view foursome = "Foursome";
        click_named("oa.maps-filter-game");
        click_named("oa.maps-players.4");
        click_named("oa.maps-search");
        {
            const LayerAnswer typed = shown->text(foursome);
            oa_layer().take_answer(*shown, typed, 0);
            frames(1);
        }
        {
            const kit::DisplayList list = shown->display_list();
            const std::vector<std::string> names = card_names(list);
            require(names.size() == 1, "chip 4 does not list Acid Foursome alone" + on);
            std::string lowered = names.front();
            for (char& letter : lowered)
                if (letter >= 'A' && letter <= 'Z')
                    letter = static_cast<char>(letter - 'A' + 'a');
            require(
                lowered.find("foursome") != std::string::npos,
                "chip 4 lists " + names.front() + ", not Acid Foursome" + on
            );
            const kit::CardLook* acid = nullptr;
            for (const kit::Item& item : list.items)
                if (const auto* card = std::get_if<kit::CardLook>(&item.look);
                    card != nullptr && !card->title.empty())
                    acid = card;
            require(acid != nullptr, "Acid Foursome's card is not shown for 4 players" + on);
            require(
                acid->subtitle.starts_with("4 ·"),
                "Acid Foursome's card reads \"" + acid->subtitle + "\"" + on
            );
            click_named("oa.maps-card-" + mb::card_word(names.front()));
            frames(1);
            const kit::DisplayList selected = shown->display_list();
            std::string details;
            for (const kit::Item& item : selected.items)
                if (item.role == kit::Role::text && item.text.find("players") != std::string::npos)
                    details = item.text;
            require(
                details.find("4 players") != std::string::npos &&
                    details.find("10 players") == std::string::npos,
                "Acid Foursome's card does not read 4 players (" + details + ")" + on
            );
            std::cout << "map browser check: Acid Foursome: " << details << '\n';
        }
        click_named("oa.maps-players.any");
        click_named("oa.maps-players.3");
        frames(1);
        require(shown_cards().empty(), "3 players lists Acid Foursome" + on);
        click_named("oa.maps-players.any");
        click_named("oa.maps-filter-all");
        click_named("oa.maps-search");
        for (std::size_t typed = foursome.size(); typed > 0; --typed) {
            const LayerAnswer erased = shown->key(kit::Key::backspace, 0);
            oa_layer().take_answer(*shown, erased, 0);
        }
        frames(1);

        // The search narrows the installed maps to one.
        click_named("oa.maps-filter-downloads");
        click_named("oa.maps-search");
        {
            const LayerAnswer answer = shown->text(kSearch);
            oa_layer().take_answer(*shown, answer, 0);
            frames(1);
            require(
                shown_cards() == std::vector<std::string>{isle},
                "the search \"" + std::string(kSearch) + "\" does not leave one map" + on
            );
        }

        // Select on the fitting map closes the browser with SELMAP's selection on it.
        click_named("oa.maps-card-" + mb::card_word(isle));
        click_named("oa.maps-select");
        frames(2);
        require(browser() == nullptr, "Select did not close the map browser" + on);
        require(
            modal_map_index_ >= 0 &&
                static_cast<std::size_t>(modal_map_index_) < bound_map_names_.size() &&
                bound_map_names_[static_cast<std::size_t>(modal_map_index_)] == isle,
            "SELMAP's selection is not " + isle + on
        );
        std::cout << "map browser check: " << size_class_name(window.size_class) << " at "
                  << window.scale << "x, " << window.columns << " columns" << on << '\n';
        close_map_modal();
        frames(2);
        require(
            oa_layer().find("more-maps") == nullptr, "MORE MAPS… stayed after SELMAP closed" + on
        );
    }

    // Nobody watches the run: SDL's dummy video driver, and no check that
    // shows the installer's prompts. A catalogue map pack, posted as the
    // download queue hands one over, waits in the inbox while the browser is
    // closed, on the main menu and over SELMAP, and installs with no prompt
    // while the browser is the top screen, as Download and select's would;
    // one that does not fit TA 3.1c is refused there with no prompt.
    auto& state = mod_install_state();
    require(
        UserFolderState::unwatched(options_.unattended) && !state.check_shows_prompts,
        "the run is watched: run the check on SDL's dummy video driver"
    );
    require(
        SDL_SetWindowSize(sdl_.window, kWindows.front().width, kWindows.front().height) &&
            SDL_SyncWindow(sdl_.window),
        "the window did not take its first size again"
    );
    apply_output_mode();
    load(Screen::main_menu);
    frames(2);
    require(
        !package_install::package_files_waiting() && state.stage == ModInstallState::Stage::idle,
        "a package waits before the check posts one"
    );
    const fs::path files_folder = user_folder_.parent_path() / "files";
    const fs::path packs_made = files_folder / "packs";
    write_map_pack_check_packs(packs_made, model_stem);
    const std::optional<map_pack::PackMapName> fitting_split = map_pack::split_pack_map_name(isle);
    const std::optional<map_pack::PackMapName> clashing_split =
        map_pack::split_pack_map_name(clash);
    require(fitting_split && clashing_split, "the check maps' names do not split");
    const fs::path online_isle_file = files_folder / (std::string(kOnlineIsles) + ".oamap");
    const fs::path online_clash_file = files_folder / (std::string(kOnlineClash) + ".oamap");
    write_online_pack(packs_made / std::string(fitting_split->id), kOnlineIsles, online_isle_file);
    write_online_pack(
        packs_made / std::string(clashing_split->id), kOnlineClash, online_clash_file
    );
    const std::string online_isle =
        std::string(fitting_split->stem) + map_pack::map_name_separator + std::string(kOnlineIsles);
    const auto post_from_catalogue = [](const fs::path& file, std::string_view id) {
        package_install::Origin from_catalogue{};
        from_catalogue.kind = package_install::OriginKind::catalogue;
        from_catalogue.registry = std::string(kCheckRegistry);
        from_catalogue.catalogue_id = std::string(id);
        from_catalogue.release = 1;
        const std::vector<uint8_t> package_bytes = read_whole(file);
        from_catalogue.sha256 = oa::base::sha256::digest_of(package_bytes);
        package_install::post_package_file(file, from_catalogue);
    };
    const uint32_t prompts_before = state.prompts_shown;
    // The pack waits first in the inbox: nothing read, asked or installed.
    const auto waits = [&](std::string_view where) {
        frames(kWaitingFrames);
        const std::optional<package_install::OpenedPackage> waiting =
            package_install::next_package_file();
        require(
            waiting && waiting->origin.kind == package_install::OriginKind::catalogue &&
                waiting->file.filename() == online_isle_file.filename() &&
                state.stage == ModInstallState::Stage::idle && state.prompt == nullptr &&
                state.prompts_shown == prompts_before &&
                !fs::exists(maps / std::string(kOnlineIsles)),
            "a catalogue map pack did not wait in the inbox " + std::string(where)
        );
    };
    // What came of a package while the browser shows, which shows no prompt.
    const auto until_outcome = [&](std::string_view what) {
        for (int32_t frame = 0; frame < kMostFrames; ++frame) {
            frames(1);
            require(
                state.prompt == nullptr && state.prompts_shown == prompts_before,
                std::string(what) + " showed a prompt"
            );
            require(
                browser() != nullptr,
                "the map browser closed while " + std::string(what) + " installed"
            );
            // Taken before the next frame's content service takes it.
            const std::vector<package_install::PackageOutcome> outcomes =
                package_install::take_package_outcomes();
            if (!outcomes.empty())
                return outcomes.front();
        }
        throw std::runtime_error(
            "map browser check: nothing came of " + std::string(what) + " while the browser showed"
        );
    };

    post_from_catalogue(online_isle_file, kOnlineIsles);
    waits("on the main menu");
    exercise_click(menu::resource_name(menu::Button::single_player));
    exercise_click(entry::resource_name(entry::Button::skirmish));
    exercise_click(skirmish::resource_name(skirmish::Button::select_map));
    require(screen_ == Screen::map_selection, "the map picker did not open for the install");
    waits("over SELMAP with the browser closed");
    std::cout << "map browser check: nobody watching, a catalogue map pack waited in the inbox "
                 "with the map browser closed\n";

    click_named("oa.more-maps");
    require(browser() != nullptr, "MORE MAPS… did not open the map browser for the install");
    {
        const package_install::PackageOutcome outcome = until_outcome("the catalogue map pack");
        require(
            outcome.result == package_install::OutcomeResult::installed,
            "the catalogue map pack was not installed while the browser showed: " + outcome.reason
        );
        const fs::path installed_folder = maps / std::string(kOnlineIsles);
        const std::optional<package_install::Origin> record =
            package_install::read_origin(installed_folder);
        require(
            fs::exists(installed_folder / std::string(map_pack::manifest_file)) && record &&
                record->kind == package_install::OriginKind::catalogue &&
                record->registry == kCheckRegistry && record->catalogue_id == kOnlineIsles &&
                record->release == 1,
            "the catalogue map pack is not in Maps with its catalogue's record"
        );
        require(
            map_packs().find(online_isle) != nullptr && !package_install::package_files_waiting() &&
                state.stage == ModInstallState::Stage::idle,
            "the installed packs do not list " + online_isle
        );
    }
    std::cout << "map browser check: nobody watching, a catalogue map pack installed with no "
                 "prompt while the map browser showed\n";

    post_from_catalogue(online_clash_file, kOnlineClash);
    {
        const package_install::PackageOutcome outcome =
            until_outcome("the catalogue map pack that does not fit");
        require(
            outcome.result == package_install::OutcomeResult::refused &&
                !fs::exists(maps / std::string(kOnlineClash)) &&
                !package_install::package_files_waiting() &&
                state.stage == ModInstallState::Stage::idle,
            "a catalogue map pack that does not fit TA 3.1c was not refused"
        );
        std::cout << "map browser check: nobody watching, a catalogue map pack that does not fit "
                     "was refused with no prompt: "
                  << outcome.reason << '\n';
    }
    click_named("oa.maps-cancel");
    frames(2);
    require(browser() == nullptr, "Cancel did not close the map browser");
    close_map_modal();
    frames(2);
    load(Screen::main_menu);
    frames(2);

    // MORE MAPS… over the battle room's map dialog: a room hosted alone, its
    // Select Map pressed, at 640x480, 1280x720 and 1920x1080.
    mp::DirectGame room_game;
    room_game.kind = mp::DirectGame::Kind::host;
    room_game.player_name = "Steve";
    room_game.game_name = "Ridge";
    mp::multiplayer_bind_direct_game(room_game);
    // The host's transport is named here, as --host names it before the
    // menus open, and put back once the room is up.
    mp::launch::LaunchBlock* const launch = mp::multiplayer_lobby().launch_link.block;
    require(launch != nullptr, "the battle room has no launch block");
    const int32_t saved_connection = launch->connection_type;
    launch->connection_type = mp::launch::connection_type::tcpip;
    exercise_click(menu::resource_name(menu::Button::multiplayer));
    frontend_pass_requested_ = true;
    bool room_opened = false;
    std::string room_notice;
    for (int32_t frame = 0; frame < kMostFrames && !room_opened; ++frame) {
        frames(1);
        if (frame % 15 == 14) {
            dialogs::reset_dialogs();
            if (LayerScreen* notice = oa_layer().find(NoticeScreen::screen_name))
                oa_layer().close(notice);
        }
        mp::BattleRoomMap battle;
        room_opened = mp::multiplayer_battle_room_map(&battle) && !battle.joiner;
        const mp::DirectGameProgress& progress = mp::multiplayer_direct_game_progress();
        if (progress.reached == mp::DirectGameStep::failed)
            room_notice = progress.notice;
    }
    require(
        room_opened,
        room_notice.empty() ? "the battle room did not open"
                            : "the battle room did not open: " + room_notice
    );
    launch->connection_type = saved_connection;
    {
        auto room_context = screen_context();
        require(mp::multiplayer_click(&room_context, "MAP"), "the battle room has no Select Map");
    }
    bool dialog_open = false;
    for (int32_t frame = 0; frame < kWaitingFrames && !dialog_open; ++frame) {
        frames(1);
        dialog_open = mp::multiplayer_map_list_rect(nullptr, nullptr, nullptr, nullptr);
    }
    require(dialog_open, "Select Map did not open the battle room's map dialog");
    for (const CheckWindow& window : kWindows) {
        if (window.width == 2560)
            continue;
        const std::string size = std::to_string(window.width) + 'x' + std::to_string(window.height);
        const std::string on = " on the " + size + " window";
        require(
            SDL_SetWindowSize(sdl_.window, window.width, window.height) &&
                SDL_SyncWindow(sdl_.window),
            "the window did not take " + size
        );
        apply_output_mode();
        frames(2);
        int32_t list_x = 0;
        int32_t list_y = 0;
        int32_t list_width = 0;
        int32_t list_height = 0;
        require(
            mp::multiplayer_map_list_rect(&list_x, &list_y, &list_width, &list_height),
            "the battle room's map dialog closed" + on
        );
        require(
            named("oa.more-maps").has_value(),
            "MORE MAPS… is not listed as oa.more-maps over the room's map dialog" + on
        );
        const LayerPlacement button = oa_layer().placement_of("more-maps");
        const kit::Rect button_rect{
            button.shown.x, button.shown.y, button.shown.width, button.shown.height
        };
        require(
            button_rect.width > 0 && button_rect.height > 0,
            "MORE MAPS… does not show over the room's map dialog" + on
        );
        const LayerView seen = oa_layer().view();
        const mp::Panel* panel = mp::multiplayer_modal();
        require(panel != nullptr, "the battle room's map dialog has no panel" + on);
        const mp::Control* listed = mp::panel_control(*panel, "MAPNAMES");
        require(listed != nullptr, "the battle room's map dialog has no map list" + on);
        // The panel's controls, where the dialog is drawn.
        const int32_t shift_x = list_x - listed->x;
        const int32_t shift_y = list_y - listed->y;
        std::vector<kit::Rect> gadgets_in_frame;
        for (int32_t index = 1; index < panel->count; ++index) {
            const mp::Control& control = panel->controls[static_cast<std::size_t>(index)];
            if (control.width <= 0 || control.height <= 0)
                continue;
            const kit::Rect in_frame{
                control.x + shift_x, control.y + shift_y, control.width, control.height
            };
            if (&control != listed)
                gadgets_in_frame.push_back(in_frame);
            const kit::Rect gadget = frame_to_window(seen, in_frame);
            const kit::Rect shared = kit::intersect(button_rect, gadget);
            require(
                shared.width <= 0 || shared.height <= 0,
                "MORE MAPS… overlaps the room's map dialog's " +
                    std::string(mp::control_name(control)) + on
            );
        }
        // The list draws its frame three frame pixels outside the gadget.
        const kit::Rect frame =
            frame_to_window(seen, {list_x - 3, list_y - 3, list_width + 6, list_height + 6});
        const kit::Rect on_frame = kit::intersect(button_rect, frame);
        require(
            on_frame.width <= 0 || on_frame.height <= 0,
            "MORE MAPS… overlaps the room's map list's frame" + on
        );
        std::cout << "map browser check: MORE MAPS… over the room's map dialog meets none of its "
                     "gadgets"
                  << on << '\n';
        more_maps_in_both_looks(
            "the room's map dialog",
            "room",
            window,
            {list_x, list_y, list_width, list_height},
            gadgets_in_frame,
            on
        );
    }
    std::string told;
    for (const std::string& fault : look_faults)
        told += (told.empty() ? "" : "; ") + fault;
    require(look_faults.empty(), told);
    std::cout << "map browser check passed\n";
}

} // namespace oa::app
