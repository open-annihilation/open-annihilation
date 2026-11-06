// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The action queue and the tools game.speed, game.pause, the camera, the
// clock, orders.issue, orders.state and units.select (actions.hpp,
// orders.cpp). Checking a request does not change the match. Applying one
// does, through the play host, at after_pump. A stepped clock holds an
// order until a step runs it. camera.get only reads.
#include "oa/app/mcp/actions.hpp"

#include "oa/formats/json.hpp"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace oa::app::mcp {
namespace {

namespace json = oa::formats::json;

constexpr std::string_view kSpeedSchema =
    "{\"type\":\"object\",\"properties\":{\"speed\":{\"type\":\"integer\",\"minimum\":0}},"
    "\"required\":[\"speed\"],\"additionalProperties\":false}";

constexpr std::string_view kPauseSchema =
    "{\"type\":\"object\",\"properties\":{\"paused\":{\"type\":\"boolean\"}},"
    "\"required\":[\"paused\"],\"additionalProperties\":false}";

constexpr std::string_view kCameraGetSchema =
    "{\"type\":\"object\",\"additionalProperties\":false}";

constexpr std::string_view kCameraSetSchema =
    "{\"type\":\"object\",\"properties\":{\"x\":{\"type\":\"integer\"},\"z\":{\"type\":\"integer\"}"
    "},"
    "\"required\":[\"x\",\"z\"],\"additionalProperties\":false}";

constexpr std::string_view kCameraCenterSchema =
    "{\"type\":\"object\",\"properties\":{"
    "\"unit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":65535},"
    "\"x\":{\"type\":\"integer\"},\"z\":{\"type\":\"integer\"}},"
    "\"additionalProperties\":false}";

constexpr std::string_view kCameraZoomSchema =
    "{\"type\":\"object\",\"properties\":{"
    "\"zoom\":{\"type\":\"number\"},\"factor\":{\"type\":\"number\"},"
    "\"x\":{\"type\":\"number\"},\"y\":{\"type\":\"number\"}},"
    "\"additionalProperties\":false}";

constexpr std::string_view kModeSchema =
    "{\"type\":\"object\",\"properties\":{\"mode\":{\"type\":\"string\",\"enum\":[\"realtime\","
    "\"stepped\"]}},\"required\":[\"mode\"],\"additionalProperties\":false}";

constexpr std::string_view kStepSchema =
    "{\"type\":\"object\",\"properties\":{\"ticks\":{\"type\":\"integer\",\"minimum\":1,"
    "\"maximum\":100000}},\"required\":[\"ticks\"],\"additionalProperties\":false}";

constexpr std::string_view kRunSchema =
    "{\"type\":\"object\",\"properties\":{\"until\":{\"type\":\"string\",\"enum\":[\"event\","
    "\"tick\"]},\"max_ticks\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100000},\"tick\":{"
    "\"type\":\"integer\",\"minimum\":0}},\"required\":[\"until\",\"max_ticks\"],"
    "\"additionalProperties\":false}";

/// Tells whether a request moves the view.
///
/// @param kind what the call asked for
/// @return true for a camera act
bool camera_act(PlayKind kind) {
    return kind == PlayKind::camera_set || kind == PlayKind::camera_center_unit ||
           kind == PlayKind::camera_center_point || kind == PlayKind::camera_zoom;
}

/// Tells whether the request steps or runs the clock.
///
/// @param kind what the call asked for
/// @return true for clock.step and clock.run
bool clock_steps(PlayKind kind) {
    return kind == PlayKind::clock_step || kind == PlayKind::clock_run;
}

/// Tells whether the request changes the clock.
///
/// @param kind what the call asked for
/// @return true for clock.mode, clock.step and clock.run
bool clock_request(PlayKind kind) {
    return kind == PlayKind::clock_mode || clock_steps(kind);
}

/// Writes a zoom as a JSON number, with no exponent and no trailing zeros.
///
/// Six decimal places round-trip the zooms the view uses. A value that is
/// not a finite number is written as 0.
///
/// @param zoom the zoom
/// @return the number's text
std::string decimal_text(double zoom) {
    if (!std::isfinite(zoom))
        return "0";
    const bool negative = zoom < 0.0;
    const double magnitude = negative ? -zoom : zoom;
    const auto scaled = static_cast<long long>(std::llround(magnitude * 1000000.0));
    std::string digits = std::to_string(scaled < 0 ? -scaled : scaled);
    if (digits.size() <= 6)
        digits.insert(0, 7 - digits.size(), '0');
    digits.insert(digits.size() - 6, 1, '.');
    while (digits.size() > 1 && digits.back() == '0')
        digits.pop_back();
    if (!digits.empty() && digits.back() == '.')
        digits.pop_back();
    if (negative && digits != "0")
        digits.insert(digits.begin(), '-');
    if (digits.empty())
        return "0";
    return digits;
}

/// Reads a JSON number's text. The reader keeps the text as written, and this
/// accepts a sign, a fraction and an exponent, which is what JSON allows.
///
/// @param text the number as written
/// @return the value, or nothing when the text is not such a number
std::optional<double> parse_decimal(std::string_view text) {
    if (text.empty())
        return std::nullopt;
    std::size_t index = 0;
    double sign = 1.0;
    if (text[index] == '+' || text[index] == '-') {
        if (text[index] == '-')
            sign = -1.0;
        ++index;
    }
    if (index >= text.size())
        return std::nullopt;
    bool saw_digit = false;
    double value = 0.0;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
        saw_digit = true;
        value = value * 10.0 + static_cast<double>(text[index] - '0');
        ++index;
    }
    if (index < text.size() && text[index] == '.') {
        ++index;
        double weight = 0.1;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            saw_digit = true;
            value += static_cast<double>(text[index] - '0') * weight;
            weight *= 0.1;
            ++index;
        }
    }
    if (!saw_digit)
        return std::nullopt;
    if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
        ++index;
        const bool tiny = index < text.size() && text[index] == '-';
        if (index < text.size() && (text[index] == '+' || text[index] == '-'))
            ++index;
        if (index >= text.size() || text[index] < '0' || text[index] > '9')
            return std::nullopt;
        int exponent = 0;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            exponent = exponent * 10 + (text[index] - '0');
            if (exponent > 99)
                return std::nullopt;
            ++index;
        }
        double scale = 1.0;
        for (int raised = 0; raised < exponent; ++raised)
            scale *= 10.0;
        if (tiny)
            value /= scale;
        else
            value *= scale;
    }
    if (index != text.size() || !std::isfinite(value))
        return std::nullopt;
    return sign * value;
}

/// Writes the view's place and zoom as object members.
///
/// @param[in,out] writer the object being written
/// @param shown the view
void write_place(json::JsonWriter& writer, const PlayView& shown) {
    writer.key("x");
    writer.integer(shown.x);
    writer.key("z");
    writer.integer(shown.z);
    writer.key("zoom");
    writer.raw(decimal_text(static_cast<double>(shown.zoom)));
}

/// A held answer: the call is not finished.
///
/// @return the answer
ToolReply held_reply() {
    ToolReply reply;
    reply.held = true;
    return reply;
}

/// A failure the tool ran into. The sentence is what the player is told.
///
/// @param text why it was refused
/// @return the answer
ToolReply failed_reply(std::string text) {
    ToolReply reply;
    reply.failed = true;
    reply.text = std::move(text);
    return reply;
}

/// Tells whether a request gives an order, sets a standing order, or selects.
///
/// @param kind what the call asked for
/// @return true for an order, a standing order or a selection
bool commands(PlayKind kind) {
    return kind == PlayKind::order || kind == PlayKind::state || kind == PlayKind::select;
}

/// Tells whether a stepped clock holds this change until a step runs it.
///
/// A selection is made at once, as a click is. Speed and pause are too.
///
/// @param kind what the call asked for
/// @return true for an order or a standing order
bool held_command(PlayKind kind) {
    return kind == PlayKind::order || kind == PlayKind::state;
}

/// Why a request must not be queued, or empty when it may.
///
/// No running match, then, for the view, the director's picture. A replay
/// and a watcher may point the view: it is this machine's picture, and the
/// recording and the other machines keep their own. For an order: a replay,
/// a watcher, then a finished match. For the speed and the pause: a replay,
/// then a watcher, then a shared match's clock, then a finished match's
/// pause or step, then a paused match's step, then a step while the clock
/// is not stepped, then a speed outside the speeds the match allows. A
/// shared match may still change its speed, its pause and its orders: the
/// host sends that change on, and an order applies to the units this machine
/// plays.
///
/// @param host the host of this frame
/// @param request what the call asked for
/// @return the sentence, or empty
std::string refusal(const PlayHost& host, const PlayRequest& request) {
    if (host.extension_state == nullptr)
        return "no match is running";
    const PlayExtensionState state = host.extension_state(host.context);
    if (!state.in_match)
        return "no match is running";
    if (camera_act(request.kind)) {
        if (state.director)
            return "the view is the director's";
        if (request.kind == PlayKind::camera_zoom) {
            const float zoom = request.absolute_zoom ? request.zoom_target : request.zoom_factor;
            if (!(zoom > 0.0F))
                return "the zoom is not a positive number";
        }
        return {};
    }
    if (commands(request.kind)) {
        if (state.replay)
            return "a replay cannot give orders";
        if (state.watcher)
            return "a watcher cannot give orders";
        if (state.finished)
            return "the match has finished";
        return {};
    }
    const bool clock = clock_request(request.kind);
    const bool pausing = request.kind == PlayKind::pause || request.kind == PlayKind::resume;
    if (state.replay) {
        if (clock)
            return "a replay keeps its own clock";
        return pausing ? "a replay cannot pause the match" : "a replay cannot change the speed";
    }
    if (state.watcher) {
        if (clock)
            return "a watcher cannot change the clock";
        return pausing ? "a watcher cannot pause the match" : "a watcher cannot change the speed";
    }
    if (clock && state.shared_match)
        return "the stepped clock is refused in a shared match";
    if ((pausing || clock_steps(request.kind)) && state.finished)
        return "the match has finished";
    if (clock_steps(request.kind) && state.paused)
        return "the match is paused";
    if (clock_steps(request.kind) && !state.stepped)
        return "the clock is not stepped";
    if (request.kind == PlayKind::speed) {
        const int64_t slowest = state.slowest;
        const int64_t fastest = state.fastest;
        // The match's speed field holds 0..65535. A wider number cannot be set.
        if (request.speed < 0 || request.speed > 65535 || request.speed < slowest ||
            request.speed > fastest)
            return "the speed is outside the match's range";
    }
    return {};
}

/// The answer's JSON once a change has been made: the tick, and the speed, the pause or the view.
///
/// @param request what was asked
/// @param state the match after the change
/// @param shown the view after a camera change; null for the speed and the pause
/// @return the object
std::string
landed_text(const PlayRequest& request, const PlayExtensionState& state, const PlayView* shown) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("applied_tick");
    writer.integer(static_cast<int64_t>(state.tick));
    if (request.kind == PlayKind::speed) {
        writer.key("speed");
        writer.integer(state.speed);
    } else if (!camera_act(request.kind)) {
        writer.key("paused");
        writer.boolean(state.paused);
    } else if (shown != nullptr) {
        write_place(writer, *shown);
    }
    writer.end_object();
    return writer.text();
}

/// The log's words for a change: the tool and what it set.
///
/// @param request what was asked
/// @param shown the view after a camera change
/// @return the words after the tick
std::string landed_detail(const PlayRequest& request, const PlayView& shown) {
    if (request.kind == PlayKind::speed)
        return "game.speed " + std::to_string(request.speed);
    if (request.kind == PlayKind::pause)
        return "game.pause paused";
    if (request.kind == PlayKind::resume)
        return "game.pause resumed";
    if (request.kind == PlayKind::clock_mode)
        return std::string("clock.mode ") + (request.mode == 1 ? "stepped" : "realtime");
    if (request.kind == PlayKind::clock_step)
        return "clock.step " + std::to_string(request.ticks);
    if (request.kind == PlayKind::clock_run) {
        if (request.until == ClockUntil::tick)
            return "clock.run tick " + std::to_string(request.until_tick);
        return "clock.run event " + std::to_string(request.ticks);
    }
    if (request.kind == PlayKind::camera_set)
        return "camera.set " + std::to_string(shown.x) + " " + std::to_string(shown.z);
    if (request.kind == PlayKind::camera_center_unit)
        return "camera.center unit " + std::to_string(request.unit);
    if (request.kind == PlayKind::camera_center_point) {
        return "camera.center point " + std::to_string(request.place_x) + " " +
               std::to_string(request.place_z);
    }
    return "camera.zoom " + decimal_text(static_cast<double>(shown.zoom));
}

/// Why the host did not make a change that had already been accepted.
///
/// @param request what was asked
/// @return the sentence
std::string unchanged_text(const PlayRequest& request) {
    if (request.kind == PlayKind::speed)
        return "the speed did not change";
    if (request.kind == PlayKind::pause)
        return "the match did not pause";
    if (request.kind == PlayKind::resume)
        return "the match did not resume";
    if (request.kind == PlayKind::clock_mode)
        return "the clock did not change";
    if (clock_steps(request.kind))
        return "the clock did not step";
    if (request.kind == PlayKind::camera_center_unit)
        return "that unit is not on the map";
    if (request.kind == PlayKind::camera_zoom)
        return "the zoom did not change";
    return "the view did not move";
}

/// The answer's JSON once a clock run has finished.
///
/// @param tick the match's tick after the run
/// @param ran how many ticks ran
/// @param game_over the match ended during the run
/// @param stopped why a clock.run stopped; empty for clock.step
/// @return the object
std::string clock_result(uint32_t tick, uint32_t ran, bool game_over, std::string_view stopped) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("tick");
    writer.integer(static_cast<int64_t>(tick));
    writer.key("ran");
    writer.integer(static_cast<int64_t>(ran));
    writer.key("events");
    writer.begin_array();
    if (game_over)
        writer.string("game_over");
    writer.end_array();
    if (!stopped.empty()) {
        writer.key("stopped");
        writer.string(stopped);
    }
    writer.end_object();
    return writer.text();
}

/// Why a finished clock.run says it stopped.
///
/// @param request what was asked
/// @param stop how the host's last batch returned
/// @return tick, max_ticks or event; empty for clock.step
std::string_view stop_word(const PlayRequest& request, oa::app::ClockStop stop) {
    if (request.kind != PlayKind::clock_run)
        return {};
    if (stop == oa::app::ClockStop::event)
        return "event";
    if (stop == oa::app::ClockStop::tick)
        return "tick";
    return "max_ticks";
}

/// The factor zoom_about multiplies by. An absolute zoom is that zoom over the zoom now.
///
/// @param host the host of this frame
/// @param request the zoom that was asked for
/// @return the factor; 1 when the zoom now cannot be read
float zoom_factor_of(const PlayHost& host, const PlayRequest& request) {
    if (!request.absolute_zoom)
        return request.zoom_factor;
    float current = 1.0F;
    if (host.read_view != nullptr) {
        PlayView shown;
        if (host.read_view(host.context, &shown) && shown.zoom > 0.0F)
            current = shown.zoom;
    }
    return request.zoom_target / current;
}

/// Makes one change through the host.
///
/// @param host the host of this frame
/// @param request what was asked
/// @return true when the host made it
bool apply_one(const PlayHost& host, const PlayRequest& request) {
    switch (request.kind) {
    case PlayKind::speed:
        return host.set_speed != nullptr &&
               host.set_speed(host.context, static_cast<uint16_t>(request.speed));
    case PlayKind::pause:
        return host.pause != nullptr && host.pause(host.context);
    case PlayKind::resume:
        return host.resume != nullptr && host.resume(host.context);
    case PlayKind::order:
    case PlayKind::state:
    case PlayKind::select:
        return false;
    case PlayKind::camera_set:
        return host.set_camera != nullptr &&
               host.set_camera(host.context, request.place_x, request.place_z);
    case PlayKind::camera_center_unit:
        return host.center_unit != nullptr && host.center_unit(host.context, request.unit);
    case PlayKind::camera_center_point:
        return host.center_point != nullptr &&
               host.center_point(host.context, request.place_x, request.place_z);
    case PlayKind::camera_zoom:
        return host.zoom_about != nullptr && host.zoom_about(
                                                 host.context,
                                                 zoom_factor_of(host, request),
                                                 request.focus_x,
                                                 request.focus_y,
                                                 request.focus_given
                                             );
    case PlayKind::clock_mode:
    case PlayKind::clock_step:
    case PlayKind::clock_run:
        return false;
    }
    return false;
}

/// The host the bridge names, or an empty one when it names none.
///
/// @param play the bridge
/// @return the host
PlayHost host_now(PlayBridge& play) {
    if (play.host_of == nullptr)
        return {};
    return play.host_of(play.context);
}

/// A member of the arguments that keeps its address when the session moves the held call.
///
/// The arguments object itself does not. It sits inside the held call, and
/// the session moves that call into a new list at the end of every pump.
/// A member of the object's tree is left where it is.
///
/// @param arguments the call's arguments
/// @return the speed or the pause member, or null when the arguments have neither
const void* stable_token(const json::Json* arguments) {
    if (arguments == nullptr)
        return nullptr;
    // The first of these a call carries. A camera zoom names zoom or factor
    // before a focus point, and a centre on a unit names the unit before a place.
    static constexpr std::string_view kMembers[] = {
        "speed", "paused", "units", "ticks", "max_ticks", "mode", "zoom", "factor", "unit", "x"
    };
    for (const std::string_view name : kMembers) {
        const json::Json* member = arguments->find(name);
        if (member != nullptr)
            return member;
    }
    return nullptr;
}

/// One member of the call's arguments.
///
/// @param call the poll and the arguments
/// @param name the member
/// @return the member, or null when the arguments have none
const json::Json* member_of(const ToolCall& call, std::string_view name) {
    if (call.arguments == nullptr)
        return nullptr;
    return call.arguments->find(name);
}

/// A member's whole number, when it is one that fits in the map's coordinates.
///
/// @param value the member
/// @return the number, or nothing when it is missing or does not fit
std::optional<int32_t> place_of(const json::Json* value) {
    const std::optional<int64_t> whole = value != nullptr ? value->integer() : std::nullopt;
    if (!whole)
        return std::nullopt;
    const auto least = static_cast<int64_t>(std::numeric_limits<int32_t>::min());
    const auto most = static_cast<int64_t>(std::numeric_limits<int32_t>::max());
    if (*whole < least || *whole > most)
        return std::nullopt;
    return static_cast<int32_t>(*whole);
}

/// A member's number, fraction and all.
///
/// @param value the member
/// @return the number, or nothing when it is missing or not a number
std::optional<double> decimal_of(const json::Json* value) {
    if (value == nullptr || value->number_text() == nullptr)
        return std::nullopt;
    return parse_decimal(*value->number_text());
}

/// The word a log line uses for an order.
///
/// @param kind the order
/// @return the tool's kind
const char* order_word(oa::app::PlayOrderKind kind) {
    switch (kind) {
    case oa::app::PlayOrderKind::move:
        return "move";
    case oa::app::PlayOrderKind::patrol:
        return "patrol";
    case oa::app::PlayOrderKind::attack_unit:
    case oa::app::PlayOrderKind::attack_ground:
        return "attack";
    case oa::app::PlayOrderKind::guard:
        return "guard";
    case oa::app::PlayOrderKind::repair:
        return "repair";
    case oa::app::PlayOrderKind::reclaim_unit:
    case oa::app::PlayOrderKind::reclaim_ground:
        return "reclaim";
    case oa::app::PlayOrderKind::mobile_build:
    case oa::app::PlayOrderKind::building_build:
        return "build";
    case oa::app::PlayOrderKind::factory:
        return "factory";
    case oa::app::PlayOrderKind::stop:
        return "stop";
    }
    return "move";
}

/// The word a log line uses for a standing order.
///
/// @param kind the standing order
/// @return the tool's state
const char* state_word(oa::app::PlayStateKind kind) {
    switch (kind) {
    case oa::app::PlayStateKind::fire:
        return "fire";
    case oa::app::PlayStateKind::move:
        return "move";
    case oa::app::PlayStateKind::on:
        return "on";
    case oa::app::PlayStateKind::off:
        return "off";
    case oa::app::PlayStateKind::cloak:
        return "cloak";
    }
    return "fire";
}

/// The first unit a call names, or 0 when it names none.
///
/// @param request what was asked
/// @return the unit
uint16_t first_unit(const PlayRequest& request) {
    if (request.units.empty())
        return 0;
    return request.units.front();
}

/// The log's words for an order, a standing order or a selection.
///
/// @param request what was asked
/// @return the words after the tick
std::string command_detail(const PlayRequest& request) {
    const uint16_t id = first_unit(request);
    if (request.kind == PlayKind::state)
        return "orders.state " + std::string(state_word(request.state)) + " " + std::to_string(id);
    if (request.kind == PlayKind::select)
        return "units.select " + std::to_string(id);
    return "orders.issue " + std::string(order_word(request.order)) + " " + std::to_string(id);
}

/// Writes one order in the words units.orders uses.
///
/// @param[in,out] writer the JSON being written
/// @param line the order
void write_line(json::JsonWriter& writer, const oa::app::PlayOrderLine& line) {
    writer.begin_object();
    writer.key("kind");
    writer.string(line.kind);
    writer.key("target");
    writer.integer(line.target);
    writer.key("point");
    writer.begin_array();
    writer.integer(line.x);
    writer.integer(line.y);
    writer.integer(line.z);
    writer.end_array();
    writer.key("parameter_1");
    writer.integer(line.parameter_1);
    writer.key("parameter_2");
    writer.integer(line.parameter_2);
    writer.key("phase");
    writer.integer(line.phase);
    writer.key("issue_tick");
    writer.integer(static_cast<int64_t>(line.issue_tick));
    writer.key("facing");
    writer.integer(line.facing);
    writer.key("build_started");
    writer.boolean(line.build_started);
    writer.key("seen_x");
    writer.integer(line.seen_x);
    writer.key("seen_z");
    writer.integer(line.seen_z);
    writer.key("build");
    writer.string(line.build);
    writer.end_object();
}

/// Writes one queue.
///
/// @param[in,out] writer the JSON being written
/// @param name the member's name
/// @param queue the orders
void write_lines(
    json::JsonWriter& writer,
    std::string_view name,
    const std::vector<oa::app::PlayOrderLine>& queue
) {
    writer.key(name);
    writer.begin_array();
    for (const oa::app::PlayOrderLine& line : queue)
        write_line(writer, line);
    writer.end_array();
}

/// The answer's JSON once an order, a standing order or a selection has been made.
///
/// An order names the tick and each unit's queues. A selection names the tick
/// and the units selected afterwards.
///
/// @param request what was asked
/// @param result what the host did
/// @return the object
std::string command_text(const PlayRequest& request, const oa::app::PlayCommandResult& result) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("applied_tick");
    writer.integer(static_cast<int64_t>(result.tick));
    if (request.kind == PlayKind::select) {
        writer.key("selected");
        writer.begin_array();
        for (uint16_t id : result.selected)
            writer.integer(id);
        writer.end_array();
    } else {
        writer.key("units");
        writer.begin_array();
        for (const oa::app::PlayUnitOrders& unit : result.queues) {
            writer.begin_object();
            writer.key("id");
            writer.integer(unit.id);
            write_lines(writer, "primary", unit.primary);
            write_lines(writer, "secondary", unit.secondary);
            writer.key("primary_truncated");
            writer.boolean(unit.primary_truncated);
            writer.key("secondary_truncated");
            writer.boolean(unit.secondary_truncated);
            writer.end_object();
        }
        writer.end_array();
    }
    writer.end_object();
    return writer.text();
}

/// Answers game.speed or game.pause from the poll the session is on.
///
/// @param play the bridge
/// @param request what the call asked for
/// @param call the poll and the arguments
/// @return the answer
ToolReply answer_play(PlayBridge& play, const PlayRequest& request, const ToolCall& call) {
    const PlayHost host = host_now(play);
    if (call.poll == 0)
        return play.queue.consider(host, request);
    return play.queue.follow(host, request, stable_token(call.arguments), call.poll);
}

/// Answers game.speed.
///
/// @param context the PlayBridge
/// @param call the arguments and the poll
/// @return the answer
ToolReply game_speed(void* context, const ToolCall& call) {
    auto* play = static_cast<PlayBridge*>(context);
    if (play == nullptr)
        return failed_reply("no match is running");
    const json::Json* speed = call.arguments != nullptr ? call.arguments->find("speed") : nullptr;
    const std::optional<int64_t> value = speed != nullptr ? speed->integer() : std::nullopt;
    if (!value)
        return failed_reply("the speed is not a number");
    PlayRequest request;
    request.kind = PlayKind::speed;
    request.speed = *value;
    return answer_play(*play, request, call);
}

/// Answers game.pause.
///
/// @param context the PlayBridge
/// @param call the arguments and the poll
/// @return the answer
ToolReply game_pause(void* context, const ToolCall& call) {
    auto* play = static_cast<PlayBridge*>(context);
    if (play == nullptr)
        return failed_reply("no match is running");
    const json::Json* paused = call.arguments != nullptr ? call.arguments->find("paused") : nullptr;
    const std::optional<bool> value = paused != nullptr ? paused->boolean() : std::nullopt;
    if (!value)
        return failed_reply("paused is not true or false");
    PlayRequest request;
    request.kind = *value ? PlayKind::pause : PlayKind::resume;
    return answer_play(*play, request, call);
}

/// Answers camera.get. The view is read at once, and nothing is queued.
///
/// @param context the PlayBridge
/// @return the place and the zoom, or why there is no view
ToolReply camera_get(void* context, const ToolCall& /*call*/) {
    auto* play = static_cast<PlayBridge*>(context);
    if (play == nullptr)
        return failed_reply("no match is running");
    const PlayHost host = host_now(*play);
    if (host.read_view == nullptr)
        return failed_reply("no match is running");
    PlayView shown;
    if (!host.read_view(host.context, &shown) || !shown.in_match)
        return failed_reply("no match is running");
    json::JsonWriter writer;
    writer.begin_object();
    write_place(writer, shown);
    writer.end_object();
    ToolReply reply;
    reply.text = writer.text();
    return reply;
}

/// Answers camera.set.
///
/// @param context the PlayBridge
/// @param call the arguments and the poll
/// @return the answer
ToolReply camera_set(void* context, const ToolCall& call) {
    auto* play = static_cast<PlayBridge*>(context);
    if (play == nullptr)
        return failed_reply("no match is running");
    const std::optional<int32_t> column = place_of(member_of(call, "x"));
    const std::optional<int32_t> row = place_of(member_of(call, "z"));
    if (!column || !row)
        return failed_reply("the place does not fit in the map's coordinates");
    PlayRequest request;
    request.kind = PlayKind::camera_set;
    request.place_x = *column;
    request.place_z = *row;
    return answer_play(*play, request, call);
}

/// Answers camera.center, on a unit or on a point of the map.
///
/// @param context the PlayBridge
/// @param call the arguments and the poll
/// @return the answer
ToolReply camera_center(void* context, const ToolCall& call) {
    auto* play = static_cast<PlayBridge*>(context);
    if (play == nullptr)
        return failed_reply("no match is running");
    const json::Json* unit_member = member_of(call, "unit");
    const json::Json* column_member = member_of(call, "x");
    const json::Json* row_member = member_of(call, "z");
    const bool named_unit = unit_member != nullptr;
    const bool named_column = column_member != nullptr;
    const bool named_row = row_member != nullptr;
    if (named_unit && (named_column || named_row))
        return failed_reply("name a unit or a point");
    PlayRequest request;
    if (named_unit) {
        const std::optional<int64_t> unit = unit_member->integer();
        if (!unit || *unit < 1 || *unit > 65535)
            return failed_reply("that unit is not on the map");
        request.kind = PlayKind::camera_center_unit;
        request.unit = static_cast<uint16_t>(*unit);
    } else if (named_column || named_row) {
        if (!named_column || !named_row)
            return failed_reply("name both x and z");
        const std::optional<int32_t> column = place_of(column_member);
        const std::optional<int32_t> row = place_of(row_member);
        if (!column || !row)
            return failed_reply("the place does not fit in the map's coordinates");
        request.kind = PlayKind::camera_center_point;
        request.place_x = *column;
        request.place_z = *row;
    } else {
        return failed_reply("name a unit or a point");
    }
    return answer_play(*play, request, call);
}

/// Answers camera.zoom, by an absolute zoom or by a factor.
///
/// @param context the PlayBridge
/// @param call the arguments and the poll
/// @return the answer
ToolReply camera_zoom(void* context, const ToolCall& call) {
    auto* play = static_cast<PlayBridge*>(context);
    if (play == nullptr)
        return failed_reply("no match is running");
    const json::Json* zoom_member = member_of(call, "zoom");
    const json::Json* factor_member = member_of(call, "factor");
    if ((zoom_member == nullptr) == (factor_member == nullptr))
        return failed_reply("name a zoom or a factor");
    const json::Json* amount_member = zoom_member != nullptr ? zoom_member : factor_member;
    const std::optional<double> amount = decimal_of(amount_member);
    if (!amount || !(*amount > 0.0))
        return failed_reply("the zoom is not a positive number");
    const json::Json* column_member = member_of(call, "x");
    const json::Json* row_member = member_of(call, "y");
    if ((column_member == nullptr) != (row_member == nullptr))
        return failed_reply("name both x and y, or neither");
    PlayRequest request;
    request.kind = PlayKind::camera_zoom;
    request.absolute_zoom = zoom_member != nullptr;
    if (request.absolute_zoom)
        request.zoom_target = static_cast<float>(*amount);
    else
        request.zoom_factor = static_cast<float>(*amount);
    if (column_member != nullptr) {
        const std::optional<double> column = decimal_of(column_member);
        const std::optional<double> row = decimal_of(row_member);
        if (!column || !row)
            return failed_reply("the point is not on the battlefield");
        request.focus_given = true;
        request.focus_x = static_cast<float>(*column);
        request.focus_y = static_cast<float>(*row);
    }
    return answer_play(*play, request, call);
}

/// Answers clock.mode.
///
/// @param context the PlayBridge
/// @param call the arguments and the poll
/// @return the answer
ToolReply clock_mode(void* context, const ToolCall& call) {
    auto* play = static_cast<PlayBridge*>(context);
    if (play == nullptr)
        return failed_reply("no match is running");
    const json::Json* mode = call.arguments != nullptr ? call.arguments->find("mode") : nullptr;
    const std::string* text = mode != nullptr ? mode->string() : nullptr;
    if (text == nullptr)
        return failed_reply("mode is not realtime or stepped");
    PlayRequest request;
    request.kind = PlayKind::clock_mode;
    if (*text == "stepped")
        request.mode = 1;
    else if (*text == "realtime")
        request.mode = 0;
    else
        return failed_reply("mode is not realtime or stepped");
    return answer_play(*play, request, call);
}

/// Answers clock.step.
///
/// @param context the PlayBridge
/// @param call the arguments and the poll
/// @return the answer
ToolReply clock_step(void* context, const ToolCall& call) {
    auto* play = static_cast<PlayBridge*>(context);
    if (play == nullptr)
        return failed_reply("no match is running");
    const json::Json* ticks = call.arguments != nullptr ? call.arguments->find("ticks") : nullptr;
    const std::optional<int64_t> value = ticks != nullptr ? ticks->integer() : std::nullopt;
    if (!value)
        return failed_reply("ticks is not a number");
    PlayRequest request;
    request.kind = PlayKind::clock_step;
    request.ticks = *value;
    return answer_play(*play, request, call);
}

/// Answers clock.run.
///
/// until tick needs a tick. The schema cannot say that on its own.
///
/// @param context the PlayBridge
/// @param call the arguments and the poll
/// @return the answer
ToolReply clock_run(void* context, const ToolCall& call) {
    auto* play = static_cast<PlayBridge*>(context);
    if (play == nullptr)
        return failed_reply("no match is running");
    const json::Json* until = call.arguments != nullptr ? call.arguments->find("until") : nullptr;
    const std::string* until_text = until != nullptr ? until->string() : nullptr;
    if (until_text == nullptr)
        return failed_reply("until is not event or tick");
    PlayRequest request;
    request.kind = PlayKind::clock_run;
    if (*until_text == "event")
        request.until = ClockUntil::event;
    else if (*until_text == "tick")
        request.until = ClockUntil::tick;
    else
        return failed_reply("until is not event or tick");
    const json::Json* max_ticks =
        call.arguments != nullptr ? call.arguments->find("max_ticks") : nullptr;
    const std::optional<int64_t> value = max_ticks != nullptr ? max_ticks->integer() : std::nullopt;
    if (!value)
        return failed_reply("max_ticks is not a number");
    request.ticks = *value;
    if (request.until == ClockUntil::tick) {
        const json::Json* tick = call.arguments != nullptr ? call.arguments->find("tick") : nullptr;
        if (tick == nullptr)
            return failed_reply("clock.run until tick needs a tick");
        const std::optional<int64_t> target = tick->integer();
        if (!target)
            return failed_reply("tick is not a number");
        // clock_no_target is reserved for "no tick", so a client cannot ask for it.
        if (*target < 0 || *target >= static_cast<int64_t>(oa::app::clock_no_target))
            return failed_reply("tick is outside the match's ticks");
        request.until_tick = *target;
    }
    return answer_play(*play, request, call);
}

} // namespace

ToolReply ActionQueue::consider(const PlayHost& host, const PlayRequest& request) const {
    if (const std::string why = refusal(host, request); !why.empty())
        return failed_reply(why);
    return held_reply();
}

std::optional<std::size_t> ActionQueue::find_token(const void* token) const {
    for (std::size_t index = 0; index < slots_.size(); ++index)
        if (slots_[index].token == token)
            return index;
    return std::nullopt;
}

ToolReply ActionQueue::take(std::size_t index) {
    if (!slots_[index].done) {
        ToolReply reply;
        reply.held = true;
        if (slots_[index].total > 0) {
            reply.progress = static_cast<int64_t>(slots_[index].ran);
            reply.total = static_cast<int64_t>(slots_[index].total);
            reply.progress_message = "stepping the clock";
        }
        return reply;
    }
    ToolReply reply;
    reply.failed = !slots_[index].ok;
    reply.text = std::move(slots_[index].text);
    slots_.erase(slots_.begin() + static_cast<std::ptrdiff_t>(index));
    return reply;
}

ToolReply ActionQueue::follow(
    const PlayHost& host, const PlayRequest& request, const void* token, int64_t poll
) {
    if (token == nullptr)
        return failed_reply("the change was lost");
    if (const std::optional<std::size_t> found = find_token(token))
        return take(*found);
    if (poll > 1)
        return failed_reply("the change was lost");
    if (const std::string why = refusal(host, request); !why.empty())
        return failed_reply(why);
    if (slots_.size() >= kQueueLimit)
        return failed_reply("too many changes are waiting");
    Slot slot;
    slot.token = token;
    slot.request = request;
    if (clock_steps(request.kind) && request.ticks > 0)
        slot.total = static_cast<uint32_t>(request.ticks);
    slots_.push_back(std::move(slot));
    return held_reply();
}

std::vector<AppliedAct> ActionQueue::apply(const PlayHost& host) {
    std::vector<AppliedAct> landed;
    // A member function's lambda can finish a private slot. The log line is
    // written only when the change is finished, so a clock run that continues
    // next frame is logged once, and an order a stepped clock is holding is
    // logged when the step that runs it lands.
    const auto finish = [&landed](Slot& slot, bool ok, std::string text, uint32_t tick) {
        slot.done = true;
        slot.ok = ok;
        slot.text = std::move(text);
        if (!ok)
            return;
        AppliedAct act;
        act.tick = tick;
        PlayView shown;
        act.detail = landed_detail(slot.request, shown);
        landed.push_back(std::move(act));
    };
    // Asks the host for one order, standing order or selection. A pending
    // result leaves the slot waiting: the stepped clock has not run it yet.
    const auto accept_command = [&](Slot& slot) {
        oa::app::PlayCommandResult result;
        const PlayRequest& asked = slot.request;
        if (asked.kind == PlayKind::order) {
            if (host.issue_orders == nullptr) {
                slot.done = true;
                slot.ok = false;
                slot.text = "the order was not given";
                return;
            }
            // The pointers are into this slot's request and live only for the call.
            oa::app::PlayOrderRequest given;
            given.kind = asked.order;
            given.append = asked.append;
            given.units = asked.units.data();
            given.unit_count = asked.units.size();
            given.target = asked.target;
            given.has_point = asked.has_point;
            given.point = asked.point;
            given.type_name = asked.type_name.empty() ? nullptr : asked.type_name.c_str();
            given.count = asked.count;
            given.facing = asked.facing;
            result = host.issue_orders(host.context, given);
        } else if (asked.kind == PlayKind::state) {
            if (host.set_state == nullptr) {
                slot.done = true;
                slot.ok = false;
                slot.text = "the order was not given";
                return;
            }
            oa::app::PlayStateRequest standing;
            standing.kind = asked.state;
            standing.value = asked.state_value;
            standing.on = asked.state_on;
            standing.units = asked.units.data();
            standing.unit_count = asked.units.size();
            result = host.set_state(host.context, standing);
        } else {
            if (host.select_units == nullptr) {
                slot.done = true;
                slot.ok = false;
                slot.text = "the order was not given";
                return;
            }
            oa::app::PlaySelectRequest selection;
            selection.units = asked.units.data();
            selection.unit_count = asked.units.size();
            selection.add = asked.select_add;
            result = host.select_units(host.context, selection);
        }
        if (result.pending && held_command(asked.kind))
            return;
        slot.done = true;
        slot.ok = result.ok;
        if (!result.ok) {
            slot.text = result.detail.empty() ? "the order was refused" : result.detail;
            return;
        }
        slot.text = command_text(asked, result);
        AppliedAct act;
        act.tick = result.tick;
        act.detail = command_detail(asked);
        landed.push_back(std::move(act));
    };
    // An order queued ahead of a step lands inside that step. Finish it
    // before the step's own line, so the log stays in arrival order.
    const auto release_earlier = [&](std::size_t before) {
        for (std::size_t earlier = 0; earlier < before; ++earlier) {
            Slot& waiting = slots_[earlier];
            if (waiting.done || !held_command(waiting.request.kind))
                continue;
            accept_command(waiting);
        }
    };
    for (std::size_t index = 0; index < slots_.size(); ++index) {
        Slot& slot = slots_[index];
        if (slot.done)
            continue;
        // A clock run that already ticked keeps what it ran when the match
        // then pauses or ends. A change that has run nothing is refused again.
        // An order that was never given is refused here without telling the
        // host. One the clock is already holding has run nothing too, and a
        // refusal now leaves that held copy until the match is torn down.
        if (slot.ran == 0) {
            if (const std::string why = refusal(host, slot.request); !why.empty()) {
                finish(slot, false, why, 0);
                continue;
            }
        }
        if (slot.request.kind == PlayKind::clock_mode) {
            if (host.set_clock_mode == nullptr ||
                !host.set_clock_mode(host.context, slot.request.mode)) {
                finish(slot, false, unchanged_text(slot.request), 0);
                continue;
            }
            PlayExtensionState state;
            if (host.extension_state != nullptr)
                state = host.extension_state(host.context);
            json::JsonWriter writer;
            writer.begin_object();
            writer.key("mode");
            writer.string(slot.request.mode == 1 ? "stepped" : "realtime");
            writer.key("applied_tick");
            writer.integer(static_cast<int64_t>(state.tick));
            writer.end_object();
            finish(slot, true, writer.text(), state.tick);
            continue;
        }
        if (clock_steps(slot.request.kind)) {
            if (host.run_ticks == nullptr) {
                finish(slot, false, "the clock did not step", 0);
                continue;
            }
            if (slot.total == 0 || slot.ran >= slot.total) {
                finish(slot, false, "the clock did not step", 0);
                continue;
            }
            uint32_t batch = slot.total - slot.ran;
            if (batch > kClockTicksPerFrame)
                batch = kClockTicksPerFrame;
            uint32_t until_tick = oa::app::clock_no_target;
            uint8_t until_event = 0;
            if (slot.request.kind == PlayKind::clock_run && slot.request.until == ClockUntil::tick)
                until_tick = static_cast<uint32_t>(slot.request.until_tick);
            if (slot.request.until == ClockUntil::event)
                until_event = 1;
            const oa::app::ClockRun run =
                host.run_ticks(host.context, batch, until_tick, until_event);
            slot.ran += run.ran;
            slot.game_over = slot.game_over || run.game_over;
            slot.stop = run.stop;
            if (run.ran > 0)
                release_earlier(index);
            const bool short_batch = run.ran < batch;
            const bool stopped =
                run.stop == oa::app::ClockStop::tick || run.stop == oa::app::ClockStop::event ||
                run.game_over || run.stop == oa::app::ClockStop::paused ||
                run.stop == oa::app::ClockStop::finished ||
                run.stop == oa::app::ClockStop::refused || short_batch || slot.ran >= slot.total;
            if (!stopped)
                continue;
            if (slot.ran == 0 && !slot.game_over && run.stop != oa::app::ClockStop::tick) {
                std::string why = "the clock did not step";
                if (run.stop == oa::app::ClockStop::paused)
                    why = "the match is paused";
                else if (run.stop == oa::app::ClockStop::finished)
                    why = "the match has finished";
                else if (run.stop == oa::app::ClockStop::refused)
                    why = "the clock is not stepped";
                finish(slot, false, std::move(why), run.tick);
                continue;
            }
            finish(
                slot,
                true,
                clock_result(run.tick, slot.ran, slot.game_over, stop_word(slot.request, run.stop)),
                run.tick
            );
            continue;
        }
        if (commands(slot.request.kind)) {
            accept_command(slot);
            continue;
        }
        if (!apply_one(host, slot.request)) {
            finish(slot, false, unchanged_text(slot.request), 0);
            continue;
        }
        PlayExtensionState state;
        if (host.extension_state != nullptr)
            state = host.extension_state(host.context);
        PlayView shown;
        const PlayView* reported = nullptr;
        if (camera_act(slot.request.kind) && host.read_view != nullptr &&
            host.read_view(host.context, &shown))
            reported = &shown;
        slot.done = true;
        slot.ok = true;
        slot.text = landed_text(slot.request, state, reported);
        AppliedAct act;
        act.tick = state.tick;
        act.detail = landed_detail(slot.request, shown);
        landed.push_back(std::move(act));
    }
    // Leaving the stepped clock, or pausing, gives a held order now. A step
    // cannot run while the pause is on, and the wall clock would otherwise
    // wait a frame.
    bool release_held = true;
    if (host.extension_state != nullptr) {
        const PlayExtensionState state = host.extension_state(host.context);
        release_held = !state.stepped || state.paused;
    }
    if (release_held) {
        for (Slot& slot : slots_) {
            if (slot.done || !held_command(slot.request.kind))
                continue;
            accept_command(slot);
        }
    }
    return landed;
}

bool ActionQueue::waiting() const {
    for (const Slot& slot : slots_)
        if (!slot.done)
            return true;
    return false;
}

void add_play_tools(Session& session, PlayBridge& play) {
    Tool speed;
    speed.name = "game.speed";
    speed.description = "Sets the running match's speed, as the + and - keys do. A replay and a "
                        "watcher are refused. The answer names the tick the change landed on.";
    speed.input_schema = std::string(kSpeedSchema);
    speed.reads = false;
    speed.idempotent = true;
    speed.context = &play;
    speed.call = game_speed;
    session.add_tool(std::move(speed));

    Tool pause;
    pause.name = "game.pause";
    pause.description = "Pauses or resumes the running match, as the Pause key does. A replay, a "
                        "watcher and a finished match are refused. The answer names the tick the "
                        "change landed on.";
    pause.input_schema = std::string(kPauseSchema);
    pause.reads = false;
    pause.idempotent = true;
    pause.context = &play;
    pause.call = game_pause;
    session.add_tool(std::move(pause));

    Tool looked;
    looked.name = "camera.get";
    looked.description = "Reads the view: the camera's place, in map pixels, and its zoom. The "
                         "place is the battlefield's top-left corner, held within the view's "
                         "limits.";
    looked.input_schema = std::string(kCameraGetSchema);
    looked.reads = true;
    looked.context = &play;
    looked.call = camera_get;
    session.add_tool(std::move(looked));

    Tool corner;
    corner.name = "camera.set";
    corner.description =
        "Puts the view's top-left corner at a map pixel. The view's limits hold the place, and "
        "the answer is the place they held. A replay and a watcher may move the view. The "
        "director's view is refused. The answer names the tick the change landed on.";
    corner.input_schema = std::string(kCameraSetSchema);
    corner.reads = false;
    corner.idempotent = true;
    corner.context = &play;
    corner.call = camera_set;
    session.add_tool(std::move(corner));

    Tool centre;
    centre.name = "camera.center";
    centre.description =
        "Centres the view on a unit, or on a point of the map named by x and z in map pixels. "
        "The view's limits hold the place. A replay and a watcher may move the view. The "
        "director's view is refused. The answer names the tick the change landed on.";
    centre.input_schema = std::string(kCameraCenterSchema);
    centre.reads = false;
    centre.idempotent = true;
    centre.context = &play;
    centre.call = camera_center;
    session.add_tool(std::move(centre));

    Tool zoom;
    zoom.name = "camera.zoom";
    zoom.description =
        "Zooms the view. zoom is how large the map is drawn, and factor multiplies the zoom it "
        "has. x and y, in canvas pixels, are the point the zoom keeps; without them it zooms "
        "about the middle of the battlefield. The zoom's limits hold the result. The answer "
        "names the tick the change landed on.";
    zoom.input_schema = std::string(kCameraZoomSchema);
    zoom.reads = false;
    zoom.idempotent = false;
    zoom.context = &play;
    zoom.call = camera_zoom;
    session.add_tool(std::move(zoom));

    add_order_tools(session, play);

    Tool mode;
    mode.name = "clock.mode";
    mode.description = "Sets the match clock to realtime or stepped. Stepped runs the match only "
                       "when clock.step or clock.run asks, and frames keep drawing. A replay, a "
                       "watcher and a shared match are refused. The answer names the tick the "
                       "change landed on.";
    mode.input_schema = std::string(kModeSchema);
    mode.reads = false;
    mode.idempotent = true;
    mode.context = &play;
    mode.call = clock_mode;
    session.add_tool(std::move(mode));

    Tool step;
    step.name = "clock.step";
    step.description = "Runs the stepped clock for this many ticks. The match's speed does not "
                       "change how many run. A replay, a watcher, a shared match, a paused match "
                       "and a clock that is not stepped are refused. The answer names the tick "
                       "the run reached.";
    step.input_schema = std::string(kStepSchema);
    step.reads = false;
    step.idempotent = false;
    step.context = &play;
    step.call = clock_step;
    session.add_tool(std::move(step));

    Tool run;
    run.name = "clock.run";
    run.description = "Runs the stepped clock until a tick, or until the match ends, and never "
                      "more than max_ticks. A replay, a watcher, a shared match, a paused match "
                      "and a clock that is not stepped are refused. The answer names the tick it "
                      "reached and why it stopped.";
    run.input_schema = std::string(kRunSchema);
    run.reads = false;
    run.idempotent = false;
    run.context = &play;
    run.call = clock_run;
    session.add_tool(std::move(run));
}

} // namespace oa::app::mcp
