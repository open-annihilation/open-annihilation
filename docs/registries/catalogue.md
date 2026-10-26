# Catalogue

A catalogue lists the packages a registry offers. OA reads the file whole,
at most 16 MiB, as UTF-8 JSON without a byte-order mark (RFC 8259), nested
at most 64 objects and arrays deep. `catalogue` MUST be `1`. A member this
version does not know is ignored. A member named twice in one object,
anywhere in the tree, refuses the whole catalogue. One package that breaks
a rule is left out, the reason is kept for the first 100 such packages, and
the rest stay. Two packages with the same key, ignoring case, are both left
out.

[examples/catalogue.json](examples/catalogue.json) has one entry of each
kind. Its schema is
[schemas/catalogue.schema.json](schemas/catalogue.schema.json). The bytes of
that file are what [examples/catalogue.json.sig](examples/catalogue.json.sig)
signs.

## Top level

| Member | Required | Rule |
|---|---|---|
| `catalogue` | yes | The whole number `1`. |
| `registry` | yes | The registry id, as in the [descriptor](descriptor.md). |
| `sequence` | yes | A whole number, at least 0. It never goes backwards. |
| `generated` | yes | UTC time. `expires` MUST be later. |
| `expires` | yes | UTC time, after `generated`. |
| `downloads` | no | An `http` or `https` address. A query is allowed here. Informational. |
| `keys` | no | At most 8 current signing keys. See [signatures.md](signatures.md). |
| `mirrors` | no | At most 8 `http` or `https` addresses with no query. |
| `icon` | no | A picture reference ending `.png`. The registry's own icon. |
| `packages` | yes | The entries. At most 20,000. An empty array is a catalogue. |

A UTC time is `YYYY-MM-DDTHH:MM:SSZ`. A fraction of a second may follow the
seconds, a dot and one or more digits; it is accepted and does not change
the second. The date is a real Gregorian day, 29 February included in a
leap year. The hour is 0 to 23 and the minute and second are 0 to 59.

A whole number is a JSON number with no fraction and no exponent, inside
64-bit signed range. `1` is a whole number. `1.0` is not, except for
`coverage`, which is read from the spelling of the number.

`keys` uses the descriptor's key rules: `id`, `public` as `ed25519:` and 44
characters of base64, ids unique, public keys unique, at most 8. A member of
a key object other than `id` and `public` is ignored. An empty list is not
a rotation. See [signatures.md](signatures.md).

`downloads` on a catalogue is any `http` or `https` address the URL reader
accepts, including one with a query. The descriptor's `catalogue`,
`mirrors` and `api` are `http` or `https` and still forbid a query.

## Every package

| Member | Required | Rule |
|---|---|---|
| `kind` | yes | `oamod`, `oamap`, `oalang`, `expansion`, `oamut`, `oabrain` or `oakeys`. |
| `id` | yes | The package key. No key contains `@`. |
| `name` | yes | 1 to 64 bytes. |
| `release` | yes | A whole number from 1 to 2147483647. |
| `size` | yes | A whole number of bytes from 1 to 4294967296. |
| `sha256` | yes | 64 lower-case hex digits. |
| `file` | yes | A catalogue reference whose last segment is `<sha256>.<kind>`, or `<sha256>.oaexp` for an expansion. |

A mod, map-pack, mutator, brain or key-layout key is lower-case kebab-case
of 1 to 64 bytes, and a key is unique across kinds, so a mutator's, a brain's
or a key layout's id must not be any other package's id. A language key is a language tag of 2
to 35 bytes: a first part of letters (`A-Z` or `a-z`), then hyphen-joined
parts of letters and digits.

A reference is absolute from the host root (`/v1/p/<name>`) or relative to
the catalogue's folder. It is at most 512 bytes. It has no scheme, no `//`,
no `.` or `..` segment, no empty segment, and no `\`, `%`, `?`, `#`, `:`,
space, control character or byte above 0x7E. OA resolves it against the host
that served the catalogue. The last segment of `file` MUST be the lower-case
hex digest, a dot, and the kind, except an expansion, whose last segment is
`<sha256>.oaexp`. A registry SHOULD put the file at
`/v1/p/<sha256>.<kind>`, and an expansion at `/v1/p/<sha256>.oaexp`.

| Member | Required | Rule |
|---|---|---|
| `version` | mods, expansions, mutators, brains and key layouts | 1 to 32 bytes. A mutator's, a brain's or a key layout's version is a mutator version: one to three dotted parts, each 0 to 65535 with no leading zero. Optional for a map pack or a language pack. |
| `revision` | mods only | A whole number from 1 to 65535. If any other kind names it, the same rule applies. |
| `publisher`, `author` | no | 1 to 64 bytes when present. |
| `summary` | no | 0 to 200 bytes. |
| `notes` | no | 0 to 2000 bytes. |
| `homepage` | no | 1 to 256 bytes, `http://` or `https://` in either case, at least one byte after the scheme, and no ASCII control, delete or space. |
| `tags` | no | At most 16. Each is lower-case kebab-case of 1 to 32 bytes. |
| `badge` | no | A reference ending `.png`. |
| `published` | no | The day this release was published, `YYYY-MM-DD`: a real Gregorian day, 29 February included in a leap year, with nothing before or after it. Any kind may name it. |
| `requires` | no | An object. `engine` is a range, below. `base` is any string and is copied as written. On a mutator, `mutators` is the prerequisites, under Mutators. `mutators` is ignored on a brain and on every other kind. Other members are ignored. |
| `sim_hash` | no | 64 hex digits, either case. |
| `hacks` | no | `sim` and `view` are whole numbers from 0 to 2147483647. `ids` is at most 256 hack ids, each 1 to 64 bytes of `a-z`, `0-9`, `.` and `-`. Other members are ignored. |

`requires.engine` is 1 to 4 comparisons in at most 64 bytes, separated by
commas that spaces may surround. Each comparison is `>=`, `>`, `<=`, `<` or
`=`, optional spaces, then `MAJOR.MINOR.PATCH`. Each part is 0 to 65535
with no leading zero; a lone `0` is a part. In YAML a value that starts
with `>` is a block scalar unless it is quoted. In the catalogue, which is
JSON, quote it as a string anyway.

`published` is the day the publisher released this entry's package, not
the day the catalogue was signed, so it stays the same as later catalogues
list the same release. The Library's New filter lists the ten entries of a
tab with the latest days, and an entry without `published` is never among
them. `release` and `revision` count the releases of one package and say
nothing of when any came out.

A picture reference, a catalogue `icon`, a package `badge` or a map
`preview`, MUST be a valid reference ending `.png`. OA does not require
the file name to be a hash. A registry SHOULD still serve pictures at
`/v1/i/<sha256>.png`. `icon` is signed with the rest of the catalogue. An
older reader ignores it.

## Mods

A mod (`oamod`) MUST name `version` and `revision`. The manifest inside the
package is described by the
[OAMOD standard](../mods/oamod-standard.md).

## Map packs

A map pack (`oamap`) MUST name `maps`, a non-empty array. Each map is an
object:

| Member | Required | Rule |
|---|---|---|
| `map` | yes | `<stem>@<package id>` with exactly one `@`, a non-empty stem, and a suffix that is exactly this package's id. |
| `name` | yes | 1 to 64 bytes. |
| `players` | yes | A whole number from 1 to 10. |
| `size` | yes | Width `x` height. Each side is 1 to 256, with no leading zero. |
| `preview` | no | A picture reference ending `.png`. |
| `compatible` | no | An object of names to booleans, in the order written. Each name is 1 to 256 bytes. |

`size` is the text `16x16`, not an object. A name in `compatible` is not a
package key. The names published first name the base game, then each
required expansion in the built-in order, then a mod: `totala`,
`totala+core-contingency`, `totala+<mod>@<revision>`. Music is not one of
those expansions. `true` means that target fits. The same rows are also
published under the older names,
`ta-3.1c` and `<mod>@<revision>`, so an older engine still finds its own.
OA reads a new name when the table has one, and an older name only when it
does not. The manifest inside the package is described by the
[OAMAP standard](../maps/oamap-standard.md).

## Mutators

A mutator (`oamut`) MUST name `version`, and that version MUST be a mutator
version: one to three dotted parts, each 0 to 65535 with no leading zero,
in at most 32 bytes. The file's last segment is `<sha256>.oamut`.
`requires.mutators`, `conflicts_with`, `layers` and `clashes` are read only
on a mutator. On a brain and on every other kind they are ignored.
`works_with` and `options` are read on a mutator and on a brain, by the
rules in this table, and ignored on every other kind.

| Member | Required | Rule |
|---|---|---|
| `requires.mutators` | no | At most 16 objects, in the order the prerequisites apply. Each `id` is a mutator key that is not this entry's id and appears once. `version` is optional and is a version range. Other members of an object are ignored. |
| `conflicts_with` | no | At most 32 objects of the same shape as `requires.mutators`. None is this entry's id or one of its prerequisites. |
| `works_with` | no | At most 32 strings. Each is a condition: `*`, `ta-3.1c`, or a mod id with an optional version range. `*` stands only alone. |
| `layers` | no | At most 16 strings. A known layer name is kept once, in the order written. An unknown name is skipped. |
| `clashes` | no | `add` or `replace`. Absent stays absent; the game's default then applies. Any other value, including `Add` and a number, skips the entry. |
| `options` | no | At most 16 objects, below. An option of an unknown type is skipped. Other members of an object are ignored. |

The layer names are `files`, `units`, `weapons`, `features`, `sides`,
`tables`, `menus`, `maps` and `hacks`.

An option names `id` (an option id), `label` (1 to 64 bytes) and `type`.
An option id is a Lua name of 1 to 32 bytes, `[a-z][a-z0-9_]*`. Lua 5.4's
reserved words are not ids (`and`, `break`, `do`, `else`, `elseif`, `end`,
`false`, `for`, `function`, `goto`, `if`, `in`, `local`, `nil`, `not`, `or`,
`repeat`, `return`, `then`, `true`, `until`, `while`), matched in that case.
`format`, `ends` and `for_` are ids. `For` is not, because of its capital.
A reserved word leaves the package out, as any other refused mutator member does.
`type` is `toggle`, `choice` or `number`. A choice names `values`, 2 to 16
numbers or 2 to 16 identifiers, all of one kind, and may name `labels`, one
string of 1 to 64 bytes for each value. An identifier is 1 to 32 bytes of
ASCII letters, digits, `_`, `-` and `.`. A number names `min`,
`max` and `step`: numbers, with `min` below `max` and `step` above zero. A
toggle names nothing more. `default` is optional: `true` or `false` for a
toggle, one of the values for a choice, or a number on a step from `min` to
`max` for a number.

`tags` and `badge` keep the rules above. A mutator's `badge` is its icon,
and `tags` are at most 16.

An older reader leaves an `oamut` entry out as an unknown kind, before the
duplicate-key check, so neither the entry nor its key affects anything else.
A reader that knows `oamut` applies the duplicate-key rule across kinds. The
manifest inside the package is described by
[oamut.yaml](../../src/formats/oamut/README.md) until the mutator standard
exists.

## Brains

A brain (`oabrain`) MUST name `version` and `levels`. `version` is a mutator
version: one to three dotted parts, each 0 to 65535 with no leading zero, in
at most 32 bytes. The file's last segment is `<sha256>.oabrain`. A brain's
key is lower-case kebab-case, and it is unique across kinds.

| Member | Required | Rule |
|---|---|---|
| `levels` | yes | 1 to 6 objects, in the order written. `id` is lower-case kebab-case of 1 to 32 bytes and appears once. `name` is 1 to 64 bytes. `difficulty` is `easy`, `medium` or `hard`. A level of any other difficulty is skipped. An entry left with no level is left out, and the reason is `levels names no level this build knows`. Other members of a level are ignored. |
| `full_awareness` | no | `true` or `false`. Absent is false. Any other type leaves the entry out. |
| `network_bytes` | no | A whole number from 0 to 67108864: the networks' bytes together. Absent is 0. Any other value leaves the entry out. |
| `works_with` | no | The mutator's rule, above. |
| `options` | no | The mutator's rule, above. An option of an unknown type is skipped. |
| `requiredExpansions` | no | The same rule as on every other kind. At most 16 expansion ids, none repeated. An empty list is allowed. Each must extend a game the entry supports. |
| `supportedGames` | no | The same rule as on every other kind, when written. At most 16 base-game ids, none repeated. An empty list is refused. |
| `requires.engine` | no | The same range as on every other kind. |
| `tags` | no | The same rule as on every other kind: at most 16, not limited to the nine a brain manifest allows. |
| `badge` | no | The same rule as on every other kind. A brain's badge is its icon. |

`requires.mutators`, `conflicts_with`, `layers` and `clashes` are ignored on
a brain. A brain publishes the expansions its manifest names under
`requires.expansions` as `requiredExpansions`, the member the Library already
reads as which expansions a package needs. When `supportedGames` is omitted,
an expansion in `requiredExpansions` must extend `totala` (both base games
for a language pack).

An older reader leaves an `oabrain` entry out as an unknown kind, before the
duplicate-key check, so neither the entry nor its key affects anything else.
The manifest inside the package is described by
[oabrain.yaml](../../src/formats/oabrain/README.md) until the OABrain standard
exists.

## Key layouts

A key layout (`oakeys`) MUST name `version` and `base`. `version` is a mutator
version: one to three dotted parts, each 0 to 65535 with no leading zero, in
at most 32 bytes. `base` is `oa-default` or `none`, as written. The file's
last segment is `<sha256>.oakeys`. A key layout's key is lower-case
kebab-case, and it is unique across kinds. The manifest inside the package
is described by [the key-layout standard](../keys/oakeys-standard.md).

| Member | Required | Rule |
|---|---|---|
| `base` | yes | `oa-default` or `none`. |
| `works_with` | no | The mutator's rule, above. |
| `keyboard` | no | `any`, `ansi`, `iso` or `jis`. Absent is stored as `any`. Any other value leaves the entry out. |
| `pointer` | no | `right-click` or `left-click`. Absent names none. Any other value leaves the entry out. |
| `helpers` | no | At most 6 of `toggle`, `sticky-modifiers`, `hold-to-confirm`, `pointer-keys`, `radial-orders` and `edge-scroll`, each once, kept in the order written. An unknown name is skipped. A list longer than 6, a repeated known name, or a value that is not text leaves the entry out. The limit is the list's length before unknown names are skipped. |
| `changes` | no | A whole number from 0 to 2048: how many actions the layout binds. Absent is 0. Any other value leaves the entry out. |
| `triggers` | no | At most 128 strings, each a trigger of the chord grammar once, kept in the order of first use. A string this build cannot read is skipped. A list longer than 128, a repeated trigger, or a value that is not text leaves the entry out. The limit is the list's length before unreadable strings are skipped. A trigger is one byte from 0x21 to 0x7E, `@` followed by letters and digits, a named key, a mouse button, a wheel turn (`WheelUp`, `WheelDown`, `WheelLeft` or `WheelRight`), or `Pad.` and a name whose first character is a capital letter and whose rest is letters and digits. A modifier on its own (`Primary`, `Ctrl`, `Alt`, `Shift` or `Meta`) is not a trigger. |
| `preview` | no | A picture reference ending `.png`, the keyboard picture for sites. |
| `tags` | no | The same rule as on every other kind. |
| `badge` | no | The same rule as on every other kind. |
| `requires.engine` | no | The same range as on every other kind. |

An older reader leaves an `oakeys` entry out as an unknown kind, before the
duplicate-key check, so neither the entry nor its key affects anything else.

## Language packs

Only a language pack is read for these members. On any other kind they are
ignored.

| Member | Required | Rule |
|---|---|---|
| `coverage` | no | A JSON number from 0 to 1, at most 18 significant digits. `1` and `1.0` are both accepted. |
| `english_name` | no | 1 to 64 bytes. |
| `word` | no | 1 to 32 bytes. |
| `locales` | no | At most 16 language tags. |
| `fallbacks` | no | At most 8 language tags. |
| `needs` | no | `game-fonts` or `modern-fonts`. |

The manifest inside the package is described by
[languages.md](../languages.md).

## Expansions

An expansion (`expansion`) MUST name `version` and `extends`. `id` is a
built-in expansion id. `extends` is that expansion's base games, in the
built-in order, and the list is not empty. A missing list is refused the
same way a different list is. The file's last segment is `<sha256>.oaexp`.
`name` is the ordinary catalogue name, 1 to 64 bytes. The manifest inside
the package requires the expansion's English name; the catalogue entry does
not. `revision` stays optional. The manifest inside the package is
described by the [OAEXP standard](../expansions/oaexp-standard.md).

## Sequence, expiry and how OA keeps a catalogue

For a registry that names a key, OA verifies the detached signature over the
exact bytes before it parses them. An unsigned registry skips that. The
version, the registry id and the sequence come next.

A lower sequence than the one OA already kept is refused. The same sequence
and the same bytes are unchanged. The same sequence and different bytes are
refused. A higher sequence replaces the catalogue.

A catalogue whose `expires` is at or before the clock OA checks with is
still used. OA marks it expired. It does not refuse some other expiry, and
it does not require a 30-day lifetime. `oa-tool registry init` signs an
empty catalogue for 30 days; that is a publisher's habit, and a registry
SHOULD set `expires` about 30 days after `generated`.

OA reads catalogues again when the one it holds is older than 6 hours at
start, and not more often than every 60 seconds from the Library. That is
the client's habit, not a field of the catalogue.

An added registry whose `keys` are present, not empty, and different from
the keys the player trusts reports the new set, so the player can accept a
rotation. Comparison is the id and the 32 bytes, in any order. A built-in
registry's keys in the catalogue are ignored. An empty key list is never a
rotation.
