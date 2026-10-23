// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// An installed steady-commander package: which files are kept, which
// packages are refused, and the seat a choice resolves to.

#include "oa/app/brain_seats.hpp"

#include "oa/base/sha256.hpp"
#include "oa/core/game_state.h"
#include "oa/data/script_host/script_host.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace seats = oa::app;
namespace brains = oa::sim::brains;
namespace host = oa::data::script_host;
namespace sha = oa::base::sha256;

struct Scratch {
    fs::path path;

    Scratch() : path(oa::test::make_scratch_directory("B01-brain-seats")) {}

    ~Scratch() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void write_file(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw std::runtime_error("cannot write " + path.string());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out)
        throw std::runtime_error("cannot write " + path.string());
}

std::string manifest_text(std::string_view id, std::string_view digest) {
    std::string text;
    text += "oabrain: 1\n";
    text += "id: ";
    text += id;
    text += "\n";
    text += R"(name: "Steady Commander"
version: "1"
author:
  name: "A. Maker"
description: "Holds the line."
tags: ["classic"]
works-with: ["ridge"]
levels:
  - id: steady
    name: "Steady"
    difficulty: easy
  - id: hard
    name: "Hard"
    difficulty: hard
fullAwareness: true
networks:
  - id: scout
    file: "nets/scout.oann"
    sha256: ")";
    text += digest;
    text += "\"\n";
    text += R"(options:
  hold:
    type: "toggle"
    label: "Hold"
    default: true
  waves:
    type: "number"
    label: "Waves"
    min: 1
    max: 8
    step: 1
    default: 3
  stance:
    type: "choice"
    label: "Stance"
    values: ["hold", "push"]
    default: "hold"
  pace:
    type: "choice"
    label: "Pace"
    values: [1, 2]
    default: 2
)";
    return text;
}

const std::string_view network_text = "scout";

std::string network_digest() {
    const auto bytes = std::vector<uint8_t>(network_text.begin(), network_text.end());
    const auto hex = sha::to_hex(sha::digest_of(bytes));
    return std::string(hex.begin(), hex.end());
}

void plant(const fs::path& folder, std::string_view written_id) {
    write_file(folder / "brain.lua", "return {}\n");
    write_file(folder / "lua" / "a" / "b.lua", "return {}\n");
    write_file(folder / "nets" / "scout.oann", network_text);
    write_file(folder / "data" / "plans.csv", "plan\n");
    write_file(folder / ".oa-origin.yaml", "origin: kept\n");
    write_file(folder / "oabrain.yaml", manifest_text(written_id, network_digest()));
}

const seats::ResolvedOption* option_named(const seats::ResolvedSeat& seat, std::string_view id) {
    for (const seats::ResolvedOption& option : seat.options) {
        if (option.id == id)
            return &option;
    }
    return nullptr;
}

void reads_every_file_but_the_origin() {
    Scratch scratch;
    const fs::path folder = scratch.path / "steady-commander";
    plant(folder, "steady-commander");
    std::string error;
    const auto installed = seats::read_installed_brain(scratch.path, "steady-commander", error);
    OA_CHECK(installed.has_value());
    if (!installed)
        return;
    OA_CHECK(error.empty());
    OA_CHECK(installed->folder == folder);
    OA_CHECK(installed->manifest.id == "steady-commander");
    OA_CHECK(installed->manifest.full_awareness);
    const std::vector<std::string> expected = {
        "brain.lua", "data/plans.csv", "lua/a/b.lua", "nets/scout.oann", "oabrain.yaml"
    };
    OA_CHECK(installed->files.size() == expected.size());
    for (std::size_t index = 0; index < expected.size() && index < installed->files.size(); ++index)
        OA_CHECK(installed->files[index].first == expected[index]);
    bool saw_origin = false;
    for (const auto& held : installed->files)
        saw_origin = saw_origin || held.first == ".oa-origin.yaml";
    OA_CHECK(!saw_origin);
    OA_CHECK(installed->files[3].second.size() == network_text.size());
}

void refuses_a_missing_folder() {
    Scratch scratch;
    std::string error;
    const auto installed = seats::read_installed_brain(scratch.path, "steady-commander", error);
    OA_CHECK(!installed.has_value());
    OA_CHECK(error == "the package folder is missing");
}

void refuses_an_id_that_is_not_a_folder_name() {
    Scratch scratch;
    std::string error;
    const auto installed = seats::read_installed_brain(scratch.path, "steady/commander", error);
    OA_CHECK(!installed.has_value());
    OA_CHECK(error == "the brain id is not a folder name");
}

void refuses_a_manifest_id_that_differs() {
    Scratch scratch;
    plant(scratch.path / "other-commander", "steady-commander");
    std::string error;
    const auto installed = seats::read_installed_brain(scratch.path, "other-commander", error);
    OA_CHECK(!installed.has_value());
    OA_CHECK(error == "the manifest id does not match the folder");
}

void refuses_a_link() {
    Scratch scratch;
    const fs::path folder = scratch.path / "steady-commander";
    plant(folder, "steady-commander");
    std::error_code link_error;
    fs::create_symlink(folder / "brain.lua", folder / "linked.lua", link_error);
    std::error_code status_error;
    if (link_error || !fs::is_symlink(fs::symlink_status(folder / "linked.lua", status_error))) {
        std::fprintf(stderr, "brain seats: skipped the symbolic link case\n");
        return;
    }
    std::string error;
    const auto installed = seats::read_installed_brain(scratch.path, "steady-commander", error);
    OA_CHECK(!installed.has_value());
    OA_CHECK(error == "the package holds a link");
}

void refuses_a_file_outside_the_layout() {
    Scratch scratch;
    const fs::path folder = scratch.path / "steady-commander";
    plant(folder, "steady-commander");
    write_file(folder / "notes.txt", "no\n");
    std::string error;
    const auto installed = seats::read_installed_brain(scratch.path, "steady-commander", error);
    OA_CHECK(!installed.has_value());
    OA_CHECK(error == "notes.txt is not a file a brain may hold");
}

void refuses_a_folder_over_the_size_guard() {
    Scratch scratch;
    const fs::path folder = scratch.path / "steady-commander";
    plant(folder, "steady-commander");
    fs::resize_file(folder / "data" / "plans.csv", (uintmax_t{128} << 20) + 1);
    std::string error;
    const auto installed = seats::read_installed_brain(scratch.path, "steady-commander", error);
    OA_CHECK(!installed.has_value());
    OA_CHECK(error == "the package is larger than 128 MiB");
}

void resolves_the_first_level_and_the_defaults() {
    Scratch scratch;
    plant(scratch.path / "steady-commander", "steady-commander");
    std::string error;
    const auto installed = seats::read_installed_brain(scratch.path, "steady-commander", error);
    OA_CHECK(installed.has_value());
    if (!installed)
        return;

    seats::BrainSeatChoice choice;
    choice.id = "steady-commander";
    seats::ResolvedSeat resolved;
    OA_CHECK(seats::resolve_seat(*installed, choice, resolved, error));
    OA_CHECK(resolved.level == "steady");
    OA_CHECK(resolved.difficulty == OA_DIFFICULTY_EASY);
    OA_CHECK(resolved.version == "1");
    OA_CHECK(resolved.full_awareness);
    OA_CHECK(resolved.options.size() == 4);
    const seats::ResolvedOption* hold = option_named(resolved, "hold");
    const seats::ResolvedOption* waves = option_named(resolved, "waves");
    const seats::ResolvedOption* stance = option_named(resolved, "stance");
    const seats::ResolvedOption* pace = option_named(resolved, "pace");
    OA_CHECK(hold != nullptr && hold->kind == brains::OptionKind::toggle && hold->toggle);
    OA_CHECK(waves != nullptr && waves->kind == brains::OptionKind::number && waves->number == 3.0);
    OA_CHECK(stance != nullptr && stance->text == "hold");
    OA_CHECK(
        pace != nullptr && pace->kind == brains::OptionKind::choice && pace->text.empty() &&
        pace->number == 2.0
    );
    OA_CHECK(resolved.options[0].id == "hold");
    OA_CHECK(resolved.options[3].id == "pace");
    OA_CHECK(resolved.networks.size() == 1);
    OA_CHECK(resolved.networks[0].id == "scout");
    OA_CHECK(resolved.networks[0].path == "nets/scout.oann");
    OA_CHECK(resolved.networks[0].sha256 == network_digest());

    seats::BrainSeatChoice named;
    named.id = "steady-commander";
    named.level = "hard";
    named.options.push_back({"stance", "push"});
    named.options.push_back({"waves", "4"});
    named.options.push_back({"waves", "6"});
    seats::ResolvedSeat picked;
    OA_CHECK(seats::resolve_seat(*installed, named, picked, error));
    OA_CHECK(picked.level == "hard");
    OA_CHECK(picked.difficulty == OA_DIFFICULTY_HARD);
    const seats::ResolvedOption* picked_waves = option_named(picked, "waves");
    const seats::ResolvedOption* picked_stance = option_named(picked, "stance");
    OA_CHECK(picked_waves != nullptr && picked_waves->number == 6.0);
    OA_CHECK(picked_stance != nullptr && picked_stance->text == "push");
    OA_CHECK(picked.options[0].id == "hold");
    OA_CHECK(picked.options[2].id == "stance");
}

void refuses_an_unknown_level_option_or_value() {
    Scratch scratch;
    plant(scratch.path / "steady-commander", "steady-commander");
    std::string error;
    const auto installed = seats::read_installed_brain(scratch.path, "steady-commander", error);
    OA_CHECK(installed.has_value());
    if (!installed)
        return;
    seats::ResolvedSeat resolved;
    resolved.level = "kept";

    seats::BrainSeatChoice level;
    level.level = "missing";
    OA_CHECK(!seats::resolve_seat(*installed, level, resolved, error));
    OA_CHECK(error == "unknown level missing");
    OA_CHECK(resolved.level == "kept");

    seats::BrainSeatChoice option;
    option.options.push_back({"nope", "1"});
    OA_CHECK(!seats::resolve_seat(*installed, option, resolved, error));
    OA_CHECK(error == "unknown option nope");

    seats::BrainSeatChoice value;
    value.options.push_back({"hold", "yes"});
    OA_CHECK(!seats::resolve_seat(*installed, value, resolved, error));
    OA_CHECK(error == "hold: yes is not true or false");
}

void seat_spec_views_match() {
    Scratch scratch;
    plant(scratch.path / "steady-commander", "steady-commander");
    std::string error;
    const auto installed = seats::read_installed_brain(scratch.path, "steady-commander", error);
    OA_CHECK(installed.has_value());
    if (!installed)
        return;
    seats::BrainSeatChoice choice;
    choice.id = "steady-commander";
    choice.level = "hard";
    seats::ResolvedSeat resolved;
    OA_CHECK(seats::resolve_seat(*installed, choice, resolved, error));

    std::vector<brains::PackageFile> files;
    std::vector<brains::OptionValue> options;
    std::vector<brains::NetworkFile> networks;
    const brains::SeatSpec spec =
        seats::seat_spec(1, *installed, resolved, files, options, networks);
    OA_CHECK(spec.player == 1);
    OA_CHECK(spec.id == "steady-commander");
    OA_CHECK(spec.name == "Steady Commander");
    OA_CHECK(spec.version == "1");
    OA_CHECK(spec.entry == "brain.lua");
    OA_CHECK(spec.level == "hard");
    OA_CHECK(spec.difficulty == OA_DIFFICULTY_HARD);
    OA_CHECK(spec.full_awareness);
    OA_CHECK(spec.bank.per_tick == host::default_tick_allowance);
    OA_CHECK(spec.bank.ticks == host::default_bank_ticks);
    OA_CHECK(spec.bank.call_time_limit == host::default_call_time_limit);
    OA_CHECK(spec.files.size() == 5);
    OA_CHECK(spec.files[0].path == "brain.lua");
    OA_CHECK(spec.options.size() == 4);
    OA_CHECK(spec.options[3].id == "pace");
    OA_CHECK(spec.options[3].text.empty());
    OA_CHECK(spec.options[3].number == 2.0);
    OA_CHECK(spec.networks.size() == 1);
    OA_CHECK(spec.networks[0].path == "nets/scout.oann");
    const auto bytes = std::vector<uint8_t>(network_text.begin(), network_text.end());
    OA_CHECK(spec.networks[0].sha256 == sha::digest_of(bytes));
    OA_CHECK(spec.version.data() == resolved.version.data());
    OA_CHECK(spec.files.data() == files.data());
}

std::string reason_in(std::string_view notice) {
    const std::size_t open = notice.rfind('(');
    const std::size_t close = notice.find("); the engine", open);
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open)
        return {};
    return std::string(notice.substr(open + 1, close - open - 1));
}

void notices_cut_a_reason_on_a_character() {
    const std::string dropped_reason(159, 'a');
    const std::string over = dropped_reason + "\xC3\xA9";
    const std::string exact(158, 'a');
    const std::string kept_reason = exact + "\xC3\xA9";
    const std::string_view words[] = {
        seats::brain_could_not_start_words,
        seats::brain_does_not_fit_words,
        seats::brain_stopped_words,
    };
    for (const std::string_view word : words) {
        const std::string dropped = seats::brain_notice(word, "Steady", over);
        const std::string cut = reason_in(dropped);
        OA_CHECK(cut == dropped_reason);
        OA_CHECK(cut.find('\xC3') == std::string::npos);
        OA_CHECK(dropped.find("Steady") != std::string::npos);
        const std::string kept = seats::brain_notice(word, "Steady", kept_reason);
        OA_CHECK(reason_in(kept) == kept_reason);
        OA_CHECK(kept.find("\xC3\xA9") != std::string::npos);
    }
}

void log_writes_the_header_and_stops_at_one_mebibyte() {
    const fs::path root = oa::test::make_scratch_directory("B04-brain-log");
    fs::path path;
    {
        seats::BrainLog log;
        log.begin(root, 2, "Steady Commander", "1", "steady-commander", "steady", 42);
        OA_CHECK(log.open());
        path = log.path();
        OA_CHECK(path.filename() == "seat-3.log");
        OA_CHECK(path.parent_path().filename() == "brains");
        OA_CHECK(path.parent_path().parent_path().filename() == "logs");
        log.line(0, "from-start");
        host::ScriptError error;
        error.kind = host::ErrorKind::runtime;
        error.package_id = "steady-commander";
        error.phase = "tick";
        error.chunk = "brain.lua";
        error.line = 4;
        error.message = "attempt to index a nil value";
        error.traceback = "stack traceback:\n\tbrain.lua:4: in main chunk";
        log.stop(7, error);
        const std::string wide(400, 'w');
        while (log.open())
            log.line(9, wide);
        OA_CHECK(!log.open());
    }
    std::ifstream in(path, std::ios::binary);
    OA_CHECK(static_cast<bool>(in));
    const std::string file((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    constexpr std::size_t mebibyte = std::size_t{1} << 20;
    OA_CHECK(file.size() <= mebibyte);
    OA_CHECK(file.starts_with(
        "Steady Commander 1 (steady-commander), level steady, seat 3, match seed 42\n"
    ));
    OA_CHECK(file.find("tick 0: from-start\n") != std::string::npos);
    OA_CHECK(
        file.find(
            "tick 7: stopped: steady-commander: tick: brain.lua:4: attempt to index a nil value\n"
        ) != std::string::npos
    );
    OA_CHECK(file.find("stack traceback:\n") != std::string::npos);
    OA_CHECK(file.find("\tbrain.lua:4: in main chunk\n") != std::string::npos);
    const std::string ending = "the log is full\n";
    OA_CHECK(file.size() >= ending.size());
    OA_CHECK(file.compare(file.size() - ending.size(), ending.size(), ending) == 0);
    std::error_code ignored;
    fs::remove_all(root, ignored);
}

} // namespace

int main() {
    try {
        reads_every_file_but_the_origin();
        refuses_a_missing_folder();
        refuses_an_id_that_is_not_a_folder_name();
        refuses_a_manifest_id_that_differs();
        refuses_a_link();
        refuses_a_file_outside_the_layout();
        refuses_a_folder_over_the_size_guard();
        resolves_the_first_level_and_the_defaults();
        refuses_an_unknown_level_option_or_value();
        seat_spec_views_match();
        notices_cut_a_reason_on_a_character();
        log_writes_the_header_and_stops_at_one_mebibyte();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "app-brain-seats: %s\n", ex.what());
        return 1;
    }
    return oa::test::check_exit_status();
}
