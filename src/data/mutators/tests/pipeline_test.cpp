// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mutation pipeline over the made-up Ridge packages. Identical inputs
// are compared whole, and so is a run with a failing mutator against the
// run of the mutators that remain. Two rule-only mutators are compared by
// the bytes the layer serves: the owner of a written file and the layer's
// label follow the list, so those two fields are not the served bytes.
#include "oa/data/defs/asset_files.hpp"
#include "oa/data/defs/unit_def_loader.hpp"
#include "oa/data/defs/weapons.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/data/mod_profile/overrides.hpp"
#include "oa/data/mutators/pipeline.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef OA_MUTATORS_TEST_DATA
#error OA_MUTATORS_TEST_DATA is the fixture folder
#endif

namespace {

namespace mutators = oa::data::mutators;
namespace oamut = oa::formats::oamut;
namespace defs = oa::data::defs;
namespace fs = std::filesystem;

const fs::path k_data{OA_MUTATORS_TEST_DATA};

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    OA_CHECK(in.good());
    if (!in.good())
        return {};
    in.seekg(0, std::ios::end);
    const std::streamoff length = in.tellg();
    in.seekg(0);
    if (length <= 0)
        return {};
    std::vector<uint8_t> bytes(static_cast<std::size_t>(length));
    in.read(reinterpret_cast<char*>(bytes.data()), length);
    return bytes;
}

void write_bytes(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    OA_CHECK(out.good());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

mutators::PipelineMutator load_mutator(const fs::path& package) {
    mutators::PipelineMutator mutator;
    mutator.package = package;
    const std::vector<uint8_t> bytes = read_bytes(package / "oamut.yaml");
    std::vector<oamut::Problem> problems;
    const bool read = oamut::read_manifest(bytes, mutator.manifest, problems);
    if (!read) {
        for (const oamut::Problem& problem : problems) {
            std::fprintf(
                stderr,
                "%s: %s: %s\n",
                package.string().c_str(),
                problem.key.c_str(),
                problem.message.c_str()
            );
        }
    }
    OA_CHECK(read);
    return mutator;
}

mutators::PipelineMutator load_id(std::string_view id) {
    return load_mutator(k_data / "packages" / std::string{id});
}

mutators::PipelineInput make_input(std::vector<mutators::PipelineMutator> list) {
    mutators::PipelineInput input;
    input.mutators = std::move(list);
    input.mod.id = "ridge";
    input.mod.version = "1.0";
    input.mod.game = "ridge";
    input.engine_version = "0.8.5";
    input.layout = &defs::data_layout();
    input.seed = 1;
    return input;
}

mutators::PipelineInput input_of(std::vector<std::string_view> ids) {
    std::vector<mutators::PipelineMutator> list;
    for (const std::string_view id : ids)
        list.push_back(load_id(id));
    return make_input(std::move(list));
}

oa::AssetStore open_ridge() {
    return oa::AssetStore(k_data / "ridge");
}

void release(oa::AssetStore& store) {
    if (store.mutator_layer_mounted())
        store.unmount_mutator_layer();
    OA_CHECK(!store.mutator_layer_mounted());
}

std::string text_of(const std::vector<uint8_t>& bytes) {
    return std::string(bytes.begin(), bytes.end());
}

std::string served_text(const oa::MutatorLayerEntry& entry) {
    if (entry.contents == nullptr)
        return {};
    return text_of(*entry.contents);
}

bool same_contents(
    const std::shared_ptr<const std::vector<uint8_t>>& left,
    const std::shared_ptr<const std::vector<uint8_t>>& right
) {
    if (left == nullptr || right == nullptr)
        return left == nullptr && right == nullptr;
    return *left == *right;
}

/// Served paths, kinds, payloads and mask patterns. Owners and the label
/// follow list order, so a reversed rule list is not the same spec.
bool payloads_match(const oa::MutatorLayerSpec& left, const oa::MutatorLayerSpec& right) {
    if (left.entries.size() != right.entries.size() || left.masks.size() != right.masks.size() ||
        left.archives.size() != right.archives.size())
        return false;
    for (std::size_t index = 0; index < left.masks.size(); ++index) {
        if (left.masks[index].pattern != right.masks[index].pattern)
            return false;
    }
    for (std::size_t index = 0; index < left.entries.size(); ++index) {
        const oa::MutatorLayerEntry& one = left.entries[index];
        const oa::MutatorLayerEntry& other = right.entries[index];
        if (one.path != other.path || one.kind != other.kind ||
            !same_contents(one.contents, other.contents))
            return false;
    }
    return true;
}

bool specs_match(const oa::MutatorLayerSpec& left, const oa::MutatorLayerSpec& right) {
    if (left.label != right.label || !payloads_match(left, right))
        return false;
    for (std::size_t index = 0; index < left.masks.size(); ++index) {
        if (left.masks[index].owner != right.masks[index].owner)
            return false;
    }
    for (std::size_t index = 0; index < left.entries.size(); ++index) {
        const oa::MutatorLayerEntry& one = left.entries[index];
        const oa::MutatorLayerEntry& other = right.entries[index];
        if (one.owner != other.owner || one.host != other.host || one.archive != other.archive ||
            one.archive_path != other.archive_path)
            return false;
    }
    for (std::size_t index = 0; index < left.archives.size(); ++index) {
        if (left.archives[index].owner != right.archives[index].owner ||
            left.archives[index].location != right.archives[index].location)
            return false;
    }
    return true;
}

/// The ids that `slots` name in `run`'s own list. Journal entries and
/// conflicts name mutators by their index in the list a run was given, so
/// two runs over different lists are compared by the ids those indices name.
std::vector<std::string>
ids_of(const std::vector<uint16_t>& slots, const mutators::PipelineResult& run) {
    std::vector<std::string> ids;
    for (const uint16_t slot : slots) {
        OA_CHECK(slot < run.mutators.size());
        if (slot < run.mutators.size())
            ids.push_back(run.mutators[slot].id);
    }
    return ids;
}

bool same_entry(
    const mutators::JournalEntry& left,
    const mutators::PipelineResult& left_run,
    const mutators::JournalEntry& right,
    const mutators::PipelineResult& right_run
) {
    return ids_of(left.mutators, left_run) == ids_of(right.mutators, right_run) &&
           left.phase == right.phase && left.collection == right.collection &&
           left.change == right.change && left.record == right.record &&
           left.block == right.block && left.field == right.field &&
           left.old_text == right.old_text && left.new_text == right.new_text &&
           left.path == right.path;
}

/// Compares two runs' journals entry by entry, naming mutators by id.
bool journals_match(const mutators::PipelineResult& left, const mutators::PipelineResult& right) {
    if (left.journal.entries.size() != right.journal.entries.size())
        return false;
    for (std::size_t index = 0; index < left.journal.entries.size(); ++index) {
        if (!same_entry(left.journal.entries[index], left, right.journal.entries[index], right))
            return false;
    }
    return true;
}

bool conflicts_match(
    const mutators::Conflict& left,
    const mutators::PipelineResult& left_run,
    const mutators::Conflict& right,
    const mutators::PipelineResult& right_run
) {
    return left.severity == right.severity && left.kind == right.kind &&
           ids_of(left.mutators, left_run) == ids_of(right.mutators, right_run) &&
           left.record == right.record && left.field == right.field && left.path == right.path &&
           left.detail == right.detail;
}

/// Compares two runs' conflict reports, naming mutators by id.
bool reports_match(const mutators::PipelineResult& left, const mutators::PipelineResult& right) {
    if (left.report.unread != right.report.unread ||
        left.report.conflicts.size() != right.report.conflicts.size())
        return false;
    for (std::size_t index = 0; index < left.report.conflicts.size(); ++index) {
        if (!conflicts_match(
                left.report.conflicts[index], left, right.report.conflicts[index], right
            ))
            return false;
    }
    return true;
}

bool lists_name(
    const oa::AssetStore& store,
    std::string_view directory,
    std::string_view extension,
    std::string_view needle
) {
    const std::vector<std::string> names =
        store.list_effective_in_mount_order(directory, extension);
    for (const std::string& name : names) {
        if (name.find(needle) != std::string::npos)
            return true;
    }
    return false;
}

/// Checks that a run with failing mutators is the run of the ones that
/// remain: the same layer, journal and report, with mutators named by id.
void check_same_run(
    const mutators::PipelineResult& with_failures, const mutators::PipelineResult& survivors
) {
    OA_CHECK(with_failures.refusal.empty());
    OA_CHECK(survivors.playable());
    OA_CHECK(specs_match(with_failures.layer, survivors.layer));
    OA_CHECK(journals_match(with_failures, survivors));
    OA_CHECK(reports_match(with_failures, survivors));
}

/// Checks that no journal entry or conflict names `slot`.
void check_no_trace(const mutators::PipelineResult& result, uint16_t slot) {
    for (const mutators::JournalEntry& entry : result.journal.entries) {
        for (const uint16_t named : entry.mutators)
            OA_CHECK(named != slot);
    }
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        for (const uint16_t named : conflict.mutators)
            OA_CHECK(named != slot);
    }
}

/// Counts the paths in `paths` that end with `tail`, ignoring ASCII case.
std::size_t count_paths(const std::vector<std::string>& paths, std::string_view tail) {
    std::size_t found = 0;
    for (const std::string& path : paths) {
        if (path.size() < tail.size())
            continue;
        bool same = true;
        for (std::size_t offset = 0; offset < tail.size(); ++offset) {
            const char left = path[path.size() - tail.size() + offset];
            const char lower =
                left >= 'A' && left <= 'Z' ? static_cast<char>(left - 'A' + 'a') : left;
            if (lower != tail[offset])
                same = false;
        }
        if (same)
            ++found;
    }
    return found;
}

const mutators::Conflict*
conflict_of(const mutators::PipelineResult& result, mutators::ConflictKind kind) {
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        if (conflict.kind == kind)
            return &conflict;
    }
    return nullptr;
}

std::size_t count_kind(const mutators::PipelineResult& result, mutators::ConflictKind kind) {
    std::size_t count = 0;
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        if (conflict.kind == kind)
            ++count;
    }
    return count;
}

void test_swift_writes_one_file() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, input_of({"ridge-swift"}));
    OA_CHECK(result.refusal.empty());
    OA_CHECK(result.playable());
    OA_CHECK(
        result.mutators.size() == 1 && result.mutators[0].outcome == mutators::Outcome::applied
    );
    OA_CHECK(result.layer.entries.size() == 1);
    if (result.layer.entries.size() == 1) {
        OA_CHECK(result.layer.entries[0].path.find("rgbot.fbi") != std::string::npos);
        OA_CHECK(result.layer.entries[0].kind == oa::MutatorSourceKind::contents);
        OA_CHECK(served_text(result.layer.entries[0]).find("2.4") != std::string::npos);
    }
    const oa::AssetData bot = store.read("units/RGBOT.fbi");
    OA_CHECK(text_of(bot.bytes).find("2.4") != std::string::npos);
    const oa::AssetData lab = store.read("units/RGLAB.fbi");
    OA_CHECK(text_of(lab.bytes).find("WorkerTime=100") != std::string::npos);
    release(store);
}

void test_rules_are_order_independent() {
    oa::AssetStore forward_store = open_ridge();
    oa::AssetStore backward_store = open_ridge();
    const mutators::PipelineResult forward =
        mutators::run_pipeline(forward_store, input_of({"ridge-swift", "ridge-swift-sm"}));
    const mutators::PipelineResult backward =
        mutators::run_pipeline(backward_store, input_of({"ridge-swift-sm", "ridge-swift"}));
    OA_CHECK(forward.playable() && backward.playable());
    OA_CHECK(payloads_match(forward.layer, backward.layer));
    OA_CHECK(forward.layer.label == "mutators ridge-swift ridge-swift-sm");
    OA_CHECK(backward.layer.label == "mutators ridge-swift-sm ridge-swift");
    if (!forward.layer.entries.empty() && !backward.layer.entries.empty()) {
        OA_CHECK(forward.layer.entries[0].owner == "ridge-swift-sm");
        OA_CHECK(backward.layer.entries[0].owner == "ridge-swift");
        OA_CHECK(served_text(forward.layer.entries[0]).find("3.6") != std::string::npos);
        OA_CHECK(served_text(backward.layer.entries[0]).find("3.6") != std::string::npos);
    } else {
        OA_CHECK(false);
    }
    const oa::AssetData forward_bot = forward_store.read("units/RGBOT.fbi");
    const oa::AssetData backward_bot = backward_store.read("units/RGBOT.fbi");
    OA_CHECK(forward_bot.bytes == backward_bot.bytes);
    OA_CHECK(text_of(forward_bot.bytes).find("3.6") != std::string::npos);
    release(forward_store);
    release(backward_store);
}

void test_hooks_follow_the_list() {
    oa::AssetStore rising = open_ridge();
    oa::AssetStore falling = open_ridge();
    const mutators::PipelineResult tougher_last =
        mutators::run_pipeline(rising, input_of({"ridge-tough", "ridge-tougher"}));
    const mutators::PipelineResult tough_last =
        mutators::run_pipeline(falling, input_of({"ridge-tougher", "ridge-tough"}));
    OA_CHECK(tougher_last.playable() && tough_last.playable());
    const oa::AssetData high = rising.read("units/RGCOM.fbi");
    const oa::AssetData low = falling.read("units/RGCOM.fbi");
    OA_CHECK(text_of(high.bytes).find("5000") != std::string::npos);
    OA_CHECK(text_of(low.bytes).find("4000") != std::string::npos);
    const mutators::Conflict* high_note =
        conflict_of(tougher_last, mutators::ConflictKind::same_field_hooked);
    const mutators::Conflict* low_note =
        conflict_of(tough_last, mutators::ConflictKind::same_field_hooked);
    OA_CHECK(high_note != nullptr && low_note != nullptr);
    if (high_note != nullptr) {
        const std::vector<std::string> ids{"ridge-tough", "ridge-tougher"};
        OA_CHECK(high_note->field == "maxdamage");
        OA_CHECK(
            mutators::describe(*high_note, ids) ==
            "ridge-tougher wins " + high_note->path + " " + high_note->field + " over ridge-tough"
        );
    }
    if (low_note != nullptr) {
        const std::vector<std::string> ids{"ridge-tougher", "ridge-tough"};
        OA_CHECK(
            mutators::describe(*low_note, ids) ==
            "ridge-tough wins " + low_note->path + " " + low_note->field + " over ridge-tougher"
        );
    }
    release(rising);
    release(falling);
}

void test_same_value_writes_nothing() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result = mutators::run_pipeline(store, input_of({"ridge-same"}));
    OA_CHECK(result.playable());
    OA_CHECK(result.layer.entries.empty());
    OA_CHECK(result.layer.masks.empty());
    OA_CHECK(!store.mutator_layer_mounted());
    for (const mutators::JournalEntry& entry : result.journal.entries)
        OA_CHECK(entry.field != "maxdamage");
    release(store);
}

void test_failed_mutator_leaves_no_trace() {
    oa::AssetStore with_boom_store = open_ridge();
    oa::AssetStore clean_store = open_ridge();
    const mutators::PipelineResult with_boom = mutators::run_pipeline(
        with_boom_store, input_of({"ridge-tough", "ridge-boom", "ridge-swift"})
    );
    const mutators::PipelineResult clean =
        mutators::run_pipeline(clean_store, input_of({"ridge-tough", "ridge-swift"}));
    OA_CHECK(with_boom.refusal.empty());
    OA_CHECK(!with_boom.playable());
    OA_CHECK(with_boom.mutators.size() == 3);
    if (with_boom.mutators.size() == 3) {
        OA_CHECK(with_boom.mutators[1].id == "ridge-boom");
        OA_CHECK(with_boom.mutators[1].outcome == mutators::Outcome::failed);
        OA_CHECK(with_boom.mutators[1].phase == mutators::Phase::hooks);
        OA_CHECK(with_boom.mutators[1].cause == mutators::Cause::script_error);
        OA_CHECK(with_boom.mutators[1].message.find("boom") != std::string::npos);
        OA_CHECK(with_boom.mutators[0].outcome == mutators::Outcome::applied);
        OA_CHECK(with_boom.mutators[2].outcome == mutators::Outcome::applied);
    }
    OA_CHECK(specs_match(with_boom.layer, clean.layer));
    OA_CHECK(journals_match(with_boom, clean));
    OA_CHECK(reports_match(with_boom, clean));
    for (const mutators::JournalEntry& entry : with_boom.journal.entries) {
        for (const uint16_t slot : entry.mutators)
            OA_CHECK(slot != 1);
    }
    const oa::AssetData bot = with_boom_store.read("units/RGBOT.fbi");
    const oa::AssetData commander = with_boom_store.read("units/RGCOM.fbi");
    OA_CHECK(text_of(bot.bytes).find("2.4") != std::string::npos);
    OA_CHECK(text_of(commander.bytes).find("4000") != std::string::npos);
    release(with_boom_store);
    release(clean_store);
}

void test_budgets() {
    oa::AssetStore loop_store = open_ridge();
    mutators::PipelineInput loop_input = input_of({"ridge-loop"});
    loop_input.budget.instructions = 2000000;
    const mutators::PipelineResult looped = mutators::run_pipeline(loop_store, loop_input);
    OA_CHECK(looped.mutators.size() == 1);
    if (looped.mutators.size() == 1) {
        OA_CHECK(looped.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(looped.mutators[0].cause == mutators::Cause::instruction_budget);
        OA_CHECK(looped.mutators[0].phase == mutators::Phase::hooks);
    }
    OA_CHECK(!loop_store.mutator_layer_mounted());
    release(loop_store);

    oa::AssetStore hog_store = open_ridge();
    mutators::PipelineInput hog_input = input_of({"ridge-hog"});
    hog_input.budget.memory_bytes = std::size_t{1} << 20;
    const mutators::PipelineResult hog = mutators::run_pipeline(hog_store, hog_input);
    OA_CHECK(hog.mutators.size() == 1);
    if (hog.mutators.size() == 1) {
        OA_CHECK(hog.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(hog.mutators[0].cause == mutators::Cause::memory_budget);
        OA_CHECK(hog.mutators[0].phase == mutators::Phase::hooks);
    }
    release(hog_store);
}

void test_plan_failures() {
    oa::AssetStore undeclared_store = open_ridge();
    const mutators::PipelineResult undeclared =
        mutators::run_pipeline(undeclared_store, input_of({"ridge-undeclared"}));
    OA_CHECK(undeclared.mutators.size() == 1);
    if (undeclared.mutators.size() == 1) {
        OA_CHECK(undeclared.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(undeclared.mutators[0].phase == mutators::Phase::plan);
        OA_CHECK(undeclared.mutators[0].cause == mutators::Cause::undeclared_layer);
        OA_CHECK(undeclared.mutators[0].message.find("units") != std::string::npos);
    }
    release(undeclared_store);

    oa::AssetStore wreck_store = open_ridge();
    const mutators::PipelineResult wreck =
        mutators::run_pipeline(wreck_store, input_of({"ridge-wreck-undeclared"}));
    OA_CHECK(wreck.mutators.size() == 1);
    if (wreck.mutators.size() == 1) {
        OA_CHECK(wreck.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(wreck.mutators[0].phase == mutators::Phase::hooks);
        OA_CHECK(wreck.mutators[0].cause == mutators::Cause::script_error);
        OA_CHECK(wreck.mutators[0].message.find("features") != std::string::npos);
    }
    release(wreck_store);

    oa::AssetStore case_store = open_ridge();
    const mutators::PipelineResult folded =
        mutators::run_pipeline(case_store, input_of({"ridge-case"}));
    OA_CHECK(folded.mutators.size() == 1);
    if (folded.mutators.size() == 1) {
        OA_CHECK(folded.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(folded.mutators[0].phase == mutators::Phase::plan);
        OA_CHECK(folded.mutators[0].cause == mutators::Cause::script_error);
        OA_CHECK(folded.mutators[0].message.find("lua/util.lua") != std::string::npos);
    }
    release(case_store);

    oa::AssetStore protected_store = open_ridge();
    const mutators::PipelineResult refused =
        mutators::run_pipeline(protected_store, input_of({"ridge-protected"}));
    OA_CHECK(refused.mutators.size() == 1);
    if (refused.mutators.size() == 1) {
        OA_CHECK(refused.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(refused.mutators[0].phase == mutators::Phase::files);
        OA_CHECK(refused.mutators[0].cause == mutators::Cause::files_refused);
    }
    release(protected_store);
}

void test_files_layer() {
    oa::AssetStore art_store = open_ridge();
    const mutators::PipelineResult art =
        mutators::run_pipeline(art_store, input_of({"ridge-art-a", "ridge-art-b"}));
    OA_CHECK(art.playable());
    const oa::AssetData picture = art_store.read("unitpics/RGBOT.pcx");
    OA_CHECK(text_of(picture.bytes) == "B\n");
    const mutators::Conflict* replaced = conflict_of(art, mutators::ConflictKind::same_path_added);
    OA_CHECK(replaced != nullptr);
    if (replaced != nullptr) {
        const std::vector<std::string> ids{"ridge-art-a", "ridge-art-b"};
        OA_CHECK(
            mutators::describe(*replaced, ids) ==
            "ridge-art-b wins " + replaced->path + " over ridge-art-a"
        );
    }
    release(art_store);

    oa::AssetStore mask_store = open_ridge();
    const mutators::PipelineResult masked =
        mutators::run_pipeline(mask_store, input_of({"ridge-art-a", "ridge-art-mask"}));
    OA_CHECK(masked.playable());
    OA_CHECK(!masked.layer.masks.empty());
    OA_CHECK(!lists_name(mask_store, "unitpics", ".pcx", "rgbot.pcx"));
    const mutators::Conflict* hidden =
        conflict_of(masked, mutators::ConflictKind::added_path_hidden);
    OA_CHECK(hidden != nullptr && hidden->severity == mutators::Severity::warning);
    if (hidden != nullptr) {
        const std::vector<std::string> ids{"ridge-art-a", "ridge-art-mask"};
        OA_CHECK(
            mutators::describe(*hidden, ids) ==
            "ridge-art-mask hides " + hidden->path + " added by ridge-art-a"
        );
    }
    release(mask_store);

    oa::AssetStore added_store = open_ridge();
    const mutators::PipelineResult added =
        mutators::run_pipeline(added_store, input_of({"ridge-add-unit", "ridge-edit-new"}));
    OA_CHECK(added.playable());
    const oa::AssetData created = added_store.read("units/RGNEW.fbi");
    OA_CHECK(text_of(created.bytes).find("2222") != std::string::npos);
    release(added_store);

    oa::AssetStore removed_store = open_ridge();
    const mutators::PipelineResult removed =
        mutators::run_pipeline(removed_store, input_of({"ridge-remove-bot"}));
    OA_CHECK(removed.playable());
    OA_CHECK(!lists_name(removed_store, "units", ".fbi", "rgbot.fbi"));
    // The hook's remove() hid the files, so the journal says the hooks did.
    std::size_t hidden_files = 0;
    for (const mutators::JournalEntry& entry : removed.journal.entries) {
        if (entry.change != mutators::Change::hidden)
            continue;
        ++hidden_files;
        OA_CHECK(entry.phase == mutators::Phase::hooks);
        OA_CHECK(entry.collection == mutators::Collection::files);
    }
    OA_CHECK(hidden_files >= 1);
    release(removed_store);
    OA_CHECK(lists_name(removed_store, "units", ".fbi", "rgbot.fbi"));
}

void test_list_rules() {
    oa::AssetStore early_store = open_ridge();
    const mutators::PipelineResult early =
        mutators::run_pipeline(early_store, input_of({"ridge-needs-swift", "ridge-swift"}));
    OA_CHECK(early.refusal.find("above it") != std::string::npos);
    OA_CHECK(!early.playable());
    OA_CHECK(!early_store.mutator_layer_mounted());
    OA_CHECK(early.layer.entries.empty());
    for (const mutators::MutatorResult& mutator : early.mutators)
        OA_CHECK(mutator.outcome == mutators::Outcome::excluded);
    release(early_store);

    oa::AssetStore late_store = open_ridge();
    const mutators::PipelineResult late =
        mutators::run_pipeline(late_store, input_of({"ridge-swift", "ridge-needs-swift"}));
    OA_CHECK(late.refusal.empty());
    OA_CHECK(late.playable());
    OA_CHECK(late.mutators.size() == 2);
    release(late_store);

    oa::AssetStore boom_store = open_ridge();
    const mutators::PipelineResult boom =
        mutators::run_pipeline(boom_store, input_of({"ridge-boom", "ridge-needs-boom"}));
    OA_CHECK(boom.mutators.size() == 2);
    if (boom.mutators.size() == 2) {
        OA_CHECK(boom.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(boom.mutators[1].outcome == mutators::Outcome::excluded);
        OA_CHECK(boom.mutators[1].cause == mutators::Cause::prerequisite_failed);
        OA_CHECK(boom.mutators[1].phase == mutators::Phase::loadout);
        OA_CHECK(
            boom.mutators[1].message.find("needs ridge-boom, which failed") != std::string::npos
        );
    }
    release(boom_store);

    oa::AssetStore conflict_store = open_ridge();
    const mutators::PipelineResult conflict =
        mutators::run_pipeline(conflict_store, input_of({"ridge-swift", "ridge-against-swift"}));
    OA_CHECK(conflict.refusal.find("conflicts") != std::string::npos);
    OA_CHECK(!conflict_store.mutator_layer_mounted());
    release(conflict_store);

    oa::AssetStore duplicate_store = open_ridge();
    mutators::PipelineInput duplicate =
        make_input({load_id("ridge-swift"), load_id("ridge-swift")});
    const mutators::PipelineResult twice = mutators::run_pipeline(duplicate_store, duplicate);
    OA_CHECK(twice.refusal.find("listed twice") != std::string::npos);
    OA_CHECK(!duplicate_store.mutator_layer_mounted());
    release(duplicate_store);

    const fs::path scratch = oa::test::make_scratch_directory("V01-loadout");
    std::vector<mutators::PipelineMutator> many;
    for (int number = 0; number < 33; ++number) {
        std::string id = "mutator-";
        if (number < 10)
            id += "0";
        id += std::to_string(number);
        const fs::path package = scratch / id;
        const std::string yaml =
            "oamut: 1\nid: " + id + "\nname: \"Loadout " + id +
            "\"\nversion: \"1.0\"\nauthor:\n  name: \"Ridge Maker\"\ntags: [\"gameplay\"]\n"
            "works-with: [\"*\"]\nlayers: [\"files\"]\n";
        write_bytes(package / "oamut.yaml", yaml);
        write_bytes(package / "mutator.lua", "function mutate(ctx)\n  return {}\nend\n");
        many.push_back(load_mutator(package));
    }
    oa::AssetStore crowd_store = open_ridge();
    const mutators::PipelineResult crowd =
        mutators::run_pipeline(crowd_store, make_input(std::move(many)));
    OA_CHECK(crowd.refusal.find("32") != std::string::npos);
    OA_CHECK(!crowd_store.mutator_layer_mounted());
    OA_CHECK(crowd.layer.entries.empty());
    release(crowd_store);
    fs::remove_all(scratch);
}

void test_compatibility_and_repeat() {
    oa::AssetStore elsewhere_store = open_ridge();
    const mutators::PipelineResult elsewhere =
        mutators::run_pipeline(elsewhere_store, input_of({"ridge-elsewhere"}));
    OA_CHECK(elsewhere.mutators.size() == 1);
    if (elsewhere.mutators.size() == 1) {
        OA_CHECK(elsewhere.mutators[0].outcome == mutators::Outcome::incompatible);
        OA_CHECK(elsewhere.mutators[0].phase == mutators::Phase::manifest);
        OA_CHECK(elsewhere.mutators[0].cause == mutators::Cause::works_with);
        OA_CHECK(elsewhere.mutators[0].message.find("some-other-mod") != std::string::npos);
        OA_CHECK(elsewhere.mutators[0].message.find("ridge") != std::string::npos);
    }
    release(elsewhere_store);

    oa::AssetStore future_store = open_ridge();
    const mutators::PipelineResult future =
        mutators::run_pipeline(future_store, input_of({"ridge-future"}));
    OA_CHECK(future.mutators.size() == 1);
    if (future.mutators.size() == 1) {
        OA_CHECK(future.mutators[0].outcome == mutators::Outcome::incompatible);
        OA_CHECK(future.mutators[0].phase == mutators::Phase::manifest);
        OA_CHECK(future.mutators[0].cause == mutators::Cause::engine_version);
        OA_CHECK(future.mutators[0].message.find("99.0") != std::string::npos);
        OA_CHECK(future.mutators[0].message.find("0.8.5") != std::string::npos);
    }
    release(future_store);

    oa::AssetStore first_store = open_ridge();
    oa::AssetStore second_store = open_ridge();
    const mutators::PipelineInput input = input_of({"ridge-tough", "ridge-swift"});
    const mutators::PipelineResult first = mutators::run_pipeline(first_store, input);
    const mutators::PipelineResult second = mutators::run_pipeline(second_store, input);
    OA_CHECK(first.playable() && second.playable());
    OA_CHECK(specs_match(first.layer, second.layer));
    OA_CHECK(journals_match(first, second));
    OA_CHECK(reports_match(first, second));
    release(first_store);
    release(second_store);

    oa::AssetStore mounted = open_ridge();
    const mutators::PipelineResult kept =
        mutators::run_pipeline(mounted, input_of({"ridge-swift"}));
    OA_CHECK(mounted.mutator_layer_mounted());
    const std::string label = kept.layer.label;
    const oa::AssetData before = mounted.read("units/RGBOT.fbi");
    const mutators::PipelineResult refused =
        mutators::run_pipeline(mounted, input_of({"ridge-swift"}));
    OA_CHECK(refused.refusal.find("already mounted") != std::string::npos);
    OA_CHECK(mounted.mutator_layer_mounted());
    OA_CHECK(kept.layer.label == label);
    const oa::AssetData after = mounted.read("units/RGBOT.fbi");
    OA_CHECK(before.bytes == after.bytes);
    release(mounted);
}

void test_failure_ends_the_pass() {
    // A hook that fails ends the pass before any mutator below it runs.
    // The retry is that lower mutator on its own.
    oa::AssetStore rule_store = open_ridge();
    oa::AssetStore picky_store = open_ridge();
    const mutators::PipelineResult rule_boom =
        mutators::run_pipeline(rule_store, input_of({"ridge-rule-boom", "ridge-picky"}));
    const mutators::PipelineResult picky_alone =
        mutators::run_pipeline(picky_store, input_of({"ridge-picky"}));
    OA_CHECK(rule_boom.mutators.size() == 2);
    if (rule_boom.mutators.size() == 2) {
        OA_CHECK(rule_boom.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(rule_boom.mutators[0].phase == mutators::Phase::hooks);
        OA_CHECK(rule_boom.mutators[0].cause == mutators::Cause::script_error);
        OA_CHECK(rule_boom.mutators[0].message.find("rule-boom hook") != std::string::npos);
        OA_CHECK(rule_boom.mutators[1].outcome == mutators::Outcome::applied);
    }
    check_same_run(rule_boom, picky_alone);
    check_no_trace(rule_boom, 0);
    OA_CHECK(text_of(rule_store.read("units/RGBOT.fbi").bytes).find("1234") != std::string::npos);
    OA_CHECK(
        text_of(rule_store.read("units/RGCOM.fbi").bytes).find("MaxDamage=3000") !=
        std::string::npos
    );
    release(rule_store);
    release(picky_store);

    // A failing hook's own files never reach the hooks below it.
    oa::AssetStore files_store = open_ridge();
    oa::AssetStore no_extra_store = open_ridge();
    const mutators::PipelineResult files_boom =
        mutators::run_pipeline(files_store, input_of({"ridge-files-boom", "ridge-no-extra"}));
    const mutators::PipelineResult no_extra_alone =
        mutators::run_pipeline(no_extra_store, input_of({"ridge-no-extra"}));
    OA_CHECK(files_boom.mutators.size() == 2);
    if (files_boom.mutators.size() == 2) {
        OA_CHECK(files_boom.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(files_boom.mutators[0].phase == mutators::Phase::hooks);
        OA_CHECK(files_boom.mutators[0].message.find("files-boom hook") != std::string::npos);
        OA_CHECK(files_boom.mutators[1].outcome == mutators::Outcome::applied);
    }
    check_same_run(files_boom, no_extra_alone);
    check_no_trace(files_boom, 0);
    OA_CHECK(!lists_name(files_store, "units", ".fbi", "rgxtra.fbi"));
    OA_CHECK(text_of(files_store.read("units/RGLAB.fbi").bytes).find("222") != std::string::npos);
    release(files_store);
    release(no_extra_store);

    // The same with the failing mutator's dependant between them.
    oa::AssetStore chain_store = open_ridge();
    oa::AssetStore chain_alone_store = open_ridge();
    const mutators::PipelineResult chain = mutators::run_pipeline(
        chain_store, input_of({"ridge-rule-boom", "ridge-needs-rule-boom", "ridge-picky"})
    );
    const mutators::PipelineResult chain_alone =
        mutators::run_pipeline(chain_alone_store, input_of({"ridge-picky"}));
    OA_CHECK(chain.mutators.size() == 3);
    if (chain.mutators.size() == 3) {
        OA_CHECK(chain.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(chain.mutators[1].outcome == mutators::Outcome::excluded);
        OA_CHECK(chain.mutators[1].cause == mutators::Cause::prerequisite_failed);
        OA_CHECK(chain.mutators[1].phase == mutators::Phase::loadout);
        OA_CHECK(
            chain.mutators[1].message.find("needs ridge-rule-boom, which failed") !=
            std::string::npos
        );
        OA_CHECK(chain.mutators[2].outcome == mutators::Outcome::applied);
    }
    check_same_run(chain, chain_alone);
    check_no_trace(chain, 0);
    check_no_trace(chain, 1);
    OA_CHECK(text_of(chain_store.read("units/RGBOT.fbi").bytes).find("777") == std::string::npos);
    release(chain_store);
    release(chain_alone_store);
}

void test_dependants_of_dependants() {
    oa::AssetStore with_store = open_ridge();
    oa::AssetStore swift_store = open_ridge();
    const mutators::PipelineResult with_chain = mutators::run_pipeline(
        with_store,
        input_of({"ridge-boom", "ridge-needs-boom", "ridge-needs-needs-boom", "ridge-swift"})
    );
    const mutators::PipelineResult swift_alone =
        mutators::run_pipeline(swift_store, input_of({"ridge-swift"}));
    OA_CHECK(with_chain.mutators.size() == 4);
    if (with_chain.mutators.size() == 4) {
        OA_CHECK(with_chain.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(with_chain.mutators[1].outcome == mutators::Outcome::excluded);
        OA_CHECK(with_chain.mutators[1].phase == mutators::Phase::loadout);
        OA_CHECK(
            with_chain.mutators[1].message == "ridge-needs-boom needs ridge-boom, which failed"
        );
        OA_CHECK(with_chain.mutators[2].outcome == mutators::Outcome::excluded);
        OA_CHECK(with_chain.mutators[2].phase == mutators::Phase::loadout);
        OA_CHECK(with_chain.mutators[2].cause == mutators::Cause::prerequisite_failed);
        OA_CHECK(
            with_chain.mutators[2].message ==
            "ridge-needs-needs-boom needs ridge-needs-boom, which did not apply"
        );
        OA_CHECK(with_chain.mutators[3].outcome == mutators::Outcome::applied);
    }
    check_same_run(with_chain, swift_alone);
    release(with_store);
    release(swift_store);
}

void test_plan_tables_are_plain() {
    // The plan is read by raw access inside the plan phase. A files table
    // with a metatable is refused there, so its __index never runs: neither
    // its error nor its endless loop can reach the engine.
    for (const std::string_view id : {"ridge-files-raise", "ridge-files-loop"}) {
        oa::AssetStore store = open_ridge();
        mutators::PipelineInput input = input_of({id});
        input.budget.instructions = 2000000;
        const mutators::PipelineResult result = mutators::run_pipeline(store, input);
        OA_CHECK(result.refusal.empty());
        OA_CHECK(result.mutators.size() == 1);
        if (result.mutators.size() == 1) {
            OA_CHECK(result.mutators[0].outcome == mutators::Outcome::failed);
            OA_CHECK(result.mutators[0].phase == mutators::Phase::plan);
            OA_CHECK(result.mutators[0].cause == mutators::Cause::invalid_plan);
            OA_CHECK(result.mutators[0].message.find("metatable") != std::string::npos);
        }
        OA_CHECK(!store.mutator_layer_mounted());
        release(store);
    }
}

void test_mask_is_journaled() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult masked =
        mutators::run_pipeline(store, input_of({"ridge-mask-lab"}));
    OA_CHECK(masked.playable());
    OA_CHECK(masked.layer.masks.size() == 2);
    OA_CHECK(!lists_name(store, "units", ".fbi", "rglab.fbi"));
    const std::vector<mutators::JournalEntry> own = masked.journal.entries_of(0);
    OA_CHECK(own.size() == 2);
    if (own.size() == 2) {
        std::vector<std::string> paths;
        for (const mutators::JournalEntry& entry : own) {
            OA_CHECK(entry.change == mutators::Change::hidden);
            OA_CHECK(entry.phase == mutators::Phase::files);
            OA_CHECK(entry.collection == mutators::Collection::files);
            OA_CHECK(entry.mutators == std::vector<uint16_t>{0});
            paths.push_back(entry.path);
        }
        OA_CHECK(count_paths(paths, "units/rglab.fbi") == 1);
        OA_CHECK(count_paths(paths, "download/rglab.tdf") == 1);
    }
    release(store);
}

void test_string_cap_is_its_own_cause() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, input_of({"ridge-long-text"}));
    OA_CHECK(result.mutators.size() == 1);
    if (result.mutators.size() == 1) {
        OA_CHECK(result.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(result.mutators[0].phase == mutators::Phase::hooks);
        OA_CHECK(result.mutators[0].cause == mutators::Cause::string_length);
    }
    release(store);
}

void test_unread_document_warned_once() {
    const fs::path scratch = oa::test::make_scratch_directory("V01-unread");
    const fs::path copy = scratch / "ridge";
    fs::copy(k_data / "ridge", copy, fs::copy_options::recursive);
    write_bytes(copy / "units" / "RGBAD.fbi", "[UNITINFO]\n{\nUnitName=RGBAD;\n");
    {
        oa::AssetStore store(copy);
        const mutators::PipelineResult result =
            mutators::run_pipeline(store, input_of({"ridge-swift"}));
        OA_CHECK(result.playable());
        OA_CHECK(count_paths(result.report.unread, "units/rgbad.fbi") == 1);
        release(store);
    }
    fs::remove_all(scratch);
}

/// Two assignments of one field. The later mutator wins, in either order,
/// and the report names that pair once even though every unit was written.
void test_later_assignment_wins() {
    oa::AssetStore forward_store = open_ridge();
    oa::AssetStore backward_store = open_ridge();
    const mutators::PipelineResult forward =
        mutators::run_pipeline(forward_store, input_of({"ridge-mark-low", "ridge-mark-high"}));
    const mutators::PipelineResult backward =
        mutators::run_pipeline(backward_store, input_of({"ridge-mark-high", "ridge-mark-low"}));
    OA_CHECK(forward.playable() && backward.playable());
    for (const char* path : {"units/RGCOM.fbi", "units/RGBOT.fbi", "units/RGLAB.fbi"}) {
        const std::string high = text_of(forward_store.read(path).bytes);
        const std::string low = text_of(backward_store.read(path).bytes);
        OA_CHECK(high.find("2500") != std::string::npos);
        OA_CHECK(high.find("1500") == std::string::npos);
        OA_CHECK(low.find("1500") != std::string::npos);
        OA_CHECK(low.find("2500") == std::string::npos);
    }
    OA_CHECK(count_kind(forward, mutators::ConflictKind::value_replaced) == 1);
    OA_CHECK(count_kind(backward, mutators::ConflictKind::value_replaced) == 1);
    const mutators::Conflict* replaced =
        conflict_of(forward, mutators::ConflictKind::value_replaced);
    OA_CHECK(replaced != nullptr);
    if (replaced != nullptr) {
        const std::vector<std::string> ids{"ridge-mark-low", "ridge-mark-high"};
        OA_CHECK(replaced->field == "maxdamage");
        OA_CHECK(replaced->mutators.size() == 2 && replaced->mutators[0] == 0);
        OA_CHECK(replaced->mutators.size() == 2 && replaced->mutators[1] == 1);
        OA_CHECK(
            mutators::describe(*replaced, ids) ==
            "ridge-mark-high replaced ridge-mark-low's value on " + replaced->path + " maxdamage"
        );
    }
    release(forward_store);
    release(backward_store);
}

/// A rule below a hook reads the value that hook wrote. Rules of every
/// mutator first would scale before the hook, and the bot would stay at 1000.
void test_rule_sees_the_hook_above() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult stepped =
        mutators::run_pipeline(store, input_of({"ridge-stamp", "ridge-double-stamp"}));
    OA_CHECK(stepped.playable());
    const std::string bot = text_of(store.read("units/RGBOT.fbi").bytes);
    const std::string commander = text_of(store.read("units/RGCOM.fbi").bytes);
    OA_CHECK(bot.find("2000") != std::string::npos);
    OA_CHECK(bot.find("1000") == std::string::npos);
    OA_CHECK(commander.find("6000") != std::string::npos);
    release(store);

    oa::AssetStore reversed_store = open_ridge();
    const mutators::PipelineResult rules_first =
        mutators::run_pipeline(reversed_store, input_of({"ridge-double-stamp", "ridge-stamp"}));
    OA_CHECK(rules_first.playable());
    const std::string untouched = text_of(reversed_store.read("units/RGBOT.fbi").bytes);
    OA_CHECK(untouched.find("1000") != std::string::npos);
    OA_CHECK(untouched.find("2000") == std::string::npos);
    release(reversed_store);
}

/// A hook that sets one field on every unit replaces the rule above it, and
/// the report names that pair once.
void test_hook_replaces_a_rule() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, input_of({"ridge-mark-low", "ridge-hook-mark"}));
    OA_CHECK(result.playable());
    for (const char* path : {"units/RGCOM.fbi", "units/RGBOT.fbi", "units/RGLAB.fbi"}) {
        const std::string text = text_of(store.read(path).bytes);
        OA_CHECK(text.find("4242") != std::string::npos);
        OA_CHECK(text.find("1500") == std::string::npos);
    }
    OA_CHECK(count_kind(result, mutators::ConflictKind::same_field_hooked) == 1);
    const mutators::Conflict* hooked =
        conflict_of(result, mutators::ConflictKind::same_field_hooked);
    OA_CHECK(hooked != nullptr);
    if (hooked != nullptr) {
        const std::vector<std::string> ids{"ridge-mark-low", "ridge-hook-mark"};
        OA_CHECK(hooked->field == "maxdamage");
        OA_CHECK(hooked->mutators.size() == 2 && hooked->mutators[0] == 0);
        OA_CHECK(hooked->mutators.size() == 2 && hooked->mutators[1] == 1);
        OA_CHECK(
            mutators::describe(*hooked, ids) ==
            "ridge-hook-mark wins " + hooked->path + " maxdamage over ridge-mark-low"
        );
    }
    release(store);
}

/// Each step names the mutator who last wrote the field. A rule under a hook
/// names that hook, and a hook under a later rule names that rule.
void test_each_step_names_the_last_writer() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult stepped = mutators::run_pipeline(
        store, input_of({"ridge-mark-low", "ridge-hook-mark", "ridge-mark-high"})
    );
    OA_CHECK(stepped.playable());
    OA_CHECK(text_of(store.read("units/RGCOM.fbi").bytes).find("2500") != std::string::npos);
    OA_CHECK(count_kind(stepped, mutators::ConflictKind::same_field_hooked) == 1);
    OA_CHECK(count_kind(stepped, mutators::ConflictKind::value_replaced) == 1);
    const std::vector<std::string> three{"ridge-mark-low", "ridge-hook-mark", "ridge-mark-high"};
    const mutators::Conflict* hooked =
        conflict_of(stepped, mutators::ConflictKind::same_field_hooked);
    const mutators::Conflict* replaced =
        conflict_of(stepped, mutators::ConflictKind::value_replaced);
    OA_CHECK(hooked != nullptr && replaced != nullptr);
    if (hooked != nullptr) {
        OA_CHECK(hooked->mutators.size() == 2 && hooked->mutators[0] == 0);
        OA_CHECK(hooked->mutators.size() == 2 && hooked->mutators[1] == 1);
        OA_CHECK(
            mutators::describe(*hooked, three) ==
            "ridge-hook-mark wins " + hooked->path + " maxdamage over ridge-mark-low"
        );
    }
    if (replaced != nullptr) {
        OA_CHECK(replaced->mutators.size() == 2 && replaced->mutators[0] == 1);
        OA_CHECK(replaced->mutators.size() == 2 && replaced->mutators[1] == 2);
        OA_CHECK(
            mutators::describe(*replaced, three) ==
            "ridge-mark-high replaced ridge-hook-mark's value on " + replaced->path + " maxdamage"
        );
    }
    release(store);

    oa::AssetStore chain_store = open_ridge();
    const mutators::PipelineResult chain = mutators::run_pipeline(
        chain_store,
        input_of({"ridge-mark-low", "ridge-hook-mark", "ridge-mark-high", "ridge-stamp"})
    );
    OA_CHECK(chain.playable());
    const std::string bot = text_of(chain_store.read("units/RGBOT.fbi").bytes);
    OA_CHECK(bot.find("1000") != std::string::npos);
    OA_CHECK(bot.find("2500") == std::string::npos);
    OA_CHECK(count_kind(chain, mutators::ConflictKind::same_field_hooked) == 2);
    const std::vector<std::string> four{
        "ridge-mark-low", "ridge-hook-mark", "ridge-mark-high", "ridge-stamp"
    };
    bool stamp_names_the_rule = false;
    bool stamp_names_the_hook = false;
    for (const mutators::Conflict& conflict : chain.report.conflicts) {
        if (conflict.kind != mutators::ConflictKind::same_field_hooked)
            continue;
        const std::string line = mutators::describe(conflict, four);
        if (line.find("ridge-stamp wins ") == std::string::npos)
            continue;
        if (line.find("over ridge-mark-high") != std::string::npos)
            stamp_names_the_rule = true;
        if (line.find("over ridge-hook-mark") != std::string::npos)
            stamp_names_the_hook = true;
    }
    OA_CHECK(stamp_names_the_rule);
    OA_CHECK(!stamp_names_the_hook);
    release(chain_store);
}

/// A lock holds against a later mutator's rules and is noted. That mutator's
/// hook still writes the field, and the report names both.
void test_lock_holds_against_rules_not_hooks() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, input_of({"ridge-lock-com", "ridge-past-lock"}));
    OA_CHECK(result.playable());
    const std::string commander = text_of(store.read("units/RGCOM.fbi").bytes);
    OA_CHECK(commander.find("7777") != std::string::npos);
    OA_CHECK(commander.find("9999") == std::string::npos);
    OA_CHECK(commander.find("1111") == std::string::npos);
    OA_CHECK(count_kind(result, mutators::ConflictKind::field_locked) == 1);
    OA_CHECK(count_kind(result, mutators::ConflictKind::same_field_hooked) == 1);
    const std::vector<std::string> ids{"ridge-lock-com", "ridge-past-lock"};
    const mutators::Conflict* locked = conflict_of(result, mutators::ConflictKind::field_locked);
    OA_CHECK(locked != nullptr);
    if (locked != nullptr) {
        OA_CHECK(locked->field == "maxdamage");
        OA_CHECK(
            mutators::describe(*locked, ids) ==
            "ridge-lock-com locked " + locked->path + " maxdamage, so ridge-past-lock left it alone"
        );
    }
    const mutators::Conflict* hooked =
        conflict_of(result, mutators::ConflictKind::same_field_hooked);
    OA_CHECK(hooked != nullptr);
    if (hooked != nullptr) {
        OA_CHECK(hooked->field == "maxdamage");
        OA_CHECK(
            mutators::describe(*hooked, ids) ==
            "ridge-past-lock wins " + hooked->path + " maxdamage over ridge-lock-com"
        );
    }
    bool wrote_blocked_value = false;
    for (const mutators::JournalEntry& entry : result.journal.entries) {
        if (entry.new_text.has_value() && *entry.new_text == "9999")
            wrote_blocked_value = true;
    }
    OA_CHECK(!wrote_blocked_value);
    release(store);
}

/// Resolves the planned hacks against the base profile, as the app's check does.
std::vector<mutators::HackCheckError>
refuse_unresolved_hacks(std::span<const mutators::MutatorHack> hacks) {
    std::vector<mutators::HackCheckError> faults;
    if (hacks.empty())
        return faults;
    const std::string text = oa::data::mod_profile::base_game_profile_text();
    oa::data::mod_profile::ResolveOptions options;
    options.accept_unimplemented_hacks = true;
    for (const mutators::MutatorHack& hack : hacks) {
        oa::data::mod_profile::MatchOption option;
        option.path = hack.path;
        option.value = hack.value;
        option.source = hack.mutator;
        option.turn_on = true;
        options.match.push_back(std::move(option));
    }
    const oa::data::mod_profile::ResolveResult resolved = oa::data::mod_profile::resolve_profile(
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(text.data()), text.size()},
        "base",
        options
    );
    if (resolved.resolution.has_value())
        return faults;
    return mutators::hack_faults(resolved.errors, hacks);
}

void test_refused_hack_drops_the_hook() {
    const fs::path scratch = oa::test::make_scratch_directory("D08-bad-range");
    const fs::path bad = scratch / "ridge-bad-range";
    const fs::path needs = scratch / "ridge-needs-bad-range";
    write_bytes(
        bad / "oamut.yaml",
        "oamut: 1\nid: ridge-bad-range\nname: \"Ridge Bad Range\"\nversion: \"1.0\"\n"
        "author:\n  name: \"Ridge Maker\"\ntags: [\"gameplay\"]\nworks-with: [\"*\"]\n"
        "layers: [\"hacks\", \"units\"]\n"
    );
    write_bytes(
        bad / "mutator.lua",
        "function mutate(ctx)\n"
        "  return {\n"
        "    hacks = { [\"console.game-speed-range\"] = { min = 15, max = 5 } },\n"
        "    units = function(units, ctx)\n"
        "      units:where { commander = true }:set { maxdamage = 7000 }\n"
        "    end\n"
        "  }\n"
        "end\n"
    );
    write_bytes(
        needs / "oamut.yaml",
        "oamut: 1\nid: ridge-needs-bad-range\nname: \"Ridge Needs Bad Range\"\nversion: \"1.0\"\n"
        "author:\n  name: \"Ridge Maker\"\ntags: [\"gameplay\"]\nworks-with: [\"*\"]\n"
        "requires:\n  mutators: [\"ridge-bad-range >= 1.0\"]\nlayers: [\"units\"]\n"
    );
    write_bytes(
        needs / "mutator.lua",
        "function mutate(ctx)\n"
        "  return {\n"
        "    units = function(units, ctx)\n"
        "      units:where { commander = true }:set { maxdamage = 8000 }\n"
        "    end\n"
        "  }\n"
        "end\n"
    );
    mutators::PipelineInput input = make_input({load_mutator(bad), load_mutator(needs)});
    input.check_hacks = refuse_unresolved_hacks;
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result = mutators::run_pipeline(store, input);
    OA_CHECK(result.refusal.empty());
    OA_CHECK(result.mutators.size() == 2);
    if (result.mutators.size() == 2) {
        OA_CHECK(result.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(result.mutators[0].phase == mutators::Phase::plan);
        OA_CHECK(result.mutators[0].cause == mutators::Cause::hack_refused);
        OA_CHECK(result.mutators[0].message.find("min <= max") != std::string::npos);
        OA_CHECK(result.mutators[0].hacks.empty());
        OA_CHECK(result.mutators[1].outcome == mutators::Outcome::excluded);
        OA_CHECK(result.mutators[1].cause == mutators::Cause::prerequisite_failed);
        OA_CHECK(result.mutators[1].message.find("ridge-bad-range") != std::string::npos);
    }
    OA_CHECK(result.journal.entries.empty());
    check_no_trace(result, 0);
    check_no_trace(result, 1);
    OA_CHECK(!store.mutator_layer_mounted());
    const std::string commander = text_of(store.read("units/RGCOM.fbi").bytes);
    OA_CHECK(commander.find("MaxDamage=3000") != std::string::npos);
    OA_CHECK(commander.find("7000") == std::string::npos);
    OA_CHECK(commander.find("8000") == std::string::npos);
    release(store);
    fs::remove_all(scratch);
}

/// Writes a mutator that only sets hacks.
void write_hack_mutator(const fs::path& package, std::string_view id, std::string_view hacks) {
    write_bytes(
        package / "oamut.yaml",
        "oamut: 1\nid: " + std::string{id} + "\nname: \"" + std::string{id} +
            "\"\nversion: \"1.0\"\nauthor:\n  name: \"Ridge Maker\"\ntags: [\"gameplay\"]\n"
            "works-with: [\"*\"]\nlayers: [\"hacks\"]\n"
    );
    write_bytes(
        package / "mutator.lua",
        "function mutate(ctx)\n  return { hacks = { " + std::string{hacks} + " } }\nend\n"
    );
}

/// A mutator whose hacks break a constraint fails alone. One beside it whose
/// hack resolves keeps it, in either order.
void test_refused_hack_spares_the_others() {
    const fs::path scratch = oa::test::make_scratch_directory("D08-spares");
    const fs::path bad = scratch / "ridge-bad-range";
    const fs::path cap = scratch / "ridge-cap";
    write_hack_mutator(
        bad, "ridge-bad-range", "[\"console.game-speed-range\"] = { min = 15, max = 5 }"
    );
    write_hack_mutator(cap, "ridge-cap", "[\"limits.units-per-player\"] = 400");
    for (const bool bad_first : {true, false}) {
        std::vector<mutators::PipelineMutator> list;
        list.push_back(load_mutator(bad_first ? bad : cap));
        list.push_back(load_mutator(bad_first ? cap : bad));
        mutators::PipelineInput input = make_input(std::move(list));
        input.check_hacks = refuse_unresolved_hacks;
        oa::AssetStore store = open_ridge();
        const mutators::PipelineResult result = mutators::run_pipeline(store, input);
        OA_CHECK(result.refusal.empty());
        OA_CHECK(result.mutators.size() == 2);
        if (result.mutators.size() == 2) {
            const mutators::MutatorResult& refused = result.mutators[bad_first ? 0 : 1];
            const mutators::MutatorResult& kept = result.mutators[bad_first ? 1 : 0];
            OA_CHECK(refused.id == "ridge-bad-range");
            OA_CHECK(refused.outcome == mutators::Outcome::failed);
            OA_CHECK(refused.cause == mutators::Cause::hack_refused);
            OA_CHECK(refused.message.find("console.game-speed-range") != std::string::npos);
            OA_CHECK(refused.message.find("min <= max") != std::string::npos);
            OA_CHECK(kept.id == "ridge-cap");
            OA_CHECK(kept.outcome == mutators::Outcome::applied);
            OA_CHECK(kept.message.empty());
            OA_CHECK(kept.hacks.size() == 1);
            if (kept.hacks.size() == 1) {
                OA_CHECK(kept.hacks[0].path == "limits.units-per-player.default");
                OA_CHECK(kept.hacks[0].value == "400");
            }
        }
        release(store);
    }
    fs::remove_all(scratch);
}

/// A hacks table in a manifest without the hacks layer fails that mutator
/// alone, as a refused hack, and the one beside it applies.
void test_hacks_without_the_layer() {
    const fs::path scratch = oa::test::make_scratch_directory("D08-no-layer");
    const fs::path bare = scratch / "ridge-no-layer";
    const fs::path cap = scratch / "ridge-cap";
    write_bytes(
        bare / "oamut.yaml",
        "oamut: 1\nid: ridge-no-layer\nname: \"Ridge No Layer\"\nversion: \"1.0\"\n"
        "author:\n  name: \"Ridge Maker\"\ntags: [\"gameplay\"]\nworks-with: [\"*\"]\n"
        "layers: [\"units\"]\n"
    );
    write_bytes(
        bare / "mutator.lua",
        "function mutate(ctx)\n"
        "  return { hacks = { [\"console.game-speed-range\"] = { max = 15 } } }\n"
        "end\n"
    );
    write_hack_mutator(cap, "ridge-cap", "[\"limits.units-per-player\"] = 400");
    mutators::PipelineInput input = make_input({load_mutator(bare), load_mutator(cap)});
    input.check_hacks = refuse_unresolved_hacks;
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result = mutators::run_pipeline(store, input);
    OA_CHECK(result.refusal.empty());
    OA_CHECK(result.mutators.size() == 2);
    if (result.mutators.size() == 2) {
        OA_CHECK(result.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(result.mutators[0].phase == mutators::Phase::plan);
        OA_CHECK(result.mutators[0].cause == mutators::Cause::hack_refused);
        OA_CHECK(
            result.mutators[0].message ==
            "hacks: console.game-speed-range: the manifest does not declare the hacks layer"
        );
        OA_CHECK(result.mutators[1].outcome == mutators::Outcome::applied);
        OA_CHECK(result.mutators[1].hacks.size() == 1);
    }
    release(store);
    fs::remove_all(scratch);
}

void test_player_option() {
    mutators::PipelineMutator swift = load_id("ridge-swift");
    for (const oamut::Option& option : swift.manifest.options) {
        if (option.id != "factor")
            continue;
        mutators::LoadoutOption chosen = mutators::default_option(option);
        for (const oamut::ChoiceValue& value : option.values) {
            if (value.text == "2")
                chosen.value = value;
        }
        swift.options.push_back(chosen);
    }
    OA_CHECK(swift.options.size() == 1);
    std::vector<mutators::PipelineMutator> list;
    list.push_back(std::move(swift));
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    OA_CHECK(text_of(store.read("units/RGBOT.fbi").bytes).find("3.2") != std::string::npos);
    release(store);
}

int weapon_slot(oa::AssetStore& store, const char* name) {
    auto table = std::make_unique<defs::WeaponTable>();
    defs::weapon_table_init(table.get());
    const defs::Files files = defs::asset_store_files(&store);
    defs::load_weapon_defs(&files, table.get(), nullptr);
    const int slot = defs::find_weapon_by_name(table->defs, name);
    defs::weapon_table_free(table.get());
    return slot;
}

/// Two mutators share weapons/extra.tdf and both ask for id 5. Each weapon
/// stays, the earlier mutator keeps 5, and each unit's Weapon1 names the
/// weapon that received that unit's id.
void test_two_weapon_files_keep_both_weapons() {
    oa::AssetStore store = open_ridge();
    const std::vector<uint8_t> base_weapons = store.read("weapons/ridge.tdf").bytes;
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, input_of({"ridge-weap-a", "ridge-weap-b"}));
    OA_CHECK(result.playable());
    OA_CHECK(store.read("weapons/ridge.tdf").bytes == base_weapons);
    OA_CHECK(weapon_slot(store, "RGGUN") == 1);
    OA_CHECK(weapon_slot(store, "RGBLAST") == 2);
    OA_CHECK(weapon_slot(store, "RGALPHA") == 5);
    OA_CHECK(weapon_slot(store, "RGBETA") == 3);
    const std::string alpha = text_of(store.read("units/RGALPHA.fbi").bytes);
    const std::string beta = text_of(store.read("units/RGBETA.fbi").bytes);
    OA_CHECK(alpha.find("Weapon1=RGALPHA") != std::string::npos);
    OA_CHECK(beta.find("Weapon1=RGBETA") != std::string::npos);
    const std::string weapons = text_of(store.read("weapons/extra.tdf").bytes);
    OA_CHECK(weapons.find("RGALPHA") != std::string::npos);
    OA_CHECK(weapons.find("RGBETA") != std::string::npos);
    release(store);
}

/// A later unit file replaces the earlier record. A replaced base weapon
/// keeps its id, and a new weapon that asked for that id takes another.
void test_records_replace_and_base_weapon_id_stays() {
    oa::AssetStore bots = open_ridge();
    const mutators::PipelineResult replaced =
        mutators::run_pipeline(bots, input_of({"ridge-bot-a", "ridge-bot-b"}));
    OA_CHECK(replaced.playable());
    const std::string bot = text_of(bots.read("units/RGBOT.fbi").bytes);
    OA_CHECK(bot.find("2222") != std::string::npos);
    OA_CHECK(bot.find("1111") == std::string::npos);
    int replaced_records = 0;
    for (const mutators::JournalEntry& entry : replaced.journal.entries) {
        if (entry.change != mutators::Change::added || entry.record != "RGBOT")
            continue;
        OA_CHECK(entry.old_text.has_value() && *entry.old_text == "RGBOT");
        ++replaced_records;
    }
    OA_CHECK(replaced_records == 2);
    OA_CHECK(count_kind(replaced, mutators::ConflictKind::value_replaced) == 2);
    const std::vector<std::string> bot_ids{"ridge-bot-a", "ridge-bot-b"};
    bool saw_game_bot = false;
    bool saw_pair_bot = false;
    for (const mutators::Conflict& conflict : replaced.report.conflicts) {
        if (conflict.kind != mutators::ConflictKind::value_replaced)
            continue;
        const std::string line = mutators::describe(conflict, bot_ids);
        if (line == "ridge-bot-a replaced RGBOT from the game on " + conflict.path)
            saw_game_bot = true;
        if (line == "ridge-bot-b replaced ridge-bot-a's RGBOT on " + conflict.path)
            saw_pair_bot = true;
    }
    OA_CHECK(saw_game_bot);
    OA_CHECK(saw_pair_bot);
    OA_CHECK(weapon_slot(bots, "RGGUN") == 1);
    release(bots);

    oa::AssetStore guns = open_ridge();
    const mutators::PipelineResult gun = mutators::run_pipeline(guns, input_of({"ridge-gun-body"}));
    OA_CHECK(gun.playable());
    OA_CHECK(weapon_slot(guns, "RGGUN") == 1);
    OA_CHECK(weapon_slot(guns, "RGBLAST") == 2);
    OA_CHECK(weapon_slot(guns, "RGWANT") == 3);
    const std::string text = text_of(guns.read("weapons/ridge.tdf").bytes);
    OA_CHECK(text.find("77") != std::string::npos);
    OA_CHECK(text.find("ID=99") == std::string::npos);
    release(guns);
}

/// Download sections from two mutators join the mod's own entry.
void test_download_sections_merge() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, input_of({"ridge-menu-a", "ridge-menu-b"}));
    OA_CHECK(result.playable());
    const std::string menu = text_of(store.read("download/rglab.tdf").bytes);
    OA_CHECK(menu.find("[MENU0]") != std::string::npos);
    OA_CHECK(menu.find("[MENU1]") == std::string::npos);
    OA_CHECK(menu.find("[MENU2]") != std::string::npos);
    OA_CHECK(menu.find("UNITNAME=RGBOT") != std::string::npos);
    OA_CHECK(menu.find("UNITNAME=RGCOM") != std::string::npos);
    release(store);
}

/// A rule in the same step sees the unit its own FBI just added.
void test_rule_sees_its_own_unit_file() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, input_of({"ridge-rule-new"}));
    OA_CHECK(result.playable());
    OA_CHECK(text_of(store.read("units/RGADDED.fbi").bytes).find("4242") != std::string::npos);
    release(store);
}

mutators::PipelineMutator scratch_mutator(const fs::path& package, std::string_view id) {
    const std::string yaml =
        "oamut: 1\nid: " + std::string{id} + "\nname: \"Ridge " + std::string{id} +
        "\"\nversion: \"1.0\"\nauthor:\n  name: \"Ridge Maker\"\ntags: [\"gameplay\"]\n"
        "works-with: [\"*\"]\nlayers: [\"files\"]\n";
    write_bytes(package / "oamut.yaml", yaml);
    write_bytes(
        package / "mutator.lua",
        "function mutate(ctx)\n  return { files = { add = { \"files\" } } }\nend\n"
    );
    return load_mutator(package);
}

/// Every id from 1 to 255 is taken, so the mutator that brings one more weapon
/// is refused by name and the base files stay as they were.
void test_full_weapon_table_refuses_the_mutator() {
    const fs::path scratch = oa::test::make_scratch_directory("V13-full");
    const fs::path copy = scratch / "ridge";
    fs::copy(k_data / "ridge", copy, fs::copy_options::recursive);
    std::string full;
    for (int id = 1; id <= 255; ++id) {
        full += "[BASE";
        full += std::to_string(id);
        full += "]\n{\nID=";
        full += std::to_string(id);
        full += ";\n}\n";
    }
    write_bytes(copy / "weapons" / "full.tdf", full);
    const fs::path package = scratch / "ridge-full";
    write_bytes(package / "files" / "weapons" / "extra.tdf", "[RGMORE]\n{\nID=1;\n}\n");
    std::vector<mutators::PipelineMutator> list;
    list.push_back(scratch_mutator(package, "ridge-full"));
    const std::vector<uint8_t> ridge_bytes = read_bytes(copy / "weapons" / "ridge.tdf");
    const std::vector<uint8_t> full_bytes = read_bytes(copy / "weapons" / "full.tdf");
    oa::AssetStore store(copy);
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.mutators.size() == 1);
    if (result.mutators.size() == 1) {
        OA_CHECK(result.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(result.mutators[0].phase == mutators::Phase::integrity);
        OA_CHECK(result.mutators[0].cause == mutators::Cause::weapon_ids);
        OA_CHECK(result.mutators[0].message.find("ridge-full") != std::string::npos);
        OA_CHECK(result.mutators[0].message.find("RGMORE") != std::string::npos);
    }
    OA_CHECK(!lists_name(store, "weapons", ".tdf", "extra.tdf"));
    OA_CHECK(store.read("weapons/ridge.tdf").bytes == ridge_bytes);
    OA_CHECK(store.read("weapons/full.tdf").bytes == full_bytes);
    release(store);
    fs::remove_all(scratch);
}

/// The earlier mutator's one weapon fits. The later mutator's weapons do not,
/// so that mutator is refused and the earlier weapon keeps its id.
void test_overflow_refuses_only_the_later_mutator() {
    const fs::path scratch = oa::test::make_scratch_directory("V13-overflow");
    const fs::path earlier = scratch / "ridge-keep";
    const fs::path later = scratch / "ridge-spill";
    write_bytes(earlier / "files" / "weapons" / "keep.tdf", "[RGKEEP]\n{\n}\n");
    write_bytes(
        earlier / "files" / "units" / "RGKEEP.fbi",
        "[UNITINFO]\n{\nUnitName=RGKEEP;\nWeapon1=RGKEEP;\n}\n"
    );
    write_bytes(earlier / "files" / "objects3d" / "RGKEEP.3DO", "3DO");
    std::string spill;
    for (int number = 0; number < 253; ++number) {
        std::string name = "B";
        if (number < 100)
            name.push_back('0');
        if (number < 10)
            name.push_back('0');
        name += std::to_string(number);
        spill += "[";
        spill += name;
        spill += "]\n{\n}\n";
    }
    write_bytes(later / "files" / "weapons" / "spill.tdf", spill);
    std::vector<mutators::PipelineMutator> list;
    list.push_back(scratch_mutator(earlier, "ridge-keep"));
    list.push_back(scratch_mutator(later, "ridge-spill"));
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.mutators.size() == 2);
    if (result.mutators.size() == 2) {
        OA_CHECK(result.mutators[0].outcome == mutators::Outcome::applied);
        OA_CHECK(result.mutators[1].outcome == mutators::Outcome::failed);
        OA_CHECK(result.mutators[1].phase == mutators::Phase::integrity);
        OA_CHECK(result.mutators[1].cause == mutators::Cause::weapon_ids);
        OA_CHECK(result.mutators[1].message.find("ridge-spill") != std::string::npos);
        OA_CHECK(result.mutators[1].message.find("B252") != std::string::npos);
    }
    OA_CHECK(weapon_slot(store, "RGGUN") == 1);
    OA_CHECK(weapon_slot(store, "RGBLAST") == 2);
    OA_CHECK(weapon_slot(store, "RGKEEP") == 3);
    OA_CHECK(
        text_of(store.read("units/RGKEEP.fbi").bytes).find("Weapon1=RGKEEP") != std::string::npos
    );
    OA_CHECK(!lists_name(store, "weapons", ".tdf", "spill.tdf"));
    release(store);
    fs::remove_all(scratch);
}

/// True when every ID= in `text` names a different number from 1 to 255.
bool weapon_ids_are_unique(std::string_view text) {
    std::vector<uint8_t> used(256, 0);
    std::size_t offset = 0;
    while (offset + 3 < text.size()) {
        const bool marker = (text[offset] == 'I' || text[offset] == 'i') &&
                            (text[offset + 1] == 'D' || text[offset + 1] == 'd') &&
                            text[offset + 2] == '=';
        if (!marker) {
            ++offset;
            continue;
        }
        offset += 3;
        int value = 0;
        bool digits = false;
        while (offset < text.size() && text[offset] >= '0' && text[offset] <= '9') {
            digits = true;
            value = value * 10 + static_cast<int>(text[offset] - '0');
            ++offset;
            if (value > 255)
                return false;
        }
        if (!digits || value < 1 || value > 255 || used[static_cast<std::size_t>(value)] != 0)
            return false;
        used[static_cast<std::size_t>(value)] = 1;
    }
    return true;
}

/// A rule or a hook that writes id on a base weapon is noted and not written.
void check_base_weapon_id_stays(std::string_view id) {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result = mutators::run_pipeline(store, input_of({id}));
    OA_CHECK(result.playable());
    OA_CHECK(result.mutators.size() == 1);
    if (result.mutators.size() == 1)
        OA_CHECK(result.mutators[0].outcome == mutators::Outcome::applied);
    OA_CHECK(weapon_slot(store, "RGGUN") == 1);
    OA_CHECK(weapon_slot(store, "RGBLAST") == 2);
    const std::string text = text_of(store.read("weapons/ridge.tdf").bytes);
    OA_CHECK(weapon_ids_are_unique(text));
    OA_CHECK(count_kind(result, mutators::ConflictKind::write_refused) == 1);
    const std::vector<std::string> ids{std::string{id}};
    const mutators::Conflict* refused = conflict_of(result, mutators::ConflictKind::write_refused);
    OA_CHECK(refused != nullptr);
    if (refused != nullptr) {
        const std::string line = mutators::describe(*refused, ids);
        OA_CHECK(line.find(id) != std::string::npos);
        OA_CHECK(line.find("refused") != std::string::npos);
    }
    for (const mutators::JournalEntry& entry : result.journal.entries)
        OA_CHECK(entry.field != "id");
    release(store);
}

void test_rule_cannot_renumber_a_base_weapon() {
    check_base_weapon_id_stays("ridge-id-rule");
}

void test_hook_cannot_renumber_a_base_weapon() {
    check_base_weapon_id_stays("ridge-id-hook");
}

/// A write of id on a weapon this mutator brought is that weapon's request.
void test_brought_weapon_keeps_its_requested_id() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, input_of({"ridge-id-own"}));
    OA_CHECK(result.playable());
    OA_CHECK(weapon_slot(store, "RGGUN") == 1);
    OA_CHECK(weapon_slot(store, "RGBLAST") == 2);
    OA_CHECK(weapon_slot(store, "RGOWN") == 9);
    OA_CHECK(weapon_ids_are_unique(text_of(store.read("weapons/ridge.tdf").bytes)));
    OA_CHECK(weapon_ids_are_unique(text_of(store.read("weapons/extra.tdf").bytes)));
    OA_CHECK(count_kind(result, mutators::ConflictKind::write_refused) == 0);
    release(store);
}

/// Ids 1 to 255 are already taken, so a record-file weapon is numbered 256.
void test_wide_limit_numbers_a_record_weapon_256() {
    const fs::path scratch = oa::test::make_scratch_directory("T05-wide-record");
    const fs::path copy = scratch / "ridge";
    fs::copy(k_data / "ridge", copy, fs::copy_options::recursive);
    std::string full;
    for (int id = 1; id <= 255; ++id) {
        full += "[BASE";
        full += std::to_string(id);
        full += "]\n{\nID=";
        full += std::to_string(id);
        full += ";\n}\n";
    }
    write_bytes(copy / "weapons" / "full.tdf", full);
    const fs::path package = scratch / "ridge-wide";
    write_bytes(package / "files" / "weapons" / "extra.tdf", "[RGMORE]\n{\n}\n");
    std::vector<mutators::PipelineMutator> list;
    list.push_back(scratch_mutator(package, "ridge-wide"));
    mutators::PipelineInput input = make_input(std::move(list));
    input.limits.weapon_types = 1024;
    oa::AssetStore store(copy);
    const mutators::PipelineResult result = mutators::run_pipeline(store, input);
    OA_CHECK(result.playable());
    OA_CHECK(result.mutators.size() == 1);
    if (result.mutators.size() == 1)
        OA_CHECK(result.mutators[0].outcome == mutators::Outcome::applied);
    OA_CHECK(text_of(store.read("weapons/extra.tdf").bytes).find("ID=256") != std::string::npos);
    release(store);
    fs::remove_all(scratch);
}

/// A rule or a hook package that brings RGWIDE and writes its id, shaped as
/// ridge-id-rule and ridge-id-hook do for a weapon the game already numbered.
mutators::PipelineMutator
brought_id_package(const fs::path& package, std::string_view id, bool hook) {
    const std::string yaml =
        "oamut: 1\nid: " + std::string{id} + "\nname: \"Ridge " + std::string{id} +
        "\"\nversion: \"1.0\"\nauthor:\n  name: \"Ridge Maker\"\ntags: [\"gameplay\"]\n"
        "works-with: [\"*\"]\nlayers: [\"files\", \"weapons\"]\n";
    write_bytes(package / "oamut.yaml", yaml);
    if (hook) {
        write_bytes(
            package / "mutator.lua",
            "function mutate(ctx)\n"
            "  return {\n"
            "    files = { add = { \"files\" } },\n"
            "    weapons = function(weapons, ctx)\n"
            "      weapons.RGWIDE.id = 300\n"
            "    end\n"
            "  }\n"
            "end\n"
        );
    } else {
        write_bytes(
            package / "mutator.lua",
            "function mutate(ctx)\n"
            "  return {\n"
            "    files = { add = { \"files\" } },\n"
            "    rules = { named(\"RGWIDE\"):on(\"weapons\", \"tdf\"):change { id = 300 } }\n"
            "  }\n"
            "end\n"
        );
    }
    write_bytes(package / "files" / "weapons" / "extra.tdf", "[RGWIDE]\n{\nRange=4;\n}\n");
    return load_mutator(package);
}

void expect_brought_id_300(
    const fs::path& package, std::string_view id, bool hook, uint32_t types
) {
    std::vector<mutators::PipelineMutator> list;
    list.push_back(brought_id_package(package, id, hook));
    mutators::PipelineInput input = make_input(std::move(list));
    input.limits.weapon_types = types;
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result = mutators::run_pipeline(store, input);
    if (types == 1024) {
        OA_CHECK(result.playable());
        OA_CHECK(result.mutators.size() == 1);
        if (result.mutators.size() == 1)
            OA_CHECK(result.mutators[0].outcome == mutators::Outcome::applied);
        OA_CHECK(count_kind(result, mutators::ConflictKind::write_refused) == 0);
        OA_CHECK(
            text_of(store.read("weapons/extra.tdf").bytes).find("ID=300") != std::string::npos
        );
    } else if (hook) {
        OA_CHECK(!result.playable());
        OA_CHECK(result.mutators.size() == 1);
        if (result.mutators.size() == 1) {
            OA_CHECK(result.mutators[0].outcome == mutators::Outcome::failed);
            OA_CHECK(result.mutators[0].message.find("255") != std::string::npos);
        }
    } else {
        OA_CHECK(result.playable());
        OA_CHECK(count_kind(result, mutators::ConflictKind::write_refused) == 1);
        const std::vector<std::string> ids{std::string{id}};
        const mutators::Conflict* refused =
            conflict_of(result, mutators::ConflictKind::write_refused);
        OA_CHECK(refused != nullptr);
        if (refused != nullptr)
            OA_CHECK(mutators::describe(*refused, ids).find("255") != std::string::npos);
        OA_CHECK(
            text_of(store.read("weapons/extra.tdf").bytes).find("ID=300") == std::string::npos
        );
    }
    release(store);
}

/// id = 300 on a weapon the mutator brought is kept at 1,024 types and
/// refused at 256, and that refusal names 255.
void test_brought_weapon_id_300_follows_the_limit() {
    const fs::path scratch = oa::test::make_scratch_directory("T05-id-300");
    expect_brought_id_300(scratch / "rule-wide", "ridge-id-wide-rule", false, 1024);
    expect_brought_id_300(scratch / "rule-base", "ridge-id-wide-rule", false, 256);
    expect_brought_id_300(scratch / "hook-wide", "ridge-id-wide-hook", true, 1024);
    expect_brought_id_300(scratch / "hook-base", "ridge-id-wide-hook", true, 256);
    fs::remove_all(scratch);
}

/// `count` sections named with `prefix` and `digits` digits. A request uses
/// ID = index + 3, which is free once the base weapons hold 1 and 2.
std::string numbered_weapons(char prefix, int count, int digits, bool request) {
    std::string text;
    for (int number = 0; number < count; ++number) {
        std::string name(1, prefix);
        std::string digits_text = std::to_string(number);
        while (static_cast<int>(digits_text.size()) < digits)
            digits_text.insert(digits_text.begin(), '0');
        name += digits_text;
        text += "[";
        text += name;
        text += "]\n{\n";
        if (request) {
            text += "ID=";
            text += std::to_string(number + 3);
            text += ";\n";
        }
        text += "}\n";
    }
    return text;
}

/// Ten unrequested weapons and 245 requests. In either list order the later
/// mutator is the one refused, and the base weapons keep their ids.
void test_request_overflow_follows_list_order() {
    const fs::path scratch = oa::test::make_scratch_directory("V13-requests");
    const fs::path earlier = scratch / "ridge-early";
    const fs::path later = scratch / "ridge-late";
    write_bytes(earlier / "files" / "weapons" / "early.tdf", numbered_weapons('E', 10, 3, false));
    write_bytes(later / "files" / "weapons" / "late.tdf", numbered_weapons('L', 245, 3, true));
    scratch_mutator(earlier, "ridge-early");
    scratch_mutator(later, "ridge-late");

    oa::AssetStore early_store = open_ridge();
    std::vector<mutators::PipelineMutator> early_first;
    early_first.push_back(load_mutator(earlier));
    early_first.push_back(load_mutator(later));
    const mutators::PipelineResult early_run =
        mutators::run_pipeline(early_store, make_input(std::move(early_first)));
    OA_CHECK(early_run.mutators.size() == 2);
    if (early_run.mutators.size() == 2) {
        OA_CHECK(early_run.mutators[0].outcome == mutators::Outcome::applied);
        OA_CHECK(early_run.mutators[1].outcome == mutators::Outcome::failed);
        OA_CHECK(early_run.mutators[1].phase == mutators::Phase::integrity);
        OA_CHECK(early_run.mutators[1].cause == mutators::Cause::weapon_ids);
        OA_CHECK(early_run.mutators[1].message.find("ridge-late") != std::string::npos);
        OA_CHECK(early_run.mutators[1].message.find("L008") != std::string::npos);
    }
    OA_CHECK(weapon_slot(early_store, "RGGUN") == 1);
    OA_CHECK(weapon_slot(early_store, "RGBLAST") == 2);
    OA_CHECK(weapon_slot(early_store, "E000") == 3);
    OA_CHECK(lists_name(early_store, "weapons", ".tdf", "early.tdf"));
    OA_CHECK(!lists_name(early_store, "weapons", ".tdf", "late.tdf"));
    release(early_store);

    oa::AssetStore late_store = open_ridge();
    std::vector<mutators::PipelineMutator> late_first;
    late_first.push_back(load_mutator(later));
    late_first.push_back(load_mutator(earlier));
    const mutators::PipelineResult late_run =
        mutators::run_pipeline(late_store, make_input(std::move(late_first)));
    OA_CHECK(late_run.mutators.size() == 2);
    if (late_run.mutators.size() == 2) {
        OA_CHECK(late_run.mutators[0].outcome == mutators::Outcome::applied);
        OA_CHECK(late_run.mutators[1].outcome == mutators::Outcome::failed);
        OA_CHECK(late_run.mutators[1].phase == mutators::Phase::integrity);
        OA_CHECK(late_run.mutators[1].cause == mutators::Cause::weapon_ids);
        OA_CHECK(late_run.mutators[1].message.find("ridge-early") != std::string::npos);
        OA_CHECK(late_run.mutators[1].message.find("E008") != std::string::npos);
    }
    OA_CHECK(weapon_slot(late_store, "RGGUN") == 1);
    OA_CHECK(weapon_slot(late_store, "RGBLAST") == 2);
    OA_CHECK(weapon_slot(late_store, "L000") == 3);
    OA_CHECK(weapon_slot(late_store, "L244") == 247);
    OA_CHECK(lists_name(late_store, "weapons", ".tdf", "late.tdf"));
    OA_CHECK(!lists_name(late_store, "weapons", ".tdf", "early.tdf"));
    release(late_store);
    fs::remove_all(scratch);
}

mutators::PipelineMutator make_package(
    const fs::path& root,
    std::string_view id,
    std::string_view clashes,
    std::string_view layers,
    std::string_view lua,
    const std::vector<std::pair<std::string, std::string>>& files
);

/// A later unit file brings back a unit a hook removed, and a later download
/// file brings back a download document that removal emptied.
void test_removed_record_file_comes_back() {
    oa::AssetStore bots = open_ridge();
    const mutators::PipelineResult revived =
        mutators::run_pipeline(bots, input_of({"ridge-remove-bot", "ridge-bot-a"}));
    OA_CHECK(revived.playable());
    OA_CHECK(revived.mutators.size() == 2);
    if (revived.mutators.size() == 2) {
        OA_CHECK(revived.mutators[0].outcome == mutators::Outcome::applied);
        OA_CHECK(revived.mutators[1].outcome == mutators::Outcome::applied);
    }
    const std::string bot = text_of(bots.read("units/RGBOT.fbi").bytes);
    OA_CHECK(bot.find("1111") != std::string::npos);
    OA_CHECK(lists_name(bots, "units", ".fbi", "rgbot.fbi"));
    release(bots);

    const fs::path scratch = oa::test::make_scratch_directory("V04-menu-back");
    std::vector<std::pair<std::string, std::string>> files;
    files.emplace_back("download/rglab.tdf", "[MENU1]\n{\nUNITMENU=RGLAB;\nUNITNAME=RGCOM;\n}\n");
    std::vector<mutators::PipelineMutator> revived_menu;
    revived_menu.push_back(load_id("ridge-remove-bot"));
    revived_menu.push_back(make_package(
        scratch,
        "ridge-menu-back",
        "",
        "\"files\"",
        "function mutate(ctx)\n  return { files = { add = { \"files\" } } }\nend\n",
        files
    ));
    oa::AssetStore menus = open_ridge();
    const mutators::PipelineResult menu =
        mutators::run_pipeline(menus, make_input(std::move(revived_menu)));
    OA_CHECK(menu.playable());
    OA_CHECK(menu.mutators.size() == 2);
    if (menu.mutators.size() == 2) {
        OA_CHECK(menu.mutators[0].outcome == mutators::Outcome::applied);
        OA_CHECK(menu.mutators[1].outcome == mutators::Outcome::applied);
    }
    const std::string text = text_of(menus.read("download/rglab.tdf").bytes);
    OA_CHECK(text.find("MENU1") != std::string::npos);
    OA_CHECK(text.find("MENU0") == std::string::npos);
    release(menus);

    oa::AssetStore named = open_ridge();
    const mutators::PipelineResult broken =
        mutators::run_pipeline(named, input_of({"ridge-remove-bot", "ridge-menu-a"}));
    OA_CHECK(broken.mutators.size() == 2);
    if (broken.mutators.size() == 2) {
        OA_CHECK(broken.mutators[0].outcome == mutators::Outcome::applied);
        OA_CHECK(broken.mutators[1].outcome == mutators::Outcome::failed);
        OA_CHECK(broken.mutators[1].phase == mutators::Phase::integrity);
        OA_CHECK(broken.mutators[1].cause == mutators::Cause::integrity);
        OA_CHECK(broken.mutators[1].message.find("RGBOT") != std::string::npos);
        if (broken.mutators[1].outcome != mutators::Outcome::failed)
            std::fprintf(stderr, "menu-a: %s\n", broken.mutators[1].message.c_str());
    }
    release(named);
    fs::remove_all(scratch);
}

/// A weapon file brings back a weapon document a hook had emptied.
void test_removed_weapon_file_comes_back() {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, input_of({"ridge-clear-guns", "ridge-revive-gun"}));
    OA_CHECK(result.playable());
    OA_CHECK(result.mutators.size() == 2);
    if (result.mutators.size() == 2) {
        OA_CHECK(result.mutators[0].outcome == mutators::Outcome::applied);
        OA_CHECK(result.mutators[1].outcome == mutators::Outcome::applied);
    }
    OA_CHECK(weapon_slot(store, "RGREVIVE") == 4);
    OA_CHECK(weapon_slot(store, "RGGUN") == -1);
    OA_CHECK(weapon_slot(store, "RGBLAST") == -1);
    const std::string text = text_of(store.read("weapons/ridge.tdf").bytes);
    OA_CHECK(text.find("RGREVIVE") != std::string::npos);
    OA_CHECK(text.find("RGGUN") == std::string::npos);
    OA_CHECK(weapon_ids_are_unique(text));
    release(store);
}

/// One listed mutator, version 1.0, with nothing required yet.
mutators::PipelineMutator listed_mutator(std::string id) {
    mutators::PipelineMutator one;
    one.manifest.id = std::move(id);
    one.manifest.version = oamut::Version{1, 0, 0, 1};
    return one;
}

/// A requirement on `id` at any version.
oamut::Condition requires_id(std::string id) {
    return oamut::Condition{std::move(id), {}, std::nullopt};
}

/// A mutator that failed because its hacks were refused.
mutators::MutatorResult hack_refused(std::string id) {
    mutators::MutatorResult one;
    one.id = std::move(id);
    one.outcome = mutators::Outcome::failed;
    one.cause = mutators::Cause::hack_refused;
    one.phase = mutators::Phase::plan;
    one.message = "hacks: a mutator cannot set this hack";
    return one;
}

/// A mutator excluded because something it requires did not apply.
mutators::MutatorResult excluded_for(std::string id) {
    mutators::MutatorResult one;
    one.id = std::move(id);
    one.outcome = mutators::Outcome::excluded;
    one.cause = mutators::Cause::prerequisite_failed;
    one.phase = mutators::Phase::loadout;
    return one;
}

/// A hook that stopped with an error.
mutators::MutatorResult hook_error(std::string id) {
    mutators::MutatorResult one;
    one.id = std::move(id);
    one.outcome = mutators::Outcome::failed;
    one.cause = mutators::Cause::script_error;
    one.phase = mutators::Phase::hooks;
    one.message = "the hook stopped";
    return one;
}

/// A hack refusal fails that mutator alone. Another failure stops the start.
void test_first_blocking() {
    mutators::PipelineMutator base = listed_mutator("base");
    mutators::PipelineMutator needs = listed_mutator("needs");
    needs.manifest.requires_mutators.push_back(requires_id("base"));
    const mutators::PipelineMutator listed[] = {base, needs};

    mutators::PipelineResult result;
    result.mutators = {hack_refused("base"), excluded_for("needs")};
    OA_CHECK(!mutators::first_blocking(result, listed).has_value());

    result.mutators[0] = hook_error("base");
    const std::optional<std::size_t> script = mutators::first_blocking(result, listed);
    OA_CHECK(script.has_value() && *script == 0);
    OA_CHECK(result.mutators[*script].id == "base");
    OA_CHECK(result.mutators[*script].message == "the hook stopped");

    mutators::MutatorResult applied;
    applied.id = "base";
    applied.outcome = mutators::Outcome::applied;
    result.refusal = "base is listed twice";
    result.mutators = {applied};
    OA_CHECK(!mutators::first_blocking(result, std::span{listed, 1}).has_value());

    mutators::PipelineMutator looped = needs;
    looped.manifest.requires_mutators = {requires_id("needs")};
    base.manifest.requires_mutators = {requires_id("needs")};
    const mutators::PipelineMutator cycle[] = {base, looped};
    result.refusal.clear();
    result.mutators = {excluded_for("base"), excluded_for("needs")};
    const std::optional<std::size_t> loop = mutators::first_blocking(result, cycle);
    OA_CHECK(loop.has_value() && *loop == 0);

    // Two levels: the top hack was refused, and each mutator below requires
    // the one above. None of them stops the start.
    mutators::PipelineMutator mid = listed_mutator("mid");
    mid.manifest.requires_mutators.push_back(requires_id("base"));
    mutators::PipelineMutator tail = listed_mutator("tail");
    tail.manifest.requires_mutators.push_back(requires_id("mid"));
    const mutators::PipelineMutator chain[] = {listed_mutator("base"), mid, tail};
    result.mutators = {hack_refused("base"), excluded_for("mid"), excluded_for("tail")};
    OA_CHECK(!mutators::first_blocking(result, chain).has_value());

    // A diamond: both middle mutators require the refused hack, and the
    // bottom requires both of them. The whole diamond fails alone.
    mutators::PipelineMutator left = listed_mutator("left");
    left.manifest.requires_mutators.push_back(requires_id("base"));
    mutators::PipelineMutator right = listed_mutator("right");
    right.manifest.requires_mutators.push_back(requires_id("base"));
    mutators::PipelineMutator both = listed_mutator("both");
    both.manifest.requires_mutators.push_back(requires_id("left"));
    both.manifest.requires_mutators.push_back(requires_id("right"));
    const mutators::PipelineMutator diamond[] = {listed_mutator("base"), left, right, both};
    result.mutators = {
        hack_refused("base"), excluded_for("left"), excluded_for("right"), excluded_for("both")
    };
    OA_CHECK(!mutators::first_blocking(result, diamond).has_value());

    // A refused hack above a hook error. The hook error stops the start and
    // is the slot a failure message names.
    const mutators::PipelineMutator hook_list[] = {listed_mutator("warp"), listed_mutator("boom")};
    result.mutators = {hack_refused("warp"), hook_error("boom")};
    const std::optional<std::size_t> hook = mutators::first_blocking(result, hook_list);
    OA_CHECK(hook.has_value() && *hook == 1);
    OA_CHECK(result.mutators[*hook].id == "boom");
    OA_CHECK(result.mutators[*hook].cause == mutators::Cause::script_error);

    // The dependent requires the refused hack and the hook error. The hook
    // error is the second requirement, so it stops the start and is named.
    mutators::PipelineMutator dependent = listed_mutator("needs-both");
    dependent.manifest.requires_mutators.push_back(requires_id("warp"));
    dependent.manifest.requires_mutators.push_back(requires_id("boom"));
    const mutators::PipelineMutator mixed[] = {
        listed_mutator("warp"), listed_mutator("boom"), dependent
    };
    result.mutators = {hack_refused("warp"), hook_error("boom"), excluded_for("needs-both")};
    const std::optional<std::size_t> mixed_stop = mutators::first_blocking(result, mixed);
    OA_CHECK(mixed_stop.has_value() && *mixed_stop == 1);
    OA_CHECK(result.mutators[*mixed_stop].id == "boom");
    OA_CHECK(result.mutators[*mixed_stop].message == "the hook stopped");
}

/// True when `key=value` occurs and the next character is not part of a name.
bool has_assignment(std::string_view text, std::string_view key, std::string_view value) {
    const std::string needle = std::string{key} + "=" + std::string{value};
    std::size_t offset = 0;
    while (offset < text.size()) {
        const std::size_t found = text.find(needle, offset);
        if (found == std::string_view::npos)
            return false;
        const std::size_t after = found + needle.size();
        if (after >= text.size())
            return true;
        const unsigned char next = static_cast<unsigned char>(text[after]);
        const bool name = (next >= 'A' && next <= 'Z') || (next >= 'a' && next <= 'z') ||
                          (next >= '0' && next <= '9') || next == '_';
        if (!name)
            return true;
        offset = after;
    }
    return false;
}

/// Prints a failed mutator's message so a refusal names itself.
void note_failures(const mutators::PipelineResult& result) {
    if (!result.refusal.empty())
        std::fprintf(stderr, "refusal: %s\n", result.refusal.c_str());
    for (const mutators::MutatorResult& one : result.mutators) {
        if (one.outcome == mutators::Outcome::applied || one.message.empty())
            continue;
        std::fprintf(stderr, "%s: %s\n", one.id.c_str(), one.message.c_str());
    }
}

const mutators::JournalEntry* entry_named(
    const mutators::PipelineResult& result, mutators::Collection collection, std::string_view record
) {
    for (const mutators::JournalEntry& entry : result.journal.entries) {
        if (entry.collection == collection && entry.record == record)
            return &entry;
    }
    return nullptr;
}

/// Folds ASCII letters. Paths compared inside `make_package` use it once.
std::string folded_key(std::string_view text) {
    std::string out(text.size(), '\0');
    for (std::size_t byte = 0; byte < text.size(); ++byte) {
        const char raw = text[byte];
        out[byte] = raw >= 'A' && raw <= 'Z' ? static_cast<char>(raw - 'A' + 'a') : raw;
    }
    return out;
}

/// Writes one scratch package and loads it.
///
/// An empty `clashes` leaves the manifest's clashes unset. `layers` is the
/// text between the brackets, quotes included.
mutators::PipelineMutator make_package(
    const fs::path& root,
    std::string_view id,
    std::string_view clashes,
    std::string_view layers,
    std::string_view lua,
    const std::vector<std::pair<std::string, std::string>>& files
) {
    const fs::path package = root / std::string{id};
    std::string yaml = "oamut: 1\nid: " + std::string{id} + "\nname: \"Ridge " + std::string{id} +
                       "\"\nversion: \"1.0\"\nauthor:\n  name: \"Ridge Maker\"\n"
                       "tags: [\"gameplay\"]\nworks-with: [\"*\"]\n";
    if (!clashes.empty()) {
        yaml += "clashes: \"";
        yaml += clashes;
        yaml += "\"\n";
    }
    yaml += "layers: [";
    yaml += layers;
    yaml += "]\n";
    write_bytes(package / "oamut.yaml", yaml);
    write_bytes(package / "mutator.lua", lua);
    // A unit this package adds needs a model, or the integrity check fails it.
    // The caller’s own model file is left as written.
    std::vector<std::pair<std::string, std::string>> stored = files;
    const std::size_t given = stored.size();
    for (std::size_t index = 0; index < given; ++index) {
        const std::string& relative = stored[index].first;
        const std::string folded = folded_key(relative);
        if (folded.rfind("units/", 0) != 0 || folded.size() < 10 ||
            folded.compare(folded.size() - 4, 4, ".fbi") != 0)
            continue;
        const std::string stem = relative.substr(6, relative.size() - 10);
        const std::string model = "objects3d/" + stem + ".3DO";
        const std::string model_folded = folded_key(model);
        bool present = false;
        for (const std::pair<std::string, std::string>& other : stored) {
            if (folded_key(other.first) == model_folded)
                present = true;
        }
        if (!present)
            stored.emplace_back(model, "3DO");
    }
    for (const std::pair<std::string, std::string>& file : stored)
        write_bytes(package / "files" / file.first, file.second);
    return load_mutator(package);
}

constexpr std::string_view k_files_lua = "function mutate(ctx)\n"
                                         "  return { files = { add = { \"files\" } } }\n"
                                         "end\n";

std::string spark_weapon(std::string_view range) {
    std::string text = "[SPARK]\n{\nRange=";
    text.append(range);
    text.append(";\n}\n");
    return text;
}

std::string spark_unit(std::string_view name) {
    std::string text = "[UNITINFO]\n{\nUnitName=";
    text.append(name);
    text.append(";\nWeapon1=SPARK;\n}\n");
    return text;
}

/// Add gives the later weapon a private name, and only that mutator's unit
/// fires it. The earlier unit keeps the weapon it named.
void test_add_gives_each_unit_its_own_weapon() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-spark-add");
    std::vector<mutators::PipelineMutator> list;
    for (const std::string_view id : {"ridge-spark-a", "ridge-spark-b"}) {
        const bool first = id == "ridge-spark-a";
        const std::string unit = first ? "SPARKA" : "SPARKB";
        const std::string range = first ? "10" : "20";
        std::vector<std::pair<std::string, std::string>> files;
        files.emplace_back("weapons/spark.tdf", spark_weapon(range));
        files.emplace_back("units/" + unit + ".fbi", spark_unit(unit));
        list.push_back(make_package(scratch, id, "add", "\"files\"", k_files_lua, files));
    }
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    if (!result.playable()) {
        note_failures(result);
        release(store);
        fs::remove_all(scratch);
        return;
    }
    const std::string weapons = text_of(store.read("weapons/spark.tdf").bytes);
    const std::string first_unit = text_of(store.read("units/SPARKA.fbi").bytes);
    const std::string second_unit = text_of(store.read("units/SPARKB.fbi").bytes);
    const std::size_t spark = weapons.find("[SPARK]");
    const std::size_t spark_two = weapons.find("[SPARK_2]");
    OA_CHECK(spark != std::string::npos);
    OA_CHECK(spark_two != std::string::npos);
    OA_CHECK(spark < spark_two);
    const std::size_t range_ten = weapons.find("Range=10");
    const std::size_t range_twenty = weapons.find("Range=20");
    OA_CHECK(range_ten != std::string::npos && range_ten < spark_two);
    OA_CHECK(range_twenty != std::string::npos && range_twenty > spark_two);
    OA_CHECK(has_assignment(first_unit, "Weapon1", "SPARK"));
    OA_CHECK(first_unit.find("SPARK_2") == std::string::npos);
    OA_CHECK(has_assignment(second_unit, "Weapon1", "SPARK_2"));
    const int first_slot = weapon_slot(store, "SPARK");
    const int second_slot = weapon_slot(store, "SPARK_2");
    OA_CHECK(first_slot > 0);
    OA_CHECK(second_slot > 0);
    OA_CHECK(first_slot != second_slot);
    const std::vector<std::string> ids{"ridge-spark-a", "ridge-spark-b"};
    const mutators::Conflict* renamed = conflict_of(result, mutators::ConflictKind::record_renamed);
    OA_CHECK(renamed != nullptr);
    if (renamed != nullptr) {
        OA_CHECK(mutators::describe(*renamed, ids) == "ridge-spark-b renamed SPARK to SPARK_2");
    }
    release(store);
    fs::remove_all(scratch);
}

/// Replace writes the later weapon over the earlier one. Both units fire it.
void test_replace_makes_both_units_fire_the_later_weapon() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-spark-replace");
    std::vector<mutators::PipelineMutator> list;
    for (const std::string_view id : {"ridge-spark-a", "ridge-spark-b"}) {
        const bool first = id == "ridge-spark-a";
        const std::string unit = first ? "SPARKA" : "SPARKB";
        const std::string range = first ? "10" : "20";
        std::vector<std::pair<std::string, std::string>> files;
        files.emplace_back("weapons/spark.tdf", spark_weapon(range));
        files.emplace_back("units/" + unit + ".fbi", spark_unit(unit));
        list.push_back(make_package(scratch, id, "replace", "\"files\"", k_files_lua, files));
    }
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    if (!result.playable()) {
        note_failures(result);
        release(store);
        fs::remove_all(scratch);
        return;
    }
    const std::string weapons = text_of(store.read("weapons/spark.tdf").bytes);
    const std::string first_unit = text_of(store.read("units/SPARKA.fbi").bytes);
    const std::string second_unit = text_of(store.read("units/SPARKB.fbi").bytes);
    OA_CHECK(weapons.find("[SPARK]") != std::string::npos);
    OA_CHECK(weapons.find("[SPARK_2]") == std::string::npos);
    OA_CHECK(weapons.find("Range=20") != std::string::npos);
    OA_CHECK(weapons.find("Range=10") == std::string::npos);
    OA_CHECK(has_assignment(first_unit, "Weapon1", "SPARK"));
    OA_CHECK(has_assignment(second_unit, "Weapon1", "SPARK"));
    OA_CHECK(second_unit.find("SPARK_2") == std::string::npos);
    OA_CHECK(weapon_slot(store, "SPARK") > 0);
    OA_CHECK(weapon_slot(store, "SPARK_2") == -1);
    OA_CHECK(count_kind(result, mutators::ConflictKind::value_replaced) == 1);
    const std::vector<std::string> ids{"ridge-spark-a", "ridge-spark-b"};
    const mutators::Conflict* note = conflict_of(result, mutators::ConflictKind::value_replaced);
    OA_CHECK(note != nullptr);
    if (note != nullptr) {
        const std::string line = mutators::describe(*note, ids);
        OA_CHECK(line == "ridge-spark-b replaced ridge-spark-a's SPARK on " + note->path);
    }
    release(store);
    fs::remove_all(scratch);
}

/// Add shares a weapon that matches the one already there. Neither unit is
/// pointed at a private name.
void test_add_shares_an_identical_weapon() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-spark-share");
    std::vector<mutators::PipelineMutator> list;
    for (const std::string_view id : {"ridge-spark-a", "ridge-spark-b"}) {
        const bool first = id == "ridge-spark-a";
        const std::string unit = first ? "SPARKA" : "SPARKB";
        std::vector<std::pair<std::string, std::string>> files;
        files.emplace_back("weapons/spark.tdf", spark_weapon("10"));
        files.emplace_back("units/" + unit + ".fbi", spark_unit(unit));
        list.push_back(make_package(scratch, id, "add", "\"files\"", k_files_lua, files));
    }
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    if (!result.playable()) {
        note_failures(result);
        release(store);
        fs::remove_all(scratch);
        return;
    }
    const std::string weapons = text_of(store.read("weapons/spark.tdf").bytes);
    OA_CHECK(weapons.find("[SPARK]") != std::string::npos);
    OA_CHECK(weapons.find("[SPARK_2]") == std::string::npos);
    OA_CHECK(weapons.find("Range=10") != std::string::npos);
    OA_CHECK(has_assignment(text_of(store.read("units/SPARKA.fbi").bytes), "Weapon1", "SPARK"));
    OA_CHECK(has_assignment(text_of(store.read("units/SPARKB.fbi").bytes), "Weapon1", "SPARK"));
    OA_CHECK(count_kind(result, mutators::ConflictKind::record_shared) == 1);
    OA_CHECK(count_kind(result, mutators::ConflictKind::record_renamed) == 0);
    const std::vector<std::string> ids{"ridge-spark-a", "ridge-spark-b"};
    const mutators::Conflict* shared = conflict_of(result, mutators::ConflictKind::record_shared);
    OA_CHECK(shared != nullptr);
    if (shared != nullptr) {
        OA_CHECK(
            mutators::describe(*shared, ids) == "ridge-spark-b shares SPARK with ridge-spark-a"
        );
    }
    release(store);
    fs::remove_all(scratch);
}

/// Add stores a different RGBOT under a private name. Its display name stays,
/// and the same mutator's menu button, build list and clone follow the
/// private name. The unit already in the game is left as it was.
void test_renamed_unit_keeps_menu_side_and_clone() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-link");
    constexpr std::string_view lua = "function mutate(ctx)\n"
                                     "  return {\n"
                                     "    files = { add = { \"files\" } },\n"
                                     "    units = function(units, ctx)\n"
                                     "      ctx.sides:add_canbuild(\"RGLAB\", \"RGBOT\")\n"
                                     "      units:clone(\"RGBOT\", \"RGCOPY\", {})\n"
                                     "    end\n"
                                     "  }\n"
                                     "end\n";
    std::vector<std::pair<std::string, std::string>> files;
    files.emplace_back(
        "units/RGBOT.fbi",
        "[UNITINFO]\n{\nUnitName=RGBOT;\nName=Ridge Bot;\nMaxDamage=999;\nWeapon1=RGGUN;\n}\n"
    );
    files.emplace_back("download/rglab.tdf", "[MENU1]\n{\nUNITMENU=RGLAB;\nUNITNAME=RGBOT;\n}\n");
    std::vector<mutators::PipelineMutator> list;
    list.push_back(
        make_package(scratch, "ridge-link", "add", "\"files\", \"units\", \"sides\"", lua, files)
    );
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    if (!result.playable()) {
        note_failures(result);
        release(store);
        fs::remove_all(scratch);
        return;
    }
    const mutators::JournalEntry* renamed =
        entry_named(result, mutators::Collection::units, "RGBOT_2");
    OA_CHECK(renamed != nullptr);
    if (renamed != nullptr) {
        const std::string body = text_of(store.read(renamed->path).bytes);
        OA_CHECK(has_assignment(body, "UnitName", "RGBOT_2"));
        OA_CHECK(body.find("Name=Ridge Bot") != std::string::npos);
        OA_CHECK(body.find("999") != std::string::npos);
    }
    const std::string base = text_of(store.read("units/RGBOT.fbi").bytes);
    OA_CHECK(has_assignment(base, "Weapon1", "RGGUN"));
    OA_CHECK(base.find("Ridge Bot") == std::string::npos);
    OA_CHECK(base.find("999") == std::string::npos);
    const std::string menu = text_of(store.read("download/rglab.tdf").bytes);
    OA_CHECK(menu.find("[MENU0]") != std::string::npos);
    OA_CHECK(menu.find("[MENU1]") != std::string::npos);
    OA_CHECK(has_assignment(menu, "UNITNAME", "RGBOT"));
    OA_CHECK(has_assignment(menu, "UNITNAME", "RGBOT_2"));
    const std::string sides = text_of(store.read("gamedata/sidedata.tdf").bytes);
    OA_CHECK(has_assignment(sides, "canbuild1", "RGBOT_2"));
    OA_CHECK(!has_assignment(sides, "canbuild1", "RGBOT"));
    const mutators::JournalEntry* clone =
        entry_named(result, mutators::Collection::units, "RGCOPY");
    OA_CHECK(clone != nullptr);
    if (clone != nullptr) {
        const std::string body = text_of(store.read(clone->path).bytes);
        OA_CHECK(body.find("999") != std::string::npos);
        OA_CHECK(body.find("Name=Ridge Bot") != std::string::npos);
        OA_CHECK(has_assignment(body, "UnitName", "RGCOPY"));
        OA_CHECK(clone->new_text.has_value());
        if (clone->new_text.has_value())
            OA_CHECK(*clone->new_text == "added unit RGCOPY (copy of RGBOT)");
    }
    const std::vector<std::string> ids{"ridge-link"};
    const mutators::Conflict* note = conflict_of(result, mutators::ConflictKind::record_renamed);
    OA_CHECK(note != nullptr);
    if (note != nullptr)
        OA_CHECK(mutators::describe(*note, ids) == "ridge-link renamed RGBOT to RGBOT_2");
    release(store);
    fs::remove_all(scratch);
}

/// Two download entries can share a section name when they name different
/// units. The later entry takes a private section name and both units stay.
void test_download_entries_match_builder_and_unit() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-menus");
    std::vector<mutators::PipelineMutator> list;
    for (const std::string_view id : {"ridge-entry-a", "ridge-entry-b"}) {
        const bool first = id == "ridge-entry-a";
        const std::string unit = first ? "RGONE" : "RGTWO";
        std::vector<std::pair<std::string, std::string>> files;
        files.emplace_back("units/" + unit + ".fbi", "[UNITINFO]\n{\nUnitName=" + unit + ";\n}\n");
        files.emplace_back(
            "download/lab.tdf", "[MENUENTRY1]\n{\nUNITMENU=RGLAB;\nUNITNAME=" + unit + ";\n}\n"
        );
        list.push_back(make_package(scratch, id, "", "\"files\"", k_files_lua, files));
    }
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    if (!result.playable()) {
        note_failures(result);
        release(store);
        fs::remove_all(scratch);
        return;
    }
    const std::string menu = text_of(store.read("download/lab.tdf").bytes);
    OA_CHECK(menu.find("[MENUENTRY1]") != std::string::npos);
    OA_CHECK(menu.find("[MENUENTRY1_2]") != std::string::npos);
    OA_CHECK(has_assignment(menu, "UNITNAME", "RGONE"));
    OA_CHECK(has_assignment(menu, "UNITNAME", "RGTWO"));
    OA_CHECK(has_assignment(text_of(store.read("units/RGONE.fbi").bytes), "UnitName", "RGONE"));
    OA_CHECK(has_assignment(text_of(store.read("units/RGTWO.fbi").bytes), "UnitName", "RGTWO"));
    const std::vector<std::string> ids{"ridge-entry-a", "ridge-entry-b"};
    const mutators::Conflict* renamed = conflict_of(result, mutators::ConflictKind::record_renamed);
    OA_CHECK(renamed != nullptr);
    if (renamed != nullptr) {
        OA_CHECK(
            mutators::describe(*renamed, ids) == "ridge-entry-b renamed MENUENTRY1 to MENUENTRY1_2"
        );
    }
    release(store);
    fs::remove_all(scratch);
}

bool bot_was_renamed(const mutators::PipelineResult& result) {
    return entry_named(result, mutators::Collection::units, "RGBOT_2") != nullptr;
}

/// The player's choice wins, then the manifest, then the preference.
void test_effective_mode_follows_player_manifest_setting() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-mode");
    constexpr std::string_view bot =
        "[UNITINFO]\n{\nUnitName=RGBOT;\nName=Ridge Bot;\nMaxDamage=999;\nWeapon1=RGGUN;\n}\n";

    oa::AssetStore replaced = open_ridge();
    {
        std::vector<std::pair<std::string, std::string>> files;
        files.emplace_back("units/RGBOT.fbi", std::string{bot});
        mutators::PipelineMutator mutator =
            make_package(scratch, "ridge-mode", "add", "\"files\"", k_files_lua, files);
        mutator.clashes = oamut::ClashMode::replace;
        std::vector<mutators::PipelineMutator> list;
        list.push_back(std::move(mutator));
        mutators::PipelineInput input = make_input(std::move(list));
        input.clash_default = oamut::ClashMode::add;
        const mutators::PipelineResult result = mutators::run_pipeline(replaced, std::move(input));
        OA_CHECK(result.playable());
        if (!result.playable())
            note_failures(result);
        OA_CHECK(!bot_was_renamed(result));
        const std::string body = text_of(replaced.read("units/RGBOT.fbi").bytes);
        OA_CHECK(body.find("999") != std::string::npos);
        OA_CHECK(body.find("Name=Ridge Bot") != std::string::npos);
        OA_CHECK(has_assignment(body, "UnitName", "RGBOT"));
    }
    release(replaced);

    oa::AssetStore manifest = open_ridge();
    {
        std::vector<std::pair<std::string, std::string>> files;
        files.emplace_back("units/RGBOT.fbi", std::string{bot});
        std::vector<mutators::PipelineMutator> list;
        list.push_back(make_package(scratch, "ridge-mode", "add", "\"files\"", k_files_lua, files));
        mutators::PipelineInput input = make_input(std::move(list));
        input.clash_default = oamut::ClashMode::replace;
        const mutators::PipelineResult result = mutators::run_pipeline(manifest, std::move(input));
        OA_CHECK(result.playable());
        if (!result.playable())
            note_failures(result);
        OA_CHECK(bot_was_renamed(result));
        const std::string base = text_of(manifest.read("units/RGBOT.fbi").bytes);
        OA_CHECK(base.find("999") == std::string::npos);
        const mutators::JournalEntry* renamed =
            entry_named(result, mutators::Collection::units, "RGBOT_2");
        OA_CHECK(renamed != nullptr);
        if (renamed != nullptr) {
            const std::string body = text_of(manifest.read(renamed->path).bytes);
            OA_CHECK(body.find("999") != std::string::npos);
            OA_CHECK(has_assignment(body, "UnitName", "RGBOT_2"));
            OA_CHECK(body.find("Name=Ridge Bot") != std::string::npos);
        }
    }
    release(manifest);

    oa::AssetStore setting = open_ridge();
    {
        std::vector<std::pair<std::string, std::string>> files;
        files.emplace_back("units/RGBOT.fbi", std::string{bot});
        std::vector<mutators::PipelineMutator> list;
        list.push_back(make_package(scratch, "ridge-mode", "", "\"files\"", k_files_lua, files));
        mutators::PipelineInput input = make_input(std::move(list));
        input.clash_default = oamut::ClashMode::add;
        const mutators::PipelineResult result = mutators::run_pipeline(setting, std::move(input));
        OA_CHECK(result.playable());
        if (!result.playable())
            note_failures(result);
        OA_CHECK(bot_was_renamed(result));
        const mutators::JournalEntry* renamed =
            entry_named(result, mutators::Collection::units, "RGBOT_2");
        OA_CHECK(renamed != nullptr);
        if (renamed != nullptr) {
            const std::string body = text_of(setting.read(renamed->path).bytes);
            OA_CHECK(body.find("999") != std::string::npos);
        }
        const std::string base = text_of(setting.read("units/RGBOT.fbi").bytes);
        OA_CHECK(base.find("999") == std::string::npos);
    }
    release(setting);
    fs::remove_all(scratch);
}

/// Add serves a renamed unit's own script, picture and model under the
/// private name, and leaves the unit already in the game with its own bytes.
void test_renamed_unit_keeps_its_name_keyed_files() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-files");
    std::vector<std::pair<std::string, std::string>> files;
    files.emplace_back(
        "units/RGBOT.fbi",
        "[UNITINFO]\n{\nUnitName=RGBOT;\nName=Ridge Bot;\nMaxDamage=999;\nWeapon1=RGGUN;\n}\n"
    );
    files.emplace_back("scripts/RGBOT.cob", "COB");
    files.emplace_back("unitpics/RGBOT.pcx", "PIC");
    files.emplace_back("objects3d/RGBOT.3do", "MODEL");
    std::vector<mutators::PipelineMutator> list;
    list.push_back(make_package(scratch, "ridge-files", "add", "\"files\"", k_files_lua, files));
    oa::AssetStore store = open_ridge();
    const std::vector<uint8_t> bot_before = store.read("units/RGBOT.fbi").bytes;
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    if (!result.playable()) {
        note_failures(result);
        release(store);
        fs::remove_all(scratch);
        return;
    }
    OA_CHECK(store.read("units/RGBOT.fbi").bytes == bot_before);
    OA_CHECK(store.file_size("scripts/RGBOT.cob") == 0);
    OA_CHECK(store.file_size("unitpics/RGBOT.pcx") == 0);
    // The mutator's model moved with the renamed unit. The bytes still served
    // at the old path are the Ridge fixture's model.
    OA_CHECK(
        store.read("objects3d/RGBOT.3do").bytes ==
        read_bytes(k_data / "ridge" / "objects3d" / "RGBOT.3DO")
    );
    OA_CHECK(store.file_size("scripts/RGBOT_2.cob") > 0);
    OA_CHECK(store.file_size("unitpics/RGBOT_2.pcx") > 0);
    OA_CHECK(store.file_size("objects3d/RGBOT_2.3do") > 0);
    if (store.file_size("scripts/RGBOT_2.cob") > 0)
        OA_CHECK(text_of(store.read("scripts/RGBOT_2.cob").bytes) == "COB");
    if (store.file_size("unitpics/RGBOT_2.pcx") > 0)
        OA_CHECK(text_of(store.read("unitpics/RGBOT_2.pcx").bytes) == "PIC");
    if (store.file_size("objects3d/RGBOT_2.3do") > 0)
        OA_CHECK(text_of(store.read("objects3d/RGBOT_2.3do").bytes) == "MODEL");
    const mutators::JournalEntry* renamed =
        entry_named(result, mutators::Collection::units, "RGBOT_2");
    OA_CHECK(renamed != nullptr);
    if (renamed != nullptr) {
        const std::string body = text_of(store.read(renamed->path).bytes);
        OA_CHECK(has_assignment(body, "UnitName", "RGBOT_2"));
        OA_CHECK(has_assignment(body, "Objectname", "RGBOT_2"));
    }
    release(store);
    fs::remove_all(scratch);
}

/// A unit that only differs by a weapon this mutator renamed is renamed too.
void test_unit_follows_its_renamed_weapon() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-follow");
    std::vector<std::pair<std::string, std::string>> files;
    files.emplace_back("weapons/ridge.tdf", "[RGGUN]\n{\nID=1;\nRange=4;\n}\n");
    files.emplace_back(
        "units/RGBOT.fbi",
        "[UNITINFO]\n{\nUnitName=RGBOT;\nMaxVelocity=1.6;\nWeapon1=RGGUN;\n"
        "Corpse=RGBOT_DEAD;\nMovementClass=RGKBOT;\n}\n"
    );
    std::vector<mutators::PipelineMutator> list;
    list.push_back(make_package(scratch, "ridge-follow", "add", "\"files\"", k_files_lua, files));
    oa::AssetStore store = open_ridge();
    const std::vector<uint8_t> bot_before = store.read("units/RGBOT.fbi").bytes;
    const std::vector<uint8_t> gun_before = store.read("weapons/ridge.tdf").bytes;
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    if (!result.playable()) {
        note_failures(result);
        release(store);
        fs::remove_all(scratch);
        return;
    }
    OA_CHECK(store.read("units/RGBOT.fbi").bytes == bot_before);
    OA_CHECK(has_assignment(text_of(gun_before), "ID", "1"));
    const std::string guns = text_of(store.read("weapons/ridge.tdf").bytes);
    OA_CHECK(guns.find("[RGGUN_2]") != std::string::npos);
    OA_CHECK(guns.find("Range=4") != std::string::npos);
    const mutators::JournalEntry* renamed =
        entry_named(result, mutators::Collection::units, "RGBOT_2");
    OA_CHECK(renamed != nullptr);
    if (renamed != nullptr) {
        const std::string body = text_of(store.read(renamed->path).bytes);
        OA_CHECK(has_assignment(body, "UnitName", "RGBOT_2"));
        OA_CHECK(has_assignment(body, "Weapon1", "RGGUN_2"));
    }
    OA_CHECK(weapon_slot(store, "RGGUN") == 1);
    OA_CHECK(weapon_slot(store, "RGGUN_2") > 2);
    release(store);
    fs::remove_all(scratch);
}

/// explodeas and selfdestructas follow a weapon this mutator renamed.
void test_corpse_weapons_follow_the_rename() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-corpse");
    std::vector<std::pair<std::string, std::string>> files;
    files.emplace_back("weapons/ridge.tdf", "[RGGUN]\n{\nID=1;\nRange=4;\n}\n");
    files.emplace_back(
        "units/RGBOOM.fbi",
        "[UNITINFO]\n{\nUnitName=RGBOOM;\nWeapon1=RGGUN;\nExplodeAs=RGGUN;\n"
        "SelfDestructAs=RGGUN;\n}\n"
    );
    std::vector<mutators::PipelineMutator> list;
    list.push_back(make_package(scratch, "ridge-corpse", "add", "\"files\"", k_files_lua, files));
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    if (!result.playable()) {
        note_failures(result);
        release(store);
        fs::remove_all(scratch);
        return;
    }
    const std::string body = text_of(store.read("units/RGBOOM.fbi").bytes);
    OA_CHECK(has_assignment(body, "Weapon1", "RGGUN_2"));
    OA_CHECK(has_assignment(body, "ExplodeAs", "RGGUN_2"));
    OA_CHECK(has_assignment(body, "SelfDestructAs", "RGGUN_2"));
    OA_CHECK(weapon_slot(store, "RGGUN") == 1);
    release(store);
    fs::remove_all(scratch);
}

/// This mutator's hook and rule name its own renamed unit, not the game's.
void test_own_name_reaches_the_private_unit() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-names");
    constexpr std::string_view hook = "function mutate(ctx)\n"
                                      "  return {\n"
                                      "    files = { add = { \"files\" } },\n"
                                      "    units = function(units, ctx)\n"
                                      "      units.RGBOT.maxdamage = 5\n"
                                      "    end\n"
                                      "  }\n"
                                      "end\n";
    constexpr std::string_view rule = "function mutate(ctx)\n"
                                      "  return {\n"
                                      "    files = { add = { \"files\" } },\n"
                                      "    rules = { where { unitname = is(\"RGBOT\") }:change { "
                                      "maxdamage = 7 } }\n"
                                      "  }\n"
                                      "end\n";
    const std::string bot =
        "[UNITINFO]\n{\nUnitName=RGBOT;\nName=Ridge Bot;\nMaxDamage=1;\nWeapon1=RGGUN;\n}\n";
    oa::AssetStore hooked = open_ridge();
    {
        std::vector<std::pair<std::string, std::string>> files;
        files.emplace_back("units/RGBOT.fbi", bot);
        std::vector<mutators::PipelineMutator> list;
        list.push_back(
            make_package(scratch, "ridge-hook-name", "add", "\"files\", \"units\"", hook, files)
        );
        const mutators::PipelineResult result =
            mutators::run_pipeline(hooked, make_input(std::move(list)));
        OA_CHECK(result.playable());
        if (!result.playable())
            note_failures(result);
        const std::string base = text_of(hooked.read("units/RGBOT.fbi").bytes);
        OA_CHECK(!has_assignment(base, "MaxDamage", "5"));
        const mutators::JournalEntry* renamed =
            entry_named(result, mutators::Collection::units, "RGBOT_2");
        OA_CHECK(renamed != nullptr);
        if (renamed != nullptr)
            OA_CHECK(has_assignment(text_of(hooked.read(renamed->path).bytes), "MaxDamage", "5"));
    }
    release(hooked);

    oa::AssetStore ruled = open_ridge();
    {
        std::vector<std::pair<std::string, std::string>> files;
        files.emplace_back("units/RGBOT.fbi", bot);
        std::vector<mutators::PipelineMutator> list;
        list.push_back(
            make_package(scratch, "ridge-rule-name", "add", "\"files\", \"units\"", rule, files)
        );
        const mutators::PipelineResult result =
            mutators::run_pipeline(ruled, make_input(std::move(list)));
        OA_CHECK(result.playable());
        if (!result.playable())
            note_failures(result);
        const std::string base = text_of(ruled.read("units/RGBOT.fbi").bytes);
        OA_CHECK(!has_assignment(base, "MaxDamage", "7"));
        const mutators::JournalEntry* renamed =
            entry_named(result, mutators::Collection::units, "RGBOT_2");
        OA_CHECK(renamed != nullptr);
        if (renamed != nullptr)
            OA_CHECK(has_assignment(text_of(ruled.read(renamed->path).bytes), "MaxDamage", "7"));
    }
    release(ruled);
    fs::remove_all(scratch);
}

/// A rule's weapon write, with or without a hook, uses the private weapon.
void test_rule_write_uses_the_private_weapon() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-rule-gun");
    constexpr std::string_view rules_only =
        "function mutate(ctx)\n"
        "  return {\n"
        "    files = { add = { \"files\" } },\n"
        "    rules = { where { unitname = is(\"RGCOM\") }:change { weapon1 = \"RGGUN\" } }\n"
        "  }\n"
        "end\n";
    constexpr std::string_view with_hook =
        "function mutate(ctx)\n"
        "  return {\n"
        "    files = { add = { \"files\" } },\n"
        "    rules = { where { unitname = is(\"RGCOM\") }:change { weapon1 = \"RGGUN\" } },\n"
        "    units = function(units, ctx)\n"
        "      units.RGCOM.maxdamage = 3000\n"
        "    end\n"
        "  }\n"
        "end\n";
    const auto run_one = [&](std::string_view id, std::string_view lua, std::string_view layers) {
        std::vector<std::pair<std::string, std::string>> files;
        files.emplace_back("weapons/ridge.tdf", "[RGGUN]\n{\nID=1;\nRange=4;\n}\n");
        std::vector<mutators::PipelineMutator> list;
        list.push_back(make_package(scratch, id, "add", layers, lua, files));
        oa::AssetStore store = open_ridge();
        const mutators::PipelineResult result =
            mutators::run_pipeline(store, make_input(std::move(list)));
        OA_CHECK(result.playable());
        if (!result.playable())
            note_failures(result);
        const std::string commander = text_of(store.read("units/RGCOM.fbi").bytes);
        OA_CHECK(has_assignment(commander, "weapon1", "RGGUN_2"));
        OA_CHECK(!has_assignment(commander, "weapon1", "RGGUN"));
        OA_CHECK(weapon_slot(store, "RGGUN") == 1);
        release(store);
    };
    run_one("ridge-rule-gun", rules_only, "\"files\", \"units\"");
    run_one("ridge-hook-gun", with_hook, "\"files\", \"units\"");
    fs::remove_all(scratch);
}

/// A later mutator that names the weapon as it was written is left on that weapon.
void test_later_mutator_keeps_the_name_it_wrote() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-later");
    constexpr std::string_view later = "function mutate(ctx)\n"
                                       "  return {\n"
                                       "    rules = { where { unitname = is(\"RGCOM\") }:change { "
                                       "weapon1 = \"RGGUN\" } }\n"
                                       "  }\n"
                                       "end\n";
    std::vector<mutators::PipelineMutator> list;
    {
        std::vector<std::pair<std::string, std::string>> files;
        files.emplace_back("weapons/ridge.tdf", "[RGGUN]\n{\nID=1;\nRange=4;\n}\n");
        list.push_back(
            make_package(scratch, "ridge-gun-add", "add", "\"files\"", k_files_lua, files)
        );
    }
    list.push_back(make_package(scratch, "ridge-gun-use", "", "\"units\"", later, {}));
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    if (!result.playable()) {
        note_failures(result);
        release(store);
        fs::remove_all(scratch);
        return;
    }
    const std::string commander = text_of(store.read("units/RGCOM.fbi").bytes);
    OA_CHECK(has_assignment(commander, "weapon1", "RGGUN"));
    OA_CHECK(commander.find("RGGUN_2") == std::string::npos);
    OA_CHECK(text_of(store.read("weapons/ridge.tdf").bytes).find("[RGGUN_2]") != std::string::npos);
    release(store);
    fs::remove_all(scratch);
}

/// A renamed builder's menu button and build list follow the private name.
void test_renamed_builder_keeps_its_menu_and_list() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-builder");
    constexpr std::string_view lua = "function mutate(ctx)\n"
                                     "  return {\n"
                                     "    files = { add = { \"files\" } },\n"
                                     "    units = function(units, ctx)\n"
                                     "      ctx.sides:add_canbuild(\"RGLAB\", \"RGNEW2\")\n"
                                     "    end\n"
                                     "  }\n"
                                     "end\n";
    std::vector<std::pair<std::string, std::string>> files;
    files.emplace_back(
        "units/RGLAB.fbi",
        "[UNITINFO]\n{\nUnitName=RGLAB;\nBuilder=1;\nBMcode=0;\nWorkerTime=50;\n}\n"
    );
    files.emplace_back("units/RGNEW2.fbi", "[UNITINFO]\n{\nUnitName=RGNEW2;\n}\n");
    files.emplace_back("download/mylab.tdf", "[MENU0]\n{\nUNITMENU=RGLAB;\nUNITNAME=RGNEW2;\n}\n");
    std::vector<mutators::PipelineMutator> list;
    list.push_back(
        make_package(scratch, "ridge-builder", "add", "\"files\", \"units\", \"sides\"", lua, files)
    );
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    if (!result.playable()) {
        note_failures(result);
        release(store);
        fs::remove_all(scratch);
        return;
    }
    const std::string menu = text_of(store.read("download/mylab.tdf").bytes);
    OA_CHECK(menu.find("[MENU0]") != std::string::npos);
    OA_CHECK(menu.find("[MENU0_2]") == std::string::npos);
    OA_CHECK(has_assignment(menu, "UNITMENU", "RGLAB_2"));
    OA_CHECK(has_assignment(menu, "UNITNAME", "RGNEW2"));
    const std::string game_menu = text_of(store.read("download/rglab.tdf").bytes);
    OA_CHECK(has_assignment(game_menu, "UNITMENU", "RGLAB"));
    OA_CHECK(game_menu.find("RGNEW2") == std::string::npos);
    const std::string sides = text_of(store.read("gamedata/sidedata.tdf").bytes);
    OA_CHECK(sides.find("[RGLAB_2]") != std::string::npos);
    OA_CHECK(sides.find("[RGLAB]") == std::string::npos);
    OA_CHECK(has_assignment(sides, "canbuild1", "RGNEW2"));
    release(store);
    fs::remove_all(scratch);
}

/// Add clones onto an existing name, notes the rename, and later name
/// lookups in the same hook reach the clone.
void test_add_clone_onto_an_existing_name() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-clone-add");
    constexpr std::string_view lua = "function mutate(ctx)\n"
                                     "  return {\n"
                                     "    units = function(units, ctx)\n"
                                     "      units:clone(\"RGBOT\", \"RGCOM\", { maxdamage = 31 })\n"
                                     "      units.RGCOM.maxdamage = 32\n"
                                     "    end\n"
                                     "  }\n"
                                     "end\n";
    std::vector<mutators::PipelineMutator> list;
    list.push_back(make_package(scratch, "ridge-clone-add", "add", "\"units\"", lua, {}));
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    if (!result.playable()) {
        note_failures(result);
        release(store);
        fs::remove_all(scratch);
        return;
    }
    const std::string game = text_of(store.read("units/RGCOM.fbi").bytes);
    OA_CHECK(has_assignment(game, "MaxDamage", "3000"));
    OA_CHECK(!has_assignment(game, "MaxDamage", "32"));
    const mutators::JournalEntry* clone =
        entry_named(result, mutators::Collection::units, "RGCOM_2");
    OA_CHECK(clone != nullptr);
    if (clone != nullptr) {
        OA_CHECK(clone->old_text.has_value() && *clone->old_text == "RGCOM");
        const std::string body = text_of(store.read(clone->path).bytes);
        OA_CHECK(has_assignment(body, "UnitName", "RGCOM_2"));
        OA_CHECK(has_assignment(body, "maxdamage", "32"));
    }
    const std::vector<std::string> ids{"ridge-clone-add"};
    bool saw_rename = false;
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        if (conflict.kind != mutators::ConflictKind::record_renamed)
            continue;
        if (mutators::describe(conflict, ids) == "ridge-clone-add renamed RGCOM to RGCOM_2")
            saw_rename = true;
    }
    OA_CHECK(saw_rename);
    release(store);
    fs::remove_all(scratch);
}

/// Replace puts a clone in place of the record that already has that name.
void test_replace_clone_overwrites_the_existing_unit() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-clone-replace");
    constexpr std::string_view lua = "function mutate(ctx)\n"
                                     "  return {\n"
                                     "    units = function(units, ctx)\n"
                                     "      units:clone(\"RGBOT\", \"RGCOM\", { maxdamage = 31 })\n"
                                     "    end\n"
                                     "  }\n"
                                     "end\n";
    std::vector<mutators::PipelineMutator> list;
    list.push_back(make_package(scratch, "ridge-clone-replace", "replace", "\"units\"", lua, {}));
    oa::AssetStore store = open_ridge();
    const std::vector<uint8_t> bot_before = store.read("units/RGBOT.fbi").bytes;
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    if (!result.playable()) {
        note_failures(result);
        release(store);
        fs::remove_all(scratch);
        return;
    }
    OA_CHECK(store.read("units/RGBOT.fbi").bytes == bot_before);
    const std::string commander = text_of(store.read("units/RGCOM.fbi").bytes);
    OA_CHECK(has_assignment(commander, "UnitName", "RGCOM"));
    OA_CHECK(has_assignment(commander, "maxdamage", "31"));
    OA_CHECK(!has_assignment(commander, "MaxDamage", "3000"));
    const mutators::JournalEntry* clone = entry_named(result, mutators::Collection::units, "RGCOM");
    OA_CHECK(clone != nullptr);
    if (clone != nullptr) {
        OA_CHECK(clone->old_text.has_value() && *clone->old_text == "RGCOM");
        OA_CHECK(clone->new_text.has_value());
        if (clone->new_text.has_value())
            OA_CHECK(*clone->new_text == "replaced unit RGCOM (copy of RGBOT)");
    }
    const std::vector<std::string> ids{"ridge-clone-replace"};
    bool saw_game = false;
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        if (conflict.kind != mutators::ConflictKind::value_replaced)
            continue;
        const std::string line = mutators::describe(conflict, ids);
        if (line == "ridge-clone-replace replaced RGCOM from the game on " + conflict.path)
            saw_game = true;
    }
    OA_CHECK(saw_game);
    release(store);
    fs::remove_all(scratch);
}

/// A Replace clone onto a unit with a script of its own serves the source's
/// script and picture under that name, so the script matches the model.
void test_replace_clone_brings_the_source_script() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-clone-script");
    std::vector<std::pair<std::string, std::string>> art;
    art.emplace_back("scripts/RGBOT.cob", "BOT-COB");
    art.emplace_back("scripts/RGCOM.cob", "COM-COB");
    art.emplace_back("unitpics/RGBOT.pcx", "BOT-PCX");
    art.emplace_back("unitpics/RGCOM.pcx", "COM-PCX");
    constexpr std::string_view lua = "function mutate(ctx)\n"
                                     "  return {\n"
                                     "    units = function(units, ctx)\n"
                                     "      units:clone(\"RGBOT\", \"RGCOM\", {})\n"
                                     "    end\n"
                                     "  }\n"
                                     "end\n";
    std::vector<mutators::PipelineMutator> list;
    list.push_back(
        make_package(scratch, "ridge-art-first", "replace", "\"files\"", k_files_lua, art)
    );
    list.push_back(make_package(scratch, "ridge-clone-onto", "replace", "\"units\"", lua, {}));
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    if (!result.playable()) {
        note_failures(result);
        release(store);
        fs::remove_all(scratch);
        return;
    }
    const std::string commander = text_of(store.read("units/RGCOM.fbi").bytes);
    OA_CHECK(has_assignment(commander, "UnitName", "RGCOM"));
    OA_CHECK(has_assignment(commander, "Objectname", "RGBOT"));
    OA_CHECK(text_of(store.read("scripts/RGCOM.cob").bytes) == "BOT-COB");
    OA_CHECK(text_of(store.read("unitpics/RGCOM.pcx").bytes) == "BOT-PCX");
    OA_CHECK(text_of(store.read("scripts/RGBOT.cob").bytes) == "BOT-COB");
    OA_CHECK(text_of(store.read("unitpics/RGBOT.pcx").bytes) == "BOT-PCX");
    const mutators::JournalEntry* clone = entry_named(result, mutators::Collection::units, "RGCOM");
    OA_CHECK(clone != nullptr);
    if (clone != nullptr) {
        OA_CHECK(clone->old_text.has_value() && *clone->old_text == "RGCOM");
        OA_CHECK(
            clone->new_text.has_value() && *clone->new_text == "replaced unit RGCOM (copy of RGBOT)"
        );
    }
    bool saw_script = false;
    for (const mutators::JournalEntry& entry : result.journal.entries) {
        if (entry.new_text.has_value() &&
            *entry.new_text == "added path scripts/RGCOM.COB, copy of scripts/RGBOT.COB")
            saw_script = entry.mutators.size() == 1 && entry.mutators.front() == 1;
    }
    OA_CHECK(saw_script);
    const std::vector<std::string> ids{"ridge-art-first", "ridge-clone-onto"};
    std::size_t notes = 0;
    bool saw_note = false;
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        if (conflict.kind != mutators::ConflictKind::value_replaced || conflict.record != "RGCOM")
            continue;
        ++notes;
        saw_note = mutators::describe(conflict, ids) ==
                   "ridge-clone-onto replaced RGCOM from the game on " + conflict.path;
    }
    OA_CHECK(notes == 1);
    OA_CHECK(saw_note);
    release(store);
    fs::remove_all(scratch);
}

/// A section name used by another download file is not a clash.
void test_download_section_name_is_per_file() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-menu-file");
    std::vector<std::pair<std::string, std::string>> files;
    files.emplace_back("units/RGNEW.fbi", "[UNITINFO]\n{\nUnitName=RGNEW;\n}\n");
    files.emplace_back("download/rgnew.tdf", "[MENU0]\n{\nUNITMENU=RGLAB;\nUNITNAME=RGNEW;\n}\n");
    std::vector<mutators::PipelineMutator> list;
    list.push_back(
        make_package(scratch, "ridge-menu-file", "replace", "\"files\"", k_files_lua, files)
    );
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    if (!result.playable()) {
        note_failures(result);
        release(store);
        fs::remove_all(scratch);
        return;
    }
    const std::string menu = text_of(store.read("download/rgnew.tdf").bytes);
    OA_CHECK(menu.find("[MENU0]") != std::string::npos);
    OA_CHECK(menu.find("[MENU0_2]") == std::string::npos);
    OA_CHECK(count_kind(result, mutators::ConflictKind::record_renamed) == 0);
    const std::string game = text_of(store.read("download/rglab.tdf").bytes);
    OA_CHECK(game.find("[MENU0]") != std::string::npos);
    release(store);
    fs::remove_all(scratch);
}

/// A sixth button spills into a new download file that starts with a placeholder.
void test_sixth_download_button_spills() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-spill");
    std::string menu;
    for (int index = 0; index < 6; ++index) {
        const char name = static_cast<char>('A' + index);
        menu += "[MENU";
        menu += std::to_string(index);
        menu += "]\n{\nUNITMENU=RGLAB;\nUNITNAME=RGN";
        menu.push_back(name);
        menu += ";\n}\n";
    }
    std::vector<std::pair<std::string, std::string>> files;
    files.emplace_back("download/six.tdf", menu);
    for (int index = 0; index < 6; ++index) {
        const char name = static_cast<char>('A' + index);
        std::string path = "units/RGN";
        path.push_back(name);
        path += ".fbi";
        std::string body = "[UNITINFO]\n{\nUnitName=RGN";
        body.push_back(name);
        body += ";\n}\n";
        files.emplace_back(std::move(path), std::move(body));
    }
    std::vector<mutators::PipelineMutator> list;
    list.push_back(
        make_package(scratch, "ridge-spill", "replace", "\"files\"", k_files_lua, files)
    );
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    if (!result.playable()) {
        note_failures(result);
        release(store);
        fs::remove_all(scratch);
        return;
    }
    const std::string original = text_of(store.read("download/six.tdf").bytes);
    OA_CHECK(original.find("UNITNAME=RGNE") != std::string::npos);
    OA_CHECK(original.find("UNITNAME=RGNF") == std::string::npos);
    OA_CHECK(original.find("PLACEHOLDER") == std::string::npos);
    const std::string spill = text_of(store.read("download/RGNF.tdf").bytes);
    OA_CHECK(spill.find("PLACEHOLDER") != std::string::npos);
    OA_CHECK(spill.find("UNITNAME=RGNF") != std::string::npos);
    OA_CHECK(has_assignment(spill, "UNITMENU", "RGLAB"));
    release(store);
    fs::remove_all(scratch);
}

/// A Ridge bot that differs from the game's, so Add renames it.
constexpr std::string_view k_other_bot =
    "[UNITINFO]\n{\nUnitName=RGBOT;\nName=Ridge Bot;\nMaxDamage=999;\nWeapon1=RGGUN;\n}\n";
/// A Ridge lab that differs from the game's, so Add renames it.
constexpr std::string_view k_other_lab =
    "[UNITINFO]\n{\nUnitName=RGLAB;\nBuilder=1;\nBMcode=0;\nWorkerTime=999;\n}\n";

/// The notes, as the log writes them.
std::vector<std::string>
note_lines(const mutators::PipelineResult& result, const std::vector<std::string>& ids) {
    std::vector<std::string> lines;
    for (const mutators::Conflict& conflict : result.report.conflicts)
        lines.push_back(mutators::describe(conflict, ids));
    return lines;
}

/// True when `lines` holds `line`.
bool has_line(const std::vector<std::string>& lines, std::string_view line) {
    for (const std::string& one : lines) {
        if (one == line)
            return true;
    }
    return false;
}

/// True when a line of `lines` holds `text`.
bool line_mentions(const std::vector<std::string>& lines, std::string_view text) {
    for (const std::string& one : lines) {
        if (one.find(text) != std::string::npos)
            return true;
    }
    return false;
}

/// The text the store serves at `path`, or empty when it serves nothing.
std::string served_at(const oa::AssetStore& store, std::string_view path) {
    if (store.file_size(path) == 0)
        return {};
    return text_of(store.read(path).bytes);
}

/// An Add rename takes its own script off the asked path, and the file it
/// had displaced there is served again; a later mutator's patch stays, and
/// the notes name only the files that still hold the path.
void test_add_rename_gives_the_path_back() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-give-back");
    std::vector<std::pair<std::string, std::string>> patch;
    patch.emplace_back("scripts/RGBOT.cob", "A-COB");
    std::vector<std::pair<std::string, std::string>> own;
    own.emplace_back("units/RGBOT.fbi", std::string{k_other_bot});
    own.emplace_back("scripts/RGBOT.cob", "B-COB");
    std::vector<std::pair<std::string, std::string>> later;
    later.emplace_back("scripts/RGBOT.cob", "C-COB");
    const std::vector<std::string> ids{"ridge-patch", "ridge-own", "ridge-later"};
    for (const bool with_later : {false, true}) {
        std::vector<mutators::PipelineMutator> list;
        list.push_back(make_package(scratch, ids[0], "replace", "\"files\"", k_files_lua, patch));
        list.push_back(make_package(scratch, ids[1], "add", "\"files\"", k_files_lua, own));
        if (with_later) {
            list.push_back(
                make_package(scratch, ids[2], "replace", "\"files\"", k_files_lua, later)
            );
        }
        oa::AssetStore store = open_ridge();
        const mutators::PipelineResult result =
            mutators::run_pipeline(store, make_input(std::move(list)));
        OA_CHECK(result.playable());
        if (!result.playable()) {
            note_failures(result);
            release(store);
            continue;
        }
        OA_CHECK(served_at(store, "scripts/RGBOT.cob") == (with_later ? "C-COB" : "A-COB"));
        OA_CHECK(served_at(store, "scripts/RGBOT_2.cob") == "B-COB");
        const std::vector<std::string> lines = note_lines(result, ids);
        OA_CHECK(has_line(lines, "ridge-own renamed RGBOT to RGBOT_2"));
        OA_CHECK(!line_mentions(lines, "wins scripts/RGBOT.cob over ridge-own"));
        OA_CHECK(!line_mentions(lines, "ridge-own wins scripts/RGBOT.cob"));
        OA_CHECK(
            has_line(lines, "ridge-later wins scripts/RGBOT.cob over ridge-patch") == with_later
        );
        bool patch_served = false;
        for (const mutators::JournalEntry& entry : result.journal.entries) {
            if (entry.collection != mutators::Collection::files ||
                entry.change != mutators::Change::added || entry.path != "scripts/RGBOT.cob")
                continue;
            patch_served = entry.mutators.size() == 1 && entry.mutators.front() == 0;
        }
        OA_CHECK(patch_served != with_later);
        release(store);
    }
    fs::remove_all(scratch);
}

/// A renamed unit that brings no script takes the script as it stood at its
/// mutator's turn: an earlier mutator's patch, else the game's file, never
/// a later mutator's patch of the original.
void test_private_unit_takes_the_script_of_its_turn() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-turn-script");
    std::vector<std::pair<std::string, std::string>> before;
    before.emplace_back("scripts/RGBOT.cob", "BASE-COB");
    std::vector<std::pair<std::string, std::string>> own;
    own.emplace_back("units/RGBOT.fbi", std::string{k_other_bot});
    std::vector<std::pair<std::string, std::string>> later;
    later.emplace_back("scripts/RGBOT.cob", "C-COB");
    const fs::path game = scratch / "game";
    write_bytes(game / "scripts" / "RGBOT.cob", "GAME-COB");
    for (const bool earlier_patch : {true, false}) {
        std::vector<mutators::PipelineMutator> list;
        if (earlier_patch) {
            list.push_back(
                make_package(scratch, "ridge-before", "replace", "\"files\"", k_files_lua, before)
            );
        }
        list.push_back(make_package(scratch, "ridge-own", "add", "\"files\"", k_files_lua, own));
        list.push_back(
            make_package(scratch, "ridge-later", "replace", "\"files\"", k_files_lua, later)
        );
        std::vector<fs::path> roots;
        roots.push_back(game);
        roots.push_back(k_data / "ridge");
        oa::AssetStore store(std::move(roots));
        const mutators::PipelineResult result =
            mutators::run_pipeline(store, make_input(std::move(list)));
        OA_CHECK(result.playable());
        if (!result.playable()) {
            note_failures(result);
            release(store);
            continue;
        }
        OA_CHECK(served_at(store, "scripts/RGBOT.cob") == "C-COB");
        OA_CHECK(
            served_at(store, "scripts/RGBOT_2.cob") == (earlier_patch ? "BASE-COB" : "GAME-COB")
        );
        OA_CHECK(
            served_at(store, "units/RGBOT_2.fbi").find("UnitName=RGBOT_2") != std::string::npos
        );
        release(store);
    }
    fs::remove_all(scratch);
}

/// A replacement note names whose record it was: an earlier mutator's
/// clone, an earlier mutator's file, the game's or the mod's.
void test_replacement_notes_name_the_owner() {
    const fs::path scratch = oa::test::make_scratch_directory("V14-owner");
    const std::string clone_new = "function mutate(ctx)\n"
                                  "  return {\n"
                                  "    units = function(units, ctx)\n"
                                  "      units:clone(\"RGBOT\", \"RGNEWX\", {})\n"
                                  "    end\n"
                                  "  }\n"
                                  "end\n";
    const std::string clone_onto = "function mutate(ctx)\n"
                                   "  return {\n"
                                   "    units = function(units, ctx)\n"
                                   "      units:clone(\"RGLAB\", \"RGNEWX\", {})\n"
                                   "    end\n"
                                   "  }\n"
                                   "end\n";
    std::vector<std::pair<std::string, std::string>> unit_file;
    unit_file.emplace_back(
        "units/RGNEWX.fbi", "[UNITINFO]\n{\nUnitName=RGNEWX;\nMaxDamage=5;\n}\n"
    );
    const std::vector<std::string> ids{"ridge-cloner", "ridge-file", "ridge-reclone"};
    std::vector<mutators::PipelineMutator> list;
    list.push_back(make_package(scratch, ids[0], "replace", "\"units\"", clone_new, {}));
    list.push_back(make_package(scratch, ids[1], "replace", "\"files\"", k_files_lua, unit_file));
    list.push_back(make_package(scratch, ids[2], "replace", "\"units\"", clone_onto, {}));
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    if (!result.playable())
        note_failures(result);
    std::size_t notes = 0;
    bool saw_clone_owner = false;
    bool saw_file_owner = false;
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        if (conflict.kind != mutators::ConflictKind::value_replaced || conflict.record != "RGNEWX")
            continue;
        ++notes;
        const std::string line = mutators::describe(conflict, ids);
        if (line == "ridge-file replaced ridge-cloner's RGNEWX on " + conflict.path)
            saw_clone_owner = true;
        if (line == "ridge-reclone replaced ridge-file's RGNEWX on " + conflict.path)
            saw_file_owner = true;
    }
    OA_CHECK(notes == 2);
    OA_CHECK(saw_clone_owner);
    OA_CHECK(saw_file_owner);
    release(store);

    // The mod's folder serves its own RGBOT; the game's folder serves RGGUN.
    const fs::path mod = scratch / "mod";
    write_bytes(mod / "units" / "RGBOT.fbi", "[UNITINFO]\n{\nUnitName=RGBOT;\nMaxDamage=77;\n}\n");
    std::vector<std::pair<std::string, std::string>> both;
    both.emplace_back("units/RGBOT.fbi", std::string{k_other_bot});
    both.emplace_back("weapons/mine.tdf", "[RGGUN]\n{\nrange=777;\n}\n");
    std::vector<mutators::PipelineMutator> replacing;
    replacing.push_back(
        make_package(scratch, "ridge-over-mod", "replace", "\"files\"", k_files_lua, both)
    );
    mutators::PipelineInput input = make_input(std::move(replacing));
    input.mod.id = "ridge-mod";
    for (const bool layered : {true, false}) {
        std::vector<fs::path> roots;
        roots.push_back(mod);
        roots.push_back(k_data / "ridge");
        oa::AssetStore over_mod(std::move(roots));
        mutators::PipelineInput run = input;
        if (!layered)
            run.mod.id = "ridge";
        const mutators::PipelineResult played = mutators::run_pipeline(over_mod, run);
        OA_CHECK(played.playable());
        if (!played.playable())
            note_failures(played);
        const std::vector<std::string> one{"ridge-over-mod"};
        bool saw_bot = false;
        bool saw_gun = false;
        for (const mutators::Conflict& conflict : played.report.conflicts) {
            if (conflict.kind != mutators::ConflictKind::value_replaced)
                continue;
            const std::string line = mutators::describe(conflict, one);
            const std::string bot_from = layered ? "the mod" : "the game";
            if (line == "ridge-over-mod replaced RGBOT from " + bot_from + " on " + conflict.path)
                saw_bot = true;
            if (line == "ridge-over-mod replaced RGGUN from the game on " + conflict.path)
                saw_gun = true;
        }
        OA_CHECK(saw_bot);
        OA_CHECK(saw_gun);
        release(over_mod);
    }
    fs::remove_all(scratch);
}

/// True when a download file the layer serves holds a button for `unit` on `builder`.
bool serves_button(
    const oa::AssetStore& store,
    const mutators::PipelineResult& result,
    std::string_view builder,
    std::string_view unit
) {
    for (const oa::MutatorLayerEntry& entry : result.layer.entries) {
        if (entry.kind == oa::MutatorSourceKind::hidden || entry.path.size() < 9)
            continue;
        std::string folder = entry.path.substr(0, 9);
        for (char& character : folder)
            character = static_cast<char>(std::tolower(static_cast<unsigned char>(character)));
        if (folder != "download/")
            continue;
        const std::string text = text_of(store.read(entry.path).bytes);
        std::size_t start = text.find('[');
        while (start != std::string::npos) {
            const std::size_t end = text.find('[', start + 1);
            const std::string_view section = std::string_view{text}.substr(
                start, end == std::string::npos ? std::string_view::npos : end - start
            );
            if (has_assignment(section, "UNITMENU", builder) &&
                has_assignment(section, "UNITNAME", unit))
                return true;
            start = end;
        }
    }
    return false;
}

/// Runs one Add-mode mutator with a menus hook over the Ridge game.
///
/// The game's `download/rglab.tdf` must come through unchanged.
///
/// @param name the scratch folder and the mutator id
/// @param files the mutator's files
/// @param menus_body the menus hook's body
/// @param check what the run must show, given the store and the result
void run_menus_case(
    std::string_view name,
    const std::vector<std::pair<std::string, std::string>>& files,
    std::string_view menus_body,
    const std::function<void(const oa::AssetStore&, const mutators::PipelineResult&)>& check
) {
    const fs::path scratch = oa::test::make_scratch_directory(std::string{name});
    std::string lua = "function mutate(ctx)\n"
                      "  return {\n"
                      "    files = { add = { \"files\" } },\n"
                      "    menus = function(menus, ctx)\n";
    lua.append(menus_body);
    lua.append("\n    end\n  }\nend\n");
    std::vector<mutators::PipelineMutator> list;
    list.push_back(make_package(scratch, name, "add", "\"files\", \"menus\"", lua, files));
    oa::AssetStore store = open_ridge();
    const std::vector<uint8_t> menu_before = store.read("download/rglab.tdf").bytes;
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    if (!result.playable()) {
        note_failures(result);
        release(store);
        fs::remove_all(scratch);
        return;
    }
    OA_CHECK(store.file_size("download/rglab.tdf") > 0);
    if (store.file_size("download/rglab.tdf") > 0)
        OA_CHECK(store.read("download/rglab.tdf").bytes == menu_before);
    check(store, result);
    release(store);
    fs::remove_all(scratch);
}

/// The same mutator's menus hook names the builder and the unit it renamed,
/// and reaches its private records: a later call does not depend on the
/// order of the calls, the game's lab keeps the game's bot, and a removal
/// leaves the game's button alone.
void test_menus_hook_reaches_the_private_records() {
    // The game's RGLAB already offers the game's RGBOT: the renamed RGBOT
    // still gets a button of its own, and `builds` reports document names.
    std::vector<std::pair<std::string, std::string>> bot;
    bot.emplace_back("units/RGBOT.fbi", std::string{k_other_bot});
    run_menus_case(
        "ridge-menus-bot",
        bot,
        "      menus:add(\"RGLAB\", \"RGBOT\")\n"
        "      local list = menus:builds(\"RGLAB\")\n"
        "      local game, own = false, false\n"
        "      for i = 1, #list do\n"
        "        if list[i] == \"RGBOT\" then game = true end\n"
        "        if list[i] == \"RGBOT_2\" then own = true end\n"
        "      end\n"
        "      if not (game and own) then error(\"builds lists \" .. #list) end",
        [](const oa::AssetStore& store, const mutators::PipelineResult& result) {
            OA_CHECK(serves_button(store, result, "RGLAB", "RGBOT_2"));
        }
    );

    // One call, and two calls, on a builder this mutator renamed.
    std::vector<std::pair<std::string, std::string>> lab;
    lab.emplace_back("units/RGLAB.fbi", std::string{k_other_lab});
    lab.emplace_back("units/RGNEW2.fbi", "[UNITINFO]\n{\nUnitName=RGNEW2;\n}\n");
    lab.emplace_back("units/RGNEW4.fbi", "[UNITINFO]\n{\nUnitName=RGNEW4;\n}\n");
    run_menus_case(
        "ridge-menus-lab",
        lab,
        "      menus:add(\"RGLAB\", \"RGNEW2\")",
        [](const oa::AssetStore& store, const mutators::PipelineResult& result) {
            OA_CHECK(serves_button(store, result, "RGLAB_2", "RGNEW2"));
            OA_CHECK(!serves_button(store, result, "RGLAB", "RGNEW2"));
        }
    );
    run_menus_case(
        "ridge-menus-lab-twice",
        lab,
        "      menus:add(\"RGLAB\", \"RGNEW2\")\n"
        "      menus:add(\"RGLAB\", \"RGNEW4\")",
        [](const oa::AssetStore& store, const mutators::PipelineResult& result) {
            OA_CHECK(serves_button(store, result, "RGLAB_2", "RGNEW2"));
            OA_CHECK(serves_button(store, result, "RGLAB_2", "RGNEW4"));
            OA_CHECK(!serves_button(store, result, "RGLAB", "RGNEW2"));
            OA_CHECK(!serves_button(store, result, "RGLAB", "RGNEW4"));
        }
    );

    // Removing the unit this mutator renamed removes nothing of the game's.
    run_menus_case(
        "ridge-menus-remove",
        bot,
        "      menus:remove(\"RGBOT\")",
        [](const oa::AssetStore&, const mutators::PipelineResult& result) {
            for (const mutators::JournalEntry& entry : result.journal.entries)
                OA_CHECK(entry.collection != mutators::Collection::menus);
        }
    );
}

/// A store with Ridge's files and a los table the tables hook can write.
oa::AssetStore open_ridge_with_los(const fs::path& scratch) {
    const fs::path extra = scratch / "los-game";
    write_bytes(extra / "gamedata" / "los.tdf", "[RADAR]\n{\nrange=1;\n}\n");
    std::vector<fs::path> roots;
    roots.push_back(extra);
    roots.push_back(k_data / "ridge");
    return oa::AssetStore(std::move(roots));
}

/// Runs one scratch package and returns the pipeline result. The store stays mounted.
mutators::PipelineResult run_made(
    const fs::path& scratch,
    oa::AssetStore& store,
    std::string_view id,
    std::string_view layers,
    std::string_view lua
) {
    std::vector<mutators::PipelineMutator> list;
    list.push_back(make_package(scratch, id, "", layers, lua, {}));
    return mutators::run_pipeline(store, make_input(std::move(list)));
}

/// True when a served file whose path contains `path_part` holds `needle`.
bool served_has(
    const mutators::PipelineResult& result, std::string_view path_part, std::string_view needle
) {
    for (const oa::MutatorLayerEntry& entry : result.layer.entries) {
        if (entry.path.find(path_part) == std::string::npos)
            continue;
        if (served_text(entry).find(needle) != std::string::npos)
            return true;
    }
    return false;
}

void expect_refused(
    const fs::path& scratch,
    std::string_view id,
    std::string_view layers,
    std::string_view lua,
    mutators::Cause cause,
    std::string_view message
) {
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result = run_made(scratch, store, id, layers, lua);
    OA_CHECK(result.mutators.size() == 1);
    if (result.mutators.size() == 1) {
        OA_CHECK(result.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(result.mutators[0].phase == mutators::Phase::plan);
        OA_CHECK(result.mutators[0].cause == cause);
        OA_CHECK(result.mutators[0].message == message);
        if (result.mutators[0].message != message)
            std::fprintf(
                stderr, "%s: %s\n", std::string{id}.c_str(), result.mutators[0].message.c_str()
            );
    }
    OA_CHECK(result.journal.entries.empty());
    OA_CHECK(!result.playable());
    release(store);
}

/// A features hook alone sets a wreck's blocking, and only the features layer is declared.
void test_features_hook_sets_a_wreck() {
    const fs::path scratch = oa::test::make_scratch_directory("V18-features");
    constexpr const char* lua = R"lua(
function mutate(ctx)
  return {
    features = function(features, ctx)
      if not rawequal(features, ctx.features) then error("features identity") end
      features.RGBOT_DEAD.blocking = 0
    end
  }
end
)lua";
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        run_made(scratch, store, "ridge-v18-features", "\"features\"", lua);
    OA_CHECK(result.playable());
    if (!result.playable())
        note_failures(result);
    OA_CHECK(served_has(result, "features/", "blocking=0"));
    OA_CHECK(!served_has(result, "units/", "OrderMark"));
    release(store);
    fs::remove_all(scratch);
}

/// A sides hook alone swaps the commander's unit, and only the sides layer is declared.
void test_sides_hook_swaps_a_commander() {
    const fs::path scratch = oa::test::make_scratch_directory("V18-sides");
    constexpr const char* lua = R"lua(
function mutate(ctx)
  return {
    sides = function(sides, ctx)
      if not rawequal(sides, ctx.sides) then error("sides identity") end
      sides[1].commander = "RGBOT"
    end
  }
end
)lua";
    oa::AssetStore store = open_ridge();
    const mutators::PipelineResult result =
        run_made(scratch, store, "ridge-v18-sides", "\"sides\"", lua);
    OA_CHECK(result.playable());
    if (!result.playable())
        note_failures(result);
    OA_CHECK(served_has(result, "sidedata.tdf", "commander=RGBOT"));
    release(store);
    fs::remove_all(scratch);
}

/// A tables hook alone writes a los key, and only the tables layer is declared.
void test_tables_hook_writes_a_los_key() {
    const fs::path scratch = oa::test::make_scratch_directory("V18-tables");
    constexpr const char* lua = R"lua(
function mutate(ctx)
  return {
    tables = function(tables, ctx)
      if not rawequal(tables, ctx.tables) then error("tables identity") end
      local los = tables["los"]
      if los == nil then error("los missing") end
      los["RADAR"].range = "8"
    end
  }
end
)lua";
    oa::AssetStore store = open_ridge_with_los(scratch);
    const mutators::PipelineResult result =
        run_made(scratch, store, "ridge-v18-tables", "\"tables\"", lua);
    OA_CHECK(result.playable());
    if (!result.playable())
        note_failures(result);
    OA_CHECK(served_has(result, "los.tdf", "range=8"));
    release(store);
    fs::remove_all(scratch);
}

/// Each of the three hooks is refused when its layer is not declared, with that sentence.
void test_layer_hook_without_its_layer() {
    const fs::path scratch = oa::test::make_scratch_directory("V18-undeclared");
    const char* names[] = {"features", "sides", "tables"};
    for (const char* name : names) {
        const std::string id = std::string{"ridge-v18-no-"} + name;
        const std::string lua = std::string{"function mutate(ctx)\n  return {\n    "} + name +
                                " = function(collection, ctx) end\n  }\nend\n";
        const std::string message = std::string{"returns a "} + name +
                                    " hook but its manifest does not declare the " + name +
                                    " layer";
        expect_refused(scratch, id, "\"units\"", lua, mutators::Cause::undeclared_layer, message);
    }
    fs::remove_all(scratch);
}

/// A plan value that is not a function is refused for each of the three hooks.
void test_layer_hook_must_be_a_function() {
    const fs::path scratch = oa::test::make_scratch_directory("V18-not-function");
    const char* names[] = {"features", "sides", "tables"};
    for (const char* name : names) {
        const std::string id = std::string{"ridge-v18-bad-"} + name;
        const std::string lua =
            std::string{"function mutate(ctx)\n  return { "} + name + " = true }\nend\n";
        const std::string message = std::string{name} + " must be a function";
        const std::string layers = std::string{"\""} + name + "\"";
        expect_refused(scratch, id, layers, lua, mutators::Cause::invalid_plan, message);
    }
    fs::remove_all(scratch);
}

/// A write through `ctx` needs the layer of the collection it changes, whichever hook makes it.
void test_layer_hook_writes_need_their_layer() {
    const fs::path scratch = oa::test::make_scratch_directory("V18-other-layer");

    struct Case {
        const char* id;
        const char* layers;
        const char* lua;
        const char* message;
    };

    const Case cases[] = {
        {"ridge-v18-features-unit",
         "\"features\"",
         "function mutate(ctx)\n  return {\n    features = function(features, ctx)\n"
         "      ctx.units.RGBOT.maxdamage = 1\n    end\n  }\nend\n",
         "this mutator does not declare the units layer"},
        {"ridge-v18-sides-feature",
         "\"sides\"",
         "function mutate(ctx)\n  return {\n    sides = function(sides, ctx)\n"
         "      ctx.features.RGBOT_DEAD.blocking = 0\n    end\n  }\nend\n",
         "this mutator does not declare the features layer"},
        {"ridge-v18-tables-side",
         "\"tables\"",
         "function mutate(ctx)\n  return {\n    tables = function(tables, ctx)\n"
         "      ctx.sides[1].commander = \"RGBOT\"\n    end\n  }\nend\n",
         "this mutator does not declare the sides layer"},
    };
    for (const Case& one : cases) {
        oa::AssetStore store = open_ridge();
        const mutators::PipelineResult result =
            run_made(scratch, store, one.id, one.layers, one.lua);
        OA_CHECK(result.mutators.size() == 1);
        if (result.mutators.size() == 1) {
            OA_CHECK(result.mutators[0].outcome == mutators::Outcome::failed);
            OA_CHECK(result.mutators[0].phase == mutators::Phase::hooks);
            OA_CHECK(result.mutators[0].cause == mutators::Cause::script_error);
            OA_CHECK(result.mutators[0].message == one.message);
            if (result.mutators[0].message != one.message)
                std::fprintf(stderr, "%s: %s\n", one.id, result.mutators[0].message.c_str());
        }
        OA_CHECK(result.journal.entries.empty());
        OA_CHECK(!result.playable());
        release(store);
    }
    fs::remove_all(scratch);
}

/// One mutator with all six hooks writes units, weapons, features, sides, tables, then menus.
void test_six_hooks_write_in_order() {
    const fs::path scratch = oa::test::make_scratch_directory("V18-six-hooks");
    constexpr const char* lua = R"lua(
function mutate(ctx)
  return {
    units = function(units, ctx)
      if units.RGBOT.OrderMark ~= nil then error("units not first") end
      units.RGBOT.OrderMark = "units"
      units.RGBOT.maxdamage = 1111
    end,
    weapons = function(weapons, ctx)
      if ctx.units.RGBOT.OrderMark ~= "units" then error("weapons out of order") end
      ctx.units.RGBOT.OrderMark = "weapons"
      weapons.RGGUN.range = 44
    end,
    features = function(features, ctx)
      if not rawequal(features, ctx.features) then error("features identity") end
      if ctx.units.RGBOT.OrderMark ~= "weapons" then error("features out of order") end
      ctx.units.RGBOT.OrderMark = "features"
      features.RGBOT_DEAD.blocking = 0
    end,
    sides = function(sides, ctx)
      if not rawequal(sides, ctx.sides) then error("sides identity") end
      if ctx.units.RGBOT.OrderMark ~= "features" then error("sides out of order") end
      ctx.units.RGBOT.OrderMark = "sides"
      sides[1].commander = "RGBOT"
    end,
    tables = function(tables, ctx)
      if not rawequal(tables, ctx.tables) then error("tables identity") end
      if ctx.units.RGBOT.OrderMark ~= "sides" then error("tables out of order") end
      ctx.units.RGBOT.OrderMark = "tables"
      local los = tables["los"]
      if los == nil then error("los missing") end
      los["RADAR"].range = "8"
    end,
    menus = function(menus, ctx)
      if ctx.units.RGBOT.OrderMark ~= "tables" then error("menus out of order") end
      ctx.units.RGBOT.OrderMark = "menus"
      menus:add("RGLAB", "RGCOM")
    end
  }
end
)lua";
    oa::AssetStore store = open_ridge_with_los(scratch);
    const mutators::PipelineResult result = run_made(
        scratch,
        store,
        "ridge-v18-six",
        "\"units\", \"weapons\", \"features\", \"sides\", \"tables\", \"menus\"",
        lua
    );
    OA_CHECK(result.playable());
    if (!result.playable())
        note_failures(result);
    std::vector<std::string> marks;
    for (const mutators::JournalEntry& entry : result.journal.entries) {
        if (entry.field == "ordermark" && entry.new_text.has_value())
            marks.push_back(*entry.new_text);
    }
    const char* expected[] = {"units", "weapons", "features", "sides", "tables", "menus"};
    OA_CHECK(marks.size() == 6);
    if (marks.size() == 6) {
        for (std::size_t index = 0; index < 6; ++index)
            OA_CHECK(marks[index] == expected[index]);
    }
    OA_CHECK(served_has(result, "rgbot.fbi", "1111"));
    OA_CHECK(served_has(result, "weapons/", "44"));
    OA_CHECK(served_has(result, "features/", "blocking=0"));
    OA_CHECK(served_has(result, "sidedata.tdf", "commander=RGBOT"));
    OA_CHECK(served_has(result, "los.tdf", "range=8"));
    OA_CHECK(serves_button(store, result, "RGLAB", "RGCOM"));
    release(store);
    fs::remove_all(scratch);
}

/// ctx.incompatible ends the mutator, drops its writes, and names the reason.
void test_declared_incompatible() {
    const fs::path scratch = oa::test::make_scratch_directory("V05-incompatible");
    const auto expect = [&](std::string_view id,
                            std::string_view layers,
                            std::string_view lua,
                            mutators::Phase phase,
                            std::string_view message,
                            bool wrote) {
        oa::AssetStore store = open_ridge();
        const mutators::PipelineResult result = run_made(scratch, store, id, layers, lua);
        OA_CHECK(result.mutators.size() == 1);
        if (result.mutators.size() == 1) {
            OA_CHECK(result.mutators[0].outcome == mutators::Outcome::incompatible);
            OA_CHECK(result.mutators[0].cause == mutators::Cause::declared);
            OA_CHECK(result.mutators[0].phase == phase);
            OA_CHECK(result.mutators[0].message == message);
            if (result.mutators[0].message != message) {
                std::fprintf(
                    stderr, "%s: %s\n", std::string{id}.c_str(), result.mutators[0].message.c_str()
                );
            }
        }
        OA_CHECK(result.journal.entries.empty());
        OA_CHECK(result.layer.entries.empty());
        OA_CHECK(!result.playable());
        OA_CHECK(!store.mutator_layer_mounted());
        if (wrote) {
            const oa::AssetData bot = store.read("units/RGBOT.fbi");
            OA_CHECK(text_of(bot.bytes).find("9000") == std::string::npos);
        }
        release(store);
    };

    expect(
        "ridge-v05-plan",
        "\"units\"",
        "function mutate(ctx)\n"
        "  ctx.incompatible(\"needs a second side\")\n"
        "end\n",
        mutators::Phase::plan,
        "needs a second side",
        false
    );
    expect(
        "ridge-v05-empty",
        "\"units\"",
        "function mutate(ctx)\n"
        "  ctx.incompatible(\"\")\n"
        "end\n",
        mutators::Phase::plan,
        "incompatible with this mod",
        false
    );
    expect(
        "ridge-v05-caught",
        "\"units\"",
        "function mutate(ctx)\n"
        "  pcall(function() ctx.incompatible(\"needs a second side\") end)\n"
        "  return { units = function(units, ctx) units.RGBOT.maxdamage = 9000 end }\n"
        "end\n",
        mutators::Phase::plan,
        "needs a second side",
        true
    );
    expect(
        "ridge-v05-hook",
        "\"units\"",
        "function mutate(ctx)\n"
        "  return {\n"
        "    units = function(units, ctx)\n"
        "      units.RGBOT.maxdamage = 9000\n"
        "      ctx.incompatible(\"needs a second side\")\n"
        "    end\n"
        "  }\n"
        "end\n",
        mutators::Phase::hooks,
        "needs a second side",
        true
    );
    expect(
        "ridge-v05-hook-caught",
        "\"units\"",
        "function mutate(ctx)\n"
        "  return {\n"
        "    units = function(units, ctx)\n"
        "      units.RGBOT.maxdamage = 9000\n"
        "      pcall(function() ctx.incompatible(\"needs a second side\") end)\n"
        "    end\n"
        "  }\n"
        "end\n",
        mutators::Phase::hooks,
        "needs a second side",
        true
    );
    fs::remove_all(scratch);
}

} // namespace

int main() {
    test_swift_writes_one_file();
    test_rules_are_order_independent();
    test_later_assignment_wins();
    test_rule_sees_the_hook_above();
    test_hook_replaces_a_rule();
    test_each_step_names_the_last_writer();
    test_lock_holds_against_rules_not_hooks();
    test_hooks_follow_the_list();
    test_same_value_writes_nothing();
    test_failed_mutator_leaves_no_trace();
    test_budgets();
    test_plan_failures();
    test_files_layer();
    test_list_rules();
    test_compatibility_and_repeat();
    test_failure_ends_the_pass();
    test_dependants_of_dependants();
    test_plan_tables_are_plain();
    test_mask_is_journaled();
    test_string_cap_is_its_own_cause();
    test_unread_document_warned_once();
    test_player_option();
    test_refused_hack_drops_the_hook();
    test_refused_hack_spares_the_others();
    test_hacks_without_the_layer();
    test_two_weapon_files_keep_both_weapons();
    test_records_replace_and_base_weapon_id_stays();
    test_download_sections_merge();
    test_rule_sees_its_own_unit_file();
    test_full_weapon_table_refuses_the_mutator();
    test_overflow_refuses_only_the_later_mutator();
    test_rule_cannot_renumber_a_base_weapon();
    test_hook_cannot_renumber_a_base_weapon();
    test_brought_weapon_keeps_its_requested_id();
    test_wide_limit_numbers_a_record_weapon_256();
    test_brought_weapon_id_300_follows_the_limit();
    test_request_overflow_follows_list_order();
    test_removed_record_file_comes_back();
    test_removed_weapon_file_comes_back();
    test_first_blocking();
    test_add_gives_each_unit_its_own_weapon();
    test_replace_makes_both_units_fire_the_later_weapon();
    test_add_shares_an_identical_weapon();
    test_renamed_unit_keeps_menu_side_and_clone();
    test_download_entries_match_builder_and_unit();
    test_effective_mode_follows_player_manifest_setting();
    test_renamed_unit_keeps_its_name_keyed_files();
    test_unit_follows_its_renamed_weapon();
    test_corpse_weapons_follow_the_rename();
    test_own_name_reaches_the_private_unit();
    test_rule_write_uses_the_private_weapon();
    test_later_mutator_keeps_the_name_it_wrote();
    test_renamed_builder_keeps_its_menu_and_list();
    test_add_clone_onto_an_existing_name();
    test_replace_clone_overwrites_the_existing_unit();
    test_replace_clone_brings_the_source_script();
    test_download_section_name_is_per_file();
    test_sixth_download_button_spills();
    test_menus_hook_reaches_the_private_records();
    test_features_hook_sets_a_wreck();
    test_sides_hook_swaps_a_commander();
    test_tables_hook_writes_a_los_key();
    test_layer_hook_without_its_layer();
    test_layer_hook_must_be_a_function();
    test_layer_hook_writes_need_their_layer();
    test_six_hooks_write_in_order();
    test_add_rename_gives_the_path_back();
    test_private_unit_takes_the_script_of_its_turn();
    test_replacement_notes_name_the_owner();
    test_declared_incompatible();
    return oa::test::check_exit_status();
}
