// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Language packs: the pseudo-language's pack of the tests' own data read
// whole, manifests refused by rule, and the layers a text is looked up
// through: a mod's pack before the game data, the player's and the
// engine's after it, an empty value and a text no pack has falling
// through, and a unit text written for other English skipped.
//
// The command line names the pseudo-language's pack folder.

#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/languages/language_pack.hpp"
#include "oa/data/languages/unit_texts.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace languages = oa::data::languages;

/// The pseudo-language's pack folder, from the command line.
std::filesystem::path pack_folder;

/// Reads a file of the pseudo-language's pack.
///
/// @param name its path inside the pack
/// @return its bytes; empty when it cannot be read
std::string pack_file(std::string_view name) {
    std::ifstream input(pack_folder / std::filesystem::path(name), std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/// Views a text's bytes.
///
/// @param text the text
/// @return its bytes
std::span<const uint8_t> bytes_of(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

/// Reads the pseudo-language's pack: its manifest and every table.
///
/// @return the pack
languages::LanguagePack pseudo_pack() {
    languages::PackManifest manifest;
    OA_CHECK(
        languages::read_manifest(bytes_of(pack_file(languages::pack_manifest_file)), manifest)
    );
    languages::LanguagePack pack(manifest);
    for (const languages::PackTable table : languages::pack_tables) {
        std::string error;
        OA_CHECK(pack.add(table, pack_file(languages::pack_table_file(table)), &error));
        OA_CHECK(error.empty());
    }
    return pack;
}

/// Makes a pack of the pseudo-language from tables given as text.
///
/// @param translate translate.tdf's text
/// @param units units.tdf's text
/// @return the pack
languages::LanguagePack
pack_of(std::string_view translate, std::string_view units, std::string_view pictures = {}) {
    languages::PackManifest manifest;
    manifest.format = languages::pack_format_version;
    manifest.tag = "en-XA";
    manifest.word = "Pseudo";
    languages::LanguagePack pack(manifest);
    OA_CHECK(pack.add(languages::PackTable::translate, translate));
    OA_CHECK(pack.add(languages::PackTable::units, units));
    OA_CHECK(pack.add(languages::PackTable::pictures, pictures));
    return pack;
}

/// Copies text into a zero-filled character field, keeping its last byte zero.
///
/// @param field the field
/// @param text the text; the characters that fit are copied
template <std::size_t Size>
void copy_text(char (&field)[Size], std::string_view text) {
    std::memcpy(field, text.data(), std::min(text.size(), Size - 1));
}

/// The pseudo-language's manifest and tables read whole.
void pseudo_pack_reads_whole() {
    const languages::LanguagePack pack = pseudo_pack();
    const languages::PackManifest& manifest = pack.manifest();
    OA_CHECK(manifest.format == 1);
    OA_CHECK(manifest.tag == "en-XA");
    OA_CHECK(manifest.word == "Pseudo");
    OA_CHECK(manifest.english_name == "Pseudo");
    OA_CHECK(manifest.version == "1");
    OA_CHECK(manifest.locales == std::vector<std::string>({"en-XA"}));
    OA_CHECK(manifest.fallbacks.empty());
    OA_CHECK(manifest.needs == languages::TextNeeds::modern_fonts);
    OA_CHECK(manifest.unicode);
    OA_CHECK(manifest.homepage == "https://example.org/pseudo");
    OA_CHECK(manifest.tags == std::vector<std::string>{"test"});
    OA_CHECK(manifest.requires_engine == ">= 0.0.1");
    OA_CHECK(manifest.warmup == "warmup.txt");
    OA_CHECK(manifest.fonts.empty());
    const languages::LanguageEntry entry = languages::entry_of(manifest);
    OA_CHECK(entry.tag == "en-XA");
    OA_CHECK(entry.endonym == manifest.name);
    OA_CHECK(
        entry.endonym == "\xEF\xBC\xBB\xE6\xB5\x8B"
                         "Pseudo"
                         "\xE8\xAF\x95\xEF\xBC\xBD"
    );
    OA_CHECK(entry.english_name == "Pseudo");
    OA_CHECK(entry.word == "Pseudo");
    OA_CHECK(entry.locales == std::vector<std::string>({"en-XA"}));
    OA_CHECK(entry.fallbacks.empty());
    OA_CHECK(entry.needs == languages::TextNeeds::modern_fonts);
    // A caption and a message, keyed by their English; an empty value adds
    // nothing.
    OA_CHECK(pack.translations().size() == 2);
    OA_CHECK(pack.translation("Select Map") != nullptr);
    OA_CHECK(pack.translation("Empty") == nullptr);
    OA_CHECK(pack.translation("select map") == nullptr);
    OA_CHECK(pack.units().size() == 2);
    OA_CHECK(pack.mission_text("lipar pass", "MissionName") != nullptr);
    OA_CHECK(pack.mission_text("Lipar Pass.ota", "missiondescription") != nullptr);
    OA_CHECK(pack.mission_text("Lipar Pass.ota", "missionhint") == nullptr);
    // A nested caption, and one turned off.
    const auto* paused = pack.pictures().find("IGTitles/Paused");
    OA_CHECK(paused != nullptr && paused->count("text") == 1 && paused->at("size") == "16");
    OA_CHECK(pack.pictures().find("paneltop") == nullptr);
    OA_CHECK(pack.pictures().all().count("paneltop") == 1);
    // The engine's own words are the interface catalogue's.
    languages::InterfaceText catalogue;
    OA_CHECK(catalogue.add(pack_file(languages::pack_interface_file)));
    OA_CHECK(catalogue.size() == 1);
    OA_CHECK(!pack_file("files/camps/briefs-Pseudo/brief.txt").empty());
}

/// A manifest is refused by the rule it breaks, and keeps the one before.
void manifests_are_refused_by_rule() {
    const auto refused = [](std::string_view text) {
        languages::PackManifest manifest;
        manifest.tag = "kept";
        std::string error;
        const bool read = languages::read_manifest(bytes_of(text), manifest, &error);
        OA_CHECK(manifest.tag == "kept");
        return !read && !error.empty();
    };
    OA_CHECK(refused("tag: zh-Hans\nword: Chinese\n"));
    OA_CHECK(refused("oalang: 2\ntag: zh-Hans\nword: Chinese\n"));
    OA_CHECK(refused("oalang: 1\ntag: zh-Hans\n"));
    OA_CHECK(refused("oalang: 1\nword: Chinese\n"));
    OA_CHECK(refused("oalang: 1\ntag: zh-Hans\nword: Chinese\nspelling: wrong\n"));
    OA_CHECK(refused("oalang: 1\ntag: zh-Hans\nword: Chinese\ntext: {needs: kerning}\n"));
    OA_CHECK(refused("oalang: 1\ntag: zh-Hans\nword: Chinese\nunicode: yes\n"));
    OA_CHECK(refused("oalang: 1\ntag: zh Hans\nword: Chinese\n"));
    OA_CHECK(refused("oalang: 1\ntag: zh-Hans\nword: Chinese\nlocales: zh\n"));
    OA_CHECK(refused("oalang: 1\ntag: zh-Hans\nword: [Chinese\n"));
    languages::PackManifest manifest;
    OA_CHECK(
        languages::read_manifest(bytes_of("oalang: 1\ntag: zh-Hans\nword: Chinese\n"), manifest)
    );
    OA_CHECK(manifest.needs == languages::TextNeeds::game_fonts && !manifest.unicode);
    OA_CHECK(manifest.fonts.empty() && manifest.warmup.empty() && !manifest.packaging);
    languages::PackManifest shaped;
    OA_CHECK(
        languages::read_manifest(
            bytes_of("oalang: 1\ntag: bn\nword: Bengali\ntext: {needs: shaping}\n"), shaped
        )
    );
    OA_CHECK(shaped.needs == languages::TextNeeds::text_shaping);
    OA_CHECK(shaped.tag == "bn" && shaped.word == "Bengali");

    // fonts, warmup and packaging are read beside homepage, tags and requires.
    languages::PackManifest held;
    std::string error;
    OA_CHECK(
        languages::read_manifest(
            bytes_of(
                "oalang: 1\n"
                "tag: zh-Hans\n"
                "word: Chinese\n"
                "homepage: \"https://example.org/lang\"\n"
                "tags: [translation]\n"
                "requires: {engine: \">= 0.0.1\"}\n"
                "fonts:\n"
                "  - {file: NotoSansCJKsc-Bold.otf, role: ideographs}\n"
                "  - {file: Letters.TTF, role: letters}\n"
                "  - {file: Third.otf, role: ideographs}\n"
                "  - {file: Fourth.ttf, role: letters}\n"
                "warmup: warmup.txt\n"
                "packaging: {revision: 3, date: \"2026-10-10\", packager: Ridge}\n"
            ),
            held,
            &error
        )
    );
    OA_CHECK(error.empty());
    OA_CHECK(held.homepage == "https://example.org/lang");
    OA_CHECK(held.tags == std::vector<std::string>{"translation"});
    OA_CHECK(held.requires_engine == ">= 0.0.1");
    OA_CHECK(held.fonts.size() == 4);
    if (held.fonts.size() == 4) {
        OA_CHECK(held.fonts[0].file == "NotoSansCJKsc-Bold.otf");
        OA_CHECK(held.fonts[0].role == languages::PackFontRole::ideographs);
        OA_CHECK(held.fonts[1].file == "Letters.TTF");
        OA_CHECK(held.fonts[1].role == languages::PackFontRole::letters);
    }
    OA_CHECK(held.warmup == "warmup.txt");
    OA_CHECK(held.packaging && held.packaging->revision == 3);
    OA_CHECK(held.packaging && held.packaging->date == "2026-10-10");
    OA_CHECK(held.packaging && held.packaging->packager == "Ridge");

    const auto refused_for = [](std::string_view text, std::string_view key) {
        languages::PackManifest kept;
        kept.tag = "kept";
        std::string failure;
        const bool read = languages::read_manifest(bytes_of(text), kept, &failure);
        OA_CHECK(kept.tag == "kept");
        return !read && failure.find(key) != std::string::npos;
    };
    constexpr std::string_view stem = "oalang: 1\ntag: zh-Hans\nword: Chinese\n";
    const auto with = [stem](std::string_view rest) {
        return std::string(stem) + std::string(rest);
    };
    OA_CHECK(refused_for(with("fonts: yes\n"), "fonts"));
    OA_CHECK(refused_for(with("warmup: 1\n"), "warmup"));
    OA_CHECK(refused_for(with("packaging: []\n"), "packaging"));
    OA_CHECK(refused_for(
        with(
            "fonts:\n"
            "  - {file: A.otf, role: ideographs}\n"
            "  - {file: B.otf, role: letters}\n"
            "  - {file: C.ttf, role: ideographs}\n"
            "  - {file: D.ttf, role: letters}\n"
            "  - {file: E.otf, role: ideographs}\n"
        ),
        "fonts"
    ));
    OA_CHECK(refused_for(
        with(
            "fonts:\n"
            "  - {file: A.otf, role: ideographs}\n"
            "  - {file: A.otf, role: letters}\n"
        ),
        "fonts"
    ));
    OA_CHECK(refused_for(with("fonts:\n  - {file: fonts/x.otf, role: ideographs}\n"), "fonts"));
    OA_CHECK(refused_for(with("fonts:\n  - {file: ../x.otf, role: ideographs}\n"), "fonts"));
    OA_CHECK(refused_for(with("fonts:\n  - {file: x.woff, role: ideographs}\n"), "fonts"));
    OA_CHECK(refused_for(with("fonts:\n  - {file: x.otf, role: symbols}\n"), "fonts"));
    OA_CHECK(refused_for(with("fonts:\n  - {file: x.otf, role: ideographs, extra: 1}\n"), "fonts"));
    OA_CHECK(refused_for(with("warmup: fonts/warm.txt\n"), "warmup"));
    OA_CHECK(refused_for(with("warmup: ../warm.txt\n"), "warmup"));
    OA_CHECK(refused_for(
        with("packaging: {revision: 0, date: \"2026-10-10\", packager: Ridge}\n"), "revision"
    ));
    OA_CHECK(refused_for(
        with("packaging: {revision: 65536, date: \"2026-10-10\", packager: Ridge}\n"), "revision"
    ));
    OA_CHECK(refused_for(
        with("packaging: {revision: 1, date: \"2026-13-01\", packager: Ridge}\n"), "date"
    ));
    OA_CHECK(refused_for(
        with("packaging: {revision: 1, date: \"2026-10-10\", packager: Ridge, extra: 1}\n"),
        "packaging"
    ));
}

/// homepage, tags and requires.engine are read; a broken one is refused,
/// and an unmet engine requirement is kept.
void package_keys_are_read_and_checked() {
    languages::PackManifest manifest;
    std::string error;
    OA_CHECK(
        languages::read_manifest(
            bytes_of(
                "oalang: 1\n"
                "tag: zh-Hans\n"
                "word: Chinese\n"
                "homepage: \"https://example.org/lang\"\n"
                "tags: [translation]\n"
                "requires: {engine: \">= 0.0.1\"}\n"
            ),
            manifest,
            &error
        )
    );
    OA_CHECK(error.empty());
    OA_CHECK(manifest.homepage == "https://example.org/lang");
    OA_CHECK(manifest.tags == std::vector<std::string>{"translation"});
    OA_CHECK(manifest.requires_engine == ">= 0.0.1");

    const auto refused = [](std::string_view text) {
        languages::PackManifest kept;
        kept.tag = "kept";
        std::string failure;
        const bool read = languages::read_manifest(bytes_of(text), kept, &failure);
        OA_CHECK(kept.tag == "kept");
        return !read && !failure.empty();
    };
    OA_CHECK(refused("oalang: 1\ntag: zh-Hans\nword: Chinese\nhomepage: \"javascript:no\"\n"));
    OA_CHECK(refused("oalang: 1\ntag: zh-Hans\nword: Chinese\ntags: [Not-Kebab]\n"));
    OA_CHECK(refused("oalang: 1\ntag: zh-Hans\nword: Chinese\ntags: []\n"));
    OA_CHECK(refused("oalang: 1\ntag: zh-Hans\nword: Chinese\nrequires: {engine: \">= 1.2\"}\n"));
    OA_CHECK(refused("oalang: 1\ntag: zh-Hans\nword: Chinese\nrequires: {base: other}\n"));

    languages::PackManifest unmet;
    OA_CHECK(
        languages::read_manifest(
            bytes_of(
                "oalang: 1\ntag: zh-Hans\nword: Chinese\nrequires: {engine: \">= 9999.0.0\"}\n"
            ),
            unmet,
            &error
        )
    );
    OA_CHECK(unmet.requires_engine == ">= 9999.0.0");
}

/// supportedGames defaults to every base game, and requires.base reads as totala.
void content_needs_are_read() {
    languages::PackManifest minimal;
    std::string error;
    OA_CHECK(
        languages::read_manifest(
            bytes_of("oalang: 1\ntag: zh-Hans\nword: Chinese\n"), minimal, &error
        )
    );
    OA_CHECK(error.empty());
    OA_CHECK(minimal.supported_games == (std::vector<std::string>{"totala", "totala-demo"}));
    OA_CHECK(minimal.required_expansions.empty());
    OA_CHECK(minimal.requires_base.empty());

    languages::PackManifest named;
    OA_CHECK(
        languages::read_manifest(
            bytes_of(
                "oalang: 1\n"
                "tag: zh-Hans\n"
                "word: Chinese\n"
                "supportedGames: [\"totala\", \"totala-demo\"]\n"
                "requiredExpansions: [\"music\"]\n"
            ),
            named,
            &error
        )
    );
    OA_CHECK(named.supported_games == (std::vector<std::string>{"totala", "totala-demo"}));
    OA_CHECK(named.required_expansions == std::vector<std::string>{"music"});

    languages::PackManifest legacy;
    OA_CHECK(
        languages::read_manifest(
            bytes_of("oalang: 1\ntag: zh-Hans\nword: Chinese\nrequires: {base: ta-3.1c}\n"),
            legacy,
            &error
        )
    );
    OA_CHECK(legacy.supported_games == std::vector<std::string>{"totala"});
    OA_CHECK(legacy.requires_base == "ta-3.1c");

    languages::PackManifest both;
    OA_CHECK(
        languages::read_manifest(
            bytes_of(
                "oalang: 1\n"
                "tag: zh-Hans\n"
                "word: Chinese\n"
                "supportedGames: [\"totala-demo\"]\n"
                "requires: {base: ta-3.1c}\n"
            ),
            both,
            &error
        )
    );
    OA_CHECK(both.supported_games == std::vector<std::string>{"totala-demo"});
    OA_CHECK(both.requires_base == "ta-3.1c");

    const auto refused = [](std::string_view text) {
        languages::PackManifest kept;
        kept.tag = "kept";
        std::string failure;
        const bool read = languages::read_manifest(bytes_of(text), kept, &failure);
        OA_CHECK(kept.tag == "kept");
        return !read ? failure : std::string{};
    };
    const std::string unknown =
        refused("oalang: 1\ntag: zh-Hans\nword: Chinese\nsupportedGames: [\"no-such\"]\n");
    OA_CHECK(unknown.find("no-such") != std::string::npos);
    OA_CHECK(unknown.find("is not a base game") != std::string::npos);
    const std::string extends = refused(
        "oalang: 1\ntag: zh-Hans\nword: Chinese\n"
        "supportedGames: [\"totala-demo\"]\n"
        "requiredExpansions: [\"core-contingency\"]\n"
    );
    OA_CHECK(extends.find("core-contingency") != std::string::npos);
    OA_CHECK(extends.find("extends none") != std::string::npos);
    OA_CHECK(
        refused("oalang: 1\ntag: zh-Hans\nword: Chinese\nrequires: {base: totala}\n")
            .find("ta-3.1c") != std::string::npos
    );
    OA_CHECK(
        refused("oalang: 1\ntag: zh-Hans\nword: Chinese\nsupportedGames: []\n")
            .find("at least one") != std::string::npos
    );
}

/// The game's own texts: a mod's pack first, then the game data in the
/// language, then the player's pack, then the engine's; an empty value and
/// a text no source has fall through, so a text left out of every pack
/// shows as the game data has it.
void game_texts_are_looked_up_through_the_layers() {
    const languages::LanguagePack engine = pseudo_pack();
    const languages::LanguagePack player = pack_of(
        "[Select Map] { Pseudo=player map; }\n[Empty] { Pseudo=player empty; }\n"
        "[Kills] { Pseudo=player kills; }\n",
        ""
    );
    const languages::LanguagePack mod = pack_of("[Kills] { Pseudo=mod kills; }\n", "");
    std::vector<languages::PackLayer> layers(1);
    layers[0].word = "PSEUDO";
    layers[0].before_data = {&mod};
    layers[0].after_data = {&player, &engine};
    const std::vector<std::string> words{"Pseudo"};
    const auto data = [](std::size_t) -> const char* { return nullptr; };
    const auto data_select = [](std::size_t index) -> const char* {
        return index == 0 ? "data map" : nullptr;
    };
    const auto lookup = [&](std::string_view english, bool with_data) {
        const char* text = languages::layered_translation(
            layers,
            words,
            english,
            with_data ? std::function<const char*(std::size_t)>(data_select)
                      : std::function<const char*(std::size_t)>(data)
        );
        return text != nullptr ? std::string(text) : std::string("(game data)");
    };
    OA_CHECK(lookup("Kills", true) == "mod kills");
    OA_CHECK(lookup("Select Map", true) == "data map");
    OA_CHECK(lookup("Select Map", false) == "player map");
    OA_CHECK(lookup("Empty", false) == "player empty");
    OA_CHECK(lookup("%s has been destroyed with %d units", false).find("%s") != std::string::npos);
    // Every pack leaves out a text naming who made the game: its own text shows.
    OA_CHECK(lookup("Made by Example Games", false) == "(game data)");
    // A word without packs has only its game data.
    OA_CHECK(
        languages::layered_translation(layers, std::vector<std::string>{"German"}, "Kills", data) ==
        nullptr
    );
}

/// Units' names and descriptions: a mod's pack, the game data's own in the
/// language, the player's pack, the engine's pack, then the type's own; a
/// text written for other English is skipped.
void unit_texts_are_looked_up_through_the_layers() {
    const languages::LanguagePack engine = pseudo_pack();
    const languages::LanguagePack player =
        pack_of("", "[CORAK] { name=player AK; name-from=AK; description=; }\n");
    const languages::LanguagePack mod =
        pack_of("", "[CORTHUD] { name=mod Thud; description=mod Light Plasma Kbot; }\n");
    std::vector<languages::PackLayer> layers(1);
    layers[0].word = "Pseudo";
    layers[0].before_data = {&mod};
    layers[0].after_data = {&player, &engine};
    languages::UnitTexts texts;
    texts.add("CORTHUD", "Pseudo", "data Thud", "data Light Plasma Kbot");
    texts.add("CORAK", "Pseudo", nullptr, "data Infantry Kbot");
    const auto unit = [](const char* unit_name, const char* name, const char* description) {
        oa::UnitDef def{};
        copy_text(def.unit_name, unit_name);
        copy_text(def.name, name);
        copy_text(def.description, description);
        return def;
    };
    const oa::UnitDef commander = unit("ARMCOM", "Commander", "Commander");
    const oa::UnitDef solar = unit("ARMSOLAR", "Solar Collector", "Produces Energy");
    const oa::UnitDef thud = unit("CORTHUD", "Thud", "Light Plasma Kbot");
    const oa::UnitDef ak = unit("CORAK", "AK", "Infantry Kbot");
    const oa::UnitDef renamed = unit("ARMCOM", "Decoy Commander", "Decoy Commander");
    languages::set_unit_texts(&texts, std::vector<std::string>{"Pseudo"});
    languages::set_unit_pack_layers(layers);
    OA_CHECK(languages::unit_display_name(thud) == "mod Thud");
    OA_CHECK(languages::unit_display_description(thud) == "mod Light Plasma Kbot");
    OA_CHECK(languages::unit_display_name(ak) == "player AK");
    // The player's pack leaves the description empty: the data's shows.
    OA_CHECK(languages::unit_display_description(ak) == "data Infantry Kbot");
    OA_CHECK(languages::unit_display_name(commander).find("Commander") != std::string_view::npos);
    OA_CHECK(languages::unit_display_name(commander) != "Commander");
    // The pack's name translates "Solar Panel": the type's own shows.
    OA_CHECK(languages::unit_display_name(solar) == "Solar Collector");
    OA_CHECK(languages::unit_display_description(solar) != "Produces Energy");
    OA_CHECK(languages::unit_display_name(renamed) == "Decoy Commander");
    OA_CHECK(languages::unit_display_name("CORAK", "AK") == "player AK");
    // English shows the types' own texts.
    languages::set_unit_texts(&texts, std::vector<std::string>{"English"});
    OA_CHECK(languages::unit_display_name(thud) == "Thud");
    languages::set_unit_pack_layers({});
    languages::set_unit_texts(nullptr, {});
    OA_CHECK(languages::unit_display_name(commander) == "Commander");
}

/// The English a unit text translates: the same, any when not given, or
/// the start of it when the game data's field cut the English short.
void unit_texts_check_the_english_they_translate() {
    OA_CHECK(languages::translates("", "Commander", 32));
    OA_CHECK(languages::translates("Commander", "Commander", 32));
    OA_CHECK(!languages::translates("Commander", "Decoy Commander", 32));
    const std::string cut(31, 'x');
    OA_CHECK(languages::translates(cut + "yz", cut, 32));
    OA_CHECK(!languages::translates(cut + "yz", cut, 0));
    OA_CHECK(!languages::translates("Commander", "Command", 32));
}

/// Missions' names: a mod's pack, the game data's own in the language, the
/// player's and the engine's packs; without any, nothing.
void mission_texts_are_looked_up_through_the_layers() {
    const languages::LanguagePack engine = pseudo_pack();
    std::vector<languages::PackLayer> layers(1);
    layers[0].word = "Pseudo";
    layers[0].after_data = {&engine};
    const std::vector<std::string> words{"Pseudo"};
    const char* name =
        languages::layered_mission_text(layers, words, "LIPAR PASS.OTA", "missionname", nullptr);
    OA_CHECK(
        name != nullptr && std::string_view(name).find("Lipar Pass") != std::string_view::npos
    );
    OA_CHECK(
        std::string_view(
            languages::layered_mission_text(
                layers, words, "Lipar Pass.ota", "missionname", "data name"
            )
        ) == "data name"
    );
    OA_CHECK(
        languages::layered_mission_text(layers, words, "Pursuit.ota", "missionname", nullptr) ==
        nullptr
    );
}

/// Captions over pictures: an earlier pack's caption wins whole, and one
/// with no keys turns the caption off.
void picture_captions_follow_the_layers() {
    const languages::LanguagePack engine = pseudo_pack();
    const languages::LanguagePack player = pack_of("", "", "[IGTITLES] { [PAUSED] { } }\n");
    std::vector<languages::PackLayer> layers(1);
    layers[0].word = "Pseudo";
    layers[0].after_data = {&engine};
    const std::vector<std::string> words{"Pseudo"};
    OA_CHECK(languages::layered_pictures(layers, words).find("igtitles/paused") != nullptr);
    layers[0].after_data = {&player, &engine};
    OA_CHECK(languages::layered_pictures(layers, words).find("igtitles/paused") == nullptr);
    OA_CHECK(languages::layered_pictures(layers, std::vector<std::string>{"German"}).all().empty());
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: oa-data-languages-pack-test <pseudo-language pack folder>\n";
        return 2;
    }
    pack_folder = argv[1];
    pseudo_pack_reads_whole();
    manifests_are_refused_by_rule();
    package_keys_are_read_and_checked();
    content_needs_are_read();
    game_texts_are_looked_up_through_the_layers();
    unit_texts_are_looked_up_through_the_layers();
    unit_texts_check_the_english_they_translate();
    mission_texts_are_looked_up_through_the_layers();
    picture_captions_follow_the_layers();
    return oa::test::check_exit_status();
}
