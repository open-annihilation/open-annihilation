# Languages

Open Annihilation shows the game in the language the player chooses,
through the game's own translations: the ones Total Annihilation 3.1c's
data holds for German, French, Italian and Spanish, and any a mod's data
holds. Language packs add to them, entry by entry, and bring Simplified
Chinese. English is the game data's own language. Every language beyond
the five 3.1c's data holds is a pack, which the game downloads from a
registry when you choose it ([language packs](#language-packs)).

## How the language is chosen

The first of these that says decides, once at start and again each time
the setting changes:

1. **3.1c's command line.** A word on its own, as `open-annihilation german`
   or `open-annihilation Chinese` writes it, names the language the game
   data knows it by, as 3.1c reads it. The setting then shows "Set on the
   command line" for the run.
2. **The setting.** Language, the first control of the Language section of
   the OA settings, offers System default and each language the game can
   show, named in itself. With no language pack installed those are
   English, Deutsch, Español, Français and Italiano, in that
   order. An installed pack adds its language among them, by its own name.
   Under them, headed AVAILABLE TO DOWNLOAD, the list offers each language
   a registry offers whose pack is not installed, with the pack's size;
   choosing one downloads the pack and then switches to it
   ([Settings](settings.md#language)). It is kept as
   `open-annihilation.language`: `system`, or the language's BCP-47 tag
   (`de`).
3. **The operating system.** System default takes the first of the user's
   preferred languages, in their order, that the game can show: the preferred
   languages on macOS, the user's interface languages on Windows (the
   user's locale on Windows XP), and `LANGUAGE`, `LC_ALL`, `LC_MESSAGES` or
   `LANG` on Linux. A region does not matter: `de-AT` and `de-CH` choose
   German. `zh-Hans`, `zh-CN`, `zh-SG`, `zh-MY` and a bare `zh` are
   Simplified Chinese: once that pack is installed they choose it, and
   before then the game shows English, having recognised the locale as
   wanting that language, and offers the pack at start
   ([below](#choosing-the-language-at-start)). Traditional Chinese
   (`zh-Hant`, `zh-TW`, `zh-HK`, `zh-MO`) is not offered and chooses none,
   never Simplified. When none is known, English.

A preferences file named with `--preferences-file` starts in English, the
game's own default, so that a check plays the same on every machine. 3.1c
takes its language from the command line or the registry and never asks
the operating system; the engine differs there on purpose.

## Choosing the language at start

No player is switched to English without being told. When the language
setting names a language whose pack is not installed, as for a player of
0.7 who chose Chinese, the game shows a notice over the main menu once that
menu has settled. The same notice shows when the setting is System default
and the computer's language is one of those, until the player answers Use
English for it. It never shows in a game, nor when 3.1c's command line
names the language.

The notice is titled Language, with the language's own name large under
it, then its text in that language, where the game has the words, and in
English: for Simplified Chinese, "简体中文现在需要单独下载（3.5 MB）。安装完成前，游戏将显示英文。"
and "Chinese (Simplified) is now a separate download (3.5 MB). Until it
is installed, the game shows English." Its buttons are **Use English** and
**下载 · Download**, which has the focus:

- **Download** downloads the pack from the registry that offers it, with no
  question, and shows how far it has come ("1.6 MB of 3.5 MB", with a bar).
  When the pack is installed the notice closes and that language is the
  one the game shows. A download that fails says "下载失败。" and "The
  download failed.", and the button becomes **重试 · Try again**. When no
  registry offers the pack, or downloads from it are off, the notice says
  "现在无法下载。" and "It cannot be downloaded now.", and Download is
  greyed.
- **Use English** leaves the game in English. When the setting names the
  language, Use English stores English. When the setting is System
  default, Use English remembers that language so the notice does not
  return.
- Escape, or B on a controller, closes the notice until the next start; Y
  presses Download.

The remembered languages are `open-annihilation.language-notice-dismissed`,
a comma-separated list of tags.

## Language names before their pack

A language's own name, and the notice that its pack is not installed yet,
are drawn before that pack is installed. The endonym face,
`NotoSansCJKsc-Bold-Endonyms.otf`, is a cut of Noto Sans CJK SC Bold that
holds those names and the notice's lines. It travels with the game, so the
list can show 简体中文, and the names the catalogue may grow into, while
the language's own pack is absent. The sans faces draw the Latin and
Cyrillic names. The face is sized as an ideograph face is, and it keeps
those rows, so a line does not move.

To add a name, add its line to `tools/text-fonts/endonyms.txt`, run
`python3 tools/bootstrap_text_fonts.py --fonts-only`, and put the SHA-256
and the character count it prints into that script.

## What is shown in it

Everything comes from the game data, read as 3.1c reads it for its
language; whatever the data leaves untranslated shows in English.

| What | Where the game data holds it |
|---|---|
| Units' names and descriptions: the build menu's buttons and the bottom bar, the unit panel, the F1 panel, the unit restrictions list, the units' spoken lines in the message log | each unit file's `GermanName` and `GermanDescription` (`FrenchName`, …), else `Name` and `Description` |
| Menus, dialogs, message boxes, panels' texts, the kill board, the loading screen, the F1 panel's labels, the units' spoken words, the message log's own phrases, a shared game's notices (speed changes, a player's disconnection), the network loading screen's line, the battle room's TIMEOUT dialog and rejection messages, features' descriptions, a chosen map's name and its description | `gamedata\translate.tdf`: a section names the English text, and its key for the language holds the translation |
| Campaign missions' names, briefings, hints and narration | a mission's `<Language>missionname` (also `brief`, `narration`, `missionhint`), and the `camps\briefs-<Language>` and `camps\hints-<Language>` folders |
| Campaigns' names in the New Campaign and Any Mission lists | `gamedata\translate.tdf`, by the campaign file's name, as `[Arm Campaign]` and `[Core Campaign]`; a name it lacks shows as the file is named. 3.1c itself lists every campaign by its file's name and looks no name up; in German, French, Italian and Spanish the engine shows the Arm and Core campaigns by the names 3.1c's table gives them (`Arm Missionen`), a deliberate difference |
| Pictures with words drawn in them | `bitmaps-<Language>`, as the battle room's `battleroom.pcx`; `unitpics-<Language>` |
| Fonts | `fonts-<Language>` |

What 3.1c's data holds:

| | German | French | Italian | Spanish |
|---|---|---|---|---|
| `translate.tdf` (3.1c's patch) | 1,028 texts | 1,027 | 1,024 | 1,028 |
| Units' names, of 278 | 277 | 277 | 276 | 276 |
| Campaign missions named, Arm and Core | all 50 | all 50 | all 50 | all 50 |
| Core Contingency missions named | all 25 | all 25 | all 25 | none |
| Briefings | 18 | 16 | 25 | 25 |
| Battle room picture | yes | yes | yes | yes |

A new language shows at once in what is drawn each frame: the bottom bar,
the unit panel, the build menu's lines, the kill board, the loading screen
and the lines the message log posts from then on. The main menu or the
in-game menu under the settings shows it at once too, with the words over
the game's pictures, and so do the titles over the battlefield. The other
menus and panels show it as they open, and a campaign's lists as they are
filled again.

The language changes only what players read. The simulation, a saved game
and what a shared game sends are the same in every language: a unit
keeps its own name in its definition and in saves, a mission is saved under
its own name, and the rules (units, weapons, maps, missions, the computer
players' scripts) are read from their own folders, never from a language's,
though 3.1c looks there too. Players in different languages play together.
The alliance lines, "allied with" or "broke alliance with" and a name,
which the battle room's ALLY buttons and the ALLIES panel say, and the
battle room's "does not have this map" go between machines in English, as
English 3.1c sends them, and each machine shows the phrase in its own
language, the names as they came; a recorded game keeps the English and
shows it in the viewer's language. This is a deliberate difference from
3.1c, which sends them in the sender's language: a 3.1c player in the room
reads them in English, and a line a player types in the same words shows
translated too. The notices about another machine (a speed it set, a
player's disconnection, a modified program) are built on each machine in
its own language, as 3.1c builds them. 3.1c's table holds no translation
of the modified-program line, so it shows in English unless a language
pack keys it.

## Language packs

A language pack is a folder of texts for one language, named by its tag.
It never replaces the game data's translations; it adds to them, entry by
entry:

```
zh-Hans/
  language.yaml   the manifest
  translate.tdf   the game's own texts, keyed by 3.1c's English
  units.tdf       units' names and descriptions, by UnitName
  missions.tdf    missions' names, descriptions and hints, by mission file
  interface.tdf   the engine's own words, as the interface catalogue reads them
  pictures.tdf    captions drawn over the player's own pictures
  files/          whole files in the game data's language folders
  fonts/          font files the manifest's fonts key names
  warmup.txt      characters drawn ahead, when the manifest names it
```

Only the manifest is required. It is strict YAML, read as mod profiles are:

```yaml
oalang: 1
tag: zh-Hans
name: 简体中文
english-name: Chinese (Simplified)
word: Chinese
version: "1"
locales: [zh-Hans, zh-CN, zh-SG, zh-MY, zh]
fallbacks: []
text: {needs: modern-fonts}
unicode: true
homepage: "https://example.org/languages"
tags: [translation]
supportedGames: [totala, totala-demo]
requires: {engine: ">= 0.8.0"}
fonts:
  - {file: NotoSansCJKsc-Bold.otf, role: ideographs}
warmup: warmup.txt
packaging: {revision: 1, date: 2026-10-10, packager: Ridge}
```

`tag` names the language, `word` the word the game data knows it by
(`ChineseName`, `Chinese=…;`, `camps/briefs-Chinese`). A pack whose tag
the game does not already show adds that language to Settings › Language,
named in itself, beside the languages the game ships. A pack for a
language the game already shows adds its text and leaves that language's
name, word and drawing needs as they are. A language a registry offers,
whose pack is not installed, is offered under AVAILABLE TO DOWNLOAD, by
the name, tag and locales the catalogue gives, until its pack arrives.
`unicode: true` turns Enable Unicode Multiplayer Chat on while the
language is shown ([Settings](settings.md#language)). A language whose
text needs the modern fonts turns it on too, with or without its pack,
since its text is UTF-8. The installer checks `requires.engine` when a pack is
installed, and reading a pack that is already installed does not, so an
installed pack stays available when this Open Annihilation is older than
the pack asks for. Quote the requirement: a value that starts with `>`
is refused unless it is quoted. `supportedGames` names the base games the
pack's text is for. Leaving it out means every base game, `totala` then
`totala-demo`. `requires.base: ta-3.1c` still reads, and when
`supportedGames` is omitted it means `[totala]` rather than every game.
`requiredExpansions` names expansions that must be on; leaving it out means
none. An id is a built-in bundle, and a required expansion must extend one
of the supported games.

`fonts`, `warmup` and `packaging` are optional, and a pack that names none
of them still reads. `fonts` names at most four files in the pack's
`fonts/` folder. Each entry has a `file`, a name ending in `.otf` or
`.ttf` with no path in it, and a `role`. `ideographs` is drawn as Noto
Sans CJK is, at the least size while a Chinese, Japanese or Korean
language is shown. `letters` is drawn at the sans faces' size. The faces
are added while the font stack is open and never change a line's rows.
`warmup` names a UTF-8 text file in the pack's folder, of at most 4,096
bytes. When the language is shown, that text is laid out in each face, so
the first screen draws few new glyphs. The Simplified Chinese pack's
`warmup.txt` holds the characters its screens use most, and its font
travels in the pack. `packaging` records a release of the pack, as a mod's does: a
`revision` from 1 to 65535, the `date` it was made (`YYYY-MM-DD`) and the
`packager`, 1 to 128 bytes.

### Installing a language pack

Open, drop, or Open With a `.oalang`. The system registration of that type
is a separate step; this is the install once the game has the file. The
pack goes in the player's folder, `Documents/Open Annihilation/Languages/<tag>`,
named by the tag in `language.yaml`. A file the player opened asks before
anything is written: Install for a language that is not there, Replace for
another version or for a folder that is not this pack, and Reinstall for
the same version and revision. The old folder is kept as `.backup` on a
replace. Install alongside is not offered. `requires.engine` must be a
range this build meets; the question that refuses names the range and this
version. Quote a requirement that starts with `>`.

When a file you opened is in place, **LANGUAGE INSTALLED** says where, with
**OPEN FOLDER** and **OK**.

When the pack is the language chosen, including a choice that read as
System default until the pack was installed, the running game switches to
it at once. The registry, the interface catalogue, the pack layers and the
font faces are read again; the game does not ask for a restart. A pack
downloaded from a registry, Core Prime's or one you added, asks nothing and
shows nothing. It installs while Settings or a notice show, and in a run
nobody watches, and it does not install while a match loads or runs.

Language packs are downloaded from inside the game: from Settings ›
Language's list, from the notice at start, and from the Library's
Languages tab, which Settings › Language's **MORE IN THE LIBRARY…** opens
([the Library](library.md)). Each pack and revision is also a release of
the open-annihilation/languages repository on GitHub, named by the pack's
tag and revision (`zh-Hans-r2`), whose `.oalang` file installs as above
when you open it, with no install ID
([downloads and privacy](downloads.md#language-packs-without-an-install-id)).

The tables are UTF-8 TDF files, each starting with its licence in `//`
comments. TDF has no escapes, so a value never holds `;`: write the
full-width `；`.

- **translate.tdf** is keyed by the English text exactly as 3.1c's
  `gamedata\translate.tdf` is, with the word as the key:
  `[Select Map] { Chinese=选择地图; }`. It covers the game's messages, the
  menus' captions, help lines, features' descriptions and maps' names and
  descriptions. An entry the pack leaves out shows as the game data has
  it; the Simplified Chinese pack leaves out the companies' names so.
  A button with stages, as `Easy|Medium|Hard`, is translated whole and
  then stage by stage, as 3.1c translates it; to give one button's stage
  a word of its own, translate its whole caption:
  `[Off|Medium|Full] { Chinese=关闭|中等|全部; }`.
- **units.tdf**: `[ARMCOM] { name=…; description=…; name-from=…;
  description-from=…; }`. `name-from` and `description-from` give the
  English the text translates; when the game data's English differs, as a
  mod that reuses a unit's name makes it, the text is skipped and the
  game data's own shows.
  [tools/language_pack_check.py](../tools/language_pack_check.py) reports
  such stale fields.
- **missions.tdf**: `[Lipar Pass.ota] { missionname=…; }`, by the
  mission's file as the campaign names it; the campaign's mission list
  shows the name.
- **interface.tdf**: the engine's own words, in the catalogue's shape
  below.
- **pictures.tdf**: captions drawn over the player's own pictures (Words
  in pictures, below). The file and each entry are optional: a picture
  with no caption, or a caption left out, is drawn as it is.
- **files/** holds whole files under the game data's language folders, as
  `files/camps/briefs-Chinese/<briefing>.txt`: the missions' briefings in
  the language, in UTF-8.

For a text in a language, the game tries, in order:

1. a mod's pack, `languages/<tag>/` in the mod;
2. the game data in the language's word, read as 3.1c reads it;
3. the player's pack, in `Languages/<tag>/` of their own folder, then a
   packager's, in the `languages` folder beside the game's `fonts` (the
   engine ships no packs);
4. the same for each of the language's fallbacks, then English.

An absent or empty value falls through to the next, so the player never
sees a blank. Packs change only what players read: they are in no mod
profile's hash and change nothing a shared game sends, and 3.1c's German,
French, Italian and Spanish show exactly as 3.1c shows them.

`python3 tools/language_pack_check.py PACK --game-dir GAME --oa-tool OA_TOOL`
checks a pack: its manifest and tables read, and it tells which of the
game data's texts and units the pack misses and which `-from` fields are
stale. `--catalogue` compares `interface.tdf` with catalogue files of the
engine's words.

### Words in pictures

Some words are part of the game's pictures: the order buttons, the top
bar's METAL and ENERGY, the PAUSED, VICTORY! and DEFEAT titles and the
save and load dialogs' titles. A pack's `pictures.tdf` names such a picture and
gives its words, which the game draws over the player's own picture in
the bundled fonts as it loads, after painting out the old words. Each
section names a GAF sequence as `[<file>.gaf/<sequence>]` or a bitmap as
`[<file>.pcx]`; `text` gives the captions (`|` between frames or spots),
`area` their spots as `x,y,width,height` and `align` left, centre or
right. Leave a picture out, or give its section no keys, and it keeps its
art; leave out `pictures.tdf` and no picture changes. A picture the game
data holds in the language's own folder (`bitmaps-<word>`,
`anims-<word>`) already shows its words and is left as it is.

## Simplified Chinese

Simplified Chinese (`zh-Hans`, the game data's word `Chinese`) is a
download, `zh-Hans.oalang`, of about 3.5 MB, from Core Prime's registry or
the open-annihilation/languages releases (`zh-Hans-r1`, and `zh-Hans-r2`,
which translates 0.8's new screens). It is not in the game. Settings ›
Language lists it, as 简体中文, under AVAILABLE TO DOWNLOAD, and choosing it
downloads it; a player who chose it before 0.8, or whose computer is set to
Chinese, sees the notice at start that offers it
([choosing the language at start](#choosing-the-language-at-start)). The
pack brings its own font, and its warm-up text is the pack's
`warmup.txt`.

Once the pack is installed, the language is drawn only in the modern
fonts, so choosing it turns Use modern fonts for game text on and keeps
it on while it is shown; its text is UTF-8. The pack asks for Unicode
multiplayer chat. A mission's briefing, a unit's name and the game's
texts come from the pack wherever the game data has none in Chinese.
Names of companies are left out of the pack, so they show as the game
data writes them; the game itself is named by its Chinese title, 横扫千军.

While Simplified Chinese is shown, ideographs are drawn from the pack's
face, at 12 px at the least, whatever the Text size, and the pack's
warm-up text is drawn ahead when the language is chosen. Lines break at
spaces and between any two of those characters, never starting a row with
a closing mark, comma or full stop, and never ending one with an opening
mark; a Latin word or a number stays whole. Mission briefings in that
language wrap the same way and are drawn in the modern fonts. A character
the fonts lack draws as a box.

Text is typed in Chinese through the operating system's input method;
[Typing](typing.md) says how, and how saves and recordings are named in
any script.

## Mods

A mod's `translate.tdf` takes the place of the game's, as 3.1c reads the
first copy the archives hold, and its units' language keys are read like
the game's. A mod may carry language packs in its own `languages/<tag>/`
folders, which come before the game data. A mod's pack adds text only and
does not add a language to the setting. A mod profile's `strings`
replace English texts; they are not shown yet.

## Unicode multiplayer chat

A chat line in a shared game is one 64-byte record of text, which 3.1c
reads in the game's 8-bit code page (Windows-1252). Enable Unicode
Multiplayer Chat, in the Language section of the OA settings
(`open-annihilation.unicode-chat`, off by default), sends and reads chat
in UTF-8 between the machines that have it on, so that players can chat
in any script, Chinese among them. A language that needs it, as Simplified
Chinese does, turns it on and holds it on while it is shown, and so does a
mod profile that sets
[ui.text-rendering](mods/standard-hacks/ui.text-rendering.md) `unicode`.

- **Saying so.** A machine with the setting on says so in the setup block
  it sends (record 0x20), in three bytes 3.1c carries unchanged and never
  reads: `U` and `8` at +0xB5 and +0xB6, and bit 0 of +0xB7.
- **Sending.** Each machine gets a line in one form: UTF-8 when its setup
  block says so, else the code page with `?` for each character the code
  page lacks. When every machine reads the same form the line goes once
  to all of them; when they differ, one copy goes to each machine. A
  record all in ASCII reads the same in both forms and goes once, so a
  line all in ASCII that fits one record goes as it would with the
  setting off. The alliance lines and the battle room's "does not have
  this map" go in English with the name as typed (see
  [What is shown in it](#what-is-shown-in-it)), and in these forms too.
- **Long lines.** A line longer than one record goes as up to four
  records, each the speaker's `<Name> ` and a part of the line, cut
  between whole characters, at a space when one lies within the part's
  last 16 bytes. What does not fit in four is left out. The speaker's own
  log shows the same lines.
- **Commands.** A line whose text starts with `+` or `.` is a command,
  which the game and the recorders read: it goes as one record, as typed.
- **Reading.** A line from a machine whose setup block says UTF-8 is read
  strictly as UTF-8: each byte that starts no well-formed sequence (an
  overlong form, a surrogate, a value past U+10FFFF, a sequence cut short
  or a stray continuation byte) shows as `?`. A line from any other
  machine is read in the code page, or, while the setting is on, as UTF-8
  where it is well-formed UTF-8.
- **Recordings.** A recording keeps one copy of each line: in UTF-8 for
  each player this machine sent or heard UTF-8 from, whose setup block in
  the recording says so, and as it was heard for the others. Playing it
  back shows those lines as they are with the setting on and in the code
  page with it off.

Players with the setting on and off play together: one with it off reads
`?` for each character its code page lacks. The setting is not one of the
game's rules and is outside every profile's hash, and the simulation never
reads chat. With it off, nothing changes on the wire: the setup block's three
bytes stay as they are, and a line goes as one record holding its first
64 bytes, cut between whole characters, as 3.1c sends it.

## The engine's own words

The OA settings dialog and the engine's own notices are written in English
and pass through the interface catalogue (`oa/data/languages/interface_text.hpp`):
TDF files in a `languages` folder beside the game's `fonts` folder, and
each language pack's `interface.tdf`, each section naming an English text
and each key a language's tag:

```
[Mouse wheel zoom]
	{
	de = ...;
	fr = ...;
	}
```

The engine ships `languages/interface.tdf` in that folder, with these
words in German (`de`), French (`fr`), Spanish (`es`) and Italian (`it`),
so those languages show Open Annihilation's own screens without a
download. The file is read with the other catalogue files, before any
pack, by the same catalogue a pack's `interface.tdf` is added to. A later
file replaces a text. The four languages address the player informally
(German du, French tu, Spanish tú, Italian tu), as games in those
languages usually do. The names Open Annihilation, OA, Core Prime, Total
Annihilation and Cavedog Entertainment are left as written. A native
speaker should still review each language.

The Simplified Chinese pack's `interface.tdf` translates them for
`zh-Hans`; the player's packs replace a packager's words, and a mod's
replace both. The engine ships no packs. These are the words: the dialog's
section names and headings, its labels, hints, values (`per player`,
`fps`, `Desktop`, `None`, `cells`), switch and level captions, lock texts,
Hardware acceleration's status lines, the footer's buttons, `System default`
and the header's title; the names of the standard hacks and their areas
in Developer Mode's list (`Deterministic Wind`, `Interface`: each hack's
and area's title in the mod registry), which the list sorts as the
language shown writes them; the notices about missing skirmish and
multiplayer maps, the end of the game's missions, the graphics card and
driver, and settings that were not saved; and network play's own notices
in the message log, which 3.1c does not have: the autopause lines, a
player who tried to unpause, cheats enabled, VerCheck reports and the
votes to reject a player. A line the host's recorder sends as chat, such
as the battle room's autopause line, shows as it arrives, as all chat
does.

The touch and gamepad controls' words come from the same catalogue, after
3.1c's own table. Where one English word stands for two things, the
control is looked up by a longer text that says which, and without a
translation of that text the word shows in English: the right rail's
NEXT, which centres the next unit, by `NEXT UNIT`, as SELECT ▾'s item is;
the build pages' NEXT, on the build ring and in the phone's drawer, by
`NEXT PAGE`; and the rings' ARM, which arms the aimed order, by
`ARM ORDER`, since 3.1c's table holds `ARM` as the side's name.

A translator finds the words a pack lacks without reading the sources.
This build lists every English text it passes to the catalogue:

```
oa-tool interface-words --json
```

That prints a JSON array of those texts, sorted, each text once. The
languages repository compares the array with a pack:

```
python3 tools/coverage.py --oa-tool /path/to/oa-tool zh-Hans
```

A `missing:` line is an English text the pack does not translate. A
`not in the engine:` line is a section the build no longer looks up. A
`cannot be a section:` line is a text the table reader cannot hold,
because the text starts or ends with a space, or holds a line break,
`]`, `//` or `/*`. The looked-up text is the section name: a joining
space, or the colon and space after a sentence, is written beside the
lookup and is not part of the text.

## Adding a language

A language is usually a language pack. Put the pack in the player's
`Languages` folder, or in the `languages` folder beside the game, and
Settings › Language offers it, named in itself, once the game starts. The
operating system's locale can choose it, and every lookup above reads it,
from game data that holds its text under its word, from the pack, and
from catalogue files that hold the engine's words under its tag.

A registry entry, one line of `src/data/languages/src/registry.inc`, is
needed only for a language 3.1c's data holds, so the game knows it without
a pack: its tag, its name in itself, its name in English, the word the
game data knows it by, the system locales that choose it, the languages
it falls back to and what drawing it needs.

A language the engine knows a pack exists for, but which is not installed,
is offered only as a download, while a registry offers its pack.

A language whose letters are all in the game's 8-bit code page
(Windows-1252), as Portuguese's or Dutch's are, can be drawn in the game's
own fonts, with two things to know:

- A pack's tables are UTF-8, and the game reads them so only while the
  pack sets `unicode: true` or its text needs the modern fonts. A pack in
  the game's fonts without `unicode: true` shows each accented letter as
  two wrong characters.
- 3.1c's menu fonts (`anims/hattfont11.GAF`, `anims/hattfont12.GAF`) hold
  their box, not a letter, for ã, õ, Ã, Õ, Â, Ê, Ô and À, so a caption
  holding one of them is drawn whole in the modern fonts.

Others need drawing the game has in part or not yet:

- **Traditional Chinese and Japanese** need their own faces: a Simplified
  Chinese face draws their characters in Chinese forms.
- **Korean** needs a face that holds Hangul. The faces that travel with
  the game hold the Korean language's own name and no more of it; a
  Korean pack brings its own. Korean is written with spaces between its
  words, so its lines break at those spaces and keep each word whole, as
  Latin text does; a word breaks inside only when it is wider than the
  line.
- **Hindi** needs a Devanagari face and complex text shaping (HarfBuzz),
  since its letters join and change order; the FreeType-only drawing lays
  characters side by side.

The registry records these needs (`TextNeeds`), and the setting offers only
the installed languages this build draws.
