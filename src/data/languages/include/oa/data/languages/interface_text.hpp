// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The interface catalogue: the Open Annihilation interface's own words (the
// settings dialog, the engine's own notices and messages) in the languages
// translations are given for. The game's own texts are 3.1c's and come from
// its data (Translate.tdf and the unit files, oa/data/defs/locale.hpp);
// these are the words the engine adds. Each is written in English in the
// source and looked up by that text, and a catalogue file has the shape of
// 3.1c's Translate.tdf: a section names the English text, and each of its
// keys, a language's tag, holds the text in that language, in UTF-8:
//
//     [Mouse wheel zoom]
//         {
//         fr = ...;
//         de = ...;
//         }
//
// The engine ships languages/interface.tdf beside the game's fonts, with
// these words in German, French, Spanish and Italian. A pack's
// interface.tdf has the same shape and is added to this same catalogue.
#pragma once

#include "oa/data/languages.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::languages {

/// The most bytes of a catalogue file read: a larger file is refused.
inline constexpr std::size_t most_catalogue_bytes = std::size_t{4} * 1024 * 1024;

/// The interface's own words in other languages, by their English text.
class InterfaceText {
  public:

    /// Adds the translations a catalogue file holds. Each top-level section
    /// names an English text, and each of its keys a language's tag, read
    /// without regard to case, with the text in that language; an empty
    /// value adds nothing. A later translation of the same text into the
    /// same language replaces the earlier one.
    ///
    /// @param file the file's bytes, at most most_catalogue_bytes
    /// @param[out] error why the file was refused; may be null
    /// @return false when the file is too large or does not parse; nothing
    ///     is added then
    bool add(std::string_view file, std::string* error = nullptr);

    /// Returns a text as a language shows it: its translation into the
    /// first language of the language's fallback chain that has one, else
    /// the English text itself.
    ///
    /// @param english the English text
    /// @param language the language
    /// @return the text; a view of the catalogue's copy, valid while the
    ///     catalogue lives and nothing is added, or of `english`
    [[nodiscard]] std::string_view text(std::string_view english, const Language& language) const;

    /// Returns how many translations the catalogue holds.
    ///
    /// @return the texts times the languages each is translated into
    [[nodiscard]] std::size_t size() const noexcept;

  private:

    /// One English text the catalogue holds translations of.
    struct EnglishText {
        const char* text{}; ///< the text, in one of blocks_ and followed by a NUL
        uint32_t size{};    ///< its bytes, without the NUL
        uint32_t first{};   ///< the place in translations_ of its first translation
    };

    /// One English text's translation into one language.
    struct Translation {
        const char* text{};  ///< the text, in one of blocks_ and followed by a NUL
        uint32_t language{}; ///< the language's tag's place in tags_
    };

    /// The characters of the texts, one block for each file that added any,
    /// each of the exact size its texts take. A block is never moved or
    /// changed while the catalogue lives; a replaced translation's text stays
    /// in its block unread.
    std::vector<std::unique_ptr<char[]>> blocks_{};
    /// The tags the translations are in, lower-case with '-' for '_', sorted
    /// by their bytes, each once.
    std::vector<std::string> tags_{};
    /// The English texts, sorted by their bytes, each once.
    std::vector<EnglishText> english_{};
    /// The translations: each English text's in the order of english_, and
    /// each one's sorted by language, one for each.
    std::vector<Translation> translations_{};
};

/// Installs the catalogue and the language the interface's own words show
/// in. The view interface_text gives stays valid until the next call. Only
/// the thread that draws the interface calls it and interface_text.
///
/// @param catalogue the catalogue; null shows every word in English
/// @param language the language
void set_interface_language(const InterfaceText* catalogue, const Language& language) noexcept;

/// Returns the language the interface's own words show in.
///
/// @return the installed language; English before any is installed
[[nodiscard]] const Language& interface_language() noexcept;

/// Returns a number that changes with every set_interface_language, the
/// same catalogue and language installed again among them: a pack read
/// again replaces that catalogue's texts in place, and its language may
/// stay the same entry, so what was drawn in the interface's words tells
/// by this number that they may have changed.
///
/// @return the generation; 0 before any installation
[[nodiscard]] uint64_t interface_generation() noexcept;

/// Returns one of the interface's own words in a named language, from the
/// catalogue set_interface_language installed. The language the interface
/// shows is left as it was. Nothing is loaded to answer it.
///
/// @param english the word, in English as the source writes it
/// @param language the language to read it in
/// @return its translation, or `english` itself without one; a view of the
///     catalogue's copy, valid while that catalogue is installed and
///     nothing is added, or of `english`
[[nodiscard]] std::string_view
interface_text_in(std::string_view english, const Language& language);

/// Returns one of the interface's own words in the installed language.
///
/// @param english the word, in English as the source writes it
/// @return its translation, or `english` itself without one
[[nodiscard]] std::string_view interface_text(std::string_view english);

} // namespace oa::data::languages
