// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Game directory resolution and installation checks for oa-game.
#include "oa/app/game_directory.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/platform/files.hpp"
#include <cctype>
#include <iostream>
#include <stdexcept>
#include <system_error>

namespace oa::app {
namespace {

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
    return GameDirectory{
        folder,
        std::move(install.archives),
        source,
        std::move(installation),
        std::move(install.demo),
        std::move(folders),
        std::move(install.profile),
        std::move(install.profile_warnings)
    };
}

/// Reports that the in-engine chooser should open: the usable folders found, the remembered
/// folder that has gone and why, the platform's folder and why, whether the system's dialog
/// may be offered beside it and why it failed.
///
/// @param request whether the chooser comes before the dialog (Game Mode)
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
    // files of both.
    const auto plan = discovery_plan_of(install.profile.get());
    oa::AssetStore probe(install.folders);
    for (const auto& outcome : probe.discover(plan)) {
        if (outcome.mounted || outcome.already_mounted)
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
            if (usable(install))
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
            if (usable(install))
                return resolved(folder, std::move(install), GameDirectorySource::stored);
            stored = RefusedFolder{
                folder,
                describe_install_problem(install),
                install.folder || !install.problem.empty()
            };
        }
    }
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
        if (usable(install))
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
    if (request.import_offered && needed != nullptr && !request.choose) {
        if (request.stored && !request.stored->empty()) {
            const auto stored = path_from_utf8(*request.stored);
            auto install = inspect_folder(host, stored);
            if (usable(install))
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
        if (!usable(install))
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
            if (usable(install))
                return resolved(start, std::move(install), GameDirectorySource::stored);
            stored_refused = RefusedFolder{
                start, describe_install_problem(install), install.folder || !install.problem.empty()
            };
        }
    }
    // The folders found on this machine fill the gap the dialog fills: one
    // usable folder is played and remembered; several, or a remembered one
    // that has gone, are offered in the in-engine chooser, as is a start
    // where the system's dialog may not show (Game Mode) or does not exist.
    const bool chooser = request.chooser_offered && needed != nullptr;
    std::vector<FoundInstall> usable_found;
    std::optional<GameDirectory> only_found;
    if (!request.choose || chooser) {
        for (const FoundInstall& candidate : request.found) {
            auto install = inspect_folder(host, candidate.folder);
            if (!usable(install))
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
    if (chooser && (request.chooser_first || !dialog || usable_found.size() > 1 ||
                    (stored_refused && !usable_found.empty()))) {
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
            if (usable(install))
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
