// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The play host (play_host.hpp). oa-app-mcp is the only caller. Speed goes
// through adjust_game_speed, one step at a time as the + and - keys do, so
// speed_changed runs and a shared match stays in step. Pause and resume go
// through the Pause key's hotkey, so pause_changed runs the same way. The
// camera goes through set_camera_position, center_camera_on_unit,
// centre_view_on and zoom_match_about, and each of those holds the view
// within its limits. A move then shows on the next presented frame the way
// a scroll does, paused or not. The clock entries hold a single-player
// match until a granted run asks it to tick, and a shared match is refused.
// Orders go through the match's own order calls. A move or a patrol of
// several units keeps the group's shape. Standing orders go through the
// order panel's path. Selection is the player's clear and adopt. A replay,
// a watcher, a finished match, and a unit this machine does not play are
// refused. A stepped clock holds an order until a step runs it, the way a
// speed change waits to be felt.
#include "oa/app/play_host.hpp"

#include "oa/app/runtime.hpp"
#include "oa/core/unit.h"
#include "oa/core/unit_def.h"
#include "oa/data/mission_types.hpp"
#include "oa/present/scene_grid.hpp"
#include "oa/present/world_renderer/world_camera.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/sim/ground_orders/goals.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/simulation_state.hpp"
#include "oa/sim/speed.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/console/hotkeys.hpp"

#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

namespace {

// A speed fits in the match's speed field, so this many steps of one reaches
// any speed that field can hold, or a step that does not move.
constexpr int kMaxSpeedSteps = 65535;

// The most units one order or selection names. A longer list is refused whole.
constexpr std::size_t kMaxOrderedUnits = 64;

// How many orders of one queue are copied back. The same bound units.orders uses.
constexpr std::size_t kQueueCopy = 32;

/// A refusal, with nothing changed.
///
/// @param detail why nothing was given
/// @return the result
PlayCommandResult refused_result(std::string detail) {
    PlayCommandResult result;
    result.detail = std::move(detail);
    return result;
}

/// Turns a whole map pixel into the 16.16 fixed point orders store.
///
/// @param pixels the map pixel
/// @return the fixed point
oa_fixed fixed_pixel(int32_t pixels) {
    return static_cast<oa_fixed>(static_cast<uint32_t>(pixels) << 16);
}

/// The fixed point a request's map pixels name. y is 0 when the caller gave only x and z.
///
/// @param pixels the map pixels
/// @return the 16.16 point
oa::FixedVec3 fixed_of(const PlayOrderPoint& pixels) {
    return oa::FixedVec3{
        fixed_pixel(pixels.x),
        fixed_pixel(pixels.y),
        fixed_pixel(pixels.z),
    };
}

/// The ground-order point of a 16.16 vector.
///
/// @param at the 16.16 point
/// @return the point orders take
oa::sim::ground_orders::Point point_of(const oa::FixedVec3& at) {
    return oa::sim::ground_orders::Point{at.x, at.y, at.z};
}

/// Copies a fixed character field, stopping at the first NUL.
///
/// @param bytes the field
/// @param capacity its size, counting a NUL when it has one
/// @return the text
std::string field_text(const char* bytes, std::size_t capacity) {
    if (bytes == nullptr)
        return {};
    std::size_t length = 0;
    while (length < capacity && bytes[length] != '\0')
        ++length;
    return std::string(bytes, length);
}

/// Tells whether a unit name and a type's unit-name field are the same, ignoring ASCII case.
///
/// @param text the name the caller gave
/// @param field the type's unit name
/// @param capacity the field's size
/// @return true when they are the same name
bool same_name(std::string_view text, const char* field, std::size_t capacity) {
    if (field == nullptr)
        return false;
    std::size_t length = 0;
    while (length < capacity && field[length] != '\0')
        ++length;
    if (length != text.size())
        return false;
    for (std::size_t place = 0; place < length; ++place) {
        const auto first = static_cast<unsigned char>(text[place]);
        const auto second = static_cast<unsigned char>(field[place]);
        if (std::tolower(first) != std::tolower(second))
            return false;
    }
    return true;
}

/// Returns a type's unit name when the index is one the match has.
///
/// @param block the match's world
/// @param parameter the order's first parameter
/// @return the unit name; empty when the parameter is not a type index
std::string type_name_of(const oa::World& block, int32_t parameter) {
    if (parameter <= 0 || block.unit_defs == nullptr)
        return {};
    const auto place = static_cast<uint32_t>(parameter);
    if (place >= block.unit_def_count)
        return {};
    const oa::UnitDef& def = block.unit_defs[place];
    return field_text(def.unit_name, sizeof def.unit_name);
}

/// Finds a unit type by its unit name, ignoring ASCII case. Slot 0 is not a type.
///
/// @param block the match's world
/// @param name the unit name
/// @return the type index, or 0 when the match has none of that name
uint16_t find_type(const oa::World& block, const char* name) {
    if (name == nullptr || name[0] == '\0' || block.unit_defs == nullptr)
        return 0;
    const std::string_view wanted(name);
    for (uint32_t place = 1; place < block.unit_def_count; ++place) {
        if (place > 65535u)
            break;
        const oa::UnitDef& def = block.unit_defs[place];
        if (same_name(wanted, def.unit_name, sizeof def.unit_name))
            return static_cast<uint16_t>(place);
    }
    return 0;
}

/// Returns a mission kind's registered name.
///
/// @param kind the order kind
/// @return the name; empty when the kind is not registered
std::string mission_name(uint8_t kind) {
    const std::span<const std::string_view> names = oa::data::mission_types::registered_names();
    if (kind >= names.size())
        return {};
    return std::string(names[kind]);
}

/// Tells whether an order's first parameter is the type it builds.
///
/// @param name the mission's name
/// @return true for a building or mobile build
bool construction_kind(std::string_view name) {
    return name == "BuildingBuild" || name == "MobileBuild" || name == "VTOL_MobileBuild";
}

/// Copies one order record into the words units.orders uses.
///
/// @param block the match's world, for a build's product name
/// @param record the match's record
/// @return the copy
PlayOrderLine
copy_line(const oa::World& block, const oa::sim::match_runtime::Match::OrderRecordView& record) {
    PlayOrderLine line;
    line.kind = mission_name(record.kind);
    line.target = record.target;
    line.x = static_cast<int32_t>(record.point[0] >> 16);
    line.y = static_cast<int32_t>(record.point[1] >> 16);
    line.z = static_cast<int32_t>(record.point[2] >> 16);
    line.parameter_1 = record.parameter_1;
    line.parameter_2 = record.parameter_2;
    line.phase = record.phase;
    line.issue_tick = record.issue_tick;
    line.facing = record.facing;
    line.build_started = record.build_started;
    line.seen_x = static_cast<int32_t>(record.seen_x);
    line.seen_z = static_cast<int32_t>(record.seen_z);
    if (construction_kind(line.kind))
        line.build = type_name_of(block, record.parameter_1);
    return line;
}

/// Copies one queue, head first, and notes when the copy stopped at its limit.
///
/// @param running the match
/// @param block the match's world
/// @param id the unit
/// @param secondary the secondary queue
/// @param[out] truncated set when the queue was longer than the copy
/// @return the orders
std::vector<PlayOrderLine> copy_queue(
    oa::sim::match_runtime::Match& running,
    const oa::World& block,
    uint16_t id,
    bool secondary,
    bool& truncated
) {
    std::array<oa::sim::match_runtime::Match::OrderRecordView, kQueueCopy> records{};
    const std::size_t written =
        running.queue_records(id, secondary, records.data(), records.size());
    truncated = written == records.size();
    std::vector<PlayOrderLine> copied;
    copied.reserve(written);
    for (std::size_t cursor = 0; cursor < written; ++cursor)
        copied.push_back(copy_line(block, records[cursor]));
    return copied;
}

/// One order a stepped clock is holding. The units are copied, so the call's
/// arguments can move; key is the call's own unit list and stays until the
/// call finishes.
struct HeldCommand {
    const void* key{}; ///< the call's unit list
    bool standing{};   ///< a standing order, not an order
    bool landed{};     ///< a step has already given it
    PlayCommandResult result{};
    PlayOrderKind order_kind{};
    bool append{};
    std::vector<uint16_t> units;
    uint16_t target{};
    bool has_point{};
    PlayOrderPoint point{};
    std::string type_name;
    int32_t count{1};
    uint8_t facing{};
    PlayStateKind state_kind{};
    int32_t state_value{};
    bool state_on{};
};

/// The orders one runtime is holding. The runtime pointer is the key.
struct HeldBag {
    Runtime* runtime{};
    std::vector<HeldCommand> commands;
};

/// Every runtime that is holding an order. Empty when the clock is not stepped.
std::vector<HeldBag> held_bags;

/// The bag for this runtime, made when the first order is held.
///
/// @param app the running game
/// @return its bag
HeldBag& bag_of(Runtime& app) {
    for (HeldBag& bag : held_bags) {
        if (bag.runtime == &app)
            return bag;
    }
    HeldBag made;
    made.runtime = &app;
    held_bags.push_back(std::move(made));
    return held_bags.back();
}

/// The held order stored under this call's unit list.
///
/// @param bag the runtime's bag
/// @param key the call's unit list
/// @param standing true for a standing order
/// @return the order, or null when this call is not holding one
HeldCommand* find_held(HeldBag& bag, const void* key, bool standing) {
    if (key == nullptr)
        return nullptr;
    for (HeldCommand& command : bag.commands) {
        if (command.key == key && command.standing == standing)
            return &command;
    }
    return nullptr;
}

/// Drops the order stored under this call's unit list.
///
/// @param bag the runtime's bag
/// @param key the call's unit list
/// @param standing true for a standing order
void erase_held(HeldBag& bag, const void* key, bool standing) {
    std::vector<HeldCommand> kept;
    kept.reserve(bag.commands.size());
    for (HeldCommand& command : bag.commands) {
        if (command.key == key && command.standing == standing)
            continue;
        kept.push_back(std::move(command));
    }
    bag.commands = std::move(kept);
}

/// A request that points at a held order's own copy. It is valid only while
/// that copy lives.
///
/// @param command the held order
/// @return the request give_order reads
PlayOrderRequest order_view(const HeldCommand& command) {
    PlayOrderRequest given;
    given.kind = command.order_kind;
    given.append = command.append;
    given.units = command.units.data();
    given.unit_count = command.units.size();
    given.target = command.target;
    given.has_point = command.has_point;
    given.point = command.point;
    given.type_name = command.type_name.empty() ? nullptr : command.type_name.c_str();
    given.count = command.count;
    given.facing = command.facing;
    return given;
}

/// A request that points at a held standing order's own copy.
///
/// @param command the held standing order
/// @return the request give_state reads
PlayStateRequest state_view(const HeldCommand& command) {
    PlayStateRequest standing;
    standing.kind = command.state_kind;
    standing.value = command.state_value;
    standing.on = command.state_on;
    standing.units = command.units.data();
    standing.unit_count = command.units.size();
    return standing;
}

} // namespace

// The runtime's side of the play host: the one friend through which its
// entries change the match, by the paths the keys use.
struct PlayHostAccess {
    /// Returns the runtime a play host's context names.
    ///
    /// @param context PlayHost::context
    /// @return the runtime
    static Runtime& runtime_of(void* context) { return *static_cast<Runtime*>(context); }

    /// Tells whether the local player may change this match's speed or pause.
    ///
    /// A replay keeps what the recording did. A watcher only watches. No match
    /// on screen has nothing to change.
    ///
    /// @param app the running game
    /// @return true when a change may be made
    static bool changes_allowed(const Runtime& app) {
        if (!app.match_running())
            return false;
        const uint32_t bits = app.current_extension_state();
        if ((bits & extension_state::replay) != 0)
            return false;
        if ((bits & extension_state::local_watcher) != 0)
            return false;
        return true;
    }

    /// Tells whether the match is paused.
    ///
    /// @param app the running game
    /// @return true when the pause is on
    static bool pause_held(const Runtime& app) {
        if (app.match_ == nullptr)
            return false;
        const oa::Game& block = app.match_->state().game;
        return (block.sim_run_flags & oa::ui::console::kSimRunPaused) != 0;
    }

    /// Why an order must not be given, or empty when it may.
    ///
    /// No running match, then a replay, then a watcher, then a finished match.
    /// A shared match may: the match's own order calls are the path a player's
    /// order takes, and they apply to the units this machine plays.
    ///
    /// @param app the running game
    /// @return the sentence, or empty
    static std::string command_refusal(const Runtime& app) {
        if (!app.match_running() || app.match_ == nullptr)
            return "no match is running";
        const uint32_t bits = app.current_extension_state();
        if ((bits & extension_state::replay) != 0)
            return "a replay cannot give orders";
        if ((bits & extension_state::local_watcher) != 0)
            return "a watcher cannot give orders";
        if (app.match_finished_)
            return "the match has finished";
        return {};
    }

    /// Gathers the units a call names, refusing the whole call when one is not this machine's.
    ///
    /// A unit counts when it is live, its owner is the local player, and that
    /// owner is simulated on this machine. A repeated id is kept once, in the
    /// order the call named them. Nothing is written to the match.
    ///
    /// @param app the running game
    /// @param units the ids the call named
    /// @param count how many ids
    /// @param selecting true when a unit the player could not select is refused
    /// @param[out] ids the units, with repeats dropped
    /// @return the sentence, or empty
    static std::string collect_units(
        const Runtime& app,
        const uint16_t* units,
        std::size_t count,
        bool selecting,
        std::vector<uint16_t>& ids
    ) {
        ids.clear();
        if (count > kMaxOrderedUnits)
            return "too many units";
        if (units == nullptr)
            return count == 0 ? std::string{} : std::string("no such unit");
        const oa::World& block = app.match_->state();
        const auto& spawned = app.match_->world();
        ids.reserve(count);
        for (std::size_t place = 0; place < count; ++place) {
            const uint16_t id = units[place];
            if (id == 0 || static_cast<std::size_t>(id) >= spawned.slots.size())
                return "no such unit";
            const auto& slot = spawned.slots[id];
            if (slot.unit == nullptr)
                return "no such unit";
            const bool played_here =
                slot.owner_index == app.match_local_player_ &&
                oa::sim::simulation_state::locally_simulated(block, slot.record);
            if (!played_here)
                return "that unit is not this machine's";
            if (selecting && !oa::sim::simulation_state::unit_selectable(block, slot.record))
                return "that unit cannot be selected";
            bool repeated = false;
            for (uint16_t earlier : ids) {
                if (earlier == id)
                    repeated = true;
            }
            if (!repeated)
                ids.push_back(id);
        }
        return {};
    }

    /// Tells whether a target slot holds a live unit.
    ///
    /// @param app the running game
    /// @param target the other unit
    /// @return true when the slot holds a unit
    static bool target_live(const Runtime& app, uint16_t target) {
        if (target == 0 || app.match_ == nullptr)
            return false;
        const auto& spawned = app.match_->world();
        if (static_cast<std::size_t>(target) >= spawned.slots.size())
            return false;
        return spawned.slots[target].unit != nullptr;
    }

    /// Tells whether any named unit is the order's target.
    ///
    /// @param ids the units
    /// @param target the other unit
    /// @return true when the order would act on its own unit
    static bool aims_at_itself(const std::vector<uint16_t>& ids, uint16_t target) {
        for (uint16_t id : ids) {
            if (id == target)
                return true;
        }
        return false;
    }

    /// Resolves a build's product, or says why the name cannot be built.
    ///
    /// @param app the running game
    /// @param request the order
    /// @param[out] product the type index
    /// @return the sentence, or empty
    static std::string
    product_of(const Runtime& app, const PlayOrderRequest& request, uint16_t& product) {
        product = 0;
        if (request.type_name == nullptr || request.type_name[0] == '\0')
            return "the order needs a unit type";
        product = find_type(app.match_->state(), request.type_name);
        if (product == 0)
            return "no such unit type";
        return {};
    }

    /// Checks the order's point, target, product and count. Nothing is given here.
    ///
    /// @param app the running game
    /// @param request the order
    /// @param ids the units, already this machine's
    /// @param[out] product the type a build or a factory order names
    /// @return the sentence, or empty
    static std::string check_request(
        const Runtime& app,
        const PlayOrderRequest& request,
        const std::vector<uint16_t>& ids,
        uint16_t& product
    ) {
        product = 0;
        switch (request.kind) {
        case PlayOrderKind::move:
        case PlayOrderKind::patrol:
            if (!request.has_point)
                return "the order needs a point";
            return {};
        case PlayOrderKind::attack_unit:
            if (request.target == 0)
                return "an attack needs a unit or a point";
            if (!target_live(app, request.target))
                return "no such target";
            if (aims_at_itself(ids, request.target))
                return "a unit cannot take that order on itself";
            return {};
        case PlayOrderKind::attack_ground:
            if (!request.has_point)
                return "an attack needs a unit or a point";
            return {};
        case PlayOrderKind::guard:
            if (request.target == 0)
                return "a guard needs a unit";
            if (!target_live(app, request.target))
                return "no such target";
            if (aims_at_itself(ids, request.target))
                return "a unit cannot take that order on itself";
            return {};
        case PlayOrderKind::repair:
            if (request.target == 0)
                return "a repair needs a unit";
            if (!target_live(app, request.target))
                return "no such target";
            if (aims_at_itself(ids, request.target))
                return "a unit cannot take that order on itself";
            return {};
        case PlayOrderKind::reclaim_unit:
            if (request.target == 0)
                return "a reclaim needs a unit or a point";
            if (!target_live(app, request.target))
                return "no such target";
            if (aims_at_itself(ids, request.target))
                return "a unit cannot take that order on itself";
            return {};
        case PlayOrderKind::reclaim_ground:
            if (!request.has_point)
                return "a reclaim needs a unit or a point";
            return {};
        case PlayOrderKind::mobile_build:
            if (!request.has_point)
                return "the order needs a point";
            return product_of(app, request, product);
        case PlayOrderKind::building_build:
            if (request.count < 1)
                return "the count is not a number of units";
            return product_of(app, request, product);
        case PlayOrderKind::factory:
            if (request.count == 0)
                return "a factory order needs a count";
            return product_of(app, request, product);
        case PlayOrderKind::stop:
            return {};
        }
        return "the order was refused";
    }

    /// Gives one unit the order. The caller has already checked the units and the target.
    ///
    /// @param app the running game
    /// @param id the unit
    /// @param request the order
    /// @param destination the 16.16 point, already the unit's own point of a group move
    /// @param product the type a build or a factory order names
    /// @return the sentence, or empty when the order was given
    static std::string give_one(
        Runtime& app,
        uint16_t id,
        const PlayOrderRequest& request,
        const oa::sim::ground_orders::Point& destination,
        uint16_t product
    ) {
        oa::sim::match_runtime::Match& running = *app.match_;
        const bool queue = request.append;
        switch (request.kind) {
        case PlayOrderKind::move: {
            const oa::sim::simulation_state::Order& issued =
                running.issue_ground_move(id, destination, queue);
            if (running.order_refused(issued))
                return "the order was refused";
            return {};
        }
        case PlayOrderKind::patrol: {
            const oa::sim::simulation_state::Order& issued =
                running.issue_patrol(id, destination, queue);
            if (running.order_refused(issued))
                return "the order was refused";
            return {};
        }
        case PlayOrderKind::attack_unit: {
            const oa::sim::ground_orders::Point* at = request.has_point ? &destination : nullptr;
            if (!running.issue_attack_command(id, request.target, queue, at))
                return "the order was refused";
            return {};
        }
        case PlayOrderKind::attack_ground: {
            oa::sim::simulation_state::Order* issued =
                running.issue_attack_ground(id, destination, queue);
            if (issued == nullptr || running.order_refused(*issued))
                return "the order was refused";
            return {};
        }
        case PlayOrderKind::guard: {
            const oa::sim::simulation_state::Order& issued =
                running.issue_guard(id, request.target, queue);
            if (running.order_refused(issued))
                return "the order was refused";
            return {};
        }
        case PlayOrderKind::repair: {
            const float remaining = app.match_->world().slots[request.target].build_remaining;
            if (remaining != 0.0F) {
                const oa::sim::simulation_state::Order& issued =
                    running.issue_help_build(id, request.target, queue);
                if (running.order_refused(issued))
                    return "the order was refused";
                return {};
            }
            const oa::sim::simulation_state::Order& issued =
                running.issue_repair(id, request.target, queue);
            if (running.order_refused(issued))
                return "the order was refused";
            return {};
        }
        case PlayOrderKind::reclaim_unit: {
            const oa::sim::simulation_state::Order& issued =
                running.issue_reclaim(id, request.target, queue);
            if (running.order_refused(issued))
                return "the order was refused";
            return {};
        }
        case PlayOrderKind::reclaim_ground: {
            const oa::sim::simulation_state::Order& issued =
                running.issue_feature_reclaim(id, destination, queue);
            if (running.order_refused(issued))
                return "the order was refused";
            return {};
        }
        case PlayOrderKind::mobile_build: {
            const oa::sim::simulation_state::Order& issued =
                running.issue_mobile_build(id, product, destination, queue, request.facing);
            if (running.order_refused(issued))
                return "the order was refused";
            return {};
        }
        case PlayOrderKind::building_build: {
            const oa::sim::simulation_state::Order& issued =
                running.issue_building_build(id, product, request.count, queue);
            if (running.order_refused(issued))
                return "the order was refused";
            return {};
        }
        case PlayOrderKind::factory:
            running.queue_factory_build(id, product, request.count);
            return {};
        case PlayOrderKind::stop: {
            const oa::sim::simulation_state::Order& issued = running.issue_stop(id);
            if (running.order_refused(issued))
                return "the order was refused";
            return {};
        }
        }
        return "the order was refused";
    }

    /// Copies each unit's queues after an order was given.
    ///
    /// @param app the running game
    /// @param ids the units
    /// @return the tick and the queues
    static PlayCommandResult queues_of(Runtime& app, const std::vector<uint16_t>& ids) {
        PlayCommandResult result;
        result.ok = true;
        result.tick = app.match_->state().game.tick;
        const oa::World& block = app.match_->state();
        result.queues.reserve(ids.size());
        for (uint16_t id : ids) {
            PlayUnitOrders copied;
            copied.id = id;
            copied.primary = copy_queue(*app.match_, block, id, false, copied.primary_truncated);
            copied.secondary = copy_queue(*app.match_, block, id, true, copied.secondary_truncated);
            result.queues.push_back(std::move(copied));
        }
        return result;
    }

    /// The units selected afterwards: this machine's, with the selected flag set.
    ///
    /// @param app the running game
    /// @return the slots, in slot order
    static std::vector<uint16_t> chosen_units(const Runtime& app) {
        std::vector<uint16_t> chosen;
        if (app.match_ == nullptr)
            return chosen;
        const auto& spawned = app.match_->world();
        for (std::size_t place = 1; place < spawned.slots.size(); ++place) {
            const auto& slot = spawned.slots[place];
            if (slot.unit == nullptr || slot.owner_index != app.match_local_player_)
                continue;
            if ((slot.record.flags & OA_UNIT_FLAG_SELECTED) == 0)
                continue;
            if (place > 65535u)
                break;
            chosen.push_back(static_cast<uint16_t>(place));
        }
        return chosen;
    }

    /// Sets the speed one key-press at a time (PlayHost::set_speed).
    ///
    /// Each step is adjust_game_speed, the path the speed keys use. The speed
    /// is not written on its own: that would skip the announcement and the
    /// notice the other machines of a shared match follow.
    ///
    /// @param context PlayHost::context
    /// @param speed the absolute speed
    /// @return true when the match is at that speed
    static bool set_speed(void* context, uint16_t speed) {
        Runtime& app = runtime_of(context);
        if (!changes_allowed(app) || app.match_ == nullptr)
            return false;
        const oa::sim::speed::Range limits = app.game_speed_range();
        const int32_t asked = static_cast<int32_t>(speed);
        if (asked < limits.slowest || asked > limits.fastest)
            return false;
        for (int step = 0; step < kMaxSpeedSteps; ++step) {
            const int32_t speed_now =
                static_cast<int32_t>(app.match_->state().game.requested_speed);
            if (speed_now == asked)
                return true;
            app.adjust_game_speed(speed_now < asked ? 1 : -1);
            const int32_t speed_next =
                static_cast<int32_t>(app.match_->state().game.requested_speed);
            if (speed_next == speed_now)
                return false;
        }
        return static_cast<int32_t>(app.match_->state().game.requested_speed) == asked;
    }

    /// Pauses as the Pause key does, and only when the match is not already paused.
    ///
    /// @param context PlayHost::context
    /// @return true when the match is paused
    static bool pause(void* context) {
        Runtime& app = runtime_of(context);
        if (!changes_allowed(app) || app.match_finished_)
            return false;
        if (pause_held(app))
            return true;
        app.run_console_hotkey(oa::ui::console::hotkey::pause);
        return pause_held(app);
    }

    /// Resumes as the Pause key does, and only when the match is paused.
    ///
    /// @param context PlayHost::context
    /// @return true when the match is running
    static bool resume(void* context) {
        Runtime& app = runtime_of(context);
        if (!changes_allowed(app) || app.match_finished_)
            return false;
        if (!pause_held(app))
            return true;
        app.run_console_hotkey(oa::ui::console::hotkey::pause);
        return !pause_held(app);
    }

    /// Reads the match the server checks before it queues a change (PlayHost::extension_state).
    ///
    /// @param context PlayHost::context
    /// @return the match as the server sees it
    static PlayExtensionState read_state(void* context) {
        const Runtime& app = runtime_of(context);
        PlayExtensionState state;
        const uint32_t bits = app.current_extension_state();
        state.in_match = app.match_running();
        state.replay = (bits & extension_state::replay) != 0;
        state.watcher = (bits & extension_state::local_watcher) != 0;
        state.shared_match = (bits & extension_state::shared_match) != 0;
        state.director = app.director_mode();
        const oa::sim::speed::Range limits = app.game_speed_range();
        state.slowest = limits.slowest;
        state.fastest = limits.fastest;
        state.finished = app.match_finished_;
        if (app.match_ != nullptr) {
            const oa::Game& block = app.match_->state().game;
            state.paused = (block.sim_run_flags & oa::ui::console::kSimRunPaused) != 0;
            state.speed = static_cast<int32_t>(block.requested_speed);
            state.tick = block.tick;
        }
        state.stepped = app.stepped_clock_;
        return state;
    }

    /// Reads the view the next frame draws (PlayHost::read_view).
    ///
    /// @param context PlayHost::context
    /// @param shown the view
    /// @return false when `shown` is null
    static bool read_view(void* context, PlayView* shown) {
        if (shown == nullptr)
            return false;
        const Runtime& app = runtime_of(context);
        const std::array<int32_t, 2> camera = app.view_camera();
        shown->in_match = app.match_running();
        shown->x = camera[0];
        shown->z = camera[1];
        shown->zoom = app.match_zoom();
        return true;
    }

    /// Tells whether a camera move may be made: a match is on screen, and the view is not the
    /// director's.
    ///
    /// @param app the running game
    /// @return true when the view may move
    static bool view_may_move(const Runtime& app) {
        return app.match_running() && !app.director_mode();
    }

    /// Shows the view the move just placed, as a scroll shows its view.
    ///
    /// A scroll calls place_match_view, which keeps the exact place and marks
    /// the frame as one whose camera moved. A jump through set_camera_position
    /// clears that exact place and does not mark the frame. The terrain stamps
    /// are dropped so the next frame cannot keep the picture it drew at the
    /// place before. A zoom has already placed its exact view, fraction of a
    /// map pixel and all, and keeps a follow; placing the camera's whole pixel
    /// again would drop that fraction, and stopping the follow would undo the
    /// zoom. The picture itself is drawn at the zoom on every frame, paused or
    /// not: the standard tier, which the software renderer uses, draws the
    /// world layer at match_zoom(), and the accelerated tiers draw a zoom too.
    ///
    /// @param app the running game
    /// @param keep_follow true when a follow of a unit goes on, as a zoom keeps it
    static void show_new_view(Runtime& app, bool keep_follow) {
        const std::array<int32_t, 2> camera = app.view_camera();
        if (!app.exact_view_.held)
            app.place_match_view(static_cast<double>(camera[0]), static_cast<double>(camera[1]));
        else
            app.camera_moved_ = true;
        if (app.match_ != nullptr) {
            app.bind_match_view();
            oa::Game& block = app.match_->state().game;
            if (!keep_follow)
                oa::present::world_renderer::camera_stop_follow(block);
            // A bookmark recall leaves the glide target on the place it
            // showed, so a later glide cannot walk back toward the old one.
            block.camera_target_x = static_cast<int32_t>(block.camera_x);
            block.camera_target_y = static_cast<int32_t>(block.camera_y);
        }
        app.terrain_cache_cam_x_ = kUncachedTerrainCamera;
        app.terrain_cache_cam_y_ = kUncachedTerrainCamera;
        app.terrain_filtered_cam_x_ = kUncachedTerrainCamera;
        app.terrain_filtered_cam_y_ = kUncachedTerrainCamera;
    }

    /// Puts the view's corner at a map pixel (PlayHost::set_camera).
    ///
    /// set_camera_position holds the place within the view's limits. A follow
    /// ends, as a scroll ends it, so the next frame does not put the view back.
    ///
    /// @param context PlayHost::context
    /// @param x the column, in map pixels
    /// @param z the row, in map pixels
    /// @return true when the view was placed
    static bool set_camera(void* context, int32_t x, int32_t z) {
        Runtime& app = runtime_of(context);
        if (!view_may_move(app))
            return false;
        app.stop_match_tracking();
        app.set_camera_position(x, z, 0);
        show_new_view(app, false);
        return true;
    }

    /// Centres the view on a unit (PlayHost::center_unit).
    ///
    /// @param context PlayHost::context
    /// @param unit the unit's id
    /// @return true when the view was centred on the unit
    static bool center_unit(void* context, uint16_t unit) {
        Runtime& app = runtime_of(context);
        if (!view_may_move(app) || !app.match_unit_present(unit))
            return false;
        app.stop_match_tracking();
        app.center_camera_on_unit(unit);
        show_new_view(app, false);
        return true;
    }

    /// Brings a map pixel into the coordinates centre_view_on can name.
    ///
    /// The point is a signed 16.16 map pixel, so a column or a row past that
    /// range is the furthest one inside it. The view's limits then hold the
    /// camera.
    ///
    /// @param place the column or the row, in map pixels
    /// @return the place, held to a signed 16-bit map pixel
    static int32_t map_pixel(int32_t place) {
        if (place < -32768)
            return -32768;
        if (place > 32767)
            return 32767;
        return place;
    }

    /// Centres the view on a point of the map (PlayHost::center_point).
    ///
    /// @param context PlayHost::context
    /// @param x the point's column, in map pixels
    /// @param z the point's row, in map pixels
    /// @return true when the view was centred
    static bool center_point(void* context, int32_t x, int32_t z) {
        Runtime& app = runtime_of(context);
        if (!view_may_move(app))
            return false;
        app.stop_match_tracking();
        const int32_t step = static_cast<int32_t>(oa::present::scene_step_one);
        app.centre_view_on(map_pixel(x) * step, map_pixel(z) * step);
        show_new_view(app, false);
        return true;
    }

    /// Zooms about a canvas point, or the battlefield's middle (PlayHost::zoom_about).
    ///
    /// zoom_match_about holds the zoom within its limits and sets the zoom
    /// the next frame draws. A follow of a unit goes on, as a pinch keeps it.
    ///
    /// @param context PlayHost::context
    /// @param factor the zoom now over the zoom before
    /// @param x canvas column, when `focus_given` is true
    /// @param y canvas row, when `focus_given` is true
    /// @param focus_given true when `x` and `y` name the point
    /// @return true when the zoom was set
    static bool zoom_about(void* context, float factor, float x, float y, bool focus_given) {
        Runtime& app = runtime_of(context);
        if (!view_may_move(app) || !(factor > 0.0F))
            return false;
        if (!focus_given) {
            const auto& layout = app.match_layout_;
            x = static_cast<float>(layout.left) +
                static_cast<float>(layout.battlefield_width()) * 0.5F;
            y = static_cast<float>(layout.top) +
                static_cast<float>(layout.battlefield_height()) * 0.5F;
        }
        app.zoom_match_about(factor, x, y);
        show_new_view(app, true);
        return true;
    }

    /// Gives the order to each named unit now.
    ///
    /// issue_orders calls this when the stepped clock is not holding the
    /// order, and a step calls it on the tick that runs the order.
    ///
    /// A unit this machine does not play refuses the call before any order is
    /// given. A move or a patrol sends each unit that has a movement object to
    /// its own point in the group; a unit without one is sent to the point
    /// itself. A later unit the simulation refuses can leave an earlier unit
    /// of this call ordered.
    ///
    /// @param app the running game
    /// @param request the order and the units
    /// @return the tick and the queues, or why nothing was given
    static PlayCommandResult give_order(Runtime& app, const PlayOrderRequest& request) {
        if (const std::string why = command_refusal(app); !why.empty())
            return refused_result(why);
        if (request.units == nullptr || request.unit_count == 0)
            return refused_result("name at least one unit");
        std::vector<uint16_t> ids;
        if (const std::string why =
                collect_units(app, request.units, request.unit_count, false, ids);
            !why.empty())
            return refused_result(why);
        if (ids.empty())
            return refused_result("name at least one unit");
        uint16_t product = 0;
        if (const std::string why = check_request(app, request, ids, product); !why.empty())
            return refused_result(why);
        try {
            if (request.kind == PlayOrderKind::move) {
                for (uint16_t id : ids) {
                    if (!app.match_->takes_move_order(id))
                        return refused_result("the order was refused");
                }
            }
            const oa::FixedVec3 asked = fixed_of(request.point);
            const oa::sim::ground_orders::Point raw = point_of(asked);
            oa::sim::gameplay_input::GroupCentre centre{};
            const bool shaped =
                request.kind == PlayOrderKind::move || request.kind == PlayOrderKind::patrol;
            const auto& spawned = app.match_->world();
            if (shaped) {
                for (uint16_t id : ids)
                    oa::sim::gameplay_input::add_to_group_centre(centre, spawned.slots[id].record);
            }
            for (uint16_t id : ids) {
                oa::sim::ground_orders::Point destination = raw;
                if (shaped && app.match_->ground_runtime(id) != nullptr) {
                    const oa::FixedVec3 shifted = oa::sim::gameplay_input::group_order_point(
                        centre, spawned.slots[id].record, asked
                    );
                    destination = point_of(shifted);
                }
                if (const std::string why = give_one(app, id, request, destination, product);
                    !why.empty())
                    return refused_result(why);
            }
            return queues_of(app, ids);
        } catch (const std::exception&) {
            return refused_result("the order was refused");
        }
    }

    /// Sets the standing order on each named unit the order reaches, now.
    ///
    /// set_state calls this when the stepped clock is not holding it.
    ///
    /// A unit the order does not reach is left as it is. The call fails only
    /// when every named unit is left.
    ///
    /// @param app the running game
    /// @param request the standing order and the units
    /// @return the tick and the queues, or why nothing was set
    static PlayCommandResult give_state(Runtime& app, const PlayStateRequest& request) {
        if (const std::string why = command_refusal(app); !why.empty())
            return refused_result(why);
        if (request.units == nullptr || request.unit_count == 0)
            return refused_result("name at least one unit");
        std::vector<uint16_t> ids;
        if (const std::string why =
                collect_units(app, request.units, request.unit_count, false, ids);
            !why.empty())
            return refused_result(why);
        if (ids.empty())
            return refused_result("name at least one unit");
        const char* tag = "Activate";
        int32_t value = 0;
        switch (request.kind) {
        case PlayStateKind::fire:
            tag = "Standing_FireOrder";
            value = request.value;
            if (value < 0 || value > 2)
                return refused_result("the standing order needs a value");
            break;
        case PlayStateKind::move:
            tag = "Standing_MoveOrder";
            value = request.value;
            if (value < 0 || value > 2)
                return refused_result("the standing order needs a value");
            break;
        case PlayStateKind::on:
            tag = "Activate";
            break;
        case PlayStateKind::off:
            tag = "Deactivate";
            break;
        case PlayStateKind::cloak:
            tag = request.on ? "Cloak_On" : "Cloak_Off";
            break;
        }
        try {
            int taken = 0;
            for (uint16_t id : ids) {
                if (app.give_state_order(id, tag, value))
                    taken += 1;
            }
            if (taken == 0)
                return refused_result("the order does not reach that unit");
            return queues_of(app, ids);
        } catch (const std::exception&) {
            return refused_result("the order was refused");
        }
    }

    /// Selects the named units, or clears the selection (PlayHost::select_units).
    ///
    /// Every named unit is checked before the selection changes, so a unit
    /// this machine does not play, or one the player could not select, leaves
    /// the selection as it was.
    ///
    /// @param context PlayHost::context
    /// @param request the units
    /// @return the tick and the units selected afterwards, or why nothing changed
    static PlayCommandResult select_units(void* context, const PlaySelectRequest& request) {
        Runtime& app = runtime_of(context);
        if (const std::string why = command_refusal(app); !why.empty())
            return refused_result(why);
        std::vector<uint16_t> ids;
        if (request.unit_count != 0) {
            if (const std::string why =
                    collect_units(app, request.units, request.unit_count, true, ids);
                !why.empty())
                return refused_result(why);
        }
        try {
            oa::World& block = app.match_->state();
            block.game.local_player_index = app.match_local_player_;
            if (!request.add)
                app.clear_local_selection();
            for (uint16_t id : ids)
                app.adopt_selection(id);
            app.apply_match_hud_for_selection();
            PlayCommandResult result;
            result.ok = true;
            result.tick = block.game.tick;
            result.selected = chosen_units(app);
            return result;
        } catch (const std::exception&) {
            return refused_result("the order was refused");
        }
    }

    /// Tells whether a stepped clock is holding orders until a step runs them.
    ///
    /// A paused match is given its order at once: a step cannot run while the
    /// pause is on. The wall clock is given its order at once too.
    ///
    /// @param app the running game
    /// @return true when the order waits for a step
    static bool orders_wait(const Runtime& app) {
        return app.stepped_clock_holds() && !pause_held(app);
    }

    /// A result that says the stepped clock is holding the order.
    ///
    /// @return pending, with nothing given
    static PlayCommandResult pending_result() {
        PlayCommandResult result;
        result.pending = true;
        return result;
    }

    /// Checks an order without giving it. The same checks give_order makes.
    ///
    /// @param app the running game
    /// @param request the order
    /// @return the sentence, or empty when it may be given
    static std::string ready_order(Runtime& app, const PlayOrderRequest& request) {
        if (const std::string why = command_refusal(app); !why.empty())
            return why;
        if (request.units == nullptr || request.unit_count == 0)
            return "name at least one unit";
        std::vector<uint16_t> ids;
        if (const std::string why =
                collect_units(app, request.units, request.unit_count, false, ids);
            !why.empty())
            return why;
        if (ids.empty())
            return "name at least one unit";
        uint16_t product = 0;
        if (const std::string why = check_request(app, request, ids, product); !why.empty())
            return why;
        if (request.kind == PlayOrderKind::move) {
            try {
                for (uint16_t id : ids) {
                    if (!app.match_->takes_move_order(id))
                        return "the order was refused";
                }
            } catch (const std::exception&) {
                return "the order was refused";
            }
        }
        return {};
    }

    /// Checks a standing order without setting it.
    ///
    /// @param app the running game
    /// @param request the standing order
    /// @return the sentence, or empty when it may be set
    static std::string ready_state(Runtime& app, const PlayStateRequest& request) {
        if (const std::string why = command_refusal(app); !why.empty())
            return why;
        if (request.units == nullptr || request.unit_count == 0)
            return "name at least one unit";
        std::vector<uint16_t> ids;
        if (const std::string why =
                collect_units(app, request.units, request.unit_count, false, ids);
            !why.empty())
            return why;
        if (ids.empty())
            return "name at least one unit";
        if (request.kind == PlayStateKind::fire || request.kind == PlayStateKind::move) {
            if (request.value < 0 || request.value > 2)
                return "the standing order needs a value";
        }
        return {};
    }

    /// Copies an order into the runtime's bag. key stays the call's unit list.
    ///
    /// @param bag the runtime's bag
    /// @param request the order
    static void remember_order(HeldBag& bag, const PlayOrderRequest& request) {
        HeldCommand armed;
        armed.key = request.units;
        armed.order_kind = request.kind;
        armed.append = request.append;
        armed.units.assign(request.units, request.units + request.unit_count);
        armed.target = request.target;
        armed.has_point = request.has_point;
        armed.point = request.point;
        if (request.type_name != nullptr)
            armed.type_name = request.type_name;
        armed.count = request.count;
        armed.facing = request.facing;
        bag.commands.push_back(std::move(armed));
    }

    /// Copies a standing order into the runtime's bag.
    ///
    /// @param bag the runtime's bag
    /// @param request the standing order
    static void remember_state(HeldBag& bag, const PlayStateRequest& request) {
        HeldCommand armed;
        armed.key = request.units;
        armed.standing = true;
        armed.state_kind = request.kind;
        armed.state_value = request.value;
        armed.state_on = request.on;
        armed.units.assign(request.units, request.units + request.unit_count);
        bag.commands.push_back(std::move(armed));
    }

    /// Gives the order, or holds it until a step runs it (PlayHost::issue_orders).
    ///
    /// The first call checks the order. A refusal is returned at once and
    /// nothing is held. A stepped clock that is running copies the order and
    /// returns pending. The step gives that copy and stores the result. The
    /// next call with the same unit list returns the result and drops it.
    /// While the clock still holds it, that next call stays pending and does
    /// not give the order twice.
    ///
    /// @param context PlayHost::context
    /// @param request the order and the units
    /// @return the tick and the queues, pending, or why nothing was given
    static PlayCommandResult issue_orders(void* context, const PlayOrderRequest& request) {
        Runtime& app = runtime_of(context);
        HeldBag& bag = bag_of(app);
        if (HeldCommand* held = find_held(bag, request.units, false)) {
            if (held->landed) {
                PlayCommandResult result = std::move(held->result);
                erase_held(bag, request.units, false);
                return result;
            }
            if (orders_wait(app))
                return pending_result();
            const PlayOrderRequest stored = order_view(*held);
            PlayCommandResult result = give_order(app, stored);
            erase_held(bag, request.units, false);
            return result;
        }
        if (const std::string why = ready_order(app, request); !why.empty())
            return refused_result(why);
        if (orders_wait(app)) {
            remember_order(bag, request);
            return pending_result();
        }
        return give_order(app, request);
    }

    /// Sets the standing order, or holds it until a step (PlayHost::set_state).
    ///
    /// The same hold as issue_orders. A unit the order does not reach is left,
    /// and the call fails only when every named unit is left, when the step
    /// gives it or when it is given at once.
    ///
    /// @param context PlayHost::context
    /// @param request the standing order and the units
    /// @return the tick and the queues, pending, or why nothing was set
    static PlayCommandResult set_state(void* context, const PlayStateRequest& request) {
        Runtime& app = runtime_of(context);
        HeldBag& bag = bag_of(app);
        if (HeldCommand* held = find_held(bag, request.units, true)) {
            if (held->landed) {
                PlayCommandResult result = std::move(held->result);
                erase_held(bag, request.units, true);
                return result;
            }
            if (orders_wait(app))
                return pending_result();
            const PlayStateRequest stored = state_view(*held);
            PlayCommandResult result = give_state(app, stored);
            erase_held(bag, request.units, true);
            return result;
        }
        if (const std::string why = ready_state(app, request); !why.empty())
            return refused_result(why);
        if (orders_wait(app)) {
            remember_state(bag, request);
            return pending_result();
        }
        return give_state(app, request);
    }

    /// Tells whether this match may leave the wall clock.
    ///
    /// A shared match keeps the wall clock, so the other machines stay in
    /// step. A replay and a watcher are already refused by changes_allowed.
    ///
    /// @param app the running game
    /// @return true when the clock may be stepped
    static bool clock_allowed(const Runtime& app) {
        if (!changes_allowed(app))
            return false;
        const uint32_t bits = app.current_extension_state();
        return (bits & extension_state::shared_match) == 0;
    }

    /// Sets the clock mode (PlayHost::set_clock_mode).
    ///
    /// @param context PlayHost::context
    /// @param mode 0 for the wall clock, 1 for the stepped clock
    /// @return true when the clock is in that mode
    static bool set_clock_mode(void* context, uint8_t mode) {
        Runtime& app = runtime_of(context);
        if (!clock_allowed(app) || mode > 1)
            return false;
        app.stepped_clock_ = mode == 1;
        app.snap_held_clock();
        return true;
    }

    /// Runs a granted batch of ticks (PlayHost::run_ticks).
    ///
    /// @param context PlayHost::context
    /// @param count how many ticks this call may run
    /// @param until_tick the tick to stop at, or clock_no_target
    /// @param until_event unused; the run stops when the outcome leaves ongoing either way
    /// @return how far the run got
    static ClockRun
    run_ticks(void* context, uint32_t count, uint32_t until_tick, uint8_t until_event) {
        (void)until_event;
        Runtime& app = runtime_of(context);
        ClockRun run;
        run.tick = app.match_ != nullptr ? app.match_->state().game.tick : 0;
        if (!app.stepped_clock_ || !clock_allowed(app)) {
            run.stop = ClockStop::refused;
            return run;
        }
        if (app.match_finished_) {
            run.stop = ClockStop::finished;
            return run;
        }
        if (!app.match_clock_steps() || pause_held(app)) {
            run.stop = ClockStop::paused;
            return run;
        }
        if (until_tick != clock_no_target && run.tick >= until_tick) {
            run.stop = ClockStop::tick;
            return run;
        }
        uint32_t limit = count;
        if (until_tick != clock_no_target) {
            const uint32_t room = until_tick - run.tick;
            if (room < limit)
                limit = room;
        }
        const Runtime::GrantedTicks grant = app.run_granted_ticks(limit);
        run.ran = grant.ran;
        run.game_over = grant.game_over;
        if (app.match_ != nullptr)
            run.tick = app.match_->state().game.tick;
        if (grant.game_over) {
            run.stop = ClockStop::event;
            return run;
        }
        if (until_tick != clock_no_target && run.tick >= until_tick) {
            run.stop = ClockStop::tick;
            return run;
        }
        if (grant.ran < limit) {
            if (app.match_finished_)
                run.stop = ClockStop::finished;
            else if (!app.match_clock_steps() || pause_held(app))
                run.stop = ClockStop::paused;
            else
                run.stop = ClockStop::ran;
            return run;
        }
        run.stop = ClockStop::ran;
        return run;
    }

    /// Offers the command line's clock once per match (PlayHost::offer_startup_clock).
    ///
    /// @param context PlayHost::context
    /// @param mode 1 to start stepped, 0 to start on the wall clock
    static void offer_startup_clock(void* context, uint8_t mode) {
        runtime_of(context).offer_stepped_clock(mode);
    }
};

namespace {

// Every entry of the play host; play_host() binds a copy to a runtime.
constexpr PlayHost kPlayHostEntries{
    nullptr,
    PlayHostAccess::set_speed,
    PlayHostAccess::pause,
    PlayHostAccess::resume,
    PlayHostAccess::read_state,
    PlayHostAccess::read_view,
    PlayHostAccess::set_camera,
    PlayHostAccess::center_unit,
    PlayHostAccess::center_point,
    PlayHostAccess::zoom_about,
    PlayHostAccess::set_clock_mode,
    PlayHostAccess::run_ticks,
    PlayHostAccess::offer_startup_clock,
    PlayHostAccess::issue_orders,
    PlayHostAccess::set_state,
    PlayHostAccess::select_units,
};

// The entries above, one for each of the table's after its context.
constexpr std::size_t kPlayHostEntryCount = 15;
static_assert(
    sizeof(PlayHost) == sizeof(void*) * (1 + kPlayHostEntryCount),
    "the play host's entries changed: set every one of them here"
);

} // namespace

void Runtime::give_held_play_orders() {
    HeldBag* bag = nullptr;
    for (HeldBag& one : held_bags) {
        if (one.runtime == this)
            bag = &one;
    }
    if (bag == nullptr)
        return;
    for (HeldCommand& command : bag->commands) {
        if (command.landed)
            continue;
        if (command.standing)
            command.result = PlayHostAccess::give_state(*this, state_view(command));
        else
            command.result = PlayHostAccess::give_order(*this, order_view(command));
        command.landed = true;
    }
}

void Runtime::drop_held_play_orders() {
    std::vector<HeldBag> kept;
    kept.reserve(held_bags.size());
    for (HeldBag& bag : held_bags) {
        if (bag.runtime != this)
            kept.push_back(std::move(bag));
    }
    held_bags = std::move(kept);
}

PlayHost play_host(Runtime& runtime) {
    PlayHost host = kPlayHostEntries;
    host.context = &runtime;
    return host;
}

} // namespace oa::app
