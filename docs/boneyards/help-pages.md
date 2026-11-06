# Help pages

The Help console reads `boneyards/Help/Help.def` and the pages it names.
The boneyards expansion still holds the 1998 copies. The game ships its
own pages as loose files at those same paths, and the reader takes a loose
file before the expansion's file. The expansion is not changed, and the
pages are not packed into `coreprime.ccx`.

A page the loose set does not hold is still the expansion's page.

## Where the packages put them

The build copies `data/boneyards/help/` to `boneyards/Help/` in the folder
the game's own files use (`OA_GAME_FILES_DIR`).

| Package | Path |
|---|---|
| macOS app | `Open Annihilation.app/Contents/Resources/boneyards/Help/` |
| Windows, beside `open-annihilation.exe` | `boneyards/Help/` |
| Linux, beside the `open-annihilation` executable | `boneyards/Help/` |

Each of those folders holds `Help.def` and the pages listed below.

## Contents

`Help.def` keeps the six categories: Getting Started, Galactic War Help,
TA Help, Screen Help, Getting Addl. Help, and Rules of Conduct.

It adds four pages the 1998 contents did not list:

| Page | Title | Why it is listed |
|---|---|---|
| Help66 | Help screen | the Help screen itself, which the old contents left out |
| Help67 | Profile screen | the Profile screen, which the old contents left out |
| Help112 | Update screen | the Update screen, which the old contents left out |
| Help45 | Editing a profile | our page for editing a profile, with no construction banner |

It does not list Help39, Help54 (the register page is Help226), Help168,
or the Locator pages Help40 and Help68, which were left out while the
Locator was gated to Admin and Beta Tester accounts.

## The contents list

Help, Ladders and News share one contents list, laid out as the 1998
console lays it out: rows 17 pixels high down the left of the console, every
title 14 pixels in from the row's left edge, categories and headlines alike.

- A category's arrow points down while the category is open and right while
  it is closed. It sits at the row's left edge, centred on the title.
- A click on a category opens or closes it. The list is built again with
  one title per row; the rows in view stay where they were, as far as the
  rows reach.
- The headline of the page shown is drawn in the light font, whether the
  page was opened from the list, from a link in another page, or is the
  first page the console opens. While its category is closed no headline is
  lit.
- While rows are out of view a scroll bar shows along the list's right
  edge, as tall as the list. Its thumb is as long as the share of the rows in
  view and follows the list as it scrolls, from the arrows, the track or a
  drag. With every row in view the scroll bar is hidden.

## Replacements

Each of these is our own wording. The expansion's file of the same name
stays as it was.

| Page | What the player reads |
|---|---|
| Help47 | Where to begin. Feedback goes to Core Prime. |
| Help114 | Moderators wear the Yard Dog insignia, and how to reach them. |
| Help115 | Support, through Core Prime. |
| Help226 | Register at `https://coreprime.net/register`, then choose Arm or Core at login for each sector. |
| Help227 | Where players gather: Core Prime only. |
| Help48 | Ranks, one side per sector: Arm and Core each keep a rank in that sector. |
| Help53 | Fighting the war, on the Core Contingency and Battle Tactics maps from the Library. |
| Help60 | A first game, with no shop. |
| Help38 | The Duel chart is scored by rank, and similar means the same rank. Winner Takes All and training use Core Prime's Glicko-2 conservative rating. |
| Help45 | Editing a profile. A password typed on the Account tab is not applied; a password changes on the web. |
| Help75 | Logging in. A wrong password resets at coreprime.net/forgot. |
