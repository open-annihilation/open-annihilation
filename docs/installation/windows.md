# Installing on Windows

Open Annihilation has four Windows packages, and an installer for each.
Together they cover Windows 95 through Windows 11, including Windows on
ARM. No game data is included: you also need the game data from your own
copy of Total Annihilation.

## 1. Choose your package

| Your computer | Package |
|---|---|
| 64-bit Windows 7, 8, 10, 11 or later | `open-annihilation-<version>-windows-x64.zip` |
| 32-bit Windows from XP SP3 on, on a Pentium III, Athlon XP or newer processor | `open-annihilation-<version>-windows-x86.zip` |
| Windows 11 on ARM | `open-annihilation-<version>-windows-arm64.zip` |
| Windows 95, 98 or Me | `open-annihilation-<version>-windows-95.zip` |

The Windows 95 package runs on Windows 95 as first released and OSR2,
Windows 98 and Windows Me. It is 32-bit x86, for a Pentium II or newer
with 128 MB of memory, and it does not need SSE. It is built for speed
the same way as the other Windows packages. It computes with the
processor's floating-point unit, and it does not include the Linux SDL
notice. A separate download keeps the programs' symbols, for reading a
crash.

Each zip has a matching installer,
`open-annihilation-<version>-<package>-setup.exe`, made from the same
files.

To see which Windows you have:

- **Windows 10 and 11:** open **Settings › System › About** and read
  **System type**.
- **Windows 7 and 8:** open **Control Panel › System** and read
  **System type**.
- **Windows XP:** right-click **My Computer**, choose **Properties** and read
  the **General** tab. Unless it says "x64 Edition", it is 32-bit.

If you are not sure, the 32-bit package runs on 64-bit Windows too. Its
installer uses **Program Files (x86)** there.

The Windows 95 installer also runs on later Windows, including Windows NT,
and does not refuse them. Use the package made for your Windows when there
is one. Windows XP and later 32-bit Windows use the 32-bit package.

## 2. Download it

1. Open the latest release on GitHub:
   [github.com/open-annihilation/open-annihilation/releases/latest](https://github.com/open-annihilation/open-annihilation/releases/latest).
2. Under **Assets**, download the installer or the zip for your package.

## 3. Install it

### With the installer

1. Double-click the setup program. Windows may ask for an administrator,
   because the game is installed for everyone who uses the computer.
2. The pages are a welcome, the licence (`LICENSE`), a desktop shortcut
   (on unless you turn it off), the folder, the install, and a finish page
   that offers to start Open Annihilation.
3. The folder starts as `Program Files\Open Annihilation`. The 64-bit and
   ARM packages use `C:\Program Files` on 64-bit Windows. The 32-bit package
   uses `C:\Program Files` on 32-bit Windows and `C:\Program Files (x86)` on
   64-bit Windows. The Windows 95 package uses `C:\Program Files`.
4. A Start Menu folder named **Open Annihilation** holds the game and the
   uninstaller.

Running the installer again replaces the copy already there, in the same
folder.

A silent install, for a script, is the setup program followed by `/S`. To
choose the folder, add `/D=` and the full path as the last thing on the
command line, with no quotes around the path:

```bat
open-annihilation-<version>-windows-x64-setup.exe /S /D=C:\Games\Open Annihilation
```

### From the zip

1. Right-click the downloaded `.zip` and choose **Extract All…**. On
   Windows XP this opens the Extraction Wizard.
2. Choose where to put it, for example `C:\Games`, and extract.

This makes a folder such as `open-annihilation-<version>-windows-x64` that
holds `open-annihilation.exe`, `oa-intro.exe`, `oa-tool.exe`, `LICENSE`,
`ATTRIBUTIONS.md`, `licenses/`, `fonts/`, `languages/interface.tdf` and
`registries/`. Always start the game from this folder, never from inside
the `.zip`.

## 4. Get the game data

Open Annihilation needs the folder of an installed Total Annihilation with
the 3.1 update: the folder that holds `totala1.hpi`.

- **From GOG:** install **Total Annihilation: Commander Pack** with GOG
  Galaxy or with GOG's offline installer, as you would to play it. Note
  the folder it installs to: GOG's installer suggests one under
  `C:\GOG Games`.
- **From the original CDs:** install the game and update it to version
  3.1.
- **The free demo:** to try Open Annihilation without the full game, see
  [Playing the demo](../../README.md#playing-the-demo).

Windows opens paths of up to 259 characters unless long paths are turned
on, which Windows 10, version 1607, and later allow: the **Enable Win32 long
paths** policy (under Computer Configuration, Administrative Templates,
System, Filesystem), or the registry value `LongPathsEnabled` set to 1 under
`HKEY_LOCAL_MACHINE\SYSTEM\CurrentControlSet\Control\FileSystem`. A Total
Annihilation folder whose path, with the names of the files in it, is longer
than that needs long paths turned on, or a folder with a shorter path; the
game says so if it cannot open the folder.

## 5. Start the game

1. Start **Open Annihilation** from the Start Menu or the desktop shortcut,
   or double-click `open-annihilation.exe` in the install folder or the
   folder from step 3.
2. Windows may say **"Windows protected your PC"**, because the program is
   new to it. Click **More info**, then **Run anyway**. On Windows XP, a
   security warning may ask whether to run the file: click **Run**.
3. The first time, it asks for your Total Annihilation folder. Choose the
   folder from step 4. It remembers your choice.

A later start opens as you left it. The first start is full screen.
After that, full screen stays full screen, and a window comes back at the
same size and place on the same display when it is still connected.
Starting once with `-d` or `--resolution` uses that choice for the one
run only.

To choose a different folder later, start it with `--choose-game-dir`:

1. Right-click `open-annihilation.exe` and choose **Create shortcut**.
2. Right-click the shortcut and choose **Properties**.
3. In **Target**, add ` --choose-game-dir` after the program's name.

Or, in a Command Prompt in the game's folder:

```bat
open-annihilation.exe --choose-game-dir
```

Every option the game takes, and every way to start a network game from a
shortcut or a launcher, is in
[Command Line and Game Launching](../command-line.md).

Open Annihilation's own settings open with **Ctrl+,**, or from
**SETTINGS…** at the foot of the Library, which the **OA** button at the
bottom right of the main menu opens. See [Settings](../settings.md), and
[the Library](../library.md) for mods, maps and languages to download.

Each start registers this copy of the game, for your Windows account, as
the opener of the file types left on in [Settings › System](../settings.md#system):
mod packages (`.oamod`), map packs (`.oamap`), language packs (`.oalang`),
mutator packages (`.oamut`), brain packages (`.oabrain`), registry files
(`.oareg`) and launch files
(`.oalaunch`). All seven are on
until you turn one off. Turning one off and pressing OK withdraws that type
at once; turning one on registers it at once. Neither waits for a restart. The game's icon is
put on the types it opens. Double-click a `.oamod` file to install the mod
it holds ([mods](../mods/README.md#installing-a-oamod-file)).
Opened while the game runs, the file goes to the running game. A file
sync, such as OneDrive's of Documents, or a virus scanner can hold a new
mod's files for a few seconds, and the install waits for them. A
`.oalaunch` file starts the network game it describes
([opening a launch file](../command-line.md#opening-a-launch-file)). The
installer associates launch files with the game for everyone who uses the
computer too. With Launch files off, the game also hides that association
from your Windows account alone: it sets the (Default) of
`HKEY_CURRENT_USER\Software\Classes\.oalaunch` to an empty text, which
names no program, so Windows asks which program to open a launch file with,
as it does for a copy you unzipped. The other accounts on the computer keep
the association, and turning the switch on again registers this copy for
you in place of the empty text. While the switch is off, a program you
choose for launch files in Windows stays your choice.

The first time you host or join a multiplayer game, Windows Defender
Firewall may ask whether Open Annihilation may communicate on networks:
allow it, at least on private networks. On Windows XP, a security alert
may ask the same: click **Unblock**. A firewall between the players must
let through UDP port 47624, which other computers use to find a hosted
game, TCP ports 2300 to 2400 and UDP ports 2350 to 2400.

## Windows XP and older computers

- On a computer with a single processor, no SSE2 or less than 512 MB of
  memory, the game starts at 800×600, at up to 60 frames a second and
  without enhanced anti-aliasing. These can all be changed in the settings.
- On Windows XP the game plays its sound through its own wave-out output,
  which leaves a single processor more time for the game than SDL's. If you
  hear no sound, start the game with SDL's sound output instead. In a
  Command Prompt in the game's folder:

  ```bat
  set OA_SOUND_OUTPUT=sdl
  open-annihilation.exe
  ```

  On a newer Windows that plays no sound, `set OA_SOUND_OUTPUT=waveout`
  chooses the game's own output in the same way.
- Windows XP, and Windows 95, 98 and Me, draw no icon stored the way the
  game's is, so the program and `.oamod` files show Windows' plain icons
  there.
- Windows 95, 98 and Me use the Windows 95 package: a Pentium II or newer,
  128 MB of memory, and no need for SSE.

## Where it keeps its files

Your saved games, screenshots, films, recordings and mods are in the
**Open Annihilation** folder in your Documents folder (**My Documents** on
Windows XP), wherever Windows keeps it, OneDrive or a folder your
organisation has moved it to included:

| Folder | What it holds |
| --- | --- |
| `Saves` | saved games, a mod's in `Saves/<mod id>` and those without a mod in `Saves/default` |
| `Screenshots` | screenshots (Ctrl+F9) and posters (`MakePoster`), in a folder for each mod as in `Saves` |
| `Films` | films (Ctrl+F10), a `MOVIEnnn` folder each, in a folder for each mod as in `Saves` |
| `Recordings` | recordings of network games, which `--play-demo` plays back, in a folder for each mod as in `Saves` |
| `Mods` | mods you add, each in a folder of its own, which the Library lists besides the game folder's `mods` folder ([mods](../mods/README.md#installing-a-mod)) |

A saved game or a recording may be named in any script, Chinese among
them, and the Documents folder may lie under a user name in any script:
both work whatever the system's language for non-Unicode programs, on
Windows XP too ([typing.md](../typing.md#saved-games-and-recordings-named-in-any-script)).

The game makes each folder the first time it needs it. In the settings
(**Ctrl+,**), **System** shows the folder under
**Your files**, whose buttons open Saves, Screenshots and Mods in Explorer.

Your settings, the log files (in `logs`) and the unpacked demo stay in
`%LOCALAPPDATA%\CorePrime\Open Annihilation`. On Windows XP that folder is
`Local Settings\Application Data\CorePrime\Open Annihilation` in your user
folder.

**Saved games from earlier versions.** Versions before 0.7 kept saved games
in a `SAVEGAME` folder in `%LOCALAPPDATA%\CorePrime\Open Annihilation`, and
a mod's in `mods/<mod id>/SAVEGAME` there; 0.7.0 kept those without a mod in
`Saves` itself. The first start of a later version moves them into
`Saves/default`, and a mod's into `Saves/<mod id>`, once, and the main menu
then says how many moved and where, with a button that opens the folder.
Nothing is overwritten: a saved game whose name that folder holds already
is moved as `NAME (2).SAV`, keeping both. One that cannot be moved stays where it is, and the
game still lists it there. The log says what moved and what did not. An
earlier version started afterwards no longer sees the saved games that
moved. Recordings, which earlier versions kept in a `demos` folder there,
and a mod's in `mods/<mod id>/demos`, move into `Recordings/default` and
`Recordings/<mod id>` in the same way, once, without a message on the main
menu.

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

- **To update an installed copy:** run the newer setup program. It replaces
  the copy in the same folder. Your settings are kept, and so is the Open
  Annihilation folder in Documents.
- **To update a zip copy:** unzip the newer package and use its folder.
  Your settings are kept.
- **To remove an installed copy:** open **Settings** or **Control Panel**,
  choose **Apps** or **Add or Remove Programs**, select **Open
  Annihilation** and remove it. Or open the Start Menu folder and run
  **Uninstall Open Annihilation**, or run `Uninstall.exe` in the install
  folder. A silent removal is `Uninstall.exe /S`.
- The uninstaller removes the files it installed, the Start Menu folder,
  and the desktop shortcut when the installer added that shortcut. It does
  not remove the Open Annihilation folder in Documents, so saved games,
  screenshots, films, recordings and mods stay. It does not remove a copy
  of the game you unzipped somewhere else.
- The uninstaller also removes the launch file association the installer
  made for everyone. For the Windows account that runs it, it removes the
  game's registration as the opener of `.oamod`, `.oalang`, `.oamap`,
  `.oamut`, `.oabrain`, `.oareg` and `.oalaunch` files while that registration opens them with
  the installed `open-annihilation.exe`, and the empty (Default) of
  `HKEY_CURRENT_USER\Software\Classes\.oalaunch` that hid the association
  while Launch files was off. A type that a copy you unzipped elsewhere
  registered last opens with that copy, and stays registered. Another
  program's choices for these files stay too.
- **Other accounts:** the uninstaller cannot reach the other Windows
  accounts on the computer, nor yours when Windows asked for another
  administrator's password to remove the game: then the account that ran
  it is that administrator's. Each account that started the installed game
  keeps its registration, which still names the removed program, so
  double-clicking one of those files there tries to start it and Windows
  says it cannot find it. The registration stays until that account starts
  Open Annihilation from another copy, one unzipped or installed again:
  that start registers the copy that runs in its place, withdraws the types
  turned off in its settings, and removes the empty (Default) of
  `.oalaunch`. Or delete the fourteen keys below in that account.
- **To remove a zip copy:** delete that folder. The Open Annihilation
  folder in Documents is separate and stays until you delete it yourself.
- **To remove your settings and logs too,** delete the Local AppData folder
  above. To make Windows forget a copy you unzipped as the opener of
  `.oamod`, `.oalang`, `.oamap`, `.oamut`, `.oabrain`, `.oareg` and `.oalaunch` files, delete
  these fourteen keys with the Registry Editor:
  `HKEY_CURRENT_USER\Software\Classes\.oamod`,
  `HKEY_CURRENT_USER\Software\Classes\OpenAnnihilation.Mod`,
  `HKEY_CURRENT_USER\Software\Classes\.oamut`,
  `HKEY_CURRENT_USER\Software\Classes\OpenAnnihilation.Mutator`,
  `HKEY_CURRENT_USER\Software\Classes\.oabrain`,
  `HKEY_CURRENT_USER\Software\Classes\OpenAnnihilation.Brain`,
  `HKEY_CURRENT_USER\Software\Classes\.oalang`,
  `HKEY_CURRENT_USER\Software\Classes\OpenAnnihilation.Language`,
  `HKEY_CURRENT_USER\Software\Classes\.oamap`,
  `HKEY_CURRENT_USER\Software\Classes\OpenAnnihilation.MapPack`,
  `HKEY_CURRENT_USER\Software\Classes\.oareg`,
  `HKEY_CURRENT_USER\Software\Classes\OpenAnnihilation.Registry`,
  `HKEY_CURRENT_USER\Software\Classes\.oalaunch` and
  `HKEY_CURRENT_USER\Software\Classes\OpenAnnihilation.Launch`.
