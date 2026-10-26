// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Library on the OA layer: its feed from the content service, the
// installed packages and their updates its worker finds, every other mod
// folder the game can play, and the download queue; the worker itself; the
// Library as a screen of the layer (named library), its input and the
// actions it asks for; and its opening, from the main menu's OA button, from
// Settings › Language, from Settings › Downloads' registries link and by the
// runtime.

#include "engine_settings_menu_host.hpp"
#include "engine_settings_state.hpp"
#include "language_state.hpp"
#include "library_host.hpp"
#include "map_packs.hpp"
#include "mod_install_state.hpp"
#include "oa_layer.hpp"
#include "package_paths.hpp"

#include "oa/app/brain_seats.hpp"
#include "oa/app/content/downloads.hpp"
#include "oa/app/content/pictures.hpp"
#include "oa/app/content/service.hpp"
#include "oa/app/content/settings.hpp"
#include "oa/app/content/updates.hpp"
#include "oa/app/content_copy.hpp"
#include "oa/app/demo_installer.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/mod_summary.hpp"
#include "oa/app/package_install.hpp"
#include "oa/app/package_install/oamod.hpp"
#include "oa/app/package_install/inbox.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/app/window_icon.hpp"
#include "oa/base/signing/ed25519.hpp"
#include "oa/data/pe_icon.hpp"
#include "oa/base/threads.hpp"
#include "oa/data/catalogue/catalogue.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/data/content_bundles/content_choice.hpp"
#include "oa/data/languages.hpp"
#include "oa/platform/system.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/registry/descriptor.hpp"
#include "oa/data/registry/trust.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/data/brain_library/library.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/sim/brains/check.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/url.hpp"
#include "oa/platform/files.hpp"
#include "oa/ui/map_previews/map_previews.hpp"
#include "oa/netgame/http/client.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/library/game_icon.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/mod_questions.hpp"
#include "oa/ui/library/prerequisites.hpp"
#include "oa/ui/library/screen.hpp"
#include "oa/ui/library/text.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <system_error>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

namespace settings = oa::ui::engine_settings;
namespace kit = oa::ui::kit;
namespace library = oa::ui::library;
namespace mutators = oa::data::mutators;

namespace {

/// The sound the Library plays as it opens: the OA button's.
constexpr std::string_view kOpenSound = "Options";
/// The sound CLOSE plays: the settings' Cancel.
constexpr std::string_view kCloseSound = "Previous";
/// The settings' screen's name on the OA layer.
constexpr std::string_view kSettingsScreen = "settings";

/// The words an Active tab map line's automation name starts with.
constexpr std::string_view kActiveMapPrefix = "library.active.map.";
/// The key of a mod folder whose name gives no word: a folder at a root.
constexpr std::string_view kOwnModWord = "mod";

/// The author's name a mod's or a map pack's manifest gives when nobody is
/// known (docs/mods/oamod-standard.md): the details name no author for it.
constexpr std::string_view kUnknownAuthor = "unknown";
/// The modifier that holds the Library's Find: Cmd on macOS, Ctrl elsewhere.
#ifdef SDL_PLATFORM_MACOS
constexpr uint16_t kCommandModifier = SDL_KMOD_GUI;
#else
constexpr uint16_t kCommandModifier = SDL_KMOD_CTRL;
#endif

/// Returns one of the Library's words in the language shown.
///
/// @param english the word, in English as the Library's text.hpp writes it
/// @return its translation, or the English
std::string_view shown_word(void* /*context*/, std::string_view english) {
    return oa::data::languages::interface_text(english);
}

/// Returns the Library's kind for a catalogue's kind.
///
/// @param kind the catalogue's kind
/// @return the Library's
library::Kind kind_of(data::catalogue::Kind kind) noexcept {
    switch (kind) {
    case data::catalogue::Kind::oamod:
        return library::Kind::mod;
    case data::catalogue::Kind::oamap:
        return library::Kind::map_pack;
    case data::catalogue::Kind::oalang:
        return library::Kind::language;
    case data::catalogue::Kind::expansion:
        // The caller lists an expansion on downloadable_expansions and does
        // not build a Library row for it.
        return library::Kind::mod;
    case data::catalogue::Kind::oamut:
        return library::Kind::mutator;
    case data::catalogue::Kind::oabrain:
        return library::Kind::brain;
    }
    return library::Kind::mod;
}

/// Returns the Library's kind for one of the installer's kinds.
///
/// @param kind the installer's kind; may be null
/// @return the Library's; nothing for a kind the Library does not list
std::optional<library::Kind> kind_of(const package_install::PackageKind* kind) {
    if (kind == nullptr)
        return std::nullopt;
    if (kind->name == "oamod")
        return library::Kind::mod;
    if (kind->name == "oamap")
        return library::Kind::map_pack;
    if (kind->name == "oalang")
        return library::Kind::language;
    if (kind->name == "oamut")
        return library::Kind::mutator;
    if (kind->name == "oabrain")
        return library::Kind::brain;
    return std::nullopt;
}

/// Tells whether a queued download has ended: installed, failed or cancelled.
///
/// @param state the queue's state
/// @return true once it has
bool ended(content::DownloadState state) noexcept {
    return state == content::DownloadState::done || state == content::DownloadState::failed ||
           state == content::DownloadState::cancelled;
}

/// Returns whether an update changes the rules, as the Library says it.
///
/// @param change what the updates logic found
/// @return the Library's
library::UpdateNote::Rules rules_of(content::RulesChange change) noexcept {
    switch (change) {
    case content::RulesChange::same:
        return library::UpdateNote::Rules::same;
    case content::RulesChange::changes:
        return library::UpdateNote::Rules::changes;
    case content::RulesChange::unknown:
        return library::UpdateNote::Rules::unknown;
    }
    return library::UpdateNote::Rules::unknown;
}

/// Returns why an update cannot be installed, as the Library says it.
///
/// @param blocked what the updates logic found
/// @return the Library's
library::UpdateNote::Blocked blocked_of(content::Blocked blocked) noexcept {
    switch (blocked) {
    case content::Blocked::none:
        return library::UpdateNote::Blocked::none;
    case content::Blocked::engine:
        return library::UpdateNote::Blocked::engine;
    case content::Blocked::hacks:
        return library::UpdateNote::Blocked::hacks;
    case content::Blocked::base:
        return library::UpdateNote::Blocked::base;
    }
    return library::UpdateNote::Blocked::none;
}

/// Returns a badge the Library draws from a decoded picture.
///
/// @param picture the decoded picture
/// @return the badge; nothing for an empty picture
std::optional<library::Badge> badge_of(const ModBadge& picture) {
    if (picture.width == 0 || picture.height == 0 || picture.pixels.empty())
        return std::nullopt;
    return library::Badge{picture.pixels, picture.width, picture.height};
}

/// Returns a byte as a lower-case ASCII letter, and every other byte as it is.
///
/// @param byte the byte
/// @return the letter
char ascii_lower(unsigned char byte) noexcept {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return static_cast<char>(byte);
}

/// Returns text with its ASCII letters in lower case.
///
/// @param text the text
/// @return the text
std::string ascii_lowered(std::string_view text) {
    std::string lowered(text);
    for (char& byte : lowered)
        byte = ascii_lower(static_cast<unsigned char>(byte));
    return lowered;
}

/// A program file's extension, including the dot, compared without regard to case.
constexpr std::string_view program_extension = ".exe";
/// The description the game's own program gives of itself.
constexpr std::string_view game_description = "Total Annihilation";
/// The description a demo's program may give of itself.
constexpr std::string_view demo_description = "Total Annihilation Demo";
/// The most program files in the game folder that are looked at.
constexpr std::size_t most_program_files = 32;

/// Reports whether a file's extension is that of a program, without regard to case.
///
/// @param extension the extension, including its dot, as the path gives it
/// @return true when it is a program's extension
bool is_program_extension(std::string_view extension) {
    return ascii_lowered(extension) == program_extension;
}

/// Reads a program file. A file longer than the icon reader accepts, or one
/// that cannot be read, is left out.
///
/// @param file the file
/// @return its bytes; nothing when it is skipped
std::optional<std::vector<uint8_t>> program_bytes(const fs::path& file) {
    std::error_code error;
    const auto size = fs::file_size(file, error);
    if (error || size > oa::data::pe_icon::most_file_bytes)
        return std::nullopt;
    std::ifstream input{file, std::ios::binary};
    if (!input)
        return std::nullopt;
    std::vector<uint8_t> bytes(static_cast<std::size_t>(size));
    if (size > 0) {
        input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
        if (input.gcount() != static_cast<std::streamsize>(size))
            return std::nullopt;
    }
    return bytes;
}

/// Reads a program file's icon. Nothing here is written anywhere.
///
/// @param bytes the file
/// @return the icon; nothing when it cannot be read
std::optional<library::Badge> icon_of_program(std::span<const uint8_t> bytes) {
    const std::optional<oa::data::pe_icon::Icon> icon = oa::data::pe_icon::read_program_icon(bytes);
    const uint64_t pixel_bytes = icon ? uint64_t{icon->width} * icon->height * 4U : 0U;
    if (!icon || icon->width == 0 || icon->height == 0 || icon->rgba.size() != pixel_bytes)
        return std::nullopt;
    return library::Badge{icon->rgba, icon->width, icon->height};
}

/// Returns the icon of the only program in `files` whose description is
/// exactly `description`.
///
/// @param files the programs
/// @param description the description
/// @return the icon; nothing when none match, more than one matches, or the icon cannot be read
std::optional<library::Badge>
icon_described(std::span<const std::span<const uint8_t>> files, std::string_view description) {
    const std::optional<std::size_t> program =
        oa::data::pe_icon::only_described(files, description);
    if (!program)
        return std::nullopt;
    return icon_of_program(files[*program]);
}

/// Returns the base game's icon from the game folder in use.
///
/// The program is the one regular file directly in the folder, among at most
/// 32 whose extension is a program's, whose FileDescription is exactly
/// "Total Annihilation", or, failing that, "Total Annihilation Demo". A file
/// longer than the reader accepts is skipped. None, or more than one of a
/// description, uses the icon the Library keeps, which Total Annihilation
/// and the demo share. The folder is read once. A folder whose program icon
/// could not be read is remembered too, and one line is logged the time it
/// is first found to have none. The icon stays in memory.
///
/// @param folder the game folder
/// @return the icon. The letters show only when this picture cannot be drawn
std::optional<library::Badge> base_game_icon(const fs::path& folder) {
    std::error_code error;
    const fs::path absolute = fs::absolute(folder, error);
    const std::string key = path_to_utf8((error ? folder : absolute).lexically_normal());

    struct Held {
        std::string key{};
        std::optional<library::Badge> badge{};
        bool ready{false};
    };

    static Held held;
    if (held.ready && held.key == key)
        return held.badge;

    std::vector<std::vector<uint8_t>> files;
    std::size_t looked = 0;
    for (fs::directory_iterator entry{folder, error}, end;
         !error && entry != end && looked < most_program_files;
         entry.increment(error)) {
        std::error_code kind;
        if (!entry->is_regular_file(kind) || kind)
            continue;
        if (!is_program_extension(path_to_utf8(entry->path().extension())))
            continue;
        ++looked;
        if (std::optional<std::vector<uint8_t>> bytes = program_bytes(entry->path()))
            files.push_back(std::move(*bytes));
    }
    std::vector<std::span<const uint8_t>> views;
    views.reserve(files.size());
    for (const std::vector<uint8_t>& file : files)
        views.emplace_back(file);
    std::optional<library::Badge> badge = icon_described(views, game_description);
    if (!badge)
        badge = icon_described(views, demo_description);
    const bool from_program = badge.has_value();
    if (!badge)
        badge = library::bundled_game_icon();
    held.key = key;
    held.badge = std::move(badge);
    held.ready = true;
    if (!from_program)
        std::cerr << "open-annihilation: library: the base game's icon could not be read\n";
    return held.badge;
}

/// Returns the seconds since 1970 by the system's clock.
///
/// @return the seconds
int64_t seconds_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch()
    )
        .count();
}

/// Returns the registry of a snapshot that has an id: the built-in one,
/// never an added registry that conflicts with it.
///
/// @param snapshot the content service's snapshot
/// @param registry the registry's id
/// @return the registry; null when the snapshot has none of that id
const content::RegistryView*
registry_view(const content::Snapshot& snapshot, std::string_view registry) {
    for (const content::RegistryView& view : snapshot.registries)
        if (!view.registry.conflicting && view.registry.descriptor.id == registry)
            return &view;
    return nullptr;
}

/// Reads a mutator's icon and decodes it as a mod badge.
///
/// A file larger than max_mod_badge_bytes, an empty file, or one that cannot
/// be read gives no badge. The bytes are decoded with read_mod_badge.
///
/// @param path the icon file
/// @return the badge; empty when there is none
ModBadge read_limited_badge(const std::filesystem::path& path) {
    std::FILE* file = oa::platform::open_file(path, "rb");
    if (file == nullptr)
        return {};
    std::vector<uint8_t> bytes;
    std::array<uint8_t, 4096> chunk{};
    bool failed = false;
    while (!failed) {
        const std::size_t read = std::fread(chunk.data(), 1, chunk.size(), file);
        if (read > 0) {
            if (bytes.size() + read > max_mod_badge_bytes) {
                failed = true;
                break;
            }
            bytes.insert(
                bytes.end(), chunk.begin(), chunk.begin() + static_cast<std::ptrdiff_t>(read)
            );
        }
        if (read < chunk.size()) {
            if (std::ferror(file) != 0)
                failed = true;
            break;
        }
    }
    std::fclose(file);
    if (failed || bytes.empty())
        return {};
    return read_mod_badge(bytes);
}

/// Finds the installed mutator whose folder is a package's folder.
///
/// @param listing the Mutators folder
/// @param folder the package's folder
/// @return the mutator, or null
const data::mutator_library::InstalledMutator*
mutator_at(const data::mutator_library::Listing& listing, const std::filesystem::path& folder) {
    for (const data::mutator_library::InstalledMutator& mutator : listing.mutators) {
        if (mutator.folder == folder)
            return &mutator;
        std::error_code error;
        if (std::filesystem::equivalent(mutator.folder, folder, error))
            return &mutator;
    }
    return nullptr;
}

/// Finds the installed brain whose folder is a package's folder.
///
/// @param listing the Brains folder
/// @param folder the package's folder
/// @return the brain, or null
const data::brain_library::InstalledBrain*
brain_at(const data::brain_library::Listing& listing, const std::filesystem::path& folder) {
    for (const data::brain_library::InstalledBrain& brain : listing.brains) {
        if (brain.folder == folder)
            return &brain;
        std::error_code error;
        if (std::filesystem::equivalent(brain.folder, folder, error))
            return &brain;
    }
    return nullptr;
}

/// Fills the names a player reads on a brain's works-with mods. `*` and the
/// base game stay unnamed. Installed copies are tried before listings.
///
/// @param[in,out] inputs the Library's inputs, listings and installed filled
void name_brain_facts(library::Inputs& inputs) {
    const auto mod_name = [&](std::string_view id) -> std::string {
        for (const library::Installed& item : inputs.installed)
            if (item.kind == library::Kind::mod && item.key == id)
                return item.name;
        for (const library::Listing& listing : inputs.listings)
            if (listing.kind == library::Kind::mod && listing.key == id)
                return listing.name;
        return {};
    };
    const auto name_one = [&](library::BrainFacts& facts) {
        for (library::WorksWith& item : facts.works_with) {
            if (!item.name.empty() || item.id == "*" || item.id == library::supported_base)
                continue;
            item.name = mod_name(item.id);
        }
    };
    for (library::Listing& listing : inputs.listings)
        if (listing.brain)
            name_one(*listing.brain);
    for (library::Installed& installed : inputs.installed)
        if (installed.brain)
            name_one(*installed.brain);
}

/// Fills the names a player reads on mutator facts: a prerequisite from a
/// mutator of that id, a works-with mod from a mod of that id. `*` and the
/// base game stay unnamed. Installed copies are tried before listings.
///
/// @param[in,out] inputs the Library's inputs, listings and installed filled
void name_mutator_facts(library::Inputs& inputs) {
    const auto mutator_name = [&](std::string_view id) -> std::string {
        for (const library::Installed& item : inputs.installed)
            if (item.kind == library::Kind::mutator && item.key == id)
                return item.name;
        for (const library::Listing& listing : inputs.listings)
            if (listing.kind == library::Kind::mutator && listing.key == id)
                return listing.name;
        return {};
    };
    const auto mod_name = [&](std::string_view id) -> std::string {
        for (const library::Installed& item : inputs.installed)
            if (item.kind == library::Kind::mod && item.key == id)
                return item.name;
        for (const library::Listing& listing : inputs.listings)
            if (listing.kind == library::Kind::mod && listing.key == id)
                return listing.name;
        return {};
    };
    const auto name_one = [&](library::MutatorFacts& facts) {
        for (library::Prerequisite& need : facts.needs)
            if (need.name.empty())
                need.name = mutator_name(need.id);
        for (library::WorksWith& item : facts.works_with) {
            if (!item.name.empty() || item.id == "*" || item.id == library::supported_base)
                continue;
            item.name = mod_name(item.id);
        }
    };
    for (library::Listing& listing : inputs.listings)
        if (listing.mutator)
            name_one(*listing.mutator);
    for (library::Installed& installed : inputs.installed)
        if (installed.mutator)
            name_one(*installed.mutator);
}

/// Searches for the installed packages and their updates on the worker:
/// lists the player's folder, matches each package to the catalogues and
/// adopts each file install a catalogue lists, finds the updates, and reads
/// each installed mod's own picture and each installed mutator's and
/// brain's icon.
///
/// @param work the worker's state, whose results it fills under its lock
/// @param snapshot the catalogues in effect
/// @param folder the player's folder
void search_updates(
    LibraryWork& work, const content::Snapshot& snapshot, const std::filesystem::path& folder
) {
    std::vector<content::InstalledPackage> installed;
    std::vector<content::Match> matches;
    std::vector<content::Update> updates;
    std::vector<ModBadge> pictures;
    std::vector<std::optional<library::MutatorFacts>> mutator_facts;
    std::vector<std::optional<library::BrainFacts>> brain_facts;
    data::mutator_library::Listing mutators;
    data::brain_library::Listing brains;
    try {
        if (!folder.empty())
            installed = content::list_installed(folder);
        if (!folder.empty())
            mutators = data::mutator_library::list_installed(
                folder / std::string(data::mutator_library::mutators_folder_name)
            );
        if (!folder.empty())
            brains = data::brain_library::list_installed(
                folder / std::string(data::brain_library::brains_folder_name)
            );
        // A folder with no package SHA-256 is read here, off the main thread,
        // so the rebuild after this search finds its digest kept.
        for (const data::mutator_library::InstalledMutator& mutator : mutators.mutators) {
            if (mutator.package_sha256.empty())
                std::ignore = work.mutator_digests.digest(mutator.folder);
        }
        for (const content::InstalledPackage& package : installed) {
            content::Match match = content::match_installed(package, snapshot);
            if (match.provenance == content::Provenance::matched) {
                std::string error;
                if (!content::adopt_match(package, match, &error) && !error.empty())
                    std::cerr << "open-annihilation: library: " << error << '\n';
            }
            matches.push_back(std::move(match));
            const std::optional<library::Kind> kind = kind_of(package.kind);
            ModBadge badge;
            std::optional<library::MutatorFacts> facts;
            std::optional<library::BrainFacts> one_brain;
            if (kind == library::Kind::mod)
                badge = read_mod_summary(package.folder).badge;
            else if (kind == library::Kind::mutator) {
                if (const data::mutator_library::InstalledMutator* mutator =
                        mutator_at(mutators, package.folder)) {
                    facts = facts_of(mutator->manifest);
                    if (!mutator->manifest.icon.empty())
                        badge = read_limited_badge(mutator->folder / mutator->manifest.icon);
                }
            } else if (kind == library::Kind::brain) {
                if (const data::brain_library::InstalledBrain* found =
                        brain_at(brains, package.folder)) {
                    one_brain = brain_facts_of(found->manifest, found->network_bytes);
                    if (!found->manifest.icon.empty())
                        badge = read_limited_badge(found->folder / found->manifest.icon);
                }
            }
            pictures.push_back(std::move(badge));
            mutator_facts.push_back(std::move(facts));
            brain_facts.push_back(std::move(one_brain));
        }
        updates = content::find_updates(installed, snapshot, content::this_engine_rules());
    } catch (const std::exception& error) {
        std::cerr << "open-annihilation: library: the search for updates failed: " << error.what()
                  << '\n';
    }
    base::threads::LockGuard guard(work.mutex);
    // The log says what a search found when it differs from the last one.
    if (work.searches == 0 || installed.size() != work.installed.size() ||
        updates.size() != work.updates.size())
        std::cerr << "open-annihilation: library: " << installed.size() << " installed, "
                  << updates.size() << (updates.size() == 1 ? " update\n" : " updates\n");
    work.installed = std::move(installed);
    work.matches = std::move(matches);
    work.updates = std::move(updates);
    work.installed_badges = std::move(pictures);
    work.installed_mutators = std::move(mutator_facts);
    work.mutator_listing = std::move(mutators);
    work.installed_brains = std::move(brain_facts);
    work.brain_listing = std::move(brains);
    ++work.searches;
}

/// The Library's worker: waits for a search or for pictures, and does each,
/// until the host stops it. Its HTTP client is its own, and carries no
/// install ID. It does not read a registry's download counts, and it does
/// not open `<data>/content/counts`.
///
/// @param argument the worker's state (LibraryWork)
void library_worker(void* argument) {
    auto& work = *static_cast<LibraryWork*>(argument);
    netgame::http::Client client;
    for (;;) {
        bool search = false;
        bool pictures = false;
        bool match = false;
        std::optional<MutatorCheck> check;
        std::shared_ptr<const content::Snapshot> snapshot;
        std::filesystem::path folder;
        {
            base::threads::LockGuard guard(work.mutex);
            while (!work.stop && !work.updates_due && !(work.pictures_due && !work.match_running) &&
                   !work.check_due)
                work.wake.wait(work.mutex);
            if (work.stop)
                return;
            search = work.updates_due;
            work.updates_due = false;
            snapshot = work.snapshot;
            folder = work.user_folder;
            match = work.match_running;
            pictures = work.pictures_due && !match;
            if (pictures)
                work.pictures_due = false;
            if (work.check_due) {
                work.check_due = false;
                check = std::move(work.check_job);
                work.check_job.reset();
            }
        }
        if (search && snapshot)
            search_updates(work, *snapshot, folder);
        if (pictures && work.pictures) {
            try {
                work.pictures->fetch_waiting(client, match);
            } catch (const std::exception& error) {
                std::cerr << "open-annihilation: library: a picture could not be read: "
                          << error.what() << '\n';
            }
        }
        if (check) {
            MutatorCheckOutcome outcome;
            outcome.id = check->id;
            outcome.kind = check->kind;
            const bool stopped = match || work.check_cancel.load();
            outcome.key = check->key;
            outcome.brain_key = check->brain_key;
            const bool brain = check->kind == CheckKind::brain;
            const bool ready =
                brain ? static_cast<bool>(check->brain_work) : static_cast<bool>(check->work);
            if (stopped || !ready) {
                outcome.cancelled = true;
            } else {
                try {
                    if (brain) {
                        outcome.brain = check->brain_work(work.check_cancel);
                        outcome.cancelled = !outcome.brain.has_value() || work.check_cancel.load();
                        if (outcome.cancelled)
                            outcome.brain.reset();
                    } else {
                        outcome.record = check->work(work.check_cancel);
                        outcome.cancelled = !outcome.record.has_value() || work.check_cancel.load();
                        if (outcome.cancelled)
                            outcome.record.reset();
                    }
                } catch (const std::exception& error) {
                    std::cerr << "open-annihilation: library: the check failed: " << error.what()
                              << '\n';
                    outcome.cancelled = true;
                } catch (...) {
                    std::cerr << "open-annihilation: library: the check failed\n";
                    outcome.cancelled = true;
                }
            }
            base::threads::LockGuard guard(work.mutex);
            work.check_outcome = std::move(outcome);
        }
    }
}

/// Returns the key a decoded minimap is kept under for an installed map.
///
/// A preview picture and a terrain file are different keys, so one map can
/// keep either.
///
/// @param map the installed map
/// @return the key
std::string installed_picture_key(const PackMap& map) {
    if (!map.preview.empty())
        return "png:" + map.preview.generic_string();
    return "tnt:" + map.folder.generic_string() + "/" + map.stem;
}

/// Returns the terrain file an installed map's minimap is read from.
///
/// A file in the manifest whose name is the stem plus ".tnt", without regard
/// to case, wins. Otherwise the file is maps/<stem>.tnt in the pack folder.
///
/// @param map the installed map
/// @return the file, which need not exist
std::filesystem::path terrain_file(const PackMap& map) {
    const std::string wanted = ascii_lowered(map.stem + ".tnt");
    for (const std::string& listed : map.files) {
        const std::filesystem::path path{listed};
        if (ascii_lowered(path.filename().string()) != wanted)
            continue;
        if (path.is_absolute())
            return path;
        return map.folder / path;
    }
    return map.folder / "maps" / (map.stem + ".tnt");
}

} // namespace

/// Minimaps decoded for the Library, and the bytes a catalogue picture is
/// waiting to be decoded from. The cache is used from the main thread only.
struct MapPreviewState {
    oa::ui::map_previews::PreviewCache cache{};
    std::optional<oa::PaletteBytes>
        palette{};        ///< the game palette; empty when it could not be read
    bool palette_tried{}; ///< the palette was looked up once
    /// The one terrain file the next base decode reads. The cache asks for
    /// "maps/<stem>.tnt"; the reader ignores that and reads this file.
    std::filesystem::path terrain{};
    /// Catalogue picture bytes taken from the worker and not yet decoded, by address.
    std::map<std::string, std::vector<uint8_t>> online_bytes{};
    /// Decoded minimaps, by installed_picture_key or by the catalogue address.
    std::map<std::string, library::MapPicture> pictures{};
    uint64_t generation{}; ///< moves on when a minimap is stored
};

namespace {

/// Copies one range of the terrain file the preview state names.
///
/// @param context the preview state
/// @param path unused: the cache names a file inside the game data
/// @param offset where the range starts, in bytes
/// @param out where it is copied
/// @param size its bytes
/// @return false when fewer are there
bool preview_read_map(
    void* context, const char* /*path*/, uint32_t offset, void* out, uint32_t size
) {
    auto* state = static_cast<MapPreviewState*>(context);
    if (state == nullptr || out == nullptr || size == 0 || state->terrain.empty())
        return false;
    std::ifstream input{state->terrain, std::ios::binary};
    if (!input)
        return false;
    input.seekg(static_cast<std::streamoff>(offset), std::ios::beg);
    if (!input)
        return false;
    input.read(static_cast<char*>(out), static_cast<std::streamsize>(size));
    return input.gcount() == static_cast<std::streamsize>(size);
}

/// Reads an installed map's preview picture, at most the picture cache's limit.
///
/// @param file the picture
/// @param[out] out its bytes
/// @return false when it cannot be read
bool preview_read_file(
    void* /*context*/, const std::filesystem::path& file, std::vector<uint8_t>* out
) {
    if (out == nullptr)
        return false;
    std::error_code size_error;
    const auto size = std::filesystem::file_size(file, size_error);
    if (size_error || size == 0 || size > static_cast<uintmax_t>(content::max_picture_bytes))
        return false;
    std::ifstream input{file, std::ios::binary};
    if (!input)
        return false;
    out->resize(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(out->data()), static_cast<std::streamsize>(size));
    if (input.gcount() != static_cast<std::streamsize>(size)) {
        out->clear();
        return false;
    }
    return true;
}

/// Copies catalogue picture bytes the host has already taken.
///
/// @param context the preview state
/// @param key the picture's address
/// @param[out] out its bytes
/// @return false when they have not been taken
bool preview_online_bytes(void* context, const std::string& key, std::vector<uint8_t>* out) {
    auto* state = static_cast<MapPreviewState*>(context);
    if (state == nullptr || out == nullptr)
        return false;
    const auto found = state->online_bytes.find(key);
    if (found == state->online_bytes.end() || found->second.empty())
        return false;
    *out = found->second;
    return true;
}

/// Copies a decoded thumbnail into the pictures the inputs are built from.
///
/// @param[in,out] state the preview state
/// @param request the picture, already asked for
/// @return true when a new picture was stored
bool store_ready_preview(
    MapPreviewState& state, const oa::ui::map_previews::PreviewRequest& request
) {
    const oa::ui::map_previews::Thumbnail* thumbnail = state.cache.ask(request);
    if (thumbnail == nullptr || thumbnail->width <= 0 || thumbnail->height <= 0)
        return false;
    if (state.pictures.find(request.key) != state.pictures.end())
        return false;
    const auto width = static_cast<uint32_t>(thumbnail->width);
    const auto height = static_cast<uint32_t>(thumbnail->height);
    const std::size_t bytes =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(height) * 4U;
    if (thumbnail->rgba.size() != bytes)
        return false;
    library::MapPicture picture;
    picture.rgba = thumbnail->rgba;
    picture.width = width;
    picture.height = height;
    state.pictures.emplace(request.key, std::move(picture));
    state.online_bytes.erase(request.key);
    return true;
}

/// Tells whether an Active map's line is on screen.
///
/// @param list the display list
/// @param number the map's place among the tiles, from 1
/// @return true when its label has room
bool active_tile_shows(const kit::DisplayList& list, std::size_t number) {
    const std::string name = std::string(kActiveMapPrefix) + std::to_string(number);
    for (const kit::Control& control : list.controls) {
        if (control.name != name)
            continue;
        return control.rect.width > 0 && control.rect.height > 0;
    }
    return false;
}

/// Tells whether an entry is an installed map pack.
///
/// @param entry the entry
/// @param pack the pack
/// @return true when the Maps tab would select this entry for the pack
bool entry_is_installed_pack(const library::Entry& entry, const library::InstalledMaps& pack) {
    if (entry.id.kind != library::Kind::map_pack)
        return false;
    if (!pack.key.empty())
        return entry.id.key == pack.key && entry.id.registry == pack.registry;
    return library::entry_name(entry) == pack.name;
}

/// One minimap to decode this frame.
struct PreviewJob {
    oa::ui::map_previews::PreviewRequest request{};
    std::filesystem::path terrain{}; ///< set for a terrain file
};

} // namespace

library::Tab library_tab_of(settings::LibraryTab tab) noexcept {
    switch (tab) {
    case settings::LibraryTab::mods:
        return library::Tab::mods;
    case settings::LibraryTab::maps:
        return library::Tab::maps;
    case settings::LibraryTab::languages:
        return library::Tab::languages;
    case settings::LibraryTab::updates:
        return library::Tab::updates;
    case settings::LibraryTab::registries:
        return library::Tab::registries;
    }
    return library::Tab::mods;
}

// ---------------------------------------------------------------------------------------------
// The host and its worker

Runtime::LibraryHost::~LibraryHost() {
    bool join = false;
    {
        base::threads::LockGuard guard(work.mutex);
        work.stop = true;
        join = work.started;
    }
    work.cancel.store(true);
    work.check_cancel.store(true);
    work.wake.notify_all();
    if (join)
        base::threads::join_thread(work.thread);
}

bool Runtime::LibraryHost::ensure_worker() {
    base::threads::LockGuard guard(work.mutex);
    if (work.started)
        return true;
    if (!base::threads::start_thread(work.thread, library_worker, &work)) {
        std::cerr << "open-annihilation: library: the worker could not start\n";
        return false;
    }
    work.started = true;
    return true;
}

void Runtime::LibraryHost::look_again(Runtime& runtime) {
    if (!own_feed)
        return;
    std::shared_ptr<const content::Snapshot> snapshot = runtime.content_service().snapshot();
    {
        base::threads::LockGuard guard(work.mutex);
        work.snapshot = std::move(snapshot);
        work.user_folder = runtime.user_folder_;
        work.updates_due = true;
    }
    if (ensure_worker())
        work.wake.notify_all();
}

void Runtime::LibraryHost::rebuild(Runtime& runtime) {
    shown_checked = false;
    shown_maps.clear();
    library::Inputs inputs = feed.inputs != nullptr ? feed.inputs(feed.context) : library::Inputs{};
    // What the installer noted of the mods chosen here: a refused item fails
    // with the installer's reason, and an item whose change waits for the
    // player to leave the mod being played is held while it waits.
    if (runtime.mod_install_state_) {
        using Phase = library::QueueItem::Phase;
        const std::optional<std::string> waiting = package_install::pending_change_target();
        std::vector<library::QueueItem> staying;
        staying.reserve(inputs.queue.size());
        for (library::QueueItem& item : inputs.queue) {
            bool leave_out = false;
            if (item.kind == library::Kind::mod && !item.expansion) {
                for (const ModInstallState::LibraryNote& note :
                     runtime.mod_install_state_->library_notes) {
                    if (note.registry != item.registry || note.key != item.key)
                        continue;
                    // A cancelled held update has already ended in the download
                    // queue. Leaving that line out is what makes it go; a
                    // download still running is the player's new ask.
                    const bool ended = item.phase == Phase::installed || item.phase == Phase::held;
                    if (note.cancelled && ended) {
                        leave_out = true;
                        break;
                    }
                    const bool installing =
                        item.phase == Phase::checking || item.phase == Phase::installing;
                    if (!note.problem.empty() && (installing || item.phase == Phase::failed)) {
                        item.phase = Phase::failed;
                        item.problem = note.problem;
                        item.retryable = true;
                    } else if (
                        !note.held_target.empty() && waiting && *waiting == note.held_target &&
                        (installing || item.phase == Phase::installed)
                    ) {
                        item.phase = Phase::held;
                    }
                }
            }
            if (!leave_out)
                staying.push_back(std::move(item));
        }
        inputs.queue = std::move(staying);
    }
    // An update queued from the Library keeps saying what it changes while
    // the queue lists it.
    std::vector<KeptUpdate> still;
    for (const KeptUpdate& kept : kept_updates) {
        bool listed = false;
        for (library::QueueItem& item : inputs.queue) {
            if (item.registry != kept.registry || item.kind != kept.kind || item.key != kept.key)
                continue;
            listed = true;
            if (item.update_from.empty() && item.update_to.empty()) {
                item.update_rules = kept.rules;
                item.update_from = kept.from;
                item.update_to = kept.to;
            }
        }
        if (listed)
            still.push_back(kept);
    }
    kept_updates = std::move(still);
    // A check that is running is `checking`. Otherwise a remembered verdict
    // is mapped, and a mutator with none remembered is unchecked. With
    // start_check null every verdict stays unset. An installed mutator with
    // no facts is not given empty ones.
    if (feed.start_check != nullptr) {
        // A mutator installed, updated or removed makes the pass again.
        std::string installed_now;
        for (const library::Installed& installed : inputs.installed) {
            if (installed.kind != library::Kind::mutator)
                continue;
            installed_now += installed.key;
            installed_now.push_back('\t');
            installed_now += path_to_utf8(installed.folder);
            installed_now.push_back('\t');
            installed_now += installed.version;
            installed_now.push_back('\t');
            installed_now += std::to_string(installed.release);
            installed_now.push_back('\n');
        }
        if (installed_now != shown_installed) {
            shown_installed = std::move(installed_now);
            shown_ready = false;
        }
        for (library::Installed& installed : inputs.installed) {
            if (installed.kind != library::Kind::mutator || !installed.mutator)
                continue;
            library::Verdict verdict;
            if (!checking_id.empty() && checking_kind == CheckKind::mutator &&
                installed.key == checking_id) {
                verdict.state = library::Verdict::State::checking;
            } else if (feed.remembered_verdict != nullptr) {
                if (const std::optional<oa::data::mutators::VerdictRecord> record =
                        feed.remembered_verdict(feed.context, installed.key)) {
                    verdict = library::verdict_of_record(*record, model.text);
                    const auto maps = shown_maps.find(installed.key);
                    if (maps != shown_maps.end())
                        library::apply_map_fact(verdict, *record, maps->second, model.text);
                } else
                    verdict.state = library::Verdict::State::unchecked;
            } else {
                verdict.state = library::Verdict::State::unchecked;
            }
            installed.mutator->verdict = std::move(verdict);
        }
    }
    // A brain check that is running is `checking`. Otherwise a remembered
    // verdict for this copy and this game is shown, and a brain with none
    // remembered is unchecked. With start_brain_check null every verdict
    // stays unset. An installed brain with no facts is not given empty ones.
    if (feed.start_brain_check != nullptr) {
        for (library::Installed& installed : inputs.installed) {
            if (installed.kind != library::Kind::brain || !installed.brain)
                continue;
            library::Verdict verdict;
            if (!checking_id.empty() && checking_kind == CheckKind::brain &&
                installed.key == checking_id) {
                verdict.state = library::Verdict::State::checking;
            } else if (feed.remembered_brain_verdict != nullptr) {
                if (const std::optional<library::Verdict> remembered =
                        feed.remembered_brain_verdict(feed.context, installed.key))
                    verdict = *remembered;
                else
                    verdict.state = library::Verdict::State::unchecked;
            } else {
                verdict.state = library::Verdict::State::unchecked;
            }
            installed.brain->verdict = std::move(verdict);
        }
    }
    library::refresh(model, std::move(inputs));
    const std::size_t offered = library::update_count(model);
    if (offered != update_count)
        std::cerr << "open-annihilation: library: updates offered: " << offered << '\n';
    update_count = offered;
    refreshed = true;
    ++model_revision;
}

void Runtime::LibraryHost::wait_turn(MutatorCheck waiting) {
    for (MutatorCheck& queued : work.checks_waiting) {
        if (queued.id == waiting.id && queued.kind == waiting.kind) {
            queued = std::move(waiting);
            return;
        }
    }
    work.checks_waiting.push_back(std::move(waiting));
}

bool Runtime::LibraryHost::hand_to_worker(MutatorCheck prepared) {
    checking_id = prepared.id;
    checking_kind = prepared.kind;
    check_open = true;
    {
        base::threads::LockGuard guard(work.mutex);
        work.check_cancel.store(false);
        work.check_job = std::move(prepared);
        work.check_due = true;
    }
    if (ensure_worker()) {
        work.wake.notify_all();
        return true;
    }
    // The thread did not start, so nothing will ever record an outcome.
    // Drop the job or the row stays "Checking…" and later checks wait.
    {
        base::threads::LockGuard guard(work.mutex);
        work.check_job.reset();
        work.check_due = false;
    }
    checking_id.clear();
    checking_kind = CheckKind::mutator;
    check_open = false;
    return false;
}

void Runtime::LibraryHost::begin_mutator_check(Runtime& runtime, std::string_view id) {
    if (feed.start_check == nullptr || id.empty())
        return;
    {
        base::threads::LockGuard guard(work.mutex);
        if (check_open) {
            wait_turn(MutatorCheck{std::string(id), {}, {}});
            return;
        }
    }
    prepared_key.reset();
    auto job = feed.start_check(feed.context, id);
    if (!job) {
        prepared_key.reset();
        return;
    }
    MutatorCheck prepared;
    prepared.id = std::string(id);
    prepared.work = std::move(job);
    prepared.key = std::move(prepared_key);
    prepared_key.reset();
    hand_to_worker(std::move(prepared));
    rebuild(runtime);
}

void Runtime::LibraryHost::begin_brain_check(Runtime& runtime, std::string_view id) {
    if (feed.start_brain_check == nullptr || id.empty())
        return;
    {
        base::threads::LockGuard guard(work.mutex);
        if (check_open) {
            MutatorCheck waiting;
            waiting.kind = CheckKind::brain;
            waiting.id = std::string(id);
            wait_turn(std::move(waiting));
            return;
        }
    }
    prepared_brain_key.reset();
    auto job = feed.start_brain_check(feed.context, id);
    if (!job) {
        prepared_brain_key.reset();
        return;
    }
    MutatorCheck prepared;
    prepared.kind = CheckKind::brain;
    prepared.id = std::string(id);
    prepared.brain_work = std::move(job);
    prepared.brain_key = std::move(prepared_brain_key);
    prepared_brain_key.reset();
    hand_to_worker(std::move(prepared));
    rebuild(runtime);
}

void Runtime::LibraryHost::enqueue_prepared_check(Runtime& runtime, MutatorCheck prepared) {
    if (prepared.id.empty() || !prepared.work)
        return;
    {
        base::threads::LockGuard guard(work.mutex);
        if (check_open) {
            wait_turn(std::move(prepared));
            return;
        }
    }
    hand_to_worker(std::move(prepared));
    rebuild(runtime);
}

bool Runtime::LibraryHost::finish_mutator_check() {
    std::optional<MutatorCheckOutcome> outcome;
    {
        base::threads::LockGuard guard(work.mutex);
        if (!work.check_outcome)
            return false;
        outcome = std::move(*work.check_outcome);
        work.check_outcome.reset();
    }
    if (!outcome->cancelled && outcome->kind == CheckKind::brain) {
        if (outcome->brain && feed.keep_brain_verdict != nullptr) {
            delivered_brain_key = std::move(outcome->brain_key);
            feed.keep_brain_verdict(feed.context, outcome->id, *outcome->brain);
            delivered_brain_key.reset();
        }
    } else if (!outcome->cancelled && outcome->record && feed.keep_verdict != nullptr) {
        delivered_key = std::move(outcome->key);
        feed.keep_verdict(feed.context, outcome->id, *outcome->record);
        delivered_key.reset();
    }

    check_open = false;
    checking_id.clear();
    checking_kind = CheckKind::mutator;
    // The oldest waiting check that can start goes to the worker. One whose
    // list no longer resolves starts nothing, and the next one is tried.
    for (;;) {
        std::optional<MutatorCheck> next;
        {
            base::threads::LockGuard guard(work.mutex);
            const bool drop_waiting = work.match_running || work.check_cancel.load();
            if (drop_waiting) {
                work.checks_waiting.clear();
            } else if (!work.checks_waiting.empty()) {
                next = std::move(work.checks_waiting.front());
                work.checks_waiting.erase(work.checks_waiting.begin());
            }
        }
        if (!next)
            return true;
        if (next->kind == CheckKind::brain) {
            std::function<std::optional<library::Verdict>(const std::atomic<bool>&)> brain_job;
            std::optional<BrainVerdictKey> brain_key;
            if (next->brain_work) {
                brain_job = std::move(next->brain_work);
                brain_key = std::move(next->brain_key);
            } else if (feed.start_brain_check != nullptr) {
                prepared_brain_key.reset();
                brain_job = feed.start_brain_check(feed.context, next->id);
                brain_key = std::move(prepared_brain_key);
                prepared_brain_key.reset();
            }
            if (!brain_job)
                continue;
            MutatorCheck prepared;
            prepared.kind = CheckKind::brain;
            prepared.id = std::move(next->id);
            prepared.brain_work = std::move(brain_job);
            prepared.brain_key = std::move(brain_key);
            if (hand_to_worker(std::move(prepared)))
                return true;
            continue;
        }
        std::function<std::optional<mutators::VerdictRecord>(const std::atomic<bool>&)> job;
        std::optional<mutators::VerdictKey> key;
        if (next->work) {
            job = std::move(next->work);
            key = std::move(next->key);
        } else if (feed.start_check != nullptr) {
            prepared_key.reset();
            job = feed.start_check(feed.context, next->id);
            key = std::move(prepared_key);
            prepared_key.reset();
        }
        if (!job)
            continue;
        MutatorCheck prepared;
        prepared.id = std::move(next->id);
        prepared.work = std::move(job);
        prepared.key = std::move(key);
        if (hand_to_worker(std::move(prepared)))
            return true;
    }
}

bool Runtime::LibraryHost::picture_known(void* context, const library::Entry& entry) {
    auto* host = static_cast<LibraryHost*>(context);
    if (host == nullptr || host->work.pictures == nullptr)
        return true;
    const auto url = host->picture_urls.find({entry.id.registry, entry.id.key});
    if (url != host->picture_urls.end() && host->badges.count(url->second) == 0 &&
        !host->work.pictures->asked(entry.id.registry, url->second))
        return false;
    if (host->previews != nullptr && entry.listing != nullptr) {
        for (const library::MapLine& map : entry.listing->maps) {
            if (map.preview_url.empty() || host->previews->pictures.count(map.preview_url) != 0 ||
                host->previews->online_bytes.count(map.preview_url) != 0)
                continue;
            if (!host->work.pictures->asked(entry.id.registry, map.preview_url))
                return false;
        }
    }
    return true;
}

void Runtime::LibraryHost::picture_request(void* context, const library::Entry& entry, bool lead) {
    auto* host = static_cast<LibraryHost*>(context);
    if (host == nullptr || host->work.pictures == nullptr)
        return;
    const auto url = host->picture_urls.find({entry.id.registry, entry.id.key});
    if (url != host->picture_urls.end() && host->badges.count(url->second) == 0 &&
        !host->work.pictures->refused(entry.id.registry, url->second)) {
        if (lead)
            host->work.pictures->prefer(entry.id.registry, url->second);
        else
            host->work.pictures->request(entry.id.registry, url->second);
    }
    if (host->previews == nullptr || entry.listing == nullptr)
        return;
    for (const library::MapLine& map : entry.listing->maps) {
        if (map.preview_url.empty() || host->previews->pictures.count(map.preview_url) != 0 ||
            host->previews->online_bytes.count(map.preview_url) != 0)
            continue;
        if (host->work.pictures->refused(entry.id.registry, map.preview_url))
            continue;
        host->work.pictures->request(entry.id.registry, map.preview_url);
    }
}

const library::Badge* Runtime::LibraryHost::shipped_logo_badge() {
    if (!shipped_logo_tried) {
        shipped_logo_tried = true;
        WindowIcon decoded;
        std::string error;
        if (decode_window_icon(window_icon_png(), decoded, error)) {
            const WindowIcon part = visible_part(decoded);
            if (!part.pixels.empty() && part.width > 0 && part.height > 0)
                shipped_logo = library::Badge{part.pixels, part.width, part.height};
        }
    }
    return shipped_logo ? &*shipped_logo : nullptr;
}

void Runtime::LibraryHost::ask_cached_picture(std::string_view registry, std::string_view url) {
    if (!work.pictures || url.empty())
        return;
    {
        base::threads::LockGuard guard(work.mutex);
        if (work.match_running)
            return;
    }
    const std::string key(url);
    if (badges.find(key) != badges.end() || work.pictures->refused(registry, key))
        return;
    if (!work.pictures->asked(registry, key))
        work.pictures->request(registry, key);
    if (!work.pictures->waiting())
        return;
    {
        base::threads::LockGuard guard(work.mutex);
        work.pictures_due = true;
    }
    if (ensure_worker())
        work.wake.notify_all();
}

std::optional<library::Badge>
Runtime::LibraryHost::cached_picture(std::string_view registry, std::string_view url) {
    if (url.empty())
        return std::nullopt;
    const std::string key(url);
    auto held = badges.find(key);
    if (held == badges.end() && work.pictures) {
        if (std::optional<ModBadge> taken = work.pictures->take(registry, key)) {
            if (std::optional<library::Badge> decoded = badge_of(*taken))
                held = badges.emplace(key, std::move(*decoded)).first;
        }
    }
    if (held == badges.end())
        return std::nullopt;
    return held->second;
}

bool Runtime::LibraryHost::registry_icon_known(void* context, const library::Registry& registry) {
    auto* host = static_cast<LibraryHost*>(context);
    if (host == nullptr || host->work.pictures == nullptr)
        return true;
    if (registry.shipped_logo || registry.icon_url.empty() || registry.icon)
        return true;
    if (host->badges.find(registry.icon_url) != host->badges.end())
        return true;
    return host->work.pictures->asked(registry.id, registry.icon_url);
}

void Runtime::LibraryHost::registry_icon_request(void* context, const library::Registry& registry) {
    auto* host = static_cast<LibraryHost*>(context);
    if (host == nullptr || host->work.pictures == nullptr)
        return;
    if (registry.shipped_logo || registry.icon_url.empty() || registry.icon)
        return;
    if (host->badges.find(registry.icon_url) != host->badges.end())
        return;
    if (host->work.pictures->refused(registry.id, registry.icon_url))
        return;
    host->work.pictures->request(registry.id, registry.icon_url);
}

void Runtime::LibraryHost::request_pictures(const kit::DisplayList& list) {
    if (!own_feed || !work.pictures)
        return;
    {
        base::threads::LockGuard guard(work.mutex);
        if (work.match_running)
            return;
    }
    // Rows are asked by their control, not by the text they draw: a maker's
    // mark rides on that text, and matching it left every marked row
    // unasked. The selected row is first. What waits is
    // forgotten as the Library closes. A registry icon uses this same cache.
    library::PictureSource source;
    source.context = this;
    source.known = picture_known;
    source.request = picture_request;
    library::request_visible_pictures(model, list, source);
    library::RegistryPictureSource icons;
    icons.context = this;
    icons.known = registry_icon_known;
    icons.request = registry_icon_request;
    library::request_registry_icons(model, icons);
    if (!work.pictures->waiting())
        return;
    {
        base::threads::LockGuard guard(work.mutex);
        work.pictures_due = true;
    }
    if (ensure_worker())
        work.wake.notify_all();
}

void Runtime::LibraryHost::pump_map_previews(Runtime& runtime, const kit::DisplayList& list) {
    if (previews == nullptr)
        return;
    MapPreviewState& preview = *previews;
    if (!preview.palette_tried) {
        preview.palette_tried = true;
        try {
            const std::vector<uint8_t> bytes = runtime.assets_.read("palettes/palette.pal").bytes;
            if (bytes.size() == oa::PaletteBytes{}.size()) {
                oa::PaletteBytes palette{};
                std::copy(bytes.begin(), bytes.end(), palette.begin());
                preview.palette = palette;
            }
        } catch (const std::exception&) {
        }
    }

    std::vector<PreviewJob> jobs;
    const auto want = [&](oa::ui::map_previews::PreviewRequest request,
                          std::filesystem::path terrain) {
        if (request.key.empty() || preview.pictures.count(request.key) != 0 ||
            preview.cache.failed(request.key))
            return;
        for (const PreviewJob& job : jobs)
            if (job.request.key == request.key)
                return;
        jobs.push_back(PreviewJob{std::move(request), std::move(terrain)});
    };

    if (model.tab == library::Tab::active) {
        const library::ActiveSetup setup = library::active_setup(model);
        for (const PackMap& map : runtime.map_packs().maps()) {
            const std::string title = map.title.empty() ? map.stem : map.title;
            bool shows = false;
            for (std::size_t place = 0; place < setup.maps.size(); ++place) {
                const library::ActiveMap& tile = setup.maps[place];
                if (tile.name == title && tile.pack_name == map.pack_name &&
                    active_tile_shows(list, place + 1))
                    shows = true;
            }
            if (!shows)
                continue;
            oa::ui::map_previews::PreviewRequest request;
            request.key = installed_picture_key(map);
            if (!map.preview.empty()) {
                request.source = oa::ui::map_previews::PreviewSource::pack;
                request.file = map.preview;
                want(std::move(request), {});
            } else if (preview.palette) {
                request.source = oa::ui::map_previews::PreviewSource::base;
                request.map = map.stem;
                want(std::move(request), terrain_file(map));
            }
        }
    } else {
        for (const library::Entry& entry : model.entries) {
            if (entry.id.kind != library::Kind::map_pack ||
                !library::entry_row_shown(model, list, entry.id))
                continue;
            if (entry.listing != nullptr) {
                for (const library::MapLine& line : entry.listing->maps) {
                    if (line.preview_url.empty() ||
                        preview.online_bytes.count(line.preview_url) == 0)
                        continue;
                    oa::ui::map_previews::PreviewRequest request;
                    request.key = line.preview_url;
                    request.source = oa::ui::map_previews::PreviewSource::online;
                    want(std::move(request), {});
                }
            }
            const library::InstalledMaps* pack = nullptr;
            for (const library::InstalledMaps& installed : model.inputs.map_packs) {
                if (entry_is_installed_pack(entry, installed)) {
                    pack = &installed;
                    break;
                }
            }
            if (pack == nullptr)
                continue;
            for (const PackMap& map : runtime.map_packs().maps()) {
                if (map.pack_name != pack->name)
                    continue;
                bool listed = false;
                for (const library::MapLine& line : pack->maps)
                    listed = listed || line.stem == map.stem;
                if (!listed)
                    continue;
                oa::ui::map_previews::PreviewRequest request;
                request.key = installed_picture_key(map);
                if (!map.preview.empty()) {
                    request.source = oa::ui::map_previews::PreviewSource::pack;
                    request.file = map.preview;
                    want(std::move(request), {});
                } else if (preview.palette) {
                    request.source = oa::ui::map_previews::PreviewSource::base;
                    request.map = map.stem;
                    want(std::move(request), terrain_file(map));
                }
            }
        }
    }

    preview.cache.new_frame();
    oa::ui::map_previews::PreviewHost reader;
    reader.context = &preview;
    reader.read_map = preview_read_map;
    reader.read_file = preview_read_file;
    reader.online_bytes = preview_online_bytes;
    reader.palette = preview.palette ? &*preview.palette : nullptr;

    const PreviewJob* base_job = nullptr;
    for (const PreviewJob& job : jobs) {
        if (job.request.source == oa::ui::map_previews::PreviewSource::base) {
            base_job = &job;
            break;
        }
    }
    if (base_job != nullptr) {
        preview.terrain = base_job->terrain;
        preview.cache.ask(base_job->request);
        preview.cache.pump(reader, 1);
    } else {
        preview.terrain.clear();
        for (const PreviewJob& job : jobs)
            preview.cache.ask(job.request);
        preview.cache.pump(reader, 4);
    }

    bool stored = false;
    const auto take_job = [&](const PreviewJob& job) {
        if (store_ready_preview(preview, job.request))
            stored = true;
        else if (preview.cache.failed(job.request.key))
            preview.online_bytes.erase(job.request.key);
    };
    if (base_job != nullptr)
        take_job(*base_job);
    else
        for (const PreviewJob& job : jobs)
            take_job(job);
    if (stored)
        ++preview.generation;
}

void Runtime::destroy_library_host(LibraryHost* host) noexcept {
    delete host;
}

Runtime::LibraryHost& Runtime::library_host() {
    if (!library_) {
        library_.reset(new LibraryHost{});
        LibraryHost& host = *library_;
        host.feed = LibraryFeed{
            this,
            LibraryHost::own_inputs,
            LibraryHost::own_generation,
            LibraryHost::own_enqueue,
            LibraryHost::own_cancel,
            LibraryHost::own_retry,
            LibraryHost::own_built_in,
            LibraryHost::own_remembered_verdict,
            LibraryHost::own_start_check,
            LibraryHost::own_keep_verdict,
            LibraryHost::own_remembered_brain_verdict,
            LibraryHost::own_start_brain_check,
            LibraryHost::own_keep_brain_verdict
        };
        host.model.text = library::TextHooks{nullptr, shown_word};
        // The pictures are kept beside the cached catalogues.
        const std::filesystem::path content_folder = content_service().content_folder();
        std::filesystem::path pictures_folder;
        if (!content_folder.empty())
            pictures_folder = content_folder / std::string(content::pictures_folder_name);
        host.work.pictures =
            std::make_unique<content::PictureCache>(pictures_folder, &host.work.cancel);
    }
    if (!library_->previews)
        library_->previews = std::make_unique<MapPreviewState>();
    return *library_;
}

// ---------------------------------------------------------------------------------------------
// The runtime's own feed

library::Inputs Runtime::LibraryHost::own_inputs(void* context) {
    return static_cast<Runtime*>(context)->library_inputs();
}

uint64_t Runtime::LibraryHost::own_generation(void* context) {
    Runtime& runtime = *static_cast<Runtime*>(context);
    return runtime.library_feed_generation();
}

std::optional<uint64_t> Runtime::LibraryHost::own_enqueue(
    void* context,
    std::string_view registry,
    std::string_view key,
    content::DownloadReason reason,
    std::string* why
) {
    return static_cast<Runtime*>(context)->queue_download(registry, key, reason, why);
}

void Runtime::LibraryHost::own_cancel(
    void* context, std::string_view registry, std::string_view key
) {
    content::Downloads& downloads = static_cast<Runtime*>(context)->content_downloads();
    std::optional<uint64_t> item;
    for (const content::DownloadView& queued : downloads.view())
        if (queued.target.registry == registry && queued.target.key == key && !ended(queued.state))
            item = queued.item;
    if (item)
        downloads.cancel(*item);
}

void Runtime::LibraryHost::own_retry(
    void* context, std::string_view registry, std::string_view key
) {
    content::Downloads& downloads = static_cast<Runtime*>(context)->content_downloads();
    std::optional<uint64_t> item;
    for (const content::DownloadView& queued : downloads.view())
        if (queued.target.registry == registry && queued.target.key == key &&
            (queued.state == content::DownloadState::failed ||
             queued.state == content::DownloadState::cancelled))
            item = queued.item;
    if (item)
        downloads.retry(*item);
}

bool Runtime::LibraryHost::own_built_in(void* context, std::string_view registry) {
    const std::shared_ptr<const content::Snapshot> snapshot =
        static_cast<Runtime*>(context)->content_service().snapshot();
    if (!snapshot)
        return false;
    const content::RegistryView* view = registry_view(*snapshot, registry);
    return view != nullptr && view->registry.origin == data::registry::Origin::built_in;
}

namespace {

/// True when a dry run stopped because it was cancelled. That verdict is not kept.
///
/// @param record the verdict
/// @return true when it is the cancelled reason
bool cancelled_dry_run(const mutators::VerdictRecord& record) {
    return record.verdict == mutators::Verdict::failed &&
           record.reason == mutators::dry_run_cancelled_reason;
}

/// The On choice for an id, or null when that mutator is not in the set.
///
/// @param on the On mutators
/// @param id the mutator's id
/// @return the choice, or null
const MutatorCheckChoice* choice_for(std::span<const MutatorCheckChoice> on, std::string_view id) {
    for (const MutatorCheckChoice& choice : on)
        if (choice.id == id)
            return &choice;
    return nullptr;
}

/// True when the list's last mutator, whose verdict is shown, declares the
/// maps layer. A prerequisite's maps layer is that prerequisite's change.
///
/// @param list the mutators, prerequisites included
/// @return true when the last one does
bool list_declares_maps(std::span<const mutators::ResolvedMutator> list) {
    if (list.empty())
        return false;
    for (const formats::oamut::Layer layer : list.back().manifest.layers) {
        if (layer == formats::oamut::Layer::maps)
            return true;
    }
    return false;
}

} // namespace

void Runtime::LibraryHost::ensure_shown_pass(Runtime& runtime) {
    if (shown_ready && shown_checked)
        return;
    shown_checked = true;
    uint64_t searched = 0;
    {
        base::threads::LockGuard guard(work.mutex);
        searched = work.searches;
    }
    const oa::base::sha256::Digest mod = runtime.mutator_mod_digest();
    const formats::oamut::ClashMode clash = runtime.mutator_clash_default();
    if (shown_ready && searched == shown_searches && mod == shown_mod && clash == shown_clash)
        return;
    if (!shown_ready || searched != shown_searches) {
        // The runtime's own rows come from the worker's last search, and so
        // does this listing. A feed of its own is listed here.
        bool taken = false;
        if (own_feed) {
            base::threads::LockGuard guard(work.mutex);
            if (work.searches != 0) {
                shown_listing = work.mutator_listing;
                searched = work.searches;
                taken = true;
            }
        }
        if (!taken) {
            const std::filesystem::path folder =
                runtime.user_folder() / std::string(data::mutator_library::mutators_folder_name);
            shown_listing = data::mutator_library::list_installed(folder);
        }
        ++shown_listings;
        shown_stored = runtime.read_mutator_verdicts();
    }
    shown_rows.clear();
    shown_searches = searched;
    shown_mod = mod;
    shown_clash = clash;
    shown_ready = true;
}

std::optional<std::vector<mutators::ResolvedMutator>> Runtime::LibraryHost::resolve_shown_list(
    const data::mutator_library::Listing& listing,
    std::string_view id,
    std::span<const MutatorCheckChoice> on,
    mutators::FolderDigests* digests
) {
    if (id.empty())
        return std::nullopt;
    std::vector<library::Need> graph;
    graph.reserve(listing.mutators.size());
    for (const data::mutator_library::InstalledMutator& installed : listing.mutators) {
        library::Need need;
        need.id = installed.manifest.id;
        need.needs.reserve(installed.manifest.requires_mutators.size());
        for (const formats::oamut::Condition& condition : installed.manifest.requires_mutators)
            need.needs.push_back(condition.id);
        graph.push_back(std::move(need));
    }
    const std::vector<std::string> roots{std::string(id)};
    const library::Closure closure = library::prerequisite_closure(graph, roots);
    if (closure.cyclic || closure.order.empty())
        return std::nullopt;

    std::vector<mutators::ResolvedMutator> resolved;
    resolved.reserve(closure.order.size());
    for (const std::string& member : closure.order) {
        mutators::MutatorChoice choice;
        choice.target = member;
        if (const MutatorCheckChoice* chosen = choice_for(on, member))
            choice.options = chosen->options;
        const data::mutator_library::InstalledMutator* installed =
            data::mutator_library::find(listing, member);
        if (installed == nullptr)
            return std::nullopt;
        std::string choice_error;
        std::optional<mutators::ResolvedMutator> one =
            mutators::resolve_installed(choice, *installed, &choice_error, digests);
        if (!one)
            return std::nullopt;
        if (const MutatorCheckChoice* chosen = choice_for(on, member))
            if (chosen->clashes)
                one->clashes = *chosen->clashes;
        resolved.push_back(std::move(*one));
    }
    if (resolved.empty())
        return std::nullopt;
    return resolved;
}

std::optional<mutators::VerdictRecord> Runtime::LibraryHost::verdict_for(
    Runtime& runtime,
    std::span<const mutators::ResolvedMutator> list,
    const oa::base::sha256::Digest& mod_digest,
    std::span<const mutators::RememberedVerdict> stored
) {
    if (list.empty())
        return std::nullopt;
    return verdict_for_key(runtime, runtime.verdict_key_for_list(list, mod_digest), stored);
}

std::optional<mutators::VerdictRecord> Runtime::LibraryHost::verdict_for_key(
    Runtime& runtime,
    const mutators::VerdictKey& key,
    std::span<const mutators::RememberedVerdict> stored
) {
    LibraryHost& host = runtime.library_host();
    for (const CachedVerdict& cached : host.verdict_cache)
        if (cached.key == key)
            return cached.record;
    const mutators::VerdictRecord* found = mutators::find_verdict(stored, key);
    std::optional<mutators::VerdictRecord> record;
    if (found != nullptr)
        record = *found;
    host.verdict_cache.push_back(CachedVerdict{key, record});
    return record;
}

std::function<std::optional<mutators::VerdictRecord>(const std::atomic<bool>&)>
Runtime::LibraryHost::dry_run_work(mutators::DryRunJob job, std::string id) {
    return [job = std::move(job), id = std::move(id)](
               const std::atomic<bool>& cancel
           ) -> std::optional<mutators::VerdictRecord> {
        const mutators::VerdictRecord record = mutators::run_dry_run(job, cancel);
        if (!cancelled_dry_run(record)) {
            const std::string label =
                record.mod_label.empty() ? std::string("No Mod") : record.mod_label;
            std::cerr << "open-annihilation: mutator check: " << id << " with " << label << ": "
                      << mutators::verdict_word(record.verdict) << '\n';
        }
        return record;
    };
}

std::function<std::optional<mutators::VerdictRecord>(const std::atomic<bool>&)>
Runtime::LibraryHost::start_default_check(Runtime& runtime, std::string_view id) {
    const std::filesystem::path folder =
        runtime.user_folder() / std::string(data::mutator_library::mutators_folder_name);
    const data::mutator_library::Listing listing = data::mutator_library::list_installed(folder);
    LibraryHost& host = runtime.library_host();
    const std::optional<std::vector<mutators::ResolvedMutator>> list =
        resolve_shown_list(listing, id, {}, &host.work.mutator_digests);
    if (!list)
        return {};
    mutators::DryRunJob job;
    try {
        job = runtime.prepare_mutator_check(*list);
    } catch (const std::exception&) {
        return {};
    }
    host.prepared_key = job.key;
    const std::string logged =
        list->back().manifest.id.empty() ? std::string(id) : list->back().manifest.id;
    return dry_run_work(std::move(job), logged);
}

void Runtime::LibraryHost::keep_delivered(Runtime& runtime, const mutators::VerdictRecord& record) {
    LibraryHost& host = runtime.library_host();
    if (!host.delivered_key)
        return;
    const mutators::VerdictKey key = std::move(*host.delivered_key);
    host.delivered_key.reset();
    if (cancelled_dry_run(record))
        return;
    ++runtime.mutator_dry_runs_;
    runtime.remember_mutator_verdict(key, record);
    bool stored = false;
    for (CachedVerdict& cached : host.verdict_cache) {
        if (cached.key == key) {
            cached.record = record;
            stored = true;
            break;
        }
    }
    if (!stored)
        host.verdict_cache.push_back(CachedVerdict{key, record});
    ++host.verdict_generation;
}

std::optional<mutators::VerdictRecord>
Runtime::LibraryHost::own_remembered_verdict(void* context, std::string_view id) {
    Runtime& runtime = *static_cast<Runtime*>(context);
    LibraryHost& host = runtime.library_host();
    host.ensure_shown_pass(runtime);
    auto row = host.shown_rows.find(id);
    if (row == host.shown_rows.end()) {
        ShownRow made;
        const std::optional<std::vector<mutators::ResolvedMutator>> list =
            resolve_shown_list(host.shown_listing, id, {}, &host.work.mutator_digests);
        if (list && !list->empty()) {
            made.key = runtime.verdict_key_for_list(*list, host.shown_mod);
            made.maps = list_declares_maps(*list);
        }
        row = host.shown_rows.emplace(std::string(id), std::move(made)).first;
    }
    if (!row->second.key)
        return std::nullopt;
    host.shown_maps.insert_or_assign(std::string(id), row->second.maps);
    return verdict_for_key(runtime, *row->second.key, host.shown_stored);
}

std::function<std::optional<mutators::VerdictRecord>(const std::atomic<bool>&)>
Runtime::LibraryHost::own_start_check(void* context, std::string_view id) {
    return start_default_check(*static_cast<Runtime*>(context), id);
}

void Runtime::LibraryHost::own_keep_verdict(
    void* context, std::string_view /*id*/, const mutators::VerdictRecord& record
) {
    keep_delivered(*static_cast<Runtime*>(context), record);
}

std::optional<BrainVerdictKey>
Runtime::LibraryHost::brain_verdict_key(Runtime& runtime, std::string_view id) {
    if (id.empty())
        return std::nullopt;
    std::optional<data::brain_library::Listing> copied;
    {
        base::threads::LockGuard guard(work.mutex);
        if (work.searches > 0)
            copied = work.brain_listing;
    }
    data::brain_library::Listing listed;
    const data::brain_library::Listing* listing = nullptr;
    if (copied) {
        listing = &*copied;
    } else {
        listed = data::brain_library::list_installed(
            runtime.user_folder() / std::string(brains_folder_name)
        );
        listing = &listed;
    }
    const data::brain_library::InstalledBrain* found = data::brain_library::find(*listing, id);
    if (found == nullptr)
        return std::nullopt;
    const oa::data::brain_facts::GameFacts game = runtime.brain_game_facts();
    BrainVerdictKey key;
    key.id = std::string(id);
    key.version = oa::formats::oamut::version_text(found->manifest.version);
    key.package_sha256 = found->package_sha256;
    key.mod_id = game.mod.id;
    key.mod_version = game.mod.version;
    return key;
}

std::optional<library::Verdict>
Runtime::LibraryHost::own_remembered_brain_verdict(void* context, std::string_view id) {
    Runtime& runtime = *static_cast<Runtime*>(context);
    LibraryHost& host = runtime.library_host();
    const std::optional<BrainVerdictKey> key = host.brain_verdict_key(runtime, id);
    if (!key)
        return std::nullopt;
    for (const RememberedBrainVerdict& kept : host.brain_verdicts) {
        if (kept.key == *key)
            return kept.verdict;
    }
    return std::nullopt;
}

std::function<std::optional<library::Verdict>(const std::atomic<bool>&)>
Runtime::LibraryHost::own_start_brain_check(void* context, std::string_view id) {
    Runtime& runtime = *static_cast<Runtime*>(context);
    LibraryHost& host = runtime.library_host();
    const std::optional<BrainVerdictKey> key = host.brain_verdict_key(runtime, id);
    if (!key)
        return {};
    host.prepared_brain_key = *key;
    const oa::data::brain_facts::GameFacts facts = runtime.brain_game_facts();
    const std::string summary(library::shown(host.model.text, library::words::fits_this_game));
    const std::filesystem::path folder = runtime.user_folder() / std::string(brains_folder_name);
    const std::string brain_id(id);
    return [facts, summary, folder, brain_id](
               const std::atomic<bool>& cancel
           ) -> std::optional<library::Verdict> {
        try {
            if (cancel.load())
                return std::nullopt;
            std::string error;
            const std::optional<InstalledBrain> installed =
                read_installed_brain(folder, brain_id, error);
            if (cancel.load())
                return std::nullopt;
            if (!installed) {
                library::Verdict verdict;
                verdict.state = library::Verdict::State::failed;
                verdict.reason =
                    error.empty() ? std::string("the brain could not be seated") : error;
                return verdict;
            }
            BrainSeatChoice choice;
            choice.id = brain_id;
            ResolvedSeat resolved;
            if (!resolve_seat(*installed, choice, resolved, error)) {
                library::Verdict verdict;
                verdict.state = library::Verdict::State::failed;
                verdict.reason =
                    error.empty() ? std::string("the brain could not be seated") : error;
                return verdict;
            }
            std::vector<oa::sim::brains::PackageFile> files;
            std::vector<oa::sim::brains::OptionValue> options;
            std::vector<oa::sim::brains::NetworkFile> networks;
            const oa::sim::brains::SeatSpec spec =
                seat_spec(0, *installed, resolved, files, options, networks);
            if (cancel.load())
                return std::nullopt;
            const oa::sim::brains::SeatOutcome outcome = oa::sim::brains::check_brain(spec, facts);
            if (cancel.load())
                return std::nullopt;
            library::Verdict verdict;
            switch (outcome.result) {
            case oa::sim::brains::SeatResult::seated:
                verdict.state = library::Verdict::State::compatible;
                verdict.summary = summary;
                break;
            case oa::sim::brains::SeatResult::does_not_fit:
                verdict.state = library::Verdict::State::incompatible;
                if (outcome.reason[0] != '\0')
                    verdict.reason = std::string(outcome.reason.data());
                break;
            case oa::sim::brains::SeatResult::refused:
            case oa::sim::brains::SeatResult::failed:
                verdict.state = library::Verdict::State::failed;
                if (outcome.reason[0] != '\0')
                    verdict.reason = std::string(outcome.reason.data());
                else {
                    const std::string described = oa::data::script_host::describe(outcome.error);
                    verdict.reason = described.empty()
                                         ? std::string("the brain could not be seated")
                                         : described;
                }
                break;
            }
            return verdict;
        } catch (const std::exception& thrown) {
            if (cancel.load())
                return std::nullopt;
            library::Verdict verdict;
            verdict.state = library::Verdict::State::failed;
            verdict.reason = thrown.what();
            if (verdict.reason.empty())
                verdict.reason = "the brain could not be seated";
            return verdict;
        } catch (...) {
            if (cancel.load())
                return std::nullopt;
            library::Verdict verdict;
            verdict.state = library::Verdict::State::failed;
            verdict.reason = "the brain could not be seated";
            return verdict;
        }
    };
}

void Runtime::LibraryHost::own_keep_brain_verdict(
    void* context, std::string_view id, const library::Verdict& verdict
) {
    Runtime& runtime = *static_cast<Runtime*>(context);
    LibraryHost& host = runtime.library_host();
    if (!host.delivered_brain_key)
        return;
    const BrainVerdictKey key = std::move(*host.delivered_brain_key);
    host.delivered_brain_key.reset();
    if (key.id != id)
        return;
    for (RememberedBrainVerdict& kept : host.brain_verdicts) {
        if (kept.key == key) {
            kept.verdict = verdict;
            return;
        }
    }
    host.brain_verdicts.push_back(RememberedBrainVerdict{key, verdict});
}

uint64_t Runtime::library_feed_generation() {
    LibraryHost& host = library_host();
    uint64_t searches = 0;
    {
        base::threads::LockGuard guard(host.work.mutex);
        searches = host.work.searches;
    }
    const uint64_t pictures = host.work.pictures ? host.work.pictures->generation() : 0;
    const uint64_t minimaps = host.previews ? host.previews->generation : 0;
    return content_service().generation() + content_downloads().generation() + searches + pictures +
           minimaps + engine_settings_state().offered_generation;
}

library::Inputs Runtime::library_inputs() {
    LibraryHost& host = library_host();
    library::Inputs inputs;
    const std::shared_ptr<const content::Snapshot> snapshot = content_service().snapshot();
    const int64_t now = seconds_now();
    std::optional<int64_t> newest;
    if (snapshot) {
        for (const content::RegistryView& view : snapshot->registries) {
            if (view.registry.conflicting)
                continue;
            const data::registry::Descriptor& descriptor = view.registry.descriptor;
            library::Registry registry;
            registry.id = descriptor.id;
            registry.name = descriptor.name;
            registry.title = descriptor.title;
            registry.description = descriptor.description;
            registry.author = descriptor.author;
            registry.built_in = view.registry.origin == data::registry::Origin::built_in;
            registry.downloads_on =
                descriptor.install_id != data::registry::InstallIdUse::required ||
                content::read_install_id(preference_values_, descriptor.id).state !=
                    content::InstallIdState::off;
            registry.on = view.registry.enabled;
            if (!view.registry.url.empty())
                registry.address = view.registry.url;
            else
                registry.address = formats::url::url_text(descriptor.catalogue);
            registry.packages = view.catalogue ? view.catalogue->packages.size() : 0;
            if (view.checked)
                registry.checked_ago = std::max(now - *view.checked, int64_t{0});
            registry.mirrors = view.registry.player_mirrors.size();
            registry.unreachable = !view.last_error.empty();
            registry.refreshing = view.refreshing;
            for (const data::registry::RegistryKey& signing_key : descriptor.keys) {
                const base::signing::FingerprintText fingerprint =
                    base::signing::fingerprint(signing_key.key);
                const base::signing::KeyText key_text =
                    base::signing::public_key_text(signing_key.key);
                library::SigningKey shown_key;
                shown_key.fingerprint = std::string(fingerprint.view());
                shown_key.public_key = std::string(key_text.view());
                registry.keys.push_back(std::move(shown_key));
            }
            // A built-in registry that names the shipped logo shows that
            // logo, from the game's own files. A catalogue icon does not
            // replace it, and nothing is fetched.
            if (registry.built_in && descriptor.shipped_logo) {
                registry.shipped_logo = true;
                if (const library::Badge* logo = host.shipped_logo_badge())
                    registry.icon = *logo;
            } else if (view.catalogue && !view.catalogue->icon.empty() && view.served_from) {
                if (const std::optional<formats::url::Url> resolved =
                        formats::url::resolve_reference(*view.served_from, view.catalogue->icon)) {
                    registry.icon_url = formats::url::url_text(*resolved);
                    if (std::optional<library::Badge> picture =
                            host.cached_picture(descriptor.id, registry.icon_url))
                        registry.icon = std::move(*picture);
                }
            }
            inputs.registries.push_back(std::move(registry));
            if (!view.registry.enabled)
                continue;
            if (view.checked && (!newest || *view.checked > *newest))
                newest = view.checked;
            if (!view.last_error.empty() && !view.refreshing)
                inputs.offline = true;
            // The header's refresh icon asks the registries that are on and
            // can be fetched, and waits while one of them is being asked.
            if (view.status != content::RegistryStatus::needs_developer_mode) {
                inputs.can_check = true;
                inputs.checking = inputs.checking || view.refreshing;
            }
        }
        host.picture_urls.clear();
        for (const content::Entry& entry : snapshot->entries) {
            if (entry.package == nullptr)
                continue;
            const data::catalogue::Package& package = *entry.package;
            if (package.kind == data::catalogue::Kind::expansion) {
                library::DownloadableExpansion offered;
                offered.id = package.id;
                offered.registry = entry.registry;
                offered.name = package.name;
                offered.version = package.version;
                offered.file = package.file;
                offered.sha256 = package.sha256;
                offered.size = package.size;
                offered.extends = package.extends;
                inputs.downloadable_expansions.push_back(std::move(offered));
                continue;
            }
            library::Listing listing;
            listing.registry = entry.registry;
            listing.kind = kind_of(package.kind);
            listing.key = package.id;
            listing.name = package.name;
            listing.version = package.version;
            listing.revision = package.revision;
            listing.release = package.release;
            listing.publisher = package.publisher;
            listing.author = package.author;
            listing.summary = package.summary;
            listing.homepage = package.homepage;
            listing.tags = package.tags;
            listing.requires_engine = package.requires_engine_text;
            listing.base = package.requires_base;
            listing.sim_hash = package.sim_hash;
            listing.game_hacks =
                static_cast<uint32_t>(std::max(package.hacks_sim.value_or(0), int32_t{0}));
            listing.view_hacks =
                static_cast<uint32_t>(std::max(package.hacks_view.value_or(0), int32_t{0}));
            listing.hack_ids = package.hack_ids;
            listing.size = package.size;
            listing.coverage = package.coverage;
            listing.published = package.published;
            listing.supported_games = package.supported_games;
            listing.required_expansions = package.required_expansions;
            if (package.kind == data::catalogue::Kind::oamut) {
                // A mutator's base and hacks are not a mod's. Another base,
                // or a hack list, would block GET for the wrong reason.
                if (listing.base != library::supported_base)
                    listing.base.clear();
                listing.hack_ids.clear();
                listing.game_hacks = 0;
                listing.view_hacks = 0;
                listing.mutator = facts_of(package);
            }
            if (package.kind == data::catalogue::Kind::oabrain) {
                // A brain's base, hacks and expansion list are not a mod's.
                // Another base, a hack list or a required expansion would
                // block GET, or replace the row's own fit words.
                if (listing.base != library::supported_base)
                    listing.base.clear();
                listing.hack_ids.clear();
                listing.game_hacks = 0;
                listing.view_hacks = 0;
                listing.required_expansions.clear();
                listing.supported_games.clear();
                listing.brain = brain_facts_of(package);
            }
            const content::RegistryView* view = registry_view(*snapshot, entry.registry);
            for (const data::catalogue::MapEntry& map : package.maps) {
                library::MapLine line;
                line.name = map.name;
                line.players = map.players;
                line.size = std::to_string(map.width) + "x" + std::to_string(map.height);
                line.stem = map.stem;
                if (!map.preview.empty() && view != nullptr && view->served_from) {
                    if (const std::optional<formats::url::Url> resolved =
                            formats::url::resolve_reference(*view->served_from, map.preview))
                        line.preview_url = formats::url::url_text(*resolved);
                }
                if (host.previews != nullptr && !line.preview_url.empty()) {
                    const auto decoded = host.previews->pictures.find(line.preview_url);
                    if (decoded != host.previews->pictures.end())
                        line.preview = decoded->second;
                    else if (
                        host.work.pictures &&
                        host.previews->online_bytes.count(line.preview_url) == 0
                    ) {
                        if (std::optional<std::vector<uint8_t>> preview_bytes =
                                host.work.pictures->take_bytes(entry.registry, line.preview_url))
                            host.previews->online_bytes.emplace(
                                line.preview_url, std::move(*preview_bytes)
                            );
                    }
                }
                listing.maps.push_back(std::move(line));
            }
            // The badge, resolved against the host that served the
            // catalogue: shown once the worker has read and checked it.
            if (!package.badge.empty() && view != nullptr && view->served_from) {
                if (const std::optional<formats::url::Url> resolved =
                        formats::url::resolve_reference(*view->served_from, package.badge)) {
                    const std::string url = formats::url::url_text(*resolved);
                    host.picture_urls[{entry.registry, package.id}] = url;
                    if (std::optional<library::Badge> badge =
                            host.cached_picture(entry.registry, url))
                        listing.badge = std::move(*badge);
                }
            }
            inputs.listings.push_back(std::move(listing));
        }
    }
    inputs.list_age_seconds = newest ? std::max(now - *newest, int64_t{0}) : int64_t{-1};

    // What the worker last found installed, and the updates for it.
    std::vector<content::InstalledPackage> installed;
    std::vector<content::Match> matches;
    std::vector<content::Update> updates;
    std::vector<ModBadge> pictures;
    std::vector<std::optional<library::MutatorFacts>> mutator_facts;
    std::vector<std::optional<library::BrainFacts>> brain_facts;
    {
        base::threads::LockGuard guard(host.work.mutex);
        installed = host.work.installed;
        matches = host.work.matches;
        updates = host.work.updates;
        pictures = host.work.installed_badges;
        mutator_facts = host.work.installed_mutators;
        brain_facts = host.work.installed_brains;
        // searches starts at 0. One more keeps a named reading apart from
        // "the inputs do not say", which is 0.
        inputs.installed_generation = host.work.searches + 1;
    }
    for (std::size_t index = 0; index < installed.size() && index < matches.size(); ++index) {
        const content::InstalledPackage& package = installed[index];
        const content::Match& match = matches[index];
        const std::optional<library::Kind> kind = kind_of(package.kind);
        if (!kind)
            continue;
        library::Installed shown;
        shown.kind = *kind;
        const bool listed = match.provenance != content::Provenance::own;
        shown.key = listed ? match.key : package.key;
        shown.folder = package.folder;
        shown.name = package.name;
        shown.version = package.version;
        shown.revision = package.revision;
        // The manifest's author, unless it says nobody is known.
        if (package.author != kUnknownAuthor)
            shown.author = package.author;
        shown.description = package.description;
        shown.supported_games = package.supported_games;
        shown.required_expansions = package.required_expansions;
        shown.registry = listed ? match.registry : std::string{};
        shown.release = listed ? match.release : 0;
        shown.rules = package.rules;
        shown.playing = *kind == library::Kind::mod && package.kind != nullptr &&
                        package_target_in_use(*package.kind, package.folder);
        if (package.has_backup)
            shown.kept = library::Kept{package.backup_version, package.backup_revision};
        // The language pack the game shows its words from is never uninstalled.
        shown.in_use = *kind == library::Kind::language && language_pack_in_use(package.folder);
        if (index < pictures.size())
            shown.badge = badge_of(pictures[index]);
        if (index < mutator_facts.size())
            shown.mutator = std::move(mutator_facts[index]);
        if (index < brain_facts.size())
            shown.brain = std::move(brain_facts[index]);
        if (*kind == library::Kind::brain) {
            shown.required_expansions.clear();
            shown.supported_games.clear();
        }
        inputs.installed.push_back(std::move(shown));
    }
    // The mod folders the game offers, as the Library lists them beside
    // the packages above.
    {
        const EngineSettingsState& offered = engine_settings_state();
        const std::size_t count = std::min(
            {offered.mod_folders.size(), offered.mod_names.size(), offered.mod_details.size()}
        );
        // A package the installer knows keeps ROLL BACK only while the version
        // it keeps can be played, as a pick is checked.
        std::vector<std::string> listed;
        std::vector<std::string> own_keys;
        for (library::Installed& shown : inputs.installed) {
            if (shown.kind != library::Kind::mod)
                continue;
            const std::string path = kept_path(shown.folder);
            listed.push_back(path);
            // Two of the player's own copies of one mod, such as one installed
            // alongside the other, are two entries: the second's key has -2
            // after it, as below.
            if (shown.registry.empty()) {
                const std::string word = shown.key;
                for (int second = 2;
                     std::find(own_keys.begin(), own_keys.end(), shown.key) != own_keys.end();
                     ++second)
                    shown.key = word + "-" + std::to_string(second);
                own_keys.push_back(shown.key);
            }
            for (std::size_t index = 0; index < count; ++index)
                if (same_folder(offered.mod_folders[index], path) &&
                    !offered.mod_details[index].roll_back)
                    shown.kept.reset();
        }
        // Every other mod folder the game can play is the player's own: the game
        // folder's mods folder's, a folder without an oamod.yaml, a folder an
        // earlier version's Pick Folder... stored, and the one played. It is
        // keyed by its profile's id, else its folder's name, a second of the
        // same key with -2 after it.
        for (std::size_t index = 0; index < count; ++index) {
            const std::string& folder = offered.mod_folders[index];
            bool known = false;
            for (const std::string& each : listed)
                known = known || same_folder(each, folder);
            if (known)
                continue;
            const EngineSettingsState::OfferedMod& details = offered.mod_details[index];
            const fs::path path = path_from_utf8(folder);
            std::string word =
                details.profile_id.empty() ? path_to_utf8(path.filename()) : details.profile_id;
            if (word.empty())
                word = std::string(kOwnModWord);
            std::string key = word;
            for (int second = 2; std::find(own_keys.begin(), own_keys.end(), key) != own_keys.end();
                 ++second)
                key = word + "-" + std::to_string(second);
            own_keys.push_back(key);
            library::Installed shown;
            shown.kind = library::Kind::mod;
            shown.key = std::move(key);
            shown.folder = path;
            shown.name = offered.mod_names[index];
            shown.version = details.version;
            // Its oamod.yaml's description. A folder with no oamod.yaml says
            // so in its status line, and one that cannot be read says why,
            // in the language shown.
            if (details.description == mod_summary_no_profile)
                shown.without_manifest = true;
            else if (
                details.description == mod_summary_unreadable_profile ||
                details.description == mod_summary_unreadable_folder
            )
                shown.description =
                    std::string(oa::data::languages::interface_text(details.description));
            else
                shown.description = details.description;
            if (details.has_profile) {
                const package_install::InstalledPackage read =
                    package_install::oamod::read_installed_mod(path);
                if (read.kind == package_install::FolderKind::package) {
                    shown.supported_games = read.supported_games;
                    shown.required_expansions = read.required_expansions;
                }
            }
            shown.playing = package_target_in_use(package_install::mod_kind(), path);
            if (details.roll_back)
                shown.kept = library::Kept{details.kept_version, details.kept_revision};
            shown.badge = badge_of(details.badge);
            inputs.installed.push_back(std::move(shown));
        }
    }
    name_mutator_facts(inputs);
    name_brain_facts(inputs);
    for (const content::Update& update : updates) {
        if (update.installed >= installed.size())
            continue;
        const std::optional<library::Kind> kind = kind_of(installed[update.installed].kind);
        if (!kind)
            continue;
        library::UpdateNote note;
        note.registry = update.registry;
        note.kind = *kind;
        note.key = update.key;
        note.to_release = update.to_release;
        note.to_version = update.to_version;
        note.to_revision = update.to_revision;
        note.size = update.size;
        note.from_rules = update.from_rules;
        note.to_rules = update.to_rules;
        note.rules = rules_of(update.rules);
        note.blocked = blocked_of(update.blocked);
        note.blocked_detail = update.blocked_detail;
        inputs.updates.push_back(std::move(note));
    }

    // The download queue, in the order the items were queued.
    for (const content::DownloadView& queued : content_downloads().view()) {
        const std::optional<library::QueueItem::Phase> phase = phase_of(queued.state);
        if (!phase)
            continue;
        library::QueueItem item;
        item.registry = queued.target.registry;
        item.kind = kind_of(queued.target.kind);
        item.expansion = queued.target.kind == data::catalogue::Kind::expansion;
        item.key = queued.target.key;
        item.name = queued.target.name;
        item.release = queued.target.release;
        if (*phase == library::QueueItem::Phase::installed) {
            const auto remembered = host.finished_scans.find(queued.item);
            if (remembered == host.finished_scans.end())
                item.finished_scan =
                    host.finished_scans.emplace(queued.item, inputs.installed_generation)
                        .first->second;
            else
                item.finished_scan = remembered->second;
        }
        if (snapshot)
            for (const content::Entry& entry : snapshot->entries)
                if (entry.package != nullptr && entry.registry == item.registry &&
                    entry.package->id == item.key) {
                    item.version = entry.package->version;
                    break;
                }
        item.phase = *phase;
        item.done_bytes = queued.done;
        item.total_bytes = queued.target.size;
        if (*phase == library::QueueItem::Phase::failed) {
            // The reason once, in the player's language: a check the server
            // asked for names the registry, and a refusal is the installer's
            // own reason.
            item.problem = content::failure_message(queued, &oa::data::languages::interface_text);
            item.retryable = queued.failure != content::DownloadFailure::install_id_off &&
                             queued.failure != content::DownloadFailure::not_offered;
        }
        inputs.queue.push_back(std::move(item));
    }
    for (auto remembered = host.finished_scans.begin(); remembered != host.finished_scans.end();) {
        bool still_finished = false;
        for (const content::DownloadView& queued : content_downloads().view()) {
            if (queued.item == remembered->first && queued.state == content::DownloadState::done) {
                still_finished = true;
                break;
            }
        }
        if (still_finished)
            ++remembered;
        else
            remembered = host.finished_scans.erase(remembered);
    }

    inputs.engine = content::this_engine_rules().engine;
    inputs.version_text = "v" + formats::oamod::engine_version_text(inputs.engine);
    const std::string update_note = update_header_note();
    if (!update_note.empty()) {
        inputs.version_text += " · ";
        inputs.version_text += update_note;
    }
    inputs.in_match = screen_ == Screen::match || match_ != nullptr;
    inputs.mod_from_command_line = !options_.mod_dir.empty() || options_.base_game;

    // The Active tab. The mod played is the folder this start layered
    // (layered_mod_folder), with its title, version and badge as read at
    // start. An expansion bundle is not that folder, so the tile says No
    // Mod while only expansions are mounted.
    const EngineSettingsState& mods = engine_settings_state();
    if (!mods.playing_mod_folder.empty()) {
        for (std::size_t index = 0;
             index < mods.mod_folders.size() && index < mods.mod_names.size() &&
             index < mods.mod_details.size();
             ++index) {
            if (mods.mod_folders[index] != mods.playing_mod_folder)
                continue;
            const EngineSettingsState::OfferedMod& details = mods.mod_details[index];
            library::PlayingMod played;
            played.name = mods.mod_names[index];
            if (details.version != mod_summary_no_version)
                played.version = details.version;
            played.badge = badge_of(details.badge);
            inputs.playing_mod = std::move(played);
            inputs.played.id = details.profile_id;
            inputs.played.name = mods.mod_names[index];
            if (details.version != mod_summary_no_version)
                inputs.played.version = details.version;
            break;
        }
    }

    // The language shown, and the first pack that holds it in the order its
    // words are looked up: the mod's, the player's, then the engine's.
    const oa::data::languages::Language& language = shown_language();
    inputs.language.tag = std::string(language.tag);
    if (language.tag != oa::data::languages::english().tag)
        inputs.language.name = std::string(language.endonym);
    if (!language.english_name.empty() && language.english_name != language.endonym)
        inputs.language.english = std::string(language.english_name);
    if (language_) {
        for (const auto* packs :
             {&language_->mod_packs, &language_->player_packs, &language_->engine_packs}) {
            for (const auto& loaded : *packs) {
                const oa::data::languages::PackManifest& manifest = loaded->pack.manifest();
                if (oa::data::languages::find_by_tag(manifest.tag) != &language)
                    continue;
                inputs.language.pack =
                    manifest.name.empty() ? std::string(language.endonym) : manifest.name;
                inputs.language.version = manifest.version;
                if (manifest.packaging)
                    inputs.language.revision = manifest.packaging->revision;
                break;
            }
            if (!inputs.language.pack.empty())
                break;
        }
    }

    // The installed map packs' maps, as their manifests list them, one pack
    // to a folder; the packs are read once and kept until one changes.
    std::vector<std::filesystem::path> pack_folders;
    for (const PackMap& map : map_packs().maps()) {
        const auto found = std::find(pack_folders.begin(), pack_folders.end(), map.folder);
        const auto place = static_cast<std::size_t>(found - pack_folders.begin());
        if (found == pack_folders.end()) {
            pack_folders.push_back(map.folder);
            library::InstalledMaps added;
            added.name = map.pack_name;
            added.supported_games = map.supported_games;
            added.required_expansions = map.required_expansions;
            for (std::size_t matched = 0; matched < installed.size() && matched < matches.size();
                 ++matched) {
                if (matches[matched].provenance == content::Provenance::own)
                    continue;
                if (!same_folder(path_to_utf8(installed[matched].folder), path_to_utf8(map.folder)))
                    continue;
                added.key = matches[matched].key;
                added.registry = matches[matched].registry;
                break;
            }
            inputs.map_packs.push_back(std::move(added));
        }
        library::MapLine line;
        line.name = map.title.empty() ? map.stem : map.title;
        line.players = map.players;
        line.size = map.size;
        line.description = map.description;
        line.stem = map.stem;
        if (host.previews != nullptr) {
            const auto decoded = host.previews->pictures.find(installed_picture_key(map));
            if (decoded != host.previews->pictures.end())
                line.preview = decoded->second;
        }
        inputs.map_packs[place].maps.push_back(std::move(line));
    }
    inputs.registries_notice = registry_edit_notice();
    // The stored choice, read for the Active tab. A pill does not write it
    // back: the draft stays on the Library until the Library closes.
    const oa::data::content_bundles::ContentChoice choice = content_choice();
    inputs.content.base_id = choice.base_game;
    inputs.content.locked = choice.command_line;
    inputs.content.enabled = choice.expansions;
    inputs.content.copied = copied_content_bundles(options_.game_dir);
    inputs.content.downloaded = registry_content_bundles(user_folder_);
    if (const oa::data::content_bundles::Bundle* music =
            oa::data::content_bundles::find_bundle("music")) {
        const oa::app::ContentRecord music_record = oa::app::read_content_record(
            oa::app::bundle_folder(options_.game_dir, *music) /
                std::string(oa::app::content_record_name),
            nullptr
        );
        if (music_record.valid && music_record.bundle_id == music->id) {
            int32_t absent = 0;
            for (const oa::app::ContentRecordFile& item : music_record.files) {
                if (item.not_in_source)
                    ++absent;
            }
            const auto track_count = static_cast<int32_t>(music->files.size());
            if (absent > 0 && absent <= track_count) {
                inputs.content.music_track_count = track_count;
                inputs.content.music_tracks = track_count - absent;
            }
        }
    }
    if (inputs.playing_mod) {
        if (const oa::data::mod_profile::ModProfile* profile = mod_profile())
            inputs.content.mod_required = profile->required_expansions;
    }
    const auto noted = preference_values_.find(std::string(settings::key::content_note));
    if (noted != preference_values_.end() &&
        noted->second == oa::data::content_bundles::required_expansion_note) {
        inputs.content.expansion_cleared = true;
        preference_values_.erase(noted);
        preferences_dirty_ = true;
        if (const std::optional<std::string> failure = EngineSettingsState::flush(*this))
            std::cerr << "open-annihilation: library: " << *failure << '\n';
    }
    const fs::path& game_folder =
        options_.game_folders.empty() ? options_.game_dir : options_.game_folders.back();
    inputs.base_game_badge = base_game_icon(game_folder);
    inputs.default_brain = default_brain();
    return inputs;
}

namespace {

/// What asking for the demo's installer did.
enum class DemoAsk : uint8_t {
    none,      ///< nothing was named
    cancelled, ///< the player closed the dialog
    ready,     ///< a path was named
};

/// A path the demo's installer was asked for by.
struct DemoInstallerPick {
    DemoAsk ask{DemoAsk::none};
    fs::path path{};
    std::string error{};
};

#ifndef OA_NATIVE_FOLDER_DIALOG
#error "OA_NATIVE_FOLDER_DIALOG (0 or 1) says whether the build offers the system's folder dialog"
#endif

#if OA_NATIVE_FOLDER_DIALOG
/// How long a file dialog is polled, in milliseconds.
constexpr uint32_t kFileDialogPollMs = 15;

/// SDL answers inside the call on macOS and from event pumping elsewhere.
/// The fields are written before `done` is released and read after it is acquired.
struct FileWait {
    std::atomic<bool> done{false};
    bool failed = false;
    std::string path{};
    std::string error{};
};

void SDLCALL installer_chosen(void* userdata, const char* const* filelist, int) {
    auto* wait = static_cast<FileWait*>(userdata);
    if (filelist == nullptr) {
        wait->failed = true;
        const char* reason = SDL_GetError();
        if (reason != nullptr)
            wait->error = reason;
    } else if (filelist[0] != nullptr && filelist[0][0] != '\0') {
        wait->path = filelist[0];
    }
    wait->done.store(true, std::memory_order_release);
}

/// Asks for one installer file with the system's file dialog.
///
/// @param window the window the dialog belongs to
/// @return the file, a cancel, or why the dialog failed
DemoInstallerPick pick_installer_file(SDL_Window* window) {
    DemoInstallerPick pick;
    FileWait wait;
    SDL_DialogFileFilter filters[] = {
        {"Installer", "exe;bin"},
        {"All files", "*"},
    };
    // The title says which file qualifies. The filters and the title stay
    // alive through Show; SDL copies them then.
    const std::string title = std::string(
        oa::data::languages::interface_text("Choose the Total Annihilation demo installer (1997).")
    );
    const SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0) {
        const char* reason = SDL_GetError();
        if (reason != nullptr)
            pick.error = reason;
        return pick;
    }
    (void)SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
    (void)SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, filters);
    (void)SDL_SetNumberProperty(properties, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 2);
    if (window != nullptr)
        (void)SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window);
    (void)SDL_SetBooleanProperty(properties, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, false);
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFILE, installer_chosen, &wait, properties);
    SDL_DestroyProperties(properties);
    while (!wait.done.load(std::memory_order_acquire)) {
        SDL_PumpEvents();
        SDL_Delay(kFileDialogPollMs);
    }
    if (wait.failed) {
        pick.error = wait.error;
        return pick;
    }
    if (wait.path.empty()) {
        pick.ask = DemoAsk::cancelled;
        return pick;
    }
    pick.ask = DemoAsk::ready;
    pick.path = path_from_utf8(wait.path);
    return pick;
}
#endif

/// Reports whether this run must not open a file dialog.
///
/// @param unattended the command line asked for no questions
/// @param headless a check is running
/// @return true when the installer is taken from the environment
bool installer_from_environment(bool unattended, bool headless) {
    if (unattended || headless)
        return true;
    const char* driver = SDL_GetCurrentVideoDriver();
    return driver != nullptr && unattended_environment({}, driver);
}

/// Names the demo's installer: the file dialog when a display can show it,
/// otherwise `OA_DEMO_INSTALLER`.
///
/// @param window the window the dialog belongs to
/// @param unattended the command line asked for no questions
/// @param headless a check is running
/// @return the installer, a cancel, or nothing
DemoInstallerPick ask_demo_installer(SDL_Window* window, bool unattended, bool headless) {
    if (!installer_from_environment(unattended, headless)) {
#if OA_NATIVE_FOLDER_DIALOG
        const DemoInstallerPick picked = pick_installer_file(window);
        if (picked.ask == DemoAsk::cancelled || picked.ask == DemoAsk::ready)
            return picked;
#else
        (void)window;
#endif
    }
    DemoInstallerPick pick;
    if (const auto named = oa::platform::environment_value("OA_DEMO_INSTALLER");
        named && !named->empty()) {
        pick.ask = DemoAsk::ready;
        pick.path = path_from_utf8(*named);
        return pick;
    }
    return pick;
}

} // namespace

// ---------------------------------------------------------------------------------------------
// The Library as a screen of the layer

/// The Library as a screen of the OA layer: modal, over a backdrop, centred
/// in the window at its size class's size on the main menu alone. Its model
/// and state are the host's; it lays them out at the frame the layer gives
/// it and hands what its input asks for to the runtime.
struct Runtime::LibraryScreen final : LayerScreen {
    /// Makes the screen.
    ///
    /// @param runtime the runtime whose Library it shows
    explicit LibraryScreen(Runtime& runtime) noexcept : runtime_(runtime) {}

    /// Returns "library".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return library_screen_name; }

    /// Returns where the Library shows: centred in the window at the view's
    /// scale, its size class's window size (library::window_size).
    ///
    /// @param view what the screen is placed on
    /// @return its place; empty in a match or off the main menu
    [[nodiscard]] LayerPlacement placement(const LayerView& view) const override {
        if (view.match || runtime_.screen_ != Screen::main_menu)
            return {};
        const kit::Point size = library::window_size(view.frame);
        return centred_placement(view, size.x, size.y);
    }

    /// Tells that the Library takes every input.
    ///
    /// @return true
    [[nodiscard]] bool modal() const override { return true; }

    /// Tells that what lies under the Library darkens.
    ///
    /// @return true
    [[nodiscard]] bool backdrop() const override { return true; }

    /// Lays the Library out again when the view's frame changed.
    ///
    /// @param view what the screen is placed on
    void lay_out(const LayerView& view) override {
        if (laid_out_ && view.frame.area.width == frame_.area.width &&
            view.frame.area.height == frame_.area.height &&
            view.frame.size_class == frame_.size_class &&
            view.frame.scale_percent == frame_.scale_percent)
            return;
        frame_ = view.frame;
        laid_out_ = true;
        relayout();
    }

    /// Draws the Library at the canvas's scale. The layer's canvas carries
    /// the app icon; the Library's header draws the mods icon.
    ///
    /// @param canvas where it draws
    void draw(const kit::Canvas& canvas) const override {
        // A rebuild in this frame's tick_library replaced the model's inputs,
        // and with them the badges and icons the last layout points into.
        // Lay out again before drawing, so nothing freed is read.
        if (runtime_.library_ && runtime_.library_->model_revision != seen_model_)
            const_cast<LibraryScreen*>(this)->relayout();
        kit::Canvas drawn = canvas;
        drawn.icon = Runtime::EngineSettingsMenuHost::mods_icon(runtime_);
        library::draw_library(drawn, list_);
        runtime_.finish_screen_change("library");
    }

    /// Takes a pointer's move, press or release.
    ///
    /// @param kind pointer_move, pointer_down or pointer_up
    /// @param button the pointer's button
    /// @param at the pointer, in the Library's points
    /// @param finger_reach a finger's reach in the Library's points; 0 for a mouse
    /// @return what the Library did
    LayerAnswer
    pointer(ScreenInputKind kind, uint8_t button, kit::Point at, int32_t finger_reach) override {
        library::PointerKind moved = library::PointerKind::move;
        if (kind == ScreenInputKind::pointer_down)
            moved = library::PointerKind::down;
        else if (kind == ScreenInputKind::pointer_up)
            moved = library::PointerKind::up;
        else if (kind != ScreenInputKind::pointer_move)
            return LayerAnswer::none;
        LibraryHost& host = runtime_.library_host();
        return take(
            library::library_pointer(host.model, host.state, list_, moved, button, at, finger_reach)
        );
    }

    /// Takes a key, as the kit names it.
    ///
    /// @param pressed the key
    /// @return what the Library did
    LayerAnswer key(kit::Key pressed, uint32_t /*sdl_key*/) override {
        LibraryHost& host = runtime_.library_host();
        return take(library::library_key(host.model, host.state, list_, pressed));
    }

    /// Takes the Library's own keys before the kit's: Cmd+F on macOS and
    /// Ctrl+F elsewhere, and `/` outside the search, find; Ctrl+Tab and
    /// Ctrl+Shift+Tab move between the tabs; F5 presses the header's
    /// refresh icon; the settings' shortcut opens Settings over the Library.
    ///
    /// @param sdl_key the key's SDL keycode
    /// @param modifiers SDL_Keymod bits
    /// @return what the Library did; pass for every other key
    LayerAnswer shortcut(uint32_t sdl_key, uint16_t modifiers) override {
        if (Runtime::engine_settings_shortcut(sdl_key, modifiers))
            return take({.action = library::Action::settings, .redraw = true});
        LibraryHost& host = runtime_.library_host();
        if (sdl_key == SDLK_F5) {
            if (!library::can_check_now(host.model))
                return LayerAnswer::none;
            return take({.action = library::Action::check_now, .redraw = true});
        }
        std::optional<library::ScreenCommand> command;
        if (sdl_key == SDLK_F && (modifiers & kCommandModifier) != 0)
            command = library::ScreenCommand::find;
        else if (sdl_key == SDLK_TAB && (modifiers & SDL_KMOD_CTRL) != 0)
            command = (modifiers & SDL_KMOD_SHIFT) != 0 ? library::ScreenCommand::previous_tab
                                                        : library::ScreenCommand::next_tab;
        else if (
            sdl_key == SDLK_SLASH && (modifiers & (SDL_KMOD_CTRL | SDL_KMOD_GUI)) == 0 &&
            !library::focused_field(host.state, list_)
        )
            command = library::ScreenCommand::find;
        if (!command)
            return LayerAnswer::pass;
        return take(library::library_command(host.model, host.state, list_, *command));
    }

    /// Takes the controller's buttons that are the Library's own: Y focuses
    /// the search, RB and LB change tabs, B goes back and X does the selected
    /// entry's first action. Every other action is left to become a key, as
    /// on any other screen.
    ///
    /// @param action the gamepad action
    /// @return what the Library did; pass for an action it does not take
    LayerAnswer pad_action(oa::ui::pad_controls::Action action) override {
        std::optional<library::ScreenCommand> command;
        switch (action) {
        case oa::ui::pad_controls::Action::library_search:
            command = library::ScreenCommand::find;
            break;
        case oa::ui::pad_controls::Action::focus_next:
            command = library::ScreenCommand::next_tab;
            break;
        case oa::ui::pad_controls::Action::focus_previous:
            command = library::ScreenCommand::previous_tab;
            break;
        case oa::ui::pad_controls::Action::back:
            command = library::ScreenCommand::back;
            break;
        case oa::ui::pad_controls::Action::library_action:
            command = library::ScreenCommand::first_action;
            break;
        default:
            return LayerAnswer::pass;
        }
        LibraryHost& host = runtime_.library_host();
        return take(library::library_command(host.model, host.state, list_, *command));
    }

    /// Takes the controller's right stick: it scrolls the pane that has the
    /// focus, in notches as the wheel.
    ///
    /// @param notches the stick's turn; positive towards the top
    /// @return what the Library did
    LayerAnswer pad_stick(float notches) override {
        LibraryHost& host = runtime_.library_host();
        return take(library::library_stick(host.model, host.state, list_, notches));
    }

    /// Takes a turn of the wheel, which scrolls the pane under the pointer.
    ///
    /// @param at the pointer, in the Library's points
    /// @param notches how far the wheel turned
    /// @return what the Library did
    LayerAnswer wheel(kit::Point at, float notches) override {
        LibraryHost& host = runtime_.library_host();
        return take(library::library_wheel(host.model, host.state, list_, at, notches));
    }

    /// Takes text typed into the search.
    ///
    /// @param utf8 the text
    /// @return what the Library did
    LayerAnswer text(std::string_view utf8) override {
        LibraryHost& host = runtime_.library_host();
        return take(library::library_text(host.model, host.state, list_, utf8));
    }

    /// Returns the search field while it has the focus.
    ///
    /// @return the field's rectangle, in the Library's points; none otherwise
    [[nodiscard]] std::optional<kit::Rect> text_field() const override {
        if (!runtime_.library_)
            return std::nullopt;
        return library::focused_field(runtime_.library_->state, list_);
    }

    /// Returns what the Library draws and where its controls are.
    ///
    /// @return the display list
    [[nodiscard]] kit::DisplayList display_list() const override { return list_; }

    /// Returns the Library's hover, press and focus.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override {
        if (!runtime_.library_)
            return {};
        return runtime_.library_->state.interaction;
    }

    /// Follows the model as the host rebuilds it, turns the refresh marks
    /// while a refresh runs, holds a mark's check after a list was read, and
    /// closes off the main menu.
    ///
    /// The turn and the check follow the time since the previous tick, not
    /// the frame count. A tick whose marks would draw the same picture does
    /// not lay the screen out again.
    ///
    /// @return close off the main menu; redraw when the model moved or a
    ///     mark changed; otherwise none
    LayerAnswer tick() override {
        if (runtime_.screen_ != Screen::main_menu)
            return LayerAnswer::close;
        bool moved = false;
        if (runtime_.library_) {
            LibraryHost& host = *runtime_.library_;
            // A refresh that closes the model's details closes the page and
            // its drop-down too, so the list the player then sees is the list
            // Escape closes.
            if (!host.model.details_open && host.state.details_page) {
                host.state.details_page = false;
                host.state.menu = library::MenuKind::none;
                moved = true;
            }
            const auto now = static_cast<uint32_t>(SDL_GetTicks());
            uint32_t elapsed = 0;
            if (refresh_clock_started_)
                elapsed = now - refresh_clock_ms_;
            refresh_clock_ms_ = now;
            refresh_clock_started_ = true;
            const std::vector<library::Registry>& registries = host.model.inputs.registries;
            std::vector<library::RegistryPulse> pulses;
            pulses.reserve(registries.size());
            for (const library::Registry& registry : registries)
                pulses.push_back({registry.id, registry.refreshing, registry.unreachable});
            const bool marks_moved = library::step_refresh_marks(
                host.state, host.model.inputs.checking, pulses, elapsed
            );
            // The page closed above lays the screen out again too.
            moved = moved || marks_moved;
        }
        if (!runtime_.library_ || (runtime_.library_->model_revision == seen_model_ && !moved))
            return LayerAnswer::none;
        relayout();
        return LayerAnswer::redraw;
    }

    /// Returns the Library's revision, which counts its layouts.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Forgets the pictures the Library waits for and any drag or open menu.
    /// A pending base game or expansion choice, and a mod the Active tab
    /// unloads, are stored now, and the files are mounted again on the next
    /// start. A match, and a choice the command line named, leave the draft
    /// where it is.
    void close(bool /*by_key*/) override {
        apply_pending_content();
        if (!runtime_.library_)
            return;
        LibraryHost& host = *runtime_.library_;
        host.state.drag = {};
        host.state.tab_drag = {};
        host.state.menu = library::MenuKind::none;
        if (host.work.pictures)
            host.work.pictures->drop_all();
    }

  private:

    /// Stores a pending base game or expansion choice, clears the mod the
    /// Active tab unloads, and asks for one soft restart. The draft is edited
    /// whenever the tile shows anything other than the stored choice and the
    /// mod played, an unload alone included, so this applies what the tile
    /// shows. A second close finds the draft already applied. A match, and
    /// `--content`, leave the draft edited and mount nothing.
    void apply_pending_content() {
        if (runtime_.library_ == nullptr)
            return;
        LibraryHost& host = *runtime_.library_;
        library::ContentDraft& draft = host.model.content;
        if (!draft.edited || draft.applied)
            return;
        if (runtime_.screen_ == Screen::match || runtime_.match_ != nullptr)
            return;
        if (!runtime_.options_.content.empty())
            return;
        namespace bundles = oa::data::content_bundles;
        bundles::ContentChangeResult result;
        result.applied = true;
        result.choice.base_game = draft.base_game.empty() ? draft.seed_base : draft.base_game;
        result.choice.expansions = draft.expansions;
        const bool command_owns_mod =
            !runtime_.options_.mod_dir.empty() || runtime_.options_.base_game;
        if (!command_owns_mod) {
            if (draft.clear_reason == library::PendingClear::base_game_chosen)
                result.mod_cleared = bundles::ModClearReason::base_game_chosen;
            else if (draft.clear_reason == library::PendingClear::required_expansion_off)
                result.mod_cleared = bundles::ModClearReason::required_expansion_off;
        }
        draft.applied = true;
        draft.edited = false;
        if (!store_content_change(runtime_.preference_values_, result))
            return;
        if (result.mod_cleared != bundles::ModClearReason::none && runtime_.engine_settings_) {
            runtime_.engine_settings_->current.mod_folder.clear();
            runtime_.engine_settings_->dropped_mod_folder.clear();
        }
        runtime_.preferences_dirty_ = true;
        if (const auto failure = Runtime::EngineSettingsState::flush(runtime_))
            Runtime::EngineSettingsState::report_failed_save(runtime_, *failure);
        runtime_.request_soft_restart();
    }

    /// Copies the base game `id` from the source the player names, then marks
    /// it on the pending choice. The files stay as they are until the Library
    /// closes. A cancel leaves the chooser open.
    ///
    /// @param id the base game that is not copied yet
    void copy_base_source(const std::string& id) {
        if (id.empty() || runtime_.library_ == nullptr)
            return;
        LibraryHost& host = *runtime_.library_;
        if (!host.model.chooser.open)
            return;
        CopyRequest request;
        request.user_folder = runtime_.user_folder_;
        request.bundles = oa::data::content_bundles::bundles();
        request.news = &std::cerr;
        CopyResult copied;
        const auto copy_named = [&](ContentSource& source) {
            runtime_.run_progress("Applying your changes", [&](LongWork& work) {
                request.on_progress = [](void* context, const CopyProgress& copy_progress) {
                    auto& held = *static_cast<LongWork*>(context);
                    if (copy_progress.files_total == 0) {
                        held.done.store(-1, std::memory_order_release);
                        return;
                    }
                    const int32_t whole =
                        copy_progress.files_total >
                                static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
                            ? std::numeric_limits<int32_t>::max()
                            : static_cast<int32_t>(copy_progress.files_total);
                    int32_t finished_files =
                        copy_progress.files_done >
                                static_cast<uint32_t>(std::numeric_limits<int32_t>::max())
                            ? whole
                            : static_cast<int32_t>(copy_progress.files_done);
                    if (finished_files > whole)
                        finished_files = whole;
                    held.whole.store(whole, std::memory_order_release);
                    held.done.store(finished_files, std::memory_order_release);
                };
                request.progress_context = &work;
                copied = copy_content(source, request);
            });
        };
        if (id == "totala-demo") {
            const DemoInstallerPick pick = ask_demo_installer(
                runtime_.sdl_.window, runtime_.options_.unattended, runtime_.options_.headless_check
            );
            if (pick.ask == DemoAsk::cancelled)
                return;
            if (pick.ask != DemoAsk::ready) {
                const std::string_view problem = pick.error.empty()
                                                     ? library::words::base_copy_failed
                                                     : std::string_view{pick.error};
                library::note_base_copy_problem(host.model, problem);
                return;
            }
            DemoInstallerSource source(pick.path);
            copy_named(source);
        } else {
            fs::path chosen;
            std::string dialog_error;
            const FolderPick picked = pick_game_folder_with_dialog({}, &chosen, &dialog_error);
            if (picked == FolderPick::cancelled)
                return;
            if (picked != FolderPick::chosen) {
                library::note_base_copy_problem(
                    host.model,
                    dialog_error.empty() ? library::words::base_copy_failed
                                         : std::string_view{dialog_error}
                );
                return;
            }
            FolderContentSource source(std::move(chosen), request.bundles);
            copy_named(source);
        }
        if (copied.status != CopyStatus::finished) {
            library::note_base_copy_problem(
                host.model,
                copied.error.empty() ? library::words::base_copy_failed
                                     : std::string_view{copied.error}
            );
            return;
        }
        // The copy is in the player's folder. A rebuild reads that. The
        // chooser stays open through it while the stored choice is the same,
        // and is opened again when a different stored choice closed it. The
        // game just copied is then marked and used. The files stay as they
        // are until the Library closes.
        host.rebuild(runtime_);
        if (!host.model.chooser.open) {
            const library::BaseOpen opened = library::open_base_chooser(host.model);
            if (opened.opened) {
                host.state.interaction.focused = opened.focus;
                host.state.interaction.focus_shown = opened.focus != kit::no_control;
            }
        }
        library::mark_base_game(host.model, id);
        const library::BaseUse used = library::use_marked_base_game(host.model);
        if (used.kind == library::BaseUseKind::needs_source)
            library::note_base_copy_problem(host.model, library::words::base_copy_failed);
    }

    /// Lays the Library out from the host's model and state, and asks for the
    /// pictures of the rows it shows.
    void relayout() {
        ++revision_;
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        if (fonts == nullptr || !runtime_.library_) {
            list_ = {};
            return;
        }
        LibraryHost& host = *runtime_.library_;
        seen_model_ = host.model_revision;
        list_ = library::library_layout(host.model, host.state, frame_, *fonts);
        host.request_pictures(list_);
        host.pump_map_previews(runtime_, list_);
    }

    /// Carries out what an input asked for, and lays the Library out again
    /// when what it shows changed.
    ///
    /// @param result what the input asked for
    /// @return close when the Library closes; redraw when it changed; otherwise none
    LayerAnswer take(const library::ScreenResult& result) {
        bool closes = false;
        if (result.action)
            closes = act(result);
        if (closes)
            return LayerAnswer::close;
        if (!result.redraw && !result.action)
            return LayerAnswer::none;
        relayout();
        return LayerAnswer::redraw;
    }

    /// Forgets what the installer noted of an entry's earlier try, as the
    /// player asks for it again.
    ///
    /// @param id the entry
    void forget_note(const library::EntryId& id) {
        if (runtime_.mod_install_state_)
            runtime_.mod_install_state_->note_for_library(runtime_, {id.registry, id.key, {}, {}});
    }

    /// Drops the held update CANCEL was pressed for.
    ///
    /// The staged files are settled away, the mod being played stays as it
    /// is, and the queue leaves the line out so the catalogue's update shows
    /// again. A roll back waiting in that update's place is left waiting, and
    /// a download that has not ended is left for the feed to cancel.
    ///
    /// @param entry the entry whose queue line was cancelled
    void drop_held_update(const library::Entry& entry) {
        if (entry.queued == nullptr || entry.queued->phase != library::QueueItem::Phase::held)
            return;
        if (runtime_.mod_install_state_ == nullptr)
            return;
        const std::string* held_folder = nullptr;
        for (const ModInstallState::LibraryNote& note :
             runtime_.mod_install_state_->library_notes) {
            if (note.registry == entry.id.registry && note.key == entry.id.key &&
                !note.held_target.empty())
                held_folder = &note.held_target;
        }
        if (held_folder == nullptr)
            return;
        const std::optional<std::string> waiting = package_install::pending_change_target();
        if (!waiting || *waiting != *held_folder)
            return;
        if (!package_install::discard_pending_change())
            return;
        runtime_.mod_install_state_->discarder.add(package_install::take_discards());
        ModInstallState::LibraryNote dropped{entry.id.registry, entry.id.key, {}, {}};
        dropped.cancelled = true;
        runtime_.mod_install_state_->note_for_library(runtime_, std::move(dropped));
    }

    /// Queues one entry's GET through the feed; what the installer noted of
    /// an earlier try is forgotten.
    ///
    /// @param host the host
    /// @param entry the entry
    void get(LibraryHost& host, const library::Entry& entry) {
        if (host.feed.enqueue == nullptr || entry.id.registry.empty())
            return;
        forget_note(entry.id);
        std::string why;
        const std::optional<uint64_t> item = host.feed.enqueue(
            host.feed.context,
            entry.id.registry,
            entry.id.key,
            content::DownloadReason::install,
            &why
        );
        if (!item && !why.empty()) {
            runtime_.status_ = why;
            std::cerr << "open-annihilation: library: " << why << '\n';
        }
    }

    /// Asks GET WITH, then queues every step of the plan, prerequisites first.
    ///
    /// CANCEL queues nothing. GET BOTH or GET ALL stops at the first refusal
    /// and leaves that reason on the question; the steps already queued stay
    /// queued. Nothing is queued before the player answers.
    ///
    /// @param host the host
    /// @param entry the mutator
    void get_with(LibraryHost& host, const library::Entry& entry) {
        const library::GetPlan plan = library::get_plan(host.model, entry);
        if (!plan.blocked.empty() || plan.steps.size() < 2)
            return;
        oa::ui::kit::Question asked = library::get_with_question(host.model, entry);
        const std::string go_id(
            plan.steps.size() == 2 ? library::get_with_both_id : library::get_with_all_id
        );
        int32_t go = -1;
        for (std::size_t place = 0; place < asked.buttons.size(); ++place) {
            if (asked.buttons[place].id == go_id)
                go = static_cast<int32_t>(place);
        }

        struct Step {
            std::string registry;
            std::string key;
        };

        std::vector<Step> steps;
        steps.reserve(plan.steps.size());
        for (const library::GetStep& step : plan.steps)
            steps.push_back(Step{step.id.registry, step.id.key});
        QuestionScreen::Host answers;
        answers.answer = [this, steps, go](int32_t button) {
            if (button != go)
                return true;
            LibraryHost& feed_host = runtime_.library_host();
            auto* raw = dynamic_cast<QuestionScreen*>(runtime_.oa_layer().top());
            for (const Step& step : steps) {
                std::string why;
                if (feed_host.feed.enqueue == nullptr || step.registry.empty()) {
                    if (raw != nullptr) {
                        raw->question().failure = why;
                        raw->changed();
                    }
                    return false;
                }
                forget_note(library::EntryId{step.registry, library::Kind::mutator, step.key});
                const std::optional<uint64_t> item = feed_host.feed.enqueue(
                    feed_host.feed.context,
                    step.registry,
                    step.key,
                    content::DownloadReason::install,
                    &why
                );
                if (!item) {
                    if (raw != nullptr) {
                        raw->question().failure = why;
                        raw->changed();
                    }
                    return false;
                }
            }
            return true;
        };
        answers.tick = [this] { return runtime_.screen_ != Screen::main_menu; };
        OaLayer& layer = runtime_.oa_layer();
        layer.push(
            std::make_unique<QuestionScreen>(layer, Screen::main_menu, std::move(asked), answers)
        );
    }

    /// Opens Settings over the Library, or, where Settings is under it, goes
    /// back to Settings: the layer never holds two of either.
    void open_settings() {
        OaLayer& layer = runtime_.oa_layer();
        if (layer.find(kSettingsScreen) != nullptr) {
            layer.return_to(kSettingsScreen);
            return;
        }
        runtime_.open_engine_settings_from_menu();
    }

    /// Switches the game's language the way Settings › Language does, and
    /// leaves the Library open in that language.
    ///
    /// @param entry the built-in language, or the installed language pack
    void use_language_now(const library::Entry& entry) {
        std::string tag;
        if (library::is_built_in_language(entry.id))
            tag = std::string(library::built_in_language_tag(entry.id));
        else if (entry.id.kind == library::Kind::language && entry.installed != nullptr)
            tag = entry.id.key;
        else
            return;
        if (!library::package_can_run(runtime_.library_host().model, entry))
            return;
        const oa::data::languages::Language* language = oa::data::languages::find_by_tag(tag);
        if (language == nullptr || !oa::data::languages::playable(*language))
            return;
        auto chosen = runtime_.engine_settings();
        const auto before = chosen;
        chosen.language = std::string(language->tag);
        if (language->needs == oa::data::languages::TextNeeds::modern_fonts)
            chosen.modern_fonts = true;
        runtime_.apply_engine_settings(chosen);
        if (const std::optional<std::string> failure =
                runtime_.save_engine_settings(before, chosen, false))
            runtime_.status_ = *failure;
        runtime_.library_host().rebuild(runtime_);
    }

    /// Downloads, cancels or retries an expansion from its pill's arrow.
    ///
    /// The package is copied into the game's folder. The pill turns on once
    /// that copy is read, on the pending choice. The game mounts the choice
    /// when the Library closes, not while the Library stays open. A refused
    /// GET forgets the ask, so the arrow is GET again.
    ///
    /// @param result the press; `expansion` is the bundle id and `registry` the offer
    void download_expansion(const library::ScreenResult& result) {
        LibraryHost& host = runtime_.library_host();
        const library::Action action = *result.action;
        const std::string& expansion_id = result.expansion;
        const std::string& registry_id = result.registry;
        if (action == library::Action::get) {
            if (host.feed.enqueue == nullptr || registry_id.empty()) {
                library::drop_expansion_download(host.model, expansion_id);
                ++host.model_revision;
                return;
            }
            std::string why;
            const std::optional<uint64_t> item = host.feed.enqueue(
                host.feed.context, registry_id, expansion_id, content::DownloadReason::install, &why
            );
            if (!item) {
                library::drop_expansion_download(host.model, expansion_id);
                if (!why.empty()) {
                    runtime_.status_ = why;
                    std::cerr << "open-annihilation: library: " << why << '\n';
                }
                ++host.model_revision;
            }
            return;
        }
        if (action == library::Action::cancel) {
            if (host.feed.cancel != nullptr && !registry_id.empty())
                host.feed.cancel(host.feed.context, registry_id, expansion_id);
            return;
        }
        if (action == library::Action::retry && host.feed.retry != nullptr && !registry_id.empty())
            host.feed.retry(host.feed.context, registry_id, expansion_id);
    }

    /// Carries out one of the Library's actions.
    ///
    /// @param result what the input asked for
    /// @return true when the Library closes
    bool act(const library::ScreenResult& result) {
        const library::Action action = *result.action;
        const library::EntryId& id = result.entry;
        LibraryHost& host = runtime_.library_host();
        if (!result.expansion.empty() &&
            (action == library::Action::get || action == library::Action::cancel ||
             action == library::Action::retry)) {
            download_expansion(result);
            return false;
        }
        const library::Entry* entry = library::find_entry(host.model, id);
        switch (action) {
        case library::Action::get:
            if (entry != nullptr)
                get(host, *entry);
            return false;
        case library::Action::get_with:
            if (entry != nullptr)
                get_with(host, *entry);
            return false;
        case library::Action::cancel:
            if (entry != nullptr) {
                drop_held_update(*entry);
                if (host.feed.cancel != nullptr)
                    host.feed.cancel(host.feed.context, id.registry, id.key);
            }
            return false;
        case library::Action::retry:
            if (entry != nullptr && host.feed.retry != nullptr) {
                forget_note(id);
                host.feed.retry(host.feed.context, id.registry, id.key);
            }
            return false;
        case library::Action::update:
        case library::Action::update_all:
        case library::Action::roll_back:
        case library::Action::play_now:
        case library::Action::switch_off:
        case library::Action::check_now:
            runtime_.library_entry_action(action, id);
            return false;
        case library::Action::open_folder:
            if (entry != nullptr && entry->installed != nullptr) {
                const FolderOpening opening = runtime_.open_player_folder(entry->installed->folder);
                if (!opening.opened)
                    runtime_.status_ = opening.reason;
            }
            return false;
        case library::Action::homepage:
            if (entry != nullptr && entry->listing != nullptr && !entry->listing->homepage.empty())
                runtime_.open_web_link(entry->listing->homepage);
            return false;
        case library::Action::settings:
            open_settings();
            return false;
        case library::Action::close:
            runtime_.play_ui_sound(kCloseSound, 0);
            return true;
        case library::Action::details:
        case library::Action::back:
            return false;
        case library::Action::add_registry:
            runtime_.open_add_registry(std::nullopt);
            return false;
        case library::Action::add_mirror:
            runtime_.open_add_mirror(result.registry);
            return false;
        case library::Action::remove_registry:
            runtime_.remove_library_registry(result.registry);
            return false;
        case library::Action::set_registry:
            runtime_.change_library_registry(result.registry, result.registry_on);
            return false;
        case library::Action::refresh_registry:
            // One registry: its catalogue. The game does not ask for download counts.
            runtime_.content_service().refresh(content::RefreshReason::check_now, result.registry);
            return false;
        case library::Action::uninstall:
            runtime_.library_entry_action(action, id);
            return false;
        case library::Action::use_now:
            if (entry != nullptr)
                use_language_now(*entry);
            return false;
        case library::Action::change_base_game: {
            const library::BaseOpen opened = library::open_base_chooser(host.model);
            if (opened.opened) {
                host.state.interaction.focused = opened.focus;
                host.state.interaction.focus_shown = opened.focus != kit::no_control;
                ++host.model_revision;
            }
            return false;
        }
        case library::Action::copy_base_game:
            copy_base_source(result.base_game);
            ++host.model_revision;
            return false;
        case library::Action::turn_on:
            if (entry != nullptr)
                library::turn_on_required(host.model, *entry);
            ++host.model_revision;
            return false;
        case library::Action::check_with_mod:
            if (entry != nullptr) {
                if (entry->id.kind == library::Kind::brain)
                    host.begin_brain_check(runtime_, entry->id.key);
                else
                    host.begin_mutator_check(runtime_, entry->id.key);
            }
            return false;
        case library::Action::use_by_default:
            if (entry != nullptr) {
                const std::string_view chosen =
                    library::is_built_in_brain(entry->id) ? std::string_view{} : entry->id.key;
                runtime_.set_default_brain(chosen);
                host.rebuild(runtime_);
            }
            return false;
        case library::Action::add_expansion:
            // G22 opens the locator. This press does not.
            return false;
        case library::Action::ask_expansion_off: {
            oa::ui::kit::Question asked = library::expansion_off_question(host.model);
            int32_t turn_off = -1;
            for (int32_t index = 0; index < static_cast<int32_t>(asked.buttons.size()); ++index) {
                if (asked.buttons[static_cast<std::size_t>(index)].id ==
                    library::turn_off_button_id)
                    turn_off = index;
            }
            QuestionScreen::Host answers;
            // The host outlives the question: it is the runtime's Library. The
            // question can tick once more after this screen has gone, so it
            // holds the runtime, not the screen.
            answers.answer = [&host, turn_off](int32_t button) {
                if (button == turn_off)
                    library::accept_expansion_off(host.model);
                else
                    library::cancel_expansion_off(host.model);
                ++host.model_revision;
                return true;
            };
            // The question goes with the main menu, as the Library does. A
            // catalogue or a picture that arrives keeps it on the draft, and a
            // different stored choice drops it there: then it closes, since
            // its answer would change nothing.
            answers.tick = [&runtime = runtime_, &host] {
                return runtime.screen_ != Screen::main_menu || !host.model.content.asking;
            };
            // A question that leaves unanswered, as when the main menu goes or
            // the Library is opened again over it, leaves the draft as CANCEL
            // does, so the pills and CHANGE… take presses again.
            answers.closed = [&host](const QuestionScreen&) {
                if (!host.model.content.asking)
                    return;
                library::cancel_expansion_off(host.model);
                ++host.model_revision;
            };
            OaLayer& layer = runtime_.oa_layer();
            layer.push(
                std::make_unique<QuestionScreen>(
                    layer, Screen::main_menu, std::move(asked), answers
                )
            );
            return false;
        }
        }
        return false;
    }

    Runtime& runtime_;             ///< the runtime whose Library it shows
    kit::DisplayList list_{};      ///< what it draws and where its controls are
    kit::Frame frame_{};           ///< the frame it was laid out for
    bool laid_out_{};              ///< frame_ was set by lay_out
    uint64_t seen_model_{};        ///< the host's model revision the list was laid out from
    uint64_t revision_{};          ///< counts its layouts
    uint32_t refresh_clock_ms_{};  ///< the previous tick's time, in milliseconds
    bool refresh_clock_started_{}; ///< refresh_clock_ms_ has been set
};

// ---------------------------------------------------------------------------------------------
// Opening and closing the Library

void Runtime::open_library(std::optional<library::Tab> tab) {
    // Nothing opens over a message box or a frame a package owns, off the
    // main menu, or while the saves notice shows.
    if (screen_ != Screen::main_menu || frame_owned_by_package() ||
        oa::ui::frontend_dialogs::dialog_count() != 0 || saves_notice_shown() ||
        engine_settings_fonts() == nullptr)
        return;
    begin_screen_change("library");
    LibraryHost& host = library_host();
    // The catalogues are asked for again as the player's setting allows; a
    // run nobody watches fetches nothing.
    if (host.own_feed && !options_.unattended && !options_.headless_check)
        content_service().refresh(content::RefreshReason::library_opened);
    if (!host.refreshed) {
        host.feed_generation =
            host.feed.generation != nullptr ? host.feed.generation(host.feed.context) : 0;
        host.rebuild(*this);
    }
    const library::Tab shown = tab ? *tab : library::Tab::active;
    OaLayer& layer = oa_layer();
    if (layer.find(library_screen_name) != nullptr) {
        // The screens above it close, and it shows the tab asked for.
        layer.return_to(library_screen_name);
        host.state.details_page = false;
        library::close_details(host.model);
        library::set_tab(host.model, shown);
    } else {
        host.state = {};
        library::set_query(host.model, {});
        library::close_details(host.model);
        library::set_tab(host.model, shown);
        layer.push(std::make_unique<LibraryScreen>(*this));
    }
    ++host.model_revision;
    EngineSettingsMenuHost& button = engine_settings_menu_host();
    button.button_hovered = false;
    button.button_pressed = false;
    play_ui_sound(kOpenSound, 0);
}

void Runtime::close_library() {
    OaLayer& layer = oa_layer();
    const LayerScreen* shown = layer.find(library_screen_name);
    if (shown == nullptr)
        return;
    begin_screen_change("main menu");
    layer.return_to(library_screen_name);
    layer.close(shown);
}

std::size_t Runtime::library_update_count() {
    return library_ ? library_->update_count : 0;
}

bool Runtime::library_registry_built_in(std::string_view registry) {
    const LibraryHost& host = library_host();
    return host.feed.built_in != nullptr && host.feed.built_in(host.feed.context, registry);
}

void Runtime::tick_library() {
    LibraryHost& host = library_host();
    const bool in_match = screen_ == Screen::match || match_ != nullptr;
    {
        base::threads::LockGuard guard(host.work.mutex);
        const bool was_running = host.work.match_running;
        host.work.match_running = in_match;
        // A match cancels a check the way it stops a picture fetch. The
        // running work sees check_cancel; a check that has not started is
        // recorded as cancelled without running, and one that was waiting
        // is dropped.
        if (in_match) {
            host.work.check_cancel.store(true);
            host.work.checks_waiting.clear();
            host.work.wake.notify_all();
        }
        // Pictures held for a match are fetched once it ends.
        if (was_running && !in_match && host.work.pictures && host.work.pictures->waiting()) {
            host.work.pictures_due = true;
            host.work.wake.notify_all();
        }
    }
    // A check that ended is kept, and a waiting one started, before the
    // rebuild that shows the verdict, even when the feed's generation has
    // not moved. The picker asks for checks over SKIRMISH, NEWGAME and the
    // battle room too, so this runs on every screen but a match's; the
    // Library's model follows the next time the main menu shows.
    bool check_ended = false;
    if (!in_match && screen_ != Screen::loading) {
        check_ended = host.finish_mutator_check();
        if (check_ended && screen_ != Screen::main_menu)
            host.refreshed = false;
    }
    // The Library and its dot are the main menu's.
    if (screen_ != Screen::main_menu)
        return;
    if (host.own_feed) {
        // A catalogue that moved on, or a package the queue installed,
        // sends the worker to search for updates again.
        const uint64_t service = content_service().generation();
        if (service != host.service_generation) {
            host.service_generation = service;
            host.look_again(*this);
        }
        const uint64_t queue = content_downloads().generation();
        if (queue != host.queue_generation) {
            host.queue_generation = queue;
            bool installed = false;
            for (const content::DownloadView& queued : content_downloads().view()) {
                if (queued.state != content::DownloadState::done ||
                    std::find(
                        host.installed_items.begin(), host.installed_items.end(), queued.item
                    ) != host.installed_items.end())
                    continue;
                host.installed_items.push_back(queued.item);
                installed = true;
            }
            if (installed)
                host.look_again(*this);
        }
    }
    // The model follows its feed, at most once a frame.
    const uint64_t generation =
        host.feed.generation != nullptr ? host.feed.generation(host.feed.context) : 0;
    if (check_ended || !host.refreshed || generation != host.feed_generation) {
        host.feed_generation = generation;
        host.rebuild(*this);
    }
}

} // namespace oa::app
