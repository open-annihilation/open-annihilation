// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// UNINSTALL over made-up registries and packages: the button on every
// installed package, after ROLL BACK and before OPEN FOLDER, and on none
// that is not installed; off for a map pack during a game and for a package
// in the download queue until its item fails or is installed, and the note
// that says why; what pressing it leads to for the mod being played, the
// language pack in use and every other package; the question's words, with
// and without the copy ROLL BACK keeps, for a listed package and the
// player's own, CANCEL marked and UNINSTALL primary; the default brain's
// extra paragraph, and none for another brain; the notices' words and
// their one OK; and a lookup that translates.
#include "oa/test/check.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/library/uninstall.hpp"

#include <cstdio>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace lib = oa::ui::library;
namespace kit = oa::ui::kit;
using lib::Action;
using lib::Kind;
using lib::UninstallStep;
using Phase = lib::QueueItem::Phase;

/// Checks that a text is what a case expects, printing both when it is not.
///
/// @param actual the text written
/// @param expected the text the case expects
/// @param file the check's file
/// @param line the check's line
/// @param expression the text's expression, as written
void check_text(
    std::string_view actual,
    std::string_view expected,
    const char* file,
    int line,
    const char* expression
) {
    if (actual == expected)
        return;
    std::fprintf(
        stderr,
        "%s:%d: %s is \"%.*s\", expected \"%.*s\"\n",
        file,
        line,
        expression,
        static_cast<int>(actual.size()),
        actual.data(),
        static_cast<int>(expected.size()),
        expected.data()
    );
    ++oa::test::failed_checks();
}

#define OA_CHECK_TEXT(actual, expected)                                                            \
    check_text((actual), (expected), __FILE__, __LINE__, #actual)

/// The built-in registry's id.
constexpr std::string_view core = "fixture-core";

/// A listing of the built-in registry.
///
/// @param kind its kind
/// @param key its key
/// @param name its name
/// @return the listing
lib::Listing listing(Kind kind, std::string_view key, std::string_view name) {
    lib::Listing made;
    made.registry = std::string(core);
    made.kind = kind;
    made.key = std::string(key);
    made.name = std::string(name);
    made.version = "1.0";
    made.release = 1;
    made.size = 1024;
    return made;
}

/// A package installed from the built-in registry, or the player's own.
///
/// @param registry its registry; empty for the player's own
/// @param kind its kind
/// @param key its key
/// @param name its name
/// @param folder its folder
/// @return the package
lib::Installed installed(
    std::string_view registry,
    Kind kind,
    std::string_view key,
    std::string_view name,
    std::string_view folder
) {
    lib::Installed made;
    made.kind = kind;
    made.key = std::string(key);
    made.name = std::string(name);
    made.folder = std::string(folder);
    made.version = "1.0";
    made.registry = std::string(registry);
    made.release = registry.empty() ? 0 : 1;
    return made;
}

/// A queue item of the built-in registry.
///
/// @param kind its kind
/// @param key its package's key
/// @param name its package's name
/// @param phase where it is
/// @return the item
lib::QueueItem queued(Kind kind, std::string_view key, std::string_view name, Phase phase) {
    lib::QueueItem item;
    item.registry = std::string(core);
    item.kind = kind;
    item.key = std::string(key);
    item.name = std::string(name);
    item.phase = phase;
    return item;
}

/// The fixture: Ridge, a mod of the built-in registry with a kept copy;
/// Cinder, the mod being played; the player's own mod; an installed map
/// pack; the language pack in use and another; and a mod not installed.
///
/// @param in_match a game is being played
/// @return the inputs
lib::Inputs fixture_inputs(bool in_match = false) {
    lib::Inputs inputs;
    lib::Registry registry;
    registry.id = std::string(core);
    registry.name = "Fixture Core";
    registry.built_in = true;
    inputs.registries.push_back(registry);
    inputs.listings.push_back(listing(Kind::mod, "ridge", "Ridge"));
    inputs.listings.push_back(listing(Kind::mod, "cinder", "Cinder"));
    inputs.listings.push_back(listing(Kind::mod, "lattice", "Lattice"));
    inputs.listings.push_back(listing(Kind::map_pack, "archipelago", "Archipelago"));
    inputs.listings.push_back(listing(Kind::language, "en-XA", "Pseudo"));
    inputs.listings.push_back(listing(Kind::language, "pt-BR", "Português (Brasil)"));
    inputs.installed.push_back(installed(core, Kind::mod, "ridge", "Ridge", "Mods/ridge"));
    inputs.installed.back().kept = lib::Kept{"0.9", 2};
    inputs.installed.push_back(installed(core, Kind::mod, "cinder", "Cinder", "Mods/cinder"));
    inputs.installed.back().playing = true;
    inputs.installed.push_back(installed("", Kind::mod, "own-mod", "Own Mod", "Mods/own-mod"));
    inputs.installed.push_back(
        installed(core, Kind::map_pack, "archipelago", "Archipelago", "Maps/archipelago")
    );
    inputs.installed.push_back(
        installed(core, Kind::language, "en-XA", "Pseudo", "Languages/en-XA")
    );
    inputs.installed.back().in_use = true;
    inputs.installed.push_back(
        installed(core, Kind::language, "pt-BR", "Português (Brasil)", "Languages/pt-BR")
    );
    inputs.in_match = in_match;
    return inputs;
}

/// Builds a Library from inputs, on its Mods tab.
///
/// @param inputs the inputs
/// @return the Library
lib::Library built(lib::Inputs inputs) {
    lib::Library library;
    lib::refresh(library, std::move(inputs));
    // A Library opens on Active, which lists no packages to select.
    lib::set_tab(library, lib::Tab::mods);
    return library;
}

/// Finds an entry.
///
/// @param library the Library
/// @param registry its registry; empty for the player's own
/// @param kind its kind
/// @param key its key
/// @return the entry, or null
const lib::Entry*
at(const lib::Library& library, std::string_view registry, Kind kind, std::string_view key) {
    return lib::find_entry(library, {std::string(registry), kind, std::string(key)});
}

/// Returns an entry's buttons and whether each is on.
///
/// @param library the Library
/// @param entry the entry
/// @return the buttons
std::vector<std::pair<Action, bool>>
actions_of(const lib::Library& library, const lib::Entry& entry) {
    std::vector<std::pair<Action, bool>> found;
    for (const lib::ActionButton& button : lib::entry_actions(library, entry))
        found.emplace_back(button.action, button.enabled);
    return found;
}

using Buttons = std::vector<std::pair<Action, bool>>;

void test_button() {
    const lib::Library library = built(fixture_inputs());
    const lib::Entry* ridge = at(library, core, Kind::mod, "ridge");
    OA_CHECK(ridge != nullptr);
    if (ridge != nullptr) {
        OA_CHECK(
            actions_of(library, *ridge) == Buttons(
                                               {{Action::play_now, true},
                                                {Action::roll_back, true},
                                                {Action::uninstall, true},
                                                {Action::open_folder, true}}
                                           )
        );
        OA_CHECK_TEXT(lib::entry_actions(library, *ridge)[2].text, "UNINSTALL");
        OA_CHECK(lib::uninstall_off_reason(library, *ridge).empty());
    }
    OA_CHECK_TEXT(lib::action_text(Action::uninstall), "UNINSTALL");
    // The player's own mod, a map pack and a language pack show it too.
    const lib::Entry* own = at(library, "", Kind::mod, "own-mod");
    if (own != nullptr)
        OA_CHECK(
            actions_of(library, *own) ==
            Buttons(
                {{Action::play_now, true}, {Action::uninstall, true}, {Action::open_folder, true}}
            )
        );
    const lib::Entry* maps = at(library, core, Kind::map_pack, "archipelago");
    OA_CHECK(maps != nullptr);
    if (maps != nullptr)
        OA_CHECK(
            actions_of(library, *maps) ==
            Buttons({{Action::uninstall, true}, {Action::open_folder, true}})
        );
    // A language pack that is installed and not in use offers USE NOW first.
    const lib::Entry* portuguese = at(library, core, Kind::language, "pt-BR");
    OA_CHECK(portuguese != nullptr);
    if (portuguese != nullptr)
        OA_CHECK(
            actions_of(library, *portuguese) ==
            Buttons(
                {{Action::use_now, true}, {Action::uninstall, true}, {Action::open_folder, true}}
            )
        );
    // A package that is not installed has none.
    const lib::Entry* lattice = at(library, core, Kind::mod, "lattice");
    OA_CHECK(lattice != nullptr);
    if (lattice != nullptr) {
        OA_CHECK(actions_of(library, *lattice) == Buttons({{Action::get, true}}));
        OA_CHECK(lib::uninstall_step(library, *lattice) == UninstallStep::off);
    }
}

void test_off() {
    // During a game a map pack's UNINSTALL is off, and says why; a mod's
    // and a language pack's are not.
    {
        lib::Library library = built(fixture_inputs(true));
        const lib::Entry* pack = at(library, core, Kind::map_pack, "archipelago");
        OA_CHECK(pack != nullptr);
        if (pack != nullptr) {
            OA_CHECK(
                actions_of(library, *pack) ==
                Buttons({{Action::uninstall, false}, {Action::open_folder, true}})
            );
            OA_CHECK_TEXT(
                lib::uninstall_off_reason(library, *pack),
                "Map packs can't be uninstalled during a game."
            );
            OA_CHECK(lib::uninstall_step(library, *pack) == UninstallStep::off);
        }
        const lib::Entry* ridge = at(library, core, Kind::mod, "ridge");
        if (ridge != nullptr)
            OA_CHECK(lib::uninstall_step(library, *ridge) == UninstallStep::ask);
        const lib::Entry* other = at(library, core, Kind::language, "pt-BR");
        if (other != nullptr)
            OA_CHECK(lib::uninstall_step(library, *other) == UninstallStep::ask);
        // The note under the buttons says why.
        lib::set_tab(library, lib::Tab::maps);
        lib::select(library, {std::string(core), Kind::map_pack, "archipelago"});
        OA_CHECK_TEXT(lib::details_note(library), "Map packs can't be uninstalled during a game.");
    }
    // In the download queue, UNINSTALL is off until the item fails or is
    // installed: CANCEL comes first.
    for (const Phase phase :
         {Phase::waiting,
          Phase::downloading,
          Phase::verifying,
          Phase::checking,
          Phase::installing,
          Phase::held,
          Phase::failed,
          Phase::installed}) {
        lib::Inputs inputs = fixture_inputs();
        lib::UpdateNote update;
        update.registry = std::string(core);
        update.kind = Kind::mod;
        update.key = "ridge";
        update.to_release = 2;
        update.to_version = "1.1";
        inputs.updates.push_back(update);
        inputs.queue.push_back(queued(Kind::mod, "ridge", "Ridge", phase));
        const lib::Library library = built(std::move(inputs));
        const lib::Entry* ridge = at(library, core, Kind::mod, "ridge");
        OA_CHECK(ridge != nullptr);
        if (ridge == nullptr)
            continue;
        const bool queue_holds = phase != Phase::failed && phase != Phase::installed;
        bool shown = false;
        for (const lib::ActionButton& button : lib::entry_actions(library, *ridge))
            if (button.action == Action::uninstall) {
                shown = true;
                OA_CHECK(button.enabled == !queue_holds);
            }
        OA_CHECK(shown);
        OA_CHECK_TEXT(
            lib::uninstall_off_reason(library, *ridge),
            queue_holds ? "It is in the download queue: CANCEL it before you uninstall it." : ""
        );
        OA_CHECK(
            lib::uninstall_step(library, *ridge) ==
            (queue_holds ? UninstallStep::off : UninstallStep::ask)
        );
    }
    // The mod being played, its update held: the playing note, then why.
    {
        lib::Inputs inputs = fixture_inputs();
        inputs.queue.push_back(queued(Kind::mod, "cinder", "Cinder", Phase::held));
        lib::Library library = built(std::move(inputs));
        lib::select(library, {std::string(core), Kind::mod, "cinder"});
        OA_CHECK_TEXT(
            lib::details_note(library),
            "Finishes when you leave Cinder. It is in the download queue: CANCEL it before you "
            "uninstall it."
        );
        lib::select(library, {std::string(core), Kind::mod, "ridge"});
        OA_CHECK_TEXT(lib::details_note(library), "");
    }
}

void test_steps() {
    const lib::Library library = built(fixture_inputs());
    const auto step_of = [&](std::string_view registry, Kind kind, std::string_view key) {
        const lib::Entry* entry = at(library, registry, kind, key);
        OA_CHECK(entry != nullptr);
        return entry != nullptr ? lib::uninstall_step(library, *entry) : UninstallStep::off;
    };
    OA_CHECK(step_of(core, Kind::mod, "ridge") == UninstallStep::ask);
    OA_CHECK(step_of("", Kind::mod, "own-mod") == UninstallStep::ask);
    OA_CHECK(step_of(core, Kind::mod, "cinder") == UninstallStep::switch_off_first);
    OA_CHECK(step_of(core, Kind::map_pack, "archipelago") == UninstallStep::ask);
    OA_CHECK(step_of(core, Kind::language, "en-XA") == UninstallStep::language_first);
    OA_CHECK(step_of(core, Kind::language, "pt-BR") == UninstallStep::ask);
    // The mod being played and the language in use keep UNINSTALL on: it
    // shows the notice that says what comes first.
    for (const std::pair<Kind, std::string_view>& used :
         {std::pair{Kind::mod, std::string_view{"cinder"}},
          std::pair{Kind::language, std::string_view{"en-XA"}}}) {
        const lib::Entry* entry = at(library, core, used.first, used.second);
        if (entry != nullptr)
            OA_CHECK(lib::uninstall_off_reason(library, *entry).empty());
    }
}

void test_question() {
    const lib::Library library = built(fixture_inputs());
    const lib::Entry* ridge = at(library, core, Kind::mod, "ridge");
    OA_CHECK(ridge != nullptr);
    if (ridge != nullptr) {
        const kit::Question asked = lib::uninstall_question(*ridge, "Mods/ridge");
        OA_CHECK_TEXT(asked.title, "Uninstall Ridge");
        OA_CHECK(asked.paragraphs.size() == 2);
        if (asked.paragraphs.size() == 2) {
            OA_CHECK_TEXT(
                asked.paragraphs[0].text,
                "Uninstall Ridge? Its folder, Mods/ridge, is removed, and with it the copy ROLL "
                "BACK keeps."
            );
            OA_CHECK_TEXT(asked.paragraphs[1].text, "GET installs it again.");
            OA_CHECK(!asked.paragraphs[0].path && !asked.paragraphs[1].path);
        }
        OA_CHECK(asked.buttons.size() == 2);
        if (asked.buttons.size() == 2) {
            OA_CHECK_TEXT(asked.buttons[0].caption, "CANCEL");
            OA_CHECK_TEXT(asked.buttons[0].id, lib::uninstall_cancel_button_id);
            OA_CHECK(!asked.buttons[0].accent);
            OA_CHECK_TEXT(asked.buttons[1].caption, "UNINSTALL");
            OA_CHECK_TEXT(asked.buttons[1].id, lib::uninstall_button_id);
            OA_CHECK(asked.buttons[1].accent);
        }
        OA_CHECK(asked.cancel_button == 0 && asked.primary_button == 1 && asked.marked == 0);
        OA_CHECK_TEXT(asked.word, kit::question_word);
    }
    const lib::Entry* own = at(library, "", Kind::mod, "own-mod");
    OA_CHECK(own != nullptr);
    if (own != nullptr) {
        const kit::Question asked = lib::uninstall_question(*own, "Mods/own-mod");
        OA_CHECK_TEXT(asked.title, "Uninstall Own Mod");
        OA_CHECK(asked.paragraphs.size() == 2);
        if (asked.paragraphs.size() == 2) {
            OA_CHECK_TEXT(
                asked.paragraphs[0].text, "Uninstall Own Mod? Its folder, Mods/own-mod, is removed."
            );
            OA_CHECK_TEXT(
                asked.paragraphs[1].text,
                "No registry lists it, so the Library cannot get it again."
            );
        }
    }
}

void test_notices() {
    const lib::Library library = built(fixture_inputs());
    const auto check_one_button = [](const kit::Question& notice) {
        OA_CHECK(notice.buttons.size() == 1);
        if (notice.buttons.size() == 1) {
            OA_CHECK_TEXT(notice.buttons[0].caption, "OK");
            OA_CHECK_TEXT(notice.buttons[0].id, lib::uninstall_ok_button_id);
            OA_CHECK(notice.buttons[0].accent);
        }
        OA_CHECK(notice.cancel_button == 0 && notice.primary_button == 0 && notice.marked == 0);
        OA_CHECK(notice.paragraphs.size() == 1);
    };
    const lib::Entry* cinder = at(library, core, Kind::mod, "cinder");
    if (cinder != nullptr) {
        const kit::Question told =
            lib::uninstall_notice(*cinder, lib::uninstall_step(library, *cinder));
        OA_CHECK_TEXT(told.title, "Uninstall Cinder");
        check_one_button(told);
        if (told.paragraphs.size() == 1)
            OA_CHECK_TEXT(
                told.paragraphs[0].text,
                "You are playing Cinder. Switch it off first with SWITCH OFF, then uninstall it."
            );
    }
    const lib::Entry* pseudo = at(library, core, Kind::language, "en-XA");
    if (pseudo != nullptr) {
        const kit::Question told =
            lib::uninstall_notice(*pseudo, lib::uninstall_step(library, *pseudo));
        OA_CHECK_TEXT(told.title, "Uninstall Pseudo");
        check_one_button(told);
        if (told.paragraphs.size() == 1)
            OA_CHECK_TEXT(
                told.paragraphs[0].text,
                "The game is shown in Pseudo. Choose another language in Settings › Language "
                "first, then uninstall it."
            );
    }
    const std::pair<lib::UninstallProblem, std::string_view> failures[] = {
        {lib::UninstallProblem::busy,
         "Open Annihilation, or another copy of it, is still changing its folder. Try again in a "
         "moment."},
        {lib::UninstallProblem::changed,
         "Its folder changed since the Library listed it. Try again."},
        {lib::UninstallProblem::link,
         "Its folder is a link, and OA never removes what a link leads to. OPEN FOLDER shows it."},
        {lib::UninstallProblem::outside,
         "Its folder is not in your own Open Annihilation folder, and OA removes nothing outside "
         "it."},
        {lib::UninstallProblem::not_moved,
         "Its folder could not be moved; another program may be using it. Close it, then try "
         "again."},
    };
    for (const auto& [problem, reason] : failures) {
        const kit::Question told = lib::uninstall_failed_notice("Ridge", problem);
        OA_CHECK_TEXT(told.title, "Uninstall Ridge");
        check_one_button(told);
        if (told.paragraphs.size() == 1)
            OA_CHECK_TEXT(
                told.paragraphs[0].text, "Ridge was not uninstalled. " + std::string(reason)
            );
    }
}

/// A lookup that knows three of the uninstall's words in French.
///
/// @param english the English pattern
/// @return the French, or the English
std::string_view french(void*, std::string_view english) {
    if (english == lib::words::uninstall_title)
        return "Désinstaller {name}";
    if (english == lib::words::action_uninstall)
        return "DÉSINSTALLER";
    if (english == lib::words::uninstall_ask)
        return "Désinstaller {name} ? Son dossier, {folder}, est supprimé.";
    return english;
}

void test_lookup() {
    lib::Library library = built(fixture_inputs());
    library.text.shown = french;
    const lib::Entry* own = at(library, "", Kind::mod, "own-mod");
    OA_CHECK(own != nullptr);
    if (own == nullptr)
        return;
    const kit::Question asked = lib::uninstall_question(*own, "Mods/own-mod", library.text);
    OA_CHECK_TEXT(asked.title, "Désinstaller Own Mod");
    if (!asked.paragraphs.empty())
        OA_CHECK_TEXT(
            asked.paragraphs[0].text,
            "Désinstaller Own Mod ? Son dossier, Mods/own-mod, est supprimé."
        );
    if (asked.buttons.size() == 2)
        OA_CHECK_TEXT(asked.buttons[1].caption, "DÉSINSTALLER");
    const std::vector<lib::ActionButton> buttons = lib::entry_actions(library, *own);
    if (buttons.size() == 3)
        OA_CHECK_TEXT(buttons[1].text, "DÉSINSTALLER");
}

/// The default brain's question adds its paragraph before the last. Another
/// brain's question does not.
void test_default_brain() {
    lib::Inputs inputs = fixture_inputs();
    lib::Listing steady = listing(Kind::brain, "steady-commander", "Steady Commander");
    steady.brain = lib::BrainFacts{};
    inputs.listings.push_back(steady);
    lib::Installed steady_held =
        installed(core, Kind::brain, "steady-commander", "Steady Commander", "steady-commander");
    steady_held.brain = lib::BrainFacts{};
    inputs.installed.push_back(std::move(steady_held));
    lib::Listing picky = listing(Kind::brain, "picky-planner", "Picky Planner");
    picky.brain = lib::BrainFacts{};
    inputs.listings.push_back(std::move(picky));
    lib::Installed picky_held =
        installed(core, Kind::brain, "picky-planner", "Picky Planner", "picky-planner");
    picky_held.brain = lib::BrainFacts{};
    inputs.installed.push_back(std::move(picky_held));
    inputs.default_brain = "steady-commander";
    const lib::Library library = built(std::move(inputs));
    const lib::Entry* steady_row = at(library, core, Kind::brain, "steady-commander");
    OA_CHECK(steady_row != nullptr && steady_row->is_default);
    if (steady_row != nullptr) {
        const kit::Question asked =
            lib::uninstall_question(library, *steady_row, "Brains/steady-commander");
        OA_CHECK(asked.paragraphs.size() == 3);
        if (asked.paragraphs.size() == 3) {
            OA_CHECK_TEXT(
                asked.paragraphs[0].text,
                "Uninstall Steady Commander? Its folder, Brains/steady-commander, is removed."
            );
            OA_CHECK_TEXT(
                asked.paragraphs[1].text,
                "It is your default computer player: new computer seats go back to the built-in "
                "player."
            );
            OA_CHECK_TEXT(asked.paragraphs[2].text, "GET installs it again.");
        }
    }
    const lib::Entry* picky_row = at(library, core, Kind::brain, "picky-planner");
    OA_CHECK(picky_row != nullptr && !picky_row->is_default);
    if (picky_row != nullptr) {
        const kit::Question asked =
            lib::uninstall_question(library, *picky_row, "Brains/picky-planner");
        OA_CHECK(asked.paragraphs.size() == 2);
        for (const kit::Paragraph& paragraph : asked.paragraphs)
            OA_CHECK(paragraph.text.find("default computer player") == std::string::npos);
    }
}

/// UNINSTALL of the layout in use adds its paragraph before the last.
/// Another layout's question does not.
void test_layout_in_use() {
    lib::Inputs inputs = fixture_inputs();
    lib::Listing grid = listing(Kind::key_layout, "grid-test", "Grid Test");
    lib::KeyLayoutFacts grid_facts;
    grid.keys = grid_facts;
    inputs.listings.push_back(grid);
    lib::Installed grid_held =
        installed(core, Kind::key_layout, "grid-test", "Grid Test", "grid-test");
    grid_held.keys = grid_facts;
    inputs.installed.push_back(std::move(grid_held));
    lib::Listing lefty = listing(Kind::key_layout, "lefty-test", "Lefty Test");
    lefty.keys = lib::KeyLayoutFacts{};
    inputs.listings.push_back(lefty);
    lib::Installed lefty_held =
        installed(core, Kind::key_layout, "lefty-test", "Lefty Test", "lefty-test");
    lefty_held.keys = lib::KeyLayoutFacts{};
    inputs.installed.push_back(std::move(lefty_held));
    inputs.keys_layout = "grid-test";
    const lib::Library library = built(std::move(inputs));
    const lib::Entry* grid_row = at(library, core, Kind::key_layout, "grid-test");
    OA_CHECK(grid_row != nullptr && lib::layout_in_use(library, *grid_row));
    if (grid_row != nullptr) {
        const kit::Question asked = lib::uninstall_question(library, *grid_row, "Keys/grid-test");
        OA_CHECK(asked.paragraphs.size() == 3);
        if (asked.paragraphs.size() == 3) {
            OA_CHECK_TEXT(
                asked.paragraphs[1].text,
                "You play with Grid Test now. Uninstall it and go back to OA default? Your own "
                "changes stay saved."
            );
        }
    }
    const lib::Entry* lefty_row = at(library, core, Kind::key_layout, "lefty-test");
    OA_CHECK(lefty_row != nullptr && !lib::layout_in_use(library, *lefty_row));
    if (lefty_row != nullptr) {
        const kit::Question asked = lib::uninstall_question(library, *lefty_row, "Keys/lefty-test");
        OA_CHECK(asked.paragraphs.size() == 2);
    }
}

} // namespace

int main() {
    test_button();
    test_off();
    test_steps();
    test_question();
    test_default_brain();
    test_layout_in_use();
    test_notices();
    test_lookup();
    return oa::test::check_exit_status();
}
