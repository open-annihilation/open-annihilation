// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Library's model: one list of every package the player can get or has
// installed, built from plain inputs, with its tabs, filters, search,
// selection, details and download queue lines. It draws nothing and reads no
// file, clock or network; the app fills the inputs and the screens show the
// result. Its words are in text.hpp.
#pragma once

#include "oa/base/sha256.hpp"
#include "oa/formats/oamod/package_keys.hpp"

#include <array>
#include <compare>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::library {

/// What kind of package an entry is.
enum class Kind : uint8_t {
    mod,        ///< an .oamod
    map_pack,   ///< an .oamap
    language,   ///< an .oalang
    mutator,    ///< an .oamut
    brain,      ///< an .oabrain
    key_layout, ///< an .oakeys
};

/// One signing key a registry's catalogue is checked against.
struct SigningKey {
    /// The fingerprint as the game shows one, "7001 · af50 · b971 · 5bb4".
    std::string fingerprint{};
    /// The full public key, "ed25519:" and its base64.
    std::string public_key{};
};

/// A package's badge picture, 8-bit RGBA, rows top to bottom. A registry's
/// icon uses the same picture.
struct Badge {
    std::vector<uint8_t> rgba{}; ///< width * height * 4 bytes
    uint32_t width{};            ///< in pixels, at most 64
    uint32_t height{};           ///< in pixels, at most 64
};

/// A registry the Library lists packages from.
///
/// `built_in` is true when the registry ships with Open Annihilation (its
/// descriptor is in the package's registries folder). It is the only thing
/// the model reads to decide whether a registry is built in.
struct Registry {
    std::string id{};          ///< the registry's id
    std::string name{};        ///< the short name: notes, "From", and downloads off
    std::string title{};       ///< the card's heading; name when empty
    std::string description{}; ///< the paragraph under the address and refresh lines
    bool built_in{};           ///< shipped with Open Annihilation; its downloads never ask
    /// False when the registry needs an install ID and the player turned it off.
    bool downloads_on{true};
    /// Why downloads are off, in the player's words; empty when they are on.
    std::string downloads_off_reason{};
    /// The registry's own switch: false when the player turned the registry off.
    bool on{true};
    /// Where the registry was read from: the address it was added from, or
    /// its catalogue's address when it was not added from one.
    std::string address{};
    std::size_t packages{}; ///< the packages the catalogue kept for it lists
    /// How long ago a refresh last found a usable catalogue, in seconds.
    /// None when no refresh ever has.
    std::optional<int64_t> checked_ago{};
    std::size_t mirrors{}; ///< the mirrors the player added
    bool unreachable{};    ///< the last refresh found nothing usable
    /// True while this registry is being asked for its catalogue.
    bool refreshing{};
    /// The keys its signatures are checked against, in the descriptor's order.
    std::vector<SigningKey> keys{};
    /// The descriptor's author. A built-in registry shows a maker's mark
    /// when this is Open Annihilation or Cavedog Entertainment.
    std::string author{};
    /// The registry's icon, when the app has it. The lettermark shows while
    /// this is empty. A display list's picture points into these pixels,
    /// which stay until the next refresh, as a badge's do.
    std::optional<Badge> icon{};
    /// The catalogue picture's address. Empty when the catalogue names none,
    /// and for the logo the game ships, which is never fetched.
    std::string icon_url{};
    /// True when the card shows the logo the game ships. A catalogue icon
    /// does not replace it, and a picture request skips this registry.
    bool shipped_logo{};
};

/// A map's minimap, 8-bit RGBA, rows top to bottom. It keeps its own shape:
/// the screen fits it into a box and does not stretch it. Wider than a badge.
struct MapPicture {
    std::vector<uint8_t> rgba{}; ///< width * height * 4 bytes
    uint32_t width{};            ///< in pixels
    uint32_t height{};           ///< in pixels
};

/// One map of a map pack, as its catalogue entry or its manifest lists it.
struct MapLine {
    std::string name{}; ///< the title a player sees
    int32_t players{};  ///< how many players it is for
    std::string size{}; ///< its size as the catalogue writes it, such as "16x16"
    /// The manifest's description; empty when the catalogue lists the map,
    /// which names none, and when the manifest leaves it unwritten.
    std::string description{};
    std::string stem{};        ///< the map's file name without its extension
    std::string preview_url{}; ///< the catalogue picture's address; empty for none
    /// The minimap, when the app has decoded it. Points stay valid until the
    /// next refresh, as a badge's do.
    std::optional<MapPicture> preview{};
};

/// One mod a mutator works with, as its manifest or its catalogue entry writes it.
struct WorksWith {
    /// The mod's id, `*` for any mod, or `ta-3.1c` for the base game.
    std::string id{};
    /// The version range as written. Empty for any version.
    std::string range{};
    /// The name a player reads. Empty when the id is not known here.
    std::string name{};
};

/// One mutator another mutator needs.
struct Prerequisite {
    /// The mutator's id.
    std::string id{};
    /// The version range as written. Empty for any version.
    std::string range{};
    /// The name a player reads. Empty when no listing or installed copy names it.
    std::string name{};
};

/// One option a mutator offers, with the captions a player reads.
struct OptionLine {
    /// The option's label.
    std::string label{};
    /// Each value's caption, as the manifest gives it.
    std::vector<std::string> values{};
};

/// What a check of an installed mutator against the mod being played found.
///
/// Set on an installed copy when the host can check mutators. Unset when it
/// cannot, which leaves out the Checked fact and CHECK WITH THIS MOD.
struct Verdict {
    /// Where the check stands.
    enum class State : uint8_t {
        unchecked,    ///< not checked with this mod yet
        checking,     ///< a check is running
        compatible,   ///< it changes the mod
        no_effect,    ///< it applies and changes nothing
        incompatible, ///< it cannot run with this mod
        failed,       ///< the check itself did not finish
    };

    /// Where the check stands. A default verdict has not been checked.
    State state{State::unchecked};
    /// Why, for an incompatible or a failed check. Empty otherwise.
    std::string reason{};
    /// What a compatible check changed, in words. Empty otherwise.
    std::string summary{};
};

/// What a mutator's manifest or catalogue entry says about it.
struct MutatorFacts {
    /// The tag ids, in the order written.
    std::vector<std::string> tags{};
    /// The mods it works with. Empty means any mod.
    std::vector<WorksWith> works_with{};
    /// The mutators it needs, in the order they apply.
    std::vector<Prerequisite> needs{};
    /// Mutator ids it cannot run beside.
    std::vector<std::string> conflicts{};
    /// The layers it uses, in the order written.
    std::vector<std::string> layers{};
    /// The settings a player picks, in the order written.
    std::vector<OptionLine> options{};
    /// The check against the mod being played. Set on an installed copy when
    /// the host can check mutators, and left unset when it cannot.
    std::optional<Verdict> verdict{};
};

/// How hard one of a brain's levels is, in the manifest's order.
enum class BrainDifficulty : uint8_t {
    easy,
    medium,
    hard,
};

/// One level a brain offers.
struct BrainLevel {
    /// The level's id.
    std::string id{};
    /// The name a player reads.
    std::string name{};
    /// How hard it is.
    BrainDifficulty difficulty{BrainDifficulty::easy};
};

/// What a brain's manifest or catalogue entry says about it.
///
/// `tags` is not on the design's fact list. An installed brain that no
/// registry lists has no catalogue tag list, and the chips and the search
/// read the tags from here, the installed manifest winning as the other
/// facts do.
struct BrainFacts {
    /// The tag ids, in the order written.
    std::vector<std::string> tags{};
    /// The levels, in the order written.
    std::vector<BrainLevel> levels{};
    /// The mods it works with. Empty means any mod.
    std::vector<WorksWith> works_with{};
    /// The engine requirement as written, such as ">= 0.8.0"; empty for none.
    std::string requires_engine{};
    /// Expansions that must be on. Empty when it names none.
    std::vector<std::string> requires_expansions{};
    /// The brain sees every unit, not only what its seat can.
    bool full_awareness{};
    /// The networks' files, in bytes. 0 when it names none.
    uint64_t network_bytes{};
    /// The check against the game being played. Set on an installed copy
    /// when the host can check brains, and left unset when it cannot.
    std::optional<Verdict> verdict{};
};

/// A key as a row of keycaps shows it: the keycaps' labels in order, and a
/// gesture word after them. No keycaps is no key.
struct KeyChord {
    std::vector<std::string> keys{}; ///< the keycaps' labels, modifiers first
    std::string gesture{};           ///< the gesture word, such as "hold"; empty for a press
};

/// One command whose key changes: a layout's own change, or what USE moves.
struct KeyMove {
    /// The command's English name, as the key editor lists it. It is looked
    /// up when it is shown.
    std::string command{};
    KeyChord before{}; ///< its first key now; empty when it has none
    KeyChord after{};  ///< its first key afterwards; empty when it will have none
    /// The key afterwards is one of the player's own changes.
    bool own{};
};

/// The most commands a list of moves or own changes holds. Its total counts
/// the rest.
inline constexpr std::size_t most_key_moves = 64;

/// What USE would move: the commands whose first key differs now and
/// afterwards, in the key editor's order, for each answer to the question
/// about the player's own changes.
///
/// `kept` is the layout with the own changes it would hold when they are kept
/// on top: its own, when it already has some, else those of the layout in
/// use. `clean` is the layout with its own changes alone. Nothing here is
/// written anywhere.
struct KeyMoves {
    /// The host read both layouts. Unset lists nothing, and the question
    /// leaves out what moves.
    bool known{};
    std::vector<KeyMove> kept{};  ///< with the own changes on top
    uint32_t kept_total{};        ///< how many commands `kept` moves, all of them
    std::vector<KeyMove> clean{}; ///< with the layout's own changes alone
    uint32_t clean_total{};       ///< how many commands `clean` moves, all of them
};

/// How the Library asks its host what USE would move. Empty asks nothing.
struct KeyUseHooks {
    /// Returns what changing from one layout to another moves. Each id is a
    /// package key, or "oa-default" for OA default.
    std::function<KeyMoves(std::string_view from, std::string_view to)> moves{};
};

/// What a key layout's manifest or catalogue entry says about it.
///
/// `tags` is read the way a brain's are: the chips and the search use them,
/// and an installed layout that no registry lists has no catalogue tag list.
/// The installed manifest wins when both are set. A layout never changes
/// the interface mode. `pointer` is only the scheme it was drawn for.
struct KeyLayoutFacts {
    /// The tag ids, in the order written.
    std::vector<std::string> tags{};
    /// The mods it is made for. Empty means any mod.
    std::vector<WorksWith> works_with{};
    /// The engine requirement as written, such as ">= 0.8.0"; empty for none.
    std::string requires_engine{};
    /// The layout this one changes: "oa-default", "none", or another id.
    std::string base{};
    /// The keyboard it was drawn for: "any", "ansi", "iso" or "jis".
    std::string keyboard{};
    /// The pointer scheme it was drawn for: "right-click" or "left-click".
    /// Empty when the file names none.
    std::string pointer{};
    /// Helpers it asks for. A catalogue listing stores the six names. An
    /// installed manifest stores one token a switch can turn on:
    /// "sticky-modifiers", "pointer-keys", "hold-to-confirm",
    /// "hold-to-confirm:<action>", "toggle:<action>", "edge-scroll:off",
    /// "radial-orders:hold-right" or "radial-orders:hold-middle".
    std::vector<std::string> helpers{};
    /// How many actions the layout names.
    uint32_t changes{};
    /// Triggers the keyboard picture marks, as the catalogue or the chords write them.
    std::vector<std::string> triggers{};
    /// Actions this version of the game does not know. Information only.
    uint32_t unknown_actions{};
    /// The player's own changes sitting on this layout. Not an entry of their own.
    uint32_t own_changes{};
    /// Those changes, each a command and its first key (`after`), in the key
    /// editor's order, at most most_key_moves. Empty when the host names
    /// none; the details then count them.
    std::vector<KeyMove> own_list{};
    /// This copy is the layout in use. The built-in layout reads Inputs instead.
    bool in_use{};
    /// "Made for {mod}" when works-with names the mod being played. Empty otherwise.
    std::string made_for{};
};

/// One package a registry's catalogue lists.
struct Listing {
    std::string registry{}; ///< the registry's id
    Kind kind{Kind::mod};
    std::string key{};     ///< the package key, unique in its registry
    std::string name{};    ///< the name a player sees
    std::string version{}; ///< the version as written; empty when the kind has none
    int64_t revision{};    ///< the packaging revision; 0 when it names none
    int64_t release{};     ///< the registry's release number
    std::string publisher{};
    std::string author{};
    std::string summary{};
    std::string homepage{}; ///< an address the game can open; empty for none
    std::vector<std::string> tags{};
    /// The engine requirement as written, such as ">= 0.8.0"; empty for none.
    std::string requires_engine{};
    std::string base{}; ///< the base game the package names, such as "ta-3.1c"; empty for none
    std::optional<base::sha256::Digest> sim_hash{}; ///< the rules hash, when the entry names one
    uint32_t game_hacks{};                          ///< how many hacks change the game
    uint32_t view_hacks{};               ///< how many hacks change only what a player sees
    std::vector<std::string> hack_ids{}; ///< the hacks the mod turns on, in catalogue order
    uint64_t size{};                     ///< the package's size, in bytes
    std::optional<double> coverage{};    ///< a language pack's coverage, 0 to 1
    std::vector<MapLine> maps{};         ///< a map pack's maps
    std::optional<Badge> badge{};        ///< the badge, when the app has it
    /// The day this release was published, `YYYY-MM-DD` as the catalogue
    /// writes it; empty when the catalogue names none.
    std::string published{};
    /// Base games the package runs with, as the catalogue's supportedGames
    /// names them. Empty when the catalogue omits the key.
    std::vector<std::string> supported_games{};
    /// Expansions that must be on, as requiredExpansions names them. Empty
    /// when the catalogue omits the key, and empty when it names none.
    std::vector<std::string> required_expansions{};
    /// A mutator's facts, when the kind is a mutator. Names may be empty
    /// until the host resolves them.
    std::optional<MutatorFacts> mutator{};
    /// A brain's facts, when the kind is a brain. Names may be empty until
    /// the host resolves them.
    std::optional<BrainFacts> brain{};
    /// A key layout's facts, when the kind is a key layout.
    std::optional<KeyLayoutFacts> keys{};
};

/// The version a package keeps from before its last update.
struct Kept {
    std::string version{}; ///< as written
    int64_t revision{};    ///< 0 when unknown
};

/// One package installed in the player's folder, with the release it matched.
struct Installed {
    Kind kind{Kind::mod};
    std::string key{};                           ///< the manifest's id
    std::filesystem::path folder{};              ///< the installed folder
    std::string name{};                          ///< the name a player sees
    std::string version{};                       ///< as written
    int64_t revision{};                          ///< the packaging revision; 0 when it names none
    std::string registry{};                      ///< where it came from; empty for the player's own
    int64_t release{};                           ///< the release it matched; 0 for the player's own
    std::optional<base::sha256::Digest> rules{}; ///< a mod's rules hash, when it resolves
    bool playing{};                              ///< the mod being played
    /// A language pack the game shows its words from now: the language
    /// chosen's, or one of its fallbacks'. False for every other kind.
    bool in_use{};
    std::optional<Kept> kept{};   ///< the copy ROLL BACK restores
    std::optional<Badge> badge{}; ///< the badge, when the app has it
    std::string author{};         ///< the manifest's author's name; empty when it names none
    /// The manifest's description; empty when it gives none. The details
    /// show it where the listing has no summary (entry_summary).
    std::string description{};
    /// A mod folder with no oamod.yaml, which layers over the game folder
    /// and plays by the game's own rules: its status line says so.
    bool without_manifest{};
    /// Base games the installed package runs with, from its manifest.
    std::vector<std::string> supported_games{};
    /// Expansions that must be on. Empty when the manifest names none.
    std::vector<std::string> required_expansions{};
    /// A mutator's facts, from the installed manifest. Names may be empty
    /// until the host resolves them. When set, these win over the listing's
    /// for works-with, prerequisites, conflicts, layers and options.
    std::optional<MutatorFacts> mutator{};
    /// A brain's facts, from the installed manifest. When set, these win
    /// over the listing's.
    std::optional<BrainFacts> brain{};
    /// A key layout's facts, from the installed manifest. When set, these
    /// win over the listing's.
    std::optional<KeyLayoutFacts> keys{};
};

/// A newer release of an installed package, in the same registry.
struct UpdateNote {
    /// Whether the update changes the rules saved games are tied to.
    enum class Rules : uint8_t {
        same,    ///< the rules hash stays, or the kind has no rules
        changes, ///< both hashes are known and differ
        unknown, ///< a mod is missing one of the two hashes
    };
    /// Why this Open Annihilation cannot install the update.
    enum class Blocked : uint8_t {
        none,   ///< it can
        engine, ///< the engine requirement is not met
        hacks,  ///< a hack is missing or not carried out
        base,   ///< it names another base game
    };

    std::string registry{};
    Kind kind{Kind::mod};
    std::string key{};
    int64_t to_release{};                             ///< the newer release
    std::string to_version{};                         ///< its version, as written
    int64_t to_revision{};                            ///< its revision; 0 when it names none
    uint64_t size{};                                  ///< what the update downloads, in bytes
    std::optional<base::sha256::Digest> from_rules{}; ///< the installed mod's rules hash
    std::optional<base::sha256::Digest> to_rules{};   ///< the newer release's rules hash
    Rules rules{Rules::same};
    Blocked blocked{Blocked::none};
    /// The engine requirement in words, the base, or the missing hacks; empty when not blocked.
    std::string blocked_detail{};
};

/// One item of the download queue.
struct QueueItem {
    /// Where the item is.
    enum class Phase : uint8_t {
        waiting,     ///< queued, or paused
        downloading, ///< its bytes are arriving
        verifying,   ///< its SHA-256 is being checked
        checking,    ///< the installer is checking it fits this Open Annihilation
        installing,  ///< it is being put in place
        held,        ///< installed, waiting until the player leaves the mod being played
        failed,      ///< it stopped; `problem` says why
        installed,   ///< it is in place
    };

    std::string registry{};
    Kind kind{Kind::mod};
    /// An expansion's download. The footer lists it with the other packages.
    /// It is not attached to a package row.
    bool expansion{false};
    std::string key{};
    std::string name{};    ///< the name a player sees
    std::string version{}; ///< as written; empty when the kind has none
    /// The catalogue release it installs; 0 when the queue did not name one.
    int64_t release{};
    /// The installed-data reading (Inputs::installed_generation) this item
    /// was first listed against once it had finished. 0 names none. A later
    /// reading no longer lets a finished item hide an update.
    uint64_t finished_scan{};
    Phase phase{Phase::waiting};
    uint64_t done_bytes{};  ///< bytes downloaded so far
    uint64_t total_bytes{}; ///< the package's size; 0 when unknown
    /// Why a failed item stopped, in the player's words; empty otherwise.
    std::string problem{};
    bool retryable{}; ///< a failed item can be tried again
    /// For an item queued as an update, whether it changes the rules; same for a GET.
    UpdateNote::Rules update_rules{UpdateNote::Rules::same};
    /// For an item queued as an update, what ROLL BACK restores, as
    /// change_texts gives it ("revision 3" or "4.8"); empty for a GET.
    std::string update_from{};
    /// For an item queued as an update, what it installs ("revision 4" or "4.9"); empty for a GET.
    std::string update_to{};
};

/// The mod being played, for a mutator's works-with check.
///
/// An empty id means no mod is played, so the base game `ta-3.1c` is what
/// a mutator is checked against. This is not the Active tab's tile: that
/// one is the installed package marked playing, or `PlayingMod`.
struct PlayedMod {
    /// The mod's manifest id. Empty for the base game.
    std::string id{};
    /// The version as written. Empty when it names none.
    std::string version{};
    /// The name a player reads. Empty when it is not known.
    std::string name{};
};

/// The mod being played, as the Active tab shows it when no installed
/// package is marked playing: a folder outside the player's Mods folder,
/// such as the game's own mods folder or a folder the player picked.
struct PlayingMod {
    std::string name{};           ///< the name a player sees
    std::string version{};        ///< as written; empty when it names none
    std::optional<Badge> badge{}; ///< the badge, when the app has it
};

/// The language the game shows its words in, as the Active tab names it.
struct LanguageInUse {
    /// The language's name in itself, such as "Deutsch"; empty for English.
    std::string name{};
    /// The name of the language pack that holds it; empty when no pack does
    /// and the game shows it from its own words.
    std::string pack{};
    std::string version{}; ///< the pack's version, as written; empty for none
    /// The name in English, such as "German". Empty when it is not known,
    /// and when it is the same as the name in itself.
    std::string english{};
    /// The pack's packaging revision; 0 when it names none.
    int64_t revision{};
    /// Its BCP-47 tag, such as "en" or "zh-Hans". Empty when the inputs do
    /// not name one. With `pack` and `name` empty as well, English is in use.
    std::string tag{};
};

/// The base game and expansions the host last read. Empty `base_id` is the
/// default base game. `files_differ` is unset when this build has no file
/// check to show. `locked` is a choice `--content` named for the run.
struct ContentSnapshot {
    std::string base_id{};
    /// Shown after the version, such as "from GOG for macOS". Empty omits it.
    std::string source{};
    /// How many of the base game's files differ. Unset omits the line.
    /// 0 is "All files match".
    std::optional<int32_t> files_differ{};
    /// Tracks the music source held. Unset leaves the Music pill named Music.
    /// Set with `music_track_count` when the source held fewer than the table.
    std::optional<int32_t> music_tracks{};
    /// Tracks the table names for music, when `music_tracks` is set.
    int32_t music_track_count{};
    /// Bundle ids copied into the game folder, base game and expansions.
    std::vector<std::string> copied{};
    /// Expansion ids a registry delivered into the player's folder. The main
    /// menu's count includes these, not the bundles copied from an installation.
    std::vector<std::string> downloaded{};
    /// Expansion ids the stored choice has on, in the built-in order.
    std::vector<std::string> enabled{};
    /// Expansions the mod being played needs. Empty when no mod is played.
    std::vector<std::string> mod_required{};
    /// `--content` named the choice. Pills do not change it.
    bool locked{};
    /// The stored note says a mod was cleared because an expansion it needs
    /// went off. The Active tab says so, and refresh keeps that for the run.
    bool expansion_cleared{};
};

/// One installed map pack, as the Active tab lists its maps.
struct InstalledMaps {
    std::string name{};          ///< the pack's name
    std::vector<MapLine> maps{}; ///< its maps, as its manifest lists them
    /// The package key; empty when the pack is not a package the Library can select.
    std::string key{};
    /// Where it came from; empty for the player's own, and when `key` is empty.
    std::string registry{};
    /// Base games the pack's maps run with, from its manifest.
    std::vector<std::string> supported_games{};
    /// Expansions that must be on. Empty when the manifest names none.
    std::vector<std::string> required_expansions{};
};

/// An expansion a registry offers for download.
///
/// The Library does not list it as a package row. The Active tab draws a
/// download arrow at the right of that expansion's pill. The whole pill is
/// the press.
struct DownloadableExpansion {
    std::string id{};              ///< the expansion's id
    std::string registry{};        ///< the registry that offers it
    std::string name{};            ///< the name a player sees
    std::string version{};         ///< the catalogue's version
    std::string file{};            ///< the catalogue reference of the .oaexp
    base::sha256::Digest sha256{}; ///< the package file's SHA-256
    uint64_t size{};               ///< the package file's size, in bytes
    /// Base games the expansion extends, in the table's order.
    std::vector<std::string> extends{};
};

/// Everything the Library is built from.
struct Inputs {
    std::vector<Registry> registries{};
    std::vector<Listing> listings{};
    /// Expansions a registry offers. Not listed as package rows.
    std::vector<DownloadableExpansion> downloadable_expansions{};
    std::vector<Installed> installed{};
    std::vector<UpdateNote> updates{};
    std::vector<QueueItem> queue{}; ///< in the order the items were queued
    /// Moves on whenever the installed packages are read again. 0 means the
    /// inputs do not say which reading this is. A finished queue item then
    /// hides an update by its release alone. Once this names a reading, a
    /// finished item hides an update only while it finished against this
    /// same reading: the install has finished and the installed copy has not
    /// been read again. A later reading that still names an older release,
    /// such as after ROLL BACK, lets the update show.
    uint64_t installed_generation{};
    formats::oamod::EngineVersion engine{}; ///< this build's release
    std::string version_text{};             ///< this build's version as the header shows it
    int64_t list_age_seconds{-1};           ///< how old the newest list is; -1 for none
    bool offline{};                         ///< the last fetch could not reach a registry
    bool in_match{};                        ///< a game is being played
    /// The command line chose this run's mod (--mod-dir or --base-game):
    /// PLAY NOW and SWITCH OFF are off, a mod's details say why, and an
    /// expansion that mod needs stays on, its pill locked.
    bool mod_from_command_line{};
    bool can_check{}; ///< a registry is on that the refresh icon can ask for its list
    bool checking{};  ///< the registries are being asked for their lists now
    /// The mod being played; none when the game plays no mod. An installed
    /// mod marked playing is shown in its place.
    std::optional<PlayingMod> playing_mod{};
    /// The mod being played, by its manifest id, for a mutator's works-with
    /// check and a brain's. An empty id is the base game.
    PlayedMod played{};
    /// The default brain's id. Empty means the built-in computer player,
    /// and so does an id no installed brain has.
    std::string default_brain{};
    LanguageInUse language{};               ///< the language the game shows its words in
    std::vector<InstalledMaps> map_packs{}; ///< every installed map pack, with its maps
    /// Why the last change of a registry was refused, in the player's words.
    /// The Registries tab shows it in amber; empty for none.
    std::string registries_notice{};
    /// The base game entry's badge: Total Annihilation's icon from the game
    /// folder in use, or the icon the Library keeps when that program cannot
    /// be read. Empty shows the entry's letters. The entry's `badge` points
    /// here until the next refresh. On the Active tab the same icon is the
    /// base game tile's, for Total Annihilation and the demo, not the No Mod
    /// tile's. The chooser's rows for those two games point here too.
    std::optional<Badge> base_game_badge{};
    /// The base game and expansions the Active tab shows. The host fills
    /// this from the stored choice. A pill does not write it back.
    ContentSnapshot content{};
    /// The layout in use. Empty, "oa-default" or "same" means OA default.
    /// The id is the layout's package key, not built_in_layout_key.
    std::string keys_layout{};
    /// How many of the player's own changes sit on the layout in use.
    uint32_t keys_own_changes{};
    /// A layout used only with the mod being played. Empty when that mod
    /// uses keys_layout. The id is the layout's package key.
    std::string keys_for_mod{};
    /// That layout's name, for the Active tile's "With {mod}" line.
    std::string keys_for_mod_name{};
    /// How many of the player's own changes sit on the per-mod layout.
    uint32_t keys_for_mod_own{};
    /// The player's keyboard: "ansi", "iso" or "jis". Empty means ANSI.
    /// The picture uses a layout's keyboard when that is not "any".
    std::string player_keyboard{};
    /// The player's interface mode: "right-click" or "left-click". Empty
    /// means right-click. A layout never writes this.
    std::string player_pointer{};
};

/// One of the Library's tabs, in the order they are shown.
enum class Tab : uint8_t {
    active, ///< the mod being played, the language in use and the installed maps; no list
    mods,
    maps,
    mutators, ///< every mutator the player has or a registry lists
    brains,   ///< every brain the player has or a registry lists
    keys,     ///< every key layout the player has or a registry lists
    languages,
    updates,    ///< every kind, opening on the entries with an update
    registries, ///< every registry, with its switch; no list of packages
};

/// How many tabs there are.
inline constexpr std::size_t tab_total = 9;

/// Which entries a tab shows, apart from its kind, in the order the filters are shown.
enum class Filter : uint8_t {
    all,
    newest,    ///< New: the tab's newest_count listings with the latest published day
    installed, ///< installed, playing and with an update
    updates,   ///< with an update
};

/// How many filters there are.
inline constexpr std::size_t filter_total = 4;

/// How many listings New keeps on a tab.
inline constexpr std::size_t newest_count = 10;

/// What an entry's row says about it.
enum class State : uint8_t {
    get,       ///< not installed
    installed, ///< installed and up to date
    update,    ///< installed, with a newer release
    playing,   ///< installed, the mod being played, up to date
};

/// Which entry: a package of one registry, or of the player's own (registry empty).
struct EntryId {
    std::string registry{};
    Kind kind{Kind::mod};
    std::string key{};

    /// Orders ids by registry, kind and key.
    friend auto operator<=>(const EntryId&, const EntryId&) = default;
};

/// The base game entry's key.
///
/// A package key is lower-case kebab-case, letters, digits and hyphens, so
/// it cannot hold `@`. No package's id is this one.
inline constexpr std::string_view base_game_key = "@base-game";

/// Returns the base game entry's id: a mod, no registry, and base_game_key.
///
/// @return the id
[[nodiscard]] inline EntryId base_game_id() {
    return {{}, Kind::mod, std::string(base_game_key)};
}

/// Tells whether an id is the base game entry's.
///
/// @param id the id
/// @return true for a mod, an empty registry and base_game_key
[[nodiscard]] inline bool is_base_game(const EntryId& id) {
    return id.kind == Kind::mod && id.registry.empty() && id.key == base_game_key;
}

/// The start of a built-in language entry's key, then its BCP-47 tag.
///
/// A package key is lower-case kebab-case, so it cannot hold `@`. No
/// package's id starts with this.
inline constexpr std::string_view built_in_language_prefix = "@language:";

/// Returns a built-in language entry's id: a language, no registry, and
/// built_in_language_prefix followed by its tag.
///
/// @param tag the language's BCP-47 tag, such as "en" or "de"
/// @return the id
[[nodiscard]] inline EntryId built_in_language_id(std::string_view tag) {
    return {{}, Kind::language, std::string(built_in_language_prefix) + std::string(tag)};
}

/// Tells whether an id is a language built into the game.
///
/// @param id the id
/// @return true for a language, an empty registry and a key that starts
///     with built_in_language_prefix and names a tag
[[nodiscard]] inline bool is_built_in_language(const EntryId& id) {
    return id.kind == Kind::language && id.registry.empty() &&
           id.key.size() > built_in_language_prefix.size() &&
           id.key.compare(0, built_in_language_prefix.size(), built_in_language_prefix) == 0;
}

/// Returns the BCP-47 tag a built-in language entry's id holds.
///
/// @param id the id
/// @return the tag; empty when the id is not a built-in language
[[nodiscard]] inline std::string_view built_in_language_tag(const EntryId& id) {
    if (!is_built_in_language(id))
        return {};
    return std::string_view(id.key).substr(built_in_language_prefix.size());
}

/// The built-in computer player's key.
///
/// A package key is lower-case kebab-case, so it cannot hold `@`. No
/// package's id is this one.
inline constexpr std::string_view built_in_brain_key = "@built-in";

/// Returns the built-in computer player's id: a brain, no registry, and
/// built_in_brain_key.
///
/// @return the id
[[nodiscard]] inline EntryId built_in_brain_id() {
    return {{}, Kind::brain, std::string(built_in_brain_key)};
}

/// Tells whether an id is the built-in computer player's.
///
/// @param id the id
/// @return true for a brain, an empty registry and built_in_brain_key
[[nodiscard]] inline bool is_built_in_brain(const EntryId& id) {
    return id.kind == Kind::brain && id.registry.empty() && id.key == built_in_brain_key;
}

/// OA default's key.
///
/// A package key is lower-case kebab-case, so it cannot hold `@`. No
/// package's id is this one. The preference that names OA default is
/// "oa-default", "same" or empty, not this key.
inline constexpr std::string_view built_in_layout_key = "@oa-default";

/// Returns OA default's id: a key layout, no registry, and built_in_layout_key.
///
/// @return the id
[[nodiscard]] inline EntryId built_in_layout_id() {
    return {{}, Kind::key_layout, std::string(built_in_layout_key)};
}

/// Tells whether an id is OA default.
///
/// @param id the id
/// @return true for a key layout, an empty registry and built_in_layout_key
[[nodiscard]] inline bool is_built_in_layout(const EntryId& id) {
    return id.kind == Kind::key_layout && id.registry.empty() && id.key == built_in_layout_key;
}

/// Tells whether a preference names OA default.
///
/// Empty, "oa-default" and "same" do, and so does built_in_layout_key.
///
/// @param id the preference, or a layout id
/// @return true when it names OA default
[[nodiscard]] inline bool layout_names_oa_default(std::string_view id) noexcept {
    return id.empty() || id == "oa-default" || id == "same" || id == built_in_layout_key;
}

/// One row of the Library.
///
/// The pointers point into the owning Library's `inputs` and stay valid until
/// the next refresh. `listing` is null for a package no registry lists, and
/// `installed` for one that is not installed.
struct Entry {
    EntryId id{};
    State state{State::get};
    const Listing* listing{};
    const Installed* installed{};
    const UpdateNote* update{}; ///< the update offered; null for none
    const QueueItem* queued{};  ///< its latest queue item; null for none
    /// GET or UPDATE is allowed. Always true for an INSTALLED or PLAYING entry.
    bool can_act{true};
    /// Why GET or UPDATE is off, in the player's words; empty when it is allowed.
    std::string blocked{};
    /// The registry ships with Open Annihilation: its GET and UPDATE never ask.
    bool built_in{};
    /// The registry was added by the player and is labelled "Not reviewed by the OA team".
    bool not_reviewed{};
    std::string registry_name{}; ///< the registry's name; empty for the player's own
    /// The registry the same kind and key is installed from, when GET would replace it.
    std::string replaces_registry_name{};
    /// A badge that is neither the listing's nor the installed copy's. The
    /// base game entry's points at Inputs::base_game_badge. Null otherwise,
    /// and valid until the next refresh.
    const Badge* badge{};
    /// The name the base game entry, or a built-in language, shows. Empty for
    /// a package, whose name comes from its listing or its installed copy.
    std::string shown_name{};
    /// The summary the base game entry shows, or a built-in language's name
    /// in English when that differs from its name in itself. Empty for a
    /// package, and for English, whose two names are the same.
    std::string shown_summary{};
    /// Information only: this mutator or brain does not fit the game being
    /// played. GET and UPDATE stay on, and the row is not dimmed.
    bool not_compatible{};
    /// The first prerequisite, by name, that no installed mutator of that id
    /// satisfies. Empty when every prerequisite is met, and for every other kind.
    std::string needs_missing{};
    /// This entry is the default computer player: the installed brain named
    /// by Inputs::default_brain, or the built-in player when that names none.
    bool is_default{};
};

/// Returns the facts that decide works-with, prerequisites, conflicts, layers
/// and options.
///
/// The installed manifest wins when it is set, even when its works-with list
/// is empty, which means any mod. Otherwise the listing's facts are used.
///
/// @param entry the entry
/// @return the facts, or null when the entry is not a mutator or names none
[[nodiscard]] inline const MutatorFacts* mutator_facts(const Entry& entry) {
    if (entry.id.kind != Kind::mutator)
        return nullptr;
    if (entry.installed != nullptr && entry.installed->mutator)
        return &*entry.installed->mutator;
    if (entry.listing != nullptr && entry.listing->mutator)
        return &*entry.listing->mutator;
    return nullptr;
}

/// Returns a mutator's tag ids.
///
/// The listing's facts win when they are set, otherwise the installed
/// manifest's. A mod's tags are not these: they stay on its listing.
///
/// @param entry the entry
/// @return the tags, or null when the entry is not a mutator or names none
[[nodiscard]] inline const std::vector<std::string>* mutator_tag_list(const Entry& entry) {
    if (entry.id.kind != Kind::mutator)
        return nullptr;
    if (entry.listing != nullptr && entry.listing->mutator)
        return &entry.listing->mutator->tags;
    if (entry.installed != nullptr && entry.installed->mutator)
        return &entry.installed->mutator->tags;
    return nullptr;
}

/// Returns the facts that decide a brain's levels, works-with, engine
/// requirement, expansions, full awareness and tags.
///
/// The installed manifest wins when it is set. Otherwise the listing's
/// facts are used.
///
/// @param entry the entry
/// @return the facts, or null when the entry is not a brain or names none
[[nodiscard]] inline const BrainFacts* brain_facts(const Entry& entry) {
    if (entry.id.kind != Kind::brain)
        return nullptr;
    if (entry.installed != nullptr && entry.installed->brain)
        return &*entry.installed->brain;
    if (entry.listing != nullptr && entry.listing->brain)
        return &*entry.listing->brain;
    return nullptr;
}

/// Returns the facts that decide a key layout's tags, works-with, keyboard,
/// pointer, helpers and the counts the details show.
///
/// The installed manifest wins when it is set. Otherwise the listing's
/// facts are used.
///
/// @param entry the entry
/// @return the facts, or null when the entry is not a key layout or names none
[[nodiscard]] inline const KeyLayoutFacts* key_facts(const Entry& entry) {
    if (entry.id.kind != Kind::key_layout)
        return nullptr;
    if (entry.installed != nullptr && entry.installed->keys)
        return &*entry.installed->keys;
    if (entry.listing != nullptr && entry.listing->keys)
        return &*entry.listing->keys;
    return nullptr;
}

/// Which maker a trusted package or registry may show.
enum class PackageMaker : uint8_t {
    none,              ///< no mark
    open_annihilation, ///< the OA badge
    cavedog,           ///< the bone
};

/// Matches an author name to a maker.
///
/// ASCII case is ignored, and space or tab at either end is ignored. Open
/// Annihilation is "OpenAnnihilation", "Open Annihilation" or "OA team".
/// The bone is "Cavedog Entertainment". Any other name matches none. The
/// publisher is not a name this matches unless it is passed as the author.
///
/// @param author the author, as a manifest or a descriptor writes it
/// @return the maker; none when the name is not one of those
[[nodiscard]] PackageMaker package_maker(std::string_view author) noexcept;

/// Returns the maker an entry may show.
///
/// None unless the entry comes from a built-in registry, listed by one or
/// installed from one. The name is the listing's author when that listing
/// names one, otherwise the installed manifest's author. A listing author
/// that is not a maker does not fall through to the manifest. The publisher
/// is never read, so a package that names no author shows no mark.
///
/// @param entry the entry
/// @return the maker; none when the entry is not trusted or names none
[[nodiscard]] PackageMaker entry_maker(const Entry& entry) noexcept;

/// How the model looks up its words in the language shown.
///
/// The model writes every text from a pattern in text.hpp, English with
/// `{place}` holes. A screen that translates gives a lookup here; the model
/// looks each pattern up whole and then fills its places.
struct TextHooks {
    void* context{};
    /// Returns the text to show for an English pattern, its places still
    /// unfilled; the view must outlive the program's texts. Null shows English.
    std::string_view (*shown)(void* context, std::string_view english){};
};

/// Why the pending choice unloads the mod when the Library closes.
enum class PendingClear : uint8_t {
    none,                   ///< the mod stays
    base_game_chosen,       ///< another base game was chosen
    required_expansion_off, ///< an expansion the mod needs went off
};

/// The choice the Active tab is holding until the Library closes.
///
/// `edited` is set when the pills, the base game chooser or the question
/// before a mod is unloaded have left something for the close to store.
/// `asking` is set while the player is being asked before a mod is unloaded. Nothing here writes a preference or remounts the files:
/// the host reads this when the Library closes, and only then.
struct ContentDraft {
    /// Closing the Library has something to store: another base game, other
    /// expansions, or a mod to unload. The host stores only an edited draft,
    /// so a draft that is not edited shows the stored choice and the mod played.
    bool edited{};
    /// The base game the pending choice names. Empty until a pill or the
    /// chooser edits it, and then the stored base game.
    std::string base_game{};
    /// Expansion ids that are on in the pending choice.
    std::vector<std::string> expansions{};
    /// The stored base game and expansions this draft was taken from.
    /// A refresh that brings a different stored choice drops the draft.
    std::string seed_base{};
    std::vector<std::string> seed_enabled{};
    bool asking{};
    /// The expansion the question is about.
    std::string ask_id{};
    /// The expansions that would be on if the player confirms.
    std::vector<std::string> proposed{};
    /// Expansions G04 turned off with `ask_id`, apart from `ask_id` itself.
    std::vector<std::string> turned_off{};
    /// The expansion the last applied pill named, for the tile's hint.
    std::string last_id{};
    /// That expansion is on in the pending choice.
    bool last_on{};
    /// Another base game was chosen. The Active tab says it is used when the
    /// Library closes, and the mod tile says No Mod when `unload_mod` is set.
    bool pending_base{};
    /// The mod tile shows No Mod. The mod stays mounted until the Library closes.
    /// Choosing another base game and a required expansion going off are the
    /// two reasons, and `clear_reason` says which. The host clears the
    /// remembered mod for either when the Library closes. Choosing the stored
    /// base game again drops an unload for another base game.
    bool unload_mod{};
    /// Why `unload_mod` is set.
    PendingClear clear_reason{PendingClear::none};
    /// The mod's name when `clear_reason` is a required expansion, for the hint.
    std::string unloaded_mod{};
    /// The expansion that unloaded it. Turning it back on clears the unload,
    /// unless another base game is already pending.
    std::string unloaded_for{};
    /// The host has already applied this draft. A second close does nothing.
    bool applied{};
    /// Expansion ids the player asked to download. The pill turns on once the
    /// copy is in the game's folder. The ask does not edit the choice, so
    /// closing the Library before the copy arrives mounts nothing.
    std::vector<std::string> downloads{};
};

/// How one base game is shown in the chooser.
enum class BaseGameState : uint8_t {
    in_use,    ///< the pending choice names it
    copied,    ///< copied, and not the pending choice
    not_added, ///< not copied: choosing it asks for a source
};

/// One base game in the chooser.
struct BaseGameRow {
    std::string id{};
    /// The letters on the badge ("TA", "DEMO") when `badge` is null.
    std::string letters{};
    /// The game's icon. Points into the inputs until the next refresh.
    /// Null draws `letters`.
    const Badge* badge{};
    std::string name{};
    std::string version{};
    BaseGameState state{BaseGameState::not_added};
    /// The state pill's words.
    std::string state_text{};
    /// The line under the name: where it was copied from, or what choosing it asks.
    std::string detail{};
    /// Expansion names, drawn as pills. They take no press.
    std::vector<std::string> expansions{};
    /// The radio marks this row.
    bool marked{};
};

/// The base game chooser over the Library.
struct BaseChooser {
    bool open{};
    /// The base game the radio marks.
    std::string marked{};
    /// Shown when the marked game is not the one in use. Empty otherwise.
    std::string note{};
    /// A copy that failed, under the note. Empty when the last copy worked.
    std::string problem{};
    std::vector<BaseGameRow> rows{};
};

/// What Use this game did.
enum class BaseUseKind : uint8_t {
    none,         ///< the chooser was closed, or the press did nothing
    applied,      ///< the pending choice now names the marked game
    needs_source, ///< the marked game is not copied; the host asks for it
    refused,      ///< a match is running, or the command line named the choice
};

/// The result of Use this game.
struct BaseUse {
    BaseUseKind kind{BaseUseKind::none};
    /// The marked base game, when `kind` is `needs_source`.
    std::string id{};
};

/// What opening the chooser did, and which control takes the focus.
struct BaseOpen {
    bool opened{};
    /// The in-use row's control. no_control when the chooser did not open.
    int32_t focus{-1};
};

/// One helper the USE question can leave on.
struct KeyHelperSwitch {
    /// The token the host writes, such as "sticky-modifiers" or "toggle:queue".
    std::string token{};
    /// The word after library.use.helper, such as "sticky-modifiers" or "toggle.queue".
    std::string name{};
    /// The words the switch shows.
    std::string label{};
    /// On until the player switches it off. Off writes nothing.
    bool on{true};
};

/// The USE question while it is open. Nothing here is written until USE.
struct KeyUse {
    bool open{};
    /// The layout the question is about.
    EntryId layout{};
    /// "Your own changes" is shown.
    bool show_own{};
    /// Keep is marked. Aside leaves the own folder of the new layout alone.
    bool keep{true};
    /// How many own changes the layout in use holds.
    uint32_t own_count{};
    /// The layout those changes sit on, for "They stay saved with {name}."
    std::string saved_with{};
    /// own-<new id> already holds changes, so keep does not write over them.
    bool already{};
    uint32_t already_count{};
    std::string already_name{};
    /// One switch per helper the layout asks for. Each starts on.
    std::vector<KeyHelperSwitch> helpers{};
    /// "Use it for" is shown, because a mod is being played.
    bool show_scope{};
    /// Only with the mod. Every game when false. Marked when works-with names the mod.
    bool for_mod{};
    std::string mod_name{};
    /// "Made for the left-click interface; yours is right-click." Empty when the schemes agree.
    std::string pointer_note{};
    /// What USE would move, from KeyUseHooks when the question opened.
    KeyMoves moves{};
};

/// What USE applies. CANCEL does not build one.
struct KeyUseResult {
    /// The layout the question named.
    EntryId layout{};
    /// Keep the own changes on top of the new layout. False sets them aside.
    bool keep{};
    /// Helpers left on, as KeyHelperSwitch::token.
    std::vector<std::string> helpers_on{};
    /// Write the per-mod preference. False writes the preference for every game.
    bool for_mod{};
};

/// The Library's whole state.
struct Library {
    Inputs inputs{};
    std::vector<Entry> entries{};
    Tab tab{Tab::active}; ///< the tab shown; Active until another is asked for
    /// Each tab's own filter, by Tab's order: Updates starts on Updates and
    /// the others on All. Active and Registries have no filters and ignore
    /// their own.
    std::array<Filter, tab_total> filters{
        Filter::all,
        Filter::all,
        Filter::all,
        Filter::all,
        Filter::all,
        Filter::all,
        Filter::all,
        Filter::updates,
        Filter::all
    };
    std::string tag{};                  ///< the tag shown; empty for every tag
    std::string query{};                ///< the search, at most max_query_bytes
    std::vector<std::size_t> visible{}; ///< indexes into `entries`, in the order shown
    std::optional<EntryId> selected{};  ///< one of the visible entries; none when none shows
    bool details_open{};                ///< the selected entry's details page is open
    TextHooks text{};                   ///< how texts are looked up; empty for English
    /// The Active tab's pending choice. A pill or the base game chooser writes
    /// it here. It is not the stored choice, and it is kept across refresh
    /// while that choice is the same. Empty `edited` shows `inputs.content`
    /// as it was read.
    ContentDraft content{};
    /// The base game chooser, over the Library. Empty `open` draws nothing.
    /// Choosing a game writes the pending choice; it does not remount.
    BaseChooser chooser{};
    /// The USE question, over the Library. Empty `open` draws nothing.
    /// GET, an install and a catalogue update never open it.
    KeyUse key_use{};
    /// How the question learns what USE would move. Empty lists nothing.
    KeyUseHooks key_hooks{};
    /// A stored note said a mod was cleared because an expansion it needs
    /// went off. Set when the inputs say so, and kept across refresh so the
    /// Active tab can still say why after the host clears the preference.
    bool expansion_note{};
    /// The base game's badge, scaled for the squares of the last layout.
    /// Those pictures point here until the next layout, which clears this.
    mutable std::deque<std::vector<uint8_t>> scaled_badge_pixels{};
};

/// The longest search kept, in bytes.
inline constexpr std::size_t max_query_bytes = 64;

/// How many missing hacks a text names before it counts the rest.
inline constexpr std::size_t named_missing_hacks = 3;

/// The base game every Open Annihilation plays.
inline constexpr std::string_view supported_base = "ta-3.1c";

/// Tells whether a mutator works with the mod being played.
///
/// An empty works-with list works with any mod, and so does `*`. `ta-3.1c`
/// works while no mod is played. A mod id works when it is the played mod
/// and its range, read by the mutator manifest's matcher, holds the played
/// version. A range that does not parse does not match.
///
/// @param inputs the Library's inputs; `played` is the mod being played
/// @param facts the mutator's facts; the installed list wins over the listing's
/// @return true when it works with the mod being played
[[nodiscard]] bool works_with_played(const Inputs& inputs, const MutatorFacts& facts);

/// How a brain's works-with list reads against the mod being played.
struct BrainWorksWith {
    /// The list is empty or names `*`: the fact says any mod.
    bool any{};
    /// One named mod is the one being played, or the list is `any`.
    bool fits{true};
    /// The mods it names, for the fact and for "Made for". Empty when `any`.
    std::vector<std::string> names{};
};

/// Reads a brain's works-with list against the mod being played.
///
/// An empty list and `*` work with any mod. `ta-3.1c` works while no mod is
/// played. A mod id works when the mutator manifest's matcher holds the
/// played version. A range that does not parse does not match. The engine
/// requirement and the expansions are not part of this reading.
///
/// @param inputs the Library's inputs; `played` is the mod being played
/// @param facts the brain's facts; the installed list wins over the listing's
/// @param text how texts are looked up; empty shows English
/// @return whether it fits, and the names the fact joins
[[nodiscard]] BrainWorksWith
brain_works_with(const Inputs& inputs, const BrainFacts& facts, const TextHooks& text = {});

/// Returns why a brain does not fit the game being played.
///
/// The engine requirement is tried first, then works-with, then the
/// expansions. Empty when it fits. An empty works-with list fits. The words
/// are information: they do not block GET or UPDATE and they do not dim the row.
///
/// @param inputs the Library's inputs; `played` is the mod, `engine` this
///     build and `content.enabled` the expansions that are on
/// @param facts the brain's facts; the installed list wins over the listing's
/// @param text how texts are looked up; empty shows English
/// @return the words; empty when the brain fits
[[nodiscard]] std::string
brain_unfit_reason(const Inputs& inputs, const BrainFacts& facts, const TextHooks& text = {});

/// Tells whether an entry is the layout in use.
///
/// The per-mod preference wins while it is set. Empty, "oa-default" and
/// "same" name OA default, and so does a preference no installed layout has.
/// KeyLayoutFacts::in_use is also read, so a host that sets only that flag
/// still shows USING.
///
/// @param library the Library
/// @param entry one of its entries
/// @return true when the player plays with this layout
[[nodiscard]] bool layout_in_use(const Library& library, const Entry& entry);

/// Returns the layout the player plays with now: the one used only with the
/// mod being played while that is set, else the one for every game.
///
/// @param inputs the Library's inputs
/// @return its package key, or "oa-default" for OA default
[[nodiscard]] std::string layout_in_effect(const Inputs& inputs);

/// Tells whether a layout's works-with list fits the mod being played.
///
/// An empty list and `*` fit any mod. `ta-3.1c` fits while no mod is played.
/// A mod id fits when the mutator manifest's matcher holds the played version.
///
/// @param inputs the Library's inputs
/// @param works the layout's works-with list
/// @return true when the layout fits
[[nodiscard]] bool layout_fits_played(const Inputs& inputs, const std::vector<WorksWith>& works);

/// Tells whether works-with names the mod being played.
///
/// @param inputs the Library's inputs
/// @param works the layout's works-with list
/// @return true when one entry is that mod and its range holds
[[nodiscard]] bool
layout_made_for_played(const Inputs& inputs, const std::vector<WorksWith>& works);

/// The words a helper token shows, as Settings › Accessibility names it.
///
/// @param token a helper token from KeyLayoutFacts::helpers
/// @param text how texts are looked up
/// @return the label
[[nodiscard]] std::string key_helper_label(std::string_view token, const TextHooks& text = {});

/// The automation word after library.use.helper for a helper token.
///
/// A latch is "toggle.<action id>". Any other helper is its name.
///
/// @param token a helper token
/// @return the word
[[nodiscard]] std::string key_helper_name(std::string_view token);

/// "· {n} changes of your own". Empty when `count` is zero.
///
/// @param count how many actions the player's own layout names
/// @param text how texts are looked up
/// @return the line
[[nodiscard]] std::string own_changes_line(uint32_t count, const TextHooks& text = {});

/// "Made for {mod}" when the layout's works-with names the mod being played.
///
/// KeyLayoutFacts::made_for wins when the host has already written it.
///
/// @param library the Library
/// @param entry a key layout
/// @return the line; empty when the layout is not made for the mod being played
[[nodiscard]] std::string layout_made_for_line(const Library& library, const Entry& entry);

/// Rebuilds the entries from new inputs.
///
/// Makes one entry per listing, joined with the package installed from the
/// same registry, kind and key, and one per installed package no listing
/// names, then the base game entry (base_game_id), which is not a package.
/// Keeps the tab, each tab's filter, the tag, query and selection, and an
/// open base game chooser while the stored choice is the same (note_content).
/// When the selected entry no longer shows, the first visible entry is
/// selected and the details close.
/// An update note is not attached when the installed copy already names a
/// catalogue release above 0 that is at least the note's, or a queue item
/// whose phase is installed does and either the inputs name no installed
/// reading or that item finished against the reading now in place. A later
/// reading leaves the note, so a roll back offers the update again. A
/// release of 0 names none, and a held item has not finished, so neither
/// retires the note.
///
/// @param[in,out] library the Library
/// @param inputs the new inputs, which the Library keeps
void refresh(Library& library, Inputs inputs);

/// Keeps or drops the Active tab's pending choice after the inputs change.
///
/// A draft taken from the stored base game and expansions now in the inputs
/// is kept while it was edited, a question is waiting for its answer, or an
/// expansion download was asked for. Any other refresh stores that choice as
/// the new seed and clears the draft, the waiting question with it.
/// An open base game chooser stays open on its marked game while the stored
/// choice is the same, its rows read again from the inputs; a different
/// stored choice closes it.
///
/// @param[in,out] library the Library, its inputs already stored
void note_content(Library& library);

/// Shows a tab with its own filter, keeping the tag and query.
///
/// The tag hides rows only while this tab's filter offers it. A tag left
/// from another tab is kept and applies again on a tab that offers it.
///
/// @param[in,out] library the Library
/// @param tab the tab
void set_tab(Library& library, Tab tab);

/// Shows the entries a filter keeps on the tab shown, which keeps it as its own.
///
/// @param[in,out] library the Library
/// @param filter the filter
void set_filter(Library& library, Filter filter);

/// Returns the filter of the tab shown.
///
/// @param library the Library
/// @return the filter
[[nodiscard]] Filter shown_filter(const Library& library);

/// Shows the entries whose listing has a tag, when the tab's filter offers it.
///
/// A tag the filter does not offer hides nothing. The tag is kept.
///
/// @param[in,out] library the Library
/// @param tag the tag; empty for every tag
void set_tag(Library& library, std::string_view tag);

/// Searches the entries, keeping at most max_query_bytes cut at a character boundary.
///
/// @param[in,out] library the Library
/// @param query the words to find; empty for every entry
void set_query(Library& library, std::string_view query);

/// Selects a visible entry. An entry that is not visible leaves the selection as it was.
///
/// @param[in,out] library the Library
/// @param id the entry
void select(Library& library, const EntryId& id);

/// Moves the selection through the visible entries, stopping at either end.
///
/// With nothing selected, selects the first visible entry.
///
/// @param[in,out] library the Library
/// @param delta how many rows to move; negative moves up
void move_selection(Library& library, int delta);

/// Opens the selected entry's details; nothing happens with nothing selected.
///
/// @param[in,out] library the Library
void open_details(Library& library);

/// Closes the details.
///
/// @param[in,out] library the Library
void close_details(Library& library);

/// Finds an entry by its id.
///
/// @param library the Library
/// @param id the entry
/// @return the entry, or null when there is none
[[nodiscard]] const Entry* find_entry(const Library& library, const EntryId& id);

/// Returns the selected entry.
///
/// @param library the Library
/// @return the entry, or null when nothing is selected
[[nodiscard]] const Entry* selected_entry(const Library& library);

/// Returns the name a player sees for an entry: its listing's, else its
/// installed package's. The base game, a built-in language and the built-in
/// computer player use the name refresh stored for them.
///
/// @param entry the entry
/// @return the name
[[nodiscard]] std::string_view entry_name(const Entry& entry);

/// Returns the words the details show under an entry's name: its listing's
/// summary, else, as for a mod of the player's own, its installed
/// manifest's description. A built-in language's is its name in English
/// when that differs from the name in itself. The built-in computer
/// player's is the summary refresh stored for it.
///
/// @param entry the entry
/// @return the words; empty when neither gives any
[[nodiscard]] std::string_view entry_summary(const Entry& entry);

/// What a tag naming a map's world starts with: the world its terrain is
/// drawn from, such as world-ice or world-green.
inline constexpr std::string_view world_tag_prefix = "world-";

/// Tells whether a tag names a map's world: world_tag_prefix and a name.
///
/// @param tag the catalogue id
/// @return true for a world tag
[[nodiscard]] constexpr bool world_tag(std::string_view tag) noexcept {
    return tag.size() > world_tag_prefix.size() && tag.starts_with(world_tag_prefix);
}

/// Returns the words a world tag shows: the world's name, without the
/// prefix, so world-ice shows as ice. Any other tag is returned as it is.
///
/// @param tag the catalogue id
/// @return a view of `tag`
[[nodiscard]] constexpr std::string_view world_name(std::string_view tag) noexcept {
    return world_tag(tag) ? tag.substr(world_tag_prefix.size()) : tag;
}

/// Lists the tags of the entries a tab shows under its filter, with how many have each.
///
/// The tag and the search are not applied. A mutator's tags are its listing's
/// facts, else its installed manifest's. A brain's are its installed
/// manifest's, else its listing's. On the Mutators tab the eight known ids
/// come first, in the kit's order, and any other id follows. On the Brains
/// tab the nine brain ids come first, in that order, and any other id
/// follows. Each id is listed only when at least one entry has it. Other
/// tabs list every tag by id. On every tab the world tags (world_tag) come
/// last, in the same order among themselves, so they stand apart under
/// their own heading.
///
/// @param library the Library
/// @return each tag and its count
[[nodiscard]] std::vector<std::pair<std::string, std::size_t>> tags_in_tab(const Library& library);

/// Counts the entries a filter keeps on the tab shown, before the tag and the search.
///
/// Mods, Maps, Mutators, Brains and Languages hold their kind and Updates
/// every kind; All keeps them all, New the newest_count listings with the
/// latest published day, Installed those installed, playing or with an
/// update, and Updates those with an update. The Active tab holds none. The
/// base game entry, the built-in languages and the built-in computer player
/// are never counted, on any tab or filter.
///
/// @param library the Library
/// @param filter the filter
/// @return how many entries it keeps
[[nodiscard]] std::size_t filter_count(const Library& library, Filter filter);

/// Counts what a tab lists under All, before the tag and the search, whichever tab is open.
///
/// Mods, Maps, Mutators, Brains and Languages count their kind. Updates
/// counts the updates that can be installed (update_count). Active counts
/// none. Registries counts the registries listed, not the packages. The
/// base game, the built-in languages and the built-in computer player are
/// left out, as they are from the filters' counts.
///
/// @param library the Library
/// @param tab the tab
/// @return how many
[[nodiscard]] std::size_t tab_count(const Library& library, Tab tab);

/// Counts the packages the player has added.
///
/// Installed mods, map packs, language packs and mutators, each once. An
/// installed brain is not counted. The base game and the languages built
/// into the game are left out. A map pack
/// the Active tab lists and that is not already one of those installed
/// packages counts too, once for the pack. A copied expansion counts. A
/// copied base game does not, and neither does a mod only the catalogue
/// lists, a mod played from a folder outside the player's Mods folder, or a
/// language the game is showing from its own words.
///
/// @param inputs the Library's inputs
/// @return how many
[[nodiscard]] std::size_t added_package_count(const Inputs& inputs);

/// Names a filter with its count, as the filter column shows it.
///
/// @param library the Library
/// @param filter the filter
/// @return such as "Installed (4)", through the pattern filter_with_count
[[nodiscard]] std::string filter_caption(const Library& library, Filter filter);

/// Counts the updates that can be installed, as UPDATE ALL acts on them.
///
/// @param library the Library
/// @return how many UPDATE entries can act
[[nodiscard]] std::size_t update_count(const Library& library);

/// Counts the UPDATE entries the list shows, as the line over the Updates tab's list says.
///
/// @param library the Library
/// @return how many visible entries have an update
[[nodiscard]] std::size_t updates_shown(const Library& library);

/// One installed map, as the Active tab lists it on its own tile.
struct ActiveMap {
    std::string name{};    ///< its title
    std::string players{}; ///< how many players it is for, in words, such as "4 players"
    std::string size{};    ///< its size as written, such as "16x16"
    /// The manifest's description; empty when it was left unwritten.
    std::string description{};
    /// The title, players and size as one line, as automation reads it:
    /// "Ridgeline · 4 players · 16x16".
    std::string line{};
    std::string pack_name{}; ///< the pack it belongs to
    /// The pack's entry, when the Maps tab can select it. The key is empty
    /// when no entry has that pack.
    EntryId pack_id{};
    /// Its minimap, which points into the Library's inputs until the next
    /// refresh; null while it has not been decoded.
    const MapPicture* preview{};
    /// "From <pack>", the link under the description that opens the pack on
    /// the Maps tab. Empty when the pack holds this map alone: the tile's
    /// title is that link, and no From line is shown.
    std::string from{};
};

/// What the Active tab shows of the player's setup.
struct ActiveSetup {
    std::string mod_name{};    ///< the mod being played; empty when no mod is
    std::string mod_version{}; ///< its version, with a revision above 1; empty for none
    /// Its version and revision as the tile states them ("4.8 (rev 3)");
    /// empty when it names neither. The base game leaves it empty.
    std::string mod_release{};
    /// Who made it, as a byline ("by the Ridge team"); empty when nobody is named.
    std::string mod_author{};
    /// "Playing" while a mod is played; empty for the base game.
    std::string mod_state{};
    /// Its badge, which points into the Library's inputs until the next
    /// refresh. Null when no mod is played: the base game's icon is the
    /// base game tile's, from active_content.
    const Badge* mod_badge{};
    /// "No Mod" when no mod is played; empty when one is.
    std::string base_title{};
    /// "{game} as it was made" when no mod is played; empty when one is.
    std::string base_look{};
    /// The mod's line as automation reads it: "Ridge 4.8 · rev 3", or "No Mod".
    std::string mod_line{};
    /// The language's name in itself; "English" when none other is named.
    std::string language_name{};
    /// The language's name in English, when it differs from the name in itself.
    std::string language_english{};
    /// The pack's name, when it differs from both of those; empty otherwise.
    std::string language_pack{};
    /// The pack's version and revision ("1.2 (rev 4)"); empty for the built-in language.
    std::string language_version{};
    /// The pack's coverage ("Coverage 92%"); empty when it names none, and for the built-in language.
    std::string language_coverage{};
    /// "Built in" when the game holds the language; empty for a pack.
    std::string language_note{};
    /// The pack's badge, which points into the Library's inputs until the
    /// next refresh; null without one, and for the built-in language.
    const Badge* language_badge{};
    /// The language's line: the pack and its version, "Deutsch 1.2", or "English · built in".
    std::string language_line{};
    /// The installed maps, ordered by title and then by pack, both folded
    /// to lower case. One tile each, not one tile per pack.
    std::vector<ActiveMap> maps{};
    /// The layout in use, "OA default" when that is the one.
    std::string keys_name{};
    /// "· {n} changes of your own" on that layout. Empty when there are none.
    std::string keys_line{};
    /// "With {mod}: {name}" when a layout is set for the mod being played.
    std::string keys_with{};
    /// "USING" while a layout is in use, or "Built in" for OA default.
    std::string keys_badge{};
    /// The layout CHANGE… opens on the Keys tab: the one in use, the per-mod
    /// one when that is set.
    EntryId keys_id{};
};

/// Describes the Active tab.
///
/// The mod is the installed mod marked playing, with its listing's name,
/// badge and author when a registry lists it, else Inputs::playing_mod,
/// else the base game. The language is the pack that holds the language
/// shown, with its name in itself and in English, its version, revision
/// and coverage, else the language the game holds itself. The maps are
/// every map of every installed pack, ordered by title and then by the
/// pack's name, folded to lower case. A map's pack is the map-pack entry
/// of the same key and registry, or, when the pack names no key, the
/// entry whose name is the pack's. A pack that holds one map leaves the
/// From line empty; a pack of several names itself there.
///
/// @param library the Library
/// @return the setup
[[nodiscard]] ActiveSetup active_setup(const Library& library);

/// How one expansion pill is drawn.
enum class ExpansionFace : uint8_t {
    on,  ///< on in the pending choice
    off, ///< copied and off, or not copied and a registry offers it
    add, ///< not copied, and no registry offers it: ADD FROM…
};

/// The download arrow drawn inside an expansion pill.
enum class ExpansionArrow : uint8_t {
    get,    ///< not copied, and not in the queue
    cancel, ///< the download is in the queue
    retry,  ///< the download failed and can be tried again
};

/// One expansion of the base game, as the Active tab's pill shows it.
struct ExpansionPill {
    std::string id{};   ///< the bundle id
    std::string text{}; ///< the name, or the name and ADD FROM…
    ExpansionFace face{ExpansionFace::off};
    /// A press does something. Off during a match and while `--content` locked the run,
    /// except while the arrow is CANCEL: that press still stops the download.
    bool enabled{true};
    /// The registry the download arrow uses. Empty when no registry offers it.
    std::string download_registry{};
    /// GET, CANCEL or RETRY, drawn inside the pill. Unset when the pill has no arrow.
    std::optional<ExpansionArrow> arrow{};
    /// The arrow takes a press. Off while that registry's downloads are off,
    /// during a match, and while `--content` locked the run. CANCEL stays on.
    bool arrow_enabled{true};
    /// Why the arrow is off, in the player's words. Empty when it takes a press
    /// or there is no arrow.
    std::string blocked{};
    /// The command line chose the mod being played, and turning this expansion
    /// off would unload that mod, which closing the Library never does. The
    /// pill stays on and takes no press (`enabled` is false). The Active tab
    /// draws it faded, as a lock, and ActiveContent::lock says why. Set only
    /// while that is the one reason a press is refused: never during a match,
    /// while `--content` locked the run, or on a pill that is off.
    bool locked{};
};

/// What the Active tab shows of the base game and its expansions.
struct ActiveContent {
    /// The letters on the badge when the icon is absent ("TA").
    std::string letters{};
    /// The base game's icon. Points into the inputs until the next refresh.
    const Badge* badge{};
    std::string name{};         ///< "Total Annihilation"
    std::string version_line{}; ///< "3.1c", or "3.1c · from GOG for macOS"
    /// "All files match" or "2 files differ". Empty when the inputs name no check.
    std::string check_line{};
    /// The check line is the accent. False draws it in the lock colour.
    bool check_good{true};
    /// The base tile's label, as automation reads it.
    std::string read{};
    /// "EXPANSIONS", or "EXPANSIONS · 5 OF 6 ON".
    std::string expansions_heading{};
    std::vector<ExpansionPill> pills{};
    /// What an expansion needs, or that a pending change applies when the Library closes.
    std::string hint{};
    /// Why the locked pills stay on, drawn after a padlock in the lock colour
    /// under the pills: "The command line chose this run's mod, which needs
    /// Campaign." It names every locked pill. Empty when none is locked.
    std::string lock{};
    /// CHANGE… takes a press. Off during a match, while `--content` locked the
    /// run, and while the command line chose the mod.
    bool change_enabled{true};
};

/// Describes the base game tile and the expansion pills.
///
/// The choice is the pending draft while it has been edited, and the stored
/// choice otherwise, and nothing here changes it. A pill is locked when the
/// command line chose the mod being played and turning that pill off would
/// unload the mod, the same rule by which press_expansion refuses it.
///
/// @param library the Library
/// @return the tile
[[nodiscard]] ActiveContent active_content(const Library& library);

/// What pressing an expansion pill did.
enum class ContentPressKind : uint8_t {
    none,            ///< refused, or the choice already said this
    redraw,          ///< the pending choice changed
    ask,             ///< a playing mod needs the expansion; the question is waiting
    add,             ///< the expansion is not copied, and no registry offers it
    download,        ///< the expansion is not copied, and a registry offers it
    cancel_download, ///< the download is in the queue, or the player just asked
    retry_download,  ///< the download failed and can be tried again
};

/// The result of pressing an expansion pill. The download arrow is part of it.
struct ContentPress {
    ContentPressKind kind{ContentPressKind::none};
    /// The expansion, when `kind` is `ask`, `add`, or a download.
    std::string expansion{};
    /// The registry a download uses. Empty otherwise.
    std::string registry{};
};

/// Presses one expansion pill. The download arrow sits inside that pill.
///
/// A copied expansion is turned on or off by turn_content_expansion, and the
/// result is kept on the pending draft. It is not written to the preferences.
/// Turning one off that a playing mod needs does not change the draft: it
/// stores the choice returned and waits for accept_expansion_off. While the
/// command line chose that mod, the press is refused instead, and
/// active_content marks that pill locked. An expansion
/// that is not copied and that no registry offers is `add` and is not turned
/// on. One a registry offers is downloaded: the ask is remembered, and the
/// pill turns on once the copy is in the game's folder, still only on the
/// pending draft. CANCEL stops that download even during a match.
///
/// @param[in,out] library the Library
/// @param id the expansion
/// @return what the press did
[[nodiscard]] ContentPress press_expansion(Library& library, std::string_view id);

/// Forgets an expansion download the feed refused, so the arrow is GET again.
///
/// @param[in,out] library the Library
/// @param id the expansion
void drop_expansion_download(Library& library, std::string_view id);

/// Turns on each expansion the player asked to download, once it is copied.
///
/// The choice stays on the pending draft. Nothing is written and nothing is
/// mounted. An expansion whose needs are not copied is left off, and that ask
/// is dropped. Nothing happens during a match, while `--content` locked the
/// run, or while the expansion-off question is waiting.
///
/// @param[in,out] library the Library
void adopt_downloaded_expansions(Library& library);

/// Keeps the choice the expansion-off question proposed, on the pending draft.
///
/// @param[in,out] library the Library
void accept_expansion_off(Library& library);

/// Drops the expansion-off question's proposal. The pending choice stays.
///
/// @param[in,out] library the Library
void cancel_expansion_off(Library& library);

/// Opens the base game chooser on the pending choice's game.
///
/// Nothing opens during a match, while `--content` locked the run, or while
/// the command line chose the mod. The files are not remounted.
///
/// @param[in,out] library the Library
/// @return whether it opened, and the control that takes the focus
[[nodiscard]] BaseOpen open_base_chooser(Library& library);

/// Closes the chooser. The pending choice stays.
///
/// @param[in,out] library the Library
void close_base_chooser(Library& library);

/// Marks one base game in the open chooser. A closed chooser is left as it is.
///
/// @param[in,out] library the Library
/// @param id the base game
void mark_base_game(Library& library, std::string_view id);

/// Rebuilds the open chooser's rows from the inputs, keeping the marked game.
///
/// @param[in,out] library the Library
void refresh_base_chooser(Library& library);

/// Keeps a failed copy's words on the open chooser.
///
/// @param[in,out] library the Library
/// @param problem the words; empty clears them
void note_base_copy_problem(Library& library, std::string_view problem);

/// Uses the marked base game.
///
/// A game that is not copied is `needs_source` and the chooser stays open.
/// A copied game is given to choose_content_base_game and kept on the pending
/// draft: every copied expansion of it is on, and a mod being played is
/// marked unloaded. The stored base game, chosen again after another, drops
/// that unload, so the mod played with it plays again. The same game as the
/// pending choice only closes the chooser. Nothing is written to the
/// preferences, and nothing is remounted.
///
/// @param[in,out] library the Library
/// @return what the press did
[[nodiscard]] BaseUse use_marked_base_game(Library& library);

/// How well one search word was found: the best field it is in.
enum class MatchClass : uint8_t {
    name,    ///< in the name
    tag,     ///< a tag that starts with it, or is it
    author,  ///< in the author or the publisher
    summary, ///< in the summary
};

/// How an entry ranks for a search.
struct Rank {
    MatchClass worst{MatchClass::name}; ///< the largest class among the words
    uint32_t total{};                   ///< the sum of the words' classes
    bool name_starts{};                 ///< the name starts with the first word
};

/// Ranks an entry for a search.
///
/// Letters A to Z are folded to lower case, in the query and the fields;
/// other bytes compare as they are. The query is split on spaces, and each
/// word takes the class of the best field it is found in. A word's tag class
/// matches the start of a tag id or the start of the label that id shows.
///
/// @param entry the entry
/// @param query the search; empty matches every entry
/// @param text how a tag's label is looked up; empty shows the English label
/// @return the rank, or nothing when a word is in no field
[[nodiscard]] std::optional<Rank>
rank(const Entry& entry, std::string_view query, const TextHooks& text = {});

/// What a button asks for.
enum class Action : uint8_t {
    get,
    update,
    update_all,
    cancel,
    retry,
    roll_back,
    play_now,
    /// The mod being played's own in place of PLAY NOW: the game plays no
    /// mod from then on, once the player answers the Switch Mod question.
    switch_off,
    open_folder,
    homepage,
    details,
    back,
    check_now, ///< the header's refresh icon: the registries are asked for their lists again
    settings,
    close,
    /// ADD REGISTRY… on the Registries tab. The host opens the add-registry question.
    add_registry,
    /// ADD MIRROR… on a registry the player added. `ScreenResult::registry` names it.
    add_mirror,
    /// REMOVE on a registry the player added. `ScreenResult::registry` names it.
    /// The host removes it at once.
    remove_registry,
    /// A registry's switch. `ScreenResult::registry` names it and
    /// `ScreenResult::registry_on` is the half that was chosen.
    set_registry,
    /// REFRESH on one registry's row. `ScreenResult::registry` names it.
    /// The host asks that registry for its catalogue.
    refresh_registry,
    /// An installed package's: asks first, then removes its folder and the
    /// copy ROLL BACK keeps (uninstall.hpp).
    uninstall,
    /// A built-in language, or an installed language pack, that is not the
    /// one the game shows: switches to it as Settings › Language does.
    use_now,
    /// CHANGE… on the base game tile. The host opens the chooser. This press
    /// does not change the choice.
    change_base_game,
    /// USE THIS GAME named a base game that is not copied. The host asks for
    /// its source and copies it. `ScreenResult::base_game` names it. The
    /// chooser stays open until the copy is in the pending choice.
    copy_base_game,
    /// ADD FROM… on an expansion that is not copied. The host opens the
    /// locator. `ScreenResult::expansion` names it. This press does not turn it on.
    add_expansion,
    /// A pill would turn off an expansion a playing mod needs. The host asks
    /// expansion_off_question and, on Turn off, calls accept_expansion_off.
    ask_expansion_off,
    /// TURN ON on a package whose required expansions are off. One press
    /// turns on every one G04 accepts, on the pending draft. It applies
    /// when the Library closes.
    turn_on,
    /// CHECK WITH THIS MOD on an installed mutator or brain. The host checks
    /// a mutator against the mod being played, with the mutator's default
    /// options, and a brain by loading it and running fits. It never
    /// switches a mutator on and never seats a brain.
    check_with_mod,
    /// USE BY DEFAULT on an installed brain that is not the default, or on
    /// the built-in computer player while it is not. The host writes that
    /// brain as the one new computer seats start with. Empty, for the
    /// built-in player, erases the preference.
    use_by_default,
    /// GET WITH on a mutator whose plan holds more than itself. The host
    /// asks, then queues every step, prerequisites first. It takes GET's place.
    get_with,
    /// USE on an installed key layout that is not the one in use, or on
    /// OA default while another layout is. The press opens the question.
    /// Nothing is written until the question's USE.
    use_layout,
    /// EDIT KEYS… on the layout in use. The host opens the key editor as
    /// Settings › Controls does.
    edit_keys,
};

/// Returns the action an expansion's download arrow asks for.
///
/// @param arrow the arrow
/// @return GET, CANCEL or RETRY
[[nodiscard]] constexpr Action action_of(ExpansionArrow arrow) noexcept {
    switch (arrow) {
    case ExpansionArrow::get:
        return Action::get;
    case ExpansionArrow::cancel:
        return Action::cancel;
    case ExpansionArrow::retry:
        return Action::retry;
    }
    return Action::get;
}

/// One button of an entry's details.
struct ActionButton {
    Action action{Action::get};
    bool enabled{};
    /// The button's words. A fixed action's are the text.hpp word, looked
    /// up. TURN ON names the expansion, so the words are owned here.
    std::string text{};
};

/// Lists an entry's buttons, in the order they are shown.
///
/// TURN ON when the package needs expansions that are off, then GET or
/// UPDATE, or CANCEL while it is queued, or RETRY when its download failed
/// and can be tried again; then PLAY NOW for an installed mod
/// that is not being played, or SWITCH OFF for the one that is, ROLL BACK
/// when a copy is kept, UNINSTALL and OPEN FOLDER when installed, and
/// HOMEPAGE when the listing has one. PLAY NOW, SWITCH OFF and ROLL BACK are
/// off during a game, and PLAY NOW and SWITCH OFF while the command line
/// chose the mod; UNINSTALL is off while uninstall_off_reason gives a reason.
/// The base game entry is not a package: while no mod is played it offers
/// nothing, and while one is it offers PLAY NOW alone, on and off by the
/// same rule as SWITCH OFF, and never UPDATE, ROLL BACK, UNINSTALL or
/// OPEN FOLDER. A built-in language offers USE NOW while it is not the one
/// in use, and nothing while it is. An installed language pack that is not
/// in use offers USE NOW where a mod offers PLAY NOW. A language that is
/// listed and not installed keeps GET and has no USE NOW.
/// TURN ON comes first when turning the package's required expansions on,
/// through the same rules as the Active tab's pills, would let it run.
/// PLAY NOW and USE NOW are off when the package's games or expansions do
/// not match the pending choice. GET stays on. A package that cannot run
/// stays in the list.
///
/// A mutator offers none of TURN ON, PLAY NOW, SWITCH OFF, USE NOW or
/// ROLL BACK. Its buttons are GET or UPDATE (CANCEL while queued, RETRY
/// when a download failed and can be tried again; still on when the mutator
/// does not work with the mod being played), then CHECK WITH THIS MOD for
/// an installed copy whose verdict is set (off during a game and while that
/// check is running), then UNINSTALL and OPEN FOLDER when installed, then
/// HOMEPAGE when the listing has one. GET WITH takes GET's place when
/// `get_plan` holds more than the mutator, or is blocked before it can:
/// it names the one missing prerequisite, or how many more there are, and
/// it is off with the plan's reason. A queued step shows CANCEL on that
/// step's own entry, as a queued GET does.
///
/// A brain offers none of TURN ON, PLAY NOW, SWITCH OFF, USE NOW, ROLL BACK
/// or GET WITH. Its buttons are GET or UPDATE (CANCEL while queued, RETRY
/// when a download failed and can be tried again; still on when the brain
/// does not fit), then USE BY DEFAULT when it is installed and not the
/// default, then CHECK WITH THIS MOD for an installed copy whose verdict is
/// set (off during a game and while that check is running), then UNINSTALL
/// and OPEN FOLDER when installed, then HOMEPAGE when the listing has one.
/// The built-in computer player offers USE BY DEFAULT while it is not the
/// default, and nothing while it is.
///
/// @param library the Library
/// @param entry one of its entries
/// @return the buttons
[[nodiscard]] std::vector<ActionButton> entry_actions(const Library& library, const Entry& entry);

/// Tells whether PLAY NOW and SWITCH OFF can act, and so whether the Active
/// tab's UNLOAD MOD can: not during a game, and not while the command line
/// chose this run's mod.
///
/// @param library the Library
/// @return true when the switch can be asked for
[[nodiscard]] bool can_switch(const Library& library) noexcept;

/// How a fact's value is marked.
enum class Mark : uint8_t {
    none,
    good, ///< met
    warn, ///< not met, or a change the player should know of
    bad,  ///< cannot run with this mod, or the check stopped
};

/// How a package's games and required expansions sit on the pending choice.
///
/// Empty `games` and `expansions` mean the package names neither, and it
/// runs. A row sentence is written when it names an expansion or the
/// current game is not one it supports. `turn_on` is empty unless one
/// press, through turn_content_expansion, would turn on every required
/// expansion that is off.
struct ContentLines {
    /// The sentence under the row. Empty when there is nothing to add.
    std::string row{};
    /// The Games fact. Empty when the package names no games.
    std::string games{};
    Mark games_mark{Mark::none};
    /// The expansion Needs fact. Empty when the package names no expansion.
    std::string expansions{};
    Mark expansions_mark{Mark::none};
    /// PLAY NOW and USE NOW stay off: the game is not one it supports, or
    /// a required expansion is off.
    bool blocks_play{};
    /// The expansions one TURN ON press turns on, in the order asked.
    std::vector<std::string> turn_on{};
    /// The first of those, as the button names it.
    std::string turn_on_name{};
    /// More than one expansion in `turn_on`.
    bool several{};
};

/// Describes a package against the pending choice, or the stored choice
/// when the draft has not been edited.
///
/// Installed lists are used when they name a game or an expansion, and the
/// listing's otherwise. Lists that are both empty name nothing. An expansion
/// list with no games takes the default games: both base games for a
/// language pack, and totala otherwise.
///
/// @param library the Library
/// @param entry one of its entries
/// @return the sentences, the facts and whether TURN ON is offered
[[nodiscard]] ContentLines content_lines(const Library& library, const Entry& entry);

/// Tells whether PLAY NOW or USE NOW can run this package on the pending choice.
///
/// A package that names no games and no expansions can. The base game entry
/// and a built-in language can.
///
/// @param library the Library
/// @param entry one of its entries
/// @return true when the package can run
[[nodiscard]] bool package_can_run(const Library& library, const Entry& entry);

/// Turns on the expansions TURN ON names, on the pending draft.
///
/// One press asks turn_content_expansion for each expansion that is off,
/// skipping one an earlier press in the same call already turned on. Nothing
/// is written to the preferences. The choice applies when the Library closes.
///
/// @param[in,out] library the Library
/// @param entry the package whose button was pressed
/// @return true when the draft changed
bool turn_on_required(Library& library, const Entry& entry);

/// The note under the buttons while TURN ON is showing.
///
/// Empty when the selected entry has no such button. A mod or a map pack
/// says PLAY NOW will work once the Library closes, and a language says
/// USE NOW.
///
/// @param library the Library
/// @return the note; empty when the selection has none
[[nodiscard]] std::string turn_on_note(const Library& library);

/// Tells whether the header's refresh icon can be pressed: a registry is on
/// that it can ask, none is being asked now, and no game is being played.
///
/// @param library the Library
/// @return true when it can
[[nodiscard]] bool can_check_now(const Library& library) noexcept;

/// One line of an entry's details, in a long form and a short form.
struct Fact {
    std::string long_label{};
    std::string long_value{};
    std::string short_label{};
    std::string short_value{};
    Mark mark{Mark::none};
    /// When set, the value is a link that opens the Keys tab with this entry
    /// selected. A mod's "Key layouts" facts use it.
    std::optional<EntryId> opens{};
};

/// Lists an entry's facts: Installed, Latest, Author, Size, Base, Games,
/// Needs, Hacks, Rules, Maps, Coverage and From, those it has.
///
/// Games names the base games the package runs with. A second Needs, after
/// the engine's, names the expansions that must be on. Both are left out
/// when the package names neither.
///
/// Installed is the installed copy's version and revision, and Latest the
/// listing's, each as release_text writes them ("4.8 (rev 3)", "1.0" for a
/// revision of 0, "rev 3" without a version); a side with neither is left
/// out. Author is the listing's author, else its publisher, else the
/// installed manifest's author; left out when none names one. An update's
/// Rules fact says whether it changes the game's rules and which saved
/// games stop loading.
///
/// A mutator leaves out Size, Base, Games, Hacks, Maps and Coverage. After
/// Author it lists Works with, Checked (an installed copy whose verdict is
/// set), one Needs per prerequisite and then the engine's Needs, Options,
/// Changes, Rules and From, leaving out a fact with nothing to say. Rules
/// says the mutator changes the game and is unranked. The installed
/// manifest's facts are used when a copy is installed, and the listing's
/// otherwise.
///
/// @param library the Library
/// @param entry one of its entries
/// @return the facts, in that order
[[nodiscard]] std::vector<Fact> facts(const Library& library, const Entry& entry);

/// Describes an entry in one line under its name.
///
/// Its queue item's phase while queued, else why it is blocked, else the
/// update offered, Installed, Playing, or Not installed with its size. An
/// update is "Revision 4 available" only for a revision above 0 of the
/// version installed, "Version 4.9 available" for another version, and
/// "Update available" when it keeps the version and names no revision. A
/// built-in language says "Built into the game". A mutator that is not
/// installed names its version instead of its size ("Not installed · 2.0 ·
/// rev 1", including a revision of 1).
///
/// @param library the Library
/// @param entry one of its entries
/// @return the line
[[nodiscard]] std::string status_line(const Library& library, const Entry& entry);

/// Names who made an entry's package: "by" its author, else its publisher.
///
/// @param entry the entry
/// @param text how texts are looked up
/// @return the line; empty for the player's own packages, and when the
///         listing names no one
[[nodiscard]] std::string byline(const Entry& entry, const TextHooks& text = {});

/// Says how an update of one mod meets the mod being played, and, on an
/// installed mod while the command line chose this run's mod, that it did,
/// before anything else.
///
/// @param library the Library
/// @param entry the entry the note is about
/// @return the note; empty when it has none
[[nodiscard]] std::string playing_note(const Library& library, const Entry& entry);

/// Says how an update of the selected mod meets the mod being played, and,
/// on an installed mod while the command line chose this run's mod, that it
/// did, before anything else. The Active tab selects nothing, so a note
/// about the mod being played uses the form that takes that entry.
///
/// @param library the Library
/// @return the note; empty when nothing is selected, or when it has none
[[nodiscard]] std::string playing_note(const Library& library);

/// Writes the line under a package's name while its download has not finished.
///
/// An expansion's hint uses the same words as a package row.
///
/// @param item the queue item
/// @param text how texts are looked up
/// @return the line; empty once the item is installed
[[nodiscard]] std::string queue_status_text(const QueueItem& item, const TextHooks& text);

/// The download queue as the footer shows it.
struct QueueLines {
    std::string first{};   ///< the item that matters most now
    std::string second{};  ///< the next item, and how many more
    std::string compact{}; ///< one short line for a small screen
};

/// Describes the download queue.
///
/// The first line is the item downloading; else the first verifying,
/// checking or installing; else the first that failed; else the first held;
/// else the first waiting; else the last installed. The second is the first
/// other item still to finish, in queue order, and how many more there are.
///
/// @param library the Library
/// @return the lines; empty when the queue is
[[nodiscard]] QueueLines queue_lines(const Library& library);

} // namespace oa::ui::library
