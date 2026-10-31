// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Check of the building drawn under the build cursor (ui.build-preview) in
// every tier: its pulse repeats every build_preview_pulse_ticks of the
// match and at no shorter period, the same at every zoom and however often
// the frames are drawn; it is drawn only over a site the game accepts; and
// it is drawn the right way up, over the finished building of its type at
// its site and reaching the same top.
#include "oa/app/runtime.hpp"

#include "match_fault.hpp"
#include "render_host.hpp"
#include "render_run.hpp"

#include "oa/app/view_rules.hpp"
#include "oa/platform/files.hpp"
#include "oa/present/model/model_draw.hpp"
#include "oa/formats/png.hpp"
#include "oa/sim/unit_spawn/spawn_runtime.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

/// The run's exit status when the machine cannot run the accelerated tiers.
constexpr int skipped_exit_code = 77;
/// The buildings placed, the first the game's data has: towers tall enough
/// that a picture turned upside down reaches below their footprint.
constexpr std::array<std::string_view, 2> preview_types{"ARMLLT", "CORLLT"};
/// Rings of 16-pixel cells searched around the commander for the site:
/// far enough that the commander stays out of the pictures compared.
constexpr int32_t site_nearest = 12;
constexpr int32_t site_farthest = 60;
/// Cells around the site that must hold no feature.
constexpr int32_t feature_clearance = 6;
/// Map pixels kept around the site in the pictures compared.
constexpr int32_t crop_reach = 48;
/// The zooms each tier is checked at.
constexpr std::array<float, 3> preview_zooms{0.5F, 1.0F, 2.0F};
/// Milliseconds waited before a frame of a moment is drawn again.
constexpr uint32_t redraw_wait_ms = 20;
/// The heading a building is built at facing south: half a turn, the
/// middle of its type's build angle.
constexpr uint16_t built_heading = 0x8000;
/// The pictures written of each tier at zoom 2: ticks into the run.
constexpr std::array<uint32_t, 4> pictured_ticks{0, 5, 10, 20};

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("build preview check: " + what);
}

/// An area of the battlefield's pixels.
struct Crop {
    int32_t left{};
    int32_t top{};
    int32_t width{};
    int32_t height{};
};

/// The pixels of a frame's area, in RGB.
using Pixels = std::vector<uint8_t>;

/// Writes an area of pixels as a PNG file.
///
/// @param path the file
/// @param rgb the area's pixels
/// @param crop the area
void write_png(const fs::path& path, const Pixels& rgb, const Crop& crop) {
    std::vector<uint8_t> file;
    const oa::formats::png::Header header{
        static_cast<uint32_t>(crop.width),
        static_cast<uint32_t>(crop.height),
        8,
        oa::formats::png::ColorType::rgb,
        {}
    };
    if (!oa::formats::png::write(oa::formats::png::Image{header, {}, rgb}, &file))
        fail("cannot encode " + oa::platform::path_to_utf8(path));
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size())
    );
    if (!output)
        fail("cannot write " + oa::platform::path_to_utf8(path));
}

/// The pixels of one picture that differ from another's, as a mask, with
/// the rows they span.
struct Changed {
    std::vector<uint8_t> mask; ///< 1 where the pictures differ
    std::size_t pixels{};
    int32_t top{std::numeric_limits<int32_t>::max()};
    int32_t bottom{std::numeric_limits<int32_t>::min()};
};

/// Finds the pixels of one picture of an area that differ from another's.
///
/// @param before the picture without the building
/// @param after the picture with it
/// @param crop the area both show
/// @return the pixels that differ
Changed changed_between(const Pixels& before, const Pixels& after, const Crop& crop) {
    Changed changed;
    changed.mask.assign(static_cast<std::size_t>(crop.width) * crop.height, 0);
    for (int32_t y = 0; y < crop.height; ++y)
        for (int32_t x = 0; x < crop.width; ++x) {
            const auto at = static_cast<std::size_t>(y) * crop.width + x;
            if (std::equal(
                    before.begin() + static_cast<std::ptrdiff_t>(at * 3U),
                    before.begin() + static_cast<std::ptrdiff_t>(at * 3U + 3U),
                    after.begin() + static_cast<std::ptrdiff_t>(at * 3U)
                ))
                continue;
            changed.mask[at] = 1;
            ++changed.pixels;
            changed.top = std::min(changed.top, y);
            changed.bottom = std::max(changed.bottom, y);
        }
    return changed;
}

/// Returns a zoom's text for the check's report: "0.5", "1", "2".
///
/// @param zoom the zoom
/// @return its text
std::string zoom_name(float zoom) {
    std::string text = std::to_string(zoom);
    text.erase(text.find_last_not_of('0') + 1);
    if (!text.empty() && text.back() == '.')
        text.pop_back();
    return text;
}

} // namespace

int Runtime::check_build_preview() {
    using oa::ui::engine_settings::HardwareAcceleration;
    if (sdl_.renderer == nullptr || !render_run_ || render_run_->host == nullptr)
        fail("needs the SDL renderer and the renderer host that made it");
    const auto& inputs = render_run_->host->tier_inputs();
    if (inputs.memory < render_policy::smallest_accelerated_memory) {
        std::cout << "build preview check: skipped: the machine reports less than the 2 GiB "
                     "threshold of memory\n";
        return skipped_exit_code;
    }
    if (inputs.capability != render_policy::Capability::capable && !options_.force_capable) {
        std::cout << "build preview check: skipped: the renderer is not capable of the "
                     "accelerated tiers\n";
        return skipped_exit_code;
    }
    if (!options_.hardware_acceleration ||
        *options_.hardware_acceleration != HardwareAcceleration::full)
        fail("needs --hardware-acceleration=full, which lets the run switch to every tier");
    if (!ui_rules().build_preview.enabled)
        fail("needs a profile that turns ui.build-preview on");
    const auto set_level = [&](HardwareAcceleration level) {
        options_.hardware_acceleration = level;
        update_render_tier();
        if ((level == HardwareAcceleration::full) != full_presentation() ||
            (level != HardwareAcceleration::off) != accelerated_presentation())
            fail("the tier did not switch");
    };
    // The match loads in Full, so that its loading screen makes the Full
    // tier's pages.
    set_level(HardwareAcceleration::full);
    start_benchmark_skirmish();

    auto& slots = match_->world().slots;
    uint16_t commander = 0;
    for (const auto& slot : slots)
        if (slot.unit_index != 0 && slot.unit != nullptr && slot.record.type_index != 0 &&
            slot.record.owner_index == match_local_player_) {
            commander = slot.unit_index;
            break;
        }
    if (commander == 0)
        fail("found no local commander");
    uint16_t type = 0;
    for (const auto name : preview_types) {
        const auto found = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
        if (found != 0 && found < spawn_types_.size() && found < loaded_commander_types_.size() &&
            loaded_commander_types_[found].model) {
            type = found;
            break;
        }
    }
    if (type == 0)
        fail("the game has none of the towers placed");
    const auto type_name = spawn_type_names_.at(type);

    // A site with no feature near enough to stand in the pictures.
    auto& world = match_->state();
    const auto clear_of_features = [&](int32_t x, int32_t z) {
        for (int32_t dz = -feature_clearance; dz <= feature_clearance; ++dz)
            for (int32_t dx = -feature_clearance; dx <= feature_clearance; ++dx) {
                const auto* plot = oa::world_plot(&world, x + dx, z + dz);
                if (plot == nullptr || plot->feature < world.feature_def_count)
                    return false;
            }
        return true;
    };
    const auto cell_x = static_cast<int32_t>(slots[commander].unit->position[0] >> 20);
    const auto cell_z = static_cast<int32_t>(slots[commander].unit->position[2] >> 20);
    std::optional<std::pair<int32_t, int32_t>> site;
    for (int32_t ring = site_nearest; ring < site_farthest && !site; ++ring)
        for (int32_t dz = -ring; dz <= ring && !site; dz += 2)
            for (int32_t dx = -ring; dx <= ring && !site; dx += 2)
                if ((dx == -ring || dx == ring || dz == -ring || dz == ring) &&
                    clear_of_features(cell_x + dx, cell_z + dz) &&
                    match_->building_site(type, cell_x + dx, cell_z + dz, 0))
                    site = std::pair{cell_x + dx, cell_z + dz};
    if (!site)
        fail("found no site for " + type_name + " near the commander");
    const auto& placed_type = spawn_types_[type];
    const auto centre_x = (site->first * 2 + static_cast<int32_t>(placed_type.footprint_x)) * 8;
    const auto centre_z = (site->second * 2 + static_cast<int32_t>(placed_type.footprint_z)) * 8;
    const oa::sim::unit_movement::Terrain terrain(*selected_tnt_);
    const auto ground =
        std::max<int32_t>(terrain.sea_level(), terrain.height(centre_x << 16, centre_z << 16));

    // Ground shadows are left out: a shadow falls beside its building, where
    // the hidden tower's stays drawn.
    world.game.graphics_flags &= static_cast<uint16_t>(~oa::present::model::graphics_shadows);

    // Frames are presented without the cursor, which would stand over the
    // building, and read back; the cursor shows again when the check ends,
    // however it ends.
    struct NoCursor {
        Runtime& runtime;
        bool kept{};

        ~NoCursor() { runtime.frame_without_cursor_ = kept; }
    } no_cursor{*this, frame_without_cursor_};

    frame_without_cursor_ = true;
    const auto tick = [&] {
        ++match_timing_.tick;
        match_->simulation().tick = match_timing_.tick;
        tick_or_raise(*match_);
    };
    // The view centred on the site at a zoom, the pointer on the site's
    // middle.
    Crop crop{};
    const auto view_site = [&](float zoom) {
        match_zoom_ = match_zoom_target_ = zoom;
        set_camera_position(
            centre_x - visible_map_width() / 2, centre_z - visible_map_height() / 2, 0
        );
        render_match_surface();
        // Where the frame draws map points: from the battlefield's corner,
        // moved by the view's offset to the nearest screen pixel.
        auto view = live_viewport(match_camera_x_, match_camera_z_);
        view.destination_x = -static_cast<int32_t>(
            std::lround(accelerated_.frame_offset.x * static_cast<double>(zoom))
        );
        view.destination_y = -static_cast<int32_t>(
            std::lround(accelerated_.frame_offset.y * static_cast<double>(zoom))
        );
        view.surface_width = static_cast<uint32_t>(match_layout_.battlefield_width());
        view.surface_height = static_cast<uint32_t>(match_layout_.battlefield_height());
        const auto pointed = project_match_point(
            view,
            {static_cast<uint32_t>(centre_x) << 16,
             static_cast<uint32_t>(ground) << 16,
             static_cast<uint32_t>(centre_z) << 16}
        );
        const int32_t x = match_layout_.left + pointed.x;
        const int32_t y = match_layout_.top + pointed.y;
        update_pointer(static_cast<float>(x), static_cast<float>(y));
        match_command_ = MatchCommand::build;
        pending_build_type_ = type;
        const auto under = build_site_under(match_pointer_x_, match_pointer_y_);
        if (!under || under->cell_x != site->first || under->cell_z != site->second)
            fail(
                "the pointer missed the site " + std::to_string(site->first) + ',' +
                std::to_string(site->second) + " at zoom " + zoom_name(zoom) + " for " +
                (under ? std::to_string(under->cell_x) + ',' + std::to_string(under->cell_z)
                       : std::string("none"))
            );
        const auto reach = static_cast<int32_t>(static_cast<float>(crop_reach) * zoom);
        crop = {x - reach, y - 2 * reach, 2 * reach, 3 * reach};
    };
    const auto present = [&]() {
        renderer::Surface frame;
        capture_frame_ = &frame;
        render();
        capture_frame_ = nullptr;
        Pixels out(static_cast<std::size_t>(crop.width) * crop.height * 3U, 0);
        for (int32_t y = 0; y < crop.height; ++y)
            for (int32_t x = 0; x < crop.width; ++x) {
                const int32_t fx = crop.left + x;
                const int32_t fy = crop.top + y;
                if (fx < 0 || fy < 0 || fx >= static_cast<int32_t>(frame.width) ||
                    fy >= static_cast<int32_t>(frame.height))
                    continue;
                const auto from = (static_cast<std::size_t>(fy) * frame.width + fx) * 3U;
                const auto to = (static_cast<std::size_t>(y) * crop.width + x) * 3U;
                std::copy_n(
                    frame.rgb.begin() + static_cast<std::ptrdiff_t>(from),
                    3,
                    out.begin() + static_cast<std::ptrdiff_t>(to)
                );
            }
        return out;
    };
    // The frame with the building being placed, and the same moment with it
    // held back: a line being started draws the site's outline alone.
    const auto placing = [&](bool shown) {
        match_command_ = MatchCommand::build;
        pending_build_type_ = type;
        build_tool_ = {};
        build_tool_.drawing_line = !shown;
        auto pixels = present();
        build_tool_ = {};
        return pixels;
    };
    const auto bare = [&]() {
        match_command_ = MatchCommand::none;
        pending_build_type_ = 0;
        build_tool_ = {};
        return present();
    };

    const fs::path report_directory = "local/reports";
    fs::create_directories(report_directory);
    const std::array<HardwareAcceleration, 3> levels{
        HardwareAcceleration::off, HardwareAcceleration::basic, HardwareAcceleration::full
    };
    const auto level_name = [](HardwareAcceleration level) -> std::string {
        switch (level) {
        case HardwareAcceleration::off:
            return "off";
        case HardwareAcceleration::basic:
            return "basic";
        case HardwareAcceleration::full:
            return "full";
        }
        return "off";
    };
    const uint32_t period = view_rules::build_preview_pulse_ticks;
    std::vector<std::string> faults;

    // What each tier and zoom drew of the building being placed, for the
    // finished building's picture to be compared with.
    struct Drawn {
        HardwareAcceleration level{};
        float zoom{};
        Changed preview{};
    };

    std::vector<Drawn> drawn;
    for (const auto level : levels) {
        set_level(level);
        for (const float zoom : preview_zooms) {
            view_site(zoom);
            const std::string which =
                "in the " + level_name(level) + " tier at zoom " + zoom_name(zoom);
            // Two periods of the pulse, a frame of each tick, drawn twice.
            std::vector<Pixels> frames;
            Changed seen;
            seen.mask.assign(static_cast<std::size_t>(crop.width) * crop.height, 0);
            for (uint32_t step = 0; step < 2 * period; ++step) {
                tick();
                const auto held = placing(false);
                auto shown = placing(true);
                SDL_Delay(redraw_wait_ms);
                if (placing(true) != shown)
                    faults.push_back(
                        "a frame drawn again " + which + " at tick " + std::to_string(step) +
                        " shows the building otherwise"
                    );
                const auto changed = changed_between(held, shown, crop);
                if (changed.pixels == 0)
                    fail("no building is drawn " + which);
                for (std::size_t at = 0; at < seen.mask.size(); ++at)
                    if (changed.mask[at] != 0 && seen.mask[at] == 0) {
                        seen.mask[at] = 1;
                        ++seen.pixels;
                    }
                seen.top = std::min(seen.top, changed.top);
                seen.bottom = std::max(seen.bottom, changed.bottom);
                if (zoom == 2.0F)
                    for (const auto at : pictured_ticks)
                        if (at == step)
                            write_png(
                                report_directory / ("build-preview-" + level_name(level) +
                                                    "-tick-" + std::to_string(at) + ".png"),
                                shown,
                                crop
                            );
                frames.push_back(std::move(shown));
            }
            // The pulse repeats every period and at no shorter one.
            for (uint32_t at = 0; at < period; ++at)
                if (frames[at] != frames[at + period]) {
                    faults.push_back(
                        "the building drawn " + which + " at tick " + std::to_string(at) +
                        " differs a period later"
                    );
                    break;
                }
            for (uint32_t shorter = 1; shorter < period; ++shorter) {
                bool differs = false;
                for (uint32_t at = 0; at + shorter < frames.size() && !differs; ++at)
                    differs = frames[at] != frames[at + shorter];
                if (!differs) {
                    faults.push_back(
                        "the building drawn " + which + " repeats every " +
                        std::to_string(shorter) + " ticks"
                    );
                    break;
                }
            }
            drawn.push_back({level, zoom, std::move(seen)});
        }
    }
    match_command_ = MatchCommand::none;
    pending_build_type_ = 0;

    // A finished tower at the site, facing as one is built facing south:
    // half a turn from heading 0, the middle of its type's build angle.
    oa::sim::unit_spawn::Request request;
    request.player = match_local_player_;
    request.type = type;
    request.finished = true;
    request.state = kGroundOccupancyState;
    request.position = {
        static_cast<uint32_t>(centre_x) << 16,
        static_cast<uint32_t>(match_->map_height(
            static_cast<uint32_t>(centre_x) << 16, static_cast<uint32_t>(centre_z) << 16
        )) << 16,
        static_cast<uint32_t>(centre_z) << 16
    };
    auto* placed = match_->create(request);
    if (placed == nullptr || placed->unit == nullptr)
        fail("could not place a finished " + type_name);
    placed->record.heading = built_heading;
    auto* instance = match_->instance(placed->unit_index);
    if (instance == nullptr)
        fail("the finished " + type_name + " has no model");
    std::vector<uint16_t> piece_flags;
    for (const auto& piece : instance->model().pieces())
        piece_flags.push_back(piece.flags);
    // The tower drawn, less the same moment with its pieces hidden: the
    // fog its sight cleared is the same in both.
    const auto tower = [&]() {
        for (auto& piece : instance->model().pieces())
            piece.flags &= static_cast<uint16_t>(
                ~static_cast<uint16_t>(oa::sim::model_runtime::PieceFlag::visible)
            );
        const auto hidden = bare();
        std::size_t at = 0;
        for (auto& piece : instance->model().pieces())
            piece.flags = piece_flags[at++];
        auto shown = bare();
        return std::pair{changed_between(hidden, shown, crop), std::move(shown)};
    };
    for (const auto& entry : drawn) {
        set_level(entry.level);
        view_site(entry.zoom);
        const std::string which =
            "in the " + level_name(entry.level) + " tier at zoom " + zoom_name(entry.zoom);
        // The game refuses the site the tower stands on: nothing is drawn
        // over it but the footprint's outline.
        if (placing(false) != placing(true))
            faults.push_back("a building is drawn over the refused site " + which);
        const auto [finished, pixels] = tower();
        if (entry.zoom == 2.0F)
            write_png(
                report_directory / ("build-preview-" + level_name(entry.level) + "-finished.png"),
                pixels,
                crop
            );
        // The building being placed lies over the tower: each of its pixels
        // within a pixel of the tower's at the zoom, its outline drawn up to
        // a pixel past the tower's edge, and its top the tower's.
        const int32_t reach = std::max(1, static_cast<int32_t>(std::lround(entry.zoom)));
        const auto near_tower = [&](int32_t x, int32_t y) {
            for (int32_t dy = -reach; dy <= reach; ++dy)
                for (int32_t dx = -reach; dx <= reach; ++dx) {
                    const int32_t nx = x + dx;
                    const int32_t ny = y + dy;
                    if (nx >= 0 && ny >= 0 && nx < crop.width && ny < crop.height &&
                        finished.mask[static_cast<std::size_t>(ny) * crop.width + nx] != 0)
                        return true;
                }
            return false;
        };
        std::size_t outside = 0;
        for (int32_t y = 0; y < crop.height; ++y)
            for (int32_t x = 0; x < crop.width; ++x)
                if (entry.preview.mask[static_cast<std::size_t>(y) * crop.width + x] != 0 &&
                    !near_tower(x, y))
                    ++outside;
        if (finished.pixels == 0)
            faults.push_back("the finished " + type_name + " is not drawn " + which);
        else if (outside != 0 || std::abs(entry.preview.top - finished.top) > reach)
            faults.push_back(
                "the building drawn " + which + " is not the finished " + type_name +
                "'s way up: its top at row " + std::to_string(entry.preview.top) +
                " of the tower's " + std::to_string(finished.top) + ", " + std::to_string(outside) +
                " of its " + std::to_string(entry.preview.pixels) + " pixels off the tower"
            );
    }
    if (!faults.empty()) {
        std::string what = std::to_string(faults.size()) + " faults:";
        for (const auto& fault : faults)
            what += "\n  " + fault;
        fail(what);
    }
    std::cout << "build preview check: " << type_name
              << " placed in the standard, Basic and Full tiers at zoom 0.5, 1 and 2 pulses every "
              << period
              << " ticks the same at every redraw, is drawn over the site the game "
                 "accepts alone, and lies over the finished tower; pictures in "
              << oa::platform::path_to_utf8(report_directory) << '\n';
    return 0;
}

} // namespace oa::app
