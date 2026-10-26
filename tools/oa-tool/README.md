# oa-tool

`oa-tool` reads archives and images from a game directory, and prints or
writes what a script asks for. Mod authors and translators run it by hand.
The checks under `tools/` run `list` and `extract` and match their output.
`pack` writes a mod or language package. `check` runs the installer's own
checks on a package and reports the facts a catalogue lists. `catalogue`
makes a publisher key and signs or checks a catalogue. `registry` makes an
empty signed registry and copies one into a folder.

## How to run it

From a build tree, `build/oa-tool` is the program.

- No arguments prints the command list on standard error and exits 2.
- `oa-tool --version` prints the game's version line (the same line as
  `open-annihilation --version`) on standard output and exits 0. It reads
  no file.
- `oa-tool help`, `oa-tool --help` and `oa-tool -h` print that list on
  standard output and exit 0.
- `oa-tool help COMMAND` and `oa-tool COMMAND --help` (or `-h` before any
  `--`) print that command's usage line and explanation on standard output
  and exit 0. `oa-tool help GROUP SUBCOMMAND` does the same for a
  subcommand.
- A group with no subcommand prints its own list on standard error and
  exits 2.

Options, for a command that has any, may be written before, between or
after the positional arguments. `--name value` and `--name=value` are the
same. `--` ends options. A command takes its options through
`parse_arguments` in `arguments.hpp`.

## Exit codes

| Code | Meaning |
|---|---|
| 0 | The command did what was asked. |
| 1 | The command ran and could not do it, or the answer is no. `Failure`, and any other exception, prints `oa-tool <command>: <message>` on standard error. |
| 2 | The invocation is not one the tool can run: no command, an unknown command, or the wrong number of arguments. The line says what is wrong and names `oa-tool help`. |

## Commands

| Command | What it does |
|---|---|
| `list ARCHIVE` | Prints each file in an HPI archive as `path`, a tab and its size in bytes. |
| `extract ARCHIVE ENTRY OUTPUT` | Writes one archive entry to OUTPUT and prints nothing. |
| `asset-extract ROOT ENTRY OUTPUT [ARCHIVE...]` | Writes ENTRY from loose files under ROOT, or from the named archives, and prints where it was read. A loose file wins. |
| `preview ARCHIVE PCX_ENTRY OUTPUT.ppm\|OUTPUT.png` | Decodes a PCX image from an archive as a PPM file, or a PNG file when the path ends in `.png`, and prints its size. |
| `decode-pcx INPUT.pcx OUTPUT.ppm\|OUTPUT.png` | Decodes a PCX file the same way. |
| `pack FOLDER [--out FILE] [--force] [--game-dir DIR]` | Packs FOLDER as a `.oamod`, a `.oalang`, a `.oamap`, a `.oamut`, a `.oabrain` or a `.oakeys`. The manifest is first, then every other file in byte order of its path. A file whose extension, without case, is png, jpg, jpeg, gif, ogg, mp3, zip, gz, bz2, xz, 7z, oamod, oalang or oamap, and an empty file, is stored; every other file is deflated. The same files give the same bytes. `--game-dir` is the game's data; map packs need the game's palette for previews. `--force` replaces an existing file. A map's `players` is the largest whole number in its OTA `numplayers` when every number is from 2 to 10, and otherwise the most start positions of a Network schema. The folder's `oamap.yaml` may leave `maps` out, or set a map's `name`, `description` or `players`. `size`, `preview` and `files` are computed. A mutator's default name is `<id>-<version>.oamut`. Its Lua is compiled and its rule files are parsed before the file is written, and neither is run. `--game-dir` with a mutator is a usage error. A brain's default name is `<id>-<version>.oabrain`. Its layout, its Lua and each network's SHA-256 are checked before the file is written. Lua is compiled and not run, and a network is hashed and not interpreted. `--game-dir` with a brain is a usage error. A key layout's default name is `<id>-<version>.oakeys`. The key-layout packer writes `oakeys.yaml` first, then every other file in byte order, with no clock time: a `.png` is stored and every other file is deflated. `--game-dir` with a key layout is a usage error. |
| `check FILE|FOLDER [--game-dir DIR] [--mod FOLDER[=KEY]]... [--accept-unimplemented-hacks] [--json] [--strict]` | Runs the checks an install runs before it writes, and reports the facts a catalogue lists. A map pack needs `--game-dir` and may name each further target with `--mod`. `--json` prints one JSON object, and a map pack adds `maps`. A mutator adds `summary`, `description`, `homepage`, `tags`, `author`, `requires`, `works_with`, `conflicts_with`, `layers`, `clashes`, `options`, `icon`, `entry`, `scripts`, `rules` and `legacy`, and its `revision` is null. `clashes` is `add` or `replace` when the manifest names it, and is left out when it does not. A brain adds `summary`, `description`, `homepage`, `tags`, `author`, `requires`, `works_with`, `levels`, `full_awareness`, `networks`, `network_bytes`, `options`, `icon`, `entry`, `scripts` and `data`, and its `revision` is null. `full_awareness` is always a boolean and `network_bytes` is always a whole number. `levels` lists `id`, `name` and `difficulty`. `networks` lists `id`, `file`, `sha256` and `size`. A key layout adds `summary`, `description`, `homepage`, `tags`, `author`, `requires`, `works_with`, `base`, `keyboard`, `pointer`, `helpers`, `changes`, `triggers`, `unknown_actions`, `clashes` and `icon`, and its `revision` is null. `keyboard`, `base`, `changes`, `triggers`, `unknown_actions` and `clashes` are always written. `--strict` refuses a key layout that has a warning and changes no other kind. A folder whose top holds `oakeys.yaml` is checked in place of a file, and the report names the folder. `--game-dir` with a key layout is a usage error. A key the package does not have is left out. Lua is compiled and not run, and a network is hashed and not interpreted. `--game-dir` with a brain is a usage error. Without `--json` each fact is a `key: value` line, a map pack then prints one line per map and target, and the last line is `result: ok` or `result: refused`. Exit 0 when the package would be installed, 1 when it would be refused or a map does not fit `ta-3.1c`. A mod that does not fit leaves that exit code unchanged. |
| `mutator pack FOLDER [--out FILE] [--force]` | Packs a folder whose top holds `oamut.yaml`, the same way `pack` does. A folder without `oamut.yaml` is a usage error. |
| `mutator check FILE [--json]` | Checks a `.oamut` and reports the same facts as `check`. A file of another kind is a usage error. |
| `mutator import INPUT... --out DIR [--id ID] [--version VERSION] [--tag TAG]... [--works-with CODE=ENTRY]... [--pack] [--force]` | Imports each `.tam` or zip into `DIR/<id>/`. A `.tam`'s companions are read from its own folder, so DIR is not that folder. A zip is read on its own, so DIR may be the folder that holds the zip. A zip's names are the names the archive recorded. `--id` and `--version` apply to one mutator. `--tag` replaces the `gameplay` tag. `--works-with` maps a compatibility code; `OTA` and `TACC` are already mapped. `--pack` also writes `DIR/<id>-<version>.oamut`. `--force` replaces an existing folder. One line per mutator, then its warnings. Exit 0 when every mutator was imported. A link is not followed. |
| `mutator diff --game-dir DIR [--mod-dir DIR] [--map NAME] [--seed N] [--mutator SPEC]... [--versus SPEC]... [--allow FILE] [--dump FILE]` | Mounts DIR the way the game does, and `--mod-dir` as a mod folder over it. Each `--mutator` and `--versus` is a package folder, with options after the last colon. An id with no path is refused. The report's change lines are `unit dropped NAME hidden\|copyright\|version`, `unit added NAME`, `file added PATH` or `file added PATH SHA`, `file hidden PATH`, `file replaced PATH OLD NEW`, `field PATH SECTION KEY: OLD -> NEW` and `menu added\|removed BUILDER PAGE BUTTON UNIT`. A missing side is `(absent)`. A section whose name repeats among its siblings is `[name@N]` from its second occurrence on. A byte below 0x20, and 0x7F, is written `\xHH` in a path, a section, a key and a value, in the report and in `--dump`. A journal id is added after a tab as `by <id>[,<id>...]`, for the section the journal names: `[unitinfo]` on a unit, `[<record>]` and `[<block>]` on a weapon or a feature, and `[globalheader]` or `[globalheader][schema N]` on a map. A field with no entry of its own names the file's owner. The header lines are `# oa-tool mutator diff 1`, `# game:`, `# mod:`, `# loadout:`, `# seed:` and, when `--map` is set, `# map:` with the map's name as the game's map list spells it. A map the game does not have is a usage error. `--versus` prints the left header, `# versus:`, the `left only` and `right only` lines, each matched difference as `allowed <line>`, then `allowed difference matched nothing: <pattern>` for each unused pattern, then `mutator diff: <n> differences, <a> allowed, <u> unused allowances`. Download lines are dropped. A unit both sides drop loses its reason, its FBI lines, its menu lines and every field of a GUI gadget whose `[COMMON] name`, or `name`, is that unit. A field path ends at ` [`. A file path may contain spaces and ends before its hash. An allow file ignores a leading BOM and blank lines, treats a `#` line as a reason and every other line as a whole-line pattern (`*` any run, `?` one character). A blank line ends a block. Each pattern needs a reason above it in its own block. `--dump` writes `path`, a tab, the source, a tab and the SHA-256, then one indented field line. Exit 0 when the report was written, or a comparison has nothing left and no unused pattern. |
| `brain pack FOLDER [--out FILE] [--force]` | Packs a folder whose top holds `oabrain.yaml`, the same way `pack` does. A folder without `oabrain.yaml` is a usage error. `oa-tool check` reports the brain's facts. |
| `brain check FILE --game-dir DIR [--mod FOLDER] [--level LEVEL] [--side N] [--json]` | Loads a `.oabrain`, or a folder whose top holds `oabrain.yaml`, against DIR. The folder's name is the brain's id. `--mod` is a mod folder layered over the game. `--level` selects a level; the first is used when it is omitted. `--side` is whose commander the brain's seat holds, from 0, and it is 0 when omitted. The check match uses the game's own definitions on a flat stand-in map and is never ticked. `--json` prints one object. Exit 0 when the brain was seated. |
| `brain net MODEL.onnx --calibrate DATA [--out FILE] [--weights int8 or int16] [--max-difference X] [--force] [--json]` | Converts an ONNX model of opset 11 to 21 into a `.oann`, quantises it with DATA and reports the difference from the floating-point model. `--weights` is `int8` by default. `--json` prints one object. An existing file needs `--force`. |
| `keys pack FOLDER [--out FILE] [--force]` | Packs a folder whose top holds `oakeys.yaml`, the same way `pack` does. A folder without `oakeys.yaml` is a usage error. |
| `keys check FILE|FOLDER [--json] [--strict]` | Checks a `.oakeys`, or a folder whose top holds `oakeys.yaml`, and reports the same facts as `check`. `--strict` refuses a warning. A file of another kind, or a folder without `oakeys.yaml`, is a usage error. |
| `keys picture FILE|FOLDER --out PNG [--width N] [--fonts DIR]` | Draws the keyboard picture of what the layout changes and writes a PNG. `--width` defaults to 1200. `--fonts` defaults to the fonts folder beside the program. |
| `keys show FILE|FOLDER|oa-default [--json]` | Prints each action with its context and chords after the base and the layout. `oa-default` is the base alone. `--json` prints an array of `action`, `context` and `chords`. |
| `catalogue keygen --id ID --out FILE [--passphrase-file FILE]` | Makes a new Ed25519 key, seals its seed with a passphrase and writes FILE. The file is created only when that name is new, and on macOS and Linux its mode is 0600. The passphrase is read from the terminal with echo off, twice, and has to be at least 12 bytes. `--passphrase-file` reads the file's first line instead. Prints `key:`, `public: ed25519:` and `fingerprint:`. |
| `catalogue public KEY_FILE` | Prints the same three lines. The passphrase is not used. |
| `catalogue sign --key KEY_FILE CATALOGUE [--out SIG] [--passphrase-file FILE]` | Signs CATALOGUE's exact bytes and writes CATALOGUE.sig, or SIG. The line is the one a catalogue check reads: `ed25519`, the key id and the signature. A catalogue this build would refuse is refused before the key is opened. The written pair is checked before the command reports success. Prints `signed <file> with <id>`. |
| `catalogue verify CATALOGUE [--sig SIG] (--descriptor FILE \| --registry ID --key ed25519:KEY [--key ...])` | Checks CATALOGUE against its signature, with the descriptor's id and keys, or with ID and the given keys. Each `--key` is read as the key the signature names, and the first is the one that is trusted. Prints `good signature by <id>` and the verdict, and exits 0 when the catalogue can be used. A refused catalogue prints the verdict and exits 1. |
| `engine-feed write --version V [--release N] --date YYYY-MM-DD --from V --page URL --mirror URL [--mirror URL]... [--notes FILE] [--rollout N] [--online-min V] [--channel stable] [--previous FEED] --out FEED PACKAGE...` | Writes a feed of one release. Each PACKAGE is a release file whose name carries `--version` and a package id, as a zip, a Windows setup or the macOS package. The command reads each file's size and SHA-256. `--release` defaults to that version's release number. `--notes` is a JSON object of language to lines. `--previous` adds the older releases of an earlier feed, five in all. The written file is read back before success. Prints the feed's path and the release number. |
| `engine-feed sign --key KEY_FILE FEED [--out SIG] [--passphrase-file FILE]` | Signs FEED's exact bytes and writes FEED.sig, or SIG. The line is the one a feed check reads. A feed this build would refuse is refused before the key is opened. The written pair is checked before the command reports success. Prints `signed <file> with <id>`. |
| `engine-feed check (FEED [--sig SIG] \| --url URL) [--channel stable] [--key ID=ed25519:BASE64]...` | Checks FEED against its signature, or fetches URL and URL plus `.sig`. Without `--key` the keys are this build's pinned keys. Each `--key` replaces those keys. Prints `good signature by <id>` and one line a release, and exits 0 when the feed can be used. A refused feed prints the verdict and exits 1. |
| `registry init FOLDER --id ID --name NAME --base-url URL --key KEY_FILE [--passphrase-file FILE] [--homepage URL]` | Makes a direct registry in FOLDER: `registry.yaml`, an empty catalogue signed for 30 days, its signature, and `v1/p/` and `v1/i/`. FOLDER must be missing or empty. `--base-url` is the http address the registry is served at, with no trailing slash. Prints the descriptor's path and the key's fingerprint. |
| `registry mirror URL FOLDER [--key ed25519:<base64>]` | Copies the registry at URL into FOLDER, laid out as on the host that served it. The signature is checked, a sequence lower than the one FOLDER holds stops the copy, and each package and picture is checked by SHA-256. A `.part` is resumed with a Range request. A tokens registry is copied through its download API. The catalogue and its signature are written last, byte for byte. |
| `interface-words --json` | Prints a JSON array of every English text this build passes to the interface catalogue, sorted, with each text once. The texts are the lists the modules that look them up export. |

## brain check

`brain check` reads a `.oabrain`, or a folder whose name is the brain's id and whose top holds `oabrain.yaml`, through the same file checks as `oa-tool check`. Lua that does not compile is refused before the game is mounted. `--game-dir` is required. `--mod` names a mod folder layered over that game the way the game layers one.

Before any of the brain's Lua runs, the manifest's `works-with` has to name the base game `ta-3.1c` when no mod folder is given, or the mod's id and version when one is. `*` names any mod and does not name the base game. A `requires.expansions` entry does not fit: this check has no expansions. A brain that does not fit is reported and nothing of it runs.

When the manifest fits, the check builds a match and does not tick it. The match holds the game's own unit, weapon and feature definitions, the computer players' profile and every type's build list, and a flat stand-in map of 64 by 64 cells with no features and no metal. Seat 1 is a computer seat on `--side` (the first side when `--side` is omitted) and holds that side's commander at the map's centre. Seat 0 is the local player and holds no units. The brain is seated with the game's facts: the content bundle is `totala`, and the base game a brain reads when no mod is mounted is `ta-3.1c`. A unit file that does not load is a warning and the check still runs. The match is not played.

The report is one fact a line: `brain`, `game`, `mod` (`none` when no mod folder was given), `load`, `fits` and `start`, each of those three with the instructions that phase used, then `error` when something failed, `lua`, `networks`, one `warning` line per unit file that did not load, and `result: ok` or `result: refused`. A phase that did not run reports `not run` and 0 instructions. The count steps by 1,000, so a short phase that did run can still report 0. `fits: none` means the brain defines no `fits`. `--json` prints one object whose keys are `check`, `ok`, `problems`, `warnings`, `id`, `version`, `level`, `game`, `mod`, `load`, `fits`, `start`, `error`, `lua_bytes` and `network_bytes`, in that order. `fits` adds `text` when the brain declined.

Exit 0 when the brain was seated. Exit 1 when it was not, the package's files were refused, the level is unknown, or the game or the mod cannot be mounted. Exit 2 when `--game-dir` is missing, the file is not a `.oabrain`, the folder holds no `oabrain.yaml`, or the arguments are not the command's.

## brain net

`brain net` reads an ONNX file with its own protocol-buffer reader and writes a `.oann`. The default domain (`""` or `ai.onnx`) at opset 11 to 21 is accepted. Any other domain is refused. The first dimension of every model input is the batch, either 1 or a name, and it is removed. A tensor that is only the batch becomes `[1]`.

The operators are `Gemm` (alpha 1, beta 1, transA 0, transB 0 or 1) and `MatMul`, optionally followed by an `Add` of a constant that is not also a graph output, which become dense; `Conv` (group 1, dilations 1, symmetric pads, kernel 1 to 15) becomes conv2d; `MaxPool` with no pads and no dilation becomes maxpool; `Relu`, `LeakyRelu`, `Tanh`, `Sigmoid` and `Softmax` (over the last axis, every other feature dimension 1) become those layers; `Add` of two computed tensors becomes add; `Concat` on an axis after the batch becomes concat; `Flatten` and `Reshape` to one feature dimension become flatten; `ArgMax` over the only non-unit feature axis becomes argmax; `Gather` on axis 0 of a constant table becomes embedding. `Identity` and `Dropout` are skipped. Anything else is refused, naming the node and the operator.

Scales are chosen in this order:

1. Read the calibration file. Each line is one JSON object, the line T03's `--brain-record` writes. A line whose `kind` is `sample` holds `features`, an object from a tensor name to `{"shape": [...], "values": [...]}`. The values are whole numbers in 65536ths. Every model input must be present on every sample line. Other kinds and other features are skipped, and a blank line is skipped. At least one sample is required. At most 100,000 samples are read; further sample lines are counted and skipped, and a warning names both counts.
2. Run the model in double on every sample and record each tensor's largest magnitude `M`. The pass uses the C library's `tanh` and `exp` for those layers.
3. A computed int16 tensor's exponent is the smallest whole `e` with `M <= 32767 × 2^e`, or `-15` when `M` is 0, held to -48..16. relu, leaky_relu, maxpool and flatten copy the input's exponent. tanh, sigmoid and softmax use -15. argmax uses 0. An embedding uses the table's exponent. An input uses the same rule, except an int64 input and a Gather's indices, which use 0.
4. A weight's exponent is the smallest `e` with `max|w| <= 127 × 2^e` for int8, the default, or `32767 × 2^e` for `--weights int16`. Each weight is `round(w / 2^e)`, half away from zero, held to the type's range. A bias uses `e(input) + e(weight)`. A bias that does not fit int32 at that exponent is refused, naming the layer. A dense or conv2d output whose magnitude would put it below `e(input) + e(weight)` is raised to that exponent, so the scale shift is 0. It is also held so the shift is at most 62.
5. `oann::write` checks the model and produces the bytes. A tensor name is the ONNX name in lower case, with every other byte replaced by `_`, cut to 31 bytes, and made unique with `_2`, `_3`.
6. `neural::Network::build` builds the integer network. Each sample's inputs are moved from 65536ths onto the input exponent with `neural::rescale`, the network is run, and each int16 output is turned back by `q × 2^e`. A difference is the largest absolute gap from the floating-point value. An argmax reports the share of samples on which the two agree.

The report is `model`, `written`, `sha256`, one `inputs` line and one `outputs` line per port (name, shape, exponent), then `layers`, `weights` (weight elements, not biases), `bytes`, `operations`, `charge`, `samples`, and one `difference` or `agreement` line per output. `--json` prints those facts as one object. Exit 0 when the file was written and every difference is within `--max-difference` (no limit when it is omitted). Exit 1 when the model cannot be converted or a difference passes the limit, and then nothing is written. Exit 2 is a usage error. `--out` defaults to the model's path with `.oann`. An existing file needs `--force`.

On one machine the same model and the same calibration file give the same bytes.

## Entry points

- `run_tool` in `command.hpp` turns an argument list into one command. The
  program's `main` passes it the process arguments and the standard streams.
  The overload that takes a table runs that table instead, which is how a
  test drives a group the tool does not ship.
- `commands` returns the table in `commands.cpp`.
- `parse_arguments` splits options from positional arguments.
- `read_file`, `write_file` and `write_image` in `files.hpp` move bytes and
  images. Each throws `Failure` when it cannot.

## State

A run reads the archives, loose files and images named on its command line
and writes the output file a command names. It keeps nothing after it exits.
`registry mirror` writes the folder it is given. When that registry asks for
an install id, the id is made for the run and kept only in memory: it is not
written and not printed. Commands print only through the `Output` they are
given, so a test can capture every line.

## Invariants

- `list`, `extract`, `asset-extract`, `preview` and `decode-pcx` print the
  same bytes on a successful run as they did before the command table, and
  that run exits 0.
- `pack` writes the manifest first and the other files in byte order of
  their paths, stored or deflated by the rule in its help, and the same
  files give the same bytes.
- `check` accepts a package exactly when an install would, and reports
  `sim_hash` and `full_hash` from the profile resolved without the packaged
  settings. A missing fact is left out. `revision` is null when a language
  pack has no packaging revision, and for every mutator and every brain. A
  mutator's Lua is compiled and its rule files are parsed, and neither is
  run. A brain's Lua is compiled and not run, and a network is hashed and
  not interpreted.
- The exit code is 0, 1 or 2.
- `catalogue sign` writes the signature with the catalogue check's own
  line, and reports success only after that check accepts the written pair.
- `catalogue keygen` creates the key file only when the name is new. On
  macOS and Linux the file's mode is 0600. The passphrase is not printed.
- Help lists commands in the table's order, and a group's subcommands under
  the group.
- `registry init` writes a descriptor the registry reader accepts and a
  catalogue the catalogue check accepts, and reports success only after that
  check's verdict is accepted.
- `registry mirror` copies the catalogue and its signature byte for byte,
  after the packages and pictures are in place. The sequence check is the
  catalogue check's own. A file is kept only after its SHA-256 matches.
- A repeating option keeps every value, in the order given. An option that
  does not repeat, and every flag, is refused the second time.
- `interface-words --json` prints one JSON array of the texts the modules
  export for the interface catalogue. The array is sorted and holds each
  text once. Any other arguments exit 2.
- `mutator diff` prints the same bytes for the same inputs. The header is
  not sorted with the change lines. Change lines are sorted by their bytes,
  including the tab and the `by` suffix. Values are the text the file or the
  formatter wrote, with a byte below 0x20 and 0x7F written `\xHH`.
- A unit name in the report is the FBI's file name, `units/<NAME>.fbi`.
  That name matches `UnitName` for all 278 FBIs on 3.1c.
- `by` follows the section the journal names, not every write of the same
  key in the file. A GUI gadget is folded from `[GADGETn][COMMON] name`.
- `mutator diff --versus` exits 0 only when no difference remains and every
  pattern matched a difference. The comparison is the text before a tab.

## Adding a command

1. Write its `run` function in a `.cpp` file under `tools/oa-tool`, and
   declare that function in `command.hpp` with the others from the same file.
2. Add its row to the table in `commands.cpp`: the name, the usage arguments,
   a one-line summary, a paragraph of help, how many arguments it takes and
   the function. A group leaves the function null and names its subcommands.
   A command whose options make the argument count vary sets the upper bound
   to `any_count` and checks the arguments itself with `parse_arguments`.
   Declare a repeating option with `repeats`; the parser already keeps every
   value.
3. Add the `.cpp` file to `oa-tool-commands` in `tools/oa-tool/CMakeLists.txt`.
4. Cover the command from `tools/oa-tool/tests/oa_tool_test.cpp`, or from
   the command's own test when it builds packages. The tests run commands
   in-process through `run_tool`, with streams of their own. `pack` and
   `check` are covered by `pack_test.cpp` and `check_test.cpp`.

## Tests

`tools-oa-tool` (`oa-tool-test`) builds an archive in memory, runs every
archive command, checks help and the usage failures, checks `parse_arguments`,
and dispatches a group through `run_tool`'s table overload.

`tools-oa-tool-pack` (`oa-tool-pack-test`) packs a made-up mod and the pseudo
language pack, checks the order and the methods, packs again after the
files' times change, and checks the refusals. Its argument is the pseudo
pack's folder.

`tools-oa-tool-pack-map` (`oa-tool-pack-map-test`) packs two maps into a
`.oamap`, checks the index, the preview and the refusals, and leaves
`pack-map-source` and `pack-map-game` in its working folder. It also checks
that `numplayers` sets `players`, that a missing or malformed `numplayers`
falls back to the start positions, that a folder's `name`, `description`
and `players` replace the computed values, and that an unknown stem is
refused. Two packs of one folder have the same bytes.

`tools-oa-tool-check` (`oa-tool-check-test`) checks a made-up mod package,
its refusals, and a language manifest read on its own. Its argument is the
pseudo pack's `language.yaml`.

`tools-oa-tool-mutator` (`oa-tool-mutator-test`) packs a made-up mutator,
checks the entry order, the methods and the JSON facts, and checks the
refusals from `pack` and from `check`. `mutator pack` and `mutator check`
are the same commands limited to a mutator.

`tools-oa-tool-brain` (`oa-tool-brain-test`) packs a made-up brain, checks
the entry order, the methods and the JSON facts, and checks the refusals
from `pack` and from `check`. `brain pack` is the same command limited to a
brain.

`tools-oa-tool-keys` (`oa-tool-keys-test`) packs a made-up key layout, checks
the entry order, the methods and the JSON facts, and checks the refusals
from `pack` and from `check`. `keys pack` is the same command limited to a
key layout. It also draws the keyboard picture and shows the bindings.

`tools-oa-tool-brain-net` (`oa-tool-brain-net-test`) writes its own ONNX
bytes and calibration lines. It converts a dense model and checks the
weights against a hand-quantised copy, converts a grid and a Gather plus
Concat, checks that int16 is closer than int8, checks that two runs write
the same bytes, and checks the refusals: an unsupported operator, external
data, opset 10, a file cut at every length, a varint of 11 bytes, a message
17 deep, a missing feature, an empty calibration file, a difference past
`--max-difference`, an existing file without `--force`, and an unknown option.

`tools-oa-tool-mutator-import` (`oa-tool-mutator-import-test`) imports
made-up `.tam` files and a zip into mutator folders, packs one and checks
it, replaces a folder with `--force`, and checks the usage errors. One
refused mutator leaves the others written. A companion that is a link is
not read, on a system that can make a link.

`tools-oa-tool-mutator-diff` (`oa-tool-mutator-diff-test`) runs `mutator diff`
against a stand-in game and made-up package folders. It checks the report,
a second run with the same bytes, a relative path after changing directory,
a dump that does not change the report, two ways of dropping a unit, a
masked file and a build menu, a map profile, an allow file and the usage
errors.

`tools-oa-tool-mutator-diff-data` runs the same program with `--data` against
the installed game. A doubling loadout's change lines are worker-time fields,
and the same loadout written as rules matches it.

`tools-oa-tool-check-map` (`oa-tool-check-map-test`) checks a made-up map
pack against a stand-in game and a made-up mod. `tools-oa-tool-check-map-data`
runs the same program with `--data` against the installed game.

`tools-oa-tool-catalogue` (`oa-tool-catalogue-test`) makes one publisher
key with the real parameters, then signs and checks a catalogue with that
key, with `--registry` and `--key` and with a descriptor. A changed
catalogue, another key, another registry and a signature that names
another key are refused. A short passphrase, an existing key file and a
catalogue the check would refuse fail before the key is opened.

`tools-oa-tool-engine-feed` (`oa-tool-engine-feed-test`) makes one publisher
key, then writes a feed from stand-in package files, signs it and checks it
with that key. A check with no `--key` is refused. `check --url` reads the
feed and its signature from the test HTTP fixture on 127.0.0.1, and the
fixture's log is two GETs with no query. A package whose name carries
another version is refused.

`tools-oa-tool-registry` (`oa-tool-registry-test`) makes one publisher key,
then an empty registry, and mirrors registries served by the test HTTP
fixture on 127.0.0.1. It covers a resumed download, a file that fails its
SHA-256 check, a signature and a sequence the catalogue check refuses, and a
tokens registry whose key requests carry reason `mirror`.

`tools-oa-tool-interface-words` runs the built `oa-tool interface-words
--json` and fails when a lookup wrapper in the sources is handed a text
that array does not hold. The comparison reads
`tools/oa-tool/tests/interface_words_scan.py`.
Each module that looks a text up exports it in its `interface_words` list;
the command only joins those lists.

`tools-oa-tool-interface-catalogue` reads `languages/interface.tdf` into
the interface catalogue and checks that German, French, Spanish and
Italian each cover that same list, that a German lookup returns German,
and that placeholders, numbers and product names are kept.

## Limitations

`brain net` uses the C library's `tanh` and `exp`. Those can differ in the last bit between machines, and that can move a scale only when a magnitude sits exactly on a power of two. The same model and calibration file give the same bytes on one machine. At most 100,000 calibration samples are read.

The archive commands take no options. `pack` takes `--out`, `--force` and,
for a map pack, `--game-dir`. `check` takes `--game-dir`, `--mod`,
`--accept-unimplemented-hacks` and `--json`. A map pack needs `--game-dir`.
Each `--mod` is one target after the base game, and exit 1 is reserved for
a map that does not fit `ta-3.1c`. `catalogue keygen` does not replace a
key file. On macOS and Linux that file's mode is 0600. The signature
`catalogue sign` writes is the one line a catalogue check reads. A
passphrase comes from the terminal, with echo off, or from
`--passphrase-file`. One key is stored in one file, and this command does
not rotate it. `registry init` makes a direct registry and signs its empty
catalogue for 30 days. `registry mirror` copies the origin's files and does
not sign anything, and it posts no install result. A mirror folder is a copy
of the registry. It is served as a direct registry: the files and signatures
stand on their own, and the origin's key API stays the origin's policy. The
macOS package does not ship `oa-tool`. `interface-words` takes only
`--json`. `mutator diff` reads package folders by path. An installed id is
refused. It does not pack a `.oamut`, and it does not check a package's
integrity. It does not run the hack check. A comparison folds removals and
where a build menu or a GUI gadget names a unit, and it does not fold
anything else. A file that parses on one side only is reported as
`file replaced` with the two hashes.
