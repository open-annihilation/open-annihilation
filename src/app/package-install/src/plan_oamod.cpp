// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The oamod kind's own rules: its profile resolved from the manifest, what a
// folder of Mods holds, and where a mod package installs.

#include "files.hpp"

#include "oa/app/mod_profile_loader.hpp"
#include "oa/app/package_install/oamod.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/data/content_bundles/content_needs.hpp"

#include <cstdint>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::package_install::oamod {

namespace fs = std::filesystem;
namespace oamod_format = oa::formats::oamod;
namespace mod_profile = oa::data::mod_profile;

namespace {

/// The longest folder-safe version, in bytes.
constexpr std::size_t longest_version_part = 32;

/// Returns a problem.
///
/// @param refusal why
/// @param subject what it names
/// @param detail English, for the log
/// @return the problem
Problem problem_of(Refusal refusal, std::string subject = {}, std::string detail = {}) {
    Problem problem{};
    problem.refusal = refusal;
    problem.subject = std::move(subject);
    problem.detail = std::move(detail);
    return problem;
}

/// Adds every diagnostic of a resolution, formatted, to a list.
///
/// @param diagnostics the diagnostics
/// @param[out] lines receives one line each
void add_lines(
    const std::vector<mod_profile::Diagnostic>& diagnostics, std::vector<std::string>& lines
) {
    for (const auto& diagnostic : diagnostics)
        lines.push_back(mod_profile::format_diagnostic(diagnostic));
}

/// Reads a scalar's text: a string's value, or any other plain scalar as written.
///
/// @param mapping the mapping
/// @param key its key
/// @return the text; empty when there is none
std::string scalar_text(const oamod_format::Node& mapping, std::string_view key) {
    const oamod_format::Node* node = oamod_format::find_entry(mapping, key);
    if (node == nullptr || node->kind == oamod_format::NodeKind::mapping ||
        node->kind == oamod_format::NodeKind::sequence ||
        node->kind == oamod_format::NodeKind::null_value)
        return {};
    return node->text;
}

/// Tells whether a folder holds the incoming mod, at any version.
///
/// @param held what the folder holds
/// @param incoming the mod
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

/// Plans an update or a reinstall of a folder that holds the mod at the
/// incoming version.
///
/// @param hooks the hooks
/// @param incoming the mod
/// @param folder the folder
/// @param held what it holds
/// @return the plan
InstallPlan same_version(
    const FolderHooks& hooks,
    const Incoming& incoming,
    const std::string& folder,
    const InstalledPackage& held
) {
    InstallPlan plan{};
    plan.kind = held.revision == incoming.revision && held.revision != 0 ? PlanKind::ask_reinstall
                                                                         : PlanKind::ask_update;
    plan.target = folder;
    plan.installed = held;
    plan.backup = backup_of(hooks, folder);
    plan.older = plan.kind == PlanKind::ask_update && held.revision != 0 &&
                 incoming.revision < held.revision;
    return plan;
}

} // namespace

bool read_manifest(
    std::span<const uint8_t> manifest,
    const ManifestContext& context,
    Package& package,
    Problem& problem
) {
    mod_profile::ResolveOptions resolve{};
    resolve.accept_unimplemented_hacks = context.options->accept_unimplemented_hacks;
    const auto first = mod_profile::resolve_profile(manifest, context.source, resolve);
    if (!first.resolution) {
        problem = problem_of(Refusal::manifest_errors);
        add_lines(first.errors, problem.lines);
        return false;
    }
    const auto& first_profile = first.resolution->profile;
    // The INI file the profile names, from the package, else the game folder.
    std::vector<uint8_t> ini_bytes;
    const EntryRead ini_read = context.read_entry == nullptr
                                   ? EntryRead::missing
                                   : context.read_entry(
                                         context.reader,
                                         first_profile.identity.settings_file,
                                         mod_ini_most_bytes,
                                         ini_bytes
                                     );
    std::optional<std::string> ini;
    if (ini_read == EntryRead::read)
        ini = std::string(ini_bytes.begin(), ini_bytes.end());
    if (ini_read == EntryRead::missing && !context.options->game_folder.empty())
        resolve.settings = mod_settings_of(
            first_profile, {context.options->game_folder}, context.options->preferences
        );
    else
        resolve.settings = mod_settings_from(
            first_profile,
            ini ? std::optional<std::string_view>(*ini) : std::nullopt,
            context.options->preferences
        );
    const auto second = mod_profile::resolve_profile(manifest, context.source, resolve);
    if (!second.resolution) {
        problem = problem_of(Refusal::manifest_errors);
        add_lines(second.errors, problem.lines);
        return false;
    }
    add_lines(second.warnings, package.warnings);
    package.profile = std::make_shared<const mod_profile::ModProfile>(second.resolution->profile);
    if (windows_device_name(package.profile->id)) {
        problem = problem_of(Refusal::reserved_id, package.profile->id);
        return false;
    }
    package.incoming = incoming_of(*package.profile);
    return true;
}

InstalledPackage read_installed_mod(const fs::path& folder) {
    InstalledPackage installed{};
    try {
        std::error_code error;
        const fs::file_status status = fs::symlink_status(folder, error);
        if (error || !fs::exists(status))
            return installed;
        installed.kind = FolderKind::other;
        if (is_link_or_junction(folder) || !fs::is_directory(status))
            return installed;
        std::string problem;
        const auto manifest = find_mod_profile(folder, problem);
        if (!manifest)
            return installed;
        std::ifstream in(*manifest, std::ios::binary);
        if (!in)
            return installed;
        std::vector<uint8_t> bytes;
        for (std::istreambuf_iterator<char> at{in}, end;
             at != end && bytes.size() <= oamod_format::max_input_bytes;
             ++at)
            bytes.push_back(static_cast<uint8_t>(*at));
        oamod_format::Node root;
        oamod_format::ReadError read{};
        if (!oamod_format::read_document(bytes, root, read))
            return installed;
        installed.id = scalar_text(root, "id");
        if (installed.id.empty())
            return installed;
        installed.name = scalar_text(root, "name");
        installed.version = scalar_text(root, "version");
        installed.description = scalar_text(root, "description");
        if (const oamod_format::Node* author = oamod_format::find_entry(root, "author");
            author != nullptr && author->kind == oamod_format::NodeKind::mapping)
            installed.author = scalar_text(*author, "name");
        if (const oamod_format::Node* packaging = oamod_format::find_entry(root, "packaging"))
            if (const oamod_format::Node* revision =
                    oamod_format::find_entry(*packaging, "revision");
                revision != nullptr && revision->kind == oamod_format::NodeKind::number) {
                int64_t value = 0;
                if (oamod_format::integer_value(revision->number, value))
                    installed.revision = value;
            }
        installed.kind = FolderKind::package;
        const oamod_format::Node* requires_node = oamod_format::find_entry(root, "requires");
        const oamod_format::Node* requires_block =
            requires_node != nullptr && requires_node->kind == oamod_format::NodeKind::mapping
                ? requires_node
                : nullptr;
        oa::data::content_bundles::ContentNeeds needs;
        std::vector<oamod_format::KeyProblem> problems;
        oa::data::content_bundles::read_content_needs(
            root,
            requires_block,
            oa::data::content_bundles::GamesWhenOmitted::totala,
            needs,
            problems
        );
        if (problems.empty()) {
            installed.supported_games = std::move(needs.supported_games);
            installed.required_expansions = std::move(needs.required_expansions);
        } else {
            installed.supported_games = {"totala"};
        }
        installed.origin = read_origin(folder);
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
        InstalledPackage backup = read_installed_mod(*kept);
        if (backup.kind == FolderKind::missing)
            return std::nullopt;
        const InstalledPackage own = read_installed_mod(folder);
        if (backup.kind == FolderKind::package &&
            (own.kind != FolderKind::package || own.id != backup.id))
            backup.kind = FolderKind::other;
        return backup;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

Incoming incoming_of(const mod_profile::ModProfile& profile) {
    return {profile.id, profile.name, profile.version, profile.packaging.revision};
}

std::string version_folder_part(std::string_view version) {
    std::string part;
    for (const char character : version) {
        char kept = '-';
        if (character >= 'A' && character <= 'Z')
            kept = static_cast<char>(character - 'A' + 'a');
        else if (
            (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9') ||
            character == '.' || character == '_'
        )
            kept = character;
        if (kept == '-' && !part.empty() && part.back() == '-')
            continue;
        part += kept;
    }
    const auto trim = [](std::string& text) {
        while (!text.empty() && (text.front() == '.' || text.front() == '-'))
            text.erase(text.begin());
        while (!text.empty() && (text.back() == '.' || text.back() == '-'))
            text.pop_back();
    };
    trim(part);
    if (part.size() > longest_version_part) {
        part.resize(longest_version_part);
        trim(part);
    }
    return part.empty() ? std::string("version") : part;
}

std::vector<std::string> alongside_names(const Incoming& incoming) {
    const std::string first = incoming.id + "-" + version_folder_part(incoming.version);
    std::vector<std::string> names{first};
    for (uint32_t number = 2; number <= alongside_tries; ++number)
        names.push_back(first + "-" + std::to_string(number));
    return names;
}

InstallPlan plan_install(const Incoming& incoming, const FolderHooks& hooks) {
    const std::string own = incoming.id;
    const InstalledPackage at_own = look(hooks, own);
    // Its own folder holds it at this version: an update or a reinstall.
    if (holds(at_own, incoming) && at_own.version == incoming.version)
        return same_version(hooks, incoming, own, at_own);
    // A folder made alongside holds it at this version.
    const std::vector<std::string> alongside = alongside_names(incoming);
    for (const std::string& folder : alongside) {
        const InstalledPackage held = look(hooks, folder);
        if (holds(held, incoming) && held.version == incoming.version)
            return same_version(hooks, incoming, folder, held);
    }
    InstallPlan plan{};
    plan.installed = at_own;
    if (at_own.kind == FolderKind::missing) {
        plan.kind = PlanKind::install;
        plan.target = own;
        return plan;
    }
    for (const std::string& folder : alongside)
        if (look(hooks, folder).kind == FolderKind::missing) {
            plan.alongside = folder;
            break;
        }
    if (plan.alongside.empty()) {
        plan.kind = PlanKind::refuse;
        return plan;
    }
    if (holds(at_own, incoming)) {
        // Another version: replace it, or install this one alongside.
        plan.kind = PlanKind::ask_version;
        plan.target = own;
        plan.backup = backup_of(hooks, own);
        return plan;
    }
    // The folder holds something else, which is never replaced.
    plan.kind = PlanKind::ask_alongside;
    return plan;
}

} // namespace oa::app::package_install::oamod
