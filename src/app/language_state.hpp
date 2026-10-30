// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The language the game shows its text in (Runtime::LanguageState): the
// operating system's choice, the player's setting, the word the game data
// knows the language by, the units' names and descriptions in every
// language their files give, and the interface catalogue of the engine's
// own words. runtime_language.cpp chooses the language at start and each
// time the setting changes, and puts it in effect.
#pragma once

#include "language_packs.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/languages/language_pack.hpp"
#include "oa/data/languages/unit_texts.hpp"
#include "oa/data/defs/locale.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace oa::app {

struct Runtime::LanguageState {
    /// The language the operating system's preferred locales choose, which
    /// the setting's System default shows in.
    const oa::data::languages::Language* system{&oa::data::languages::english()};
    /// The setting: oa::data::languages::system_choice or a tag.
    std::string choice{oa::data::languages::system_choice};
    /// The language the game shows its text in.
    const oa::data::languages::Language* shown{&oa::data::languages::english()};
    /// The word the game data's lookups use: 3.1c's command-line word as
    /// typed when no playable language has it, else the shown language's
    /// game_name. Either stays for the whole run, so a copy of it never
    /// dangles.
    const char* data_word{"English"};
    /// The words a unit's name and description are looked up by, in order
    /// (oa::data::languages::data_words).
    std::vector<std::string> words;
    /// The words the unit loaders read each unit's texts in: every live
    /// language's, one that is not installed included, and the command
    /// line's word when no playable language has it and the list does not
    /// already hold it. They are set once, so that the loaders' pointers
    /// to them hold.
    std::vector<std::string> sink_words;
    /// sink_words as the loaders take them.
    std::vector<const char*> sink_word_pointers;
    /// The sink the unit loaders hand each unit's texts to, which fills
    /// unit_texts; installed for every loader (oa::data::languages::unit_text_sink).
    oa::data::defs::UnitTextSink sink{};
    /// The units' names and descriptions in every language their files give.
    oa::data::languages::UnitTexts unit_texts;
    /// The interface catalogue of the engine's own words.
    oa::data::languages::InterfaceText catalogue;
    /// True after a translation asked for the catalogue and it was read,
    /// including a read that found nothing. Cleared when the catalogue is
    /// dropped, so the next such word reads it again.
    bool catalogue_sought{};
    /// The game data's translation table and fonts are loaded for a word.
    bool loaded{};
    /// The word they are loaded for.
    std::string loaded_word;

    /// A translation table of gamedata/translate.tdf for one word, freed
    /// with it.
    struct FallbackTable {
        oa::data::defs::LocaleTable table{};

        /// Starts with no language loaded.
        FallbackTable() noexcept { oa::data::defs::locale_table_init(&table); }

        /// Frees the loaded texts.
        ~FallbackTable() { oa::data::defs::locale_table_free(&table); }

        FallbackTable(const FallbackTable&) = delete;
        FallbackTable& operator=(const FallbackTable&) = delete;
    };

    /// The translation tables of the words after the first (words), tried
    /// in order after the game's own table: a language's fallbacks.
    std::vector<std::unique_ptr<FallbackTable>> fallback_tables;

    /// The language packs of the mod played (its languages folder), read at
    /// start and again when a pack is installed; they come before the game data.
    std::vector<std::unique_ptr<LoadedLanguagePack>> mod_packs;
    /// The player's language packs (Languages in their own folder), read at
    /// start and again when a pack is installed; they come after the game
    /// data, before the engine's.
    std::vector<std::unique_ptr<LoadedLanguagePack>> player_packs;
    /// The engine's own language packs (the languages folder beside the
    /// game's other files), read at start and again when a pack is installed;
    /// they come last.
    std::vector<std::unique_ptr<LoadedLanguagePack>> engine_packs;
    /// The packs each of words is looked up in, set with the language.
    std::vector<oa::data::languages::PackLayer> layers;
    /// The font files of the language shown, in the order a stack adds
    /// them: each word's packs before the game data, then those after it,
    /// each pack's fonts in manifest order, a path once, and at most
    /// oa::platform::text_font::most_pack_faces.
    std::vector<PackFontFile> fonts;
    /// The warm-up text of the first of those packs that has one.
    std::string warmup;
    /// Bumped when fonts or warmup change, so the modern fonts follow them.
    uint64_t fonts_generation{};
    /// The captions drawn over pictures in the language shown.
    oa::data::languages::PictureCaptions pictures;
    /// The manifest of the first pack of the language shown that asks for
    /// multiplayer chat in UTF-8 (unicode: true); null for none.
    const oa::data::languages::PackManifest* unicode_manifest{};
    /// The bytes of the last file of the language folders a pack answered,
    /// which TranslationHooks::language_file's answer points to.
    std::string language_file;

    /// Language packs the catalogues offer that are not installed, each with
    /// the registry that lists it. The first of two with one tag is kept.
    std::vector<Runtime::CatalogueLanguage> available;
    /// The catalogue generation `available` was read from.
    uint64_t catalogue_generation{};
    /// True once `available` has been read from the catalogues or a check.
    bool catalogue_seen{};
    /// A check's list stays until the check lets the catalogues replace it.
    bool hold_available{};
    /// The download queue generation the open dialog was last filled from.
    uint64_t dialog_downloads_generation{};
    /// True once the open dialog was filled for those generations.
    bool dialog_languages_filled{};
};

} // namespace oa::app
