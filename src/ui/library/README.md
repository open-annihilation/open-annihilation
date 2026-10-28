# Library

The Library's model (`oa/ui/library/library.hpp`, `oa::ui::library`): one
list of every package the player can get or has installed, mods, map packs,
language packs, mutators and brains together, searchable on the player's own machine and
honest about what each package does to the game. It turns plain inputs into
the Library's state: the entries and their states, which of them show, the
selection, the facts and buttons of the selected entry, and the footer's
download queue lines. It has no drawing, no input handling, no file, clock
or network access, and no app header: the app copies what it knows into
`Inputs`, and the Library screens lay out and draw what the model says.

The Library's screens (`oa/ui/library/screen.hpp`) lay that state out at
the three size classes, made only of the UI kit's components
([src/ui/kit](../kit/README.md)), draw it through the kit's painter, and
take the pointer, a finger, the keys, typed text, the wheel and a
controller. They read the model's visible entries, actions, facts, status
lines, bylines, playing note and queue lines, and decide none of its rules.
Nothing here hosts the screens, fetches or installs: a host lays the screen
out for its frame, draws it, passes its input in and acts on the actions it
returns.

The words both show are in `oa/ui/library/text.hpp`, English with `{place}`
holes, each a `constexpr std::string_view` there and nowhere else, so a
translator finds them together. Every word passes through `Library::text`
(`TextHooks`) before it is shown.

## Entry points

- `refresh(library, inputs)` rebuilds the entries from new inputs:
  - one entry per listing (registry, kind and key), joined with the package
    installed from the same registry, kind and key, and one per installed
    package no listing names (the player's own, or one whose registry is off
    or no longer lists it). Two registries that list one key are two
    entries. It then makes the base game entry (`base_game_id`): a mod, no
    registry, key `@base-game`. A package key is lower-case kebab-case, so
    it cannot hold `@`. That entry is not a package: it has no listing, no
    installed copy and no update, whatever a folder of the same key held;
  - its state: Update when an update note names it (blocked or not), else
    PLAYING for the mod being played, else INSTALLED, else Get;
  - whether Get or Update is allowed, and why not, the first reason that
    holds: a game is being played ("Not during a game"); the registry's
    downloads are off; for Get, an engine requirement this build does not
    meet, a base game other than `ta-3.1c`, or hacks the registry does not
    have or this build does not carry out (up to three named, then
    "· N more"); for Update, what its update note says. An INSTALLED or
    PLAYING entry is never blocked, so Roll back and Open folder stay. A
    blocked Get is listed with its reason, never hidden. A mutator that
    does not work with the mod being played is still listed, and Get and
    Update stay on: `not_compatible` is information, and the details name
    the mods it works with (`details_note`). An unmet prerequisite is
    recorded on `needs_missing` and does not block Get or dim the row. A
    brain that does not fit the game being played is still listed, and Get
    and Update stay on. `brain_unfit_reason` names the first failure: the
    engine range ("Needs Open Annihilation {range}"), then the mods it is
    made for ("Made for {names}", the base game as its own words), then an
    expansion that is off ("Needs {name}"). `not_compatible` carries that.
    The row says it in the lock colour. It does not block and it does not
    dim the row. The installed facts win, as a mutator's do;
    `works_with_played` reads the installed facts when they are set, even
    when that works-with list is empty (any mod), otherwise the listing's.
    An empty list or `*` works with any mod, `ta-3.1c` works while no mod
    is played, and a mod id works when it is the played mod and its range,
    read by the mutator manifest's matcher, holds the played version;
  - `built_in`, taken only from `Registry::built_in` (its Get and Update
    never ask the player first), `not_reviewed` for every other registry,
    the registry's name, and, on a Get entry, the name of another registry
    the same kind and key is installed from (`replaces_registry_name`).

  It keeps the tab, each tab's filter, the tag, search and selection. An
  open base game chooser stays open on the game it marks while the stored
  base game and expansions are the same, its rows read again from the new
  inputs, so a catalogue or a picture that arrives while the player
  chooses does not close it; a different stored choice closes it. The
  question asked before an expansion a playing mod needs goes off waits
  for its answer the same way, so its Turn off still turns the expansion
  off; a different stored choice drops it, and the host closes it. When
  the selected entry no longer shows, the first visible entry is selected
  and the details close. An update the installed copy has already reached,
  or a finished queue item whose catalogue release is above 0 and at least
  the note's, is not attached, so the entry is not an update and Updates
  does not count it. A release of 0 names none, and an item waiting until
  the player leaves the mod being played has not finished.
- `set_tab`, `set_filter`, `set_tag`, `set_query`, `select`,
  `move_selection`, `open_details` and `close_details` browse; each
  recomputes `visible`, the entries shown in their order:
  - the tabs are Active, Mods, Maps, Mutators, Brains, Keys, Languages, Updates
    and Registries, in that order. A Library opens on Active (`Library::tab`),
    which lists no entry and selects none: it shows the player's setup
    (`active_setup`, below). Registries lists the registries
    (`Inputs::registries`) and has no search, no filter and no package
    list. Mutators lists every mutator the player has or a registry lists,
    and nothing there switches one on. Brains lists the built-in computer
    player first, then every brain the player has or a registry lists.
    The built-in entry (`@built-in`) is not a package: it shows on Brains
    under All and Installed only, including when a search matches its name
    or "default", and on no other tab or filter, and it is left out of
    every count. It is marked Default unless `Inputs::default_brain` names
    an installed brain. It reads Built in where a package reads its version.
    While it is not the default it offers Use by default, and while it is
    the default it offers nothing. Keys lists OA default first
    (`@oa-default`), then every key layout the player has or a registry
    lists. OA default is not a package: it shows on Keys under All and
    Installed only, and it is left out of every count. While it is the
    layout in use it offers Edit keys…, and while it is not it offers Use.
    Layouts made for the mod being played come next. A player's own
    changes are a line on the layout they sit on, not an entry. Use asks
    first (`open_key_use`), where first: every game or only the mod being
    played; then, when the layout in use holds the player's own changes,
    Keep them on top or Start clean, and that either way they stay saved
    with that layout and come back when it is used again; then what moves,
    each command's key now and afterwards (`KeyMoves`, which the question
    asks of `Library::key_hooks` once as it opens, from the layout in use
    to the one named: `kept` with the own changes on top, `clean` without,
    a command on one of the player's own keys listed even when its key
    stays, marked "yours"); then one switch per helper, under the note that
    helpers act in every game. A layout that already holds the player's own
    changes keeps them: the question says so in place of the choice, and
    `key_use_keeps` is false, so Use never writes over them. Use's button
    names the answer to Where (`key_use_button`: "Use for {mod}" or "Use
    for every game"). Cancel writes nothing. The question's controls are
    `library.use.for.game`, `library.use.for.mod`, `library.use.keep`,
    `library.use.aside`, `library.use.helper.<name>`, `library.use.block`,
    `prompt.use` and `prompt.cancel`, and its labels `library.use.saved`
    (where the own changes stay), `library.use.moves` (one line a command,
    "Move: M → Z", "Patrol (yours): J → J", then "and {count} more") and
    `library.use.helpers`;
  - Mods, Maps, Mutators, Brains, Keys and Languages show their kind and Updates every kind, under
    the tab's own filter (`Library::filters`, `shown_filter`) and, when that
    filter offers it, the tag. A tag left from another tab or filter is kept
    and hides nothing until a tab offers it again. The filters are:
    All; New keeps the tab's ten listings (`newest_count`) with the latest
    `Listing::published` day, ties by name, and never a listing without a
    day, so a package's release and revision play no part; Installed keeps
    INSTALLED, PLAYING and Update; Updates keeps Update. The base game
    entry shows on Mods under All and Installed only, including when a
    search matches its name, and on no other tab or filter. New never
    holds it. Each tab keeps its filter when another is shown: Updates
    starts on Updates and the others on All;
  - with no search: under New, the latest day first, then by name; under
    the others, updates, then the other installed entries, then Get
    entries that can be installed, then blocked ones; within each, by name
    folded to lower case, built-in registries first, then registry id;
  - with a search (`rank`, in `search.cpp`): every word must be found; a
    word's class is the best field it is in: the name, a tag whose id or
    shown label starts with it, the author or publisher, the summary. A
    label is the word of one of the eight ids (`statistics`, `economy`,
    `commanders`, `new-units`, `disable`, `gameplay`, `ai`, `maps`),
    matched exactly; any other id, including a different case of one of
    those, is searched as written and is not given that word. Entries order by the
    worst class among the words, then the sum of the classes, then names
    that start with the first word, then as with no search. The search is
    kept to 64 bytes, cut at a character boundary. Whenever the list keeps
    the base game entry, it is moved to the front after that order, and
    the built-in computer player is moved to the front of Brains the same
    way, and OA default is moved to the front of Keys. A brain's tag is matched by `brain_tag_label`. A key layout's tag is matched by `key_tag_label`. "default", and the
    translated Default, find the built-in player and the installed brain
    that is the default; that word is not one of the row's chips. "default"
    also finds OA default, and so do "built" and "in", including under a
    translation of Built in. The row still shows its own summary;
- `tags_in_tab` counts the tags of the entries a tab shows under its
  filter. A mutator's tags are the ids on its facts, the listing's before
  the installed copy's, and not the listing's own tag list as well. A
  brain's tags are the ids on its facts, the installed manifest's when
  it is installed and otherwise the listing's. On
  Mutators the eight ids come first, in the order statistics, economy,
  commanders, new-units, disable, gameplay, ai, maps, and any other id
  after them in alphabetical order. On Brains the nine ids come first,
  classic, economy, rush, turtle, air, naval, learned, teaching and
  experimental, and any other id after them. On the other tabs the ids stay in
  alphabetical order. A mutator's chips and detail pills, and the Mutators
  tab's filter pills, pane and menu, show the word of an id matched
  exactly (`tag_label`). Brains uses `brain_tag_label` in those places
  and gives a brain's tag no mutator icon. Economy is the mutators' word.
  A mod's or a map pack's tag stays as written on
  every tab, so `economy` stays `economy` and `maps` stays `maps`, and a
  different case such as `Economy` stays as written. The search still
  matches a mutator's label, and a brain's. The list applies a stored tag only when that count includes it;
  `filter_count` counts the packages a filter keeps on the tab
  shown before the tag and the search, so no count moves with them, and
  leaves the base game entry, the built-in languages and the built-in
  computer player out of every count, and
  `filter_caption` writes it as the filters show it, "Installed (4)"
  (`words::filter_with_count`); `update_count` counts the updates that can
  be installed, the number on the Updates tab and the OA button, and
  `updates_shown` the Update entries the list shows, the Updates tab's
  "{n} updates".
- `active_setup` describes the Active tab from the inputs: the mod being
  played, the installed mod marked playing (its listing's name, badge and
  author when a registry lists it) or else `Inputs::playing_mod`, with its
  version ("Highland 0.4.0") and the release the tile states
  ("0.4.0 (rev 1)"), or No Mod ("No Mod", "Total Annihilation as it was
  made"); the language in use, its name
  in itself and in English, its pack, version, revision and coverage
  (`Inputs::language`: "Polski 1.0", "Coverage 92%") or the language the
  game holds itself ("English · built in", "Built in"); and each installed
  map (`Inputs::map_packs`), ordered by folded title and then folded pack
  name, with its players ("4 players"), size, description, minimap when
  one has been decoded, and the link "From Alpha Pack" when the pack holds
  several maps (empty when it holds one). The pack's entry
  is the map-pack entry of the same key and registry, or, when the pack
  names no key, the entry of the pack's name.
- `entry_actions`, `facts`, `status_line`, `byline` and `playing_note`
  describe an entry:
  - the buttons Turn on first, when `content_lines` says one press would
    turn on every required expansion that is off (it writes the Active
    tab's draft, not the preferences), then Get or Update (Cancel while
    queued, Retry after a failure that can be retried), Play now, or
    Switch off on the mod being played, Roll back, Uninstall, Open folder
    and Homepage, each with whether it is enabled: Play now and Use now
    are off when the package cannot run on the pending choice, Play now,
    Switch off and Roll back are off during a game, and Play now and
    Switch off while `Inputs::mod_from_command_line` says `--mod-dir` or
    `--base-game` chose the mod. Get stays on. A package that cannot run
    stays in the list. A mutator offers Get or Update (Cancel while queued,
    Retry after a failure that can be retried), then Check with this mod
    when an installed copy's verdict is set (off during a game and while a
    check runs), then Uninstall, Open folder and Homepage. It never offers
    Roll back, Play now, Switch off, Use now or Turn on, and an incompatible
    verdict does not turn Get or Update off. A brain offers Get or Update
    (Cancel while queued, Retry after a failure that can be retried; each
    stays on when the brain does not fit), then Use by default when an
    installed copy is not the default, then Check with this mod when an
    installed copy's verdict is set (off during a game and while a check
    runs), then Uninstall, Open folder and Homepage. The host's check loads
    the brain on the Library's worker and runs fits against the game being
    played. It does not seat the brain. Use by default writes
    `open-annihilation.brain.default`, or erases it for the built-in player;
  - the facts Installed, Latest, Author, Size, Base, Games, Needs, Hacks, Rules,
    Maps, Coverage and From, in a long and a short form. Installed and
    Latest are the installed copy's and the listing's version and revision
    (`release_text`: "4.8 (rev 3)", "1.0" for a revision of 0, "rev 3"
    without a version), and Author is the listing's author, else its
    publisher, else the installed manifest's (`Installed::author`); each is
    left out when it has nothing to say. Games and a second Needs name the
    package's base games and required expansions when it declares them
    (`content_lines`); the engine's Needs stays first. An update's Rules fact says in words whether
    it changes the game's rules and which saved games stop loading
    ("9c1f 4e0b → 51d2 07aa · changes the game's rules; saved games from
    revision 3 won't load under revision 4"), since a built-in registry's
    update asks no question that would say it. From says "Not reviewed by
    the OA team" for an added registry. A mutator leaves out Size, Base,
    Games, Hacks, Maps and Coverage. After Author it lists Works with
    ("Any mod" when the list is empty or names `*`, marked when the mutator
    does not fit the mod being played), Checked (an installed copy whose
    verdict is set: the played mod's name, or "No Mod"; a bad mark is the
    danger colour), one Needs per prerequisite (the name alone when it names
    no range: "Gatehouse: not installed", or "Gatehouse ✓" when it is met)
    and then the engine's Needs,
    Options, Changes, Rules ("Changes the game: unranked on Core Prime",
    shown whole) and From. `verdict_of_record` maps a dry-run record; a
    compatible summary counts the collections ("changes 2 units"). A brain
    leaves out Base, Games, Hacks, Maps, Coverage, Options, Changes and
    Rules. After Author it lists Levels (the level names joined as "Easy,
    Medium and Hard"), Works with ("Any mod", or the names joined with "or",
    marked when the brain does not fit), Needs (the engine's, then one per
    required expansion: good when that expansion is on, a warning when it is
    off), Full awareness only when the brain sees every unit (a warning),
    Network only when it carries a learned network, and Checked when an
    installed copy's verdict is set (the same words as a mutator's, a
    compatible one reading "it fits this game"), then Size and From. The
    built-in entry lists only Levels ("Easy, Medium and Hard") and Works
    with ("Any mod"). A key layout says what it does before which copy it
    is: Changes ("12 commands"), Your changes (the player's own changes on
    it, up to three by command and key, "Patrol on Q, Cloak on K and 2
    more", from `KeyLayoutFacts::own_list`, or their count when the host
    names none), Asks for (its helpers), Works with (its mods, then its
    keyboards and the interface it was drawn for: "Any mod · any keyboard",
    "Ridge · ISO keyboards · the left-click interface", marked when it does
    not fit the mod being played), the engine's Needs and Not in this
    version, then Installed, Latest, Author, Size and From. OA default
    lists Works with alone;
  - the byline names the author, or the publisher when there is no author.
    A listing that names neither shows nothing more. The byline does not
    change an entry's state, order or search rank;
  - the note on updating the mod being played ("Finishes when you leave
    Ridge."), after "The command line chose this run's mod." on every
    installed mod while the command line chose it;
  - the status line says "No oamod.yaml present" for a mod folder without
    one (`Installed::without_manifest`), installed or played. A mutator that
    is not installed names its version, including a revision of 1
    ("Not installed · 2.0 · rev 1"). A brain to Get names its version the
    same way. An installed brain reads Installed, and Installed · Default
    while it is the default. The built-in entry reads Built in, and Built
    in · Default while it is the default. An update's line and a queue's
    line stay those lines while a brain is the default. The installed key
    layout in use reads In use for every game, or In use with {mod} while
    it is used only with the mod being played.
- `entry_summary` gives the words under the details' status line: the
  listing's summary, else the installed manifest's description
  (`Installed::description`), as for a mod of the player's own.
- `can_check_now` says whether the header's refresh icon can be pressed: a
  registry is on that it can ask (`Inputs::can_check`), none is being asked
  (`Inputs::checking`), and no game is being played. While one is being
  asked the icon turns and its text is "Refreshing". The check after a
  list was read does not disable it.
- `queue_lines` writes the footer: what the queue is doing now, what comes
  next, and a line for a small screen. After an update that changed the
  rules, it says so and that Roll back restores the previous revision.
- `text.hpp` writes sizes in binary units (`size_text`: "41.0 MB",
  "188 MB"), download progress, ages, rules hashes
  ("9c1f 4e0b"), versions ("4.8 · rev 3", "1.2 · 2 maps"), a release as
  the facts give it (`release_text`: "4.8 (rev 3)"), engine
  requirements, what an update changes from and to (`change_texts`:
  "revision 3" and "revision 4", or the versions, and "the old copy" or
  "the new copy" for a side no version or revision above 0 names), and the
  names of tabs, filters, states and buttons.
- No text says "Revision 0" or "rev 0". A catalogue that names no
  revision, as Core Prime's does for language packs, leaves it 0: the
  status line then says "Update available · 3.4 MB to download" for an
  update of the same version (`status_update_available`); "Revision
  {revision}" is written only for a revision above 0 of the version
  installed (`status_line`), and the update question's revision heading
  only when both revisions are above 0.

### The update question

`update_question.hpp` holds the question asked before an update that may
stop saved games loading, and the one place its rule lives. The Library's
Update and Update all go through it (the app's `Runtime::request_update`),
so the screens and the tests share its words.

- `asks_before_update(entry, update)` is true only for a mod
  (`Kind::mod`) from a registry the player added (`!Entry::built_in`, which
  `refresh` takes from `Registry::built_in`) whose update changes the rules
  (`UpdateNote::Rules::changes`) or whose rules cannot be known
  (`unknown`), since saved games load only under the same rules hash. It is
  false for an update that keeps the rules, for a language or map pack,
  whose rules are always the same, and for every built-in registry's
  package: downloads from the official registry never ask. Their rules
  change shows on the row and in the Rules fact instead, before Update, and
  in the queue line after it.
- `update_question(entry, update, playing)` writes the question, or nothing
  when it is not asked: "Update {name}"; the heading "{name} 4.8 ·
  revision 3 → 4" for a new revision, or "{name} 4.8 → 4.9" for a new
  version; the body, "Revision 4 changes the game's rules. Saved games from
  revision 3 won't load under revision 4. Revision 3 is kept, and Roll back
  brings it back.", or for an unknown rules hash "OA can't tell whether
  revision 4 changes the game's rules. …", with "It finishes when you leave
  {name}." for the mod being played; and the facts Rules ("9c1f 4e0b →
  51d2 07aa", or "unknown") and Download (`size_text`).
- `update_all_question(entries)` writes Update all's one question over the
  entries whose updates ask, or nothing when none does: "Update all", and
  "N updates change the game's rules: …", naming up to three, then "and N
  more" ("1 update changes …" for one). The others start at once.
- `update_prompt(question)` makes the kit question that asks it: the
  heading, the body and each fact ("Rules: …") as its paragraphs, then Not
  now (`not-now`) and Update (`update`), Not now marked at first and
  answering Escape, B and N, Update with the primary look. Its word is the
  kit's default, so automation lists `oa.prompt.not-now`,
  `oa.prompt.update` and `oa.prompt.body`.

### The questions about the mod played

`mod_questions.hpp` holds what the app asks and tells over the Library
about the mod the game plays, as kit questions whose word is the kit's
default (`oa.prompt.<id>` to automation):

- `switch_off_question()`: Switch off's **SWITCH MOD**, "Switch to No Mod
  now? The game reloads its data for the new mod and returns to the main
  menu. Your other settings are kept.", with Cancel (`cancel`), marked
  and answering Escape, B and N, and Switch (`switch`), primary, which Y
  answers;
- `switch_refused_note(reason)`: why Play now could not switch, under the
  same title, with OK (`ok`);
- `roll_back_question(title, from, to, playing)`: Roll back's **ROLL BACK
  MOD**, "Roll back {title} to {to}? {from} is kept in its place, and Roll
  back again brings it back.", with "The game reloads its data for the mod
  and returns to the main menu." for the mod the game plays, Cancel
  (`cancel`) marked and Roll back (`roll-back`) primary.

### Uninstalling

`uninstall.hpp` holds Uninstall's rules, its question and its notices. The
app removes the folder (`Runtime::library_uninstall`,
`package_install::uninstall_package`).

- `entry_actions` shows Uninstall on every installed entry, after Roll back
  and before Open folder, never as the main button. `uninstall_off_reason`
  turns it off, and says why, for a map pack while a game is played ("Map
  packs can't be uninstalled during a game.") and for any package with a
  queue item that has not failed or been installed ("It is in the download
  queue: Cancel it before you uninstall it."). `details_note` adds that
  reason after `playing_note`'s, and `turn_on_note`'s after that, as the
  note under the buttons. `turn_on_note` is empty unless Turn on is showing.
- `uninstall_step(library, entry)` says what pressing it leads to: nothing
  while it is off, a notice for the mod being played
  (`Installed::playing`) and for a language pack the game shows its words
  from (`Installed::in_use`), and the question for every other installed
  package.
- `uninstall_question(entry, folder)` makes the kit question: "Uninstall
  {name}", "Uninstall {name}? Its folder, {folder}, is removed." with ",
  and with it the copy Roll back keeps." when a copy is kept, then "Get
  installs it again." for a package a registry lists, or "No registry
  lists it, so the Library cannot get it again."; Cancel (`cancel`) marked
  and answering Escape, B and N, and Uninstall (`uninstall`) primary. Its
  word is the kit's default, so automation lists `oa.prompt.cancel` and
  `oa.prompt.uninstall`. `uninstall_question(library, entry, folder)` is
  the same question, and for a mutator other installed mutators need it
  adds, before that last paragraph, that they stay installed and cannot
  be switched on until it is back. For the default brain it adds, before
  that last paragraph, "It is your default computer player: new computer
  seats go back to the built-in player." The host uses this form. After
  the folder is removed, uninstalling the default erases
  `open-annihilation.brain.default`. The answer and the removal are
  unchanged.
- `uninstall_notice(entry, step)` makes the notice for a package in use,
  and `uninstall_failed_notice(name, problem)` the one for an uninstall
  that removed nothing (busy, changed, a link, outside the player's
  folders, or not moved), each with one OK (`ok`).

### The screens

- `library_layout(library, state, frame, fonts)` returns the display list of
  the Library in a frame's window (`window_size`: 480 by 324, 720 by 486 or
  960 by 600 points, never larger than the frame), from its top left corner:
  - **The Active tab**, at every class, shows under the tabs, with no
    search and no filters. At every size the first row is BASE GAME and
    LANGUAGE and the second is EXPANSIONS and MOD, then KEYS, then the MAPS
    heading. On a small window KEYS is a heading and one line, the layout
    in use and Change…, with no badge, so the expansion rows keep their
    text when the page scrolls to the maps. On a larger window KEYS is a
    full-width tile.
    The base-game tile has Total Annihilation's icon, from the program in
    the game folder that describes itself as Total Annihilation,
    nearest-neighbour and centred in the badge square. When that program
    cannot be read, the tile draws the icon the Library ships, the same
    one for Total Annihilation and the demo. The letters "TA" stay only
    when that picture cannot be drawn. The tile also has the name,
    the version and, when the inputs name them, the source and the file
    check, and Change… (`library.active.change`) at the bottom right.
    The expansions tile has one pill `library.active.expansion.<id>` for
    each expansion of that game. One that is not copied and that a registry
    offers keeps the name and carries the download arrow at the right,
    inside the pill. The whole pill is the press: the same control asks
    for the download, cancels it, or tries again. While the command line
    chose the mod played, a pill that is on and whose press would unload
    that mod (it needs the expansion, or one that goes off with it) is
    locked (`ExpansionPill::locked`), by the rule `press_expansion`
    refuses that press by: the pill keeps its face under the kit's locked
    fade, its item is marked locked, and it takes no press, key or Tab.
    Under the pills the padlock and `ActiveContent::lock` ("The command
    line chose this run's mod, which needs Campaign.", naming every locked
    pill) stand in the lock colour, wrapped beside the padlock, above the
    hint. No pill is locked during a game, while `--content` named the
    choice or while the mod is chosen in the game. The mod tile has a large badge, the name,
    the version and revision and the author, with Playing as a pill on the
    title's line at the top right, or No Mod with an em dash and "Total
    Annihilation as it was made" and no pill.
    While a mod is played, Unload mod
    (`library.active.unload`, kind `button`, text "Unload mod") sits at the
    mod tile's bottom right. It is accent while it can act and plain while
    disabled, by the same rule as Switch off, and a press asks for
    `Action::switch_off` of the mod being played. The language tile has the
    pack's badge or a language mark, the name in itself and in English,
    the version and revision and the coverage, or Built in as a neutral
    pill on the title's line. Each installed map on a small window is one
    line: its name, its players and its size ("Twin Rivers · 2 players ·
    12x12"). On a larger window each map is a tile with its minimap, its
    title, its players and size, its description cut to one line, and a
    link `library.active.from.<n>` that opens the Maps tab with that pack
    selected and its row in view. A pack of several maps keeps the link on
    a From line ("From Core Map Pack") under the tile's lines, and at the
    right of the one line on a small window. A pack of one map leaves that
    line out and the title is the link, its text the title, drawn in the
    accent and lighter while hovered. A minimap is
    fitted in its square, centred, and the letterbox is the list's well
    colour. Badge, title and lines start
    at the top of the tile, with the same padding above them and below the
    last line or the button. The four tiles stand two by two at every size.
    Compact lists the maps on one line each; Regular and Large put the
    titles on one line and the tiles the same height; Large puts the maps
    in two columns of equal height when more than one is installed. The mod label stops above Unload mod, the
    language label covers its tile, and each map's label covers its title
    line, or the players-and-size line when the title is the link, and on
    a small window the part of the line after the title when that title is
    the link, so it does not meet the link. On Regular and Large the maps
    pane scrolls on its own when the maps are taller than the room under
    the heading, and the wheel over a tile above the maps does not scroll
    them. On Compact, when the tiles leave the maps fewer than four rows,
    the page scrolls as one, the tiles then the maps, by the wheel, a
    finger, the scroll bar, the page keys, Home, End and the right stick.
    Once the maps are in view, four rows fit. Focusing a map link, or the
    maps search, scrolls that control into view. The expansions hint wraps
    onto as many lines as it needs. The labels take
    no press. Unload mod does, while it is enabled, and so does a map's
    link while its line shows; Tab and the arrows reach both. A pack of one map
    wears that map's minimap on every row and in the details. A pack of
    several keeps its badge or its letters, and on the Maps tab a pack
    with no badge shows the first map's minimap. The details show one
    large minimap, or a grid, with each map's name, players, size and
    description. While a minimap has not been decoded the square is the
    well colour with an outline, and a row shows a dashed square.
    Compact's footer holds no Details there;
  - **The Registries tab**, at every class, shows under the tabs, with no
    search and no filters, one card for each registry, one under another
    with `card_gap`. A card has its avatar in the `list_row_badge` square:
    the registry's icon when `Registry::icon` has pixels, otherwise the
    initials (`registry_initials`, from the short name, on the accent for a
    built-in registry and on `colour::switch_idle` for one the player
    added). The initials stay while the picture is missing. A registry with
    `shipped_logo` draws the logo the game ships and is never fetched. Its
    heading (the title, or the short name when the title is empty), its
    address, when it was last read (`registry_refresh_line`: "refreshed 5
    min ago", "never refreshed", or "couldn't reach · 2 h ago"), its
    description when it has one, and its subtitle (`registry_subtitle`:
    how many packages, "not reviewed" for one the player added, and how
    many mirrors). The key icon is the kit's key glyph in the icon square
    (`state_icon_side`) at the right of the title band; resting on it
    shows a card headed "Signing key" or "Signing keys", each fingerprint
    and full public key, and "built in" or "added
    by you", and pressing it asks for nothing. REFRESH, a row's small
    secondary button with the kit's refresh arrow before its word, sits
    beside the switch and asks the host to refresh that registry
    (`Action::refresh_registry`); it is greyed while the registry is off
    or already being asked, and the refresh mark beside it turns while it
    is. When the ask reads a catalogue the mark shows a check
    (`ButtonLook::check_ms`) and then the arrow; a failed ask leaves the
    arrow. The Off/On switch is the kit's switch. A built-in registry has a
    neutral Built in pill on the title's line, at its right. REMOVE and
    ADD MIRROR… sit under the subtitle for one the
    player added. A note under the last card (`registry_note`) names the
    built-in registries by their short names, and a refusal
    (`Inputs::registries_notice`) stands over it in the lock colour. The
    cards and the note scroll as the details do. ADD REGISTRY… sits at
    the top right, above the cards, and joins Tab's order after them; the
    footer holds Close and no Details. During a match the padlock and
    "Choose from the main menu." stand where ADD REGISTRY… was, and ADD
    MIRROR… gives way to the same lock; the switches, REFRESH and REMOVE
    stay. A switch, REFRESH, REMOVE, ADD MIRROR… and ADD REGISTRY… ask
    the host (`Action::set_registry`, `refresh_registry`,
    `remove_registry`, `add_mirror`, `add_registry`); the host applies a
    switch and a removal at once, and asks the content service to refresh
    that one registry;
  - The shell takes Kit 2's sizes at every class (`Window` in `layout.cpp`):
    two spaces round its parts and between its panes (`Window::gutter`, 8,
    12 and 16 points, a space less than Kit 2's window padding, which the
    base game chooser and Use's question keep), the tab strip standing on
    the header's rule and a space between the last part and the footer, a
    tab strip of `Metrics::tab_strip_height`
    (32/38/44), the search fields (the Active tab's maps search too) and
    Compact's filter drop-down of `control_height` (28/34/38), as Settings'
    buttons are, the filter pills of `filter_chip_height`
    (24/26/28, each wider by what it gains in height), buttons of
    `control_height` (28/34/38) in the footer, on the Updates line and over
    the Registries cards, and the footer itself, a rule and a band of
    `window_footer_height` (48) at Regular and Large, and at Compact the
    controls with a space above and below (36), with its stacked parts a
    space apart, so the list keeps five of its 36-point rows in a 480 by 324
    window. Regular and Large stack them `row_gap` apart. The header is
    Kit 2's window header (`window_header_height`, 36, through
    `HeaderLook::height`) at Regular and Large; Compact keeps the kit's
    Compact band (26), since ten points more would cost its list a row and
    leave the Active page's lowest controls over the footer's, where no
    arrow passes them. When the tabs' names do not all fit, the gaps round
    them close no further than a space either side, so each tab reads
    apart, and the strip scrolls to the rest with its arrow.
  - **Compact** shows a list page: the header (the mods icon, the plates
    over drive bays, "LIBRARY", this build's version, and a refresh icon
    at the top right; "offline" in the lock colour beside it when the last
    refresh could not reach a registry),
    the tabs with their full names and All counts in brackets ("Active",
    "Mods (11)", "Maps (2)", "Languages (2)", "Updates (15)", "Registries (2)";
    the strip scrolls sideways, with an arrow at an end, when the names do not fit; it shows
    the open tab when that tab opens, and then stays where an arrow, the wheel or a drag
    leaves it, from the first tab to the last), the search
    field and the filter
    drop-down (All, New, Installed and Updates with their counts, "All
    (11)", then "Tag: balance (3)" for each tag of the tab), on the
    Updates tab "{n} updates" and Update all on a line over the list, the
    list of Kit 2's 36-point rows (`Metrics::row_height`: the name, then the version, size and an
    added registry's name, and the state's icon: a download arrow for Get,
    a circled up-arrow for Update, a circled check for INSTALLED and
    PLAYING, dimmed when Get or Update cannot act), and the footer (the queue's short line with an
    8-point bar under it while downloading, Details and Close, Close drawn as the
    main button). While
    `ScreenState::details_page` and the model's `details_open` are both set
    it shows the selected entry's details page instead
    (`details_page_layout`): its name and version in the header, the status
    line, the byline, the summary in at most three lines, the package's tags
    as inactive pills under it, the short facts in label and value columns,
    the playing note, and in the footer the first three actions, More
    holding the rest, and BACK. BACK returns to the list and keeps its tab,
    filter, search, scroll and row. The list page's button stays CLOSE;
  - **Regular** shows "OPEN ANNIHILATION LIBRARY" and "v0.8.0", the same
    refresh icon, the tabs with their All counts and a 240-point search
    beside them (on a row of its own under them when the tabs do not all
    fit beside it, so that none hides beside the search; the same at
    Large), a row of pills (All, New,
    Installed and Updates with their counts, and the tags while they fit,
    the chosen one in the accent ground and the others in the muted ground,
    Tags holding the rest; on Updates, "{n} updates" and Update all on a
    line over the list), a 400-point list of
    Kit 2's 40-point rows (the name and byline, the summary, the version over the
    size, the state's icon), the details (a 40-point badge beside the
    name, its state's icon and the status line, the summary in at most six lines, the package's tags as inactive pills under it, the long facts,
    the playing note, which scroll, and under them the actions three to a
    line at Kit 2's control height, 34 points, 38 at Large, in a bar at the
    pane's foot that does not scroll), and the footer (both queue
    lines, the 8-point bar under the first while a download runs, and CLOSE);
  - **Large** shows Regular's header, tabs and search, a 180-point filter
    pane (a nav list of All, New, Installed and Updates with their counts,
    the heading TAGS and the tab's tags with their counts, then the heading
    WORLD and the world tags, each by its world's name) in place of the
    pills, every entry in a view that scrolls with the wheel, its bar, a
    finger and the arrows, with no drop-down; Tab stops once in it, on the
    entry the focus last had there or else the chosen one, so the next Tab
    reaches the list and Shift+Tab comes back, while Up, Down, Home, End
    and the page keys walk every entry and the view follows; a newly chosen
    entry is brought into view (`ScreenState::filters_scroll`,
    `filters_chosen`, `filters_stop`). Then a 400-point list of Kit 2's
    44-point rows of three lines (the tags
    as chips under the summary), on Updates under "{n} updates" and Update
    ALL, and the details.

  A key layout's row says what it does: its name and author, its summary,
  and one state in words where another kind shows an icon, In use (on the
  accent), Built in, Installed, Update, or Get with its size ("Get · 1 KB",
  which asks Get as a row's arrow does), with no version or size beside it.
  A layout installed from its own file, which no registry lists, takes its
  author and summary from its manifest, at every class.
  At Regular and Large the player's own changes on it sit on the chip line
  in the lock colour ("2 changes of your own"), with Made for and its tags;
  Compact keeps them on the line under the name. Its details draw the
  keyboard picture as wide as the details: beside the list and on
  Compact's page alike the main block alone (`KeyboardLook::main_block`),
  the changed keys filled with the accent under their legends, every
  legend drawn (in the small font where the regular one is too wide for
  its key), unless a changed key lies outside that block, when it draws
  the whole keyboard with those keys marked. The details say the state once:
  the chip says it in words, "In use for every game" or "In use with {mod}"
  for the layout in use, and the status line under the name is left out
  when it would say the same (`library.status` then reads the chip's
  words).

  Use's question is a dialog a notice wide (the whole window at Compact,
  inside half its padding), as tall as its parts: each section numbered
  ("1 · Where"), its two choices the kit's radio rows at Kit 2's control
  height, the marked one with its dot, on the list's selected face with
  the accent border; What
  moves, a command a line with its keys as chord chips either side of an
  arrow, two columns at Compact; the helpers in a box of the lock colour's
  tint; and Cancel and Use at the foot. Where the window is short, What
  moves keeps fewer lines and counts the rest, then each note keeps one
  line, then What moves is left out.

  A badge is the package's picture, or its letters (`badge_letters`: a
  word's first three, or the first of up to three words) in the kit's ink
  for the accent (`colour::on_accent`) on `colour::hint`, which reads at
  every class, at 28 points in every class's rows and 40 in the
  details. A mutator's picture is its listing's
  badge, or the installed copy's icon when the listing has none. With no
  picture, the badge is the icon of its first tag on that same hint ground
  when the id is one of the eight, and its letters when the id is not or it
  has no tag. The listing's tags are used when that list is not empty, and
  otherwise the installed manifest's. Large's mutator rows carry those tags
  as chips under the summary, then "Needs {name}" and "Not compatible with
  this mod" in the lock colour. When the row cannot hold every chip, those
  two stay and the tag chips are the ones left off. Compact's mutator rows
  keep the version on the second line and, after the title, "· needs {name}"
  or "· not compatible with this mod" in the lock colour, the missing
  mutator first when both apply. Neither dims the row, and Get stays on.
  A brain's row at Regular and Large, and on the Brains tab at Regular
  where the row is the large form, carries Default on the default entry,
  Full awareness, its tags, and, when it does not fit, the unfit words.
  Those words use the lock colour. At Compact they follow the title as
  "· {words}" in the lock colour. The built-in row reads Built in where a
  package reads its version. An unfit brain is not dimmed, and Get stays on.
  On the Mutators tab the Regular tag pills, Large's filter pane and the
  details' tag pills show those eight icons beside the label. A tag outside
  the eight draws no icon, and a mod's, map pack's or language's tag never
  does. Compact's mutator details page puts the 40-point badge beside the
  name, and the header stays Library and Mutators; a mod's Compact details
  page is unchanged. A fact marked bad is drawn in the danger colour. The Rules and From facts are shown
  whole at every class; the other facts take at most two lines at Compact
  and three in the details pane. The list and the details scroll, each with
  the kit's scroll bar while it holds more than it shows. A press on a
  bar, the mouse's or a finger's, holds it: on the thumb it grabs the thumb
  at the row pressed, on the well the thumb's middle jumps to the pointer,
  and the thumb then follows the pointer until the release. The bar is
  drawn held meanwhile, and the press reaches nothing under it.
- `draw_library(canvas, list)` draws a display list with `kit::paint`.
- `library_pointer`, `library_key`, `library_text`, `library_wheel`,
  `library_command` and `library_stick` take input in the form the OA
  layer's screens take it (`ScreenResult`: the action for the host, the
  entry it is for, and whether to draw again):
  - **Keys** (`kit::Key`): Tab and Shift+Tab follow the declared order: the
    tabs, the search, the filters, Update all and the list, the details'
    tag pills and actions, the footer's buttons (on Active, the tabs and the footer's
    buttons alone; on Registries, the tabs, each registry's key icon,
    REFRESH, switch, REMOVE and ADD MIRROR…, ADD REGISTRY…, and the
    footer's buttons); on Compact's details page, the tag pills, the
    actions, More and Back. The arrows move by where controls sit (`kit::key`). On
    the list, Up and Down move the selection and keep the focus there, and
    past its first or last entry the focus leaves it, as it leaves a kit
    scroll area; Page Up and Page Down move by the rows shown, Home and End
    to the ends; on Active and on Registries, the page keys, Home and End
    scroll those lines.
    Enter on the list opens the details page at Compact and moves the
    focus to the first enabled action at Regular and Large.
    Escape clears a search that has the focus and holds words, else leaves
    the details page, else asks to close. On the search, typed text and the
    editing keys search as they are typed. An open drop-down takes Up,
    Down, Page Up, Page Down, Home, End, Enter, Space and Escape.
  - **Commands** (`ScreenCommand`), which a host maps: `find` (Ctrl+F, Cmd+F on
    macOS, `/` outside the search, the controller's Y), `next_tab` and
    `previous_tab` (Ctrl+Tab and Ctrl+Shift+Tab, RB and LB), `back` (B: the
    details page to the list, else close) and `first_action` (X: the
    selected entry's Get, Update, Cancel or Retry when it is first and
    enabled). The controller's D-pad and left stick are the arrows, its A
    is Space, and its right stick (`library_stick`) scrolls the pane that
    holds the focus.
  - **Pointer and finger**: a press on a row selects it, and at Compact
    opens its details page; no press, double press or tap on a row gets,
    updates or installs anything, but for its icon: a row's download arrow
    is its Get and its circled up-arrow its Update, a control of their own
    tried before the row, which select the row and ask for that action
    while the details' first button is that action and enabled, and take
    no press otherwise (blocked, queued or failed), the press reaching the
    row. A finger reaches the nearest control
    within its reach (`kit::finger_down`); a finger that moves farther than
    its reach in the list or the details scrolls that pane and presses
    nothing. A press on tabs that do not fit that moves sideways, three
    points for the mouse or past a finger's reach, drags the strip with the
    pointer (`kit::tab_scroll_by`) and opens nothing. A press outside an
    open drop-down only closes it. The wheel over the tab strip scrolls the
    tabs when they do not fit, and otherwise scrolls the pane under the
    pointer, or an open drop-down's items.
- Every control is named for automation (the OA layer lists each as `oa.`
  and its name):
  - `library.tab.active`, `library.tab.mods`, `library.tab.maps`,
    `library.tab.mutators`, `library.tab.brains`, `library.tab.keys`,
    `library.tab.languages`, `library.tab.updates` (every tab after Active
    reads its title and its count in brackets: "Updates (2)"; Active stays
    "Active"), `library.tab.registries`; `library.tab.scroll-left` and
    `library.tab.scroll-right` when the strip has more tabs that way;
    `library.search`;
  - on the Registries tab `library.registry.<id>` (its halves `.off` and
    `.on`), `.key` (its text the fingerprints, one a line) and `.refresh`,
    and for a registry the player added `.remove` and `.add-mirror`; the
    labels `.address` and `.refreshed`; `library.add-registry`; the labels
    `library.registries.note` and `library.registries.notice`;
  - `library.filter` (Compact's drop-down, whose items, while it is open,
    are `library.filter.all`, `.new`, `.installed`, `.updates` and
    `.tag-<tag>`, a world tag's text "World: ice (3)"), or the pills or nav entries `library.filter.all`,
    `library.filter.new`, `library.filter.installed` and
    `library.filter.updates`, their text the filter and its count ("All
    (11)"); the tags' pills or nav entries `library.tag.<tag>`, and
    Regular's `library.tags` (Tags, its items `library.tags.<tag>` while it
    is open, the world tags last under the heading WORLD, which is not named,
    marked or pressed); a tag under the description is
    `library.detail.tag.<tag>`, an inactive pill, and pressing it chooses
    that tag as the filter row does. A world tag (`world_tag`, `world-`
    and a name) shows its world's name wherever a tag shows (`world_name`:
    `world-ice` is "ice"), comes after the other tags (`tags_in_tab`), and
    a search finds it by that name or by the whole tag;
  - `library.list` (the list, kind `list`, no text), and one control for
    each row that shows, kind `list_item`, its text the entry's name:
    `library.row.<registry>.<key>`, or `library.row.<key>` for a package of
    the player's own, each word lower-cased with other characters as
    hyphens; a Get or Update row's icon, after it, is
    `library.row.<registry>.<key>.get` or `.update`, kind `button`, its
    text "Get" or "Update", listed disabled while it takes no press, and
    never in Tab's order;
  - `library.action.get`, `.update`, `.update-all`, `.cancel`, `.retry`,
    `.roll-back`, `.play-now`, `.switch-off`, `.use`, `.edit-keys`,
    `.open-folder` and `.homepage`; `library.keys.picture` (kind `area`,
    the keys the picture marks); on Compact's
    details page More is `library.action`, whose open items are the
    actions after the first three under those same names;
  - `library.refresh` (the header's icon, kind `button`, its text "Refresh",
    or "Refreshing" while the registries are being asked; disabled while no
    registry can be asked, while one is being asked and during a game),
    `library.details`, `library.back`, `library.close`;
  - the labels `library.status`, `library.queue`, `library.note`,
    `library.from` and `library.rules`: controls of kind `area` that are
    neither enabled nor focusable, whose text is the words shown (the From
    fact's long value, and the Rules fact's value whole, short at Compact
    and long at Regular and Large);
  - on the Active tab the labels `library.active.base` ("Total
    Annihilation · 3.1c"), `library.active.expansions` ("EXPANSIONS · 5 OF
    6 ON"), `library.active.mod` ("Highland 0.4.0", or "No Mod"),
    `library.active.language`
    ("Polski 1.0") and `library.active.map.<n>`, from 1 in the order shown
    ("Ridgeline · 4 players · 16x16"), the link `library.active.from.<n>`
    ("From Core Map Pack", or the map's title when its pack holds that map
    alone) for the same map, while a pill is locked the label
    `library.active.lock` (the lock's words whole), and while a mod is
    played the button `library.active.unload` ("Unload mod"), disabled
    during a game and while the command line chose the mod, the label
    `library.active.keys` (the layout in use, its own-changes line and the
    with-mod line) and the button `library.active.keys.change` ("Change…").
    The mod label is the part of
    the tile above that button, the language label the part of its tile that
    shows, and a map's the part of its title line that shows, or the
    players-and-size line when the title is the link, empty while
    that part is scrolled out. The pack's link is left out while its line is
    scrolled out of the area. At most 4,000 maps are named.
- `focused_field(state, list)` gives the search field while it has the
  focus, over which a host keeps the system's text input started.
- `focus_tab_search(library, state)` gives the open tab's search the text
  focus, shown: the catalogue search, or the Active tab's maps search; the
  Registries tab has none. A host calls it as it opens the Library; a tab
  opened inside it, by the pointer, a finger, Ctrl+Tab or the controller,
  does the same, while a key on the tab strip keeps the focus there.

## State

None beyond the `Library` value the caller holds: its inputs, entries,
browsing state and selection. The model keeps nothing else between calls.
The screens keep their hover, press and focus, the two panes' scroll and the
wheel's carry, the search field, the open drop-down and the Compact details
page in the caller's `ScreenState`. The input reads where the screen's
controls and panes are from the display list it is given, the one the
screen shows. A display list's badges point into the model's inputs, and
stay valid until the model's next refresh.

## Invariants

- Pure: no file, clock, network or random access; the same inputs and calls
  give the same state on every machine.
- `Entry` pointers point into the owning `Library::inputs` and stay valid
  until the next `refresh`. A copied `Library` still points into the
  original's inputs: refresh the copy before reading its entries.
- The selection is always one of the visible entries, or none when none
  shows.
- Every text comes from a pattern in `text.hpp`, looked up whole through
  `Library::text` (`TextHooks`) before its places are filled; with no
  lookup it is English.
- The screens are made only of the kit's components and its display list's
  generic parts; every colour is a kit token and every size a kit metric,
  but for the panes' widths the design gives (Compact's 128 points beside
  the search for the filter and its count, the 240-point search, the 400-point lists, the
  180-point filter pane and the details' 40-point badge). `draw.cpp` only
  paints the display list.
- The layout decides no rule of the model: which entries show, in which
  order, their states, what blocks them, their actions, the filters'
  counts and what the Active tab says are the model's.
- The Active tab shows what closing the Library applies: the pending base
  game, its expansions, and No Mod only while closing unloads the mod. A
  draft is edited exactly when closing has something to store, an unload
  alone included, and the host stores only an edited draft. Choosing the
  stored base game again, after another, plays the mod again, and an
  expansion the command line's mod needs stays on, since closing never
  unloads that mod. Its pill is locked exactly while that is why its
  press is refused, and only then.
- On the Active tab Unload mod takes a press, a key or Tab only while a
  mod is played and Switch off would act, and each map's link to its pack
  does while its line shows. The labels do not.
- Every action Regular offers is reachable at Compact: on the list page, or
  on the details page, its first three as buttons and the rest in More.
- No two controls meet, but the list and the rows it holds and each row
  and its icon; a row that does not show has no control. Every control that takes the focus is in Tab's
  declared order and reached by the arrows from the first tab.
- An update's Rules fact is shown whole at every class, never cut. An
  updatable mutator's actions are controls at every class, Compact's on the
  details page with More open, its Rules fact is whole, and an
  incompatible verdict is drawn in the danger colour.
- A press on a row only selects it, and at Compact opens its details; only
  its download arrow or up-arrow asks for Get, Get with or Update, by the
  same rule as the details' button. The arrow's name stays `get` or
  `update` from the row's state.

### Prerequisites

`prerequisites.hpp` is the order every mutator screen uses when it switches
a mutator on or downloads one. The graph is ids only (`Need`): the caller
turns the Library's prerequisite records into needs, and version ranges
stay with the caller.

- `prerequisite_closure(graph, ids)` returns every id `ids` reach. An id
  is met when `ids` names it, or when a need list names it while each id's
  needs are read from left to right before the next id in `ids`. The ids
  of one cycle stay together in that order. Every other id goes after
  everything it needs. A component goes after the components it needs, and
  a tie keeps the component whose id was met first. An id the graph does
  not hold is a leaf. A cycle sets `Closure::cyclic`. A repeated id in
  `ids` is kept once.
- `dependants_of(graph, among, id)` returns every id in `among` that needs
  `id`, directly or through other ids, in `among`'s order. `id` itself is
  included only when a cycle leads back to it.
- `prerequisites_above(graph, order)` is true when no id in `order` comes
  before an id in `order` that it needs, directly or through other ids.

The list applies from top to bottom, so a prerequisite stands above the
mutator that needs it. The Library, the picker and the other mutator
screens all call these three, so a mutator applies after the mutators it
needs whichever screen put it in a list. They read no file, clock or
network, and they hold no words.

`needs_of(library)` is the graph those three read: one record per mutator
entry, its id the package key, its needs the installed manifest's when
that copy is installed and otherwise the listing's. `get_plan(library,
entry)` is the download: every missing prerequisite, transitively, from
the listing that offers a version in range (the entry's own registry,
then a registry that ships with the game, then the lowest registry id;
the newest release in that set), in that closure's order, then the entry.
A prerequisite already installed in range is left out. One installed
below its range is an update of the listing that meets it. A prerequisite
no usable listing offers in range blocks the plan, and so does a step
Get would refuse; a step that does not work with the mod being played
does not. A cycle blocks the plan and names the ids in the loop, and
places nothing. The picker reuses this plan. No other part of the Library
orders prerequisites.

Get with takes Get's place when that plan holds more than the mutator, or
is blocked before the mutator can be the only download. One missing
prerequisite names it (`Get with {name}`); more than one counts them
(`Get with {count} more`). The button is off while the plan is blocked.
`details_note` then says those packages download first and sit above this
mutator in every list. `get_with_question(library, entry)` asks "Get
{name}?" with one paragraph, one line per package that will download (its
badge, "{name} {version}", and " · from {registry}"), Cancel marked and
Get both for two packages or Get all for more. Nothing is queued until
that button. Automation reads `library.action.get-with`, `oa.prompt.cancel`,
`oa.prompt.get-both` and `oa.prompt.get-all`.

## Tests

`ui-library-prerequisites` closes made-up mutators: one id, a chain of
three, a diamond, two roots that share a prerequisite, a cycle of two and
of three, an id the graph does not hold, a repeated id, and an id that
needs a cycle without being on it. It takes
dependants direct, through another id and none, in the list's own order,
and an order that keeps a chain, a diamond and a list naming an outside
id, and one that does not. A chain of 1,000 closes with the first id at
the top. It also builds the download plan: needs from an installed
manifest and from a listing, Gatehouse Keep with Gatehouse absent, installed
below its range and in range, listed only below the range, and listed by
two registries; a chain of three missing prerequisites; a prerequisite the
played mod cannot use; a cycle; Get with's words and whether it is on;
the question's title, paragraph, items, buttons and fit at every class;
the note; the uninstall paragraph for one dependant and for two; and a
lookup that translates.

`ui-library-update-question` builds Libraries over a built-in and an added
registry: `asks_before_update` and `update_question` for an added
registry's mod whose rules change or are unknown (asked), one that keeps
them, the built-in registry's whose rules change and whose are unknown, and
a language and a map pack (never asked); the design's words exactly, with
the sample numbers; a new version and an unknown rules hash; the mod being
played; Update all over a built-in and an added registry's rules-changing
mods naming only the added one, none to ask, and five named as three and
"2 more"; the kit question's paragraphs, Not now marked and Update primary,
its automation names and its text fitting at every class; the questions
about the mod played, their words, buttons and marks; an update that names
no revision, whose status line and question never say "Revision 0"; and a
lookup that translates.

`ui-library` builds Libraries over made-up registries and packages: the
state of a listing alone, a package installed alone, matched with and
without an update, played with and without one, a blocked update, and each
queue phase with its status line and button; every reason Get or Update is
off and their order; installed entries never blocked; built-in and added
registries, two registries with one key, and a key installed from another
registry; the Library opening on Active; tabs, Mutators between Maps and
Brains and Brains between Mutators and Languages, each of those opening on
All, each tab's own filter, the
Updates tab's filters across every kind, and tags with their counts; the
filters' counts unmoved by the tag and the search; New's ten newest by
day, ties by name, never an undated listing, on Mods, Maps and Updates;
the Mutators tab's Get, INSTALLED and Update rows, an update beside a
mod's, a mutator that does not work with the mod being played still
getting with its note naming the mods, an unmet prerequisite that does
not block, the eight tags ahead of any other id, and each tag's label,
an unknown id and a different case left as written; the Brains tab's
built-in computer player first, marked Default until Steady Commander is,
left out of every count and off every other tab, a listing alone, a brain
installed alone, one matched with an update and one without, an update
beside a mod's, why a brain does not fit (Ridge with no mod and with
ridge@28, Core Contingency off and on, an engine range, and one that
fits) without blocking Get or dimming the row, and each brain tag's
label, an unknown id left as written, and a lookup that translates; a mutator's badge at
every class and window, its picture, else its first tag's icon, else its
letters, Large's "Needs {name}" and "Not compatible with this mod" chips
in the lock colour with the tag chips kept only while they fit, Compact's
lock-coloured bylines, the tag icons on the pills and in the filter pane,
and an incompatible row left bright with Get on;
a key layout's facts in their order, what it changes, works with and asks
for, and the player's own changes by command and key; Use's question
asking its host what moves once, from the layout in use, the kept list
while Keep is marked and the clean one otherwise, its button named by the
answer to Where, and a layout that already holds own changes never written
over;
the Active tab's mod, language and maps in each case, ordered by folded
title and then pack, with the From link when the pack holds several maps and the minimap when one is set;
the order with
no search; the update count; the selection across refreshes and browsing;
the base game chooser, the expansion-off question and its answer, and TURN
ON kept through a refresh that brings the same stored choice, and the
chooser and the question dropped by another; another base game and then
the stored one again, the mod played again after it; every sequence of
five pill presses, answers to the expansion-off question, base game
choices and refreshes, each leaving the Active tab on what closing the
Library applies, with the mod needing what is stored and not, chosen by
the command line, and no mod; a pill locked, with the lock's words naming
it, exactly while the command line is what refuses its press (refused as
it stands, asked about with the mod chosen in the game), by hand for a
need stored on, one through Core Contingency that locks Campaign too, one
stored off that locks once turned on, a game, a choice `--content` named
and no mod, and after every step of every sequence of four from six
starting states; the buttons of every state,
Switch off on the mod played and Play now and
Switch off off while the command line chose the mod, its note, and when
the refresh icon can be pressed; the summary, the manifest's description standing
in for a listing's for a mod of the player's own, and "No oamod.yaml
present" for a folder without one; the facts with the sample numbers, Installed,
Latest and Author among them, a revision of 0 left out, a version left
out, a fact with nothing to say left out and the manifest's author
standing in; a listed and an installed mutator's facts in both forms, each
verdict state's words and mark from `verdict_of_record`, the counts'
summary, no Checked fact and no Check with this mod when the verdict is
unset, Ridge Walls' Works with marked while No Mod is played with Get
still on, an incompatible verdict marked and still offered Update and
Check with this mod, the status "Not installed · 2.0 · rev 1", and no
Roll back even with a kept copy; the Rules fact for an unchanged, changed and unknown rules
hash in both lengths; status lines, "Update available" for an update
that names no revision, bylines with a maker and with none, and
the playing note; the queue lines, after an update that changed the
rules and one that kept them; `change_texts` for a new revision, a new
version and a side that names no revision; every text function,
`release_text` among them, and the Registries tab's initials, ages,
subtitles and note; and a lookup that translates.

`ui-library-uninstall` builds a Library over a built-in registry, the
player's own mod, the mod being played, a map pack and two language packs,
one of them in use: Uninstall on every installed package and on none
other, in its place among the buttons; off for a map pack during a game
and in each queue phase but failed and installed, with the note saying
why; the step for every kind and use; the question's words with and
without a kept copy, for a listed package and the player's own, Cancel
marked and Uninstall primary; the notices' words for the mod being played,
the language in use and each problem, each with one OK; and a lookup that
translates.

`ui-library-search` checks that a name beats a tag, a tag an author or
publisher and those a summary; several words; a word in no field hiding
the entry; ASCII folding; tag prefixes, a translated label of a known id,
and a different case of that id matching the id and not the label; the
64-byte cap at a character boundary; and ties ordered by name and then registry.

`ui-library-layout` lays the screens out over a fixture of fifteen
packages of the three kinds, over five made-up mutators on the
Mutators tab, and over made-up brains on the Brains tab, from a built-in and an added registry (an
update of the built-in Ridge that changes the rules, a blocked update, the
mod being played, a Get blocked by three missing hacks, and four queue
items: downloading, waiting, failed, and an installed update that changed
the rules), in a synthetic font, at Compact, Regular and Large and in the
windows of 640x480 at 1x, 1920x1080 at 2x (Regular, 960 by 540 points;
at Auto that window is Large at 1x, as 1280x720 is, and Regular is
800x600) and 2560x1440 at 2x. At each it
checks that every part lies inside the window; that no two controls or
press areas meet; that every action of the selected entry is a control
(at Compact on its details page, More open), Update all over the list
on the Updates tab beside the Mods tab's filters, the refresh icon and Close;
that the Active tab is first, has no search, filter, list or Details,
reads its mod, language and maps as labels, shows Playing and Built in
as pills, lists each installed map rather than each pack, and while a mod
is played puts Unload mod in Tab's order with the tabs, each visible From
link and the footer (absent on the base-game tile, and out of that
order while disabled); that a one-map pack's row and details wear that
map's minimap, fitted to the square with the well colour in the
letterbox, that a several-map pack keeps its badge, that a several-map
pack with no badge shows the first map's minimap on the Maps tab and its
letters on Updates, that the details show one large picture with its
map's players, size and description and no name, or a grid with each
map's name, players, size and description, that a minimap not
yet decoded is a placeholder and then the picture, and that a one-map pack's title is the link to its pack with no From line while a pack of several keeps its From line, and that the link
opens Maps with that pack selected and its row in view; that the Registries tab lists each registry on a card, its heading the
title, its switch, key icon and REFRESH, its address, refresh time and
description, a neutral Built in pill on the title's line for one that
ships with the game, REMOVE and ADD MIRROR… only for one the player
added, and ADD REGISTRY… at the top right above the cards, with no
search or Details; that resting on the key
icon or focusing it shows that registry's fingerprints and public keys,
and pressing it asks for nothing; that REFRESH asks for that registry and
is greyed, with the refresh mark turning beside it, while the registry is
off or already being asked; that a refresh which reads a list shows the
check and then the arrow, and one which fails shows the arrow; that a
switch's halves, Space, REMOVE, ADD
MIRROR… and ADD REGISTRY… ask for those actions, with ADD REGISTRY… and
ADD MIRROR… absent during a match; that
Tab follows the declared order and the
arrows reach every control that takes the focus from the first tab, and
no label; that the names are the screen's and unique; that every text
fits by the kit's measure, and that the Rules and From facts read their
whole values; that a mod of the player's own shows its description in
the details; and that Use's question puts Where, the own changes, What
moves and the helpers in that order above its buttons, its choices radio
rows, the marked one's dot showing, its choices and buttons at Kit 2's
control height, What moves naming each command's keys
and the player's own, Use named by the answer to Where, and that it still
fits every class with two helpers and the interface note. Regular shows more rows than Compact, and Large has the
filter pane. It then drives the screens: Enter at Compact and at Regular,
Escape's three cases, the tab commands both ways, the controller's X, Y
and B, the list's keys, Tab's walk, presses, double presses and taps on
rows (which never get anything), a row's up-arrow and download arrow at
every frame (named after the row, a button whose text is the word,
pressed as Update and Get, and listed disabled and passing the press to
the row while blocked or queued), an action's button, the refresh icon greyed
until a registry can be asked and then asking, and saying "Refreshing" while
one is being asked, the labels taking no
press, the header's close mark to the right of refresh asking what CLOSE
asks, the wheel and a finger's drag, the scroll bars of the Compact list,
the Regular details, the Compact details page and the Registries tab
dragged by the mouse and by a finger, typing and Backspace, the
filter drop-down with its counts, each tab keeping its filter, New's
pill, and a press outside the drop-down, the tag pills, More, and the
Active tab with forty more maps: the four tiles take no press on their
labels, and on Compact the page scrolls as one by the wheel, a finger, its
scroll bar, the page keys, Home, End and the right stick until four map
rows are in view, and focusing a partly shown map link scrolls it wholly
into view. On Regular the maps pane scrolls on its own and the tiles stay
put. Change… and the expansion pills take a press, a pill the command
line's mod needs drawn locked at every size (its face kept under the
locked fade, out of Tab's order, its press doing nothing, the padlock and
the lock's words under the pills, which `library.active.lock` reads whole)
while the pills the player can turn keep their look, and nothing locked
with the mod chosen in the game,
Unload mod asking what Switch off asks and greyed during a
game, and the maps search filtering
by map name and by pack name, counting the tiles that match and saying
when nothing matches.

`ui-library-draw-data` lays out and draws, in the installed game's fonts
and the modern fonts beside the test, Compact in the 640x480 window at
1x, Regular in 1920x1080 at 2x (960 by 540 points; at Auto that window is
Large at 1x, as 1280x720 is, and Regular is 800x600) and Large in
2560x1440 at 2x, Compact's details
page, the Active tab at Compact, Regular and Large, Compact scrolled to
its maps, with many maps at
Regular, and at Compact while the command line chose a mod that needs
Campaign, whose pill is locked, the Updates tab at Compact and Large, the Registries tab at
Compact, Regular and Large,
Languages at Compact, the Maps tab at Large for one map and for
several, and the Mutators tab at Compact (640x480), Regular (1920x1080
at 2x) and Large (2560x1440 at 2x): each twice, byte for byte the same, with nothing
drawn outside the window and every pixel of it drawn, every part inside
it and every text fitting. The Active and Maps pictures dress the
fixture's maps with made-up minimaps and a long description, on that
drawing only. Compact lists each map on one line, the pack link at the
right when the pack holds several. Regular and Large show a minimap, a
cut description and a From link. A one-map pack shows its minimap on the
Maps row and large in the details, and a several-map pack with no badge
shows the first map on the row and a grid in the details. The Registries pictures dress the fixture's
built-in registry as Core Prime, title, description, author and short
name, on that copy only, and gives it the app icon as its avatar. That
author is Open Annihilation, so the card shows the app icon under its key,
the plates filtered down, or the drawn plate when no icon has been given.
The Languages picture, on that drawing
only, sets the fixture language pack Portugues (Brasil) to author
"OA team" and adds an installed copy, so the row shows that mark
under its check. The built-in languages lead that list, each marked
Built in. `library-languages-zh-compact.png` is the same window with
简体中文 installed and in use, on its details page.
`oa-ui-library-draw-test --data --frames DIR` also writes
`library-compact.png`, `library-regular.png`, `library-large.png`,
`library-details-compact.png`, `library-active-compact.png`, `library-active-compact-maps.png`,
`library-active-regular.png`, `library-active-large.png`,
`library-active-many-regular.png`, `library-active-locked-compact.png`,
`library-updates-compact.png`, `library-updates-large.png`,
`library-registries-compact.png`, `library-registries-regular.png`,
`library-registries-large.png`, `library-languages-compact.png`, `library-languages-zh-compact.png`,
`library-maps-one-large.png`, `library-maps-several-large.png`,
`library-mutators-compact.png`, `library-mutators-regular.png`,
`library-mutators-large.png`, `library-keys-compact.png`,
`library-keys-regular.png`, `library-keys-large.png`,
`library-keys-use-compact.png`, `library-keys-use-regular.png`,
`library-keys-use-large.png`, `library-keys-lefty-large.png` and
`library-active-keys-regular.png` for review. The Keys pictures dress two
made-up layouts described by what they do, the player's own changes on
the one in use, and what Use would move from it to Lefty. The Mutators pictures dress the fixture with made-up mutators
on that drawing only, so a picture, a tag icon, letters, "Needs Gatehouse"
and "Not compatible with this mod" are on the list.

## Limitations

- Folding is ASCII only: letters outside A to Z match only themselves.
- A tag is matched from its start, never inside it.
- An update that cannot be installed is described with the detail its
  update note carries, as the app wrote it; the model does not reread the
  catalogue for it.
- The OA layer's Library screen ([src/app](../../app/README.md#the-library))
  fills the inputs, places the window and maps the keyboard's own keys to
  `ScreenCommand`; the controller's Y, X, LB, RB and right stick do not
  reach `library_command` and `library_stick` yet.
- Only rows that show have controls: a journey scrolls or searches before
  it presses a row that does not show.
- Regular's pills show as many tags as fit, and Tags holds the rest.
