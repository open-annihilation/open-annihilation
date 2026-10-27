// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The parts of a --stage file that play out over its match: lines timed for
// later ticks, units placed by map pixel, the groups the stage's units join
// and the orders it gives them. apply_stage (runtime_benchmark.cpp) reads the
// file and carries out its lines through run_stage_direction; the match's
// steps run the timed lines (run_due_stage_lines).
#include "oa/app/runtime.hpp"
#include "stage_state.hpp"

#include "oa/app/frame_coordinates.hpp"

#include "oa/core/unit.h"
#include "oa/core/world.h"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/sim/ground_orders/goals.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/unit_spawn/spawn.hpp"
#include "oa/sim/unit_spawn/spawn_runtime.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {
namespace {

/// The shift from whole map pixels to 16.16 world units.
constexpr uint32_t kStageFixedShift = 16;

/// A facing a place line names, and the quarter turns from south it stands
/// for.
struct FacingName {
    std::string_view name{};
    uint8_t facing{};
};

/// The facings a place line may name.
constexpr std::array<FacingName, 4> kFacingNames{
    {{"south", oa::sim::unit_spawn::facing_south},
     {"east", oa::sim::unit_spawn::facing_east},
     {"north", oa::sim::unit_spawn::facing_north},
     {"west", oa::sim::unit_spawn::facing_west}}
};

/// A command an arm line names, and the command it arms.
struct ArmedCommandName {
    std::string_view name{};
    MatchCommand command{};
};

/// The commands an arm line may name.
constexpr std::array<ArmedCommandName, 10> kArmedCommands{
    {{"none", MatchCommand::none},
     {"move", MatchCommand::move},
     {"attack", MatchCommand::attack},
     {"patrol", MatchCommand::patrol},
     {"guard", MatchCommand::guard},
     {"repair", MatchCommand::repair},
     {"reclaim", MatchCommand::reclaim},
     {"capture", MatchCommand::capture},
     {"load", MatchCommand::load},
     {"unload", MatchCommand::unload}}
};

/// Returns a map pixel as a 16.16 world point on the ground.
///
/// @param match the running match, whose terrain gives the height
/// @param x map pixels across
/// @param z map-image row
/// @return the point
[[nodiscard]] oa::sim::ground_orders::Point
ground_point(oa::sim::match_runtime::Match& match, int32_t x, int32_t z) {
    const auto fixed_x = static_cast<uint32_t>(x) << kStageFixedShift;
    const auto fixed_z = static_cast<uint32_t>(z) << kStageFixedShift;
    return {
        static_cast<int32_t>(fixed_x),
        static_cast<int32_t>(
            static_cast<uint32_t>(match.map_height(fixed_x, fixed_z)) << kStageFixedShift
        ),
        static_cast<int32_t>(fixed_z)
    };
}

} // namespace

void Runtime::destroy_stage_state(StageState* state) noexcept {
    delete state;
}

void Runtime::begin_stage_state() {
    stage_.reset(new StageState{});
    for (const auto& slot : match_->world().slots)
        if (slot.unit != nullptr && slot.record.type_index != 0)
            stage_->origins.try_emplace(
                slot.record.owner_index,
                static_cast<int32_t>(slot.unit->position[0] >> kStageFixedShift),
                static_cast<int32_t>(slot.unit->position[2] >> kStageFixedShift)
            );
}

bool Runtime::take_stage_tick(
    std::istream& line, const std::string& where, const StageLine& source, std::string& action
) {
    int64_t tick = -1;
    if (!(line >> tick) || tick < 0 || tick > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error(where + ": at takes TICK ACTION");
    if (tick > match_timing_.tick) {
        // Kept after every line of the same tick or an earlier one, so that
        // a tick's lines run in the order the file gives them.
        auto& timed = stage_->timed;
        const auto later = std::upper_bound(
            timed.begin() + static_cast<std::ptrdiff_t>(stage_->next_timed),
            timed.end(),
            tick,
            [](int64_t value, const StageState::TimedLine& entry) { return value < entry.tick; }
        );
        timed.insert(later, {static_cast<uint32_t>(tick), source});
        return false;
    }
    if (!(line >> action))
        throw std::runtime_error(where + ": at takes TICK ACTION");
    return true;
}

void Runtime::run_due_stage_lines() {
    if (!stage_ || !match_)
        return;
    std::vector<StageLine> due;
    const auto& timed = stage_->timed;
    while (stage_->next_timed < timed.size() &&
           timed[stage_->next_timed].tick <= match_timing_.tick)
        due.push_back(timed[stage_->next_timed++].line);
    if (due.empty())
        return;
    std::printf("stage: tick %u\n", match_timing_.tick);
    run_stage_lines(due);
}

void Runtime::join_stage_group(uint16_t unit) {
    stage_->last_unit = unit;
    // A unit in a slot an earlier member held is not that member.
    for (auto& [name, members] : stage_->groups)
        std::erase_if(members, [unit](const StageState::Member& member) {
            return member.unit == unit;
        });
    if (stage_->group.empty())
        return;
    const auto& record = match_->world().slots[unit].record;
    stage_->groups[stage_->group].push_back({unit, record.owner_index, record.type_index});
}

bool Runtime::run_stage_direction(
    const std::string& action, std::istream& line, const std::string& where
) {
    // The live units of a group, in the order they joined; throws for a
    // group no unit joined.
    const auto live_members = [&](const std::string& name) {
        const auto found = stage_->groups.find(name);
        if (found == stage_->groups.end())
            throw std::runtime_error(where + ": no unit joined the group " + name);
        std::vector<uint16_t> units;
        const auto& slots = match_->world().slots;
        for (const auto& member : found->second) {
            if (member.unit >= slots.size())
                continue;
            const auto& slot = slots[member.unit];
            if (slot.unit != nullptr && slot.record.type_index == member.type &&
                slot.record.owner_index == member.owner &&
                oa::unit_is_live_target(slot.record.flags))
                units.push_back(member.unit);
        }
        return units;
    };
    // A pointer event at a point of the match canvas, sent as the window
    // sends the player's.
    const auto send_stage_pointer = [this](SDL_EventType type, uint8_t button, float x, float y) {
        float window_x = x;
        float window_y = y;
        if (sdl_.renderer != nullptr && !frame_to_window(sdl_.renderer, x, y, &window_x, &window_y))
            throw std::runtime_error(
                std::string("the pointer could not reach the window: ") + SDL_GetError()
            );
        SDL_Event event{};
        event.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            event.motion.windowID = sdl_.window != nullptr ? SDL_GetWindowID(sdl_.window) : 0;
            event.motion.x = window_x;
            event.motion.y = window_y;
        } else {
            event.button.windowID = sdl_.window != nullptr ? SDL_GetWindowID(sdl_.window) : 0;
            event.button.button = button;
            event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
        }
        bool running = true;
        dispatch_event(event, running);
    };
    if (action == "group") {
        std::string name;
        line >> name;
        stage_->group = name;
        std::printf("stage: group %s\n", name.empty() ? "(none)" : name.c_str());
        return true;
    }
    if (action == "place") {
        int32_t player = -1;
        std::string name;
        int32_t x = 0;
        int32_t z = 0;
        if (!(line >> player >> name >> x >> z))
            throw std::runtime_error(where + ": place takes PLAYER TYPE X Z [FACING]");
        uint8_t facing = oa::sim::unit_spawn::facing_south;
        if (std::string word; line >> word) {
            const auto named = std::find_if(
                kFacingNames.begin(), kFacingNames.end(), [&word](const FacingName& entry) {
                    return entry.name == word;
                }
            );
            if (named == kFacingNames.end())
                throw std::runtime_error(
                    where + ": a place line faces south, east, north or west, not " + word
                );
            facing = named->facing;
        }
        const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
        if (type == 0)
            throw std::runtime_error(where + ": the game has no type " + name);
        if (player < 0 || player >= OA_PLAYER_COUNT ||
            match_->state().game.players[player].in_use == 0)
            throw std::runtime_error(
                where + ": player " + std::to_string(player) + " is not playing"
            );
        const auto& game = match_->state().game;
        if (x < 0 || z < 0 || x >= game.map_pixel_width || z >= game.map_pixel_height)
            throw std::runtime_error(
                where + ": " + std::to_string(x) + "," + std::to_string(z) + " is off the map"
            );
        const auto point = ground_point(*match_, x, z);
        oa::sim::unit_spawn::Request request;
        request.player = static_cast<uint8_t>(player);
        request.type = type;
        request.finished = true;
        request.state = kGroundOccupancyState;
        request.position = {
            static_cast<uint32_t>(point[0]),
            static_cast<uint32_t>(point[1]),
            static_cast<uint32_t>(point[2])
        };
        request.facing = facing;
        const auto* placed = match_->create(request);
        if (placed == nullptr || placed->unit == nullptr)
            throw std::runtime_error(where + ": " + name + " could not be placed");
        join_stage_group(placed->unit_index);
        std::printf(
            "stage: unit %u %s of player %d at %d,%d\n",
            static_cast<unsigned>(placed->unit_index),
            name.c_str(),
            player,
            x,
            z
        );
        return true;
    }
    if (action == "move" || action == "patrol") {
        std::string group;
        int32_t x = 0;
        int32_t z = 0;
        if (!(line >> group >> x >> z))
            throw std::runtime_error(where + ": " + action + " takes GROUP X Z");
        const auto point = ground_point(*match_, x, z);
        // The group is ordered as a selection of its live units is: each
        // keeps its place around the point, measured from the centre of all
        // of them.
        const auto members = live_members(group);
        oa::sim::gameplay_input::GroupCentre centre{};
        const auto& world = match_->state();
        for (const auto unit : members)
            if (const auto* member = oa::world_unit_at(&world, unit); member != nullptr)
                oa::sim::gameplay_input::add_to_group_centre(centre, *member);
        const auto command = action == "move" ? oa::sim::gameplay_input::OrderCommand::move
                                              : oa::sim::gameplay_input::OrderCommand::patrol;
        std::size_t ordered = 0;
        for (const auto unit : members) {
            if (!match_->takes_move_order(unit))
                continue;
            const auto at = group_order_destination(centre, command, unit, 0, point);
            if (action == "move")
                match_->issue_ground_move(unit, at, false);
            else
                match_->issue_patrol(unit, at, false);
            ++ordered;
        }
        std::printf(
            "stage: %s %s: %zu units to %d,%d\n", action.c_str(), group.c_str(), ordered, x, z
        );
        return true;
    }
    if (action == "attack") {
        std::string group;
        std::string targets;
        if (!(line >> group >> targets))
            throw std::runtime_error(where + ": attack takes GROUP TARGETS");
        const auto candidates = live_members(targets);
        const auto& slots = match_->world().slots;
        std::size_t ordered = 0;
        for (const auto unit : live_members(group)) {
            // The nearest live unit of the targets; the first of them joined
            // when two are as near.
            const auto& from = slots[unit].unit->position;
            uint16_t nearest = 0;
            int64_t nearest_distance = 0;
            for (const auto candidate : candidates) {
                const auto& to = slots[candidate].unit->position;
                const int64_t dx = (static_cast<int32_t>(to[0]) >> kStageFixedShift) -
                                   (static_cast<int32_t>(from[0]) >> kStageFixedShift);
                const int64_t dz = (static_cast<int32_t>(to[2]) >> kStageFixedShift) -
                                   (static_cast<int32_t>(from[2]) >> kStageFixedShift);
                const int64_t distance = dx * dx + dz * dz;
                if (nearest == 0 || distance < nearest_distance) {
                    nearest = candidate;
                    nearest_distance = distance;
                }
            }
            if (nearest != 0 && match_->issue_attack_command(unit, nearest, false, nullptr))
                ++ordered;
        }
        std::printf("stage: attack %s: %zu units on %s\n", group.c_str(), ordered, targets.c_str());
        return true;
    }
    if (action == "attack-ground") {
        std::string group;
        int32_t x = 0;
        int32_t z = 0;
        if (!(line >> group >> x >> z))
            throw std::runtime_error(where + ": attack-ground takes GROUP X Z");
        const auto point = ground_point(*match_, x, z);
        std::size_t ordered = 0;
        for (const auto unit : live_members(group))
            if (match_->issue_attack_ground(unit, point, false) != nullptr)
                ++ordered;
        std::printf("stage: attack-ground %s: %zu units at %d,%d\n", group.c_str(), ordered, x, z);
        return true;
    }
    if (action == "guard") {
        std::string group;
        std::string guarded;
        if (!(line >> group >> guarded))
            throw std::runtime_error(where + ": guard takes GROUP GUARDED");
        // The first live unit of the guarded group, by the order it joined.
        const auto candidates = live_members(guarded);
        std::size_t ordered = 0;
        for (const auto unit : live_members(group)) {
            if (candidates.empty() || candidates.front() == unit)
                continue;
            (void)match_->issue_guard(unit, candidates.front(), false);
            ++ordered;
        }
        std::printf("stage: guard %s: %zu units on %s\n", group.c_str(), ordered, guarded.c_str());
        return true;
    }
    if (action == "health") {
        std::string group;
        int32_t percent = -1;
        if (!(line >> group >> percent) || percent < 1 || percent > 100)
            throw std::runtime_error(where + ": health takes GROUP PERCENT, from 1 to 100");
        // Each live unit left with that share of its type's health, rounded
        // down, but never below 1.
        std::size_t changed = 0;
        auto& world = match_->state();
        for (const auto unit : live_members(group)) {
            auto& record = match_->world().slots[unit].record;
            const auto* def = oa::world_unit_def_of(&world, &record);
            if (def == nullptr)
                continue;
            const auto health = static_cast<int64_t>(def->max_damage) * percent / int64_t{100};
            record.health = static_cast<int16_t>(std::max<int64_t>(health, 1));
            ++changed;
        }
        std::printf("stage: health %s: %zu units at %d%%\n", group.c_str(), changed, percent);
        return true;
    }
    if (action == "select") {
        std::string group;
        if (!(line >> group))
            throw std::runtime_error(where + ": select takes GROUP");
        // The local player's selection becomes the group's live units of the
        // local player, as a box dragged round them makes it.
        clear_local_selection();
        std::size_t selected = 0;
        for (const auto unit : live_members(group))
            if (match_->world().slots[unit].record.owner_index == match_local_player_) {
                adopt_selection(unit);
                ++selected;
            }
        apply_match_hud_for_selection();
        std::printf("stage: select %s: %zu units\n", group.c_str(), selected);
        return true;
    }
    if (action == "interface") {
        std::string kind;
        if (!(line >> kind) || (kind != "left" && kind != "right"))
            throw std::runtime_error(where + ": interface takes left or right");
        match_->state().game.interface_type = kind == "left"
                                                  ? oa::sim::gameplay_input::interface_left_click
                                                  : oa::sim::gameplay_input::interface_right_click;
        std::printf("stage: interface %s\n", kind.c_str());
        return true;
    }
    if (action == "arm") {
        std::string name;
        line >> name;
        const auto armed = std::find_if(
            kArmedCommands.begin(), kArmedCommands.end(), [&name](const ArmedCommandName& entry) {
                return entry.name == name;
            }
        );
        if (armed == kArmedCommands.end())
            throw std::runtime_error(
                where + ": arm takes none, move, attack, patrol, guard, repair, reclaim, " +
                "capture, load or unload"
            );
        // The command as its order button arms it; none goes back to the
        // default order.
        reset_match_command();
        match_command_ = armed->command;
        std::printf("stage: arm %s\n", name.c_str());
        return true;
    }
    if (action == "hover") {
        std::string group;
        if (!(line >> group))
            throw std::runtime_error(where + ": hover takes GROUP");
        const auto members = live_members(group);
        if (members.empty())
            throw std::runtime_error(where + ": the group " + group + " has no live unit");
        // The view centred on the group's first live unit, as far as the
        // view's limits let it go, and the pointer moved onto it, as the
        // player's pointer moves. A unit past the map's left or top edge
        // stands at a place below 0.
        const auto unit = members.front();
        const auto& position = match_->world().slots[unit].unit->position;
        set_camera_position(
            (static_cast<int32_t>(position[0]) >> kStageFixedShift) - visible_map_width() / 2,
            (static_cast<int32_t>(position[2]) >> kStageFixedShift) - visible_map_height() / 2,
            0
        );
        render_match_surface();
        const auto point =
            project_match_point(live_viewport(match_camera_x_, match_camera_z_), position);
        send_stage_pointer(
            SDL_EVENT_MOUSE_MOTION, 0, static_cast<float>(point.x), static_cast<float>(point.y)
        );
        const auto cursor = pick_match_cursor();
        const std::string cursor_name = cursor < kCursorNames.size()
                                            ? std::string(kCursorNames[cursor])
                                            : std::to_string(cursor);
        std::printf(
            "stage: hover %s: unit %u under the pointer, cursor %s\n",
            group.c_str(),
            static_cast<unsigned>(hovered_match_unit_),
            cursor_name.c_str()
        );
        return true;
    }
    if (action == "press") {
        std::string button;
        if (!(line >> button) || (button != "left" && button != "right"))
            throw std::runtime_error(where + ": press takes left or right");
        // The button pressed and let go where the pointer is.
        const uint8_t index = button == "left" ? SDL_BUTTON_LEFT : SDL_BUTTON_RIGHT;
        status_.clear();
        for (const auto type : {SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP})
            send_stage_pointer(type, index, pointer_x_, pointer_y_);
        std::printf(
            "stage: press %s: %s\n", button.c_str(), status_.empty() ? "-" : status_.c_str()
        );
        return true;
    }
    if (action == "drag") {
        std::array<int32_t, 4> corners{};
        if (!(line >> corners[0] >> corners[1] >> corners[2] >> corners[3]))
            throw std::runtime_error(where + ": drag takes X0 Z0 X1 Z1");
        // The points of the battlefield that show the map pixels at height
        // 0, from the view as it is, past the map's edges too; a point out of
        // view is held a pixel inside the battlefield's edges, as the
        // screen's edges hold the player's pointer short of the scroll.
        render_match_surface();
        const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
        const auto shown = [&](int32_t x, int32_t z) {
            const auto point = project_match_point(
                viewport,
                {static_cast<uint32_t>(x) << kStageFixedShift,
                 0,
                 static_cast<uint32_t>(z) << kStageFixedShift}
            );
            return std::array<float, 2>{
                static_cast<float>(std::clamp(
                    point.x,
                    match_layout_.left + 1,
                    match_layout_.left + match_layout_.battlefield_width() - 2
                )),
                static_cast<float>(std::clamp(
                    point.y,
                    match_layout_.top + 1,
                    match_layout_.top + match_layout_.battlefield_height() - 2
                ))
            };
        };
        const auto from = shown(corners[0], corners[1]);
        const auto to = shown(corners[2], corners[3]);
        // The select button held from the one to the other, through the
        // middle, as the player drags a box; the same point twice is a click.
        const uint8_t select = match_select_button();
        status_.clear();
        send_stage_pointer(SDL_EVENT_MOUSE_MOTION, 0, from[0], from[1]);
        send_stage_pointer(SDL_EVENT_MOUSE_BUTTON_DOWN, select, from[0], from[1]);
        send_stage_pointer(
            SDL_EVENT_MOUSE_MOTION, 0, (from[0] + to[0]) / 2.0F, (from[1] + to[1]) / 2.0F
        );
        send_stage_pointer(SDL_EVENT_MOUSE_MOTION, 0, to[0], to[1]);
        // The box as it shows while the button is still held.
        if (from != to)
            write_stage_snapshot("drag");
        send_stage_pointer(SDL_EVENT_MOUSE_BUTTON_UP, select, to[0], to[1]);
        std::printf(
            "stage: drag %d,%d to %d,%d: %s\n",
            corners[0],
            corners[1],
            corners[2],
            corners[3],
            status_.empty() ? "-" : status_.c_str()
        );
        return true;
    }
    if (action == "activate" || action == "deactivate") {
        std::string group;
        if (!(line >> group))
            throw std::runtime_error(where + ": " + action + " takes GROUP");
        // The ON/OFF button's order, given to each unit as to a selection: a
        // type that cannot be switched on and off takes it and stays as it is.
        const char* tag = action == "activate" ? "ACTIVATE" : "DEACTIVATE";
        std::size_t ordered = 0;
        for (const auto unit : live_members(group))
            if (give_state_order(unit, tag, 0))
                ++ordered;
        std::printf("stage: %s %s: %zu units\n", action.c_str(), group.c_str(), ordered);
        return true;
    }
    if (action == "move-order" || action == "fire-order") {
        std::string group;
        int32_t stance = -1;
        if (!(line >> group >> stance) || stance < 0 || stance > 2)
            throw std::runtime_error(
                where + ": " + action + " takes GROUP STANCE, the stance 0, 1 or 2"
            );
        // The order panel's standing-order button's order, given to each
        // unit as to a selection: a type without that button stays as it is.
        const char* tag = action == "move-order" ? "Standing_MoveOrder" : "Standing_FireOrder";
        std::size_t ordered = 0;
        for (const auto unit : live_members(group))
            if (give_state_order(unit, tag, stance))
                ++ordered;
        std::printf(
            "stage: %s %s: %zu units to %d\n", action.c_str(), group.c_str(), ordered, stance
        );
        return true;
    }
    return false;
}

} // namespace oa::app
