# Open Annihilation for iPhone and iPad

This folder builds the game for iOS and iPadOS: the same engine as the desktop game, built as an
application bundle that the iOS simulator on a Mac and iPhone and iPad devices install. It plays
from your own Total Annihilation 3.1c game folder, as the desktop game does, which it keeps in its
own storage: on the first start the **Game files** screen brings the folder in (see
[Game files](#game-files)). Everything that only the iOS build needs lives here; the engine's
sources stay the same on every platform and switch what iOS cannot do with plain build options
and platform hooks (see [What iOS changes](#what-ios-changes)).

The touch controls (tap, drag, hold, pinch, the thumb column, the order rail and the rest) are
part of the engine and are on from the start in this build. A hardware keyboard, mouse or
trackpad works as on the desktop, and the Apple Pencil works as a mouse that hovers.

## What you need

- A Mac with Xcode (the iOS 26 system images were used here) and its command line tools, CMake
  3.24 or later, Ninja and Python 3. ccache is used when it is installed.
- The fonts the desktop build uses: `python3 tools/bootstrap_text_fonts.py --fonts-only` from the
  checkout's folder, once.
- Your Total Annihilation folder (the one holding `totala1.hpi`).

SDL 3.4 and FreeType are built for iOS by [tools/bootstrap_ios_deps.py](tools/bootstrap_ios_deps.py),
from the same pinned releases the desktop build uses, into `local/deps/ios-iphonesimulator` and
`local/deps/ios-iphoneos`. The build scripts run it for you; it builds nothing that is already
current. Each library's folder records what it was built with, SDL's patches from
[tools/sdl-patches](../../tools/sdl-patches/) among them, and a library whose record differs, such
as an SDL built before a patch was added or changed, is built again. The desktop build keeps
accepting SDL 3.2; only this build asks for 3.4.

## Run it in the simulator

From the checkout's folder:

```sh
./run.sh --ios-simulator                 # the iPhone 17 simulator
./run.sh --ios-simulator --ipad          # the iPad Air 11-inch (M3) simulator
./run.sh --ios-simulator --udid <UDID>   # any other simulator (xcrun simctl list devices)
```

[run.sh](run.sh) here does the work (the root `./run.sh --ios-simulator` hands its other arguments
to it): it builds current source and stops if the build fails, signs a copy of the bundle for the
simulator, installs it, copies your game folder into the game's `Documents/Total Annihilation`
the first time, and launches the game with `--skip-intro --mute`. It starts the simulator if it
is not running; open the Simulator app to see and play it.

The game folder copied is the one you name with `--game-data <folder>`, else `OA_GAME_DIR`, else
the one copied last time, else the `OA_GAME_DIR` a desktop build tree of this checkout was
configured with. On the same disk the copy is an instant clone that takes no extra space.
`--fresh-data` replaces the copy on the simulator, and
[scripts/push-game-data.sh](scripts/push-game-data.sh) does just the copy.

Other options: `--no-build` installs the bundle built last; `--no-data` copies no game folder,
so the game starts as a player's first start does, on the Game files screen; `--reinstall`
removes the game and its data from the simulator first, for a fresh install; `--no-launch`
installs without launching; `--screenshot <file>` saves a screenshot a few seconds after the
launch (the screenshot is the simulator's portrait screen, so the landscape game appears turned
on its side); options the script does not know, and anything after `--`, go to the game.

The game writes its log and preferences in its data container, under
`Library/Application Support/net.coreprime.open-annihilation`, and the player's saves,
screenshots, films, recordings and mods in `Documents/Open Annihilation`, which the Files app
shows; `xcrun simctl get_app_container <UDID> net.coreprime.open-annihilation data` prints where
the container is.

## Build only

```sh
platforms/ios/scripts/build.sh               # simulator: build-ios-sim/engine/open-annihilation.app
platforms/ios/scripts/build.sh --device      # devices:   build-ios-device/engine/open-annihilation.app
```

[scripts/build.sh](scripts/build.sh) configures [CMakeLists.txt](CMakeLists.txt) with Ninja and
the toolchain file [cmake/OaIosToolchain.cmake](cmake/OaIosToolchain.cmake) (arm64, iOS 15.0 and
later) on first use, and builds only the game's target, `oa-game`: on iOS every program CMake
builds becomes an application bundle, so the tools and tests are built and run on the desktop
instead. `--build-type`, `--jobs` and `--build-dir` change the defaults (Check, 3 jobs,
`build-ios-sim` or `build-ios-device`).

## On an iPhone or iPad

A device needs the game signed with your Apple developer identity and a provisioning profile,
which Xcode manages:

```sh
platforms/ios/scripts/xcode.sh --device --team <your team ID> --open
```

[scripts/xcode.sh](scripts/xcode.sh) generates an Xcode project in `build-ios-xcode` (it is never
committed). Choose the `oa-game` scheme, the Check or Release configuration and your device, and
run. `./run.sh --ios-device` builds for devices with Ninja and prints these steps.

Then open the game: the Game files screen brings your game folder in (see
[Game files](#game-files)). For a test device you can instead build the data into the bundle:
configure with `-DOA_IOS_BUNDLED_GAME_DIR=<your game folder>`, and the game uses that copy when
the Documents folder has none. The game data never goes into the repository.

## The game folder

`--game-dir` names the game folder for one run (the run script passes the simulator's copy).
Without it the game looks in `Documents/Total Annihilation` first, then in the copy built into the
bundle, then in the folder it remembers; the Documents folder comes first because its path changes
whenever the game is installed afresh. With none of them usable the game opens the Game files
screen.

## Game files

On the first start without a game folder it can play, the game opens the **Game files** screen,
which the engine draws itself in its bundled fonts, with no game art (none is installed yet):
[docs/game-files.md](../../docs/game-files.md) describes it. It offers three ways in:

- **Choose the game folder** (Choose folder): the system's document picker, which reaches iCloud
  Drive, a USB drive, a computer's shared folder and the device itself. The game lists the folder
  (names and sizes only, so nothing is downloaded yet), checks it by name and, when its files are
  on the device, with the game's own check, and shows **Ready to copy**: the parts it found, with a
  switch for each optional one, what it leaves out and why, and the space it needs. Copy copies it
  into a staging folder, downloading each file iCloud holds as it goes, checks the copy, and puts
  it in place as `Documents/Total Annihilation` with one rename.
- **Copy it yourself** (I have copied it): the player copies the folder into On My iPhone or On My
  iPad › Open Annihilation with the Files app, or from a Mac with the Finder, and the game checks
  it. A folder copied under another name, or archives copied loose, are found and put in place.
- **Play the 1997 demo** (Choose installer): the picker chooses the installer of the Total
  Annihilation demo (1997), which the system copies for the game; the game recognises it, never
  runs it, and unpacks its game data. This is the route for App Review.

Stop asks whether to keep what was copied; a copy that is stopped, or cut short when the system
ends the game, continues on the next start when the same folder is chosen again, and files already
copied are not copied again. A folder that is replaced is kept as `Total Annihilation (old)` until
the player removes it. The game files are kept out of the device's backups, which would otherwise
count 1.1 GB against the player's iCloud storage; **Settings › Game files** in the main menu's OA
settings shows what is installed, has **Include in device backups** to put them back, and
**Manage…** to check, add, replace or remove the files (removals and replacements take effect from
the next start).

Where things are, in the game's data container:

| Path | What |
|---|---|
| `Documents/Total Annihilation` | the game folder, shown in the Files app and the Finder |
| `Documents/Open Annihilation` | the player's own folder: `Saves`, `Screenshots`, `Films`, `Recordings` and `Mods`, shown in the Files app and the Finder; saves and recordings an earlier version kept beside the preferences file move here once |
| `Documents/Open Annihilation/Games` | a base game copied into the player's folder, with a `.oa-content` record of each file |
| `Documents/Open Annihilation/Expansions` | expansions copied into the player's folder, each under the base game it extends |
| `Documents/Total Annihilation (old)` | a folder set aside by a replacement, until removed |
| `Library/Application Support/net.coreprime.open-annihilation/import/` | the copy being made (`Total Annihilation/`) and its state file |
| `Documents/Open Annihilation/Games/totala-demo` | the demo, with its `.oa-content` record. An earlier unpack in `Library/Application Support/net.coreprime.open-annihilation/demo-1997/` is copied from once and left there |

The Game files screen still fills `Documents/Total Annihilation`, and that folder is what the
game plays from. The same start also copies the known files into `Games` and `Expansions` under
`Documents/Open Annihilation`. That copy only reads the folder the screen filled. On the same
volume the second copy shares the file's disk blocks, so it takes no extra space.

`--no-game-files-screen` shows the message the game had before the screen instead (the **Check
again** alert): it says where to copy the folder with the Files app or the Finder, and stays up
until the folder is there, so the game never closes for lack of its files.

[src/ios_game_files.mm](src/ios_game_files.mm) is the iOS side, the engine's `GameFilesHooks`
(`src/app/include/oa/app/game_files_hooks.hpp`):

| Hook | iOS |
|---|---|
| capabilities | folders, several files, the Files app reaching the game folder, cloud files, background time |
| text | the device's name and the words that name the Files app, the Finder, iCloud and the Settings app, for an iPad or an iPhone |
| game_folder | `Documents/Total Annihilation` |
| show_picker | `UIDocumentPickerViewController` over the game's window: a folder opened in place; the installer as a copy the system makes for the game, moved at once out of the Inbox folder the system empties into `tmp/Chosen files/`; several archives of the type `Info.plist` declares |
| release_source | ends the security-scoped access each chosen item holds, or removes the installer's copy if the game did not take it |
| list_source | the file manager's enumerator; an iCloud file not downloaded yet, listed under its own name or as its `.<name>.icloud` placeholder, is reported under its own name as remote |
| copy_file | the engine's chunked copy inside a coordinated read, which downloads the file first; Stop cancels a wait for a download |
| free_space | the volume's capacity available for important use |
| keep_running | a background task while copying; when its time runs out the copy stops cleanly and continues when the game is back |
| set_backed_up | the excluded-from-backup property of the game folder, the staging folder and the demo's data |

## Testing the Game files screen in the simulator

[scripts/stage-files-source.sh](scripts/stage-files-source.sh) puts a source where the picker
finds it: into the simulator's own storage (On My iPhone or On My iPad), or into the game's
Documents folder. It clones on the same disk, so even the whole game folder takes no time and no
space; its test folders are written by the script, never kept in the repository.

```sh
S=platforms/ios/scripts/stage-files-source.sh
$S --game-dir <your game folder>          # On My iPhone › Total Annihilation, for Choose folder
$S --demo-installer <the demo installer>  # On My iPhone › Total Annihilation.exe, for Choose installer
$S --fixture not-a-game                   # On My iPhone › Not a game: text files only
$S --fixture nested                       # On My iPhone › Old PC › Games › Total Annihilation
$S --fixture mod                          # On My iPhone › Picker Test Mod: a usable mod profile
$S --fixture mutator                      # On My iPhone › Picker Test Mutator.oamut: tap it to install
$S --into-documents "TA Commander Pack"   # the game folder under another name, for I have copied it
$S --clear                                # remove everything it put there
```

`--ipad` (or `--udid`) chooses the simulator, `--name` the item's name. A test of the first start:

```sh
platforms/ios/run.sh --reinstall --no-data          # build, a fresh install, the Game files screen
platforms/ios/run.sh --no-build --no-data -- --no-game-files-screen   # the Check again alert
```

Then Choose folder, Browse › On My iPhone › Total Annihilation › Open. The simulator is landscape
and the picker is the system's, so drive it by hand in the Simulator app, or with any tool that
taps in the simulator's portrait screen points. To see an iPhone's other landscape side (the
camera housing on the right) without turning the simulator, launch with
`SIMCTL_CHILD_OA_IOS_LANDSCAPE=left` in the environment (`right` is the other side): the game
asks iOS for that orientation as its window opens. An iPad ignores it, since iPadOS would put the
game in a window instead. A copy within the simulator's own storage takes about a second, too
short to press Stop or to leave the game while it runs: `SIMCTL_CHILD_OA_IOS_COPY_RATE=10000000`
holds the copy to about 10 MB a second, as a slow shared folder would (each file waits its share
of time before it is read, and Stop ends the wait). The game's log
(`Library/Application Support/net.coreprime.open-annihilation/logs` in its data container) records
the copy, including the files a continued copy skipped. `xattr -l` on the container's
`Documents/Total Annihilation` shows `com.apple.metadata:com_apple_backup_excludeItem` while the
game files are kept out of backups.

[images/](images/) holds the screenshots the installation guide
([docs/installation/ios.md](../../docs/installation/ios.md)) shows, taken this way in the simulator
(`xcrun simctl io <UDID> screenshot`, turned a quarter turn and scaled down). They show only the
Game files screen and the system's message, no game art.

## What iOS changes

[CMakeLists.txt](CMakeLists.txt) adds the engine with these options set, each one a plain
capability that any other platform can set the same way:

| Option | iOS | What it does |
|---|---|---|
| `OA_NATIVE_APP_MENU` | OFF | no macOS menu bar item |
| `OA_PROCESS_SPAWNING` | OFF | the video capture and the director's encoder, which start another program, say this build starts none |
| `OA_SELF_UPDATE` | OFF | the game does not replace itself; a newer release is a notice with the release page, and the player builds it the same way |
| `OA_NATIVE_FOLDER_DIALOG` | OFF | no folder dialog; the game says where its folder goes |
| `OA_NATIVE_DENSITY_WINDOWS` | ON | the window opens at the screen's own pixel density |
| `OA_TOUCH_FIRST` | ON | the touch controls are on from the start |

[src/ios_platform.mm](src/ios_platform.mm) and [src/ios_game_files.mm](src/ios_game_files.mm)
are the only iOS code. The first keeps the game's window landscape and never upside down (see
[Orientation](#orientation)), keeps the home indicator dim (a swipe from the edge only shows it; a
second swipe goes home) and the status bar hidden, plays the touch controls' haptics, gives the
game its default folder, the advice shown without one and its **Check again** button, shows the
player's folders in the Files app (the settings' Your files and Open Mods Folder buttons, and the
notice of moved saves, open it at the folder through its `shareddocuments` link, as `open` does on
the Mac), brings each file opened in the game into the app, and keeps the
system's three-finger editing gestures (undo, copy, paste) from taking the fingers of a
three-finger touch. The second is the Game files screen's side (see [Game files](#game-files)).

A file opened in the game (`.oamod`, `.oalang`, `.oamap`, `.oamut` or `.oareg`) tapped in the Files app, or
shared to Open Annihilation, reaches SDL as an opened URL, which SDL sends the engine as a dropped
file. Before it does, `ios_platform.mm` brings the file into `tmp/Opened mods/<UUID>/` on a
background queue: a copy the system made in an Inbox folder is moved, and any other file (one
opened in place from iCloud Drive or another app's files) is copied inside a coordinated read
under the access its URL grants, which downloads it first. SDL then sends the copy's path; the
engine reads it through `take_opened_file`, which answers with why when the copy failed, and gives
the copy back through `release_opened_file`, which removes it. A mod package is installed into
`Documents/Open Annihilation/Mods`. A language pack or a map pack reaches the same installer. A
registry file waits to be added. Each start removes the copies an earlier run left. Tried in the
iPhone and iPad simulators with mod packages opened through the system from the Files app's own
storage (On My iPhone, On My iPad), which the game copies: opened while the game ran, a package
was brought in and installed in the Mods folder; one that started the game was installed at the
main menu once the intro had played; one whose `oamod.yaml` has errors was refused with them, and
the Mods folder stayed as it was; each copy was still there while the outcome's message showed,
and was gone at the next start. Not yet tried on a device, nor with a file the system hands over
in an Inbox folder or one in iCloud Drive.
The engine offers the Game files screen because these hooks are installed, not because of a build
option: the desktop installs none.

[Info.plist.in](Info.plist.in) sets the orientations, full screen on every iPhone and iPad, the
icon, the distribution keys (see [For distribution](#for-distribution)), shows the Documents
folder in the Files app and the Finder, reads a mouse or trackpad as a pointer, and declares the
type of the game's archives (`net.coreprime.open-annihilation.game-archive`: `.hpi`, `.ufo`,
`.ccx` and `.gp3`), which the picker offers when the player adds archives, and the eight file
types (`.oamod`, `.oalang`, `.oamap`, `.oamut`, `.oabrain`, `.oakeys`, `.oareg` and `.oalaunch`), which the app exports
and opens as their owner, so that the Files app and the share sheet offer Open Annihilation for each, with the app's
icon. `app-bundle-file-types` checks both bundles' declarations.
[LaunchScreen.storyboard](LaunchScreen.storyboard) is the black launch screen with the title.

## Orientation

The game is always landscape, fills the screen and is never upside down:

- It follows the device from one landscape orientation to the other, so the picture is the right
  way up for the person holding it.
- Held portrait, upside down or flat, the device keeps the last landscape orientation the game
  had (landscape right, with the home indicator to the right of the picture, if it has had
  none): the picture stays put, sideways, and is the right way up again as soon as the device is
  turned back to landscape.

An iPhone does this itself: the bundle declares only the two landscape orientations. iPadOS 26
does not turn a landscape-only app's screen when the iPad is held portrait; it shows the app
letterboxed, small, in the portrait screen. So on iPad the bundle declares all four
orientations, and when the screen is portrait the game's view is made landscape-sized and turned
a quarter turn inside it. The game itself only ever sees a landscape window, and touches,
the pointer and the pencil land where they should because SDL reads them in the turned view.

`UIRequiresFullScreen` keeps the game full screen. It is deprecated in iPadOS 26 and still
honoured; with the iOS 27 SDK it would let iPadOS resize the window in steps instead. In iPadOS
26's Windowed Apps multitasking mode every app opens in a window, this one included, which the
player can make full screen; in the Full Screen Apps mode the game opens full screen.

## The app icon

The icon is the Open Annihilation badge, the riveted gold "OA" on a metal plate, made from
`branding/open-annihilation-icon.png` whenever the game is built: no copy of the artwork is
stored here. [tools/make_app_icon.py](tools/make_app_icon.py) finds the plate, cuts a square
just inside its edge with `sips` and scales it to 1024 by 1024, so iOS's own rounded mask falls
on the metal and no second corner shows, and writes it without transparency as the single-size
`AppIcon` of an asset catalogue in the build tree. The asset compiler (`actool`) turns it into
`Assets.car` and the home screen icons in the bundle; Info.plist names them.

## For distribution

What the bundle carries for TestFlight and the App Store:

- **Privacy manifest.** [PrivacyInfo.plist](PrivacyInfo.plist) goes into the bundle as
  `PrivacyInfo.xcprivacy`: no tracking, no tracking domains, no data collected, and the reasons
  for the system calls Apple asks apps to explain (see [the scan](#privacy-manifest)).
- **Info.plist.** `ITSAppUsesNonExemptEncryption` is false (the game uses no encryption; it
  checks files with SHA-256, which is hashing), `NSLocalNetworkUsageDescription` says why LAN
  games use the local network, `LSApplicationCategoryType` is strategy games, and the version
  keys come from the engine's own version.
- **Entitlements.** [OpenAnnihilation.entitlements](OpenAnnihilation.entitlements) asks for the
  multicast networking entitlement (`com.apple.developer.networking.multicast`), which LAN games
  need to find each other by broadcast. Apple grants it to a developer team on request (the
  Multicast Networking Entitlement Request form, with a short account of the LAN games); a team
  without it (every personal team) cannot sign a build that asks for it, so it is off by
  default. Once the team has it, generate the project with `platforms/ios/scripts/xcode.sh
  --device --team <ID> --multicast-entitlement` (CMake option `OA_IOS_MULTICAST_ENTITLEMENT`).
  Simulator builds never use it and sign ad hoc. Without it, iOS refuses the broadcasts LAN
  games use to find each other on a device.
- **Export options.** [export/](export/) holds `ExportOptions-development.plist`,
  `ExportOptions-ad-hoc.plist` and `ExportOptions-app-store.plist` for exporting an archive (see
  [Archives](#archives)); replace `YOUR_TEAM_ID` in them with your team's ID.
- **Store listing.** [store/app-store-metadata.md](store/app-store-metadata.md) is a draft of the
  name, description, keywords, privacy answers, age rating and notes for App Review.

### Privacy manifest

The reasons were chosen from the calls the linked game actually makes, found by listing the
executable's undefined symbols (`nm -u build-ios-sim/engine/open-annihilation.app/open-annihilation`)
and its Objective-C selectors and classes (`strings`), including what SDL and the C++ library
bring in:

| Category | Calls found | Why the game makes them | Reason |
|---|---|---|---|
| File timestamp | `stat`, `fstat`, `lstat`; the C++ library's `last_write_time` and `status` | reading its game folder, saves and settings in its own container, listing saves, and stamping each copied game file with its source's time | C617.1 |
| File timestamp | `lstat`, `stat`, `NSURLContentModificationDateKey` on the items the player chose in the picker | resuming an interrupted copy and noticing a file that changed since the copy began | 3B52.1 |
| System boot time | `mach_absolute_time` (SDL's clock) | timing frames and events | 35F9.1 |
| Disk space | the C++ library's `space`; `NSURLVolumeAvailableCapacityForImportantUsageKey` and `NSURLVolumeAvailableCapacityKey` | checking there is room before unpacking the 1997 demo's data or copying game files, and showing the player the free space on the Game files screen and in the settings | E174.1, 85F4.1 |

None of `statfs`, `statvfs`, `getattrlist` and their relatives, `NSUserDefaults`,
`UITextInputMode` (active keyboards) or `systemUptime` appears. Run the same scan again when a
new library is linked.

### Archives

An archive for distribution is made from the Xcode project with the team's signing:

```sh
platforms/ios/scripts/xcode.sh --device --team <your team ID>
xcodebuild -project build-ios-xcode/open_annihilation_ios.xcodeproj -scheme oa-game \
  -configuration Release -sdk iphoneos -archivePath build-ios-xcode/OpenAnnihilation.xcarchive archive
xcodebuild -exportArchive -archivePath build-ios-xcode/OpenAnnihilation.xcarchive \
  -exportOptionsPlist platforms/ios/export/ExportOptions-ad-hoc.plist -exportPath build-ios-xcode/export
```

Use `ExportOptions-development.plist` for the team's own devices and
`ExportOptions-app-store.plist` for App Store Connect and TestFlight. None of this has been run
here: it needs a paid developer team.

## Not done yet

- The Game files screen's later items: a copy that goes on in the background with the system's own
  progress (iOS 26), continuing a copy without choosing the folder again, importing a .zip,
  "Open in Open Annihilation", drag and drop, and VoiceOver for the engine-drawn screens. Changes
  made in Manage take effect from the next start: the game reloads itself without ending for a
  switch of mod (the Library's Play now), but that reload does not take them up yet.

- No resume after iOS ends the game in the background: a match played alone pauses when the
  game leaves the screen (or waits behind the in-game menu with When the window loses focus at
  Keep playing), and is lost if iOS then ends the process.
- Device builds are built and linked here but have not been installed on a device, and no
  archive has been exported.
- The on-screen keyboard follows the screen, not the game: on an iPad held portrait it is
  expected to open along the portrait screen's edge while the game's picture is turned (not
  tried).
