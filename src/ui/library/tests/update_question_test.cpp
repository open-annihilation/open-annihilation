// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The update question over made-up registries and mods: when it is asked
// (an added registry's mod whose rules change or cannot be known) and when
// never (a built-in registry's, one that keeps the rules, a language or map
// pack), its words for a new revision, a new version, an unknown rules hash
// and the mod being played, with the design's sample numbers; UPDATE ALL's
// one question, naming only the added registries' updates; the kit question
// with NOT NOW marked and UPDATE primary; the questions about the mod the
// game plays (SWITCH OFF's, PLAY NOW's refusal and ROLL BACK's), their words
// and buttons; an update that names no revision, which never says "Revision
// 0"; and a lookup that translates.
#include "oa/test/check.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/mod_questions.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/library/update_question.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace lib = oa::ui::library;
namespace kit = oa::ui::kit;
using lib::Kind;
using Rules = lib::UpdateNote::Rules;

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

/// The built-in registry's id and the added one's.
constexpr std::string_view core = "fixture-core";
constexpr std::string_view extra = "fixture-extra";

/// The design's sample download: 3.2 MB.
constexpr uint64_t sample_size = 3355443;

/// A digest whose first four bytes are given and whose others are zero.
///
/// @param b0 the first byte
/// @param b1 the second byte
/// @param b2 the third byte
/// @param b3 the fourth byte
/// @return the digest
oa::base::sha256::Digest digest(uint8_t b0, uint8_t b1, uint8_t b2, uint8_t b3) {
    oa::base::sha256::Digest value{};
    value[0] = b0;
    value[1] = b1;
    value[2] = b2;
    value[3] = b3;
    return value;
}

/// The sample rules hashes "9c1f 4e0b" and "51d2 07aa".
const oa::base::sha256::Digest old_rules = digest(0x9c, 0x1f, 0x4e, 0x0b);
const oa::base::sha256::Digest new_rules = digest(0x51, 0xd2, 0x07, 0xaa);

/// One made-up package installed with an update.
struct Made {
    std::string_view registry{}; ///< its registry
    Kind kind{Kind::mod};        ///< its kind
    std::string_view key{};      ///< its key
    std::string_view name{};     ///< its name
    std::string_view version{};  ///< the version installed
    std::string_view to{};       ///< the update's version
    Rules rules{Rules::same};    ///< what the update does to the rules
};

/// Builds a Library of the two registries and the packages given, each
/// installed at revision 3 with an update to revision 4 of 3.2 MB.
///
/// @param made the packages
/// @return the Library
lib::Library library_of(const std::vector<Made>& made) {
    lib::Inputs inputs;
    lib::Registry built_in;
    built_in.id = std::string(core);
    built_in.name = "Fixture Core";
    built_in.built_in = true;
    lib::Registry added;
    added.id = std::string(extra);
    added.name = "Fixture Extra";
    inputs.registries = {built_in, added};
    for (const Made& each : made) {
        lib::Installed installed;
        installed.registry = std::string(each.registry);
        installed.kind = each.kind;
        installed.key = std::string(each.key);
        installed.name = std::string(each.name);
        installed.version = std::string(each.version);
        installed.revision = 3;
        installed.release = 3;
        installed.rules = old_rules;
        inputs.installed.push_back(std::move(installed));
        lib::UpdateNote update;
        update.registry = std::string(each.registry);
        update.kind = each.kind;
        update.key = std::string(each.key);
        update.to_release = 4;
        update.to_version = std::string(each.to);
        update.to_revision = 4;
        update.size = sample_size;
        update.rules = each.rules;
        update.from_rules = old_rules;
        if (each.rules == Rules::changes)
            update.to_rules = new_rules;
        else if (each.rules == Rules::same)
            update.to_rules = old_rules;
        inputs.updates.push_back(std::move(update));
    }
    lib::Library library;
    lib::refresh(library, std::move(inputs));
    return library;
}

/// Finds an entry.
///
/// @param library the Library
/// @param registry the registry's id
/// @param kind the kind
/// @param key the key
/// @return the entry; null when there is none
const lib::Entry*
at(const lib::Library& library, std::string_view registry, Kind kind, std::string_view key) {
    const lib::Entry* found =
        lib::find_entry(library, {std::string(registry), kind, std::string(key)});
    OA_CHECK(found != nullptr && found->update != nullptr);
    return found != nullptr && found->update != nullptr ? found : nullptr;
}

/// Writes a question's paragraphs as the kit question shows them.
///
/// @param question the kit question
/// @return its paragraphs' texts
std::vector<std::string> paragraphs_of(const kit::Question& question) {
    std::vector<std::string> texts;
    for (const kit::Paragraph& paragraph : question.paragraphs)
        texts.push_back(paragraph.text);
    return texts;
}

void asks_only_for_an_added_registrys_mod_whose_rules_may_change() {
    const lib::Library library = library_of({
        {extra, Kind::mod, "ridge", "Ridge", "4.8", "4.8", Rules::changes},
        {extra, Kind::mod, "lattice", "Lattice", "3.0", "3.0", Rules::unknown},
        {extra, Kind::mod, "cinder", "Cinder", "1.2", "1.2", Rules::same},
        {core, Kind::mod, "dunes", "Dunes", "0.9", "0.9", Rules::changes},
        {core, Kind::mod, "ember", "Ember", "2.0", "2.0", Rules::unknown},
        {core, Kind::mod, "flint", "Flint", "5.1", "5.1", Rules::same},
        {extra, Kind::language, "xx", "Language Pack", "1.0", "1.0", Rules::changes},
        {extra, Kind::map_pack, "dune-maps", "Dune Maps", "1.0", "1.0", Rules::unknown},
    });

    struct Case {
        std::string_view registry{};
        Kind kind{Kind::mod};
        std::string_view key{};
        bool asks{};
    };

    for (const Case& each : {
             Case{extra, Kind::mod, "ridge", true},
             Case{extra, Kind::mod, "lattice", true},
             Case{extra, Kind::mod, "cinder", false},
             Case{core, Kind::mod, "dunes", false},
             Case{core, Kind::mod, "ember", false},
             Case{core, Kind::mod, "flint", false},
             Case{extra, Kind::language, "xx", false},
             Case{extra, Kind::map_pack, "dune-maps", false},
         }) {
        const lib::Entry* entry = at(library, each.registry, each.kind, each.key);
        if (entry == nullptr)
            continue;
        OA_CHECK(lib::asks_before_update(*entry, *entry->update) == each.asks);
        OA_CHECK(lib::update_question(*entry, *entry->update, false).has_value() == each.asks);
        OA_CHECK(lib::update_question(*entry, *entry->update, true).has_value() == each.asks);
    }
}

void the_question_reads_as_the_design_shows_it() {
    const lib::Library library =
        library_of({{extra, Kind::mod, "ridge", "Ridge", "4.8", "4.8", Rules::changes}});
    const lib::Entry* ridge = at(library, extra, Kind::mod, "ridge");
    if (ridge == nullptr)
        return;
    const std::optional<lib::UpdateQuestion> asked =
        lib::update_question(*ridge, *ridge->update, false);
    OA_CHECK(asked.has_value());
    if (!asked)
        return;
    OA_CHECK_TEXT(asked->title, "Update Ridge");
    OA_CHECK_TEXT(asked->heading, "Ridge 4.8 · revision 3 → 4");
    OA_CHECK_TEXT(
        asked->body,
        "Revision 4 changes the game's rules. Saved games from revision 3 won't load under "
        "revision 4. Revision 3 is kept, and Roll back brings it back."
    );
    OA_CHECK(asked->facts.size() == 2);
    if (asked->facts.size() == 2) {
        OA_CHECK_TEXT(asked->facts[0].long_label, "Rules");
        OA_CHECK_TEXT(asked->facts[0].long_value, "9c1f 4e0b → 51d2 07aa");
        OA_CHECK(asked->facts[0].mark == lib::Mark::warn);
        OA_CHECK_TEXT(asked->facts[1].long_label, "Download");
        OA_CHECK_TEXT(asked->facts[1].long_value, "3.2 MB");
    }

    // The kit question: the heading, the body and the facts, NOT NOW marked
    // and the one Escape answers, UPDATE primary.
    const kit::Question question = lib::update_prompt(*asked);
    OA_CHECK_TEXT(question.title, "Update Ridge");
    OA_CHECK(
        paragraphs_of(question) ==
        std::vector<std::string>(
            {"Ridge 4.8 · revision 3 → 4",
             "Revision 4 changes the game's rules. Saved games from revision 3 won't load under "
             "revision 4. Revision 3 is kept, and Roll back brings it back.",
             "Rules: 9c1f 4e0b → 51d2 07aa",
             "Download: 3.2 MB"}
        )
    );
    OA_CHECK(question.buttons.size() == 2);
    if (question.buttons.size() == 2) {
        OA_CHECK_TEXT(question.buttons[0].caption, "Not now");
        OA_CHECK_TEXT(question.buttons[0].id, "not-now");
        OA_CHECK(!question.buttons[0].accent);
        OA_CHECK_TEXT(question.buttons[1].caption, "Update");
        OA_CHECK_TEXT(question.buttons[1].id, "update");
        OA_CHECK(question.buttons[1].accent);
    }
    OA_CHECK(question.marked == 0 && question.cancel_button == 0 && question.primary_button == 1);
    OA_CHECK_TEXT(kit::question_word_of(question), "prompt");
    // Automation names its buttons and its words in the prompt's words, and
    // its text fits at every class.
    for (const kit::SizeClass size_class :
         {kit::SizeClass::compact, kit::SizeClass::regular, kit::SizeClass::large}) {
        kit::Question sized = question;
        sized.size_class = size_class;
        OA_CHECK(!kit::place_question(sized, nullptr).cut);
        const kit::DisplayList list = kit::question_list(sized, nullptr);
        std::vector<std::string> names;
        for (const kit::Control& control : list.controls)
            names.push_back(control.name);
        OA_CHECK(
            names == std::vector<std::string>({"prompt.not-now", "prompt.update", "prompt.body"})
        );
    }

    // For the mod being played, it finishes when the player leaves it.
    const std::optional<lib::UpdateQuestion> playing =
        lib::update_question(*ridge, *ridge->update, true);
    OA_CHECK(playing.has_value());
    if (playing)
        OA_CHECK_TEXT(
            playing->body,
            "Revision 4 changes the game's rules. Saved games from revision 3 won't load under "
            "revision 4. Revision 3 is kept, and Roll back brings it back. It finishes when you "
            "leave Ridge."
        );
}

void a_new_version_and_an_unknown_rules_hash_say_so() {
    const lib::Library library = library_of({
        {extra, Kind::mod, "ridge", "Ridge", "4.8", "4.9", Rules::changes},
        {extra, Kind::mod, "lattice", "Lattice", "3.0", "3.0", Rules::unknown},
        {extra, Kind::mod, "cinder", "Cinder", "1.2", "2.0", Rules::unknown},
    });
    if (const lib::Entry* ridge = at(library, extra, Kind::mod, "ridge")) {
        const auto asked = lib::update_question(*ridge, *ridge->update, false);
        OA_CHECK(asked.has_value());
        if (asked) {
            OA_CHECK_TEXT(asked->title, "Update Ridge");
            OA_CHECK_TEXT(asked->heading, "Ridge 4.8 → 4.9");
            OA_CHECK_TEXT(
                asked->body,
                "4.9 changes the game's rules. Saved games from 4.8 won't load under 4.9. 4.8 is "
                "kept, and Roll back brings it back."
            );
        }
    }
    if (const lib::Entry* lattice = at(library, extra, Kind::mod, "lattice")) {
        const auto asked = lib::update_question(*lattice, *lattice->update, false);
        OA_CHECK(asked.has_value());
        if (asked) {
            OA_CHECK_TEXT(asked->heading, "Lattice 3.0 · revision 3 → 4");
            OA_CHECK_TEXT(
                asked->body,
                "OA can't tell whether revision 4 changes the game's rules. If it does, saved "
                "games from revision 3 won't load under it. Revision 3 is kept, and Roll back "
                "brings it back."
            );
            OA_CHECK(asked->facts.size() == 2);
            if (asked->facts.size() == 2) {
                OA_CHECK_TEXT(asked->facts[0].long_value, "unknown");
                OA_CHECK(asked->facts[0].mark == lib::Mark::warn);
            }
            const auto playing = lib::update_question(*lattice, *lattice->update, true);
            OA_CHECK(playing.has_value());
            if (playing)
                OA_CHECK(playing->body.ends_with(" It finishes when you leave Lattice."));
        }
    }
    if (const lib::Entry* cinder = at(library, extra, Kind::mod, "cinder")) {
        const auto asked = lib::update_question(*cinder, *cinder->update, false);
        OA_CHECK(asked.has_value());
        if (asked) {
            OA_CHECK_TEXT(asked->heading, "Cinder 1.2 → 2.0");
            OA_CHECK_TEXT(
                asked->body,
                "OA can't tell whether 2.0 changes the game's rules. If it does, saved games from "
                "1.2 won't load under it. 1.2 is kept, and Roll back brings it back."
            );
            const kit::Question question = lib::update_prompt(*asked);
            OA_CHECK(paragraphs_of(question).size() == 4);
            OA_CHECK(paragraphs_of(question).back() == "Download: 3.2 MB");
        }
    }
}

void update_all_asks_once_about_the_added_registries_updates() {
    const lib::Library library = library_of({
        {core, Kind::mod, "dunes", "Dunes", "0.9", "0.9", Rules::changes},
        {core, Kind::mod, "ember", "Ember", "2.0", "2.0", Rules::unknown},
        {extra, Kind::mod, "ridge", "Ridge", "4.8", "4.8", Rules::changes},
        {extra, Kind::mod, "cinder", "Cinder", "1.2", "1.2", Rules::same},
        {extra, Kind::language, "xx", "Language Pack", "1.0", "1.0", Rules::changes},
    });
    // A built-in registry's update whose rules change, beside an added one's:
    // only the added one is named.
    std::vector<lib::Entry> both;
    for (const auto& [registry, key] :
         {std::pair{core, std::string_view("dunes")}, std::pair{extra, std::string_view("ridge")}})
        if (const lib::Entry* entry = at(library, registry, Kind::mod, key))
            both.push_back(*entry);
    const auto asked = lib::update_all_question(both);
    OA_CHECK(asked.has_value());
    if (asked) {
        OA_CHECK_TEXT(asked->title, "Update all");
        OA_CHECK(asked->heading.empty() && asked->facts.empty());
        OA_CHECK_TEXT(
            asked->body,
            "1 update changes the game's rules: Ridge. Saved games from its current version won't "
            "load under the new one. It is kept, and Roll back brings it back."
        );
        const kit::Question question = lib::update_prompt(*asked);
        OA_CHECK(paragraphs_of(question) == std::vector<std::string>({asked->body}));
        OA_CHECK(question.buttons.size() == 2 && question.marked == 0);
    }
    // Nothing to ask: built-in registries' updates, one that keeps the
    // rules, a language pack, and an entry with no update.
    std::vector<lib::Entry> quiet;
    for (const lib::Entry& entry : library.entries)
        if (entry.id.key != "ridge")
            quiet.push_back(entry);
    lib::Entry without;
    without.id = {std::string(extra), Kind::mod, "gone"};
    quiet.push_back(without);
    OA_CHECK(!lib::update_all_question(quiet).has_value());
    OA_CHECK(!lib::update_all_question({}).has_value());

    // Several are counted, and up to three named, then how many more.
    const lib::Library many = library_of({
        {extra, Kind::mod, "a", "Alder", "1.0", "1.0", Rules::changes},
        {core, Kind::mod, "b", "Birch", "1.0", "1.0", Rules::changes},
        {extra, Kind::mod, "c", "Cedar", "1.0", "1.0", Rules::unknown},
        {extra, Kind::mod, "d", "Dogwood", "1.0", "2.0", Rules::changes},
        {extra, Kind::mod, "e", "Elm", "1.0", "1.0", Rules::unknown},
        {extra, Kind::mod, "f", "Fir", "1.0", "1.0", Rules::changes},
    });
    std::vector<lib::Entry> listed;
    for (const std::string_view key : {"a", "b", "c", "d", "e", "f"})
        for (const lib::Entry& entry : many.entries)
            if (entry.id.key == key)
                listed.push_back(entry);
    const auto five = lib::update_all_question(listed);
    OA_CHECK(five.has_value());
    if (five)
        OA_CHECK_TEXT(
            five->body,
            "5 updates change the game's rules: Alder, Cedar, Dogwood and 2 more. Saved games "
            "from their current versions won't load under the new ones. Each is kept, and Roll "
            "back brings it back."
        );
    listed.resize(3);
    const auto two = lib::update_all_question(listed);
    OA_CHECK(two.has_value());
    if (two)
        OA_CHECK(two->body.starts_with("2 updates change the game's rules: Alder, Cedar. "));
}

/// An update that keeps the version and names no revision, as a catalogue
/// that leaves the revision out gives one, never says "Revision 0": the
/// status line calls it an update, and its question's heading names the
/// version alone and its body the new copy.
void an_update_without_a_revision_never_says_revision_0() {
    const lib::Library made = library_of({
        {core, Kind::language, "pt-br", "Portugues", "1.0", "1.0", Rules::same},
        {core, Kind::mod, "lattice", "Lattice", "3.0", "3.0", Rules::changes},
        {extra, Kind::mod, "cinder", "Cinder", "1.2", "1.2", Rules::unknown},
    });
    lib::Inputs inputs = made.inputs;
    for (lib::UpdateNote& update : inputs.updates)
        update.to_revision = 0;
    lib::Library library;
    lib::refresh(library, std::move(inputs));

    if (const lib::Entry* language = at(library, core, Kind::language, "pt-br"))
        OA_CHECK_TEXT(
            lib::status_line(library, *language), "Update available · 3.2 MB to download"
        );
    if (const lib::Entry* cinder = at(library, extra, Kind::mod, "cinder")) {
        const auto asked = lib::update_question(*cinder, *cinder->update, false);
        OA_CHECK(asked.has_value());
        if (asked) {
            OA_CHECK_TEXT(asked->heading, "Cinder 1.2");
            OA_CHECK_TEXT(
                asked->body,
                "OA can't tell whether the new copy changes the game's rules. If it does, saved "
                "games from revision 3 won't load under it. Revision 3 is kept, and Roll back "
                "brings it back."
            );
        }
    }
}

void the_questions_about_the_mod_played_keep_the_mods_pages_words() {
    // SWITCH OFF's: the Switch Mod question of No Mod, CANCEL marked.
    const kit::Question off = lib::switch_off_question();
    OA_CHECK_TEXT(off.title, "SWITCH MOD");
    OA_CHECK(off.paragraphs.size() == 1);
    if (off.paragraphs.size() == 1)
        OA_CHECK_TEXT(
            off.paragraphs[0].text,
            "Switch to No Mod now? The game reloads its data for the new mod and returns to the "
            "main menu. Your other settings are kept."
        );
    OA_CHECK(off.buttons.size() == 2);
    if (off.buttons.size() == 2) {
        OA_CHECK(off.buttons[0].caption == "Cancel" && off.buttons[0].id == "cancel");
        OA_CHECK(!off.buttons[0].accent);
        OA_CHECK(off.buttons[1].caption == "Switch" && off.buttons[1].id == "switch");
        OA_CHECK(off.buttons[1].accent);
    }
    OA_CHECK(off.cancel_button == 0 && off.primary_button == 1 && off.marked == 0);

    // PLAY NOW's refusal: the reason under the same title, and OK.
    const kit::Question refused =
        lib::switch_refused_note("That folder cannot be played; the log says why.");
    OA_CHECK_TEXT(refused.title, "SWITCH MOD");
    OA_CHECK(refused.paragraphs.size() == 1);
    if (refused.paragraphs.size() == 1)
        OA_CHECK_TEXT(
            refused.paragraphs[0].text, "That folder cannot be played; the log says why."
        );
    OA_CHECK(refused.buttons.size() == 1);
    if (refused.buttons.size() == 1)
        OA_CHECK(
            refused.buttons[0].caption == "OK" && refused.buttons[0].id == "ok" &&
            refused.buttons[0].accent
        );
    OA_CHECK(refused.cancel_button == 0 && refused.primary_button == 0 && refused.marked == 0);

    // ROLL BACK's: the Roll Back Mod question, with the reload line for the
    // mod the game plays.
    const kit::Question back =
        lib::roll_back_question("Ridge", "4.8 revision 3", "4.8 revision 2", false);
    OA_CHECK_TEXT(back.title, "ROLL BACK MOD");
    OA_CHECK(back.paragraphs.size() == 1);
    if (back.paragraphs.size() == 1)
        OA_CHECK_TEXT(
            back.paragraphs[0].text,
            "Roll back Ridge to 4.8 revision 2? 4.8 revision 3 is kept in its place, and Roll "
            "back again brings it back."
        );
    OA_CHECK(back.buttons.size() == 2);
    if (back.buttons.size() == 2) {
        OA_CHECK(back.buttons[0].caption == "Cancel" && back.buttons[0].id == "cancel");
        OA_CHECK(back.buttons[1].caption == "Roll back" && back.buttons[1].id == "roll-back");
        OA_CHECK(back.buttons[1].accent);
    }
    OA_CHECK(back.cancel_button == 0 && back.primary_button == 1 && back.marked == 0);
    const kit::Question played = lib::roll_back_question("Ridge", "1.1", "1.0", true);
    OA_CHECK(played.paragraphs.size() == 1);
    if (played.paragraphs.size() == 1)
        OA_CHECK_TEXT(
            played.paragraphs[0].text,
            "Roll back Ridge to 1.0? 1.1 is kept in its place, and Roll back again brings it "
            "back. The game reloads its data for the mod and returns to the main menu."
        );
}

/// A lookup that shows a few patterns in other words, keeping their places.
///
/// @param english the English pattern
/// @return its other words, or the English
std::string_view other_words(void* /*context*/, std::string_view english) {
    if (english == lib::words::question_title)
        return "Mettre à jour {name}";
    if (english == lib::words::action_not_now)
        return "PAS MAINTENANT";
    if (english == lib::words::question_fact)
        return "{label} : {value}";
    if (english == lib::words::switch_title)
        return "CHANGER DE MOD";
    if (english == lib::words::switch_ask)
        return "Passer à {title} ?";
    if (english == lib::words::no_mod)
        return "Aucun mod";
    return english;
}

void every_word_is_looked_up() {
    lib::TextHooks hooks;
    hooks.shown = other_words;
    const lib::Library library =
        library_of({{extra, Kind::mod, "ridge", "Ridge", "4.8", "4.8", Rules::changes}});
    const lib::Entry* ridge = at(library, extra, Kind::mod, "ridge");
    if (ridge == nullptr)
        return;
    const auto asked = lib::update_question(*ridge, *ridge->update, false, hooks);
    OA_CHECK(asked.has_value());
    if (!asked)
        return;
    OA_CHECK_TEXT(asked->title, "Mettre à jour Ridge");
    const kit::Question question = lib::update_prompt(*asked, hooks);
    OA_CHECK(!question.buttons.empty() && question.buttons[0].caption == "PAS MAINTENANT");
    OA_CHECK(paragraphs_of(question).size() == 4);
    if (paragraphs_of(question).size() == 4)
        OA_CHECK_TEXT(paragraphs_of(question)[2], "Rules : 9c1f 4e0b → 51d2 07aa");
    const kit::Question off = lib::switch_off_question(hooks);
    OA_CHECK_TEXT(off.title, "CHANGER DE MOD");
    OA_CHECK(off.paragraphs.size() == 1);
    if (off.paragraphs.size() == 1)
        OA_CHECK_TEXT(off.paragraphs[0].text, "Passer à Aucun mod ?");
}

} // namespace

int main() {
    asks_only_for_an_added_registrys_mod_whose_rules_may_change();
    the_question_reads_as_the_design_shows_it();
    a_new_version_and_an_unknown_rules_hash_say_so();
    update_all_asks_once_about_the_added_registries_updates();
    an_update_without_a_revision_never_says_revision_0();
    the_questions_about_the_mod_played_keep_the_mods_pages_words();
    every_word_is_looked_up();
    return oa::test::check_exit_status();
}
