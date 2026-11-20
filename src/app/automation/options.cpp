// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The automation endpoint's options, their checks and their usage text
// (options.hpp).
#include "options.hpp"

#include "oa/app/app.hpp"
#include "oa/app/netgame/launch_file.hpp"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <string_view>

namespace oa::app::automation {

/// The options check_options accepted, for the status stream.
///
/// @return the pointer; null until a command line is accepted
EndpointOptions*& bound_options() noexcept;

namespace {

// The hosts a listen address may name, as written.
constexpr std::string_view kIpv4Loopback = "127.0.0.1";
constexpr std::string_view kIpv6Loopback = "[::1]";
// The most decimal digits of a port.
constexpr size_t kMaxPortDigits = 5;
constexpr uint32_t kMaxPort = 65535;

// The refusal of an address that is not this machine's loopback address.
constexpr const char* kLoopbackOnly = "automation: only a loopback address can be served";

/// Returns the EndpointOptions behind an extension context pointer.
///
/// @param context the extension table's context
/// @return the options it points to
EndpointOptions& options_of(void* context) {
    return *static_cast<EndpointOptions*>(context);
}

/// Takes one of the endpoint's long options (Extension::take_option).
///
/// --fark is a switch, and makes the run one a program on this machine may
/// control; --fark-address and --fark-file take a value each. A missing
/// value throws std::runtime_error.
///
/// @param context the EndpointOptions that receive the option
/// @param name the option with its dashes
/// @param values the arguments that follow the option
/// @param[in,out] effects gains option_effect::remote_controlled for --fark
/// @return false when the option is not the endpoint's
bool take_option(void* context, const char* name, const OptionValues& values, uint32_t& effects) {
    auto& options = options_of(context);
    const std::string_view option(name);
    if (option == "--fark") {
        options.enabled = true;
        effects |= option_effect::remote_controlled;
    } else if (option == "--fark-address") {
        options.address = std::string(values.next(values.arguments));
    } else if (option == "--fark-file") {
        const std::string_view file = values.next(values.arguments);
        options.file = std::filesystem::path(std::u8string(file.begin(), file.end()));
    } else if (option == "--status-stdout") {
        options.status_stdout = true;
        options.status_stdout_on_command = true;
    } else if (option == "--status-file") {
        const std::string_view file = values.next(values.arguments);
        options.status_file = std::filesystem::path(std::u8string(file.begin(), file.end()));
        options.status_file_on_command = true;
    } else if (option == "--quit-after-game") {
        options.quit_after = QuitAfter::game;
        options.quit_after_on_command = true;
    } else if (option == "--quit-after-game=after-score") {
        options.quit_after = QuitAfter::score;
        options.quit_after_on_command = true;
    } else if (option.starts_with("--quit-after-game=")) {
        throw std::runtime_error("--quit-after-game takes after-score");
    } else {
        return false;
    }
    return true;
}

/// A path as a filesystem path, from its UTF-8 bytes.
///
/// @param text the path as the launch gave it
/// @return the path
std::filesystem::path path_from_utf8(std::string_view text) {
    return std::filesystem::path(std::u8string(text.begin(), text.end()));
}

/// Turns on what the launch's status block asked, where the command line
/// did not name that field itself.
///
/// Network play reads the launch, from a file, -c, --launch or standard
/// input, and keeps its status block before this check runs.
///
/// @param[in,out] options the options the command line set
void apply_launch_status(EndpointOptions& options) {
    namespace netgame = oa::app::netgame;
    const netgame::LaunchStatusChoice& status = netgame::kept_launch_status();
    if (!options.status_stdout_on_command && status.standard_output_named)
        options.status_stdout = status.standard_output;
    if (!options.status_file_on_command && !status.file.empty())
        options.status_file = path_from_utf8(status.file);
    if (!options.quit_after_on_command) {
        if (status.quit_after == netgame::QuitAfter::game)
            options.quit_after = QuitAfter::game;
        else if (status.quit_after == netgame::QuitAfter::after_score)
            options.quit_after = QuitAfter::score;
        else if (status.quit_after == netgame::QuitAfter::off)
            options.quit_after = QuitAfter::none;
    }
}

/// Checks the endpoint's options as a whole (Extension::check_options).
///
/// --fark-address and --fark-file need --fark, and the address must be
/// this machine's loopback address; a refusal throws std::runtime_error.
/// The launch's status block, which network play kept, is applied first.
///
/// @param context the EndpointOptions checked; their listen address is read
void check_options(void* context) {
    auto& options = options_of(context);
    apply_launch_status(options);
    if (options.status_file && options.status_file->empty())
        throw std::runtime_error("--status-file needs a file");
    if (!options.enabled) {
        if (options.address)
            throw std::runtime_error("--fark-address needs --fark");
        if (options.file)
            throw std::runtime_error("--fark-file needs --fark");
        bound_options() = &options;
        return;
    }
    const std::string problem = read_listen_address(
        options.address ? std::string_view(*options.address) : default_listen_address,
        options.listen
    );
    if (!problem.empty())
        throw std::runtime_error(problem);
    if (options.file && options.file->empty())
        throw std::runtime_error("--fark-file needs a file");
    bound_options() = &options;
}

/// Returns the endpoint's usage text (Extension::text).
///
/// @param which the text asked for
/// @return the options for the run options' line and the line after the
///         usage; null for the others, which keep the engine's wording or
///         another extension's
const char* text(void* /*context*/, ExtensionText which) {
    if (which == ExtensionText::usage_runs)
        return "[--fark [--fark-address 127.0.0.1:PORT|[::1]:PORT] [--fark-file PATH]] "
               "[--status-stdout] [--status-file PATH] [--quit-after-game[=after-score]] ";
    if (which == ExtensionText::usage_note)
        return "--fark serves the automation endpoint, through which a program on this "
               "machine drives the game. --status-stdout and --status-file report the "
               "game as lines of JSON, and --quit-after-game leaves once it has ended "
               "(docs/automation.md).\n";
    return nullptr;
}

} // namespace

EndpointOptions*& bound_options() noexcept {
    static EndpointOptions* bound = nullptr;
    return bound;
}

namespace {

const std::optional<std::filesystem::path> kNoStatusFile;

} // namespace

bool command_status_stdout() noexcept {
    const EndpointOptions* bound = bound_options();
    return bound != nullptr && bound->status_stdout;
}

bool command_status_lines() noexcept {
    const EndpointOptions* bound = bound_options();
    return bound != nullptr && (bound->status_stdout || bound->status_file.has_value());
}

bool command_status_following() noexcept {
    const EndpointOptions* bound = bound_options();
    return bound != nullptr && (bound->status_stdout || bound->status_file.has_value() ||
                                bound->quit_after != QuitAfter::none);
}

const std::optional<std::filesystem::path>& command_status_file() noexcept {
    const EndpointOptions* bound = bound_options();
    return bound != nullptr ? bound->status_file : kNoStatusFile;
}

QuitAfter command_quit_after() noexcept {
    const EndpointOptions* bound = bound_options();
    return bound != nullptr ? bound->quit_after : QuitAfter::none;
}

std::string read_listen_address(std::string_view text, ListenAddress& address) {
    const size_t colon = text.rfind(':');
    if (colon == std::string_view::npos || colon == 0)
        return "--fark-address expects 127.0.0.1:PORT or [::1]:PORT";
    const std::string_view host = text.substr(0, colon);
    const std::string_view port_text = text.substr(colon + 1);
    LoopbackFamily family = LoopbackFamily::ipv4;
    if (host == kIpv4Loopback)
        family = LoopbackFamily::ipv4;
    else if (host == kIpv6Loopback)
        family = LoopbackFamily::ipv6;
    else
        return kLoopbackOnly;
    uint32_t port = 0;
    const auto [end, problem] =
        std::from_chars(port_text.data(), port_text.data() + port_text.size(), port);
    if (port_text.empty() || port_text.size() > kMaxPortDigits || problem != std::errc{} ||
        end != port_text.data() + port_text.size() || port > kMaxPort)
        return "--fark-address expects a port from 0 through 65535";
    address = {family, static_cast<uint16_t>(port)};
    return {};
}

std::string address_text(LoopbackFamily family, uint16_t port) {
    std::string text(family == LoopbackFamily::ipv6 ? kIpv6Loopback : kIpv4Loopback);
    text += ':';
    text += std::to_string(port);
    return text;
}

void fill_option_hooks(Extension& table, EndpointOptions& options) {
    table.context = &options;
    table.take_option = take_option;
    table.check_options = check_options;
    table.text = text;
}

std::string_view conflicting_option(const oa::app::Options& options) {
    // The options that imply a headless run are named before it.
    if (!options.render_script.empty())
        return "--render-script";
    if (!options.generate_script.empty())
        return "--generate-script";
    if (!options.replay_index.empty())
        return "--replay-index";
    if (options.showcase != Showcase::none)
        return "--showcase";
    if (options.benchmark_frames)
        return "--benchmark";
    if (options.match_ticks)
        return "--match-ticks";
    if (options.headless_check)
        return "--headless-check";
    if (options.fixed_clock || options.check_navigation || options.check_multiplayer_menu ||
        options.check_briefing_narration || options.check_game_files)
        return "a --check-* option";
    return {};
}

} // namespace oa::app::automation
