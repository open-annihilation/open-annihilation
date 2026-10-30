# Layout manifests

`oa-formats-oakeys` reads, checks and writes a key layout's manifest,
`oakeys.yaml`, and the short text of its bindings. The Library, a shared
package and a paste all go through this file. Target `oa-formats-oakeys`,
header `oa/formats/oakeys/manifest.hpp`, namespace `oa::formats::oakeys`.

The text is the strict YAML a mod profile is written in
(`oa/formats/oamod.hpp`). `read_document` reads the bytes.
`read_package_keys` reads `homepage`, `tags` and `requires.engine`. The
author, works-with and strings are the mutator rules, called through
`oa/formats/oamut/shared.hpp`. A chord stays text, with the line and column
it was written at. Parsing a chord is another module's work.

## Entry points

- `read_manifest` reads one manifest. Every broken rule is reported, and the
  manifest is left unchanged when any rule is broken. A document the strict
  reader refuses is one problem on the key `document`. `describe` prints one
  problem as `oakeys.yaml:<line>:<column>: <key>: <message>`.
- `write_manifest` writes a manifest back in the key order below, with
  two-space indentation. Every string is double-quoted. Each binding is one
  flow sequence on one line. A chord is left bare when it is only letters,
  digits and `+`, and quoted otherwise, including a chord a bare scalar
  would read as a number, a boolean or null. `keyboard` is left out when it
  is `any`. An optional key that was left out is left out again. An empty
  helper list that was written is written again.
- `write_bindings_text` writes the paste: a comment
  `# oakeys bindings, base <base>`, then the bindings at the top level, in
  the same form as a binding in the manifest.
- `read_bindings_text` reads that paste. The base is the comment, and
  `oa-default` when the comment is absent. A text whose top mapping has an
  `oakeys` key is a whole manifest, read by `read_manifest`, and the comment
  is ignored. The manifest is left unchanged when any rule is broken.

## State

None. Callers pass bytes and a `Manifest` and get values back. Nothing here
is installed for the rest of the process, and nothing opens a file.

## Invariants

- `oakeys` is the integer 1. A later format is refused.
- A layout id is a mutator id: lower-case kebab-case of 1 to 64 bytes. The
  formats layer does not copy that grammar; it calls `oamut::mutator_id`.
  The ids `oa-default` and `classic` are refused. Any other id of that
  shape is kept, including one that starts with `own-`.
- A version is a mutator version of one to three parts, written in quotes.
  `parts` remembers how many were written, and the writer uses that.
- `works-with` and `strings` keep the order written. Bindings keep the
  order written. A chord's text is kept as written, and so is an action id.
- `base` is `oa-default` or `none`, byte for byte.
- A key the manifest does not define is refused with
  `<key> is not a key of oakeys.yaml`.
- `homepage`, `tags` and `requires.engine` follow the shared package-key
  rules. Tags are then limited to four. When the shared rules already
  report a tag problem, the four-tag limit is not reported again.
- A binding key is lower-case kebab-case words joined by dots, 1 to 96
  bytes, at least two words. Its value is a list of at most four strings.
  A list that is longer, or a value that is not a string, is refused, and
  that binding is not kept.
- `helpers` holds only the six keys below. `radial-orders` is `off`,
  `hold-right` or `hold-middle`. `edge-scroll` is `on` or `off`. A boolean
  helper is a boolean, not the string `"true"`.
- The manifest is replaced only when every rule passed. A refused read
  leaves the caller's manifest as it was.

## Manifest rules

The top-level keys, in the order `write_manifest` writes them, are
`oakeys`, `id`, `name`, `version`, `author`, `description`, `homepage`,
`icon`, `tags`, `base`, `works-with`, `requires`, `keyboard`, `pointer`,
`bindings`, `helpers`, `strings` and `imported-from`.

Required:

- `oakeys`: the integer 1. `1.0` and `"1"` are refused with `oakeys must
  be 1`.
- `id`: lower-case kebab-case, 1 to 64 bytes. `oa-default` and `classic`
  are `id is a built-in layout`.
- `name`: 1 to 64 bytes, with no control character.
- `version`: a quoted mutator version, such as `"1.0"`. A plain number, and
  an unquoted scalar, are refused with `write the version in quotes, as
  "1.0"`.
- `author`: the mutator author.
- `tags`: the shared tag rule, then at most four. More than four, when the
  shared rule passed, is `tags lists N tags; at most 4`.
- `base`: `oa-default` or `none`.

Optional:

- `description`: 1 to 2,000 bytes. A line feed is allowed. Any other control
  character is refused. Absent means no description.
- `homepage` and `icon`: the shared address rule, and a relative path
  ending in `.png`.
- `works-with`: the mutator rule, 1 to 32 conditions.
- `requires`: a mapping of only `engine`.
- `keyboard`: `any`, `ansi`, `iso` or `jis`. Absent means `any`, and `any`
  is left out of the written file.
- `pointer`: `right-click` or `left-click`. Absent means the file named no
  scheme.
- `bindings`: a mapping of action ids to chord lists, at most 2,048. Absent
  means no bindings. Each chord string keeps its own line and column, the
  first byte of the scalar, including the opening quote.
- `helpers`: `toggle` and `hold-to-confirm` are lists of action ids, and may
  be empty. `sticky-modifiers` and `pointer-keys` are booleans.
  `radial-orders` and `edge-scroll` are the names above. A helper left out
  stays unset, which is different from an empty list.
- `strings`: the mutator strings block, at most 16 languages.
- `imported-from`: `tool`, `file` and `date`, each required when the block
  is present. `tool` is 1 to 64 bytes, `file` 1 to 256, `date` 1 to 32, with
  no control character.

Indexes in a path start at 0. A problem is reported at the value: the first
byte of a scalar, including its opening quote, the `[` of a flow sequence,
or the first key of a block mapping. A binding's own key is reported at the
key. A missing required key is reported at the document's start. A position
the strict reader leaves at zero is reported as line 1, column 1.

## Tests

`formats-oakeys` (`tests/manifest_test.cpp`) reads the standard's example,
checks the writer's text reads back to the same manifest and writes back
byte for byte, refuses each broken rule with its key and position, refuses
a text longer than 256 KiB and a list of 2,049 bindings, and round-trips a
chord of every printable character but space, the paste, and a whole
manifest read as a paste.

## Limitations

No file is opened, and no `.oakeys` package is packed or installed. A chord
stays text: this module does not parse it, and it does not know which
actions this version has. `helpers` are stored and written; they are not
applied to the player's settings. An id that starts with `own-` is kept.
The paste does not carry the layout's id, name or the other manifest keys.
