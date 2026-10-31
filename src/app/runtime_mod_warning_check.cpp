// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-mod-warning: a mod whose files are missing is switched to and
// plays its menus, and warns of them: its games start without a side's
// missing interface art and font, and cannot start without its units. Each
// run of the check is one turn. The first writes four made-up profiles into
// the player's own Mods folder, and two folders without a profile: one
// whose second side's intgaf and font name files no folder holds, and one
// whose second side lacks its LOGO section, whose start would end and whose
// switch is refused, a note over the Library saying why; it switches to the
// first of those folders. Every switch is the Library's PLAY NOW. The second
// sees its warning of both files once over the main menu, then plays a
// skirmish on that side, its HUD without those panels and in COMIX; it
// switches to a profile whose SIDEDATA names the same files, and the third
// does the same. The fourth plays the profile whose unit files are missing:
// its warning shows once over the main menu, then over a refused Skirmish
// start that stays on the setup, and a start past the warning fails without
// ending the run, whose warning a finger's tap just under OK closes. The
// fifth plays a profile whose first side's commander has
// its file but the unit catalog drops it, by its Copyright line, and whose
// second side's intgaf names a GAF no folder holds: it finds that commander
// missing, and no other, and that GAF. The sixth plays the profile that
// plays whole: it sees no warning and starts a skirmish.

#include "engine_settings_menu_host.hpp"
#include "engine_settings_state.hpp"
#include "library_host.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "touch_state.hpp"
#include "user_folder_state.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/engine_settings/notice.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/library/library.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

namespace oa::app {

namespace settings = oa::ui::engine_settings;
namespace library = oa::ui::library;

namespace {

/// The profile whose unit files are missing: it reads units from files of
/// an extension no folder holds.
constexpr std::string_view kHollowFolder = "warning-check-hollow";
/// Its oamod.yaml.
constexpr std::string_view kHollowProfile =
    "oamod: 1\n"
    "id: warning-check-hollow\n"
    "name: Warning Check Hollow\n"
    "version: \"1.0\"\n"
    "description: A made-up profile whose unit files are missing.\n"
    "requires: {base: ta-3.1c, catalogue: 1}\n"
    "author: {name: unknown}\n"
    "packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}\n"
    "layout:\n"
    "  file-extensions: {unit-definition: HLW}\n";
/// The profile whose first side's commander the unit catalog drops: the
/// game's own units, with that commander's file in the profile's folder
/// carrying a Copyright line of its own, and the game's own SIDEDATA with
/// the second side's intgaf naming kMissingPanels.
constexpr std::string_view kDroppedFolder = "warning-check-dropped";
/// Its oamod.yaml.
constexpr std::string_view kDroppedProfile =
    "oamod: 1\n"
    "id: warning-check-dropped\n"
    "name: Warning Check Dropped\n"
    "version: \"1.0\"\n"
    "description: A made-up profile whose first side's commander is dropped.\n"
    "requires: {base: ta-3.1c, catalogue: 1}\n"
    "author: {name: unknown}\n"
    "packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}\n";
/// The GAF the dropped profile's and the side art folders' second side
/// names for its panels, which no folder holds.
constexpr std::string_view kMissingPanels = "WARNCHECKINT";
/// The font the side art folders' second side names, which no folder holds.
constexpr std::string_view kMissingFont = "WARNCHECKFNT";
/// A folder without a profile: the game's own SIDEDATA with the second
/// side's intgaf naming kMissingPanels and its font kMissingFont.
constexpr std::string_view kSidelessFolder = "warning-check-sideless";
/// The profile whose SIDEDATA is the folder without a profile's: the game's
/// own units.
constexpr std::string_view kSideArtFolder = "warning-check-sideart";
/// Its oamod.yaml.
constexpr std::string_view kSideArtProfile =
    "oamod: 1\n"
    "id: warning-check-sideart\n"
    "name: Warning Check Side Art\n"
    "version: \"1.0\"\n"
    "description: A made-up profile whose second side's art and font are missing.\n"
    "requires: {base: ta-3.1c, catalogue: 1}\n"
    "author: {name: unknown}\n"
    "packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}\n";
/// Match ticks a skirmish over the side art folders plays.
constexpr uint32_t kSideArtTicks = 300;
/// A folder without a profile: the game's own SIDEDATA without the second
/// side's LOGO section.
constexpr std::string_view kLogolessFolder = "warning-check-logoless";
/// The profile that plays whole: the game's own units.
constexpr std::string_view kWholeFolder = "warning-check-whole";
/// Its oamod.yaml.
constexpr std::string_view kWholeProfile =
    "oamod: 1\n"
    "id: warning-check-whole\n"
    "name: Warning Check Whole\n"
    "version: \"1.0\"\n"
    "description: A made-up profile that plays whole.\n"
    "requires: {base: ta-3.1c, catalogue: 1}\n"
    "author: {name: unknown}\n"
    "packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}\n";

/// The touch device the check's finger comes from, which no system registers.
constexpr SDL_TouchID kCheckTouchDevice = 0x70c5;

/// Where the pointer rests while nothing is clicked: off every button.
constexpr oa::ui::display_layout::Point kRestingPointer{4, 240};
/// The window's size for the snapshots: a 16:9 window.
constexpr int kSnapshotWidth = 1280;
/// The window's height for the snapshots.
constexpr int kSnapshotHeight = 720;

/// Throws when a step of the check fails.
///
/// @param ok the step passed
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("mod warning check: " + std::string(what));
}

/// Returns a folder's path as the settings keep it: absolute, normal, in UTF-8.
///
/// @param folder the folder
/// @return its path
std::string kept_path(const fs::path& folder) {
    std::error_code error;
    const fs::path absolute = fs::absolute(folder, error);
    return path_to_utf8((error ? folder : absolute).lexically_normal());
}

/// Returns SIDEDATA's text with the value of a key of SIDE1's section
/// replaced: the first key of that name after the section's head, before
/// any of its own sections.
///
/// @param sidedata the text
/// @param key the key, in lower case
/// @param value the new value
/// @return the text
std::string
name_second_side_file(std::string sidedata, std::string_view key, std::string_view value) {
    std::string folded = sidedata;
    for (char& c : folded)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto section = folded.find("[side1]");
    auto at = section;
    std::size_t equals = std::string::npos;
    // A whole key: after a space, a line's end, a brace or a semicolon, and
    // followed by its '=', so that font does not match fontgui.
    while (at != std::string::npos && equals == std::string::npos) {
        at = folded.find(key, at + 1);
        if (at == std::string::npos)
            break;
        const char before = folded[at - 1];
        const auto next = folded.find_first_not_of(" \t", at + key.size());
        if ((std::isspace(static_cast<unsigned char>(before)) != 0 || before == '{' ||
             before == ';') &&
            next != std::string::npos && folded[next] == '=')
            equals = next;
    }
    const auto end = equals == std::string::npos ? equals : folded.find(';', equals);
    require(
        end != std::string::npos, "the game's SIDEDATA names no " + std::string(key) + " for SIDE1"
    );
    return sidedata.replace(equals + 1, end - equals - 1, value);
}

/// Returns SIDEDATA's text without SIDE1's LOGO section.
///
/// @param sidedata the text
/// @return the text
std::string drop_second_side_logo(std::string sidedata) {
    std::string folded = sidedata;
    for (char& c : folded)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    const auto section = folded.find("[side1]");
    const auto logo = section == std::string::npos ? section : folded.find("[logo]", section);
    const auto end = logo == std::string::npos ? logo : folded.find('}', logo);
    require(end != std::string::npos, "the game's SIDEDATA has no LOGO for SIDE1");
    return sidedata.erase(logo, end + 1 - logo);
}

/// Writes a profile into its folder, made when missing.
///
/// @param folder the folder
/// @param profile the oamod.yaml's text
void write_profile(const fs::path& folder, std::string_view profile) {
    fs::create_directories(folder);
    std::ofstream file(folder / "oamod.yaml", std::ios::binary | std::ios::trunc);
    file << profile;
    require(static_cast<bool>(file), "a test profile could not be written");
}

} // namespace

void Runtime::check_mod_warning() {
    require(!user_folder_.empty(), "the run has no folder of the player's own");
    require(sdl_.window != nullptr, "the check needs the SDL presenter");
    const fs::path mods = user_folder_ / std::string(user_mods_folder_name);
    const std::string hollow = kept_path(mods / std::string(kHollowFolder));
    const std::string dropped = kept_path(mods / std::string(kDroppedFolder));
    const std::string whole = kept_path(mods / std::string(kWholeFolder));
    const std::string sideless = kept_path(mods / std::string(kSidelessFolder));
    const std::string sideart = kept_path(mods / std::string(kSideArtFolder));
    // The first side and its commander, which every profile here takes from
    // the game's own SIDEDATA.
    const oa::Side* first = oa::data::defs::side_at(side_table_, 0);
    const std::string commander(
        first->commander, ::strnlen(first->commander, sizeof first->commander)
    );
    const std::string side(first->name, ::strnlen(first->name, sizeof first->name));
    const auto commander_line = [&side, &commander] {
        return "The " + side + " commander (" + commander + ") isn't in this mod's units.";
    };
    auto& settings_state = engine_settings_state();
    auto& state = user_folder_state();
    // The notices show although nobody watches the check.
    state.check_shows_notice = true;
    // The warning on the OA layer, and where the layer shows it.
    const auto shown_warning = [this]() -> const NoticeScreen& {
        const auto* shown =
            dynamic_cast<const NoticeScreen*>(oa_layer().find(NoticeScreen::screen_name));
        require(shown != nullptr, "no warning is on the OA layer");
        return *shown;
    };
    const auto warning_place = [&]() { return shown_warning().placement(oa_layer().view()); };

    // The window's frame of a step, when --snapshot names a file.
    if (!options_.snapshot.empty() &&
        (!SDL_SetWindowSize(sdl_.window, kSnapshotWidth, kSnapshotHeight) ||
         !SDL_SyncWindow(sdl_.window)))
        throw std::runtime_error(std::string("SDL_SetWindowSize: ") + SDL_GetError());
    const auto snapshot = [this](std::string_view step) {
        if (options_.snapshot.empty())
            return;
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, kRestingPointer, 0);
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        auto file = options_.snapshot;
        file.replace_extension();
        file += "-" + std::string(step) + ".ppm";
        write_ppm(file, presented);
    };
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
    // A press and a release of a key that closes the warning, which stays on
    // the screen it showed over.
    const auto close_with = [&](SDL_Keycode code, Screen screen, std::string_view where) {
        require(key(SDL_EVENT_KEY_DOWN, code), "a key on the warning ended the run");
        require(!saves_notice_shown(), "a key did not close the warning");
        require(key(SDL_EVENT_KEY_UP, code), "a key's release ended the run");
        require(screen_ == screen, std::string("closing the warning left ") + std::string(where));
    };
    // A finger's tap through the touch dispatcher just under the warning's
    // OK, within a finger's reach of it, which closes the warning.
    const auto tap_under_ok = [&](Screen screen, std::string_view where) {
        const auto* fonts = engine_settings_fonts();
        const auto placed = warning_place();
        const auto& shown = shown_warning().notice();
        // A finger's reach, in the warning's points as the layer shows it:
        // the pick distance in the picture's pixels, over the picture's
        // pixels a point takes.
        const LayerView seen = oa_layer().view();
        const int32_t reach = EngineSettingsState::finger_reach(
            *this,
            static_cast<double>(placed.shown.width) / static_cast<double>(placed.points_width) *
                static_cast<double>(seen.picture_size.x) / static_cast<double>(seen.picture.width)
        );
        oa::ui::display_layout::Point pixel{};
        for (const auto& part : settings::notice_layout(shown, fonts))
            if (part.control == settings::notice_ok_control)
                pixel = layer_window_pixel(
                    placed,
                    {part.rect.x + part.rect.width / 2,
                     part.rect.y + part.rect.height - 1 + std::max(1, reach * 2 / 3)}
                );
        touch_state().dispatch.accept_unregistered_touch = true;
        for (const SDL_EventType type : {SDL_EVENT_FINGER_DOWN, SDL_EVENT_FINGER_UP}) {
            SDL_Event event =
                window_finger_event(sdl_.window, sdl_.renderer, type, pixel, kCheckTouchDevice, 1);
            bool running = true;
            dispatch_event(event, running);
            require(running, "a finger on the warning ended the run");
        }
        require(!saves_notice_shown(), "a finger just under OK did not close the warning");
        require(screen_ == screen, std::string("closing the warning left ") + std::string(where));
    };
    // PLAY NOW on a mod folder's entry in the Library, as the run's own
    // feed lists it. A folder without an oamod.yaml is marked so, which its
    // status line says.
    const auto press_switch = [&](const std::string& folder) {
        list_offered_mods();
        LibraryHost& host = library_host();
        host.rebuild(*this);
        std::optional<library::EntryId> wanted;
        bool offered = false;
        bool marked = false;
        for (const library::Entry& each : host.model.entries) {
            if (each.id.kind != library::Kind::mod || each.installed == nullptr ||
                kept_path(each.installed->folder) != folder)
                continue;
            wanted = each.id;
            marked = each.installed->without_manifest;
            for (const library::ActionButton& button : library::entry_actions(host.model, each))
                offered = offered || (button.action == library::Action::play_now && button.enabled);
        }
        require(wanted.has_value(), "the Library does not list the test profile");
        require(offered, "the test profile's entry offers no PLAY NOW");
        std::error_code missing;
        require(
            marked == !fs::exists(path_from_utf8(folder) / "oamod.yaml", missing),
            "the Library does not mark whether a mod folder has an oamod.yaml"
        );
        library_entry_action(library::Action::play_now, *wanted);
    };
    // PLAY NOW, which ends the run.
    const auto switch_to = [&](const std::string& folder) {
        press_switch(folder);
        require(soft_restart_requested(), "PLAY NOW did not end the run");
    };

    // The second side's name and the files the side art folders' second side
    // names.
    const oa::Side* second_side = oa::data::defs::side_at(side_table_, 1);
    const std::string second(
        second_side->name, ::strnlen(second_side->name, sizeof second_side->name)
    );
    const std::string panels = "anims/" + std::string(kMissingPanels) + ".GAF";
    const std::string font = "fonts/" + std::string(kMissingFont) + ".FNT";
    const auto side_file_line = [&second](const std::string& path) {
        return "The " + second + " side's " + path +
               " isn't in this mod's files; games show without it.";
    };
    // A side art folder's turn: its warning names both files once over the
    // main menu and says its games still start; a skirmish on the second
    // side starts from Skirmish's Start and plays, its HUD without the
    // side's panels and its numbers in COMIX, unmeasured.
    const auto play_side_art = [&](std::string_view name,
                                   const std::string& folder,
                                   std::string_view step) {
        const ModStartGaps gaps = mod_start_gaps();
        require(
            !gaps.games_cannot_start() && gaps.missing_side_files.size() == 2 &&
                gaps.missing_side_files[0].side == second &&
                gaps.missing_side_files[0].path == panels &&
                gaps.missing_side_files[1].side == second &&
                gaps.missing_side_files[1].path == font,
            "the second side's missing panels and font, and they alone, are not found missing"
        );
        const auto previous_tick = fake_frontend_tick_;
        fake_frontend_tick_ = 1000U;
        load(Screen::main_menu);
        require(engine_settings_fonts() != nullptr, "the dialog's fonts did not load");
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, kRestingPointer, 0);
        for (int pass = 0; pass < 4; ++pass)
            tell_incomplete_mod();
        require(
            saves_notice_shown() && state.mod_warnings_shown == 1 &&
                shown_warning().over() == Screen::main_menu,
            "the side files' warning did not show over the main menu"
        );
        const auto& notice = shown_warning().notice();
        require(
            notice.title == "MOD FILES MISSING" && notice.open_caption == "Open mod folder" &&
                notice.paragraphs.size() == 5 &&
                notice.paragraphs[0].text == std::string(name) + " is missing files." &&
                notice.paragraphs[1].text == side_file_line(panels) &&
                notice.paragraphs[2].text == side_file_line(font) &&
                notice.paragraphs[3].text ==
                    "Its games still start. Add the missing files to its folder to show them:" &&
                notice.paragraphs[4].path && notice.paragraphs[4].text == folder,
            "the warning does not name the second side's panels and font, say that games still "
            "start and show the mod's folder"
        );
        snapshot(std::string(step) + "-warning");
        close_with(SDLK_RETURN, Screen::main_menu, "the main menu");
        for (int pass = 0; pass < 4; ++pass)
            tell_incomplete_mod();
        require(!saves_notice_shown() && state.mod_warnings_shown == 1, "the warning showed again");
        exercise_click(menu::resource_name(menu::Button::single_player));
        exercise_click(entry::resource_name(entry::Button::skirmish));
        require(screen_ == Screen::skirmish, "Skirmish did not open its setup");
        // The player plays the second side, the computer the first.
        skirmish_settings_.slots[0].side = 1;
        skirmish_settings_.slots[1].side = 0;
        exercise_click(skirmish::resource_name(skirmish::Button::start));
        require(
            screen_ == Screen::match && match_ && !saves_notice_shown() &&
                state.mod_warnings_shown == 1,
            "the skirmish did not start, or warned"
        );
        require(
            match_view_side() == 1 && match_hud_.has_value() && match_side_panel_gaf().empty() &&
                match_side_font().empty() && !match_side_names_font_,
            "the second side's HUD is not drawn without its panels and in COMIX, unmeasured"
        );
        for (uint32_t tick = 0; tick < kSideArtTicks; ++tick)
            step_match_simulation();
        render();
        snapshot(std::string(step) + "-match");
        require(
            screen_ == Screen::match && match_ && match_->simulation().tick >= kSideArtTicks,
            "the skirmish did not play its ticks"
        );
        leave_match();
        // Back on the main menu, where the Library's PLAY NOW can be pressed.
        load(Screen::main_menu);
        fake_frontend_tick_ = previous_tick;
        std::cout << "mod warning check: " << name
                  << " warned once of the second side's panels and font, and its skirmish on "
                     "that side played "
                  << kSideArtTicks << " ticks\n";
    };

    if (options_.restarts == 0) {
        // No mod plays and nothing warns.
        write_profile(mods / std::string(kHollowFolder), kHollowProfile);
        write_profile(mods / std::string(kDroppedFolder), kDroppedProfile);
        write_profile(mods / std::string(kSideArtFolder), kSideArtProfile);
        write_profile(mods / std::string(kWholeFolder), kWholeProfile);
        // The dropped profile's commander file, whose Copyright line is not
        // the one the catalog keeps a unit for.
        const fs::path units = mods / std::string(kDroppedFolder) /
                               oa::data::defs::directory_name(oa::data::defs::DataDirectory::units);
        fs::create_directories(units);
        std::ofstream unit(
            units / (commander + "." + oa::data::defs::unit_extension()),
            std::ios::binary | std::ios::trunc
        );
        unit << "[UNITINFO]\r\n{\r\n\tUnitName=" << commander
             << ";\r\n\tVersion=1.0;\r\n\tCopyright=A made-up line;\r\n}\r\n";
        require(static_cast<bool>(unit), "the dropped profile's unit file could not be written");
        unit.close();
        // Its SIDEDATA, whose second side's panels are missing, and the side
        // art folders', whose second side's panels and font are missing.
        const auto read = assets_.read(
            oa::data::defs::data_path(oa::data::defs::DataDirectory::gamedata, "sidedata.tdf")
        );
        const std::string sidedata(read.bytes.begin(), read.bytes.end());
        const std::string named = name_second_side_file(sidedata, "intgaf", kMissingPanels);
        const std::string named_both = name_second_side_file(named, "font", kMissingFont);
        const auto write_sidedata = [&mods](std::string_view folder, const std::string& text) {
            const fs::path gamedata =
                mods / std::string(folder) /
                oa::data::defs::directory_name(oa::data::defs::DataDirectory::gamedata);
            fs::create_directories(gamedata);
            std::ofstream sides(gamedata / "SIDEDATA.TDF", std::ios::binary | std::ios::trunc);
            sides << text;
            require(static_cast<bool>(sides), "a test folder's SIDEDATA could not be written");
        };
        write_sidedata(kDroppedFolder, named);
        write_sidedata(kSideArtFolder, named_both);
        write_sidedata(kSidelessFolder, named_both);
        require(mod_profile() == nullptr && !plays_mod(), "the first run plays a mod");
        require(!mod_start_gaps().any(), "the game played without a mod warns");
        // The folder without a profile whose start would end on its second
        // side's missing LOGO is refused: a note over the Library says why,
        // and the mod played stays chosen. It is listed alone, and goes once
        // refused, so that the Library lists it.
        write_sidedata(kLogolessFolder, drop_second_side_logo(sidedata));
        const std::string played_before = settings_state.current.mod_folder;
        press_switch(kept_path(mods / std::string(kLogolessFolder)));
        {
            const auto* told = dynamic_cast<const QuestionScreen*>(oa_layer().top());
            require(
                !soft_restart_requested() && settings_state.current.mod_folder == played_before &&
                    told != nullptr && told->question().paragraphs.size() == 1 &&
                    told->question().paragraphs[0].text ==
                        oa::data::languages::interface_text(
                            "That folder cannot be played; the log says why."
                        ),
                "the switch to " + std::string(kLogolessFolder) + " was not refused"
            );
        }
        {
            LayerScreen* told = oa_layer().top();
            oa_layer().take_answer(
                *told, told->key(oa::ui::kit::Key::enter, SDLK_RETURN), SDLK_RETURN
            );
            oa_layer().forget_latched_key();
        }
        require(
            dynamic_cast<const QuestionScreen*>(oa_layer().top()) == nullptr,
            "OK did not close the refused switch's note"
        );
        fs::remove_all(mods / std::string(kLogolessFolder));
        // The one whose side files alone are missing is not.
        switch_to(sideless);
        std::cout << "mod warning check: refused the folder whose second side's LOGO is "
                     "missing, and switched to the one whose second side's panels and font are "
                     "missing\n";
        return;
    }

    if (options_.restarts == 1) {
        require(
            settings_state.playing_mod_folder == sideless && mod_profile() == nullptr &&
                plays_mod(),
            "the switch to the folder without a profile did not play it"
        );
        play_side_art("This mod", sideless, "sideless");
        switch_to(sideart);
        return;
    }

    if (options_.restarts == 2) {
        const auto* profile = mod_profile();
        require(
            settings_state.playing_mod_folder == sideart && profile != nullptr &&
                profile->id == kSideArtFolder,
            "the switch to the side art profile did not play it"
        );
        play_side_art("Warning Check Side Art", sideart, "sideart");
        switch_to(hollow);
        return;
    }

    if (options_.restarts == 3) {
        const auto* profile = mod_profile();
        require(
            settings_state.playing_mod_folder == hollow && profile != nullptr &&
                profile->id == kHollowFolder,
            "the switch to the hollow profile did not play it"
        );
        const ModStartGaps gaps = mod_start_gaps();
        require(
            gaps.no_units && gaps.missing_commanders.size() == side_table_.count,
            "the hollow profile's missing units and commanders are not found"
        );
        // The main menu shows; the warning shows once over it.
        const auto previous_tick = fake_frontend_tick_;
        fake_frontend_tick_ = 1000U;
        load(Screen::main_menu);
        require(screen_ == Screen::main_menu, "the main menu did not open");
        require(engine_settings_fonts() != nullptr, "the dialog's fonts did not load");
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, kRestingPointer, 0);
        // The main menu at each larger window, read back whole and drawn
        // from the same sparks, with no OA screen open: what the warning's
        // backdrop darkens there.
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
        const auto sparks = menu_sparks_;
        const auto window_frame = [&] {
            renderer::Surface presented;
            menu_sparks_ = sparks;
            capture_frame_ = &presented;
            render();
            capture_frame_ = nullptr;
            return presented;
        };
        std::vector<renderer::Surface> closed_windows;
        oa_layer().read_whole_window(true);
        for (const LayerWindow& larger : larger_layer_windows) {
            window_of(larger.width, larger.height);
            closed_windows.push_back(window_frame());
        }
        oa_layer().read_whole_window(false);
        window_of(run_width, run_height);
        for (int pass = 0; pass < 4; ++pass)
            tell_incomplete_mod();
        require(
            saves_notice_shown() && state.mod_warnings_shown == 1 &&
                shown_warning().over() == Screen::main_menu,
            "the warning did not show over the main menu"
        );
        const auto& notice = shown_warning().notice();
        require(
            notice.title == "MOD FILES MISSING" && notice.open_caption == "Open mod folder" &&
                notice.paragraphs.front().text == "Warning Check Hollow is missing files." &&
                notice.paragraphs[2].text == commander_line() && notice.paragraphs.back().path &&
                notice.paragraphs.back().text == hollow,
            "the warning does not name the mod, its first side's commander and its folder"
        );
        // At 800x600 and 2560x1440 the warning is laid out at the window's
        // class and drawn at its scale, centred over the main menu darkened
        // over the whole window.
        oa_layer().read_whole_window(true);
        for (std::size_t index = 0; index < larger_layer_windows.size(); ++index) {
            const LayerWindow& larger = larger_layer_windows[index];
            const std::string size =
                std::to_string(larger.width) + 'x' + std::to_string(larger.height);
            window_of(larger.width, larger.height);
            const auto presented = window_frame();
            require(
                oa_layer().screen_class() == larger.size_class &&
                    oa_layer().screen_scale() == larger.scale,
                "the warning is not laid out at " +
                    std::string(size_class_name(larger.size_class)) + ", " +
                    std::to_string(larger.scale) + "x on the " + size + " window"
            );
            settings::Notice sized = shown_warning().notice();
            sized.size_class = larger.size_class;
            const auto* fonts = engine_settings_fonts();
            const int32_t sized_height = settings::notice_height(sized, fonts);
            const auto at = warning_place();
            const int32_t shown_width =
                oa::ui::kit::metrics_of(larger.size_class).notice_width * larger.scale;
            require(
                at.shown.width == shown_width && at.shown.height == sized_height * larger.scale &&
                    at.shown.x == (larger.width - shown_width) / 2 &&
                    at.shown.y == (larger.height - at.shown.height) / 2,
                "the warning is not centred in the " + size + " window at its class's size"
            );
            renderer::Surface drawing;
            drawing.width = static_cast<uint32_t>(at.shown.width);
            drawing.height = static_cast<uint32_t>(at.shown.height);
            drawing.rgb.assign(static_cast<std::size_t>(drawing.width) * drawing.height * 3U, 0);
            settings::draw_notice(
                drawing, {0, 0, larger.scale}, sized, *fonts, engine_settings_icon()
            );
            apply_gamma_rgb(drawing.rgb.data(), drawing.rgb.size() / 3U, 3);
            auto expected = expected_layer_frame(closed_windows[index], drawing, at);
            // The closed frame holds the larger icons. Darkening that frame
            // is not what the window draws: each icon is darkened and blended
            // on its own, over its window rectangle. Those squares are taken
            // from the window. The warning covers its own place.
            EngineSettingsMenuHost::copy_presented_menu_buttons(
                *this,
                presented,
                expected,
                {at.shown.x, at.shown.y, at.shown.width, at.shown.height}
            );
            const auto differing = layer_differences(
                presented, expected, window_pointer(oa_layer().view(), kRestingPointer)
            );
            require(
                differing == 0,
                "the " + size + " window does not show the warning over the darkened main menu: " +
                    std::to_string(differing) + " pixels differ"
            );
            std::cout << "mod warning check: the warning at " << size_class_name(larger.size_class)
                      << ", " << larger.scale << "x, on the " << size << " window\n";
        }
        oa_layer().read_whole_window(false);
        window_of(run_width, run_height);
        // A frame lays the warning out for the window again.
        menu_sparks_ = sparks;
        static_cast<void>(frame_without_cursor());
        snapshot("main-menu");
        // OPEN MOD FOLDER, where the OA layer shows it, shows the mod's
        // folder and the warning stays.
        const auto* fonts = engine_settings_fonts();
        const auto placed = warning_place();
        oa::ui::display_layout::Point open_pixel{};
        for (const auto& part : settings::notice_layout(notice, fonts))
            if (part.control == settings::notice_open_control)
                open_pixel = layer_window_pixel(
                    placed, {part.rect.x + part.rect.width / 2, part.rect.y + part.rect.height / 2}
                );
        state.opened.clear();
        for (const SDL_EventType type :
             {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event = window_pointer_event(
                sdl_.window,
                sdl_.renderer,
                type,
                open_pixel,
                type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT
            );
            bool running = true;
            dispatch_event(event, running);
            require(running, "a click on the warning ended the run");
        }
        require(
            saves_notice_shown() && state.opened == std::vector<fs::path>{path_from_utf8(hollow)},
            "OPEN MOD FOLDER did not show the mod's folder, or closed the warning"
        );
        // Escape closes it, and its release never reaches the main menu.
        close_with(SDLK_ESCAPE, Screen::main_menu, "the main menu");
        // Once from each start: it shows no more over the main menu.
        for (int pass = 0; pass < 4; ++pass)
            tell_incomplete_mod();
        require(!saves_notice_shown() && state.mod_warnings_shown == 1, "the warning showed again");
        std::cout << "mod warning check: the warning showed once over the main menu, showed the "
                     "mod's folder and closed on Escape\n";

        // Skirmish's Start shows it again and the setup stays.
        exercise_click(menu::resource_name(menu::Button::single_player));
        exercise_click(entry::resource_name(entry::Button::skirmish));
        require(screen_ == Screen::skirmish, "Skirmish did not open its setup");
        exercise_click(skirmish::resource_name(skirmish::Button::start));
        require(
            screen_ == Screen::skirmish && !match_ && saves_notice_shown() &&
                shown_warning().over() == Screen::skirmish && state.mod_warnings_shown == 2,
            "Start did not warn and stay on the skirmish setup"
        );
        snapshot("skirmish");
        close_with(SDLK_RETURN, Screen::skirmish, "the skirmish setup");
        // A start past the warning fails without ending the run: the
        // missing commander stops it, and the setup comes back, warning again.
        const auto* start = widget(skirmish::resource_name(skirmish::Button::start));
        require(start != nullptr, "the skirmish setup has no Start");
        const auto origin = panel_origin();
        const float x = static_cast<float>(origin.x + start->common.x) +
                        static_cast<float>(start->common.width) / 2.0F;
        const float y = static_cast<float>(origin.y + start->common.y) +
                        static_cast<float>(start->common.height) / 2.0F;
        update_pointer(x, y);
        selected_ = hovered_ ? static_cast<int32_t>(*hovered_) : -1;
        update_pointer(x, y);
        require(
            !start_skirmish_from_setup(entry::Event{{kFrontendMenuHandle}, 0}) && !match_ &&
                screen_ == Screen::skirmish && saves_notice_shown() &&
                state.mod_warnings_shown == 3,
            "a failed start did not come back to the skirmish setup with the warning"
        );
        selected_ = -1;
        tap_under_ok(Screen::skirmish, "the skirmish setup");
        std::cout << "mod warning check: Skirmish's Start warned and stayed on the setup; a start "
                     "past the warning came back to it, and a finger just under OK closed it\n";
        fake_frontend_tick_ = previous_tick;
        switch_to(dropped);
        return;
    }

    if (options_.restarts == 4) {
        // A commander whose file is there but whose unit the catalog drops
        // is missing all the same; the other side's, which it keeps, is not.
        const auto* profile = mod_profile();
        require(
            settings_state.playing_mod_folder == dropped && profile != nullptr &&
                profile->id == kDroppedFolder,
            "the switch to the dropped profile did not play it"
        );
        const ModStartGaps gaps = mod_start_gaps();
        require(
            !gaps.no_units && gaps.missing_commanders.size() == 1 &&
                gaps.missing_commanders.front().commander == commander,
            "the dropped commander, and it alone, is not found missing"
        );
        // The second side's panels are missing, and nothing else a side names.
        require(
            gaps.missing_side_files.size() == 1 && gaps.missing_side_files.front().side == second &&
                gaps.missing_side_files.front().path == panels,
            "the second side's missing panels, and they alone, are not found missing"
        );
        const auto previous_tick = fake_frontend_tick_;
        fake_frontend_tick_ = 1000U;
        load(Screen::main_menu);
        send_check_pointer(SDL_EVENT_MOUSE_MOTION, kRestingPointer, 0);
        for (int pass = 0; pass < 4; ++pass)
            tell_incomplete_mod();
        require(
            saves_notice_shown() && state.mod_warnings_shown == 1 &&
                shown_warning().notice().paragraphs.size() > 2 &&
                shown_warning().notice().paragraphs[1].text == commander_line() &&
                shown_warning().notice().paragraphs[2].text == side_file_line(panels) &&
                shown_warning().notice().paragraphs[3].text ==
                    "Its games can't start until the mod's files are added to its folder:",
            "the warning of the dropped commander and the missing panels did not show over the "
            "main menu"
        );
        close_with(SDLK_RETURN, Screen::main_menu, "the main menu");
        std::cout << "mod warning check: a commander whose unit the catalog drops and a side's "
                     "missing panels warned as missing, and nothing else\n";
        fake_frontend_tick_ = previous_tick;
        switch_to(whole);
        return;
    }

    // The profile that plays whole warns of nothing, and its skirmish starts.
    const auto* profile = mod_profile();
    require(
        settings_state.playing_mod_folder == whole && profile != nullptr &&
            profile->id == kWholeFolder,
        "the switch to the whole profile did not play it"
    );
    require(!mod_start_gaps().any(), "the whole profile has gaps");
    load(Screen::main_menu);
    for (int pass = 0; pass < 4; ++pass)
        tell_incomplete_mod();
    require(!saves_notice_shown() && state.mod_warnings_shown == 0, "the whole profile warned");
    exercise_click(menu::resource_name(menu::Button::single_player));
    exercise_click(entry::resource_name(entry::Button::skirmish));
    exercise_click(skirmish::resource_name(skirmish::Button::start));
    require(screen_ == Screen::match && match_, "the whole profile's skirmish did not start");
    require(!saves_notice_shown(), "the whole profile's skirmish warned");
    leave_match();
    // The next start of the check plays No Mod again.
    preference_values_.erase(std::string(settings::key::mod_directory));
    preferences_dirty_ = true;
    std::cout << "mod warning check: the whole profile warned of nothing and started its "
                 "skirmish\n";
}

} // namespace oa::app
