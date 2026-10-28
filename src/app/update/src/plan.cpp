// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/app/update/plan.hpp"

#include "json_fields.hpp"

#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app::update {

using namespace fields;

const char* action_name(Action action) noexcept {
    switch (action) {
    case Action::install:
        return "install";
    case Action::roll_back:
        return "roll-back";
    }
    return "";
}

std::optional<Action> parse_action(std::string_view text) noexcept {
    if (text == "install")
        return Action::install;
    if (text == "roll-back")
        return Action::roll_back;
    return std::nullopt;
}

const char* roll_back_reason_name(RollBackReason reason) noexcept {
    switch (reason) {
    case RollBackReason::none:
        return "";
    case RollBackReason::failed_starts:
        return "failed-starts";
    case RollBackReason::player:
        return "player";
    }
    return "";
}

std::optional<RollBackReason> parse_roll_back_reason(std::string_view text) noexcept {
    if (text == "failed-starts")
        return RollBackReason::failed_starts;
    if (text == "player")
        return RollBackReason::player;
    return std::nullopt;
}

const char* route_name(Route route) noexcept {
    switch (route) {
    case Route::check_folder:
        return "check-folder";
    case Route::macos_bundle:
        return "macos-bundle";
    case Route::windows_installer:
        return "windows-installer";
    case Route::windows_folder:
        return "windows-folder";
    case Route::linux_versions:
        return "linux-versions";
    }
    return "";
}

std::optional<Route> parse_route(std::string_view text) noexcept {
    if (text == "check-folder")
        return Route::check_folder;
    if (text == "macos-bundle")
        return Route::macos_bundle;
    if (text == "windows-installer")
        return Route::windows_installer;
    if (text == "windows-folder")
        return Route::windows_folder;
    if (text == "linux-versions")
        return Route::linux_versions;
    return std::nullopt;
}

std::optional<Plan> read_plan(std::span<const uint8_t> bytes, std::string* error) {
    const std::optional<json::Json> root = parse_object(bytes, error);
    if (!root)
        return std::nullopt;
    Plan plan;
    int64_t format = 0;
    if (!require_integer(*root, "plan", "the plan", format, error))
        return std::nullopt;
    if (format != plan_format) {
        set_error(error, "the plan is not plan 1");
        return std::nullopt;
    }
    plan.plan = format;

    std::string action;
    if (!require_string(*root, "action", "the plan", action, error))
        return std::nullopt;
    const std::optional<Action> parsed_action = parse_action(action);
    if (!parsed_action) {
        set_error(error, "the action is not known");
        return std::nullopt;
    }
    plan.action = *parsed_action;

    if (const json::Json* reason = member_of(*root, "roll_back_reason")) {
        const std::string* text = reason->string();
        if (text == nullptr) {
            set_error(error, "roll_back_reason is not text");
            return std::nullopt;
        }
        const std::optional<RollBackReason> parsed_reason = parse_roll_back_reason(*text);
        if (!parsed_reason) {
            set_error(error, "the roll-back reason is not known");
            return std::nullopt;
        }
        plan.roll_back_reason = *parsed_reason;
    }
    if (plan.action == Action::roll_back && plan.roll_back_reason == RollBackReason::none) {
        set_error(error, "the plan has no roll_back_reason");
        return std::nullopt;
    }
    if (plan.action == Action::install && plan.roll_back_reason != RollBackReason::none) {
        set_error(error, "an install has a roll_back_reason");
        return std::nullopt;
    }

    std::string route;
    if (!require_string(*root, "route", "the plan", route, error))
        return std::nullopt;
    const std::optional<Route> parsed_route = parse_route(route);
    if (!parsed_route) {
        set_error(error, "the route is not known");
        return std::nullopt;
    }
    plan.route = *parsed_route;

    if (!require_integer(*root, "wait_for", "the plan", plan.wait_for, error))
        return std::nullopt;
    if (plan.wait_for < 0) {
        set_error(error, "wait_for is not a process");
        return std::nullopt;
    }
    if (!require_absolute(*root, "install", "the plan", plan.install, error) ||
        !require_absolute(*root, "program", "the plan", plan.program, error))
        return std::nullopt;

    const json::Json* arguments = require_member(*root, "arguments", "the plan", error);
    if (arguments == nullptr)
        return std::nullopt;
    if (arguments->type() != json::JsonType::array) {
        set_error(error, "arguments is not a list");
        return std::nullopt;
    }
    constexpr std::size_t max_arguments = 64;
    if (arguments->elements().size() > max_arguments) {
        set_error(error, "arguments has too many entries");
        return std::nullopt;
    }
    for (const json::Json& element : arguments->elements()) {
        const std::string* text = element.string();
        if (text == nullptr) {
            set_error(error, "an argument is not text");
            return std::nullopt;
        }
        if (text->size() > 4096) {
            set_error(error, "an argument is too long");
            return std::nullopt;
        }
        plan.arguments.push_back(*text);
    }

    if (!require_version(*root, "from_version", "the plan", plan.from_version, error) ||
        !require_absolute(*root, "feed", "the plan", plan.feed, error) ||
        !require_absolute(*root, "signature", "the plan", plan.signature, error) ||
        !require_integer(*root, "release", "the plan", plan.release, error))
        return std::nullopt;
    if (plan.release < 0) {
        set_error(error, "release is not a release number");
        return std::nullopt;
    }

    std::string package;
    std::string file;
    if (!require_string(*root, "package", "the plan", package, error) ||
        !require_absolute(*root, "file", "the plan", file, error) ||
        !require_bool(*root, "installer", "the plan", plan.installer, error) ||
        !require_absolute(*root, "preferences", "the plan", plan.preferences, error) ||
        !require_absolute(*root, "updates", "the plan", plan.updates, error) ||
        !require_line(*root, "title", "the plan", plan.title, error) ||
        !require_line(*root, "line", "the plan", plan.line, error))
        return std::nullopt;
    if (package.empty()) {
        set_error(error, "package is empty");
        return std::nullopt;
    }
    plan.package = std::move(package);
    plan.file = std::move(file);
    return plan;
}

std::string windows_command_argument(std::string_view argument) {
    const bool plain = !argument.empty() && argument.back() != '\\' &&
                       argument.find_first_of(" \t\n\v\"") == std::string_view::npos;
    if (plain)
        return std::string(argument);
    std::string quoted;
    quoted.push_back('"');
    std::size_t index = 0;
    while (index < argument.size()) {
        std::size_t slashes = 0;
        while (index < argument.size() && argument[index] == '\\') {
            ++slashes;
            ++index;
        }
        if (index == argument.size()) {
            quoted.append(slashes * 2, '\\');
            break;
        }
        if (argument[index] == '"')
            quoted.append(slashes * 2 + 1, '\\');
        else
            quoted.append(slashes, '\\');
        quoted.push_back(argument[index]);
        ++index;
    }
    quoted.push_back('"');
    return quoted;
}

std::string
windows_helper_command(std::string_view program, std::span<const std::string> arguments) {
    std::string line = windows_command_argument(program);
    for (const std::string& argument : arguments) {
        line.push_back(' ');
        line += windows_command_argument(argument);
    }
    return line;
}

std::string plan_text(const Plan& plan) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("plan");
    writer.integer(plan.plan);
    writer.key("action");
    writer.string(action_name(plan.action));
    if (plan.roll_back_reason != RollBackReason::none) {
        writer.key("roll_back_reason");
        writer.string(roll_back_reason_name(plan.roll_back_reason));
    }
    writer.key("route");
    writer.string(route_name(plan.route));
    writer.key("wait_for");
    writer.integer(plan.wait_for);
    writer.key("install");
    writer.string(plan.install);
    writer.key("program");
    writer.string(plan.program);
    writer.key("arguments");
    writer.begin_array();
    for (const std::string& argument : plan.arguments)
        writer.string(argument);
    writer.end_array();
    writer.key("from_version");
    writer.string(plan.from_version);
    writer.key("feed");
    writer.string(plan.feed);
    writer.key("signature");
    writer.string(plan.signature);
    writer.key("release");
    writer.integer(plan.release);
    writer.key("package");
    writer.string(plan.package);
    writer.key("file");
    writer.string(plan.file);
    writer.key("installer");
    writer.boolean(plan.installer);
    writer.key("preferences");
    writer.string(plan.preferences);
    writer.key("updates");
    writer.string(plan.updates);
    writer.key("title");
    writer.string(plan.title);
    writer.key("line");
    writer.string(plan.line);
    writer.end_object();
    return writer.text() + "\n";
}

std::optional<State> read_state(std::span<const uint8_t> bytes, std::string* error) {
    const std::optional<json::Json> root = parse_object(bytes, error);
    if (!root)
        return std::nullopt;
    State state;
    if (!require_version(*root, "installed_version", "the state", state.installed_version, error) ||
        !require_integer(*root, "installed_release", "the state", state.installed_release, error))
        return std::nullopt;
    if (state.installed_release < 0) {
        set_error(error, "installed_release is not a release number");
        return std::nullopt;
    }
    if (!require_string(*root, "kept_version", "the state", state.kept_version, error))
        return std::nullopt;
    const bool nothing_kept = state.kept_version.empty();
    if (!nothing_kept && !oa::formats::oamod::parse_engine_version(state.kept_version)) {
        set_error(error, "kept_version is not a version");
        return std::nullopt;
    }
    if (nothing_kept) {
        if (!require_string(*root, "kept_path", "the state", state.kept_path, error) ||
            !require_string(*root, "preferences_copy", "the state", state.preferences_copy, error))
            return std::nullopt;
        if (!state.kept_path.empty() || !state.preferences_copy.empty()) {
            set_error(error, "a state with nothing kept still names a path");
            return std::nullopt;
        }
    } else if (!require_absolute(*root, "kept_path", "the state", state.kept_path, error)) {
        return std::nullopt;
    }
    std::string route;
    if (!require_string(*root, "kept_route", "the state", route, error))
        return std::nullopt;
    const std::optional<Route> parsed_route = parse_route(route);
    if (!parsed_route) {
        set_error(error, "the route is not known");
        return std::nullopt;
    }
    state.kept_route = *parsed_route;
    if (!nothing_kept &&
        !require_absolute(*root, "preferences_copy", "the state", state.preferences_copy, error))
        return std::nullopt;
    if (!require_bool(*root, "confirmed", "the state", state.confirmed, error))
        return std::nullopt;
    int64_t failed = 0;
    if (!require_integer(*root, "failed_starts", "the state", failed, error))
        return std::nullopt;
    if (failed < 0 || failed > static_cast<int64_t>(std::numeric_limits<int32_t>::max())) {
        set_error(error, "failed_starts is out of range");
        return std::nullopt;
    }
    state.failed_starts = static_cast<int32_t>(failed);
    if (member_of(*root, "quit_before_menu") != nullptr &&
        !require_bool(*root, "quit_before_menu", "the state", state.quit_before_menu, error))
        return std::nullopt;
    return state;
}

std::string state_text(const State& state) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("installed_version");
    writer.string(state.installed_version);
    writer.key("installed_release");
    writer.integer(state.installed_release);
    writer.key("kept_version");
    writer.string(state.kept_version);
    writer.key("kept_path");
    writer.string(state.kept_path);
    writer.key("kept_route");
    writer.string(route_name(state.kept_route));
    writer.key("preferences_copy");
    writer.string(state.preferences_copy);
    writer.key("confirmed");
    writer.boolean(state.confirmed);
    writer.key("failed_starts");
    writer.integer(state.failed_starts);
    if (state.quit_before_menu) {
        writer.key("quit_before_menu");
        writer.boolean(true);
    }
    writer.end_object();
    return writer.text() + "\n";
}

bool write_state(const fs::path& path, const State& state, std::string* error) {
    return replace_file(path, state_text(state), "the state file", error);
}

void confirm_state(State& state) noexcept {
    state.confirmed = true;
    state.failed_starts = 0;
    state.quit_before_menu = false;
}

int32_t count_failed_start(State& state) noexcept {
    if (state.failed_starts < std::numeric_limits<int32_t>::max())
        state.failed_starts += 1;
    state.quit_before_menu = false;
    return state.failed_starts;
}

void uncount_quit_start(State& state) noexcept {
    if (state.confirmed)
        return;
    if (state.failed_starts > 0)
        state.failed_starts -= 1;
    state.quit_before_menu = true;
}

StartEnd start_end(const std::optional<State>& state) noexcept {
    if (!state.has_value())
        return StartEnd::failed;
    if (state->confirmed)
        return StartEnd::reached_menu;
    if (state->quit_before_menu)
        return StartEnd::quit;
    return StartEnd::failed;
}

} // namespace oa::app::update
