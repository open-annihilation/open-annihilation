// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Status lines: hello, the events, and one exit line, each flushed, the
// code --quit-after-game gives a game that ended or a player who left, and
// the exit line of a refused launch.
#include "status.hpp"

#include "oa/formats/json.hpp"
#include "oa/test/check.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace automation = oa::app::automation;
namespace event_kind = automation::event_kind;
namespace json = oa::formats::json;

using json::Json;

struct Sink {
    std::vector<std::string> lines;
    int flushes{};
};

void keep_line(void* context, std::string_view line) {
    auto& sink = *static_cast<Sink*>(context);
    ++sink.flushes;
    sink.lines.emplace_back(line);
}

Json parsed_line(std::string_view line) {
    if (!line.empty() && line.back() == '\n')
        line.remove_suffix(1);
    json::JsonError error;
    const auto parsed = json::parse_json(line, error);
    OA_CHECK(parsed && parsed->type() == json::JsonType::object);
    return parsed ? *parsed : Json{};
}

std::string text(const Json& object, std::string_view name) {
    const Json* member = object.find(name);
    return member != nullptr && member->string() != nullptr ? *member->string() : "<none>";
}

std::optional<int64_t> number(const Json& object, std::string_view name) {
    const Json* member = object.find(name);
    return member != nullptr ? member->integer() : std::nullopt;
}

/// hello, one screen line and one exit line, each ending in a newline and
/// each one flush.
void check_lines() {
    Sink sink;
    automation::StatusStream stream;
    stream.set_writer(&sink, keep_line);
    OA_CHECK(stream.has_destination());
    stream.hello("Open Annihilation 0.8.0", 42);
    stream.hello("again", 1);
    OA_CHECK(stream.opened());
    automation::announce_screen(stream.watch());
    stream.set_clock(1, 0);
    automation::Observed now;
    now.screen = "main_menu";
    const automation::EventHooks hooks = stream.hooks();
    OA_CHECK(hooks.wanted(hooks.context, event_kind::screen));
    OA_CHECK(!hooks.wanted(hooks.context, event_kind::transfer));
    OA_CHECK(!hooks.wanted(hooks.context, event_kind::focus));
    automation::watch_events(stream.watch(), now, hooks);
    stream.finish(7);
    stream.finish(9);
    OA_CHECK(sink.flushes == 3);
    OA_CHECK(sink.lines.size() == static_cast<size_t>(sink.flushes));
    for (const std::string& line : sink.lines)
        OA_CHECK(!line.empty() && line.back() == '\n');
    if (sink.lines.size() != 3)
        return;
    const Json hello = parsed_line(sink.lines[0]);
    OA_CHECK(text(hello, "event") == "hello");
    OA_CHECK(number(hello, "status") == 1);
    OA_CHECK(text(hello, "version") == "Open Annihilation 0.8.0");
    OA_CHECK(number(hello, "pid") == 42);
    const Json screen = parsed_line(sink.lines[1]);
    OA_CHECK(text(screen, "event") == "screen");
    OA_CHECK(text(screen, "name") == "main_menu");
    OA_CHECK(number(screen, "seq") == 1);
    OA_CHECK(number(screen, "frame") == 1);
    OA_CHECK(number(screen, "tick") == 0);
    const Json exit_line = parsed_line(sink.lines[2]);
    OA_CHECK(text(exit_line, "event") == "exit");
    OA_CHECK(text(exit_line, "reason") == "quit");
    OA_CHECK(number(exit_line, "code") == 7);
}

/// A game that ended writes hello, its game_over line and an exit of 0. A
/// player who left, or lost the connection, exits 3. A later leave does not
/// replace a game that ended.
void check_quit() {
    Sink ended;
    automation::StatusStream game;
    game.set_writer(&ended, keep_line);
    game.set_quit(automation::QuitAfter::game);
    game.hello("Open Annihilation 0.8.0", 7);
    automation::EventHooks hooks = game.hooks();
    automation::JsonWriter event = hooks.begin(hooks.context, "game_over");
    event.key("outcome");
    event.string("victory");
    hooks.send(hooks.context, event);
    OA_CHECK(game.take_quit());
    OA_CHECK(!game.take_quit());
    game.note_departure(true);
    game.finish(7);
    OA_CHECK(ended.lines.size() == 3);
    if (ended.lines.size() == 3) {
        const Json over = parsed_line(ended.lines[1]);
        OA_CHECK(text(over, "event") == "game_over");
        OA_CHECK(text(over, "outcome") == "victory");
        const Json exit_line = parsed_line(ended.lines.back());
        OA_CHECK(text(exit_line, "reason") == "game-ended");
        OA_CHECK(number(exit_line, "code") == 0);
    }

    Sink left;
    automation::StatusStream early;
    early.set_writer(&left, keep_line);
    early.set_quit(automation::QuitAfter::game);
    early.hello("Open Annihilation 0.8.0", 7);
    early.note_departure(false);
    OA_CHECK(early.take_quit());
    early.finish(7);
    OA_CHECK(left.lines.size() == 2);
    if (left.lines.size() == 2) {
        const Json exit_line = parsed_line(left.lines.back());
        OA_CHECK(text(exit_line, "reason") == "left");
        OA_CHECK(number(exit_line, "code") == 3);
    }

    automation::StatusStream lost;
    lost.set_quit(automation::QuitAfter::game);
    lost.note_departure(true);
    OA_CHECK(lost.resolved_code(7) == 3);
    OA_CHECK(lost.take_quit());

    automation::StatusStream score;
    score.set_quit(automation::QuitAfter::score);
    score.note_game_over();
    OA_CHECK(!score.take_quit());
    score.note_score_left();
    OA_CHECK(score.take_quit());
    OA_CHECK(score.resolved_code(4) == 0);

    automation::StatusStream bare;
    bare.set_quit(automation::QuitAfter::game);
    bare.hello("Open Annihilation 0.8.0", 1);
    OA_CHECK(!bare.opened());
    bare.note_game_over();
    OA_CHECK(bare.take_quit());
    bare.finish(4);
    OA_CHECK(bare.resolved_code(4) == 0);
}

/// A launch refused after hello ends the stream with reason refused and the
/// refusal's code, with or without --quit-after-game; before hello nothing
/// is written.
void check_refused() {
    for (const automation::QuitAfter quit :
         {automation::QuitAfter::none, automation::QuitAfter::game}) {
        Sink sink;
        automation::StatusStream stream;
        stream.set_writer(&sink, keep_line);
        stream.set_quit(quit);
        stream.hello("Open Annihilation 0.8.0", 5);
        stream.note_refused();
        OA_CHECK(stream.resolved_code(2) == 2);
        stream.finish(2);
        OA_CHECK(sink.lines.size() == 2);
        if (sink.lines.size() != 2)
            continue;
        const Json exit_line = parsed_line(sink.lines.back());
        OA_CHECK(text(exit_line, "event") == "exit");
        OA_CHECK(text(exit_line, "reason") == "refused");
        OA_CHECK(number(exit_line, "code") == 2);
    }

    Sink unopened;
    automation::StatusStream before;
    before.set_writer(&unopened, keep_line);
    before.note_refused();
    before.finish(2);
    OA_CHECK(unopened.lines.empty());
}

/// The same line is written to a file, and flushed there.
void check_file() {
    // A fixed name in the shared temporary folder collides when two runs
    // overlap, and removing it while the stream still holds the handle
    // throws on Windows: the handle does not share delete.
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path path = std::filesystem::temp_directory_path() /
                                       ("oa-g68-status-lines-" + std::to_string(stamp) + ".jsonl");
    std::error_code removed;
    std::filesystem::remove(path, removed);
    {
        automation::StatusStream stream;
        OA_CHECK(stream.open_file(path));
        stream.hello("Open Annihilation 0.8.0", 3);
        stream.finish(0);
    }
    std::ifstream in(path, std::ios::binary);
    std::string hello;
    std::string exit_line;
    OA_CHECK(static_cast<bool>(std::getline(in, hello)));
    OA_CHECK(static_cast<bool>(std::getline(in, exit_line)));
    OA_CHECK(text(parsed_line(hello), "event") == "hello");
    OA_CHECK(text(parsed_line(exit_line), "reason") == "quit");
    OA_CHECK(number(parsed_line(exit_line), "code") == 0);
    in.close();
    std::filesystem::remove(path, removed);
}

} // namespace

int main() {
    check_lines();
    check_quit();
    check_refused();
    check_file();
    return oa::test::check_exit_status();
}
