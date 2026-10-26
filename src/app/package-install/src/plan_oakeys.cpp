// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The oakeys kind's own rules: its manifest, the files a key layout may
// hold, its icon, the engine it needs, what this build cannot use, what a
// folder of Keys holds, and where a layout installs. Nothing is written.

#include "files.hpp"

#include "oa/app/package_install/oakeys.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/data/key_library/library.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/formats/oakeys/manifest.hpp"
#include "oa/formats/png.hpp"
#include "oa/ui/key_bindings/layout_file.hpp"

#include <cstdint>
#include <exception>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef OA_ENGINE_VERSION
#error                                                                                             \
    "OA_ENGINE_VERSION names this build's version, which a key layout's engine requirement is checked against"
#endif

namespace oa::app::package_install::oakeys {

namespace fs = std::filesystem;
namespace layouts = oa::formats::oakeys;
namespace png = oa::formats::png;
namespace engine = oa::formats::oamod;
namespace bindings = oa::ui::key_bindings;
namespace library = oa::data::key_library;

static_assert(keys_folder_name == oa::data::key_library::keys_folder_name);

namespace {

/// How many things this build cannot use the question lists before "and {n} more".
constexpr std::size_t shown_cannot_use = 3;

/// A cannot_use line the prompt re-translates. The count follows the newline.
constexpr std::string_view cannot_use_more = "and {n} more\n";

/// The most bytes of an origin record read to recognise the player's own changes.
constexpr std::size_t origin_most_bytes = 4096;

/// The release this build is, parsed once from OA_ENGINE_VERSION.
///
/// @return that release; 0.0.0 when the macro is not a version
engine::EngineVersion this_build_version() {
    static const engine::EngineVersion version = [] {
        if (const std::optional<engine::EngineVersion> parsed =
                engine::parse_engine_version(OA_ENGINE_VERSION))
            return *parsed;
        return engine::EngineVersion{};
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

/// Tells whether a folder holds the incoming layout, at any version.
///
/// @param held what the folder holds
/// @param incoming the layout
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

/// Reads a folder's oakeys.yaml.
///
/// @param folder the folder
/// @param[out] manifest the manifest, when it was read
/// @return true when the folder holds a readable manifest
bool read_folder_manifest(const fs::path& folder, layouts::Manifest& manifest) {
    const auto found = entry_without_case(folder, layouts::manifest_file);
    if (!found)
        return false;
    std::ifstream in(*found, std::ios::binary);
    if (!in)
        return false;
    std::vector<uint8_t> bytes;
    for (std::istreambuf_iterator<char> at{in}, end;
         at != end && bytes.size() <= engine::max_input_bytes;
         ++at)
        bytes.push_back(static_cast<uint8_t>(*at));
    if (bytes.size() > engine::max_input_bytes)
        return false;
    std::vector<layouts::Problem> problems;
    return layouts::read_manifest(bytes, manifest, problems);
}

/// Fills what a folder holds from a manifest it gave.
///
/// @param manifest the manifest
/// @return the folder, of kind package; revision is 0
InstalledPackage installed_of(const layouts::Manifest& manifest) {
    InstalledPackage installed{};
    installed.kind = FolderKind::package;
    installed.id = manifest.id;
    installed.name = manifest.name;
    installed.version = oa::formats::oamut::version_text(manifest.version);
    installed.revision = 0;
    installed.author = manifest.author.name;
    installed.description = manifest.description;
    return installed;
}

/// The sentence for an engine requirement this build does not meet.
///
/// @param range the requirement
/// @return the wording a map pack uses for the same check
std::string unmet_engine(const engine::EngineRange& range) {
    return "needs Open Annihilation " + engine::describe_engine_range(range) +
           "; this is Open Annihilation " + engine::engine_version_text(this_build_version());
}

/// Tells whether a folder's name is kept for the player's own changes.
///
/// @param folder the folder
/// @return true when the name starts with own-
bool own_folder_name(const fs::path& folder) {
    const std::string name = detail::utf8_of(folder.filename());
    return name.starts_with("own-");
}

/// Tells whether a folder's origin record says the player's own changes.
///
/// The installer's origin reader knows only file and catalogue, so an own
/// record looks like no record there. This reads the record itself.
///
/// @param folder the folder
/// @return true when the record says origin: own
bool origin_says_own(const fs::path& folder) {
    const auto found = entry_without_case(folder, ".oa-origin.yaml");
    if (!found)
        return false;
    std::ifstream in(*found, std::ios::binary);
    if (!in)
        return false;
    std::vector<uint8_t> bytes;
    for (std::istreambuf_iterator<char> at{in}, end; at != end && bytes.size() <= origin_most_bytes;
         ++at)
        bytes.push_back(static_cast<uint8_t>(*at));
    if (bytes.size() > origin_most_bytes)
        return false;
    engine::Node root;
    engine::ReadError read_error{};
    if (!engine::read_document(bytes, root, read_error) || root.kind != engine::NodeKind::mapping)
        return false;
    const engine::Node* origin = engine::find_entry(root, "origin");
    return origin != nullptr && origin->kind == engine::NodeKind::string && origin->text == "own";
}

/// The action a warning names, without the bindings prefix or a chord index.
///
/// @param key the problem's path
/// @return the action, or the path when it is not a binding
std::string shown_action(std::string key) {
    constexpr std::string_view prefix = "bindings.";
    if (key.starts_with(prefix))
        key.erase(0, prefix.size());
    const std::size_t bracket = key.rfind('[');
    if (bracket != std::string::npos && !key.empty() && key.back() == ']') {
        bool digits = bracket + 1 < key.size() - 1;
        for (std::size_t index = bracket + 1; digits && index + 1 < key.size(); ++index)
            digits = key[index] >= '0' && key[index] <= '9';
        if (digits)
            key.erase(bracket);
    }
    return key;
}

/// One warning, as the install question lists it.
///
/// @param problem the warning
/// @return the sentence
std::string warning_sentence(const bindings::LayoutProblem& problem) {
    return shown_action(problem.key) + ": " + problem.message;
}

/// One layout error, in the manifest's diagnostic form.
///
/// @param problem the error
/// @return oakeys.yaml:line:column: key: message
std::string layout_error_line(const bindings::LayoutProblem& problem) {
    return std::string(layouts::manifest_file) + ":" + std::to_string(problem.line) + ":" +
           std::to_string(problem.column) + ": " + problem.key + ": " + problem.message;
}

/// A plan that refuses the target and leaves it as it is.
///
/// @param incoming the layout
/// @param hooks what the folder holds
/// @return the refusal
InstallPlan refuse_target(const Incoming& incoming, const FolderHooks& hooks) {
    InstallPlan plan{};
    plan.kind = PlanKind::refuse;
    plan.refusal = Refusal::not_a_pack;
    plan.target = incoming.id;
    plan.installed = look(hooks, incoming.id);
    return plan;
}

} // namespace

bool read_manifest(
    std::span<const uint8_t> manifest_bytes,
    const ManifestContext& context,
    Package& package,
    Problem& problem
) {
    layouts::Manifest manifest;
    std::vector<layouts::Problem> problems;
    if (!layouts::read_manifest(manifest_bytes, manifest, problems)) {
        problem = problem_of(Refusal::manifest_errors);
        for (const layouts::Problem& one : problems)
            problem.lines.push_back(layouts::describe(one));
        return false;
    }
    if (manifest.id.starts_with("own-")) {
        problem = problem_of(Refusal::reserved_id, manifest.id);
        return false;
    }
    if (windows_device_name(manifest.id)) {
        problem = problem_of(Refusal::reserved_id, manifest.id);
        return false;
    }
    package.incoming.id = manifest.id;
    package.incoming.name = manifest.name;
    package.incoming.version = oa::formats::oamut::version_text(manifest.version);
    package.incoming.revision = 0;

    bool held_icon = manifest.icon.empty();
    for (const PackagedFile& file : package.files) {
        if (directory_name(file.name))
            continue;
        if (library::file_role(file.name, manifest.icon) == library::FileRole::outside) {
            problem = problem_of(Refusal::outside_layout, file.name);
            return false;
        }
        if (!manifest.icon.empty() && file.name == manifest.icon)
            held_icon = true;
    }

    if (!manifest.icon.empty()) {
        if (!held_icon || context.read_entry == nullptr) {
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

    if (!manifest.requires_engine.empty()) {
        const std::optional<engine::EngineRange> range =
            engine::parse_engine_range(manifest.requires_engine);
        if (!range || !engine::engine_range_met(*range, this_build_version())) {
            problem = problem_of(
                Refusal::engine_unmet,
                manifest.requires_engine,
                range ? unmet_engine(*range) : std::string("Its engine requirement cannot be read.")
            );
            return false;
        }
    }

    bindings::Layout layout;
    std::vector<bindings::LayoutProblem> layout_problems;
    const bool parsed = bindings::layout_from_manifest(manifest, nullptr, layout, layout_problems);
    if (!parsed) {
        problem = problem_of(Refusal::manifest_errors);
        for (const bindings::LayoutProblem& one : layout_problems) {
            if (!one.warning)
                problem.lines.push_back(layout_error_line(one));
        }
        if (problem.lines.empty())
            problem.lines.push_back(
                std::string(layouts::manifest_file) + ": the layout cannot be read"
            );
        return false;
    }
    std::size_t warnings = 0;
    std::size_t extra = 0;
    for (const bindings::LayoutProblem& one : layout_problems) {
        if (!one.warning)
            continue;
        if (warnings < shown_cannot_use) {
            package.incoming.cannot_use.push_back(warning_sentence(one));
            ++warnings;
        } else {
            ++extra;
        }
    }
    if (extra > 0)
        package.incoming.cannot_use.push_back(std::string(cannot_use_more) + std::to_string(extra));
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
        // A folder the player keeps for their own changes is never a package,
        // even when its oakeys.yaml would otherwise read.
        if (own_folder_name(folder) || origin_says_own(folder))
            return installed;
        layouts::Manifest manifest;
        if (read_folder_manifest(folder, manifest) && !manifest.id.empty())
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
    // No package takes an id kept for the player's own changes. The folder,
    // when there is one, is left as it is.
    if (incoming.id.starts_with("own-"))
        return refuse_target(incoming, hooks);
    const std::string target = incoming.id;
    const InstalledPackage at_target = look(hooks, target);
    if (at_target.kind == FolderKind::missing) {
        InstallPlan plan{};
        plan.target = target;
        // A file the player opened asks before a new folder is made. The
        // Install button answers alongside, which the runtime installs into
        // `alongside`; that folder is this id, and it is missing.
        if (incoming.origin.kind == OriginKind::catalogue) {
            plan.kind = PlanKind::install;
            return plan;
        }
        plan.kind = PlanKind::ask_install;
        plan.alongside = target;
        return plan;
    }
    if (!holds(at_target, incoming)) {
        InstallPlan plan{};
        plan.kind = PlanKind::refuse;
        plan.refusal = Refusal::not_a_pack;
        plan.target = target;
        plan.installed = at_target;
        return plan;
    }
    const bool incoming_catalogue = incoming.origin.kind == OriginKind::catalogue;
    const bool held_catalogue =
        at_target.origin.has_value() && at_target.origin->kind == OriginKind::catalogue;
    if (incoming_catalogue && held_catalogue &&
        at_target.origin->registry == incoming.origin.registry) {
        InstallPlan plan{};
        plan.kind = PlanKind::install;
        plan.replacing = true;
        plan.target = target;
        plan.installed = at_target;
        plan.backup = backup_of(hooks, target);
        return plan;
    }
    if (incoming_catalogue && held_catalogue) {
        InstallPlan plan{};
        plan.kind = PlanKind::ask_version;
        plan.target = target;
        plan.installed = at_target;
        plan.backup = backup_of(hooks, target);
        return plan;
    }
    if (at_target.version == incoming.version) {
        InstallPlan plan{};
        plan.kind = PlanKind::ask_reinstall;
        plan.target = target;
        plan.installed = at_target;
        plan.backup = backup_of(hooks, target);
        return plan;
    }
    InstallPlan plan{};
    plan.kind = PlanKind::ask_update;
    plan.target = target;
    plan.installed = at_target;
    plan.backup = backup_of(hooks, target);
    return plan;
}

} // namespace oa::app::package_install::oakeys
