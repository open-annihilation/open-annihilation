// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The automation endpoint's options through oa-game's command line: --fark
// alone and with --fark-address and --fark-file, the run they make one a
// program may control, none of them without the extension, the addresses
// refused, --fark-address and --fark-file without --fark refused, the
// engine options a run with the endpoint refuses, the usage text; and the
// endpoint's token.
#include "oa/app/app.hpp"
#include "oa/app/netgame/launch_file.hpp"
#include "oa/test/check.hpp"
#include "options.hpp"
#include "token.hpp"

#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace automation = oa::app::automation;

// What one parse gave.
struct Parsed {
    automation::EndpointOptions endpoint;
    oa::app::Options app;
};

/// Parses a command line, with or without the endpoint's option hooks.
///
/// @param arguments the arguments after the program's name
/// @param extended with the endpoint's hooks
/// @return what the parse gave; throws what the parse throws
Parsed parse(const std::vector<const char*>& arguments, bool extended = true) {
    std::vector<std::string> storage{"open-annihilation"};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& argument : storage)
        argv.push_back(argument.data());
    automation::EndpointOptions endpoint{};
    oa::app::Extension table{};
    if (extended)
        automation::fill_option_hooks(table, endpoint);
    auto app = oa::app::parse_options(static_cast<int>(argv.size()), argv.data(), table);
    return {endpoint, std::move(app)};
}

/// Returns the message a parse refuses a command line with.
///
/// @param arguments the arguments after the program's name
/// @param extended with the endpoint's hooks
/// @return the message, or "" when the parse takes the command line
std::string rejection(const std::vector<const char*>& arguments, bool extended = true) {
    try {
        (void)parse(arguments, extended);
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    return {};
}

/// --fark turns the endpoint on at 127.0.0.1 and a port the system chooses,
/// and makes the run one a program may control; the address and the file
/// are taken as given; nothing without --fark.
void check_taken() {
    const Parsed off = parse({});
    OA_CHECK(!off.endpoint.enabled);
    OA_CHECK(!off.app.remote_controlled);

    const Parsed on = parse({"--fark"});
    OA_CHECK(on.endpoint.enabled);
    OA_CHECK(on.app.remote_controlled);
    OA_CHECK(on.endpoint.listen.family == automation::LoopbackFamily::ipv4);
    OA_CHECK(on.endpoint.listen.port == 0);
    OA_CHECK(!on.endpoint.file);
    OA_CHECK(!on.app.unattended);

    const Parsed given =
        parse({"--fark", "--fark-address", "[::1]:47123", "--fark-file", "work/endpoint.json"});
    OA_CHECK(given.endpoint.listen.family == automation::LoopbackFamily::ipv6);
    OA_CHECK(given.endpoint.listen.port == 47123);
    OA_CHECK(given.endpoint.file && *given.endpoint.file == "work/endpoint.json");

    const Parsed later = parse({"--fark-address", "127.0.0.1:65535", "--skip-intro", "--fark"});
    OA_CHECK(later.endpoint.listen.port == 65535);

    // The endpoint goes with a network game started from the command line.
    OA_CHECK(rejection({"--fark", "--frames", "100"}).empty());

    // Without the extension the options are unknown.
    OA_CHECK(rejection({"--fark"}, false) == "unknown option: --fark");
}

/// --fark-address and --fark-file need --fark, and only this machine's
/// loopback address can be served.
void check_refused() {
    OA_CHECK(rejection({"--fark-address", "127.0.0.1:0"}) == "--fark-address needs --fark");
    OA_CHECK(rejection({"--fark-file", "endpoint.json"}) == "--fark-file needs --fark");
    const std::string loopback_only = "automation: only a loopback address can be served";
    for (const char* address : {
             "0.0.0.0:47123",
             "192.0.2.7:47123",
             "localhost:47123",
             "[::]:47123",
             "127.0.0.2:47123",
         })
        OA_CHECK(rejection({"--fark", "--fark-address", address}) == loopback_only);
    for (const char* address : {
             "127.0.0.1",
             "127.0.0.1:",
             "127.0.0.1:65536",
             "127.0.0.1:-1",
             "127.0.0.1:12ab",
             ":47123",
         })
        OA_CHECK(
            rejection({"--fark", "--fark-address", address}).starts_with("--fark-address expects")
        );
    OA_CHECK(rejection({"--fark", "--fark-address"}) == "--fark-address requires a value");
    OA_CHECK(rejection({"--fark", "--fark-file"}) == "--fark-file requires a value");
}

/// The engine options that run no main loop, or own it, are named as the
/// ones a run with the endpoint cannot be used with; the others are not.
void check_conflicts() {
    const auto conflict = [](const std::vector<const char*>& arguments) {
        return std::string(automation::conflicting_option(parse(arguments).app));
    };
    OA_CHECK(conflict({"--fark"}).empty());
    OA_CHECK(conflict({"--fark", "--frames", "10"}).empty());
    OA_CHECK(conflict({"--fark", "--skip-intro", "--mute"}).empty());
    OA_CHECK(conflict({"--fark", "--headless-check"}) == "--headless-check");
    OA_CHECK(conflict({"--fark", "--benchmark", "10"}) == "--benchmark");
    OA_CHECK(conflict({"--fark", "--match-ticks", "10"}) == "--match-ticks");
    OA_CHECK(conflict({"--fark", "--render-script", "film.oascript"}) == "--render-script");
    OA_CHECK(
        conflict({"--fark", "--replay-index", "game.tad", "--output", "game.json"}) ==
        "--replay-index"
    );
    OA_CHECK(conflict({"--fark", "--showcase", "skirmish-battle"}) == "--showcase");
    OA_CHECK(conflict({"--fark", "--check-navigation"}) == "a --check-* option");
    OA_CHECK(conflict({"--fark", "--check-multiplayer-menu"}) == "a --check-* option");
    OA_CHECK(conflict({"--fark", "--check-load-save"}) == "a --check-* option");
    OA_CHECK(conflict({"--fark", "--check-user-folder"}) == "a --check-* option");
}

/// Status lines and --quit-after-game need no endpoint, and do not make the
/// run one a program controls. A following word is not the option's value.
/// An empty status file is refused.
void check_status_options() {
    const Parsed lines = parse({"--status-stdout"});
    OA_CHECK(lines.endpoint.status_stdout);
    OA_CHECK(!lines.endpoint.enabled);
    OA_CHECK(!lines.app.remote_controlled);
    OA_CHECK(lines.endpoint.quit_after == automation::QuitAfter::none);

    const Parsed file = parse({"--status-file", "work/status.jsonl", "--quit-after-game"});
    OA_CHECK(file.endpoint.status_file && *file.endpoint.status_file == "work/status.jsonl");
    OA_CHECK(!file.endpoint.status_stdout);
    OA_CHECK(file.endpoint.quit_after == automation::QuitAfter::game);
    OA_CHECK(!file.app.remote_controlled);

    const Parsed score = parse({"--quit-after-game=after-score"});
    OA_CHECK(score.endpoint.quit_after == automation::QuitAfter::score);
    OA_CHECK(!score.app.remote_controlled);

    const Parsed bare = parse({"--quit-after-game", "--mute"});
    OA_CHECK(bare.endpoint.quit_after == automation::QuitAfter::game);
    OA_CHECK(bare.app.mute);

    OA_CHECK(rejection({"--quit-after-game=later"}) == "--quit-after-game takes after-score");
    OA_CHECK(rejection({"--status-file"}) == "--status-file requires a value");
    OA_CHECK(rejection({"--status-stdout"}, false) == "unknown option: --status-stdout");

    automation::EndpointOptions empty;
    empty.status_file = std::filesystem::path{};
    oa::app::Extension table{};
    automation::fill_option_hooks(table, empty);
    std::string message;
    try {
        table.check_options(table.context);
    } catch (const std::runtime_error& error) {
        message = error.what();
    }
    OA_CHECK(message == "--status-file needs a file");
}

/// The launch's status block, which network play keeps as it checks the
/// command line, turns the stream on. The command line replaces one field.
/// A run with no status block changes nothing.
void check_launch_status() {
    namespace netgame = oa::app::netgame;
    netgame::LaunchStatusChoice both;
    both.standard_output_named = true;
    both.standard_output = true;
    both.quit_after = netgame::QuitAfter::after_score;
    netgame::keep_launch_status(both);
    const Parsed turned = parse({});
    OA_CHECK(turned.endpoint.status_stdout);
    OA_CHECK(turned.endpoint.quit_after == automation::QuitAfter::score);
    OA_CHECK(!turned.endpoint.status_file);
    OA_CHECK(!turned.endpoint.status_stdout_on_command);
    OA_CHECK(!turned.endpoint.quit_after_on_command);

    netgame::LaunchStatusChoice named;
    named.standard_output_named = true;
    named.standard_output = true;
    named.file = "other.jsonl";
    named.quit_after = netgame::QuitAfter::game;
    netgame::keep_launch_status(named);
    const Parsed replaced = parse({"--status-file", "chosen.jsonl"});
    OA_CHECK(replaced.endpoint.status_stdout);
    OA_CHECK(replaced.endpoint.status_file && *replaced.endpoint.status_file == "chosen.jsonl");
    OA_CHECK(replaced.endpoint.quit_after == automation::QuitAfter::game);
    OA_CHECK(replaced.endpoint.status_file_on_command);

    netgame::keep_launch_status({});
    const Parsed unchanged = parse({});
    OA_CHECK(!unchanged.endpoint.status_stdout);
    OA_CHECK(!unchanged.endpoint.status_file);
    OA_CHECK(unchanged.endpoint.quit_after == automation::QuitAfter::none);
}

/// The usage names the options on the run options' line, says what --fark
/// serves on the line after it, and words nothing else.
void check_usage() {
    automation::EndpointOptions endpoint{};
    oa::app::Extension table{};
    automation::fill_option_hooks(table, endpoint);
    const char* runs = table.text(table.context, oa::app::ExtensionText::usage_runs);
    OA_CHECK(runs != nullptr && std::string_view(runs).find("[--fark ") != std::string_view::npos);
    OA_CHECK(table.text(table.context, oa::app::ExtensionText::usage_checks) == nullptr);
    const char* note = table.text(table.context, oa::app::ExtensionText::usage_note);
    OA_CHECK(note != nullptr && std::string_view(note).starts_with("--fark serves the automation"));
    OA_CHECK(std::string_view(note).ends_with("(docs/automation.md).\n"));
    OA_CHECK(table.text(table.context, oa::app::ExtensionText::register_switch) == nullptr);
}

/// A token is 32 lowercase hexadecimal digits, new each time, and only the
/// whole token matches it.
void check_token() {
    const auto first = automation::make_token();
    const auto second = automation::make_token();
    OA_CHECK(first && second);
    if (!first || !second)
        return;
    OA_CHECK(first->size() == automation::token_bits / 4);
    OA_CHECK(first->find_first_not_of("0123456789abcdef") == std::string::npos);
    OA_CHECK(*first != *second);
    OA_CHECK(automation::token_matches(*first, *first));
    OA_CHECK(!automation::token_matches(*second, *first));
    OA_CHECK(!automation::token_matches(first->substr(1), *first));
    OA_CHECK(!automation::token_matches(*first + "0", *first));
    OA_CHECK(!automation::token_matches("", *first));
}

} // namespace

int main() {
    check_taken();
    check_refused();
    check_status_options();
    check_launch_status();
    check_conflicts();
    check_usage();
    check_token();
    return oa::test::check_exit_status();
}
