// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/app/acceleration_status.hpp"

#include "oa/app/render_policy.hpp"

#include <optional>

namespace oa::app {

namespace settings = oa::ui::engine_settings;

namespace {

/// Returns the status of Basic drawing where Full was asked for and
/// something keeps it to Basic.
///
/// @param shortfall why Basic draws
/// @return the state; none where nothing keeps Full to Basic, which the
///     states of Basic's own then describe
std::optional<settings::AccelerationState> full_shortfall_state(FullShortfall shortfall) noexcept {
    using settings::AccelerationState;
    switch (shortfall) {
    case FullShortfall::none:
        return std::nullopt;
    case FullShortfall::lacks_feature:
        return AccelerationState::full_lacks_feature;
    case FullShortfall::failed_before:
        return AccelerationState::full_failed_before;
    case FullShortfall::stopped:
        return AccelerationState::full_stopped;
    case FullShortfall::too_little_memory:
        return AccelerationState::full_too_little_memory;
    case FullShortfall::cannot_save:
        return AccelerationState::full_cannot_save;
    case FullShortfall::waiting_for_game_end:
        return AccelerationState::full_waiting_for_game_end;
    }
    return std::nullopt;
}

} // namespace

bool enough_memory_for_acceleration(uint64_t physical_memory) noexcept {
    return physical_memory >= render_policy::smallest_accelerated_memory;
}

AccelerationReport report_acceleration(const AccelerationFacts& facts) noexcept {
    using settings::AccelerationState;
    using settings::HardwareAcceleration;
    AccelerationReport report{};
    // A flag that names Basic or Full asks for the graphics card.
    const bool flag_on = facts.flag.has_value() && *facts.flag != HardwareAcceleration::off;
    const bool flag_off = facts.flag == HardwareAcceleration::off;
    const bool asked = facts.asked != HardwareAcceleration::off;
    // The environment's driver keeps the frames on the processor unless a
    // flag or a check lifts it.
    const bool environment = facts.environment_driver && !flag_on && !facts.force_capable;
    const bool memory = enough_memory_for_acceleration(facts.physical_memory);
    // A renderer nothing has looked at yet is not ruled out; SDL's software
    // renderer always is. A function test that drew wrongly rules it out
    // even where --force-capable lifts the rest.
    const bool capable = !facts.function_test_failed &&
                         (facts.force_capable ||
                          (facts.renderer_capable.value_or(true) && !facts.software_renderer));
    // SDL's software renderer has no graphics card at all, whatever it lacks.
    const bool lacks_feature = facts.lacks_feature && !facts.software_renderer;
    // A driver that failed in this run explains the renderer it left, which
    // setting Off and back may try again, so it locks nothing; neither does
    // a renderer reached because records skipped the drivers before it,
    // since clearing them lets the next start try those again.
    report.acceleration_unavailable = environment || (!memory && !facts.driver_skipped) ||
                                      (!capable && !facts.driver_failed && !facts.driver_skipped);
    // The records that skipped a driver still stand.
    const bool skipping = facts.driver_skipped && !facts.skipped_cleared;
    report.vertical_sync_unavailable = (facts.software_renderer && !facts.force_capable) ||
                                       facts.vertical_sync_resets_device ||
                                       facts.vertical_sync_refused;
    report.status.reach = facts.reach;
    // Under 2 GiB the processor draws whatever the setting or the flags say,
    // and the status says why first.
    const auto recorded_state = [](RecordedTrouble trouble) {
        return trouble == RecordedTrouble::stopped ? AccelerationState::game_stopped
                                                   : AccelerationState::driver_failed;
    };
    // A flag that asks for the card ignores a record against the driver.
    const bool recorded = facts.recorded != RecordedTrouble::none && !flag_on;
    if (!memory)
        report.status.state = skipping ? AccelerationState::needs_memory_driver_skipped
                                       : AccelerationState::needs_memory;
    else if ((flag_off || !asked) && skipping)
        report.status.state = AccelerationState::off_driver_skipped;
    else if (flag_off)
        report.status.state = AccelerationState::off_by_command_line;
    else if (!asked)
        report.status.state = AccelerationState::off_by_setting;
    else if (environment)
        report.status.state = AccelerationState::environment_driver;
    else if (
        capable && (facts.shared_game || facts.replay) && !facts.tier_accelerated &&
        !facts.driver_failed && !facts.memory_dropped
    )
        report.status.state = AccelerationState::waiting_for_game_end;
    else if (facts.driver_failed && !facts.tier_accelerated)
        report.status.state = AccelerationState::driver_failed;
    else if (facts.memory_dropped && !facts.tier_accelerated)
        report.status.state = AccelerationState::too_little_memory;
    else if (facts.engine_error && !facts.tier_accelerated)
        report.status.state = AccelerationState::engine_error;
    else if (recorded && !facts.tier_accelerated)
        report.status.state = recorded_state(facts.recorded);
    else if (facts.records_unreadable && !facts.tier_accelerated)
        report.status.state = AccelerationState::game_stopped;
    else if (!capable && facts.skipped_recorded != RecordedTrouble::none && skipping)
        report.status.state = recorded_state(facts.skipped_recorded);
    else if (!capable && facts.driver_skipped && facts.skipped_cleared)
        report.status.state = AccelerationState::next_start;
    else if (!capable)
        report.status.state =
            lacks_feature ? AccelerationState::lacks_feature : AccelerationState::no_usable_card;
    else if (facts.trial_unwritten && !facts.tier_accelerated)
        report.status.state = AccelerationState::cannot_save;
    else if (facts.tier_full)
        // The graphics card draws the battlefield.
        report.status.state = AccelerationState::full_in_use;
    else if (facts.tier_accelerated) {
        // Where Full was asked for and something keeps it to Basic, the
        // status says what before anything else; otherwise Basic's own
        // states say what the card does.
        const std::optional<AccelerationState> shortfall =
            facts.asked == HardwareAcceleration::full ? full_shortfall_state(facts.full_shortfall)
                                                      : std::nullopt;
        report.status.state = shortfall              ? *shortfall
                              : facts.driver_skipped ? AccelerationState::in_use_on_another_driver
                              : facts.no_smoothing   ? AccelerationState::in_use_no_smoothing
                                                     : AccelerationState::in_use;
    } else
        report.status.state = AccelerationState::next_start;
    // The wait says whether it is for a replay, and which level then takes
    // effect.
    report.status.replay = (report.status.state == AccelerationState::waiting_for_game_end ||
                            report.status.state == AccelerationState::full_waiting_for_game_end) &&
                           facts.replay;
    report.status.asked = facts.asked;
    report.status.supersample = facts.full_supersample;
    // The factor drawn shows only while frames are drawn in Full.
    report.status.full_supersample = facts.tier_accelerated ? facts.full_supersample_drawn : 0;
    return report;
}

namespace {

/// Returns why Basic draws where Full was asked for, from the facts the
/// tier is decided from, in the order the tier is decided.
///
/// @param inputs the facts
/// @return the shortfall; none where nothing keeps Full to Basic
FullShortfall full_shortfall_of(const render_policy::TierInputs& inputs) noexcept {
    using render_policy::AccelerationFlag;
    using render_policy::FullDrop;
    using render_policy::MatchKind;
    const bool forced = inputs.flag == AccelerationFlag::full;
    if (inputs.full_drop == FullDrop::function_test)
        return FullShortfall::lacks_feature;
    if (inputs.full_unusable_record && !forced)
        return FullShortfall::failed_before;
    switch (inputs.full_drop) {
    case FullDrop::card_failure:
    case FullDrop::frame_refused:
        return FullShortfall::stopped;
    case FullDrop::memory:
        return FullShortfall::too_little_memory;
    case FullDrop::trial_unwritten:
        return FullShortfall::cannot_save;
    case FullDrop::function_test:
    case FullDrop::none:
        break;
    }
    if (inputs.match.kind != MatchKind::none && !inputs.match.full)
        return FullShortfall::waiting_for_game_end;
    return FullShortfall::none;
}

} // namespace

AccelerationFacts tier_acceleration_facts(
    const render_policy::TierInputs& inputs,
    const render_policy::LadderState& rung,
    render_policy::RenderTier tier
) noexcept {
    using render_policy::AccelerationFlag;
    using render_policy::Capability;
    const bool tier_accelerated = render_policy::card_tier(tier);
    AccelerationFacts facts{};
    if (inputs.flag != AccelerationFlag::none)
        facts.flag = render_policy::acceleration_asked(inputs.flag, inputs.setting);
    facts.asked = render_policy::acceleration_asked(inputs.flag, inputs.setting);
    facts.force_capable = inputs.force_capable;
    facts.environment_driver = inputs.render_driver_named || inputs.virtual_video_driver;
    facts.physical_memory = inputs.memory;
    const bool test_failed = inputs.function_test == render_policy::FunctionTest::failed;
    if (inputs.renderer)
        facts.renderer_capable = inputs.capability == Capability::capable && !test_failed;
    facts.function_test_failed = test_failed;
    facts.software_renderer = inputs.capability == Capability::software_renderer;
    facts.lacks_feature = inputs.capability == Capability::small_texture_limit || test_failed;
    facts.driver_failed = inputs.drop == render_policy::Drop::driver_failure;
    facts.memory_dropped = inputs.drop == render_policy::Drop::memory;
    facts.engine_error = inputs.drop == render_policy::Drop::engine_fault;
    // Where the records live in memory a trial is always written, and a file
    // that could not be read keeps nothing standard.
    const bool on_disk =
        render_policy::records_on_disk(inputs.players_own_profile, inputs.render_driver_named);
    facts.trial_unwritten =
        (inputs.function_test == render_policy::FunctionTest::trial_unwritten && on_disk) ||
        inputs.drop == render_policy::Drop::path_trial_unwritten;
    facts.records_unreadable = inputs.records_unreadable_after_unclean_start && on_disk;
    facts.shared_game = inputs.match.kind == render_policy::MatchKind::shared_game;
    facts.replay = inputs.match.kind == render_policy::MatchKind::replay;
    facts.tier_accelerated = tier_accelerated;
    facts.tier_full = tier == render_policy::RenderTier::full;
    if (facts.asked == settings::HardwareAcceleration::full && !facts.tier_full)
        facts.full_shortfall = full_shortfall_of(inputs);
    facts.no_smoothing = rung.budget == render_policy::SceneBudget::none;
    facts.reach = acceleration_reach(rung);
    return facts;
}

settings::AccelerationReach acceleration_reach(const render_policy::LadderState& rung) noexcept {
    using settings::AccelerationReach;
    const bool smoothed = rung.budget != render_policy::SceneBudget::none;
    if (!rung.filtered_chrome)
        return smoothed ? AccelerationReach::nearest_zoomed_out : AccelerationReach::nearest_none;
    if (smoothed)
        return AccelerationReach::zoomed_out;
    return rung.magnify ? AccelerationReach::zoomed_in : AccelerationReach::menus;
}

} // namespace oa::app
