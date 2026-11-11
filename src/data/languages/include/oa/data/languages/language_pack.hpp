// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Language packs: a folder of texts for one language, which adds to the
// game data's own translations entry by entry and never replaces them. A
// pack is a manifest, language.yaml, and optional tables, each a UTF-8 TDF
// file:
//
//     zh-Hans/
//       language.yaml   the manifest (PackManifest)
//       translate.tdf   the game's own texts, in Translate.tdf's shape:
//                       [English text] { <word> = ...; }
//       units.tdf       [UNITNAME] { name = ...; description = ...;
//                       name-from = <English>; description-from = <English>; }
//       missions.tdf    [mission file] { missionname = ...;
//                       missiondescription = ...; missionhint = ...; }
//       interface.tdf   the engine's own words, as the interface catalogue
//                       (interface_text.hpp) reads them
//       pictures.tdf    captions drawn over the player's own pictures
//       files/          whole files in the game data's language folders,
//                       as files/camps/briefs-<word>/<briefing>.txt
//       fonts/          font files the manifest's fonts key names
//       warmup.txt      characters drawn ahead, when the manifest names it
//
// The application reads the files and hands their bytes here; nothing here
// opens a file. For a language L the game tries, in order: a mod's pack for
// L, the game data in L's word, the player's pack for L, the engine's pack
// for L, then the same for each of L's fallbacks, then English. An absent
// or empty value falls through to the next. Packs change only what players
// read: nothing here reaches the simulation, a saved game or what a shared
// game sends.

#pragma once

#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/languages/unit_texts.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::languages {

/// The version of the pack format this build reads: the manifest's oalang.
inline constexpr int64_t pack_format_version = 1;

/// The name of a pack's manifest.
inline constexpr std::string_view pack_manifest_file = "language.yaml";

/// The name of a pack's table of the engine's own words.
inline constexpr std::string_view pack_interface_file = "interface.tdf";

/// The folder of a pack that holds whole files in the game data's language
/// folders.
inline constexpr std::string_view pack_files_folder = "files";

/// The folder a mod keeps its packs in, one folder per tag inside it.
inline constexpr std::string_view mod_languages_folder = "languages";

/// The most bytes of a pack's table read: a larger table is refused.
inline constexpr std::size_t most_pack_table_bytes = most_catalogue_bytes;

/// The folder of a pack that holds the font files its manifest names.
inline constexpr std::string_view pack_fonts_folder = "fonts";

/// The most font files a pack's manifest may name.
inline constexpr std::size_t most_pack_fonts = 4;

/// The most bytes of a pack's warm-up text: the font stack's max_text_bytes.
inline constexpr std::size_t most_warmup_bytes = 4096;

/// What a pack's font is drawn as.
enum class PackFontRole : uint8_t {
    ideographs, ///< drawn as Noto Sans CJK is, at the least size for its script
    letters,    ///< drawn at the sans faces' size
};

/// One font file a pack's manifest names, in the pack's fonts folder.
struct PackFont {
    std::string file;    ///< the file name, as "NotoSansCJKsc-Bold.otf"
    PackFontRole role{}; ///< how the face is drawn
};

/// Who numbered a pack and when, as a release of it (oamod.yaml's packaging).
struct PackPackaging {
    int64_t revision{};   ///< the pack's revision, 1 to 65535
    std::string date;     ///< the day it was made, YYYY-MM-DD
    std::string packager; ///< who made it, 1 to 128 bytes
};

/// A pack's manifest, language.yaml: which language the pack holds text in.
struct PackManifest {
    int64_t format{};                       ///< oalang: the pack format, pack_format_version
    std::string tag{};                      ///< the language's BCP-47 tag, as "zh-Hans"
    std::string name{};                     ///< its name in itself, as the settings list it
    std::string english_name{};             ///< its name in English
    std::string word{};                     ///< the word the game data knows it by, as "Chinese"
    std::string version{};                  ///< the pack's own version, as its author numbers it
    std::vector<std::string> locales{};     ///< the operating system's locales that choose it
    std::vector<std::string> fallbacks{};   ///< the tags its text falls back to before English
    TextNeeds needs{TextNeeds::game_fonts}; ///< text: needs: what drawing its text needs
    /// unicode: true asks that multiplayer chat be sent and read as UTF-8
    /// while the language is shown, whatever the player's setting.
    bool unicode{};
    /// An http or https address for the pack; empty when the manifest has none.
    std::string homepage{};
    /// How the pack is filed, in the order written; empty when the manifest has none.
    std::vector<std::string> tags{};
    /// The engine requirement as written; empty when the manifest has none.
    /// Reading the pack does not refuse one this build does not meet.
    std::string requires_engine{};
    /// Base games the pack's text is for. Every base game when supportedGames
    /// is omitted, or totala when requires.base is ta-3.1c and the key is omitted.
    std::vector<std::string> supported_games{};
    /// Expansions that must be on. Empty when the manifest names none.
    std::vector<std::string> required_expansions{};
    /// requires.base when it is ta-3.1c; empty when the manifest names no base.
    std::string requires_base{};
    /// Font files in the pack's fonts folder, in the order written; empty
    /// when the manifest names none.
    std::vector<PackFont> fonts{};
    /// The warm-up file's name in the pack's folder; empty when the manifest
    /// names none.
    std::string warmup{};
    /// The pack's revision, when the manifest gives one.
    std::optional<PackPackaging> packaging{};
};

/// Reads a pack's manifest, in the strict YAML of mod profiles
/// (oa/formats/oamod.hpp).
///
/// oalang, tag and word are required, and oalang must be
/// pack_format_version; name, english-name, version, locales, fallbacks,
/// text (needs: game-fonts, modern-fonts or shaping), unicode, homepage, tags,
/// supportedGames, requiredExpansions, requires (engine, and base when it
/// is ta-3.1c), fonts, warmup and packaging are optional. homepage, tags
/// and requires.engine follow the shared package-key rules. An omitted
/// supportedGames means every base game, unless requires.base is ta-3.1c,
/// which then reads as totala. An engine requirement this build does not meet is
/// still read. fonts names at most most_pack_fonts files in the pack's
/// fonts folder, each with a role of ideographs or letters. warmup names a
/// text file in the pack's folder. packaging gives a revision from 1 to
/// 65535, an ISO 8601 date and a packager of 1 to 128 bytes. A manifest
/// without fonts, warmup and packaging still reads. Keys it does not know
/// are refused, so that a misspelt one is noticed.
///
/// @param bytes the manifest's bytes
/// @param[out] manifest the manifest; left unchanged on failure
/// @param[out] error why the manifest was refused; may be null
/// @return true when the manifest was read
[[nodiscard]] bool
read_manifest(std::span<const uint8_t> bytes, PackManifest& manifest, std::string* error = nullptr);

/// Returns the registry entry a pack's manifest describes.
///
/// The tag, the name in itself, the English name, the word, the locales,
/// the fallbacks and what drawing the text needs are copied as the manifest
/// gives them. set_pack_languages applies the registry's own rules.
///
/// @param manifest the pack's manifest
/// @return the entry
[[nodiscard]] LanguageEntry entry_of(const PackManifest& manifest);

/// The tables of a pack other than its manifest and interface.tdf.
enum class PackTable : uint8_t {
    translate, ///< translate.tdf: the game's own texts, by their English
    units,     ///< units.tdf: unit types' names and descriptions, by UnitName
    missions,  ///< missions.tdf: missions' names and descriptions, by mission file
    pictures,  ///< pictures.tdf: captions drawn over the player's own pictures
};

/// Every PackTable, in order.
inline constexpr std::array<PackTable, 4> pack_tables{
    PackTable::translate, PackTable::units, PackTable::missions, PackTable::pictures
};

/// Returns the file name a table is read from.
///
/// @param table the table
/// @return its name inside the pack, as "units.tdf"
[[nodiscard]] std::string_view pack_table_file(PackTable table) noexcept;

/// Captions drawn over the player's own pictures (pictures.tdf): each names
/// a picture by its section's path, the names of its nested sections joined
/// by '/' and matched without regard to case ("igtitles/paused"), and holds
/// the keys its drawing reads, by lower-case key. What the keys mean is the
/// drawing's; a section with no keys turns the caption off.
class PictureCaptions {
  public:

    /// One caption's keys and values, by lower-case key.
    using Keys = std::map<std::string, std::string, std::less<>>;

    /// Adds the captions a pictures.tdf file holds; a caption added again
    /// replaces the earlier one whole.
    ///
    /// @param file the file's bytes, at most most_pack_table_bytes
    /// @param[out] error why the file was refused; may be null
    /// @return false when the file is too large or does not parse; nothing
    ///     is added then
    bool add(std::string_view file, std::string* error = nullptr);

    /// Adds every caption of another set this one does not hold, so that
    /// this set's own win.
    ///
    /// @param lower the set whose captions come after this one's
    void add_missing(const PictureCaptions& lower);

    /// Finds a caption.
    ///
    /// @param path the picture's path, in any case
    /// @return its keys; null when no caption names it, or when it turns
    ///     the caption off
    [[nodiscard]] const Keys* find(std::string_view path) const;

    /// Returns every caption, by lower-case path; those that turn the
    /// caption off hold no keys.
    ///
    /// @return the captions
    [[nodiscard]] const std::map<std::string, Keys, std::less<>>& all() const noexcept {
        return captions_;
    }

    /// Forgets every caption.
    void clear() noexcept { captions_.clear(); }

  private:

    /// The captions, by lower-case path.
    std::map<std::string, Keys, std::less<>> captions_{};
};

/// One unit type's texts in a pack, each with the English it translates.
struct PackUnitText {
    std::string name{};             ///< its name; empty for none
    std::string name_from{};        ///< the English name it translates; empty for any
    std::string description{};      ///< its description; empty for none
    std::string description_from{}; ///< the English description it translates; empty for any
};

/// Tells whether a pack's text still translates the English the game data
/// gives: its -from field is empty, equal to the English, or begins with the
/// English that a field of the given size cut.
///
/// @param from the English the pack's text translates; empty for any
/// @param english the English the game data gives now
/// @param field_bytes the bytes of the field the English is kept in,
///     its NUL included; 0 when it is not cut
/// @return true when the text applies
[[nodiscard]] bool
translates(std::string_view from, std::string_view english, std::size_t field_bytes) noexcept;

/// One language pack's tables.
class LanguagePack {
  public:

    /// Makes an empty pack for a manifest's language.
    ///
    /// @param manifest the pack's manifest
    explicit LanguagePack(PackManifest manifest) : manifest_(std::move(manifest)) {}

    /// Adds a table's entries; an entry added again replaces the earlier
    /// one, and an empty value adds nothing.
    ///
    /// translate.tdf's values are read under the manifest's word, or its
    /// tag; units.tdf's under name, description, name-from and
    /// description-from; missions.tdf's under missionname,
    /// missiondescription and missionhint.
    ///
    /// @param table the table
    /// @param file the file's bytes, at most most_pack_table_bytes
    /// @param[out] error why the file was refused; may be null
    /// @return false when the file is too large or does not parse; nothing
    ///     is added then
    bool add(PackTable table, std::string_view file, std::string* error = nullptr);

    /// Returns the pack's manifest.
    ///
    /// @return the manifest
    [[nodiscard]] const PackManifest& manifest() const noexcept { return manifest_; }

    /// Translates one of the game's own texts.
    ///
    /// @param english the text, exactly as the game data holds it
    /// @return the translation; null without one
    [[nodiscard]] const std::string* translation(std::string_view english) const;

    /// Returns a unit type's name in the pack, when it translates the
    /// English the game data gives now.
    ///
    /// @param unit_name the unit's name (UnitDef.unit_name), any case
    /// @param english the type's own name, as UnitDef.name holds it
    /// @return the name; null without one, or when its name-from differs
    [[nodiscard]] const std::string*
    unit_name(std::string_view unit_name, std::string_view english) const;

    /// Returns a unit type's description in the pack, when it translates
    /// the English the game data gives now.
    ///
    /// @param unit_name the unit's name (UnitDef.unit_name), any case
    /// @param english the type's own description, as UnitDef.description holds it
    /// @return the description; null without one, or when its
    ///     description-from differs
    [[nodiscard]] const std::string*
    unit_description(std::string_view unit_name, std::string_view english) const;

    /// Returns a mission's text in the pack.
    ///
    /// @param mission_file the mission's file as the campaign names it
    ///     ("Lipar Pass.ota"), any case, with or without its extension
    /// @param key missionname, missiondescription or missionhint, any case
    /// @return the text; null without one
    [[nodiscard]] const std::string*
    mission_text(std::string_view mission_file, std::string_view key) const;

    /// Returns the captions the pack draws over pictures.
    ///
    /// @return the captions
    [[nodiscard]] const PictureCaptions& pictures() const noexcept { return pictures_; }

    /// Returns every unit text the pack holds, by unit name.
    ///
    /// @return the texts
    [[nodiscard]] const std::map<std::string, PackUnitText, UnitTexts::NoCaseLess>&
    units() const noexcept {
        return units_;
    }

    /// Returns every translation translate.tdf gave, by English text.
    ///
    /// @return the translations
    [[nodiscard]] const std::map<std::string, std::string, std::less<>>&
    translations() const noexcept {
        return translations_;
    }

  private:

    PackManifest manifest_{}; ///< the pack's manifest
    /// The game's texts, by their exact English.
    std::map<std::string, std::string, std::less<>> translations_{};
    /// Unit texts, by unit name in any case.
    std::map<std::string, PackUnitText, UnitTexts::NoCaseLess> units_{};
    /// Missions' texts: lower-case mission file without extension, then
    /// lower-case key.
    std::map<std::string, std::map<std::string, std::string, std::less<>>, std::less<>> missions_{};
    PictureCaptions pictures_{}; ///< the captions
};

/// The packs one language's text is looked up in, around its game data.
struct PackLayer {
    /// The word the game data knows the language by, as data_words gives
    /// it: the layer applies to lookups by that word.
    std::string word{};
    /// The packs tried before the game data: a mod's, first winning.
    std::vector<const LanguagePack*> before_data{};
    /// The packs tried after the game data: the player's, then the engine's.
    std::vector<const LanguagePack*> after_data{};
};

/// Installs the language packs the interface's unit names and descriptions
/// are looked up in, around the game data's (set_unit_texts): for each word
/// of the installed words, a mod's packs, the game data in the word, then
/// the player's and the engine's packs.
///
/// @param layers the layers, kept until the next call; empty for none
void set_unit_pack_layers(std::span<const PackLayer> layers) noexcept;

/// Finds the layer of a word.
///
/// @param layers the layers
/// @param word the game data's word, any case
/// @return the layer; null when none has the word
[[nodiscard]] const PackLayer* layer_of(std::span<const PackLayer> layers, std::string_view word);

/// Translates one of the game's own texts through the layers: for each
/// word in order, its layer's packs before the game data, the game data's
/// own translation in the word, then its layer's packs after it. A text no
/// source translates shows as it is, as 3.1c shows it.
///
/// @param layers the language packs, by word
/// @param words the words to try, in order (data_words)
/// @param english the text, exactly as the game data holds it
/// @param data the game data's own translation in the word at an index of
///     `words`; it returns null for none
/// @return the translation, valid while the packs and the data's tables
///     live; null when no source has one
[[nodiscard]] const char* layered_translation(
    std::span<const PackLayer> layers,
    std::span<const std::string> words,
    std::string_view english,
    const std::function<const char*(std::size_t)>& data
);

/// Returns a mission's text through the layers: for each word in order,
/// its layer's packs before the game data, the game data's own text (for
/// the first word only), then its layer's packs after it.
///
/// @param layers the language packs, by word
/// @param words the words to try, in order (data_words)
/// @param mission_file the mission's file, as the campaign names it
/// @param key missionname, missiondescription or missionhint
/// @param data_text the game data's own text in the first word; null for none
/// @return the text, valid while the packs live, or `data_text`; null when
///     no source has one
[[nodiscard]] const char* layered_mission_text(
    std::span<const PackLayer> layers,
    std::span<const std::string> words,
    std::string_view mission_file,
    std::string_view key,
    const char* data_text
);

/// Returns the captions drawn over pictures for the words: each word's
/// layer's packs in order, a caption of an earlier pack winning whole.
///
/// @param layers the language packs, by word
/// @param words the words to try, in order (data_words)
/// @return the captions
[[nodiscard]] PictureCaptions
layered_pictures(std::span<const PackLayer> layers, std::span<const std::string> words);

} // namespace oa::data::languages
