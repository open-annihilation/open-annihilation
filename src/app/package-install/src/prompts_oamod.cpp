// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The oamod kind's prompts: the English templates a mod install shows, looked
// up in the interface catalogue, then filled.

#include "prompt_text.hpp"

#include "oa/app/package_install/oamod.hpp"
#include "oa/ui/game_files.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace oa::app::package_install::oamod {

namespace fs = std::filesystem;
namespace settings = oa::ui::engine_settings;

using detail::add_button;
using detail::alongside_caption;
using detail::cancel_caption;
using detail::fill;
using detail::ok_caption;
using detail::open_folder_caption;
using detail::play_now_caption;
using detail::reinstall_caption;
using detail::replace_caption;
using detail::set_keys;
using detail::text_of;
using detail::tr;

namespace {

/// Returns a paragraph that shows a folder's path.
///
/// @param folder the folder
/// @return the paragraph
settings::NoticeParagraph path_of(const fs::path& folder) {
    return detail::shown_path(folder);
}

/// The most profile diagnostics a refusal shows; the log has them all.
constexpr std::size_t shown_diagnostics = 3;

/// Returns the sentence that names the kept version a replace removes.
///
/// @param backup what the .backup holds
/// @return the sentence
std::string backup_removed(const InstalledPackage& backup) {
    if (backup.kind != FolderKind::package)
        return fill(
            "The folder kept for Roll back, which holds no mod that can be read, is removed.", {}
        );
    return fill(
        "The folder kept for Roll back ({backup}) is removed.",
        {{"backup", version_label(backup.version, backup.revision, true)}}
    );
}

/// The sentence for a change to the mod the game plays now.
constexpr std::string_view played_text =
    "The game reloads its data for the mod and returns to the main menu.";

} // namespace

std::string version_label(std::string_view version, int64_t revision, bool with_revision) {
    if (!with_revision)
        return std::string(version);
    if (revision == 0)
        return fill("{version} of an unknown revision", {{"version", std::string(version)}});
    return fill(
        "{version} revision {n}",
        {{"version", std::string(version)}, {"n", std::to_string(revision)}}
    );
}

PackagePrompt installing_prompt(
    const Incoming& incoming,
    uint64_t done_bytes,
    uint64_t total_bytes,
    InstallingPhase phase,
    std::string_view file_name
) {
    PackagePrompt made{};
    made.prompt.title = tr("INSTALLING MOD");
    if (phase == InstallingPhase::checking) {
        made.prompt.paragraphs.push_back(
            text_of(fill("Checking {file}...", {{"file", std::string(file_name)}}))
        );
        made.prompt.paragraphs.push_back(text_of(fill(
            "{done} of {total}",
            {{"done", oa::ui::game_files::size_text(done_bytes)},
             {"total", oa::ui::game_files::size_text(total_bytes)}}
        )));
    } else if (phase == InstallingPhase::placing) {
        made.prompt.paragraphs.push_back(text_of(fill("Putting the files in place...", {})));
    } else {
        made.prompt.paragraphs.push_back(text_of(fill(
            "Unpacking {title} {version}...",
            {{"title", incoming.name}, {"version", incoming.version}}
        )));
        made.prompt.paragraphs.push_back(text_of(fill(
            "{done} of {total}",
            {{"done", oa::ui::game_files::size_text(done_bytes)},
             {"total", oa::ui::game_files::size_text(total_bytes)}}
        )));
    }
    made.prompt.progress =
        total_bytes == 0 || phase == InstallingPhase::placing
            ? settings::prompt_progress_whole
            : static_cast<int32_t>(
                  done_bytes * static_cast<uint64_t>(settings::prompt_progress_whole) / total_bytes
              );
    add_button(made, cancel_caption, Answer::cancel);
    set_keys(made, Answer::cancel, Answer::cancel, Answer::cancel);
    return made;
}

PackagePrompt question_prompt(
    const InstallPlan& plan,
    const Incoming& incoming,
    std::string_view file_name,
    const fs::path& mods,
    bool played
) {
    PackagePrompt made{};
    auto& text = made.prompt.paragraphs;
    const InstalledPackage& installed = plan.installed;
    const std::string title = incoming.name;
    const std::string file(file_name);
    switch (plan.kind) {
    case PlanKind::ask_update: {
        made.prompt.title = tr("UPDATE MOD");
        if (installed.revision == 0)
            text.push_back(text_of(fill(
                "{title} {version}, an unknown revision, is installed. Replace it with "
                "revision "
                "{incoming} from {file}?",
                {{"title", title},
                 {"version", incoming.version},
                 {"incoming", std::to_string(incoming.revision)},
                 {"file", file}}
            )));
        else
            text.push_back(text_of(fill(
                "{title} {version}, revision {installed}, is installed. Replace it with "
                "revision "
                "{incoming} from {file}?",
                {{"title", title},
                 {"version", incoming.version},
                 {"installed", std::to_string(installed.revision)},
                 {"incoming", std::to_string(incoming.revision)},
                 {"file", file}}
            )));
        if (plan.older)
            text.push_back(text_of(fill(
                "Revision {incoming} is older than the one installed.",
                {{"incoming", std::to_string(incoming.revision)}}
            )));
        if (installed.revision == 0)
            text.push_back(text_of(fill(
                "The version installed is kept, and Roll back in the Library brings it back.", {}
            )));
        else
            text.push_back(text_of(fill(
                "Revision {installed} is kept, and Roll back in the Library brings it back.",
                {{"installed", std::to_string(installed.revision)}}
            )));
        if (plan.backup)
            text.push_back(text_of(backup_removed(*plan.backup)));
        if (played)
            text.push_back(text_of(fill(played_text, {})));
        add_button(made, cancel_caption, Answer::cancel);
        add_button(made, replace_caption, Answer::replace, true);
        set_keys(
            made, Answer::cancel, Answer::replace, plan.backup ? Answer::cancel : Answer::replace
        );
        break;
    }
    case PlanKind::ask_version:
        made.prompt.title = tr("ANOTHER VERSION");
        text.push_back(text_of(fill(
            "{title} {installed} is installed. Replace it with {incoming}, or install "
            "{incoming} "
            "alongside it?",
            {{"title", title}, {"installed", installed.version}, {"incoming", incoming.version}}
        )));
        text.push_back(text_of(fill(
            "Replacing keeps {installed}, and Roll back in the Library brings it back.",
            {{"installed", installed.version}}
        )));
        if (plan.backup)
            text.push_back(text_of(backup_removed(*plan.backup)));
        if (played)
            text.push_back(text_of(fill(played_text, {})));
        text.push_back(text_of(fill(
            "Alongside, {incoming} goes in a folder of its own:", {{"incoming", incoming.version}}
        )));
        text.push_back(path_of(mods / detail::path_of(plan.alongside)));
        add_button(made, cancel_caption, Answer::cancel);
        add_button(made, alongside_caption, Answer::alongside);
        add_button(made, replace_caption, Answer::replace, true);
        set_keys(
            made, Answer::cancel, Answer::replace, plan.backup ? Answer::cancel : Answer::replace
        );
        break;
    case PlanKind::ask_reinstall:
        made.prompt.title = tr("ALREADY INSTALLED");
        text.push_back(text_of(fill(
            "{title} {version}, revision {revision}, is already installed. Install its "
            "files "
            "again from {file}?",
            {{"title", title},
             {"version", incoming.version},
             {"revision", std::to_string(incoming.revision)},
             {"file", file}}
        )));
        text.push_back(text_of(fill(
            "Files added to its folder, or changed, since it was installed are replaced or "
            "removed.",
            {}
        )));
        if (plan.backup) {
            if (plan.backup->kind == FolderKind::package)
                text.push_back(text_of(fill(
                    "The version kept, {backup}, stays.",
                    {{"backup", version_label(plan.backup->version, plan.backup->revision, true)}}
                )));
            else
                text.push_back(text_of(fill("The folder kept for Roll back stays.", {})));
        }
        if (played)
            text.push_back(text_of(fill(played_text, {})));
        add_button(made, cancel_caption, Answer::cancel);
        add_button(made, reinstall_caption, Answer::reinstall, true);
        set_keys(made, Answer::cancel, Answer::reinstall, Answer::cancel);
        break;
    case PlanKind::ask_install:
        break;
    case PlanKind::ask_alongside:
    case PlanKind::install:
    case PlanKind::refuse:
        made.prompt.title = tr("FOLDER IN USE");
        text.push_back(text_of(fill(
            "Your Mods folder already has a folder named {folder} that does not hold "
            "{title}. It "
            "is left as it is.",
            {{"folder", incoming.id}, {"title", title}}
        )));
        text.push_back(text_of(fill(
            "Install {title} {version} alongside it, in a folder of its own?",
            {{"title", title}, {"version", incoming.version}}
        )));
        text.push_back(path_of(mods / detail::path_of(plan.alongside)));
        add_button(made, cancel_caption, Answer::cancel);
        add_button(made, alongside_caption, Answer::alongside, true);
        set_keys(made, Answer::cancel, Answer::alongside, Answer::alongside);
        break;
    }
    return made;
}

PackagePrompt installed_prompt(const Incoming& incoming, const fs::path& folder, bool play_now) {
    PackagePrompt made{};
    made.prompt.title = tr("MOD INSTALLED");
    auto& text = made.prompt.paragraphs;
    text.push_back(text_of(fill(
        "{title} {version} is installed in your Mods folder:",
        {{"title", incoming.name}, {"version", incoming.version}}
    )));
    text.push_back(path_of(folder));
    text.push_back(text_of(
        play_now ? fill(
                       "Choose Play now to play it, or choose it later with Play now in the "
                       "Library.",
                       {}
                   )
                 : fill("Choose it with Play now in the Library to play it.", {})
    ));
    add_button(made, open_folder_caption, Answer::open_folder);
    if (play_now)
        add_button(made, play_now_caption, Answer::play_now);
    add_button(made, ok_caption, Answer::ok, true);
    set_keys(made, Answer::ok, Answer::ok, Answer::ok);
    return made;
}

PackagePrompt updated_prompt(
    Change change,
    const Incoming& now,
    const InstalledPackage& before,
    const fs::path& folder,
    const ChangeResult& result,
    bool play_now
) {
    PackagePrompt made{};
    made.prompt.title = tr("MOD UPDATED");
    auto& text = made.prompt.paragraphs;
    const bool same_version = now.version == before.version;
    const std::string new_label = version_label(now.version, now.revision, same_version);
    const std::string old_label = version_label(before.version, before.revision, same_version);
    switch (change) {
    case Change::replace:
    case Change::install:
        if (result.backup_kept)
            text.push_back(text_of(fill(
                "{title} is now {new}. {old} is kept, and Roll back in the Library brings it "
                "back.",
                {{"title", now.name}, {"new", new_label}, {"old", old_label}}
            )));
        else
            text.push_back(
                text_of(fill("{title} is now {new}.", {{"title", now.name}, {"new", new_label}}))
            );
        break;
    case Change::reinstall:
        text.push_back(text_of(fill(
            "The files of {title} {version} were installed again.",
            {{"title", now.name}, {"version", now.version}}
        )));
        break;
    case Change::roll_back:
        if (result.backup_kept)
            text.push_back(text_of(fill(
                "{title} is back to {new}. {old} is kept, and Roll back brings it back.",
                {{"title", now.name}, {"new", new_label}, {"old", old_label}}
            )));
        else
            text.push_back(text_of(
                fill("{title} is back to {new}.", {{"title", now.name}, {"new", new_label}})
            ));
        break;
    }
    text.push_back(path_of(folder));
    if (!result.left_over.empty()) {
        text.push_back(text_of(
            fill("{old} could not be kept as the earlier version; it is in:", {{"old", old_label}})
        ));
        text.push_back(path_of(result.left_over));
    }
    add_button(made, open_folder_caption, Answer::open_folder);
    if (play_now)
        add_button(made, play_now_caption, Answer::play_now);
    add_button(made, ok_caption, Answer::ok, true);
    set_keys(made, Answer::ok, Answer::ok, Answer::ok);
    return made;
}

std::string refusal_text(const Problem& problem) {
    const std::string name = problem.subject;
    const auto size = [](uint64_t bytes) { return oa::ui::game_files::size_text(bytes); };
    switch (problem.refusal) {
    case Refusal::none:
    case Refusal::unreadable:
        return fill("It cannot be read.", {});
    case Refusal::not_zip:
        return fill("It is not a zip archive.", {});
    case Refusal::damaged:
        return name.empty()
                   ? fill("It is damaged.", {})
                   : fill("It is damaged: {name} does not unpack as recorded.", {{"name", name}});
    case Refusal::no_manifest:
        return fill("It holds no oamod.yaml at its top, or in a single folder at its top.", {});
    case Refusal::manifest_too_large:
        return fill("Its oamod.yaml is larger than 256 KiB.", {});
    case Refusal::manifest_errors:
        return fill("Its oamod.yaml has errors:", {});
    case Refusal::unsafe_name:
        return fill("It holds a file it cannot unpack safely: {name}.", {{"name", name}});
    case Refusal::case_clash:
        return fill("It holds two names that differ only in case: {name}.", {{"name", name}});
    case Refusal::link:
        return fill("It holds a link to another file: {name}.", {{"name", name}});
    case Refusal::encrypted:
        return fill("It holds an encrypted file: {name}.", {{"name", name}});
    case Refusal::method:
        return fill(
            "It holds a file packed in a way it cannot unpack: {name}. Pack it again with "
            "standard (deflate) compression.",
            {{"name", name}}
        );
    case Refusal::too_large:
        return fill(
            "It unpacks to {size}, more than the {limit} a mod may take.",
            {{"size", size(problem.size_bytes)}, {"limit", size(problem.limit_bytes)}}
        );
    case Refusal::bomb:
        return fill(
            "It unpacks to {size} from {packed}, far more than a mod's files take.",
            {{"size", size(problem.size_bytes)}, {"packed", size(problem.limit_bytes)}}
        );
    case Refusal::too_many_folders:
        return fill(
            "It holds more than {limit} folders, more than a mod may take.",
            {{"limit", std::to_string(max_package_folders)}}
        );
    case Refusal::no_space:
        return fill(
            "It needs {size} free, and the disk holding your Mods folder has {free}.",
            {{"size", size(problem.size_bytes)}, {"free", size(problem.limit_bytes)}}
        );
    case Refusal::path_too_long:
        return problem.detail;
    case Refusal::reserved_id:
        return fill("Its id, {id}, cannot name a folder on Windows.", {{"id", name}});
    case Refusal::no_free_folder:
        return fill("Your Mods folder has no free folder name for it.", {});
    case Refusal::not_placed:
        return fill(
            "Its files could not be put in place. Another program, such as a file sync or a "
            "virus scanner, may be using its files; try again in a moment.",
            {}
        );
    case Refusal::changed:
        return fill(
            "Your Mods folder changed while this mod was being installed; nothing was changed. "
            "Open the file again.",
            {}
        );
    case Refusal::busy:
        return fill(
            "Another copy of Open Annihilation is changing your Mods folder. Try again once it "
            "has finished.",
            {}
        );
    case Refusal::unknown_kind:
        return fill("It is not a kind of package this game installs.", {});
    default:
        break;
    }
    return fill("It cannot be read.", {});
}

PackagePrompt
refused_prompt(std::string_view file_name, const Problem& problem, bool change_failed) {
    PackagePrompt made{};
    made.prompt.title = tr("MOD NOT INSTALLED");
    auto& text = made.prompt.paragraphs;
    text.push_back(
        text_of(fill("{file} cannot be installed.", {{"file", std::string(file_name)}}))
    );
    text.push_back(text_of(refusal_text(problem)));
    for (std::size_t index = 0; index < problem.lines.size() && index < shown_diagnostics; ++index)
        text.push_back(text_of(problem.lines[index]));
    if (problem.lines.size() > shown_diagnostics)
        text.push_back(text_of(fill(
            "And {n} more; the log has them all.",
            {{"n", std::to_string(problem.lines.size() - shown_diagnostics)}}
        )));
    if (change_failed && problem.refusal != Refusal::changed)
        text.push_back(text_of(fill("Nothing was changed.", {})));
    add_button(made, ok_caption, Answer::ok, true);
    set_keys(made, Answer::ok, Answer::ok, Answer::ok);
    return made;
}

PackagePrompt roll_back_failed_prompt(std::string_view title) {
    PackagePrompt made{};
    made.prompt.title = tr("MOD NOT ROLLED BACK");
    made.prompt.paragraphs.push_back(text_of(fill(
        "{title} could not be rolled back; nothing was changed.", {{"title", std::string(title)}}
    )));
    add_button(made, ok_caption, Answer::ok, true);
    set_keys(made, Answer::ok, Answer::ok, Answer::ok);
    return made;
}

PackagePrompt
roll_back_refused_prompt(std::string_view title, std::string_view to, std::string_view reason) {
    PackagePrompt made{};
    made.prompt.title = tr("MOD NOT ROLLED BACK");
    auto& text = made.prompt.paragraphs;
    text.push_back(text_of(fill(
        "{title} {to}, the version kept for Roll back, cannot be played.",
        {{"title", std::string(title)}, {"to", std::string(to)}}
    )));
    text.push_back(text_of(std::string(reason)));
    text.push_back(text_of(fill("Nothing was changed.", {})));
    add_button(made, ok_caption, Answer::ok, true);
    set_keys(made, Answer::ok, Answer::ok, Answer::ok);
    return made;
}

} // namespace oa::app::package_install::oamod
