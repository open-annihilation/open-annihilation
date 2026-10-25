// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// oa-tool's command table. A command is a row: its name, the arguments it
// takes, the line help prints, and the function that runs it. A group names
// subcommands and leaves run null. commands.cpp holds the table; each
// command's function is declared here, so that file includes nothing else.

#pragma once

#include <cstdint>
#include <iosfwd>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>

namespace oa::tool {

/// The command finished what it was asked.
inline constexpr int exit_done = 0;

/// The command ran and could not do it, or the answer is no.
inline constexpr int exit_failed = 1;

/// The invocation does not name a command the tool can run.
inline constexpr int exit_usage = 2;

/// No upper bound on how many arguments a command takes.
inline constexpr uint32_t any_count = UINT32_MAX;

/// Where a command prints. Tests pass their own streams; the program passes
/// the standard streams.
struct Output {
    std::ostream& out; ///< ordinary results
    std::ostream& err; ///< failures and the usage shown for a bad invocation
};

/// One command, or a group of subcommands.
///
/// least and most count every argument after the command's own name, options
/// included, before the command runs. A command whose options make that count
/// vary sets most to any_count and checks its own arguments.
struct Command {
    std::string_view name;      ///< the word that selects the command
    std::string_view arguments; ///< usage after the name, empty when it takes none
    std::string_view summary;   ///< one line, printed in the command list
    std::string_view help;      ///< the paragraph `help` prints under the usage line
    uint32_t least{};           ///< fewest arguments, inclusive
    uint32_t most{};            ///< most arguments, inclusive; any_count for no limit
    int (*run)(std::span<const std::string> arguments, Output& output){}; ///< null for a group
    std::span<const Command> subcommands{}; ///< empty for a command that runs itself
};

/// Reports that a command ran and could not finish.
///
/// run_tool prints the message and exits with exit_failed.
class Failure : public std::runtime_error {
  public:

    /// Reports a failure with the message run_tool prints.
    ///
    /// @param what the reason, with no command path; run_tool adds the path
    explicit Failure(const std::string& what) : std::runtime_error(what) {}
};

// The archive commands, defined in archive_commands.cpp.

/// Lists every file in an archive.
///
/// @param arguments the archive path
/// @param[in,out] output receives one `path<TAB>size` line per file
/// @return exit_done
/// @throws Failure when the archive cannot be read
[[nodiscard]] int run_list(std::span<const std::string> arguments, Output& output);

/// Copies one file out of an archive.
///
/// @param arguments the archive path, the entry and the output path
/// @param output unused; the command writes a file and prints nothing
/// @return exit_done
/// @throws Failure when the archive or the entry cannot be read, or the file cannot be written
[[nodiscard]] int run_extract(std::span<const std::string> arguments, Output& output);

/// Copies one file from a game directory's loose files or its archives.
///
/// @param arguments the game directory, the entry, the output path and any archive paths
/// @param[in,out] output receives the loose-or-archive line
/// @return exit_done
/// @throws Failure when the file cannot be read or written
[[nodiscard]] int run_asset_extract(std::span<const std::string> arguments, Output& output);

/// Decodes one image stored in an archive.
///
/// @param arguments the archive path, the image entry and the output path
/// @param[in,out] output receives the image's size line
/// @return exit_done
/// @throws Failure when the image cannot be read, decoded or written
[[nodiscard]] int run_preview(std::span<const std::string> arguments, Output& output);

/// Decodes a PCX image file.
///
/// @param arguments the PCX path and the output path
/// @param[in,out] output receives the image's size line
/// @return exit_done
/// @throws Failure when the image cannot be read, decoded or written
[[nodiscard]] int run_decode_pcx(std::span<const std::string> arguments, Output& output);

// The package command, defined in pack.cpp.

/// Packs a folder as a .oamod, a .oalang, a .oamap, a .oaexp, a .oamut or a .oabrain.
///
/// One positional argument is the folder. `--out FILE` names the package,
/// and `--force` replaces a file that already exists. `--game-dir DIR` is
/// the game's data; map packs need the game's palette for previews, and
/// giving it for another kind is a usage error. Options may be written
/// before or after the folder. The manifest is written first and every other
/// file follows in byte order of its path.
///
/// @param arguments the folder and any options
/// @param[in,out] output receives the packed line, and diagnostics on failure
/// @return exit_done, or exit_usage when the arguments are not the command's
/// @throws Failure when the folder cannot be packed
[[nodiscard]] int run_pack(std::span<const std::string> arguments, Output& output);

// The check command, defined in check.cpp.

/// Checks a package and reports the facts a catalogue lists.
///
/// One positional argument is the package file. `--game-dir DIR` is the game
/// folder an install would read settings from, and a map pack requires it.
/// Each `--mod FOLDER[=KEY]` is a further target for a map pack. The key is
/// the text after the last `=`, or the folder's catalogue origin, or the
/// profile's id when that folder has no release. `--accept-unimplemented-hacks`
/// accepts a hack this build does not carry out yet. `--json` prints one JSON
/// object; otherwise each fact is a `key: value` line, and a map pack then
/// prints one line per map and target. Options may be written before or
/// after the file. A map that does not fit the base game is refused. A mod
/// that does not fit is only recorded.
///
/// @param arguments the package and any options
/// @param[in,out] output receives the report, and a usage line when the
///        arguments are not the command's
/// @return exit_done when the package would be installed, exit_failed when it
///         would be refused or a map does not fit the base game, or exit_usage
///         when the arguments are not the command's
[[nodiscard]] int run_check(std::span<const std::string> arguments, Output& output);

// The mutator commands, defined in mutator.cpp.

/// Packs a mutator folder as a .oamut.
///
/// One positional argument is the folder. It must hold `oamut.yaml` at its
/// top; another folder is a usage error. The other arguments are `run_pack`'s,
/// and the package is packed by that command. `--game-dir` is a usage error.
///
/// @param arguments the folder and any options
/// @param[in,out] output receives the packed line, and a usage line when the
///        folder is not a mutator
/// @return exit_done, or exit_usage when the folder holds no oamut.yaml or
///         the arguments are not the command's
/// @throws Failure when the folder cannot be packed
[[nodiscard]] int run_mutator_pack(std::span<const std::string> arguments, Output& output);

// The brain commands, defined in brain.cpp.

/// Packs a brain folder as a .oabrain.
///
/// One positional argument is the folder. It must hold `oabrain.yaml` at its
/// top; another folder is a usage error. The other arguments are `run_pack`'s,
/// and the package is packed by that command. `--game-dir` is a usage error.
///
/// @param arguments the folder and any options
/// @param[in,out] output receives the packed line, and a usage line when the
///        folder is not a brain
/// @return exit_done, or exit_usage when the folder holds no oabrain.yaml or
///         the arguments are not the command's
/// @throws Failure when the folder cannot be packed
[[nodiscard]] int run_brain_pack(std::span<const std::string> arguments, Output& output);

/// Loads a brain against a game and reports how it started.
///
/// One positional argument is a `.oabrain` or a folder whose top holds
/// `oabrain.yaml`. The folder's name is the brain's id. `--game-dir` is
/// required. `--mod` is a mod folder layered over the game. `--level`
/// selects a level; the first is used when it is omitted. `--side` is the
/// side whose commander the seat holds, from 0 to 255, and it is 0 when
/// omitted. `--json` prints one object. The check seats the brain in a
/// match of the game's definitions on a flat stand-in map and never ticks
/// that match.
///
/// @param arguments the file or folder and the options
/// @param[in,out] output receives the report, and a usage line when the
///        arguments are not the command's
/// @return exit_done when the brain was seated, exit_failed when it was not
///         or the game cannot be mounted, or exit_usage when the arguments
///         are not the command's
[[nodiscard]] int run_brain_check(std::span<const std::string> arguments, Output& output);

/// Converts an ONNX model to a .oann and reports the integer difference.
///
/// One positional argument is the model. `--calibrate` is required. `--out`
/// names the .oann; without it the model's path is used with `.oann`.
/// `--weights` is `int8` or `int16`. `--max-difference` refuses a larger
/// difference and writes nothing. `--force` replaces an existing file.
/// `--json` prints one object. A model that cannot be converted, or a
/// difference past the limit, writes nothing.
///
/// @param arguments the model and the options
/// @param[in,out] output receives the report, and a usage line when the
///        arguments are not the command's
/// @return exit_done when the file was written and every difference is within
///         the limit, or exit_usage when the arguments are not the command's
/// @throws Failure when the model cannot be converted or a difference passes
///         the limit
[[nodiscard]] int run_brain_net(std::span<const std::string> arguments, Output& output);

/// Checks a .oamut and reports the facts a catalogue lists.
///
/// One positional argument is the package. A file of another kind is a usage
/// error. The other arguments are `run_check`'s, and the report is that
/// command's.
///
/// @param arguments the package and any options
/// @param[in,out] output receives the report, and a usage line when the file
///        is not a .oamut
/// @return exit_done when the mutator would be installed, exit_failed when it
///         would be refused, or exit_usage when the file is not a .oamut or
///         the arguments are not the command's
[[nodiscard]] int run_mutator_check(std::span<const std::string> arguments, Output& output);

/// Imports TA: Mutation mutators into mutator folders.
///
/// Each positional argument is a `.tam` or a `.zip`. A `.tam`'s companions
/// are read from its own folder. A zip's names are the names the archive
/// recorded. `--out` is the directory that receives one folder per mutator.
/// `--id` and `--version` set that mutator's id and version, and they are a
/// usage error when more than one mutator is named. `--tag` is repeated.
/// `--works-with CODE=ENTRY` maps one compatibility code. `--pack` also
/// writes a `.oamut`. `--force` replaces an existing folder and package.
/// A link is not followed.
///
/// @param arguments the inputs and the options
/// @param[in,out] output receives one line per mutator, its warnings and a
///        refusal, and a usage line when the arguments are not the command's
/// @return exit_done when every mutator was imported, exit_failed when one
///         was refused or could not be written, or exit_usage when the
///         arguments are not the command's
/// @throws Failure when a folder cannot be written
[[nodiscard]] int run_mutator_import(std::span<const std::string> arguments, Output& output);

/// Reports what a loadout changes, or compares two loadouts.
///
/// `--game-dir` is the game folder, mounted the way the game mounts it.
/// `--mod-dir` is a mod folder over that game. Each `--mutator` is a package
/// folder, with options after the last colon. An id with no path is refused.
/// `--versus` is a second loadout. `--allow` is a file of differences that
/// may remain, and it needs `--versus`. `--map` names the map whose maps
/// hooks run. `--seed` is the match seed, and it is 0 when left out.
/// `--dump` writes the mutated file list and does not change the report.
/// A map the game does not have, and a `--dump` file that cannot be
/// created, are usage errors.
///
/// @param arguments the options
/// @param[in,out] output receives the report or the comparison, and a usage
///        line when the arguments are not the command's
/// @return exit_done when the report was written, or a comparison has no
///         remaining difference and no unused allowance; exit_failed when a
///         mutator fails, the folder is not a usable game, or a comparison
///         does not match; exit_usage when the arguments are not the command's
/// @throws Failure when a mutator fails, the folder is not a usable game,
///         or the dump cannot be written
[[nodiscard]] int run_mutator_diff(std::span<const std::string> arguments, Output& output);

// The catalogue commands, defined in catalogue.cpp.

/// Makes a sealed publisher key.
///
/// `--id` is the key id a signature names. `--out` is the key file, which is
/// created only when that name is new. `--passphrase-file` reads the
/// passphrase from a file; otherwise the terminal asks twice, with echo off.
/// The passphrase is at least 12 bytes. Prints the key id, the public key
/// and its fingerprint.
///
/// @param arguments the options
/// @param[in,out] output receives the three lines, and a usage line when the
///        arguments are not the command's
/// @return exit_done, or exit_usage when the arguments are not the command's
/// @throws Failure when the key cannot be made
[[nodiscard]] int run_catalogue_keygen(std::span<const std::string> arguments, Output& output);

/// Prints a sealed key's id, public key and fingerprint.
///
/// The passphrase is not used.
///
/// @param arguments the key file
/// @param[in,out] output receives the three lines
/// @return exit_done
/// @throws Failure when the file is not a sealed key
[[nodiscard]] int run_catalogue_public(std::span<const std::string> arguments, Output& output);

/// Signs a catalogue with a sealed key.
///
/// `--key` is the sealed key. The positional argument is the catalogue.
/// `--out` names the signature file; without it the signature is written
/// beside the catalogue, with `.sig` added to the name. The catalogue is
/// refused, before the key is opened, when this build cannot read it. The
/// written pair is checked before the command reports success.
///
/// @param arguments the key, the catalogue and any options
/// @param[in,out] output receives the signed line, and a usage line when the
///        arguments are not the command's
/// @return exit_done, or exit_usage when the arguments are not the command's
/// @throws Failure when the catalogue cannot be signed
[[nodiscard]] int run_catalogue_sign(std::span<const std::string> arguments, Output& output);

/// Checks a catalogue's signature.
///
/// The positional argument is the catalogue. `--sig` names the signature
/// file; without it the signature is the catalogue's name with `.sig` added.
/// `--descriptor` reads that registry's id and keys. `--registry` and one or
/// more `--key` values name them directly; every key is the key the
/// signature names, and the first is the one that is trusted. Prints the
/// signing key and the verdict, and exits 0 when the catalogue can be used.
/// A refused catalogue prints the verdict and exits 1.
///
/// @param arguments the catalogue and any options
/// @param[in,out] output receives the verdict, and a usage line when the
///        arguments are not the command's
/// @return exit_done when the catalogue can be used, exit_failed when it is
///         refused, or exit_usage when the arguments are not the command's
/// @throws Failure when the catalogue or the signature cannot be read
[[nodiscard]] int run_catalogue_verify(std::span<const std::string> arguments, Output& output);

// The engine feed commands, defined in engine_feed.cpp.

/// Writes one release of the signed engine feed.
///
/// `--version`, `--date`, `--from`, `--page`, `--out` and at least one
/// `--mirror` are required, with one or more package files. `--release`
/// defaults to the release number of `--version`. `--notes` is a JSON object
/// of language to lines. `--previous` copies older releases from an earlier
/// feed, five releases in all. The file is read back before success.
///
/// @param arguments the options and the package files
/// @param[in,out] output receives the written line, and a usage line when the
///        arguments are not the command's
/// @return exit_done, or exit_usage when the arguments are not the command's
/// @throws Failure when the feed cannot be written
[[nodiscard]] int run_engine_feed_write(std::span<const std::string> arguments, Output& output);

/// Signs an engine feed with a sealed key.
///
/// `--key` is the sealed key. The positional argument is the feed. `--out`
/// names the signature file; without it the signature is written beside the
/// feed, with `.sig` added to the name. A feed this build would refuse is
/// refused before the key is opened. The written pair is checked before the
/// command reports success.
///
/// @param arguments the key, the feed and any options
/// @param[in,out] output receives the signed line, and a usage line when the
///        arguments are not the command's
/// @return exit_done, or exit_usage when the arguments are not the command's
/// @throws Failure when the feed cannot be signed
[[nodiscard]] int run_engine_feed_sign(std::span<const std::string> arguments, Output& output);

/// Checks an engine feed's signature.
///
/// The positional argument is the feed, or `--url` fetches the feed and that
/// address with `.sig` added. `--sig` names the signature file; without it
/// the signature is the feed's name with `.sig` added. `--key` is an id, `=`,
/// `ed25519:` and the key in base64. Without `--key` the keys are this
/// build's pinned keys. Prints the signing key and one line a release, and
/// exits 0 when the feed can be used. A refused feed prints the verdict and
/// exits 1.
///
/// @param arguments the feed or the address, and any options
/// @param[in,out] output receives the verdict, and a usage line when the
///        arguments are not the command's
/// @return exit_done when the feed can be used, exit_failed when it is
///         refused, or exit_usage when the arguments are not the command's
/// @throws Failure when the feed or the signature cannot be read
[[nodiscard]] int run_engine_feed_check(std::span<const std::string> arguments, Output& output);

// The registry commands, defined in registry.cpp.

/// Makes an empty direct registry and signs its catalogue.
///
/// FOLDER must be missing or empty. `--base-url` is the http address the
/// registry is served at, with no trailing slash. `--id` and `--name` name
/// it. `--key` is a sealed publisher key. `--passphrase-file` reads the
/// passphrase from a file; otherwise the terminal asks. `--homepage` is an
/// optional page. The descriptor and the signed catalogue are read back
/// before the command reports success.
///
/// @param arguments the folder and the options
/// @param[in,out] output receives the descriptor path and the fingerprint,
///        and a usage line when the arguments are not the command's
/// @return exit_done, or exit_usage when the arguments are not the command's
/// @throws Failure when the registry cannot be made
[[nodiscard]] int run_registry_init(std::span<const std::string> arguments, Output& output);

/// Copies a registry into a folder.
///
/// URL is the registry descriptor. The catalogue's signature is checked, and
/// a sequence lower than the one FOLDER already holds stops the copy. Each
/// package and picture is checked by SHA-256. A `.part` is resumed with a
/// Range request. A tokens registry is copied through its download API. The
/// catalogue and its signature are written last, byte for byte.
///
/// @param arguments the descriptor URL, the folder and any options
/// @param[in,out] output receives the registry's name, catalogue and
///        fingerprints, a check's code when one is asked for, and a usage
///        line when the arguments are not the command's
/// @return exit_done when the folder holds the registry, exit_failed when a
///         check stops the copy, or exit_usage when the arguments are not
///         the command's
/// @throws Failure when the registry cannot be read
[[nodiscard]] int run_registry_mirror(std::span<const std::string> arguments, Output& output);

// The interface-word command, defined in interface_words.cpp.

/// Prints the English texts the build passes to the interface catalogue.
///
/// The texts are the ones each module exports (the Library's words, the
/// settings dialog, the map browser, and the rest). `--json` is required.
/// The array is sorted and holds each text once.
///
/// @param arguments `--json`
/// @param[in,out] output receives the JSON array, or a usage line
/// @return exit_done, or exit_usage when the arguments are not `--json`
[[nodiscard]] int run_interface_words(std::span<const std::string> arguments, Output& output);

/// Returns the tool's commands, in the order help lists them.
///
/// @return the command table
[[nodiscard]] std::span<const Command> commands();

/// Runs one invocation of the tool.
///
/// No arguments prints the command list on output.err and returns exit_usage.
/// `--help`, `-h` or `help` prints it on output.out and returns exit_done.
/// `help NAME` and `NAME --help` print that command. An unknown command, or a
/// count of arguments outside the command's least and most, prints the
/// problem and `oa-tool help` on output.err and returns exit_usage. A
/// Failure, or any other exception the command throws, prints the message on
/// output.err and returns exit_failed.
///
/// @param arguments the program's arguments, without the program name
/// @param[in,out] output where the command and the dispatcher print
/// @return exit_done, exit_failed or exit_usage
[[nodiscard]] int run_tool(std::span<const std::string> arguments, Output& output);

/// Runs one invocation against a chosen command table.
///
/// The table replaces commands() for the duration of the call. Tests use it
/// for a group that the tool does not ship.
///
/// @param table the commands that can be named, and their subcommands
/// @param arguments the program's arguments, without the program name
/// @param[in,out] output where the command and the dispatcher print
/// @return exit_done, exit_failed or exit_usage
[[nodiscard]] int
run_tool(std::span<const Command> table, std::span<const std::string> arguments, Output& output);

} // namespace oa::tool
