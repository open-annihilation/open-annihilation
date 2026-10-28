// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The language registry: its entries and their order, tags and 3.1c's words
// found without regard to case, fallback chains ending in English, the
// operating system's locales normalised and matched alone and in order, the
// settings' choice, and the interface catalogue read from small built
// files, malformed and oversized ones included, and merged by text and
// language, with a generation each installation of it moves.

#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/languages/unit_texts.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace languages = oa::data::languages;

/// Returns the tag of a language a lookup found, or "none".
///
/// @param language the lookup's result
/// @return its tag
std::string_view tag_of(const languages::Language* language) {
    return language != nullptr ? language->tag : std::string_view{"none"};
}

/// Drops installed and available languages, so a case reads none another left.
void clear_pack_languages() {
    languages::set_pack_languages({}, {});
}

/// Makes an entry with the fields the registry keeps.
///
/// @param tag its tag
/// @param endonym its name in itself
/// @param english_name its name in English
/// @param word the word the game data knows it by
/// @param needs what drawing its text needs
/// @return the entry
languages::LanguageEntry make_entry(
    std::string_view tag,
    std::string_view endonym,
    std::string_view english_name,
    std::string_view word,
    languages::TextNeeds needs = languages::TextNeeds::game_fonts
) {
    languages::LanguageEntry entry;
    entry.tag = tag;
    entry.endonym = endonym;
    entry.english_name = english_name;
    entry.word = word;
    entry.needs = needs;
    return entry;
}

/// The registry holds English, German, Spanish, French and Italian as
/// built-in languages, English first and the others in the order of their
/// own names, and Simplified Chinese as available. Each has its name in
/// itself and the game data's word for it.
void registry_lists_the_built_in_languages_in_menu_order() {
    clear_pack_languages();
    const auto known = languages::known_languages();
    OA_CHECK(known.size() == 6);
    const std::array<std::string_view, 6> tags{"en", "de", "es", "fr", "it", "zh-Hans"};
    const std::array<std::string_view, 6> endonyms{
        "English",
        "Deutsch",
        "Espa\xC3\xB1"
        "ol",
        "Fran\xC3\xA7"
        "ais",
        "Italiano",
        "\347\256\200\344\275\223\344\270\255\346\226\207"
    };
    const std::array<std::string_view, 6> words{
        "English", "German", "Spanish", "French", "Italian", "Chinese"
    };
    for (std::size_t index = 0; index < known.size() && index < tags.size(); ++index) {
        OA_CHECK(known[index]->tag == tags[index]);
        OA_CHECK(known[index]->endonym == endonyms[index]);
        OA_CHECK(known[index]->game_name == words[index]);
        const bool available = known[index]->tag == "zh-Hans";
        OA_CHECK(
            known[index]->source ==
            (available ? languages::Source::available : languages::Source::built_in)
        );
        OA_CHECK(
            known[index]->needs ==
            (available ? languages::TextNeeds::modern_fonts : languages::TextNeeds::game_fonts)
        );
        OA_CHECK(languages::drawable(*known[index]));
        OA_CHECK(languages::playable(*known[index]) == !available);
    }
    OA_CHECK(&languages::english() == known[0]);
    OA_CHECK(languages::english().english_name == "English");
    // After English, the endonyms are in order.
    for (std::size_t index = 2; index < known.size(); ++index)
        OA_CHECK(known[index - 1]->endonym < known[index]->endonym);
}

void only_the_needs_this_build_meets_are_drawable() {
    clear_pack_languages();
    languages::Language language{};
    language.needs = languages::TextNeeds::game_fonts;
    OA_CHECK(languages::drawable(language));
    language.needs = languages::TextNeeds::modern_fonts;
    OA_CHECK(languages::drawable(language));
    language.needs = languages::TextNeeds::more_font_faces;
    OA_CHECK(!languages::drawable(language));
    language.needs = languages::TextNeeds::text_shaping;
    OA_CHECK(!languages::drawable(language));
}

/// Tags and 3.1c's words are found without regard to case.
// Simplified Chinese turns Unicode chat on whether or not its pack is
// there; a language of the code page only when a pack of it asks.
void languages_in_utf8_turn_unicode_chat_on() {
    clear_pack_languages();
    const auto* chinese = languages::find_by_tag("zh-Hans");
    const auto* german = languages::find_by_tag("de");
    OA_CHECK(chinese != nullptr && german != nullptr);
    if (chinese == nullptr || german == nullptr)
        return;
    OA_CHECK(languages::turns_unicode_chat_on(*chinese, false));
    OA_CHECK(languages::turns_unicode_chat_on(*chinese, true));
    OA_CHECK(!languages::turns_unicode_chat_on(*german, false));
    OA_CHECK(languages::turns_unicode_chat_on(*german, true));
    OA_CHECK(!languages::turns_unicode_chat_on(languages::english(), false));
}

void tags_and_game_words_are_found_without_regard_to_case() {
    clear_pack_languages();
    OA_CHECK(tag_of(languages::find_by_tag("de")) == "de");
    OA_CHECK(tag_of(languages::find_by_tag("DE")) == "de");
    OA_CHECK(tag_of(languages::find_by_tag("Fr")) == "fr");
    OA_CHECK(tag_of(languages::find_by_tag("en")) == "en");
    OA_CHECK(tag_of(languages::find_by_tag("pt")) == "none");
    OA_CHECK(tag_of(languages::find_by_tag("")) == "none");
    OA_CHECK(tag_of(languages::find_by_tag("de-AT")) == "none");
    // 3.1c's command line names a language by the data's word for it.
    OA_CHECK(tag_of(languages::find_by_game_name("german")) == "de");
    OA_CHECK(tag_of(languages::find_by_game_name("GERMAN")) == "de");
    OA_CHECK(tag_of(languages::find_by_game_name("Spanish")) == "es");
    OA_CHECK(tag_of(languages::find_by_game_name("french")) == "fr");
    OA_CHECK(tag_of(languages::find_by_game_name("italian")) == "it");
    OA_CHECK(tag_of(languages::find_by_game_name("english")) == "en");
    // Simplified Chinese is available until its pack is installed, so 3.1c's
    // word does not find it yet.
    OA_CHECK(tag_of(languages::find_by_game_name("chinese")) == "none");
    languages::LanguageEntry chinese;
    chinese.tag = "zh-Hans";
    chinese.endonym = "\347\256\200\344\275\223\344\270\255\346\226\207";
    chinese.english_name = "Chinese (Simplified)";
    chinese.word = "Chinese";
    chinese.needs = languages::TextNeeds::modern_fonts;
    languages::set_pack_languages(std::array<languages::LanguageEntry, 1>{chinese}, {});
    OA_CHECK(tag_of(languages::find_by_game_name("chinese")) == "zh-Hans");
    clear_pack_languages();
    OA_CHECK(tag_of(languages::find_by_game_name("piglatin")) == "none");
    OA_CHECK(tag_of(languages::find_by_game_name("")) == "none");
}

/// Every chain ends in English, which is its own whole chain.
void fallback_chains_end_in_english() {
    clear_pack_languages();
    const auto english_chain = languages::fallback_chain(languages::english());
    OA_CHECK(english_chain.count == 1);
    OA_CHECK(english_chain.view()[0] == &languages::english());
    for (const languages::Language* language : languages::known_languages()) {
        const auto chain = languages::fallback_chain(*language);
        OA_CHECK(chain.count >= 1);
        OA_CHECK(chain.view().front() == language);
        OA_CHECK(chain.view().back() == &languages::english());
    }
    const auto french = languages::fallback_chain(*languages::find_by_tag("fr"));
    OA_CHECK(french.count == 2);
    // An entry's own fallbacks come between it and English, each once, and
    // English listed among them still comes last.
    const std::array<std::string_view, 4> fallbacks{"en", "fr", "fr", "xx"};
    const std::array<std::string_view, 1> locales{"wa"};
    const languages::Language walloon{"wa", "Walon", "Walloon", "Walloon", locales, fallbacks};
    const auto chain = languages::fallback_chain(walloon);
    OA_CHECK(chain.count == 3);
    OA_CHECK(chain.view()[0] == &walloon);
    OA_CHECK(tag_of(chain.view()[1]) == "fr");
    OA_CHECK(chain.view()[2] == &languages::english());
}

/// The operating system's locales are written as BCP-47 tags.
void locales_are_normalised() {
    clear_pack_languages();
    OA_CHECK(languages::normalised_locale("de_DE.UTF-8@euro") == "de-DE");
    OA_CHECK(languages::normalised_locale("fr_CA") == "fr-CA");
    OA_CHECK(languages::normalised_locale("en-GB") == "en-GB");
    OA_CHECK(languages::normalised_locale(" es ") == "es");
    OA_CHECK(languages::normalised_locale("zh_Hant_TW") == "zh-Hant-TW");
    OA_CHECK(languages::normalised_locale("C").empty());
    OA_CHECK(languages::normalised_locale("C.UTF-8").empty());
    OA_CHECK(languages::normalised_locale("POSIX").empty());
    OA_CHECK(languages::normalised_locale("").empty());
    OA_CHECK(languages::normalised_locale("de DE").empty());
    OA_CHECK(languages::normalised_locale("-de").empty());
    OA_CHECK(languages::normalised_locale(std::string(65, 'a')).empty());
}

/// A locale chooses the language whose locale is its whole tag or its
/// leading subtags.
void locales_choose_languages_by_their_leading_subtags() {
    clear_pack_languages();
    OA_CHECK(tag_of(languages::match_locale("de")) == "de");
    OA_CHECK(tag_of(languages::match_locale("de-AT")) == "de");
    OA_CHECK(tag_of(languages::match_locale("de_CH.UTF-8")) == "de");
    OA_CHECK(tag_of(languages::match_locale("DE-de")) == "de");
    OA_CHECK(tag_of(languages::match_locale("fr-CA")) == "fr");
    OA_CHECK(tag_of(languages::match_locale("it_IT")) == "it");
    OA_CHECK(tag_of(languages::match_locale("es-419")) == "es");
    OA_CHECK(tag_of(languages::match_locale("en-GB")) == "en");
    OA_CHECK(tag_of(languages::match_locale("pt-BR")) == "none");
    // Simplified Chinese is available until its pack is installed. Its
    // script, its places and Chinese alone want it, and none of them chooses
    // it. Traditional Chinese's script and places choose none either way.
    OA_CHECK(tag_of(languages::match_locale("zh-CN")) == "none");
    OA_CHECK(tag_of(languages::match_locale("zh-CN", true)) == "zh-Hans");
    OA_CHECK(tag_of(languages::match_locale("zh_SG.UTF-8")) == "none");
    OA_CHECK(tag_of(languages::match_locale("zh_SG.UTF-8", true)) == "zh-Hans");
    OA_CHECK(tag_of(languages::match_locale("zh-Hans-CN")) == "none");
    OA_CHECK(tag_of(languages::match_locale("zh-Hans-CN", true)) == "zh-Hans");
    OA_CHECK(tag_of(languages::match_locale("zh")) == "none");
    OA_CHECK(tag_of(languages::match_locale("zh", true)) == "zh-Hans");
    OA_CHECK(tag_of(languages::match_locale("zh-TW")) == "none");
    OA_CHECK(tag_of(languages::match_locale("zh-TW", true)) == "none");
    OA_CHECK(tag_of(languages::match_locale("zh-Hant-HK")) == "none");
    OA_CHECK(tag_of(languages::match_locale("zh_HK")) == "none");
    OA_CHECK(tag_of(languages::match_locale("deu")) == "none");
    OA_CHECK(tag_of(languages::match_locale("frx")) == "none");
    OA_CHECK(tag_of(languages::match_locale("C")) == "none");
}

/// The first preferred locale that chooses a known language wins; none
/// leaves English.
void preferred_locales_choose_the_first_known_language() {
    clear_pack_languages();
    const auto pick = [](std::vector<std::string> locales) {
        return languages::preferred_language(locales).tag;
    };
    OA_CHECK(pick({"pt-BR", "fr-FR", "de"}) == "fr");
    OA_CHECK(pick({"de_DE.UTF-8", "fr"}) == "de");
    OA_CHECK(pick({"en-US", "de"}) == "en");
    OA_CHECK(pick({"ja-JP", "zh-Hant-TW", "ko"}) == "en");
    OA_CHECK(pick({"zh-TW", "zh-CN"}) == "en");
    const std::vector<std::string> chinese_locales{"zh-TW", "zh-CN"};
    OA_CHECK(tag_of(languages::wanted_language(chinese_locales)) == "zh-Hans");
    OA_CHECK(pick({"C", "es_ES"}) == "es");
    OA_CHECK(pick({}) == "en");
}

/// The settings' choice: a known tag, else the operating system's language.
void settings_choice_names_a_language_or_the_system_one() {
    clear_pack_languages();
    const languages::Language& german = *languages::find_by_tag("de");
    const languages::Language& italian = *languages::find_by_tag("it");
    OA_CHECK(&languages::chosen_language(languages::system_choice, german) == &german);
    OA_CHECK(&languages::chosen_language("it", german) == &italian);
    OA_CHECK(&languages::chosen_language("IT", german) == &italian);
    OA_CHECK(&languages::chosen_language("en", german) == &languages::english());
    OA_CHECK(&languages::chosen_language("", german) == &german);
    OA_CHECK(&languages::chosen_language("xx-YY", german) == &german);
}

/// A small catalogue file: two texts, one translated into French and
/// German, one into French alone, with keys in mixed case.
constexpr std::string_view kCatalogue = "[Mouse wheel zoom]\n"
                                        "\t{\n"
                                        "\tFR=Zoom \xC3\xA0 la molette;\n"
                                        "\tde=Mausrad-Zoom;\n"
                                        "\t}\n"
                                        "[Font shadow]\n"
                                        "\t{\n"
                                        "\tfr=Ombre du texte;\n"
                                        "\tit=;\n"
                                        "\t}\n";

/// Texts are looked up by their English, in the language's chain.
void catalogue_translates_by_english_text() {
    clear_pack_languages();
    languages::InterfaceText catalogue;
    std::string error;
    OA_CHECK(catalogue.add(kCatalogue, &error));
    OA_CHECK(error.empty());
    OA_CHECK(catalogue.size() == 3);
    const auto& french = *languages::find_by_tag("fr");
    const auto& german = *languages::find_by_tag("de");
    const auto& italian = *languages::find_by_tag("it");
    OA_CHECK(catalogue.text("Mouse wheel zoom", french) == "Zoom \xC3\xA0 la molette");
    OA_CHECK(catalogue.text("Mouse wheel zoom", german) == "Mausrad-Zoom");
    OA_CHECK(catalogue.text("Font shadow", german) == "Font shadow");
    // An empty value adds nothing, so Italian falls back to English.
    OA_CHECK(catalogue.text("Font shadow", italian) == "Font shadow");
    OA_CHECK(catalogue.text("Mouse wheel zoom", languages::english()) == "Mouse wheel zoom");
    // The lookup is by the exact text.
    OA_CHECK(catalogue.text("mouse wheel zoom", french) == "mouse wheel zoom");
    OA_CHECK(catalogue.text("Not in the catalogue", french) == "Not in the catalogue");
    // A later file replaces a translation and adds to the others.
    OA_CHECK(catalogue.add("[Font shadow]\n{\nfr=Ombre;\nes=Sombra;\n}\n"));
    OA_CHECK(catalogue.text("Font shadow", french) == "Ombre");
    OA_CHECK(catalogue.text("Font shadow", *languages::find_by_tag("es")) == "Sombra");
    OA_CHECK(catalogue.text("Mouse wheel zoom", german) == "Mausrad-Zoom");
    OA_CHECK(catalogue.size() == 4);
}

/// A file that does not parse, or is too large, adds nothing.
void malformed_catalogues_add_nothing() {
    clear_pack_languages();
    languages::InterfaceText catalogue;
    std::string error;
    OA_CHECK(!catalogue.add("[Mouse wheel zoom]\n{\nfr=Zoom\n}\n", &error));
    OA_CHECK(!error.empty());
    OA_CHECK(catalogue.size() == 0);
    error.clear();
    OA_CHECK(!catalogue.add("[Mouse wheel zoom\n{\nfr=Zoom;\n}\n", &error));
    OA_CHECK(!error.empty());
    error.clear();
    const std::string oversized(languages::most_catalogue_bytes + 1, ' ');
    OA_CHECK(!catalogue.add(oversized, &error));
    OA_CHECK(!error.empty());
    OA_CHECK(catalogue.size() == 0);
    // An empty file is a catalogue with nothing in it.
    OA_CHECK(catalogue.add(""));
    OA_CHECK(catalogue.size() == 0);
}

/// Files merge into one catalogue by English text and language: a text
/// already held keeps the translations a later file does not give, a later
/// file's translation of a text into a language replaces the earlier one,
/// and within one file the last one read is kept.
void catalogue_files_merge_by_text_and_language() {
    clear_pack_languages();
    const auto& french = *languages::find_by_tag("fr");
    const auto& german = *languages::find_by_tag("de");
    const auto& spanish = *languages::find_by_tag("es");
    languages::InterfaceText catalogue;
    OA_CHECK(catalogue.add(
        "[Bravo]\n{\nfr=Bravo fr;\nde=Bravo de;\n}\n"
        "[Delta]\n{\nfr=Delta fr;\n}\n"
        "[Delta]\n{\nFR=Delta fr again;\n}\n"
    ));
    OA_CHECK(catalogue.size() == 3);
    OA_CHECK(catalogue.text("Delta", french) == "Delta fr again");
    // Texts before, between and after the held ones, one of them in UTF-8.
    OA_CHECK(catalogue.add(
        "[Alpha]\n{\nes=Alfa;\n}\n"
        "[Charlie]\n{\nde=Charlie de;\n}\n"
        "[Delta]\n{\nde=Delta de;\nfr=Delta fr last;\n}\n"
        "[Echo]\n{\nfr=\xC3\x89\x63ho;\n}\n"
        "[\xC3\x89t\xC3\xA9]\n{\nfr=\xC3\x89t\xC3\xA9 fr;\n}\n"
    ));
    OA_CHECK(catalogue.size() == 8);
    OA_CHECK(catalogue.text("Alpha", spanish) == "Alfa");
    OA_CHECK(catalogue.text("Alpha", french) == "Alpha");
    OA_CHECK(catalogue.text("Bravo", french) == "Bravo fr");
    OA_CHECK(catalogue.text("Bravo", german) == "Bravo de");
    OA_CHECK(catalogue.text("Charlie", german) == "Charlie de");
    OA_CHECK(catalogue.text("Delta", french) == "Delta fr last");
    OA_CHECK(catalogue.text("Delta", german) == "Delta de");
    OA_CHECK(catalogue.text("Echo", french) == "\xC3\x89\x63ho");
    OA_CHECK(catalogue.text("\xC3\x89t\xC3\xA9", french) == "\xC3\x89t\xC3\xA9 fr");
    OA_CHECK(catalogue.text("\xC3\x89t\xC3\xA9", german) == "\xC3\x89t\xC3\xA9");
    // Texts the catalogue does not hold, before, among and after its own.
    OA_CHECK(catalogue.text("", french).empty());
    OA_CHECK(catalogue.text("Al", french) == "Al");
    OA_CHECK(catalogue.text("Alphabet", french) == "Alphabet");
    OA_CHECK(catalogue.text("\xC3\xBF", french) == "\xC3\xBF");
}

/// Tags are read without regard to case and with '_' as '-', and a text
/// shows in the first language of the chain that has it: the language
/// itself, its fallbacks, then English, whose own translation counts too.
void catalogue_follows_tags_and_fallback_chains() {
    clear_pack_languages();
    languages::InterfaceText catalogue;
    OA_CHECK(catalogue.add(
        "[Settings]\n{\nZH_hans=\xE8\xAE\xBE\xE7\xBD\xAE;\n"
        "fr=Param\xC3\xA8tres;\nWA=Apontiaedjes;\n}\n"
        "[Back]\n{\nfr=Retour;\n}\n"
        "[Colour]\n{\nEN=Color;\n}\n"
    ));
    OA_CHECK(catalogue.size() == 5);
    const languages::Language* chinese = languages::find_by_tag("zh-Hans");
    OA_CHECK(chinese != nullptr);
    if (chinese != nullptr)
        OA_CHECK(catalogue.text("Settings", *chinese) == "\xE8\xAE\xBE\xE7\xBD\xAE");
    const std::array<std::string_view, 1> fallbacks{"fr"};
    const std::array<std::string_view, 1> locales{"wa"};
    const languages::Language walloon{"wa", "Walon", "Walloon", "Walloon", locales, fallbacks};
    OA_CHECK(catalogue.text("Settings", walloon) == "Apontiaedjes");
    OA_CHECK(catalogue.text("Back", walloon) == "Retour");
    OA_CHECK(catalogue.text("Colour", walloon) == "Color");
    OA_CHECK(catalogue.text("Colour", languages::english()) == "Color");
    OA_CHECK(catalogue.text("Back", languages::english()) == "Back");
    OA_CHECK(catalogue.text("Not in the catalogue", walloon) == "Not in the catalogue");
}

/// A refused file, or one whose every value is empty, leaves the catalogue
/// as it was.
void refused_catalogues_leave_the_catalogue_as_it_was() {
    clear_pack_languages();
    const auto& german = *languages::find_by_tag("de");
    languages::InterfaceText catalogue;
    OA_CHECK(catalogue.add(kCatalogue));
    OA_CHECK(catalogue.size() == 3);
    std::string error;
    OA_CHECK(
        !catalogue.add("[Cancel]\n{\nde=Abbrechen;\n}\n[Mouse wheel zoom]\n{\nde=Neu\n}\n", &error)
    );
    OA_CHECK(!error.empty());
    std::string oversized = "[Cancel]\n{\nde=Abbrechen;\n}\n";
    oversized.resize(languages::most_catalogue_bytes + 1, ' ');
    error.clear();
    OA_CHECK(!catalogue.add(oversized, &error));
    OA_CHECK(!error.empty());
    OA_CHECK(catalogue.add("[Cancel]\n{\nde=;\n}\n[Mouse wheel zoom]\n{\nde=;\n}\n"));
    OA_CHECK(catalogue.size() == 3);
    OA_CHECK(catalogue.text("Cancel", german) == "Cancel");
    OA_CHECK(catalogue.text("Mouse wheel zoom", german) == "Mausrad-Zoom");
}

/// An add leaves the texts it does not replace where they were, so a view
/// of one taken before it still reads the same text.
void catalogue_keeps_texts_in_place_through_later_adds() {
    clear_pack_languages();
    const auto& french = *languages::find_by_tag("fr");
    const auto& german = *languages::find_by_tag("de");
    languages::InterfaceText catalogue;
    OA_CHECK(catalogue.add(kCatalogue));
    const std::string_view zoom = catalogue.text("Mouse wheel zoom", german);
    const std::string_view shadow = catalogue.text("Font shadow", french);
    OA_CHECK(catalogue.add(
        "[Font shadow]\n{\nde=Schatten;\n}\n[Cancel]\n{\nde=Abbrechen;\n}\n"
        "[Mouse wheel zoom]\n{\nfr=Zoom;\n}\n"
    ));
    OA_CHECK(zoom == "Mausrad-Zoom");
    OA_CHECK(zoom.data() == catalogue.text("Mouse wheel zoom", german).data());
    OA_CHECK(shadow == "Ombre du texte");
    OA_CHECK(shadow.data() == catalogue.text("Font shadow", french).data());
    OA_CHECK(catalogue.text("Mouse wheel zoom", french) == "Zoom");
    OA_CHECK(catalogue.text("Font shadow", german) == "Schatten");
}

/// The installed catalogue and language answer interface_text; without a
/// catalogue every word is English.
void installed_language_answers_interface_text() {
    clear_pack_languages();
    OA_CHECK(&languages::interface_language() == &languages::english());
    OA_CHECK(languages::interface_text("Mouse wheel zoom") == "Mouse wheel zoom");
    languages::InterfaceText catalogue;
    OA_CHECK(catalogue.add(kCatalogue));
    const auto& french = *languages::find_by_tag("fr");
    languages::set_interface_language(&catalogue, french);
    OA_CHECK(&languages::interface_language() == &french);
    OA_CHECK(languages::interface_text("Mouse wheel zoom") == "Zoom \xC3\xA0 la molette");
    OA_CHECK(languages::interface_text("Cancel") == "Cancel");
    languages::set_interface_language(nullptr, french);
    OA_CHECK(languages::interface_text("Mouse wheel zoom") == "Mouse wheel zoom");
    languages::set_interface_language(nullptr, languages::english());
}

/// Every installation moves the interface's generation, the same catalogue
/// and language installed again too, as a pack read again installs them
/// after replacing the catalogue's texts in place; reading changes nothing.
void every_installation_moves_the_interface_generation() {
    clear_pack_languages();
    languages::InterfaceText catalogue;
    OA_CHECK(catalogue.add(kCatalogue));
    const auto& french = *languages::find_by_tag("fr");
    languages::set_interface_language(&catalogue, french);
    const uint64_t first = languages::interface_generation();
    OA_CHECK(languages::interface_text("Mouse wheel zoom") == "Zoom \xC3\xA0 la molette");
    OA_CHECK(languages::interface_generation() == first);
    OA_CHECK(catalogue.add("[Mouse wheel zoom]\n{\nfr=Molette;\n}\n"));
    languages::set_interface_language(&catalogue, french);
    OA_CHECK(languages::interface_text("Mouse wheel zoom") == "Molette");
    const uint64_t again = languages::interface_generation();
    OA_CHECK(again != first);
    OA_CHECK(&languages::interface_language() == &french);
    languages::set_interface_language(nullptr, languages::english());
    OA_CHECK(languages::interface_generation() != again);
}

/// A named language is read from the installed catalogue. The language the
/// interface shows is left as it was, and without a catalogue the word is English.
void named_language_reads_one_text() {
    clear_pack_languages();
    languages::InterfaceText catalogue;
    OA_CHECK(catalogue.add(kCatalogue));
    const auto& french = *languages::find_by_tag("fr");
    const auto& german = *languages::find_by_tag("de");
    languages::set_interface_language(&catalogue, french);
    OA_CHECK(languages::interface_text_in("Mouse wheel zoom", german) == "Mausrad-Zoom");
    OA_CHECK(languages::interface_text_in("Font shadow", german) == "Font shadow");
    OA_CHECK(&languages::interface_language() == &french);
    OA_CHECK(languages::interface_text("Mouse wheel zoom") == "Zoom \xC3\xA0 la molette");
    languages::set_interface_language(nullptr, languages::english());
    OA_CHECK(languages::interface_text_in("Mouse wheel zoom", german) == "Mouse wheel zoom");
    OA_CHECK(&languages::interface_language() == &languages::english());
}

/// The catalogue a test loader installs, and how many times it was asked.
struct CatalogueLoad {
    languages::InterfaceText catalogue;
    int calls = 0;

    /// Installs this catalogue and leaves the language shown as it was.
    ///
    /// @param context a CatalogueLoad
    static void load(void* context) {
        auto& held = *static_cast<CatalogueLoad*>(context);
        ++held.calls;
        languages::set_interface_language(&held.catalogue, languages::interface_language());
    }
};

/// English does not read a catalogue. A word asked for in another language
/// does, through a loader, the first time, and the language shown stays.
void a_translated_word_reads_the_catalogue_once() {
    clear_pack_languages();
    CatalogueLoad load;
    OA_CHECK(load.catalogue.add(kCatalogue));

    struct ClearLoader {
        ~ClearLoader() {
            languages::set_interface_catalogue_loader(nullptr, nullptr);
            languages::set_interface_language(nullptr, languages::english());
        }
    } clear_loader;

    languages::set_interface_language(nullptr, languages::english());
    languages::set_interface_catalogue_loader(&CatalogueLoad::load, &load);
    OA_CHECK(languages::interface_text("Mouse wheel zoom") == "Mouse wheel zoom");
    OA_CHECK(load.calls == 0);
    const auto& german = *languages::find_by_tag("de");
    OA_CHECK(languages::interface_text_in("Mouse wheel zoom", german) == "Mausrad-Zoom");
    OA_CHECK(load.calls == 1);
    OA_CHECK(&languages::interface_language() == &languages::english());
    OA_CHECK(languages::interface_text_in("Mouse wheel zoom", german) == "Mausrad-Zoom");
    OA_CHECK(load.calls == 1);
    // An empty catalogue is missing too, so the next translated word reads again.
    languages::InterfaceText empty;
    languages::set_interface_language(&empty, languages::english());
    OA_CHECK(languages::interface_text_in("Mouse wheel zoom", german) == "Mausrad-Zoom");
    OA_CHECK(load.calls == 2);
    OA_CHECK(&languages::interface_language() == &languages::english());
    static_cast<void>(clear_loader);
}

/// Copies text into a zero-filled character field, keeping its last byte zero.
///
/// @param field the field
/// @param text the text; the characters that fit are copied
template <std::size_t Size>
void copy_text(char (&field)[Size], std::string_view text) {
    std::memcpy(field, text.data(), std::min(text.size(), Size - 1));
}

/// Returns a unit type with its own name and description, as a unit file's
/// Name and Description give them.
///
/// @param unit_name the unit's name
/// @param name its name
/// @param description its description
/// @return the type
oa::UnitDef unit_type(const char* unit_name, const char* name, const char* description) {
    oa::UnitDef def{};
    copy_text(def.unit_name, unit_name);
    copy_text(def.name, name);
    copy_text(def.description, description);
    return def;
}

/// The words a language's text is looked up by: its chain's, English's
/// only for English itself, or the command line's word alone.
void data_words_follow_the_chain() {
    clear_pack_languages();
    OA_CHECK(
        languages::data_words(languages::english(), "") == std::vector<std::string>({"English"})
    );
    OA_CHECK(
        languages::data_words(*languages::find_by_tag("de"), "") ==
        std::vector<std::string>({"German"})
    );
    OA_CHECK(
        languages::data_words(languages::english(), "piglatin") ==
        std::vector<std::string>({"piglatin"})
    );
}

/// A unit's name and description in a language come from its file's keys,
/// found without regard to case, and fall back to its own.
void unit_texts_fall_back_to_the_types_own() {
    clear_pack_languages();
    languages::UnitTexts texts;
    texts.add("ARMCOM", "French", "Commandeur", "Commandant");
    texts.add("armcom", "german", nullptr, "Kommandant");
    texts.add("ARMCOM", "Spanish", "Comandante", nullptr);
    texts.add("", "French", "x", "y");
    texts.add("ARMSOLAR", "French", nullptr, nullptr);
    OA_CHECK(texts.size() == 1);
    const oa::UnitDef commander = unit_type("ARMCOM", "Commander", "Commander");
    const oa::UnitDef solar = unit_type("ARMSOLAR", "Solar Collector", "Produces Energy");
    const std::vector<std::string> french{"French"};
    const std::vector<std::string> german{"GERMAN"};
    const std::vector<std::string> spanish{"spanish"};
    OA_CHECK(texts.name(commander, french) == "Commandeur");
    OA_CHECK(texts.description(commander, french) == "Commandant");
    OA_CHECK(texts.name(commander, german) == "Commander");
    OA_CHECK(texts.description(commander, german) == "Kommandant");
    OA_CHECK(texts.name(commander, spanish) == "Comandante");
    OA_CHECK(texts.description(commander, spanish) == "Commander");
    OA_CHECK(texts.name(commander, {}) == "Commander");
    OA_CHECK(texts.name(solar, french) == "Solar Collector");
    OA_CHECK(texts.description(solar, french) == "Produces Energy");
    // A later word is tried after an earlier one that has no text.
    const std::vector<std::string> chain{"Italian", "French"};
    OA_CHECK(texts.name(commander, chain) == "Commandeur");
    // A text added again replaces the first.
    texts.add("ARMCOM", "FRENCH", "Chef", nullptr);
    OA_CHECK(texts.name(commander, french) == "Chef");
    OA_CHECK(texts.description(commander, french) == "Commandant");
    // A field filled to its end without a NUL is read to its end.
    oa::UnitDef full = commander;
    std::memset(full.name, 'N', sizeof full.name);
    OA_CHECK(texts.name(full, {}).size() == sizeof full.name);
    texts.clear();
    OA_CHECK(texts.size() == 0);
    OA_CHECK(texts.name(commander, french) == "Commander");
}

/// The installed table and words answer the interface's lookups; without
/// a table every type shows its own name and description.
void installed_unit_texts_answer_the_interface() {
    clear_pack_languages();
    const oa::UnitDef commander = unit_type("ARMCOM", "Commander", "Commander");
    OA_CHECK(languages::unit_display_name(commander) == "Commander");
    languages::UnitTexts texts;
    texts.add("ARMCOM", "Italian", "Comandante", "Comandante");
    const auto words = languages::data_words(*languages::find_by_tag("it"), "");
    languages::set_unit_texts(&texts, words);
    OA_CHECK(languages::unit_display_name(commander) == "Comandante");
    OA_CHECK(languages::unit_display_description(commander) == "Comandante");
    languages::set_unit_texts(&texts, languages::data_words(languages::english(), ""));
    OA_CHECK(languages::unit_display_name(commander) == "Commander");
    languages::set_unit_texts(nullptr, words);
    OA_CHECK(languages::unit_display_description(commander) == "Commander");
}

/// Installed languages sit among the built-ins by their names' bytes, and
/// an available language comes after every one of those.
void packs_add_languages_in_menu_order() {
    clear_pack_languages();
    // "Nederlands" and "Português" fall among the built-ins by their names.
    // "Japanese" and 简体中文 are available, and "Japanese" comes first.
    const languages::LanguageEntry dutch = make_entry("nl", "Nederlands", "Dutch", "Dutch");
    const languages::LanguageEntry portuguese = make_entry(
        "pt-BR",
        "Portugu\xC3\xAA"
        "s",
        "Portuguese",
        "Portuguese"
    );
    const languages::LanguageEntry japanese =
        make_entry("ja", "Japanese", "Japanese", "Japanese", languages::TextNeeds::more_font_faces);
    const std::array<languages::LanguageEntry, 2> installed{dutch, portuguese};
    const std::array<languages::LanguageEntry, 1> available{japanese};
    languages::set_pack_languages(installed, available);

    const auto known = languages::known_languages();
    const std::array<std::string_view, 9> tags{
        "en", "de", "es", "fr", "it", "nl", "pt-BR", "ja", "zh-Hans"
    };
    OA_CHECK(known.size() == tags.size());
    for (std::size_t index = 0; index < known.size() && index < tags.size(); ++index)
        OA_CHECK(known[index]->tag == tags[index]);
    OA_CHECK(known[5]->source == languages::Source::installed);
    OA_CHECK(known[6]->source == languages::Source::installed);
    OA_CHECK(languages::playable(*known[5]));
    OA_CHECK(languages::playable(*known[6]));
    OA_CHECK(known[7]->source == languages::Source::available);
    OA_CHECK(!languages::playable(*known[7]));
    OA_CHECK(!languages::drawable(*known[7]));
    OA_CHECK(known[8]->source == languages::Source::available);
    OA_CHECK(!languages::playable(*known[8]));
    OA_CHECK(languages::drawable(*known[8]));
}

/// Each rule for keeping a pack entry: what is dropped, what is filled,
/// which of two tags wins, and when an entry keeps its place.
void pack_entries_are_kept_by_rule() {
    clear_pack_languages();
    const languages::Language* german = languages::find_by_tag("de");
    OA_CHECK(german != nullptr);
    const auto generation = languages::registry_generation();

    languages::LanguageEntry underscore;
    underscore.tag = "pt_BR";
    underscore.word = "Portuguese";
    languages::LanguageEntry empty_tag;
    empty_tag.word = "Missing";
    languages::LanguageEntry c_locale;
    c_locale.tag = "C";
    c_locale.word = "C";
    languages::LanguageEntry no_word;
    no_word.tag = "sv";
    languages::LanguageEntry built_in = make_entry("de", "Germanic", "Germanic", "Deutsch");
    built_in.needs = languages::TextNeeds::modern_fonts;
    languages::LanguageEntry built_in_case =
        make_entry("DE", "Germanic", "Germanic", "Deutsch", languages::TextNeeds::modern_fonts);
    // An empty name in itself becomes the English name. An empty English
    // name becomes the tag. Locales are normalised, and ones that are not
    // locales are dropped. Fallbacks are kept as written.
    languages::LanguageEntry dutch = make_entry("nl", "", "Dutch", "Dutch");
    dutch.locales = {"nl_NL.UTF-8", "C", "not a locale"};
    dutch.fallbacks = {"pt_BR", "en"};
    languages::LanguageEntry dutch_again = make_entry("NL", "Holland", "Holland", "Holland");
    languages::LanguageEntry bare = make_entry("pt-BR", "", "", "Portuguese");
    bare.locales = {"pt_BR.UTF-8", "C", "not a locale", "pt"};

    const std::array<languages::LanguageEntry, 8> installed{
        underscore, empty_tag, c_locale, no_word, built_in, built_in_case, dutch, dutch_again
    };
    const std::array<languages::LanguageEntry, 1> available{bare};
    languages::set_pack_languages(installed, available);

    OA_CHECK(languages::find_by_tag("de") == german);
    OA_CHECK(german->endonym == "Deutsch");
    OA_CHECK(german->game_name == "German");
    OA_CHECK(german->needs == languages::TextNeeds::game_fonts);
    OA_CHECK(german->source == languages::Source::built_in);
    // "pt_BR" is not a tag the registry keeps. The lookup still finds
    // "pt-BR", because '_' is read as '-'.
    OA_CHECK(tag_of(languages::find_by_tag("pt_BR")) == "pt-BR");
    OA_CHECK(languages::find_by_tag("sv") == nullptr);
    OA_CHECK(languages::find_by_tag("C") == nullptr);

    const languages::Language* kept_dutch = languages::find_by_tag("nl");
    OA_CHECK(kept_dutch != nullptr);
    if (kept_dutch != nullptr) {
        OA_CHECK(kept_dutch->tag == "nl");
        OA_CHECK(kept_dutch->endonym == "Dutch");
        OA_CHECK(kept_dutch->english_name == "Dutch");
        OA_CHECK(kept_dutch->game_name == "Dutch");
        OA_CHECK(kept_dutch->source == languages::Source::installed);
        OA_CHECK(kept_dutch->locales.size() == 1);
        OA_CHECK(kept_dutch->locales.size() == 1 && kept_dutch->locales[0] == "nl-NL");
        OA_CHECK(kept_dutch->fallbacks.size() == 2);
        OA_CHECK(kept_dutch->fallbacks.size() == 2 && kept_dutch->fallbacks[0] == "pt_BR");
        OA_CHECK(kept_dutch->fallbacks.size() == 2 && kept_dutch->fallbacks[1] == "en");
    }
    const languages::Language* portuguese = languages::find_by_tag("pt-BR");
    OA_CHECK(portuguese != nullptr);
    if (portuguese != nullptr) {
        OA_CHECK(portuguese->endonym == "pt-BR");
        OA_CHECK(portuguese->english_name == "pt-BR");
        OA_CHECK(portuguese->source == languages::Source::available);
        OA_CHECK(portuguese->locales.size() == 2);
        OA_CHECK(portuguese->locales.size() == 2 && portuguese->locales[0] == "pt-BR");
        OA_CHECK(portuguese->locales.size() == 2 && portuguese->locales[1] == "pt");
    }
    OA_CHECK(languages::registry_generation() != generation);

    // The same entries again keep their places, and the generation stays.
    const auto settled = languages::registry_generation();
    languages::set_pack_languages(installed, available);
    OA_CHECK(languages::find_by_tag("nl") == kept_dutch);
    OA_CHECK(languages::find_by_tag("pt-BR") == portuguese);
    OA_CHECK(languages::registry_generation() == settled);

    // An installed entry wins over an available one with the same tag.
    const languages::LanguageEntry installed_dutch =
        make_entry("nl", "Nederlands", "Dutch", "Dutch");
    languages::LanguageEntry available_dutch =
        make_entry("nl", "Available", "Available", "Available");
    languages::set_pack_languages(
        std::array<languages::LanguageEntry, 1>{installed_dutch},
        std::array<languages::LanguageEntry, 1>{available_dutch}
    );
    const languages::Language* shown_dutch = languages::find_by_tag("nl");
    OA_CHECK(shown_dutch != nullptr && shown_dutch != kept_dutch);
    if (shown_dutch != nullptr) {
        OA_CHECK(shown_dutch->source == languages::Source::installed);
        OA_CHECK(shown_dutch->endonym == "Nederlands");
        OA_CHECK(shown_dutch->game_name == "Dutch");
    }
    OA_CHECK(kept_dutch->endonym == "Dutch");

    // The same endonym is ordered by tag.
    const languages::LanguageEntry flemish =
        make_entry("nl-BE", "Nederlands", "Flemish", "Flemish");
    languages::set_pack_languages(
        std::array<languages::LanguageEntry, 2>{installed_dutch, flemish}, {}
    );
    std::size_t dutch_at = 0;
    std::size_t flemish_at = 0;
    const auto known = languages::known_languages();
    for (std::size_t index = 0; index < known.size(); ++index) {
        if (known[index]->tag == "nl")
            dutch_at = index;
        if (known[index]->tag == "nl-BE")
            flemish_at = index;
    }
    OA_CHECK(dutch_at != 0 && flemish_at != 0 && dutch_at < flemish_at);

    // A compiled offered language suppresses an available entry for its tag.
    // Installing the same tag removes the available one.
    const languages::LanguageEntry waiting_chinese =
        make_entry("zh-Hans", "Waiting", "Waiting", "Chinese", languages::TextNeeds::modern_fonts);
    languages::set_pack_languages({}, std::array<languages::LanguageEntry, 1>{waiting_chinese});
    const languages::Language* offered_chinese = languages::find_by_tag("zh-Hans");
    OA_CHECK(offered_chinese != nullptr);
    if (offered_chinese != nullptr) {
        OA_CHECK(offered_chinese->source == languages::Source::available);
        OA_CHECK(offered_chinese->english_name == "Chinese (Simplified)");
    }
    const languages::LanguageEntry waiting = make_entry("ja", "Japanese", "Japanese", "Japanese");
    languages::set_pack_languages({}, std::array<languages::LanguageEntry, 1>{waiting});
    const languages::Language* available_japanese = languages::find_by_tag("ja");
    OA_CHECK(available_japanese != nullptr);
    if (available_japanese != nullptr)
        OA_CHECK(available_japanese->source == languages::Source::available);
    const languages::LanguageEntry installed_japanese =
        make_entry("ja", "\xE6\x97\xA5\xE6\x9C\xAC\xE8\xAA\x9E", "Japanese", "Japanese");
    languages::set_pack_languages(
        std::array<languages::LanguageEntry, 1>{installed_japanese},
        std::array<languages::LanguageEntry, 1>{waiting}
    );
    const languages::Language* shown_japanese = languages::find_by_tag("ja");
    OA_CHECK(shown_japanese != nullptr && shown_japanese != available_japanese);
    if (shown_japanese != nullptr)
        OA_CHECK(shown_japanese->source == languages::Source::installed);
    for (const languages::Language* language : languages::known_languages())
        OA_CHECK(!(language->tag == "ja" && language->source == languages::Source::available));
    OA_CHECK(available_japanese != nullptr && available_japanese->endonym == "Japanese");

    clear_pack_languages();
    OA_CHECK(languages::find_by_tag("nl") == nullptr);
    OA_CHECK(languages::find_by_tag("ja") == nullptr);
    OA_CHECK(languages::known_languages().size() == 6);
}

/// An available language can be listed and matched, and it is never the
/// language the game shows.
void available_languages_are_never_shown() {
    clear_pack_languages();
    languages::LanguageEntry portuguese = make_entry(
        "pt-BR",
        "Portugu\xC3\xAA"
        "s",
        "Portuguese",
        "Portuguese",
        languages::TextNeeds::modern_fonts
    );
    portuguese.locales = {"pt-BR"};
    languages::set_pack_languages({}, std::array<languages::LanguageEntry, 1>{portuguese});
    const languages::Language* available = languages::find_by_tag("pt-BR");
    OA_CHECK(available != nullptr);
    if (available == nullptr)
        return;
    OA_CHECK(available->source == languages::Source::available);
    OA_CHECK(available->needs == languages::TextNeeds::modern_fonts);
    OA_CHECK(languages::drawable(*available));
    OA_CHECK(!languages::playable(*available));
    OA_CHECK(&languages::chosen_language("pt-BR", languages::english()) == &languages::english());
    OA_CHECK(languages::find_by_game_name("Portuguese") == nullptr);
    OA_CHECK(languages::find_by_game_name("portuguese") == nullptr);
    OA_CHECK(languages::match_locale("pt-BR") == nullptr);
    OA_CHECK(languages::match_locale("pt-BR", false) == nullptr);
    OA_CHECK(languages::match_locale("pt-BR", true) == available);
    const std::vector<std::string> locales{"pt_BR.UTF-8", "en"};
    OA_CHECK(languages::wanted_language(locales) == available);
    OA_CHECK(languages::preferred_language(locales).tag == "en");
    OA_CHECK(&languages::preferred_language(locales) == &languages::english());
}

/// A language a later call replaces stays readable, and one that did not
/// change keeps its place. The generation changes when the list does.
void entries_outlive_their_replacement() {
    clear_pack_languages();
    const languages::LanguageEntry dutch = make_entry("nl", "Nederlands", "Dutch", "Dutch");
    const languages::LanguageEntry portuguese = make_entry(
        "pt-BR",
        "Portugu\xC3\xAA"
        "s",
        "Portuguese",
        "Portuguese"
    );
    const std::array<languages::LanguageEntry, 2> installed{dutch, portuguese};
    languages::set_pack_languages(installed, {});
    const languages::Language* first_dutch = languages::find_by_tag("nl");
    const languages::Language* first_portuguese = languages::find_by_tag("pt-BR");
    const auto generation = languages::registry_generation();
    OA_CHECK(first_dutch != nullptr && first_portuguese != nullptr);
    if (first_dutch == nullptr || first_portuguese == nullptr)
        return;

    languages::LanguageEntry renamed = dutch;
    renamed.endonym = "Holland";
    const std::array<languages::LanguageEntry, 2> replaced{renamed, portuguese};
    languages::set_pack_languages(replaced, {});
    OA_CHECK(first_dutch->tag == "nl");
    OA_CHECK(first_dutch->endonym == "Nederlands");
    OA_CHECK(languages::find_by_tag("nl") != first_dutch);
    OA_CHECK(languages::find_by_tag("nl")->endonym == "Holland");
    OA_CHECK(languages::find_by_tag("pt-BR") == first_portuguese);
    OA_CHECK(
        first_portuguese->endonym == "Portugu\xC3\xAA"
                                     "s"
    );
    OA_CHECK(languages::registry_generation() != generation);
    for (const languages::Language* language : languages::known_languages())
        OA_CHECK(language != first_dutch);

    const languages::Language* second_dutch = languages::find_by_tag("nl");
    const auto generation_after = languages::registry_generation();
    languages::set_pack_languages(replaced, {});
    OA_CHECK(languages::find_by_tag("nl") == second_dutch);
    OA_CHECK(languages::find_by_tag("pt-BR") == first_portuguese);
    OA_CHECK(languages::registry_generation() == generation_after);
    OA_CHECK(first_dutch->tag == "nl");
    OA_CHECK(first_dutch->endonym == "Nederlands");
}

} // namespace

int main() {
    registry_lists_the_built_in_languages_in_menu_order();
    packs_add_languages_in_menu_order();
    pack_entries_are_kept_by_rule();
    available_languages_are_never_shown();
    entries_outlive_their_replacement();
    only_the_needs_this_build_meets_are_drawable();
    languages_in_utf8_turn_unicode_chat_on();
    tags_and_game_words_are_found_without_regard_to_case();
    fallback_chains_end_in_english();
    locales_are_normalised();
    locales_choose_languages_by_their_leading_subtags();
    preferred_locales_choose_the_first_known_language();
    settings_choice_names_a_language_or_the_system_one();
    catalogue_translates_by_english_text();
    malformed_catalogues_add_nothing();
    catalogue_files_merge_by_text_and_language();
    catalogue_follows_tags_and_fallback_chains();
    refused_catalogues_leave_the_catalogue_as_it_was();
    catalogue_keeps_texts_in_place_through_later_adds();
    installed_language_answers_interface_text();
    every_installation_moves_the_interface_generation();
    named_language_reads_one_text();
    a_translated_word_reads_the_catalogue_once();
    data_words_follow_the_chain();
    unit_texts_fall_back_to_the_types_own();
    installed_unit_texts_answer_the_interface();
    return oa::test::check_exit_status();
}
