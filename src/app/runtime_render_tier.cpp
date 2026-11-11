// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The tier each frame is drawn in: decided before the frame from the facts
// the game's renderer keeps (render_policy::step_tier), and acted on, with
// the start-up function test run where only it is missing and the
// accelerated presentation switched on or off to match, its watch started
// as it switches on (runtime_tier_watch.cpp). Off applies at once; Basic
// and Full apply at once too, except that a match played with other
// machines or a replay, known from its loading screen, keeps the tier it
// began with until it ends. A replay decides its tier as that screen
// begins, so one that is the first scene begins in the tier the player
// chose. Once the ladder has moved, the tier switches on at the rung it
// reached. The renderer records follow the run: the first accelerated
// frame moves the sentinel, Full's first match frame stands under its own
// trial (runtime_full.cpp), switching off closes the stage of a path's
// first frames, and what a match strikes or records is written when it
// ends.
#include "oa/app/runtime.hpp"

#include "engine_settings_state.hpp"
#include "render_host.hpp"
#include "render_run.hpp"

#include <algorithm>
#include <cstdint>

namespace oa::app {

namespace policy = render_policy;

void Runtime::update_render_tier() {
    if (!render_run_ || render_run_->host == nullptr)
        return;
    RendererHost& host = *render_run_->host;
    policy::TierInputs& inputs = host.tier_inputs();
    inputs.renderer =
        sdl_.renderer != nullptr && sdl_.renderer == host.renderer() && !options_.headless_check;
    inputs.director_frame = director_ != nullptr;
    inputs.flag = policy::acceleration_flag(options_.hardware_acceleration);
    inputs.force_capable = options_.force_capable;
    if (engine_settings_)
        inputs.setting = engine_settings_->current.hardware_acceleration;
    inputs.device_lost = render_run_->device_lost;
    const policy::TierStep step =
        policy::step_tier(inputs, accelerated_.on, host.function_test_hooks());
    const bool full = step.decision.tier == policy::RenderTier::full;
    switch (step.action) {
    case policy::TierAction::switch_on: {
        begin_accelerated_watch();
        policy::LadderState rung = render_tier_rung();
        rung.full = full;
        switch_accelerated_presentation(true, rung);
        host.note_first_accelerated_frame();
        break;
    }
    case policy::TierAction::switch_off:
        switch_accelerated_presentation(false, accelerated_.rung);
        // A path whose first frames stood under its own sentinel stops with
        // the tier, as when it is dropped.
        host.end_path_stage();
        break;
    case policy::TierAction::none:
    case policy::TierAction::run_function_test:
        break;
    }
    // Full is a branch of the accelerated presentation: the card draws the
    // battlefield's terrain from the next frame, or stops. The rung says
    // which tier draws at it, so that the memory guard drops Full first.
    set_full_presentation(full);
    if (accelerated_.on) {
        accelerated_.rung.full = full;
        if (render_run_->watch)
            render_run_->watch->rung.full = full;
    }
    render_run_->tier = step.decision;
}

void Runtime::begin_render_tier_match(policy::MatchKind kind) {
    if (!render_run_ || render_run_->host == nullptr)
        return;
    // A replay can be the first scene: --play-demo, or a pending replay
    // after a content switch's soft restart, both before any frame has
    // applied the tier. Decide it now, and run the function test where only
    // that is missing, so the gate, the loading screen and Full's pages take
    // the tier the player chose. A replay opened after other frames is
    // already decided, and this leaves that tier as it is. A shared game is
    // not decided here. It keeps the tier its loading screen finds.
    if (kind == policy::MatchKind::replay)
        update_render_tier();
    policy::begin_match(
        render_run_->host->tier_inputs().match, kind, accelerated_.on, full_presentation()
    );
    // Strikes and records that arise from here wait for the match's end.
    render_run_->host->set_match_running(true);
}

void Runtime::end_render_tier_match() {
    if (!render_run_ || render_run_->host == nullptr)
        return;
    policy::end_match(render_run_->host->tier_inputs().match);
    render_run_->host->set_match_running(false);
}

void Runtime::forget_render_failures() {
    if (!render_run_ || render_run_->host == nullptr)
        return;
    policy::forget_failures(render_run_->host->tier_inputs());
    // The fresh try starts the ladder again from the top; the memory guard,
    // once it has tripped, stays tripped. Where the tier stays on, as a
    // raise from Basic to Full keeps it, the presentation takes the top
    // rung at once, as a switch back on would.
    auto* watch = render_run_->watch.get();
    const bool restart = watch != nullptr && watch->moved && accelerated_.on;
    if (watch != nullptr)
        watch->moved = false;
    if (restart) {
        policy::LadderState rung = render_tier_rung();
        rung.full = accelerated_.rung.full;
        begin_accelerated_watch();
        switch_accelerated_presentation(true, rung);
    }
}

policy::LadderState Runtime::render_tier_rung() const {
    if (!render_run_ || render_run_->host == nullptr)
        return {};
    if (render_run_->watch && render_run_->watch->moved)
        return render_run_->watch->rung;
    return render_run_->rung.value_or(render_run_->host->start_rung());
}

} // namespace oa::app
