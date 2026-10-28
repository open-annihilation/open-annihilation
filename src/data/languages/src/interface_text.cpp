// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/languages/interface_text.hpp"

#include "oa/formats/tdf.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::languages {

namespace {

/// Lowers the ASCII letters of a tag and reads '_' as '-'.
///
/// @param tag the tag
/// @return the folded tag
std::string folded_tag(std::string_view tag) {
    std::string folded(tag);
    for (char& letter : folded) {
        if (letter >= 'A' && letter <= 'Z')
            letter = static_cast<char>(letter - 'A' + 'a');
        else if (letter == '_')
            letter = '-';
    }
    return folded;
}

/// One translation a catalogue file gives, as it is read.
struct ReadTranslation {
    std::string_view english{}; ///< the section's name, in the parsed document
    std::string tag{};          ///< the key, folded as folded_tag folds it
    std::string_view text{};    ///< the value, in the parsed document
};

/// One translation of the catalogue an add builds: one it holds, or one the
/// file gives.
struct MergedTranslation {
    std::string_view english{}; ///< the English text
    std::string_view text{};    ///< the text in the language
    const char* english_copy{}; ///< the catalogue's copy of the English text; null without one
    const char* text_copy{};    ///< the catalogue's copy of the text; null for the file's
    uint32_t language{};        ///< the language's tag's place in the tags after the add
};

/// Tells whether one translation of an add comes before another in the
/// catalogue: by the English text's bytes, then by the language's place.
///
/// @param first the first
/// @param second the second
/// @return true when `first` comes first
bool comes_before(const MergedTranslation& first, const MergedTranslation& second) noexcept {
    const int order = first.english.compare(second.english);
    return order < 0 || (order == 0 && first.language < second.language);
}

/// Copies a text, and a NUL after it, into a block.
///
/// @param[in,out] at where the copy goes; moved past its NUL
/// @param text the text
/// @return the copy
const char* copy_into(char*& at, std::string_view text) noexcept {
    char* copy = at;
    std::memcpy(copy, text.data(), text.size());
    copy[text.size()] = '\0';
    at += text.size() + 1;
    return copy;
}

/// The catalogue and language the interface's own words show in.
struct Installed {
    const InterfaceText* catalogue{};  ///< null for English alone
    const Language* language{};        ///< null before any is installed
    uint64_t generation{};             ///< counts the installations
    InterfaceCatalogueLoader loader{}; ///< null until one is registered
    void* loader_context{};            ///< passed to loader
};

/// Tells whether a word is wanted in a language other than English.
///
/// @param language the language
/// @return false for English, which is the text the source writes
bool asks_for_translation(const Language& language) noexcept {
    return language.tag != english().tag;
}

/// The one installation the interface reads.
Installed& installed() noexcept {
    static Installed state{};
    return state;
}

} // namespace

bool InterfaceText::add(std::string_view file, std::string* error) {
    if (file.size() > most_catalogue_bytes) {
        if (error != nullptr)
            *error = "the catalogue file is larger than " + std::to_string(most_catalogue_bytes) +
                     " bytes";
        return false;
    }
    oa::formats::tdf::OwnedDocument document;
    oa::formats::tdf::ParseError parse_error{};
    if (!document.parse(file, &parse_error)) {
        if (error != nullptr)
            *error = oa::formats::tdf::describe(parse_error);
        return false;
    }
    std::vector<ReadTranslation> read;
    const oa::formats::tdf::Block* root = document.root();
    for (uint32_t index = 0; index < oa::formats::tdf::child_count(root); ++index) {
        const oa::formats::tdf::Block* section = oa::formats::tdf::child_at(root, index);
        if (section == nullptr || section->name == nullptr || section->name[0] == '\0')
            continue;
        const uint32_t keys = oa::formats::tdf::property_count(section);
        for (uint32_t key_index = 0; key_index < keys; ++key_index) {
            const char* key =
                oa::formats::tdf::property_key_at(section, static_cast<int32_t>(key_index));
            const char* value = oa::formats::tdf::find_value(section, key);
            if (key == nullptr || value == nullptr || value[0] == '\0')
                continue;
            read.push_back({section->name, folded_tag(key), value});
        }
    }
    if (read.empty())
        return true;
    // A later translation of a text into a language replaces an earlier one.
    // Reversed, the later one comes first among its equals, which the stable
    // sort keeps first and unique keeps alone.
    std::reverse(read.begin(), read.end());
    std::stable_sort(
        read.begin(), read.end(), [](const ReadTranslation& first, const ReadTranslation& second) {
            const int order = first.english.compare(second.english);
            return order < 0 || (order == 0 && first.tag < second.tag);
        }
    );
    read.erase(
        std::unique(
            read.begin(),
            read.end(),
            [](const ReadTranslation& first, const ReadTranslation& second) {
                return first.english == second.english && first.tag == second.tag;
            }
        ),
        read.end()
    );

    // The tags after the add: the catalogue's and the file's, sorted, each once.
    std::vector<std::string_view> names(tags_.begin(), tags_.end());
    names.reserve(tags_.size() + read.size());
    for (const ReadTranslation& translation : read)
        names.push_back(translation.tag);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    std::vector<std::string> tags(names.begin(), names.end());
    const auto place_of = [&tags](std::string_view tag) {
        return static_cast<uint32_t>(
            std::lower_bound(tags.begin(), tags.end(), tag) - tags.begin()
        );
    };

    // The file's translations, then the catalogue's, each in order. Merged,
    // the file's translation of a text into a language comes before the
    // catalogue's, and unique keeps it alone.
    std::vector<MergedTranslation> merged;
    merged.reserve(read.size() + translations_.size());
    for (const ReadTranslation& translation : read)
        merged.push_back(
            {translation.english, translation.text, nullptr, nullptr, place_of(translation.tag)}
        );
    for (std::size_t index = 0; index < english_.size(); ++index) {
        const EnglishText& english = english_[index];
        const std::size_t end =
            index + 1 < english_.size() ? english_[index + 1].first : translations_.size();
        for (std::size_t held = english.first; held < end; ++held) {
            const Translation& translation = translations_[held];
            merged.push_back(
                {std::string_view(english.text, english.size),
                 translation.text,
                 english.text,
                 translation.text,
                 place_of(tags_[translation.language])}
            );
        }
    }
    const auto given = merged.begin() + static_cast<std::ptrdiff_t>(read.size());
    std::inplace_merge(merged.begin(), given, merged.end(), comes_before);
    merged.erase(
        std::unique(
            merged.begin(),
            merged.end(),
            [](const MergedTranslation& first, const MergedTranslation& second) {
                return first.english == second.english && first.language == second.language;
            }
        ),
        merged.end()
    );

    // Each English text is kept once: one the catalogue holds keeps its copy,
    // and the file's others are copied once. The block holds those and the
    // texts the file gives, each followed by a NUL.
    std::size_t block_bytes = 0;
    std::size_t english_count = 0;
    for (std::size_t first = 0, end = 0; first < merged.size(); first = end) {
        const char* english_copy = nullptr;
        for (end = first; end < merged.size() && merged[end].english == merged[first].english;
             ++end)
            if (merged[end].english_copy != nullptr)
                english_copy = merged[end].english_copy;
        ++english_count;
        if (english_copy == nullptr)
            block_bytes += merged[first].english.size() + 1;
        for (std::size_t index = first; index < end; ++index) {
            merged[index].english_copy = english_copy;
            if (merged[index].text_copy == nullptr)
                block_bytes += merged[index].text.size() + 1;
        }
    }
    std::unique_ptr<char[]> block = std::make_unique<char[]>(block_bytes);
    char* at = block.get();
    std::vector<EnglishText> english;
    english.reserve(english_count);
    std::vector<Translation> translations;
    translations.reserve(merged.size());
    for (std::size_t index = 0; index < merged.size(); ++index) {
        const MergedTranslation& translation = merged[index];
        if (index == 0 || merged[index - 1].english != translation.english) {
            const char* copy = translation.english_copy != nullptr
                                   ? translation.english_copy
                                   : copy_into(at, translation.english);
            english.push_back(
                {copy,
                 static_cast<uint32_t>(translation.english.size()),
                 static_cast<uint32_t>(translations.size())}
            );
        }
        const char* text = translation.text_copy != nullptr ? translation.text_copy
                                                            : copy_into(at, translation.text);
        translations.push_back({text, translation.language});
    }
    // Nothing changes before the block is kept, so a failed allocation adds nothing.
    blocks_.push_back(std::move(block));
    tags_ = std::move(tags);
    english_ = std::move(english);
    translations_ = std::move(translations);
    return true;
}

std::string_view InterfaceText::text(std::string_view english, const Language& language) const {
    const auto found = std::lower_bound(
        english_.begin(),
        english_.end(),
        english,
        [](const EnglishText& held, std::string_view wanted) {
            return std::string_view(held.text, held.size) < wanted;
        }
    );
    if (found == english_.end() || std::string_view(found->text, found->size) != english)
        return english;
    // The text's translations, sorted by language.
    const auto first = translations_.begin() + found->first;
    const auto last = found + 1 != english_.end() ? translations_.begin() + (found + 1)->first
                                                  : translations_.end();
    const FallbackChain chain = fallback_chain(language);
    for (const Language* step : chain.view()) {
        const std::string tag = folded_tag(step->tag);
        const auto known = std::lower_bound(tags_.begin(), tags_.end(), tag);
        if (known == tags_.end() || *known != tag)
            continue;
        const auto place = static_cast<uint32_t>(known - tags_.begin());
        const auto translation =
            std::lower_bound(first, last, place, [](const Translation& held, uint32_t wanted) {
                return held.language < wanted;
            });
        if (translation != last && translation->language == place)
            return translation->text;
    }
    return english;
}

std::size_t InterfaceText::size() const noexcept {
    return translations_.size();
}

void set_interface_language(const InterfaceText* catalogue, const Language& language) noexcept {
    Installed& state = installed();
    state.catalogue = catalogue;
    state.language = &language;
    ++state.generation;
}

void set_interface_catalogue_loader(InterfaceCatalogueLoader loader, void* context) noexcept {
    Installed& state = installed();
    state.loader = loader;
    state.loader_context = context;
}

const Language& interface_language() noexcept {
    const Language* language = installed().language;
    return language != nullptr ? *language : english();
}

uint64_t interface_generation() noexcept {
    return installed().generation;
}

std::string_view interface_text_in(std::string_view english, const Language& language) {
    if (asks_for_translation(language)) {
        Installed& held = installed();
        const bool unread = held.catalogue == nullptr || held.catalogue->size() == 0;
        if (unread && held.loader != nullptr) {
            const InterfaceCatalogueLoader load = held.loader;
            void* const context = held.loader_context;
            load(context);
        }
    }
    const InterfaceText* installed_catalogue = installed().catalogue;
    if (installed_catalogue == nullptr)
        return english;
    return installed_catalogue->text(english, language);
}

std::string_view interface_text(std::string_view english) {
    return interface_text_in(english, interface_language());
}

} // namespace oa::data::languages
