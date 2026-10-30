// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-underwater-features: metal patches on the sea bed of Steel
// Jungle, one in open water, one on the shallows of a shore and one at the
// edge of a boat's sight, drawn by every tier, Off, Basic and Full, and held to the
// processor's picture, which draws them as the game does: in the colours
// the map's art gives them, the colours of the water around them, and
// greyed or blacked out by the fog pixel by pixel as the ground around
// them is. The water's effects off and the fog Classic, Basic and Full
// draw the processor's pixels exactly; with the water's effects at their
// defaults the patches keep the processor's colours, not darkened and
// tinted blue; with the fog Smooth the pixels of a patch wholly in sight,
// wholly out of sight or wholly never mapped are the processor's, and
// those at the fog's soft edge lie between its colour and its fog. The
// pictures of each case go to local/reports.
#include "oa/app/runtime.hpp"

#include "engine_settings_state.hpp"
#include "full_presentation.hpp"
#include "match_models.hpp"
#include "render_host.hpp"
#include "render_run.hpp"
#include "world_draws.hpp"

#include "oa/core/map_plot.h"
#include "oa/formats/png.hpp"
#include "oa/platform/machine.hpp"
#include "oa/present/world_renderer/world_draw_order.hpp"
#include "oa/present/world_renderer/world_fog.hpp"
#include "oa/sim/unit_spawn/spawn_runtime.hpp"
#include "oa/sim/visibility_state.hpp"
#include "oa/ui/console/game_fields.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

namespace {

namespace fs = std::filesystem;
namespace policy = render_policy;
namespace wr = oa::present::world_renderer;
namespace visibility = oa::ui::console::visibility_flag;
using oa::ui::engine_settings::HardwareAcceleration;

/// Exit code of a check that skipped, which ctest reports as skipped.
constexpr int skipped_code = 77;

/// The map: underwater metal patches of the Water World's art, the art of
/// the Core Contingency's water missions, in open water and on the
/// shallows of a gentle shore.
constexpr std::string_view kMapName = "Steel Jungle";
/// The window the match is drawn in: the game's own screen, whose
/// battlefield is drawn a screen pixel to a map pixel at zoom 1.
constexpr int kWindowWidth = 640;
constexpr int kWindowHeight = 480;
/// How far past a patch's drawn frame its picture and its comparison reach.
constexpr int kMargin = 8;
/// Most cells from a patch's footprint to dry ground for the shore case,
/// the fewest for open water, the fewest for the sight's edge, whose boats
/// float east of it within their sight and a cell, and the farthest
/// looked at.
constexpr int32_t kShoreCells = 4;
constexpr int32_t kOpenWaterCells = 12;
constexpr int32_t kEdgeWaterCells = 22;
constexpr int32_t kSearchCells = 24;
/// The least depth of water over a patch, in map heights.
constexpr int32_t kLeastDepth = 8;
/// Map pixels the cases keep from one another, and the sight's edge from
/// each start, where the commanders' sight lies.
constexpr int32_t kCasesApart = 1200;
/// The boat whose sight's edge crosses the fog case's patch, and the ticks
/// its sight takes to settle.
constexpr std::string_view kEyeUnit = "ARMPT";
constexpr int kSightTicks = 8;
/// The most boats placed to bring the sight's edge to the patch.
constexpr int32_t kMostEyeBoats = 6;
/// The fewest pixels of a patch the fog cases need on each side of the
/// fog's edge, and the narrowest patch they take: one sight cell's soft
/// edge and part of a cell either side.
constexpr std::size_t kLeastSidePixels = 40;
constexpr int32_t kLeastEdgeWidth = 64;
/// Pixels in from a patch's own edge its mean colour is taken over, past
/// where Water ripples move its outline by a pixel or two.
constexpr int kMeanInset = 3;
/// Most a channel's mean over a patch, in Full with the water's effects
/// at their defaults, may stray from the processor's: the ripples move the
/// art by a pixel or two, which moves no mean that far; the shimmer's tint
/// moved blue by about 60 levels.
constexpr double kMostMeanShift = 6.0;
/// Most a pixel at the Smooth fog's edge may lie past the range from its
/// colour to its fog, per channel.
constexpr int kMostEdgeSpill = 3;
/// Most a channel's mean in the turned view may stray from the top-down
/// Full picture. The turn and the water's ripples move a mean by a few
/// levels; the underwater tint moved blue by about 60.
constexpr double kMostTurnedMean = 18.0;
/// Most a channel's mean over a wholly fogged tile of the turned view may
/// stray from the top-down Full picture. The soft edge is not compared.
constexpr double kMostTurnedFog = 12.0;
/// A wholly unexplored part of a patch in the turned view reads as black.
constexpr int kMostTurnedBlack = 8;
/// The turned view, mild enough that a patch centred in the battlefield
/// stays in view. Pitch is held to the unlocked camera's range.
constexpr float kTurnedYaw = 32;
constexpr float kTurnedPitch = 24;

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("underwater features check: " + what);
}

/// Writes part of a frame as a PNG file.
///
/// @param path the file
/// @param frame the frame
/// @param left the part's left column
/// @param top its top row
/// @param width its columns
/// @param height its rows
void write_part(
    const fs::path& path, const renderer::Surface& frame, int left, int top, int width, int height
) {
    left = std::clamp(left, 0, static_cast<int>(frame.width));
    top = std::clamp(top, 0, static_cast<int>(frame.height));
    width = std::clamp(width, 0, static_cast<int>(frame.width) - left);
    height = std::clamp(height, 0, static_cast<int>(frame.height) - top);
    std::vector<uint8_t> rgb(static_cast<std::size_t>(width) * height * 3U);
    for (int row = 0; row < height; ++row)
        std::copy_n(
            frame.rgb.data() + (static_cast<std::size_t>(top + row) * frame.width + left) * 3U,
            static_cast<std::size_t>(width) * 3U,
            rgb.data() + static_cast<std::size_t>(row) * width * 3U
        );
    std::vector<uint8_t> file;
    const oa::formats::png::Header header{
        static_cast<uint32_t>(width),
        static_cast<uint32_t>(height),
        8,
        oa::formats::png::ColorType::rgb,
        {}
    };
    if (!oa::formats::png::write(oa::formats::png::Image{header, {}, rgb}, &file))
        fail("cannot encode " + path.string());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size())
    );
    if (!output)
        fail("cannot write " + path.string());
}

/// A metal patch on the sea bed the check draws.
struct Patch {
    std::string name; ///< the case: open-water, shore, sight-edge
    int32_t cell_x{}; ///< its footprint's origin
    int32_t cell_z{};
    int32_t map_x{}; ///< the point it is drawn at, in map pixels
    int32_t map_z{};
    oa::formats::objects3d::FixedVector3 position{};
    int32_t shore{};       ///< cells from its footprint to dry ground
    int32_t width{};       ///< its frame's width, in map pixels
    int32_t from_starts{}; ///< map pixels to the nearest start, across and down
};

/// A frame's pixels over a patch: the frame and where the patch's own
/// frame lies on it.
struct Drawn {
    renderer::Surface frame;
    int left{}; ///< the patch's frame on the window
    int top{};
    int width{};
    int height{};
    /// One byte a pixel of the patch's frame: set where it draws a pixel.
    std::vector<uint8_t> opaque;
    wr::FogGrid grid; ///< the fog the frame drew, when it drew one
    int field_x{};    ///< the battlefield's corner on the window
    int field_y{};
};

/// The colour of a pixel of a frame.
///
/// @param frame the frame
/// @param x its column
/// @param y its row
/// @return its red, green and blue
std::array<int, 3> pixel_at(const renderer::Surface& frame, int x, int y) {
    const auto at = (static_cast<std::size_t>(y) * frame.width + static_cast<std::size_t>(x)) * 3U;
    return {frame.rgb[at], frame.rgb[at + 1], frame.rgb[at + 2]};
}

/// How two frames differ over a patch and the margin round it.
struct Difference {
    std::size_t pixels{}; ///< pixels compared
    std::size_t differ{}; ///< pixels that differ
    int most{};           ///< the largest channel difference
};

/// Compares two frames over a patch's frame and the margin round it.
///
/// @param drawn the frame drawn
/// @param expected the frame expected, of the same size
/// @param area the patch's place
/// @return the difference
Difference
compare(const renderer::Surface& drawn, const renderer::Surface& expected, const Drawn& area) {
    Difference difference;
    const int right = std::min(area.left + area.width + kMargin, static_cast<int>(drawn.width));
    const int bottom = std::min(area.top + area.height + kMargin, static_cast<int>(drawn.height));
    for (int y = std::max(area.top - kMargin, area.field_y); y < bottom; ++y)
        for (int x = std::max(area.left - kMargin, area.field_x); x < right; ++x) {
            ++difference.pixels;
            const auto seen = pixel_at(drawn, x, y);
            const auto wanted = pixel_at(expected, x, y);
            int most = 0;
            for (std::size_t channel = 0; channel < 3; ++channel)
                most = std::max(most, std::abs(seen[channel] - wanted[channel]));
            difference.differ += most != 0 ? 1 : 0;
            difference.most = std::max(difference.most, most);
        }
    return difference;
}

/// The mean of each channel over a patch's own pixels kMeanInset or more in
/// from its edge.
///
/// @param frame the frame
/// @param area the patch's place and pixels
/// @return the means
std::array<double, 3> patch_mean(const renderer::Surface& frame, const Drawn& area) {
    std::array<double, 3> sum{};
    std::size_t count = 0;
    // A pixel of the patch with its own pixels all round it.
    const auto inside = [&](int column, int row) {
        for (int y = row - kMeanInset; y <= row + kMeanInset; ++y)
            for (int x = column - kMeanInset; x <= column + kMeanInset; ++x)
                if (x < 0 || y < 0 || x >= area.width || y >= area.height ||
                    area.opaque[static_cast<std::size_t>(y) * area.width + x] == 0)
                    return false;
        return true;
    };
    for (int row = 0; row < area.height; ++row)
        for (int column = 0; column < area.width; ++column) {
            if (!inside(column, row))
                continue;
            const auto rgb = pixel_at(frame, area.left + column, area.top + row);
            for (std::size_t channel = 0; channel < 3; ++channel)
                sum[channel] += rgb[channel];
            ++count;
        }
    if (count != 0)
        for (auto& channel : sum)
            channel /= static_cast<double>(count);
    return sum;
}

} // namespace

int Runtime::check_underwater_features() {
    if (sdl_.renderer == nullptr || sdl_.window == nullptr || !render_run_ ||
        render_run_->host == nullptr)
        fail("needs the SDL renderer and the renderer host that made it");
    auto& inputs = render_run_->host->tier_inputs();
    if (inputs.memory < policy::smallest_accelerated_memory) {
        std::cout << "underwater features check: skipped: the machine reports less than the "
                     "2 GiB threshold of memory\n";
        return skipped_code;
    }
    if (inputs.capability != policy::Capability::capable && !options_.force_capable) {
        std::cout << "underwater features check: skipped: the renderer "
                  << render_run_->host->facts().renderer
                  << " is not capable of the accelerated tiers\n";
        return skipped_code;
    }
    if (!options_.hardware_acceleration ||
        *options_.hardware_acceleration == HardwareAcceleration::off)
        fail("needs --hardware-acceleration, which switches the accelerated tiers on");
    if (inputs.function_test != policy::FunctionTest::passed)
        fail("the start-up function test did not pass");
    if (!engine_settings_)
        fail("needs the engine settings");
    auto& settings = engine_settings_->current;
    const fs::path reports = "local/reports";
    fs::create_directories(reports);

    // The tier as --hardware-acceleration would set it; each frame drawn
    // checks the tier drew it.
    HardwareAcceleration level_set = HardwareAcceleration::off;
    const auto set_level = [&](HardwareAcceleration level) {
        options_.hardware_acceleration = level;
        update_render_tier();
        level_set = level;
    };
    // The lights of weapons, nanolathes and wrecks and the heat's shimmer
    // are off: the cases hold the water's effects and the fog alone.
    settings.weapon_lighting = 0;
    settings.nanolathe_lighting = 0;
    settings.wreck_glow = 0;
    settings.heat_distortion = 0;
    const auto water_effects = [&](bool on) {
        using namespace oa::ui::engine_settings;
        settings.water_ripples = on ? default_water_ripples : 0;
        settings.coastal_waves = on ? default_coastal_waves : 0;
        settings.underwater_shimmer = on ? default_underwater_shimmer : 0;
        settings.naval_reflections = on ? default_naval_reflections : 0;
    };

    // The skirmish on the map, loaded in Full so that its loading screen
    // makes the Full tier's pages.
    if (!SDL_SetWindowSize(sdl_.window, kWindowWidth, kWindowHeight) ||
        !SDL_SyncWindow(sdl_.window))
        fail(std::string("SDL_SetWindowSize: ") + SDL_GetError());
    apply_output_mode();
    set_level(HardwareAcceleration::full);
    exercise_click(menu::resource_name(menu::Button::single_player));
    exercise_click(entry::resource_name(entry::Button::skirmish));
    skirmish_settings_.map_name = std::string(kMapName);
    if (select_map(kMapName) == 0)
        fail("cannot select " + std::string(kMapName));
    state_.player_count = 2;
    exercise_click(skirmish::resource_name(skirmish::Button::start));
    if (screen_ != Screen::match || !match_)
        fail("the skirmish on " + std::string(kMapName) + " did not start");
    apply_output_mode();
    if (!full_presentation())
        fail("the match did not load in the Full tier");
    match_zoom_ = match_zoom_target_ = 1.0F;
    auto& world = match_->state();
    const int32_t sea = world.game.sea_level;
    const int32_t map_width = world.game.map_width;
    const int32_t map_height = world.game.map_height;
    const auto plot_height = [&](int32_t x, int32_t z) {
        const auto* plot = oa::world_plot(&world, x, z);
        return plot != nullptr ? static_cast<int32_t>(plot->height) : -1;
    };

    // Where the players start, which the cases keep away from.
    std::vector<std::array<int32_t, 2>> starts;
    for (const auto& slot : match_->world().slots)
        if (slot.unit != nullptr && slot.record.type_index != 0)
            starts.push_back(
                {static_cast<int32_t>(slot.unit->position[0] >> 16),
                 static_cast<int32_t>(slot.unit->position[2] >> 16)}
            );

    // The metal patches lying on the sea bed, each with how far its
    // footprint lies from dry ground, and the cases among them.
    std::vector<Patch> patches;
    for (const auto& feature : match_gaf_features_) {
        if (feature.feature_index >= world.feature_def_count)
            continue;
        const auto& def = world.feature_defs[feature.feature_index];
        const int32_t height = feature.position.y >> 16;
        if (wr::feature_stands(def.height) || def.metal == 0 || sea - height < kLeastDepth)
            continue;
        Patch patch;
        patch.name.assign(def.name, ::strnlen(def.name, sizeof def.name));
        patch.cell_x = feature.cell_x;
        patch.cell_z = feature.cell_z;
        patch.position = feature.position;
        if (feature.anim < match_gaf_anims_.size() &&
            !match_gaf_anims_[feature.anim].frames.empty())
            patch.width = match_gaf_anims_[feature.anim].frames.front().width;
        patch.map_x = feature.position.x >> 16;
        patch.map_z = (feature.position.z >> 16) - height / 2;
        // The view centred on the patch stays on the map.
        const int32_t reach_x = visible_map_width() / 2 + kMargin;
        const int32_t reach_z = visible_map_height() / 2 + kMargin;
        if (patch.map_x < reach_x || patch.map_z < reach_z ||
            patch.map_x > map_width * OA_MAP_CELL_PIXELS - reach_x ||
            patch.map_z > map_height * OA_MAP_CELL_PIXELS - reach_z)
            continue;
        patch.shore = kSearchCells + 1;
        for (int32_t z = feature.cell_z - kSearchCells;
             z < feature.cell_z + def.footprint_z + kSearchCells;
             ++z)
            for (int32_t x = feature.cell_x - kSearchCells;
                 x < feature.cell_x + def.footprint_x + kSearchCells;
                 ++x) {
                if (plot_height(x, z) < sea)
                    continue;
                const int32_t across =
                    std::max({feature.cell_x - x, x - (feature.cell_x + def.footprint_x - 1), 0});
                const int32_t down =
                    std::max({feature.cell_z - z, z - (feature.cell_z + def.footprint_z - 1), 0});
                patch.shore = std::min(patch.shore, std::max(across, down));
            }
        patch.from_starts = std::numeric_limits<int32_t>::max();
        for (const auto& start : starts)
            patch.from_starts = std::min(
                patch.from_starts,
                std::abs(start[0] - patch.map_x) + std::abs(start[1] - patch.map_z)
            );
        patches.push_back(patch);
    }
    const auto apart = [&](const Patch& patch, const std::vector<Patch>& chosen) {
        return std::all_of(chosen.begin(), chosen.end(), [&](const Patch& other) {
            return std::abs(other.map_x - patch.map_x) + std::abs(other.map_z - patch.map_z) >=
                   kCasesApart;
        });
    };
    std::vector<Patch> cases;
    // The patch that fits a case, nearest the shore or farthest from it.
    const auto choose = [&](const char* name, bool near_shore, auto&& fits) {
        const Patch* best = nullptr;
        for (const auto& patch : patches)
            if (fits(patch) && apart(patch, cases) &&
                (best == nullptr ||
                 (near_shore ? patch.shore < best->shore : patch.shore > best->shore)))
                best = &patch;
        if (best == nullptr)
            fail(std::string("found no metal patch on the sea bed for the ") + name + " case");
        cases.push_back(*best);
        cases.back().name = name;
    };
    choose("shore", true, [](const Patch& patch) { return patch.shore <= kShoreCells; });
    choose("open-water", false, [](const Patch& patch) { return patch.shore > kOpenWaterCells; });
    choose("sight-edge", false, [](const Patch& patch) {
        return patch.shore >= kEdgeWaterCells && patch.width >= kLeastEdgeWidth &&
               patch.from_starts >= kCasesApart;
    });
    for (const auto& patch : cases)
        std::cout << "underwater features check: " << patch.name << ": a patch at cell "
                  << patch.cell_x << "," << patch.cell_z << ", drawn at " << patch.map_x << ","
                  << patch.map_z << ", " << (sea - (patch.position.y >> 16)) << " under the sea, "
                  << patch.shore << " cells from dry ground\n";

    // A boat east of the sight-edge patch, so that the west edge of its
    // sight crosses the patch: a first boat twice its sight away, too far
    // to see the patch, shows how far west of a boat its sight reaches in
    // the patch's row of sight cells, and a second boat that far east of
    // the patch's middle.
    const Patch& edge = cases[2];
    {
        const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, kEyeUnit);
        if (type == 0)
            fail("the game has no " + std::string(kEyeUnit));
        const int32_t sight =
            type < world.unit_def_count ? world.unit_defs[type].sight_distance : 0;
        if (sight <= 0)
            fail(std::string(kEyeUnit) + " sees nothing");
        const int32_t z = edge.position.z >> 16;
        const auto place_boat = [&](int32_t x) {
            oa::sim::unit_spawn::Request request;
            request.player = match_local_player_;
            request.type = type;
            request.finished = true;
            request.state = kGroundOccupancyState;
            request.position = {
                static_cast<uint32_t>(x) << 16U,
                static_cast<uint32_t>(sea) << 16U,
                static_cast<uint32_t>(z) << 16U
            };
            auto* slot = match_->create(request);
            if (slot == nullptr || slot->unit == nullptr)
                fail("could not place " + std::string(kEyeUnit));
            slot->unit->object_present = true;
            for (int tick = 0; tick < kSightTicks; ++tick)
                step_match_simulation();
        };
        // The map pixel where the sight of the boats in the patch's row of
        // sight cells ends to the west, walking west from a boat's cell.
        const int32_t row = edge.map_z / wr::fog_cell_pixels;
        const auto west_edge = [&](int32_t boat_x) {
            const auto coverage = match_->player_coverage(match_local_player_);
            const auto& cells = match_->sight();
            const auto in_sight = [&](int32_t at) {
                if (at < 0 || at >= cells.width || row < 0 || row >= cells.height)
                    return false;
                const auto index =
                    static_cast<std::size_t>(row) * static_cast<std::size_t>(cells.width) +
                    static_cast<std::size_t>(at);
                return index < coverage.size() &&
                       oa::sim::visibility_state::count_in_sight(
                           coverage[index], oa::sim::visibility_state::SightReading::player
                       );
            };
            int32_t column = boat_x / wr::fog_cell_pixels;
            if (!in_sight(column))
                fail(std::string(kEyeUnit) + " does not see the cell it floats in");
            while (in_sight(column - 1))
                --column;
            return column * wr::fog_cell_pixels;
        };
        // The first boat a cell east of where its sight could reach the
        // patch.
        const int32_t first = edge.map_x + sight + 2 * wr::fog_cell_pixels;
        place_boat(first);
        const int32_t reach = first - west_edge(first);
        std::cout << "underwater features check: a " << kEyeUnit << " sees " << reach
                  << " map pixels west in the patch's row\n";
        // Boats placed a sight cell further west each time, from two cells
        // east of where the edge would fall at the patch's middle, until the
        // sight's edge lies within half a cell of it: each one's sight joins
        // the others', so the edge moves west a cell at a time.
        int32_t place_at = edge.map_x + reach + 2 * wr::fog_cell_pixels;
        int32_t off = 0;
        for (int32_t boat = 0; boat < kMostEyeBoats; ++boat) {
            place_boat(place_at);
            off = west_edge(place_at) - edge.map_x;
            if (off <= wr::fog_cell_pixels / 2)
                break;
            place_at -= wr::fog_cell_pixels;
        }
        std::cout << "underwater features check: the sight's edge lies " << off
                  << " map pixels east of the patch's middle\n";
    }

    // The fog each case draws: none, out of sight greyed with every cell
    // mapped, or out of sight greyed and never-mapped ground black.
    enum class FogCase : uint8_t { none, unseen, unmapped };
    const auto set_fog = [&](FogCase fog) {
        auto& flags = world.game.visibility_flags;
        const auto kept =
            static_cast<uint8_t>(flags & ~(visibility::mapping | visibility::line_of_sight));
        switch (fog) {
        case FogCase::none:
            flags = kept;
            reset_sight_presentation();
            break;
        case FogCase::unseen:
            flags = static_cast<uint8_t>(kept | visibility::line_of_sight);
            reset_sight_presentation();
            break;
        case FogCase::unmapped:
            // As the console's Mapping turns mapping on: what the player sees
            // now is all it has mapped.
            flags = static_cast<uint8_t>(kept | visibility::line_of_sight | visibility::mapping);
            reset_match_sight(true);
            break;
        }
    };

    const auto tier_name = [](HardwareAcceleration level) {
        switch (level) {
        case HardwareAcceleration::off:
            return "off";
        case HardwareAcceleration::basic:
            return "basic";
        case HardwareAcceleration::full:
            break;
        }
        return "full";
    };
    // Draws the frame of a patch in the tier the check stands at.
    const auto draw = [&](const Patch& patch) {
        match_zoom_ = match_zoom_target_ = 1.0F;
        match_pointer_x_ = static_cast<float>(match_layout_.left + 2);
        match_pointer_y_ = static_cast<float>(match_layout_.top + 2);
        set_camera_position(
            patch.map_x - visible_map_width() / 2, patch.map_z - visible_map_height() / 2, 0
        );
        Drawn drawn;
        capture_frame_ = &drawn.frame;
        render();
        capture_frame_ = nullptr;
        const bool accelerated = level_set != HardwareAcceleration::off;
        const bool full = level_set == HardwareAcceleration::full;
        if (accelerated_presentation() != accelerated || full_presentation() != full ||
            (full && !full_frame_drawn()))
            fail(std::string("the ") + tier_name(level_set) + " tier did not draw the frame");
        if (drawn.frame.width != static_cast<uint32_t>(match_layout_.width) ||
            drawn.frame.height != static_cast<uint32_t>(match_layout_.height))
            fail("the frame read back is not the match's layout");
        drawn.field_x = match_layout_.left;
        drawn.field_y = match_layout_.top;
        const auto& draws = match_models().draws;
        const SpriteDraw* found = nullptr;
        for (const auto& sprite : draws.sprites)
            if (!sprite.shadow && sprite.on_map && sprite.frame != nullptr &&
                sprite.world_position && sprite.world_position->x == patch.position.x &&
                sprite.world_position->z == patch.position.z)
                found = &sprite;
        if (found == nullptr)
            fail("the frame drew no " + patch.name + " patch");
        const SceneRect rect = sprite_scene_rect(*found, 1.0F);
        drawn.left = drawn.field_x + static_cast<int>(rect.left);
        drawn.top = drawn.field_y + static_cast<int>(rect.top);
        drawn.width = static_cast<int>(rect.right - rect.left);
        drawn.height = static_cast<int>(rect.bottom - rect.top);
        const auto& image = *found->frame;
        if (drawn.width != image.width || drawn.height != image.height)
            fail("the " + patch.name + " patch is not drawn at its frame's size at zoom 1");
        drawn.opaque.assign(static_cast<std::size_t>(drawn.width) * drawn.height, 0);
        for (std::size_t pixel = 0; pixel < drawn.opaque.size(); ++pixel)
            drawn.opaque[pixel] = pixel < image.coverage.size() && image.coverage[pixel] != 0 &&
                                          pixel < image.pixels.size() &&
                                          image.pixels[pixel] != image.transparency_index
                                      ? 1
                                      : 0;
        if (drawn.left - kMargin < drawn.field_x || drawn.top - kMargin < drawn.field_y ||
            drawn.left + drawn.width + kMargin >
                drawn.field_x + match_layout_.battlefield_width() ||
            drawn.top + drawn.height + kMargin > drawn.field_y + match_layout_.battlefield_height())
            fail("the " + patch.name + " patch is not drawn whole in the battlefield");
        drawn.grid = fog_grid_;
        return drawn;
    };
    const auto picture = [&](const Drawn& drawn, const std::string& name) {
        write_part(
            reports / ("native-underwater-features-" + name + ".png"),
            drawn.frame,
            drawn.left - 2 * kMargin,
            drawn.top - 2 * kMargin,
            drawn.width + 4 * kMargin,
            drawn.height + 4 * kMargin
        );
    };
    constexpr std::array<HardwareAcceleration, 3> tiers{
        HardwareAcceleration::off, HardwareAcceleration::basic, HardwareAcceleration::full
    };
    // Each case's frames in the three tiers, the processor's first.
    const auto draw_tiers = [&](const Patch& patch, const std::string& name) {
        std::array<Drawn, 3> drawn;
        for (std::size_t tier = 0; tier < tiers.size(); ++tier) {
            set_level(tiers[tier]);
            drawn[tier] = draw(patch);
            picture(drawn[tier], name + "-" + tier_name(tiers[tier]));
        }
        return drawn;
    };
    // Every case runs, and the check fails at its end with each case that
    // failed.
    std::vector<std::string> failures;
    const auto require = [&](bool held, const std::string& what) {
        if (held)
            return;
        std::cout << "underwater features check: FAILED: " << what << '\n';
        failures.push_back(what);
    };
    const auto require_exact = [&](const std::array<Drawn, 3>& drawn, const std::string& name) {
        for (std::size_t tier = 1; tier < tiers.size(); ++tier) {
            const auto difference = compare(drawn[tier].frame, drawn[0].frame, drawn[0]);
            std::cout << "underwater features check: " << name << ": " << tier_name(tiers[tier])
                      << " against the processor over " << difference.pixels
                      << " pixels: " << difference.differ << " differ, most " << difference.most
                      << '\n';
            require(
                difference.pixels != 0 && difference.most == 0,
                name + ": the " + tier_name(tiers[tier]) +
                    " tier does not draw the processor's pixels over the patch"
            );
        }
    };

    // The game's own look: the water's effects off and the fog Classic.
    // Basic and Full draw the processor's pixels over each patch, in sight,
    // at the edge of sight and at the edge of never-mapped ground.
    water_effects(false);
    settings.fog_of_war = oa::ui::engine_settings::FogOfWar::classic;
    for (const auto& patch : {cases[0], cases[1]}) {
        set_fog(FogCase::none);
        require_exact(
            draw_tiers(patch, patch.name + "-classic"), patch.name + " in the game's look"
        );
    }
    set_fog(FogCase::unseen);
    require_exact(draw_tiers(edge, "sight-edge-unseen-classic"), "the edge of sight, Classic fog");
    set_fog(FogCase::unmapped);
    require_exact(
        draw_tiers(edge, "sight-edge-unmapped-classic"),
        "the edge of never-mapped ground, Classic fog"
    );

    // The water's effects at their defaults: Full keeps the colours the
    // map's art gives each patch, which the processor draws, as the water's
    // art around it keeps its own; Basic draws the processor's pixels.
    water_effects(true);
    settings.fog_of_war = oa::ui::engine_settings::FogOfWar::smooth;
    set_fog(FogCase::none);
    for (const auto& patch : {cases[0], cases[1]}) {
        const auto drawn = draw_tiers(patch, patch.name + "-water-effects");
        const auto basic = compare(drawn[1].frame, drawn[0].frame, drawn[0]);
        require(
            basic.most == 0,
            patch.name + " with the water's effects: Basic does not draw the processor's pixels"
        );
        const auto expected = patch_mean(drawn[0].frame, drawn[0]);
        const auto seen = patch_mean(drawn[2].frame, drawn[2]);
        double most = 0.0;
        for (std::size_t channel = 0; channel < 3; ++channel)
            most = std::max(most, std::abs(seen[channel] - expected[channel]));
        std::cout << "underwater features check: " << patch.name
                  << " with the water's effects: the patch's mean colour " << seen[0] << ","
                  << seen[1] << "," << seen[2] << " in Full, " << expected[0] << "," << expected[1]
                  << "," << expected[2] << " drawn by the processor; most " << most << '\n';
        require(
            most <= kMostMeanShift,
            patch.name +
                " with the water's effects: Full does not draw the patch in the colours the "
                "processor draws it in"
        );
    }

    // The fog Smooth, the water's effects off: each pixel of the patch
    // takes the fog the ground under it takes. Where its fog tile is wholly
    // in sight, wholly out of sight or wholly never mapped, Full draws the
    // processor's pixel; where the soft edge ramps, a pixel between its
    // colour and its fog.
    water_effects(false);
    set_fog(FogCase::none);
    set_level(HardwareAcceleration::off);
    const Drawn colour = draw(edge);
    for (const FogCase fog : {FogCase::unseen, FogCase::unmapped}) {
        const std::string name =
            fog == FogCase::unseen ? "sight-edge-unseen-smooth" : "sight-edge-unmapped-smooth";
        set_fog(fog);
        const auto drawn = draw_tiers(edge, name);
        const Drawn& processor = drawn[0];
        const Drawn& card = drawn[2];
        if (card.grid.tiles.empty())
            fail(name + ": the frame drew no fog");
        std::size_t clear = 0;
        std::size_t covered = 0;
        std::size_t ramped = 0;
        int most_whole = 0;
        int most_spill = 0;
        for (int row = 0; row < card.height; ++row)
            for (int column = 0; column < card.width; ++column) {
                if (card.opaque[static_cast<std::size_t>(row) * card.width + column] == 0)
                    continue;
                const int x = card.left + column;
                const int y = card.top + row;
                const int tile_column = static_cast<int>(std::floor(
                    static_cast<double>(x - card.field_x - card.grid.offset_x) / wr::fog_cell_pixels
                ));
                const int tile_row = static_cast<int>(std::floor(
                    static_cast<double>(y - card.field_y - card.grid.offset_z) / wr::fog_cell_pixels
                ));
                if (tile_column < 0 || tile_row < 0 || tile_column >= card.grid.width ||
                    tile_row >= card.grid.height)
                    fail(name + ": the patch lies past the fog's grid");
                const auto& tile = card.grid.at(tile_column, tile_row);
                const auto seen = pixel_at(card.frame, x, y);
                const auto wanted = pixel_at(processor.frame, x, y);
                const bool whole_tile = (tile.unseen == 0 && tile.unmapped == 0) ||
                                        tile.unmapped == wr::fog_mask_full ||
                                        (tile.unseen == wr::fog_mask_full && tile.unmapped == 0);
                if (whole_tile) {
                    if (tile.unseen == 0 && tile.unmapped == 0)
                        ++clear;
                    else
                        ++covered;
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        most_whole =
                            std::max(most_whole, std::abs(seen[channel] - wanted[channel]));
                    continue;
                }
                // The edge: between the pixel's colour, its grey and the
                // black of ground never mapped.
                ++ramped;
                const auto original = pixel_at(colour.frame, x, y);
                const auto level =
                    static_cast<std::size_t>((original[0] + original[1] + original[2]) / 3);
                const auto& grey = fog_shading_.gray_levels[level];
                const auto& black = fog_shading_.unmapped_rgb;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const int low =
                        std::min({original[channel], int{grey[channel]}, int{black[channel]}});
                    const int high =
                        std::max({original[channel], int{grey[channel]}, int{black[channel]}});
                    most_spill = std::max({most_spill, low - seen[channel], seen[channel] - high});
                }
            }
        std::cout << "underwater features check: " << name << ": " << clear
                  << " pixels of the patch in sight, " << covered << " under the fog, " << ramped
                  << " at its edge; most " << most_whole << " from the processor's off the edge, "
                  << most_spill << " past the edge's range\n";
        if (clear < kLeastSidePixels || covered < kLeastSidePixels)
            fail(name + ": the fog's edge does not cross the patch");
        require(
            most_whole == 0,
            name + ": the Full tier does not draw the patch's pixels in sight and under the fog as "
                   "the processor does"
        );
        require(
            most_spill <= kMostEdgeSpill,
            name + ": the Full tier draws pixels at the fog's edge past their colour and fog"
        );
        const auto basic = compare(drawn[1].frame, processor.frame, processor);
        require(basic.most == 0, name + ": Basic does not draw the processor's pixels");
    }

    // The turned view, held to the top-down Full picture: no tint on a
    // patch in open water or on a shore, and the same fog as the ground
    // on a wholly seen, wholly grey or wholly unexplored part of the
    // sight-edge patch. A machine with no graphics card skips this and
    // leaves the checks above standing.
    settings.allow_full_3d = true;
    const uint16_t saved_graphics = world.game.graphics_flags;
    bool turned_skipped = false;
    const auto card_turned = [&] {
        return live_renderer_active() && !live_renderer_unavailable() && !live_renderer_failed();
    };
    const auto find_patch = [&](const Patch& patch) -> const SpriteDraw* {
        for (const auto& sprite : match_models().draws.sprites)
            if (!sprite.shadow && sprite.on_map && sprite.frame != nullptr &&
                sprite.world_position && sprite.world_position->x == patch.position.x &&
                sprite.world_position->z == patch.position.z)
                return &sprite;
        return nullptr;
    };
    // The mean of one class of a patch's own pixels in the turned picture.
    // `keep` chooses the pixels; ones the turn carries off the battlefield
    // are left out.
    const auto turned_mean = [&](const Drawn& turned, const Patch& patch, auto&& keep) {
        struct Mean {
            std::array<double, 3> value{};
            std::size_t count{};
        };
        Mean mean;
        const SpriteDraw* sprite = find_patch(patch);
        if (sprite == nullptr || sprite->frame == nullptr || !sprite->world_position)
            return mean;
        const auto& image = *sprite->frame;
        const float anchor_x = static_cast<float>(sprite->world_position->x) / 65536.0F;
        const float anchor_z = static_cast<float>(sprite->world_position->z) / 65536.0F;
        const int field_right = turned.field_x + match_layout_.battlefield_width();
        const int field_bottom = turned.field_y + match_layout_.battlefield_height();
        const auto inside = [&](int column, int row) {
            for (int y = row - kMeanInset; y <= row + kMeanInset; ++y)
                for (int x = column - kMeanInset; x <= column + kMeanInset; ++x)
                    if (x < 0 || y < 0 || x >= turned.width || y >= turned.height ||
                        turned.opaque[static_cast<std::size_t>(y) * turned.width + x] == 0)
                        return false;
            return true;
        };
        for (int row = 0; row < turned.height; ++row)
            for (int column = 0; column < turned.width; ++column) {
                if (!inside(column, row) || !keep(column, row))
                    continue;
                const float world_x = anchor_x - static_cast<float>(image.origin_x) + column;
                const float world_z = anchor_z - static_cast<float>(image.origin_y) + row;
                const int32_t cell_x =
                    static_cast<int32_t>(std::floor(world_x)) / OA_MAP_CELL_PIXELS;
                const int32_t cell_z =
                    static_cast<int32_t>(std::floor(world_z)) / OA_MAP_CELL_PIXELS;
                const int32_t height = plot_height(cell_x, cell_z);
                if (height < 0)
                    continue;
                const auto screen =
                    live_project_world(world_x, static_cast<float>(height), world_z);
                if (screen.x < turned.field_x + kMargin || screen.y < turned.field_y + kMargin ||
                    screen.x >= field_right - kMargin || screen.y >= field_bottom - kMargin ||
                    screen.x < 0 || screen.y < 0 ||
                    screen.x >= static_cast<int32_t>(turned.frame.width) ||
                    screen.y >= static_cast<int32_t>(turned.frame.height))
                    continue;
                const auto rgb = pixel_at(turned.frame, screen.x, screen.y);
                for (std::size_t channel = 0; channel < 3; ++channel)
                    mean.value[channel] += rgb[channel];
                ++mean.count;
            }
        if (mean.count != 0)
            for (auto& channel : mean.value)
                channel /= static_cast<double>(mean.count);
        return mean;
    };
    // 0 wholly in sight, 1 wholly grey, 2 wholly unexplored, -1 the edge.
    const auto whole_class = [&](const Drawn& top, int column, int row) {
        const int x = top.left + column;
        const int y = top.top + row;
        const int tile_column = static_cast<int>(std::floor(
            static_cast<double>(x - top.field_x - top.grid.offset_x) / wr::fog_cell_pixels
        ));
        const int tile_row = static_cast<int>(std::floor(
            static_cast<double>(y - top.field_y - top.grid.offset_z) / wr::fog_cell_pixels
        ));
        if (tile_column < 0 || tile_row < 0 || tile_column >= top.grid.width ||
            tile_row >= top.grid.height)
            return -1;
        const auto& tile = top.grid.at(tile_column, tile_row);
        const bool whole = (tile.unseen == 0 && tile.unmapped == 0) ||
                           tile.unmapped == wr::fog_mask_full ||
                           (tile.unseen == wr::fog_mask_full && tile.unmapped == 0);
        if (!whole)
            return -1;
        if (tile.unseen == 0 && tile.unmapped == 0)
            return 0;
        if (tile.unmapped == wr::fog_mask_full)
            return 2;
        return 1;
    };
    const auto channel_gap = [](const std::array<double, 3>& seen,
                                const std::array<double, 3>& wanted) {
        double most = 0;
        for (std::size_t channel = 0; channel < 3; ++channel)
            most = std::max(most, std::abs(seen[channel] - wanted[channel]));
        return most;
    };

    set_level(HardwareAcceleration::full);
    water_effects(true);
    settings.fog_of_war = oa::ui::engine_settings::FogOfWar::smooth;
    set_fog(FogCase::none);
    world.game.graphics_flags =
        static_cast<uint16_t>(saved_graphics & ~init::preference_flags::dithered_fog);
    for (const auto& patch : {cases[0], cases[1]}) {
        if (turned_skipped)
            break;
        reset_live_renderer();
        const Drawn top = draw(patch);
        const auto expected = patch_mean(top.frame, top);
        if (!turn_live_camera(kTurnedYaw, kTurnedPitch))
            fail("the camera did not turn");
        const Drawn turned = draw(patch);
        if (!card_turned()) {
            std::cout << "underwater features check: turned view skipped: the graphics card did "
                         "not draw it\n";
            turned_skipped = true;
            reset_live_renderer();
            break;
        }
        const auto seen = turned_mean(turned, patch, [](int, int) { return true; });
        const double most = channel_gap(seen.value, expected);
        std::cout << "underwater features check: " << patch.name
                  << " turned, with the water's effects: mean " << seen.value[0] << ","
                  << seen.value[1] << "," << seen.value[2] << " over " << seen.count
                  << " pixels, top-down Full " << expected[0] << "," << expected[1] << ","
                  << expected[2] << "; most " << most << '\n';
        require(
            seen.count > 0 && most <= kMostTurnedMean,
            patch.name + " turned: the patch is tinted, or it left the view"
        );
    }

    if (!turned_skipped) {
        water_effects(false);
        // Whole tiles of Smooth and Classic grey the same way in the turned
        // view, so both are held to the top-down Smooth picture. The soft
        // edge, where the styles differ, is left out. Classic in the turned
        // view is the game's dithered fog.
        settings.fog_of_war = oa::ui::engine_settings::FogOfWar::smooth;
        world.game.graphics_flags =
            static_cast<uint16_t>(saved_graphics & ~init::preference_flags::dithered_fog);
        for (const FogCase fog : {FogCase::unseen, FogCase::unmapped}) {
            if (turned_skipped)
                break;
            set_fog(fog);
            reset_live_renderer();
            const Drawn top = draw(edge);
            const char* place =
                fog == FogCase::unseen ? "the edge of sight" : "never-mapped ground";
            for (const bool dithered : {false, true}) {
                if (turned_skipped)
                    break;
                settings.fog_of_war = dithered ? oa::ui::engine_settings::FogOfWar::classic
                                               : oa::ui::engine_settings::FogOfWar::smooth;
                world.game.graphics_flags = static_cast<uint16_t>(
                    dithered ? saved_graphics | init::preference_flags::dithered_fog
                             : saved_graphics & ~init::preference_flags::dithered_fog
                );
                if (!turn_live_camera(kTurnedYaw, kTurnedPitch))
                    fail("the camera did not turn");
                const Drawn turned = draw(edge);
                if (!card_turned()) {
                    std::cout << "underwater features check: turned view skipped: the graphics "
                                 "card did not draw it\n";
                    turned_skipped = true;
                    reset_live_renderer();
                    break;
                }
                const auto name = std::string(dithered ? "classic" : "smooth") + " fog, " + place;
                for (const int kind : {0, 1, 2}) {
                    if (fog == FogCase::unseen && kind == 2)
                        continue;
                    std::array<double, 3> wanted{};
                    std::size_t wanted_count = 0;
                    for (int row = 0; row < top.height; ++row)
                        for (int column = 0; column < top.width; ++column) {
                            if (top.opaque[static_cast<std::size_t>(row) * top.width + column] == 0)
                                continue;
                            if (whole_class(top, column, row) != kind)
                                continue;
                            const auto rgb = pixel_at(top.frame, top.left + column, top.top + row);
                            for (std::size_t channel = 0; channel < 3; ++channel)
                                wanted[channel] += rgb[channel];
                            ++wanted_count;
                        }
                    if (wanted_count != 0)
                        for (auto& channel : wanted)
                            channel /= static_cast<double>(wanted_count);
                    const auto seen = turned_mean(turned, edge, [&](int column, int row) {
                        return whole_class(top, column, row) == kind;
                    });
                    const char* kind_name = kind == 0   ? "in sight"
                                            : kind == 1 ? "grey"
                                                        : "unexplored";
                    if (wanted_count == 0) {
                        require(false, name + ": the patch has no pixels " + kind_name);
                        continue;
                    }
                    const double most = channel_gap(seen.value, wanted);
                    int darkest = 0;
                    for (double channel : seen.value)
                        darkest = std::max(darkest, static_cast<int>(std::lround(channel)));
                    std::cout << "underwater features check: sight-edge turned, " << name << ", "
                              << kind_name << ": mean " << seen.value[0] << "," << seen.value[1]
                              << "," << seen.value[2] << " over " << seen.count
                              << " pixels, top-down Full " << wanted[0] << "," << wanted[1] << ","
                              << wanted[2] << "; most " << most << '\n';
                    if (kind == 2) {
                        require(
                            seen.count > 0 && darkest <= kMostTurnedBlack,
                            name + ": unexplored ground in the turned view is not black"
                        );
                    } else {
                        require(
                            seen.count > 0 && most <= kMostTurnedFog,
                            name + ": the turned view does not " +
                                (kind == 0 ? std::string("keep the patch's colours in sight")
                                           : std::string("grey the patch with the ground"))
                        );
                    }
                }
            }
        }
    }
    world.game.graphics_flags = saved_graphics;
    reset_live_renderer();
    set_fog(FogCase::none);
    if (!failures.empty()) {
        std::string all;
        for (const auto& failure : failures)
            all += (all.empty() ? "" : "; ") + failure;
        fail(all);
    }
    std::cout << "underwater features check: passed\n";
    return 0;
}

} // namespace oa::app
