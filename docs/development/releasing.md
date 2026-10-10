# Releasing for macOS

`tools/release_macos.sh` builds the macOS release of Open Annihilation,
packages it and checks the packages. The maintainer runs it on the Mac that
holds the project's signing keys. Continuous integration builds and tests
the game unsigned and never signs or notarizes anything, and no key,
password or signing identity is kept in the repository or on GitHub. The
other platforms' packages are not built by a script in this repository yet;
the Steam Deck package, the Linux x86_64 package with the Deck's own files
added, is described at the end ([The Steam Deck
package](#the-steam-deck-package)).

## What it makes

The script writes two files and a folder in the folder `--out` names (by default
`build-release-macos/package`), for the version `v` followed by the
`VERSION` of the project in `CMakeLists.txt` (or the one `--version` gives,
with or without the `v`), which the application's `Info.plist` must carry:

| File | What it holds |
|---|---|
| `open-annihilation-vX.Y.Z-macos-universal.zip` | the folder `open-annihilation-vX.Y.Z-macos-universal`, holding `Open Annihilation.app`, `LICENSE`, `ATTRIBUTIONS.md` and `licenses/`, as in every earlier macOS release |
| `open-annihilation-vX.Y.Z-macos-universal.pkg` | an installer package that puts `Open Annihilation.app` in `/Applications` on the startup disk; Installer asks for an administrator's password |
| `open-annihilation-vX.Y.Z-macos-universal-symbols/open-annihilation` | the game's executable as the build made it, with the symbols the packaged one is stripped of; it is not shipped, and it is kept for reading the addresses of a player's crash report (its UUID is the packaged executable's) |

Both run on Apple silicon and Intel Macs (arm64 and x86_64) with macOS 11.0
or later. The folder `work` beside them keeps what the script staged,
expanded and checked, and the notary service's logs.

## What it does

1. **Builds the game.** The tree must have no uncommitted changes;
   `--allow-local-changes` builds one anyway, for a test and never for a
   release. The script says whether `HEAD` carries the version's tag.
   `tools/bootstrap_macos_deps.py` builds the pinned zlib, SDL3 and
   FreeType as static libraries for both architectures and macOS 11.0 under
   `local/deps/macos-11.0` (the first run downloads their archives and
   checks each against its SHA-256). Each library's folder records in
   `build-settings.json` the version, archive checksum, deployment target,
   architectures and options it was built with, and SDL's also the patches
   of `tools/sdl-patches` it was built with, each by its SHA-256. A library
   is built again whenever these differ from the pins, patches and options
   of the tools, so the notices never name a version the application does
   not hold, and an SDL built before a patch was added or changed is never
   linked.
   `tools/bootstrap_text_fonts.py --fonts-only` puts the text fonts in
   `local/deps/text-fonts`, and the build copies them into the bundle's
   `Contents/Resources/fonts`. The
   `oa-game` target is then built in `build-release-macos`, its CMake cache
   cleared first and its bundle made afresh: a Release build without
   tests or the game's self-checks (`OA_SELF_CHECKS=OFF`), for both
   architectures, linking those libraries and no others (never Homebrew's).
2. **Checks the build:** both architectures, macOS 11.0 as the oldest
   release in each and in `Info.plist`, only the system's libraries linked,
   the source's notices in `Contents/Resources`, the text fonts in its
   `fonts` folder, each the bootstrap's byte for byte, and nothing else in
   the bundle. The notices are `LICENSE`, `ATTRIBUTIONS.md` and the files of
   `licenses/` that Git tracks; a file Git does not track there, a
   `.DS_Store` for one, stops the script, since the build would put it in
   the application.
3. **Stages the application** as `Open Annihilation.app`, the name its
   `Info.plist` gives (`CFBundleName`), with the notices Git tracks beside
   it. The build tree, `run.sh` and the tests keep the target's names,
   `open-annihilation.app` and the executable `open-annihilation`. The
   staged executable is stripped of its symbols (`strip`), and the build's
   own is kept in the `-symbols` folder.
4. **Signs it inside-out:** any code inside the bundle besides its
   executable first, deepest first (the game has none today), then the
   application, each with the hardened runtime and a secure timestamp
   (`codesign --options runtime --timestamp`), never with `--deep`.
5. **Notarizes and staples it:** `ditto` zips the application,
   `xcrun notarytool submit --wait` sends it to Apple's notary service and
   waits for the verdict, the service's log is kept, and
   `xcrun stapler staple` attaches the ticket to the application.
6. **Writes the zip** from the staged folder, after stapling.
7. **Builds the installer package:** `pkgbuild` makes a component that
   installs the application in `/Applications` and is not relocatable, so
   Installer writes there even when another copy of the application, a
   build tree's for instance, is elsewhere on the disk. `pkgbuild` stores
   each file's extended attributes as an AppleDouble (`._`) entry, and a
   file written in some sessions, a terminal inside another application
   for one, carries a `com.apple.provenance` attribute that cannot be
   removed; the script rewrites the component without those entries, in its
   bill of materials and its payload, so Installer sets no attribute on the
   files it writes, as with the zip. `productbuild` wraps the component in a
   distribution titled Open Annihilation that requires the application's
   `LSMinimumSystemVersion` (macOS 11.0), runs natively on the
   architectures of its executable (both) and installs only on the startup
   disk, and `productsign --timestamp` signs it. The package is then
   notarized and stapled like the application.
8. **Checks both packages** as players receive them: the zip holds one
   folder and nothing else, the source's notices, and the staged
   application unchanged whether `ditto` or `unzip` extracts it; the
   installer package holds that application alone, installing in
   `/Applications`. Each copy of the application, and the installer
   package, must pass the checks listed under
   [Checking packages by hand](#checking-packages-by-hand).
9. **Starts the game** from the zip, with `--game-dir`, headless from both
   the `ditto` and the `unzip` copy under each architecture this Mac runs
   (x86_64 through Rosetta on Apple silicon), and with `--save` loads a
   saved game under each, plays 1000 ticks and requires the same final
   digest. `--previous-zip` compares the zip's file
   list with an earlier release's; a stapled application has one file more,
   `Contents/CodeResources`, the ticket.

It prints each package's SHA-256 at the end. A notarized package never has
the bytes of an earlier build, so release notes take these values. Its last
lines say whether the packages are a release: only when the release build
made them from a tree without uncommitted changes whose `HEAD` carries the
version's tag, and both were signed with a Developer ID, notarized and
stapled. Otherwise they say NOT FOR RELEASE and why.

## Setting up the Mac

The Xcode Command Line Tools (`xcode-select --install`) provide `codesign`,
`xcrun notarytool` and `xcrun stapler`; `pkgbuild` and `productbuild` come
with macOS. The build needs CMake 3.24 or newer and Python 3, and uses Ninja
when it is installed. The x86_64 checks need Rosetta on Apple silicon
(`softwareupdate --install-rosetta`). Downloading the dependencies, secure
timestamps, notarization and stapling need internet access.

### Signing identities

The login keychain holds the two Developer ID certificates, each with its
private key, and Apple's Developer ID intermediate certificate:
**Developer ID Application**, which signs the application, and **Developer
ID Installer**, which signs the installer package. `security find-identity -v`
lists both as valid. The script takes them from the environment of the
shell that runs it, never from a file of the repository:

```sh
export OA_MACOS_APP_IDENTITY="Developer ID Application"
export OA_MACOS_INSTALLER_IDENTITY="Developer ID Installer"
```

Each value is a certificate's name or a part of it that only one identity
in the keychains matches, as `codesign` and `productsign` take it. When
the keychains hold two of a kind, a renewed certificate beside an old one
for instance, give the full name that `security find-identity -v` prints.

The first time `codesign` or `productsign` uses a private key, macOS may
ask for the login keychain's password in a dialog, and the script waits
until it is answered; **Always Allow** lets that tool use the key from then
on without asking. Sign something once by hand to answer it before a
release, for example `productsign --sign "Developer ID Installer" in.pkg
out.pkg` on any unsigned package.

### Notary credentials

The script sends submissions with an App Store Connect API key. The
recommended way is to store the key's credentials in the login keychain
once, as a notarytool profile, and name the profile:

```sh
xcrun notarytool store-credentials open-annihilation-notary \
    --key /path/to/AuthKey_KEYID.p8 --key-id KEYID --issuer ISSUER-ID
export OA_MACOS_NOTARY_PROFILE=open-annihilation-notary
```

`store-credentials` checks the credentials with Apple before it saves them.
Without a profile, three variables name the key instead:
`OA_MACOS_NOTARY_KEY` (the path of the `.p8` file), `OA_MACOS_NOTARY_KEY_ID`
and `OA_MACOS_NOTARY_ISSUER`. Keep the key file outside the checkout. The
script passes these values to `xcrun notarytool` and never prints them.

The script does not print the identities either, but `codesign -dvv`,
`spctl -vvv`, `productsign` and `pkgutil --check-signature` print the name
of the certificate that signed a package, with its team identifier. Anyone
can read them from a signed package; keep the script's output and the
`work` folder private all the same.

## Making a release

Tag the release commit (the tag can stay local until the packages pass),
then, with the identities and the notary profile set, run the script from a
checkout of that commit without uncommitted changes:

```sh
git tag -a v0.5.0 -m "Open Annihilation 0.5.0"
tools/release_macos.sh --out "$HOME/releases/v0.5.0" \
    --game-dir "/path/to/Total Annihilation" --save "/path/to/a saved game.SAV" \
    --previous-zip "$HOME/releases/v0.4.3/open-annihilation-v0.4.3-macos-universal.zip"
```

The last line it prints must be "release v0.5.0: built from the tagged
tree, signed with a Developer ID, notarized and stapled". Then attach the
zip and the installer package to the GitHub release, with the SHA-256
values the script printed.

The first signed release also changes what the README and the release
notes tell a player: that the application is signed and notarized, so
Gatekeeper opens it without asking, and that the installer package puts it
in `/Applications`.

`tools/release_macos.sh --help` lists every option: `--build-dir`, `--deps`
and `--jobs` choose where and how the game is built.

## Without a Developer ID

Without `OA_MACOS_APP_IDENTITY`, or with `--unsigned`, the application is
signed ad hoc, the installer package is left unsigned and nothing is
notarized; the script says so first, and its last line says NOT FOR
RELEASE. Gatekeeper then asks before it opens the application, as the
README describes. Signing without notary credentials signs and does not
notarize, and Apple notarizes only what a Developer ID signed.

`--app BUNDLE` packages a bundle already built instead of building one, and
leaves out the checks of the release build, so its packages are never a
release, whatever signs them. The `release-macos-package`
test runs the script that way, with `--unsigned`, on the game every macOS
build makes, so that continuous integration keeps the packaging working
without any key or Apple's services.

## The hardened runtime

The application is signed with the hardened runtime and no entitlements,
since it needs none: it loads no code but its own executable and the
system's libraries (SDL3 and zlib are linked into the executable),
makes no executable memory, reads no `DYLD_` variables, and uses neither
camera nor microphone. The x86_64 code runs under Rosetta with the same
signature. Add an entitlement only when a checked run shows the hardened
runtime stopping the game, and say beside it why.

## When Apple rejects a submission

The script prints the submission's ID and status and the issues Apple's log
lists, and keeps the log in `work` as `notary-application.json` or
`notary-installer.json`. `xcrun notarytool log ID`, with the same
credentials, fetches it again, and `xcrun notarytool history` lists the
submissions.

## Checking packages by hand

These are the script's checks, to run again on a package; nothing is
installed. For an application:

```sh
codesign --verify --deep --strict --verbose=2 "Open Annihilation.app"
codesign -dvv "Open Annihilation.app"   # runtime flag, Developer ID Application, Timestamp, TeamIdentifier
spctl -a -vvv -t exec "Open Annihilation.app"   # accepted, source=Notarized Developer ID
xcrun stapler validate "Open Annihilation.app"
```

For the installer package:

```sh
pkgutil --check-signature open-annihilation-vX.Y.Z-macos-universal.pkg
spctl -a -vvv -t install open-annihilation-vX.Y.Z-macos-universal.pkg
xcrun stapler validate open-annihilation-vX.Y.Z-macos-universal.pkg
pkgutil --payload-files open-annihilation-vX.Y.Z-macos-universal.pkg
pkgutil --expand-full open-annihilation-vX.Y.Z-macos-universal.pkg expanded
```

`pkgutil --payload-files` also lists the AppleDouble (`._`) entries that
carry a file's extended attributes, which Installer would set on the files
it writes; the script leaves them out, so it lists none. The expanded
package holds the application in `open-annihilation.pkg/Payload`.

## The oldest macOS release

The packages run on macOS 11.0 or later: `macos_minimum` in
`tools/release_macos.sh` names it, the dependencies and the game are built
for it, and the checks hold the build to it. Supporting an older release
on Intel Macs is a change of its own: the dependencies rebuilt for it, that
value and the release notes changed, and a run on that release of macOS. Apple silicon needs
macOS 11.0 in any case.

## The Steam Deck package

The Linux packages are not built by a script in this repository yet. The
Steam Deck package is the Linux x86_64 package with one folder more,
`steam-deck`, beside `open-annihilation`:

| File | What it holds |
|---|---|
| `open-annihilation-vX.Y.Z-steam-deck.zip` | the folder `open-annihilation-vX.Y.Z-steam-deck`, holding the Linux x86_64 package's files (`open-annihilation`, `oa-intro`, `oa-tool`, `LICENSE`, `ATTRIBUTIONS.md`, `licenses/`, `fonts/` and `languages/`) and the `steam-deck` folder |

The `steam-deck` folder holds the Steam Input templates and the library
artwork that the Steam Deck guide
([steam-deck.md](../installation/steam-deck.md)) has the player install by
hand, on a Steam Deck or any Linux computer with Steam; nothing in it adds
the game to Steam. Make it first, in a build tree of the release's commit
for the machine that makes the package, which need not be the tree the
package's game comes from:

```sh
cmake --build build --target oa-steam-deck-files
```

This writes `build/steam-deck`, made afresh each time. `oa-steam-artwork`,
which draws the pictures, is a program of that build and runs while the
target is built, so a tree cross-compiled for another system cannot write
the folder. It holds:

| File | What it is |
|---|---|
| `artwork/portrait.png`, `wide.png`, `hero.png`, `logo.png`, `icon.png` | Steam's library pictures, which `oa-steam-artwork` makes from `branding/open-annihilation-icon.png` and the bundled fonts at that moment: 600×900, 920×430, 1920×620, the logo as wide as the icon and the name need, and 256×256 |
| `open-annihilation.vdf`, `open-annihilation-keyboard-mouse.vdf` | The two Steam Input templates |
| `README.md` | What the folder holds, and how to install the templates and the artwork by hand |

Then put the Linux x86_64 package's files in a folder named
`open-annihilation-vX.Y.Z-steam-deck`, copy the `steam-deck` folder into it
beside them, and zip that folder as the other packages are zipped: the
folder alone at the top of the zip, with `open-annihilation`, `oa-intro`
and `oa-tool` still allowed to run. The artwork is the Open Annihilation
branding, under the terms of
`licenses/LicenseRef-OpenAnnihilation-Branding.txt`, which the package
already carries; it is made at packaging time and never committed.

Before a release, check the folder: each picture has the size above, and
both templates and the README are there. The Linux x86_64, arm64 and armhf
packages have no `steam-deck` folder.

The SDL in the Linux and Steam Deck packages, as in every package, is the
pinned release with the patches in
[tools/sdl-patches](../../tools/sdl-patches/README.md) applied, which
`tools/bootstrap_sdl.py` applies when it unpacks the release;
[ATTRIBUTIONS.md](../../ATTRIBUTIONS.md) says so. One of them lets the
game play its light ticks on the Steam Deck's trackpads.
