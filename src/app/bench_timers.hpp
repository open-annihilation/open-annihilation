// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The stage timers --bench reads (runtime_bench.cpp): how long each part of
// a match frame takes, and what the turned view's scene and atlas did, summed
// over the frames since the timers were cleared. The timers stay off in
// every other run: each mark is then one test of a flag, and nothing is read
// from the clock or written. The match frame is drawn on one thread, which
// alone marks and reads them; the turned view's scene passes, which run on
// the drawing threads at once, time themselves, and that thread adds their
// times in (add_nanoseconds, count_packed_materials).
#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace oa::app::bench_timers {

/// A part of the match frame the timers measure. The order is the order of
/// the bench's STAGE lines.
enum class Stage : uint8_t {
    frame,             ///< render_match_surface, the whole match frame
    live_total,        ///< render_live_match_surface, the turned view
    scene_build,       ///< the turned view's scene, built on the processor
    survey,            ///< the terrain's survey and plan
    terrain,           ///< the terrain's tiles and triangles
    ground_sprites,    ///< the effect cards laid on the ground
    shadows,           ///< the units' shadows
    foundations,       ///< the buildings' foundations
    models,            ///< the units' and features' models
    projectiles,       ///< the projectiles
    sprites,           ///< the effect cards standing up
    particles,         ///< the particles
    water,             ///< the translucent units, the water and the rest
    gpu_total,         ///< the graphics card's frame, from check to download
    gpu_validate,      ///< the check of the frame's vertices
    gpu_upload,        ///< the copy into the upload buffer
    gpu_wait,          ///< the submission and the wait for the card
    gpu_download,      ///< the copy of the picture back
    rgb_copy,          ///< the picture's pixels into the match's layer
    picking,           ///< the pick's triangles, rebuilt while the card draws
    effects,           ///< the lights and the heat over the turned view
    overlays,          ///< draw_live_overlays
    match_setup,       ///< draw_match_surface before the interface
    match_hud,         ///< compose_match_hud
    match_plan,        ///< the frame's draw list
    match_post,        ///< the fog and the bars
    match_live,        ///< draw_match_surface's call of the turned view
    match_after,       ///< what lies over the battlefield: megamap, text
    selection,         ///< draw_live_selection
    unit_overlays,     ///< draw_live_unit_overlays
    on_screen_units,   ///< rebuild_on_screen_units
    cursor_pick,       ///< the pointer's pick of the unit under it
    effect_visibility, ///< the depth the effects test their lights against
    count
};

/// Stages the timers measure.
inline constexpr std::size_t stage_count = static_cast<std::size_t>(Stage::count);

/// A stage's name in the bench's lines, and whether it is part of the stage
/// listed before it.
struct StageName {
    std::string_view name{};
    bool nested{};
};

/// Each stage's name, by Stage. The names stay the same from one release to
/// the next, so that runs of different builds compare.
inline constexpr std::array<StageName, stage_count> stage_names{
    {{"frame(render_match_surface)", false},
     {"live_total", false},
     {"scene_build", false},
     {"survey+plan", true},
     {"terrain", true},
     {"ground_sprites", true},
     {"unit_shadows", true},
     {"foundations", true},
     {"models", true},
     {"projectiles", true},
     {"sprites", true},
     {"particles", true},
     {"water+rest", true},
     {"gpu_total", false},
     {"validate", true},
     {"upload_copy", true},
     {"submit+wait", true},
     {"download_copy", true},
     {"rgba_to_rgb", false},
     {"picking", false},
     {"effects", false},
     {"overlays(draw_live_overlays)", false},
     {"ds_setup", false},
     {"ds_hud(compose_match_hud)", false},
     {"ds_plan(draw list)", false},
     {"ds_post(fog,bars)", false},
     {"ds_live(render_live_match_surface)", false},
     {"ds_after(megamap,hud text)", false},
     {"ov_selection", false},
     {"ov_unit_overlays", false},
     {"ov_on_screen_units", false},
     {"cursor_pick", false},
     {"effect_visibility", true}}
};

/// What the timers have summed since they were last cleared.
struct Totals {
    /// The time in each stage; for a scene pass that runs on the drawing
    /// threads, the time it took on them.
    std::array<double, stage_count> milliseconds{};
    /// Materials packed into the atlas afresh, by the stage open as each was
    /// packed, and their texels.
    std::array<double, stage_count> materials_packed{};
    std::array<double, stage_count> texels_packed{};
    double terrain_squares_packed{}; ///< terrain tiles packed into the atlas
    double atlas_emptied{};          ///< frames whose atlas was emptied to fit
    double atlas_rows_uploaded{};    ///< atlas rows sent to the card
    double whole_atlas_uploads{};    ///< uploads of every row of the atlas
    double ground_revisions{};       ///< frames whose kept ground changed
};

/// The scene of the last frame the turned view drew.
struct SceneCounts {
    double vertices{};         ///< every vertex of the frame
    double terrain_vertices{}; ///< the ground's vertices
    double models{};
    double sprites{};
    double particles{};
    double nano_particles{};
    double shadows{};
    double crop_cells{}; ///< terrain cells of the map part the frame surveys
    double supersample{};
};

/// The timers' state.
struct Timers {
    bool on{};                   ///< only --bench turns the timers on
    Totals totals{};             ///< sums since the last clear()
    SceneCounts last{};          ///< the last frame's scene
    uint64_t ground_revision{};  ///< the kept ground's revision last frame
    Stage open = Stage::count;   ///< the innermost Span running, which packing charges
    Stage marked = Stage::count; ///< the part of draw_match_surface under way
    uint64_t marked_since_ns{};  ///< when it began
};

/// The process's timers.
inline Timers timers{};

/// Reads the steady clock the timers measure by.
///
/// @return nanoseconds since the clock's epoch
[[nodiscard]] inline uint64_t now_ns() noexcept {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()
    )
                                     .count());
}

/// Adds the time since a start to a stage.
///
/// @param stage the stage
/// @param since_ns the start, from now_ns()
inline void add_time(Stage stage, uint64_t since_ns) noexcept {
    timers.totals.milliseconds[static_cast<std::size_t>(stage)] +=
        static_cast<double>(now_ns() - since_ns) / 1.0e6;
}

/// Times a stage from its construction to its destruction, or between
/// start() and stop(), while the timers are on. A stage started inside
/// another is the one packing charges until it stops.
class Span {
  public:

    /// Starts timing a stage.
    ///
    /// @param stage the stage
    explicit Span(Stage stage) noexcept : stage_(stage) { start(); }

    Span(const Span&) = delete;
    Span& operator=(const Span&) = delete;

    /// Stops timing, if it has not stopped.
    ~Span() { stop(); }

    /// Starts timing again after stop(); does nothing while timing, or
    /// while the timers are off.
    void start() noexcept {
        if (!timers.on || running_)
            return;
        running_ = true;
        outer_ = timers.open;
        timers.open = stage_;
        since_ns_ = now_ns();
    }

    /// Adds the time since the start to the stage and stops timing; does
    /// nothing when not timing.
    void stop() noexcept {
        if (!running_)
            return;
        running_ = false;
        add_time(stage_, since_ns_);
        timers.open = outer_;
    }

  private:

    Stage stage_{};
    Stage outer_ = Stage::count;
    uint64_t since_ns_{};
    bool running_{};
};

/// Ends the part of draw_match_surface under way, adding its time, and
/// begins another. Parts follow one another rather than nest.
///
/// @param stage the part that begins; Stage::count for none
inline void mark(Stage stage) noexcept {
    if (!timers.on)
        return;
    const uint64_t now = now_ns();
    if (timers.marked != Stage::count)
        timers.totals.milliseconds[static_cast<std::size_t>(timers.marked)] +=
            static_cast<double>(now - timers.marked_since_ns) / 1.0e6;
    timers.marked = stage;
    timers.marked_since_ns = now;
}

/// Ends the part of draw_match_surface under way, if any.
inline void end_marks() noexcept {
    mark(Stage::count);
}

/// Ends the part of draw_match_surface mark() began when it goes, however
/// its scope is left.
class MarkedParts {
  public:

    MarkedParts() = default;
    MarkedParts(const MarkedParts&) = delete;
    MarkedParts& operator=(const MarkedParts&) = delete;

    /// Ends the part under way.
    ~MarkedParts() { end_marks(); }
};

/// Counts a material packed into the atlas afresh, against the stage open.
///
/// @param texels the material's width times its height
inline void count_packed_material(uint64_t texels) noexcept {
    if (!timers.on || timers.open == Stage::count)
        return;
    const auto stage = static_cast<std::size_t>(timers.open);
    timers.totals.materials_packed[stage] += 1;
    timers.totals.texels_packed[stage] += static_cast<double>(texels);
}

/// Adds time a stage took on the drawing threads to it.
///
/// @param stage the stage
/// @param nanoseconds the time it took
inline void add_nanoseconds(Stage stage, uint64_t nanoseconds) noexcept {
    if (timers.on)
        timers.totals.milliseconds[static_cast<std::size_t>(stage)] +=
            static_cast<double>(nanoseconds) / 1.0e6;
}

/// Counts materials a stage packed into the atlas afresh on the drawing
/// threads against it.
///
/// @param stage the stage
/// @param materials the materials
/// @param texels their widths times their heights
inline void count_packed_materials(Stage stage, uint64_t materials, uint64_t texels) noexcept {
    if (!timers.on)
        return;
    timers.totals.materials_packed[static_cast<std::size_t>(stage)] +=
        static_cast<double>(materials);
    timers.totals.texels_packed[static_cast<std::size_t>(stage)] += static_cast<double>(texels);
}

/// Counts a terrain tile packed into the atlas.
inline void count_terrain_square() noexcept {
    if (timers.on)
        timers.totals.terrain_squares_packed += 1;
}

/// Counts an atlas emptied so that a frame's materials fit.
inline void count_atlas_emptied() noexcept {
    if (timers.on)
        timers.totals.atlas_emptied += 1;
}

/// Counts the atlas rows a frame sends to the card.
///
/// @param rows the rows sent
/// @param whole every row of the atlas was sent
inline void count_atlas_upload(uint32_t rows, bool whole) noexcept {
    if (!timers.on)
        return;
    timers.totals.atlas_rows_uploaded += rows;
    if (whole)
        timers.totals.whole_atlas_uploads += 1;
}

/// Keeps the scene of the frame just built, and counts a frame whose kept
/// ground has a revision other than the frame before's.
///
/// @param scene the frame's scene counts
/// @param ground_revision the kept ground's revision
inline void note_scene(const SceneCounts& scene, uint64_t ground_revision) noexcept {
    if (!timers.on)
        return;
    timers.last = scene;
    if (ground_revision != timers.ground_revision) {
        timers.totals.ground_revisions += 1;
        timers.ground_revision = ground_revision;
    }
}

/// Keeps the supersampling the card drew the last frame at.
///
/// @param supersample the factor
inline void note_supersample(uint32_t supersample) noexcept {
    if (timers.on)
        timers.last.supersample = supersample;
}

/// Turns the timers on or off and clears what they have summed.
///
/// @param on whether the timers measure from now on
inline void reset(bool on) noexcept {
    const uint64_t ground_revision = timers.ground_revision;
    const SceneCounts last = timers.last;
    timers = {};
    timers.on = on;
    // The scene and the ground's revision carry on from the frames before.
    timers.ground_revision = ground_revision;
    timers.last = last;
}

} // namespace oa::app::bench_timers
