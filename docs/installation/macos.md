# Installing on macOS

Open Annihilation runs on macOS 11 (Big Sur) or later, on Intel and Apple
silicon Macs alike. No game data is included: you also need the game data
from your own copy of Total Annihilation, and step 3 shows how to get it on
a Mac.

## 1. Download Open Annihilation

1. Open the latest release on GitHub:
   [github.com/open-annihilation/open-annihilation/releases/latest](https://github.com/open-annihilation/open-annihilation/releases/latest).
2. Under **Assets**, download
   `open-annihilation-<version>-macos-universal.pkg`, the installer. The
   `open-annihilation-<version>-macos-universal.zip` beside it holds the same
   app without an installer.

## 2. Install it

- **With the installer:** double-click the `.pkg` and follow its steps. It
  puts **Open Annihilation.app** in your Applications folder.
- **With the zip:** double-click the `.zip` to unpack it, then drag
  **Open Annihilation.app** from the folder it makes into Applications.

The app and the installer are signed with a Developer ID and notarized by
Apple, so macOS opens them without a warning.

## 3. Get the game data

Open Annihilation needs the folder of an installed Total Annihilation with
the 3.1 update: the folder that holds `totala1.hpi`. Choose one of the ways
below.

### From GOG's Mac version

GOG sells **Total Annihilation: Commander Pack** with a macOS version, which
holds the game's folder inside its app.

1. Install GOG's Mac version of Total Annihilation, with GOG Galaxy or with
   GOG's installer.
2. In Finder, find the game's icon (in Applications), right-click it and
   choose **Show Package Contents**.
3. Open the **drive_c** folder, then **Program Files**, then **GOG.com**.
4. Copy the **Total Annihilation** folder somewhere convenient, for example
   your **Documents** folder. Hold **Option** while you drag it, so that
   Finder copies it and GOG's version keeps working.

### From a Windows PC

Copy the whole folder of an installed Total Annihilation, the one that
holds `totala1.hpi`, from the PC to the Mac, for example with a USB stick,
into your **Documents** folder. A copy from the original CDs must be
installed on Windows and updated to 3.1 first.

### The free demo

To try Open Annihilation without the full game, use the 1997 demo instead:
see [Playing the demo](../../README.md#playing-the-demo).

## 4. Start the game

1. Open **Open Annihilation** from Applications or Launchpad.
2. The first time, it asks for your Total Annihilation folder. Choose the
   folder from step 3. It remembers your choice.

On a Retina display the setup, Settings and the menus are laid out from
the window's size in points and drawn at the display's pixel density. The
same window is the same arrangement, with sharper edges. The setup also
grows with the window: past 960 by 540 points it draws the same
arrangement larger, so a 1920 point wide window shows its words and
buttons twice their size. In a window, the setup's top bar leaves out the
Open Annihilation icon and name, which the window's title bar already
shows. Full screen keeps them.

The next time you open it, it comes back as you left it: full screen, or
the window at the same size and place, on the same display when that
display is still connected. A window is kept on the screen. The first
start opens a window.

To choose a different folder later, start it once from Terminal with
`--choose-game-dir`:

```sh
open -a "Open Annihilation" --args --choose-game-dir
```

Every option the game takes, and every way to start a network game from
Terminal or a launcher, is in
[Command Line and Game Launching](../command-line.md).

Open Annihilation's own settings open with **Cmd+,**, or from
**SETTINGS…** at the foot of the Library, which the **OA** button at the
bottom right of the main menu opens. See [Settings](../settings.md), and
[the Library](../library.md) for mods, maps and languages to download.

The app declares `.oamod`, `.oalang`, `.oamap`, `.oamut`, `.oabrain`, `.oareg` and
`.oalaunch` files.
Double-click a `.oamod` file to install the mod it holds in Open
Annihilation ([mods](../mods/README.md#installing-a-oamod-file)) while
Open Annihilation is the program Finder uses for that type, and a
`.oalaunch` file to start the network game it describes
([opening a launch file](../command-line.md#opening-a-launch-file)).
[Settings › System](../settings.md#system) lists a switch for each type
under **File Type Registration**. On makes this copy the program Finder
opens that type with. Off hands it back to the program that opens a zip
archive (the five packages, usually Archive Utility) or plain text (a
registry file or a launch file). The type and its icon stay declared by the app; only the
program that opens the file changes. The switch shows the choice Finder
has now. It changes when you move it and press **OK**, and not before. A
choice you made in Finder's Get Info › Open with stays until then, and
starting the game does not put a type back. If macOS refuses the change,
the game logs it and the switch shows the choice macOS kept.

With the macOS firewall on, the first time you host or join a multiplayer
game macOS asks whether Open Annihilation may accept incoming network
connections: click **Allow**. A firewall between the players must let
through UDP port 47624, which other computers use to find a hosted game,
TCP ports 2300 to 2400 and UDP ports 2350 to 2400.

## Where it keeps its files

Your saved games, screenshots, films, recordings and mods are in the
**Open Annihilation** folder in your Documents folder (`~/Documents/Open
Annihilation`):

| Folder | What it holds |
| --- | --- |
| `Saves` | saved games, a mod's in `Saves/<mod id>` and those without a mod in `Saves/default` |
| `Screenshots` | screenshots (Ctrl+F9) and posters (`MakePoster`), in a folder for each mod as in `Saves` |
| `Films` | films (Ctrl+F10), a `MOVIEnnn` folder each, in a folder for each mod as in `Saves` |
| `Recordings` | recordings of network games, which `--play-demo` plays back, in a folder for each mod as in `Saves` |
| `Mods` | mods you add, each in a folder of its own, which the Library lists besides the game folder's `mods` folder ([mods](../mods/README.md#installing-a-mod)) |

The game makes each folder the first time it needs it. In the settings
(**Cmd+,**), **System** shows the folder under
**Your files**, whose buttons open Saves, Screenshots and Mods in Finder.

**macOS asks first.** On macOS 10.15 and later, the first time the game
looks in Documents, which is at its first start, macOS asks whether "Open
Annihilation" may access files in your Documents folder. Click **Allow**
(**OK** on older versions). If you click **Don't Allow**, the game can
neither save games and screenshots there nor list your mods; to change your
mind, turn Open Annihilation on under System Settings › Privacy & Security ›
Files and Folders, or put the folder elsewhere (below). Started from
Terminal, as `run.sh` does, the game has the access Terminal has, and macOS
asks on Terminal's behalf.

Your settings, the log files (in `logs`) and the unpacked demo stay in
`~/Library/Application Support/net.coreprime.open-annihilation`.

**Saved games from earlier versions.** Versions before 0.7 kept saved games
in a `SAVEGAME` folder in `~/Library/Application
Support/net.coreprime.open-annihilation`, and a mod's in `mods/<mod
id>/SAVEGAME` there; 0.7.0 kept those without a mod in `Saves` itself. The
first start of a later version moves them into `Saves/default`, and a mod's
into `Saves/<mod id>`, once, and the main menu then says how many moved and
where, with a button that opens the folder. Nothing is overwritten: a saved
game whose name that folder holds already is moved as `NAME (2).SAV`,
keeping both. One
that cannot be moved stays where it is, and the game still lists it there.
The log says what moved and what did not. An earlier version started
afterwards no longer sees the saved games that moved. Recordings, which
earlier versions kept in a `demos` folder there, and a mod's in `mods/<mod
id>/demos`, move into `Recordings/default` and `Recordings/<mod id>` in the
same way, once, without a message on the main menu.

**Putting the folder elsewhere.** Start the game with `--user-folder PATH`,
or add the key `open-annihilation.user-folder` with an absolute path to the
preferences file, and that folder takes the place of the Open Annihilation
folder in Documents. Saved games and recordings from earlier versions move
only into the folder the key or Documents names: a start with
`--user-folder` leaves them where they are and lists the saved games there.
With `--preferences-file`, the folder is the **Open Annihilation** folder
beside that file instead, and saved games and recordings beside that file
are not moved either: the game lists the saved games where they are. An Image Output Directory set in the game, with the console's `Film`
command, still takes the screenshots and films.

## Updating and removing

- **To update:** download the newer `.pkg` and install it over the old one,
  or replace the app with the newer one. Your settings are kept.
- **To remove:** drag **Open Annihilation.app** to the Trash. To remove your
  settings and logs too, delete the Application Support folder above; your
  saved games, screenshots, films, recordings and mods are in the Open
  Annihilation folder in Documents.
