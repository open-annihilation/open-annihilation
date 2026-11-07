// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Network play's state for one runtime and the work it does on it: the
// network session behind the multiplayer screens and the networked match,
// .tad playback, the match report and the console's network commands.
// Network play's extension owns one for each runtime its hooks are called
// for (extension.cpp) and frees it as that runtime is destroyed
// (Extension::release_runtime). It is Runtime's one friend from network
// play; what it uses of Runtime's private part only shrinks
// (netgame-runtime-surface, tools/runtime-surface-baseline.json).
#pragma once

#include "oa/app/netgame/opened_launch.hpp"
#include "oa/app/presence_facts.hpp"
#include "oa/app/runtime.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/session/demo/clock.hpp"
#include "oa/sim/match_runtime.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

struct NetState;       // net_state.hpp
struct DemoState;      // demo_state.hpp
struct SkirmishRecord; // runtime_net.cpp

/// Network play's state for one runtime, and the work network play does on that runtime.
class NetworkPlay {
  public:

    /// Creates network play's state for a runtime: no session, no playback.
    ///
    /// @param runtime The runtime it belongs to; it outlives the state.
    explicit NetworkPlay(Runtime& runtime) noexcept;

    NetworkPlay(const NetworkPlay&) = delete;
    NetworkPlay& operator=(const NetworkPlay&) = delete;

    /// Ends the playback, then the session, as the runtime is destroyed.
    ~NetworkPlay();

    /// Returns network play's state for a runtime, creating it on first use.
    ///
    /// @param runtime The runtime.
    /// @return Its state, which lives until free_for frees it.
    static NetworkPlay& of(Runtime& runtime);

    /// Returns network play's state for a runtime when it has any.
    ///
    /// @param runtime The runtime.
    /// @return Its state, or null before the first use.
    [[nodiscard]] static NetworkPlay* find(const Runtime& runtime) noexcept;

    /// Frees network play's state for a runtime that is being destroyed (Extension::release_runtime).
    ///
    /// The playback ends first, then the session's connection is destroyed.
    /// Nothing happens for a runtime with no state.
    ///
    /// @param runtime The runtime.
    static void free_for(const Runtime& runtime) noexcept;

    // Networked match (runtime_net.cpp): the session behind the multiplayer
    // screens, the launch from the battle room and the per-tick binding.

    /// Destroys a network session, releasing its connection storage when it was allocated.
    ///
    /// @param state Session to destroy; null is allowed.
    static void destroy_net_state(NetState* state) noexcept;

    /// Destroys a skirmish recording; null is allowed.
    ///
    /// @param record Recording to destroy.
    static void destroy_skirmish_record(SkirmishRecord* record) noexcept;

    /// Binds the multiplayer screens to the network session, creating the session on first use.
    ///
    /// The match report's hold beside the game is released first
    /// (release_match_report). A new session takes the
    /// "-p" send pacing when one was given; when its connection storage cannot
    /// be allocated the failure is reported on stderr and the screens get no
    /// transport. The screens' start hook is bound to this state, their
    /// translation of interface texts (translate_ui) to its runtime, and
    /// their battle room takes the "-t" player timeout either way.
    void net_bind_multiplayer();

    /// Binds what the multiplayer screens and the session read of the
    /// profile the game plays (Runtime::mod_profile, Runtime::ui_rules): its
    /// network rules, its setup and team rules, and the battle room buttons
    /// its display rules add.
    void bind_profile_rules();

    /// Binds the profile's rules again once they differ from those bound:
    /// the settings' Developer Mode lays its overrides over the profile as
    /// the settings are read, after the screens are first bound, and changes
    /// them while the game runs, outside a match. Run every frame.
    void follow_profile_rules();

    /// Tells whether this machine sends and reads chat as UTF-8: Unicode
    /// multiplayer chat is on (Runtime::unicode_chat_on).
    ///
    /// @return true when it does
    [[nodiscard]] bool unicode_chat() const;

    /// Passes unicode_chat to the battle room, the network match and a
    /// recording's playback, which follow it from their next line and
    /// setup block. Run every frame.
    void follow_unicode_chat();

    /// Passes this machine's presence record to the battle room
    /// (multiplayer_bind_presence_record), which sends it to the OA players
    /// of other machines. Run every frame.
    ///
    /// The facts (presence_facts) are read while the multiplayer screens
    /// show, and on the first call. The record carries this build's version,
    /// the platform and architecture, the mod and where it was published, the
    /// sim hash, the mutators the battle room is playing, the game and view
    /// hacks with their counts, and Developer Mode with its overrides; the
    /// host's map pack is the battle room's to add. It is written within
    /// presence_record_max_bytes less presence_map_pack_field_max_bytes, so
    /// that the host's pack always fits beside it, unless its fixed fields
    /// alone need more. A fact that breaks the record's rules is left out,
    /// with one line on the error stream the first time. The battle room is
    /// told when the record's bytes change. The full mutator list is bound
    /// even when those bytes are cut to the same record, so a part can still
    /// be drawn from it. The room's layer is followed first, including on
    /// the way out of the multiplayer screens; a change to its field while
    /// the screens are hidden is written when they show again. No setup
    /// block carries any of it.
    void follow_presence();

    /// Runs one frame of the network match; nothing without an active match.
    ///
    /// While loading it runs the load barrier and finishes the load once the
    /// barrier passes (a failure aborts the match). While the match goes on
    /// (Runtime::match_running: on its screen, or beneath the preferences its
    /// menu opens) it sends local speed changes, announces a pause set on
    /// another player's machine, applies the received speed, pumps a paused
    /// frame while the pause bit is set, syncs the match timing, watches the
    /// player timeouts and steps the match report. The Pause key alone pauses (the
    /// extension's pause_changed); a menu, and a finished game holding on its
    /// outcome until its final economy settles, send no pause: the other
    /// players play on, and the timeouts still drop a player who stops
    /// answering. Each frame of the match also shares the cameras
    /// (share_cameras).
    void net_frame();

    /// Shares this machine's camera and takes the other machines' cameras
    /// (ui.camera-sharing): the camera's centre goes to the match, which
    /// sends it with the recorder's traffic, and the cameras received go to
    /// the runtime's minimap, camera lock and resource panel.
    void share_cameras();

    /// Runs one simulation tick through the network binding.
    ///
    /// Under the lag guard (network.lag-guard) a step while every other
    /// player has been silent for the guard's period runs at most once per
    /// period; a step it holds runs no tick and still counts as the
    /// binding's.
    ///
    /// @return True when the networked binding ran or held the tick; false
    ///         without a running (loaded) network match.
    bool net_simulation_step();

    /// Starts a skirmish or campaign recording on the first tick, when one is due.
    ///
    /// The local player and every computer player send the records a network
    /// game would send, through a transport that only records. The match's
    /// unit ranges stay as the match dealt them. A later tick does nothing.
    /// A network match and a replay are left alone.
    void consider_skirmish_recording();

    /// Raises the recording's send bit for this tick and records what the last
    /// tick left: a unit a command made while the bit was down, and each
    /// sender's economy on the beat a network game sends it.
    ///
    /// Nothing without a recording. The engine's own step then runs, and the
    /// bit stays up through that step so its creates, deaths, shots and
    /// feature changes are recorded.
    void advance_skirmish_recording();

    /// Clears the live-game bit when the recording set it.
    ///
    /// A bit something else set, a multiplayer game or a check, is left set.
    /// Nothing without a recording.
    void hide_skirmish_live_bit();

    /// Tells whether a skirmish or campaign recording is packing on its own thread.
    ///
    /// A won or lost game starts that thread before the end screen opens. The
    /// torn-down event leaves the thread running, so the end screen does not
    /// wait for the pack. The next recording, or the destructor, waits for it,
    /// and the file is written either way.
    ///
    /// @return true while that thread has been started and not yet reaped
    [[nodiscard]] bool skirmish_packing() const noexcept;

    /// Finishes the skirmish or campaign recording, if one is open, and takes the hooks down.
    ///
    /// Each sender's economy is recorded once more, as a network game sends it
    /// when the match ends. The tick and the outcome are the running match's,
    /// so a won or lost game is recorded as finished with that length. A later
    /// call, once this one has closed the recording, waits for a pack that is
    /// still running and says where the file went. It does not write the end
    /// again. A run nobody watches finishes before this returns. A game a
    /// person plays packs on a thread of its own, and this returns without
    /// waiting for that thread.
    void finish_skirmish_recording();

    /// Prints how many map cells hold a feature, and a digest of those cells.
    ///
    /// The line is `LABEL: cells N digest DIGEST`. A cell counts when its
    /// feature is one of the map's own, and the digest folds each such cell's
    /// column, row and feature in order.
    ///
    /// @param label the line's name
    /// @param world the world whose map is read
    static void print_feature_cells(const char* label, const oa::World& world);

    /// Returns the wall clock the lag guard measures with.
    ///
    /// @return milliseconds on the steady clock
    [[nodiscard]] static uint32_t lag_guard_clock_ms();

    /// Returns how long the remote players have all been silent.
    ///
    /// @return milliseconds since any remote player still in the game was
    ///         last heard; 0 with none
    [[nodiscard]] uint32_t net_remote_silence_ms() const;

    /// Shares the Pause key's pause with the other players.
    ///
    /// While the lag guard holds the game the key does nothing: the bit the
    /// engine flipped is put back. Otherwise the pause goes to every player
    /// (net_match_set_pause), which may keep the game paused under the
    /// recorder's autopause.
    ///
    /// @param paused The pause bit after the key flipped it.
    void net_pause_key(bool paused);

    /// Tells whether the lag guard holds the game, which keeps the Pause key from working.
    ///
    /// @return false without a running network match or with the guard off
    [[nodiscard]] bool net_lag_guard_closed() const;

    /// Tests whether the local player of the active network match only watches.
    ///
    /// @return False without an active network match.
    [[nodiscard]] bool net_local_watcher() const;

    /// Tests whether a network match owns the session, loading or running.
    ///
    /// @return Whether a network match is active.
    [[nodiscard]] bool net_match_active() const;

    /// Tests whether the connection is in a session, from the multiplayer screens or a match.
    ///
    /// @return Whether the connection exists and is in a session.
    [[nodiscard]] bool net_session_open() const;

    /// Returns the session of the network match waiting on its load barrier.
    ///
    /// @return The session, or null when no network match is loading.
    [[nodiscard]] const NetState* loading_net_match() const;

    /// Returns the session of the network match that owns it, loading or running.
    ///
    /// @return The session, or null when no network match is active.
    [[nodiscard]] const NetState* session_net_match() const;

    /// Leaves the active network match and closes its transport; nothing without one.
    ///
    /// With a match built, the connection finishes with its Game block: every
    /// channel is flushed and the session closed, which tells the other
    /// machines the local players left; no record announces it. The binding
    /// and the match state are then cleared, the transport closed and the
    /// multiplayer screens reset.
    void net_leave();

    /// Sends a chat line from the local player to every player as "<name> text".
    ///
    /// A line that reaches everyone (a chat mode other than chosen players,
    /// allies or local-only) also goes to the match report (report_chat). Nothing is sent
    /// without a running network match or for empty text.
    ///
    /// @param text Chat text without the speaker prefix.
    void net_say(std::string_view text);

    /// Sends an already formatted chat line to every player.
    ///
    /// Nothing is sent without a running network match or for an empty line.
    ///
    /// @param line Chat line, speaker prefix included; null sends nothing.
    void net_send_chat(const char* line);

    /// Sends a chat line a computer seat of this machine said, from that seat.
    ///
    /// Nothing is sent without a running network match or for an empty line.
    ///
    /// @param seat Player slot of the computer seat.
    /// @param line Formatted chat line; null sends nothing.
    /// @param mode Chat mode the line was said in.
    /// @param targets Chosen-player slots, or null when the mode names none.
    void net_send_seat_chat(uint8_t seat, const char* line, uint8_t mode, const uint8_t* targets);

    /// Gives the lines the chat line net_send_chat just sent went out as,
    /// when Unicode chat cut it into more than one record.
    ///
    /// @param line the line net_send_chat sent
    /// @param index which of them
    /// @return the line at `index` in UTF-8; null past the last, and for a
    ///         line that went out as one or was not sent
    [[nodiscard]] const char* shared_chat_line(const char* line, std::size_t index) const;

    /// Gives the bytes a chat line net_send_chat sends may hold, its end
    /// included: while Unicode chat may cut it into several records, what
    /// they carry, oa::netgame::chat_line_parts records' worth.
    ///
    /// @return the bytes; 0 without a running network match or with
    ///         Unicode chat off, where one record carries the line
    [[nodiscard]] std::size_t shared_chat_line_bytes() const;

    /// Resets the traffic statistics of the connection's packet queue at the current tick.
    ///
    /// The console's "NetStats" and every console setup call it; the
    /// statistics feed the "BPS" and debug-key traffic readouts. Without a
    /// connection or packet queue there is nothing to reset.
    void net_reset_traffic_stats();

    /// Sends every local player's lobby block and team to all players; nothing without an active match.
    void net_send_player_status();

    /// Pumps a finished network game and tests its final-economy gate.
    ///
    /// A finished network game leaves once every peer holds the local final
    /// economy.
    ///
    /// @return True once the gate has passed, or when no loaded network match
    ///         runs; false keeps the outcome screen, which retries each frame.
    bool net_final_economy_settled();

    /// Gives resources between players of a network match.
    ///
    /// The match debits, credits and sends the resource record, as the share
    /// transfers do; nothing is given while the match loads.
    ///
    /// @param from Giving player index.
    /// @param to Receiving player index.
    /// @param metal True for metal, false for energy.
    /// @param amount Amount given.
    /// @return False when no network match is active, so the caller gives locally.
    bool net_give(uint8_t from, uint8_t to, bool metal, float amount);

    /// Runs --net-loopback-check: hosts, and joins from a second in-process runtime, over 127.0.0.1.
    ///
    /// The two machines publish and join a session, sync their unit tables,
    /// launch through the load barrier and run the match, which the check
    /// compares between them; with --net-loopback-watcher the joiner only
    /// watches. With --net-loopback-photo the host's player enters photo
    /// mode, holds the picture and leaves. Progress goes to stdout.
    ///
    /// @param ticks Networked ticks to run once the match has started.
    /// @return 0 when every check passes; a failed check throws std::runtime_error.
    int run_net_loopback_check(std::size_t ticks);

    /// Selects a map by the name lobbies and recordings use (runtime_netgame.cpp).
    ///
    /// The name is first tried as a map file stem. A name with a package
    /// suffix then matches only that way, so it is not paired with another
    /// map's title. Any other name is searched for among the installed maps
    /// as an OTA mission name, ignoring case.
    ///
    /// @param name Map file stem or OTA mission name.
    /// @return Whether a map was selected; false for an empty name or a
    ///         suffixed name that is not installed.
    bool select_map_named(std::string_view name);

    /// Draws a network load's player bars and ready count over the loading screen.
    ///
    /// Each bar is drawn in the player-bar colour, filled in the done colour
    /// up to the player's progress, with the player's name over it; the names
    /// and the status text use the text colour the last category row set.
    /// 3.1c redraws the root gadget panel first; this loading screen has no
    /// such panel. Nothing is drawn unless a network match is loading.
    ///
    /// @param[in,out] target Surface of the loading screen.
    /// @param font Font of the names and the status text.
    void draw_loading_players(oa::Surface& target, const oa::present::GafSprites* font);

    // .tad playback (runtime_demo.cpp).

    /// Ends a demo playback session and frees it.
    ///
    /// @param session Session to destroy.
    static void destroy_demo_session(DemoState* session) noexcept;

    /// Opens the --play-demo recording and builds a match to watch it in.
    ///
    /// A recording the engine handed over (NetgameContext::staged_recording)
    /// is taken and opened in its place, and its strict flag stands for
    /// --demo-unit-table. The recording's map must be installed and a slot
    /// left for the watcher. The match is built for the first recorded
    /// player's view with the recorded unit-sync verdicts; unless
    /// --demo-unit-table ignore is given the installed unit table must match
    /// the recording's. The recorded players then become remote and the
    /// watcher local, the sight grids are rebuilt and the view is the
    /// watcher's. Throws std::runtime_error when the recording cannot be
    /// opened or played.
    void start_demo_playback();

    /// Delivers one tick of recorded packets to the demo match.
    ///
    /// Tick errors are reported and the recording's chat lines posted. The
    /// session ends when its match is no longer the running one.
    void step_demo_frame();

    /// Runs --replay-keyframe-check on the playback that was just started: a keyframe at the
    /// start, the replay to `at`, a keyframe there, 3,000 ticks, the same 3,000 ticks again from
    /// the restored keyframe, and a line that says whether both runs ended on the same digest.
    /// Leaves the replay at the end of the second run, to go on from.
    ///
    /// @param at The tick to take the keyframe at.
    /// @param[out] finished Set when the replay has played everything it holds.
    /// @return False when a keyframe could not be taken or restored, or the restored replay
    ///         differed from the first.
    [[nodiscard]] bool run_keyframe_check(uint32_t at, bool* finished);

    /// Notes a recorded game speed and the viewer's sound, without writing the viewer's speed.
    ///
    /// A recorded speed change still paces playback, multiplied by the viewer's
    /// speed. It no longer copies itself over the speed the viewer chose.
    void demo_frame();

    /// Runs the ticks one frame of a replay owes, and seeks that are due.
    ///
    /// The game clock does not pace a replay. Only the last tick of a frame is
    /// drawn; a seek's ticks are not drawn until it lands. Pause and an open
    /// menu hold the clock.
    ///
    /// @param now_ms the frame's clock, in milliseconds
    /// @param host how the frame steps, draws and stops the match
    void run_replay_frame(uint32_t now_ms, const ReplayFrameHost& host);

    /// Forgets the time an open menu held, so the next frame is one frame.
    ///
    /// @param now_ms the frame's clock, in milliseconds
    void replay_hold(uint32_t now_ms);

    /// Plays a paused replay. The next frame runs at the viewer's speed.
    void replay_play();

    /// Holds the replay's clock. A recorded pause does not.
    void replay_pause();

    /// Sets the viewer's speed. A recorded speed change does not call this.
    ///
    /// @param speed one of the ladder speeds
    void replay_set_speed(oa::session::demo::PlaybackSpeed speed);

    /// Sets the viewer's speed from thousandths of 1x. Another value changes nothing.
    ///
    /// @param milli 500, 1000, 2000, 4000, 8000 or 16000
    void replay_set_speed_milli(int32_t milli);

    /// Steps the viewer's speed one place, as + and - do while a replay plays.
    ///
    /// @param delta positive to go faster, negative to go slower
    void replay_step_speed(int delta);

    /// Asks to jump to a tick. The jump runs on the next frame.
    ///
    /// @param play_tick the tick to land on
    void replay_seek_to(uint32_t play_tick);

    /// The tick, the length known so far, the furthest point watched, the spans
    /// ready to jump to, a catch-up's progress and the speed reached.
    ///
    /// @return that state; an empty one when no replay is playing
    [[nodiscard]] oa::session::demo::PlaybackState replay_playback_state() const;

    /// Fills the view the replay_state hook returns. The spans and the sentence
    /// stay valid until the next call.
    ///
    /// @param[out] out the view
    /// @return false when no replay is playing
    bool replay_fill_state(ReplayPlaybackView* out);

    /// Takes the sentence a failed jump left for the player, and clears it.
    ///
    /// @return the sentence, or empty when there is none
    std::string take_replay_stop_notice();

    /// Places the headless snapshot camera: at --camera when given, else on the followed unit.
    void place_demo_camera();

    /// Replays up to `ticks` networked ticks headlessly and reports the replay's statistics.
    ///
    /// Prints the recording, the final digest and the full unit records the
    /// recording carried for the followed unit, with how far every unit had
    /// drifted from each of its full records; writes --snapshot when given.
    /// With --headless-check it first checks that Enter opens no chat line
    /// for the watcher.
    ///
    /// @param ticks Most ticks to replay.
    /// @return 0 when the replay was clean and the followed unit's full records
    ///         came units_per_player ticks apart; 1 otherwise.
    int run_headless_demo(std::size_t ticks);

    // The match report (runtime_report.cpp), kept with the network session.

    /// Returns the world the match report reads: the running match's, else the end-of-game screen's.
    ///
    /// @return The world, or null when neither exists.
    oa::World* reporter_world();

    /// Starts the match report for a network game.
    ///
    /// The end-of-game screen's state, which the report needs, is created here.
    /// When the lobby kept its own Game block, its provider GUID and game name
    /// are copied into the match's. The battle room launches straight into the
    /// match, so the battle-room and launch events are reported at once.
    ///
    /// @param[in,out] world Match world; its Game block receives the session details.
    /// @param lobby_game The lobby's Game block; null or the match's own block copies nothing.
    void start_reporter(oa::World& world, const oa::Game* lobby_game);

    /// Reports a game event to every route; nothing before the end-of-game state exists.
    ///
    /// @param event An extension_api::report_event value.
    void report_game_event(int32_t event);

    /// Remembers a chat line as the last one reported and reports it.
    ///
    /// @param line Chat line; null is kept as empty.
    void report_chat_line(const char* line);

    /// Runs the match report's per-frame step on the running match.
    void step_reporter_frame();

    /// Reports the closed session, then closes the match report; nothing if it never started.
    void close_reporter();

    /// Asks the extension to release what its match report holds beside the game, as opening the provider list does.
    void release_match_report();

    // Drop, Share*, Compression, Page and Senderror (runtime_console_network.cpp).

    /// Binds the console's network commands to this state.
    ///
    /// A share toggle re-sends the local players' lobby blocks, "NetStats"
    /// resets the traffic statistics and "Page" pages through the extension
    /// built on network play while a launch is active. The commands are
    /// added through the host's extend hook.
    ///
    /// @param[in,out] host Console host whose player_info_changed,
    ///                     extension_context and extend are set.
    void bind_console_network_hooks(oa::ui::console::ConsoleHost& host);

    /// Checks the console's network commands outside a live network game.
    ///
    /// The share toggles and "Compression" must do nothing; "Drop" must set
    /// its bit for 0 (or no value) and clear it for anything else; "Page" must
    /// say what it needs; the developer command "Senderror" must take 0..100
    /// from a single value and any other value as 0. A failure throws
    /// std::runtime_error.
    ///
    /// @param enter_line Types one chat line into the console.
    void check_console_network_commands(const std::function<void(const char*)>& enter_line);

    /// Checks the same commands in a live network game against `peer`.
    ///
    /// The share toggles must reach the peer's copy of this machine's lobby
    /// block, "Compression" must send every frame stored, "Senderror 100" must
    /// drop every frame before the transport (still counted as sent) and
    /// "Drop 0" must keep the stall check from naming a silent peer. A failure
    /// throws std::runtime_error.
    ///
    /// @param peer The other machine of the loopback game.
    /// @param step Runs one networked tick on this machine, and on the peer too
    ///             when passed true.
    void
    check_console_network_session(Runtime& peer, const std::function<void(bool peer_runs)>& step);

    /// Checks the console's cheat gate in a network game against `peer` (runtime_session_cheats.cpp).
    ///
    /// Every machine's cheat flag must be the host's CHEATING option, which the
    /// Game Settings sheet shows as its Cheat Codes row. The host's "+atm"
    /// must run only when the option is on, and either way its line must reach
    /// the other machine and the match report. A watching peer's Enter must
    /// open no chat line; a playing peer's "+atm" must follow the option and
    /// reach this machine the same way. The shared commands and the takeover
    /// cheats are checked by Runtime::check_console_network_takeover. A failure
    /// throws std::runtime_error.
    ///
    /// @param peer The other machine of the loopback game.
    /// @param step Runs one networked tick on this machine, and on the peer too
    ///             when passed true.
    void
    check_console_session_cheats(Runtime& peer, const std::function<void(bool peer_runs)>& step);

    /// Returns the --match-ticks run length, when one was given.
    ///
    /// @return The ticks, or nothing without --match-ticks.
    [[nodiscard]] std::optional<std::size_t> match_ticks() const;

    /// Posts a player's departure from a network game to the match's message log.
    ///
    /// @param[in,out] world Match world the departure is posted to.
    /// @param player_id Session id of the player who left.
    void post_departure(oa::World& world, uint32_t player_id);

    // Launch files the system opens while the game runs (runtime_opened_launch.cpp).

    /// Returns what the game plays, for comparing an opened launch's content: the mounted
    /// base game and expansions where it plays the player's copy, and the mod in play, read
    /// through Runtime's public methods.
    ///
    /// @return the content
    [[nodiscard]] netgame::RunningContent running_content() const;

    /// Posts a line to the running match's message log, as a notice from no player, through
    /// the console host the engine hands network play as each match starts.
    ///
    /// @param line the line, already in the language shown
    /// @return false when no match's console host is kept, and nothing was posted
    bool post_match_line(std::string_view line);

  private:

    // The callbacks handed to the multiplayer screens and the net match, and
    // the launch steps they drive (runtime_net.cpp).
    struct NetHost;

    /// Translates one of the game's own texts into the language shown, for the
    /// net match and a recorded game's playback
    /// (NetMatchHooks::translate_game_text).
    ///
    /// Never throws: a failed translation gives null, which shows the English.
    ///
    /// @param play this network play
    /// @param english the English text, as the game's translate.tdf keys it
    /// @return the translation, valid until the next call; or null
    static const char* translate_text(void* play, const char* english) noexcept;

    // Network play's hooks (extension.cpp), which reach this state.
    friend struct RuntimeExtension;

    /// Sets the keyframe budget, the viewer's speed and the seeks the command line named.
    void arm_replay_controls();

    /// Wraps the match's point-sound hook so a fast replay can thin what plays.
    void wrap_replay_sound();

    /// Keeps a recorded speed that is inside the game's range. Zero leaves the last one.
    void note_recorded_speed();

    /// Mutes speech above 4× and while skipping, and brings it back at 4× and below.
    ///
    /// @param host the frame's host, which owns the speech bit
    void apply_replay_speech(const ReplayFrameHost& host);

    /// Notes the play head, takes a keyframe when one is due, then thins and evicts.
    ///
    /// @param play_tick the tick just reached
    void follow_play_head(uint32_t play_tick);

    /// Takes a keyframe at one tick. A refusal is logged once and playback goes on.
    ///
    /// @param play_tick the tick
    void take_replay_keyframe(uint32_t play_tick);

    /// Forgets keyframes the reel dropped.
    ///
    /// @param ticks the ticks to drop
    void drop_replay_keyframes(const std::vector<uint32_t>& ticks);

    /// Raises the length known so far from the play head and the recording's last frame.
    ///
    /// @param play_tick the tick just reached
    void note_replay_length(uint32_t play_tick);

    /// Runs the one seek that is armed, when the play head has reached its FROM.
    ///
    /// Only the first waiting jump is armed. It fires when the play head is at
    /// or past its FROM, and the next is armed when it lands. A FROM already
    /// passed fires at once. A jump asked while one is running is not this
    /// list: replay_seek_to replaces it.
    ///
    /// @param windowed true when the ticks go through the drawn match step
    /// @param[in,out] finished set when the replay has played everything it holds
    /// @param deadline the frame's time budget; ignored when bound is false
    /// @param bound true to stop at the deadline and carry the seek into the next frame
    /// @param host the frame's host; null on the headless path, which does not stop the match
    /// @return true when a seek ran
    bool perform_due_seeks(
        bool windowed,
        bool* finished,
        std::chrono::steady_clock::time_point deadline,
        bool bound,
        const ReplayFrameHost* host
    );

    /// Starts a jump. A restore that fails ends the playback. A jump back with no keyframe stays.
    ///
    /// @param jump the jump
    /// @param host the frame's host; null on the headless path
    void begin_seek(const oa::session::demo::ReplayJump& jump, const ReplayFrameHost* host);

    /// Runs a jump until it lands, the replay ends, or the frame's time is spent.
    ///
    /// @param windowed true when the ticks go through the drawn match step
    /// @param[in,out] finished set when the replay has played everything it holds
    /// @param deadline the frame's time budget; ignored when bound is false
    /// @param bound true to stop at the deadline and carry the jump into the next frame
    /// @param host the frame's host; null on the headless path
    void continue_seek(
        bool windowed,
        bool* finished,
        std::chrono::steady_clock::time_point deadline,
        bool bound,
        const ReplayFrameHost* host
    );

    /// The match's point-sound hook before a replay wrapped it, kept after the replay ends.
    oa::sim::match_runtime::Match::PointSoundHook replay_saved_sound_{};

    /// Milliseconds of the previous replay frame, so the next plans from the gap.
    uint32_t replay_last_ms_ = 0;
    /// False until the first frame, which plans nothing and starts the gap.
    bool replay_have_ms_ = false;
    /// The tick being run is not drawn, so its unit sounds stay quiet.
    bool replay_hide_tick_ = false;
    /// Milliseconds the unit-sound gate counts for the tick being run.
    uint32_t replay_sound_ms_ = 0;
    /// A sentence for the player after a jump stopped the replay. Shown once the match is gone.
    std::string replay_stop_sentence_{};

    /// The milliseconds since the previous frame, and that frame becomes the previous.
    ///
    /// The first frame is 0. The subtraction wraps with the 32-bit clock.
    ///
    /// @param now_ms the frame's clock
    /// @return the gap, in milliseconds
    uint32_t replay_elapsed(uint32_t now_ms);

    /// Runs one tick through the host, hiding its sounds when it is not drawn.
    ///
    /// @param host the frame's host
    /// @param draw true to draw the tick
    /// @return false when the tick did not run
    bool run_replay_tick(const ReplayFrameHost& host, bool draw);

    /// The sentence the player reads for an English line, with {time} filled in.
    ///
    /// @param english the catalogue line
    /// @param tick the tick {time} names; ignored when the line has no {time}
    /// @return the sentence
    std::string replay_player_line(const char* english, uint32_t tick) const;

    Runtime& runtime_;
    std::unique_ptr<NetState, void (*)(NetState*) noexcept> net_{nullptr, destroy_net_state};
    /// A skirmish kept as a recording. Null when this run is not recording one.
    std::unique_ptr<SkirmishRecord, void (*)(SkirmishRecord*) noexcept> skirmish_{
        nullptr, destroy_skirmish_record
    };
    /// The running skirmish has already decided whether it records.
    bool skirmish_decided_ = false;
    /// select_map_named took the map through the campaign loader, because
    /// select_map does not accept it. Playback then keeps the definitions the
    /// recording's unit checks name. A map select_map accepts leaves this false.
    /// It records which loader took the map and starts nothing.
    bool mission_map_taken_ = false;
    bool demo_unit_table_differs_ = false;
    // How many of the playback's keyframe faults the log has told of.
    std::size_t keyframe_faults_logged_ = 0;
    // Declared after net_: the playback ends before the session.
    std::unique_ptr<DemoState, void (*)(DemoState*) noexcept> demo_{nullptr, destroy_demo_session};
    // The last chat line handed to the match report.
    std::string reported_chat_;
    // The last text translate_text gave.
    std::string translated_text_;
    // The profile whose rules are bound (bind_profile_rules), null for
    // 3.1c's, its sim hash, and the battle room buttons bound.
    const oa::data::mod_profile::ModProfile* bound_profile_ = nullptr;
    oa::base::sha256::Digest bound_sim_hash_{};
    uint8_t bound_lobby_buttons_ = 0;
    // The presence record bound to the battle room (follow_presence), the
    // facts it was written from, whether they have been read once, and the
    // problems with them already reported (one bit each).
    std::vector<uint8_t> bound_presence_record_;
    PresenceFacts presence_facts_{};
    bool presence_taken_ = false;
    uint32_t presence_reported_ = 0;
    // The base game, expansions and language the bound record was written
    // from. They are not part of PresenceFacts. Read through Runtime's
    // public methods only.
    std::string presence_content_key_;
    // The mutator field the bound record was written from. The generation
    // skips the copy until that field changes. Both start at 0, so seen
    // tells a first read from a field that has never been sent.
    uint32_t presence_mutator_generation_ = 0;
    bool presence_mutators_seen_ = false;
    bool presence_mutators_held_ = false;
    oa::netgame::PresenceMutators presence_mutators_{};
};

} // namespace oa::app
