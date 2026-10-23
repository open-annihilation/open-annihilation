// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The oabrain kind's own rules: its manifest, the files a brain may hold,
// its scripts and icon, its networks and tables, the engine it needs, what
// a folder of Brains holds, and where a brain installs. No script is parsed
// or run. Network bytes are hashed only after the package has been unpacked.

#include "files.hpp"

#include "oa/app/package_install/oabrain.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/brain_library/library.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/formats/oabrain/manifest.hpp"
#include "oa/formats/png.hpp"

#include <array>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#ifndef OA_ENGINE_VERSION
#error                                                                                             \
    "OA_ENGINE_VERSION names this build's version, which a brain's engine requirement is checked against"
#endif

namespace oa::app::package_install::oabrain {

namespace fs = std::filesystem;
namespace brain = oa::formats::oabrain;
namespace mut = oa::formats::oamut;
namespace png = oa::formats::png;
namespace keys = oa::formats::oamod;
namespace sha = oa::base::sha256;

static_assert(brains_folder_name == oa::data::brain_library::brains_folder_name);

namespace {

/// The bytes hashed in one piece of a staged network.
constexpr std::size_t hash_piece_bytes = std::size_t{64} << 10;

/// The release this build is, parsed once from OA_ENGINE_VERSION.
///
/// @return that release; 0.0.0 when the macro is not a version
keys::EngineVersion this_build_version() {
    static const keys::EngineVersion version = [] {
        if (const std::optional<keys::EngineVersion> parsed =
                keys::parse_engine_version(OA_ENGINE_VERSION))
            return *parsed;
        return keys::EngineVersion{};
    }();
    return version;
}

/// Returns a problem.
///
/// @param refusal why
/// @param subject what it names
/// @param detail English, for the log and, where it is the whole sentence, the player
/// @return the problem
Problem problem_of(Refusal refusal, std::string subject = {}, std::string detail = {}) {
    Problem problem{};
    problem.refusal = refusal;
    problem.subject = std::move(subject);
    problem.detail = std::move(detail);
    return problem;
}

/// Tells whether a folder holds the incoming brain, at any version.
///
/// @param held what the folder holds
/// @param incoming the brain
/// @return true when it holds that id
bool holds(const InstalledPackage& held, const Incoming& incoming) noexcept {
    return held.kind == FolderKind::package && held.id == incoming.id;
}

/// Returns what a folder holds, through the hooks.
///
/// @param hooks the hooks
/// @param folder the folder's name
/// @return what it holds; missing without a hook
InstalledPackage look(const FolderHooks& hooks, std::string_view folder) {
    return hooks.look != nullptr ? hooks.look(hooks.context, folder) : InstalledPackage{};
}

/// Returns what a folder's .backup holds, through the hooks.
///
/// @param hooks the hooks
/// @param folder the folder's name
/// @return what it holds; nothing without a hook
std::optional<InstalledPackage> backup_of(const FolderHooks& hooks, std::string_view folder) {
    return hooks.backup_of != nullptr ? hooks.backup_of(hooks.context, folder) : std::nullopt;
}

/// Tells whether a packaged name is a folder.
///
/// @param name the name
/// @return true when it ends in '/'
bool directory_name(std::string_view name) noexcept {
    return !name.empty() && name.back() == '/';
}

/// Reads a folder's oabrain.yaml.
///
/// @param folder the folder
/// @param[out] manifest the manifest, when it was read
/// @param[out] problems the reader's problems, when it did not read
/// @return true when the folder holds a readable manifest
bool read_folder_manifest(
    const fs::path& folder, brain::Manifest& manifest, std::vector<brain::Problem>& problems
) {
    const auto found = entry_without_case(folder, brain::manifest_file);
    if (!found)
        return false;
    std::error_code size_error;
    const uintmax_t size = fs::file_size(*found, size_error);
    if (!size_error && size > oa::formats::oamod::max_input_bytes)
        return false;
    std::ifstream in(*found, std::ios::binary);
    if (!in)
        return false;
    std::vector<uint8_t> bytes;
    for (std::istreambuf_iterator<char> at{in}, end;
         at != end && bytes.size() <= oa::formats::oamod::max_input_bytes;
         ++at)
        bytes.push_back(static_cast<uint8_t>(*at));
    if (!in && !in.eof())
        return false;
    if (bytes.size() > oa::formats::oamod::max_input_bytes)
        return false;
    return brain::read_manifest(bytes, manifest, problems);
}

/// Fills what a folder holds from a manifest it gave.
///
/// @param manifest the manifest
/// @return the folder, of kind package; revision is 0
InstalledPackage installed_of(const brain::Manifest& manifest) {
    InstalledPackage installed{};
    installed.kind = FolderKind::package;
    installed.id = manifest.id;
    installed.name = manifest.name;
    installed.version = mut::version_text(manifest.version);
    installed.revision = 0;
    installed.author = manifest.author.name;
    installed.description = manifest.description;
    return installed;
}

/// The sentence for an engine requirement this build does not meet.
///
/// @param range the requirement
/// @return the wording a map pack uses for the same check
std::string unmet_engine(const keys::EngineRange& range) {
    return "needs Open Annihilation " + keys::describe_engine_range(range) +
           "; this is Open Annihilation " + keys::engine_version_text(this_build_version());
}

/// Refuses a script that is missing or larger than the script limit.
///
/// A missing script leaves limit_bytes 0. A script the reader would not
/// return whole sets it to the script limit, which the sentence uses.
///
/// @param name the script
/// @param too_large true when the script is there and larger than the bound
/// @return the problem
Problem script_problem(std::string name, bool too_large) {
    Problem problem = problem_of(Refusal::script_missing, std::move(name));
    if (too_large)
        problem.limit_bytes = static_cast<uint64_t>(brain::most_script_bytes);
    return problem;
}

/// Finds a packaged file by the name the package wrote.
///
/// @param package the package
/// @param name the name, as written
/// @return the file; null when the package holds none of that name
const PackagedFile* packaged_file(const Package& package, std::string_view name) noexcept {
    for (const PackagedFile& file : package.files) {
        if (directory_name(file.name))
            continue;
        if (file.name == name)
            return &file;
    }
    return nullptr;
}

/// Returns an entry's unpacked size, or the largest uint64_t when the entry
/// is not in the directory.
///
/// @param package the package
/// @param file the file
/// @return the size
uint64_t entry_bytes(const Package& package, const PackagedFile& file) noexcept {
    if (file.entry >= package.directory.entries.size())
        return std::numeric_limits<uint64_t>::max();
    return package.directory.entries[file.entry].bytes;
}

/// Refuses an expansion id this build does not know.
///
/// @param manifest the manifest
/// @param[out] problem the refusal, when an id is unknown
/// @return true when every named expansion is one this build knows
bool known_expansions(const brain::Manifest& manifest, Problem& problem) {
    for (std::size_t index = 0; index < manifest.requires_expansions.size(); ++index) {
        const std::string& id = manifest.requires_expansions[index];
        const oa::data::content_bundles::Bundle* bundle =
            oa::data::content_bundles::find_bundle(id);
        if (bundle != nullptr && bundle->kind == oa::data::content_bundles::Kind::expansion)
            continue;
        problem = problem_of(Refusal::manifest_errors);
        problem.lines.push_back(
            "requires.expansions[" + std::to_string(index) + "]: " + id +
            " is not an expansion this game knows"
        );
        return false;
    }
    return true;
}

/// Tells whether a digest's hex is the one a manifest names.
///
/// @param hex the digest, lower-case hex
/// @param wanted the manifest's SHA-256
/// @return true when they are the same 64 digits
bool same_sha256(const std::array<char, sha::hex_size>& hex, std::string_view wanted) noexcept {
    return wanted.size() == hex.size() && std::string_view(hex.data(), hex.size()) == wanted;
}

} // namespace

bool read_manifest(
    std::span<const uint8_t> manifest_bytes,
    const ManifestContext& context,
    Package& package,
    Problem& problem
) {
    brain::Manifest manifest;
    std::vector<brain::Problem> problems;
    if (!brain::read_manifest(manifest_bytes, manifest, problems)) {
        problem = problem_of(Refusal::manifest_errors);
        for (const brain::Problem& one : problems)
            problem.lines.push_back(brain::describe(one));
        return false;
    }
    if (!known_expansions(manifest, problem))
        return false;
    if (windows_device_name(manifest.id)) {
        problem = problem_of(Refusal::reserved_id, manifest.id);
        return false;
    }
    package.incoming.id = manifest.id;
    package.incoming.name = manifest.name;
    package.incoming.version = mut::version_text(manifest.version);
    package.incoming.revision = 0;

    bool held_entry = false;
    for (const PackagedFile& file : package.files) {
        if (directory_name(file.name))
            continue;
        if (file.name == manifest.entry)
            held_entry = true;
        if (brain::file_role(file.name, manifest) == brain::FileRole::outside) {
            problem = problem_of(Refusal::outside_layout, file.name);
            return false;
        }
    }
    if (!held_entry) {
        problem = script_problem(manifest.entry, false);
        return false;
    }

    for (const PackagedFile& file : package.files) {
        if (directory_name(file.name))
            continue;
        const brain::FileRole role = brain::file_role(file.name, manifest);
        if (role != brain::FileRole::entry && role != brain::FileRole::module)
            continue;
        if (context.read_entry == nullptr) {
            problem = script_problem(file.name, false);
            return false;
        }
        std::vector<uint8_t> bytes;
        const EntryRead read = context.read_entry(
            context.reader, file.name, static_cast<uint64_t>(brain::most_script_bytes), bytes
        );
        if (read == EntryRead::missing) {
            problem = script_problem(file.name, false);
            return false;
        }
        if (read != EntryRead::read) {
            problem = script_problem(file.name, true);
            return false;
        }
        // An empty file is source. Only the bytecode mark is precompiled.
        if (!bytes.empty() && bytes.front() == 0x1B) {
            problem = problem_of(Refusal::precompiled, file.name);
            return false;
        }
    }

    if (!manifest.icon.empty()) {
        if (packaged_file(package, manifest.icon) == nullptr || context.read_entry == nullptr) {
            problem = problem_of(Refusal::icon_invalid, manifest.icon);
            return false;
        }
        std::vector<uint8_t> bytes;
        const EntryRead read =
            context.read_entry(context.reader, manifest.icon, most_icon_bytes, bytes);
        png::Info info{};
        const bool png_ok = read == EntryRead::read && png::has_signature(bytes) &&
                            png::read_info(bytes, png::Messages{}, &info);
        const bool square = png_ok && info.header.width == info.header.height &&
                            info.header.width >= 1 && info.header.width <= most_icon_side;
        if (!square) {
            problem = problem_of(Refusal::icon_invalid, manifest.icon);
            return false;
        }
    }

    uint64_t networks = 0;
    for (const brain::Network& network : manifest.networks) {
        const PackagedFile* file = packaged_file(package, network.file);
        if (file == nullptr) {
            problem = problem_of(Refusal::network_missing, network.file);
            return false;
        }
        const uint64_t bytes = entry_bytes(package, *file);
        if (bytes > std::numeric_limits<uint64_t>::max() - networks) {
            problem = problem_of(Refusal::networks_too_large);
            problem.size_bytes = std::numeric_limits<uint64_t>::max();
            problem.limit_bytes = static_cast<uint64_t>(brain::most_network_bytes);
            return false;
        }
        networks += bytes;
    }
    if (networks > static_cast<uint64_t>(brain::most_network_bytes)) {
        problem = problem_of(Refusal::networks_too_large);
        problem.size_bytes = networks;
        problem.limit_bytes = static_cast<uint64_t>(brain::most_network_bytes);
        return false;
    }

    for (const PackagedFile& file : package.files) {
        if (directory_name(file.name))
            continue;
        if (brain::file_role(file.name, manifest) != brain::FileRole::data)
            continue;
        if (entry_bytes(package, file) > static_cast<uint64_t>(brain::most_data_bytes)) {
            problem = problem_of(Refusal::data_too_large, file.name);
            return false;
        }
    }

    if (!manifest.requires_engine.empty()) {
        const std::optional<keys::EngineRange> range =
            keys::parse_engine_range(manifest.requires_engine);
        if (!range || !keys::engine_range_met(*range, this_build_version())) {
            problem = problem_of(
                Refusal::engine_unmet,
                manifest.requires_engine,
                range ? unmet_engine(*range) : std::string("Its engine requirement cannot be read.")
            );
            return false;
        }
    }
    return true;
}

InstalledPackage read_installed(const fs::path& folder) {
    InstalledPackage installed{};
    try {
        std::error_code error;
        const fs::file_status status = fs::symlink_status(folder, error);
        if (error || !fs::exists(status))
            return installed;
        installed.kind = FolderKind::other;
        if (is_link_or_junction(folder) || !fs::is_directory(status))
            return installed;
        brain::Manifest manifest;
        std::vector<brain::Problem> problems;
        if (read_folder_manifest(folder, manifest, problems) && !manifest.id.empty())
            return installed_of(manifest);
        // Empty folders alone, as a file sync can bring back after an
        // uninstall, hold nothing of the player's: the folder is free, and
        // an install removes them first.
        if (detail::holds_only_empty_folders(folder))
            return InstalledPackage{};
        return installed;
    } catch (const std::exception&) {
        return installed;
    }
}

std::optional<InstalledPackage> read_backup(const fs::path& folder) {
    try {
        const auto kept = entry_without_case(folder, backup_folder_name);
        if (!kept)
            return std::nullopt;
        InstalledPackage backup = read_installed(*kept);
        if (backup.kind == FolderKind::missing)
            return std::nullopt;
        const InstalledPackage own = read_installed(folder);
        if (backup.kind == FolderKind::package &&
            (own.kind != FolderKind::package || own.id != backup.id))
            backup.kind = FolderKind::other;
        return backup;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

InstallPlan plan_install(const Incoming& incoming, const FolderHooks& hooks) {
    const std::string own = incoming.id;
    const InstalledPackage at_own = look(hooks, own);
    if (at_own.kind == FolderKind::missing) {
        InstallPlan plan{};
        plan.target = own;
        // A file the player opened asks before a new folder is made. The
        // Install button answers alongside, which the runtime installs into
        // `alongside`; that folder is this id, and it is missing.
        if (incoming.origin.kind == OriginKind::catalogue) {
            plan.kind = PlanKind::install;
            return plan;
        }
        plan.kind = PlanKind::ask_install;
        plan.alongside = own;
        return plan;
    }
    if (!holds(at_own, incoming)) {
        InstallPlan plan{};
        plan.kind = PlanKind::refuse;
        plan.refusal = Refusal::not_a_pack;
        plan.target = own;
        plan.installed = at_own;
        return plan;
    }
    const bool incoming_catalogue = incoming.origin.kind == OriginKind::catalogue;
    const bool held_catalogue =
        at_own.origin.has_value() && at_own.origin->kind == OriginKind::catalogue;
    if (incoming_catalogue && held_catalogue &&
        at_own.origin->registry == incoming.origin.registry) {
        InstallPlan plan{};
        plan.kind = PlanKind::install;
        plan.replacing = true;
        plan.target = own;
        plan.installed = at_own;
        plan.backup = backup_of(hooks, own);
        return plan;
    }
    if (incoming_catalogue && held_catalogue) {
        InstallPlan plan{};
        plan.kind = PlanKind::ask_version;
        plan.target = own;
        plan.installed = at_own;
        plan.backup = backup_of(hooks, own);
        return plan;
    }
    if (at_own.version == incoming.version) {
        InstallPlan plan{};
        plan.kind = PlanKind::ask_reinstall;
        plan.target = own;
        plan.installed = at_own;
        plan.backup = backup_of(hooks, own);
        return plan;
    }
    InstallPlan plan{};
    plan.kind = PlanKind::ask_update;
    plan.target = own;
    plan.installed = at_own;
    plan.backup = backup_of(hooks, own);
    return plan;
}

bool check_staged(
    const fs::path& staging, const Package&, const PackageOptions&, Problem& problem
) {
    try {
        brain::Manifest manifest;
        std::vector<brain::Problem> problems;
        if (!read_folder_manifest(staging, manifest, problems)) {
            problem = problem_of(Refusal::manifest_errors);
            for (const brain::Problem& one : problems)
                problem.lines.push_back(brain::describe(one));
            return false;
        }
        std::vector<uint8_t> piece(hash_piece_bytes);
        for (const brain::Network& network : manifest.networks) {
            const fs::path file = staging / detail::path_of(network.file);
            std::ifstream in(file, std::ios::binary);
            if (!in) {
                problem = problem_of(Refusal::network_missing, network.file);
                return false;
            }
            sha::Hasher hasher;
            while (in) {
                in.read(
                    reinterpret_cast<char*>(piece.data()),
                    static_cast<std::streamsize>(piece.size())
                );
                const std::streamsize count = in.gcount();
                if (count > 0) {
                    sha::update(
                        hasher,
                        std::span<const uint8_t>(piece.data(), static_cast<std::size_t>(count))
                    );
                }
                if (in.bad()) {
                    problem = problem_of(Refusal::network_mismatch, network.file);
                    return false;
                }
            }
            if (!same_sha256(sha::to_hex(sha::finish(hasher)), network.sha256)) {
                problem = problem_of(Refusal::network_mismatch, network.file);
                return false;
            }
        }
        return true;
    } catch (const std::exception&) {
        problem = problem_of(Refusal::manifest_errors, {}, "Its oabrain.yaml cannot be read.");
        return false;
    }
}

} // namespace oa::app::package_install::oabrain
