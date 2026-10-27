// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The oakeys kind's prompts: the English templates a key-layout install
// shows, looked up in the interface catalogue, then filled. PLAY NOW is
// never offered: installing a layout does not switch it on.

#include "prompt_text.hpp"

#include "oa/app/package_install/oakeys.hpp"
#include "oa/ui/game_files.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app::package_install::oakeys {

namespace fs = std::filesystem;
namespace settings = oa::ui::engine_settings;

using detail::add_button;
using detail::cancel_caption;
using detail::fill;
using detail::ok_caption;
using detail::open_folder_caption;
using detail::reinstall_caption;
using detail::replace_caption;
using detail::set_keys;
using detail::text_of;
using detail::tr;

namespace {

/// The most manifest diagnostics a refusal shows; the log has them all.
constexpr std::size_t shown_diagnostics = 3;

/// A cannot_use line the plan stored for the prompt to re-translate. The
/// count follows the newline. The same spelling as plan_oakeys.cpp.
constexpr std::string_view cannot_use_more = "and {n} more\n";

/// The Install button of a new layout the player opened.
constexpr std::string_view install_caption = "INSTALL";

/// Returns a paragraph that shows a folder's path.
///
/// @param folder the folder
/// @return the paragraph
settings::NoticeParagraph path_of(const fs::path& folder) {
    return detail::shown_path(folder);
}

/// Returns a version as a player reads it.
///
/// @param version the version
/// @param revision its revision
/// @param with_revision true to name the revision
/// @return the text
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

/// Returns the sentence that names the kept version a replace removes.
///
/// @param backup what the .backup holds
/// @return the sentence
std::string backup_removed(const InstalledPackage& backup) {
    if (backup.kind != FolderKind::package)
        return fill("The version kept, which cannot be read, is removed.", {});
    return fill(
        "The version kept ({backup}) is removed.",
        {{"backup", version_label(backup.version, backup.revision, true)}}
    );
}

/// Tells whether a question replaces a layout from another registry.
///
/// @param plan the plan
/// @param incoming the layout
/// @return true when both records are catalogues and their registries differ
bool other_registry(const InstallPlan& plan, const Incoming& incoming) {
    return incoming.origin.kind == OriginKind::catalogue && plan.installed.origin &&
           plan.installed.origin->kind == OriginKind::catalogue &&
           plan.installed.origin->registry != incoming.origin.registry;
}

/// Adds OPEN FOLDER and OK. PLAY NOW is not offered.
///
/// @param[in,out] made the prompt
void add_done_buttons(PackagePrompt& made) {
    add_button(made, open_folder_caption, Answer::open_folder);
    add_button(made, ok_caption, Answer::ok, true);
    set_keys(made, Answer::ok, Answer::ok, Answer::ok);
}

/// Lists what this build cannot use, for a file the player opened.
///
/// @param[in,out] text the question's paragraphs
/// @param incoming the layout
void add_cannot_use(std::vector<settings::NoticeParagraph>& text, const Incoming& incoming) {
    if (incoming.origin.kind == OriginKind::catalogue || incoming.cannot_use.empty())
        return;
    text.push_back(text_of(tr("This version cannot use:")));
    for (const std::string& line : incoming.cannot_use) {
        if (line.starts_with(cannot_use_more))
            text.push_back(
                text_of(fill("and {n} more", {{"n", line.substr(cannot_use_more.size())}}))
            );
        else
            text.push_back(text_of(line));
    }
}

} // namespace

PackagePrompt installing_prompt(
    const Incoming& incoming,
    uint64_t done_bytes,
    uint64_t total_bytes,
    InstallingPhase phase,
    std::string_view file_name
) {
    PackagePrompt made{};
    made.prompt.title = tr("INSTALLING KEY LAYOUT");
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
    const fs::path&,
    bool
) {
    PackagePrompt made{};
    auto& text = made.prompt.paragraphs;
    const InstalledPackage& installed = plan.installed;
    const std::string title = incoming.name;
    const std::string file(file_name);
    switch (plan.kind) {
    case PlanKind::ask_install:
        made.prompt.title = tr("INSTALL KEY LAYOUT");
        text.push_back(text_of(fill(
            "Install the key layout {name} {version}?",
            {{"name", title}, {"version", incoming.version}}
        )));
        add_cannot_use(text, incoming);
        add_button(made, cancel_caption, Answer::cancel);
        add_button(made, install_caption, Answer::alongside, true);
        set_keys(made, Answer::cancel, Answer::alongside, Answer::alongside);
        break;
    case PlanKind::ask_update:
        made.prompt.title = tr("UPDATE KEY LAYOUT");
        text.push_back(text_of(fill(
            "{title} {installed} is installed. Replace it with {incoming} from {file}?",
            {{"title", title},
             {"installed", installed.version},
             {"incoming", incoming.version},
             {"file", file}}
        )));
        text.push_back(
            text_of(fill("The version installed is kept, and can be brought back.", {}))
        );
        if (plan.backup)
            text.push_back(text_of(backup_removed(*plan.backup)));
        add_cannot_use(text, incoming);
        add_button(made, cancel_caption, Answer::cancel);
        add_button(made, replace_caption, Answer::replace, true);
        set_keys(
            made, Answer::cancel, Answer::replace, plan.backup ? Answer::cancel : Answer::replace
        );
        break;
    case PlanKind::ask_reinstall:
        made.prompt.title = tr("REINSTALL KEY LAYOUT");
        text.push_back(text_of(fill(
            "{title} {version}, revision {revision}, is already installed. Install its "
            "files again from {file}?",
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
                text.push_back(text_of(fill("The version kept stays.", {})));
        }
        add_cannot_use(text, incoming);
        add_button(made, cancel_caption, Answer::cancel);
        add_button(made, reinstall_caption, Answer::reinstall, true);
        set_keys(made, Answer::cancel, Answer::reinstall, Answer::cancel);
        break;
    case PlanKind::ask_version:
        made.prompt.title = tr("REPLACE KEY LAYOUT");
        if (other_registry(plan, incoming))
            text.push_back(text_of(fill(
                "Replace {name} from {old} with the one from {new}?",
                {{"name", title},
                 {"old", installed.origin->registry},
                 {"new", incoming.origin.registry}}
            )));
        else
            text.push_back(text_of(fill(
                "{title} {installed} is installed. Replace it with {incoming} from {file}?",
                {{"title", title},
                 {"installed", installed.version},
                 {"incoming", incoming.version},
                 {"file", file}}
            )));
        text.push_back(
            text_of(fill("The version installed is kept, and can be brought back.", {}))
        );
        if (plan.backup)
            text.push_back(text_of(backup_removed(*plan.backup)));
        add_button(made, cancel_caption, Answer::cancel);
        add_button(made, replace_caption, Answer::replace, true);
        set_keys(
            made, Answer::cancel, Answer::replace, plan.backup ? Answer::cancel : Answer::replace
        );
        break;
    case PlanKind::install:
    case PlanKind::ask_alongside:
    case PlanKind::refuse:
        break;
    }
    return made;
}

PackagePrompt installed_prompt(const Incoming& incoming, const fs::path& folder, bool) {
    PackagePrompt made{};
    made.prompt.title = tr("KEY LAYOUT INSTALLED");
    auto& text = made.prompt.paragraphs;
    text.push_back(text_of(fill(
        "{name} is installed. Choose USE in the Library's Keys tab to play with it.",
        {{"name", incoming.name}}
    )));
    text.push_back(path_of(folder));
    add_done_buttons(made);
    return made;
}

PackagePrompt updated_prompt(
    Change change,
    const Incoming& now,
    const InstalledPackage& before,
    const fs::path& folder,
    const ChangeResult& result,
    bool
) {
    PackagePrompt made{};
    made.prompt.title = tr("KEY LAYOUT UPDATED");
    auto& text = made.prompt.paragraphs;
    const bool same_version = now.version == before.version;
    const std::string new_label = version_label(now.version, now.revision, same_version);
    const std::string old_label = version_label(before.version, before.revision, same_version);
    switch (change) {
    case Change::replace:
    case Change::install:
        if (result.backup_kept)
            text.push_back(text_of(fill(
                "{title} is now {new}. {old} is kept, and can be brought back.",
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
                "{title} is back to {new}. {old} is kept, and can be brought back.",
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
    add_done_buttons(made);
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
        return fill("It holds no oakeys.yaml at its top, or in a single folder at its top.", {});
    case Refusal::manifest_too_large:
        return fill("Its oakeys.yaml is larger than 256 KiB.", {});
    case Refusal::manifest_errors:
        return fill("Its oakeys.yaml has errors:", {});
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
            "It unpacks to {size}, more than the {limit} a key layout may take.",
            {{"size", size(problem.size_bytes)}, {"limit", size(problem.limit_bytes)}}
        );
    case Refusal::bomb:
        return fill(
            "It unpacks to {size} from {packed}, far more than a key layout's files take.",
            {{"size", size(problem.size_bytes)}, {"packed", size(problem.limit_bytes)}}
        );
    case Refusal::too_many_folders:
        return fill(
            "It holds more than {limit} folders, more than a key layout may take.",
            {{"limit", std::to_string(max_package_folders)}}
        );
    case Refusal::no_space:
        return fill(
            "It needs {size} free, and the disk holding your Keys folder has {free}.",
            {{"size", size(problem.size_bytes)}, {"free", size(problem.limit_bytes)}}
        );
    case Refusal::path_too_long:
        return problem.detail;
    case Refusal::reserved_id:
        if (name.starts_with("own-"))
            return fill("The id {id} is kept for your own changes.", {{"id", name}});
        return fill("Its id, {id}, cannot name a folder on Windows.", {{"id", name}});
    case Refusal::no_free_folder:
        return fill("Your Keys folder has no free folder name for it.", {});
    case Refusal::not_placed:
        return fill(
            "Its files could not be put in place. Another program, such as a file sync or a "
            "virus scanner, may be using its files; try again in a moment.",
            {}
        );
    case Refusal::changed:
        return fill(
            "Your Keys folder changed while this key layout was being installed; nothing was "
            "changed. Open the file again.",
            {}
        );
    case Refusal::busy:
        return fill(
            "Another copy of Open Annihilation is changing your Keys folder. Try again once "
            "it has finished.",
            {}
        );
    case Refusal::unknown_kind:
        return fill("It is not a kind of package this game installs.", {});
    case Refusal::engine_unmet:
        return problem.detail.empty() ? fill("It needs a different Open Annihilation.", {})
                                      : problem.detail;
    case Refusal::not_a_pack:
        return fill(
            "The folder {name} does not hold this key layout. It is left as it is.",
            {{"name", name}}
        );
    case Refusal::outside_layout:
        return fill(
            "The package holds {name}, outside the files a key layout may hold.", {{"name", name}}
        );
    case Refusal::icon_invalid:
        return fill(
            "Its icon {name} is not a square PNG of at most 256 pixels and 256 KiB.",
            {{"name", name}}
        );
    default:
        return fill("It cannot be read.", {});
    }
}

PackagePrompt
refused_prompt(std::string_view file_name, const Problem& problem, bool change_failed) {
    PackagePrompt made{};
    made.prompt.title = tr("KEY LAYOUT NOT INSTALLED");
    auto& text = made.prompt.paragraphs;
    text.push_back(
        text_of(fill("{file} cannot be installed.", {{"file", std::string(file_name)}}))
    );
    text.push_back(text_of(refusal_text(problem)));
    if (problem.refusal == Refusal::manifest_errors)
        text.push_back(text_of(tr("This layout has problems:")));
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

} // namespace oa::app::package_install::oakeys
