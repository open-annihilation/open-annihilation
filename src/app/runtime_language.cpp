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
#include "oa/app/content/downloads.hpp"
#include "oa/app/content/service.hpp"
#include "oa/app/content/settings.hpp"
#include "oa/data/catalogue/catalogue.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/registry/descriptor.hpp"
#include "oa/data/defs/locale.hpp"
#include "oa/data/languages/translation.hpp"
#include "oa/platform/locale.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/ui/engine_settings.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <tuple>
#include <fstream>
#include <iostream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
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
        languages::set_interface_catalogue_loader(nullptr, nullptr);
        languages::set_translation_hooks({});
    }
    delete state;
}

Runtime::LanguageState& Runtime::language_state() {
    if (!language_)
        language_.reset(new LanguageState{});
    return *language_;
}

namespace {

/// Drops the spaces and tabs at both ends of a preference.
///
/// @param text the text
/// @return the text inside them
std::string_view trimmed_preference(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
                             text.back() == '\n'))
        text.remove_suffix(1);
    return text;
}

/// Tells whether a preference names System default, without regard to case.
///
/// @param text the preference
/// @return true for "system"
bool names_system(std::string_view text) noexcept {
    if (text.size() != languages::system_choice.size())
        return false;
    for (std::size_t index = 0; index < text.size(); ++index) {
        char letter = text[index];
        if (letter >= 'A' && letter <= 'Z')
            letter = static_cast<char>(letter - 'A' + 'a');
        if (letter != languages::system_choice[index])
            return false;
    }
    return true;
}

} // namespace

void Runtime::load_language_packs() {
    auto& state = language_state();
    // The packs first, and not their interface texts yet: the catalogue's
    // own files come before any pack. A pack whose interface.tdf does not
    // read is still left out.
    state.engine_packs.clear();
    state.player_packs.clear();
    state.mod_packs.clear();
    if (const char* base = SDL_GetBasePath(); base != nullptr)
        read_language_packs(
            path_from_utf8(base) / path_from_utf8(kCatalogueFolder), state.engine_packs, nullptr
        );
    if (!user_folder().empty())
        read_language_packs(
            user_folder() / path_from_utf8(player_languages_folder), state.player_packs, nullptr
        );
    if (plays_mod())
        read_language_packs(
            played_mod_folder() / path_from_utf8(languages::mod_languages_folder),
            state.mod_packs,
            nullptr
        );
    // A mod's packs add text and no language. The player's pack wins over
    // the engine's when both hold one tag. A catalogue's pack stays
    // available until one of those lists installs it.
    register_available_languages();
    state.system = &languages::preferred_language(oa::platform::locale::preferred_locales());
    // English is the text the source writes, so the catalogue is not read
    // here. apply_language_now reads it when the language shown needs a
    // translation, and drops it again when the game shows English. A word
    // asked for in another language reads it then (ensure_interface_catalogue).
    state.catalogue = {};
    state.catalogue_sought = false;
}

void Runtime::start_language() {
    auto& state = language_state();
    // Before the catalogue is dropped below. A word in another language reads
    // it on that request; English does not. The state's deleter clears this.
    install_interface_catalogue_loader();
    load_language_packs();
    // The loaders read every live language's unit texts, one that is not
    // installed included, and the command line's word when it names one no
    // playable entry knows and the list does not already hold, as 3.1c
    // reads it.
    state.sink_words.clear();
    for (const languages::Language* language : languages::known_languages())
        if (!language->game_name.empty())
            state.sink_words.emplace_back(language->game_name);
    const char* word = oa::app::command_line::launch_language(options_.launch);
    if (word != nullptr && languages::find_by_game_name(word) == nullptr &&
        std::find(state.sink_words.begin(), state.sink_words.end(), word) == state.sink_words.end())
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
    state.choice = oa::ui::engine_settings::stored_language(
        preference_values_, !options_.preferences_file.has_value()
    );
    apply_language();
}

void Runtime::reload_language_packs() {
    // Nothing may keep a pointer into the packs this replaces: the unit
    // layers, the picture captions and the manifest a chat setting reads.
    languages::set_unit_pack_layers({});
    languages::set_interface_language(nullptr, languages::english());
    auto& state = language_state();
    state.layers.clear();
    state.unicode_manifest = nullptr;
    state.pictures.clear();
    // The side column's art is captioned again from the packs as it next
    // shows.
    match_hud_art_.reset();
    load_language_packs();
    apply_language();
    // A pack that arrived leaves the list of languages to download, and an
    // open dialog shows it among the installed languages.
    state.dialog_languages_filled = false;
    if (auto* dialog = engine_settings_dialog())
        std::ignore = refresh_settings_languages(*dialog);
    // The raw setting, not the choice read at start: a tag the registry did
    // not know then is still the player's choice. A command-line word decides
    // the run and is left as it is.
    if (oa::app::command_line::launch_language(options_.launch) == nullptr) {
        const auto found =
            preference_values_.find(std::string{oa::ui::engine_settings::key::language});
        if (found != preference_values_.end()) {
            const std::string_view raw = trimmed_preference(found->second);
            if (!names_system(raw)) {
                if (const languages::Language* language = languages::find_by_tag(raw);
                    language != nullptr && languages::playable(*language) &&
                    (state.shown == nullptr || state.shown->tag != language->tag))
                    set_language_choice(language->tag);
            }
        }
    }
    const std::size_t installed =
        state.engine_packs.size() + state.player_packs.size() + state.mod_packs.size();
    std::cerr << "open-annihilation: language packs read again: " << installed << " installed\n";
}

namespace {

namespace settings = oa::ui::engine_settings;
namespace catalogue = oa::data::catalogue;

/// Tells whether two language tags are the same tag.
///
/// ASCII letters match in either case, and '_' is '-'. The whole tag
/// matches, so "pt" is not "pt-BR".
///
/// @param left a tag
/// @param right a tag
/// @return true when they are one tag
bool same_language_tag(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        char a = left[index];
        char b = right[index];
        if (a == '_')
            a = '-';
        if (b == '_')
            b = '-';
        if (a >= 'A' && a <= 'Z')
            a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z')
            b = static_cast<char>(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

/// Tells whether the game can already show a language.
///
/// @param tag the tag
/// @return true when a playable entry has it
bool already_playable(std::string_view tag) noexcept {
    const languages::Language* known = languages::find_by_tag(tag);
    return known != nullptr && languages::playable(*known);
}

/// Returns how much of a download has arrived, from 0 to 100, rounded down.
///
/// @param done the bytes held
/// @param size the pack's size
/// @return the percent
uint8_t download_percent(uint64_t done, uint64_t size) noexcept {
    if (size == 0)
        return 0;
    if (done >= size)
        return 100;
    // A catalogue's size is at most 2^32, and done is below it, so this fits.
    const uint64_t percent = done * 100 / size;
    return static_cast<uint8_t>(std::min<uint64_t>(percent, 100));
}

/// Returns the field's state for one queued pack.
///
/// @param download where the queue says it stands
/// @return the state the Language field shows
settings::AvailableLanguage::State language_download_state(content::DownloadState download) {
    switch (download) {
    case content::DownloadState::waiting:
    case content::DownloadState::asking:
    case content::DownloadState::challenge:
    case content::DownloadState::paused_for_match:
        return settings::AvailableLanguage::State::waiting;
    case content::DownloadState::downloading:
    case content::DownloadState::verifying:
        return settings::AvailableLanguage::State::downloading;
    case content::DownloadState::handing_over:
    case content::DownloadState::installing:
    case content::DownloadState::done:
        return settings::AvailableLanguage::State::installing;
    case content::DownloadState::failed:
    case content::DownloadState::cancelled:
        return settings::AvailableLanguage::State::failed;
    }
    return settings::AvailableLanguage::State::offered;
}

/// Returns why a queued pack failed, as the hint can show it.
///
/// @param item the queued pack
/// @return the reason
std::string download_failure_text(const content::DownloadView& item) {
    if (item.failure != content::DownloadFailure::none)
        return content::failure_text(item.failure);
    if (!item.detail.empty())
        return item.detail;
    if (item.state == content::DownloadState::cancelled)
        return "the download was cancelled";
    return "the download failed";
}

/// Tells whether two available rows show the same thing.
///
/// @param left a row
/// @param right a row
/// @return true when the dialog would draw them alike
bool same_available_row(
    const settings::AvailableLanguage& left, const settings::AvailableLanguage& right
) {
    return left.tag == right.tag && left.endonym == right.endonym &&
           left.english_name == right.english_name && left.aside == right.aside &&
           left.endonym_drawable == right.endonym_drawable && left.choosable == right.choosable &&
           left.state == right.state && left.percent == right.percent &&
           left.failure == right.failure;
}

/// Tells whether a registry can download.
///
/// A registry the snapshot does not list can: a check's registry is one.
/// Downloads are off when the registry requires an install id and that id
/// is off.
///
/// @param snapshot the catalogues in effect
/// @param values the preference values
/// @param registry the registry id
/// @return false when its downloads are off
bool registry_downloads_on(
    const content::Snapshot& snapshot,
    const oa::platform::preferences::Values& values,
    std::string_view registry
) {
    for (const content::RegistryView& listed : snapshot.registries) {
        if (listed.registry.descriptor.id != registry)
            continue;
        const bool required =
            listed.registry.descriptor.install_id == oa::data::registry::InstallIdUse::required;
        if (required &&
            content::read_install_id(values, registry).state == content::InstallIdState::off)
            return false;
        return true;
    }
    return true;
}

/// The English name a catalogue entry shows when it names none.
///
/// @param tag the pack's tag
/// @param named the catalogue's English name; empty when it names none
/// @return the name
std::string catalogue_english_name(std::string_view tag, std::string_view named) {
    if (!named.empty())
        return std::string(named);
    if (const languages::Language* known = languages::find_by_tag(tag);
        known != nullptr && !known->english_name.empty())
        return std::string(known->english_name);
    return std::string(tag);
}

} // namespace

void Runtime::register_available_languages() {
    auto& state = language_state();
    std::vector<languages::LanguageEntry> entries;
    entries.reserve(state.available.size());
    for (const CatalogueLanguage& pack : state.available)
        entries.push_back(pack.entry);
    languages::set_pack_languages(
        installed_entries({&state.player_packs, &state.engine_packs}), entries
    );
}

void Runtime::refresh_available_languages() {
    auto& state = language_state();
    if (state.hold_available || !content_)
        return;
    const std::shared_ptr<const content::Snapshot> snapshot = content_service().snapshot();
    std::vector<CatalogueLanguage> offered;
    if (snapshot) {
        for (const content::Entry& listed : snapshot->entries) {
            if (listed.package == nullptr || listed.package->kind != catalogue::Kind::oalang)
                continue;
            const catalogue::Package& package = *listed.package;
            bool duplicate = false;
            for (const CatalogueLanguage& kept : offered)
                if (same_language_tag(kept.entry.tag, package.id))
                    duplicate = true;
            if (package.id.empty() || duplicate || already_playable(package.id))
                continue;
            CatalogueLanguage pack;
            pack.registry = listed.registry;
            pack.reviewed = listed.reviewed;
            pack.entry.tag = package.id;
            pack.entry.endonym = package.name;
            pack.entry.english_name = catalogue_english_name(package.id, package.english_name);
            pack.entry.word = package.word.empty() ? package.id : package.word;
            if (package.locales.empty())
                pack.entry.locales.push_back(package.id);
            else
                pack.entry.locales = package.locales;
            pack.entry.fallbacks = package.fallbacks;
            pack.entry.needs = package.needs == "game-fonts" ? languages::TextNeeds::game_fonts
                                                             : languages::TextNeeds::modern_fonts;
            pack.publisher = package.publisher;
            pack.coverage = package.coverage;
            pack.bytes = package.size;
            offered.push_back(std::move(pack));
        }
    }
    state.available = std::move(offered);
    state.catalogue_generation = content_service().generation();
    state.catalogue_seen = true;
    state.dialog_languages_filled = false;
    register_available_languages();
}

void Runtime::refresh_available_languages(std::span<const CatalogueLanguage> offered) {
    auto& state = language_state();
    state.available.assign(offered.begin(), offered.end());
    state.hold_available = true;
    state.catalogue_seen = true;
    state.dialog_languages_filled = false;
    register_available_languages();
    if (auto* dialog = engine_settings_dialog())
        std::ignore = refresh_settings_languages(*dialog);
}

void Runtime::release_available_languages() {
    auto& state = language_state();
    state.hold_available = false;
    state.catalogue_seen = false;
    refresh_available_languages();
}

bool Runtime::download_language(std::string_view tag, std::string& failure) {
    failure.clear();
    if (language_download_hook_ != nullptr)
        return language_download_hook_(language_download_hook_context_, tag, failure);
    const CatalogueLanguage* offered = nullptr;
    for (const CatalogueLanguage& pack : language_state().available) {
        if (!same_language_tag(pack.entry.tag, tag))
            continue;
        offered = &pack;
        break;
    }
    if (offered == nullptr || offered->registry.empty()) {
        failure = "the catalogue has no such package";
        return false;
    }
    std::string why;
    const auto queued = queue_download(
        offered->registry, offered->entry.tag, content::DownloadReason::install, &why
    );
    if (!queued) {
        failure = why.empty() ? std::string("the catalogue has no such package") : std::move(why);
        return false;
    }
    return true;
}

void Runtime::note_catalogue_languages() {
    if (!content_)
        return;
    auto& state = language_state();
    if (state.hold_available)
        return;
    if (!state.catalogue_seen || state.catalogue_generation != content_service().generation())
        refresh_available_languages();
}

bool Runtime::refresh_settings_languages(settings::Dialog& dialog) {
    if (!content_)
        return false;
    auto& state = language_state();
    if (!state.hold_available) {
        const uint64_t catalogues = content_service().generation();
        if (!state.catalogue_seen || state.catalogue_generation != catalogues)
            refresh_available_languages();
    }
    const uint64_t catalogues = content_service().generation();
    // Read before the items, so a queue that moves while they are copied is
    // filled again on the next frame.
    const uint64_t downloads = content_downloads().generation();
    if (state.dialog_languages_filled && state.dialog_downloads_generation == downloads &&
        (state.hold_available || state.catalogue_generation == catalogues))
        return false;

    const std::shared_ptr<const content::Snapshot> snapshot = content_service().snapshot();
    const std::vector<content::DownloadView> queued = content_downloads().view();
    const std::vector<settings::AvailableLanguage> earlier = dialog.available_languages;
    std::vector<settings::AvailableLanguage> rows;
    rows.reserve(state.available.size());
    for (const CatalogueLanguage& pack : state.available) {
        if (already_playable(pack.entry.tag))
            continue;
        settings::AvailableLanguage row;
        row.tag = pack.entry.tag;
        row.endonym = pack.entry.endonym;
        row.english_name = pack.entry.english_name;
        row.endonym_drawable = modern_fonts_draw(pack.entry.endonym);
        // A catalogue that has not been read yet still offers the pack. One
        // the snapshot does not list can be chosen: a check's registry.
        row.choosable = snapshot == nullptr ||
                        registry_downloads_on(*snapshot, preference_values_, pack.registry);
        row.aside = settings::language_download_aside(
            pack.reviewed, pack.publisher, pack.coverage, pack.bytes, row.choosable
        );
        const content::DownloadView* matched = nullptr;
        for (const content::DownloadView& item : queued) {
            if (item.target.registry == pack.registry &&
                same_language_tag(item.target.key, pack.entry.tag))
                matched = &item;
        }
        if (matched != nullptr) {
            row.state = language_download_state(matched->state);
            if (row.state == settings::AvailableLanguage::State::downloading)
                row.percent = download_percent(matched->done, matched->target.size);
            else if (row.state == settings::AvailableLanguage::State::failed)
                row.failure = download_failure_text(*matched);
        } else {
            for (const settings::AvailableLanguage& old : earlier) {
                if (!same_language_tag(old.tag, row.tag))
                    continue;
                row.state = old.state;
                row.percent = old.percent;
                row.failure = old.failure;
                break;
            }
        }
        rows.push_back(std::move(row));
    }
    bool changed = rows.size() != dialog.available_languages.size();
    for (std::size_t index = 0; !changed && index < rows.size(); ++index)
        changed = !same_available_row(rows[index], dialog.available_languages[index]);
    if (changed)
        dialog.available_languages = std::move(rows);
    state.dialog_downloads_generation = downloads;
    state.dialog_languages_filled = true;
    return changed;
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

void Runtime::ensure_interface_catalogue() {
    auto& state = language_state();
    if (state.catalogue.size() != 0 || state.catalogue_sought)
        return;
    state.catalogue_sought = true;
    read_interface_catalogue();
    add_pack_interface_texts(state.engine_packs, state.catalogue);
    add_pack_interface_texts(state.player_packs, state.catalogue);
    add_pack_interface_texts(state.mod_packs, state.catalogue);
    // The language shown stays. A lookup in another language reads this
    // catalogue without switching the interface to it.
    languages::set_interface_language(&state.catalogue, languages::interface_language());
}

void Runtime::install_interface_catalogue_loader() {
    languages::set_interface_catalogue_loader(
        [](void* context) { static_cast<Runtime*>(context)->ensure_interface_catalogue(); }, this
    );
}

void Runtime::set_language_choice(std::string_view choice) {
    auto& state = language_state();
    if (state.choice == choice)
        return;
    state.choice = std::string(choice);
    // The notice of the change stays on this thread, after the dialog.
    const auto apply_and_warm = [this] {
        apply_language();
        // The modern fonts lay out the new language's warm-up text ahead of
        // its first screen.
        warm_game_text();
    };
    if (long_work_running() || options_.headless_check || sdl_.window == nullptr ||
        sdl_.renderer == nullptr)
        apply_and_warm();
    else
        run_progress("Applying your changes", [apply_and_warm](LongWork&) { apply_and_warm(); });
    show_language_change();
}

void Runtime::show_language_change() {
    // Kept for later, they load again in the language as they next show,
    // the side column's art with its captions among them.
    match_titles_ = {};
    talk_layout_.reset();
    match_talk_ = {};
    match_hud_art_.reset();
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
    if (long_work_running() || options_.headless_check || sdl_.window == nullptr ||
        sdl_.renderer == nullptr) {
        apply_language_now();
        return;
    }
    run_progress("Applying your changes", [this](LongWork&) { apply_language_now(); });
}

void Runtime::apply_language_now() {
    auto& state = language_state();
    const char* word = oa::app::command_line::launch_language(options_.launch);
    const languages::Language* named =
        word != nullptr ? languages::find_by_game_name(word) : nullptr;
    // A language's word ends in a NUL and stays for the whole run.
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
    // The catalogue holds every shipped translation. English never reads it:
    // a missing catalogue shows the source text. Another language reads it
    // once, packs included, and a return to English drops it.
    const bool want_catalogue =
        state.shown != nullptr && state.shown->tag != languages::english().tag;
    if (!want_catalogue) {
        if (state.catalogue.size() != 0)
            state.catalogue = {};
        // The next word in another language reads it again. A sought flag
        // left set would answer that word from the empty catalogue.
        state.catalogue_sought = false;
    } else if (state.catalogue.size() == 0 && !state.catalogue_sought) {
        read_interface_catalogue();
        add_pack_interface_texts(state.engine_packs, state.catalogue);
        add_pack_interface_texts(state.player_packs, state.catalogue);
        add_pack_interface_texts(state.mod_packs, state.catalogue);
        state.catalogue_sought = true;
    }
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
    // Fonts of the packs of the words, in lookup order, and the first
    // warm-up. A path is taken once. Past most_pack_faces, the rest are
    // named in one line and left out.
    const auto loaded_of =
        [&state](const languages::LanguagePack* pack) -> const LoadedLanguagePack* {
        for (const auto* group : {&state.mod_packs, &state.player_packs, &state.engine_packs})
            for (const auto& loaded : *group)
                if (loaded != nullptr && &loaded->pack == pack)
                    return loaded.get();
        return nullptr;
    };
    std::vector<PackFontFile> fonts;
    std::string warmup;
    std::vector<std::string> left_out;
    const auto consider = [&](const languages::LanguagePack* pack) {
        const LoadedLanguagePack* loaded = loaded_of(pack);
        if (loaded == nullptr)
            return;
        if (warmup.empty() && !loaded->warmup.empty())
            warmup = loaded->warmup;
        for (const PackFontFile& font : pack_fonts(*loaded)) {
            const bool held =
                std::any_of(fonts.begin(), fonts.end(), [&font](const PackFontFile& have) {
                    return have.file == font.file;
                });
            if (held)
                continue;
            if (fonts.size() >= oa::platform::text_font::most_pack_faces) {
                left_out.push_back(path_to_utf8(font.file));
                continue;
            }
            fonts.push_back(font);
        }
    };
    for (const languages::PackLayer& layer : state.layers) {
        for (const languages::LanguagePack* pack : layer.before_data)
            consider(pack);
        for (const languages::LanguagePack* pack : layer.after_data)
            consider(pack);
    }
    if (!left_out.empty()) {
        std::cerr << "open-annihilation: language fonts left out:";
        for (const std::string& path : left_out)
            std::cerr << ' ' << path;
        std::cerr << '\n';
    }
    const bool fonts_changed = fonts.size() != state.fonts.size() ||
                               !std::equal(
                                   fonts.begin(),
                                   fonts.end(),
                                   state.fonts.begin(),
                                   [](const PackFontFile& left, const PackFontFile& right) {
                                       return left.file == right.file && left.role == right.role;
                                   }
                               );
    if (fonts_changed || warmup != state.warmup) {
        state.fonts = std::move(fonts);
        state.warmup = std::move(warmup);
        ++state.fonts_generation;
    }
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

uint64_t Runtime::language_fonts_generation() const noexcept {
    return language_ ? language_->fonts_generation : 0;
}

std::string_view Runtime::language_warmup() const {
    return language_ ? std::string_view(language_->warmup) : std::string_view{};
}

void Runtime::use_language_fonts(oa::platform::text_font::FontStack& stack) const {
    stack.remove_pack_faces();
    if (language_ == nullptr)
        return;
    for (const PackFontFile& font : language_->fonts) {
        if (!stack.add_face(font.file, font.role))
            std::cerr << "open-annihilation: the language font " << path_to_utf8(font.file)
                      << " did not open\n";
    }
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
    for (const languages::Language* known : languages::known_languages())
        if (languages::playable(*known) && languages::turns_unicode_chat_on(*known, false))
            tags.emplace_back(known->tag);
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
