// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-user-folder: the player's own folder beside the check's
// preferences file, the saved games moved into it once, those loose in Saves
// into Saves/default once, the recordings into Recordings once, the paths
// the game names placed in it, a save, a screenshot and a film in the folders
// of the mod played, the main menu's notice of the moves shown once on the OA
// layer and closed, notices and a question waiting for the layer one at a
// time, a question over Settings, and the settings' Your files buttons, all
// through a recorded opener, so that no file manager opens.

#include "engine_settings_menu_host.hpp"
#include "engine_settings_state.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "user_folder_state.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/mod_profile_loader.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/engine_settings/notice.hpp"
#include "oa/ui/engine_settings/prompt.hpp"
#include "oa/ui/frontend/savegame_dialogs.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/layout.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

namespace oa::app {

namespace settings = oa::ui::engine_settings;
namespace kit = oa::ui::kit;
namespace artless = oa::ui::frontend_renderer;

namespace {

/// A point of the main menu's picture over none of its buttons, where the
/// pointer rests between the check's steps.
constexpr oa::ui::display_layout::Point kRestingPointer{4, 240};

/// How far round the pointer the cursor may draw, in the picture's pixels.
constexpr int32_t kCursorReach = 40;

/// Stops the check with a reason unless a condition holds.
///
/// @param condition what must hold
/// @param what what went wrong otherwise
void require(bool condition, std::string_view what) {
    if (!condition)
        throw std::runtime_error("user folder check: " + std::string(what));
}

/// Writes a small file, making its folder.
///
/// @param file the file
/// @param text what it holds
void write_file(const fs::path& file, std::string_view text) {
    std::error_code error;
    fs::create_directories(file.parent_path(), error);
    std::ofstream(file, std::ios::binary) << text;
}

/// Reads a file whole.
///
/// @param file the file
/// @return what it holds; empty when it cannot be read
std::string read_file(const fs::path& file) {
    std::ifstream input(file, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(input), {});
}

/// Counts the pixels in which two frames of one size differ, leaving out a
/// rectangle.
///
/// @param first one frame
/// @param second the other frame
/// @param left_out the pixels not compared; empty for none
/// @return the differing pixels; every pixel and one more when the sizes differ
std::size_t differing_pixels(
    const renderer::Surface& first,
    const renderer::Surface& second,
    const artless::SourceRect& left_out = {}
) {
    if (first.width != second.width || first.height != second.height ||
        first.rgb.size() != second.rgb.size())
        return static_cast<std::size_t>(first.width) * first.height + 1U;
    std::size_t differing = 0;
    for (uint32_t y = 0; y < first.height; ++y)
        for (uint32_t x = 0; x < first.width; ++x) {
            const auto column = static_cast<int32_t>(x);
            const auto row = static_cast<int32_t>(y);
            if (column >= left_out.x && row >= left_out.y && column < left_out.x + left_out.width &&
                row < left_out.y + left_out.height)
                continue;
            const auto at = (static_cast<std::size_t>(y) * first.width + x) * 3U;
            if (first.rgb[at] != second.rgb[at] || first.rgb[at + 1] != second.rgb[at + 1] ||
                first.rgb[at + 2] != second.rgb[at + 2])
                ++differing;
        }
    return differing;
}

/// Returns a made-up notice of the check's, with a folder's path.
///
/// @param title its title
/// @param folder the folder it names
/// @return the notice
settings::Notice check_notice(std::string_view title, const fs::path& folder) {
    settings::Notice made;
    made.title = std::string(title);
    made.open_caption = "OPEN FOLDER";
    made.paragraphs.push_back({"A notice of the check's, which waits for the layer:", false});
    made.paragraphs.push_back({path_to_utf8(folder), true});
    return made;
}

/// Returns a made-up question of the check's, with two buttons.
///
/// @param title its title
/// @return the question
oa::ui::kit::Question check_question(std::string_view title) {
    oa::ui::kit::Question made;
    made.title = std::string(title);
    made.paragraphs.push_back({"A question of the check's.", false});
    made.buttons.push_back({"CANCEL", false, "cancel"});
    made.buttons.push_back({"OK", true, "ok"});
    made.cancel_button = 0;
    made.primary_button = 1;
    made.marked = 1;
    return made;
}

/// Writes one step's snapshot, <stem>-<step>.ppm beside --snapshot, when
/// one is asked for.
///
/// @param snapshot the --snapshot path; empty for none
/// @param step the step's name
/// @param frame the frame
void step_snapshot(
    const fs::path& snapshot, std::string_view step, const renderer::Surface& frame
) {
    if (snapshot.empty())
        return;
    write_ppm(
        snapshot.parent_path() / (snapshot.stem().string() + '-' + std::string(step) + ".ppm"),
        frame
    );
}

/// Shows nothing and tells that the file manager failed, as a system
/// without one would.
///
/// @return the failure
FolderOpening refuse_folder(void*, const fs::path&) {
    return FolderOpening{false, std::string(no_file_manager_text), {}};
}

} // namespace

void Runtime::check_user_folder() {
    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        throw std::runtime_error("user folder check: needs the SDL presenter");
    // The check moves files, so it never runs over the player's own.
    if (!options_.preferences_file || options_.user_folder)
        throw std::runtime_error(
            "user folder check: needs --preferences-file, and the folder beside it"
        );
    namespace platform_preferences = oa::platform::preferences;
    std::error_code error;
    const fs::path earlier_root =
        fs::absolute(preference_path_, error).lexically_normal().parent_path();
    require(
        user_folder_ == user_folder_beside(*options_.preferences_file),
        "the player's folder is not \"Open Annihilation\" beside the preferences file"
    );
    // player_folder, which an extension calls with the runtime, is the
    // folder that runtime chose: beside the preferences file, then the
    // preferences' key, then --user-folder, which wins over the key.
    require(
        player_folder(*this) == user_folder(),
        "player_folder is not the folder beside the preferences file"
    );
    const fs::path chosen = earlier_root / "chosen-folder";
    preference_values_[std::string(user_folder_preference)] = path_to_utf8(chosen);
    start_user_folder();
    require(
        player_folder(*this) == user_folder() && user_folder() == chosen,
        "the preferences' folder did not become the player's"
    );
    const fs::path from_option = earlier_root / "option-folder";
    options_.user_folder = from_option;
    start_user_folder();
    require(
        player_folder(*this) == user_folder() && user_folder() == from_option,
        "--user-folder did not become the player's folder"
    );
    options_.user_folder.reset();
    preference_values_.clear();
    start_user_folder();
    require(
        player_folder(*this) == user_folder() &&
            user_folder() == user_folder_beside(*options_.preferences_file),
        "clearing the overrides did not return the folder beside the preferences file"
    );
    // Each run starts from nothing: no record, no folder of its own, and
    // saved games and recordings where earlier versions kept them, beside
    // the preferences file and loose in Saves, two saved games named as ones
    // in Saves/default are.
    preference_values_.clear();
    platform_preferences::save(preference_path_, preference_values_);
    fs::remove_all(user_folder_, error);
    fs::remove_all(earlier_root / "SAVEGAME", error);
    fs::remove_all(earlier_root / "demos", error);
    fs::remove_all(earlier_root / "mods", error);
    const fs::path saves = oa::app::saves_folder(user_folder_, {});
    const fs::path loose = user_folder_ / "Saves";
    require(saves == loose / "default", "3.1c's saved games do not go in Saves/default");
    write_file(earlier_root / "SAVEGAME" / "ALPHA.SAV", "alpha");
    write_file(earlier_root / "SAVEGAME" / "BETA.SAV", "beta");
    write_file(earlier_root / "SAVEGAME" / "UNITS.LST", "list");
    write_file(earlier_root / "mods" / "check-mod" / "SAVEGAME" / "GAMMA.SAV", "gamma");
    write_file(saves / "alpha.sav", "kept");
    write_file(loose / "LOOSE.SAV", "loose");
    write_file(loose / "BETA.SAV", "loose beta");
    write_file(earlier_root / "demos" / "OLD GAME.tad", "old game");
    write_file(earlier_root / "mods" / "check-mod" / "demos" / "MOD GAME.tad", "mod game");

    // A start with a named preferences file moves nothing: what lies beside
    // it and in Saves stays, and the dialogs find it there.
    start_user_folder();
    require(
        read_file(earlier_root / "SAVEGAME" / "ALPHA.SAV") == "alpha" &&
            read_file(loose / "LOOSE.SAV") == "loose" &&
            read_file(earlier_root / "demos" / "OLD GAME.tad") == "old game" &&
            !preference_values_.contains(std::string(saves_moved_preference)) &&
            !preference_values_.contains(std::string(loose_saves_moved_preference)) &&
            !preference_values_.contains(std::string(recordings_moved_preference)) &&
            save_roots().earlier == std::vector<fs::path>{loose, earlier_root / "SAVEGAME"},
        "a start with --preferences-file moved the saved games or recordings beside it or in "
        "Saves"
    );
    // The moves, as a start with the player's own file makes them: three
    // saved games from beside the preferences file and two loose in Saves,
    // each named twice kept under another name, nothing overwritten, the
    // emptied folders gone, the moves recorded and their notices due.
    move_saves_once();
    require(read_file(saves / "alpha.sav") == "kept", "the move overwrote a saved game");
    require(read_file(saves / "ALPHA (2).SAV") == "alpha", "ALPHA.SAV was not kept beside it");
    require(read_file(saves / "BETA.SAV") == "beta", "BETA.SAV did not move");
    require(read_file(saves / "UNITS.LST") == "list", "the restriction list did not move");
    require(
        read_file(loose / "check-mod" / "GAMMA.SAV") == "gamma",
        "a mod's saved game did not move into its own folder"
    );
    require(
        !fs::exists(earlier_root / "SAVEGAME", error) &&
            !fs::exists(earlier_root / "mods" / "check-mod" / "SAVEGAME", error),
        "the emptied earlier folders stayed"
    );
    require(read_file(saves / "LOOSE.SAV") == "loose", "a saved game loose in Saves did not move");
    require(
        read_file(saves / "BETA (2).SAV") == "loose beta" && !holds_a_file(loose),
        "the saved games loose in Saves did not all move into Saves/default"
    );
    const auto recorded = recorded_saves_move(preference_values_);
    require(recorded && recorded->moved == 3 && recorded->left == 0, "the move is not recorded");
    const auto loose_recorded = recorded_loose_saves_move(preference_values_);
    require(
        loose_recorded && loose_recorded->moved == 2 && loose_recorded->left == 0,
        "the move of the saved games loose in Saves is not recorded"
    );
    const auto told = moves_to_tell(preference_values_);
    require(
        saves_notice_due_in(preference_values_) && told.beside_preferences && told.loose_in_saves,
        "the moves' notices are not due"
    );
    const auto written = platform_preferences::load(preference_path_);
    require(
        written.contains(std::string(saves_moved_preference)) &&
            written.contains(std::string(loose_saves_moved_preference)),
        "the moves' records were not written"
    );

    // The recordings, as a start with the player's own file moves them: the
    // game's into Recordings/default and the mod's into its own folder.
    move_recordings_once();
    const fs::path recordings = user_folder_ / "Recordings";
    require(
        read_file(recordings / "default" / "OLD GAME.tad") == "old game" &&
            read_file(recordings / "check-mod" / "MOD GAME.tad") == "mod game" &&
            !fs::exists(earlier_root / "demos", error),
        "the recordings did not move into Recordings/default and Recordings/check-mod"
    );
    const auto recordings_recorded = recorded_recordings_move(preference_values_);
    require(
        recordings_recorded && recordings_recorded->moved == 2 &&
            platform_preferences::load(preference_path_)
                .contains(std::string(recordings_moved_preference)),
        "the move of the recordings is not recorded"
    );

    // Once recorded, the moves are not made again: saved games and
    // recordings an earlier version writes later stay, and the dialogs find
    // the saved games where they are.
    write_file(earlier_root / "SAVEGAME" / "DELTA.SAV", "delta");
    write_file(loose / "EPSILON.SAV", "epsilon");
    write_file(earlier_root / "demos" / "LATER.tad", "later");
    move_saves_once();
    move_recordings_once();
    require(
        read_file(earlier_root / "SAVEGAME" / "DELTA.SAV") == "delta" &&
            read_file(loose / "EPSILON.SAV") == "epsilon" &&
            read_file(earlier_root / "demos" / "LATER.tad") == "later",
        "a second start moved the saved games or recordings again"
    );
    const auto roots = save_roots();
    require(roots.saves == saves, "the saved games are not written to Saves/default");
    require(
        roots.earlier == std::vector<fs::path>{loose, earlier_root / "SAVEGAME"} &&
            game_file_path("SAVEGAME\\DELTA.SAV", ui::frontend::SavePathUse::read) ==
                earlier_root / "SAVEGAME" / "DELTA.SAV" &&
            game_file_path("SAVEGAME\\EPSILON.SAV", ui::frontend::SavePathUse::read) ==
                loose / "EPSILON.SAV" &&
            game_file_path("savegame\\BETA.SAV", ui::frontend::SavePathUse::read) ==
                saves / "BETA.SAV" &&
            game_file_path("SAVEGAME\\DELTA.SAV", ui::frontend::SavePathUse::write) ==
                saves / "DELTA.SAV",
        "the earlier folders' saved games are not read where they are, or written to "
        "Saves/default"
    );

    // Captures: the player's own folder is the Image Output Directory, its
    // screenshots folder Screenshots/default and its films in Films/default.
    const std::string output = own_image_output_directory();
    require(
        output == path_to_utf8(user_folder_) && preferences_.image_output_directory == output,
        "the Image Output Directory is not the player's own folder"
    );
    require(
        game_file_path(output + "\\screenshots\\SHOT0001.pcx", ui::frontend::SavePathUse::write) ==
                user_folder_ / "Screenshots" / "default" / "SHOT0001.pcx" &&
            game_file_path(output + "\\MOVIE001\\FRAM0001.pcx", ui::frontend::SavePathUse::write) ==
                user_folder_ / "Films" / "default" / "MOVIE001" / "FRAM0001.pcx" &&
            game_file_path(output + "\\MOVIE*", ui::frontend::SavePathUse::write) ==
                user_folder_ / "Films" / "default" / "MOVIE*",
        "screenshots and films do not go in Screenshots/default and Films/default"
    );
    std::cout << "user folder check: 3 saved games moved into " << path_to_utf8(saves)
              << " and 2 loose in Saves after them, none overwritten, once; 2 recordings moved "
                 "into Recordings/default and Recordings/check-mod, once; screenshots and "
                 "films go in Screenshots/default and Films/default\n";

    // The notice, over the main menu, in a 640x480 window, where the OA
    // layer draws it at Compact, 1x, pixel for pixel as 0.7.3; then at the
    // larger windows' classes and scales.
    const auto previous_tick = fake_frontend_tick_;
    fake_frontend_tick_ = 1000U;
    int run_width = 0;
    int run_height = 0;
    SDL_GetWindowSize(sdl_.window, &run_width, &run_height);
    const auto window_of = [this](int width, int height) {
        require(
            SDL_SetWindowSize(sdl_.window, width, height) && SDL_SyncWindow(sdl_.window),
            "the window did not take " + std::to_string(width) + 'x' + std::to_string(height)
        );
        apply_output_mode();
    };
    window_of(kCanvasWidth, kCanvasHeight);
    load(Screen::main_menu);
    require(screen_ == Screen::main_menu, "the main menu did not open");
    const auto* fonts = engine_settings_fonts();
    require(fonts != nullptr, "the dialog's fonts did not load");
    send_check_pointer(SDL_EVENT_MOUSE_MOTION, kRestingPointer, 0);
    const auto sparks = menu_sparks_;
    const auto frame = [&] {
        menu_sparks_ = sparks;
        return frame_without_cursor();
    };
    const auto menu = frame();
    // The window's frame, read back whole, drawn from the same sparks.
    const auto window_frame = [&] {
        renderer::Surface presented;
        menu_sparks_ = sparks;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        return presented;
    };
    // The main menu at each larger window, with no OA screen open, which
    // the notice's backdrop darkens there.
    std::vector<renderer::Surface> closed_windows;
    oa_layer().read_whole_window(true);
    for (const LayerWindow& larger : larger_layer_windows) {
        window_of(larger.width, larger.height);
        closed_windows.push_back(window_frame());
    }
    oa_layer().read_whole_window(false);
    window_of(kCanvasWidth, kCanvasHeight);
    // A run nobody watches leaves it due.
    for (int pass = 0; pass < 4; ++pass)
        tell_saves_moved();
    require(
        !saves_notice_shown() && saves_notice_due_in(preference_values_),
        "a run nobody watches showed the notice"
    );
    auto& state = user_folder_state();
    state.check_shows_notice = true;
    for (int pass = 0; pass < 4; ++pass)
        tell_saves_moved();
    require(saves_notice_shown(), "the notice did not show over the main menu");
    const auto told_now = platform_preferences::load(preference_path_);
    require(
        !saves_notice_due_in(preference_values_) &&
            told_now.at(std::string(saves_notice_preference)) == saves_notice_told &&
            told_now.at(std::string(loose_saves_notice_preference)) == saves_notice_told,
        "the notice shown was not recorded told"
    );
    const auto* notice_screen =
        dynamic_cast<const NoticeScreen*>(oa_layer().find(NoticeScreen::screen_name));
    require(
        notice_screen != nullptr && notice_screen->over() == Screen::main_menu,
        "the notice is not a notice screen of the OA layer over the main menu"
    );
    const auto& notice = notice_screen->notice();
    require(
        notice.title == "SAVED GAMES MOVED" && notice.paragraphs.size() == 4 &&
            notice.paragraphs[0].text == "5 saved games have moved to:" &&
            notice.paragraphs[1].path && notice.paragraphs[1].text == path_to_utf8(saves),
        "the notice does not say that 5 saved games moved to Saves/default"
    );
    // Over the darkened main menu, centred, in the settings dialog's look.
    const int32_t height = settings::notice_height(notice, fonts);
    const artless::Placement placement{
        (kCanvasWidth - settings::notice_width) / 2, (kCanvasHeight - height) / 2, 1
    };
    const auto placed = notice_screen->placement(oa_layer().view());
    require(
        placed.shown.x == placement.x && placed.shown.y == placement.y &&
            placed.shown.width == settings::notice_width && placed.shown.height == height &&
            placed.points_width == settings::notice_width && placed.points_height == height,
        "the notice is not centred on the main menu at 1x"
    );
    // The 640x480 window shows a picture: the frame under the layer in it,
    // the layer's screens over it in the window's own pixels and the cursor
    // above both, its square left out.
    const auto window_shows = [&](const renderer::Surface& picture, std::string_view step) {
        int kept_width = 0;
        int kept_height = 0;
        SDL_GetWindowSize(sdl_.window, &kept_width, &kept_height);
        require(
            SDL_SetWindowSize(sdl_.window, kCanvasWidth, kCanvasHeight) &&
                SDL_SyncWindow(sdl_.window),
            "the window did not take 640x480"
        );
        apply_output_mode();
        renderer::Surface presented;
        menu_sparks_ = sparks;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        auto corrected = picture;
        apply_gamma_rgb(corrected.rgb.data(), corrected.rgb.size() / 3U, 3);
        const artless::SourceRect cursor{
            kRestingPointer.x - kCursorReach,
            kRestingPointer.y - kCursorReach,
            2 * kCursorReach,
            2 * kCursorReach
        };
        const auto differing = differing_pixels(presented, corrected, cursor);
        if (differing != 0) {
            step_snapshot(options_.snapshot, std::string(step) + "-presented", presented);
            step_snapshot(options_.snapshot, std::string(step) + "-picture", corrected);
        }
        require(
            SDL_SetWindowSize(sdl_.window, kept_width, kept_height) && SDL_SyncWindow(sdl_.window),
            "the window did not take its size back"
        );
        apply_output_mode();
        return differing;
    };
    {
        auto expected = menu;
        artless::blend_source_rect(
            expected,
            {0, 0, 1},
            {0, 0, static_cast<int32_t>(expected.width), static_cast<int32_t>(expected.height)},
            settings::backdrop_color,
            settings::menu_backdrop_opacity
        );
        const auto darkened = expected;
        settings::draw_notice(expected, placement, notice, *fonts, engine_settings_icon());
        // Its corners rounded, the darkened menu showing outside them.
        round_drawn_screen(expected, darkened, placed);
        const auto shown = frame();
        step_snapshot(options_.snapshot, "notice", shown);
        require(
            differing_pixels(shown, expected) == 0,
            "the notice is not drawn centred over the darkened main menu"
        );
        const auto differing_shown = window_shows(expected, "notice");
        require(
            differing_shown == 0,
            "the 640x480 window does not show the notice over the darkened main menu: " +
                std::to_string(differing_shown) + " pixels differ"
        );
    }
    // At 800x600 and 2560x1440 the notice is laid out at the window's
    // class and drawn at its scale, centred over the main menu darkened
    // over the whole window.
    oa_layer().read_whole_window(true);
    for (std::size_t index = 0; index < larger_layer_windows.size(); ++index) {
        const LayerWindow& larger = larger_layer_windows[index];
        const std::string on = " on the " + std::to_string(larger.width) + 'x' +
                               std::to_string(larger.height) + " window";
        window_of(larger.width, larger.height);
        const auto presented = window_frame();
        require(
            oa_layer().screen_class() == larger.size_class &&
                oa_layer().screen_scale() == larger.scale,
            "the notice is not laid out at " + std::string(size_class_name(larger.size_class)) +
                ", " + std::to_string(larger.scale) + "x" + on
        );
        kit::Notice sized = notice_screen->notice();
        sized.size_class = larger.size_class;
        const auto& metrics = kit::metrics_of(larger.size_class);
        const int32_t sized_height = settings::notice_height(sized, fonts);
        const auto at = notice_screen->placement(oa_layer().view());
        require(
            at.points_width == metrics.notice_width && at.points_height == sized_height &&
                at.shown.width == metrics.notice_width * larger.scale &&
                at.shown.height == sized_height * larger.scale &&
                at.shown.x == (larger.width - at.shown.width) / 2 &&
                at.shown.y == (larger.height - at.shown.height) / 2,
            "the notice is not centred in the window at its class's size" + on
        );
        renderer::Surface drawing;
        drawing.width = static_cast<uint32_t>(at.shown.width);
        drawing.height = static_cast<uint32_t>(at.shown.height);
        drawing.rgb.assign(static_cast<std::size_t>(drawing.width) * drawing.height * 3U, 0);
        settings::draw_notice(drawing, {0, 0, larger.scale}, sized, *fonts, engine_settings_icon());
        apply_gamma_rgb(drawing.rgb.data(), drawing.rgb.size() / 3U, 3);
        auto expected = expected_layer_frame(closed_windows[index], drawing, at);
        // The closed frame holds the larger icons. Darkening that frame is
        // not what the window draws: each icon is darkened and blended on
        // its own, over its window rectangle. Those squares are taken from
        // the window. The notice covers its own place.
        EngineSettingsMenuHost::copy_presented_menu_buttons(
            *this, presented, expected, {at.shown.x, at.shown.y, at.shown.width, at.shown.height}
        );
        const auto differing = layer_differences(
            presented, expected, window_pointer(oa_layer().view(), kRestingPointer)
        );
        if (differing != 0) {
            const std::string size =
                std::to_string(larger.width) + 'x' + std::to_string(larger.height);
            step_snapshot(options_.snapshot, "notice-presented-" + size, presented);
            step_snapshot(options_.snapshot, "notice-expected-" + size, expected);
        }
        require(
            differing == 0,
            "the window does not show the notice over the darkened main menu" + on + ": " +
                std::to_string(differing) + " pixels differ"
        );
        std::cout << "user folder check: the notice at " << size_class_name(larger.size_class)
                  << ", " << larger.scale << "x, on the " << larger.width << 'x' << larger.height
                  << " window\n";
    }
    oa_layer().read_whole_window(false);
    window_of(kCanvasWidth, kCanvasHeight);
    // A frame lays the notice out at Compact again, for the clicks below.
    static_cast<void>(frame());
    // Its buttons, where the OA layer shows it: Open folder shows Saves and
    // the notice stays; one that fails says why in it.
    const auto parts = settings::notice_layout(notice, fonts);
    const auto button = [&](int32_t control) {
        for (const auto& part : parts)
            if (part.control == control)
                return oa::ui::display_layout::Point{
                    placed.shown.x + part.rect.x + part.rect.width / 2,
                    placed.shown.y + part.rect.y + part.rect.height / 2
                };
        throw std::runtime_error("user folder check: the notice has no such button");
    };
    const auto click = [&](oa::ui::display_layout::Point at) {
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, at, 0);
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, at, SDL_BUTTON_LEFT);
        send_check_pointer(SDL_EVENT_MOUSE_BUTTON_UP, at, SDL_BUTTON_LEFT);
    };
    state.opened.clear();
    click(button(settings::notice_open_control));
    require(
        saves_notice_shown() && oa_layer().find(NoticeScreen::screen_name) == notice_screen &&
            state.opened == std::vector<fs::path>{saves},
        "Open folder did not show Saves, or closed the notice"
    );
    const FolderOpenerHooks recording = state.opener;
    state.opener = FolderOpenerHooks{nullptr, refuse_folder};
    click(button(settings::notice_open_control));
    state.opener = recording;
    require(
        saves_notice_shown() && notice_screen->notice().failure == no_file_manager_text,
        "the notice does not say why the folder could not be shown"
    );
    // Enter closes it, and its release never reaches the main menu.
    const auto key = [this](SDL_EventType type, SDL_Keycode code) {
        SDL_Event event{};
        event.type = type;
        event.key.windowID = SDL_GetWindowID(sdl_.window);
        event.key.key = code;
        event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
        event.key.down = type == SDL_EVENT_KEY_DOWN;
        bool running = true;
        dispatch_event(event, running);
        return running;
    };
    require(key(SDL_EVENT_KEY_DOWN, SDLK_RETURN), "Enter on the notice ended the run");
    require(!saves_notice_shown(), "Enter did not close the notice");
    require(key(SDL_EVENT_KEY_UP, SDLK_RETURN), "Enter's release ended the run");
    require(screen_ == Screen::main_menu, "Enter on the notice left the main menu");
    // Told, it shows no more.
    for (int pass = 0; pass < 4; ++pass)
        tell_saves_moved();
    require(!saves_notice_shown() && state.notices_shown == 1, "the notice showed again");
    std::cout << "user folder check: the notice showed once over the main menu at " << placement.x
              << ',' << placement.y
              << ", showed Saves, said why one could not be shown and "
                 "closed on Enter\n";

    // Notices and questions nobody asked for wait for the OA layer, first
    // in, first out, and show one at a time: none over Settings, none over
    // another. A question Settings asks of its own shows over it at once,
    // Settings darkened under it with the menu.
    {
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, kRestingPointer, 0);
        const auto closed = frame();
        open_engine_settings_from_menu();
        const auto* dialog = engine_settings_dialog();
        require(
            dialog != nullptr && oa_layer().top() != nullptr &&
                oa_layer().top()->name() == "settings",
            "the settings dialog did not open on the OA layer"
        );
        auto answered = std::make_shared<std::vector<int32_t>>();
        const auto check_host = [answered] {
            QuestionScreen::Host host;
            host.answer = [answered](int32_t pressed) {
                answered->push_back(pressed);
                return true;
            };
            return host;
        };
        UserFolderState::show_notice(
            *this, check_notice("CHECK NOTICE ONE", saves), saves, Screen::main_menu
        );
        oa_layer().show_when_free(
            std::make_unique<QuestionScreen>(
                oa_layer(), Screen::main_menu, check_question("CHECK QUESTION"), check_host()
            )
        );
        UserFolderState::show_notice(
            *this, check_notice("CHECK NOTICE TWO", saves), saves, Screen::main_menu
        );
        tick_screen_packages();
        require(
            !saves_notice_shown() && oa_layer().find(QuestionScreen::screen_name) == nullptr &&
                oa_layer().top()->name() == "settings" &&
                oa_layer().waiting(NoticeScreen::screen_name) != nullptr &&
                oa_layer().waiting(QuestionScreen::screen_name) != nullptr,
            "a notice or a question nobody asked for opened over Settings"
        );
        // The question Settings asks, over it at once.
        oa_layer().push(
            std::make_unique<QuestionScreen>(
                oa_layer(), Screen::main_menu, check_question("SETTINGS ASKS"), check_host()
            )
        );
        const auto* asked = oa_layer().top();
        require(
            asked != nullptr && asked->name() == QuestionScreen::screen_name,
            "the question Settings asks is not over it"
        );
        const auto& question = static_cast<const QuestionScreen*>(asked)->question();
        const int32_t question_height = settings::prompt_height(question, fonts);
        const artless::Placement question_at{
            (kCanvasWidth - settings::notice_width) / 2, (kCanvasHeight - question_height) / 2, 1
        };
        const auto question_placed = asked->placement(oa_layer().view());
        require(
            question_placed.shown.x == question_at.x && question_placed.shown.y == question_at.y &&
                question_placed.shown.height == question_height,
            "the question is not centred on the main menu at 1x"
        );
        {
            // The menu darkened under Settings and again under the
            // question, Settings darkened under the question, and the
            // question over both.
            auto expected = closed;
            const auto darken_whole = [&expected] {
                artless::blend_source_rect(
                    expected,
                    {0, 0, 1},
                    {0,
                     0,
                     static_cast<int32_t>(expected.width),
                     static_cast<int32_t>(expected.height)},
                    settings::backdrop_color,
                    settings::menu_backdrop_opacity
                );
            };
            darken_whole();
            const auto under_settings = expected;
            settings::draw_dialog(
                expected,
                {(kCanvasWidth - settings::dialog_width) / 2,
                 (kCanvasHeight - settings::dialog_height) / 2,
                 1},
                *dialog,
                *fonts,
                EngineSettingsMenuHost::settings_icon(*this)
            );
            // Each with its corners rounded, what lies under it showing
            // outside them.
            round_drawn_screen(expected, under_settings, oa_layer().placement_of("settings"));
            darken_whole();
            const auto under_question = expected;
            settings::draw_prompt(expected, question_at, question, *fonts, engine_settings_icon());
            round_drawn_screen(expected, under_question, question_placed);
            const auto shown = frame();
            step_snapshot(options_.snapshot, "question-over-settings", shown);
            require(
                differing_pixels(shown, expected) == 0,
                "the question is not drawn over Settings darkened under it"
            );
            const auto differing_shown = window_shows(expected, "question-over-settings");
            require(
                differing_shown == 0,
                "the 640x480 window does not show the question over Settings darkened under it: " +
                    std::to_string(differing_shown) + " pixels differ"
            );
        }
        // Escape answers it, and Settings is on top again; the others wait.
        require(key(SDL_EVENT_KEY_DOWN, SDLK_ESCAPE), "Escape on the question ended the run");
        require(key(SDL_EVENT_KEY_UP, SDLK_ESCAPE), "Escape's release ended the run");
        require(
            *answered == std::vector<int32_t>{0} && engine_settings_dialog() != nullptr &&
                oa_layer().top() != nullptr && oa_layer().top()->name() == "settings" &&
                !saves_notice_shown(),
            "Escape did not answer the question over Settings, or reached Settings"
        );
        // Settings closes: the first notice shows, and the others wait.
        const auto shown_title = [this] {
            const auto* shown_notice =
                dynamic_cast<const NoticeScreen*>(oa_layer().find(NoticeScreen::screen_name));
            return shown_notice != nullptr ? shown_notice->notice().title : std::string();
        };
        require(key(SDL_EVENT_KEY_DOWN, SDLK_ESCAPE), "Escape in Settings ended the run");
        require(key(SDL_EVENT_KEY_UP, SDLK_ESCAPE), "Escape's release ended the run");
        require(
            engine_settings_dialog() == nullptr && shown_title() == "CHECK NOTICE ONE" &&
                oa_layer().find(QuestionScreen::screen_name) == nullptr &&
                oa_layer().waiting(QuestionScreen::screen_name) != nullptr &&
                oa_layer().waiting(NoticeScreen::screen_name) != nullptr,
            "closing Settings did not show the first notice alone"
        );
        // Each closed shows the next, alone.
        require(key(SDL_EVENT_KEY_DOWN, SDLK_RETURN), "Enter on the notice ended the run");
        require(key(SDL_EVENT_KEY_UP, SDLK_RETURN), "Enter's release ended the run");
        const auto* waited =
            dynamic_cast<const QuestionScreen*>(oa_layer().find(QuestionScreen::screen_name));
        require(
            !saves_notice_shown() && waited != nullptr &&
                waited->question().title == "CHECK QUESTION" &&
                oa_layer().waiting(NoticeScreen::screen_name) != nullptr,
            "closing the first notice did not show the question alone"
        );
        require(key(SDL_EVENT_KEY_DOWN, SDLK_RETURN), "Enter on the question ended the run");
        require(key(SDL_EVENT_KEY_UP, SDLK_RETURN), "Enter's release ended the run");
        require(
            *answered == std::vector<int32_t>{0, 1} && shown_title() == "CHECK NOTICE TWO" &&
                oa_layer().find(QuestionScreen::screen_name) == nullptr &&
                oa_layer().waiting(NoticeScreen::screen_name) == nullptr,
            "answering the question did not show the last notice alone"
        );
        require(key(SDL_EVENT_KEY_DOWN, SDLK_RETURN), "Enter on the notice ended the run");
        require(key(SDL_EVENT_KEY_UP, SDLK_RETURN), "Enter's release ended the run");
        require(
            oa_layer().top() == nullptr &&
                oa_layer().waiting(NoticeScreen::screen_name) == nullptr &&
                screen_ == Screen::main_menu && engine_settings_dialog() == nullptr,
            "the last notice did not close, or a key reached the main menu"
        );
        std::cout << "user folder check: two notices and a question waited for Settings and "
                     "showed one at a time, first in first; a question over Settings darkened it\n";
    }
    window_of(run_width, run_height);

    // A save, a screenshot and a film land in the folders of the mod
    // played: default without a mod, and the mod's id with a made-up one,
    // whose save the dialogs list for it alone.
    const auto played = options_.mod_profile;
    auto made_up = std::make_shared<data::mod_profile::ModProfile>();
    made_up->id = "made-up-mod";
    const auto listed_saves = [this] {
        const auto listing = save_roots();
        auto files = ui::frontend::savegame_host_files(&listing);
        std::vector<std::string> found;
        files.find(
            files.context,
            "SAVEGAME\\*.SAV",
            [](void* user, const data::campaign::FindRecord& record) {
                static_cast<std::vector<std::string>*>(user)->push_back(record.name);
            },
            &found
        );
        return found;
    };
    const auto lands = [&](std::string_view folder, std::string_view save_name) {
        const fs::path save =
            game_file_path("SAVEGAME\\" + std::string(save_name), ui::frontend::SavePathUse::write);
        write_file(save, "check");
        capture_named_screenshot();
        const std::string shot = status_;
        const std::string film = output + "\\MOVIE001";
        fs::create_directories(game_file_path(film, ui::frontend::SavePathUse::write), error);
        begin_film_capture(film.c_str());
        const auto names = listed_saves();
        require(
            save == user_folder_ / "Saves" / folder / save_name &&
                std::find(names.begin(), names.end(), save_name) != names.end(),
            "a save does not land in Saves/" + std::string(folder) + ", or is not listed there"
        );
        require(
            fs::is_regular_file(user_folder_ / "Screenshots" / folder / path_from_utf8(shot)) &&
                holds_a_file(user_folder_ / "Films" / folder / "MOVIE001"),
            "a screenshot or a film does not land in Screenshots/" + std::string(folder) +
                " and Films/" + std::string(folder)
        );
    };
    lands("default", "CHECK.SAV");
    require(
        game_file_path("SAVEGAME\\x\\..\\..\\outside.SAV", ui::frontend::SavePathUse::write)
            .empty(),
        "a saved game's name that leaves the saved games folder is given a place"
    );
    options_.mod_profile = made_up;
    lands("made-up-mod", "MADEUP.SAV");
    const auto mod_saves = listed_saves();
    options_.mod_profile = played;
    const auto game_saves = listed_saves();
    require(
        std::find(mod_saves.begin(), mod_saves.end(), "CHECK.SAV") == mod_saves.end() &&
            std::find(game_saves.begin(), game_saves.end(), "MADEUP.SAV") == game_saves.end(),
        "a made-up mod and the game without a mod share their saved games"
    );
    std::cout << "user folder check: a save, a screenshot and a film land in Saves, Screenshots "
                 "and Films/default without a mod and in their made-up-mod folders with one\n";

    // Your files: the row shows the folder; its buttons show Saves and
    // Screenshots, which hold each mod's folder, and the Mods folder, made
    // when missing.
    open_engine_settings_from_menu();
    auto* dialog = engine_settings_dialog();
    require(dialog != nullptr, "the settings dialog did not open");
    require(dialog->user_folder == path_to_utf8(user_folder_), "Your files shows another folder");
    dialog->page = settings::Page::common_tweaks;
    step_snapshot(options_.snapshot, "your-files", frame());
    state.opened.clear();
    for (const auto which :
         {settings::FolderButton::saves,
          settings::FolderButton::screenshots,
          settings::FolderButton::mods}) {
        dialog->folder_to_open = which;
        require(
            !take_engine_settings_action(settings::DialogAction::open_folder),
            "a Your files button closed the dialog"
        );
    }
    const std::vector<fs::path> shown{loose, user_folder_ / "Screenshots", user_folder_ / "Mods"};
    require(state.opened == shown, "Your files did not show Saves, Screenshots and Mods");
    require(
        std::all_of(
            shown.begin(),
            shown.end(),
            [](const fs::path& folder) {
                std::error_code missing;
                return fs::is_directory(folder, missing);
            }
        ),
        "Your files did not make the folders it showed"
    );
    std::ignore = take_engine_settings_action(settings::DialogAction::cancelled);
    // The settings screen goes from the OA layer with its dialog.
    oa_layer().close_top();

    // The Mods folder's mods are offered beside the game folder's, a folder
    // without an oamod.yaml among them, each with what the Library shows of it.
    write_file(
        user_folder_ / "Mods" / "Check Mod" / std::string(mod_profile_name),
        "id: check\nname: Check Mod Title\nversion: \"3.2\"\ndescription: A check's mod.\n"
    );
    fs::create_directories(user_folder_ / "Mods" / "Plain Folder");
    const auto listed = list_mods_in(user_folder_ / "Mods");
    require(
        listed.size() == 2 && listed.front().filename() == "Check Mod" &&
            listed.back().filename() == "Plain Folder",
        "the Mods folder's mods are not listed"
    );
    load_engine_settings();
    const auto& settings_state = engine_settings_state();
    const auto details_of = [&settings_state](const fs::path& folder) {
        const auto& offered = settings_state.mod_folders;
        const auto found = std::find(offered.begin(), offered.end(), path_to_utf8(folder));
        require(found != offered.end(), "the Library does not offer the Mods folder's mods");
        const auto index = static_cast<std::size_t>(std::distance(offered.begin(), found));
        return std::pair{settings_state.mod_names[index], settings_state.mod_details[index]};
    };
    const auto [titled, titled_details] = details_of(listed.front());
    require(
        titled == "Check Mod Title" && titled_details.version == "3.2" &&
            titled_details.description == "A check's mod." && titled_details.has_profile,
        "the Library does not show the oamod.yaml's name, version and description"
    );
    const auto [plain, plain_details] = details_of(listed.back());
    require(
        plain == "Plain Folder" && plain_details.version == "N/A" &&
            plain_details.description == "No oamod.yaml present" && !plain_details.has_profile,
        "the Library does not show a folder without an oamod.yaml by its name"
    );
    fake_frontend_tick_ = previous_tick;
    send_check_pointer(SDL_EVENT_MOUSE_MOTION, kRestingPointer, 0);
    std::cout << "user folder check: Your files showed " << path_to_utf8(user_folder_)
              << " and its Saves, Screenshots and Mods; the Library lists Mods' mods\n";

    // Nothing is left behind for the next run.
    fs::remove_all(user_folder_, error);
    fs::remove_all(earlier_root / "SAVEGAME", error);
    fs::remove_all(earlier_root / "demos", error);
    fs::remove_all(earlier_root / "mods", error);
    preference_values_.clear();
    preferences_dirty_ = true;
    std::cout << "user folder check: passed\n";
}

} // namespace oa::app
