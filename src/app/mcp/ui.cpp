// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The screen tools (ui.hpp). A click, a key or a picture is pushed at the
// pump that receives it and answered on a later poll, so the frame is never
// waited on. The picture of a presented frame is encoded once that frame
// has been shown.
#include "tools.hpp"

#include "oa/app/automation/chords.hpp"
#include "oa/app/automation/drive.hpp"
#include "oa/app/automation/screen.hpp"
#include "oa/app/automation/shots.hpp"
#include "oa/app/automation/view.hpp"
#include "oa/app/automation_host.hpp"
#include "oa/app/check_host.hpp"

#include "events.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app::mcp {
namespace {

namespace automation = oa::app::automation;
namespace json = oa::formats::json;

/// How many events a wait remembers. Older ones are dropped.
constexpr std::size_t kEventCap = 256;

/// How many times a select may press a list's scroll button.
constexpr int kMaxScrolls = 64;

constexpr std::string_view kEmptySchema = "{\"type\":\"object\",\"additionalProperties\":false}";

constexpr std::string_view kClickSchema =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"name\":{\"type\":\"string\"},"
    "\"window\":{\"type\":\"string\"},"
    "\"button\":{\"type\":\"string\",\"enum\":[\"left\",\"right\",\"middle\"]},"
    "\"double\":{\"type\":\"boolean\"},"
    "\"expect_screen\":{\"type\":\"string\"}},"
    "\"required\":[\"name\"]}";

constexpr std::string_view kSelectSchema =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"list\":{\"type\":\"string\"},"
    "\"window\":{\"type\":\"string\"},"
    "\"item\":{\"type\":\"string\"},"
    "\"row\":{\"type\":\"integer\"},"
    "\"double\":{\"type\":\"boolean\"}},"
    "\"required\":[\"list\"]}";

constexpr std::string_view kTypeSchema =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"text\":{\"type\":\"string\"},"
    "\"submit\":{\"type\":\"boolean\"}},"
    "\"required\":[\"text\"]}";

constexpr std::string_view kKeySchema =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"chord\":{\"type\":\"string\"},"
    "\"action\":{\"type\":\"string\"},"
    "\"phase\":{\"type\":\"string\",\"enum\":[\"tap\",\"press\",\"release\"]},"
    "\"repeat\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":30}}}";

constexpr std::string_view kPointerSchema =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"name\":{\"type\":\"string\"},"
    "\"window\":{\"type\":\"string\"},"
    "\"x\":{\"type\":\"integer\"},"
    "\"y\":{\"type\":\"integer\"},"
    "\"space\":{\"type\":\"string\",\"enum\":[\"game\",\"window\"]}}}";

constexpr std::string_view kScrollSchema =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"name\":{\"type\":\"string\"},"
    "\"window\":{\"type\":\"string\"},"
    "\"x\":{\"type\":\"integer\"},"
    "\"y\":{\"type\":\"integer\"},"
    "\"dx\":{\"type\":\"integer\"},"
    "\"dy\":{\"type\":\"integer\"},"
    "\"space\":{\"type\":\"string\",\"enum\":[\"game\",\"window\"]}}}";

constexpr std::string_view kDragSchema =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"from\":{\"type\":\"string\"},"
    "\"to\":{\"type\":\"string\"},"
    "\"window\":{\"type\":\"string\"},"
    "\"x0\":{\"type\":\"integer\"},"
    "\"y0\":{\"type\":\"integer\"},"
    "\"x1\":{\"type\":\"integer\"},"
    "\"y1\":{\"type\":\"integer\"},"
    "\"space\":{\"type\":\"string\",\"enum\":[\"game\",\"window\"]},"
    "\"button\":{\"type\":\"string\",\"enum\":[\"left\",\"right\",\"middle\"]}}}";

constexpr std::string_view kWaitSchema =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"screen\":{\"type\":\"string\"},"
    "\"control\":{\"type\":\"string\"},"
    "\"window\":{\"type\":\"string\"},"
    "\"text\":{\"type\":\"string\"},"
    "\"event\":{\"type\":\"string\"},"
    "\"timeout_ms\":{\"type\":\"integer\",\"minimum\":0,\"maximum\":600000}}}";

constexpr std::string_view kShotSchema =
    "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
    "\"source\":{\"type\":\"string\",\"enum\":[\"presented\",\"composed\"]},"
    "\"space\":{\"type\":\"string\",\"enum\":[\"game\",\"window\"]},"
    "\"region\":{\"type\":\"array\"},"
    "\"scale\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":100},"
    "\"max_width\":{\"type\":\"integer\",\"minimum\":0}}}";

/// What an act is doing.
enum class ActKind : uint8_t { none, click, select, type, key, pointer, scroll, drag };

/// One act, remembered until the game has taken its events.
struct Act {
    bool open{};
    ActKind kind{ActKind::none};
    bool await_row{};  ///< a select still has to click its row
    bool by_item{};    ///< the row was named by its words
    bool twice{};      ///< the click is a double click
    int scrolls{};     ///< the scroll buttons already pressed
    std::string label; ///< the control, the list, or the action id
    std::string item;  ///< the row's words, the chord, or the text
    std::string query; ///< the words a select was asked for
    std::string phase; ///< tap, press or release
    int32_t row{-1};   ///< a select's row
    int32_t at_x{};    ///< where the act lands
    int32_t at_y{};
    int32_t to_x{}; ///< a drag's other end
    int32_t to_y{};
    int32_t wheel_x{}; ///< a scroll's horizontal notches
    int32_t wheel_y{};
    automation::PointSpace space{automation::PointSpace::game};
    automation::PointSpace to_space{automation::PointSpace::game};
    std::string owner; ///< the window the name was asked in; empty is any
};

/// One wait.
struct Wait {
    bool open{};
    std::string shown;    ///< the screen name, empty when not asked
    std::string control;  ///< a visible control, empty when not asked
    std::string words;    ///< a text to find, empty when not asked
    std::string event;    ///< an event kind, empty when not asked
    std::string owner;    ///< the window the control was asked in; empty is any
    uint32_t started{};   ///< SDL_GetTicks when the wait began
    int64_t timeout_ms{}; ///< how long it may run; 0 checks once
    uint64_t since_seq{}; ///< events at or before this do not count
};

/// One event the watch reported.
struct SeenEvent {
    uint64_t seq{};
    std::string kind;
};

/// The check host and the automation host of a frame, both by value.
struct Bound {
    CheckHost check{};
    AutomationHost automation{};
    bool ok{}; ///< both are filled and the screen can be read
};

/// A whole number read from an argument.
struct Whole {
    bool present{}; ///< the argument was given
    bool ok{true};  ///< it is a whole number that fits
    int32_t value{};
};

/// A failed tool answer.
///
/// @param why the reason
/// @return the answer
ToolReply refuse(std::string why) {
    ToolReply reply;
    reply.failed = true;
    reply.text = std::move(why);
    return reply;
}

/// A held answer.
///
/// @param progress how far it has got
/// @param total what that counts toward
/// @param message a short note
/// @return the answer
ToolReply still_working(int64_t progress, int64_t total, std::string message) {
    ToolReply reply;
    reply.held = true;
    reply.progress = progress;
    reply.total = total < 1 ? 1 : total;
    reply.progress_message = std::move(message);
    return reply;
}

/// Reads the running game's hosts.
///
/// @param serve the game
/// @return the hosts; ok is false when the game is not running
Bound bind_game(Serve& serve) {
    Bound bound;
    if (serve.runtime == nullptr)
        return bound;
    bound.automation = automation_host(*serve.runtime);
    bound.check = check_host(*serve.runtime);
    bound.ok = bound.check.screen != nullptr;
    return bound;
}

/// Reads a string argument.
///
/// @param args the arguments
/// @param key the member
/// @return the text; empty when the member is absent or not a string
std::string_view member_text(const json::Json& args, const char* key) {
    const json::Json* value = args.find(key);
    if (value == nullptr || value->string() == nullptr)
        return {};
    return *value->string();
}

/// Reads a boolean argument.
///
/// @param args the arguments
/// @param key the member
/// @return its value; false when it is absent
bool member_flag(const json::Json& args, const char* key) {
    const json::Json* value = args.find(key);
    return value != nullptr && value->boolean().value_or(false);
}

/// Reads a whole number that fits in an int32_t.
///
/// @param args the arguments
/// @param key the member
/// @return whether it was present and in range
Whole read_whole(const json::Json& args, const char* key) {
    Whole read;
    const json::Json* value = args.find(key);
    if (value == nullptr)
        return read;
    read.present = true;
    const std::optional<int64_t> whole = value->integer();
    if (!whole || *whole < std::numeric_limits<int32_t>::min() ||
        *whole > std::numeric_limits<int32_t>::max()) {
        read.ok = false;
        return read;
    }
    read.value = static_cast<int32_t>(*whole);
    return read;
}

/// Compares two texts ignoring ASCII case.
///
/// @param one one text
/// @param other the other
/// @return true when they match aside from ASCII case
bool same_ascii(std::string_view one, std::string_view other) noexcept {
    if (one.size() != other.size())
        return false;
    for (std::size_t at = 0; at < one.size(); ++at) {
        unsigned char first = static_cast<unsigned char>(one[at]);
        unsigned char second = static_cast<unsigned char>(other[at]);
        if (first >= 'A' && first <= 'Z')
            first = static_cast<unsigned char>(first - 'A' + 'a');
        if (second >= 'A' && second <= 'Z')
            second = static_cast<unsigned char>(second - 'A' + 'a');
        if (first != second)
            return false;
    }
    return true;
}

/// The name a point's space is given.
///
/// @param space the space
/// @return window or game
const char* space_name(automation::PointSpace space) noexcept {
    return space == automation::PointSpace::window ? "window" : "game";
}

/// Reads a space argument. Anything but window is the game's space.
///
/// @param word the argument
/// @return the space
automation::PointSpace point_space(std::string_view word) noexcept {
    return word == "window" ? automation::PointSpace::window : automation::PointSpace::game;
}

/// Reads a button argument. Anything but right or middle is the left button.
///
/// @param word the argument
/// @return the button
automation::PointerButton pointer_button(std::string_view word) noexcept {
    if (word == "right")
        return automation::PointerButton::right;
    if (word == "middle")
        return automation::PointerButton::middle;
    return automation::PointerButton::left;
}

/// Finds a control by name, and by window when one was asked.
///
/// An empty window keeps the old search: the exact name, then the name
/// ignoring ASCII case, anywhere. A window asks for that name on that
/// window: the exact pair, then the pair ignoring ASCII case.
///
/// @param listing the listing
/// @param wanted the name
/// @param owner the window; empty is any
/// @return the control, or null
const automation::ListedControl* match_control(
    const automation::ControlListing& listing, std::string_view wanted, std::string_view owner
) {
    if (owner.empty())
        return automation::find_named(listing, wanted);
    const automation::ListedControl* folded = nullptr;
    for (const automation::ListedControl& listed : listing.flat) {
        if (listed.control.name == wanted && listed.control.window == owner)
            return &listed;
        if (folded == nullptr && same_ascii(listed.control.name, wanted) &&
            same_ascii(listed.control.window, owner))
            folded = &listed;
    }
    return folded;
}

/// Finds a visible control by name and copies it out of the listing.
///
/// @param listing the listing
/// @param wanted the name
/// @param owner the window; empty is any
/// @param[out] why why it was not found
/// @return the control, or nothing
std::optional<AutomationControl> shown_control(
    const automation::ControlListing& listing,
    std::string_view wanted,
    std::string_view owner,
    std::string& why
) {
    const automation::ListedControl* found = match_control(listing, wanted, owner);
    if (found == nullptr) {
        why = "no control by that name";
        return std::nullopt;
    }
    if (!found->control.visible) {
        why = found->control.name + " is not shown";
        return std::nullopt;
    }
    return found->control;
}

/// Finds a visible control by name, on any window.
///
/// @param listing the listing
/// @param wanted the name
/// @param[out] why why it was not found
/// @return the control, or nothing
std::optional<AutomationControl> shown_control(
    const automation::ControlListing& listing, std::string_view wanted, std::string& why
) {
    return shown_control(listing, wanted, {}, why);
}

/// Finds a list's scroll button.
///
/// The list's own name is tried as the window first, then the list's
/// window, then the name alone. A game list's buttons keep the name-only
/// match.
///
/// @param listing the listing
/// @param row_list the list
/// @param scroller the button's name
/// @param[out] why why it was not found
/// @return the button, or nothing
std::optional<AutomationControl> scroll_knob(
    const automation::ControlListing& listing,
    const AutomationControl& row_list,
    std::string_view scroller,
    std::string& why
) {
    const auto take =
        [&why](const automation::ListedControl* found) -> std::optional<AutomationControl> {
        if (found == nullptr)
            return std::nullopt;
        if (!found->control.visible) {
            why = found->control.name + " is not shown";
            return std::nullopt;
        }
        return found->control;
    };
    if (const std::optional<AutomationControl> knob =
            take(match_control(listing, scroller, row_list.name)))
        return knob;
    if (!why.empty())
        return std::nullopt;
    if (!row_list.window.empty() && row_list.window != row_list.name) {
        if (const std::optional<AutomationControl> knob =
                take(match_control(listing, scroller, row_list.window)))
            return knob;
        if (!why.empty())
            return std::nullopt;
    }
    return shown_control(listing, scroller, why);
}

/// The centre of a control.
///
/// @param control the control
/// @param[out] why set when the control has no area
/// @return the point
std::optional<automation::ClickPoint>
centre_of(const AutomationControl& control, std::string& why) {
    std::optional<automation::ClickPoint> point = automation::control_centre(control);
    if (!point)
        why = control.name + " has no area";
    return point;
}

/// Tells whether a row is on the page a list is showing.
///
/// @param row_list the list
/// @param row the row
/// @return true when a click can land on it
bool on_page(const AutomationControl& row_list, int32_t row) noexcept {
    return row_list.rows > 0 && row >= row_list.first_visible &&
           row < row_list.first_visible + row_list.rows;
}

/// Finds a list row by its words: exact words, exact keys, then either
/// ignoring ASCII case.
///
/// @param row_list the list
/// @param query the words
/// @param[out] why set when no row matches
/// @return the row, or nothing
std::optional<int32_t>
match_item(const AutomationControl& row_list, std::string_view query, std::string& why) {
    const auto exact = [&query](const std::vector<std::string>& words) -> std::optional<int32_t> {
        for (std::size_t at = 0; at < words.size(); ++at)
            if (words[at] == query)
                return static_cast<int32_t>(at);
        return std::nullopt;
    };
    const auto folded = [&query](const std::vector<std::string>& words) -> std::optional<int32_t> {
        for (std::size_t at = 0; at < words.size(); ++at)
            if (same_ascii(words[at], query))
                return static_cast<int32_t>(at);
        return std::nullopt;
    };
    if (const std::optional<int32_t> found = exact(row_list.items))
        return found;
    if (const std::optional<int32_t> found = exact(row_list.item_keys))
        return found;
    if (const std::optional<int32_t> found = folded(row_list.items))
        return found;
    if (const std::optional<int32_t> found = folded(row_list.item_keys))
        return found;
    why = "no such row";
    return std::nullopt;
}

/// Appends a click at a point: the pointer moves, then the button goes down
/// and up. A double click does that twice, counting 1 and then 2.
///
/// @param[out] events the events
/// @param point where the click lands
/// @param button the button
/// @param twice a double click
void press_at(
    std::vector<automation::InputEvent>& events,
    const automation::ClickPoint& point,
    automation::PointerButton button,
    bool twice
) {
    automation::InputEvent move;
    move.kind = automation::InputKind::pointer_move;
    move.has_point = true;
    move.x = point.x;
    move.y = point.y;
    move.space = point.space;
    events.push_back(move);
    const uint8_t times = twice ? 2 : 1;
    for (uint8_t presses = 1; presses <= times; ++presses) {
        automation::InputEvent down;
        down.kind = automation::InputKind::button_down;
        down.has_point = true;
        down.x = point.x;
        down.y = point.y;
        down.space = point.space;
        down.button = button;
        down.clicks = presses;
        events.push_back(down);
        automation::InputEvent up = down;
        up.kind = automation::InputKind::button_up;
        events.push_back(std::move(up));
    }
}

/// Writes a point as [x, y].
///
/// @param[in,out] writer the JSON being written
/// @param key the member
/// @param x the column
/// @param y the row
void put_point(json::JsonWriter& writer, const char* key, int32_t x, int32_t y) {
    writer.key(key);
    writer.begin_array();
    writer.integer(x);
    writer.integer(y);
    writer.end_array();
}

/// Tells whether a visible control's text or one of its list rows contains
/// a text.
///
/// @param listing the listing
/// @param words the text
/// @return true when one does
bool contains_words(const automation::ControlListing& listing, std::string_view words) {
    for (const automation::ListedControl& listed : listing.flat) {
        if (!listed.control.visible)
            continue;
        if (listed.control.text.find(words) != std::string::npos)
            return true;
        for (const std::string& item : listed.control.items)
            if (item.find(words) != std::string::npos)
                return true;
    }
    return false;
}

/// Drops an open act.
///
/// @param context the Serve
void cancel_act_call(void* context) {
    auto* serve = static_cast<Serve*>(context);
    if (serve == nullptr || serve->runtime == nullptr) {
        if (serve != nullptr)
            serve->ui.cancel_act(nullptr, nullptr);
        return;
    }
    const CheckHost check = check_host(*serve->runtime);
    const AutomationHost automation = automation_host(*serve->runtime);
    serve->ui.cancel_act(&check, &automation);
}

/// Drops an open wait.
///
/// @param context the Serve
void cancel_wait_call(void* context) {
    if (auto* serve = static_cast<Serve*>(context))
        serve->ui.cancel_wait();
}

/// Drops a picture.
///
/// @param context the Serve
void cancel_shot_call(void* context) {
    auto* serve = static_cast<Serve*>(context);
    if (serve == nullptr || serve->runtime == nullptr) {
        if (serve != nullptr)
            serve->ui.cancel_shot(nullptr);
        return;
    }
    const AutomationHost automation = automation_host(*serve->runtime);
    serve->ui.cancel_shot(&automation);
}

ToolReply call_screen(void* context, const ToolCall& call) {
    return static_cast<Serve*>(context)->ui.on_screen(*static_cast<Serve*>(context), call);
}

ToolReply call_tree(void* context, const ToolCall& call) {
    return static_cast<Serve*>(context)->ui.on_tree(*static_cast<Serve*>(context), call);
}

ToolReply call_click(void* context, const ToolCall& call) {
    return static_cast<Serve*>(context)->ui.on_click(*static_cast<Serve*>(context), call);
}

ToolReply call_select(void* context, const ToolCall& call) {
    return static_cast<Serve*>(context)->ui.on_select(*static_cast<Serve*>(context), call);
}

ToolReply call_type(void* context, const ToolCall& call) {
    return static_cast<Serve*>(context)->ui.on_type(*static_cast<Serve*>(context), call);
}

ToolReply call_key(void* context, const ToolCall& call) {
    return static_cast<Serve*>(context)->ui.on_key(*static_cast<Serve*>(context), call);
}

ToolReply call_pointer(void* context, const ToolCall& call) {
    return static_cast<Serve*>(context)->ui.on_pointer(*static_cast<Serve*>(context), call);
}

ToolReply call_scroll(void* context, const ToolCall& call) {
    return static_cast<Serve*>(context)->ui.on_scroll(*static_cast<Serve*>(context), call);
}

ToolReply call_drag(void* context, const ToolCall& call) {
    return static_cast<Serve*>(context)->ui.on_drag(*static_cast<Serve*>(context), call);
}

ToolReply call_wait(void* context, const ToolCall& call) {
    return static_cast<Serve*>(context)->ui.on_wait(*static_cast<Serve*>(context), call);
}

ToolReply call_shot(void* context, const ToolCall& call) {
    return static_cast<Serve*>(context)->ui.on_shot(*static_cast<Serve*>(context), call);
}

/// Adds one tool.
///
/// @param[in,out] session the session
/// @param serve the game, kept as the tool's context
/// @param tool the tool, apart from its context
void offer(Session& session, Serve& serve, Tool tool) {
    tool.context = &serve;
    session.add_tool(std::move(tool));
}

} // namespace

struct UiState::State {
    automation::DriveInput drive;
    automation::ShotBuffer shot;
    automation::ShotRequest shot_ask;
    bool shot_open{};
    automation::EventWatch watch;
    std::deque<SeenEvent> events;
    uint64_t next_event{1};
    std::string pending_kind;
    Act act{};
    Wait wait;
};

UiState::UiState() : state_(std::make_unique<State>()) {
}

UiState::~UiState() = default;
UiState::UiState(UiState&&) noexcept = default;
UiState& UiState::operator=(UiState&&) noexcept = default;

void UiState::before_pump(const CheckHost& check, const AutomationHost& automation) {
    if (state_ == nullptr || check.screen == nullptr)
        return;
    const std::string shown = automation::screen_name(check.screen(check.context));
    const bool dialogs = automation::screen_event_due(state_->watch, shown);
    automation::Observed now;
    // events.read keeps every kind, so this feed always reads the focus.
    automation::fill_observed(check, automation, now, dialogs, true);
    automation::EventHooks hooks;
    hooks.context = state_.get();
    hooks.wanted = [](void*, uint32_t) { return true; };
    hooks.begin = [](void* context, std::string_view kind) {
        auto* held = static_cast<State*>(context);
        held->pending_kind = std::string(kind);
        json::JsonWriter writer;
        writer.begin_object();
        return writer;
    };
    hooks.send = [](void* context, json::JsonWriter& event) {
        event.end_object();
        auto* held = static_cast<State*>(context);
        if (held->events.size() == kEventCap)
            held->events.pop_front();
        SeenEvent seen;
        seen.seq = held->next_event;
        seen.kind = std::move(held->pending_kind);
        ++held->next_event;
        held->events.push_back(std::move(seen));
    };
    automation::watch_events(state_->watch, now, hooks);
}

void UiState::cancel_act(const CheckHost* check, const AutomationHost* automation) {
    if (state_ == nullptr)
        return;
    if (check != nullptr && automation != nullptr)
        state_->drive.release(*check, *automation);
    else
        state_->drive.finish();
    state_->act = {};
}

void UiState::cancel_wait() noexcept {
    if (state_ != nullptr)
        state_->wait.open = false;
}

void UiState::cancel_shot(const AutomationHost* automation) {
    if (state_ == nullptr)
        return;
    if (automation != nullptr)
        state_->shot.abandon(*automation);
    state_->shot_open = false;
}

ToolReply UiState::on_screen(Serve& serve, const ToolCall& /*call*/) {
    const Bound bound = bind_game(serve);
    if (!bound.ok)
        return refuse("the game is not running");
    ToolReply reply;
    reply.text = automation::screen_object(bound.check, bound.automation);
    return reply;
}

ToolReply UiState::on_tree(Serve& serve, const ToolCall& /*call*/) {
    const Bound bound = bind_game(serve);
    if (!bound.ok)
        return refuse("the game is not running");
    ToolReply reply;
    reply.text = automation::tree_object(bound.check, bound.automation);
    return reply;
}

ToolReply UiState::push_act(
    Serve& serve,
    const CheckHost& check,
    const AutomationHost& automation,
    const std::vector<automation::InputEvent>& events,
    std::string_view expect_screen
) {
    if (const std::optional<automation::DriveFailure> failure =
            state_->drive.push(check, automation, events, expect_screen, serve.frame)) {
        state_->act = {};
        return refuse(failure->message);
    }
    state_->act.open = true;
    return still_working(0, 2, "waiting for the game");
}

ToolReply UiState::finish_act(const CheckHost& check, const AutomationHost& automation) {
    json::JsonWriter writer;
    writer.begin_object();
    const Act& act = state_->act;
    switch (act.kind) {
    case ActKind::click:
        writer.key("clicked");
        writer.string(act.label);
        put_point(writer, "at", act.at_x, act.at_y);
        writer.key("space");
        writer.string(space_name(act.space));
        break;
    case ActKind::select:
        writer.key("list");
        writer.string(act.label);
        writer.key("item");
        writer.string(act.item);
        writer.key("row");
        writer.integer(act.row);
        break;
    case ActKind::type:
        writer.key("typed");
        writer.string(act.item);
        break;
    case ActKind::key:
        writer.key("chord");
        writer.string(act.item);
        writer.key("phase");
        writer.string(act.phase);
        if (!act.label.empty()) {
            writer.key("action");
            writer.string(act.label);
        }
        break;
    case ActKind::pointer:
        if (!act.label.empty()) {
            writer.key("name");
            writer.string(act.label);
        }
        put_point(writer, "at", act.at_x, act.at_y);
        writer.key("space");
        writer.string(space_name(act.space));
        break;
    case ActKind::scroll:
        if (!act.label.empty()) {
            writer.key("name");
            writer.string(act.label);
        }
        put_point(writer, "at", act.at_x, act.at_y);
        writer.key("dx");
        writer.integer(act.wheel_x);
        writer.key("dy");
        writer.integer(act.wheel_y);
        writer.key("space");
        writer.string(space_name(act.space));
        break;
    case ActKind::drag:
        put_point(writer, "from", act.at_x, act.at_y);
        put_point(writer, "to", act.to_x, act.to_y);
        writer.key("from_space");
        writer.string(space_name(act.space));
        writer.key("to_space");
        writer.string(space_name(act.to_space));
        break;
    case ActKind::none:
        break;
    }
    writer.key("screen");
    writer.raw(automation::screen_object(check, automation));
    writer.end_object();
    state_->drive.finish();
    state_->act = {};
    ToolReply reply;
    reply.text = writer.text();
    return reply;
}

ToolReply UiState::aim_row(Serve& serve, const CheckHost& check, const AutomationHost& automation) {
    const automation::ControlListing listing = automation::listing_of(check, automation);
    std::string why;
    const std::optional<AutomationControl> row_list =
        shown_control(listing, state_->act.label, state_->act.owner, why);
    if (!row_list) {
        state_->act = {};
        return refuse(std::move(why));
    }
    if (state_->act.by_item) {
        const std::optional<int32_t> row_at = match_item(*row_list, state_->act.query, why);
        if (!row_at) {
            state_->act = {};
            return refuse(std::move(why));
        }
        state_->act.row = *row_at;
        if (*row_at >= 0 && static_cast<std::size_t>(*row_at) < row_list->items.size())
            state_->act.item = row_list->items[static_cast<std::size_t>(*row_at)];
        else
            state_->act.item = state_->act.query;
    }
    if (on_page(*row_list, state_->act.row)) {
        const std::optional<automation::ClickPoint> point =
            automation::list_row_point(*row_list, state_->act.row);
        if (!point) {
            state_->act = {};
            return refuse("the row has no place");
        }
        state_->act.await_row = false;
        state_->act.at_x = point->x;
        state_->act.at_y = point->y;
        state_->act.space = point->space;
        std::vector<automation::InputEvent> events;
        press_at(events, *point, automation::PointerButton::left, state_->act.twice);
        return push_act(serve, check, automation, events, {});
    }
    if (state_->act.scrolls >= kMaxScrolls) {
        state_->act = {};
        return refuse("the row did not come onto the page");
    }
    const std::string& scroller =
        state_->act.row < row_list->first_visible ? row_list->scroll_up : row_list->scroll_down;
    if (scroller.empty()) {
        state_->act = {};
        return refuse("the row is off the page");
    }
    const std::optional<AutomationControl> knob = scroll_knob(listing, *row_list, scroller, why);
    if (!knob) {
        state_->act = {};
        return refuse(std::move(why));
    }
    const std::optional<automation::ClickPoint> point = centre_of(*knob, why);
    if (!point) {
        state_->act = {};
        return refuse(std::move(why));
    }
    ++state_->act.scrolls;
    state_->act.await_row = true;
    std::vector<automation::InputEvent> events;
    press_at(events, *point, automation::PointerButton::left, false);
    return push_act(serve, check, automation, events, {});
}

ToolReply UiState::poll_act(Serve& serve, const ToolCall& call) {
    if (state_ == nullptr || !state_->act.open)
        return refuse("the input was cancelled");
    if (serve.stage != ServeStage::pump || !state_->drive.taken(serve.frame))
        return still_working(call.poll, 2, "waiting for the game");
    const Bound bound = bind_game(serve);
    if (!bound.ok)
        return refuse("the game is not running");
    if (state_->act.kind == ActKind::select && state_->act.await_row) {
        state_->drive.finish();
        return aim_row(serve, bound.check, bound.automation);
    }
    return finish_act(bound.check, bound.automation);
}

ToolReply UiState::on_click(Serve& serve, const ToolCall& call) {
    if (call.poll > 0)
        return poll_act(serve, call);
    if (state_->act.open || state_->drive.pending())
        return refuse("the last input has not been taken");
    const Bound bound = bind_game(serve);
    if (!bound.ok)
        return refuse("the game is not running");
    const json::Json& args = *call.arguments;
    const std::string_view wanted = member_text(args, "name");
    const std::string_view owner = member_text(args, "window");
    const automation::ControlListing listing =
        automation::listing_of(bound.check, bound.automation);
    std::string why;
    const std::optional<AutomationControl> control = shown_control(listing, wanted, owner, why);
    if (!control)
        return refuse(std::move(why));
    const std::optional<automation::ClickPoint> point = centre_of(*control, why);
    if (!point)
        return refuse(std::move(why));
    state_->act = {};
    state_->act.kind = ActKind::click;
    state_->act.label = control->name;
    state_->act.owner = std::string(owner);
    state_->act.at_x = point->x;
    state_->act.at_y = point->y;
    state_->act.space = point->space;
    std::vector<automation::InputEvent> events;
    press_at(
        events, *point, pointer_button(member_text(args, "button")), member_flag(args, "double")
    );
    return push_act(
        serve, bound.check, bound.automation, events, member_text(args, "expect_screen")
    );
}

ToolReply UiState::on_select(Serve& serve, const ToolCall& call) {
    if (call.poll > 0)
        return poll_act(serve, call);
    if (state_->act.open || state_->drive.pending())
        return refuse("the last input has not been taken");
    const Bound bound = bind_game(serve);
    if (!bound.ok)
        return refuse("the game is not running");
    const json::Json& args = *call.arguments;
    const bool has_item = args.find("item") != nullptr;
    const bool has_row = args.find("row") != nullptr;
    if (has_item == has_row)
        return refuse("give an item or a row");
    const automation::ControlListing listing =
        automation::listing_of(bound.check, bound.automation);
    std::string why;
    const std::string_view owner = member_text(args, "window");
    const std::optional<AutomationControl> row_list =
        shown_control(listing, member_text(args, "list"), owner, why);
    if (!row_list)
        return refuse(std::move(why));
    state_->act = {};
    state_->act.kind = ActKind::select;
    state_->act.label = row_list->name;
    state_->act.owner = std::string(owner);
    state_->act.twice = member_flag(args, "double");
    state_->act.by_item = has_item;
    if (has_item) {
        state_->act.query = std::string(member_text(args, "item"));
        const std::optional<int32_t> row_at = match_item(*row_list, state_->act.query, why);
        if (!row_at)
            return refuse(std::move(why));
        state_->act.row = *row_at;
    } else {
        const Whole row_read = read_whole(args, "row");
        if (!row_read.ok)
            return refuse("row is out of range");
        if (row_read.value < 0 || row_list->items.empty() ||
            static_cast<std::size_t>(row_read.value) >= row_list->items.size())
            return refuse("no such row");
        state_->act.row = row_read.value;
        state_->act.item = row_list->items[static_cast<std::size_t>(row_read.value)];
    }
    return aim_row(serve, bound.check, bound.automation);
}

ToolReply UiState::on_type(Serve& serve, const ToolCall& call) {
    if (call.poll > 0)
        return poll_act(serve, call);
    if (state_->act.open || state_->drive.pending())
        return refuse("the last input has not been taken");
    const Bound bound = bind_game(serve);
    if (!bound.ok)
        return refuse("the game is not running");
    const json::Json& args = *call.arguments;
    const std::string_view words = member_text(args, "text");
    if (words.size() > automation::max_input_text_bytes)
        return refuse("the text is too long");
    if (words.find('\0') != std::string_view::npos)
        return refuse("the text has a NUL");
    std::vector<automation::InputEvent> events;
    automation::InputEvent typed;
    typed.kind = automation::InputKind::text;
    typed.text = std::string(words);
    events.push_back(std::move(typed));
    if (member_flag(args, "submit")) {
        std::string why;
        const std::optional<std::vector<automation::InputEvent>> enter =
            automation::chord_events("Enter", automation::KeyPhase::tap, 1, why);
        if (!enter)
            return refuse(why.empty() ? std::string("the chord has no key") : why);
        events.insert(events.end(), enter->begin(), enter->end());
    }
    state_->act = {};
    state_->act.kind = ActKind::type;
    state_->act.item = std::string(words);
    return push_act(serve, bound.check, bound.automation, events, {});
}

ToolReply UiState::on_key(Serve& serve, const ToolCall& call) {
    if (call.poll > 0)
        return poll_act(serve, call);
    if (state_->act.open || state_->drive.pending())
        return refuse("the last input has not been taken");
    const Bound bound = bind_game(serve);
    if (!bound.ok)
        return refuse("the game is not running");
    const json::Json& args = *call.arguments;
    const bool has_chord = args.find("chord") != nullptr;
    const bool has_action = args.find("action") != nullptr;
    if (has_chord == has_action)
        return refuse(
            has_chord ? "give a chord or an action, not both" : "give a chord or an action"
        );
    std::string_view phase_word = member_text(args, "phase");
    if (phase_word.empty())
        phase_word = "tap";
    const std::optional<automation::KeyPhase> phase = automation::key_phase(phase_word);
    if (!phase)
        return refuse("phase is tap, press or release");
    int repeat = 1;
    if (args.find("repeat") != nullptr) {
        const Whole read = read_whole(args, "repeat");
        if (!read.ok || read.value < 1 || read.value > 30)
            return refuse("repeat is 1 to 30");
        repeat = read.value;
    }
    std::string why;
    std::string chord_used;
    std::string action_id;
    std::optional<std::vector<automation::InputEvent>> events;
    if (has_chord) {
        chord_used = std::string(member_text(args, "chord"));
        events = automation::chord_events(chord_used, *phase, repeat, why);
    } else {
        action_id = std::string(member_text(args, "action"));
        events = automation::action_events(action_id, *phase, repeat, chord_used, why);
    }
    if (!events)
        return refuse(why.empty() ? std::string("chord is missing") : why);
    state_->act = {};
    state_->act.kind = ActKind::key;
    state_->act.label = std::move(action_id);
    state_->act.item = std::move(chord_used);
    state_->act.phase = std::string(phase_word);
    return push_act(serve, bound.check, bound.automation, *events, {});
}

ToolReply UiState::on_pointer(Serve& serve, const ToolCall& call) {
    if (call.poll > 0)
        return poll_act(serve, call);
    if (state_->act.open || state_->drive.pending())
        return refuse("the last input has not been taken");
    const Bound bound = bind_game(serve);
    if (!bound.ok)
        return refuse("the game is not running");
    const json::Json& args = *call.arguments;
    automation::ClickPoint point;
    std::string label;
    const std::string_view wanted = member_text(args, "name");
    if (args.find("name") != nullptr && !wanted.empty()) {
        const automation::ControlListing listing =
            automation::listing_of(bound.check, bound.automation);
        std::string why;
        const std::optional<AutomationControl> control =
            shown_control(listing, wanted, member_text(args, "window"), why);
        if (!control)
            return refuse(std::move(why));
        const std::optional<automation::ClickPoint> centre = centre_of(*control, why);
        if (!centre)
            return refuse(std::move(why));
        point = *centre;
        label = control->name;
    } else {
        const Whole across = read_whole(args, "x");
        const Whole down = read_whole(args, "y");
        if (!across.present || !down.present)
            return refuse("give a control or a point");
        if (!across.ok || !down.ok)
            return refuse("the point is out of range");
        point.x = across.value;
        point.y = down.value;
        point.space = point_space(member_text(args, "space"));
    }
    automation::InputEvent move;
    move.kind = automation::InputKind::pointer_move;
    move.has_point = true;
    move.x = point.x;
    move.y = point.y;
    move.space = point.space;
    state_->act = {};
    state_->act.kind = ActKind::pointer;
    state_->act.label = std::move(label);
    state_->act.at_x = point.x;
    state_->act.at_y = point.y;
    state_->act.space = point.space;
    return push_act(serve, bound.check, bound.automation, {std::move(move)}, {});
}

ToolReply UiState::on_scroll(Serve& serve, const ToolCall& call) {
    if (call.poll > 0)
        return poll_act(serve, call);
    if (state_->act.open || state_->drive.pending())
        return refuse("the last input has not been taken");
    const Bound bound = bind_game(serve);
    if (!bound.ok)
        return refuse("the game is not running");
    const json::Json& args = *call.arguments;
    if (args.find("dx") == nullptr && args.find("dy") == nullptr)
        return refuse("give a wheel turn");
    const Whole dx = read_whole(args, "dx");
    const Whole dy = read_whole(args, "dy");
    if ((dx.present && !dx.ok) || (dy.present && !dy.ok))
        return refuse("the wheel turn is out of range");
    automation::ClickPoint point;
    std::string label;
    const std::string_view wanted = member_text(args, "name");
    if (args.find("name") != nullptr && !wanted.empty()) {
        const automation::ControlListing listing =
            automation::listing_of(bound.check, bound.automation);
        std::string why;
        const std::optional<AutomationControl> control =
            shown_control(listing, wanted, member_text(args, "window"), why);
        if (!control)
            return refuse(std::move(why));
        const std::optional<automation::ClickPoint> centre = centre_of(*control, why);
        if (!centre)
            return refuse(std::move(why));
        point = *centre;
        label = control->name;
    } else {
        const Whole across = read_whole(args, "x");
        const Whole down = read_whole(args, "y");
        if (!across.present || !down.present)
            return refuse("give a control or a point");
        if (!across.ok || !down.ok)
            return refuse("the point is out of range");
        point.x = across.value;
        point.y = down.value;
        point.space = point_space(member_text(args, "space"));
    }
    automation::InputEvent wheel;
    wheel.kind = automation::InputKind::wheel;
    wheel.has_point = true;
    wheel.x = point.x;
    wheel.y = point.y;
    wheel.space = point.space;
    wheel.wheel_x = dx.present ? dx.value : 0;
    wheel.wheel_y = dy.present ? dy.value : 0;
    state_->act = {};
    state_->act.kind = ActKind::scroll;
    state_->act.label = std::move(label);
    state_->act.at_x = point.x;
    state_->act.at_y = point.y;
    state_->act.space = point.space;
    state_->act.wheel_x = wheel.wheel_x;
    state_->act.wheel_y = wheel.wheel_y;
    return push_act(serve, bound.check, bound.automation, {std::move(wheel)}, {});
}

ToolReply UiState::on_drag(Serve& serve, const ToolCall& call) {
    if (call.poll > 0)
        return poll_act(serve, call);
    if (state_->act.open || state_->drive.pending())
        return refuse("the last input has not been taken");
    const Bound bound = bind_game(serve);
    if (!bound.ok)
        return refuse("the game is not running");
    const json::Json& args = *call.arguments;
    const automation::PointerButton button = pointer_button(member_text(args, "button"));
    automation::ClickPoint from;
    automation::ClickPoint to;
    const bool named = args.find("from") != nullptr || args.find("to") != nullptr;
    if (named) {
        if (args.find("from") == nullptr || args.find("to") == nullptr)
            return refuse("give both ends");
        const automation::ControlListing listing =
            automation::listing_of(bound.check, bound.automation);
        std::string why;
        const std::string_view owner = member_text(args, "window");
        const std::optional<AutomationControl> start =
            shown_control(listing, member_text(args, "from"), owner, why);
        if (!start)
            return refuse(std::move(why));
        const std::optional<automation::ClickPoint> start_at = centre_of(*start, why);
        if (!start_at)
            return refuse(std::move(why));
        const std::optional<AutomationControl> end =
            shown_control(listing, member_text(args, "to"), owner, why);
        if (!end)
            return refuse(std::move(why));
        const std::optional<automation::ClickPoint> end_at = centre_of(*end, why);
        if (!end_at)
            return refuse(std::move(why));
        from = *start_at;
        to = *end_at;
    } else {
        const Whole x0 = read_whole(args, "x0");
        const Whole y0 = read_whole(args, "y0");
        const Whole x1 = read_whole(args, "x1");
        const Whole y1 = read_whole(args, "y1");
        if (!x0.present || !y0.present || !x1.present || !y1.present)
            return refuse("give both ends");
        if (!x0.ok || !y0.ok || !x1.ok || !y1.ok)
            return refuse("the point is out of range");
        const automation::PointSpace space = point_space(member_text(args, "space"));
        from = {x0.value, y0.value, space};
        to = {x1.value, y1.value, space};
    }
    std::vector<automation::InputEvent> events;
    automation::InputEvent move;
    move.kind = automation::InputKind::pointer_move;
    move.has_point = true;
    move.x = from.x;
    move.y = from.y;
    move.space = from.space;
    events.push_back(move);
    automation::InputEvent down;
    down.kind = automation::InputKind::button_down;
    down.has_point = true;
    down.x = from.x;
    down.y = from.y;
    down.space = from.space;
    down.button = button;
    down.clicks = 1;
    events.push_back(down);
    automation::InputEvent across = move;
    across.x = to.x;
    across.y = to.y;
    across.space = to.space;
    events.push_back(across);
    automation::InputEvent up = down;
    up.kind = automation::InputKind::button_up;
    up.x = to.x;
    up.y = to.y;
    up.space = to.space;
    events.push_back(up);
    state_->act = {};
    state_->act.kind = ActKind::drag;
    state_->act.at_x = from.x;
    state_->act.at_y = from.y;
    state_->act.to_x = to.x;
    state_->act.to_y = to.y;
    state_->act.space = from.space;
    state_->act.to_space = to.space;
    return push_act(serve, bound.check, bound.automation, events, {});
}

ToolReply UiState::poll_wait(Serve& serve) {
    if (state_ == nullptr || !state_->wait.open)
        return refuse("the wait was cancelled");
    const Bound bound = bind_game(serve);
    if (!bound.ok) {
        state_->wait.open = false;
        return refuse("the game is not running");
    }
    const Wait& waiting = state_->wait;
    bool met = true;
    const std::string current = automation::screen_name(bound.check.screen(bound.check.context));
    if (!waiting.shown.empty() && current != waiting.shown)
        met = false;
    if (met && (!waiting.control.empty() || !waiting.words.empty())) {
        const automation::ControlListing listing =
            automation::listing_of(bound.check, bound.automation);
        if (!waiting.control.empty()) {
            const automation::ListedControl* found =
                match_control(listing, waiting.control, waiting.owner);
            if (found == nullptr || !found->control.visible)
                met = false;
        }
        if (met && !waiting.words.empty() && !contains_words(listing, waiting.words))
            met = false;
    }
    if (met && !waiting.event.empty()) {
        bool arrived = false;
        for (const SeenEvent& seen : state_->events)
            if (seen.seq > waiting.since_seq && seen.kind == waiting.event)
                arrived = true;
        if (!arrived)
            met = false;
    }
    const auto elapsed = static_cast<uint32_t>(SDL_GetTicks()) - waiting.started;
    if (met) {
        state_->wait.open = false;
        json::JsonWriter writer;
        writer.begin_object();
        writer.key("ok");
        writer.boolean(true);
        writer.key("screen");
        writer.raw(automation::screen_object(bound.check, bound.automation));
        writer.end_object();
        ToolReply reply;
        reply.text = writer.text();
        return reply;
    }
    if (static_cast<int64_t>(elapsed) >= waiting.timeout_ms) {
        state_->wait.open = false;
        return refuse("timed out");
    }
    return still_working(static_cast<int64_t>(elapsed), waiting.timeout_ms, "waiting");
}

ToolReply UiState::on_wait(Serve& serve, const ToolCall& call) {
    if (call.poll == 0) {
        if (state_->wait.open)
            return refuse("a wait is already running");
        const json::Json& args = *call.arguments;
        const std::string shown(member_text(args, "screen"));
        const std::string control(member_text(args, "control"));
        const std::string words(member_text(args, "text"));
        const std::string event(member_text(args, "event"));
        if (shown.empty() && control.empty() && words.empty() && event.empty())
            return refuse("give a screen, a control, a text or an event");
        if (!event.empty() && automation::event_kind_named(event) == 0)
            return refuse("no such event");
        int64_t timeout = 30000;
        if (args.find("timeout_ms") != nullptr) {
            const Whole read = read_whole(args, "timeout_ms");
            if (!read.ok || read.value < 0)
                return refuse("timeout_ms is out of range");
            timeout = read.value;
        }
        state_->wait = {};
        state_->wait.shown = shown;
        state_->wait.control = control;
        state_->wait.words = words;
        state_->wait.event = event;
        state_->wait.owner = std::string(member_text(args, "window"));
        state_->wait.started = static_cast<uint32_t>(SDL_GetTicks());
        state_->wait.timeout_ms = timeout;
        state_->wait.since_seq = state_->next_event - 1;
        state_->wait.open = true;
    }
    return poll_wait(serve);
}

ToolReply UiState::poll_shot(Serve& serve) {
    if (state_ == nullptr || !state_->shot_open)
        return refuse("the picture was cancelled");
    const Bound bound = bind_game(serve);
    if (!bound.ok) {
        state_->shot_open = false;
        return refuse("the game is not running");
    }
    const automation::ShotStep step = serve.stage == ServeStage::presented
                                          ? automation::ShotStep::presented
                                          : automation::ShotStep::pump;
    automation::advance_shot(state_->shot, state_->shot_ask, bound.check, bound.automation, step);
    if (state_->shot.failed()) {
        const std::string why = state_->shot.failure();
        state_->shot_open = false;
        return refuse(why);
    }
    if (state_->shot.ready()) {
        json::JsonWriter writer;
        writer.begin_object();
        writer.key("width");
        writer.integer(state_->shot.width());
        writer.key("height");
        writer.integer(state_->shot.height());
        writer.key("source");
        writer.string(state_->shot.source());
        writer.end_object();
        ToolReply reply;
        reply.text = writer.text();
        reply.png = state_->shot.png();
        state_->shot_open = false;
        return reply;
    }
    return still_working(
        static_cast<int64_t>(state_->shot.waited()),
        automation::shot_frame_wait_limit,
        "waiting for the frame"
    );
}

ToolReply UiState::on_shot(Serve& serve, const ToolCall& call) {
    if (call.poll > 0)
        return poll_shot(serve);
    if (state_->shot_open || state_->shot.waiting())
        return refuse("a picture is already being taken");
    const Bound bound = bind_game(serve);
    if (!bound.ok)
        return refuse("the game is not running");
    const json::Json& args = *call.arguments;
    automation::ShotRequest ask;
    const std::string_view source = member_text(args, "source");
    ask.presented = source != "composed";
    ask.window_space = member_text(args, "space") == "window";
    if (const json::Json* region = args.find("region")) {
        if (region->type() != json::JsonType::array || region->elements().size() != 4)
            return refuse("region is x, y, width and height");
        int32_t parts[4] = {};
        const auto elements = region->elements();
        for (std::size_t at = 0; at < 4; ++at) {
            const std::optional<int64_t> whole = elements[at].integer();
            if (!whole || *whole < std::numeric_limits<int32_t>::min() ||
                *whole > std::numeric_limits<int32_t>::max())
                return refuse("region is x, y, width and height");
            parts[at] = static_cast<int32_t>(*whole);
        }
        ask.has_region = true;
        ask.region_x = parts[0];
        ask.region_y = parts[1];
        ask.region_w = parts[2];
        ask.region_h = parts[3];
    }
    if (args.find("scale") != nullptr) {
        const Whole read = read_whole(args, "scale");
        if (!read.ok || read.value < 1 || read.value > 100)
            return refuse("scale is 1 to 100");
        ask.scale_percent = read.value;
    }
    if (args.find("max_width") != nullptr) {
        const Whole read = read_whole(args, "max_width");
        if (!read.ok || read.value < 0)
            return refuse("max_width is out of range");
        ask.max_width = read.value;
    }
    state_->shot.abandon(bound.automation);
    state_->shot_ask = ask;
    automation::advance_shot(
        state_->shot, state_->shot_ask, bound.check, bound.automation, automation::ShotStep::pump
    );
    if (state_->shot.failed()) {
        const std::string why = state_->shot.failure();
        state_->shot_open = false;
        return refuse(why);
    }
    state_->shot_open = true;
    return still_working(0, automation::shot_frame_wait_limit, "waiting for the frame");
}

void add_ui_tools(Session& session, Serve& serve) {
    Tool screen;
    screen.name = "ui.screen";
    screen.description = "Reads the screen shown: its name, the dialogs over it, the pointer, "
                         "the camera and the control under the pointer.";
    screen.input_schema = std::string(kEmptySchema);
    screen.reads = true;
    screen.call = call_screen;
    offer(session, serve, std::move(screen));

    Tool tree;
    tree.name = "ui.tree";
    tree.description =
        "Reads the screen's controls as a tree: each control's name, place, parent, "
        "role, label, value and states, with extension windows first, then the screen.";
    tree.input_schema = std::string(kEmptySchema);
    tree.reads = true;
    tree.call = call_tree;
    offer(session, serve, std::move(tree));

    Tool click;
    click.name = "ui.click";
    click.description = "Clicks a control by its name, at the centre of where it is drawn. "
                        "window is the extension window, or the list or slider a scroll button "
                        "belongs to, and picks that control when the name is repeated. "
                        "button is left, right or middle. double clicks twice. expect_screen "
                        "refuses when another screen is shown.";
    click.input_schema = std::string(kClickSchema);
    click.reads = false;
    click.idempotent = false;
    click.call = call_click;
    click.cancel = cancel_act_call;
    offer(session, serve, std::move(click));

    Tool select;
    select.name = "ui.select";
    select.description = "Chooses a row of a list by its words or its index. window names the "
                         "list's extension window when the list name is repeated. A row off the "
                         "page is brought on by the list's scroll buttons, then clicked.";
    select.input_schema = std::string(kSelectSchema);
    select.reads = false;
    select.idempotent = false;
    select.call = call_select;
    select.cancel = cancel_act_call;
    offer(session, serve, std::move(select));

    Tool type;
    type.name = "ui.type";
    type.description = "Types text into the control that has the keyboard. submit presses Enter "
                       "after it. Text is delivered only while the game is taking text.";
    type.input_schema = std::string(kTypeSchema);
    type.reads = false;
    type.idempotent = false;
    type.call = call_type;
    type.cancel = cancel_act_call;
    offer(session, serve, std::move(type));

    Tool key;
    key.name = "ui.key";
    key.description = "Presses a chord, or the key an action id is bound to. phase is tap, press "
                      "or release. The binding is the action's own default, not a player's layout.";
    key.input_schema = std::string(kKeySchema);
    key.reads = false;
    key.idempotent = false;
    key.call = call_key;
    key.cancel = cancel_act_call;
    offer(session, serve, std::move(key));

    Tool pointer;
    pointer.name = "ui.pointer";
    pointer.description = "Moves the pointer onto a control, or to a point on the screen. A name "
                          "is used where one is given. window picks that control when the name "
                          "is repeated.";
    pointer.input_schema = std::string(kPointerSchema);
    pointer.reads = false;
    pointer.idempotent = false;
    pointer.call = call_pointer;
    pointer.cancel = cancel_act_call;
    offer(session, serve, std::move(pointer));

    Tool scroll;
    scroll.name = "ui.scroll";
    scroll.description = "Turns the wheel at a control, or at a point. A name is used where one "
                         "is given. window picks that control when the name is repeated. dx and "
                         "dy are notches, to the right and away from the player.";
    scroll.input_schema = std::string(kScrollSchema);
    scroll.reads = false;
    scroll.idempotent = false;
    scroll.call = call_scroll;
    scroll.cancel = cancel_act_call;
    offer(session, serve, std::move(scroll));

    Tool drag;
    drag.name = "ui.drag";
    drag.description = "Drags from one control to another, or from one point to another. Names "
                       "are used where they are given. window applies to both named ends.";
    drag.input_schema = std::string(kDragSchema);
    drag.reads = false;
    drag.idempotent = false;
    drag.call = call_drag;
    drag.cancel = cancel_act_call;
    offer(session, serve, std::move(drag));

    Tool wait;
    wait.name = "ui.wait";
    wait.description = "Waits until a screen is shown, a control is visible, a text is on screen "
                       "and an event has arrived, whichever of those were asked for. window names "
                       "the control's extension window. event is a match-watch kind. timeout_ms "
                       "is milliseconds, 30000 by default, and 0 checks once. The frame is not "
                       "stopped.";
    wait.input_schema = std::string(kWaitSchema);
    wait.reads = true;
    wait.call = call_wait;
    wait.cancel = cancel_wait_call;
    offer(session, serve, std::move(wait));

    Tool shot;
    shot.name = "capture.screenshot";
    shot.description = "Returns a PNG of the frame the game presented, or of the one it composed. "
                       "region is x, y, width and height. scale is a percent from 1 to 100. "
                       "max_width limits the width. space is game or window, for a presented "
                       "region.";
    shot.input_schema = std::string(kShotSchema);
    shot.reads = true;
    shot.call = call_shot;
    shot.cancel = cancel_shot_call;
    offer(session, serve, std::move(shot));
}

void ui_before_pump(Serve& serve) {
    if (serve.runtime == nullptr)
        return;
    const AutomationHost automation = automation_host(*serve.runtime);
    const CheckHost check = check_host(*serve.runtime);
    if (check.screen == nullptr)
        return;
    serve.ui.before_pump(check, automation);
}

} // namespace oa::app::mcp
