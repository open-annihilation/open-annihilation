// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The plan the game writes for the update helper, and the record of what
// that helper installed. The bytes are JSON. Nothing here reads a package
// or changes an install.
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::update {

/// The plan format this build reads.
inline constexpr int64_t plan_format = 1;

/// The most bytes of a plan or a state file this build reads.
inline constexpr std::size_t max_plan_bytes = 256 * 1024;

/// The most bytes of the window's title or its line.
inline constexpr std::size_t max_window_line_bytes = 200;

/// What the helper is asked to do.
enum class Action : uint8_t {
    install,   ///< put the package in place
    roll_back, ///< put the kept version back
};

/// Why a roll-back plan was written. An install plan leaves this unset.
enum class RollBackReason : uint8_t {
    none,          ///< not a roll back
    failed_starts, ///< the new version closed twice before the main menu
    player,        ///< the player pressed ROLL BACK
};

/// Starts that did not reach the main menu before the next one rolls back.
///
/// The first two starts record 1 and 2 and keep going. The third finds 3.
inline constexpr int32_t failed_starts_that_roll_back = 3;

/// How the install folder is laid out.
enum class Route : uint8_t {
    check_folder,      ///< a plain folder, for a self-check build
    macos_bundle,      ///< a macOS application bundle
    windows_installer, ///< a Windows installer
    windows_folder,    ///< a Windows folder
    linux_versions,    ///< a Linux versions folder
};

/// The plan the game writes and the helper reads.
struct Plan {
    int64_t plan{plan_format};      ///< the format, 1
    Action action{Action::install}; ///< install, or roll back
    /// Why this is a roll back. Written only when it is not `none`, so an
    /// install plan keeps the bytes it had before roll back existed.
    RollBackReason roll_back_reason{RollBackReason::none};
    Route route{Route::check_folder};   ///< which layout the install uses
    int64_t wait_for{};                 ///< the process to wait for; 0 waits for nothing
    std::string install;                ///< the folder or bundle the route replaces
    std::string program;                ///< the program to start afterwards
    std::vector<std::string> arguments; ///< that program's arguments, before the update flag
    std::string from_version;           ///< the version that is running
    std::string feed;                   ///< the saved feed
    std::string signature;              ///< the saved signature
    int64_t release{};                  ///< the release number in the feed
    std::string package;                ///< the package id in that release
    std::string file;                   ///< the downloaded package
    bool installer{};                   ///< true when file is the package's installer
    std::string preferences;            ///< the preferences file to copy
    std::string updates;                ///< the data folder's updates directory
    std::string title;                  ///< the window title, already translated
    std::string line;                   ///< the window line, already translated
};

/// What is installed, and what was kept.
struct State {
    std::string installed_version;         ///< the version now in place
    int64_t installed_release{};           ///< that version's release number
    std::string kept_version;              ///< the version kept beside it
    std::string kept_path;                 ///< where the kept version sits
    Route kept_route{Route::check_folder}; ///< how the kept version is laid out
    std::string preferences_copy;          ///< the copy of the preferences file
    bool confirmed{};                      ///< true once a later start has settled
    int32_t failed_starts{};               ///< starts of the new version that did not settle
};

/// The action as the plan writes it.
///
/// @param action the action
/// @return `install` or `roll-back`; empty for a value this build does not know
[[nodiscard]] const char* action_name(Action action) noexcept;

/// Reads an action name.
///
/// @param text `install` or `roll-back`
/// @return the action, or nothing for any other text
[[nodiscard]] std::optional<Action> parse_action(std::string_view text) noexcept;

/// The roll-back reason as the plan writes it.
///
/// @param reason the reason
/// @return `failed-starts` or `player`; empty for `none` or a value this build does not know
[[nodiscard]] const char* roll_back_reason_name(RollBackReason reason) noexcept;

/// Reads a roll-back reason.
///
/// @param text `failed-starts` or `player`
/// @return the reason, or nothing for any other text, including empty
[[nodiscard]] std::optional<RollBackReason> parse_roll_back_reason(std::string_view text) noexcept;

/// The route as the plan writes it.
///
/// @param route the route
/// @return its name; empty for a value this build does not know
[[nodiscard]] const char* route_name(Route route) noexcept;

/// Reads a route name.
///
/// @param text the name
/// @return the route, or nothing for any other text
[[nodiscard]] std::optional<Route> parse_route(std::string_view text) noexcept;

/// Reads a plan.
///
/// The bytes are one JSON object of format 1, at most max_plan_bytes.
/// A member this build does not know is ignored. A member named twice
/// refuses the plan. A path must be absolute. `title` and `line` are at
/// most max_window_line_bytes. `program` must not be empty.
///
/// @param bytes the plan's bytes
/// @param[out] error why the bytes were refused; may be null
/// @return the plan, or nothing when the bytes are not one
[[nodiscard]] std::optional<Plan> read_plan(std::span<const uint8_t> bytes, std::string* error);

/// Writes a plan in a compact form.
///
/// Members are in the order the format shows them.
///
/// @param plan the plan
/// @return the JSON text, with a trailing line end
[[nodiscard]] std::string plan_text(const Plan& plan);

/// Reads a state file.
///
/// The same rules as a plan: one JSON object, unknown members ignored, a
/// member named twice refused, paths absolute.
///
/// @param bytes the file's bytes
/// @param[out] error why the bytes were refused; may be null
/// @return the state, or nothing when the bytes are not one
[[nodiscard]] std::optional<State> read_state(std::span<const uint8_t> bytes, std::string* error);

/// Writes a state file in a compact form.
///
/// @param state the state
/// @return the JSON text, with a trailing line end
[[nodiscard]] std::string state_text(const State& state);

/// Replaces a state file whole.
///
/// The new bytes are written beside the file and then take its place, so a
/// reader sees the previous file or the whole new one.
///
/// @param path the file; its folder must already exist
/// @param state the state
/// @param[out] error why the file was not replaced; may be null
/// @return true when the file was replaced
[[nodiscard]] bool
write_state(const std::filesystem::path& path, const State& state, std::string* error);

/// Marks a start that reached the main menu.
///
/// `confirmed` becomes true and `failed_starts` becomes 0. Nothing else moves.
///
/// @param[in,out] state the state
void confirm_state(State& state) noexcept;

/// Counts one start that did not reach the main menu.
///
/// The count stops at the largest int32_t. A confirmed state is still counted
/// when the caller asks: the caller decides whether a confirmed version counts.
///
/// @param[in,out] state the state
/// @return the count after this start
int32_t count_failed_start(State& state) noexcept;

} // namespace oa::app::update
