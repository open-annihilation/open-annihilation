// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The language the game shows its text in: chosen at start from 3.1c's
// command line, the player's setting and the operating system's preferred
// locales, and put in effect there and each time the setting changes. The
// game data's own texts follow it through 3.1c's keys (Translate.tdf, the
// units' <Language>Name and <Language>Description, the language folders);
// the engine's own words through the interface catalogue. Language packs
// add to both (oa/data/languages/language_pack.hpp): a mod's before the
// game data, the player's and the engine's after it. Nothing here reaches
// the simulation, a saved game or what a shared game sends.

#include "language_packs.hpp"
#include "language_state.hpp"
#include "oa/platform/system.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"

#include "oa/app/asset_files.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/defs/locale.hpp"
#include "oa/data/languages/translation.hpp"
#include "oa/platform/locale.hpp"
#include "oa/ui/engine_settings.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace oa::app {

namespace {

namespace languages = oa::data::languages;

/// The folder beside the game's other files that holds interface catalogue
/// files (oa/data/languages/interface_text.hpp), each a TDF file, and the
/// engine's language packs.
constexpr std::string_view kCatalogueFolder = engine_languages_folder;

/// A catalogue file's extension, matched without regard to case.
constexpr std::string_view kCatalogueExtension = ".tdf";

/// Tells whether a file name ends in the catalogue's extension.
///
/// @param name the file name
/// @return true for a .tdf file, in any case
bool catalogue_file(std::string_view name) {
    if (name.size() <= kCatalogueExtension.size())
        return false;
    const auto tail = name.substr(name.size() - kCatalogueExtension.size());
    return std::equal(tail.begin(), tail.end(), kCatalogueExtension.begin(), [](char a, char b) {
        return (a >= 'A' && a <= 'Z' ? static_cast<char>(a - 'A' + 'a') : a) == b;
    });
}

/// Returns the word a pack's texts are looked up by: the registry's word
/// for a language it knows by the pack's tag, else the manifest's.
///
/// @param manifest the pack's manifest
/// @return the word
std::string_view pack_word(const languages::PackManifest& manifest) {
    const languages::Language* known = languages::find_by_tag(manifest.tag);
    return known != nullptr && !known->game_name.empty() ? known->game_name
                                                         : std::string_view(manifest.word);
}

/// Tells whether two words are the same, ignoring the case of ASCII letters.
///
/// @param left a word
/// @param right a word
/// @return true when they match
bool same_word(std::string_view left, std::string_view right) {
    const languages::UnitTexts::NoCaseLess less{};
    return !less(left, right) && !less(right, left);
}

} // namespace

void Runtime::destroy_language_state(LanguageState* state) noexcept {
    // The interface's lookups must not outlive the tables they read.
    if (state != nullptr) {
        languages::set_unit_pack_layers({});
        languages::set_unit_texts(nullptr, {});
        languages::set_unit_text_sink(nullptr);
        languages::set_interface_language(nullptr, languages::english());
        languages::set_translation_hooks({});
    }
    delete state;
}

Runtime::LanguageState& Runtime::language_state() {
    if (!language_)
        language_.reset(new LanguageState{});
    return *language_;
}

void Runtime::start_language() {
    auto& state = language_state();
    state.system = &languages::preferred_language(oa::platform::locale::preferred_locales());
    // The loaders read every known language's unit texts, and the command
    // line's word when it names one no entry knows, as 3.1c reads it.
    state.sink_words.clear();
    for (const languages::Language& language : languages::known_languages())
        if (!language.game_name.empty())
            state.sink_words.emplace_back(language.game_name);
    const char* word = oa::app::command_line::launch_language(options_.launch);
    if (word != nullptr && languages::find_by_game_name(word) == nullptr)
        state.sink_words.emplace_back(word);
    state.sink_word_pointers.clear();
    for (const std::string& sink_word : state.sink_words)
        state.sink_word_pointers.push_back(sink_word.c_str());
    state.sink.context = &state;
    state.sink.languages = state.sink_word_pointers.data();
    state.sink.language_count = static_cast<uint32_t>(state.sink_word_pointers.size());
    state.sink.text = [](void* context,
                         const char* unit_name,
                         uint32_t language,
                         const char* name,
                         const char* description) {
        auto& owner = *static_cast<LanguageState*>(context);
        if (language < owner.sink_words.size())
            owner.unit_texts.add(unit_name, owner.sink_words[language], name, description);
    };
    languages::set_unit_text_sink(&state.sink);
    // Every screen and panel the game loads translates its texts, and looks
    // in its language's folders, through these.
    languages::TranslationHooks hooks{};
    hooks.context = this;
    hooks.translate = translation_hook;
    hooks.word = [](void* runtime) {
        return static_cast<const Runtime*>(runtime)->game_language();
    };
    hooks.mission_text = [](void* runtime,
                            const char* mission_file,
                            const char* key,
                            const char* data_text) -> const char* {
        const auto& owner = *static_cast<const Runtime*>(runtime);
        if (!owner.language_)
            return data_text;
        return languages::layered_mission_text(
            owner.language_->layers, owner.language_->words, mission_file, key, data_text
        );
    };
    hooks.language_file =
        [](void* runtime, const char* path, bool before_data) -> const std::string* {
        auto& owner = *static_cast<Runtime*>(runtime);
        if (!owner.language_)
            return nullptr;
        auto& language = *owner.language_;
        // A mod's packs before the game data, the player's and then the
        // engine's after it; each answers its own word's folders only.
        const auto before = {&language.mod_packs};
        const auto after = {&language.player_packs, &language.engine_packs};
        for (const auto* packs : before_data ? before : after)
            for (const auto& loaded : *packs) {
                const fs::path file = language_pack_file(*loaded, path);
                if (file.empty())
                    continue;
                std::string failure;
                if (auto bytes = read_pack_file(file, failure)) {
                    language.language_file = std::move(*bytes);
                    return &language.language_file;
                }
                std::cerr << "open-annihilation: " << path_to_utf8(file)
                          << " was not read: " << failure << '\n';
            }
        return nullptr;
    };
    languages::set_translation_hooks(hooks);
    read_interface_catalogue();
    // The language packs: the engine's, then the player's, then the mod's,
    // so that each one's words in the interface catalogue replace those
    // before it.
    if (const char* base = SDL_GetBasePath(); base != nullptr)
        read_language_packs(
            path_from_utf8(base) / path_from_utf8(kCatalogueFolder),
            state.engine_packs,
            &state.catalogue
        );
    if (!user_folder().empty())
        read_language_packs(
            user_folder() / path_from_utf8(player_languages_folder),
            state.player_packs,
            &state.catalogue
        );
    if (plays_mod())
        read_language_packs(
            played_mod_folder() / path_from_utf8(languages::mod_languages_folder),
            state.mod_packs,
            &state.catalogue
        );
    state.choice = oa::ui::engine_settings::stored_language(
        preference_values_, !options_.preferences_file.has_value()
    );
    apply_language();
}

void Runtime::read_interface_catalogue() {
    auto& state = language_state();
    const std::string base = oa::platform::program_directory();
    if (base.empty())
        return;
    const fs::path folder = path_from_utf8(base.c_str()) / path_from_utf8(kCatalogueFolder);
    std::error_code error;
    if (!fs::is_directory(folder, error))
        return;
    std::vector<fs::path> files;
    for (fs::directory_iterator entry(folder, error), end; !error && entry != end;
         entry.increment(error))
        if (entry->is_regular_file(error) && catalogue_file(path_to_utf8(entry->path().filename())))
            files.push_back(entry->path());
    // A later file's translation of a text replaces an earlier one's.
    std::sort(files.begin(), files.end());
    for (const fs::path& file : files) {
        std::ifstream input(file, std::ios::binary);
        std::string text;
        if (input)
            text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        std::string failure;
        if (!input || !state.catalogue.add(text, &failure))
            std::cerr << "open-annihilation: the interface catalogue " << path_to_utf8(file)
                      << " was not read" << (failure.empty() ? "" : ": ") << failure << '\n';
    }
}

void Runtime::set_language_choice(std::string_view choice) {
    auto& state = language_state();
    if (state.choice == choice)
        return;
    state.choice = std::string(choice);
    apply_language();
    // The modern fonts draw the new language's commonest characters ahead
    // of its first screen.
    warm_game_text();
    show_language_change();
}

void Runtime::show_language_change() {
    // Kept for later, they load again in the language as they next show.
    match_titles_ = {};
    talk_layout_.reset();
    match_talk_ = {};
    // The settings change the language over the main menu, alone of the
    // front end's screens, and in a match beside the in-game menu. A main
    // menu without its panel, not yet loaded or taken off, loads in the
    // language as it shows.
    if (screen_ == Screen::main_menu) {
        if (!resources_.layout.gadgets.empty())
            reload_main_menu_language();
        return;
    }
    if (screen_ != Screen::match || !match_ || match_finished_)
        return;
    // The in-game menu opens again in the language over the page it keeps
    // darkened below it; the page shows the language as the game resumes.
    if (ingame_menu_column_shown())
        show_match_pause_menu();
    else if (!match_paused_)
        apply_match_hud_for_selection();
}

void Runtime::apply_language() {
    auto& state = language_state();
    const char* word = oa::app::command_line::launch_language(options_.launch);
    const languages::Language* named =
        word != nullptr ? languages::find_by_game_name(word) : nullptr;
    // The registry's words are string literals, so each word ends in a NUL.
    if (named != nullptr) {
        // 3.1c's command line names a known language: it decides the run's.
        state.shown = named;
        state.words = languages::data_words(*named, {});
    } else if (word != nullptr) {
        // A word no entry knows: the game data's texts in it, as 3.1c reads
        // them, and the engine's own words in English.
        state.shown = &languages::english();
        state.words = languages::data_words(languages::english(), word);
    } else {
        state.shown = &languages::chosen_language(state.choice, *state.system);
        state.words = languages::data_words(*state.shown, {});
    }
    state.data_word = data_word_for(state.choice);
    // The game data's translation table and fonts, loaded again only for a
    // new word.
    if (!state.loaded || state.loaded_word != state.data_word) {
        state.loaded = true;
        state.loaded_word = state.data_word;
        load_translations(game_language());
        load_common_fonts();
        // A language's fallbacks' tables, tried after its own.
        state.fallback_tables.clear();
        const auto files = asset_files(assets_);
        const auto path =
            std::string(oa::data::defs::directory_name(oa::data::defs::DataDirectory::gamedata)) +
            "\\translate.tdf";
        for (std::size_t index = 1; index < state.words.size(); ++index) {
            auto& table = state.fallback_tables.emplace_back(
                std::make_unique<LanguageState::FallbackTable>()
            );
            static_cast<void>(oa::data::defs::load_locale_table(
                &files, &table->table, path.c_str(), state.words[index].c_str()
            ));
        }
    }
    // The packs of each word: a mod's before the game data, the player's
    // and the engine's after it.
    state.layers.clear();
    state.unicode_manifest = nullptr;
    for (const std::string& layer_word : state.words) {
        languages::PackLayer layer;
        layer.word = layer_word;
        const auto add =
            [&layer_word](const auto& packs, std::vector<const languages::LanguagePack*>& into) {
                for (const auto& loaded : packs)
                    if (same_word(pack_word(loaded->pack.manifest()), layer_word))
                        into.push_back(&loaded->pack);
            };
        add(state.mod_packs, layer.before_data);
        add(state.player_packs, layer.after_data);
        add(state.engine_packs, layer.after_data);
        if (layer.before_data.empty() && layer.after_data.empty())
            continue;
        // The language shown asks for chat in UTF-8 when one of its own
        // packs says so.
        if (state.unicode_manifest == nullptr && layer_word == state.words.front())
            for (const auto* packs : {&layer.before_data, &layer.after_data})
                for (const languages::LanguagePack* pack : *packs)
                    if (state.unicode_manifest == nullptr && pack->manifest().unicode)
                        state.unicode_manifest = &pack->manifest();
        state.layers.push_back(std::move(layer));
    }
    state.pictures = languages::layered_pictures(state.layers, state.words);
    languages::set_unit_texts(&state.unit_texts, state.words);
    languages::set_unit_pack_layers(state.layers);
    languages::set_interface_language(&state.catalogue, *state.shown);
}

const char* Runtime::game_translation(const char* text) const {
    if (text == nullptr)
        return nullptr;
    // The game data's own translation in the word at an index of the
    // language's words: its own table, then its fallbacks' tables.
    const auto data = [this, text](std::size_t index) -> const char* {
        const oa::data::defs::LocaleTable* table = nullptr;
        if (index == 0)
            table = &translations_.table;
        else if (language_ && index - 1 < language_->fallback_tables.size())
            table = &language_->fallback_tables[index - 1]->table;
        if (table == nullptr)
            return nullptr;
        const char* translated = oa::data::defs::locale_translate(table, text);
        return translated != text ? translated : nullptr;
    };
    if (!language_ || language_->words.empty())
        return data(0);
    return languages::layered_translation(language_->layers, language_->words, text, data);
}

const oa::data::languages::PictureCaptions& Runtime::language_pictures() const {
    static const languages::PictureCaptions none{};
    return language_ ? language_->pictures : none;
}

std::vector<std::filesystem::path> Runtime::language_pack_folders() const {
    std::vector<fs::path> folders;
    if (!language_)
        return folders;
    // Each word's packs in the order its text is looked up in them.
    for (const std::string& word : language_->words)
        for (const auto* packs :
             {&language_->mod_packs, &language_->player_packs, &language_->engine_packs})
            for (const auto& loaded : *packs)
                if (same_word(pack_word(loaded->pack.manifest()), word))
                    folders.push_back(loaded->folder);
    return folders;
}

const oa::data::languages::PackManifest* Runtime::language_unicode_chat() const {
    return language_ ? language_->unicode_manifest : nullptr;
}

std::vector<std::string> Runtime::unicode_chat_language_tags() const {
    std::vector<std::string> tags;
    for (const languages::Language& known : languages::known_languages())
        if (languages::turns_unicode_chat_on(known, false))
            tags.emplace_back(known.tag);
    if (!language_)
        return tags;
    for (const auto* packs :
         {&language_->mod_packs, &language_->player_packs, &language_->engine_packs})
        for (const auto& loaded : *packs) {
            const languages::PackManifest& manifest = loaded->pack.manifest();
            const languages::Language* known = languages::find_by_tag(manifest.tag);
            const std::string tag = known != nullptr ? std::string(known->tag) : manifest.tag;
            if (manifest.unicode && std::find(tags.begin(), tags.end(), tag) == tags.end())
                tags.push_back(tag);
        }
    return tags;
}

bool Runtime::language_needs_modern_fonts() const {
    return shown_language().needs == languages::TextNeeds::modern_fonts;
}

const char* Runtime::translation_hook(void* runtime, const char* text) {
    return static_cast<const Runtime*>(runtime)->game_translation(text);
}

const char* Runtime::data_word_for(std::string_view choice) const {
    if (const char* word = oa::app::command_line::launch_language(options_.launch);
        word != nullptr) {
        const languages::Language* named = languages::find_by_game_name(word);
        return named != nullptr ? named->game_name.data() : word;
    }
    return languages::chosen_language(choice, system_language()).game_name.data();
}

const char* Runtime::game_language() const {
    if (!language_ || language_->data_word == nullptr || language_->data_word[0] == '\0')
        return nullptr;
    return language_->data_word;
}

const oa::data::languages::Language& Runtime::shown_language() const {
    return language_ ? *language_->shown : languages::english();
}

const oa::data::languages::Language& Runtime::system_language() const {
    return language_ ? *language_->system : languages::english();
}

const oa::data::defs::UnitTextSink* Runtime::unit_text_sink() {
    return &language_state().sink;
}

} // namespace oa::app
