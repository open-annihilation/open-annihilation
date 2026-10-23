// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Library's host (Runtime::LibraryHost), which runtime_library.cpp
// drives: the Library's model and its screen's state, the feed its inputs
// come from (LibraryFeed), the pictures it has taken, the updates it keeps
// for the queue, and its worker (LibraryWork), which finds the installed
// packages and their updates and reads and fetches the badge pictures off
// the main thread. The Library itself is a screen of the OA layer
// (Runtime::LibraryScreen, named library).
#pragma once

#include "oa/app/content/downloads.hpp"
#include "oa/app/content/pictures.hpp"
#include "oa/app/content/service.hpp"
#include "oa/app/content/updates.hpp"
#include "oa/app/mod_summary.hpp"
#include "oa/app/runtime.hpp"
#include "oa/base/threads.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/data/brain_library/library.hpp"
#include "oa/data/catalogue/catalogue.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/formats/oabrain/manifest.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/dry_run.hpp"
#include "oa/data/mutators/verdicts.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/screen.hpp"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

/// The name the Library has on the OA layer.
inline constexpr std::string_view library_screen_name = "library";

/// Minimaps the Library has decoded. Defined in runtime_library.cpp: this
/// header stays free of the preview cache, which the rest of the app does
/// not include. The host holds them because the Library's inputs are built
/// here and the screen asks for the rows it shows.
struct MapPreviewState;

/// Where the Library's inputs come from, and where its downloads go. The
/// runtime fills it from the content service, the Library's worker and the
/// download queue (queue_download); --check-library fills it with fixtures
/// and starts no download.
struct LibraryFeed {
    void* context{}; ///< passed back to each function
    /// Returns everything the Library is built from now. Null shows an empty
    /// Library.
    oa::ui::library::Inputs (*inputs)(void* context){};
    /// Returns a count that moves on whenever inputs would give something
    /// else. Null never moves: the Library is built once.
    uint64_t (*generation)(void* context){};
    /// Queues one package of a registry for download, GET's or UPDATE's.
    /// Null queues nothing.
    ///
    /// Returns the queue item, or nothing when it was refused, why saying
    /// why when it is not null.
    std::optional<uint64_t> (*enqueue)(
        void* context,
        std::string_view registry,
        std::string_view key,
        content::DownloadReason reason,
        std::string* why
    ){};
    /// Cancels the download of one package of a registry that has not ended.
    /// Null cancels nothing.
    void (*cancel)(void* context, std::string_view registry, std::string_view key){};
    /// Queues one package's failed download of a registry again. Null queues
    /// nothing.
    void (*retry)(void* context, std::string_view registry, std::string_view key){};
    /// Tells whether a registry is built in: the Registry::built_in inputs
    /// gives it. Null tells that none is.
    bool (*built_in)(void* context, std::string_view registry){};
    /// The verdict remembered for an installed mutator with its default
    /// options against the mod being played. Called on the main thread.
    /// Null remembers none.
    std::optional<oa::data::mutators::VerdictRecord> (*remembered_verdict)(
        void* context, std::string_view id
    ){};
    /// Starts a check of an installed mutator with its default options
    /// against the mod being played, and returns the work the Library's
    /// worker runs. Called on the main thread. The work gives the verdict,
    /// or nothing when it is cancelled. An empty function means the check
    /// could not start. Null: the host checks nothing, and the Library
    /// leaves every verdict unset.
    std::function<std::optional<oa::data::mutators::VerdictRecord>(
        const std::atomic<bool>& cancel
    )> (*start_check)(void* context, std::string_view id){};
    /// Keeps a verdict the worker found, for the installed mutator with its
    /// default options against the mod being played. Called on the main
    /// thread. Null keeps nothing.
    void (*keep_verdict)(
        void* context, std::string_view id, const oa::data::mutators::VerdictRecord& record
    ){};
};

/// One mutator that is On, for the list a check is about.
///
/// A member of that list whose id is here takes these options and this clash
/// mode. Every other member takes its defaults.
struct MutatorCheckChoice {
    /// The mutator's id.
    std::string id{};
    /// Each option's id and the text of its value, in the order shown.
    std::vector<std::pair<std::string, std::string>> options{};
    /// The clash mode in force. Empty when the player did not choose one.
    std::optional<oa::formats::oamut::ClashMode> clashes{};
};

/// One check handed to the Library's worker.
struct MutatorCheck {
    /// The mutator's id.
    std::string id{};
    /// The work the worker runs, not the main thread. Empty when the main
    /// thread still has to call `start_check` for a check that is waiting.
    std::function<std::optional<oa::data::mutators::VerdictRecord>(const std::atomic<bool>&)>
        work{};
    /// The key the job carries, kept with it until the verdict is kept or the
    /// check is dropped. Empty for a fixture's check, which keeps nothing of
    /// its own.
    std::optional<oa::data::mutators::VerdictKey> key{};
};

/// What a check left for the main thread.
struct MutatorCheckOutcome {
    /// The mutator's id.
    std::string id{};
    /// The check was cancelled, or gave no verdict. Nothing is kept.
    bool cancelled{};
    /// The verdict, when the check finished and was not cancelled.
    std::optional<oa::data::mutators::VerdictRecord> record{};
    /// The key the job carried. Empty when the check had none.
    std::optional<oa::data::mutators::VerdictKey> key{};
};

/// The Library's worker: one thread of its own, started at its first job and
/// joined as the host goes, which finds the installed packages and the
/// updates the catalogues offer for them after a catalogue refresh or an
/// install, and reads and fetches the badge pictures asked for. What it
/// found is kept under its lock for the main thread to read.
struct LibraryWork {
    base::threads::Mutex
        mutex{}; ///< guards every member below but cancel, check_cancel and pictures
    base::threads::ConditionVariable wake{}; ///< signalled when a job is due or the worker stops
    base::threads::Thread thread{};          ///< the worker, once started
    bool started{};                          ///< the worker was started
    bool stop{};                             ///< the worker stops at its next wait
    /// Set as the worker stops: a picture's fetch ends within 50 milliseconds.
    std::atomic<bool> cancel{false};
    /// Set when a match starts, so a running check stops. Distinct from
    /// `cancel`, which also stops the worker and picture fetches.
    std::atomic<bool> check_cancel{false};
    /// The next search for updates is due: list the installed packages
    /// again, match them to the catalogues and find their updates.
    bool updates_due{};
    std::shared_ptr<const content::Snapshot> snapshot{}; ///< the catalogues the next search reads
    std::filesystem::path user_folder{};                 ///< the player's folder it lists
    /// The packages the last search found installed, in list_installed's order.
    std::vector<content::InstalledPackage> installed{};
    std::vector<content::Match> matches{};  ///< the catalogue release each is matched to
    std::vector<content::Update> updates{}; ///< the updates the catalogues offer for them
    std::vector<ModBadge>
        installed_badges{}; ///< each installed package's own picture; empty for none
    /// Each installed package's mutator facts, in `installed`'s order. Unset
    /// for every other kind. The names are empty until the main thread
    /// resolves them from the other inputs.
    std::vector<std::optional<oa::ui::library::MutatorFacts>> installed_mutators{};
    /// The Mutators folder as the last search listed it.
    oa::data::mutator_library::Listing mutator_listing{};
    /// Each installed package's brain facts, in `installed`'s order. Unset
    /// for every other kind. The names are empty until the main thread
    /// resolves them from the other inputs.
    std::vector<std::optional<oa::ui::library::BrainFacts>> installed_brains{};
    /// The Brains folder as the last search listed it. Read on the worker.
    oa::data::brain_library::Listing brain_listing{};
    uint64_t searches{};  ///< counts the searches done
    bool pictures_due{};  ///< pictures wait to be read or fetched
    bool match_running{}; ///< a match runs: nothing is fetched
    /// A check is due. The worker runs `check_job`, or records a cancelled
    /// outcome when a match has started.
    bool check_due{};
    /// The check the worker runs. Set on the main thread and taken by the worker.
    std::optional<MutatorCheck> check_job{};
    /// The checks that wait until the running one ends, oldest first. One
    /// check runs at a time. A newer check of the same mutator replaces its
    /// waiting one; a check of another mutator waits behind it, so the
    /// picker's checks of several On mutators each run. Empty work means
    /// the main thread calls `start_check` when its turn comes. Work already
    /// set is run as it is.
    std::vector<MutatorCheck> checks_waiting{};
    /// The result the worker left for the main thread.
    std::optional<MutatorCheckOutcome> check_outcome{};
    /// The pictures asked for, read and fetched; its own lock guards it.
    std::unique_ptr<content::PictureCache> pictures{};
    /// The digests of the mutator folders that carry no package SHA-256.
    /// Each search makes them, so the main thread's Library and picker read
    /// no such folder again until it changes. Its own lock guards it.
    oa::data::mutators::FolderDigests mutator_digests{};
};

/// An update queued from the Library, kept so that the queue line still says
/// what it changes once its update note is gone.
struct KeptUpdate {
    std::string registry{};       ///< the package's registry
    oa::ui::library::Kind kind{}; ///< its kind
    std::string key{};            ///< its key
    /// Whether the update changes the rules saved games are tied to, as its
    /// update note said when it was queued.
    oa::ui::library::UpdateNote::Rules rules{oa::ui::library::UpdateNote::Rules::same};
    std::string from{}; ///< what ROLL BACK restores, as change_texts gives it
    std::string to{};   ///< what the update installs
};

/// The Library's host: its model and screen state, its feed, the pictures
/// it holds, the updates it keeps for the queue and its worker.
struct Runtime::LibraryHost {
    /// Makes the host, its worker not started.
    LibraryHost() = default;

    /// Stops the worker and joins it, within a second even while it fetches.
    ~LibraryHost();

    LibraryHost(const LibraryHost&) = delete;
    LibraryHost& operator=(const LibraryHost&) = delete;

    oa::ui::library::Library model{};     ///< the Library's model
    oa::ui::library::ScreenState state{}; ///< its screen's hover, focus, scroll and search
    LibraryFeed feed{};                   ///< where its inputs come from
    bool own_feed{true};                  ///< the feed is the runtime's own
    bool refreshed{};                     ///< the model was built from the feed once
    uint64_t feed_generation{};           ///< the feed's generation the model was built at
    uint64_t service_generation{};        ///< the content service's generation last searched at
    uint64_t queue_generation{};          ///< the download queue's generation last read
    uint64_t model_revision{};            ///< counts the model's rebuilds
    std::size_t update_count{};           ///< the updates that can be installed (update_count)
    /// The pictures taken from the worker, by address: badges and registry
    /// icons. They stay while the host lives.
    std::map<std::string, oa::ui::library::Badge> badges{};
    /// The logo the game ships, decoded once for a built-in registry that
    /// names it. Never fetched.
    std::optional<oa::ui::library::Badge> shipped_logo{};
    bool shipped_logo_tried{}; ///< true once the logo was decoded, or the decode failed
    /// Minimaps decoded for the rows on screen. Null until the host is made.
    /// The type lives in runtime_library.cpp.
    std::unique_ptr<MapPreviewState> previews{};
    /// Each listing's picture's address, by registry and key, as the last
    /// inputs resolved it.
    std::map<std::pair<std::string, std::string>, std::string> picture_urls{};
    std::vector<KeptUpdate> kept_updates{};  ///< the updates queued from the Library
    std::vector<uint64_t> installed_items{}; ///< the queue's items seen installed
    /// The installed-data reading a finished queue item was first listed
    /// against, by the queue's item id. A later reading of the installed
    /// packages no longer lets that item hide its update.
    std::map<uint64_t, uint64_t> finished_scans{};
    LibraryWork work{}; ///< the worker and what it found
    /// The mutator whose check is running. Empty when none is.
    std::string checking_id{};
    /// A check was handed to the worker and has not come back.
    bool check_open{};

    /// A verdict read from the file, or found missing, by the key of its list.
    /// A dry run that is kept replaces that key's entry. The main thread only.
    struct CachedVerdict {
        oa::data::mutators::VerdictKey key{}; ///< the list, the mod and the engine version
        /// The stored record. Nothing means the file held none for this key.
        std::optional<oa::data::mutators::VerdictRecord> record{};
    };

    std::vector<CachedVerdict> verdict_cache{};
    /// Moves when a verdict is kept, so the picker rebuilds its list.
    uint64_t verdict_generation{};
    /// The key `start_check` just prepared, taken by the host onto the job.
    /// The main thread only.
    std::optional<oa::data::mutators::VerdictKey> prepared_key{};
    /// The key of the verdict `keep_verdict` is about to keep. The main thread only.
    std::optional<oa::data::mutators::VerdictKey> delivered_key{};

    /// How many times a Library rebuild, or the picker's own list, listed the
    /// Mutators folder to resolve the verdicts it shows. The worker's search
    /// for updates is not counted. A check reads this.
    uint64_t shown_listings{0};
    /// True while the pass below holds: the listing, the verdict file and
    /// each row's key were made, and nothing they rest on changed since.
    bool shown_ready{false};
    /// True once this rebuild has asked whether the pass still holds. Each
    /// rebuild clears it.
    bool shown_checked{false};
    /// The worker's search count the pass listed the folder at.
    uint64_t shown_searches{};
    /// The installed mutators the rebuild was given when the pass was made:
    /// each one's key, folder, version and release.
    std::string shown_installed{};
    /// The listing every shown list resolves from.
    oa::data::mutator_library::Listing shown_listing{};
    /// The mod digest shared by every verdict key of the pass.
    oa::base::sha256::Digest shown_mod{};
    /// The clash mode the pass's keys were made with.
    oa::formats::oamut::ClashMode shown_clash{};
    /// The verdict file, read once for the pass. A verdict kept later is in
    /// `verdict_cache`.
    std::vector<oa::data::mutators::RememberedVerdict> shown_stored{};

    /// One row's verdict key and whether its list declares the maps layer.
    struct ShownRow {
        /// The key of the row's list; nothing when the list cannot be resolved.
        std::optional<oa::data::mutators::VerdictKey> key{};
        bool maps{}; ///< the last mutator of the list declares the maps layer
    };

    /// Each row's key, by mutator id, made once for the pass.
    std::map<std::string, ShownRow, std::less<>> shown_rows{};
    /// Whether the last mutator of the list resolved for an id declares the
    /// maps layer.
    std::map<std::string, bool, std::less<>> shown_maps{};

    /// Starts the worker when it has not started.
    ///
    /// @return false when the worker could not start
    bool ensure_worker();

    /// Asks the worker to search for updates again over the content
    /// service's snapshot now: after a catalogue refresh, an install or a
    /// roll back. The runtime's own feed only.
    ///
    /// @param runtime the runtime
    void look_again(Runtime& runtime);

    /// Builds the model from the feed's inputs, as they are now, and keeps
    /// the update count; the screen lays itself out again at its next tick.
    ///
    /// @param runtime the runtime
    void rebuild(Runtime& runtime);

    /// Marks one installed mutator as checking and hands `start_check`'s
    /// work to the worker. While a check runs, this id waits its turn,
    /// replacing a waiting check of the same mutator, and `start_check` is
    /// called on the main thread when its turn comes. An empty function from
    /// `start_check` marks nothing.
    /// The model is rebuilt so the details say the check is running.
    ///
    /// @param runtime the runtime
    /// @param id the mutator's id
    void begin_mutator_check(Runtime& runtime, std::string_view id);

    /// Hands work already prepared to the worker. While a check runs, this
    /// one waits its turn, replacing a waiting check of the same mutator.
    /// The model is rebuilt when this check starts now.
    ///
    /// @param runtime the runtime
    /// @param prepared the id, the work and the key the job carries
    void enqueue_prepared_check(Runtime& runtime, MutatorCheck prepared);

    /// Takes a check the worker finished. A verdict it gave is kept, on the
    /// main thread. A cancelled check is not kept, and the checks that were
    /// waiting are dropped when a match cancelled this one. Otherwise the
    /// oldest waiting check is started. Nothing is rebuilt here.
    ///
    /// @return true when a check ended, so the caller rebuilds the model
    bool finish_mutator_check();

    /// Asks for the pictures of the rows in view and a few rows beyond each
    /// edge, the selected row's first, and for a registry icon the Registries
    /// tab or a From line is showing. Wakes the worker while any waits.
    /// The runtime's own feed only; nothing while a match runs. A picture
    /// already asked for, held or refused is not asked for again. A map
    /// pack's catalogue preview is asked for the same rows. One picture
    /// cache serves badges, map previews and registry icons.
    ///
    /// @param list the display list the Library shows
    void request_pictures(const oa::ui::kit::DisplayList& list);

    /// Returns the logo the game ships, decoded once from the app icon.
    ///
    /// The visible part of the window icon, so the plates fill the card.
    /// Nothing is fetched. Empty when the icon cannot be decoded, and then
    /// the lettermark stays.
    ///
    /// @return the logo; null when it could not be decoded
    const oa::ui::library::Badge* shipped_logo_badge();

    /// Asks the picture cache for one picture and wakes the worker. Nothing
    /// while a match runs. A picture already asked for, held or refused is
    /// not asked for again.
    ///
    /// @param registry the registry the catalogue names it under
    /// @param url its address, resolved against the host that served the catalogue
    void ask_cached_picture(std::string_view registry, std::string_view url);

    /// Returns a picture the cache has handed over, taking it into `badges`
    /// the first time. The caller gets a copy.
    ///
    /// @param registry the registry it was asked for under
    /// @param url its address, as asked for
    /// @return the picture; nothing while it is not ready
    [[nodiscard]] std::optional<oa::ui::library::Badge>
    cached_picture(std::string_view registry, std::string_view url);

    /// The picture source's known: an entry whose badge and map previews
    /// are held, already asked for or refused.
    ///
    /// @param context the host
    /// @param entry the entry
    /// @return true when nothing of it remains to ask for
    static bool picture_known(void* context, const oa::ui::library::Entry& entry);

    /// The picture source's request: the entry's badge, and a map pack's
    /// catalogue previews. The lead, the selected row, is fetched first.
    ///
    /// @param context the host
    /// @param entry the entry
    /// @param lead the selected row
    static void picture_request(void* context, const oa::ui::library::Entry& entry, bool lead);

    /// The registry-icon source's known: a shipped logo, an empty address,
    /// a picture already held, or one already asked for or refused.
    ///
    /// @param context the host
    /// @param registry the registry
    /// @return true when nothing remains to ask for
    static bool registry_icon_known(void* context, const oa::ui::library::Registry& registry);

    /// The registry-icon source's request. A shipped logo is never asked for.
    ///
    /// @param context the host
    /// @param registry the registry
    static void registry_icon_request(void* context, const oa::ui::library::Registry& registry);

    /// Decodes a few minimaps for the rows this list shows, into `previews`.
    ///
    /// An installed map uses its preview picture, or its terrain file's
    /// minimap. A catalogue picture uses bytes the worker has already handed
    /// over. One terrain file is decoded per call, or a few pictures when
    /// none is waiting, so a frame does not decode every map.
    ///
    /// @param runtime the runtime, whose game palette colours a terrain minimap
    /// @param list the display list the Library shows
    void pump_map_previews(Runtime& runtime, const oa::ui::kit::DisplayList& list);

    /// The runtime's own feed: the Library's inputs (Runtime::library_inputs).
    ///
    /// @param context the runtime
    /// @return the inputs
    static oa::ui::library::Inputs own_inputs(void* context);

    /// The runtime's own feed: what moves whenever the inputs would change
    /// (Runtime::library_feed_generation).
    ///
    /// @param context the runtime
    /// @return the count
    static uint64_t own_generation(void* context);

    /// The runtime's own feed: queues a package (Runtime::queue_download).
    ///
    /// @param context the runtime
    /// @param registry the registry's id
    /// @param key the package's key
    /// @param reason why the player asked
    /// @param[out] why why it was not queued; may be null
    /// @return the queue item; nothing when it was refused
    static std::optional<uint64_t> own_enqueue(
        void* context,
        std::string_view registry,
        std::string_view key,
        content::DownloadReason reason,
        std::string* why
    );

    /// The runtime's own feed: cancels the latest download of a package that
    /// has not ended.
    ///
    /// @param context the runtime
    /// @param registry the registry's id
    /// @param key the package's key
    static void own_cancel(void* context, std::string_view registry, std::string_view key);

    /// The runtime's own feed: queues the latest failed or cancelled
    /// download of a package again.
    ///
    /// @param context the runtime
    /// @param registry the registry's id
    /// @param key the package's key
    static void own_retry(void* context, std::string_view registry, std::string_view key);

    /// The runtime's own feed: whether a registry is built in, from the
    /// content service's snapshot: its origin is built-in, as the inputs'
    /// Registry::built_in says it.
    ///
    /// @param context the runtime
    /// @param registry the registry's id
    /// @return true for a registry that ships with the game
    static bool own_built_in(void* context, std::string_view registry);

    /// The runtime's own feed: the verdict the file holds for an installed
    /// mutator with its default options, prerequisites included. A list that
    /// cannot be resolved remembers nothing.
    ///
    /// @param context the runtime
    /// @param id the mutator's id
    /// @return the stored record, or nothing
    static std::optional<oa::data::mutators::VerdictRecord>
    own_remembered_verdict(void* context, std::string_view id);

    /// The runtime's own feed: prepares a dry run of an installed mutator
    /// with its default options and returns the work the Library's worker
    /// runs. An empty function means the list cannot be resolved, or a match
    /// is in the way. The work calls `run_dry_run` and touches no runtime.
    ///
    /// @param context the runtime
    /// @param id the mutator's id
    /// @return the work, or an empty function
    static std::function<std::optional<oa::data::mutators::VerdictRecord>(const std::atomic<bool>&)>
    own_start_check(void* context, std::string_view id);

    /// The runtime's own feed: keeps a verdict the worker found, under the
    /// key the job carried, and moves the picker's generation on.
    ///
    /// @param context the runtime
    /// @param id the mutator's id
    /// @param record the verdict
    static void own_keep_verdict(
        void* context, std::string_view id, const oa::data::mutators::VerdictRecord& record
    );

    /// The list a check is about: the mutator's prerequisites, then the
    /// mutator. Members named in `on` take those options. A cycle, or a
    /// prerequisite that is not installed, returns nothing. The listing is
    /// the one the caller already made; this does not list the folder again.
    ///
    /// @param listing the installed mutators
    /// @param id the mutator's id
    /// @param on the On mutators, each with the options it is shown with
    /// @param digests the folder digests kept so far; null makes each afresh
    /// @return the list, or nothing when it cannot be resolved
    [[nodiscard]] static std::optional<std::vector<oa::data::mutators::ResolvedMutator>>
    resolve_shown_list(
        const oa::data::mutator_library::Listing& listing,
        std::string_view id,
        std::span<const MutatorCheckChoice> on,
        oa::data::mutators::FolderDigests* digests = nullptr
    );

    /// The remembered verdict for this list, from the cache or `stored`.
    /// The mod digest was computed once for the rebuild. A miss stores the
    /// file's answer, including none. This does not prepare a dry run.
    ///
    /// @param runtime the runtime
    /// @param list the mutators, prerequisites included
    /// @param mod_digest the running mod's key
    /// @param stored the verdict file, already read
    /// @return the stored record, or nothing
    [[nodiscard]] static std::optional<oa::data::mutators::VerdictRecord> verdict_for(
        Runtime& runtime,
        std::span<const oa::data::mutators::ResolvedMutator> list,
        const oa::base::sha256::Digest& mod_digest,
        std::span<const oa::data::mutators::RememberedVerdict> stored
    );

    /// The remembered verdict for a key, from the cache or `stored`. A miss
    /// stores the file's answer, including none.
    ///
    /// @param runtime the runtime
    /// @param key the list's key
    /// @param stored the verdict file, already read
    /// @return the stored record, or nothing
    [[nodiscard]] static std::optional<oa::data::mutators::VerdictRecord> verdict_for_key(
        Runtime& runtime,
        const oa::data::mutators::VerdictKey& key,
        std::span<const oa::data::mutators::RememberedVerdict> stored
    );

    /// Keeps the pass, or makes it again: the listing is taken again, the
    /// worker's from its last search for the runtime's own feed or a new one
    /// for any other, and the verdict file read, when a search finished or
    /// the installed mutators changed since; the rows' keys are made again
    /// when the mod key or the clash mode changed too. A later call in the
    /// same rebuild does nothing, so a rebuild that changes none of those
    /// lists nothing and reads no package.
    ///
    /// @param runtime the runtime
    void ensure_shown_pass(Runtime& runtime);

    /// Prepares a dry run of one mutator with default options and leaves the
    /// key on `prepared_key`. An empty function means it cannot start.
    ///
    /// @param runtime the runtime
    /// @param id the mutator's id
    /// @return the work the worker runs, or an empty function
    [[nodiscard]] static std::function<
        std::optional<oa::data::mutators::VerdictRecord>(const std::atomic<bool>&)>
    start_default_check(Runtime& runtime, std::string_view id);

    /// Keeps the verdict `delivered_key` names. A cancelled verdict is not kept.
    ///
    /// @param runtime the runtime
    /// @param record the verdict
    static void keep_delivered(Runtime& runtime, const oa::data::mutators::VerdictRecord& record);

    /// The work a worker runs for one prepared job: `run_dry_run`, then the
    /// line a dry run logs. The function captures the job by value and
    /// touches no runtime.
    ///
    /// @param job the prepared run
    /// @param id the mutator's id, as the log line names it
    /// @return the work
    [[nodiscard]] static std::function<
        std::optional<oa::data::mutators::VerdictRecord>(const std::atomic<bool>&)>
    dry_run_work(oa::data::mutators::DryRunJob job, std::string id);

  private:

    /// Queues a check behind the running one, in place of a waiting check
    /// of the same mutator. The caller holds `work.mutex`.
    ///
    /// @param waiting the check
    void wait_turn(MutatorCheck waiting);

    /// Puts one check on the worker. The caller rebuilds the model.
    ///
    /// @param prepared the id, the work and the key
    /// @return false when the worker could not start, and the check is then dropped
    bool hand_to_worker(MutatorCheck prepared);
};

/// Returns where a queued download stands, as the Library's queue line says it.
///
/// @param state the queue's state
/// @return the phase; nothing for a cancelled item, which the lists leave out
inline std::optional<oa::ui::library::QueueItem::Phase>
phase_of(content::DownloadState state) noexcept {
    using Phase = oa::ui::library::QueueItem::Phase;
    switch (state) {
    case content::DownloadState::waiting:
    case content::DownloadState::asking:
    case content::DownloadState::challenge:
    case content::DownloadState::paused_for_match:
        return Phase::waiting;
    case content::DownloadState::downloading:
        return Phase::downloading;
    case content::DownloadState::verifying:
        return Phase::verifying;
    case content::DownloadState::handing_over:
    case content::DownloadState::installing:
        return Phase::installing;
    case content::DownloadState::failed:
        return Phase::failed;
    case content::DownloadState::done:
        return Phase::installed;
    case content::DownloadState::cancelled:
        return std::nullopt;
    }
    return std::nullopt;
}

/// Copies a mutator manifest's facts. Names stay empty for the caller to fill.
///
/// @param manifest the installed manifest
/// @return the facts
inline oa::ui::library::MutatorFacts facts_of(const oa::formats::oamut::Manifest& manifest) {
    oa::ui::library::MutatorFacts facts;
    facts.tags = manifest.tags;
    for (const oa::formats::oamut::Condition& condition : manifest.works_with) {
        oa::ui::library::WorksWith item;
        item.id = condition.id;
        item.range = condition.range_text;
        facts.works_with.push_back(std::move(item));
    }
    for (const oa::formats::oamut::Condition& condition : manifest.requires_mutators) {
        oa::ui::library::Prerequisite need;
        need.id = condition.id;
        need.range = condition.range_text;
        facts.needs.push_back(std::move(need));
    }
    for (const oa::formats::oamut::Condition& condition : manifest.conflicts_with)
        facts.conflicts.push_back(condition.id);
    for (const oa::formats::oamut::Layer layer : manifest.layers)
        facts.layers.emplace_back(oa::formats::oamut::layer_name(layer));
    for (const oa::formats::oamut::Option& option : manifest.options) {
        oa::ui::library::OptionLine line;
        line.label = option.label;
        if (!option.value_labels.empty())
            line.values = option.value_labels;
        else
            for (const oa::formats::oamut::ChoiceValue& value : option.values)
                line.values.push_back(value.text);
        facts.options.push_back(std::move(line));
    }
    return facts;
}

/// Copies a catalogue mutator's facts. Names stay empty for the caller to fill.
///
/// @param package the catalogue entry
/// @return the facts
inline oa::ui::library::MutatorFacts facts_of(const oa::data::catalogue::Package& package) {
    oa::ui::library::MutatorFacts facts;
    facts.tags = package.tags;
    for (const std::string& written : package.works_with) {
        const std::optional<oa::formats::oamut::Condition> condition =
            oa::formats::oamut::parse_condition(written);
        if (!condition)
            continue;
        oa::ui::library::WorksWith item;
        item.id = condition->id;
        item.range = condition->range_text;
        facts.works_with.push_back(std::move(item));
    }
    for (const oa::data::catalogue::MutatorRequirement& requirement : package.requires_mutators) {
        oa::ui::library::Prerequisite need;
        need.id = requirement.id;
        need.range = requirement.range_text;
        facts.needs.push_back(std::move(need));
    }
    for (const oa::data::catalogue::MutatorRequirement& requirement : package.conflicts_with)
        facts.conflicts.push_back(requirement.id);
    for (const oa::formats::oamut::Layer layer : package.layers)
        facts.layers.emplace_back(oa::formats::oamut::layer_name(layer));
    for (const oa::data::catalogue::OptionSummary& option : package.options) {
        oa::ui::library::OptionLine line;
        line.label = option.label;
        if (!option.value_labels.empty())
            line.values = option.value_labels;
        else
            for (const oa::data::catalogue::ChoiceValueText& value : option.values)
                line.values.push_back(value.text);
        facts.options.push_back(std::move(line));
    }
    return facts;
}

/// Maps a manifest difficulty onto the Library's, in the same order.
///
/// @param difficulty the manifest's difficulty
/// @return the Library's
inline oa::ui::library::BrainDifficulty
brain_difficulty_of(oa::formats::oabrain::Difficulty difficulty) noexcept {
    switch (difficulty) {
    case oa::formats::oabrain::Difficulty::easy:
        return oa::ui::library::BrainDifficulty::easy;
    case oa::formats::oabrain::Difficulty::medium:
        return oa::ui::library::BrainDifficulty::medium;
    case oa::formats::oabrain::Difficulty::hard:
        return oa::ui::library::BrainDifficulty::hard;
    }
    return oa::ui::library::BrainDifficulty::easy;
}

/// Copies an installed brain's facts. Names stay empty for the caller to fill.
///
/// @param manifest the installed manifest
/// @param network_bytes the networks' files, in bytes, already measured
/// @return the facts
inline oa::ui::library::BrainFacts
brain_facts_of(const oa::formats::oabrain::Manifest& manifest, uint64_t network_bytes) {
    oa::ui::library::BrainFacts facts;
    facts.tags = manifest.tags;
    for (const oa::formats::oabrain::Level& level : manifest.levels) {
        oa::ui::library::BrainLevel line;
        line.id = level.id;
        line.name = level.name;
        line.difficulty = brain_difficulty_of(level.difficulty);
        facts.levels.push_back(std::move(line));
    }
    for (const oa::formats::oamut::Condition& condition : manifest.works_with) {
        oa::ui::library::WorksWith item;
        item.id = condition.id;
        item.range = condition.range_text;
        facts.works_with.push_back(std::move(item));
    }
    facts.requires_engine = manifest.requires_engine;
    facts.requires_expansions = manifest.requires_expansions;
    facts.full_awareness = manifest.full_awareness;
    facts.network_bytes = network_bytes;
    return facts;
}

/// Copies a catalogue brain's facts. Names stay empty for the caller to fill.
///
/// @param package the catalogue entry
/// @return the facts
inline oa::ui::library::BrainFacts brain_facts_of(const oa::data::catalogue::Package& package) {
    oa::ui::library::BrainFacts facts;
    facts.tags = package.tags;
    for (const oa::data::catalogue::BrainLevel& level : package.levels) {
        oa::ui::library::BrainLevel line;
        line.id = level.id;
        line.name = level.name;
        line.difficulty = brain_difficulty_of(level.difficulty);
        facts.levels.push_back(std::move(line));
    }
    for (const std::string& written : package.works_with) {
        const std::optional<oa::formats::oamut::Condition> condition =
            oa::formats::oamut::parse_condition(written);
        if (!condition)
            continue;
        oa::ui::library::WorksWith item;
        item.id = condition->id;
        item.range = condition->range_text;
        facts.works_with.push_back(std::move(item));
    }
    facts.requires_engine = package.requires_engine_text;
    facts.requires_expansions = package.required_expansions;
    facts.full_awareness = package.full_awareness;
    facts.network_bytes = package.network_bytes;
    return facts;
}

/// Returns the Library's tab a tab of the settings dialog names.
///
/// @param tab the dialog's tab (Dialog::library_tab)
/// @return the Library's
[[nodiscard]] oa::ui::library::Tab library_tab_of(oa::ui::engine_settings::LibraryTab tab) noexcept;

} // namespace oa::app
