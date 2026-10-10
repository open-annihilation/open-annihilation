# Installing on Linux and Raspberry Pi

Open Annihilation has four Linux packages: one for 64-bit PCs, one for the
Steam Deck, and two for ARM computers such as the Raspberry Pi. No game
data is included: you also need the game data from your own copy of Total
Annihilation. On a Steam Deck, see [Steam Deck](#steam-deck) below.

## 1. Choose your package

| Your computer | Package | Needs |
|---|---|---|
| 64-bit PC | `open-annihilation-<version>-linux-x86_64.zip` | glibc 2.28 or later: Debian 10, Ubuntu 20.04, Fedora 29, RHEL 8, SteamOS 3 or newer |
| Steam Deck | `open-annihilation-<version>-steam-deck.zip`, the 64-bit PC package with the Deck's files for Steam; follow [its own guide](steam-deck.md) | SteamOS 3 or newer |
| 64-bit ARM, including 64-bit Raspberry Pi OS | `open-annihilation-<version>-linux-arm64.zip` | glibc 2.28 or later |
| 32-bit ARMv7, including 32-bit Raspberry Pi OS | `open-annihilation-<version>-linux-armhf.zip` | glibc 2.36 or later: Debian 12, Raspberry Pi OS Bookworm or newer |

To see which you need, run these in a terminal:

```sh
uname -m          # x86_64, aarch64 (64-bit ARM) or armv7l (32-bit ARM)
ldd --version     # the first line ends with the glibc version
```

On a Raspberry Pi, `dpkg --print-architecture` prints `arm64` for the
64-bit system and `armhf` for the 32-bit one.

The game runs on an X11 or Wayland desktop, or from the text console with
no desktop. For sound it uses PipeWire, PulseAudio or ALSA, whichever your
system has.

## 2. Download it

Open the latest release on GitHub,
[github.com/open-annihilation/open-annihilation/releases/latest](https://github.com/open-annihilation/open-annihilation/releases/latest),
and download your package under **Assets**. Or download it in a terminal,
with the release's version in place of `v0.7.4`:

```sh
curl -LO https://github.com/open-annihilation/open-annihilation/releases/download/v0.7.4/open-annihilation-v0.7.4-linux-x86_64.zip
```

## 3. Unzip it

```sh
unzip open-annihilation-<version>-linux-x86_64.zip
```

This makes the folder `open-annihilation-<version>-linux-x86_64`, which
holds the game, `open-annihilation`. If `unzip` is missing, install it, for
example with `sudo apt install unzip`.

## 4. Get the game data

Open Annihilation needs the folder of an installed Total Annihilation with
the 3.1 update: the folder that holds `totala1.hpi`. The examples keep it in
`~/Games/Total Annihilation`; any folder works.

### From a Windows PC

Copy the whole folder of an installed Total Annihilation, the one that
holds `totala1.hpi`, for example with a USB stick:

```sh
mkdir -p ~/Games
cp -r "/media/$USER/<stick>/Total Annihilation" ~/Games/"Total Annihilation"
```

A copy from the original CDs must be installed on Windows and updated to
3.1 first.

### From GOG's Mac version

GOG sells **Total Annihilation: Commander Pack** for Windows and macOS, not
for Linux. If you have GOG's Mac version on a Mac, copy the game's folder
from it: right-click the game's icon in Finder, choose **Show Package
Contents**, open **drive_c**, **Program Files**, then **GOG.com**, and copy
the **Total Annihilation** folder to the Linux computer as above.

### From Steam, Heroic or Lutris

Total Annihilation bought on Steam (app 298030) and installed there, on any
of your Steam libraries, or GOG's installed with Heroic Games Launcher, or
installed in a Lutris, Bottles or Wine prefix under `~/Games`, can be played
where it is: the game finds it at its first start (step 5). Steam's copy
never needs to be started or run through Proton.

### The free demo

To try Open Annihilation without the full game, see
[Playing the demo](../../README.md#playing-the-demo).

## 5. Start the game

From the game's folder:

```sh
cd open-annihilation-<version>-linux-x86_64
./open-annihilation
```

- **On a desktop:** the first time, it looks for Total Annihilation where
  Steam, Heroic and Lutris put it. When it finds exactly one usable copy, it
  plays it without asking and says on the main menu where it found it.
  Otherwise it asks for your Total Annihilation folder, with your desktop's
  file chooser, or with its own folder chooser when it found several copies
  or the file chooser cannot open. Choose the folder from step 4. It
  remembers your choice. To choose another folder later, start it with
  `--choose-game-dir`; a folder named with `--game-dir`, or one you chose
  before, always wins over the copies it finds.
- **From the text console** (for example Raspberry Pi OS Lite, or a Pi with
  the desktop switched off): there is no folder dialog, so name the folder
  each time:

  ```sh
  ./open-annihilation --game-dir ~/Games/"Total Annihilation"
  ```

  The game draws straight to the screen.

If the shell says `Permission denied`, the unzip tool lost the program's
permission: run `chmod +x open-annihilation` once.

Open Annihilation's own settings open from the **OA** button at the bottom
right of the main menu, or with **Ctrl+,**. See
[Settings](../settings.md).

Each start on a desktop makes this copy of the game the opener of `.oamod`
files for your account, with the game's icon on them: open one from the
file manager to install the mod it holds
([mods](../mods/README.md#installing-a-oamod-file)). Opened while the game
runs, the file goes to the running game.

For multiplayer, a firewall on the computer must let through UDP port
47624, which other computers use to find a hosted game, TCP ports 2300 to
2400 and UDP ports 2350 to 2400.

## Raspberry Pi

Open Annihilation runs on a Raspberry Pi 4, Pi 400 or Pi 5 with the 64-bit
Raspberry Pi OS (Bookworm or later), with the `linux-arm64` package. A
Pi 2, Pi 3 or Pi 4 with the 32-bit Raspberry Pi OS (Bookworm or later)
needs the `linux-armhf` package. The steps are the ones above: copy the game
data to the Pi, unzip the package and start the game.

On a Raspberry Pi the game starts at 60 frames a second, without enhanced
anti-aliasing, which the Pi's graphics keep up with. Both can be changed in
the settings like on any other computer.

## Steam Deck

The Steam Deck has a package of its own,
`open-annihilation-<version>-steam-deck.zip`: the 64-bit PC package with
one folder more, `steam-deck`, which holds the game's Steam Input layouts
and library artwork. The 64-bit PC package runs on a Deck too, without that
folder. Steam Deck support is experimental. Its own guide,
[Installing on the Steam Deck](steam-deck.md), walks through it: unzipping
the Steam Deck package in Desktop Mode, adding the game to Steam as a
non-Steam game, installing the Steam Input layouts and the artwork from the
`steam-deck` folder by hand, the controls, the first start and the Deck's
settings.

Steam's own **Add a Non-Steam Game** adds the game to Steam on any Linux
computer with Steam in the same way, and the layouts from the Steam Deck
package install there as the guide describes. A gamepad works on every
Linux computer, with or without Steam; [controllers.md](../controllers.md)
describes the gamepad controls.

## Where it keeps its files

Your saved games, screenshots, films, recordings and mods are in the
**Open Annihilation** folder in your Documents folder: the folder
`XDG_DOCUMENTS_DIR` names in `~/.config/user-dirs.dirs` (in
`$XDG_CONFIG_HOME` when that is set), as your desktop shows it, else
`~/Documents`:

| Folder | What it holds |
| --- | --- |
| `Saves` | saved games, a mod's in `Saves/<mod id>` and those without a mod in `Saves/default` |
| `Screenshots` | screenshots (Ctrl+F9) and posters (`MakePoster`), in a folder for each mod as in `Saves` |
| `Films` | films (Ctrl+F10), a `MOVIEnnn` folder each, in a folder for each mod as in `Saves` |
| `Recordings` | recordings of network games, which `--play-demo` plays back, in a folder for each mod as in `Saves` |
| `Mods` | mods you add, each in a folder of its own, which the settings' Mods page lists besides the game folder's `mods` folder ([mods](../mods/README.md#installing-a-mod)) |

The game makes each folder the first time it needs it. In the settings (the
**OA** button on the main menu), **Common Tweaks** shows the folder under
**Your files**, whose buttons open Saves, Screenshots and Mods in your file
manager.

The buttons open a folder with `xdg-open`, from the `xdg-utils` package.
Without it they ask the desktop portal through `gdbus`; when neither is
there, the row says no file manager is there to open the folder, and the log
says what to install.

Your settings stay in `~/.config/open-annihilation`, or in
`$XDG_CONFIG_HOME/open-annihilation` when `XDG_CONFIG_HOME` is set. The log
files (in `logs`) and the unpacked demo stay in
`~/.local/share/open-annihilation`, or in `$XDG_DATA_HOME/open-annihilation`
when `XDG_DATA_HOME` is set.

**Saved games from earlier versions.** Versions before 0.7 kept saved games
in a `SAVEGAME` folder in `~/.config/open-annihilation`, and a mod's in
`mods/<mod id>/SAVEGAME` there; 0.7.0 kept those without a mod in `Saves`
itself. The first start of a later version moves them into `Saves/default`,
and a mod's into `Saves/<mod id>`, once, and the main menu then says how
many moved and where, with a button that opens the folder. Nothing is
overwritten: a saved game whose name that folder holds already is moved as
`NAME (2).SAV`, keeping both. One that cannot be moved stays where it is, and the game still lists
it there. The log says what moved and what did not. An earlier version
started afterwards no longer sees the saved games that moved. Recordings,
which earlier versions kept in a `demos` folder there, and a mod's in
`mods/<mod id>/demos`, move into `Recordings/default` and `Recordings/<mod
id>` in the same way, once, without a message on the main menu.

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

- **To update:** unzip the newer package and start the game from its
  folder. Your settings are kept.
- **To remove:** delete the game's folder. To remove your settings and logs
  too, delete the two folders above; your saved games, screenshots, films,
  recordings and mods are in the Open Annihilation folder in Documents. To make your
  desktop forget the game as the opener of `.oamod` files, delete
  `~/.local/share/mime/packages/net.coreprime.open-annihilation.xml`,
  `~/.local/share/applications/net.coreprime.open-annihilation.desktop`,
  the two `net.coreprime.open-annihilation.png` and `application-x-oamod.png`
  icons under `~/.local/share/icons/hicolor/256x256`, and the hidden empty
  `.net.coreprime.open-annihilation.updated` files in the `mime`,
  `applications` and `icons/hicolor` folders there.
