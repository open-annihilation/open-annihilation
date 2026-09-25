# Open Annihilation

> **Source code being prepared for release this week.**

Open Annihilation is an open-source game engine for playing
**Total Annihilation** on modern macOS, Windows and Linux, using the game
data from your own copy of the original game.

No game data is included. You need an installed copy of Total Annihilation
with the 3.1 update, such as the GOG edition.

Open Annihilation is an independent project. It is not affiliated with or
endorsed by the owners of Total Annihilation. Total Annihilation and related
names are trademarks of their respective owners.

## Version 0.1

This is an early release, and many parts of the game are still being
completed.

- Single player: the Arm and Core campaigns, and skirmish against the
  computer.
- Save and load, options, intro movies and music.
- This release has no multiplayer or online features.

## Download

Download the zip for your platform from the
[Releases](https://github.com/open-annihilation/open-annihilation/releases)
page:

| Platform | File |
|---|---|
| macOS 11 or later (Intel and Apple silicon) | `open-annihilation-v0.1-macos-universal.zip` |
| Windows (64-bit) | `open-annihilation-v0.1-windows-x64.zip` |
| Windows on ARM (experimental, not yet tested on hardware) | `open-annihilation-v0.1-windows-arm64-experimental.zip` |
| Linux (x86-64) | `open-annihilation-v0.1-linux-x86_64.zip` |
| Linux (ARM64) | `open-annihilation-v0.1-linux-arm64.zip` |

## Running

Unzip the file and start the game: **Open Annihilation.app** on macOS,
`oa-game.exe` on Windows, `oa-game` on Linux.

The first time it starts, Open Annihilation asks you to choose the folder
where Total Annihilation is installed, and remembers your choice. To choose a
different folder later, start it with `--choose-game-dir`. To use a folder
for one run only, start it with `--game-dir <folder>`.

- **macOS:** the app is not yet signed by Apple, so macOS asks once before
  it opens. Right-click **Open Annihilation.app** and choose **Open**. On
  macOS 15 or later, open it once, then choose **Open Anyway** in
  System Settings > Privacy & Security.
- **Linux:** an X11 or Wayland desktop is required. The folder dialog uses
  your desktop's file chooser (the XDG portal, or `zenity`).

## Licence

Open Annihilation is free software, released under the GNU General Public
License version 3. See [LICENSE](LICENSE).

The releases include third-party libraries under their own licences. See
[ATTRIBUTIONS.md](ATTRIBUTIONS.md).
