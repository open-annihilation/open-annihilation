// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Playback of a TA Demo recording (.tad) or a .oademo replay through the match's own network
// receive path. The third-party recorder kept every datagram the recording
// client sent or received, minus the condenser envelope and the frame
// sequence dword, stored unit-state (0x2c) records without their tick behind
// a tick base, and kept each player's lobby block as sent. Playback rebuilds
// each recorded packet as a broadcast frame from its sender and offers it to
// the packet layer as a transport would once the local tick reaches the
// sender ticks it carries, so frame ordering, tick spreading, admission and
// record dispatch run exactly as for a live peer. The local machine is a
// watcher slot with no units; every recorded player is a remote slot, under
// its recorded id, whose units arrive through 0x09 and 0x2c records.

#include "oa/sim/match_runtime.hpp"
#include "oa/sim/match_runtime/keyframe.hpp"
#include "oa/netgame/match/match_binding.hpp"
#include "oa/formats/tad.hpp"
#include "oa/netgame/records.hpp"
#include "oa/netgame/wire_rules.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace oa::session::demo {

inline constexpr std::size_t sender_count = 256; // one entry per recorded player number

struct DemoFrame {
    uint64_t time_ms{};            // recorder wall clock at arrival
    uint32_t due_tick{};           // local tick from which it is delivered
    uint8_t sender{};              // recorded player number
    std::vector<uint8_t> datagram; // condenser-wrapped broadcast frame
};

// A recorded player's lobby block: from the 0x20 record of its status
// datagram, else only the side and colour of its player chunk.
struct RecordedPlayer {
    PlayerSetupInfo info{};
    bool from_status{};
};

/// Reads one recorded player's lobby block, as playback binds it.
///
/// The block is the 0x20 record of the player's status datagram when the
/// recording holds one that reads. Otherwise only the side and colour are
/// known, and they are the bytes of the player's own chunk.
///
/// @param demo Parsed recording the player belongs to.
/// @param player A player of `demo`.
/// @return The block, and whether it came from a status datagram.
[[nodiscard]] RecordedPlayer
demo_recorded_player(const formats::tad::Demo& demo, const formats::tad::Player& player);

struct PlaybackStats {
    uint32_t frames{};              // datagrams rebuilt
    uint32_t frames_delivered{};    // handed to the packet layer
    uint32_t frames_oversized{};    // larger than the receive buffer; dropped
    uint32_t empty_packets{};       // recorded packets with no game record
    uint32_t empty_ticks{};         // 0xff markers restored as minimal 0x2c records
    uint32_t bad_packets{};         // no payload marker or an unsplittable tail
    uint32_t recorder_records{};    // 0xf9..0xfe, not part of the game stream
    uint32_t untimed_unit_states{}; // 0xfd or 0xff with no tick base before it
    uint32_t sends_dropped{};       // the watcher's own outgoing frames
    uint32_t unknown_senders{};     // packets from a number no player chunk names; dropped
    uint32_t wide_records{};        // 0xE0..0xE2 kept in the rebuilt frames
};

struct DemoPlayback {
    std::vector<uint8_t> file;
    formats::tad::Demo demo;                         // views file
    std::vector<RecordedPlayer> players;             // demo.players order
    std::array<uint32_t, sender_count> sender_ids{}; // recorded player number -> net id
    uint32_t watcher_id{};                           // sorts after every recorded id
    // The watcher's battleroom: a joined client's unit sync table after the
    // recorded 0x1a records arrived. Only the host's verdicts (subtype 3)
    // change a client's table.
    std::unique_ptr<ui::frontend_multiplayer::Lobby> battleroom;
    uint32_t recorded_verdicts{};    // subtype-3 records in the recording
    uint32_t recorded_definitions{}; // the recorded game's unit table, slot 0 excluded
    unsigned unit_def_bits{};        // its 0x2c index width
    std::vector<DemoFrame> frames;   // recorded order
    std::size_t cursor{};
    uint32_t tick{}; // local tick being delivered for
    PlaybackStats stats;
    /// The limits sector when the recording carried one that could be read.
    std::optional<formats::tad::RecordedLimits> limits;
    /// True when a limits sector was present and could not be read.
    bool limits_unreadable{};
    /// True when the map the stream names was taken only by the campaign
    /// loader. The mission keeps its own unit table, and playback keeps those
    /// definitions and no others, so a created unit's type is the mission's.
    /// The container's description does not decide this: a .tad has none.
    bool mission_table{};
    /// How a recording of ten players is watched (recorder.ten-player-replay);
    /// off refuses one, as the viewer then has no slot of its own.
    netgame::TenPlayerReplay ten_player_replay{};
};

/// How a recording's limits sit against the mod the player has.
enum class DemoLimitsVerdict : uint8_t {
    plays,        ///< the recording can play
    other_limits, ///< it names limits this mod does not
    base_limits,  ///< it has no limits sector and this mod raises one
    unreadable,   ///< its limits sector cannot be read
};

/// Tells whether a recording plays under two limits.
///
/// A readable sector equal to the two values plays, and one that differs
/// does not. An unreadable sector does not play. No sector plays when both
/// values are 3.1c's weapon types and sides, and does not when either is raised.
///
/// @param playback Loaded playback.
/// @param weapon_types Weapon types the mod numbers.
/// @param sides Sides the mod loads.
/// @return The verdict.
[[nodiscard]] DemoLimitsVerdict
demo_limits_verdict(const DemoPlayback& playback, uint32_t weapon_types, uint32_t sides) noexcept;

/// Tells how the match sees a playback's viewer: as a slotless viewer when every slot belongs to a
/// recorded player (recorder.ten-player-replay), a watcher under watcher-view and every recorded
/// player's ally under allied-fake-player; otherwise as the viewer's own seated slot.
///
/// @param playback Loaded playback.
/// @return SlotlessViewer::none unless demo_watches_without_slot holds.
[[nodiscard]] sim::match_runtime::SlotlessViewer
demo_slotless_viewer(const DemoPlayback& playback) noexcept;

/// The ticks between two settlements of a player's economy.
inline constexpr uint32_t economy_settlement_ticks = 30;

/// A recorded player's last economy record (0x28), from which a watching
/// viewer's income figures for that player follow.
struct EconomySample {
    bool taken{};    ///< a record has been seen
    uint32_t tick{}; ///< the local tick it arrived at
    float energy_produced_total{};
    float energy_requested_total{};
    float metal_produced_total{};
    float metal_requested_total{};
};

/// Gives a recorded player the income figures its economy records imply
/// (recorder.ten-player-replay watcher-view).
///
/// A player simulated on another machine settles no economy here, so its
/// production and use per settlement stay unknown; its economy records
/// carry only running totals. From the second record on, each of the
/// player's four per-settlement figures (energy and metal produced and
/// requested) becomes the growth of its total since the previous record,
/// scaled to one settlement (economy_settlement_ticks). A record arriving
/// at the same tick as the previous one changes nothing.
///
/// @param[in,out] sample The player's previous record, replaced by this one.
/// @param[in,out] player The player whose figures are set.
/// @param record The economy record.
/// @param tick The local tick it arrived at.
void demo_follow_economy(
    EconomySample* sample, Player* player, const netgame::EconomyRecord& record, uint32_t tick
) noexcept;

/// Tells whether a playback watches without a slot of its own: a recording of ten players under the
/// ten-player rule.
///
/// @param playback Loaded playback.
/// @return True when every slot belongs to a recorded player.
[[nodiscard]] bool demo_watches_without_slot(const DemoPlayback& playback) noexcept;

/// Tells whether bytes are a TA Demo recording or a `.oademo` replay.
///
/// A replay is recognised from its zip local header and the start of its `oademo` line.
/// A recording is recognised from the first chunk's length field and the
/// magic that opens its data. Bytes that pass may still fail demo_load.
///
/// @param bytes Whole file, or any other file's bytes.
/// @return True when the bytes open like a `.oademo` or a recording's header chunk holds the magic.
[[nodiscard]] bool demo_recognised(std::span<const uint8_t> bytes) noexcept;

/// Reads a recording file and loads it with demo_load.
///
/// @param[out] playback Playback state to fill.
/// @param path Path of the `.tad` or `.oademo` file.
/// @param[out] error Reason for a failure.
/// @return False when the file cannot be opened or demo_load fails.
[[nodiscard]] bool
demo_open(DemoPlayback* playback, const std::filesystem::path& path, std::string* error);

/// Parses a recording and rebuilds its packets as condenser-wrapped broadcast frames.
///
/// A `.oademo` is read first and replaced by the recording it stores. When
/// that read fails, `error` is the sentence the replay reader returns.
/// Binds each recorded player to an id and lobby block, replays the
/// recorded 0x1a unit checks into the watcher's battle room, sizes the 0x2c
/// index width from the recorded unit table (slot 0 included), and gives
/// each frame the local tick from which it is due: the last sender tick it
/// carries, or the previous frame's for frames without unit states.
///
/// @param[out] playback Playback state to fill; it takes the bytes.
/// @param bytes Whole `.tad` or `.oademo`.
/// @param[out] error Reason for a failure.
/// @return False when the file cannot be read, the recording does not parse, has no players, cannot bind
///         them or announces no unit definitions.
[[nodiscard]] bool
demo_load(DemoPlayback* playback, std::vector<uint8_t> bytes, std::string* error);

/// Returns a transport over the recording: receive yields, in recorded order, every frame whose due tick
/// has come; send drops the frame.
///
/// @param playback Loaded playback, passed back to the transport as its context.
/// @return The transport.
[[nodiscard]] netgame::NetTransport demo_transport(DemoPlayback* playback) noexcept;

/// Tells whether every rebuilt frame has been delivered.
///
/// @param playback Loaded playback.
/// @return True once the cursor is past the last frame.
[[nodiscard]] bool demo_exhausted(const DemoPlayback& playback) noexcept;

/// Returns the recording's length on the recorder's clock.
///
/// @param playback Loaded playback.
/// @return Milliseconds up to the last packet; 0 without packets.
[[nodiscard]] uint64_t demo_duration_ms(const DemoPlayback& playback) noexcept;

/// Returns the watcher's unit sync table for the match's unit table.
///
/// @param playback Loaded playback.
/// @return The table, or null when the recording holds no verdicts. Without
///         verdicts the match keeps every local type, except a campaign
///         mission, which keeps the definitions its recording names.
[[nodiscard]] const ui::frontend_multiplayer::UnitSync*
demo_unit_sync(const DemoPlayback& playback) noexcept;

// The recorded game's unit table against the match's unit definitions' FBI
// hashes (local_keys[i] belongs to definition index i + 1). The recorded
// table holds the types whose verdict has both sides set, else, without
// verdicts, the recorder's own subtype-2 list. Unit-definition indices
// travel in 0x09 and 0x2c records, so playback needs the identical table.
struct UnitTableCheck {
    uint32_t recorded{};
    uint32_t matched{};
    uint32_t missing{}; // recorded definitions absent here
    uint32_t extra{};   // local definitions the recording lacks
    bool identical{};
};

/// Compares the recorded game's unit table with the match's unit definitions.
///
/// @param playback Loaded playback.
/// @param local_keys FBI hashes of the match's unit definitions; local_keys[i] belongs to definition index i + 1.
/// @param[out] extra_indices Indices into local_keys of definitions the recording lacks, appended when not null.
/// @return Counts of recorded, matched, missing and extra definitions, and whether the tables are identical.
[[nodiscard]] UnitTableCheck demo_check_unit_table(
    const DemoPlayback& playback,
    std::span<const uint32_t> local_keys,
    std::vector<std::size_t>* extra_indices
);

/// Builds the player table for playback.
///
/// Recorded players become remote slots in recorded order with their
/// recorded ids and lobby blocks, one local watcher slot follows them, and
/// free slots take the -1 id. The host block's options apply as at a
/// multiplayer launch, and the watcher, local and viewing, sees the whole
/// map as a launched watcher does. A recording of ten players under the
/// ten-player rule leaves no slot for the watcher: the view is the first
/// recorded player's slot, which stays remote, with the whole map shown, and
/// watcher_slot is OA_PLAYER_COUNT.
///
/// @param playback Loaded playback.
/// @param[in,out] world Match world whose player records and Game fields are set.
/// @param[out] watcher_slot The watcher's slot, or OA_PLAYER_COUNT for none.
/// @return False for a null world, no players or no free watcher slot.
[[nodiscard]] bool
demo_bind_players(const DemoPlayback& playback, World* world, uint8_t* watcher_slot) noexcept;

// Full records of one kind of unit that placed a unit an earlier record
// already placed.
struct FullRecordDrift {
    uint32_t measured{};
    uint32_t moved{};     // measured placements whose recorded position changed
    uint64_t drift_sum{}; // world units over measured placements
    uint32_t drift_max{};
};

// Every detached full unit record the replay applied to a live unit. Between
// two records a remote unit moves only by its remote driver, so drift is the
// distance the replay failed to reproduce.
struct FullRecordStats {
    uint32_t placements{};
    FullRecordDrift ground{}; // units that cannot fly
    FullRecordDrift air{};
};

struct FullRecordSample {
    uint32_t tick{};
    FixedVec3 before{};
    FixedVec3 recorded{};
};

/// State the program that runs a playback keeps outside the playback and the match, which the
/// next tick of the match reads (the meteor storm), and so its keyframes carry.
struct KeyframeExtension {
    void* context{};
    /// Writes the state as sections of a keyframe.
    ///
    /// @param context The extension's context.
    /// @param out The keyframe's writer.
    void (*save_sections)(void* context, sim::match_runtime::keyframe::Writer* out){};
    /// Puts the state back from a keyframe's sections.
    ///
    /// @param context The extension's context.
    /// @param in The keyframe's reader.
    /// @return False when the sections do not fit.
    bool (*load_sections)(void* context, sim::match_runtime::keyframe::Reader* in){};
};

/// The digest a replay that began at a restore must have when it reaches a tick.
struct KeyframeExpectation {
    uint32_t tick{};
    uint64_t digest{};
};

/// A restored replay that reached the next keyframe's tick with another digest: a part of the
/// playback's state the keyframe did not carry.
struct KeyframeFault {
    uint32_t tick{};     ///< the tick both digests belong to
    uint64_t expected{}; ///< the digest the keyframe taken at that tick holds
    uint64_t found{};    ///< the digest the replay reached
};

struct DemoSession {
    DemoPlayback playback;
    sim::match_runtime::Match* match{};
    netgame::match::NetConnection connection{}; // packets over demo_transport, no host
    netgame::match::NetMatch net{};
    netgame::match::MatchBinding binding{};
    uint8_t watcher_slot{};
    uint32_t tick_errors{};
    std::string last_error;
    std::vector<std::string> lines; // chat and notices since the last drain
    /// The viewer reads chat as UTF-8: a recorded line kept in UTF-8 shows
    /// as it is, else in the code page (netgame::match::net_match_chat_text).
    /// demo_session_begin passes it to the match; set it again while bound
    /// through net.unicode_chat.
    bool unicode_chat{};
    /// The context `translate_game_text` takes back.
    void* game_text_context{};
    /// Translates one of the game's own texts into the language shown, as
    /// netgame::match::NetMatchHooks::translate_game_text does:
    /// demo_session_begin passes it to the match, which builds the recorded
    /// game's notices from such texts. Null, or a null return, shows the
    /// English.
    const char* (*translate_game_text)(void* context, const char* english){};
    FullRecordStats full_records;
    std::vector<uint16_t> placed_type; // by unit slot: type at its last full record
    std::vector<FixedVec3> placed_at;  // by unit slot: position of that record
    uint16_t tracked_unit{};           // first recorded player's first unit
    std::vector<FullRecordSample> tracked;
    // Each slot's last economy record, under recorder.ten-player-replay
    // watcher-view (demo_follow_economy).
    std::array<EconomySample, OA_PLAYER_COUNT> economy_samples{};
    /// The caller's state a keyframe also carries; none by default.
    KeyframeExtension keyframe_extension{};
    /// The bytes of the first keyframe's fixed sections (docs/replays.md), which every keyframe
    /// of this session is stored as the difference from. Empty before the first keyframe.
    std::vector<uint8_t> keyframe_base;
    /// The keyframe a replay that began at a restore is checked against when it reaches that
    /// keyframe's tick; empty when none is being checked.
    std::optional<KeyframeExpectation> keyframe_expected;
    /// Replays that reached a keyframe's tick with another digest, oldest first, until the
    /// caller reads and clears them.
    std::vector<KeyframeFault> keyframe_faults;
    /// Room a take compresses into and a restore decompresses into. Not part of a
    /// keyframe: write_session names each field it keeps, and this buffer is not one.
    std::unique_ptr<uint8_t[]> keyframe_scratch{};
    /// How many bytes keyframe_scratch holds. It grows, with spare room past the
    /// request, and does not shrink.
    std::size_t keyframe_scratch_bytes{};
    /// The room the kept writer holds after the last take that passed the budget:
    /// both regions' capacity, which may be more than the bytes written. Counted
    /// with the compress buffer. Not part of a keyframe: write_session names each
    /// field it keeps, and this size is not one.
    std::size_t keyframe_image_bytes{};
    /// The writer's image, kept for the next take. A take builds into this buffer
    /// instead of a new one, and a take the budget refuses releases it. Not part of
    /// a keyframe: write_session names each field it keeps, and this writer is not one.
    sim::match_runtime::keyframe::Writer keyframe_image{};
};

/// Binds a match to the recording: player table, unit ranges, the packet layer over the recording and the
/// in-game pump.
///
/// The match must be built at the recording's unit limit with its outcomes
/// already configured with defeat disabled. demo_session_end releases the
/// packet layer.
///
/// Under recorder.ten-player-replay the match learns how a viewer without a
/// slot of its own sees (demo_slotless_viewer). Such a viewer is shown every
/// recorded chat line, as a seated watcher is. Under watcher-view each
/// recorded player's economy records also give it income figures
/// (demo_follow_economy), with a slot for the viewer or without.
///
/// @param[in,out] session Session holding a loaded playback; any previous binding is ended first.
/// @param match Match runtime to drive.
/// @param[out] error Reason for a failure.
/// @return False for a null session or match, a different unit limit or index width, or no free watcher slot.
[[nodiscard]] bool
demo_session_begin(DemoSession* session, sim::match_runtime::Match* match, std::string* error);

/// Runs one multiplayer tick: pumps the frames due by then, simulates unless paused and flushes.
///
/// A pause the recording carries is cleared once the tick has run, so
/// playback runs through the recorded pauses. Simulation exceptions are
/// counted in tick_errors and the last one kept.
///
/// @param[in,out] session Bound session; ignored when unbound.
void demo_session_frame(DemoSession* session);

/// Releases the packet layer and unbinds the match.
///
/// @param[in,out] session Session to end; null is ignored.
void demo_session_end(DemoSession* session) noexcept;

/// Tells whether every frame was delivered and no record is still held by the packet layer.
///
/// @param session Session to test.
/// @return True when playback is complete.
[[nodiscard]] bool demo_session_finished(const DemoSession& session) noexcept;

// How far a gap between two of the tracked unit's full records may stray
// from units_per_player ticks: delivery spreads a frame's records over up to
// 30 ticks after a send interval of 6.
inline constexpr int64_t full_record_slack_ticks = 36;

// A replay's verdict so far.
struct DemoVerdict {
    // No record failed (forgiven when the unit table differs), no unit was
    // created past the unit table (likewise forgiven), no tick failed and no
    // create was refused.
    bool clean{};
    // Every gap between the tracked unit's consecutive full records is
    // units_per_player ticks, give or take full_record_slack_ticks.
    bool paced{};
};

/// Judges a replay so far: whether it ran clean and whether the tracked unit's full records came paced.
///
/// Delta layouts follow the receiving unit's definition, so over a unit
/// table that differs from the recording's, unit states cannot decode
/// reliably and record errors do not count, nor do creates of indices a
/// larger recorded table reached. The spacing is the bound match's
/// units_per_player, or the recording's unit limit while unbound.
///
/// @param session Session to judge, bound or not.
/// @param unit_table_differs Whether the match's unit table differs from the recording's.
/// @return The verdict.
[[nodiscard]] DemoVerdict
demo_session_verdict(const DemoSession& session, bool unit_table_differs) noexcept;

/// Hashes the live units in slot order and the player economies with FNV-1a.
///
/// @param world World to hash, or null.
/// @return The digest; the FNV offset basis for a null world.
[[nodiscard]] uint64_t demo_world_digest(const World* world) noexcept;

/// Counts the occupied unit slots.
///
/// @param world World to count, or null.
/// @return Units with a nonzero type; 0 for a null world.
[[nodiscard]] uint32_t demo_live_units(const World* world) noexcept;

/// Ticks between the keyframes near the play head: ten seconds of game time.
inline constexpr uint32_t keyframe_near_interval_ticks = 300;
/// Ticks between the keyframes away from the play head: sixty seconds of game time.
inline constexpr uint32_t keyframe_far_interval_ticks = 1800;
/// How far either side of the play head the near interval holds: two minutes of game time.
inline constexpr uint32_t keyframe_near_window_ticks = 3600;
/// The most memory a replay's keyframes take on a computer.
inline constexpr std::size_t keyframe_budget_bytes = std::size_t{256} << 20;
/// The most memory a replay's keyframes take on iOS.
inline constexpr std::size_t keyframe_budget_bytes_ios = std::size_t{96} << 20;
/// The zstd level a keyframe is compressed at.
inline constexpr int keyframe_zstd_level = 1;
/// The refusal a take gives when the keyframes already kept, the base, the writer's
/// image and the compress buffer would pass the budget. The caller says this to the
/// player in its own words.
inline constexpr char keyframe_over_budget[] = "over the memory budget";

/// A playback session's state at one tick, compressed, in memory.
///
/// Holds what the next tick of the session reads: the world, the match runtime, the
/// network match with the packet layer's held frames and the binding, and the playback's
/// cursor and statistics. It holds no sound and no drawn picture, but it copies the world's
/// tables whole, so the camera and interface fields of the game state and each unit's selection
/// flag come back as they were (docs/replays.md); the chat ring, the video capture's settings
/// and the profiler's times stay as the player has them. It is only meaningful to the session it
/// was taken from: the match's sections hold this run's addresses, and the first keyframe a
/// session takes is the base the others' fixed sections are stored as the difference from.
struct DemoKeyframe {
    uint32_t tick{};             ///< Game.tick when it was taken
    uint64_t digest{};           ///< demo_world_digest at that tick
    uint64_t image_digest{};     ///< FNV-1a over the uncompressed image
    std::size_t fixed_bytes{};   ///< the image's fixed sections' size
    std::size_t varying_bytes{}; ///< the image's other sections' size
    /// Whether the fixed sections are stored as the base's difference. Not when a section was left
    /// out, which changes their size.
    bool delta{};
    std::vector<uint8_t>
        frame; ///< the image as one zstd frame, the fixed sections as the base's difference
    /// The network match, copied whole: it holds no bytes the image could, but only values.
    std::shared_ptr<const netgame::match::NetMatch> net;
    /// What the image compresses to without the base, for the measurement; 0 when not measured.
    std::size_t plain_frame_bytes{};

    /// Returns the raw size of the image.
    ///
    /// @return Fixed plus varying bytes.
    [[nodiscard]] std::size_t raw_bytes() const noexcept { return fixed_bytes + varying_bytes; }

    /// Returns what the keyframe takes of memory.
    ///
    /// @return The frame, the network match copy and the keyframe itself.
    [[nodiscard]] std::size_t memory_bytes() const noexcept;
};

/// What taking a keyframe asks for beyond the keyframe.
struct DemoKeyframeOptions {
    /// Sections to leave out, for a test.
    sim::match_runtime::keyframe::Omission omission{};
    /// Also compress the image without the base, to measure what the base saves. It copies the
    /// image and compresses it a second time, so a take that is being timed leaves it off.
    bool measure_plain{};
    /// Takes the image only: nothing is compressed and the keyframe holds no frame, so it cannot
    /// be restored. For a test that compares what two moments hold.
    bool image_only{};
    /// Receives the uncompressed fixed sections, when not null.
    std::vector<uint8_t>* image_fixed{};
    /// Receives the uncompressed varying sections, when not null.
    std::vector<uint8_t>* image_varying{};
    /// Hashes the uncompressed image into image_digest. Off unless a test asks,
    /// so a take the viewer times does not walk the image.
    bool hash_image{};
    /// The most the base, the keyframes already held, this take's compress
    /// buffer and the image writer's room may take. Zero skips the check, which
    /// a unit test does.
    std::size_t budget_bytes{};
    /// Bytes of keyframes already held, not counting the base or the working buffer.
    std::size_t committed_bytes{};
};

/// Takes a keyframe of a bound playback session.
///
/// The session's first keyframe also becomes its base, unless the take is refused
/// before that. A session that holds state a keyframe cannot carry (an order, a
/// computer player, a unit steered on this machine) is refused, never kept wrongly.
/// The two regions are compressed as one frame straight from the writer, into one
/// buffer the session keeps and reuses. The image is hashed only when
/// options.hash_image is set. What a take takes of time is in docs/replays.md.
///
/// @param[in,out] session Session bound with demo_session_begin, between two frames.
/// @param[out] key The keyframe.
/// @param[out] refusal The reason, when the session cannot be kept; may be null.
/// @param options Sections to leave out and what to hand back; none by default.
/// @return False when the session is unbound or holds state a keyframe cannot carry.
[[nodiscard]] bool demo_keyframe_take(
    DemoSession* session,
    DemoKeyframe* key,
    std::string* refusal,
    const DemoKeyframeOptions& options = {}
);

/// Puts a bound playback session back to the state a keyframe holds.
///
/// The replay then plays on from the keyframe's tick as it did from there the first time.
/// When `next` is given, the session compares its world digest with `next`'s when it reaches
/// `next`'s tick, and reports a difference in `keyframe_faults`.
///
/// @param[in,out] session The session the keyframe was taken from, bound, between two frames.
/// @param key The keyframe to restore.
/// @param next The keyframe at the next tick the caller holds one for, or null.
/// @param[out] error The reason, when the keyframe does not fit the session; may be null. A
///        session whose restore failed is unusable: the caller ends the playback.
/// @return False when the keyframe cannot be read or does not fit the session.
[[nodiscard]] bool demo_keyframe_restore(
    DemoSession* session,
    const DemoKeyframe& key,
    const DemoKeyframe* next = nullptr,
    std::string* error = nullptr
);

/// Names the sections in which two uncompressed images differ, the first eight at most.
///
/// @param first_fixed The first image's fixed sections.
/// @param first_varying The first image's varying sections.
/// @param second_fixed The second image's fixed sections.
/// @param second_varying The second image's varying sections.
/// @return Each section, with the first byte that differs and how many do, separated by
///         semicolons; empty when the images are the same bytes.
[[nodiscard]] std::string demo_keyframe_difference(
    std::span<const uint8_t> first_fixed,
    std::span<const uint8_t> first_varying,
    std::span<const uint8_t> second_fixed,
    std::span<const uint8_t> second_varying
);

/// Lists the sections of an uncompressed image with their sizes.
///
/// @param varying The image's varying sections, whose marks name every section.
/// @return Each section's name and byte count (0 for a section left out), in order.
[[nodiscard]] std::vector<std::pair<std::string, std::size_t>>
demo_keyframe_sections(std::span<const uint8_t> varying);

/// Returns the bytes the session's base keyframe holds.
///
/// @param session The session.
/// @return The bytes, 0 before its first keyframe.
[[nodiscard]] std::size_t demo_keyframe_base_bytes(const DemoSession& session) noexcept;

/// The viewer's picture of the world, kept across a restore so a jump gives it back.
///
/// The chat ring is not part of the picture. A restore leaves the ring, and the
/// recording would post those lines again, so giving the picture back clears it.
struct ViewerPicture {
    bool paused{}; ///< the pause bit of Game.sim_run_flags
    uint8_t viewpoint_player{};
    uint8_t local_player_index{};
    uint32_t follow_unit{};
    uint32_t follow_target{};
    FixedVec3 follow_point{};
    int16_t follow_point_ticks{};
    int32_t camera_slot_x[4]{};
    int32_t camera_slot_y[4]{};
    uint8_t camera_slot_valid[4]{};
    uint8_t debug_overlay{};
    /// One byte a slot: 1 when the viewer had that unit selected.
    std::vector<uint8_t> selected{};
};

/// Copies the viewer's picture out of a world. A null picture is ignored.
///
/// @param world the world the viewer is watching
/// @param[out] picture the picture
void take_viewer_picture(const World& world, ViewerPicture* picture);

/// Writes a picture back, and clears the chat ring.
///
/// Only the pause bit of Game.sim_run_flags is changed. An empty unit slot
/// loses a selection, and a slot the picture did not select loses one too.
/// A null world is ignored.
///
/// @param[in,out] world the world a restore just wrote
/// @param picture the picture taken before that restore
void give_viewer_picture(World* world, const ViewerPicture& picture);

} // namespace oa::session::demo
