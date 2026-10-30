// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The oaexp kind's prompts: the English templates an expansion install shows,
// looked up in the interface catalogue, then filled.

#include "prompt_text.hpp"

#include "oa/app/package_install/oaexp.hpp"
#include "oa/ui/game_files.hpp"

#include <string>
#include <string_view>

namespace oa::app::package_install::oaexp {

namespace fs = std::filesystem;

using detail::add_button;
using detail::cancel_caption;
using detail::fill;
using detail::ok_caption;
using detail::open_folder_caption;
using detail::set_keys;
using detail::text_of;
using detail::tr;

namespace {

/// The most manifest diagnostics a refusal shows; the log has them all.
constexpr std::size_t shown_diagnostics = 3;

/// The Install button of a package the player opened.
constexpr std::string_view install_caption = "Install";

/// The sentence for a manifest that is not an expansion's.
constexpr std::string_view not_an_expansion = "It is not one of the game's expansions.";

/// Adds OPEN FOLDER and OK.
///
/// @param[in,out] made the prompt
void add_done_buttons(PackagePrompt& made) {
    add_button(made, open_folder_caption, Answer::open_folder);
    add_button(made, ok_caption, Answer::ok, true);
    set_keys(made, Answer::ok, Answer::ok, Answer::ok);
}

/// Returns the prompt shown when the copy is done.
///
/// @param name the expansion's name
/// @param folder the folder
/// @return the prompt
PackagePrompt done_prompt(std::string_view name, const fs::path& folder) {
    PackagePrompt made{};
    made.prompt.title = tr("EXPANSION INSTALLED");
    made.prompt.paragraphs.push_back(text_of(
        fill("{name} is installed. Turn it on in the Library.", {{"name", std::string(name)}})
    ));
    made.prompt.paragraphs.push_back(detail::shown_path(folder));
    add_done_buttons(made);
    return made;
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
    made.prompt.title = tr("INSTALLING EXPANSION");
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
    add_button(made, cancel_caption, Answer::cancel, true);
    set_keys(made, Answer::cancel, Answer::cancel, Answer::cancel);
    return made;
}

PackagePrompt question_prompt(
    const InstallPlan&, const Incoming& incoming, std::string_view, const fs::path&, bool
) {
    PackagePrompt made{};
    made.prompt.title = tr("INSTALLING EXPANSION");
    made.prompt.paragraphs.push_back(
        text_of(fill("Install the expansion {name}?", {{"name", incoming.name}}))
    );
    add_button(made, cancel_caption, Answer::cancel);
    add_button(made, install_caption, Answer::ok, true);
    set_keys(made, Answer::cancel, Answer::ok, Answer::cancel);
    return made;
}

PackagePrompt installed_prompt(const Incoming& incoming, const fs::path& folder, bool) {
    return done_prompt(incoming.name, folder);
}

PackagePrompt updated_prompt(
    Change,
    const Incoming& now,
    const InstalledPackage&,
    const fs::path& folder,
    const ChangeResult&,
    bool
) {
    return done_prompt(now.name, folder);
}

std::string refusal_text(const Problem& problem) {
    const std::string name = problem.subject;
    switch (problem.refusal) {
    case Refusal::none:
    case Refusal::unreadable:
        return fill("It cannot be read.", {});
    case Refusal::not_zip:
        return fill("It is not a zip archive.", {});
    case Refusal::damaged:
        return name.empty()
                   ? fill("It is damaged.", {})
                   : fill("The file {name} is not the expansion's file.", {{"name", name}});
    case Refusal::no_manifest:
        return fill("It holds no oaexp.yaml at its top, or in a single folder at its top.", {});
    case Refusal::manifest_too_large:
        return fill("Its oaexp.yaml is larger than 256 KiB.", {});
    case Refusal::manifest_errors:
        if (problem.detail == not_an_expansion)
            return fill(not_an_expansion, {});
        return fill("Its oaexp.yaml has errors.", {});
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
    case Refusal::reserved_id:
        return fill("Its id, {id}, cannot name a folder on Windows.", {{"id", name}});
    case Refusal::missing_entry:
        return fill("The expansion is missing {name}.", {{"name", name}});
    case Refusal::stray_file:
        return fill("The expansion holds {name}, which it does not have.", {{"name", name}});
    case Refusal::unknown_kind:
        return fill(not_an_expansion, {});
    case Refusal::not_placed:
        return fill(
            "Its files could not be put in place. Another program, such as a file sync or a "
            "virus scanner, may be using its files; try again in a moment.",
            {}
        );
    default:
        return fill("It cannot be read.", {});
    }
}

PackagePrompt refused_prompt(std::string_view file_name, const Problem& problem, bool) {
    PackagePrompt made{};
    made.prompt.title = tr("EXPANSION NOT INSTALLED");
    made.prompt.paragraphs.push_back(
        text_of(fill("{file} cannot be installed.", {{"file", std::string(file_name)}}))
    );
    made.prompt.paragraphs.push_back(text_of(refusal_text(problem)));
    for (std::size_t index = 0; index < problem.lines.size() && index < shown_diagnostics; ++index)
        made.prompt.paragraphs.push_back(text_of(problem.lines[index]));
    add_button(made, ok_caption, Answer::ok, true);
    set_keys(made, Answer::ok, Answer::ok, Answer::ok);
    return made;
}

} // namespace oa::app::package_install::oaexp
