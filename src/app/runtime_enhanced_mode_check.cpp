// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The opt-in preset, graphics independence and recorded rules of loaded games.
#include "oa/app/runtime.hpp"
#include "oa/app/enhanced_mode.hpp"
#include "engine_settings_state.hpp"
#include "oa/data/enhanced_mode.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/sim/unit_spawn/restore_ranges.hpp"

#include <array>
#include <cstdio>
#include <stdexcept>
#include <string>

namespace oa::app {
namespace {

/// Stops a native check when its expected behavior is absent.
void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(std::string("Enhanced Mode check: ") + message);
}

} // namespace

void Runtime::check_enhanced_mode() {
    namespace settings = oa::ui::engine_settings;
    namespace mode = oa::data::enhanced_mode;
    require(options_.preferences_file.has_value(), "needs an isolated --preferences-file");
    const settings::EngineSettings kept = engine_settings();
    auto chosen = kept;
    chosen.enhanced_mode = false;
    apply_engine_settings(chosen);
    require(enhanced_mode_version(*this) == mode::off_version, "Off did not keep released rules");
    const std::string released_hash = simulation_hash(*this);

    chosen.enhanced_mode = true;
    apply_engine_settings(chosen);
    require(enhanced_mode_version(*this) == mode::current_version, "On did not resolve the preset");
    const std::string enhanced_hash = simulation_hash(*this);
    require(enhanced_hash != released_hash, "the preset did not enter the simulation hash");
    constexpr std::array renderers{
        settings::HardwareAcceleration::off,
        settings::HardwareAcceleration::basic,
        settings::HardwareAcceleration::full
    };
    for (const auto renderer : renderers) {
        chosen.hardware_acceleration = renderer;
        apply_engine_settings(chosen);
        require(simulation_hash(*this) == enhanced_hash, "graphics changed enhanced rules");
    }
    chosen.hardware_acceleration = settings::HardwareAcceleration::off;
    chosen.unit_limit = oa::data::limits::highest_enhanced_units_per_player;
    apply_engine_settings(chosen);

    start_benchmark_skirmish_as_chosen();
    require(enhanced_mode_version(*this) == mode::current_version, "the match lost its preset");
    require(
        match_->world().slots.size() == 20'001, "the match did not reserve two 10,000-unit ranges"
    );
    require(match_->state().game.units_per_player == 10'000, "the match clipped the unit limit");
    require(
        oa::world_projectile_capacity(&match_->world().record) == 32'767,
        "the match kept the classic projectile capacity"
    );
    const fs::path enhanced_save = options_.preferences_file->parent_path() / "enhanced.sav";
    require(save_match_game(enhanced_save, "Enhanced Mode", 0), "the enhanced save failed");
    chosen.enhanced_mode = false;
    apply_engine_settings(chosen);
    require(enhanced_mode_version(*this) == mode::current_version, "a live match changed mode");
    require(simulation_hash(*this) == enhanced_hash, "a live match changed rules identity");
    leave_match();
    load(Screen::main_menu);
    require(enhanced_mode_version(*this) == mode::off_version, "the next game did not take Off");
    require(simulation_hash(*this) == released_hash, "Off did not restore the original hash");
    require(load_saved_game(enhanced_save), "the enhanced save did not load with Off selected");
    require(
        enhanced_mode_version(*this) == mode::current_version, "the save did not restore its mode"
    );
    require(!engine_settings().enhanced_mode, "loading changed the menu preference");
    require(match_->world().slots.size() == 20'001, "the saved match lost its unit ranges");
    require(match_->unit_player_mask() == 3, "the saved match lost its allocation mask");
    require(hold_enhanced_rules_once(mode::off_version), "a subsequent load could not hold Off");
    release_enhanced_rules_once(true);
    require(
        engine_settings_->restored_enhanced_version == mode::current_version &&
            engine_settings_->restored_enhanced_started,
        "a failed subsequent load lost the active match's mode hold"
    );
    leave_match();
    load(Screen::main_menu);
    require(enhanced_mode_version(*this) == mode::off_version, "leaving kept the recorded mode");
    require(
        frontend_game().max_units_setting <= oa::data::limits::highest_units_per_player,
        "the enhanced save left an unsupported classic unit limit"
    );

    chosen.unit_limit = kept.unit_limit;
    apply_engine_settings(chosen);
    start_benchmark_skirmish_as_chosen();
    const fs::path released_save = options_.preferences_file->parent_path() / "released.sav";
    require(save_match_game(released_save, "Released rules", 0), "the released save failed");
    require(
        load_saved_game(enhanced_save), "an enhanced save could not replace a live classic match"
    );
    require(match_->world().slots.size() == 20'001, "replacement clipped the enhanced capacity");
    require(
        load_saved_game(released_save), "a classic save could not replace a live enhanced match"
    );
    require(enhanced_mode_version(*this) == mode::off_version, "replacement kept enhanced rules");
    leave_match();
    load(Screen::main_menu);
    chosen.enhanced_mode = true;
    apply_engine_settings(chosen);
    require(load_saved_game(released_save), "a released save did not load with On selected");
    require(
        enhanced_mode_version(*this) == mode::off_version, "a released save acquired new rules"
    );
    require(engine_settings().enhanced_mode, "loading changed the menu preference");
    std::array<sim::unit_spawn::Request, 2> reordered_units{};
    std::array<bool, 2> found_units{};
    for (const auto& slot : match_->world().slots) {
        const auto& unit = slot.record;
        if (unit.type_index == 0 || unit.owner_index >= reordered_units.size())
            continue;
        auto& request = reordered_units[unit.owner_index];
        request.player = unit.owner_index;
        request.type = unit.type_index;
        request.position = {
            static_cast<uint32_t>(unit.position.x),
            static_cast<uint32_t>(unit.position.y),
            static_cast<uint32_t>(unit.position.z)
        };
        request.finished = true;
        request.state = kGroundOccupancyState;
        found_units[unit.owner_index] = true;
    }
    require(found_units[0] && found_units[1], "the allocation-order fixture lacks its units");
    leave_match();
    load(Screen::main_menu);
    require(enhanced_mode_version(*this) == mode::current_version, "leaving lost the menu's On");
    bootstrap_match({.units_per_player = 10'000, .place_commanders = false, .seat_roster = true});
    constexpr std::array<uint8_t, 2> reversed_order{1, 0};
    require(
        sim::unit_spawn::restore_unit_player_order(match_->state(), reversed_order),
        "the allocation-order fixture could not assign reversed ranges"
    );
    match_->rebind_player_ranges();
    for (const auto& request : reordered_units)
        require(match_->create(request) != nullptr, "the allocation-order fixture could not spawn");
    enter_match_view();
    const fs::path ordered_save = options_.preferences_file->parent_path() / "ordered.sav";
    require(
        save_match_game(ordered_save, "Reversed unit ranges", 0), "reversed ranges did not save"
    );
    require(load_saved_game(ordered_save), "reversed ranges did not load");
    require(
        match_->state().game.players[1].base_unit_id == 1 &&
            match_->state().game.players[0].base_unit_id == 10'001 &&
            match_->state().units[1].type_index == reordered_units[1].type &&
            match_->state().units[10'001].type_index == reordered_units[0].type,
        "the saved allocation order changed unit ownership or ids"
    );
    leave_match();
    load(Screen::main_menu);
    require(
        !hold_enhanced_rules_once(mode::current_version + 1), "an unknown version was accepted"
    );
    require(
        enhanced_mode_version(*this) == mode::current_version, "a rejected version changed rules"
    );
    show_mission_briefing();
    start_campaign_mission();
    require(match_ != nullptr, "the campaign fixture did not start");
    const fs::path campaign_save = options_.preferences_file->parent_path() / "campaign.sav";
    require(save_match_game(campaign_save, "Enhanced campaign", 0), "the campaign save failed");
    leave_match();
    load(Screen::main_menu);
    chosen.enhanced_mode = false;
    apply_engine_settings(chosen);
    require(load_saved_game(campaign_save), "the enhanced campaign did not load");
    require(enhanced_mode_version(*this) == mode::current_version, "the campaign lost its mode");
    restart_campaign_mission();
    require(match_ != nullptr, "the loaded campaign did not restart");
    require(
        enhanced_mode_version(*this) == mode::current_version,
        "a campaign restart lost its recorded mode"
    );
    require(!engine_settings().enhanced_mode, "the campaign changed the menu preference");
    leave_match();
    load(Screen::main_menu);
    require(
        enhanced_mode_version(*this) == mode::off_version, "leaving the campaign kept its mode"
    );
    const auto command_line_mode = options_.enhanced_mode;
    const std::string mode_key(settings::key::enhanced_mode);
    preference_values_[mode_key] = "1";
    options_.enhanced_mode = false;
    chosen.enhanced_mode = false;
    apply_engine_settings(chosen);
    require(
        !save_engine_settings(chosen, chosen, true),
        "Restore defaults could not write the preferences"
    );
    auto written = oa::platform::preferences::load(*options_.preferences_file);
    require(
        written.contains(mode_key) && written.at(mode_key) == "1",
        "a command-line Off erased the stored On preference"
    );
    preference_values_.erase(mode_key);
    options_.enhanced_mode = true;
    chosen.enhanced_mode = true;
    apply_engine_settings(chosen);
    require(
        !save_engine_settings(chosen, chosen, true),
        "Restore defaults could not write an absent preference"
    );
    written = oa::platform::preferences::load(*options_.preferences_file);
    require(!written.contains(mode_key), "a command-line On was stored as a preference");
    options_.enhanced_mode = command_line_mode;
    apply_engine_settings(kept);
    std::puts(
        "Enhanced Mode: preset, graphics independence, frozen matches and save restoration passed"
    );
}

} // namespace oa::app
