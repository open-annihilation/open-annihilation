// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The cursor a selected commander shows over the map's vegetation, another
// reclaimable feature and a wreck, driven through synthetic SDL pointer motion,
// and over a tree under the OA layer's screens; its clicks on trees out of
// sight, on mapped ground and on ground never mapped, and where a
// construction kbot's and a construction aircraft's Reclaim of a tree draw
// their markers.
#include "oa/app/runtime.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"
#include "oa/core/map_plot.h"
#include "oa/formats/png.hpp"
#include "oa/sim/feature_runtime.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <bit>
#include <cstdlib>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {
namespace {

namespace input = oa::sim::gameplay_input;

// Kinds of vegetation (flammable reclaimable features) hovered, at most.
constexpr std::size_t kVegetationKinds = 4;
// Cells around the commander searched for features, and the wreck's site.
constexpr int32_t kSearchCells = 40;
constexpr int32_t kWreckNearest = 6;
// Screen pixels between pointer samples, and around a footprint's drawn box.
constexpr int32_t kSampleStep = 2;
constexpr int32_t kSampleMargin = 12;
constexpr uint16_t kNoFeature = 0xffff;
// The Reclaim orders' kinds, of a ground unit and of an aircraft.
constexpr uint8_t kReclaimKind = 32;
constexpr uint8_t kAirReclaimKind = 58;
constexpr std::string_view kWreckName = "armsolar_dead";
// Map pixels a sight cell spans, and the most a sight cell sits north of the
// ground it covers: half the tallest ground.
constexpr int32_t kSightCellPixels = 32;
constexpr int32_t kHalfTallestGround = 128;
// Map cells mapped or unmapped around a tree under the fog, the cells between
// the two trees, and the ticks the commander has to reclaim the mapped one.
constexpr int32_t kFogMarginCells = 2;
constexpr int32_t kFogTreesApartCells = 24;
constexpr int kFogReclaimTicks = 3000;
// Map pixels west and south of the first tree the construction kbot is
// placed at, and the aircraft further off; the ticks it then flies toward the
// tree before the overlays are drawn, and the picture of them.
constexpr int32_t kKbotApart = 48;
constexpr int32_t kAircraftApart = 160;
constexpr int kAircraftFlightTicks = 90;
constexpr const char* kOverlayPicture = "reclaim-order-overlays.png";
// Half the side of the screen that is not modal laid over a tree, and how
// far right of the tree the pointer is moved off it, in canvas pixels.
constexpr int32_t kMarkHalf = 8;
constexpr float kBesideMark = 24.0F;

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("reclaim cursor check: " + what);
}

std::string feature_name(const oa::FeatureDef& def) {
    return {def.name, ::strnlen(def.name, sizeof def.name)};
}

struct Hovered {
    int32_t cell_x{};
    int32_t cell_z{};
    const oa::FeatureDef* def{};
};

/// Writes a frame as a PNG file.
///
/// @param path the file
/// @param frame the frame
void write_png(const char* path, const oa::present::world_renderer::Surface& frame) {
    std::vector<uint8_t> file;
    const oa::formats::png::Header header{
        frame.width, frame.height, 8, oa::formats::png::ColorType::rgb, {}
    };
    if (!oa::formats::png::write(oa::formats::png::Image{header, {}, frame.rgb}, &file))
        fail(std::string("cannot encode ") + path);
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size())
    );
    if (!output)
        fail(std::string("cannot write ") + path);
}

} // namespace

void Runtime::check_reclaim_cursor() {
    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        fail("needs the SDL renderer");
    start_benchmark_skirmish();
    apply_output_mode();
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
    auto& world = match_->state();
    const auto commander_x = static_cast<int32_t>(slots[commander].unit->position[0] >> 20);
    const auto commander_z = static_cast<int32_t>(slots[commander].unit->position[2] >> 20);

    bool running = true;
    const auto send = [&](SDL_EventType type, uint8_t button, float x, float y) {
        float window_x = 0.0F;
        float window_y = 0.0F;
        if (!frame_to_window(sdl_.renderer, x, y, &window_x, &window_y))
            fail(SDL_GetError());
        SDL_Event event{};
        event.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            event.motion.windowID = SDL_GetWindowID(sdl_.window);
            event.motion.x = window_x;
            event.motion.y = window_y;
        } else {
            event.button.windowID = SDL_GetWindowID(sdl_.window);
            event.button.button = button;
            event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
        }
        dispatch_event(event, running);
    };
    const auto click = [&](float x, float y) {
        send(SDL_EVENT_MOUSE_MOTION, 0, x, y);
        send(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT, x, y);
        send(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT, x, y);
    };
    const auto viewport = [&] { return live_viewport(match_camera_x_, match_camera_z_); };

    center_camera_on_unit(commander);
    render_match_surface();
    const auto at_commander = project_match_point(viewport(), slots[commander].unit->position);
    click(static_cast<float>(at_commander.x), static_cast<float>(at_commander.y));
    if (selected_match_unit_ != commander ||
        (slots[commander].unit->flags & OA_UNIT_FLAG_SELECTED) == 0)
        fail("did not select the commander with a click");

    const auto origin_word = [&](int32_t x, int32_t z) {
        const auto* plot = oa::world_plot(&world, x, z);
        return plot != nullptr ? plot->feature : kNoFeature;
    };
    const auto visible_cell = [&](int32_t x, int32_t z) {
        const auto height = match_->map_height(
            static_cast<uint32_t>(x * OA_MAP_CELL_PIXELS + 8) << 16U,
            static_cast<uint32_t>(z * OA_MAP_CELL_PIXELS + 8) << 16U
        );
        return match_->point_shown(
            match_local_player_,
            {static_cast<uint32_t>(x * OA_MAP_CELL_PIXELS + 8) << 16U,
             static_cast<uint32_t>(std::max(height, 0)) << 16U,
             static_cast<uint32_t>(z * OA_MAP_CELL_PIXELS + 8) << 16U}
        );
    };
    const auto footprint_visible = [&](int32_t x, int32_t z, const oa::FeatureDef& def) {
        for (int32_t row = 0; row < def.footprint_z; ++row)
            for (int32_t column = 0; column < def.footprint_x; ++column)
                if (!visible_cell(x + column, z + row))
                    return false;
        return true;
    };

    // The nearest visible feature of each kind, flammable vegetation first,
    // then one that does not burn.
    std::vector<Hovered> hovered;
    std::vector<uint16_t> kinds;
    const auto pick_kinds = [&](bool vegetation, std::size_t wanted) {
        std::size_t taken = 0;
        for (int32_t ring = 0; ring < kSearchCells && taken < wanted; ++ring)
            for (int32_t dz = -ring; dz <= ring && taken < wanted; ++dz)
                for (int32_t dx = -ring; dx <= ring && taken < wanted; ++dx) {
                    if (dx != -ring && dx != ring && dz != -ring && dz != ring)
                        continue;
                    const auto x = commander_x + dx;
                    const auto z = commander_z + dz;
                    const auto word = origin_word(x, z);
                    if (word >= world.feature_def_count ||
                        std::find(kinds.begin(), kinds.end(), word) != kinds.end())
                        continue;
                    const auto& def = world.feature_defs[word];
                    const bool flammable = (def.flags & OA_FEATURE_FLAG_FLAMABLE) != 0;
                    if ((def.flags & OA_FEATURE_FLAG_RECLAIMABLE) == 0 || flammable != vegetation ||
                        !footprint_visible(x, z, def))
                        continue;
                    kinds.push_back(word);
                    hovered.push_back({x, z, &def});
                    ++taken;
                }
        return taken;
    };
    if (pick_kinds(true, kVegetationKinds) < 2)
        fail("found fewer than two kinds of visible vegetation near the commander");
    (void)pick_kinds(false, 1);

    // A wreck placed on open ground the commander sees.
    const auto wreck_index =
        oa::sim::map_runtime::find_feature_index(feature_table_, kWreckName.data());
    if (wreck_index >= world.feature_def_count)
        fail("found no " + std::string(kWreckName) + " definition");
    const auto& wreck_def = world.feature_defs[wreck_index];
    bool wreck_placed = false;
    for (int32_t ring = kWreckNearest; ring < kSearchCells && !wreck_placed; ++ring)
        for (int32_t dz = -ring; dz <= ring && !wreck_placed; ++dz)
            for (int32_t dx = -ring; dx <= ring && !wreck_placed; ++dx) {
                const auto x = commander_x + dx;
                const auto z = commander_z + dz;
                bool open = footprint_visible(x, z, wreck_def);
                for (int32_t row = -1; open && row <= wreck_def.footprint_z; ++row)
                    for (int32_t column = -1; open && column <= wreck_def.footprint_x; ++column) {
                        const auto* plot = oa::world_plot(&world, x + column, z + row);
                        open = plot != nullptr && plot->feature == kNoFeature &&
                               plot->ground_unit == 0;
                    }
                if (open && console_place_feature(kWreckName.data(), x, z)) {
                    hovered.push_back({x, z, &wreck_def});
                    wreck_placed = true;
                }
            }
    if (!wreck_placed)
        fail("found no open ground for " + std::string(kWreckName));

    // A click with RECLAIM armed at `point` on the first tree's footprint.
    const auto check_reclaim_click = [&](const Hovered& tree, std::array<float, 2> point) {
        match_command_ = MatchCommand::reclaim;
        click(point[0], point[1]);
        std::vector<oa::sim::match_runtime::Match::QueuedCommandView> queue;
        match_->visit_primary_queue(commander, [&](const auto& order) { queue.push_back(order); });
        const auto& def = *tree.def;
        const auto column = queue.empty() ? -1 : (queue.front().destination[0] >> 20) - tree.cell_x;
        const auto row = queue.empty() ? -1 : (queue.front().destination[2] >> 20) - tree.cell_z;
        if (queue.empty() || queue.front().kind != kReclaimKind || column < 0 || row < 0 ||
            column >= def.footprint_x || row >= def.footprint_z)
            fail(
                "a RECLAIM click on " + feature_name(def) + " gave the commander no Reclaim there"
            );
    };

    // Over a screen of the OA layer the cursor is the arrow, whatever lies
    // under it, and the order cursor again once the screen closes: Settings,
    // modal, opened over the paused in-game menu as F2's OA Settings opens
    // it, and a screen that is not modal laid over the tree alone, beside
    // which the cursor stays the match's.
    const auto shown_cursor = [&] {
        const auto cursor = shown_match_cursor();
        return cursor ? static_cast<int>(*cursor) : -1;
    };
    const auto expect_cursor = [&](float x, float y, int cursor, const std::string& what) {
        send(SDL_EVENT_MOUSE_MOTION, 0, x, y);
        if (const auto shown = shown_cursor(); shown != cursor)
            fail(
                "the pointer at " + std::to_string(static_cast<int>(x)) + "," +
                std::to_string(static_cast<int>(y)) + " " + what + " shows cursor " +
                std::to_string(shown) + ", not " + std::to_string(cursor)
            );
    };
    const auto tap_key = [&](SDL_Keycode key) {
        for (const bool down : {true, false}) {
            SDL_Event event{};
            event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
            event.key.windowID = SDL_GetWindowID(sdl_.window);
            event.key.key = key;
            event.key.down = down;
            dispatch_event(event, running);
        }
    };
    const auto check_layer_cursor = [&](const Hovered& tree, std::array<float, 2> point) {
        const auto tree_at = feature_name(*tree.def) + " at " + std::to_string(tree.cell_x) + "," +
                             std::to_string(tree.cell_z);
        const auto on_tree = "on " + tree_at;
        const auto reclaim = static_cast<int>(input::OrderCursor::reclaim);
        const auto arrow = static_cast<int>(input::OrderCursor::normal);
        expect_cursor(point[0], point[1], reclaim, on_tree);

        open_engine_settings_in_match();
        if (engine_settings_dialog() == nullptr || !oa_layer().modal_shown(true))
            fail("Settings did not open over the match");
        expect_cursor(point[0], point[1], arrow, on_tree + " with Settings open");
        const auto dialog = oa_layer().placement_of("settings").shown;
        expect_cursor(
            static_cast<float>(dialog.x + dialog.width / 2),
            static_cast<float>(dialog.y + dialog.height / 2),
            arrow,
            "in the middle of Settings"
        );
        tap_key(SDLK_ESCAPE);
        if (engine_settings_dialog() != nullptr)
            fail("Escape did not close Settings");
        if (OaLayer::match_button_shown(*this)) {
            const auto button =
                OaLayer::match_button_rect(match_layout_, OaLayer::match_fits_safe_area(*this));
            expect_cursor(
                static_cast<float>(button.x + button.width / 2),
                static_cast<float>(button.y + button.height / 2),
                arrow,
                "on the in-game menu's OA Settings button"
            );
        }
        resume_match_pause();
        if (match_paused_ || ingame_menu_column_shown())
            fail("the in-game menu did not resume");
        expect_cursor(point[0], point[1], reclaim, on_tree + " once Settings closed");

        const float beside_x = point[0] + kBesideMark;
        send(SDL_EVENT_MOUSE_MOTION, 0, beside_x, point[1]);
        const auto beside = shown_cursor();
        MarkLook look{};
        look.name = "reclaim-cursor-mark";
        look.opaque = true;
        look.in_match = true;
        const auto room = oa_layer().view().room;
        look.area = {
            static_cast<int32_t>(point[0]) - room.x - kMarkHalf,
            static_cast<int32_t>(point[1]) - room.y - kMarkHalf,
            2 * kMarkHalf,
            2 * kMarkHalf
        };
        look.mark = {0, 0, 2 * kMarkHalf, 2 * kMarkHalf};
        oa_layer().push(std::make_unique<MarkScreen>(look, std::make_shared<MarkSeen>()));
        const LayerScreen* mark = oa_layer().find(look.name);
        if (mark == nullptr || oa_layer().placement_of(look.name).shown.width <= 0 ||
            oa_layer().modal_shown(true))
            fail("the screen that is not modal does not show over the match");
        expect_cursor(point[0], point[1], arrow, on_tree + " under a screen that is not modal");
        expect_cursor(beside_x, point[1], beside, "beside that screen");
        oa_layer().close(mark);
        expect_cursor(point[0], point[1], reclaim, on_tree + " once that screen closed");
        std::cout << "reclaim cursor check: " << tree_at
                  << " shows the arrow under Settings and under a screen that is not modal, "
                     "and Reclaim again once each closed\n";
    };

    std::size_t reclaim_samples = 0;
    std::size_t move_samples = 0;
    for (const auto& feature : hovered) {
        const auto& def = *feature.def;
        const auto centre_x =
            feature.cell_x * OA_MAP_CELL_PIXELS + def.footprint_x * OA_MAP_CELL_PIXELS / 2;
        const auto centre_z =
            feature.cell_z * OA_MAP_CELL_PIXELS + def.footprint_z * OA_MAP_CELL_PIXELS / 2;
        match_camera_x_ = centre_x - visible_map_width() / 2;
        match_camera_z_ = centre_z - visible_map_height() / 2;
        render_match_surface();
        const auto view = viewport();
        int32_t left = std::numeric_limits<int32_t>::max();
        int32_t top = left;
        int32_t right = std::numeric_limits<int32_t>::min();
        int32_t bottom = right;
        for (int32_t row = 0; row <= def.footprint_z; ++row)
            for (int32_t column = 0; column <= def.footprint_x; ++column) {
                const auto x =
                    static_cast<uint32_t>((feature.cell_x + column) * OA_MAP_CELL_PIXELS);
                const auto z = static_cast<uint32_t>((feature.cell_z + row) * OA_MAP_CELL_PIXELS);
                const auto height = std::max(
                    match_->map_height(x << 16U, z << 16U),
                    static_cast<int32_t>(world.game.sea_level)
                );
                const auto screen = project_match_point(
                    view, {x << 16U, static_cast<uint32_t>(height) << 16U, z << 16U}
                );
                left = std::min(left, screen.x);
                right = std::max(right, screen.x);
                top = std::min(top, screen.y);
                bottom = std::max(bottom, screen.y);
            }
        std::vector<bool> reached(static_cast<std::size_t>(def.footprint_x * def.footprint_z));
        std::size_t over = 0;
        std::optional<std::array<float, 2>> first_over;
        for (auto y = top - kSampleMargin; y <= bottom + kSampleMargin; y += kSampleStep)
            for (auto x = left - kSampleMargin; x <= right + kSampleMargin; x += kSampleStep) {
                const auto fx = static_cast<float>(x);
                const auto fy = static_cast<float>(y);
                if (x < match_layout_.left || y < match_layout_.top ||
                    x >= match_layout_.left + match_layout_.battlefield_width() ||
                    y >= match_layout_.top + match_layout_.battlefield_height() ||
                    radar_contains(fx, fy))
                    continue;
                send(SDL_EVENT_MOUSE_MOTION, 0, fx, fy);
                if (hovered_ || hovered_match_unit_ != 0)
                    continue;
                const auto ground = match_world_point(fx, fy);
                if (!ground || !match_->point_shown(
                                   match_local_player_,
                                   {static_cast<uint32_t>((*ground)[0]),
                                    static_cast<uint32_t>((*ground)[1]),
                                    static_cast<uint32_t>((*ground)[2])}
                               ))
                    continue;
                const auto cell_x = (*ground)[0] >> 20;
                const auto cell_z = (*ground)[2] >> 20;
                const auto column = cell_x - feature.cell_x;
                const auto row = cell_z - feature.cell_z;
                const bool on_footprint =
                    column >= 0 && row >= 0 && column < def.footprint_x && row < def.footprint_z;
                const auto* plot = oa::world_plot(&world, cell_x, cell_z);
                if (!on_footprint && (plot == nullptr || plot->feature != kNoFeature))
                    continue;
                const auto cursor = static_cast<input::OrderCursor>(pick_match_cursor());
                const auto expected =
                    on_footprint ? input::OrderCursor::reclaim : input::OrderCursor::move;
                if (cursor != expected)
                    fail(
                        "pointer " + std::to_string(x) + "," + std::to_string(y) + " on cell " +
                        std::to_string(cell_x) + "," + std::to_string(cell_z) + " (" +
                        (on_footprint
                             ? feature_name(def) + " at " + std::to_string(feature.cell_x) + "," +
                                   std::to_string(feature.cell_z)
                             : std::string("open ground")) +
                        ") shows cursor " + std::to_string(static_cast<int>(cursor)) + ", not " +
                        std::to_string(static_cast<int>(expected))
                    );
                if (!on_footprint) {
                    ++move_samples;
                    continue;
                }
                reached[static_cast<std::size_t>(row * def.footprint_x + column)] = true;
                ++over;
                if (!first_over)
                    first_over = std::array<float, 2>{fx, fy};
            }
        if (std::find(reached.begin(), reached.end(), false) != reached.end()) {
            std::string cells;
            for (const bool cell : reached)
                cells += cell ? '1' : '0';
            fail(
                "the pointer reached only cells " + cells + " of " + feature_name(def) +
                "'s footprint at " + std::to_string(feature.cell_x) + "," +
                std::to_string(feature.cell_z)
            );
        }
        reclaim_samples += over;
        std::cout << "reclaim cursor check: " << feature_name(def) << " (" << def.footprint_x << "x"
                  << def.footprint_z << ") at cell " << feature.cell_x << "," << feature.cell_z
                  << " shows Reclaim at " << over << " pointer positions\n";
        if (&feature == &hovered.front()) {
            check_layer_cursor(feature, *first_over);
            check_reclaim_click(feature, *first_over);
        }
    }

    std::cout << "reclaim cursor check: " << hovered.size() << " features, " << reclaim_samples
              << " pointer positions over their footprints show Reclaim, " << move_samples
              << " over open ground Move; a RECLAIM click on " << feature_name(*hovered.front().def)
              << " gave the commander Reclaim\n";

    // Under the fog, as 3.1c does: a tree out of sight on mapped ground
    // shows Reclaim and a click on it gives the commander a Reclaim, which it
    // carries out; a tree on ground never mapped shows Move and a click on it
    // gives a Move.
    if ((slots[commander].unit->flags & OA_UNIT_FLAG_SELECTED) == 0)
        fail("lost the commander's selection");
    if (world.game.interface_type != input::interface_left_click)
        fail("needs the left-click interface");
    match_->stop_orders(commander);
    match_command_ = MatchCommand::none;
    const auto& spatial = match_->spatial();
    const auto map_width = static_cast<int32_t>(spatial.terrain_width);
    const auto map_height = static_cast<int32_t>(spatial.terrain_height);
    const auto footprint_unseen = [&](int32_t x, int32_t z, const oa::FeatureDef& def) {
        for (int32_t row = -1; row <= def.footprint_z; ++row)
            for (int32_t column = -1; column <= def.footprint_x; ++column)
                if (x + column >= 0 && z + row >= 0 && x + column < map_width &&
                    z + row < map_height && visible_cell(x + column, z + row))
                    return false;
        return true;
    };
    // The nearest vegetation out of sight, far enough from `away`, that the
    // view can centre on.
    const auto edge_x = visible_map_width() / (2 * OA_MAP_CELL_PIXELS) + 1;
    const auto edge_z = visible_map_height() / (2 * OA_MAP_CELL_PIXELS) + 1;
    const auto unseen_tree = [&](const Hovered* away) -> std::optional<Hovered> {
        for (int32_t ring = 0; ring < std::max(map_width, map_height); ++ring)
            for (int32_t dz = -ring; dz <= ring; ++dz)
                for (int32_t dx = -ring; dx <= ring; ++dx) {
                    if (dx != -ring && dx != ring && dz != -ring && dz != ring)
                        continue;
                    const auto x = commander_x + dx;
                    const auto z = commander_z + dz;
                    const auto word = origin_word(x, z);
                    if (word >= world.feature_def_count)
                        continue;
                    const auto& def = world.feature_defs[word];
                    if ((def.flags & OA_FEATURE_FLAG_RECLAIMABLE) == 0 ||
                        (def.flags & OA_FEATURE_FLAG_FLAMABLE) == 0 || x < edge_x || z < edge_z ||
                        x + def.footprint_x > map_width - edge_x ||
                        z + def.footprint_z > map_height - edge_z)
                        continue;
                    if (away != nullptr &&
                        std::max(std::abs(x - away->cell_x), std::abs(z - away->cell_z)) <
                            kFogTreesApartCells)
                        continue;
                    if (footprint_unseen(x, z, def))
                        return Hovered{x, z, &def};
                }
        return std::nullopt;
    };
    // Marks the sight cells around a tree mapped by the player watching, or
    // never mapped.
    auto& sight = match_->sight_mutable();
    const auto viewer_bit = static_cast<uint16_t>(1U << sight.viewpoint_player);
    const auto map_around = [&](const Hovered& tree, bool mapped) {
        const auto& def = *tree.def;
        const auto first_x =
            std::max(0, (tree.cell_x - kFogMarginCells) * OA_MAP_CELL_PIXELS / kSightCellPixels);
        const auto last_x = std::min(
            sight.width - 1,
            (tree.cell_x + def.footprint_x + kFogMarginCells) * OA_MAP_CELL_PIXELS /
                kSightCellPixels
        );
        const auto first_z = std::max(
            0,
            ((tree.cell_z - kFogMarginCells) * OA_MAP_CELL_PIXELS - kHalfTallestGround) /
                kSightCellPixels
        );
        const auto last_z = std::min(
            sight.height - 1,
            (tree.cell_z + def.footprint_z + kFogMarginCells) * OA_MAP_CELL_PIXELS /
                kSightCellPixels
        );
        for (auto z = first_z; z <= last_z; ++z)
            for (auto x = first_x; x <= last_x; ++x) {
                auto& bits = sight.player_bits[static_cast<std::size_t>(z * sight.width + x)];
                bits = mapped ? static_cast<uint16_t>(bits | viewer_bit)
                              : static_cast<uint16_t>(bits & ~viewer_bit);
            }
    };
    // Points at the middle of a tree's footprint, checks the cursor and
    // clicks: a Reclaim of the tree on mapped ground, a Move elsewhere.
    const auto click_unseen_tree = [&](const Hovered& tree, bool mapped) {
        const auto& def = *tree.def;
        const auto where = feature_name(def) + " at " + std::to_string(tree.cell_x) + "," +
                           std::to_string(tree.cell_z);
        const auto centre_x = static_cast<uint32_t>(
            tree.cell_x * OA_MAP_CELL_PIXELS + def.footprint_x * OA_MAP_CELL_PIXELS / 2
        );
        const auto centre_z = static_cast<uint32_t>(
            tree.cell_z * OA_MAP_CELL_PIXELS + def.footprint_z * OA_MAP_CELL_PIXELS / 2
        );
        match_camera_x_ = static_cast<int32_t>(centre_x) - visible_map_width() / 2;
        match_camera_z_ = static_cast<int32_t>(centre_z) - visible_map_height() / 2;
        render_match_surface();
        const auto height = std::max(
            match_->map_height(centre_x << 16U, centre_z << 16U),
            static_cast<int32_t>(world.game.sea_level)
        );
        const auto screen = project_match_point(
            viewport(), {centre_x << 16U, static_cast<uint32_t>(height) << 16U, centre_z << 16U}
        );
        const auto x = static_cast<float>(screen.x);
        const auto y = static_cast<float>(screen.y);
        send(SDL_EVENT_MOUSE_MOTION, 0, x, y);
        const auto ground = match_world_point(x, y);
        if (hovered_ || hovered_match_unit_ != 0 || !ground)
            fail("the pointer found no open ground over " + where);
        const auto column = ((*ground)[0] >> 20) - tree.cell_x;
        const auto row = ((*ground)[2] >> 20) - tree.cell_z;
        if (column < 0 || row < 0 || column >= def.footprint_x || row >= def.footprint_z)
            fail("the pointer missed the footprint of " + where);
        const std::array<uint32_t, 3> point{
            static_cast<uint32_t>((*ground)[0]),
            static_cast<uint32_t>((*ground)[1]),
            static_cast<uint32_t>((*ground)[2])
        };
        if (match_->point_shown(match_local_player_, point) ||
            match_->point_mapped(point) != mapped)
            fail(
                where + " is not out of sight on " +
                (mapped ? "mapped ground" : "ground never mapped")
            );
        const auto cursor = static_cast<input::OrderCursor>(pick_match_cursor());
        const auto shows = mapped ? input::OrderCursor::reclaim : input::OrderCursor::move;
        if (cursor != shows)
            fail(
                where + " under the fog shows cursor " + std::to_string(static_cast<int>(cursor)) +
                ", not " + std::to_string(static_cast<int>(shows))
            );
        click(x, y);
        std::vector<oa::sim::match_runtime::Match::QueuedCommandView> queue;
        match_->visit_primary_queue(commander, [&](const auto& order) { queue.push_back(order); });
        const auto kind = mapped ? kReclaimKind : oa::sim::ground_orders::move_ground_kind;
        if (queue.empty() || queue.front().kind != kind)
            fail("a click on " + where + " under the fog gave no " + (mapped ? "Reclaim" : "Move"));
        if (mapped && ((queue.front().destination[0] >> 20) - tree.cell_x < 0 ||
                       (queue.front().destination[2] >> 20) - tree.cell_z < 0 ||
                       (queue.front().destination[0] >> 20) - tree.cell_x >= def.footprint_x ||
                       (queue.front().destination[2] >> 20) - tree.cell_z >= def.footprint_z))
            fail("the Reclaim of " + where + " is aimed off its footprint");
    };

    const auto fogged = unseen_tree(nullptr);
    if (!fogged)
        fail("found no vegetation out of the commander's sight");
    const auto unmapped = unseen_tree(&*fogged);
    if (!unmapped)
        fail("found no second vegetation out of the commander's sight");
    map_around(*fogged, true);
    map_around(*unmapped, false);
    click_unseen_tree(*unmapped, false);
    std::cout << "reclaim cursor check: " << feature_name(*unmapped->def) << " at cell "
              << unmapped->cell_x << "," << unmapped->cell_z
              << " on ground never mapped shows Move, and a click there gives a Move\n";
    match_->stop_orders(commander);
    click_unseen_tree(*fogged, true);
    const auto fogged_word = origin_word(fogged->cell_x, fogged->cell_z);
    int reclaimed_tick = 0;
    for (int tick = 1; tick <= kFogReclaimTicks && reclaimed_tick == 0; ++tick) {
        step_match_simulation();
        if (origin_word(fogged->cell_x, fogged->cell_z) != fogged_word)
            reclaimed_tick = tick;
    }
    if (reclaimed_tick == 0)
        fail(
            "the commander did not reclaim " + feature_name(*fogged->def) + " within " +
            std::to_string(kFogReclaimTicks) + " ticks"
        );
    std::cout << "reclaim cursor check: " << feature_name(*fogged->def) << " at cell "
              << fogged->cell_x << "," << fogged->cell_z
              << " out of sight on mapped ground shows Reclaim, a click there gives a Reclaim, "
                 "and the commander reclaimed it by tick "
              << reclaimed_tick << "\n";

    // A construction kbot and a construction aircraft placed south of the
    // first tree are given its Reclaim with a RECLAIM click; once the aircraft
    // is flying, the overlays Shift shows are drawn into a picture. Each order
    // stands on the ground where the tree does, so that its marker is drawn
    // on the tree and the aircraft's path comes down to it.
    const auto& tree = hovered.front();
    const auto& tree_def = *tree.def;
    const auto stands = oa::sim::feature_runtime::feature_center(
        world, static_cast<int16_t>(tree.cell_x), static_cast<int16_t>(tree.cell_z), tree_def
    );
    const auto place = [&](const char* name, int32_t apart) {
        const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
        if (type == 0)
            fail(std::string("the game has no ") + name);
        const auto x = static_cast<uint32_t>((stands.x >> 16) - apart);
        const auto z = static_cast<uint32_t>((stands.z >> 16) + apart);
        oa::sim::unit_spawn::Request request;
        request.player = match_local_player_;
        request.type = type;
        request.finished = true;
        request.state = kGroundOccupancyState;
        request.position = {
            x << 16U,
            static_cast<uint32_t>(std::max(match_->map_height(x << 16U, z << 16U), 0)) << 16U,
            z << 16U
        };
        auto* slot = match_->create(request);
        if (slot == nullptr || slot->unit == nullptr)
            fail(std::string("could not place ") + name);
        slot->unit->object_present = true;
        return slot->unit_index;
    };
    match_->stop_orders(commander);
    clear_local_selection();
    const auto kbot = place("ARMCK", kKbotApart);
    const auto aircraft = place("ARMCA", kAircraftApart);
    adopt_selection(kbot);
    adopt_selection(aircraft);
    match_camera_x_ = (stands.x >> 16) - visible_map_width() / 2;
    match_camera_z_ = (stands.z >> 16) - visible_map_height() / 2;
    render_match_surface();
    const auto on_tree = project_match_point(
        viewport(),
        {static_cast<uint32_t>(stands.x),
         static_cast<uint32_t>(std::max(stands.y, 0)),
         static_cast<uint32_t>(stands.z)}
    );
    match_command_ = MatchCommand::reclaim;
    click(static_cast<float>(on_tree.x), static_cast<float>(on_tree.y));
    for (int tick = 0; tick < kAircraftFlightTicks; ++tick)
        step_match_simulation();
    // The aircraft's path pips are drawn as for the unit in the order panel.
    world.game.panel_unit_id = aircraft;
    render_match_surface();
    oa::present::world_renderer::Surface picture{
        match_world_cpu_.width, match_world_cpu_.height, match_world_cpu_.rgb
    };
    auto drawn_view = viewport();
    drawn_view.destination_x = 0;
    drawn_view.destination_y = 0;
    drawn_view.surface_width = picture.width;
    drawn_view.surface_height = picture.height;
    const auto drawn = draw_order_overlays(picture, drawn_view);
    write_png(kOverlayPicture, picture);
    const auto aimed_on_tree = [&](uint16_t unit, uint8_t kind, const std::string& who) {
        std::vector<oa::sim::match_runtime::Match::QueuedCommandView> queue;
        match_->visit_primary_queue(unit, [&](const auto& order) { queue.push_back(order); });
        if (queue.empty() || queue.front().kind != kind)
            fail(
                "a RECLAIM click on " + feature_name(tree_def) + " gave the " + who + " no Reclaim"
            );
        const auto& aim = queue.front().destination;
        if (aim[0] != stands.x || aim[1] != stands.y || aim[2] != stands.z)
            fail(
                "the " + who + "'s Reclaim of " + feature_name(tree_def) + " is aimed at " +
                std::to_string(aim[0] >> 16) + "," + std::to_string(aim[1] >> 16) + "," +
                std::to_string(aim[2] >> 16) + ", not on the tree at " +
                std::to_string(stands.x >> 16) + "," + std::to_string(stands.y >> 16) + "," +
                std::to_string(stands.z >> 16) + " (" + kOverlayPicture + ")"
            );
    };
    aimed_on_tree(kbot, kReclaimKind, "construction kbot");
    aimed_on_tree(aircraft, kAirReclaimKind, "construction aircraft");
    if (drawn.sprites < 2)
        fail("the order overlays drew no Reclaim markers");
    std::cout << "reclaim cursor check: a construction kbot's and a construction aircraft's "
                 "Reclaim of "
              << feature_name(tree_def) << " stand on the tree, at height " << (stands.y >> 16)
              << ", and draw their markers there (" << kOverlayPicture << ")\n";
}

} // namespace oa::app
