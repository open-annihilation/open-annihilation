// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --mcp, --mcp-http and --mcp-clock (options.hpp). --mcp is taken here, before
// check_options, so a piped launch is disarmed before network play reads
// standard input. --mcp-http does not touch those streams. --mcp-clock needs
// --mcp or --mcp-http, checked once the line is read. It does not itself mark
// the run as one a program may control.
#include "oa/app/mcp/options.hpp"

#include "oa/app/mcp_quit.hpp"

#include "oa/app/app.hpp"
#include "oa/app/command_line.hpp"
#include "net_options.hpp"
#include "options.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

namespace oa::app::mcp {
namespace {

/// Returns the McpOptions behind an extension context pointer.
///
/// @param context the extension table's context
/// @return the options it points to
McpOptions& options_of(void* context) {
    return *static_cast<McpOptions*>(context);
}

/// The options check_options last accepted.
///
/// @return the pointer; null until then
McpOptions*& bound_options() noexcept {
    static McpOptions* bound = nullptr;
    return bound;
}

/// The switch handler bound to the options fill_option_hooks was given.
///
/// One handler is enough: the game has one MCP extension, and a test parses
/// one command line at a time.
///
/// @return the handler
command_line::SwitchHandler& switch_handler_slot() noexcept {
    static command_line::SwitchHandler handler{};
    return handler;
}

/// Stops network play reading standard input for a piped launch, and notes
/// a --launch - already taken.
///
/// @param[in,out] options gains stdin_launch when --launch - was taken
void disarm_stdin(McpOptions& options) {
    auto& net = oa::app::netgame_context();
    net.options.piped_launch_allowed = false;
    if (net.options.launch_path == "-") {
        net.options.launch_path.clear();
        options.stdin_launch = true;
    }
}

/// Takes -c when its file is standard input and --mcp is on.
///
/// The handler is asked before network play's, because this extension is
/// registered after it and a switch is offered to the last extension first.
/// Taking "-c -" here means network play does not read standard input. Any
/// other -c is left for network play, and -c is left for it when --mcp is off.
///
/// @param context the McpOptions
/// @param letter the switch letter
/// @param arguments the text after the letter
/// @param effects unused; refusing the run does not change them
/// @return 1 when this handler took -c -
int take_switch(
    void* context,
    char letter,
    const command_line::SwitchArguments* arguments,
    uint32_t* /*effects*/
) {
    if (letter != 'c' || arguments == nullptr)
        return 0;
    auto& options = options_of(context);
    bool attached = false;
    const char* token = arguments->attached;
    if (token != nullptr && token[0] != '\0')
        attached = true;
    else if (arguments->peek_token != nullptr)
        token = arguments->peek_token(arguments->tokens);
    else
        token = nullptr;
    if (token == nullptr || std::string_view(token) != "-")
        return 0;
    if (!options.enabled)
        return 0;
    if (!attached && arguments->next_token != nullptr)
        (void)arguments->next_token(arguments->tokens);
    options.stdin_launch = true;
    return 1;
}

/// Returns the switch handler (Extension::switch_handler).
///
/// @param context the McpOptions
/// @return the handler
const command_line::SwitchHandler* switch_handler(void* context) {
    command_line::SwitchHandler& handler = switch_handler_slot();
    handler.context = context;
    handler.take = take_switch;
    handler.reset = nullptr;
    return &handler;
}

/// Reads stepped or realtime, or nothing when the text is neither.
///
/// @param text the value --mcp-clock was given
/// @return the clock, or nothing when the text is not one of the two
std::optional<McpClock> clock_named(std::string_view text) {
    if (text == "stepped")
        return McpClock::stepped;
    if (text == "realtime")
        return McpClock::realtime;
    return std::nullopt;
}

/// Takes --mcp, --mcp-http and --mcp-clock (Extension::take_option).
///
/// --mcp and --mcp=observe mark the run as one a program may control.
/// A value other than observe is refused. When --mcp is on, --launch -
/// is taken here so network play does not read standard input, and a
/// piped launch is disarmed. --mcp-http marks the same way and may be
/// followed by 127.0.0.1:PORT or [::1]:PORT. An address joined with '='
/// is refused. A following argument that starts with '-' is left alone,
/// and the default address is used. --mcp-clock stepped holds a
/// single-player match until clock.step or clock.run asks; realtime is
/// the wall clock. It needs --mcp or --mcp-http, which check_options
/// requires, so the options may come in either order. It does not mark
/// the run by itself. Both transports share the one clock it names.
///
/// @param context the McpOptions
/// @param name the option with its dashes
/// @param values the arguments that follow the option
/// @param[in,out] effects gains option_effect::remote_controlled
/// @return false when the option is not this extension's
bool take_option(void* context, const char* name, const OptionValues& values, uint32_t& effects) {
    auto& options = options_of(context);
    const std::string_view option(name);
    if (option == "--mcp") {
        options.enabled = true;
        effects |= option_effect::remote_controlled;
        disarm_stdin(options);
        return true;
    }
    if (option == "--mcp=observe") {
        options.enabled = true;
        options.observe = true;
        effects |= option_effect::remote_controlled;
        disarm_stdin(options);
        return true;
    }
    if (option.starts_with("--mcp="))
        throw std::runtime_error("--mcp takes observe");
    if (option == "--mcp-http") {
        options.http = true;
        effects |= option_effect::remote_controlled;
        if (values.following != nullptr) {
            const char* following = values.following(values.arguments);
            if (following != nullptr && following[0] != '\0' && following[0] != '-') {
                (void)values.next(values.arguments);
                options.http_address = following;
            }
        }
        return true;
    }
    if (option.starts_with("--mcp-http"))
        throw std::runtime_error("--mcp-http takes an address after it, or none");
    if (option == "--mcp-clock" || option.starts_with("--mcp-clock=")) {
        std::string_view value;
        bool take_next = false;
        if (option == "--mcp-clock") {
            const char* following =
                values.following != nullptr ? values.following(values.arguments) : nullptr;
            if (following == nullptr || following[0] == '\0')
                throw std::runtime_error("--mcp-clock takes stepped or realtime");
            value = following;
            take_next = true;
        } else {
            value = option.substr(std::string_view("--mcp-clock=").size());
        }
        const std::optional<McpClock> clock = clock_named(value);
        if (!clock)
            throw std::runtime_error("--mcp-clock takes stepped or realtime");
        if (take_next && values.next != nullptr)
            (void)values.next(values.arguments);
        options.clock = *clock;
        options.clock_set = true;
        return true;
    }
    if (option == "--launch" && options.enabled && values.following != nullptr) {
        const char* following = values.following(values.arguments);
        if (following != nullptr && std::string_view(following) == "-") {
            (void)values.next(values.arguments);
            options.stdin_launch = true;
            return true;
        }
    }
    return false;
}

/// Tells whether the command line names a launch read from standard input.
///
/// @return true when it does
bool stdin_launch_named() {
    const auto& net = oa::app::netgame_context();
    if (net.options.launch_path == "-")
        return true;
    if (net.launch.launch_path == "-")
        return true;
    return net.options.launch_source == "standard input";
}

/// Checks --mcp against the options that share its streams, reads the
/// --mcp-http address, and requires a transport for --mcp-clock
/// (Extension::check_options).
///
/// The address is only read. Nothing is opened here. --mcp-clock is one
/// clock for every transport that is on.
///
/// @param context the McpOptions
void check_options(void* context) {
    auto& options = options_of(context);
    if (options.clock_set && !options.enabled && !options.http)
        throw std::runtime_error("--mcp-clock needs --mcp or --mcp-http");
    if (!options.enabled && !options.http) {
        bound_options() = &options;
        return;
    }
    if (options.enabled) {
        if (oa::app::automation::command_status_stdout())
            throw std::runtime_error("--mcp cannot be used with --status-stdout");
        if (options.stdin_launch || stdin_launch_named())
            throw std::runtime_error("--mcp cannot be used with a launch read from standard input");
        if (oa::app::netgame_context().options.list_content)
            throw std::runtime_error("--mcp cannot be used with --list-content");
    }
    if (options.http) {
        const std::string_view text =
            options.http_address ? std::string_view(*options.http_address) : default_listen_address;
        const std::string why = read_listen_address(text, options.http_listen);
        if (!why.empty())
            throw std::runtime_error(why);
    }
    bound_options() = &options;
}

/// Returns the usage text (Extension::text).
///
/// usage_note is left unset so the automation endpoint's note stays the one
/// --help prints. This text is only added to the run options.
///
/// @param which the text asked for
/// @return the run options, or null
const char* text(void* /*context*/, ExtensionText which) {
    if (which == ExtensionText::usage_runs)
        return "[--mcp[=observe]] [--mcp-http [127.0.0.1:PORT|[::1]:PORT]] "
               "[--mcp-clock stepped|realtime] ";
    return nullptr;
}

} // namespace

const McpOptions* bound_mcp_options() noexcept {
    return bound_options();
}

bool http_serving() noexcept {
    const McpOptions* options = bound_mcp_options();
    return options != nullptr && options->http;
}

void fill_option_hooks(Extension& table, McpOptions& options) {
    table.context = &options;
    table.take_option = take_option;
    table.check_options = check_options;
    table.text = text;
    table.switch_handler = switch_handler;
}

std::string_view conflicting_option(const oa::app::Options& run) {
    // The same set automation::conflicting_option names: the options that
    // run no main loop, or own it, which --fark refuses.
    if (!run.render_script.empty())
        return "--render-script";
    if (!run.generate_script.empty())
        return "--generate-script";
    if (run.showcase != Showcase::none)
        return "--showcase";
    if (run.benchmark_frames)
        return "--benchmark";
    if (run.match_ticks)
        return "--match-ticks";
    if (run.headless_check)
        return "--headless-check";
    if (run.fixed_clock || run.check_navigation || run.check_multiplayer_menu ||
        run.check_briefing_narration || run.check_game_files)
        return "a --check-* option";
    return {};
}

} // namespace oa::app::mcp
