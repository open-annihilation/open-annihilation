// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The oalang kind's prompts: the English templates a language pack install
// shows, looked up in the interface catalogue, then filled.

#include "prompt_text.hpp"

#include "oa/app/package_install/oalang.hpp"
#include "oa/app/package_install/oamod.hpp"
#include "oa/ui/game_files.hpp"

#include <string>
#include <string_view>
#include <utility>

namespace oa::app::package_install::oalang {

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

/// The Install button of a new pack the player opened.
constexpr std::string_view install_caption = "Install";

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

/// Returns the English name a question shows beside the endonym.
///
/// @param incoming the pack
/// @return its English name, or its tag when the manifest names none
std::string english_of(const Incoming& incoming) {
    return incoming.english_name.empty() ? incoming.id : incoming.english_name;
}

/// Adds OPEN FOLDER and OK. PLAY NOW is never offered for a language.
///
/// @param[in,out] made the prompt
void add_done_buttons(PackagePrompt& made) {
    add_button(made, open_folder_caption, Answer::open_folder);
    add_button(made, ok_caption, Answer::ok, true);
    set_keys(made, Answer::ok, Answer::ok, Answer::ok);
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
    made.prompt.title = tr("INSTALLING LANGUAGE");
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
    const std::string english = english_of(incoming);
    const std::string file(file_name);
    switch (plan.kind) {
    case PlanKind::ask_install:
        made.prompt.title = tr("INSTALL LANGUAGE");
        text.push_back(text_of(fill(
            "Install the language {endonym} ({english})?",
            {{"endonym", title}, {"english", english}}
        )));
        add_button(made, cancel_caption, Answer::cancel);
        add_button(made, install_caption, Answer::alongside, true);
        set_keys(made, Answer::cancel, Answer::alongside, Answer::alongside);
        break;
    case PlanKind::ask_update: {
        made.prompt.title = tr("UPDATE LANGUAGE");
        if (installed.revision == 0)
            text.push_back(text_of(fill(
                "{title} {version}, an unknown revision, is installed. Replace it with "
                "revision {incoming} from {file}?",
                {{"title", title},
                 {"version", incoming.version},
                 {"incoming", std::to_string(incoming.revision)},
                 {"file", file}}
            )));
        else
            text.push_back(text_of(fill(
                "{title} {version}, revision {installed}, is installed. Replace it with "
                "revision {incoming} from {file}?",
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
    }
    case PlanKind::ask_reinstall:
        made.prompt.title = tr("REINSTALL LANGUAGE");
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
        add_button(made, cancel_caption, Answer::cancel);
        add_button(made, reinstall_caption, Answer::reinstall, true);
        set_keys(made, Answer::cancel, Answer::reinstall, Answer::cancel);
        break;
    case PlanKind::ask_version:
        made.prompt.title = tr("REPLACE LANGUAGE");
        if (installed.kind == FolderKind::package)
            text.push_back(text_of(fill(
                "{title} {installed} is installed. Replace it with {incoming} from {file}?",
                {{"title", installed.name.empty() ? title : installed.name},
                 {"installed", installed.version},
                 {"incoming", incoming.version},
                 {"file", file}}
            )));
        else
            text.push_back(text_of(fill(
                "The folder {tag} does not hold this language. Replace it with {endonym} "
                "({english}) from {file}? It is kept, and can be brought back.",
                {{"tag", incoming.id}, {"endonym", title}, {"english", english}, {"file", file}}
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
    made.prompt.title = tr("LANGUAGE INSTALLED");
    auto& text = made.prompt.paragraphs;
    text.push_back(text_of(fill(
        "{endonym} ({english}) {version} is installed in your Languages folder:",
        {{"endonym", incoming.name},
         {"english", english_of(incoming)},
         {"version", incoming.version}}
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
    made.prompt.title = tr("LANGUAGE UPDATED");
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
    switch (problem.refusal) {
    case Refusal::no_manifest:
        return fill("It holds no language.yaml.", {});
    case Refusal::manifest_errors:
        return problem.detail.empty() ? fill("Its language.yaml has errors.", {}) : problem.detail;
    case Refusal::engine_unmet:
        return problem.detail.empty() ? fill("It needs a different Open Annihilation.", {})
                                      : problem.detail;
    case Refusal::font_missing:
        return fill("It lists a font it does not hold: {name}.", {{"name", name}});
    case Refusal::font_unreadable:
        return name.empty() ? fill("A font in the pack cannot be opened.", {})
                            : fill("Its font {name} cannot be opened.", {{"name", name}});
    case Refusal::warmup_invalid:
        return problem.detail.empty() ? fill("Its warm-up text cannot be read.", {})
                                      : problem.detail;
    case Refusal::table_errors:
        return problem.detail.empty() ? fill("A table in the pack does not parse.", {})
                                      : problem.detail;
    default:
        return oamod::refusal_text(problem);
    }
}

PackagePrompt
refused_prompt(std::string_view file_name, const Problem& problem, bool change_failed) {
    PackagePrompt made{};
    made.prompt.title = tr("LANGUAGE NOT INSTALLED");
    auto& text = made.prompt.paragraphs;
    text.push_back(
        text_of(fill("{file} cannot be installed.", {{"file", std::string(file_name)}}))
    );
    text.push_back(text_of(refusal_text(problem)));
    // The reader's message is the refusal sentence. Listing the same lines
    // again would show it twice.
    if (problem.refusal != Refusal::manifest_errors && problem.refusal != Refusal::table_errors &&
        problem.refusal != Refusal::warmup_invalid && problem.refusal != Refusal::engine_unmet) {
        for (std::size_t index = 0; index < problem.lines.size() && index < shown_diagnostics;
             ++index)
            text.push_back(text_of(problem.lines[index]));
        if (problem.lines.size() > shown_diagnostics)
            text.push_back(text_of(fill(
                "And {n} more; the log has them all.",
                {{"n", std::to_string(problem.lines.size() - shown_diagnostics)}}
            )));
    }
    if (change_failed && problem.refusal != Refusal::changed)
        text.push_back(text_of(fill("Nothing was changed.", {})));
    add_button(made, ok_caption, Answer::ok, true);
    set_keys(made, Answer::ok, Answer::ok, Answer::ok);
    return made;
}

const KindPrompts& prompts() noexcept {
    static const KindPrompts prompts{
        .installing = &installing_prompt,
        .question = &question_prompt,
        .installed = &installed_prompt,
        .updated = &updated_prompt,
        .refusal_text = &refusal_text,
        .refused = &refused_prompt,
    };
    return prompts;
}

} // namespace oa::app::package_install::oalang
