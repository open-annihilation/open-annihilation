// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The megamap's picture (ui.megamap), then its clicks against the
// battlefield's, in both interface types, the clicks of the panels over it,
// and the touch controls' fingers on it, through synthetic SDL input on a
// skirmish.
#include "engine_settings_state.hpp"
#include "oa/app/runtime.hpp"
#include "touch_state.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/sim/match_runtime/attack_orders.hpp"
#include "oa/sim/match_runtime/construction_orders.hpp"
#include "oa/ui/hud/megamap.hpp"
#include "oa/ui/touch_hud.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace input = oa::sim::gameplay_input;
namespace hud = oa::ui::hud;

/// Map pixels between the units the check places, which keeps their icons
/// apart on the megamap and the enemy in the mover's sight.
constexpr int32_t kApart = 192;
/// Ticks the match may run for the enemy to come into sight and onto the
/// radar.
constexpr int kSightTicks = 30;
/// Map pixels the units keep from the map's edges.
constexpr int32_t kEdgeMargin = 96;
/// Map pixels of a footprint cell.
constexpr int32_t cell_pixels = 16;
/// Picture pixels around a drawn feature's spot that its picture may cover
/// beyond its scaled size.
constexpr int32_t feature_reach_margin = 2;
/// Palette index of the bars beside the megamap's picture.
constexpr uint8_t megamap_bar_color = 95;
/// The touch device the check's fingers come from: a fixed id that no device
/// registers, which the dispatcher takes because the check allows unregistered
/// touch.
constexpr SDL_TouchID kCheckTouchDevice = 0x70c4;
constexpr uint64_t kNanosecondsPerMillisecond = 1'000'000;
/// One frame of the fingers' clock, in milliseconds.
constexpr uint64_t kFrameMs = 16;
/// How long a tapping finger rests before it lifts.
constexpr uint64_t kTapRestMs = 60;
/// The pause after a tap, longer than a double tap's window.
constexpr uint64_t kAfterTapMs = 400;
/// How long a held finger rests, past the 350 ms default hold delay, in steps.
constexpr uint64_t kHoldMs = 400;
constexpr uint64_t kHoldStepMs = 100;
/// The moves a dragging finger makes.
constexpr int kDragSteps = 8;
/// Megamap pixels a dragged box reaches past the icons it takes in.
constexpr float kBoxMargin = 12.0F;
/// Map pixels a building's site may lie from the point tapped, snapped to its
/// footprint.
constexpr int32_t kSiteReach = 48;

/// Fails the check, saying what went wrong.
[[noreturn]] void fail(std::string_view what) {
    throw std::runtime_error("megamap click check: " + std::string(what));
}

/// Fails the check, saying what went wrong, unless `ok` holds.
void require(bool ok, std::string_view what) {
    if (!ok)
        fail(what);
}

/// Names a step's frame: <stem>-<step>.ppm beside --snapshot.
fs::path step_snapshot(const fs::path& snapshot, std::string_view step) {
    return snapshot.parent_path() / (snapshot.stem().string() + '-' + std::string(step) + ".ppm");
}

/// What a click left behind: the selection, the armed command and the
/// orders of the mover and the solar collector.
struct ClickResult {
    bool mover_selected{};
    bool friend_selected{};
    bool any_selected{};
    MatchCommand command{};
    std::vector<uint8_t> kinds;                               ///< the mover's queue, head first
    std::optional<oa::sim::ground_orders::Point> destination; ///< its head's point
    std::vector<uint8_t> collector_kinds; ///< the solar collector's queue, head first

    /// Tells whether two clicks left the same selection, armed command and
    /// queues; the destinations differ by where each surface's pointer was.
    [[nodiscard]] bool same_as(const ClickResult& other) const {
        return mover_selected == other.mover_selected && friend_selected == other.friend_selected &&
               any_selected == other.any_selected && command == other.command &&
               kinds == other.kinds && collector_kinds == other.collector_kinds;
    }
};

/// Describes what a click left behind in one line of the check's output.
std::string describe(const ClickResult& result) {
    std::string text = std::string("mover ") + (result.mover_selected ? "selected" : "unselected") +
                       ", friend " + (result.friend_selected ? "selected" : "unselected") +
                       (result.any_selected ? "" : ", nothing selected") + ", command " +
                       std::to_string(static_cast<int>(result.command)) + ", orders [";
    for (std::size_t index = 0; index < result.kinds.size(); ++index)
        text += (index == 0 ? "" : " ") + std::to_string(result.kinds[index]);
    text += "]";
    if (result.collector_kinds.empty())
        return text;
    text += ", solar collector orders [";
    for (std::size_t index = 0; index < result.collector_kinds.size(); ++index)
        text += (index == 0 ? "" : " ") + std::to_string(result.collector_kinds[index]);
    return text + "]";
}

} // namespace

void Runtime::check_megamap_clicks() {
    namespace orders = oa::sim::match_runtime;
    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        fail("needs the SDL renderer");
    start_benchmark_skirmish();
    apply_output_mode();
    if (!ui_rules().megamap.enabled) {
        std::cout << "megamap click check: ui.megamap is off, so nothing was checked\n";
        return;
    }
    auto& world = match_->state();
    auto& slots = match_->world().slots;
    const auto local = match_local_player_;
    uint16_t commander = 0;
    uint8_t enemy_player = local;
    for (const auto& slot : slots) {
        if (slot.unit == nullptr || slot.record.type_index == 0)
            continue;
        if (commander == 0 && slot.record.owner_index == local)
            commander = slot.unit_index;
        if (enemy_player == local && slot.record.owner_index != local)
            enemy_player = slot.record.owner_index;
    }
    require(commander != 0 && enemy_player != local, "found no commander or enemy player");
    const auto map_width = static_cast<int32_t>(selected_tnt_->tile_width * 32U);
    const auto map_height = static_cast<int32_t>(selected_tnt_->tile_height * 32U);
    const auto map_x = [&](uint16_t id) {
        return static_cast<int32_t>(slots[id].unit->position[0] >> 16);
    };
    const auto map_z = [&](uint16_t id) {
        return static_cast<int32_t>(slots[id].unit->position[2] >> 16);
    };
    const auto spawn = [&](std::string_view name, uint8_t owner, int32_t x, int32_t z) {
        const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
        if (type == 0)
            fail("lacks " + std::string(name));
        x = std::clamp(x, kEdgeMargin, map_width - kEdgeMargin);
        z = std::clamp(z, kEdgeMargin, map_height - kEdgeMargin);
        oa::sim::unit_spawn::Request request;
        request.player = owner;
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
        auto* slot = match_->create(request);
        if (slot == nullptr || slot->unit == nullptr)
            fail("could not spawn " + std::string(name));
        slot->unit->object_present = true;
        // Held fire, so that the units do not fight while the check runs.
        slot->record.flags &= ~OA_UNIT_FLAG_FIRE_ORDER_MASK;
        return slot->unit_index;
    };
    // The mover, a friend beside it and an enemy in its sight, placed from
    // the commander toward the map's middle; the ground clicked lies
    // between them. A solar collector, which cannot patrol, stands between
    // the commander and the mover.
    const int32_t way_x = map_x(commander) < map_width / 2 ? 1 : -1;
    const int32_t way_z = map_z(commander) < map_height / 2 ? 1 : -1;
    const auto mover =
        spawn("ARMPW", local, map_x(commander) + way_x * kApart, map_z(commander) + way_z * kApart);
    const auto friend_unit = spawn("ARMPW", local, map_x(mover) + way_x * kApart, map_z(mover));
    const auto enemy = spawn("CORAK", enemy_player, map_x(mover), map_z(mover) + way_z * kApart);
    const auto collector = spawn(
        "ARMSOLAR", local, map_x(mover) - way_x * kApart / 2, map_z(mover) - way_z * kApart / 2
    );
    const std::array<int32_t, 2> open_ground{
        map_x(mover) + way_x * kApart / 2, map_z(mover) + way_z * kApart / 2
    };

    // The megamap acts only with the Mouse wheel zoom setting off.
    auto& wheel_zoom = engine_settings_state().current.wheel_zoom;
    const bool kept_wheel_zoom = std::exchange(wheel_zoom, false);
    megamap_wheel_zoom_changed();
    set_megamap_open(false);
    // The match runs until the enemy is in sight and on the radar, as the
    // minimap and the megamap show it; then the enemy stands where it is.
    for (int step = 0;
         step < kSightTicks && !((slots[enemy].unit->flags & OA_UNIT_FLAG_RADAR_CONTACT) != 0 &&
                                 match_->unit_shown(local, enemy));
         ++step)
        step_match_simulation();
    match_->stop_orders(enemy);
    render_match_surface();
    require(
        match_->unit_shown(local, enemy) &&
            (slots[enemy].unit->flags & OA_UNIT_FLAG_RADAR_CONTACT) != 0,
        "the enemy is not in the mover's sight and on the radar"
    );
    check_megamap_picture();

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
    const auto click = [&](uint8_t button, std::pair<float, float> at) {
        send(SDL_EVENT_MOUSE_BUTTON_DOWN, button, at.first, at.second);
        send(SDL_EVENT_MOUSE_BUTTON_UP, button, at.first, at.second);
    };
    // The point clicked on the battlefield: a unit's, or the open ground's,
    // with the view centred there. The pointer rests on it for a frame, as
    // a player's does before clicking.
    const auto battlefield_at = [&](uint16_t id) {
        const std::array<int32_t, 2> on =
            id != 0 ? std::array<int32_t, 2>{map_x(id), map_z(id)} : open_ground;
        set_camera_position(on[0] - visible_map_width() / 2, on[1] - visible_map_height() / 2, 0);
        render_match_surface();
        const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
        std::array<uint32_t, 3> position{};
        if (id != 0) {
            position = slots[id].unit->position;
        } else {
            const auto ground = map_world_point(on[0], on[1]);
            require(ground.has_value(), "found no ground at the open ground's point");
            position = {
                static_cast<uint32_t>((*ground)[0]),
                static_cast<uint32_t>((*ground)[1]),
                static_cast<uint32_t>((*ground)[2])
            };
        }
        const auto screen = project_match_point(viewport, position);
        const std::pair<float, float> at{
            static_cast<float>(screen.x), static_cast<float>(screen.y)
        };
        send(SDL_EVENT_MOUSE_MOTION, 0, at.first, at.second);
        render_match_surface();
        require(
            hovered_match_unit_ == id,
            id == 0 ? "a unit lies under the open ground on the battlefield"
                    : "the battlefield's pointer did not pick the unit"
        );
        return at;
    };
    // The canvas point the megamap draws a map point at.
    const auto megamap_pixel = [&](std::array<int32_t, 2> on) {
        const auto pixel = hud::megamap_point(megamap_.layout, on[0], on[1]);
        return std::pair<float, float>{
            static_cast<float>(match_layout_.left + pixel[0]),
            static_cast<float>(match_layout_.top + pixel[1])
        };
    };
    // A unit's icon on the megamap, or the open ground's pixel.
    const auto megamap_spot = [&](uint16_t id) {
        return megamap_pixel(id != 0 ? std::array<int32_t, 2>{map_x(id), map_z(id)} : open_ground);
    };
    // The point clicked on the megamap: a unit's icon, or the open ground's
    // pixel.
    const auto megamap_at = [&](uint16_t id) {
        const auto at = megamap_spot(id);
        send(SDL_EVENT_MOUSE_MOTION, 0, at.first, at.second);
        render_match_surface();
        require(
            megamap_.hovered == id,
            id == 0 ? "a unit's icon lies under the open ground's pixel"
                    : "the megamap's pointer did not pick the unit's icon"
        );
        return at;
    };
    const auto result = [&] {
        ClickResult out;
        out.mover_selected = (slots[mover].unit->flags & OA_UNIT_FLAG_SELECTED) != 0;
        out.friend_selected = (slots[friend_unit].unit->flags & OA_UNIT_FLAG_SELECTED) != 0;
        out.any_selected = has_local_selection();
        out.command = match_command_;
        match_->visit_primary_queue(mover, [&](const auto& view) {
            if (out.kinds.empty())
                out.destination = view.destination;
            out.kinds.push_back(view.kind);
        });
        match_->visit_primary_queue(collector, [&](const auto& view) {
            out.collector_kinds.push_back(view.kind);
        });
        return out;
    };

    struct Case {
        const char* name{};
        int32_t interface_type{};
        bool mover_selected{};  ///< the mover selected before the click
        MatchCommand command{}; ///< the command armed before it
        uint8_t button{};       ///< the button clicked
        uint16_t on{};          ///< the unit clicked on, or 0 for the open ground
        std::function<bool(const ClickResult&)> expected; ///< what both must leave
        bool collector_selected{}; ///< the solar collector selected beside the mover
    };

    const auto moved = [&](const ClickResult& r) {
        return r.mover_selected && r.kinds.size() == 1 &&
               r.kinds.front() == oa::sim::ground_orders::move_ground_kind;
    };
    const auto attacked = [&](const ClickResult& r) {
        return r.mover_selected && r.kinds.size() == 1 &&
               r.kinds.front() == orders::attack_chase_kind;
    };
    const auto deselected = [](const ClickResult& r) { return !r.any_selected && r.kinds.empty(); };
    const std::vector<Case> cases{
        {"left-click interface: a left click on open ground moves the selection",
         input::interface_left_click,
         true,
         MatchCommand::none,
         SDL_BUTTON_LEFT,
         0,
         [&](const ClickResult& r) { return moved(r) && r.command == MatchCommand::none; },
         false},
        {"left-click interface: a left click on an enemy attacks it",
         input::interface_left_click,
         true,
         MatchCommand::none,
         SDL_BUTTON_LEFT,
         enemy,
         [&](const ClickResult& r) { return attacked(r); },
         false},
        {"left-click interface: a left click on an own unit selects it alone",
         input::interface_left_click,
         true,
         MatchCommand::none,
         SDL_BUTTON_LEFT,
         friend_unit,
         [](const ClickResult& r) {
             return !r.mover_selected && r.friend_selected && r.kinds.empty();
         },
         false},
        {"left-click interface: a right press on open ground deselects without an order",
         input::interface_left_click,
         true,
         MatchCommand::none,
         SDL_BUTTON_RIGHT,
         0,
         deselected,
         false},
        {"left-click interface: a right press cancels an armed PATROL alone",
         input::interface_left_click,
         true,
         MatchCommand::patrol,
         SDL_BUTTON_RIGHT,
         0,
         [](const ClickResult& r) {
             return r.mover_selected && r.command == MatchCommand::none && r.kinds.empty();
         },
         false},
        {"left-click interface: a left click with PATROL armed sends the mover on patrol and "
         "gives the solar collector selected beside it no order",
         input::interface_left_click,
         true,
         MatchCommand::patrol,
         SDL_BUTTON_LEFT,
         0,
         [](const ClickResult& r) {
             return r.mover_selected && r.command == MatchCommand::none && r.kinds.size() == 1 &&
                    r.kinds.front() == orders::patrol_kind && r.collector_kinds.empty();
         },
         true},
        {"right-click interface: a left click on open ground deselects without an order",
         input::interface_right_click,
         true,
         MatchCommand::none,
         SDL_BUTTON_LEFT,
         0,
         deselected,
         false},
        {"right-click interface: a left click on an own unit selects it",
         input::interface_right_click,
         false,
         MatchCommand::none,
         SDL_BUTTON_LEFT,
         friend_unit,
         [](const ClickResult& r) {
             return !r.mover_selected && r.friend_selected && r.kinds.empty();
         },
         false},
        {"right-click interface: a right press on open ground moves the selection",
         input::interface_right_click,
         true,
         MatchCommand::none,
         SDL_BUTTON_RIGHT,
         0,
         [&](const ClickResult& r) { return moved(r); },
         false},
        {"right-click interface: a right press on an own unit guards it",
         input::interface_right_click,
         true,
         MatchCommand::none,
         SDL_BUTTON_RIGHT,
         friend_unit,
         [](const ClickResult& r) {
             return r.mover_selected && !r.friend_selected && r.kinds.size() == 1 &&
                    r.kinds.front() == orders::follow_ground_kind;
         },
         false},
        {"right-click interface: a left click with ATTACK armed attacks the enemy",
         input::interface_right_click,
         true,
         MatchCommand::attack,
         SDL_BUTTON_LEFT,
         enemy,
         [&](const ClickResult& r) { return attacked(r) && r.command == MatchCommand::none; },
         false},
        {"left-click interface: a left click with MOVE armed on an enemy moves to it",
         input::interface_left_click,
         true,
         MatchCommand::move,
         SDL_BUTTON_LEFT,
         enemy,
         [&](const ClickResult& r) { return moved(r) && r.command == MatchCommand::none; },
         false},
        {"right-click interface: a left click with MOVE armed on an own unit guards it",
         input::interface_right_click,
         true,
         MatchCommand::move,
         SDL_BUTTON_LEFT,
         friend_unit,
         [](const ClickResult& r) {
             return r.mover_selected && !r.friend_selected && r.kinds.size() == 1 &&
                    r.kinds.front() == orders::follow_ground_kind &&
                    r.command == MatchCommand::none;
         },
         false},
    };

    // Each case from the same start on the battlefield, then on the megamap.
    const auto prepare = [&](const Case& one) {
        world.game.interface_type = one.interface_type;
        match_->stop_orders(mover);
        match_->stop_orders(collector);
        clear_local_selection();
        reset_match_command();
        pending_build_type_ = 0;
        if (one.mover_selected) {
            adopt_selection(mover);
            selected_match_unit_ = mover;
        }
        if (one.collector_selected)
            adopt_selection(collector);
        apply_match_hud_for_selection();
        match_command_ = one.command;
    };
    std::vector<std::string> failures;
    // With --snapshot, each surface's frame just before and just after the
    // click: case<number>-<battlefield|megamap>-<before|after>.
    const auto snapshot = [&](std::size_t number, std::string_view surface, bool after) {
        if (options_.snapshot.empty())
            return;
        render_match_surface();
        renderer::Surface frame;
        compose_match_frame(frame);
        write_ppm(
            step_snapshot(
                options_.snapshot,
                "case" + std::to_string(number) + "-" + std::string(surface) +
                    (after ? "-after" : "-before")
            ),
            frame
        );
    };
    for (std::size_t index = 0; index < cases.size(); ++index) {
        const auto& one = cases[index];
        set_megamap_open(false);
        prepare(one);
        const auto battlefield_click = battlefield_at(one.on);
        snapshot(index + 1, "battlefield", false);
        click(one.button, battlefield_click);
        snapshot(index + 1, "battlefield", true);
        const auto battlefield = result();
        const auto battlefield_ground =
            one.on == 0 ? match_world_point(pointer_x_, pointer_y_) : std::nullopt;

        prepare(one);
        set_megamap_open(true);
        render_match_surface();
        require(megamap_shown(), "the megamap did not open");
        const auto at = megamap_at(one.on);
        std::optional<oa::sim::ground_orders::Point> megamap_ground;
        if (one.on == 0)
            if (const auto point = hud::megamap_map_point(
                    megamap_.layout,
                    static_cast<int32_t>(at.first) - match_layout_.left,
                    static_cast<int32_t>(at.second) - match_layout_.top
                ))
                megamap_ground = map_world_point((*point)[0], (*point)[1]);
        snapshot(index + 1, "megamap", false);
        click(one.button, at);
        snapshot(index + 1, "megamap", true);
        const auto megamap = result();
        set_megamap_open(false);

        std::cout << "megamap click check: case " << index + 1 << ": " << one.name
                  << "\n  battlefield: " << describe(battlefield) << " (clicked at "
                  << battlefield_click.first << "," << battlefield_click.second << ")"
                  << "\n  megamap:     " << describe(megamap) << " (clicked at " << at.first << ","
                  << at.second << ")\n";
        // A move goes to the ground under the pointer on each.
        const bool battlefield_point =
            battlefield.kinds.empty() ||
            battlefield.kinds.front() != oa::sim::ground_orders::move_ground_kind ||
            !battlefield_ground || battlefield.destination == battlefield_ground;
        const bool megamap_point =
            megamap.kinds.empty() ||
            megamap.kinds.front() != oa::sim::ground_orders::move_ground_kind || !megamap_ground ||
            megamap.destination == megamap_ground;
        if (!one.expected(battlefield) || !battlefield_point)
            failures.push_back(
                std::string(one.name) + " (battlefield: " + describe(battlefield) + ")"
            );
        if (!megamap.same_as(battlefield) || !one.expected(megamap) || !megamap_point)
            failures.push_back(std::string(one.name) + " (megamap: " + describe(megamap) + ")");
    }

    // The panels over the open megamap take their own clicks, as they do over
    // the battlefield at every zoom, in both interface types, from the mouse
    // and from the touch controls, whose taps come as the left button's
    // events in the canvas's own pixels, from no window.
    std::vector<std::string> panel_failures;
    const auto showing = [&](const char* layout) {
        return screen_ == Screen::match && match_paused_ && match_hud_ &&
               match_hud_panel_ == oa::data::defs::gui_path(layout);
    };
    const auto over_megamap = [&](float x, float y) {
        return hud::megamap_map_point(
                   megamap_.layout,
                   static_cast<int32_t>(x) - match_layout_.left,
                   static_cast<int32_t>(y) - match_layout_.top
        )
            .has_value();
    };
    const auto pointer_event = [&](SDL_EventType type, float x, float y, bool finger) {
        if (!finger) {
            send(type, SDL_BUTTON_LEFT, x, y);
            return;
        }
        SDL_Event event{};
        event.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            event.motion.which = SDL_TOUCH_MOUSEID;
            event.motion.x = x;
            event.motion.y = y;
        } else {
            event.button.which = SDL_TOUCH_MOUSEID;
            event.button.button = SDL_BUTTON_LEFT;
            event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = x;
            event.button.y = y;
        }
        dispatch_event(event, running);
    };
    // A left click on the shown panel's button where it is drawn, with the
    // pointer brought onto it first; `on_megamap` asks for one drawn over
    // the megamap's picture.
    const auto press_button = [&](std::string_view name, bool finger, bool on_megamap) {
        const auto& gadgets = match_hud_->layout.gadgets;
        std::size_t index = 1;
        while (index < gadgets.size() &&
               (gadgets[index].common.name != name || gadgets[index].common.active == 0))
            ++index;
        require(index < gadgets.size(), match_hud_panel_ + " has no " + std::string(name));
        // A dialog placed over the match shows its root's pixels in its area.
        auto centre = hud_gadget_centre(index);
        if (const auto area = placed_panel_area()) {
            const auto& root = gadgets.front().common;
            const auto& record = gadgets[index].common;
            centre = {
                area->x + (2 * (record.x - root.x) + record.width) * area->width / (2 * root.width),
                area->y +
                    (2 * (record.y - root.y) + record.height) * area->height / (2 * root.height)
            };
        }
        const auto x = static_cast<float>(centre.x);
        const auto y = static_cast<float>(centre.y);
        require(
            !on_megamap || over_megamap(x, y),
            std::string(name) + " of " + match_hud_panel_ + " is not over the megamap"
        );
        pointer_event(SDL_EVENT_MOUSE_MOTION, x, y, finger);
        require(
            hovered_ == index,
            "the pointer did not find " + std::string(name) + " of " + match_hud_panel_
        );
        pointer_event(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, finger);
        pointer_event(SDL_EVENT_MOUSE_BUTTON_UP, x, y, finger);
        render_match_surface();
    };
    const auto mover_alone = [&](int32_t interface_type) {
        Case start{};
        start.interface_type = interface_type;
        start.mover_selected = true;
        prepare(start);
    };
    const auto back_to_menu = [&] {
        if (!showing("ARMOPT.GUI"))
            show_match_pause_menu();
        render_match_surface();
    };

    struct PanelPass {
        const char* name{};
        int32_t interface_type{};
        bool finger{}; ///< taps as the touch controls send them, else the mouse's clicks
    };

    const std::array<PanelPass, 4> panel_passes{{
        {"left-click interface, mouse", input::interface_left_click, false},
        {"right-click interface, mouse", input::interface_right_click, false},
        {"left-click interface, touch", input::interface_left_click, true},
        {"right-click interface, touch", input::interface_right_click, true},
    }};
    for (const auto& pass : panel_passes) {
        const auto fault = [&](const std::string& what) {
            panel_failures.push_back(std::string(pass.name) + ": " + what);
        };
        mover_alone(pass.interface_type);
        set_megamap_open(true);
        show_match_pause_menu();
        render_match_surface();
        require(
            megamap_shown() && showing("ARMOPT.GUI"),
            "the in-game menu did not open over the megamap"
        );
        // EXIT, in the side column, opens the exit menu over the megamap;
        // its CANCEL goes back to the menu, and EXITGAME and MAINMENU ask,
        // where CHOICE2 goes back too.
        press_button("EXIT", pass.finger, false);
        require(showing("EXITMENU.GUI"), "EXIT did not open the exit menu");
        press_button("CANCEL", pass.finger, true);
        if (!showing("ARMOPT.GUI"))
            fault("the exit menu's CANCEL over the megamap did not go back to the menu");
        for (const std::string_view asks : {"EXITGAME", "MAINMENU"}) {
            back_to_menu();
            press_button("EXIT", pass.finger, false);
            require(showing("EXITMENU.GUI"), "EXIT did not open the exit menu");
            press_button(asks, pass.finger, true);
            if (!showing("YESORNO.GUI")) {
                fault("the exit menu's " + std::string(asks) + " over the megamap did not ask");
                continue;
            }
            press_button("CHOICE2", pass.finger, true);
            if (!showing("ARMOPT.GUI"))
                fault(
                    "CHOICE2 over the megamap did not answer " + std::string(asks) +
                    "'s question and go back to the menu"
                );
        }
        // While the menu holds the match a left click beside it on the
        // megamap, on the friend's icon, selects nothing and gives no order,
        // as on the battlefield.
        back_to_menu();
        if (!pass.finger) {
            const auto beside = megamap_at(friend_unit);
            click(SDL_BUTTON_LEFT, beside);
            const auto after = result();
            if (!after.mover_selected || after.friend_selected || !after.kinds.empty() ||
                !showing("ARMOPT.GUI"))
                fault("a left click on the megamap beside the menu acted: " + describe(after));
        }
        // The save page over the match takes the pointer in its own pixels:
        // a press where the megamap lies on the match's screen is not the
        // megamap's, and its CANCEL goes back to the menu.
        back_to_menu();
        press_button("SAVEGAME", pass.finger, false);
        require(screen_ == Screen::load_game, "SAVEGAME did not open the save page");
        const auto area = overlay_area();
        const auto middle_x = static_cast<float>(area.x + area.width / 2);
        const auto middle_y = static_cast<float>(area.y + area.height / 2);
        pointer_event(SDL_EVENT_MOUSE_MOTION, middle_x, middle_y, pass.finger);
        pointer_event(SDL_EVENT_MOUSE_BUTTON_DOWN, middle_x, middle_y, pass.finger);
        if (std::exchange(megamap_.pressed, false))
            fault("the megamap took a press on the save page");
        pointer_event(SDL_EVENT_MOUSE_BUTTON_UP, middle_x, middle_y, pass.finger);
        require(screen_ == Screen::load_game, "a press on the save page closed it");
        const auto* cancel = widget("CANCEL");
        require(cancel != nullptr, "the save page has no CANCEL");
        const auto origin = panel_origin();
        const auto cancel_x =
            static_cast<float>(origin.x + cancel->common.x + cancel->common.width / 2);
        const auto cancel_y =
            static_cast<float>(origin.y + cancel->common.y + cancel->common.height / 2);
        pointer_event(SDL_EVENT_MOUSE_MOTION, cancel_x, cancel_y, pass.finger);
        pointer_event(SDL_EVENT_MOUSE_BUTTON_DOWN, cancel_x, cancel_y, pass.finger);
        tick_screen_packages();
        pointer_event(SDL_EVENT_MOUSE_BUTTON_UP, cancel_x, cancel_y, pass.finger);
        if (!showing("ARMOPT.GUI")) {
            fault("the save page's CANCEL did not go back to the menu");
            if (screen_ == Screen::load_game)
                leave_load_dialog();
        }
        if (match_paused_)
            resume_match_pause();
        set_megamap_open(false);
    }
    // A right press beside the menu is the battlefield's, as in 3.1c, and
    // the megamap's leaves what the battlefield's does: the selection, the
    // orders and whether the menu stays.
    for (const auto interface_type : {input::interface_left_click, input::interface_right_click}) {
        const std::string name = interface_type == input::interface_left_click
                                     ? "left-click interface"
                                     : "right-click interface";
        mover_alone(interface_type);
        show_match_pause_menu();
        click(SDL_BUTTON_RIGHT, battlefield_at(0));
        const auto battlefield = result();
        const bool battlefield_menu = showing("ARMOPT.GUI");
        if (match_paused_)
            resume_match_pause();
        mover_alone(interface_type);
        set_megamap_open(true);
        show_match_pause_menu();
        render_match_surface();
        click(SDL_BUTTON_RIGHT, megamap_at(0));
        const auto megamap = result();
        const bool megamap_menu = showing("ARMOPT.GUI");
        if (match_paused_)
            resume_match_pause();
        set_megamap_open(false);
        std::cout << "megamap click check: " << name
                  << ": a right press beside the in-game menu\n  battlefield: "
                  << describe(battlefield) << (battlefield_menu ? ", menu open" : ", menu closed")
                  << "\n  megamap:     " << describe(megamap)
                  << (megamap_menu ? ", menu open" : ", menu closed") << '\n';
        if (!megamap.same_as(battlefield) || megamap_menu != battlefield_menu)
            panel_failures.push_back(
                name + ": a right press beside the in-game menu on the megamap left " +
                describe(megamap) + (megamap_menu ? ", menu open" : ", menu closed") +
                ", the battlefield's " + describe(battlefield) +
                (battlefield_menu ? ", menu open" : ", menu closed")
            );
    }
    // The unit info panel over the open megamap takes its own clicks: a left
    // click on its face does nothing, and one on its DONE closes it.
    mover_alone(input::interface_left_click);
    std::ignore = battlefield_at(mover);
    require(open_unit_info(), "the unit info panel did not open for the mover");
    set_megamap_open(true);
    render_match_surface();
    const auto info_area = unit_info_area();
    const oa::ui::gui_layout::Gadget* done = nullptr;
    for (const auto& gadget : unit_info_panel_->screen->layout.gadgets)
        if (gadget.common.name == "DONE")
            done = &gadget;
    require(info_area.has_value() && done != nullptr, "the unit info panel shows no DONE");
    const auto& info_root = unit_info_panel_->root;
    const std::pair<float, float> info_face{
        static_cast<float>(info_area->x + 2),
        static_cast<float>(info_area->y + info_area->height / 2)
    };
    const std::pair<float, float> info_done{
        static_cast<float>(
            info_area->x + (2 * (done->common.x - info_root.x) + done->common.width) *
                               info_area->width / (2 * info_root.width)
        ),
        static_cast<float>(
            info_area->y + (2 * (done->common.y - info_root.y) + done->common.height) *
                               info_area->height / (2 * info_root.height)
        )
    };
    require(
        over_megamap(info_face.first, info_face.second) &&
            over_megamap(info_done.first, info_done.second),
        "the unit info panel is not over the megamap"
    );
    send(SDL_EVENT_MOUSE_MOTION, 0, info_face.first, info_face.second);
    click(SDL_BUTTON_LEFT, info_face);
    const auto on_face = result();
    if (!unit_info_panel_ || !on_face.mover_selected || on_face.friend_selected ||
        !on_face.kinds.empty())
        panel_failures.push_back(
            "a left click on the unit info panel's face over the megamap acted: " +
            describe(on_face) + (unit_info_panel_ ? "" : ", the panel closed")
        );
    send(SDL_EVENT_MOUSE_MOTION, 0, info_done.first, info_done.second);
    click(SDL_BUTTON_LEFT, info_done);
    if (unit_info_panel_) {
        panel_failures.push_back("the unit info panel's DONE over the megamap did not close it");
        close_unit_info();
    }
    set_megamap_open(false);

    // A press on the megamap that drops the selection closes the unit info
    // panel too, as one on the battlefield does: a right press on open ground
    // in the left-click interface, a left click there in the right-click
    // interface, and a box that takes in none of the viewer's units. The box
    // is drawn over open ground far from the units, off the panel wherever
    // the layout puts it.
    const std::array<int32_t, 2> far_ground{
        map_width - open_ground[0], map_height - open_ground[1]
    };
    const auto bare_spot = [&](float reach) {
        for (const auto& on :
             {far_ground,
              std::array<int32_t, 2>{open_ground[0], far_ground[1]},
              std::array<int32_t, 2>{far_ground[0], open_ground[1]}}) {
            const auto at = megamap_pixel(on);
            if (!unit_info_point(at.first, at.second) &&
                !unit_info_point(at.first + reach, at.second + reach))
                return at;
        }
        fail("the unit info panel covers every spot tried for an empty box");
    };

    struct Drop {
        const char* name{};
        int32_t interface_type{};
        uint8_t button{}; ///< the button pressed on open ground, or 0 for the box
    };

    const std::array<Drop, 3> drops{{
        {"left-click interface: a right press on open ground",
         input::interface_left_click,
         SDL_BUTTON_RIGHT},
        {"right-click interface: a left click on open ground",
         input::interface_right_click,
         SDL_BUTTON_LEFT},
        {"left-click interface: a box that takes in no unit", input::interface_left_click, 0},
    }};
    for (const auto& drop : drops) {
        mover_alone(drop.interface_type);
        std::ignore = battlefield_at(mover);
        require(open_unit_info(), "the unit info panel did not open for the mover");
        set_megamap_open(true);
        render_match_surface();
        if (drop.button != 0) {
            const auto at = megamap_at(0);
            require(
                !unit_info_point(at.first, at.second), "the unit info panel covers open ground"
            );
            click(drop.button, at);
        } else {
            const auto from = bare_spot(kBoxMargin);
            const std::pair<float, float> to{from.first + kBoxMargin, from.second + kBoxMargin};
            send(SDL_EVENT_MOUSE_MOTION, 0, from.first, from.second);
            render_match_surface();
            send(SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_BUTTON_LEFT, from.first, from.second);
            send(SDL_EVENT_MOUSE_MOTION, 0, to.first, to.second);
            send(SDL_EVENT_MOUSE_BUTTON_UP, SDL_BUTTON_LEFT, to.first, to.second);
        }
        const auto after = result();
        if (unit_info_panel_ || after.any_selected || !after.kinds.empty())
            panel_failures.push_back(
                std::string(drop.name) + " on the megamap left " + describe(after) +
                (unit_info_panel_ ? ", the unit info panel open" : "")
            );
        close_unit_info();
        set_megamap_open(false);
    }

    // Fingers on the open megamap, through the touch controls' dispatcher as a
    // touch screen's come, act as the mouse's clicks there in both interface
    // types, on the icon under the finger and at the map point under it, and
    // never on the battlefield hidden under the megamap: a tap is a left click,
    // or in the right-click interface the right press an order needs; a double
    // tap on an own unit selects every unit of its type; the order wheel a hold
    // opens gives its order there; a drag is the megamap's box; a tap with a
    // building armed places it there, once for a double tap. While the in-game
    // menu holds the match a tap is a click beside the menu and a hold opens no
    // wheel. The view stands well away from the units, so that the battlefield
    // under the megamap holds other ground.
    std::vector<std::string> touch_failures;
    auto& dispatch = touch_state().dispatch;
    dispatch.forced = true;
    dispatch.accept_unregistered_touch = true;
    uint64_t clock_ns = SDL_GetTicksNS() + 1000 * kNanosecondsPerMillisecond;
    uint64_t next_finger = 1;
    const auto touch_frames = [&](uint64_t ms) {
        clock_ns += ms * kNanosecondsPerMillisecond;
        dispatch.check_clock_ns = clock_ns;
        tick_touch();
        render_match_surface();
    };
    // A finger event in normalised window coordinates, as SDL reports one.
    const auto finger = [&](SDL_EventType type, uint64_t id, std::pair<float, float> at) {
        float window_x = 0.0F;
        float window_y = 0.0F;
        if (!frame_to_window(sdl_.renderer, at.first, at.second, &window_x, &window_y))
            fail(SDL_GetError());
        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSize(sdl_.window, &width, &height) || width <= 0 || height <= 0)
            fail(SDL_GetError());
        SDL_Event event{};
        event.type = type;
        event.tfinger.timestamp = clock_ns;
        event.tfinger.touchID = kCheckTouchDevice;
        event.tfinger.fingerID = id;
        event.tfinger.x = window_x / static_cast<float>(width);
        event.tfinger.y = window_y / static_cast<float>(height);
        event.tfinger.pressure = type == SDL_EVENT_FINGER_UP ? 0.0F : 1.0F;
        event.tfinger.windowID = SDL_GetWindowID(sdl_.window);
        dispatch_event(event, running);
    };
    const auto tap = [&](std::pair<float, float> at, int taps) {
        for (int count = 1; count <= taps; ++count) {
            const auto id = next_finger++;
            finger(SDL_EVENT_FINGER_DOWN, id, at);
            touch_frames(kTapRestMs);
            finger(SDL_EVENT_FINGER_UP, id, at);
            touch_frames(count < taps ? kTapRestMs : kAfterTapMs);
        }
    };
    const auto hold = [&](std::pair<float, float> at) {
        const auto id = next_finger++;
        finger(SDL_EVENT_FINGER_DOWN, id, at);
        for (uint64_t rested = 0; rested < kHoldMs; rested += kHoldStepMs)
            touch_frames(kHoldStepMs);
        finger(SDL_EVENT_FINGER_UP, id, at);
        touch_frames(kFrameMs);
    };
    const auto drag = [&](std::pair<float, float> from, std::pair<float, float> to) {
        const auto id = next_finger++;
        finger(SDL_EVENT_FINGER_DOWN, id, from);
        touch_frames(kFrameMs);
        for (int step = 1; step <= kDragSteps; ++step) {
            const float part = static_cast<float>(step) / static_cast<float>(kDragSteps);
            finger(
                SDL_EVENT_FINGER_MOTION,
                id,
                {from.first + (to.first - from.first) * part,
                 from.second + (to.second - from.second) * part}
            );
            touch_frames(kFrameMs);
        }
        finger(SDL_EVENT_FINGER_UP, id, to);
        touch_frames(kFrameMs);
    };
    // A tap on a wedge of the open order wheel.
    const auto pick = [&](oa::ui::touch_hud::RadialItem item) {
        const auto& radial = touch_state().hud.radial;
        if (!radial)
            return false;
        for (const auto& wedge : radial->wedges)
            if (wedge.item == item && wedge.available) {
                tap({static_cast<float>(wedge.hit.x + wedge.hit.width / 2),
                     static_cast<float>(wedge.hit.y + wedge.hit.height / 2)},
                    1);
                return true;
            }
        return false;
    };
    const auto touch_start = [&](int32_t interface_type, bool mover_selected) {
        Case start{};
        start.interface_type = interface_type;
        start.mover_selected = mover_selected;
        prepare(start);
        match_->stop_orders(commander);
        set_megamap_open(true);
        touch_frames(kFrameMs);
    };
    set_camera_position(
        far_ground[0] - visible_map_width() / 2, far_ground[1] - visible_map_height() / 2, 0
    );
    apply_output_mode();
    touch_start(input::interface_left_click, false);
    touch_frames(kFrameMs);
    require(touch_controls_active() && megamap_shown(), "the touch controls did not come on");
    // The ground under the open ground's pixel, where a move there goes.
    const auto touch_ground = [&]() -> std::optional<oa::sim::ground_orders::Point> {
        const auto at = megamap_spot(0);
        const auto point = hud::megamap_map_point(
            megamap_.layout,
            static_cast<int32_t>(at.first) - match_layout_.left,
            static_cast<int32_t>(at.second) - match_layout_.top
        );
        return point ? map_world_point((*point)[0], (*point)[1]) : std::nullopt;
    }();
    require(touch_ground.has_value(), "found no ground under the open ground's pixel");
    const auto moved_there = [&](const ClickResult& r) {
        return moved(r) && r.destination == touch_ground;
    };
    const auto friend_alone = [](const ClickResult& r) {
        return !r.mover_selected && r.friend_selected && r.kinds.empty();
    };

    struct TouchCase {
        const char* name{};
        int32_t interface_type{};
        bool mover_selected{};                            ///< the mover selected before the gesture
        std::function<void()> gesture;                    ///< the fingers' gesture
        std::function<bool(const ClickResult&)> expected; ///< what it must leave
    };

    const auto box_from = [&] {
        const auto a = megamap_spot(mover);
        const auto b = megamap_spot(friend_unit);
        return std::pair<float, float>{
            std::min(a.first, b.first) - kBoxMargin, std::min(a.second, b.second) - kBoxMargin
        };
    };
    const auto box_to = [&] {
        const auto a = megamap_spot(mover);
        const auto b = megamap_spot(friend_unit);
        return std::pair<float, float>{
            std::max(a.first, b.first) + kBoxMargin, std::max(a.second, b.second) + kBoxMargin
        };
    };
    const std::vector<TouchCase> touch_cases{
        {"left-click interface: a tap on open ground moves the selection there",
         input::interface_left_click,
         true,
         [&] { tap(megamap_spot(0), 1); },
         moved_there},
        {"left-click interface: a tap on an own unit selects it alone",
         input::interface_left_click,
         true,
         [&] { tap(megamap_spot(friend_unit), 1); },
         friend_alone},
        {"left-click interface: a double tap on an own unit selects every unit of its type",
         input::interface_left_click,
         false,
         [&] { tap(megamap_spot(friend_unit), 2); },
         [](const ClickResult& r) {
             return r.mover_selected && r.friend_selected && r.kinds.empty();
         }},
        {"right-click interface: a tap on open ground moves the selection there",
         input::interface_right_click,
         true,
         [&] { tap(megamap_spot(0), 1); },
         moved_there},
        {"right-click interface: a tap on an enemy attacks it",
         input::interface_right_click,
         true,
         [&] { tap(megamap_spot(enemy), 1); },
         [&](const ClickResult& r) { return attacked(r); }},
        {"right-click interface: a tap on an own unit selects it",
         input::interface_right_click,
         false,
         [&] { tap(megamap_spot(friend_unit), 1); },
         friend_alone},
        {"left-click interface: MOVE from the order wheel of a hold on open ground moves the "
         "selection there",
         input::interface_left_click,
         true,
         [&] {
             hold(megamap_spot(0));
             if (!pick(oa::ui::touch_hud::RadialItem::move))
                 touch_failures.push_back("a hold on the megamap opened no wheel with MOVE");
         },
         moved_there},
        {"left-click interface: a drag boxes the viewer's units",
         input::interface_left_click,
         false,
         [&] { drag(box_from(), box_to()); },
         [](const ClickResult& r) {
             return r.mover_selected && r.friend_selected && r.kinds.empty();
         }},
    };
    for (const auto& one : touch_cases) {
        touch_start(one.interface_type, one.mover_selected);
        one.gesture();
        touch_state().hud.radial.reset();
        const auto after = result();
        std::cout << "megamap click check: touch: " << one.name << "\n  " << describe(after)
                  << '\n';
        if (!one.expected(after))
            touch_failures.push_back(std::string(one.name) + ": " + describe(after));
    }
    // A building armed for the commander: a tap on the megamap places it at
    // the map point tapped, and a double tap places it once and gives nothing
    // more.
    const auto solar = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMSOLAR");
    for (const int taps : {1, 2}) {
        touch_start(input::interface_left_click, false);
        adopt_selection(commander);
        selected_match_unit_ = commander;
        apply_match_hud_for_selection();
        match_command_ = MatchCommand::build;
        pending_build_type_ = solar;
        touch_frames(kFrameMs);
        tap(megamap_spot(0), taps);
        std::vector<uint8_t> kinds;
        std::optional<oa::sim::ground_orders::Point> site;
        match_->visit_primary_queue(commander, [&](const auto& view) {
            if (kinds.empty())
                site = view.destination;
            kinds.push_back(view.kind);
        });
        const bool near = site && std::abs((*site)[0] - (*touch_ground)[0]) <= (kSiteReach << 16) &&
                          std::abs((*site)[2] - (*touch_ground)[2]) <= (kSiteReach << 16);
        if (kinds.size() != 1 || kinds.front() != orders::mobile_build_kind || !near)
            touch_failures.push_back(
                std::string(taps == 1 ? "a tap" : "a double tap") +
                " on the megamap with ARMSOLAR armed did not place it once there: " +
                std::to_string(kinds.size()) + " orders" + (near ? "" : ", not at the point")
            );
        match_->stop_orders(commander);
        reset_match_command();
        pending_build_type_ = 0;
    }
    // While the in-game menu holds the match: in the left-click interface a
    // tap beside it does nothing, and a hold opens no wheel; in the
    // right-click interface a tap on open ground beside it leaves what the
    // mouse's right press there does.
    touch_start(input::interface_left_click, true);
    show_match_pause_menu();
    touch_frames(kFrameMs);
    tap(megamap_spot(friend_unit), 1);
    const auto beside = result();
    if (!beside.mover_selected || beside.friend_selected || !beside.kinds.empty() ||
        !showing("ARMOPT.GUI"))
        touch_failures.push_back("a tap on the megamap beside the menu acted: " + describe(beside));
    hold(megamap_spot(friend_unit));
    if (touch_state().hud.radial) {
        touch_failures.push_back("a hold on the megamap beside the menu opened the order wheel");
        touch_state().hud.radial.reset();
    }
    if (match_paused_)
        resume_match_pause();
    touch_start(input::interface_right_click, true);
    show_match_pause_menu();
    touch_frames(kFrameMs);
    click(SDL_BUTTON_RIGHT, megamap_at(0));
    const auto by_mouse = result();
    const bool mouse_menu = showing("ARMOPT.GUI");
    if (match_paused_)
        resume_match_pause();
    touch_start(input::interface_right_click, true);
    show_match_pause_menu();
    touch_frames(kFrameMs);
    tap(megamap_spot(0), 1);
    const auto by_touch = result();
    const bool touch_menu = showing("ARMOPT.GUI");
    if (match_paused_)
        resume_match_pause();
    if (!by_touch.same_as(by_mouse) || by_touch.destination != by_mouse.destination ||
        touch_menu != mouse_menu)
        touch_failures.push_back(
            "right-click interface: a tap on the megamap beside the menu left " +
            describe(by_touch) + ", the mouse's right press " + describe(by_mouse)
        );
    // A box that takes in no unit closes the unit info panel, as the mouse's
    // does.
    touch_start(input::interface_left_click, true);
    set_megamap_open(false);
    std::ignore = battlefield_at(mover);
    require(open_unit_info(), "the unit info panel did not open for the mover");
    set_megamap_open(true);
    touch_frames(kFrameMs);
    const auto box_corner = bare_spot(2 * kBoxMargin);
    drag(box_corner, {box_corner.first + 2 * kBoxMargin, box_corner.second + 2 * kBoxMargin});
    const auto empty_box = result();
    if (unit_info_panel_ || empty_box.any_selected)
        touch_failures.push_back(
            "a drag on the megamap that takes in no unit left " + describe(empty_box) +
            (unit_info_panel_ ? ", the unit info panel open" : "")
        );
    close_unit_info();
    set_megamap_open(false);

    match_->stop_orders(mover);
    match_->stop_orders(collector);
    match_->stop_orders(commander);
    clear_local_selection();
    wheel_zoom = kept_wheel_zoom;
    megamap_wheel_zoom_changed();
    // Every part's failures, each under its heading.
    std::string text;
    for (const auto& [heading, list] :
         {std::pair{"the megamap's clicks differ from the battlefield's:", &failures},
          std::pair{"the panels over the megamap do not take their own clicks:", &panel_failures},
          std::pair{
              "fingers on the megamap do not act as the mouse's clicks there:", &touch_failures
          }}) {
        if (list->empty())
            continue;
        text += std::string(text.empty() ? "" : "\n") + heading;
        for (const auto& failure : *list)
            text += "\n  " + failure;
    }
    if (!text.empty())
        fail(text);
    std::cout << "megamap click check: the megamap's clicks give the battlefield's results in "
                 "both interface types: orders, selection, deselection, guard, the armed "
                 "command's order and cancel, and no patrol for a solar collector\n";
    std::cout << "megamap click check: over the megamap the exit menu, its questions and the "
                 "save page take their own clicks and taps in both interface types; beside the "
                 "in-game menu a left click does nothing and a right press acts as the "
                 "battlefield's; a drop of the selection on it closes the unit info panel\n";
    std::cout << "megamap click check: the touch controls' taps, double tap, order wheel, box "
                 "and placement on the megamap act as the mouse's clicks there in both "
                 "interface types, and beside the in-game menu as the mouse's\n";
}

void Runtime::check_megamap_picture() {
    set_megamap_open(true);
    render_match_surface();
    require(megamap_shown(), "the megamap did not open");
    const auto& world = match_->state();
    const auto layout = megamap_.layout;
    require(layout.width > 0 && layout.height > 0, "the megamap has no picture");

    // Every difference from what the megamap should draw, reported together.
    std::vector<std::string> failures;

    // The terrain keeps to the colours of the map's tiles.
    const auto plain = downscale_megamap_terrain(layout);
    require(
        !plain.empty() && plain.size() == megamap_.terrain.size(),
        "the terrain picture and the megamap's picture differ in size"
    );
    std::array<bool, 256> map_colours{};
    for (const uint8_t pixel : selected_tnt_->tile_palette_indices)
        map_colours[pixel] = true;
    std::size_t foreign = 0;
    for (const uint8_t pixel : plain)
        foreign += map_colours[pixel] ? 0U : 1U;
    if (foreign != 0)
        failures.push_back(
            "the terrain picture takes " + std::to_string(foreign) +
            " pixels of colours the map's tiles do not use"
        );

    // The features: those the megamap draws are the indestructible ones
    // that cannot be reclaimed; each may change the picture only near its
    // spot, raised by half the ground's height.
    struct Drawn {
        int32_t x{};
        int32_t y{};
        int32_t reach{};
        uint8_t mark{};
        bool picture{};
        std::size_t changed{};
        std::size_t unlike_mark{};
    };

    std::vector<Drawn> drawn;
    std::size_t left_alone = 0;
    const auto width = static_cast<int32_t>(world.game.map_width);
    const auto cells = static_cast<std::size_t>(world.game.map_width) * world.game.map_height;
    for (std::size_t index = 0; index < cells && width > 0; ++index) {
        const auto& plot = world.plots[index];
        if (plot.feature >= OA_PLOT_FEATURE_RESERVED || plot.feature >= world.feature_def_count)
            continue;
        const auto& def = world.feature_defs[plot.feature];
        if ((def.flags & OA_FEATURE_FLAG_INDESTRUCTIBLE) == 0 ||
            (def.flags & OA_FEATURE_FLAG_RECLAIMABLE) != 0) {
            ++left_alone;
            continue;
        }
        const auto cell_x = static_cast<int32_t>(index % static_cast<std::size_t>(width));
        const auto cell_z = static_cast<int32_t>(index / static_cast<std::size_t>(width));
        const auto* frame = feature_sequence_image(def.seq_name, 0);
        const int32_t frame_side =
            frame != nullptr ? std::max<int32_t>(frame->width, frame->height) : 0;
        const auto spot = hud::megamap_point(
            layout,
            cell_x * cell_pixels + std::max<int32_t>(1, def.footprint_x) * cell_pixels / 2,
            cell_z * cell_pixels + std::max<int32_t>(1, def.footprint_z) * cell_pixels / 2 -
                plot.height / 2
        );
        const int32_t scaled = static_cast<int32_t>(
            static_cast<int64_t>(frame_side) * layout.width / layout.map_width
        );
        drawn.push_back(
            {spot[0] - layout.left,
             spot[1] - layout.top,
             std::max(scaled, hud::feature_mark_size) + feature_reach_margin,
             hud::feature_mark_color(def),
             frame != nullptr}
        );
    }
    require(left_alone != 0, "the map places no feature the megamap leaves out");
    require(
        std::any_of(drawn.begin(), drawn.end(), [](const Drawn& one) { return one.picture; }),
        "the map places no feature with a picture the megamap draws"
    );
    std::size_t stray = 0;
    for (int32_t y = 0; y < layout.height; ++y)
        for (int32_t x = 0; x < layout.width; ++x) {
            const auto at = static_cast<std::size_t>(y) * static_cast<std::size_t>(layout.width) +
                            static_cast<std::size_t>(x);
            if (megamap_.terrain[at] == plain[at])
                continue;
            bool near = false;
            for (auto& one : drawn)
                if (std::abs(x - one.x) <= one.reach && std::abs(y - one.y) <= one.reach) {
                    near = true;
                    ++one.changed;
                    one.unlike_mark += megamap_.terrain[at] != one.mark ? 1U : 0U;
                }
            stray += near ? 0U : 1U;
        }
    if (stray != 0)
        failures.push_back(
            std::to_string(stray) +
            " pixels of the megamap's picture changed away from the features it draws: "
            "reclaimable or destructible features are drawn"
        );
    std::size_t pictured = 0;
    std::size_t flat = 0;
    for (const auto& one : drawn) {
        if (!one.picture || one.changed == 0)
            continue;
        ++pictured;
        flat += one.unlike_mark == 0 ? 1U : 0U;
    }
    if (pictured == 0)
        failures.push_back("no feature with a picture changed the megamap's picture");
    if (flat != 0)
        failures.push_back(
            std::to_string(flat) + " of " + std::to_string(pictured) +
            " features with a picture are drawn as flat marks of one colour"
        );

    // Moving the main view leaves the megamap as it was: it has no
    // rectangle for the view. The bars beside the map are a dark grey.
    const auto view_at = [&](int32_t x, int32_t z) {
        set_camera_position(x, z, 0);
        render_match_surface();
        return match_world_cpu_;
    };
    const auto first = view_at(0, 0);
    const auto second = view_at(
        world.game.map_pixel_width - visible_map_width(),
        world.game.map_pixel_height - visible_map_height()
    );
    require(
        first.width == second.width && first.height == second.height,
        "the battlefield layer changed size"
    );
    const auto pixel_at = [](const renderer::Surface& layer, int32_t x, int32_t y) {
        const auto at =
            (static_cast<std::size_t>(y) * layer.width + static_cast<std::size_t>(x)) * 3U;
        return std::array<uint8_t, 3>{layer.rgb[at], layer.rgb[at + 1], layer.rgb[at + 2]};
    };
    std::size_t moved = 0;
    for (int32_t y = std::max(0, layout.top);
         y < std::min(layout.top + layout.height, static_cast<int32_t>(first.height));
         ++y)
        for (int32_t x = std::max(0, layout.left);
             x < std::min(layout.left + layout.width, static_cast<int32_t>(first.width));
             ++x)
            moved += pixel_at(first, x, y) != pixel_at(second, x, y) ? 1U : 0U;
    if (moved != 0)
        failures.push_back(
            std::to_string(moved) +
            " pixels of the megamap changed with the main view: it draws the view's rectangle"
        );
    const auto area = overlay_area();
    const int32_t area_left = area.x - match_layout_.battlefield_x();
    const int32_t area_top = area.y - match_layout_.battlefield_y();
    std::optional<std::array<int32_t, 2>> bar;
    if (layout.left > area_left)
        bar = std::array<int32_t, 2>{(area_left + layout.left) / 2, layout.top + layout.height / 2};
    else if (layout.top + layout.height < area_top + area.height)
        bar = std::array<int32_t, 2>{
            layout.left + layout.width / 2,
            (layout.top + layout.height + area_top + area.height) / 2
        };
    if (bar) {
        const auto pal = static_cast<std::size_t>(megamap_bar_color) * 4U;
        const std::array<uint8_t, 3> grey{
            match_palette_[pal], match_palette_[pal + 1], match_palette_[pal + 2]
        };
        if (pixel_at(second, (*bar)[0], (*bar)[1]) != grey)
            failures.push_back("the bars beside the megamap's picture are not the dark grey");
    }
    set_megamap_open(false);
    if (!failures.empty()) {
        std::string text;
        for (const auto& failure : failures)
            text += "\n  " + failure;
        fail("the megamap's picture differs from what it should draw:" + text);
    }
    std::cout << "megamap click check: the picture keeps to the map's colours, draws " << pictured
              << " of the map's indestructible features by their pictures and " << left_alone
              << " others not at all, and draws no rectangle for the main view"
              << (bar ? ", beside dark grey bars\n" : "\n");
}

} // namespace oa::app
