// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The overlays a profile's visual rules paint over the battlefield, for
// --check-render-tiers under a profile that turns them on: the whiteboard's
// marks and the build tools' line of sites, each where the frame draws its
// map points, in every tier, at a zoom the area pass reduces, at zoom 1, at a
// zoom the card magnifies and with the view between map pixels; the
// whiteboard's pointer finding the map pixel drawn under it; a marker's
// label in the modern fonts over the battlefield the graphics card draws,
// its shadow, outline and letter edges the card's; the building being placed drawn over its
// site at the scene's draw scale, or by the card; the megamap opening only
// with the Mouse wheel zoom setting off and closing when it is turned on;
// the megamap presented as the processor composes it; text in the modern
// fonts with the chat's backdrop presented as composed, what of it lies
// beside the backdrop the card's in the Full tier; and a named screenshot
// keeping the standard tier's picture.
#include "oa/app/runtime.hpp"

#include "engine_settings_state.hpp"
#include "full_presentation.hpp"
#include "oa/present/game_text.hpp"
#include "oa/sim/messages.hpp"
#include "oa/sim/unit_spawn/spawn_runtime.hpp"
#include "oa/ui/hud/shared_views.hpp"
#include "oa/ui/hud/whiteboard.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

namespace {

/// The zooms the overlays are checked at: one the area pass reduces, the
/// whole zoom, and one the card magnifies.
constexpr std::array<float, 3> overlay_zooms{0.5F, 1.0F, 2.5F};
/// The Full tier's zooms: its zoom floor, a sixth, where the card draws a
/// view the processor never does, and the others.
constexpr std::array<float, 4> full_overlay_zooms{kMinFullBattlefieldZoom, 0.5F, 1.0F, 2.5F};
/// The zoom, and the view's place past the camera's map pixel, the overlays
/// are checked at with the view between map pixels: three quarters of a map
/// pixel, three screen pixels at that zoom.
constexpr float between_zoom = 4.0F;
constexpr double between_offset = 0.75;
/// The zooms the megamap is checked at, where the card would reduce or
/// magnify the battlefield under it.
constexpr std::array<float, 2> megamap_zooms{0.5F, 2.5F};
/// The Full tier's, with its zoom floor.
constexpr std::array<float, 3> full_megamap_zooms{kMinFullBattlefieldZoom, 0.5F, 2.5F};
/// The share of a mark's inside, its edge left out, that must show the
/// mark's colour in the presented frame: where a painter leaves a pixel as
/// the scene had it the card shows its own magnification of the scene there.
constexpr double least_shown_share = 0.9;
/// Cells right of the check's unit the line of sites starts at, and the
/// buildings it lays.
constexpr int32_t line_start_cells = 4;
constexpr int32_t line_buildings = 3;
/// Buildings the line of sites is laid with and the build preview shows,
/// the first the game's data has.
constexpr std::array<std::string_view, 4> line_types{
    "ARMSOLAR", "CORSOLAR", "ARMMSTOR", "CORMSTOR"
};
/// Cells right of the check's unit the pointer places the previewed
/// building at: the first of preview_search_cells from preview_cells on
/// whose site the game accepts, where alone the preview shows.
constexpr int32_t preview_cells = 8;
constexpr int32_t preview_search_cells = 24;
/// A chat line with characters the 8-bit fonts lack, which the modern
/// fonts draw.
constexpr std::string_view modern_text_line = "Gr\xc3\xbc\xc3\x9f"
                                              "e \xe6\x97\xa5\xe6\x9c\xac";
/// The label of the whiteboard marker whose shadow the Full tier's card
/// draws: characters the 8-bit fonts lack, which the modern fonts draw
/// whatever the settings say where game text holds UTF-8.
constexpr std::string_view marker_label = "Mark \xe6\x97\xa5\xe6\x9c\xac";
/// Most a channel each of the card's quads over a pixel may move it from
/// the blend computed exactly: the renderer rounds the blend its own way.
constexpr int most_quad_rounding = 2;

/// A copy of the world layer.
struct WorldCopy {
    uint32_t width{};
    uint32_t height{};
    std::vector<uint8_t> rgb;
};

/// The pixels of one world layer that differ from another's of the same size.
struct Changed {
    std::size_t pixels{}; ///< how many differ
    int left{std::numeric_limits<int>::max()};
    int top{std::numeric_limits<int>::max()};
    int right{std::numeric_limits<int>::min()};  ///< the last column that differs
    int bottom{std::numeric_limits<int>::min()}; ///< the last row that differs
};

/// Finds the pixels of one world layer that differ from another's.
///
/// @param before the layer without the overlay
/// @param after the layer with it, of the same size
/// @return the pixels that differ and the rectangle around them
Changed changed_between(const WorldCopy& before, const WorldCopy& after) {
    Changed changed;
    if (before.width != after.width || before.height != after.height ||
        before.rgb.size() != after.rgb.size())
        return changed;
    for (uint32_t y = 0; y < after.height; ++y)
        for (uint32_t x = 0; x < after.width; ++x) {
            const std::size_t at = (std::size_t{y} * after.width + x) * 3U;
            if (std::equal(
                    before.rgb.begin() + static_cast<std::ptrdiff_t>(at),
                    before.rgb.begin() + static_cast<std::ptrdiff_t>(at + 3U),
                    after.rgb.begin() + static_cast<std::ptrdiff_t>(at)
                ))
                continue;
            ++changed.pixels;
            changed.left = std::min(changed.left, static_cast<int>(x));
            changed.top = std::min(changed.top, static_cast<int>(y));
            changed.right = std::max(changed.right, static_cast<int>(x));
            changed.bottom = std::max(changed.bottom, static_cast<int>(y));
        }
    return changed;
}

/// Returns a zoom's text for the check's report: "0.5", "1", "2.5".
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

void Runtime::check_visual_rule_overlays(
    const std::function<void(oa::ui::engine_settings::HardwareAcceleration)>& set_level,
    std::span<const oa::ui::engine_settings::HardwareAcceleration> levels,
    const std::function<void(float)>& at_zoom,
    uint16_t unit
) {
    using oa::ui::engine_settings::HardwareAcceleration;
    const auto fail = [](const std::string& what) {
        throw std::runtime_error("render tiers check: visual rules: " + what);
    };
    if (!match_ || !selected_tnt_)
        fail("needs a match");
    if (levels.empty())
        fail("names no tier");
    const auto& rules = ui_rules();
    if (!rules.whiteboard.enabled && !rules.build_tools.enabled && !rules.megamap.enabled &&
        !rules.text_rendering.enabled) {
        std::cout << "render tiers check: visual rules: the profile turns none on\n";
        return;
    }
    // The tiers, each by the level that draws in it.
    const auto checks = [&](HardwareAcceleration level) {
        return std::find(levels.begin(), levels.end(), level) != levels.end();
    };
    const auto tier_name = [](HardwareAcceleration level) -> std::string {
        switch (level) {
        case HardwareAcceleration::off:
            return "in the standard tier";
        case HardwareAcceleration::basic:
            return "in the accelerated tier";
        case HardwareAcceleration::full:
            return "in the full tier";
        }
        return "in the standard tier";
    };
    const auto names_of = [&](std::span<const HardwareAcceleration> named) {
        std::string text;
        for (std::size_t at = 0; at < named.size(); ++at) {
            if (at != 0)
                text += at + 1 == named.size() ? " and " : ", ";
            text += tier_name(named[at]);
        }
        return text;
    };
    const auto tiers_name = [&]() { return names_of(levels); };
    const auto switch_to = [&](HardwareAcceleration level) {
        set_level(level);
        if (level == HardwareAcceleration::full && !full_presentation())
            fail("the full tier did not switch on");
        if (level != HardwareAcceleration::off && !accelerated_presentation())
            fail("the accelerated presentation did not switch on");
        if (level != HardwareAcceleration::full && full_presentation())
            fail("the full tier did not switch off");
    };
    std::vector<HardwareAcceleration> accelerated;
    for (const auto level : levels)
        if (level != HardwareAcceleration::off)
            accelerated.push_back(level);
    // The check's unit, or once it has left the match the local player's
    // first unit still in it.
    const auto& slots = match_->world().slots;
    if (unit >= slots.size() || slots[unit].unit == nullptr) {
        unit = 0;
        for (const auto& slot : slots)
            if (slot.unit_index != 0 && slot.unit != nullptr && slot.record.type_index != 0 &&
                slot.owner_index == match_local_player_) {
                unit = slot.unit_index;
                break;
            }
        if (unit == 0)
            fail("the local player has no unit left");
    }
    // Whole ticks, the interface's clock held and the cursor in the blank
    // corner right of the bottom bar, so that two frames of one moment draw
    // the same battlefield. The cursor steps through its sequence by that
    // clock, however long a frame takes to draw, and where the pointer
    // places the build preview the presented frame shows the cursor over
    // the battlefield. The clock runs again when the check ends, however it
    // ends.
    set_presentation_alpha(1.0F);

    struct HeldClock {
        Runtime& runtime;
        std::optional<uint32_t> kept{};

        ~HeldClock() { runtime.fake_frontend_tick_ = kept; }
    } held_clock{*this, fake_frontend_tick_};

    fake_frontend_tick_ = frontend_tick();
    const auto rest_pointer = [&]() {
        update_pointer(
            static_cast<float>(match_layout_.width - 1),
            static_cast<float>(match_layout_.height - 1)
        );
    };
    rest_pointer();

    // A frame presented and read back, the world layer it drew and its
    // scene: the one drawn apart, or else the world layer itself. In the
    // Full tier the world layer is the overlay canvas, the key colour but
    // where the painters painted, and the painters asked the card for the
    // frame's quads.
    struct Drawn {
        renderer::Surface frame;
        WorldCopy world;
        WorldCopy scene;
        std::vector<FullWorldQuad> quads;
    };

    const auto drawn = [&]() {
        Drawn result;
        capture_frame_ = &result.frame;
        render();
        capture_frame_ = nullptr;
        result.world = {match_world_cpu_.width, match_world_cpu_.height, match_world_cpu_.rgb};
        result.scene =
            accelerated_.frame.apart
                ? WorldCopy{match_scene_cpu_.width, match_scene_cpu_.height, match_scene_cpu_.rgb}
                : result.world;
        if (full_presentation() && full_->drawn)
            result.quads = full_->world_quads;
        return result;
    };
    // The battlefield of a presented frame, from its corner.
    const auto battlefield_of = [&](const Drawn& read) {
        WorldCopy field{
            static_cast<uint32_t>(std::max(0, match_layout_.battlefield_width())),
            static_cast<uint32_t>(std::max(0, match_layout_.battlefield_height())),
            {}
        };
        field.rgb.resize(std::size_t{field.width} * field.height * 3U);
        for (uint32_t y = 0; y < field.height; ++y) {
            const auto row = static_cast<std::size_t>(match_layout_.top) + y;
            if (row >= read.frame.height)
                break;
            const std::size_t from =
                (row * read.frame.width + static_cast<std::size_t>(match_layout_.left)) * 3U;
            const std::size_t bytes =
                std::min<std::size_t>(field.width, read.frame.width - match_layout_.left) * 3U;
            std::copy_n(
                read.frame.rgb.begin() + static_cast<std::ptrdiff_t>(from),
                bytes,
                field.rgb.begin() + static_cast<std::ptrdiff_t>(std::size_t{y} * field.width * 3U)
            );
        }
        return field;
    };

    // The pixels a Full frame's painters left to the card that changed from
    // one frame to another of the same moment, each either drawn as the
    // painters' quads over it ask, replayed over the earlier frame's pixel,
    // or not: the canvas holds the key colour there, and the card alone
    // changed what the frame shows.
    struct Shadowing {
        std::size_t painted{}; ///< pixels the painters painted on the canvas
        std::size_t drawn{};   ///< left to the card and drawn as the quads over them ask
        std::size_t other{};   ///< left to the card and changed otherwise
    };

    const auto shadowing = [&](const Drawn& before, const Drawn& after) {
        Shadowing found;
        const auto key = full_overlay_key();
        const auto field_before = battlefield_of(before);
        const auto field_after = battlefield_of(after);
        if (after.world.width != field_after.width || after.world.height != field_after.height)
            fail("the full tier's canvas is not the battlefield's size");
        auto expected = field_before.rgb;
        const auto over = replay_world_quads(
            after.quads,
            full_->executor.capabilities().minimum_composed,
            expected,
            field_after.width,
            field_after.height,
            0,
            0
        );
        for (std::size_t at = 0; at + 3U <= after.world.rgb.size(); at += 3U) {
            const uint8_t* canvas = after.world.rgb.data() + at;
            if (!std::equal(key.begin(), key.end(), canvas)) {
                ++found.painted;
                continue;
            }
            const uint8_t* was = field_before.rgb.data() + at;
            const uint8_t* is = field_after.rgb.data() + at;
            if (std::equal(was, was + 3, is))
                continue;
            const int quads = over[at / 3U];
            bool as_asked = quads != 0;
            for (std::size_t channel = 0; as_asked && channel < 3; ++channel)
                as_asked = std::abs(int{is[channel]} - int{expected[at + channel]}) <=
                           most_quad_rounding * quads;
            if (as_asked)
                ++found.drawn;
            else
                ++found.other;
        }
        return found;
    };
    // Where the frame's painters after the fog place map points: from the
    // battlefield's corner at the world layer's origin, moved by the view's
    // offset to the nearest screen pixel.
    const auto painted_view = [&]() {
        const auto zoom = static_cast<double>(match_zoom());
        auto viewport = live_viewport(match_camera_x_, match_camera_z_);
        viewport.destination_x =
            -static_cast<int32_t>(std::lround(accelerated_.frame_offset.x * zoom));
        viewport.destination_y =
            -static_cast<int32_t>(std::lround(accelerated_.frame_offset.y * zoom));
        viewport.surface_width = static_cast<uint32_t>(match_layout_.battlefield_width());
        viewport.surface_height = static_cast<uint32_t>(match_layout_.battlefield_height());
        return viewport;
    };
    // A palette entry as the presented frame shows it, through the gamma.
    const auto shown = [&](uint8_t index) {
        const auto pal = static_cast<std::size_t>(index) * 4U;
        std::array<uint8_t, 3> color{
            match_palette_[pal], match_palette_[pal + 1], match_palette_[pal + 2]
        };
        if (!gamma_identity_)
            for (auto& channel : color)
                channel = gamma_table_[channel];
        return color;
    };
    // The unit's map pixel, from its 16.16 place.
    const auto& position = slots[unit].unit->position;
    const uint32_t place_x = position[0];
    const uint32_t place_z = position[2];
    const int32_t unit_x = std::bit_cast<int32_t>(place_x) >> 16;
    const int32_t unit_z = std::bit_cast<int32_t>(place_z) >> 16;
    // The view moved three quarters of a map pixel past the camera's, as a
    // slow scroll leaves it; false, said so, where the map leaves no room.
    const auto between_pixels = [&]() {
        at_zoom(between_zoom);
        std::ignore = drawn();
        const auto camera = view_camera();
        scroll_match_view(1, 0, between_offset * static_cast<double>(between_zoom));
        std::ignore = drawn();
        if (view_camera() == camera && accelerated_.frame_offset.x == between_offset)
            return true;
        std::cout << "render tiers check: visual rules: the map leaves no room to move the view "
                     "between map pixels\n";
        return false;
    };
    // The view between map pixels is the accelerated tier's alone: the
    // standard tier and the Full tier draw on the camera's map pixel.
    const bool basic_checked = checks(HardwareAcceleration::basic);
    // The zooms of a tier: the Full tier's reach to its own floor.
    const auto zooms_of = [](HardwareAcceleration level) -> std::span<const float> {
        if (level == HardwareAcceleration::full)
            return full_overlay_zooms;
        return overlay_zooms;
    };

    // ui.whiteboard: a dot at the unit's map point is drawn where the frame
    // draws that point, and the pointer over the map pixel drawn there finds it.
    if (rules.whiteboard.enabled) {
        const uint8_t color = oa::ui::hud::player_dot_color(match_->state(), match_local_player_);
        const auto expected = shown(color);
        const int32_t half = oa::ui::hud::kWhiteboardDotSide / 2;
        const auto check_dot = [&](const std::string& which) {
            whiteboard_ = {};
            const auto bare = drawn();
            oa::ui::hud::whiteboard_place_marker(whiteboard_, unit_x, unit_z, color, "");
            const auto marked = drawn();
            whiteboard_ = {};
            const auto at = project_match_point(
                painted_view(),
                {static_cast<uint32_t>(unit_x) << 16, 0, static_cast<uint32_t>(unit_z) << 16}
            );
            const auto changed = changed_between(bare.world, marked.world);
            if (changed.pixels == 0)
                fail("the whiteboard's dot was not drawn " + which);
            if (changed.left < at.x - half || changed.top < at.y - half ||
                changed.right >= at.x + half || changed.bottom >= at.y + half)
                fail(
                    "the whiteboard's dot was drawn " + which + " at " +
                    std::to_string(changed.left) + ',' + std::to_string(changed.top) +
                    " where its map point is drawn at " + std::to_string(at.x - half) + ',' +
                    std::to_string(at.y - half)
                );
            // The presented frame shows the dot there, its edge left out.
            std::size_t inside = 0;
            std::size_t matched = 0;
            for (int32_t y = at.y - half + 1; y < at.y + half - 1; ++y)
                for (int32_t x = at.x - half + 1; x < at.x + half - 1; ++x) {
                    const int32_t column = match_layout_.left + x;
                    const int32_t row = match_layout_.top + y;
                    if (column < 0 || row < 0 ||
                        column >= static_cast<int32_t>(marked.frame.width) ||
                        row >= static_cast<int32_t>(marked.frame.height))
                        continue;
                    ++inside;
                    const std::size_t pixel = (static_cast<std::size_t>(row) * marked.frame.width +
                                               static_cast<std::size_t>(column)) *
                                              3U;
                    if (std::equal(
                            expected.begin(),
                            expected.end(),
                            marked.frame.rgb.begin() + static_cast<std::ptrdiff_t>(pixel)
                        ))
                        ++matched;
                }
            if (inside == 0 ||
                static_cast<double>(matched) < least_shown_share * static_cast<double>(inside))
                fail(
                    "the presented frame does not show the whiteboard's dot " + which + ": " +
                    std::to_string(matched) + " of " + std::to_string(inside) + " pixels"
                );
            return at;
        };
        for (const auto level : levels)
            for (const float zoom : zooms_of(level)) {
                switch_to(level);
                at_zoom(zoom);
                std::ignore = check_dot(tier_name(level) + " at zoom " + zoom_name(zoom));
            }
        bool between = false;
        if (basic_checked) {
            switch_to(HardwareAcceleration::basic);
            between = between_pixels();
        }
        if (between) {
            const auto at = check_dot("with the view between map pixels");
            // The pointer over the middle of the map pixel drawn there finds
            // the dot's map pixel.
            const auto zoom = static_cast<float>(match_zoom());
            const auto found = battlefield_map_point(
                static_cast<float>(match_layout_.left + at.x) + zoom / 2.0F,
                static_cast<float>(match_layout_.top + at.y) + zoom / 2.0F
            );
            if (found[0] != unit_x || found[1] != unit_z)
                fail(
                    "the pointer over the dot's map pixel drawn between map pixels finds " +
                    std::to_string(found[0]) + ',' + std::to_string(found[1]) + ", not " +
                    std::to_string(unit_x) + ',' + std::to_string(unit_z)
                );
        }
        // A marker's label in the modern fonts over the battlefield the card
        // draws: the pixels its letters cover whole on the canvas, and its
        // shadow, outline and letter edges over the battlefield the card's
        // quads, since the canvas holds no battlefield to blend with.
        const auto text = game_text_settings();
        const bool label_checked = checks(HardwareAcceleration::full) && modern_fonts_open() &&
                                   text.style.shadow && (text.style.modern_fonts || text.utf8);
        if (label_checked)
            for (const float zoom : full_overlay_zooms) {
                switch_to(HardwareAcceleration::full);
                at_zoom(zoom);
                whiteboard_ = {};
                const auto bare = drawn();
                oa::ui::hud::whiteboard_place_marker(
                    whiteboard_, unit_x, unit_z, color, std::string(marker_label)
                );
                const auto marked = drawn();
                whiteboard_ = {};
                const auto found = shadowing(bare, marked);
                if (found.painted == 0 || found.drawn == 0 || found.other != 0)
                    fail(
                        "the marker's label in the full tier at zoom " + zoom_name(zoom) +
                        " painted " + std::to_string(found.painted) + " pixels on the canvas, " +
                        "and the card drew " + std::to_string(found.drawn) +
                        " pixels of the battlefield as its quads ask and changed " +
                        std::to_string(found.other) + " otherwise"
                    );
            }
        rest_pointer();
        std::cout << "render tiers check: visual rules: the whiteboard's dot is drawn where its "
                     "map point is, "
                  << tiers_name() << (between ? ", and between map pixels" : "")
                  << (label_checked ? "; a label's shadow, outline and letter edges are the "
                                      "card's in the full tier"
                                    : "")
                  << '\n';
    }

    // The building the line of sites is laid with and the preview shows.
    uint16_t type = 0;
    for (const auto name : line_types)
        if (const auto index = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
            index != 0 && index < spawn_types_.size()) {
            type = static_cast<uint16_t>(index);
            break;
        }

    // ui.build-tools: a line of sites is outlined where the frame draws them.
    if (rules.build_tools.enabled) {
        if (type == 0) {
            std::cout << "render tiers check: visual rules: the data has no building to lay a "
                         "line of\n";
        } else {
            const auto check_line = [&](const std::string& which) {
                match_command_ = MatchCommand::none;
                pending_build_type_ = 0;
                build_tool_ = {};
                const auto bare = drawn();
                match_command_ = MatchCommand::build;
                pending_build_type_ = type;
                const auto footprint = pending_build_footprint();
                auto& tool = build_tool_;
                tool.drawing_line = true;
                tool.footprint_x = footprint[0];
                tool.footprint_z = footprint[1];
                tool.start_x = unit_x + line_start_cells * OA_MAP_CELL_PIXELS;
                tool.start_z = unit_z;
                tool.end_x =
                    tool.start_x + (line_buildings - 1) * footprint[0] * OA_MAP_CELL_PIXELS;
                tool.end_z = unit_z;
                lay_build_tool();
                const auto lined = drawn();
                const auto viewport = painted_view();
                // Every site's outline: its footprint's corners raised by its height.
                int left = std::numeric_limits<int>::max();
                int top = std::numeric_limits<int>::max();
                int right = std::numeric_limits<int>::min();
                int bottom = std::numeric_limits<int>::min();
                std::size_t sites = 0;
                for (const auto& slot : tool.layout.slots) {
                    const auto site =
                        pending_build_site({int32_t{slot.x} << 16, 0, int32_t{slot.z} << 16});
                    if (!site)
                        continue;
                    const auto height = static_cast<uint32_t>(site->world[1]);
                    const auto corner = [](int32_t cell) {
                        return static_cast<uint32_t>(cell * OA_MAP_CELL_PIXELS) << 16;
                    };
                    const auto from = project_match_point(
                        viewport, {corner(site->cell_x), height, corner(site->cell_z)}
                    );
                    const auto to = project_match_point(
                        viewport,
                        {corner(site->cell_x + site->footprint_x),
                         height,
                         corner(site->cell_z + site->footprint_z)}
                    );
                    left = std::min(left, from.x);
                    top = std::min(top, from.y);
                    right = std::max(right, to.x);
                    bottom = std::max(bottom, to.y);
                    ++sites;
                }
                match_command_ = MatchCommand::none;
                pending_build_type_ = 0;
                build_tool_ = {};
                if (sites != static_cast<std::size_t>(line_buildings))
                    fail(
                        "the build tools laid " + std::to_string(sites) + " sites " + which +
                        ", not " + std::to_string(line_buildings)
                    );
                const auto changed = changed_between(bare.world, lined.world);
                // The outlines' outer edges are the sites' own, clipped to
                // the battlefield.
                const int width = static_cast<int>(lined.world.width);
                const int height = static_cast<int>(lined.world.height);
                const int clip_left = std::max(left, 0);
                const int clip_top = std::max(top, 0);
                const int clip_right = std::min(right, width - 1);
                const int clip_bottom = std::min(bottom, height - 1);
                if (changed.pixels == 0 || changed.left != clip_left || changed.top != clip_top ||
                    changed.right != clip_right || changed.bottom != clip_bottom)
                    fail(
                        "the line of sites " + which + " was outlined over " +
                        std::to_string(changed.left) + ',' + std::to_string(changed.top) + " to " +
                        std::to_string(changed.right) + ',' + std::to_string(changed.bottom) +
                        ", not where the sites are drawn, " + std::to_string(clip_left) + ',' +
                        std::to_string(clip_top) + " to " + std::to_string(clip_right) + ',' +
                        std::to_string(clip_bottom)
                    );
            };
            for (const auto level : levels)
                for (const float zoom : zooms_of(level)) {
                    switch_to(level);
                    at_zoom(zoom);
                    check_line(tier_name(level) + " at zoom " + zoom_name(zoom));
                }
            bool between = false;
            if (basic_checked) {
                switch_to(HardwareAcceleration::basic);
                between = between_pixels();
            }
            if (between)
                check_line("with the view between map pixels");
            rest_pointer();
            std::cout << "render tiers check: visual rules: the build tools' line of sites is "
                         "outlined where the sites are drawn, "
                      << tiers_name() << (between ? ", and between map pixels" : "") << '\n';
        }
    }

    // ui.build-preview: the building being placed is drawn into the scene
    // at the scene's own draw scale, over the site the pointer is on; in
    // the Full tier the card draws it, at the zoom.
    if (rules.build_preview.enabled && type != 0) {
        // The pointer over a site the game accepts, found once: a site is
        // the same at every tier and zoom.
        std::optional<int32_t> preview_x;
        switch_to(levels.front());
        at_zoom(1.0F);
        match_command_ = MatchCommand::build;
        pending_build_type_ = type;
        build_tool_ = {};
        for (int32_t cells = preview_cells;
             cells < preview_cells + preview_search_cells && !preview_x;
             ++cells) {
            const int32_t x = unit_x + cells * OA_MAP_CELL_PIXELS;
            const auto pointed = project_match_point(
                painted_view(),
                {static_cast<uint32_t>(x) << 16, 0, static_cast<uint32_t>(unit_z) << 16}
            );
            update_pointer(
                static_cast<float>(match_layout_.left + pointed.x),
                static_cast<float>(match_layout_.top + pointed.y)
            );
            if (const auto site = build_site_under(match_pointer_x_, match_pointer_y_);
                site && site->legal)
                preview_x = x;
        }
        match_command_ = MatchCommand::none;
        pending_build_type_ = 0;
        if (!preview_x)
            fail("found no site the game accepts for the build preview");
        for (const auto level : levels)
            for (const float zoom : zooms_of(level)) {
                switch_to(level);
                at_zoom(zoom);
                const bool card = level == HardwareAcceleration::full;
                const std::string which = tier_name(level) + " at zoom " + zoom_name(zoom);
                const auto pointed = project_match_point(
                    painted_view(),
                    {static_cast<uint32_t>(*preview_x) << 16,
                     0,
                     static_cast<uint32_t>(unit_z) << 16}
                );
                update_pointer(
                    static_cast<float>(match_layout_.left + pointed.x),
                    static_cast<float>(match_layout_.top + pointed.y)
                );
                match_command_ = MatchCommand::none;
                pending_build_type_ = 0;
                build_tool_ = {};
                // The card's frame is held to the same frame without the
                // building: the site's outline and the cursor are the same
                // in both while a line is being started, which holds the
                // preview back.
                if (card) {
                    match_command_ = MatchCommand::build;
                    pending_build_type_ = type;
                    build_tool_.drawing_line = true;
                }
                const auto bare = drawn();
                build_tool_ = {};
                match_command_ = MatchCommand::build;
                pending_build_type_ = type;
                const auto site = build_site_under(match_pointer_x_, match_pointer_y_);
                const auto previewed = drawn();
                match_command_ = MatchCommand::none;
                pending_build_type_ = 0;
                if (!site)
                    fail("the pointer is over no site " + which);
                // The site's middle in the scene: from the camera's map pixel
                // at the scene's draw scale.
                auto scene_view = live_viewport(match_camera_x_, match_camera_z_);
                scene_view.destination_x = 0;
                scene_view.destination_y = 0;
                scene_view.scale = accelerated_.frame.draw_scale;
                const auto middle = project_match_point(
                    scene_view,
                    {static_cast<uint32_t>(site->world[0]),
                     static_cast<uint32_t>(site->world[1]),
                     static_cast<uint32_t>(site->world[2])}
                );
                const auto changed =
                    card ? changed_between(battlefield_of(bare), battlefield_of(previewed))
                         : changed_between(bare.scene, previewed.scene);
                // The card draws the building: the canvas holds none of it.
                if (card) {
                    const auto canvas = changed_between(bare.world, previewed.world);
                    if (canvas.pixels != 0)
                        fail(
                            "the build preview " + which + " changed " +
                            std::to_string(canvas.pixels) + " pixels of the overlay canvas"
                        );
                }
                // How far from the site's middle the building may reach,
                // across and down the scene, at least a pixel each way.
                const auto scale = static_cast<double>(accelerated_.frame.draw_scale);
                const int reach_x = static_cast<int>(
                    std::ceil(site->footprint_x * OA_MAP_CELL_PIXELS * scale / 2.0) + 1
                );
                const int reach_z = static_cast<int>(
                    std::ceil(site->footprint_z * OA_MAP_CELL_PIXELS * scale / 2.0) + 1
                );
                const int centre_x = (changed.left + changed.right) / 2;
                const int centre_y = (changed.top + changed.bottom) / 2;
                if (changed.pixels == 0 || std::abs(centre_x - middle.x) > reach_x ||
                    std::abs(centre_y - middle.y) > 2 * reach_z)
                    fail(
                        "the build preview " + which + " was drawn around " +
                        std::to_string(centre_x) + ',' + std::to_string(centre_y) +
                        " of the scene, where its site's middle is drawn at " +
                        std::to_string(middle.x) + ',' + std::to_string(middle.y)
                    );
            }
        rest_pointer();
        std::cout << "render tiers check: visual rules: the build preview is drawn over its site "
                     "at the scene's draw scale, "
                  << tiers_name() << '\n';
    }

    // ui.megamap acts only with the Mouse wheel zoom setting off: with it
    // on, Tab and the wheel rolled toward the player open no megamap; with
    // it off, Tab opens it; turning the setting on in the match, as the
    // settings dialog does, closes it. The setting stays off for the
    // megamap's presentation below.
    const bool wheel_zoom = engine_settings().wheel_zoom;
    // The setting changed with the battlefield's zoom left as it is.
    const auto set_wheel_zoom = [&](bool on) {
        engine_settings_state().current.wheel_zoom = on;
        megamap_wheel_zoom_changed();
    };
    if (rules.megamap.enabled) {
        SDL_KeyboardEvent tab{};
        tab.type = SDL_EVENT_KEY_DOWN;
        tab.key = SDLK_TAB;
        tab.scancode = SDL_SCANCODE_TAB;
        tab.down = true;
        const auto middle_x =
            static_cast<float>(match_layout_.left + match_layout_.battlefield_width() / 2);
        const auto middle_y =
            static_cast<float>(match_layout_.top + match_layout_.battlefield_height() / 2);
        set_wheel_zoom(true);
        if (take_megamap_toggle(tab) || megamap_wheel(-1.0F, middle_x, middle_y) || megamap_open_)
            fail("the megamap opened with the Mouse wheel zoom setting on");
        set_wheel_zoom(false);
        if (!take_megamap_toggle(tab) || !megamap_shown())
            fail("Tab did not open the megamap with the Mouse wheel zoom setting off");
        auto chosen = engine_settings();
        chosen.wheel_zoom = true;
        apply_engine_settings(chosen);
        if (megamap_open_ || megamap_shown())
            fail("turning the Mouse wheel zoom setting on did not close the megamap");
        set_wheel_zoom(false);
        std::cout << "render tiers check: visual rules: the megamap opens only with the Mouse "
                     "wheel zoom setting off, and turning the setting on closes it\n";
    }

    // ui.megamap: open over the battlefield, it is presented exactly as the
    // processor composes it, whatever zoom the battlefield under it has.
    if (rules.megamap.enabled && !accelerated.empty()) {
        for (const auto level : accelerated) {
            switch_to(level);
            const std::span<const float> zooms = level == HardwareAcceleration::full
                                                     ? std::span<const float>(full_megamap_zooms)
                                                     : std::span<const float>(megamap_zooms);
            for (const float zoom : zooms) {
                at_zoom(zoom);
                set_megamap_open(true);
                const auto read = drawn();
                renderer::Surface composed;
                compose_match_frame(composed);
                set_megamap_open(false);
                if (read.frame.width != composed.width || read.frame.height != composed.height)
                    fail("the presented frame and the composition differ in size over the megamap");
                int most = 0;
                for (int32_t y = 0; y < match_layout_.battlefield_height(); ++y)
                    for (int32_t x = 0; x < match_layout_.battlefield_width(); ++x) {
                        const std::size_t pixel =
                            (static_cast<std::size_t>(match_layout_.top + y) * composed.width +
                             static_cast<std::size_t>(match_layout_.left + x)) *
                            3U;
                        for (std::size_t channel = 0; channel < 3; ++channel)
                            most = std::max(
                                most,
                                std::abs(
                                    static_cast<int>(read.frame.rgb[pixel + channel]) -
                                    static_cast<int>(composed.rgb[pixel + channel])
                                )
                            );
                    }
                if (most != 0)
                    fail(
                        "the megamap " + tier_name(level) + " at zoom " + zoom_name(zoom) +
                        " is not presented as the processor composes it: most " +
                        std::to_string(most)
                    );
            }
        }
        std::cout << "render tiers check: visual rules: the megamap is presented as composed at "
                     "zoom 0.5 and 2.5, "
                  << names_of(accelerated)
                  << (checks(HardwareAcceleration::full) ? ", and at the full tier's floor" : "")
                  << '\n';
    }
    if (rules.megamap.enabled)
        set_wheel_zoom(wheel_zoom);

    // ui.text-rendering: a chat line in the modern fonts, over its
    // backdrop, is presented as the processor composes it, at each zoom; in
    // the Full tier what of it lies beside the backdrop is the card's quads.
    if (rules.text_rendering.enabled && modern_fonts_open() && !accelerated.empty()) {
        auto& game = match_->state().game;
        for (const auto level : accelerated) {
            switch_to(level);
            const bool card = level == HardwareAcceleration::full;
            at_zoom(overlay_zooms.front());
            // The message log before and after the line, so that the line
            // can be shown and taken away again.
            const auto head = game.chat_head;
            const auto tail = game.chat_tail;
            const auto bare = drawn();
            post_match_message(modern_text_line, oa::sim::messages::kind_player_chat);
            const auto posted_head = game.chat_head;
            const auto posted_tail = game.chat_tail;
            const auto posted = drawn();
            // The line's pixels: those it changed on the battlefield, where no
            // zoom moves the message log.
            std::vector<std::size_t> line;
            for (std::size_t pixel = 0; pixel + 3U <= posted.world.rgb.size(); pixel += 3U)
                if (pixel + 3U <= bare.world.rgb.size() &&
                    !std::equal(
                        bare.world.rgb.begin() + static_cast<std::ptrdiff_t>(pixel),
                        bare.world.rgb.begin() + static_cast<std::ptrdiff_t>(pixel + 3U),
                        posted.world.rgb.begin() + static_cast<std::ptrdiff_t>(pixel)
                    ))
                    line.push_back(pixel / 3U);
            if (line.empty())
                fail("the chat line in the modern fonts was not drawn " + tier_name(level));
            std::size_t card_drawn = 0;
            for (const float zoom : overlay_zooms) {
                at_zoom(zoom);
                // The same moment without the line, for the card's shadow.
                Drawn without;
                if (card) {
                    game.chat_head = head;
                    game.chat_tail = tail;
                    without = drawn();
                    game.chat_head = posted_head;
                    game.chat_tail = posted_tail;
                }
                const auto written = drawn();
                renderer::Surface composed;
                compose_match_frame(composed);
                // On a magnified frame the card shows its own magnification of
                // the scene wherever a painter left the scene's picture as it
                // was, so only the pixels the overlay holds are presented as
                // composed; on every other frame, all of them are.
                const bool magnified = accelerated_.frame.method == SceneMethod::magnify;
                std::size_t held = 0;
                for (const std::size_t at : line) {
                    const std::size_t layer = at * 3U;
                    if (magnified && layer + 3U <= accelerated_.base.size() &&
                        std::equal(
                            accelerated_.base.begin() + static_cast<std::ptrdiff_t>(layer),
                            accelerated_.base.begin() + static_cast<std::ptrdiff_t>(layer + 3U),
                            written.world.rgb.begin() + static_cast<std::ptrdiff_t>(layer)
                        ))
                        continue;
                    ++held;
                    const std::size_t pixel =
                        (static_cast<std::size_t>(
                             match_layout_.top + static_cast<int32_t>(at / written.world.width)
                         ) * composed.width +
                         static_cast<std::size_t>(
                             match_layout_.left + static_cast<int32_t>(at % written.world.width)
                         )) *
                        3U;
                    if (!std::equal(
                            composed.rgb.begin() + static_cast<std::ptrdiff_t>(pixel),
                            composed.rgb.begin() + static_cast<std::ptrdiff_t>(pixel + 3U),
                            written.frame.rgb.begin() + static_cast<std::ptrdiff_t>(pixel)
                        ))
                        fail(
                            "the chat line in the modern fonts " + tier_name(level) + " at zoom " +
                            zoom_name(zoom) + " is not presented as composed"
                        );
                }
                if (static_cast<double>(held) <
                    least_shown_share * static_cast<double>(line.size()))
                    fail(
                        "the overlay holds only " + std::to_string(held) + " of the chat line's " +
                        std::to_string(line.size()) + " pixels " + tier_name(level) + " at zoom " +
                        zoom_name(zoom)
                    );
                // Beside what the canvas holds, the line changes the frame
                // only by the quads the card draws for it.
                if (card) {
                    const auto found = shadowing(without, written);
                    if (found.other != 0)
                        fail(
                            "the chat line in the full tier at zoom " + zoom_name(zoom) +
                            " changed " + std::to_string(found.other) +
                            " pixels the canvas leaves to the card otherwise than by its quads"
                        );
                    card_drawn += found.drawn;
                }
            }
            // The line taken away again, so that the checks after this one
            // do not see it.
            game.chat_head = head;
            game.chat_tail = tail;
            if (card)
                std::cout << "render tiers check: visual rules: what of the chat line lies "
                             "beside its backdrop is the card's in the full tier, "
                          << card_drawn << " pixels over the zooms\n";
        }
        std::cout << "render tiers check: visual rules: a chat line in the modern fonts over "
                     "its backdrop is presented as composed at zoom 0.5, 1 and 2.5\n";
    } else if (rules.text_rendering.enabled && !modern_fonts_open()) {
        std::cout << "render tiers check: visual rules: the modern fonts do not open here\n";
    }

    // ui.display-modes: a named screenshot keeps the standard tier's picture.
    if (rules.display_modes.enabled && !accelerated.empty()) {
        for (const auto level : accelerated) {
            switch_to(HardwareAcceleration::off);
            at_zoom(overlay_zooms.back());
            std::ignore = drawn();
            renderer::Surface standard;
            compose_match_frame(standard);
            switch_to(level);
            std::ignore = drawn();
            oa::present::DisplayContext capture{};
            oa::present::SurfaceBuffer frame{};
            if (!indexed_frame(capture, frame))
                fail("the named screenshot found no frame");
            bool same = surface_.width == standard.width && surface_.height == standard.height;
            for (int32_t y = 0; same && y < match_layout_.battlefield_height(); ++y) {
                const std::size_t row =
                    (static_cast<std::size_t>(match_layout_.top + y) * standard.width +
                     static_cast<std::size_t>(match_layout_.left)) *
                    3U;
                same = std::equal(
                    standard.rgb.begin() + static_cast<std::ptrdiff_t>(row),
                    standard.rgb.begin() +
                        static_cast<std::ptrdiff_t>(
                            row + static_cast<std::size_t>(match_layout_.battlefield_width()) * 3U
                        ),
                    surface_.rgb.begin() + static_cast<std::ptrdiff_t>(row)
                );
            }
            if (!same)
                fail(
                    "the named screenshot " + tier_name(level) +
                    " does not keep the standard tier's battlefield"
                );
        }
        std::cout << "render tiers check: visual rules: a named screenshot keeps the standard "
                     "tier's battlefield\n";
    }
    switch_to(levels.back());
}

} // namespace oa::app
