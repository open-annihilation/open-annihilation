// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// game.speed, game.pause, the camera tools, the clock tools, orders and
// selection with no game: a replay, a watcher and a shared match's clock
// are refused before anything is queued, and a replay and a watcher may
// still point the view. A change that is allowed is applied only when the
// queue is applied, which the frame does at after_pump. The answer names
// the tick the change landed on. An order for a unit this machine does not
// play is refused when the queue is applied, and nothing is logged for it.
// A place past the view's limits is reported where those limits hold it.
// camera.get reads, and is not queued. A stepped clock holds an order until
// a step runs it.
#include "oa/app/mcp/actions.hpp"
#include "oa/formats/json.hpp"
#include "oa/test/check.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace json = oa::formats::json;
namespace mcp = oa::app::mcp;

/// An order the fake stepped clock is holding until a step runs it.
struct ArmedCall {
    const void* key{}; ///< the call's unit list
    bool standing{};   ///< a standing order
    bool landed{};     ///< a step has given it
    uint32_t tick{};   ///< the tick it was given on
    uint16_t unit{};   ///< the unit the queue names
    std::string kind;  ///< the mission the queue names
};

/// The match a fake host reports, and how many times its entries were called.
struct World {
    bool in_match{true};
    bool replay{};
    bool watcher{};
    bool shared_match{};
    bool paused{};
    bool finished{};
    bool refuse_speed{};
    int32_t speed{10};
    int32_t slowest{1};
    int32_t fastest{20};
    uint32_t tick{40};
    int32_t speed_calls{};
    int32_t pause_calls{};
    int32_t resume_calls{};
    int32_t command_calls{};
    int32_t issued{};
    uint16_t own_unit{4};
    std::vector<uint16_t> selected{};
    bool director{};
    int32_t camera_x{10};
    int32_t camera_z{20};
    float zoom{1.0F};
    float least_zoom{1.0F};
    float most_zoom{4.0F};
    int32_t limit_x{400};
    int32_t limit_z{300};
    uint16_t unit{3};
    int32_t set_calls{};
    int32_t center_unit_calls{};
    int32_t center_point_calls{};
    int32_t zoom_calls{};
    uint16_t last_unit{};
    bool stepped{};
    /// Ticks until the match ends. 0 does not end it.
    uint32_t end_after{};
    int32_t mode_calls{};
    int32_t run_calls{};
    /// Orders waiting for a step, or the result of one a step has given.
    std::vector<ArmedCall> armed{};
};

/// Parses one JSON text. A failure is a failed check and an empty result.
///
/// @param text the text
/// @return the value
std::optional<json::Json> parsed(std::string_view text) {
    json::JsonError failure;
    std::optional<json::Json> value = json::parse_json(text, failure);
    OA_CHECK(value.has_value());
    return value;
}

/// One request.
///
/// @param id the id, as JSON
/// @param method the method
/// @param params the params object, as JSON; empty omits params
/// @return the request
std::string request(std::string_view id, std::string_view method, std::string_view params = {}) {
    std::string text = "{\"jsonrpc\":\"2.0\",\"id\":";
    text.append(id);
    text.append(",\"method\":\"");
    text.append(method);
    text.push_back('"');
    if (!params.empty()) {
        text.append(",\"params\":");
        text.append(params);
    }
    text.push_back('}');
    return text;
}

/// One tools/call.
///
/// @param id the id, as JSON
/// @param name the tool
/// @param arguments the arguments object, as JSON; empty omits arguments
/// @return the request
std::string call_tool(std::string_view id, std::string_view name, std::string_view arguments = {}) {
    std::string params = "{\"name\":\"";
    params.append(name);
    params.push_back('"');
    if (!arguments.empty()) {
        params.append(",\"arguments\":");
        params.append(arguments);
    }
    params.push_back('}');
    return request(id, "tools/call", params);
}

/// The one answer a message produced, parsed.
///
/// @param session the session
/// @param message the message
/// @return the answer, or nothing when it was not one JSON object
std::optional<json::Json> only(mcp::Session& session, std::string_view message) {
    const std::vector<std::string> lines = session.receive(message);
    OA_CHECK(lines.size() == 1);
    if (lines.size() != 1)
        return std::nullopt;
    return parsed(lines[0]);
}

/// Checks an error answer's code and message.
///
/// @param answer the answer
/// @param code the code
/// @param message the message
void check_error(const json::Json& answer, int64_t code, std::string_view message) {
    const json::Json* error = answer.find("error");
    OA_CHECK(error != nullptr);
    if (error == nullptr)
        return;
    const json::Json* got = error->find("code");
    OA_CHECK(got != nullptr && got->integer() && *got->integer() == code);
    const json::Json* why = error->find("message");
    OA_CHECK(why != nullptr && why->string() && *why->string() == message);
}

/// The text of a tool result.
///
/// @param answer the answer
/// @return the text, or empty when the answer has none
std::string tool_text(const json::Json& answer) {
    const json::Json* result = answer.find("result");
    if (result == nullptr)
        return {};
    const json::Json* content = result->find("content");
    if (content == nullptr || content->elements().empty())
        return {};
    const json::Json* text = content->elements().front().find("text");
    if (text == nullptr || text->string() == nullptr)
        return {};
    return *text->string();
}

/// Tells whether a tool result is a failure.
///
/// @param answer the answer
/// @return true when isError is true
bool tool_failed(const json::Json& answer) {
    const json::Json* result = answer.find("result");
    if (result == nullptr)
        return false;
    const json::Json* flag = result->find("isError");
    return flag != nullptr && flag->boolean() && *flag->boolean();
}

/// The fake host's reading of the match.
///
/// @param context the World
/// @return the state
oa::app::PlayExtensionState read_state(void* context) {
    const auto* world = static_cast<const World*>(context);
    oa::app::PlayExtensionState state;
    if (world == nullptr)
        return state;
    state.in_match = world->in_match;
    state.replay = world->replay;
    state.watcher = world->watcher;
    state.shared_match = world->shared_match;
    state.paused = world->paused;
    state.finished = world->finished;
    state.director = world->director;
    state.stepped = world->stepped;
    state.speed = world->speed;
    state.slowest = world->slowest;
    state.fastest = world->fastest;
    state.tick = world->tick;
    return state;
}

/// Sets the fake match's speed, counting the call.
///
/// @param context the World
/// @param speed the absolute speed
/// @return false when the match refuses it
bool fake_set_speed(void* context, uint16_t speed) {
    auto* world = static_cast<World*>(context);
    if (world == nullptr)
        return false;
    world->speed_calls += 1;
    if (!world->in_match || world->replay || world->watcher || world->refuse_speed)
        return false;
    if (static_cast<int32_t>(speed) < world->slowest ||
        static_cast<int32_t>(speed) > world->fastest)
        return false;
    world->speed = static_cast<int32_t>(speed);
    return true;
}

/// Pauses the fake match, counting the call.
///
/// @param context the World
/// @return false when the match refuses it
bool fake_pause(void* context) {
    auto* world = static_cast<World*>(context);
    if (world == nullptr)
        return false;
    world->pause_calls += 1;
    if (!world->in_match || world->replay || world->watcher || world->finished)
        return false;
    world->paused = true;
    return true;
}

/// Resumes the fake match, counting the call.
///
/// @param context the World
/// @return false when the match refuses it
bool fake_resume(void* context) {
    auto* world = static_cast<World*>(context);
    if (world == nullptr)
        return false;
    world->resume_calls += 1;
    if (!world->in_match || world->replay || world->watcher || world->finished)
        return false;
    world->paused = false;
    return true;
}

/// Holds a map pixel to the fake view's limits, which stand in for the real ones.
///
/// @param asked the place that was asked for
/// @param limit the furthest place the view holds
/// @return the place the view holds
int32_t held_place(int32_t asked, int32_t limit) {
    if (asked < 0)
        return 0;
    if (asked > limit)
        return limit;
    return asked;
}

/// Holds a zoom to the fake view's limits.
///
/// @param zoom the zoom that was asked for
/// @param least the least zoom
/// @param most the most zoom
/// @return the zoom the view holds
float held_zoom(float zoom, float least, float most) {
    if (zoom < least)
        return least;
    if (zoom > most)
        return most;
    return zoom;
}

/// Reads the fake view.
///
/// @param context the World
/// @param shown the view
/// @return false when there is nothing to fill
bool fake_read_view(void* context, oa::app::PlayView* shown) {
    const auto* world = static_cast<const World*>(context);
    if (world == nullptr || shown == nullptr)
        return false;
    shown->in_match = world->in_match;
    shown->x = world->camera_x;
    shown->z = world->camera_z;
    shown->zoom = world->zoom;
    return true;
}

/// Puts the fake view's corner at a map pixel, held within its limits.
///
/// @param context the World
/// @param x the column
/// @param z the row
/// @return false when the view may not move
bool fake_set_camera(void* context, int32_t x, int32_t z) {
    auto* world = static_cast<World*>(context);
    if (world == nullptr)
        return false;
    world->set_calls += 1;
    if (!world->in_match || world->director)
        return false;
    world->camera_x = held_place(x, world->limit_x);
    world->camera_z = held_place(z, world->limit_z);
    return true;
}

/// Centres the fake view on its one unit, by putting the corner at that unit's place.
///
/// @param context the World
/// @param unit the unit's id
/// @return false when that unit is not the world's
bool fake_center_unit(void* context, uint16_t unit) {
    auto* world = static_cast<World*>(context);
    if (world == nullptr)
        return false;
    world->center_unit_calls += 1;
    world->last_unit = unit;
    if (!world->in_match || world->director || unit != world->unit)
        return false;
    world->camera_x = world->limit_x / 2;
    world->camera_z = world->limit_z / 2;
    return true;
}

/// Centres the fake view on a point by holding that point within the view's limits.
///
/// @param context the World
/// @param x the point's column
/// @param z the point's row
/// @return false when the view may not move
bool fake_center_point(void* context, int32_t x, int32_t z) {
    auto* world = static_cast<World*>(context);
    if (world == nullptr)
        return false;
    world->center_point_calls += 1;
    if (!world->in_match || world->director)
        return false;
    world->camera_x = held_place(x, world->limit_x);
    world->camera_z = held_place(z, world->limit_z);
    return true;
}

/// Multiplies the fake view's zoom, or sets it when the factor was made from an absolute zoom.
///
/// The tool turns an absolute zoom into a factor over the zoom now, so this
/// stores the product and then holds it within the zoom's limits.
///
/// @param context the World
/// @param factor the multiplier
/// @return false when the view may not zoom
bool fake_zoom(void* context, float factor, float /*x*/, float /*y*/, bool /*focus_given*/) {
    auto* world = static_cast<World*>(context);
    if (world == nullptr)
        return false;
    world->zoom_calls += 1;
    if (!world->in_match || world->director || !(factor > 0.0F))
        return false;
    const float current = world->zoom > 0.0F ? world->zoom : 1.0F;
    world->zoom = held_zoom(current * factor, world->least_zoom, world->most_zoom);
    return true;
}

/// The held order stored under this call's unit list.
///
/// @param world the fake match
/// @param key the call's unit list
/// @param standing true for a standing order
/// @return the order, or null
ArmedCall* find_armed(World& world, const void* key, bool standing) {
    if (key == nullptr)
        return nullptr;
    for (ArmedCall& one : world.armed) {
        if (one.key == key && one.standing == standing)
            return &one;
    }
    return nullptr;
}

/// Drops the order stored under this call's unit list.
///
/// @param world the fake match
/// @param key the call's unit list
/// @param standing true for a standing order
void erase_armed(World& world, const void* key, bool standing) {
    std::vector<ArmedCall> kept;
    kept.reserve(world.armed.size());
    for (ArmedCall& one : world.armed) {
        if (one.key == key && one.standing == standing)
            continue;
        kept.push_back(std::move(one));
    }
    world.armed = std::move(kept);
}

/// Gives every order the clock is holding, on the tick the step has reached.
///
/// @param world the fake match
void land_armed(World& world) {
    for (ArmedCall& one : world.armed) {
        if (one.landed)
            continue;
        one.landed = true;
        one.tick = world.tick;
        world.issued += 1;
    }
}

/// The answer a landed order returns. The queue names the mission and the tick.
///
/// @param world the fake match
/// @param one the landed order
/// @return the tick and the queue
oa::app::PlayCommandResult armed_result(const World& world, const ArmedCall& one) {
    oa::app::PlayCommandResult result;
    result.ok = true;
    result.tick = one.tick;
    oa::app::PlayUnitOrders queues;
    queues.id = one.unit == 0 ? world.own_unit : one.unit;
    oa::app::PlayOrderLine line;
    line.kind = one.kind;
    line.issue_tick = one.tick;
    queues.primary.push_back(std::move(line));
    result.queues.push_back(std::move(queues));
    return result;
}

/// Gives the fake unit an order, counting the call. Unit 4 is this machine's.
///
/// A stepped clock that is running holds the order: the first call checks it
/// and gives nothing, and the step gives it. A second call returns that result
/// without counting again. An enemy unit is refused at once and is not held.
///
/// @param context the World
/// @param request the order
/// @return the tick and a queue, pending, or why nothing was given
oa::app::PlayCommandResult fake_issue(void* context, const oa::app::PlayOrderRequest& request) {
    oa::app::PlayCommandResult result;
    auto* world = static_cast<World*>(context);
    if (world == nullptr) {
        result.detail = "no match is running";
        return result;
    }
    if (ArmedCall* held = find_armed(*world, request.units, false)) {
        if (held->landed) {
            result = armed_result(*world, *held);
            erase_armed(*world, request.units, false);
            return result;
        }
        if (world->stepped && !world->paused) {
            result.pending = true;
            return result;
        }
        world->issued += 1;
        held->landed = true;
        held->tick = world->tick;
        result = armed_result(*world, *held);
        erase_armed(*world, request.units, false);
        return result;
    }
    world->command_calls += 1;
    if (request.units == nullptr || request.unit_count == 0) {
        result.detail = "name at least one unit";
        return result;
    }
    for (std::size_t place = 0; place < request.unit_count; ++place) {
        if (request.units[place] != world->own_unit) {
            result.detail = "that unit is not this machine's";
            return result;
        }
    }
    if (world->stepped && !world->paused) {
        ArmedCall armed_call;
        armed_call.key = request.units;
        armed_call.unit = world->own_unit;
        armed_call.kind = "Move_Ground";
        world->armed.push_back(std::move(armed_call));
        result.pending = true;
        return result;
    }
    world->issued += 1;
    result.ok = true;
    result.tick = world->tick;
    oa::app::PlayUnitOrders queues;
    queues.id = world->own_unit;
    oa::app::PlayOrderLine line;
    line.kind = "Move_Ground";
    line.issue_tick = world->tick;
    queues.primary.push_back(std::move(line));
    result.queues.push_back(std::move(queues));
    return result;
}

/// Sets a standing order on the fake unit, counting the call.
///
/// Held the same way as an order while a stepped clock is running.
///
/// @param context the World
/// @param request the standing order
/// @return the tick and a queue, pending, or why nothing was set
oa::app::PlayCommandResult fake_state(void* context, const oa::app::PlayStateRequest& request) {
    oa::app::PlayCommandResult result;
    auto* world = static_cast<World*>(context);
    if (world == nullptr) {
        result.detail = "no match is running";
        return result;
    }
    if (ArmedCall* held = find_armed(*world, request.units, true)) {
        if (held->landed) {
            result = armed_result(*world, *held);
            erase_armed(*world, request.units, true);
            return result;
        }
        if (world->stepped && !world->paused) {
            result.pending = true;
            return result;
        }
        world->issued += 1;
        held->landed = true;
        held->tick = world->tick;
        result = armed_result(*world, *held);
        erase_armed(*world, request.units, true);
        return result;
    }
    world->command_calls += 1;
    if (request.units == nullptr || request.unit_count == 0) {
        result.detail = "name at least one unit";
        return result;
    }
    for (std::size_t place = 0; place < request.unit_count; ++place) {
        if (request.units[place] != world->own_unit) {
            result.detail = "that unit is not this machine's";
            return result;
        }
    }
    if (world->stepped && !world->paused) {
        ArmedCall armed_call;
        armed_call.key = request.units;
        armed_call.standing = true;
        armed_call.unit = world->own_unit;
        armed_call.kind = "Standing_FireOrder";
        world->armed.push_back(std::move(armed_call));
        result.pending = true;
        return result;
    }
    world->issued += 1;
    result.ok = true;
    result.tick = world->tick;
    oa::app::PlayUnitOrders queues;
    queues.id = world->own_unit;
    oa::app::PlayOrderLine line;
    line.kind = "Standing_FireOrder";
    line.issue_tick = world->tick;
    queues.primary.push_back(std::move(line));
    result.queues.push_back(std::move(queues));
    return result;
}

/// Selects the fake unit, counting the call. An enemy unit changes nothing.
///
/// A selection is made at once, including while the clock is stepped.
///
/// @param context the World
/// @param request the units
/// @return the tick and the selection, or why nothing changed
oa::app::PlayCommandResult fake_select(void* context, const oa::app::PlaySelectRequest& request) {
    oa::app::PlayCommandResult result;
    auto* world = static_cast<World*>(context);
    if (world == nullptr) {
        result.detail = "no match is running";
        return result;
    }
    world->command_calls += 1;
    if (request.unit_count != 0) {
        if (request.units == nullptr) {
            result.detail = "no such unit";
            return result;
        }
        for (std::size_t place = 0; place < request.unit_count; ++place) {
            if (request.units[place] != world->own_unit) {
                result.detail = "that unit is not this machine's";
                return result;
            }
        }
    }
    if (!request.add)
        world->selected.clear();
    if (request.unit_count != 0) {
        bool already = false;
        for (uint16_t id : world->selected) {
            if (id == world->own_unit)
                already = true;
        }
        if (!already)
            world->selected.push_back(world->own_unit);
    }
    world->issued += 1;
    result.ok = true;
    result.tick = world->tick;
    result.selected = world->selected;
    return result;
}

/// Sets the fake clock, counting the call.
///
/// @param context the World
/// @param mode 0 for the wall clock, 1 for stepped
/// @return false when the match refuses it
bool fake_set_clock_mode(void* context, uint8_t mode) {
    auto* world = static_cast<World*>(context);
    if (world == nullptr)
        return false;
    world->mode_calls += 1;
    if (!world->in_match || world->replay || world->watcher || world->shared_match || mode > 1)
        return false;
    world->stepped = mode == 1;
    return true;
}

/// Runs the fake clock, counting the call. The whole count runs in this call.
///
/// The first tick of the run gives the orders the clock was holding, and the
/// rest of the count only moves the tick. No held order leaves the tick where
/// the clock tests left it.
///
/// @param context the World
/// @param count how many ticks this call may run
/// @param until_tick the tick to stop at, or clock_no_target
/// @param until_event unused; the run stops when end_after says the match ends
/// @return how far the run got
oa::app::ClockRun
fake_run_ticks(void* context, uint32_t count, uint32_t until_tick, uint8_t until_event) {
    (void)until_event;
    auto* world = static_cast<World*>(context);
    oa::app::ClockRun run;
    if (world == nullptr)
        return run;
    world->run_calls += 1;
    run.tick = world->tick;
    if (!world->in_match || world->replay || world->watcher || world->shared_match ||
        !world->stepped) {
        run.stop = oa::app::ClockStop::refused;
        return run;
    }
    if (world->finished) {
        run.stop = oa::app::ClockStop::finished;
        return run;
    }
    if (world->paused) {
        run.stop = oa::app::ClockStop::paused;
        return run;
    }
    if (until_tick != oa::app::clock_no_target && world->tick >= until_tick) {
        run.stop = oa::app::ClockStop::tick;
        return run;
    }
    uint32_t steps = count;
    if (until_tick != oa::app::clock_no_target) {
        const uint32_t room = until_tick - world->tick;
        if (room < steps)
            steps = room;
    }
    bool over = false;
    if (world->end_after > 0 && world->end_after <= steps) {
        steps = world->end_after;
        over = true;
        world->end_after = 0;
        world->finished = true;
    } else if (world->end_after > 0) {
        world->end_after -= steps;
    }
    if (steps > 0) {
        world->tick += 1;
        land_armed(*world);
        world->tick += steps - 1;
    }
    run.ran = steps;
    run.tick = world->tick;
    run.game_over = over;
    if (over)
        run.stop = oa::app::ClockStop::event;
    else if (until_tick != oa::app::clock_no_target && world->tick >= until_tick)
        run.stop = oa::app::ClockStop::tick;
    else
        run.stop = oa::app::ClockStop::ran;
    return run;
}

/// The fake host bound to a world.
///
/// @param context the World
/// @return the host
oa::app::PlayHost host_from_world(void* context) {
    oa::app::PlayHost host;
    host.context = context;
    host.set_speed = fake_set_speed;
    host.pause = fake_pause;
    host.resume = fake_resume;
    host.extension_state = read_state;
    host.read_view = fake_read_view;
    host.set_camera = fake_set_camera;
    host.center_unit = fake_center_unit;
    host.center_point = fake_center_point;
    host.zoom_about = fake_zoom;
    host.set_clock_mode = fake_set_clock_mode;
    host.run_ticks = fake_run_ticks;
    host.issue_orders = fake_issue;
    host.set_state = fake_state;
    host.select_units = fake_select;
    return host;
}

/// A session whose act tools use a fake host. The world outlives the session.
struct Held {
    World world{};
    mcp::PlayBridge bridge{};
    std::optional<mcp::Session> session{};

    /// Opens the session and offers the act tools.
    ///
    /// @param observe true when only tools that read are offered
    void open(bool observe) {
        bridge.host_of = host_from_world;
        bridge.context = &world;
        mcp::SessionConfig config;
        config.observe = observe;
        session.emplace(config);
        mcp::add_play_tools(*session, bridge);
        (void)session->receive(request("1", "initialize", "{\"protocolVersion\":\"2025-06-18\"}"));
        (void)session->receive("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    }
};

/// Checks that a call is refused at once, and that the host was not called.
///
/// @param held the session
/// @param call the tools/call
/// @param sentence the failure
void expect_refused(Held& held, std::string_view call, std::string_view sentence) {
    const int speed_calls = held.world.speed_calls;
    const int pause_calls = held.world.pause_calls;
    const int resume_calls = held.world.resume_calls;
    const int command_calls = held.world.command_calls;
    const int issued = held.world.issued;
    const int mode_calls = held.world.mode_calls;
    const int run_calls = held.world.run_calls;
    if (const std::optional<json::Json> answer = only(*held.session, call)) {
        OA_CHECK(tool_failed(*answer));
        OA_CHECK(tool_text(*answer) == sentence);
    }
    OA_CHECK(held.world.speed_calls == speed_calls);
    OA_CHECK(held.world.pause_calls == pause_calls);
    OA_CHECK(held.world.resume_calls == resume_calls);
    OA_CHECK(held.world.command_calls == command_calls);
    OA_CHECK(held.world.issued == issued);
    OA_CHECK(held.world.mode_calls == mode_calls);
    OA_CHECK(held.world.run_calls == run_calls);
    OA_CHECK(!held.bridge.queue.waiting());
}

/// Receives a call, leaves it held across the same pump, applies the queue, and returns the result.
///
/// The same pump is the one the frame's pump runs after it has read the
/// call. The host's entries stay uncalled until apply, which the frame runs
/// at after_pump. The result is the next pump's.
///
/// @param held the session
/// @param call the tools/call
/// @return the result, or nothing when it did not arrive
std::optional<json::Json> land(Held& held, std::string_view call) {
    const std::vector<std::string> queued = held.session->receive(call);
    OA_CHECK(queued.empty());
    const int speed_calls = held.world.speed_calls;
    const int pause_calls = held.world.pause_calls;
    const int resume_calls = held.world.resume_calls;
    const int command_calls = held.world.command_calls;
    const int issued = held.world.issued;
    const int mode_calls = held.world.mode_calls;
    const int run_calls = held.world.run_calls;
    const std::vector<std::string> waiting = held.session->pump();
    OA_CHECK(waiting.empty());
    OA_CHECK(held.world.speed_calls == speed_calls);
    OA_CHECK(held.world.pause_calls == pause_calls);
    OA_CHECK(held.world.resume_calls == resume_calls);
    OA_CHECK(held.world.command_calls == command_calls);
    OA_CHECK(held.world.issued == issued);
    OA_CHECK(held.world.mode_calls == mode_calls);
    OA_CHECK(held.world.run_calls == run_calls);
    OA_CHECK(held.bridge.queue.waiting());
    const std::vector<mcp::AppliedAct> acts =
        held.bridge.queue.apply(held.bridge.host_of(held.bridge.context));
    OA_CHECK(!held.bridge.queue.waiting());
    const std::vector<std::string> done = held.session->pump();
    OA_CHECK(done.size() == 1);
    if (done.size() != 1)
        return std::nullopt;
    std::optional<json::Json> answer = parsed(done[0]);
    if (!answer)
        return std::nullopt;
    // A change the host did not make has no tick. One it made names that tick.
    if (tool_failed(*answer)) {
        OA_CHECK(acts.empty());
        return answer;
    }
    OA_CHECK(acts.size() == 1);
    if (acts.size() == 1) {
        const std::optional<json::Json> text = parsed(tool_text(*answer));
        if (text) {
            const json::Json* tick = text->find("applied_tick");
            if (tick == nullptr)
                tick = text->find("tick");
            OA_CHECK(
                tick != nullptr && tick->integer() &&
                *tick->integer() == static_cast<int64_t>(acts[0].tick)
            );
        }
    }
    return answer;
}

/// A replay cannot change the speed or the pause.
void replay_refuses_speed_and_pause() {
    Held held;
    held.world.replay = true;
    held.open(false);
    expect_refused(
        held, call_tool("2", "game.speed", "{\"speed\":4}"), "a replay cannot change the speed"
    );
    expect_refused(
        held, call_tool("3", "game.pause", "{\"paused\":true}"), "a replay cannot pause the match"
    );
    expect_refused(
        held, call_tool("4", "game.pause", "{\"paused\":false}"), "a replay cannot pause the match"
    );
}

/// A watcher cannot change the speed or the pause.
void watcher_refuses_speed_and_pause() {
    Held held;
    held.world.watcher = true;
    held.open(false);
    expect_refused(
        held, call_tool("2", "game.speed", "{\"speed\":4}"), "a watcher cannot change the speed"
    );
    expect_refused(
        held, call_tool("3", "game.pause", "{\"paused\":true}"), "a watcher cannot pause the match"
    );
}

/// No running match refuses both tools.
void no_match_refuses() {
    Held held;
    held.world.in_match = false;
    held.open(false);
    expect_refused(held, call_tool("2", "game.speed", "{\"speed\":4}"), "no match is running");
    expect_refused(held, call_tool("3", "game.pause", "{\"paused\":true}"), "no match is running");
}

/// A speed the match does not allow is refused before the host is called. 21 is
/// inside no schema maximum.
void speed_outside_the_range_is_refused() {
    Held held;
    held.open(false);
    expect_refused(
        held,
        call_tool("2", "game.speed", "{\"speed\":21}"),
        "the speed is outside the match's range"
    );
    expect_refused(
        held,
        call_tool("3", "game.speed", "{\"speed\":0}"),
        "the speed is outside the match's range"
    );
    if (const std::optional<json::Json> answer =
            only(*held.session, call_tool("4", "game.speed", "{\"speed\":-1}")))
        check_error(
            *answer,
            mcp::invalid_params,
            "arguments do not match the tool's schema: speed is below 0"
        );
    if (const std::optional<json::Json> answer =
            only(*held.session, call_tool("5", "game.speed", "{\"speed\":\"fast\"}")))
        check_error(
            *answer,
            mcp::invalid_params,
            "arguments do not match the tool's schema: speed must be a integer"
        );
    OA_CHECK(held.world.speed_calls == 0);
}

/// A finished match refuses a pause and still takes a speed.
void finished_match_refuses_pause() {
    Held held;
    held.world.finished = true;
    held.open(false);
    expect_refused(
        held, call_tool("2", "game.pause", "{\"paused\":true}"), "the match has finished"
    );
    if (const std::optional<json::Json> answer =
            land(held, call_tool("3", "game.speed", "{\"speed\":6}"))) {
        OA_CHECK(!tool_failed(*answer));
        OA_CHECK(held.world.speed == 6);
        OA_CHECK(held.world.speed_calls == 1);
        OA_CHECK(held.world.pause_calls == 0);
    }
}

/// A shared match is changed, so the other machines can stay in step.
void shared_match_applies() {
    Held held;
    held.world.shared_match = true;
    held.open(false);
    if (const std::optional<json::Json> answer =
            land(held, call_tool("2", "game.speed", "{\"speed\":12}"))) {
        OA_CHECK(!tool_failed(*answer));
        OA_CHECK(held.world.speed == 12);
        OA_CHECK(held.world.speed_calls == 1);
    }
}

/// The speed and the pause land on the pump after apply, and the answer names that tick.
void speed_and_pause_name_the_tick() {
    Held held;
    held.world.tick = 80;
    held.open(false);
    if (const std::optional<json::Json> listed = only(*held.session, request("2", "tools/list"))) {
        const json::Json* result = listed->find("result");
        const json::Json* tools = result != nullptr ? result->find("tools") : nullptr;
        OA_CHECK(tools != nullptr && tools->elements().size() == 12);
    }
    if (const std::optional<json::Json> answer =
            land(held, call_tool("3", "game.speed", "{\"speed\":4}"))) {
        OA_CHECK(!tool_failed(*answer));
        OA_CHECK(held.world.speed == 4);
        OA_CHECK(held.world.speed_calls == 1);
        const std::optional<json::Json> body = parsed(tool_text(*answer));
        if (body) {
            const json::Json* tick = body->find("applied_tick");
            const json::Json* speed = body->find("speed");
            OA_CHECK(tick != nullptr && tick->integer() && *tick->integer() == 80);
            OA_CHECK(speed != nullptr && speed->integer() && *speed->integer() == 4);
        }
    }
    held.world.tick = 81;
    if (const std::optional<json::Json> answer =
            land(held, call_tool("4", "game.pause", "{\"paused\":true}"))) {
        OA_CHECK(!tool_failed(*answer));
        OA_CHECK(held.world.paused);
        OA_CHECK(held.world.pause_calls == 1);
        const std::optional<json::Json> body = parsed(tool_text(*answer));
        if (body) {
            const json::Json* tick = body->find("applied_tick");
            const json::Json* paused = body->find("paused");
            OA_CHECK(tick != nullptr && tick->integer() && *tick->integer() == 81);
            OA_CHECK(paused != nullptr && paused->boolean() && *paused->boolean());
        }
    }
    held.world.tick = 81;
    if (const std::optional<json::Json> answer =
            land(held, call_tool("5", "game.pause", "{\"paused\":false}"))) {
        OA_CHECK(!tool_failed(*answer));
        OA_CHECK(!held.world.paused);
        OA_CHECK(held.world.resume_calls == 1);
        const std::optional<json::Json> body = parsed(tool_text(*answer));
        if (body) {
            const json::Json* paused = body->find("paused");
            OA_CHECK(paused != nullptr && paused->boolean() && !*paused->boolean());
        }
    }
}

/// A speed the match is already at still names the tick.
void speed_already_set_still_names_the_tick() {
    Held held;
    held.world.speed = 4;
    held.world.tick = 12;
    held.open(false);
    if (const std::optional<json::Json> answer =
            land(held, call_tool("2", "game.speed", "{\"speed\":4}"))) {
        OA_CHECK(!tool_failed(*answer));
        OA_CHECK(held.world.speed == 4);
        OA_CHECK(held.world.speed_calls == 1);
        const std::optional<json::Json> body = parsed(tool_text(*answer));
        if (body) {
            const json::Json* tick = body->find("applied_tick");
            OA_CHECK(tick != nullptr && tick->integer() && *tick->integer() == 12);
        }
    }
}

/// Two changes queued in one pump are both applied, and both answers come on the next pump.
void two_acts_apply_together() {
    Held held;
    held.world.tick = 15;
    held.open(false);
    OA_CHECK(held.session->receive(call_tool("2", "game.speed", "{\"speed\":8}")).empty());
    OA_CHECK(held.session->receive(call_tool("3", "game.pause", "{\"paused\":true}")).empty());
    OA_CHECK(held.session->pump().empty());
    OA_CHECK(held.world.speed_calls == 0);
    OA_CHECK(held.world.pause_calls == 0);
    const std::vector<mcp::AppliedAct> acts =
        held.bridge.queue.apply(held.bridge.host_of(held.bridge.context));
    OA_CHECK(acts.size() == 2);
    OA_CHECK(held.world.speed == 8);
    OA_CHECK(held.world.paused);
    OA_CHECK(acts.size() >= 1 && acts[0].detail == "game.speed 8" && acts[0].tick == 15);
    OA_CHECK(acts.size() >= 2 && acts[1].detail == "game.pause paused" && acts[1].tick == 15);
    const std::vector<std::string> done = held.session->pump();
    OA_CHECK(done.size() == 2);
    if (done.size() == 2) {
        const std::optional<json::Json> speed = parsed(done[0]);
        const std::optional<json::Json> pause = parsed(done[1]);
        OA_CHECK(speed && !tool_failed(*speed));
        OA_CHECK(pause && !tool_failed(*pause));
    }
}

/// A host that does not make the change is reported, and the match is left as it was.
void host_refusal_is_reported() {
    Held held;
    held.world.refuse_speed = true;
    held.open(false);
    if (const std::optional<json::Json> answer =
            land(held, call_tool("2", "game.speed", "{\"speed\":4}"))) {
        OA_CHECK(tool_failed(*answer));
        OA_CHECK(tool_text(*answer) == "the speed did not change");
        OA_CHECK(held.world.speed == 10);
        OA_CHECK(held.world.speed_calls == 1);
    }
}

/// Eight changes can wait. A ninth is refused, and none of them is applied by queueing.
void queue_holds_eight() {
    World world;
    const oa::app::PlayHost host = host_from_world(&world);
    mcp::ActionQueue queue;
    mcp::PlayRequest request;
    request.kind = mcp::PlayKind::speed;
    request.speed = 4;
    int tokens[9] = {};
    for (int index = 0; index < 8; ++index) {
        const mcp::ToolReply reply = queue.follow(host, request, &tokens[index], 1);
        OA_CHECK(reply.held);
        OA_CHECK(!reply.failed);
    }
    const mcp::ToolReply full = queue.follow(host, request, &tokens[8], 1);
    OA_CHECK(full.failed);
    OA_CHECK(full.text == "too many changes are waiting");
    const mcp::ToolReply lost = queue.follow(host, request, &tokens[8], 2);
    OA_CHECK(lost.failed);
    OA_CHECK(lost.text == "the change was lost");
    OA_CHECK(world.speed_calls == 0);
    OA_CHECK(queue.waiting());
}

/// A session that only lists tools that read offers camera.get and not the act tools.
void observe_lists_only_the_reading_tools() {
    Held held;
    held.open(true);
    if (const std::optional<json::Json> listed = only(*held.session, request("2", "tools/list"))) {
        const json::Json* result = listed->find("result");
        const json::Json* tools = result != nullptr ? result->find("tools") : nullptr;
        OA_CHECK(tools != nullptr && tools->elements().size() == 1);
        if (tools != nullptr && !tools->elements().empty()) {
            const json::Json* name = tools->elements().front().find("name");
            OA_CHECK(name != nullptr && name->string() && *name->string() == "camera.get");
        }
    }
    if (const std::optional<json::Json> answer =
            only(*held.session, call_tool("3", "game.speed", "{\"speed\":4}")))
        check_error(*answer, mcp::invalid_params, "unknown tool game.speed");
    if (const std::optional<json::Json> answer =
            only(*held.session, call_tool("4", "game.pause", "{\"paused\":true}")))
        check_error(*answer, mcp::invalid_params, "unknown tool game.pause");
    if (const std::optional<json::Json> answer = only(
            *held.session, call_tool("5", "orders.issue", "{\"units\":[4],\"kind\":\"stop\"}")
        ))
        check_error(*answer, mcp::invalid_params, "unknown tool orders.issue");
    if (const std::optional<json::Json> answer = only(
            *held.session,
            call_tool("9", "orders.state", "{\"units\":[4],\"state\":\"fire\",\"value\":0}")
        ))
        check_error(*answer, mcp::invalid_params, "unknown tool orders.state");
    if (const std::optional<json::Json> answer =
            only(*held.session, call_tool("10", "units.select", "{\"units\":[4]}")))
        check_error(*answer, mcp::invalid_params, "unknown tool units.select");
    if (const std::optional<json::Json> answer =
            only(*held.session, call_tool("11", "camera.set", "{\"x\":1,\"z\":2}")))
        check_error(*answer, mcp::invalid_params, "unknown tool camera.set");
    if (const std::optional<json::Json> answer =
            only(*held.session, call_tool("12", "camera.center", "{\"unit\":3}")))
        check_error(*answer, mcp::invalid_params, "unknown tool camera.center");
    if (const std::optional<json::Json> answer =
            only(*held.session, call_tool("13", "camera.zoom", "{\"factor\":2}")))
        check_error(*answer, mcp::invalid_params, "unknown tool camera.zoom");
    if (const std::optional<json::Json> answer =
            only(*held.session, call_tool("6", "clock.mode", "{\"mode\":\"stepped\"}")))
        check_error(*answer, mcp::invalid_params, "unknown tool clock.mode");
    if (const std::optional<json::Json> answer =
            only(*held.session, call_tool("7", "clock.step", "{\"ticks\":1}")))
        check_error(*answer, mcp::invalid_params, "unknown tool clock.step");
    if (const std::optional<json::Json> answer = only(
            *held.session, call_tool("8", "clock.run", "{\"until\":\"event\",\"max_ticks\":1}")
        ))
        check_error(*answer, mcp::invalid_params, "unknown tool clock.run");
    if (const std::optional<json::Json> reading =
            only(*held.session, call_tool("14", "camera.get"))) {
        OA_CHECK(!tool_failed(*reading));
        const std::optional<json::Json> body = parsed(tool_text(*reading));
        if (body) {
            const json::Json* column = body->find("x");
            const json::Json* row = body->find("z");
            OA_CHECK(column != nullptr && column->integer() && *column->integer() == 10);
            OA_CHECK(row != nullptr && row->integer() && *row->integer() == 20);
        }
    }
    OA_CHECK(held.world.speed_calls == 0);
    OA_CHECK(held.world.pause_calls == 0);
    OA_CHECK(held.world.command_calls == 0);
    OA_CHECK(held.world.set_calls == 0);
    OA_CHECK(held.world.mode_calls == 0);
    OA_CHECK(held.world.run_calls == 0);
    OA_CHECK(!held.bridge.queue.waiting());
}

/// camera.get reads the view at once. A place past the limits is reported where they hold it.
void camera_reports_the_place_the_limits_hold() {
    Held held;
    held.world.tick = 30;
    held.open(false);
    if (const std::optional<json::Json> answer =
            only(*held.session, call_tool("2", "camera.get"))) {
        OA_CHECK(!tool_failed(*answer));
        const std::optional<json::Json> body = parsed(tool_text(*answer));
        if (body) {
            const json::Json* column = body->find("x");
            const json::Json* zoom = body->find("zoom");
            OA_CHECK(column != nullptr && column->integer() && *column->integer() == 10);
            OA_CHECK(zoom != nullptr && zoom->integer() && *zoom->integer() == 1);
        }
    }
    OA_CHECK(held.world.set_calls == 0);
    OA_CHECK(!held.bridge.queue.waiting());
    if (const std::optional<json::Json> answer =
            land(held, call_tool("3", "camera.set", "{\"x\":100000,\"z\":-40}"))) {
        OA_CHECK(!tool_failed(*answer));
        OA_CHECK(held.world.set_calls == 1);
        OA_CHECK(held.world.camera_x == 400);
        OA_CHECK(held.world.camera_z == 0);
        const std::optional<json::Json> body = parsed(tool_text(*answer));
        if (body) {
            const json::Json* column = body->find("x");
            const json::Json* row = body->find("z");
            const json::Json* tick = body->find("applied_tick");
            OA_CHECK(column != nullptr && column->integer() && *column->integer() == 400);
            OA_CHECK(row != nullptr && row->integer() && *row->integer() == 0);
            OA_CHECK(tick != nullptr && tick->integer() && *tick->integer() == 30);
        }
    }
    if (const std::optional<json::Json> answer =
            only(*held.session, call_tool("4", "camera.get"))) {
        OA_CHECK(!tool_failed(*answer));
        const std::optional<json::Json> body = parsed(tool_text(*answer));
        if (body) {
            const json::Json* column = body->find("x");
            OA_CHECK(column != nullptr && column->integer() && *column->integer() == 400);
        }
    }
}

/// A replay may point the view, and still may not change the speed.
void replay_may_point_the_view() {
    Held held;
    held.world.replay = true;
    held.open(false);
    expect_refused(
        held, call_tool("2", "game.speed", "{\"speed\":4}"), "a replay cannot change the speed"
    );
    if (const std::optional<json::Json> answer =
            land(held, call_tool("3", "camera.set", "{\"x\":12,\"z\":34}"))) {
        OA_CHECK(!tool_failed(*answer));
        OA_CHECK(held.world.camera_x == 12);
        OA_CHECK(held.world.camera_z == 34);
        OA_CHECK(held.world.speed_calls == 0);
        OA_CHECK(held.world.set_calls == 1);
    }
}

/// The director's view is refused, and the host is not asked to move it.
void director_refuses_a_camera_move() {
    Held held;
    held.world.director = true;
    held.open(false);
    expect_refused(
        held, call_tool("2", "camera.set", "{\"x\":12,\"z\":34}"), "the view is the director's"
    );
    expect_refused(
        held, call_tool("3", "camera.center", "{\"unit\":3}"), "the view is the director's"
    );
    expect_refused(
        held, call_tool("4", "camera.zoom", "{\"factor\":2}"), "the view is the director's"
    );
    OA_CHECK(held.world.set_calls == 0);
    OA_CHECK(held.world.center_unit_calls == 0);
    OA_CHECK(held.world.zoom_calls == 0);
}

/// Centring on a unit the match has not got is reported, and a point past the limits is held.
void centre_on_a_unit_or_a_point() {
    Held held;
    held.world.tick = 9;
    held.open(false);
    if (const std::optional<json::Json> answer =
            land(held, call_tool("2", "camera.center", "{\"unit\":9}"))) {
        OA_CHECK(tool_failed(*answer));
        OA_CHECK(tool_text(*answer) == "that unit is not on the map");
        OA_CHECK(held.world.center_unit_calls == 1);
        OA_CHECK(held.world.camera_x == 10);
    }
    if (const std::optional<json::Json> answer =
            land(held, call_tool("3", "camera.center", "{\"unit\":3}"))) {
        OA_CHECK(!tool_failed(*answer));
        OA_CHECK(held.world.last_unit == 3);
        OA_CHECK(held.world.camera_x == 200);
        OA_CHECK(held.world.camera_z == 150);
        const std::optional<json::Json> body = parsed(tool_text(*answer));
        if (body) {
            const json::Json* column = body->find("x");
            OA_CHECK(column != nullptr && column->integer() && *column->integer() == 200);
        }
    }
    if (const std::optional<json::Json> answer =
            land(held, call_tool("4", "camera.center", "{\"x\":900,\"z\":40}"))) {
        OA_CHECK(!tool_failed(*answer));
        OA_CHECK(held.world.camera_x == 400);
        OA_CHECK(held.world.camera_z == 40);
        OA_CHECK(held.world.center_point_calls == 1);
    }
    if (const std::optional<json::Json> answer =
            only(*held.session, call_tool("5", "camera.center", "{\"unit\":3,\"x\":1,\"z\":2}"))) {
        OA_CHECK(tool_failed(*answer));
        OA_CHECK(tool_text(*answer) == "name a unit or a point");
    }
    if (const std::optional<json::Json> answer =
            only(*held.session, call_tool("6", "camera.center", "{}"))) {
        OA_CHECK(tool_failed(*answer));
        OA_CHECK(tool_text(*answer) == "name a unit or a point");
    }
    OA_CHECK(!held.bridge.queue.waiting());
}

/// A factor and an absolute zoom are both held within the zoom's limits.
void zoom_is_held_within_its_limits() {
    Held held;
    held.world.zoom = 1.0F;
    held.world.tick = 4;
    held.open(false);
    if (const std::optional<json::Json> answer =
            land(held, call_tool("2", "camera.zoom", "{\"factor\":2}"))) {
        OA_CHECK(!tool_failed(*answer));
        OA_CHECK(held.world.zoom == 2.0F);
        OA_CHECK(held.world.zoom_calls == 1);
        const std::optional<json::Json> body = parsed(tool_text(*answer));
        if (body) {
            const json::Json* zoom = body->find("zoom");
            OA_CHECK(zoom != nullptr && zoom->integer() && *zoom->integer() == 2);
        }
    }
    if (const std::optional<json::Json> answer =
            land(held, call_tool("3", "camera.zoom", "{\"zoom\":10}"))) {
        OA_CHECK(!tool_failed(*answer));
        OA_CHECK(held.world.zoom == 4.0F);
        const std::optional<json::Json> body = parsed(tool_text(*answer));
        if (body) {
            const json::Json* zoom = body->find("zoom");
            OA_CHECK(zoom != nullptr && zoom->integer() && *zoom->integer() == 4);
        }
    }
    if (const std::optional<json::Json> answer =
            only(*held.session, call_tool("4", "camera.zoom", "{\"factor\":0}"))) {
        OA_CHECK(tool_failed(*answer));
        OA_CHECK(tool_text(*answer) == "the zoom is not a positive number");
    }
    if (const std::optional<json::Json> answer =
            only(*held.session, call_tool("5", "camera.zoom", "{\"zoom\":2,\"factor\":2}"))) {
        OA_CHECK(tool_failed(*answer));
        OA_CHECK(tool_text(*answer) == "name a zoom or a factor");
    }
    OA_CHECK(held.world.zoom_calls == 2);
    OA_CHECK(!held.bridge.queue.waiting());
}

/// No running match refuses a reading and a move.
void no_match_refuses_the_camera() {
    Held held;
    held.world.in_match = false;
    held.open(false);
    expect_refused(held, call_tool("2", "camera.get"), "no match is running");
    expect_refused(held, call_tool("3", "camera.set", "{\"x\":1,\"z\":2}"), "no match is running");
    expect_refused(held, call_tool("4", "camera.zoom", "{\"factor\":2}"), "no match is running");
}

/// Tells whether a landed order's queues name a kind.
///
/// @param body the tool's JSON
/// @param kind the mission name
/// @return true when a primary or secondary order has that kind
bool queue_has_kind(const json::Json& body, std::string_view kind) {
    const json::Json* units = body.find("units");
    if (units == nullptr)
        return false;
    for (const json::Json& unit : units->elements()) {
        const json::Json* primary = unit.find("primary");
        const json::Json* secondary = unit.find("secondary");
        for (const json::Json* queue : {primary, secondary}) {
            if (queue == nullptr)
                continue;
            for (const json::Json& line : queue->elements()) {
                const json::Json* got = line.find("kind");
                if (got != nullptr && got->string() != nullptr && *got->string() == kind)
                    return true;
            }
        }
    }
    return false;
}

/// A move lands on the pump after apply, and the answer names that tick and the queue.
void order_names_the_tick_and_the_queue() {
    Held held;
    held.world.tick = 44;
    held.open(false);
    if (const std::optional<json::Json> answer = land(
            held,
            call_tool(
                "2",
                "orders.issue",
                "{\"units\":[4],\"kind\":\"move\",\"point\":[12,30],\"append\":false}"
            )
        )) {
        OA_CHECK(!tool_failed(*answer));
        OA_CHECK(held.world.command_calls == 1);
        OA_CHECK(held.world.issued == 1);
        const std::optional<json::Json> body = parsed(tool_text(*answer));
        if (body) {
            const json::Json* tick = body->find("applied_tick");
            OA_CHECK(tick != nullptr && tick->integer() && *tick->integer() == 44);
            OA_CHECK(queue_has_kind(*body, "Move_Ground"));
        }
    }
}

/// A standing fire order names the tick and Standing_FireOrder.
void state_names_the_tick_and_the_queue() {
    Held held;
    held.world.tick = 45;
    held.open(false);
    if (const std::optional<json::Json> answer = land(
            held, call_tool("2", "orders.state", "{\"units\":[4],\"state\":\"fire\",\"value\":1}")
        )) {
        OA_CHECK(!tool_failed(*answer));
        OA_CHECK(held.world.issued == 1);
        const std::optional<json::Json> body = parsed(tool_text(*answer));
        if (body) {
            const json::Json* tick = body->find("applied_tick");
            OA_CHECK(tick != nullptr && tick->integer() && *tick->integer() == 45);
            OA_CHECK(queue_has_kind(*body, "Standing_FireOrder"));
        }
    }
}

/// Selecting the machine's unit names the tick and that unit.
void select_names_the_units() {
    Held held;
    held.world.tick = 46;
    held.open(false);
    if (const std::optional<json::Json> answer =
            land(held, call_tool("2", "units.select", "{\"units\":[4]}"))) {
        OA_CHECK(!tool_failed(*answer));
        OA_CHECK(held.world.issued == 1);
        OA_CHECK(held.world.selected.size() == 1 && held.world.selected[0] == 4);
        const std::optional<json::Json> body = parsed(tool_text(*answer));
        if (body) {
            const json::Json* tick = body->find("applied_tick");
            const json::Json* selected = body->find("selected");
            OA_CHECK(tick != nullptr && tick->integer() && *tick->integer() == 46);
            OA_CHECK(
                selected != nullptr && selected->elements().size() == 1 &&
                selected->elements().front().integer() &&
                *selected->elements().front().integer() == 4
            );
        }
    }
}

/// A shared match, a replay, a watcher and no match refuse the clock, and the host is not called.
void clock_is_refused_for_the_matches_that_keep_their_own() {
    Held shared;
    shared.world.shared_match = true;
    shared.world.stepped = true;
    shared.open(false);
    expect_refused(
        shared,
        call_tool("2", "clock.mode", "{\"mode\":\"stepped\"}"),
        "the stepped clock is refused in a shared match"
    );
    expect_refused(
        shared,
        call_tool("3", "clock.step", "{\"ticks\":4}"),
        "the stepped clock is refused in a shared match"
    );
    OA_CHECK(shared.world.speed_calls == 0);

    Held replay;
    replay.world.replay = true;
    replay.world.stepped = true;
    replay.open(false);
    expect_refused(
        replay, call_tool("2", "clock.step", "{\"ticks\":4}"), "a replay keeps its own clock"
    );

    Held watcher;
    watcher.world.watcher = true;
    watcher.open(false);
    expect_refused(
        watcher,
        call_tool("2", "clock.mode", "{\"mode\":\"realtime\"}"),
        "a watcher cannot change the clock"
    );

    Held none;
    none.world.in_match = false;
    none.open(false);
    expect_refused(none, call_tool("2", "clock.step", "{\"ticks\":1}"), "no match is running");
}

/// A clock that is not stepped, a paused match and a finished match refuse a step.
void step_needs_a_stepped_clock_that_is_running() {
    Held held;
    held.open(false);
    expect_refused(held, call_tool("2", "clock.step", "{\"ticks\":3}"), "the clock is not stepped");
    held.world.stepped = true;
    held.world.paused = true;
    expect_refused(held, call_tool("3", "clock.step", "{\"ticks\":3}"), "the match is paused");
    held.world.paused = false;
    held.world.finished = true;
    expect_refused(held, call_tool("4", "clock.step", "{\"ticks\":3}"), "the match has finished");
    if (const std::optional<json::Json> answer =
            only(*held.session, call_tool("5", "clock.step", "{\"ticks\":0}")))
        check_error(
            *answer,
            mcp::invalid_params,
            "arguments do not match the tool's schema: ticks is below 1"
        );
    if (const std::optional<json::Json> answer =
            only(*held.session, call_tool("6", "clock.mode", "{\"mode\":\"fast\"}")))
        check_error(
            *answer,
            mcp::invalid_params,
            "arguments do not match the tool's schema: mode is not one of the allowed values"
        );
    OA_CHECK(held.world.run_calls == 0);
}

/// clock.mode lands on the tick it found, and clock.step runs that many ticks.
void mode_and_step_name_the_tick() {
    Held held;
    held.world.tick = 20;
    held.open(false);
    if (const std::optional<json::Json> answer =
            land(held, call_tool("2", "clock.mode", "{\"mode\":\"stepped\"}"))) {
        OA_CHECK(!tool_failed(*answer));
        OA_CHECK(held.world.stepped);
        OA_CHECK(held.world.mode_calls == 1);
        OA_CHECK(held.world.run_calls == 0);
        const std::optional<json::Json> body = parsed(tool_text(*answer));
        if (body) {
            const json::Json* mode = body->find("mode");
            const json::Json* tick = body->find("applied_tick");
            OA_CHECK(mode != nullptr && mode->string() && *mode->string() == "stepped");
            OA_CHECK(tick != nullptr && tick->integer() && *tick->integer() == 20);
        }
    }
    if (const std::optional<json::Json> answer =
            land(held, call_tool("3", "clock.step", "{\"ticks\":7}"))) {
        OA_CHECK(!tool_failed(*answer));
        OA_CHECK(held.world.tick == 27);
        OA_CHECK(held.world.run_calls == 1);
        const std::optional<json::Json> body = parsed(tool_text(*answer));
        if (body) {
            const json::Json* tick = body->find("tick");
            const json::Json* ran = body->find("ran");
            const json::Json* events = body->find("events");
            OA_CHECK(tick != nullptr && tick->integer() && *tick->integer() == 27);
            OA_CHECK(ran != nullptr && ran->integer() && *ran->integer() == 7);
            OA_CHECK(events != nullptr && events->elements().empty());
        }
    }
}

/// clock.run stops at a tick, or when the match ends, before max_ticks.
void run_stops_at_a_tick_or_when_the_match_ends() {
    Held ticks;
    ticks.world.stepped = true;
    ticks.world.tick = 10;
    ticks.open(false);
    if (const std::optional<json::Json> answer = land(
            ticks, call_tool("2", "clock.run", "{\"until\":\"tick\",\"tick\":15,\"max_ticks\":100}")
        )) {
        OA_CHECK(!tool_failed(*answer));
        OA_CHECK(ticks.world.tick == 15);
        const std::optional<json::Json> body = parsed(tool_text(*answer));
        if (body) {
            const json::Json* ran = body->find("ran");
            const json::Json* stopped = body->find("stopped");
            OA_CHECK(ran != nullptr && ran->integer() && *ran->integer() == 5);
            OA_CHECK(stopped != nullptr && stopped->string() && *stopped->string() == "tick");
        }
    }
    if (const std::optional<json::Json> missing = only(
            *ticks.session, call_tool("3", "clock.run", "{\"until\":\"tick\",\"max_ticks\":4}")
        ))
        OA_CHECK(
            tool_failed(*missing) && tool_text(*missing) == "clock.run until tick needs a tick"
        );

    Held ending;
    ending.world.stepped = true;
    ending.world.tick = 10;
    ending.world.end_after = 3;
    ending.open(false);
    if (const std::optional<json::Json> answer =
            land(ending, call_tool("2", "clock.run", "{\"until\":\"event\",\"max_ticks\":100}"))) {
        OA_CHECK(!tool_failed(*answer));
        OA_CHECK(ending.world.tick == 13);
        OA_CHECK(ending.world.finished);
        const std::optional<json::Json> body = parsed(tool_text(*answer));
        if (body) {
            const json::Json* ran = body->find("ran");
            const json::Json* stopped = body->find("stopped");
            const json::Json* events = body->find("events");
            OA_CHECK(ran != nullptr && ran->integer() && *ran->integer() == 3);
            OA_CHECK(stopped != nullptr && stopped->string() && *stopped->string() == "event");
            OA_CHECK(
                events != nullptr && events->elements().size() == 1 &&
                events->elements().front().string() &&
                *events->elements().front().string() == "game_over"
            );
        }
    }
}

/// An order, a standing order and a selection for a unit this machine does not play are refused.
///
/// The host is called when the queue is applied, and it gives nothing: the
/// issued count stays, and a selection that was already made stays.
void guard_refuses_an_enemy_unit() {
    Held held;
    held.open(false);
    held.world.selected.push_back(held.world.own_unit);
    const int issued = held.world.issued;
    if (const std::optional<json::Json> answer = land(
            held,
            call_tool("2", "orders.issue", "{\"units\":[9],\"kind\":\"move\",\"point\":[12,30]}")
        )) {
        OA_CHECK(tool_failed(*answer));
        OA_CHECK(tool_text(*answer) == "that unit is not this machine's");
        OA_CHECK(held.world.command_calls == 1);
        OA_CHECK(held.world.issued == issued);
    }
    if (const std::optional<json::Json> answer = land(
            held, call_tool("3", "orders.state", "{\"units\":[9],\"state\":\"fire\",\"value\":1}")
        )) {
        OA_CHECK(tool_failed(*answer));
        OA_CHECK(tool_text(*answer) == "that unit is not this machine's");
        OA_CHECK(held.world.issued == issued);
    }
    if (const std::optional<json::Json> answer =
            land(held, call_tool("4", "units.select", "{\"units\":[9]}"))) {
        OA_CHECK(tool_failed(*answer));
        OA_CHECK(tool_text(*answer) == "that unit is not this machine's");
        OA_CHECK(held.world.issued == issued);
        OA_CHECK(held.world.selected.size() == 1 && held.world.selected[0] == held.world.own_unit);
        OA_CHECK(held.world.command_calls == 3);
    }
}

/// A replay cannot give orders, set a standing order or select.
void replay_refuses_orders() {
    Held held;
    held.world.replay = true;
    held.open(false);
    expect_refused(
        held,
        call_tool("2", "orders.issue", "{\"units\":[4],\"kind\":\"move\",\"point\":[1,2]}"),
        "a replay cannot give orders"
    );
    expect_refused(
        held,
        call_tool("3", "orders.state", "{\"units\":[4],\"state\":\"fire\",\"value\":1}"),
        "a replay cannot give orders"
    );
    expect_refused(
        held, call_tool("4", "units.select", "{\"units\":[4]}"), "a replay cannot give orders"
    );
}

/// A watcher cannot give orders.
void watcher_refuses_orders() {
    Held held;
    held.world.watcher = true;
    held.open(false);
    expect_refused(
        held,
        call_tool("2", "orders.issue", "{\"units\":[4],\"kind\":\"stop\"}"),
        "a watcher cannot give orders"
    );
    expect_refused(
        held, call_tool("3", "units.select", "{\"units\":[]}"), "a watcher cannot give orders"
    );
}

/// No running match refuses an order.
void no_match_refuses_orders() {
    Held held;
    held.world.in_match = false;
    held.open(false);
    expect_refused(
        held,
        call_tool("2", "orders.issue", "{\"units\":[4],\"kind\":\"stop\"}"),
        "no match is running"
    );
}

/// A finished match refuses an order.
void finished_match_refuses_orders() {
    Held held;
    held.world.finished = true;
    held.open(false);
    expect_refused(
        held,
        call_tool("2", "orders.issue", "{\"units\":[4],\"kind\":\"move\",\"point\":[1,2]}"),
        "the match has finished"
    );
    expect_refused(
        held, call_tool("3", "units.select", "{\"units\":[4]}"), "the match has finished"
    );
}

/// An order that names no unit, no point or no attack is refused before the host is called.
void order_arguments_are_refused_at_once() {
    Held held;
    held.open(false);
    expect_refused(
        held,
        call_tool("2", "orders.issue", "{\"units\":[],\"kind\":\"move\",\"point\":[1,2]}"),
        "name at least one unit"
    );
    expect_refused(
        held,
        call_tool("3", "orders.issue", "{\"units\":[4],\"kind\":\"move\"}"),
        "the order needs a point"
    );
    expect_refused(
        held,
        call_tool("4", "orders.issue", "{\"units\":[4],\"kind\":\"attack\"}"),
        "an attack needs a unit or a point"
    );
    expect_refused(
        held,
        call_tool("5", "orders.issue", "{\"units\":[4],\"kind\":\"guard\"}"),
        "a guard needs a unit"
    );
    expect_refused(
        held,
        call_tool("6", "orders.state", "{\"units\":[4],\"state\":\"cloak\"}"),
        "cloak needs on"
    );
    expect_refused(
        held,
        call_tool("7", "orders.state", "{\"units\":[4],\"state\":\"fire\"}"),
        "the standing order needs a value"
    );
    expect_refused(
        held,
        call_tool("8", "orders.issue", "{\"units\":[\"no\"],\"kind\":\"stop\"}"),
        "units must be whole numbers"
    );
}

/// A host with no order entry fails the change and is not called.
void missing_order_entry_is_refused() {
    World world;
    oa::app::PlayHost host = host_from_world(&world);
    host.issue_orders = nullptr;
    mcp::ActionQueue queue;
    mcp::PlayRequest request;
    request.kind = mcp::PlayKind::order;
    request.order = oa::app::PlayOrderKind::move;
    request.units.push_back(4);
    request.has_point = true;
    int token = 0;
    const mcp::ToolReply queued = queue.follow(host, request, &token, 1);
    OA_CHECK(queued.held);
    OA_CHECK(!queued.failed);
    const std::vector<mcp::AppliedAct> acts = queue.apply(host);
    OA_CHECK(acts.empty());
    const mcp::ToolReply reply = queue.follow(host, request, &token, 2);
    OA_CHECK(reply.failed);
    OA_CHECK(reply.text == "the order was not given");
    OA_CHECK(world.command_calls == 0);
    OA_CHECK(world.issued == 0);
}

/// A step longer than one frame stays held, then finishes on the next apply.
void a_long_step_continues_on_the_next_frame() {
    Held held;
    held.world.stepped = true;
    held.world.tick = 4;
    held.open(false);
    const uint32_t many = mcp::kClockTicksPerFrame + 5;
    const std::string call =
        call_tool("2", "clock.step", "{\"ticks\":" + std::to_string(many) + "}");
    OA_CHECK(held.session->receive(call).empty());
    OA_CHECK(held.session->pump().empty());
    OA_CHECK(held.world.run_calls == 0);
    const oa::app::PlayHost host = held.bridge.host_of(held.bridge.context);
    const std::vector<mcp::AppliedAct> first = held.bridge.queue.apply(host);
    OA_CHECK(first.empty());
    OA_CHECK(held.bridge.queue.waiting());
    OA_CHECK(held.world.tick == 4 + mcp::kClockTicksPerFrame);
    OA_CHECK(held.world.run_calls == 1);
    OA_CHECK(held.session->pump().empty());
    const std::vector<mcp::AppliedAct> second = held.bridge.queue.apply(host);
    OA_CHECK(!held.bridge.queue.waiting());
    OA_CHECK(held.world.tick == 4 + many);
    OA_CHECK(second.size() == 1);
    OA_CHECK(second.size() == 1 && second[0].tick == 4 + many);
    OA_CHECK(second.size() == 1 && second[0].detail == "clock.step " + std::to_string(many));
    const std::vector<std::string> done = held.session->pump();
    OA_CHECK(done.size() == 1);
    if (done.size() == 1) {
        const std::optional<json::Json> answer = parsed(done[0]);
        OA_CHECK(answer && !tool_failed(*answer));
        const std::optional<json::Json> body = answer ? parsed(tool_text(*answer)) : std::nullopt;
        if (body) {
            const json::Json* ran = body->find("ran");
            OA_CHECK(
                ran != nullptr && ran->integer() && *ran->integer() == static_cast<int64_t>(many)
            );
        }
    }
}

/// An order sent under a stepped clock lands on the tick the step runs.
void order_under_a_stepped_clock_lands_on_the_step() {
    Held held;
    held.world.stepped = true;
    held.world.tick = 30;
    held.open(false);
    const std::string move_call = call_tool(
        "2", "orders.issue", "{\"units\":[4],\"kind\":\"move\",\"point\":[12,30],\"append\":false}"
    );
    OA_CHECK(held.session->receive(move_call).empty());
    OA_CHECK(held.session->pump().empty());
    const oa::app::PlayHost host = held.bridge.host_of(held.bridge.context);
    const std::vector<mcp::AppliedAct> waiting = held.bridge.queue.apply(host);
    OA_CHECK(waiting.empty());
    OA_CHECK(held.bridge.queue.waiting());
    OA_CHECK(held.world.issued == 0);
    OA_CHECK(held.world.tick == 30);
    OA_CHECK(held.world.command_calls == 1);
    OA_CHECK(held.session->pump().empty());

    const std::string step = call_tool("3", "clock.step", "{\"ticks\":1}");
    OA_CHECK(held.session->receive(step).empty());
    OA_CHECK(held.session->pump().empty());
    const std::vector<mcp::AppliedAct> acts = held.bridge.queue.apply(host);
    OA_CHECK(!held.bridge.queue.waiting());
    OA_CHECK(held.world.tick == 31);
    OA_CHECK(held.world.issued == 1);
    OA_CHECK(held.world.command_calls == 1);
    OA_CHECK(acts.size() == 2);
    OA_CHECK(acts.size() >= 1 && acts[0].detail == "orders.issue move 4" && acts[0].tick == 31);
    OA_CHECK(acts.size() >= 2 && acts[1].detail == "clock.step 1" && acts[1].tick == 31);
    const std::vector<std::string> done = held.session->pump();
    OA_CHECK(done.size() == 2);
    if (done.size() == 2) {
        const std::optional<json::Json> order_answer = parsed(done[0]);
        const std::optional<json::Json> step_answer = parsed(done[1]);
        OA_CHECK(order_answer && !tool_failed(*order_answer));
        OA_CHECK(step_answer && !tool_failed(*step_answer));
        const std::optional<json::Json> order_body =
            order_answer ? parsed(tool_text(*order_answer)) : std::nullopt;
        const std::optional<json::Json> step_body =
            step_answer ? parsed(tool_text(*step_answer)) : std::nullopt;
        if (order_body) {
            const json::Json* tick = order_body->find("applied_tick");
            OA_CHECK(tick != nullptr && tick->integer() && *tick->integer() == 31);
            OA_CHECK(queue_has_kind(*order_body, "Move_Ground"));
            const json::Json* units = order_body->find("units");
            const json::Json* primary = units != nullptr && !units->elements().empty()
                                            ? units->elements().front().find("primary")
                                            : nullptr;
            const json::Json* issued_at = primary != nullptr && !primary->elements().empty()
                                              ? primary->elements().front().find("issue_tick")
                                              : nullptr;
            OA_CHECK(issued_at != nullptr && issued_at->integer() && *issued_at->integer() == 31);
        }
        if (step_body) {
            const json::Json* tick = step_body->find("tick");
            const json::Json* ran = step_body->find("ran");
            OA_CHECK(tick != nullptr && tick->integer() && *tick->integer() == 31);
            OA_CHECK(ran != nullptr && ran->integer() && *ran->integer() == 1);
        }
    }
}

} // namespace

int main() {
    replay_refuses_speed_and_pause();
    watcher_refuses_speed_and_pause();
    no_match_refuses();
    speed_outside_the_range_is_refused();
    finished_match_refuses_pause();
    shared_match_applies();
    speed_and_pause_name_the_tick();
    speed_already_set_still_names_the_tick();
    two_acts_apply_together();
    host_refusal_is_reported();
    queue_holds_eight();
    observe_lists_only_the_reading_tools();
    camera_reports_the_place_the_limits_hold();
    replay_may_point_the_view();
    director_refuses_a_camera_move();
    centre_on_a_unit_or_a_point();
    zoom_is_held_within_its_limits();
    no_match_refuses_the_camera();
    order_names_the_tick_and_the_queue();
    state_names_the_tick_and_the_queue();
    select_names_the_units();
    guard_refuses_an_enemy_unit();
    replay_refuses_orders();
    watcher_refuses_orders();
    no_match_refuses_orders();
    finished_match_refuses_orders();
    order_arguments_are_refused_at_once();
    missing_order_entry_is_refused();
    clock_is_refused_for_the_matches_that_keep_their_own();
    step_needs_a_stepped_clock_that_is_running();
    mode_and_step_name_the_tick();
    run_stops_at_a_tick_or_when_the_match_ends();
    a_long_step_continues_on_the_next_frame();
    order_under_a_stepped_clock_lands_on_the_step();
    return oa::test::check_exit_status();
}
