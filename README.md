<p align="center">
  <img src="https://raw.githubusercontent.com/open-annihilation/branding/main/logos/open-annihilation-header.png" alt="Open Annihilation" width="760">
</p>

<p align="center">
  <a href="https://github.com/open-annihilation/open-annihilation/releases/latest"><img alt="Latest release" src="https://img.shields.io/github/v/release/open-annihilation/open-annihilation?style=flat-square&label=release&color=c0392b"></a>
  <a href="https://github.com/open-annihilation/open-annihilation/releases"><img alt="Downloads" src="https://img.shields.io/github/downloads/open-annihilation/open-annihilation/total?style=flat-square&color=2c3e50"></a>
  <a href="LICENSE"><img alt="Licence: GPL v3" src="https://img.shields.io/badge/licence-GPL%20v3-2c3e50?style=flat-square"></a>
  <img alt="Platforms: macOS, Windows, Linux" src="https://img.shields.io/badge/platforms-macOS%20%7C%20Windows%20%7C%20Linux-2c3e50?style=flat-square">
  <a href="https://discord.gg/GWgWTQKuv"><img alt="Discord" src="https://img.shields.io/badge/Discord-join%20us-5865F2?style=flat-square&logo=discord&logoColor=white"></a>
</p>

<p align="center"><b>Built with</b></p>

<p align="center">
  <img alt="C++20" src="https://img.shields.io/badge/C%2B%2B20-00599C?style=for-the-badge&logo=cplusplus&logoColor=white">
  <img alt="SDL3" src="https://img.shields.io/badge/SDL3-1B3C73?style=for-the-badge">
  <img alt="FFmpeg" src="https://img.shields.io/badge/FFmpeg-007808?style=for-the-badge&logo=ffmpeg&logoColor=white">
  <img alt="CMake" src="https://img.shields.io/badge/CMake-064F8C?style=for-the-badge&logo=cmake&logoColor=white">
  <img alt="Claude" src="https://img.shields.io/badge/Claude-D97757?style=for-the-badge&logo=claude&logoColor=white">
</p>

> [!NOTE]
> **Source code is being prepared for release this week.**

**Open Annihilation** is an open-source game engine for playing
**Total Annihilation** on modern macOS, Windows and Linux, using the game
data from your own copy of the original game. Support for
**Total Annihilation: Kingdoms** is on the [roadmap](#roadmap).

No game data is included. You need an installed copy of Total Annihilation
with the 3.1 update, such as the GOG edition.

Open Annihilation is an independent project. It is not affiliated with or
endorsed by the owners of Total Annihilation, Total Annihilation: Kingdoms or
the Boneyards online service. Total Annihilation, Total Annihilation: Kingdoms
and Boneyards, including their names, game data and other content, are the
copyright and trademarks of their respective owners.

## Related projects

Open Annihilation is part of a family of projects for Total Annihilation and
Total Annihilation: Kingdoms:

- **[CorePrime](https://coreprime.net/)** is bringing Total Annihilation's
  Boneyards online service back to life. The services launch in the coming
  weeks: **[register at coreprime.net](https://coreprime.net/)** in advance.
- **[KBot](https://github.com/coreprime/kbot)** is an open-source toolkit with
  full support for both Total Annihilation and TA: Kingdoms game assets: a
  command-line tool for the games' file formats, and a browser-based studio
  with an asset explorer, map editor, unit viewer and live sandbox.

## Version 0.1

This is an early release, and many parts of the game are still being
completed.

- Total Annihilation single player: the Arm and Core campaigns, and skirmish
  against the computer.
- Save and load, options, intro movies and music.
- This release has no multiplayer or online features.

## Roadmap

- Completing the rest of Total Annihilation's single-player game.
- Support for **Total Annihilation: Kingdoms**.

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

## Community

Join the Open Annihilation Discord server to follow development, report
problems and talk about the game:
**[discord.gg/GWgWTQKuv](https://discord.gg/GWgWTQKuv)**

Everyone taking part is expected to follow the [Code of Conduct](CODE_OF_CONDUCT.md).
Credits are in [CONTRIBUTORS.md](CONTRIBUTORS.md).

## Licence

Open Annihilation is dual licensed. The code in this repository is free
software, released under the GNU General Public License version 3: see
[LICENSE](LICENSE). A separate edition of Open Annihilation is also
maintained under closed terms. Contributions are accepted under a
[Contributor Licence Agreement](CLA.md), so that they can be included in both.

The releases include third-party libraries under their own licences. See
[ATTRIBUTIONS.md](ATTRIBUTIONS.md).

### Why Dual Licensing

Open Annihilation is published here under the GNU GPL v3, so that anyone can
play it, study it and improve it.

A separate edition of Open Annihilation, which can play online with the
original game, is maintained alongside this one. That edition is not open
source.
Publishing code that is network-compatible with the original game would make
it easy to build cheats that affect real players in live online matches, and
keeping it closed protects that community.

So that improvements made here can be included in both editions, contributions
are accepted under a [Contributor Licence Agreement](CLA.md) that assigns
copyright in each contribution to the maintainer. Everything published in this
repository remains available under the GNU GPL v3.
