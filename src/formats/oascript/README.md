# Director scripts (.oascript)

`oa-formats-oascript` reads and writes director scripts: the recording to
replay, the video to make of it and the camera's shots. A script is text in
one of two forms, strict JSON (RFC 8259) or a subset of YAML 1.2, and both
read into the same document tree. The typed `Script` is decoded from that
tree and encoded back into it.

## Entry points

`oa/formats/oascript/document.hpp`, the document tree:

- `detect_form` tells the form: the first byte that is not white space,
  after an optional UTF-8 byte order mark, is `{` for JSON; anything else is
  read as YAML.
- `read_document` reads a text into a `Node` tree whose top level is a
  mapping; `ReadError` says what went wrong and where (a 1-based line and a
  1-based byte column; columns of the first line count from after the byte
  order mark).
- `write_document` writes a tree as JSON or YAML.
- `find_entry`, `decimal_text` and `compare_decimals` help callers read the
  tree.

`oa/formats/oascript.hpp`, the script:

- `decode_script` and `read_script` check a tree or a text against the
  script format and fill a `Script`, reporting every error and warning with
  its key path (`director.shots[2].cameraEnd.position.y`) and position.
- `encode_script` and `write_script` build the tree and the text of a
  `Script`.

The library reads and writes byte spans and strings; it opens no files.

## Numbers

Numbers are exact decimals (`Decimal`, a mantissa and a count of places),
read and written digit by digit; no binary floating point touches them, so
a script reads and writes alike on every platform. A number keeps the places
it was written with (`1.50` has two), an exponent is folded in (`2.5e2` is
250), and leading zeros count for nothing. More than 18 significant digits
or 18 places is `number_out_of_range`.

## The YAML subset

Block mappings and block sequences nested by space indentation (a sequence
may sit at its key's indentation), flow mappings and sequences that may span
lines (a trailing comma and an entry without a value are allowed, as YAML
allows), plain, single-quoted and double-quoted scalars (all of YAML's
escapes), `#` comments, one optional `---` before the document and a `...`
after it. Plain scalars resolve by YAML 1.2's core schema with decimal numbers
only; `yes`, `no`, `on` and `off` stay strings. Keys are plain or quoted
scalars.

Refused, each by its own status: a tab in indentation, anchors, aliases,
tags, block scalars, directives, complex keys, a second document,
hexadecimal and octal numbers, `.inf` and `.nan`. Also refused: control
characters other than tab, carriage return and line feed; a carriage return
not followed by a line feed; a quoted scalar or a plain scalar that runs onto
a second line; content after `---` on its line.

## Writing

The same tree always gives the same bytes. Entries keep their order, lines
end with a line feed and the text ends with one. The top-level mapping is
written one entry per line. Below it, a mapping or sequence whose children
are all scalars is written in flow style when its whole line (indentation,
the key or dash before it, and in JSON the comma after it) fits in
`max_flow_width` bytes; an empty one is always `{}` or `[]`; everything else
is written in block style (YAML) or one entry per line (JSON), two spaces a
level. An empty top-level mapping is written `{}`, which YAML reads as JSON.

YAML strings and keys are written plain only when they read back as the
same string and hold nothing but letters, digits, multi-byte characters,
spaces inside, and `_ . / - + ( ) = ; ~ $ ^`; they never start with a digit,
a sign, a point or a space, and `yes`, `no`, `on` and `off` in their usual
spellings are quoted, so that YAML 1.1 readers read them as strings too.
Everything else is double-quoted with JSON's escapes. Both forms escape
control characters, C1 control characters, U+2028, U+2029 and U+FEFF as
`\uXXXX`.

`read_document(write_document(tree))` gives the tree back in both forms, and
`decode_script(encode_script(script))` gives the script back.

## The script

The keys, their defaults and their ranges are documented in
`oa/formats/oascript.hpp`. Omitted optional keys take their defaults; unknown
keys are errors; a transition's `type` defaults to `dissolve`, and its
`duration` is required. A camera's `orientation` angles default to 0, and a
roll that is not zero gives a warning: a camera does not roll. Errors are reported in the order the
decoder meets them: the entries of each mapping in document order, then that
mapping's missing required keys; the check of `endTick` against the last
shot comes after the rest of `director`.

## Limits

Every read is bounded before anything is allocated for it:
`max_input_bytes` of text, `max_nesting_depth` levels, `max_node_count`
nodes, `max_string_bytes` per key or string. `too_large` carries no position
(line and column 0); every other failure carries one.

## Tests

- `formats-oascript-document` (`tests/document_test.cpp`): form detection;
  the same script in both forms reads to the same tree; the values of each
  form (escapes, surrogate pairs, the core schema); every read status with
  its position; each limit at and past its bound; exact decimals; the
  writers' exact bytes for a fixed tree; round trips of hard strings and of
  random trees under a fixed seed; a sweep of every truncation of the
  example scripts and of damaged copies under a fixed seed.
- `formats-oascript-script` (`tests/script_test.cpp`): defaults; a script
  that sets every key, in both forms; every decode error and warning with
  its key path and position; the encoder's key order and the writers' exact
  bytes; `decode(encode(s)) == s` and round trips through both forms; a
  sweep of truncated and damaged scripts.

## Limitations

- Multi-line scalars, YAML's folding of quoted scalars across lines, and
  single-pair mappings inside flow sequences (`[a: 1]`) are not supported.
- Strings keep UTF-8 as read; no Unicode normalisation.
- The decoder checks what it can without the map or the recording; map
  bounds and the recording's length are checked by their users.
