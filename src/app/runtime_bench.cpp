// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --bench: a skirmish as the preferences set it up, a stage played into it,
// the camera held where a view file says, a warm-up to a match tick and a
// measured span on the application's own loop, its frames paced and
// presented as a player's are, reported in "bench:" lines
// (docs/command-line.md#--bench).
#include "oa/app/runtime.hpp"
#include "oa/app/enhanced_mode.hpp"
#include "bench.hpp"
#include "bench_timers.hpp"
#include "render_run.hpp"
#include "full_presentation.hpp"
#include "match_models.hpp"
#include "plan_work_counts.hpp"
#include "root_prefix_counts.hpp"
#include "oa/app/frame_coordinates.hpp"

#include "oa/core/unit.h"
#include "oa/base/sha256.hpp"
#include "oa/data/enhanced_mode.hpp"
#include "oa/sim/state_hash.hpp"
#include "oa/ui/console/game_fields.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <limits>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

/// Nanoseconds in a second.
constexpr double kBenchNanosecondsPerSecond = 1e9;
/// Milliseconds in a second.
constexpr double kMillisecondsPerSecond = 1000.0;
/// Frames the measured span keeps room for before it grows.
constexpr std::size_t kExpectedFrames = 4096;
/// Players the unit counts tell apart: the local player, and the rest
/// together.
constexpr std::size_t kCountedPlayers = 2;

/// Throws the bench's error, which run_bench prints in its FAILED line.
///
/// @param what what went wrong
[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error(what);
}

/// The times one frame of the measured span took, in milliseconds.
struct FrameSample {
    double frame{};        ///< from the end of the frame before to the end of this one
    double work{};         ///< the loop's pass, without the wait for the frame's time
    double sim{};          ///< the match ticks the frame ran
    double draw{};         ///< composing the frame
    double present{};      ///< uploading and presenting it
    double full_stage{};   ///< classic Full model/sprite construction and uploads
    double full_execute{}; ///< classic Full's first executor call
    bench::SimulationFrame simulation{};
    renderer_live_scene::GroundIndexBuilds index_builds{}; ///< bounded build-only diagnostics
    double average_unit_pixels{};       ///< automatic transition's owned model diameter
    uint32_t average_unit_count{};      ///< units contributing to that average
    bool automatic_unit_detail{};       ///< the Unit size boundary was selected
    bool units_as_dots{};               ///< the completed frame used the global dot representation
    BodyLodCounts body{};               ///< actual unit model representations emitted by the frame
    SceneModelWorkCounts scene_work{};  ///< independent model packets retained by the frame
    ShadowIndexCounts shadow_indices{}; ///< successful backend shadow draws and uploads
    plan_work::PlanWorkCounts plan_work{};    ///< raw optional planner diagnostics
    RootPrefixActivationCounts root_prefix{}; ///< completed Scene and backend recipe ownership
    uint64_t deferred_geometry{}; ///< invisible model updates deferred during this frame
};

/// Enables planner clocks only within an explicitly requested diagnostic frame.
struct PlanWorkFrame {
    bool previous{};

    explicit PlanWorkFrame(bool enabled) : previous(plan_work::enabled) {
        plan_work::enabled = enabled;
        if (enabled)
            plan_work::frame_counts = {};
    }

    ~PlanWorkFrame() { plan_work::enabled = previous; }
};

/// Prints raw sampled planner sums after the measured span has completed.
/// @param frame the measured frame's index
/// @param work the final attempt's planner snapshot
/// @param root_prefix the completed live frame's owned recipe and draw counts
void print_plan_work(
    std::size_t frame,
    const plan_work::PlanWorkCounts& work,
    const RootPrefixActivationCounts& root_prefix
) {
    const auto word = [](uint64_t value) { return static_cast<unsigned long long>(value); };
    std::printf(
        "bench: PLAN_WORK_FRAME %zu source_frame %llu completed %u sampling_stride %u",
        frame,
        word(work.frame_id),
        unsigned(work.completed),
        work.sampling_stride
    );
    for (std::size_t index = 0; index < plan_work::work_count; ++index)
        std::printf(" %s %llu", plan_work::work_names[index].data(), word(work.work[index]));
    std::printf(
        " root_prefix_scene_instances %llu root_prefix_gpu_instances %llu "
        "root_prefix_gpu_surface_indices %llu",
        word(root_prefix.scene_instances),
        word(root_prefix.gpu_instances),
        word(root_prefix.gpu_surface_indices)
    );
    std::printf(" phases");
    for (std::size_t index = 0; index < plan_work::phase_count; ++index) {
        const auto& phase = work.phases[index];
        std::printf(
            " %s calls %llu samples %llu wall_ns %llu cpu_ns %llu cpu_samples %llu",
            plan_work::phase_names[index].data(),
            word(phase.calls),
            word(phase.samples),
            word(phase.wall_ns),
            word(phase.cpu_ns),
            word(phase.cpu_samples)
        );
    }
    std::printf("\n");
}

/// Prints successful shadow buffer draws and transfers after measurement.
/// @param frame the measured frame's index
/// @param counts the final successful backend snapshot
void print_shadow_indices(std::size_t frame, const ShadowIndexCounts& counts) {
    const auto word = [](uint64_t value) { return static_cast<unsigned long long>(value); };
    std::printf(
        "bench: SHADOW_INDEX_FRAME %zu logical_indices %llu dynamic_indices %llu "
        "cached_indices %llu dynamic_uploaded_indices %llu template_uploaded_indices %llu "
        "cache_bytes %llu requested_runs %u cached_runs %u cache_hits %u cache_misses %u "
        "admitted_patterns %u fallback_runs %u cached_patterns %u mapping_uploaded_words %llu\n",
        frame,
        word(counts.logical_indices),
        word(counts.dynamic_indices),
        word(counts.cached_indices),
        word(counts.dynamic_uploaded_indices),
        word(counts.template_uploaded_indices),
        word(counts.cache_bytes),
        counts.requested_runs,
        counts.cached_runs,
        counts.cache_hits,
        counts.cache_misses,
        counts.admitted_patterns,
        counts.fallback_runs,
        counts.cached_patterns,
        word(counts.mapping_uploaded_words)
    );
}

/// Room for one stage's milliseconds in a FRAME line.
constexpr std::size_t kStageValueBytes = 24;

/// Prints build-only metadata after the measured span, never from a worker.
/// @param frame the measured frame's index
/// @param builds the bounded records collected after its scene joined
void print_index_builds(std::size_t frame, const renderer_live_scene::GroundIndexBuilds& builds) {
    const auto word = [](uint64_t value) { return static_cast<unsigned long long>(value); };
    for (std::size_t index = 0; index < builds.size; ++index) {
        const auto& build = builds.records[index];
        const auto& old = build.before;
        const auto& key = build.requested;
        std::printf(
            "bench: GROUND_INDEX_BUILD_FRAME %zu ordinal %zu previous_attempts %llu "
            "was_valid %u completed %u reason_mask %u old_revision %llu revision %llu "
            "old_vertices %zu vertices %zu old_card %u card %u parallel %u "
            "old_axes %016llx,%016llx,%016llx,%016llx "
            "axes %016llx,%016llx,%016llx,%016llx "
            "old_fp %llx,%llx,%llx fp %llx,%llx,%llx "
            "bounds_ms %.3f world_ms %.3f card_ms %.3f wall_ms %.3f\n",
            frame,
            index,
            word(build.previous_attempts),
            unsigned(build.was_valid),
            unsigned(build.completed),
            build.reasons,
            word(old.revision),
            word(key.revision),
            old.vertices,
            key.vertices,
            unsigned(old.card),
            unsigned(key.card),
            unsigned(build.constructors_parallel),
            word(old.card_axes[0]),
            word(old.card_axes[1]),
            word(old.card_axes[2]),
            word(old.card_axes[3]),
            word(key.card_axes[0]),
            word(key.card_axes[1]),
            word(key.card_axes[2]),
            word(key.card_axes[3]),
            word(old.float_control[0]),
            word(old.float_control[1]),
            word(old.float_control[2]),
            word(key.float_control[0]),
            word(key.float_control[1]),
            word(key.float_control[2]),
            double(build.bounds_nanoseconds) / 1e6,
            double(build.world_nanoseconds) / 1e6,
            double(build.card_nanoseconds) / 1e6,
            double(build.total_nanoseconds) / 1e6
        );
    }
    if (builds.dropped != 0)
        std::printf(
            "bench: GROUND_INDEX_BUILD_DROPPED_FRAME %zu count %llu\n", frame, word(builds.dropped)
        );
}

/// Returns a measure's values, one a frame.
///
/// @param samples the frames
/// @param field the measure
/// @return its values
[[nodiscard]] std::vector<double>
measure_values(const std::vector<FrameSample>& samples, double FrameSample::* field) {
    std::vector<double> values;
    values.reserve(samples.size());
    for (const auto& sample : samples)
        values.push_back(sample.*field);
    return values;
}

/// Prints the SLOW lines: what the slowest 1% of the span's frames, those
/// the 1% low frame rate is taken from, were made of.
///
/// @param samples the span's frames
/// @param frame_totals what the stage timers summed over each of them
void print_slowest_frames(
    const std::vector<FrameSample>& samples, const std::vector<bench_timers::Totals>& frame_totals
) {
    const auto slowest = bench::slowest_frames(measure_values(samples, &FrameSample::frame));
    bench_timers::Totals sum{};
    double frame_total = 0;
    for (const std::size_t index : slowest) {
        bench_timers::add_totals(sum, frame_totals[index]);
        frame_total += samples[index].frame;
    }
    const auto count = static_cast<double>(slowest.size());
    std::printf(
        "bench: SLOW frames %zu of %zu, the slowest 1%%: frame avg %.2f ground_revisions %.0f "
        "terrain_packs %.1f a frame atlas_resets %.0f atlas_rows %.0f a frame "
        "full_atlas_uploads %.0f\n",
        slowest.size(),
        samples.size(),
        frame_total / count,
        sum.ground_revisions,
        sum.terrain_squares_packed / count,
        sum.atlas_emptied,
        sum.atlas_rows_uploaded / count,
        sum.whole_atlas_uploads
    );
    for (std::size_t stage = 0; stage < bench_timers::stage_count; ++stage)
        std::printf(
            "%s\n",
            bench::slow_stage_line(
                static_cast<bench_timers::Stage>(stage), sum.milliseconds[stage], slowest.size()
            )
                .c_str()
        );
    for (std::size_t stage = 0; stage < bench_timers::stage_count; ++stage)
        if (sum.materials_packed[stage] > 0) {
            const auto& name = bench_timers::stage_names[stage];
            const std::string shown = (name.nested ? "  " : "") + std::string(name.name);
            std::printf(
                "bench: SLOW ATLAS packed by %-34s %8.1f materials %10.0f texels a frame\n",
                shown.c_str(),
                sum.materials_packed[stage] / count,
                sum.texels_packed[stage] / count
            );
        }
}

} // namespace

void Runtime::run_bench() {
    // A bench nobody watches says why it stopped in its lines and ends with
    // status 1, rather than waiting on a message box.
    try {
        play_bench();
    } catch (const std::exception& error) {
        std::printf("bench: FAILED %s\n", error.what());
        std::fflush(stdout);
        exit_status_ = 1;
    }
}

bool Runtime::bench_ignores_event(const SDL_Event& event) const {
    if (options_.bench_file.empty())
        return false;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_EDITING:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_JOYSTICK_AXIS_MOTION:
    case SDL_EVENT_JOYSTICK_BALL_MOTION:
    case SDL_EVENT_JOYSTICK_HAT_MOTION:
    case SDL_EVENT_JOYSTICK_BUTTON_DOWN:
    case SDL_EVENT_JOYSTICK_BUTTON_UP:
    case SDL_EVENT_PEN_DOWN:
    case SDL_EVENT_PEN_UP:
    case SDL_EVENT_PEN_MOTION:
    case SDL_EVENT_PEN_BUTTON_DOWN:
    case SDL_EVENT_PEN_BUTTON_UP:
    case SDL_EVENT_PEN_AXIS:
        return true;
    default:
        return false;
    }
}

void Runtime::play_bench() {
    if (sdl_.window == nullptr || sdl_.renderer == nullptr)
        fail("needs the game's window");
    // The bench measures the window a player looks at, so it brings it to
    // the front. On macOS 14 and later a game started from a shell or a test
    // runner is not made the active program, and its window opens behind the
    // front program's windows. There a covered window gets its next drawable
    // back about once a second, and the frame waiting for it takes a second.
    std::ignore = SDL_RaiseWindow(sdl_.window);
    bench::View view;
    if (std::string error; !bench::read_view(options_.bench_file, view, error))
        fail(error);
    if (std::string error;
        !bench::validate_rules_request(view, engine_settings().enhanced_mode, error))
        fail(error);
    if (view.require_gameplay &&
        (options_.fixed_clock || options_.frame_rate || options_.frame_clock_ms ||
         options_.headless_check || director_ || video_capture_ || capture_frame_ ||
         !options_.snapshot.empty() || !options_.capture_video.empty() || !options_.seed))
        fail("require_gameplay needs a real window, wall clock, explicit seed and no captures");
    if (view.display_number != 0) {
        int displays_count{};
        SDL_DisplayID* displays = SDL_GetDisplays(&displays_count);
        if (!displays || view.display_number > uint32_t(displays_count)) {
            SDL_free(displays);
            fail("display_number is outside SDL's real displays");
        }
        const auto display = displays[view.display_number - 1];
        SDL_free(displays);
        if (!SDL_SetWindowPosition(
                sdl_.window,
                SDL_WINDOWPOS_CENTERED_DISPLAY(display),
                SDL_WINDOWPOS_CENTERED_DISPLAY(display)
            ))
            fail("could not place the benchmark on its real display");
        if (view.expected_output_width &&
            !SDL_SetWindowSize(
                sdl_.window, int(view.expected_output_width), int(view.expected_output_height)
            ))
            fail("could not size the benchmark window");
        std::ignore = SDL_RaiseWindow(sdl_.window);
    }
    // A paused rendering workload keeps wall-driven presentation animation
    // fixed as well as its simulation. Bench timing still uses SDL's clock.
    if (view.require_paused)
        options_.fixed_clock = true;
    const char* label = view.label.c_str();
    bool running = true;
    const auto now = [] {
        return static_cast<double>(SDL_GetTicksNS()) / kBenchNanosecondsPerSecond;
    };
    // One pass of the application loop, paced as run() paces it; the time it
    // worked, without the wait, is kept in `work`.
    double work = 0.0;
    const auto frame = [&] {
        const double start = now();
        run_frame(running);
        work = now() - start;
        if (!running || exit_requested_)
            fail("the game was closed");
        pace_next_frame(running);
    };
    const double menu_end = now() + view.menu_seconds;
    while (now() < menu_end)
        frame();
    bool held_rules{};
    const auto restore_rules = [&] {
        if (held_rules)
            release_enhanced_rules_once(true);
    };

    struct RestoreRules {
        const decltype(restore_rules)& restore;

        ~RestoreRules() { restore(); }
    } restore{restore_rules};

    namespace mode = oa::data::enhanced_mode;
    mode::Choice requested;
    if (view.enhanced_revision != 0 || view.enhanced_features_named) {
        const uint32_t revision =
            view.enhanced_revision != 0 ? view.enhanced_revision : mode::highest_revision;
        mode::FeatureSet features = mode::revision_features(revision);
        if (view.enhanced_features_named) {
            std::string list_error;
            if (!bench::resolve_feature_list(
                    view.enhanced_features, revision, features, list_error, "enhanced_features"
                ))
                fail(list_error);
        }
        requested = mode::Choice{revision, features};
        if (!hold_enhanced_rules_once(requested))
            fail("the requested Enhanced Mode rules could not be selected");
        held_rules = true;
    }
    exercise_click(menu::resource_name(menu::Button::single_player));
    exercise_click(entry::resource_name(entry::Button::skirmish));
    state_.player_count = view.players;
    exercise_click(skirmish::resource_name(skirmish::Button::start));
    if (screen_ != Screen::match || !match_)
        fail("the skirmish did not start");
    if (!view.stage.empty()) {
        options_.stage_file = view.stage;
        apply_stage();
    }
    const mode::Choice actual_choice = enhanced_mode_choice(*this);
    if (std::string error; !bench::validate_started_rules(view, actual_choice, error))
        fail(error);
    const std::string actual_features = bench::feature_list_text(actual_choice.features);
    const std::string asked_features = view.enhanced_features_named
                                           ? bench::feature_list_text(requested.features)
                                           : std::string("-");
    // Live timings are painted into the software battlefield itself; they
    // would make identical paused scenes produce different image receipts.
    show_frame_stats(!view.require_paused && !view.require_gameplay);
    // Staging is synchronous setup, not elapsed game time to catch up with.
    const auto initial_census = [&] {
        std::array<uint32_t, OA_PLAYER_COUNT> result{};
        for (const auto& slot : match_->world().slots)
            if (slot.unit && slot.record.type_index != 0 &&
                oa::unit_is_live_target(slot.record.flags))
                ++result[slot.record.owner_index];
        return result;
    }();
    uint32_t initial_total{};
    for (const auto count : initial_census)
        initial_total += count;
    if (view.expected_initial_units && initial_total != view.expected_initial_units)
        fail("the staged initial roster differs from expected_initial_units");
    if (view.expected_initial_per_side && (initial_census[0] != view.expected_initial_per_side ||
                                           initial_census[1] != view.expected_initial_per_side))
        fail("the staged initial sides differ from expected_initial_per_side");

    struct CombatCounts {
        std::array<uint64_t, OA_PLAYER_COUNT> shots{}, damage{}, deaths{}, births{};
    } combat;

    sim::match_runtime::EventHooks events{};
    events.context = &combat;
    events.unit_created = [](void* context, const oa::World& world, uint16_t id) {
        ++static_cast<CombatCounts*>(context)->births[world.units[id].owner_index];
    };
    events.shot_placed = [](void* context,
                            const oa::World&,
                            const oa::Projectile& shot,
                            sim::match_runtime::ShotSource,
                            const oa::FixedVec3*,
                            uint16_t) {
        if (shot.owner_index < OA_PLAYER_COUNT)
            ++static_cast<CombatCounts*>(context)->shots[shot.owner_index];
    };
    events.unit_damaged = [](void* context,
                             const oa::World& world,
                             uint16_t target,
                             uint16_t,
                             int16_t amount,
                             uint8_t kind) {
        constexpr uint8_t weapon_damage_kind = 1;
        if (kind == weapon_damage_kind && amount > 0)
            static_cast<CombatCounts*>(context)->damage[world.units[target].owner_index] +=
                uint16_t(amount);
    };
    events.unit_died = [](void* context,
                          const oa::World& world,
                          uint16_t id,
                          const sim::match_runtime::KillOutcome&,
                          bool) {
        ++static_cast<CombatCounts*>(context)->deaths[world.units[id].owner_index];
    };
    const auto listener = view.require_gameplay ? match_->add_event_listener(events) : 0;
    if (view.require_gameplay && listener == 0)
        fail("could not install gameplay counters");

    struct ListenerGuard {
        decltype(match_)& current;
        sim::match_runtime::Match* original;
        sim::match_runtime::EventListenerId id;

        ~ListenerGuard() {
            if (id && current.get() == original)
                std::ignore = current->remove_event_listener(id);
        }
    } listener_guard{match_, match_.get(), listener};

    const auto slots = match_->world().slots.size();
    std::vector<oa::FixedVec3> initial_positions(view.require_gameplay ? slots : 0);
    std::vector<oa::FixedVec3> sampled_positions(view.require_gameplay ? slots : 0);
    std::vector<uint32_t> initial_generations(view.require_gameplay ? slots : 0);
    std::vector<uint8_t> moved(view.require_gameplay ? slots : 0);
    std::array<uint32_t, OA_PLAYER_COUNT> moved_units{};
    if (view.require_gameplay) {
        for (const auto& slot : match_->world().slots)
            if (slot.unit && slot.record.type_index) {
                initial_positions[slot.unit_index] = slot.record.position;
                initial_generations[slot.unit_index] =
                    match_->runtime_state(slot.unit_index).instance_generation;
            }
        snap_held_clock();
        std::printf(
            "bench: INITIAL units %u owner0 %u owner1 %u seed %u controller0 %u "
            "controller1 %u setup_clock_reanchored 1 enhanced_mode %u revision %u "
            "features %s requested_revision %u requested_features %s\n",
            initial_total,
            initial_census[0],
            initial_census[1],
            *options_.seed,
            unsigned(match_->state().game.players[0].status),
            unsigned(match_->state().game.players[1].status),
            unsigned(actual_choice.revision != 0),
            static_cast<unsigned>(actual_choice.revision),
            actual_features.c_str(),
            static_cast<unsigned>(view.enhanced_revision),
            asked_features.c_str()
        );
    } else {
        std::printf(
            "bench: INITIAL enhanced_mode %u revision %u features %s "
            "requested_revision %u requested_features %s\n",
            unsigned(actual_choice.revision != 0),
            static_cast<unsigned>(actual_choice.revision),
            actual_features.c_str(),
            static_cast<unsigned>(view.enhanced_revision),
            asked_features.c_str()
        );
    }
    // The warm-up's frames are timed too, so that the kept ground's
    // revision the span starts from is the frame before's.
    bench_timers::reset(view.stage_timers);
    double applied_yaw = std::numeric_limits<double>::quiet_NaN();
    float applied_zoom = -1;
    // Holds the view at the file's zoom and centre, and turns the camera to
    // a yaw at the file's pitch; yaw and pitch 0 keep the classic view.
    const auto aim = [&](double yaw) {
        const float zoom =
            view.zoom <= 0
                ? least_match_zoom()
                : std::clamp(static_cast<float>(view.zoom), least_match_zoom(), most_match_zoom());
        if (zoom != applied_zoom) {
            match_zoom_ = match_zoom_target_ = zoom;
            place_match_view(
                view.centre_x - match_layout_.battlefield_width() / (2.0 * zoom),
                view.centre_z - match_layout_.battlefield_height() / (2.0 * zoom)
            );
            applied_zoom = zoom;
        }
        const bool turned = yaw != 0 || view.pitch != 0;
        if (turned && (yaw != applied_yaw || !live_renderer_active())) {
            if (!turn_live_camera(static_cast<float>(yaw), static_cast<float>(view.pitch)) &&
                (view.require_paused || view.require_gameplay))
                fail("the requested turned camera is unavailable");
            applied_yaw = yaw;
        }
    };
    // The pointer rests over the middle of the battlefield, as a player's
    // does, so that the frame's pick of the unit under it is a player's.
    const auto point_at_battle = [&] {
        float x{}, y{};
        if (!frame_to_window(
                sdl_.renderer,
                static_cast<float>(match_layout_.left + match_layout_.battlefield_width() / 2),
                static_cast<float>(match_layout_.top + match_layout_.battlefield_height() / 2),
                &x,
                &y
            ))
            return;
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.windowID = SDL_GetWindowID(sdl_.window);
        event.motion.x = x;
        event.motion.y = y;
        handle_sdl_event(event, running);
    };
    std::printf("bench: WARMUP label=%s to tick %u\n", label, view.warmup_ticks);
    std::fflush(stdout);
    aim(view.yaw);
    point_at_battle();
    const double warmup_end = now() + view.measure_seconds;
    while (match_timing_.tick < view.warmup_ticks) {
        if (view.require_gameplay && now() >= warmup_end)
            break;
        aim(view.yaw);
        frame();
        if (screen_ != Screen::match)
            fail("the match ended in the warm-up");
        if (view.require_gameplay &&
            ((match_->state().game.sim_run_flags & oa::ui::console::kSimRunPaused) != 0 ||
             match_timing_.requested_rate != oa::base::game_loop::normal_game_speed))
            fail("gameplay was paused or intentionally slowed in warm-up");
    }
    point_at_battle();
    // The live units of the local player and of the others.
    const auto count_units = [&] {
        std::array<int, kCountedPlayers> units{};
        for (const auto& slot : match_->world().slots)
            if (slot.unit != nullptr && slot.record.type_index != 0 &&
                oa::unit_is_live_target(slot.record.flags))
                ++units[slot.record.owner_index == match_local_player_ ? 0 : 1];
        return units;
    };
    const auto counted_population = [&] {
        uint32_t units = 0;
        for (const auto& player : match_->state().game.players)
            units += player.unit_count;
        return units;
    };
    const auto check_population = [&] {
        if (view.expected_live_units != 0 && counted_population() != view.expected_live_units)
            fail("the live unit counters differ from expected_live_units");
    };
    const auto check_roster = [&] {
        if (view.expected_live_units != 0) {
            const auto units = count_units();
            if (uint32_t(units[0] + units[1]) != view.expected_live_units)
                fail("the live unit roster differs from expected_live_units");
            check_population();
        }
    };
    const uint32_t held_tick = match_timing_.tick;
    const auto check_paused = [&] {
        if (view.require_paused &&
            ((match_->state().game.sim_run_flags & oa::ui::console::kSimRunPaused) == 0 ||
             match_timing_.tick != held_tick))
            fail("the paused benchmark advanced or resumed its match");
    };
    check_roster();
    check_paused();
    for (uint32_t index = 0; index < view.warmup_frames; ++index) {
        aim(view.yaw);
        frame();
        if (screen_ != Screen::match || !match_)
            fail("the match ended in the frame warm-up");
        check_paused();
        check_population();
    }
    const auto has_input_focus = [&] {
        return (SDL_GetWindowFlags(sdl_.window) & SDL_WINDOW_INPUT_FOCUS) != 0 &&
               SDL_GetKeyboardFocus() == sdl_.window;
    };
    if (view.require_gameplay) {
        // Raising the normal SDL window may need another application frame
        // before its real focus event arrives. Keep this grace outside the
        // measured span; measured frames still fail immediately when away.
        const double readiness_start = now();
        const uint32_t readiness_tick = match_timing_.tick;
        const bool was_away = window_away() || !has_input_focus();
        const bool raised = was_away && SDL_RaiseWindow(sdl_.window);
        uint32_t readiness_frames{};
        while ((window_away() || !has_input_focus()) && now() - readiness_start < 1.0) {
            aim(view.yaw);
            frame();
            ++readiness_frames;
            if (screen_ != Screen::match || !match_)
                fail("the match ended while waiting for benchmark focus");
        }
        std::printf(
            "bench: READINESS away_before %u raised %u away_after %u frames %u "
            "extra_ticks %u seconds %.6f tick %u application_active %u window_flags %llu "
            "input_focus %u keyboard_focus %u\n",
            unsigned(was_away),
            unsigned(raised),
            unsigned(window_away() || !has_input_focus()),
            readiness_frames,
            match_timing_.tick - readiness_tick,
            now() - readiness_start,
            match_timing_.tick,
            unsigned(application_active_),
            static_cast<unsigned long long>(SDL_GetWindowFlags(sdl_.window)),
            unsigned((SDL_GetWindowFlags(sdl_.window) & SDL_WINDOW_INPUT_FOCUS) != 0),
            unsigned(SDL_GetKeyboardFocus() == sdl_.window)
        );
        std::fflush(stdout);
    }
    const std::array<int32_t, 2> held_camera{match_camera_x_, match_camera_z_};
    const float held_zoom = match_zoom_;
    const auto held_angles = live_camera_angles();
    const bool held_acceleration = accelerated_presentation();
    const bool held_full = full_presentation();
    const auto check_gameplay = [&] {
        if (!view.require_gameplay)
            return;
        if (!render_run_ || render_run_->device_lost || render_run_->present_false_logged ||
            window_away() || !has_input_focus() || options_.fixed_clock || capture_frame_ ||
            video_capture_ ||
            (match_->state().game.sim_run_flags & oa::ui::console::kSimRunPaused) != 0 ||
            match_timing_.requested_rate != oa::base::game_loop::normal_game_speed) {
            const auto flags = SDL_GetWindowFlags(sdl_.window);
            std::printf(
                "bench: GAMEPLAY_STATE tick %u render_run %u device_lost %u "
                "present_false %u window_away %u application_active %u minimised %u "
                "background %u away_held %u fixed_clock %u capture_frame %u video_capture %u "
                "sim_flags %u paused %u menu_paused %u match_finished %u requested_rate %u "
                "actual_rate %u window_flags %llu input_focus %u hidden %u minimized %u "
                "keyboard_focus %u\n",
                match_timing_.tick,
                unsigned(bool(render_run_)),
                unsigned(render_run_ && render_run_->device_lost),
                unsigned(render_run_ && render_run_->present_false_logged),
                unsigned(window_away()),
                unsigned(application_active_),
                unsigned(away_pause_.minimised),
                unsigned(away_pause_.background),
                unsigned(away_pause_.held),
                unsigned(options_.fixed_clock),
                unsigned(bool(capture_frame_)),
                unsigned(bool(video_capture_)),
                unsigned(match_->state().game.sim_run_flags),
                unsigned(
                    (match_->state().game.sim_run_flags & oa::ui::console::kSimRunPaused) != 0
                ),
                unsigned(match_paused_),
                unsigned(match_finished_),
                unsigned(match_timing_.requested_rate),
                unsigned(match_timing_.actual_rate),
                static_cast<unsigned long long>(flags),
                unsigned((flags & SDL_WINDOW_INPUT_FOCUS) != 0),
                unsigned((flags & SDL_WINDOW_HIDDEN) != 0),
                unsigned((flags & SDL_WINDOW_MINIMIZED) != 0),
                unsigned(SDL_GetKeyboardFocus() == sdl_.window)
            );
            std::fflush(stdout);
            fail("gameplay lost its active, unpaused real-time presentation");
        }
        if (!accelerated_presentation() || !full_presentation() ||
            live_renderer_active() != (view.yaw != 0 || view.pitch != 0) ||
            live_renderer_failed() || live_renderer_unavailable() ||
            held_camera != std::array<int32_t, 2>{match_camera_x_, match_camera_z_} ||
            held_zoom != match_zoom_ || held_angles != live_camera_angles())
            fail("gameplay changed its requested Full renderer or camera");
        if (!live_renderer_active() && (!full_ || !full_->drawn))
            fail("gameplay's classic Full frame fell back to Basic");
        if (live_renderer_active() &&
            (!render_run_->host || !render_run_->host->presenting_on_card() ||
             bench_timers::timers.totals.milliseconds[size_t(bench_timers::Stage::gpu_download)] !=
                 0))
            fail("gameplay's turned frame was read back instead of presented on the card");
        int width{}, height{}, pixel_width{}, pixel_height{};
        if (!SDL_GetRenderOutputSize(sdl_.renderer, &width, &height) ||
            !SDL_GetWindowSizeInPixels(sdl_.window, &pixel_width, &pixel_height) ||
            (view.expected_output_width && (width != int(view.expected_output_width) ||
                                            height != int(view.expected_output_height) ||
                                            pixel_width != width || pixel_height != height))) {
            std::printf(
                "bench: OUTPUT_MISMATCH actual_output %dx%d actual_pixels %dx%d "
                "expected %ux%u display %u\n",
                width,
                height,
                pixel_width,
                pixel_height,
                view.expected_output_width,
                view.expected_output_height,
                SDL_GetDisplayForWindow(sdl_.window)
            );
            fail("gameplay's real window/output pixels differ from the requested size");
        }
    };
    const auto check_frame = [&] {
        check_population();
        check_paused();
        check_gameplay();
        if (view.require_paused &&
            (held_camera != std::array<int32_t, 2>{match_camera_x_, match_camera_z_} ||
             held_zoom != match_zoom_ || held_angles != live_camera_angles() ||
             held_acceleration != accelerated_presentation() || held_full != full_presentation() ||
             ((view.yaw != 0 || view.pitch != 0) != live_renderer_active())))
            fail("the paused benchmark changed its camera");
    };
    const auto world_digest = [&] {
        return oa::sim::trace::match_state_hash(
            *match_, match_timing_, match_camera_x_, match_camera_z_, nullptr
        );
    };
    const uint64_t held_world = view.require_paused ? world_digest() : 0;
    std::vector<FrameSample> samples;
    samples.reserve(kExpectedFrames);
    // What the stage timers summed over each frame, for the slowest frames'
    // lines and the frame log.
    std::vector<bench_timers::Totals> frame_totals;
    frame_totals.reserve(kExpectedFrames);
    bench_timers::reset(view.stage_timers);
    const uint32_t first_tick = match_timing_.tick;
    check_gameplay();
    const CombatCounts combat_at_measure = combat;
    const auto print_combat = [&](const char* line, const CombatCounts& baseline) {
        std::printf(
            "bench: %s tick %u shots %llu+%llu damage %llu+%llu deaths %llu+%llu births "
            "%llu+%llu\n",
            line,
            match_timing_.tick,
            static_cast<unsigned long long>(combat.shots[0] - baseline.shots[0]),
            static_cast<unsigned long long>(combat.shots[1] - baseline.shots[1]),
            static_cast<unsigned long long>(combat.damage[0] - baseline.damage[0]),
            static_cast<unsigned long long>(combat.damage[1] - baseline.damage[1]),
            static_cast<unsigned long long>(combat.deaths[0] - baseline.deaths[0]),
            static_cast<unsigned long long>(combat.deaths[1] - baseline.deaths[1]),
            static_cast<unsigned long long>(combat.births[0] - baseline.births[0]),
            static_cast<unsigned long long>(combat.births[1] - baseline.births[1])
        );
    };
    if (view.require_gameplay) {
        for (const auto& slot : match_->world().slots)
            if (slot.unit && slot.record.type_index)
                sampled_positions[slot.unit_index] = slot.record.position;
        print_combat("ACTIVITY_BASELINE", {});
    }
    const uint64_t first_presented = render_run_ ? render_run_->presented : 0;
    uint64_t previous_presented = first_presented;
    uint64_t previous_present_ns = render_run_ ? render_run_->last_present_ns : 0;
    std::vector<double> presented_intervals;
    presented_intervals.reserve(kExpectedFrames);
    uint16_t minimum_actual_rate = match_timing_.actual_rate;
    size_t peak_projectiles = match_->projectiles().size();
    std::array<uint64_t, 4> stalls{};
    constexpr std::array<double, 4> stall_milliseconds{1000.0 / 60.0, 1000.0 / 30.0, 50.0, 100.0};
    if (view.require_gameplay) {
        int width{}, height{}, pixels_width{}, pixels_height{}, vsync{};
        std::ignore = SDL_GetRenderOutputSize(sdl_.renderer, &width, &height);
        std::ignore = SDL_GetWindowSizeInPixels(sdl_.window, &pixels_width, &pixels_height);
        std::ignore = SDL_GetRenderVSync(sdl_.renderer, &vsync);
        const auto display = SDL_GetDisplayForWindow(sdl_.window);
        const auto* mode = SDL_GetCurrentDisplayMode(display);
        std::printf(
            "bench: REAL_WINDOW output %dx%d pixels %dx%d display %u refresh %.3f "
            "density %.3f vsync %d max_fps %u warmup_complete %u\n",
            width,
            height,
            pixels_width,
            pixels_height,
            display,
            mode ? double(mode->refresh_rate) : 0.0,
            double(SDL_GetWindowPixelDensity(sdl_.window)),
            vsync,
            options_.max_frames_per_second,
            unsigned(first_tick >= view.warmup_ticks)
        );
    }
    std::printf(
        "bench: MEASURING label=%s tick=%u seconds=%.0f\n", label, first_tick, view.measure_seconds
    );
    std::fflush(stdout);
    const double start = now();
    double previous = start;
    double last_activity = start;
    uint32_t last_activity_tick = first_tick;
    bool shot = false;
    int second = 0;
    // Frames drawn while the window was away, which another window may have
    // covered or the system may have slowed.
    std::size_t away_frames = 0;
    std::size_t second_first = 0;
    for (;;) {
        const double elapsed = now() - start;
        if (elapsed >= view.measure_seconds ||
            (view.measure_frames != 0 && samples.size() >= view.measure_frames) ||
            (view.measure_ticks != 0 && match_timing_.tick - first_tick >= view.measure_ticks))
            break;
        aim(view.yaw + view.turn_rate * elapsed);
        const auto phases = phase_times_;
        const auto totals_before = bench_timers::timers.totals;
        bench_timers::clear_ground_index_builds();
        bench::SimulationFrame simulation{};
        simulation.tick_before = match_timing_.tick;
        double finished{};
        {
            const bench::SimulationFrameScope recording(view.frame_log ? &simulation : nullptr);
            const PlanWorkFrame plan_recording(view.frame_log && view.plan_work);
            bench_timers::body_counts = {};
            bench_timers::scene_model_work_counts = {};
            bench_timers::shadow_index_counts = {};
            frame();
            finished = now();
            simulation.tick_after = match_timing_.tick;
        }
        if (screen_ != Screen::match || !match_) {
            if (view.require_paused || view.require_gameplay || view.expected_live_units != 0)
                fail("the benchmark lost its match");
        } else {
            check_frame();
        }
        frame_totals.push_back(
            bench_timers::totals_since(bench_timers::timers.totals, totals_before)
        );
        const auto ms = [](int64_t ns) {
            return static_cast<double>(ns) * kMillisecondsPerSecond / kBenchNanosecondsPerSecond;
        };
        samples.push_back(
            {(finished - previous) * kMillisecondsPerSecond,
             work * kMillisecondsPerSecond,
             ms(phase_times_.simulation - phases.simulation),
             ms(phase_times_.compose - phases.compose),
             ms((phase_times_.upload - phases.upload) + (phase_times_.present - phases.present))}
        );
        samples.back().simulation = simulation;
        {
            const auto& settings = engine_settings();
            auto& sample = samples.back();
            sample.body = bench_timers::body_counts;
            sample.scene_work = bench_timers::scene_model_work_counts;
            sample.shadow_indices = bench_timers::shadow_index_counts;
            if (view.frame_log && view.plan_work) {
                sample.plan_work = plan_work::frame_counts;
                sample.root_prefix = live_root_prefix_activation_counts(sample.plan_work.frame_id);
            }
            if (match_models_ && match_models_->match == match_.get())
                sample.deferred_geometry =
                    match_models_->preplanning_visibility.deferred_geometry_count;
            sample.automatic_unit_detail =
                settings.zoomed_out_units == oa::ui::engine_settings::ZoomedOutUnits::dots &&
                settings.zoomed_out_after == oa::ui::engine_settings::ZoomedOutAfter::unit_size;
            sample.units_as_dots = frame_draws_.units_as_dots;
            if (sample.automatic_unit_detail && match_models_ &&
                match_models_->match == match_.get()) {
                const auto& average = match_models_->unit_transition.average;
                sample.average_unit_pixels = average.pixels * match_zoom();
                sample.average_unit_count = average.units;
            }
        }
        if (bench_timers::timers.ground_index_builds.size != 0 ||
            bench_timers::timers.ground_index_builds.dropped != 0)
            samples.back().index_builds = bench_timers::timers.ground_index_builds;
        previous = finished;
        minimum_actual_rate = std::min(minimum_actual_rate, match_timing_.actual_rate);
        if (view.require_gameplay && match_)
            peak_projectiles = std::max(peak_projectiles, match_->projectiles().size());
        if (view.require_gameplay) {
            if (render_run_->presented != previous_presented + 1 ||
                render_run_->last_present_ns <= previous_present_ns)
                fail("gameplay did not present exactly one new frame in its loop");
            presented_intervals.push_back(
                double(render_run_->last_present_ns - previous_present_ns) / 1e6
            );
            previous_presented = render_run_->presented;
            previous_present_ns = render_run_->last_present_ns;
        }
        for (size_t index = 0; index < stalls.size(); ++index)
            stalls[index] +=
                (view.require_gameplay ? presented_intervals.back() : samples.back().frame) >
                stall_milliseconds[index];
        if (view.require_gameplay && full_ && !live_renderer_active()) {
            samples.back().full_stage = ms(int64_t(full_->stage_ns));
            samples.back().full_execute = ms(int64_t(full_->execute_ns));
        }
        // A window that never had the focus lost none, so the focus is read
        // from the window itself.
        if (window_away() || (SDL_GetWindowFlags(sdl_.window) & SDL_WINDOW_INPUT_FOCUS) == 0)
            ++away_frames;
        const uint32_t span_ticks = match_timing_.tick - first_tick;
        if (!view.require_gameplay && !shot &&
            (view.shot_ticks != 0 ? span_ticks >= view.shot_ticks
                                  : finished - start >= view.shot_at)) {
            std::printf("bench: SHOT label=%s\n", label);
            std::fflush(stdout);
            shot = true;
        }
        if (finished - start >= second + 1) {
            std::array<uint32_t, OA_PLAYER_COUNT> moving_units{};
            if (view.require_gameplay)
                for (const auto& slot : match_->world().slots) {
                    const auto id = slot.unit_index;
                    if (!slot.unit || !slot.record.type_index ||
                        !oa::unit_is_live_target(slot.record.flags) ||
                        initial_generations[id] != match_->runtime_state(id).instance_generation)
                        continue;
                    const auto& sampled = sampled_positions[id];
                    if (slot.record.position.x != sampled.x || slot.record.position.z != sampled.z)
                        ++moving_units[slot.record.owner_index];
                    sampled_positions[id] = slot.record.position;
                    const auto& initial = initial_positions[id];
                    if (!moved[id] && (slot.record.position.x != initial.x ||
                                       slot.record.position.y != initial.y ||
                                       slot.record.position.z != initial.z)) {
                        moved[id] = 1;
                        ++moved_units[slot.record.owner_index];
                    }
                }
            double frame_total = 0, sim_total = 0, draw_total = 0;
            const std::size_t frames = samples.size() - second_first;
            for (std::size_t index = second_first; index < samples.size(); ++index) {
                frame_total += samples[index].frame;
                sim_total += samples[index].sim;
                draw_total += samples[index].draw;
            }
            const auto mean = [&](double total) {
                return frames == 0 ? 0.0 : total / static_cast<double>(frames);
            };
            const auto units = count_units();
            std::printf(
                "bench: SEC %d frames %zu frame %.1f sim %.1f draw %.1f units %d+%d vertices %.0f "
                "tick %u\n",
                second,
                frames,
                mean(frame_total),
                mean(sim_total),
                mean(draw_total),
                units[0],
                units[1],
                bench_timers::timers.last.vertices,
                match_timing_.tick
            );
            std::fflush(stdout);
            if (view.require_gameplay)
                std::printf(
                    "bench: ACTIVITY tick %u moved %u+%u shots %llu+%llu damage %llu+%llu "
                    "deaths %llu+%llu births %llu+%llu projectiles %zu "
                    "moving_xz %u+%u sample_ticks %u sample_seconds %.3f\n",
                    match_timing_.tick,
                    moved_units[0],
                    moved_units[1],
                    static_cast<unsigned long long>(combat.shots[0]),
                    static_cast<unsigned long long>(combat.shots[1]),
                    static_cast<unsigned long long>(combat.damage[0]),
                    static_cast<unsigned long long>(combat.damage[1]),
                    static_cast<unsigned long long>(combat.deaths[0]),
                    static_cast<unsigned long long>(combat.deaths[1]),
                    static_cast<unsigned long long>(combat.births[0]),
                    static_cast<unsigned long long>(combat.births[1]),
                    match_->projectiles().size(),
                    moving_units[0],
                    moving_units[1],
                    match_timing_.tick - last_activity_tick,
                    finished - last_activity
                );
            last_activity = finished;
            last_activity_tick = match_timing_.tick;
            ++second;
            second_first = samples.size();
        }
        if (screen_ != Screen::match)
            break;
    }
    const double seconds = previous - start;
    const std::size_t frames = samples.size();
    if (view.measure_frames != 0 && frames != view.measure_frames)
        fail("the benchmark reached its time or tick limit before measure_frames");
    check_roster();
    if (view.require_paused) {
        if (frames == 0 || world_digest() != held_world)
            fail("the paused benchmark changed its world or measured no frames");
        std::printf(
            "bench: STATIC tick %u units %u world %016llx warmup_frames %u verified_frames %zu\n",
            held_tick,
            counted_population(),
            static_cast<unsigned long long>(held_world),
            view.warmup_frames,
            frames
        );
        // The software world image is current here. Accelerated frames may
        // retain older CPU pixels, so they need a separate readback check.
        const bool cpu_image = !held_acceleration && !live_renderer_active();
        const auto image_hash = oa::base::sha256::to_hex(
            oa::base::sha256::digest_of(
                cpu_image ? std::span<const uint8_t>{match_world_cpu_.rgb}
                          : std::span<const uint8_t>{}
            )
        );
        std::printf(
            "bench: RENDER accelerated %u full %u turned %u drawn %u cpu_image_bytes %zu "
            "cpu_image_sha256 %.*s\n",
            unsigned(held_acceleration),
            unsigned(held_full),
            unsigned(live_renderer_active()),
            frame_draws_.units_drawn,
            cpu_image ? match_world_cpu_.rgb.size() : size_t{0},
            cpu_image ? int(image_hash.size()) : 1,
            cpu_image ? image_hash.data() : "-"
        );
    }
    const uint32_t ticks = match_timing_.tick - first_tick;
    if (view.require_gameplay) {
        print_combat("COMBAT_DELTA", combat_at_measure);
        const auto presented = render_run_->presented - first_presented;
        const auto final_units = count_units();
        std::printf(
            "bench: GAMEPLAY presented %llu loops %zu fps %.3f ticks %u target_ticks %u "
            "achieved_tps %.3f complete %u actual_rate_min %u actual_rate_final %u "
            "requested_rate %u peak_projectiles %zu final_units %d+%d "
            "stalls_gt_16_67_33_33_50_100 %llu %llu %llu %llu\n",
            static_cast<unsigned long long>(presented),
            frames,
            seconds > 0 ? double(presented) / seconds : 0,
            ticks,
            view.measure_ticks,
            seconds > 0 ? ticks / seconds : 0,
            unsigned(ticks >= view.measure_ticks),
            unsigned(minimum_actual_rate),
            unsigned(match_timing_.actual_rate),
            unsigned(match_timing_.requested_rate),
            peak_projectiles,
            final_units[0],
            final_units[1],
            static_cast<unsigned long long>(stalls[0]),
            static_cast<unsigned long long>(stalls[1]),
            static_cast<unsigned long long>(stalls[2]),
            static_cast<unsigned long long>(stalls[3])
        );
        std::printf(
            "%s\n",
            bench::measure_line("presented interval", bench::spread(presented_intervals)).c_str()
        );
        for (const auto& [name, field] :
             std::array<std::pair<const char*, double FrameSample::*>, 2>{
                 {{"classic Full stage+upload", &FrameSample::full_stage},
                  {"classic Full first execute", &FrameSample::full_execute}}
             })
            if (!live_renderer_active())
                std::printf(
                    "%s\n",
                    bench::measure_line(name, bench::spread(measure_values(samples, field))).c_str()
                );
    }
    const auto frame_spread = bench::spread(measure_values(samples, &FrameSample::frame));
    const auto per_frame = [&](double total) {
        return frames == 0 ? 0.0 : total / static_cast<double>(frames);
    };
    std::printf(
        "bench: RESULT label=%s seconds %.2f frames %zu fps %.2f low1_fps %.2f ticks %u (%.1f/s) "
        "game speed %d of %d\n",
        label,
        seconds,
        frames,
        seconds > 0 ? static_cast<double>(frames) / seconds : 0.0,
        bench::frames_per_second(frame_spread.low1),
        ticks,
        seconds > 0 ? ticks / seconds : 0.0,
        static_cast<int>(match_timing_.actual_rate),
        static_cast<int>(match_timing_.requested_rate)
    );
    if (away_frames != 0)
        std::printf(
            "bench: AWAY label=%s frames %zu of %zu: the window was away, so the span may not "
            "measure what a player sees\n",
            label,
            away_frames,
            frames
        );
    double sim_total = 0;
    for (const auto& sample : samples)
        sim_total += sample.sim;
    const std::array<std::pair<const char*, double FrameSample::*>, 5> measures{
        {{"frame", &FrameSample::frame},
         {"work", &FrameSample::work},
         {"sim(per frame)", &FrameSample::sim},
         {"draw", &FrameSample::draw},
         {"present", &FrameSample::present}}
    };
    for (const auto& [name, field] : measures)
        std::printf(
            "%s\n", bench::measure_line(name, bench::spread(measure_values(samples, field))).c_str()
        );
    std::printf("bench: MEASURE tick(per tick)    avg %8.2f\n", ticks ? sim_total / ticks : 0.0);
    const auto& totals = bench_timers::timers.totals;
    const auto& scene = bench_timers::timers.last;
    if (view.stage_timers)
        for (std::size_t stage = 0; stage < bench_timers::stage_count; ++stage)
            std::printf(
                "%s\n",
                bench::stage_line(
                    static_cast<bench_timers::Stage>(stage), totals.milliseconds[stage], frames
                )
                    .c_str()
            );
    const auto units = count_units();
    std::printf(
        "bench: COUNTS units %d (player0 %d, others %d) drawn %u vertices %.0f terrain %.0f "
        "models %.0f sprites %.0f particles %.0f nano %.0f shadows %.0f crop_cells %.0f "
        "projectiles %zu tick %u supersample %.0f multisample %.0f battlefield %dx%d "
        "atlas_rows %.0f full_atlas_uploads %.0f card_models %.0f\n",
        units[0] + units[1],
        units[0],
        units[1],
        frame_draws_.units_drawn,
        scene.vertices,
        scene.terrain_vertices,
        scene.models,
        scene.sprites,
        scene.particles,
        scene.nano_particles,
        scene.shadows,
        scene.crop_cells,
        match_->projectiles().size(),
        match_timing_.tick,
        scene.supersample,
        bench_timers::timers.multisample,
        match_layout_.battlefield_width(),
        match_layout_.battlefield_height(),
        per_frame(totals.atlas_rows_uploaded),
        totals.whole_atlas_uploads,
        scene.card_models
    );
    std::printf("bench: GROUND revisions %.0f in %zu frames\n", totals.ground_revisions, frames);
    if (view.frame_log && match_) {
        // Cold metadata makes the actual mesh reference auditable without
        // adding geometry work or output inside a measured frame.
        const auto& world = match_->world().record;
        std::vector<uint32_t> owned(loaded_commander_types_.size());
        std::vector<uint32_t> present(owned.size());
        std::vector<oa_ref32> definitions(owned.size());
        for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
            const auto& unit = world.units[slot];
            if (oa::unit_is_live_target(unit.flags) && unit.health > 0 &&
                unit.build_remaining == 0 && unit.type_index < owned.size()) {
                ++present[unit.type_index];
                if (unit.owner_index == world.game.viewpoint_player)
                    ++owned[unit.type_index];
                definitions[unit.type_index] = unit.def;
            }
        }
        unit_detail_transition::GeometryCache mesh_cache;
        const auto geometry = mesh_cache.prepare(loaded_commander_types_);
        for (std::size_t type = 0; type < owned.size(); ++type) {
            if (present[type] == 0)
                continue;
            const auto* definition = oa::world_unit_def(&world, definitions[type]);
            const auto& dimensions = geometry[type].dimensions;
            std::printf(
                "bench: UNIT_GEOMETRY type %zu present %u owned %u rest_mesh_x %.4f rest_mesh_y "
                "%.4f "
                "rest_mesh_z %.4f footprint_x %d footprint_z %d name=%s\n",
                type,
                present[type],
                owned[type],
                dimensions[0],
                dimensions[1],
                dimensions[2],
                definition ? definition->footprint_x : 0,
                definition ? definition->footprint_z : 0,
                loaded_commander_types_[type].unit_name.c_str()
            );
        }
    }
    if (view.require_gameplay && match_models_) {
        const auto& models = *match_models_;
        size_t deferred_draws{}, preplanned_omissions{}, deferred_original{}, deferred_copies{},
            cache_bytes{};
        uint64_t cache_hits{}, cache_misses{}, cache_stores{}, cache_overflows{};
        for (const auto& draw : models.draws.models) {
            // An omitted record retains its ordinary state for a possible
            // same-frame classic replan; that state is not an active image.
            // The historical total covers both before-Scene omission paths;
            // the split below distinguishes copy preplanning from whole-tick
            // records whose complete ordinary plan was already built.
            preplanned_omissions += draw.preplan_omitted;
            deferred_draws += draw.plan.linked.drawn && !draw.preplan_omitted && draw.model.state &&
                              draw.model.state->deferred_image.has_value();
        }
        for (const auto& unit : models.units)
            deferred_original += unit.state.deferred_image.has_value();
        for (const auto& unit : models.presentation.units)
            deferred_copies += unit.state.deferred_image.has_value();
        // A worker resets its counters per planning run, not per benchmark.
        // Inactive workers can retain earlier counters; label the sum as
        // retained state rather than claiming a measured-span total.
        for (const auto& cache : models.transform_caches) {
            cache_hits += cache.counts().hits;
            cache_misses += cache.counts().misses;
            cache_stores += cache.counts().stores;
            cache_overflows += cache.counts().overflows;
            cache_bytes += cache.storage_bytes();
        }
        std::printf(
            "bench: NATIVE last_frame_planned_models %zu last_frame_deferred_draws %zu "
            "last_frame_preplanned_omissions %zu "
            "last_frame_copy_preplan_omissions %zu last_frame_whole_tick_postplan_omissions %zu "
            "retained_deferred_original_states %zu retained_deferred_copy_states %zu "
            "retained_pose_workers %zu retained_pose_hits %llu retained_pose_misses %llu "
            "retained_pose_stores %llu retained_pose_overflows %llu retained_pose_bytes %zu\n",
            models.draws.models.size(),
            deferred_draws,
            preplanned_omissions,
            models.preplanning_visibility.copy_preplan_omitted_count,
            models.preplanning_visibility.whole_tick_postplan_omitted_count,
            deferred_original,
            deferred_copies,
            models.transform_caches.size(),
            static_cast<unsigned long long>(cache_hits),
            static_cast<unsigned long long>(cache_misses),
            static_cast<unsigned long long>(cache_stores),
            static_cast<unsigned long long>(cache_overflows),
            cache_bytes
        );
    }
    std::printf(
        "bench: ATLAS terrain_packs %.1f a frame, atlas_resets %.0f in %zu frames\n",
        per_frame(totals.terrain_squares_packed),
        totals.atlas_emptied,
        frames
    );
    for (std::size_t stage = 0; stage < bench_timers::stage_count; ++stage)
        if (totals.materials_packed[stage] > 0) {
            const auto& name = bench_timers::stage_names[stage];
            const std::string shown = (name.nested ? "  " : "") + std::string(name.name);
            std::printf(
                "bench: ATLAS packed by %-34s %8.1f materials %10.0f texels a frame\n",
                shown.c_str(),
                per_frame(totals.materials_packed[stage]),
                per_frame(totals.texels_packed[stage])
            );
        }
    if (view.stage_timers && frames != 0)
        print_slowest_frames(samples, frame_totals);
    if (view.stage_timers)
        std::printf(
            "bench: GROUND_INDEX builds %.0f hits %.0f triangles %.0f wall_ms %.3f\n",
            totals.ground_index_builds,
            totals.ground_index_hits,
            totals.ground_index_triangles,
            totals.ground_index_wall_ms
        );
    if (view.stage_timers)
        std::printf(
            "bench: POSE units %.0f pieces %.0f vm_triplets %.0f\n",
            totals.pose_capture_units,
            totals.pose_capture_pieces,
            totals.pose_vm_speed_triplets
        );
    if (view.stage_timers)
        for (std::size_t kind = 0; kind < bench_timers::material_kind_count; ++kind)
            std::printf(
                "bench: MATERIAL %s insertions %.0f texels %.0f row_texels %.0f\n",
                bench_timers::material_kind_names[kind].data(),
                totals.persistent_materials[kind],
                totals.persistent_texels[kind],
                totals.persistent_row_texels[kind]
            );
    if (view.frame_log)
        for (std::size_t index = 0; index < frames; ++index) {
            const auto& sample = frame_totals[index];
            double materials = 0;
            for (const double packed : sample.materials_packed)
                materials += packed;
            std::string stages;
            for (const double milliseconds : sample.milliseconds) {
                char value[kStageValueBytes];
                std::snprintf(value, sizeof value, " %.2f", milliseconds);
                stages += value;
            }
            std::printf(
                "bench: FRAME %zu frame %.2f work %.2f draw %.2f ground %.0f terrain_packs %.0f "
                "atlas_resets %.0f atlas_rows %.0f materials %.0f stages%s\n",
                index,
                samples[index].frame,
                samples[index].work,
                samples[index].draw,
                sample.ground_revisions,
                sample.terrain_squares_packed,
                sample.atlas_emptied,
                sample.atlas_rows_uploaded,
                materials,
                stages.c_str()
            );
            std::printf(
                "%s\n",
                bench::simulation_frame_line(index, samples[index].sim, samples[index].simulation)
                    .c_str()
            );
            std::printf(
                "bench: UNIT_DETAIL_FRAME %zu automatic %d average_pixels %.4f dot_pixels %.1f "
                "model_ratio %.2f reference_units %u dots %d\n",
                index,
                samples[index].automatic_unit_detail,
                samples[index].average_unit_pixels,
                unit_detail_transition::dot_pixels,
                unit_detail_transition::model_to_dot_ratio,
                samples[index].average_unit_count,
                samples[index].units_as_dots
            );
            const auto& body = samples[index].body;
            std::printf(
                "bench: BODY_LOD_FRAME %zu full_mesh_models %u body_lod_models %u "
                "distance_markers %u budget_markers %u full_mesh_vertices %llu "
                "body_lod_vertices %llu shadow_vertices %llu deferred_geometry %llu\n",
                index,
                body.full_mesh_models,
                body.body_lod_models,
                body.distance_markers,
                body.budget_markers,
                static_cast<unsigned long long>(body.full_mesh_vertices),
                static_cast<unsigned long long>(body.body_lod_vertices),
                static_cast<unsigned long long>(body.shadow_vertices),
                static_cast<unsigned long long>(samples[index].deferred_geometry)
            );
            const auto& scene_work = samples[index].scene_work;
            std::printf(
                "bench: SCENE_WORK_FRAME %zu candidate_bands %u candidate_slots %u "
                "accepted_bands %u accepted_slots %u fallback_frames %u\n",
                index,
                scene_work.candidate_bands,
                scene_work.candidate_slots,
                scene_work.accepted_bands,
                scene_work.accepted_slots,
                scene_work.fallback_frames
            );
            print_shadow_indices(index, samples[index].shadow_indices);
            if (view.plan_work)
                print_plan_work(index, samples[index].plan_work, samples[index].root_prefix);
            std::printf(
                "bench: GROUND_INDEX_FRAME %zu builds %.0f hits %.0f triangles %.0f wall_ms %.3f\n",
                index,
                sample.ground_index_builds,
                sample.ground_index_hits,
                sample.ground_index_triangles,
                sample.ground_index_wall_ms
            );
            print_index_builds(index, samples[index].index_builds);
            std::printf(
                "bench: POSE_FRAME %zu units %.0f pieces %.0f vm_triplets %.0f\n",
                index,
                sample.pose_capture_units,
                sample.pose_capture_pieces,
                sample.pose_vm_speed_triplets
            );
            for (std::size_t kind = 0; kind < bench_timers::material_kind_count; ++kind)
                std::printf(
                    "bench: MATERIAL_FRAME %zu %s insertions %.0f texels %.0f row_texels %.0f\n",
                    index,
                    bench_timers::material_kind_names[kind].data(),
                    sample.persistent_materials[kind],
                    sample.persistent_texels[kind],
                    sample.persistent_row_texels[kind]
                );
        }
    std::fflush(stdout);
    bench_timers::reset(false);
    const double hold_end = now() + view.hold_after;
    while (now() < hold_end)
        frame();
    std::printf("bench: DONE label=%s\n", label);
    std::fflush(stdout);
}

} // namespace oa::app
