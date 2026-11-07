// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The languages the game shows its text in: for each, its BCP-47 tag, its
// name in itself, the word 3.1c's game data knows it by, the languages its
// text falls back to and what drawing its text needs. The operating
// system's preferred locales and the player's choice in the settings are
// matched against them here; the text of each language comes from the game
// data through 3.1c's own keys (oa/data/defs/locale.hpp) and, for the Open
// Annihilation interface's own words, from the interface catalogue
// (oa/data/languages/interface_text.hpp). Nothing here reaches the
// simulation, a saved game or what a shared game sends.
//
// Adding a language is usually a language pack: its manifest becomes an
// entry when the pack is installed (set_pack_languages). A language 3.1c's
// data holds is an entry of src/registry.inc, so the game knows it without
// a pack. A language whose letters the game cannot draw yet waits,
// unoffered, until the drawing it needs (TextNeeds) is built.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::data::languages {

/// What drawing a language's text needs, beyond the game's own fonts.
enum class TextNeeds : uint8_t {
    /// Nothing more: every letter is in the game's 8-bit code page
    /// (Windows-1252), which the game's own GUI and FNT fonts draw. English,
    /// French, Italian, German and Spanish.
    game_fonts,
    /// The modern fonts (oa/platform/text_font.hpp), which hold the
    /// letters outside the code page, and game data that holds its text as
    /// UTF-8 (a mod profile's ui.text-rendering, unicode). Simplified
    /// Chinese is one: a pack the engine knows of, whose font comes with
    /// the pack.
    modern_fonts,
    /// A modern font face the game does not bundle: Traditional Chinese and
    /// Japanese draw their characters in their own regional forms, which a
    /// Simplified Chinese face draws differently, and Korean needs a face
    /// that holds its Hangul syllables.
    more_font_faces,
    /// Complex text shaping: letters that join, stack and change order, as
    /// Devanagari's do in Hindi, which the FreeType-only drawing cannot lay
    /// out. It needs a shaping library (HarfBuzz) and a face for the
    /// script.
    text_shaping,
};

/// Where a language's entry comes from.
enum class Source : uint8_t {
    /// Compiled into the game: English and the languages 3.1c's data holds.
    built_in,
    /// An installed language pack, in the player's Languages folder or the
    /// languages folder beside the game.
    installed,
    /// A language a pack exists for that is not installed. It can be listed
    /// and matched, and it is never the language the game shows.
    available,
};

/// One language the game knows.
struct Language {
    /// Its BCP-47 tag, as the settings keep it: "en", "fr", "zh-Hans".
    std::string_view tag{};
    /// Its name in itself, in UTF-8, as the settings list it: "Français".
    std::string_view endonym{};
    /// Its name in English, for logs and documents: "French".
    std::string_view english_name{};
    /// The word 3.1c's game data knows the language by: the key of its
    /// entries in Translate.tdf, the prefix of a unit file's Name and
    /// Description keys (GermanName) and the suffix of its language
    /// folders (bitmaps-German). English has one too, "English", which 3.1c
    /// runs in by default; its data holds no English entries, so English
    /// shows the data's own text, but a mod's may.
    std::string_view game_name{};
    /// The operating system's locales that choose it, each matched as a
    /// whole tag or as the leading subtags of one ("de" matches "de-AT").
    std::span<const std::string_view> locales{};
    /// The tags its text falls back to, in order, where the data has no
    /// text in it; English, the data's own text, comes last whether listed
    /// or not.
    std::span<const std::string_view> fallbacks{};
    /// What drawing its text needs.
    TextNeeds needs{TextNeeds::game_fonts};
    /// Where the entry comes from. It is last so a compiled entry's
    /// initialiser lists the fields it always had.
    Source source{Source::built_in};
};

/// The fields a pack, or a catalogue, gives one registry entry.
struct LanguageEntry {
    std::string tag{};     ///< its BCP-47 tag, as normalised_locale leaves it
    std::string endonym{}; ///< its name in itself; empty becomes the English name, else the tag
    std::string english_name{};             ///< its name in English; empty becomes the tag
    std::string word{};                     ///< the word the game data knows it by
    std::vector<std::string> locales{};     ///< the operating system's locales that choose it
    std::vector<std::string> fallbacks{};   ///< the tags its text falls back to before English
    TextNeeds needs{TextNeeds::game_fonts}; ///< what drawing its text needs
};

/// The word the settings keep for the operating system's choice of language.
inline constexpr std::string_view system_choice = "system";

/// The most languages a fallback chain holds: a language, the fallbacks an
/// entry may list and English.
inline constexpr std::size_t most_chain_languages = 6;

/// The most bytes of a locale read: longer text is not a locale.
inline constexpr std::size_t most_locale_bytes = 64;

/// The languages a text is looked up in, in order: a language, its
/// fallbacks, then English.
struct FallbackChain {
    std::array<const Language*, most_chain_languages> languages{}; ///< the first `count` hold one
    std::size_t count{}; ///< 1 or more; the last is always English

    /// Returns the chain's languages.
    ///
    /// @return the first `count` entries of `languages`
    [[nodiscard]] std::span<const Language* const> view() const noexcept {
        return {languages.data(), count};
    }
};

/// Returns every live language: English first, then the built-in and
/// installed languages in the order of their names' UTF-8 bytes, ties
/// broken by tag, then the available languages in that order.
///
/// An entry stays readable for the whole run, including one a later
/// set_pack_languages has replaced and left off this list. The list itself
/// is valid until that next call. The registry is changed and read on the
/// thread that draws the interface only.
///
/// @return the languages; never empty
[[nodiscard]] std::span<const Language* const> known_languages() noexcept;

/// Returns English, the game data's own language.
///
/// @return English's entry
[[nodiscard]] const Language& english() noexcept;

/// Tells whether this build draws text with these needs, and so may offer
/// a language or a pack that asks for them.
///
/// @param needs what drawing the text needs
/// @return true for TextNeeds::game_fonts and TextNeeds::modern_fonts, and
///     for TextNeeds::text_shaping on a build that shapes; the other needs
///     are not built yet
[[nodiscard]] bool meets(TextNeeds needs) noexcept;

/// Tells whether this build draws a language's text, and so offers it.
///
/// @param language the language
/// @return true when meets() is true for the language's needs
[[nodiscard]] bool drawable(const Language& language) noexcept;

/// Tells whether a language can be the one the game shows: its source is
/// not available, and this build draws it.
///
/// @param language the language
/// @return true when the setting, the operating system's locale or 3.1c's
///     command-line word may choose it
[[nodiscard]] bool playable(const Language& language) noexcept;

/// Tells whether a language turns Unicode multiplayer chat on while it is
/// shown: its text is held in UTF-8 (TextNeeds::modern_fonts, or
/// TextNeeds::text_shaping), so that a line typed in it reaches each
/// machine in the form that machine reads, or one of its packs asks for
/// it (unicode: true).
///
/// @param language the language shown
/// @param pack_asks a pack of the language asks for Unicode chat
/// @return true when it turns it on
[[nodiscard]] bool turns_unicode_chat_on(const Language& language, bool pack_asks) noexcept;

/// Finds any live language by its tag, a built-in, installed or available
/// one, matched without regard to the case of its letters, '_' read as '-'.
/// A language a later set_pack_languages replaced is not found.
///
/// @param tag a BCP-47 tag
/// @return the language; null for a tag no live entry has
[[nodiscard]] const Language* find_by_tag(std::string_view tag) noexcept;

/// Finds a playable language by the word 3.1c's command line and game data
/// name it by ("german", "english"), or its English name, matched without
/// regard to the case of its letters. An available language is not found.
///
/// @param name the word
/// @return the language; null for a word no playable entry has
[[nodiscard]] const Language* find_by_game_name(std::string_view name) noexcept;

/// Returns the chain a language's text is looked up in: the language, the
/// known fallbacks its entry lists, then English, each once.
///
/// @param language the language
/// @return the chain; English's is English alone
[[nodiscard]] FallbackChain fallback_chain(const Language& language) noexcept;

/// Writes a locale as the operating system gives it in BCP-47's form: the
/// encoding (".UTF-8") and modifier ("@euro") dropped and '_' read as '-'.
///
/// @param locale a locale, as "de_DE.UTF-8", "fr-CA" or "en"
/// @return the tag, as "de-DE"; empty for "C", "POSIX", text past
///     most_locale_bytes or text that is not a locale
[[nodiscard]] std::string normalised_locale(std::string_view locale);

/// Replaces the installed and available languages with the entries given.
/// The built-in languages stay. An entry equal in every field to a live one
/// keeps that entry; any other is added, and the live entry it replaces
/// leaves the list and stays readable until the game exits. The list is
/// then ordered again, and registry_generation changes when it differs.
///
/// A tag that is empty, or not the text normalised_locale leaves, is
/// dropped, as is an entry with no word. An empty name in itself becomes
/// the English name, or the tag when that is empty too; an empty English
/// name becomes the tag. Each locale is normalised, and one that does not
/// normalise is dropped. An entry whose tag is a built-in language's is
/// dropped, and the first of two entries with one tag wins. An installed
/// entry wins over an available one with the same tag, and a language
/// compiled in kOfferedLanguages wins over an available entry passed for
/// its tag.
///
/// The registry is changed and read on the thread that draws the interface
/// only.
///
/// @param installed the packs installed on this machine, player's then engine's
/// @param available languages a catalogue offers that are not installed
void set_pack_languages(
    std::span<const LanguageEntry> installed, std::span<const LanguageEntry> available
);

/// Returns a number that changes whenever known_languages() changes.
///
/// @return the generation, starting at one once the registry has been read
[[nodiscard]] uint64_t registry_generation() noexcept;

/// Finds the language a locale chooses: the entry with the longest of its
/// locales that is the locale's whole tag or its leading subtags. Only a
/// playable language is chosen, unless `with_available` is set, when an
/// available language this build can draw is chosen too. A locale of a
/// language the game does not offer yet, as Traditional Chinese's "zh-TW"
/// and "zh-Hant-HK", chooses none when its match is longer than any
/// entry's, so it never falls to a shorter one such as "zh".
///
/// @param locale a locale, in any form normalised_locale reads
/// @param with_available true to match an available language this build
///     can draw
/// @return the language; null when none is chosen
[[nodiscard]] const Language* match_locale(std::string_view locale, bool with_available = false);

/// Finds the language the preferred locales ask for among the playable
/// languages and the available ones this build can draw: the first locale,
/// in the order given, that matches one. Unlike preferred_language, an
/// available language counts, and no locale matching leaves null rather
/// than English.
///
/// @param locales the preferred locales, most preferred first
/// @return the language; null when none matches
[[nodiscard]] const Language* wanted_language(std::span<const std::string> locales);

/// Finds the language the operating system's preferred locales choose: the
/// first locale, in the order given, that chooses a playable language. An
/// available language is not chosen.
///
/// @param locales the preferred locales, most preferred first
/// @return the language; English when none chooses one
[[nodiscard]] const Language& preferred_language(std::span<const std::string> locales);

/// Returns the language a settings choice names. Only a playable language
/// is chosen: an available tag reads as the system's, so a language that
/// is not installed yet is never shown.
///
/// @param choice system_choice, or a tag; anything else, a tag of a
///     language that is not playable included, reads as system_choice
/// @param system the language the operating system chooses
/// @return the language
[[nodiscard]] const Language&
chosen_language(std::string_view choice, const Language& system) noexcept;

/// The lines a language's start-of-game notice shows in that language.
/// English is written beside them. A language with no entry is told in
/// English alone.
struct PackNotice {
    /// The body when the pack's size is known. "%s" stands where the size goes.
    std::string_view body_sized{};
    std::string_view body{};        ///< the body when the size is not known
    std::string_view download{};    ///< the Download button, kept whole
    std::string_view failed{};      ///< the line when the download failed
    std::string_view retry{};       ///< the button after a failure, kept whole
    std::string_view unavailable{}; ///< the line when the pack cannot be downloaded
};

/// Returns the notice lines the engine holds for a language.
///
/// The tag is matched without regard to the case of its letters, '_' read
/// as '-'. The lines stay for the whole run.
///
/// @param tag a BCP-47 tag
/// @return the lines; null when the engine holds none for that tag
[[nodiscard]] const PackNotice* pack_notice(std::string_view tag) noexcept;

} // namespace oa::data::languages
