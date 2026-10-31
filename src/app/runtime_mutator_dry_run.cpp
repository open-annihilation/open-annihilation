// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-mutator-dry-run: two made-up mutators stack commander turn rate
// and worker time past 65535, the end of the unsigned 16 bits the game reads
// them in. The verdict stays compatible and each field is held, with one
// note.
#include "oa/app/runtime.hpp"

#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/pipeline.hpp"
#include "oa/platform/files.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {
namespace {

namespace mutators = oa::data::mutators;
namespace fs = std::filesystem;

[[noreturn]] void fail(const std::string& what) {
    throw std::runtime_error("mutator dry run check: " + what);
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

void write_package(const fs::path& folder, std::string_view id) {
    std::string yaml = "oamut: 1\nid: ";
    yaml.append(id);
    yaml += "\nname: \"";
    yaml.append(id);
    yaml += "\"\nversion: \"1.0\"\nauthor:\n  name: \"Ridge Maker\"\n"
            "tags: [\"gameplay\"]\nworks-with: [\"*\"]\nlayers: [\"units\"]\n";
    write_text(folder / "oamut.yaml", yaml);
    write_text(
        folder / "mutator.lua",
        "function mutate(ctx)\n"
        "  return {\n"
        "    units = function(units, ctx)\n"
        "      units:where { commander = true }:scale { turnrate = 200, workertime = 200 }\n"
        "    end\n"
        "  }\n"
        "end\n"
    );
}

mutators::ResolvedMutator resolve_package(const fs::path& folder) {
    std::string error;
    const std::optional<mutators::MutatorChoice> choice =
        mutators::parse_choice(oa::platform::path_to_utf8(folder), &error);
    if (!choice)
        fail(error.empty() ? "could not read a package choice" : error);
    const std::optional<mutators::ResolvedMutator> resolved =
        mutators::resolve_choice(*choice, std::filesystem::path{}, &error);
    if (!resolved)
        fail(error.empty() ? "could not read a package" : error);
    return *resolved;
}

std::string layer_text(const oa::MutatorLayerSpec& spec, std::string_view path) {
    for (const oa::MutatorLayerEntry& entry : spec.entries) {
        if (entry.contents == nullptr)
            continue;
        if (entry.path != path)
            continue;
        return std::string(entry.contents->begin(), entry.contents->end());
    }
    return {};
}

} // namespace

void Runtime::check_mutator_dry_run() {
    if (!options_.preferences_file.has_value())
        fail("the check needs a preferences file");
    const fs::path beside = preference_path_.parent_path();
    const fs::path wide = beside / "ridge-hold-wide";
    const fs::path again = beside / "ridge-hold-again";
    write_package(wide, "ridge-hold-wide");
    write_package(again, "ridge-hold-again");
    const mutators::ResolvedMutator first = resolve_package(wide);
    const mutators::ResolvedMutator second = resolve_package(again);
    const mutators::ResolvedMutator both[] = {first, second};
    const mutators::DryRunJob job = prepare_mutator_check(both);

    oa::AssetStore store(job.loose_roots);
    for (const fs::path& archive : job.mounts) {
        std::string error;
        if (!store.try_mount(archive, &error))
            fail(error.empty() ? "a mount failed" : error);
    }
    const mutators::PipelineResult result = mutators::run_pipeline(store, job.input);
    if (store.mutator_layer_mounted())
        store.unmount_mutator_layer();
    if (!result.refusal.empty())
        fail(result.refusal);
    const mutators::VerdictRecord verdict =
        mutators::verdict_of(result, job.mod_label, job.input.mutators);
    if (verdict.verdict != mutators::Verdict::compatible)
        fail(std::string(mutators::verdict_word(verdict.verdict)) + " " + verdict.reason);
    if (!verdict.reason.empty())
        fail("a compatible verdict carried a reason");

    int turn_notes = 0;
    int worker_notes = 0;
    // Each held value is noted once, however many mutators stacked it.
    std::set<std::string> noted;
    std::vector<std::string> ids;
    ids.reserve(job.input.mutators.size());
    for (const mutators::PipelineMutator& mutator : job.input.mutators)
        ids.push_back(mutator.manifest.id);
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        if (conflict.kind != mutators::ConflictKind::value_held)
            continue;
        const std::string line = mutators::describe(conflict, ids);
        std::cout << "mutator dry run check: " << line << '\n';
        if (!noted.insert(conflict.path + '\n' + conflict.record + '\n' + conflict.field).second)
            fail("noted twice: " + line);
        const std::string expected_turn = conflict.record + ": turnrate ";
        const std::string expected_worker = conflict.record + ": workertime ";
        if (conflict.field == "turnrate") {
            if (line.find(expected_turn) != 0 || line.find(" held at 65535") == std::string::npos)
                fail(line);
            const std::string text = layer_text(result.layer, conflict.path);
            if (text.find("65535") == std::string::npos)
                fail(conflict.path + " did not hold turnrate");
            ++turn_notes;
        }
        if (conflict.field == "workertime") {
            if (line.find(expected_worker) != 0 || line.find(" held at 65535") == std::string::npos)
                fail(line);
            const std::string text = layer_text(result.layer, conflict.path);
            if (text.find("65535") == std::string::npos)
                fail(conflict.path + " did not hold workertime");
            ++worker_notes;
        }
    }
    for (const std::string_view commander : {"ARMCOM", "CORCOM"}) {
        for (const std::string_view field : {"turnrate", "workertime"}) {
            bool found = false;
            for (const std::string& key : noted) {
                const std::string::size_type path_end = key.find('\n');
                const std::string::size_type record_end = key.find('\n', path_end + 1);
                if (key.compare(path_end + 1, record_end - path_end - 1, commander) == 0 &&
                    key.compare(record_end + 1, std::string::npos, field) == 0)
                    found = true;
            }
            if (!found)
                fail(std::string(commander) + " " + std::string(field) + " was not held");
        }
    }
    if (turn_notes < 1 || worker_notes < 1)
        fail(
            "held " + std::to_string(turn_notes) + " turn rates and " +
            std::to_string(worker_notes) + " worker times"
        );
    std::cout << "mutator dry run check: passed\n";
}

} // namespace oa::app
