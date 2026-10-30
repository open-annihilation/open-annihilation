// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Where a package installs and what it asks, row by row of the decision
// table, over a Mods folder kept in a map; and each prompt's title, buttons,
// first mark and sentences, each fitting the prompt's height with its
// longest texts and a long path.

#include "oa/app/package_install.hpp"
#include "oa/app/package_install/oamod.hpp"
#include "oa/app/package_install/prompts.hpp"
#include "oa/test/check.hpp"

#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace install = oa::app::package_install;
using install::FolderKind;
using install::InstalledPackage;
using install::PlanKind;

/// A Mods folder kept in a map: each folder's contents and its .backup's.
struct Folders {
    std::map<std::string, InstalledPackage, std::less<>> held{};
    std::map<std::string, InstalledPackage, std::less<>> backups{};

    /// Returns the hooks that read it.
    ///
    /// @return the hooks
    install::FolderHooks hooks() {
        install::FolderHooks made{};
        made.context = this;
        made.look = [](void* context, std::string_view folder) {
            auto& self = *static_cast<Folders*>(context);
            const auto found = self.held.find(folder);
            return found == self.held.end() ? InstalledPackage{} : found->second;
        };
        made.backup_of = [](void* context, std::string_view folder) {
            auto& self = *static_cast<Folders*>(context);
            const auto found = self.backups.find(folder);
            return found == self.backups.end() ? std::optional<InstalledPackage>{}
                                               : std::optional<InstalledPackage>{found->second};
        };
        return made;
    }
};

/// Returns a folder that holds a mod.
///
/// @param id its id
/// @param version its version
/// @param revision its revision
/// @return what the folder holds
InstalledPackage mod(std::string id, std::string version, int64_t revision) {
    return {FolderKind::package, std::move(id), "Example Mod", std::move(version), revision};
}

/// The incoming mod of the tests.
const install::Incoming incoming{"example-mod", "Example Mod", "1.0", 3};

void test_rows() {
    Folders folders{};
    // 5: nothing of its id: installed with no question.
    auto plan = install::oamod::plan_install(incoming, folders.hooks());
    OA_CHECK(plan.kind == PlanKind::install && plan.target == "example-mod");
    // 1: the same version at another revision: an update, the older one said.
    folders.held["example-mod"] = mod("example-mod", "1.0", 2);
    folders.backups["example-mod"] = mod("example-mod", "1.0", 1);
    plan = install::oamod::plan_install(incoming, folders.hooks());
    OA_CHECK(plan.kind == PlanKind::ask_update && plan.target == "example-mod" && !plan.older);
    OA_CHECK(plan.installed.revision == 2 && plan.backup && plan.backup->revision == 1);
    folders.held["example-mod"] = mod("example-mod", "1.0", 4);
    plan = install::oamod::plan_install(incoming, folders.hooks());
    OA_CHECK(plan.kind == PlanKind::ask_update && plan.older);
    // An installed revision that cannot be read counts as another.
    folders.held["example-mod"] = mod("example-mod", "1.0", 0);
    plan = install::oamod::plan_install(incoming, folders.hooks());
    OA_CHECK(plan.kind == PlanKind::ask_update && !plan.older);
    // 2: the same version and revision: a reinstall.
    folders.held["example-mod"] = mod("example-mod", "1.0", 3);
    plan = install::oamod::plan_install(incoming, folders.hooks());
    OA_CHECK(plan.kind == PlanKind::ask_reinstall && plan.target == "example-mod");
    // 6: another version: replace, or alongside in the first free folder.
    folders.held["example-mod"] = mod("example-mod", "0.9", 7);
    folders.backups.clear();
    plan = install::oamod::plan_install(incoming, folders.hooks());
    OA_CHECK(plan.kind == PlanKind::ask_version && plan.target == "example-mod");
    OA_CHECK(plan.alongside == "example-mod-1.0" && !plan.backup);
    // 3 and 4: an alongside folder of this version is updated, or reinstalled.
    folders.held["example-mod-1.0"] = mod("example-mod", "1.0", 1);
    plan = install::oamod::plan_install(incoming, folders.hooks());
    OA_CHECK(plan.kind == PlanKind::ask_update && plan.target == "example-mod-1.0");
    folders.held["example-mod-1.0"] = mod("example-mod", "1.0", 3);
    plan = install::oamod::plan_install(incoming, folders.hooks());
    OA_CHECK(plan.kind == PlanKind::ask_reinstall && plan.target == "example-mod-1.0");
    // The first alongside folder holds another mod, the second this version.
    folders.held["example-mod-1.0"] = mod("other-mod", "1.0", 1);
    folders.held["example-mod-1.0-2"] = mod("example-mod", "1.0", 1);
    plan = install::oamod::plan_install(incoming, folders.hooks());
    OA_CHECK(plan.kind == PlanKind::ask_update && plan.target == "example-mod-1.0-2");
    // 7: the folder of its id holds something else: never replaced.
    folders.held.clear();
    folders.held["example-mod"] = InstalledPackage{FolderKind::other, {}, {}, {}, 0};
    plan = install::oamod::plan_install(incoming, folders.hooks());
    OA_CHECK(plan.kind == PlanKind::ask_alongside && plan.target.empty());
    OA_CHECK(plan.alongside == "example-mod-1.0");
    folders.held["example-mod"] = mod("other-mod", "1.0", 1);
    folders.held["example-mod-1.0"] = InstalledPackage{FolderKind::other, {}, {}, {}, 0};
    plan = install::oamod::plan_install(incoming, folders.hooks());
    OA_CHECK(plan.kind == PlanKind::ask_alongside && plan.alongside == "example-mod-1.0-2");
    // 8: no free folder alongside.
    for (const auto& name : install::oamod::alongside_names(incoming))
        folders.held[name] = InstalledPackage{FolderKind::other, {}, {}, {}, 0};
    plan = install::oamod::plan_install(incoming, folders.hooks());
    OA_CHECK(plan.kind == PlanKind::refuse);
    const auto names = install::oamod::alongside_names(incoming);
    OA_CHECK(names.size() == install::alongside_tries);
    OA_CHECK(names.front() == "example-mod-1.0" && names.back() == "example-mod-1.0-99");
}

/// Returns a prompt's paragraphs joined, one a line.
///
/// @param made the prompt
/// @return the text
std::string text_of(const install::PackagePrompt& made) {
    std::string text;
    for (const auto& paragraph : made.prompt.paragraphs)
        text += paragraph.text + "\n";
    return text;
}

/// Tells whether a prompt's text holds a part.
///
/// @param made the prompt
/// @param part the part
/// @return true when it does
bool says(const install::PackagePrompt& made, std::string_view part) {
    return text_of(made).find(part) != std::string::npos;
}

/// Returns a prompt's captions.
///
/// @param made the prompt
/// @return the captions, left to right
std::vector<std::string> captions_of(const install::PackagePrompt& made) {
    std::vector<std::string> captions;
    for (const auto& button : made.prompt.buttons)
        captions.push_back(button.caption);
    return captions;
}

void test_prompts() {
    // A long Mods folder, as a path of 200 characters shows.
    const fs::path mods =
        fs::path("/") / std::string(80, 'd') / std::string(80, 'e') / "Open Annihilation" / "Mods";
    install::InstallPlan plan{};
    plan.kind = PlanKind::ask_update;
    plan.target = "example-mod";
    plan.installed = mod("example-mod", "1.0", 2);
    plan.backup = mod("example-mod", "1.0", 1);
    plan.older = true;
    auto made =
        install::oamod::question_prompt(plan, incoming, "example-mod-1.0.oamod", mods, true);
    OA_CHECK(made.prompt.title == "UPDATE MOD");
    OA_CHECK((captions_of(made) == std::vector<std::string>{"Cancel", "Replace"}));
    OA_CHECK(made.answers[made.prompt.cancel_button] == install::Answer::cancel);
    OA_CHECK(made.answers[made.prompt.primary_button] == install::Answer::replace);
    // A kept version would be removed: CANCEL is marked first.
    OA_CHECK(made.answers[made.prompt.marked] == install::Answer::cancel);
    OA_CHECK(says(made, "revision 2, is installed. Replace it with revision 3"));
    OA_CHECK(says(made, "Revision 3 is older than the one installed."));
    OA_CHECK(says(made, "The folder kept for Roll back (1.0 revision 1) is removed."));
    OA_CHECK(says(made, "The game reloads its data"));
    OA_CHECK(oa::ui::engine_settings::prompt_fits(made.prompt));
    plan.backup.reset();
    made = install::oamod::question_prompt(plan, incoming, "example-mod-1.0.oamod", mods, false);
    OA_CHECK(made.answers[made.prompt.marked] == install::Answer::replace);
    OA_CHECK(!says(made, "removed") && !says(made, "reloads"));
    plan.backup = InstalledPackage{FolderKind::other, {}, {}, {}, 0};
    made = install::oamod::question_prompt(plan, incoming, "example-mod-1.0.oamod", mods, true);
    OA_CHECK(says(made, "which holds no mod that can be read, is removed."));

    plan.kind = PlanKind::ask_version;
    plan.installed = mod("example-mod", "0.9", 7);
    plan.alongside = "example-mod-1.0";
    plan.backup = mod("example-mod", "0.8", 2);
    made = install::oamod::question_prompt(plan, incoming, "example-mod-1.0.oamod", mods, true);
    OA_CHECK(made.prompt.title == "ANOTHER VERSION");
    OA_CHECK(
        (captions_of(made) == std::vector<std::string>{"Cancel", "Install alongside", "Replace"})
    );
    OA_CHECK(made.answers[made.prompt.marked] == install::Answer::cancel);
    OA_CHECK(made.prompt.paragraphs.back().path);
    OA_CHECK(made.prompt.paragraphs.back().text.ends_with("example-mod-1.0"));
    OA_CHECK(oa::ui::engine_settings::prompt_fits(made.prompt));

    plan.kind = PlanKind::ask_reinstall;
    plan.installed = mod("example-mod", "1.0", 3);
    plan.backup = mod("example-mod", "1.0", 2);
    made = install::oamod::question_prompt(plan, incoming, "example-mod-1.0.oamod", mods, true);
    OA_CHECK(made.prompt.title == "ALREADY INSTALLED");
    OA_CHECK((captions_of(made) == std::vector<std::string>{"Cancel", "Reinstall"}));
    // A reinstall replaces files that cannot be brought back: CANCEL first.
    OA_CHECK(made.answers[made.prompt.marked] == install::Answer::cancel);
    OA_CHECK(says(made, "The version kept, 1.0 revision 2, stays."));
    OA_CHECK(oa::ui::engine_settings::prompt_fits(made.prompt));

    plan.kind = PlanKind::ask_alongside;
    plan.target.clear();
    made = install::oamod::question_prompt(plan, incoming, "example-mod-1.0.oamod", mods, false);
    OA_CHECK(made.prompt.title == "FOLDER IN USE");
    OA_CHECK((captions_of(made) == std::vector<std::string>{"Cancel", "Install alongside"}));
    OA_CHECK(made.answers[made.prompt.marked] == install::Answer::alongside);
    OA_CHECK(oa::ui::engine_settings::prompt_fits(made.prompt));

    made = install::oamod::installing_prompt(
        incoming, 500, 1000, install::InstallingPhase::unpacking, "example-mod-1.0.oamod"
    );
    OA_CHECK(made.prompt.title == "INSTALLING MOD" && made.prompt.progress == 500);
    OA_CHECK(says(made, "Unpacking Example Mod 1.0...") && says(made, "500 bytes of"));
    OA_CHECK((captions_of(made) == std::vector<std::string>{"Cancel"}));

    made = install::oamod::installing_prompt(
        incoming, 500, 1000, install::InstallingPhase::checking, "example-mod-1.0.oamod"
    );
    OA_CHECK(made.prompt.title == "INSTALLING MOD");
    OA_CHECK(made.answers[made.prompt.marked] == install::Answer::cancel);
    OA_CHECK(says(made, "Checking example-mod-1.0.oamod..."));
    OA_CHECK(says(made, "500 bytes of"));
    OA_CHECK(oa::ui::engine_settings::prompt_fits(made.prompt));

    made = install::oamod::installed_prompt(incoming, mods / "example-mod", true);
    OA_CHECK(made.prompt.title == "MOD INSTALLED");
    OA_CHECK((captions_of(made) == std::vector<std::string>{"Open folder", "Play now", "OK"}));
    OA_CHECK(made.answers[made.prompt.marked] == install::Answer::ok);
    OA_CHECK(made.answers[made.prompt.cancel_button] == install::Answer::ok);
    OA_CHECK(oa::ui::engine_settings::prompt_fits(made.prompt));
    made = install::oamod::installed_prompt(incoming, mods / "example-mod", false);
    OA_CHECK((captions_of(made) == std::vector<std::string>{"Open folder", "OK"}));
    OA_CHECK(says(made, "Choose it with Play now in the Library"));

    install::ChangeResult result{};
    result.changed = true;
    result.backup_kept = true;
    result.left_over = mods / "example-mod-left-over";
    made = install::oamod::updated_prompt(
        install::Change::replace,
        incoming,
        mod("example-mod", "1.0", 2),
        mods / "example-mod",
        result,
        false
    );
    OA_CHECK(made.prompt.title == "MOD UPDATED");
    OA_CHECK(says(made, "Example Mod is now 1.0 revision 3. 1.0 revision 2 is kept"));
    OA_CHECK(says(made, "could not be kept as the earlier version"));
    OA_CHECK(oa::ui::engine_settings::prompt_fits(made.prompt));
    made = install::oamod::updated_prompt(
        install::Change::roll_back,
        incoming,
        mod("example-mod", "2.0", 1),
        mods / "example-mod",
        {},
        true
    );
    OA_CHECK(says(made, "Example Mod is back to 1.0."));
    OA_CHECK((captions_of(made) == std::vector<std::string>{"Open folder", "Play now", "OK"}));

    // A refusal shows three diagnostics at most, and says how many more.
    install::Problem problem{};
    problem.refusal = install::Refusal::manifest_errors;
    for (int line = 0; line < 8; ++line)
        problem.lines.push_back(
            "example-mod-1.0.oamod/oamod.yaml:" + std::to_string(line + 3) +
            ":1: hacks.example-hack: no standard hack of this name; this build knows none like it"
        );
    made = install::oamod::refused_prompt("example-mod-1.0.oamod", problem, false);
    OA_CHECK(made.prompt.title == "MOD NOT INSTALLED");
    OA_CHECK(made.prompt.paragraphs.size() == 6);
    OA_CHECK(says(made, "And 5 more; the log has them all."));
    OA_CHECK((captions_of(made) == std::vector<std::string>{"OK"}));
    OA_CHECK(oa::ui::engine_settings::prompt_fits(made.prompt));
    problem = {};
    problem.refusal = install::Refusal::not_placed;
    made = install::oamod::refused_prompt("example-mod-1.0.oamod", problem, true);
    OA_CHECK(says(made, "virus scanner") && says(made, "Nothing was changed."));
    // Every refusal has its sentence.
    for (int refusal = 0; refusal <= static_cast<int>(install::Refusal::busy); ++refusal) {
        problem = {};
        problem.refusal = static_cast<install::Refusal>(refusal);
        problem.subject = "units/armcom.fbi";
        problem.detail = "the path is too long";
        OA_CHECK(!install::oamod::refusal_text(problem).empty());
    }
    // ROLL BACK's refusals, told over the settings dialog, wrap to fit.
    const std::string long_title =
        "A Mod Whose Name Runs On Through Many Words Of Its Own, Longer Than Any Line Of The "
        "Prompt Holds";
    made = install::oamod::roll_back_refused_prompt(
        long_title, "1.0 revision 2", "Its oamod.yaml cannot be played; the log says why."
    );
    OA_CHECK(made.prompt.title == "MOD NOT ROLLED BACK");
    OA_CHECK(says(made, ", the version kept for Roll back, cannot be played."));
    OA_CHECK(says(made, "the log says why.") && says(made, "Nothing was changed."));
    OA_CHECK((captions_of(made) == std::vector<std::string>{"OK"}));
    OA_CHECK(made.answers[made.prompt.marked] == install::Answer::ok);
    OA_CHECK(made.answers[made.prompt.cancel_button] == install::Answer::ok);
    OA_CHECK(oa::ui::engine_settings::prompt_fits(made.prompt));
    made = install::oamod::roll_back_failed_prompt(long_title);
    OA_CHECK(made.prompt.title == "MOD NOT ROLLED BACK");
    OA_CHECK(says(made, "could not be rolled back; nothing was changed."));
    OA_CHECK((captions_of(made) == std::vector<std::string>{"OK"}));
    OA_CHECK(oa::ui::engine_settings::prompt_fits(made.prompt));
    OA_CHECK(install::oamod::version_label("1.0", 0, true) == "1.0 of an unknown revision");
    OA_CHECK(install::oamod::version_label("1.0", 2, false) == "1.0");
}

} // namespace

int main() {
    test_rows();
    test_prompts();
    return oa::test::check_exit_status();
}
