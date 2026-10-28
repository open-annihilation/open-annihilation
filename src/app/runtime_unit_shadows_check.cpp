// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// New unit shadows, for --check-render-tiers: a tank and an aircraft placed
// beside a tree, which casts the shadow drawn in its own art, drawn at zoom 1
// in each tier with the setting On and Off, and with shadows off for each
// tier's picture without them. The processor's tiers draw the same frame
// either way, the game's own shadows. With it Off the Full tier shades the
// pixels the processor's shadows shade, wherever the two tiers draw the same
// picture without shadows, to within a handful; with it On they part, the
// aircraft's shadow softened at its edge and every shadow laid under the
// units. Pictures of each go to the report.
#include "oa/app/runtime.hpp"

#include "oa/formats/png.hpp"
#include "oa/present/model/model_draw.hpp"
#include "oa/sim/unit_spawn/spawn_runtime.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace settings = oa::ui::engine_settings;

/// The tanks and aircraft the check places, the first the game has of each.
constexpr std::array<std::string_view, 3> kTanks{"ARMSTUMP", "CORRAID", "ARMPW"};
constexpr std::array<std::string_view, 4> kAircraft{"ARMFIG", "CORVENG", "ARMPEEP", "CORFINK"};
/// How far from the local commander a tree is looked for, in plot cells.
constexpr int32_t kTreeReach = 32;
/// How far a tree is looked for from the commander first, in map pixels: as
/// far down or up, or as far across, as keeps it clear of the fight the
/// check's skirmish stages about the commander.
constexpr int32_t kClearOfFightZ = 160;
constexpr int32_t kClearOfFightX = 240;
/// How far a tree stands from the map's edges, in map pixels past half a
/// picture, so that the pictures show the map about it.
constexpr int32_t kTreeEdgeMargin = 64;
/// The least height of a feature that stands among the units, as a tree
/// does, rather than lying under them; a height is read as unsigned, so a
/// negative one stands.
constexpr uint8_t kStandingHeight = 10;
/// Where the tank stands from the tree's place, in map pixels: a row in
/// front of it and a little to its left, so that the tank is drawn after the
/// tree and its shadow, five pixels to its right, reaches the tree's foot.
constexpr int32_t kTankDx = -12;
constexpr int32_t kTankDz = 18;
/// Where the aircraft flies from the tree's place, and how high above the
/// ground: high enough that its shadow takes the soft edge's wider reach
/// where New unit shadows draw it.
constexpr int32_t kAircraftDx = -44;
constexpr int32_t kAircraftDz = 44;
constexpr int32_t kAircraftAltitude = 112;
/// The part of the battlefield each picture shows, about the map point the
/// camera centres between the tree and the aircraft's shadow, and how many
/// times it is enlarged.
constexpr int32_t kPictureWidth = 200;
constexpr int32_t kPictureHeight = 150;
constexpr uint32_t kPictureScale = 3;
/// The brightest channel of a pixel the two shadows may part on: a colour
/// that dark the game's alpha table can darken to itself, while the card
/// still takes half of it, or the other way about.
constexpr uint8_t kDarkChannel = 24;

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("render tiers check: unit shadows: " + what);
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
        fail("cannot encode " + path.string());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size())
    );
    if (!output)
        fail("cannot write " + path.string());
}

/// Returns the colour of a frame's pixel, black outside it.
///
/// @param frame the frame
/// @param x the column
/// @param y the row
/// @return the pixel's red, green and blue
std::array<uint8_t, 3> pixel_of(const renderer::Surface& frame, int32_t x, int32_t y) {
    if (x < 0 || y < 0 || static_cast<uint32_t>(x) >= frame.width ||
        static_cast<uint32_t>(y) >= frame.height)
        return {};
    const std::size_t at =
        (std::size_t{static_cast<uint32_t>(y)} * frame.width + static_cast<uint32_t>(x)) * 3U;
    return {frame.rgb[at], frame.rgb[at + 1], frame.rgb[at + 2]};
}

/// Returns a part of a frame enlarged by nearest replication.
///
/// @param frame the frame
/// @param left the part's left column
/// @param top the part's top row
/// @return the part, kPictureScale times its size
renderer::Surface picture_of(const renderer::Surface& frame, int32_t left, int32_t top) {
    renderer::Surface result{
        kPictureWidth * kPictureScale,
        kPictureHeight * kPictureScale,
        std::vector<uint8_t>(
            std::size_t{kPictureWidth} * kPictureScale * kPictureHeight * kPictureScale * 3U
        )
    };
    for (uint32_t y = 0; y < result.height; ++y)
        for (uint32_t x = 0; x < result.width; ++x) {
            const auto colour = pixel_of(
                frame,
                left + static_cast<int32_t>(x / kPictureScale),
                top + static_cast<int32_t>(y / kPictureScale)
            );
            std::copy(
                colour.begin(),
                colour.end(),
                result.rgb.begin() +
                    static_cast<std::ptrdiff_t>((std::size_t{y} * result.width + x) * 3U)
            );
        }
    return result;
}

/// The pixels of the battlefield a tier's shadows change: those that differ
/// between its frame with shadows and its frame without.
struct ShadowMask {
    int32_t left{};
    int32_t top{};
    int32_t width{};
    int32_t height{};
    std::vector<uint8_t> shaded; ///< width by height, 1 where the shadows change the pixel
    std::size_t count{};         ///< pixels shaded
};

/// Returns the pixels of the battlefield one frame shades against another.
///
/// @param with the frame with shadows
/// @param without the same frame without shadows
/// @param left the battlefield's left column
/// @param top its top row
/// @param width its width
/// @param height its height
/// @return the mask
ShadowMask shadow_mask(
    const renderer::Surface& with,
    const renderer::Surface& without,
    int32_t left,
    int32_t top,
    int32_t width,
    int32_t height
) {
    ShadowMask mask{left, top, width, height, {}, 0};
    mask.shaded.assign(static_cast<std::size_t>(width) * static_cast<std::size_t>(height), 0);
    for (int32_t y = 0; y < height; ++y)
        for (int32_t x = 0; x < width; ++x)
            if (pixel_of(with, left + x, top + y) != pixel_of(without, left + x, top + y)) {
                mask.shaded
                    [static_cast<std::size_t>(y) * static_cast<std::size_t>(width) +
                     static_cast<std::size_t>(x)] = 1;
                ++mask.count;
            }
    return mask;
}

/// How two tiers' shadows compare, pixel by pixel.
struct MaskComparison {
    std::size_t both{}; ///< pixels both shade
    /// Pixels one shades alone, too bright to excuse, with no pixel the
    /// other shades within a pixel of them.
    std::size_t one_alone{};
    /// Pixels one shades alone beside a pixel the other shades: a shadow's
    /// edge a pixel apart, as where the processor's drawing region cuts it.
    std::size_t edge{};
    std::size_t dark{}; ///< pixels one shades alone, so dark the table may keep them
    /// Pixels not compared: the two tiers draw them otherwise without
    /// shadows, as the card draws a unit's or a sprite's edge a pixel from
    /// the processor's, which may cover a shadow in one tier alone.
    std::size_t apart_without{};
    /// The first of the pixels one shades alone, in frame pixels, for the
    /// report.
    std::vector<std::pair<int32_t, int32_t>> alone_at;
};

/// The most pixels one tier shades alone that a comparison names.
constexpr std::size_t kNamedPixels = 8;
/// The most pixels the Full tier's game shadows may part from the standard
/// tier's by, at an edge or alone: a hair of the thousands both shade, as
/// where SDL's software renderer takes the texel beside a pixel's own.
constexpr std::size_t kPartedPixels = 16;

/// Compares the pixels two tiers' shadows shade, where the tiers draw the
/// same picture without shadows.
///
/// @param mask the first tier's
/// @param other the second tier's, of the same battlefield
/// @param without the first tier's frame without shadows
/// @param other_without the second tier's frame without shadows
/// @return the comparison
MaskComparison compare_masks(
    const ShadowMask& mask,
    const ShadowMask& other,
    const renderer::Surface& without,
    const renderer::Surface& other_without
) {
    MaskComparison result;
    for (int32_t y = 0; y < mask.height; ++y)
        for (int32_t x = 0; x < mask.width; ++x) {
            const std::size_t at =
                static_cast<std::size_t>(y) * static_cast<std::size_t>(mask.width) +
                static_cast<std::size_t>(x);
            if (pixel_of(without, mask.left + x, mask.top + y) !=
                pixel_of(other_without, mask.left + x, mask.top + y)) {
                ++result.apart_without;
                continue;
            }
            const bool first = mask.shaded[at] != 0;
            const bool second = other.shaded[at] != 0;
            if (first && second) {
                ++result.both;
                continue;
            }
            if (first == second)
                continue;
            const auto colour = pixel_of(without, mask.left + x, mask.top + y);
            if (*std::max_element(colour.begin(), colour.end()) <= kDarkChannel) {
                ++result.dark;
                continue;
            }
            // The other tier's shadow within a pixel.
            const ShadowMask& opposite = first ? other : mask;
            bool beside = false;
            for (int32_t dy = -1; dy <= 1 && !beside; ++dy)
                for (int32_t dx = -1; dx <= 1 && !beside; ++dx) {
                    const int32_t nx = x + dx;
                    const int32_t ny = y + dy;
                    beside = nx >= 0 && ny >= 0 && nx < mask.width && ny < mask.height &&
                             opposite.shaded
                                     [static_cast<std::size_t>(ny) *
                                          static_cast<std::size_t>(mask.width) +
                                      static_cast<std::size_t>(nx)] != 0;
                }
            if (result.alone_at.size() < kNamedPixels)
                result.alone_at.emplace_back(mask.left + x, mask.top + y);
            if (beside)
                ++result.edge;
            else
                ++result.one_alone;
        }
    return result;
}

/// Returns the name a tier's pictures take.
///
/// @param level the level that draws in the tier
/// @return "standard", "basic" or "full"
std::string tier_name(settings::HardwareAcceleration level) {
    switch (level) {
    case settings::HardwareAcceleration::off:
        return "standard";
    case settings::HardwareAcceleration::basic:
        return "basic";
    case settings::HardwareAcceleration::full:
        return "full";
    }
    return "standard";
}

} // namespace

void Runtime::check_unit_shadow_switch(
    const std::function<void(oa::ui::engine_settings::HardwareAcceleration)>& set_level,
    std::span<const oa::ui::engine_settings::HardwareAcceleration> levels,
    const fs::path& report_directory
) {
    using settings::HardwareAcceleration;
    if (!match_ || !selected_tnt_ || levels.empty() || levels.front() != HardwareAcceleration::off)
        fail("needs a match and the standard tier first");
    oa::World& world = match_->state();
    // The local commander, and the nearest tree to it that stands on dry
    // ground and casts a shadow of its own.
    const oa::Unit* commander = nullptr;
    for (const auto& slot : match_->world().slots)
        if (slot.unit_index != 0 && slot.unit != nullptr && slot.record.type_index != 0) {
            const oa::Unit* record = oa::world_unit_at(&world, slot.unit_index);
            if (record != nullptr && record->owner_index == match_local_player_) {
                commander = record;
                break;
            }
        }
    if (commander == nullptr)
        fail("found no local unit");
    const int32_t commander_cell_x = (commander->position.x >> 16) / OA_MAP_CELL_PIXELS;
    const int32_t commander_cell_z = (commander->position.z >> 16) / OA_MAP_CELL_PIXELS;
    std::optional<std::pair<int32_t, int32_t>> tree;
    const int32_t margin_x = kPictureWidth / 2 + kTreeEdgeMargin;
    const int32_t margin_z = kPictureHeight / 2 + kTreeEdgeMargin;
    // The nearest tree clear of the fight, else the nearest at all.
    for (const bool clear_of_fight : {true, false})
        for (int32_t ring = 2; ring <= kTreeReach && !tree; ++ring)
            for (int32_t dz = -ring; dz <= ring && !tree; ++dz)
                for (int32_t dx = -ring; dx <= ring && !tree; ++dx) {
                    if (dx != -ring && dx != ring && dz != -ring && dz != ring)
                        continue;
                    if (clear_of_fight && std::abs(dz * OA_MAP_CELL_PIXELS) < kClearOfFightZ &&
                        std::abs(dx * OA_MAP_CELL_PIXELS) < kClearOfFightX)
                        continue;
                    const int32_t cell_x = commander_cell_x + dx;
                    const int32_t cell_z = commander_cell_z + dz;
                    const oa::MapPlot* plot = oa::world_plot(&world, cell_x, cell_z);
                    if (plot == nullptr || plot->feature >= world.feature_def_count ||
                        plot->height <= world.game.sea_level)
                        continue;
                    const oa::FeatureDef& def = world.feature_defs[plot->feature];
                    if ((def.flags & OA_FEATURE_FLAG_SPRITE) == 0 || def.seq_name_shadow == 0 ||
                        static_cast<uint8_t>(def.height) < kStandingHeight)
                        continue;
                    const int32_t x =
                        cell_x * OA_MAP_CELL_PIXELS + def.footprint_x * OA_MAP_CELL_PIXELS / 2;
                    const int32_t z =
                        cell_z * OA_MAP_CELL_PIXELS + def.footprint_z * OA_MAP_CELL_PIXELS / 2;
                    if (x < margin_x || z < margin_z || x > world.game.map_pixel_width - margin_x ||
                        z > world.game.map_pixel_height - margin_z)
                        continue;
                    tree = std::pair{x, z};
                }
    if (!tree)
        fail("found no tree near the local commander");
    const auto type_of = [&](std::span<const std::string_view> names) -> uint16_t {
        for (const auto name : names)
            if (const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
                type != 0)
                return static_cast<uint16_t>(type);
        return 0;
    };
    const uint16_t tank_type = type_of(kTanks);
    const uint16_t aircraft_type = type_of(kAircraft);
    if (tank_type == 0 || aircraft_type == 0)
        fail("the game has none of the tanks or none of the aircraft placed");
    const auto place = [&](uint16_t type, int32_t x, int32_t z) -> uint16_t {
        oa::sim::unit_spawn::Request request;
        request.player = match_local_player_;
        request.type = type;
        request.finished = true;
        request.state = kGroundOccupancyState;
        request.position = {
            static_cast<uint32_t>(x) << 16,
            static_cast<uint32_t>(
                match_->map_height(static_cast<uint32_t>(x) << 16, static_cast<uint32_t>(z) << 16)
            ) << 16,
            static_cast<uint32_t>(z) << 16
        };
        const auto* placed = match_->create(request);
        if (placed == nullptr || placed->unit == nullptr)
            fail("could not place " + spawn_type_names_.at(type));
        return static_cast<uint16_t>(placed->unit_index);
    };
    place(tank_type, tree->first + kTankDx, tree->second + kTankDz);
    const uint16_t aircraft =
        place(aircraft_type, tree->first + kAircraftDx, tree->second + kAircraftDz);
    // Lifted into the air where it stands: the frames are drawn between no
    // two ticks, so it stays there.
    oa::Unit* flying = oa::world_unit_at(&world, aircraft);
    if (flying == nullptr)
        fail("the aircraft placed has no record");
    const int32_t ground = match_->map_height(
        static_cast<uint32_t>(flying->position.x), static_cast<uint32_t>(flying->position.z)
    );
    flying->position.y =
        static_cast<int32_t>(static_cast<uint32_t>(ground + kAircraftAltitude) << 16);

    // Whole ticks, the interface's clock held and the pointer in the blank
    // corner, so that every frame is of one moment; the settings, the
    // shadows' option and the clock are put back however the check ends.
    const settings::EngineSettings saved = engine_settings();
    const uint16_t saved_flags = world.game.graphics_flags;

    struct Restore {
        Runtime& runtime;
        settings::EngineSettings kept_settings;
        uint16_t kept_flags{};
        std::optional<uint32_t> kept_clock{};

        ~Restore() {
            runtime.match_->state().game.graphics_flags = kept_flags;
            runtime.fake_frontend_tick_ = kept_clock;
            // A failure putting the settings back is the check's own to
            // report, not this unwinding's.
            try {
                runtime.apply_engine_settings(kept_settings);
            } catch (const std::exception& error) {
                std::cerr << "render tiers check: unit shadows: the settings were not put back: "
                          << error.what() << '\n';
            }
        }
    } restore{*this, saved, saved_flags, fake_frontend_tick_};

    fake_frontend_tick_ = frontend_tick();
    world.game.graphics_flags = static_cast<uint16_t>(
        saved_flags | oa::present::model::graphics_shadows |
        oa::present::model::graphics_vehicle_shadows
    );
    const uint16_t shaded_flags = world.game.graphics_flags;
    update_pointer(
        static_cast<float>(match_layout_.width - 1), static_cast<float>(match_layout_.height - 1)
    );
    settings::EngineSettings chosen = saved;
    chosen.smooth_motion = false;
    // No explosion flashes: the fight's lights over a shadow end white on
    // the processor and keep a tint of it on the card, which says nothing
    // of the shadows.
    chosen.explosion_flash = settings::ExplosionFlash::off;
    const auto presented = [&]() {
        renderer::Surface frame;
        capture_frame_ = &frame;
        render();
        capture_frame_ = nullptr;
        return frame;
    };
    const int32_t left = match_layout_.left;
    const int32_t top = match_layout_.top;
    const int32_t width = match_layout_.battlefield_width();
    const int32_t height = match_layout_.battlefield_height();
    std::optional<ShadowMask> standard;
    renderer::Surface standard_plain;
    renderer::Surface standard_off;
    // The map point the pictures centre on: between the tree and the
    // aircraft's shadow, at the ground, which the battlefield shows half its
    // height higher up.
    const int32_t middle_x = tree->first + kAircraftDx / 2;
    const int32_t middle_z = tree->second + kAircraftDz / 2;
    const int32_t middle_lift =
        match_->map_height(
            static_cast<uint32_t>(middle_x) << 16, static_cast<uint32_t>(middle_z) << 16
        ) /
        2;
    for (const auto level : levels) {
        const std::string tier = tier_name(level);
        set_level(level);
        if ((level == HardwareAcceleration::full) != full_presentation() ||
            (level != HardwareAcceleration::off) != accelerated_presentation())
            fail("the " + tier + " tier did not switch on");
        match_zoom_ = match_zoom_target_ = 1.0F;
        match_camera_x_ = middle_x - visible_map_width() / 2;
        match_camera_z_ = middle_z - middle_lift - visible_map_height() / 2;
        // Each frame takes the setting at once, in the running match.
        std::array<renderer::Surface, 2> frames;
        for (const bool on : {true, false}) {
            chosen.new_unit_shadows = on;
            apply_engine_settings(chosen);
            frames[on ? 0 : 1] = presented();
        }
        world.game.graphics_flags =
            static_cast<uint16_t>(shaded_flags & ~oa::present::model::graphics_shadows);
        const renderer::Surface plain = presented();
        world.game.graphics_flags = shaded_flags;
        // The pictures: the part of the battlefield about the middle, at
        // zoom 1 a map pixel to a frame pixel from the camera's corner.
        const int32_t picture_left = std::clamp(
            left + middle_x - static_cast<int32_t>(world.game.camera_x) - kPictureWidth / 2,
            left,
            std::max(left, left + width - kPictureWidth)
        );
        const int32_t picture_top = std::clamp(
            top + middle_z - middle_lift - static_cast<int32_t>(world.game.camera_y) -
                kPictureHeight / 2,
            top,
            std::max(top, top + height - kPictureHeight)
        );
        std::cout << "render tiers check: unit shadows: " << tier << " tier: the tree at map "
                  << tree->first << ", " << tree->second << ", the camera at "
                  << static_cast<int32_t>(world.game.camera_x) << ", "
                  << static_cast<int32_t>(world.game.camera_y) << ", the battlefield at " << left
                  << ", " << top << " (" << width << " by " << height << "), the pictures from "
                  << picture_left << ", " << picture_top << '\n';
        const std::string name = "native-render-tiers-unit-shadows-" + tier;
        write_png(
            report_directory / (name + "-on.png"), picture_of(frames[0], picture_left, picture_top)
        );
        write_png(
            report_directory / (name + "-off.png"), picture_of(frames[1], picture_left, picture_top)
        );
        write_png(
            report_directory / (name + "-none.png"), picture_of(plain, picture_left, picture_top)
        );
        const ShadowMask on = shadow_mask(frames[0], plain, left, top, width, height);
        const ShadowMask off = shadow_mask(frames[1], plain, left, top, width, height);
        if (on.count == 0 || off.count == 0)
            fail("the " + tier + " tier drew no shadow about the tree");
        const bool same = frames[0].rgb == frames[1].rgb;
        std::cout << "render tiers check: unit shadows: " << tier << " tier: " << on.count
                  << " pixels shaded with New unit shadows on, " << off.count << " off, "
                  << (same ? "the same frame" : "frames that differ");
        if (level != HardwareAcceleration::full) {
            std::cout << '\n';
            // The processor's tiers draw the game's own shadows either way.
            if (!same)
                fail("the " + tier + " tier's frame changed with New unit shadows");
            if (level == HardwareAcceleration::off) {
                standard = off;
                standard_plain = plain;
                standard_off = frames[1];
            }
            continue;
        }
        if (!standard)
            fail("the full tier came before the standard tier");
        // Off, the card shades what the processor shades; on, the aircraft's
        // soft edge and the shadows laid under the units part from it.
        const MaskComparison game = compare_masks(off, *standard, plain, standard_plain);
        const MaskComparison layered = compare_masks(on, *standard, plain, standard_plain);
        std::cout << "; where both tiers draw the same picture without shadows, off against "
                     "the standard tier: "
                  << game.both << " pixels both shade, " << game.edge << " at an edge, "
                  << game.one_alone << " one alone, " << game.dark
                  << " dark ones apart; on: " << layered.both << " both, " << layered.edge
                  << " at an edge, " << layered.one_alone << " one alone, " << layered.dark
                  << " dark ones apart; " << game.apart_without
                  << " pixels drawn otherwise without shadows\n";
        if (same)
            fail("the full tier's frame did not change with New unit shadows");
        const auto colour_text = [](const std::array<uint8_t, 3>& colour) {
            return std::to_string(colour[0]) + "," + std::to_string(colour[1]) + "," +
                   std::to_string(colour[2]);
        };
        for (const auto& [x, y] : game.alone_at)
            std::cout << "render tiers check: unit shadows: (" << x << ", " << y
                      << ") without shadows " << colour_text(pixel_of(plain, x, y))
                      << ", the full tier's " << colour_text(pixel_of(frames[1], x, y))
                      << ", the standard tier's " << colour_text(pixel_of(standard_off, x, y))
                      << '\n';
        if (game.one_alone + game.edge > kPartedPixels)
            fail(
                "with New unit shadows off the full tier's shadows part from the standard "
                "tier's in " +
                std::to_string(game.one_alone + game.edge) + " pixels"
            );
        if (layered.one_alone + layered.edge <= kPartedPixels)
            fail("with New unit shadows on the full tier's shadows are the standard tier's");
    }
}

} // namespace oa::app
