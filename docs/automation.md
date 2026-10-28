# The automation endpoint

A program on the same machine as the game can drive it and watch it
through the automation endpoint: a service on the loopback address that
the game, `open-annihilation`, serves when it is started with `--fark`. The
program reads what the game shows and holds (the screen, its controls, the
frames presented, the preferences, the running match and the battle room),
hands it keys, text, pointer and finger events as a device does, and is
told of changes as they happen. It is meant for programs that test the
game or play scripted journeys through it, on every platform the game
runs on, in the same build players run.

It is off unless `--fark` is on the command line. A player never needs it.

An MCP client such as Claude Code can use the endpoint through the MCP
server in [tools/mcp](../tools/mcp/README.md) ([below](#through-mcp)).

## Contents

- [Turning it on](#turning-it-on)
- [Connecting](#connecting)
- [Requests](#requests)
- [OA's own screens](#oas-own-screens)
- [Input](#input)
- [Pressing a bound key](#pressing-a-bound-key)
- [Opening the key screens](#opening-the-key-screens)
- [Events](#events)
- [What it never does](#what-it-never-does)
- [A map list's names](#a-map-lists-names)
- [A list that does not fit](#a-list-that-does-not-fit)
- [In a battle room](#in-a-battle-room)
- [The welcome setup](#the-welcome-setup)
- [Through MCP](#through-mcp)
- [Checks](#checks)

## Turning it on

```
--fark                     serve the automation endpoint
--fark-address ADDRESS     where: 127.0.0.1:PORT or [::1]:PORT; port 0, the default
                           (127.0.0.1:0), lets the system choose
--fark-file PATH           where the address and token are written; by default
                           automation.json in the folder of the preferences file
```

- Only the command line turns it on: no setting, environment variable or
  file does.
- Only the loopback address can be served. Any other is refused with
  `automation: only a loopback address can be served`, and the game exits
  as for any refused option. `--fark-address` or `--fark-file` without
  `--fark` is refused too.
- The endpoint is served from the game's main loop, so `--fark` cannot go
  with an option that runs no main loop or runs its own:
  `--headless-check` and the options that imply it, the `--check-*`
  options, `--benchmark`, `--match-ticks`, `--render-script`,
  `--generate-script` and `--showcase`. It goes with `--host` and
  `--join`, so a game can start in a battle room and still be driven.
- With no window to open, start the game on SDL's dummy drivers
  (`SDL_VIDEO_DRIVER=dummy SDL_AUDIO_DRIVER=dummy`): the endpoint works
  the same, frames included.
- While `--fark` is on, the game keeps running every frame when its window
  is not the active one, so the endpoint is always served.

As it starts, the game listens and writes the file, whole (first beside
it, then renamed over it) and readable only by its owner where the system
has such modes:

```json
{"version":1,"address":"127.0.0.1:53122","token":"9c1f…(32 hex digits)","pid":41877}
```

The token is 128 random bits from the system's generator, new each run.
The log says `automation: serving on 127.0.0.1:53122` and where the file
is, and never holds the token. [settings.md](settings.md#where-the-settings-are-kept)
says where the preferences file, and so the default file, is kept;
`--preferences-file` names another.

## Connecting

The endpoint speaks the automation protocol over one TCP connection. Every
message, both ways, is a frame: a header line, a JSON object and an
optional payload of raw bytes.

```
AUTO/1 - <json-length> <payload-length> <crc32>\n
<json-length bytes of UTF-8 JSON><payload-length bytes>
```

The lengths are in decimal, and the CRC is CRC-32 (IEEE 802.3) over the
JSON and the payload together, in eight lowercase hexadecimal digits. A
JSON part above 1 MiB or a payload above 64 MiB is refused.
[src/app/automation](../src/app/automation/README.md#the-codec) gives the
framing in full.

- One client at a time. A connection whose first bytes are not `AUTO/1 `
  is closed unanswered.
- The first request is `hello` with the file's token:
  `{"id": 1, "op": "hello", "versions": [1], "client": "NAME", "token": "…"}`.
  Its JSON is at most 4 KiB, with no payload, or the connection is closed
  unanswered. A wrong token is answered `denied` and the connection
  closed. While a client is connected, a second connection is answered
  `busy` and closed.
- A request is `{"id", "op", ...}`. Its answer is
  `{"id", "ok": true, "frame", "tick", ...}`, where `frame` counts the
  frames served and `tick` is the running match's, or
  `{"id", "ok": false, "error": {"code", "message"}}`. Requests are
  answered in order. A client may send several requests at once and then
  end its side of the connection: each is still answered before the
  connection closes. The game reads no more from a client that sends
  faster than it answers until it has caught up.
- Every text the game gives is UTF-8. Text it holds in a code page that is
  not, as an installation whose files use another code page or another
  player's game can give it (captions, list rows, names, chat,
  preferences), is replaced whole by `Non-Unicode Text Error::` followed
  by the standard base64 (RFC 4648, with padding) of its bytes, from which
  the client recovers them: the four bytes of `Jörg` in Latin-1 come as
  `"Non-Unicode Text Error::SvZyZw=="`.

A short client in Python:

```python
import json, socket, zlib

def frame(message):
    body = json.dumps(message).encode()
    return b"AUTO/1 - %d 0 %08x\n" % (len(body), zlib.crc32(body)) + body

def read(sock):
    header = b""
    while not header.endswith(b"\n"):
        header += sock.recv(1)
    _, _, json_length, payload_length, _ = header.split()
    size = int(json_length) + int(payload_length)
    data = b""
    while len(data) < size:
        data += sock.recv(size - len(data))
    return json.loads(data[:int(json_length)]), data[int(json_length):]

endpoint = json.load(open("automation.json"))
host, _, port = endpoint["address"].rpartition(":")
sock = socket.create_connection((host.strip("[]"), int(port)))
sock.sendall(frame({"id": 1, "op": "hello", "versions": [1], "client": "example",
                    "token": endpoint["token"]}))
print(read(sock)[0])
sock.sendall(frame({"id": 2, "op": "screen"}))
print(read(sock)[0]["screen"])  # main_menu
```

## Requests

| Request | What it answers |
|---|---|
| `hello` | the protocol's version, the engine's, the window's size, where the game's 640x480 canvas lies in it, the tick rate (30) |
| `screen` | the screen shown (`main_menu`, `skirmish`, `mp_battleroom`, `match` and the like), the dialogs over it, the control that takes the keys, the pointer, and in a match where the camera looks |
| `controls` | an extension's windows first, then a message box's, then Open Annihilation's own screens' ([below](#oas-own-screens)), then every control of the panel that takes the pointer, a dialog's first: its name, its kind, the extension window it belongs to (or none), its place on the canvas and in the window, its state and text, and a list's rows. A map list's rows are the words it shows, and `item_keys` names each row ([below](#a-map-lists-names)) |
| `input` | once the game has taken the events it was given, the frame and tick at which it took them ([below](#input)) |
| `frame` | the frame the game presented in its window, or the one it composed, whole or a region of it, as RGB pixels, a PNG file, or a hash and mean colour |
| `prefs` | the preferences as the game holds them now, and their file |
| `match` | the running match: pause, speed, each player's name, side, team, allies, units, kills and losses, the outcome and winner; with `state_hash`, the digest of its state that a saved game carries |
| `room` | the battle room: the game's name, its host, the seated players with their ready flags, sides, colours, teams and pings, the map, the options and the chat |
| `subscribe` | the kinds of event the client is sent from then on |
| `quit` | nothing more: the game then quits as the main menu's Exit does, leaving a running match first with no question |

[src/app/automation](../src/app/automation/README.md#the-protocol) gives
every request's fields and answers, and the errors it is refused with.

## OA's own screens

Open Annihilation's own screens (the OA button and the main menu's
Settings button, Settings, a notice, a prompt, and every later one: the
Library, the map browser, the key editor, the update screens, Arrange
Overlays, the gallery) are drawn by the engine's UI kit,
and `controls` lists them under names of one scheme, so that a journey
presses them by name as it presses the original screens' controls.

**The names.** Each control is listed as `oa.` followed by its kit name:
lower-case words of `a`–`z`, `0`–`9` and `-` joined by dots, at most 120
bytes in all, unique ignoring case. Every word is one of the code's, never
a text as the player sees it, so no name changes with the language:

- a word made from a code name is the enumerator's name with `_` as `-`;
- a word made from a value is the word the setting's preference stores,
  lower-cased, every other character a `-` (`1/32` gives `1-32`, `zh-Hans`
  gives `zh-hans`);
- a button's word is its English caption as the code holds it, lower-cased,
  an ellipsis dropped and spaces as `-` (Manage… gives `manage`).

A part of a control that takes a press of its own is listed after it with
one word more: a switch's halves `.off` and `.on`, a strip's levels and an
open drop-down's items by their values, a row of buttons' buttons by their
words. A click at a part's centre presses that part; a click at a switch's
centre would land on the line between Off and On.

| Control | Name | Kind, and what it reports |
|---|---|---|
| The OA button (main menu, in-game menu) | `oa.button` | button; no dialog; on the main menu, the mods icon (the plates over drive bays) at the menu's bottom right, left of the Settings button, where it opens the Library, its text "Library · 1 package" or "Library · N packages", and "Library · N packages · 1 update" or "Library · N packages · M updates" while updates wait; a package is a mod, a map pack, a language pack or a downloaded expansion; while the package count is not final yet the package count is left out, so the text is "Library", or "Library · 1 update" and "Library · N updates"; in the in-game menu, the settings icon under Resume, where it opens Settings, empty |
| The Settings button (main menu) | `oa.settings-button` | button; no dialog; the settings icon right of the OA button, which opens Settings with no Library under it; its text "Settings" |
| GET MAP in the battle room, while a joiner lacks the host's pack map | `oa.get-map`, `oa.get-map-text`, `oa.get-map-progress` | no dialog, listed only while they show: the button (its text "GET MAP"); a label, its text the line saying what it fetches; a label, its text how much of the download has come, as a percentage, while the bar shows in the button's place |
| A section's entry | `oa.settings.nav.<page>` | check box, checked while its section shows |
| A switch, and its halves | `oa.settings.<row>`, `oa.settings.<row>.off`, `oa.settings.<row>.on` | the switch a label with its text; the halves check boxes, checked on the half shown |
| A level strip, and its levels | `oa.settings.<row>`, `oa.settings.<row>.<value>` | label; check boxes, checked on the level chosen |
| A drop-down, closed | `oa.settings.<row>` | button; its text the choice shown |
| A drop-down's item, while open | `oa.settings.<row>.<value>` | check box, checked on the choice |
| A slider | `oa.settings.<row>` | slider; its text the value shown |
| A row's own button (Manage…) | `oa.settings.<row>.<button>`, as `oa.settings.game-files-summary.manage` | button |
| The key layout card's buttons, on Controls | `oa.settings.key-layout.edit`, `oa.settings.key-layout.change` | button, their texts "Edit keys…" and "Change layout…". Edit keys… opens the key editor over Settings, on Common commands. Change layout… is listed only where the Library opens over Settings, and opens it on its Keys tab |
| Hold keys that latch's Choose… | `oa.settings.latching.choose` | button, its text "Choose…". It opens the key editor over Settings, on Latchable |
| The practice pad's Turn every helper off, on Accessibility | `oa.settings.practice.helpers-off` | button, its text "Turn every helper off"; disabled while every helper it turns off is already off |
| Window size, on User Interface | `oa.settings.window-size`, and while open `.auto`, `.compact`, `.regular` and `.large` | drop-down, its text the choice shown. It takes effect at OK |
| Reset window positions, on User Interface | `oa.settings.reset-window-positions.reset` | button, its text "Reset". It puts every moved window back in its centred place at once |
| Look, on User Interface | `oa.settings.look`, `oa.settings.look.modern`, `oa.settings.look.classic` | as a level strip. It applies as it is chosen |
| Edge scrolling | `oa.settings.edge-scroll`, with `.off` and `.on` | as a switch. Off stops the view scrolling while the pointer rests at the window's edge |
| Key layout with the mod being played | `oa.settings.key-layout-for-mod`, and while open `.same` and `.<id>` (a layout's id, as `.ridge-keys`) | drop-down, its text the choice shown; absent while no mod is played |
| Show keys on buttons | `oa.settings.show-keys-on-buttons`, with `.off` and `.on` | as a switch |
| Game Content's buttons | `oa.settings.content-check.check-again`, `oa.settings.content-copy.copy-again`, `oa.settings.content-open.open-folder` | button. The section's entry is `oa.settings.nav.game-content`. While a bundle is being copied, that bundle's row offers Cancel as `oa.settings.<id>.cancel-copy` |
| Updates' card | `oa.settings.update-status.<action>`, up to three, the first in the accent: `check-now`, `download`, `pause`, `resume`, `cancel-download`, `when-i-quit` (Install when I quit), `restart-now` (the route's install-now action, as Install and restart), `unschedule` (Don't install when I quit), `skip`, `try-version`, `try-again`, `details`, `show-the-log`, `page` (Open the release page) and `whats-new` | button, its text the action's caption. The card shows the actions the update centre's state offers now, as the menu's update centre does, less Roll back…, Later and Keep it ready. Check now is absent while Check for updates is Never. The section's entry is `oa.settings.nav.updates` |
| Updates' rows | `oa.settings.update-download` with `.off` and `.on` (Download automatically); `oa.settings.update-check`, and while open `.automatically`, `.when-asked` and `.never` (Check for updates); `oa.settings.update-install`, and while open `.ask` and `.when-i-quit` (Install updates); `oa.settings.update-previous.roll-back` (Roll back…, while a version is kept) | a switch, two drop-downs and a button. Roll back… asks the question `update-previous`, whose buttons are `oa.update-previous.cancel` and `oa.update-previous.roll-back` |
| Your files' buttons, on System | `oa.settings.user-folder.saves`, `.screenshots`, `.mods` | button |
| File Type Registration, on System on Windows, Linux and macOS | `oa.settings.file-type-registration` (the heading); `oa.settings.file-type-oamod`, `oa.settings.file-type-oamap`, `oa.settings.file-type-oalang`, `oa.settings.file-type-oareg`, `oa.settings.file-type-oamut`, `oa.settings.file-type-oabrain`, `oa.settings.file-type-oakeys`, `oa.settings.file-type-oalaunch` | the heading a label; each switch as a switch |
| File types, on System on iOS | `oa.settings.file-types` | label |
| Language's MORE IN THE LIBRARY… (main menu only) | `oa.settings.more-in-library` | button |
| Downloads' Registries in the Library… (main menu only) | `oa.settings.registries-in-library` | button; opens the Library on its Registries tab |
| Developer's own controls | `oa.settings.developer-mode.off`/`.on`, `oa.settings.frame-stats.off`/`.on`, `oa.settings.kit-gallery.show` (SHOW, which opens the UI kit gallery), `oa.settings.active-only.off`/`.on`, `oa.settings.restore-profile-values` | as above |
| Developer's list | `oa.settings.hack-area.<area>`, `oa.settings.hack.<hack>` (its header, with its switch's `.off`/`.on`), `oa.settings.hack.<hack>.<parameter>` (a switch's with `.off`/`.on`) | the headers check boxes, a hack's checked while it is on; a parameter's switch as above, its slider a slider |
| The title bar's close mark | `oa.settings.close-x` | button, its text "Close", at the top right. It asks for the same close as Cancel |
| The footer | `oa.settings.restore-defaults`, `oa.settings.cancel`, `oa.settings.ok` | button |
| The section's scroll bar | `oa.settings.scroll-bar` | slider |
| A notice's buttons | `oa.<word>.ok`, `oa.<word>.open` | button |
| A prompt's buttons | `oa.<word>.<id>`, or `oa.<word>.button-<n>` from 1 where a button has no id | button |
| A notice's or a prompt's words | `oa.<word>.body` | label; its title and paragraphs, one a line |
| The Library's UNINSTALL in an installed package's details, its question and its notices | `oa.library.action.uninstall`; the question's `oa.prompt.cancel` and `oa.prompt.uninstall`; a notice's `oa.prompt.ok` | button; UNINSTALL is listed disabled for a map pack during a game and while the package is in the download queue, and `oa.library.note` then says why |
| The UI kit gallery's working samples, in dialog `oa.gallery` | `oa.gallery.<section>.<n>` from 1, as `oa.gallery.toggle.1` (with `.off`/`.on`), `oa.gallery.drop-down.1`; CLOSE `oa.gallery.close` | as above, by kind |
| The Library's tabs, in dialog `oa.library` | `oa.library.tab.<tab>`: `active`, `mods`, `maps`, `mutators`, `brains`, `keys`, `languages`, `updates`, `registries`, in that order, so the last is `oa.library.tab.registries` | check box, checked on the tab shown; the Library opens on `active` when no tab is asked for; every tab after Active reads its title and its count in brackets, "Updates (2)", and Active's text stays "Active" |
| The Library's Registries tab | `oa.library.registry.<id>` with its halves `.off` and `.on`, and for a registry the player added `.remove` and `.add-mirror`; `.key` (the signing-key icon on the card; its text the fingerprints, one a line), `.refresh` (REFRESH, beside the switch, disabled while that registry is off or already being asked), and the labels `.address` (the address it was added from, or its catalogue's address) and `.refreshed` ("refreshed 5 min ago", "never refreshed", or "couldn't reach · 2 h ago"); `oa.library.add-registry` at the top right of the tab, above the cards, absent during a game; the labels `oa.library.registries.note` and `oa.library.registries.notice` | each registry is a card. The switch is a label, its halves check boxes; the key icon, REFRESH, REMOVE, ADD MIRROR… and ADD REGISTRY… are buttons. The heading (the title, or the short name) and the description are drawn text, not controls. Built in is a neutral pill under the key icon, REFRESH and the switch, and it is not a control. The labels' text is the address, the refresh time, the note under the last card and, while a change was refused, why |
| The Library's filters | `oa.library.filter.<filter>`: `all`, `new`, `installed`, `updates`; at Compact the drop-down `oa.library.filter`, its open items under the same names | a chip a button, a column's entry or an open item a check box, checked on the tab's own filter; the text the filter and how many packages it holds on the tab, whatever the tag and the search: "Installed (4)"; the drop-down a button, its text the choice shown |
| The Library's Active tab | `oa.library.active.base`, `oa.library.active.change`, `oa.library.active.expansions`, `oa.library.active.expansion.<id>`, `oa.library.active.mod`, `oa.library.active.language`, `oa.library.active.map.<n>` from 1 in the order shown; `oa.library.active.from.<n>` for the same map; while a mod is played, `oa.library.active.unload` | label on the base-game tile ("Total Annihilation · 3.1c"), the expansions heading ("EXPANSIONS · 5 OF 6 ON"), the mod tile, the language tile and each map line: "No Mod" or "Ridge 4.8 · rev 3"; "Deutsch 1.2" or "English · built in"; "Ridgeline · 4 players · 16x16". Those labels take no press, except a map whose pack holds that map alone: its title is the link `oa.library.active.from.<n>`, and the link's text is the title ("Atoll"). A pack of several keeps the From line as the link, "From Core Map Pack". Choosing either opens the Maps tab with that pack selected and in view. The link is left out while its line is scrolled out of the area. On a small window the page scrolls as one when the tiles leave fewer than four map rows, and a map's label and link are absent until that line is in view. CHANGE… is a button, its text "CHANGE…", at the base-game tile's bottom right. Each expansion is one button `oa.library.active.expansion.<id>` (the bundle id, such as `campaign`). Its text is the expansion's name, or "<name> · ADD FROM…" when it is not copied and no registry offers it. One a registry offers and that is not copied keeps the name, and the download arrow sits at the right inside that chip. The whole chip is the press: the same button asks for the download, cancels it while it is in the queue, and tries again after a failure. There is no separate `.get`, `.cancel` or `.retry` control. Unload mod is a button, its text "Unload mod", at the mod tile's bottom right; pressing it asks what Switch off asks. It is listed disabled during a game and while the command line chose the mod, and is absent on the No Mod tile. The expansion pills are listed disabled during a game and while the command line named the content choice, except while a download can be cancelled, which stays enabled. A download or a retry is listed disabled then too, and while that registry's downloads are off |
| The Add registry window, in dialog `oa.add-registry` | `oa.add-registry.url`, `oa.add-registry.key-field` (the Signing key field, under the address, for an address add only; absent for an `.oareg` file and for a mirror), `oa.add-registry.check`, `oa.add-registry.add`, `oa.add-registry.cancel`, and the labels `oa.add-registry.name`, `oa.add-registry.address`, `oa.add-registry.key` (the fingerprints), `oa.add-registry.refusal` | the fields are text fields; the buttons are buttons; the labels are labels. The names of the controls that were already there are unchanged |

`<page>` is the section's word: `controls`, `accessibility`, `touch`,
`controller`, `graphics`, `user-interface`, `language`, `game-files`,
`downloads`, `updates`, `game-content`, `engine-limits`, `system` and
`developer`, in the order the section list shows them, and `mod-keys` for
a mod's keys. A stage
file's `settings system` opens System, and `settings engine-limits` opens
Engine Limits. A stage file may still say `common-tweaks` for that
section. Language & Text keeps the word
`language`, so `oa.settings.nav.language` still names that section, and
User Interface is `user-interface`. `<row>` is the setting's
(`wheel-zoom`, `hud-scaling`), `<value>` the stored
word (`whole-map`, `whole-steps`, `1-32`). `<hack>`
is the standard hack's id with its dots kept. `<word>` is a notice's or a
prompt's own word: `notice` and `prompt` unless the code that raises it
gives one, as the missing-language question does (`language-notice`). The
installs' prompts' ids are their answers: `cancel`, `ok`, `replace`,
`alongside`, `reinstall`, `open-folder`, `play-now`. The Library's
Uninstall question names its buttons `cancel` and `uninstall`, as the
update question names its `not-now` and `update`, and the notices UNINSTALL
shows instead of it for the mod being played and the language pack in use
name their one button `ok`. Its Switch Mod question names its buttons
`cancel` and `switch`, its Roll Back Mod question `cancel` and
`roll-back`, and the note Play now shows for a folder it cannot play its
one button `ok`.

**The kinds.** Each kit control maps to one kind: a button, a link and a
row of buttons' button are `button`; a switch, a strip and a row of
buttons themselves are `label` with the shown text, their parts listed
after them; a drop-down is a `button` whose text is the shown choice, an
open one's items `check box`; a slider and a scroll bar are `slider`; a tab
and a list's row are `check box`, checked while selected; a text field is
`text field` with the typed text; an area is `area`, but one neither
enabled nor focusable is a line a journey reads, such as a notice's words,
listed as `label` with its text; a list is `list` with `items` empty, its
rows being controls of their own.

**Where they are listed.** `dialog` is the OA screen's name after `oa.`:
`oa.settings`, `oa.notice`, `oa.prompt` (or `oa.<word>`), and `oa.<name>`
for every later screen; it is null for `oa.button` and
`oa.settings-button`, so that `screen`'s `dialogs` names the screen that
is open. The OA screen's controls come
right after a message box's, ahead of everything else but an extension's
windows. While an OA screen takes every input, the controls listed after
it, the screen's own and its panels', are listed disabled. `rect` is on
the canvas, which on the menus is the 640x480 frame while the OA screens
are drawn in the window's pixels: `window_rect` is the exact rectangle in
the window, and `rect` that rectangle's corners mapped back onto the
canvas, rounded outwards, so it may reach past 640. A click at `rect`'s
centre through `input` presses the control or part at every window size.

**Every other OA screen** is listed by the same rule, with no table of its
own: each control `oa.` and its kit name, each part as above, `dialog`
`oa.` and the screen's name. A screen names its kit controls with `[a-z0-9-]`
words joined by dots, and a journey clicks the listed names, `oa.`
included.

**An example journey.** Turning the mouse wheel's zoom off and letting the
map zoom out to the whole map, from the main menu:

1. click `oa.settings-button`, which opens Settings, and wait until
   `oa.settings.ok` is listed;
2. click `oa.settings.nav.controls`, then `oa.settings.wheel-zoom.off`
   (its `checked` turns true);
3. click `oa.settings.max-zoom-out`, then
   `oa.settings.max-zoom-out.whole-map`;
4. click `oa.settings.ok`: `prefs` then holds
   `open-annihilation.wheel-zoom` `0` and `open-annihilation.max-zoom-out`
   `whole-map`, and the main menu shows again (had Settings opened over
   the Library, the Library shows: click `oa.library.close` to go back to
   the main menu).

### The Library

The Library (`dialog` `oa.library`) names every control as its module
lists them ([src/ui/library](../src/ui/library/README.md#the-screens)).
Its rows and their state icons are listed so:

| Control | Name | Kind, and what it reports |
|---|---|---|
| A row of the list | `oa.library.row.<registry>.<key>`, or `oa.library.row.<key>` for a package of the player's own | check box, checked while chosen; its text the package's name, and " · Open Annihilation" or " · Cavedog Entertainment" when the row shows that maker's mark |
| A row's download arrow, its GET | `oa.library.row.<registry>.<key>.get` | button, its text "GET", listed just before its row; disabled while the details offer no GET that can act: blocked, in the download queue, or failed |
| A row's circled up-arrow, its UPDATE | `oa.library.row.<registry>.<key>.update` | button, its text "UPDATE", listed just before its row; disabled while the details offer no UPDATE that can act |
| The details' buttons | `oa.library.action.get`, `.update`, `.cancel`, `.retry`, `.play-now`, `.switch-off`, `.roll-back`, `.uninstall`, `.open-folder`, `.homepage` | button, its text the button's words; Switch off stands in Play now's place on the mod being played, and both are listed disabled during a game and while the command line chose the mod. The Active tab's Unload mod (`oa.library.active.unload`) asks the same question |
| Switch off's Switch Mod question, and the note Play now shows for a folder it cannot play | `oa.prompt.cancel`, `oa.prompt.switch`; the note's `oa.prompt.ok`; `oa.prompt.body` | button; label |
| The header's refresh icon | `oa.library.refresh` | button, its text "Refresh", or "Refreshing" while it turns; disabled while no registry can be asked, while one is being asked and during a game. F5 presses it |
| The header's close mark | `oa.library.close-x` | button, its text "Close", to the right of the refresh icon. It asks for the same close as `oa.library.close` |
| The details' and the footer's lines | `oa.library.status`, `oa.library.from`, `oa.library.rules`, `oa.library.note`, `oa.library.queue` | label, its text the words shown |

A row's circled check, installed and up to date, is no control. A click
at a row's centre chooses it; a click at its icon's centre presses GET or
UPDATE for that row, as the details' button does, asking the same
questions. `<registry>` and `<key>` are the registry's id and the package's
key, lower-cased, every other run of characters one `-`. The base game
entry is not a package. Its key is `@base-game`, and the row is
`oa.library.row.base-game`: the `@` is not a letter or a digit, so the
row's name does not keep it.

Play now and the Switch Mod question's `oa.prompt.switch` end the run
with a soft restart, and the next run opens the Library again as it
showed ([after a switch](library.md#after-a-switch)). Until it does, for
at most five seconds, the switch's work dialog, `oa.work`, may show over
the main menu, its one part the label `oa.progress.body` with its words,
such as "Switching on Ridge…": a journey waits for `oa.library` there.

### The update screens

The update screens read one state, the update centre's, and name their
buttons by the action each takes:

| Control | Name | Kind, and what it reports |
|---|---|---|
| The update pill, at the main menu's top right while a download is under way, paused, being checked, ready, scheduled or stopped | `oa.update-pill.open`, and its button `oa.update-pill.pause`, `.resume`, `.install`, `.change` or `.retry` | the pill itself, its text the pill's words ("0.8.8 · 41%", "0.8.8 is ready"); a press on it opens the update centre. Its one button is Pause while downloading, Resume while paused, Install… while ready (the ready prompt again), Change while scheduled (the update centre) and Try again after a stopped download |
| The update centre, in dialog `oa.update-centre` | `oa.update-centre.<action>`, then `oa.update-centre.close` | button. The actions are the state's, with the same words as Settings' card: `pause`, `resume`, `cancel-download`, `when-i-quit`, `restart-now`, `keep-ready`, `unschedule`, `try-again` and the others. It follows the state while it shows: Pause turns into Resume |
| The offer, in dialog `oa.prompt` | `oa.prompt.skip`, `oa.prompt.later`, `oa.prompt.update` | button: Skip this version, Later and Download update (Open the release page where this copy can only open it) |
| The ready prompt, in dialog `oa.ready-update` | `oa.ready-update.keep-ready`, `.restart-now`, `.when-i-quit` | button: Keep it ready, the install-now action named for the route (Install and restart, Open the installer, Close the game) and Install when I quit |
| The recovery screen, in dialog `oa.update-receipt` | `oa.update-receipt.show-the-log`, `.try-again`, `.ok` | button: Show the log, Try again where the release can be tried again, and Continue |
| The roll back question, in dialog `oa.update-previous` | `oa.update-previous.cancel`, `oa.update-previous.roll-back` | button: Cancel, marked, and Roll back and restart |

Each one's words are `oa.<word>.body`, as for every question.

### The keys overlay

The keys overlay (`dialog` `oa.keys-overlay`) lists the keys of the
contexts that are active in a match. It is not modal: input it does not
take passes to the match, and the match keeps running. Its names are the
module's ([src/ui/key-editor](../src/ui/key-editor/README.md)):

| Control | Name | Kind, and what it reports |
|---|---|---|
| The list | `oa.keys-overlay.list` | list, its text every word the panel shows, one line each: the title, the layout and its changes, the On line and its chips, each group's name and its lines (name, "held" where it is held, and chord), and the footer |
| Edit keys… | `oa.keys-overlay.edit` | button. It closes the overlay and opens `oa.key-editor` |
| The close mark | `oa.keys-overlay.close` | button |
| The scroll bar | `oa.keys-overlay.bar` | scroll bar, present while the list scrolls |

### Arrange Overlays

Settings › Accessibility's **Move overlays** (`oa.settings.move-overlays.on`,
`.off`) turns the overlays' places on; **Overlay places**' ARRANGE…
(`oa.settings.arrange-overlays.arrange`, enabled while Move overlays is on)
opens Arrange Overlays (`dialog` `oa.arrange-overlays`), a modal screen as
large as the window that pictures the match and places the keys overlay,
the chat line and the latch chips. In a match the in-game menu's OA
Settings button (`oa.button`, its text "OA Settings") opens Settings.

| Control | Name | Kind, and what it reports |
|---|---|---|
| A handle | `oa.arrange-overlays.keys-overlay`, `.chat-line`, `.latch-chips` | button, its text the handle's words ("Keys Overlay", "Chat Line", "Latch Chips"), its rectangle where that overlay's sample shows. A drag moves it on the 8-point grid; a double-click puts it back |
| Reset, Cancel, Done | `oa.arrange-overlays.reset`, `oa.arrange-overlays.cancel`, `oa.arrange-overlays.done` | button. Reset puts every overlay back where it lays itself out; Cancel puts the places back as they were when the screen opened; Done keeps them. Cancel and Done go back to Settings |

### The key editor

The key editor (`dialog` `oa.key-editor`) lists every rebindable action,
opening on Common commands. Its names are the module's, and
[src/ui/key-editor](../src/ui/key-editor/README.md#the-screen) lists every
one with what it is. The ones a journey uses most:

| Control | Name | Kind, and what it reports |
|---|---|---|
| The search, and Find by key beside it | `oa.key-editor.search`, `oa.key-editor.find` | text field; button. Typing in the search only searches. Find by key opens `oa.key-editor.find-key`, which listens for a chord and lists each action on it as `oa.key-editor.find-key.row.<n>` from 1, with `oa.key-editor.find-key.close`; it changes nothing |
| A row | `oa.key-editor.row.<id>` | list, its text the action's name and its chords |
| A chord's Change…, and its ✕ | `oa.key-editor.detail.chord.<n>`, `oa.key-editor.detail.chord.<n>.remove` | button, numbered from 1. A fixed chord has neither |
| + Add a key | `oa.key-editor.detail.add` | button |
| The capture | `oa.key-editor.capture`, its well `oa.key-editor.capture.well`, its line `oa.key-editor.capture.status` | label, its text the title, the chord and the line that says the chord is free, what uses it, or why it is refused |
| Same place on every keyboard, and How it acts | `oa.key-editor.capture.means`; `oa.key-editor.capture.starts.press`, `.double`, `.hold` | under More options (`oa.key-editor.capture.more`). The switch's parts are `.off` (the letter) and `.on` (the key's place). How it acts is a press, a double press or a hold |
| The capture's buttons | `oa.key-editor.capture.cancel`, `oa.key-editor.capture.ok` | button. `ok` is Use {chord} for {action}, or Choose what happens… for a chord in use, and is disabled while the chord is refused or still waiting |
| The clash question | `oa.key-editor.clash`; its outcomes `oa.key-editor.clash.replace` (Move it to {action}) and `.swap` (Swap them), or, for a clash already in the draft, `oa.key-editor.card.keep-here` and `.keep-there`; `oa.key-editor.clash.confirm`, `.another` and `.cancel` | the question a label, its text the question and every outcome; the rest buttons. A press on an outcome only marks it: nothing changes until the confirm button, which is named after the marked outcome |
| Settle…, on a draft clash in the details | `oa.key-editor.card.settle` | button. It opens the clash question for that clash |
| The footer | `oa.key-editor.draft` (the draft line), `oa.key-editor.undo`, `oa.key-editor.more`, `oa.key-editor.cancel`, `oa.key-editor.save` | the draft line a label; the rest buttons. Restore defaults, Import…, Share… and Paste… are `oa.key-editor.restore`, `.import`, `.share` and `.paste`, items of More |
| A key on the keyboard, and the inspector | `oa.key-editor.keyboard.<code word>`; `oa.key-editor.key-card` with `oa.key-editor.key-card.change` and `.use` | button; label, its text the key and every line it shows. A press on a key only inspects it; the inspector's buttons open the capture |

Closing with unsaved edits asks its own question, `oa.key-editor.leave`,
whose text is the question and, while a clash holds Save back, the line
that says so. Its buttons are `oa.key-editor.leave.save` (absent while a
clash holds Save back), `oa.key-editor.leave.discard` and
`oa.key-editor.leave.keep-editing`. Choosing another layout with unsaved
edits asks `oa.prompt.keep-them`, `oa.prompt.set-aside` and
`oa.prompt.cancel`. Its words are `oa.prompt.body`. On a menu that
question is the prompt screen. In a match the same question is
`oa.key-editor-prompt`, because a prompt does not place there, and its
controls stay `oa.prompt.*`. The name field is
`oa.key-editor.name.field`, with `oa.key-editor.name.cancel` and
`oa.key-editor.name.save`, on `oa.key-editor-name`. A failed save's notice
is `oa.notice` on a menu and `oa.key-editor-notice` in a match.

### Brains and mutators in the rooms

OA's additions to the skirmish setup, the new-campaign screen (and Any
Mission) and the battle room are drawn in the room's own art, and listed
with these names:

| Control | Name | Kind, and what it reports |
|---|---|---|
| A computer seat's brain, on the skirmish setup | `oa.brain-seat.<slot>` | button, its text the seat's whole label: the notch at the right end of that row's Player button, which opens the brain menu. The right button anywhere on the Player button opens it too |
| The campaign's computer player, on NEWGAME and Any Mission | `oa.brain-seat.campaign` | button under Difficulty, its text the seat's whole label; a press opens the brain menu |
| A computer row in the battle room | `oa.brain-seat.<slot>` (its Resolution cell), `oa.brain-seat-level.<slot>` (its Mem cell, while it shows a brain's level), `oa.brain-seat-menu.<slot>` (the notch on its name) | the Resolution cell a button on a computer this machine runs, its text the seat's whole label, and an area on another machine's; the Mem cell a button, its text the level, which a press steps; the notch a button. The Resolution cell and the notch open the brain menu |
| The brain menu, while open | `oa.brain-menu.row.<id>` (`oa.brain-menu.row.built-in` for the built-in player), `oa.brain-menu.level.previous` and `.next` (on the brain the seat plays), `oa.brain-menu.options`, `oa.brain-menu.every-seat`, `oa.brain-menu.get-brains` | a row a list item, its text the brain's name and, after " · ", its level, checked on the brain the seat plays and disabled on one that cannot play here; the arrows, Options… and Get more brains… buttons; Use for every computer seat a toggle, checked while ticked. Options… is listed while the seat's brain has options, and opens the brain picker on that seat. Use for every computer seat is absent for the campaign, and Get more brains… in the battle room |
| The host's Mutators | `oa.mutators` | button, its text "Mutators · off" or "Mutators · N on", checked while its lamp is green; a press opens the mutator picker |
| The line beside START while a player lacks a mutator | `oa.mutators.waiting` | label, its text the line |
| A guest's Mutators, in the battle room | `oa.mutators-view` | button where the host's stands, as large as Select Map and over it, its text "Mutators · N on" or "Mutators · off", checked while this machine has built the host's list; a press opens the host's set, read only |
| A guest's GET MUTATORS, its bar and its line | `oa.get-mutators`, `oa.get-mutators-progress`, `oa.get-mutators-text`, `oa.get-mutators-ready` | listed only while they show, left of the guest's Mutators: the button; a label, its text the percent while a download runs; a label, its text the whole line (the set, or why a mutator cannot be fetched); and the "Ready once … is here" line beside START |

`<slot>` is the row's number in the room, from 0. A seat's text is its
label, as "Steady Commander · Firm". `<id>` is the brain's id,
lower-cased, every character other than a letter or a digit a `-`.

## Input

`input` carries `events`: `key_down` and `key_up` with the key's SDL
scancode and key code, `text`, `pointer_move`, `button_down` and
`button_up` with the button and the clicks, `wheel`, and `finger_down`,
`finger_move` and `finger_up`. Points are on the canvas (`space`
`game`, the default) or in the window's pixels (`window`).

- The events go onto SDL's event queue, and the game takes them in its
  next frame exactly as it takes a keyboard's, a mouse's or a touch
  screen's. The endpoint never calls the game's handlers, never gives
  orders and never types console lines on its own.
- The answer comes once the game has taken them: `consumed`, with the
  frame and tick.
- With `expect_screen`, the events are refused `screen_changed`, and none
  is pushed, when another screen shows by then, so that a click never
  lands on the screen that replaced the one it was meant for.
- Keys and buttons the events hold down count as held for the game's
  reads of what is held (the arrow keys that scroll the camera, Shift,
  Ctrl, a held button) until an event lets them go or the client leaves.
  The desktop's pointer is never moved.
- Text is typed only into a field that takes it, as SDL types it.

### Pressing a bound key

A script does not name an action. It sends the same device events a
keyboard, a mouse or a touch screen would, and the layout in use decides
which action that chord starts. Moving Stop off S means a script that
still sends S no longer stops: it sends the key the player bound.

`key_down` and `key_up` carry `scancode`, a whole number from 1 to 511,
the physical key. `keycode` is optional, a whole number from 0 to
4294967295, the character the key types, and 0 when it types none or when
the field is left out. `key` may name the key; the game does not require
it. A missing `scancode`, a scancode of 0 or 512 or more, or a keycode
below 0, is refused.

A chord is the modifiers' `key_down`, then the key's, then `key_up` in
the reverse order. The events stay held, as a real key would, until
`key_up` or the client leaves. `text` types into a field that takes it.
It does not press a command. `button_down`, `button_up` and `wheel` are
the mouse chords in the same layout (`MouseLeft`, `WheelUp` and the
rest).

```json
{"events":[
  {"kind":"key_down","scancode":4,"keycode":97},
  {"kind":"key_up","scancode":4,"keycode":97}
]}
```

Scancode 4 is the physical A key, and keycode 97 is the letter a. With
the default layout that press is Attack (`order.attack`) while playing,
because Attack is bound to A. A field that is taking text gets the letter
instead, as it would from the keyboard.

### Opening the key screens

The key editor, the keys panel and Settings › Controls and ›
Accessibility are ordinary controls. `controls` lists them, and a click
through `input` presses the name. There is no separate request for them.

| Screen | How a script opens it |
| --- | --- |
| The key editor | click `oa.settings.key-layout.edit` (Edit keys… on Controls), or `oa.keys-overlay.edit` (Edit keys… on the keys panel). The list opens on Common commands. A draft that already has a clash, or a chord taken from another command, opens on Clashes instead |
| The key editor, on Latchable | click `oa.settings.latching.choose` (Choose… beside Hold keys that latch) |
| Settings › Controls | click `oa.settings.nav.controls` |
| Settings › Accessibility | click `oa.settings.nav.accessibility` |
| The keys panel | it is `dialog` `oa.keys-overlay`, opened by the **Keys overlay** chord (Primary+/ by default) sent as `input`, or left as the player opened it |

The editor's controls are `oa.key-editor.` and the name in its module.
The ones a script uses most are already listed under
[The key editor](#the-key-editor): `oa.key-editor.search`,
`oa.key-editor.find`, `oa.key-editor.row.<id>`,
`oa.key-editor.detail.add`, `oa.key-editor.capture` and
`oa.key-editor.capture.ok`, `oa.key-editor.capture.means`,
`oa.key-editor.clash.replace`, `oa.key-editor.clash.swap`,
`oa.key-editor.clash.confirm`, `oa.key-editor.leave.save`,
`oa.key-editor.leave.discard`, `oa.key-editor.leave.keep-editing`,
`oa.key-editor.save`, and the tabs `oa.key-editor.view.list`,
`.keyboard` and `.mouse`. A press on `oa.key-editor.keyboard.<code word>`
only inspects that key.

The keys panel's controls are `oa.keys-overlay.list`,
`oa.keys-overlay.edit`, `oa.keys-overlay.close` and, while it scrolls,
`oa.keys-overlay.bar`.

Settings' key rows follow the same `oa.settings.<row>` rule as the other
rows: `oa.settings.key-layout.edit`, `oa.settings.key-layout.change`,
`oa.settings.key-layout-for-mod` (and `.same` or `.<id>` while open),
`oa.settings.show-keys-on-buttons` with `.off` and `.on`,
`oa.settings.sticky-modifiers`, `oa.settings.hold-to-confirm`,
`oa.settings.order-wheel`, `oa.settings.pointer-keys`,
`oa.settings.edge-scroll` and `oa.settings.move-overlays`.

## Events

After `subscribe`, the client is sent events unasked, among the answers:
`{"event", "seq", "frame", "tick", ...}`. The kinds are `screen`,
`player` (a battle-room seat taken or left), `ready`, `chat`, `loaded`,
`match` (started or ended), `pause`, `speed`, `alliance` and `game_over`.
A `match` line whose change is `ended`, and `game_over`, also carry
`results` and `tick`: how the game ended for each player, the same on
every machine. `game_over` keeps its own `outcome` and `winner`, which
are this machine's. The ended `match` line carries the shared `winner`.
The endpoint learns them by comparing what the game holds from one frame
to the next; nothing is added to the game to produce them.

The same events can be written as lines on standard output, or to a
file, with no connection to the endpoint.
See [what a game reports as it is played](multiplayer/status.md).

## What it never does

- **Change the game.** It reads the match and the battle room and changes
  neither. A match played with the endpoint on, and a client reading it at
  every frame, plays out tick for tick as it does without it.
- **Change what goes to other players.** Nothing the endpoint reads or is
  given is sent over a network game's connections, and the network
  protocol is the same with it on. The one difference the other players
  see is the engine's chat line in the battle room
  ([below](#in-a-battle-room)).
- **Run without `--fark`.** Without it no socket is opened and no
  endpoint file is written. Status lines are separate: `--status-stdout`
  and `--status-file` need no endpoint
  ([status.md](multiplayer/status.md)).
- **Listen beyond this machine.** It serves the loopback address only.
- **Read or write files, start programs or reach the network** for a
  client: it has no request that does.
- **Hold the game up.** It is served on the game's own thread at fixed
  points of each frame, takes requests for at most a millisecond each
  time, and never waits.
  A client that stops reading is dropped once 68 MiB of answers wait for
  it (`automation: client dropped: not reading` in the log).

## A map list's names

A map list's `items` are the words each row shows. A pack map's row shows the title its pack's index gives, or the title in its map file when the index has none. When that title would read the same as another row, the pack map's row adds its pack's name in brackets, as in `Comet Catcher (Cavedog: Comet Catcher)`. The game's own maps keep their file names. The same list gives `item_keys`, one key for each row, in the same order: the name the battle room, a saved game and GET MAP use, `<stem>@<id>` for a pack map and the file name for one of the game's own maps. `select_item` takes either the shown text or the key, matched exactly, the shown text first, so a request that names `isle_one@oa-test-isles` still chooses that map when the row reads its title. The Campaign list's rows are each campaign's name in the language shown, and its `item_keys` the campaigns' file names (`Arm Campaign`), which `select_item` takes too. Other lists omit `item_keys`.

## A list that does not fit

When a list on a multiplayer screen holds more rows than it can show, `controls` names the scroll arrows that move it. `scroll_up` is the arrow toward the start of the list and `scroll_down` is the arrow toward the end. A click on either, sent as input in the game's own space, moves the list by one page, the `rows` it shows at once. Select Map in the battle room is that list: `MAPNAMES`, with arrows `SLIDER.up` and `SLIDER.down`. A player clicking the same arrow still moves the knob by one step. A list whose rows all fit, and a list on any other screen, reports no arrows.

## In a battle room

Every player in a battle room is told when a game there can be driven: the
engine's line in the room's chat ends with ` REMOTED` while `--fark` is
on, as it ends with ` DEV MODE` while Developer Mode is:

```
[Engine: OpenAnnihilation v0.7.2 REMOTED]
[Engine: OpenAnnihilation v0.7.2 DEV MODE REMOTED]
```

## The welcome setup

The welcome at the first main menu is the screen `first-run`. A start
that still has to copy the game uses the same screen for five steps, and
`<step>` is then `language`, `game`, `copying`, `online` and `buttons`.
A player who already has the game uses `language`, `online` and `buttons`.
Controls are listed as `oa.first-run.<step>.<control>`:

| Control | Name |
|---|---|
| Quit on a first start, Skip on a re-run | `oa.first-run.language.skip`, `oa.first-run.game.skip`, `oa.first-run.copying.skip`, `oa.first-run.online.skip` |
| Back | `oa.first-run.language.back`, `oa.first-run.game.back`, `oa.first-run.online.back`, `oa.first-run.buttons.back` |
| Next | `oa.first-run.language.next`, `oa.first-run.online.next` |
| Copy the game | `oa.first-run.game.next` |
| Pause, or Continue once the check has finished. A clear check counts down about three seconds on Continue, then goes on; a key, a press, the wheel or a gamepad stops that | `oa.first-run.copying.next` |
| Locate another… | `oa.first-run.game.locate` |
| A source row | `oa.first-run.game.source.<index>`, from 0 |
| A language card | `oa.first-run.language.card.<tag>`, the tag in lower case and anything else a hyphen (`de`, `en-xa`) |
| The carousel's arrows | `oa.first-run.language.previous`, `oa.first-run.language.next-card` |
| Cancel download, Retry | `oa.first-run.language.cancel`, `oa.first-run.language.retry` |
| Turn on online features, Not now | `oa.first-run.online.on`, `oa.first-run.online.not-now` |
| Start playing | `oa.first-run.buttons.start` |

`--first-run` shows the welcome even when it is already done, and even in
a run that would otherwise hide it (a headless check, `--preferences-file`,
a benchmark, a stage run, `--fark`, an unattended run). Without it, those
runs do not show the welcome, and neither does a run whose preference
`open-annihilation.first-run` is `done`.

A stage file opens one step with a line of its own, and does not show the
welcome otherwise:

```
first-run language
first-run game
first-run copying
first-run online
first-run buttons
```

The log answers `stage: first-run language` (or `game`, `copying`,
`online`, or `buttons`).
On its own the welcome logs `first-run: showing step language` when that
step opens, and `first-run: not showing:` with `done`, `headless check`,
`preferences file`, `benchmark`, `stage`, `fark` or `unattended` when it
stays hidden.

Settings › System › Welcome setup is `oa.settings.first-run`. Show again
waits for OK; during a game the row is locked.

## Through MCP

[tools/mcp/oa_mcp_server.py](../tools/mcp/README.md) is an MCP server that
starts the game with `--fark`, or attaches to one started so, and offers
the endpoint's requests to an MCP client as tools: the screen and its
controls, a click on a control by its name, a list's row by its text, keys,
typed text, the pointer and the wheel, a picture of the frame, the
preferences, the match, the battle room and the events. It is registered
with the client by hand; the repository holds no project file that starts
it.

Unlike the endpoint, the server starts a program: Open Annihilation's own
executable alone, with the command-line options it allows and a new
preferences file in a scratch folder of its own unless told otherwise. It
reads the endpoint file and the game's log, writes only that scratch
folder, and connects only to the loopback address. The game it drives does
what its input does, as a player's would: it can change and save the
settings, install and fetch packages, write to the player's folders, and
in a battle room send chat that the other players see.
[tools/mcp](../tools/mcp/README.md#what-it-reads-writes-and-starts) says
it all.

## Checks

The engine's tests of the endpoint are `automation-protocol`,
`automation-options`, `automation-input`, `automation-reports` and the
native checks `native-automation-menus`, `native-automation-input`,
`native-automation-frame`, `native-automation-settings`,
`native-automation-hit-areas` and `native-automation-digest`, which run
the game with `--fark` on the dummy drivers over an installed game.
`mcp-server-selftest` and `native-mcp-server` check the MCP server
([below](#through-mcp)).
[testing.md](development/testing.md#the-automation-endpoint) says how to run them,
and [src/app/automation](../src/app/automation/README.md#tests) what each
checks.
