// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Chat delivered at a seated brain's turn, and brain.say. The made-up brain
// is chat-listener. Hooks append one line per call; the test reads that
// table back with run_host.

#include "match_fixture.hpp"

#include "oa/base/game_loop.hpp"
#include "oa/core/game_state.h"
#include "oa/core/player.h"
#include "oa/data/brain_facts.hpp"
#include "oa/sim/brains/chat.hpp"
#include "oa/sim/brains/check.hpp"
#include "oa/sim/brains/seat.hpp"
#include "oa/sim/state_hash.hpp"

#include "lua.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace brains = oa::sim::brains;
namespace host = oa::data::script_host;

constexpr uint32_t chat_run_ticks = 600;

struct Package {
    std::string id{"chat-listener"};
    std::string name{"Chat Listener"};
    std::string version{"1"};
    std::string entry{"brain.lua"};
    std::string level{"hard"};
    int32_t difficulty{OA_DIFFICULTY_HARD};
    uint8_t player{computer};
    std::vector<std::pair<std::string, std::string>> files;
};

void add_file(Package& package, std::string path, std::string text) {
    package.files.emplace_back(std::move(path), std::move(text));
}

std::string load_script(const char* name) {
    const std::string path = std::string(OA_CHAT_LISTENER_DIR) + "/" + name;
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error(std::string("missing ") + name);
    std::string text;
    std::string line;
    while (std::getline(input, line)) {
        text.append(line);
        text.push_back('\n');
    }
    if (text.empty())
        throw std::runtime_error(std::string("empty ") + name);
    return text;
}

brains::SeatOutcome seat_it(Fixture& fixture, Package& package) {
    std::vector<brains::PackageFile> files;
    files.reserve(package.files.size());
    for (const auto& held : package.files)
        files.push_back(
            {held.first,
             std::span<const uint8_t>{
                 reinterpret_cast<const uint8_t*>(held.second.data()), held.second.size()
             }}
        );

    brains::SeatSpec spec;
    spec.player = package.player;
    spec.id = package.id;
    spec.name = package.name;
    spec.version = package.version;
    spec.entry = package.entry;
    spec.level = package.level;
    spec.difficulty = package.difficulty;
    spec.files = files;
    return brains::seat_brain(*fixture.match, spec);
}

brains::SeatSpec spec_of(
    Package& package,
    std::vector<brains::PackageFile>& files,
    std::vector<brains::OptionValue>&,
    std::vector<brains::NetworkFile>&
) {
    files.reserve(package.files.size());
    for (const auto& held : package.files)
        files.push_back(
            {held.first,
             std::span<const uint8_t>{
                 reinterpret_cast<const uint8_t*>(held.second.data()), held.second.size()
             }}
        );
    brains::SeatSpec spec;
    spec.player = package.player;
    spec.id = package.id;
    spec.name = package.name;
    spec.version = package.version;
    spec.entry = package.entry;
    spec.level = package.level;
    spec.difficulty = package.difficulty;
    spec.files = files;
    return spec;
}

void advance(Fixture& fixture) {
    ++fixture.match->state().game.tick;
    fixture.match->tick();
}

Package package_with(std::string script, uint8_t player = computer) {
    Package package;
    package.player = player;
    add_file(package, "brain.lua", std::move(script));
    return package;
}

host::Sandbox* seat_ready(Fixture& fixture, Package& package) {
    const brains::SeatOutcome outcome = seat_it(fixture, package);
    CHECK(outcome.result == brains::SeatResult::seated);
    CHECK(brains::brain_seated(*fixture.match, package.player));
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, package.player);
    CHECK(sandbox != nullptr);
    return sandbox;
}

struct LogRead {
    const char* name{"log"};
    std::vector<std::string>* lines{};
};

void read_log(void* context, lua_State* state) {
    auto* held = static_cast<LogRead*>(context);
    lua_getglobal(state, held->name);
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
            held->lines->emplace_back(text, length);
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
}

std::vector<std::string> read_named(host::Sandbox& sandbox, const char* name) {
    std::vector<std::string> lines;
    LogRead held;
    held.name = name;
    held.lines = &lines;
    host::ScriptError failure;
    CHECK(sandbox.run_host("read", &held, read_log, failure));
    return lines;
}

std::vector<std::string> read_lines(host::Sandbox& sandbox) {
    return read_named(sandbox, "log");
}

int line_at(const std::vector<std::string>& lines, const std::string& wanted) {
    for (int row = 0; row < static_cast<int>(lines.size()); ++row) {
        if (lines[static_cast<std::size_t>(row)] == wanted)
            return row;
    }
    return -1;
}

int prefix_at(const std::vector<std::string>& lines, std::string_view prefix) {
    for (int row = 0; row < static_cast<int>(lines.size()); ++row) {
        const std::string& line = lines[static_cast<std::size_t>(row)];
        if (line.size() >= prefix.size() &&
            std::string_view(line).substr(0, prefix.size()) == prefix)
            return row;
    }
    return -1;
}

std::string joined(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines)
        text += line + "\n";
    return text;
}

void require_line(const std::vector<std::string>& lines, const std::string& wanted) {
    if (line_at(lines, wanted) >= 0)
        return;
    throw std::runtime_error(wanted + " missing\n" + joined(lines));
}

struct Chunk {
    const char* source{};
};

int push_chunk(void* context, lua_State* state) {
    const auto* chunk = static_cast<const Chunk*>(context);
    if (chunk == nullptr || chunk->source == nullptr)
        return luaL_error(state, "missing probe");
    if (luaL_loadstring(state, chunk->source) != 0) {
        const char* message = lua_tostring(state, -1);
        if (message == nullptr)
            message = "probe failed to load";
        return luaL_error(state, "%s", message);
    }
    return 0;
}

void run_chunk(host::Sandbox& sandbox, const char* source) {
    Chunk chunk{source};
    host::CallHooks hooks;
    hooks.context = &chunk;
    hooks.push = push_chunk;
    host::ScriptError failure;
    if (!sandbox.call("probe", hooks, failure))
        throw std::runtime_error(failure.message.empty() ? "probe failed" : failure.message);
}

struct Said {
    uint8_t seat{};
    uint8_t mode{};
    uint8_t target{};
    std::string text;
};

struct SayTape {
    std::vector<Said> calls;
    std::vector<uint8_t> answers;
    std::size_t next{};
    bool hear{};
    bool heard{};
    oa::sim::match_runtime::Match* match{};
    uint8_t hear_seat{};
};

uint8_t play_say(void* context, uint8_t seat, std::string_view text, uint8_t mode, uint8_t target) {
    auto* tape = static_cast<SayTape*>(context);
    Said said;
    said.seat = seat;
    said.mode = mode;
    said.target = target;
    said.text.assign(text.data(), text.size());
    tape->calls.push_back(std::move(said));
    uint8_t answer = 0;
    if (tape->next < tape->answers.size()) {
        answer = tape->answers[tape->next];
        ++tape->next;
    }
    if (tape->hear && !tape->heard && tape->match != nullptr) {
        tape->heard = true;
        brains::hear_chat(*tape->match, tape->hear_seat, 3, OA_CHAT_MODE_ALLIES, "late");
    }
    return answer;
}

void bind_say(Fixture& fixture, SayTape& tape) {
    brains::ChatHooks hooks;
    hooks.context = &tape;
    hooks.say = play_say;
    brains::set_chat_hooks(*fixture.match, hooks);
}

struct SayCount {
    uint32_t calls{};
};

uint8_t count_say(void* context, uint8_t, std::string_view, uint8_t, uint8_t) {
    auto* count = static_cast<SayCount*>(context);
    if (count != nullptr)
        ++count->calls;
    return 0;
}

std::string chat_line(uint8_t from, std::string_view text, std::string_view who) {
    return "chat " + std::to_string(from) + " " + std::to_string(text.size()) + " " +
           std::string(who) + " " + std::string(text);
}

void lines_reach_on_chat_in_order() {
    Fixture fixture;
    Package package = package_with(load_script("brain.lua"));
    host::Sandbox* sandbox = seat_ready(fixture, package);

    (void)fixture.spawn(computer, collector, 8, 8);
    brains::hear_chat(*fixture.match, computer, 2, OA_CHAT_MODE_EVERYONE, "m-all");
    brains::hear_chat(*fixture.match, computer, 2, OA_CHAT_MODE_ALLIES, "m-allies");
    brains::hear_chat(*fixture.match, computer, 2, OA_CHAT_MODE_ENEMIES, "m-enemies");
    brains::hear_chat(*fixture.match, computer, 2, OA_CHAT_MODE_CHOSEN, "m-me");
    brains::hear_chat(*fixture.match, computer, 8, OA_CHAT_MODE_EVERYONE, "p1");
    brains::hear_chat(*fixture.match, computer, 8, OA_CHAT_MODE_EVERYONE, "p2");
    brains::hear_chat(*fixture.match, computer, 8, OA_CHAT_MODE_EVERYONE, "p3");
    brains::hear_chat(*fixture.match, computer, 8, OA_CHAT_MODE_EVERYONE, "p4");
    advance(fixture);

    const std::vector<std::string> lines = read_lines(*sandbox);
    const int created = prefix_at(lines, "created ");
    const int all = line_at(lines, chat_line(2, "m-all", "all"));
    const int allies = line_at(lines, chat_line(2, "m-allies", "allies"));
    const int enemies = line_at(lines, chat_line(2, "m-enemies", "enemies"));
    const int mine = line_at(lines, chat_line(2, "m-me", "me"));
    const int part1 = line_at(lines, chat_line(8, "p1", "all"));
    const int part2 = line_at(lines, chat_line(8, "p2", "all"));
    const int part3 = line_at(lines, chat_line(8, "p3", "all"));
    const int part4 = line_at(lines, chat_line(8, "p4", "all"));
    const int tick = line_at(lines, "tick 1");
    CHECK(created >= 0);
    CHECK(all > created);
    CHECK(allies > all);
    CHECK(enemies > allies);
    CHECK(mine > enemies);
    CHECK(part1 > mine);
    CHECK(part2 > part1);
    CHECK(part3 > part2);
    CHECK(part4 > part3);
    CHECK(tick > part4);
    CHECK(line_at(lines, "timer") < 0);
    CHECK(prefix_at(lines, "alliance ") < 0);

    std::array<uint8_t, 10> allies_with{};
    allies_with[computer] = 1;
    allies_with[0] = 1;
    fixture.match->configure_player_alliances(computer, allies_with);
    brains::hear_chat(*fixture.match, computer, 2, OA_CHAT_MODE_ALLIES, "m-after");
    advance(fixture);
    const std::vector<std::string> later = read_lines(*sandbox);
    const int alliance = line_at(later, "alliance 0 true");
    const int after = line_at(later, chat_line(2, "m-after", "allies"));
    const int tick_two = line_at(later, "tick 2");
    CHECK(alliance > tick);
    CHECK(after > alliance);
    CHECK(tick_two > after);
}

void line_heard_during_the_turn_waits() {
    Fixture fixture;
    Package package = package_with(load_script("during.lua"));
    host::Sandbox* sandbox = seat_ready(fixture, package);
    SayTape tape;
    tape.hear = true;
    tape.match = fixture.match.get();
    tape.hear_seat = computer;
    bind_say(fixture, tape);

    brains::hear_chat(*fixture.match, computer, 2, OA_CHAT_MODE_EVERYONE, "early");
    advance(fixture);
    const std::vector<std::string> first = read_lines(*sandbox);
    const int early = line_at(first, chat_line(2, "early", "all"));
    const int tick = line_at(first, "tick 1");
    CHECK(early >= 0);
    CHECK(tick > early);
    CHECK(line_at(first, chat_line(3, "late", "allies")) < 0);
    CHECK(tape.heard);
    CHECK(tape.calls.size() == 1);
    CHECK(tape.calls[0].text == "during");
    CHECK(tape.calls[0].mode == OA_CHAT_MODE_EVERYONE);

    advance(fixture);
    const std::vector<std::string> second = read_lines(*sandbox);
    const int late = line_at(second, chat_line(3, "late", "allies"));
    const int tick_two = line_at(second, "tick 2");
    CHECK(late > tick);
    CHECK(tick_two > late);
}

void the_33rd_line_is_dropped() {
    Fixture fixture;
    Package package = package_with(load_script("brain.lua"));
    host::Sandbox* sandbox = seat_ready(fixture, package);
    for (int number = 0; number < 33; ++number) {
        const std::string word = "c" + std::to_string(number);
        brains::hear_chat(*fixture.match, computer, 4, OA_CHAT_MODE_EVERYONE, word);
    }
    CHECK(brains::dropped_chat_lines(*fixture.match, computer) == 1);
    advance(fixture);
    const std::vector<std::string> lines = read_lines(*sandbox);
    CHECK(line_at(lines, chat_line(4, "c0", "all")) >= 0);
    CHECK(line_at(lines, chat_line(4, "c31", "all")) > line_at(lines, chat_line(4, "c0", "all")));
    CHECK(line_at(lines, chat_line(4, "c32", "all")) < 0);
    int chats = 0;
    for (const std::string& line : lines) {
        if (line.size() >= 5 && std::string_view(line).substr(0, 5) == "chat ")
            ++chats;
    }
    CHECK(chats == 32);
    CHECK(brains::dropped_chat_lines(*fixture.match, computer) == 1);
}

void long_words_are_cut_between_characters() {
    Fixture fixture;
    Package package = package_with(load_script("brain.lua"));
    host::Sandbox* sandbox = seat_ready(fixture, package);
    std::string kept(253, 'a');
    kept.append("\xE4\xBD\xA0", 3);
    std::string past(254, 'a');
    past.append("\xE4\xBD\xA0", 3);
    brains::hear_chat(*fixture.match, computer, 6, OA_CHAT_MODE_EVERYONE, kept);
    brains::hear_chat(*fixture.match, computer, 6, OA_CHAT_MODE_EVERYONE, past);
    advance(fixture);
    const std::vector<std::string> lines = read_lines(*sandbox);
    const std::string kept_line =
        chat_line(6, std::string(253, 'a') + std::string("\xE4\xBD\xA0", 3), "all");
    const std::string past_line = chat_line(6, std::string(254, 'a'), "all");
    require_line(lines, kept_line);
    require_line(lines, past_line);
}

void say_passes_each_form_and_each_answer() {
    Fixture fixture;
    Package package = package_with(load_script("quiet.lua"));
    host::Sandbox* sandbox = seat_ready(fixture, package);

    run_chunk(
        *sandbox,
        "probe = {}\n"
        "local function note(line)\n"
        "  probe[#probe + 1] = line\n"
        "end\n"
        "local function raised(body)\n"
        "  local ok, err = pcall(body)\n"
        "  if ok then note('no-raise') else note('raise|' .. tostring(err)) end\n"
        "end\n"
        "local ok, reason = brain.say('hello')\n"
        "if reason == nil then reason = '' end\n"
        "note((ok == true and 'true' or 'false') .. '|' .. reason)\n"
        "raised(function() brain.say(1) end)\n"
        "raised(function() brain.say() end)\n"
        "raised(function() brain.say('hi', 'me') end)\n"
        "raised(function() brain.say('hi', 10) end)\n"
        "raised(function() brain.say('hi', -1) end)\n"
        "raised(function() brain.say('hi', 1.5) end)\n"
        "raised(function() brain.say('hi', true) end)\n"
    );
    const std::vector<std::string> before = read_named(*sandbox, "probe");
    require_line(before, "false|no_chat");
    CHECK(before.size() == 8);
    for (std::size_t row = 1; row < before.size(); ++row) {
        CHECK(before[row].size() >= 6 && before[row].substr(0, 6) == "raise|");
        CHECK(before[row].find("no-raise") == std::string::npos);
    }
    CHECK(before[1].find("say expects text") != std::string::npos);
    CHECK(before[2].find("say expects text") != std::string::npos);
    const char* to_message = "say expects \"all\", \"allies\", \"enemies\" or a player from 0 to 9";
    for (std::size_t row = 3; row < before.size(); ++row)
        CHECK(before[row].find(to_message) != std::string::npos);

    SayTape tape;
    bind_say(fixture, tape);
    run_chunk(
        *sandbox,
        "probe = {}\n"
        "local function once(text, to)\n"
        "  local ok, reason = brain.say(text, to)\n"
        "  if reason == nil then reason = '' end\n"
        "  probe[#probe + 1] = (ok == true and 'true' or 'false') .. '|' .. reason\n"
        "end\n"
        "once('hello')\n"
        "once('hello', 'all')\n"
        "once('allies-line', 'allies')\n"
        "once('enemies-line', 'enemies')\n"
        "once('to-four', 4)\n"
        "once('to-zero', 0)\n"
        "once('to-nine', 9)\n"
    );
    CHECK(tape.calls.size() == 7);
    CHECK(tape.calls[0].seat == computer);
    CHECK(tape.calls[0].text == "hello");
    CHECK(tape.calls[0].mode == OA_CHAT_MODE_EVERYONE);
    CHECK(tape.calls[0].target == 0);
    CHECK(tape.calls[1].text == "hello");
    CHECK(tape.calls[1].mode == OA_CHAT_MODE_EVERYONE);
    CHECK(tape.calls[1].target == 0);
    CHECK(tape.calls[2].text == "allies-line");
    CHECK(tape.calls[2].mode == OA_CHAT_MODE_ALLIES);
    CHECK(tape.calls[2].target == 0);
    CHECK(tape.calls[3].text == "enemies-line");
    CHECK(tape.calls[3].mode == OA_CHAT_MODE_ENEMIES);
    CHECK(tape.calls[4].text == "to-four");
    CHECK(tape.calls[4].mode == OA_CHAT_MODE_CHOSEN);
    CHECK(tape.calls[4].target == 4);
    CHECK(tape.calls[5].mode == OA_CHAT_MODE_CHOSEN);
    CHECK(tape.calls[5].target == 0);
    CHECK(tape.calls[6].mode == OA_CHAT_MODE_CHOSEN);
    CHECK(tape.calls[6].target == 9);
    const std::vector<std::string> forms = read_named(*sandbox, "probe");
    CHECK(forms.size() == 7);
    for (const std::string& line : forms)
        CHECK(line == "true|");

    const std::size_t already = tape.calls.size();
    tape.answers = {0, 2, 3, 4, 5, 1, 9};
    run_chunk(
        *sandbox,
        "probe = {}\n"
        "local function once(text)\n"
        "  local ok, reason = brain.say(text)\n"
        "  if reason == nil then reason = '' end\n"
        "  probe[#probe + 1] = (ok == true and 'true' or 'false') .. '|' .. reason\n"
        "end\n"
        "once('fine')\n"
        "once('')\n"
        "once('+go')\n"
        "once('soon')\n"
        "once('nobody')\n"
        "once('missing')\n"
        "once('odd')\n"
    );
    const std::vector<std::string> reasons = read_named(*sandbox, "probe");
    require_line(reasons, "true|");
    require_line(reasons, "false|empty");
    require_line(reasons, "false|command");
    require_line(reasons, "false|too_soon");
    require_line(reasons, "false|no_target");
    require_line(reasons, "false|not_a_seat");
    require_line(reasons, "false|no_chat");
    CHECK(reasons.size() == 7);
    CHECK(tape.calls.size() == already + 7);
    CHECK(tape.calls[already].text == "fine");
    CHECK(tape.calls[already + 1].text.empty());
    CHECK(tape.calls[already + 2].text == "+go");
}

void seat_without_on_chat_still_ticks() {
    Fixture fixture;
    Package package = package_with(load_script("quiet.lua"));
    host::Sandbox* sandbox = seat_ready(fixture, package);
    brains::hear_chat(*fixture.match, 0, 1, OA_CHAT_MODE_EVERYONE, "ghost");
    CHECK(brains::dropped_chat_lines(*fixture.match, 0) == 0);
    CHECK(!brains::brain_seated(*fixture.match, 0));
    brains::hear_chat(*fixture.match, computer, 1, OA_CHAT_MODE_EVERYONE, "unread");
    advance(fixture);
    const std::vector<std::string> lines = read_lines(*sandbox);
    require_line(lines, "tick 1");
    CHECK(lines.size() == 1);
    CHECK(brains::dropped_chat_lines(*fixture.match, computer) == 0);
    advance(fixture);
    const std::vector<std::string> later = read_lines(*sandbox);
    require_line(later, "tick 2");
    CHECK(later.size() == 2);
}

void say_outside_a_match_has_no_chat() {
    Package package = package_with(load_script("outside.lua"));
    std::vector<brains::PackageFile> files;
    std::vector<brains::OptionValue> options;
    std::vector<brains::NetworkFile> networks;
    const brains::SeatSpec spec = spec_of(package, files, options, networks);
    const oa::data::brain_facts::GameFacts facts{};
    const brains::SeatOutcome outcome = brains::check_brain(spec, facts);
    CHECK(outcome.result == brains::SeatResult::seated);
    CHECK(outcome.reason[0] == '\0');

    Fixture fixture;
    CHECK(!fixture.match->seat_state());
    brains::hear_chat(*fixture.match, computer, 1, OA_CHAT_MODE_EVERYONE, "nobody");
    CHECK(brains::dropped_chat_lines(*fixture.match, computer) == 0);
    CHECK(!fixture.match->seat_state());
}

void same_stream(Fixture& one, Fixture& other, uint32_t ticks) {
    CHECK(one.match->random_state() == other.match->random_state());
    for (uint32_t passed = 0; passed < ticks; ++passed) {
        advance(one);
        advance(other);
        if (one.match->random_state() != other.match->random_state())
            throw std::runtime_error("random diverged at tick " + std::to_string(passed + 1));
    }
    const oa::base::game_loop::Timing timing{};
    const uint64_t one_hash = oa::sim::trace::match_state_hash(*one.match, timing, 0, 0, nullptr);
    const uint64_t other_hash =
        oa::sim::trace::match_state_hash(*other.match, timing, 0, 0, nullptr);
    CHECK(one_hash == other_hash);
}

void no_brain_and_a_chatting_brain_stay_in_step() {
    Fixture plain_one;
    Fixture plain_other;
    same_stream(plain_one, plain_other, chat_run_ticks);

    Fixture talking_one;
    Fixture talking_other;
    Package one_package = package_with(load_script("chatter.lua"));
    Package other_package = package_with(load_script("chatter.lua"));
    (void)seat_ready(talking_one, one_package);
    (void)seat_ready(talking_other, other_package);
    SayCount one_count;
    SayCount other_count;
    brains::ChatHooks one_hooks;
    one_hooks.context = &one_count;
    one_hooks.say = count_say;
    brains::ChatHooks other_hooks;
    other_hooks.context = &other_count;
    other_hooks.say = count_say;
    brains::set_chat_hooks(*talking_one.match, one_hooks);
    brains::set_chat_hooks(*talking_other.match, other_hooks);
    same_stream(talking_one, talking_other, chat_run_ticks);
    CHECK(one_count.calls == chat_run_ticks);
    CHECK(other_count.calls == chat_run_ticks);
}

} // namespace

int main() {
    try {
        lines_reach_on_chat_in_order();
        line_heard_during_the_turn_waits();
        the_33rd_line_is_dropped();
        long_words_are_cut_between_characters();
        say_passes_each_form_and_each_answer();
        seat_without_on_chat_still_ticks();
        say_outside_a_match_has_no_chat();
        no_brain_and_a_chatting_brain_stay_in_step();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "sim-brains-chat: %s\n", ex.what());
        return 1;
    }
    return 0;
}
