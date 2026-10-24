// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-render-tiers: the accelerated presentation switched on over the
// main menu and a skirmish, or a campaign mission, each presented frame read
// back and compared with what the processor composes or with the card's
// references applied to the scene; frames that depend on none before them,
// the first after the tier is switched on or the window resized among them;
// the standard tier's picture for the readers that keep one; the view drawn
// between map pixels as a slow scroll moves it, and the pointer picking what
// is drawn (runtime_smooth_pan_check.cpp); the card's textures made once
// and its prescale targets drawn once a painted frame; the Full tier
// following a walking unit at its farthest zoom, the ground at the view's
// exact place every frame and its picture moving by whole pixels; and the
// Full tier's model stage over the zoom-1 frame, its frame of models
// against the processor's raster of the same list (check_full_models).
// The tier comes from the game's own decision: --hardware-acceleration
// switches it on after the start-up function test passed, and the check
// switches it off and on again as --no-hardware-acceleration and
// --hardware-acceleration would. With --native-density, or the Native pixel
// density setting on, the window opened at the display's own density, and
// after the main menu and the loading screen
// the check runs its density case alone: the match laid out in window
// points, read back at the display's size, at zoom 1 and a whole-number
// density the processor's composition enlarged by nearest replication, and
// the unit under the pointer the one drawn there.
#include "oa/app/runtime.hpp"

#include "engine_settings_menu_host.hpp"
#include "engine_settings_state.hpp"
#include "frame_stats_panel.hpp"
#include "full_fog.hpp"
#include "full_presentation.hpp"
#include "match_models.hpp"
#include "render_host.hpp"
#include "render_run.hpp"
#include "runtime_full.hpp"
#include "xrgb_conversion.hpp"

#include "oa/formats/png.hpp"
#include "oa/platform/machine.hpp"
#include "oa/platform/render_probe.hpp"
#include "oa/present/game_text.hpp"
#include "oa/present/model/rgb_bridge.hpp"
#include "oa/sim/feature_runtime.hpp"
#include "oa/sim/unit_spawn/spawn_runtime.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/gadget_render.hpp"
#include "oa/ui/hud/kill_board.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <optional>
#include <set>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace policy = render_policy;
namespace wr = oa::present::world_renderer;
namespace gw = oa::present::gpu_world;

/// Exit code of a check that skipped, which ctest reports as skipped.
constexpr int skipped_exit_code = 77;

/// A window whose chrome and front end scale by a whole number (2), and
/// one whose chrome does not (1.6).
constexpr int whole_scale_width = 1280;
constexpr int whole_scale_height = 960;
/// A window as tall whose chrome scales by 2 with ui.resource-panel on too,
/// its top bar wide enough for the hack's clock line at that scale.
constexpr int wide_whole_scale_width = 1920;
constexpr int part_scale_width = 1024;
constexpr int part_scale_height = 768;

/// Distance from the pointer that covers every cursor frame.
constexpr int cursor_reach = 64;

/// The zooms the area pass is checked at, and those the card magnifies at.
constexpr std::array<float, 3> area_zooms{0.5F, 0.6F, 0.75F};
constexpr std::array<float, 3> magnified_zooms{1.37F, 2.0F, 4.0F};
/// The zoom of the frame drawn after frames between ticks, and the
/// fractions of a tick those show.
constexpr float history_zoom = 1.37F;
constexpr std::array<float, 3> between_ticks{0.25F, 0.5F, 0.75F};
/// The zoom the capture is checked at.
constexpr float capture_zoom = 2.0F;
/// The zoom ease: from the lowest zoom to the highest and back, each frame
/// this many times the last.
constexpr float ease_step = 1.0905F;
/// The zooms the pictures for the maintainer are taken at.
constexpr std::array<float, 3> picture_zooms{0.5F, 1.0F, 2.5F};
/// The zooms the first frame after the tier is switched on, or after the
/// window is resized, is checked magnified at.
constexpr std::array<float, 2> first_frame_zooms{1.37F, 2.5F};
/// The zooms the standard tier's picture kept for a reader is checked at:
/// one the area pass reduces and one the card magnifies.
constexpr std::array<float, 2> kept_picture_zooms{0.5F, 2.0F};
/// Frames the loop presents with the HUD's prescale target counted.
constexpr int counted_hud_frames = 3;
/// Units each side of the fight the check draws, and the ticks it plays
/// before its first frame, so that lasers fire and units move.
constexpr std::size_t fight_units_per_side = 10;
constexpr int fight_ticks = 150;
/// The weapon render types of a laser's shot, drawn as lines, and of a
/// shot that draws a lens of the ground under it.
constexpr uint8_t laser_render_type = 0;
constexpr uint8_t lens_render_type = 2;
/// How far the projectile pass's case puts a laser's tail from its head,
/// 16.16 map pixels across, and how far past the view's edge it puts the
/// shots off the battlefield, in map pixels.
constexpr oa_fixed lens_check_laser_length = 16 << 16;
constexpr int32_t lens_check_margin = 8;
/// The most shots that case puts in the pool at once.
constexpr std::ptrdiff_t lens_check_shots = 2;

/// The bounds the Full tier's explosion flashes are held to against the
/// processor's, over the pixels where a flash alone is the card's own and
/// one flash reaches: the card lights each pixel by the light table's
/// share, one more than row/30 times the colour under it, held to twice it,
/// where the processor takes the palette entry nearest that light (and
/// lights row 31 a thirtieth further). The bounds hold the share with room:
/// the fight's flashes measured a mean of under one level and at most 59
/// levels, where the palette has no colour near the light.
///
/// Where flashes stack, the processor goes back to the palette after each
/// flash, and the palette's nearest colours to a lit one are paler, so a few
/// flashes take a pixel to white; the card multiplies flash after flash and
/// keeps the colour's hue (the base game's grass under three flashes of row
/// 31: white, and 152, 255, 255 on the card). Those pixels are reported, not
/// held: the Full tier does not light stacked flashes as the light table
/// does.
constexpr double most_flash_mean_difference = 2.0;
constexpr int most_flash_difference = 80;

/// Most a channel of a frame a graphics card scaled may differ from the
/// reference: a card weighs a texel's neighbours at a precision of its own,
/// as coarse as 64ths of a texel, which moves a level by up to 4 at full
/// contrast, and rounds its own way.
constexpr int most_card_difference = 4;
/// Most a channel of a frame SDL's software renderer scaled may differ from
/// that renderer's own LINEAR (software_linear_rgb24), which it equals on a
/// processor with SSE2 or NEON; a build without either truncates twice and
/// can give one level less.
constexpr int most_software_difference = 2;
/// The most the mean difference of a channel may be, on any renderer.
constexpr double most_mean_scaled_difference = 0.5;
/// Most a channel of the HUD may differ from the composition at a whole-number scale.
constexpr int most_hud_difference = 1;
/// The share of the battlefield a Full frame of a view between map pixels
/// may differ from the frame before moved by the view's offset: one in this
/// many pixels, which the renderer may place a pixel apart.
constexpr uint64_t most_moved_mismatch_share = 100;
/// The follow at the farthest zoom: map pixels the followed unit walks
/// toward the map's middle, the ticks it walks before the follow begins,
/// the frames followed, four to a tick, as a frame rate four times the
/// tick rate draws them.
constexpr int32_t follow_walk_reach = 1500;
constexpr int follow_walk_start_ticks = 45;
constexpr int follow_frames = 48;
constexpr int follow_frames_a_tick = 4;
/// Screen pixels the ground drawn may lie from the view's exact place:
/// what float arithmetic leaves.
constexpr double follow_place_tolerance = 1.0e-3;
/// Layout pixels around the battlefield's middle, where the followed unit
/// walks, left out of the whole-pixel moves.
constexpr int follow_middle_reach = 40;
/// The fractions of a screen pixel the follow's shifts are told apart by,
/// and the least number of them the follow is to pass through.
constexpr double follow_shift_grain = 100.0;
constexpr std::size_t follow_least_shifts = 8;
/// Most a window point may lie from a layout pixel's place at native
/// density, in window points: the view maps one onto the other exactly but
/// for the rounding of floats.
constexpr float window_point_tolerance = 0.01F;
/// The most the mean of a channel of the Full tier's two-level blend of the
/// terrain, between zoom 0.5 and 1, may differ from the standard tier's
/// box filter of the same view: the blend is the card's own filter, not
/// today's bytes, so the bound is that of the terrain of the installed
/// game's maps on SDL's software renderer, with room; a picture drawn from
/// the wrong level, or a map pixel off, strays past it. The blend itself
/// is held to the renderer's own LINEAR of each tile's quad, pass over
/// pass, within the renderer's tolerance.
constexpr double most_blend_mean_difference = 24.0;
/// Most a channel of such a pixel may differ at all: the blend of two
/// levels lands anywhere between them.
constexpr int most_blend_difference = 255;
/// The levels SDL's software renderer's blend of a texture at an alpha
/// gives: dst = ((src - dst) * alpha >> 8) + dst, within a level of the
/// exact blend, so a blended pass of the reference computed exactly keeps
/// within most_software_difference of the read-back.
constexpr int blend_shift = 8;
/// The most the mean difference of a channel may be against that model on
/// SDL's software renderer: two passes, each rounded by the renderer, and
/// the blend's own rounding, where one LINEAR draw is held to
/// most_mean_scaled_difference; a card is held to that mean.
constexpr double most_blended_mean_difference = 1.0;
/// Most a channel of the zoomed-out terrain at zoom 0.5 may differ from the
/// box filter: level 0 reduced by halving rounds the mean of four as the
/// renderer does, which may lie a level from the box filter's rounding.
constexpr int most_halving_difference = 1;
/// Pixels left out at each edge of a HUD strip when it is held to its
/// reference: a card's LINEAR read may take the HUD layer beyond the
/// strip's edge from the prescale target, where the reference clamps.
constexpr int strip_edge_inset = 1;
/// The fewest terrain pixels a zoomed-out Full frame must show beside
/// what the card draws its own way for its comparison to count: the fog
/// covers most of a zoomed-out view, and a unit the rest where one stands.
constexpr std::size_t least_terrain_pixels = 4096;
/// The fewest pixels the Full tier's whole-frame comparison must find
/// beside what the card draws its own way at a whole-number zoom.
constexpr std::size_t least_compared_pixels = 16384;
/// The fewest pixels a fog case must find under the tiles it holds exactly.
constexpr std::size_t least_fog_pixels = 2048;
/// Most a channel the card blended may differ from the blend computed
/// exactly: the renderer rounds each blend its own way.
constexpr int most_blend_rounding = 2;
/// Most a channel of the greyed fog by its mask may differ from the greyed
/// terrain, or lie past the range from the colour to it: the mask rounds
/// through four blends (the colour darkened, the levels added, the sum
/// scaled, the resolve) where the terrain drawn alone rounds through one a
/// level.
constexpr int most_mask_rounding = 4;
/// Pixels a sprite's rectangle is grown by each side in the mask of what
/// the card draws its own way, for the card's placing between pixels.
constexpr int sprite_mask_margin = 1;
/// Pixels of the kill board neither shaded nor painted that a failure lists.
constexpr std::size_t most_strays_listed = 12;

/// Frames the kill board takes to slide out or away, with room.
constexpr int most_slide_frames = 64;

/// A rectangle of a frame.
struct Area {
    int x{};
    int y{};
    int w{};
    int h{};
};

/// How far two frames differ over an area.
struct Difference {
    int most{};
    double mean{};
    std::size_t pixels{}; ///< pixels compared
};

/// Compares two frames over an area, leaving out the pixels in a second area.
///
/// @param first a frame
/// @param second another of the same size
/// @param area the area compared, clipped to the frames
/// @param left_out an area not compared
/// @return the largest and the mean difference of a channel
Difference compare(
    const renderer::Surface& first,
    const renderer::Surface& second,
    const Area& area,
    const Area& left_out
) {
    Difference difference;
    if (first.width != second.width || first.height != second.height) {
        difference.most = 255;
        return difference;
    }
    double sum = 0.0;
    std::size_t channels = 0;
    const int right = std::min(area.x + area.w, static_cast<int>(first.width));
    const int bottom = std::min(area.y + area.h, static_cast<int>(first.height));
    for (int y = std::max(area.y, 0); y < bottom; ++y)
        for (int x = std::max(area.x, 0); x < right; ++x) {
            if (x >= left_out.x && x < left_out.x + left_out.w && y >= left_out.y &&
                y < left_out.y + left_out.h)
                continue;
            ++difference.pixels;
            const auto at = (static_cast<std::size_t>(y) * first.width + x) * 3U;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const int delta =
                    std::abs(int{first.rgb[at + channel]} - int{second.rgb[at + channel]});
                difference.most = std::max(difference.most, delta);
                sum += delta;
                ++channels;
            }
        }
    difference.mean = channels != 0 ? sum / static_cast<double>(channels) : 0.0;
    return difference;
}

/// How far two frames differ over an area, apart from and under a mask.
struct MaskedDifference {
    Difference beside{}; ///< over the pixels the mask leaves
    Difference under{};  ///< over the pixels the mask marks
};

/// Compares two frames over an area, leaving out the pixels in a second
/// area, the pixels a mask marks counted apart from the rest.
///
/// @param first a frame
/// @param second another of the same size
/// @param area the area compared, clipped to the frames
/// @param left_out an area not compared
/// @param mask one byte a pixel of the frames; a set byte marks the pixel
/// @return the differences beside and under the mask
MaskedDifference compare_masked(
    const renderer::Surface& first,
    const renderer::Surface& second,
    const Area& area,
    const Area& left_out,
    const std::vector<uint8_t>& mask
) {
    MaskedDifference difference;
    if (first.width != second.width || first.height != second.height ||
        mask.size() != std::size_t{first.width} * first.height) {
        difference.beside.most = 255;
        return difference;
    }
    double beside_sum = 0.0;
    double under_sum = 0.0;
    const int right = std::min(area.x + area.w, static_cast<int>(first.width));
    const int bottom = std::min(area.y + area.h, static_cast<int>(first.height));
    for (int y = std::max(area.y, 0); y < bottom; ++y)
        for (int x = std::max(area.x, 0); x < right; ++x) {
            if (x >= left_out.x && x < left_out.x + left_out.w && y >= left_out.y &&
                y < left_out.y + left_out.h)
                continue;
            const auto pixel = static_cast<std::size_t>(y) * first.width + x;
            Difference& counted = mask[pixel] != 0 ? difference.under : difference.beside;
            double& sum = mask[pixel] != 0 ? under_sum : beside_sum;
            ++counted.pixels;
            for (std::size_t channel = 0; channel < 3; ++channel) {
                const int delta = std::abs(
                    int{first.rgb[pixel * 3U + channel]} - int{second.rgb[pixel * 3U + channel]}
                );
                counted.most = std::max(counted.most, delta);
                sum += delta;
            }
        }
    if (difference.beside.pixels != 0)
        difference.beside.mean = beside_sum / (3.0 * static_cast<double>(difference.beside.pixels));
    if (difference.under.pixels != 0)
        difference.under.mean = under_sum / (3.0 * static_cast<double>(difference.under.pixels));
    return difference;
}

/// Marks a rectangle of a mask, grown by a margin on each side.
///
/// @param[in,out] mask one byte a pixel of a frame
/// @param width the frame's columns
/// @param height the frame's rows
/// @param x the rectangle's left column
/// @param y its top row
/// @param w its columns
/// @param h its rows
/// @param margin pixels it is grown by on each side
void mark_rect(
    std::vector<uint8_t>& mask,
    uint32_t width,
    uint32_t height,
    int x,
    int y,
    int w,
    int h,
    int margin
) {
    const int left = std::max(x - margin, 0);
    const int top = std::max(y - margin, 0);
    const int right = std::min(x + w + margin, static_cast<int>(width));
    const int bottom = std::min(y + h + margin, static_cast<int>(height));
    for (int row = top; row < bottom; ++row)
        for (int column = left; column < right; ++column)
            mask[static_cast<std::size_t>(row) * width + static_cast<std::size_t>(column)] = 1;
}

/// Marks the pixels of a line, as the game steps it, and those within a
/// reach of them.
///
/// @param[in,out] mask one byte a pixel of a frame
/// @param width the frame's columns
/// @param height the frame's rows
/// @param x0 the first pixel's column
/// @param y0 its row
/// @param x1 the last pixel's column
/// @param y1 its row
/// @param reach pixels each side of the line marked with it
void mark_line(
    std::vector<uint8_t>& mask,
    uint32_t width,
    uint32_t height,
    int x0,
    int y0,
    int x1,
    int y1,
    int reach
) {
    // A line off the frame is marked only where it crosses it; one far off
    // it would step for long, so its pixels are bounded first.
    const int bound = static_cast<int>(std::max(width, height)) * 2;
    if (std::abs(x0) > bound || std::abs(y0) > bound || std::abs(x1) > bound ||
        std::abs(y1) > bound)
        return;
    int dx = std::abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -std::abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int error = dx + dy;
    while (true) {
        mark_rect(mask, width, height, x0, y0, 1, 1, reach);
        if (x0 == x1 && y0 == y1)
            break;
        const auto twice = error * 2;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

/// Prints the draw batches of a card frame whose triangles cover a pixel,
/// with their page, level, blend, sampling and the texture coordinates of
/// the triangle's corners, for the report of a frame that strays.
///
/// @param frame the card frame as it was run
/// @param x the pixel's column, in the frame's final target
/// @param y the pixel's row
void print_card_draws_at(const card::CardFrame& frame, int x, int y) {
    const float px = static_cast<float>(x) + 0.5F;
    const float py = static_cast<float>(y) + 0.5F;
    const auto side = [](const card::Vertex& a, const card::Vertex& b, float cx, float cy) {
        return (b.x - a.x) * (cy - a.y) - (b.y - a.y) * (cx - a.x);
    };
    for (std::size_t index = 0; index < frame.batches.size(); ++index) {
        const auto& batch = frame.batches[index];
        if (batch.operation != card::Operation::draw || batch.target != card::TargetHandle{})
            continue;
        for (card::Index at = batch.first_index; at + 2 < batch.first_index + batch.index_count;
             at += 3) {
            const auto& a = frame.vertices[frame.indices[at]];
            const auto& b = frame.vertices[frame.indices[at + 1]];
            const auto& c = frame.vertices[frame.indices[at + 2]];
            const float ab = side(a, b, px, py);
            const float bc = side(b, c, px, py);
            const float ca = side(c, a, px, py);
            if ((ab >= 0 && bc >= 0 && ca >= 0) || (ab <= 0 && bc <= 0 && ca <= 0))
                std::cout << "render tiers check:   batch " << index << " page " << batch.page.value
                          << " level " << int{batch.level} << " blend "
                          << int{static_cast<uint8_t>(batch.blend)} << " sampling "
                          << int{static_cast<uint8_t>(batch.sampling)} << " triangle at index "
                          << at << ": (" << a.x << ", " << a.y << " uv " << a.u << ", " << a.v
                          << " a " << a.colour.alpha << ") (" << b.x << ", " << b.y << " uv " << b.u
                          << ", " << b.v << ") (" << c.x << ", " << c.y << " uv " << c.u << ", "
                          << c.v << ")\n";
        }
    }
}

/// Writes a frame as a PNG file.
///
/// @param path the file
/// @param frame the frame
void write_png(const fs::path& path, const renderer::Surface& frame) {
    std::vector<uint8_t> file;
    const oa::formats::png::Header header{
        frame.width, frame.height, 8, oa::formats::png::ColorType::rgb, {}
    };
    if (!oa::formats::png::write(oa::formats::png::Image{header, {}, frame.rgb}, &file))
        throw std::runtime_error("render tiers check: cannot encode " + path.string());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size())
    );
    if (!output)
        throw std::runtime_error("render tiers check: cannot write " + path.string());
}

/// Returns a frame enlarged by nearest replication: each pixel a square
/// block of a whole number of pixels on a side.
///
/// @param frame the frame
/// @param factor pixels on a side of each block, at least 1
/// @return the enlarged frame
renderer::Surface enlarged(const renderer::Surface& frame, uint32_t factor) {
    renderer::Surface result{
        frame.width * factor,
        frame.height * factor,
        std::vector<uint8_t>(std::size_t{frame.width} * factor * frame.height * factor * 3U)
    };
    for (uint32_t y = 0; y < result.height; ++y)
        for (uint32_t x = 0; x < result.width; ++x)
            std::copy_n(
                frame.rgb.begin() + static_cast<std::ptrdiff_t>(
                                        (std::size_t{y / factor} * frame.width + x / factor) * 3U
                                    ),
                3,
                result.rgb.begin() +
                    static_cast<std::ptrdiff_t>((std::size_t{y} * result.width + x) * 3U)
            );
    return result;
}

/// Presents a frame and returns the frame as the processor composed it.
///
/// @param presented presents a frame
/// @param composed composes the frame
/// @return the composition
renderer::Surface composed_after(
    const std::function<renderer::Surface()>& presented,
    const std::function<renderer::Surface()>& composed
) {
    std::ignore = presented();
    return composed();
}

/// Reduces the Full tier's world target on the processor as the card
/// reduces it (full_supersampling::WorldTargetPlan): by the factor's
/// halvings, each a LINEAR draw at exactly one half, which on SDL's
/// software renderer is that renderer's own LINEAR (software_linear_rgb24)
/// and on a card the exact box.
///
/// @param texture the target's texture, RGB24, rows of `width` pixels
/// @param width texture pixels across
/// @param height texture pixels down
/// @param plan how the frame was drawn through the target
/// @param software the renderer is SDL's software renderer
/// @return the reduced picture, RGB24 at the plan's destination's size
std::vector<uint8_t> reduce_world_target_reference(
    const std::vector<uint8_t>& texture,
    uint32_t width,
    uint32_t height,
    const full_supersampling::WorldTargetPlan& plan,
    bool software
) {
    const auto destination_width = static_cast<uint32_t>(plan.destination.width);
    const auto destination_height = static_cast<uint32_t>(plan.destination.height);
    std::vector<uint8_t> out(std::size_t{destination_width} * destination_height * 3U);
    const wr::RgbTarget target{
        out.data(), destination_width, destination_height, destination_width
    };
    const auto linear = [&](const wr::RgbSource& source, const wr::RgbTarget& into) {
        const SDL_Rect landed{0, 0, static_cast<int>(into.width), static_cast<int>(into.height)};
        if (software)
            software_linear_rgb24(source, 1, landed, into);
        else
            wr::bilinear_rgb24(
                source,
                {static_cast<double>(into.width) / source.width,
                 static_cast<double>(into.height) / source.height,
                 0.0,
                 0.0},
                into
            );
    };
    // The texture's half: a LINEAR draw at exactly one half, the box of four.
    const uint32_t half_width = width / 2;
    const uint32_t half_height = height / 2;
    std::vector<uint8_t> half_storage(std::size_t{half_width} * half_height * 3U);
    const wr::RgbSource whole{texture.data(), width, height, width};
    linear(whole, {half_storage.data(), half_width, half_height, half_width});
    const wr::RgbSource half{half_storage.data(), half_width, half_height, half_width};
    linear(plan.factor == 4 ? half : whole, target);
    return out;
}

/// Returns a name's text for a zoom: "0.5", "1", "2.5".
///
/// @param zoom the zoom
/// @return its text
std::string zoom_text(float zoom) {
    std::string text = std::to_string(zoom);
    text.erase(text.find_last_not_of('0') + 1);
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text;
}

/// Counts the explosions' flashes that reach each pixel of a frame: each
/// flash's frame at the zoom, grown by sprite_mask_margin as card_draw_mask
/// marks it, held to 255.
///
/// @param list the frame's draws
/// @param field the battlefield on the frame
/// @param zoom the frame's zoom
/// @param width the frame's columns
/// @param height the frame's rows
/// @return one byte a pixel of the frame: how many flashes reach it
std::vector<uint8_t> flash_cover(
    const WorldDrawList& list, const Area& field, float zoom, uint32_t width, uint32_t height
) {
    std::vector<uint8_t> cover(std::size_t{width} * height, 0);
    for (const auto& draw : list.draws) {
        if (draw.kind != WorldDrawKind::lit_sprite)
            continue;
        const auto& sprite = list.sprites[draw.index];
        if (sprite.frame == nullptr)
            continue;
        const SceneRect rect = sprite_scene_rect(sprite, zoom);
        const int x = field.x + static_cast<int>(rect.left);
        const int y = field.y + static_cast<int>(rect.top);
        const int w = std::max(1, static_cast<int>(rect.right - rect.left));
        const int h = std::max(1, static_cast<int>(rect.bottom - rect.top));
        const int left = std::max(x - sprite_mask_margin, 0);
        const int top = std::max(y - sprite_mask_margin, 0);
        const int right = std::min(x + w + sprite_mask_margin, static_cast<int>(width));
        const int bottom = std::min(y + h + sprite_mask_margin, static_cast<int>(height));
        for (int row = top; row < bottom; ++row)
            for (int column = left; column < right; ++column) {
                auto& count =
                    cover[static_cast<std::size_t>(row) * width + static_cast<std::size_t>(column)];
                if (count < 255)
                    ++count;
            }
    }
    return cover;
}

/// What the card draws its own way, marked on a frame of the fight: the
/// sprites the alpha table blends, the explosions' flashes, which the card
/// lights by the light table's share where the processor snaps the light
/// to the palette (held within most_flash_difference and
/// most_flash_mean_difference of it),
/// every sprite at a zoom that is not a
/// whole number, every sprite that reaches a fog tile that is not wholly
/// clear, which the fog's masks cut where the card draws the sprite whole,
/// greyed or not at all by its cell; the lines, grown by their width; the
/// units, projectiles, debris and fragments, which the card draws as
/// meshes (check_full_models holds them to the processor's raster) with
/// their shadows; the projectiles' lenses, which the card does not draw;
/// and the fog tiles that are not wholly in sight or wholly
/// out of it, whose edges ramp where the processor's masks cut, with
/// every tile a corner out of sight at a zoom below 1, where the greyed
/// level is the box of the grey where the processor grays the box; and the
/// quads the painters after the fog asked the card for over the world, such
/// as the shadow of text in the modern fonts, which the card blends where
/// the processor reduces its blend to the palette.
/// Everything beside the mark is the processor's picture exactly at a
/// whole-number zoom.
///
/// @param list the frame's draws
/// @param grid the fog grid the frame drew its fog from
/// @param quads the painters' quads, in pixels of the battlefield layer
/// @param field the battlefield on the frame
/// @param zoom the frame's zoom
/// @param width the frame's columns
/// @param height the frame's rows
/// @param flashes the explosions' flashes are marked too
/// @return one byte a pixel of the frame, set where the card's own draws lie
std::vector<uint8_t> card_draw_mask(
    const WorldDrawList& list,
    const wr::FogGrid& grid,
    std::span<const FullWorldQuad> quads,
    const Area& field,
    float zoom,
    uint32_t width,
    uint32_t height,
    bool flashes = true
) {
    std::vector<uint8_t> mask(std::size_t{width} * height, 0);
    for (const FullWorldQuad& quad : quads)
        mark_rect(
            mask, width, height, field.x + quad.x, field.y + quad.y, quad.width, quad.height, 0
        );
    const bool whole = std::floor(zoom) == zoom;
    const int reach = static_cast<int>(std::ceil(std::max(1.0F, zoom)));
    const auto scaled = [&](int32_t pixels) {
        return static_cast<int>(
            std::lround(static_cast<double>(pixels) * static_cast<double>(zoom))
        );
    };
    // The fog tiles that are not wholly clear, as the frame's fog laid
    // them, each grown by a pixel for the stepping of their edges; and
    // the tiles whose edges ramp.
    std::vector<uint8_t> fogged(mask.size(), 0);
    const int tile = static_cast<int>(std::ceil(wr::fog_cell_pixels * zoom)) + 2;
    for (int32_t row = 0; row < grid.height; ++row)
        for (int32_t column = 0; column < grid.width; ++column) {
            const auto& masks = grid.at(column, row);
            const int x = field.x + scaled(grid.offset_x + column * wr::fog_cell_pixels) - 1;
            const int y = field.y + scaled(grid.offset_z + row * wr::fog_cell_pixels) - 1;
            if (masks.unseen != 0 || masks.unmapped != 0)
                mark_rect(fogged, width, height, x, y, tile, tile, 0);
            const bool unseen_edge = masks.unseen != 0 && masks.unseen != wr::fog_mask_full;
            const bool unmapped_edge = masks.unmapped != 0 && masks.unmapped != wr::fog_mask_full;
            if (unseen_edge || unmapped_edge || (!whole && masks.unseen != 0))
                mark_rect(mask, width, height, x, y, tile, tile, 0);
        }
    const auto touches_fog = [&](int x, int y, int w, int h) {
        for (int row = std::max(y, 0); row < std::min(y + h, static_cast<int>(height)); ++row)
            for (int column = std::max(x, 0); column < std::min(x + w, static_cast<int>(width));
                 ++column)
                if (fogged
                        [static_cast<std::size_t>(row) * width +
                         static_cast<std::size_t>(column)] != 0)
                    return true;
        return false;
    };
    // A region the bridge captures, in map pixels about the scene's
    // corner, on the window at the zoom.
    const auto mark_region = [&](const oa::Rect32& region) {
        mark_rect(
            mask,
            width,
            height,
            field.x + scaled(region.x1),
            field.y + scaled(region.y1),
            scaled(region.x2 - region.x1 + 1),
            scaled(region.y2 - region.y1 + 1),
            reach
        );
    };
    for (const auto& draw : list.draws) {
        switch (draw.kind) {
        case WorldDrawKind::sprite:
        case WorldDrawKind::blended_sprite:
        case WorldDrawKind::lit_sprite: {
            const auto& sprite = list.sprites[draw.index];
            if (sprite.frame == nullptr || (draw.kind == WorldDrawKind::lit_sprite && !flashes))
                break;
            SceneRect rect = sprite_scene_rect(sprite, zoom);
            const bool effect_styled =
                sprite.soft_effect || sprite.effect_scale != 1.0F || sprite.effect_opacity != 1.0F;
            if (effect_styled) {
                // The standard tier keeps the original cutout. Full may shrink,
                // fade or filter it, so exclude precisely the union of both
                // footprints from palette-exact and flash-only comparisons.
                const float left =
                    (sprite.place.x - sprite.frame->origin_x * sprite.effect_scale) * zoom;
                const float top =
                    (sprite.place.y - sprite.frame->origin_y * sprite.effect_scale) * zoom;
                const float right = left + sprite.frame->width * sprite.effect_scale * zoom;
                const float bottom = top + sprite.frame->height * sprite.effect_scale * zoom;
                rect.left = std::min(rect.left, static_cast<int64_t>(std::floor(left)));
                rect.top = std::min(rect.top, static_cast<int64_t>(std::floor(top)));
                rect.right = std::max(rect.right, static_cast<int64_t>(std::ceil(right)));
                rect.bottom = std::max(rect.bottom, static_cast<int64_t>(std::ceil(bottom)));
            }
            const int left = field.x + static_cast<int>(rect.left);
            const int top = field.y + static_cast<int>(rect.top);
            const int w = std::max(1, static_cast<int>(rect.right - rect.left));
            const int h = std::max(1, static_cast<int>(rect.bottom - rect.top));
            const bool fog_cut = touches_fog(
                left - sprite_mask_margin,
                top - sprite_mask_margin,
                w + 2 * sprite_mask_margin,
                h + 2 * sprite_mask_margin
            );
            if (whole && draw.kind == WorldDrawKind::sprite && !fog_cut && !effect_styled)
                break;
            mark_rect(mask, width, height, left, top, w, h, sprite_mask_margin);
            break;
        }
        case WorldDrawKind::model:
            mark_region(list.models[draw.index].plan.region);
            break;
        case WorldDrawKind::projectile:
            mark_region(list.projectiles[draw.index].region);
            break;
        case WorldDrawKind::debris:
            mark_region(list.debris[draw.index].region);
            break;
        case WorldDrawKind::fragment:
            mark_region(list.fragments[draw.index].region);
            break;
        case WorldDrawKind::lens: {
            const auto& centre = list.lenses[draw.index];
            const int side = std::max(1, scaled(projectile_lens_side));
            mark_rect(
                mask,
                width,
                height,
                field.x + centre.x - scaled(projectile_lens_side / 2),
                field.y + centre.y - scaled(projectile_lens_side / 2),
                side,
                side,
                reach
            );
            break;
        }
        case WorldDrawKind::line: {
            const auto& line = list.lines[draw.index];
            mark_line(
                mask,
                width,
                height,
                field.x + line.x0,
                field.y + line.y0,
                field.x + line.x1,
                field.y + line.y1,
                reach
            );
            break;
        }
        case WorldDrawKind::selection_line: {
            // Map pixels about the camera, at the zoom.
            const auto& line = list.lines[draw.index];
            const auto at = [&](int32_t pixel) {
                return static_cast<int>(std::lround((static_cast<double>(pixel) + 0.5) * zoom));
            };
            mark_line(
                mask,
                width,
                height,
                field.x + at(line.x0),
                field.y + at(line.y0),
                field.x + at(line.x1),
                field.y + at(line.y1),
                reach + 1
            );
            break;
        }
        case WorldDrawKind::pixel_square:
        case WorldDrawKind::commit:
        case WorldDrawKind::commit_always:
            break;
        }
    }
    return mask;
}

/// Returns a mask enlarged by nearest replication, as enlarged() enlarges
/// a frame.
///
/// @param mask one byte a pixel of a frame
/// @param width the frame's columns
/// @param height the frame's rows
/// @param factor pixels on a side of each block, at least 1
/// @return the enlarged mask
std::vector<uint8_t>
enlarged_mask(const std::vector<uint8_t>& mask, uint32_t width, uint32_t height, uint32_t factor) {
    std::vector<uint8_t> result(std::size_t{width} * factor * height * factor, 0);
    const uint32_t wide = width * factor;
    for (uint32_t y = 0; y < height * factor; ++y)
        for (uint32_t x = 0; x < wide; ++x)
            result[std::size_t{y} * wide + x] = mask[std::size_t{y / factor} * width + x / factor];
    return result;
}

} // namespace

int Runtime::check_render_tiers() {
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("render tiers check: " + what);
    };
    if (sdl_.renderer == nullptr || sdl_.window == nullptr || !render_run_ ||
        render_run_->host == nullptr)
        fail("needs the SDL renderer and the renderer host that made it");
    // Under the 2 GiB threshold the accelerated tier never runs, whatever
    // the flags; on a renderer the probe rejects it runs only when forced.
    const auto machine = oa::platform::read_machine_traits();
    auto& inputs = render_run_->host->tier_inputs();
    if (inputs.memory < policy::smallest_accelerated_memory) {
        std::cout << "render tiers check: skipped: the machine reports less than the 2 GiB "
                     "threshold of memory\n";
        return skipped_exit_code;
    }
    // What the renderer host found of the renderer when it made it, and the
    // capability the tier was decided from.
    const auto& facts = render_run_->host->facts();
    if (inputs.capability != policy::Capability::capable && !options_.force_capable) {
        std::cout << "render tiers check: skipped: the renderer " << facts.renderer
                  << " is not capable of the accelerated tier\n";
        return skipped_exit_code;
    }
    using oa::ui::engine_settings::HardwareAcceleration;
    if (!options_.hardware_acceleration ||
        *options_.hardware_acceleration == HardwareAcceleration::off)
        fail("needs --hardware-acceleration, which switches the accelerated tier on");
    // The tier as the flags decide it: the Basic cases switch it as
    // --hardware-acceleration=basic and --no-hardware-acceleration would,
    // and the Full cases, which need --hardware-acceleration=full, set the
    // level themselves.
    const HardwareAcceleration asked_level = *options_.hardware_acceleration;
    const auto set_level = [&](HardwareAcceleration level) {
        options_.hardware_acceleration = level;
        update_render_tier();
    };
    const auto switch_tier = [&](bool on) {
        set_level(on ? HardwareAcceleration::basic : HardwareAcceleration::off);
        if (accelerated_presentation() != on)
            fail(
                on ? "--hardware-acceleration=basic did not switch the accelerated tier on"
                   : "--no-hardware-acceleration did not switch the accelerated tier off"
            );
        if (on && full_presentation())
            fail("--hardware-acceleration=basic drew in the full tier");
    };
    const fs::path report_directory = "local/reports";
    fs::create_directories(report_directory);
    // SDL's software renderer is held to its own LINEAR exactly; a card to
    // the references within its precision.
    const bool software = facts.renderer == oa::platform::render_probe::software_renderer;
    const int most_scaled_difference = software ? most_software_difference : most_card_difference;
    const auto scaled_reference = [&](const wr::RgbSource& source,
                                      const CardScale& card,
                                      const SDL_Rect& rectangle,
                                      const wr::RgbTarget& target) {
        const wr::ScenePlacement placement{
            static_cast<double>(rectangle.w) / source.width,
            static_cast<double>(rectangle.h) / source.height,
            static_cast<double>(rectangle.x),
            static_cast<double>(rectangle.y)
        };
        switch (card.filter) {
        case policy::ScaleFilter::nearest:
            wr::nearest_rgb24(source, placement, target);
            break;
        case policy::ScaleFilter::pixelart:
            // SDL's software renderer draws its pixel-art mode NEAREST.
            if (software)
                wr::nearest_rgb24(source, placement, target);
            else
                wr::pixelart_rgb24(source, placement, target);
            break;
        case policy::ScaleFilter::sharp_bilinear:
            if (software)
                software_linear_rgb24(source, card.factor, rectangle, target);
            else
                wr::sharp_bilinear_rgb24(source, placement, card.factor, target);
            break;
        case policy::ScaleFilter::linear:
            if (software)
                software_linear_rgb24(source, 1, rectangle, target);
            else
                wr::bilinear_rgb24(source, placement, target);
            break;
        }
    };

    // The rung every case draws at: the full scene budget with the area
    // pass, magnify on, the chrome filtered, and the card's magnification
    // by PIXELART where the renderer has it, else by sharp-bilinear.
    policy::LadderState rung{};
    rung.method = policy::ZoomOutMethod::area;
    rung.budget = policy::SceneBudget::full;
    rung.magnify = true;
    rung.filtered_chrome = true;
    rung.card =
        oa::app::probe_pixelart(sdl_.renderer, nullptr)
            ? policy::CardFilter::pixelart
            : (oa::platform::light_machine(machine) || oa::platform::running_on_raspberry_pi()
                   ? policy::CardFilter::prescale_quarter
                   : policy::CardFilter::prescale_full);
    rung.standard = false;
    const auto& counts = accelerated_.counts;
    const auto resize = [&](int width, int height) {
        if (!SDL_SetWindowSize(sdl_.window, width, height) || !SDL_SyncWindow(sdl_.window))
            fail(std::string("SDL_SetWindowSize: ") + SDL_GetError());
        apply_output_mode();
    };
    // The match at a whole-number chrome scale: on the whole-scale window,
    // or where ui.resource-panel draws the chrome smaller there to give its
    // clock line room in the top bar, on one as tall and wider.
    const auto resize_whole = [&] {
        resize(whole_scale_width, whole_scale_height);
        if (std::floor(match_layout_.scale) != match_layout_.scale)
            resize(wide_whole_scale_width, whole_scale_height);
    };
    const auto presented = [&]() {
        renderer::Surface frame;
        capture_frame_ = &frame;
        render();
        capture_frame_ = nullptr;
        return frame;
    };
    const auto gamma_of = [&](std::vector<uint8_t> bytes) {
        if (!gamma_identity_)
            for (auto& byte : bytes)
                byte = gamma_table_[byte];
        return bytes;
    };

    // The start-up function test passed, so the tier the flag asks for is
    // accelerated; from here it draws at the check's rung.
    if (inputs.function_test != policy::FunctionTest::passed)
        fail("the start-up function test did not pass");
    render_run_->rung = rung;
    switch_tier(false);
    switch_tier(true);
    if (render_run_->tier.tier != policy::RenderTier::accelerated)
        fail("the tier decided for --hardware-acceleration=basic is not the accelerated tier");

    // The main menu, letterboxed at 2.25 by the card's filter, and at 2 by
    // NEAREST with no prescale target.
    for (const auto& [width, height] :
         {std::pair{kDefaultWindowWidth, kDefaultWindowHeight},
          std::pair{whole_scale_width, whole_scale_height}}) {
        resize(width, height);
        const uint64_t draws_before = counts.prescale_draws;
        const auto read = presented();
        if (!accelerated_presentation())
            fail("the accelerated presentation stopped over the main menu");
        SDL_FRect letterbox{};
        if (!SDL_GetRenderLogicalPresentationRect(sdl_.renderer, &letterbox))
            fail(std::string("SDL_GetRenderLogicalPresentationRect: ") + SDL_GetError());
        const double scale = static_cast<double>(letterbox.w) / surface_.width;
        const auto card = accelerated_card_scale(
            policy::chrome_filter(accelerated_.rung, scale),
            scale,
            surface_.width,
            surface_.height,
            accelerated_.screen_prescale
        );
        const bool whole = std::floor(scale) == scale;
        if (whole &&
            (card.filter != policy::ScaleFilter::nearest || counts.prescale_draws != draws_before))
            fail("the main menu at a whole-number scale was drawn through a prescale target");
        // A larger window draws the menu's buttons again in its own pixels,
        // over a picture whose squares were put back to the menu under them.
        // The card scales that picture. The button squares are then taken
        // from the read-back, which holds them as the window drew them.
        std::vector<uint8_t> punched;
        if (!replace_menu_button_pixels(punched))
            punched = surface_.rgb;
        const auto source_rgb = gamma_of(std::move(punched));
        const wr::RgbSource source{
            source_rgb.data(), surface_.width, surface_.height, surface_.width
        };
        renderer::Surface reference{
            read.width, read.height, std::vector<uint8_t>(read.rgb.size(), 0)
        };
        const wr::RgbTarget target{reference.rgb.data(), read.width, read.height, read.width};
        // The read-back covers the letterbox alone.
        scaled_reference(
            source,
            card,
            {0, 0, static_cast<int>(letterbox.w), static_cast<int>(letterbox.h)},
            target
        );
        EngineSettingsMenuHost::copy_presented_menu_buttons(*this, read, reference, {});
        if (read.width != static_cast<uint32_t>(letterbox.w) ||
            read.height != static_cast<uint32_t>(letterbox.h))
            fail("the main menu's read-back is not its letterbox");
        const Area area{0, 0, static_cast<int>(read.width), static_cast<int>(read.height)};
        const auto difference = compare(read, reference, area, {});
        std::cout << "render tiers check: main menu at " << width << 'x' << height << ", scale "
                  << scale << ", filter " << static_cast<int>(card.filter) << " x" << card.factor
                  << ": most " << difference.most << ", mean " << difference.mean << '\n';
        if (difference.pixels == 0 || difference.most > (whole ? 0 : most_scaled_difference) ||
            difference.mean > most_mean_scaled_difference) {
            write_png(report_directory / "native-render-tiers-menu-presented.png", read);
            write_png(report_directory / "native-render-tiers-menu-reference.png", reference);
            fail("the main menu's presented frame strays from the card's reference");
        }
    }

    // A skirmish, or the campaign mission --campaign and --mission name, its
    // loading screen letterboxed by the card's filter, each of its frames
    // drawn into the prescale target, then drawn at a window whose chrome
    // scales by 2. With Full asked, the match loads in Full, so that its
    // loading screen makes the Full tier's pages.
    resize(kDefaultWindowWidth, kDefaultWindowHeight);
    if (asked_level == HardwareAcceleration::full)
        set_level(HardwareAcceleration::full);
    const uint64_t draws_before_loading = counts.prescale_draws;
    if (options_.campaign_mission)
        std::ignore = start_headless_campaign_mission();
    else
        start_benchmark_skirmish();
    if (!accelerated_presentation())
        fail("the accelerated presentation stopped over the loading screen");
    if (rung.card != policy::CardFilter::pixelart && counts.prescale_draws == draws_before_loading)
        fail("the loading screen was not drawn by the card's filter");
    std::cout << "render tiers check: the loading screen drew "
              << counts.prescale_draws - draws_before_loading << " frames by the card's filter\n";
    // The loading screen opened the executor, ran the Full function test
    // and built and uploaded the terrain pages with the terrain, and the
    // zoomed-out levels' mosaics and their greyed mosaics, before the world
    // was built, letting the atlas's texels go once uploaded; the first
    // match frame draws from those pages and makes none.
    if (asked_level == HardwareAcceleration::full) {
        if (!full_presentation() || !full_ || render_run_->tier.tier != policy::RenderTier::full)
            fail("the full tier was not on as the match loaded");
        const auto& full = *full_;
        if (!full.pages_from_load || full.pages.empty() ||
            full.pages.size() != full.atlas.pages.size())
            fail("the loading screen did not make the terrain pages");
        const uint64_t pages_alive = full.executor.counts().pages_alive;
        std::size_t mosaic_pages = 0;
        for (const auto& pages : full.mosaic_pages)
            mosaic_pages += pages.size();
        for (const auto& pages : full.greyed_mosaic_pages)
            mosaic_pages += pages.size();
        if (full.greyed_pages.size() != full.pages.size() ||
            pages_alive != full.pages.size() + full.greyed_pages.size() + mosaic_pages)
            fail(
                "the terrain pages alive are not the loading screen's, their greyed pages and "
                "the mosaics' pages, colour and greyed"
            );
        for (const auto& page : full.atlas.pages)
            if (!page.texels.empty())
                fail("the atlas kept a page's texels after the page was filled");
        std::cout << "render tiers check: the loading screen made the full tier's terrain pages: "
                  << full.pages.size() << " pages of " << full.atlas.slot_tiles.size()
                  << " slots within " << full.atlas_page_edge << ", "
                  << full.page_bytes / 1024 / 1024 << " MiB, built in "
                  << full.atlas_build_ns / 1000 << " us and filled in "
                  << full.page_upload_ns / 1000 << " us\n";
        const uint64_t frames_before = full.frames;
        std::ignore = presented();
        if (!full_->drawn || full_->frames != frames_before + 1)
            fail("the first match frame was not drawn by the card");
        if (!full_->pages_from_load || full_->executor.counts().pages_alive < pages_alive)
            fail("the first match frame lost a terrain page");
        std::cout << "render tiers check: the first match frame drew from the loading screen's "
                     "terrain pages\n";
        // The Basic cases run at basic.
        switch_tier(true);
    }
    uint16_t anchor = 0;
    for (const auto& slot : match_->world().slots)
        if (slot.unit_index != 0 && slot.unit != nullptr && slot.record.type_index != 0 &&
            oa::world_unit_at(&match_->state(), slot.unit_index)->owner_index ==
                match_local_player_) {
            anchor = slot.unit_index;
            break;
        }
    if (anchor == 0)
        fail("found no local unit");
    const auto at_zoom = [&](float zoom) {
        match_zoom_ = match_zoom_target_ = zoom;
        center_camera_on_unit(anchor);
    };
    const auto cursor = [&]() {
        return Area{
            static_cast<int>(match_pointer_x_) - cursor_reach,
            static_cast<int>(match_pointer_y_) - cursor_reach,
            2 * cursor_reach,
            2 * cursor_reach
        };
    };
    const auto battlefield = [&]() {
        return Area{
            match_layout_.left,
            match_layout_.top,
            match_layout_.battlefield_width(),
            match_layout_.battlefield_height()
        };
    };
    const auto composed = [&]() {
        renderer::Surface frame;
        compose_match_frame(frame);
        return frame;
    };
    // The HUD strips of a presented frame, held to the chrome's filter at
    // the display's scale as sharp_draw draws them from the HUD layer:
    // each strip against its own source scaled by the card's filter, a
    // pixel in from the strip's edges and the pointer's reach left out. At
    // a whole-number scale the strips are NEAREST and equal; at another,
    // where the rung filters the chrome through a prescale target, they
    // are sharp-bilinear from it, within the renderer's tolerance.
    const auto check_hud_strips = [&](const renderer::Surface& read, const std::string& which) {
        const double chrome = match_layout_.scale * match_display_density();
        const auto card = accelerated_card_scale(
            policy::chrome_filter(accelerated_.rung, chrome),
            chrome,
            match_hud_cpu_.width,
            match_hud_cpu_.height,
            accelerated_.hud_prescale
        );
        const bool whole = std::floor(chrome) == chrome;
        if (!whole && rung.filtered_chrome && rung.card != policy::CardFilter::pixelart &&
            card.filter != policy::ScaleFilter::sharp_bilinear)
            fail(which + ": the HUD strips are not drawn sharp-bilinear from the prescale target");
        const auto hud_rgb = gamma_of(match_hud_cpu_.rgb);
        const Area pointer = cursor();
        Difference difference;
        double sum = 0.0;
        std::size_t channels = 0;
        renderer::Surface reference = read;
        for (const auto& strip : match_hud_strips()) {
            if (strip.source_w <= 0 || strip.source_h <= 0 || strip.w <= 0 || strip.h <= 0)
                continue;
            const wr::RgbSource source{
                hud_rgb.data() + (static_cast<std::size_t>(strip.source_y) * match_hud_cpu_.width +
                                  static_cast<std::size_t>(strip.source_x)) *
                                     3U,
                static_cast<uint32_t>(strip.source_w),
                static_cast<uint32_t>(strip.source_h),
                match_hud_cpu_.width
            };
            // The card's references write every pixel of their target, so
            // each strip is drawn and compared before the next.
            scaled_reference(
                source,
                card,
                {strip.x, strip.y, strip.w, strip.h},
                {reference.rgb.data(), reference.width, reference.height, reference.width}
            );
            const int right =
                std::min(strip.x + strip.w - strip_edge_inset, static_cast<int>(read.width));
            const int bottom =
                std::min(strip.y + strip.h - strip_edge_inset, static_cast<int>(read.height));
            for (int y = strip.y + strip_edge_inset; y < bottom; ++y)
                for (int x = strip.x + strip_edge_inset; x < right; ++x) {
                    if (x >= pointer.x && x < pointer.x + pointer.w && y >= pointer.y &&
                        y < pointer.y + pointer.h)
                        continue;
                    ++difference.pixels;
                    const auto at = (static_cast<std::size_t>(y) * read.width + x) * 3U;
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        const int delta = std::abs(
                            int{read.rgb[at + channel]} - int{reference.rgb[at + channel]}
                        );
                        difference.most = std::max(difference.most, delta);
                        sum += delta;
                        ++channels;
                    }
                }
        }
        difference.mean = channels != 0 ? sum / static_cast<double>(channels) : 0.0;
        std::cout << "render tiers check: " << which << ": the HUD strips at chrome scale "
                  << chrome << " by filter " << static_cast<int>(card.filter) << " x" << card.factor
                  << " over " << difference.pixels << " pixels: most " << difference.most
                  << ", mean " << difference.mean << '\n';
        if (difference.pixels == 0 || difference.most > (whole ? 0 : most_scaled_difference) ||
            difference.mean > most_mean_scaled_difference) {
            write_png(report_directory / "native-render-tiers-hud-presented.png", read);
            write_png(report_directory / "native-render-tiers-hud-reference.png", reference);
            fail(which + ": the HUD strips stray from the chrome's filter");
        }
    };

    // --native-density, or the setting: the window opened at the display's
    // own density, and this case alone runs, since every other compares the
    // read-back with the processor's composition pixel for pixel.
    if (options_.native_density || engine_settings().native_density) {
        if (!native_density_window())
            fail("native density was asked for, but the window did not open at it");
        // The match is laid out in window points.
        int points_w = 0;
        int points_h = 0;
        int pixels_w = 0;
        int pixels_h = 0;
        if (!SDL_GetWindowSize(sdl_.window, &points_w, &points_h) ||
            !SDL_GetWindowSizeInPixels(sdl_.window, &pixels_w, &pixels_h))
            fail(std::string("SDL window size: ") + SDL_GetError());
        const auto in_points = oa::ui::display_layout::make_match_layout(points_w, points_h);
        if (match_layout_.width != in_points.width || match_layout_.height != in_points.height ||
            match_layout_.left != in_points.left || match_layout_.top != in_points.top ||
            match_layout_.bottom != in_points.bottom || match_layout_.scale != in_points.scale)
            fail("the match is not laid out in window points");
        const double density = match_display_density();
        // The read-back has the display's size.
        update_pointer(
            static_cast<float>(match_layout_.width - 1),
            static_cast<float>(match_layout_.height - 1)
        );
        at_zoom(1.0F);
        const auto read = presented();
        if (!accelerated_presentation())
            fail("the accelerated presentation stopped at native density");
        if (accelerated_.frame.method != SceneMethod::none)
            fail("zoom 1 was split from the world layer at native density");
        if (read.width != static_cast<uint32_t>(pixels_w) ||
            read.height != static_cast<uint32_t>(pixels_h))
            fail("the read-back at native density is not the display's size");
        std::cout << "render tiers check: native density " << density << ", " << points_w << 'x'
                  << points_h << " points on " << pixels_w << 'x' << pixels_h << " pixels\n";
        // At zoom 1 and a whole-number density the battlefield is the
        // processor's enlarged by nearest replication, and the chrome too
        // where its own scale is a whole number.
        if (std::floor(density) == density && density >= 1.0) {
            const auto factor = static_cast<int>(density);
            const auto on_display = [&](const Area& area) {
                return Area{area.x * factor, area.y * factor, area.w * factor, area.h * factor};
            };
            const auto expected = enlarged(composed(), static_cast<uint32_t>(factor));
            const auto world =
                compare(read, expected, on_display(battlefield()), on_display(cursor()));
            const bool chrome_whole = std::floor(match_layout_.scale) == match_layout_.scale;
            const auto whole = compare(
                read,
                expected,
                on_display({0, 0, match_layout_.width, match_layout_.height}),
                on_display(cursor())
            );
            std::cout << "render tiers check: zoom 1 at native density: battlefield most "
                      << world.most << ", whole frame most " << whole.most << '\n';
            if (world.pixels == 0 || world.most != 0 ||
                (chrome_whole && whole.most > most_hud_difference)) {
                write_png(report_directory / "native-render-tiers-density-presented.png", read);
                write_png(report_directory / "native-render-tiers-density-composed.png", expected);
                fail(
                    "zoom 1 at native density is not the composition enlarged by nearest "
                    "replication"
                );
            }
        } else {
            std::cout << "render tiers check: the density is not a whole number; the frame is "
                         "drawn by the chrome's filter\n";
        }
        // The unit under the pointer is the one drawn there: its place in
        // layout pixels is its place in window points, and SDL maps the
        // pointer back through the view.
        const auto& slots = match_->world().slots;
        const auto point = project_match_point(
            live_viewport(match_camera_x_, match_camera_z_), slots[anchor].unit->position
        );
        float window_x = 0.0F;
        float window_y = 0.0F;
        if (!frame_to_window(
                sdl_.renderer,
                static_cast<float>(point.x),
                static_cast<float>(point.y),
                &window_x,
                &window_y
            ))
            fail(std::string("SDL_RenderCoordinatesToWindow: ") + SDL_GetError());
        if (std::abs(window_x - static_cast<float>(point.x)) > window_point_tolerance ||
            std::abs(window_y - static_cast<float>(point.y)) > window_point_tolerance)
            fail("a layout pixel is not a window point at native density");
        SDL_Event motion{};
        motion.type = SDL_EVENT_MOUSE_MOTION;
        motion.motion.windowID = SDL_GetWindowID(sdl_.window);
        motion.motion.x = window_x;
        motion.motion.y = window_y;
        bool running = true;
        dispatch_event(motion, running);
        if (hovered_match_unit_ != anchor)
            fail("the unit under the pointer at native density is not the one drawn there");
        std::cout << "render tiers check: at native density the match is laid out in window "
                     "points, read back at the display's size, and picks the unit drawn under "
                     "the pointer\n";
        // The Full tier at zoom 1 and a whole-number density: the card's
        // terrain and the overlay, stretched by logical presentation, equal
        // the composition enlarged by nearest replication too.
        if (asked_level == HardwareAcceleration::full && std::floor(density) == density &&
            density >= 1.0) {
            update_pointer(
                static_cast<float>(match_layout_.width - 1),
                static_cast<float>(match_layout_.height - 1)
            );
            set_level(HardwareAcceleration::full);
            at_zoom(1.0F);
            const auto full_read = presented();
            if (!full_presentation() || !full_ || !full_->drawn)
                fail("the full tier did not draw at native density");
            const auto factor = static_cast<int>(density);
            // Beside the card's own draws (card_draw_mask), as the Full case
            // at zoom 1 holds the frame; the world layer is the overlay
            // canvas, so the standard picture is drawn for the composition.
            const auto points_wide = full_read.width / static_cast<uint32_t>(factor);
            const auto points_high = full_read.height / static_cast<uint32_t>(factor);
            const std::vector<uint8_t> mask = enlarged_mask(
                card_draw_mask(
                    match_models().draws,
                    full_->fog.grid,
                    full_->world_quads,
                    battlefield(),
                    1.0F,
                    points_wide,
                    points_high
                ),
                points_wide,
                points_high,
                static_cast<uint32_t>(factor)
            );
            ensure_screen_world();
            const auto expected = enlarged(composed(), static_cast<uint32_t>(factor));
            const Area field{
                battlefield().x * factor,
                battlefield().y * factor,
                battlefield().w * factor,
                battlefield().h * factor
            };
            const Area pointer{
                cursor().x * factor, cursor().y * factor, cursor().w * factor, cursor().h * factor
            };
            const auto world = compare_masked(full_read, expected, field, pointer, mask);
            std::cout << "render tiers check: full tier at zoom 1 and native density: "
                      << world.beside.pixels << " battlefield pixels beside the card's own: most "
                      << world.beside.most << "; under them most " << world.under.most << '\n';
            if (world.beside.pixels < least_compared_pixels || world.beside.most != 0) {
                write_png(report_directory / "native-render-tiers-density-full.png", full_read);
                write_png(
                    report_directory / "native-render-tiers-density-full-composed.png", expected
                );
                fail(
                    "the full tier at zoom 1 and native density is not the composition "
                    "enlarged by nearest replication beside the card's own draws"
                );
            }
            set_level(HardwareAcceleration::off);
        }
        return 0;
    }

    switch_tier(false);
    resize_whole();
    // The cursor waits in the blank corner right of the bottom bar.
    update_pointer(
        static_cast<float>(match_layout_.width - 1), static_cast<float>(match_layout_.height - 1)
    );
    // A fight beside the commander, played until lasers fire, where the
    // game's data has its units; otherwise the match's own units, played as
    // long.
    const bool fight = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMPW") != 0 &&
                       oa::sim::unit_spawn::find_type_index(spawn_type_names_, "CORAK") != 0;
    if (fight)
        spawn_combat_armies(fight_units_per_side);
    else
        std::cout
            << "render tiers check: the data has no fight's units; the match's own are drawn\n";
    for (int tick = 0; tick < fight_ticks; ++tick)
        step_match_simulation();

    // The pictures for the maintainer: one moment at each zoom in both tiers.
    for (const float zoom : picture_zooms)
        for (const bool on : {false, true}) {
            switch_tier(on);
            at_zoom(zoom);
            const auto read = presented();
            write_png(
                report_directory /
                    ("native-render-tiers-" + std::string(on ? "accelerated" : "standard") +
                     "-zoom-" + zoom_text(zoom) + ".png"),
                read
            );
        }

    // At zoom 0.5 the area pass of the terrain drawn at one pixel per map
    // pixel is, byte for byte, the terrain the standard tier's box filter
    // averages.
    {
        switch_tier(false);
        at_zoom(kMinBattlefieldZoom);
        std::ignore = presented();
        const auto boxed = match_terrain_cache_;
        switch_tier(true);
        std::ignore = presented();
        const auto& scene = match_terrain_cache_;
        if (accelerated_.frame.method != SceneMethod::area || accelerated_.frame.draw_scale != 1.0F)
            fail("zoom 0.5 was not drawn at one pixel per map pixel");
        wr::AreaPlan plan;
        std::vector<uint8_t> averaged(boxed.rgb.size());
        if (wr::plan_area_filter(plan, wr::area_scale_min, boxed.width, boxed.height) !=
                wr::AreaError::none ||
            wr::area_filter_rgb24(
                plan,
                {scene.rgb.data(), scene.width, scene.height, scene.width},
                {averaged.data(), boxed.width, boxed.height, boxed.width}
            ) != wr::AreaError::none)
            fail("the area pass refused the terrain at zoom 0.5");
        if (averaged != boxed.rgb)
            fail("the area pass of the terrain at zoom 0.5 differs from the box filter's");
        std::cout << "render tiers check: at zoom 0.5 the area pass of the terrain equals the "
                     "box filter's, "
                  << boxed.width << 'x' << boxed.height << '\n';
    }
    switch_tier(true);
    // A shot of render type 2 whose lens's centre lies off the battlefield
    // draws no lens, and the projectiles after it in the pool are drawn as
    // without it: a laser's shot behind a lens in the view, and behind one
    // just past the view's edge, lists the lines it lists alone, and only
    // the lens in the view is listed. Each pool holds only the case's shots,
    // at the local unit's place, which is in its player's sight; the
    // match's own pool is put back after.
    {
        // The first weapon of each kind, past record zero, the fallback; a
        // laser is one with a colour.
        std::optional<uint8_t> lens_weapon;
        std::optional<uint8_t> laser_weapon;
        for (std::size_t index = 1; index < oa::sim::combat_state::weapon_registry_capacity;
             ++index) {
            const auto weapon = static_cast<uint8_t>(index);
            if (weapon_registry_.name(weapon).empty())
                continue;
            const auto& definition = weapon_registry_.definition(weapon);
            if (definition.rendertype == lens_render_type && !lens_weapon)
                lens_weapon = weapon;
            else if (
                definition.rendertype == laser_render_type && definition.color != 0 && !laser_weapon
            )
                laser_weapon = weapon;
        }
        if (!lens_weapon || !laser_weapon) {
            std::cout << "render tiers check: the data has no weapon of render type 2 or no "
                         "laser with a colour; the projectile pass past a lens is not checked\n";
        } else {
            oa::World& world = match_->state();
            // The match's count and every record a case writes over.
            const int32_t match_count = world.game.projectile_count;
            const std::vector<oa::Projectile> match_shots(
                world.projectiles,
                world.projectiles + std::max<std::ptrdiff_t>(lens_check_shots, match_count)
            );
            const oa::FixedVec3 place = oa::world_unit_at(&world, anchor)->position;
            const auto shot_of = [&](uint8_t weapon) {
                oa::Projectile shot{};
                shot.def = static_cast<oa_ref32>(weapon) + 1U;
                shot.position = place;
                shot.origin = place;
                shot.origin.x += lens_check_laser_length;
                return shot;
            };

            struct Listed {
                std::size_t lenses{};
                std::vector<LineDraw> lines;
            };

            // Draws a frame with only the shots in the pool and returns what
            // its list holds.
            const auto listed_with = [&](std::initializer_list<oa::Projectile> shots) {
                std::copy(shots.begin(), shots.end(), world.projectiles);
                world.game.projectile_count = static_cast<int32_t>(shots.size());
                std::ignore = presented();
                const WorldDrawList& list = match_models().draws;
                return Listed{list.lenses.size(), list.lines};
            };
            const auto same_lines = [](const Listed& one, const Listed& other) {
                return std::equal(
                    one.lines.begin(),
                    one.lines.end(),
                    other.lines.begin(),
                    other.lines.end(),
                    [](const LineDraw& a, const LineDraw& b) {
                        return a.x0 == b.x0 && a.y0 == b.y0 && a.x1 == b.x1 && a.y1 == b.y1 &&
                               a.color == b.color;
                    }
                );
            };
            // Where the frame just drawn puts the unit's place: on the
            // battlefield rectangle or off it, as the pass tests a lens.
            const auto place_on_battlefield = [&]() {
                const oa::sim::effect_particles::ExplosionView view{
                    match_camera_x_,
                    match_camera_z_,
                    {oa::sim::effect_particles::battlefield_screen_x,
                     oa::sim::effect_particles::battlefield_screen_y,
                     oa::sim::effect_particles::battlefield_screen_x + visible_map_width() - 1,
                     oa::sim::effect_particles::battlefield_screen_y + visible_map_height() - 1}
                };
                return projectile_lens_on_battlefield(
                    view, oa::sim::match_runtime::fixed_words(place)
                );
            };
            if (!match_->point_shown(
                    match_view_player(), oa::sim::match_runtime::fixed_words(place)
                ))
                fail("the local unit's place is out of its player's sight");
            at_zoom(1.0F);
            const Listed laser_in_view = listed_with({shot_of(*laser_weapon)});
            const Listed lens_in_view =
                listed_with({shot_of(*lens_weapon), shot_of(*laser_weapon)});
            if (!place_on_battlefield())
                fail("the view centred on the local unit leaves its place off the battlefield");
            // The view moved so that the place lies just past one of its
            // edges, the first the map leaves room for.
            const int32_t place_x = static_cast<int32_t>(static_cast<uint32_t>(place.x) >> 16);
            const int32_t place_z = static_cast<int32_t>(static_cast<uint32_t>(place.z) >> 16);
            const int32_t across = visible_map_width();
            const int32_t down = visible_map_height();
            bool moved = false;
            for (const auto& [camera_x, camera_z] :
                 {std::pair{place_x + lens_check_margin, place_z - down / 2},
                  std::pair{place_x - across - lens_check_margin, place_z - down / 2},
                  std::pair{place_x - across / 2, place_z + lens_check_margin},
                  std::pair{place_x - across / 2, place_z - down - lens_check_margin}}) {
                set_camera_position(camera_x, camera_z, 0);
                std::ignore = presented();
                if (!place_on_battlefield()) {
                    moved = true;
                    break;
                }
            }
            if (!moved)
                fail("the map leaves no room to put the local unit's place past the view's edge");
            const Listed laser_past_view = listed_with({shot_of(*laser_weapon)});
            const Listed lens_past_view =
                listed_with({shot_of(*lens_weapon), shot_of(*laser_weapon)});
            std::copy(match_shots.begin(), match_shots.end(), world.projectiles);
            world.game.projectile_count = match_count;
            at_zoom(1.0F);
            std::ignore = presented();
            if (laser_in_view.lines.empty() || laser_past_view.lines.empty())
                fail("a laser's shot listed no line");
            if (laser_in_view.lenses != 0 || laser_past_view.lenses != 0)
                fail("a laser's shot listed a lens");
            if (lens_in_view.lenses != 1)
                fail("a lens in the view was not listed");
            if (!same_lines(lens_in_view, laser_in_view))
                fail("a laser's shot after a lens in the view lists other lines than alone");
            if (lens_past_view.lenses != 0)
                fail("a lens whose centre lies off the battlefield was listed");
            if (!same_lines(lens_past_view, laser_past_view))
                fail(
                    "a laser's shot after a lens off the battlefield lists other lines than "
                    "alone"
                );
            std::cout << "render tiers check: a lens off the battlefield draws nothing and the "
                         "shots after it are drawn; weapons "
                      << weapon_registry_.name(*lens_weapon) << " and "
                      << weapon_registry_.name(*laser_weapon) << '\n';
        }
    }
    // Zoom 1 at a whole-number chrome scale: the frame the processor composes.
    at_zoom(1.0F);
    {
        const auto read = presented();
        const auto expected = composed();
        if (accelerated_.frame.method != SceneMethod::none)
            fail("zoom 1 was split from the world layer");
        const auto world = compare(read, expected, battlefield(), cursor());
        const auto whole =
            compare(read, expected, {0, 0, match_layout_.width, match_layout_.height}, cursor());
        if (world.most != 0 || whole.most > most_hud_difference) {
            write_png(report_directory / "native-render-tiers-zoom-1-presented.png", read);
            write_png(report_directory / "native-render-tiers-zoom-1-composed.png", expected);
            fail("zoom 1 differs from compose_match_frame");
        }
    }
    // The Full tier's model stage over the same frame: the card's frame of
    // the list's models against the processor's raster of them.
    check_full_models(report_directory);
    // Zoomed out: the area pass's picture, uploaded as the processor composed it.
    for (const float zoom : area_zooms) {
        at_zoom(zoom);
        const auto read = presented();
        const auto expected = composed();
        if (accelerated_.frame.method != SceneMethod::area)
            fail("zoom " + zoom_text(zoom) + " was not reduced by the area pass");
        const auto difference = compare(read, expected, battlefield(), cursor());
        std::cout << "render tiers check: zoom " << zoom_text(zoom)
                  << " by the area pass at draw scale " << accelerated_.frame.draw_scale
                  << ": most " << difference.most << '\n';
        if (difference.most != 0) {
            write_png(
                report_directory /
                    ("native-render-tiers-zoom-" + zoom_text(zoom) + "-presented.png"),
                read
            );
            write_png(
                report_directory /
                    ("native-render-tiers-zoom-" + zoom_text(zoom) + "-composed.png"),
                expected
            );
            fail("zoom " + zoom_text(zoom) + " differs from the area pass's composition");
        }
    }
    // Zoomed in: the scene by the card's filter, then the overlay, against
    // the references applied to the same scene. A frame read back is held
    // to them as the frame the card magnified.
    const auto check_magnified = [&](float zoom,
                                     const renderer::Surface& read,
                                     const std::string& which) {
        if (accelerated_.frame.method != SceneMethod::magnify || !accelerated_.magnified)
            fail(which + " at zoom " + zoom_text(zoom) + " was not magnified");
        const uint32_t bf_w = match_world_cpu_.width;
        const uint32_t bf_h = match_world_cpu_.height;
        const uint32_t scene_w = match_scene_cpu_.width;
        const uint32_t scene_h = match_scene_cpu_.height;
        const auto corner = [&](uint32_t battlefield_extent, uint32_t scene_extent) {
            return std::min(
                static_cast<uint32_t>(
                    std::ceil(static_cast<double>(battlefield_extent) / static_cast<double>(zoom))
                ),
                scene_extent
            );
        };
        const uint32_t width = corner(bf_w, scene_w);
        const uint32_t height = corner(bf_h, scene_h);
        const auto card = accelerated_card_scale(
            policy::world_filter(accelerated_.rung, zoom),
            zoom,
            std::min(width + 1, scene_w),
            std::min(height + 1, scene_h),
            accelerated_.world_prescale
        );
        const auto scene_rgb = gamma_of(match_scene_cpu_.rgb);
        const wr::RgbSource source{scene_rgb.data(), width, height, scene_w};
        renderer::Surface reference = read;
        renderer::Surface scaled = reference;
        scaled_reference(
            source,
            card,
            {match_layout_.left,
             match_layout_.top,
             static_cast<int>(std::lround(static_cast<double>(width) * zoom)),
             static_cast<int>(std::lround(static_cast<double>(height) * zoom))},
            {scaled.rgb.data(), scaled.width, scaled.height, scaled.width}
        );
        // The battlefield of the reference is the scaled scene, then the
        // overlay of what the painters changed.
        std::vector<uint8_t> overlay(std::size_t{bf_w} * bf_h * 4U);
        std::vector<uint8_t> bands(platform::job_pool::bands_of_rows(bf_h, xrgb_band_rows));
        convert_rgb24_overlay_argb(
            match_world_cpu_.rgb.data(),
            accelerated_.base.data(),
            bf_w,
            bf_h,
            overlay.data(),
            std::size_t{bf_w} * 4U,
            gamma_identity_ ? nullptr : &gamma_table_,
            bands,
            nullptr
        );
        renderer::Surface field{bf_w, bf_h, std::vector<uint8_t>(std::size_t{bf_w} * bf_h * 3U)};
        for (uint32_t y = 0; y < bf_h; ++y)
            std::copy_n(
                scaled.rgb.begin() +
                    static_cast<std::ptrdiff_t>(
                        ((static_cast<std::size_t>(y) + match_layout_.top) * scaled.width +
                         match_layout_.left) *
                        3U
                    ),
                bf_w * 3U,
                field.rgb.begin() + static_cast<std::ptrdiff_t>(std::size_t{y} * bf_w * 3U)
            );
        wr::overlay_rgb24(
            {field.rgb.data(), bf_w, bf_h, bf_w},
            reinterpret_cast<const uint32_t*>(overlay.data()),
            bf_w
        );
        for (uint32_t y = 0; y < bf_h; ++y)
            std::copy_n(
                field.rgb.begin() + static_cast<std::ptrdiff_t>(std::size_t{y} * bf_w * 3U),
                bf_w * 3U,
                reference.rgb.begin() +
                    static_cast<std::ptrdiff_t>(
                        ((static_cast<std::size_t>(y) + match_layout_.top) * reference.width +
                         match_layout_.left) *
                        3U
                    )
            );
        // The last column and row may read past the corner, which renderers
        // clamp differently.
        auto inner = battlefield();
        inner.w -= 1;
        inner.h -= 1;
        const auto difference = compare(read, reference, inner, cursor());
        std::cout << "render tiers check: " << which << " at zoom " << zoom_text(zoom)
                  << " by filter " << static_cast<int>(card.filter) << " x" << card.factor
                  << ": most " << difference.most << ", mean " << difference.mean << '\n';
        const bool whole = std::floor(zoom) == zoom;
        if (difference.most > (whole ? 0 : most_scaled_difference) ||
            difference.mean > most_mean_scaled_difference) {
            write_png(
                report_directory /
                    ("native-render-tiers-zoom-" + zoom_text(zoom) + "-presented.png"),
                read
            );
            write_png(
                report_directory /
                    ("native-render-tiers-zoom-" + zoom_text(zoom) + "-reference.png"),
                reference
            );
            fail(which + " at zoom " + zoom_text(zoom) + " strays from the card's reference");
        }
    };
    for (const float zoom : magnified_zooms) {
        at_zoom(zoom);
        const auto read = presented();
        check_magnified(zoom, read, "a magnified frame");
        // The capture has the window's size and is the presented picture.
        if (zoom == capture_zoom) {
            int window_w = 0;
            int window_h = 0;
            SDL_GetWindowSizeInPixels(sdl_.window, &window_w, &window_h);
            if (read.width != static_cast<uint32_t>(window_w) ||
                read.height != static_cast<uint32_t>(window_h))
                fail("the capture at zoom 2 is not the window's size");
        }
    }
    // The very first frame after the tier is switched on, and the first
    // after each change of the window's size, is magnified as every other.
    for (const float zoom : first_frame_zooms) {
        switch_tier(false);
        at_zoom(zoom);
        std::ignore = presented();
        switch_tier(true);
        check_magnified(zoom, presented(), "the first frame after the tier was switched on");
    }
    for (const float zoom : first_frame_zooms)
        for (const bool whole : {false, true}) {
            if (whole)
                resize_whole();
            else
                resize(part_scale_width, part_scale_height);
            update_pointer(
                static_cast<float>(match_layout_.width - 1),
                static_cast<float>(match_layout_.height - 1)
            );
            at_zoom(zoom);
            check_magnified(
                zoom,
                presented(),
                "the first frame at " + std::to_string(match_layout_.width) + 'x' +
                    std::to_string(match_layout_.height)
            );
        }
    // The standard tier's picture of the same moment, which screenshots,
    // film frames and the backdrops keep: drawn again with the tier on, it
    // is the standard tier's battlefield, and it changes neither the
    // frame's counts of units drawn nor the HUD's resource readout, which
    // saves keep.
    for (const float zoom : kept_picture_zooms) {
        switch_tier(false);
        at_zoom(zoom);
        std::ignore = presented();
        const auto standard = composed();
        switch_tier(true);
        std::ignore = presented();
        if (accelerated_.frame.method == SceneMethod::none)
            fail("zoom " + zoom_text(zoom) + " was drawn as the standard tier draws it");
        const auto draws = frame_draws_;
        const auto readout = match_->state().game.resource_readout;
        ensure_screen_world();
        const auto kept = composed();
        const auto difference = compare(kept, standard, battlefield(), cursor());
        if (difference.pixels == 0 || difference.most != 0) {
            write_png(
                report_directory / ("native-render-tiers-kept-zoom-" + zoom_text(zoom) + ".png"),
                kept
            );
            write_png(
                report_directory /
                    ("native-render-tiers-standard-kept-zoom-" + zoom_text(zoom) + ".png"),
                standard
            );
            fail(
                "the picture kept at zoom " + zoom_text(zoom) +
                " is not the standard tier's battlefield"
            );
        }
        if (frame_draws_.units_drawn != draws.units_drawn ||
            frame_draws_.units_between_ticks != draws.units_between_ticks ||
            frame_draws_.probe_unit != draws.probe_unit ||
            frame_draws_.probe_drawn != draws.probe_drawn ||
            frame_draws_.probe_x != draws.probe_x || frame_draws_.probe_z != draws.probe_z)
            fail("drawing the kept picture changed the frame's counts of units drawn");
        const auto& after = match_->state().game.resource_readout;
        if (std::memcmp(&after, &readout, sizeof readout) != 0)
            fail("drawing the kept picture eased the HUD's resource readout");
        std::cout << "render tiers check: the picture kept at zoom " << zoom_text(zoom)
                  << " is the standard tier's\n";
    }
    // History: a whole-tick frame drawn after frames between ticks shows the
    // battlefield the one drawn before them showed.
    {
        at_zoom(history_zoom);
        set_presentation_alpha(1.0F);
        const auto before = presented();
        for (const float fraction : between_ticks) {
            set_presentation_alpha(fraction);
            std::ignore = presented();
        }
        set_presentation_alpha(1.0F);
        const auto after = presented();
        // The battlefield: the HUD's resource readout eases toward the
        // stores at every draw, whatever the tier.
        if (compare(before, after, battlefield(), cursor()).most != 0) {
            write_png(report_directory / "native-render-tiers-history-before.png", before);
            write_png(report_directory / "native-render-tiers-history-after.png", after);
            fail("a whole-tick frame's battlefield differs after frames between ticks");
        }
    }
    // Needless work: at a whole-number chrome scale the HUD has no prescale
    // target, and a zoom ease makes and destroys nothing.
    {
        at_zoom(kMinBattlefieldZoom);
        std::ignore = presented();
        if (accelerated_.hud_prescale.made())
            fail("the HUD has a prescale target at a whole-number chrome scale");
        const uint64_t made = counts.textures_created;
        const uint64_t destroyed = counts.textures_destroyed;
        std::vector<float> ease;
        for (float zoom = kMinBattlefieldZoom; zoom < kMaxBattlefieldZoom; zoom *= ease_step)
            ease.push_back(zoom);
        ease.push_back(kMaxBattlefieldZoom);
        const std::vector<float> back(ease.rbegin(), ease.rend());
        ease.insert(ease.end(), back.begin(), back.end());
        for (const float zoom : ease) {
            match_zoom_ = match_zoom_target_ = zoom;
            std::ignore = presented();
        }
        if (counts.textures_created != made || counts.textures_destroyed != destroyed)
            fail(
                "a zoom ease made " + std::to_string(counts.textures_created - made) +
                " and destroyed " + std::to_string(counts.textures_destroyed - destroyed) +
                " textures"
            );
        at_zoom(capture_zoom);
        const uint64_t draws = counts.prescale_draws;
        std::ignore = presented();
        if (counts.prescale_draws != draws)
            fail("zoom 2 drew a prescale target");
        std::cout << "render tiers check: a zoom ease of " << ease.size()
                  << " frames made and destroyed no texture\n";
    }
    // Smooth panning: the view drawn between map pixels as a slow scroll
    // moves it, at zoom 2.5 and 0.5, and the pointer picking what is drawn.
    // It runs after the zoom ease: it switches the tier off and on, which
    // drops the prescale targets the frames before made, and the ease
    // counts on finding them made.
    {
        // Raw terrain references elsewhere keep optional effects off. Panning
        // must also move illuminated and distorted scene pixels with the view.
        if (!engine_settings_)
            fail("smooth panning needs the current graphics settings");
        auto& settings = engine_settings_->current;

        struct RestoreEffects {
            oa::ui::engine_settings::EngineSettings& settings;
            std::array<uint32_t, 4> saved;

            ~RestoreEffects() {
                settings.weapon_lighting = saved[0];
                settings.nanolathe_lighting = saved[1];
                settings.wreck_glow = saved[2];
                settings.heat_distortion = saved[3];
            }
        } restore{
            settings,
            {settings.weapon_lighting,
             settings.nanolathe_lighting,
             settings.wreck_glow,
             settings.heat_distortion}
        };

        settings.weapon_lighting = 100;
        settings.nanolathe_lighting = 100;
        settings.wreck_glow = 100;
        settings.heat_distortion = 100;
        check_smooth_panning(switch_tier, at_zoom, anchor);
    }
    // The overlays a profile's visual rules paint, where it turns them on,
    // in the standard and accelerated tiers; the Full tier's after its own
    // cases.
    {
        constexpr std::array<HardwareAcceleration, 2> standard_and_basic{
            HardwareAcceleration::off, HardwareAcceleration::basic
        };
        check_visual_rule_overlays(set_level, standard_and_basic, at_zoom, anchor);
        switch_tier(true);
    }
    // At a chrome scale of 1.6 the HUD strips are the chrome's filter from
    // the HUD's prescale target, which is drawn into once on each frame
    // that paints the layer, which every frame the loop presents does, and
    // on none presented again without a paint.
    {
        resize(part_scale_width, part_scale_height);
        update_pointer(
            static_cast<float>(match_layout_.width - 1),
            static_cast<float>(match_layout_.height - 1)
        );
        at_zoom(1.0F);
        const auto hud_read = presented();
        if (!accelerated_.hud_prescale.made() && rung.card != policy::CardFilter::pixelart)
            fail("the HUD has no prescale target at a chrome scale of 1.6");
        check_hud_strips(hud_read, "the basic tier at a chrome scale of 1.6");
        const uint64_t draws = counts.prescale_draws;
        for (int frame = 0; frame < counted_hud_frames; ++frame)
            std::ignore = presented();
        const uint64_t expected =
            rung.card == policy::CardFilter::pixelart ? draws : draws + counted_hud_frames;
        if (counts.prescale_draws != expected)
            fail(
                "the HUD was drawn into its prescale target " +
                std::to_string(counts.prescale_draws - draws) + " times over " +
                std::to_string(counted_hud_frames) + " frames"
            );
        const uint64_t painted = counts.prescale_draws;
        present_match_layers();
        if (counts.prescale_draws != painted)
            fail("the HUD was drawn into its prescale target with its layer unchanged");
    }
    // Switched off, every frame is the standard tier's again.
    switch_tier(false);
    resize(whole_scale_width, whole_scale_height);
    update_pointer(
        static_cast<float>(match_layout_.width - 1), static_cast<float>(match_layout_.height - 1)
    );

    // The map the view shows ends before the mosaic: the game never shows a
    // map's last 32 columns and 128 rows of map pixels, where maps end in
    // filler tiles. A camera asked past the map's end is held where the
    // shown map's end reaches the battlefield's middle, and the terrain
    // fill's last column and row are the shown map's last, never the
    // filler's, with black past them; one asked before the map's start is
    // held where the map's start reaches the middle, with black before it.
    {
        namespace features = oa::sim::feature_runtime;
        at_zoom(1.0F);
        const auto [shown_w, shown_h] = shown_map_size();
        const auto mosaic_w = static_cast<int32_t>(selected_tnt_->tile_width * 32U);
        const auto mosaic_h = static_cast<int32_t>(selected_tnt_->tile_height * 32U);
        if (shown_w != mosaic_w - features::hidden_right_edge ||
            shown_h != mosaic_h - features::hidden_bottom_edge)
            fail("the shown map is not the mosaic less the edges the game never shows");
        // Where a map's edge at a battlefield's middle puts the camera.
        const auto edge_at_middle = [](double edge, int32_t battlefield) {
            return static_cast<int32_t>(std::floor(edge - static_cast<double>(battlefield) / 2.0));
        };
        const int32_t bf_w = match_layout_.battlefield_width();
        const int32_t bf_h = match_layout_.battlefield_height();
        set_camera_position(mosaic_w * 2, mosaic_h * 2, 0);
        std::ignore = presented();
        const auto camera = view_camera();
        if (camera[0] != edge_at_middle(shown_w, bf_w) ||
            camera[1] != edge_at_middle(shown_h, bf_h))
            fail("the camera asked past the map's end was not held with it at the middle");
        const auto& cache = match_terrain_cache_;
        if (cache.width == 0 || cache.height == 0 ||
            cache.rgb.size() < static_cast<std::size_t>(cache.width) * cache.height * 3U)
            fail("the standard tier's frame left no terrain fill");
        // The fill's last shown column and row against the map, through
        // the palette, and black past them where the fill reaches further.
        const auto map_rgb = [&](int32_t map_x, int32_t map_y) {
            const auto& map = *selected_tnt_;
            const std::size_t tile = map.tile_indices
                                         [static_cast<std::size_t>(map_y / 32) * map.tile_width +
                                          static_cast<std::size_t>(map_x / 32)];
            const uint8_t index = map.tile_palette_indices
                                      [tile * 1024U + static_cast<std::size_t>(map_y % 32) * 32U +
                                       static_cast<std::size_t>(map_x % 32)];
            return std::array<uint8_t, 3>{
                match_palette_[index * oa::palette_entry_bytes],
                match_palette_[index * oa::palette_entry_bytes + 1],
                match_palette_[index * oa::palette_entry_bytes + 2]
            };
        };
        const auto cache_rgb = [&](uint32_t x, uint32_t y) {
            const std::size_t at = (static_cast<std::size_t>(y) * cache.width + x) * 3U;
            return std::array<uint8_t, 3>{cache.rgb[at], cache.rgb[at + 1], cache.rgb[at + 2]};
        };
        const auto last_column = shown_w - 1 - camera[0];
        const auto last_row = shown_h - 1 - camera[1];
        if (last_column < 0 || last_column >= static_cast<int32_t>(cache.width) || last_row < 0 ||
            last_row >= static_cast<int32_t>(cache.height))
            fail("the terrain fill does not reach the shown map's last column and row");
        uint32_t wrong = 0;
        uint32_t beyond = 0;
        for (uint32_t y = 0; y < cache.height; ++y) {
            const auto map_y = camera[1] + static_cast<int32_t>(y);
            if (map_y < shown_h &&
                cache_rgb(static_cast<uint32_t>(last_column), y) != map_rgb(shown_w - 1, map_y))
                ++wrong;
            for (auto x = static_cast<uint32_t>(last_column) + 1; x < cache.width; ++x) {
                ++beyond;
                if (cache_rgb(x, y) != std::array<uint8_t, 3>{0, 0, 0})
                    ++wrong;
            }
        }
        for (uint32_t x = 0; x < cache.width; ++x) {
            const auto map_x = camera[0] + static_cast<int32_t>(x);
            if (map_x < shown_w &&
                cache_rgb(x, static_cast<uint32_t>(last_row)) != map_rgb(map_x, shown_h - 1))
                ++wrong;
            for (auto y = static_cast<uint32_t>(last_row) + 1; y < cache.height; ++y) {
                ++beyond;
                if (cache_rgb(x, y) != std::array<uint8_t, 3>{0, 0, 0})
                    ++wrong;
            }
        }
        if (wrong != 0)
            fail(
                "the terrain fill at the shown map's end differs from the map in " +
                std::to_string(wrong) + " pixels"
            );
        // Before the map's start, the map's first column and row at the
        // battlefield's middle, black before them.
        set_camera_position(-mosaic_w * 2, -mosaic_h * 2, 0);
        std::ignore = presented();
        const auto before = view_camera();
        if (before[0] != static_cast<int32_t>(std::ceil(-static_cast<double>(bf_w) / 2.0)) ||
            before[1] != static_cast<int32_t>(std::ceil(-static_cast<double>(bf_h) / 2.0)))
            fail("the camera asked before the map's start was not held with it at the middle");
        const auto first_column = static_cast<uint32_t>(-before[0]);
        const auto first_row = static_cast<uint32_t>(-before[1]);
        uint32_t black_before = 0;
        for (uint32_t y = 0; y < cache.height; ++y)
            for (uint32_t x = 0; x < cache.width; ++x) {
                if (x >= first_column && y >= first_row) {
                    if (x < first_column + 2U && y < first_row + 2U &&
                        cache_rgb(x, y) != map_rgb(
                                               static_cast<int32_t>(x - first_column),
                                               static_cast<int32_t>(y - first_row)
                                           ))
                        ++wrong;
                    continue;
                }
                ++black_before;
                if (cache_rgb(x, y) != std::array<uint8_t, 3>{0, 0, 0})
                    ++wrong;
            }
        if (wrong != 0)
            fail(
                "the terrain fill before the map's start differs from black and the map in " +
                std::to_string(wrong) + " pixels"
            );
        std::cout << "render tiers check: the view ends at the shown map, " << shown_w << 'x'
                  << shown_h << " of the " << mosaic_w << 'x' << mosaic_h
                  << " mosaic, with the camera at " << camera[0] << ", " << camera[1] << " and "
                  << beyond << " fill pixels black past it; before its start at " << before[0]
                  << ", " << before[1] << " with " << black_before << " black before it\n";
    }
    // The Full tier: the terrain drawn by the card from the atlas pages, the
    // rest by the processor over it.
    if (asked_level == HardwareAcceleration::full) {
        check_full_render_tier(
            set_level,
            at_zoom,
            presented,
            composed,
            resize,
            check_hud_strips,
            report_directory,
            software
        );
        // The overlays a profile's visual rules paint, in the Full tier.
        constexpr std::array<HardwareAcceleration, 1> full_only{HardwareAcceleration::full};
        check_visual_rule_overlays(set_level, full_only, at_zoom, anchor);
    } else
        std::cout << "render tiers check: the Full cases need --hardware-acceleration=full; "
                     "skipped\n";
    // Switched off, every frame is the standard tier's again, the whole
    // frame where the chrome's scale is a whole number.
    switch_tier(false);
    resize_whole();
    update_pointer(
        static_cast<float>(match_layout_.width - 1), static_cast<float>(match_layout_.height - 1)
    );
    for (const float zoom : {0.5F, 1.0F, 2.0F}) {
        at_zoom(zoom);
        const auto read = presented();
        if (compare(read, composed(), {0, 0, match_layout_.width, match_layout_.height}, cursor())
                .most != 0)
            fail(
                "a frame after the accelerated presentation was switched off differs at zoom " +
                zoom_text(zoom)
            );
    }
    std::cout << "render tiers check: the accelerated presentation draws within its references at "
                 "every zoom, with "
              << counts.textures_created << " textures made and " << counts.prescale_draws
              << " prescale draws; pictures in " << report_directory.string() << '\n';
    return 0;
}

void Runtime::check_full_render_tier(
    const std::function<void(oa::ui::engine_settings::HardwareAcceleration)>& set_level,
    const std::function<void(float)>& at_zoom,
    const std::function<renderer::Surface()>& presented,
    const std::function<renderer::Surface()>& composed,
    const std::function<void(int, int)>& resize,
    const std::function<void(const renderer::Surface&, const std::string&)>& check_hud_strips,
    const fs::path& report_directory,
    bool software
) {
    using oa::ui::engine_settings::HardwareAcceleration;
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("render tiers check: full tier: " + what);
    };
    const auto battlefield = [&]() {
        return Area{
            match_layout_.left,
            match_layout_.top,
            match_layout_.battlefield_width(),
            match_layout_.battlefield_height()
        };
    };
    const auto cursor = [&]() {
        return Area{
            static_cast<int>(match_pointer_x_) - cursor_reach,
            static_cast<int>(match_pointer_y_) - cursor_reach,
            2 * cursor_reach,
            2 * cursor_reach
        };
    };
    const auto picture = [&](const std::string& name) {
        return report_directory / ("native-render-tiers-full-" + name + ".png");
    };
    const auto gamma_of = [&](std::vector<uint8_t> bytes) {
        if (!gamma_identity_)
            for (auto& byte : bytes)
                byte = gamma_table_[byte];
        return bytes;
    };
    // A Full frame, which never runs the box filter, the bands or the fog's
    // raster, keeps the terrain pages and their greyed pages alive: the
    // frame's world layer is the overlay canvas.
    const auto full_frame = [&]() {
        const uint64_t runs = terrain_box_filter_runs_;
        auto frame = presented();
        if (terrain_box_filter_runs_ != runs)
            fail("the box filter ran for a full frame");
        if (!full_presentation() || !full_ || !full_->drawn || !full_frame_drawn())
            fail("the frame at zoom " + zoom_text(match_zoom()) + " was not drawn by the card");
        const auto& full = *full_;
        const uint64_t alive = full.executor.counts().pages_alive;
        if (full.pages.size() != full.atlas.pages.size() ||
            full.greyed_pages.size() != full.pages.size() ||
            alive < full.pages.size() + full.greyed_pages.size())
            fail("the terrain pages alive are not the atlas's pages and their greyed pages");
        return frame;
    };
    // The processor's picture of the same moment: the standard tier's
    // draw, as the readers that keep a picture get it.
    const auto standard = [&]() {
        ensure_screen_world();
        return composed();
    };
    // What the terrain cost the processor in the frame just presented.
    const auto cost = [&]() {
        const auto& full = *full_;
        return "terrain quads " + std::to_string(full.drawn_quads) + ", fog quads " +
               std::to_string(full.drawn_fog_quads) + ", " +
               std::to_string(full.sprites.sprites + full.sprites.squares + full.sprites.lines) +
               " sprite draws, " + std::to_string(full.drawn_batches) +
               " batches; processor cost: build " + std::to_string(full.build_ns / 1000) +
               " us, card " + std::to_string(full.execute_ns / 1000) + " us, overlay " +
               std::to_string(full.overlay_ns / 1000) + " us";
    };
    // Where the card's own draws lie on a frame (card_draw_mask).
    const auto card_mask = [&](float zoom, const renderer::Surface& read) {
        return card_draw_mask(
            match_models().draws,
            full_->fog.grid,
            full_->world_quads,
            battlefield(),
            zoom,
            read.width,
            read.height
        );
    };
    // Where an explosion's flash alone is the card's own on a frame: the
    // flashes' marks less every other draw's, where one flash reaches or,
    // with `stacked`, where more than one does.
    const auto flash_mask = [&](float zoom, const renderer::Surface& read, bool stacked) {
        auto marked = card_mask(zoom, read);
        const auto cover =
            flash_cover(match_models().draws, battlefield(), zoom, read.width, read.height);
        const auto others = card_draw_mask(
            match_models().draws,
            full_->fog.grid,
            full_->world_quads,
            battlefield(),
            zoom,
            read.width,
            read.height,
            false
        );
        for (std::size_t pixel = 0; pixel < marked.size(); ++pixel)
            if (others[pixel] != 0 || (cover[pixel] > 1) != stacked)
                marked[pixel] = 0;
        return marked;
    };
    // The picture of where two frames differ beside a mask: the first
    // dimmed, the mask in blue, each differing pixel beside it white.
    const auto differing_picture = [&](const renderer::Surface& read,
                                       const renderer::Surface& expected,
                                       const std::vector<uint8_t>& mask) {
        renderer::Surface differing = expected;
        for (std::size_t pixel = 0; pixel < mask.size(); ++pixel) {
            auto* shown = differing.rgb.data() + pixel * 3U;
            const auto* seen = read.rgb.data() + pixel * 3U;
            const bool differs = shown[0] != seen[0] || shown[1] != seen[1] || shown[2] != seen[2];
            if (mask[pixel] != 0) {
                shown[0] /= 4;
                shown[1] /= 4;
                shown[2] = static_cast<uint8_t>(shown[2] / 4 + 96);
            } else if (differs) {
                shown[0] = shown[1] = shown[2] = 255;
            } else {
                shown[0] /= 2;
                shown[1] /= 2;
                shown[2] /= 2;
            }
        }
        return differing;
    };

    set_level(HardwareAcceleration::full);
    if (!render_run_ || render_run_->tier.tier != policy::RenderTier::full)
        fail("the tier decided for --hardware-acceleration=full is not the full tier");
    if (!full_presentation())
        fail("--hardware-acceleration=full did not switch the full tier on");

    // Whole-number zooms: level 0 NEAREST, every sprite nearest, the
    // processor's picture exactly beside what the card draws its own way.
    // The check's own atlas of the map, built as the tier builds its own,
    // holds the texels the references read: the tier lets its own go once
    // uploaded.
    gw::TerrainAtlas reference_atlas;
    uint64_t pages_alive = 0;
    for (const float zoom : {1.0F, 2.0F, 4.0F}) {
        at_zoom(zoom);
        const auto read = full_frame();
        if (full_->plan.pass_count != 1 || full_->plan.passes[0].level != 0 ||
            full_->plan.passes[0].sampling != card::Sampling::nearest || full_->plan.through_target)
            fail("zoom " + zoom_text(zoom) + " was not drawn from level 0 NEAREST");
        const auto mask = card_mask(zoom, read);
        // The standard reference rebuilds the list without Full's effect trails.
        // Keep the Full frame's exclusions before that replaces its draw plan.
        const auto single_flash_mask = flash_mask(zoom, read, false);
        const auto stacked_flash_mask = flash_mask(zoom, read, true);
        const std::string full_cost = cost();
        const auto stage = full_->sprites;
        if (stage.pages_overflowed)
            fail("the sprite pages overflowed at zoom " + zoom_text(zoom));
        const auto expected = standard();
        const auto difference = compare_masked(read, expected, battlefield(), cursor(), mask);
        const auto whole =
            compare(read, expected, {0, 0, match_layout_.width, match_layout_.height}, cursor());
        std::cout << "render tiers check: full tier zoom " << zoom_text(zoom) << ": "
                  << difference.beside.pixels << " pixels beside the card's own: most "
                  << difference.beside.most << "; under them most " << difference.under.most
                  << ", mean " << difference.under.mean << " over " << difference.under.pixels
                  << "; whole frame most " << whole.most << "; " << stage.sprites << " sprites ("
                  << stage.greyed << " greyed, " << stage.refused << " refused), " << stage.squares
                  << " squares, " << stage.lines << " lines; " << full_cost << '\n';
        write_png(picture("zoom-" + zoom_text(zoom)), read);
        // The fight's explosion flashes, which the card lights close to the
        // processor's light table, within their bounds, where one flash
        // reaches a pixel; stacked flashes are reported, not held
        // (most_flash_difference).
        const auto flashes =
            compare_masked(read, expected, battlefield(), cursor(), single_flash_mask);
        const auto stacked =
            compare_masked(read, expected, battlefield(), cursor(), stacked_flash_mask);
        std::cout << "render tiers check: full tier zoom " << zoom_text(zoom) << ": " << stage.lit
                  << " explosion flashes; " << flashes.under.pixels
                  << " pixels one alone lights: most " << flashes.under.most << ", mean "
                  << flashes.under.mean << "; " << stacked.under.pixels
                  << " pixels under stacked flashes, not held: most " << stacked.under.most
                  << ", mean " << stacked.under.mean << '\n';
        if (zoom == 1.0F && (stage.lit == 0 || flashes.under.pixels + stacked.under.pixels == 0))
            fail("the fight drew no explosion flash of its own at zoom 1");
        if (flashes.under.most > most_flash_difference ||
            flashes.under.mean > most_flash_mean_difference)
            fail(
                "the explosion flashes at zoom " + zoom_text(zoom) +
                " differ from the standard tier's beyond their bounds"
            );
        if (difference.beside.pixels < least_compared_pixels || difference.beside.most != 0 ||
            whole.most > std::max(most_hud_difference, difference.under.most)) {
            write_png(picture("zoom-" + zoom_text(zoom) + "-standard"), expected);
            write_png(
                picture("zoom-" + zoom_text(zoom) + "-differing"),
                differing_picture(read, expected, mask)
            );
            // The draws under the first pixel that differs beside the mask.
            const Area field = battlefield();
            bool listed = false;
            for (int y = field.y; y < field.y + field.h && !listed; ++y)
                for (int x = field.x; x < field.x + field.w && !listed; ++x) {
                    const auto pixel = static_cast<std::size_t>(y) * read.width + x;
                    if (mask[pixel] != 0 ||
                        std::memcmp(&read.rgb[pixel * 3U], &expected.rgb[pixel * 3U], 3) == 0)
                        continue;
                    std::cout << "render tiers check: the first differing pixel beside the mask, ("
                              << x << ", " << y << "), is drawn by:\n";
                    print_card_draws_at(full_->frame, x, y);
                    listed = true;
                }
            fail(
                "zoom " + zoom_text(zoom) +
                " differs from the standard tier's picture beside the card's own draws"
            );
        }
        if (zoom == 1.0F) {
            const auto& full = *full_;
            pages_alive = full.executor.counts().pages_alive;
            if (full.pages.size() != full.atlas.pages.size() ||
                full.greyed_pages.size() != full.pages.size() ||
                pages_alive < 2 * full.pages.size())
                fail("the terrain pages alive are not the atlas's and its greyed pages");
            std::cout << "render tiers check: full tier terrain atlas: " << full.atlas.pages.size()
                      << " pages of " << full.atlas.slot_tiles.size() << " slots within "
                      << full.atlas_page_edge << ", " << full.page_bytes / 1024 / 1024
                      << " MiB uploaded with their greyed pages in " << full.page_upload_ns / 1000
                      << " us, built in " << full.atlas_build_ns / 1000 << " us"
                      << (full.pages_from_load ? ", as the match loaded" : ", at a frame") << '\n';
            for (const auto& page : full.atlas.pages)
                if (!page.texels.empty())
                    fail("the atlas kept a page's texels after the page was filled");
            if (!selected_tnt_)
                fail("the match has no map");
            const auto atlas_start = std::chrono::steady_clock::now();
            const auto [atlas_columns, atlas_rows] = shown_tile_grid();
            if (atlas_columns != selected_tnt_->tile_width - 1 ||
                atlas_rows != selected_tnt_->tile_height - 4)
                fail("the atlas grid is not the map less the edges the game never shows");
            if (gw::build_terrain_atlas(
                    *selected_tnt_,
                    atlas_columns,
                    atlas_rows,
                    match_palette_,
                    gamma_identity_ ? nullptr : &gamma_table_,
                    full.atlas_page_edge,
                    reference_atlas
                ) != gw::TerrainAtlasError::none)
                fail("the check could not build its own atlas of the map");
            if (reference_atlas.grid != full.atlas.grid ||
                reference_atlas.pages.size() != full.atlas.pages.size())
                fail("the check's atlas of the map differs from the tier's");
            std::cout << "render tiers check: the check's own atlas of the map built in "
                      << std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - atlas_start
                         )
                             .count()
                      << " us\n";
        } else if (full_->executor.counts().pages_alive < pages_alive) {
            fail("the terrain pages were lost at zoom " + zoom_text(zoom));
        }
    }

    // The state case: what the match reads back from a Full frame equals
    // the standard tier's at the same moment.
    {
        at_zoom(1.0F);
        std::ignore = full_frame();
        std::ignore = full_frame();
        const auto full_read = match_draw_read_back();
        set_level(HardwareAcceleration::off);
        std::ignore = presented();
        const auto standard_read = match_draw_read_back();
        if (const char* differs = draw_read_back_difference(full_read, standard_read);
            differs != nullptr)
            fail(
                std::string("a full frame left ") + differs +
                " otherwise than the standard tier's frame of the same moment"
            );
        std::cout << "render tiers check: full tier: a full frame leaves what the match reads as "
                     "the standard tier's frame does\n";
    }

    // Zoomed out past the processor's floor, which only Full reaches: the
    // zoom holds, and the card draws the terrain into the zoomed-out target
    // by the level rule at the target's texels, LINEAR: at a factor of 2,
    // a third of a texel a map pixel, level 2 with level 1 over it.
    {
        set_level(HardwareAcceleration::full);
        at_zoom(kMinFullBattlefieldZoom);
        const auto read = full_frame();
        const auto& plan = full_->plan;
        if (std::abs(match_zoom() - kMinFullBattlefieldZoom) > 1.0e-6F)
            fail("the full tier's zoom floor did not hold");
        const float texel_zoom =
            kMinFullBattlefieldZoom * static_cast<float>(full_->moved_target_factor);
        const auto expected = full_terrain::plan_terrain_draw(std::min(texel_zoom, 1.0F), false);
        bool levels_kept = full_->moved_target_factor != 0 &&
                           plan.pass_count == expected.pass_count && !plan.through_target;
        for (uint32_t index = 0; levels_kept && index < plan.pass_count; ++index)
            levels_kept = plan.passes[index].level == expected.passes[index].level &&
                          plan.passes[index].sampling == card::Sampling::linear;
        if (!levels_kept || plan.passes[0].level != 2)
            fail(
                "the full tier's zoom floor was not drawn by the level rule at the zoomed-out "
                "target's texels, from level 2 LINEAR"
            );
        if (!full_->drawn || full_->drawn_quads == 0)
            fail("the card drew no terrain at the full tier's zoom floor");
        write_png(picture("zoom-" + zoom_text(kMinFullBattlefieldZoom)), read);
        // The atlas holds the shown map alone, so where the view reaches
        // past the shown map's right edge the card draws nothing: black,
        // the filler tiles of the edge the game never shows left undrawn.
        {
            const auto [columns, rows] = shown_tile_grid();
            if (full_->atlas.grid_width != columns || full_->atlas.grid_height != rows)
                fail("the atlas grid is not the shown map's tiles");
            const auto shown_w = shown_map_size()[0];
            const auto field = battlefield();
            const auto pointer = cursor();
            const auto edge =
                field.x +
                static_cast<int>(std::ceil(
                    static_cast<double>(shown_w - static_cast<int32_t>(full_->frame_camera_x)) *
                    static_cast<double>(kMinFullBattlefieldZoom)
                ));
            // Past the sprites that may overhang the edge from units beside it.
            const int first = edge + 8;
            uint32_t sampled = 0;
            uint32_t lit = 0;
            for (int y = field.y; y < field.y + field.h; y += 4)
                for (int x = first; x < field.x + field.w; x += 4) {
                    if (x >= pointer.x && x < pointer.x + pointer.w && y >= pointer.y &&
                        y < pointer.y + pointer.h)
                        continue;
                    const std::size_t at =
                        (static_cast<std::size_t>(y) * read.width + static_cast<std::size_t>(x)) *
                        3U;
                    ++sampled;
                    if (read.rgb[at] != 0 || read.rgb[at + 1] != 0 || read.rgb[at + 2] != 0)
                        ++lit;
                }
            if (lit != 0)
                fail(
                    "the frame at the zoom floor is lit in " + std::to_string(lit) + " of " +
                    std::to_string(sampled) + " pixels past the shown map's right edge"
                );
            std::cout << "render tiers check: full tier: at the zoom floor the shown map ends at "
                      << edge << " of the battlefield's " << field.x + field.w << ", with "
                      << sampled << " pixels sampled black past it\n";
        }
        std::cout << "render tiers check: full tier: the zoom floor of "
                  << zoom_text(kMinFullBattlefieldZoom) << " is drawn from level 2 LINEAR"
                  << (plan.pass_count == 2 ? " with level 1 over it" : "")
                  << " into the zoomed-out target at factor " << full_->moved_target_factor << ", "
                  << full_->drawn_quads << " quads\n";
    }

    // Zoomed out to the whole map (Maximum zoom out), past the zoom each
    // tier draws units whole at: the processor draws the far view at the
    // zoom in every tier, Full's presented as Basic presents a frame
    // without leaving Full; its terrain is refreshed from the map's
    // pyramid. With Zoomed out units at Rendered its units are models; at
    // Dots they are dots and no model is planned; back at the floor the
    // card draws the frame again. Pictures of each go to the report.
    {
        namespace engine = oa::ui::engine_settings;
        const engine::EngineSettings saved = engine_settings();
        engine::EngineSettings whole_map = saved;
        whole_map.max_zoom_out = engine::ZoomOutLimit::whole_map;
        for (const auto units : {engine::ZoomedOutUnits::rendered, engine::ZoomedOutUnits::dots})
            for (const auto level :
                 {HardwareAcceleration::off,
                  HardwareAcceleration::basic,
                  HardwareAcceleration::full}) {
                whole_map.zoomed_out_units = units;
                apply_engine_settings(whole_map);
                set_level(level);
                const float whole = least_match_zoom();
                const bool chose_dots = units == engine::ZoomedOutUnits::dots;
                // Dots are drawn only past After zoom; a map that fits
                // whole nearer than that, as the demo's does, keeps models.
                const bool dots =
                    chose_dots && whole < engine::zoomed_out_zoom(whole_map.zoomed_out_after);
                const std::string tier = std::string(
                                             level == HardwareAcceleration::off     ? "off"
                                             : level == HardwareAcceleration::basic ? "basic"
                                                                                    : "full"
                                         ) +
                                         (chose_dots ? " dots" : " rendered");
                // A map that fits whole before the tier's units stop being
                // drawn whole, as the demo's does, has no far view to check.
                if (!(whole < detail_zoom_floor())) {
                    std::cout << "render tiers check: " << tier << " tier: the whole map fits at "
                              << zoom_text(whole) << ", before the far view; nothing to check\n";
                    continue;
                }
                at_zoom(whole);
                // The terrain drawn afresh, and the frame's own units counted.
                terrain_cache_cam_x_ = kUncachedTerrainCamera;
                frame_draws_.units_drawn = 0;
                const uint64_t runs = terrain_box_filter_runs_;
                const auto read = presented();
                if (!far_view_frame() || match_zoom() != whole)
                    fail("the " + tier + " tier did not hold the whole map's zoom as the far view");
                if (terrain_box_filter_runs_ == runs)
                    fail("the " + tier + " tier's far view did not refresh its terrain");
                if (dots_frame() != dots)
                    fail("the " + tier + " tier's far view did not draw its units as chosen");
                if (match_models().draws.models.empty() != dots)
                    fail(
                        "the " + tier + " tier's far view " +
                        (dots ? "planned a model" : "planned no model")
                    );
                if (frame_draws_.units_drawn == 0)
                    fail("the " + tier + " tier's far view drew no unit");
                if (level == HardwareAcceleration::full &&
                    (!full_presentation() || full_frame_drawn() || full_->drawn))
                    fail("the full tier's far view was not the processor's, presented as Basic's");
                const std::string slug = std::string(
                                             level == HardwareAcceleration::off     ? "off"
                                             : level == HardwareAcceleration::basic ? "basic"
                                                                                    : "full"
                                         ) +
                                         (dots ? "-dots" : "-rendered");
                write_png(
                    report_directory / ("native-render-tiers-far-view-" + slug + ".png"), read
                );
                std::cout << "render tiers check: " << tier << " tier: the whole map at zoom "
                          << zoom_text(whole) << " is the far view, " << frame_draws_.units_drawn
                          << " units drawn " << (dots ? "as dots" : "as models") << '\n';
            }
        // In the Full tier with After zoom at 1/4, a view at 1/5, nearer
        // than the tier's floor, is the card's, its units dots on the
        // overlay canvas and no model planned.
        whole_map.zoomed_out_units = engine::ZoomedOutUnits::dots;
        whole_map.zoomed_out_after = engine::ZoomedOutAfter::one_quarter;
        apply_engine_settings(whole_map);
        set_level(HardwareAcceleration::full);
        constexpr float card_dots_zoom = 0.2F;
        if (least_match_zoom() > card_dots_zoom) {
            std::cout << "render tiers check: full tier: the whole map fits at "
                      << zoom_text(least_match_zoom()) << ", nearer than "
                      << zoom_text(card_dots_zoom) << "; no card frame past After zoom to check\n";
        } else {
            at_zoom(card_dots_zoom);
            frame_draws_.units_drawn = 0;
            const auto card_dots = full_frame();
            if (far_view_frame() || !dots_frame() || !match_models().draws.models.empty() ||
                frame_draws_.units_drawn == 0)
                fail("the card's frame past After zoom did not draw its units as dots");
            write_png(report_directory / "native-render-tiers-full-card-dots.png", card_dots);
            std::cout << "render tiers check: full tier: at zoom " << zoom_text(card_dots_zoom)
                      << ", past After zoom 1/4, the card draws the frame and "
                      << frame_draws_.units_drawn << " units are dots\n";
        }
        at_zoom(kMinFullBattlefieldZoom);
        std::ignore = full_frame();
        apply_engine_settings(saved);
    }

    // Shadows lighten as the view zooms out and are not drawn from a
    // quarter out. The standard tier draws the game's own at zoom 1 and, at
    // its floor of 0.5, shadows at half the game's darkness through the
    // faded table; on the card, zoom 1 casts the game's own, 0.5 and a
    // third lighter ones, and a quarter and the zoom floor none, the list
    // holding no shadow either. Pictures of each go to the report.
    {
        set_level(HardwareAcceleration::off);
        for (const float zoom : {1.0F, kMinBattlefieldZoom}) {
            at_zoom(zoom);
            const auto read = presented();
            const bool whole = zoom == 1.0F;
            const auto& models = match_models();
            if (models.draws.shadow_level != oa::present::model::shadow_level(zoom) ||
                (models.renderer.shadow_table == nullptr) != whole)
                fail(
                    "the standard tier at zoom " + zoom_text(zoom) +
                    " did not draw shadows at its level"
                );
            write_png(
                report_directory / ("native-render-tiers-off-shadows-" + zoom_text(zoom) + ".png"),
                read
            );
        }
        set_level(HardwareAcceleration::full);
        for (const float zoom : {1.0F, 0.5F, 1.0F / 3.0F, 0.25F, kMinFullBattlefieldZoom}) {
            at_zoom(zoom);
            const uint64_t before = full_->models.counts().shadows;
            const auto read = full_frame();
            const uint64_t cast = full_->models.counts().shadows - before;
            const WorldDrawList& list = match_models().draws;
            const uint32_t level = oa::present::model::shadow_level(zoom);
            if (list.shadow_level != level)
                fail("the frame at zoom " + zoom_text(zoom) + " drew shadows at another level");
            std::size_t listed = 0;
            for (const SpriteDraw& sprite : list.sprites)
                listed += sprite.shadow ? 1U : 0U;
            for (const ProjectileDraw& shot : list.projectiles)
                listed += shot.shadow ? 1U : 0U;
            if (level == 0 && (cast != 0 || listed != 0))
                fail("the frame at zoom " + zoom_text(zoom) + " drew shadows");
            if (zoom == 1.0F && cast == 0)
                fail("the frame at zoom 1 cast no shadow");
            write_png(picture("shadows-" + zoom_text(zoom)), read);
            std::cout << "render tiers check: full tier: at zoom " << zoom_text(zoom)
                      << " shadows at level " << level << " of "
                      << oa::present::model::shadow_full_level << ", " << cast
                      << " cast on the card, " << listed << " shadow sprites listed\n";
        }
    }

    // Health bars keep the game's size, a trough 35 pixels across and 5
    // down, at zoom 1 and zoomed in, and shrink as the view zooms out: 29
    // by 5 at 0.75, 23 by 3 at a half, 15 by 3 at a quarter and 13 by 3 at
    // the Full tier's floor of a sixth, the same at the same zoom in every
    // tier. Each bar the frame lays out has that size, and each that no
    // other bar or the pointer covers shows on the frame: its trough's
    // corners in its colour and its fill's first pixel in the fill's.
    // Pictures of each go to the report.
    {
        struct BarCase {
            HardwareAcceleration level{};
            float zoom{};
            int32_t width{};
            int32_t height{};
        };

        constexpr std::array<BarCase, 13> bar_cases{{
            {HardwareAcceleration::off, 2.0F, 35, 5},
            {HardwareAcceleration::off, 1.0F, 35, 5},
            {HardwareAcceleration::off, 0.75F, 29, 5},
            {HardwareAcceleration::off, kMinBattlefieldZoom, 23, 3},
            {HardwareAcceleration::basic, 2.0F, 35, 5},
            {HardwareAcceleration::basic, 1.0F, 35, 5},
            {HardwareAcceleration::basic, 0.75F, 29, 5},
            {HardwareAcceleration::basic, kMinBattlefieldZoom, 23, 3},
            {HardwareAcceleration::full, 4.0F, 35, 5},
            {HardwareAcceleration::full, 1.0F, 35, 5},
            {HardwareAcceleration::full, 0.5F, 23, 3},
            {HardwareAcceleration::full, 0.25F, 15, 3},
            {HardwareAcceleration::full, kMinFullBattlefieldZoom, 13, 3},
        }};
        const auto level_name = [](HardwareAcceleration level) {
            return level == HardwareAcceleration::off     ? std::string("off")
                   : level == HardwareAcceleration::basic ? std::string("basic")
                                                          : std::string("full");
        };
        auto& game = match_->state().game;
        const uint16_t graphics_flags = game.graphics_flags;
        game.graphics_flags =
            static_cast<uint16_t>(graphics_flags | oa::ui::hud::kGraphicsDamageBars);
        const auto colour = [&](uint8_t index) {
            const std::size_t at = static_cast<std::size_t>(index) * oa::palette_entry_bytes;
            return gamma_of({match_palette_[at], match_palette_[at + 1], match_palette_[at + 2]});
        };
        for (const BarCase& bar_case : bar_cases) {
            const std::string where =
                level_name(bar_case.level) + " at zoom " + zoom_text(bar_case.zoom);
            set_level(bar_case.level);
            at_zoom(bar_case.zoom);
            const auto read =
                bar_case.level == HardwareAcceleration::full ? full_frame() : presented();
            write_png(
                report_directory /
                    ("native-render-tiers-health-bars-" + level_name(bar_case.level) + "-zoom-" +
                     zoom_text(bar_case.zoom) + ".png"),
                read
            );
            const auto& bars = drawn_health_bars_;
            if (bars.empty())
                fail("the frame " + where + " drew no health bar");
            const auto field = battlefield();
            const auto pointer = cursor();
            const auto overlaps = [](const oa::Rect32& a, const Area& b) {
                return a.x1 < b.x + b.w && b.x <= a.x2 && a.y1 < b.y + b.h && b.y <= a.y2;
            };
            const auto pixel = [&](int32_t x, int32_t y) {
                const std::size_t at =
                    (static_cast<std::size_t>(y) * read.width + static_cast<std::size_t>(x)) * 3U;
                return std::vector<uint8_t>{read.rgb[at], read.rgb[at + 1], read.rgb[at + 2]};
            };
            uint32_t shown = 0;
            for (std::size_t index = 0; index < bars.size(); ++index) {
                const auto& bar = bars[index];
                if (bar.trough.x2 - bar.trough.x1 + 1 != bar_case.width ||
                    bar.trough.y2 - bar.trough.y1 + 1 != bar_case.height ||
                    bar.fill.y2 - bar.fill.y1 + 1 != bar_case.height - 2)
                    fail(
                        "a health bar " + where + " is " +
                        std::to_string(bar.trough.x2 - bar.trough.x1 + 1) + " by " +
                        std::to_string(bar.trough.y2 - bar.trough.y1 + 1) + ", not " +
                        std::to_string(bar_case.width) + " by " + std::to_string(bar_case.height)
                    );
                // Where the bar lies on the frame.
                const oa::Rect32 trough{
                    bar.trough.x1 + field.x,
                    bar.trough.y1 + field.y,
                    bar.trough.x2 + field.x,
                    bar.trough.y2 + field.y
                };
                if (trough.x1 < field.x || trough.y1 < field.y || trough.x2 >= field.x + field.w ||
                    trough.y2 >= field.y + field.h || overlaps(trough, pointer))
                    continue;
                bool covered = false;
                for (std::size_t other = 0; other < bars.size() && !covered; ++other) {
                    const auto& by = bars[other].trough;
                    covered =
                        other != index &&
                        overlaps(
                            bar.trough, Area{by.x1, by.y1, by.x2 - by.x1 + 1, by.y2 - by.y1 + 1}
                        );
                }
                if (covered)
                    continue;
                const auto trough_colour = colour(bar.trough_color);
                for (const auto& [x, y] :
                     {std::pair{trough.x1, trough.y1},
                      std::pair{trough.x2, trough.y1},
                      std::pair{trough.x1, trough.y2},
                      std::pair{trough.x2, trough.y2}})
                    if (pixel(x, y) != trough_colour)
                        fail(
                            "a health bar's trough " + where + " is not drawn at " +
                            std::to_string(x) + ", " + std::to_string(y)
                        );
                if (pixel(trough.x1 + 1, trough.y1 + 1) != colour(bar.fill_color))
                    fail("a health bar's fill " + where + " is not drawn");
                ++shown;
            }
            if (shown == 0)
                fail("no health bar " + where + " showed apart from the others");
            std::cout << "render tiers check: health bars " << where << ": " << bar_case.width
                      << " by " << bar_case.height << ", " << bars.size() << " laid out, " << shown
                      << " held to the frame\n";
        }

        // The +stats panel covers the health bars under it in every tier:
        // with the camera moved for a bar at full health to lie at the
        // panel's middle, no pixel of such a bar's fill inside the panel
        // shows the fill's colour, which the panel never draws in. The view
        // is zoomed in so that a bar can come under the panel when the
        // commander starts by the map's edge.
        const uint8_t full_health = game.ui_colors[oa::ui::hud::kHealthHighColor];
        constexpr int stats_zoom = 4;
        for (const HardwareAcceleration level :
             {HardwareAcceleration::off, HardwareAcceleration::basic, HardwareAcceleration::full}) {
            const std::string where = level_name(level);
            const auto frame = [&]() {
                return level == HardwareAcceleration::full ? full_frame() : presented();
            };
            set_level(level);
            at_zoom(static_cast<float>(stats_zoom));
            show_frame_stats(true);
            std::ignore = frame();
            if (!frame_stats_place_)
                fail("+stats placed no panel " + where);
            const Area field = battlefield();
            // The panel's middle on the battlefield, where a map pixel is
            // stats_zoom pixels.
            const auto& panel = frame_stats_place_->panel;
            const int middle_x = panel.x + panel.width / 2 - field.x;
            const int middle_y = panel.y + panel.height / 2 - field.y;
            const auto camera = view_camera();
            const auto [map_width, map_height] = shown_map_size();
            const int most_x = std::max(0, map_width - visible_map_width());
            const int most_z = std::max(0, map_height - visible_map_height());
            bool moved = false;
            for (const auto& bar : drawn_health_bars_) {
                const int camera_x =
                    camera[0] + ((bar.fill.x1 + bar.fill.x2) / 2 - middle_x) / stats_zoom;
                const int camera_z =
                    camera[1] + ((bar.fill.y1 + bar.fill.y2) / 2 - middle_y) / stats_zoom;
                if (bar.fill_color != full_health || camera_x < 0 || camera_z < 0 ||
                    camera_x > most_x || camera_z > most_z)
                    continue;
                match_camera_x_ = camera_x;
                match_camera_z_ = camera_z;
                moved = true;
                break;
            }
            if (!moved)
                fail("no health bar at full health " + where + " can lie under the +stats panel");
            const auto read = frame();
            write_png(
                report_directory / ("native-render-tiers-stats-over-health-bars-" + where + ".png"),
                read
            );
            if (!frame_stats_place_)
                fail("+stats placed no panel " + where);
            const auto& covered = frame_stats_place_->panel;
            const Area pointer = cursor();
            const auto fill_colour = colour(full_health);
            std::size_t under = 0;
            std::size_t showing = 0;
            for (const auto& bar : drawn_health_bars_) {
                if (bar.fill_color != full_health)
                    continue;
                for (int32_t y = bar.fill.y1; y <= bar.fill.y2; ++y)
                    for (int32_t x = bar.fill.x1; x <= bar.fill.x2; ++x) {
                        const int at_x = x + field.x;
                        const int at_y = y + field.y;
                        if (at_x < covered.x || at_y < covered.y ||
                            at_x >= covered.x + covered.width ||
                            at_y >= covered.y + covered.height ||
                            (at_x >= pointer.x && at_y >= pointer.y &&
                             at_x < pointer.x + pointer.w && at_y < pointer.y + pointer.h))
                            continue;
                        ++under;
                        const std::size_t at = (static_cast<std::size_t>(at_y) * read.width +
                                                static_cast<std::size_t>(at_x)) *
                                               3U;
                        if (std::equal(
                                fill_colour.begin(), fill_colour.end(), read.rgb.data() + at
                            ))
                            ++showing;
                    }
            }
            show_frame_stats(false);
            std::cout << "render tiers check: +stats over the health bars " << where << ": "
                      << under << " pixels of bars at full health under the panel, " << showing
                      << " showing their colour\n";
            if (under == 0)
                fail("no health bar " + where + " lay under the +stats panel");
            if (showing != 0)
                fail("health bars " + where + " show over the +stats panel");
        }
        game.graphics_flags = graphics_flags;
    }

    // A card target read back, the texture's size.
    const auto read_card_target = [&](card::TargetHandle target) {
        renderer::Surface picture;
        SDL_Texture* texture = full_->executor.target_texture(target);
        SDL_Texture* before = SDL_GetRenderTarget(sdl_.renderer);
        SDL_Surface* read = nullptr;
        if (texture != nullptr && SDL_SetRenderTarget(sdl_.renderer, texture))
            read = SDL_RenderReadPixels(sdl_.renderer, nullptr);
        std::ignore = SDL_SetRenderTarget(sdl_.renderer, before);
        SDL_Surface* rgb =
            read != nullptr ? SDL_ConvertSurface(read, SDL_PIXELFORMAT_RGB24) : nullptr;
        SDL_DestroySurface(read);
        if (rgb == nullptr)
            fail(std::string("reading back a card target: ") + SDL_GetError());
        picture.width = static_cast<uint32_t>(rgb->w);
        picture.height = static_cast<uint32_t>(rgb->h);
        picture.rgb.resize(std::size_t{picture.width} * picture.height * 3U);
        for (int row = 0; row < rgb->h; ++row)
            std::memcpy(
                picture.rgb.data() + static_cast<std::size_t>(row) * picture.width * 3U,
                static_cast<const uint8_t*>(rgb->pixels) +
                    static_cast<std::ptrdiff_t>(row) * rgb->pitch,
                std::size_t{picture.width} * 3U
            );
        SDL_DestroySurface(rgb);
        return picture;
    };
    // Holds the battlefield presented to the zoomed-out target it was
    // reduced from, read back at its factor with its room of a layout pixel
    // on every side, reduced on the processor as the card reduces it: to
    // the presented frame's pixels by the factor's halvings, each a LINEAR
    // draw at one half, within the renderer's tolerance; the pointer left
    // out.
    const auto hold_presented_to_reduced = [&](const renderer::Surface& presented_frame,
                                               const renderer::Surface& target_picture,
                                               uint32_t factor,
                                               const std::string& what) {
        const auto layout_width = static_cast<uint32_t>(match_layout_.width);
        const uint32_t density = layout_width != 0 ? presented_frame.width / layout_width : 0U;
        const Area field = battlefield();
        const auto room_width = static_cast<uint32_t>(field.w) + 2U;
        const auto room_height = static_cast<uint32_t>(field.h) + 2U;
        if (density == 0 || factor == 0 || density * layout_width != presented_frame.width ||
            factor % density != 0 || target_picture.width != room_width * factor ||
            target_picture.height != room_height * factor)
            fail(
                what + ": the presented frame of " + std::to_string(presented_frame.width) + "x" +
                std::to_string(presented_frame.height) + " and the zoomed-out target of " +
                std::to_string(target_picture.width) + "x" + std::to_string(target_picture.height) +
                " at factor " + std::to_string(factor) + " do not match a layout " +
                std::to_string(layout_width) + " wide"
            );
        const uint32_t reduction = factor / density;
        const uint32_t width = room_width * density;
        const uint32_t height = room_height * density;
        std::vector<uint8_t> reduced;
        if (reduction == 1) {
            reduced = target_picture.rgb;
        } else {
            if (reduction != 2 && reduction != 4)
                fail(
                    what + ": the zoomed-out target is reduced by " + std::to_string(reduction) +
                    ", which the check does not model"
                );
            full_supersampling::WorldTargetPlan plan;
            plan.factor = reduction;
            plan.destination = {0, 0, static_cast<int32_t>(width), static_cast<int32_t>(height)};
            reduced = reduce_world_target_reference(
                target_picture.rgb, target_picture.width, target_picture.height, plan, software
            );
        }
        const Area pointer = cursor();
        const auto d = static_cast<int>(density);
        Difference difference;
        double sum = 0.0;
        std::size_t channels = 0;
        for (int y = 0; y < field.h * d; ++y)
            for (int x = 0; x < field.w * d; ++x) {
                const int sx = field.x * d + x;
                const int sy = field.y * d + y;
                if (sx >= pointer.x * d && sx < (pointer.x + pointer.w) * d &&
                    sy >= pointer.y * d && sy < (pointer.y + pointer.h) * d)
                    continue;
                const auto at = (static_cast<std::size_t>(sy) * presented_frame.width +
                                 static_cast<std::size_t>(sx)) *
                                3U;
                const auto expected_at =
                    (static_cast<std::size_t>(d + y) * width + static_cast<std::size_t>(d + x)) *
                    3U;
                ++difference.pixels;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const int delta = std::abs(
                        int{presented_frame.rgb[at + channel]} - int{reduced[expected_at + channel]}
                    );
                    difference.most = std::max(difference.most, delta);
                    sum += delta;
                    ++channels;
                }
            }
        difference.mean = channels != 0 ? sum / static_cast<double>(channels) : 0.0;
        const int most_allowed = software ? most_software_difference : most_card_difference;
        if (difference.pixels == 0 || difference.most > most_allowed ||
            difference.mean > most_mean_scaled_difference) {
            renderer::Surface reference{width, height, reduced};
            write_png(picture("zoomed-out-reduced"), reference);
            write_png(picture("zoomed-out-presented"), presented_frame);
            fail(
                what +
                " presented strays from the zoomed-out target reduced on the processor: "
                "most " +
                std::to_string(difference.most) + ", mean " + std::to_string(difference.mean)
            );
        }
        return difference;
    };

    // Zoomed out: the card's levels, never the box filter, drawn into the
    // zoomed-out target at its factor by the level rule at its texels and
    // reduced onto the battlefield. The target's terrain beside the card's
    // own draws is held to the renderer's own LINEAR of each tile at the
    // target's texels, pass over pass; the battlefield presented to the
    // target reduced on the processor as the card reduces it; and the
    // terrain presented, where the standard tier shows its terrain too, to
    // the standard tier's box filter of the same moment: at zoom 0.5 with
    // the camera on an even map pixel, where level 0 drawn at one texel a
    // map pixel and reduced by halving is that filter, within the level the
    // renderer's rounding of the mean of four may take; at 0.75 level 0
    // enlarged between the target's texels and reduced is the card's own
    // filter, held to a mean difference and printed. The camera is set on
    // an even map pixel, so the target lands on the battlefield with no
    // shift.
    for (const float zoom : {kMinBattlefieldZoom, 0.75F}) {
        set_level(HardwareAcceleration::full);
        at_zoom(zoom);
        // The camera the frame uses, within the map as the frame holds it
        // (a small map holds it at an edge), on an even map pixel.
        const auto [map_width, map_height] = shown_map_size();
        match_camera_x_ =
            std::clamp(match_camera_x_, 0, std::max(0, map_width - visible_map_width())) & ~1;
        match_camera_z_ =
            std::clamp(match_camera_z_, 0, std::max(0, map_height - visible_map_height())) & ~1;
        const auto read = full_frame();
        const auto& plan = full_->plan;
        // The level rule at the target's texels: at a factor of 2 level 0
        // alone, one texel a map pixel at zoom 0.5 and enlarged at 0.75.
        {
            const float texel_zoom = zoom * static_cast<float>(full_->moved_target_factor);
            const auto expected =
                full_terrain::plan_terrain_draw(std::min(texel_zoom, 1.0F), false);
            bool levels_kept = full_->moved_target_factor != 0 &&
                               plan.pass_count == expected.pass_count && !plan.through_target;
            for (uint32_t index = 0; levels_kept && index < plan.pass_count; ++index)
                levels_kept = plan.passes[index].level == expected.passes[index].level &&
                              plan.passes[index].blend == expected.passes[index].blend &&
                              plan.passes[index].alpha == expected.passes[index].alpha &&
                              plan.passes[index].sampling == card::Sampling::linear;
            if (!levels_kept)
                fail(
                    "zoom " + zoom_text(zoom) +
                    " did not take the level rule's passes at the zoomed-out target's texels"
                );
        }
        const std::string full_cost = cost();
        const auto mask = card_mask(zoom, read);
        const auto overlay = full_->overlay;
        const int32_t cam_x = full_->frame_camera_x;
        const int32_t cam_y = full_->frame_camera_y;
        if (cam_x % 2 != 0 || cam_y % 2 != 0)
            fail("the camera is not on an even map pixel at zoom " + zoom_text(zoom));
        write_png(picture("zoom-" + zoom_text(zoom)), read);
        // The zoomed-out target the card drew the battlefield into, a layout
        // pixel of room on every side, and the target it was moved into and
        // reduced from.
        const auto& drawn_into = *full_;
        const auto factor = static_cast<int>(drawn_into.moved_target_factor);
        if (drawn_into.moved_target == card::TargetHandle{} || factor == 0)
            fail("zoom " + zoom_text(zoom) + " was not drawn into the zoomed-out target");
        const auto moved_picture = read_card_target(drawn_into.moved_target);
        const auto reduced_picture = read_card_target(
            drawn_into.shifted_target != card::TargetHandle{} ? drawn_into.shifted_target
                                                              : drawn_into.moved_target
        );
        // The renderer's own picture of the passes at the target's texels,
        // each tile's quad drawn LINEAR from the check's atlas and each pass
        // blended over the one before at its alpha. On SDL's
        // software renderer each quad is a texture copy: its corners scaled
        // to the target's texels, its rectangle truncated to whole pixels,
        // the tile's texels at the level stretched into it as that renderer
        // draws a texture LINEAR (software_linear_rgb24), and the blend in
        // that renderer's 256ths. On a card each quad covers the texels
        // whose centres lie within it, each sampled LINEAR at its centre
        // from the tile, clamped at the tile's edge as the gutter clamps it
        // (bilinear_rgb24), and the blend is exact, rounded once.
        renderer::Surface renderer_reference = moved_picture;
        {
            const auto& atlas = reference_atlas;
            const Area field = battlefield();
            // The battlefield's texels in the target, past its room.
            const int field_left = factor;
            const int field_top = factor;
            const int field_right = factor + field.w * factor;
            const int field_bottom = factor + field.h * factor;
            const auto within_field = [&](int x, int y) {
                return x >= field_left && x < field_right && y >= field_top && y < field_bottom;
            };
            full_terrain::TerrainView view;
            view.camera_x = cam_x;
            view.camera_y = cam_y;
            view.origin_x = 1.0F;
            view.origin_y = 1.0F;
            view.scale = zoom;
            view.width = static_cast<uint32_t>(field.w);
            view.height = static_cast<uint32_t>(field.h);
            const auto range = full_terrain::visible_tiles(atlas, view);
            const auto tile_pixels = static_cast<float>(gw::tile_edge) * zoom;
            const auto scale = static_cast<float>(factor);
            // Lays a drawn tile over the reference at a texel, replacing or
            // blending by the pass.
            const auto lay = [&](int sx,
                                 int sy,
                                 const uint8_t* drawn,
                                 card::Blend blend,
                                 int alpha_256ths,
                                 double alpha) {
                if (!within_field(sx, sy))
                    return;
                const auto at = (static_cast<std::size_t>(sy) * renderer_reference.width + sx) * 3U;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const int source_level = drawn[channel];
                    uint8_t& destination = renderer_reference.rgb[at + channel];
                    if (blend == card::Blend::none)
                        destination = static_cast<uint8_t>(source_level);
                    else if (software)
                        destination = static_cast<uint8_t>(
                            (((source_level - destination) * alpha_256ths) >> blend_shift) +
                            destination
                        );
                    else
                        destination = static_cast<uint8_t>(
                            std::lround(destination + (source_level - destination) * alpha)
                        );
                }
            };
            for (uint32_t pass = 0; pass < plan.pass_count; ++pass) {
                const auto& draw = plan.passes[pass];
                const double alpha = std::clamp(static_cast<double>(draw.alpha), 0.0, 1.0);
                const auto alpha_256ths = static_cast<int>(std::lround(alpha * 255.0));
                for (uint32_t row = range.first_row; row < range.end_row; ++row)
                    for (uint32_t column = range.first_column; column < range.end_column;
                         ++column) {
                        const auto rect = gw::tile_rect(
                            atlas,
                            atlas.grid[static_cast<std::size_t>(row) * atlas.grid_width + column],
                            draw.level
                        );
                        if (!rect)
                            fail("the atlas lacks a tile at zoom " + zoom_text(zoom));
                        const auto& page = atlas.pages[rect->page];
                        const auto& level = page.levels[draw.level];
                        std::vector<uint8_t> tile(std::size_t{rect->edge} * rect->edge * 3U);
                        for (uint32_t ty = 0; ty < rect->edge; ++ty)
                            for (uint32_t tx = 0; tx < rect->edge; ++tx)
                                std::memcpy(
                                    &tile[(std::size_t{ty} * rect->edge + tx) * 3U],
                                    &page.texels
                                         [level.offset +
                                          ((std::size_t{rect->y} + ty) * level.width + rect->x +
                                           tx) *
                                              4U],
                                    3
                                );
                        const wr::RgbSource source{tile.data(), rect->edge, rect->edge, rect->edge};
                        // The quad's corner and size as the builder gives them.
                        const auto corner_x = static_cast<float>(
                            static_cast<double>(view.origin_x) +
                            (static_cast<double>(column) * gw::tile_edge -
                             static_cast<double>(cam_x)) *
                                zoom
                        );
                        const auto corner_y = static_cast<float>(
                            static_cast<double>(view.origin_y) +
                            (static_cast<double>(row) * gw::tile_edge -
                             static_cast<double>(cam_y)) *
                                zoom
                        );
                        // The corners at the target's texels, as the renderer
                        // scales them.
                        const float texel_x = corner_x * scale;
                        const float texel_y = corner_y * scale;
                        const float texel_right = (corner_x + tile_pixels) * scale;
                        const float texel_bottom = (corner_y + tile_pixels) * scale;
                        if (software) {
                            const SDL_Rect landed{
                                static_cast<int>(texel_x),
                                static_cast<int>(texel_y),
                                static_cast<int>(texel_right - texel_x),
                                static_cast<int>(texel_bottom - texel_y)
                            };
                            if (landed.w <= 0 || landed.h <= 0)
                                continue;
                            const auto landed_w = static_cast<uint32_t>(landed.w);
                            const auto landed_h = static_cast<uint32_t>(landed.h);
                            std::vector<uint8_t> stretched(std::size_t{landed_w} * landed_h * 3U);
                            software_linear_rgb24(
                                source,
                                1,
                                {0, 0, landed.w, landed.h},
                                {stretched.data(), landed_w, landed_h, landed_w}
                            );
                            for (int y = 0; y < landed.h; ++y)
                                for (int x = 0; x < landed.w; ++x)
                                    lay(landed.x + x,
                                        landed.y + y,
                                        &stretched
                                            [(static_cast<std::size_t>(y) * landed_w + x) * 3U],
                                        draw.blend,
                                        alpha_256ths,
                                        alpha);
                            continue;
                        }
                        // The pixels whose centres the quad covers, by the
                        // top-left rule: from the first centre at or past the
                        // quad's left and top edges to the last before its
                        // right and bottom edges.
                        const double left = texel_x;
                        const double top = texel_y;
                        const double size = static_cast<double>(tile_pixels) * factor;
                        const auto first_x = static_cast<int>(std::ceil(left - 0.5));
                        const auto first_y = static_cast<int>(std::ceil(top - 0.5));
                        const auto end_x = static_cast<int>(std::ceil(left + size - 0.5));
                        const auto end_y = static_cast<int>(std::ceil(top + size - 0.5));
                        if (end_x <= first_x || end_y <= first_y)
                            continue;
                        const auto covered_w = static_cast<uint32_t>(end_x - first_x);
                        const auto covered_h = static_cast<uint32_t>(end_y - first_y);
                        std::vector<uint8_t> drawn(std::size_t{covered_w} * covered_h * 3U);
                        wr::bilinear_rgb24(
                            source,
                            {size / rect->edge,
                             size / rect->edge,
                             left - static_cast<double>(first_x),
                             top - static_cast<double>(first_y)},
                            {drawn.data(), covered_w, covered_h, covered_w}
                        );
                        for (uint32_t y = 0; y < covered_h; ++y)
                            for (uint32_t x = 0; x < covered_w; ++x)
                                lay(first_x + static_cast<int>(x),
                                    first_y + static_cast<int>(y),
                                    &drawn[(std::size_t{y} * covered_w + x) * 3U],
                                    draw.blend,
                                    alpha_256ths,
                                    alpha);
                    }
            }
        }
        // The standard tier's frame of the same moment, whose terrain base
        // is the box filter's.
        set_level(HardwareAcceleration::off);
        const uint64_t filter_ns = terrain_box_filter_ns_;
        const uint64_t filter_runs = terrain_box_filter_runs_;
        const auto standard_frame = composed_after(presented, composed);
        if (terrain_cache_cam_x_ != cam_x || terrain_cache_cam_y_ != cam_y ||
            terrain_box_filter_runs_ == filter_runs)
            fail("the standard tier did not box-filter the same view at zoom " + zoom_text(zoom));
        const auto boxed = gamma_of(match_terrain_cache_.rgb);
        const Area field = battlefield();
        const Area pointer = cursor();
        const auto bf_w = static_cast<uint32_t>(field.w);
        Difference difference;
        double sum = 0.0;
        std::size_t channels = 0;
        std::size_t covered = 0;
        for (int y = 0; y < field.h; ++y)
            for (int x = 0; x < field.w; ++x) {
                const int sx = field.x + x;
                const int sy = field.y + y;
                if (sx >= pointer.x && sx < pointer.x + pointer.w && sy >= pointer.y &&
                    sy < pointer.y + pointer.h)
                    continue;
                const auto cell = static_cast<std::size_t>(y) * bf_w + static_cast<uint32_t>(x);
                const auto marked = static_cast<std::size_t>(sy) * read.width + sx;
                if (overlay[cell * 4U + 3U] != 0 || mask[marked] != 0) {
                    ++covered;
                    continue;
                }
                const auto at = marked * 3U;
                if (std::memcmp(&standard_frame.rgb[at], &boxed[cell * 3U], 3) != 0) {
                    ++covered;
                    continue;
                }
                ++difference.pixels;
                for (std::size_t channel = 0; channel < 3; ++channel) {
                    const int delta =
                        std::abs(int{read.rgb[at + channel]} - int{boxed[cell * 3U + channel]});
                    difference.most = std::max(difference.most, delta);
                    sum += delta;
                    ++channels;
                }
            }
        difference.mean = channels != 0 ? sum / static_cast<double>(channels) : 0.0;
        std::cout << "render tiers check: full tier zoom " << zoom_text(zoom) << ": terrain of "
                  << difference.pixels << " pixels against the box filter (" << covered
                  << " under the overlay, the card's own draws or a unit): most " << difference.most
                  << ", mean " << difference.mean << "; " << full_cost
                  << "; the standard tier's box filter took "
                  << (terrain_box_filter_ns_ - filter_ns) / 1000 << " us\n";
        const bool enough = difference.pixels >= least_terrain_pixels;
        const bool exact = zoom == kMinBattlefieldZoom;
        if (!enough || (exact ? difference.most > most_halving_difference
                              : (difference.mean > most_blend_mean_difference ||
                                 difference.most > most_blend_difference))) {
            write_png(picture("zoom-" + zoom_text(zoom) + "-standard"), standard_frame);
            fail(
                "the terrain at zoom " + zoom_text(zoom) +
                (exact ? " differs from the box filter" : " strays from the box filter")
            );
        }
        // The target's terrain is held to the renderer's own LINEAR of each
        // tile at its texels, pass over pass, within the renderer's
        // tolerance: SDL's software renderer to its own, a card to the
        // card's.
        {
            Difference modelled;
            double model_sum = 0.0;
            std::size_t model_channels = 0;
            for (int y = 0; y < field.h; ++y)
                for (int x = 0; x < field.w; ++x) {
                    const auto cell = static_cast<std::size_t>(y) * bf_w + static_cast<uint32_t>(x);
                    const auto marked =
                        static_cast<std::size_t>(field.y + y) * read.width + (field.x + x);
                    if (overlay[cell * 4U + 3U] != 0 || mask[marked] != 0)
                        continue;
                    for (int ty = 0; ty < factor; ++ty)
                        for (int tx = 0; tx < factor; ++tx) {
                            const auto at = (static_cast<std::size_t>(factor + y * factor + ty) *
                                                 moved_picture.width +
                                             static_cast<std::size_t>(factor + x * factor + tx)) *
                                            3U;
                            ++modelled.pixels;
                            for (std::size_t channel = 0; channel < 3; ++channel) {
                                const int delta = std::abs(
                                    int{moved_picture.rgb[at + channel]} -
                                    int{renderer_reference.rgb[at + channel]}
                                );
                                modelled.most = std::max(modelled.most, delta);
                                model_sum += delta;
                                ++model_channels;
                            }
                        }
                }
            modelled.mean =
                model_channels != 0 ? model_sum / static_cast<double>(model_channels) : 0.0;
            const int most_allowed = software ? most_software_difference : most_card_difference;
            const double mean_allowed =
                software ? most_blended_mean_difference : most_mean_scaled_difference;
            std::cout << "render tiers check: full tier zoom " << zoom_text(zoom) << ": terrain of "
                      << modelled.pixels << " texels of the zoomed-out target at factor " << factor
                      << " against the " << (software ? "software renderer's" : "card's")
                      << " LINEAR of each tile, blended: most " << modelled.most << ", mean "
                      << modelled.mean << '\n';
            if (modelled.pixels < least_terrain_pixels || modelled.most > most_allowed ||
                modelled.mean > mean_allowed) {
                write_png(picture("zoom-" + zoom_text(zoom) + "-reference"), renderer_reference);
                write_png(picture("zoom-" + zoom_text(zoom) + "-target"), moved_picture);
                fail(
                    "the terrain at zoom " + zoom_text(zoom) +
                    " strays from the renderer's own LINEAR of each tile"
                );
            }
        }
        // What the player sees: the battlefield presented is the target
        // reduced on the processor as the card reduces it, each pixel the
        // box of its texels, the pointer left out.
        const auto seen = hold_presented_to_reduced(
            read,
            reduced_picture,
            static_cast<uint32_t>(factor),
            "the battlefield at zoom " + zoom_text(zoom)
        );
        std::cout << "render tiers check: full tier zoom " << zoom_text(zoom) << ": the "
                  << seen.pixels << " pixels of the battlefield presented against the "
                  << "zoomed-out target reduced on the processor: most " << seen.most << ", mean "
                  << seen.mean << '\n';
    }

    // Following a walking unit at the farthest zoom: the card draws the
    // battlefield into its moved target on the screen pixels laid from the
    // map's corner, so the target's picture moves by whole pixels and is
    // never sampled afresh, and moves the target by the rest of a screen
    // pixel at its own texels into the shifted target, which it reduces
    // onto the battlefield, so that the ground and everything on it lie at
    // the view's exact place every frame, gliding with the followed unit
    // rather than stepping a pixel at a time. Every frame presented is the
    // shifted target reduced, and on a card the shifted target is the
    // moved target moved by the shift. The pointer finds there the map
    // point drawn under it, and the followed unit where it is drawn.
    {
        set_level(HardwareAcceleration::full);
        const float zoom = least_match_zoom();
        at_zoom(zoom);
        // A walk across the map, toward its middle, by the first unit of the
        // local player that takes it and is still walking once it has
        // started: a mission's unit may fall to an enemy close by first.
        const auto [map_width, map_height] = shown_map_size();
        const auto toward_middle = [](int32_t at, int32_t size, int32_t reach) {
            return std::clamp(at + (at < size / 2 ? reach : -reach), 64, size - 64);
        };
        uint16_t walker = 0;
        for (std::size_t index = 1; index < match_->world().slots.size() && walker == 0; ++index) {
            const auto& slot = match_->world().slots[index];
            if (slot.unit == nullptr || slot.record.type_index == 0 ||
                slot.record.owner_index != match_local_player_)
                continue;
            const uint16_t id = slot.unit_index;
            const uint32_t start_x = slot.unit->position[0];
            const uint32_t start_z = slot.unit->position[2];
            const int32_t goal_x = toward_middle(start_x >> 16, map_width, follow_walk_reach);
            const int32_t goal_z = toward_middle(start_z >> 16, map_height, follow_walk_reach);
            const auto& order = match_->issue_ground_move(
                id,
                {goal_x * 0x10000,
                 match_->map_height(
                     static_cast<uint32_t>(goal_x) << 16, static_cast<uint32_t>(goal_z) << 16
                 ) * 0x10000,
                 goal_z * 0x10000},
                false
            );
            if (match_->order_refused(order))
                continue;
            for (int tick = 0; tick < follow_walk_start_ticks; ++tick)
                step_match_simulation();
            if (!match_unit_present(id))
                continue;
            const auto* unit = match_->world().slots[id].unit;
            if (unit->position[0] != start_x || unit->position[2] != start_z)
                walker = id;
        }
        if (walker == 0)
            fail("no unit of the local player took a walk and was still walking to follow");
        begin_match_tracking(walker);
        // The moved target read back.
        const auto read_moved = [&]() { return read_card_target(full_->moved_target); };
        // The share of the battlefield's pixels, the middle where the unit
        // walks left out, that differ from the frame before moved by a
        // whole pixel, at the best such move.
        const auto unmatched = [&](const renderer::Surface& before,
                                   const renderer::Surface& after) {
            const auto factor = static_cast<int>(full_->moved_target_factor);
            const int width = match_layout_.battlefield_width() * factor;
            const int height = match_layout_.battlefield_height() * factor;
            const int middle = follow_middle_reach * factor;
            uint64_t best = UINT64_MAX;
            uint64_t compared = 0;
            for (int dy = -factor; dy <= factor; ++dy)
                for (int dx = -factor; dx <= factor; ++dx) {
                    uint64_t differing = 0;
                    compared = 0;
                    for (int y = 2 * factor; y < height - 2 * factor; ++y)
                        for (int x = 2 * factor; x < width - 2 * factor; ++x) {
                            if (std::abs(x - width / 2) < middle &&
                                std::abs(y - height / 2) < middle)
                                continue;
                            ++compared;
                            const auto at = [&](const renderer::Surface& picture, int px, int py) {
                                return picture.rgb.data() +
                                       (static_cast<std::size_t>(py + factor) * picture.width +
                                        static_cast<std::size_t>(px + factor)) *
                                           3U;
                            };
                            if (!std::equal(
                                    at(before, x, y),
                                    at(before, x, y) + 3,
                                    at(after, x + dx, y + dy)
                                ))
                                ++differing;
                        }
                    best = std::min(best, differing);
                }
            return std::pair{best, compared};
        };
        renderer::Surface moved_before;
        int presented_most = 0;
        std::set<long long> shifts_seen;
        int glided = 0;
        for (int frame = 0; frame < follow_frames; ++frame) {
            move_match_camera();
            const int within = frame % follow_frames_a_tick;
            if (within == 0)
                step_match_simulation();
            set_presentation_alpha(
                static_cast<float>(within + 1) / static_cast<float>(follow_frames_a_tick)
            );
            place_tracking_camera();
            const auto presented_frame = full_frame();
            set_presentation_alpha(1.0F);
            if (!match_tracking_)
                fail("the follow ended at frame " + std::to_string(frame));
            const auto& full = *full_;
            const bool shifted_apart = full.shifted_target != card::TargetHandle{};
            if (full.moved_target == card::TargetHandle{} ||
                full.moved_frame.batches.size() != (shifted_apart ? 2U : 1U))
                fail("a frame at zoom " + zoom_text(zoom) + " was not drawn into the moved target");
            // Every draw of the battlefield goes into the moved target, or
            // into the shifted target for the fog's greyed picture, which is
            // resolved into the moved target.
            for (const auto& batch : full.frame.batches)
                if (batch.target != full.moved_target &&
                    (!shifted_apart || batch.target != full.shifted_target))
                    fail(
                        "a draw of the battlefield at zoom " + zoom_text(zoom) +
                        " went past the moved target"
                    );
            // The target's picture lands the shift past its corner: in the
            // shifted target at its texels, which lands a pixel before the
            // battlefield's corner with no shift; or, with no shifted target,
            // there itself. The map's corner lies with it where the view's
            // exact place puts it.
            const auto& moved_by = full.moved_frame.batches.front();
            const auto& onto = full.moved_frame.batches.back();
            const auto shift = accelerated_.frame_shift;
            if (shifted_apart &&
                (moved_by.source != full.moved_target || moved_by.target != full.shifted_target ||
                 moved_by.destination.x != 0 || moved_by.destination.y != 0 ||
                 onto.source != full.shifted_target || onto.shift_x != 0.0F ||
                 onto.shift_y != 0.0F))
                fail(
                    "the moved target was not moved at its texels into the shifted target and "
                    "reduced from it"
                );
            if (onto.destination.x != match_layout_.left - 1 ||
                onto.destination.y != match_layout_.top - 1 ||
                std::abs(static_cast<double>(moved_by.shift_x) - shift[0]) >
                    follow_place_tolerance ||
                std::abs(static_cast<double>(moved_by.shift_y) - shift[1]) > follow_place_tolerance)
                fail(
                    "the moved target did not land the frame's shift past the battlefield's corner"
                );
            const auto exact = match_view_place();
            const auto offset = accelerated_.frame_offset;
            const std::array<double, 2> drawn{
                static_cast<double>(full.frame_camera_x) + offset.x,
                static_cast<double>(full.frame_camera_y) + offset.y
            };
            for (std::size_t axis = 0; axis < 2; ++axis) {
                // Screen pixels from the battlefield's corner to the map's.
                const double ground =
                    -drawn[axis] * static_cast<double>(zoom) +
                    static_cast<double>(axis == 0 ? moved_by.shift_x : moved_by.shift_y);
                const double view = -exact[axis] * static_cast<double>(zoom);
                if (std::abs(ground - view) > follow_place_tolerance)
                    fail(
                        "frame " + std::to_string(frame) + " drew the ground " +
                        std::to_string(ground - view) +
                        " screen pixels from the view's exact place" +
                        (axis == 0 ? " across" : " down")
                    );
            }
            shifts_seen.insert(std::llround(shift[0] * follow_shift_grain));
            // The moved target's picture moves by whole pixels.
            auto moved = read_moved();
            if (!moved_before.rgb.empty()) {
                const auto [differing, compared] = unmatched(moved_before, moved);
                if (differing * most_moved_mismatch_share > compared)
                    fail(
                        "frame " + std::to_string(frame) + " at zoom " + zoom_text(zoom) +
                        " sampled the ground afresh: " + std::to_string(differing) + " of " +
                        std::to_string(compared) + " pixels match no whole-pixel move"
                    );
            }
            // What the player sees: the frame presented is the shifted
            // target reduced; on a card the shifted target is the moved
            // target moved by the shift at its texels, LINEAR, where SDL's
            // software renderer lands a texture on whole pixels only.
            const auto shifted = shifted_apart ? read_card_target(full.shifted_target) : moved;
            const auto seen = hold_presented_to_reduced(
                presented_frame, shifted, full.moved_target_factor, "frame " + std::to_string(frame)
            );
            presented_most = std::max(presented_most, seen.most);
            if (shifted_apart && !software) {
                const auto factor = static_cast<double>(full.moved_target_factor);
                renderer::Surface expected = shifted;
                wr::bilinear_rgb24(
                    {moved.rgb.data(), moved.width, moved.height, moved.width},
                    {1.0, 1.0, shift[0] * factor, shift[1] * factor},
                    {expected.rgb.data(), expected.width, expected.height, expected.width}
                );
                // The texels the move brings in at an edge are left out.
                const auto edge = static_cast<uint32_t>(full.moved_target_factor);
                int most = 0;
                for (uint32_t y = edge; y + edge < shifted.height; ++y)
                    for (uint32_t x = edge; x + edge < shifted.width; ++x)
                        for (std::size_t channel = 0; channel < 3; ++channel) {
                            const auto at = (std::size_t{y} * shifted.width + x) * 3U + channel;
                            most = std::max(
                                most, std::abs(int{shifted.rgb[at]} - int{expected.rgb[at]})
                            );
                        }
                if (most > most_card_difference)
                    fail(
                        "frame " + std::to_string(frame) +
                        ": the shifted target is not the moved target moved by the shift (most " +
                        std::to_string(most) + ")"
                    );
            }
            moved_before = std::move(moved);
            if (shift[0] != 0.0 || shift[1] != 0.0)
                ++glided;
            // The pointer finds the map point drawn under it: a quarter of
            // the way in, and at the middle, where the followed unit is.
            const auto camera = view_camera();
            for (const int share : {4, 2}) {
                const int x = match_layout_.left + match_layout_.battlefield_width() / share;
                const int y = match_layout_.top + match_layout_.battlefield_height() / share;
                const auto found = game_screen_point(static_cast<float>(x), static_cast<float>(y));
                const auto expected = [&](int at, int edge, std::size_t axis) {
                    return static_cast<int>(std::llround(
                        static_cast<double>(at - edge) / static_cast<double>(zoom) + exact[axis] -
                        static_cast<double>(camera[axis])
                    ));
                };
                namespace layout = oa::ui::display_layout;
                if (found.x != layout::kSourceLeft + expected(x, match_layout_.left, 0) ||
                    found.y != layout::kSourceTop + expected(y, match_layout_.top, 1))
                    fail(
                        "frame " + std::to_string(frame) +
                        ": the pointer found a map point other than the one drawn under it"
                    );
            }
            // The pointer over the followed unit, where it is drawn: its
            // place lifted by its height, on the camera's map pixel, moved by
            // the view's offset and the card's shift, finds it.
            if (frame + 1 == follow_frames) {
                const auto* followed = match_->world().slots[walker].unit;
                if (followed == nullptr)
                    fail("the followed unit is gone");
                const auto lifted =
                    project_match_point(live_viewport(camera[0], camera[1]), followed->position);
                const auto at = [&](int32_t projected, double past, double moved) {
                    return static_cast<float>(
                        static_cast<double>(projected) - past * static_cast<double>(zoom) + moved
                    );
                };
                update_pointer(at(lifted.x, offset.x, shift[0]), at(lifted.y, offset.y, shift[1]));
                if (hovered_match_unit_ != walker)
                    fail(
                        "the pointer over the followed unit at zoom " + zoom_text(zoom) +
                        " did not find it"
                    );
                // The cursor waits in the blank corner right of the bottom bar.
                update_pointer(
                    static_cast<float>(match_layout_.width - 1),
                    static_cast<float>(match_layout_.height - 1)
                );
            }
        }
        // The view glided between screen pixels rather than stepping.
        if (glided * 2 < follow_frames || shifts_seen.size() < follow_least_shifts)
            fail("the ground at zoom " + zoom_text(zoom) + " did not move by fractions of a pixel");
        stop_match_tracking();
        std::cout << "render tiers check: following a walking unit at zoom " << zoom_text(zoom)
                  << ", " << follow_frames << " frames: the ground at the view's exact place "
                  << "every frame, moved past the screen pixel it is drawn on by "
                  << shifts_seen.size() << " fractions, its picture moving by whole pixels; the "
                  << "zoomed-out target at factor " << full_->moved_target_factor
                  << (full_->shifted_target != card::TargetHandle{} ? ", moved at its texels"
                                                                    : ", moved in one draw")
                  << "; every frame presented within " << presented_most
                  << " levels of the target reduced\n";
    }

    // A zoom above 1 that is not whole: level 0 through the target at the
    // next whole number, drawn LINEAR to the window, the Basic tier's
    // sharp-bilinear; the terrain beside the card's own draws is held to
    // the reference of the level-0 view enlarged that many times and drawn
    // as the renderer draws a texture LINEAR, within the renderer's
    // tolerance. A renderer with the pixel-art sampling mode draws straight,
    // held to that filter's reference drawn as the card draws it, each tile
    // alone and clamped at its edges.
    {
        const float zoom = 1.37F;
        set_level(HardwareAcceleration::full);
        at_zoom(zoom);
        const auto read = full_frame();
        const auto& full = *full_;
        const auto& plan = full.plan;
        if (plan.pass_count != 1 || plan.passes[0].level != 0)
            fail("zoom " + zoom_text(zoom) + " was not drawn from level 0");
        write_png(picture("zoom-" + zoom_text(zoom)), read);
        const auto mask = card_mask(zoom, read);
        renderer::Surface reference = read;
        const int32_t cam_x = full.frame_camera_x;
        const int32_t cam_y = full.frame_camera_y;
        const Area field = battlefield();
        bool compared = true;
        std::string how;
        if (full.drawn_through_target) {
            const uint32_t factor = plan.target_zoom;
            const uint32_t view_w = full.target_width / factor;
            const uint32_t view_h = full.target_height / factor;
            std::vector<uint8_t> rgba(std::size_t{view_w} * view_h * 4U);
            if (gw::read_terrain_view(
                    reference_atlas,
                    0,
                    cam_x,
                    cam_y,
                    view_w,
                    view_h,
                    rgba.data(),
                    std::size_t{view_w} * 4U
                ) != gw::TerrainAtlasError::none)
                fail("the atlas refused the level-0 view at zoom " + zoom_text(zoom));
            std::vector<uint8_t> rgb(std::size_t{view_w} * view_h * 3U);
            for (std::size_t i = 0; i < std::size_t{view_w} * view_h; ++i)
                std::memcpy(&rgb[i * 3U], &rgba[i * 4U], 3);
            const SDL_Rect landed{
                field.x,
                field.y,
                static_cast<int>(std::lround(
                    static_cast<double>(full.target_width) * zoom / static_cast<double>(factor)
                )),
                static_cast<int>(std::lround(
                    static_cast<double>(full.target_height) * zoom / static_cast<double>(factor)
                ))
            };
            const wr::RgbSource source{rgb.data(), view_w, view_h, view_w};
            const wr::RgbTarget target{
                reference.rgb.data(), reference.width, reference.height, reference.width
            };
            if (software)
                software_linear_rgb24(source, factor, landed, target);
            else
                wr::sharp_bilinear_rgb24(
                    source,
                    {static_cast<double>(landed.w) / view_w,
                     static_cast<double>(landed.h) / view_h,
                     static_cast<double>(landed.x),
                     static_cast<double>(landed.y)},
                    factor,
                    target
                );
            how = "through the target at " + std::to_string(factor);
        } else if (plan.passes[0].sampling == card::Sampling::pixel_art) {
            // The card draws each tile by itself, from its own texels and
            // the gutter that repeats its edge, so the reference draws each
            // tile alone too, clamped at its edges, into the pixels whose
            // centres the tile's quad covers; past the map, the clear's
            // black.
            for (int y = field.y; y < field.y + field.h; ++y)
                std::memset(
                    reference.rgb.data() + (static_cast<std::size_t>(y) * reference.width +
                                            static_cast<std::size_t>(field.x)) *
                                               3U,
                    0,
                    static_cast<std::size_t>(field.w) * 3U
                );
            constexpr auto tile = static_cast<int32_t>(gw::tile_edge);
            const auto first_tile = [](int32_t pixel) {
                return pixel >= 0 ? pixel / tile : -((-pixel + tile - 1) / tile);
            };
            // The pixels whose centres a tile's span covers along one axis,
            // within the battlefield.
            const auto covered = [](double start, double span, int lowest, int end) {
                const int first = std::max(lowest, static_cast<int>(std::ceil(start - 0.5)));
                const int last = std::min(end, static_cast<int>(std::ceil(start + span - 0.5)));
                return std::pair{first, last};
            };
            const double span = static_cast<double>(tile) * zoom;
            std::vector<uint8_t> rgba(std::size_t{gw::tile_edge} * gw::tile_edge * 4U);
            std::vector<uint8_t> rgb(std::size_t{gw::tile_edge} * gw::tile_edge * 3U);
            const int32_t end_column = std::min(
                static_cast<int32_t>(reference_atlas.grid_width),
                first_tile(cam_x + static_cast<int32_t>(std::ceil(field.w / zoom))) + 1
            );
            const int32_t end_row = std::min(
                static_cast<int32_t>(reference_atlas.grid_height),
                first_tile(cam_y + static_cast<int32_t>(std::ceil(field.h / zoom))) + 1
            );
            for (int32_t row = std::max(0, first_tile(cam_y)); row < end_row; ++row)
                for (int32_t column = std::max(0, first_tile(cam_x)); column < end_column;
                     ++column) {
                    const double left =
                        field.x + (column * tile - cam_x) * static_cast<double>(zoom);
                    const double top = field.y + (row * tile - cam_y) * static_cast<double>(zoom);
                    const auto [x0, x1] = covered(left, span, field.x, field.x + field.w);
                    const auto [y0, y1] = covered(top, span, field.y, field.y + field.h);
                    if (x0 >= x1 || y0 >= y1)
                        continue;
                    if (gw::read_terrain_view(
                            reference_atlas,
                            0,
                            column * tile,
                            row * tile,
                            gw::tile_edge,
                            gw::tile_edge,
                            rgba.data(),
                            std::size_t{gw::tile_edge} * 4U
                        ) != gw::TerrainAtlasError::none)
                        fail("the atlas refused a level-0 tile at zoom " + zoom_text(zoom));
                    for (std::size_t i = 0; i < std::size_t{gw::tile_edge} * gw::tile_edge; ++i)
                        std::memcpy(&rgb[i * 3U], &rgba[i * 4U], 3);
                    wr::pixelart_rgb24(
                        {rgb.data(), gw::tile_edge, gw::tile_edge, gw::tile_edge},
                        {zoom, zoom, left - x0, top - y0},
                        {reference.rgb.data() + (static_cast<std::size_t>(y0) * reference.width +
                                                 static_cast<std::size_t>(x0)) *
                                                    3U,
                         static_cast<uint32_t>(x1 - x0),
                         static_cast<uint32_t>(y1 - y0),
                         reference.width}
                    );
                }
            how = "by the pixel-art sampling mode, tile by tile";
        } else {
            compared = false;
            how = "LINEAR straight, with no target";
        }
        if (compared) {
            const auto overlay = full.overlay;
            const auto bf_w = static_cast<uint32_t>(field.w);
            // The last column and row may read past the corner, which
            // renderers clamp differently.
            Difference difference;
            double sum = 0.0;
            std::size_t channels = 0;
            const Area pointer = cursor();
            for (int y = 0; y + 1 < field.h; ++y)
                for (int x = 0; x + 1 < field.w; ++x) {
                    const int sx = field.x + x;
                    const int sy = field.y + y;
                    if (sx >= pointer.x && sx < pointer.x + pointer.w && sy >= pointer.y &&
                        sy < pointer.y + pointer.h)
                        continue;
                    const auto cell = static_cast<std::size_t>(y) * bf_w + static_cast<uint32_t>(x);
                    const auto marked = static_cast<std::size_t>(sy) * read.width + sx;
                    if (overlay[cell * 4U + 3U] != 0 || mask[marked] != 0)
                        continue;
                    const auto at = marked * 3U;
                    ++difference.pixels;
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        const int delta = std::abs(
                            int{read.rgb[at + channel]} - int{reference.rgb[at + channel]}
                        );
                        difference.most = std::max(difference.most, delta);
                        sum += delta;
                        ++channels;
                    }
                }
            difference.mean = channels != 0 ? sum / static_cast<double>(channels) : 0.0;
            const int most_allowed = software ? most_software_difference : most_card_difference;
            std::cout << "render tiers check: full tier zoom " << zoom_text(zoom) << " " << how
                      << ": terrain of " << difference.pixels << " pixels: most " << difference.most
                      << ", mean " << difference.mean << "; " << cost() << '\n';
            if (difference.pixels == 0 || difference.most > most_allowed ||
                difference.mean > most_mean_scaled_difference) {
                write_png(picture("zoom-" + zoom_text(zoom) + "-reference"), reference);
                fail(
                    "the terrain at zoom " + zoom_text(zoom) + " strays from the card's reference"
                );
            }
        } else {
            std::cout << "render tiers check: full tier zoom " << zoom_text(zoom) << " drawn "
                      << how << ", which no reference holds; " << cost() << '\n';
        }
    }
    // At a chrome scale of 1.6 the HUD strips are drawn by the chrome's
    // filter from the HUD layer's prescale target, as Basic draws them.
    {
        set_level(HardwareAcceleration::full);
        resize(part_scale_width, part_scale_height);
        update_pointer(
            static_cast<float>(match_layout_.width - 1),
            static_cast<float>(match_layout_.height - 1)
        );
        at_zoom(1.0F);
        const auto read = full_frame();
        write_png(picture("hud-scale-1.6"), read);
        check_hud_strips(read, "the full tier at a chrome scale of 1.6");
        resize(whole_scale_width, whole_scale_height);
        update_pointer(
            static_cast<float>(match_layout_.width - 1),
            static_cast<float>(match_layout_.height - 1)
        );
    }
    // Anti-aliasing: the Enhanced anti-aliasing row's level chooses the
    // world target's factor, 2x giving 2 and 4x 4, within the budget S and
    // the texture limit at this battlefield; the processor draws no unit
    // finer in Full; and from zoom 1 up the battlefield under a transparent
    // overlay and beside the painters' quads equals the target read back
    // and reduced on the processor by the factor's halvings, as the card
    // reduces it, with the terrain, the fog and the stages in the target.
    // Below zoom 1 no world target is drawn: the battlefield goes through
    // the zoomed-out target, at twice the display's density. With the row
    // off again the target is freed and the frame drawn straight.
    {
        using oa::present::model::UnitSupersampling;
        const auto level_before = unit_supersampling_;
        const Area field = battlefield();
        const auto bf_w = static_cast<uint32_t>(field.w);
        const auto bf_h = static_cast<uint32_t>(field.h);
        const Area pointer = cursor();
        // One case: a Full frame at a zoom with the row at a level, held to
        // the target reduced on the processor; false when the budget allows
        // no world target here, which skips the level.
        const auto hold_reduced = [&](UnitSupersampling level, float zoom) {
            unit_supersampling_ = level;
            const uint32_t asked = oa::present::model::supersampling_factor(level);
            at_zoom(zoom);
            const auto read = full_frame();
            const auto& full = *full_;
            const auto& plan = full.drawn_plan;
            const uint32_t expected = policy::fit_supersample_factor(
                asked,
                full_supersampling::rounded_up(bf_w, full_supersampling::target_grain),
                full_supersampling::rounded_up(bf_h, full_supersampling::target_grain),
                full.supersample_budget,
                render_texture_limit()
            );
            if (full.supersample != expected || plan.factor != expected)
                fail(
                    "anti-aliasing " + std::to_string(asked) + "x drew at factor " +
                    std::to_string(plan.factor) + ", not the " + std::to_string(expected) +
                    " the budget allows"
                );
            if (plan.factor == 1) {
                std::cout << "render tiers check: full tier anti-aliasing " << asked
                          << "x: the budget of " << full.supersample_budget * 4 / (1024 * 1024)
                          << " MiB allows no world target at " << bf_w << 'x' << bf_h
                          << "; the cases are skipped\n";
                return false;
            }
            for (const auto& model : match_models().draws.models)
                if (model.plan.level != UnitSupersampling::off)
                    fail("a unit was drawn finer on the processor in a full frame");
            if (full.world_target == card::TargetHandle{} ||
                full.world_target_factor != plan.factor)
                fail("the world target is not alive at the factor drawn");
            if (full.sprites.sprites == 0)
                fail(
                    "the sprite stage drew nothing into the world target at zoom " + zoom_text(zoom)
                );
            // The world target read back, the texture's size.
            SDL_Texture* texture = full.executor.target_texture(full.world_target);
            if (texture == nullptr || !SDL_SetRenderTarget(sdl_.renderer, texture))
                fail(std::string("SDL_SetRenderTarget of the world target: ") + SDL_GetError());
            SDL_Surface* read_back = SDL_RenderReadPixels(sdl_.renderer, nullptr);
            const bool back = SDL_SetRenderTarget(sdl_.renderer, nullptr);
            SDL_Surface* texels = read_back != nullptr
                                      ? SDL_ConvertSurface(read_back, SDL_PIXELFORMAT_RGB24)
                                      : nullptr;
            SDL_DestroySurface(read_back);
            if (texels == nullptr || !back)
                fail(std::string("reading the world target back: ") + SDL_GetError());
            const uint32_t texture_width = full.world_target_width * plan.factor;
            const uint32_t texture_height = full.world_target_height * plan.factor;
            if (static_cast<uint32_t>(texels->w) != texture_width ||
                static_cast<uint32_t>(texels->h) != texture_height) {
                SDL_DestroySurface(texels);
                fail("the world target read back is not the texture's size");
            }
            std::vector<uint8_t> texture_rgb(std::size_t{texture_width} * texture_height * 3U);
            for (uint32_t y = 0; y < texture_height; ++y)
                std::memcpy(
                    &texture_rgb[std::size_t{y} * texture_width * 3U],
                    static_cast<const uint8_t*>(texels->pixels) +
                        static_cast<std::ptrdiff_t>(y) * texels->pitch,
                    std::size_t{texture_width} * 3U
                );
            SDL_DestroySurface(texels);
            // Reduced on the processor as the card reduces it, and held to
            // the battlefield under transparent overlays, the cursor, the
            // painters' quads, which the card draws over the reduced picture,
            // and the last column and row left out.
            const auto reduced = reduce_world_target_reference(
                texture_rgb, texture_width, texture_height, plan, software
            );
            const auto reduced_width = static_cast<uint32_t>(plan.destination.width);
            const auto overlay = full.overlay;
            std::vector<uint8_t> under_quads(std::size_t{bf_w} * bf_h, 0);
            for (const FullWorldQuad& quad : full.world_quads)
                mark_rect(under_quads, bf_w, bf_h, quad.x, quad.y, quad.width, quad.height, 0);
            Difference difference;
            double sum = 0.0;
            std::size_t channels = 0;
            for (int y = 0; y + 1 < field.h; ++y)
                for (int x = 0; x + 1 < field.w; ++x) {
                    const int sx = field.x + x;
                    const int sy = field.y + y;
                    if (sx >= pointer.x && sx < pointer.x + pointer.w && sy >= pointer.y &&
                        sy < pointer.y + pointer.h)
                        continue;
                    const auto cell = static_cast<std::size_t>(y) * bf_w + static_cast<uint32_t>(x);
                    if (overlay[cell * 4U + 3U] != 0 || under_quads[cell] != 0)
                        continue;
                    const auto at = (static_cast<std::size_t>(sy) * read.width + sx) * 3U;
                    const auto expected_at =
                        (static_cast<std::size_t>(y) * reduced_width + static_cast<uint32_t>(x)) *
                        3U;
                    ++difference.pixels;
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        const int delta = std::abs(
                            int{read.rgb[at + channel]} - int{reduced[expected_at + channel]}
                        );
                        difference.most = std::max(difference.most, delta);
                        sum += delta;
                        ++channels;
                    }
                }
            difference.mean = channels != 0 ? sum / static_cast<double>(channels) : 0.0;
            const int most_allowed = software ? most_software_difference : most_card_difference;
            const double most_mean_allowed = most_mean_scaled_difference;
            const std::string name = "aa-" + std::to_string(asked) + "x-zoom-" + zoom_text(zoom);
            std::cout << "render tiers check: full tier anti-aliasing " << asked << "x at zoom "
                      << zoom_text(zoom) << ": factor " << plan.factor << ", the world target of "
                      << full.world_target_width << 'x' << full.world_target_height << " ("
                      << texture_width << 'x' << texture_height << " texels) and its half hold "
                      << full.world_target_bytes / (1024 * 1024) << " MiB within the budget of "
                      << full.supersample_budget * 4 / (1024 * 1024) << " MiB; the battlefield of "
                      << difference.pixels << " pixels against the target reduced "
                      << "by halving on the processor: most " << difference.most << ", mean "
                      << difference.mean << "; " << cost() << '\n';
            write_png(picture(name), read);
            if (full.world_target_bytes > full.supersample_budget * card::texel_bytes)
                fail("the world target exceeds the budget");
            if (difference.pixels == 0 || difference.most > most_allowed ||
                difference.mean > most_mean_allowed) {
                renderer::Surface reference{
                    reduced_width, static_cast<uint32_t>(plan.destination.height), {}
                };
                reference.rgb = reduced;
                write_png(picture(name + "-reduced"), reference);
                fail(
                    "anti-aliasing " + std::to_string(asked) + "x at zoom " + zoom_text(zoom) +
                    " strays from the target reduced on the processor"
                );
            }
            return true;
        };
        bool budget_allows = true;
        for (const auto level : {UnitSupersampling::x2, UnitSupersampling::x4}) {
            for (const float zoom : {1.0F, 1.37F, 2.0F})
                if (!hold_reduced(level, zoom)) {
                    budget_allows = false;
                    break;
                }
            if (!budget_allows)
                break;
            // Below zoom 1 the frame is drawn into the zoomed-out target and
            // nothing into the world target.
            unit_supersampling_ = level;
            at_zoom(kMinBattlefieldZoom);
            std::ignore = full_frame();
            const auto& full = *full_;
            if (full.drawn_plan.factor != 1 || full.drawn_through_target ||
                full.moved_target == card::TargetHandle{})
                fail(
                    "anti-aliasing " +
                    std::to_string(oa::present::model::supersampling_factor(level)) + "x at zoom " +
                    zoom_text(kMinBattlefieldZoom) + " was not drawn through the zoomed-out target"
                );
            for (const auto& batch : full.frame.batches)
                if (full.world_target != card::TargetHandle{} && batch.target == full.world_target)
                    fail(
                        "anti-aliasing at zoom " + zoom_text(kMinBattlefieldZoom) +
                        " drew into the world target"
                    );
        }
        // Off again: the target is freed and the frame drawn straight.
        unit_supersampling_ = level_before;
        at_zoom(1.0F);
        std::ignore = full_frame();
        if (full_->world_target != card::TargetHandle{} || full_->drawn_plan.factor != 1 ||
            full_->supersample != 1)
            fail("the world target was kept with anti-aliasing off");
        std::cout << "render tiers check: full tier: with anti-aliasing off again the world target "
                     "is freed and the frame drawn straight\n";
    }
    // A view between map pixels: at zoom 4, half a map pixel on, the card
    // draws the battlefield two screen pixels further left, the terrain, the
    // fog, the stages and the painters together.
    {
        constexpr float between_zoom = 4.0F;
        constexpr double between_offset = 0.5;
        constexpr int shifted = 2;
        at_zoom(between_zoom);
        const auto on_pixel = full_frame();
        scroll_match_view(1, 0, between_offset * static_cast<double>(between_zoom));
        const auto between = full_frame();
        if (accelerated_.frame_offset.x != between_offset)
            fail("a view half a map pixel on was not drawn between map pixels");
        const Area field = battlefield();
        const Area pointer = cursor();
        uint64_t compared = 0;
        uint64_t differing = 0;
        for (int y = field.y; y < field.y + field.h; ++y)
            for (int x = field.x; x + shifted < field.x + field.w; ++x) {
                if (x + shifted >= pointer.x && x < pointer.x + pointer.w && y >= pointer.y &&
                    y < pointer.y + pointer.h)
                    continue;
                const auto at =
                    (static_cast<std::size_t>(y) * between.width + static_cast<std::size_t>(x)) *
                    3U;
                const auto from = at + static_cast<std::size_t>(shifted) * 3U;
                ++compared;
                if (between.rgb[at] != on_pixel.rgb[from] ||
                    between.rgb[at + 1] != on_pixel.rgb[from + 1] ||
                    between.rgb[at + 2] != on_pixel.rgb[from + 2])
                    ++differing;
            }
        std::cout << "render tiers check: full tier: half a map pixel on at zoom 4, "
                  << compared - differing << " of " << compared
                  << " battlefield pixels are the frame before moved two pixels left\n";
        if (differing * most_moved_mismatch_share > compared) {
            write_png(picture("between-on-pixel"), on_pixel);
            write_png(picture("between"), between);
            fail("a view half a map pixel on was not the picture moved two pixels left");
        }
    }
    std::cout
        << "render tiers check: the full tier drew the battlefield on the card at every zoom, "
        << full_->frames << " frames, with the box filter never run; pictures in "
        << report_directory.string() << '\n';

    // The fog, the overlay canvas, the kill board and the +stats panel.
    check_full_overlays(set_level, at_zoom, presented, composed, report_directory);
    set_level(HardwareAcceleration::off);
}

namespace {

namespace model_render = oa::present::model;

/// A colour no entry of a palette has, for the processor's undrawn pixels,
/// whose nearest entry a shadow darkens to another entry, so that a shadow
/// over it shows: over a colour that darkens to itself the bridge writes
/// nothing back.
std::array<uint8_t, 3>
colour_outside(const oa::Palette& palette, const model_render::ModelDisplay& display) {
    for (uint32_t candidate = 0xc1c2c3U;; candidate += 0x30507U) {
        const std::array<uint8_t, 3> colour{
            static_cast<uint8_t>(candidate >> 16),
            static_cast<uint8_t>(candidate >> 8),
            static_cast<uint8_t>(candidate)
        };
        bool used = false;
        uint8_t nearest = 0;
        int64_t least = INT64_MAX;
        for (std::size_t i = 0; i < OA_PALETTE_COLORS; ++i) {
            const auto& entry = palette.entries[i];
            if (entry.r == colour[0] && entry.g == colour[1] && entry.b == colour[2])
                used = true;
            const int64_t dr = int64_t{entry.r} - colour[0];
            const int64_t dg = int64_t{entry.g} - colour[1];
            const int64_t db = int64_t{entry.b} - colour[2];
            const int64_t distance = dr * dr + dg * dg + db * db;
            if (distance < least) {
                least = distance;
                nearest = static_cast<uint8_t>(i);
            }
        }
        if (!used && display.alpha.size() > nearest && display.alpha[nearest] != nearest)
            return colour;
    }
}

/// The draw kinds the model stage draws, which the processor's raster of
/// the models alone keeps, with the commits that write the bridge back.
bool model_kind(WorldDrawKind kind) noexcept {
    switch (kind) {
    case WorldDrawKind::model:
    case WorldDrawKind::projectile:
    case WorldDrawKind::debris:
    case WorldDrawKind::fragment:
    case WorldDrawKind::commit:
    case WorldDrawKind::commit_always:
        return true;
    case WorldDrawKind::pixel_square:
    case WorldDrawKind::sprite:
    case WorldDrawKind::blended_sprite:
    case WorldDrawKind::lit_sprite:
    case WorldDrawKind::line:
    case WorldDrawKind::selection_line:
    case WorldDrawKind::lens:
        return false;
    }
    return false;
}

/// The bounds a frame of the fight is held to against the band raster,
/// which samples each pixel at its corner where the card samples its
/// centre: the coverage within the model raster bounds, and the colours
/// within the share the installed game's unit models measured against the
/// band raster at zoom 1, with room (app-full-models-data prints it).
constexpr double most_corner_sampled_far_share = 0.6;

} // namespace

void Runtime::check_full_models(const fs::path& report_directory) {
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("render tiers check: " + what);
    };
    MatchModels& models = match_models();
    const uint32_t bf_w = match_world_cpu_.width;
    const uint32_t bf_h = match_world_cpu_.height;
    if (bf_w == 0 || bf_h == 0 || accelerated_.frame.method != SceneMethod::none ||
        models.bridge.scale != 1.0F)
        fail("the model stage case needs the battlefield drawn at zoom 1");
    // The card's frame of the list's models, into a transparent target the
    // battlefield's size on the game's renderer.
    card::Executor executor;
    if (!executor.open(sdl_.renderer, render_texture_limit()))
        fail("the card executor cannot open on the renderer: " + executor.error());
    full::ModelStage stage;
    stage.set_palette(models.display.palette, 1.0F);
    oa::formats::gaf::RenderedFrame shadow_sprite;
    const oa::Sprite& shadow = models.projectile_shadow;
    if (shadow.data != nullptr && shadow.width != 0 && shadow.height != 0) {
        shadow_sprite.width = shadow.width;
        shadow_sprite.height = shadow.height;
        shadow_sprite.origin_x = shadow.origin_x;
        shadow_sprite.origin_y = shadow.origin_y;
        shadow_sprite.transparency_index = shadow.key;
        const auto size = static_cast<std::size_t>(shadow.width) * shadow.height;
        const auto* pixels = static_cast<const uint8_t*>(shadow.data);
        shadow_sprite.pixels.assign(pixels, pixels + size);
        shadow_sprite.coverage.resize(size);
        for (std::size_t i = 0; i < size; ++i)
            shadow_sprite.coverage[i] = pixels[i] != shadow.key ? 1 : 0;
    }
    const oa::World& world = match_->world().record;
    full::ModelFrameInputs inputs;
    inputs.draws = &models.draws;
    inputs.world = &world;
    inputs.library = &models.library;
    inputs.display = &models.display;
    inputs.graphics_flags = models.renderer.graphics_flags;
    inputs.build_pulse_tick = models.renderer.tick - models.renderer.build_pulse_lag;
    std::copy_n(models.renderer.team_colors, full::team_colour_players, inputs.team_colors.begin());
    inputs.light = {models.renderer.light[0], models.renderer.light[1], models.renderer.light[2]};
    inputs.light_scale = models.renderer.light_scale;
    inputs.projectile_shadow = shadow_sprite.width != 0 ? &shadow_sprite : nullptr;
    full::SceneView view;
    view.zoom = 1.0F;
    view.camera_x = models.renderer.camera_x;
    view.camera_y = models.renderer.camera_y;
    view.width = static_cast<int32_t>(bf_w);
    view.height = static_cast<int32_t>(bf_h);
    SDL_Texture* target = SDL_CreateTexture(
        sdl_.renderer,
        SDL_PIXELFORMAT_RGBA32,
        SDL_TEXTUREACCESS_TARGET,
        static_cast<int>(bf_w),
        static_cast<int>(bf_h)
    );
    if (target == nullptr)
        fail(std::string("SDL_CreateTexture for the model stage's target: ") + SDL_GetError());

    struct TargetGuard {
        SDL_Texture* texture{};

        ~TargetGuard() { SDL_DestroyTexture(texture); }
    } guard{target};

    if (!SDL_SetRenderTarget(sdl_.renderer, target) ||
        !SDL_SetRenderDrawBlendMode(sdl_.renderer, SDL_BLENDMODE_NONE) ||
        !SDL_SetRenderDrawColor(sdl_.renderer, 0, 0, 0, 0) || !SDL_RenderClear(sdl_.renderer) ||
        !SDL_SetRenderTarget(sdl_.renderer, nullptr))
        fail(std::string("clearing the model stage's target: ") + SDL_GetError());
    card::CardFrame frame;
    stage.emit_frame(inputs, view, executor, frame);
    if (!stage.error().empty())
        fail("the model stage: " + stage.error());
    if (!stage.upload(executor))
        fail("the model stage's upload: " + stage.error());
    if (!executor.execute(frame, target))
        fail("the card refused the frame of models: " + executor.error());
    std::vector<uint8_t> rgba(std::size_t{bf_w} * bf_h * 4U);
    {
        if (!SDL_SetRenderTarget(sdl_.renderer, target))
            fail(
                std::string("SDL_SetRenderTarget to read the model stage's target: ") +
                SDL_GetError()
            );
        SDL_Surface* read = SDL_RenderReadPixels(sdl_.renderer, nullptr);
        std::ignore = SDL_SetRenderTarget(sdl_.renderer, nullptr);
        if (read == nullptr)
            fail(
                std::string("SDL_RenderReadPixels of the model stage's target: ") + SDL_GetError()
            );
        SDL_Surface* converted = SDL_ConvertSurface(read, SDL_PIXELFORMAT_RGBA32);
        SDL_DestroySurface(read);
        if (converted == nullptr)
            fail(std::string("SDL_ConvertSurface of the model stage's target: ") + SDL_GetError());
        for (uint32_t y = 0; y < bf_h; ++y)
            std::memcpy(
                rgba.data() + std::size_t{y} * bf_w * 4U,
                static_cast<const uint8_t*>(converted->pixels) +
                    std::size_t{y} * static_cast<std::size_t>(converted->pitch),
                std::size_t{bf_w} * 4U
            );
        SDL_DestroySurface(converted);
    }
    const auto counts = stage.counts();
    stage.close(executor);
    // The processor's raster of the same list's models alone, through a
    // model bridge of its own as a band of the frame draws them.
    WorldDrawList models_only = models.draws;
    std::erase_if(models_only.draws, [](const WorldDraw& draw) { return !model_kind(draw.kind); });
    add_world_draw(models_only, WorldDrawKind::commit_always, 0);
    const std::array<uint8_t, 3> key = colour_outside(models.display.palette, models.display);
    renderer::Surface raster{bf_w, bf_h, std::vector<uint8_t>(std::size_t{bf_w} * bf_h * 3U)};
    for (std::size_t i = 0; i < raster.rgb.size(); i += 3) {
        raster.rgb[i] = key[0];
        raster.rgb[i + 1] = key[1];
        raster.rgb[i + 2] = key[2];
    }
    model_render::RgbBridge bridge;
    model_render::bridge_begin(
        bridge,
        {raster.rgb.data(),
         static_cast<int32_t>(bf_w),
         static_cast<int32_t>(bf_h),
         static_cast<int32_t>(bf_w) * 3},
        models.bridge.area,
        1.0F,
        models.display.palette
    );
    std::vector<model_render::BridgeBand> bands;
    model_render::bridge_split(bridge, 1, bands);
    WorldFrameDraw frame_draw{};
    frame_draw.target = {
        raster.rgb.data(),
        static_cast<int32_t>(bf_w),
        static_cast<int32_t>(bf_h),
        0,
        0,
        static_cast<int32_t>(bf_w),
        static_cast<int32_t>(bf_h),
        0,
        static_cast<int32_t>(bf_h)
    };
    frame_draw.palette = &match_palette_;
    frame_draw.scale = 1.0F;
    frame_draw.bridge = &bridge;
    frame_draw.display = &models.display;
    frame_draw.projectile_shadow = &models.projectile_shadow;
    frame_draw.debris_view = {0, 0, static_cast<int32_t>(bf_w) - 1, static_cast<int32_t>(bf_h) - 1};
    draw_world_band(
        models_only,
        frame_draw,
        bands.front(),
        models.renderer,
        models.supersample,
        models.debris_points
    );
    model_render::bridge_join_band(bridge, bands.front());
    const auto comparison =
        full::compare_model_rasters(rgba.data(), raster.rgb.data(), bf_w, bf_h, key);
    full::ModelRasterBounds bounds;
    bounds.far_share = most_corner_sampled_far_share;
    std::cout << "render tiers check: the model stage drew " << counts.units << " units, "
              << counts.projectiles << " projectiles, " << counts.debris << " debris pieces, "
              << counts.fragments << " fragments and " << counts.shadows << " shadows as "
              << counts.polygons << " polygons in " << frame.batches.size() << " batches; against "
              << "the processor's raster: " << comparison.drawn << " pixels drawn, "
              << comparison.same << " the same, " << comparison.phase << " a texel of phase, "
              << comparison.edge_colour << " another colour at an edge, " << comparison.far
              << " far, " << comparison.edge_coverage << " covered by one within a pixel of the "
              << "other, " << comparison.far_coverage << " covered by one alone, "
              << comparison.shadowed << " shadowed\n";
    if (counts.units == 0 || comparison.drawn == 0)
        fail("the model stage drew no unit of the frame");
    if (!full::within_bounds(comparison, bounds)) {
        // The card's picture over the key colour, so that the two compare.
        renderer::Surface card_picture{bf_w, bf_h, std::vector<uint8_t>(raster.rgb.size())};
        for (std::size_t i = 0; i < std::size_t{bf_w} * bf_h; ++i) {
            const uint8_t* pixel = rgba.data() + i * 4U;
            const unsigned left = 255U - pixel[3];
            for (std::size_t channel = 0; channel < 3; ++channel)
                card_picture.rgb[i * 3U + channel] =
                    static_cast<uint8_t>(pixel[channel] + (key[channel] * left) / 255U);
        }
        write_png(report_directory / "native-render-tiers-models-card.png", card_picture);
        write_png(report_directory / "native-render-tiers-models-processor.png", raster);
        // The first pixels one picture alone draws with nothing of the other
        // within a pixel, each with its surroundings, for the report.
        const auto by_card = [&](int x, int y) {
            return x >= 0 && y >= 0 && x < static_cast<int>(bf_w) && y < static_cast<int>(bf_h) &&
                   rgba
                           [(std::size_t{static_cast<uint32_t>(y)} * bf_w +
                             static_cast<uint32_t>(x)) *
                                4U +
                            3U] != 0;
        };
        const auto by_processor = [&](int x, int y) {
            if (x < 0 || y < 0 || x >= static_cast<int>(bf_w) || y >= static_cast<int>(bf_h))
                return false;
            const uint8_t* pixel =
                raster.rgb.data() +
                (std::size_t{static_cast<uint32_t>(y)} * bf_w + static_cast<uint32_t>(x)) * 3U;
            return pixel[0] != key[0] || pixel[1] != key[1] || pixel[2] != key[2];
        };
        const auto alone = [&](int x, int y) {
            const bool card = by_card(x, y);
            const bool processor = by_processor(x, y);
            if (card == processor)
                return false;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    if (card ? by_processor(x + dx, y + dy) : by_card(x + dx, y + dy))
                        return false;
            return true;
        };
        constexpr int most_listed = 6;
        constexpr int reach = 10;
        int listed = 0;
        int skip_until_y = -1;
        for (int y = 0; y < static_cast<int>(bf_h) && listed < most_listed; ++y) {
            if (y < skip_until_y)
                continue;
            for (int x = 0; x < static_cast<int>(bf_w) && listed < most_listed; ++x) {
                if (!alone(x, y))
                    continue;
                std::cout << "render tiers check: (" << x << ", " << y << ") drawn by the "
                          << (by_card(x, y) ? "card" : "processor") << " alone; around it:\n";
                for (int row = y - reach; row <= y + reach; ++row) {
                    std::cout << "  card:";
                    for (int column = x - reach; column <= x + reach; ++column)
                        std::cout << (by_card(column, row) ? '#' : '.');
                    std::cout << "  processor:";
                    for (int column = x - reach; column <= x + reach; ++column)
                        std::cout << (by_processor(column, row) ? '#' : '.');
                    std::cout << '\n';
                }
                ++listed;
                skip_until_y = y + 2 * reach;
                break;
            }
        }
        fail("the model stage's frame strays from the processor's raster of the models");
    }
}

void Runtime::check_full_overlays(
    const std::function<void(oa::ui::engine_settings::HardwareAcceleration)>& set_level,
    const std::function<void(float)>& at_zoom,
    const std::function<renderer::Surface()>& presented,
    const std::function<renderer::Surface()>& composed,
    const fs::path& report_directory
) {
    using oa::ui::engine_settings::HardwareAcceleration;
    namespace visibility_flag = oa::ui::console::visibility_flag;
    namespace hud = oa::ui::hud;
    namespace panel = frame_stats_panel;
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("render tiers check: full tier: " + what);
    };
    const auto battlefield = [&]() {
        return Area{
            match_layout_.left,
            match_layout_.top,
            match_layout_.battlefield_width(),
            match_layout_.battlefield_height()
        };
    };
    const auto cursor = [&]() {
        return Area{
            static_cast<int>(match_pointer_x_) - cursor_reach,
            static_cast<int>(match_pointer_y_) - cursor_reach,
            2 * cursor_reach,
            2 * cursor_reach
        };
    };
    const auto picture = [&](const std::string& name) {
        return report_directory / ("native-render-tiers-full-" + name + ".png");
    };
    const auto inside = [](const Area& area, int x, int y) {
        return x >= area.x && x < area.x + area.w && y >= area.y && y < area.y + area.h;
    };
    const auto pixel = [](const renderer::Surface& frame, int x, int y) {
        return frame.rgb.data() + (static_cast<std::size_t>(y) * frame.width + x) * 3U;
    };
    const auto same = [](const uint8_t* a, const uint8_t* b) { return std::memcmp(a, b, 3) == 0; };
    // The exact comparisons read the gray table on the frames' bytes, which
    // are the palette's only at a display gamma of 1.
    if (!gamma_identity_)
        fail("the fog cases need a display gamma of 1");
    const auto full_frame = [&]() {
        auto frame = presented();
        if (!full_presentation() || !full_ || !full_->drawn || !full_frame_drawn())
            fail("the frame at zoom " + zoom_text(match_zoom()) + " was not drawn by the card");
        return frame;
    };
    const auto standard = [&]() {
        ensure_screen_world();
        return composed();
    };
    auto& game = match_->state().game;
    const uint8_t saved_visibility = game.visibility_flags;
    const uint16_t saved_graphics = game.graphics_flags;
    const auto set_fog = [&](bool line_of_sight, bool mapping, bool dithered) {
        auto visibility = static_cast<uint8_t>(
            saved_visibility & ~(visibility_flag::mapping | visibility_flag::line_of_sight)
        );
        if (line_of_sight)
            visibility = static_cast<uint8_t>(visibility | visibility_flag::line_of_sight);
        if (mapping)
            visibility = static_cast<uint8_t>(visibility | visibility_flag::mapping);
        game.visibility_flags = visibility;
        game.graphics_flags = static_cast<uint16_t>(
            dithered ? saved_graphics | init::preference_flags::dithered_fog
                     : saved_graphics & ~init::preference_flags::dithered_fog
        );
        reset_sight_presentation(false);
    };
    const auto restore_fog = [&]() {
        game.visibility_flags = saved_visibility;
        game.graphics_flags = saved_graphics;
        reset_sight_presentation(false);
    };
    // The gray table's colour of a frame's pixel, as the fog grays it.
    ensure_fog_frames();
    const auto greyed = [&](const uint8_t* rgb) {
        const auto level = (static_cast<unsigned>(rgb[0]) + rgb[1] + rgb[2]) / 3U;
        return fog_shading_.gray_levels[level];
    };

    // Where a fog tile lands on the frame at the zoom, from the grid the
    // last Full frame drew its fog from.
    struct TileArea {
        Area area{};
        wr::FogTile tile{};
    };

    const auto fog_tiles = [&](float zoom) {
        std::vector<TileArea> tiles;
        const auto& fog = full_->fog;
        if (fog.grid.tiles.empty())
            return tiles;
        const Area field = battlefield();
        const double span = wr::fog_cell_pixels * static_cast<double>(zoom);
        for (int32_t row = 0; row < fog.grid.height; ++row)
            for (int32_t column = 0; column < fog.grid.width; ++column) {
                const double x = field.x + (fog.grid.offset_x + column * wr::fog_cell_pixels) *
                                               static_cast<double>(zoom);
                const double y = field.y + (fog.grid.offset_z + row * wr::fog_cell_pixels) *
                                               static_cast<double>(zoom);
                // The pixels whose centres the tile covers.
                const int x0 = static_cast<int>(std::ceil(x - 0.5));
                const int y0 = static_cast<int>(std::ceil(y - 0.5));
                const int x1 = static_cast<int>(std::ceil(x + span - 0.5));
                const int y1 = static_cast<int>(std::ceil(y + span - 0.5));
                tiles.push_back({{x0, y0, x1 - x0, y1 - y0}, fog.grid.at(column, row)});
            }
        return tiles;
    };

    // The pixels the fog cases pass over, where the design accepts that the
    // card's fog differs from the processor's: a sprite is wholly greyed or
    // wholly in colour by the cell under its point, where the processor
    // greys its pixels through the mask, so a sprite under tiles of more
    // than one state is passed over; a blended sprite over a fogged tile,
    // whose blend the processor's gray table reads; and every model,
    // projectile, debris piece, fragment, line and square, which the card
    // draws in colour where the processor greys them, and which the frames
    // between two cases may move. Each is grown by a pixel for the edges of
    // its raster. Also passed over: what the painters after the fog lay
    // over everything, the same over every fog in both tiers, which are the
    // overlay canvas's painted pixels, such as the clock line and the
    // resource panel of a profile's visual rules, and the quads they ask the
    // card for.
    enum class TileState : uint8_t { clear, unseen, unmapped, edge };
    // A pixel of the mask under the painters' quads and nothing else of it.
    constexpr uint8_t quads_alone = 2;
    const auto object_mask = [&](float zoom, const renderer::Surface& frame) {
        const auto& list = match_models().draws;
        const Area field = battlefield();
        const int reach = static_cast<int>(std::ceil(std::max(1.0F, zoom)));
        const auto scaled = [&](int32_t pixels) {
            return static_cast<int>(
                std::lround(static_cast<double>(pixels) * static_cast<double>(zoom))
            );
        };
        std::vector<uint8_t> mask(std::size_t{frame.width} * frame.height, 0);
        std::vector<uint8_t> states(mask.size(), static_cast<uint8_t>(TileState::clear));
        for (const auto& tile : fog_tiles(zoom)) {
            TileState state = TileState::clear;
            if (tile.tile.unmapped == wr::fog_mask_full)
                state = TileState::unmapped;
            else if (tile.tile.unseen == wr::fog_mask_full && tile.tile.unmapped == 0)
                state = TileState::unseen;
            else if (tile.tile.unseen != 0 || tile.tile.unmapped != 0)
                state = TileState::edge;
            if (state == TileState::clear)
                continue;
            for (int y = std::max(tile.area.y, 0);
                 y < std::min(tile.area.y + tile.area.h, static_cast<int>(frame.height));
                 ++y)
                for (int x = std::max(tile.area.x, 0);
                     x < std::min(tile.area.x + tile.area.w, static_cast<int>(frame.width));
                     ++x)
                    states
                        [static_cast<std::size_t>(y) * frame.width + static_cast<std::size_t>(x)] =
                            static_cast<uint8_t>(state);
        }
        // Whether a rectangle lies under tiles of more than one state, or
        // under an edge tile; and whether it touches any fogged tile.
        const auto states_under = [&](int x, int y, int w, int h, bool any_fogged) {
            std::optional<uint8_t> first;
            for (int row = std::max(y, 0); row < std::min(y + h, static_cast<int>(frame.height));
                 ++row)
                for (int column = std::max(x, 0);
                     column < std::min(x + w, static_cast<int>(frame.width));
                     ++column) {
                    const uint8_t state = states
                        [static_cast<std::size_t>(row) * frame.width +
                         static_cast<std::size_t>(column)];
                    if (any_fogged) {
                        if (state != static_cast<uint8_t>(TileState::clear))
                            return true;
                        continue;
                    }
                    if (state == static_cast<uint8_t>(TileState::edge))
                        return true;
                    if (!first)
                        first = state;
                    else if (*first != state)
                        return true;
                }
            return false;
        };
        const auto mark_region = [&](const oa::Rect32& region) {
            mark_rect(
                mask,
                frame.width,
                frame.height,
                field.x + scaled(region.x1),
                field.y + scaled(region.y1),
                scaled(region.x2 - region.x1 + 1),
                scaled(region.y2 - region.y1 + 1),
                reach
            );
        };
        for (const auto& draw : list.draws) {
            switch (draw.kind) {
            case WorldDrawKind::sprite:
            case WorldDrawKind::blended_sprite:
            case WorldDrawKind::lit_sprite: {
                const auto& sprite = list.sprites[draw.index];
                if (sprite.frame == nullptr)
                    break;
                const SceneRect rect = sprite_scene_rect(sprite, zoom);
                const int left = field.x + static_cast<int>(rect.left);
                const int top = field.y + static_cast<int>(rect.top);
                const int w = std::max(1, static_cast<int>(rect.right - rect.left));
                const int h = std::max(1, static_cast<int>(rect.bottom - rect.top));
                // A flash lights what is under it, which the processor's
                // fog then greys, as a blended sprite blends it.
                const bool blended = draw.kind != WorldDrawKind::sprite;
                if (states_under(
                        left - sprite_mask_margin,
                        top - sprite_mask_margin,
                        w + 2 * sprite_mask_margin,
                        h + 2 * sprite_mask_margin,
                        blended
                    ))
                    mark_rect(mask, frame.width, frame.height, left, top, w, h, sprite_mask_margin);
                break;
            }
            case WorldDrawKind::model:
                mark_region(list.models[draw.index].plan.region);
                break;
            case WorldDrawKind::projectile:
                mark_region(list.projectiles[draw.index].region);
                break;
            case WorldDrawKind::debris:
                mark_region(list.debris[draw.index].region);
                break;
            case WorldDrawKind::fragment:
                mark_region(list.fragments[draw.index].region);
                break;
            case WorldDrawKind::lens: {
                // The card draws no lens.
                const auto& centre = list.lenses[draw.index];
                const int side = std::max(1, scaled(projectile_lens_side));
                mark_rect(
                    mask,
                    frame.width,
                    frame.height,
                    field.x + centre.x - scaled(projectile_lens_side / 2),
                    field.y + centre.y - scaled(projectile_lens_side / 2),
                    side,
                    side,
                    reach
                );
                break;
            }
            case WorldDrawKind::line: {
                const auto& line = list.lines[draw.index];
                mark_line(
                    mask,
                    frame.width,
                    frame.height,
                    field.x + line.x0,
                    field.y + line.y0,
                    field.x + line.x1,
                    field.y + line.y1,
                    reach
                );
                break;
            }
            case WorldDrawKind::selection_line: {
                const auto& line = list.lines[draw.index];
                const auto at = [&](int32_t pixel) {
                    return static_cast<int>(std::lround((static_cast<double>(pixel) + 0.5) * zoom));
                };
                mark_line(
                    mask,
                    frame.width,
                    frame.height,
                    field.x + at(line.x0),
                    field.y + at(line.y0),
                    field.x + at(line.x1),
                    field.y + at(line.y1),
                    reach + 1
                );
                break;
            }
            case WorldDrawKind::pixel_square: {
                const auto& square = list.squares[draw.index];
                mark_rect(
                    mask,
                    frame.width,
                    frame.height,
                    field.x + square.left,
                    field.y + square.top,
                    square.right - square.left + 1,
                    square.bottom - square.top + 1,
                    reach
                );
                break;
            }
            case WorldDrawKind::commit:
            case WorldDrawKind::commit_always:
                break;
            }
        }
        if (full_frame_drawn()) {
            const auto key = full_overlay_key();
            const auto& canvas = match_world_cpu_;
            for (uint32_t y = 0; y < canvas.height; ++y)
                for (uint32_t x = 0; x < canvas.width; ++x) {
                    const uint8_t* painted =
                        canvas.rgb.data() + (std::size_t{y} * canvas.width + x) * 3U;
                    const int fx = field.x + static_cast<int>(x);
                    const int fy = field.y + static_cast<int>(y);
                    if (std::equal(key.begin(), key.end(), painted) || fx < 0 || fy < 0 ||
                        fx >= static_cast<int>(frame.width) || fy >= static_cast<int>(frame.height))
                        continue;
                    mask
                        [static_cast<std::size_t>(fy) * frame.width +
                         static_cast<std::size_t>(fx)] = 1;
                }
            // The quads alone, apart from the rest, for the case of the
            // black fog to hold them to what they ask over its black.
            for (const FullWorldQuad& quad : full_->world_quads)
                for (int y = std::max(field.y + quad.y, 0);
                     y < std::min(field.y + quad.y + quad.height, static_cast<int>(frame.height));
                     ++y)
                    for (int x = std::max(field.x + quad.x, 0);
                         x < std::min(field.x + quad.x + quad.width, static_cast<int>(frame.width));
                         ++x) {
                        auto& marked = mask
                            [static_cast<std::size_t>(y) * frame.width +
                             static_cast<std::size_t>(x)];
                        if (marked == 0)
                            marked = quads_alone;
                    }
        }
        return mask;
    };

    set_level(HardwareAcceleration::full);
    for (const float zoom : {1.0F, 2.0F}) {
        at_zoom(zoom);
        // No fog: the pictures everything else is held against.
        set_fog(false, false, false);
        const auto full_clear = full_frame();
        if (full_->drawn_fog_quads != 0)
            fail("a frame with the fog off drew fog quads at zoom " + zoom_text(zoom));
        const auto processor_clear = standard();
        // Line of sight alone: every cell out of sight is greyed, mapped or
        // not. Where the two tiers agree with the fog off, a pixel under a
        // tile wholly out of sight is the gray table's colour in both; a
        // pixel under a tile at an edge lies between its colour and its
        // grey.
        set_fog(true, false, false);
        const auto full_unseen = full_frame();
        if (full_->drawn_fog_quads == 0)
            fail("a frame with line of sight on drew no fog quads at zoom " + zoom_text(zoom));
        auto objects = object_mask(zoom, full_unseen);
        const auto processor_unseen = standard();
        write_png(picture("fog-unseen-zoom-" + zoom_text(zoom)), full_unseen);
        const Area field = battlefield();
        const Area pointer = cursor();
        const auto passed_over = [&](int x, int y) {
            return inside(pointer, x, y) ||
                   objects
                           [static_cast<std::size_t>(y) * full_unseen.width +
                            static_cast<std::size_t>(x)] != 0;
        };
        std::size_t interior = 0;
        std::size_t interior_differing = 0;
        std::size_t edge = 0;
        std::size_t edge_outside = 0;
        std::size_t clear = 0;
        std::size_t clear_differing = 0;
        // A view without a tile wholly out of sight shown in full, as a
        // small map's at a close zoom or a view in sight but for a sliver
        // of such a tile at its border, holds the edge tiles to their range
        // and the sliver to the processor's, without the fewest pixels.
        bool whole_in_view = false;
        const auto shown_in_full = [&](const Area& area) {
            return inside(field, area.x, area.y) &&
                   inside(field, area.x + area.w - 1, area.y + area.h - 1);
        };
        std::vector<uint8_t> under_tile(
            static_cast<std::size_t>(field.w) * static_cast<std::size_t>(field.h), 0
        );
        const auto under = [&](int x, int y) -> uint8_t& {
            return under_tile
                [static_cast<std::size_t>(y - field.y) * static_cast<std::size_t>(field.w) +
                 static_cast<std::size_t>(x - field.x)];
        };
        for (const auto& tile : fog_tiles(zoom)) {
            if (tile.tile.unseen == 0)
                continue;
            const bool whole = tile.tile.unseen == wr::fog_mask_full;
            whole_in_view = whole_in_view || (whole && shown_in_full(tile.area));
            for (int y = tile.area.y; y < tile.area.y + tile.area.h; ++y)
                for (int x = tile.area.x; x < tile.area.x + tile.area.w; ++x) {
                    if (!inside(field, x, y))
                        continue;
                    under(x, y) = 1;
                    if (passed_over(x, y))
                        continue;
                    const uint8_t* clear_full = pixel(full_clear, x, y);
                    const uint8_t* fogged = pixel(full_unseen, x, y);
                    if (whole) {
                        if (!same(clear_full, pixel(processor_clear, x, y)))
                            continue;
                        ++interior;
                        if (!same(fogged, pixel(processor_unseen, x, y)))
                            ++interior_differing;
                        continue;
                    }
                    ++edge;
                    const auto grey = greyed(clear_full);
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        const int low = std::min<int>(clear_full[channel], grey[channel]);
                        const int high = std::max<int>(clear_full[channel], grey[channel]);
                        if (fogged[channel] < low - most_blend_rounding ||
                            fogged[channel] > high + most_blend_rounding) {
                            ++edge_outside;
                            break;
                        }
                    }
                }
        }
        // Pixels under no fog tile with a corner out of sight are untouched.
        for (int y = field.y; y < field.y + field.h; ++y)
            for (int x = field.x; x < field.x + field.w; ++x) {
                if (under(x, y) != 0 || passed_over(x, y))
                    continue;
                ++clear;
                if (!same(pixel(full_clear, x, y), pixel(full_unseen, x, y)))
                    ++clear_differing;
            }
        std::cout << "render tiers check: full tier fog at zoom " << zoom_text(zoom) << ": "
                  << interior << " pixels under tiles wholly out of sight, " << interior_differing
                  << " differing from the processor's; " << edge << " under edge tiles, "
                  << edge_outside << " outside the colour-to-grey range; " << clear
                  << " under no tile, " << clear_differing << " changed\n";
        if ((whole_in_view && interior < least_fog_pixels) || interior_differing != 0 ||
            edge == 0 || edge_outside != 0 || clear_differing != 0) {
            write_png(
                picture("fog-unseen-zoom-" + zoom_text(zoom) + "-standard"), processor_unseen
            );
            write_png(picture("fog-off-zoom-" + zoom_text(zoom)), full_clear);
            fail("the greyed fog at zoom " + zoom_text(zoom) + " strays from the processor's");
        }
        // Line of sight and mapping: the black pass over the cells never
        // mapped, over everything, and nothing else changes.
        set_fog(true, true, false);
        const auto full_mapped = full_frame();
        objects = object_mask(zoom, full_mapped);
        const auto black = fog_shading_.unmapped_rgb;
        // The painters' quads over never-mapped ground draw over its black:
        // the shadow, outline and letter edges of game text in the modern
        // fonts there, over the black the processor's text lies on. The
        // outline holds the black where the renderer takes the minimum blend.
        std::vector<uint8_t> over_black(full_mapped.rgb.size());
        for (std::size_t at = 0; at + 3U <= over_black.size(); at += 3U)
            std::copy(
                black.begin(), black.end(), over_black.begin() + static_cast<std::ptrdiff_t>(at)
            );
        const auto quads_over_black = replay_world_quads(
            full_->world_quads,
            full_->executor.capabilities().minimum_composed,
            over_black,
            full_mapped.width,
            full_mapped.height,
            field.x,
            field.y
        );
        const auto processor_mapped = standard();
        write_png(picture("fog-unmapped-zoom-" + zoom_text(zoom)), full_mapped);
        std::size_t quads_unmapped = 0;
        std::size_t quads_unmapped_differing = 0;
        std::size_t unmapped = 0;
        std::size_t unmapped_differing = 0;
        std::size_t unmapped_edge = 0;
        std::size_t unmapped_edge_outside = 0;
        std::size_t untouched = 0;
        std::size_t untouched_changed = 0;
        // A view without a tile wholly unmapped shown in full, as a small
        // map's at a close zoom or one with a sliver of such a tile at its
        // border, holds the edge tiles to their range and the sliver to the
        // processor's black, without the fewest pixels.
        whole_in_view = false;
        std::fill(under_tile.begin(), under_tile.end(), 0);
        for (const auto& tile : fog_tiles(zoom)) {
            if (tile.tile.unmapped == 0)
                continue;
            const bool whole = tile.tile.unmapped == wr::fog_mask_full;
            whole_in_view = whole_in_view || (whole && shown_in_full(tile.area));
            for (int y = tile.area.y; y < tile.area.y + tile.area.h; ++y)
                for (int x = tile.area.x; x < tile.area.x + tile.area.w; ++x) {
                    if (!inside(field, x, y))
                        continue;
                    under(x, y) = 1;
                    const auto cell = static_cast<std::size_t>(y) * full_mapped.width +
                                      static_cast<std::size_t>(x);
                    if (whole && !inside(pointer, x, y) && objects[cell] == quads_alone) {
                        ++quads_unmapped;
                        const uint8_t* drawn = pixel(full_mapped, x, y);
                        for (std::size_t channel = 0; channel < 3; ++channel)
                            if (std::abs(
                                    int{drawn[channel]} - int{over_black[cell * 3U + channel]}
                                ) > most_blend_rounding * int{quads_over_black[cell]}) {
                                ++quads_unmapped_differing;
                                break;
                            }
                        continue;
                    }
                    if (passed_over(x, y))
                        continue;
                    const uint8_t* fogged = pixel(full_mapped, x, y);
                    if (whole) {
                        ++unmapped;
                        if (!same(fogged, black.data()) ||
                            !same(fogged, pixel(processor_mapped, x, y)))
                            ++unmapped_differing;
                        continue;
                    }
                    ++unmapped_edge;
                    const uint8_t* before = pixel(full_unseen, x, y);
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        const int low = std::min<int>(before[channel], black[channel]);
                        const int high = std::max<int>(before[channel], black[channel]);
                        if (fogged[channel] < low - most_blend_rounding ||
                            fogged[channel] > high + most_blend_rounding) {
                            ++unmapped_edge_outside;
                            break;
                        }
                    }
                }
        }
        for (int y = field.y; y < field.y + field.h; ++y)
            for (int x = field.x; x < field.x + field.w; ++x) {
                if (under(x, y) != 0 || passed_over(x, y))
                    continue;
                ++untouched;
                if (!same(pixel(full_unseen, x, y), pixel(full_mapped, x, y)))
                    ++untouched_changed;
            }
        std::cout << "render tiers check: full tier fog at zoom " << zoom_text(zoom) << ": "
                  << unmapped << " pixels under tiles never mapped, " << unmapped_differing
                  << " not the processor's black; " << unmapped_edge << " under edge tiles, "
                  << unmapped_edge_outside << " outside the range to black; " << untouched
                  << " under no such tile, " << untouched_changed << " changed; " << quads_unmapped
                  << " under the painters' quads over never-mapped ground, "
                  << quads_unmapped_differing << " not drawn over its black as asked\n";
        if ((whole_in_view && unmapped < least_fog_pixels) || unmapped_differing != 0 ||
            unmapped_edge == 0 || unmapped_edge_outside != 0 || untouched_changed != 0 ||
            quads_unmapped_differing != 0) {
            write_png(
                picture("fog-unmapped-zoom-" + zoom_text(zoom) + "-standard"), processor_mapped
            );
            fail("the black fog at zoom " + zoom_text(zoom) + " strays from the processor's");
        }
        // The dithered option: the dither colour at alpha one half over the
        // frame with the fog off under a tile wholly out of sight, the even
        // tone the processor's every-other pixel averages to.
        set_fog(true, false, true);
        const auto full_dithered = full_frame();
        objects = object_mask(zoom, full_dithered);
        write_png(picture("fog-dithered-zoom-" + zoom_text(zoom)), full_dithered);
        const auto dither = fog_shading_.dither_rgb;
        std::size_t dithered = 0;
        std::size_t dithered_differing = 0;
        whole_in_view = false;
        for (const auto& tile : fog_tiles(zoom)) {
            if (tile.tile.unseen != wr::fog_mask_full || tile.tile.unmapped == wr::fog_mask_full)
                continue;
            whole_in_view = whole_in_view || shown_in_full(tile.area);
            for (int y = tile.area.y; y < tile.area.y + tile.area.h; ++y)
                for (int x = tile.area.x; x < tile.area.x + tile.area.w; ++x) {
                    if (!inside(field, x, y))
                        continue;
                    if (passed_over(x, y))
                        continue;
                    ++dithered;
                    const uint8_t* before = pixel(full_clear, x, y);
                    const uint8_t* after = pixel(full_dithered, x, y);
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        const double expected = 0.5 * before[channel] + 0.5 * dither[channel];
                        if (std::abs(after[channel] - expected) > most_blend_rounding) {
                            ++dithered_differing;
                            break;
                        }
                    }
                }
        }
        std::cout << "render tiers check: full tier dithered fog at zoom " << zoom_text(zoom)
                  << ": " << dithered << " pixels under tiles wholly out of sight, "
                  << dithered_differing << " not the even tone\n";
        if ((whole_in_view && dithered < least_fog_pixels) || dithered_differing != 0)
            fail("the dithered fog at zoom " + zoom_text(zoom) + " is not the even tone");
    }
    // At a quarter and at the floor, where every level drawn comes from its
    // mosaic, one level at a quarter and two at the floor: the greyed pass
    // by the fog mask, in fewer quads than tile by tile, shows the frame
    // with the fog off under no tile out of sight, the same frame drawn
    // from the greyed mosaics under a tile wholly out of sight, and between
    // the two under a tile at an edge, a pixel where tiles of two states
    // meet held to the range alone; the objects passed over as elsewhere,
    // and every sprite.
    for (const float zoom : {0.25F, kMinFullBattlefieldZoom}) {
        at_zoom(zoom);
        set_fog(false, false, false);
        const auto clear_frame = full_frame();
        std::swap(full_->mosaic_pages, full_->greyed_mosaic_pages);
        const auto greyed_frame = full_frame();
        std::swap(full_->mosaic_pages, full_->greyed_mosaic_pages);
        set_fog(true, false, false);
        full_->fog_mask_off = true;
        full_frame();
        const uint32_t tiled_quads = full_->drawn_fog_quads;
        full_->fog_mask_off = false;
        const auto masked = full_frame();
        const uint32_t masked_quads = full_->drawn_fog_quads;
        auto objects = object_mask(zoom, masked);
        const Area field = battlefield();
        const Area pointer = cursor();
        const auto width = static_cast<int>(masked.width);
        const auto height = static_cast<int>(masked.height);
        // Every sprite too: the sprite stage greys one whole under a tile
        // out of sight, where the frames with the fog off show it in colour.
        {
            const auto& list = match_models().draws;
            for (const auto& draw : list.draws) {
                if (draw.kind != WorldDrawKind::sprite &&
                    draw.kind != WorldDrawKind::blended_sprite &&
                    draw.kind != WorldDrawKind::lit_sprite)
                    continue;
                const auto& sprite = list.sprites[draw.index];
                if (sprite.frame == nullptr)
                    continue;
                const SceneRect rect = sprite_scene_rect(sprite, zoom);
                mark_rect(
                    objects,
                    masked.width,
                    masked.height,
                    field.x + static_cast<int>(rect.left),
                    field.y + static_cast<int>(rect.top),
                    std::max(1, static_cast<int>(rect.right - rect.left)),
                    std::max(1, static_cast<int>(rect.bottom - rect.top)),
                    sprite_mask_margin
                );
            }
        }
        std::vector<uint8_t> states(masked.rgb.size() / 3U, static_cast<uint8_t>(TileState::clear));
        const auto state_at = [&](int x, int y) -> uint8_t& {
            return states[static_cast<std::size_t>(y) * masked.width + static_cast<std::size_t>(x)];
        };
        for (const auto& tile : fog_tiles(zoom)) {
            if (tile.tile.unseen == 0)
                continue;
            const auto state =
                tile.tile.unseen == wr::fog_mask_full ? TileState::unseen : TileState::edge;
            for (int y = std::max(tile.area.y, 0); y < std::min(tile.area.y + tile.area.h, height);
                 ++y)
                for (int x = std::max(tile.area.x, 0);
                     x < std::min(tile.area.x + tile.area.w, width);
                     ++x)
                    if (state_at(x, y) != static_cast<uint8_t>(TileState::edge))
                        state_at(x, y) = static_cast<uint8_t>(state);
        }
        std::size_t clear = 0;
        std::size_t clear_differing = 0;
        std::size_t interior = 0;
        std::size_t interior_differing = 0;
        std::size_t edge = 0;
        std::size_t edge_outside = 0;
        for (int y = field.y + 1; y + 1 < field.y + field.h; ++y)
            for (int x = field.x + 1; x + 1 < field.x + field.w; ++x) {
                const auto at =
                    static_cast<std::size_t>(y) * masked.width + static_cast<std::size_t>(x);
                if (inside(pointer, x, y) || objects[at] != 0)
                    continue;
                // A pixel among tiles of more than one state is an edge's.
                uint8_t state = state_at(x, y);
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                        if (state_at(x + dx, y + dy) != state_at(x, y))
                            state = static_cast<uint8_t>(TileState::edge);
                const uint8_t* by_mask = pixel(masked, x, y);
                const uint8_t* fog_off = pixel(clear_frame, x, y);
                const uint8_t* grey = pixel(greyed_frame, x, y);
                const auto near_to = [&](const uint8_t* wanted, int rounding) {
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        if (std::abs(by_mask[channel] - wanted[channel]) > rounding)
                            return false;
                    return true;
                };
                if (state == static_cast<uint8_t>(TileState::clear)) {
                    ++clear;
                    if (!near_to(fog_off, most_blend_rounding))
                        ++clear_differing;
                } else if (state == static_cast<uint8_t>(TileState::unseen)) {
                    ++interior;
                    if (!near_to(grey, most_mask_rounding))
                        ++interior_differing;
                } else {
                    ++edge;
                    for (std::size_t channel = 0; channel < 3; ++channel) {
                        const int low = std::min<int>(fog_off[channel], grey[channel]);
                        const int high = std::max<int>(fog_off[channel], grey[channel]);
                        if (by_mask[channel] < low - most_mask_rounding ||
                            by_mask[channel] > high + most_mask_rounding) {
                            ++edge_outside;
                            break;
                        }
                    }
                }
            }
        std::cout << "render tiers check: full tier fog by its mask at zoom " << zoom_text(zoom)
                  << ": " << masked_quads << " quads where the tiles take " << tiled_quads << "; "
                  << interior << " pixels under tiles wholly out of sight, " << interior_differing
                  << " not the greyed mosaics'; " << edge << " under edge tiles, " << edge_outside
                  << " outside the colour-to-grey range; " << clear << " under no tile, "
                  << clear_differing << " not the fog-off frame's\n";
        if (interior < least_fog_pixels || interior_differing != 0 || edge == 0 ||
            edge_outside != 0 || clear_differing != 0 || masked_quads == 0 ||
            masked_quads >= tiled_quads) {
            write_png(picture("fog-unseen-zoom-" + zoom_text(zoom) + "-masked"), masked);
            write_png(picture("fog-greyed-zoom-" + zoom_text(zoom)), greyed_frame);
            write_png(picture("fog-off-zoom-" + zoom_text(zoom)), clear_frame);
            fail(
                "the greyed fog by its mask at zoom " + zoom_text(zoom) +
                " strays from the colour and greyed terrain"
            );
        }
    }
    // At the zoom floor, a sixth, where the processor draws no picture to
    // hold the card's to and a fog tile spans a fraction of a pixel or a
    // few: the black pass leaves no pixel of never-mapped ground
    // unblackened between its quads, the ground's edge pixels left out.
    {
        const float zoom = kMinFullBattlefieldZoom;
        at_zoom(zoom);
        set_fog(true, true, false);
        const auto floor_mapped = full_frame();
        const auto objects = object_mask(zoom, floor_mapped);
        const auto black = fog_shading_.unmapped_rgb;
        const Area field = battlefield();
        const Area pointer = cursor();
        const auto width = static_cast<int>(floor_mapped.width);
        const auto height = static_cast<int>(floor_mapped.height);
        std::vector<uint8_t> unmapped_ground(floor_mapped.rgb.size() / 3U, 0);
        for (const auto& tile : fog_tiles(zoom)) {
            if (tile.tile.unmapped != wr::fog_mask_full)
                continue;
            for (int y = std::max(tile.area.y, 0); y < std::min(tile.area.y + tile.area.h, height);
                 ++y)
                for (int x = std::max(tile.area.x, 0);
                     x < std::min(tile.area.x + tile.area.w, width);
                     ++x)
                    unmapped_ground
                        [static_cast<std::size_t>(y) * floor_mapped.width +
                         static_cast<std::size_t>(x)] = 1;
        }
        const auto ground_at = [&](int x, int y) {
            return x >= 0 && y >= 0 && x < width && y < height &&
                   unmapped_ground
                           [static_cast<std::size_t>(y) * floor_mapped.width +
                            static_cast<std::size_t>(x)] != 0;
        };
        std::size_t inside_unmapped = 0;
        std::size_t not_black = 0;
        for (int y = field.y + 1; y + 1 < field.y + field.h; ++y)
            for (int x = field.x + 1; x + 1 < field.x + field.w; ++x) {
                bool within = true;
                for (int dy = -1; within && dy <= 1; ++dy)
                    for (int dx = -1; within && dx <= 1; ++dx)
                        within = ground_at(x + dx, y + dy);
                if (!within || inside(pointer, x, y) ||
                    objects
                            [static_cast<std::size_t>(y) * floor_mapped.width +
                             static_cast<std::size_t>(x)] != 0)
                    continue;
                ++inside_unmapped;
                if (!same(pixel(floor_mapped, x, y), black.data()))
                    ++not_black;
            }
        std::cout << "render tiers check: full tier fog at zoom " << zoom_text(zoom) << ": "
                  << inside_unmapped << " pixels within the ground never mapped, " << not_black
                  << " not black\n";
        if (not_black != 0) {
            write_png(picture("fog-unmapped-zoom-" + zoom_text(zoom)), floor_mapped);
            fail("the black fog at zoom " + zoom_text(zoom) + " leaves pixels between its quads");
        }
    }
    restore_fog();
    at_zoom(1.0F);

    // The overlay canvas: after a Full frame the world layer holds the key
    // colour but where the painters painted, and the overlay is opaque
    // exactly there. With nothing shown over the world the painters may
    // paint nothing at all.
    const auto canvas_matches_overlay = [&](const std::string& what, bool painted_something) {
        const auto key = full_->overlay_key;
        const uint32_t bf_w = match_world_cpu_.width;
        const uint32_t bf_h = match_world_cpu_.height;
        std::size_t painted = 0;
        std::size_t opaque = 0;
        std::size_t disagree = 0;
        for (uint32_t y = 0; y < bf_h; ++y)
            for (uint32_t x = 0; x < bf_w; ++x) {
                const auto cell = static_cast<std::size_t>(y) * bf_w + x;
                const bool is_painted =
                    std::memcmp(&match_world_cpu_.rgb[cell * 3U], key.data(), 3) != 0;
                const bool is_opaque = full_->overlay[cell * 4U + 3U] != 0;
                painted += is_painted ? 1U : 0U;
                opaque += is_opaque ? 1U : 0U;
                disagree += is_painted != is_opaque ? 1U : 0U;
            }
        std::cout << "render tiers check: full tier canvas " << what << ": " << painted
                  << " painted pixels, " << opaque << " opaque in the overlay, " << disagree
                  << " disagreeing\n";
        if (disagree != 0 || (painted_something && painted == 0))
            fail("the overlay canvas " + what + " does not match the overlay");
    };

    // The kill board: its foreground painted on the overlay as in every tier,
    // its shading of the world under it a black quad on the card at the
    // shade level's alpha, what its text in the modern fonts leaves to the
    // card drawn as asked, and nothing changed outside it.
    {
        const auto hidden = full_frame();
        canvas_matches_overlay("with the board hidden", false);
        bool running = true;
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = SDLK_F4;
        event.key.scancode = SDL_SCANCODE_F4;
        handle_sdl_event(event, running);
        if ((game.graphics_flags & hud::kGraphicsBoardPinned) == 0)
            fail("F4 did not pin the kill board");
        int frames = 0;
        while (kill_board_.slide != hud::kBoardWidth && frames < most_slide_frames) {
            std::ignore = full_frame();
            ++frames;
        }
        const auto shown = full_frame();
        // A game whose interface has no kills board, as the 1997 demo's,
        // leaves the slide where it was: the board's cases are skipped.
        const bool no_board = kill_board_.slide == 0;
        if (no_board)
            std::cout << "render tiers check: full tier: the game's interface has no kills "
                         "board; the kill board cases are skipped\n";
        else if (kill_board_.slide != hud::kBoardWidth)
            fail("the kill board did not slide out");
        if (!no_board) {
            canvas_matches_overlay("with the board shown", true);
            write_png(picture("kill-board"), shown);
            const int scale = hud_text_scale();
            const int left = oa::ui::display_layout::kSourceWidth - hud::kBoardWidth;
            const int bottom = game.player_count * hud::kBoardRowHeight + 0x2e;
            const auto corner = board_canvas(left, hud::kBoardTop);
            const auto end = board_canvas(oa::ui::display_layout::kSourceWidth, bottom + 1);
            const Area board{corner.x, corner.y, end.x - corner.x, end.y - corner.y};
            // The local player's row is lit, not shaded: it is left out, to the
            // last column and row the board lights.
            const auto& local = game.players[game.local_player_index];
            const int row_bottom = hud::kBoardTop + 0x34 + local.board_row * hud::kBoardRowHeight;
            const auto lit_corner = board_canvas(left + 4, row_bottom - 0x26);
            const auto lit_end = board_canvas(left + hud::kBoardWidth - 4 + 1, row_bottom + 2);
            const Area lit{
                lit_corner.x, lit_corner.y, lit_end.x - lit_corner.x, lit_end.y - lit_corner.y
            };
            const Area field = battlefield();
            const Area pointer = cursor();
            // What the shade level darkens a channel to, as the card blends black
            // over it at the level's alpha.
            const double kept = 1.0 - full_fog::level_quad(hud::kBoardShadeLevel).colour.alpha;
            // What the shadow of its text in the modern fonts darkens a
            // shaded channel to, as the card blends it over the shade.
            const double shadow_kept = kept * (1.0 - oa::present::text_shadow_alpha / 255.0);
            std::size_t shaded = 0;
            std::size_t shadowed = 0;
            std::size_t text_quads = 0;
            std::size_t foreground = 0;
            std::size_t neither = 0;
            std::size_t outside_changed = 0;
            // The pixels neither shaded nor painted, for the report of a failure.
            renderer::Surface stray = shown;
            for (auto& byte : stray.rgb)
                byte = static_cast<uint8_t>(byte / 2U);
            std::vector<std::tuple<int, int, std::array<uint8_t, 3>, std::array<uint8_t, 3>>>
                strays;
            const auto darkened = [&](const uint8_t* before, const uint8_t* after, double share) {
                for (std::size_t channel = 0; channel < 3; ++channel)
                    if (std::abs(after[channel] - before[channel] * share) > most_blend_rounding)
                        return false;
                return true;
            };
            // The frame without the board with the shown frame's quads drawn
            // over it on the processor: the shade, the light and what the
            // board's text in the modern fonts leaves to the card.
            auto replayed = hidden.rgb;
            const auto over = replay_world_quads(
                full_->world_quads,
                full_->executor.capabilities().minimum_composed,
                replayed,
                hidden.width,
                hidden.height,
                field.x,
                field.y
            );
            const auto as_quads_ask = [&](int x, int y, const uint8_t* after) {
                const auto at =
                    static_cast<std::size_t>(y) * hidden.width + static_cast<std::size_t>(x);
                const int quads = over[at];
                if (quads == 0)
                    return false;
                for (std::size_t channel = 0; channel < 3; ++channel)
                    if (std::abs(int{after[channel]} - int{replayed[at * 3U + channel]}) >
                        most_blend_rounding * quads)
                        return false;
                return true;
            };
            for (int y = field.y; y < field.y + field.h; ++y)
                for (int x = field.x; x < field.x + field.w; ++x) {
                    if (inside(pointer, x, y))
                        continue;
                    const uint8_t* before = pixel(hidden, x, y);
                    const uint8_t* after = pixel(shown, x, y);
                    if (!inside(board, x, y)) {
                        if (!same(before, after))
                            ++outside_changed;
                        continue;
                    }
                    if (inside(lit, x, y))
                        continue;
                    if (darkened(before, after, kept)) {
                        ++shaded;
                        continue;
                    }
                    // The shadow its text in the modern fonts casts beside the
                    // letters, a darkening the card blends over the shade.
                    if (darkened(before, after, shadow_kept)) {
                        ++shadowed;
                        continue;
                    }
                    // The foreground: a pixel the processor painted, the same in
                    // both tiers.
                    const auto cell =
                        static_cast<std::size_t>(y - field.y) * match_world_cpu_.width +
                        static_cast<std::size_t>(x - field.x);
                    if (full_->overlay[cell * 4U + 3U] != 0) {
                        ++foreground;
                        continue;
                    }
                    // The outline and letter edges of its text in the modern
                    // fonts, which the card draws over the shade.
                    if (as_quads_ask(x, y, after)) {
                        ++text_quads;
                        continue;
                    }
                    ++neither;
                    std::fill_n(
                        stray.rgb.data() + (static_cast<std::size_t>(y) * stray.width + x) * 3U,
                        3,
                        uint8_t{255}
                    );
                    if (strays.size() < most_strays_listed)
                        strays.emplace_back(
                            x,
                            y,
                            std::array<uint8_t, 3>{before[0], before[1], before[2]},
                            std::array<uint8_t, 3>{after[0], after[1], after[2]}
                        );
                }
            // Screen columns 515-516, left of the header and the highlight, are
            // shaded battlefield alone.
            std::size_t margin = 0;
            std::size_t margin_unshaded = 0;
            for (int y = board.y; y < board.y + board.h; ++y)
                for (int x = board.x; x < board.x + 2 * scale; ++x) {
                    if (inside(lit, x, y) || inside(pointer, x, y))
                        continue;
                    ++margin;
                    if (!darkened(pixel(hidden, x, y), pixel(shown, x, y), kept))
                        ++margin_unshaded;
                }
            std::cout << "render tiers check: full tier kill board " << board.w << 'x' << board.h
                      << ": " << shaded << " pixels shaded by the card, " << shadowed
                      << " shadowed by its text, " << text_quads
                      << " drawn as its text's other quads ask, " << foreground << " painted, "
                      << neither << " neither; margin " << margin << " with " << margin_unshaded
                      << " unshaded; " << outside_changed << " changed outside\n";
            if (shaded == 0 || foreground == 0 || neither != 0 || margin == 0 ||
                margin_unshaded != 0 || outside_changed != 0) {
                write_png(picture("kill-board-hidden"), hidden);
                write_png(picture("kill-board-stray"), stray);
                for (const auto& [x, y, before, after] : strays)
                    std::cout << "render tiers check: full tier kill board: (" << x << ", " << y
                              << ") was " << int{before[0]} << ' ' << int{before[1]} << ' '
                              << int{before[2]} << ", is " << int{after[0]} << ' ' << int{after[1]}
                              << ' ' << int{after[2]} << '\n';
                fail("the kill board's shading or foreground is not the card's and the overlay's");
            }
            handle_sdl_event(event, running);
            frames = 0;
            while (kill_board_.slide != 0 && frames < most_slide_frames) {
                std::ignore = full_frame();
                ++frames;
            }
            const auto gone = full_frame();
            if (kill_board_.slide != 0)
                fail("the kill board did not slide away");
            if (compare(gone, hidden, field, pointer).most != 0)
                fail("the kill board left pixels behind");
        }
    }

    // The +stats panel: its text and edges on the overlay, the battlefield
    // under it darkened by a black quad at the panel's opacity, the graph by
    // another, and nothing changed outside it.
    {
        const auto hidden = full_frame();
        show_frame_stats(true);
        const auto shown = full_frame();
        write_png(picture("frame-stats"), shown);
        if (!frame_stats_place_)
            fail("+stats placed no panel");
        const auto place = *frame_stats_place_;
        const Area panel_area{place.panel.x, place.panel.y, place.panel.width, place.panel.height};
        const Area field = battlefield();
        const Area pointer = cursor();
        std::size_t outside_changed = 0;
        for (int y = field.y; y < field.y + field.h; ++y)
            for (int x = field.x; x < field.x + field.w; ++x)
                if (!inside(panel_area, x, y) && !inside(pointer, x, y) &&
                    !same(pixel(hidden, x, y), pixel(shown, x, y)))
                    ++outside_changed;
        // The padding inside the raised edge: darkened by the panel's fill
        // alone; the graph's well: by the panel's and the graph's fills.
        const int padding_x = panel_area.x + panel::kBevel * place.scale;
        const int middle_y = panel_area.y + panel_area.h / 2;
        const auto darkened = [&](int x, int y, double kept_share) {
            const uint8_t* before = pixel(hidden, x, y);
            const uint8_t* after = pixel(shown, x, y);
            for (std::size_t channel = 0; channel < 3; ++channel)
                if (std::abs(after[channel] - before[channel] * kept_share) > most_blend_rounding)
                    return false;
            return true;
        };
        const double panel_kept = 1.0 - panel::kPanelOpacity / 256.0;
        const double graph_kept = panel_kept * (1.0 - panel::kGraphOpacity / 256.0);
        const bool padding_dark = darkened(padding_x, middle_y, panel_kept);
        const Area graph{place.graph.x, place.graph.y, place.graph.width, place.graph.height};
        // A well pixel no bar or line covers: the graph's top-left corner.
        const bool well_dark = darkened(graph.x, graph.y, graph_kept);
        const auto dark =
            ui_color_rgb(static_cast<uint8_t>(oa::ui::gadget_render::color_slot::dark_edge));
        const auto light =
            ui_color_rgb(static_cast<uint8_t>(oa::ui::gadget_render::color_slot::light_edge));
        const bool outline =
            same(pixel(shown, panel_area.x + panel_area.w / 2, panel_area.y), dark.data()) &&
            same(
                pixel(shown, panel_area.x + panel_area.w / 2, panel_area.y + place.scale),
                light.data()
            );
        std::cout << "render tiers check: full tier +stats panel " << panel_area.w << 'x'
                  << panel_area.h << ": padding darkened " << padding_dark
                  << ", graph well darkened " << well_dark << ", outline " << outline << ", "
                  << outside_changed << " changed outside\n";
        if (!padding_dark || !well_dark || !outline || outside_changed != 0) {
            write_png(picture("frame-stats-hidden"), hidden);
            fail("the +stats panel's darkening or foreground is not the card's and the overlay's");
        }
        canvas_matches_overlay("with +stats shown", true);
        show_frame_stats(false);
    }
    std::cout << "render tiers check: the full tier's fog, overlay canvas, kill board and +stats "
                 "panel are the card's and the overlay's\n";
}

} // namespace oa::app
