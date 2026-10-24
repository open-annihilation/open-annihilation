// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The settings' defaults on each platform and preferences file, what they
// read from the preferences and an installation's totala.ini, what they
// write back, and the locks a game, the command line and the renderer put
// on them. The Language switches and text size: their defaults, a
// file without them, a file with CR LF line ends, the round trip, the text
// size's range and the text style the drawing reads. Developer Mode's
// switch and the overrides kept under each profile's id. The Touch
// section's settings: their keys, defaults, words, the hold delay's range
// and stops, unknown values, and the round trip. The Game files section's
// backups switch: Off by default everywhere, read, written and restored as
// every switch. A Steam Deck's defaults: its screen's frame rate and Control
// size Larger, a stored value winning and Restore defaults bringing them
// back. Control size and the Controller section's settings: their keys,
// defaults, words, the speeds' ranges and stops, unknown values, and the
// round trip. Interface size: its key, its default Auto, its five words,
// the words it refuses and its round trip, which leaves other keys alone.

#include "oa/ui/engine_settings.hpp"

#include "oa/data/languages.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace {

int failures = 0;

void check(bool condition, const char* expression, const char* file, int line) {
    if (condition)
        return;
    std::cerr << file << ':' << line << ": check failed: " << expression << '\n';
    ++failures;
}

#define CHECK(expression) check((expression), #expression, __FILE__, __LINE__)

namespace settings = oa::ui::engine_settings;
using oa::platform::preferences::Values;
using settings::HardwareAcceleration;

/// The platform and file the tests run the player's own preferences on.
constexpr settings::Inputs players_own_on_linux{true, false, {}};

/// Returns a preferences map holding one key.
///
/// @param key the key
/// @param value its value
/// @return the map
Values one_key(std::string_view key, std::string value) {
    Values values;
    values[std::string{key}] = std::move(value);
    return values;
}

/// Returns the settings read from one key's value, with the defaults of an
/// explicit preferences file.
///
/// @param key the key
/// @param value its value
/// @return the settings
settings::EngineSettings read_one(std::string_view key, std::string value) {
    return settings::read_settings(one_key(key, std::move(value)), {}, false);
}

void defaults_play_as_without_the_settings() {
    const auto defaults = settings::default_settings({});
    CHECK(defaults.path_search_nodes == settings::base_path_search_nodes);
    CHECK(defaults.wheel_zoom);
    CHECK(defaults.middle_click_pan);
    CHECK(!defaults.escape_opens_menu);
    CHECK(!defaults.switch_alt);
    CHECK(defaults.unit_limit == settings::default_unit_limit);
    CHECK(defaults.max_frame_rate == settings::highest_frame_rate);
    CHECK(defaults.anti_aliasing == settings::AntiAliasing::off);
    CHECK(!defaults.frame_stats);
    CHECK(defaults.hardware_acceleration == HardwareAcceleration::off);
    CHECK(!defaults.vertical_sync);
    CHECK(defaults == settings::EngineSettings{});
}

void hardware_acceleration_defaults_to_full_for_the_players_own_file_on_every_machine() {
    // The player's own file: Full on every platform and machine, a light
    // machine and a Raspberry Pi included; Vertical sync Off everywhere.
    const std::array<settings::Inputs, 5> own{{
        players_own_on_linux,
        {true, true, {}},
        {true, false, {}, true},
        {true, false, {}, false, true, {1024, 768}},
        {true, false, {}, true, true, {640, 480}},
    }};
    for (const auto& inputs : own) {
        const auto defaults = settings::default_settings(inputs);
        CHECK(defaults.hardware_acceleration == HardwareAcceleration::full);
        CHECK(!defaults.vertical_sync);
        CHECK(settings::read_settings({}, inputs, false) == defaults);
    }
    // A named file: Off, as the game plays without the setting, on every machine.
    for (auto inputs : own) {
        inputs.players_own_profile = false;
        const auto defaults = settings::default_settings(inputs);
        CHECK(defaults.hardware_acceleration == HardwareAcceleration::off);
        CHECK(!defaults.vertical_sync);
    }
}

void hardware_acceleration_reads_its_words_and_the_switchs_numbers() {
    // The words name the levels, and the levels' order is the dialog's.
    CHECK(settings::hardware_acceleration_text(HardwareAcceleration::off) == "off");
    CHECK(settings::hardware_acceleration_text(HardwareAcceleration::basic) == "basic");
    CHECK(settings::hardware_acceleration_text(HardwareAcceleration::full) == "full");
    for (const auto level : settings::hardware_acceleration_levels)
        CHECK(
            settings::hardware_acceleration_from_text(
                settings::hardware_acceleration_text(level)
            ) == level
        );
    CHECK(settings::hardware_acceleration_levels[0] == HardwareAcceleration::off);
    CHECK(settings::hardware_acceleration_levels[1] == HardwareAcceleration::basic);
    CHECK(settings::hardware_acceleration_levels[2] == HardwareAcceleration::full);
    for (const char* text : {"", "Off", "BASIC", "Full", "on", " full", "full ", "1"})
        CHECK(!settings::hardware_acceleration_from_text(text));
    for (const auto& inputs : {settings::Inputs{}, players_own_on_linux}) {
        const auto read = [&](std::string_view key, const char* text) {
            return settings::read_settings(one_key(key, text), inputs, false);
        };
        const auto level = [&](const char* text) {
            return read(settings::key::hardware_acceleration, text).hardware_acceleration;
        };
        CHECK(level("off") == HardwareAcceleration::off);
        CHECK(level("basic") == HardwareAcceleration::basic);
        CHECK(level("full") == HardwareAcceleration::full);
        // A file the On and Off switch wrote keeps working: 0 is Off and any
        // number above it Full, on either file.
        CHECK(level("0") == HardwareAcceleration::off);
        CHECK(level("1") == HardwareAcceleration::full);
        CHECK(level("7") == HardwareAcceleration::full);
        CHECK(level("-1") == HardwareAcceleration::off);
        CHECK(!read(settings::key::vertical_sync, "0").vertical_sync);
        CHECK(read(settings::key::vertical_sync, "1").vertical_sync);
        // Any other text gives the default: another word or letter case, or
        // what is neither a word nor a whole number. Vertical sync reads
        // only numbers.
        const auto defaults = settings::default_settings(inputs);
        for (const char* text :
             {"Off",
              "BASIC",
              "Full",
              "false",
              "no",
              "on",
              "",
              " 0",
              "0 ",
              "1.0",
              " full",
              "full "}) {
            CHECK(read(settings::key::hardware_acceleration, text) == defaults);
            CHECK(read(settings::key::vertical_sync, text) == defaults);
        }
        CHECK(read(settings::key::vertical_sync, "off") == defaults);
    }
    // Written as its word, and Vertical sync as 1 or 0, only when changed;
    // Restore defaults erases each at its default, the player's own file's
    // Full included.
    const auto own = settings::default_settings(players_own_on_linux);
    auto off = own;
    off.hardware_acceleration = HardwareAcceleration::off;
    off.vertical_sync = true;
    Values values;
    settings::write_settings(values, own, off, own, false);
    CHECK(values.size() == 2);
    CHECK(values.at(std::string{settings::key::hardware_acceleration}) == "off");
    CHECK(values.at(std::string{settings::key::vertical_sync}) == "1");
    CHECK(settings::read_settings(values, players_own_on_linux, false) == off);
    settings::write_settings(values, off, own, own, true);
    CHECK(values.empty());
    // Back to Full by hand, on the player's own file, writes full over the
    // switch's 0; Basic writes basic.
    values = one_key(settings::key::hardware_acceleration, "0");
    settings::write_settings(values, off, own, own, false);
    CHECK(values.at(std::string{settings::key::hardware_acceleration}) == "full");
    auto basic = own;
    basic.hardware_acceleration = HardwareAcceleration::basic;
    settings::write_settings(values, own, basic, own, false);
    CHECK(values.at(std::string{settings::key::hardware_acceleration}) == "basic");
    CHECK(settings::read_settings(values, players_own_on_linux, false) == basic);
    // A file that still holds the switch's 1 reads as the player's own
    // default, so Restore defaults then OK erases it.
    values = one_key(settings::key::hardware_acceleration, "1");
    CHECK(settings::read_settings(values, players_own_on_linux, false) == own);
    settings::write_settings(values, own, own, own, true);
    CHECK(values.empty());
}

void escape_opens_the_menu_by_default_only_on_macos_with_the_players_own_file() {
    CHECK(settings::default_settings({true, true, {}}).escape_opens_menu);
    CHECK(!settings::default_settings({false, true, {}}).escape_opens_menu);
    CHECK(!settings::default_settings(players_own_on_linux).escape_opens_menu);
    CHECK(!settings::default_settings({}).escape_opens_menu);
}

void the_unit_limit_defaults_to_the_installations_with_the_players_own_file() {
    constexpr std::string_view ini = "[Preferences]\r\nUnitLimit=500\r\n";
    CHECK(settings::default_settings({true, false, ini}).unit_limit == 500);
    CHECK(settings::default_settings({true, true, ini}).unit_limit == 500);
    // A named preferences file never reads the installation.
    CHECK(settings::default_settings({false, false, ini}).unit_limit == 250);
    CHECK(settings::default_settings({true, false, "[Preferences]\n"}).unit_limit == 250);
    CHECK(settings::default_settings({true, false, ""}).unit_limit == 250);
    // The installation's value is a default: a stored key wins over it.
    const auto stored = settings::read_settings(
        one_key(settings::key::unit_limit, "300"), {true, false, ini}, false
    );
    CHECK(stored.unit_limit == 300);
    const auto absent = settings::read_settings({}, {true, false, ini}, false);
    CHECK(absent.unit_limit == 500);
}

void a_raspberry_pi_starts_at_60_frames_without_anti_aliasing() {
    constexpr settings::Inputs pi{true, false, {}, true};
    const auto defaults = settings::default_settings(pi);
    CHECK(defaults.max_frame_rate == settings::raspberry_pi_frame_rate);
    CHECK(defaults.max_frame_rate == 60);
    CHECK(defaults.anti_aliasing == settings::AntiAliasing::off);
    // Every other default is as on any other machine.
    auto others = defaults;
    others.max_frame_rate = settings::highest_frame_rate;
    CHECK(others == settings::default_settings(players_own_on_linux));
    // A named preferences file plays as the game does everywhere.
    CHECK(settings::default_settings({false, false, {}, true}) == settings::default_settings({}));
    CHECK(settings::default_settings(players_own_on_linux).max_frame_rate == 120);

    // Without a stored value the Pi's defaults hold; a stored value wins.
    CHECK(settings::read_settings({}, pi, false) == defaults);
    const auto faster =
        settings::read_settings(one_key(settings::key::max_frame_rate, "120"), pi, false);
    CHECK(faster.max_frame_rate == 120);
    const auto smoother =
        settings::read_settings(one_key(settings::key::anti_aliasing, "4"), pi, false);
    CHECK(smoother.anti_aliasing == settings::AntiAliasing::x4);
    CHECK(smoother.max_frame_rate == 60);

    // The player's choice is stored; Restore defaults puts the Pi's back.
    auto chosen = defaults;
    chosen.max_frame_rate = 90;
    chosen.anti_aliasing = settings::AntiAliasing::x2;
    Values values;
    settings::write_settings(values, defaults, chosen, defaults, false);
    CHECK(values.at(std::string{settings::key::max_frame_rate}) == "90");
    CHECK(values.at(std::string{settings::key::anti_aliasing}) == "2");
    CHECK(settings::read_settings(values, pi, false) == chosen);
    settings::write_settings(values, chosen, defaults, defaults, true);
    CHECK(values.empty());
    CHECK(settings::read_settings(values, pi, false) == defaults);
}

void a_light_machine_starts_at_800x600_and_60_frames_without_anti_aliasing() {
    settings::Inputs light{true, false, {}, false, true, {1024, 768}};
    const auto defaults = settings::default_settings(light);
    CHECK(defaults.max_frame_rate == settings::light_machine_frame_rate);
    CHECK(defaults.max_frame_rate == 60);
    CHECK(defaults.anti_aliasing == settings::AntiAliasing::off);
    CHECK(defaults.screen_size == settings::light_machine_screen_size);
    CHECK((defaults.screen_size == settings::ScreenSize{800, 600}));
    // Every other default is as on any other machine.
    auto others = defaults;
    others.max_frame_rate = settings::highest_frame_rate;
    others.screen_size = settings::desktop_screen_size;
    CHECK(others == settings::default_settings(players_own_on_linux));
    // A desktop narrower or shorter than 800x600 starts at 640x480; an
    // unknown one at 800x600.
    light.desktop = {640, 480};
    CHECK(settings::default_settings(light).screen_size == settings::small_desktop_screen_size);
    light.desktop = {800, 480};
    CHECK((settings::default_settings(light).screen_size == settings::ScreenSize{640, 480}));
    light.desktop = {720, 600};
    CHECK((settings::default_settings(light).screen_size == settings::ScreenSize{640, 480}));
    light.desktop = {800, 600};
    CHECK((settings::default_settings(light).screen_size == settings::ScreenSize{800, 600}));
    light.desktop = {};
    CHECK((settings::default_settings(light).screen_size == settings::ScreenSize{800, 600}));
    // A named preferences file plays as the game does everywhere, and a
    // machine that is not light starts at the desktop's size.
    CHECK(
        settings::default_settings({false, false, {}, false, true, {1024, 768}}) ==
        settings::default_settings({})
    );
    CHECK(
        settings::default_settings({true, false, {}, false, false, {1024, 768}}).screen_size ==
        settings::desktop_screen_size
    );

    // Without a stored value the light defaults hold; a stored value wins.
    light.desktop = {1024, 768};
    CHECK(settings::read_settings({}, light, false) == defaults);
    const auto desktop =
        settings::read_settings(one_key(settings::key::screen_size, "desktop"), light, false);
    CHECK(desktop.screen_size == settings::desktop_screen_size);
    CHECK(desktop.max_frame_rate == 60);
    const auto larger =
        settings::read_settings(one_key(settings::key::screen_size, "1024x768"), light, false);
    CHECK((larger.screen_size == settings::ScreenSize{1024, 768}));

    // The player's choice is stored; Restore defaults puts the light machine's back.
    auto chosen = defaults;
    chosen.max_frame_rate = 120;
    chosen.screen_size = settings::desktop_screen_size;
    Values values;
    settings::write_settings(values, defaults, chosen, defaults, false);
    CHECK(values.at(std::string{settings::key::max_frame_rate}) == "120");
    CHECK(values.at(std::string{settings::key::screen_size}) == "desktop");
    CHECK(settings::read_settings(values, light, false) == chosen);
    settings::write_settings(values, chosen, defaults, defaults, true);
    CHECK(values.empty());
    CHECK(settings::read_settings(values, light, false) == defaults);
}

void screen_sizes_are_stored_as_text() {
    CHECK(settings::screen_size_text(settings::desktop_screen_size) == "desktop");
    CHECK(settings::screen_size_text({640, 480}) == "640x480");
    CHECK(settings::screen_size_text({1280, 1024}) == "1280x1024");
    for (const auto size : settings::screen_sizes)
        CHECK(settings::screen_size_from_text(settings::screen_size_text(size)) == size);
    // Any size a display may offer, from the game's own screen up, is read
    // as written: a 4K monitor's and an ultrawide's among them.
    CHECK((settings::screen_size_from_text("1920x1080") == settings::ScreenSize{1920, 1080}));
    CHECK((settings::screen_size_from_text("3840x2160") == settings::ScreenSize{3840, 2160}));
    CHECK((settings::screen_size_from_text("3440x1440") == settings::ScreenSize{3440, 1440}));
    CHECK((settings::screen_size_from_text("8192x8192") == settings::ScreenSize{8192, 8192}));
    CHECK(
        (read_one(settings::key::screen_size, "2560x1440").screen_size ==
         settings::ScreenSize{2560, 1440})
    );
    // Text written otherwise, or a size below the game's screen or beyond
    // the longest side, is not.
    CHECK(!settings::screen_size_from_text(""));
    CHECK(!settings::screen_size_from_text("Desktop"));
    CHECK(!settings::screen_size_from_text("800X600"));
    CHECK(!settings::screen_size_from_text("800x600 "));
    CHECK(!settings::screen_size_from_text(" 800x600"));
    CHECK(!settings::screen_size_from_text("0x0"));
    CHECK(!settings::screen_size_from_text("0800x600"));
    CHECK(!settings::screen_size_from_text("+800x600"));
    CHECK(!settings::screen_size_from_text("800x"));
    CHECK(!settings::screen_size_from_text("x600"));
    CHECK(!settings::screen_size_from_text("800x600x2"));
    CHECK(!settings::screen_size_from_text("639x480"));
    CHECK(!settings::screen_size_from_text("640x479"));
    CHECK(!settings::screen_size_from_text("8193x4320"));
    CHECK(!settings::screen_size_from_text("99999x480"));
    CHECK(
        read_one(settings::key::screen_size, "320x200").screen_size == settings::desktop_screen_size
    );
    CHECK(
        (read_one(settings::key::screen_size, "640x480").screen_size ==
         settings::ScreenSize{640, 480})
    );
}

void totala_ini_gives_its_unit_limit_as_the_game_reads_it() {
    using settings::installation_unit_limit;
    CHECK(!installation_unit_limit(""));
    CHECK(!installation_unit_limit("UnitLimit=400\n"));
    CHECK(!installation_unit_limit("[Preferences]\nOther=1\n"));
    CHECK(!installation_unit_limit("[Other]\nUnitLimit=400\n"));
    CHECK(installation_unit_limit("[Preferences]\nUnitLimit=400\n") == 400);
    CHECK(installation_unit_limit("[Preferences]\nUnitLimit=400") == 400);
    CHECK(installation_unit_limit("[preferences]\r\nunitlimit = 400\r\n") == 400);
    CHECK(installation_unit_limit("[ PREFERENCES ]\r\n  UNITLIMIT\t=\t333 \r\n") == 333);
    CHECK(installation_unit_limit("[Other]\nUnitLimit=99\n[Preferences]\nUnitLimit=400\n") == 400);
    // Values: the leading whole number, then 3.1c's own clamp.
    CHECK(installation_unit_limit("[Preferences]\nUnitLimit=abc\n") == 20);
    CHECK(installation_unit_limit("[Preferences]\nUnitLimit=\n") == 20);
    CHECK(installation_unit_limit("[Preferences]\nUnitLimit=3\n") == 20);
    CHECK(installation_unit_limit("[Preferences]\nUnitLimit=-40\n") == 20);
    CHECK(installation_unit_limit("[Preferences]\nUnitLimit=9999\n") == 500);
    CHECK(installation_unit_limit("[Preferences]\nUnitLimit=99999999999999999999\n") == 500);
    CHECK(installation_unit_limit("[Preferences]\nUnitLimit=300 ; more\n") == 300);
    CHECK(installation_unit_limit("[Preferences]\nUnitLimit=300units\n") == 300);
    // Comments, the first line of the key and the first section count.
    CHECK(installation_unit_limit("[Preferences]\n;UnitLimit=100\nUnitLimit=400\n") == 400);
    CHECK(installation_unit_limit("[Preferences]\nUnitLimit=400\nUnitLimit=100\n") == 400);
    CHECK(
        !installation_unit_limit("[Preferences]\nA=1\n[Other]\nB=2\n[Preferences]\nUnitLimit=400\n")
    );
    // Only the first installation_ini_limit bytes are read.
    const std::string head = "[Preferences]\n;";
    std::string padded =
        head + std::string(settings::installation_ini_limit - head.size() - 1, 'x');
    padded += "\nUnitLimit=400\n";
    CHECK(!installation_unit_limit(padded));
    std::string inside =
        head + std::string(settings::installation_ini_limit - head.size() - 20, 'x');
    inside += "\nUnitLimit=400\n";
    CHECK(installation_unit_limit(inside) == 400);
}

void absent_keys_and_values_that_are_not_numbers_give_the_defaults() {
    CHECK(settings::read_settings({}, {}, false) == settings::default_settings({}));
    for (const std::string_view key :
         {settings::key::path_search_nodes,
          settings::key::wheel_zoom,
          settings::key::middle_click_pan,
          settings::key::escape_opens_menu,
          settings::key::unit_limit,
          settings::key::max_frame_rate,
          settings::key::anti_aliasing,
          settings::key::frame_stats}) {
        for (const char* text : {"", "abc", "12x", " 5", "5 ", "-", "+", "0x10", "1.5"})
            CHECK(read_one(key, text) == settings::default_settings({}));
    }
    // Other keys, the game's own included, leave the settings alone.
    Values others;
    others["Total Annihilation|UnitLimit"] = "1000";
    others["open-annihilation.game-directory"] = "/games/ta";
    CHECK(settings::read_settings(others, {}, false) == settings::default_settings({}));
}

void stored_values_are_read_and_clamped_into_their_ranges() {
    CHECK(read_one(settings::key::path_search_nodes, "5332").path_search_nodes == 5332);
    CHECK(read_one(settings::key::path_search_nodes, "2000").path_search_nodes == 2000);
    CHECK(read_one(settings::key::path_search_nodes, "+2666").path_search_nodes == 2666);
    CHECK(read_one(settings::key::path_search_nodes, "1").path_search_nodes == 1333);
    CHECK(read_one(settings::key::path_search_nodes, "-9").path_search_nodes == 1333);
    CHECK(read_one(settings::key::path_search_nodes, "99999").path_search_nodes == 10664);
    CHECK(
        read_one(settings::key::path_search_nodes, "999999999999999999999999").path_search_nodes ==
        10664
    );

    CHECK(!read_one(settings::key::wheel_zoom, "0").wheel_zoom);
    CHECK(read_one(settings::key::wheel_zoom, "1").wheel_zoom);
    CHECK(!read_one(settings::key::wheel_zoom, "-1").wheel_zoom);
    CHECK(!read_one(settings::key::middle_click_pan, "0").middle_click_pan);
    CHECK(read_one(settings::key::middle_click_pan, "1").middle_click_pan);
    CHECK(read_one(settings::key::escape_opens_menu, "1").escape_opens_menu);
    CHECK(read_one(settings::key::escape_opens_menu, "7").escape_opens_menu);
    CHECK(!settings::read_settings(
               one_key(settings::key::escape_opens_menu, "0"), {true, true, {}}, false
    )
               .escape_opens_menu);
    CHECK(read_one(settings::key::frame_stats, "1").frame_stats);

    CHECK(read_one(settings::key::unit_limit, "1500").unit_limit == 1500);
    CHECK(read_one(settings::key::unit_limit, "477").unit_limit == 477);
    CHECK(read_one(settings::key::unit_limit, "30").unit_limit == 30);
    CHECK(read_one(settings::key::unit_limit, "5").unit_limit == 20);
    CHECK(read_one(settings::key::unit_limit, "4000").unit_limit == 1500);

    CHECK(read_one(settings::key::max_frame_rate, "60").max_frame_rate == 60);
    CHECK(read_one(settings::key::max_frame_rate, "63").max_frame_rate == 63);
    CHECK(read_one(settings::key::max_frame_rate, "35").max_frame_rate == 35);
    CHECK(read_one(settings::key::max_frame_rate, "30").max_frame_rate == 30);
    CHECK(read_one(settings::key::max_frame_rate, "29").max_frame_rate == 30);
    CHECK(read_one(settings::key::max_frame_rate, "10").max_frame_rate == 30);
    CHECK(read_one(settings::key::max_frame_rate, "0").max_frame_rate == 30);
    CHECK(read_one(settings::key::max_frame_rate, "500").max_frame_rate == 120);

    using settings::AntiAliasing;
    const auto level = [](const char* text) {
        return read_one(settings::key::anti_aliasing, text).anti_aliasing;
    };
    CHECK(level("-3") == AntiAliasing::off);
    CHECK(level("0") == AntiAliasing::off);
    CHECK(level("1") == AntiAliasing::off);
    CHECK(level("2") == AntiAliasing::x2);
    CHECK(level("3") == AntiAliasing::x2);
    CHECK(level("4") == AntiAliasing::x4);
    CHECK(level("5") == AntiAliasing::x4);
    CHECK(level("8") == AntiAliasing::x8);
    CHECK(level("15") == AntiAliasing::x8);
    CHECK(level("16") == AntiAliasing::x16);
    CHECK(level("100") == AntiAliasing::x16);
}

void switch_alt_comes_from_the_games_own_key() {
    CHECK(settings::read_settings({}, {}, true).switch_alt);
    CHECK(!settings::read_settings({}, {}, false).switch_alt);
    Values values;
    settings::EngineSettings chosen{};
    chosen.switch_alt = true;
    settings::write_settings(values, {}, chosen, {}, false);
    settings::write_settings(values, {}, chosen, {}, true);
    CHECK(values.empty());
}

/// Returns settings with every value away from its default.
settings::EngineSettings changed_settings() {
    settings::EngineSettings chosen{};
    chosen.path_search_nodes = 4 * settings::base_path_search_nodes;
    chosen.wheel_zoom = false;
    chosen.middle_click_pan = false;
    chosen.escape_opens_menu = true;
    chosen.unit_limit = 1000;
    chosen.max_frame_rate = 60;
    chosen.anti_aliasing = settings::AntiAliasing::x16;
    chosen.frame_stats = true;
    chosen.screen_size = {1024, 768};
    chosen.hardware_acceleration = HardwareAcceleration::basic;
    chosen.vertical_sync = true;
    chosen.modern_fonts = true;
    chosen.text_outline = false;
    chosen.text_shadow = false;
    chosen.text_background = true;
    chosen.text_size = 150;
    chosen.language = "de";
    return chosen;
}

void only_changed_settings_are_written() {
    const settings::EngineSettings defaults{};
    Values values;
    values["Total Annihilation|SwitchAlt"] = "1";
    settings::write_settings(values, defaults, defaults, defaults, false);
    CHECK(values.size() == 1);

    const auto chosen = changed_settings();
    settings::write_settings(values, defaults, chosen, defaults, false);
    CHECK(values.size() == 18);
    CHECK(values.at(std::string{settings::key::path_search_nodes}) == "5332");
    CHECK(values.at(std::string{settings::key::wheel_zoom}) == "0");
    CHECK(values.at(std::string{settings::key::middle_click_pan}) == "0");
    CHECK(values.at(std::string{settings::key::escape_opens_menu}) == "1");
    CHECK(values.at(std::string{settings::key::unit_limit}) == "1000");
    CHECK(values.at(std::string{settings::key::max_frame_rate}) == "60");
    CHECK(values.at(std::string{settings::key::anti_aliasing}) == "16");
    CHECK(values.at(std::string{settings::key::frame_stats}) == "1");
    CHECK(values.at(std::string{settings::key::screen_size}) == "1024x768");
    CHECK(values.at(std::string{settings::key::hardware_acceleration}) == "basic");
    CHECK(values.at(std::string{settings::key::vertical_sync}) == "1");
    CHECK(values.at(std::string{settings::key::modern_fonts}) == "1");
    CHECK(values.at(std::string{settings::key::text_outline}) == "0");
    CHECK(values.at(std::string{settings::key::text_shadow}) == "0");
    CHECK(values.at(std::string{settings::key::text_background}) == "1");
    CHECK(values.at(std::string{settings::key::text_size}) == "150");
    CHECK(values.at(std::string{settings::key::language}) == "de");
    CHECK(values.at("Total Annihilation|SwitchAlt") == "1");
    CHECK(settings::read_settings(values, {}, false) == chosen);

    // One setting changed: only its key is written, even back to its default.
    Values one;
    auto back = chosen;
    back.max_frame_rate = settings::highest_frame_rate;
    settings::write_settings(one, chosen, back, defaults, false);
    CHECK(one.size() == 1);
    CHECK(one.at(std::string{settings::key::max_frame_rate}) == "120");

    // An unchanged setting keeps whatever its key holds.
    Values kept = one_key(settings::key::unit_limit, "garbage");
    settings::write_settings(kept, defaults, defaults, defaults, false);
    CHECK(kept.at(std::string{settings::key::unit_limit}) == "garbage");
}

void file_type_switches_store_on_and_off() {
    const settings::EngineSettings defaults{};
    CHECK(
        defaults.file_type_oamod && defaults.file_type_oamap && defaults.file_type_oalang &&
        defaults.file_type_oareg && defaults.file_type_oamut
    );

    // Absent keys, "on", and any other text read as on. Only "off" turns one off.
    const auto from_absent = settings::read_settings({}, {}, false);
    CHECK(
        from_absent.file_type_oamod && from_absent.file_type_oamap &&
        from_absent.file_type_oalang && from_absent.file_type_oareg && from_absent.file_type_oamut
    );
    Values mixed;
    mixed[std::string{settings::key::file_type_oamod}] = "off";
    mixed[std::string{settings::key::file_type_oamap}] = "on";
    mixed[std::string{settings::key::file_type_oalang}] = "0";
    mixed[std::string{settings::key::file_type_oareg}] = "no";
    mixed[std::string{settings::key::file_type_oamut}] = "off";
    const auto read = settings::read_settings(mixed, {}, false);
    CHECK(!read.file_type_oamod);
    CHECK(read.file_type_oamap);
    CHECK(read.file_type_oalang);
    CHECK(read.file_type_oareg);
    CHECK(!read.file_type_oamut);
    CHECK(settings::key::file_type_oamut == "open-annihilation.file-types.oamut");

    // A switch moved off is stored as off. One left on writes nothing.
    auto chosen = defaults;
    chosen.file_type_oamod = false;
    chosen.file_type_oamap = false;
    Values written;
    settings::write_settings(written, defaults, chosen, defaults, false);
    CHECK(written.at(std::string{settings::key::file_type_oamod}) == "off");
    CHECK(written.at(std::string{settings::key::file_type_oamap}) == "off");
    CHECK(written.count(std::string{settings::key::file_type_oalang}) == 0);
    CHECK(written.count(std::string{settings::key::file_type_oareg}) == 0);
    CHECK(written.count(std::string{settings::key::file_type_oamut}) == 0);
    CHECK(settings::read_settings(written, {}, false).file_type_oamod == false);
    CHECK(settings::read_settings(written, {}, false).file_type_oalang);

    // Turning one back on stores on. Restore defaults erases a stored off.
    auto back = chosen;
    back.file_type_oamod = true;
    Values again;
    settings::write_settings(again, chosen, back, defaults, false);
    CHECK(again.at(std::string{settings::key::file_type_oamod}) == "on");
    CHECK(again.count(std::string{settings::key::file_type_oamap}) == 0);
    Values restored = written;
    settings::write_settings(restored, chosen, defaults, defaults, true);
    CHECK(restored.count(std::string{settings::key::file_type_oamod}) == 0);
    CHECK(restored.count(std::string{settings::key::file_type_oamap}) == 0);
    CHECK(settings::read_settings(restored, {}, false).file_type_oamod);

    // The mutator packages' switch moved off is stored as off, and reads back off.
    auto mutators_off = defaults;
    mutators_off.file_type_oamut = false;
    Values mutator_written;
    settings::write_settings(mutator_written, defaults, mutators_off, defaults, false);
    CHECK(mutator_written.at(std::string{settings::key::file_type_oamut}) == "off");
    CHECK(!settings::read_settings(mutator_written, {}, false).file_type_oamut);
}

void the_launch_file_switch_round_trips() {
    // On by default, read as on from an absent key, "on" or any text but off.
    const settings::EngineSettings defaults{};
    CHECK(defaults.file_type_oalaunch);
    CHECK(settings::key::file_type_oalaunch == "open-annihilation.file-types.oalaunch");
    CHECK(settings::read_settings({}, {}, false).file_type_oalaunch);
    for (const std::string_view text : {"on", "0", "no", "OFF", ""})
        CHECK(
            settings::read_settings(
                one_key(settings::key::file_type_oalaunch, std::string(text)), {}, false
            )
                .file_type_oalaunch
        );
    const auto off =
        settings::read_settings(one_key(settings::key::file_type_oalaunch, "off"), {}, false);
    CHECK(!off.file_type_oalaunch);
    CHECK(
        off.file_type_oamod && off.file_type_oamap && off.file_type_oalang && off.file_type_oareg &&
        off.file_type_oamut
    );

    // Moved off, it alone is stored, as off, and reads back off with the other five on.
    auto chosen = defaults;
    chosen.file_type_oalaunch = false;
    Values written;
    settings::write_settings(written, defaults, chosen, defaults, false);
    CHECK(written.at(std::string{settings::key::file_type_oalaunch}) == "off");
    for (const std::string_view other :
         {settings::key::file_type_oamod,
          settings::key::file_type_oamap,
          settings::key::file_type_oalang,
          settings::key::file_type_oareg,
          settings::key::file_type_oamut})
        CHECK(written.count(std::string{other}) == 0);
    const auto read = settings::read_settings(written, {}, false);
    CHECK(!read.file_type_oalaunch);
    CHECK(
        read.file_type_oamod && read.file_type_oamap && read.file_type_oalang &&
        read.file_type_oareg && read.file_type_oamut
    );

    // Moved on again, it is stored as on and reads back on.
    Values again = written;
    settings::write_settings(again, chosen, defaults, defaults, false);
    CHECK(again.at(std::string{settings::key::file_type_oalaunch}) == "on");
    CHECK(settings::read_settings(again, {}, false).file_type_oalaunch);

    // Restore defaults erases a stored off.
    Values restored = written;
    settings::write_settings(restored, chosen, defaults, defaults, true);
    CHECK(restored.count(std::string{settings::key::file_type_oalaunch}) == 0);
    CHECK(settings::read_settings(restored, {}, false).file_type_oalaunch);
}

void restore_defaults_erases_the_keys_of_settings_at_their_defaults() {
    const settings::EngineSettings defaults{};
    const auto stored = changed_settings();
    Values values;
    settings::write_settings(values, defaults, stored, defaults, false);
    values["Total Annihilation|SwitchAlt"] = "1";

    // Restore defaults, then OK: every key goes, and other keys stay.
    Values restored = values;
    settings::write_settings(restored, stored, defaults, defaults, true);
    CHECK(restored.size() == 1);
    CHECK(restored.count("Total Annihilation|SwitchAlt") == 1);
    CHECK(settings::read_settings(restored, {}, false) == defaults);

    // Restore defaults, then a setting set again: it is written; one set back
    // to the value the dialog opened with keeps its key.
    auto chosen = defaults;
    chosen.unit_limit = 750;
    chosen.anti_aliasing = stored.anti_aliasing;
    Values again = values;
    settings::write_settings(again, stored, chosen, defaults, true);
    CHECK(again.size() == 3);
    CHECK(again.at(std::string{settings::key::unit_limit}) == "750");
    CHECK(again.at(std::string{settings::key::anti_aliasing}) == "16");
    CHECK(settings::read_settings(again, {}, false) == chosen);

    // A default that depends on the platform is erased, not written.
    const auto mac_defaults = settings::default_settings({true, true, {}});
    Values mac = one_key(settings::key::escape_opens_menu, "0");
    auto mac_opened = mac_defaults;
    mac_opened.escape_opens_menu = false;
    settings::write_settings(mac, mac_opened, mac_defaults, mac_defaults, true);
    CHECK(mac.empty());
}

void the_round_trip_keeps_every_value() {
    for (const auto level : settings::anti_aliasing_levels) {
        auto chosen = changed_settings();
        chosen.anti_aliasing = level;
        chosen.unit_limit = settings::lowest_unit_limit;
        chosen.max_frame_rate = settings::lowest_frame_rate;
        chosen.path_search_nodes = settings::highest_path_search_nodes;
        chosen.screen_size =
            settings::screen_sizes[static_cast<std::size_t>(level) % settings::screen_sizes.size()];
        Values values;
        settings::write_settings(values, {}, chosen, {}, false);
        CHECK(settings::read_settings(values, players_own_on_linux, false) == chosen);
        // Every level of Hardware acceleration, against the player's own
        // file's Full.
        const auto own = settings::default_settings(players_own_on_linux);
        chosen.vertical_sync = false;
        for (const auto acceleration : settings::hardware_acceleration_levels) {
            chosen.hardware_acceleration = acceleration;
            Values against_own;
            settings::write_settings(against_own, own, chosen, own, false);
            CHECK(
                settings::read_settings(against_own, players_own_on_linux, false)
                    .hardware_acceleration == acceleration
            );
            CHECK(!settings::read_settings(against_own, players_own_on_linux, false).vertical_sync);
        }
    }
}

void the_frame_rate_goes_down_to_a_frame_a_tick() {
    // The lowest maximum frame rate is the simulation's 30 ticks a second,
    // and the setting's steps reach it from the highest.
    CHECK(settings::lowest_frame_rate == 30);
    CHECK(
        (settings::highest_frame_rate - settings::lowest_frame_rate) % settings::frame_rate_step ==
        0
    );
    settings::EngineSettings chosen{};
    chosen.max_frame_rate = settings::lowest_frame_rate;
    Values values;
    settings::write_settings(values, {}, chosen, {}, false);
    CHECK(values.at(std::string{settings::key::max_frame_rate}) == "30");
    CHECK(settings::read_settings(values, players_own_on_linux, false).max_frame_rate == 30);
}

void the_path_credit_shows_as_whole_cycles() {
    CHECK(settings::path_search_multiplier(1333) == 1);
    CHECK(settings::path_search_multiplier(1999) == 1);
    CHECK(settings::path_search_multiplier(2000) == 2);
    CHECK(settings::path_search_multiplier(2666) == 2);
    CHECK(settings::path_search_multiplier(10664) == 8);
    CHECK(settings::path_search_multiplier(0) == 1);
    CHECK(settings::path_search_multiplier(-5000) == 1);
    CHECK(settings::path_search_multiplier(1'000'000) == 8);
    CHECK(settings::path_search_multiplier(2'147'483'647) == 8);
}

void shared_games_and_replays_search_at_one_cycle() {
    settings::EngineSettings chosen{};
    chosen.path_search_nodes = 3 * settings::base_path_search_nodes;
    CHECK(settings::match_path_search_nodes(chosen, false) == 3999);
    CHECK(settings::match_path_search_nodes(chosen, true) == settings::base_path_search_nodes);
}

void a_mods_unit_limits_set_the_range_and_the_default() {
    // A mod's limits of 1,500 in 20 to 1,500, and the largest a profile may name.
    settings::Inputs raised{};
    raised.units_per_player = {1500, 20, 1500, 1500};
    CHECK(settings::default_settings(raised).unit_limit == 1500);
    CHECK(settings::highest_offered_unit_limit(raised.units_per_player) == 1500);
    raised.players_own_profile = true;
    raised.installation_ini = "[Preferences]\nUnitLimit=1200\n";
    CHECK(settings::default_settings(raised).unit_limit == 1200);
    settings::Inputs largest{};
    largest.units_per_player = {
        oa::data::limits::highest_units_per_player,
        20,
        oa::data::limits::highest_units_per_player,
        1500
    };
    CHECK(
        settings::highest_offered_unit_limit(largest.units_per_player) ==
        oa::data::limits::highest_units_per_player
    );
    const auto stored = [&](const char* text) {
        return settings::read_settings(one_key(settings::key::unit_limit, text), largest, false)
            .unit_limit;
    };
    CHECK(stored("6000") == 6000);
    CHECK(stored("99999") == oa::data::limits::highest_units_per_player);
    CHECK(stored("5") == 20);
    // 3.1c's limits keep the setting's own highest stop.
    CHECK(settings::highest_offered_unit_limit({}) == settings::highest_unit_limit);
}

void a_mods_path_budget_scales_the_credit() {
    settings::EngineSettings chosen{};
    chosen.path_search_nodes = 3 * settings::base_path_search_nodes;
    CHECK(settings::match_path_search_nodes(chosen, false, 66650) == 3 * 66650);
    CHECK(settings::match_path_search_nodes(chosen, true, 66650) == 66650);
    // The largest budget a profile may name, times eight, holds to that budget.
    chosen.path_search_nodes = settings::highest_path_search_nodes;
    CHECK(
        settings::match_path_search_nodes(
            chosen, false, oa::data::limits::highest_path_search_nodes
        ) == oa::data::limits::highest_path_search_nodes
    );
    CHECK(
        settings::match_path_search_nodes(
            chosen, true, oa::data::limits::highest_path_search_nodes
        ) == oa::data::limits::highest_path_search_nodes
    );
}

void the_skirmish_player_count_is_not_an_engine_key() {
    settings::EngineSettings chosen{};
    chosen.skirmish_players = 6;
    Values values;
    settings::write_settings(values, {}, chosen, {}, false);
    CHECK(values.empty());
    CHECK(
        settings::read_settings(values, {}, false).skirmish_players ==
        settings::default_skirmish_players
    );
}

void a_game_locks_the_next_game_settings() {
    const auto alone = settings::settings_locks({true, false, false, false});
    CHECK(alone.path_search == settings::Lock::in_game);
    CHECK(alone.unit_limit == settings::Lock::in_game);
    CHECK(alone.skirmish_players == settings::Lock::in_game);
    CHECK(alone.max_frame_rate == settings::Lock::none);
    CHECK(!alone.shared_game);
    const auto shared = settings::settings_locks({true, true, false, false});
    CHECK(shared.path_search == settings::Lock::set_by_host);
    CHECK(shared.unit_limit == settings::Lock::set_by_host);
    CHECK(shared.skirmish_players == settings::Lock::set_by_host);
    CHECK(shared.shared_game);
    const auto replay = settings::settings_locks({true, false, true, false});
    CHECK(replay.path_search == settings::Lock::set_by_host);
    CHECK(replay.unit_limit == settings::Lock::set_by_host);
    CHECK(replay.skirmish_players == settings::Lock::set_by_host);
    CHECK(!replay.shared_game);
    const auto menu = settings::settings_locks({});
    CHECK(menu.path_search == settings::Lock::none);
    CHECK(menu.unit_limit == settings::Lock::none);
    CHECK(menu.skirmish_players == settings::Lock::none);
    CHECK(!menu.shared_game);
    const auto command_line = settings::settings_locks({false, false, false, true});
    CHECK(command_line.max_frame_rate == settings::Lock::command_line);
    CHECK(command_line.path_search == settings::Lock::none);
    // --max-fps locks only its own row.
    CHECK(command_line.hardware_acceleration == settings::Lock::none);
    CHECK(command_line.vertical_sync == settings::Lock::none);
}

void the_renderer_settings_lock_by_the_flags_and_the_renderer() {
    using settings::Lock;
    // Hardware acceleration: either flag, then nothing in the game that
    // could help; never a game, shared or not, so Off stays possible.
    for (const bool in_game : {false, true})
        for (const bool shared : {false, true})
            for (const bool replay : {false, true}) {
                settings::GameState state{in_game, shared, replay, false};
                CHECK(settings::settings_locks(state).hardware_acceleration == Lock::none);
                state.acceleration_unavailable = true;
                CHECK(settings::settings_locks(state).hardware_acceleration == Lock::unavailable);
                state.renderer_from_command_line = true;
                CHECK(settings::settings_locks(state).hardware_acceleration == Lock::command_line);
                state.acceleration_unavailable = false;
                CHECK(settings::settings_locks(state).hardware_acceleration == Lock::command_line);
                // Neither touches Vertical sync or the other rows.
                const auto locks = settings::settings_locks(state);
                const auto plain =
                    settings::settings_locks(settings::GameState{in_game, shared, replay, false});
                CHECK(locks.vertical_sync == plain.vertical_sync);
                CHECK(locks.path_search == plain.path_search);
                CHECK(locks.max_frame_rate == Lock::none);
            }
    // Vertical sync: during a shared game or a replay, not in a game played
    // alone; a renderer that cannot wait for the display wins over the game.
    CHECK(settings::settings_locks({}).vertical_sync == Lock::none);
    CHECK(settings::settings_locks({true, false, false, false}).vertical_sync == Lock::none);
    CHECK(settings::settings_locks({true, true, false, false}).vertical_sync == Lock::in_game);
    CHECK(settings::settings_locks({true, false, true, false}).vertical_sync == Lock::in_game);
    // A shared or replay flag outside a game locks nothing.
    CHECK(settings::settings_locks({false, true, true, false}).vertical_sync == Lock::none);
    for (const bool in_game : {false, true})
        for (const bool shared : {false, true}) {
            settings::GameState state{in_game, shared, false, false};
            state.vertical_sync_unavailable = true;
            CHECK(settings::settings_locks(state).vertical_sync == Lock::unavailable);
            CHECK(settings::settings_locks(state).hardware_acceleration == Lock::none);
        }
}

/// The Language switches' keys, and the text size's.
constexpr std::array<std::string_view, 5> text_keys{
    settings::key::modern_fonts,
    settings::key::text_outline,
    settings::key::text_shadow,
    settings::key::text_background,
    settings::key::text_size,
};

void language_and_text_defaults_to_modern_fonts_with_outline_and_shadow() {
    // The player's own file, on every platform and machine: modern fonts,
    // their outline and shadow On, the background Off; the text style the
    // drawing reads is TextStyle's own default.
    const std::array<settings::Inputs, 4> own{{
        players_own_on_linux,
        {true, true, {}},
        {true, false, {}, true},
        {true, false, {}, false, true, {1024, 768}},
    }};
    for (const auto& inputs : own) {
        const auto defaults = settings::default_settings(inputs);
        CHECK(defaults.modern_fonts);
        CHECK(defaults.text_outline);
        CHECK(defaults.text_shadow);
        CHECK(!defaults.text_background);
        CHECK(defaults.text_size == 80);
        CHECK(settings::text_style(defaults) == oa::present::TextStyle{});
        CHECK(settings::text_style(defaults).size == 80);
    }
    // A named file draws game text in the game's own fonts; the outline,
    // shadow and background keep their defaults.
    for (auto inputs : own) {
        inputs.players_own_profile = false;
        const auto defaults = settings::default_settings(inputs);
        CHECK(!defaults.modern_fonts);
        CHECK(defaults.text_outline && defaults.text_shadow && !defaults.text_background);
        CHECK(defaults.text_size == 80);
        const auto style = settings::text_style(defaults);
        CHECK(!style.modern_fonts && style.outline && style.shadow && !style.background);
        CHECK(style.size == 80);
    }
}

void a_file_without_the_text_switches_reads_their_defaults() {
    // A file an earlier version wrote: other settings and the game's own
    // keys, and none of Language's.
    Values earlier;
    earlier[std::string{settings::key::wheel_zoom}] = "0";
    earlier[std::string{settings::key::max_frame_rate}] = "60";
    earlier[std::string{settings::key::hardware_acceleration}] = "basic";
    earlier["Total Annihilation|UnitLimit"] = "1000";
    for (const auto& inputs : {settings::Inputs{}, players_own_on_linux}) {
        const auto defaults = settings::default_settings(inputs);
        const auto read = settings::read_settings(earlier, inputs, false);
        CHECK(!read.wheel_zoom && read.max_frame_rate == 60);
        CHECK(read.modern_fonts == defaults.modern_fonts);
        CHECK(read.text_outline == defaults.text_outline);
        CHECK(read.text_shadow == defaults.text_shadow);
        CHECK(read.text_background == defaults.text_background);
        // Without its key the text size is the default, 80%.
        CHECK(read.text_size == 80);
        CHECK(settings::text_style(read) == settings::text_style(defaults));
        // A value that is no whole number gives the default too.
        for (const auto key : text_keys)
            for (const char* text : {"", "on", "true", " 1", "1.0"})
                CHECK(
                    settings::text_style(
                        settings::read_settings(one_key(key, text), inputs, false)
                    ) == settings::text_style(defaults)
                );
    }
    // Each reads as a switch: on above 0.
    const auto own = [](std::string_view key, const char* text) {
        return settings::read_settings(one_key(key, text), players_own_on_linux, false);
    };
    CHECK(!own(settings::key::modern_fonts, "0").modern_fonts);
    CHECK(read_one(settings::key::modern_fonts, "1").modern_fonts);
    CHECK(!own(settings::key::text_outline, "0").text_outline);
    CHECK(!own(settings::key::text_shadow, "-1").text_shadow);
    CHECK(own(settings::key::text_background, "1").text_background);
    CHECK(own(settings::key::text_background, "3").text_background);
}

void the_text_switches_round_trip_and_restore() {
    const auto defaults = settings::default_settings(players_own_on_linux);
    // Every combination of the four, chosen in a dialog opened on the
    // defaults: only those away from it are written, and they read back.
    for (uint32_t bits = 0; bits < 16; ++bits) {
        auto chosen = defaults;
        chosen.modern_fonts = (bits & 1U) != 0;
        chosen.text_outline = (bits & 2U) != 0;
        chosen.text_shadow = (bits & 4U) != 0;
        chosen.text_background = (bits & 8U) != 0;
        Values values;
        settings::write_settings(values, defaults, chosen, defaults, false);
        std::size_t changed = 0;
        for (const auto& [key, member] :
             std::array<std::pair<std::string_view, bool settings::EngineSettings::*>, 4>{{
                 {settings::key::modern_fonts, &settings::EngineSettings::modern_fonts},
                 {settings::key::text_outline, &settings::EngineSettings::text_outline},
                 {settings::key::text_shadow, &settings::EngineSettings::text_shadow},
                 {settings::key::text_background, &settings::EngineSettings::text_background},
             }}) {
            if (chosen.*member == defaults.*member)
                continue;
            ++changed;
            CHECK(values.at(std::string{key}) == (chosen.*member ? "1" : "0"));
        }
        CHECK(values.size() == changed);
        const auto read = settings::read_settings(values, players_own_on_linux, false);
        CHECK(read == chosen);
        CHECK(settings::text_style(read) == settings::text_style(chosen));
        // Restore defaults, then OK: their keys go.
        Values restored = values;
        settings::write_settings(restored, chosen, defaults, defaults, true);
        CHECK(restored.empty());
    }
}

void the_text_size_reads_writes_and_restores_in_its_range() {
    CHECK(settings::key::text_size == "open-annihilation.text-size");
    CHECK(settings::lowest_text_size == 50 && settings::highest_text_size == 300);
    CHECK(settings::text_size_step == 10 && settings::default_text_size == 80);
    // Stored sizes are clamped into 50 to 300, and one between the steps
    // is kept as stored; what is no whole number gives the default.
    const auto size = [](const char* text) {
        return settings::read_settings(
                   one_key(settings::key::text_size, text), players_own_on_linux, false
        )
            .text_size;
    };
    CHECK(size("50") == 50 && size("80") == 80 && size("150") == 150 && size("300") == 300);
    CHECK(size("85") == 85 && size("+120") == 120);
    CHECK(size("10") == 50 && size("0") == 50 && size("-20") == 50);
    CHECK(size("301") == 300 && size("99999999999999") == 300);
    for (const char* text : {"", "big", "80%", " 80", "80 ", "1.5", "0x50"})
        CHECK(size(text) == 80);
    // It reads the same with a named file, where modern fonts start Off.
    CHECK(read_one(settings::key::text_size, "200").text_size == 200);
    CHECK(read_one(settings::key::text_size, "200").modern_fonts == false);

    // Written as its percent only when it changed, and read back; Restore
    // defaults, then OK, erases it.
    const auto defaults = settings::default_settings(players_own_on_linux);
    for (int32_t percent = settings::lowest_text_size; percent <= settings::highest_text_size;
         percent += settings::text_size_step) {
        auto chosen = defaults;
        chosen.text_size = percent;
        Values values;
        settings::write_settings(values, defaults, chosen, defaults, false);
        if (percent == settings::default_text_size) {
            CHECK(values.empty());
            continue;
        }
        CHECK(values.size() == 1);
        CHECK(values.at(std::string{settings::key::text_size}) == std::to_string(percent));
        const auto read = settings::read_settings(values, players_own_on_linux, false);
        CHECK(read == chosen);
        CHECK(settings::text_style(read).size == percent);
        settings::write_settings(values, chosen, defaults, defaults, true);
        CHECK(values.empty());
    }
    // Back to the default by hand keeps a key, written as 80.
    Values values = one_key(settings::key::text_size, "150");
    auto larger = defaults;
    larger.text_size = 150;
    settings::write_settings(values, larger, defaults, defaults, false);
    CHECK(values.at(std::string{settings::key::text_size}) == "80");
}

void the_language_reads_writes_and_restores() {
    namespace languages = oa::data::languages;
    // The player's own file follows the operating system; a named file plays
    // in English, as the game does without a language on its command line.
    CHECK(settings::default_settings(players_own_on_linux).language == "system");
    CHECK(settings::default_settings(settings::Inputs{}).language == "en");
    // A file without the key, as an older version wrote, reads as the
    // default: the system's language with the player's own file.
    Values earlier;
    earlier[std::string{settings::key::wheel_zoom}] = "0";
    CHECK(settings::read_settings(earlier, players_own_on_linux, false).language == "system");
    CHECK(settings::read_settings(earlier, settings::Inputs{}, false).language == "en");
    CHECK(settings::stored_language(earlier, true) == "system");
    // "system" or a known tag, in any case and with spaces around it; any
    // other value, a tag this build does not know included, the default.
    const auto read = [](const char* text, bool own) {
        return settings::stored_language(one_key(settings::key::language, text), own);
    };
    CHECK(read("system", false) == "system");
    CHECK(read("System", false) == "system");
    CHECK(read("de", true) == "de");
    CHECK(read("DE", true) == "de");
    CHECK(read(" fr ", true) == "fr");
    CHECK(read("es", true) == "es" && read("it", true) == "it" && read("en", true) == "en");
    for (const char* other : {"", "pt", "de-AT", "german", "1", "xx_YY"}) {
        CHECK(read(other, true) == "system");
        CHECK(read(other, false) == "en");
    }
    for (const languages::Language* language : languages::known_languages())
        CHECK(read(std::string(language->tag).c_str(), true) == language->tag);
    // A changed language is written as its tag; one left alone is not
    // written, so a tag a later version wrote stays in the file.
    const auto defaults = settings::default_settings(players_own_on_linux);
    auto chosen = defaults;
    chosen.language = "it";
    Values written;
    settings::write_settings(written, defaults, chosen, defaults, false);
    CHECK(written.at(std::string{settings::key::language}) == "it");
    CHECK(settings::read_settings(written, players_own_on_linux, false).language == "it");
    Values kept = one_key(settings::key::language, "pt");
    const auto opened = settings::read_settings(kept, players_own_on_linux, false);
    CHECK(opened.language == "system");
    settings::write_settings(kept, opened, opened, defaults, false);
    CHECK(kept.at(std::string{settings::key::language}) == "pt");
    // Back to System default is written too, and Restore defaults erases it.
    settings::write_settings(written, chosen, defaults, defaults, false);
    CHECK(written.at(std::string{settings::key::language}) == "system");
    settings::write_settings(written, chosen, defaults, defaults, true);
    CHECK(written.count(std::string{settings::key::language}) == 0);
    // The command line's language locks the setting for the run; no game does.
    settings::GameState state{};
    CHECK(settings::settings_locks(state).language == settings::Lock::none);
    state.in_game = true;
    state.shared_game = true;
    CHECK(settings::settings_locks(state).language == settings::Lock::none);
    state.language_from_command_line = true;
    CHECK(settings::settings_locks(state).language == settings::Lock::command_line);
    // The language changes nothing the text style draws.
    CHECK(settings::text_style(chosen) == settings::text_style(defaults));
}

/// A language that is not installed yet is kept when this build can draw
/// it, and a shorter tag is still the system's language.
void an_available_language_is_kept_until_its_pack_is_installed() {
    namespace languages = oa::data::languages;
    languages::set_pack_languages({}, {});
    languages::LanguageEntry portuguese;
    portuguese.tag = "pt-BR";
    portuguese.endonym = "Portugu\xC3\xAA"
                         "s";
    portuguese.english_name = "Portuguese";
    portuguese.word = "Portuguese";
    portuguese.needs = languages::TextNeeds::game_fonts;
    languages::set_pack_languages({}, std::array<languages::LanguageEntry, 1>{portuguese});
    CHECK(settings::stored_language(one_key(settings::key::language, "pt-BR"), true) == "pt-BR");
    CHECK(settings::stored_language(one_key(settings::key::language, "pt"), true) == "system");
    languages::set_pack_languages({}, {});
}

void a_preferences_file_with_crlf_line_ends_reads_its_settings() {
    const auto folder =
        std::filesystem::temp_directory_path() /
        ("oa-engine-settings-crlf-" +
         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    std::filesystem::create_directories(folder);
    const auto file = folder / "preferences.conf";
    std::ofstream(file, std::ios::binary) << "open-annihilation-preferences 1\r\n"
                                             "\"open-annihilation.max-fps\" \"60\"\r\n"
                                             "\"open-annihilation.modern-fonts\" \"0\"\r\n"
                                             "\"open-annihilation.text-background\" \"1\"\r\n"
                                             "\"open-annihilation.text-shadow\" \"0\"\r\n"
                                             "\"open-annihilation.text-size\" \"120\"\r\n";
    Values values;
    bool loaded = true;
    try {
        values = oa::platform::preferences::load(file);
    } catch (const std::exception& error) {
        std::cerr << "CR LF preferences: " << error.what() << '\n';
        loaded = false;
    }
    std::error_code ignored;
    std::filesystem::remove_all(folder, ignored);
    CHECK(loaded);
    const auto read = settings::read_settings(values, players_own_on_linux, false);
    CHECK(read.max_frame_rate == 60);
    CHECK(!read.modern_fonts);
    CHECK(read.text_outline);
    CHECK(!read.text_shadow);
    CHECK(read.text_background);
    CHECK(read.text_size == 120);
}

void developer_mode_and_its_overrides_are_kept_under_the_profiles_id() {
    namespace profiles = oa::data::mod_profile;
    const settings::EngineSettings defaults{};
    CHECK(!defaults.developer_mode && defaults.hack_overrides.empty());
    CHECK(!settings::default_settings(players_own_on_linux).developer_mode);
    // A file without them reads Off and no override.
    settings::Inputs base{};
    base.profile_id = profiles::base_game_id;
    const auto none = settings::read_settings({}, base, false);
    CHECK(!none.developer_mode && none.hack_overrides.empty());

    auto chosen = defaults;
    chosen.developer_mode = true;
    chosen.hack_overrides = {
        profiles::HackOverride{
            "ai.attack-wave-size", true, {{"units", profiles::make_integer(40)}}
        },
        profiles::HackOverride{"repair.rate", false, {}},
    };
    Values values;
    settings::write_settings(values, defaults, chosen, defaults, false, profiles::base_game_id);
    const std::string base_key = std::string{settings::key::hack_overrides} + "ta-3.1c";
    CHECK(values.size() == 2);
    CHECK(values.at(std::string{settings::key::developer_mode}) == "1");
    CHECK(values.at(base_key) == "{\"ai.attack-wave-size\":{\"units\":40},\"repair.rate\":false}");
    const auto read = settings::read_settings(values, base, false);
    CHECK(read.developer_mode);
    CHECK(read.hack_overrides.size() == chosen.hack_overrides.size());
    for (const auto& override : chosen.hack_overrides) {
        const auto* found = profiles::find_override(read.hack_overrides, override.hack);
        CHECK(found != nullptr && *found == override);
    }

    // Each profile keeps its own; without an id none is read or written.
    settings::Inputs mod{};
    mod.profile_id = "some-mod";
    CHECK(settings::read_settings(values, mod, false).hack_overrides.empty());
    CHECK(settings::read_settings(values, {}, false).hack_overrides.empty());
    Values without_id;
    settings::write_settings(without_id, defaults, chosen, defaults, false);
    CHECK(without_id.size() == 1 && !without_id.contains(base_key));

    // Restore defaults turns Developer Mode off and keeps the overrides.
    auto restored_choice = defaults;
    restored_choice.hack_overrides = chosen.hack_overrides;
    Values restored = values;
    settings::write_settings(
        restored, chosen, restored_choice, defaults, true, profiles::base_game_id
    );
    CHECK(!restored.contains(std::string{settings::key::developer_mode}));
    CHECK(restored.at(base_key) == values.at(base_key));

    // With none left, the key goes; an unchanged list is not written.
    auto cleared = chosen;
    cleared.hack_overrides.clear();
    Values emptied = values;
    settings::write_settings(emptied, chosen, cleared, defaults, false, profiles::base_game_id);
    CHECK(!emptied.contains(base_key));
    Values kept = one_key(base_key, "garbage");
    settings::write_settings(kept, chosen, chosen, defaults, false, profiles::base_game_id);
    CHECK(kept.at(base_key) == "garbage");

    // A text the profile grammar does not read as a mapping gives none.
    CHECK(
        settings::read_settings(one_key(base_key, "{not json"), base, false).hack_overrides.empty()
    );
}

} // namespace

void the_mod_is_stored_as_its_folder() {
    const std::vector<std::string> folders{"/games/ta/mods/alpha", "/games/ta/mods/beta"};
    settings::Inputs inputs{};
    inputs.mod_folders = folders;
    const settings::EngineSettings defaults{};
    CHECK(defaults.mod_folder.empty() && defaults.picked_mod_folder.empty());

    Values values;
    auto chosen = defaults;
    chosen.mod_folder = folders[1];
    settings::write_settings(values, defaults, chosen, defaults, false);
    CHECK(values.size() == 1);
    CHECK(values.at(std::string{settings::key::mod_directory}) == "/games/ta/mods/beta");
    const auto read = settings::read_settings(values, inputs, false);
    CHECK(read.mod_folder == folders[1] && read.picked_mod_folder.empty());

    // No mod erases the key, never writing an empty path.
    settings::write_settings(values, chosen, defaults, defaults, false);
    CHECK(!values.contains(std::string{settings::key::mod_directory}));

    // Restore defaults forgets the mod.
    values[std::string{settings::key::mod_directory}] = folders[0];
    auto first = defaults;
    first.mod_folder = folders[0];
    settings::write_settings(values, first, defaults, defaults, true);
    CHECK(!values.contains(std::string{settings::key::mod_directory}));
}

void the_touch_settings_default_alike_everywhere() {
    CHECK(settings::key::touch_drag == "open-annihilation.touch-drag");
    CHECK(settings::key::touch_hold_delay == "open-annihilation.touch-hold-delay");
    CHECK(settings::key::touch_latches == "open-annihilation.touch-latches");
    CHECK(settings::key::touch_haptics == "open-annihilation.touch-haptics");
    CHECK(settings::key::touch_left_handed == "open-annihilation.touch-left-handed");
    settings::Inputs own_mac = players_own_on_linux;
    own_mac.macos = true;
    settings::Inputs pi = players_own_on_linux;
    pi.raspberry_pi = true;
    settings::Inputs light = players_own_on_linux;
    light.light_machine = true;
    for (const auto& inputs : {settings::Inputs{}, players_own_on_linux, own_mac, pi, light}) {
        const auto defaults = settings::default_settings(inputs);
        CHECK(defaults.touch_drag == settings::TouchDrag::automatic);
        CHECK(defaults.touch_hold_ms == settings::default_touch_hold_ms);
        CHECK(defaults.touch_hold_ms == 350);
        CHECK(defaults.touch_latches == settings::TouchLatches::stay_on);
        CHECK(defaults.touch_haptics);
        CHECK(!defaults.touch_left_handed);
        // A file without the keys reads the defaults.
        CHECK(settings::read_settings({}, inputs, false) == defaults);
    }
}

void the_touch_settings_read_their_words_and_drop_others() {
    // The words each way is kept as, and back.
    for (const auto drag : settings::touch_drag_choices)
        CHECK(settings::touch_drag_from_text(settings::touch_drag_text(drag)) == drag);
    for (const auto latches : settings::touch_latches_choices)
        CHECK(settings::touch_latches_from_text(settings::touch_latches_text(latches)) == latches);
    CHECK(settings::touch_drag_text(settings::TouchDrag::automatic) == "automatic");
    CHECK(settings::touch_drag_text(settings::TouchDrag::box) == "box");
    CHECK(settings::touch_drag_text(settings::TouchDrag::scroll) == "scroll");
    CHECK(settings::touch_latches_text(settings::TouchLatches::stay_on) == "stay-on");
    CHECK(settings::touch_latches_text(settings::TouchLatches::one_action) == "one-action");
    const auto drag = [](const char* text) {
        return read_one(settings::key::touch_drag, text).touch_drag;
    };
    CHECK(drag("box") == settings::TouchDrag::box);
    CHECK(drag("scroll") == settings::TouchDrag::scroll);
    CHECK(drag("automatic") == settings::TouchDrag::automatic);
    for (const char* text : {"", "Box", "SCROLL", " box", "box ", "1", "2", "boxes", "drag"})
        CHECK(drag(text) == settings::TouchDrag::automatic);
    const auto latches = [](const char* text) {
        return read_one(settings::key::touch_latches, text).touch_latches;
    };
    CHECK(latches("one-action") == settings::TouchLatches::one_action);
    CHECK(latches("stay-on") == settings::TouchLatches::stay_on);
    for (const char* text : {"", "one_action", "One-action", "1", "once", "one-action "})
        CHECK(latches(text) == settings::TouchLatches::stay_on);
    // The switches read as every switch does; a value that is no number is
    // dropped for the default.
    CHECK(!read_one(settings::key::touch_haptics, "0").touch_haptics);
    CHECK(!read_one(settings::key::touch_haptics, "-1").touch_haptics);
    CHECK(read_one(settings::key::touch_haptics, "1").touch_haptics);
    CHECK(read_one(settings::key::touch_haptics, "off").touch_haptics);
    CHECK(read_one(settings::key::touch_left_handed, "1").touch_left_handed);
    CHECK(read_one(settings::key::touch_left_handed, "7").touch_left_handed);
    CHECK(!read_one(settings::key::touch_left_handed, "0").touch_left_handed);
    CHECK(!read_one(settings::key::touch_left_handed, "yes").touch_left_handed);
    // One key changes its own setting alone.
    auto expected = settings::read_settings({}, {}, false);
    expected.touch_drag = settings::TouchDrag::scroll;
    CHECK(read_one(settings::key::touch_drag, "scroll") == expected);
}

void the_hold_delay_is_held_to_its_range_and_stops() {
    static_assert(settings::snapped_touch_hold_ms(275) == 300);
    static_assert(settings::snapped_touch_hold_ms(274) == 250);
    CHECK(settings::lowest_touch_hold_ms == 250 && settings::highest_touch_hold_ms == 700);
    CHECK(settings::touch_hold_step_ms == 50);
    const auto hold = [](const char* text) {
        return read_one(settings::key::touch_hold_delay, text).touch_hold_ms;
    };
    // Each stop reads as itself.
    for (uint32_t ms = 250; ms <= 700; ms += 50)
        CHECK(hold(std::to_string(ms).c_str()) == ms);
    // Out of range, the nearest end; between stops, the nearest stop, half a
    // step up.
    CHECK(hold("0") == 250 && hold("-40") == 250 && hold("249") == 250 && hold("100") == 250);
    CHECK(hold("701") == 700 && hold("5000") == 700 && hold("99999999999999") == 700);
    CHECK(hold("274") == 250 && hold("275") == 300 && hold("299") == 300 && hold("324") == 300);
    CHECK(hold("326") == 350 && hold("374") == 350 && hold("375") == 400 && hold("699") == 700);
    CHECK(hold("+450") == 450);
    // What is no whole number gives the default.
    for (const char* text : {"", "fast", "350ms", " 350", "350 ", "0.4", "0x100"})
        CHECK(hold(text) == settings::default_touch_hold_ms);
}

void the_touch_settings_round_trip_and_restore() {
    const auto defaults = settings::default_settings(players_own_on_linux);

    struct Change {
        std::string_view key;
        std::string_view text;
        void (*apply)(settings::EngineSettings&);
    };

    const std::array<Change, 6> changes{{
        {settings::key::touch_drag,
         "box",
         [](settings::EngineSettings& chosen) { chosen.touch_drag = settings::TouchDrag::box; }},
        {settings::key::touch_drag,
         "scroll",
         [](settings::EngineSettings& chosen) { chosen.touch_drag = settings::TouchDrag::scroll; }},
        {settings::key::touch_hold_delay,
         "600",
         [](settings::EngineSettings& chosen) { chosen.touch_hold_ms = 600; }},
        {settings::key::touch_latches,
         "one-action",
         [](settings::EngineSettings& chosen) {
             chosen.touch_latches = settings::TouchLatches::one_action;
         }},
        {settings::key::touch_haptics,
         "0",
         [](settings::EngineSettings& chosen) { chosen.touch_haptics = false; }},
        {settings::key::touch_left_handed, "1", [](settings::EngineSettings& chosen) {
             chosen.touch_left_handed = true;
         }},
    }};
    // Each change alone writes its key alone, reads back, and Restore
    // defaults then OK erases it.
    for (const Change& change : changes) {
        auto chosen = defaults;
        change.apply(chosen);
        Values values;
        settings::write_settings(values, defaults, chosen, defaults, false);
        CHECK(values.size() == 1);
        CHECK(values.at(std::string{change.key}) == change.text);
        CHECK(settings::read_settings(values, players_own_on_linux, false) == chosen);
        settings::write_settings(values, chosen, defaults, defaults, true);
        CHECK(values.empty());
    }
    // All of them at once: five keys, read back alike.
    auto chosen = defaults;
    for (const Change& change : changes)
        change.apply(chosen);
    Values values;
    settings::write_settings(values, defaults, chosen, defaults, false);
    CHECK(values.size() == 5);
    CHECK(values.at(std::string{settings::key::touch_drag}) == "scroll");
    CHECK(settings::read_settings(values, players_own_on_linux, false) == chosen);
    // An unchanged setting is never written, even away from its default.
    Values again;
    settings::write_settings(again, chosen, chosen, defaults, false);
    CHECK(again.empty());
    // Back to the defaults by hand keeps keys, written as the defaults.
    settings::write_settings(values, chosen, defaults, defaults, false);
    CHECK(values.at(std::string{settings::key::touch_drag}) == "automatic");
    CHECK(values.at(std::string{settings::key::touch_hold_delay}) == "350");
    CHECK(values.at(std::string{settings::key::touch_latches}) == "stay-on");
    CHECK(values.at(std::string{settings::key::touch_haptics}) == "1");
    CHECK(values.at(std::string{settings::key::touch_left_handed}) == "0");
    CHECK(settings::read_settings(values, players_own_on_linux, false) == defaults);
}

void the_backups_switch_is_off_by_default_and_round_trips() {
    CHECK(settings::key::game_files_backed_up == "open-annihilation.game-files-backed-up");
    settings::Inputs own_mac = players_own_on_linux;
    own_mac.macos = true;
    settings::Inputs light = players_own_on_linux;
    light.light_machine = true;
    for (const auto& inputs : {settings::Inputs{}, players_own_on_linux, own_mac, light}) {
        CHECK(!settings::default_settings(inputs).game_files_backed_up);
        CHECK(!settings::read_settings({}, inputs, false).game_files_backed_up);
    }
    // It reads as every switch does.
    CHECK(read_one(settings::key::game_files_backed_up, "1").game_files_backed_up);
    CHECK(read_one(settings::key::game_files_backed_up, "3").game_files_backed_up);
    CHECK(!read_one(settings::key::game_files_backed_up, "0").game_files_backed_up);
    CHECK(!read_one(settings::key::game_files_backed_up, "on").game_files_backed_up);
    auto expected = settings::read_settings({}, {}, false);
    expected.game_files_backed_up = true;
    CHECK(read_one(settings::key::game_files_backed_up, "1") == expected);
    // Turned on, only its key is written; Restore defaults then erases it.
    const auto defaults = settings::default_settings(players_own_on_linux);
    auto chosen = defaults;
    chosen.game_files_backed_up = true;
    Values values;
    settings::write_settings(values, defaults, chosen, defaults, false);
    CHECK(values.size() == 1);
    CHECK(values.at(std::string{settings::key::game_files_backed_up}) == "1");
    CHECK(settings::read_settings(values, players_own_on_linux, false) == chosen);
    settings::write_settings(values, chosen, defaults, defaults, true);
    CHECK(values.empty());
    // Turned off again by hand, the key stays, written as Off.
    settings::write_settings(values, defaults, chosen, defaults, false);
    settings::write_settings(values, chosen, defaults, defaults, false);
    CHECK(values.at(std::string{settings::key::game_files_backed_up}) == "0");
    CHECK(!settings::read_settings(values, players_own_on_linux, false).game_files_backed_up);
}

void a_picked_folder_is_kept_while_it_is_played() {
    const std::vector<std::string> folders{"/games/ta/mods/alpha"};
    settings::Inputs inputs{};
    inputs.mod_folders = folders;
    const settings::EngineSettings defaults{};
    const std::string picked = "/home/player/my mods/Some Mod";
    const std::string picked_key{settings::key::picked_mod_directory};

    // A picked folder is the mod and the picked folder, each under its key.
    Values values;
    auto chosen = defaults;
    chosen.mod_folder = picked;
    chosen.picked_mod_folder = picked;
    settings::write_settings(values, defaults, chosen, defaults, false);
    CHECK(values.size() == 2);
    CHECK(values.at(std::string{settings::key::mod_directory}) == picked);
    CHECK(values.at(picked_key) == picked);
    auto read = settings::read_settings(values, inputs, false);
    CHECK(read.mod_folder == picked && read.picked_mod_folder == picked);

    // Restore defaults leaves the mod played, and the picked folder with it.
    auto restored = defaults;
    restored.mod_folder = picked;
    restored.picked_mod_folder = picked;
    settings::write_settings(values, chosen, restored, defaults, true);
    CHECK(values.at(picked_key) == picked);

    // Another mod, or No Mod, forgets it.
    for (const std::string& next_mod : {folders[0], std::string{}}) {
        Values played = values;
        auto switched = chosen;
        switched.mod_folder = next_mod;
        settings::write_settings(played, chosen, switched, defaults, false);
        CHECK(!played.contains(picked_key));
        read = settings::read_settings(played, inputs, false);
        CHECK(read.mod_folder == next_mod && read.picked_mod_folder.empty());
    }

    // A key an earlier version left naming a folder that is not the mod
    // reads as none, and the next save erases it.
    Values stale;
    stale[picked_key] = picked;
    stale[std::string{settings::key::mod_directory}] = folders[0];
    read = settings::read_settings(stale, inputs, false);
    CHECK(read.mod_folder == folders[0] && read.picked_mod_folder.empty());
    CHECK(settings::remembered_picked_folder(stale).empty());
    settings::write_settings(stale, read, read, defaults, false);
    CHECK(!stale.contains(picked_key));

    // A mod folder the game folder does not offer reads as the picked one,
    // as a file a version without Pick Folder... wrote may hold.
    Values older;
    older[std::string{settings::key::mod_directory}] = "/old/install/mods/gone";
    read = settings::read_settings(older, inputs, false);
    CHECK(read.mod_folder == "/old/install/mods/gone");
    CHECK(read.picked_mod_folder == "/old/install/mods/gone");
}

void an_older_preferences_file_reads_no_mod() {
    // A file a version before the Mod drop-down wrote: no mod key, no
    // picked folder; every other setting reads as before.
    Values older;
    older[std::string{settings::key::unit_limit}] = "500";
    older[std::string{settings::key::wheel_zoom}] = "0";
    const auto read = settings::read_settings(older, {}, false);
    CHECK(read.mod_folder.empty() && read.picked_mod_folder.empty());
    CHECK(read.unit_limit == 500 && !read.wheel_zoom);
    // An offered mod an older version chose reads as that mod.
    const std::vector<std::string> folders{"/games/ta/mods/alpha"};
    settings::Inputs inputs{};
    inputs.mod_folders = folders;
    older[std::string{settings::key::mod_directory}] = folders[0];
    const auto modded = settings::read_settings(older, inputs, false);
    CHECK(modded.mod_folder == folders[0] && modded.picked_mod_folder.empty());
}

void a_folder_without_a_profile_keeps_its_overrides_under_its_own_id() {
    namespace profiles = oa::data::mod_profile;
    // The id the game keeps a mod folder without a profile's overrides under:
    // "folder:" and its path, which no profile's id can be.
    const std::string folder_id = "folder:/home/player/plain archives/a%7Cb";
    const std::string folder_key = std::string{settings::key::hack_overrides} + folder_id;
    const std::string base_key = std::string{settings::key::hack_overrides} + "ta-3.1c";
    const settings::EngineSettings defaults{};
    auto chosen = defaults;
    chosen.developer_mode = true;
    chosen.hack_overrides = {profiles::HackOverride{"repair.rate", false, {}}};

    // A file an earlier version wrote holds the plain game's overrides
    // only: the folder reads none of them, and they stay as they were.
    Values values = one_key(base_key, "{\"ai.attack-wave-size\":{\"units\":40}}");
    settings::Inputs folder{};
    folder.profile_id = folder_id;
    CHECK(settings::read_settings(values, folder, false).hack_overrides.empty());
    settings::write_settings(values, defaults, chosen, defaults, false, folder_id);
    CHECK(values.at(folder_key) == "{\"repair.rate\":false}");
    CHECK(values.at(base_key) == "{\"ai.attack-wave-size\":{\"units\":40}}");
    const auto read = settings::read_settings(values, folder, false);
    CHECK(
        read.hack_overrides.size() == 1 && read.hack_overrides.front() == chosen.hack_overrides[0]
    );
    settings::Inputs base{};
    base.profile_id = profiles::base_game_id;
    const auto base_read = settings::read_settings(values, base, false);
    CHECK(base_read.hack_overrides.size() == 1);
    CHECK(base_read.hack_overrides.front().hack == "ai.attack-wave-size");
}

void a_steam_deck_starts_at_its_screens_rate_with_larger_controls() {
    settings::Inputs lcd = players_own_on_linux;
    lcd.steam_deck_panel_hz = 60;
    settings::Inputs oled = players_own_on_linux;
    oled.steam_deck_panel_hz = 90;
    const auto plain = settings::default_settings(players_own_on_linux);
    CHECK(plain.max_frame_rate == settings::highest_frame_rate);
    CHECK(plain.touch_control_size == settings::ControlSize::standard);
    CHECK(settings::steam_deck_control_size == settings::ControlSize::larger);
    for (const auto& [inputs, rate] : {std::pair{lcd, 60U}, std::pair{oled, 90U}}) {
        const auto defaults = settings::default_settings(inputs);
        CHECK(defaults.max_frame_rate == rate);
        CHECK(defaults.touch_control_size == settings::ControlSize::larger);
        // Nothing else moves.
        auto others = defaults;
        others.max_frame_rate = plain.max_frame_rate;
        others.touch_control_size = plain.touch_control_size;
        CHECK(others == plain);
        // A named preferences file plays as the game does everywhere.
        settings::Inputs named = inputs;
        named.players_own_profile = false;
        CHECK(settings::default_settings(named) == settings::default_settings({}));

        // Without stored values the Deck's defaults hold; a stored value wins.
        CHECK(settings::read_settings({}, inputs, false) == defaults);
        const auto faster =
            settings::read_settings(one_key(settings::key::max_frame_rate, "120"), inputs, false);
        CHECK(faster.max_frame_rate == 120);
        CHECK(faster.touch_control_size == settings::ControlSize::larger);
        const auto smaller = settings::read_settings(
            one_key(settings::key::touch_control_size, "standard"), inputs, false
        );
        CHECK(smaller.touch_control_size == settings::ControlSize::standard);
        CHECK(smaller.max_frame_rate == rate);

        // The player's choices are stored; Restore defaults puts the Deck's back.
        auto chosen = defaults;
        chosen.max_frame_rate = 45;
        chosen.touch_control_size = settings::ControlSize::standard;
        Values values;
        settings::write_settings(values, defaults, chosen, defaults, false);
        CHECK(values.size() == 2);
        CHECK(values.at(std::string{settings::key::max_frame_rate}) == "45");
        CHECK(values.at(std::string{settings::key::touch_control_size}) == "standard");
        CHECK(settings::read_settings(values, inputs, false) == chosen);
        settings::write_settings(values, chosen, defaults, defaults, true);
        CHECK(values.empty());
        CHECK(settings::read_settings(values, inputs, false) == defaults);
        // Chosen by hand, the Deck's own values are kept as stored ones.
        settings::write_settings(values, chosen, defaults, defaults, false);
        CHECK(values.at(std::string{settings::key::max_frame_rate}) == std::to_string(rate));
        CHECK(values.at(std::string{settings::key::touch_control_size}) == "larger");
        CHECK(settings::read_settings(values, inputs, false) == defaults);
    }
    // A screen's rate is held to the setting's range and put on its nearest stop.
    const auto rate_for = [](uint32_t hz) {
        settings::Inputs deck = players_own_on_linux;
        deck.steam_deck_panel_hz = hz;
        return settings::default_settings(deck).max_frame_rate;
    };
    CHECK(rate_for(144) == settings::highest_frame_rate);
    CHECK(rate_for(20) == settings::lowest_frame_rate);
    CHECK(rate_for(62) == 60);
    CHECK(rate_for(63) == 65);
    CHECK(rate_for(0) == settings::highest_frame_rate);
    // A Raspberry Pi's or a light machine's defaults are as before.
    settings::Inputs pi = players_own_on_linux;
    pi.raspberry_pi = true;
    CHECK(settings::default_settings(pi).touch_control_size == settings::ControlSize::standard);
}

void the_controller_settings_default_alike_everywhere() {
    namespace pad = oa::ui::pad_controls;
    CHECK(settings::key::touch_control_size == "open-annihilation.touch-control-size");
    CHECK(settings::key::pad_scheme == "open-annihilation.pad-scheme");
    CHECK(settings::key::pad_right_trackpad == "open-annihilation.pad-right-trackpad");
    CHECK(settings::key::pad_pointer_speed == "open-annihilation.pad-pointer-speed");
    CHECK(settings::key::pad_acceleration == "open-annihilation.pad-acceleration");
    CHECK(settings::key::pad_glide == "open-annihilation.pad-glide");
    CHECK(settings::key::pad_right_stick == "open-annihilation.pad-right-stick");
    CHECK(settings::key::pad_magnetism == "open-annihilation.pad-magnetism");
    CHECK(settings::key::pad_gyro == "open-annihilation.pad-gyro");
    CHECK(settings::key::pad_gyro_speed == "open-annihilation.pad-gyro-speed");
    CHECK(settings::key::pad_haptics == "open-annihilation.pad-haptics");
    CHECK(settings::key::pad_prompts == "open-annihilation.pad-prompts");
    CHECK(settings::key::pad_left_handed == "open-annihilation.pad-left-handed");
    CHECK(settings::control_size_scale(settings::ControlSize::standard) == 1.0F);
    CHECK(settings::control_size_scale(settings::ControlSize::large) == 1.25F);
    CHECK(settings::control_size_scale(settings::ControlSize::larger) == 1.5F);
    settings::Inputs own_mac = players_own_on_linux;
    own_mac.macos = true;
    settings::Inputs pi = players_own_on_linux;
    pi.raspberry_pi = true;
    settings::Inputs deck = players_own_on_linux;
    deck.steam_deck_panel_hz = 90;
    for (const auto& inputs : {settings::Inputs{}, players_own_on_linux, own_mac, pi, deck}) {
        const auto defaults = settings::default_settings(inputs);
        CHECK(defaults.pad_scheme == pad::Scheme::trackpads);
        CHECK(defaults.pad_right_trackpad == pad::RightTrackpad::relative);
        CHECK(defaults.pad_pointer_speed == pad::default_pointer_speed);
        CHECK(defaults.pad_pointer_speed == 100);
        CHECK(defaults.pad_acceleration == pad::Acceleration::low);
        CHECK(!defaults.pad_glide);
        CHECK(defaults.pad_right_stick == pad::RightStick::zoom_and_pages);
        CHECK(defaults.pad_magnetism);
        CHECK(defaults.pad_gyro == pad::Gyro::off);
        CHECK(defaults.pad_gyro_speed == pad::default_gyro_speed);
        CHECK(defaults.pad_haptics == pad::Haptics::light);
        CHECK(defaults.pad_prompts == pad::Prompts::automatic);
        CHECK(!defaults.pad_left_handed);
        CHECK(settings::read_settings({}, inputs, false) == defaults);
    }
}

void the_controller_settings_read_their_words_and_drop_others() {
    namespace pad = oa::ui::pad_controls;
    const auto defaults = settings::read_settings({}, {}, false);
    // Each word reads as its choice; one key changes its own setting alone.
    const auto read_word = [](std::string_view key, const char* text) {
        return read_one(key, text);
    };
    auto expected = defaults;
    expected.touch_control_size = settings::ControlSize::large;
    CHECK(read_word(settings::key::touch_control_size, "large") == expected);
    CHECK(
        read_word(settings::key::touch_control_size, "larger").touch_control_size ==
        settings::ControlSize::larger
    );
    CHECK(
        read_word(settings::key::touch_control_size, "standard").touch_control_size ==
        settings::ControlSize::standard
    );
    CHECK(read_word(settings::key::pad_scheme, "sticks").pad_scheme == pad::Scheme::sticks);
    CHECK(read_word(settings::key::pad_scheme, "trackpads").pad_scheme == pad::Scheme::trackpads);
    CHECK(
        read_word(settings::key::pad_right_trackpad, "absolute").pad_right_trackpad ==
        pad::RightTrackpad::absolute
    );
    CHECK(
        read_word(settings::key::pad_acceleration, "off").pad_acceleration == pad::Acceleration::off
    );
    CHECK(
        read_word(settings::key::pad_acceleration, "high").pad_acceleration ==
        pad::Acceleration::high
    );
    CHECK(
        read_word(settings::key::pad_right_stick, "pointer").pad_right_stick ==
        pad::RightStick::pointer
    );
    CHECK(
        read_word(settings::key::pad_right_stick, "nothing").pad_right_stick ==
        pad::RightStick::nothing
    );
    CHECK(
        read_word(settings::key::pad_right_stick, "zoom").pad_right_stick ==
        pad::RightStick::zoom_and_pages
    );
    CHECK(read_word(settings::key::pad_gyro, "right-pad").pad_gyro == pad::Gyro::right_pad_touched);
    CHECK(
        read_word(settings::key::pad_gyro, "right-stick").pad_gyro == pad::Gyro::right_stick_touched
    );
    CHECK(read_word(settings::key::pad_gyro, "always").pad_gyro == pad::Gyro::always);
    CHECK(read_word(settings::key::pad_haptics, "off").pad_haptics == pad::Haptics::off);
    CHECK(read_word(settings::key::pad_haptics, "strong").pad_haptics == pad::Haptics::strong);
    CHECK(
        read_word(settings::key::pad_prompts, "steam-deck").pad_prompts == pad::Prompts::steam_deck
    );
    CHECK(read_word(settings::key::pad_prompts, "xbox").pad_prompts == pad::Prompts::xbox);
    CHECK(
        read_word(settings::key::pad_prompts, "playstation").pad_prompts ==
        pad::Prompts::playstation
    );
    CHECK(read_word(settings::key::pad_prompts, "nintendo").pad_prompts == pad::Prompts::nintendo);
    CHECK(read_word(settings::key::pad_prompts, "off").pad_prompts == pad::Prompts::off);
    // Any other text gives the default: words match letter for letter.
    for (const char* text : {"", "Larger", "LARGE", " large", "larger ", "2", "huge"})
        CHECK(read_word(settings::key::touch_control_size, text) == defaults);
    for (const char* text : {"", "Sticks", "stick", "1"})
        CHECK(read_word(settings::key::pad_scheme, text) == defaults);
    for (const char* text : {"", "Absolute", "pointer", "1"})
        CHECK(read_word(settings::key::pad_right_trackpad, text) == defaults);
    for (const char* text : {"", "medium", "0", "HIGH"})
        CHECK(read_word(settings::key::pad_acceleration, text) == defaults);
    for (const char* text : {"", "zoom-and-pages", "Pointer", "none"})
        CHECK(read_word(settings::key::pad_right_stick, text) == defaults);
    for (const char* text : {"", "right_pad", "on", "1", "Always"})
        CHECK(read_word(settings::key::pad_gyro, text) == defaults);
    for (const char* text : {"", "medium", "0", "Strong"})
        CHECK(read_word(settings::key::pad_haptics, text) == defaults);
    for (const char* text : {"", "steam_deck", "deck", "Xbox", "auto"})
        CHECK(read_word(settings::key::pad_prompts, text) == defaults);
    // The switches read as every switch does.
    CHECK(read_one(settings::key::pad_glide, "1").pad_glide);
    CHECK(!read_one(settings::key::pad_glide, "0").pad_glide);
    CHECK(!read_one(settings::key::pad_glide, "on").pad_glide);
    CHECK(!read_one(settings::key::pad_magnetism, "0").pad_magnetism);
    CHECK(read_one(settings::key::pad_magnetism, "off").pad_magnetism);
    CHECK(read_one(settings::key::pad_left_handed, "3").pad_left_handed);
    CHECK(!read_one(settings::key::pad_left_handed, "-1").pad_left_handed);
}

void the_controller_speeds_are_held_to_their_ranges_and_stops() {
    namespace pad = oa::ui::pad_controls;
    const auto pointer = [](const char* text) {
        return read_one(settings::key::pad_pointer_speed, text).pad_pointer_speed;
    };
    const auto gyro = [](const char* text) {
        return read_one(settings::key::pad_gyro_speed, text).pad_gyro_speed;
    };
    CHECK(pad::lowest_pointer_speed == 50 && pad::highest_pointer_speed == 300);
    CHECK(pad::lowest_gyro_speed == 50 && pad::highest_gyro_speed == 400);
    for (uint32_t percent = 50; percent <= 300; percent += 10)
        CHECK(pointer(std::to_string(percent).c_str()) == percent);
    for (uint32_t percent = 50; percent <= 400; percent += 10)
        CHECK(gyro(std::to_string(percent).c_str()) == percent);
    CHECK(pointer("0") == 50 && pointer("-30") == 50 && pointer("49") == 50);
    CHECK(pointer("301") == 300 && pointer("99999999999") == 300);
    CHECK(pointer("54") == 50 && pointer("55") == 60 && pointer("124") == 120);
    CHECK(gyro("401") == 400 && gyro("10") == 50 && gyro("395") == 400 && gyro("394") == 390);
    CHECK(pointer("+150") == 150);
    for (const char* text : {"", "fast", "100%", " 100", "1.5"}) {
        CHECK(pointer(text) == pad::default_pointer_speed);
        CHECK(gyro(text) == pad::default_gyro_speed);
    }
}

void the_controller_settings_round_trip_and_restore() {
    namespace pad = oa::ui::pad_controls;
    const auto defaults = settings::default_settings(players_own_on_linux);

    struct Change {
        std::string_view key;
        std::string_view text;
        void (*apply)(settings::EngineSettings&);
    };

    const std::array<Change, 14> changes{{
        {settings::key::touch_control_size,
         "large",
         [](settings::EngineSettings& chosen) {
             chosen.touch_control_size = settings::ControlSize::large;
         }},
        {settings::key::pad_scheme,
         "sticks",
         [](settings::EngineSettings& chosen) { chosen.pad_scheme = pad::Scheme::sticks; }},
        {settings::key::pad_right_trackpad,
         "absolute",
         [](settings::EngineSettings& chosen) {
             chosen.pad_right_trackpad = pad::RightTrackpad::absolute;
         }},
        {settings::key::pad_pointer_speed,
         "250",
         [](settings::EngineSettings& chosen) { chosen.pad_pointer_speed = 250; }},
        {settings::key::pad_acceleration,
         "high",
         [](settings::EngineSettings& chosen) {
             chosen.pad_acceleration = pad::Acceleration::high;
         }},
        {settings::key::pad_glide,
         "1",
         [](settings::EngineSettings& chosen) { chosen.pad_glide = true; }},
        {settings::key::pad_right_stick,
         "pointer",
         [](settings::EngineSettings& chosen) {
             chosen.pad_right_stick = pad::RightStick::pointer;
         }},
        {settings::key::pad_magnetism,
         "0",
         [](settings::EngineSettings& chosen) { chosen.pad_magnetism = false; }},
        {settings::key::pad_gyro,
         "right-stick",
         [](settings::EngineSettings& chosen) {
             chosen.pad_gyro = pad::Gyro::right_stick_touched;
         }},
        {settings::key::pad_gyro_speed,
         "320",
         [](settings::EngineSettings& chosen) { chosen.pad_gyro_speed = 320; }},
        {settings::key::pad_haptics,
         "strong",
         [](settings::EngineSettings& chosen) { chosen.pad_haptics = pad::Haptics::strong; }},
        {settings::key::pad_prompts,
         "playstation",
         [](settings::EngineSettings& chosen) { chosen.pad_prompts = pad::Prompts::playstation; }},
        {settings::key::pad_left_handed,
         "1",
         [](settings::EngineSettings& chosen) { chosen.pad_left_handed = true; }},
        {settings::key::pad_prompts, "off", [](settings::EngineSettings& chosen) {
             chosen.pad_prompts = pad::Prompts::off;
         }},
    }};
    // Each change alone writes its key alone, reads back, and Restore
    // defaults then OK erases it.
    for (const Change& change : changes) {
        auto chosen = defaults;
        change.apply(chosen);
        Values values;
        settings::write_settings(values, defaults, chosen, defaults, false);
        CHECK(values.size() == 1);
        CHECK(values.at(std::string{change.key}) == change.text);
        CHECK(settings::read_settings(values, players_own_on_linux, false) == chosen);
        settings::write_settings(values, chosen, defaults, defaults, true);
        CHECK(values.empty());
    }
    // All of them at once: thirteen keys, read back alike.
    auto chosen = defaults;
    for (const Change& change : changes)
        change.apply(chosen);
    Values values;
    settings::write_settings(values, defaults, chosen, defaults, false);
    CHECK(values.size() == 13);
    CHECK(settings::read_settings(values, players_own_on_linux, false) == chosen);
    // An unchanged setting is never written, even away from its default.
    Values again;
    settings::write_settings(again, chosen, chosen, defaults, false);
    CHECK(again.empty());
    // Back to the defaults by hand keeps keys, written as the defaults.
    settings::write_settings(values, chosen, defaults, defaults, false);
    CHECK(values.at(std::string{settings::key::touch_control_size}) == "standard");
    CHECK(values.at(std::string{settings::key::pad_scheme}) == "trackpads");
    CHECK(values.at(std::string{settings::key::pad_right_trackpad}) == "relative");
    CHECK(values.at(std::string{settings::key::pad_pointer_speed}) == "100");
    CHECK(values.at(std::string{settings::key::pad_acceleration}) == "low");
    CHECK(values.at(std::string{settings::key::pad_glide}) == "0");
    CHECK(values.at(std::string{settings::key::pad_right_stick}) == "zoom");
    CHECK(values.at(std::string{settings::key::pad_magnetism}) == "1");
    CHECK(values.at(std::string{settings::key::pad_gyro}) == "off");
    CHECK(values.at(std::string{settings::key::pad_gyro_speed}) == "100");
    CHECK(values.at(std::string{settings::key::pad_haptics}) == "light");
    CHECK(values.at(std::string{settings::key::pad_prompts}) == "automatic");
    CHECK(values.at(std::string{settings::key::pad_left_handed}) == "0");
    CHECK(settings::read_settings(values, players_own_on_linux, false) == defaults);
    // Every other choice's word reads back as it.
    for (const auto gyro_choice :
         {pad::Gyro::off,
          pad::Gyro::right_pad_touched,
          pad::Gyro::right_stick_touched,
          pad::Gyro::always}) {
        auto one = defaults;
        one.pad_gyro = gyro_choice;
        Values written;
        settings::write_settings(written, defaults, one, defaults, false);
        CHECK(settings::read_settings(written, players_own_on_linux, false) == one);
    }
    for (const auto size : settings::control_size_choices) {
        auto one = defaults;
        one.touch_control_size = size;
        Values written;
        settings::write_settings(written, defaults, one, defaults, false);
        CHECK(settings::read_settings(written, players_own_on_linux, false) == one);
    }
}

/// Explosion flash: Full everywhere by default, read from its three words
/// alone, and each other level written alone and read back; Restore
/// defaults then OK erases it.
void explosion_flash_default_read_and_round_trip() {
    CHECK(settings::key::explosion_flash == "open-annihilation.explosion-flash");
    settings::Inputs own_mac = players_own_on_linux;
    own_mac.macos = true;
    settings::Inputs light = players_own_on_linux;
    light.light_machine = true;
    for (const auto& inputs : {settings::Inputs{}, players_own_on_linux, own_mac, light}) {
        const auto defaults = settings::default_settings(inputs);
        CHECK(defaults.explosion_flash == settings::ExplosionFlash::full);
        CHECK(settings::read_settings({}, inputs, false) == defaults);
    }
    const auto flash = [](const char* text) {
        return read_one(settings::key::explosion_flash, text).explosion_flash;
    };
    CHECK(flash("off") == settings::ExplosionFlash::off);
    CHECK(flash("reduced") == settings::ExplosionFlash::reduced);
    CHECK(flash("full") == settings::ExplosionFlash::full);
    for (const char* text : {"", "Off", "OFF", "0", "1", "half", " off", "none"})
        CHECK(flash(text) == settings::ExplosionFlash::full);

    const auto defaults = settings::default_settings(players_own_on_linux);
    for (const auto& [level, word] :
         {std::pair{settings::ExplosionFlash::off, "off"},
          std::pair{settings::ExplosionFlash::reduced, "reduced"}}) {
        auto chosen = defaults;
        chosen.explosion_flash = level;
        Values values;
        settings::write_settings(values, defaults, chosen, defaults, false);
        CHECK(values.size() == 1);
        CHECK(values.at(std::string{settings::key::explosion_flash}) == word);
        CHECK(settings::read_settings(values, players_own_on_linux, false) == chosen);
        // Set back to Full by hand, the key stays, written as full.
        settings::write_settings(values, chosen, defaults, defaults, false);
        CHECK(values.at(std::string{settings::key::explosion_flash}) == "full");
        settings::write_settings(values, defaults, chosen, defaults, false);
        settings::write_settings(values, chosen, defaults, defaults, true);
        CHECK(values.empty());
    }
}

/// The zoom's limits: Automatic and 4x everywhere by default, as the view
/// always zoomed; a file written before them, which holds neither key,
/// reads the defaults; each word reads its choice and any other text the
/// default; each other choice written alone and read back, and Restore
/// defaults then OK erases both keys.
void the_zoom_limits_default_read_and_round_trip() {
    CHECK(settings::key::max_zoom_out == "open-annihilation.max-zoom-out");
    CHECK(settings::key::max_zoom_in == "open-annihilation.max-zoom-in");
    settings::Inputs own_mac = players_own_on_linux;
    own_mac.macos = true;
    settings::Inputs light = players_own_on_linux;
    light.light_machine = true;
    for (const auto& inputs : {settings::Inputs{}, players_own_on_linux, own_mac, light}) {
        const auto defaults = settings::default_settings(inputs);
        CHECK(defaults.max_zoom_out == settings::ZoomOutLimit::automatic);
        CHECK(defaults.max_zoom_in == settings::ZoomInLimit::four_times);
        CHECK(settings::read_settings({}, inputs, false) == defaults);
    }
    // An older file: the wheel's switch and other settings, no limits.
    Values older{
        {std::string{settings::key::wheel_zoom}, "1"},
        {std::string{settings::key::escape_opens_menu}, "1"},
    };
    const auto read_older = settings::read_settings(older, players_own_on_linux, false);
    CHECK(read_older.max_zoom_out == settings::ZoomOutLimit::automatic);
    CHECK(read_older.max_zoom_in == settings::ZoomInLimit::four_times);
    CHECK(read_older.escape_opens_menu);

    const auto out = [](const char* text) {
        return read_one(settings::key::max_zoom_out, text).max_zoom_out;
    };
    const auto in = [](const char* text) {
        return read_one(settings::key::max_zoom_in, text).max_zoom_in;
    };
    const std::array<std::pair<settings::ZoomOutLimit, const char*>, 7> out_words{{
        {settings::ZoomOutLimit::automatic, "automatic"},
        {settings::ZoomOutLimit::whole_map, "whole-map"},
        {settings::ZoomOutLimit::one_thirty_second, "1/32"},
        {settings::ZoomOutLimit::one_sixteenth, "1/16"},
        {settings::ZoomOutLimit::one_eighth, "1/8"},
        {settings::ZoomOutLimit::one_quarter, "1/4"},
        {settings::ZoomOutLimit::one_half, "1/2"},
    }};
    const std::array<std::pair<settings::ZoomInLimit, const char*>, 4> in_words{{
        {settings::ZoomInLimit::none, "1"},
        {settings::ZoomInLimit::twice, "2"},
        {settings::ZoomInLimit::three_times, "3"},
        {settings::ZoomInLimit::four_times, "4"},
    }};
    for (const auto& [limit, word] : out_words)
        CHECK(out(word) == limit);
    for (const auto& [limit, word] : in_words)
        CHECK(in(word) == limit);
    for (const char* text : {"", "Whole map", "WHOLE-MAP", "32", "0.5", "1/3", " 1/8", "8x"})
        CHECK(out(text) == settings::ZoomOutLimit::automatic);
    for (const char* text : {"", "0", "5", "8", "4x", "none", " 2"})
        CHECK(in(text) == settings::ZoomInLimit::four_times);

    const auto defaults = settings::default_settings(players_own_on_linux);
    for (const auto& [limit, word] : out_words) {
        if (limit == defaults.max_zoom_out)
            continue;
        auto chosen = defaults;
        chosen.max_zoom_out = limit;
        Values values;
        settings::write_settings(values, defaults, chosen, defaults, false);
        CHECK(values.size() == 1);
        CHECK(values.at(std::string{settings::key::max_zoom_out}) == word);
        CHECK(settings::read_settings(values, players_own_on_linux, false) == chosen);
    }
    for (const auto& [limit, word] : in_words) {
        if (limit == defaults.max_zoom_in)
            continue;
        auto chosen = defaults;
        chosen.max_zoom_in = limit;
        Values values;
        settings::write_settings(values, defaults, chosen, defaults, false);
        CHECK(values.size() == 1);
        CHECK(values.at(std::string{settings::key::max_zoom_in}) == word);
        CHECK(settings::read_settings(values, players_own_on_linux, false) == chosen);
    }
    auto both = defaults;
    both.max_zoom_out = settings::ZoomOutLimit::whole_map;
    both.max_zoom_in = settings::ZoomInLimit::none;
    Values values;
    settings::write_settings(values, defaults, both, defaults, false);
    CHECK(values.size() == 2);
    CHECK(settings::read_settings(values, players_own_on_linux, false) == both);
    settings::write_settings(values, both, defaults, defaults, true);
    CHECK(values.empty());
}

/// View past the map's edge: 50% everywhere by default, as far as the view
/// always went; a file written before it reads the default; each word reads
/// its choice and any other text the default; each other choice written
/// alone and read back, and Restore defaults then OK erases the key. Each
/// choice stands for its share of the battlefield.
void the_view_past_the_map_edge_defaults_reads_and_round_trips() {
    CHECK(settings::key::view_past_map_edge == "open-annihilation.view-past-map-edge");
    for (const auto& inputs : {settings::Inputs{}, players_own_on_linux}) {
        const auto defaults = settings::default_settings(inputs);
        CHECK(defaults.view_past_map_edge == settings::ViewPastMapEdge::one_half);
    }
    const Values older{{std::string{settings::key::max_zoom_out}, "whole-map"}};
    CHECK(
        settings::read_settings(older, players_own_on_linux, false).view_past_map_edge ==
        settings::ViewPastMapEdge::one_half
    );
    const auto past = [](const char* text) {
        return read_one(settings::key::view_past_map_edge, text).view_past_map_edge;
    };
    const std::array<std::pair<settings::ViewPastMapEdge, const char*>, 3> words{{
        {settings::ViewPastMapEdge::off, "off"},
        {settings::ViewPastMapEdge::one_quarter, "25"},
        {settings::ViewPastMapEdge::one_half, "50"},
    }};
    for (const auto& [limit, word] : words)
        CHECK(past(word) == limit);
    for (const char* text : {"", "0", "25%", "1/4", "Off", "75"})
        CHECK(past(text) == settings::ViewPastMapEdge::one_half);
    const auto defaults = settings::default_settings(players_own_on_linux);
    for (const auto& [limit, word] : words) {
        if (limit == defaults.view_past_map_edge)
            continue;
        auto chosen = defaults;
        chosen.view_past_map_edge = limit;
        Values values;
        settings::write_settings(values, defaults, chosen, defaults, false);
        CHECK(values.size() == 1);
        CHECK(values.at(std::string{settings::key::view_past_map_edge}) == word);
        CHECK(settings::read_settings(values, players_own_on_linux, false) == chosen);
        settings::write_settings(values, chosen, defaults, defaults, true);
        CHECK(values.empty());
    }
    CHECK(settings::past_map_edge_share(settings::ViewPastMapEdge::off) == 0.0);
    CHECK(settings::past_map_edge_share(settings::ViewPastMapEdge::one_quarter) == 0.25);
    CHECK(settings::past_map_edge_share(settings::ViewPastMapEdge::one_half) == 0.5);
}

/// How units look zoomed out: Rendered and 1/6 everywhere by default; a
/// file written before them reads the defaults; each word reads its choice,
/// and any other text, Icons' word among them, the default; each other
/// choice written alone and read back, and Restore defaults then OK erases
/// both keys. Each After zoom choice stands for its share of normal size.
void the_zoomed_out_units_default_read_and_round_trip() {
    CHECK(settings::key::zoomed_out_units == "open-annihilation.zoomed-out-units");
    CHECK(settings::key::zoomed_out_after == "open-annihilation.zoomed-out-after");
    settings::Inputs own_mac = players_own_on_linux;
    own_mac.macos = true;
    settings::Inputs light = players_own_on_linux;
    light.light_machine = true;
    for (const auto& inputs : {settings::Inputs{}, players_own_on_linux, own_mac, light}) {
        const auto defaults = settings::default_settings(inputs);
        CHECK(defaults.zoomed_out_units == settings::ZoomedOutUnits::rendered);
        CHECK(defaults.zoomed_out_after == settings::ZoomedOutAfter::one_sixth);
        CHECK(settings::read_settings({}, inputs, false) == defaults);
    }
    // An older file: the zoom's limits, without the keys.
    Values older{
        {std::string{settings::key::max_zoom_out}, "whole-map"},
        {std::string{settings::key::explosion_flash}, "reduced"},
    };
    const auto read_older = settings::read_settings(older, players_own_on_linux, false);
    CHECK(read_older.zoomed_out_units == settings::ZoomedOutUnits::rendered);
    CHECK(read_older.zoomed_out_after == settings::ZoomedOutAfter::one_sixth);
    CHECK(read_older.max_zoom_out == settings::ZoomOutLimit::whole_map);

    const auto units = [](const char* text) {
        return read_one(settings::key::zoomed_out_units, text).zoomed_out_units;
    };
    const auto after = [](const char* text) {
        return read_one(settings::key::zoomed_out_after, text).zoomed_out_after;
    };
    CHECK(units("rendered") == settings::ZoomedOutUnits::rendered);
    CHECK(units("dots") == settings::ZoomedOutUnits::dots);
    for (const char* text : {"", "icons", "Dots", "DOTS", "dot", " dots", "1"})
        CHECK(units(text) == settings::ZoomedOutUnits::rendered);
    const std::array<std::tuple<settings::ZoomedOutAfter, const char*, float>, 7> after_words{{
        {settings::ZoomedOutAfter::one_half, "1/2", 0.5F},
        {settings::ZoomedOutAfter::one_third, "1/3", 1.0F / 3.0F},
        {settings::ZoomedOutAfter::one_quarter, "1/4", 0.25F},
        {settings::ZoomedOutAfter::one_sixth, "1/6", 1.0F / 6.0F},
        {settings::ZoomedOutAfter::one_eighth, "1/8", 0.125F},
        {settings::ZoomedOutAfter::one_twelfth, "1/12", 1.0F / 12.0F},
        {settings::ZoomedOutAfter::one_sixteenth, "1/16", 0.0625F},
    }};
    for (const auto& [choice, word, zoom] : after_words) {
        CHECK(after(word) == choice);
        CHECK(settings::zoomed_out_zoom(choice) == zoom);
    }
    for (const char* text : {"", "1/5", "1/32", "0.25", "6", " 1/8", "1/6 "})
        CHECK(after(text) == settings::ZoomedOutAfter::one_sixth);

    const auto defaults = settings::default_settings(players_own_on_linux);
    auto dots = defaults;
    dots.zoomed_out_units = settings::ZoomedOutUnits::dots;
    Values values;
    settings::write_settings(values, defaults, dots, defaults, false);
    CHECK(values.size() == 1);
    CHECK(values.at(std::string{settings::key::zoomed_out_units}) == "dots");
    CHECK(settings::read_settings(values, players_own_on_linux, false) == dots);
    for (const auto& [choice, word, zoom] : after_words) {
        if (choice == defaults.zoomed_out_after)
            continue;
        auto chosen = defaults;
        chosen.zoomed_out_after = choice;
        Values one;
        settings::write_settings(one, defaults, chosen, defaults, false);
        CHECK(one.size() == 1);
        CHECK(one.at(std::string{settings::key::zoomed_out_after}) == word);
        CHECK(settings::read_settings(one, players_own_on_linux, false) == chosen);
    }
    auto both = dots;
    both.zoomed_out_after = settings::ZoomedOutAfter::one_quarter;
    Values pair;
    settings::write_settings(pair, defaults, both, defaults, false);
    CHECK(pair.size() == 2);
    CHECK(settings::read_settings(pair, players_own_on_linux, false) == both);
    settings::write_settings(pair, both, defaults, defaults, true);
    CHECK(pair.empty());
}

/// Window edge: Capture everywhere by default, also for a file an earlier
/// build wrote without the key; "capture", "sticky" and "free" read their
/// choices, so an earlier build's Sticky or Free is kept, and round-trip
/// alone; any other text reads Capture; writing Sticky leaves every other
/// key as it is, and Restore defaults then OK erases the key.
void the_window_edge_default_read_and_round_trip() {
    CHECK(settings::key::window_edge == "open-annihilation.window-edge");
    settings::Inputs own_mac = players_own_on_linux;
    own_mac.macos = true;
    for (const auto& inputs : {settings::Inputs{}, players_own_on_linux, own_mac}) {
        const auto defaults = settings::default_settings(inputs);
        CHECK(defaults.window_edge == settings::WindowEdge::capture);
        CHECK(settings::read_settings({}, inputs, false) == defaults);
    }
    const auto edge = [](const char* text) {
        return read_one(settings::key::window_edge, text).window_edge;
    };
    CHECK(edge("capture") == settings::WindowEdge::capture);
    CHECK(edge("sticky") == settings::WindowEdge::sticky);
    CHECK(edge("free") == settings::WindowEdge::free);
    for (const char* text : {"", "Free", "Capture", "sticky ", "1", "always"})
        CHECK(edge(text) == settings::WindowEdge::capture);

    const auto defaults = settings::default_settings(players_own_on_linux);
    for (const auto way : {settings::WindowEdge::sticky, settings::WindowEdge::free}) {
        auto chosen = defaults;
        chosen.window_edge = way;
        Values values;
        settings::write_settings(values, defaults, chosen, defaults, false);
        CHECK(values.size() == 1);
        CHECK(
            values.at(std::string{settings::key::window_edge}) ==
            (way == settings::WindowEdge::sticky ? "sticky" : "free")
        );
        CHECK(settings::read_settings(values, players_own_on_linux, false) == chosen);
        settings::write_settings(values, chosen, defaults, defaults, true);
        CHECK(values.empty());
    }
    // Capture chosen again after Sticky is written as such.
    auto sticky = defaults;
    sticky.window_edge = settings::WindowEdge::sticky;
    Values values;
    values[std::string{settings::key::window_edge}] = "sticky";
    settings::write_settings(values, sticky, defaults, defaults, false);
    CHECK(values.at(std::string{settings::key::window_edge}) == "capture");
    CHECK(settings::read_settings(values, players_own_on_linux, false) == defaults);
}

/// Edge stickiness: 1.0 everywhere by default and for a file without the
/// key; a decimal reads to the nearest tenth, a half rounding up, held to
/// 0.0 to 5.0; any other text reads 1.0; it is written with one decimal,
/// alone, whatever Window edge is, and Restore defaults then OK erases it.
void the_edge_stickiness_default_read_and_round_trip() {
    CHECK(settings::key::edge_stickiness == "open-annihilation.window-edge-stickiness");
    CHECK(settings::default_edge_stickiness == 10);
    for (const auto& inputs : {settings::Inputs{}, players_own_on_linux})
        CHECK(settings::default_settings(inputs).edge_stickiness == 10);
    const auto level = [](const char* text) {
        return read_one(settings::key::edge_stickiness, text).edge_stickiness;
    };
    CHECK(level("1.0") == 10);
    CHECK(level("0.0") == 0);
    CHECK(level("0") == 0);
    CHECK(level("2.5") == 25);
    CHECK(level("5.0") == 50);
    CHECK(level("3") == 30);
    CHECK(level(".5") == 5);
    CHECK(level("4.") == 40);
    CHECK(level("1.25") == 13);
    CHECK(level("1.249") == 12);
    CHECK(level("+0.3") == 3);
    CHECK(level("7.5") == 50);
    CHECK(level("-1.0") == 0);
    CHECK(level("99999999999999") == 50);
    for (const char* text : {"", ".", "1,5", "1.0 ", " 1.0", "1.2.3", "high", "1e1", "-", "0x1"})
        CHECK(level(text) == 10);
    CHECK(settings::edge_stickiness_text(0) == "0.0");
    CHECK(settings::edge_stickiness_text(10) == "1.0");
    CHECK(settings::edge_stickiness_text(25) == "2.5");
    CHECK(settings::edge_stickiness_text(50) == "5.0");
    CHECK(settings::edge_stickiness_from_text("2.5") == std::optional<uint32_t>{25});
    CHECK(!settings::edge_stickiness_from_text("x"));

    const auto defaults = settings::default_settings(players_own_on_linux);
    for (const auto way : settings::window_edge_choices) {
        auto chosen = defaults;
        chosen.window_edge = way;
        chosen.edge_stickiness = 25;
        auto opened = defaults;
        opened.window_edge = way;
        Values values;
        settings::write_settings(values, opened, chosen, defaults, false);
        CHECK(values.size() == 1);
        CHECK(values.at(std::string{settings::key::edge_stickiness}) == "2.5");
        auto read = settings::read_settings(values, players_own_on_linux, false);
        CHECK(read.edge_stickiness == 25 && read.window_edge == settings::WindowEdge::capture);
        settings::write_settings(values, chosen, defaults, defaults, true);
        CHECK(values.empty());
    }
}

/// Window frame: Hidden in play everywhere by default; a file written
/// before it reads the default; each word reads its way and any other text
/// the default; Always shown written alone and read back, and Restore
/// defaults then OK erases the key.
void the_window_frame_default_read_and_round_trip() {
    CHECK(settings::key::window_frame == "open-annihilation.window-frame");
    settings::Inputs own_mac = players_own_on_linux;
    own_mac.macos = true;
    for (const auto& inputs : {settings::Inputs{}, players_own_on_linux, own_mac}) {
        const auto defaults = settings::default_settings(inputs);
        CHECK(defaults.window_frame == settings::WindowFrame::hidden_in_play);
        CHECK(settings::read_settings({}, inputs, false) == defaults);
    }
    const auto frame = [](const char* text) {
        return read_one(settings::key::window_frame, text).window_frame;
    };
    CHECK(frame("hidden-in-play") == settings::WindowFrame::hidden_in_play);
    CHECK(frame("always-shown") == settings::WindowFrame::always_shown);
    for (const char* text : {"", "Always-Shown", "always", "1", " always-shown"})
        CHECK(frame(text) == settings::WindowFrame::hidden_in_play);

    const auto defaults = settings::default_settings(players_own_on_linux);
    auto shown = defaults;
    shown.window_frame = settings::WindowFrame::always_shown;
    Values values;
    settings::write_settings(values, defaults, shown, defaults, false);
    CHECK(values.size() == 1);
    CHECK(values.at(std::string{settings::key::window_frame}) == "always-shown");
    CHECK(settings::read_settings(values, players_own_on_linux, false) == shown);
    settings::write_settings(values, shown, defaults, defaults, true);
    CHECK(values.empty());
}

/// HUD scaling: On everywhere by default; a file written before it reads
/// the default; a number above 0 reads On and any other number Off; Off
/// written alone and read back, and Restore defaults then OK erases the key.
void the_hud_scaling_default_read_and_round_trip() {
    CHECK(settings::key::hud_scaling == "open-annihilation.hud-scaling");
    settings::Inputs own_mac = players_own_on_linux;
    own_mac.macos = true;
    for (const auto& inputs : {settings::Inputs{}, players_own_on_linux, own_mac}) {
        const auto defaults = settings::default_settings(inputs);
        CHECK(defaults.hud_scaling);
        CHECK(settings::read_settings({}, inputs, false) == defaults);
    }
    CHECK(read_one(settings::key::hud_scaling, "1").hud_scaling);
    CHECK(!read_one(settings::key::hud_scaling, "0").hud_scaling);

    const auto defaults = settings::default_settings(players_own_on_linux);
    auto unscaled = defaults;
    unscaled.hud_scaling = false;
    Values values;
    settings::write_settings(values, defaults, unscaled, defaults, false);
    CHECK(values.size() == 1);
    CHECK(values.at(std::string{settings::key::hud_scaling}) == "0");
    CHECK(settings::read_settings(values, players_own_on_linux, false) == unscaled);
    settings::write_settings(values, unscaled, defaults, defaults, true);
    CHECK(values.empty());
}

/// Intensity values supersede legacy switches, clamp and persist independently.
void visual_effect_switches_round_trip() {
    const auto defaults = settings::default_settings(players_own_on_linux);

    struct Effect {
        std::string_view legacy;
        std::string_view key;
        uint32_t settings::EngineSettings::* field;
    };

    const std::array<Effect, 4> effects{{
        {settings::key::weapon_lighting,
         settings::key::weapon_lighting_intensity,
         &settings::EngineSettings::weapon_lighting},
        {settings::key::nanolathe_lighting,
         settings::key::nanolathe_lighting_intensity,
         &settings::EngineSettings::nanolathe_lighting},
        {settings::key::wreck_glow,
         settings::key::wreck_glow_intensity,
         &settings::EngineSettings::wreck_glow},
        {settings::key::heat_distortion,
         settings::key::heat_distortion_intensity,
         &settings::EngineSettings::heat_distortion},
    }};
    for (const auto& [legacy, key, field] : effects) {
        CHECK(defaults.*field == 100);
        CHECK(read_one(legacy, "0").*field == 0);
        CHECK(read_one(legacy, "1").*field == 100);
        CHECK(read_one(key, "invalid").*field == 100);
        CHECK(read_one(key, "-10").*field == 0);
        CHECK(read_one(key, "999").*field == 200);
        CHECK(read_one(key, "143").*field == 140);
        CHECK(read_one(key, "146").*field == 150);
        Values values{{std::string{legacy}, "0"}, {std::string{key}, "150"}};
        auto opened = settings::read_settings(values, players_own_on_linux, false);
        CHECK(opened.*field == 150);
        auto chosen = opened;
        chosen.*field = 200;
        settings::write_settings(values, opened, chosen, defaults, false);
        CHECK(values.size() == 1);
        CHECK(values.at(std::string{key}) == "200");
        CHECK(settings::read_settings(values, players_own_on_linux, false) == chosen);
        settings::write_settings(values, chosen, defaults, defaults, true);
        CHECK(values.empty());
        values = {{std::string{legacy}, "0"}};
        opened = settings::read_settings(values, players_own_on_linux, false);
        settings::write_settings(values, opened, defaults, defaults, false);
        CHECK(settings::read_settings(values, players_own_on_linux, false).*field == 100);
    }
    CHECK(defaults.allow_full_3d);
    CHECK(!defaults.unlocked_camera);
    auto chosen = defaults;
    chosen.allow_full_3d = false;
    chosen.unlocked_camera = true;
    Values values;
    settings::write_settings(values, defaults, chosen, defaults, false);
    CHECK(settings::read_settings(values, players_own_on_linux, false) == chosen);
    settings::write_settings(values, chosen, defaults, defaults, true);
    CHECK(values.empty());
}

/// Water effect intensity persists independently, clamps and restores defaults.
void water_effects_round_trip() {
    const auto defaults = settings::default_settings(players_own_on_linux);

    struct Effect {
        std::string_view key;
        uint32_t settings::EngineSettings::* field;
    };

    const std::array<Effect, 4> effects{{
        {settings::key::water_ripples_intensity, &settings::EngineSettings::water_ripples},
        {settings::key::coastal_waves_intensity, &settings::EngineSettings::coastal_waves},
        {settings::key::underwater_shimmer_intensity,
         &settings::EngineSettings::underwater_shimmer},
        {settings::key::naval_reflections_intensity, &settings::EngineSettings::naval_reflections},
    }};
    for (const auto& [key, field] : effects) {
        CHECK(defaults.*field == 100);
        CHECK(read_one(key, "invalid").*field == 100);
        CHECK(read_one(key, "-10").*field == 0);
        CHECK(read_one(key, "999").*field == 200);
        CHECK(read_one(key, "143").*field == 140);
        CHECK(read_one(key, "146").*field == 150);
        Values values;
        auto chosen = defaults;
        chosen.*field = 0;
        settings::write_settings(values, defaults, chosen, defaults, false);
        CHECK(values.size() == 1);
        CHECK(settings::read_settings(values, players_own_on_linux, false) == chosen);
        settings::write_settings(values, chosen, defaults, defaults, true);
        CHECK(values.empty());
    }
}

/// Smooth motion: On everywhere by default, a light machine and a Steam
/// Deck included; a file written before it reads On; a number above 0 reads
/// On and any other number Off; Off written alone and read back, On chosen
/// again written as 1, and Restore defaults then OK erases the key. It
/// leaves every other key as it was.
void the_smooth_motion_default_read_and_round_trip() {
    CHECK(settings::key::smooth_motion == "open-annihilation.smooth-motion");
    settings::Inputs own_mac = players_own_on_linux;
    own_mac.macos = true;
    settings::Inputs light = players_own_on_linux;
    light.light_machine = true;
    settings::Inputs deck = players_own_on_linux;
    deck.steam_deck_panel_hz = 90;
    for (const auto& inputs : {settings::Inputs{}, players_own_on_linux, own_mac, light, deck}) {
        const auto defaults = settings::default_settings(inputs);
        CHECK(defaults.smooth_motion);
        CHECK(settings::read_settings({}, inputs, false) == defaults);
    }
    CHECK(read_one(settings::key::smooth_motion, "1").smooth_motion);
    CHECK(read_one(settings::key::smooth_motion, "2").smooth_motion);
    CHECK(!read_one(settings::key::smooth_motion, "0").smooth_motion);
    CHECK(!read_one(settings::key::smooth_motion, "-1").smooth_motion);
    // A text that is no number keeps the default.
    CHECK(read_one(settings::key::smooth_motion, "off").smooth_motion);

    const auto defaults = settings::default_settings(players_own_on_linux);
    auto off = defaults;
    off.smooth_motion = false;
    Values values;
    values[std::string{settings::key::explosion_flash}] = "reduced";
    values["open-annihilation.not-a-setting"] = "kept";
    const Values others = values;
    const auto opened = settings::read_settings(values, players_own_on_linux, false);
    auto chosen = opened;
    chosen.smooth_motion = false;
    settings::write_settings(values, opened, chosen, defaults, false);
    CHECK(values.size() == others.size() + 1);
    for (const auto& [key, value] : others)
        CHECK(values.contains(key) && values.at(key) == value);
    CHECK(values.at(std::string{settings::key::smooth_motion}) == "0");
    CHECK(!settings::read_settings(values, players_own_on_linux, false).smooth_motion);
    settings::write_settings(values, chosen, opened, defaults, false);
    CHECK(values.at(std::string{settings::key::smooth_motion}) == "1");
    CHECK(settings::read_settings(values, players_own_on_linux, false).smooth_motion);

    Values alone;
    settings::write_settings(alone, defaults, off, defaults, false);
    CHECK(alone.size() == 1);
    CHECK(alone.at(std::string{settings::key::smooth_motion}) == "0");
    CHECK(settings::read_settings(alone, players_own_on_linux, false) == off);
    settings::write_settings(alone, off, defaults, defaults, true);
    CHECK(alone.empty());
}

/// Interface size: Auto everywhere by default, with the player's own file
/// and with a named one, a file written before it included; each of its
/// six words reads its size and round-trips alone, 150 while Use modern
/// fonts for game text reads On; any other text, 0 among them, reads Auto;
/// writing it leaves every other key as it is, and Restore defaults then OK
/// erases its key.
void the_interface_size_default_read_and_round_trip() {
    using settings::InterfaceSize;
    CHECK(settings::key::interface_size == "open-annihilation.interface-size");
    settings::Inputs own_mac = players_own_on_linux;
    own_mac.macos = true;
    settings::Inputs light = players_own_on_linux;
    light.light_machine = true;
    settings::Inputs deck = players_own_on_linux;
    deck.steam_deck_panel_hz = 90;
    for (const auto& inputs : {settings::Inputs{}, players_own_on_linux, own_mac, light, deck}) {
        const auto defaults = settings::default_settings(inputs);
        CHECK(defaults.interface_size == InterfaceSize::automatic);
        CHECK(settings::read_settings({}, inputs, false) == defaults);
    }
    CHECK(settings::EngineSettings{}.interface_size == InterfaceSize::automatic);

    // The dialog's order, each size's word and percent, and back; 150 with
    // the modern fonts On.
    CHECK(
        settings::interface_size_choices == (std::array<InterfaceSize, 6>{
                                                InterfaceSize::automatic,
                                                InterfaceSize::size_100,
                                                InterfaceSize::size_150,
                                                InterfaceSize::size_200,
                                                InterfaceSize::size_300,
                                                InterfaceSize::size_400,
                                            })
    );
    constexpr std::array<std::string_view, 6> words{"automatic", "100", "150", "200", "300", "400"};
    constexpr std::array<int32_t, 6> percents{0, 100, 150, 200, 300, 400};
    for (std::size_t index = 0; index < words.size(); ++index) {
        const InterfaceSize size = settings::interface_size_choices[index];
        CHECK(settings::interface_size_text(size) == words[index]);
        CHECK(settings::interface_size_from_text(words[index]) == size);
        CHECK(settings::interface_size_percent(size) == percents[index]);
        Values stored = one_key(settings::key::interface_size, std::string{words[index]});
        stored[std::string{settings::key::modern_fonts}] = "1";
        CHECK(settings::read_settings(stored, {}, false).interface_size == size);
    }

    // 150% is drawn in the modern fonts: a stored 150 read while they read
    // Off, from the key or from a named file's default, is 100%; every
    // other size reads as itself with them Off.
    CHECK(
        settings::interface_size_with_fonts(InterfaceSize::size_150, false) ==
        InterfaceSize::size_100
    );
    CHECK(
        settings::interface_size_with_fonts(InterfaceSize::size_150, true) ==
        InterfaceSize::size_150
    );
    for (const InterfaceSize size : settings::interface_size_choices)
        if (size != InterfaceSize::size_150)
            CHECK(settings::interface_size_with_fonts(size, false) == size);
    CHECK(read_one(settings::key::interface_size, "150").interface_size == InterfaceSize::size_100);
    {
        Values off = one_key(settings::key::interface_size, "150");
        off[std::string{settings::key::modern_fonts}] = "0";
        CHECK(
            settings::read_settings(off, players_own_on_linux, false).interface_size ==
            InterfaceSize::size_100
        );
        // The player's own file has the modern fonts On by default.
        CHECK(
            settings::read_settings(
                one_key(settings::key::interface_size, "150"), players_own_on_linux, false
            )
                .interface_size == InterfaceSize::size_150
        );
        Values on = off;
        on[std::string{settings::key::modern_fonts}] = "1";
        CHECK(settings::read_settings(on, {}, false).interface_size == InterfaceSize::size_150);
    }

    // Any other text reads Auto: 0 is no size.
    const auto size_of = [](const char* text) {
        return read_one(settings::key::interface_size, text).interface_size;
    };
    for (const char* text :
         {"0",
          "",
          "Auto",
          "auto",
          "Automatic",
          "100%",
          " 100",
          "100 ",
          "1",
          "2",
          "500",
          "-100",
          "1000",
          "0100",
          "1/2"}) {
        CHECK(!settings::interface_size_from_text(text));
        CHECK(size_of(text) == InterfaceSize::automatic);
    }
    auto expected = settings::read_settings({}, {}, false);
    expected.interface_size = InterfaceSize::size_300;
    CHECK(read_one(settings::key::interface_size, "300") == expected);

    // Each size chosen alone writes its word alone and reads back; Auto
    // chosen again by hand is written as its word; Restore defaults then OK
    // erases the key.
    const auto defaults = settings::default_settings(players_own_on_linux);
    for (const InterfaceSize size : settings::interface_size_choices) {
        if (size == InterfaceSize::automatic)
            continue;
        auto chosen = defaults;
        chosen.interface_size = size;
        Values values;
        settings::write_settings(values, defaults, chosen, defaults, false);
        CHECK(values.size() == 1);
        CHECK(
            values.at(std::string{settings::key::interface_size}) ==
            settings::interface_size_text(size)
        );
        CHECK(settings::read_settings(values, players_own_on_linux, false) == chosen);
        settings::write_settings(values, chosen, defaults, defaults, false);
        CHECK(values.at(std::string{settings::key::interface_size}) == "automatic");
        CHECK(settings::read_settings(values, players_own_on_linux, false) == defaults);
        settings::write_settings(values, defaults, chosen, defaults, false);
        settings::write_settings(values, chosen, defaults, defaults, true);
        CHECK(values.empty());
    }

    // Writing it leaves every other key as it was: another setting's, a
    // key of the game's own and one the settings do not know.
    Values values;
    values[std::string{settings::key::hud_scaling}] = "0";
    values[std::string{settings::key::menu_scaling}] = "unfiltered";
    values[std::string{settings::key::text_size}] = "120";
    values["Total Annihilation|SwitchAlt"] = "1";
    values["open-annihilation.not-a-setting"] = "kept";
    const Values others = values;
    const auto opened = settings::read_settings(values, players_own_on_linux, false);
    auto chosen = opened;
    chosen.interface_size = InterfaceSize::size_200;
    settings::write_settings(values, opened, chosen, defaults, false);
    CHECK(values.size() == others.size() + 1);
    for (const auto& [key, value] : others)
        CHECK(values.contains(key) && values.at(key) == value);
    CHECK(values.at(std::string{settings::key::interface_size}) == "200");
    const auto read_back = settings::read_settings(values, players_own_on_linux, false);
    CHECK(read_back.interface_size == InterfaceSize::size_200);
    CHECK(!read_back.hud_scaling);
    CHECK(read_back.menu_scaling == settings::MenuScaling::unfiltered);
    CHECK(read_back.text_size == 120);
}

void menu_scaling_and_native_density_default_read_and_round_trip() {
    CHECK(settings::key::menu_scaling == "open-annihilation.menu-scaling");
    CHECK(settings::key::native_density == "open-annihilation.native-density");
    settings::Inputs own_mac = players_own_on_linux;
    own_mac.macos = true;
    settings::Inputs light = players_own_on_linux;
    light.light_machine = true;
    // Sharp and Off everywhere, a file without the keys alike.
    for (const auto& inputs : {settings::Inputs{}, players_own_on_linux, own_mac, light}) {
        const auto defaults = settings::default_settings(inputs);
        CHECK(defaults.menu_scaling == settings::MenuScaling::sharp);
        CHECK(!defaults.native_density);
        CHECK(settings::read_settings({}, inputs, false) == defaults);
    }
    // The words each way is kept as, and back; any other value gives Sharp.
    for (const auto scaling : settings::menu_scaling_choices)
        CHECK(settings::menu_scaling_from_text(settings::menu_scaling_text(scaling)) == scaling);
    CHECK(settings::menu_scaling_text(settings::MenuScaling::sharp) == "sharp");
    CHECK(settings::menu_scaling_text(settings::MenuScaling::whole_steps) == "whole-steps");
    CHECK(settings::menu_scaling_text(settings::MenuScaling::unfiltered) == "unfiltered");
    const auto scaling = [](const char* text) {
        return read_one(settings::key::menu_scaling, text).menu_scaling;
    };
    CHECK(scaling("whole-steps") == settings::MenuScaling::whole_steps);
    CHECK(scaling("unfiltered") == settings::MenuScaling::unfiltered);
    CHECK(scaling("sharp") == settings::MenuScaling::sharp);
    for (const char* text : {"", "Sharp", "whole_steps", "whole", "1", "2", "nearest", " sharp"})
        CHECK(scaling(text) == settings::MenuScaling::sharp);
    // The density reads as every switch does.
    CHECK(read_one(settings::key::native_density, "1").native_density);
    CHECK(read_one(settings::key::native_density, "4").native_density);
    CHECK(!read_one(settings::key::native_density, "0").native_density);
    CHECK(!read_one(settings::key::native_density, "on").native_density);
    auto expected = settings::read_settings({}, {}, false);
    expected.menu_scaling = settings::MenuScaling::unfiltered;
    CHECK(read_one(settings::key::menu_scaling, "unfiltered") == expected);

    // Each change alone writes its key alone and reads back; Restore
    // defaults then OK erases it, back to Sharp and Off.
    const auto defaults = settings::default_settings(players_own_on_linux);
    for (const auto chosen_scaling :
         {settings::MenuScaling::whole_steps, settings::MenuScaling::unfiltered}) {
        auto chosen = defaults;
        chosen.menu_scaling = chosen_scaling;
        Values values;
        settings::write_settings(values, defaults, chosen, defaults, false);
        CHECK(values.size() == 1);
        CHECK(
            values.at(std::string{settings::key::menu_scaling}) ==
            settings::menu_scaling_text(chosen_scaling)
        );
        CHECK(settings::read_settings(values, players_own_on_linux, false) == chosen);
        settings::write_settings(values, chosen, defaults, defaults, true);
        CHECK(values.empty());
        CHECK(
            settings::read_settings(values, players_own_on_linux, false).menu_scaling ==
            settings::MenuScaling::sharp
        );
    }
    auto dense = defaults;
    dense.native_density = true;
    Values values;
    settings::write_settings(values, defaults, dense, defaults, false);
    CHECK(values.size() == 1);
    CHECK(values.at(std::string{settings::key::native_density}) == "1");
    CHECK(settings::read_settings(values, players_own_on_linux, false) == dense);
    // Turned off by hand, the key stays, written as Off.
    settings::write_settings(values, dense, defaults, defaults, false);
    CHECK(values.at(std::string{settings::key::native_density}) == "0");
    CHECK(!settings::read_settings(values, players_own_on_linux, false).native_density);
    settings::write_settings(values, defaults, dense, defaults, false);
    settings::write_settings(values, dense, defaults, defaults, true);
    CHECK(values.empty());

    // Where the platform opens every window at native density it is On, as
    // stored and by default, and a stored Off does not turn it off.
    settings::Inputs dense_platform = players_own_on_linux;
    dense_platform.native_density_windows = true;
    const auto platform_defaults = settings::default_settings(dense_platform);
    CHECK(platform_defaults.native_density);
    CHECK(settings::read_settings({}, dense_platform, false).native_density);
    Values off;
    off[std::string{settings::key::native_density}] = "0";
    CHECK(settings::read_settings(off, dense_platform, false).native_density);
    // Unchanged, nothing is written; Restore defaults erases the key.
    Values platform_values;
    settings::write_settings(
        platform_values, platform_defaults, platform_defaults, platform_defaults, false
    );
    CHECK(platform_values.empty());
    settings::write_settings(off, platform_defaults, platform_defaults, platform_defaults, true);
    CHECK(!off.contains(std::string{settings::key::native_density}));

    // The locks: the platform's before the command line's; no game locks
    // either row.
    using settings::Lock;
    settings::GameState state{};
    CHECK(settings::settings_locks(state).native_density == Lock::none);
    state.in_game = true;
    state.shared_game = true;
    CHECK(settings::settings_locks(state).native_density == Lock::none);
    state.native_density_from_command_line = true;
    CHECK(settings::settings_locks(state).native_density == Lock::command_line);
    state.native_density_windows = true;
    CHECK(settings::settings_locks(state).native_density == Lock::always_on);
    state.native_density_from_command_line = false;
    CHECK(settings::settings_locks(state).native_density == Lock::always_on);
}

int main() {
    defaults_play_as_without_the_settings();
    escape_opens_the_menu_by_default_only_on_macos_with_the_players_own_file();
    the_unit_limit_defaults_to_the_installations_with_the_players_own_file();
    a_raspberry_pi_starts_at_60_frames_without_anti_aliasing();
    a_light_machine_starts_at_800x600_and_60_frames_without_anti_aliasing();
    screen_sizes_are_stored_as_text();
    totala_ini_gives_its_unit_limit_as_the_game_reads_it();
    absent_keys_and_values_that_are_not_numbers_give_the_defaults();
    stored_values_are_read_and_clamped_into_their_ranges();
    switch_alt_comes_from_the_games_own_key();
    only_changed_settings_are_written();
    file_type_switches_store_on_and_off();
    the_launch_file_switch_round_trips();
    restore_defaults_erases_the_keys_of_settings_at_their_defaults();
    the_round_trip_keeps_every_value();
    the_frame_rate_goes_down_to_a_frame_a_tick();
    the_path_credit_shows_as_whole_cycles();
    shared_games_and_replays_search_at_one_cycle();
    a_mods_unit_limits_set_the_range_and_the_default();
    a_mods_path_budget_scales_the_credit();
    the_skirmish_player_count_is_not_an_engine_key();
    a_game_locks_the_next_game_settings();
    the_mod_is_stored_as_its_folder();
    a_picked_folder_is_kept_while_it_is_played();
    an_older_preferences_file_reads_no_mod();
    a_folder_without_a_profile_keeps_its_overrides_under_its_own_id();
    hardware_acceleration_defaults_to_full_for_the_players_own_file_on_every_machine();
    hardware_acceleration_reads_its_words_and_the_switchs_numbers();
    the_renderer_settings_lock_by_the_flags_and_the_renderer();
    language_and_text_defaults_to_modern_fonts_with_outline_and_shadow();
    a_file_without_the_text_switches_reads_their_defaults();
    the_text_switches_round_trip_and_restore();
    the_text_size_reads_writes_and_restores_in_its_range();
    the_language_reads_writes_and_restores();
    an_available_language_is_kept_until_its_pack_is_installed();
    a_preferences_file_with_crlf_line_ends_reads_its_settings();
    developer_mode_and_its_overrides_are_kept_under_the_profiles_id();
    the_touch_settings_default_alike_everywhere();
    the_touch_settings_read_their_words_and_drop_others();
    the_hold_delay_is_held_to_its_range_and_stops();
    the_touch_settings_round_trip_and_restore();
    the_backups_switch_is_off_by_default_and_round_trips();
    a_steam_deck_starts_at_its_screens_rate_with_larger_controls();
    the_controller_settings_default_alike_everywhere();
    the_controller_settings_read_their_words_and_drop_others();
    the_controller_speeds_are_held_to_their_ranges_and_stops();
    the_controller_settings_round_trip_and_restore();
    menu_scaling_and_native_density_default_read_and_round_trip();
    explosion_flash_default_read_and_round_trip();
    the_zoomed_out_units_default_read_and_round_trip();
    the_window_edge_default_read_and_round_trip();
    the_edge_stickiness_default_read_and_round_trip();
    the_window_frame_default_read_and_round_trip();
    the_hud_scaling_default_read_and_round_trip();
    the_smooth_motion_default_read_and_round_trip();
    visual_effect_switches_round_trip();
    water_effects_round_trip();
    the_interface_size_default_read_and_round_trip();
    the_zoom_limits_default_read_and_round_trip();
    the_view_past_the_map_edge_defaults_reads_and_round_trips();
    if (failures != 0)
        return 1;
    std::cout << "engine settings: ok\n";
    return 0;
}
