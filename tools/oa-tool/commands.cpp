// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "command.hpp"

#include <exception>
#include <ostream>
#include <string>

namespace oa::tool {
namespace {

const Command catalogue_commands[] = {
    {
        "keygen",
        "--id ID --out FILE [--passphrase-file FILE]",
        "Make a sealed publisher key.",
        "Makes a new Ed25519 key, seals its seed with a passphrase and writes the key file. "
        "--id is the key id a catalogue signature names. The file is created only when that "
        "name is new, and on macOS and Linux its mode is 0600. The passphrase is read from "
        "the terminal with echo off, twice, and has to be at least 12 bytes. --passphrase-file "
        "reads the file's first line instead, and does not ask twice. Prints the key id, the "
        "public key as ed25519: and its base64, and the key's fingerprint.",
        0,
        any_count,
        run_catalogue_keygen,
        {},
    },
    {
        "public",
        "KEY_FILE",
        "Print a sealed key's public half.",
        "Reads KEY_FILE and prints the key id, the public key and its fingerprint. The "
        "passphrase is not used, and the seed is not opened.",
        1,
        1,
        run_catalogue_public,
        {},
    },
    {
        "sign",
        "--key KEY_FILE CATALOGUE [--out SIG] [--passphrase-file FILE]",
        "Sign a catalogue with a sealed key.",
        "Reads CATALOGUE, opens KEY_FILE with the passphrase and writes a detached signature. "
        "Without --out the signature is CATALOGUE with .sig added to the name. The signature "
        "is one line: ed25519, the key's id and the signature of the catalogue's exact bytes. "
        "A catalogue this build would refuse is refused before the key is opened. The written "
        "pair is checked, with the catalogue's own registry and this key, before the command "
        "reports success. Prints the signature file and the key id.",
        0,
        any_count,
        run_catalogue_sign,
        {},
    },
    {
        "verify",
        "CATALOGUE [--sig SIG] (--descriptor FILE | --registry ID --key ed25519:<base64> [--key "
        "...])",
        "Check a catalogue's signature.",
        "Checks CATALOGUE against its signature. Without --sig the signature is CATALOGUE with "
        ".sig added to the name. Pass --descriptor FILE to use that registry's id and keys, or "
        "--registry ID and at least one --key. Each --key is ed25519: and the key in base64, "
        "and is read as the key the signature names; the first one is the key that is trusted. "
        "Prints the signing key and the verdict, and exits 0 when the catalogue can be used. "
        "A refused catalogue prints the verdict and exits 1.",
        0,
        any_count,
        run_catalogue_verify,
        {},
    },
};

const Command engine_feed_commands[] = {
    {
        "write",
        "--version V [--release N] --date YYYY-MM-DD --from V --page URL --mirror URL "
        "[--mirror URL]... [--notes FILE] [--rollout N] [--online-min V] [--channel stable] "
        "[--previous FEED] --out FEED PACKAGE...",
        "Write one release of the signed engine feed.",
        "Reads each package file, hashes it and writes a feed of format 1. A package is named "
        "open-annihilation, an optional v, the version, and then the package id as a zip, a "
        "Windows setup or the macOS package. The version in each name must be --version. "
        "--release defaults to that version's release number. --notes is a JSON object of "
        "language to lines. --previous adds the older releases of an earlier feed, five in "
        "all. The written file is read back before the command reports success. Prints the "
        "feed's path and the release number.",
        0,
        any_count,
        run_engine_feed_write,
        {},
    },
    {
        "sign",
        "--key KEY_FILE FEED [--out SIG] [--passphrase-file FILE]",
        "Sign an engine feed with a sealed key.",
        "Reads FEED, opens KEY_FILE with the passphrase and writes a detached signature. "
        "Without --out the signature is FEED with .sig added to the name. A feed this build "
        "would refuse is refused before the key is opened. The written pair is checked, with "
        "the feed's own channel and this key, before the command reports success. Prints the "
        "signature file and the key id.",
        0,
        any_count,
        run_engine_feed_sign,
        {},
    },
    {
        "check",
        "(FEED [--sig SIG] | --url URL) [--channel stable] [--key ID=ed25519:BASE64]...",
        "Check an engine feed's signature.",
        "Checks FEED against its signature, or fetches --url and that address with .sig added. "
        "Without --sig the signature is FEED with .sig added to the name. Without --key the "
        "keys are this build's pinned keys. Each --key is an id, =, ed25519: and the key in "
        "base64. Prints the signing key and one line a release, and exits 0 when the feed can "
        "be used. A refused feed prints the verdict and exits 1.",
        0,
        any_count,
        run_engine_feed_check,
        {},
    },
};

const Command registry_commands[] = {
    {
        "init",
        "FOLDER --id ID --name NAME --base-url URL --key KEY_FILE [--passphrase-file FILE] "
        "[--homepage URL]",
        "Make an empty signed registry.",
        "Makes a direct registry in FOLDER: registry.yaml, an empty catalogue signed for 30 days, "
        "its signature, and the v1/p and v1/i folders. FOLDER must be missing or empty. "
        "--base-url is the http address the registry is served at, with no trailing slash. --id "
        "and --name are the registry's. --key is a sealed publisher key; --passphrase-file reads "
        "the passphrase from a file, and otherwise the terminal asks. --homepage is an optional "
        "page. The descriptor and the signed catalogue are read back before the command reports "
        "success. Prints the descriptor's path and the key's fingerprint.",
        0,
        any_count,
        run_registry_init,
        {},
    },
    {
        "mirror",
        "URL FOLDER [--key ed25519:<base64>]",
        "Copy a registry into a folder.",
        "Copies a registry into FOLDER, laid out as on the host that served it. URL is the "
        "registry descriptor. The catalogue's signature is checked, and a sequence lower than "
        "the one FOLDER already holds stops the copy. Each package and picture is checked by "
        "SHA-256. A file already there with the right SHA-256 is skipped, and a .part is resumed "
        "with a Range request. A file that fails its check is deleted. The catalogue and its "
        "signature are written last, byte for byte. A tokens registry is copied through its "
        "download API: the tool asks for a key, prints a check's code and address, and carries "
        "on once the check is passed. A direct registry is fetched from the catalogue's own "
        "addresses. The folder is served as a direct registry.",
        0,
        any_count,
        run_registry_mirror,
        {},
    },
};

const Command mutator_commands[] = {
    {
        "pack",
        "FOLDER [--out FILE] [--force]",
        "Pack a mutator folder as a .oamut.",
        "Reads FOLDER, whose top holds oamut.yaml, and writes a .oamut. The manifest is written "
        "first, then every other file in byte order of its path, with fixed times. A .png is "
        "stored and a .lua file is deflated, by the same rule as pack. Without --out the name "
        "is <id>-<version>.oamut in the current folder. --force replaces an existing file. "
        "--game-dir is a usage error. A manifest problem, a Lua source that does not compile "
        "and a rule file that does not parse are listed, each with its file and line, and "
        "nothing is written. A folder the installer would refuse is not packed. A folder "
        "without oamut.yaml is a usage error.",
        1,
        any_count,
        run_mutator_pack,
        {},
    },
    {
        "check",
        "FILE [--json]",
        "Check a .oamut and report the facts a catalogue lists.",
        "Opens FILE through the same checks as check, and only when it is a .oamut. The report's "
        "keys, in order, are check, ok, problems, warnings, file, kind, id, name, version, "
        "revision, size, sha256, unpacked_size, files, then summary, description, homepage, "
        "tags, author, requires, works_with, conflicts_with, layers, clashes, options, icon, "
        "entry, scripts, rules and legacy. revision is null. clashes is add or replace when "
        "the manifest names it, and is left out when it does not. requires.mutators and "
        "conflicts_with are objects with id and, when a range was written, version. works_with "
        "and layers are the "
        "manifest's strings. options lists id, label, type and default, and a choice's values "
        "and labels or a number's min, max and step. icon gives the icon's path, sha256, size, "
        "width and height. scripts is how many Lua files compiled. rules lists the .sm paths. "
        "legacy lists each original's name, prefix, compatibility and files. A key the mutator "
        "does not have is left out. Lua is compiled and rule files are parsed, and neither is "
        "run. --json prints one JSON object; otherwise each fact is a key: value line and the "
        "last line is result: ok or result: refused. A file that is not a .oamut is a usage "
        "error. Exit 0 when the mutator would be installed, and 1 when it would be refused.",
        1,
        any_count,
        run_mutator_check,
        {},
    },
    {
        "import",
        "INPUT... --out DIR [--id ID] [--version VERSION] [--tag TAG]... "
        "[--works-with CODE=ENTRY]... [--pack] [--force]",
        "Import TA: Mutation mutators into mutator folders.",
        "Reads each INPUT, a .tam or a zip, and writes one mutator folder under DIR. A .tam's "
        "companions are read from its own folder, and a zip's names are the names the archive "
        "recorded. --id and --version set that one mutator's id and version, and are a usage "
        "error when more than one mutator is named. --tag is repeated and replaces the gameplay "
        "tag. --works-with CODE=ENTRY maps a compatibility code to a works-with entry; OTA and "
        "TACC are already mapped. --pack also writes DIR/<id>-<version>.oamut. --force replaces "
        "an existing folder and, with --pack, an existing package. One line per mutator names "
        "the id, the input, the rule files and the archives, and its warnings follow indented. "
        "A refused mutator prints its problem and the others are still written. Exit 0 when "
        "every mutator was imported, 1 when one was refused or could not be written, and 2 when "
        "the arguments are not the command's. A link is not followed. DIR is not the folder "
        "that holds a .tam, whose companions are read from that folder. A zip has no companions "
        "beside it, so DIR may be the folder that holds the zip.",
        1,
        any_count,
        run_mutator_import,
        {},
    },
    {
        "diff",
        "--game-dir DIR [--mod-dir DIR] [--map NAME] [--seed N] [--mutator SPEC]... "
        "[--versus SPEC]... [--allow FILE] [--dump FILE]",
        "Report what a loadout changes, or compare two loadouts.",
        "Mounts DIR the way the game does, and --mod-dir as a mod folder over it. Each "
        "--mutator is a package folder, with options after the last colon, in the same text "
        "as the game's --mutator. An id with no path is refused: this command reads package "
        "folders. The mutators run in the order given, through the game's mutation pipeline, "
        "and --map also runs each mutator's maps hook for that map. A map the game does not "
        "have is a usage error. --seed is the match seed, "
        "and it is 0 when left out. The report lists units dropped or added, files added, "
        "hidden or replaced, every changed field, and build-menu entries added or removed. "
        "--versus builds a second loadout the same way and compares the two reports. Lines "
        "that name the same removal or the same menu in two ways are folded together. --allow "
        "is a file of differences that may remain; each pattern needs a reason above it, and "
        "a pattern that matches nothing fails the comparison. --allow without --versus is a "
        "usage error. --dump writes the mutated file list and its fields, and does not change "
        "the report. Exit 0 when the report was written, and when a comparison has no "
        "remaining difference and no unused allowance. Exit 1 when a mutator fails, the "
        "folder is not a usable game, or a comparison does not match. Exit 2 when the "
        "arguments are not the command's.",
        0,
        any_count,
        run_mutator_diff,
        {},
    },
};

const Command brain_commands[] = {
    {
        "pack",
        "FOLDER [--out FILE] [--force]",
        "Pack a brain folder as a .oabrain.",
        "Reads FOLDER, whose top holds oabrain.yaml, and writes a .oabrain. The manifest is "
        "written "
        "first, then every other file in byte order of its path, with fixed times. A .png is "
        "stored and a .lua file is deflated, by the same rule as pack. Without --out the name "
        "is <id>-<version>.oabrain in the current folder. --force replaces an existing file. "
        "--game-dir is a usage error. A manifest problem, a file outside the layout, a Lua source "
        "that does not compile, a network whose SHA-256 is not the one oabrain.yaml gives and an "
        "icon that is not a square PNG within the limits are listed, each with its file, and "
        "nothing is written. Lua is compiled and not run. A .oann file is hashed and not "
        "interpreted. A folder the installer would refuse is not packed. A folder without "
        "oabrain.yaml is a usage error. oa-tool check reports the brain's facts.",
        1,
        any_count,
        run_brain_pack,
        {},
    },
    {
        "check",
        "FILE --game-dir DIR [--mod FOLDER] [--level LEVEL] [--side N] [--json]",
        "Load a brain against a game and report how it started.",
        "FILE is a .oabrain or a folder whose top holds oabrain.yaml. The folder's name is the "
        "brain's id. --game-dir is the game, mounted the way the game mounts it, and is required. "
        "--mod is a mod folder layered over that game. --level selects a level; the first level "
        "is used when it is omitted. --side is the side whose commander the brain's seat holds, "
        "from 0 to 255, and it is 0 when omitted. The check builds a match from the game's own "
        "unit, weapon and feature definitions and its computer-player profile, on a flat "
        "stand-in map, seats the brain, and never ticks the match. It reports whether the brain "
        "loaded, whether it fits, whether it started, the instructions each phase used, and the "
        "memory its Lua and its networks hold. A unit file that does not load is a warning. "
        "--json prints one object. Exit 0 when the brain was seated, 1 when it was not, and 2 "
        "when the arguments are not the command's. A file of another kind, or a folder without "
        "oabrain.yaml, is a usage error.",
        1,
        any_count,
        run_brain_check,
        {},
    },
    {
        "net",
        "MODEL.onnx --calibrate DATA [--out FILE] [--weights int8|int16] "
        "[--max-difference X] [--force] [--json]",
        "Convert an ONNX model to a .oann.",
        "Reads MODEL.onnx, a model of the default domain at opset 11 to 21, and writes a .oann. "
        "The layers are Gemm and MatMul (a following Add of a constant is the bias), Conv, "
        "MaxPool, Relu, LeakyRelu, Tanh, Sigmoid, Softmax, Add, Concat, Flatten, Reshape, ArgMax "
        "and Gather. Identity and Dropout are skipped. The first dimension of every input is the "
        "batch and is removed. --calibrate is a file of JSON lines; a line whose kind is sample "
        "holds features as whole numbers in 65536ths. Weights are int8 unless --weights int16 is "
        "set. The integer network is run on the same samples, and the report gives each output's "
        "largest difference, or the share of samples an argmax agrees on. --max-difference "
        "refuses a larger difference and writes nothing. Without --out the file is the model's "
        "path with .oann. An existing file needs --force. --json prints one object. Exit 0 when "
        "the file was written and every difference is within the limit. Exit 1 when the model "
        "cannot be converted or a difference passes the limit. Exit 2 when the arguments are not "
        "the command's.",
        1,
        any_count,
        run_brain_net,
        {},
    },
};

const Command keys_commands[] = {
    {
        "pack",
        "FOLDER [--out FILE] [--force]",
        "Pack a key-layout folder as a .oakeys.",
        "Reads FOLDER, whose top holds oakeys.yaml, and writes a .oakeys. The package is the one "
        "the key-layout packer writes: oakeys.yaml first, then every other file in byte order of "
        "its name. A .png is stored and every other file is deflated, and the archive records no "
        "clock time. Without --out the name is <id>-<version>.oakeys in the current folder. "
        "--force replaces an existing file. --game-dir is a usage error. A manifest problem, a "
        "file outside the layout, an icon that is not a square PNG within the limits and a chord "
        "that does not parse are listed, each with its file, and nothing is written. A warning "
        "does not refuse the package. A folder the installer would refuse is not packed. A folder "
        "without oakeys.yaml is a usage error. oa-tool check reports the layout's facts.",
        1,
        any_count,
        run_keys_pack,
        {},
    },
    {
        "check",
        "FILE|FOLDER [--json] [--strict]",
        "Check a key layout and report the facts a catalogue lists.",
        "FILE is a .oakeys or a folder whose top holds oakeys.yaml. The report is the one check "
        "writes for a key layout: summary, description, homepage, tags, author, requires, "
        "works_with, base, keyboard, pointer, helpers, changes, triggers, unknown_actions, "
        "clashes and icon. revision is null. keyboard, base, changes, triggers, unknown_actions "
        "and clashes are always written. --json prints one object. --strict refuses a layout "
        "that has a warning and keeps those warnings in the report. --game-dir is a usage error. "
        "Exit 0 when the layout would be installed, 1 when it would be refused or --strict meets "
        "a warning, and 2 when the arguments are not the command's. A file of another kind, or a "
        "folder without oakeys.yaml, is a usage error.",
        1,
        any_count,
        run_keys_check,
        {},
    },
    {
        "picture",
        "FILE|FOLDER --out PNG [--width PIXELS] [--fonts DIR]",
        "Draw the keyboard picture of what a layout changes.",
        "FILE is a .oakeys or a folder whose top holds oakeys.yaml. Draws the keyboard, marking "
        "each key the layout's own triggers change, and writes a PNG. The labels are the US "
        "keyboard's. any draws an ANSI keyboard. --out names the PNG and is required. --width is "
        "the picture's width in pixels and is 1200 when omitted. --fonts is the folder of text "
        "fonts; the fonts beside the program are used when it is omitted. A font that cannot be "
        "opened writes nothing. A layout whose chords do not parse writes nothing. Exit 0 when "
        "the PNG was written, 1 when the layout or the fonts cannot be read, and 2 when the "
        "arguments are not the command's.",
        1,
        any_count,
        run_keys_picture,
        {},
    },
    {
        "show",
        "FILE|FOLDER|oa-default [--json]",
        "Print the bindings after the base and one layout.",
        "FILE is a .oakeys, a folder whose top holds oakeys.yaml, or the literal oa-default. "
        "Prints each action with its context and its chords, in the registry's order, then each "
        "action this build does not know. oa-default is the base with no layout. No mod and no "
        "own changes are applied. --json prints one array of action, context and chords. A layout "
        "whose chords do not parse is refused. A warning is still shown. Exit 0 when the bindings "
        "were printed, 1 when the layout cannot be read, and 2 when the arguments are not the "
        "command's.",
        1,
        any_count,
        run_keys_show,
        {},
    },
};

const Command command_table[] = {
    {
        "list",
        "ARCHIVE",
        "List the files in an archive.",
        "Reads an HPI archive and writes nothing. Prints one line per file, in the archive's "
        "order: the path inside the archive, a tab and the file's size in bytes.",
        1,
        1,
        run_list,
        {},
    },
    {
        "extract",
        "ARCHIVE ENTRY OUTPUT",
        "Copy one file out of an archive.",
        "Reads one entry from an HPI archive and writes its bytes to OUTPUT. Prints nothing.",
        3,
        3,
        run_extract,
        {},
    },
    {
        "asset-extract",
        "ROOT ENTRY OUTPUT [ARCHIVE...]",
        "Copy one file from a game directory.",
        "Reads ENTRY from the loose files under ROOT, and from the archives named after OUTPUT "
        "when no loose file has that path. A loose file wins over an archive. Writes the bytes "
        "to OUTPUT. Prints whether the file was loose or in an archive, the path it was read "
        "from, and its size in bytes.",
        3,
        any_count,
        run_asset_extract,
        {},
    },
    {
        "preview",
        "ARCHIVE PCX_ENTRY OUTPUT.ppm|OUTPUT.png",
        "Decode one image from an archive.",
        "Reads a PCX image from an HPI archive and writes it as a PPM file, or as a PNG file when "
        "the output path ends in .png. Prints the image's width and height and the output path.",
        3,
        3,
        run_preview,
        {},
    },
    {
        "decode-pcx",
        "INPUT.pcx OUTPUT.ppm|OUTPUT.png",
        "Decode a PCX image file.",
        "Reads a PCX file and writes it as a PPM file, or as a PNG file when the output path ends "
        "in .png. Prints the image's width and height and the output path.",
        2,
        2,
        run_decode_pcx,
        {},
    },
    {
        "pack",
        "FOLDER [--out FILE] [--force] [--game-dir DIR]",
        "Pack a folder as a mod, a language package, a map pack, an expansion, a mutator, a "
        "brain or a key layout.",
        "Reads FOLDER and writes a .oamod, a .oalang, a .oamap, a .oaexp, a .oamut, a .oabrain or "
        "a .oakeys. The folder's top holds one manifest, oamod.yaml, language.yaml, oamap.yaml, "
        "oaexp.yaml, oamut.yaml, oabrain.yaml or oakeys.yaml. The manifest is written first, then "
        "every other file in byte "
        "order of its path, with fixed times. A file whose extension, without case, is png, jpg, "
        "jpeg, gif, ogg, mp3, zip, gz, bz2, xz, 7z, oamod, oalang or oamap, and an empty file, is "
        "stored; every other file is deflated. An expansion's files are all stored. The same files "
        "give the same bytes on any machine with the same zlib. --out names the package file; "
        "without it the name comes from the manifest and is written in the current folder. A "
        "mutator's default name is <id>-<version>.oamut. A brain's default name is "
        "<id>-<version>.oabrain. A key layout's default name is <id>-<version>.oakeys. A key "
        "layout stores a .png and deflates every other file, and the archive records no clock "
        "time. --force replaces an existing file. A folder the installer would "
        "refuse is not packed. A mutator whose manifest, Lua source or rule file is broken is not "
        "packed, and every problem is listed. A brain whose manifest, layout, Lua source, network "
        "SHA-256 or icon is broken is not packed, and every problem is listed. A key layout whose "
        "manifest, icon or chords are broken is not packed, and every problem is listed. Lua is "
        "compiled and not run. check reports a brain's summary, description, homepage, tags, "
        "author, requires, works_with, levels, full_awareness, networks, network_bytes, options, "
        "icon, entry, scripts and data. check reports a key layout's summary, description, "
        "homepage, tags, author, requires, works_with, base, keyboard, pointer, helpers, changes, "
        "triggers, unknown_actions, clashes and icon. A map pack reads the game's palette from "
        "--game-dir: map packs need the game's palette for previews. --game-dir on a mod, a "
        "language pack, an expansion, a mutator, a brain or a key layout is a usage error.",
        1,
        any_count,
        run_pack,
        {},
    },
    {
        "check",
        "FILE|FOLDER [--game-dir DIR] [--mod FOLDER[=KEY]]... [--accept-unimplemented-hacks] "
        "[--json] [--strict]",
        "Check a package and report the facts a catalogue lists.",
        "Opens FILE through the same checks an install runs before it writes, and reports "
        "whether that install would accept it. The report's keys, in order, are check, ok, "
        "problems, warnings, file, kind, id, name, version, revision, size, sha256, "
        "unpacked_size, files, and then the kind's own facts: for a mod, summary, homepage, "
        "tags, supportedGames, requiredExpansions, author, requires, sim_hash, full_hash, hacks "
        "and packaging; for a language pack, supportedGames, requiredExpansions and language in "
        "place of those; for a map pack, summary, author, homepage, tags, supportedGames, "
        "requiredExpansions, requires and packaging, then maps; for an expansion, extends, needs "
        "and author; for a mutator, summary, description, homepage, tags, author, requires, "
        "works_with, conflicts_with, layers, options, icon, entry, scripts, rules and legacy; "
        "for a brain, summary, description, homepage, tags, author, requires, works_with, levels, "
        "full_awareness, networks, network_bytes, options, icon, entry, scripts and data; for a "
        "key layout, summary, description, homepage, tags, author, requires, works_with, base, "
        "keyboard, pointer, helpers, changes, triggers, unknown_actions, clashes and icon. "
        "A mutator's Lua is compiled and its rule files are parsed, and neither is run. A brain's "
        "Lua is compiled and not run, and a .oann file is hashed and not interpreted. "
        "full_awareness is a boolean and network_bytes is a whole number, and both are always "
        "written for a brain. levels lists id, name and difficulty. networks lists id, file, "
        "sha256 and size. requires.expansions is the expansion ids. data lists the table paths in "
        "byte order. Warnings may suggest writing "
        "supportedGames instead of requires.base, or requiredExpansions instead of an expansion "
        "archive named in layout.installation-archives. A key the package does not have is left "
        "out, and revision is null when a language pack has no packaging revision, for every "
        "mutator, for every brain and for every key layout. keyboard, base, changes, triggers, "
        "unknown_actions and clashes are always written for a key layout. A key layout may also "
        "be a folder whose top holds oakeys.yaml; the report then names that folder, and the "
        "size and hash are the package the folder would pack to. --strict refuses a key layout "
        "that has a warning and keeps those warnings in the report. It changes no other kind. "
        "An expansion does not take "
        "--game-dir. A brain does not take --game-dir. A key layout does not take --game-dir. "
        "A map pack needs --game-dir, "
        "the game folder. The check builds each target the way the game would. Each --mod "
        "FOLDER[=KEY] is one more target, after the base game ta-3.1c. The key is the text "
        "after the last =, or the folder's catalogue origin as id@release, or the profile's id "
        "when that folder has no release. maps lists each map. compatible holds ta-3.1c and "
        "then each mod, and failures holds the targets that do not fit. --json prints one JSON "
        "object; otherwise each fact is a key: value line, then one line per map and target, "
        "and the last line is result: ok or result: refused. --accept-unimplemented-hacks "
        "accepts a hack this build does not carry out yet. Exit 0 when the package would be "
        "installed, 1 when it would be refused or a map does not fit ta-3.1c. A mod that does "
        "not fit leaves that exit code unchanged.",
        1,
        any_count,
        run_check,
        {},
    },
    {
        "mutator",
        "COMMAND",
        "Pack and check mutators, and import TA: Mutation mutators.",
        "Packs a mutator folder into a .oamut, checks a .oamut before anyone installs it, and "
        "imports TA: Mutation mutators. mutator pack packs a folder whose top holds oamut.yaml, "
        "and mutator check reads a .oamut. A folder without oamut.yaml, or a file of another "
        "kind, is a usage error. Those two subcommands otherwise run the same work as pack and "
        "check. mutator import reads .tam files or a zip and writes one mutator folder per "
        "mutator. mutator diff mounts a game, runs package folders through the mutation "
        "pipeline and reports what changed.",
        0,
        0,
        nullptr,
        mutator_commands,
    },
    {
        "brain",
        "COMMAND",
        "Pack brains, check one against a game, and convert a network.",
        "Packs a brain folder into a .oabrain, checks a brain against a game, and converts an "
        "ONNX model into a .oann. brain pack reads a folder whose top holds oabrain.yaml. "
        "oa-tool check reports a brain's facts: summary, description, homepage, tags, author, "
        "requires, works_with, levels, full_awareness, networks, network_bytes, options, icon, "
        "entry, scripts and data. A folder without oabrain.yaml is a usage error. brain check "
        "loads a .oabrain, or a folder whose top holds oabrain.yaml, against a game. It seats "
        "the brain in a match of the game's definitions on a flat stand-in map and never ticks "
        "that match. It reports whether the brain loaded, fitted and started. brain net reads "
        "an ONNX model of opset 11 to 21, quantises it with a calibration file and reports how "
        "far the integer network is from the floating-point one.",
        0,
        0,
        nullptr,
        brain_commands,
    },
    {
        "keys",
        "COMMAND",
        "Pack, check and draw key layouts.",
        "Packs a key-layout folder into a .oakeys, checks a .oakeys or a folder whose top holds "
        "oakeys.yaml, draws the keyboard picture of what the layout changes, and prints the "
        "bindings after the base and the layout. keys pack reads a folder whose top holds "
        "oakeys.yaml. oa-tool check reports a key layout's facts: summary, description, homepage, "
        "tags, author, requires, works_with, base, keyboard, pointer, helpers, changes, triggers, "
        "unknown_actions, clashes and icon. A folder without oakeys.yaml, or a file of another "
        "kind, is a usage error. keys picture writes a PNG. keys show prints the resolved "
        "bindings, and oa-default is the base with no layout.",
        0,
        0,
        nullptr,
        keys_commands,
    },
    {
        "catalogue",
        "COMMAND",
        "Make a publisher key and sign or check a catalogue.",
        "Makes a sealed publisher key, prints its public half, and signs or checks a catalogue. "
        "The key's seed is encrypted with a passphrase. A signature is the one line a catalogue "
        "check reads, and the check is the one the game uses.",
        0,
        0,
        nullptr,
        catalogue_commands,
    },
    {
        "engine-feed",
        "COMMAND",
        "Write, sign or check the signed engine feed.",
        "Writes a feed of the releases one channel offers, signs it with a sealed key, and "
        "checks a feed against that signature. The check is the one the game uses. A signature "
        "is the one line a catalogue check reads.",
        0,
        0,
        nullptr,
        engine_feed_commands,
    },
    {
        "registry",
        "COMMAND",
        "Make an empty signed registry or copy one into a folder.",
        "Makes a direct registry, with an empty catalogue signed for 30 days, or copies a "
        "registry's descriptor, catalogue, signature, pictures and packages into a folder. A "
        "copied folder is a registry of its own. It is served as a direct registry: the files "
        "and signatures stand on their own, and the origin's download API stays the origin's "
        "policy.",
        0,
        0,
        nullptr,
        registry_commands,
    },
    {
        "interface-words",
        "--json",
        "List the English texts the interface catalogue is given.",
        "Prints a JSON array of every English text this build passes to the interface "
        "catalogue, sorted, with each text once. The texts are the lists the modules that "
        "look them up export. A translator compares the array with a language pack.",
        1,
        1,
        run_interface_words,
        {},
    },
};

/// Returns the command named `name`, or null when the table has none.
const Command* find_command(std::span<const Command> table, std::string_view name) {
    for (const Command& command : table)
        if (command.name == name)
            return &command;
    return nullptr;
}

/// Joins a command path and the next word with a space.
std::string join_path(std::string_view path, std::string_view name) {
    if (path.empty())
        return std::string(name);
    std::string joined;
    joined.reserve(path.size() + 1 + name.size());
    joined.append(path);
    joined.push_back(' ');
    joined.append(name);
    return joined;
}

/// Prints one command's usage line and its one-line summary.
void print_entry(std::ostream& out, std::string_view path, const Command& command) {
    out << "oa-tool " << path;
    if (!command.arguments.empty())
        out << ' ' << command.arguments;
    out << '\n';
    if (!command.summary.empty())
        out << "    " << command.summary << '\n';
}

/// Prints a table, and each group's subcommands under it.
void print_list(std::ostream& out, std::span<const Command> table, std::string_view prefix) {
    for (const Command& command : table) {
        const std::string path = join_path(prefix, command.name);
        print_entry(out, path, command);
        for (const Command& sub : command.subcommands)
            print_entry(out, join_path(path, sub.name), sub);
    }
}

/// Prints one command's usage line, its paragraph and, for a group, its subcommands.
void print_command_help(std::ostream& out, std::string_view path, const Command& command) {
    out << "oa-tool " << path;
    if (!command.arguments.empty())
        out << ' ' << command.arguments;
    out << "\n\n" << command.help << '\n';
    if (!command.subcommands.empty()) {
        out << '\n';
        for (const Command& sub : command.subcommands)
            print_entry(out, join_path(path, sub.name), sub);
    }
}

/// Prints a usage failure and the help command that explains it.
void report_usage(Output& output, std::string_view path, std::string_view problem) {
    output.err << "oa-tool " << path << ": " << problem << '\n';
    output.err << "run 'oa-tool help " << path << "'\n";
}

/// Describes an argument count outside the command's least and most.
std::string count_problem(const Command& command, std::size_t count) {
    std::string problem = "expected ";
    if (command.least == command.most) {
        problem += std::to_string(command.least);
        problem += command.least == 1 ? " argument" : " arguments";
    } else if (command.most == any_count) {
        problem += "at least ";
        problem += std::to_string(command.least);
        problem += command.least == 1 ? " argument" : " arguments";
    } else {
        problem += std::to_string(command.least);
        problem += " to ";
        problem += std::to_string(command.most);
        problem += " arguments";
    }
    problem += ", got ";
    problem += std::to_string(count);
    return problem;
}

/// Reports whether --help or -h appears before a --.
bool asks_for_help(std::span<const std::string> arguments) {
    for (const std::string& argument : arguments) {
        if (argument == "--")
            return false;
        if (argument == "--help" || argument == "-h")
            return true;
    }
    return false;
}

/// Reports whether a row is a group of subcommands rather than a command that runs.
bool is_group(const Command& command) {
    return command.run == nullptr || !command.subcommands.empty();
}

/// Runs one command, or one of a group's subcommands, after help and the argument count.
int invoke(
    const Command& command,
    const std::string& path,
    std::span<const std::string> arguments,
    Output& output
) {
    if (is_group(command)) {
        if (!arguments.empty() && (arguments[0] == "--help" || arguments[0] == "-h")) {
            print_command_help(output.out, path, command);
            return exit_done;
        }
        if (arguments.empty()) {
            print_entry(output.err, path, command);
            for (const Command& sub : command.subcommands)
                print_entry(output.err, join_path(path, sub.name), sub);
            return exit_usage;
        }
        const Command* sub = find_command(command.subcommands, arguments[0]);
        if (sub == nullptr) {
            report_usage(output, join_path(path, arguments[0]), "unknown command");
            return exit_usage;
        }
        return invoke(*sub, join_path(path, sub->name), arguments.subspan(1), output);
    }
    if (asks_for_help(arguments)) {
        print_command_help(output.out, path, command);
        return exit_done;
    }
    if (arguments.size() < command.least || arguments.size() > command.most) {
        report_usage(output, path, count_problem(command, arguments.size()));
        return exit_usage;
    }
    try {
        return command.run(arguments, output);
    } catch (const std::exception& error) {
        output.err << "oa-tool " << path << ": " << error.what() << '\n';
        return exit_failed;
    }
}

/// Selects a command from a table, or prints help or a usage failure.
int dispatch(
    std::span<const Command> table, std::span<const std::string> arguments, Output& output
) {
    if (arguments.empty()) {
        print_list(output.err, table, {});
        return exit_usage;
    }
    const std::string& first = arguments[0];
    if (first == "--help" || first == "-h") {
        print_list(output.out, table, {});
        return exit_done;
    }
    if (first == "help") {
        if (arguments.size() == 1) {
            print_list(output.out, table, {});
            return exit_done;
        }
        const Command* command = find_command(table, arguments[1]);
        if (command == nullptr) {
            report_usage(output, "help", "unknown command '" + arguments[1] + "'");
            return exit_usage;
        }
        if (arguments.size() == 2) {
            print_command_help(output.out, command->name, *command);
            return exit_done;
        }
        if (arguments.size() == 3) {
            const Command* sub = find_command(command->subcommands, arguments[2]);
            if (sub == nullptr) {
                report_usage(
                    output, std::string(command->name), "unknown command '" + arguments[2] + "'"
                );
                return exit_usage;
            }
            print_command_help(output.out, join_path(command->name, sub->name), *sub);
            return exit_done;
        }
        report_usage(output, "help", "expected a command name");
        return exit_usage;
    }
    const Command* command = find_command(table, first);
    if (command == nullptr) {
        report_usage(output, first, "unknown command");
        return exit_usage;
    }
    return invoke(*command, first, arguments.subspan(1), output);
}

} // namespace

std::span<const Command> commands() {
    return command_table;
}

int run_tool(std::span<const std::string> arguments, Output& output) {
    return dispatch(command_table, arguments, output);
}

int run_tool(
    std::span<const Command> table, std::span<const std::string> arguments, Output& output
) {
    return dispatch(table, arguments, output);
}

} // namespace oa::tool
