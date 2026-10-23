// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// brain.after, brain.every and brain.cancel. Timers run after the queued
// events and before onTick, in due-tick then sequence order.

#include "match_fixture.hpp"

#include "oa/data/brain_facts.hpp"
#include "oa/sim/brains/check.hpp"
#include "oa/sim/brains/seat.hpp"

#include "lua.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace {

namespace brains = oa::sim::brains;
namespace host = oa::data::script_host;

struct Package {
    std::string id{"steady-commander"};
    std::string version{"1.2"};
    std::string entry{"brain.lua"};
    uint8_t player{computer};
    std::string script;
};

brains::SeatOutcome seat_script(Fixture& fixture, Package& package) {
    std::vector<brains::PackageFile> files;
    files.push_back(
        {package.entry,
         std::span<const uint8_t>(
             reinterpret_cast<const uint8_t*>(package.script.data()), package.script.size()
         )}
    );
    brains::SeatSpec spec;
    spec.player = package.player;
    spec.id = package.id;
    spec.name = "Steady Commander";
    spec.version = package.version;
    spec.entry = package.entry;
    spec.level = "steady";
    spec.difficulty = OA_DIFFICULTY_MEDIUM;
    spec.files = files;
    return brains::seat_brain(*fixture.match, spec);
}

host::Sandbox* seat_ready(Fixture& fixture, Package& package) {
    const brains::SeatOutcome outcome = seat_script(fixture, package);
    CHECK(outcome.result == brains::SeatResult::seated);
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, package.player);
    CHECK(sandbox != nullptr);
    CHECK(!sandbox->failed());
    return sandbox;
}

void advance(Fixture& fixture) {
    ++fixture.match->state().game.tick;
    fixture.match->tick();
}

struct LogRead {
    std::vector<std::string> lines;
};

void read_log(void* context, lua_State* state) {
    auto* held = static_cast<LogRead*>(context);
    lua_getglobal(state, "log");
    if (lua_istable(state, -1) == 0) {
        lua_pop(state, 1);
        return;
    }
    const int count = static_cast<int>(lua_rawlen(state, -1));
    for (int row = 1; row <= count; ++row) {
        lua_rawgeti(state, -1, row);
        std::size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        if (text != nullptr)
            held->lines.emplace_back(text, length);
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
}

LogRead read_lines(host::Sandbox& sandbox) {
    LogRead held;
    host::ScriptError error;
    CHECK(sandbox.run_host("read", &held, read_log, error));
    return held;
}

int line_at(const std::vector<std::string>& lines, const std::string& wanted) {
    for (int row = 0; row < static_cast<int>(lines.size()); ++row) {
        if (lines[static_cast<std::size_t>(row)] == wanted)
            return row;
    }
    return -1;
}

void require_line(const std::vector<std::string>& lines, const std::string& wanted) {
    if (line_at(lines, wanted) >= 0)
        return;
    std::string text;
    for (const std::string& line : lines)
        text += line + "\n";
    throw std::runtime_error(wanted + " missing\n" + text);
}

void require_absent(const std::vector<std::string>& lines, const std::string& wanted) {
    if (line_at(lines, wanted) < 0)
        return;
    throw std::runtime_error(wanted + " was delivered");
}

const char* order_script = R"(local hooks = {}
log = {}
local function note(line)
  log[#log + 1] = line
end
function hooks.fits()
  return true
end
function hooks.start()
  note("start " .. tostring(me.tick))
  local early_b
  local late = brain.after(2, function()
    note("late")
  end)
  local early_a = brain.after(1, function()
    note("early-a")
    note(brain.cancel(early_b) and "cancelled" or "missed")
  end)
  early_b = brain.after(1, function()
    note("early-b")
  end)
  brain.every(2, function()
    note("every " .. tostring(me.tick))
  end)
  note("ids " .. tostring(late) .. " " .. tostring(early_a) .. " " .. tostring(early_b))
end
function hooks.onUnitCreated()
  note("created")
end
function hooks.onTick(tick)
  if tick == 3 then
    local id = brain.after(1, function()
      note("from-tick")
    end)
    note("new-id " .. tostring(id))
  end
  note("tick " .. tostring(tick))
end
return hooks
)";

void timers_run_in_order_between_events_and_on_tick() {
    Fixture fixture;
    Package package;
    package.script = order_script;
    host::Sandbox* sandbox = seat_ready(fixture, package);
    (void)fixture.spawn(computer, collector, 8, 8);
    for (int tick = 0; tick < 6; ++tick)
        advance(fixture);
    const std::vector<std::string> lines = read_lines(*sandbox).lines;
    const std::string wanted[] = {
        "start 0",
        "ids 1 2 3",
        "created",
        "early-a",
        "cancelled",
        "tick 1",
        "late",
        "every 2",
        "tick 2",
        "new-id 5",
        "tick 3",
        "every 4",
        "from-tick",
        "tick 4",
        "tick 5",
        "every 6",
        "tick 6",
    };
    CHECK(lines.size() == sizeof wanted / sizeof wanted[0]);
    for (std::size_t index = 0; index < lines.size(); ++index)
        CHECK(lines[index] == wanted[index]);
    require_absent(lines, "early-b");
    require_line(lines, "cancelled");
    const int created = line_at(lines, "created");
    const int early = line_at(lines, "early-a");
    const int tick = line_at(lines, "tick 1");
    CHECK(created >= 0 && early > created && tick > early);
    const int from_tick = line_at(lines, "from-tick");
    const int tick_3 = line_at(lines, "tick 3");
    const int tick_4 = line_at(lines, "tick 4");
    CHECK(from_tick > tick_3 && from_tick < tick_4);
}

const char* limit_script = R"(local hooks = {}
log = {}
local function note(line)
  log[#log + 1] = line
end
local function capture(label, call)
  local fine, message = pcall(call)
  note(label .. " " .. (fine and "accepted" or message))
end
function hooks.fits()
  return true
end
function hooks.start()
  local ids = {}
  for _ = 1, 1024 do
    ids[#ids + 1] = brain.after(50, function() end)
  end
  capture("full", function()
    brain.after(50, function() end)
  end)
  local removed = brain.cancel(ids[1])
  local again = brain.after(50, function() end)
  note("again " .. tostring(removed) .. " " .. tostring(again))
  capture("every-full", function()
    brain.every(1, function() end)
  end)
  capture("zero", function()
    brain.after(0, function() end)
  end)
  capture("fraction", function()
    brain.after(1.5, function() end)
  end)
  capture("huge", function()
    brain.after(2147483648, function() end)
  end)
  capture("text", function()
    brain.after("1", function() end)
  end)
  capture("nofn", function()
    brain.after(1, 5)
  end)
  capture("every-zero", function()
    brain.every(0, function() end)
  end)
  capture("cancel-text", function()
    brain.cancel("x")
  end)
  note(brain.cancel(999999) and "missing yes" or "missing no")
  local once = brain.cancel(ids[2])
  local twice = brain.cancel(ids[2])
  note("twice " .. tostring(once) .. " " .. tostring(twice))
end
return hooks
)";

bool contains(const std::string& text, const char* needle) {
    return text.find(needle) != std::string::npos;
}

void limits_and_argument_errors() {
    Fixture fixture;
    Package package;
    package.script = limit_script;
    host::Sandbox* sandbox = seat_ready(fixture, package);
    const std::vector<std::string> lines = read_lines(*sandbox).lines;
    require_line(lines, "again true 1025");
    require_line(lines, "missing no");
    require_line(lines, "twice true false");
    bool saw_full = false;
    bool saw_every = false;
    bool saw_zero = false;
    bool saw_fraction = false;
    bool saw_huge = false;
    bool saw_text = false;
    bool saw_nofn = false;
    bool saw_every_zero = false;
    bool saw_cancel = false;
    for (const std::string& line : lines) {
        if (line.rfind("full ", 0) == 0) {
            saw_full = contains(line, "brain.after: a brain may keep at most 1024 timers waiting");
        } else if (line.rfind("every-full ", 0) == 0) {
            saw_every = contains(line, "brain.every: a brain may keep at most 1024 timers waiting");
        } else if (line.rfind("zero ", 0) == 0) {
            saw_zero = contains(line, "brain.after: ticks must be an integer from 1 to 2147483647");
        } else if (line.rfind("fraction ", 0) == 0) {
            saw_fraction =
                contains(line, "brain.after: ticks must be an integer from 1 to 2147483647");
        } else if (line.rfind("huge ", 0) == 0) {
            saw_huge = contains(line, "brain.after: ticks must be an integer from 1 to 2147483647");
        } else if (line.rfind("text ", 0) == 0) {
            saw_text = contains(line, "brain.after: ticks must be an integer from 1 to 2147483647");
        } else if (line.rfind("nofn ", 0) == 0) {
            saw_nofn = contains(line, "brain.after: argument 2 must be a function");
        } else if (line.rfind("every-zero ", 0) == 0) {
            saw_every_zero =
                contains(line, "brain.every: ticks must be an integer from 1 to 2147483647");
        } else if (line.rfind("cancel-text ", 0) == 0) {
            saw_cancel = contains(line, "brain.cancel: argument 1 must be an integer");
        }
    }
    CHECK(saw_full && saw_every && saw_zero && saw_fraction && saw_huge);
    CHECK(saw_text && saw_nofn && saw_every_zero && saw_cancel);
    CHECK(!sandbox->failed());
}

const char* error_script = "local hooks = {}\n"
                           "function hooks.fits()\n"
                           "  return true\n"
                           "end\n"
                           "function hooks.start()\n"
                           "  brain.after(1, function()\n"
                           "    error(\"timer broke\")\n"
                           "  end)\n"
                           "  brain.after(1, function()\n"
                           "    later = true\n"
                           "  end)\n"
                           "end\n"
                           "function hooks.onTick()\n"
                           "  ticked = true\n"
                           "end\n"
                           "return hooks\n";

int source_line(const char* script, const char* needle) {
    const std::string text(script);
    const std::size_t found = text.find(needle);
    CHECK(found != std::string::npos);
    int line = 1;
    for (std::size_t index = 0; index < found; ++index) {
        if (text[index] == '\n')
            ++line;
    }
    return line;
}

void a_timer_error_stops_the_seat() {
    Fixture fixture;
    Package package;
    package.script = error_script;
    host::Sandbox* sandbox = seat_ready(fixture, package);
    advance(fixture);
    CHECK(sandbox->failed());
    CHECK(sandbox->failure().kind == host::ErrorKind::runtime);
    CHECK(sandbox->failure().chunk == "brain.lua");
    CHECK(sandbox->failure().line == source_line(error_script, "error(\"timer broke\")"));
    CHECK(sandbox->failure().message.find("timer broke") != std::string::npos);
    CHECK(brains::brain_seated(*fixture.match, computer));
    advance(fixture);
    CHECK(sandbox->failed());
    CHECK(brains::brain_seated(*fixture.match, computer));
    host::ScriptError error;
    CHECK(!sandbox->run_host("read", nullptr, nullptr, error));
}

const char* check_script = R"(local hooks = {}
function hooks.fits()
  local fine, message = pcall(function()
    brain.after(0, function() end)
  end)
  if fine then
    return "accepted zero"
  end
  if not string.find(message, "brain.after: ticks must be an integer from 1 to 2147483647", 1, true) then
    return message
  end
  local id = brain.every(3, function() end)
  if brain.cancel(id) ~= true or brain.cancel(id) ~= false then
    return "cancel"
  end
  if brain.shared_random ~= nil then
    return "shared"
  end
  return "ok-timers"
end
return hooks
)";

void timers_install_with_no_match() {
    Package package;
    package.player = 0;
    package.script = check_script;
    std::vector<brains::PackageFile> files;
    files.push_back(
        {"brain.lua",
         std::span<const uint8_t>(
             reinterpret_cast<const uint8_t*>(package.script.data()), package.script.size()
         )}
    );
    brains::SeatSpec spec;
    spec.player = 0;
    spec.id = package.id;
    spec.name = "Steady Commander";
    spec.version = package.version;
    spec.entry = package.entry;
    spec.level = "steady";
    spec.difficulty = OA_DIFFICULTY_MEDIUM;
    spec.files = files;
    oa::data::brain_facts::GameFacts facts;
    const brains::SeatOutcome outcome = brains::check_brain(spec, facts);
    CHECK(outcome.result == brains::SeatResult::does_not_fit);
    CHECK(std::string(outcome.reason.data()) == "ok-timers");
}

} // namespace

int main() {
    try {
        timers_run_in_order_between_events_and_on_tick();
        limits_and_argument_errors();
        a_timer_error_stops_the_seat();
        timers_install_with_no_match();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "sim-brains-timers: %s\n", ex.what());
        return 1;
    }
    return 0;
}
