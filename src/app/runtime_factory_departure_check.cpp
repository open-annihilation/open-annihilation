// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-factory-departure: factories with platforms each build a unit,
// which drives off the platform. On every tick from the unit's release until
// its footprint has left the factory's, the frame is drawn at each camera row
// of one plot's scroll, and every frame must draw the unit after the
// factory, so that the platform never covers it.
#include "oa/app/runtime.hpp"
#include "match_models.hpp"

#include "oa/core/unit.h"
#include "oa/core/unit_def.h"
#include "oa/core/world.h"
#include "oa/present/world_renderer/world_draw_order.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/unit_spawn/spawn.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace oa::app {
namespace {

/// A factory and the unit it builds.
struct FactoryProduct {
    std::string_view factory{};
    std::string_view product{};
};

/// The factories with platforms the check builds from, in turn: the
/// vehicle plants, the Kbot labs, the hovercraft platforms, the aircraft
/// plants and the shipyards of both sides. The first must be in the installed
/// game; one whose types the game lacks, or which finds no site on the map,
/// is passed over with a line saying so.
constexpr std::array<FactoryProduct, 10> kFactories{
    {{"ARMVP", "ARMCV"},
     {"CORVP", "CORCV"},
     {"ARMLAB", "ARMCK"},
     {"CORLAB", "CORCK"},
     {"ARMHP", "ARMCH"},
     {"CORHP", "CORCH"},
     {"ARMAP", "ARMCA"},
     {"CORAP", "CORCA"},
     {"ARMSY", "ARMCS"},
     {"CORSY", "CORCS"}}
};

/// Ticks a factory gets to build its unit at full resources.
constexpr int kBuildTickLimit = 3600;
/// Ticks the unit gets to leave the factory's footprint.
constexpr int kDepartTickLimit = 450;
/// Camera rows of the scroll each frame of a departure is drawn at: one plot.
constexpr int32_t kScrollRows = oa::present::world_renderer::draw_row_depth;
/// Map pixels across and down one footprint cell.
constexpr int32_t kCellPixels = oa::present::world_renderer::footprint_cell_pixels;
/// Unit.flags occupancy of a unit on the ground or the water.
constexpr uint32_t kGroundOccupancy = 1;

/// Ends the check with a failure.
///
/// @param what what failed
[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("factory departure check: " + what);
}

/// Returns the whole map pixel of a 16.16 coordinate.
///
/// @param fixed the coordinate
/// @return its whole pixels, rounded toward negative infinity
int32_t whole(int32_t fixed) noexcept {
    return fixed >> 16;
}

/// Tells whether two units' footprints, each centred on its unit's place, overlap.
///
/// @param a a unit
/// @param b another unit
/// @return true when they share a map pixel
bool footprints_overlap(const oa::Unit& a, const oa::Unit& b) noexcept {
    const int32_t dx = std::abs(whole(a.position.x) - whole(b.position.x));
    const int32_t dz = std::abs(whole(a.position.z) - whole(b.position.z));
    return dx * 2 < (a.footprint_x + b.footprint_x) * kCellPixels &&
           dz * 2 < (a.footprint_z + b.footprint_z) * kCellPixels;
}

/// Tells whether a unit stands on the ground or the water, rather than flying.
///
/// @param unit the unit
/// @return true when its occupancy is the ground's
bool grounded(const oa::Unit& unit) noexcept {
    return (unit.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) == kGroundOccupancy;
}

} // namespace

void Runtime::check_factory_departure() {
    const fs::path snapshot_stem = [&] {
        fs::path path = options_.snapshot;
        path.replace_extension();
        return path;
    }();
    int factories_checked = 0;
    int frames_checked = 0;
    for (const auto& [factory_name, product_name] : kFactories) {
        if (match_) {
            if (match_tracking_)
                stop_match_tracking();
            leave_match();
            load(Screen::main_menu);
        }
        start_benchmark_skirmish();
        apply_output_mode();
        const auto factory_type =
            oa::sim::unit_spawn::find_type_index(spawn_type_names_, factory_name);
        const auto product_type =
            oa::sim::unit_spawn::find_type_index(spawn_type_names_, product_name);
        const bool first_pair = factory_name == kFactories.front().factory;
        if (factory_type == 0 || product_type == 0) {
            if (first_pair)
                fail("lacks " + std::string(factory_name) + " or " + std::string(product_name));
            std::printf(
                "factory departure: the game has no %.*s or %.*s; passed over\n",
                static_cast<int>(factory_name.size()),
                factory_name.data(),
                static_cast<int>(product_name.size()),
                product_name.data()
            );
            continue;
        }
        auto& slots = match_->world().slots;
        uint16_t commander = 0;
        for (const auto& slot : slots)
            if (slot.unit != nullptr && slot.record.type_index != 0 &&
                slot.record.owner_index == match_local_player_) {
                commander = slot.unit_index;
                break;
            }
        if (commander == 0)
            fail("found no local commander");
        const auto* placed = place_finished_structure(factory_type, commander);
        if (placed == nullptr && selected_tnt_ && factory_type < spawn_types_.size()) {
            // A shipyard finds no water beside the commander: the accepted
            // site nearest it anywhere on the map, every other cell.
            const oa::Unit& commander_unit = slots[commander].record;
            const int32_t commander_x = whole(commander_unit.position.x) / kCellPixels;
            const int32_t commander_z = whole(commander_unit.position.z) / kCellPixels;
            const auto cells_x = static_cast<int32_t>(selected_tnt_->tile_width * 2U);
            const auto cells_z = static_cast<int32_t>(selected_tnt_->tile_height * 2U);
            std::optional<std::array<int32_t, 3>> site;
            int64_t nearest = 0;
            for (int32_t cell_z = 0; cell_z < cells_z; cell_z += 2)
                for (int32_t cell_x = 0; cell_x < cells_x; cell_x += 2) {
                    const int64_t dx = cell_x - commander_x;
                    const int64_t dz = cell_z - commander_z;
                    if (site && dx * dx + dz * dz >= nearest)
                        continue;
                    if (const auto height =
                            match_->building_site(factory_type, cell_x, cell_z, 0)) {
                        site = std::array<int32_t, 3>{cell_x, cell_z, *height};
                        nearest = dx * dx + dz * dz;
                    }
                }
            if (site) {
                // The structure stands at its footprint's centre.
                const auto& type = spawn_types_[factory_type];
                const auto x = static_cast<uint32_t>(
                    (*site)[0] * kCellPixels + type.footprint_x * kCellPixels / 2
                );
                const auto z = static_cast<uint32_t>(
                    (*site)[1] * kCellPixels + type.footprint_z * kCellPixels / 2
                );
                oa::sim::unit_spawn::Request request;
                request.player = match_local_player_;
                request.type = factory_type;
                request.finished = true;
                request.state = kGroundOccupancyState;
                request.position = {x << 16, static_cast<uint32_t>((*site)[2]) << 16, z << 16};
                placed = match_->create(request);
                if (placed != nullptr && placed->unit == nullptr)
                    placed = nullptr;
            }
        }
        if (placed == nullptr) {
            if (first_pair)
                fail("could not place " + std::string(factory_name) + " near the commander");
            std::printf(
                "factory departure: no site for %.*s on the map; passed over\n",
                static_cast<int>(factory_name.size()),
                factory_name.data()
            );
            continue;
        }
        const uint16_t factory_unit = placed->unit_index;
        match_->queue_factory_build(factory_unit, product_type, 1);

        auto& player = match_->world().players[match_local_player_];
        const auto step_tick = [&] {
            player.metal = player.metal_cap;
            player.energy = player.energy_cap;
            step_match_simulation();
            if (match_->fault() != nullptr)
                fail("the match raised a simulation fault");
        };
        // The unit, from the tick it leaves the factory's build piece finished.
        uint16_t product_unit = 0;
        for (int step = 0; step < kBuildTickLimit && product_unit == 0; ++step) {
            step_tick();
            for (const auto& slot : slots)
                if (slot.unit != nullptr && slot.record.type_index == product_type &&
                    slot.record.owner_index == match_local_player_ &&
                    slot.record.build_remaining == 0.0F && slot.record.attach_parent == 0)
                    product_unit = slot.unit_index;
        }
        const std::string pair_name =
            std::string(factory_name) + " and " + std::string(product_name);
        if (product_unit == 0)
            fail(
                "saw no finished " + std::string(product_name) + " leave " +
                std::string(factory_name) + "'s build piece"
            );

        const oa::Unit& factory_record = slots[factory_unit].record;
        const oa::Unit& product_record = slots[product_unit].record;
        std::printf(
            "factory departure: %s %u at %d,%d, %s %u released at %d,%d\n",
            std::string(factory_name).c_str(),
            static_cast<unsigned>(factory_unit),
            whole(factory_record.position.x),
            whole(factory_record.position.z),
            std::string(product_name).c_str(),
            static_cast<unsigned>(product_unit),
            whole(product_record.position.x),
            whole(product_record.position.z)
        );
        // The scroll's first row: a plot above the camera centred on the
        // factory, so that a camera held at the map's foot still moves.
        center_camera_on_unit(factory_unit);
        const int32_t camera_x = match_camera_x_;
        const int32_t camera_z = match_camera_z_ - kScrollRows;
        // Where a unit is in the frame's model draws; -1 when not drawn.
        const auto draw_place = [&](uint16_t unit) {
            const auto& draws = match_models().draws.models;
            for (std::size_t at = 0; at < draws.size(); ++at)
                if (draws[at].model.unit != nullptr && draws[at].model.unit->id == unit)
                    return static_cast<int32_t>(at);
            return int32_t{-1};
        };
        int departure_ticks = 0;
        int frames_under = 0;
        for (int step = 0;
             step < kDepartTickLimit && footprints_overlap(factory_record, product_record);
             ++step) {
            std::string row_marks;
            for (int32_t row = 0; row < kScrollRows; ++row) {
                set_camera_position(camera_x, camera_z + row, 0);
                render_match_surface();
                if (match_models().renderer.camera_y != camera_z + row)
                    fail(pair_name + ": the camera did not scroll to row " + std::to_string(row));
                const int32_t factory_at = draw_place(factory_unit);
                const int32_t product_at = draw_place(product_unit);
                if (factory_at < 0 || product_at < 0)
                    fail(pair_name + ": a frame did not draw both");
                const bool over = product_at > factory_at;
                row_marks += over ? '+' : '-';
                frames_under += over ? 0 : 1;
                ++frames_checked;
                if (departure_ticks == 0 && !options_.snapshot.empty()) {
                    char file[64];
                    std::snprintf(
                        file,
                        sizeof file,
                        "-%s-row%02d.ppm",
                        std::string(factory_name).c_str(),
                        static_cast<int>(row)
                    );
                    write_ppm(fs::path(snapshot_stem) += file, match_world_cpu_);
                }
            }
            if (departure_ticks % 10 == 0 || row_marks.find('-') != std::string::npos)
                std::printf(
                    "factory departure: %s tick %u, %s at %d,%d, rows %s\n",
                    std::string(factory_name).c_str(),
                    static_cast<unsigned>(match_->simulation().tick),
                    std::string(product_name).c_str(),
                    whole(product_record.position.x),
                    whole(product_record.position.z),
                    row_marks.c_str()
                );
            ++departure_ticks;
            step_tick();
        }
        if (departure_ticks == 0)
            fail(pair_name + ": the unit was released off the factory's footprint");
        // An aircraft may stay over the platform, hovering or landed; any
        // other unit must have left it.
        const auto* product_def = oa::world_unit_def_of(&match_->state(), &product_record);
        const bool flies =
            product_def != nullptr && (product_def->flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0;
        const bool stayed = footprints_overlap(factory_record, product_record);
        if (stayed && !flies)
            fail(pair_name + ": the unit stayed on the factory's footprint");
        if (frames_under != 0)
            fail(
                pair_name + ": " + std::to_string(frames_under) + " frames of " +
                std::to_string(departure_ticks * kScrollRows) + " drew the factory over the unit"
            );
        std::printf(
            "factory departure: %s %s %s's platform in %d ticks, drawn over it\n",
            std::string(product_name).c_str(),
            !stayed                    ? "left"
            : grounded(product_record) ? "stayed landed on"
                                       : "stayed over",
            std::string(factory_name).c_str(),
            departure_ticks
        );
        ++factories_checked;
    }
    std::printf(
        "factory departure check: passed, %d factories, %d frames\n",
        factories_checked,
        frames_checked
    );
}

} // namespace oa::app
