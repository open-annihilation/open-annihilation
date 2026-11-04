# Engine follow-ups

Some of the Boneyards' behaviour in 3.1c, its overlay on the main menu and a
game it launched, needed something the engine or its network play did not
offer. The module reaches the rest of the engine only through its extension table
(`src/app/include/oa/app/extension.hpp`), the check host
(`src/app/include/oa/app/check_host.hpp`), the screens'
`ScreenServices`
(`src/ui/screen-registry/include/oa/ui/screen_registry.hpp`) and
the engine's other declared headers, and network play only through its API
for the extensions built on it
(`src/app/netgame/include/oa/app/netgame/extension_api.hpp`) and the
engine's other public headers. Each section below gives the behaviour 3.1c
has, what the engine or its network play offers for it, and how the
module uses it. Every follow-up in the table below is done. The sources
are now in this repository ([decisions](decisions.md)). This document
records the additions made for them.

## Versions

The module checks the version of the engine's extension table
(`OA_EXTENSION_API_VERSION`, now 19) and
of network play's API (`OA_NET_EXTENSION_API_VERSION`, now 3) with
`static_assert` in
`src/boneyards/extension/boneyards_extension.cpp` (`kExtensionApiVersionFollowed`,
`kNetworkPlayApiVersionFollowed`), so a build against another engine stops
until the hooks follow its change. The root `CMakeLists.txt` includes
`cmake/OaBoneyards.cmake`, which registers it with `oa_add_extension`,
taking the "-y" switch and the online-module "-c". Its library links `oa-app-netgame-sdk`,
so the engine lists it after network play's extension, and where the engine
asks the extension built on the others first, this one answers before
network play.

The Boneyards fills the engine's hooks for its screens, options, checks and
answers, and network play's hooks
(`oa::app::netgame::extension_api::Hooks`) for the match report (started,
told of events, chat lines and frames, closed and released;
`src/boneyards/extension/boneyards_report.cpp` keeps it with
`src/boneyards/reporter`), the console's Page and a failed launched join. It
reaches network play's launch block, applies and ends a launch, posts lines
to a battle and leaves the game through network play's API. Its "-c"
switch, when it names the online module, clears the launch block, as
3.1c's online module loader does first. A launch file's "-c" is left for
network play.

| Behaviour | Engine or network play addition | Status |
|---|---|---|
| [Entering from the "-y" switch](#entering-from-the--y-switch) | network play's `request_setup`, dispatcher step 0x10b | done |
| [Closing the window](#closing-the-window) | `Extension::close_requested` | done |
| [Ending the program](#ending-the-program-from-a-screen) | `ScreenServices::quit`, network play's `leave_game` and `join_failed` | done |
| [Background music and sound stops](#background-music-and-sound-stops) | `ScreenServices::stop_sounds`, `play_sound_alternate` | done |
| [The launcher's label](#the-launchers-label-in-the-match-menus) | `Extension::return_label` | done |
| [Frontend queries](#frontend-queries) | `query_register` | done |
| [The return after a launch](#the-map-list-and-the-return-after-a-launch) | `ScreenServices::run_frontend`, dispatcher step 0x109 | done |
| [The stored password after a launch](#the-stored-password-after-a-launch) | `Extension::keep_stored_password` | done |
| [Application modes set again](#application-modes-set-again) | `Extension::app_mode_set` | done |

## Entering from the "-y" switch

**3.1c.** The "-y" switch has the main menu's first update open the
Boneyards window, as ENTER BONEYARDS does but without its check that the
network the Boneyards needs is there.

**Engine and network play.** The engine hands "-y" to the extension that
takes it (`SWITCHES cy`); network play's `request_setup` marks its switches'
setup request, and its main menu's first update asks for dispatcher step
0x10b (`setup_requested`). The engine runs the frontend's dispatcher on
input or when asked (`ScreenServices::run_frontend`).

**Boneyards.** The Boneyards' switch handler takes "-y"
(`boneyards_options.cpp`), skips the intro and calls `request_setup`; the
main-menu strip registers step 0x10b (`register_boneyards_console_screens`),
which enters the Boneyards (`boneyards_status_widget_enter`), and after
"-y" the strip's first tick asks for the frontend pass that holds the main
menu's first update. `native-boneyards-entry-switch` starts the game with
"-y" and checks that it enters without a key pressed.

## Closing the window

**3.1c.** Closing the window runs the handler the game has installed. Each
application mode the game sets installs one: while the Boneyards window is
up and enabled outside a battle it launched, the Boneyards box; otherwise
in a match (mode 6) the exit confirmation, and in any other mode the leave.
Three moments install their own: the Boneyards' entry (ENTER BONEYARDS, the
"-y" switch, and the main menu's return to the Boneyards after a launched
battle) the Boneyards box, the Boneyards' launch reply the exit
confirmation, and a leave none. Nothing else changes it, so the main menu
right after the Boneyards' EXIT still asks "Exit to Windows?", and after a
launch reply the connection screens and the battle room ask for
confirmation until the game next sets an application mode. The Boneyards
box asks "Exit to Windows?". YES has the idle work leave the game and end
the program; NO closes the box.

**Engine.** `Extension::close_requested(void* context, Runtime& runtime)`
is asked for each close request the main loop receives, the extension
built on the others first. True means the extension answered; false leaves
the request to the next.

**Boneyards.** The Boneyards keeps its box (`ExitPrompt`,
`src/boneyards/extension/boneyards_exit_prompt.hpp`) as network play keeps its
close handlers: each mode set, and each change of mode it sees at a frame
or a request, installs the box while the Boneyards is up outside a battle
it launched and removes it otherwise; the Boneyards coming up outside such
a battle installs it; its launch reply (`on_boneyards_launch`) removes it.
It follows a match from its start (`match_game`) to its end
(`match_event`). The Boneyards' `close_requested`
(`exit_prompt_close_requested`) answers first: while the box is installed,
`boneyards_status_widget_ask_exit` opens "Exit to Windows?", and the
Boneyards' idle work acts on the answer: NO is forgotten, YES leaves
through `MainMenuHost::leave_game` (network play's leave, with the reason).
When the box cannot open, the Boneyards' interface having gone down as the
main menu was left for another screen, or when the box is not installed,
the Boneyards declines and network play's installed handler answers.
`boneyards-exit-prompt` plays the game's modes, matches, the Boneyards'
entry and EXIT and a Boneyards launch into both and checks which of the two
answers, and runs a request to a box that cannot open.

## Ending the program from a screen

**3.1c.** YES in the Boneyards "Exit to Windows?" box ends the program and
shows the disconnect reason, if any. A joiner whose host is not found while
the Boneyards still counts the game as launched is told "Host not found -
Host may have left the game before you arrived." in a Boneyards box, and
goes back to the Boneyards.

**Engine and network play.** `ScreenServices::quit` ends the run; network
play's `leave_game(true)` leaves the game with the reason and ends the
program. Network play's game list hands a failed launched join to the
`join_failed` hook of the extension that launched the game.

**Boneyards.** YES leaves through network play's `leave_game` with the
reason (`boneyards_launch_bridge`; `boneyards-launch` checks it through
network play's launch record) and drops the battle tunnel first.
`boneyards_join_failed` drops the tunnel, shows the text in a Boneyards
message box (kept until the console is up again when the box cannot open
yet), then the Boneyards hears the battle it launched was left and every
playing sound stops. A return asked for while the game list is still the
screen finishes on the main menu's next tick, with that box in front.

## Background music and sound stops

**3.1c.** At four points the Boneyards stops every playing sound and, unless
the `BYStopThatIncessantDroning` preference is set, plays the sound named
`BGM` on the alternate route, which always streams: when the Main Frame is
built, on each of the two big-button slides (stop, BIGBUTTON, then `BGM`),
and when the chat console is built again for a launched battle's
debriefing. It also stops every sound on the return to the Boneyards after a
launched battle, and when the "Host not found" Boneyards box opens. Profile
APPLY, as it stores a change of the preference, stops every sound when the
drone goes off and plays `BGM` when it comes on.

**Engine.** `ScreenServices::stop_sounds(void* host)` stops the interface
and unit sounds, the stream and the alternate route (the CD and track music
go on); `ScreenServices::play_sound_alternate(void* host, const char* name)`
plays a registered sound on the alternate route, streamed and repeated in
place of whatever that route plays, as the menu music plays.

**Boneyards.** `MainMenuHost` (`src/boneyards/console/include/oa/boneyards/console/status_widget.hpp`)
has `stop_sounds` and `play_sound_alternate` beside `play_sound`, which
the console binds to the kept services. `console_stop_sounds` and
`console_play_background` sound the Main Frame build, both slides
(`main_frame_present_screen`, `main_frame_release_map`), the chat console's
reopening, the return from a battle (a stop only), the "Host not found"
path (`boneyards_join_failed` in `src/boneyards/extension/boneyards_launch.cpp`, a
stop only) and Profile APPLY (`ProfileConsole::background_sound`). The
drone plays while the chat session's `background_sound` is on, which the
navigation reads from `BYStopThatIncessantDroning` and Profile APPLY
updates.

## The launcher's label in the match menus

**3.1c.** When the Boneyards launched the game and the launcher's label (the
provider label, "Boneyards") is 9 characters or fewer:

- the end-of-game screen's MAIN MENU entry reads the label, and leaves the
  pointer's picture as it is;
- the in-game exit menu hides EXITGAME and puts the label on MAINMENU;
- the exit confirmation reads "Surrender this battle and return to
  Boneyards?".

**Engine.** `Extension::return_label(void* context)` is asked as each
match starts; the runtime fills `IngameContext` and `EndMissionContext`
(`return_label`, empty for none) for that match's menus.

**Boneyards.** The Boneyards' `return_label` (`boneyards_return_label`)
answers the online launch block's provider label (`kBoneyardsProviderLabel`
after a Boneyards launch) while the online launch is active, whenever the
Boneyards overlay's status widget records that the Boneyards launched the
game; otherwise null.

## Frontend queries

**3.1c.** The frontend's dispatcher asks the game questions, such as the
context flags or whether an adapter lookup is needed.

**Engine.** `query_register` binds a handler (`ScreenQueryFn`) to a
`frontend_state::Query`; `Runtime::query` answers through it, and 0 for a
query no handler takes.

**Boneyards.** `register_boneyards_queries`
(`src/boneyards/extension/boneyards_launch.hpp`) binds `context_flags` (a launch is
active), `adapter_lookup_needed` (the network the Boneyards needs is there)
and `generate_default_game_name` (0: no lobby program launches the native
game); without the Boneyards they answer 0. Network play answers the
questions about joining and leaving a session.

## The map list and the return after a launch

**3.1c.** When the Boneyards launch reply arrives, the game selects map list
3 (the multiplayer maps), clears the single-player flag and enters frontend
state 16 (the connection selection), whose initialize runs on the next
frontend frame. The returns from a launched game set frontend state 2 and
app mode 1, and the main menu then returns to the Boneyards.

**Engine.** `ScreenServices::run_frontend(void* host)` runs one pass of the
frontend dispatcher once the current event or frame has been handled.

**Boneyards.** The launch writes state 16 and signal 0 and asks for a pass
(`MainMenuHost::run_frontend`); network play's state 16 applies the launch.
After the returns, the main menu's initialize asks for dispatcher step
0x109 (network play's `return_after_launch`), which the main-menu strip
registers: from another screen it asks for the main menu, whose own return
takes the player to the Boneyards.

## The stored password after a launch

**3.1c.** The preferences write keeps the stored password after a
Boneyards launch.

**Engine.** The preferences write skips the password while
`Extension::keep_stored_password` answers true.

**Boneyards.** The Boneyards answers true while its overlay's status
widget records that it launched the game.

## Application modes set again

**3.1c.** Every time the game sets an application mode it installs that
mode's close handler, also when the mode is the one it already runs, so
after the Boneyards' EXIT a briefing's return to the single-player menu
installs the leave, where the main menu had kept "Exit to Windows?".

**Engine.** `Extension::app_mode_set(void* context, Runtime& runtime,
int32_t mode)` is called each time the engine sets the frontend Game's
application mode, the mode it already holds included.

**Boneyards.** The Boneyards' `app_mode_set` follows it into its box
(`exit_prompt_mode_set`); `boneyards-exit-prompt`
(`test_answers_mode_set_again`) covers it after the Boneyards' EXIT and over
the Boneyards box.

## Reading the game's files

**3.1c.** Pages and the other files the Boneyards reads when the player has
no copy of them come from the game's files.

**Engine.** Version 14 of the extension table adds `read_game_file`. A loose
file wins over a file of the same path in any archive. Where several loose
folders are layered, the first that holds the path wins, and a mod's folder
is layered ahead of the game folder.

**Boneyards.** The session's virtual-file reader calls `read_game_file` with
the runtime. The mod folder holds `coreprime.ccx`, mounted beside the
install's `.ccx` archives; the two share no path, and the profile names no
installation archives. A page the install already holds as a loose file is
therefore the install's copy, even when a newer copy of that path is inside
`coreprime.ccx`. A file that must replace one in `by.ccx`, and an updated
page whose path the install holds loose, ships as a loose file in the mod
folder, which wins over every archive, and not inside `coreprime.ccx`.
A profile that does not mount the mod still has the archive the program
carries, in the same folder as the page fonts. When the game's files do not
hold a path, the reader takes it from that archive. That is how the new
screens' layouts are found, and their art: the console's picture store
reads an animation file the game's files do not hold through the same
reader, so `anims/BY_New.gaf` (the scanner, the queue meter, the Locator's
icons and the registration code) comes from that archive too. The war map
is not in the archive. The player's
copy is still read first, and the expansion folder is still read after the
game's files, so the map in the boneyards expansion is used when the player
has none, and a map in the game folder is used when neither has it.
