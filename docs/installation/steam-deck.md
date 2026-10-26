# Installing on the Steam Deck

Open Annihilation runs natively on the Steam Deck from the Steam Deck
package, which holds the Linux 64-bit PC (x86_64) game. No game data is
included: you also need the game data from your own copy of Total
Annihilation. This guide adds the game to Steam as a non-Steam game, with
its own controller layout and artwork, so that it starts from the library
in Game Mode like any other game.

**Steam Deck support is experimental.** It has been tried on one Steam
Deck so far, where Steam's own **Add a Non-Steam Game** added it. Other
Decks, SteamOS versions and Steam settings have not been checked, much of
the gamepad scheme has still to be tried on a Deck (see
[What needs a Deck](../controllers.md#what-needs-a-deck)), and this guide
has no screenshots yet. If something goes wrong, or works where this guide
says it may not, please tell us on [Discord](../../README.md#community) or
in an [issue on GitHub](https://github.com/open-annihilation/open-annihilation/issues),
with the game's log (see [Logs](#logs-and-reporting-a-problem)).

## 1. What you need

- A Steam Deck, the LCD or the OLED model.
- **Total Annihilation with the 3.1 update**, from Steam, from GOG or copied
  from a Windows PC (step 3), or the free 1997 demo.
- A few minutes in **Desktop Mode**, where the game is unzipped and added
  to Steam. **STEAM** and **X** together open the on-screen keyboard
  wherever you need to type, and in Desktop Mode **L2**, or a press of the
  left trackpad, is the right mouse button.

## 2. Download and unzip it (Desktop Mode)

1. Switch to **Desktop Mode**: press the **STEAM** button, choose **Power**,
   then **Switch to Desktop**.
2. Open the latest release on GitHub in the browser,
   [github.com/open-annihilation/open-annihilation/releases/latest](https://github.com/open-annihilation/open-annihilation/releases/latest),
   and download `open-annihilation-<version>-steam-deck.zip` under
   **Assets**: the Steam Deck package, the Linux 64-bit PC game with the
   `steam-deck` folder this guide uses. The Linux 64-bit PC package runs on
   a Deck too but lacks that folder, and the other Linux packages are for
   ARM computers and do not run on a Deck.
3. In **Dolphin**, the file manager, make a folder called `Applications`
   in your home folder, move the zip there, right-click it and choose
   **Extract**, then **Extract archive here**. This makes the folder
   `open-annihilation-<version>-steam-deck`, which holds the game,
   `open-annihilation`, and the `steam-deck` folder with the game's
   controller layouts and artwork for Steam.

Keep the folder where it is. Steam's shortcut points at `open-annihilation`
in it, and that file keeps starting the game after an update.

## 3. Get the game data

Open Annihilation needs the folder of an installed Total Annihilation with
the 3.1 update: the folder that holds `totala1.hpi`. On a Deck it usually
finds that folder by itself at its first start (step 7).

### From Steam

Install **Total Annihilation** (app 298030, with Core Contingency and
Battle Tactics) from your library as usual, on the Deck's own storage or on
an SD card. Open Annihilation finds it in any of your Steam libraries and
plays the files Steam installed: you never need to start Total Annihilation
itself or run it through Proton.

### From GOG, with Heroic or Lutris

Install GOG's **Total Annihilation: Commander Pack** with **Heroic Games
Launcher** or **Lutris**, both in the Discover store in Desktop Mode.
Open Annihilation finds a copy Heroic installed, and one in a Lutris or Wine
prefix in `~/Games` or in a Bottles bottle.

### From a Windows PC

Copy the whole folder of an installed Total Annihilation, the one that
holds `totala1.hpi`, to the Deck, for example with a USB stick or an SD
card, into `~/Games/Total Annihilation`. A copy from the original CDs must
be installed on Windows and updated to 3.1 first. The game does not look
for a folder copied like this: choose it at the first start (step 7).

### The free demo

To try Open Annihilation without the full game, see
[Playing the demo](../../README.md#playing-the-demo).

## 4. Add it to Steam

Still in Desktop Mode, add `open-annihilation` to Steam in one of two ways:

- **In Steam:** choose **Games**, then **Add a Non-Steam Game to My
  Library…**, then **Browse…**. Go to the game's folder and choose
  `open-annihilation`; if it is not listed, set **File type** to **All
  Files** first. Then choose **Add Selected Programs**.
- **In Dolphin:** right-click `open-annihilation` and choose **Add to
  Steam**.

Then set its properties. In Steam's library, right-click the new entry,
named after the file (`open-annihilation`), and choose **Properties**:

- Rename it **Open Annihilation**. Steam shares community layouts by this
  name (see [For the maintainer](#for-the-maintainer-publishing-the-community-layout)).
- Leave **Launch Options** empty.
- Under **Compatibility**, leave **Force the use of a specific Steam Play
  compatibility tool** off. Open Annihilation is a native Linux program,
  and Proton is for Windows programs.

## 5. Install the controller layouts

The `steam-deck` folder holds Open Annihilation's two Steam Input
templates, `open-annihilation.vdf` and
`open-annihilation-keyboard-mouse.vdf`. Steam lists the templates it finds
in its `controller_base/templates` folder. Open **Konsole**, the terminal,
from the application launcher (under **System**), and copy both files
there:

```sh
cd ~/Applications/open-annihilation-<version>-steam-deck
mkdir -p ~/.steam/steam/controller_base/templates
cp steam-deck/*.vdf ~/.steam/steam/controller_base/templates/
```

Steam reads the templates when it starts. Switching back to Game Mode in
step 7 restarts it.

## 6. Add the artwork (optional)

The `steam-deck/artwork` folder holds Steam's library pictures for the
game, made from the Open Annihilation icon; none of them uses Total
Annihilation's art. Without them Steam shows the game with a plain tile. In
Steam, still in Desktop Mode:

- **The library's picture:** right-click Open Annihilation in the library,
  choose **Manage**, then **Set custom artwork**, and choose
  `portrait.png`.
- **The banner and the logo:** on the game's page, right-click the banner
  at the top and choose **Set Custom Background** with `hero.png`, then
  **Set Custom Logo** with `logo.png`.
- **The icon:** in the game's **Properties**, click the icon beside its
  name and choose `icon.png`.

## 7. Start the game in Game Mode

1. Double-click **Return to Gaming Mode** on the desktop.
2. In the library, under **Non-Steam**, select Open Annihilation and open
   its **controller settings** (the controller icon on its page). Choose the
   current layout, then **Templates**, and pick **Open Annihilation**.
3. Choose **Play**.

A later start opens as you left it. In Game Mode the first start is still
the screen's size. A window you leave is kept only as far as it fits that
screen.

At its first start the game looks for Total Annihilation where Steam,
Heroic, Lutris and Bottles put it: Steam's copy in any of your Steam
libraries, SD cards included, a Heroic install, a Lutris or Wine prefix in
`~/Games`, or a Bottles bottle.

- **When it finds exactly one usable copy**, it plays it without asking and
  says on the main menu where it found it; it remembers it as if you had
  chosen it.
- **When it finds several**, or none, or the folder you chose before has
  gone, it asks in its own locator. The title says to choose a folder or an
  installer, and a line says what qualifies. Places include Home, Downloads
  and a microSD card, and the games it knows from Steam, Heroic, Lutris and
  Bottles. Each file or folder is one row: an icon, a name and a size,
  folders first. Typing filters the names, and Y does the same. The D-pad
  moves, A opens, X chooses the folder and B goes back. A trackpad, a finger
  or the keys do the same. Game Mode does not open the desktop's folder
  dialog. It remembers your choice.

  ![The locator on a Steam Deck](../images/folder-chooser/deck.png)
- **To name the folder yourself**, give it as a launch option: in the
  game's **Properties**, **Launch Options**, type for example
  `--game-dir "/home/deck/Games/Total Annihilation"`. A folder named this
  way, or one you chose before, always wins over the copies the game finds.
  `--choose-game-dir` as the launch option makes it ask at every start
  until you remove it again; the two cannot be given together.

Every launch option the game takes, among them `-c` for a launch file that
opens a network game's battle room, is in
[Command Line and Game Launching](../command-line.md).

## Controls

### Steam Input on, with Open Annihilation's layout (start here)

Steam Input is on for every game unless you turn it off, and this is the
way to start: it needs no setting changed but the template of step 7.

With this layout the sticks and buttons reach the game as a gamepad's, the
right trackpad is the mouse (its press is the left button), the left
trackpad pans like the left stick, and the four back grips work as
Open Annihilation's: **R4 QUEUE** (x5 on a build button), **L4 ADD**,
**R5 FORCE** and **L5 groups**. In short:

| Input | Does |
|---|---|
| Right trackpad | The pointer; press for a left click |
| R2, L2 | The left and right mouse buttons |
| R1 | The order ring at the pointer: release to give the order there, A to arm it |
| L1 | The build ring of the selected builder |
| Left stick, left trackpad | Scroll the map; L3 centres |
| Right stick | Zoom; a flick left or right turns the build page; R3 follows |
| A, B, X, Y | Click, clear, stop, every unit of the type |
| D-pad | Commander, next unit, next report, SELECT ▾ |
| View, Menu | Unit info (hold: game keys such as pause and chat), the in-game menu |

[controllers.md](../controllers.md) describes every input, the rings, the
groups and the settings. Everything there works on this route except the
left trackpad as a minimap, the rings aimed by the right trackpad, the
group ring, the gyro and the trackpads' own ticks, which need Steam Input
off.

The other template, **Open Annihilation: keyboard and mouse**, sends only
the mouse and keys (Shift, Ctrl, the group keys, radial menus of the order
and selection keys), for players who prefer Steam's mouse and keys to the
game's gamepad controls.

If the templates are not listed, check that both files are in
`~/.steam/steam/controller_base/templates` and restart Steam. If they are
still not listed, or if a grip does nothing, choose Steam's own **Gamepad
with Mouse Trackpad** template and, in its settings, bind R4 to F13, R5 to
F14, L4 to F15 and L5 to F16. Without the grips the game plays its fallback
map: tap R1 to latch QUEUE, tap L1 to latch ADD, hold View for the groups
([The fallback map](../controllers.md#the-fallback-map)).

A community layout named "Open Annihilation" will also appear under
**Community Layouts** once it is published (see
[For the maintainer](#for-the-maintainer-publishing-the-community-layout)).

### Menus and movies

On the 640×480 menus, the main menu among them, **A** and **Menu** press
the focused button. No button is focused when the main menu opens: the
first A focuses its first button, and the next A presses it. The D-pad
moves the focus, **B** goes back as Escape does, and **R2** or a press of
the right trackpad clicks at the pointer, as a mouse does.
[Menus](../controllers.md#menus) lists every input outside a match.

Movies, the intro among them, skip with **A**, **B** or **Menu**, a tap on
the screen or a click.

### Steam Input off (the whole scheme)

With Steam Input off for Open Annihilation, the game reads the controller
itself: both trackpads as trackpads (the left one a minimap under the thumb
when pressed and held), the four grips, the stick touches, the gyro and a
light tick on each trackpad. To turn it off, open the game's controller
settings, or its properties' **Controller** page, and choose **Disable
Steam Input** for Open Annihilation.

Players report the **STEAM** and **···** buttons behaving oddly in some
games with Steam Input off, and this has not been checked with Open
Annihilation on a Deck yet; if they misbehave, turn Steam Input back on.
The game says in its Controller settings when Steam Input is on.

## Desktop Mode

Open Annihilation plays in Desktop Mode too, but start it from Steam's
library there as well, not from Dolphin. Steam gives a game its controller
layout only when Steam starts it: started from Dolphin, the game gets the
controls as Steam's desktop layout sends them, the right trackpad as the
mouse and the buttons as keys, without Open Annihilation's template.

In Desktop Mode the window opens as on any Linux desktop, and when the game
finds no copy of Total Annihilation, the desktop's folder dialog asks for
the folder instead of the game's own chooser.

Once the game has run, a `.oamod` file opened in Dolphin installs the mod
it holds ([mods](../mods/README.md#installing-a-oamod-file)). Opened while
the game runs, the file goes to the running game.

## Frame rate, Control size and the window

On a Steam Deck the game starts with **Maximum frame rate** at the
screen's rate, 60 frames a second on the LCD model and 90 on the OLED, and
with **Control size** at **Larger**, so the touch controls are near the
size they have on an iPad. Both are ordinary settings: change them in
Open Annihilation's settings (**SETTINGS…** at the foot of the Library,
which the **OA** button at the bottom right of the main menu opens):
Maximum frame rate under **Graphics**, Control size under
**Touch** or **Controller**. A value you set is kept, and **Restore
defaults** brings the Deck's starting values back. Steam's own frame limit
and power settings in the Quick Access menu (···) stay yours and work as
for any game.

In Game Mode the window opens at the screen's size, 1280×800 on the Deck's
own screen, unless `--resolution` or the **Screen size** setting names
another. A later start opens as you left it, and a remembered window is
kept only as far as it fits the screen. Steam shows a larger window shrunk
to fit, but its pointer stops short of that window's lower and right
edges, so leave Screen size at **Desktop**. The log says the size the
window opened at (see [Logs](#logs-and-reporting-a-problem)).

## The touch screen

The game's touch controls ([touch-controls.md](../touch-controls.md)) need
the touches themselves. In the game's controller settings, leave the
touch screen on native touch, not the setting where Steam turns touches
into mouse clicks: with that one a tap clicks, but every gesture is lost.
The touch controls appear when a finger first touches the screen, and the
gamepad and the touch screen work together: one set of QUEUE and ADD
latches, and the gamepad's pointer takes the hover back when the right
trackpad moves.

## The on-screen keyboard

In Game Mode, Steam's keyboard opens by itself when the game wants text:
the chat line, a saved game's name, the player's name. The game places it
clear of the field being typed in. Enter on the keyboard sends; B cancels
the line. **STEAM** and **X** together open the keyboard by hand. In
Desktop Mode with Steam closed, the desktop's own on-screen keyboard
serves, and the touch screen and a USB keyboard always work.

Steam's keyboard sends finished text: with its Chinese layout, for
example, the characters are chosen in the keyboard and go into the field
once chosen ([typing.md](../typing.md#steam-deck)).

## Proton

The Windows package also runs through Proton, but that is not the supported
way: the game folder shows as a `Z:` drive, the folder dialog is Wine's,
the controller reaches the game through Wine, and Proton's own problems
become the game's. Use the Steam Deck package, with no compatibility tool
forced in its properties.

## Troubleshooting

- **The game does not start, or Dolphin opens the game as a file.** The
  unzip tool lost the permission to run it. In Konsole, in the game's
  folder, run `chmod +x open-annihilation` once.
- **Steam does not start the game.** In the game's **Properties**, check
  that **Target** names `open-annihilation` in the game's folder, and that
  under **Compatibility** no Steam Play compatibility tool is forced.
- **The templates are not listed.** Check that both `.vdf` files are in
  `~/.steam/steam/controller_base/templates`, then restart Steam, for
  example by switching to Desktop Mode and back.
- **The game asks for Total Annihilation although it is installed.** The
  folder must hold `totala1.hpi`. Choose it in the game's folder chooser,
  or name it with `--game-dir` as in step 7.
- **The pointer does not reach the right or lower edge in Game Mode.** Set
  **Screen size** under **Graphics** back to **Desktop**, remove any
  `--resolution` from the launch options, and start the game again.
- **The controls do nothing, or act as a mouse and keys only.** Start the
  game from Steam's library, not from Dolphin, and check that the game's
  controller settings use the **Open Annihilation** template. The log says
  whether a gamepad reached the game (below).

## Logs and reporting a problem

The game's logs are in `~/.local/share/open-annihilation/logs`, in files
named `open-annihilation-YYYYMMDD-HHMMSS.log` after the date and time, in
UTC, that each was begun; short runs on the same day share one file. In
Dolphin, `.local` is a hidden folder: show hidden files with **Ctrl+H**.

Two lines in the log say most of what is needed to look into a problem on a
Deck:

- `open-annihilation: window: ...` gives the size the window opened at,
  the desktop's size and whether the game saw Steam's Game Mode, for example
  `open-annihilation: window: 1280x800 on a 1280x800 desktop, in Steam's Game Mode`;
- `open-annihilation: gamepad opened: ...` names each gamepad the game
  opened, with its vendor and product numbers, and ends with "through Steam
  Input" when Steam Input gives it. With no such line, no gamepad reached
  the game.

When you report a problem, send the newest log file, or at least those two
lines, and say which Deck you have (LCD or OLED), which template the game
uses or whether Steam Input is off, and what happened.

## Where it keeps its files

The Deck keeps the game's files where any Linux computer does
([Where it keeps its files](linux.md#where-it-keeps-its-files)): your saved
games, screenshots, films, recordings and mods in the **Open Annihilation**
folder in Documents, your settings in `~/.config/open-annihilation`, and the logs and
the unpacked demo in `~/.local/share/open-annihilation`.

## Updating and removing

- **To update:** from 0.8.7 on, Open Annihilation updates itself. On a
  Deck the new version installs the next time you start the game, and the
  shortcut you added keeps working. An install of 0.8.6 or older is moved
  to 0.8.7 once, by unzipping the new package and pointing the shortcut at
  it as in step 4; after that the game updates itself. Your settings, saved
  games and the Total Annihilation folder you chose are kept.
- **To remove:** in Steam, right-click Open Annihilation, choose
  **Manage**, then **Remove non-Steam game from your library**, and delete
  the game's folder. To remove the templates too, delete
  `open-annihilation.vdf` and `open-annihilation-keyboard-mouse.vdf` from
  `~/.steam/steam/controller_base/templates`. Your settings, logs, saved
  games, screenshots, films, recordings and mods are in the folders above.

## For the maintainer: publishing the community layout

Steam shares layouts for a non-Steam game by the shortcut's name, so a
layout published for the shortcut **Open Annihilation** (the name step 4
gives it) appears under **Community Layouts** for every player whose
shortcut has that name, with no file to install and no release needed to
update it. To publish it from a Deck:

1. Add the game to Steam as in step 4, so the shortcut is named exactly
   "Open Annihilation", and install the templates as in step 5.
2. In Game Mode, open its controller settings, pick the **Open
   Annihilation** template, and check every binding on the device (the
   grips must send F13 to F16).
3. In the controller settings' menu, choose **Export Layout**, then a new
   public (community) layout, titled "Open Annihilation", with the
   template's description.
4. On a second account or Deck, add the game the same way and check that
   the layout appears under **Community Layouts**.

Publish an updated layout the same way whenever the template in the
Steam Deck package's `steam-deck` folder changes.
