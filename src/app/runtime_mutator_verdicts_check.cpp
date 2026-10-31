// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-mutator-verdicts: four made-up mutators against the installed
// game, remembered in the player's Cache folder, then read back from it.
#include "oa/app/runtime.hpp"

#include "oa/app/user_folder.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/verdicts.hpp"
#include "oa/platform/files.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace oa::app {
namespace {

namespace mutators = oa::data::mutators;
namespace fs = std::filesystem;

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("mutator verdicts check: " + what);
}

void write_text(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out)
        fail("could not write " + oa::platform::path_to_utf8(path));
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    if (!out)
        fail("could not write " + oa::platform::path_to_utf8(path));
}

void write_package(
    const fs::path& folder,
    std::string_view id,
    std::string_view name,
    std::string_view layers,
    std::string_view options,
    std::string_view lua
) {
    std::string yaml = "oamut: 1\nid: ";
    yaml.append(id);
    yaml += "\nname: \"";
    yaml.append(name);
    yaml += "\"\nversion: \"1.0\"\nauthor:\n  name: \"Ridge Maker\"\n"
            "tags: [\"gameplay\"]\nworks-with: [\"*\"]\n";
    yaml.append(options);
    yaml += "layers: [";
    yaml.append(layers);
    yaml += "]\n";
    write_text(folder / "oamut.yaml", yaml);
    write_text(folder / "mutator.lua", lua);
}

mutators::ResolvedMutator resolve_package(const fs::path& folder, std::string_view options) {
    std::string text = oa::platform::path_to_utf8(folder);
    if (!options.empty()) {
        text.push_back(':');
        text.append(options);
    }
    std::string error;
    const std::optional<mutators::MutatorChoice> choice = mutators::parse_choice(text, &error);
    if (!choice)
        fail(error.empty() ? "could not read a package choice" : error);
    const std::optional<mutators::ResolvedMutator> resolved =
        mutators::resolve_choice(*choice, std::filesystem::path{}, &error);
    if (!resolved)
        fail(error.empty() ? "could not read a package" : error);
    return *resolved;
}

std::vector<mutators::RememberedVerdict> read_file(const fs::path& path) {
    std::FILE* file = oa::platform::open_file(path, "rb");
    if (file == nullptr)
        return {};
    std::vector<uint8_t> bytes(mutators::verdicts_file_most_bytes + 1);
    const std::size_t got = std::fread(bytes.data(), 1, bytes.size(), file);
    const bool failed = std::ferror(file) != 0;
    std::fclose(file);
    if (failed)
        fail("could not read the verdict file");
    return mutators::read_verdicts(std::span<const uint8_t>(bytes.data(), got));
}

void expect_verdict(
    const mutators::VerdictRecord& record, mutators::Verdict verdict, std::string_view why
) {
    if (record.verdict != verdict) {
        fail(
            std::string(why) + " was " + std::string(mutators::verdict_word(record.verdict)) +
            " (" + record.reason + ")"
        );
    }
}

void expect_commander_counts(const mutators::ChangeCounts& counts) {
    if (counts.units_changed < 2)
        fail("changed " + std::to_string(counts.units_changed) + " units");
    if (counts.units_added != 0 || counts.units_removed != 0 || counts.weapons_changed != 0 ||
        counts.weapons_added != 0 || counts.weapons_removed != 0 || counts.features_changed != 0 ||
        counts.features_added != 0 || counts.features_removed != 0 ||
        counts.menu_entries_changed != 0 || counts.files_added != 0 || counts.files_hidden != 0) {
        fail("a commander check changed more than unit fields");
    }
}

} // namespace

void Runtime::check_mutator_verdicts() {
    if (!options_.preferences_file.has_value())
        fail("the check needs a preferences file");
    const fs::path beside = preference_path_.parent_path();
    const fs::path health_dir = beside / "ridge-verdict-health";
    const fs::path quiet_dir = beside / "ridge-verdict-quiet";
    const fs::path stop_dir = beside / "ridge-verdict-stop";
    const fs::path bad_dir = beside / "ridge-verdict-bad";
    constexpr std::string_view health_options =
        "options:\n  health:\n    type: number\n    label: \"Health\"\n"
        "    min: 0\n    max: 30000\n    step: 1000\n    default: 4000\n";
    write_package(
        health_dir,
        "ridge-verdict-health",
        "Ridge Verdict Health",
        "\"units\"",
        health_options,
        "function mutate(ctx)\n"
        "  return {\n"
        "    units = function(units, ctx)\n"
        "      units:where { commander = true }:set { maxdamage = ctx.options.health }\n"
        "    end\n"
        "  }\n"
        "end\n"
    );
    write_package(
        quiet_dir,
        "ridge-verdict-quiet",
        "Ridge Verdict Quiet",
        "\"units\"",
        "",
        "function mutate(ctx)\n"
        "  return {\n"
        "    rules = {\n"
        "      where { unitname = is(\"NOUNIT\") }:change { maxdamage = 1 }\n"
        "    }\n"
        "  }\n"
        "end\n"
    );
    write_package(
        stop_dir,
        "ridge-verdict-stop",
        "Ridge Verdict Stop",
        "\"units\"",
        "",
        "function mutate(ctx)\n"
        "  ctx.incompatible(\"needs a second side\")\n"
        "end\n"
    );
    write_package(
        bad_dir,
        "ridge-verdict-bad",
        "Ridge Verdict Bad",
        "\"units\"",
        "",
        "function mutate(ctx)\n"
        "  return {\n"
        "end\n"
    );

    const mutators::ResolvedMutator health = resolve_package(health_dir, "");
    const mutators::ResolvedMutator quiet = resolve_package(quiet_dir, "");
    const mutators::ResolvedMutator stop = resolve_package(stop_dir, "");
    const mutators::ResolvedMutator bad = resolve_package(bad_dir, "");
    const auto one = [](const mutators::ResolvedMutator& mutator) {
        return std::span<const mutators::ResolvedMutator>(&mutator, 1);
    };

    const uint32_t began = mutator_dry_runs_;
    const mutators::VerdictRecord health_record = check_mutators(one(health));
    expect_verdict(health_record, mutators::Verdict::compatible, "commanders");
    expect_commander_counts(health_record.counts);
    if (!health_record.reason.empty())
        fail("a compatible verdict carried a reason");

    const mutators::VerdictRecord quiet_record = check_mutators(one(quiet));
    expect_verdict(quiet_record, mutators::Verdict::no_effect, "rules that match nothing");
    if (!quiet_record.reason.empty() || quiet_record.counts != mutators::ChangeCounts{})
        fail("a no-effect verdict carried a reason or a count");

    const mutators::VerdictRecord stop_record = check_mutators(one(stop));
    expect_verdict(stop_record, mutators::Verdict::incompatible, "a declared stop");
    if (stop_record.reason != "needs a second side")
        fail("the declared reason was " + stop_record.reason);

    const mutators::VerdictRecord bad_record = check_mutators(one(bad));
    expect_verdict(bad_record, mutators::Verdict::failed, "a script that does not load");
    if (bad_record.reason.empty())
        fail("a failed verdict had no reason");

    const fs::path cache =
        user_folder_ / std::string(cache_folder_name) / std::string(mutators::verdicts_file_name);
    if (mutator_dry_runs_ != began + 4)
        fail("the first four checks ran " + std::to_string(mutator_dry_runs_ - began) + " times");
    if (read_file(cache).size() != 4)
        fail("the verdict file did not hold four lines");

    if (check_mutators(one(health)) != health_record ||
        check_mutators(one(quiet)) != quiet_record || check_mutators(one(stop)) != stop_record ||
        check_mutators(one(bad)) != bad_record) {
        fail("asking again did not answer from the file");
    }
    if (mutator_dry_runs_ != began + 4)
        fail("asking again ran a dry run");

    const mutators::ResolvedMutator changed = resolve_package(health_dir, "health=5000");
    const mutators::VerdictRecord again = check_mutators(one(changed));
    expect_verdict(again, mutators::Verdict::compatible, "a changed option");
    if (mutator_dry_runs_ != began + 5)
        fail("a changed option did not run again");
    if (read_file(cache).size() != 5)
        fail("a changed option did not add a line");

    write_text(cache, "this is not a verdict file\n");
    if (!read_file(cache).empty())
        fail("garbage was read as a verdict");
    check_mutators(one(health));
    check_mutators(one(quiet));
    check_mutators(one(stop));
    check_mutators(one(bad));
    if (mutator_dry_runs_ != began + 9)
        fail("garbage was not checked again");
    if (read_file(cache).size() != 4)
        fail("garbage was not rewritten as four verdicts");

    const fs::path stray = user_folder_ /
                           std::string(oa::data::mutator_library::mutators_folder_name) /
                           std::string(mutators::verdicts_file_name);
    std::error_code exists_error;
    if (fs::exists(stray, exists_error))
        fail("the Mutators folder holds a verdicts file");

    std::cout << "mutator verdicts check: passed\n";
}

} // namespace oa::app
