// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The plan and the state file, from the game's side. A scratch folder holds
// the state file. Nothing is installed.

#include "oa/app/update/plan.hpp"

#include "oa/test/check.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace update = oa::app::update;

/// A scratch folder removed when the case ends.
struct Scratch {
    fs::path path;

    explicit Scratch(std::string_view name) {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
               (std::string("oa-v07-") + std::string(name) + "-" + std::to_string(ticks));
        fs::create_directories(path);
    }

    ~Scratch() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

/// A path as the plan stores it, with forward slashes.
std::string spell(const fs::path& path) {
    const std::u8string text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::vector<uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

update::Plan sample(const Scratch& scratch) {
    update::Plan plan;
    plan.plan = update::plan_format;
    plan.action = update::Action::install;
    plan.route = update::Route::check_folder;
    plan.wait_for = 0;
    plan.install = spell(scratch.path / "install");
    plan.program = spell(scratch.path / "program");
    plan.arguments = {"--skip-intro", "--mute"};
    plan.from_version = "0.0.1";
    plan.feed = spell(scratch.path / "feed.json");
    plan.signature = spell(scratch.path / "feed.json.sig");
    plan.release = 2;
    plan.package = "linux-x86_64";
    plan.file = spell(scratch.path / "open-annihilation-0.0.2-linux-x86_64.zip");
    plan.installer = false;
    plan.preferences = spell(scratch.path / "preferences.conf");
    plan.updates = spell(scratch.path / "updates");
    plan.title = "Installing 0.0.2";
    plan.line = "0.0.1 is kept.";
    return plan;
}

update::State sample_state(const Scratch& scratch) {
    update::State state;
    state.installed_version = "0.0.2";
    state.installed_release = 2;
    state.kept_version = "0.0.1";
    state.kept_path = spell(scratch.path / "previous");
    state.kept_route = update::Route::check_folder;
    state.preferences_copy = spell(scratch.path / "previous" / "preferences.conf");
    state.confirmed = false;
    state.failed_starts = 0;
    return state;
}

bool same_plan(const update::Plan& left, const update::Plan& right) {
    return left.plan == right.plan && left.action == right.action &&
           left.roll_back_reason == right.roll_back_reason && left.route == right.route &&
           left.wait_for == right.wait_for && left.install == right.install &&
           left.program == right.program && left.arguments == right.arguments &&
           left.from_version == right.from_version && left.feed == right.feed &&
           left.signature == right.signature && left.release == right.release &&
           left.package == right.package && left.file == right.file &&
           left.installer == right.installer && left.preferences == right.preferences &&
           left.updates == right.updates && left.title == right.title && left.line == right.line;
}

void names() {
    OA_CHECK(std::string_view(update::action_name(update::Action::install)) == "install");
    OA_CHECK(std::string_view(update::action_name(update::Action::roll_back)) == "roll-back");
    OA_CHECK(update::parse_action("install") == update::Action::install);
    OA_CHECK(update::parse_action("roll-back") == update::Action::roll_back);
    OA_CHECK(!update::parse_action("undo"));
    OA_CHECK(
        std::string_view(update::roll_back_reason_name(update::RollBackReason::failed_starts)) ==
        "failed-starts"
    );
    OA_CHECK(
        std::string_view(update::roll_back_reason_name(update::RollBackReason::player)) == "player"
    );
    OA_CHECK(std::string_view(update::roll_back_reason_name(update::RollBackReason::none)).empty());
    OA_CHECK(
        update::parse_roll_back_reason("failed-starts") == update::RollBackReason::failed_starts
    );
    OA_CHECK(update::parse_roll_back_reason("player") == update::RollBackReason::player);
    OA_CHECK(!update::parse_roll_back_reason("install"));
    OA_CHECK(std::string_view(update::route_name(update::Route::check_folder)) == "check-folder");
    OA_CHECK(std::string_view(update::route_name(update::Route::macos_bundle)) == "macos-bundle");
    OA_CHECK(
        std::string_view(update::route_name(update::Route::windows_installer)) ==
        "windows-installer"
    );
    OA_CHECK(
        std::string_view(update::route_name(update::Route::windows_folder)) == "windows-folder"
    );
    OA_CHECK(
        std::string_view(update::route_name(update::Route::linux_versions)) == "linux-versions"
    );
    OA_CHECK(update::parse_route("linux-versions") == update::Route::linux_versions);
    OA_CHECK(!update::parse_route("steam"));
}

void round_trip() {
    const Scratch scratch("plan");
    const update::Plan plan = sample(scratch);
    const std::string text = update::plan_text(plan);
    OA_CHECK(text.find("\"plan\":1") != std::string::npos);
    OA_CHECK(text.find("\"action\":\"install\"") != std::string::npos);
    OA_CHECK(text.find("\"route\":\"check-folder\"") != std::string::npos);
    OA_CHECK(text.find("\"installer\":false") != std::string::npos);
    const std::string order = text;
    OA_CHECK(order.find("\"plan\"") < order.find("\"action\""));
    OA_CHECK(order.find("\"line\"") > order.find("\"title\""));
    std::string error;
    const std::optional<update::Plan> loaded = update::read_plan(bytes_of(text), &error);
    OA_CHECK(loaded.has_value());
    OA_CHECK(error.empty());
    if (loaded)
        OA_CHECK(same_plan(*loaded, plan));
    if (loaded)
        OA_CHECK(update::plan_text(*loaded) == text);

    update::Plan back = plan;
    back.action = update::Action::roll_back;
    back.roll_back_reason = update::RollBackReason::failed_starts;
    back.route = update::Route::macos_bundle;
    back.installer = true;
    back.wait_for = 44;
    const std::string back_text = update::plan_text(back);
    const std::optional<update::Plan> loaded_back = update::read_plan(bytes_of(back_text), &error);
    OA_CHECK(loaded_back.has_value());
    if (loaded_back)
        OA_CHECK(same_plan(*loaded_back, back));
    OA_CHECK(back_text.find("\"roll_back_reason\":\"failed-starts\"") != std::string::npos);
    OA_CHECK(back_text.find("\"action\"") < back_text.find("\"roll_back_reason\""));
    OA_CHECK(text.find("roll_back_reason") == std::string::npos);

    update::State cleared = sample_state(scratch);
    update::confirm_state(cleared);
    OA_CHECK(cleared.confirmed);
    OA_CHECK(cleared.failed_starts == 0);
    OA_CHECK(update::count_failed_start(cleared) == 1);
    OA_CHECK(update::count_failed_start(cleared) == 2);
    cleared.failed_starts = std::numeric_limits<int32_t>::max();
    OA_CHECK(update::count_failed_start(cleared) == std::numeric_limits<int32_t>::max());
    OA_CHECK(update::failed_starts_that_roll_back == 3);

    update::State kept_none = sample_state(scratch);
    kept_none.kept_version.clear();
    kept_none.kept_path.clear();
    kept_none.preferences_copy.clear();
    kept_none.confirmed = true;
    const std::string none_text = update::state_text(kept_none);
    const std::optional<update::State> loaded_none =
        update::read_state(bytes_of(none_text), &error);
    OA_CHECK(loaded_none.has_value());
    if (loaded_none) {
        OA_CHECK(loaded_none->kept_version.empty());
        OA_CHECK(loaded_none->kept_path.empty());
        OA_CHECK(loaded_none->preferences_copy.empty());
        OA_CHECK(loaded_none->confirmed);
        OA_CHECK(loaded_none->installed_version == "0.0.2");
    }
}

void ignores_an_unknown_member() {
    const Scratch scratch("extra");
    std::string text = update::plan_text(sample(scratch));
    const std::size_t end = text.rfind('}');
    OA_CHECK(end != std::string::npos);
    text.insert(end, ",\"note\":\"kept\"");
    std::string error;
    const std::optional<update::Plan> loaded = update::read_plan(bytes_of(text), &error);
    OA_CHECK(loaded.has_value());
    if (loaded)
        OA_CHECK(loaded->package == "linux-x86_64");
}

void refuses(std::string_view name, std::string_view json, std::string_view snippet) {
    std::string error;
    const std::optional<update::Plan> loaded = update::read_plan(bytes_of(json), &error);
    OA_CHECK(!loaded.has_value());
    if (error.find(snippet) == std::string::npos) {
        std::fprintf(
            stderr,
            "expected detail containing '%.*s' for %.*s, got '%s'\n",
            static_cast<int>(snippet.size()),
            snippet.data(),
            static_cast<int>(name.size()),
            name.data(),
            error.c_str()
        );
    }
    OA_CHECK(error.find(snippet) != std::string::npos);
}

void refusals() {
    const Scratch scratch("refuse");
    const std::string good = update::plan_text(sample(scratch));
    auto with = [&](std::string_view from, std::string_view to) {
        std::string text = good;
        const std::size_t at = text.find(from);
        OA_CHECK(at != std::string::npos);
        if (at != std::string::npos)
            text.replace(at, from.size(), to);
        return text;
    };

    refuses("format", with("\"plan\":1", "\"plan\":2"), "not plan 1");
    refuses("route", with("\"route\":\"check-folder\"", "\"route\":\"steam\""), "not known");
    refuses("action", with("\"action\":\"install\"", "\"action\":\"swap\""), "not known");
    refuses(
        "program",
        with(
            std::string("\"program\":\"") + spell(scratch.path / "program") + "\"",
            "\"program\":\"\""
        ),
        "empty"
    );
    refuses(
        "install",
        with(
            std::string("\"install\":\"") + spell(scratch.path / "install") + "\"",
            "\"install\":\"install\""
        ),
        "absolute"
    );
    refuses(
        "title",
        with(
            "\"title\":\"Installing 0.0.2\"",
            "\"title\":\"" + std::string(update::max_window_line_bytes + 1, 'a') + "\""
        ),
        "too long"
    );
    refuses(
        "line",
        with(
            "\"line\":\"0.0.1 is kept.\"",
            "\"line\":\"" + std::string(update::max_window_line_bytes + 1, 'b') + "\""
        ),
        "too long"
    );
    refuses("version", with("\"from_version\":\"0.0.1\"", "\"from_version\":\"\""), "version");
    refuses("package", with("\"package\":\"linux-x86_64\"", "\"package\":\"\""), "empty");
    refuses("wait", with("\"wait_for\":0", "\"wait_for\":-1"), "process");
    refuses("release", with("\"release\":2", "\"release\":-3"), "release");
    refuses("installer", with("\"installer\":false", "\"installer\":\"no\""), "true or false");
    refuses(
        "arguments",
        with("\"arguments\":[\"--skip-intro\",\"--mute\"]", "\"arguments\":\"--mute\""),
        "list"
    );
    refuses("twice", with("\"plan\":1", "\"plan\":1,\"plan\":1"), "twice");
    refuses("object", "[]", "object");
    refuses("huge", std::string(update::max_plan_bytes + 1, ' '), "too large");

    const std::string missing = "{\"plan\":1}";
    refuses("missing", missing, "has no");

    // A relative feed, file, signature, preferences and updates.
    refuses(
        "feed",
        with(
            std::string("\"feed\":\"") + spell(scratch.path / "feed.json") + "\"",
            "\"feed\":\"feed.json\""
        ),
        "absolute"
    );
}

void state_round_trip() {
    const Scratch scratch("state");
    const update::State state = sample_state(scratch);
    const std::string text = update::state_text(state);
    OA_CHECK(text.find("\"confirmed\":false") != std::string::npos);
    OA_CHECK(text.find("\"failed_starts\":0") != std::string::npos);
    OA_CHECK(text.find("\"installed_version\"") < text.find("\"kept_version\""));
    std::string error;
    const std::optional<update::State> loaded = update::read_state(bytes_of(text), &error);
    OA_CHECK(loaded.has_value());
    if (!loaded)
        return;
    OA_CHECK(loaded->installed_version == state.installed_version);
    OA_CHECK(loaded->installed_release == state.installed_release);
    OA_CHECK(loaded->kept_version == state.kept_version);
    OA_CHECK(loaded->kept_path == state.kept_path);
    OA_CHECK(loaded->kept_route == state.kept_route);
    OA_CHECK(loaded->preferences_copy == state.preferences_copy);
    OA_CHECK(loaded->confirmed == state.confirmed);
    OA_CHECK(loaded->failed_starts == state.failed_starts);
    OA_CHECK(update::state_text(*loaded) == text);

    std::string error_write;
    const fs::path file = scratch.path / "state.json";
    OA_CHECK(update::write_state(file, state, &error_write));
    // The reader is closed before the file is replaced. Windows will not
    // replace a file that is still open.
    std::string stored;
    {
        std::ifstream in(file, std::ios::binary);
        stored.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    OA_CHECK(stored == text);
    const std::optional<update::State> from_file = update::read_state(bytes_of(stored), &error);
    OA_CHECK(from_file.has_value());
    if (from_file)
        OA_CHECK(from_file->kept_path == state.kept_path);

    // Replacing the file whole leaves the new bytes, not a mix.
    update::State again = state;
    again.confirmed = true;
    again.failed_starts = 2;
    again.kept_route = update::Route::linux_versions;
    OA_CHECK(update::write_state(file, again, &error_write));
    std::ifstream second(file, std::ios::binary);
    const std::string replaced((std::istreambuf_iterator<char>(second)), {});
    const std::optional<update::State> reread = update::read_state(bytes_of(replaced), &error);
    OA_CHECK(reread.has_value());
    if (reread) {
        OA_CHECK(reread->confirmed);
        OA_CHECK(reread->failed_starts == 2);
        OA_CHECK(reread->kept_route == update::Route::linux_versions);
    }
}

void state_refusals() {
    const Scratch scratch("state-refuse");
    const std::string good = update::state_text(sample_state(scratch));
    auto with = [&](std::string_view from, std::string_view to) {
        std::string text = good;
        const std::size_t at = text.find(from);
        OA_CHECK(at != std::string::npos);
        if (at != std::string::npos)
            text.replace(at, from.size(), to);
        return text;
    };
    auto refuse = [&](std::string_view json, std::string_view snippet) {
        std::string error;
        OA_CHECK(!update::read_state(bytes_of(json), &error));
        if (error.find(snippet) == std::string::npos) {
            std::fprintf(
                stderr,
                "expected detail containing '%.*s', got '%s'\n",
                static_cast<int>(snippet.size()),
                snippet.data(),
                error.c_str()
            );
        }
        OA_CHECK(error.find(snippet) != std::string::npos);
    };
    refuse(with("\"kept_route\":\"check-folder\"", "\"kept_route\":\"steam\""), "not known");
    refuse(with("\"confirmed\":false", "\"confirmed\":1"), "true or false");
    refuse(with("\"failed_starts\":0", "\"failed_starts\":3000000000"), "out of range");
    refuse(with("\"failed_starts\":0", "\"failed_starts\":-1"), "out of range");
    refuse(
        with(
            std::string("\"kept_path\":\"") + spell(scratch.path / "previous") + "\"",
            "\"kept_path\":\"previous\""
        ),
        "absolute"
    );
    refuse(with("\"installed_version\":\"0.0.2\"", "\"installed_version\":\"next\""), "version");
    refuse("{", "expected a member name");
}

} // namespace

int main() {
    names();
    round_trip();
    ignores_an_unknown_member();
    refusals();
    state_round_trip();
    state_refusals();
    return oa::test::check_exit_status();
}
