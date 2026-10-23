# The extension API

An extension is a library linked into the game, `open-annihilation`, that
adds to it through one table of hooks: `oa::app::Extension`, in
[extension.hpp][ext]. This chapter is for the author of one. It says what
the table offers, how an extension joins the build, which version of the
table it follows, the rules on threads, ownership and errors, and how
extensions are tested. The header is the contract, and every hook's own
`///` block there is its full description; this chapter is the way into
it.

[ui-kit.md](ui-kit.md) is the chapter on the OA UI kit, through which an
extension builds Settings pages, screens of its own, notices, prompts and
the parts of a match overlay, in the look and with the scaling, focus and
touch reach of Open Annihilation's own screens.

[ext]: ../../src/app/include/oa/app/extension.hpp
[reg]: ../../src/ui/screen-registry/include/oa/ui/screen_registry.hpp
[regkit]: ../../src/ui/screen-registry/include/oa/ui/screen_registry/kit.hpp
[calls]: ../../src/app/include/oa/app/hook_call.hpp

## Contents

- [What an extension is](#what-an-extension-is)
- [Registering one](#registering-one)
- [The version](#the-version)
- [Threads, ownership and errors](#threads-ownership-and-errors)
- [Screens and overlays](#screens-and-overlays)
- [The services table](#the-services-table)
- [The hooks](#the-hooks)
- [The automation endpoint for an extension's windows](#the-automation-endpoint-for-an-extensions-windows)
- [Testing an extension](#testing-an-extension)
- [Version history](#version-history)

## What an extension is

An extension is a static library with one init function,
`void <init>(oa::app::Extension* table)`, which fills a zeroed table: a
`context` pointer of the extension's own and the hooks, function pointers
the engine calls at set points, such as a long option it does not know, a
frame, a match event or a drawn match frame. Every hook the extension
leaves null keeps the engine's own behaviour, so an extension fills only
what it needs.

The engine has two extensions of its own, registered in every build:
network play (`oa-app-netgame`, [src/app/netgame](../../src/app/netgame)),
which takes the 3.1c network switches and runs the multiplayer screens, the
networked match and the replay of recorded games; and the automation
endpoint (`oa-app-automation`,
[src/app/automation](../../src/app/automation/README.md)), which serves
`--fark`. A project that builds the game may register further extensions
after them. The engine's code and documents never name such an extension:
what one needs that the table does not offer becomes a new hook or a
declared header, open to every extension.

An extension reaches the game only through the table and the headers
declared for it:

- `oa/app/extension.hpp`: the table, the types its hooks take, and the
  functions an extension calls, which are not hooks (version 14 onwards);
- `oa/ui/screen_registry.hpp` and `oa/ui/screen_registry/kit.hpp`: the
  registrations of screens, overlays, dispatcher steps and queries, kit
  screens and Settings pages, and the services table;
- `oa/ui/kit/*.hpp`: the OA UI kit, part of the contract from version 15;
- `oa/app/check_host.hpp`: the table through which a check the extension
  runs drives the game, as the engine's own checks do. It is not part of
  the extension table, and `OA_EXTENSION_API_VERSION` does not number it.

A library that needs only those links `oa::app::headers`, which brings the
kit and the registrations with it. One that includes `runtime.hpp` builds
against `oa::extension-sdk`, the include directories and libraries an
extension may use. A new need is met with a hook or a declared header,
never with a new `Runtime` member or friend or another use of one of
`Runtime`'s private names
([conventions.md](../development/conventions.md#application-extensions-and-runsh)).
What network play still uses of them, and the members one extension may
still add through `OA_RUNTIME_EXTENSION_MEMBERS`, are frozen and may only
shrink: [src/app/README.md](../../src/app/README.md#extensions) describes
the `runtime-surface-names` check that holds them.

## Registering one

A project registers each extension library once, after it adds the
engine's directory, with `oa_add_extension` from
[cmake/OaExtensions.cmake](../../cmake/OaExtensions.cmake):

```cmake
add_subdirectory(open-annihilation)

add_library(sample-extension STATIC sample_extension.cpp)
target_link_libraries(sample-extension PRIVATE oa::app::headers)
oa_add_extension(sample-extension INIT oa_extension_init_sample)
```

`INIT` names a global C++ function that fills the table it is given:

```cpp
#include "oa/app/extension.hpp"

namespace {

// What the extension keeps for the process: the table's context.
struct Sample {
    bool started{};
};

Sample& sample_state() {
    static Sample state;
    return state;
}

void startup(void* context, oa::app::Runtime&) {
    static_cast<Sample*>(context)->started = true;
}

} // namespace

void oa_extension_init_sample(oa::app::Extension* table) {
    table->context = &sample_state();
    table->startup = startup;
}
```

`SWITCHES` names the letters of the game's command line that the
extension's switch handler takes, and `GAME_FILES` the commands that copy
the files it needs beside the game; the comment at the top of
`OaExtensions.cmake` describes both. When the configure ends, the engine
lists the registered extensions, each after every registered extension its
library links and the rest in the order they were registered, compiles the
list into the game and links the libraries. Network play comes first.

`main()` calls every init once, before the command line is parsed, and
`ExtensionList` combines the tables into the one the engine calls. The
header's opening comment states the rules in full; in short:

- most hooks are called for every extension, in list order;
- `shutdown` and `release_runtime` are called in reverse list order;
- a hook that takes something (an option, a switch, a run, a close
  request, a recording, the MULTI button) asks the last extension first,
  since it builds on those before it, and the first that takes it has it;
- answers are combined as each hook needs: the first answer, asking the
  last extension first, for a text such as `return_label`; the usage texts
  joined in list order; the bits of every answer for `state`; true from any
  for `keep_stored_password`, and from every one for `outcome_ready`;
- `frontend_game`, `frontend_states` and each entry of the hosts that
  `message_hooks`, `console_host` and `team_panel_host` fill belong to one
  extension at most. A second one stops the start, or the call, with a
  message naming both.

With one extension the combined table behaves as that extension's own.

## The version

`OA_EXTENSION_API_VERSION` numbers the table's contract, and
`oa::app::extension_api_version` is its typed copy. The version is 17. It
rises by one with each change to the contract: a hook added, removed or
renamed; a hook's parameters, its return value or the meaning of leaving it
null; when or in which order the engine calls it; what it may keep or must
free; its error behaviour; a type, enumerator or bit the table uses; or a
function an extension calls to read a value the engine resolved. A change
of wording alone keeps it.

Version 15 takes in the OA UI kit's public headers, `oa/ui/kit/*.hpp`, and
the registrations and services the table names. The header's history
states the rule for them: a change to a kit header before 0.8.0, the first
release that carries version 15, keeps 15, and the engine's own followers
and test extensions follow it in the same change; from 0.8.0 on, any change
to a kit header raises the version. The kit stays provisional all the same
([ui-kit.md](ui-kit.md#provisional-until-it-is-stable)).

Version 16 adds [`run_mutators`][ext]. It tells an extension how many
mutators the run plays, their ids in order, and the simulation hash the run
would have without them. That hash is 64 lower-case hexadecimal digits and
equals [`simulation_hash`][ext] when none play. The two read the same
profile and the same mutator digest, so they agree at the moment of the
call. A running match keeps both until it ends. It is not a hook.

Version 17 adds [`hear_remote_seat_chat`][ext]. It hands a chat line from
another machine to the computer seats on this machine that the line
reached. A null line, and a call while no match runs, hand nothing on.
It is not a hook.

An extension states the version it follows, so that a build against a
table of another version stops until its author has read the change. The
automation endpoint's fixture window
([window_fixture.cpp](../../src/app/automation/window_fixture.cpp)) does it
so:

```cpp
// The version of the engine's extension table these hooks follow. A build
// against a table of another version stops here until the engine's change
// has been read and the hooks follow it.
constexpr uint32_t kExtensionApiVersionFollowed = 17;
static_assert(
    oa::app::extension_api_version == kExtensionApiVersionFollowed,
    "the engine's extension table changed: follow its change, then raise "
    "kExtensionApiVersionFollowed"
);
```

The macro serves an extension that builds against more than one engine
and tests the version with `#if OA_EXTENSION_API_VERSION >= 15`.

## Threads, ownership and errors

**Threads.** The engine calls every hook on the thread that runs `main()`,
and the services table and the check host are called on that thread too.
The functions an extension calls say which may come from any hook and
which from any thread: the modern text calls (`modern_text_chain`,
`modern_text_layout`, `modern_text_pixels`) may be called from any thread,
and take turns with the faces.

**Ownership.** The extension owns `Extension::context`, which the engine
passes back unchanged to every hook, and keeps it valid until the process
exits. A pointer or a reference a hook receives is valid for that call
only, unless the hook's documentation says otherwise: the runtime given to
`startup` lives until `main()` returns, a `ScreenContext`'s host and
services may be kept while the runtime lives, and the hosts that
`console_host` and `team_panel_host` fill keep their addresses as long as
the runtime. What a registration names (a screen's state, a page's rows, a
kit screen's name and model) must live as long as the runtime. A text a
hook returns stays the extension's and is read at once. A notice or a
prompt request is copied when it is queued.

**Errors.** A hook reports bad input by throwing `std::runtime_error`, as
the engine's own code does. The engine calls every hook through one guarded
call, `call_hook` ([hook_call.hpp][calls]), which catches whatever the hook
throws before it reaches engine code. What follows is the hook's handling,
which its `///` block states and `hook_call.hpp` lists:

- **raised:** the engine raises the hook's message as its own error at the
  call. Mostly that ends the game, which prints
  `open-annihilation: <message>` and exits with status 1. A match start the
  front end falls back from (a campaign mission, a skirmish started from its
  setup, a saved game loaded from the load dialog, the in-game restart) is
  abandoned instead, and the front end runs on, its status line showing the
  message. Every hook not named below is raised.
- **reported:** the engine writes one line,
  `open-annihilation: extension hook <hook>: <message>`, to standard error
  (the game's log when it plays), and carries on mostly as it does when the
  hook is null. The same line again is written only when its count reaches a
  power of two, with the count. These are `simulation_step`, `player_gone`,
  `message_hooks`, `match_event`, `draw_loading`, `draw_match_hud`,
  `draw_match_overlay`, `pause_changed` and `load_progress`.
- **must not throw:** `speed_changed`, `app_mode_set`, `release_runtime`
  and the hooks of a replay (`ReplayHooks`). One that throws all the same is
  reported, and the engine carries on as for a null hook.

When several extensions fill a hook, those after the one that threw are not
called for that call.

## Screens and overlays

From `register_screens` an extension adds to the runtime's
`ScreenRegistry` ([screen_registry.hpp][reg]), as the engine's own screen
packages do:

- a screen, a `ScreenDesc` with an id from `kFirstPackageScreen` up, its
  GUI assets and its `enter`, `leave`, `event`, `tick` and `draw`
  (`screen_register`);
- an overlay over every screen or one, an `OverlayDesc` drawn in ascending
  `z` and given input in descending `z`, before the screen itself
  (`overlay_register`);
- a handler for a step or a query of the front end's dispatcher
  (`step_register`, `query_register`);
- a kit screen, a `KitScreenDesc` ([screen_registry/kit.hpp][regkit]) laid
  out with the OA UI kit and hosted by the engine's OA layer
  (`kit_screen_register`, version 15; [ui-kit.md](ui-kit.md#a-kit-screen)).

A registration the registry refuses stops the start once the hook returns,
with "screen registration rejected: <name>", the name of the first one it
refused. Settings pages are registered by a hook of their
own, `register_settings_pages`, called right after `register_screens`
([ui-kit.md](ui-kit.md#a-settings-page)).

Each callback receives a `ScreenContext`: the assets, the frame's surface,
the match (null outside one), the input during an event, the screen shown,
and the host and the services table. Event callbacks return nonzero to
consume the input. [src/app/README.md](../../src/app/README.md#adding-a-screen-or-overlay)
walks through adding a screen or an overlay step by step.

## The services table

`ScreenServices` ([screen_registry.hpp][reg]) is the engine's table of
services, reached through a `ScreenContext`'s `services`, with its `host`
as each entry's first argument. Its host and services stay the same while
the runtime lives, so an extension may keep them from an overlay's
`create` and use them outside its callbacks, on the main thread.

| Entries | What they do |
|---|---|
| `request_screen`, `run_frontend`, `set_frontend_signal`, `set_frontend_state` | move the front end: show a screen, run a pass of its dispatcher once the current event or frame is handled, set its signal and state bytes |
| `close_main_menu_panel`, `reload_main_menu_panel`, `set_main_menu_overlay` | take the main menu's panel away, load it again, or say that an overlay with buttons of its own stands on the main menu |
| `play_sound`, `play_sound_alternate`, `stop_sounds`, `register_sound` | play a registered sound, play one on the menu music's route, stop every sound, register a sound file under a name |
| `read_number`, `write_number`, `read_string`, `write_string` | read and write the preferences; `read_string` with a null section reads a whole key, as a Settings page's row binds one (version 15) |
| `set_status`, `set_text_input`, `current_tick`, `pointer_position` | set the status line, start or stop text input, read the front end's clock and the pointer |
| `quit` | end the run once the current event or frame is handled, leaving a running match first, with a reason and an exit status |
| `open_kit_screen`, `show_notice`, `ask_prompt` | open a registered kit screen by name, and queue a notice or a prompt with the engine's own (version 15; [ui-kit.md](ui-kit.md#notices-and-prompts)) |

Each entry's comment in the header says when it acts and what it reads.

## The hooks

Each paragraph names a group of hooks; every hook's own block in
[extension.hpp][ext] says when it is called, what it is given, what it
answers and how its errors are handled.

**The command line.** [`take_option`][ext] takes a long option the engine
does not know, with its values and the `option_effect` bits it implies;
[`check_options`][ext] checks the options taken, as a whole;
[`switch_handler`][ext] returns the handler of the game switches the
extension takes; [`text`][ext] words the usage `--help` prints and the
reason `-r` stops the start.

**Start-up and registration.** [`startup`][ext] starts the extension's
part of the runtime as it is built; [`register_screens`][ext] registers its
screens, overlays, dispatcher steps and kit screens;
[`register_settings_pages`][ext] its Settings pages; [`ready`][ext]
finishes its start once every screen exists, before the preferences load;
[`frontend_entry`][ext] gives the front end's launch values (the game name
and the nickname); [`frontend_states`][ext] names front-end states the
extension runs in place of the engine's.

**Runs and frames.** [`run_mode`][ext] offers the extension a run of its
own at three points of the run; [`start_scene`][ext] starts an interactive
run's first scene; [`frame`][ext] does its work at three stages of every
frame (`FrameStage`); [`close_requested`][ext] answers the window's close
button or the system's quit; [`app_mode_set`][ext] reports each
application mode the front end sets; [`shutdown`][ext] stops its work once
the main loop ends; [`release_runtime`][ext] frees what it keeps for a
runtime as that runtime is destroyed.

**The main menu and the front end.** [`select_multiplayer`][ext] chooses
what MULTI does, and may take the game over; [`frontend_game`][ext] returns
the front end's Game block, which the extension then keeps;
[`keep_stored_password`][ext] tells the preferences write whether to keep
the stored password; [`check_multiplayer_menu`][ext] runs
`--check-multiplayer-menu` in place of the engine's check;
[`state`][ext] returns the `extension_state` bits of the running game
(a multiplayer session, a shared match, a replay, a watcher).

**The match.** [`match_game`][ext] writes the launch values the extension
owns into a new match's Game block; [`simulation_step`][ext] runs a
pending simulation step itself; [`outcome_ready`][ext] holds a finished
match on its outcome; [`match_event`][ext] follows a match's life
(`MatchEvent`); [`player_gone`][ext] announces a player who lost the last
unit; [`give_resources`][ext] moves a console gift itself;
[`disconnect_text`][ext] words the end-of-game screen's disconnect reason;
[`return_label`][ext] names the match's return in its menus;
[`pause_changed`][ext] and [`speed_changed`][ext] report the Pause key and
the game speed the local player set; [`message_hooks`][ext],
[`console_host`][ext] and [`team_panel_host`][ext] fill the extension's
part of the message log's hooks, the console's host and the team panels'
host; [`check_console`][ext] checks the extension's console commands in
the navigation check.

**Drawing.** [`draw_loading`][ext] draws over the loading screen and
[`load_progress`][ext] follows its rows; [`draw_match_hud`][ext] draws the
extension's readouts in the HUD pass, over the side panel and the bars;
[`draw_match_overlay`][ext] draws over the battlefield through the
`MatchOverlay` painter, whose calls include the kit's text, chips, progress
bars and hover card from version 15
([ui-kit.md](ui-kit.md#drawing-in-a-match-overlay)).

**Recordings.** [`open_recording`][ext] opens a recording a director script
names and replays it into a match the engine steps one tick at a time,
through the `ReplayHooks` the extension fills.

**Functions an extension calls.** From version 14 the header also declares
functions an extension calls; none is a hook, the engine does not call
them, and no extension fills them: [`player_folder`][ext],
[`set_unit_limit`][ext], [`keep_running_while_inactive`][ext],
[`web_address_available`][ext] and [`open_web_address`][ext], the modern
text calls ([`modern_text_chain`][ext], [`modern_text_layout`][ext],
[`modern_text_pixels`][ext], [`message_log_text_size`][ext],
[`game_text_preferences_of`][ext], [`game_text_preferences`][ext],
[`set_focused_text_field`][ext]), [`read_game_file`][ext],
[`set_extension_window_source`][ext], [`extension_clock`][ext],
[`simulation_hash`][ext], from version 16 [`run_mutators`][ext], and from
version 17 [`hear_remote_seat_chat`][ext].

## The automation endpoint for an extension's windows

A program on the same machine can drive the game through the automation
endpoint ([docs/automation.md](../automation.md)), which lists the screen's
controls by name. Windows an extension draws itself are shown to it through
[`set_extension_window_source`][ext]: the extension registers a source,
which the endpoint calls on the main thread whenever the program asks for
the controls. The source appends `ExtensionWindow`s, each a name and its
`ExtensionControl`s (a kind, a place on the canvas, its state, text and a
list's rows), and the endpoint lists them ahead of the screen's own
controls, since they take the pointer over it. A null source removes it,
and at most 16 are kept. [`extension_clock`][ext] is the clock an
extension's idle work follows, so that a run the program drives advances it
by a fixed step each frame.

Pages, screens, notices and prompts built with the kit need no source: the
endpoint lists them under the OA layer's names
([ui-kit.md](ui-kit.md#automation-names)).

## Testing an extension

The engine tests its boundary with test extensions of its own, under
[tests/extension](../../tests/extension), which only a build configured
with `-DOA_RECORD_EXTENSION_HOOKS=ON` combines:

- the recorder, which fills every hook with a recorder but the two that
  network play fills, and the follower, whose library links the recorder's.
  `extension-hooks-options` and, over the installed game,
  `extension-hooks-game` check that the game calls every hook of both in the
  order the rules set;
- the automation endpoint's fixture window, which
  `native-automation-extension-windows` drives through the endpoint;
- the Kit fixture ([kit_fixture.cpp](../../tests/extension/kit_fixture.cpp)),
  which uses the OA UI kit through the table alone: a Settings page, a kit
  screen, a prompt and the overlay's kit calls. `extension-kit` drives it
  through the automation endpoint in 640x480, 1280x720 and 1920x1080
  windows, and finds its chip in a headless match's frame.

Every build also runs `app-extension-list`, which checks each rule of the
combined table over two test extensions; `app-hook-call`, which throws from
a hook of each handling; `hook-calls`, which fails when engine code calls a
hook pointer other than through `call_hook`; and
`extension-layout-mismatch`, a unit that sees `Runtime` with members the
game does not have, which must fail to link.

An extension's own tests can take the same road. A build of the game with
the extension registered runs it on the dummy drivers (`SDL_VIDEO_DRIVER`
and `SDL_AUDIO_DRIVER` set to `dummy`) with `--fark`, and a test drives it
by control name through the endpoint, as
[check_kit_fixture.py](../../tests/extension/check_kit_fixture.py) does.
`--headless-check` with `--match-ticks` and `--snapshot` runs a match
without a window and writes its frame, in which the test finds what the
extension drew over the battlefield.
[testing.md](../development/testing.md#the-kit-for-an-extension) describes
these checks, and [src/app/README.md](../../src/app/README.md#extensions)
what each test extension fills.

## Version history

One line per version; the paragraphs of
[src/app/README.md](../../src/app/README.md#extensions) and the history in
[extension.hpp][ext], beside `OA_EXTENSION_API_VERSION`, describe each.

- 1: the table before the layered layout.
- 2: the layered layout's names, `oa/app/extension.hpp` among them
  ([extension.hpp][ext]).
- 3: `select_multiplayer` in place of `offers_multiplayer`
  ([extension.hpp][ext]).
- 4: `post_message`'s sender, `console_host` at each match start, and
  `draw_match_overlay` ([extension.hpp][ext]).
- 5: `pause_changed`, `load_progress` and `team_panel_host`
  ([extension.hpp][ext]).
- 6: `close_requested`, the services `quit`, `stop_sounds`,
  `play_sound_alternate` and `run_frontend`, and `query_register`
  ([extension.hpp][ext]).
- 7: `speed_changed` and `app_mode_set` ([extension.hpp][ext]).
- 8: several extensions, each with its own init, combined by the rules;
  `return_label` and `keep_stored_password` ([extension.hpp][ext]).
- 9: `open_recording`
  ([src/app/README.md](../../src/app/README.md#extensions)).
- 10: every hook called through `call_hook`
  ([src/app/README.md](../../src/app/README.md#extensions)).
- 11: `release_runtime`
  ([src/app/README.md](../../src/app/README.md#extensions)).
- 12: `option_effect::remote_controlled`
  ([src/app/README.md](../../src/app/README.md#extensions)).
- 13: `FrameStage::presented`
  ([src/app/README.md](../../src/app/README.md#extensions)).
- 14: the functions an extension calls
  ([src/app/README.md](../../src/app/README.md#extensions)).
- 15: the OA UI kit
  ([src/app/README.md](../../src/app/README.md#extensions),
  [ui-kit.md](ui-kit.md)).
- 16: `run_mutators`
  ([extension.hpp][ext]).
