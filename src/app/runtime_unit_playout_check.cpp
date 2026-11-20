// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-unit-playout: the units of another machine's player (mirrored
// units) drawn on their playout, over the headless skirmish. The other player
// is taken as another machine's: its runner follows the path the local runner
// took beside it, placed only as that machine's records arrive, sent every six
// ticks and every fourth send late, as 3.1c sends them, each with the
// runner's speed and the head of its route. Drawn at 120 frames a second by
// the application loop's frame steps, the mirrored runner moves on every frame
// by about its pace, within two ticks of its simulated place on average,
// where its records alone hold it still and make it jump; a whole tick's
// frame keeps it on its playout; the tracking camera and the pointer's pick
// follow where it is drawn; the local runner and the world are as without
// the playout.
// With Smooth motion Off each frame draws the mirrored runner where its playout
// has it at the latest tick, so the frames of a tick show it at one place, and
// the world is as with it On.
// With no player taken as another machine's, every frame and the world are as
// without it.
#include "oa/app/runtime.hpp"
#include "engine_settings_state.hpp"
#include "match_models.hpp"
#include "presentation_interpolation.hpp"

#include "oa/base/game_loop.hpp"
#include "oa/base/sha256.hpp"
#include "oa/present/unit_playout.hpp"
#include "oa/sim/gameplay_input/input.hpp"
#include "oa/sim/match_runtime.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {
namespace {

namespace sha256 = oa::base::sha256;
namespace input = oa::sim::gameplay_input;
namespace unit_playout = oa::present::unit_playout;

/// Frames the check draws a second, as the application loop does unless --max-fps says otherwise.
constexpr uint32_t kFramesPerSecond = 120;
/// Frames drawn a tick at that rate and normal speed.
constexpr uint32_t kFramesPerTick = 4;
/// Ticks the runners are given to set off.
constexpr uint32_t kStartTicks = 30;
/// Ticks the other player's playout clock is given to settle: its delay window.
constexpr uint32_t kSettleTicks = unit_playout::delay_window_ticks;
/// Ticks the runners' frames are measured over.
constexpr uint32_t kMeasuredTicks = 120;
/// Ticks the tracking camera is checked over, after the measured ones.
constexpr uint32_t kTrackedTicks = 30;
/// Ticks of the passes with no player taken as another machine's.
constexpr uint32_t kPlainTicks = 120;
/// Units a side in those passes' fight.
constexpr std::size_t kPlainArmy = 6;
/// Ticks between the other machine's sends of its records: 200 milliseconds,
/// 3.1c's default.
constexpr uint32_t kSendTicks = 6;
/// The most ticks of its pace the mirrored runner is drawn from its simulated
/// place on average.
constexpr double kMostMeanLagTicks = 2.0;
/// Ticks a send takes to arrive.
constexpr uint32_t kSendLatency = 2;
/// Every kLateSendEvery-th send arrives kLateSendTicks later still.
constexpr uint32_t kLateSendEvery = 4;
constexpr uint32_t kLateSendTicks = 3;
/// Where the local runner starts from the local commander, map pixels.
constexpr int32_t kRunnerOffsetX = -260;
constexpr int32_t kRunnerOffsetZ = 170;
/// The fewest map pixels the runners need ahead of them, and the margin
/// they keep from the map's edges.
constexpr int32_t kRunnerTrip = 1100;
constexpr int32_t kMapMargin = 64;
/// How far beside the local runner's path the mirrored runner goes, map pixels.
constexpr int32_t kLaneGap = 48;
/// Fast units tried in turn as the runners.
constexpr std::array<std::string_view, 3> kRunnerNames{"ARMFAV", "CORFAV", "ARMFLASH"};
/// Occupancy layer of a ground unit (Match::place_unit_at).
constexpr uint8_t kGroundLayer = 1;
/// The search, in screen pixels, for points only one of two pick boxes holds.
constexpr int32_t kPickReach = 48;
/// A whole map pixel, 16.16.
constexpr int32_t kPixel = 1 << 16;
/// Map pixels a side of a map tile.
constexpr uint32_t kTilePixels = 32;

/// Ends the check with a failure.
///
/// @param what what failed
[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("unit playout check: " + what);
}

/// Ends the check with a failure unless a condition holds.
///
/// @param ok the condition
/// @param what what failed when it does not hold
void require(bool ok, const std::string& what) {
    if (!ok)
        fail(what);
}

/// Returns the tick of the newest record of the other machine's that has
/// arrived by a tick, counted from the pass's start: it sends every
/// kSendTicks ticks, each send arriving kSendLatency ticks later, every
/// kLateSendEvery-th kLateSendTicks later still.
///
/// @param tick ticks since the pass began
/// @return the newest owner tick arrived; 0 before the first
uint32_t newest_record(uint32_t tick) {
    uint32_t newest = 0;
    for (uint32_t send = 0; send * kSendTicks + kSendLatency <= tick; ++send) {
        const uint32_t late = send % kLateSendEvery == kLateSendEvery - 1 ? kLateSendTicks : 0;
        if (send * kSendTicks + kSendLatency + late <= tick)
            newest = std::max(newest, send * kSendTicks);
    }
    return newest;
}

/// One frame of a runner pass.
struct RunnerFrame {
    uint32_t tick{};         ///< ticks since the pass began
    int32_t drawn_along{};   ///< where the mirrored runner was drawn, along the trip, 16.16
    int32_t held_along{};    ///< where its newest record holds it, along the trip
    int32_t local_along{};   ///< where the local runner was drawn, along the trip
    FixedVec3 drawn{};       ///< where the mirrored runner was drawn
    FixedVec3 local_drawn{}; ///< where the local runner was drawn
    uint64_t world{};        ///< the world's digest after the frame (frame_run_digest)
};

/// What a runner pass saw.
struct RunnerPass {
    std::vector<RunnerFrame> frames; ///< the measured frames
    int64_t pace{};                  ///< the local runner's mean step a tick there, 16.16
};

/// What the frames of one quantity show: how many held still, and the
/// smallest and largest steps, as parts of the even step.
struct StepSummary {
    uint32_t steps{};
    uint32_t still{};
    double smallest{std::numeric_limits<double>::max()};
    double largest{};
};

/// Sums up the steps of a series of places against an even step.
///
/// @param places the places, one a frame, 16.16 along the trip
/// @param even the even step a frame, 16.16
/// @return the summary
StepSummary summarise(const std::vector<int32_t>& places, double even) {
    StepSummary summary{};
    for (std::size_t index = 1; index < places.size(); ++index) {
        const double step = static_cast<double>(places[index] - places[index - 1]) / even;
        ++summary.steps;
        summary.still += places[index] == places[index - 1] ? 1 : 0;
        summary.smallest = std::min(summary.smallest, step);
        summary.largest = std::max(summary.largest, step);
    }
    if (summary.steps == 0)
        summary.smallest = 0;
    return summary;
}

} // namespace

void Runtime::check_unit_playout() {
    const auto start_skirmish = [this] {
        start_benchmark_skirmish();
        match_layout_ = lay_out_match(options_.match_width, options_.match_height);
        match_zoom_ = std::clamp(options_.match_zoom, kMinBattlefieldZoom, kMaxBattlefieldZoom);
        match_zoom_target_ = match_zoom_;
    };
    const auto restart = [this] {
        if (match_tracking_)
            stop_match_tracking();
        frame_run_clock_ns_.reset();
        leave_match();
        load(Screen::main_menu);
    };

    // The frames' clock, the tracking and Smooth motion end with the check
    // however it ends.
    struct ClockEnd {
        Runtime& runtime;
        bool smooth{};

        explicit ClockEnd(Runtime& owner) : runtime(owner), smooth(owner.smooth_motion()) {}

        ClockEnd(const ClockEnd&) = delete;
        ClockEnd& operator=(const ClockEnd&) = delete;

        ~ClockEnd() {
            runtime.frame_run_clock_ns_.reset();
            runtime.engine_settings_state().current.smooth_motion = smooth;
            if (runtime.match_tracking_)
                runtime.stop_match_tracking();
        }
    } clock_end{*this};

    // Frames as the application loop draws them at 120 a second, on a clock
    // of the check's own: the camera, the frame's pump, the clock step with
    // its ticks and the unit playout's reading after each, the frame's
    // fraction and the drawing.
    uint64_t frame = 0;
    const auto begin_frames = [&] {
        frame_run_clock_ns_ = 0;
        match_timing_.previous_clock = oa::base::game_loop::scaled_clock(0, match_clock_scale());
        tick_presentation_ = {};
        scroll_clock_ = 0;
        zoom_clock_valid_ = false;
        frame_draws_ = {};
        frame = 0;
    };
    const auto draw_frame = [&](const std::function<void()>& pump) {
        frame_run_clock_ns_ =
            (2 * frame + 1) * frame_pacing::kNanosecondsPerSecond / (2 * kFramesPerSecond);
        take_frame_time();
        camera_moved_ = false;
        move_match_camera();
        pump();
        step_match_frame();
        frame_draws_.units_drawn = 0;
        frame_draws_.units_between_ticks = 0;
        frame_draws_.probe_drawn = false;
        rebuild_surface();
        presentation_alpha_ = 1.0F;
        ++frame;
    };
    const auto frame_digest = [this] { return sha256::digest_of(match_world_cpu_.rgb); };
    // The records a mirrored pass's steps apply (run_mirrored); null outside one.
    static const std::function<void()>* step_records = nullptr;
    const auto map_size = [this] {
        return std::array<int32_t, 2>{
            static_cast<int32_t>(selected_tnt_->tile_width * kTilePixels),
            static_cast<int32_t>(selected_tnt_->tile_height * kTilePixels)
        };
    };
    const auto ground_at = [this](int32_t x, int32_t z) {
        return FixedVec3{
            x * kPixel,
            match_->map_height(static_cast<uint32_t>(x) << 16U, static_cast<uint32_t>(z) << 16U) *
                kPixel,
            z * kPixel
        };
    };
    // The local commander, and the player taken as another machine's.
    const auto local_commander = [this]() -> uint16_t {
        for (const auto& slot : match_->world().slots)
            if (slot.unit != nullptr && slot.record.type_index != 0 &&
                slot.record.owner_index == match_local_player_)
                return slot.unit_index;
        fail("found no local commander");
    };
    const auto other_player = [this]() -> uint8_t {
        const auto& players = match_->state().game.players;
        for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player)
            if (player != match_local_player_ && players[player].in_use != 0)
                return player;
        fail("the skirmish has no other player");
    };

    // The local runner, sent across the map the way it has most room, and
    // the unit type it is.
    struct Trip {
        uint16_t runner{};
        uint16_t type{};
        std::array<int32_t, 2> direction{}; ///< a unit step along the trip, x and z
    };

    const auto send_runner = [&]() -> Trip {
        const uint16_t commander = local_commander();
        const auto& home = match_->world().record.units[commander].position;
        const auto map = map_size();
        const int32_t map_w = map[0];
        const int32_t map_h = map[1];
        const int32_t start_x =
            std::clamp((home.x >> 16) + kRunnerOffsetX, kMapMargin, map_w - kMapMargin);
        const int32_t start_z =
            std::clamp((home.z >> 16) + kRunnerOffsetZ, kMapMargin, map_h - kMapMargin);
        const std::array<std::array<int32_t, 2>, 4> ways{{{1, 0}, {-1, 0}, {0, 1}, {0, -1}}};
        const auto room = [&](const std::array<int32_t, 2>& way) {
            if (way[0] > 0)
                return map_w - kMapMargin - start_x;
            if (way[0] < 0)
                return start_x - kMapMargin;
            if (way[1] > 0)
                return map_h - kMapMargin - start_z;
            return start_z - kMapMargin;
        };
        const auto way = *std::max_element(ways.begin(), ways.end(), [&](auto a, auto b) {
            return room(a) < room(b);
        });
        require(
            room(way) >= kRunnerTrip,
            "the map leaves the runners " + std::to_string(room(way)) + " pixels, not " +
                std::to_string(kRunnerTrip)
        );
        Trip trip{};
        trip.direction = way;
        for (const auto name : kRunnerNames) {
            const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
            if (type == 0)
                continue;
            oa::sim::unit_spawn::Request request;
            request.player = match_local_player_;
            request.type = type;
            request.finished = true;
            request.state = kGroundOccupancyState;
            const auto at = ground_at(start_x, start_z);
            request.position = {
                static_cast<uint32_t>(at.x),
                static_cast<uint32_t>(at.y),
                static_cast<uint32_t>(at.z)
            };
            if (const auto* made = match_->create(request); made != nullptr) {
                trip.runner = made->unit_index;
                trip.type = static_cast<uint16_t>(type);
                break;
            }
        }
        require(trip.runner != 0, "found no runner type to make");
        const auto goal = ground_at(
            start_x + way[0] * (kRunnerTrip - kMapMargin),
            start_z + way[1] * (kRunnerTrip - kMapMargin)
        );
        (void)match_->issue_ground_move(trip.runner, {goal.x, goal.y, goal.z}, false);
        return trip;
    };
    const auto along = [](const Trip& trip, const FixedVec3& place) {
        return trip.direction[0] != 0 ? place.x * trip.direction[0] : place.z * trip.direction[1];
    };
    // Where the frame just drawn showed the local runner: its copies when
    // it was drawn from them, else its record.
    const auto local_drawn = [this](uint16_t runner) {
        auto& presentation = match_models().presentation;
        const auto& motion = presentation.units[runner];
        return motion.blended_draw == presentation.draw
                   ? motion.record.position
                   : match_->world().record.units[runner].position;
    };

    // Passes 1, 2 and 3: the other player taken as another machine's, its
    // runner placed as its records arrive; drawn on the playout, then as
    // without it (MatchPresentation::playout null), then on the playout with
    // Smooth motion Off, frame for frame.
    const auto run_mirrored = [&](bool on_playout, bool smooth) {
        start_skirmish();
        engine_settings_state().current.smooth_motion = smooth;
        const Trip trip = send_runner();
        const uint8_t other = other_player();
        auto& game = match_->state().game;
        auto& owner = game.players[other];
        owner.status = OA_PLAYER_STATUS_MIRRORED;
        // Allies, so that neither runner stops to fight.
        game.players[match_local_player_].alliance[other] = 1;
        owner.alliance[match_local_player_] = 1;
        // The mirrored runner starts beside the local one and goes where the
        // local runner went, a lane over.
        const std::array<int32_t, 2> lane{
            trip.direction[1] * kLaneGap * kPixel, trip.direction[0] * kLaneGap * kPixel
        };
        const auto beside = [&](const FixedVec3& place) {
            return FixedVec3{place.x + lane[0], place.y, place.z + lane[1]};
        };
        std::vector<FixedVec3> path{beside(match_->world().record.units[trip.runner].position)};
        std::vector<oa_angle> headings{match_->world().record.units[trip.runner].heading};
        // What each record shares of the runner's movement: the local
        // runner's speed and the head of its route, a lane over.
        struct Shared {
            oa::sim::unit_movement::Fixed speed{};
            uint8_t count{};
            std::array<std::array<int16_t, 2>, 3> route{};
        };
        const auto share = [&] {
            Shared shared{};
            const auto* ground = match_->ground_runtime(trip.runner);
            if (ground == nullptr)
                return shared;
            shared.speed = ground->movement.speed;
            const auto& navigation = ground->navigation;
            shared.count = static_cast<uint8_t>(std::min<uint32_t>(navigation.count, 3));
            for (std::size_t i = 0; i < shared.count; ++i)
                shared.route[i] = {
                    static_cast<int16_t>(navigation.points[i][0] + (lane[0] >> 16)),
                    static_cast<int16_t>(navigation.points[i][1] + (lane[1] >> 16))
                };
            return shared;
        };
        std::vector<Shared> shared{share()};
        oa::sim::unit_spawn::Request request;
        request.player = other;
        request.type = trip.type;
        request.finished = true;
        request.state = kGroundOccupancyState;
        request.position = {
            static_cast<uint32_t>(path[0].x),
            static_cast<uint32_t>(path[0].y),
            static_cast<uint32_t>(path[0].z)
        };
        const auto* made = match_->create(request);
        require(made != nullptr, "the mirrored runner was not made");
        const uint16_t mirrored = made->unit_index;
        const uint32_t base_tick = match_timing_.tick;
        owner.last_sim_tick = static_cast<int32_t>(base_tick);
        // The frame's pump: the camera looks at the local runner, the same in
        // both passes.
        const auto pump = [&] {
            if (match_tracking_)
                return;
            const auto& local = match_->world().record.units[trip.runner].position;
            set_camera_position(
                (local.x >> 16) - visible_map_width() / 2,
                (local.z >> 16) - visible_map_height() / 2,
                0
            );
        };
        // The other machine's records arrive within the match's steps, as an
        // extension that shares the match applies them
        // (Extension::simulation_step): each step runs the tick, then places
        // the mirrored runner where its newest record puts it, with the tick
        // that record was made at.
        const std::function<void()> records = [&] {
            const uint32_t newest = std::min<uint32_t>(
                newest_record(match_timing_.tick - base_tick),
                static_cast<uint32_t>(path.size() - 1)
            );
            const auto& place = path[newest];
            match_->place_unit_at(mirrored, place.x, place.y, place.z, kGroundLayer);
            match_->world().record.units[mirrored].heading = headings[newest];
            // The record's speed and route head, as a shared match applies them.
            if (auto* ground = match_->ground_runtime(mirrored)) {
                ground->movement.speed = shared[newest].speed;
                ground->mirrored_navigation.count = shared[newest].count;
                for (std::size_t i = 0; i < 3; ++i)
                    ground->mirrored_navigation.points[i] = shared[newest].route[i];
            }
            owner.last_sim_tick = static_cast<int32_t>(base_tick + newest);
        };
        struct StepEnd {
            Runtime& runtime;
            const Extension saved;

            StepEnd(Runtime& owner, const std::function<void()>& applied)
                : runtime(owner), saved(owner.extension_) {
                step_records = &applied;
            }

            StepEnd(const StepEnd&) = delete;
            StepEnd& operator=(const StepEnd&) = delete;

            ~StepEnd() {
                runtime.extension_ = saved;
                step_records = nullptr;
            }
        } step_end{*this, records};
        extension_.simulation_step = [](void* /*context*/, Runtime& runtime) {
            const std::function<void()>* pending = step_records;
            if (pending == nullptr)
                return false;
            // The headless step asks this hook too. With the records put
            // aside, that ask leaves the tick to the step, which runs it once.
            step_records = nullptr;
            runtime.step_match_simulation();
            step_records = pending;
            (*pending)();
            return true;
        };
        // The local runner's place at each tick, which the other machine's
        // runner reaches a few ticks later.
        const auto note_path = [&] {
            const auto& record = match_->world().record.units[trip.runner];
            while (path.size() <= match_timing_.tick - base_tick) {
                path.push_back(beside(record.position));
                headings.push_back(record.heading);
                shared.push_back(share());
            }
        };
        auto& models = match_models();
        if (!on_playout)
            models.presentation.playout = nullptr;
        begin_frames();
        frame_draws_.probe_unit = mirrored;
        RunnerPass pass;
        const uint32_t measured_from = kStartTicks + kSettleTicks;
        const uint32_t measured_to = measured_from + kMeasuredTicks;
        std::optional<FixedVec3> local_from;
        while (match_timing_.tick - base_tick < measured_to) {
            draw_frame(pump);
            note_path();
            const auto& record = match_->world().record.units[trip.runner];
            const uint32_t tick = match_timing_.tick - base_tick;
            if (tick == measured_from && !local_from)
                local_from = record.position;
            if (tick < measured_from)
                continue;
            RunnerFrame sample{};
            sample.tick = tick;
            sample.drawn = {frame_draws_.probe_x, 0, frame_draws_.probe_z};
            sample.drawn_along = along(trip, sample.drawn);
            sample.held_along = along(trip, match_->world().record.units[mirrored].position);
            sample.local_drawn = local_drawn(trip.runner);
            sample.local_along = along(trip, sample.local_drawn);
            sample.world = frame_run_digest();
            pass.frames.push_back(sample);
            require(
                frame_draws_.probe_drawn,
                "the mirrored runner was not drawn at tick " + std::to_string(tick)
            );
            if (!smooth) {
                // Off, the runner is drawn where its playout has it at the
                // latest tick, on every frame of the tick.
                const auto at_tick =
                    mirrored_pose(models, match_->state(), mirrored, {models.presentation.tick, 0});
                require(
                    at_tick && frame_draws_.probe_x == at_tick->position.x &&
                        frame_draws_.probe_z == at_tick->position.z,
                    "with Smooth motion Off the mirrored runner was not drawn on its playout at "
                    "tick " +
                        std::to_string(tick)
                );
            }
        }
        const auto& record = match_->world().record.units[trip.runner];
        require(local_from.has_value(), "the runners' measured ticks were not reached");
        pass.pace =
            (static_cast<int64_t>(along(trip, record.position)) - along(trip, *local_from)) /
            kMeasuredTicks;
        if (!on_playout || !smooth)
            return pass;

        // The camera tracking the mirrored runner centres where each frame
        // draws it.
        begin_match_tracking(mirrored);
        for (uint32_t end = match_timing_.tick + kTrackedTicks; match_timing_.tick < end;) {
            draw_frame(pump);
            note_path();
            require(frame_draws_.probe_drawn, "the tracked mirrored runner was not drawn");
            require(
                match_camera_x_ == (frame_draws_.probe_x >> 16) - visible_map_width() / 2 &&
                    match_camera_z_ == (frame_draws_.probe_z >> 16) - visible_map_height() / 2,
                "the tracking camera is at " + std::to_string(match_camera_x_) + "," +
                    std::to_string(match_camera_z_) + ", not centred on the mirrored runner at " +
                    std::to_string(frame_draws_.probe_x >> 16) + "," +
                    std::to_string(frame_draws_.probe_z >> 16)
            );
        }

        // The pointer picks the mirrored runner where the frame drew it: a
        // point in its box there picks it, a point only in its box at its
        // simulated place does not.
        const auto drawn_pose =
            mirrored_pose(models, match_->state(), mirrored, models.presentation.drawn_moment);
        require(drawn_pose.has_value(), "the mirrored runner has no pose on the playout");
        auto* instance = match_->instance(mirrored);
        require(instance != nullptr, "the mirrored runner has no model");
        const auto& held = match_->world().record.units[mirrored];
        input::PickUnit shown_box;
        shown_box.id = mirrored;
        shown_box.position = {
            drawn_pose->position.x, drawn_pose->position.y, drawn_pose->position.z
        };
        shown_box.rotation = {
            drawn_pose->bank, static_cast<int16_t>(drawn_pose->heading), drawn_pose->pitch
        };
        shown_box.model = &instance->model().model();
        input::PickUnit held_box = shown_box;
        held_box.position = {held.position.x, held.position.y, held.position.z};
        held_box.rotation = {held.bank, static_cast<int16_t>(held.heading), held.pitch};
        const input::Camera camera{
            static_cast<int32_t>(game.camera_x), static_cast<int32_t>(game.camera_y)
        };
        std::optional<input::ScreenPoint> shown_only;
        std::optional<input::ScreenPoint> held_only;
        for (const auto* box : {&shown_box, &held_box}) {
            const auto centre = input::project({}, box->position, camera);
            for (int32_t dy = -kPickReach; dy <= kPickReach; ++dy)
                for (int32_t dx = -kPickReach; dx <= kPickReach; ++dx) {
                    const input::ScreenPoint point{centre.x + dx, centre.y + dy};
                    const bool in_shown = input::hits_root_bounds(shown_box, camera, point);
                    const bool in_held = input::hits_root_bounds(held_box, camera, point);
                    if (in_shown && !in_held && !shown_only)
                        shown_only = point;
                    if (in_held && !in_shown && !held_only)
                        held_only = point;
                }
        }
        require(
            shown_only && held_only,
            "the mirrored runner's box where it is drawn and where it is held are the same"
        );
        const auto pick_at = [&](input::ScreenPoint point) {
            const auto canvas = game_screen_canvas(point.x, point.y);
            pointer_x_ = static_cast<float>(canvas.x);
            pointer_y_ = static_cast<float>(canvas.y);
            pick_cursor_unit(false);
            return hovered_match_unit_;
        };
        require(
            pick_at(*shown_only) == mirrored,
            "a point in the mirrored runner's box where it is drawn did not pick it"
        );
        require(
            pick_at(*held_only) != mirrored,
            "a point only in the mirrored runner's box at its simulated place picked it"
        );
        std::printf(
            "unit playout: the pointer picks the mirrored runner where it is drawn, %d pixels from "
            "its simulated place\n",
            std::abs(along(trip, drawn_pose->position) - along(trip, held.position)) >> 16
        );

        // A frame of the whole tick draws the mirrored runner on its playout
        // at the tick, not at its simulated place.
        stop_match_tracking();
        const auto before_whole = frame_draws_.probe_x;
        const auto before_whole_z = frame_draws_.probe_z;
        set_presentation_alpha(1.0F);
        frame_draws_.probe_drawn = false;
        render_match_surface();
        const auto at_tick =
            mirrored_pose(models, match_->state(), mirrored, {models.presentation.tick, 0});
        require(
            frame_draws_.probe_drawn && at_tick && frame_draws_.probe_x == at_tick->position.x &&
                frame_draws_.probe_z == at_tick->position.z,
            "a frame of the whole tick did not draw the mirrored runner on its playout"
        );
        const int32_t whole_step = std::abs(
            along(trip, {frame_draws_.probe_x, 0, frame_draws_.probe_z}) -
            along(trip, {before_whole, 0, before_whole_z})
        );
        require(
            whole_step <= 2 * static_cast<int32_t>(std::abs(pass.pace)),
            "a frame of the whole tick moved the mirrored runner " +
                std::to_string(whole_step >> 16) + " pixels from the frame before"
        );
        std::printf(
            "unit playout: a whole tick's frame draws the mirrored runner on its playout, %d "
            "pixels from its simulated place\n",
            std::abs(
                along(trip, at_tick->position) -
                along(trip, match_->world().record.units[mirrored].position)
            ) >> 16
        );
        return pass;
    };

    const RunnerPass shown = run_mirrored(true, true);
    restart();
    const RunnerPass plain = run_mirrored(false, true);
    restart();
    const RunnerPass ticked = run_mirrored(true, false);
    restart();
    require(
        shown.frames.size() == plain.frames.size() && !shown.frames.empty(),
        "the passes drew " + std::to_string(shown.frames.size()) + " and " +
            std::to_string(plain.frames.size()) + " frames"
    );
    require(shown.pace > kPixel, "the local runner did not run");
    std::vector<int32_t> playout_places;
    std::vector<int32_t> held_places;
    std::vector<int32_t> plain_places;
    std::vector<int32_t> local_places;
    double lag_total = 0;
    double lag_most = 0;
    double lag_least = std::numeric_limits<double>::max();
    for (std::size_t index = 0; index < shown.frames.size(); ++index) {
        const auto& with = shown.frames[index];
        const auto& without = plain.frames[index];
        require(
            with.world == without.world,
            "drawing the mirrored runner on its playout changed the world at tick " +
                std::to_string(with.tick)
        );
        require(
            with.local_drawn.x == without.local_drawn.x &&
                with.local_drawn.z == without.local_drawn.z,
            "the local runner was drawn elsewhere with the playout at tick " +
                std::to_string(with.tick)
        );
        playout_places.push_back(with.drawn_along);
        held_places.push_back(with.held_along);
        plain_places.push_back(without.drawn_along);
        local_places.push_back(with.local_along);
        const double lag = static_cast<double>(with.held_along - with.drawn_along) /
                           static_cast<double>(shown.pace);
        lag_total += lag;
        lag_most = std::max(lag_most, lag);
        lag_least = std::min(lag_least, lag);
    }
    const double even = static_cast<double>(shown.pace) / kFramesPerTick;
    const auto report = [&](const char* what, const StepSummary& summary) {
        std::printf(
            "unit playout: %s: %u steps, %u still, %.2f to %.2f of the even step\n",
            what,
            summary.steps,
            summary.still,
            summary.smallest,
            summary.largest
        );
    };
    const auto on_playout = summarise(playout_places, even);
    const auto without_playout = summarise(plain_places, even);
    const auto held = summarise(held_places, even);
    const auto local = summarise(local_places, even);
    std::printf(
        "unit playout: the runners run %.2f pixels a tick; %zu frames measured at %u a second\n",
        static_cast<double>(shown.pace) / kPixel,
        shown.frames.size(),
        kFramesPerSecond
    );
    report("the mirrored runner on its playout", on_playout);
    report("the mirrored runner without it", without_playout);
    report("the mirrored runner's simulated place", held);
    report("the local runner", local);
    const double lag_mean = lag_total / static_cast<double>(shown.frames.size());
    std::printf(
        "unit playout: drawn %.2f to %.2f ticks behind its simulated place (below 0 ahead of "
        "it), %.2f on average\n",
        lag_least,
        lag_most,
        lag_mean
    );
    std::fflush(stdout);
    // Records alone hold the mirrored runner still on most frames and then
    // jump it on; on its playout it moves on every frame, by three quarters
    // to one and a half of its pace, kept here within a half and twice, moved
    // on ahead of its newest record between them and drawn within two ticks
    // of its simulated place on average.
    require(
        without_playout.still * 2 >= without_playout.steps && without_playout.largest >= 3.0,
        "the mirrored runner's records did not arrive in bursts"
    );
    require(on_playout.still == 0, "the mirrored runner held still on its playout");
    require(
        on_playout.smallest >= 0.5 && on_playout.largest <= 2.0,
        "the mirrored runner did not move evenly on its playout"
    );
    require(
        lag_least >= -static_cast<double>(unit_playout::predict_ticks) && lag_least < 0.0 &&
            lag_most <= static_cast<double>(unit_playout::max_delay_ticks + kSendTicks) &&
            std::abs(lag_mean) <= kMostMeanLagTicks,
        "the mirrored runner was not drawn near its simulated place, moved on ahead of it"
    );

    // With Smooth motion Off the world is as with it On, frame for frame,
    // and the mirrored runner, drawn on its playout at each tick, holds its
    // place over the frames of a tick and moves on once a tick.
    require(
        ticked.frames.size() == shown.frames.size(),
        "the pass with Smooth motion Off drew " + std::to_string(ticked.frames.size()) +
            " frames, not " + std::to_string(shown.frames.size())
    );
    std::vector<int32_t> ticked_places;
    for (std::size_t index = 0; index < ticked.frames.size(); ++index) {
        const auto& off = ticked.frames[index];
        const auto& on = shown.frames[index];
        require(
            off.tick == on.tick && off.world == on.world,
            "Smooth motion Off changed the world at tick " + std::to_string(off.tick)
        );
        if (index > 0 && ticked.frames[index - 1].tick == off.tick)
            require(
                ticked.frames[index - 1].drawn_along == off.drawn_along,
                "with Smooth motion Off the mirrored runner moved within tick " +
                    std::to_string(off.tick)
            );
        ticked_places.push_back(off.drawn_along);
    }
    const auto at_ticks = summarise(ticked_places, even);
    report("the mirrored runner on its playout, Smooth motion Off", at_ticks);
    std::fflush(stdout);
    require(
        at_ticks.still * 2 >= at_ticks.steps && at_ticks.largest > 0.0,
        "with Smooth motion Off the mirrored runner did not move on once a tick"
    );

    // Passes 4 and 5: the skirmish's fight with no player taken as another
    // machine's. Each frame, and the world, is the same with the playout and
    // without it.
    const auto run_plain = [&](bool on_playout) {
        start_skirmish();
        spawn_combat_armies(kPlainArmy);
        (void)send_runner();
        auto& models = match_models();
        if (!on_playout)
            models.presentation.playout = nullptr;
        begin_frames();
        std::vector<sha256::Digest> frames;
        std::vector<uint64_t> worlds;
        const uint32_t base_tick = match_timing_.tick;
        while (match_timing_.tick - base_tick < kPlainTicks) {
            draw_frame([] {});
            frames.push_back(frame_digest());
            worlds.push_back(frame_run_digest());
        }
        // No unit of the skirmish is drawn on the playout.
        const auto& world = match_->state();
        for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot)
            require(
                !mirrored_pose(
                     match_models(),
                     world,
                     static_cast<uint16_t>(slot),
                     match_models().presentation.drawn_moment
                )
                     .has_value(),
                "unit " + std::to_string(slot) + " of the skirmish was drawn on the playout"
            );
        return std::pair{frames, worlds};
    };
    const auto with_playout = run_plain(true);
    restart();
    const auto without = run_plain(false);
    restart();
    require(
        with_playout.first.size() == without.first.size(),
        "the passes with no other machine drew different numbers of frames"
    );
    for (std::size_t index = 0; index < with_playout.first.size(); ++index) {
        require(
            with_playout.first[index] == without.first[index],
            "with no other machine's player, frame " + std::to_string(index) +
                " differs with the playout"
        );
        require(
            with_playout.second[index] == without.second[index],
            "with no other machine's player, the world differs with the playout at frame " +
                std::to_string(index)
        );
    }
    std::printf(
        "unit playout: %zu frames drew the other machine's runner evenly on its playout, with the "
        "local runner and the world as without it, and %zu frames with no other machine's player "
        "as without it\n",
        shown.frames.size(),
        with_playout.first.size()
    );
    std::fflush(stdout);
}

} // namespace oa::app
