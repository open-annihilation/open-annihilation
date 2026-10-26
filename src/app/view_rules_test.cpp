// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A profile's display rules as the records and decisions of the modules
// that carry them out: 3.1c's without rules, and each parameter on its own.
#include "oa/app/view_rules.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <functional>
#include <iostream>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

using oa::app::view_rules::match_display_rules;
using oa::data::mod_profile::UiRules;

void match_display_rules_follow_the_profile() {
    const UiRules base{};
    OA_CHECK(match_display_rules(base) == oa::sim::match_runtime::DisplayRules{});
    const auto plain = match_display_rules(base);
    OA_CHECK(!plain.reclaim_voice_once && !plain.vtol_reclaim_voice_at_start);
    OA_CHECK(plain.landing_fail_voice == 7 && !plain.end_smoke_explosion);
    OA_CHECK(plain.explosion_smoke_column);

    UiRules voices{};
    voices.unit_voice_fixes.enabled = true;
    voices.unit_voice_fixes.reclaim_voice_once = true;
    OA_CHECK(match_display_rules(voices).reclaim_voice_once);
    OA_CHECK(!match_display_rules(voices).vtol_reclaim_voice_at_start);
    voices.unit_voice_fixes.vtol_reclaim_voice_at_start = true;
    OA_CHECK(match_display_rules(voices).vtol_reclaim_voice_at_start);
    voices.unit_voice_fixes.landing_fail_voice = 6;
    OA_CHECK(match_display_rules(voices).landing_fail_voice == 6);
    voices.unit_voice_fixes.landing_fail_voice = 255;
    OA_CHECK(match_display_rules(voices).landing_fail_voice == 255);

    UiRules effects{};
    effects.effects_tweaks.enabled = true;
    effects.effects_tweaks.endsmoke_explosion = true;
    OA_CHECK(match_display_rules(effects).end_smoke_explosion);
    OA_CHECK(match_display_rules(effects).explosion_smoke_column);
    effects.effects_tweaks.explosion_smoke_puff = false;
    OA_CHECK(!match_display_rules(effects).explosion_smoke_column);

    // The external exports change nothing the game shows or plays.
    UiRules exports{};
    exports.external_exports.enabled = true;
    exports.external_exports.live_export = true;
    exports.external_exports.lobby_state_export = true;
    OA_CHECK(match_display_rules(exports) == plain);
    OA_CHECK(oa::app::view_rules::music_source(exports) == oa::app::view_rules::music_source(base));
    OA_CHECK(
        oa::app::view_rules::minimum_mode_height(exports) ==
        oa::app::view_rules::minimum_mode_height(base)
    );
}

/// The explosions' flashes are drawn at the lower of the player's
/// Explosion flash setting and the level the profile designs them at
/// (ui.explosion-flash): the player can always reduce them or turn them
/// off, and a profile can reduce them but never draw them above the
/// player's choice. Without the hack the player's choice is drawn, whatever
/// its parameter holds, and the hack never reaches the match's rules.
void explosion_flash_follows_the_lower_level() {
    using oa::app::view_rules::explosion_flash_drawn;
    using oa::data::mod_profile::UiExplosionFlashLevel;
    using oa::ui::engine_settings::ExplosionFlash;
    constexpr std::array<ExplosionFlash, 3> players{
        ExplosionFlash::off, ExplosionFlash::reduced, ExplosionFlash::full
    };
    const UiRules base{};
    for (const ExplosionFlash player : players)
        OA_CHECK(explosion_flash_drawn(base, player) == player);
    UiRules unset = base;
    unset.explosion_flash.level = UiExplosionFlashLevel::off;
    for (const ExplosionFlash player : players)
        OA_CHECK(explosion_flash_drawn(unset, player) == player);

    // Each level of the profile against each of the player's: the lower.
    struct Case {
        UiExplosionFlashLevel designed{};
        ExplosionFlash player{};
        ExplosionFlash drawn{};
    };

    constexpr std::array<Case, 9> cases{{
        {UiExplosionFlashLevel::full, ExplosionFlash::full, ExplosionFlash::full},
        {UiExplosionFlashLevel::full, ExplosionFlash::reduced, ExplosionFlash::reduced},
        {UiExplosionFlashLevel::full, ExplosionFlash::off, ExplosionFlash::off},
        {UiExplosionFlashLevel::reduced, ExplosionFlash::full, ExplosionFlash::reduced},
        {UiExplosionFlashLevel::reduced, ExplosionFlash::reduced, ExplosionFlash::reduced},
        {UiExplosionFlashLevel::reduced, ExplosionFlash::off, ExplosionFlash::off},
        {UiExplosionFlashLevel::off, ExplosionFlash::full, ExplosionFlash::off},
        {UiExplosionFlashLevel::off, ExplosionFlash::reduced, ExplosionFlash::off},
        {UiExplosionFlashLevel::off, ExplosionFlash::off, ExplosionFlash::off},
    }};
    for (const Case& entry : cases) {
        UiRules designed = base;
        designed.explosion_flash.enabled = true;
        designed.explosion_flash.level = entry.designed;
        OA_CHECK(explosion_flash_drawn(designed, entry.player) == entry.drawn);
        OA_CHECK(match_display_rules(designed) == match_display_rules(base));
    }
}

void victory_announcement_gate() {
    using oa::app::view_rules::victory_announcement_due;
    uint32_t last = 0;
    OA_CHECK(!victory_announcement_due(300, last) && last == 300);
    OA_CHECK(!victory_announcement_due(300, last));
    OA_CHECK(!victory_announcement_due(600, last) && last == 600);
    OA_CHECK(victory_announcement_due(901, last) && last == 901);
    OA_CHECK(!victory_announcement_due(901, last));
    // A later match whose banner shows at an earlier tick.
    OA_CHECK(victory_announcement_due(500, last) && last == 500);
    last = 0;
    OA_CHECK(victory_announcement_due(301, last));
}

void music_source_follows_the_profile() {
    using oa::app::view_rules::music_source;
    using oa::app::view_rules::MusicSource;
    using oa::data::mod_profile::UiAudioMusic;
    UiRules ui{};
    OA_CHECK(music_source(ui) == MusicSource::disc);
    ui.audio.enabled = true;
    ui.audio.music = UiAudioMusic::numbered_mp3;
    OA_CHECK(music_source(ui) == MusicSource::numbered_mp3);
    ui.audio.music = UiAudioMusic::folder_scan;
    OA_CHECK(music_source(ui) == MusicSource::folder_scan);
    ui.audio.music = UiAudioMusic::cd;
    OA_CHECK(music_source(ui) == MusicSource::disc);
}

void display_modes_follow_the_profile() {
    using oa::app::view_rules::display_mode_setting;
    using oa::app::view_rules::minimum_mode_height;
    namespace init = oa::ui::frontend_state::initialization;
    UiRules ui{};
    OA_CHECK(minimum_mode_height(ui) == 480);
    OA_CHECK(!display_mode_setting(ui).raise_smaller && display_mode_setting(ui).width == 640);
    ui.display_modes.enabled = true;
    ui.display_modes.min_height_768 = true;
    OA_CHECK(minimum_mode_height(ui) == 768);
    const auto tall = display_mode_setting(ui);
    OA_CHECK(tall.raise_smaller && tall.width == 1024 && tall.height == 768);
    // The graphics-driver warning: the engine never shows one, so either
    // value plays the same.
    ui.display_modes.dx_warning = false;
    OA_CHECK(minimum_mode_height(ui) == 768);
    ui.display_modes.min_height_768 = false;
    OA_CHECK(display_mode_setting(ui).width == init::base_display_mode_setting.width);
}

void screenshot_names() {
    using oa::app::view_rules::screenshot_file_name;
    using oa::app::view_rules::screenshot_safe_text;
    using oa::app::view_rules::ScreenshotScene;
    OA_CHECK(screenshot_safe_text("a\\b/c:d*e?f\"g<h>i|j") == "a_b_c_d_e_f_g_h_i_j");
    OA_CHECK(screenshot_file_name("10/03/26 - ", nullptr, 0) == "10_03_26 - SHOT0000.pcx");
    OA_CHECK(screenshot_file_name("10/03/26 - ", nullptr, 12345) == "10_03_26 - SHOT12345.pcx");
    ScreenshotScene scene{};
    scene.map = "Coast To Coast";
    scene.players[0] = "Red";
    scene.players[2] = "Blue?";
    OA_CHECK(
        screenshot_file_name("10/03/26 - ", &scene, 7) ==
        "10_03_26 - Coast To Coast - Red, Blue_ 0007.pcx"
    );
    scene.players[0].clear();
    OA_CHECK(
        screenshot_file_name("10/03/26 - ", &scene, 7) ==
        "10_03_26 - Coast To Coast - , Blue_ 0007.pcx"
    );
}

void click_snap_search() {
    namespace vr = oa::app::view_rules;
    OA_CHECK(vr::click_snap_radius(3, 0) == 0);
    OA_CHECK(vr::click_snap_radius(3, 1) == 1);
    OA_CHECK(vr::click_snap_radius(-2, 5) == 0);
    OA_CHECK(vr::click_snap_radius(9, 12) == 9);
    OA_CHECK(vr::footprint_first_offset(1) == 0 && vr::footprint_last_offset(1) == 0);
    OA_CHECK(vr::footprint_first_offset(2) == -1 && vr::footprint_last_offset(2) == 0);
    OA_CHECK(vr::footprint_first_offset(3) == -1 && vr::footprint_last_offset(3) == 1);
    OA_CHECK(vr::footprint_first_offset(4) == -2 && vr::footprint_last_offset(4) == 1);
    OA_CHECK(vr::footprint_last_offset(0) < vr::footprint_first_offset(0));

    // Nothing scores: no snap.
    const auto none = [](int32_t, int32_t) { return 0; };
    OA_CHECK(!vr::snap_cell({10, 10}, 2, 168.0, 168.0, none));
    // The highest score wins over a nearer lower one.
    const auto scored = [](int32_t x, int32_t z) {
        if (x == 11 && z == 10)
            return 1;
        if (x == 8 && z == 12)
            return 3;
        return 0;
    };
    const auto best = vr::snap_cell({10, 10}, 2, 168.0, 168.0, scored);
    OA_CHECK(best && (*best)[0] == 8 && (*best)[1] == 12 && (*best)[2] == 3);
    // Out of the radius, a cell is not looked at.
    OA_CHECK((*vr::snap_cell({10, 10}, 1, 168.0, 168.0, scored))[0] == 11);
    // Equal scores: the cell whose middle is nearest the cursor.
    const auto both = [](int32_t x, int32_t z) { return (x == 9 || x == 11) && z == 10 ? 1 : 0; };
    // Cursor at pixel 180 (cell 11.25): cell 11's middle (11.5) is nearer.
    OA_CHECK((*vr::snap_cell({10, 10}, 1, 180.0, 168.0, both))[0] == 11);
    // Cursor at pixel 152 (cell 9.5): cell 9's middle is nearer.
    OA_CHECK((*vr::snap_cell({10, 10}, 1, 152.0, 168.0, both))[0] == 9);
    // Cursor at pixel 168 (cell 10.5): both lie one cell off; the first
    // taken, from the most negative offset, stays.
    OA_CHECK((*vr::snap_cell({10, 10}, 1, 168.0, 168.0, both))[0] == 9);

    const std::array<uint8_t, 4> geo{0x35, 0x8f, 0x35, 0x35};
    OA_CHECK(vr::yard_has_geothermal_cell(geo));
    const std::array<uint8_t, 4> open_first{0x00, 0x8f, 0x35, 0x35};
    OA_CHECK(!vr::yard_has_geothermal_cell(open_first));
    const std::array<uint8_t, 2> plain{0x35, 0x35};
    OA_CHECK(!vr::yard_has_geothermal_cell(plain));
    std::array<uint8_t, 70> long_yard{};
    long_yard.fill(0x35);
    long_yard[65] = 0x8f;
    OA_CHECK(!vr::yard_has_geothermal_cell(long_yard));
    long_yard[63] = 0x8f;
    OA_CHECK(vr::yard_has_geothermal_cell(long_yard));
}

void view_settings_round_trip() {
    namespace vr = oa::app::view_rules;
    using oa::data::match_rules::OrdersConPatrolGuardOptions;
    UiRules ui{};
    ui.click_snap.enabled = true;
    ui.click_snap.mex_default = 3;
    ui.click_snap.mex_max = 3;
    ui.click_snap.wreck_default = 1;
    ui.click_snap.wreck_max = 1;
    ui.text_rendering.chat_backdrop = true;
    OrdersConPatrolGuardOptions builders{};
    const auto fresh = vr::read_view_settings(ui, builders, {});
    OA_CHECK(fresh.mex_snap_radius == 3 && fresh.wreck_snap_radius == 1);
    OA_CHECK(fresh.chat_macro == vr::default_chat_macro && fresh.chat_backdrop);
    OA_CHECK(fresh.optimize_dt_rows && fresh.full_rings && !fresh.vsync);
    OA_CHECK(fresh.panel_background == vr::PanelBackground::text);
    OA_CHECK(fresh.patrol[0] == vr::PatrolOption::both && fresh.guard[2] == vr::GuardOption::base);

    std::map<std::string, uint32_t> numbers;
    std::map<std::string, std::string> texts;
    auto changed = fresh;
    changed.mex_snap_radius = 2;
    changed.chat_macro = "+shootall";
    changed.patrol[0] = vr::PatrolOption::assist_only;
    changed.guard[1] = vr::GuardOption::scatter;
    changed.full_rings = false;
    vr::write_view_settings(
        changed,
        ui,
        [&](std::string_view name, uint32_t value) { numbers[std::string(name)] = value; },
        [&](std::string_view name, std::string_view value) {
            texts[std::string(name)] = std::string(value);
        }
    );
    vr::ViewSettingsStore store{};
    store.number = [&](std::string_view name) -> std::optional<uint32_t> {
        const auto found = numbers.find(std::string(name));
        return found == numbers.end() ? std::nullopt : std::optional<uint32_t>(found->second);
    };
    store.text = [&](std::string_view name) -> std::optional<std::string> {
        const auto found = texts.find(std::string(name));
        return found == texts.end() ? std::nullopt : std::optional<std::string>(found->second);
    };
    const auto read = vr::read_view_settings(ui, builders, store);
    OA_CHECK(numbers.count("ClickSnapOverrideKey") == 0);
    OA_CHECK(numbers.count("KeyCode") == 0);
    OA_CHECK(numbers.count("WhiteboardKey") == 0);
    OA_CHECK(numbers.count("MegamapKey") == 0);
    OA_CHECK(numbers.count("RotateBuildKey") == 0);
    OA_CHECK(read.mex_snap_radius == 2 && read.chat_macro == "+shootall" && !read.full_rings);
    OA_CHECK(read.patrol[0] == vr::PatrolOption::assist_only);
    OA_CHECK(read.guard[1] == vr::GuardOption::scatter);
    // A stored radius above the profile's maximum is held to it.
    numbers["MexSnapRadius"] = 9;
    OA_CHECK(vr::read_view_settings(ui, builders, store).mex_snap_radius == 3);
    ui.click_snap.mex_max = 0;
    OA_CHECK(vr::read_view_settings(ui, builders, store).mex_snap_radius == 0);

    // The builders' options reach a match only while the profile has them on.
    OrdersConPatrolGuardOptions rules{};
    vr::apply_builder_options(read, rules);
    OA_CHECK(rules == OrdersConPatrolGuardOptions{});
    rules.enabled = true;
    vr::apply_builder_options(read, rules);
    OA_CHECK(
        rules.patrol_hold_position ==
        oa::data::match_rules::OrdersConPatrolGuardOptionsPatrolHoldPosition::assist_only
    );
    OA_CHECK(
        rules.guard_maneuver ==
        oa::data::match_rules::OrdersConPatrolGuardOptionsGuardManeuver::scatter
    );
}

/// A snap radius the profile gives the player no room to choose (ui.click-snap
/// off, or that radius's maximum 0) is not stored as the player's choice
/// when the settings are written: once the profile gives it room, its
/// default plays, not a 0 written while it had none.
void unchosen_snap_radius_not_stored() {
    namespace vr = oa::app::view_rules;
    using oa::data::match_rules::OrdersConPatrolGuardOptions;
    const OrdersConPatrolGuardOptions builders{};
    std::map<std::string, uint32_t> numbers;
    vr::ViewSettingsStore store{};
    store.number = [&](std::string_view name) -> std::optional<uint32_t> {
        const auto found = numbers.find(std::string(name));
        return found == numbers.end() ? std::nullopt : std::optional<uint32_t>(found->second);
    };
    const auto write = [&](const vr::ViewSettings& settings, const UiRules& ui) {
        vr::write_view_settings(
            settings,
            ui,
            [&](std::string_view name, uint32_t value) { numbers[std::string(name)] = value; },
            [](std::string_view, std::string_view) {}
        );
    };
    // With ui.click-snap off, OK in the options dialog writes neither radius.
    const UiRules off{};
    auto settings = vr::read_view_settings(off, builders, store);
    OA_CHECK(settings.mex_snap_radius == 0 && settings.wreck_snap_radius == 0);
    vr::apply_dialog_options(vr::dialog_options(settings, off), off, settings);
    write(settings, off);
    OA_CHECK(numbers.count("MexSnapRadius") == 0 && numbers.count("WreckSnapRadius") == 0);
    // Turned on with its defaults the wreck radius is 1, and the mex radius
    // has no room and is not written.
    UiRules on{};
    on.click_snap.enabled = true;
    on.click_snap.wreck_default = 1;
    on.click_snap.wreck_max = 1;
    settings = vr::read_view_settings(on, builders, store);
    OA_CHECK(settings.wreck_snap_radius == 1 && settings.mex_snap_radius == 0);
    write(settings, on);
    OA_CHECK(numbers.count("WreckSnapRadius") == 1 && numbers.at("WreckSnapRadius") == 1);
    OA_CHECK(numbers.count("MexSnapRadius") == 0);
    // Given room later, the mex radius starts at the profile's default.
    on.click_snap.mex_default = 4;
    on.click_snap.mex_max = 4;
    OA_CHECK(vr::read_view_settings(on, builders, store).mex_snap_radius == 4);
    // A radius the player chose with room for it, 0 included, is kept.
    settings = vr::read_view_settings(on, builders, store);
    settings.wreck_snap_radius = 0;
    write(settings, on);
    OA_CHECK(vr::read_view_settings(on, builders, store).wreck_snap_radius == 0);
}

void build_tool_layouts() {
    namespace vr = oa::app::view_rules;
    using Slots = std::vector<vr::BuildSlot>;
    // A short drag lays the start alone; z wins a tie.
    auto still = vr::line_build_slots(100, 100, 110, 105, 2, 2, 0);
    OA_CHECK(still && still->axis == vr::LineAxis::down && still->slots == (Slots{{100, 100}}));
    // Across: 10 cells right, 2x2 buildings with no spacing: 5 steps of 32
    // pixels, 6 buildings, the row never moving.
    auto across = vr::line_build_slots(100, 100, 260, 110, 2, 2, 0);
    OA_CHECK(across && across->axis == vr::LineAxis::across);
    OA_CHECK(
        across->slots ==
        (Slots{{100, 100}, {132, 100}, {164, 100}, {196, 100}, {228, 100}, {260, 100}})
    );
    // Spacing 1 widens the step to 3 cells: 10 / 3 = 3 steps.
    auto spaced = vr::line_build_slots(100, 100, 260, 110, 2, 2, 1);
    OA_CHECK(spaced && spaced->slots.size() == 4 && spaced->slots[3] == (vr::BuildSlot{244, 100}));
    // Left and up: the cells count toward zero, the steps go negative.
    auto back = vr::line_build_slots(300, 300, 100, 260, 1, 1, 0);
    OA_CHECK(back && back->axis == vr::LineAxis::across && back->slots.size() == 13);
    OA_CHECK(back->slots[0] == (vr::BuildSlot{300, 300}) && back->slots[12].x == 300 - 12 * 16);
    // The two cells of z are spread over the 12 steps: z moves up by 16 at
    // the steps the straight line crosses a row.
    int moves = 0;
    for (std::size_t i = 1; i < back->slots.size(); ++i)
        if (back->slots[i].z != back->slots[i - 1].z) {
            OA_CHECK(back->slots[i].z == back->slots[i - 1].z - 16);
            ++moves;
        }
    OA_CHECK(moves == 2);
    // Down a diagonal: 4 cells each way, 2x2: 2 steps along z, x stepping
    // a cell at a time.
    auto diagonal = vr::line_build_slots(0, 0, 64, 64, 2, 2, 0);
    OA_CHECK(diagonal && diagonal->axis == vr::LineAxis::down);
    OA_CHECK(diagonal->slots == (Slots{{0, 0}, {32, 32}, {64, 64}}));
    // More than 999 steps lays nothing.
    OA_CHECK(!vr::line_build_slots(0, 0, 16 * 1000, 0, 1, 1, 0));
    OA_CHECK(vr::line_build_slots(0, 0, 16 * 999, 0, 1, 1, 0).has_value());

    // Staggered double row: a line down whose x moves every other step.
    vr::BuildLine teeth{};
    teeth.axis = vr::LineAxis::down;
    teeth.slots = {{0, 0}, {16, 32}, {16, 64}, {32, 96}, {32, 128}, {48, 160}};
    vr::optimize_dt_rows(teeth);
    // From the second: x 16 next to x 16 swap z; then from the fourth: x 32
    // next to x 32 swap z.
    OA_CHECK(teeth.slots == (Slots{{0, 0}, {16, 64}, {16, 32}, {32, 128}, {32, 96}, {48, 160}}));
    vr::BuildLine every_other{};
    every_other.axis = vr::LineAxis::across;
    every_other.slots = {{0, 0}, {32, 16}, {64, 0}, {96, 16}, {128, 0}};
    vr::optimize_dt_rows(every_other);
    OA_CHECK(every_other.slots == (Slots{{0, 0}, {96, 16}, {64, 0}, {32, 16}, {128, 0}}));
    vr::BuildLine short_line{};
    short_line.slots = {{0, 0}, {0, 32}, {0, 64}};
    vr::optimize_dt_rows(short_line);
    OA_CHECK(short_line.slots == (Slots{{0, 0}, {0, 32}, {0, 64}}));

    // A ring of 1x1 buildings around a 2x2 rectangle at (32, 32): three a
    // side (2 / 1 + 1), clockwise from the top left.
    const auto ring = vr::ring_build_slots(32, 32, 2, 2, 1, 1, false);
    OA_CHECK(ring.size() == 12);
    OA_CHECK(ring[0] == (vr::BuildSlot{40, 24}) && ring[2] == (vr::BuildSlot{72, 24}));
    OA_CHECK(ring[3] == (vr::BuildSlot{72, 40}) && ring[5] == (vr::BuildSlot{72, 72}));
    OA_CHECK(ring[6] == (vr::BuildSlot{56, 72}) && ring[8] == (vr::BuildSlot{24, 72}));
    OA_CHECK(ring[9] == (vr::BuildSlot{24, 56}) && ring[11] == (vr::BuildSlot{24, 24}));
    // 2x2 around a 3x3 rectangle: 3 / 2 + 1 = 2 a side; full rings close
    // the corners with one more.
    OA_CHECK(vr::ring_build_slots(0, 0, 3, 3, 2, 2, false).size() == 8);
    OA_CHECK(vr::ring_build_slots(0, 0, 3, 3, 2, 2, true).size() == 12);
    // A footprint of 3 never gets the extra place, nor a side it divides.
    OA_CHECK(vr::ring_build_slots(0, 0, 4, 4, 3, 3, true).size() == 8);
    OA_CHECK(vr::ring_build_slots(0, 0, 4, 4, 2, 2, true).size() == 12);
    // Off the top left the coordinates wrap at 16 bits, as the game keeps them.
    const auto edge = vr::ring_build_slots(0, 0, 1, 1, 1, 1, false);
    OA_CHECK(edge[0] == (vr::BuildSlot{8, -8}));
}

void build_facings() {
    namespace vr = oa::app::view_rules;
    namespace facing = oa::data::match_rules::build_facing;
    using vr::BuildFacing;
    OA_CHECK(vr::facing_allowed(0, BuildFacing::south));
    OA_CHECK(!vr::facing_allowed(facing::south, BuildFacing::east));
    const uint8_t all = facing::south | facing::east | facing::north | facing::west;
    OA_CHECK(vr::next_build_facing(all, BuildFacing::south, 1) == BuildFacing::east);
    OA_CHECK(vr::next_build_facing(all, BuildFacing::west, 1) == BuildFacing::south);
    OA_CHECK(vr::next_build_facing(all, BuildFacing::south, -1) == BuildFacing::west);
    // Facings the type lacks are stepped over.
    const uint8_t south_north = facing::south | facing::north;
    OA_CHECK(vr::next_build_facing(south_north, BuildFacing::south, 1) == BuildFacing::north);
    OA_CHECK(vr::next_build_facing(south_north, BuildFacing::north, 1) == BuildFacing::south);
    OA_CHECK(vr::next_build_facing(south_north, BuildFacing::north, -5) == BuildFacing::south);
    // One facing turns nothing.
    OA_CHECK(vr::next_build_facing(facing::south, BuildFacing::south, 1) == BuildFacing::south);
    // South is always allowed, even when the bits leave it out.
    OA_CHECK(vr::next_build_facing(facing::west, BuildFacing::south, 1) == BuildFacing::west);
    OA_CHECK(vr::facing_letter(BuildFacing::north) == 'N');
    OA_CHECK(vr::rotate_hint("/", "Alt") == "Press /, or Alt+wheel, to rotate");
}

void facing_toward_points() {
    namespace vr = oa::app::view_rules;
    using vr::BuildFacing;
    OA_CHECK(vr::facing_toward(5, 3) == BuildFacing::east);
    OA_CHECK(vr::facing_toward(-5, 3) == BuildFacing::west);
    OA_CHECK(vr::facing_toward(5, -5) == BuildFacing::north);
    OA_CHECK(vr::facing_toward(-3, 3) == BuildFacing::south);
    OA_CHECK(vr::facing_toward(0, 1) == BuildFacing::south);
    OA_CHECK(vr::facing_toward(0, 0) == BuildFacing::north);
}

// The opponent's facing of a build preview: the site within build distance
// of a selected, finished unit of the local player's, then toward the
// nearest first unit of a player who is no ally and no watcher.
void opponent_facings() {
    namespace vr = oa::app::view_rules;
    using vr::BuildFacing;
    oa::World* world = oa::world_create();
    const oa::WorldCapacity capacity{16, 4, 1};
    OA_CHECK(world != nullptr && oa::world_alloc_tables(world, &capacity) != 0);
    if (world == nullptr)
        return;
    world->game.local_player_index = 0;
    for (uint8_t index = 0; index < 3; ++index) {
        auto& player = world->game.players[index];
        player.index = index;
        player.in_use = 1;
        player.alliance[index] = 1;
        player.info = static_cast<oa::oa_ref32>(index + 1U);
        player.first_unit = oa::world_unit_ref(world, &world->units[1U + index * 4U]);
        player.last_unit = oa::world_unit_ref(world, &world->units[4U + index * 4U]);
    }
    world->unit_defs[1].build_distance = 100;
    const auto place = [&](uint32_t slot, uint8_t owner, int32_t x, int32_t z) -> oa::Unit& {
        auto& unit = world->units[slot];
        unit.movement = 1;
        unit.type_index = 1;
        unit.def = oa::oa_ref_from_index(1);
        unit.owner_index = owner;
        unit.position.x = x << 16;
        unit.position.z = z << 16;
        return unit;
    };
    auto& builder = place(1, 0, 100, 100);
    builder.flags = OA_UNIT_FLAG_SELECTED;
    place(5, 1, 300, 120);
    place(9, 2, 140, 0);
    // The nearer enemy lies north; allied, the farther one east is faced.
    OA_CHECK(vr::opponent_facing(*world, 150, 100) == BuildFacing::north);
    world->game.players[0].alliance[2] = 1;
    OA_CHECK(vr::opponent_facing(*world, 150, 100) == BuildFacing::east);
    // A watcher is no opponent.
    world->player_info[1].options = OA_SETUP_OPTION_WATCHER;
    OA_CHECK(!vr::opponent_facing(*world, 150, 100));
    world->player_info[1].options = 0;
    // Exactly at build distance counts; past it the preview is not turned.
    OA_CHECK(vr::opponent_facing(*world, 200, 100) == BuildFacing::east);
    OA_CHECK(!vr::opponent_facing(*world, 201, 100));
    // The builder must be selected and finished.
    builder.build_remaining = 0.5F;
    OA_CHECK(!vr::opponent_facing(*world, 150, 100));
    builder.build_remaining = 0.0F;
    builder.flags = 0;
    OA_CHECK(!vr::opponent_facing(*world, 150, 100));
    // The player's last unit is not counted.
    auto& last = place(4, 0, 100, 100);
    last.flags = OA_UNIT_FLAG_SELECTED;
    OA_CHECK(!vr::opponent_facing(*world, 150, 100));
    builder.flags = OA_UNIT_FLAG_SELECTED;
    // An enemy whose first unit has no movement object is not faced.
    world->units[5].movement = 0;
    OA_CHECK(!vr::opponent_facing(*world, 150, 100));
    oa::world_destroy(world);
}

void build_preview_helpers() {
    namespace vr = oa::app::view_rules;
    OA_CHECK(vr::preview_lists_piece("body2, turret2, guns2, supports", "TURRET2"));
    OA_CHECK(vr::preview_lists_piece("body2,turret2", "body2"));
    OA_CHECK(vr::preview_lists_piece(" supports ", "supports"));
    OA_CHECK(!vr::preview_lists_piece("body2, turret2", "turret"));
    OA_CHECK(!vr::preview_lists_piece("", "base"));
    OA_CHECK(!vr::preview_lists_piece(", ,", ""));
    // Without a piece list the muzzle flashes and wakes are left out.
    OA_CHECK(vr::preview_skips_piece("FLARE"));
    OA_CHECK(vr::preview_skips_piece("Wake"));
    OA_CHECK(!vr::preview_skips_piece("flare1"));
    OA_CHECK(!vr::preview_skips_piece("base"));
    // The outline walks bright green to black and back once each pulse of
    // 30 ticks, from 24 steps into the walk; the fill half a walk away.
    const auto start = vr::build_preview_look(0, true, 40);
    OA_CHECK(start.outline == 0xa7);
    OA_CHECK(start.fill == uint8_t{0xa8});
    OA_CHECK(vr::build_preview_look(7, false, 40).outline == 0xa0);
    OA_CHECK(vr::build_preview_look(22, false, 40).outline == 0xaf);
    OA_CHECK(!vr::build_preview_look(22, false, 40).fill);
    for (uint32_t tick = 0; tick < vr::build_preview_pulse_ticks; ++tick) {
        const auto look = vr::build_preview_look(tick, true, 40);
        const auto later = vr::build_preview_look(tick + vr::build_preview_pulse_ticks, true, 40);
        OA_CHECK(look.outline == later.outline && look.fill == later.fill);
        OA_CHECK(look.scanning == later.scanning && look.scan_low == later.scan_low);
    }
    // The scanline climbs a model 254 pixels tall from its foot to its top
    // over the first 15 ticks of the pulse, lighting 5 heights a tick.
    const auto foot = vr::build_preview_look(0, false, 254);
    OA_CHECK(foot.scanning && foot.scan_low == 0 && foot.scan_high == 2);
    const auto top = vr::build_preview_look(14, false, 254);
    OA_CHECK(top.scanning && top.scan_low == 252 && top.scan_high == 254);
    const auto middle = vr::build_preview_look(7, false, 254);
    OA_CHECK(middle.scanning && middle.scan_low == 125 && middle.scan_high == 129);
    OA_CHECK(!vr::build_preview_look(15, false, 254).scanning);
    OA_CHECK(!vr::build_preview_look(29, false, 254).scanning);
    // A flat model is lit on the climb's first tick only.
    OA_CHECK(vr::build_preview_look(0, false, 0).scanning);
    OA_CHECK(!vr::build_preview_look(1, false, 0).scanning);
}

void chat_helpers() {
    namespace vr = oa::app::view_rules;
    // A line without a logo: from 0x86 to 4 past the text, a row above and
    // below.
    OA_CHECK(vr::chat_backdrop_rect(false, 0x34, 50, 14) == (vr::SourceBox{0x86, 0x33, 58, 16}));
    // A logo counts as a line height.
    OA_CHECK(vr::chat_backdrop_rect(true, 0x42, 50, 14) == (vr::SourceBox{0x86, 0x41, 72, 16}));
    const auto lines = vr::chat_macro_lines(vr::default_chat_macro);
    OA_CHECK(
        lines == (std::vector<std::string>{
                     "+setshareenergy 1000", "+setsharemetal 1000", "+shareall", "+shootall"
                 })
    );
    OA_CHECK(vr::chat_macro_lines("gg\r\n\rwp\n").size() == 2);
    OA_CHECK(vr::chat_macro_lines("").empty());
}

void share_sliders() {
    namespace vr = oa::app::view_rules;
    // A threshold of 250 of 1000 storage on a 100-position slider.
    OA_CHECK(vr::share_threshold_knob(250.0F, 1000.0F, 100) == 25);
    OA_CHECK(vr::share_threshold_knob(250.0F, 1000.9F, 100) == 25);
    OA_CHECK(vr::share_threshold_knob(250.0F, 0.5F, 100) == 0);
    // Knob 25 of positions 0 to 99: 25/99 of 1000, truncated.
    OA_CHECK(vr::share_threshold_value(25, 100, 1000.0F) == 252);
    OA_CHECK(vr::share_threshold_value(99, 100, 1000.0F) == 1000);
    OA_CHECK(vr::share_threshold_value(150, 100, 1000.0F) == 1000);
    OA_CHECK(vr::share_threshold_value(0, 100, 1000.0F) == 0);
}

/// A key slider shows the binding's first chord when that chord is one of the
/// option keys, and Alt otherwise. A move maps the way a stored key is
/// imported; a lone modifier a repeat or a hold cannot take maps to nothing.
void key_sliders_follow_the_binding() {
    namespace vr = oa::app::view_rules;
    namespace es = oa::ui::engine_settings;
    const uint32_t alt = es::option_keys[0].code;
    const uint32_t slash = static_cast<uint32_t>('/');
    const uint32_t ex = static_cast<uint32_t>('x');
    const uint32_t cue = static_cast<uint32_t>('q');
    const std::array<std::string_view, 2> rotate_defaults{"@Slash", "Shift+@Slash"};
    OA_CHECK(vr::slider_value("build.rotate", rotate_defaults) == slash);
    const std::array<std::string_view, 1> shifted{"Shift+@Slash"};
    OA_CHECK(vr::slider_value("build.rotate", shifted) == alt);
    OA_CHECK(vr::slider_value("build.rotate", std::span<const std::string_view>{}) == alt);
    const std::array<std::string_view, 1> line_default{"@KeyX"};
    OA_CHECK(vr::slider_value("build.line", line_default) == ex);
    const std::array<std::string_view, 1> snap_default{"Alt"};
    OA_CHECK(vr::slider_value("build.snap-override", snap_default) == alt);

    const std::vector<std::string> turned = vr::slider_chords("build.rotate", cue);
    OA_CHECK(turned.size() == 2);
    if (turned.size() == 2) {
        OA_CHECK(turned[0] == "@KeyQ");
        OA_CHECK(turned[1] == "Shift+@KeyQ");
    }
    OA_CHECK(vr::slider_chords("build.rotate", alt).empty());
    OA_CHECK(vr::slider_chords("build.line", alt).empty());
    const std::vector<std::string> lined = vr::slider_chords("build.line", ex);
    OA_CHECK(lined.size() == 1 && lined[0] == "@KeyX");
    const std::vector<std::string> snapped = vr::slider_chords("build.snap-override", alt);
    OA_CHECK(snapped.size() == 1 && snapped[0] == "Alt");
    const std::vector<std::string> letter = vr::slider_chords("build.snap-override", ex);
    OA_CHECK(letter.size() == 1 && letter[0] == "X");
    OA_CHECK(vr::slider_chords("build.rotate", 0).empty());

    const std::function<std::optional<uint32_t>(std::string_view)> unread;
    const auto missing = vr::imported_key("build.rotate", unread);
    OA_CHECK(missing && missing->setting == "RotateBuildKey" && missing->code == slash);
    std::map<std::string, uint32_t> numbers{{"KeyCode", static_cast<uint32_t>('b')}};
    const auto stored =
        vr::imported_key("build.line", [&](std::string_view name) -> std::optional<uint32_t> {
            const auto found = numbers.find(std::string(name));
            return found == numbers.end() ? std::nullopt : std::optional<uint32_t>(found->second);
        });
    OA_CHECK(stored && stored->setting == "KeyCode" && stored->code == static_cast<uint32_t>('b'));
    OA_CHECK(!vr::imported_key("whiteboard.draw", unread));
    const auto snapped_key = vr::imported_key("build.snap-override", unread);
    OA_CHECK(snapped_key && snapped_key->code == alt);
}

} // namespace

void options_dialog_round_trip() {
    namespace vr = oa::app::view_rules;
    namespace es = oa::ui::engine_settings;
    oa::data::mod_profile::UiRules ui{};
    ui.click_snap.enabled = true;
    ui.click_snap.mex_max = 5;
    ui.click_snap.wreck_max = 0;
    vr::ViewSettings settings{};
    settings.patrol[2] = vr::PatrolOption::assist_only;
    settings.guard[0] = vr::GuardOption::scatter;
    settings.mex_snap_radius = 4;
    settings.panel_background = vr::PanelBackground::solid;
    settings.full_rings = false;
    const auto options = vr::dialog_options(settings, ui);
    OA_CHECK(options.patrol[2] == 2 && options.guard[0] == 2);
    OA_CHECK(options.mex_snap_most == 5 && options.wreck_snap_most == 0);
    OA_CHECK(options.mex_snap_radius == 4 && options.panel_background == 2);
    OA_CHECK(!options.full_rings && options.optimize_dt_rows);
    // What the dialog chose comes back; what it does not show stays.
    auto chosen = options;
    chosen.mex_snap_radius = 9;
    chosen.wreck_snap_radius = 3;
    chosen.chat_backdrop = true;
    chosen.patrol[0] = 7;
    auto applied = settings;
    applied.chat_macro = "+shootall";
    vr::apply_dialog_options(chosen, ui, applied);
    OA_CHECK(applied.mex_snap_radius == 5 && applied.wreck_snap_radius == 0);
    OA_CHECK(applied.chat_backdrop && applied.chat_macro == "+shootall");
    OA_CHECK(applied.patrol[0] == vr::PatrolOption::assist_only);
    vr::apply_dialog_options(options, ui, applied);
    applied.chat_macro = settings.chat_macro;
    OA_CHECK(vr::dialog_options(applied, ui) == options);
    // A radius the profile gives no room is set by the mod.
    const auto locks = vr::dialog_option_locks(ui);
    OA_CHECK(locks.mex_snap == es::Lock::none && locks.wreck_snap == es::Lock::set_by_mod);
    const auto unset = vr::dialog_option_locks({});
    OA_CHECK(unset.mex_snap == es::Lock::set_by_mod);
}

/// A profile's strings show only where they differ from 3.1c's, each
/// elimination ending on its own; the credits gadget is the profile's name.
void profile_texts_follow_the_profile() {
    namespace vr = oa::app::view_rules;
    const auto none = vr::profile_texts(nullptr);
    OA_CHECK(none.nanolathing_status == nullptr && none.kill_lead == nullptr);
    OA_CHECK(vr::credits_gadget(nullptr) == "Credits");
    oa::data::mod_profile::ModProfile profile{};
    const auto baseline = vr::profile_texts(&profile);
    OA_CHECK(baseline.nanolathing_status == nullptr && baseline.paralyzed_status == nullptr);
    OA_CHECK(baseline.leave_question == nullptr && baseline.kill_lead == nullptr);
    for (const char* ending : baseline.elimination_endings)
        OA_CHECK(ending == nullptr);
    profile.strings.status.nanolathing = "Building";
    profile.strings.status.paralyzed = "Stunned";
    profile.strings.message.exit_confirm = "Abandon this made-up war?";
    profile.strings.message.kill_lead = "%s leads with %d";
    profile.strings.message.elimination[1] = "have been made up";
    profile.strings.gadget.credits = "Thanks";
    const auto texts = vr::profile_texts(&profile);
    OA_CHECK(std::string_view(texts.nanolathing_status) == "Building");
    OA_CHECK(std::string_view(texts.paralyzed_status) == "Stunned");
    OA_CHECK(std::string_view(texts.leave_question) == "Abandon this made-up war?");
    OA_CHECK(std::string_view(texts.kill_lead) == "%s leads with %d");
    OA_CHECK(texts.elimination_endings[0] == nullptr && texts.elimination_endings[2] == nullptr);
    OA_CHECK(std::string_view(texts.elimination_endings[1]) == "have been made up");
    OA_CHECK(vr::credits_gadget(&profile) == "Thanks");
}

int main() {
    match_display_rules_follow_the_profile();
    explosion_flash_follows_the_lower_level();
    victory_announcement_gate();
    music_source_follows_the_profile();
    display_modes_follow_the_profile();
    screenshot_names();
    click_snap_search();
    view_settings_round_trip();
    unchosen_snap_radius_not_stored();
    build_tool_layouts();
    build_facings();
    facing_toward_points();
    opponent_facings();
    build_preview_helpers();
    chat_helpers();
    share_sliders();
    key_sliders_follow_the_binding();
    options_dialog_round_trip();
    profile_texts_follow_the_profile();
    std::cout << "view rules checked\n";
    return oa::test::check_exit_status();
}
