# Design decisions

The maintainer's decisions of 8 October 2026 on finishing the Boneyards, and
what each one means for the code. The work landed in the 0.9 series. The
sources are a plain copy in the engine, under `src/boneyards/`, with no
project history. [The work](#the-work) lists the phases they lead to;
[Open questions](#open-questions) lists what is still to be settled. Where
these decisions and an older document disagree, these hold until that
document is updated.

| Decision | In short |
|---|---|
| [The engine pin moves](#the-engine-pin-moves) | The fold follows 0.8.14, extension table 19 |
| [The unfinished features are built](#the-unfinished-features-are-built) | QuickPlay, Locator, Matchmaking and registration; QuickPlay and Locator released to every account on 2026-11-06, Matchmaking for Admin and Beta Tester accounts until a release build; no licence screen |
| [Mocked where the protocol is missing](#mocked-where-the-protocol-is-missing) | Features that need Core Prime to extend its protocol get their UX now, with nothing behind it |
| [News and Ladders stay on HTTP](#news-and-ladders-stay-on-http) | No TLS library |
| [Exit to the system](#exit-to-the-system) | The Boneyards box asks "Exit to the system?" |
| [The Boneyards stays its own project for now](#the-boneyards-stays-its-own-project-for-now) | The sources are copied into `src/boneyards/`; words outside it stay reserved |
| [Every language](#every-language) | Text other than English works everywhere; rows grow where needed; rooms stay shared |
| [Master servers baked in at build time](#master-servers-baked-in-at-build-time) | Alpha (`alpha1.clusters.coreprime.net`, two shards) by default; a build can carry another list |
| [The Boneyards mod](#the-boneyards-mod) | `coreprime.ccx` beside the install's `.ccx` archives; no installation archives, so the base sim hash stays; a replacement for a file in `by.ccx` is a loose file in the mod folder |
| [Login checks](#login-checks) | No CD check or disc rule; a placeholder update pass; no version check yet |
| [Passwords masked](#passwords-masked) | The panes' password entries show asterisks |
| [Testing against Core Prime](#testing-against-core-prime) | Live tests use credentials the maintainer provides |
| [Where the design work lives](#where-the-design-work-lives) | The flow map and the plan live in research's `boneyards` folder |
| [Release lanes](#release-lanes) | Updated as the work goes; dropped at the fold |
| [Futures](#futures) | Galactic maps over the wire, ladders per mod, server-side QuickPlay and Matchmaking |

## The engine pin moves

The earlier pin was the engine at v0.7.3, extension table 13. The fold
follows the engine at 0.8.14. The extension table is version 19. Versions
15 to 19 are additions; the module follows them and fills none of the new
hooks. The engine's text fonts and its FreeType draw the pages.
`cmake/OaBoneyards.cmake` and `tools/boneyards/bootstrap_fonts.py` keep only
these fonts. Windows packages are the engine's. The module is still not
released until the maintainer says so.

## The unfinished features are built

3.1c shows QuickPlay, Locator and Matchmaking only to accounts whose status
is 4 (Admin) or 5 (Beta Tester), and every one of their actions ends in a
"coming soon" box or does nothing. REGISTER only points at a web site. All
four are built. Each unfinished feature has a compile-time constant,
`OA_BONEYARDS_<FEATURE>_RELEASED` (0 or 1). When a feature is complete it is
ungated and its constant removed for the next release.

The maintainer released QuickPlay and the Locator to every account on
2026-11-06. Their constants are gone: every account sees both buttons lit,
with no dust cover, and opens both consoles, and Core Prime's game manager
takes a QuickPlay search from every logged-in account. Matchmaking still
shows only to Admin and Beta Tester accounts, as in 3.1c, and the server
still queues only those accounts; its constant,
`OA_BONEYARDS_MATCHMAKING_RELEASED`, and the registration form's,
`OA_BONEYARDS_REGISTRATION_FORM_RELEASED`, are both 0 for now.

Where 3.1c has no screen, the new screens
are built from the Boneyards' own widgets (the green console,
Function_Button_Large, radio buttons, check boxes, Icon_Circle rows, the
message box), and their pictures are drawn in the same style and palette as
GAF frames. The startup licence screen is not built.

- **QuickPlay:** a console where the player picks a game type, where to
  look, skill, side and a favourite map, then FIND BATTLE. FIND BATTLE
  searches through GameManager. A forming battle can be joined, and when
  none fits the player can host one with the numbers the offer carries.
  Game types follow the Host Battle rules (Galactic War only on a contested
  planet for a player with an allegiance, War Training only on the player's
  own side's planet).
- **Locator:** the console the 3.1c Help pages describe. It searches for a
  player (the user locate request), a planet (the war map, locally), a
  battle (the game locate request, slot 66, which Core Prime's game
  manager answers for every planet at once), a frequency (each planet's
  room list in turn; a frequency the server marks global, or one every
  planet lists under the same id and name when the server sends no global,
  is one row whose WHERE reads every planet), or a server. Find Battle
  shows a forming battle's seats as "forming n/m" when the answer carries
  them. JOIN on a battle that needs a password opens Battle Intelligence
  so the password can be entered there. Server Transfer lists the build's
  master servers and the yard master's list, shows each server's region
  when the list has one (US East for CorePrime (Alpha)), logs out and logs
  in to the chosen one, and is disabled for the current server. RELOCATE,
  JOIN and PAGE act on the selected result.
- **Matchmaking:** a dialog that opens over the Chat or Battle console,
  with game type, skill, side and planets, a queue meter and ACTIVATE. The
  battle console's match control panel opens it too. The queue is Core
  Prime's, so the dialog is mocked: ACTIVATE shows the searching state and
  no match is made yet.
- **Registration:** REGISTER opens a registration console showing only the
  web route, unless the player has logged in before as an Admin or Beta
  Tester account: the Boneyards then sets the `BetaLoginUX` player
  preference, and from the next visit the console also offers the in-game
  route; a later login as an ordinary account clears it. There is one
  address for every server. On the web (working): a QR code and the address
  `https://coreprime.net/register`,
  with OPEN IN BROWSER on a desktop; on Steam Deck only the QR code and the
  address. In the game (mocked): name, password and its confirmation,
  e-mail, a side and acceptance of the Rules of Conduct, checked before
  sending; until Core Prime takes registrations over its protocol the form
  says the server does not take them yet and points to the web page. No CD
  key or proof of ownership is needed: Total Annihilation never had CD
  keys. The guest's chat notice points at the same console.

## Mocked where the protocol is missing

The maintainer runs the servers and builds the protocol and service features
later. Where a feature needs a protocol extension or the server to
orchestrate it, its UX is built now in mocked form: complete in look and
flow, with no function behind it. Today that is Matchmaking's queue,
registration in the game, UTF-8 on the wire, and the game locate request. What works with the requests the client
already has is built working.

## News and Ladders stay on HTTP

News, Ladders and any other remote contents are fetched over plain HTTP,
as 3.1c fetches them. No TLS library is added, which keeps the oldest
Windows builds and the Raspberry Pi buildable. A content server that
answers only over HTTPS is not supported.

## Exit to the system

The Boneyards box that asks "Exit to Windows?" asks "Exit to the system?",
the wording the base game uses for every exit.

## The Boneyards stays its own project for now

The sources are a plain copy under `src/boneyards/`. The project's own
history is not in this repository. Words outside that module stay reserved;
the carve-out that names the module is a later change. The registration
address is `https://coreprime.net/register`. The runtime-boundary check,
the project's own launcher and the 32-bit library are not part of this
build.

## Every language

Text in any language works in every place the Boneyards shows or sends
text: typed text, chat, names, profiles, battle and room names, server
messages, HTML pages, message boxes and the lines it posts to a battle.

- Every string the Boneyards holds is UTF-8. Typed text comes from the
  system's text input, with composition for input methods and the
  candidate window placed by the text field.
- Every font keeps its original glyphs for the characters it covers, so
  English looks exactly as it does now. Every other character comes from an
  open font of similar design, drawn at a size that reads well and coloured
  as the original glyphs are. The fonts are chosen: the three
  Haettenschweiler GAF fonts and the message boxes' shaded Hattfont12 take
  Fira Sans Extra Condensed for Latin, Greek, Cyrillic and Vietnamese, then
  Noto Sans CJK at medium weight, then Noto Emoji. That collection is drawn
  into the bitmap atlas, which is committed and copied into the game; the
  font files themselves are not shipped. The HTML pages
  keep Liberation Sans and Serif for Arial and Times New Roman, with DejaVu
  Sans and Noto Emoji behind them. Chinese, Japanese and Korean on those
  pages come from the engine's text faces: the endonym face, and a language
  pack's faces when that stack holds them. A line those faces do not hold
  shows as the engine's own text does. Pixel CJK fonts are not pursued.
- With the engine's "Use modern fonts for game text" on (its default), the
  chat log and the entry line follow the player's Text size setting: they
  use the engine's modern faces at the size the in-battle message log uses
  (DejaVu Sans Bold at 14 pixels times Text size, 11 at the default 80%;
  Noto Sans CJK at 12 pixels at least; Noto Emoji), so one setting governs
  both. Rows grow, long lines wrap, and the entry line grows with the text.
  The lists, labels and buttons keep the Boneyards' fonts. With modern fonts
  off, chat keeps the Boneyards' own fonts.
- Rows grow where a string's glyphs need the room, and the UX adapts where
  it makes sense, keeping the consoles coherent and the Boneyards' look
  intact. The lists' 17-pixel rows already hold a 13-pixel CJK glyph, so
  they keep their height; the 14-pixel entry lines grow to 17, and the
  planet summary, Recon's fields and the message boxes adapt.
- Measuring and wrapping work on runs of glyphs across those fonts, and a
  line can break between two CJK ideographs. Nothing assumes one byte per
  character. Name limits count characters.
- Chat rooms stay shared between original 3.1c clients and Open
  Annihilation's. Until Core Prime announces UTF-8 and converts text for the
  older clients, the wire carries Windows-1252 with "?" for what it cannot
  hold, and incoming text is decoded the same way. Match reports and chat
  relays follow the same rule. Every field is cut on a character boundary.
- The Boneyards' own words go through the engine's language catalogue in a
  `languages/boneyards.tdf` it ships, and its 3.1c texts through the
  engine's translations.
- Checks draw each font in Western and Central European scripts, Greek,
  Cyrillic and CJK, and pass chat through Rover to a server that takes
  UTF-8 and to one that does not.

## Master servers baked in at build time

Rover's single built-in server becomes a list of master servers the build
bakes in, set in one place (a CMake cache entry naming a server-list file in
the format of `server.byd`), so a build can carry a new list where needed.
The default list holds Core Prime's Alpha: `alpha` on `clusters.coreprime.net` with two
shards, so the yard master is reached at `alpha1.clusters.coreprime.net`.
Alpha's line also names its region, US East, which Server Transfer shows.
The player's saved list (`serv_sav.byd`) and the player's own `server.byd`
still come first, and `--boneyards-server` still writes a one-entry list.
`server.byd` in the expansion or the game folder is the 1998 list, and it
is not read, so a player with neither file reaches Alpha. The login
console's server list and Locator's Server Transfer show the result; more
servers come later.

## The Boneyards mod

The Boneyards becomes a mod in Open Annihilation. The mod folder holds
`coreprime.ccx`, mounted beside the install's `.ccx` archives. The profile
names no installation archives, so the mod keeps the base game's sim hash.
The two archives share no path. A file that must replace one in `by.ccx`
ships as a loose file in the mod folder, which wins over every archive.
`coreprime.ccx` holds the new GAFs (the search radar, the Locator icons, the
queue meter and the registration QR code, pre-rendered from the build's
registration address) and the GUI TDFs that lay out the new screens. The war
map is not in that archive. After login the player's copy is the one the
server sent; otherwise the boneyards expansion's copy is used, then a copy
in the game folder. The GUI TDFs follow
TA's GUI gadget format, extended with the Boneyards' widget types. Only the
new screens use them: the Boneyards' 3.1c consoles stay in code, and TA's
own screens keep the engine's handling. The player's own 3.1c data is never
changed. Rover and the Boneyards read their
`boneyards/*` files through the engine's file system, so data in either
archive is found; what they write goes to the player's folder. FIND
BATTLE's wider button is not shipped as a picture: it is stretched at run
time from the player's own Function_Button_Large.

## Login checks

The CD check is already skipped, and the battle room drops 3.1c's START
rule for game discs (one disc, two above three players, three above six),
as Core Prime's patch and GOG's release do; no modern computer has a CD
drive. Checking the client's version was the auto-updater's job, not the
protocol's. The update console plays a placeholder pass ("Checking
auto-updates...", file checks filling its list, the vertical progress bar
rising) before the login console; game content will reach players another
way.

## Passwords masked

The password entries of the Create Frequency, Change Frequency, Host Battle
and Battle Intelligence panes show asterisks, where 3.1c shows what is
typed.

## Testing against Core Prime

The Boneyards service has been largely rebuilt as Core Prime. Live tests
run against it, with the maintainer's login or a set of regression
credentials provided when a test needs them (`OA_BY_LIVE_SERVER`,
`OA_BY_USER`, `OA_BY_PASSWORD`). Recorded replies cover everything else.

## Where the design work lives

The clickable screen-flow map, the screen specs it is built from, its new
art and the completion plan live in the research repository's `boneyards`
folder. Only their sources are kept in Git: the built map embeds game art,
so it is generated beside them and ignored.

## Release lanes

The `release-work/boneyards-*` lanes were updated as the work went, onto the
engine and its native pipeline. The fold dropped them. The engine's release
carries the module.

## Futures

- New galactic maps are downloaded from Core Prime over the wire, using the
  messages the protocol already has (the file-server address and an HTTP
  download), instead of shipping in `coreprime.ccx`; a downloaded map goes
  to the player's folder and takes precedence.
- The Boneyards' content merges into the main game, in an architecture
  where each mod can have its own ladders (Ridge, for example) and
  only the base game runs the Galactic War.
- Core Prime orchestrates QuickPlay and Matchmaking, takes registrations
  from the game, answers the game locate request, and announces UTF-8 and
  converts text for older clients in shared rooms. The mocked UX goes live
  then without changing its look.
- Each finished feature's `OA_BONEYARDS_<FEATURE>_RELEASED` gate comes out,
  as QuickPlay's and the Locator's did on 2026-11-06.
- The login carries a version challenge from the server, which sends an
  out-of-date player away to update.
- Right-to-left scripts (Arabic, Hebrew) get bidirectional layout.
- The fold is done: a plain copy under `src/boneyards/`, without the
  project's history. The rule that keeps its words out of the rest of the
  engine, and the baselined purity findings, are a later change. The release
  lanes are retired.

## The work

| Phase | Work |
|---|---|
| 0 | Move the pin, follow the engine's fonts and Windows build, run the full ctest |
| 1 | The engine's standards: warnings as errors, sanitizers, check timing, nothing written into the game folder, "Exit to the system?" |
| 2 | The gaps against 3.1c and the master servers: the contents consoles' missing green background, disconnects, the network check, consoles kept between openings, the login name, the unit limit, server chat lines and ranks, the contents art, map retries, toolkit sounds, and checks through the screens |
| 3 | QuickPlay, Locator, Matchmaking and registration, working or mocked as above, gated to Admin and Beta Tester accounts until release (QuickPlay and Locator released 2026-11-06) |
| 4 | Every language |
| 5 | Gamepad and Steam Deck, mods, the session kept alive in the background, the remote test driver, the settings document |
| 6 | Release, when the maintainer says |

## Open questions

None. Every question raised in review is answered above; what waits is in
[Futures](#futures).
