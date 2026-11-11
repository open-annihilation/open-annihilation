// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Library's Replays section in the running game: a worker that reads the
// player's Recordings folder into its index (oa/data/replays/replays.hpp),
// the rows the model is given with their local day and time and their maps'
// pictures, and the section's actions. WATCH hands a replay's bytes to the
// extension that replays recordings (Extension::open_recording), which starts
// its match in the running game; the Library keeps what it showed, and the
// main menu opens it again on the same replay when the match is left. A
// recording the extension refuses after its match began loading is
// torn down again, and the Library, still open, shows the refusal in the
// card. KEEP marks replays in `kept.json`, by recording, which the index
// never holds; OPEN FOLDER shows a replay's folder and TURN ON RECORDING turns
// the player's setting on. EXPORT opens at once and measures each replay on
// its own worker, then writes the chosen format a chunk at a time. A file
// that was written marks its recording, so the keep rule leaves it. While the
// Library is open and no match is running, one low-priority process reads
// each replay that has no timeline yet. Its own output stays in its folder.
// A replay Watch would have to fetch, turn on or refuse is left out, and a
// replay whose mod is installed is read with that mod. A pass that cannot
// read a file remembers the installed content and tries again when that
// changes; it does not store an empty timeline. Closing the Library, a
// match and Watch stop it, and a stopped pass writes nothing. Where a Watch
// was left is written into the index once the match has gone, including a
// replay the game restarted to switch the mod or the expansions. A replay
// that had finished stores no tick, so the card offers Watch.

#include "library_host.hpp"
#include "replay_index_pass.hpp"

#include "engine_settings_state.hpp"
#include "map_packs.hpp"
#include "oa_layer.hpp"
#include "user_folder_state.hpp"

#include "oa/app/content/updates.hpp"
#include "oa/app/extension.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/app/mod_profile_loader.hpp"
#include "oa/app/package_install/inbox.hpp"
#include "oa/app/package_install/oamap.hpp"
#include "oa/app/package_install/oamod.hpp"
#include "oa/app/package_install/replay_release.hpp"
#include "oa/app/platform_hooks.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/base/sha256.hpp"
#include "oa/base/threads.hpp"
#include "oa/data/content_bundles/content_choice.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/map_pack/manifest.hpp"
#include "oa/data/map_pack/map_name.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/data/replays/content.hpp"
#include "oa/data/replays/import.hpp"
#include "oa/data/replays/interface_words.hpp"
#include "oa/data/replays/replays.hpp"
#include "oa/data/saves/content_record.hpp"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/oademo/timeline_index.hpp"
#include "oa/formats/ota.hpp"
#include "oa/formats/tad.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/game_installs.hpp"
#include "oa/platform/trash.hpp"
#include "oa/session/demo/imported_recording.hpp"
#include "oa/session/demo/replay_record.hpp"
#include "oa/ui/folder_chooser.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/library/export.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/replays.hpp"
#include "oa/ui/library/share.hpp"
#include "oa/ui/library/screen.hpp"
#include "oa/ui/library/text.hpp"
#include "oa/ui/map_previews/map_previews.hpp"

#ifndef OA_PROCESS_SPAWNING
#error "OA_PROCESS_SPAWNING (0 or 1) says whether the game may start other programs"
#endif

#include <SDL3/SDL.h>
#include <SDL3/SDL_dialog.h>

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <ostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

#ifndef OA_ENGINE_VERSION
#error "OA_ENGINE_VERSION names the engine a wrapped recording is exported with"
#endif

namespace oa::app {

namespace fs = std::filesystem;
namespace kit = oa::ui::kit;
namespace library = oa::ui::library;
namespace replay_data = oa::data::replays;
namespace previews = oa::ui::map_previews;
namespace oademo = oa::formats::oademo;
namespace tad = oa::formats::tad;
namespace chooser = oa::ui::folder_chooser;
namespace saves = oa::data::saves;
namespace demo = oa::session::demo;
namespace installs = oa::platform::game_installs;

// A `.tad` with no note plays with the mod its folder names, as recordings_folder names it.
static_assert(replay_data::no_mod_recordings_folder == no_mod_folder_name);
static_assert(replay_data::default_id_recordings_folder == default_id_folder_name);

/// The Replays section's own state.
struct ReplayShelf {
    /// Guards every member down to `moves`.
    base::threads::Mutex mutex{};
    base::threads::ConditionVariable wake{}; ///< signalled when a read is due or the worker stops
    base::threads::Thread thread{};          ///< the worker, once started
    bool started{};                          ///< the worker was started
    bool stop{};                             ///< the worker stops at its next wait
    bool due{};                              ///< a read of the folder is asked for
    fs::path recordings{};                   ///< the folder the next read reads
    bool load{true};                         ///< the next read loads the index file first
    replay_data::Index from{};               ///< the index the next read starts from
    bool scanning{};                         ///< a read is under way
    /// A replay opened from outside, copied on this worker. The main thread
    /// shows the notice once it has ended and no modal screen shows.
    bool import_due{};            ///< an import is asked for
    fs::path import_recordings{}; ///< the Recordings folder it copies into
    fs::path import_source{};     ///< the file it reads
    bool import_running{};        ///< an import is under way
    std::optional<replay_data::ImportResult> import_done{}; ///< an import that ended
    std::vector<replay_data::Replay> partial{}; ///< what the read under way has given so far
    std::size_t done{};                         ///< files the read under way has done
    std::size_t total{};                        ///< files it found
    std::optional<replay_data::Index> result{}; ///< a read that ended, for the main thread
    uint64_t moves{};                           ///< moves on whenever anything above changes
    /// Set as the shelf goes: a read stops before its next file.
    std::atomic<bool> cancel{false};

    // The main thread's own.
    replay_data::Index index{};                   ///< the last whole index
    bool listed{};                                ///< a read has ended at least once
    uint64_t moves_seen{};                        ///< the worker's moves the model last took
    bool reading{};                               ///< the model shows a read under way
    std::size_t read{};                           ///< how far it is, as the model shows it
    std::size_t to_read{};                        ///< files it has to read, as the model shows it
    std::vector<replay_data::Replay> rows_read{}; ///< the first read's rows so far
    /// The replays the player keeps, as `kept.json` holds them: read when the
    /// shelf first reads the folder, and saved at once when a mark changes,
    /// so a Keep pressed during a read is never lost.
    replay_data::KeptMarks kept{};
    bool kept_loaded{}; ///< `kept` was read from the folder
    /// The read of kept.json failed. The bytes were set aside. A Delete writes
    /// a new file only after that, and never over the damaged one.
    bool kept_damaged{};
    /// kept.json was set aside this start. The consideration that sees it
    /// removes nothing.
    bool kept_set_aside{};
    /// Replays the player exported, as exported.json holds them.
    replay_data::KeptMarks exported{};
    bool exported_loaded{};  ///< `exported` was read from the folder
    bool exported_damaged{}; ///< the read of exported.json failed
    /// The maps' pictures: the cache that decodes them, the palette, and the
    /// pictures decoded, by map name.
    previews::PreviewCache cache{64, 96};
    std::optional<oa::PaletteBytes> palette{};
    bool palette_tried{};
    std::map<std::string, library::MapPicture> pictures{};
    Runtime* runtime{}; ///< the runtime whose game files the cache reads
    /// Reads a range of one of the runtime's game files (LibraryHost::read_game_chunk).
    bool (*read_chunk)(
        Runtime& runtime, const char* path, uint32_t offset, std::span<uint8_t> out
    ){};
    /// The replay WATCH opened, until its match has ended and it is closed.
    std::optional<ReplayHooks> watching{};
    /// A replay opened from outside, selected once the folder's read shows it.
    /// Relative to Recordings. Empty when none waits.
    std::string land_path{};
    /// The platform's copy of a file being imported, handed back when the
    /// import ends. The main thread's own. Empty when the file was read where
    /// it was opened.
    fs::path import_release{};
    /// Paths moved to the trash this start. A read that listed one before the
    /// move finished does not write it back into the index.
    std::vector<std::string> gone{};
    /// The tick a Watch was left at, sampled while its match ran. Written
    /// into the index once the match has gone.
    std::optional<uint32_t> left_tick{};
    /// The replay those samples belong to, below Recordings.
    std::string left_path{};
    /// A Watch from that waits until the replay can say where it is.
    std::optional<uint32_t> pending_seek{};
    /// The replay had finished when it was left: the hooks said so, or the
    /// tick was the index's length. The card then offers Watch, and an
    /// earlier watched tick is cleared.
    bool left_finished{};
    /// The index's length in ticks, read when Watch starts. Nothing when the
    /// pass has not stored a timeline.
    std::optional<uint32_t> watch_length{};
#if !defined(__ANDROID__) && OA_PROCESS_SPAWNING
    /// The index pass's process. One at a time. Null while none runs.
    SDL_Process* index_process{};
    int index_pid{};               ///< its process id; 0 when unnamed
    std::string index_target{};    ///< the replay it is reading, below Recordings
    fs::path index_output{};       ///< the file it writes
    fs::path index_errors{};       ///< its standard error, in the pass's folder
    fs::path index_scratch{};      ///< its folder, under the player's own
    int index_fail_count{};        ///< spawns that failed for index_fail_path
    std::string index_fail_path{}; ///< the replay those failures belong to
    int index_backoff{};           ///< frames to wait before the next spawn
#endif

    /// One replay measured for export. `bytes` is the chosen format only.
    /// The export worker never touches this vector: it publishes a result,
    /// and the main thread moves that here.
    struct HeldExport {
        std::string path{};
        std::string title{};
        bool unreadable{};
        bool ready{};
        bool written{};
        /// Why it could not be measured, for the log.
        std::string why{};
        uint64_t oademo_bytes{};
        uint64_t tad_bytes{};
        uint64_t tad_without_bytes{};
        bool names_cut{};
        bool status_cut{};
        bool wide_records{};
        bool mutators{};
        std::vector<std::string> mutator_ids{};
        std::vector<std::string> mutator_names{};
        /// `timeline.json` copied from the index on the main thread. Empty
        /// when the pass has not read this replay.
        std::string timeline{};
        /// `bytes` is a `.oademo`. Otherwise it is a `.tad`.
        bool held_oademo{};
        /// A held `.tad` kept Open Annihilation's own sectors.
        bool held_sectors{true};
        std::vector<uint8_t> bytes{};
    };

    /// What the export worker is asked to measure.
    struct ExportAsk {
        uint64_t generation{};
        /// Replace the chosen bytes only. The sizes already on the dialog stay.
        bool rebuild{};
        fs::path recordings{};
        std::vector<std::string> paths{};
        std::vector<std::string> titles{};
        /// One timeline for each path, copied on the main thread. Empty when
        /// the index has none the export should carry.
        std::vector<std::string> timelines{};
    };

    /// A measurement the worker finished, for the main thread.
    struct ExportResult {
        uint64_t generation{};
        bool rebuild{};
        std::vector<HeldExport> held{};
    };

    /// The file the main thread is writing, one chunk a frame. It is not
    /// assigned as a whole: the stream does not copy.
    struct ExportJob {
        bool active{};
        int hold{};
        fs::path folder{};
        fs::path destination{};
        fs::path partial{};
        std::ofstream output{};
        bool opened{};
        std::size_t piece{};
        std::size_t offset{};
        uint32_t files_done{};
        uint32_t files_total{};
        bool as_oademo{true};
        bool sectors{true};
        std::string extension{};
    };

    /// Guards the export worker's request and its result. Never held together
    /// with `mutex`.
    base::threads::Mutex export_mutex{};
    base::threads::ConditionVariable export_wake{};
    base::threads::Thread export_thread{};
    bool export_started{};
    bool export_stop{};
    bool export_pending{};
    ExportAsk export_ask{};
    std::optional<ExportResult> export_result{};
    /// Set to stop the measurement under way. Cleared when the worker takes
    /// the next request.
    std::atomic<bool> export_cancel{false};
    std::atomic<uint64_t> export_generation{0};
    std::atomic<bool> export_busy{false};
    /// The format the dialog is showing, copied each pump. The worker reads
    /// them just before it keeps a file's bytes.
    std::atomic<bool> export_want_oademo{true};
    std::atomic<bool> export_want_sectors{true};
    std::atomic<uint64_t> export_byte_done{0};
    std::atomic<uint64_t> export_byte_total{0};
    std::atomic<bool> export_compressing{false};
    std::atomic<uint64_t> export_progress_stamp{0};
    uint64_t export_progress_seen{};
    /// A rebuild was asked for and has not been published yet.
    bool export_rebuild_asked{};
    std::vector<HeldExport> held{};
    ExportJob job{};

    /// Packages Get asked for, watched once they are installed.
    struct PendingGet {
        std::string path{}; ///< the replay the card is selected by
        /// The file below Recordings when `path` is not that file. Empty
        /// watches `path`. A Core Prime replay's card is selected by its id.
        std::string watch_path{};
        fs::path file{};                    ///< that file on disk
        std::vector<uint64_t> items{};      ///< the downloads queue returned
        std::vector<std::string> turn_on{}; ///< expansions to turn on before watching
        bool map_pack{};                    ///< a map pack was among the downloads
        std::string mod_id{};               ///< a mod to play this replay with, once installed
        /// A release kept beside the one in use, already on disk.
        fs::path beside_mod{};
        /// The digest of a release being installed beside the one in use.
        /// The folder is Replay releases/<sha> once the download has finished.
        std::string beside_mod_sha{};
        /// The digest of a map pack being installed beside the one in use.
        /// The folder is Replay releases/<sha> once the download has finished.
        /// It is not a mod folder.
        std::string beside_pack_sha{};
        /// The folder of a map pack the row already matched. Watch mounts
        /// this pack. It is not passed as a mod.
        std::string pack_folder{};
        /// The folder of a release the row already matched. Watch plays this,
        /// not the first Mods folder of the id.
        std::string play_folder{};
        /// True when the mod need names a version, a revision or a digest.
        /// The first Mods folder of the id is used only when this is false.
        bool mod_names_release{};
        std::string mod_version{};
        std::string mod_sha{};
        std::optional<int64_t> mod_revision{};
        /// The tick Watch from jumps to after the restart. Nothing starts
        /// at the beginning.
        std::optional<uint32_t> from{};
    };

    std::optional<PendingGet> pending_get{};
    /// Stock map stems and mission names, read once until a pack is installed.
    std::vector<std::string> stock_maps{};
    bool stock_maps_read{};
    /// The catalogue snapshot the rows' gaps were compared against.
    uint64_t content_generation{};

    /// A digest a registry answered 404 or 409 for, until a later catalogue
    /// sequence lists it again.
    struct NotOffered {
        std::string sha256{};
        std::string registry{};
        int64_t sequence = 0;
    };

    std::vector<NotOffered> not_offered{};

    /// A registry a press could not reach. It stays until a refresh moves
    /// `checked` forward, or a download from that registry finishes.
    struct RegistryAway {
        std::string id{};
        std::optional<int64_t> checked{};
    };

    std::vector<RegistryAway> away{};

    /// One release indexed by digest or by expansion id.
    struct IndexedRelease {
        replay_data::ListedRelease offer{};
        int64_t sequence = 0;
    };

    /// Digests and expansion ids, built once per catalogue generation.
    struct OfferBook {
        uint64_t generation = 0;
        bool built = false;
        std::map<std::string, IndexedRelease> by_sha{};
        std::map<std::string, IndexedRelease> failed_sha{};
        std::map<std::string, IndexedRelease> expansions{};
        std::map<std::string, IndexedRelease> failed_expansions{};
        /// The mods the catalogues offer now, for a `.tad` that names a mod
        /// by its name or its id and no release. Live registries first.
        std::vector<IndexedRelease> mods{};
        std::vector<IndexedRelease> failed_mods{};
        bool any_ready = false;
        bool any_pending = false;
        bool any_failed = false;
    };

    OfferBook offers{};
    /// Installed content, read again only when its stamp changes.
    bool have_ready = false;
    std::string have_stamp{};
    replay_data::HaveContent have_cached{};
    /// What the card says while `pending_get` is in flight, including 0%.
    std::string fetch_sentence{};
};

/// Stops the index pass's process and reaps it. A pass that is stopped
/// writes nothing into the index. Safe when none is running.
///
/// @param shelf the shelf
void stop_replay_index_pass(ReplayShelf& shelf) noexcept {
#if !defined(__ANDROID__) && OA_PROCESS_SPAWNING
    if (shelf.index_process == nullptr)
        return;
    const int pid = shelf.index_pid;
    SDL_KillProcess(shelf.index_process, true);
    int exit_code = 0;
    std::ignore = SDL_WaitProcess(shelf.index_process, true, &exit_code);
    SDL_DestroyProcess(shelf.index_process);
    shelf.index_process = nullptr;
    shelf.index_pid = 0;
    shelf.index_target.clear();
    std::error_code failed;
    if (!shelf.index_output.empty()) {
        fs::remove(shelf.index_output, failed);
        shelf.index_output.clear();
    }
    // A pass that was stopped writes nothing, and its output is not the game's.
    if (!shelf.index_errors.empty()) {
        fs::remove(shelf.index_errors, failed);
        shelf.index_errors.clear();
    }
    std::cerr << "open-annihilation: library: index pass stopped pid " << pid << '\n';
#else
    static_cast<void>(shelf);
#endif
}

void destroy_replay_shelf(ReplayShelf* shelf) noexcept {
    if (shelf == nullptr)
        return;
    bool join = false;
    {
        base::threads::LockGuard guard(shelf->mutex);
        shelf->stop = true;
        join = shelf->started;
    }
    shelf->cancel.store(true);
    shelf->wake.notify_all();
    if (join)
        base::threads::join_thread(shelf->thread);
    if (!shelf->import_release.empty()) {
        const PlatformHooks& hooks = platform_hooks();
        if (hooks.release_opened_file != nullptr)
            hooks.release_opened_file(
                hooks.context, platform::path_to_utf8(shelf->import_release).c_str()
            );
    }
    bool join_export = false;
    {
        base::threads::LockGuard guard(shelf->export_mutex);
        shelf->export_stop = true;
        shelf->export_cancel.store(true);
        shelf->export_pending = false;
        join_export = shelf->export_started;
    }
    shelf->export_wake.notify_all();
    if (join_export)
        base::threads::join_thread(shelf->export_thread);
    if (shelf->job.output.is_open())
        shelf->job.output.close();
    if (!shelf->job.partial.empty()) {
        std::error_code failed;
        fs::remove(shelf->job.partial, failed);
    }
    stop_replay_index_pass(*shelf);
#if !defined(__ANDROID__) && OA_PROCESS_SPAWNING
    if (!shelf->index_scratch.empty() && shelf->index_scratch.filename() == ".index-pass") {
        std::error_code failed;
        fs::remove_all(shelf->index_scratch, failed);
    }
#endif
    delete shelf;
}

/// The Library can start the index pass. A build that starts no other
/// program, and Android, leave every row on what its header says and show
/// no Reading… chip.
#if defined(__ANDROID__) || !OA_PROCESS_SPAWNING
constexpr bool replay_index_pass_available = false;
#else
constexpr bool replay_index_pass_available = true;
#endif

/// The installed folder of a mod the index pass plays one replay with.
///
/// It is the folder Watch plays: the release the replay names, in Mods or
/// Replay releases, when it is installed and is not the mod being played,
/// or the installed mod a `.tad`'s note or Recordings folder names. When
/// the mod the replay names is the one being played, it is the folder this
/// run layers. Otherwise the first Mods folder of the replay's mod.
///
/// @param runtime the runtime
/// @param shelf the section's shelf, whose cached install the rows were compared with
/// @param replay the replay
/// @return the folder, or none when that mod is not installed
std::optional<fs::path>
index_pass_mod_folder(Runtime& runtime, ReplayShelf& shelf, const replay_data::Replay& replay);

/// The kept map pack the index pass mounts for one replay.
///
/// @param runtime the runtime
/// @param shelf the section's shelf
/// @param replay the replay
/// @return the pack folder, or none when Watch plays Maps
std::optional<fs::path>
index_pass_map_folder(Runtime& runtime, ReplayShelf& shelf, const replay_data::Replay& replay);

/// The engine's build and what is installed, so a refused read can be tried
/// again once either changes.
///
/// @param runtime the runtime
/// @return the stamp
std::string index_pass_content_stamp(Runtime& runtime);

namespace {

/// The words the log's lines start with.
constexpr std::string_view kLogPrefix = "open-annihilation: library: ";
/// Minutes in an hour.
constexpr int32_t kMinutesPerHour = 60;
/// The years struct tm counts from.
constexpr int32_t kTmYearBase = 1900;

/// The catalogue's sentence with `{version}` filled.
///
/// @param result what the import said
/// @return the sentence in the language shown
std::string refusal_sentence(const replay_data::ImportResult& result) {
    std::string text(oa::data::languages::interface_text(result.sentence));
    if (result.version.empty())
        return text;
    const std::string field = "{version}";
    const std::size_t at = text.find(field);
    if (at != std::string::npos)
        text.replace(at, field.size(), result.version);
    return text;
}

/// The notice's paragraph when a replay was copied.
///
/// @param title the map's name
/// @return the sentence in the language shown
std::string added_sentence(std::string_view title) {
    std::string text(oa::data::languages::interface_text(replay_data::added_to_replays));
    const std::string field = "{title}";
    const std::size_t at = text.find(field);
    if (at != std::string::npos)
        text.replace(at, field.size(), title);
    return text;
}

/// Tells the player what became of a replay opened from outside.
///
/// @param layer the OA layer
/// @param paragraph the sentence
void show_replay_notice(OaLayer& layer, std::string paragraph) {
    kit::Notice told;
    told.title = std::string(oa::data::languages::interface_text("REPLAYS"));
    told.paragraphs.push_back({std::move(paragraph), false});
    told.word = "replay-import";
    layer.push(
        std::make_unique<NoticeScreen>(
            layer, Screen::main_menu, std::move(told), NoticeScreen::Host{}
        )
    );
}

/// Writes a line of the Replays section's to standard error, which the log keeps.
///
/// @param line the line
void replays_log(std::string_view line) {
    std::cerr << kLogPrefix << line << '\n';
}

/// A replay's place on the player's clock.
struct LocalWhen {
    int32_t day{};    ///< the local date, as day_number counts
    int32_t minute{}; ///< minutes into that day
};

/// Converts an instant to the player's local date and time.
///
/// @param seconds seconds since 1970 (UTC)
/// @return the local date and time; 1970-01-01 at midnight when the clock cannot say
LocalWhen local_when(int64_t seconds) {
    const auto instant = static_cast<std::time_t>(seconds);
    std::tm parts{};
#if defined(_WIN32)
    if (localtime_s(&parts, &instant) != 0)
        return {};
#else
    if (localtime_r(&instant, &parts) == nullptr)
        return {};
#endif
    LocalWhen when;
    when.day = replay_data::day_number(
        parts.tm_year + kTmYearBase,
        static_cast<uint32_t>(parts.tm_mon + 1),
        static_cast<uint32_t>(parts.tm_mday)
    );
    when.minute = parts.tm_hour * kMinutesPerHour + parts.tm_min;
    return when;
}

/// The worker: one read of the folder at a time, telling each replay as it
/// is done, until the shelf goes. An import asked for goes before a read, so
/// opening a replay does not wait for the folder.
///
/// @param argument the shelf
void replay_worker(void* argument) {
    auto& shelf = *static_cast<ReplayShelf*>(argument);
    for (;;) {
        fs::path folder;
        bool load = false;
        replay_data::Index from;
        bool importing = false;
        fs::path import_folder;
        fs::path import_source;
        {
            base::threads::LockGuard guard(shelf.mutex);
            while (!shelf.stop && !shelf.due && !shelf.import_due)
                shelf.wake.wait(shelf.mutex);
            if (shelf.stop)
                return;
            if (shelf.import_due) {
                importing = true;
                shelf.import_due = false;
                shelf.import_running = true;
                import_folder = shelf.import_recordings;
                import_source = shelf.import_source;
                ++shelf.moves;
            } else {
                shelf.due = false;
                folder = shelf.recordings;
                load = shelf.load;
                shelf.load = false;
                from = std::move(shelf.from);
                shelf.from = {};
                shelf.scanning = true;
                shelf.partial.clear();
                shelf.done = 0;
                shelf.total = 0;
                ++shelf.moves;
            }
        }
        if (importing) {
            replay_data::ImportResult result;
            try {
                result = replay_data::import_replay(import_folder, import_source);
            } catch (const std::exception& error) {
                result.ok = false;
                result.error = error.what();
                result.sentence = std::string(replay_data::file_unreadable);
            }
            base::threads::LockGuard guard(shelf.mutex);
            shelf.import_running = false;
            shelf.import_done = std::move(result);
            ++shelf.moves;
            continue;
        }
        replay_data::Index found;
        try {
            if (load) {
                replay_data::IndexRead read = replay_data::read_index(folder);
                if (read.damaged) {
                    // The index is a cache, so the folder is read again; the
                    // file that did not read is kept beside it, not overwritten.
                    const std::optional<fs::path> aside =
                        replay_data::set_aside(replay_data::index_path(folder));
                    replays_log(
                        aside ? "the replays' index could not be read; it is kept as " +
                                    platform::path_to_utf8(aside->filename())
                              : std::string("the replays' index could not be read")
                    );
                }
                from = std::move(read.index);
            }
            replay_data::ScanHooks hooks;
            hooks.context = &shelf;
            hooks.replay = [](void* context,
                              const replay_data::Replay& replay,
                              std::size_t done,
                              std::size_t total) {
                auto& self = *static_cast<ReplayShelf*>(context);
                base::threads::LockGuard guard(self.mutex);
                self.partial.push_back(replay);
                self.done = done;
                self.total = total;
                ++self.moves;
            };
            hooks.stop = [](void* context) {
                auto& self = *static_cast<ReplayShelf*>(context);
                return self.cancel.load();
            };
            found = replay_data::scan(folder, from, hooks);
        } catch (const std::exception& error) {
            replays_log(std::string("the replays could not be read: ") + error.what());
        }
        base::threads::LockGuard guard(shelf.mutex);
        shelf.scanning = false;
        shelf.result = std::move(found);
        ++shelf.moves;
    }
}

/// Starts the shelf's worker. The caller holds the shelf's mutex.
///
/// @param shelf the shelf
/// @return false when the thread did not start
bool start_replay_worker(ReplayShelf& shelf) {
    if (shelf.started)
        return true;
    if (!base::threads::start_thread(shelf.thread, replay_worker, &shelf)) {
        replays_log("the replays' worker could not start");
        return false;
    }
    shelf.started = true;
    return true;
}

/// A file the platform brought in for the game to read.
struct BroughtFile {
    bool brought{true};  ///< take_opened_file agreed, or there is no such hook
    fs::path readable{}; ///< the path the game reads
    std::string why{};   ///< why the platform could not bring it in
};

/// Asks the platform for the path the game can read, when it brings opened
/// files into its own storage.
///
/// @param source the path the game was given
/// @return the path to read, and why when the platform refused
BroughtFile bring_opened_file(const fs::path& source) {
    BroughtFile brought;
    brought.readable = source;
    const PlatformHooks& hooks = platform_hooks();
    if (hooks.take_opened_file == nullptr)
        return brought;
    std::string copy;
    brought.brought = hooks.take_opened_file(
        hooks.context, platform::path_to_utf8(source).c_str(), &copy, &brought.why
    );
    if (brought.brought && !copy.empty())
        brought.readable = platform::path_from_utf8(copy);
    return brought;
}

/// Hands a platform copy back. A file read where it was opened is left alone.
///
/// @param source the path the game was given
/// @param readable the path bring_opened_file wrote
void release_brought_file(const fs::path& source, const fs::path& readable) {
    if (readable == source)
        return;
    const PlatformHooks& hooks = platform_hooks();
    if (hooks.release_opened_file != nullptr)
        hooks.release_opened_file(hooks.context, platform::path_to_utf8(readable).c_str());
}

} // namespace

void Runtime::apply_opened_replay(
    const replay_data::ImportResult& result, const fs::path& release
) {
    if (!release.empty()) {
        const PlatformHooks& hooks = platform_hooks();
        if (hooks.release_opened_file != nullptr)
            hooks.release_opened_file(hooks.context, platform::path_to_utf8(release).c_str());
    }
    if (!result.ok) {
        replays_log("a replay was not imported: " + result.error);
        show_replay_notice(oa_layer(), refusal_sentence(result));
        return;
    }
    library_host().replay_shelf().land_path = result.path;
    open_library(library::Tab::replays);
    if (result.copied)
        show_replay_notice(oa_layer(), added_sentence(result.title));
    replays_log(
        std::string(result.copied ? "imported " : "selected ") + result.path + " (" + result.title +
        ")"
    );
}

namespace {

/// Splits a replay's path below Recordings into a path of the file system,
/// refusing one that would leave the folder.
///
/// @param recordings the Recordings folder
/// @param path the replay's path, '/' between folders, UTF-8
/// @return the file; none for a path that leaves Recordings
std::optional<fs::path> replay_file(const fs::path& recordings, std::string_view path) {
    fs::path file = recordings;
    std::size_t at = 0;
    while (at <= path.size()) {
        const std::size_t slash = path.find('/', at);
        const std::size_t end = slash == std::string_view::npos ? path.size() : slash;
        const std::string_view part = path.substr(at, end - at);
        if (part.empty() || part == "." || part == ".." ||
            part.find('\\') != std::string_view::npos || part.find(':') != std::string_view::npos)
            return std::nullopt;
        file /= platform::path_from_utf8(std::string(part));
        if (slash == std::string_view::npos)
            break;
        at = slash + 1;
    }
    return file;
}

/// Reads a whole replay file, at most the size a replay may have.
///
/// @param file the file
/// @param[out] bytes its bytes
/// @return false when it cannot be read, or is larger
bool read_replay_bytes(const fs::path& file, std::vector<uint8_t>& bytes) {
    std::error_code error;
    const auto size = fs::file_size(file, error);
    if (error || size > replay_data::most_file_bytes)
        return false;
    std::ifstream input(file, std::ios::binary);
    if (!input)
        return false;
    bytes.resize(static_cast<std::size_t>(size));
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
    return input.gcount() == static_cast<std::streamsize>(size);
}

/// Copies a range of a game file into the preview cache's buffer.
///
/// @param context the shelf
/// @param path the file inside the game's data
/// @param offset where the range starts, in bytes
/// @param out where it is copied
/// @param size its bytes
/// @return false when fewer are there
bool replay_read_map(void* context, const char* path, uint32_t offset, void* out, uint32_t size) {
    auto* shelf = static_cast<ReplayShelf*>(context);
    if (shelf == nullptr || shelf->runtime == nullptr || path == nullptr || out == nullptr)
        return false;
    if (shelf->read_chunk == nullptr)
        return false;
    return shelf->read_chunk(
        *shelf->runtime, path, offset, std::span<uint8_t>(static_cast<uint8_t*>(out), size)
    );
}

/// Tells whether a mark file is missing while a damaged copy of it is kept.
///
/// Defined with the retention read. A missing file with no damaged copy is a
/// player who keeps nothing, and is not this.
///
/// @param file the mark file
/// @return true when the file is absent and `file.damaged` is there
bool marks_waiting(const fs::path& file);

/// Reads a whole file as text, or empty when it cannot be read.
///
/// @param file the file
/// @return the text
std::string read_text_file(const fs::path& file) {
    std::error_code failure;
    const auto size = fs::file_size(file, failure);
    if (failure || size > 64ULL * 1024ULL * 1024ULL)
        return {};
    std::ifstream input(file, std::ios::binary);
    if (!input)
        return {};
    std::string text(static_cast<std::size_t>(size), '\0');
    input.read(text.data(), static_cast<std::streamsize>(text.size()));
    if (input.gcount() != static_cast<std::streamsize>(text.size()))
        return {};
    return text;
}

/// The bytes of every set-aside kept.json beside the live file.
///
/// @param folder the Recordings folder
/// @return each file's bytes; none when the folder has no such file
std::vector<std::string> damaged_kept_texts(const fs::path& folder) {
    std::vector<std::string> texts;
    std::error_code failure;
    const fs::directory_iterator end;
    fs::directory_iterator cursor(replay_data::kept_path(folder).parent_path(), failure);
    if (failure)
        return texts;
    const std::string prefix =
        std::string(replay_data::kept_name) + std::string(replay_data::damaged_ending);
    for (; cursor != end; cursor.increment(failure)) {
        if (failure)
            break;
        std::error_code kind;
        if (!cursor->is_regular_file(kind) || kind)
            continue;
        const std::string name = platform::path_to_utf8(cursor->path().filename());
        if (!name.starts_with(prefix))
            continue;
        std::string text = read_text_file(cursor->path());
        if (!text.empty())
            texts.push_back(std::move(text));
    }
    return texts;
}

/// Joins the whole mark objects in each text.
///
/// @param texts the texts
/// @return the marks
replay_data::KeptMarks marks_from_texts(const std::vector<std::string>& texts) {
    replay_data::KeptMarks merged;
    for (const std::string& text : texts) {
        const replay_data::KeptMarks part = replay_data::salvage_kept(text);
        for (const replay_data::KeptMark& mark : part.marks) {
            replay_data::Replay replay;
            replay.sha256 = mark.sha256;
            replay.path = mark.path;
            replay_data::set_kept(merged, replay, true);
        }
    }
    return merged;
}

/// What setting a damaged kept.json aside still held.
struct KeptSalvage {
    bool saw{false};                ///< the file was damaged or already set aside
    bool blocked{false};            ///< it could not be set aside, so nothing is written
    replay_data::KeptMarks marks{}; ///< every whole mark the bytes still held
};

/// Sets a damaged kept.json aside and keeps every whole mark in its text.
///
/// A file already set aside is read from those copies. Nothing is written
/// here: the caller saves once. A set-aside that fails writes nothing.
///
/// @param folder the Recordings folder
/// @param damaged `kept.json` is there and did not read
/// @param waiting `kept.json` is missing and a set-aside copy is there
/// @return the marks that were still whole
KeptSalvage salvage_kept_file(const fs::path& folder, bool damaged, bool waiting) {
    KeptSalvage out;
    if (!damaged && !waiting)
        return out;
    out.saw = true;
    if (damaged) {
        const fs::path file = replay_data::kept_path(folder);
        const std::string text = read_text_file(file);
        const std::optional<fs::path> aside = replay_data::set_aside(file);
        replays_log(
            aside ? "the kept replays' file could not be read; it is kept as " +
                        platform::path_to_utf8(aside->filename())
                  : std::string("the kept replays' file could not be read")
        );
        out.marks = replay_data::salvage_kept(text);
        out.blocked = !aside;
        return out;
    }
    out.marks = marks_from_texts(damaged_kept_texts(folder));
    replays_log("the kept replays' file was set aside; its marks were read from what was kept");
    return out;
}

/// Reads the replays the player keeps, once, the first time the folder is read.
///
/// A `kept.json` that does not read is set aside. Every whole mark still in
/// its text is written to a new file, so those replays still show Kept. A
/// set-aside that fails writes nothing.
///
/// @param shelf the shelf
/// @param folder the Recordings folder
void load_kept_marks(ReplayShelf& shelf, const fs::path& folder) {
    if (shelf.kept_loaded)
        return;
    shelf.kept_loaded = true;
    replay_data::KeptRead read = replay_data::read_kept(folder);
    const bool waiting = !read.damaged && marks_waiting(replay_data::kept_path(folder));
    if (!read.damaged && !waiting) {
        shelf.kept = std::move(read.kept);
        shelf.kept_damaged = false;
        return;
    }
    KeptSalvage salvaged = salvage_kept_file(folder, read.damaged, waiting);
    shelf.kept = std::move(salvaged.marks);
    shelf.kept_damaged = salvaged.blocked;
    shelf.kept_set_aside = !salvaged.blocked;
    if (salvaged.blocked || shelf.kept.marks.empty())
        return;
    if (!replay_data::save_kept(folder, shelf.kept))
        replays_log("the kept replays could not be saved");
}

/// Reads the exported marks once, and sets a damaged file aside.
///
/// @param shelf the shelf
/// @param folder the Recordings folder
void load_exported_marks(ReplayShelf& shelf, const fs::path& folder) {
    if (shelf.exported_loaded)
        return;
    shelf.exported_loaded = true;
    replay_data::ExportedRead read = replay_data::read_exported(folder);
    shelf.exported_damaged = read.damaged;
    if (read.damaged) {
        const std::optional<fs::path> aside =
            replay_data::set_aside(replay_data::exported_path(folder));
        replays_log(
            aside ? "the exported replays' file could not be read; it is kept as " +
                        platform::path_to_utf8(aside->filename())
                  : std::string("the exported replays' file could not be read")
        );
    }
    shelf.exported = std::move(read.exported);
}

/// Finds a replay among the rows the section shows: the whole index once a
/// read has ended, the rows read so far before.
///
/// @param shelf the shelf
/// @param path the replay's path below Recordings
/// @return the replay; null when no row has the path
const replay_data::Replay* shown_replay(const ReplayShelf& shelf, std::string_view path) {
    const std::vector<replay_data::Replay>& rows =
        shelf.listed ? shelf.index.replays : shelf.rows_read;
    for (const replay_data::Replay& replay : rows)
        if (replay.path == path)
            return &replay;
    return nullptr;
}

/// Marks a replay an export has just written, so the keep rule leaves it.
///
/// The mark is the recording and its path, in `exported.json`, written when
/// the exported file is in place. A file of marks that could not be read was
/// set aside when it was loaded. A later start has no `exported.json`, only
/// that set-aside copy. Nothing is written over either, and this export is
/// not marked.
///
/// @param[in,out] shelf the shelf; its exported marks gain the replay
/// @param folder the Recordings folder, which holds the marks
/// @param relative the replay's path below Recordings
/// @return true when the marks changed
bool mark_exported_replay(ReplayShelf& shelf, const fs::path& folder, std::string_view relative) {
    if (folder.empty() || relative.empty())
        return false;
    load_exported_marks(shelf, folder);
    if (shelf.exported_damaged)
        return false;
    // The set-aside copy is the only record of the marks. A new file would
    // end the hold, and the next consideration would move old replays.
    if (marks_waiting(replay_data::exported_path(folder)))
        return false;
    replay_data::Replay marked;
    if (const replay_data::Replay* shown = shown_replay(shelf, relative))
        marked = *shown;
    else
        marked.path = std::string(relative);
    if (marked.path.empty() && marked.sha256.empty())
        return false;
    if (!replay_data::set_exported(shelf.exported, marked, true))
        return false;
    if (!replay_data::save_exported(folder, shelf.exported))
        replays_log("the exported replays could not be saved");
    replays_log("exported " + marked.path);
    return true;
}

/// Reads one preference's text. The map may reallocate, so the text is copied.
///
/// @param values the preferences
/// @param key the preference
/// @return its text; empty when it is absent
std::string
preference_text(const std::map<std::string, std::string>& values, std::string_view key) {
    const auto found = values.find(std::string(key));
    if (found == values.end())
        return {};
    return found->second;
}

/// Drops one replay from an index's rows.
///
/// @param[in,out] rows the rows
/// @param relative the replay's path below Recordings
void drop_replay(std::vector<replay_data::Replay>& rows, std::string_view relative) {
    std::erase_if(rows, [&](const replay_data::Replay& replay) { return replay.path == relative; });
}

/// The index and the marks of one sweep, loaded once and saved once.
struct StoredBatch {
    std::vector<replay_data::Replay> listed{}; ///< the rows a mark is released against
    replay_data::Index index{};                ///< the index file's rows
    bool save_index{false};                    ///< `index` is the file, or the shelf's list
    bool index_dirty{false};                   ///< a row was dropped
    replay_data::KeptMarks kept{};
    bool kept_open{false};    ///< `kept` may be saved
    bool kept_blocked{false}; ///< a damaged file could not be set aside
    bool kept_dirty{false};
    replay_data::KeptMarks exported{};
    bool exported_open{false};
    bool exported_blocked{false};
    bool exported_dirty{false};
};

/// Loads the index and the marks once.
///
/// A damaged kept.json is set aside and its whole marks are kept. Nothing
/// is saved here.
///
/// @param shelf the shelf, when the Library has one; null otherwise
/// @param folder the Recordings folder
/// @return the batch
StoredBatch open_batch(ReplayShelf* shelf, const fs::path& folder) {
    StoredBatch batch;
    if (shelf != nullptr && shelf->listed) {
        batch.listed = shelf->index.replays;
        batch.index = shelf->index;
        batch.save_index = true;
    } else {
        batch.index = replay_data::load_index(folder);
        batch.save_index = true;
        batch.listed =
            shelf != nullptr && !shelf->rows_read.empty() ? shelf->rows_read : batch.index.replays;
    }
    if (shelf != nullptr && shelf->kept_loaded && !shelf->kept_damaged) {
        batch.kept = shelf->kept;
        batch.kept_open = true;
    } else {
        const replay_data::KeptRead read = replay_data::read_kept(folder);
        if (read.damaged || marks_waiting(replay_data::kept_path(folder))) {
            const KeptSalvage salvaged = salvage_kept_file(folder, read.damaged, !read.damaged);
            batch.kept = salvaged.marks;
            batch.kept_blocked = salvaged.blocked;
            batch.kept_open = !salvaged.blocked;
            batch.kept_dirty = !salvaged.blocked && !batch.kept.marks.empty();
        } else {
            batch.kept = read.kept;
            batch.kept_open = true;
        }
    }
    if (shelf != nullptr && shelf->exported_loaded && !shelf->exported_damaged) {
        batch.exported = shelf->exported;
        batch.exported_open = true;
    } else {
        const replay_data::ExportedRead read = replay_data::read_exported(folder);
        if (!read.damaged) {
            batch.exported = read.exported;
            batch.exported_open = true;
        } else {
            const std::optional<fs::path> aside =
                replay_data::set_aside(replay_data::exported_path(folder));
            replays_log(
                aside ? "the exported replays' file could not be read; it is kept as " +
                            platform::path_to_utf8(aside->filename())
                      : std::string("the exported replays' file could not be read")
            );
            if (!aside) {
                batch.exported_blocked = true;
            } else {
                batch.exported_open = true;
                batch.exported_dirty = true;
            }
        }
    }
    return batch;
}

/// The replay a path names, from the shelf or the batch, or the path alone.
///
/// @param shelf the shelf, when the Library has one; null otherwise
/// @param batch the index and the marks loaded for this sweep
/// @param relative the replay's path below Recordings
/// @return the replay; its path set and its identity empty when no row has it
replay_data::Replay
replay_for(const ReplayShelf* shelf, const StoredBatch& batch, std::string_view relative) {
    if (shelf != nullptr)
        if (const replay_data::Replay* shown = shown_replay(*shelf, relative))
            return *shown;
    for (const replay_data::Replay& row : batch.listed)
        if (row.path == relative)
            return row;
    for (const replay_data::Replay& row : batch.index.replays)
        if (row.path == relative)
            return row;
    replay_data::Replay bare;
    bare.path = std::string(relative);
    return bare;
}

/// Drops a replay the trash took from the batch's index and its marks.
///
/// The marks are not saved here. Another listed file that still has the
/// recording keeps the mark.
///
/// @param[in,out] batch the index and the marks
/// @param shelf the shelf, when the Library has one; null otherwise
/// @param replay the replay that went
void note_moved(StoredBatch& batch, ReplayShelf* shelf, const replay_data::Replay& replay) {
    if (batch.kept_open && !batch.kept_blocked &&
        replay_data::release_kept(batch.kept, batch.listed, replay))
        batch.kept_dirty = true;
    if (batch.exported_open && !batch.exported_blocked &&
        replay_data::release_kept(batch.exported, batch.listed, replay))
        batch.exported_dirty = true;
    const std::size_t before = batch.index.replays.size();
    drop_replay(batch.index.replays, replay.path);
    if (batch.index.replays.size() != before)
        batch.index_dirty = true;
    drop_replay(batch.listed, replay.path);
    if (shelf == nullptr)
        return;
    drop_replay(shelf->rows_read, replay.path);
    shelf->gone.emplace_back(replay.path);
}

/// Writes the batch's index and its marks, once, and copies them onto the shelf.
///
/// A damaged file that could not be set aside is not written over. While a
/// set-aside `exported.json` is the only copy, nothing is written in its
/// place: a new file would end the hold. An empty index that was never
/// stored is not created.
///
/// @param batch the index and the marks
/// @param shelf the shelf, when the Library has one; null otherwise
/// @param folder the Recordings folder
void commit_batch(StoredBatch& batch, ReplayShelf* shelf, const fs::path& folder) {
    if (batch.save_index && batch.index_dirty) {
        std::error_code missing;
        if ((!batch.index.replays.empty() ||
             fs::exists(replay_data::index_path(folder), missing)) &&
            !replay_data::save_index(folder, batch.index))
            replays_log("the replays' index could not be saved");
    }
    if (batch.kept_open && !batch.kept_blocked && batch.kept_dirty &&
        !replay_data::save_kept(folder, batch.kept))
        replays_log("the kept replays could not be saved");
    if (batch.exported_open && !batch.exported_blocked && batch.exported_dirty &&
        !marks_waiting(replay_data::exported_path(folder)) &&
        !replay_data::save_exported(folder, batch.exported))
        replays_log("the exported replays could not be saved");
    if (shelf == nullptr)
        return;
    if (shelf->listed && batch.index_dirty)
        shelf->index = batch.index;
    if (batch.kept_open && !batch.kept_blocked) {
        shelf->kept = batch.kept;
        shelf->kept_loaded = true;
        shelf->kept_damaged = false;
    }
    if (batch.exported_open && !batch.exported_blocked) {
        shelf->exported = batch.exported;
        shelf->exported_loaded = true;
        shelf->exported_damaged = false;
    }
}

/// What moving one replay to the trash did.
struct TrashOutcome {
    bool gone{false};    ///< the file is no longer in Recordings
    std::string error{}; ///< why not, when it stayed
};

/// Tells whether a file, or a folder between it and Recordings, is a symlink.
///
/// Recordings itself may be a symlink. Anything under it that is a symlink
/// is left where it is, so a folder turned into a link after the read cannot
/// lead the move outside Recordings.
///
/// @param recordings the Recordings folder
/// @param file the file replay_file built under it
/// @return true when the move must be refused
bool reached_through_link(const fs::path& recordings, const fs::path& file) {
    const fs::path relative = file.lexically_relative(recordings);
    fs::path walk = recordings;
    for (const fs::path& part : relative) {
        if (part.empty() || part == "." || part == "..")
            return true;
        walk /= part;
        std::error_code failure;
        const fs::file_status status = fs::symlink_status(walk, failure);
        if (!failure && fs::is_symlink(status))
            return true;
        if (failure || !fs::exists(status))
            return false;
    }
    return false;
}

/// Tells whether a reason is one this program wrote, rather than the system.
///
/// @param text the reason
/// @return true when the card must not say it
bool engine_reason(std::string_view text) {
    static constexpr std::string_view canned[] = {
        "the replay's path leaves Recordings",
        "it is reached through a link",
        "the trash did not take the file",
        "the trash could not take the file",
        "the trash could not be made",
        "the trash could not record the file",
        "the trash already holds this name",
        "this process has no home, so it has nowhere to keep the trash",
        "the file's path cannot be written",
        "the file is not there",
        "the file could not be deleted",
        "the Recycle Bin cannot take a file on this drive",
    };
    for (const std::string_view one : canned)
        if (text == one)
            return true;
    constexpr std::string_view coded = "the trash did not take the file (";
    return text.size() >= coded.size() && text.substr(0, coded.size()) == coded;
}

/// The reason the card may say: one the system gave, or none.
///
/// @param text the reason, as the move reported it
/// @return the reason; empty when it is this program's own, or there is none
std::string card_reason(std::string_view text) {
    if (text.empty() || engine_reason(text))
        return {};
    return std::string(text);
}

/// Moves one replay's file to the trash. Does not read it, or touch the index.
///
/// A file already gone counts as gone. A trash that refuses leaves the file.
/// The system is never asked to delete outright where it has a trash. A
/// symlink under Recordings is left where it is. The card is given only a
/// reason the system itself gave; this program's own reasons go to the log.
///
/// @param[in,out] trash_left where a trash copy went, when the system said
/// @param folder the Recordings folder
/// @param relative the replay's path below Recordings
/// @param refusal when non-empty, the move is not tried and this is the reason
/// @return whether the file is gone, and the reason the card may say
TrashOutcome try_move(
    std::vector<fs::path>& trash_left,
    const fs::path& folder,
    std::string_view relative,
    std::string_view refusal
) {
    const std::optional<fs::path> file = replay_file(folder, relative);
    if (!file) {
        replays_log("cannot delete " + std::string(relative));
        return {false, card_reason("the replay's path leaves Recordings")};
    }
    if (reached_through_link(folder, *file)) {
        replays_log("will not move " + std::string(relative) + ": it is reached through a link");
        return {false, card_reason("it is reached through a link")};
    }
    std::error_code failure;
    if (!fs::is_regular_file(*file, failure))
        return {true, {}};
    if (!refusal.empty()) {
        replays_log(
            "could not move " + std::string(relative) + " to the trash: " + std::string(refusal)
        );
        return {false, card_reason(refusal)};
    }
    const platform::TrashMove moved = platform::move_to_trash(*file);
    if (!moved.moved) {
        const std::string reason =
            moved.error.empty() ? std::string("the trash did not take the file") : moved.error;
        replays_log("could not move " + std::string(relative) + " to the trash: " + reason);
        return {false, card_reason(reason)};
    }
    if (!moved.kept_at.empty())
        trash_left.push_back(moved.kept_at);
    if (!moved.note_at.empty())
        trash_left.push_back(moved.note_at);
    replays_log(
        std::string(moved.place == platform::TrashPlace::deleted ? "deleted " : "trashed ") +
        std::string(relative)
    );
    return {true, {}};
}

/// Replaces `{count}` in a sentence.
///
/// @param text the sentence
/// @param count the number
/// @return the sentence
std::string with_count(std::string text, std::size_t count) {
    constexpr std::string_view token = "{count}";
    const std::size_t at = text.find(token);
    if (at != std::string::npos)
        text.replace(at, token.size(), std::to_string(count));
    return text;
}

/// Looks up an interface sentence and copies it. The next lookup may reuse the view.
///
/// @param english the English sentence
/// @return the sentence in the player's language
std::string interface_sentence(std::string_view english) {
    return std::string(oa::data::languages::interface_text(english));
}

/// The card's sentence when a delete left files behind.
///
/// This program's own reasons are left out: the log has them. A reason the
/// system gave is added after the sentence.
///
/// @param stayed how many files stayed
/// @param reason a reason the system gave; empty when there is none
/// @return the sentence; empty when nothing stayed
std::string trash_failure_note(std::size_t stayed, std::string_view reason) {
    if (stayed == 0)
        return {};
    std::string sentence =
        stayed == 1
            ? interface_sentence(library::words::replay_trash_refused)
            : with_count(interface_sentence(library::words::replay_trash_refused_many), stayed);
    if (!reason.empty())
        sentence += " " + std::string(reason);
    return sentence;
}

/// Takes a refusal's file name off its front: the card already names the file.
///
/// @param refusal what the extension said
/// @param name the recording's file name, as the extension was given it
/// @return the sentence alone; the refusal as it was when it does not begin with the name
std::string without_file_name(std::string refusal, std::string_view name) {
    const std::string prefix = std::string(name) + ": ";
    if (!name.empty() && refusal.size() > prefix.size() &&
        refusal.compare(0, prefix.size(), prefix) == 0)
        refusal.erase(0, prefix.size());
    return refusal;
}

/// The key a map's picture is kept under in the cache.
///
/// @param map the map's name
/// @return the key
std::string picture_key(std::string_view map) {
    return "replay-map:" + std::string(map);
}

/// The SHA-256 as 64 lower-case hex digits.
std::string digest_hex(const oa::base::sha256::Digest& digest) {
    const std::array<char, oa::base::sha256::hex_size> hex = oa::base::sha256::to_hex(digest);
    return std::string(hex.begin(), hex.end());
}

/// The catalogue kind a gap names, or nothing when the text is not one.
std::optional<data::catalogue::Kind> catalogue_kind(std::string_view text) {
    for (const data::catalogue::Kind kind :
         {data::catalogue::Kind::oalang,
          data::catalogue::Kind::oamod,
          data::catalogue::Kind::oamap,
          data::catalogue::Kind::expansion,
          data::catalogue::Kind::oamut,
          data::catalogue::Kind::oabrain,
          data::catalogue::Kind::oakeys}) {
        if (text == data::catalogue::kind_name(kind))
            return kind;
    }
    return std::nullopt;
}

/// The name a sentence uses for one gap.
std::string gap_title(const replay_data::ContentGap& gap) {
    if (gap.kind == replay_data::GapKind::map) {
        if (!gap.pack_name.empty())
            return gap.pack_name;
        if (!gap.map_name.empty())
            return gap.map_name;
    }
    if (!gap.name.empty())
        return gap.name;
    return gap.id;
}

/// A map named only by its title is not a package.
bool shown_gap(const replay_data::ContentGap& gap) {
    return !(gap.kind == replay_data::GapKind::map && gap.sha256.empty() && gap.id.empty());
}

/// Frames an export waits, after it is asked for, before the first byte is written.
constexpr int export_hold_frames = 2;
/// Letters a file name keeps of a replay's title.
constexpr std::size_t export_name_cap = 120;
/// Bytes of one export written on a single frame, and the piece a watched
/// compression stops on.
constexpr std::size_t export_chunk_bytes = 256U * 1024U;

/// The folder an export starts in.
struct DownloadsFolder {
    fs::path path{};
    /// The system named a Downloads folder and it is a directory.
    bool system{};
};

/// The system's Downloads folder, or the player's own folder when the system
/// names none or the named path is not a directory.
///
/// @param user_folder the player's own folder
/// @return the folder, and whether it is the system's
DownloadsFolder downloads_folder(const fs::path& user_folder) {
    DownloadsFolder folder;
    folder.path = user_folder;
    const char* named = SDL_GetUserFolder(SDL_FOLDER_DOWNLOADS);
    if (named == nullptr || named[0] == '\0')
        return folder;
    const fs::path candidate = platform::path_from_utf8(named);
    std::error_code failed;
    if (candidate.empty() || !fs::is_directory(candidate, failed) || failed)
        return folder;
    folder.path = candidate;
    folder.system = true;
    return folder;
}

/// Tells whether two folders are the same directory.
///
/// @param left one folder
/// @param right the other
/// @return true when both name a directory and it is the same one
bool same_folder(const fs::path& left, const fs::path& right) {
    std::error_code failed;
    const fs::path left_real = fs::weakly_canonical(left, failed);
    if (failed)
        return false;
    const fs::path right_real = fs::weakly_canonical(right, failed);
    if (failed)
        return false;
    return left_real == right_real;
}

/// Tells whether `text` ends with `suffix`, ignoring the case of ASCII letters.
///
/// @param text the text
/// @param suffix the ending
/// @return true when it does
bool ends_ci(std::string_view text, std::string_view suffix) {
    if (text.size() < suffix.size())
        return false;
    for (std::size_t at = 0; at < suffix.size(); ++at) {
        unsigned char left = static_cast<unsigned char>(text[text.size() - suffix.size() + at]);
        unsigned char right = static_cast<unsigned char>(suffix[at]);
        if (left >= 'A' && left <= 'Z')
            left = static_cast<unsigned char>(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z')
            right = static_cast<unsigned char>(right - 'A' + 'a');
        if (left != right)
            return false;
    }
    return true;
}

/// Drops a trailing `.oademo` or `.tad`, ignoring the case of ASCII letters.
///
/// @param name a file name or a title taken from one
/// @return the name without that ending
std::string strip_replay_extension(std::string name) {
    if (ends_ci(name, ".oademo"))
        name.resize(name.size() - std::string_view(".oademo").size());
    else if (ends_ci(name, ".tad"))
        name.resize(name.size() - std::string_view(".tad").size());
    return name;
}

/// Tells whether a file stem is reserved on Windows.
///
/// @param name the stem, after it has been capped
/// @return true for CON, PRN, AUX, NUL, COM1 to COM9 and LPT1 to LPT9
bool reserved_file_stem(std::string_view name) {
    std::string lower;
    lower.reserve(name.size());
    for (const unsigned char character : name) {
        const unsigned char folded = character >= 'A' && character <= 'Z'
                                         ? static_cast<unsigned char>(character - 'A' + 'a')
                                         : character;
        lower.push_back(static_cast<char>(folded));
    }
    if (lower == "con" || lower == "prn" || lower == "aux" || lower == "nul")
        return true;
    if (lower.size() == 4 && (lower.starts_with("com") || lower.starts_with("lpt"))) {
        const char digit = lower[3];
        return digit >= '1' && digit <= '9';
    }
    return false;
}

/// Makes a replay's title safe as a file name: the characters a folder
/// cannot hold become `_`, spaces and dots at either end go, a trailing
/// `.oademo` or `.tad` is dropped, and what is left is at most
/// `export_name_cap` bytes. A stem Windows reserves gains a trailing `_`.
/// An empty name is "replay".
///
/// @param title the Library's title
/// @return the name, without an extension
std::string export_file_stem(std::string_view title) {
    std::string name;
    name.reserve(title.size());
    for (const unsigned char character : title) {
        const bool bad = character < 0x20 || character == 0x7F || character == '/' ||
                         character == '\\' || character == ':' || character == '*' ||
                         character == '?' || character == '"' || character == '<' ||
                         character == '>' || character == '|';
        name.push_back(bad ? '_' : static_cast<char>(character));
    }
    std::size_t begin = 0;
    while (begin < name.size() && (name[begin] == ' ' || name[begin] == '.'))
        ++begin;
    std::size_t end = name.size();
    while (end > begin && (name[end - 1] == ' ' || name[end - 1] == '.'))
        --end;
    name = name.substr(begin, end - begin);
    name = strip_replay_extension(std::move(name));
    while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
    if (name.size() > export_name_cap)
        name.resize(export_name_cap);
    while (!name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.pop_back();
    if (name.empty())
        name = "replay";
    if (reserved_file_stem(name))
        name.push_back('_');
    return name;
}

/// A free name in a folder: the stem, then " (2)", " (3)" and so on.
///
/// @param folder the folder
/// @param stem the name, without an extension
/// @param extension the extension, with its dot
/// @return the file; empty when every name tried is taken
fs::path
export_destination(const fs::path& folder, const std::string& stem, const std::string& extension) {
    const auto free_name = [&](const std::string& candidate) {
        const fs::path file = folder / platform::path_from_utf8(candidate);
        std::error_code failed;
        return !fs::exists(file, failed) && !failed ? file : fs::path{};
    };
    if (const fs::path plain = free_name(stem + extension); !plain.empty())
        return plain;
    for (int copy = 2; copy < 10000; ++copy) {
        const fs::path file = free_name(stem + " (" + std::to_string(copy) + ")" + extension);
        if (!file.empty())
            return file;
    }
    return {};
}

/// The error from the call just made. A library that leaves errno at 0
/// still reports a failure, as an I/O error.
///
/// @return the error; never a default-constructed code
std::error_code capture_errno() {
    const int saved = errno;
    errno = 0;
    if (saved == 0)
        return std::make_error_code(std::errc::io_error);
    return {saved, std::generic_category()};
}

/// Removes a partial export. A missing file is not a failure.
///
/// @param partial the `.part` file
void remove_partial(const fs::path& partial) {
    if (partial.empty())
        return;
    std::error_code failed;
    fs::remove(partial, failed);
}

/// Closes the job's stream and removes its partial file. The bytes already
/// renamed into place stay.
///
/// @param[in,out] job the job
void close_export_output(ReplayShelf::ExportJob& job) {
    if (job.output.is_open())
        job.output.close();
    job.output.clear();
    remove_partial(job.partial);
    job.partial.clear();
    job.destination.clear();
    job.opened = false;
    job.offset = 0;
}

/// Drops a job's stream and its fields without assigning the job.
///
/// @param[in,out] job the job
void reset_export_job(ReplayShelf::ExportJob& job) {
    close_export_output(job);
    job.active = false;
    job.hold = 0;
    job.folder.clear();
    job.piece = 0;
    job.files_done = 0;
    job.files_total = 0;
    job.as_oademo = true;
    job.sectors = true;
    job.extension.clear();
}

/// Writes the next piece of an export, at most one chunk.
///
/// The first call opens `destination` beside a `.part`. The last call closes
/// that file and renames it into place. A failure removes the `.part`. An
/// empty code is a chunk that was written, whether or not the file is finished:
/// `offset` then says how far the bytes have gone, and it stays at the end
/// when the rename has happened.
///
/// @param[in,out] job the job; `destination` is set before the first call
/// @param bytes the file
/// @return the error; empty when this chunk was written
std::error_code write_export_chunk(ReplayShelf::ExportJob& job, const std::vector<uint8_t>& bytes) {
    if (job.destination.empty())
        return std::make_error_code(std::errc::io_error);
    if (!job.opened) {
        const std::string part_name = platform::path_to_utf8(job.destination.filename()) + ".part";
        job.partial = job.destination.parent_path() / platform::path_from_utf8(part_name);
        job.offset = 0;
        job.output.clear();
        errno = 0;
        job.output.open(job.partial, std::ios::binary | std::ios::trunc);
        if (!job.output) {
            const std::error_code failed = capture_errno();
            job.output.clear();
            remove_partial(job.partial);
            job.partial.clear();
            return failed;
        }
        job.opened = true;
    }
    if (job.offset > bytes.size())
        return std::make_error_code(std::errc::io_error);
    const std::size_t left = bytes.size() - job.offset;
    const std::size_t take = std::min(left, export_chunk_bytes);
    if (take > 0) {
        errno = 0;
        job.output.write(
            reinterpret_cast<const char*>(bytes.data() + job.offset),
            static_cast<std::streamsize>(take)
        );
        if (!job.output) {
            const std::error_code failed = capture_errno();
            job.output.close();
            job.output.clear();
            remove_partial(job.partial);
            job.partial.clear();
            job.opened = false;
            job.offset = 0;
            return failed;
        }
        job.offset += take;
    }
    if (job.offset < bytes.size())
        return {};
    errno = 0;
    job.output.close();
    if (job.output.fail()) {
        const std::error_code failed = capture_errno();
        job.output.clear();
        remove_partial(job.partial);
        job.partial.clear();
        job.opened = false;
        job.offset = 0;
        return failed;
    }
    std::error_code renamed;
    fs::rename(job.partial, job.destination, renamed);
    if (renamed) {
        remove_partial(job.partial);
        job.partial.clear();
        job.opened = false;
        job.offset = 0;
        job.destination.clear();
        return renamed;
    }
    job.partial.clear();
    job.opened = false;
    job.destination.clear();
    return {};
}

/// The folder as the dialog shows it.
///
/// @param model the Library
/// @return Downloads, the label, or the path when neither is set
std::string export_problem_folder(const library::Library& model) {
    const library::ExportDialog& dialog = model.export_dialog;
    if (dialog.folder_is_downloads)
        return std::string(library::shown(model.text, library::words::export_downloads));
    if (!dialog.folder_label.empty())
        return dialog.folder_label;
    return dialog.folder;
}

/// The sentence a failed write shows. A full disk and a folder that cannot
/// be written name themselves; anything else keeps the general sentence.
/// Some files already written are counted in front of it.
///
/// @param model the Library
/// @param code why the write failed
/// @param done files already renamed into place
/// @param total files the export was asked to write
/// @return the sentence, already looked up
std::string export_problem_sentence(
    const library::Library& model, const std::error_code& code, uint32_t done, uint32_t total
) {
    const std::string folder = export_problem_folder(model);
    std::string reason;
    if (code == std::errc::no_space_on_device)
        reason = library::filled(model.text, library::words::export_no_room, {{"folder", folder}});
    else if (
        code == std::errc::permission_denied || code == std::errc::read_only_file_system ||
        code == std::errc::operation_not_permitted
    )
        reason =
            library::filled(model.text, library::words::export_cannot_write, {{"folder", folder}});
    else
        reason = std::string(library::shown(model.text, library::words::export_failed));
    if (done > 0 && total > 0 && done < total) {
        const std::string saved = library::filled(
            model.text,
            library::words::export_saved_some,
            {{"done", std::to_string(done)}, {"total", std::to_string(total)}}
        );
        return saved + " " + reason;
    }
    return reason;
}

/// The sentence the player sees when an export cannot be written.
///
/// @param model the Library
/// @return the sentence, already looked up
std::string export_failed_sentence(const library::Library& model) {
    return std::string(library::shown(model.text, library::words::export_failed));
}

/// The name the dialog shows for a folder. Downloads is the catalogue's word,
/// which the screen looks up from the flag, so this is empty then.
///
/// @param folder the folder
/// @param downloads the folder is the system's Downloads folder
/// @return the folder's own name, or its path when it has none
std::string export_folder_label(const fs::path& folder, bool downloads) {
    if (downloads)
        return {};
    const std::string name = platform::path_to_utf8(folder.filename());
    if (!name.empty())
        return name;
    return platform::path_to_utf8(folder);
}

/// Tells whether a path is a directory.
///
/// @param folder the path
/// @return true when it is a directory
bool is_folder(const fs::path& folder) {
    std::error_code failed;
    return !folder.empty() && fs::is_directory(folder, failed) && !failed;
}

/// Tells whether a chooser row's name must not be joined onto the folder.
///
/// @param name the row's name
/// @return true for an empty name, `.`, `..` or a name that holds a slash
bool export_name_refused(std::string_view name) {
    return name.empty() || name == "." || name == ".." ||
           name.find('/') != std::string_view::npos || name.find('\\') != std::string_view::npos;
}

/// Tells whether a locator place is not one Export offers.
///
/// Home, Desktop, Documents, Downloads and the volumes stay. Applications,
/// Program Files and the deck's game folders do not.
///
/// @param place the place
/// @return true when the chooser leaves it out
bool export_place_skipped(const installs::LocatorPlace& place) {
    if (place.group == installs::LocatorGroup::deck_games)
        return true;
    if (place.group != installs::LocatorGroup::places &&
        place.group != installs::LocatorGroup::volumes)
        return true;
    return place.label == "Applications" || place.label == "Program Files";
}

/// Lists the places the locator offers, then every subfolder, for the chooser.
///
/// The places come first, in the locator's order, each with its own path.
/// Subfolders are sorted and are not capped: a long list pages.
///
/// @param[in,out] model the Library
/// @param[in,out] revision the model's revision, counted on when the list shows
/// @param folder the folder listed
void list_export_folder(library::Library& model, uint64_t& revision, const fs::path& folder) {
    std::vector<library::ExportFolder> folders;
    const std::vector<installs::LocatorPlace> places =
        installs::locator_places(installs::locator_place_facts(false));
    for (const installs::LocatorPlace& place : places) {
        if (export_place_skipped(place) || !is_folder(place.folder))
            continue;
        library::ExportFolder one;
        one.name = place.label;
        one.path = platform::path_to_utf8(place.folder);
        folders.push_back(std::move(one));
    }
    std::vector<chooser::Entry> rows;
    std::error_code failed;
    try {
        for (const fs::directory_entry& entry : fs::directory_iterator(folder, failed)) {
            if (failed)
                break;
            std::error_code skipped;
            if (!entry.is_directory(skipped) || skipped)
                continue;
            const std::string name = platform::path_to_utf8(entry.path().filename());
            if (name.empty() || name.front() == '.')
                continue;
            if (!chooser::name_matches(name, {}))
                continue;
            chooser::Entry row;
            row.name = name;
            row.directory = true;
            row.what = chooser::describe_source(name, 0, true, {}, 0).what;
            rows.push_back(std::move(row));
        }
    } catch (const fs::filesystem_error&) {
        rows.clear();
    }
    chooser::order_entries(rows);
    folders.reserve(folders.size() + rows.size());
    for (chooser::Entry& row : rows) {
        library::ExportFolder one;
        one.name = std::move(row.name);
        one.what = std::move(row.what);
        folders.push_back(std::move(one));
    }
    library::open_export_browse(model, platform::path_to_utf8(folder), std::move(folders));
    ++revision;
}

/// Passed to a watched compression so a cancel stops it at the next piece.
struct ExportWatch {
    ReplayShelf* shelf{};
    uint64_t generation{};
};

/// Stores how far a compression has come, unless this measurement was cancelled.
///
/// @param context an `ExportWatch`
/// @param taken recording bytes compressed
/// @param whole recording bytes in all
void export_watch_report(void* context, uint64_t taken, uint64_t whole) {
    const auto* watch = static_cast<const ExportWatch*>(context);
    ReplayShelf& shelf = *watch->shelf;
    if (shelf.export_cancel.load() || shelf.export_generation.load() != watch->generation)
        return;
    shelf.export_byte_done.store(taken);
    shelf.export_byte_total.store(whole);
    shelf.export_compressing.store(true);
    shelf.export_progress_stamp.fetch_add(1);
}

/// Tells a compression to stop once the measurement was cancelled.
///
/// @param context an `ExportWatch`
/// @return true when the frame should not be finished
bool export_watch_stop(void* context) {
    const auto* watch = static_cast<const ExportWatch*>(context);
    return watch->shelf->export_cancel.load() ||
           watch->shelf->export_generation.load() != watch->generation;
}

/// Shows a measurement's progress. A cancelled measurement leaves the new one alone.
///
/// @param shelf the shelf
/// @param generation the measurement this progress belongs to
/// @param done bytes taken
/// @param total bytes in the recording
void export_note_bytes(ReplayShelf& shelf, uint64_t generation, uint64_t done, uint64_t total) {
    if (shelf.export_cancel.load() || shelf.export_generation.load() != generation)
        return;
    shelf.export_byte_done.store(done);
    shelf.export_byte_total.store(total);
    shelf.export_progress_stamp.fetch_add(1);
}

/// Tells whether this measurement should stop.
///
/// @param shelf the shelf
/// @param generation the measurement
/// @return true when a newer one has replaced it, or the player cancelled
bool export_stopped(const ReplayShelf& shelf, uint64_t generation) {
    return shelf.export_cancel.load() || shelf.export_generation.load() != generation;
}

/// Records both `.tad` sizes and the warnings, and keeps none of those bytes.
///
/// @param recording the recording
/// @param[in,out] held the sizes and the warnings
/// @param[out] why the format's sentence, when it cannot
/// @return false when either export cannot be made
bool measure_tad_sizes(
    std::span<const uint8_t> recording, ReplayShelf::HeldExport& held, std::string& why
) {
    tad::ExportOptions keep_options;
    keep_options.keep_oa_sectors = true;
    keep_options.fit_tadr_sizes = true;
    tad::ExportResult kept = tad::export_recording(recording, keep_options);
    if (!kept.ok() || kept.bytes.empty()) {
        why = kept.error ? kept.error->message : "the export was empty";
        return false;
    }
    held.tad_bytes = kept.bytes.size();
    for (const tad::ExportNotice& notice : kept.notices) {
        switch (notice.kind) {
        case tad::ExportNoticeKind::names_cut:
            held.names_cut = true;
            break;
        case tad::ExportNoticeKind::status_cut:
            held.status_cut = true;
            break;
        case tad::ExportNoticeKind::wide_records:
            held.wide_records = true;
            break;
        case tad::ExportNoticeKind::mutators:
            held.mutators = true;
            held.mutator_ids = notice.mutators;
            break;
        }
    }
    kept.bytes.clear();
    kept.bytes.shrink_to_fit();
    tad::ExportOptions drop_options = keep_options;
    drop_options.keep_oa_sectors = false;
    tad::ExportResult dropped = tad::export_recording(recording, drop_options);
    if (!dropped.ok() || dropped.bytes.empty()) {
        why = dropped.error ? dropped.error->message : "the export was empty";
        return false;
    }
    held.tad_without_bytes = dropped.bytes.size();
    dropped.bytes.clear();
    dropped.bytes.shrink_to_fit();
    return true;
}

/// Keeps one `.tad` export. The other format's bytes are not kept beside it.
///
/// @param shelf the shelf, for the sectors the dialog is showing
/// @param recording the recording
/// @param[in,out] held where the bytes go
/// @param[out] why the format's sentence, when it cannot
/// @return false when the export cannot be made
bool keep_tad_bytes(
    const ReplayShelf& shelf,
    std::span<const uint8_t> recording,
    ReplayShelf::HeldExport& held,
    std::string& why
) {
    const bool sectors = shelf.export_want_sectors.load();
    tad::ExportOptions options;
    options.keep_oa_sectors = sectors;
    options.fit_tadr_sizes = true;
    tad::ExportResult exported = tad::export_recording(recording, options);
    if (!exported.ok() || exported.bytes.empty()) {
        why = exported.error ? exported.error->message : "the export was empty";
        return false;
    }
    held.bytes = std::move(exported.bytes);
    held.held_oademo = false;
    held.held_sectors = sectors;
    return true;
}

/// The note beside a recording: its path with the content-note suffix.
///
/// @param file the recording
/// @return the note; a missing file is an empty one
saves::ContentRecord content_note_beside(const fs::path& file) {
    const std::string note_path =
        platform::path_to_utf8(file) + std::string(saves::content_note_suffix);
    std::ifstream input(platform::path_from_utf8(note_path), std::ios::binary);
    if (!input)
        return {};
    input.seekg(0, std::ios::end);
    const std::streamoff length = input.tellg();
    constexpr std::streamoff most_note = 1024 * 1024;
    if (length <= 0 || length > most_note)
        return {};
    std::string text(static_cast<std::size_t>(length), '\0');
    input.seekg(0);
    input.read(text.data(), length);
    if (!input)
        return {};
    return saves::content_record_from_text(text);
}

/// The installed package's file digest: its origin record, or else the folder.
///
/// @param package the installed package
/// @return 64 lower-case hex digits, or empty when neither digest can be made
std::string installed_digest(const content::InstalledPackage& package) {
    if (package.origin && package.origin->sha256) {
        const auto hex = oa::base::sha256::to_hex(*package.origin->sha256);
        return std::string(hex.data(), hex.size());
    }
    if (package.folder.empty())
        return {};
    std::string problem;
    oa::data::mutators::FolderDigests digests;
    const std::optional<oa::base::sha256::Digest> hashed = digests.digest(package.folder, &problem);
    if (!hashed)
        return {};
    const auto hex = oa::base::sha256::to_hex(*hashed);
    return std::string(hex.data(), hex.size());
}

/// The installed oamod whose id or name is the note's mod line.
///
/// The note stores the name the player saw, which may be the package id.
/// An id match is preferred. The list is copied: nothing keeps a pointer into it.
///
/// @param user_folder the player's folder
/// @param label the note's mod line
/// @return the package, or none
std::optional<content::InstalledPackage>
installed_mod_named(const fs::path& user_folder, std::string_view label) {
    if (label.empty() || user_folder.empty())
        return std::nullopt;
    const std::vector<content::InstalledPackage> installed = content::list_installed(user_folder);
    std::optional<content::InstalledPackage> by_name;
    for (const content::InstalledPackage& package : installed) {
        if (package.kind == nullptr || package.kind->name != "oamod")
            continue;
        if (package.key == label)
            return package;
        if (!by_name && package.name == label)
            by_name = package;
    }
    return by_name;
}

/// Lays a `.tad`'s content note over a described replay. An existing
/// `.oademo` is not described here, so it is not overlaid. Brains stay as
/// the recording's sector named them. Mutator names are for the warning only.
///
/// A mod line is the name the player saw. The installed package of that name
/// or id supplies the id, version and digest. A mod that is not installed is
/// kept as a name only, and no package need is written without a digest.
///
/// @param[in,out] replay the described replay
/// @param note the sidecar; a missing one changes nothing
/// @param[in,out] held the warning names, beside the sector's ids
/// @param user_folder the player's folder, parent of Recordings
void overlay_tad_note(
    oademo::Replay& replay,
    const saves::ContentRecord& note,
    ReplayShelf::HeldExport& held,
    const fs::path& user_folder
) {
    if (!note.present)
        return;
    if (!note.base_game.empty()) {
        replay.meta.content.base.id = note.base_game;
        if (note.base_game != "totala")
            replay.meta.content.base.version.clear();
        for (oademo::Need& need : replay.manifest.needs) {
            if (need.kind != "base")
                continue;
            need.id = note.base_game;
            if (note.base_game != "totala")
                need.version.reset();
        }
    }
    replay.meta.content.expansions = note.expansions;
    std::vector<oademo::Need> needs;
    needs.reserve(replay.manifest.needs.size() + note.expansions.size() + 1);
    for (oademo::Need& need : replay.manifest.needs) {
        const bool base = need.kind == "base";
        needs.push_back(std::move(need));
        if (!base)
            continue;
        for (const std::string& id : note.expansions) {
            oademo::Need extra;
            extra.kind = "expansion";
            extra.id = id;
            needs.push_back(std::move(extra));
        }
    }
    if (!note.mod.empty()) {
        const std::optional<content::InstalledPackage> installed =
            installed_mod_named(user_folder, note.mod);
        if (installed) {
            oademo::ModRef mod;
            mod.id = installed->key;
            mod.name = installed->name.empty() ? installed->key : installed->name;
            mod.version = installed->version;
            if (installed->revision != 0)
                mod.revision = installed->revision;
            replay.meta.content.mod = std::move(mod);
            const std::string digest = installed_digest(*installed);
            if (!digest.empty()) {
                oademo::Need extra;
                extra.kind = "oamod";
                extra.id = installed->key;
                if (!installed->version.empty())
                    extra.version = installed->version;
                if (installed->revision != 0)
                    extra.revision = installed->revision;
                extra.sha256 = digest;
                needs.push_back(std::move(extra));
            }
        } else {
            oademo::ModRef mod;
            mod.name = note.mod;
            replay.meta.content.mod = std::move(mod);
        }
    }
    replay.manifest.needs = std::move(needs);
    held.mutator_names.assign(held.mutator_ids.size(), std::string{});
    for (std::size_t at = 0; at < held.mutator_ids.size(); ++at) {
        for (const saves::ContentMutator& one : note.mutators) {
            if (one.id != held.mutator_ids[at])
                continue;
            held.mutator_names[at] = one.name;
            break;
        }
    }
}

/// Compresses a replay for export, so the compression can be stopped.
///
/// @param shelf the shelf
/// @param generation the measurement
/// @param replay the replay
/// @param archive true for a `.oademo` already read; false for one just described
/// @param timeline the index's timeline; empty adds none. A `.oademo` that
///        already carries one keeps it. A wrapped `.tad` is given this one.
/// @return the archive, or the sentence when it cannot be written
oademo::WriteResult compress_export(
    ReplayShelf& shelf,
    uint64_t generation,
    oademo::Replay& replay,
    bool archive,
    const std::string& timeline
) {
    ExportWatch watch{&shelf, generation};
    oademo::WriteWatch hooks;
    hooks.report = export_watch_report;
    hooks.stop = export_watch_stop;
    hooks.context = &watch;
    if (archive) {
        oademo::RecordingExport wrapping;
        wrapping.recorded_with = OA_ENGINE_VERSION;
        wrapping.watch = &hooks;
        // A timeline already in the file is kept. An empty one, or none, is
        // replaced by the index's when the pass has read the replay.
        if (!timeline.empty() && (!replay.timeline.has_value() || replay.timeline->empty()))
            wrapping.timeline = timeline;
        return oademo::export_replay(replay, wrapping);
    }
    if (!timeline.empty())
        replay.timeline = timeline;
    oademo::WriteOptions options;
    options.level = oademo::level_on_export;
    options.watch = hooks;
    return oademo::write(replay, options);
}

/// One file the worker measured, or a measurement that was cancelled.
struct MeasuredExport {
    bool cancelled{};
    ReplayShelf::HeldExport held{};
};

/// Measures one replay and keeps only the format the dialog is showing.
///
/// A file that cannot be read is marked, and the dialog stays open. Cancel
/// is checked before a failure is kept, so a stopped compression is not
/// reported as a replay that could not be read.
///
/// @param shelf the shelf
/// @param generation the measurement
/// @param recordings the Recordings folder
/// @param path the replay, below Recordings
/// @param title the name the file is given
/// @param timeline the index's timeline, copied on the main thread; empty for none
/// @return the measurement
MeasuredExport measure_export_file(
    ReplayShelf& shelf,
    uint64_t generation,
    const fs::path& recordings,
    const std::string& path,
    const std::string& title,
    const std::string& timeline
) {
    MeasuredExport measured;
    ReplayShelf::HeldExport& held = measured.held;
    held.path = path;
    held.title = title;
    held.timeline = timeline;
    if (export_stopped(shelf, generation)) {
        measured.cancelled = true;
        return measured;
    }
    std::optional<fs::path> file = replay_file(recordings, path);
    std::vector<uint8_t> bytes;
    if (!file || !read_replay_bytes(*file, bytes)) {
        held.unreadable = true;
        held.why = "the file could not be read";
        return measured;
    }
    export_note_bytes(shelf, generation, 0, bytes.size());
    const auto fail_read = [&](std::string why) {
        if (export_stopped(shelf, generation)) {
            measured.cancelled = true;
            return;
        }
        held.unreadable = true;
        held.ready = false;
        held.bytes.clear();
        held.why = std::move(why);
    };
    try {
        oademo::Replay replay;
        bool from_archive = false;
        std::span<const uint8_t> recording;
        if (oademo::recognised(bytes)) {
            oademo::ReadResult read = oademo::read(bytes);
            bytes.clear();
            bytes.shrink_to_fit();
            if (export_stopped(shelf, generation)) {
                measured.cancelled = true;
                return measured;
            }
            if (!read.ok() || !read.replay) {
                fail_read(
                    read.error.empty() ? std::string("the file could not be read") : read.error
                );
                return measured;
            }
            replay = std::move(*read.replay);
            from_archive = true;
            recording = replay.recording;
        } else {
            recording = bytes;
        }
        std::string why;
        if (!measure_tad_sizes(recording, held, why)) {
            fail_read(why.empty() ? std::string("the file could not be read") : std::move(why));
            return measured;
        }
        if (export_stopped(shelf, generation)) {
            measured.cancelled = true;
            return measured;
        }
        if (!from_archive) {
            demo::ImportedRecording described =
                demo::describe_imported_recording(bytes, OA_ENGINE_VERSION);
            if (export_stopped(shelf, generation)) {
                measured.cancelled = true;
                return measured;
            }
            if (!described.ok()) {
                fail_read(
                    described.error.empty() ? std::string("the file could not be read")
                                            : described.error
                );
                return measured;
            }
            if (file)
                overlay_tad_note(
                    described.replay, content_note_beside(*file), held, recordings.parent_path()
                );
            // The description owns its recording, so the file's bytes can go
            // once a `.tad` has been kept from them. A `.oademo` does not need them.
            const bool want_oademo = shelf.export_want_oademo.load();
            if (!want_oademo) {
                if (!keep_tad_bytes(shelf, bytes, held, why)) {
                    fail_read(
                        why.empty() ? std::string("the file could not be read") : std::move(why)
                    );
                    return measured;
                }
            }
            bytes.clear();
            bytes.shrink_to_fit();
            replay = std::move(described.replay);
            recording = {};
        }
        oademo::WriteResult archive =
            compress_export(shelf, generation, replay, from_archive, timeline);
        if (export_stopped(shelf, generation)) {
            measured.cancelled = true;
            return measured;
        }
        if (!archive.ok() || archive.bytes.empty()) {
            fail_read(archive.error.empty() ? std::string("the export was empty") : archive.error);
            return measured;
        }
        held.oademo_bytes = archive.bytes.size();
        // Read the choice again, now the slow compression is done. A `.tad`
        // of a raw recording has to be the file's own bytes, so one that was
        // not kept before those bytes were released is left empty: Export
        // reads the file again rather than wrapping the described recording.
        const bool want_oademo = shelf.export_want_oademo.load();
        if (want_oademo) {
            held.bytes = std::move(archive.bytes);
            held.held_oademo = true;
            held.held_sectors = true;
        } else if (from_archive) {
            archive.bytes.clear();
            archive.bytes.shrink_to_fit();
            if (!keep_tad_bytes(shelf, replay.recording, held, why)) {
                fail_read(why.empty() ? std::string("the file could not be read") : std::move(why));
                return measured;
            }
        } else if (
            !held.bytes.empty() && !held.held_oademo &&
            held.held_sectors == shelf.export_want_sectors.load()
        ) {
            archive.bytes.clear();
            archive.bytes.shrink_to_fit();
        } else {
            archive.bytes.clear();
            archive.bytes.shrink_to_fit();
            held.bytes.clear();
        }
        replay.recording.clear();
        replay.recording.shrink_to_fit();
        const bool as_oademo = shelf.export_want_oademo.load();
        const bool sectors = shelf.export_want_sectors.load();
        const bool matches =
            !held.bytes.empty() &&
            (as_oademo ? held.held_oademo : !held.held_oademo && held.held_sectors == sectors);
        if (!matches)
            held.bytes.clear();
        held.ready = held.oademo_bytes > 0 && held.tad_bytes > 0 && held.tad_without_bytes > 0;
    } catch (const std::exception& failed) {
        if (export_stopped(shelf, generation)) {
            measured.cancelled = true;
            return measured;
        }
        held.unreadable = true;
        held.ready = false;
        held.bytes.clear();
        held.why = failed.what();
    }
    return measured;
}

/// Makes only the file the dialog is now writing. The sizes stay as measured.
///
/// A `.tad` is `tad::export_recording` with the switch's sectors and is not
/// compressed. A `.oademo` is compressed only when that format is asked for.
///
/// @param shelf the shelf
/// @param generation the measurement
/// @param recordings the Recordings folder
/// @param path the replay, below Recordings
/// @param title the name the file is given
/// @param timeline the index's timeline, copied on the main thread; empty for none
/// @return the file, or a measurement that was cancelled
MeasuredExport rebuild_export_file(
    ReplayShelf& shelf,
    uint64_t generation,
    const fs::path& recordings,
    const std::string& path,
    const std::string& title,
    const std::string& timeline
) {
    MeasuredExport measured;
    ReplayShelf::HeldExport& held = measured.held;
    held.path = path;
    held.title = title;
    held.timeline = timeline;
    if (export_stopped(shelf, generation)) {
        measured.cancelled = true;
        return measured;
    }
    std::optional<fs::path> file = replay_file(recordings, path);
    std::vector<uint8_t> bytes;
    if (!file || !read_replay_bytes(*file, bytes)) {
        held.unreadable = true;
        held.why = "the file could not be read";
        return measured;
    }
    const auto fail = [&](std::string why) {
        if (export_stopped(shelf, generation)) {
            measured.cancelled = true;
            return;
        }
        held.unreadable = true;
        held.ready = false;
        held.bytes.clear();
        held.why = std::move(why);
    };
    try {
        if (!shelf.export_want_oademo.load()) {
            std::span<const uint8_t> recording;
            oademo::Replay archive;
            if (oademo::recognised(bytes)) {
                oademo::ReadResult read = oademo::read(bytes);
                bytes.clear();
                bytes.shrink_to_fit();
                if (export_stopped(shelf, generation)) {
                    measured.cancelled = true;
                    return measured;
                }
                if (!read.ok() || !read.replay) {
                    fail(
                        read.error.empty() ? std::string("the file could not be read") : read.error
                    );
                    return measured;
                }
                archive = std::move(*read.replay);
                recording = archive.recording;
            } else {
                recording = bytes;
            }
            std::string why;
            if (!keep_tad_bytes(shelf, recording, held, why)) {
                fail(why.empty() ? std::string("the file could not be read") : std::move(why));
                return measured;
            }
            if (export_stopped(shelf, generation)) {
                measured.cancelled = true;
                held.bytes.clear();
                return measured;
            }
            held.ready = true;
            return measured;
        }
        oademo::Replay replay;
        bool from_archive = false;
        if (oademo::recognised(bytes)) {
            oademo::ReadResult read = oademo::read(bytes);
            bytes.clear();
            bytes.shrink_to_fit();
            if (export_stopped(shelf, generation)) {
                measured.cancelled = true;
                return measured;
            }
            if (!read.ok() || !read.replay) {
                fail(read.error.empty() ? std::string("the file could not be read") : read.error);
                return measured;
            }
            replay = std::move(*read.replay);
            from_archive = true;
        } else {
            demo::ImportedRecording described =
                demo::describe_imported_recording(bytes, OA_ENGINE_VERSION);
            if (export_stopped(shelf, generation)) {
                measured.cancelled = true;
                return measured;
            }
            if (!described.ok()) {
                fail(
                    described.error.empty() ? std::string("the file could not be read")
                                            : described.error
                );
                return measured;
            }
            if (file)
                overlay_tad_note(
                    described.replay, content_note_beside(*file), held, recordings.parent_path()
                );
            bytes.clear();
            bytes.shrink_to_fit();
            replay = std::move(described.replay);
        }
        oademo::WriteResult archive =
            compress_export(shelf, generation, replay, from_archive, timeline);
        if (export_stopped(shelf, generation)) {
            measured.cancelled = true;
            return measured;
        }
        if (!archive.ok() || archive.bytes.empty()) {
            fail(archive.error.empty() ? std::string("the export was empty") : archive.error);
            return measured;
        }
        held.bytes = std::move(archive.bytes);
        held.held_oademo = true;
        held.held_sectors = true;
        held.ready = true;
    } catch (const std::exception& failed) {
        if (export_stopped(shelf, generation)) {
            measured.cancelled = true;
            return measured;
        }
        held.unreadable = true;
        held.ready = false;
        held.bytes.clear();
        held.why = failed.what();
    }
    return measured;
}

/// The export worker: one measurement at a time, off the main thread.
///
/// @param argument the shelf
void export_worker(void* argument) {
    auto& shelf = *static_cast<ReplayShelf*>(argument);
    for (;;) {
        ReplayShelf::ExportAsk ask;
        {
            base::threads::LockGuard guard(shelf.export_mutex);
            while (!shelf.export_stop && !shelf.export_pending)
                shelf.export_wake.wait(shelf.export_mutex);
            if (shelf.export_stop)
                return;
            ask = std::move(shelf.export_ask);
            shelf.export_ask = {};
            shelf.export_pending = false;
            shelf.export_cancel.store(false);
            shelf.export_busy.store(true);
        }
        ReplayShelf::ExportResult result;
        result.generation = ask.generation;
        result.rebuild = ask.rebuild;
        bool cancelled = false;
        for (std::size_t at = 0; at < ask.paths.size(); ++at) {
            if (export_stopped(shelf, ask.generation)) {
                cancelled = true;
                break;
            }
            const std::string title = at < ask.titles.size() ? ask.titles[at] : ask.paths[at];
            const std::string timeline =
                at < ask.timelines.size() ? ask.timelines[at] : std::string{};
            MeasuredExport measured =
                ask.rebuild
                    ? rebuild_export_file(
                          shelf, ask.generation, ask.recordings, ask.paths[at], title, timeline
                      )
                    : measure_export_file(
                          shelf, ask.generation, ask.recordings, ask.paths[at], title, timeline
                      );
            if (measured.cancelled) {
                cancelled = true;
                break;
            }
            result.held.push_back(std::move(measured.held));
        }
        {
            base::threads::LockGuard guard(shelf.export_mutex);
            const bool keep = !cancelled && !shelf.export_cancel.load() &&
                              ask.generation == shelf.export_generation.load();
            if (keep)
                shelf.export_result = std::move(result);
            shelf.export_busy.store(false);
        }
    }
}

/// Asks the export worker to measure. The generation moves on, so a measurement
/// already running does not publish over this one.
///
/// @param[in,out] shelf the shelf
/// @param ask the replays; its generation is set here
/// @return false when the worker could not start
bool post_export(ReplayShelf& shelf, ReplayShelf::ExportAsk ask) {
    bool started = true;
    {
        base::threads::LockGuard guard(shelf.export_mutex);
        ask.generation = shelf.export_generation.fetch_add(1) + 1;
        shelf.export_cancel.store(true);
        shelf.export_ask = std::move(ask);
        shelf.export_pending = true;
        shelf.export_result.reset();
        if (!shelf.export_started) {
            if (!base::threads::start_thread(shelf.export_thread, export_worker, &shelf)) {
                shelf.export_pending = false;
                shelf.export_cancel.store(false);
                started = false;
            } else {
                shelf.export_started = true;
            }
        }
    }
    shelf.export_wake.notify_all();
    return started;
}

/// Tells whether held bytes are the format the dialog is writing.
///
/// @param held the measurement
/// @param as_oademo a `.oademo` is chosen
/// @param sectors a `.tad` keeps Open Annihilation's own sectors
/// @return true when `held.bytes` is that file
bool held_matches(const ReplayShelf::HeldExport& held, bool as_oademo, bool sectors) {
    if (!held.ready || held.unreadable || held.bytes.empty())
        return false;
    if (as_oademo)
        return held.held_oademo;
    return !held.held_oademo && held.held_sectors == sectors;
}

/// How many of the measured files have been renamed into place.
///
/// @param held the measurements
/// @return the count
uint32_t exports_written(const std::vector<ReplayShelf::HeldExport>& held) {
    uint32_t done = 0;
    for (const ReplayShelf::HeldExport& one : held)
        if (one.written)
            ++done;
    return done;
}

/// The first measured file that has not been renamed into place.
///
/// @param held the measurements
/// @return its place, or `held.size()` when every one has been written
std::size_t next_unwritten(const std::vector<ReplayShelf::HeldExport>& held) {
    for (std::size_t place = 0; place < held.size(); ++place)
        if (!held[place].written)
            return place;
    return held.size();
}

/// The title a file name shows, without a trailing `.oademo` or `.tad`.
///
/// @param path the replay's path below Recordings
/// @return the file's name, without that ending
std::string export_title_from_path(std::string_view path) {
    const std::size_t slash = path.rfind('/');
    std::string title(slash == std::string_view::npos ? path : path.substr(slash + 1));
    return strip_replay_extension(std::move(title));
}

/// The index's timeline for one replay. Empty when the pass has not read it,
/// or the read left nothing the export should carry.
///
/// @param shelf the shelf
/// @param path the replay, below Recordings
/// @return the timeline text
std::string indexed_timeline(const ReplayShelf& shelf, std::string_view path) {
    const replay_data::Replay* row = shown_replay(shelf, path);
    if (row == nullptr || !row->timeline.has_value() || row->timeline->empty())
        return {};
    return *row->timeline;
}

/// Copies a timeline and a watched tick written while a read was running onto
/// that read's result, for a file the read did not open again.
///
/// @param[in,out] ended the read that ended
/// @param live the index the main thread holds
void keep_passed_index(replay_data::Index& ended, const replay_data::Index& live) {
    for (replay_data::Replay& replay : ended.replays) {
        const replay_data::Replay* prior = nullptr;
        for (const replay_data::Replay& have : live.replays) {
            if (have.path != replay.path)
                continue;
            prior = &have;
            break;
        }
        if (prior == nullptr || prior->size != replay.size || prior->modified != replay.modified ||
            prior->file != replay.file)
            continue;
        if (prior->timeline.has_value())
            replay.timeline = prior->timeline;
        if (prior->watched_tick.has_value())
            replay.watched_tick = prior->watched_tick;
        // A cleared stamp stays cleared. A refusal's stamp stays with the file.
        replay.read_stamp = prior->read_stamp;
    }
}

/// Won or Lost when the winners are known, and nothing when they are not.
///
/// @param number the player's number; 0 is nobody
/// @param winners the winners' numbers
/// @return the result
library::RecordedResult recorded_result(int number, const std::vector<int>& winners) {
    if (number == 0 || winners.empty())
        return library::RecordedResult::none;
    for (const int winner : winners)
        if (winner == number)
            return library::RecordedResult::won;
    return library::RecordedResult::lost;
}

/// Fills a row's chip, result, standings and timeline marks from its timeline.
/// `final.units` is not shown.
///
/// @param[in,out] row the row
void apply_timeline(library::ReplayRow& row) {
    // A timeline with no `events` member is read again. The text stays until
    // the new one is stored, so the marks below still draw from the old one.
    // An empty timeline is a pass that stored nothing, and is not read again.
    const bool held =
        row.replay.timeline.has_value() && !oademo::timeline_wants_events(*row.replay.timeline);
    row.indexing = replay_index_pass_available && !replay_data::damaged(row.replay) && !held;
    if (!row.replay.timeline.has_value() || row.replay.timeline->empty())
        return;
    const std::optional<oademo::TimelineIndex> parsed =
        oademo::parse_timeline_index(*row.replay.timeline);
    if (!parsed)
        return;
    row.length_ticks = parsed->length_ticks;
    int recorder = 0;
    for (const replay_data::Player& player : row.replay.players) {
        if (!player.recorded_here)
            continue;
        recorder = player.number;
        break;
    }
    row.recorded = recorded_result(recorder, parsed->result.winners);
    row.standings.resize(row.replay.players.size());
    for (std::size_t place = 0; place < row.replay.players.size(); ++place) {
        const replay_data::Player& player = row.replay.players[place];
        library::PlayerStanding& standing = row.standings[place];
        standing.result = recorded_result(player.number, parsed->result.winners);
        for (const oademo::IndexPlayer& one : parsed->players) {
            if (one.number != player.number)
                continue;
            // A player who only watched has no result and no counts, so the
            // card draws no line under that name. The row's result is nothing
            // when the recording player was that watcher.
            if (one.watcher) {
                standing.result = library::RecordedResult::none;
                standing.known = false;
                if (player.recorded_here)
                    row.recorded = library::RecordedResult::none;
                break;
            }
            standing.known = true;
            standing.kills = one.final.kills;
            standing.losses = one.final.losses;
            break;
        }
    }
    for (const oademo::IndexMarker& marker : parsed->markers) {
        if (marker.kind == oademo::MarkerKind::chat)
            continue;
        library::TimelineMark mark;
        mark.tick = marker.tick;
        mark.player = marker.player;
        mark.commander = marker.kind == oademo::MarkerKind::commander_death;
        row.marks.push_back(std::move(mark));
    }
}

/// A gap Watch would have to fetch, turn on or refuse. An installed mod that
/// is not the one playing is played with `--mod-dir`. A kept map pack is
/// mounted with `--replay-map-pack`. A map named only by its title is not a
/// package Watch fetches.
///
/// @param gaps the row's comparison
/// @return true when the pass leaves the row out
bool index_pass_blocked(const std::vector<replay_data::ContentGap>& gaps) {
    for (const replay_data::ContentGap& gap : gaps) {
        if (gap.play_installed)
            continue;
        if (gap.kind == replay_data::GapKind::map && gap.sha256.empty() && gap.id.empty())
            continue;
        if (gap.fetchable || gap.turn_on || gap.other_release ||
            gap.kind == replay_data::GapKind::engine || gap.block != replay_data::GapBlock::none ||
            gap.kind == replay_data::GapKind::expansion_missing ||
            gap.kind == replay_data::GapKind::mutator ||
            gap.kind == replay_data::GapKind::mod_missing || gap.kind == replay_data::GapKind::map)
            return true;
    }
    return false;
}

#if !defined(__ANDROID__) && OA_PROCESS_SPAWNING

/// Spawns of one file that fail before that row is left as unread no longer.
constexpr int kIndexSpawnTries = 3;
/// Frames between a spawn that failed and the next try.
constexpr int kIndexBackoffFrames = 45;

/// Writes a timeline onto one row and saves the index.
///
/// @param shelf the shelf
/// @param user_folder the player's folder
/// @param path the replay, below Recordings
/// @param text the timeline the pass wrote
/// @return true when the row was there
bool remember_index_text(
    ReplayShelf& shelf, const fs::path& user_folder, const std::string& path, std::string text
) {
    if (!shelf.listed)
        return false;
    bool stored = false;
    for (replay_data::Replay& replay : shelf.index.replays) {
        if (replay.path != path)
            continue;
        replay.timeline = std::move(text);
        replay.read_stamp.reset();
        stored = true;
        break;
    }
    if (!stored || user_folder.empty())
        return stored;
    const fs::path folder = user_folder / std::string(recordings_folder_name);
    if (!replay_data::save_index(folder, shelf.index))
        replays_log("the replays' index could not be saved");
    return true;
}

/// Remembers that a pass could not read one file. A timeline already stored
/// stays, so the result and the marks remain, and a later change of what is
/// installed reads the file again. An empty timeline is not stored.
///
/// @param shelf the shelf
/// @param user_folder the player's folder
/// @param path the replay, below Recordings
/// @param stamp what is installed now
/// @return true when the row was there
bool remember_index_refusal(
    ReplayShelf& shelf,
    const fs::path& user_folder,
    const std::string& path,
    const std::string& stamp
) {
    if (!shelf.listed)
        return false;
    bool stored = false;
    for (replay_data::Replay& replay : shelf.index.replays) {
        if (replay.path != path)
            continue;
        replay.read_stamp = stamp;
        stored = true;
        break;
    }
    if (!stored || user_folder.empty())
        return stored;
    const fs::path folder = user_folder / std::string(recordings_folder_name);
    if (!replay_data::save_index(folder, shelf.index))
        replays_log("the replays' index could not be saved");
    return true;
}

/// Counts a spawn that did not start. After a few tries the row is left until
/// what is installed changes, so the same file is not started on every frame.
///
/// @param shelf the shelf
/// @param user_folder the player's folder
/// @param path the replay
/// @param stamp what is installed now
/// @return true when the refusal was stored
bool note_index_spawn_failure(
    ReplayShelf& shelf,
    const fs::path& user_folder,
    const std::string& path,
    const std::string& stamp
) {
    if (shelf.index_fail_path != path) {
        shelf.index_fail_path = path;
        shelf.index_fail_count = 0;
    }
    ++shelf.index_fail_count;
    if (shelf.index_fail_count >= kIndexSpawnTries) {
        const bool stored = remember_index_refusal(shelf, user_folder, path, stamp);
        shelf.index_fail_count = 0;
        shelf.index_fail_path.clear();
        shelf.index_backoff = 0;
        return stored;
    }
    shelf.index_backoff = kIndexBackoffFrames;
    return false;
}

/// The replay to read next: the selected one when it still needs a timeline,
/// otherwise the first unindexed path. Damaged files, a row Watch would have
/// to fetch or refuse, and a refusal at the current stamp are left out.
///
/// @param shelf the shelf
/// @param model the Library
/// @param stamp what is installed now
/// @return the path; empty when every row has been read or is waiting
std::string
next_unindexed(const ReplayShelf& shelf, const library::Library& model, const std::string& stamp) {
    const auto needs = [&](const replay_data::Replay& replay) {
        if (replay_data::damaged(replay))
            return false;
        // Kept when the stored text already names its battle events. A
        // version 1 timeline with no `events` member is read again, and the
        // text stays until that read is stored.
        if (replay.timeline.has_value() && !oademo::timeline_wants_events(*replay.timeline))
            return false;
        if (replay.read_stamp.has_value() && *replay.read_stamp == stamp)
            return false;
        for (const library::ReplayRow& row : model.inputs.replays.rows) {
            if (row.replay.path != replay.path)
                continue;
            return !index_pass_blocked(row.gaps);
        }
        // The row is not in the model yet. The next fill adds its gaps.
        return false;
    };
    if (model.replays.selected) {
        for (const replay_data::Replay& replay : shelf.index.replays) {
            if (replay.path == *model.replays.selected && needs(replay))
                return replay.path;
        }
    }
    const replay_data::Replay* earliest = nullptr;
    for (const replay_data::Replay& replay : shelf.index.replays) {
        if (!needs(replay))
            continue;
        if (earliest == nullptr || replay.path < earliest->path)
            earliest = &replay;
    }
    return earliest != nullptr ? earliest->path : std::string{};
}

/// Starts one index pass through the shared launcher. Its standard output is
/// discarded, so a line it prints cannot fill a pipe. It plays in the player's
/// folder, and a replay whose mod is installed is started with that folder.
///
/// @param shelf the shelf
/// @param runtime the runtime, for the installed mod
/// @param user_folder the player's folder
/// @param game_dir the game's folder; empty is left off the command
/// @param relative the replay, below Recordings
/// @return false when the process did not start
bool spawn_index_pass(
    ReplayShelf& shelf,
    Runtime& runtime,
    const fs::path& user_folder,
    const fs::path& game_dir,
    const std::string& relative
) {
    const fs::path recordings = user_folder / std::string(recordings_folder_name);
    const std::optional<fs::path> file = replay_file(recordings, relative);
    if (!file)
        return false;
    std::error_code failed;
    fs::path absolute = fs::absolute(*file, failed);
    if (failed)
        absolute = *file;
    IndexPassRequest request;
    request.recording = absolute;
    request.user_folder = user_folder;
    request.game_dir = game_dir;
    request.scratch = user_folder / ".index-pass";
    request.output_name = "out.json";
    request.pipe_stdout = false;
    for (const replay_data::Replay& replay : shelf.index.replays) {
        if (replay.path != relative)
            continue;
        if (const std::optional<fs::path> mod_folder =
                index_pass_mod_folder(runtime, shelf, replay))
            request.mod_folder = *mod_folder;
        if (const std::optional<fs::path> map_folder =
                index_pass_map_folder(runtime, shelf, replay))
            request.map_pack = *map_folder;
        break;
    }
    const IndexPassHandles started = launch_index_pass(request);
    if (!started.started || started.process == nullptr)
        return false;
    shelf.index_process = started.process;
    shelf.index_pid = started.pid;
    shelf.index_target = relative;
    shelf.index_output = started.output;
    shelf.index_errors = started.errors;
    shelf.index_scratch = request.scratch;
    shelf.index_fail_count = 0;
    shelf.index_fail_path.clear();
    replays_log("index pass started pid " + std::to_string(started.pid));
    return true;
}

/// Polls the index pass and starts the next file when none is running.
///
/// One process at a time. It does not run while a match does, nor while the
/// Library is closed, and a pass stopped then writes nothing.
///
/// @param shelf the shelf
/// @param model the Library, whose selected replay is read first
/// @param library_open the Library is on the screen
/// @param in_match a match is running
/// @param user_folder the player's folder
/// @param game_dir the game's folder
/// @param runtime the runtime, for the installed mod and the content stamp
/// @return true when a timeline or a refusal was stored
bool advance_replay_index(
    ReplayShelf& shelf,
    const library::Library& model,
    bool library_open,
    bool in_match,
    const fs::path& user_folder,
    const fs::path& game_dir,
    Runtime& runtime
) {
    if (!library_open || in_match || !shelf.listed || user_folder.empty()) {
        stop_replay_index_pass(shelf);
        return false;
    }
    if (shelf.index_process != nullptr) {
        int exit_code = 0;
        if (!SDL_WaitProcess(shelf.index_process, false, &exit_code))
            return false;
        const int pid = shelf.index_pid;
        const std::string target = shelf.index_target;
        const fs::path output = shelf.index_output;
        const fs::path errors = shelf.index_errors;
        SDL_DestroyProcess(shelf.index_process);
        shelf.index_process = nullptr;
        shelf.index_pid = 0;
        shelf.index_target.clear();
        shelf.index_output.clear();
        shelf.index_errors.clear();
        replays_log("index pass stopped pid " + std::to_string(pid));
        std::string text;
        if (exit_code == 0)
            text = read_text_file(output);
        const std::string reason = index_pass_reason(errors);
        std::error_code failed;
        fs::remove(output, failed);
        fs::remove(errors, failed);
        const bool usable =
            exit_code == 0 && !text.empty() && oademo::parse_timeline_index(text).has_value();
        if (usable) {
            replays_log("index pass read " + target);
            return remember_index_text(shelf, user_folder, target, std::move(text));
        }
        std::string line =
            "index pass could not read " + target + " (exit " + std::to_string(exit_code) + ")";
        if (!reason.empty()) {
            line += ": ";
            line += reason;
        }
        replays_log(line);
        return remember_index_refusal(
            shelf, user_folder, target, index_pass_content_stamp(runtime)
        );
    }
    if (shelf.index_backoff > 0) {
        --shelf.index_backoff;
        return false;
    }
    const std::string stamp = index_pass_content_stamp(runtime);
    const std::string relative = next_unindexed(shelf, model, stamp);
    if (relative.empty())
        return false;
    if (spawn_index_pass(shelf, runtime, user_folder, game_dir, relative))
        return false;
    replays_log("the index pass could not start");
    return note_index_spawn_failure(shelf, user_folder, relative, stamp);
}

#else

bool advance_replay_index(
    ReplayShelf&, const library::Library&, bool, bool, const fs::path&, const fs::path&, Runtime&
) {
    return false;
}

#endif

} // namespace

/// Wraps a raw `.tad` the way Export does, so a share sends the engine line,
/// the mod and the content note beside the file. `export_tad` with its
/// defaults would send the base game only and an empty engine line.
///
/// @param bytes the `.tad`
/// @param file the recording, so the note beside it can be read
/// @param user_folder the player's folder, parent of Recordings
/// @return the `.oademo`, or the sentence when it cannot be written
oademo::WriteResult
wrap_shared_tad(std::span<const uint8_t> bytes, const fs::path& file, const fs::path& user_folder) {
    demo::ImportedRecording described = demo::describe_imported_recording(bytes, OA_ENGINE_VERSION);
    if (!described.ok()) {
        oademo::WriteResult failed;
        failed.error = described.error.empty() ? "the file could not be read" : described.error;
        return failed;
    }
    ReplayShelf::HeldExport held;
    overlay_tad_note(described.replay, content_note_beside(file), held, user_folder);
    oademo::WriteOptions options;
    options.level = oademo::level_on_export;
    return oademo::write(described.replay, options);
}

/// How the Replays section reads what is installed and puts expansions back.
struct ReplayContentAccess {
    /// Remembers the stock maps' stems and mission names once.
    ///
    /// @param runtime the runtime
    /// @param shelf the section's shelf
    static void remember_stock_maps(Runtime& runtime, ReplayShelf& shelf);

    /// What is installed, for one comparison.
    ///
    /// @param runtime the runtime
    /// @param shelf the section's shelf
    /// @return the installed content
    static replay_data::HaveContent have_content(Runtime& runtime, ReplayShelf& shelf);

    /// Turns expansions on, and plays this replay with one mod, remembering
    /// what the player had chosen.
    ///
    /// A soft restart applies the change. The replay plays when the next run
    /// starts, and `from_tick` is kept for that start, which seeks there once
    /// the replay can say where it is. An expansion the choice refuses is
    /// left as it was, and the mod is not switched. `--mod-dir` and
    /// `--base-game` keep the mod they named.
    ///
    /// @param runtime the runtime
    /// @param file the replay
    /// @param ids the expansions to turn on
    /// @param mod_folder the mod to play; empty leaves the mod as it is
    /// @param pack_folder the kept map pack to mount after the restart; empty leaves Maps
    /// @param from_tick the tick Watch from jumps to after the restart; none
    /// starts at the beginning
    /// @return 2 when a restart will play it, 1 when nothing had to change, 0 when it could not
    static int switch_for_replay(
        Runtime& runtime,
        const fs::path& file,
        const std::vector<std::string>& ids,
        const fs::path& mod_folder,
        const fs::path& pack_folder,
        std::optional<uint32_t> from_tick
    );

    /// Puts the player's expansions and mod back once the player has left the
    /// end screen of the replay that changed them, and at the next start when
    /// that replay will not play. The end screen itself keeps the choice.
    ///
    /// @param runtime the runtime
    /// @return true when a restart was asked for
    static bool restore_replay_expansions(Runtime& runtime);

    /// What would change the installed content, without reading a package file.
    ///
    /// It begins with the engine's build and `|`, so that a new build reads
    /// once more each replay an older one refused. Those reads are the
    /// background pass's own, one at a time while the Library is open.
    ///
    /// @param runtime the runtime
    /// @return a stamp that changes when the installed content or the engine's build does
    static std::string installed_stamp(Runtime& runtime);

    /// The first Mods folder of one mod id.
    ///
    /// A need that names a release does not use this. That release's own
    /// folder is played.
    ///
    /// @param runtime the runtime
    /// @param id the mod
    /// @return the folder, or none when that mod is not installed
    static std::optional<fs::path> installed_mod_folder(Runtime& runtime, std::string_view id);

    /// The mod folder this run layers over the game.
    ///
    /// @param runtime the runtime
    /// @return the folder, or none when no mod plays
    static std::optional<fs::path> played_mod_folder(const Runtime& runtime);

    /// The folder a mod need plays from, once any release it named is installed.
    ///
    /// A need that names no release uses the first Mods folder of the id.
    /// A need that names a version, a revision or a digest uses the folder of
    /// the release that matches, never the first folder of the id.
    ///
    /// @param runtime the runtime
    /// @param shelf the section's shelf; its cached install is read again
    /// @param id the mod
    /// @param version the release's version; empty when the need names none
    /// @param revision the release's revision, when the need names one
    /// @param sha the release's digest; empty when the need names none
    /// @param names_release true when the need names a version, revision or digest
    /// @return the folder, or none when that release is not installed
    static std::optional<fs::path> folder_to_play(
        Runtime& runtime,
        ReplayShelf& shelf,
        std::string_view id,
        std::string_view version,
        const std::optional<int64_t>& revision,
        std::string_view sha,
        bool names_release
    );
};

void ReplayContentAccess::remember_stock_maps(Runtime& runtime, ReplayShelf& shelf) {
    if (shelf.stock_maps_read)
        return;
    shelf.stock_maps_read = true;
    try {
        const std::vector<std::string> files = runtime.assets_.list_effective("maps", ".ota");
        for (const std::string& path : files) {
            const std::string stem = platform::path_to_utf8(path_from_utf8(path).stem());
            if (!stem.empty())
                shelf.stock_maps.push_back(stem);
            const std::optional<std::vector<uint8_t>> bytes = runtime.read(path);
            if (!bytes)
                continue;
            const std::string_view text(
                reinterpret_cast<const char*>(bytes->data()), bytes->size()
            );
            const auto parsed = oa::formats::ota::parse(text);
            if (!parsed.ok() || parsed.metadata->mission_name.empty())
                continue;
            shelf.stock_maps.push_back(parsed.metadata->mission_name);
        }
    } catch (const std::exception&) {
    }
}

std::string ReplayContentAccess::installed_stamp(Runtime& runtime) {
    // A build that may read what an older one could not tries each refused
    // replay again.
    std::string stamp = runtime.options_.engine_build;
    stamp.push_back('|');
    const auto& mods = runtime.engine_settings_state();
    std::error_code error;
    for (const std::string& folder : mods.mod_folders) {
        stamp += folder;
        stamp.push_back('\n');
        // A replace keeps the path and swaps a .backup inside the folder, so
        // the path alone does not say which release is there.
        const auto written = fs::last_write_time(path_from_utf8(folder), error);
        if (!error)
            stamp += std::to_string(static_cast<long long>(written.time_since_epoch().count()));
        stamp.push_back('\n');
    }
    if (!runtime.user_folder_.empty()) {
        const fs::path mods_root = runtime.user_folder_ / std::string(user_mods_folder_name);
        const auto mods_written = fs::last_write_time(mods_root, error);
        if (!error)
            stamp +=
                std::to_string(static_cast<long long>(mods_written.time_since_epoch().count()));
    }
    stamp.push_back('|');
    if (const oa::data::mod_profile::ModProfile* profile = runtime.mod_profile()) {
        stamp += profile->id;
        stamp.push_back('\n');
        stamp += profile->name;
    }
    stamp.push_back('|');
    const oa::data::content_bundles::ContentChoice choice = runtime.content_choice();
    for (const std::string& id : choice.expansions) {
        stamp += id;
        stamp.push_back(',');
    }
    stamp.push_back('|');
    // The write times below change when a pack is added, removed or replaced.
    // The manifests are read once, in have_content, when this stamp changes.
    try {
        const std::span<const PackMap> maps = runtime.map_packs().maps();
        stamp += std::to_string(maps.size());
        if (!maps.empty()) {
            stamp.push_back(':');
            stamp += maps.front().name;
        }
    } catch (const std::exception&) {
    }
    // A pack updated in place keeps its name. The folder's write time changes
    // with the files, as a mod folder's does.
    if (!runtime.user_folder_.empty()) {
        const fs::path maps_root =
            runtime.user_folder_ / std::string(package_install::oamap::maps_folder_name);
        const auto maps_written = fs::last_write_time(maps_root, error);
        if (!error)
            stamp +=
                std::to_string(static_cast<long long>(maps_written.time_since_epoch().count()));
        stamp.push_back('\n');
        if (fs::is_directory(maps_root, error)) {
            for (fs::directory_iterator cursor(maps_root, error), end; !error && cursor != end;
                 cursor.increment(error)) {
                const std::string name = platform::path_to_utf8(cursor->path().filename());
                if (name.empty() || name.front() == '.')
                    continue;
                std::error_code directory;
                if (!cursor->is_directory(directory) || directory)
                    continue;
                stamp += name;
                stamp.push_back('\n');
                const auto written = fs::last_write_time(cursor->path(), directory);
                if (!directory)
                    stamp +=
                        std::to_string(static_cast<long long>(written.time_since_epoch().count()));
                stamp.push_back('\n');
            }
        }
    }
    stamp.push_back('|');
    const fs::path mutators =
        runtime.user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name);
    const auto written = fs::last_write_time(mutators, error);
    if (!error) {
        // The clock's count is wider than the overloads of std::to_string.
        stamp += std::to_string(static_cast<long long>(written.time_since_epoch().count()));
    }
    stamp.push_back('|');
    const fs::path releases =
        runtime.user_folder_ / std::string(package_install::replay_releases_folder_name);
    const auto releases_written = fs::last_write_time(releases, error);
    if (!error) {
        stamp +=
            std::to_string(static_cast<long long>(releases_written.time_since_epoch().count()));
    }
    return stamp;
}

std::optional<fs::path>
ReplayContentAccess::installed_mod_folder(Runtime& runtime, std::string_view id) {
    if (id.empty() || runtime.user_folder_.empty())
        return std::nullopt;
    try {
        const fs::path root = runtime.user_folder_ / std::string(user_mods_folder_name);
        for (const fs::path& folder : list_mods_in(root)) {
            const package_install::InstalledPackage read =
                package_install::oamod::read_installed_mod(folder);
            if (read.kind == package_install::FolderKind::package && read.id == id)
                return folder;
        }
    } catch (const std::exception&) {
    }
    return std::nullopt;
}

std::optional<fs::path> ReplayContentAccess::played_mod_folder(const Runtime& runtime) {
    if (runtime.options_.layered_mod_folder.empty())
        return std::nullopt;
    return runtime.options_.layered_mod_folder;
}

std::optional<fs::path> ReplayContentAccess::folder_to_play(
    Runtime& runtime,
    ReplayShelf& shelf,
    std::string_view id,
    std::string_view version,
    const std::optional<int64_t>& revision,
    std::string_view sha,
    bool names_release
) {
    // A need that names no release has one folder: the first of its id.
    if (!names_release)
        return installed_mod_folder(runtime, id);
    shelf.have_ready = false;
    const replay_data::HaveContent have = have_content(runtime, shelf);
    for (const replay_data::HaveMod& mod : have.mods) {
        if (mod.id != id || mod.folder.empty())
            continue;
        if (mod.version != version)
            continue;
        if (revision.has_value() && mod.revision.has_value() && *revision != *mod.revision)
            continue;
        if (!sha.empty() && !mod.sha256.empty() && sha != mod.sha256)
            continue;
        return path_from_utf8(mod.folder);
    }
    return std::nullopt;
}

std::optional<fs::path>
index_pass_mod_folder(Runtime& runtime, ReplayShelf& shelf, const replay_data::Replay& replay) {
    // The folder Watch plays: the release the row matched, in Mods or Replay
    // releases. The rows were compared with the cached install, so the pass
    // reads that one too. A map pack's folder is not a mod.
    const replay_data::HaveContent have =
        shelf.have_ready ? shelf.have_cached : ReplayContentAccess::have_content(runtime, shelf);
    bool mod_gap = false;
    for (const replay_data::ContentGap& gap : replay_data::compare_needs(replay, have, {})) {
        if (gap.kind == replay_data::GapKind::map || gap.kind == replay_data::GapKind::engine)
            continue;
        if (gap.play_installed && !gap.play_folder.empty())
            return path_from_utf8(gap.play_folder);
        if (gap.kind == replay_data::GapKind::mod_missing ||
            gap.kind == replay_data::GapKind::mod_other)
            mod_gap = true;
    }
    // Nothing differs from the mod being played, and the replay names a mod:
    // its manifest, a `.tad`'s note, or the Recordings folder of a `.tad`
    // that has neither. Watch plays the folder this run layers.
    bool names_mod = false;
    for (const replay_data::ContentNeed& need : replay.needs) {
        if (need.kind == "oamod")
            names_mod = true;
    }
    const std::string folder_mod = replay_data::folder_mod_id(replay);
    if (!folder_mod.empty() && folder_mod == have.playing_mod_id)
        names_mod = true;
    if (names_mod && !mod_gap) {
        if (std::optional<fs::path> played = ReplayContentAccess::played_mod_folder(runtime))
            return played;
    }
    return ReplayContentAccess::installed_mod_folder(runtime, replay.mod_id);
}

/// The kept map pack Watch would mount for this replay.
///
/// @param runtime the runtime
/// @param shelf the shelf
/// @param replay the replay
/// @return the pack folder; nothing when Watch plays Maps
std::optional<fs::path>
index_pass_map_folder(Runtime& runtime, ReplayShelf& shelf, const replay_data::Replay& replay) {
    const replay_data::HaveContent have =
        shelf.have_ready ? shelf.have_cached : ReplayContentAccess::have_content(runtime, shelf);
    for (const replay_data::ContentGap& gap : replay_data::compare_needs(replay, have, {})) {
        if (gap.kind != replay_data::GapKind::map || !gap.play_installed || gap.play_folder.empty())
            continue;
        return path_from_utf8(gap.play_folder);
    }
    return std::nullopt;
}

std::string index_pass_content_stamp(Runtime& runtime) {
    return ReplayContentAccess::installed_stamp(runtime);
}

replay_data::HaveContent ReplayContentAccess::have_content(Runtime& runtime, ReplayShelf& shelf) {
    const std::string stamp = installed_stamp(runtime);
    if (shelf.have_ready && shelf.have_stamp == stamp)
        return shelf.have_cached;
    // One read of Maps when the stamp changes, including a pack copied after
    // this process started. The stamp's write times are what notice that.
    runtime.map_packs().refresh();
    replay_data::HaveContent have;
    // Recordings name >=0.9.1, the first release that reads a .oademo, whichever
    // build wrote them. This build already reads that format, so a replay is
    // short of an engine only when it needs something newer than both.
    const formats::oamod::EngineVersion running = content::this_engine_rules().engine;
    const std::optional<formats::oamod::EngineVersion> floor =
        formats::oamod::parse_engine_version(oa::session::demo::first_release_reading_replays);
    const formats::oamod::EngineVersion engine =
        floor.has_value() && running < *floor ? *floor : running;
    have.engine_major = engine.major;
    have.engine_minor = engine.minor;
    have.engine_patch = engine.patch;
    if (const oa::data::mod_profile::ModProfile* profile = runtime.mod_profile()) {
        have.playing_mod_id = profile->id;
        have.playing_mod_name = profile->name;
        have.playing_mod_version = profile->version;
    }
    const auto& mods = runtime.engine_settings_state();
    for (std::size_t index = 0; index < mods.mod_folders.size(); ++index) {
        const package_install::InstalledPackage read =
            package_install::oamod::read_installed_mod(path_from_utf8(mods.mod_folders[index]));
        if (read.kind != package_install::FolderKind::package || read.id.empty())
            continue;
        replay_data::HaveMod mod;
        mod.id = read.id;
        mod.name = read.name;
        if (index < mods.mod_names.size() && !mods.mod_names[index].empty())
            mod.name = mods.mod_names[index];
        mod.version = read.version;
        if (read.revision > 0)
            mod.revision = static_cast<int64_t>(read.revision);
        if (read.origin && read.origin->sha256)
            mod.sha256 = digest_hex(*read.origin->sha256);
        mod.folder = mods.mod_folders[index];
        if (mods.playing_mod_folder == mods.mod_folders[index])
            have.playing_mod_sha256 = mod.sha256;
        have.mods.push_back(std::move(mod));
    }
    // Releases a replay fetched, kept beside Mods and Maps. A mod counts as
    // installed when its origin names the folder's digest, so a later replay
    // of that release needs nothing more and plays with this folder. A map
    // pack is read the same way, from its oamap.yaml, and is not a mod.
    std::vector<replay_data::HaveMapPack> kept_packs;
    if (!runtime.user_folder_.empty()) {
        const fs::path releases =
            runtime.user_folder_ / std::string(package_install::replay_releases_folder_name);
        std::error_code walked;
        if (fs::is_directory(releases, walked)) {
            for (fs::directory_iterator cursor(releases, walked), end; !walked && cursor != end;
                 cursor.increment(walked)) {
                std::error_code directory;
                if (!cursor->is_directory(directory) || directory)
                    continue;
                const std::string name = platform::path_to_utf8(cursor->path().filename());
                const std::optional<fs::path> verified =
                    package_install::replay_release_folder(runtime.user_folder_, name);
                if (!verified)
                    continue;
                if (!package_install::installed_pack_manifest(*verified)) {
                    const package_install::InstalledPackage early =
                        package_install::oamod::read_installed_mod(*verified);
                    if (early.kind != package_install::FolderKind::package || early.id.empty())
                        continue;
                }
                const package_install::InstalledPackage read =
                    package_install::oamod::read_installed_mod(*verified);
                if (read.kind != package_install::FolderKind::package || read.id.empty()) {
                    const std::optional<fs::path> manifest_path = entry_without_case(
                        *verified, std::string(oa::data::map_pack::manifest_file)
                    );
                    if (!manifest_path)
                        continue;
                    std::ifstream manifest_in(*manifest_path, std::ios::binary);
                    const std::vector<uint8_t> yaml{
                        std::istreambuf_iterator<char>(manifest_in),
                        std::istreambuf_iterator<char>()
                    };
                    if (!manifest_in && !manifest_in.eof())
                        continue;
                    oa::data::map_pack::Manifest manifest;
                    std::vector<oa::data::map_pack::Problem> problems;
                    if (!oa::data::map_pack::read_manifest(
                            yaml, oa::data::map_pack::ManifestUse::package, manifest, problems
                        ) ||
                        manifest.id.empty())
                        continue;
                    replay_data::HaveMapPack pack;
                    pack.id = manifest.id;
                    pack.name = manifest.name;
                    pack.version = manifest.version;
                    pack.sha256 = name;
                    pack.folder = platform::path_to_utf8(*verified);
                    pack.kept = true;
                    for (const oa::data::map_pack::MapEntry& map : manifest.maps) {
                        const std::string listed =
                            oa::data::map_pack::pack_map_name(map.stem, manifest.id);
                        if (!listed.empty())
                            pack.maps.push_back(listed);
                        if (!map.title.empty())
                            pack.maps.push_back(map.title);
                        if (!map.stem.empty())
                            pack.maps.push_back(map.stem);
                    }
                    kept_packs.push_back(std::move(pack));
                    continue;
                }
                replay_data::HaveMod mod;
                mod.id = read.id;
                mod.name = read.name;
                mod.version = read.version;
                if (read.revision > 0)
                    mod.revision = static_cast<int64_t>(read.revision);
                if (read.origin && read.origin->sha256)
                    mod.sha256 = digest_hex(*read.origin->sha256);
                mod.folder = platform::path_to_utf8(*verified);
                have.mods.push_back(std::move(mod));
            }
        }
    }
    remember_stock_maps(runtime, shelf);

    have.maps = shelf.stock_maps;
    try {
        for (const PackMap& map : runtime.map_packs().maps()) {
            if (!map.name.empty())
                have.maps.push_back(map.name);
            if (!map.title.empty())
                have.maps.push_back(map.title);
            if (!map.stem.empty())
                have.maps.push_back(map.stem);
            // A catalogue copy is the package digest. A hand copy is the
            // folder digest, which is what a recording of that copy stores.
            // The digest of oamap.yaml is neither.
            const std::string folder = platform::path_to_utf8(map.folder);
            replay_data::HaveMapPack* listed = nullptr;
            for (replay_data::HaveMapPack& pack : have.map_packs) {
                if (pack.folder == folder) {
                    listed = &pack;
                    break;
                }
            }
            if (listed == nullptr) {
                replay_data::HaveMapPack pack;
                pack.id = map.id;
                pack.name = map.pack_name;
                pack.version = map.pack_version;
                pack.folder = folder;
                const std::string releases_name(package_install::replay_releases_folder_name);
                pack.kept = folder.find(releases_name) != std::string::npos;
                // The origin's digest whenever the folder has a record with
                // one, including a pack installed from its file. The folder
                // digest is only for a pack with no record. oamap.yaml's
                // digest is neither.
                const std::optional<package_install::Origin> origin =
                    package_install::read_origin(map.folder);
                if (origin && origin->sha256)
                    pack.sha256 = digest_hex(*origin->sha256);
                else
                    pack.sha256 = runtime.folder_digest_text(map.folder);
                have.map_packs.push_back(std::move(pack));
                listed = &have.map_packs.back();
            }
            if (!map.name.empty())
                listed->maps.push_back(map.name);
            if (!map.title.empty())
                listed->maps.push_back(map.title);
            if (!map.stem.empty())
                listed->maps.push_back(map.stem);
        }
    } catch (const std::exception&) {
    }
    for (replay_data::HaveMapPack& pack : kept_packs) {
        bool already = false;
        for (const replay_data::HaveMapPack& listed : have.map_packs) {
            if (listed.folder == pack.folder) {
                already = true;
                break;
            }
        }
        if (already)
            continue;
        have.maps.insert(have.maps.end(), pack.maps.begin(), pack.maps.end());
        have.map_packs.push_back(std::move(pack));
    }
    const fs::path mutators =
        runtime.user_folder_ / std::string(oa::data::mutator_library::mutators_folder_name);
    const oa::data::mutator_library::Listing listing =
        oa::data::mutator_library::list_installed(mutators);
    for (const oa::data::mutator_library::InstalledMutator& item : listing.mutators) {
        if (item.manifest.id.empty())
            continue;
        replay_data::HaveMutator mutator;
        mutator.id = item.manifest.id;
        mutator.sha256 = item.package_sha256;
        have.mutators.push_back(std::move(mutator));
    }
    const oa::data::content_bundles::ContentChoice choice = runtime.content_choice();
    have.expansions_on = choice.expansions;
    have.expansions_have = copied_content_bundles(runtime.options_.game_dir);
    const std::vector<std::string> downloaded = registry_content_bundles(runtime.user_folder_);
    have.expansions_have.insert(have.expansions_have.end(), downloaded.begin(), downloaded.end());
    have.expansions_have.insert(
        have.expansions_have.end(), choice.expansions.begin(), choice.expansions.end()
    );
    shelf.have_cached = have;
    shelf.have_stamp = stamp;
    shelf.have_ready = true;
    return shelf.have_cached;
}

/// Forgets that a press could not reach this registry.
///
/// @param shelf the section's shelf
/// @param id the registry
void clear_registry_away(ReplayShelf& shelf, std::string_view id) {
    for (auto mark = shelf.away.begin(); mark != shelf.away.end(); ++mark) {
        if (mark->id != id)
            continue;
        shelf.away.erase(mark);
        return;
    }
}

/// Remembers that a press could not reach this registry, at its checked time.
///
/// @param shelf the section's shelf
/// @param snapshot the catalogues in effect; null when none is held
/// @param id the registry
void mark_registry_away(ReplayShelf& shelf, const content::Snapshot* snapshot, std::string id) {
    std::optional<int64_t> checked;
    if (snapshot != nullptr) {
        for (const content::RegistryView& registry_view : snapshot->registries) {
            if (registry_view.registry.descriptor.id == id)
                checked = registry_view.checked;
        }
    }
    for (ReplayShelf::RegistryAway& mark : shelf.away) {
        if (mark.id != id)
            continue;
        mark.checked = checked;
        return;
    }
    ReplayShelf::RegistryAway mark;
    mark.id = std::move(id);
    mark.checked = checked;
    shelf.away.push_back(std::move(mark));
}

/// Reports whether a press still cannot treat this registry as reachable.
///
/// A newer checked time means a refresh found a catalogue, and the mark goes.
///
/// @param shelf the section's shelf
/// @param snapshot the catalogues in effect
/// @param id the registry
/// @return true while the mark stands
bool registry_is_away(ReplayShelf& shelf, const content::Snapshot& snapshot, std::string_view id) {
    auto mark = shelf.away.begin();
    for (; mark != shelf.away.end(); ++mark) {
        if (mark->id == id)
            break;
    }
    if (mark == shelf.away.end())
        return false;
    for (const content::RegistryView& registry_view : snapshot.registries) {
        if (registry_view.registry.descriptor.id != id)
            continue;
        if (registry_view.checked.has_value() &&
            (!mark->checked.has_value() || *registry_view.checked > *mark->checked)) {
            shelf.away.erase(mark);
            return false;
        }
        return true;
    }
    return true;
}

namespace {

/// One listed release, filled from a catalogue package or an older release.
replay_data::ListedRelease listed_from(
    std::string sha,
    const content::RegistryView& view,
    data::catalogue::Kind kind,
    std::string id,
    std::string name,
    std::string version,
    std::optional<int64_t> revision,
    std::string file,
    int64_t release,
    uint64_t size
) {
    replay_data::ListedRelease offer;
    offer.sha256 = std::move(sha);
    offer.reach = replay_data::ReleaseReach::listed;
    offer.registry = view.registry.descriptor.id;
    offer.package_kind = data::catalogue::kind_name(kind);
    offer.id = std::move(id);
    offer.name = std::move(name);
    offer.version = std::move(version);
    offer.revision = revision;
    offer.file = std::move(file);
    offer.release = release;
    offer.size = size;
    return offer;
}

/// A registry whose last refresh succeeded, including one past its expires.
bool registry_live(const content::RegistryView& view) {
    return view.catalogue != nullptr && (view.status == content::RegistryStatus::fresh ||
                                         view.status == content::RegistryStatus::out_of_date);
}

/// Indexes one release. A live registry wins over a failed one that still
/// holds a cached catalogue. The first live entry for a digest is kept.
void keep_indexed(
    ReplayShelf::OfferBook& book,
    const content::RegistryView& view,
    replay_data::ListedRelease offer,
    bool expansion
) {
    ReplayShelf::IndexedRelease indexed;
    indexed.sequence = view.catalogue != nullptr ? view.catalogue->sequence : 0;
    const std::string sha = offer.sha256;
    const std::string id = offer.id;
    indexed.offer = std::move(offer);
    if (registry_live(view)) {
        if (!sha.empty())
            book.by_sha.emplace(sha, indexed);
        book.failed_sha.erase(sha);
        if (expansion && !id.empty()) {
            book.expansions.emplace(id, indexed);
            book.failed_expansions.erase(id);
        }
        return;
    }
    if (view.status != content::RegistryStatus::failed)
        return;
    if (!sha.empty() && book.by_sha.find(sha) == book.by_sha.end())
        book.failed_sha.emplace(sha, indexed);
    if (expansion && !id.empty() && book.expansions.find(id) == book.expansions.end())
        book.failed_expansions.emplace(id, indexed);
}

/// Keeps a mod the catalogue offers now. A live registry's comes first; a
/// failed one that still holds a cached catalogue is kept apart.
void keep_mod(
    ReplayShelf::OfferBook& book,
    const content::RegistryView& view,
    const replay_data::ListedRelease& offer
) {
    ReplayShelf::IndexedRelease indexed;
    indexed.sequence = view.catalogue != nullptr ? view.catalogue->sequence : 0;
    indexed.offer = offer;
    if (registry_live(view))
        book.mods.push_back(std::move(indexed));
    else if (view.status == content::RegistryStatus::failed)
        book.failed_mods.push_back(std::move(indexed));
}

/// The mod a need names by id or by name and no release, among releases a
/// catalogue offers: the id first, then the name.
///
/// @param releases the releases
/// @param need the need
/// @return the release, or null
const ReplayShelf::IndexedRelease* mod_named(
    const std::vector<ReplayShelf::IndexedRelease>& releases, const replay_data::ContentNeed& need
) {
    const std::string& key = need.id.empty() ? need.name : need.id;
    for (const ReplayShelf::IndexedRelease& indexed : releases) {
        if (indexed.offer.id == key)
            return &indexed;
    }
    if (!need.id.empty())
        return nullptr;
    for (const ReplayShelf::IndexedRelease& indexed : releases) {
        if (indexed.offer.name == need.name)
            return &indexed;
    }
    return nullptr;
}

/// Builds the digest and expansion maps once for this catalogue generation.
void build_offer_book(ReplayShelf& shelf, const content::Snapshot& snapshot) {
    if (shelf.offers.built && shelf.offers.generation == snapshot.generation)
        return;
    ReplayShelf::OfferBook book;
    book.generation = snapshot.generation;
    book.built = true;
    std::map<std::string, const content::RegistryView*> views;
    for (const content::RegistryView& view : snapshot.registries) {
        views.emplace(view.registry.descriptor.id, &view);
        if (registry_live(view)) {
            if (view.catalogue->releases.has_value() && view.releases == nullptr)
                book.any_pending = true;
            else
                book.any_ready = true;
        }
        if (view.status == content::RegistryStatus::failed)
            book.any_failed = true;
    }
    for (const content::Entry& entry : snapshot.entries) {
        if (entry.package == nullptr)
            continue;
        const auto found = views.find(entry.registry);
        if (found == views.end() || found->second == nullptr)
            continue;
        const data::catalogue::Package& package = *entry.package;
        std::optional<int64_t> revision;
        if (package.revision > 0)
            revision = static_cast<int64_t>(package.revision);
        replay_data::ListedRelease offer = listed_from(
            digest_hex(package.sha256),
            *found->second,
            package.kind,
            package.id,
            package.name,
            package.version,
            revision,
            package.file,
            package.release,
            package.size
        );
        if (package.kind == data::catalogue::Kind::oamod)
            keep_mod(book, *found->second, offer);
        keep_indexed(
            book, *found->second, std::move(offer), package.kind == data::catalogue::Kind::expansion
        );
    }
    for (const content::RegistryView& view : snapshot.registries) {
        if (view.releases == nullptr)
            continue;
        for (const data::catalogue::ReleaseEntry& entry : view.releases->packages) {
            std::optional<int64_t> revision;
            if (entry.revision.has_value())
                revision = static_cast<int64_t>(*entry.revision);
            keep_indexed(
                book,
                view,
                listed_from(
                    digest_hex(entry.sha256),
                    view,
                    entry.kind,
                    entry.id,
                    entry.name,
                    entry.version,
                    revision,
                    entry.file,
                    entry.release,
                    entry.size
                ),
                false
            );
        }
    }
    shelf.offers = std::move(book);
}

/// The reach of a digest no live catalogue names.
replay_data::ReleaseReach unknown_reach(const ReplayShelf::OfferBook& book) {
    if (book.any_ready && !book.any_pending)
        return replay_data::ReleaseReach::unlisted;
    if (!book.any_ready && book.any_failed)
        return replay_data::ReleaseReach::offline;
    return replay_data::ReleaseReach::unknown;
}

/// A listed release, or unlisted when this catalogue sequence still refuses it.
replay_data::ListedRelease
from_index(ReplayShelf& shelf, const ReplayShelf::IndexedRelease& indexed) {
    for (auto refused = shelf.not_offered.begin(); refused != shelf.not_offered.end(); ++refused) {
        if (refused->sha256 != indexed.offer.sha256 || refused->registry != indexed.offer.registry)
            continue;
        if (indexed.sequence <= refused->sequence) {
            replay_data::ListedRelease missing = indexed.offer;
            missing.reach = replay_data::ReleaseReach::unlisted;
            return missing;
        }
        shelf.not_offered.erase(refused);
        break;
    }
    return indexed.offer;
}

/// The releases a replay's digests name, and a reach for each digest no list has.
///
/// The maps are built once per catalogue generation. A registry whose last
/// refresh failed is offline for a package only it still caches. A registry a
/// press could not reach is offline the same way until a refresh or a download
/// from it succeeds. The away mark is applied here, not in the cached book.
std::vector<replay_data::ListedRelease> offers_for(
    ReplayShelf& shelf, const content::Snapshot& snapshot, const replay_data::Replay& replay
) {
    build_offer_book(shelf, snapshot);
    const ReplayShelf::OfferBook& book = shelf.offers;
    std::vector<replay_data::ListedRelease> offers;
    for (const replay_data::ContentNeed& need : replay.needs) {
        if (need.kind == "expansion" && !need.id.empty()) {
            const auto live = book.expansions.find(need.id);
            if (live != book.expansions.end()) {
                if (registry_is_away(shelf, snapshot, live->second.offer.registry)) {
                    replay_data::ListedRelease missing = live->second.offer;
                    missing.reach = replay_data::ReleaseReach::offline;
                    offers.push_back(std::move(missing));
                    continue;
                }
                offers.push_back(from_index(shelf, live->second));
                continue;
            }
            const auto failed = book.failed_expansions.find(need.id);
            if (failed != book.failed_expansions.end()) {
                replay_data::ListedRelease missing = failed->second.offer;
                missing.reach = replay_data::ReleaseReach::offline;
                offers.push_back(std::move(missing));
                continue;
            }
            replay_data::ListedRelease missing;
            missing.id = need.id;
            missing.package_kind = "expansion";
            missing.name = need.name.empty() ? need.id : need.name;
            missing.reach = unknown_reach(book);
            offers.push_back(std::move(missing));
            continue;
        }
        // A `.tad`'s note names a mod by the name the player saw, or its id,
        // and no release: the catalogue's release of that mod is offered.
        if (need.kind == "oamod" && need.sha256.empty() && need.version.empty() &&
            !need.revision.has_value() && (!need.id.empty() || !need.name.empty())) {
            if (const ReplayShelf::IndexedRelease* live = mod_named(book.mods, need)) {
                if (registry_is_away(shelf, snapshot, live->offer.registry)) {
                    replay_data::ListedRelease missing = live->offer;
                    missing.reach = replay_data::ReleaseReach::offline;
                    offers.push_back(std::move(missing));
                    continue;
                }
                offers.push_back(from_index(shelf, *live));
                continue;
            }
            if (const ReplayShelf::IndexedRelease* failed = mod_named(book.failed_mods, need)) {
                replay_data::ListedRelease missing = failed->offer;
                missing.reach = replay_data::ReleaseReach::offline;
                offers.push_back(std::move(missing));
                continue;
            }
            replay_data::ListedRelease missing;
            missing.id = need.id;
            missing.package_kind = "oamod";
            missing.name = need.name.empty() ? need.id : need.name;
            missing.reach = unknown_reach(book);
            offers.push_back(std::move(missing));
            continue;
        }
        if (need.sha256.empty())
            continue;
        bool known = false;
        for (const replay_data::ListedRelease& offer : offers) {
            if (offer.sha256 == need.sha256)
                known = true;
        }
        if (known)
            continue;
        const auto live = book.by_sha.find(need.sha256);
        if (live != book.by_sha.end()) {
            if (registry_is_away(shelf, snapshot, live->second.offer.registry)) {
                replay_data::ListedRelease missing = live->second.offer;
                missing.reach = replay_data::ReleaseReach::offline;
                offers.push_back(std::move(missing));
                continue;
            }
            offers.push_back(from_index(shelf, live->second));
            continue;
        }
        const auto failed = book.failed_sha.find(need.sha256);
        if (failed != book.failed_sha.end()) {
            replay_data::ListedRelease missing = failed->second.offer;
            missing.reach = replay_data::ReleaseReach::offline;
            offers.push_back(std::move(missing));
            continue;
        }
        replay_data::ListedRelease missing;
        missing.sha256 = need.sha256;
        missing.reach = unknown_reach(book);
        offers.push_back(std::move(missing));
    }
    return offers;
}

/// A download of the release a gap names. Never the catalogue's newest.
std::optional<content::DownloadTarget> target_for(
    const content::Snapshot& snapshot, const replay_data::ContentGap& gap, std::string* why
) {
    const content::RegistryView* view = nullptr;
    for (const content::RegistryView& candidate : snapshot.registries) {
        if (candidate.registry.descriptor.id == gap.registry)
            view = &candidate;
    }
    const std::optional<data::catalogue::Kind> kind = catalogue_kind(gap.package_kind);
    const std::optional<oa::base::sha256::Digest> digest = oa::base::sha256::parse_hex(gap.sha256);
    if (view == nullptr || !kind || !digest) {
        if (why != nullptr)
            *why = "the catalogue has no such package";
        return std::nullopt;
    }
    content::DownloadTarget target;
    target.registry = gap.registry;
    target.registry_name = view->registry.descriptor.name;
    target.kind = *kind;
    target.key = gap.id;
    target.name = gap_title(gap);
    target.prompt = true;
    target.release = gap.release;
    target.size = gap.size;
    target.sha256 = *digest;
    target.files = content::reference_urls(*view, gap.file);
    target.mode = view->registry.descriptor.mode;
    target.api = view->registry.descriptor.api;
    target.install_id = view->registry.descriptor.install_id;
    if (target.files.empty()) {
        if (why != nullptr)
            *why = "the catalogue names no address for that file";
        return std::nullopt;
    }
    return target;
}

/// The ids that are copied or downloaded, as views into `copied` and `downloaded`.
std::vector<std::string_view>
bundle_views(const std::vector<std::string>& copied, const std::vector<std::string>& downloaded) {
    std::vector<std::string_view> views;
    views.reserve(copied.size() + downloaded.size());
    for (const std::string& id : copied)
        views.push_back(id);
    for (const std::string& id : downloaded)
        views.push_back(id);
    return views;
}

/// A folder stored the way the mod preference stores one. Empty stays empty.
fs::path normal_folder(const fs::path& folder) {
    if (folder.empty())
        return {};
    std::error_code error;
    fs::path absolute = fs::absolute(folder, error);
    if (error)
        absolute = folder;
    return absolute.lexically_normal();
}

/// True when `left` and `right` are the same folder.
bool same_pack_folder(const fs::path& left, const fs::path& right) {
    std::error_code error;
    if (fs::equivalent(left, right, error))
        return true;
    return normal_folder(left) == normal_folder(right);
}

/// True when the map a replay names is listed from `mount`.
///
/// An empty mount plays Maps. A mount whose manifest lists no map is refused:
/// playback would use the pack of that name in Maps. A name MapPacks does not
/// list, such as a stock map's title, is not that pack.
bool replay_mount_holds_map(
    std::span<const PackMap> maps, const fs::path& mount, const replay_data::Replay* replay
) {
    if (mount.empty())
        return true;
    const std::optional<oa::data::map_pack::Manifest> manifest =
        package_install::installed_pack_manifest(mount);
    if (!manifest || manifest->maps.empty())
        return false;
    if (replay == nullptr)
        return true;
    const auto from_mount = [&](std::string_view label) {
        if (label.empty())
            return true;
        bool seen = false;
        bool held = false;
        for (const PackMap& map : maps) {
            if (map.name != label && map.title != label && map.stem != label)
                continue;
            seen = true;
            if (same_pack_folder(map.folder, mount))
                held = true;
        }
        return !seen || held;
    };
    if (!from_mount(replay->map))
        return false;
    for (const replay_data::ContentNeed& need : replay->needs) {
        if (!from_mount(need.map_name))
            return false;
    }
    return true;
}

/// Expansion ids joined the way the preference stores them.
std::vector<std::string> split_ids(std::string_view text) {
    std::vector<std::string> ids;
    std::size_t start = 0;
    while (start < text.size()) {
        const std::size_t comma = text.find(',', start);
        const std::string_view piece = comma == std::string_view::npos
                                           ? text.substr(start)
                                           : text.substr(start, comma - start);
        if (!piece.empty())
            ids.emplace_back(piece);
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    return ids;
}

/// How far a download has come, as a whole percent. A size of 0 is 0.
int percent_done(uint64_t done, uint64_t size) {
    if (size == 0 || done == 0)
        return 0;
    if (done >= size)
        return 100;
    return static_cast<int>((done % size) * 100 / size);
}

/// "Getting {name}… {percent}%".
std::string getting_sentence(const library::TextHooks& text, std::string_view name, int percent) {
    return library::filled(
        text,
        library::words::replay_getting,
        {{"name", std::string(name)}, {"percent", std::to_string(percent)}}
    );
}

/// The sentence for the package still coming, or the first when every one has ended.
std::string fetch_progress_sentence(
    const library::TextHooks& text,
    const ReplayShelf::PendingGet& pending,
    const std::vector<content::DownloadView>& items
) {
    const content::DownloadView* active = nullptr;
    const content::DownloadView* first = nullptr;
    for (const uint64_t id : pending.items) {
        for (const content::DownloadView& item : items) {
            if (item.item != id)
                continue;
            if (first == nullptr)
                first = &item;
            if (item.state != content::DownloadState::done &&
                item.state != content::DownloadState::failed &&
                item.state != content::DownloadState::cancelled)
                active = &item;
            break;
        }
        if (active != nullptr)
            break;
    }
    const content::DownloadView* used = active != nullptr ? active : first;
    const std::string name = used != nullptr ? used->target.name : std::string{};
    const int percent = used != nullptr ? percent_done(used->done, used->target.size) : 0;
    return getting_sentence(text, name, percent);
}

/// Remembers a digest a registry refused, until a later catalogue sequence.
void remember_refusal(ReplayShelf& shelf, std::string sha, std::string registry, int64_t sequence) {
    for (ReplayShelf::NotOffered& refused : shelf.not_offered) {
        if (refused.sha256 != sha || refused.registry != registry)
            continue;
        if (sequence > refused.sequence)
            refused.sequence = sequence;
        return;
    }
    ReplayShelf::NotOffered refused;
    refused.sha256 = std::move(sha);
    refused.registry = std::move(registry);
    refused.sequence = sequence;
    shelf.not_offered.push_back(std::move(refused));
}

/// The catalogue sentence for one gap. An empty version names none.
std::string
catalogue_sentence(const library::Library& library, const replay_data::ContentGap& gap) {
    const std::string title = gap_title(gap);
    const std::string registry = library::replay_registry_name(library, gap.registry);
    if (gap.version.empty())
        return library::filled(
            library.text,
            library::words::replay_not_in_catalogue_name,
            {{"name", title}, {"registry", registry}}
        );
    return library::filled(
        library.text,
        library::words::replay_not_in_catalogue,
        {{"name", title}, {"version", gap.version}, {"registry", registry}}
    );
}

/// "You're offline." and one sentence for each package the card still names.
std::string
offline_sentences(const library::TextHooks& text, std::span<const replay_data::ContentGap> gaps) {
    std::string said(library::shown(text, library::words::replay_offline));
    for (const replay_data::ContentGap& gap : gaps) {
        if (!shown_gap(gap) || gap.play_installed)
            continue;
        const std::string title = gap_title(gap);
        said.push_back(' ');
        if (gap.version.empty()) {
            said +=
                library::filled(text, library::words::replay_offline_get_name, {{"name", title}});
        } else {
            said += library::filled(
                text,
                library::words::replay_offline_get,
                {{"name", title}, {"version", gap.version}}
            );
        }
    }
    return said;
}

/// The tick named by `open-annihilation.replay-watch-from`. Nothing when the
/// text is empty or is not a whole number of ticks.
///
/// @param text the preference's text
/// @return the tick
std::optional<uint32_t> tick_text(std::string_view text) {
    if (text.empty())
        return std::nullopt;
    uint32_t value = 0;
    for (const char digit : text) {
        if (digit < '0' || digit > '9')
            return std::nullopt;
        const auto piece = static_cast<uint32_t>(digit - '0');
        if (value > 429496729u || (value == 429496729u && piece > 5u))
            return std::nullopt;
        value = value * 10u + piece;
    }
    return value;
}

/// `tick` as decimal text, with no leading zero except for 0 itself.
///
/// @param tick the tick
/// @return the text
std::string tick_decimal(uint32_t tick) {
    char digits[10];
    std::size_t count = 0;
    uint32_t value = tick;
    do {
        digits[count] = static_cast<char>('0' + static_cast<int>(value % 10u));
        ++count;
        value /= 10u;
    } while (value != 0u && count < 10u);
    std::string text(count, '0');
    for (std::size_t index = 0; index < count; ++index)
        text[count - 1u - index] = digits[index];
    return text;
}

/// The replay's path below Recordings, with '/' between folders.
///
/// @param recordings the Recordings folder
/// @param file the replay
/// @return that path; nothing when `file` is not inside the folder
std::optional<std::string> replay_path_below(const fs::path& recordings, const fs::path& file) {
    const fs::path relative = file.lexically_relative(recordings);
    std::string path;
    bool any = false;
    for (const fs::path& part : relative) {
        const std::string name = platform::path_to_utf8(part);
        if (name.empty() || name == "." || name == "..")
            return std::nullopt;
        if (any)
            path.push_back('/');
        path += name;
        any = true;
    }
    if (!any)
        return std::nullopt;
    return path;
}

/// Writes where a replay was left onto one index entry. A missing entry is
/// read from the file, so a later scan keeps the tick. A damaged index is
/// not this function's concern: the caller does not hand one over.
///
/// @param index the index
/// @param folder the Recordings folder
/// @param path the replay, below Recordings
/// @param tick where it was left; nothing when it had finished
/// @param finished the replay had finished, so an earlier tick is cleared
/// @return true when an entry changed
bool store_watched_place(
    replay_data::Index& index,
    const fs::path& folder,
    const std::string& path,
    const std::optional<uint32_t>& tick,
    bool finished
) {
    bool found = false;
    for (replay_data::Replay& replay : index.replays) {
        if (replay.path != path)
            continue;
        found = true;
        if (finished) {
            if (!replay.watched_tick.has_value())
                return false;
            replay.watched_tick.reset();
            return true;
        }
        if (!tick.has_value() || (replay.watched_tick.has_value() && *replay.watched_tick == *tick))
            return false;
        replay.watched_tick = *tick;
        return true;
    }
    if (found || finished || !tick.has_value())
        return false;
    replay_data::Replay replay = replay_data::read_file(folder, path);
    if (!replay.damage.empty())
        return false;
    replay.watched_tick = *tick;
    index.replays.push_back(std::move(replay));
    std::sort(
        index.replays.begin(),
        index.replays.end(),
        [](const replay_data::Replay& earlier, const replay_data::Replay& later) {
            return earlier.path < later.path;
        }
    );
    return true;
}

/// The index's length for the replay the shelf is sampling, when a pass has
/// stored a timeline.
///
/// @param shelf the shelf
/// @param index the index to read
void remember_watch_length(ReplayShelf& shelf, const replay_data::Index& index) {
    for (const replay_data::Replay& replay : index.replays) {
        if (replay.path != shelf.left_path)
            continue;
        if (replay.timeline.has_value() && !replay.timeline->empty()) {
            if (const std::optional<oademo::TimelineIndex> parsed =
                    oademo::parse_timeline_index(*replay.timeline))
                shelf.watch_length = parsed->length_ticks;
        }
        break;
    }
}

/// The recording's own length, when the index has no timeline.
///
/// A replay that needs a package or an expansion is not read by the index
/// pass, so nothing else can say where it ends. An Open Annihilation replay
/// names that length in its description. A sampled tick at or past it is the
/// replay finished, as a tick at or past a timeline's length is.
///
/// @param shelf the shelf
/// @param folder the Recordings folder
void remember_recorded_length(ReplayShelf& shelf, const fs::path& folder) {
    if (shelf.watch_length.has_value() || shelf.left_path.empty())
        return;
    const std::optional<fs::path> file = replay_file(folder, shelf.left_path);
    if (!file.has_value())
        return;
    std::vector<uint8_t> bytes;
    if (!read_replay_bytes(*file, bytes))
        return;
    const oademo::DescriptionResult described = oademo::read_description(bytes);
    if (!described.ok() || !described.meta->game.length_ticks.has_value())
        return;
    const uint64_t length = *described.meta->game.length_ticks;
    if (length == 0 || length > 0xffffffffULL)
        return;
    shelf.watch_length = static_cast<uint32_t>(length);
}

} // namespace

int ReplayContentAccess::switch_for_replay(
    Runtime& runtime,
    const fs::path& file,
    const std::vector<std::string>& ids,
    const fs::path& mod_folder,
    const fs::path& pack_folder,
    std::optional<uint32_t> from_tick
) {
    namespace bundles = oa::data::content_bundles;
    const std::string from_key(replay_watch_from_preference);
    const bool had_from = runtime.preference_values_.erase(from_key) > 0;
    const auto keep_cleared_from = [&] {
        if (!had_from)
            return;
        runtime.preferences_dirty_ = true;
        runtime.flush_preferences();
    };
    const bundles::ContentChoice now = runtime.content_choice();
    const std::vector<std::string> copied = copied_content_bundles(runtime.options_.game_dir);
    const std::vector<std::string> downloaded = registry_content_bundles(runtime.user_folder_);
    const std::vector<std::string_view> present = bundle_views(copied, downloaded);
    std::vector<std::string_view> required;
    const oa::data::mod_profile::ModProfile* profile = runtime.mod_profile();
    const bool mod_active = profile != nullptr;
    if (profile != nullptr) {
        required.reserve(profile->required_expansions.size());
        for (const std::string& id : profile->required_expansions)
            required.push_back(id);
    }
    bundles::ContentChoice working = now;
    bool expansions_changed = false;
    for (const std::string& id : ids) {
        const bundles::ContentChangeResult step = bundles::turn_content_expansion(
            working, id, true, present, false, mod_active, required
        );
        if (step.refusal != bundles::ContentRefusal::none) {
            keep_cleared_from();
            return 0;
        }
        if (step.applied)
            expansions_changed = true;
        working = step.choice;
    }
    // The command line names the mod for this run. The replay plays with it.
    const bool command_mod = !runtime.options_.mod_dir.empty() || runtime.options_.base_game;
    bool mod_changed = false;
    fs::path previous;
    fs::path wanted;
    if (!mod_folder.empty() && !command_mod) {
        previous = chosen_mod_directory(
            runtime.options_.mod_dir, runtime.options_.base_game, runtime.preference_values_
        );
        wanted = normal_folder(mod_folder);
        const fs::path previous_normal = previous.empty() ? fs::path{} : normal_folder(previous);
        mod_changed = wanted != previous_normal;
    }
    if (!expansions_changed && !mod_changed) {
        keep_cleared_from();
        return 1;
    }
    if (expansions_changed) {
        runtime.preference_values_[std::string(replay_expansion_preference)] =
            bundles::expansions_text(now.expansions);
        bundles::ContentChangeResult result;
        result.choice = working;
        result.applied = true;
        if (!store_content_change(runtime.preference_values_, result)) {
            runtime.preference_values_.erase(std::string(replay_expansion_preference));
            return 0;
        }
    }
    if (mod_changed) {
        runtime.preference_values_[std::string(replay_mod_marker_preference)] = "1";
        runtime.preference_values_[std::string(replay_mod_preference)] =
            previous.empty() ? std::string{} : platform::path_to_utf8(normal_folder(previous));
        remember_mod_directory(runtime.preference_values_, wanted);
    }
    runtime.preference_values_[std::string(replay_expansion_state_preference)] = "wait";
    runtime.preference_values_[std::string(oa::data::saves::pending_demo_key)] =
        platform::path_to_utf8(file);
    // The restarted run mounts this pack before the replay's match. An empty
    // path plays Maps, and a folder left from an earlier replay is dropped.
    if (pack_folder.empty())
        runtime.preference_values_.erase(std::string(oa::data::saves::replay_map_pack_key));
    else
        runtime.preference_values_[std::string(oa::data::saves::replay_map_pack_key)] =
            platform::path_to_utf8(pack_folder);
    if (from_tick.has_value())
        runtime.preference_values_[from_key] = tick_decimal(*from_tick);
    runtime.preferences_dirty_ = true;
    runtime.flush_preferences();
    runtime.request_soft_restart();
    return 2;
}

bool ReplayContentAccess::restore_replay_expansions(Runtime& runtime) {
    namespace bundles = oa::data::content_bundles;
    const auto state =
        runtime.preference_values_.find(std::string(replay_expansion_state_preference));
    if (state == runtime.preference_values_.end())
        return false;
    if (state->second != "wait" && state->second != "play")
        return false;
    // A restart that is about to play the replay, or one whose pending demo
    // is still waiting, keeps the switch. A `wait` left with no demo is a
    // start that ended before the replay began: the player's set comes back.
    if (state->second == "wait") {
        if (runtime.soft_restart_requested() || !runtime.pending_demo_path().empty())
            return false;
        const auto pending =
            runtime.preference_values_.find(std::string(oa::data::saves::pending_demo_key));
        if (pending != runtime.preference_values_.end() && !pending->second.empty())
            return false;
    }
    if (runtime.screen_ == Screen::match || runtime.match_ != nullptr)
        return false;
    // The end screen stays until the player leaves it. A later start is not
    // on that screen, so a replay that never reached it still puts the choice
    // back, and does not play.
    if (runtime.screen_ == Screen::campaign_end)
        return false;

    const auto saved_expansions =
        runtime.preference_values_.find(std::string(replay_expansion_preference));
    const bool have_expansions = saved_expansions != runtime.preference_values_.end();
    bool expansions_differ = false;
    bundles::ContentChoice next = runtime.content_choice();
    if (have_expansions) {
        next.expansions = split_ids(saved_expansions->second);
        expansions_differ = next.expansions != runtime.content_choice().expansions;
    }
    const auto marker = runtime.preference_values_.find(std::string(replay_mod_marker_preference));
    const bool have_mod = marker != runtime.preference_values_.end();
    bool mod_differ = false;
    fs::path saved_mod;
    if (have_mod) {
        const auto saved = runtime.preference_values_.find(std::string(replay_mod_preference));
        const std::string text =
            saved == runtime.preference_values_.end() ? std::string{} : saved->second;
        saved_mod = text.empty() ? fs::path{} : platform::path_from_utf8(text);
        const fs::path current = chosen_mod_directory({}, false, runtime.preference_values_);
        const fs::path saved_normal = saved_mod.empty() ? fs::path{} : normal_folder(saved_mod);
        const fs::path current_normal = current.empty() ? fs::path{} : normal_folder(current);
        mod_differ = saved_normal != current_normal;
    }
    if (expansions_differ) {
        bundles::ContentChangeResult result;
        result.choice = std::move(next);
        result.applied = true;
        if (!store_content_change(runtime.preference_values_, result)) {
            runtime.preferences_dirty_ = true;
            runtime.flush_preferences();
            return false;
        }
    }
    if (have_mod && mod_differ)
        remember_mod_directory(runtime.preference_values_, saved_mod);
    // The command line names the mod for this run, so putting the preference
    // back does not change what this run plays.
    if (!runtime.options_.mod_dir.empty() || runtime.options_.base_game)
        mod_differ = false;
    if (have_expansions)
        runtime.preference_values_.erase(std::string(replay_expansion_preference));
    runtime.preference_values_.erase(std::string(replay_expansion_state_preference));
    if (have_mod) {
        runtime.preference_values_.erase(std::string(replay_mod_marker_preference));
        runtime.preference_values_.erase(std::string(replay_mod_preference));
    }
    // The pending replay and the pack it would have mounted are done, including
    // a start that ended before that replay.
    runtime.preference_values_.erase(std::string(oa::data::saves::pending_demo_key));
    runtime.preference_values_.erase(std::string(oa::data::saves::replay_map_pack_key));
    runtime.preference_values_.erase(std::string(replay_watch_from_preference));
    runtime.preferences_dirty_ = true;
    runtime.flush_preferences();
    if (!expansions_differ && !mod_differ)
        return false;
    runtime.request_soft_restart();
    return true;
}

void Runtime::begin_replay_content_playback() {
    const auto state = preference_values_.find(std::string(replay_expansion_state_preference));
    if (state == preference_values_.end() || state->second != "wait")
        return;
    preference_values_[std::string(replay_expansion_state_preference)] = "play";
    preferences_dirty_ = true;
    flush_preferences();
}

void Runtime::settle_replay_content() {
    static_cast<void>(ReplayContentAccess::restore_replay_expansions(*this));
}

void Runtime::LibraryHost::begin_export(Runtime& runtime, std::vector<std::string> paths) {
    ReplayShelf& shelf = replay_shelf();
    reset_export_job(shelf.job);
    shelf.held.clear();
    shelf.export_rebuild_asked = false;
    shelf.export_byte_done.store(0);
    shelf.export_byte_total.store(0);
    shelf.export_compressing.store(false);
    shelf.export_progress_seen = shelf.export_progress_stamp.load();
    const DownloadsFolder downloads = downloads_folder(runtime.user_folder_);
    library::ExportOffer offer;
    if (!downloads.path.empty()) {
        offer.folder = platform::path_to_utf8(downloads.path);
        offer.folder_is_downloads = downloads.system;
        offer.folder_label = export_folder_label(downloads.path, downloads.system);
    }
    const fs::path recordings = runtime.user_folder_ / std::string(recordings_folder_name);
    ReplayShelf::ExportAsk ask;
    ask.recordings = recordings;
    for (const std::string& path : paths) {
        library::ExportFile shown;
        shown.path = path;
        const replay_data::Replay* row = shown_replay(shelf, path);
        if (row != nullptr && !row->map.empty())
            shown.title = row->map;
        else
            shown.title = export_title_from_path(path);
        ask.paths.push_back(path);
        ask.titles.push_back(shown.title);
        ask.timelines.push_back(indexed_timeline(shelf, path));
        offer.files.push_back(std::move(shown));
    }
    if (offer.folder.empty() && !paths.empty())
        offer.problem = export_failed_sentence(model);
    // The dialog opens before anything is read. A `.oademo` is the choice
    // until the player says otherwise, which the worker reads each file.
    shelf.export_want_oademo.store(true);
    shelf.export_want_sectors.store(true);
    library::open_export(model, std::move(offer));
    ++model_revision;
    if (paths.empty() || model.export_dialog.folder.empty())
        return;
    if (!post_export(shelf, std::move(ask))) {
        replays_log("the export's worker could not start");
        library::fail_export(model, export_failed_sentence(model));
        ++model_revision;
    }
}

bool Runtime::LibraryHost::step_replay_export(Runtime& runtime) {
    if (!replays)
        return false;
    ReplayShelf& shelf = *replays;
    library::ExportDialog& dialog = model.export_dialog;
    if (!dialog.open) {
        const bool busy = shelf.job.active || shelf.job.opened || !shelf.held.empty() ||
                          shelf.export_busy.load() || shelf.export_rebuild_asked;
        if (busy) {
            {
                base::threads::LockGuard guard(shelf.export_mutex);
                shelf.export_cancel.store(true);
                shelf.export_pending = false;
                shelf.export_result.reset();
            }
            reset_export_job(shelf.job);
            shelf.held.clear();
            shelf.export_rebuild_asked = false;
            shelf.export_byte_done.store(0);
            shelf.export_byte_total.store(0);
            shelf.export_compressing.store(false);
            shelf.export_progress_seen = shelf.export_progress_stamp.load();
        }
        return false;
    }
    // The worker reads these just before it keeps a file. A choice made while
    // a measurement is running is the one that measurement keeps.
    shelf.export_want_oademo.store(dialog.format == library::ExportFormat::oademo);
    shelf.export_want_sectors.store(dialog.keep_oa_sectors);
    bool changed = false;
    const uint64_t stamp = shelf.export_progress_stamp.load();
    if (stamp != shelf.export_progress_seen) {
        const uint64_t done = shelf.export_byte_done.load();
        const uint64_t total = shelf.export_byte_total.load();
        const bool compressing = shelf.export_compressing.load();
        shelf.export_progress_seen = stamp;
        if (dialog.progress_bytes_done != done || dialog.progress_bytes_total != total ||
            dialog.compressing != compressing) {
            library::set_export_byte_progress(model, done, total, compressing);
            ++model_revision;
            changed = true;
        }
    }
    if (shelf.export_rebuild_asked && !shelf.export_busy.load()) {
        bool pending = false;
        bool published = false;
        {
            base::threads::LockGuard guard(shelf.export_mutex);
            pending = shelf.export_pending;
            published = shelf.export_result.has_value();
        }
        if (!pending && !published) {
            shelf.export_rebuild_asked = false;
            reset_export_job(shelf.job);
            replays_log("the export's measurement stopped before it finished");
            library::fail_export(model, export_failed_sentence(model));
            ++model_revision;
            return true;
        }
    }
    std::optional<ReplayShelf::ExportResult> result;
    {
        base::threads::LockGuard guard(shelf.export_mutex);
        if (shelf.export_result &&
            shelf.export_result->generation == shelf.export_generation.load())
            result = std::move(*shelf.export_result);
        shelf.export_result.reset();
    }
    if (result) {
        if (result->rebuild) {
            for (ReplayShelf::HeldExport& published : result->held) {
                if (published.unreadable && !published.why.empty())
                    replays_log("cannot export " + published.title + ": " + published.why);
                for (std::size_t place = 0; place < shelf.held.size(); ++place) {
                    ReplayShelf::HeldExport& have = shelf.held[place];
                    if (have.path != published.path || have.written)
                        continue;
                    if (shelf.job.opened && place == shelf.job.piece)
                        continue;
                    have.bytes = std::move(published.bytes);
                    have.held_oademo = published.held_oademo;
                    have.held_sectors = published.held_sectors;
                    if (!published.timeline.empty())
                        have.timeline = std::move(published.timeline);
                    have.unreadable = published.unreadable;
                    have.why = std::move(published.why);
                    have.ready = published.ready;
                }
                for (library::ExportFile& file : dialog.files) {
                    if (file.path != published.path)
                        continue;
                    file.ready = published.ready;
                    file.unreadable = published.unreadable;
                }
            }
            shelf.export_rebuild_asked = false;
        } else {
            shelf.held = std::move(result->held);
            for (const ReplayShelf::HeldExport& held : shelf.held) {
                if (held.unreadable && !held.why.empty())
                    replays_log("cannot export " + held.title + ": " + held.why);
                for (library::ExportFile& file : dialog.files) {
                    if (file.path != held.path)
                        continue;
                    file.oademo_bytes = held.oademo_bytes;
                    file.tad_bytes = held.tad_bytes;
                    file.tad_without_bytes = held.tad_without_bytes;
                    file.names_cut = held.names_cut;
                    file.status_cut = held.status_cut;
                    file.wide_records = held.wide_records;
                    file.mutators = held.mutators;
                    file.mutator_ids = held.mutator_ids;
                    file.mutator_names = held.mutator_names;
                    file.ready = held.ready;
                    file.unreadable = held.unreadable;
                }
            }
        }
        shelf.export_byte_done.store(0);
        shelf.export_byte_total.store(0);
        shelf.export_compressing.store(false);
        shelf.export_progress_seen = shelf.export_progress_stamp.load();
        library::set_export_byte_progress(model, 0, 0, false);
        ++model_revision;
        changed = true;
    }
    if (!shelf.job.active)
        return changed;
    // Two pumps pass before the first byte, so Cancel on the press after
    // Export still finds nothing written.
    if (shelf.job.hold > 0) {
        --shelf.job.hold;
        return changed;
    }
    if (!shelf.job.opened) {
        const std::size_t place = next_unwritten(shelf.held);
        if (place >= shelf.held.size()) {
            reset_export_job(shelf.job);
            library::finish_export(model);
            ++model_revision;
            return true;
        }
        ReplayShelf::HeldExport& held = shelf.held[place];
        if (held.unreadable) {
            reset_export_job(shelf.job);
            dialog.exporting = false;
            ++model_revision;
            return true;
        }
        if (!held_matches(held, shelf.job.as_oademo, shelf.job.sectors)) {
            if (!shelf.export_rebuild_asked && !shelf.export_busy.load()) {
                ReplayShelf::ExportAsk ask;
                ask.rebuild = true;
                ask.recordings = runtime.user_folder_ / std::string(recordings_folder_name);
                for (const ReplayShelf::HeldExport& one : shelf.held) {
                    if (one.written || one.unreadable)
                        continue;
                    if (held_matches(one, shelf.job.as_oademo, shelf.job.sectors))
                        continue;
                    ask.paths.push_back(one.path);
                    ask.titles.push_back(one.title);
                    std::string timeline = indexed_timeline(shelf, one.path);
                    if (timeline.empty())
                        timeline = one.timeline;
                    ask.timelines.push_back(std::move(timeline));
                }
                if (ask.paths.empty()) {
                    reset_export_job(shelf.job);
                    library::fail_export(model, export_failed_sentence(model));
                    ++model_revision;
                    return true;
                }
                shelf.export_rebuild_asked = true;
                if (!post_export(shelf, std::move(ask))) {
                    shelf.export_rebuild_asked = false;
                    reset_export_job(shelf.job);
                    replays_log("the export's worker could not start");
                    library::fail_export(model, export_failed_sentence(model));
                    ++model_revision;
                    return true;
                }
            }
            return changed;
        }
        const fs::path destination =
            export_destination(shelf.job.folder, export_file_stem(held.title), shelf.job.extension);
        if (destination.empty()) {
            const uint32_t done = exports_written(shelf.held);
            const uint32_t total = shelf.job.files_total;
            replays_log("cannot export " + held.title + ": no free name");
            reset_export_job(shelf.job);
            library::fail_export(
                model,
                export_problem_sentence(
                    model, std::make_error_code(std::errc::io_error), done, total
                )
            );
            ++model_revision;
            return true;
        }
        shelf.job.destination = destination;
        shelf.job.piece = place;
        shelf.job.offset = 0;
    }
    const std::size_t place = shelf.job.piece;
    if (place >= shelf.held.size()) {
        reset_export_job(shelf.job);
        library::fail_export(model, export_failed_sentence(model));
        ++model_revision;
        return true;
    }
    const std::string title = shelf.held[place].title;
    const std::error_code failed = write_export_chunk(shelf.job, shelf.held[place].bytes);
    if (failed) {
        const uint32_t done = exports_written(shelf.held);
        const uint32_t total = shelf.job.files_total;
        replays_log("cannot export " + title + ": " + failed.message());
        reset_export_job(shelf.job);
        library::fail_export(model, export_problem_sentence(model, failed, done, total));
        ++model_revision;
        return true;
    }
    if (shelf.job.offset < shelf.held[place].bytes.size()) {
        library::set_export_byte_progress(
            model, shelf.job.offset, shelf.held[place].bytes.size(), false
        );
        ++model_revision;
        return true;
    }
    shelf.held[place].written = true;
    shelf.held[place].bytes.clear();
    shelf.held[place].bytes.shrink_to_fit();
    shelf.job.offset = 0;
    // The exported file is in place. Mark the recording it came from, so the
    // keep rule leaves it. The card reads the mark on this frame. A damaged
    // marks file was set aside when it was loaded, and is not written over.
    if (!runtime.user_folder_.empty()) {
        const fs::path recordings = runtime.user_folder_ / std::string(recordings_folder_name);
        if (mark_exported_replay(shelf, recordings, shelf.held[place].path)) {
            fill_replay_inputs(runtime, model.inputs);
            library::update_replays(model);
        }
    }
    const uint32_t done = exports_written(shelf.held);
    const uint32_t total = shelf.job.files_total;
    library::set_export_progress(model, done, total);
    library::set_export_byte_progress(model, 0, 0, false);
    bool all_written = !shelf.held.empty();
    for (const ReplayShelf::HeldExport& one : shelf.held)
        all_written = all_written && one.written;
    if (all_written) {
        reset_export_job(shelf.job);
        library::finish_export(model);
    }
    ++model_revision;
    return true;
}

void Runtime::LibraryHost::open_export_listing(Runtime& runtime, const fs::path& folder) {
    fs::path listed = folder;
    if (!is_folder(listed))
        listed = downloads_folder(runtime.user_folder_).path;
    if (!is_folder(listed)) {
        library::open_export_browse(model, platform::path_to_utf8(listed), {});
        ++model_revision;
        return;
    }
    list_export_folder(model, model_revision, listed);
}

void Runtime::LibraryHost::start_export_job() {
    library::ExportDialog& dialog = model.export_dialog;
    if (!library::export_can_run(dialog))
        return;
    ReplayShelf& shelf = replay_shelf();
    reset_export_job(shelf.job);
    const bool as_oademo = dialog.format == library::ExportFormat::oademo;
    shelf.export_want_oademo.store(as_oademo);
    shelf.export_want_sectors.store(dialog.keep_oa_sectors);
    shelf.job.folder = platform::path_from_utf8(dialog.folder);
    shelf.job.as_oademo = as_oademo;
    shelf.job.sectors = dialog.keep_oa_sectors;
    shelf.job.extension = as_oademo ? std::string(".oademo") : std::string(".tad");
    shelf.job.files_total = static_cast<uint32_t>(dialog.files.size());
    shelf.job.files_done = exports_written(shelf.held);
    shelf.job.hold = export_hold_frames;
    shelf.job.active = true;
    library::set_export_progress(model, shelf.job.files_done, shelf.job.files_total);
    // One file shows the bar at once, including the two pumps before the
    // first byte. The bytes stay where the measurement left them.
    if (dialog.files.size() == 1) {
        for (const ReplayShelf::HeldExport& held : shelf.held) {
            if (held.path != dialog.files.front().path)
                continue;
            if (held_matches(held, as_oademo, dialog.keep_oa_sectors))
                library::set_export_byte_progress(model, 0, held.bytes.size(), false);
            break;
        }
    }
    ++model_revision;
}

bool Runtime::LibraryHost::read_game_chunk(
    Runtime& runtime, const char* path, uint32_t offset, std::span<uint8_t> out
) {
    try {
        return runtime.assets_.read_chunk(path, offset, out);
    } catch (const std::exception&) {
        return false;
    }
}

bool Runtime::LibraryHost::replay_jump_waiting(const Runtime& runtime) {
    if (replay_shelf().pending_seek.has_value())
        return true;
    return runtime.preference_values_.contains(std::string(replay_watch_from_preference));
}

ReplayShelf& Runtime::LibraryHost::replay_shelf() {
    if (!replays)
        replays.reset(new ReplayShelf());
    return *replays;
}

void Runtime::LibraryHost::scan_replays(Runtime& runtime) {
    if (runtime.user_folder_.empty())
        return;
    ReplayShelf& shelf = replay_shelf();
    const fs::path folder = runtime.user_folder_ / std::string(recordings_folder_name);
    load_kept_marks(shelf, folder);
    load_exported_marks(shelf, folder);
    {
        base::threads::LockGuard guard(shelf.mutex);
        shelf.recordings = folder;
        shelf.from = shelf.index;
        shelf.due = true;
        if (!start_replay_worker(shelf)) {
            shelf.due = false;
            return;
        }
    }
    shelf.wake.notify_all();
    // The section says that it is reading from the moment the Library opens,
    // not once the worker has begun: no "No replays yet." while the folder is read.
    shelf.reading = true;
    if (replays_held)
        return;
    fill_replay_inputs(runtime, model.inputs);
    library::update_replays(model);
}

void Runtime::LibraryHost::land_opened_replay() {
    if (!replays || replays->land_path.empty() || model.inputs.replays.reading)
        return;
    const std::string path = replays->land_path;
    const library::ReplayRow* row = nullptr;
    for (const library::ReplayRow& candidate : model.inputs.replays.rows)
        if (candidate.replay.path == path)
            row = &candidate;
    replays->land_path.clear();
    if (row == nullptr)
        return;
    model.replays.kind.clear();
    library::set_query(model, {});
    const std::string imported = std::string(replay_data::imported_folder_name) + "/";
    if (path.starts_with(imported))
        library::set_replay_filter(model, library::ReplayFilter::imported);
    else if (row->day > model.inputs.replays.today - library::month_days)
        library::set_replay_filter(model, library::ReplayFilter::this_month);
    else
        library::set_replay_filter(model, library::ReplayFilter::older);
    library::select_replay(model, path);
    if (model.replays.selected != path) {
        library::set_replay_filter(model, library::ReplayFilter::older);
        library::select_replay(model, path);
    }
}

bool Runtime::LibraryHost::pump_replays(Runtime& runtime) {
    const bool wrote = step_replay_export(runtime);
    if (!replays)
        return wrote;
    ReplayShelf& shelf = *replays;
    bool moved = false;
    std::optional<replay_data::Index> ended;
    {
        base::threads::LockGuard guard(shelf.mutex);
        moved = shelf.moves != shelf.moves_seen;
        if (moved) {
            shelf.moves_seen = shelf.moves;
            if (shelf.result) {
                ended = std::move(shelf.result);
                shelf.result.reset();
            }
            shelf.reading = shelf.scanning || shelf.due;
            shelf.read = shelf.done;
            shelf.to_read = shelf.total;
            // Until a read has ended once, the rows show as they are read.
            if (!shelf.listed)
                shelf.rows_read = shelf.partial;
        }
    }
    if (moved) {
        if (!shelf.gone.empty()) {
            const auto trashed = [&](const replay_data::Replay& replay) {
                return std::find(shelf.gone.begin(), shelf.gone.end(), replay.path) !=
                       shelf.gone.end();
            };
            std::erase_if(shelf.rows_read, trashed);
            if (ended)
                std::erase_if(ended->replays, trashed);
        }
        if (ended) {
            // A timeline or a watched tick written while this read ran stays
            // on a file the read did not open again.
            keep_passed_index(*ended, shelf.index);
            shelf.index = std::move(*ended);
            shelf.listed = true;
            shelf.rows_read.clear();
            if (!runtime.user_folder_.empty()) {
                const fs::path folder = runtime.user_folder_ / std::string(recordings_folder_name);
                // A kept replay that moved or was renamed keeps its mark; the mark's
                // path follows it.
                if (!shelf.kept_damaged &&
                    replay_data::apply_kept(shelf.kept, shelf.index.replays) &&
                    !replay_data::save_kept(folder, shelf.kept))
                    replays_log("the kept replays could not be saved");
                if (!shelf.exported_damaged &&
                    replay_data::apply_exported(shelf.exported, shelf.index.replays) &&
                    !replay_data::save_exported(folder, shelf.exported))
                    replays_log("the exported replays could not be saved");
                std::error_code missing;
                // An empty folder that was never made keeps no index; any other is saved.
                if ((!shelf.index.replays.empty() ||
                     fs::exists(replay_data::index_path(folder), missing)) &&
                    !replay_data::save_index(folder, shelf.index))
                    replays_log("the replays' index could not be saved");
            }
        }
    }
    // A check's rows stay. The shelf has still taken the worker's result, and
    // no index pass is polled under the rows the check put in.
    if (replays_held)
        return moved ? false : wrote;
    // The pass is polled every frame, including one the folder's read did
    // not move. It does not run during a match or while the Library is closed.
    // A change in what is installed is filled first, so a row that was waiting
    // on a package is read once that package is there.
    const bool library_open = runtime.oa_layer().find(library_screen_name) != nullptr;
    const bool in_match = runtime.screen_ == Screen::match || runtime.match_ != nullptr;
    bool refresh = false;
    if (shelf.listed && !in_match && library_open) {
        const std::string stamp = index_pass_content_stamp(runtime);
        refresh = !shelf.have_ready || shelf.have_stamp != stamp;
    }
    if ((moved || refresh) && !replays_held) {
        fill_replay_inputs(runtime, model.inputs);
        library::update_replays(model);
    }
    const bool landed = advance_replay_index(
        shelf,
        model,
        library_open,
        in_match,
        runtime.user_folder_,
        runtime.options_.game_dir,
        runtime
    );
    if (!moved && !refresh && !landed) {
        // A replay opened from outside is selected once the folder's read
        // has ended and shows it.
        if (shelf.land_path.empty() || shelf.reading)
            return wrote;
        land_opened_replay();
        ++model_revision;
        return true;
    }
    if (landed || !moved) {
        fill_replay_inputs(runtime, model.inputs);
        library::update_replays(model);
    }
    land_opened_replay();
    ++model_revision;
    return true;
}

void Runtime::LibraryHost::select_kept_replay() {
    if (!kept_replay || !replays || !replays->listed)
        return;
    const std::string path = *kept_replay;
    if (!model.replays.selected || *model.replays.selected != path)
        library::select_replay(model, path);
    // The folder can finish before this replay is in the list the filter
    // shows. The next read tries again. Opening the Library forgets it.
    if (!model.replays.selected || *model.replays.selected != path)
        return;
    if (model.tab == library::Tab::replays) {
        state.list_scroll = kept_replay_scroll;
        if (const std::optional<std::size_t> place = library::replay_place(model, path))
            state.reveal_row = static_cast<int32_t>(*place);
        state.replay_page =
            kept_replay_page && model.replays.selected && *model.replays.selected == path;
    }
    const bool share = share_kept_replay;
    kept_replay.reset();
    if (share) {
        share_kept_replay = false;
        if (signed_handle().empty()) {
            library::open_sign_in(model);
            if (model.sign_in.open)
                share_after_sign_in = path;
            else
                share_after_sign_in.reset();
        } else {
            share_after_sign_in.reset();
            const library::ReplayRow* row = library::find_replay(model, path);
            const std::string title = row == nullptr ? std::string{} : row->replay.map;
            library::open_share_question(model, path, title);
        }
    }
    ++model_revision;
}

bool Runtime::LibraryHost::replays_reading() {
    if (!replays)
        return false;
    base::threads::LockGuard guard(replays->mutex);
    return replays->scanning || replays->due || replays->result.has_value() ||
           replays->moves != replays->moves_seen;
}

bool Runtime::LibraryHost::replays_watching() const {
    return replays && replays->watching.has_value();
}

void Runtime::LibraryHost::fill_replay_inputs(Runtime& runtime, library::Inputs& inputs) {
    // A check's rows stay, whichever caller is filling: a rebuild, a picture,
    // a fetch, or a catalogue. Copying them keeps a fresh inputs object from
    // replacing the model with the folder.
    if (replays_held) {
        if (&inputs != &model.inputs)
            inputs.replays = model.inputs.replays;
        return;
    }
    library::ReplayInputs& shown = inputs.replays;
    shown = {};
    shown.kinds = replay_data::registered_kinds();
    shown.today = local_when(static_cast<int64_t>(std::time(nullptr))).day;
    shown.recording_on = replays_record_setting(runtime.preference_values_);
    const replay_data::KeepSpan span = replay_data::keep_span_of(
        preference_text(runtime.preference_values_, replays_keep_preference)
    );
    shown.keep_forever = span == replay_data::KeepSpan::forever;
    if (const std::optional<int32_t> days = replay_data::keep_span_days(span))
        shown.keep_days = *days;
    if (!replays) {
        publish_core_prime(shown);
        publish_account(shown);
        return;
    }
    ReplayShelf& shelf = *replays;
    shown.reading = shelf.reading;
    shown.read = shelf.read;
    shown.total = shelf.to_read;
    const std::vector<replay_data::Replay>& source =
        shelf.listed ? shelf.index.replays : shelf.rows_read;
    const std::shared_ptr<const content::Snapshot> snapshot =
        source.empty() ? nullptr : runtime.content_service().snapshot();
    shelf.content_generation = snapshot ? snapshot->generation : 0;
    const std::optional<replay_data::HaveContent> have =
        source.empty() ? std::nullopt
                       : std::optional<replay_data::HaveContent>{
                             ReplayContentAccess::have_content(runtime, shelf)
                         };
    shown.rows.reserve(source.size());
    std::size_t index_done = 0;
    std::size_t index_total = 0;
    for (const replay_data::Replay& replay : source) {
        library::ReplayRow row;
        row.replay = replay;
        row.replay.kept = replay_data::is_kept(shelf.kept, replay);
        // The marks are what an export wrote. A row's own flag is set when a
        // read ends, so a replay exported while the list is open would not
        // say so until the next read.
        row.replay.exported = replay_data::is_kept(shelf.exported, replay);
        const LocalWhen when = local_when(replay.recorded);
        row.day = when.day;
        row.minute = when.minute;
        if (const auto picture = shelf.pictures.find(replay.map); picture != shelf.pictures.end())
            row.picture = picture->second;
        apply_timeline(row);
        if (have) {
            const std::vector<replay_data::ListedRelease> offers =
                snapshot != nullptr ? offers_for(shelf, *snapshot, replay)
                                    : std::vector<replay_data::ListedRelease>{};
            row.gaps = replay_data::compare_needs(replay, *have, offers);
        }
        // A row Watch would have to fetch, turn on or refuse is not read, shows
        // no Reading… chip and is not part of the footer's count. A refusal at
        // the current stamp counts as read, so the footer is not stuck. A
        // timeline with no `events` member waits for its new read like a row
        // with none, and keeps its result and marks while it waits.
        const bool blocked = index_pass_blocked(row.gaps);
        const bool read =
            replay.timeline.has_value() && !oademo::timeline_wants_events(*replay.timeline);
        const bool refused =
            !read && replay.read_stamp.has_value() && *replay.read_stamp == shelf.have_stamp;
        if (blocked || refused)
            row.indexing = false;
        if (!replay_data::damaged(replay) && !blocked) {
            ++index_total;
            if (read || refused)
                ++index_done;
        }
        shown.rows.push_back(std::move(row));
    }
    shown.index_done = index_done;
    shown.index_total = index_total;
    shown.indexing = replay_index_pass_available && index_done < index_total;
    if (shelf.pending_get) {
        shown.fetching = true;
        shown.fetch_path = shelf.pending_get->path;
        shown.fetch_sentence = shelf.fetch_sentence;
    }
    publish_core_prime(shown);
    publish_account(shown);
}

std::vector<replay_data::ContentGap>
Runtime::LibraryHost::replay_gaps(Runtime& runtime, const replay_data::Replay& replay) {
    ReplayShelf& shelf = replay_shelf();
    const std::shared_ptr<const content::Snapshot> snapshot = runtime.content_service().snapshot();
    const replay_data::HaveContent have = ReplayContentAccess::have_content(runtime, shelf);
    const std::vector<replay_data::ListedRelease> offers =
        snapshot != nullptr ? offers_for(shelf, *snapshot, replay)
                            : std::vector<replay_data::ListedRelease>{};
    return replay_data::compare_needs(replay, have, offers);
}

Runtime::LibraryHost::ReplayFetch Runtime::LibraryHost::queue_replay_get(
    Runtime& runtime,
    std::string relative,
    const fs::path& file,
    const replay_data::Replay& replay,
    std::optional<uint32_t> from
) {
    // A replay the feed says needs nothing can be watched without asking a
    // registry. Asking would start the content service. It plays Maps, so a
    // pack mounted for an earlier replay goes.
    if (replay.needs.empty() && replay.engine_needs.empty()) {
        runtime.use_replay_map_pack({});
        return ReplayFetch::watch;
    }
    bool engine = false;
    bool other = false;
    bool blocked = false;
    bool map_pack = false;
    std::string mod_id;
    std::string play_folder;
    std::string pack_folder;
    bool mod_names_release = false;
    std::string mod_version;
    std::string mod_sha;
    std::optional<int64_t> mod_revision;
    bool matched_release = false;
    std::vector<const replay_data::ContentGap*> fetch;
    std::vector<const replay_data::ContentGap*> beside;
    std::vector<std::string> turn_on;
    const std::vector<replay_data::ContentGap> gaps = replay_gaps(runtime, replay);
    for (const replay_data::ContentGap& gap : gaps) {
        if (!shown_gap(gap))
            continue;
        if (gap.kind == replay_data::GapKind::engine)
            engine = true;
        if (gap.other_release)
            other = true;
        if (gap.block != replay_data::GapBlock::none)
            blocked = true;
        if (gap.fetchable && !gap.other_release)
            fetch.push_back(&gap);
        if ((gap.kind == replay_data::GapKind::mod_other ||
             gap.kind == replay_data::GapKind::mod_missing ||
             gap.kind == replay_data::GapKind::map) &&
            gap.other_release && gap.block == replay_data::GapBlock::none && !gap.sha256.empty() &&
            !gap.file.empty() && !gap.registry.empty())
            beside.push_back(&gap);
        if (gap.kind == replay_data::GapKind::map && gap.fetchable)
            map_pack = true;
        if (gap.play_installed) {
            // The folder compare_needs matched. Not the first of the id.
            // A map pack is mounted for this replay. It is not a mod, and
            // Watch does not restart to play it.
            if (gap.kind == replay_data::GapKind::map)
                pack_folder = gap.play_folder;
            else {
                matched_release = true;
                play_folder = gap.play_folder;
            }
        } else if (
            gap.kind == replay_data::GapKind::mod_missing && !gap.id.empty() && gap.fetchable
        ) {
            mod_id = gap.id;
            mod_version = gap.version;
            mod_sha = gap.sha256;
            mod_revision = gap.revision;
            // A mod a `.tad` names with no release plays from the first
            // folder of its id once the catalogue's release is installed.
            mod_names_release =
                !gap.any_release &&
                (!gap.version.empty() || gap.revision.has_value() || !gap.sha256.empty());
        }
        if (gap.turn_on && !gap.id.empty())
            turn_on.push_back(gap.id);
    }
    // A newer engine opens the update centre and does not fetch or play.
    if (engine) {
        runtime.start_update_check(true);
        return ReplayFetch::refused;
    }
    // The button is left off for these. A press that still arrives does not play.
    // Another release is fetched and installed beside the one in use.
    if (blocked || (other && beside.empty() && fetch.empty() && turn_on.empty()))
        return ReplayFetch::refused;
    fs::path kept_mod;
    fs::path kept_pack;
    for (const replay_data::ContentGap* gap : beside) {
        const std::optional<fs::path> kept =
            package_install::replay_release_folder(runtime.user_folder_, gap->sha256);
        if (!kept)
            continue;
        // A kept pack counts only when it still reads as a pack. An origin
        // with no manifest is fetched again, and the install replaces it.
        if (gap->kind == replay_data::GapKind::map) {
            if (package_install::installed_pack_manifest(*kept))
                kept_pack = *kept;
        } else if (
            gap->kind == replay_data::GapKind::mod_other ||
            gap->kind == replay_data::GapKind::mod_missing
        )
            kept_mod = *kept;
    }
    if (!fetch.empty() || !beside.empty()) {
        const std::shared_ptr<const content::Snapshot> snapshot =
            runtime.content_service().snapshot();
        if (snapshot == nullptr) {
            library::refuse_replay(model, replay.path, offline_sentences(model.text, gaps));
            ++model_revision;
            return ReplayFetch::refused;
        }
        content::Downloads& downloads = runtime.content_downloads();
        ReplayShelf::PendingGet pending;
        pending.path = replay.path;
        pending.watch_path = std::move(relative);
        pending.file = file;
        pending.from = from;
        pending.turn_on = std::move(turn_on);
        pending.map_pack = map_pack;
        pending.mod_id = mod_id;
        pending.beside_mod = kept_mod;
        pending.pack_folder = pack_folder;
        pending.play_folder = play_folder;
        pending.mod_names_release = mod_names_release;
        pending.mod_version = mod_version;
        pending.mod_sha = mod_sha;
        pending.mod_revision = mod_revision;
        std::vector<const replay_data::ContentGap*> waiting = fetch;
        waiting.insert(waiting.end(), beside.begin(), beside.end());
        for (const replay_data::ContentGap* gap : waiting) {
            const bool install_beside =
                std::find(beside.begin(), beside.end(), gap) != beside.end();
            if (install_beside && (gap->kind == replay_data::GapKind::mod_other ||
                                   gap->kind == replay_data::GapKind::mod_missing))
                pending.beside_mod_sha = gap->sha256;
            if (install_beside && gap->kind == replay_data::GapKind::map)
                pending.beside_pack_sha = gap->sha256;
            if (install_beside && gap->kind == replay_data::GapKind::map && !kept_pack.empty())
                continue;
            if (install_beside && gap->kind != replay_data::GapKind::map && !kept_mod.empty())
                continue;
            std::string why;
            std::optional<content::DownloadTarget> target = target_for(*snapshot, *gap, &why);
            if (!target) {
                library::refuse_replay(
                    model,
                    replay.path,
                    why.empty() ? std::string("the catalogue has no such package") : why
                );
                ++model_revision;
                return ReplayFetch::refused;
            }
            target->beside = install_beside;
            std::optional<uint64_t> id =
                downloads.queue(std::move(*target), content::DownloadReason::install, &why);
            bool installed = false;
            if (!id) {
                for (const content::DownloadView& item : downloads.view()) {
                    if (item.target.registry != gap->registry || item.target.key != gap->id ||
                        item.target.release != gap->release)
                        continue;
                    if (item.state == content::DownloadState::done)
                        installed = true;
                    else if (
                        item.state != content::DownloadState::failed &&
                        item.state != content::DownloadState::cancelled
                    )
                        id = item.item;
                    break;
                }
            }
            if (!id && !installed) {
                library::refuse_replay(
                    model,
                    replay.path,
                    why.empty()
                        ? std::string(
                              library::shown(model.text, library::words::replay_cannot_watch)
                          )
                        : why
                );
                ++model_revision;
                return ReplayFetch::refused;
            }
            if (id)
                pending.items.push_back(*id);
        }
        if (!pending.items.empty()) {
            const replay_data::ContentGap* named = !beside.empty() ? beside.front() : nullptr;
            if (named == nullptr && !fetch.empty())
                named = fetch.front();
            const std::string name = named == nullptr ? std::string{} : gap_title(*named);
            ReplayShelf& shelf = replay_shelf();
            shelf.fetch_sentence = getting_sentence(model.text, name, 0);
            shelf.pending_get = std::move(pending);
            replays_log("getting " + replay.path);
            fill_replay_inputs(runtime, model.inputs);
            library::update_replays(model);
            ++model_revision;
            return ReplayFetch::waiting;
        }
        turn_on = std::move(pending.turn_on);
        mod_id = std::move(pending.mod_id);
        kept_mod = std::move(pending.beside_mod);
        if (kept_mod.empty() && !pending.beside_mod_sha.empty()) {
            const std::optional<fs::path> kept = package_install::replay_release_folder(
                runtime.user_folder_, pending.beside_mod_sha
            );
            if (kept)
                kept_mod = *kept;
        }
        if (kept_pack.empty() && !pending.beside_pack_sha.empty()) {
            const std::optional<fs::path> kept = package_install::replay_release_folder(
                runtime.user_folder_, pending.beside_pack_sha
            );
            if (kept)
                kept_pack = *kept;
        }
        if (pack_folder.empty())
            pack_folder = pending.pack_folder;
    }
    fs::path mod_folder = kept_mod;
    // A release the row matched is played from its own folder.
    if (mod_folder.empty() && !play_folder.empty())
        mod_folder = path_from_utf8(play_folder);
    // The first Mods folder of the id is only for a need that names no release.
    if (mod_folder.empty() && (mod_names_release || !mod_id.empty())) {
        const std::optional<fs::path> installed = ReplayContentAccess::folder_to_play(
            runtime, replay_shelf(), mod_id, mod_version, mod_revision, mod_sha, mod_names_release
        );
        if (!installed) {
            library::refuse_replay(
                model,
                replay.path,
                std::string(library::shown(model.text, library::words::replay_cannot_watch))
            );
            ++model_revision;
            return ReplayFetch::refused;
        }
        mod_folder = *installed;
    }
    if (matched_release && mod_folder.empty()) {
        library::refuse_replay(
            model,
            replay.path,
            std::string(library::shown(model.text, library::words::replay_cannot_watch))
        );
        ++model_revision;
        return ReplayFetch::refused;
    }
    fs::path pack_mount = kept_pack;
    if (pack_mount.empty() && !pack_folder.empty())
        pack_mount = path_from_utf8(pack_folder);
    if (!pack_mount.empty()) {
        std::error_code pack_missing;
        if (!fs::is_directory(pack_mount, pack_missing)) {
            library::refuse_replay(
                model,
                replay.path,
                std::string(library::shown(model.text, library::words::replay_cannot_watch))
            );
            ++model_revision;
            return ReplayFetch::refused;
        }
    }
    if (!turn_on.empty() || !mod_folder.empty()) {
        const int turned = ReplayContentAccess::switch_for_replay(
            runtime, file, turn_on, mod_folder, pack_mount, from
        );
        if (turned == 2) {
            // The Library is still open, so this row crosses the restart into
            // the run that plays the replay and the one that puts the choice back.
            // The pack is mounted on that run, not in this one.
            library::select_replay(model, replay.path);
            runtime.keep_library_for_restart();
            return ReplayFetch::waiting;
        }
        if (turned == 0) {
            library::refuse_replay(
                model,
                replay.path,
                std::string(library::shown(model.text, library::words::replay_cannot_watch))
            );
            ++model_revision;
            return ReplayFetch::refused;
        }
    }
    // Empty clears a pack mounted for an earlier replay. A different path
    // drops the map decisions made for the pack it replaces.
    runtime.use_replay_map_pack(pack_mount);
    if (!replay_mount_holds_map(runtime.map_packs().maps(), pack_mount, &replay)) {
        runtime.use_replay_map_pack({});
        library::refuse_replay(
            model,
            replay.path,
            std::string(library::shown(model.text, library::words::replay_cannot_watch))
        );
        ++model_revision;
        return ReplayFetch::refused;
    }
    return ReplayFetch::watch;
}

void Runtime::LibraryHost::pump_replay_pictures(Runtime& runtime, const kit::DisplayList& list) {
    if (model.tab != library::Tab::replays || !replays)
        return;
    ReplayShelf& shelf = *replays;
    shelf.runtime = &runtime;
    shelf.read_chunk = read_game_chunk;
    if (!shelf.palette_tried) {
        shelf.palette_tried = true;
        try {
            const std::vector<uint8_t> bytes = runtime.assets_.read("palettes/palette.pal").bytes;
            if (bytes.size() == oa::PaletteBytes{}.size()) {
                oa::PaletteBytes palette{};
                std::copy(bytes.begin(), bytes.end(), palette.begin());
                shelf.palette = palette;
            }
        } catch (const std::exception&) {
        }
    }
    if (!shelf.palette)
        return;
    // The maps of the rows the list shows, the selected replay's first.
    std::vector<std::string> wanted;
    const auto want = [&](const library::ReplayRow& row) {
        if (row.replay.map.empty() || shelf.pictures.count(row.replay.map) != 0 ||
            shelf.cache.failed(picture_key(row.replay.map)) ||
            std::find(wanted.begin(), wanted.end(), row.replay.map) != wanted.end())
            return;
        wanted.push_back(row.replay.map);
    };
    if (const library::ReplayRow* selected = library::selected_replay(model))
        want(*selected);
    const std::vector<library::ReplayRow>& listed_rows = library::shown_replay_rows(model);
    for (const std::size_t place : library::replay_places_shown(model, list)) {
        if (place >= model.replays.visible.size())
            continue;
        const std::size_t index = model.replays.visible[place];
        if (index < listed_rows.size())
            want(listed_rows[index]);
    }
    if (wanted.empty())
        return;
    shelf.cache.new_frame();
    for (const std::string& map : wanted) {
        previews::PreviewRequest request;
        request.key = picture_key(map);
        request.source = previews::PreviewSource::base;
        request.map = map;
        static_cast<void>(shelf.cache.ask(request));
    }
    previews::PreviewHost reader;
    reader.context = &shelf;
    reader.read_map = replay_read_map;
    reader.palette = &*shelf.palette;
    shelf.cache.pump(reader, 1);
    bool stored = false;
    for (const std::string& map : wanted) {
        previews::PreviewRequest request;
        request.key = picture_key(map);
        request.source = previews::PreviewSource::base;
        request.map = map;
        const previews::Thumbnail* thumbnail = shelf.cache.ask(request);
        if (thumbnail == nullptr || thumbnail->width <= 0 || thumbnail->height <= 0)
            continue;
        const auto width = static_cast<uint32_t>(thumbnail->width);
        const auto height = static_cast<uint32_t>(thumbnail->height);
        if (thumbnail->rgba.size() != static_cast<std::size_t>(width) * height * 4U)
            continue;
        library::MapPicture picture;
        picture.rgba = thumbnail->rgba;
        picture.width = width;
        picture.height = height;
        shelf.pictures.emplace(map, std::move(picture));
        stored = true;
    }
    if (stored && !replays_held) {
        fill_replay_inputs(runtime, model.inputs);
        library::update_replays(model);
        ++model_revision;
    }
}

bool Runtime::LibraryHost::start_replay_watch(
    Runtime& runtime, const std::string& path, std::optional<uint32_t> from
) {
    // Watch stops the pass before the replay opens. A stopped pass
    // writes nothing, so the row says Reading… again afterwards.
    stop_index_pass();
    const fs::path folder = runtime.user_folder_ / std::string(recordings_folder_name);
    const std::optional<fs::path> file = replay_file(folder, path);
    std::vector<uint8_t> bytes;
    if (!file || !read_replay_bytes(*file, bytes)) {
        library::refuse_replay(
            model, path, std::string(library::shown(model.text, replay_data::file_unreadable))
        );
        ++model_revision;
        return false;
    }
    runtime.replay_recording_path_ = *file;
    runtime.replay_handed_timeline_ = timeline_of(runtime, path);
    const std::string name = platform::path_to_utf8(file->filename());
    const RecordingInput input{name.c_str(), bytes.data(), bytes.size(), true};
    ReplayHooks hooks{};
    RecordingInfo info{};
    std::string refusal;
    try {
        if (!call_hook_or_raise<&Extension::open_recording>(
                runtime.extension_, runtime, input, hooks, info
            ))
            refusal = "no extension of this build replays " + name;
    } catch (const std::exception& error) {
        refusal = error.what();
    }
    if (refusal.empty() &&
        (hooks.step == nullptr || hooks.status == nullptr || hooks.close == nullptr)) {
        refusal = "the extension that replays " + name + " left a replay hook null";
    }
    if (!refusal.empty()) {
        runtime.replay_handed_timeline_.clear();
        replays_log("cannot watch " + path + ": " + refusal);
        // The extension may have refused after its match began loading: the
        // screen is then the loading screen, with a half-made match behind it,
        // and nothing else would ever leave it. What was begun goes, the main
        // menu shows again under the Library, and the Library stays on the
        // same row, at the same scroll, with the refusal in its card.
        if (runtime.screen_ != Screen::main_menu || runtime.match_ != nullptr) {
            runtime.leave_match();
            runtime.load(Screen::main_menu);
        }
        library::refuse_replay(model, path, without_file_name(std::move(refusal), name));
        ++model_revision;
        return false;
    }
    replays_log("watching " + path);
    ReplayShelf& shelf = replay_shelf();
    shelf.watching = hooks;
    shelf.pending_seek.reset();
    shelf.left_tick.reset();
    shelf.left_finished = false;
    shelf.watch_length.reset();
    shelf.left_path = path;
    for (const replay_data::Replay& replay : shelf.index.replays) {
        if (replay.path != path || !replay.timeline.has_value() || replay.timeline->empty())
            continue;
        if (const std::optional<oademo::TimelineIndex> parsed =
                oademo::parse_timeline_index(*replay.timeline))
            shelf.watch_length = parsed->length_ticks;
        break;
    }
    // The jump waits until the replay can say where it is. Seeking in
    // this call would find no match yet.
    if (from)
        shelf.pending_seek = from;
    // What the Library shows comes back when the match is left.
    auto shown = std::make_shared<LibraryReturn>();
    shown->tab = library::Tab::replays;
    shown->filters = model.filters;
    shown->query = model.query;
    shown->list_scroll = state.list_scroll;
    shown->replay_filter = model.replays.filter;
    shown->replay_kind = model.replays.kind;
    shown->replay_sort = model.replays.sort;
    shown->replay_source = model.replays.source;
    shown->core_filter = model.replays.core_filter;
    shown->core_planet = model.replays.planet;
    if (model.replays.source == library::ReplaySource::core_prime && model.replays.selected)
        shown->replay = *model.replays.selected;
    else
        shown->replay = path;
    shown->replay_page = state.replay_page;
    runtime.library_return_ = std::move(shown);
    runtime.library_return_since_.reset();
    return true;
}

void Runtime::LibraryHost::pump_replay_fetch(Runtime& runtime) {
    if (!replays)
        return;
    // A catalogue that arrives after the rows were filled changes what they need.
    // A check's rows already say what they need, and are not read from the folder.
    const std::shared_ptr<const content::Snapshot> snapshot = runtime.content_service().snapshot();
    const uint64_t generation = snapshot != nullptr ? snapshot->generation : 0;
    if (!replays_held && generation != replays->content_generation) {
        fill_replay_inputs(runtime, model.inputs);
        library::update_replays(model);
        ++model_revision;
    }
    if (!replays->pending_get)
        return;
    if (runtime.oa_layer().find(library_screen_name) == nullptr) {
        replays->pending_get.reset();
        replays->fetch_sentence.clear();
        return;
    }
    const ReplayShelf::PendingGet pending = *replays->pending_get;
    const std::vector<content::DownloadView> items = runtime.content_downloads().view();
    bool waiting = false;
    for (const uint64_t id : pending.items) {
        const content::DownloadView* found = nullptr;
        for (const content::DownloadView& item : items) {
            if (item.item == id)
                found = &item;
        }
        if (found != nullptr && found->state != content::DownloadState::failed &&
            found->state != content::DownloadState::cancelled) {
            if (found->state == content::DownloadState::done)
                clear_registry_away(*replays, found->target.registry);
            else
                waiting = true;
            continue;
        }
        const library::ReplayRow* row = library::find_replay(model, pending.path);
        std::string sentence;
        if (found != nullptr && found->failure == content::DownloadFailure::not_offered) {
            int64_t sequence = 0;
            if (snapshot != nullptr) {
                for (const content::RegistryView& view : snapshot->registries) {
                    if (view.registry.descriptor.id == found->target.registry &&
                        view.catalogue != nullptr)
                        sequence = view.catalogue->sequence;
                }
            }
            remember_refusal(
                *replays, digest_hex(found->target.sha256), found->target.registry, sequence
            );
            const replay_data::ContentGap* gap = nullptr;
            if (row != nullptr) {
                for (const replay_data::ContentGap& candidate : row->gaps) {
                    if (candidate.id == found->target.key &&
                        candidate.release == found->target.release)
                        gap = &candidate;
                }
            }
            if (gap != nullptr)
                sentence = catalogue_sentence(model, *gap);
            else
                sentence = library::filled(
                    model.text,
                    library::words::replay_not_in_catalogue_name,
                    {{"name", found->target.name},
                     {"registry", library::replay_registry_name(model, found->target.registry)}}
                );
        } else if (
            found != nullptr && found->failure == content::DownloadFailure::unreachable &&
            found->target.prompt
        ) {
            // The same offline card as a failed refresh: the sentences in the
            // box, no Get, and the chip kept. A plain refusal would offer Get again.
            mark_registry_away(*replays, snapshot.get(), found->target.registry);
            replays->pending_get.reset();
            replays->fetch_sentence.clear();
            fill_replay_inputs(runtime, model.inputs);
            library::update_replays(model);
            ++model_revision;
            return;
        } else if (found == nullptr || found->failure == content::DownloadFailure::unreachable) {
            sentence = offline_sentences(
                model.text,
                row != nullptr ? std::span<const replay_data::ContentGap>{row->gaps}
                               : std::span<const replay_data::ContentGap>{}
            );
        } else {
            sentence = content::failure_message(*found, &oa::data::languages::interface_text);
        }
        replays->pending_get.reset();
        replays->fetch_sentence.clear();
        // The refusal is read against the gaps the next catalogue comparison
        // gives, so a 409 is unlisted before the card is told.
        fill_replay_inputs(runtime, model.inputs);
        library::update_replays(model);
        library::refuse_replay(model, pending.path, std::move(sentence));
        ++model_revision;
        return;
    }
    if (waiting) {
        const std::string sentence = fetch_progress_sentence(model.text, pending, items);
        if (sentence != replays->fetch_sentence) {
            replays->fetch_sentence = sentence;
            fill_replay_inputs(runtime, model.inputs);
            library::update_replays(model);
            ++model_revision;
        }
        return;
    }
    replays->pending_get.reset();
    replays->fetch_sentence.clear();
    replays->have_ready = false;
    if (pending.map_pack) {
        runtime.map_packs().refresh();
        replays->have_ready = false;
    }
    fs::path mod_folder = pending.beside_mod;
    // A release the row matched is played from its own folder.
    if (mod_folder.empty() && !pending.play_folder.empty())
        mod_folder = path_from_utf8(pending.play_folder);
    if (mod_folder.empty() && !pending.beside_mod_sha.empty()) {
        const std::optional<fs::path> kept =
            package_install::replay_release_folder(runtime.user_folder_, pending.beside_mod_sha);
        if (!kept) {
            library::refuse_replay(
                model,
                pending.path,
                std::string(library::shown(model.text, library::words::replay_cannot_watch))
            );
            ++model_revision;
            return;
        }
        mod_folder = *kept;
    }
    // The first Mods folder of the id is only for a need that names no release.
    if (mod_folder.empty() && (pending.mod_names_release || !pending.mod_id.empty())) {
        const std::optional<fs::path> installed = ReplayContentAccess::folder_to_play(
            runtime,
            *replays,
            pending.mod_id,
            pending.mod_version,
            pending.mod_revision,
            pending.mod_sha,
            pending.mod_names_release
        );
        if (!installed) {
            library::refuse_replay(
                model,
                pending.path,
                std::string(library::shown(model.text, library::words::replay_cannot_watch))
            );
            ++model_revision;
            return;
        }
        mod_folder = *installed;
    }
    // The kept pack is known before a restart, so the next run can mount it.
    // A folder whose origin verifies but that does not read as a pack is
    // refused: Watch would otherwise play the copy in Maps.
    fs::path pack_mount;
    if (!pending.beside_pack_sha.empty()) {
        const std::optional<fs::path> kept =
            package_install::replay_release_folder(runtime.user_folder_, pending.beside_pack_sha);
        if (!kept || !package_install::installed_pack_manifest(*kept)) {
            library::refuse_replay(
                model,
                pending.path,
                std::string(library::shown(model.text, library::words::replay_cannot_watch))
            );
            ++model_revision;
            return;
        }
        pack_mount = *kept;
    } else if (!pending.pack_folder.empty()) {
        pack_mount = path_from_utf8(pending.pack_folder);
    }
    const int turned = ReplayContentAccess::switch_for_replay(
        runtime, pending.file, pending.turn_on, mod_folder, pack_mount, pending.from
    );
    if (turned == 2) {
        // The Library is still open, so this row crosses the restart into the
        // run that plays the replay and the one that puts the choice back.
        // The pack is mounted on that run, not in this one.
        library::select_replay(model, pending.path);
        runtime.keep_library_for_restart();
        return;
    }
    if (turned == 0) {
        library::refuse_replay(
            model,
            pending.path,
            std::string(library::shown(model.text, library::words::replay_cannot_watch))
        );
        ++model_revision;
        return;
    }
    runtime.use_replay_map_pack(pack_mount);
    const library::ReplayRow* watching = library::find_replay(model, pending.path);
    if (!replay_mount_holds_map(
            runtime.map_packs().maps(),
            pack_mount,
            watching != nullptr ? &watching->replay : nullptr
        )) {
        runtime.use_replay_map_pack({});
        library::refuse_replay(
            model,
            pending.path,
            std::string(library::shown(model.text, library::words::replay_cannot_watch))
        );
        ++model_revision;
        return;
    }
    const std::string watch_at = pending.watch_path.empty() ? pending.path : pending.watch_path;
    static_cast<void>(start_replay_watch(runtime, watch_at, pending.from));
}

bool Runtime::LibraryHost::replay_action(Runtime& runtime, const library::ScreenResult& result) {
    if (!result.action)
        return false;
    const fs::path folder = runtime.user_folder_ / std::string(recordings_folder_name);
    switch (*result.action) {
    case library::Action::watch_replay:
    case library::Action::watch_from: {
        const std::optional<uint32_t> from =
            *result.action == library::Action::watch_from ? result.watch_from : std::nullopt;
        // A press while packages are coming cancels that fetch. It does not
        // start another, for this replay or any other.
        if (replays && replays->pending_get) {
            if (replays->pending_get->path != result.replay)
                return false;
            for (const uint64_t id : replays->pending_get->items)
                runtime.content_downloads().cancel(id);
            replays->pending_get.reset();
            replays->fetch_sentence.clear();
            fill_replay_inputs(runtime, model.inputs);
            library::update_replays(model);
            ++model_revision;
            replays_log("stopped getting " + result.replay);
            return false;
        }
        if (model.replays.source == library::ReplaySource::core_prime) {
            start_core_download(runtime, result.replay, true);
            return false;
        }
        const library::ReplayRow* row = library::find_replay(model, result.replay);
        if (row == nullptr)
            return false;
        const std::optional<fs::path> file = replay_file(folder, result.replay);
        if (!file) {
            library::refuse_replay(
                model,
                result.replay,
                std::string(library::shown(model.text, replay_data::file_unreadable))
            );
            ++model_revision;
            return false;
        }
        // The same offer, restart and watch as a Core Prime replay whose file
        // is already on this computer.
        const ReplayFetch fetch =
            queue_replay_get(runtime, result.replay, *file, row->replay, from);
        if (fetch != ReplayFetch::watch)
            return false;
        // Watch from jumps to where the replay was left, once the replay can say
        // where it is. A restart keeps that tick and seeks after it.
        return start_replay_watch(runtime, result.replay, from);
    }
    case library::Action::keep_replays: {
        ReplayShelf& shelf = replay_shelf();
        load_kept_marks(shelf, folder);
        // The marks are the replays' own, by recording, in a file that no change
        // to the index touches, and are saved at once: a Keep pressed while the
        // folder is still being read is kept, whatever the Library does next.
        bool changed = false;
        for (const std::string& path : result.replays)
            if (const replay_data::Replay* replay = shown_replay(shelf, path))
                changed = replay_data::set_kept(shelf.kept, *replay, result.keep) || changed;
        if (!changed)
            return false;
        if (!replay_data::save_kept(folder, shelf.kept))
            replays_log("the kept replays could not be saved");
        replays_log(
            std::string(result.keep ? "kept " : "no longer keeping ") +
            std::to_string(result.replays.size()) + " replays"
        );
        fill_replay_inputs(runtime, model.inputs);
        library::update_replays(model);
        ++model_revision;
        return false;
    }
    case library::Action::open_replay_folder: {
        const std::optional<fs::path> file = replay_file(folder, result.replay);
        if (!file)
            return false;
        const FolderOpening opening = runtime.open_player_folder(file->parent_path());
        if (!opening.opened)
            runtime.status_ = opening.reason;
        return false;
    }
    case library::Action::turn_on_recording:
        runtime.preference_values_[std::string(replays_record_preference)] = "on";
        runtime.preferences_dirty_ = true;
        runtime.flush_preferences();
        replays_log("recording turned on");
        fill_replay_inputs(runtime, model.inputs);
        library::update_replays(model);
        ++model_revision;
        return false;
    case library::Action::open_replay_file:
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__)) ||                            \
    (defined(__APPLE__) && !TARGET_OS_IPHONE)
    {
        // The filter outlives the call: the dialog may answer on another thread.
        static const SDL_DialogFileFilter filters[] = {{"Replay", "oademo;tad"}};
        SDL_ShowOpenFileDialog(
            [](void*, const char* const* files, int) {
                if (files == nullptr || files[0] == nullptr)
                    return;
                package_install::post_replay_file(platform::path_from_utf8(files[0]));
            },
            nullptr,
            runtime.sdl_.window,
            filters,
            1,
            nullptr,
            false
        );
    }
#endif
        return false;
    case library::Action::delete_replays:
        // The first press only shows the confirm button. It deletes nothing.
        return false;
    case library::Action::confirm_delete_replays: {
        ReplayShelf* shelf = replays ? replays.get() : nullptr;
        StoredBatch batch = open_batch(shelf, folder);
        bool changed = false;
        std::size_t stayed = 0;
        std::string reason;
        for (const std::string& relative : result.replays) {
            const replay_data::Replay replay = replay_for(shelf, batch, relative);
            const TrashOutcome outcome = try_move(
                runtime.replay_trash_left_, folder, relative, runtime.replay_trash_refusal_
            );
            if (!outcome.gone) {
                ++stayed;
                if (reason.empty())
                    reason = outcome.error;
                continue;
            }
            changed = true;
            note_moved(batch, shelf, replay);
        }
        if (changed || batch.kept_dirty || batch.exported_dirty || batch.index_dirty)
            commit_batch(batch, shelf, folder);
        if (!changed && stayed == 0)
            return false;
        fill_replay_inputs(runtime, model.inputs);
        library::update_replays(model);
        model.replays.delete_failure = trash_failure_note(stayed, reason);
        ++model_revision;
        return false;
    }
    case library::Action::export_replays:
        begin_export(runtime, result.replays);
        return false;
    case library::Action::choose_export_folder: {
        const fs::path current = platform::path_from_utf8(model.export_dialog.folder);
        const fs::path start = is_folder(current) ? current : fs::path{};
        fs::path chosen;
        std::string why;
        const FolderPick pick = pick_folder_with_dialog(
            start,
            library::shown(model.text, library::words::export_choose_folder),
            &chosen,
            &why,
            runtime.sdl_.window
        );
        if (pick == FolderPick::chosen && is_folder(chosen)) {
            const DownloadsFolder downloads = downloads_folder(runtime.user_folder_);
            const bool system = downloads.system && same_folder(chosen, downloads.path);
            library::set_export_folder(
                model, platform::path_to_utf8(chosen), export_folder_label(chosen, system), system
            );
            ++model_revision;
        } else if (pick == FolderPick::unavailable) {
            if (!why.empty())
                replays_log("the folder dialog is not available: " + why);
            open_export_listing(runtime, start);
        }
        return false;
    }
    case library::Action::run_export:
        start_export_job();
        return false;
    case library::Action::show_export_folder: {
        const fs::path chosen_folder = platform::path_from_utf8(model.export_dialog.folder);
        if (!is_folder(chosen_folder))
            return false;
        const FolderOpening opening = runtime.open_player_folder(chosen_folder);
        if (!opening.opened)
            runtime.status_ = opening.reason;
        return false;
    }
    case library::Action::export_browse_up: {
        const fs::path current = platform::path_from_utf8(model.export_dialog.browse_folder);
        const fs::path parent = current.parent_path();
        if (!is_folder(current) || parent.empty() || parent == current)
            return false;
        open_export_listing(runtime, parent);
        return false;
    }
    case library::Action::export_browse_open: {
        fs::path published;
        try {
            published = platform::path_from_utf8(result.export_path);
            // A place carries its own path. A child is only a name, and a
            // name of "." or ".." is not a folder the row may open.
            if (published.is_absolute()) {
                if (!is_folder(published))
                    return false;
                open_export_listing(runtime, published);
                return false;
            }
            if (export_name_refused(result.export_path))
                return false;
            const fs::path current = platform::path_from_utf8(model.export_dialog.browse_folder);
            if (!is_folder(current))
                return false;
            const fs::path next = current / published;
            if (!is_folder(next))
                return false;
            open_export_listing(runtime, next);
        } catch (const fs::filesystem_error&) {
            return false;
        }
        return false;
    }
    case library::Action::export_browse_use: {
        const fs::path chosen = platform::path_from_utf8(model.export_dialog.browse_folder);
        if (!is_folder(chosen))
            return false;
        const DownloadsFolder downloads = downloads_folder(runtime.user_folder_);
        const bool system = downloads.system && same_folder(chosen, downloads.path);
        library::set_export_folder(
            model, platform::path_to_utf8(chosen), export_folder_label(chosen, system), system
        );
        ++model_revision;
        return false;
    }
    case library::Action::download_replay:
        if (model.replays.source != library::ReplaySource::core_prime)
            return false;
        if (model.inputs.replays.core_download_waiting &&
            model.inputs.replays.core_download_id == result.replay) {
            cancel_core_download();
            return false;
        }
        start_core_download(runtime, result.replay, false);
        return false;
    case library::Action::browse_core_prime:
        library::set_replay_source(model, library::ReplaySource::core_prime);
        ++model_revision;
        return false;
    case library::Action::sign_in:
    case library::Action::submit_sign_in:
    case library::Action::sign_out:
    case library::Action::share_replay:
    case library::Action::submit_share:
    case library::Action::open_shared_replay:
    case library::Action::cancel_sign_in:
    case library::Action::cancel_share:
        return take_share_action(runtime, result);
    default:
        return false;
    }
}

void Runtime::LibraryHost::open_replay_export(Runtime& runtime, std::string path) {
    std::vector<std::string> paths;
    if (!path.empty())
        paths.push_back(std::move(path));
    begin_export(runtime, std::move(paths));
}

void Runtime::take_opened_replays() {
    if (!library_can_open())
        return;
    LibraryHost& host = library_host();
    // One file at a time. The next waits until this one's notice has closed,
    // and until the copy on the worker has been told. The Library this import
    // opens is itself a modal and stays up, so "a modal" would never let the
    // next file through. The replay notice is the one that has to close.
    // Any other modal waits, as a registry file does.
    const OaLayer& layer = oa_layer();
    const bool notice = layer.find("replay-import") != nullptr;
    const LayerScreen* const top = layer.top();
    const bool library_only = top != nullptr && top->name() == library_screen_name && !notice;
    const bool blocked = notice || (layer.modal_shown(false) && !library_only);
    if (host.replays) {
        ReplayShelf& shelf = *host.replays;
        std::optional<replay_data::ImportResult> done;
        fs::path release;
        bool busy = false;
        {
            base::threads::LockGuard guard(shelf.mutex);
            if (shelf.import_done && !blocked) {
                done = std::move(*shelf.import_done);
                shelf.import_done.reset();
                release = std::move(shelf.import_release);
                shelf.import_release.clear();
            } else if (shelf.import_running || shelf.import_due || shelf.import_done) {
                busy = true;
            }
        }
        if (done) {
            apply_opened_replay(*done, release);
            return;
        }
        if (busy)
            return;
    }
    if (blocked || !package_install::replay_files_waiting())
        return;
    const std::optional<fs::path> source = package_install::take_replay_file();
    if (!source)
        return;
    const BroughtFile brought = bring_opened_file(*source);
    if (!brought.brought) {
        show_replay_notice(oa_layer(), brought.why);
        return;
    }
    if (user_folder_.empty()) {
        show_replay_notice(
            oa_layer(),
            std::string(oa::data::languages::interface_text(replay_data::file_unreadable))
        );
        release_brought_file(*source, brought.readable);
        return;
    }
    const fs::path recordings = user_folder_ / std::string(recordings_folder_name);
    const fs::path release = brought.readable != *source ? brought.readable : fs::path{};
    ReplayShelf& shelf = host.replay_shelf();
    bool queued = false;
    {
        base::threads::LockGuard guard(shelf.mutex);
        // The copy's path is stored before the worker is told, so a fast
        // worker cannot finish and be applied before it is set.
        shelf.import_release = release;
        shelf.import_recordings = recordings;
        shelf.import_source = brought.readable;
        shelf.import_due = true;
        queued = start_replay_worker(shelf);
        if (!queued) {
            shelf.import_due = false;
            shelf.import_release.clear();
        }
    }
    if (!queued) {
        const replay_data::ImportResult result =
            replay_data::import_replay(recordings, brought.readable);
        apply_opened_replay(result, release);
        return;
    }
    shelf.wake.notify_all();
}

/// A move of old replays off the main thread. The worker moves files only.
struct Runtime::ReplayTrashSweep {
    fs::path folder{};                       ///< the Recordings folder
    std::vector<replay_data::Replay> due{};  ///< the rows the read already held
    base::threads::Thread thread{};          ///< the worker, once started
    base::threads::Mutex mutex{};            ///< guards the fields below
    std::atomic<bool> stop{false};           ///< set before the join, checked between files
    bool finished{false};                    ///< the worker has stored what it did
    std::vector<replay_data::Replay> gone{}; ///< the rows whose files went
    std::size_t stayed{0};                   ///< files the trash left behind
    std::string reason{};                    ///< the first reason the card may say
    std::vector<fs::path> trash_left{};      ///< trash copies, when the system said
};

/// The replays a move will take, held until the worker starts.
struct Runtime::ReplayTrashRequest {
    fs::path folder{};
    std::vector<replay_data::Replay> due{};
};

void Runtime::destroy_replay_trash_request(ReplayTrashRequest* request) noexcept {
    delete request;
}

void Runtime::destroy_replay_trash_sweep(ReplayTrashSweep* sweep) noexcept {
    if (sweep == nullptr)
        return;
    sweep->stop.store(true);
    base::threads::join_thread(sweep->thread);
    delete sweep;
}

void Runtime::trash_worker(void* argument) {
    auto* sweep = static_cast<ReplayTrashSweep*>(argument);
    std::vector<replay_data::Replay> gone;
    std::size_t stayed = 0;
    std::string reason;
    std::vector<fs::path> trash_left;
    for (const replay_data::Replay& replay : sweep->due) {
        if (sweep->stop.load())
            break;
        const TrashOutcome outcome = try_move(trash_left, sweep->folder, replay.path, "");
        if (outcome.gone) {
            gone.push_back(replay);
            continue;
        }
        ++stayed;
        if (reason.empty())
            reason = outcome.error;
    }
    // What already went is recorded even when exit asked the move to stop,
    // so a later collection can drop those rows. Exit itself does not collect.
    base::threads::LockGuard held(sweep->mutex);
    sweep->gone = std::move(gone);
    sweep->stayed = stayed;
    sweep->reason = std::move(reason);
    sweep->trash_left = std::move(trash_left);
    sweep->finished = true;
}

void Runtime::begin_replay_trash_sweep() {
    if (replay_trash_due_ == nullptr || replay_trash_sweep_ != nullptr)
        return;
    auto* sweep = new ReplayTrashSweep();
    sweep->folder = std::move(replay_trash_due_->folder);
    sweep->due = std::move(replay_trash_due_->due);
    replay_trash_due_.reset();
    if (sweep->due.empty() ||
        !base::threads::start_thread(sweep->thread, &Runtime::trash_worker, sweep)) {
        if (!sweep->due.empty())
            replays_log("old replays could not be moved off the main thread");
        delete sweep;
        return;
    }
    replay_trash_sweep_.reset(sweep);
}

void Runtime::collect_replay_trash_sweep() {
    if (replay_trash_sweep_ == nullptr)
        return;
    bool finished = false;
    {
        base::threads::LockGuard held(replay_trash_sweep_->mutex);
        finished = replay_trash_sweep_->finished;
    }
    if (!finished)
        return;
    fs::path folder;
    std::vector<replay_data::Replay> gone;
    std::size_t stayed = 0;
    std::string reason;
    std::vector<fs::path> trash_left;
    {
        base::threads::LockGuard held(replay_trash_sweep_->mutex);
        folder = std::move(replay_trash_sweep_->folder);
        gone = std::move(replay_trash_sweep_->gone);
        stayed = replay_trash_sweep_->stayed;
        reason = std::move(replay_trash_sweep_->reason);
        trash_left = std::move(replay_trash_sweep_->trash_left);
    }
    replay_trash_sweep_.reset();
    ReplayShelf* shelf = nullptr;
    if (library_ && library_->replays)
        shelf = library_->replays.get();
    if (!gone.empty()) {
        StoredBatch batch = open_batch(shelf, folder);
        for (const replay_data::Replay& replay : gone)
            note_moved(batch, shelf, replay);
        commit_batch(batch, shelf, folder);
    }
    for (fs::path& left : trash_left)
        replay_trash_left_.push_back(std::move(left));
    if (library_) {
        library_->fill_replay_inputs(*this, library_->model.inputs);
        library::update_replays(library_->model);
        library_->model.replays.delete_failure = trash_failure_note(stayed, reason);
        ++library_->model_revision;
    }
    begin_replay_trash_sweep();
}

namespace {

/// Frames in a row the main menu shows before old replays are considered.
constexpr uint32_t kRetentionMenuFrames = 2;

/// What a read of Recordings found. The worker fills it and does not decide
/// which replays go: the main thread reads the marks after the scan, so a
/// Keep pressed while the folder was read is not lost.
struct RetentionRead {
    bool failed{false};          ///< the folder could not be read
    bool have_index{false};      ///< `stored` is a scan that finished
    replay_data::Index stored{}; ///< the index the scan built
};

/// Reads Recordings into an index. Does not save it, and does not read the marks.
///
/// @param folder the Recordings folder
/// @param hooks when to stop; a stop leaves the files not yet read unread
/// @return the index, or a failed read
RetentionRead scan_recordings(const fs::path& folder, const replay_data::ScanHooks& hooks = {}) {
    RetentionRead read;
    try {
        read.stored = replay_data::scan(folder, replay_data::load_index(folder), hooks);
        read.have_index = true;
    } catch (const std::exception& error) {
        replays_log(std::string("the replays could not be read: ") + error.what());
        read.failed = true;
    }
    return read;
}

/// Tells whether a mark file is missing while a damaged copy of it is kept.
///
/// A missing file with no damaged copy is a player who keeps nothing, and
/// is not this. A start that could not read the file set it aside.
///
/// @param file the mark file
/// @return true when the file is absent and `file.damaged` is there
bool marks_waiting(const fs::path& file) {
    std::error_code failure;
    if (fs::exists(file, failure) || failure)
        return false;
    fs::path aside = file;
    aside += std::string(replay_data::damaged_ending);
    failure.clear();
    return fs::is_regular_file(aside, failure);
}

/// The question's one sentence.
///
/// @param count how many replays would go
/// @return the sentence in the player's language
std::string retention_body(std::size_t count) {
    const bool trash = platform::system_has_trash();
    if (count == 1) {
        return interface_sentence(
            trash ? library::words::replay_retention_one
                  : library::words::replay_retention_one_removed
        );
    }
    return with_count(
        interface_sentence(
            trash ? library::words::replay_retention_many
                  : library::words::replay_retention_many_removed
        ),
        count
    );
}

} // namespace

/// A read of old replays running off the main thread.
struct Runtime::ReplayRetentionScan {
    fs::path folder{};              ///< the Recordings folder
    base::threads::Thread thread{}; ///< the worker, once started
    base::threads::Mutex mutex{};   ///< guards the fields below `stop`
    std::atomic<bool> stop{false};  ///< set before the join; the scan stops before its next file
    bool finished{false};           ///< the worker has stored the read
    bool failed{false};             ///< the folder could not be read
    bool have_index{false};         ///< `stored` is a scan that finished
    replay_data::Index stored{};    ///< the index the scan built
};

void Runtime::retention_worker(void* argument) {
    auto* scan = static_cast<ReplayRetentionScan*>(argument);
    replay_data::ScanHooks hooks;
    hooks.context = scan;
    hooks.stop = [](void* context) {
        return static_cast<ReplayRetentionScan*>(context)->stop.load();
    };
    RetentionRead read = scan_recordings(scan->folder, hooks);
    if (scan->stop.load())
        return;
    base::threads::LockGuard held(scan->mutex);
    if (scan->stop.load())
        return;
    scan->failed = read.failed;
    scan->have_index = read.have_index;
    scan->stored = std::move(read.stored);
    scan->finished = true;
}

void Runtime::destroy_replay_retention_scan(ReplayRetentionScan* scan) noexcept {
    if (scan == nullptr)
        return;
    scan->stop.store(true);
    base::threads::join_thread(scan->thread);
    delete scan;
}

void Runtime::consider_replay_retention(bool forced) {
    collect_replay_trash_sweep();
    // A move still running keeps the menu drawing. The next frame collects it.
    if (replay_trash_sweep_ != nullptr)
        return;
    if (replay_retention_settled_ && !forced)
        return;
    if (!forced && options_.check_library_replays)
        return;
    if (replay_retention_asking_)
        return;
    if (user_folder_.empty())
        return;
    const std::string asked =
        preference_text(preference_values_, replays_retention_asked_preference);
    const bool answered = asked == replays_retention_keep || asked == replays_retention_let_go;
    // A run nobody watches, with the question unanswered, removes nothing
    // and does not read the folder.
    if (!forced && !answered && UserFolderState::unwatched(options_.unattended)) {
        replay_retention_settled_ = true;
        return;
    }

    const auto on_menu = [this] {
        return screen_ == Screen::main_menu && !frame_owned_by_package() &&
               state_.state == frontend::state_id::main_menu &&
               state_.pending_signal != frontend::signal_id::multiplayer;
    };
    const auto blocked = [this] {
        return oa::ui::frontend_dialogs::dialog_count() != 0 ||
               engine_settings_dialog() != nullptr ||
               (oa_layer_ != nullptr && oa_layer_->top() != nullptr);
    };
    const fs::path folder = user_folder_ / std::string(recordings_folder_name);

    // Applies a finished read on the main thread: saves the index, then reads
    // the marks. A damaged kept.json is set aside and its whole marks are
    // kept; that consideration removes nothing, and while the set-aside copy
    // is there no replay recorded before it goes. Forever removes nothing. The
    // question is shown only after the read. Files that then go are moved
    // off this thread, from the rows this read already holds.
    const auto finish = [this, &folder](RetentionRead read) {
        if (!read.failed && read.have_index) {
            std::error_code missing;
            if ((!read.stored.replays.empty() ||
                 fs::exists(replay_data::index_path(folder), missing)) &&
                !replay_data::save_index(folder, read.stored))
                replays_log("the replays' index could not be saved");
        }
        const std::string span_text = preference_text(preference_values_, replays_keep_preference);
        if (read.failed || replay_data::keep_span_of(span_text) == replay_data::KeepSpan::forever) {
            replay_retention_settled_ = true;
            return;
        }
        const replay_data::KeptRead kept = replay_data::read_kept(folder);
        const replay_data::ExportedRead exported = replay_data::read_exported(folder);
        const fs::path kept_file = replay_data::kept_path(folder);
        const fs::path exported_file = replay_data::exported_path(folder);
        const bool kept_bad = kept.damaged || marks_waiting(kept_file);
        const bool exported_bad = exported.damaged || marks_waiting(exported_file);
        ReplayShelf* shelf = nullptr;
        if (library_ && library_->replays)
            shelf = library_->replays.get();
        const auto refresh_library = [this] {
            if (!library_)
                return;
            library_->fill_replay_inputs(*this, library_->model.inputs);
            library::update_replays(library_->model);
            ++library_->model_revision;
        };
        bool block = false;
        if (kept_bad && !replay_kept_damage_settled_) {
            const KeptSalvage salvaged = salvage_kept_file(folder, kept.damaged, !kept.damaged);
            if (shelf != nullptr) {
                shelf->kept = salvaged.marks;
                shelf->kept_loaded = true;
                shelf->kept_damaged = salvaged.blocked;
                shelf->kept_set_aside = false;
            }
            if (!salvaged.blocked && !salvaged.marks.marks.empty() &&
                !replay_data::save_kept(folder, salvaged.marks))
                replays_log("the kept replays could not be saved");
            // A set-aside that fails leaves the damaged file. Nothing is
            // written over it, and this consideration still removes nothing.
            if (!salvaged.blocked)
                replay_kept_damage_settled_ = true;
            block = true;
        } else if (kept.damaged) {
            block = true;
        } else if (!replay_kept_damage_settled_ && shelf != nullptr && shelf->kept_set_aside) {
            shelf->kept_set_aside = false;
            replay_kept_damage_settled_ = true;
            block = true;
        }
        if (exported_bad) {
            if (!exported.damaged) {
                replays_log(
                    "the exported replays' file was set aside; old replays stay until it can be "
                    "read"
                );
            } else {
                const std::optional<fs::path> aside = replay_data::set_aside(exported_file);
                replays_log(
                    aside ? "the exported replays' file could not be read; it is kept as " +
                                platform::path_to_utf8(aside->filename())
                          : std::string("the exported replays' file could not be read")
                );
            }
            if (shelf != nullptr) {
                shelf->exported = {};
                shelf->exported_loaded = true;
                shelf->exported_damaged = true;
            }
            block = true;
        }
        if (block) {
            replay_retention_settled_ = true;
            refresh_library();
            return;
        }
        replay_data::KeptMarks kept_marks = kept.kept;
        replay_data::KeptMarks exported_marks = exported.exported;
        if (replay_data::apply_kept(kept_marks, read.stored.replays) &&
            !replay_data::save_kept(folder, kept_marks))
            replays_log("the kept replays could not be saved");
        if (replay_data::apply_exported(exported_marks, read.stored.replays) &&
            !replay_data::save_exported(folder, exported_marks))
            replays_log("the exported replays could not be saved");
        if (shelf != nullptr) {
            if (!shelf->kept_damaged) {
                shelf->kept = kept_marks;
                shelf->kept_loaded = true;
            }
            if (!shelf->exported_damaged) {
                shelf->exported = exported_marks;
                shelf->exported_loaded = true;
            }
        }
        const replay_data::KeepSpan span = replay_data::keep_span_of(span_text);
        const int32_t today = local_when(static_cast<int64_t>(std::time(nullptr))).day;
        const std::vector<std::string> damaged_texts = damaged_kept_texts(folder);
        // While a damaged kept.json is set aside beside the new one, a mark
        // cut off with it cannot be known: every replay recorded by the time
        // it was set aside stays, and nobody is asked again. Later replays go
        // by the new kept.json (replay_data::spared_by_set_aside).
        const std::optional<int64_t> set_aside_at = replay_data::kept_set_aside_at(folder);
        std::size_t spared_by_damage = 0;
        std::vector<replay_data::Replay> due;
        for (const replay_data::Replay& replay : read.stored.replays) {
            const LocalWhen when = local_when(replay.recorded);
            if (!replay_data::due_to_go(replay, when.day, span, today))
                continue;
            if (replay_data::spared_by_set_aside(replay, set_aside_at)) {
                ++spared_by_damage;
                continue;
            }
            bool spared = false;
            for (const std::string& text : damaged_texts) {
                if (replay_data::spared_by_kept_bytes(text, replay.sha256, replay.path)) {
                    spared = true;
                    break;
                }
            }
            if (!spared)
                due.push_back(replay);
        }
        if (spared_by_damage != 0) {
            replays_log(
                std::to_string(spared_by_damage) +
                " old replays stay: they were recorded before the kept replays' file was set "
                "aside, so it may have kept them"
            );
        }
        if (due.empty()) {
            replay_retention_settled_ = true;
            return;
        }
        const std::string answer_text =
            preference_text(preference_values_, replays_retention_asked_preference);
        const bool answer_known =
            answer_text == replays_retention_keep || answer_text == replays_retention_let_go;
        const auto send_due = [this, &folder](std::vector<replay_data::Replay> rows) {
            if (rows.empty())
                return;
            auto* request = new ReplayTrashRequest();
            request->folder = folder;
            request->due = std::move(rows);
            replay_trash_due_.reset(request);
            begin_replay_trash_sweep();
        };
        if (answer_known) {
            send_due(std::move(due));
            replay_retention_settled_ = true;
            return;
        }
        kit::Question asked_question;
        asked_question.title = interface_sentence(library::words::replay_retention_title);
        asked_question.paragraphs.push_back({retention_body(due.size()), false});
        asked_question.buttons.push_back(
            {interface_sentence(library::words::replay_retention_keep_all),
             true,
             "keep-them-all",
             false,
             false}
        );
        asked_question.buttons.push_back(
            {interface_sentence(library::words::replay_retention_let_go),
             false,
             "let-them-go",
             false,
             true}
        );
        asked_question.primary_button = 0;
        // Escape does nothing: the question stays, and nothing is removed.
        asked_question.cancel_button = -1;
        const std::vector<replay_data::Replay> due_rows = std::move(due);
        QuestionScreen::Host answers;
        answers.answer = [this, due_rows](int32_t button) {
            if (button == 0) {
                preference_values_[std::string(replays_keep_preference)] =
                    std::string(replay_data::keep_forever);
                preference_values_[std::string(replays_retention_asked_preference)] =
                    std::string(replays_retention_keep);
                preferences_dirty_ = true;
                flush_preferences();
                replay_retention_settled_ = true;
                return true;
            }
            if (button != 1)
                return false;
            preference_values_[std::string(replays_retention_asked_preference)] =
                std::string(replays_retention_let_go);
            preferences_dirty_ = true;
            flush_preferences();
            auto* request = new ReplayTrashRequest();
            request->folder = user_folder_ / std::string(recordings_folder_name);
            request->due = due_rows;
            replay_trash_due_.reset(request);
            begin_replay_trash_sweep();
            replay_retention_settled_ = true;
            return true;
        };
        answers.tick = [this] { return screen_ != Screen::main_menu; };
        answers.closed = [this](const QuestionScreen&) { replay_retention_asking_ = false; };
        replay_retention_asking_ = true;
        OaLayer& layer = oa_layer();
        layer.push(
            std::make_unique<QuestionScreen>(
                layer, Screen::main_menu, std::move(asked_question), std::move(answers)
            )
        );
    };

    // A read already running is collected here. Forever does not abandon it:
    // the index it built is still saved, and nothing is removed.
    if (replay_retention_scan_ != nullptr) {
        bool finished = false;
        {
            base::threads::LockGuard held(replay_retention_scan_->mutex);
            finished = replay_retention_scan_->finished;
        }
        if (!finished)
            return;
        if (!on_menu() || blocked() || oa_layer_ == nullptr)
            return;
        RetentionRead read;
        {
            base::threads::LockGuard held(replay_retention_scan_->mutex);
            read.failed = replay_retention_scan_->failed;
            read.have_index = replay_retention_scan_->have_index;
            read.stored = std::move(replay_retention_scan_->stored);
        }
        replay_retention_scan_.reset();
        finish(std::move(read));
        return;
    }

    // Forever reads nothing.
    const std::string keep_text = preference_text(preference_values_, replays_keep_preference);
    if (replay_data::keep_span_of(keep_text) == replay_data::KeepSpan::forever) {
        replay_retention_settled_ = true;
        return;
    }

    if (!on_menu()) {
        if (!forced)
            replay_retention_frames_ = 0;
        return;
    }
    if (!forced && (++replay_retention_frames_ < kRetentionMenuFrames || blocked()))
        return;
    if (forced && (blocked() || oa_layer_ == nullptr))
        return;

    // The replays check calls this itself and needs the question on the next
    // frames, so that call reads on this thread. The start's own call does not.
    if (forced) {
        finish(scan_recordings(folder));
        return;
    }
    std::unique_ptr<ReplayRetentionScan, void (*)(ReplayRetentionScan*) noexcept> scan(
        new ReplayRetentionScan(), destroy_replay_retention_scan
    );
    scan->folder = folder;
    if (!base::threads::start_thread(scan->thread, &Runtime::retention_worker, scan.get())) {
        replays_log("old replays could not be read off the main thread");
        replay_retention_settled_ = true;
        return;
    }
    replay_retention_scan_ = std::move(scan);
}

void Runtime::LibraryHost::note_watched_replay(Runtime& runtime) {
    if (runtime.screen_ != Screen::match && runtime.match_ == nullptr)
        return;
    const auto expansion_state =
        runtime.preference_values_.find(std::string(replay_expansion_state_preference));
    const bool playing_switched =
        expansion_state != runtime.preference_values_.end() && expansion_state->second == "play";
    // The shelf is what remembers a place. A restart that plays the replay
    // never opens the Library first, so there is no shelf until this makes one.
    if (!replays) {
        if (!playing_switched)
            return;
        replay_shelf();
    }
    ReplayShelf& shelf = *replays;
    // A restart that switched the mod or the expansions opens the replay
    // itself. The Library's hooks are not on it. The row it was left on, or
    // the pending replay when that row was not carried, names the file.
    if (!shelf.watching && shelf.left_path.empty()) {
        if (playing_switched) {
            std::string path;
            if (runtime.library_return_ && runtime.library_return_->replay.has_value() &&
                runtime.library_return_->replay_source != library::ReplaySource::core_prime)
                path = *runtime.library_return_->replay;
            if (path.empty() && !runtime.pending_demo_path().empty() &&
                !runtime.user_folder_.empty()) {
                const fs::path recordings =
                    runtime.user_folder_ / std::string(recordings_folder_name);
                if (const std::optional<std::string> below = replay_path_below(
                        recordings, platform::path_from_utf8(runtime.pending_demo_path())
                    ))
                    path = *below;
            }
            if (!path.empty()) {
                shelf.left_path = std::move(path);
                shelf.left_finished = false;
                if (shelf.listed) {
                    remember_watch_length(shelf, shelf.index);
                } else if (!runtime.user_folder_.empty()) {
                    const fs::path folder =
                        runtime.user_folder_ / std::string(recordings_folder_name);
                    const replay_data::IndexRead read = replay_data::read_index(folder);
                    if (!read.damaged)
                        remember_watch_length(shelf, read.index);
                }
                // The pass leaves a replay that needs a package or an
                // expansion unread, so the description names the length.
                if (!shelf.watch_length.has_value() && !runtime.user_folder_.empty()) {
                    remember_recorded_length(
                        shelf, runtime.user_folder_ / std::string(recordings_folder_name)
                    );
                }
            }
        }
    }
    if (!shelf.watching && shelf.left_path.empty())
        return;
    if (runtime.match_finished_ || runtime.screen_ == Screen::campaign_end)
        shelf.left_finished = true;
    ReplayPlaybackView view{};
    if (!call_hook_or_report<&Extension::replay_state>(
            runtime.extension_, runtime.hook_error_report(), runtime, &view
        ))
        return;
    if (shelf.pending_seek) {
        const uint32_t jump = *shelf.pending_seek;
        shelf.pending_seek.reset();
        call_hook_or_report<&Extension::replay_seek>(
            runtime.extension_, runtime.hook_error_report(), runtime, jump
        );
        if (runtime.preference_values_.erase(std::string(replay_watch_from_preference)) > 0) {
            runtime.preferences_dirty_ = true;
            runtime.flush_preferences();
        }
    } else if (!shelf.watching) {
        const auto found =
            runtime.preference_values_.find(std::string(replay_watch_from_preference));
        if (found != runtime.preference_values_.end()) {
            const std::optional<uint32_t> jump = tick_text(found->second);
            runtime.preference_values_.erase(found);
            runtime.preferences_dirty_ = true;
            runtime.flush_preferences();
            if (jump.has_value()) {
                call_hook_or_report<&Extension::replay_seek>(
                    runtime.extension_, runtime.hook_error_report(), runtime, *jump
                );
            }
        }
    }
    if (shelf.watching && shelf.watching->status != nullptr) {
        RecordingStatus status{};
        try {
            shelf.watching->status(shelf.watching->context, status);
            if (status.finished)
                shelf.left_finished = true;
            if (shelf.watch_length.has_value() && status.tick > 0 &&
                status.tick >= *shelf.watch_length)
                shelf.left_finished = true;
        } catch (const std::exception&) {
        }
    }
    if (shelf.watch_length.has_value() && view.tick > 0 && view.tick >= *shelf.watch_length)
        shelf.left_finished = true;
    // A tick of 0 after one already past the start is the session's clock
    // once the match has gone, not where the replay was. A replay that had
    // finished stays finished.
    if (!shelf.left_finished &&
        !(view.tick == 0 && shelf.left_tick.has_value() && *shelf.left_tick > 0))
        shelf.left_tick = view.tick;
}

void Runtime::LibraryHost::stop_index_pass() noexcept {
    if (replays)
        stop_replay_index_pass(*replays);
}

bool Runtime::LibraryHost::index_pass_running() const noexcept {
#if !defined(__ANDROID__) && OA_PROCESS_SPAWNING
    return replays && replays->index_process != nullptr;
#else
    return false;
#endif
}

int Runtime::LibraryHost::index_pass_pid() const noexcept {
#if !defined(__ANDROID__) && OA_PROCESS_SPAWNING
    return replays != nullptr ? replays->index_pid : 0;
#else
    return 0;
#endif
}

bool Runtime::LibraryHost::watched_replay_finished() const {
    if (!replays)
        return false;
    if (replays->left_finished)
        return true;
    if (!replays->watching || replays->watching->status == nullptr)
        return false;
    RecordingStatus status{};
    try {
        replays->watching->status(replays->watching->context, status);
    } catch (const std::exception&) {
        return false;
    }
    if (status.finished)
        return true;
    return replays->watch_length.has_value() && status.tick > 0 &&
           status.tick >= *replays->watch_length;
}

namespace {

/// Saves the index after a check has written a timeline onto it.
///
/// @param user_folder the player's folder
/// @param shelf the shelf
void save_marked_index(const fs::path& user_folder, ReplayShelf& shelf) {
    if (user_folder.empty())
        return;
    const fs::path folder = user_folder / std::string(recordings_folder_name);
    if (!replay_data::save_index(folder, shelf.index))
        replays_log("the replays' index could not be saved");
}

} // namespace

void Runtime::LibraryHost::mark_replays_read(Runtime& runtime) {
    stop_index_pass();
    if (!replays || !replays->listed)
        return;
    bool changed = false;
    for (replay_data::Replay& replay : replays->index.replays) {
        const bool blank = replay.timeline.has_value() && replay.timeline->empty();
        if (blank && !replay.read_stamp.has_value())
            continue;
        replay.timeline = std::string();
        replay.read_stamp.reset();
        changed = true;
    }
    if (!changed)
        return;
    save_marked_index(runtime.user_folder_, *replays);
    if (replays_held)
        return;
    fill_replay_inputs(runtime, model.inputs);
    library::update_replays(model);
    ++model_revision;
}

void Runtime::LibraryHost::remember_watched_timeline(
    Runtime& runtime, const std::string& path, std::string text
) {
    if (path.empty() || text.empty() || !oademo::parse_timeline_index(text))
        return;
    const fs::path folder = runtime.user_folder_ / std::string(recordings_folder_name);
    const auto store_on = [&](replay_data::Replay& replay) {
        replay.timeline = text;
        replay.read_stamp.reset();
    };
    if (replays && replays->listed) {
        for (replay_data::Replay& replay : replays->index.replays) {
            if (replay.path != path)
                continue;
            store_on(replay);
            save_marked_index(runtime.user_folder_, *replays);
            return;
        }
    }
    replay_data::Index index = replay_data::load_index(folder);
    bool found = false;
    for (replay_data::Replay& replay : index.replays) {
        if (replay.path != path)
            continue;
        store_on(replay);
        found = true;
        break;
    }
    if (!found) {
        replay_data::Replay read = replay_data::read_file(folder, path);
        if (read.path.empty())
            read.path = path;
        store_on(read);
        index.replays.push_back(std::move(read));
        std::sort(
            index.replays.begin(),
            index.replays.end(),
            [](const replay_data::Replay& left, const replay_data::Replay& right) {
                return left.path < right.path;
            }
        );
    }
    if (!replay_data::save_index(folder, index))
        replays_log("the replays' index could not be saved");
    if (replays && replays->listed)
        replays->index = std::move(index);
}

std::string Runtime::LibraryHost::replay_read_stamp(Runtime& runtime) {
    return index_pass_content_stamp(runtime);
}

std::string Runtime::LibraryHost::timeline_of(const Runtime& runtime, const std::string& relative) {
    const auto from_index = [](const replay_data::Index& index, const std::string& path) {
        for (const replay_data::Replay& replay : index.replays) {
            if (replay.path != path || !replay.timeline.has_value() || replay.timeline->empty())
                continue;
            return *replay.timeline;
        }
        return std::string{};
    };
    if (replays && replays->listed)
        return from_index(replays->index, relative);
    const fs::path folder = runtime.user_folder_ / std::string(recordings_folder_name);
    return from_index(replay_data::load_index(folder), relative);
}

void Runtime::LibraryHost::store_replay_timeline(
    Runtime& runtime, const std::string& path, std::string text
) {
    stop_index_pass();
    if (!replays || !replays->listed)
        return;
    bool stored = false;
    for (replay_data::Replay& replay : replays->index.replays) {
        if (replay.path != path)
            continue;
        replay.timeline = std::move(text);
        replay.read_stamp.reset();
        stored = true;
        break;
    }
    if (!stored)
        return;
    save_marked_index(runtime.user_folder_, *replays);
    if (replays_held)
        return;
    fill_replay_inputs(runtime, model.inputs);
    library::update_replays(model);
    ++model_revision;
}

void Runtime::LibraryHost::end_watched_replay(Runtime& runtime) {
    const bool in_match = runtime.screen_ == Screen::match || runtime.match_ != nullptr;
    // A pack mounted for a pending replay stays until that match starts. The
    // first frame of the match drops the hold, so leaving the match still
    // clears it. The same empty path changes nothing.
    if (in_match)
        runtime.hold_replay_map_pack_ = false;
    else if (!runtime.hold_replay_map_pack_)
        runtime.use_replay_map_pack({});
    if (replays && (replays->watching || !replays->left_path.empty())) {
        if (in_match)
            return;
        // Reaching the end screen is the replay finished. The index is
        // written once the player has left that screen, so the card does
        // not keep a place from the last tick of a replay that ended.
        if (runtime.screen_ == Screen::campaign_end) {
            replays->left_finished = true;
            return;
        }
        ReplayShelf& shelf = *replays;
        const bool opened_here = shelf.watching.has_value();
        const std::optional<ReplayHooks> hooks = shelf.watching;
        shelf.watching.reset();
        shelf.pending_seek.reset();
        const std::optional<uint32_t> tick = shelf.left_tick;
        const bool finished = shelf.left_finished;
        const std::string path = shelf.left_path;
        shelf.left_tick.reset();
        shelf.left_finished = false;
        shelf.watch_length.reset();
        shelf.left_path.clear();
        bool changed = false;
        const fs::path folder = runtime.user_folder_ / std::string(recordings_folder_name);
        // A replay the Library opened has its index in memory. One a restart
        // opened does not: the index on disk is what the next Library reads.
        if (!path.empty() && (finished || tick.has_value())) {
            if (shelf.listed) {
                changed = store_watched_place(shelf.index, folder, path, tick, finished);
                if (changed)
                    save_marked_index(runtime.user_folder_, shelf);
            } else if (!runtime.user_folder_.empty()) {
                const replay_data::IndexRead read = replay_data::read_index(folder);
                if (!read.damaged) {
                    replay_data::Index stored = read.index;
                    if (store_watched_place(stored, folder, path, tick, finished)) {
                        if (!replay_data::save_index(folder, stored))
                            replays_log("the replays' index could not be saved");
                        else if (finished)
                            replays_log("left " + path + " at the end");
                        else
                            replays_log("left " + path + " at " + tick_decimal(*tick));
                    }
                }
            }
        }
        if (opened_here && hooks.has_value())
            call_hook_or_report<&ReplayHooks::close>(*hooks, runtime.hook_error_report());
        if (changed) {
            fill_replay_inputs(runtime, model.inputs);
            library::update_replays(model);
            ++model_revision;
        }
    }
    // A replay that turned expansions on, or played another mod, puts the
    // player's set back once the player has left its end screen, including
    // one the next run started after a restart. A switch still waiting to
    // play is left. The row the Library showed crosses that restart.
    if (!in_match && ReplayContentAccess::restore_replay_expansions(runtime) &&
        runtime.library_return_)
        runtime.library_kept_ = runtime.library_return_;
}

} // namespace oa::app
