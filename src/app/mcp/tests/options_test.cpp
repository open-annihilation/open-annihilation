// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --mcp and --mcp-http through oa-game's command line: the run they mark
// as one a program may control, --mcp=observe, the options refused beside
// --mcp, the loopback address --mcp-http takes, and the usage text. The
// automation endpoint and network play are on the same command line, in
// the order the game asks them.
#include "oa/app/app.hpp"
#include "oa/app/command_line.hpp"
#include "oa/app/mcp/options.hpp"
#include "oa/app/mcp_quit.hpp"
#include "oa/test/check.hpp"
#include "net_options.hpp"
#include "options.hpp"

#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace mcp = oa::app::mcp;
namespace automation = oa::app::automation;

// The three extensions' tables, and the one table parse_options is given.
struct Harness {
    mcp::McpOptions mcp{};
    automation::EndpointOptions endpoint{};
    oa::app::Extension mcp_table{};
    oa::app::Extension net_table{};
    oa::app::Extension auto_table{};
    oa::app::Extension combined{};
    oa::app::command_line::SwitchHandler switches{};
};

// What one parse gave.
struct Parsed {
    mcp::McpOptions mcp{};
    oa::app::Options app{};
};

/// Clears a launch left by an earlier parse, and optionally keeps a piped launch.
///
/// @param keep_pipe restore piped_launch_allowed after the clear
void reset_net(bool keep_pipe) {
    auto& net = oa::app::netgame_context();
    const bool pipe = net.options.piped_launch_allowed;
    net.options = {};
    net.launch = {};
    if (keep_pipe)
        net.options.piped_launch_allowed = pipe;
}

/// Offers a switch to MCP, then to network play.
int take_switch(
    void* context,
    char letter,
    const oa::app::command_line::SwitchArguments* arguments,
    uint32_t* effects
) {
    auto* harness = static_cast<Harness*>(context);
    const auto* mcp_handler = harness->mcp_table.switch_handler != nullptr
                                  ? harness->mcp_table.switch_handler(harness->mcp_table.context)
                                  : nullptr;
    if (mcp_handler != nullptr && mcp_handler->take != nullptr &&
        mcp_handler->take(mcp_handler->context, letter, arguments, effects) != 0)
        return 1;
    const auto* net_handler = harness->net_table.switch_handler != nullptr
                                  ? harness->net_table.switch_handler(harness->net_table.context)
                                  : nullptr;
    if (net_handler != nullptr && net_handler->take != nullptr &&
        net_handler->take(net_handler->context, letter, arguments, effects) != 0)
        return 1;
    return 0;
}

/// Takes a long option from MCP, then network play, then the automation endpoint.
bool take_all(
    void* context, const char* name, const oa::app::OptionValues& values, uint32_t& effects
) {
    auto* harness = static_cast<Harness*>(context);
    if (harness->mcp_table.take_option != nullptr &&
        harness->mcp_table.take_option(harness->mcp_table.context, name, values, effects))
        return true;
    if (harness->net_table.take_option != nullptr &&
        harness->net_table.take_option(harness->net_table.context, name, values, effects))
        return true;
    if (harness->auto_table.take_option != nullptr &&
        harness->auto_table.take_option(harness->auto_table.context, name, values, effects))
        return true;
    return false;
}

/// Checks network play, then the automation endpoint, then MCP.
void check_all(void* context) {
    auto* harness = static_cast<Harness*>(context);
    if (harness->net_table.check_options != nullptr)
        harness->net_table.check_options(harness->net_table.context);
    if (harness->auto_table.check_options != nullptr)
        harness->auto_table.check_options(harness->auto_table.context);
    if (harness->mcp_table.check_options != nullptr)
        harness->mcp_table.check_options(harness->mcp_table.context);
}

/// Points the combined table at the three extensions.
///
/// @param harness the tables
void wire(Harness& harness) {
    mcp::fill_option_hooks(harness.mcp_table, harness.mcp);
    automation::fill_option_hooks(harness.auto_table, harness.endpoint);
    oa::app::fill_option_hooks(harness.net_table, oa::app::netgame_context());
    harness.switches.context = &harness;
    harness.switches.take = take_switch;
    harness.combined.context = &harness;
    harness.combined.take_option = take_all;
    harness.combined.check_options = check_all;
    harness.combined.switch_handler =
        [](void* context) -> const oa::app::command_line::SwitchHandler* {
        return &static_cast<Harness*>(context)->switches;
    };
    harness.combined.text = [](void* context, oa::app::ExtensionText which) -> const char* {
        auto* tables = static_cast<Harness*>(context);
        if (tables->mcp_table.text == nullptr)
            return static_cast<const char*>(nullptr);
        return tables->mcp_table.text(tables->mcp_table.context, which);
    };
}

/// Parses a command line.
///
/// @param arguments the arguments after the program's name
/// @param extended with MCP, network play and the automation endpoint
/// @param keep_pipe keep piped_launch_allowed across the reset
/// @return what the parse gave
Parsed
parse(const std::vector<const char*>& arguments, bool extended = true, bool keep_pipe = false) {
    reset_net(keep_pipe);
    std::vector<std::string> storage{"open-annihilation"};
    storage.insert(storage.end(), arguments.begin(), arguments.end());
    std::vector<char*> argv;
    for (auto& argument : storage)
        argv.push_back(argument.data());
    Harness harness;
    oa::app::Extension empty{};
    if (extended)
        wire(harness);
    auto app = oa::app::parse_options(
        static_cast<int>(argv.size()), argv.data(), extended ? harness.combined : empty
    );
    return {harness.mcp, std::move(app)};
}

/// Returns the message a parse refuses a command line with.
///
/// @param arguments the arguments after the program's name
/// @param extended with the three extensions
/// @param keep_pipe keep piped_launch_allowed across the reset
/// @return the message, or "" when the parse takes the command line
std::string
rejection(const std::vector<const char*>& arguments, bool extended = true, bool keep_pipe = false) {
    try {
        (void)parse(arguments, extended, keep_pipe);
    } catch (const std::runtime_error& error) {
        return error.what();
    }
    return {};
}

/// --mcp and --mcp=observe turn the server on and mark the run remote-controlled.
void check_taken() {
    const Parsed off = parse({});
    OA_CHECK(!off.mcp.enabled);
    OA_CHECK(!off.mcp.observe);
    OA_CHECK(!off.app.remote_controlled);

    const Parsed on = parse({"--mcp"});
    OA_CHECK(on.mcp.enabled);
    OA_CHECK(!on.mcp.observe);
    OA_CHECK(on.app.remote_controlled);

    const Parsed watching = parse({"--mcp=observe"});
    OA_CHECK(watching.mcp.enabled);
    OA_CHECK(watching.mcp.observe);
    OA_CHECK(watching.app.remote_controlled);

    OA_CHECK(rejection({"--mcp=nope"}) == "--mcp takes observe");
    OA_CHECK(rejection({"--mcp"}, false) == "unknown option: --mcp");
    OA_CHECK(rejection({"--mcp", "--fark"}).empty());
    OA_CHECK(rejection({"--mcp", "--skip-intro", "--mute"}).empty());

    const Parsed stepped = parse({"--mcp", "--mcp-clock", "stepped"});
    OA_CHECK(stepped.mcp.enabled);
    OA_CHECK(stepped.mcp.clock_set);
    OA_CHECK(stepped.mcp.clock == mcp::McpClock::stepped);

    const Parsed attached = parse({"--mcp-clock=realtime", "--mcp"});
    OA_CHECK(attached.mcp.enabled);
    OA_CHECK(attached.mcp.clock_set);
    OA_CHECK(attached.mcp.clock == mcp::McpClock::realtime);

    const Parsed plain = parse({"--mcp"});
    OA_CHECK(!plain.mcp.clock_set);
    OA_CHECK(plain.mcp.clock == mcp::McpClock::realtime);

    const Parsed http_clock = parse({"--mcp-http", "--mcp-clock", "stepped"});
    OA_CHECK(http_clock.mcp.http);
    OA_CHECK(!http_clock.mcp.enabled);
    OA_CHECK(http_clock.mcp.clock_set);
    OA_CHECK(http_clock.mcp.clock == mcp::McpClock::stepped);
    OA_CHECK(http_clock.app.remote_controlled);

    const Parsed both = parse({"--mcp", "--mcp-http", "--mcp-clock", "stepped"});
    OA_CHECK(both.mcp.enabled);
    OA_CHECK(both.mcp.http);
    OA_CHECK(both.mcp.clock_set);
    OA_CHECK(both.mcp.clock == mcp::McpClock::stepped);

    const Parsed clock_first = parse({"--mcp-clock", "stepped", "--mcp-http"});
    OA_CHECK(clock_first.mcp.http);
    OA_CHECK(!clock_first.mcp.enabled);
    OA_CHECK(clock_first.mcp.clock == mcp::McpClock::stepped);

    OA_CHECK(rejection({"--mcp-clock", "stepped"}) == "--mcp-clock needs --mcp or --mcp-http");
    OA_CHECK(rejection({"--mcp-clock=fast", "--mcp"}) == "--mcp-clock takes stepped or realtime");
    OA_CHECK(rejection({"--mcp", "--mcp-clock"}) == "--mcp-clock takes stepped or realtime");
    OA_CHECK(
        rejection({"--mcp", "--mcp-clock", "later"}) == "--mcp-clock takes stepped or realtime"
    );
    OA_CHECK(
        rejection({"--mcp-http", "--mcp-clock", "later"}) == "--mcp-clock takes stepped or realtime"
    );
}

/// Standard input, --list-content and --status-stdout cannot share --mcp.
void check_refused() {
    const std::string stdin_launch = "--mcp cannot be used with a launch read from standard input";
    OA_CHECK(rejection({"--mcp", "-c", "-"}) == stdin_launch);
    OA_CHECK(rejection({"-c", "-", "--mcp"}) == stdin_launch);
    OA_CHECK(rejection({"--launch", "-", "--mcp"}) == stdin_launch);
    OA_CHECK(rejection({"--mcp", "--launch", "-"}) == stdin_launch);
    OA_CHECK(
        rejection({"--mcp", "--list-content", "--json"}) ==
        "--mcp cannot be used with --list-content"
    );
    OA_CHECK(
        rejection({"--status-stdout", "--mcp"}) == "--mcp cannot be used with --status-stdout"
    );

    oa::app::allow_piped_launch();
    const Parsed piped = parse({"--mcp"}, true, true);
    OA_CHECK(piped.mcp.enabled);
    OA_CHECK(!oa::app::netgame_context().options.piped_launch_allowed);
}

/// The options that own the main loop are the ones --fark refuses.
void check_conflicts() {
    const auto conflict = [](const std::vector<const char*>& arguments) {
        return std::string(mcp::conflicting_option(parse(arguments).app));
    };
    OA_CHECK(conflict({"--mcp"}).empty());
    OA_CHECK(conflict({"--mcp", "--frames", "10"}).empty());
    OA_CHECK(conflict({"--mcp", "--headless-check"}) == "--headless-check");
    OA_CHECK(conflict({"--mcp", "--benchmark", "10"}) == "--benchmark");
    OA_CHECK(conflict({"--mcp", "--match-ticks", "10"}) == "--match-ticks");
    OA_CHECK(conflict({"--mcp", "--render-script", "film.oascript"}) == "--render-script");
    OA_CHECK(conflict({"--mcp", "--showcase", "skirmish-battle"}) == "--showcase");
    OA_CHECK(conflict({"--mcp", "--check-navigation"}) == "a --check-* option");
    OA_CHECK(conflict({"--mcp", "--check-multiplayer-menu"}) == "a --check-* option");
    OA_CHECK(conflict({"--mcp", "--check-load-save"}) == "a --check-* option");
    OA_CHECK(conflict({"--mcp-http", "--headless-check"}) == "--headless-check");
}

/// The usage line names --mcp and does not replace another extension's note.
void check_usage() {
    oa::app::Extension table{};
    mcp::McpOptions options{};
    mcp::fill_option_hooks(table, options);
    const char* runs = table.text(&options, oa::app::ExtensionText::usage_runs);
    OA_CHECK(runs != nullptr);
    OA_CHECK(std::string(runs).find("[--mcp[=observe]]") != std::string::npos);
    OA_CHECK(std::string(runs).find("[--mcp-clock stepped|realtime]") != std::string::npos);
    OA_CHECK(
        std::string(runs).find("[--mcp-http [127.0.0.1:PORT|[::1]:PORT]]") != std::string::npos
    );
    OA_CHECK(table.text(&options, oa::app::ExtensionText::usage_note) == nullptr);
}

/// --mcp-http marks the run, takes only a loopback address, and does not
/// turn on the standard-stream server.
void check_http() {
    const Parsed off = parse({});
    OA_CHECK(!off.mcp.http);
    OA_CHECK(!off.mcp.enabled);

    const Parsed bare = parse({"--mcp-http"});
    OA_CHECK(bare.mcp.http);
    OA_CHECK(!bare.mcp.enabled);
    OA_CHECK(!bare.mcp.observe);
    OA_CHECK(!bare.mcp.http_address.has_value());
    OA_CHECK(bare.mcp.http_listen.family == mcp::HttpFamily::ipv4);
    OA_CHECK(bare.mcp.http_listen.port == 0);
    OA_CHECK(bare.app.remote_controlled);

    const Parsed skipped = parse({"--mcp-http", "--skip-intro"});
    OA_CHECK(skipped.mcp.http);
    OA_CHECK(!skipped.mcp.http_address.has_value());
    OA_CHECK(skipped.mcp.http_listen.port == 0);
    OA_CHECK(skipped.app.skip_intro);

    const Parsed v6 = parse({"--mcp-http", "[::1]:9"});
    OA_CHECK(v6.mcp.http_listen.family == mcp::HttpFamily::ipv6);
    OA_CHECK(v6.mcp.http_listen.port == 9);

    const Parsed with_mcp = parse({"--mcp", "--mcp-http", "127.0.0.1:1"});
    OA_CHECK(with_mcp.mcp.enabled);
    OA_CHECK(with_mcp.mcp.http);
    OA_CHECK(with_mcp.mcp.http_listen.port == 1);

    const Parsed watching = parse({"--mcp=observe", "--mcp-http"});
    OA_CHECK(watching.mcp.enabled);
    OA_CHECK(watching.mcp.observe);
    OA_CHECK(watching.mcp.http);

    const Parsed beside_status = parse({"--mcp-http", "--status-stdout"});
    OA_CHECK(beside_status.mcp.http);
    OA_CHECK(!beside_status.mcp.enabled);

    OA_CHECK(
        rejection({"--mcp", "--mcp-http", "--status-stdout"}) ==
        "--mcp cannot be used with --status-stdout"
    );
    OA_CHECK(
        rejection({"--mcp-http=127.0.0.1:1"}) == "--mcp-http takes an address after it, or none"
    );
    OA_CHECK(
        rejection({"--mcp-http", "10.0.0.1:1"}) == "mcp: only a loopback address can be served"
    );
    OA_CHECK(
        rejection({"--mcp-http", "10.0.0.1:nope"}) == "mcp: only a loopback address can be served"
    );
    OA_CHECK(
        rejection({"--mcp-http", "127.0.0.1:nope"}) ==
        "--mcp-http expects a port from 0 through 65535"
    );
    OA_CHECK(
        rejection({"--mcp-http", "127.0.0.1:"}) == "--mcp-http expects a port from 0 through 65535"
    );
    OA_CHECK(
        rejection({"--mcp-http", "localhost:1"}) == "mcp: only a loopback address can be served"
    );
    OA_CHECK(
        rejection({"--mcp-http", "not-an-address"}) ==
        "--mcp-http expects 127.0.0.1:PORT or [::1]:PORT"
    );
}

/// SIGTERM leaves when no window is shown, on the dummy driver, or with --mcp-http.
void check_system_quit() {
    OA_CHECK(mcp::window_shown_to_player(true, false, false, "cocoa"));
    OA_CHECK(!mcp::window_shown_to_player(false, false, false, "cocoa"));
    OA_CHECK(!mcp::window_shown_to_player(true, true, false, "cocoa"));
    OA_CHECK(!mcp::window_shown_to_player(true, false, true, "cocoa"));
    OA_CHECK(!mcp::window_shown_to_player(true, false, false, "dummy"));
    OA_CHECK(mcp::window_shown_to_player(true, false, false, ""));

    const bool shown = mcp::window_shown_to_player(true, false, false, "cocoa");
    const bool dummy = mcp::window_shown_to_player(true, false, false, "dummy");
    OA_CHECK(!mcp::system_quit_leaves(shown, false));
    OA_CHECK(mcp::system_quit_leaves(shown, true));
    OA_CHECK(mcp::system_quit_leaves(dummy, false));
    OA_CHECK(mcp::system_quit_leaves(false, false));
}

} // namespace

int main() {
    check_taken();
    check_refused();
    check_conflicts();
    check_usage();
    check_http();
    check_system_quit();
    return oa::test::check_exit_status();
}
