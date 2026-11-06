// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The catalogue the engine ships (languages/interface.tdf) covers every
// English text this build passes to the interface catalogue, in German,
// French, Spanish and Italian. A text is read back through the catalogue
// the game uses, and a German lookup returns German. Each translation keeps
// the English text's placeholders and numbers, and the product names stay
// as written. A text that names a mutator button names it as that language
// shows the button.
//
// The command line names the catalogue file.

#include "interface_words.hpp"

#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace languages = oa::data::languages;

/// How many failures are printed in full. The rest are counted.
constexpr int most_shown = 30;

/// Failures printed so far.
int shown = 0;

/// Prints a coverage failure, up to most_shown of them.
///
/// @param detail what failed
void show(std::string_view detail) {
    if (shown < most_shown)
        std::cerr << "interface catalogue: " << detail << '\n';
    ++shown;
    OA_CHECK(false);
}

/// Reads a whole file.
///
/// @param path the file
/// @return its bytes; empty when it does not read
std::string read_file(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/// Tells whether a byte is an ASCII letter.
bool letter(char byte) noexcept {
    return (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
}

/// Tells whether a byte can continue a placeholder name.
bool name_byte(char byte) noexcept {
    return letter(byte) || byte == '_' || (byte >= '0' && byte <= '9');
}

/// Appends the `{name}` placeholders of a text, in order.
void collect_places(std::string_view text, std::vector<std::string>& places) {
    for (std::size_t index = 0; index < text.size(); ++index) {
        if (text[index] != '{')
            continue;
        const std::size_t start = index + 1;
        if (start >= text.size() || !(letter(text[start]) || text[start] == '_'))
            continue;
        std::size_t end = start + 1;
        while (end < text.size() && name_byte(text[end]))
            ++end;
        if (end < text.size() && text[end] == '}')
            places.emplace_back(text.substr(start - 1, end - start + 2));
    }
}

/// Appends the digit runs of a text that sit outside its placeholders.
void collect_numbers(std::string_view text, std::vector<std::string>& numbers) {
    std::vector<std::string> places;
    collect_places(text, places);
    std::string bare(text);
    for (const std::string& place : places) {
        const auto at = bare.find(place);
        if (at != std::string::npos)
            bare.replace(at, place.size(), " ");
    }
    for (std::size_t index = 0; index < bare.size(); ++index) {
        if (bare[index] < '0' || bare[index] > '9')
            continue;
        const std::size_t start = index;
        while (index < bare.size() && bare[index] >= '0' && bare[index] <= '9')
            ++index;
        numbers.emplace_back(bare.substr(start, index - start));
        --index;
    }
}

/// Counts a needle in a text, without overlapping.
std::size_t count_of(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t at = 0; (at = text.find(needle, at)) != std::string_view::npos;) {
        ++count;
        at += needle.size();
    }
    return count;
}

/// Counts `OA` standing as its own word.
std::size_t count_oa(std::string_view text) {
    std::size_t count = 0;
    for (std::size_t index = 0; index + 1 < text.size(); ++index) {
        if (text[index] != 'O' || text[index + 1] != 'A')
            continue;
        const bool before = index > 0 && letter(text[index - 1]);
        const bool after = index + 2 < text.size() && letter(text[index + 2]);
        if (!before && !after)
            ++count;
    }
    return count;
}

/// A bag of texts, so a repeated placeholder has to be kept as often.
using Bag = std::map<std::string, int, std::less<>>;

Bag bag_of(const std::vector<std::string>& items) {
    Bag bag;
    for (const std::string& item : items)
        ++bag[item];
    return bag;
}

/// The built-in languages the catalogue translates into, and one word each
/// that is not the English.
constexpr std::pair<std::string_view, std::string_view> kGermanWords[] = {
    {"Library", "Bibliothek"},
    {"Cancel", "Abbrechen"},
    {"Settings", "Einstellungen"},
    {"Enhanced Mode", "Erweiterter Modus"},
    {"Use the enhanced rules preset. Applies from the next game.",
     "Das erweiterte Regelprofil verwenden. Gilt ab dem nächsten Spiel."},
};

/// The mutator buttons other texts name. A text that names one shows it
/// as the button shows in the same language.
constexpr std::string_view kButtonNames[] = {
    "GET MUTATORS",
};

/// Tells whether a text holds a button's name as its own words: no ASCII
/// letter just before or just after it.
///
/// @param text the text
/// @param button the button's name
/// @return true when the text names the button
bool names_button(std::string_view text, std::string_view button) {
    for (std::size_t at = text.find(button); at != std::string_view::npos;
         at = text.find(button, at + 1)) {
        const std::size_t end = at + button.size();
        const bool before = at > 0 && letter(text[at - 1]);
        const bool after = end < text.size() && letter(text[end]);
        if (!before && !after)
            return true;
    }
    return false;
}

/// Reads the shipped catalogue and checks the four languages cover it.
void shipped_catalogue_covers_every_word(const std::filesystem::path& catalogue_path) {
    const std::string file = read_file(catalogue_path);
    if (file.empty()) {
        show("the catalogue file did not read");
        return;
    }
    languages::InterfaceText catalogue;
    std::string failure;
    if (!catalogue.add(file, &failure)) {
        show(failure);
        return;
    }
    const std::vector<std::string_view> words = oa::tool::collected_interface_words();
    const languages::Language* german = languages::find_by_tag("de");
    const languages::Language* french = languages::find_by_tag("fr");
    const languages::Language* spanish = languages::find_by_tag("es");
    const languages::Language* italian = languages::find_by_tag("it");
    if (german == nullptr || french == nullptr || spanish == nullptr || italian == nullptr) {
        show("a built-in language is missing");
        return;
    }
    const languages::Language* const tags[] = {german, french, spanish, italian};
    OA_CHECK(catalogue.size() == words.size() * 4);
    for (const std::string_view word : words) {
        std::vector<std::string> places;
        std::vector<std::string> numbers;
        collect_places(word, places);
        collect_numbers(word, numbers);
        const Bag wanted_places = bag_of(places);
        const Bag wanted_numbers = bag_of(numbers);
        for (const languages::Language* language : tags) {
            const std::string_view translated = catalogue.text(word, *language);
            // A missing text is handed back as the English view itself.
            if (translated.data() == word.data()) {
                show(std::string(language->tag) + " has no translation of " + std::string(word));
                continue;
            }
            std::vector<std::string> kept_places;
            std::vector<std::string> kept_numbers;
            collect_places(translated, kept_places);
            collect_numbers(translated, kept_numbers);
            if (bag_of(kept_places) != wanted_places)
                show(std::string(language->tag) + " drops a placeholder in " + std::string(word));
            const Bag kept_number_bag = bag_of(kept_numbers);
            for (const auto& [number, count] : wanted_numbers) {
                const auto found = kept_number_bag.find(number);
                if (found == kept_number_bag.end() || found->second < count)
                    show(
                        std::string(language->tag) + " drops the number " + number + " in " +
                        std::string(word)
                    );
            }
            for (const std::string_view name :
                 {"Open Annihilation",
                  "Core Prime",
                  "Total Annihilation",
                  "Cavedog Entertainment",
                  "Steam Input"}) {
                if (count_of(translated, name) < count_of(word, name))
                    show(
                        std::string(language->tag) + " translates " + std::string(name) + " in " +
                        std::string(word)
                    );
            }
            if (count_oa(translated) < count_oa(word))
                show(std::string(language->tag) + " translates OA in " + std::string(word));
            for (const std::string_view button : kButtonNames) {
                if (word == button || !names_button(word, button))
                    continue;
                const std::string_view shown_button = catalogue.text(button, *language);
                if (translated.find(shown_button) == std::string_view::npos)
                    show(
                        std::string(language->tag) + " does not name " + std::string(button) +
                        " as " + std::string(shown_button) + " in " + std::string(word)
                    );
            }
        }
    }
    // The lookup the game uses, and the language it is showing stays put.
    languages::set_interface_language(&catalogue, languages::english());
    for (const auto& [english, german_text] : kGermanWords) {
        OA_CHECK(languages::interface_text_in(english, *german) == german_text);
        OA_CHECK(languages::interface_text(english) == english);
    }
    languages::set_interface_language(&catalogue, *german);
    OA_CHECK(languages::interface_text("Library") == "Bibliothek");
    OA_CHECK(&languages::interface_language() == german);
    languages::set_interface_language(nullptr, languages::english());
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::cerr << "interface catalogue: name the catalogue file\n";
        return 2;
    }
    shipped_catalogue_covers_every_word(argv[1]);
    if (shown > most_shown)
        std::cerr << "interface catalogue: " << (shown - most_shown) << " more\n";
    return oa::test::check_exit_status();
}
