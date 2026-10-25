// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Registration of the multiplayer screens with the application.
//
// Screen ids 0x100..0x104. The main menu reaches the flow by requesting
// kScreenProviders. Dialogs (TCP address, reject confirmation, map view and
// selection, unit restrictions, timeout, message boxes) stack inside these
// screens rather than taking ids of their own.
#pragma once

#include "oa/netgame/presence.hpp"
#include "oa/ui/frontend_multiplayer/connect.hpp"
#include "oa/ui/frontend_multiplayer/dialogs.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/restrict.hpp"
#include "oa/ui/screen_registry.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::frontend_multiplayer {

inline constexpr app::ScreenId kScreenProviders = 0x100;  // SELPROV.GUI
inline constexpr app::ScreenId kScreenTcp = 0x101;        // TCP.GUI over SELPROV
inline constexpr app::ScreenId kScreenGameList = 0x102;   // SELGAME.GUI
inline constexpr app::ScreenId kScreenNewGame = 0x103;    // NEWMULTI.GUI
inline constexpr app::ScreenId kScreenBattleroom = 0x104; // LOUNGE2.GUI
inline constexpr app::ScreenId kScreenMainMenu = 0;       // app::Screen::main_menu
inline constexpr app::ScreenId kScreenOptions = 6;        // app::Screen::options

enum class ModalKind : uint8_t {
    none,
    tcp,
    confirm,
    viewmap,
    selmap,
    restrict,
    timeout,
    exit_confirm
};

/// Replaces the loopback network with a real session binding; call before the first multiplayer screen is
/// entered.
///
/// The binding survives multiplayer_reset.
///
/// @param net Network table the lobby uses from now on.
void multiplayer_bind_net(const LobbyNet& net) noexcept;

/// A clock the multiplayer screens' timers read in place of the steady clock.
struct LobbyClock {
    void* context = nullptr;
    /// Returns the clock in milliseconds; null means the steady clock.
    uint32_t (*now_ms)(void* context) = nullptr;
};

/// Binds the clock the multiplayer screens' timers read.
///
/// The binding survives multiplayer_reset.
///
/// @param clock The clock; a null now_ms restores the steady clock.
void multiplayer_bind_clock(const LobbyClock& clock) noexcept;

/// The screen sizes the battle room's RES column offers, and the size it
/// starts at. The sizes are the display's, the same list the settings' Screen
/// size offers after Desktop. There is no second list.
struct LobbyDisplayModes {
    void* context{};
    /// Writes the sizes the display offers, the narrower first and of two
    /// as wide the shorter, at most `capacity` of them, and returns how many
    /// it wrote. Null, or none written, offers no size.
    int32_t (*modes)(void* context, DisplayMode* out, int32_t capacity){};
    /// Returns the size the game plays at, one of those `modes` writes, which
    /// the local player's column starts at; null, or a size of 0 by 0,
    /// starts it at 640x480.
    DisplayMode (*screen_size)(void* context){};
    /// Stores and applies a size the RES column stepped to, as choosing it
    /// in the settings does. Null only announces the size to the other
    /// players.
    void (*choose)(void* context, int32_t width, int32_t height){};
};

/// Stores and applies a screen size the battle room's RES column stepped to,
/// when multiplayer_bind_display_modes supplied a choose function.
///
/// @param width the size's width, in the window system's units
/// @param height the size's height
void multiplayer_choose_screen_size(int32_t width, int32_t height) noexcept;

/// Binds the screen sizes the battle room's RES column offers and starts at.
///
/// The binding survives multiplayer_reset.
///
/// @param display_modes The sizes; a null member keeps its default.
void multiplayer_bind_display_modes(const LobbyDisplayModes& display_modes) noexcept;

/// Sets the player timeout of the battle room's game (the -t switch), which its stall scan reads.
///
/// The value survives multiplayer_reset.
///
/// @param seconds Player timeout in seconds; 0 or less gives kDefaultPlayerTimeoutSeconds, which
///        also applies until one is set.
void multiplayer_bind_player_timeout(int32_t seconds) noexcept;

/// Sets the battle room buttons a mod's display rules add (lobby_button
/// bits), which the battle room offers its host when its GUI has them.
///
/// The value survives multiplayer_reset.
///
/// @param buttons the buttons; 0, as in 3.1c, for none
void multiplayer_bind_lobby_buttons(uint8_t buttons) noexcept;

/// Binds the network rules the battle room plays by: the version bytes it presents and how it compares
/// them, the private chat channel and the recorder.
///
/// The binding survives multiplayer_reset; until one is bound the rules are 3.1c's.
///
/// @param rules The rules.
/// @param program The line this machine's recorder answers .report with, such as "Program 1.0"; null
///        for none.
void multiplayer_bind_wire_rules(const netgame::WireRules& rules, const char* program) noexcept;

/// Binds whether this machine sends and reads chat as UTF-8 (Lobby::unicode_chat).
///
/// The binding survives multiplayer_reset; until one is bound it is off.
///
/// @param on Unicode chat is on.
void multiplayer_bind_unicode_chat(bool on) noexcept;

/// Binds how the battle room's chat list measures a line (Lobby::chat_measure),
/// so a line about a 3.1c player is fitted to the list as the room's mutator
/// lines are.
///
/// The binding survives multiplayer_reset; until one is bound such a line is
/// cut by bytes alone.
///
/// @param measure The list's room and a line's width.
void multiplayer_bind_chat_measure(const ChatMeasure& measure) noexcept;

/// Binds the presence record this machine sends to the OA players in the battle room
/// (PresenceRecords::source), without a map pack.
///
/// The screens keep a copy, which survives multiplayer_reset. The battle
/// room sends it as it changes (presence_refresh, presence_send_due), the
/// host's with the map pack of its selected map.
///
/// @param record the record, type byte first; null binds none
/// @param size its length; 0, or more than netgame::presence_record_max_bytes, binds none
void multiplayer_bind_presence_record(const uint8_t* record, std::size_t size) noexcept;

/// Binds the full mutator list this machine draws a part from.
///
/// The presence record may be cut so a map pack still fits. The battle room
/// sends the mutators that did not fit from this list. Null binds none. The
/// binding is kept across a new game, as the source record is.
///
/// @param loadout the list; null binds none
void multiplayer_bind_mutator_loadout(const netgame::PresenceMutators* loadout) noexcept;

/// Binds the map packs' answer to which pack the host's selected map comes from (LobbyMaps::pack).
///
/// The binding survives multiplayer_reset. The battle room asks it once
/// after each binding and once each time the selected map changes. Until
/// one is bound, no record names a map pack.
///
/// @param context passed back to pack
/// @param pack the answer; null for none
void multiplayer_bind_map_pack(
    void* context, bool (*pack)(void* context, netgame::PresenceMapPack* out)
) noexcept;

/// Binds the launch the connection screens, the battle room and the session read.
///
/// The binding survives multiplayer_reset.
///
/// @param link The launch block and the launch's answers; a null block reads as empty.
void multiplayer_bind_launch_link(const LaunchLink& link) noexcept;

/// One installed pack map, as the battle room's map list shows it.
///
/// The pointers stay valid until the next call of the source that wrote them.
struct LobbyPackMap {
    const char* name{};        ///< `<stem>@<id>`, the name a choice and the battle room use
    const char* description{}; ///< one line about the map; empty when unwritten
    const char* size{};        ///< the map's width and height, as the list shows it
    int32_t memory_mb{};       ///< memory the map needs, in MB
    /// The name the pack's index gives the map. Null when this row is a base
    /// map, or the index has no entry.
    const char* title{};
    /// The pack's name. Null when this row is a base map, or the pack has none.
    const char* pack_name{};
    /// The map file's title, when the source already holds it. The list does
    /// not open the file to find one. Null when it is not held.
    const char* file_title{};
    /// The short English reason the row is greyed with. Null when the map can be played.
    const char* blocked_reason{};
};

/// Where the battle room reads installed pack maps and the base maps' summaries,
/// and mounts the pack map it plays.
///
/// A null member does nothing. The binding survives multiplayer_reset.
struct LobbyMapSource {
    void* context{};
    /// Returns how many installed pack maps the list shows, greyed ones included.
    int32_t (*count)(void* context){};
    /// Writes the pack map at `index`.
    ///
    /// @param context the source's context
    /// @param index the map's place, from 0
    /// @param[out] out the map; left unchanged when `index` is out of range
    /// @return true when `out` was written
    bool (*at)(void* context, int32_t index, LobbyPackMap* out){};
    /// Mounts the named pack map's files, and no other pack map's.
    ///
    /// @param context the source's context
    /// @param name the map's name, `<stem>@<id>`
    /// @param[out] reason receives why the map was refused, when not null
    /// @param capacity bytes of `reason`, counting the terminating NUL
    /// @return true when the map's files are mounted
    bool (*prepare)(void* context, const char* name, char* reason, std::size_t capacity){};
    /// Unmounts the pack map whose files are mounted.
    void (*release)(void* context){};
    /// Returns how many base maps the one start scan listed.
    int32_t (*base_count)(void* context){};
    /// Writes the base map at `index`.
    ///
    /// The pointers stay valid until that scan lists the maps again.
    ///
    /// @param context the source's context
    /// @param index the map's place, from 0
    /// @param[out] out the map; left unchanged when `index` is out of range
    /// @return true when `out` was written
    bool (*base_at)(void* context, int32_t index, LobbyPackMap* out){};
    /// Writes the map pack of the pack map whose files `prepare` mounted, as
    /// this machine's presence record names it. Bound for the battle room
    /// with multiplayer_bind_map_pack, with `context` as its context: on the
    /// host it is the selected map's pack.
    ///
    /// @param context the source's context
    /// @param[out] out the map pack; left unchanged when there is none
    /// @return false when no pack map's files are mounted, or its pack was
    ///     not installed from a registry's catalogue
    bool (*pack)(void* context, netgame::PresenceMapPack* out){};
};

/// Binds the pack maps the battle room lists beside the base maps, and the
/// base maps' summaries when the source carries them.
///
/// The binding survives multiplayer_reset. A null member does nothing. The
/// list is read the next time it is asked for, and again when a pack is
/// installed or removed. With no base maps bound, the list scans the
/// installed maps as before.
///
/// @param source the pack maps; a null member keeps that part unused
void multiplayer_bind_map_source(const LobbyMapSource& source) noexcept;

/// A TCP/IP game the multiplayer screens host or join at once, as a player
/// would through them: the address typed into TCP.GUI and accepted, the
/// names and password typed into the game list and NEWMULTI, and OK or the
/// listed game's join.
struct DirectGame {
    enum class Kind : uint8_t {
        none,
        host, // create a game on NEWMULTI
        join, // join a game the game list shows
    };
    Kind kind = Kind::none;
    std::string address;     // join: typed into TCP.GUI's ADDRESS; host: the box keeps its own
    std::string player_name; // typed into the name boxes; empty keeps the name they show
    // host: typed into NEWMULTI's GAMENAME, empty keeping the name it shows;
    // join: the listed game joined, empty for the first listed.
    std::string game_name;
    // host: typed into NEWMULTI's PASSWORD; join: into the game list's;
    // empty keeps the password the box shows.
    std::string password;
};

/// How far the direct game has gone (multiplayer_direct_game_progress).
enum class DirectGameStep : uint8_t {
    none,        // no direct game was asked for
    connecting,  // on its way through the connection screens
    battle_room, // its battle room opened
    failed,      // a screen refused it, with the notice DirectGameProgress::notice
};

struct DirectGameProgress {
    DirectGameStep reached = DirectGameStep::none; // how far it has gone
    std::string notice;                            // failed: the notice the screen showed, as shown
    std::string game_name;   // battle_room: the game's name, as the game list shows it
    std::string player_name; // battle_room: the local player's name, as the battle room shows it
};

/// Asks the multiplayer screens to host or join a game as they are next entered.
///
/// The provider list takes TCP/IP when the launch block's connection type
/// names it ("-n1" writes it). Each screen then goes on at once: TCP.GUI
/// accepts the address without storing it, the game list types the player's
/// name and, to join, the password, then goes on to NEWMULTI or waits for
/// the game to be listed as a launched join does, and NEWMULTI types the
/// game's name and password and takes OK. Each screen does its part once:
/// a refusal leaves the player on that screen with its notice, to go on by
/// hand. The binding survives multiplayer_reset.
///
/// @param game The game; kind none asks for nothing.
void multiplayer_bind_direct_game(const DirectGame& game);

/// Returns how far the game multiplayer_bind_direct_game asked for has gone.
///
/// @return The step and, once it failed, the notice that ended it.
[[nodiscard]] const DirectGameProgress& multiplayer_direct_game_progress() noexcept;

/// Binds the mod profile's rules the battle room keeps (Lobby::rules).
///
/// The binding survives multiplayer_reset.
///
/// @param rules The rules, or null for 3.1c's.
void multiplayer_bind_rules(const data::match_rules::MatchRules* rules) noexcept;

// The game's translation of interface text, which the screens' texts go
// through (LobbyServices::translate).
struct TextTranslation {
    void* context{};
    /// Translates `text` into the game's language; null translates nothing.
    std::string (*translate)(void* context, std::string_view text){};
};

/// Binds the translation of the screens' interface texts, such as the map
/// summary's "Players".
///
/// The binding survives multiplayer_reset. Until one is bound, and when it
/// fails, the texts show as they are.
///
/// @param translation The game's translation.
void multiplayer_bind_translation(const TextTranslation& translation) noexcept;

/// Binds the number of sides the game's SIDEDATA loaded, which the battle
/// room's SIDE button cycles through before a watcher: a mod may have more
/// than ARM and CORE. The binding survives multiplayer_reset. Until one is
/// bound, or with 0, the battle room assumes the two sides of 3.1c.
///
/// @param side_count the loaded sides, 0 for unknown
void multiplayer_bind_side_count(int32_t side_count) noexcept;

/// Binds the SIDEDATA names of the loaded sides, one per side number.
///
/// The binding survives multiplayer_reset, as the side count does. A side
/// the battle room's art has no frame for shows its name on the plain face.
///
/// @param names the names, side 0 first
void multiplayer_bind_side_names(std::span<const std::string> names) noexcept;

// The line this machine says of its engine in the battle room's chat, which
// the engine gives (multiplayer_bind_engine_banner).
struct EngineBanner {
    void* context{};
    /// Returns the line, such as "[Engine: OpenAnnihilation v0.6.2]"; null,
    /// or an empty line, says nothing.
    std::string (*line)(void* context){};
};

/// Binds the line this machine says of its engine in the battle room: once
/// as the battle room is entered, hosting or joining, and again each time the
/// line changes while it shows. The line goes out as the local player's chat
/// line through the battle room's own chat, so every player sees it,
/// whatever game client they play.
///
/// The binding survives multiplayer_reset; until one is bound nothing is said.
///
/// @param banner The line's source; a null line says nothing.
void multiplayer_bind_engine_banner(const EngineBanner& banner) noexcept;

/// Returns the line an engine says of itself in the battle room's chat.
///
/// @param version The engine's version as the engine shows it, such as "v0.6.2".
/// @param developer_mode Developer Mode is on.
/// @param remote_controlled A program on the machine may control the game
///        (oa::app::Options::remote_controlled).
/// @return "[Engine: OpenAnnihilation <version>]", with " DEV MODE" before
///         the closing bracket while Developer Mode is on, and then
///         " REMOTED" while the game may be remote-controlled
[[nodiscard]] std::string
engine_banner_line(std::string_view version, bool developer_mode, bool remote_controlled);

// Receives the lobby when the battle room starts a game (the host's START,
// or the host's 0x08 on a client) and takes over from the frontend. Without
// one START only reports that the match loader is not connected.
using StartHandler = void (*)(void* context, Lobby& lobby);

/// Tells whether one of the multiplayer screens is on screen.
///
/// @return true from a screen's entry until it is left for a screen that is not one of them, or
///         until the battle room hands its lobby to the match loader
[[nodiscard]] bool multiplayer_showing() noexcept;

/// Asks for the exit confirmation (YESORNO.GUI, exit_confirm_open) over the multiplayer screen that
/// shows, as a request to close the window does after a launch.
///
/// The screen's next tick opens it over whatever is in front, which is back
/// in front once No closes it; Yes leaves the game through
/// LaunchLink::leave_game without the disconnect reason.
///
/// @return false when no multiplayer screen shows
bool multiplayer_request_exit_confirm() noexcept;

/// Installs the handler that takes over when the battle room starts a game.
///
/// @param handler Start handler, or null to leave START unconnected.
/// @param context Passed back to the handler.
void multiplayer_bind_start(StartHandler handler, void* context) noexcept;

// Inspection hooks for tests and the integrator.

/// Returns the frontend's lobby state.
[[nodiscard]] Lobby& multiplayer_lobby() noexcept;

/// Returns the loopback network used until multiplayer_bind_net.
[[nodiscard]] LoopbackNet& multiplayer_loopback() noexcept;

/// Returns the connection screens' state.
[[nodiscard]] ConnectState& multiplayer_connect() noexcept;

/// Returns the interactive panel of the current screen: the stacked dialog when one is open.
[[nodiscard]] Panel& multiplayer_panel() noexcept;

/// Where the interactive panel's controls lie on the 640x480 canvas.
struct PanelOffset {
    int16_t x{}; ///< columns added to a control's x
    int16_t y{}; ///< rows added to a control's y
};

/// Returns where the interactive panel (multiplayer_panel) lies on the canvas.
///
/// @return the offset its controls' positions take on the canvas
[[nodiscard]] PanelOffset multiplayer_panel_offset() noexcept;

/// Where the battle room draws one OA badge.
struct PresenceBadgeSpot {
    int32_t slot{}; ///< the row's player slot, 0..9
    /// The badge's rectangle on the 640x480 frame: the row's CD<slot>
    /// rectangle and the screen's offset, in pixels.
    int32_t x{}, y{}, width{}, height{};
    bool rules_differ{}; ///< the badge carries the dot (lobby_rules_differ_from_host)
};

/// The rules dot's ring, in canvas pixels: a square this many pixels across,
/// its four corner pixels left out.
inline constexpr int32_t presence_badge_dot_ring = 6;
/// How far the rules dot reaches past the badge's right edge and above its
/// top, in canvas pixels. The dot's face is the square one pixel inside the ring.
inline constexpr int32_t presence_badge_dot_overhang = 2;

/// Lists the OA badges the battle room draws now, in slot order.
///
/// A row shows the badge when lobby_row_badge says open_annihilation and the
/// battle room's GUI has its CD<slot> control. The rectangles are in the
/// 640x480 frame the battle room is drawn in, before the menu frame scales
/// it.
///
/// @param[out] out receives the badges; null receives none
/// @param capacity how many badges out holds
/// @return how many badges were written: 0 when the battle room is not shown
///         or a dialog covers it, or its GUI has no CD<slot> control
int32_t multiplayer_presence_badges(PresenceBadgeSpot* out, int32_t capacity) noexcept;

/// Returns the slot of the battle room's row whose name has the keyboard's
/// focus: the front panel's focused control when it is a remote player's
/// PLAYER<slot> name (the local player's own row is a label and takes no
/// focus). Tab and the arrow keys, which are also the pad's D-pad, move
/// the focus onto those rows after the lists and before the chat line.
/// Up and Down stay inside a list, and Left and Right stay in the chat line.
/// Return and the pad's A open that row's card when it shows an OA badge,
/// and otherwise do nothing; they never press the row.
///
/// @return the slot, 0..9; -1 while the battle room is not shown, a dialog
///     covers it, or the focus is on no player's row
[[nodiscard]] int32_t multiplayer_focused_player_slot() noexcept;

/// One control of the battle room's panel, in frame pixels.
struct BattleRoomControl {
    int32_t index{-1};       ///< the control's index
    std::string name{};      ///< its name
    int32_t x{};             ///< its left edge, plus the panel's offset
    int32_t y{};             ///< its top edge, plus the panel's offset
    int32_t width{};         ///< its width
    int32_t height{};        ///< its height
    int32_t focus_place{-1}; ///< its place in the battle room's focus order, or -1
};

/// The battle room's controls that have an area, including ones that are hidden.
///
/// Empty while the battle room is not the screen in front.
///
/// @return the controls, in index order
[[nodiscard]] std::vector<BattleRoomControl> multiplayer_battle_room_controls();

/// The control the battle room's own focus is on.
///
/// @return the control index, or kNoControl while the battle room is not the
///     screen in front
[[nodiscard]] int32_t multiplayer_battle_room_focus() noexcept;

/// Moves the battle room's own focus.
///
/// Does nothing while the battle room is not the screen in front.
///
/// @param index the control, or kNoControl for none
void multiplayer_set_battle_room_focus(int32_t index) noexcept;

/// Tells whether the battle room is the multiplayer screen.
///
/// True while a dialog covers it. False once the room is left.
///
/// @return true while the battle room is showing
[[nodiscard]] bool multiplayer_battle_room_open() noexcept;

class PresenceCardTrigger;

/// Hands the battle room the card trigger Return and the pad's A use.
///
/// While a remote player's row has the focus, those keys open its card when
/// the row shows an OA badge, and do nothing otherwise. They never press
/// the row. A null trigger still takes the key and opens no card. The
/// binding survives multiplayer_reset. The presence card screen binds its
/// own trigger and clears the binding when that screen goes.
///
/// @param trigger the card's trigger, or null
void multiplayer_bind_row_key(PresenceCardTrigger* trigger) noexcept;

/// Tells the battle room that the OA layer draws each OA badge at the
/// window's own pixels, so the 640x480 frame leaves its pixel badge out.
///
/// `draws` is asked while the frame is drawn and returns true when the
/// layer will draw the badges this frame. A null binding, the one the
/// screens start with, keeps the frame's badge: a renderer without the
/// layer, and the screens' own tests. The binding survives multiplayer_reset.
/// The layer binds its own when its overlay is registered, and clears it
/// when that layer goes.
///
/// @param context handed to draws; null with a null draws
/// @param draws returns true when the layer draws the badges; null clears the binding
void multiplayer_bind_layer_presence_badges(void* context, bool (*draws)(void*)) noexcept;

/// Returns the gadget records of the current base screen as its loader left them.
[[nodiscard]] const ui::gui_layout::Layout& multiplayer_screen_layout() noexcept;

/// Returns the stacked dialog's panel, or null when none is open.
[[nodiscard]] Panel* multiplayer_modal() noexcept;

/// Returns which dialog is stacked, or none.
[[nodiscard]] ModalKind multiplayer_modal_kind() noexcept;

/// Returns the unit restriction dialog's state.
[[nodiscard]] RestrictPanel& multiplayer_restrict() noexcept;

/// Returns the open message box text; empty when none is open.
[[nodiscard]] const std::string& multiplayer_message() noexcept;

/// Returns the battle room's unit table, read from the assets on first use.
///
/// @param assets Asset store the unit headers are read from.
/// @param[out] count Table length, the reserved empty type included.
/// @return The first entry; a failed read leaves the table as it was.
[[nodiscard]] LobbyUnit* multiplayer_units(const oa::AssetStore& assets, int32_t* count) noexcept;

/// Computes the battle room's LobbyServices::unit_checksum over the installed files of the unit table.
///
/// @param context Service context.
/// @param unit Unit whose content checksum is wanted.
/// @return The checksum; the unit's stored content checksum when the files cannot be read.
[[nodiscard]] uint32_t multiplayer_unit_checksum(void* context, const LobbyUnit& unit) noexcept;

/// Clicks a named control of the front panel as the pointer would: press, gadget-engine update, handler.
///
/// An open message box is dismissed instead.
///
/// @param ctx Screen context of the running frontend.
/// @param name Control name or link.
/// @param button Mouse button, 1 left or 2 right.
/// @return False when the control is missing.
bool multiplayer_click(app::ScreenContext* ctx, const char* name, uint8_t button = 1) noexcept;

/// Types text into the focused text box, up to its length limit.
///
/// The battle room's chat line (MESSAGE) takes every character but the
/// control characters, whole, as game text: UTF-8 where game text holds it,
/// else the game's code page with '?' for a character it lacks. The other
/// boxes take printable ASCII.
///
/// @param ctx Screen context of the running frontend.
/// @param text NUL-terminated UTF-8; null types nothing.
void multiplayer_type(app::ScreenContext* ctx, const char* text) noexcept;

/// Shows the input method's composition at the end of the battle room's
/// focused chat line, in place of the one shown before; typed text replaces
/// it.
///
/// @param composition NUL-terminated UTF-8; empty or null takes the shown
///     one away.
/// @return false when none of these screens shows, or the focused box is
///     not the chat line
bool multiplayer_compose(const char* composition) noexcept;

/// Returns where the host's open map dialog in the battle room lists the
/// maps (its MAPNAMES list), in the 640x480 frame the battle room is drawn
/// in, before the menu frame scales it.
///
/// @param[out] x the list's left column; not written when false is returned
/// @param[out] y the list's top row
/// @param[out] width the list's width, in pixels
/// @param[out] height the list's height, in pixels
/// @return false when the battle room's map dialog is not open, or this
///     machine is not the host
bool multiplayer_map_list_rect(int32_t* x, int32_t* y, int32_t* width, int32_t* height) noexcept;

/// Tells whether a point on the canvas lies on a shown list's scroll arrow,
/// and that list shows a page of rows the arrow can move.
///
/// A list whose rows all fit, a bar that is not shown, and a list with no
/// row pitch are not such arrows: a click there is the game's own.
///
/// @param x the point's column on the canvas
/// @param y the point's row on the canvas
/// @return true when the point is on such an arrow
[[nodiscard]] bool multiplayer_list_page_arrow(int32_t x, int32_t y) noexcept;

/// Moves the list whose scroll arrow contains the point by one page of the
/// rows it shows, toward that arrow, and the scroll bar's knob follows.
///
/// One step of the knob does not bring a new row onto the page, so a program
/// that clicks the arrow to reach a row would run out of clicks. A page is
/// the rows the list shows. A player's own click on the arrow does not come
/// here, and still steps the knob once.
///
/// @param x the point's column on the canvas
/// @param y the point's row on the canvas
/// @return true when the list's first row changed
bool multiplayer_scroll_list_page(int32_t x, int32_t y) noexcept;

/// Shows a map in the host's open map dialog, as a click on its row
/// highlights it: when the name is not in the screens' map list, which is
/// built once a visit, the list is built again, so that a pack installed
/// since is in it; then the dialog's list is filled again from the lobby's
/// maps, the map's row is selected and the map previewed. Nothing is sent
/// to the other players: the host chooses the map with the dialog's LOAD.
///
/// @param name the map's name, `<stem>@<id>` for a pack map; null shows nothing
/// @return false when the dialog is not open, this machine is not the
///     host, or no map has the name
bool multiplayer_offer_map(const char* name) noexcept;

/// Builds the battle room's map list again when it has already been read.
///
/// A list that has not been read yet is left for the next read, which sees
/// the packs installed then. The selected map keeps its place by name. An
/// open map dialog's rows are filled again, and the highlighted row stays
/// when that map is still listed. The dialog does not preview the map.
void multiplayer_reload_maps();

/// Returns how many maps the battle room's list holds, reading it as the
/// battle room does.
///
/// A fully bound map source is read even when no multiplayer screen has a
/// context yet. Any other list waits for that context and then counts nothing
/// until it is set.
///
/// @return the number of maps
[[nodiscard]] int32_t multiplayer_map_count();

/// Returns the game name of one map in the battle room's list.
///
/// The pointer stays valid until the list is read again.
///
/// @param index the map's place, from 0
/// @return the name; null when `index` is out of range
[[nodiscard]] const char* multiplayer_map_at(int32_t index);

/// The words a map list shows for each name, in the same order.
///
/// A pack map shows the title its index gives, or the map file's title when
/// the source already holds one, or its own name when neither is known. A
/// shared title adds the pack's name in brackets. The `name\@package` key is
/// not shown. A base map's row is its file name. A name the list does not
/// hold is shown as its own name when it is a pack map, and as itself
/// otherwise.
///
/// @param names the maps' names, in list order
/// @return one string per name
[[nodiscard]] std::vector<std::string> multiplayer_map_texts(std::span<const std::string> names);

/// The words a screen shows for one map.
///
/// A pack map uses the same words as its row in the map list, so the name
/// has no `@`. A base map's name is returned as given. The key the game
/// stores is unchanged.
///
/// @param key the map's name, as selection and the battle room carry it
/// @return the words to show; empty when `key` is null
[[nodiscard]] std::string multiplayer_shown_map_name(const char* key);

/// Fills MAPNAMES with the words `multiplayer_map_texts` gives `names`, and
/// keeps `names` as the rows' keys.
///
/// @param[in,out] panel the dialog
/// @param names the maps' names, in list order
void multiplayer_fill_map_names(Panel& panel, const std::vector<std::string>& names);

/// Orders map names by the name each row shows.
///
/// The grey reason is not part of that name. Equal names stay as they were.
/// A list past the game's sorting limits is left as it was given.
///
/// @param[in,out] names the maps' names
void multiplayer_order_map_names(std::vector<std::string>& names);

/// Returns the pack index's title for a pack map the battle room's source lists.
///
/// The pointer stays valid until the next call of that source.
///
/// @param name the map's game name; null returns null
/// @return the title; null when the source does not list the map, or its
///     index has no title
[[nodiscard]] const char* multiplayer_pack_map_title(const char* name);

/// Fills the map dialog's MAPNAMES list again from the lobby's maps, in name
/// order, selects the row of a map and previews it (mapselect_preview), as a
/// click on the row highlights it. Nothing is sent. A name the list lacks
/// leaves the row highlighted before, and previews nothing.
///
/// @param[in,out] lobby Lobby state whose map services list and select the maps.
/// @param[in,out] select Map selection state; its previous map is kept.
/// @param[in,out] panel The dialog's panel.
/// @param name the map's name
/// @return true when the map's row is selected
bool mapselect_offer(Lobby& lobby, MapSelect& select, Panel& panel, std::string_view name) noexcept;

/// Starts a new frontend session: drops lobby, connection and dialog state and resets the loopback network.
///
/// A network bound with multiplayer_bind_net is kept.
void multiplayer_reset() noexcept;

/// Returns the frontend game block the lobby reads, its unit header table included.
[[nodiscard]] Game& multiplayer_game() noexcept;

/// Runs the frontend mode tick's unit header step.
///
/// The lobby's units are read again while none are loaded or a full unit
/// load marked them stale.
///
/// @param ctx Screen context of the running frontend.
void multiplayer_reload_unit_headers(app::ScreenContext* ctx);

/// Drops the battle room's unit table, reads it again and restarts unit sync.
///
/// The table is read from the install the room already uses. With no install
/// bound, the table is cleared and sync still restarts.
void multiplayer_units_changed();

/// Drops one peer and greets it again at once.
///
/// @param player_id the peer
void multiplayer_regreet(uint32_t player_id);

/// Copies the host's mutator list as this guest holds it.
///
/// False on the host, and false when the host's records have named no list.
/// A list that is still arriving is copied as far as it has come.
///
/// @param[out] out the list; null asks only whether there is one
/// @return true when this machine is a guest and the host has named a list
[[nodiscard]] bool multiplayer_room_mutators(netgame::PresenceLoadoutAssembly* out);

/// How many times the guest's copy of the host's list has changed.
///
/// @return the generation, starting at 0
[[nodiscard]] uint32_t multiplayer_room_mutators_generation() noexcept;

} // namespace oa::ui::frontend_multiplayer

namespace oa::app {
/// Registers the five multiplayer screens (ids 0x100..0x104) with the application.
///
/// @param[in,out] registry Screen registry to add to.
void register_multiplayer_screens(ScreenRegistry* registry);
} // namespace oa::app
