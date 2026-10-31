// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Headless check of the local player's build cursor at the bottom edge of
// the map the view shows, on a map with geothermal vents in the rows past
// that edge (Kill the Middle's): the simulation's site test lets a
// geothermal plant stand over such a vent, as 3.1c's does, and the cursor
// refuses every such site, from the pointer past the edge too, while sites
// within the map, a metal extractor's on raised ground drawn within it
// among them, are placed.
#include "oa/app/runtime.hpp"
#include "oa/core/map_plot.h"
#include "oa/sim/gameplay_input/input.hpp"
#include "oa/sim/unit_spawn/spawn_runtime.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("build past edge check: " + what);
}

std::string cell_text(int32_t x, int32_t z) {
    return std::to_string(x) + "," + std::to_string(z);
}

/// Map pixels below the shown map's bottom edge the pointer is put at.
constexpr int32_t kPointerPastEdge = 8;

} // namespace

void Runtime::check_build_past_edge() {
    namespace input = oa::sim::gameplay_input;
    namespace orders = oa::sim::match_runtime;
    start_benchmark_skirmish();
    uint16_t builder = 0;
    for (const auto& slot : match_->world().slots)
        if (slot.unit != nullptr && slot.record.type_index != 0 &&
            slot.record.owner_index == match_local_player_) {
            builder = slot.unit_index;
            break;
        }
    if (builder == 0)
        fail("found no local commander");
    // The buildings of the commander's own side.
    const bool core = match_->world().slots[builder].record.type_index ==
                      oa::sim::unit_spawn::find_type_index(spawn_type_names_, "CORCOM");
    const auto type_named = [&](const char* arm, const char* cor) {
        const auto* name = core ? cor : arm;
        const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
        if (type == 0 || type >= spawn_types_.size())
            fail(std::string("the game has no ") + name);
        return type;
    };
    const auto geo = type_named("ARMGEO", "CORGEO");
    const auto mex = type_named("ARMMEX", "CORMEX");
    // The whole map mapped and line of sight off, so that only a site's own
    // rules refuse it.
    chat_buffer_ = "+NowISee";
    submit_chat_line();

    const auto [shown_width, shown_height] = shown_map_size();
    const auto& spatial = match_->spatial();
    const auto width = static_cast<int32_t>(spatial.terrain_width);
    const auto height = static_cast<int32_t>(spatial.terrain_height);
    const auto plot = [&](int32_t x, int32_t z) -> const oa::sim::spatial_state::Plot& {
        return spatial.plots
            [static_cast<std::size_t>(z) * spatial.terrain_width + static_cast<std::size_t>(x)];
    };
    const auto primary_count = [&] {
        std::size_t count = 0;
        for (const auto* order = match_->orders(builder).primary; order != nullptr;
             order = order->next)
            ++count;
        return count;
    };
    const auto arm = [&](uint16_t type) {
        match_command_ = MatchCommand::build;
        pending_build_type_ = type;
    };
    // The middle of a footprint, where a click puts it.
    const auto centre = [&](uint16_t type, int32_t cell_x, int32_t cell_z) {
        return oa::sim::ground_orders::Point{
            (cell_x * 2 + spawn_types_[type].footprint_x) * 0x80000,
            0,
            (cell_z * 2 + spawn_types_[type].footprint_z) * 0x80000
        };
    };
    // A click at the site must give no order and leave the building armed.
    const auto expect_refused = [&](uint16_t type, const PendingBuildSite& site, const char* what) {
        if (site.legal || site.shown)
            fail(
                std::string("the cursor accepted the ") + what + ' ' +
                cell_text(site.cell_x, site.cell_z) + ", past the map's bottom edge"
            );
        const auto before = primary_count();
        place_pending_build_at(site.world);
        if (primary_count() != before || match_command_ != MatchCommand::build ||
            pending_build_type_ != type)
            fail(
                std::string("a click on the ") + what + ' ' + cell_text(site.cell_x, site.cell_z) +
                " past the map's bottom edge placed the building"
            );
    };
    // A click at the site must order MobileBuild there.
    const auto expect_placed = [&](
                                   uint16_t type, int32_t cell_x, int32_t cell_z, const char* what
                               ) {
        arm(type);
        const auto site = pending_build_site(centre(type, cell_x, cell_z));
        if (!site || site->cell_x != cell_x || site->cell_z != cell_z || !site->legal ||
            !site->shown)
            fail(std::string("the cursor refused the ") + what + ' ' + cell_text(cell_x, cell_z));
        match_->stop_orders(builder);
        place_pending_build_at(site->world, false);
        std::optional<orders::Match::QueuedCommandView> first;
        match_->visit_primary_queue(builder, [&](const auto& queued) {
            if (!first)
                first = queued;
        });
        if (!first || first->kind != orders::mobile_build_kind || first->build_type != type ||
            first->destination[0] != site->world[0] || first->destination[2] != site->world[2])
            fail(
                std::string("a click on the ") + what + ' ' + cell_text(cell_x, cell_z) +
                " did not order MobileBuild there"
            );
        match_->stop_orders(builder);
    };

    clear_local_selection();
    adopt_selection(builder);
    selected_match_unit_ = builder;
    match_->stop_orders(builder);
    arm(geo);
    const int32_t geo_x = spawn_types_[geo].footprint_x;
    const int32_t geo_z = spawn_types_[geo].footprint_z;

    // Every geothermal site over a vent in the rows from the shown height
    // down that the site test lets through.
    std::vector<std::pair<int32_t, int32_t>> vents;
    for (int32_t z = shown_height / OA_MAP_CELL_PIXELS; z < height; ++z)
        for (int32_t x = 0; x < width; ++x)
            if (plot(x, z).geo_feature)
                vents.emplace_back(x, z);
    if (vents.empty())
        fail("the map has no geothermal vent past its bottom edge");
    int refused = 0;
    for (const auto& [vent_x, vent_z] : vents)
        for (int32_t cell_z = vent_z - geo_z + 1; cell_z <= vent_z; ++cell_z)
            for (int32_t cell_x = vent_x - geo_x + 1; cell_x <= vent_x; ++cell_x) {
                if (!match_->building_site(geo, cell_x, cell_z, 0, match_local_player_))
                    continue;
                const auto site = pending_build_site(centre(geo, cell_x, cell_z));
                if (!site || site->cell_x != cell_x || site->cell_z != cell_z)
                    fail(
                        "the cursor's site at the middle of " + cell_text(cell_x, cell_z) + " is " +
                        (site ? cell_text(site->cell_x, site->cell_z) : std::string("none"))
                    );
                expect_refused(geo, *site, "geothermal site");
                ++refused;
            }
    if (refused == 0)
        fail(
            "the site test let no geothermal site over the " + std::to_string(vents.size()) +
            " vents past the map's bottom edge through"
        );

    // The pointer over the ground past the edge, below the first vent, with
    // the view past the edge: the classic pick holds it to the shown map.
    const auto [vent_x, vent_z] = vents.front();
    match_camera_x_ = vent_x * OA_MAP_CELL_PIXELS - visible_map_width() / 2;
    match_camera_z_ = shown_height - visible_map_height() / 2;
    const auto held = view_camera();
    match_camera_x_ = held[0];
    match_camera_z_ = held[1];
    const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
    const auto pointer = project_match_point(
        viewport,
        {static_cast<uint32_t>(vent_x * OA_MAP_CELL_PIXELS + OA_MAP_CELL_PIXELS / 2) << 16,
         0,
         static_cast<uint32_t>(shown_height + kPointerPastEdge) << 16}
    );
    const auto px = static_cast<float>(pointer.x);
    const auto py = static_cast<float>(pointer.y);
    if (!battlefield_contains(px, py))
        fail("the view does not show the ground past the map's bottom edge");
    update_pointer(px, py);
    const auto under = build_site_under(px, py);
    if (!under)
        fail("the pointer past the map's bottom edge gives no site");
    if (under->legal || under->shown)
        fail(
            "the cursor accepted the site " + cell_text(under->cell_x, under->cell_z) +
            " the pointer past the map's bottom edge gives"
        );
    render_match_surface();
    renderer::Surface frame;
    compose_match_frame(frame);
    const fs::path report_directory = "local/reports";
    fs::create_directories(report_directory);
    write_ppm(report_directory / "native-build-past-edge.ppm", frame);
    // The outline's top edge, at its middle, in the refused colour.
    const auto top_left = project_match_point(
        viewport,
        {static_cast<uint32_t>(under->cell_x * OA_MAP_CELL_PIXELS) << 16,
         static_cast<uint32_t>(under->world[1]),
         static_cast<uint32_t>(under->cell_z * OA_MAP_CELL_PIXELS) << 16}
    );
    const auto top_right = project_match_point(
        viewport,
        {static_cast<uint32_t>((under->cell_x + under->footprint_x) * OA_MAP_CELL_PIXELS) << 16,
         static_cast<uint32_t>(under->world[1]),
         static_cast<uint32_t>(under->cell_z * OA_MAP_CELL_PIXELS) << 16}
    );
    const auto edge_x = (top_left.x + top_right.x) / 2;
    const auto edge_y = top_left.y;
    if (edge_x < 0 || edge_y < 0 || edge_x >= static_cast<int>(frame.width) ||
        edge_y >= static_cast<int>(frame.height))
        fail("the outline of the site past the map's bottom edge is off the frame");
    const auto at =
        (static_cast<std::size_t>(edge_y) * frame.width + static_cast<std::size_t>(edge_x)) * 3U;
    const std::array<uint8_t, 3> drawn{frame.rgb[at], frame.rgb[at + 1], frame.rgb[at + 2]};
    if (drawn != ui_color_rgb(kBuildSiteRefusedColor))
        fail("the outline of the site past the map's bottom edge is not the refused colour");
    const auto clicked_before = primary_count();
    handle_match_left_click(px, py, 1);
    if (primary_count() != clicked_before || match_command_ != MatchCommand::build ||
        pending_build_type_ != geo)
        fail(
            "a click on the ground past the map's bottom edge placed the building at " +
            cell_text(under->cell_x, under->cell_z)
        );

    // A geothermal site over a vent within the map is placed.
    std::optional<std::pair<int32_t, int32_t>> inside;
    for (int32_t z = 0; z < height && !inside; ++z)
        for (int32_t x = 0; x < width && !inside; ++x) {
            if (!plot(x, z).geo_feature)
                continue;
            for (int32_t cell_z = z - geo_z + 1; cell_z <= z && !inside; ++cell_z)
                for (int32_t cell_x = x - geo_x + 1; cell_x <= x && !inside; ++cell_x) {
                    const auto stands =
                        match_->building_site(geo, cell_x, cell_z, 0, match_local_player_);
                    if (stands &&
                        input::footprint_within_shown_map(
                            cell_x, cell_z, geo_x, geo_z, *stands, shown_width, shown_height
                        ))
                        inside = std::pair{cell_x, cell_z};
                }
        }
    if (!inside)
        fail("the map has no geothermal site within it");
    expect_placed(geo, inside->first, inside->second, "geothermal site");

    // So is a metal extractor's site whose last row lies past the shown
    // height, on raised ground the view draws within the map.
    const int32_t mex_x = spawn_types_[mex].footprint_x;
    const int32_t mex_z = spawn_types_[mex].footprint_z;
    std::optional<std::pair<int32_t, int32_t>> raised;
    for (int32_t z = shown_height / OA_MAP_CELL_PIXELS - mex_z + 1; z + mex_z < height && !raised;
         ++z)
        for (int32_t x = 1; x + mex_x < width && !raised; ++x) {
            const auto stands = match_->building_site(mex, x, z, 0, match_local_player_);
            if (stands && (z + mex_z) * OA_MAP_CELL_PIXELS > shown_height &&
                input::footprint_within_shown_map(
                    x, z, mex_x, mex_z, *stands, shown_width, shown_height
                ))
                raised = std::pair{x, z};
        }
    if (!raised)
        fail("the map has no metal extractor site past its shown height drawn within it");
    expect_placed(mex, raised->first, raised->second, "metal extractor site on raised ground");

    std::cout << "build past edge check: " << refused << " geothermal sites over " << vents.size()
              << " vents past the map's bottom edge, which the site test lets through, refused "
                 "and no order given, also from the pointer past the edge at "
              << cell_text(under->cell_x, under->cell_z) << "; geothermal site "
              << cell_text(inside->first, inside->second) << " and metal extractor site "
              << cell_text(raised->first, raised->second) << " on raised ground placed\n";
}

} // namespace oa::app
