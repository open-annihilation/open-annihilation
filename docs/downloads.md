# Downloads and privacy

Open Annihilation can download language packs, mods and map packs from
inside the game, from **registries**: places that publish a signed list of
packages, called a catalogue. Core Prime's registry, at
downloads.coreprime.net, is built in, and you can add others. This page
says how registries work for a player, what the Downloads settings do, and,
under [Privacy](#privacy), exactly what a download sends, what Core Prime
records and what someone on the network can see.

Browsing and installing are in [the Library](library.md). To run a
registry of your own, read the [OA Registry Specification](registries/README.md).

## Contents

- [Registries](#registries)
  - [Core Prime's registry](#core-primes-registry)
  - [Adding a registry](#adding-a-registry)
  - [Not reviewed by the OA team](#not-reviewed-by-the-oa-team)
  - [Turning a registry off, and removing it](#turning-a-registry-off-and-removing-it)
  - [Mirrors](#mirrors)
  - [A registry you are building](#a-registry-you-are-building)
- [The Downloads settings](#the-downloads-settings)
  - [When OA checks for content](#when-oa-checks-for-content)
  - [The install ID](#the-install-id)
  - [Downloaded files](#downloaded-files)
- [When a download fails](#when-a-download-fails)
  - [A check the game cannot do](#a-check-the-game-cannot-do)
- [Privacy](#privacy)
  - [What a download sends](#what-a-download-sends)
  - [What Core Prime records](#what-core-prime-records)
  - [What someone on the network can see](#what-someone-on-the-network-can-see)
  - [Turning the install ID off](#turning-the-install-id-off)
  - [Language packs without an install ID](#language-packs-without-an-install-id)
- [Where it is all kept](#where-it-is-all-kept)

## Registries

A registry is a web address that serves a descriptor (its name, where its
catalogue is, the keys that sign it and how its packages are downloaded), a
catalogue of its packages with a signature beside it, and the packages
themselves, each named by its SHA-256. The Library, Settings › Language and
the map browser show the packages of every registry that is on, together,
each marked with where it comes from.

Every package, from any registry or from a file you open, passes the same
checks before OA uses it: the package's limits, the hacks a mod needs, and
for a map pack whether its maps fit the game and the mod they are played
with. A package key is unique within its registry, and OA keeps one copy of
a key at a time: installing the same key from another registry replaces
the copy you have, and a mod from a registry you added, or any map pack,
asks first ([which downloads ask first](library.md#which-downloads-ask-first)).

The Library's **Registries** tab lists them
([the Registries tab](library.md#the-registries-tab)). Settings ›
Downloads, on the main menu, has **Registries in the Library…**, which
opens the Library on that tab ([picture](settings.md#downloads)).

### Core Prime's registry

Core Prime's registry is built into Open Annihilation. Its descriptor ships
in the `registries` folder beside the game, and the key that signs its
catalogue is compiled into the engine, so no one else can sign a list OA
takes for Core Prime's. Its card on the Library's Registries tab is
headed "Core Prime - Official Open Annihilation Packages", and says what
the registry is, "Built in", how many packages it lists ("1 package") and
when it was last read ("refreshed 4 min ago"). It can be turned off, but
not removed,
so it is always one switch away. Its packages are published by the OA team.

A download from Core Prime's registry installs when you press its button,
GET, UPDATE, Update all, a language in Settings › Language or the
language notice's Download, the map browser's Download and select, or GET
MAP, with no question, mods and their updates included. The one exception
is a map pack that would replace a copy of it you did not get from Core
Prime ([which downloads ask first](library.md#which-downloads-ask-first)).

### Adding a registry

OA suggests no registry: you add each one yourself. There are three ways:

- **By its address.** On the Library's Registries tab, press
  **ADD REGISTRY…** at the top right, above the cards, type the address of the
  registry's descriptor (`http://…` or `https://…`, for example
  `https://maps.example.org/oa/registry.yaml`) and press **CHECK**. Under
  the address, **Signing key** takes the registry's public key when you
  have it: `ed25519:` and 43 base64 characters and `=`, pasted as the
  address is. Leave it empty and OA keeps the descriptor's keys and shows
  them for you to confirm. Type a key and **CHECK** verifies the catalogue
  against that key alone. A key that is not of that form is refused before
  anything is fetched ("The signing key could not be read."). When the
  catalogue is not signed by the key you gave, the window says "The
  catalogue isn't signed by this key." and **ADD** stays disabled. Where
  this system has TLS, an `https://` address uses it, and a certificate
  the system rejects fails the check. Where it does not, OA reads that
  address as `http`, and Settings › Downloads says so. If that host serves
  no HTTP, the window says "This registry needs HTTPS, which this system
  cannot use".
- **By opening an `.oareg` file**, a registry's descriptor as a file a
  registry hands out: double-click it, choose Open With and Open
  Annihilation, drag it onto the game's window, or start the game with its
  path. It opens on the main menu, one at a time, never during a game, and
  the window says "From *file*" where the address would be. It keeps the
  file's keys and shows no **Signing key** field.
- **From GET MAP** in the battle room, when the host plays a map from a
  registry you have not added: the line says "Add *registry* to get *pack*",
  and GET MAP opens the window with the registry's address already checked
  ([map packs](maps/README.md#get-map-in-the-battle-room)).

OA then reads the descriptor and fetches the registry's catalogue. With
no signing key typed, it checks the signature with the descriptor's keys.
With one typed, it checks the signature with that key alone. The **Add
registry** window shows what you are about to trust:

- the registry's name, with its icon beside the name once that picture
  has arrived. The logo the game ships is never shown for a registry a
  player adds;
- **Address**: the host its catalogue comes from;
- **Packages**: how many it lists and of which kinds, as "31 · maps";
- **Signing key**, or **Signing keys**: the fingerprint of each key that signs its
  catalogue, four groups of four hexadecimal digits such as
  `7001 · af50 · b971 · 5bb4`, up to three and then how many more. When
  you gave a signing key, this is that key's fingerprint;
- **Downloads**: "direct", "with download keys", or "with download keys ·
  needs an install ID" ([the install ID](#the-install-id));
- "Not reviewed by the OA team. Only add registries you trust. OA checks
  every package it downloads."

**Cancel**, which has the focus, adds nothing; **ADD** adds the registry at
once, and its packages join the Library. From then on OA trusts the keys
you saw and no others for that registry: a catalogue signed by any other
key is refused. When you gave a signing key, that one key is the
registry's only trusted key, in place of the descriptor's. A registry
changes its keys by listing new ones in a catalogue signed with a key you
already trust, and OA takes them without asking.

The window refuses, and says why, a registry that would pass for a built-in
one ("*name* is built in, so it can't be added again.", or "This registry
uses Core Prime's keys or address, so OA refuses it."), one that names the
logo the game ships ("*name* can't use the Open Annihilation logo."), one
already added
("*name* is already added. To give it another address, use ADD MIRROR… on
its row."), an unsigned one outside a builder's own machine
([below](#a-registry-you-are-building)), more than 64 added registries, and
anything it cannot read, reach or verify ("OA couldn't use *address*:
*what was wrong*.").

### Not reviewed by the OA team

The OA team publishes the packages in Core Prime's registry. Packages from
a registry you added are marked "Not reviewed by the OA team" in the
Library's details ("*registry* · Not reviewed by the OA team"), "not
reviewed" on the registry's card and in Settings › Language, and the
Registries tab says under the last card: "Content from registries other than
Core Prime isn't reviewed by the OA team. OA still checks every package
before using it."

OA checks such a package exactly as it checks Core Prime's: the same
limits, the same hacks, the same map fit. What it cannot check is what the
package is for, so add only registries you trust. A mod from a registry you
added asks before an update that changes the game's rules, and before it
replaces a copy you did not get from that registry
([which downloads ask first](library.md#which-downloads-ask-first)).

### Turning a registry off, and removing it

On the Library's Registries tab each registry has a switch. Off, its
packages leave the Library at once and nothing downloads from it; what you
installed from it keeps working. A registry you added also has **Remove**,
which forgets the registry and the list OA kept of it at once, and leaves
what you installed from it in place. **Close** does not put a switch or a
removal back. Core Prime's registry can be turned off, never removed.
Turning a registry on fetches its list at once.

### Mirrors

A registry can be served from more than one host. Its descriptor, or its
signed catalogue, may list mirrors: other hosts with the same catalogue and
packages, which OA tries in turn when the first cannot be reached. OA
trusts the signature and the SHA-256, never the host, so a mirror needs no
trust of its own.

You can add a mirror to a registry you added: **ADD MIRROR…** on its row
opens the window as **Add mirror**, for "Mirror of *name*". Type the
mirror's address
and press **CHECK**: OA fetches its catalogue and checks it with the keys
you already trust for that registry ("Checked against the keys you trust
for *name*"), and **ADD MIRROR** adds it. The registry's row then says how
many mirrors you added. Mirrors of Core Prime's registry come only from
its own signed catalogue ("Mirrors of Core Prime come only from its own
catalogue."). [Mirrors](registries/mirrors.md) in the specification says
how to copy a registry onto a host of your own.

### A registry you are building

Every registry's catalogue must be signed. While you build one, OA also
accepts an unsigned registry, but only with Developer Mode on (Settings ›
Developer) and only when its catalogue, its mirrors and its download
address are all on `127.0.0.1`, your own machine; otherwise the window says
"Unsigned registries can be added only in Developer mode, from 127.0.0.1.".
The window then shows "Unsigned · accepted in Developer mode only" in place
of the keys. [Running a registry](registries/running.md) says how to make
and serve one, and how to sign its catalogue.

## The Downloads settings

Settings › Downloads ([pictures](settings.md#downloads)) holds when OA
checks for content, the install ID and the files downloads keep. The
registries are on the Library's Registries tab. On the main menu,
**Registries in the Library…** opens it there.

### When OA checks for content

OA fetches a registry's catalogue to know what it can download and which
updates wait. **Check for content updates** says when:

| Choice | OA fetches the lists |
|---|---|
| **Automatically**, the default | as the game starts, for each registry whose list is more than 6 hours old; when the Library opens, at most once a minute; and when you ask |
| **Only when I open the Library** | when the Library opens, at most once a minute, and when you ask |
| **Never** | only when you ask |

You ask with **Check now** at the foot of the Library. Whatever the choice, OA also
fetches a registry's list when you turn that registry on or add it, when a
download finds that its release is no longer offered, and when GET MAP
needs the host's registry's list.

Whatever the choice, OA never installs anything until you press a button.
Without a network, everything you installed keeps working, and the Library
shows the last list it fetched, marked offline
([offline](library.md#offline)).

### The install ID

An install ID is a random number, 128 bits from the system's random
generator, which a registry can ask for so that it can count installs and
keep automated programs from downloading in bulk. Core Prime's registry
asks for one. A registry whose Add registry window did not say "needs an
install ID" never receives one.

- **One for each registry.** Each registry that asks has its own ID, made
  separately, so no two registries can tell that they serve the same
  player.
- **Made at your first download** from that registry, not when the game
  starts. Until then Settings › Downloads says "Made at your first
  download" under the Install ID hint. Afterwards a row labelled "Your
  install ID" shows the ID in part, as "7f3a90d2 · · · c91e".
- **Sent only when asking that registry for a download**, never with
  anything else ([what a download sends](#what-a-download-sends)).
- **Reset** replaces it at once with a new random ID. Cancel does not undo
  it.
- **Turned off**, OA forgets the ID and makes none while it is off, and
  downloads from that registry are off ([below](#turning-the-install-id-off)).
  Turned on again, the next download makes a new one.

The switch is labelled **Install ID**, or "Install ID · *registry*" when
more than one registry uses one. It is kept in your preferences file as
`open-annihilation.install-id.<registry>`.

### Downloaded files

Downloads are written to a downloads folder in the game's data folder
([where](#where-it-is-all-kept)), a part at a time, so that one cut short
resumes where it stopped, even at the next start. **Downloaded files** says
how much that folder keeps, "Nothing kept" when it is empty, and **Empty**
deletes those package files at once. It never touches what you installed,
nor a file a download is using: EMPTY is greyed while a download is
waiting, running or paused, and during a game.

## When a download fails

The package's line in the Library says why, naming the reason once, and
**Retry** tries again ([the download queue](library.md#the-download-queue)). A
package the installer refuses says the installer's own reason, such as "The
folder cavedog-evad-river-delta does not hold this map pack. It is left as
it is."

Every failure also writes one line to the game's log (the `logs` folder of
the game's data folder, [below](#where-it-is-all-kept)). It names the
package, the reason, and what the registry's server last answered for the
key request and for the package: the address, the status, the content type
and the length, or the network failure when nothing was answered:

```text
open-annihilation: content: download of Ridge (ridge ridge, release 28) failed: the server's file is not the size the catalogue promised; package: http://downloads.example.net/v1/p/3f2a….oamod answered 200, application/octet-stream, 60 bytes
```

The address is written without its query, which holds a signed address's
key, and the line never holds the install ID.

### A check the game cannot do

A registry's server, or the network in front of it, can answer a game's
request with a check meant for a web browser, such as a picture puzzle,
in place of a package or a key. OA knows one by the header the server's
firewall sends with it, or by a web page where a package was expected. It
never answers such a check, asks again by itself, or looks for another
address around it. The Library says:

> Core Prime asked for a check the game cannot do. Try again later. · RETRY

and **Retry** asks again when you choose. The log line says `the server
asked for a check the game cannot do`, with the status, the header and the
page's length.

This is not the check that you are a person a registry asks for itself,
which shows a code to enter on its page
([the check](library.md#check-that-youre-a-person)).

## Privacy

Open Annihilation reports nothing about how you play. For content it makes
only these requests: it fetches registries' catalogues, their signatures,
packages' pictures and packages; and from a registry
that uses download keys it asks for a key before each package, reports how
the download ended, and, while a check that you are a person waits, asks
whether it is passed.

### What a download sends

From a registry that uses download keys, such as Core Prime's, a download
makes two requests to the registry's download address. The first asks for
a key and sends:

| Field | What it holds |
|---|---|
| `install` | the install ID for that registry, sent only when the registry asks for one |
| `package` | the package's key in the catalogue, such as `zh-Hans` |
| `kind` | `oalang`, `oamod` or `oamap` |
| `release` | the catalogue's release number of the package |
| `sha256` | the package's SHA-256, from the catalogue |
| `engine` | the Open Annihilation version, such as `0.8.0` |
| `platform` | `windows`, `macos`, `linux`, `ios` or `android` |
| `arch` | `x64`, `x86`, `arm64` or `arm` |
| `language` | the language the game shows its text in, such as `de` |
| `reason` | why: `install` (GET, a language you choose, Download and select), `update` (UPDATE and Update all) or `offered-in-lobby` (GET MAP in the battle room); `oa-tool registry mirror` sends `mirror` as it copies a registry |

The registry answers with a web address for the package, good for one
download and a short time. When the download ends, the second request
reports how, and sends:

| Field | What it holds |
|---|---|
| `result` | `installed`, `refused`, `failed` or `cancelled` |
| `bytes` | how many bytes were fetched |
| `seconds` | how long it took, from the first key |

A key that runs out before the download ends is asked for again, with the
same fields. Nothing else goes with a download. The catalogue, its
signature and the pictures are fetched with no
install ID and nothing about you. A registry that downloads directly
("direct") gets no key request and no result: it sees only the package
fetched.

Every request OA makes, like any connection on the internet, reaches the
registry's host from your network address, and says it comes from Open
Annihilation and its version (`User-Agent: OpenAnnihilation/0.8.0`).

### What Core Prime records

Core Prime records each download from its registry, and nothing else about
you:

| Recorded | From | Used for |
|---|---|---|
| Install ID | the key request | counting installs; limits; challenges |
| Package, kind, release, SHA-256 | the key request, checked against the current catalogue | downloads of each package and release |
| OA version, platform, architecture, language | the key request | which builds and platforms are in use, and how fast updates spread |
| Reason and result, bytes, seconds | both requests | how often installs succeed, and failed and repeated downloads |
| Country | worked out from your address when the first key is issued | where players are; the address itself is never stored |
| Daily counts | worked out each day from the records | the figures the Library shows ("12,400 downloads") |

With each record it keeps when the first key was issued, how many keys the
download took, when the last was issued, and when the result came.

- **How long.** Records and daily counts are kept for 30 years, then
  deleted. The running totals the Library shows are worked out again from
  the daily counts.
- **Your address is never stored.** It is used while a request lasts: to
  work out the country, through a lookup of Core Prime's own that keeps
  its answers for a day under a one-way hash of the address, not the
  address; to count recent requests against limits, in memory, under such
  a hash; and inside the package's download address, which works only from
  the address that asked for it and only for 15 minutes. Neither the
  service nor the host in front of it keeps access logs that would hold
  addresses.
- **Checks that you are a person.** When many downloads come from one
  address in a short time, Core Prime asks for a check
  ([the challenge](library.md#check-that-youre-a-person)). It keeps the
  check's code with your install ID until a day after the check expires,
  and once you pass it, your install ID as trusted for a day.
- **Anonymous.** Install IDs are random and resettable, and no address is
  stored, so the records describe installs, not people. Nothing links an
  install ID to an account or a person.
- **No play.** Core Prime's download service records downloads and their
  checks only. Open Annihilation reports nothing about what you play.

Another registry keeps its own records, under its own rules. The
specification asks every registry never to record the network address a
request came from.

### What someone on the network can see

Where this system has TLS, an `https` catalogue or package is encrypted on
the path, and a certificate the system rejects is a failed fetch, not a
quiet retry over HTTP. Where it does not, OA reads an `https` address as
`http` (Settings › Downloads says so once), so that a build without a TLS
library can still download. On that plain path, someone between you and a
registry, on a shared Wi-Fi network say, can:

- **see what you download**: the addresses fetched, which name each
  package by its SHA-256, and the key requests and results, install ID
  included;
- **stop a download**: the Library then says it could not reach the
  registry, and what you installed keeps working.

The signatures and hashes stop them from changing anything, whichever
transport carried the bytes:

| They try to | OA checks | And so |
|---|---|---|
| change a package on its way | the package's SHA-256 against the signed catalogue's, before it is installed | the download is refused, fetched once more, then reported as failed |
| serve a forged catalogue | its Ed25519 signature over the exact bytes, against the keys built in for Core Prime or the ones you trusted when you added the registry | it is refused, and OA keeps the last good list |
| serve an older catalogue, to hide a fix | its sequence number, which never goes back | it is refused |

A check that you are a person names the page to open. That page may be
`http` or `https`. Core Prime's check is at
`https://downloads.coreprime.net/verify`; do not complete a check in Core
Prime's name on any other page.

### Turning the install ID off

Turn **Install ID** off in Settings › Downloads, and press OK. OA forgets
the ID and sends none. You can still browse the Library and see updates,
but downloads from that registry are off: its GET and UPDATE say
"Downloads from Core Prime are off (Settings › Downloads)", a download
already waiting stops, and Settings › Language marks its languages
"downloads off". Everything you installed keeps working, a package file you
open still installs, and other registries' downloads are not affected.

### Language packs without an install ID

Language packs are also published as plain files: each pack and revision
is a release of the open-annihilation/languages repository on GitHub, such
as `zh-Hans-r2`, with its `.oalang` file. Download it with your browser and
open it, double-click it or drag it onto the game's window, and it
installs as any `.oalang` file does
([installing a language pack](languages.md#installing-a-language-pack)),
with no install ID and no request to Core Prime. A language downloaded from
inside the game, from Settings › Language, the notice at start or the
Library, comes from Core Prime's registry, and so with its install ID.

## Where it is all kept

- **The registries you added**, and the built-in ones you turned off, are
  in `Registries.yaml` in your own Open Annihilation folder, beside `Mods`
  ([the installation guides](installation/macos.md#where-it-keeps-its-files)
  say where that is). OA writes it as you add, remove or switch registries.
- **The lists OA fetched**, the pictures and the
  downloaded files are in the game's data folder, under `content`: on macOS
  `~/Library/Application Support/net.coreprime.open-annihilation`, on
  Windows `CorePrime\Open Annihilation` in your local application data, and
  on Linux `~/.local/share/open-annihilation`. Deleting that `content`
  folder loses nothing you installed: OA fetches the lists again.
- **Check for content updates and the install IDs** are in your preferences
  file ([settings](settings.md#where-the-settings-are-kept)).
