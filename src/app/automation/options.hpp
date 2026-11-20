// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The automation endpoint's command line: --fark, which turns the endpoint
// on, --fark-address, the loopback address it listens on, and --fark-file,
// where its address and token are written; their checks, and the engine
// options a run with the endpoint cannot be used with.
#pragma once

#include "oa/app/extension.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace oa::app {
struct Options;
}

namespace oa::app::automation {

/// The address the endpoint listens on when --fark-address names none:
/// 127.0.0.1, at a port the system chooses.
inline constexpr std::string_view default_listen_address = "127.0.0.1:0";

/// The name of the file the endpoint's address and token are written to when
/// --fark-file names none, in the folder of the preferences file.
inline constexpr std::string_view default_endpoint_file_name = "automation.json";

/// The loopback address a listener is bound to.
enum class LoopbackFamily : uint8_t {
    ipv4, ///< 127.0.0.1
    ipv6, ///< ::1
};

/// A loopback address and port the endpoint listens on.
struct ListenAddress {
    LoopbackFamily family = LoopbackFamily::ipv4;
    uint16_t port{}; ///< 0 lets the system choose one
};

/// When --quit-after-game ends the run.
enum class QuitAfter : uint8_t {
    none,  ///< --quit-after-game was not given
    game,  ///< once the game has ended
    score, ///< once the player leaves the score screen
};

/// What the command line asked of the endpoint.
struct EndpointOptions {
    bool enabled{};                                   ///< --fark was given
    std::optional<std::string> address;               ///< --fark-address as given
    std::optional<std::filesystem::path> file;        ///< --fark-file as given
    ListenAddress listen{};                           ///< the address check_options read
    bool status_stdout{};                             ///< --status-stdout was given
    std::optional<std::filesystem::path> status_file; ///< --status-file as given
    QuitAfter quit_after{QuitAfter::none};            ///< --quit-after-game
    /// --status-stdout was on the command line, so a launch file does not
    /// replace standard output.
    bool status_stdout_on_command{};
    /// --status-file was on the command line, so a launch file does not
    /// replace the file.
    bool status_file_on_command{};
    /// --quit-after-game was on the command line, so a launch file does not
    /// replace when the run leaves.
    bool quit_after_on_command{};
};

/// Reads a listen address: 127.0.0.1:PORT or [::1]:PORT, PORT from 0 to 65535.
///
/// @param text the address
/// @param[out] address the address read; unchanged on failure
/// @return an empty text when the address was read; otherwise why it was
///         refused: "automation: only a loopback address can be served" for
///         another host, or a description of the form expected
[[nodiscard]] std::string read_listen_address(std::string_view text, ListenAddress& address);

/// Writes an address as the endpoint gives it: 127.0.0.1:PORT or [::1]:PORT.
///
/// @param family the loopback address
/// @param port the port
/// @return the text
[[nodiscard]] std::string address_text(LoopbackFamily family, uint16_t port);

/// Fills the extension hooks that need no running game: the options, their
/// check as a whole and the usage text.
///
/// @param[in,out] table the extension's table; context, take_option,
///        check_options and text are set
/// @param[in,out] options what the hooks read and write; becomes table.context
void fill_option_hooks(Extension& table, EndpointOptions& options);

/// Tells whether the checked command line asked for status on standard output.
///
/// @return true after check_options when --status-stdout was given
[[nodiscard]] bool command_status_stdout() noexcept;

/// Tells whether the checked command line asked for status lines.
///
/// @return true for --status-stdout or --status-file
[[nodiscard]] bool command_status_lines() noexcept;

/// Tells whether the checked command line follows the match for a launcher.
///
/// @return true for status lines or --quit-after-game
[[nodiscard]] bool command_status_following() noexcept;

/// Returns the file the checked command line named for status lines.
///
/// @return the path; nothing when --status-file was not given
[[nodiscard]] const std::optional<std::filesystem::path>& command_status_file() noexcept;

/// Returns when the checked command line leaves after a game.
///
/// @return the option; none when it was not given
[[nodiscard]] QuitAfter command_quit_after() noexcept;

/// Names the engine option a run with the endpoint cannot be used with:
/// one that runs no main loop or owns it (--headless-check, the checks,
/// --benchmark, --match-ticks, --render-script, --generate-script,
/// --replay-index and --showcase), since the endpoint is served from the main
/// loop.
///
/// @param options the run's options
/// @return the option's name, or "a --check-* option" for a check; empty
///         when there is none
[[nodiscard]] std::string_view conflicting_option(const oa::app::Options& options);

} // namespace oa::app::automation
