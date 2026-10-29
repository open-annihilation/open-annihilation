// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Steps 1 to 3 before the game is mounted. The window is the game's own
// window, drawn with the bundled fonts, so a language's name in itself can
// be read before any archive is open. The copy runs on a side thread, and so
// does the scan of installers already on this machine; this thread only
// pumps the window and reads the progress. A player who already has the
// game, or who named --game-dir, does not see these steps.

#include "oa/app/setup_before_game.hpp"

#include "render_host.hpp"
#include "window_pixels.hpp"

#include "oa/app/content/downloads.hpp"
#include "oa/app/content/service.hpp"
#include "oa/app/content/settings.hpp"
#include "oa/app/content_copy.hpp"
#include "oa/app/long_work.hpp"
#include "oa/app/disk_image_source.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/game_files_hooks.hpp"
#include "oa/app/gog_download.hpp"
#include "oa/platform/web_sign_in.hpp"
#include "language_packs.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/app/window_icon.hpp"
#include "oa/data/catalogue/catalogue.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/game_installs.hpp"
#include "oa/platform/log_files.hpp"
#include "oa/platform/locale.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/platform/system.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/paint/painter.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <variant>
#include <vector>

namespace oa::app {
namespace {

namespace fs = std::filesystem;
namespace bundles = oa::data::content_bundles;
namespace catalogue = oa::data::catalogue;
namespace content = oa::app::content;
namespace first_run = oa::ui::first_run;
namespace installs = oa::platform::game_installs;
namespace kit = oa::ui::kit;
namespace languages = oa::data::languages;
namespace locale = oa::platform::locale;
namespace paint = oa::ui::paint;
namespace preferences = oa::platform::preferences;
namespace renderer = oa::ui::frontend_renderer;
namespace settings = oa::ui::engine_settings;
namespace text_font = oa::platform::text_font;

/// How many places the game step lists. More than this would run off the step.
constexpr int32_t kMostSources = 4;

/// Which reader a remembered place uses.
enum class SourceKind : uint8_t {
    folder,  ///< a folder of game files
    windows, ///< the Windows installer
    macos,   ///< the macOS package
    disc,    ///< the disk image
    fixture, ///< the check's small install
};

/// A place the game step offered, so the copy can open it again.
struct Remembered {
    std::string token{}; ///< the path, as the row keeps it
    SourceKind kind{SourceKind::folder};
    fs::path path{};
};

/// What this start tells the menu welcome.
struct Resume {
    bool continue_setup{false};                     ///< the five-step walk carries on at online
    bool copied{false};                             ///< the copying step finished the copy
    bool leave_set{false};                          ///< leave was chosen for the window that ran
    first_run::Leave leave{first_run::Leave::quit}; ///< that window's left button
};

/// The copy, shared by the pre-game window and the menu's copying step.
struct CopyJob {
    ~CopyJob() {
        stop.store(true);
        {
            const std::lock_guard<std::mutex> lock(mutex);
            paused = false;
        }
        wake.notify_all();
        if (worker.joinable())
            worker.join();
    }

    std::mutex mutex{};
    std::condition_variable wake{};
    std::atomic<bool> stop{false};
    bool paused{false};
    bool changed{false};
    first_run::CopyView view{};
    std::thread worker{};
    std::chrono::steady_clock::time_point started{};
    /// Bundle ids in the same order as `view.bundles`. The progress report names an id.
    std::vector<std::string> bundle_ids{};
    uint64_t bytes_done{};  ///< bytes finished, for the line the window's thread writes
    uint64_t bytes_total{}; ///< bytes of every matched file
    uint32_t files_done{};  ///< files finished
    uint32_t files_total{}; ///< matched files
    bool needs_check{
        false
    }; ///< the copy's thread checked it; the window's thread has not applied it
    ContentCheck pending_check{}; ///< that check; empty until needs_check
    /// Bundle ids the copy reported. The start-up check reuses only these.
    std::vector<std::string> copied_bundle_ids{};
};

CopyJob& copy_job() {
    static CopyJob job;
    return job;
}

Resume& resume_state() {
    static Resume state;
    return state;
}

std::vector<Remembered>& remembered_sources() {
    static std::vector<Remembered> sources;
    return sources;
}

/// The catalogue read before the game is mounted. It lives for the process.
languages::InterfaceText& early_catalogue() {
    static languages::InterfaceText catalogue;
    static bool loaded = false;
    if (loaded)
        return catalogue;
    loaded = true;
    const std::string base = oa::platform::program_directory();
    if (base.empty())
        return catalogue;
    const fs::path folder = path_from_utf8(base) / path_from_utf8(engine_languages_folder);
    std::error_code failure;
    if (!fs::is_directory(folder, failure) || failure)
        return catalogue;
    std::vector<fs::path> files;
    for (fs::directory_iterator entry(folder, failure), end; !failure && entry != end;
         entry.increment(failure)) {
        if (!entry->is_regular_file(failure))
            continue;
        const std::string name = path_to_utf8(entry->path().filename());
        if (name.size() >= 4 && name.compare(name.size() - 4, 4, ".tdf") == 0)
            files.push_back(entry->path());
    }
    std::sort(files.begin(), files.end());
    for (const fs::path& file : files) {
        std::ifstream input(file, std::ios::binary);
        std::string text;
        if (input)
            text.assign(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
        std::string error;
        if (!input || text.size() > languages::most_catalogue_bytes || !catalogue.add(text, &error))
            std::cerr << "open-annihilation: the interface catalogue " << path_to_utf8(file)
                      << " was not read" << (error.empty() ? "" : ": ") << error << '\n';
    }
    return catalogue;
}

/// Writes one preference without dropping the others.
///
/// @param options the parsed command line
/// @param key the preference
/// @param value the value
void write_preference(const Options& options, std::string_view key, std::string_view value) {
    try {
        const fs::path file = preference_file(options.preferences_file);
        std::error_code failure;
        preferences::Values values;
        if (fs::exists(file, failure) && !failure)
            values = preferences::load(file);
        values[std::string(key)] = std::string(value);
        preferences::save(file, values, preferences::SyncFolder::no);
    } catch (const std::exception& error) {
        std::cerr << "open-annihilation: the setup could not store a preference: " << error.what()
                  << '\n';
    }
}

/// Puts a language in effect for the words drawn before the game is mounted.
///
/// @param choice a tag, or the operating system's choice
void apply_early_language(std::string_view choice) {
    const languages::Language& system = languages::preferred_language(locale::preferred_locales());
    const languages::Language& chosen = languages::chosen_language(choice, system);
    languages::set_interface_language(&early_catalogue(), chosen);
}

/// The player's folder, or empty when it cannot be resolved.
///
/// @param options the parsed command line
/// @return the folder
fs::path resolve_user_folder(const Options& options) {
    try {
        const fs::path preferences = preference_file(options.preferences_file);
        std::error_code failure;
        preferences::Values values;
        if (fs::exists(preferences, failure) && !failure)
            values = preferences::load(preferences);
        std::string note;
        return own_user_folder(
            options.user_folder, options.preferences_file, preferences, values, note
        );
    } catch (const std::exception&) {
        return {};
    }
}

/// Tells whether a place lies inside the managed copy.
///
/// @param path the place
/// @param user_folder the player's folder
/// @return true when copying it would write into itself
bool inside_managed(const fs::path& path, const fs::path& user_folder) {
    if (path.empty() || user_folder.empty())
        return false;
    std::error_code failure;
    const fs::path place = fs::weakly_canonical(path, failure);
    if (failure)
        return false;
    const std::string place_text = path_to_utf8(place);
    for (const std::string_view folder : {managed_games_folder, managed_expansions_folder}) {
        const fs::path root = fs::weakly_canonical(user_folder / std::string(folder), failure);
        if (failure)
            continue;
        const std::string root_text = path_to_utf8(root);
        if (place_text == root_text)
            return true;
        if (place_text.size() > root_text.size() &&
            place_text.compare(0, root_text.size(), root_text) == 0 &&
            (place_text[root_text.size()] == '/' || place_text[root_text.size()] == '\\'))
            return true;
    }
    return false;
}

/// A size as the player reads it, in the same steps the downloads page uses.
///
/// @param bytes the size
/// @return the text
std::string bytes_text(uint64_t bytes) {
    if (bytes < 1000)
        return std::to_string(bytes) + " B";
    if (bytes < 1000ULL * 1000ULL)
        return std::to_string(bytes / 1000ULL) + " kB";
    if (bytes < 1000ULL * 1000ULL * 1000ULL) {
        const uint64_t tenths = bytes / 100000ULL;
        return std::to_string(tenths / 10ULL) + "." + std::to_string(tenths % 10ULL) + " MB";
    }
    const uint64_t tenths = bytes / 100000000ULL;
    return std::to_string(tenths / 10ULL) + "." + std::to_string(tenths % 10ULL) + " GB";
}

/// The bundle's name, in the language shown.
///
/// @param table the table
/// @param id the bundle id
/// @return the name; the id when the table has none
std::string bundle_name(std::span<const bundles::Bundle> table, std::string_view id) {
    for (const bundles::Bundle& bundle : table) {
        if (bundle.id == id)
            return std::string(oa::data::languages::interface_text(bundle.english_name));
    }
    return std::string(id);
}

/// Fills a row from a description. A place with no matched file is not listed,
/// and a bundle the place lacks is not a chip. The check's install uses this.
/// The machine's list is gathered and ranked by fill_sources.
///
/// @param[in,out] model the game step
/// @param[in,out] memory the places already listed, so a scan can keep its own
/// @param path the place
/// @param kind which reader
/// @param mark the badge
/// @param detail the line under the title
/// @param described what the place holds
/// @param table the table the description was made from
/// @return true when the row was added
bool add_described(
    first_run::Model& model,
    std::vector<Remembered>& memory,
    const fs::path& path,
    SourceKind kind,
    first_run::SourceMark mark,
    std::string detail,
    const SourceDescription& described,
    std::span<const bundles::Bundle> table
) {
    if (described.bundles.empty() || static_cast<int32_t>(model.sources.size()) >= kMostSources)
        return false;
    const std::string token = path_to_utf8(path);
    for (const Remembered& held : memory) {
        if (held.token == token)
            return false;
    }
    first_run::GameSource row;
    row.token = token;
    row.title =
        path.filename().empty() ? std::string("Total Annihilation") : path_to_utf8(path.filename());
    row.detail = std::move(detail);
    row.mark = mark;
    row.footnote = bytes_text(described.bytes_to_copy);
    for (const std::string& id : described.bundles) {
        first_run::SourceChip chip;
        chip.name = bundle_name(table, id);
        chip.held = true;
        chip.id = id;
        row.chips.push_back(std::move(chip));
    }
    memory.push_back(Remembered{token, kind, path});
    model.sources.push_back(std::move(row));
    return true;
}

/// One place gathered before the list is ranked.
struct HeldSource {
    first_run::GameSource row{};   ///< the row
    Remembered remembered{};       ///< how the copy opens it
    installs::SetupSource facts{}; ///< what the rank reads
};

/// The path the row is known by, so the same place is not listed twice.
///
/// @param path the place
/// @return its canonical path, or the path itself when that cannot be found
std::string source_token(const fs::path& path) {
    std::error_code failure;
    const fs::path canonical = fs::weakly_canonical(path, failure);
    return path_to_utf8(failure ? path.lexically_normal() : canonical);
}

/// A path shortened under the home folder, for the line under a row's title.
///
/// @param home the home folder
/// @param path the place
/// @return the path to show
std::string shortened_path(const fs::path& home, const fs::path& path) {
    if (path.empty())
        return {};
    return installs::path_under_home(home, path);
}

/// The rank's facts for what a place holds.
///
/// @param token the row's token
/// @param place which kind of place
/// @param described what it holds
/// @param table the table the description was made from
/// @return the facts
installs::SetupSource rank_facts(
    std::string token,
    installs::SetupPlace place,
    const SourceDescription& described,
    std::span<const bundles::Bundle> table
) {
    installs::SetupSource facts;
    facts.token = std::move(token);
    facts.place = place;
    facts.bundle_count = static_cast<int>(described.bundles.size());
    bool base_game = false;
    bool demo = false;
    int expansions = 0;
    for (const std::string& id : described.bundles) {
        const bundles::Bundle* bundle = nullptr;
        for (const bundles::Bundle& known : table) {
            if (known.id == id) {
                bundle = &known;
                break;
            }
        }
        if (bundle != nullptr && bundle->kind == bundles::Kind::expansion)
            ++expansions;
        if (id == "totala")
            base_game = true;
        else if (id == "totala-demo")
            demo = true;
    }
    facts.base_rank = base_game ? 2 : (demo ? 1 : 0);
    facts.expansion_count = expansions;
    return facts;
}

/// Tells whether a file's size and name are a published installer's.
///
/// @param file the file
/// @param size its size
/// @param context unused
/// @return true when both match a known installer
bool known_installer_file(const fs::path& file, uint64_t size, void* context) {
    std::ignore = context;
    const bundles::Installer* installer = bundles::find_installer(size);
    return installer != nullptr && is_published_installer_name(file, *installer);
}

/// Tells whether a token is already in a gathered list.
///
/// @param found the list
/// @param token the token
/// @return true when it is there
bool already_gathered(const std::vector<HeldSource>& found, const std::string& token) {
    for (const HeldSource& held : found) {
        if (held.row.token == token)
            return true;
    }
    return false;
}

/// Keeps a later "where it was found" line when the row was gathered without one.
///
/// @param[in,out] found the gathered places
/// @param token the row
/// @param status the line; empty does nothing
void note_status(
    std::vector<HeldSource>& found, const std::string& token, const std::string& status
) {
    if (status.empty())
        return;
    for (HeldSource& held : found) {
        if (held.row.token == token && held.row.status.empty())
            held.row.status = status;
    }
}

/// Builds a row for an installer file. A file that is not a known installer is left out.
///
/// @param[in,out] found the gathered places
/// @param file the installer
/// @param user_folder the player's folder
/// @param home the home folder, for the shortened path
void gather_installer(
    std::vector<HeldSource>& found,
    const fs::path& file,
    const fs::path& user_folder,
    const fs::path& home
) {
    if (file.empty() || inside_managed(file, user_folder))
        return;
    const std::string token = source_token(file);
    if (already_gathered(found, token))
        return;
    std::error_code failure;
    if (!fs::is_regular_file(file, failure) || failure)
        return;
    const auto size = fs::file_size(file, failure);
    if (failure)
        return;
    const bundles::Installer* installer = bundles::find_installer(static_cast<uint64_t>(size));
    if (installer == nullptr)
        return;
    SourceKind kind = SourceKind::windows;
    first_run::SourceMark mark = first_run::SourceMark::gog;
    if (installer->kind == bundles::InstallerKind::macos) {
        kind = SourceKind::macos;
        mark = first_run::SourceMark::package;
    } else if (installer->kind == bundles::InstallerKind::older_mac) {
        kind = SourceKind::disc;
        mark = first_run::SourceMark::disc;
    }
    const std::span<const bundles::Bundle> table = bundles::bundles();
    SourceDescription described;
    std::string error;
    if (!describe_published_installer(file, described, &error) || described.bundles.empty())
        return;
    HeldSource held;
    held.row.token = token;
    held.row.title =
        file.filename().empty() ? std::string("Total Annihilation") : path_to_utf8(file.filename());
    held.row.detail = shortened_path(home, file.parent_path());
    held.row.mark = mark;
    held.row.footnote = bytes_text(described.bytes_to_copy);
    for (const std::string& id : described.bundles) {
        first_run::SourceChip chip;
        chip.name = bundle_name(table, id);
        chip.held = true;
        chip.id = id;
        held.row.chips.push_back(std::move(chip));
    }
    held.remembered = Remembered{token, kind, file};
    held.facts = rank_facts(token, installs::SetupPlace::installer, described, table);
    found.push_back(std::move(held));
}

/// Builds a row for a folder when it is a game installation.
///
/// @param[in,out] found the gathered places
/// @param folder the folder
/// @param user_folder the player's folder
/// @param status where it was found, such as a Steam library; empty when that is not known
/// @param home the home folder, for the shortened path
void gather_folder(
    std::vector<HeldSource>& found,
    const fs::path& folder,
    const fs::path& user_folder,
    std::string status,
    const fs::path& home
) {
    if (folder.empty() || inside_managed(folder, user_folder))
        return;
    const std::string token = source_token(folder);
    if (already_gathered(found, token)) {
        note_status(found, token, status);
        return;
    }
    std::error_code failure;
    if (!fs::is_directory(folder, failure) || failure)
        return;
    const std::span<const bundles::Bundle> table = bundles::bundles();
    FolderContentSource source(folder, table);
    SourceDescription described;
    std::string error;
    if (!describe_source(source, table, described, &error) || described.bundles.empty())
        return;
    HeldSource held;
    held.row.token = token;
    held.row.title = folder.filename().empty() ? std::string("Total Annihilation")
                                               : path_to_utf8(folder.filename());
    held.row.detail = shortened_path(home, folder);
    held.row.mark = first_run::SourceMark::folder;
    held.row.status = std::move(status);
    held.row.footnote = bytes_text(described.bytes_to_copy);
    for (const std::string& id : described.bundles) {
        first_run::SourceChip chip;
        chip.name = bundle_name(table, id);
        chip.held = true;
        chip.id = id;
        held.row.chips.push_back(std::move(chip));
    }
    held.remembered = Remembered{token, SourceKind::folder, folder};
    held.facts = rank_facts(token, installs::SetupPlace::installation, described, table);
    found.push_back(std::move(held));
}

/// Gathers the rows a path contributes. A folder that is not an installation contributes the
/// installer files inside it, and is not itself a row.
///
/// @param[in,out] found the gathered places
/// @param path the folder or the file
/// @param user_folder the player's folder
/// @param status where a folder was found; empty when that is not known
/// @param home the home folder, for the shortened path
void gather_path(
    std::vector<HeldSource>& found,
    const fs::path& path,
    const fs::path& user_folder,
    std::string status,
    const fs::path& home
) {
    if (path.empty() || inside_managed(path, user_folder))
        return;
    std::error_code failure;
    if (!fs::is_directory(path, failure) || failure) {
        gather_installer(found, path, user_folder, home);
        return;
    }
    const installs::SetupFolderOffer offer =
        installs::offer_from_folder(path, known_installer_file, nullptr);
    for (const fs::path& installer : offer.installers)
        gather_installer(found, installer, user_folder, home);
    if (offer.folder_offered)
        gather_folder(found, path, user_folder, std::move(status), home);
}

/// Appends gathered places, fullest first, up to the step's limit.
///
/// `found` is this function's own list. The match is taken from it, and copied out, before
/// the function returns.
///
/// @param[in,out] model the game step
/// @param[in,out] memory the places already listed; the scan keeps its own
/// @param found the places
/// @param select_best true to select the fullest of the places appended
/// @return true when a place was appended
bool append_ranked(
    first_run::Model& model,
    std::vector<Remembered>& memory,
    std::vector<HeldSource> found,
    bool select_best
) {
    std::vector<installs::SetupSource> facts;
    facts.reserve(found.size());
    for (const HeldSource& held : found)
        facts.push_back(held.facts);
    const std::vector<installs::SetupSource> ranked =
        installs::rank_setup_sources(std::move(facts));
    bool appended = false;
    int32_t first_added = -1;
    for (const installs::SetupSource& ranked_row : ranked) {
        if (static_cast<int32_t>(model.sources.size()) >= kMostSources)
            break;
        const HeldSource* match = nullptr;
        for (const HeldSource& held : found) {
            if (held.facts.token == ranked_row.token) {
                match = &held;
                break;
            }
        }
        if (match == nullptr)
            continue;
        bool seen = false;
        for (const Remembered& remembered : memory) {
            if (remembered.token == match->remembered.token)
                seen = true;
        }
        if (seen)
            continue;
        if (first_added < 0)
            first_added = static_cast<int32_t>(model.sources.size());
        memory.push_back(match->remembered);
        model.sources.push_back(match->row);
        appended = true;
    }
    if (select_best && first_added >= 0)
        model.source = first_added;
    return appended;
}

/// Lists a folder the player just chose. An installation is one row. A folder of loose files,
/// or one that holds installers, lists those installers instead, and the fullest new row is
/// selected.
///
/// @param[in,out] model the game step
/// @param[in,out] memory the places already listed, so a scan can keep its own
/// @param folder the folder
/// @param user_folder the player's folder
/// @param detail unused; the row says the shortened path
/// @param table unused; the built-in table is read
/// @param kind unused; a chosen folder is read as a folder
void add_folder_source(
    first_run::Model& model,
    std::vector<Remembered>& memory,
    const fs::path& folder,
    const fs::path& user_folder,
    std::string detail,
    std::span<const bundles::Bundle> table,
    SourceKind kind
) {
    std::ignore = detail;
    std::ignore = table;
    std::ignore = kind;
    std::vector<HeldSource> found;
    gather_path(found, folder, user_folder, {}, installs::home_folder());
    std::ignore = append_ranked(model, memory, std::move(found), true);
}

/// Lists an installer the player just chose, and selects it.
///
/// @param[in,out] model the game step
/// @param[in,out] memory the places already listed, so a scan can keep its own
/// @param file the installer
/// @param user_folder the player's folder
void add_installer_source(
    first_run::Model& model,
    std::vector<Remembered>& memory,
    const fs::path& file,
    const fs::path& user_folder
) {
    std::vector<HeldSource> found;
    gather_installer(found, file, user_folder, installs::home_folder());
    std::ignore = append_ranked(model, memory, std::move(found), true);
}

/// Lists an installer the game downloaded, and selects it. A row already
/// listing it is selected instead; on a full list it takes the last row's place.
///
/// @param[in,out] model the game step
/// @param[in,out] memory the places already listed, in the rows' order
/// @param file the installer
/// @param user_folder the player's folder
/// @return true when a row lists it and is selected
bool add_downloaded_installer(
    first_run::Model& model,
    std::vector<Remembered>& memory,
    const fs::path& file,
    const fs::path& user_folder
) {
    const auto listed = [&]() -> int32_t {
        for (std::size_t at = 0; at < memory.size() && at < model.sources.size(); ++at) {
            std::error_code failure;
            if (memory[at].path == file || fs::equivalent(memory[at].path, file, failure))
                return static_cast<int32_t>(at);
        }
        return -1;
    };
    int32_t row = listed();
    if (row < 0) {
        if (static_cast<int32_t>(model.sources.size()) >= kMostSources &&
            memory.size() == model.sources.size()) {
            model.sources.pop_back();
            memory.pop_back();
            if (model.source >= static_cast<int32_t>(model.sources.size()))
                model.source = -1;
        }
        add_installer_source(model, memory, file, user_folder);
        row = listed();
    }
    if (row < 0)
        return false;
    model.source = row;
    return true;
}

/// Lists the places this machine already has, fullest first, and selects that one.
///
/// A folder that is not a game installation is not a row. Installer files found directly
/// inside it are. At most kMostSources rows are kept, after the rank, so the fullest stay.
/// The scan calls this on its own thread and passes its own list; nothing here touches the
/// list the game step is drawing.
///
/// @param[in,out] model the game step
/// @param user_folder the player's folder
/// @param found_install a folder already found; empty when there is none
/// @param held the scan's own list; null uses the list the game step keeps
void fill_sources(
    first_run::Model& model,
    const fs::path& user_folder,
    const fs::path& found_install,
    std::vector<Remembered>* held
) {
    std::vector<Remembered>& memory = held != nullptr ? *held : remembered_sources();
    model.sources.clear();
    model.source = -1;
    memory.clear();
    const fs::path home = installs::home_folder();
    std::vector<HeldSource> found;
    if (!found_install.empty())
        gather_path(found, found_install, user_folder, {}, home);
    for (const installs::Candidate& candidate :
         installs::find_candidates(installs::default_search_roots())) {
        gather_path(
            found,
            candidate.folder,
            user_folder,
            std::string(installs::source_words(candidate.source, candidate.removable)),
            home
        );
    }
    std::vector<fs::path> downloads;
    if (!home.empty())
        downloads.push_back(home / "Downloads");
    try {
        const fs::path documents = preferences::documents_directory();
        if (!documents.empty())
            downloads.push_back(documents.parent_path() / "Downloads");
    } catch (const std::exception&) {
    }
    for (const fs::path& folder : downloads)
        gather_path(found, folder, user_folder, {}, home);
    std::ignore = append_ranked(model, memory, std::move(found), false);
    if (!model.sources.empty())
        model.source = 0;
}

/// The installer scan, off the thread that draws the window.
///
/// The rows land in `model` and `remembered` and are copied onto the setup
/// only after `done`. The check's window replaces that list with its own
/// install, so it does not run this scan.
struct SourceScan {
    ~SourceScan() { finish(); }

    /// Joins the scan and records how long it took. A second call does nothing.
    void finish() {
        if (!started || applied)
            return;
        if (worker.joinable())
            worker.join();
        applied = true;
        oa::platform::log_files::note_timing_span("sources", began_ms);
    }

    std::thread worker{};
    std::atomic<bool> done{false};
    first_run::Model model{};
    std::vector<Remembered> remembered{};
    std::exception_ptr failure{};
    uint32_t began_ms = 0;
    bool started = false;
    bool applied = false;
};

/// Starts the scan. The caller keeps `scan` until the setup returns.
void start_source_scan(
    SourceScan& scan, const fs::path& user_folder, const fs::path& found_install
) {
    scan.began_ms = oa::platform::tick_ms();
    scan.started = true;
    scan.worker = std::thread([&scan, user_folder, found_install] {
        try {
            fill_sources(scan.model, user_folder, found_install, &scan.remembered);
        } catch (...) {
            scan.failure = std::current_exception();
        }
        scan.done.store(true);
    });
}

/// Copies a finished scan onto the game step. False when it is still running.
bool take_source_scan(SourceScan& scan, first_run::Model& model) {
    if (!scan.started || scan.applied || !scan.done.load())
        return false;
    scan.finish();
    if (scan.failure)
        std::rethrow_exception(scan.failure);
    model.sources = std::move(scan.model.sources);
    model.source = scan.model.source;
    remembered_sources() = std::move(scan.remembered);
    return true;
}

/// Scans still running when the setup ends. They finish after the menu is up,
/// and are joined when the process leaves, so Skip does not wait for them.
struct ScanKeeper {
    std::vector<std::unique_ptr<SourceScan>> held{};
};

ScanKeeper& scan_keeper() {
    static ScanKeeper keeper;
    return keeper;
}

/// Core Prime's registry id. The language step reads that catalogue.
constexpr std::string_view kCorePrime = "coreprime";

/// The registry `--check-first-run` serves. A played start never reads it.
constexpr std::string_view kCheckRegistry = "ridge";

/// The progress bar's whole, so a large pack never narrows into a byte count.
constexpr int32_t kProgressWhole = 1000;

/// Tells whether two language tags match.
///
/// @param tag a tag
/// @param other a tag
/// @return true when they match
bool same_language_tag(std::string_view tag, std::string_view other) noexcept {
    return settings::language_notice_same_tag(tag, other);
}

/// A catalogue phrase as the player reads it.
///
/// @param english the English phrase
/// @return the phrase in the language shown
std::string looked_up(std::string_view english) {
    return std::string(languages::interface_text(english));
}

/// The catalogues and the download queue for the language step.
///
/// The service is destroyed with this window. The menu's own service starts
/// later and reads the same cache and the same saved queue.
struct LanguageOffers {
    preferences::Values values{};
    fs::path preferences_path{};
    std::unique_ptr<content::Service> service{};
    std::unique_ptr<content::Downloads> downloads{};
    uint64_t seen_catalogue{0};
    uint64_t seen_downloads{0};
    bool filled{false};
    uint64_t download_item{0};
    std::string pending_failure{};
    std::string pending_failure_tag{};
};

/// Asks the next card rebuild to read the catalogue and the queue again.
///
/// @param[in,out] offers the catalogues
void forget_language_view(LanguageOffers& offers) noexcept {
    offers.seen_catalogue = 0;
    offers.seen_downloads = 0;
}

/// Writes the preferences the language step changed.
///
/// @param offers the catalogues
void store_language_preferences(const LanguageOffers& offers) {
    if (offers.preferences_path.empty())
        return;
    try {
        preferences::save(offers.preferences_path, offers.values, preferences::SyncFolder::no);
    } catch (const std::exception& caught) {
        std::cerr << "open-annihilation: the setup could not store a preference: " << caught.what()
                  << '\n';
    }
}

/// Passes each registry's stored install ID to the queue. None is made.
///
/// @param[in,out] offers the catalogues
void pass_install_ids(LanguageOffers& offers) {
    if (!offers.downloads || !offers.service)
        return;
    const std::shared_ptr<const content::Snapshot> catalogues = offers.service->snapshot();
    if (!catalogues)
        return;
    for (const content::RegistryView& listed : catalogues->registries) {
        const content::InstallId stored =
            content::read_install_id(offers.values, listed.registry.descriptor.id);
        std::optional<std::string> held;
        if (stored.state == content::InstallIdState::on)
            held = stored.text;
        offers.downloads->set_install_id(listed.registry.descriptor.id, std::move(held));
    }
}

/// Keeps a finished pack for the welcome that runs once the game is mounted.
///
/// Nothing can install a pack before then. Accepting the file leaves it in
/// the queue instead of reporting that the install failed.
///
/// @return true so the queue waits, and keeps the file when this window closes
bool keep_language_pack(
    void* /*context*/,
    uint64_t /*item*/,
    const std::filesystem::path& /*file*/,
    const content::InstallOrigin& /*origin*/
) {
    return true;
}

/// Opens the cached catalogues and the download queue. A check does not refresh.
///
/// @param options the parsed command line
/// @param user the player's folder
/// @param language_tag the language the window is speaking
/// @return the catalogues; a failed open still leaves the built-in languages
LanguageOffers
open_language_offers(const Options& options, const fs::path& user, std::string language_tag) {
    LanguageOffers offers;
    try {
        offers.preferences_path = preference_file(options.preferences_file);
        std::error_code io_error;
        if (fs::exists(offers.preferences_path, io_error) && !io_error)
            offers.values = preferences::load(offers.preferences_path);
    } catch (const std::exception& caught) {
        std::cerr << "open-annihilation: the setup could not read preferences: " << caught.what()
                  << '\n';
    }

    content::ServiceOptions service_options;
    if (options.data_dir) {
        service_options.data_folder = *options.data_dir;
    } else {
        try {
            service_options.data_folder = preferences::data_directory();
        } catch (const std::exception& caught) {
            std::cerr << "open-annihilation: content: " << caught.what() << '\n';
        }
    }
    const fs::path data_folder = service_options.data_folder;
    service_options.player_folder = user;
    const std::string program = oa::platform::program_directory();
    if (!program.empty()) {
        service_options.builtin_folder =
            path_from_utf8(program) /
            path_from_utf8(std::string(content::builtin_registries_folder_name));
    }
    service_options.developer_mode = false;
    // A window check must not fetch: a catalogue that arrives mid-walk would
    // move the cards the check is pressing. A player's start refreshes as
    // the menu's welcome does, and the first snapshot is the cache.
    service_options.automatic =
        !options.check_setup_window && !options.unattended && !options.headless_check;
    service_options.check = content::read_check_for_updates(offers.values);
    try {
        offers.service = std::make_unique<content::Service>(std::move(service_options));
        offers.service->start();
    } catch (const std::exception& caught) {
        std::cerr << "open-annihilation: the setup could not read the catalogue: " << caught.what()
                  << '\n';
        offers.service.reset();
    }

    try {
        content::DownloadsOptions queue_options;
        queue_options.data_folder = data_folder;
        queue_options.language = std::move(language_tag);
        queue_options.handoff = keep_language_pack;
        offers.downloads = std::make_unique<content::Downloads>(std::move(queue_options));
        if (offers.service) {
            const std::shared_ptr<const content::Snapshot> catalogues = offers.service->snapshot();
            if (catalogues)
                offers.downloads->start(*catalogues);
        }
        pass_install_ids(offers);
    } catch (const std::exception& caught) {
        std::cerr << "open-annihilation: the setup could not open downloads: " << caught.what()
                  << '\n';
        offers.downloads.reset();
    }
    return offers;
}

/// Tells whether Core Prime's downloads are on.
///
/// @param catalogues the snapshot
/// @param values the preferences
/// @return false when Core Prime's install ID is off
bool downloads_on(const content::Snapshot& catalogues, const preferences::Values& values) {
    for (const content::RegistryView& listed : catalogues.registries) {
        if (listed.registry.descriptor.id != kCorePrime)
            continue;
        const bool required =
            listed.registry.descriptor.install_id == oa::data::registry::InstallIdUse::required;
        if (required &&
            content::read_install_id(values, kCorePrime).state == content::InstallIdState::off)
            return false;
        return true;
    }
    return true;
}

/// Tells whether a catalogue the setup reads can be read.
///
/// @param catalogues the snapshot
/// @param check_registry true while `--check-first-run` may read its own registry
/// @return true when the snapshot lists one with a catalogue and no error
bool catalogue_readable(const content::Snapshot& catalogues, bool check_registry) {
    bool check_catalogue = false;
    for (const content::RegistryView& listed : catalogues.registries) {
        const bool readable = listed.catalogue != nullptr && listed.last_error.empty();
        if (!readable)
            continue;
        if (listed.registry.descriptor.id == kCorePrime)
            return true;
        if (check_registry && listed.registry.descriptor.id == kCheckRegistry)
            check_catalogue = true;
    }
    return check_catalogue;
}

/// The English name a catalogue gives a language, or the tag.
///
/// @param tag the language's tag
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

/// Scales a download into the progress bar.
///
/// @param[in,out] card the card
/// @param done bytes held
/// @param size the pack's size
void set_pack_progress(first_run::Card& card, uint64_t done, uint64_t size) {
    card.progress_done_text = settings::download_size_text(done);
    card.progress_size_text = settings::download_size_text(size);
    if (size == 0) {
        card.progress_done = 0;
        card.progress_whole = 0;
        return;
    }
    const double share = static_cast<double>(done) / static_cast<double>(size);
    const double clamped = share < 0.0 ? 0.0 : (share > 1.0 ? 1.0 : share);
    long rounded = std::lround(clamped * static_cast<double>(kProgressWhole));
    if (rounded < 0)
        rounded = 0;
    if (rounded > kProgressWhole)
        rounded = kProgressWhole;
    card.progress_done = static_cast<int32_t>(rounded);
    card.progress_whole = kProgressWhole;
}

/// Puts a queued pack's state on its card.
///
/// @param[in,out] card the card
/// @param item the queue's row
void apply_queue(first_run::Card& card, const content::DownloadView& item) {
    switch (item.state) {
    case content::DownloadState::waiting:
    case content::DownloadState::asking:
    case content::DownloadState::challenge:
    case content::DownloadState::downloading:
    case content::DownloadState::paused_for_match:
        card.pack = first_run::PackState::downloading;
        set_pack_progress(card, item.done, item.target.size);
        break;
    case content::DownloadState::verifying:
    case content::DownloadState::handing_over:
    case content::DownloadState::installing:
    case content::DownloadState::done:
        card.pack = first_run::PackState::checking;
        break;
    case content::DownloadState::failed:
    case content::DownloadState::cancelled:
        card.pack = first_run::PackState::failed;
        card.failure = looked_up(content::failure_text(item.failure));
        break;
    }
}

/// Reads the queue's row for a pack, when one is there.
///
/// @param offers the catalogues
/// @param tag the pack's tag
/// @param check_registry true while `--check-first-run` may read its own registry
/// @param[out] item the row; unchanged when there is none
/// @return true when the queue holds it
bool queued_pack(
    const LanguageOffers& offers,
    std::string_view tag,
    bool check_registry,
    content::DownloadView& item
) {
    if (!offers.downloads)
        return false;
    const std::vector<content::DownloadView> items = offers.downloads->view();
    for (const content::DownloadView& queued : items) {
        const bool core_prime = queued.target.registry == kCorePrime;
        const bool check = check_registry && queued.target.registry == kCheckRegistry;
        if ((!core_prime && !check) || !same_language_tag(queued.target.key, tag))
            continue;
        item = queued;
        return true;
    }
    return false;
}

/// Tells whether two cards show the same thing.
///
/// @param earlier a card
/// @param later a card
/// @return true when they do
bool same_language_card(const first_run::Card& earlier, const first_run::Card& later) {
    return earlier.tag == later.tag && earlier.own_name == later.own_name &&
           earlier.english_name == later.english_name && earlier.kind == later.kind &&
           earlier.pack == later.pack && earlier.current == later.current &&
           earlier.size_text == later.size_text &&
           earlier.progress_done_text == later.progress_done_text &&
           earlier.progress_size_text == later.progress_size_text &&
           earlier.progress_done == later.progress_done &&
           earlier.progress_whole == later.progress_whole && earlier.failure == later.failure;
}

/// Rebuilds the language cards from the built-ins, the installed packs and the catalogue.
///
/// The selected card stays the one the player was on. The first list, and a
/// list whose shape changed, puts that card in the centre with no move.
///
/// @param[in,out] session the language step
/// @param[in,out] offers the catalogues
/// @param shown the tag in effect
/// @param check_registry true while `--check-first-run` may read its own registry
/// @return true when what the step shows changed
bool refresh_language_cards(
    first_run::Session& session, LanguageOffers& offers, std::string_view shown, bool check_registry
) {
    const uint64_t catalogue_generation = offers.service ? offers.service->generation() : 0;
    const uint64_t download_generation = offers.downloads ? offers.downloads->generation() : 0;
    if (offers.filled && catalogue_generation == offers.seen_catalogue &&
        download_generation == offers.seen_downloads)
        return false;

    const std::shared_ptr<const content::Snapshot> catalogues =
        offers.service ? offers.service->snapshot() : nullptr;
    const bool online = catalogues ? downloads_on(*catalogues, offers.values) : true;
    const bool readable = catalogues && catalogue_readable(*catalogues, check_registry);
    first_run::CatalogueNote note = first_run::CatalogueNote::none;
    if (!online)
        note = first_run::CatalogueNote::downloads_off;
    else if (!readable)
        note = first_run::CatalogueNote::unread;

    std::string kept;
    if (session.carousel.selected >= 0 &&
        static_cast<std::size_t>(session.carousel.selected) < session.model.cards.size())
        kept = session.model.cards[static_cast<std::size_t>(session.carousel.selected)].tag;

    std::vector<first_run::Card> built;
    for (const languages::Language* language : languages::known_languages()) {
        if (language == nullptr || language->source != languages::Source::built_in)
            continue;
        first_run::Card card;
        card.tag = std::string(language->tag);
        card.own_name = std::string(language->endonym);
        card.english_name = std::string(language->english_name);
        card.kind = first_run::CardKind::built_in;
        card.current = same_language_tag(card.tag, shown);
        built.push_back(std::move(card));
    }
    for (const languages::Language* language : languages::known_languages()) {
        if (language == nullptr || language->source != languages::Source::installed)
            continue;
        if (!languages::playable(*language))
            continue;
        first_run::Card card;
        card.tag = std::string(language->tag);
        card.own_name = std::string(language->endonym);
        card.english_name = std::string(language->english_name);
        card.kind = first_run::CardKind::installed;
        card.current = same_language_tag(card.tag, shown);
        built.push_back(std::move(card));
    }
    if (catalogues) {
        for (const content::Entry& listed : catalogues->entries) {
            if (listed.package == nullptr || listed.package->kind != catalogue::Kind::oalang)
                continue;
            const bool core_prime = listed.registry == kCorePrime;
            const bool check = check_registry && listed.registry == kCheckRegistry;
            if (!core_prime && !check)
                continue;
            const catalogue::Package& package = *listed.package;
            if (package.id.empty())
                continue;
            bool already = false;
            for (const first_run::Card& card : built) {
                if (same_language_tag(card.tag, package.id))
                    already = true;
            }
            if (already)
                continue;
            const languages::Language* known = languages::find_by_tag(package.id);
            if (known != nullptr && languages::playable(*known))
                continue;
            first_run::Card card;
            card.tag = package.id;
            card.english_name = catalogue_english_name(package.id, package.english_name);
            card.own_name = package.name.empty() ? card.english_name : package.name;
            card.kind = first_run::CardKind::pack;
            card.current = same_language_tag(card.tag, shown);
            card.size_text = settings::download_size_text(package.size);
            if (note != first_run::CatalogueNote::none) {
                card.pack = first_run::PackState::offline;
            } else {
                content::DownloadView item;
                if (queued_pack(offers, card.tag, check_registry, item)) {
                    apply_queue(card, item);
                    if (offers.download_item == 0 || same_language_tag(card.tag, kept))
                        offers.download_item = item.item;
                } else if (same_language_tag(offers.pending_failure_tag, card.tag)) {
                    card.pack = first_run::PackState::failed;
                    card.failure = offers.pending_failure;
                }
            }
            built.push_back(std::move(card));
        }
    }

    bool changed = note != session.model.catalogue || built.size() != session.model.cards.size();
    for (std::size_t at = 0; !changed && at < built.size(); ++at)
        changed = !same_language_card(built[at], session.model.cards[at]);
    offers.seen_catalogue = catalogue_generation;
    offers.seen_downloads = download_generation;
    if (!changed && offers.filled)
        return false;

    int32_t chosen = 0;
    const auto count = static_cast<int32_t>(built.size());
    bool found = false;
    if (!kept.empty()) {
        for (int32_t at = 0; at < count; ++at) {
            if (same_language_tag(built[static_cast<std::size_t>(at)].tag, kept)) {
                chosen = at;
                found = true;
                break;
            }
        }
    }
    if (!found) {
        for (int32_t card_at = 0; card_at < count; ++card_at) {
            if (built[static_cast<std::size_t>(card_at)].current)
                chosen = card_at;
        }
    }
    const bool snap =
        !offers.filled || chosen != session.carousel.selected || session.carousel.count != count;
    session.model.cards = std::move(built);
    session.model.catalogue = note;
    session.carousel.selected = chosen;
    first_run::bind(session);
    if (snap)
        kit::carousel_settle(session.carousel);
    offers.filled = true;
    return true;
}

/// Queues a pack. A refusal is kept on the card until a download replaces it.
///
/// @param[in,out] offers the catalogues
/// @param tag the pack's tag
void queue_language_pack(LanguageOffers& offers, std::string_view tag) {
    offers.pending_failure.clear();
    offers.pending_failure_tag.clear();
    forget_language_view(offers);
    const auto refuse = [&](std::string_view why) {
        offers.pending_failure_tag = std::string(tag);
        offers.pending_failure = looked_up(why);
    };
    if (!offers.service || !offers.downloads) {
        refuse("the catalogue has no such package");
        return;
    }
    const std::shared_ptr<const content::Snapshot> catalogues = offers.service->snapshot();
    if (!catalogues) {
        refuse("the catalogue has no such package");
        return;
    }
    std::string registry_id;
    for (const content::Entry& listed : catalogues->entries) {
        if (listed.package == nullptr || listed.package->kind != catalogue::Kind::oalang)
            continue;
        if (!same_language_tag(listed.package->id, tag))
            continue;
        registry_id = listed.registry;
        break;
    }
    if (registry_id.empty()) {
        refuse("the catalogue has no such package");
        return;
    }
    std::string why;
    std::optional<content::DownloadTarget> target =
        content::download_target(*catalogues, registry_id, tag, &why);
    if (!target) {
        refuse(why.empty() ? "the catalogue has no such package" : why);
        return;
    }
    if (target->install_id == oa::data::registry::InstallIdUse::required) {
        bool changed = false;
        const std::optional<std::string> id =
            content::ensure_install_id(offers.values, registry_id, changed);
        if (changed)
            store_language_preferences(offers);
        if (!id) {
            refuse(content::failure_text(content::DownloadFailure::install_id_off));
            return;
        }
        offers.downloads->set_install_id(registry_id, id);
    }
    const std::optional<uint64_t> queued =
        offers.downloads->queue(std::move(*target), content::DownloadReason::install, &why);
    if (!queued) {
        refuse(why.empty() ? "the catalogue has no such package" : why);
        return;
    }
    offers.download_item = *queued;
}

/// Queues a failed pack again, or starts it when the queue has dropped it.
///
/// @param[in,out] offers the catalogues
/// @param session the language step
void retry_language_pack(LanguageOffers& offers, const first_run::Session& session) {
    const int32_t picked = session.carousel.selected;
    if (picked < 0 || static_cast<std::size_t>(picked) >= session.model.cards.size())
        return;
    const std::string tag = session.model.cards[static_cast<std::size_t>(picked)].tag;
    offers.pending_failure.clear();
    offers.pending_failure_tag.clear();
    forget_language_view(offers);
    if (offers.downloads) {
        const std::vector<content::DownloadView> items = offers.downloads->view();
        for (const content::DownloadView& item : items) {
            if (!same_language_tag(item.target.key, tag))
                continue;
            if (item.state != content::DownloadState::failed &&
                item.state != content::DownloadState::cancelled)
                continue;
            offers.downloads->retry(item.item);
            offers.download_item = item.item;
            return;
        }
    }
    queue_language_pack(offers, tag);
}

/// Opens the centred card's language.
///
/// @param[in,out] session the language step
/// @param options the parsed command line
/// @param[in,out] shown the tag in effect
void apply_selected_language(
    first_run::Session& session, const Options& options, std::string& shown
) {
    const int32_t picked = session.carousel.selected;
    if (picked < 0 || static_cast<std::size_t>(picked) >= session.model.cards.size())
        return;
    const first_run::Card& card = session.model.cards[static_cast<std::size_t>(picked)];
    const languages::Language* known = languages::find_by_tag(card.tag);
    if (known == nullptr || !languages::playable(*known))
        return;
    apply_early_language(card.tag);
    write_preference(options, settings::key::language, known->tag);
    shown = std::string(known->tag);
    for (first_run::Card& other : session.model.cards)
        other.current = same_language_tag(other.tag, shown);
}

/// Replaces `{token}` once.
void replace_token(std::string& text, std::string_view token, std::string_view value) {
    const auto at = text.find(token);
    if (at != std::string::npos)
        text.replace(at, token.size(), value);
}

/// The overall line, in the language the interface shows.
std::string overall_line(uint64_t done, uint64_t total) {
    std::string text{languages::interface_text("{done} of {total} · {percent}%")};
    const uint64_t percent = total == 0 ? 0 : (done * 100ULL) / total;
    replace_token(text, "{done}", std::to_string(done));
    replace_token(text, "{total}", std::to_string(total));
    replace_token(text, "{percent}", std::to_string(percent));
    return text;
}

/// About how long is left, or empty when it cannot be said.
std::string remaining_line(const CopyJob& job, uint64_t done, uint64_t total) {
    if (done == 0 || total <= done)
        return {};
    const auto elapsed = std::chrono::steady_clock::now() - job.started;
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    if (seconds < 1)
        return {};
    const uint64_t left = ((total - done) * static_cast<uint64_t>(seconds)) / done;
    std::string text{languages::interface_text("about {seconds} seconds left")};
    replace_token(text, "{seconds}", std::to_string(left));
    return text;
}

/// Copies one progress report into the view. The report's views die with it.
/// A paused copy waits here, so the bytes stop between chunks.
void note_progress(CopyJob& job, const CopyProgress& progress) {
    std::unique_lock<std::mutex> lock(job.mutex);
    job.wake.wait(lock, [&job] { return !job.paused || job.stop.load(); });
    first_run::CopyView& view = job.view;
    view.whole = 1000;
    if (progress.bytes_total == 0)
        view.done = 0;
    else {
        const uint64_t scaled = (progress.bytes_done * 1000ULL) / progress.bytes_total;
        view.done = static_cast<int32_t>(std::min<uint64_t>(scaled, 1000ULL));
    }
    job.bytes_done = progress.bytes_done;
    job.bytes_total = progress.bytes_total;
    job.files_done = progress.files_done;
    job.files_total = progress.files_total;
    if (!progress.bundle_id.empty()) {
        bool seen = false;
        for (const std::string& id : job.copied_bundle_ids) {
            if (id == progress.bundle_id)
                seen = true;
        }
        if (!seen)
            job.copied_bundle_ids.emplace_back(progress.bundle_id);
    }
    view.running = true;
    int32_t current = -1;
    const auto rows = static_cast<int32_t>(job.bundle_ids.size());
    for (int32_t index = 0; index < rows; ++index) {
        if (job.bundle_ids[static_cast<std::size_t>(index)] == progress.bundle_id)
            current = index;
    }
    if (current >= 0) {
        for (int32_t index = 0; index < current; ++index) {
            first_run::CopyBundle& earlier = view.bundles[static_cast<std::size_t>(index)];
            if (earlier.phase == first_run::CopyPhase::differ)
                continue;
            earlier.phase = first_run::CopyPhase::done;
            earlier.done = earlier.whole;
            earlier.file.clear();
        }
        first_run::CopyBundle& row = view.bundles[static_cast<std::size_t>(current)];
        row.phase = first_run::CopyPhase::copying;
        row.file = std::string(progress.copy_name);
        row.done = view.done;
        row.amount = bytes_text(progress.bytes_done);
    }
    job.changed = true;
}

/// The progress callback. It runs on the copy's thread.
void on_copy_progress(void* context, const CopyProgress& progress) {
    if (context == nullptr)
        return;
    note_progress(*static_cast<CopyJob*>(context), progress);
}

/// The check's progress. It runs on the copy's thread and only stores counts.
bool on_copy_check(uint32_t done, uint32_t total, void* context) {
    if (context == nullptr)
        return false;
    auto* job = static_cast<CopyJob*>(context);
    const std::lock_guard<std::mutex> lock(job->mutex);
    job->view.hashing = true;
    job->view.running = true;
    job->view.check_done = done;
    job->view.check_total = total;
    job->changed = true;
    return !job->stop.load();
}

/// One row for each bundle this source holds. A bundle with no matched file
/// is left off, so the list does not show a bar for it.
void prime_bundles(
    CopyJob& job,
    std::span<const bundles::Bundle> table,
    std::span<const first_run::SourceChip> chips
) {
    job.view.bundles.clear();
    job.bundle_ids.clear();
    for (const bundles::Bundle& bundle : table) {
        bool held = false;
        for (const first_run::SourceChip& chip : chips) {
            if (chip.held && chip.id == bundle.id)
                held = true;
        }
        if (!held)
            continue;
        first_run::CopyBundle row;
        row.name = bundle_name(table, bundle.id);
        row.id = std::string(bundle.id);
        row.whole = 1000;
        row.phase = first_run::CopyPhase::waiting;
        job.bundle_ids.emplace_back(bundle.id);
        job.view.bundles.push_back(std::move(row));
    }
}

/// Adds two counts, stopping at the greatest uint32_t.
uint32_t add_count(uint32_t left, uint32_t right) noexcept {
    constexpr uint32_t kLimit = 0xffffffffu;
    if (left > kLimit - right)
        return kLimit;
    return left + right;
}

/// Marks every listed row from the check and sets the verdict.
///
/// The overall count is the files those rows take. A file the source does
/// not hold is not one of them. A music bundle that only lacks a track, and
/// a bundle whose only gap is a file left out for its size, is a note, not a
/// difference.
void apply_check(first_run::CopyView& view, const ContentCheck& check, bool finished) {
    uint32_t differ = 0;
    uint32_t taken = 0;
    uint32_t done_files = 0;
    CopyJob& job = copy_job();
    const auto rows = static_cast<int32_t>(view.bundles.size());
    for (int32_t index = 0; index < rows; ++index) {
        first_run::CopyBundle& row = view.bundles[static_cast<std::size_t>(index)];
        const ContentBundleCheck* found = nullptr;
        const std::string id = index < static_cast<int32_t>(job.bundle_ids.size())
                                   ? job.bundle_ids[static_cast<std::size_t>(index)]
                                   : std::string{};
        for (const ContentBundleCheck& bundle : check.bundles) {
            if (bundle.id == id || bundle.id == row.id)
                found = &bundle;
        }
        if (found == nullptr)
            continue;
        row.amount = content_bundle_line(*found);
        // Tracks the source did not hold, and files left out for their size,
        // are a note while nothing differs.
        row.note =
            found->files_differ == 0 && ((found->id == "music" && found->files_not_in_source > 0 &&
                                          found->files_not_in_source <= found->file_count) ||
                                         !found->left_out.empty());
        const uint32_t absent = found->files_not_in_source > found->file_count
                                    ? found->file_count
                                    : found->files_not_in_source;
        const uint32_t row_taken = found->file_count - absent;
        const uint32_t row_differ =
            found->files_differ > row_taken ? row_taken : found->files_differ;
        taken = add_count(taken, row_taken);
        done_files = add_count(done_files, row_taken - row_differ);
        row.file.clear();
        if (found->files_differ == 0) {
            row.phase = first_run::CopyPhase::done;
            row.done = row.whole;
        } else {
            row.phase = first_run::CopyPhase::differ;
            differ += found->files_differ;
        }
    }
    view.running = false;
    view.paused = false;
    view.hashing = false;
    view.remaining.clear();
    if (!finished) {
        view.verdict = std::string(languages::interface_text("The game could not be copied."));
        view.checked = false;
        return;
    }
    view.verdict = content_match_line(differ);
    view.checked = true;
    if (taken > 0) {
        const CopyFileCount tally = copy_file_count(done_files, taken, 0);
        job.files_done = tally.done;
        job.files_total = tally.total;
        if (differ == 0 || view.whole <= 0)
            view.done = view.whole;
        else
            view.done = static_cast<int32_t>(
                (static_cast<uint64_t>(tally.done) * static_cast<uint64_t>(view.whole)) /
                tally.total
            );
    } else {
        view.done = view.whole;
    }
}

/// Opens the chosen place.
std::unique_ptr<ContentSource> open_source(const Remembered& remembered) {
    const std::span<const bundles::Bundle> table =
        remembered.kind == SourceKind::fixture ? copy_check_bundles() : bundles::bundles();
    switch (remembered.kind) {
    case SourceKind::folder:
    case SourceKind::fixture:
        return std::make_unique<FolderContentSource>(remembered.path, table);
    case SourceKind::windows:
        return std::make_unique<WindowsInstallerSource>(remembered.path, table);
    case SourceKind::macos:
        return std::make_unique<MacosInstallerSource>(remembered.path, table, true);
    case SourceKind::disc:
        return std::make_unique<DiskImageContentSource>(
            remembered.path, table, DiskImageOptions{true}
        );
    }
    return nullptr;
}

/// Ends access a system picker gave, after the copy no longer reads the place.
void release_picked_sources() {
    const GameFilesHooks& hooks = game_files_hooks();
    if (hooks.release_source == nullptr)
        return;
    for (const Remembered& remembered : remembered_sources()) {
        if (remembered.kind == SourceKind::fixture)
            continue;
        const std::string path = path_to_utf8(remembered.path);
        hooks.release_source(hooks.context, path.c_str());
    }
}

/// A key the setup answers, or nothing.
std::optional<kit::Key> setup_key(const SDL_Event& event) {
    if (event.type != SDL_EVENT_KEY_DOWN || event.key.repeat)
        return std::nullopt;
    switch (event.key.key) {
    case SDLK_RETURN:
    case SDLK_KP_ENTER:
        return kit::Key::enter;
    case SDLK_ESCAPE:
        return kit::Key::escape;
    case SDLK_UP:
        return kit::Key::up;
    case SDLK_DOWN:
        return kit::Key::down;
    case SDLK_LEFT:
        return kit::Key::left;
    case SDLK_RIGHT:
        return kit::Key::right;
    case SDLK_SPACE:
        return kit::Key::space;
    case SDLK_TAB:
        return (event.key.mod & SDL_KMOD_SHIFT) != 0 ? kit::Key::back_tab : kit::Key::tab;
    // A list that does not fit scrolls under these.
    case SDLK_PAGEUP:
        return kit::Key::page_up;
    case SDLK_PAGEDOWN:
        return kit::Key::page_down;
    case SDLK_HOME:
        return kit::Key::home;
    case SDLK_END:
        return kit::Key::end;
    default:
        return std::nullopt;
    }
}

/// The greatest point count whose smooth drawing is no wider than `pixels`.
///
/// to_canvas rounds each edge on its own, so floor(pixels * 100 / scale) can
/// land one pixel past the canvas. The last point that still fits is the one
/// the footer and the header may use.
///
/// @param pixels the pixels the drawing may cover
/// @param scale the canvas scale, in percent
/// @return the points, never negative
int32_t points_fitting(int32_t pixels, int32_t scale) noexcept {
    if (pixels <= 0 || scale <= 0)
        return 0;
    int32_t points =
        static_cast<int32_t>((static_cast<int64_t>(pixels) * 100) / static_cast<int64_t>(scale));
    while (points > 0 && kit::to_canvas({0, 0, points, 0}, scale).width > pixels)
        --points;
    return points;
}

/// A point origin whose pixel is `pixel`, or the nearest point that does not
/// pass it.
///
/// @param pixel the pixel the origin should land on
/// @param scale the canvas scale, in percent
/// @return the origin, in points
int32_t origin_at_most(int32_t pixel, int32_t scale) noexcept {
    if (pixel <= 0 || scale <= 0)
        return 0;
    int32_t points =
        static_cast<int32_t>((static_cast<int64_t>(pixel) * 100) / static_cast<int64_t>(scale));
    while (kit::to_canvas({points, 0, 0, 0}, scale).x < pixel)
        ++points;
    while (points > 0 && kit::to_canvas({points, 0, 0, 0}, scale).x > pixel)
        --points;
    return points;
}

/// Keeps the renderer's safe area as well as the window's. Interactive
/// content stays inside both. A failed read leaves the window's area.
///
/// @param renderer the window's renderer
/// @param[in,out] viewport the window's pixels and the area kept clear
void absorb_render_safe(SDL_Renderer* renderer, oa::ui::game_files::Viewport& viewport) {
    if (renderer == nullptr || viewport.width <= 0 || viewport.height <= 0)
        return;
    SDL_Rect safe{};
    if (!SDL_GetRenderSafeArea(renderer, &safe) || safe.w <= 0 || safe.h <= 0)
        return;
    const int32_t left = std::clamp(safe.x, 0, viewport.width);
    const int32_t top = std::clamp(safe.y, 0, viewport.height);
    const int32_t right = std::clamp(viewport.width - (safe.x + safe.w), 0, viewport.width);
    const int32_t bottom = std::clamp(viewport.height - (safe.y + safe.h), 0, viewport.height);
    viewport.safe.left = std::max(viewport.safe.left, left);
    viewport.safe.top = std::max(viewport.safe.top, top);
    viewport.safe.right = std::max(viewport.safe.right, right);
    viewport.safe.bottom = std::max(viewport.safe.bottom, bottom);
}

/// The frame the window is laid out in: the safe pixels, at the setup's
/// scale for them (first_run::setup_scale_percent, never below the window's
/// density), shrunk so the drawing cannot pass the canvas. The window is
/// drawn on the smooth form, so the scale may be a fraction.
///
/// @param viewport the window's pixels, density and safe area
/// @return the frame
kit::Frame frame_of_window(const oa::ui::game_files::Viewport& viewport) {
    const float density = viewport.px_per_point > 0.F ? viewport.px_per_point : 1.F;
    int32_t least = static_cast<int32_t>(std::lround(static_cast<double>(density) * 100.0));
    if (least < 100)
        least = 100;
    int32_t safe_left = std::max(viewport.safe.left, 0);
    int32_t safe_top = std::max(viewport.safe.top, 0);
    int32_t safe_right = std::max(viewport.safe.right, 0);
    int32_t safe_bottom = std::max(viewport.safe.bottom, 0);
    int32_t inner_width = viewport.width - safe_left - safe_right;
    int32_t inner_height = viewport.height - safe_top - safe_bottom;
    if (inner_width < 1 || inner_height < 1) {
        safe_left = 0;
        safe_top = 0;
        safe_right = 0;
        safe_bottom = 0;
        inner_width = std::max(viewport.width, 0);
        inner_height = std::max(viewport.height, 0);
    }
    const int32_t scale = first_run::setup_scale_percent(inner_width, inner_height, least, true);
    kit::Frame frame;
    frame.scale_percent = scale;
    frame.area.x = origin_at_most(safe_left, scale);
    frame.area.y = origin_at_most(safe_top, scale);
    frame.area.width = points_fitting(inner_width, scale);
    frame.area.height = points_fitting(inner_height, scale);
    kit::Rect drawn = kit::to_canvas(frame.area, scale);
    const int32_t limit_right = safe_left + inner_width;
    const int32_t limit_bottom = safe_top + inner_height;
    while (frame.area.width > 1 && drawn.x + drawn.width > limit_right) {
        --frame.area.width;
        drawn = kit::to_canvas(frame.area, scale);
    }
    while (frame.area.height > 1 && drawn.y + drawn.height > limit_bottom) {
        --frame.area.height;
        drawn = kit::to_canvas(frame.area, scale);
    }
    frame.size_class = kit::size_class_of(frame.area.width, frame.area.height);
    return frame;
}

/// A canvas pixel as a point of the frame.
kit::Point point_of(float x, float y, const kit::Frame& frame) {
    const double scale = static_cast<double>(std::max(frame.scale_percent, int32_t{1})) / 100.0;
    return {
        static_cast<int32_t>(std::lround(static_cast<double>(x) / scale)) - frame.area.x,
        static_cast<int32_t>(std::lround(static_cast<double>(y) / scale)) - frame.area.y
    };
}

struct PickWait {
    bool done{false};
    bool quit{false};
    std::vector<std::string> paths{};
    std::string error{};
};

void picker_done(void* userdata, const std::vector<std::string>& paths, bool, const char* error) {
    auto* wait = static_cast<PickWait*>(userdata);
    wait->paths = paths;
    if (error != nullptr)
        wait->error = error;
    wait->done = true;
}

void SDLCALL folder_done(void* userdata, const char* const* filelist, int) {
    auto* wait = static_cast<PickWait*>(userdata);
    if (filelist == nullptr) {
        const char* error = SDL_GetError();
        if (error != nullptr && error[0] != '\0')
            wait->error = error;
    } else if (filelist[0] != nullptr) {
        wait->paths.emplace_back(filelist[0]);
    }
    wait->done = true;
}

/// Pumps until the picker answers. A quit is pushed back for the setup.
bool pump_until_picked(PickWait& wait) {
    while (!wait.done) {
        SDL_Event event;
        if (!SDL_WaitEventTimeout(&event, 50))
            continue;
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            wait.quit = true;
            wait.done = true;
            SDL_PushEvent(&event);
            break;
        }
    }
    return !wait.quit && !wait.paths.empty() && wait.error.empty();
}

/// Writes the check's small files under `folder`, one path piece at a time.
bool write_fixture(const fs::path& folder) {
    for (const CopyCheckFile& file : copy_check_files()) {
        fs::path dest = folder;
        std::string part;
        for (const char byte : file.copy_name) {
            if (byte == '/' || byte == '\\') {
                if (!part.empty()) {
                    dest /= path_from_utf8(part);
                    part.clear();
                }
            } else {
                part.push_back(byte);
            }
        }
        if (!part.empty())
            dest /= path_from_utf8(part);
        const auto* data = reinterpret_cast<const uint8_t*>(file.bytes.data());
        std::string error;
        if (!oa::platform::replace_file(
                dest, std::span<const uint8_t>(data, file.bytes.size()), &error
            ))
            return false;
    }
    return true;
}

/// Tells whether this start shows the game steps.
bool setup_due(const Options& options, bool mount_copy) {
    // The pre-game window check walks these steps here. --game-dir is what the
    // menu plays afterwards, and a dummy video would otherwise count as unattended.
    if (options.check_setup_window && !options.check_first_run)
        return true;
    if (!mount_copy || options.game_dir_named || options.check_first_run)
        return false;
    if (options.unattended || options.headless_check || options.benchmark_frames.has_value() ||
        !options.stage_file.empty() || options.remote_controlled)
        return false;
    const char* ci = SDL_getenv("CI");
    const char* video = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    if (unattended_environment(
            ci == nullptr ? std::string_view{} : std::string_view{ci},
            video == nullptr ? std::string_view{} : std::string_view{video}
        ))
        return false;
    return true;
}

/// Reads the preference that records a finished welcome.
bool welcome_is_done(const Options& options) {
    try {
        const fs::path file = preference_file(options.preferences_file);
        std::error_code failure;
        if (!fs::exists(file, failure) || failure)
            return false;
        const preferences::Values values = preferences::load(file);
        const auto found = values.find(std::string(first_run::preference_key));
        return found != values.end() && found->second == first_run::preference_done;
    } catch (const std::exception&) {
        return false;
    }
}

/// The language the preferences already name, or the operating system's.
std::string stored_language(const Options& options) {
    try {
        const fs::path file = preference_file(options.preferences_file);
        std::error_code failure;
        if (!fs::exists(file, failure) || failure)
            return std::string(languages::system_choice);
        const preferences::Values values = preferences::load(file);
        const auto found = values.find(std::string(settings::key::language));
        if (found == values.end() || found->second.empty())
            return std::string(languages::system_choice);
        return found->second;
    } catch (const std::exception&) {
        return std::string(languages::system_choice);
    }
}

/// How long the pre-game window check may run, in frames of this loop.
constexpr int kSetupCheckFrames = 2000;

/// Deutsch on the language carousel.
constexpr std::string_view kDeutschCard = "first-run.language.card.de";

/// Where the pre-game window check is in its walk. One call takes one action.
struct SetupWindowDrive {
    enum class Phase : uint8_t {
        mode,     ///< the header follows the window; switch to full screen
        full,     ///< the header shows the mark and the name; back to the window
        window,   ///< the header leaves them out again
        turn,     ///< Left, Left and Right on the carousel
        turned,   ///< the focus is on the card in the middle
        seek,     ///< bring Deutsch onto the carousel
        pick,     ///< choose Deutsch
        language, ///< the German lines, then Next
        game,     ///< the fixture, then Next
        copying,  ///< the copying step's German lines
        wait,     ///< the copy and its check
        records,  ///< the managed copy and its records, then Next
        done,     ///< the walk has left the window
    };
    Phase phase{Phase::mode};
    int frames{0};  ///< frames this walk has taken
    int keys{0};    ///< carousel keys pressed
    int back{0};    ///< previous-card presses
    int forward{0}; ///< next-card presses
    int pause{0};   ///< frames to wait before the next card press
    int picks{0};   ///< presses on Deutsch
};

/// Throws when a step of the pre-game window check failed.
void setup_check_expect(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("setup window check: " + std::string(what));
}

/// Walks language, the game and the copy. One call is one press, or a look.
///
/// The scene is the one just drawn. A press is handled and the next call sees
/// the step it opened. The check's install is already the chosen source.
template <typename Handle>
void drive_setup_window_check(
    SetupWindowDrive& drive,
    SDL_Window* window,
    const fs::path& user,
    first_run::Session& session,
    const first_run::Scene& scene,
    Handle&& handle
) {
    if (drive.phase == SetupWindowDrive::Phase::done)
        return;
    ++drive.frames;
    setup_check_expect(drive.frames <= kSetupCheckFrames, "ran out of frames");

    const auto has = [&](std::string_view name) {
        for (const kit::Control& control : scene.list.controls)
            if (control.name == name && control.enabled)
                return true;
        return false;
    };
    const auto shows = [&](std::string_view words) {
        std::string joined;
        for (const kit::Item& item : scene.list.items) {
            if (item.text.find(words) != std::string::npos)
                return true;
            joined += item.text;
        }
        for (const kit::Control& control : scene.list.controls) {
            if (control.text.find(words) != std::string::npos)
                return true;
            joined += control.text;
        }
        return joined.find(words) != std::string::npos;
    };
    const auto click = [&](std::string_view name) {
        kit::Rect rect{};
        bool enabled = false;
        for (const kit::Control& control : scene.list.controls) {
            if (control.name != name)
                continue;
            rect = control.rect;
            enabled = control.enabled;
            break;
        }
        setup_check_expect(enabled, std::string(name) + " is not on the setup");
        const kit::Point at{rect.x + rect.width / 2, rect.y + rect.height / 2};
        handle(first_run::pointer(session, scene, first_run::PointerKind::down, at, 0, 1));
        handle(first_run::pointer(session, scene, first_run::PointerKind::up, at, 0, 1));
    };
    const auto german = [](std::string_view key, std::string_view words) {
        const std::string got{languages::interface_text(key)};
        setup_check_expect(got == words, std::string(key) + " is not German");
    };

    if (drive.pause > 0) {
        if (drive.phase == SetupWindowDrive::Phase::seek && has(kDeutschCard)) {
            drive.pause = 0;
            drive.phase = SetupWindowDrive::Phase::pick;
            return;
        }
        --drive.pause;
        return;
    }

    // Whether the header shows the mark and the name.
    const auto branded = [&] {
        for (const kit::Item& item : scene.list.items)
            if (const auto* look = std::get_if<kit::SetupChromeLook>(&item.look))
                return look->brand;
        return false;
    };
    const auto full_screen = [&] {
        return (SDL_GetWindowFlags(window) & SDL_WINDOW_FULLSCREEN) != 0;
    };
    const auto switch_mode = [&](bool full, std::string_view what) {
        setup_check_expect(
            SDL_SetWindowFullscreen(window, full) && SDL_SyncWindow(window),
            std::string(what) + " did not take"
        );
        drive.pause = 10;
    };

    switch (drive.phase) {
    case SetupWindowDrive::Phase::mode:
        setup_check_expect(!full_screen(), "the setup did not open in a window");
        setup_check_expect(!branded(), "the header names the game in a window");
        switch_mode(true, "full screen");
        drive.phase = SetupWindowDrive::Phase::full;
        return;
    case SetupWindowDrive::Phase::full:
        setup_check_expect(full_screen(), "the window is not full screen");
        setup_check_expect(branded(), "the header does not name the game full screen");
        switch_mode(false, "the window");
        drive.phase = SetupWindowDrive::Phase::window;
        return;
    case SetupWindowDrive::Phase::window:
        setup_check_expect(!full_screen(), "the window did not come back");
        setup_check_expect(!branded(), "the header still names the game in the window");
        setup_check_expect(
            SDL_SetWindowSize(window, 1280, 720) && SDL_SyncWindow(window),
            "the window did not take 1280x720 again"
        );
        std::cout << "setup window check: the header follows the window\n" << std::flush;
        drive.pause = 10;
        drive.phase = SetupWindowDrive::Phase::turn;
        return;
    case SetupWindowDrive::Phase::turn: {
        constexpr kit::Key kTurns[] = {kit::Key::left, kit::Key::left, kit::Key::right};
        handle(first_run::key(session, scene, kTurns[drive.keys]));
        ++drive.keys;
        drive.pause = 4;
        if (drive.keys >= static_cast<int>(std::size(kTurns))) {
            drive.pause = 60;
            drive.phase = SetupWindowDrive::Phase::turned;
        }
        return;
    }
    case SetupWindowDrive::Phase::turned: {
        const kit::Control* focused = kit::control_of(scene.list, session.interaction.focused);
        setup_check_expect(
            focused != nullptr &&
                session.interaction.focused == first_run::card_id + session.carousel.selected,
            "the focus is not on the chosen card"
        );
        const int32_t middle = focused->rect.x + focused->rect.width / 2;
        setup_check_expect(
            middle >= scene.width / 2 - 2 && middle <= scene.width / 2 + 2,
            "the focus is not on the card in the middle"
        );
        std::cout << "setup window check: the box is on " << focused->name << "\n" << std::flush;
        drive.phase = SetupWindowDrive::Phase::seek;
        return;
    }
    case SetupWindowDrive::Phase::seek:
        setup_check_expect(
            session.model.step == first_run::Step::language, "the language step did not show"
        );
        if (has(kDeutschCard)) {
            drive.phase = SetupWindowDrive::Phase::pick;
            return;
        }
        if (drive.back < 8 && has("first-run.language.previous")) {
            click("first-run.language.previous");
            ++drive.back;
            drive.pause = 40;
            return;
        }
        if (drive.forward < 8 && has("first-run.language.next-card")) {
            click("first-run.language.next-card");
            ++drive.forward;
            drive.pause = 40;
            return;
        }
        throw std::runtime_error("setup window check: Deutsch is not on the carousel");
    case SetupWindowDrive::Phase::pick:
        setup_check_expect(has(kDeutschCard), "Deutsch is not on the carousel");
        click(kDeutschCard);
        ++drive.picks;
        if (drive.picks >= 2)
            drive.phase = SetupWindowDrive::Phase::language;
        return;
    case SetupWindowDrive::Phase::language:
        german(
            "Locate Total Annihilation Game Files", "Spieldateien von Total Annihilation suchen"
        );
        german(
            "OA copies the game once. The installation is left as it is.",
            "OA kopiert das Spiel einmal. Die Installation bleibt, wie sie ist."
        );
        german(
            "If OA stops, the copy carries on from here next time.",
            "Wenn OA anhält, geht die Kopie beim nächsten Mal von hier weiter."
        );
        german("Copying Total Annihilation", "Total Annihilation wird kopiert");
        german("All files match", "Alle Dateien stimmen überein");
        german("Not now", "Nicht jetzt");
        german("Quit", "Beenden");
        setup_check_expect(shows("Beenden"), "the language step's left button is not Quit");
        click("first-run.language.next");
        drive.phase = SetupWindowDrive::Phase::game;
        return;
    case SetupWindowDrive::Phase::game:
        setup_check_expect(
            session.model.step == first_run::Step::game, "the game step did not show"
        );
        setup_check_expect(session.model.source == 0, "the check's install was not chosen");
        setup_check_expect(
            scene.title_text == "Spieldateien von Total Annihilation suchen",
            "the game step's title is not German"
        );
        setup_check_expect(shows("OA kopiert das Spiel einmal"), "the game step is not in German");
        setup_check_expect(shows("Beenden"), "the game step's left button is not Quit");
        click("first-run.game.next");
        drive.phase = SetupWindowDrive::Phase::copying;
        return;
    case SetupWindowDrive::Phase::copying:
        setup_check_expect(
            session.model.step == first_run::Step::copying, "the copying step did not show"
        );
        setup_check_expect(
            scene.title_text == "Total Annihilation wird kopiert",
            "the copying step's title is not German"
        );
        setup_check_expect(shows("Wenn OA anhält"), "the copying step is not in German");
        setup_check_expect(shows("Beenden"), "the copying step's left button is not Quit");
        drive.phase = SetupWindowDrive::Phase::wait;
        return;
    case SetupWindowDrive::Phase::wait:
        setup_check_expect(
            session.model.step == first_run::Step::copying, "the copying step closed early"
        );
        if (shows("Alle Dateien stimmen überein"))
            drive.phase = SetupWindowDrive::Phase::records;
        return;
    case SetupWindowDrive::Phase::records: {
        const fs::path games = user / std::string(bundles::managed_games_folder) / "totala";
        const fs::path campaign =
            user / std::string(bundles::managed_expansions_folder) / "totala" / "campaign";
        const auto regular = [](const fs::path& path) {
            std::error_code failure;
            return fs::is_regular_file(path, failure) && !failure;
        };
        setup_check_expect(
            regular(games / std::string(bundles::content_record_name)),
            "Games/totala/.oa-content was not written"
        );
        setup_check_expect(regular(games / "rev31.gp3"), "rev31.gp3 was not copied");
        setup_check_expect(regular(games / "Data" / "2.zrb"), "Data/2.zrb was not copied");
        setup_check_expect(
            regular(campaign / std::string(bundles::content_record_name)),
            "Expansions/totala/campaign/.oa-content was not written"
        );
        setup_check_expect(regular(campaign / "totala4.hpi"), "totala4.hpi was not copied");
        setup_check_expect(session.model.copy.checked, "the copy was not checked");
        click("first-run.copying.next");
        drive.phase = SetupWindowDrive::Phase::done;
        std::cout << "setup window check: the game was copied\n" << std::flush;
        return;
    }
    case SetupWindowDrive::Phase::done:
        return;
    }
}

/// Stops a copy this window started, and lets the display sleep again.
///
/// @param[in,out] session the session
/// @param[in,out] awake true when this window asked the display to stay awake
void stop_running_copy(first_run::Session& session, bool& awake) {
    setup_request_cancel();
    setup_stop_join();
    session.model.copy.running = false;
    session.model.copy.paused = false;
    if (awake) {
        SDL_EnableScreenSaver();
        awake = false;
    }
}

} // namespace

bool setup_before_game_due(const Options& options, bool mount_copy) {
    return setup_due(options, mount_copy);
}

void apply_stored_interface_language(const Options& options) {
    apply_early_language(stored_language(options));
}

void setup_welcome_again() noexcept {
    resume_state().continue_setup = false;
}

SetupBefore run_setup_before_game(
    const Options& options, SetupDisplay display, bool mount_copy, const fs::path& found_install
) {
    resume_state() = {};
    if (!setup_due(options, mount_copy) || display.window == nullptr || display.renderer == nullptr)
        return SetupBefore::not_run;
    const fs::path user = resolve_user_folder(options);
    if (user.empty())
        return SetupBefore::not_run;
    if (managed_base_present(user, bundles::bundles()))
        return SetupBefore::not_run;
    // The setup draws this window from here. A launch pump must not clear it.
    release_launch_window();

    const std::string language = stored_language(options);
    apply_early_language(language);
    const languages::Language& shown = languages::interface_language();

    first_run::Session session;
    session.model.walk = first_run::Walk::setup;
    // Nothing is set up yet: Quit, on every step. A welcome already recorded
    // as done is a re-run of the copy, so the same button says Skip.
    const bool again = welcome_is_done(options);
    session.model.leave = again ? first_run::Leave::skip : first_run::Leave::quit;
    resume_state().leave = session.model.leave;
    resume_state().leave_set = true;
    session.model.step = again ? first_run::Step::game : first_run::Step::language;
    session.model.controller = SDL_HasGamepad();
    // The checks draw the step as it was before GOG was offered.
    session.model.gog_offered =
        setup_gog_offered() && !options.check_setup_window && !options.check_first_run;
    std::string shown_tag(shown.tag);
    LanguageOffers offers = open_language_offers(options, user, shown_tag);
    static_cast<void>(refresh_language_cards(session, offers, shown_tag, options.check_first_run));

    // The step on screen, and whether its first frame has been presented.
    struct SetupStepTiming {
        std::string_view token;
        uint32_t began_ms = 0;
        bool framed = false;
    };

    SetupStepTiming step_timing;
    const auto note_setup_step = [&] {
        const std::string_view token = first_run::step_token(session.model.step);
        if (token == step_timing.token)
            return;
        step_timing.token = token;
        step_timing.began_ms = oa::platform::tick_ms();
        step_timing.framed = false;
        std::string phase = "screen setup ";
        phase.append(token);
        phase.append(" begin");
        oa::platform::log_files::note_timing(phase);
    };
    const auto note_setup_frame = [&] {
        if (step_timing.framed || step_timing.token.empty())
            return;
        std::string phase = "screen setup ";
        phase.append(step_timing.token);
        phase.append(" first frame");
        oa::platform::log_files::note_timing_span(phase, step_timing.began_ms);
        step_timing.framed = true;
    };
    note_setup_step();
    // The check's list is its own small install. A player's list is read
    // while the language step is on screen, and the game step draws at once.
    auto owned_scan = std::make_unique<SourceScan>();
    SourceScan& sources = *owned_scan;

    // A scan that is still reading when this function returns keeps running.
    // Joining it here would hold the menu behind the installer scan.
    struct ParkScan {
        std::unique_ptr<SourceScan>& owned;

        ~ParkScan() {
            if (!owned)
                return;
            if (owned->started && !owned->applied && !owned->done.load()) {
                scan_keeper().held.push_back(std::move(owned));
                return;
            }
            owned->finish();
        }
    } park_scan{owned_scan};

    if (options.check_setup_window) {
        if (session.model.step == first_run::Step::game)
            setup_install_check_source(user, session.model);
    } else {
        start_source_scan(sources, user, found_install);
    }

    std::unique_ptr<text_font::FontStack> fonts;
    const uint32_t fonts_began = oa::platform::tick_ms();
    try {
        fonts = text_font::FontStack::open(text_font::bundled_font_directory());
    } catch (const std::exception& error) {
        std::cerr << "open-annihilation: the setup's fonts could not be opened: " << error.what()
                  << '\n';
    }
    oa::platform::log_files::note_timing_span("fonts", fonts_began);
    RenderState render_state(display.renderer);
    WindowFrame frame(display.renderer, "the setup");
    oa::ui::game_files::Viewport viewport;
    WindowIcon icon_full;
    WindowIcon icon_shown;
    std::string icon_error;
    if (decode_window_icon(window_icon_png(), icon_full, icon_error))
        icon_shown = visible_part(icon_full);
    renderer::RgbaPicture app_icon;
    if (!icon_shown.pixels.empty())
        app_icon = {icon_shown.width, icon_shown.height, icon_shown.pixels};
    paint::Canvas pixels;
    bool dirty = true;
    bool awake = false;
    SetupBefore ended = SetupBefore::skipped;
    uint64_t last_ticks = 0;

    const auto relayout = [&](const kit::Frame& placed) {
        first_run::Scene laid = first_run::lay_out(session, placed, nullptr);
        first_run::ensure_focus(session, laid.list);
        return laid;
    };

    // The check's German sentences stay on one line at the picture's wide size.
    if (options.check_setup_window &&
        (!SDL_SetWindowSize(display.window, 1280, 720) || !SDL_SyncWindow(display.window)))
        throw std::runtime_error("setup window check: the window did not take 1280x720");
    if (read_window_viewport(display.window, display.renderer, viewport))
        absorb_render_safe(display.renderer, viewport);
    kit::Frame room = frame_of_window(viewport);
    // The window's own title bar names the game; full screen has none.
    const auto follow_window_mode = [&] {
        const bool windowed = (SDL_GetWindowFlags(display.window) & SDL_WINDOW_FULLSCREEN) == 0;
        if (windowed == session.model.windowed)
            return false;
        session.model.windowed = windowed;
        return true;
    };
    std::ignore = follow_window_mode();
    first_run::Scene scene = relayout(room);
    SetupWindowDrive window_check;

    /// The installer scan's dialog: shown once the game step has waited, and
    /// kept up long enough to read after the scan finishes.
    struct SetupProgress {
        bool shown = false;
        bool closed = false;
        uint32_t shown_at = 0;
        int last_eighth = -1;
    };

    SetupProgress progress_hold;
    const auto scan_waiting = [&] {
        return sources.started && !sources.applied && session.model.step == first_run::Step::game;
    };
    const auto progress_visible = [&](uint32_t now_ms) {
        if (!progress_hold.shown)
            return false;
        if (scan_waiting())
            return true;
        return now_ms - progress_hold.shown_at < long_work_stay_ms;
    };

    const auto present = [&] {
        if (viewport.width <= 0 || viewport.height <= 0)
            return;
        if (pixels.width != viewport.width || pixels.height != viewport.height)
            pixels = paint::make_canvas(viewport.width, viewport.height);
        paint::Painter painter(pixels);
        painter.fill_rect({0, 0, pixels.width, pixels.height}, paint::Rgba{0x1b, 0x1e, 0x19, 255});
        kit::Canvas canvas;
        canvas.smooth.painter = &painter;
        canvas.smooth.fonts = fonts.get();
        canvas.smooth.scale_percent = room.scale_percent;
        canvas.smooth.origin = {room.area.x, room.area.y};
        canvas.icon = app_icon;
        first_run::draw(canvas, scene, session);
        // The game step stays up while the installer scan finishes. After a
        // moment the progress dialog says so, over the step, and the step's
        // buttons stay live under it.
        const uint32_t now_ms = oa::platform::tick_ms();
        if (progress_visible(now_ms)) {
            const uint32_t eighth = kit::refresh_eighth(now_ms - progress_hold.shown_at);
            if (static_cast<int>(eighth) != progress_hold.last_eighth) {
                progress_hold.last_eighth = static_cast<int>(eighth);
                std::cerr << "open-annihilation: progress dialog: spinner\n";
            }
            kit::blend_rect(
                canvas,
                {0, 0, room.area.width, room.area.height},
                kit::colour::backdrop,
                kit::menu_backdrop_opacity
            );
            kit::WorkDialog dialog;
            dialog.title = std::string(languages::interface_text("Open Annihilation"));
            dialog.line =
                std::string(languages::interface_text("Checking your copy of Total Annihilation"));
            dialog.progress = -1;
            dialog.turn = eighth;
            dialog.size_class = room.size_class;
            const kit::PlacedWorkDialog placed = kit::place_work_dialog(dialog, nullptr);
            const int32_t box = kit::metrics_of(room.size_class).notice_width;
            const int32_t x = (room.area.width - box) / 2;
            const int32_t y = (room.area.height - placed.height) / 2;
            kit::draw_work_dialog(kit::placed_at(canvas, {x, y}), dialog);
        }
        static_cast<void>(frame.show(pixels, render_state, display.host));
        dirty = false;
        note_setup_frame();
    };

    const auto handle = [&](const first_run::Outcome& outcome) {
        if (outcome.ask == first_run::Ask::none) {
            dirty = dirty || outcome.redraw;
            return;
        }
        switch (outcome.ask) {
        case first_run::Ask::none:
            break;
        case first_run::Ask::skip:
            if (session.model.step == first_run::Step::copying)
                stop_running_copy(session, awake);
            write_preference(options, first_run::preference_key, first_run::preference_done);
            ended = SetupBefore::skipped;
            return;
        case first_run::Ask::quit:
            if (session.model.step == first_run::Step::copying)
                stop_running_copy(session, awake);
            ended = SetupBefore::quit;
            return;
        case first_run::Ask::back:
            if (session.model.step == first_run::Step::game)
                session.model.step = first_run::Step::language;
            else if (session.model.step == first_run::Step::online) {
                session.model.step = first_run::Step::copying;
                first_run::reset_copy_continue(session);
            }
            session.focus_ready = false;
            break;
        case first_run::Ask::advance:
            if (session.model.step == first_run::Step::language) {
                apply_selected_language(session, options, shown_tag);
                if (offers.downloads)
                    offers.downloads->set_language(shown_tag);
                session.model.step = first_run::Step::game;
                note_setup_step();
                // The check's install replaces the machine's list. A player's
                // list is already being read, and is shown when it is ready.
                if (options.check_setup_window)
                    setup_install_check_source(user, session.model);
                else
                    std::ignore = take_source_scan(sources, session.model);
                session.focus_ready = false;
            } else if (session.model.step == first_run::Step::game) {
                std::ignore = take_source_scan(sources, session.model);
                if (sources.started && !sources.applied)
                    break;
                if (setup_begin_copy(user, session.model)) {
                    session.model.step = first_run::Step::copying;
                    first_run::reset_copy_continue(session);
                    session.copy_continue_armed = true;
                    session.focus_ready = false;
                    if (!awake) {
                        SDL_DisableScreenSaver();
                        awake = true;
                    }
                }
            } else if (
                session.model.step == first_run::Step::copying && session.model.copy.checked
            ) {
                resume_state().continue_setup = true;
                resume_state().copied = true;
                ended = SetupBefore::copied;
                return;
            }
            break;
        case first_run::Ask::pick: {
            const int32_t picked = session.carousel.selected;
            apply_selected_language(session, options, shown_tag);
            if (offers.downloads)
                offers.downloads->set_language(shown_tag);
            if (picked >= 0 && static_cast<std::size_t>(picked) < session.model.cards.size()) {
                const first_run::Card& card = session.model.cards[static_cast<std::size_t>(picked)];
                if (card.kind == first_run::CardKind::pack &&
                    card.pack != first_run::PackState::downloading &&
                    card.pack != first_run::PackState::checking)
                    queue_language_pack(offers, card.tag);
            }
            break;
        }
        case first_run::Ask::locate:
            std::ignore = take_source_scan(sources, session.model);
            if (sources.started && !sources.applied)
                break;
            if (setup_locate_source(display.window, user, session.model))
                session.focus_ready = false;
            break;
        case first_run::Ask::pause_copy:
            setup_set_paused(!session.model.copy.paused);
            session.model.copy.paused = !session.model.copy.paused;
            break;
        case first_run::Ask::cancel_copy:
            setup_request_cancel();
            setup_stop_join();
            session.model.copy.running = false;
            session.model.copy.paused = false;
            session.model.copy.changed.clear();
            session.model.step = first_run::Step::game;
            first_run::reset_copy_continue(session);
            session.focus_ready = false;
            if (awake) {
                SDL_EnableScreenSaver();
                awake = false;
            }
            break;
        case first_run::Ask::cancel_download:
            if (offers.downloads && offers.download_item != 0)
                offers.downloads->cancel(offers.download_item);
            forget_language_view(offers);
            break;
        case first_run::Ask::retry_download:
            retry_language_pack(offers, session);
            break;
        case first_run::Ask::choose_on:
        case first_run::Ask::choose_off:
            break;
        case first_run::Ask::gog_open:
        case first_run::Ask::gog_paste:
        case first_run::Ask::gog_cancel:
            setup_gog_ask(outcome.ask, display.window, user, session.model);
            session.focus_ready = false;
            break;
        }
        note_setup_step();
        dirty = true;
    };

    WindowPress press;
    ended = SetupBefore::not_run;
    bool running = true;
    while (running) {
        if (read_window_viewport(display.window, display.renderer, viewport))
            dirty = true;
        absorb_render_safe(display.renderer, viewport);
        const kit::Frame fitted = frame_of_window(viewport);
        if (fitted.area.x != room.area.x || fitted.area.y != room.area.y ||
            fitted.area.width != room.area.width || fitted.area.height != room.area.height ||
            fitted.scale_percent != room.scale_percent) {
            room = fitted;
            dirty = true;
        }
        if (follow_window_mode())
            dirty = true;
        const int32_t continue_before = session.copy_continue_ms;
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                if (session.model.step == first_run::Step::copying && session.model.copy.running)
                    setup_request_cancel();
                setup_stop_join();
                ended = SetupBefore::quit;
                running = false;
                break;
            }
            if (const std::optional<kit::Key> pressed = setup_key(event)) {
                handle(first_run::key(session, scene, *pressed));
                continue;
            }
            if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
                if (first_run::stop_copy_continue(session))
                    dirty = true;
                kit::Key pressed = kit::Key::info;
                bool mapped = true;
                switch (event.gbutton.button) {
                case SDL_GAMEPAD_BUTTON_SOUTH:
                    pressed = kit::Key::enter;
                    break;
                case SDL_GAMEPAD_BUTTON_EAST:
                    pressed = kit::Key::escape;
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_UP:
                    pressed = kit::Key::up;
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
                    pressed = kit::Key::down;
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
                    pressed = kit::Key::left;
                    break;
                case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
                    pressed = kit::Key::right;
                    break;
                default:
                    mapped = false;
                    break;
                }
                if (mapped)
                    handle(first_run::key(session, scene, pressed));
                continue;
            }
            const PointerEvent pointer = read_pointer(event, viewport, press);
            if (pointer.step == PointerStep::down) {
                press.active = true;
                press.finger = pointer.finger;
                press.touch = pointer.touch;
                press.id = pointer.id;
            } else if (pointer.step == PointerStep::up || pointer.step == PointerStep::cancel) {
                press.active = false;
            }
            if (pointer.step == PointerStep::wheel) {
                const float notches = -pointer.wheel_points / wheel_notch_points;
                handle(first_run::wheel(session, scene, notches));
                continue;
            }
            if (pointer.step == PointerStep::none)
                continue;
            const first_run::PointerKind kind =
                pointer.step == PointerStep::down ? first_run::PointerKind::down
                : pointer.step == PointerStep::up ? first_run::PointerKind::up
                                                  : first_run::PointerKind::move;
            const int32_t reach = pointer.finger ? 22 : 0;
            handle(
                first_run::pointer(
                    session, scene, kind, point_of(pointer.x, pointer.y, room), reach, 1
                )
            );
        }
        if (!running)
            break;
        if (session.model.step == first_run::Step::copying) {
            first_run::CopyView view;
            if (setup_pull_copy(view)) {
                session.model.copy = std::move(view);
                dirty = true;
            }
        }
        const uint64_t now = SDL_GetTicks();
        uint32_t dt = 16;
        if (last_ticks != 0 && now >= last_ticks)
            dt = static_cast<uint32_t>(std::min<uint64_t>(now - last_ticks, 1000ULL));
        last_ticks = now;
        // The carousel's place is how long the move has been running. Draw
        // every frame of that move: a single paint when the key arrives
        // would hold the first frame until the move had already finished.
        const uint32_t elapsed_before = session.carousel.elapsed_ms;
        first_run::tick(session, dt);
        if (session.carousel.elapsed_ms != elapsed_before)
            dirty = true;
        if (refresh_language_cards(session, offers, shown_tag, options.check_first_run))
            dirty = true;
        if (take_source_scan(sources, session.model))
            dirty = true;
        if (session.model.step == first_run::Step::game) {
            const bool listed = !(sources.started && !sources.applied);
            const first_run::GogPhase was = session.model.gog.phase;
            const GogTick gog = setup_gog_tick(user, listed, session.model);
            if (gog != GogTick::none)
                dirty = true;
            if (session.model.gog.phase != was)
                session.focus_ready = false;
            // The installer is chosen: the copy begins as Next would begin it.
            if (gog == GogTick::ready) {
                first_run::Outcome advance;
                advance.ask = first_run::Ask::advance;
                handle(advance);
            }
        }
        const uint32_t progress_now = oa::platform::tick_ms();
        if (!progress_hold.shown && scan_waiting() &&
            progress_now - step_timing.began_ms >= long_work_show_after_ms) {
            progress_hold.shown = true;
            progress_hold.shown_at = progress_now;
            std::cerr << "open-annihilation: progress dialog: shown\n";
            oa::platform::log_files::note_timing("progress dialog shown");
        }
        if (progress_visible(progress_now))
            dirty = true;
        else if (progress_hold.shown && !progress_hold.closed) {
            progress_hold.closed = true;
            std::cerr << "open-annihilation: progress dialog: closed\n";
            oa::platform::log_files::note_timing("progress dialog closed");
        }
        if (session.copy_continue_ms != continue_before)
            dirty = true;
        if (session.model.step == first_run::Step::copying && session.copy_continue_ms == 0) {
            session.copy_continue_ms = -1;
            session.copy_continue_stopped = true;
            first_run::Outcome advance;
            advance.ask = first_run::Ask::advance;
            handle(advance);
        }
        if (dirty) {
            scene = relayout(room);
            present();
        }
        if (options.check_setup_window) {
            try {
                drive_setup_window_check(
                    window_check, display.window, user, session, scene, handle
                );
            } catch (...) {
                setup_request_cancel();
                setup_stop_join();
                if (awake) {
                    SDL_EnableScreenSaver();
                    awake = false;
                }
                release_picked_sources();
                throw;
            }
        }
        if (ended == SetupBefore::skipped || ended == SetupBefore::quit ||
            ended == SetupBefore::copied)
            running = false;
        else
            SDL_Delay(16);
    }
    if (awake)
        SDL_EnableScreenSaver();
    setup_stop_join();
    setup_gog_stop();
    release_picked_sources();
    if (ended == SetupBefore::not_run)
        ended = SetupBefore::quit;
    if (progress_hold.shown && !progress_hold.closed) {
        std::cerr << "open-annihilation: progress dialog: closed\n";
        oa::platform::log_files::note_timing("progress dialog closed");
    }
    return ended;
}

first_run::Step setup_menu_step() noexcept {
    return resume_state().continue_setup ? first_run::Step::online : first_run::Step::language;
}

first_run::Walk setup_menu_walk() noexcept {
    return resume_state().continue_setup ? first_run::Walk::setup : first_run::Walk::welcome;
}

first_run::Leave setup_menu_leave() noexcept {
    if (resume_state().leave_set)
        return resume_state().leave;
    return first_run::Leave::quit;
}

bool setup_copied_this_start() noexcept {
    return resume_state().copied;
}

void setup_install_check_source(const fs::path& user_folder, first_run::Model& model) {
    model.sources.clear();
    model.source = -1;
    remembered_sources().clear();
    if (user_folder.empty())
        return;
    const fs::path folder = user_folder / "fixture-install";
    if (!write_fixture(folder)) {
        std::cerr << "open-annihilation: the setup check could not write its install\n";
        return;
    }
    FolderContentSource source(folder, copy_check_bundles());
    SourceDescription described;
    std::string error;
    if (!describe_source(source, copy_check_bundles(), described, &error)) {
        std::cerr << "open-annihilation: the setup check could not read its install"
                  << (error.empty() ? "" : ": ") << error << '\n';
        return;
    }
    add_described(
        model,
        remembered_sources(),
        folder,
        SourceKind::fixture,
        first_run::SourceMark::folder,
        {},
        described,
        copy_check_bundles()
    );
    if (!model.sources.empty()) {
        model.source = 0;
        // The copying step's "from" line says this is the check's install,
        // and the check that follows uses the check's table.
        model.sources.front().title = "Total Annihilation";
    }
}

bool setup_begin_copy(const fs::path& user_folder, const first_run::Model& model) {
    if (model.source < 0 || static_cast<std::size_t>(model.source) >= model.sources.size())
        return false;
    const std::string token = model.sources[static_cast<std::size_t>(model.source)].token;
    const Remembered* chosen = nullptr;
    for (const Remembered& remembered : remembered_sources()) {
        if (remembered.token == token)
            chosen = &remembered;
    }
    if (chosen == nullptr || user_folder.empty())
        return false;
    setup_stop_join();
    CopyJob& job = copy_job();
    job.stop.store(false);
    job.paused = false;
    job.changed = true;
    job.started = std::chrono::steady_clock::now();
    job.bundle_ids.clear();
    job.view = {};
    job.view.from = path_to_utf8(chosen->path.filename());
    job.view.running = true;
    job.view.whole = 1000;
    const bool fixture = chosen->kind == SourceKind::fixture;
    job.needs_check = false;
    job.pending_check = {};
    job.copied_bundle_ids.clear();
    // A copy that does not finish must not leave an earlier result for the
    // start-up check. A finished copy replaces this before the menu opens.
    keep_this_run_copy_check({});
    job.bytes_done = 0;
    job.bytes_total = 0;
    job.files_done = 0;
    job.files_total = 0;
    const std::span<const bundles::Bundle> table =
        fixture ? copy_check_bundles() : bundles::bundles();
    const first_run::GameSource& chosen_row = model.sources[static_cast<std::size_t>(model.source)];
    prime_bundles(job, table, chosen_row.chips);
    const Remembered copy = *chosen;
    job.worker = std::thread([copy, user_folder] {
        CopyJob& held = copy_job();
        CopyResult result;
        try {
            const std::unique_ptr<ContentSource> source = open_source(copy);
            if (!source) {
                result.status = CopyStatus::failed;
            } else {
                CopyRequest request;
                request.user_folder = user_folder;
                request.bundles =
                    copy.kind == SourceKind::fixture ? copy_check_bundles() : bundles::bundles();
                request.stop = &held.stop;
                request.on_progress = on_copy_progress;
                request.progress_context = &held;
                request.news = &std::cerr;
                result = copy_content(*source, request);
            }
        } catch (const std::exception& error) {
            result.status = CopyStatus::failed;
            result.error = error.what();
        }
        const bool bytes_done = result.status == CopyStatus::finished;
        const bool run_check = bytes_done && !held.stop.load();
        const std::string error = result.error;
        if (!run_check) {
            const std::lock_guard<std::mutex> lock(held.mutex);
            held.view.running = false;
            held.view.paused = false;
            held.view.hashing = false;
            held.view.bytes_landed = bytes_done;
            // A changed install copies nothing. The step asks where to copy
            // from instead, naming the archives that stopped it and, in
            // English, what each belongs to; the step shows that name in the
            // language in use.
            if (result.status == CopyStatus::changed) {
                const std::span<const bundles::Bundle> table =
                    copy.kind == SourceKind::fixture ? copy_check_bundles() : bundles::bundles();
                for (const ChangedContentFile& changed : result.changed) {
                    std::string owner = changed.bundle_id;
                    for (const bundles::Bundle& bundle : table) {
                        if (bundle.id == changed.bundle_id)
                            owner = std::string(bundle.english_name);
                    }
                    held.view.changed.push_back(
                        {changed.copy_name, std::move(owner), changed.source_size, changed.size}
                    );
                }
                held.changed = true;
                return;
            }
            if (!bytes_done && result.status != CopyStatus::stopped && !held.stop.load()) {
                held.view.checked = false;
                held.view.verdict =
                    error.empty() ? std::string("The game could not be copied.") : error;
            }
            held.changed = true;
            return;
        }
        {
            const std::lock_guard<std::mutex> lock(held.mutex);
            held.view.bytes_landed = true;
            held.view.hashing = true;
            held.view.running = true;
            for (first_run::CopyBundle& row : held.view.bundles)
                row.file.clear();
            held.view.check_done = 0;
            held.view.check_total = 0;
            held.changed = true;
        }
        ContentCheck check;
        std::string failure;
        bool threw = false;
        try {
            const std::span<const bundles::Bundle> checked =
                copy.kind == SourceKind::fixture ? copy_check_bundles() : bundles::bundles();
            check = check_managed_content(user_folder, checked, on_copy_check, &held, &held.stop);
        } catch (const std::exception& caught) {
            threw = true;
            failure = caught.what();
        }
        const bool stopped = held.stop.load();
        const std::lock_guard<std::mutex> lock(held.mutex);
        held.view.hashing = false;
        held.view.running = false;
        held.view.paused = false;
        if (threw) {
            held.view.checked = false;
            held.view.verdict = std::move(failure);
        } else if (!stopped) {
            ContentCheck for_start;
            for (const ContentBundleCheck& bundle : check.bundles) {
                bool copied = false;
                for (const std::string& id : held.copied_bundle_ids) {
                    if (id == bundle.id)
                        copied = true;
                }
                if (copied)
                    for_start.bundles.push_back(bundle);
            }
            // Only bundles this copy reported. One already in the folder, and
            // not part of this copy, is checked again when the menu opens.
            keep_this_run_copy_check(std::move(for_start));
            held.pending_check = std::move(check);
            held.needs_check = true;
        }
        held.changed = true;
    });
    return true;
}

void setup_set_paused(bool paused) {
    CopyJob& job = copy_job();
    {
        const std::lock_guard<std::mutex> lock(job.mutex);
        job.paused = paused;
        job.view.paused = paused;
        job.changed = true;
    }
    job.wake.notify_all();
}

void setup_request_cancel() {
    CopyJob& job = copy_job();
    job.stop.store(true);
    {
        const std::lock_guard<std::mutex> lock(job.mutex);
        job.paused = false;
        job.view.paused = false;
    }
    job.wake.notify_all();
}

bool setup_pull_copy(first_run::CopyView& view) {
    CopyJob& job = copy_job();
    ContentCheck pending;
    bool check_now = false;
    {
        const std::lock_guard<std::mutex> lock(job.mutex);
        check_now = job.needs_check;
        if (check_now) {
            pending = std::move(job.pending_check);
            job.pending_check = {};
            job.needs_check = false;
        }
    }
    // The copy's thread already hashed. This thread only writes the lines.
    if (check_now) {
        const std::lock_guard<std::mutex> lock(job.mutex);
        apply_check(job.view, pending, true);
        job.changed = true;
    }
    const std::lock_guard<std::mutex> lock(job.mutex);
    view = job.view;
    if (job.files_total > 0)
        view.overall = overall_line(job.files_done, job.files_total);
    if (view.running && !view.paused && !view.hashing)
        view.remaining = remaining_line(job, job.bytes_done, job.bytes_total);
    if (view.verdict == "The game could not be copied.")
        view.verdict = std::string(languages::interface_text(view.verdict));
    const bool changed = job.changed;
    job.changed = false;
    return changed;
}

void setup_stop_join() {
    CopyJob& job = copy_job();
    job.stop.store(true);
    {
        const std::lock_guard<std::mutex> lock(job.mutex);
        job.paused = false;
    }
    job.wake.notify_all();
    if (job.worker.joinable())
        job.worker.join();
}

bool setup_locate_source(SDL_Window* window, const fs::path& user_folder, first_run::Model& model) {
    PickWait wait;
    const GameFilesHooks& hooks = game_files_hooks();
    if (hooks.show_picker != nullptr) {
        hooks.show_picker(hooks.context, PickKind::game_folder, picker_done, &wait);
        if (!pump_until_picked(wait))
            return false;
    } else {
        // The title says which folder qualifies. SDL copies it during Show.
        const std::string title =
            std::string(languages::interface_text("Choose the folder that holds totala1.hpi."));
        const SDL_PropertiesID properties = SDL_CreateProperties();
        if (properties == 0)
            return false;
        (void)SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
        if (window != nullptr)
            (void)SDL_SetPointerProperty(properties, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, window);
        (void)SDL_SetBooleanProperty(properties, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, false);
        SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, folder_done, &wait, properties);
        SDL_DestroyProperties(properties);
        if (!pump_until_picked(wait))
            return false;
    }
    bool added = false;
    for (const std::string& path : wait.paths) {
        const fs::path folder = path_from_utf8(path);
        const std::size_t before = model.sources.size();
        std::error_code failure;
        if (fs::is_directory(folder, failure) && !failure)
            add_folder_source(
                model,
                remembered_sources(),
                folder,
                user_folder,
                {},
                bundles::bundles(),
                SourceKind::folder
            );
        else
            add_installer_source(model, remembered_sources(), folder, user_folder);
        // add_folder_source and add_installer_source select the fullest new row.
        if (model.sources.size() != before)
            added = true;
    }
    return added;
}

void setup_release_located() {
    release_picked_sources();
}

namespace {

/// The GOG download and what the game step last showed of it.
struct GogState {
    std::unique_ptr<gog::Download> download{};
    /// The game's own window with GOG's page, which hands the address over.
    std::unique_ptr<oa::platform::web_sign_in::Window> window{};
    /// The clipboard's text when GOG's page was opened, so an address copied
    /// before then is not taken for the new one.
    std::string clipboard_before{};
    uint64_t clipboard_checked_ms{};
    gog::Phase shown{gog::Phase::idle};
    uint64_t shown_received{};
    uint64_t shown_ms{}; ///< when the progress was last shown, in SDL ticks
};

GogState& gog_state() {
    static GogState state;
    return state;
}

/// How often the clipboard is read while GOG's page is open, in milliseconds.
constexpr uint64_t gog_clipboard_every_ms = 500;
/// How often the download's progress is shown, in milliseconds.
constexpr uint64_t gog_progress_every_ms = 100;

/// The clipboard's text; empty when it holds none.
std::string clipboard_text() {
    if (!SDL_HasClipboardText())
        return {};
    char* text = SDL_GetClipboardText();
    if (text == nullptr)
        return {};
    std::string copied(text);
    SDL_free(text);
    return copied;
}

/// The folder the installer is downloaded to: the player's Downloads, where
/// the game step's own search finds it again, else one in the player's folder.
fs::path gog_folder(const fs::path& user_folder) {
    const fs::path home = installs::home_folder();
    if (!home.empty())
        return home / "Downloads";
    return user_folder / "Downloads";
}

/// Opens GOG's sign-in page in the web browser, where the player copies the
/// address it ends on.
void open_gog_in_browser(GogState& state, first_run::Model& model) {
    state.clipboard_before = clipboard_text();
    state.clipboard_checked_ms = SDL_GetTicks();
    model.gog.in_window = false;
    const std::string address = gog::sign_in_address();
    if (!SDL_OpenURL(address.c_str()))
        std::cerr << "open-annihilation: gog: the browser did not open: " << SDL_GetError()
                  << '\n';
    std::cerr << "open-annihilation: gog: sign-in page opened in the browser\n";
}

/// A kit colour as the sign-in panel takes one, 0xRRGGBB.
uint32_t rgb_of(kit::Colour colour) {
    return (uint32_t{colour.r} << 16) | (uint32_t{colour.g} << 8) | colour.b;
}

/// The sign-in panel's frame in the colours of the game's dialogs.
oa::platform::web_sign_in::Look sign_in_look() {
    oa::platform::web_sign_in::Look look;
    look.face = rgb_of(kit::colour::panel);
    look.band = rgb_of(kit::colour::band);
    look.rule = rgb_of(kit::colour::rule);
    look.edge_light = rgb_of(kit::colour::edge_light);
    look.edge_dark = rgb_of(kit::colour::edge_dark);
    look.text = rgb_of(kit::colour::text);
    look.quiet = rgb_of(kit::colour::hint);
    look.hover = rgb_of(kit::colour::hover);
    look.held = rgb_of(kit::colour::list_selected);
    return look;
}

/// Opens GOG's sign-in page in a panel inside the game's window, else in the
/// web browser.
void open_gog(GogState& state, SDL_Window* window, first_run::Model& model) {
    namespace web = oa::platform::web_sign_in;
    if (!state.window)
        state.window = std::make_unique<web::Window>();
    web::Request request;
    request.address = gog::sign_in_address();
    request.landing = std::string(gog::landing_address());
    request.title = std::string(languages::interface_text("Sign in to GOG"));
    request.look = sign_in_look();
    if (window != nullptr)
        request.parent = SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr
        );
    if (state.window->open(request)) {
        model.gog.in_window = true;
        std::cerr << "open-annihilation: gog: sign-in window opened\n";
        return;
    }
    open_gog_in_browser(state, model);
}

/// Starts the download with a code, and shows the question working.
void start_gog(
    GogState& state, std::string code, const fs::path& user_folder, first_run::Model& model
) {
    if (!state.download)
        state.download = std::make_unique<gog::Download>();
    state.download->start(std::move(code), gog_folder(user_folder));
    state.shown = gog::Phase::signing_in;
    state.shown_received = 0;
    model.gog = {};
    model.gog.phase = first_run::GogPhase::working;
    std::cerr << "open-annihilation: gog: signing in\n";
}

/// The amount line: how much of the installer has arrived.
std::string gog_amount(uint64_t received, uint64_t size) {
    std::string text{languages::interface_text("{done} of {total} · {percent}%")};
    const uint64_t percent = size == 0 ? 0 : (received * 100ULL) / size;
    replace_token(text, "{done}", settings::download_size_text(received));
    replace_token(text, "{total}", settings::download_size_text(size));
    replace_token(text, "{percent}", std::to_string(percent));
    return text;
}

} // namespace

bool setup_gog_offered() noexcept {
    return gog::available();
}

void setup_gog_ask(
    first_run::Ask ask, SDL_Window* window, const fs::path& user_folder, first_run::Model& model
) {
    GogState& state = gog_state();
    switch (ask) {
    case first_run::Ask::gog_open:
        if (state.download)
            state.download->cancel();
        model.gog = {};
        model.gog.phase = first_run::GogPhase::sign_in;
        open_gog(state, window, model);
        break;
    case first_run::Ask::gog_paste: {
        std::optional<std::string> code = gog::login_code(clipboard_text());
        if (code) {
            start_gog(state, std::move(*code), user_folder, model);
            break;
        }
        model.gog.failure = std::string(languages::interface_text(
            "The clipboard holds no GOG sign-in address. Copy the whole address from the "
            "browser's address bar first."
        ));
        break;
    }
    case first_run::Ask::gog_cancel:
        if (state.window)
            state.window->close();
        if (state.download)
            state.download->cancel();
        state.shown = gog::Phase::idle;
        model.gog = {};
        std::cerr << "open-annihilation: gog: closed\n";
        break;
    default:
        break;
    }
}

GogTick setup_gog_tick(const fs::path& user_folder, bool sources_ready, first_run::Model& model) {
    GogState& state = gog_state();
    if (model.gog.phase == first_run::GogPhase::none)
        return GogTick::none;
    if (model.gog.phase == first_run::GogPhase::sign_in && model.gog.in_window) {
        namespace web = oa::platform::web_sign_in;
        const web::State window = state.window ? state.window->state() : web::State::closed;
        if (window == web::State::landed) {
            std::optional<std::string> code = gog::login_code(state.window->landed_address());
            if (code) {
                start_gog(state, std::move(*code), user_folder, model);
                return GogTick::changed;
            }
            std::cerr << "open-annihilation: gog: the sign-in ended without a code\n";
            model.gog = {};
            model.gog.phase = first_run::GogPhase::failed;
            model.gog.failure =
                std::string(languages::interface_text(gog::failure_text(gog::Failure::sign_in)));
            return GogTick::changed;
        }
        if (window == web::State::closed) {
            // The player closed the window: the question closes with it.
            std::cerr << "open-annihilation: gog: sign-in window closed\n";
            model.gog = {};
            return GogTick::changed;
        }
        if (window == web::State::failed) {
            std::cerr << "open-annihilation: gog: the sign-in window could not start\n";
            open_gog_in_browser(state, model);
            return GogTick::changed;
        }
        return GogTick::none;
    }
    if (model.gog.phase == first_run::GogPhase::sign_in) {
        const uint64_t now = SDL_GetTicks();
        if (state.clipboard_checked_ms != 0 &&
            now - state.clipboard_checked_ms < gog_clipboard_every_ms)
            return GogTick::none;
        state.clipboard_checked_ms = now;
        const std::string copied = clipboard_text();
        if (copied == state.clipboard_before)
            return GogTick::none;
        std::optional<std::string> code = gog::login_code(copied);
        if (!code)
            return GogTick::none;
        start_gog(state, std::move(*code), user_folder, model);
        return GogTick::changed;
    }
    if (model.gog.phase == first_run::GogPhase::failed || !state.download)
        return GogTick::none;
    const gog::Status status = state.download->status();
    if (status.phase == gog::Phase::done) {
        if (!sources_ready)
            return GogTick::none;
        std::cerr << "open-annihilation: gog: downloaded " << path_to_utf8(status.installer)
                  << '\n';
        state.download.reset();
        state.shown = gog::Phase::idle;
        if (!add_downloaded_installer(
                model, remembered_sources(), status.installer, user_folder
            )) {
            model.gog = {};
            model.gog.phase = first_run::GogPhase::failed;
            std::string text{languages::interface_text(
                "The installer was saved in {folder}, but it could not be read."
            )};
            replace_token(text, "{folder}", path_to_utf8(status.installer.parent_path()));
            model.gog.failure = std::move(text);
            return GogTick::changed;
        }
        model.gog = {};
        return GogTick::ready;
    }
    if (status.phase == gog::Phase::failed) {
        std::cerr << "open-annihilation: gog: failed: " << status.detail << '\n';
        state.shown = gog::Phase::idle;
        if (status.failure == gog::Failure::cancelled) {
            model.gog = {};
            return GogTick::changed;
        }
        model.gog = {};
        model.gog.phase = first_run::GogPhase::failed;
        model.gog.failure =
            std::string(languages::interface_text(gog::failure_text(status.failure)));
        return GogTick::changed;
    }
    if (status.phase == gog::Phase::downloading && status.size > 0) {
        const uint64_t now = SDL_GetTicks();
        if (state.shown == gog::Phase::downloading &&
            (status.received == state.shown_received ||
             now - state.shown_ms < gog_progress_every_ms))
            return GogTick::none;
        state.shown_ms = now;
        state.shown = gog::Phase::downloading;
        state.shown_received = status.received;
        model.gog.phase = first_run::GogPhase::downloading;
        model.gog.amount = gog_amount(status.received, status.size);
        // The bar counts kilobytes, so its numbers stay within int32_t.
        model.gog.whole = static_cast<int32_t>(std::max<uint64_t>(status.size >> 10, 1));
        model.gog.done = static_cast<int32_t>(status.received >> 10);
        return GogTick::changed;
    }
    return GogTick::none;
}

void setup_gog_stop() {
    GogState& state = gog_state();
    state.window.reset();
    state.download.reset();
    state.shown = gog::Phase::idle;
}

} // namespace oa::app
