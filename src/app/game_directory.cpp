// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Game directory resolution and installation checks for oa-game.
#include "oa/app/game_directory.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/data/content_bundles/record.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/platform/dataless.hpp"
#include "oa/platform/files.hpp"
#include "oa/ui/engine_settings.hpp"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iostream>
#include <mutex>
#include <span>
#include <stdexcept>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

/// Reads of the copied bundles, and the generation those reads belong to.
std::atomic<uint64_t> bundle_walks{0};
std::atomic<uint64_t> bundle_generation{1};

// Opened by the frontend before anything else draws; each 3.1 installation
// carries them in totala1.hpi. Each lies in a directory of the data layout,
// or in palettes when none is named.
struct RequiredResource {
    std::optional<data::defs::DataDirectory> directory;
    std::string_view name;
};

constexpr RequiredResource kRequiredResources[]{
    {data::defs::DataDirectory::guis, "mainmenu.gui"},
    {std::nullopt, "palettes/palette.pal"},
    {data::defs::DataDirectory::gamedata, "sidedata.tdf"},
    {data::defs::DataDirectory::gamedata, "sound.tdf"}
};

constexpr std::string_view kWhatToChoose =
    "Choose the Total Annihilation folder that holds totala1.hpi, or the folder that holds "
    "the installer of the Total Annihilation demo (1997).";

constexpr std::string_view kNoArchives =
    "It holds no Total Annihilation archives (.hpi, .ufo, .ccx or rev31.gp3 files)";

constexpr std::string_view kNeedInstallation =
    "Open Annihilation needs your Total Annihilation installation.";

// The notice's first sentence while it stays up with its look-again button,
// as the player copies the files in.
constexpr std::string_view kNeedFiles = "Open Annihilation needs your Total Annihilation files.";

// What to do where no dialog can ask for the folder, unless the platform
// gives its own advice.
constexpr std::string_view kGameDirAdvice =
    "Name your Total Annihilation folder on the command line:\n\n"
    "open-annihilation --game-dir PATH";

} // namespace

std::string path_length_problem(
    const fs::path& folder, std::size_t names, std::size_t longest, bool long_paths_turned_off
) {
    std::error_code error;
    const auto absolute = fs::absolute(folder, error);
    const std::size_t length = (error ? folder : absolute).native().size();
    if (length + names <= longest)
        return {};
    std::string text = "its path is " + std::to_string(length) + " characters long, and ";
    if (length <= longest)
        text += "the names of the files in it make their paths longer than the " +
                std::to_string(longest) + " characters this system opens. ";
    else
        text += "this system opens paths of at most " + std::to_string(longest) + " characters. ";
    if (long_paths_turned_off)
        text += "Turn on long paths in Windows (Windows 10, version 1607, or later), or move the "
                "folder to one with a shorter path.";
    else
        text += "Move the folder to one with a shorter path.";
    return text;
}

std::string path_length_problem(const fs::path& folder, std::size_t names) {
    return path_length_problem(
        folder, names, oa::platform::longest_path(), oa::platform::long_paths_turned_off()
    );
}

namespace {

[[nodiscard]] bool
accepted_install(const GameDirectoryRequest& request, const GameInstall& install);

[[nodiscard]] bool equal_ignoring_case(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t i = 0; i < left.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(left[i])) !=
            std::tolower(static_cast<unsigned char>(right[i])))
            return false;
    return true;
}

// The scan throws on folders it cannot read, such as a name outside the
// Windows ANSI code page or two names differing only in case; the user is
// told why and asked again like for any other unusable folder.
[[nodiscard]] GameInstall inspect_folder(const GameDirectoryHost& host, const fs::path& folder) {
    try {
        return host.inspect(host.context, folder);
    } catch (const std::exception& error) {
        // A folder whose files' paths are longer than the system opens is
        // reported as such, rather than by the error of the call that failed.
        GameInstall install;
        install.problem = path_length_problem(folder, file_name_room);
        if (install.problem.empty())
            install.problem = error.what();
        return install;
    }
}

// The advice given where no dialog can ask for the folder: the platform's,
// else the command line's.
[[nodiscard]] std::string missing_folder_advice(const GameDirectoryRequest& request) {
    return request.platform_advice.empty() ? std::string(kGameDirAdvice) : request.platform_advice;
}

// A folder resolution looked at and could not play, and why.
struct RefusedFolder {
    fs::path folder;
    std::string problem;
    /// The folder is there: it exists, or it could not be read.
    bool there{};
};

/// Says that no folder can be played and that no dialog can ask for one:
/// which folders were looked at and why each cannot be played, then the
/// advice.
///
/// @param request the platform's advice
/// @param platform the platform's default folder and why it cannot be played; none when it is
///     not there
/// @param stored the folder chosen earlier and why it can no longer be used; none when there is
///     none
/// @param looking_again the notice stays up with its look-again button: it asks for the files
/// @return the notice's text
[[nodiscard]] std::string no_folder_text(
    const GameDirectoryRequest& request,
    const std::optional<RefusedFolder>& platform,
    const std::optional<RefusedFolder>& stored,
    bool looking_again
) {
    std::string text(looking_again ? kNeedFiles : kNeedInstallation);
    if (platform)
        text += "\n\nIt looks for it first in:\n\n" + path_to_utf8(platform->folder) + "\n\n" +
                platform->problem;
    if (stored)
        text += "\n\nThe Total Annihilation folder chosen earlier can no longer be used:\n\n" +
                path_to_utf8(stored->folder) + "\n\n" + stored->problem;
    text += "\n\n" + missing_folder_advice(request);
    return text;
}

// Tells the user that no folder can be played and that no dialog can ask
// for one: which folders were looked at and why each cannot be played,
// then the advice.
void tell_no_folder(
    const GameDirectoryHost& host,
    const GameDirectoryRequest& request,
    const std::optional<RefusedFolder>& platform,
    const std::optional<RefusedFolder>& stored
) {
    host.tell_user(host.context, Notice::warning, no_folder_text(request, platform, stored, false));
}

// The resolved folder: where it lies, its archives and, when it held the
// demo's installer, where the archive was unpacked.
[[nodiscard]] GameDirectory
resolved(const fs::path& folder, GameInstall&& install, GameDirectorySource source) {
    auto installation = install.installation.empty() ? folder : std::move(install.installation);
    auto folders = std::move(install.folders);
    if (folders.empty())
        folders.push_back(installation);
    GameDirectory directory{
        folder,
        std::move(install.archives),
        source,
        std::move(installation),
        std::move(install.demo),
        std::move(folders),
        std::move(install.profile),
        std::move(install.profile_warnings)
    };
    directory.mod_folder = std::move(install.mod_folder);
    directory.mod_layered = !directory.mod_folder.empty();
    return directory;
}

/// Reports that the in-engine chooser should open: the usable folders found, the remembered
/// folder that has gone and why, the platform's folder and why, whether the system's dialog
/// may be offered beside it and why it failed.
///
/// @param request the ask; chooser_first hides the system's dialog (Game Mode)
/// @param platform the platform's default folder and why it cannot be played; none when it is
///     not there
/// @param stored the folder chosen earlier and why it can no longer be used; none when there is
///     none
/// @param found the usable folders found on this machine
/// @param dialog the build offers the system's folder dialog
/// @param dialog_problem why the dialog gave no folder; empty when it did not fail
/// @param[out] needed the report
void ask_for_chooser(
    const GameDirectoryRequest& request,
    const std::optional<RefusedFolder>& platform,
    const std::optional<RefusedFolder>& stored,
    std::vector<FoundInstall> found,
    bool dialog,
    std::string dialog_problem,
    GameFilesNeeded& needed
) {
    needed.needed = true;
    needed.chooser = true;
    needed.found = std::move(found);
    if (platform && platform->there) {
        needed.folder = platform->folder;
        needed.problem = platform->problem;
    }
    if (stored) {
        needed.stored_folder = stored->folder;
        needed.stored_problem = stored->problem;
    }
    needed.dialog_offered = dialog && !request.chooser_first && dialog_problem.empty();
    needed.dialog_problem = std::move(dialog_problem);
}

// Records a probe store's archives as the installation's, and the required
// resources neither they nor the loose files in its folders hold, named in
// the layout of the installation's profile.
void take_mounted(const fs::path& root, const oa::AssetStore& probe, GameInstall& install) {
    const auto mounted = probe.mount_paths();
    install.archives.assign(mounted.begin(), mounted.end());
    install.missing.clear();
    const auto layout = data_layout_of(install.profile.get());
    for (const auto& resource : kRequiredResources) {
        const auto path = resource.directory
                              ? layout.directories[static_cast<std::size_t>(*resource.directory)] +
                                    '/' + std::string(resource.name)
                              : std::string(resource.name);
        if (probe.file_size(path) == 0)
            install.missing.push_back(path);
    }
    install.installation = root;
}

// Runs the archive discovery on the installation's folders into `install`,
// as its profile's layout names the archives. A named installation archive
// that is missing or cannot be opened is also a profile error, so the folder
// cannot be played.
void discover_archives(const fs::path& root, GameInstall& install, const fs::path& profile_file) {
    // The game's discovery scan (AssetStore::discover) decides
    // both which archives are mounted and their lookup precedence: the
    // revision patch, then *.CCX, *.UFO, the installation archives the
    // profile names in the order written, and at most ten *.HPI, each group
    // in Windows name order, then every *.hpi on each CD-ROM root with no
    // limit but with already-mounted paths rejected. A listed name is left
    // out of those groups. The game discs carry totala3.hpi, totala4.hpi and
    // worlds.hpi, which an install without discs keeps in the game directory
    // instead; the game directory then serves as the disc root: the archives
    // past the *.HPI limit mount after everything else, as the disc copies
    // would. A scratch store runs the scan so main() mounts the same archives
    // in the same order. --archive bypasses this. A mod folder layers over
    // the folder: discovery runs over both, as over one folder holding the
    // files of both. An empty file in an archive's place hides the archive
    // of its name on purpose (oamod-standard 4.1), so it is not skipped.
    const auto plan = discovery_plan_of(install.profile.get());
    oa::AssetStore probe(install.folders);
    for (const auto& outcome : probe.discover(plan)) {
        if (outcome.mounted || outcome.already_mounted || outcome.hiding)
            continue;
        std::cerr << "open-annihilation: skipping archive " << path_to_utf8(outcome.path.filename())
                  << ": " << outcome.error << '\n';
        install.skipped.push_back(SkippedArchive{outcome.path, outcome.error});
        const std::string name = path_to_utf8(outcome.path.filename());
        bool named = false;
        for (const auto& archive : plan.installation_archives)
            named = named || oa::same_archive_file_name(name, archive);
        if (!named)
            continue;
        // The profile was resolved before discovery, so the line of the name
        // is not kept. The diagnostic names the file and the layout path.
        install.profile_errors.push_back(
            data::mod_profile::format_diagnostic(
                data::mod_profile::Diagnostic{
                    path_to_utf8(profile_file), {}, "layout.installation-archives", outcome.error
                }
            )
        );
    }
    take_mounted(root, probe, install);
}

// Takes `archive` as the only archive of the installation in `root`: the
// demo's checked archive, whatever else its folder holds.
void take_archive(const fs::path& root, const fs::path& archive, GameInstall& install) {
    install.folders = {root};
    oa::AssetStore probe(root);
    std::string error;
    if (!probe.try_mount(archive, &error)) {
        std::cerr << "open-annihilation: skipping archive " << path_to_utf8(archive.filename())
                  << ": " << error << '\n';
        install.skipped.push_back(SkippedArchive{archive, error});
    }
    take_mounted(root, probe, install);
}

/// Tells whether the missing-folder notice looks again instead of ending the
/// start: the platform gave its look-again button and the host can show it.
///
/// @param host the dialogs
/// @param request the button's label
/// @return true when the notice is asked again until a folder is usable
[[nodiscard]] bool looks_again(const GameDirectoryHost& host, const GameDirectoryRequest& request) {
    return host.ask != nullptr && !request.check_again_label.empty() && !request.unattended;
}

/// Shows the missing-folder notice with its look-again button until a folder
/// can be played: after each press the platform's default folder is looked
/// for again and inspected, then the folder chosen earlier, and the first
/// usable one is taken. Otherwise the notice is written again from what was
/// found, the platform's folder named once it is there, and shown again.
///
/// @param host the dialogs, the notice and the inspection
/// @param request the stored folder, the advice and the button's label
/// @param platform the platform's default folder and why it cannot be played, as first found
/// @param stored the folder chosen earlier and why it can no longer be used, as first found
/// @return the usable folder; nullopt when the notice could not be shown
[[nodiscard]] std::optional<GameDirectory> ask_until_usable(
    const GameDirectoryHost& host,
    const GameDirectoryRequest& request,
    std::optional<RefusedFolder> platform,
    std::optional<RefusedFolder> stored
) {
    for (;;) {
        if (!host.ask(
                host.context,
                Notice::warning,
                no_folder_text(request, platform, stored, true),
                request.check_again_label
            ))
            return std::nullopt;
        // The platform's folder may have appeared since the start.
        fs::path platform_folder = request.platform_default;
        if (host.find_platform_default != nullptr) {
            fs::path found;
            platform_folder = host.find_platform_default(host.context, &found) ? found : fs::path{};
        }
        platform.reset();
        if (!platform_folder.empty()) {
            auto install = inspect_folder(host, platform_folder);
            if (accepted_install(request, install))
                return resolved(platform_folder, std::move(install), GameDirectorySource::platform);
            platform = RefusedFolder{
                platform_folder,
                describe_install_problem(install),
                install.folder || !install.problem.empty()
            };
        }
        stored.reset();
        if (request.stored && !request.stored->empty()) {
            const auto folder = path_from_utf8(*request.stored);
            auto install = inspect_folder(host, folder);
            if (accepted_install(request, install))
                return resolved(folder, std::move(install), GameDirectorySource::stored);
            stored = RefusedFolder{
                folder,
                describe_install_problem(install),
                install.folder || !install.problem.empty()
            };
        }
    }
}

// The managed copy's folder names are content_bundles::managed_games_folder
// and managed_expansions_folder. The copy module links this one, so this
// file uses those names and does not call it.

[[nodiscard]] bool directory_named(const fs::path& folder, std::string_view name) {
    return equal_ignoring_case(path_to_utf8(folder.filename()), name);
}

[[nodiscard]] bool has_managed_layout(const fs::path& user) {
    std::error_code games_failure;
    std::error_code expansions_failure;
    return fs::is_directory(
               user / std::string(oa::data::content_bundles::managed_games_folder), games_failure
           ) ||
           fs::is_directory(
               user / std::string(oa::data::content_bundles::managed_expansions_folder),
               expansions_failure
           );
}

[[nodiscard]] fs::path
bundle_directory(const fs::path& user, const oa::data::content_bundles::Bundle& bundle) {
    const fs::path id{std::string(bundle.id)};
    if (bundle.kind == oa::data::content_bundles::Kind::base_game)
        return user / std::string(oa::data::content_bundles::managed_games_folder) / id;
    if (bundle.extends.empty())
        return user / std::string(oa::data::content_bundles::managed_expansions_folder) / id;
    return user / std::string(oa::data::content_bundles::managed_expansions_folder) /
           std::string(bundle.extends[0]) / id;
}

[[nodiscard]] std::vector<std::string> recorded_content_bundles(const fs::path& user) {
    std::vector<std::string> ids;
    std::error_code failure;
    if (user.empty() || !fs::is_directory(user, failure) || failure)
        return ids;
    for (const auto& bundle : oa::data::content_bundles::bundles()) {
        const oa::data::content_bundles::ContentRecord record =
            oa::data::content_bundles::read_bundle_folder_record(
                bundle_directory(user, bundle), nullptr
            );
        if (!oa::data::content_bundles::record_complete(bundle, record))
            continue;
        ids.emplace_back(bundle.id);
    }
    return ids;
}

// The player's folder behind `folder`: the folder itself when it holds
// Games/ or Expansions/, or the folder above a managed bundle folder.
[[nodiscard]] std::optional<fs::path> managed_user_of(const fs::path& folder) {
    if (folder.empty())
        return std::nullopt;
    if (has_managed_layout(folder))
        return folder;
    std::error_code failure;
    if (directory_named(folder.parent_path(), oa::data::content_bundles::managed_games_folder)) {
        const fs::path user = folder.parent_path().parent_path();
        const bool known =
            oa::data::content_bundles::find_bundle(path_to_utf8(folder.filename())) != nullptr;
        const bool recorded = fs::is_regular_file(
            folder / std::string(oa::data::content_bundles::content_record_name), failure
        );
        if ((known || recorded) && has_managed_layout(user))
            return user;
    }
    const fs::path expansions = folder.parent_path().parent_path();
    if (directory_named(expansions, oa::data::content_bundles::managed_expansions_folder)) {
        const fs::path user = expansions.parent_path();
        if (has_managed_layout(user))
            return user;
    }
    return std::nullopt;
}

[[nodiscard]] bool listed_id(const std::vector<std::string>& ids, std::string_view id) {
    for (const std::string& known : ids)
        if (known == id)
            return true;
    return false;
}

/// A folder this start will play. The demo, when that is what was chosen and
/// it is not copied yet, comes only from its installer.
[[nodiscard]] bool
accepted_install(const GameDirectoryRequest& request, const GameInstall& install) {
    if (!usable(install))
        return false;
    if (request.demo_only && install.demo.outcome != DemoOutcome::ready)
        return false;
    return true;
}

} // namespace

std::string describe_archive_problem(const DemoSetup& demo) {
    switch (demo.outcome) {
    case DemoOutcome::not_searched:
        break;
    case DemoOutcome::no_installer:
        return std::string(kNoArchives) +
               " and no installer of the Total Annihilation demo (1997).";
    case DemoOutcome::unrecognised: {
        std::string names;
        for (std::size_t i = 0; i < demo.rejected.size(); ++i) {
            if (i != 0)
                names += i + 1 == demo.rejected.size() ? " and " : ", ";
            names += path_to_utf8(demo.rejected[i].filename());
        }
        return std::string(kNoArchives) + ", and " + names +
               (demo.rejected.size() == 1 ? " is not" : " are not") +
               " the release of the Total Annihilation demo (1997) that Open Annihilation "
               "recognises.";
    }
    case DemoOutcome::ready:
        return "The Total Annihilation demo (1997) unpacked to " + path_to_utf8(demo.archive) +
               " could not be opened.";
    case DemoOutcome::unpack_failed:
        return "It holds the installer of the Total Annihilation demo (1997), " +
               path_to_utf8(demo.installer.filename()) +
               ", but its game data could not be unpacked: " + demo.problem + '.';
    case DemoOutcome::disk_full:
        return "It holds the installer of the Total Annihilation demo (1997), " +
               path_to_utf8(demo.installer.filename()) + ", but the disk is full: " + demo.problem +
               '.';
    }
    return std::string(kNoArchives) + '.';
}

std::string describe_install_problem(const GameInstall& install) {
    if (!install.problem.empty())
        return "It could not be read: " + install.problem;
    if (!install.profile_errors.empty()) {
        std::string text = "Its mod profile cannot be used:";
        for (const auto& error : install.profile_errors)
            text += "\n  " + error;
        return text;
    }
    if (!install.folder)
        return "The folder does not exist.";
    if (install.archives.empty())
        return describe_archive_problem(install.demo);
    std::string text = "Its archives lack ";
    for (std::size_t i = 0; i < install.missing.size(); ++i) {
        if (i != 0)
            text += ", ";
        text += install.missing[i];
    }
    return text + '.';
}

GameInstall inspect_game_install(
    const fs::path& root,
    const fs::path& data_folder,
    const DemoRelease& release,
    const ModChoice& mod,
    const fs::path& overlay
) {
    GameInstall install;
    std::error_code error;
    install.folder = fs::is_directory(root, error);
    if (!install.folder) {
        install.problem = path_length_problem(root);
        return install;
    }
    // Files laid over the folder come first, ahead of the mod folder.
    if (!overlay.empty())
        install.folders.push_back(overlay);
    if (!mod.folder.empty()) {
        if (!fs::is_directory(mod.folder, error)) {
            const auto reason = path_length_problem(mod.folder);
            install.profile_errors.push_back(
                path_to_utf8(mod.folder) + ": the mod folder " +
                (reason.empty() ? "does not exist" : "cannot be read: " + reason)
            );
            return install;
        }
        install.mod_folder = mod.folder;
        install.folders.push_back(mod.folder);
    }
    install.folders.push_back(root);
    // The profile is resolved before any archive is mounted; one that cannot
    // be used stops here and never falls back to the base game.
    auto profile = resolve_folder_profile(install.folders, mod);
    install.profile = std::move(profile.profile);
    install.profile_errors = std::move(profile.errors);
    install.profile_warnings = std::move(profile.warnings);
    if (!install.profile_errors.empty())
        return install;
    discover_archives(root, install, profile.file);
    if (!install.archives.empty() || install.profile)
        return install;
    // No archive mounts from a folder whose files' paths are longer than the
    // system opens; that is the reason given, not a missing archive.
    install.problem = path_length_problem(root, file_name_room);
    if (!install.problem.empty())
        return install;
    // A folder with no archives may hold the demo's installer; its unpacked
    // archive, checked, is the one archive mounted from the folder it was
    // unpacked to.
    install.demo = set_up_demo(root, data_folder, release);
    if (install.demo.outcome == DemoOutcome::ready)
        take_archive(install.demo.folder, install.demo.archive, install);
    return install;
}

bool usable(const GameInstall& install) {
    return install.problem.empty() && install.profile_errors.empty() && install.folder &&
           !install.archives.empty() && install.missing.empty();
}

std::optional<GameDirectory> resolve_game_directory(
    const GameDirectoryRequest& request, const GameDirectoryHost& host, GameFilesNeeded* needed
) {
    // The Game files screen's report says nothing until resolution has
    // found no usable folder with the screen offered.
    if (needed != nullptr)
        *needed = GameFilesNeeded{};
    if (!request.argument.empty()) {
        if (request.archives_named)
            return GameDirectory{
                request.argument,
                {},
                GameDirectorySource::argument,
                request.argument,
                {},
                {},
                {},
                {}
            };
        auto install = inspect_folder(host, request.argument);
        if (install.problem.empty() && !install.folder)
            throw std::runtime_error(
                "game directory does not exist: " + path_to_utf8(request.argument) +
                " (name it with --game-dir PATH)"
            );
        if (!install.problem.empty() || !install.profile_errors.empty() || install.archives.empty())
            throw std::runtime_error(
                "the folder --game-dir names cannot be played: " + path_to_utf8(request.argument) +
                " (" + describe_install_problem(install) + ")"
            );
        return resolved(request.argument, std::move(install), GameDirectorySource::argument);
    }
    // With no dialog in the build, nothing can be chosen.
    const bool dialog = host.pick_folder != nullptr;
    if (request.choose && request.unattended)
        throw std::runtime_error(
            "--choose-game-dir opens a dialog, which CI and the dummy or offscreen video "
            "driver never show"
        );
    if (request.choose && !dialog)
        throw std::runtime_error(
            "--choose-game-dir opens the folder dialog, which this build does not offer; name "
            "the folder with --game-dir PATH"
        );
    // The platform's own folder comes before the stored one: where the
    // platform moves the folders an app keeps, a stored path goes stale.
    std::optional<RefusedFolder> platform_refused;
    if (!request.choose && !request.platform_default.empty()) {
        auto install = inspect_folder(host, request.platform_default);
        if (accepted_install(request, install))
            return resolved(
                request.platform_default, std::move(install), GameDirectorySource::platform
            );
        platform_refused = RefusedFolder{
            request.platform_default,
            describe_install_problem(install),
            install.folder || !install.problem.empty()
        };
    }
    // Where the Game files screen is offered, a start with no usable folder
    // opens it in place of any notice; the folder chosen earlier is still
    // tried first, and an unattended run that checks the screen does not stop.
    if (request.import_offered && needed != nullptr && !request.choose && !request.defer_dialog) {
        if (request.stored && !request.stored->empty()) {
            const auto stored = path_from_utf8(*request.stored);
            auto install = inspect_folder(host, stored);
            if (accepted_install(request, install))
                return resolved(stored, std::move(install), GameDirectorySource::stored);
        }
        needed->needed = true;
        if (platform_refused && platform_refused->there) {
            needed->folder = platform_refused->folder;
            needed->problem = platform_refused->problem;
        }
        return std::nullopt;
    }
    if (request.unattended) {
        if (!request.stored || request.stored->empty()) {
            if (platform_refused)
                throw std::runtime_error(
                    "the default Total Annihilation folder cannot be played: " +
                    path_to_utf8(platform_refused->folder) + " (" + platform_refused->problem +
                    "), none was chosen earlier and nobody can answer the folder dialog in "
                    "this run; name one with --game-dir PATH"
                );
            throw std::runtime_error(
                "no Total Annihilation folder is known and nobody can answer the folder dialog "
                "in this run; name it with --game-dir PATH"
            );
        }
        const auto stored = path_from_utf8(*request.stored);
        auto install = inspect_folder(host, stored);
        if (!accepted_install(request, install))
            throw std::runtime_error(
                "the Total Annihilation folder chosen earlier can no longer be used: " +
                path_to_utf8(stored) + " (" + describe_install_problem(install) +
                "); name one with --game-dir PATH"
            );
        return resolved(stored, std::move(install), GameDirectorySource::stored);
    }
    fs::path start;
    std::optional<RefusedFolder> stored_refused;
    if (request.stored && !request.stored->empty()) {
        start = path_from_utf8(*request.stored);
        if (!request.choose) {
            auto install = inspect_folder(host, start);
            if (accepted_install(request, install))
                return resolved(start, std::move(install), GameDirectorySource::stored);
            stored_refused = RefusedFolder{
                start, describe_install_problem(install), install.folder || !install.problem.empty()
            };
        }
    }
    // One usable folder is played and remembered. Otherwise the system's
    // dialog asks, on macOS, Windows and a Linux desktop whose portal is
    // there. The locator asks only in Steam's Game Mode, in a build with no
    // dialog, and when that dialog cannot open.
    const bool chooser = request.chooser_offered && needed != nullptr;
    std::vector<FoundInstall> usable_found;
    std::optional<GameDirectory> only_found;
    if (!request.choose || chooser) {
        for (const FoundInstall& candidate : request.found) {
            auto install = inspect_folder(host, candidate.folder);
            if (!accepted_install(request, install))
                continue;
            if (usable_found.empty()) {
                only_found =
                    resolved(candidate.folder, std::move(install), GameDirectorySource::found);
                only_found->found_from = candidate;
            }
            usable_found.push_back(candidate);
        }
    }
    if (!request.choose && usable_found.size() == 1 && !stored_refused)
        return only_found;
    // The five-step setup asks where the game is. While it will run, nothing
    // here opens the system's folder dialog, the in-engine locator or the
    // Game files screen.
    if (request.defer_dialog)
        return std::nullopt;
    // Game Mode, and a build with no dialog, open the locator and never the
    // system's dialog. Everywhere else the dialog asks first; when it cannot
    // open, the loop below hands the same folders to the locator.
    if (chooser && (request.chooser_first || !dialog)) {
        ask_for_chooser(
            request, platform_refused, stored_refused, std::move(usable_found), dialog, {}, *needed
        );
        return std::nullopt;
    }
    if (stored_refused) {
        if (!dialog) {
            // The platform's look-again button keeps the notice up until
            // a folder can be played.
            if (looks_again(host, request))
                return ask_until_usable(host, request, platform_refused, *stored_refused);
            tell_no_folder(host, request, platform_refused, stored_refused);
            return std::nullopt;
        }
        host.tell_user(
            host.context,
            Notice::information,
            "The Total Annihilation folder chosen earlier can no longer be used:\n\n" +
                path_to_utf8(start) + "\n\n" + stored_refused->problem +
                "\n\nChoose the folder again."
        );
    } else if (!request.choose) {
        if (!dialog) {
            if (looks_again(host, request))
                return ask_until_usable(host, request, platform_refused, std::nullopt);
            tell_no_folder(host, request, platform_refused, std::nullopt);
            return std::nullopt;
        }
        // The dialog opens where the platform keeps the game, or nearest it.
        std::string looked_at;
        if (platform_refused) {
            start = platform_refused->folder;
            looked_at = "\nIt looks for it first in:\n\n" + path_to_utf8(platform_refused->folder) +
                        "\n\n" + platform_refused->problem + '\n';
        }
        host.tell_user(
            host.context,
            Notice::information,
            std::string(kNeedInstallation) + "\n" + looked_at + "\n" + std::string(kWhatToChoose) +
                "\nIt is remembered for later starts; --choose-game-dir changes it."
        );
    }
    // Windows reports a choice with no folder on disk behind it (This PC, a
    // library) as a failure, so a failure opens the dialog once more; where
    // there is no dialog the second attempt fails at once.
    bool failed_before = false;
    for (;;) {
        fs::path chosen;
        std::string error;
        const auto pick = host.pick_folder(host.context, start, &chosen, &error);
        if (pick == FolderPick::unavailable && !failed_before) {
            failed_before = true;
            continue;
        }
        failed_before = false;
        switch (pick) {
        case FolderPick::chosen: {
            auto install = inspect_folder(host, chosen);
            if (accepted_install(request, install))
                return resolved(chosen, std::move(install), GameDirectorySource::chosen);
            host.tell_user(
                host.context,
                Notice::warning,
                "This folder does not hold a Total Annihilation installation:\n\n" +
                    path_to_utf8(chosen) + "\n\n" + describe_install_problem(install) + "\n\n" +
                    std::string(kWhatToChoose)
            );
            start = chosen;
            break;
        }
        case FolderPick::cancelled:
            host.tell_user(
                host.context,
                Notice::information,
                "Open Annihilation cannot start without your Total Annihilation folder.\n"
                "Start it again to choose the folder, or name it on the command line:\n\n"
                "open-annihilation --game-dir PATH"
            );
            return std::nullopt;
        case FolderPick::unavailable:
            // Where the in-engine chooser can be shown, it takes over from
            // a dialog that cannot open, and says why.
            if (chooser) {
                ask_for_chooser(
                    request,
                    platform_refused,
                    stored_refused,
                    std::move(usable_found),
                    dialog,
                    error.empty() ? std::string("it gave no reason") : error,
                    *needed
                );
                return std::nullopt;
            }
            host.tell_user(
                host.context,
                Notice::warning,
                "Open Annihilation could not get a folder from the system's folder dialog:\n" +
                    error + "\n\n" + missing_folder_advice(request)
            );
            return std::nullopt;
        }
    }
}

bool unattended_environment(std::string_view ci, std::string_view video_drivers) {
    if (!ci.empty())
        return true;
    const auto first = video_drivers.substr(0, video_drivers.find(','));
    return equal_ignoring_case(first, "dummy") || equal_ignoring_case(first, "offscreen");
}

std::string dialog_location(const fs::path& start) {
    if (start.empty())
        return {};
    std::error_code error;
    auto folder = fs::absolute(start, error);
    if (error)
        return {};
    while (!fs::is_directory(folder, error)) {
        const auto parent = folder.parent_path();
        if (parent.empty() || parent == folder)
            return {};
        folder = parent;
    }
    auto text = path_to_utf8(folder.make_preferred());
    if (text.back() != static_cast<char>(fs::path::preferred_separator))
        text += static_cast<char>(fs::path::preferred_separator);
    return text;
}

fs::path preference_file(const std::optional<fs::path>& explicit_file) {
    return explicit_file ? *explicit_file : oa::platform::preferences::default_file();
}

std::optional<std::string> stored_game_directory(const oa::platform::preferences::Values& values) {
    const auto found = values.find(std::string(kGameDirectoryPreference));
    if (found == values.end())
        return std::nullopt;
    return found->second;
}

void remember_game_directory(oa::platform::preferences::Values& values, const fs::path& folder) {
    values[std::string(kGameDirectoryPreference)] =
        path_to_utf8(fs::absolute(folder).lexically_normal());
}

fs::path chosen_mod_directory(
    const fs::path& mod_dir, bool base_game, const oa::platform::preferences::Values& values
) {
    if (!mod_dir.empty())
        return mod_dir;
    if (base_game)
        return {};
    const auto found = values.find(std::string(mod_directory_preference));
    if (found == values.end() || found->second.empty())
        return {};
    return path_from_utf8(found->second);
}

void remember_mod_directory(oa::platform::preferences::Values& values, const fs::path& folder) {
    if (folder.empty()) {
        values.erase(std::string(mod_directory_preference));
        return;
    }
    values[std::string(mod_directory_preference)] =
        path_to_utf8(fs::absolute(folder).lexically_normal());
}

uint64_t copied_content_bundles_walks() noexcept {
    return bundle_walks.load(std::memory_order_relaxed);
}

uint64_t content_bundles_generation() noexcept {
    return bundle_generation.load(std::memory_order_relaxed);
}

void note_content_bundles_changed() noexcept {
    bundle_generation.fetch_add(1, std::memory_order_relaxed);
}

std::vector<std::string> copied_content_bundles(const fs::path& folder) {
    bundle_walks.fetch_add(1, std::memory_order_relaxed);
    if (const std::optional<fs::path> managed = managed_user_of(folder))
        return recorded_content_bundles(*managed);
    std::vector<std::string> ids;
    if (folder.empty())
        return ids;
    std::error_code error;
    if (!fs::is_directory(folder, error) || error)
        return ids;
    std::vector<std::pair<std::string, uint64_t>> owned;
    const fs::recursive_directory_iterator end;
    for (fs::recursive_directory_iterator cursor(
             folder, fs::directory_options::skip_permission_denied, error
         );
         !error && cursor != end;
         cursor.increment(error)) {
        if (!cursor->is_regular_file(error) || error) {
            error.clear();
            continue;
        }
        const fs::path relative = cursor->path().lexically_relative(folder);
        if (relative.empty() || *relative.begin() == "..")
            continue;
        std::error_code size_error;
        const auto bytes = cursor->file_size(size_error);
        if (!size_error) {
            const auto spelling = relative.generic_u8string();
            owned.emplace_back(
                std::string(spelling.begin(), spelling.end()), static_cast<uint64_t>(bytes)
            );
        }
        error.clear();
    }
    std::vector<oa::data::content_bundles::SourceFile> sources;
    sources.reserve(owned.size());
    for (const std::pair<std::string, uint64_t>& entry : owned)
        sources.push_back({entry.first, entry.second});
    for (const std::string_view id : oa::data::content_bundles::present_bundles(sources))
        ids.emplace_back(id);
    return ids;
}

std::vector<std::string> registry_content_bundles(const fs::path& user) {
    std::vector<std::string> ids;
    std::error_code failure;
    if (user.empty() || !fs::is_directory(user, failure) || failure)
        return ids;
    constexpr std::string_view registry_source = "registry ";
    for (const auto& bundle : oa::data::content_bundles::bundles()) {
        const oa::data::content_bundles::ContentRecord record =
            oa::data::content_bundles::read_bundle_folder_record(
                bundle_directory(user, bundle), nullptr
            );
        if (!record.valid || record.bundle_id != bundle.id)
            continue;
        if (record.source.rfind(registry_source, 0) != 0)
            continue;
        ids.emplace_back(bundle.id);
    }
    return ids;
}

namespace {

struct ArchiveWaitState {
    ArchiveWait progress{};
    bool reporting{};
    EvictedArchiveHold hold{};
    void* hold_context{};
    oa::platform::FileResidenceProbe probe{};
    void* probe_context{};
    mutable std::mutex line_mutex;
    std::string line;
};

ArchiveWaitState& archive_wait_state() {
    static ArchiveWaitState state;
    return state;
}

/// Reads one byte of an evicted file, after the check's hold. The read is
/// what makes macOS download the file.
void read_evicted_byte(const fs::path& path) {
    const ArchiveWaitState& wait = archive_wait_state();
    if (wait.hold != nullptr)
        wait.hold(path, wait.hold_context);
    std::ifstream input(path, std::ios::binary);
    char byte = 0;
    if (input)
        input.read(&byte, 1);
}

/// Counts the files that are not on this computer and reads one byte of each
/// on macOS, so the download happens on this thread. The dialog's line and
/// bar follow the count. Only a compose that shows its wait looks, and only
/// on a system that keeps files elsewhere.
void fetch_evicted_archives(const std::vector<fs::path>& folders) {
    const ArchiveWaitState& wait = archive_wait_state();
    if (!wait.reporting || !oa::platform::host_reports_dataless_files())
        return;
    const oa::platform::FileResidenceProbe probe =
        wait.probe != nullptr ? wait.probe : oa::platform::system_file_residence;
    std::vector<fs::path> evicted;
    std::ignore =
        oa::platform::count_evicted_files(folders, probe, wait.probe_context, &evicted, nullptr);
    if (evicted.empty())
        return;
    if (wait.progress.line != nullptr)
        wait.progress.line(wait.progress.context, icloud_download_line);
    {
        std::lock_guard lock(archive_wait_state().line_mutex);
        archive_wait_state().line = std::string(icloud_download_line);
    }
    const int32_t whole = static_cast<int32_t>(evicted.size());
    if (wait.progress.amounts != nullptr)
        wait.progress.amounts(wait.progress.context, 0, whole);
    int32_t fetched = 0;
    for (const fs::path& path : evicted) {
        if (wait.progress.stopped != nullptr && wait.progress.stopped(wait.progress.context))
            return;
        read_evicted_byte(path);
        ++fetched;
        if (wait.progress.amounts != nullptr)
            wait.progress.amounts(wait.progress.context, fetched, whole);
    }
}

} // namespace

void set_archive_wait(const ArchiveWait* progress) noexcept {
    ArchiveWaitState& state = archive_wait_state();
    if (progress == nullptr) {
        state.progress = {};
        state.reporting = false;
        std::lock_guard lock(state.line_mutex);
        state.line.clear();
        return;
    }
    state.progress = *progress;
    state.reporting = true;
}

void set_evicted_archive_hold(EvictedArchiveHold hold, void* context) noexcept {
    archive_wait_state().hold = hold;
    archive_wait_state().hold_context = context;
}

void set_archive_residence_probe(oa::platform::FileResidenceProbe probe, void* context) noexcept {
    archive_wait_state().probe = probe;
    archive_wait_state().probe_context = context;
}

std::string archive_wait_line() {
    const ArchiveWaitState& state = archive_wait_state();
    std::lock_guard lock(state.line_mutex);
    return state.line;
}

void plant_icloud_wait_copy(const fs::path& user) {
    const oa::data::content_bundles::Bundle* base =
        oa::data::content_bundles::find_bundle(oa::data::content_bundles::default_base_game_id);
    if (base == nullptr)
        throw std::runtime_error("icloud wait: the base game is not known");
    const fs::path folder = bundle_directory(user, *base);
    fs::create_directories(folder);
    const std::vector<uint8_t> text{'o', 'k'};
    const oa::HpiWriteFile file{"readme.txt", text};
    const std::vector<uint8_t> archive = oa::write_hpi(std::span<const oa::HpiWriteFile>(&file, 1));
    const std::string hash(64, '0');
    std::ofstream record(
        folder / std::string(oa::data::content_bundles::content_record_name),
        std::ios::binary | std::ios::trunc
    );
    record << "oa-content 1\n";
    record << "bundle " << base->id << '\n';
    record << "source G05\n";
    for (const oa::data::content_bundles::ContentFile& item : base->files) {
        record << "file " << item.copy_name << '\n';
        record << "size 1\n";
        record << "modified 1\n";
        record << "sha256 " << hash << '\n';
        const fs::path placed = folder / path_from_utf8(item.copy_name);
        fs::create_directories(placed.parent_path());
        std::ofstream out(placed, std::ios::binary | std::ios::trunc);
        const std::string_view name = item.copy_name;
        const auto dot = name.rfind('.');
        const std::string_view extension =
            dot == std::string_view::npos ? std::string_view{} : name.substr(dot);
        const bool archive_file = extension == ".hpi" || extension == ".HPI" ||
                                  extension == ".ufo" || extension == ".ccx" || extension == ".gp3";
        if (archive_file)
            out.write(
                reinterpret_cast<const char*>(archive.data()),
                static_cast<std::streamsize>(archive.size())
            );
        else
            out.put('x');
        if (!out)
            throw std::runtime_error("icloud wait: a recorded file was not written");
    }
    if (!record)
        throw std::runtime_error("icloud wait: the content record was not written");
    note_content_bundles_changed();
}

std::optional<GameDirectory> compose_managed_content(
    const ManagedMount& request, const oa::platform::preferences::Values& values
) {
    const std::vector<std::string> recorded = recorded_content_bundles(request.user_folder);
    std::vector<std::string_view> copied;
    copied.reserve(recorded.size());
    for (const std::string& id : recorded)
        copied.emplace_back(id);
    const auto choice = content_choice_from_preferences(values, copied, request.content);
    if (!choice.error.empty())
        throw std::runtime_error(choice.error);
    if (!listed_id(recorded, choice.base_game)) {
        if (!request.content.empty())
            throw std::runtime_error("Open Annihilation has not copied " + choice.base_game + ".");
        return std::nullopt;
    }
    if (!request.content.empty()) {
        for (const std::string& id : choice.expansions)
            if (!listed_id(recorded, id))
                throw std::runtime_error("Open Annihilation has not copied " + id + ".");
    }
    const oa::data::content_bundles::Bundle* base =
        oa::data::content_bundles::find_bundle(choice.base_game);
    if (base == nullptr)
        return std::nullopt;

    std::vector<fs::path> folders;
    fs::path mod = chosen_mod_directory(request.mod_dir, request.base_game, values);
    std::error_code failure;
    if (!mod.empty() && !fs::is_directory(mod, failure))
        mod.clear();
    if (!mod.empty())
        folders.push_back(mod);
    for (const std::string& id : choice.expansions) {
        const oa::data::content_bundles::Bundle* bundle =
            oa::data::content_bundles::find_bundle(id);
        if (bundle == nullptr)
            continue;
        const fs::path expansion = bundle_directory(request.user_folder, *bundle);
        if (fs::is_directory(expansion, failure))
            folders.push_back(expansion);
    }
    const fs::path installation = bundle_directory(request.user_folder, *base);
    folders.push_back(installation);

    ModChoice mod_choice{};
    mod_choice.folder = mod;
    mod_choice.profile_file = request.mod_file;
    mod_choice.accept_unimplemented_hacks = request.accept_unimplemented_hacks;
    mod_choice.preferences = &values;
    auto resolved_profile = resolve_folder_profile(folders, mod_choice);
    if (!resolved_profile.errors.empty()) {
        std::string message = "the mod profile cannot be used:";
        for (const std::string& line : resolved_profile.errors)
            message += "\n  " + line;
        throw std::runtime_error(message);
    }

    GameInstall install{};
    install.folder = true;
    install.folders = folders;
    install.profile = std::move(resolved_profile.profile);
    install.profile_warnings = std::move(resolved_profile.warnings);
    fetch_evicted_archives(folders);
    discover_archives(installation, install, resolved_profile.file);
    if (!install.profile_errors.empty()) {
        std::string message = "the mod profile cannot be used:";
        for (const std::string& line : install.profile_errors)
            message += "\n  " + line;
        throw std::runtime_error(message);
    }
    if (install.archives.empty()) {
        if (!request.content.empty())
            throw std::runtime_error("Open Annihilation has not copied " + choice.base_game + ".");
        return std::nullopt;
    }

    GameDirectory directory{};
    directory.path = installation;
    directory.archives = std::move(install.archives);
    directory.source = GameDirectorySource::managed;
    directory.installation = std::move(install.installation);
    directory.folders = std::move(install.folders);
    directory.profile = std::move(install.profile);
    directory.profile_warnings = std::move(install.profile_warnings);
    directory.mod_folder = std::move(mod);
    directory.mod_layered = !directory.mod_folder.empty();
    return directory;
}

std::string
managed_copy_problem(const ManagedMount& request, const oa::platform::preferences::Values& values) {
    namespace bundles = oa::data::content_bundles;
    const std::vector<std::string> recorded = recorded_content_bundles(request.user_folder);
    std::vector<std::string_view> copied;
    copied.reserve(recorded.size());
    for (const std::string& id : recorded)
        copied.emplace_back(id);
    const auto choice = content_choice_from_preferences(values, copied, request.content);
    if (!choice.error.empty())
        return choice.error;
    const bundles::Bundle* base = bundles::find_bundle(choice.base_game);
    if (base == nullptr || base->kind != bundles::Kind::base_game)
        return "Open Annihilation does not know the base game " + choice.base_game + ".";
    const fs::path folder = bundle_directory(request.user_folder, *base);
    const std::string game(base->english_name);
    const std::string where = path_to_utf8(folder);
    const bundles::ContentRecord record = bundles::read_bundle_folder_record(folder, nullptr);
    if (!record.valid || record.bundle_id != base->id || record.files.empty())
        return "Open Annihilation has not copied " + game + " into " + where +
               ". Start Open Annihilation again to copy it.";
    const std::vector<std::string_view> missing = bundles::files_not_recorded(*base, record);
    if (!missing.empty()) {
        std::string names;
        for (const std::string_view name : missing) {
            if (!names.empty())
                names += ", ";
            names += name;
        }
        return "Open Annihilation's copy of " + game + " in " + where +
               " is not finished: " + names + (missing.size() == 1 ? " was" : " were") +
               " not copied. Start Open Annihilation again to finish it. If the place you copy "
               "from holds a file at another size, copy from another place.";
    }
    return "Open Annihilation's copy of " + game + " in " + where +
           " holds no archive the game can mount. Delete that folder and start Open "
           "Annihilation again to copy the game afresh.";
}

bool roots_hold_movies(std::span<const fs::path> roots) {
    std::error_code failure;
    for (const auto& root : roots)
        for (const auto& folder : fs::directory_iterator(root, failure)) {
            std::string name = path_to_utf8(folder.path().filename());
            std::transform(name.begin(), name.end(), name.begin(), [](unsigned char character) {
                return static_cast<char>(std::tolower(character));
            });
            if (name != "data" || !folder.is_directory(failure))
                continue;
            for (const auto& file : fs::directory_iterator(folder.path(), failure)) {
                std::string extension = path_to_utf8(file.path().extension());
                std::transform(
                    extension.begin(),
                    extension.end(),
                    extension.begin(),
                    [](unsigned char character) {
                        return static_cast<char>(std::tolower(character));
                    }
                );
                if (extension == ".zrb")
                    return true;
            }
        }
    return false;
}

oa::data::content_bundles::ContentChoice content_choice_from_preferences(
    const oa::platform::preferences::Values& values,
    std::span<const std::string_view> copied,
    std::string_view command_line
) {
    namespace bundles = oa::data::content_bundles;
    namespace settings = oa::ui::engine_settings;
    bundles::StoredContent stored;
    std::string base_text;
    std::string expansions_stored;
    const auto base = values.find(std::string(settings::key::base_game));
    if (base != values.end()) {
        base_text = base->second;
        stored.base_game = base_text;
        const auto expansions =
            values.find(std::string(settings::key::expansions_prefix) + base_text);
        if (expansions != values.end()) {
            expansions_stored = expansions->second;
            stored.expansions = expansions_stored;
        }
    }
    return bundles::resolve_content_choice(stored, copied, command_line);
}

void remember_content_choice(
    oa::platform::preferences::Values& values,
    const oa::data::content_bundles::ContentChoice& choice
) {
    namespace settings = oa::ui::engine_settings;
    values[std::string(settings::key::base_game)] = choice.base_game;
    values[std::string(settings::key::expansions_prefix) + choice.base_game] =
        oa::data::content_bundles::expansions_text(choice.expansions);
}

bool store_content_change(
    oa::platform::preferences::Values& values,
    const oa::data::content_bundles::ContentChangeResult& result
) {
    namespace bundles = oa::data::content_bundles;
    namespace settings = oa::ui::engine_settings;
    if (!result.applied)
        return false;
    remember_content_choice(values, result.choice);
    if (result.mod_cleared == bundles::ModClearReason::none)
        return true;
    remember_mod_directory(values, {});
    values.erase(std::string(settings::key::picked_mod_directory));
    if (result.mod_cleared == bundles::ModClearReason::required_expansion_off) {
        values[std::string(settings::key::content_note)] =
            std::string(bundles::required_expansion_note);
    }
    return true;
}

std::optional<GameDirectory>
take_picked_folder(const GameDirectoryHost& host, const fs::path& folder, std::string* problem) {
    auto install = inspect_folder(host, folder);
    if (usable(install))
        return resolved(folder, std::move(install), GameDirectorySource::chosen);
    if (problem != nullptr)
        *problem = describe_install_problem(install);
    return std::nullopt;
}

std::string found_install_notice(const FoundInstall& install) {
    return "Playing Total Annihilation from " +
           std::string(
               oa::platform::game_installs::source_words(install.source, install.removable)
           ) +
           ":\n" + path_to_utf8(install.folder);
}

} // namespace oa::app
