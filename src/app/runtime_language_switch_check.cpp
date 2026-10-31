// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-language-switch: the language chosen in the settings over the main
// menu, and over a skirmish's in-game menu, from the language the run starts
// in, Simplified Chinese or the installed pack's language --check-language
// names, to English, German and back. After each change, with the settings
// closed by OK, the screen under them shows what it shows opened again in the
// language, and back in the first language what it showed as the run started
// in it.

#include "language_state.hpp"

#include "oa/app/runtime.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/hud/resource_bar.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace oa::app {

namespace {

namespace settings = oa::ui::engine_settings;

/// The language the run starts in unless --check-language names another;
/// the check's preferences file chooses it.
constexpr std::string_view kStartTag = "zh-Hans";
/// The languages the settings choose before going back to the one the run
/// starts in: the first two of these that are not that one.
constexpr std::array<std::string_view, 3> kOtherTags{"en", "de", "fr"};
/// The most steps the bars' numbers take to come to rest.
constexpr int kMostReadoutSteps = 10000;
/// A point of the main menu's picture over none of its buttons, where the
/// pointer rests.
constexpr oa::ui::display_layout::Point kRestingPointer{4, 240};

/// Stops the check with a reason.
///
/// @param what what went wrong
[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("language switch check: " + what);
}

/// Counts the pixels in which two frames differ.
///
/// @param first one frame
/// @param second the other frame
/// @return the differing pixels; every pixel and one more when the sizes differ
std::size_t differing_pixels(const renderer::Surface& first, const renderer::Surface& second) {
    if (first.width != second.width || first.height != second.height ||
        first.rgb.size() != second.rgb.size())
        return static_cast<std::size_t>(first.width) * first.height + 1U;
    std::size_t differing = 0;
    for (std::size_t at = 0; at + 2 < first.rgb.size(); at += 3)
        if (first.rgb[at] != second.rgb[at] || first.rgb[at + 1] != second.rgb[at + 1] ||
            first.rgb[at + 2] != second.rgb[at + 2])
            ++differing;
    return differing;
}

/// Returns the path of one step's frame: <stem>-<step>.ppm beside --snapshot.
///
/// @param snapshot the --snapshot path
/// @param step the step's name
/// @return the step's path
fs::path step_snapshot(const fs::path& snapshot, std::string_view step) {
    fs::path file = snapshot.parent_path() / snapshot.stem();
    file += '-' + std::string(step) + ".ppm";
    return file;
}

} // namespace

void Runtime::check_language_switch() {
    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        fail("needs the SDL presenter");
    // The check saves settings, so it never runs over the player's own file.
    if (!options_.preferences_file)
        fail("needs --preferences-file");
    const std::string start_tag = options_.check_languages.empty()
                                      ? std::string(kStartTag)
                                      : options_.check_languages.front();
    // The other languages chosen in turn, then the first again.
    std::vector<std::string> chosen_tags;
    for (const std::string_view other : kOtherTags)
        if (other != start_tag && chosen_tags.size() < 2)
            chosen_tags.emplace_back(other);
    chosen_tags.push_back(start_tag);
    if (shown_language().tag != start_tag)
        fail(
            "the run shows " + std::string(shown_language().tag) + ", not " + start_tag +
            ": its preferences file chooses the language"
        );
    if (language_pack_folders().empty())
        fail("no installed language pack holds " + start_tag);
    // The first language's text is drawn in the faces its pack brings, as
    // Simplified Chinese's own: the stack holds each of them.
    const std::size_t pack_faces = language_ != nullptr ? language_->fonts.size() : 0;
    if (modern_font_pack_faces() != pack_faces)
        fail(
            "the modern font stack holds " + std::to_string(modern_font_pack_faces()) +
            " of the pack's faces, not " + std::to_string(pack_faces)
        );

    // However the check ends, its preferences file chooses the language the
    // run started in again, for the next run.
    struct StartLanguageKept {
        Runtime& runtime;
        std::string start_tag;

        ~StartLanguageKept() {
            namespace preferences = oa::platform::preferences;
            try {
                auto values = preferences::load(runtime.preference_path_);
                values[std::string(settings::key::language)] = start_tag;
                preferences::save(runtime.preference_path_, values);
            } catch (const std::exception& error) {
                std::cerr << "language switch check: the preferences file keeps the last "
                             "language chosen: "
                          << error.what() << '\n';
            }
        }
    } start_language_kept{*this, start_tag};

    // The extensions' overlays are set aside while the check holds the main
    // menu to the engine's own drawing, and put back when it ends.
    set_extension_overlays_aside(true);

    struct PutBack {
        Runtime& runtime;

        ~PutBack() { runtime.set_extension_overlays_aside(false); }
    } put_back{*this};

    const auto snapshot = [this](std::string_view step, const renderer::Surface& frame) {
        if (!options_.snapshot.empty())
            write_ppm(step_snapshot(options_.snapshot, step), frame);
    };
    // Chooses a language in the open settings, as a click on its list does,
    // and closes them with OK.
    const auto choose = [this, &start_tag, pack_faces](std::string_view tag, auto&& close) {
        auto* dialog = engine_settings_dialog();
        if (dialog == nullptr)
            fail("the settings did not open");
        dialog->chosen.language = std::string(tag);
        std::ignore = take_engine_settings_action(settings::DialogAction::changed);
        close(settings::DialogAction::accepted);
        if (engine_settings_dialog() != nullptr)
            fail("OK did not close the settings");
        if (shown_language().tag != tag)
            fail(
                "the settings chose " + std::string(tag) + ", but the run shows " +
                std::string(shown_language().tag)
            );
        if (tag == start_tag && modern_font_pack_faces() != pack_faces)
            fail("the modern font stack does not hold the pack's faces");
    };
    // Requires a frame shown after a change to be the one the screen shows
    // opened again, and to differ from the frame of the language before.
    const auto expect_shown = [](std::string_view screen,
                                 std::string_view tag,
                                 const renderer::Surface& switched,
                                 const renderer::Surface& reopened,
                                 const renderer::Surface& before) {
        if (const auto differing = differing_pixels(switched, reopened); differing != 0)
            fail(
                "after the change to " + std::string(tag) + " " + std::string(screen) + " shows " +
                std::to_string(differing) + " pixels other than it shows opened again in it"
            );
        if (differing_pixels(switched, before) == 0)
            fail(std::string(screen) + " looks the same in " + std::string(tag) + " as before it");
    };
    // Requires every caption the pack draws over the match's pictures, the
    // top bar's metal and energy among them, to show each of its
    // characters: the captions' fonts hold the pack's face, and none of its
    // words draws the missing-glyph box.
    const auto expect_captions_whole = [this, &start_tag](std::string_view when) {
        // The Chinese fixture pack gives captions; another pack may give none.
        if (language_pictures().all().empty()) {
            if (start_tag == kStartTag)
                fail("no installed pack gives " + start_tag + " captions over pictures");
            return;
        }
        const auto missing = picture_captions_missing_glyphs();
        if (!missing.empty())
            fail(
                std::string(when) + " the caption " + missing.front() +
                " draws the missing-glyph box, as do " + std::to_string(missing.size() - 1U) +
                " more"
            );
    };

    // The main menu: each frame drawn from sparks started afresh, without
    // the pointer.
    const auto menu_frame = [this] {
        menu_sparks_ = {};
        return frame_without_cursor();
    };
    load(Screen::main_menu);
    if (screen_ != Screen::main_menu)
        fail("the main menu did not open");
    send_check_pointer(SDL_EVENT_MOUSE_MOTION, kRestingPointer, 0);
    const auto menu_started = menu_frame();
    snapshot("menu-" + start_tag + "-start", menu_started);
    auto menu_before = menu_started;
    for (const std::string& tag : chosen_tags) {
        open_engine_settings_from_menu();
        choose(tag, [this](settings::DialogAction action) { take_settings_screen_action(action); });
        const auto switched = menu_frame();
        snapshot("menu-" + std::string(tag), switched);
        load(Screen::main_menu);
        const auto reopened = menu_frame();
        snapshot("menu-" + std::string(tag) + "-opened-again", reopened);
        expect_shown("the main menu", tag, switched, reopened, menu_before);
        menu_before = switched;
    }
    if (const auto differing = differing_pixels(menu_before, menu_started); differing != 0)
        fail(
            "back in " + start_tag + " the main menu shows " + std::to_string(differing) +
            " pixels other than it showed as the run started"
        );
    std::cout << "language switch check: the main menu shows each language as it opens in it\n";

    // A skirmish's in-game menu, the pointer at the window's far corner.
    start_benchmark_skirmish();
    apply_output_mode();
    show_match_pause_menu();
    if (!ingame_menu_column_shown())
        fail("the in-game menu did not open");
    const auto match_frame = [this] {
        send_check_pointer(
            SDL_EVENT_MOUSE_MOTION, {match_layout_.width - 1, match_layout_.height - 1}, 0
        );
        // The bars' numbers ease toward the stores a step each frame drawn:
        // they are brought to rest first, so that frames differ only where
        // the language does.
        auto& game = match_->state().game;
        auto& readout = game.resource_readout;
        for (int step = 0; step < kMostReadoutSteps; ++step) {
            const float metal = readout.metal;
            const float energy = readout.energy;
            oa::ui::hud::update_resource_readout(
                readout, game.players[match_view_player()], game.tick
            );
            if (readout.metal == metal && readout.energy == energy)
                break;
        }
        render_match_surface();
        renderer::Surface frame;
        compose_match_frame(frame);
        return frame;
    };
    const auto match_started = match_frame();
    snapshot("match-" + start_tag + "-start", match_started);
    expect_captions_whole("as the skirmish starts,");
    auto match_before = match_started;
    for (const std::string& tag : chosen_tags) {
        open_engine_settings_in_match();
        choose(tag, [this](settings::DialogAction action) { take_settings_screen_action(action); });
        if (!ingame_menu_column_shown())
            fail("the in-game menu closed with the settings");
        const auto switched = match_frame();
        snapshot("match-" + std::string(tag), switched);
        // The game resumed, and the in-game menu opened again over it with
        // the pictures and panels kept for later loaded afresh, as a run
        // started in the language shows it.
        match_titles_ = {};
        talk_layout_.reset();
        match_talk_ = {};
        resume_match_pause();
        show_match_pause_menu();
        const auto reopened = match_frame();
        snapshot("match-" + std::string(tag) + "-opened-again", reopened);
        expect_shown("the in-game menu", tag, switched, reopened, match_before);
        if (tag == start_tag)
            expect_captions_whole("back in " + start_tag + ",");
        match_before = switched;
    }
    if (const auto differing = differing_pixels(match_before, match_started); differing != 0)
        fail(
            "back in " + start_tag + " the in-game menu shows " + std::to_string(differing) +
            " pixels other than it showed as the skirmish started"
        );
    std::cout << "language switch check: the in-game menu shows each language as it opens in "
                 "it\n";
    std::cout << "language switch check: every caption over the match's pictures shows each of "
                 "its characters in "
              << start_tag << '\n';
}

} // namespace oa::app
