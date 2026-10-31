// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-interpolation: frames drawn between ticks over the headless
// skirmish's fight. Such frames leave the world, and every frame of a whole
// tick, as they are without them; a unit, its turret and a projectile show
// part of the way from one tick to the next; and a moving unit, and the
// ground under a camera that tracks it, move evenly from frame to frame
// where whole ticks alone move them in steps of the tick rate. Drawn as the
// application loop draws them, with Smooth motion Off every frame shows its
// tick whole, so a tick's frames repeat one picture, and the world is as
// with it On.
#include "oa/app/runtime.hpp"
#include "engine_settings_state.hpp"
#include "match_models.hpp"
#include "presentation_interpolation.hpp"

#include "oa/base/game_loop.hpp"
#include "oa/base/sha256.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/state_hash.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {
namespace {

namespace sha256 = oa::base::sha256;

/// Units a side in the check's fight.
constexpr size_t kCheckArmy = 10;
/// Ticks each pass of the frames compared plays.
constexpr uint32_t kCheckTicks = 150;
/// Frames drawn a tick: 120 frames a second at 30 ticks.
constexpr uint32_t kFramesPerTick = 4;
/// Ticks the runner may take to set off in a straight line.
constexpr uint32_t kRunnerStartTicks = 150;
/// Ticks the runner's frames are measured over.
constexpr uint32_t kRunnerTicks = 30;
/// The fewest map pixels a tick the runner must cover for its frames to be measured.
constexpr int32_t kRunnerSpeed = 2;
/// How far the runner is sent, in map pixels, and where it starts from the fight.
constexpr int32_t kRunnerTrip = 600;
constexpr int32_t kRunnerOffsetX = -260;
constexpr int32_t kRunnerOffsetZ = 170;
/// Fast units tried in turn as the runner; the commander runs without one.
constexpr std::array<std::string_view, 3> kRunnerNames{"ARMFAV", "CORFAV", "ARMFLASH"};
/// The farthest the ground is searched for between two frames, in pixels.
constexpr int32_t kGroundSearch = 8;
/// Pixels between the samples the ground search compares, and the margin it keeps.
constexpr int32_t kGroundSampleStep = 4;
constexpr int32_t kGroundMargin = 16;
/// Pixels a measured place may stray from the place expected: the box and
/// the bars round to whole pixels.
constexpr int32_t kPlaceTolerance = 1;
/// Bytes a pixel.
constexpr size_t kRgbBytes = 3;
/// Frames the application loop draws a second, unless --max-fps says otherwise.
constexpr uint32_t kLoopFramesPerSecond = 120;
/// The most frames the loop's passes draw: twice the frames their ticks
/// take, so that a match clock that stopped stepping ends them.
constexpr uint64_t kLoopFrameLimit =
    uint64_t{kCheckTicks + 1} * kLoopFramesPerSecond / frame_pacing::kTicksPerSecond * 2;

/// Ends the check with a failure.
///
/// @param what what failed
[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("interpolation check: " + what);
}

/// Ends the check with a failure unless a condition holds.
///
/// @param ok the condition
/// @param what what failed when it does not hold
void require(bool ok, const std::string& what) {
    if (!ok)
        fail(what);
}

/// An inclusive pixel rectangle; empty when right < left.
struct PixelBox {
    int32_t left{std::numeric_limits<int32_t>::max()};
    int32_t top{std::numeric_limits<int32_t>::max()};
    int32_t right{std::numeric_limits<int32_t>::min()};
    int32_t bottom{std::numeric_limits<int32_t>::min()};

    /// Tells whether the box holds no pixel.
    ///
    /// @return true when right < left
    [[nodiscard]] bool empty() const noexcept { return right < left; }

    /// Returns twice the centre's column, so that it stays whole.
    ///
    /// @return left + right
    [[nodiscard]] int32_t centre_x2() const noexcept { return left + right; }

    /// Returns twice the centre's row.
    ///
    /// @return top + bottom
    [[nodiscard]] int32_t centre_y2() const noexcept { return top + bottom; }

    /// Tells whether two boxes are the same.
    ///
    /// @return true when every edge is equal
    [[nodiscard]] bool operator==(const PixelBox&) const noexcept = default;
};

/// Returns the box around every pixel two frames of one size differ in.
///
/// @param a a frame, three bytes a pixel
/// @param b another frame of the same size
/// @param width the frames' width, pixels
/// @return the box; empty when they are the same
PixelBox
differing_pixels(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t width) {
    PixelBox box{};
    if (a.size() != b.size() || width == 0)
        return box;
    for (size_t pixel = 0; pixel * kRgbBytes < a.size(); ++pixel) {
        const size_t at = pixel * kRgbBytes;
        if (a[at] == b[at] && a[at + 1] == b[at + 1] && a[at + 2] == b[at + 2])
            continue;
        const auto x = static_cast<int32_t>(pixel % width);
        const auto y = static_cast<int32_t>(pixel / width);
        box.left = std::min(box.left, x);
        box.right = std::max(box.right, x);
        box.top = std::min(box.top, y);
        box.bottom = std::max(box.bottom, y);
    }
    return box;
}

/// Returns how far the picture moved from one frame to the next: the shift
/// that best lays the second over the first, searched within kGroundSearch
/// pixels on a sparse grid of samples.
///
/// @param before the earlier frame, three bytes a pixel
/// @param after the later frame
/// @param width the frames' width, pixels
/// @param height the frames' height, rows
/// @return the shift in x and y, pixels
std::array<int32_t, 2> ground_shift(
    const std::vector<uint8_t>& before,
    const std::vector<uint8_t>& after,
    uint32_t width,
    uint32_t height
) {
    const auto w = static_cast<int32_t>(width);
    const auto h = static_cast<int32_t>(height);
    int64_t best = std::numeric_limits<int64_t>::max();
    std::array<int32_t, 2> shift{};
    const int32_t edge = kGroundMargin + kGroundSearch;
    for (int32_t dy = -kGroundSearch; dy <= kGroundSearch; ++dy)
        for (int32_t dx = -kGroundSearch; dx <= kGroundSearch; ++dx) {
            int64_t sum = 0;
            for (int32_t y = edge; y < h - edge; y += kGroundSampleStep)
                for (int32_t x = edge; x < w - edge; x += kGroundSampleStep) {
                    const size_t first = (static_cast<size_t>(y) * width + x) * kRgbBytes;
                    const size_t second =
                        (static_cast<size_t>(y - dy) * width + (x - dx)) * kRgbBytes;
                    for (size_t channel = 0; channel < kRgbBytes; ++channel)
                        sum += std::abs(
                            static_cast<int32_t>(before[first + channel]) -
                            static_cast<int32_t>(after[second + channel])
                        );
                }
            if (sum < best) {
                best = sum;
                shift = {dx, dy};
            }
        }
    return shift;
}

/// What a series of per-frame steps shows: how many frames held still while
/// the thing moved, and the largest and smallest steps.
struct StepSummary {
    uint32_t frames{};
    uint32_t still{};
    int32_t largest{};
    int32_t smallest{std::numeric_limits<int32_t>::max()};
    int64_t total{};
};

/// Sums up per-frame steps along their main axis.
///
/// @param steps the steps, pixels a frame
/// @return the summary
StepSummary summarise(const std::vector<int32_t>& steps) {
    StepSummary summary{};
    for (const int32_t step : steps) {
        const int32_t size = std::abs(step);
        ++summary.frames;
        summary.still += size == 0 ? 1 : 0;
        summary.largest = std::max(summary.largest, size);
        summary.smallest = std::min(summary.smallest, size);
        summary.total += size;
    }
    if (summary.frames == 0)
        summary.smallest = 0;
    return summary;
}

/// Writes a series of steps as text: "steps: 0 0 0 4 0 ...".
///
/// @param steps the steps
/// @return the text
std::string step_text(const std::vector<int32_t>& steps) {
    std::string text;
    for (const int32_t step : steps)
        text += (text.empty() ? "" : " ") + std::to_string(step);
    return text;
}

} // namespace

void Runtime::check_interpolation() {
    // The headless skirmish with the fight --combat sets up.
    const auto start_fight = [this] {
        start_benchmark_skirmish();
        match_layout_ = lay_out_match(options_.match_width, options_.match_height);
        match_zoom_ = std::clamp(options_.match_zoom, kMinBattlefieldZoom, kMaxBattlefieldZoom);
        match_zoom_target_ = match_zoom_;
        spawn_combat_armies(kCheckArmy);
    };
    const auto restart = [this] {
        if (match_tracking_)
            stop_match_tracking();
        leave_match();
        load(Screen::main_menu);
    };
    const auto step_tick = [this] {
        try {
            step_match_simulation();
        } catch (const std::exception& error) {
            report_match_tick_error(error.what());
        }
    };
    const auto draw_at = [this](float alpha) {
        set_presentation_alpha(alpha);
        try {
            render_match_surface();
        } catch (...) {
            set_presentation_alpha(1.0F);
            throw;
        }
        set_presentation_alpha(1.0F);
    };
    // The battlefield layer: the world and what is painted over it (health
    // bars, labels, boxes). The interface's resource readout eases toward the
    // stores once a draw, as it always has, so the interface is left out.
    const auto frame_digest = [this] { return sha256::digest_of(match_world_cpu_.rgb); };
    // The world a save keeps, less the meteor-storm state, which the runtime
    // keeps from one match to the next: the passes are matches one after
    // another.
    const auto world_digest = [this] {
        return oa::sim::trace::match_state_hash(
            *match_, match_timing_, match_camera_x_, match_camera_z_, nullptr
        );
    };
    const auto between = [](uint32_t frame) {
        return static_cast<float>(frame) / static_cast<float>(kFramesPerTick);
    };
    const auto models = [this]() -> MatchModels& { return match_models(); };

    // Pass 1: a frame of each whole tick alone, as before frames between
    // ticks were drawn.
    start_fight();
    std::vector<sha256::Digest> whole_frames;
    std::vector<uint64_t> worlds;
    for (uint32_t tick = 0; tick < kCheckTicks; ++tick) {
        step_tick();
        draw_at(1.0F);
        whole_frames.push_back(frame_digest());
        worlds.push_back(world_digest());
    }
    restart();

    // Pass 2: three frames between ticks before each whole tick's.
    start_fight();
    uint64_t drawn_between = 0;
    size_t units_between = 0;
    int64_t between_ns = 0;
    int64_t whole_ns = 0;
    const auto timed = [&](float alpha, int64_t& spent) {
        const auto start = std::chrono::steady_clock::now();
        draw_at(alpha);
        spent += std::chrono::duration_cast<std::chrono::nanoseconds>(
                     std::chrono::steady_clock::now() - start
        )
                     .count();
    };
    for (uint32_t tick = 0; tick < kCheckTicks; ++tick) {
        step_tick();
        for (uint32_t frame = 1; frame < kFramesPerTick; ++frame) {
            timed(between(frame), between_ns);
            ++drawn_between;
        }
        for (const auto& motion : models().presentation.units)
            units_between += motion.seen && motion.moved ? 1 : 0;
        timed(1.0F, whole_ns);
        require(
            frame_digest() == whole_frames[tick],
            "the frame of tick " + std::to_string(tick + 1) +
                " drawn after frames between ticks is not the frame drawn without them"
        );
        require(
            world_digest() == worlds[tick],
            "frames between ticks changed the world by tick " + std::to_string(tick + 1)
        );
    }
    require(units_between != 0, "no unit moved between two ticks of the fight");
    restart();

    // Pass 3: frames between ticks alone, never a whole tick, as a fast
    // frame loop draws; then the last tick whole.
    start_fight();
    for (uint32_t tick = 0; tick < kCheckTicks; ++tick) {
        step_tick();
        for (uint32_t frame = 1; frame < kFramesPerTick; ++frame)
            draw_at(between(frame));
        require(
            world_digest() == worlds[tick],
            "frames between ticks alone changed the world by tick " + std::to_string(tick + 1)
        );
    }
    draw_at(1.0F);
    require(
        frame_digest() == whole_frames.back(),
        "the last tick drawn whole after frames between ticks alone is not its frame"
    );

    // The fight goes on: units, pieces and projectiles show between their
    // two poses. A turret that turned shows half way round at half a tick.
    bool turned_piece = false;
    bool moved_shot = false;
    for (uint32_t tick = 0; tick < kCheckTicks && !(turned_piece && moved_shot); ++tick) {
        step_tick();
        draw_at(0.5F);
        auto& presentation = models().presentation;
        const uint32_t now = match_->simulation().tick;
        for (uint32_t slot = 1; slot < presentation.units.size(); ++slot) {
            const auto& motion = presentation.units[slot];
            if (!motion.seen || !motion.moved || motion.blended_draw != presentation.draw)
                continue;
            const auto expected =
                blend_point(motion.previous.position, motion.current.position, whole_tick / 2);
            require(
                motion.record.position.x == expected.x && motion.record.position.z == expected.z,
                "unit " + std::to_string(slot) + " is not drawn half way between its places"
            );
            const auto pieces = motion.instance.pieces();
            for (size_t piece = 0; piece < pieces.size(); ++piece) {
                const auto from = motion.previous.pieces[piece].rotation.xz;
                const auto to = motion.current.pieces[piece].rotation.xz;
                const auto turn = static_cast<int16_t>(static_cast<uint16_t>(to - from));
                if (std::abs(static_cast<int32_t>(turn)) < 4)
                    continue;
                require(
                    pieces[piece].rotation.xz == blend_angle(from, to, whole_tick / 2),
                    "a piece of unit " + std::to_string(slot) + " is not drawn half way round"
                );
                require(
                    pieces[piece].rotation.xz != from && pieces[piece].rotation.xz != to,
                    "a turning piece of unit " + std::to_string(slot) + " jumps"
                );
                turned_piece = true;
            }
            // The match's own instance keeps the tick's pose.
            auto* live = match_->instance(static_cast<uint16_t>(slot));
            require(
                live != nullptr && live->model().pieces().size() == motion.current.pieces.size() &&
                    std::equal(
                        live->model().pieces().begin(),
                        live->model().pieces().end(),
                        motion.current.pieces.begin(),
                        [](const auto& piece, const auto& pose) {
                            return piece.rotation.xz == pose.rotation.xz &&
                                   piece.translation.x == pose.translation.x;
                        }
                    ),
                "drawing unit " + std::to_string(slot) + " between ticks moved its own pieces"
            );
        }
        const auto shots = match_->projectiles();
        for (size_t index = 0; index < shots.size(); ++index) {
            const auto shown =
                presented_shot(presentation.shots, now, index, shots[index], whole_tick / 2);
            if (shown.position.x != shots[index].position.x ||
                shown.position.z != shots[index].position.z)
                moved_shot = true;
        }
    }
    require(turned_piece, "no piece turned in the fight");
    require(moved_shot, "no projectile showed between two ticks");
    // Frames of the fight for the eye beside --snapshot: each tick drawn at
    // 120 frames a second between ticks, and each tick's whole frame drawn
    // as often, as before.
    const fs::path stem = [&] {
        fs::path path = options_.snapshot;
        path.replace_extension();
        return path;
    }();
    if (!options_.snapshot.empty()) {
        uint32_t written = 0;
        for (uint32_t tick = 0; tick < kRunnerTicks; ++tick) {
            step_tick();
            for (uint32_t frame = 1; frame <= kFramesPerTick; ++frame, ++written) {
                char file[64];
                draw_at(between(frame));
                std::snprintf(file, sizeof file, "-fight-between-ticks-%04u.ppm", written);
                write_ppm(fs::path(stem) += file, surface_);
                draw_at(1.0F);
                std::snprintf(file, sizeof file, "-fight-whole-ticks-%04u.ppm", written);
                write_ppm(fs::path(stem) += file, surface_);
            }
        }
    }
    restart();

    // Pass 4: a runner sent across the map, its selection box measured.
    start_fight();
    const auto& slots = match_->world().slots;
    uint16_t runner = 0;
    std::array<int32_t, 2> centre{};
    for (const auto& slot : slots)
        if (slot.unit != nullptr && slot.record.type_index != 0 &&
            slot.record.owner_index == match_local_player_) {
            centre = {
                static_cast<int32_t>(slot.unit->position[0] >> 16),
                static_cast<int32_t>(slot.unit->position[2] >> 16)
            };
            runner = slot.unit_index;
            break;
        }
    require(runner != 0, "found no local commander");
    const auto map_w = static_cast<int32_t>(selected_tnt_->tile_width * 32U);
    const auto map_h = static_cast<int32_t>(selected_tnt_->tile_height * 32U);
    const int32_t start_x = std::clamp(centre[0] + kRunnerOffsetX, 64, map_w - 64);
    const int32_t start_z = std::clamp(centre[1] + kRunnerOffsetZ, 64, map_h - 64);
    for (const auto name : kRunnerNames) {
        const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
        if (type == 0)
            continue;
        oa::sim::unit_spawn::Request request;
        request.player = match_local_player_;
        request.type = type;
        request.finished = true;
        request.state = kGroundOccupancyState;
        request.position = {
            static_cast<uint32_t>(start_x) << 16,
            static_cast<uint32_t>(match_->map_height(
                static_cast<uint32_t>(start_x) << 16, static_cast<uint32_t>(start_z) << 16
            )) << 16,
            static_cast<uint32_t>(start_z) << 16
        };
        if (const auto* made = match_->create(request); made != nullptr) {
            runner = made->unit_index;
            break;
        }
    }
    auto& runner_record = match_->world().record.units[runner];
    const int32_t goal_x = std::clamp(
        static_cast<int32_t>(runner_record.position.x >> 16) + kRunnerTrip, 64, map_w - 64
    );
    const int32_t goal_z = static_cast<int32_t>(runner_record.position.z >> 16);
    (void)match_->issue_ground_move(
        runner,
        {goal_x * 0x10000,
         match_->map_height(
             static_cast<uint32_t>(goal_x) << 16, static_cast<uint32_t>(goal_z) << 16
         ) * 0x10000,
         goal_z * 0x10000},
        false
    );
    // The runner sets off; then its frames are drawn from where the camera
    // looks at it, still.
    const auto runner_at = [&] { return runner_record.position; };
    bool running = false;
    for (uint32_t tick = 0; tick < kRunnerStartTicks && !running; ++tick) {
        const auto before = runner_at();
        const auto heading = runner_record.heading;
        step_tick();
        const auto after = runner_at();
        running = std::abs((after.x >> 16) - (before.x >> 16)) >= kRunnerSpeed &&
                  heading == runner_record.heading;
    }
    require(running, "the runner did not set off in a straight line");
    center_camera_on_unit(runner);
    const int32_t still_camera_x = match_camera_x_;
    const int32_t still_camera_z = match_camera_z_;

    // The runner's box: the pixels a frame differs in with the runner
    // selected and without.
    const auto selection_box = [&](float alpha) {
        const uint32_t kept = runner_record.flags;
        runner_record.flags = kept | OA_UNIT_FLAG_SELECTED;
        draw_at(alpha);
        const auto selected = match_world_cpu_.rgb;
        runner_record.flags = kept & ~OA_UNIT_FLAG_SELECTED;
        draw_at(alpha);
        runner_record.flags = kept;
        return differing_pixels(selected, match_world_cpu_.rgb, match_world_cpu_.width);
    };
    // Drawn at none, half and all of a tick, the runner is where the tick
    // before drew it, half way, and where the tick draws it.
    const PixelBox previous = selection_box(1.0F);
    require(!previous.empty(), "the runner's selection box was not drawn");
    step_tick();
    const PixelBox at_none = selection_box(0.0F);
    const PixelBox at_half = selection_box(0.5F);
    const PixelBox at_whole = selection_box(1.0F);
    require(
        at_none == previous, "the runner at none of a tick is not where the tick before drew it"
    );
    require(!(at_whole == previous), "the runner did not move in the tick");
    const auto near_middle = [&](int32_t from, int32_t to, int32_t middle) {
        return std::abs(2 * middle - (from + to)) <= 4 * kPlaceTolerance;
    };
    require(
        near_middle(previous.centre_x2(), at_whole.centre_x2(), at_half.centre_x2()) &&
            near_middle(previous.centre_y2(), at_whole.centre_y2(), at_half.centre_y2()),
        "the runner at half a tick is not half way between its places"
    );
    std::printf(
        "interpolation: runner %u at 0, 1/2 and 1 of a tick: box centres x %.1f %.1f %.1f "
        "(the tick before %.1f)\n",
        static_cast<unsigned>(runner),
        at_none.centre_x2() / 2.0,
        at_half.centre_x2() / 2.0,
        at_whole.centre_x2() / 2.0,
        previous.centre_x2() / 2.0
    );

    // Frame by frame at 120 frames a second: the runner's box under a still
    // camera, and the ground under a camera tracking it, whole ticks alone
    // (each tick's frame drawn four times, as before) and with frames
    // between ticks. Each is held against the even path: the runner's place
    // part of the way from one tick's place to the next at each frame.
    struct Series {
        std::vector<int32_t> unit_steps;   ///< the runner's box, pixels a frame
        std::vector<int32_t> unit_strays;  ///< the box off the even path, pixels
        std::vector<int32_t> ground_steps; ///< the ground under the tracking camera
        std::vector<int32_t> ground_strays;
    };

    const auto even_x = [&](const FixedVec3& from, const FixedVec3& to, uint32_t frame) {
        const auto place = blend_point(from, to, frame * (whole_tick / kFramesPerTick));
        return project_match_point(
                   live_viewport(match_camera_x_, match_camera_z_),
                   oa::sim::match_runtime::fixed_words(place)
        )
            .x;
    };
    const auto measure = [&](bool interpolate, const char* name) {
        Series series;
        std::optional<int32_t> offset;
        std::optional<int32_t> last_centre;
        std::optional<int32_t> last_even_camera;
        std::vector<uint8_t> last_ground;
        uint32_t written = 0;
        for (uint32_t tick = 0; tick < kRunnerTicks; ++tick) {
            const auto from = runner_at();
            step_tick();
            const auto to = runner_at();
            for (uint32_t frame = 1; frame <= kFramesPerTick; ++frame) {
                const float alpha = interpolate ? between(frame) : 1.0F;
                // A still camera: the runner moves over the ground.
                stop_match_tracking();
                set_camera_position(still_camera_x, still_camera_z, 0);
                const PixelBox box = selection_box(alpha);
                require(!box.empty(), "the runner left the still camera's view");
                const int32_t centre = box.centre_x2() / 2;
                const int32_t even = even_x(from, to, frame);
                if (!offset)
                    offset = centre - even;
                series.unit_strays.push_back(centre - even - *offset);
                if (last_centre)
                    series.unit_steps.push_back(centre - *last_centre);
                last_centre = centre;
                // A tracking camera, placed as the application loop places
                // it: centred where a tick holds the runner before the
                // frame's clock step (on the frame that runs the tick, the
                // tick before), then where the frame shows it
                // (place_tracking_camera). The ground moves under the runner.
                begin_match_tracking(runner);
                if (frame == 1)
                    set_camera_position(
                        (from.x >> 16) - visible_map_width() / 2,
                        (from.z >> 16) - visible_map_height() / 2,
                        0
                    );
                set_presentation_alpha(alpha);
                place_tracking_camera();
                const int32_t even_camera =
                    (blend_point(from, to, frame * (whole_tick / kFramesPerTick)).x >> 16);
                const uint32_t kept = runner_record.flags;
                runner_record.flags = kept | OA_UNIT_FLAG_SELECTED;
                draw_at(alpha);
                runner_record.flags = kept;
                // The pointer, clicks and the build box map through the
                // camera the frame was drawn from.
                require(
                    static_cast<int32_t>(terrain_cache_cam_x_) == match_camera_x_ &&
                        static_cast<int32_t>(terrain_cache_cam_y_) == match_camera_z_,
                    "a frame tracking the runner was drawn from another camera than the "
                    "pointer's"
                );
                const auto& ground = match_world_cpu_.rgb;
                if (!last_ground.empty()) {
                    const int32_t shift = ground_shift(
                        last_ground, ground, match_world_cpu_.width, match_world_cpu_.height
                    )[0];
                    series.ground_steps.push_back(shift);
                    series.ground_strays.push_back(shift - (even_camera - *last_even_camera));
                }
                last_ground = ground;
                last_even_camera = even_camera;
                if (!options_.snapshot.empty()) {
                    char file[64];
                    std::snprintf(file, sizeof file, "-%s-%04u.ppm", name, written++);
                    write_ppm(fs::path(stem) += file, surface_);
                }
            }
        }
        stop_match_tracking();
        set_camera_position(still_camera_x, still_camera_z, 0);
        return series;
    };
    const Series whole = measure(false, "whole-ticks");
    const Series blended = measure(true, "between-ticks");
    const auto report = [](const char* what, const std::vector<int32_t>& steps) {
        const auto summary = summarise(steps);
        std::printf(
            "interpolation: %s: %u frames, %u still, %d to %d pixels: %s\n",
            what,
            summary.frames,
            summary.still,
            summary.smallest,
            summary.largest,
            step_text(steps).c_str()
        );
    };
    report("runner's steps, whole ticks", whole.unit_steps);
    report("runner's steps, between ticks", blended.unit_steps);
    report("runner off the even path, whole ticks", whole.unit_strays);
    report("runner off the even path, between ticks", blended.unit_strays);
    report("ground's steps under the tracking camera, whole ticks", whole.ground_steps);
    report("ground's steps under the tracking camera, between ticks", blended.ground_steps);
    report("ground off the even path, whole ticks", whole.ground_strays);
    report("ground off the even path, between ticks", blended.ground_strays);
    std::fflush(stdout);
    // Whole ticks alone hold the runner still most frames and stray from
    // the even path; frames between ticks keep it, and the ground, within
    // a pixel or two of it on every frame.
    require(
        summarise(whole.unit_steps).still >= summarise(whole.unit_steps).frames / 2,
        "whole ticks alone moved the runner on most frames"
    );
    require(
        summarise(blended.unit_strays).largest <= 2 * kPlaceTolerance,
        "frames between ticks did not move the runner along the even path"
    );
    require(
        summarise(blended.ground_strays).largest <= 2 * kPlaceTolerance,
        "frames between ticks did not move the ground under the tracking camera evenly"
    );
    restart();

    // Pass 5: frames as the application loop draws them, 120 a second on a
    // clock of the check's own (the camera, the clock step with its ticks,
    // the frame's fraction and the drawing), over the fight from its start,
    // with Smooth motion On and then Off. On, most frames lie between two
    // ticks, with units drawn from their copies and projectiles part of the
    // way from one tick to the next. Off, every frame shows its tick whole:
    // no unit is drawn from its copies and every projectile shows at its
    // tick's place, so the frames of a tick are one picture, the tick drawn
    // whole. The ticks each frame runs, and the world after it, are the same
    // either way.
    struct LoopFrame {
        uint32_t tick{};        ///< the ticks the pass ran by the frame's clock step
        float alpha{};          ///< the fraction of a tick the frame was drawn at
        sha256::Digest frame{}; ///< the battlefield as drawn
        uint64_t world{};       ///< the world after the frame (frame_run_digest)
        size_t units_between{}; ///< units drawn from their copies, between two ticks
        bool shots_between{};   ///< a projectile was drawn off its tick's place
    };

    // The frames' clock and Smooth motion as they were, however the pass ends.
    struct LoopEnd {
        Runtime& runtime;
        bool smooth{};

        LoopEnd(Runtime& owner, bool kept) : runtime(owner), smooth(kept) {}

        LoopEnd(const LoopEnd&) = delete;
        LoopEnd& operator=(const LoopEnd&) = delete;

        ~LoopEnd() {
            runtime.frame_run_clock_ns_.reset();
            runtime.engine_settings_state().current.smooth_motion = smooth;
        }
    } loop_end{*this, smooth_motion()};

    const auto loop_frames = [&](bool smooth) {
        start_fight();
        engine_settings_state().current.smooth_motion = smooth;
        frame_run_clock_ns_ = 0;
        match_timing_.previous_clock = oa::base::game_loop::scaled_clock(0, match_clock_scale());
        tick_presentation_ = {};
        scroll_clock_ = 0;
        zoom_clock_valid_ = false;
        frame_draws_ = {};
        std::vector<LoopFrame> frames;
        const uint32_t base_tick = match_timing_.tick;
        for (uint64_t frame = 0; match_timing_.tick - base_tick < kCheckTicks; ++frame) {
            require(frame < kLoopFrameLimit, "the loop's match clock stopped stepping");
            frame_run_clock_ns_ =
                (2 * frame + 1) * frame_pacing::kNanosecondsPerSecond / (2 * kLoopFramesPerSecond);
            take_frame_time();
            camera_moved_ = false;
            move_match_camera();
            step_match_frame();
            LoopFrame drawn{};
            drawn.tick = match_timing_.tick - base_tick;
            drawn.alpha = presentation_alpha();
            frame_draws_.units_between_ticks = 0;
            rebuild_surface();
            presentation_alpha_ = 1.0F;
            drawn.frame = frame_digest();
            drawn.world = frame_run_digest();
            drawn.units_between = frame_draws_.units_between_ticks;
            const auto& shown = models().presentation.presented_shots;
            const auto shots = match_->projectiles();
            for (size_t index = 0; index < shots.size() && index < shown.size(); ++index)
                drawn.shots_between = drawn.shots_between ||
                                      shown[index].position.x != shots[index].position.x ||
                                      shown[index].position.y != shots[index].position.y ||
                                      shown[index].position.z != shots[index].position.z;
            if (!smooth) {
                // The tick drawn whole is the picture the loop's frame drew.
                draw_at(1.0F);
                require(
                    frame_digest() == drawn.frame,
                    "with Smooth motion Off the loop's frame " + std::to_string(frame) +
                        " is not its tick drawn whole"
                );
            }
            frames.push_back(drawn);
        }
        restart();
        return frames;
    };
    const std::vector<LoopFrame> smooth = loop_frames(true);
    const std::vector<LoopFrame> plain = loop_frames(false);
    require(
        smooth.size() == plain.size() && !smooth.empty(),
        "the loop drew " + std::to_string(smooth.size()) + " frames with Smooth motion On and " +
            std::to_string(plain.size()) + " with it Off"
    );
    size_t smooth_between = 0;
    size_t smooth_units = 0;
    size_t smooth_shots = 0;
    size_t repeated = 0;
    for (size_t index = 0; index < smooth.size(); ++index) {
        const auto& on = smooth[index];
        const auto& off = plain[index];
        const std::string at =
            "frame " + std::to_string(index) + ", tick " + std::to_string(off.tick);
        require(on.tick == off.tick, "Smooth motion Off ran other ticks by " + at);
        require(on.world == off.world, "Smooth motion Off changed the world by " + at);
        smooth_between += on.alpha < 1.0F ? 1 : 0;
        smooth_units += on.units_between;
        smooth_shots += on.shots_between ? 1 : 0;
        require(off.alpha == 1.0F, "Smooth motion Off drew " + at + " between two ticks");
        require(off.units_between == 0, "Smooth motion Off drew units between two ticks at " + at);
        require(
            !off.shots_between, "Smooth motion Off drew a projectile between two ticks at " + at
        );
        if (index > 0 && plain[index - 1].tick == off.tick) {
            require(
                plain[index - 1].frame == off.frame,
                "with Smooth motion Off, " + at + " is not the picture of the frame before"
            );
            ++repeated;
        }
    }
    require(
        smooth_between * 2 >= smooth.size(),
        "with Smooth motion On fewer than half the loop's frames lay between two ticks"
    );
    require(
        smooth_units != 0 && smooth_shots != 0,
        "with Smooth motion On no unit or no projectile was drawn between two ticks"
    );
    require(
        repeated * 2 >= plain.size(),
        "with Smooth motion Off fewer than half the loop's frames repeated their tick's picture"
    );
    std::printf(
        "interpolation: the loop's %zu frames at %u a second: Smooth motion On drew %zu between "
        "ticks, with %zu units and %zu frames of projectiles between them; Off drew each at its "
        "tick, %zu repeating the frame before, and reached the same world at every frame\n",
        plain.size(),
        kLoopFramesPerSecond,
        smooth_between,
        smooth_units,
        smooth_shots,
        repeated
    );
    std::printf(
        "interpolation: a frame between ticks took %.2f ms, a whole tick's %.2f ms\n",
        static_cast<double>(between_ns) / 1.0e6 /
            static_cast<double>(std::max<uint64_t>(drawn_between, 1)),
        static_cast<double>(whole_ns) / 1.0e6 / kCheckTicks
    );
    std::printf(
        "interpolation: %llu frames between ticks drew %zu moving units with the world and every "
        "whole tick's frame as without them\n",
        static_cast<unsigned long long>(drawn_between),
        units_between
    );
    std::fflush(stdout);
}

} // namespace oa::app
