// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --mcp, --mcp=observe, --mcp-http and --mcp-clock, and the engine options a
// run that serves MCP cannot be used with (options.cpp).
#pragma once

#include "oa/app/extension.hpp"
#include "oa/app/mcp/http.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace oa::app {
struct Options;
}

namespace oa::app::mcp {

/// Which clock --mcp-clock asked for. realtime is the wall clock.
enum class McpClock : uint8_t {
    realtime, ///< ticks follow the wall clock
    stepped,  ///< ticks run only when clock.step or clock.run asks
};

/// What --mcp, --mcp-http and --mcp-clock asked for.
///
/// One process has one of these. When both transports are on they share this
/// clock, and a step from either client runs it.
struct McpOptions {
    bool enabled{};      ///< --mcp or --mcp=observe
    bool observe{};      ///< --mcp=observe: offer only tools that read
    bool stdin_launch{}; ///< a launch would have been read from standard input
    bool http{};         ///< --mcp-http
    /// The address written after --mcp-http. Empty means the default.
    std::optional<std::string> http_address;
    /// The loopback address check_options accepted. Unused until --mcp-http is on.
    HttpListen http_listen{};
    bool clock_set{};                   ///< --mcp-clock was given
    McpClock clock{McpClock::realtime}; ///< the clock --mcp-clock named; realtime when it was not
};

/// Fills the hooks that need no running game: the option, its check and the usage text.
///
/// @param[in,out] table the extension table
/// @param[in,out] options the options the hooks read and write; becomes table.context
void fill_option_hooks(Extension& table, McpOptions& options);

/// Returns the options check_options accepted.
///
/// @return the options; null until a command line is accepted
[[nodiscard]] const McpOptions* bound_mcp_options() noexcept;

/// Returns the engine option a run that serves MCP cannot be used with.
///
/// The set is the one --fark refuses (automation/options.cpp): an option
/// that runs no main loop, or owns it. Empty when none of them is set.
///
/// @param run the run's options
/// @return the option's name, or empty
[[nodiscard]] std::string_view conflicting_option(const oa::app::Options& run);

} // namespace oa::app::mcp
