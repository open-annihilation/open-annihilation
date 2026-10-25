// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// `mutator diff` mounts a game the way the game does, runs package folders
// through the mutation pipeline, and prints what changed. `--versus` compares
// a second loadout after folding two ways of saying the same removal.

#include "command.hpp"

#include "arguments.hpp"
#include "mutator_report.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/mod_profile_loader.hpp"
#include "oa/data/content_bundles/content_choice.hpp"
#include "oa/data/mod_profile/overrides.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/data/mutators/pipeline.hpp"
#include "oa/formats/oamut/manifest.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef OA_ENGINE_VERSION
#error OA_ENGINE_VERSION is set from the project version
#endif

namespace oa::tool {
namespace {

namespace fs = std::filesystem;
namespace app = oa::app;
namespace bundles = oa::data::content_bundles;
namespace mutators = oa::data::mutators;
namespace oamut = oa::formats::oamut;
namespace report = mutator_report;

constexpr std::string_view command_name = "oa-tool mutator diff";

const OptionSpec diff_options[] = {
    {"game-dir", true, false},
    {"mod-dir", true, false},
    {"map", true, false},
    {"seed", true, false},
    {"mutator", true, true},
    {"versus", true, true},
    {"allow", true, false},
    {"dump", true, false},
};

/// Prints one usage line and returns exit_usage.
int usage(Output& output, std::string_view problem) {
    output.err << command_name << ": " << problem << '\n';
    return exit_usage;
}

/// A path as UTF-8, for a refusal that names a file the player passed.
std::string path_text(const fs::path& path) {
    const auto text = path.u8string();
    return {text.begin(), text.end()};
}

/// A command-line path, read as UTF-8 rather than the ANSI code page.
fs::path utf8_path(std::string_view text) {
    return fs::path(std::u8string(text.begin(), text.end()));
}

/// Folds ASCII letters to lower case.
std::string lower_ascii(std::string_view text) {
    std::string out(text);
    for (char& piece : out) {
        if (piece >= 'A' && piece <= 'Z')
            piece = static_cast<char>(piece - 'A' + 'a');
    }
    return out;
}

/// Every value of a repeating option, or none.
std::vector<std::string> repeated(const Arguments& arguments, std::string_view name) {
    const auto found = arguments.values.find(name);
    if (found == arguments.values.end())
        return {};
    return found->second;
}

/// Reads a match seed. Empty, a non-digit and an overflow are refused.
bool parse_seed(std::string_view text, uint64_t& seed) {
    if (text.empty())
        return false;
    uint64_t value = 0;
    for (const char piece : text) {
        if (piece < '0' || piece > '9')
            return false;
        const auto digit = static_cast<uint64_t>(piece - '0');
        if (value > (UINT64_MAX - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    seed = value;
    return true;
}

/// Reads an allow file. A missing file is a usage error.
std::optional<report::AllowList> read_allow(const fs::path& path, std::string& problem) {
    std::ifstream in(path, std::ios::in | std::ios::binary);
    if (!in) {
        problem = path_text(path) + ": cannot be read";
        return std::nullopt;
    }
    std::string text;
    char piece = 0;
    while (in.get(piece))
        text.push_back(piece);
    if (!in.eof()) {
        problem = path_text(path) + ": cannot be read";
        return std::nullopt;
    }
    std::string error;
    std::optional<report::AllowList> list = report::parse_allowed(text, &error);
    if (!list)
        problem = path_text(path) + ":" + error;
    return list;
}

/// Archive file names in mount order, separated by one space.
std::string game_label(const std::vector<fs::path>& archives) {
    std::string line;
    for (const fs::path& archive : archives) {
        if (!line.empty())
            line.push_back(' ');
        const auto text = archive.filename().u8string();
        line.append(text.begin(), text.end());
    }
    return line;
}

/// One mutator's place in the loadout line.
std::string mutator_label(const mutators::ResolvedMutator& resolved) {
    std::string line = resolved.manifest.id;
    line.push_back(' ');
    line.append(oamut::version_text(resolved.manifest.version));
    for (const mutators::LoadoutOption& option : resolved.options) {
        line.push_back(' ');
        line.append(option.id);
        line.push_back('=');
        line.append(mutators::option_text(option));
    }
    return line;
}

/// The loadout header, or `none`.
std::string loadout_label(const std::vector<mutators::ResolvedMutator>& listed) {
    if (listed.empty())
        return "none";
    std::string line;
    for (std::size_t index = 0; index < listed.size(); ++index) {
        if (index != 0)
            line.append("; ");
        line.append(mutator_label(listed[index]));
    }
    return line;
}

/// The report header. `# map` is written only when a map was named.
std::string header_text(
    const app::GameInstall& install,
    const std::vector<mutators::ResolvedMutator>& listed,
    uint64_t seed,
    const std::string* map_name
) {
    std::string text = "# oa-tool mutator diff 1\n# game: ";
    text.append(game_label(install.archives));
    text.append("\n# mod: ");
    if (install.profile == nullptr) {
        text.append("none");
    } else {
        text.append(install.profile->id);
        text.push_back(' ');
        text.append(install.profile->version);
    }
    text.append("\n# loadout: ");
    text.append(loadout_label(listed));
    text.append("\n# seed: ");
    text.append(std::to_string(seed));
    text.push_back('\n');
    if (map_name != nullptr) {
        text.append("# map: ");
        text.append(*map_name);
        text.push_back('\n');
    }
    return text;
}

/// Resolves package folders. An id, a bad choice and a bad folder are usage errors.
std::optional<std::vector<mutators::ResolvedMutator>>
resolve_specs(const std::vector<std::string>& specs, std::string& problem) {
    std::vector<mutators::ResolvedMutator> listed;
    listed.reserve(specs.size());
    for (const std::string& spec : specs) {
        std::string error;
        const std::optional<mutators::MutatorChoice> choice = mutators::parse_choice(spec, &error);
        if (!choice) {
            problem = error.empty() ? spec + " is not a mutator choice" : error;
            return std::nullopt;
        }
        if (!choice->by_path) {
            problem = choice->target + " names an id, and this command reads a package folder";
            return std::nullopt;
        }
        const std::optional<mutators::ResolvedMutator> resolved =
            mutators::resolve_choice(*choice, std::filesystem::path{}, &error);
        if (!resolved) {
            problem = error.empty() ? spec + " is not a mutator package" : error;
            return std::nullopt;
        }
        listed.push_back(*resolved);
    }
    return listed;
}

/// The OTA file name the game's map list shows for `name`, ASCII case
/// ignored, without `.ota`; `name` itself when no listed OTA matches.
std::string listed_map_name(const oa::AssetStore& store, std::string_view name) {
    constexpr std::string_view extension = ".ota";
    const std::string folded = lower_ascii(name);
    for (const oa::FoundEntry& entry : store.find("maps\\*.ota")) {
        if (entry.directory)
            continue;
        const auto slash = entry.name.find_last_of("/\\");
        const std::string_view file = slash == std::string::npos
                                          ? std::string_view{entry.name}
                                          : std::string_view{entry.name}.substr(slash + 1);
        if (file.size() <= extension.size() ||
            lower_ascii(file.substr(file.size() - extension.size())) != extension)
            continue;
        const std::string_view stem = file.substr(0, file.size() - extension.size());
        if (lower_ascii(stem) == folded)
            return std::string(stem);
    }
    return std::string(name);
}

/// The map the maps phase reads, named as the game's map list spells it, so
/// `--map` in any case gives the same `# map:` line. A missing OTA is
/// reported through `problem`.
std::optional<mutators::ChosenMap>
chosen_map(const oa::AssetStore& store, std::string_view name, std::string& problem) {
    mutators::ChosenMap chosen;
    chosen.key = listed_map_name(store, name);
    chosen.ota_path = "maps/" + chosen.key + ".ota";
    const std::size_t mark = chosen.key.rfind('@');
    if (mark != std::string::npos && mark + 1 < chosen.key.size())
        chosen.pack = chosen.key.substr(mark + 1);
    std::optional<std::vector<uint8_t>> bytes = store.load_file_contents(chosen.ota_path);
    if (!bytes || bytes->empty()) {
        problem = "map " + std::string(name) + " is not in this game";
        return std::nullopt;
    }
    chosen.ota = std::make_shared<const std::vector<uint8_t>>(std::move(*bytes));
    return chosen;
}

/// The dump field key a journal entry names, or empty for a whole file.
std::string journal_field(const mutators::JournalEntry& entry) {
    if (entry.field.empty())
        return {};
    const std::string field = lower_ascii(entry.field);
    const std::string block = lower_ascii(entry.block);
    const bool ota = entry.record.starts_with("map:") || lower_ascii(entry.path).ends_with(".ota");
    if (ota) {
        std::string section = "[globalheader]";
        constexpr std::string_view schema = "/Schema ";
        const auto mark = entry.record.find(schema);
        if (mark != std::string::npos)
            section += "[schema " + lower_ascii(entry.record.substr(mark + schema.size())) + "]";
        return section + " " + field;
    }
    std::string section;
    if (entry.collection == mutators::Collection::units) {
        section = "[unitinfo]";
    } else if (
        entry.collection == mutators::Collection::weapons ||
        entry.collection == mutators::Collection::features ||
        entry.collection == mutators::Collection::menus ||
        entry.collection == mutators::Collection::sides ||
        entry.collection == mutators::Collection::tables
    ) {
        if (!entry.record.empty())
            section = "[" + lower_ascii(entry.record) + "]";
    }
    if (section.empty())
        return field;
    if (!block.empty())
        section += "[" + block + "]";
    return section + " " + field;
}

/// Records journal writers in loadout order, each id once per path and field.
void attribute(
    report::Attribution& attribution,
    const mutators::Journal& journal,
    const std::vector<std::string>& ids
) {
    const auto add_index = [&](std::string_view path, std::string_view field, uint16_t index) {
        if (static_cast<std::size_t>(index) >= ids.size())
            return;
        attribution.add(path, field, ids[index]);
    };
    for (const mutators::JournalEntry& entry : journal.entries) {
        const bool file_level = entry.field.empty() && (entry.change == mutators::Change::added ||
                                                        entry.change == mutators::Change::hidden ||
                                                        entry.change == mutators::Change::removed);
        const std::string field = journal_field(entry);
        for (const uint16_t index : entry.mutators) {
            if (file_level || field.empty())
                add_index(entry.path, {}, index);
            if (!field.empty())
                add_index(entry.path, field, index);
            if (entry.collection != mutators::Collection::menus)
                continue;
            if (!entry.record.empty())
                add_index("menu", entry.record, index);
            if (lower_ascii(entry.field) == "unitname" && entry.new_text.has_value())
                add_index("menu", *entry.new_text, index);
        }
    }
}

/// Why the first mutator that did not apply stopped.
std::string failure_reason(const mutators::PipelineResult& result) {
    for (const mutators::MutatorResult& one : result.mutators) {
        if (one.outcome == mutators::Outcome::applied)
            continue;
        if (!one.message.empty())
            return one.id + ": " + one.message;
        if (!result.refusal.empty())
            return one.id + ": " + result.refusal;
        return one.id + ": the mutator did not apply";
    }
    if (!result.refusal.empty())
        return result.refusal;
    return "the mutator did not apply";
}

/// One loadout's change lines and the store after it ran.
struct Side {
    std::vector<std::string> lines;
    report::Dump after;
};

/// Runs one loadout. An empty list leaves the store as it was.
Side apply_loadout(
    oa::AssetStore& store,
    const report::Dump& before,
    const std::vector<mutators::ResolvedMutator>& listed,
    const app::GameInstall& install,
    const oa::data::defs::DataLayout& layout,
    uint64_t seed,
    const std::optional<mutators::ChosenMap>& map
) {
    Side side;
    if (listed.empty()) {
        side.after = before;
        return side;
    }
    mutators::PipelineInput input;
    input.mutators.reserve(listed.size());
    std::vector<std::string> ids;
    ids.reserve(listed.size());
    for (const mutators::ResolvedMutator& resolved : listed) {
        mutators::PipelineMutator item;
        item.package = resolved.package_folder;
        item.manifest = resolved.manifest;
        item.options = resolved.options;
        ids.push_back(resolved.manifest.id);
        input.mutators.push_back(std::move(item));
    }
    if (install.profile != nullptr) {
        input.mod.id = install.profile->id;
        input.mod.version = install.profile->version;
        input.mod.game = install.profile->supported_games.empty()
                             ? std::string(bundles::default_base_game_id)
                             : install.profile->supported_games.front();
    } else {
        input.mod.id = std::string(oa::data::mod_profile::base_game_id);
        input.mod.game = std::string(bundles::default_base_game_id);
    }
    input.engine_version = OA_ENGINE_VERSION;
    input.layout = &layout;
    input.limits.weapon_types =
        install.profile != nullptr ? install.profile->limits.weapon_types.types : 256;
    input.limits.sides = install.profile != nullptr ? install.profile->limits.sides.count : 5;
    input.check_integrity = false;
    input.seed = seed;
    input.map = map;
    const mutators::PipelineResult result = mutators::run_pipeline(store, input);
    if (!result.playable())
        throw Failure(failure_reason(result));
    report::Attribution attribution;
    attribute(attribution, result.journal, ids);
    side.after = report::dump_store(store, &before);
    side.lines = report::report_lines(before, side.after, attribution);
    return side;
}

/// Prints the comparison and returns 0 only when nothing remains unmatched.
int write_comparison(
    Output& output,
    const std::string& header,
    const std::string& versus,
    const report::Comparison& compared
) {
    output.out << header;
    output.out << "# versus: " << versus << '\n';
    for (const std::string& line : compared.left_only)
        output.out << line << '\n';
    for (const std::string& line : compared.right_only)
        output.out << line << '\n';
    for (const std::string& line : compared.allowed)
        output.out << "allowed " << line << '\n';
    for (const std::string& pattern : compared.unused_patterns)
        output.out << "allowed difference matched nothing: " << pattern << '\n';
    const std::size_t differences = compared.left_only.size() + compared.right_only.size();
    output.out << "mutator diff: " << differences << " differences, " << compared.allowed.size()
               << " allowed, " << compared.unused_patterns.size() << " unused allowances\n";
    if (differences == 0 && compared.unused_patterns.empty())
        return exit_done;
    return exit_failed;
}

} // namespace

int run_mutator_diff(std::span<const std::string> arguments, Output& output) {
    std::string problem;
    const std::optional<Arguments> parsed = parse_arguments(arguments, diff_options, problem);
    if (!parsed)
        return usage(output, problem);
    if (!parsed->positional.empty())
        return usage(output, "unexpected argument '" + parsed->positional.front() + "'");
    const std::string* const game_dir = parsed->value("game-dir");
    if (game_dir == nullptr)
        return usage(output, "--game-dir is required");
    const std::string* const allow_path = parsed->value("allow");
    const std::vector<std::string> versus = repeated(*parsed, "versus");
    if (allow_path != nullptr && versus.empty())
        return usage(output, "--allow needs --versus");

    uint64_t seed = 0;
    if (const std::string* const seed_text = parsed->value("seed")) {
        if (!parse_seed(*seed_text, seed))
            return usage(output, "seed is not a whole number");
    }
    const std::string* const map_name = parsed->value("map");
    if (map_name != nullptr && (map_name->empty() || map_name->find('/') != std::string::npos ||
                                map_name->find('\\') != std::string::npos)) {
        return usage(output, "map name is empty or holds a slash");
    }

    report::AllowList allowed;
    if (allow_path != nullptr) {
        std::optional<report::AllowList> read = read_allow(utf8_path(*allow_path), problem);
        if (!read)
            return usage(output, problem);
        allowed = std::move(*read);
    }

    const std::vector<std::string> specs = repeated(*parsed, "mutator");
    const std::optional<std::vector<mutators::ResolvedMutator>> listed =
        resolve_specs(specs, problem);
    if (!listed)
        return usage(output, problem);
    const std::optional<std::vector<mutators::ResolvedMutator>> other =
        resolve_specs(versus, problem);
    if (!other)
        return usage(output, problem);

    // Opened after the SPECs resolve, so a refused SPEC leaves no empty file.
    std::ofstream dumped;
    const std::string* const dump_path = parsed->value("dump");
    if (dump_path != nullptr) {
        dumped.open(utf8_path(*dump_path), std::ios::out | std::ios::binary);
        if (!dumped)
            return usage(output, *dump_path + ": cannot be written");
    }

    const fs::path game = utf8_path(*game_dir);
    const std::string* const mod_text = parsed->value("mod-dir");
    app::ModChoice choice;
    if (mod_text != nullptr)
        choice.folder = utf8_path(*mod_text);
    const app::GameInstall install = app::inspect_game_install(game, {}, app::demo_1997, choice);
    if (!install.profile_errors.empty() || !install.problem.empty() || !install.folder ||
        install.archives.empty() || install.folders.empty()) {
        throw Failure(path_text(game) + ": " + app::describe_install_problem(install));
    }

    oa::AssetStore store(install.folders);
    for (const fs::path& candidate : install.archives) {
        const fs::path archive = candidate.is_absolute() ? candidate : game / candidate;
        store.mount(archive);
    }
    const oa::data::defs::DataLayout layout = app::data_layout_of(install.profile.get());
    std::optional<mutators::ChosenMap> map;
    if (map_name != nullptr) {
        map = chosen_map(store, *map_name, problem);
        if (!map)
            return usage(output, problem);
    }
    const std::string* const shown_map = map.has_value() ? &map->key : nullptr;

    const report::Dump before = report::dump_store(store);
    const Side left = apply_loadout(store, before, *listed, install, layout, seed, map);
    if (dump_path != nullptr) {
        report::write_dump(dumped, left.after);
        if (!dumped)
            throw Failure(*dump_path + ": cannot be written");
    }

    if (!versus.empty()) {
        store.unmount_mutator_layer();
        const Side right = apply_loadout(store, before, *other, install, layout, seed, map);
        const report::Comparison compared =
            report::compare(left.lines, right.lines, left.after, right.after, allowed);
        return write_comparison(
            output, header_text(install, *listed, seed, shown_map), loadout_label(*other), compared
        );
    }

    output.out << header_text(install, *listed, seed, shown_map);
    for (const std::string& line : left.lines)
        output.out << line << '\n';
    return exit_done;
}

} // namespace oa::tool
