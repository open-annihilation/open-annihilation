// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The presence record, between OA machines only. A machine running 3.1c
// stops its split at the type byte and loses nothing else, because the
// record travels alone in its frame and is never sent to such a machine.
//
// The record, every integer little-endian. Fields fill it exactly.
//   0x00  u8 type 0xf0
//   0x01  u16 the record's whole length, the type byte included, 4 to 1024
//   0x03  u8 format version (1; a reader reads any version of 1 or more and
//         skips what it does not know)
//   0x04  fields to the end, each u8 tag, u16 value size, then the value
//
// A string of a to b is a u8 length from a to b and that many bytes. The
// bytes are ascii (0x21 to 0x7e), kebab (a to z, 0 to 9 and '-', not
// starting or ending with '-'), id (kebab with '.' too, as hack ids are)
// or text (well-formed UTF-8 without control characters).
//   0x01 engine: ascii string 1 to 32 the OA version text ("0.8.0", or
//        "0.8.0-dev" for a development build), ascii string 0 to 16 the
//        platform ("Windows", "macOS", "Linux", "iOS", "Android"), ascii
//        string 0 to 16 the architecture ("x64", "x86", "arm64", "armhf").
//   0x10 mod: kebab string 1 to 64 the mod's id, text string 1 to 64 its
//        name, text string 0 to 32 its version, u16 its packaging revision.
//        Absent: 3.1c's rules, no mod.
//   0x11 mod origin: kebab string 1 to 32 the registry's id, ascii string
//        0 to 255 its descriptor URL (empty for a built-in registry, for
//        one added from a file, and whenever the sender cannot give it in
//        255 bytes), u32 the catalogue release (1 or more), 32 bytes the
//        package's SHA-256.
//   0x20 sim hash: 32 bytes, the sim hash the machine plays.
//   0x21 game hacks and 0x22 view hacks: u16 the number of such hacks on,
//        then as many ids as were sent, each an id string 1 to 64. Fewer
//        ids than the count means the list was cut.
//   0x23 developer: u8 flags (bit 0 Developer Mode on, other bits 0), u16
//        the number of overrides, then as many as were sent, each u8 on
//        (0 or 1) and an id string 1 to 64.
//   0x24 limits: u32 the most weapon types this build plays, u8 the most
//        sides. A value that is not five bytes, or names fewer than 256
//        weapon types or five sides, is left out. Absent: an OA build from
//        before these limits.
//   0x30 content: u8 flags, bit 0 the machine can fetch content (it has a
//        downloads folder and at least one enabled registry whose downloads
//        are on); bits 1 to 7 sent as 0 and ignored when read. Absent: the
//        sender does not say.
//   0x31 bundles: u8 flags, bit 0 a chosen bundle differs from a hash the
//        game knows (bits 1 to 7 sent as 0 and ignored when read); a kebab
//        string 1 to 64, the base game; u8 count, at most 16; then that many
//        kebab strings 1 to 64, the expansions. Music changes no rules, so a
//        sender leaves it out. A value that breaks this rule is left out.
//        Absent: the sender does not say which content it plays (an OA 0.7
//        machine, or one that is not playing a managed copy).
//   0x32 language: a text string 1 to 32, the language's name in English
//        ("English", "Simplified Chinese"). Absent: the sender does not say.
//   0x40 map pack: kebab string 1 to 64 the package key, u32 the catalogue
//        release, 32 bytes the package's SHA-256, kebab string 1 to 32 the
//        registry's id, ascii string 0 to 255 its descriptor URL (empty as
//        for 0x11), text string 0 to 64 the pack's name, text string 0 to
//        32 its version (both shown only; empty when the sender cannot
//        give them within those sizes).
//   0x41 computers: the computer players this machine runs, five bytes each,
//        a little-endian player id and a difficulty (0 easy, 1 medium,
//        2 hard). Absent: this machine names none.
//   0x42 computer edit: a little-endian player id, a kind (1 side, 2 colour,
//        3 team, 4 difficulty) and the new value. The host asks the machine
//        that runs that computer player to change it. Absent: no request.
//   0x43 computer brains: u8 count, 1 to 10, then that many items, each a
//        little-endian u32 player id, kebab string 1 to 64 the brain's id,
//        text string 1 to 32 its name, text string 0 to 16 its version,
//        kebab string 1 to 64 its level's id, text string 1 to 24 its
//        level's name, u8 flags (bit 0 full awareness; bits 1 to 7 sent as
//        0 and ignored when read). A computer player on the built-in player
//        is not listed. Absent: this machine's computer players play the
//        built-in player. A value that breaks the rule is left out.
//   0x50 mutators: u8 flags (bit 0 built: the layer was built for this
//        loadout without an error; bit 1 lacking: a package of the loadout
//        is not installed here at its SHA-256; bit 2 failed: a mutator
//        stopped with an error; bits 3 to 7 sent as 0 and ignored when
//        read); 32 bytes the loadout digest; 32 bytes the mutator digest of
//        the applied loadout and its layer (all zero unless built); u8 index,
//        while built the first mutator dropped because its hacks were refused,
//        and while lacking or failed the mutator that names that (0xff for
//        none); u8 total, the mutators in the
//        loadout, 1 to 32; u8 held, how many leading mutators of the list
//        the sender holds, 0 to total; 8 bytes, the sender's state of each
//        position as a little-endian 64-bit word, two bits a position
//        (position n in bits 2n and 2n+1): 0 has it (installed at the
//        list's SHA-256 and version; always 0 from the host), 1 not
//        installed, 2 another release installed, 3 not known yet (the list
//        is not whole here), every position at or past total 0; then as
//        many mutators as were sent, at most total, each: kebab string 1
//        to 64 the id, text string 0 to 32 the version, 8 bytes the first
//        eight bytes of the package's SHA-256, u8 the number of options
//        (0 to 8), then each option as an ascii string 1 to 32 (its key)
//        and a text string 0 to 32 (its value). Absent: the machine plays
//        no mutators.
//   0x51 mutator part: 32 bytes the loadout digest the part belongs to;
//        u8 first, the position of its first mutator (0 to 31); then one
//        or more mutators in the 0x50 form, the last at a position below
//        32. Only a host sends it.
//   0x52 mutator modes: one little-endian u32. Bit n is the clash mode of
//        the mutator at position n in the host's list: set means Add, clear
//        means Replace. Bits at or past total are 0. The host sends this
//        whenever it sends 0x50, because each machine has its own default.
//        A value that is not four bytes, or that sets a bit past the list,
//        is left out. Absent: an older sender. The reader then treats every
//        mode as Replace, and a loadout built that way has a different
//        digest from the host's whenever any mode was Add.
//
// A loadout longer than one record overflows as a host head, a guest's
// held and a host part. The host's 0x50 carries the loadout's state and
// as many leading mutators as fit; total says how many there are. A
// guest's 0x50 carries the host's digest, its own build state, held (how
// many leading mutators of that list it holds) and its state of each
// position, and it sends no items. While a guest whose 0x50 names the
// host's digest holds fewer than total, the host appends one 0x51
// starting at the smallest such held, after the map pack. The guest adds
// the items; its next record raises held. A guest that sends no 0x50 is
// not part of this exchange.

#include "oa/netgame/wire.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::netgame {

/// Type byte of a presence record. Above 3.1c's table, and not a recorder record.
inline constexpr uint8_t presence_record_type = 0xf0;
/// The most bytes a presence record may be, the type byte included.
inline constexpr std::size_t presence_record_max_bytes = 1024;
/// Type byte, length word and format version.
inline constexpr std::size_t presence_record_header_bytes = 4;
/// Format version this engine writes. A reader accepts 1 or more.
inline constexpr uint8_t presence_record_version = 1;
/// Tag byte and the u16 value size that follow it.
inline constexpr std::size_t presence_field_header_bytes = 3;
/// The largest whole map-pack field: 3 bytes of field header plus 65 + 4 +
/// 32 + 33 + 256 + 65 + 33 of value, every string at its longest (the key,
/// the release, the SHA-256, the registry id, the descriptor URL, the name
/// and the version). The sender keeps that much room free for the host's
/// map pack.
inline constexpr std::size_t presence_map_pack_field_max_bytes = 491;
static_assert(
    presence_map_pack_field_max_bytes ==
    presence_field_header_bytes + 65 + 4 + 32 + 33 + 256 + 65 + 33
);

/// Tags of the fields a presence record carries. A tag this engine does not
/// know is skipped.
enum class PresenceTag : uint8_t {
    engine = 0x01,          ///< OA version, platform and architecture
    mod = 0x10,             ///< the mod's id, name, version and packaging revision
    mod_origin = 0x11,      ///< the registry, release and SHA-256 the mod came from
    sim_hash = 0x20,        ///< the sim hash the machine plays
    game_hacks = 0x21,      ///< game hacks that are on
    view_hacks = 0x22,      ///< view hacks that are on
    developer = 0x23,       ///< Developer Mode and its overrides
    limits = 0x24,          ///< the most weapon types and sides this build plays
    content = 0x30,         ///< whether the machine can fetch content
    bundles = 0x31,         ///< the base game and the rule-changing expansions
    language = 0x32,        ///< the language's name in English
    map_pack = 0x40,        ///< the map pack the host is playing
    computers = 0x41,       ///< computer players this machine runs, and their difficulty
    computer_edit = 0x42,   ///< a host's change to another machine's computer player
    computer_brains = 0x43, ///< brains this machine's computer players play
    mutators = 0x50,        ///< the loadout's state and the mutators that fit
    mutator_part = 0x51,    ///< the host's loadout continued from a position
    mutator_modes = 0x52,   ///< one bit per mutator: set means Add, clear means Replace
};

/// 32 bytes: a SHA-256, or the sim hash.
using PresenceDigest = std::array<uint8_t, 32>;

/// One computer player this machine runs with a brain, as field 0x43 carries it.
///
/// A computer player on the built-in player is not listed.
struct PresenceBrain {
    uint32_t player{};     ///< the computer player's id
    std::string id;        ///< the brain's id
    std::string name;      ///< the brain's name
    std::string version;   ///< the brain's version; may be empty
    std::string level_id;  ///< the level's id
    std::string level;     ///< the level's name
    bool full_awareness{}; ///< the brain sees every unit

    /// Compares two brain seats.
    friend bool operator==(const PresenceBrain&, const PresenceBrain&) = default;
};

/// The engine line of a presence record.
struct PresenceEngine {
    std::string version;      ///< OA version text, such as "0.8.0"
    std::string platform;     ///< "Windows", "macOS", "Linux", "iOS" or "Android"; may be empty
    std::string architecture; ///< "x64", "x86", "arm64" or "armhf"; may be empty

    /// Compares two engine lines.
    friend bool operator==(const PresenceEngine&, const PresenceEngine&) = default;
};

/// The mod a presence record names. Absent when the machine plays 3.1c's rules.
struct PresenceMod {
    std::string id;                ///< the mod's id
    std::string name;              ///< the name the card shows
    std::string version;           ///< the mod's version; may be empty
    uint16_t packaging_revision{}; ///< the mod's packaging revision

    /// Compares two mods.
    friend bool operator==(const PresenceMod&, const PresenceMod&) = default;
};

/// Where a mod or a map pack was published.
struct PresenceOrigin {
    std::string registry;       ///< the registry's id
    std::string descriptor_url; ///< the registry's descriptor URL; empty when the sender has none
    uint32_t release{};         ///< the catalogue release, 1 or more
    PresenceDigest sha256{};    ///< the package's SHA-256

    /// Compares two origins.
    friend bool operator==(const PresenceOrigin&, const PresenceOrigin&) = default;
};

/// Hacks of one kind that are on. `ids` may be shorter than `total` when the list was cut.
struct PresenceHacks {
    uint16_t total{};             ///< how many such hacks are on
    std::vector<std::string> ids; ///< the ids that were sent, in order

    /// Compares two hack lists.
    friend bool operator==(const PresenceHacks&, const PresenceHacks&) = default;
};

/// One developer override.
struct PresenceOverride {
    std::string hack; ///< the hack's id
    bool on{};        ///< the override's switch

    /// Compares two overrides.
    friend bool operator==(const PresenceOverride&, const PresenceOverride&) = default;
};

/// Developer Mode and the overrides that were sent. `overrides` may be shorter than `total`.
struct PresenceDeveloper {
    bool on{};                               ///< Developer Mode is on
    uint16_t total{};                        ///< how many overrides are set
    std::vector<PresenceOverride> overrides; ///< the overrides that were sent, in order

    /// Compares two developer blocks.
    friend bool operator==(const PresenceDeveloper&, const PresenceDeveloper&) = default;
};

/// The map pack a presence record names.
struct PresenceMapPack {
    std::string key;            ///< the package key
    uint32_t release{};         ///< the catalogue release
    PresenceDigest sha256{};    ///< the package's SHA-256
    std::string registry;       ///< the registry's id
    std::string descriptor_url; ///< the registry's descriptor URL; empty when the sender has none
    std::string name;           ///< the name the card shows; empty when it would not fit
    std::string version;        ///< the pack's version; empty when it would not fit

    /// Compares two map packs.
    friend bool operator==(const PresenceMapPack&, const PresenceMapPack&) = default;
};

/// Whether a machine can fetch content: the content field, tag 0x30.
struct PresenceContent {
    /// The machine has a downloads folder and at least one enabled registry
    /// whose downloads are on.
    bool fetches{};

    /// Compares two content fields.
    friend bool operator==(const PresenceContent&, const PresenceContent&) = default;
};

/// How many expansions the bundles field carries.
inline constexpr std::size_t presence_bundle_count_max = 16;
/// The longest base-game or expansion id the bundles field carries.
inline constexpr std::size_t presence_bundle_id_max = 64;
/// The expansion that changes no rules. Comparison, the tip and the digest
/// ignore it, and a machine that writes its own record leaves it out.
inline constexpr std::string_view presence_uncompared_expansion{"music"};
/// The most bytes of the canonical content identity: the longest base id,
/// sixteen longest expansion ids, a zero after each, one more zero, and the
/// flag byte.
inline constexpr std::size_t presence_content_identity_max_bytes =
    presence_bundle_id_max + 1 + presence_bundle_count_max * (presence_bundle_id_max + 1) + 1 + 1;

/// The base game and expansions a machine plays, tag 0x31.
struct PresenceBundles {
    std::string base_game;               ///< the base game's id
    std::vector<std::string> expansions; ///< expansion ids, music included when a peer sent it
    /// A chosen base game or rule-changing expansion differs from a hash the game knows.
    bool differs_from_known{};

    /// Compares two bundle statements.
    friend bool operator==(const PresenceBundles&, const PresenceBundles&) = default;
};

/// How two records' bundle statements compare. Unknown when either omits the field.
enum class PresenceContentMatch : uint8_t {
    unknown, ///< one record omits the bundles field, or its value broke the rule
    same,    ///< the same base game and the same rule-changing expansions, and neither hash flag
    differs, ///< the base games, the expansions or a hash flag disagree
};

/// What differs between a player's bundles and the host's. Allocating; the
/// battle room's card uses it. The draw and the start gate use
/// presence_content_match, which does not allocate.
struct PresenceContentGap {
    bool comparable{};       ///< both records carry the bundles field
    bool hashes_differ{};    ///< either side's differs-from-known flag is set
    bool bases_differ{};     ///< the base games are not the same id
    std::string player_base; ///< the player's base game, when comparable
    std::string host_base;   ///< the host's base game, when comparable
    std::vector<std::string>
        missing;                    ///< rule-changing expansions the host has and the player lacks
    std::vector<std::string> extra; ///< rule-changing expansions the player has and the host lacks
};

/// The most mutators one loadout names on the wire.
inline constexpr std::size_t presence_mutators_max = 32;
/// The most options one mutator names on the wire.
inline constexpr std::size_t presence_mutator_options_max = 8;
/// Bytes of a package SHA-256 carried with each mutator.
inline constexpr std::size_t presence_mutator_prefix_bytes = 8;
/// Index value that names no mutator.
inline constexpr uint8_t presence_mutator_index_none = 0xff;

/// One option of a mutator on the wire.
struct PresenceMutatorOption {
    std::string key;   ///< the option's key, ascii, 1 to 32 bytes
    std::string value; ///< the option's value, text, 0 to 32 bytes

    /// Compares two options.
    friend bool operator==(const PresenceMutatorOption&, const PresenceMutatorOption&) = default;
};

/// One mutator on the wire. The SHA-256 prefix is the package's first eight bytes.
struct PresenceMutator {
    std::string id;      ///< the mutator's id, kebab, 1 to 64 bytes
    std::string version; ///< the mutator's version, text, 0 to 32 bytes; may be empty
    /// The first eight bytes of the package's SHA-256.
    std::array<uint8_t, presence_mutator_prefix_bytes> sha256_prefix{};
    std::vector<PresenceMutatorOption> options; ///< the options that were sent, at most 8

    /// Compares two mutators.
    friend bool operator==(const PresenceMutator&, const PresenceMutator&) = default;
};

/// The mutators field, tag 0x50.
///
/// `states` holds two bits a position, position n in bits 2n and 2n+1: 0 the
/// package is installed at the list's SHA-256 and version (always 0 from the
/// host), 1 it is not installed, 2 another release is installed, 3 it is not
/// known yet. Bits at or past `total` are 0. The layer digest is all zero
/// unless `built` is set. While built, `layer_digest` is the mutator digest
/// of the applied loadout and its layer, and `index` is the first mutator
/// this machine dropped because its hacks were refused, or
/// presence_mutator_index_none when it dropped none. `mutators` may be
/// shorter than `total` when the list was cut, and a guest sends none.
struct PresenceMutators {
    bool built{};            ///< the layer was built for this loadout without an error
    bool lacking{};          ///< a package of the loadout is not installed here at its SHA-256
    bool failed{};           ///< a mutator stopped with an error
    PresenceDigest digest{}; ///< the loadout digest
    /// The mutator digest of the applied loadout and its layer; all zero unless built.
    PresenceDigest layer_digest{};
    /// While built, the first mutator this machine dropped because its hacks
    /// were refused. While lacking or failed, the mutator that names that.
    /// presence_mutator_index_none when none of those apply.
    uint8_t index{presence_mutator_index_none};
    uint8_t total{};   ///< how many mutators the loadout names, 1 to 32
    uint8_t held{};    ///< how many leading mutators of that list the sender holds
    uint64_t states{}; ///< two bits a position, little-endian on the wire
    std::vector<PresenceMutator> mutators; ///< the mutators that were sent, in order

    /// Compares two mutator fields.
    friend bool operator==(const PresenceMutators&, const PresenceMutators&) = default;
};

/// Returns the two-bit state of one mutator position.
///
/// Position n lives in bits 2n and 2n+1. A position of 32 or more reads as 0.
///
/// @param states the field's state word
/// @param position the mutator's position, 0 to 31
/// @return 0, 1, 2 or 3
[[nodiscard]] inline uint8_t presence_mutator_state(uint64_t states, uint8_t position) noexcept {
    if (position >= presence_mutators_max)
        return 0;
    const auto shift = static_cast<unsigned>(2u * position);
    return static_cast<uint8_t>((states >> shift) & 0x3u);
}

/// Stores the two-bit state of one mutator position.
///
/// Only the low two bits of `state` are kept. A position of 32 or more is
/// left unchanged.
///
/// @param[in,out] states the field's state word
/// @param position the mutator's position, 0 to 31
/// @param state the state code, 0 to 3
inline void presence_mutator_set_state(uint64_t& states, uint8_t position, uint8_t state) noexcept {
    if (position >= presence_mutators_max)
        return;
    const auto shift = static_cast<unsigned>(2u * position);
    const uint64_t mask = uint64_t{0x3} << shift;
    states = (states & ~mask) | (static_cast<uint64_t>(state & 0x3u) << shift);
}

/// One continued run of the host's mutators, tag 0x51.
struct PresenceMutatorPart {
    PresenceDigest digest{};               ///< the loadout digest the part belongs to
    uint8_t first{};                       ///< the position of the first mutator, 0 to 31
    std::vector<PresenceMutator> mutators; ///< the mutators, in order, at least one

    /// Compares two parts.
    friend bool operator==(const PresenceMutatorPart&, const PresenceMutatorPart&) = default;
};

/// The ceilings this build plays, tag 0x24. These name the build, not the profile.
struct PresenceLimits {
    uint32_t weapon_types{}; ///< the most weapon types this build plays
    uint8_t sides{};         ///< the most sides this build plays

    /// Compares two ceilings.
    friend bool operator==(const PresenceLimits&, const PresenceLimits&) = default;
};

/// One presence record. A field that was not sent, or whose value broke its rule, is absent.
struct PresenceRecord {
    uint8_t version{presence_record_version}; ///< format version, 1 or more
    std::optional<PresenceEngine> engine;
    std::optional<PresenceMod> mod;
    std::optional<PresenceOrigin> mod_origin;
    std::optional<PresenceDigest> sim_hash;
    /// The most weapon types and sides this build plays. Absent on a record
    /// from before these limits, and when the value named fewer than 256
    /// weapon types or five sides.
    std::optional<PresenceLimits> limits;
    std::optional<PresenceHacks> game_hacks;
    std::optional<PresenceHacks> view_hacks;
    std::optional<PresenceDeveloper> developer;
    std::optional<PresenceMapPack> map_pack;
    /// Whether the machine can fetch content; absent when the sender does not say.
    std::optional<PresenceContent> content;
    /// The base game and expansions; absent when the sender does not say.
    std::optional<PresenceBundles> bundles;
    /// The language's English name; absent when the sender does not say.
    std::optional<std::string> language;
    /// The loadout state and the mutators that were sent. Absent when the machine plays none.
    std::optional<PresenceMutators> mutators;
    /// A continued run of the host's mutators. Absent when this record carries none.
    std::optional<PresenceMutatorPart> mutator_part;
    /// Clash modes of the host's list, one bit a position. Set means Add,
    /// clear means Replace. Absent when the sender did not say (an older
    /// record): every mode then reads as Replace.
    std::optional<uint32_t> mutator_modes;

    /// Compares two records.
    friend bool operator==(const PresenceRecord&, const PresenceRecord&) = default;
};

/// Returns the length of the presence record at bytes[0], from its length word.
///
/// The bytes after the length word need not be present. A length word of 4
/// to 1024 is the record's length.
///
/// @param bytes record start; null is refused
/// @param available bytes readable from bytes
/// @param[out] length the record's length, the type byte included
/// @return ok; invalid_type when the byte is not 0xf0; truncated when fewer
///         than 3 bytes are readable; length_mismatch when the length word
///         is outside 4 to 1024; bad_argument for a null pointer
[[nodiscard]] WireError
presence_record_length(const uint8_t* bytes, std::size_t available, uint16_t* length) noexcept;

/// Writes a presence record.
///
/// Fields that are present go out in the order engine, content, bundles,
/// language, mod, mod origin, sim hash, limits, mutators, mutator modes, map pack,
/// mutator part, developer, game hacks, view hacks. A list field carries its full count
/// and as many entries, in order, as keep the record within min(capacity,
/// 1024). Whole mutators are filled before developer overrides and hack
/// ids. A mutator part is written whole. A string that does not fit its
/// rule is refused; it is never cut. The record is never longer than 1024
/// bytes.
///
/// @param record the fields to write
/// @param[out] out storage for the record
/// @param capacity bytes available at out
/// @param[out] written the record's length
/// @return ok; bad_argument for a null pointer, a version of 0, or a value
///         outside its rule; buffer_too_small when the fixed fields do not fit
[[nodiscard]] WireError encode_presence(
    const PresenceRecord& record, uint8_t* out, std::size_t capacity, std::size_t* written
) noexcept;

/// Reads a presence record.
///
/// A wrong frame, a version of 0, or a field that runs past the end refuses
/// the whole record and leaves `out` unchanged. An unknown tag is skipped.
/// A known field whose value breaks its rule is left out: a content field
/// whose value is not one byte, a bundles field whose value breaks its
/// rule, a mutators or mutator-part field outside its rule, and a mutator
/// modes field that is not four bytes or that sets a bit past the list,
/// among them. Bits of a flag byte other than the ones named above are ignored.
/// A repeated tag keeps the first occurrence. It may allocate.
///
/// @param record the record, type byte first
/// @param size its length in bytes; nothing past size is read
/// @param[out] out the fields that were kept
/// @return ok; invalid_type; truncated; length_mismatch when the length word
///         is not size or is outside 4 to 1024; bad_argument for a null
///         pointer or a version of 0
[[nodiscard]] WireError
decode_presence(const uint8_t* record, std::size_t size, PresenceRecord* out);

/// Finds the first value of a tag in a well-framed record.
///
/// The value is not checked against the tag's rule. A record whose frame is
/// wrong, or a tag that is absent, answers false and leaves `value` unchanged.
///
/// @param record the record, type byte first
/// @param size its length in bytes
/// @param tag the tag to find
/// @param[out] value the first value's bytes, inside the record
/// @return true when the record is well-framed and the tag occurs
[[nodiscard]] bool find_presence_field(
    const uint8_t* record, std::size_t size, PresenceTag tag, std::span<const uint8_t>* value
) noexcept;

/// Reads the sim hash of a well-framed record, without allocating.
///
/// The first 0x20 field is the hash when its value is 32 bytes.
///
/// @param record the record, type byte first; null is nothing
/// @param size its length in bytes
/// @return the hash, or nothing
[[nodiscard]] std::optional<PresenceDigest>
presence_sim_hash(const uint8_t* record, std::size_t size) noexcept;

/// Reads the build ceilings of a well-framed record, without allocating.
///
/// The first 0x24 field is kept when its value is five bytes and names at
/// least 256 weapon types and five sides. A shorter, longer or smaller
/// value, and a record without the tag, answers nothing.
///
/// @param record the record, type byte first; null is nothing
/// @param size its length in bytes
/// @return the ceilings, or nothing
[[nodiscard]] std::optional<PresenceLimits>
presence_limits(const uint8_t* record, std::size_t size) noexcept;

/// Compares the bundles fields of two records, without allocating.
///
/// Music is ignored, and the expansions are a set. Either side's
/// differs-from-known flag makes them differ, including when both are set.
/// A record that omits the field, or whose field broke its rule, makes the
/// result unknown: the caller does not treat that as a difference.
///
/// @param first_record one record, type byte first; empty is unknown
/// @param second_record the other record
/// @return unknown, same or differs
[[nodiscard]] PresenceContentMatch presence_content_match(
    std::span<const uint8_t> first_record, std::span<const uint8_t> second_record
) noexcept;

/// Names how a player's bundles differ from the host's.
///
/// Music is ignored. The result is not comparable when either record omits
/// the field. It may allocate.
///
/// @param player the player's record
/// @param host the host's record
/// @return the gap
[[nodiscard]] PresenceContentGap
presence_content_gap(const PresenceRecord& player, const PresenceRecord& host);

/// Writes the canonical bytes of a record's bundles, for the integrity digest.
///
/// Empty, and `written` zero, when the record has no bundles field, so a
/// digest that appends these bytes is unchanged. Otherwise: the base game,
/// a zero, each rule-changing expansion in lexicographic order each ended
/// by a zero, one more zero, then a byte 1 when differs-from-known is set
/// and 0 otherwise. Music is left out. Duplicates are left out.
///
/// @param record the record
/// @param[out] out storage for the bytes
/// @param capacity bytes available at out
/// @param[out] written how many bytes were written
/// @return ok; bad_argument for a null pointer or a bundles value outside
///         its rule; buffer_too_small
[[nodiscard]] WireError presence_content_identity(
    const PresenceRecord& record, uint8_t* out, std::size_t capacity, std::size_t* written
) noexcept;

/// Writes the value of a map-pack field, tag 0x40, without its field header.
///
/// @param pack the map pack
/// @param[out] out storage for the value
/// @param capacity bytes available at out
/// @param[out] written the value's length
/// @return ok; bad_argument for a null pointer or a value outside its rule;
///         buffer_too_small
[[nodiscard]] WireError encode_presence_map_pack(
    const PresenceMapPack& pack, uint8_t* out, std::size_t capacity, std::size_t* written
) noexcept;

/// Adds a field to a well-framed presence record and fixes its length word.
///
/// The field is written at the record's end. A result past 1024 bytes, or
/// past capacity, is refused and the record is left unchanged.
///
/// @param[in,out] record the record, type byte first
/// @param size the record's length before the field is added
/// @param capacity bytes available at record
/// @param tag the field's tag
/// @param value the field's value
/// @param[out] written the record's length after the field is added
/// @return ok; bad_argument for a null pointer, a value of more than 65535
///         bytes, or a record that is not well-framed; buffer_too_small
[[nodiscard]] WireError append_presence_field(
    uint8_t* record,
    std::size_t size,
    std::size_t capacity,
    PresenceTag tag,
    std::span<const uint8_t> value,
    std::size_t* written
) noexcept;

/// Writes the value of a computer-brains field, tag 0x43, without its field header.
///
/// The value is a count and that many whole items, in order, as many as fit
/// in `capacity` and never more than ten. A name, version or level longer
/// than its limit is cut at a UTF-8 character boundary. An id or a level id
/// is not cut. `sent` is how many items were written. The count byte is that
/// number, not the size of `brains`.
///
/// @param brains the computer players that play a brain, in the order to send
/// @param[out] out storage for the value
/// @param capacity bytes available at out
/// @param[out] written the value's length
/// @param[out] sent how many items were written
/// @return ok; bad_argument for a null pointer, an empty list, or an item
///         outside its rule; buffer_too_small when not even one item fits
[[nodiscard]] WireError encode_presence_brains(
    std::span<const PresenceBrain> brains,
    uint8_t* out,
    std::size_t capacity,
    std::size_t* written,
    std::size_t* sent
) noexcept;

/// Reads the value of a computer-brains field, tag 0x43.
///
/// A value that breaks the field's rule is left out: a count outside 1 to
/// 10, an id or a level id that is not kebab, an empty name or level, a
/// version or a text that is not well-formed, a flag bit past 0, bytes left
/// over, or a value cut short. The read never looks past `value`. A broken
/// value does not refuse the record it was found in; the caller leaves the
/// field out.
///
/// @param value the field's value, without its header
/// @return the items, or nothing when the value breaks the rule
[[nodiscard]] std::optional<std::vector<PresenceBrain>>
decode_presence_brains(std::span<const uint8_t> value);

/// Writes the value of a mutator-part field, tag 0x51, without its field header.
///
/// The value is the loadout digest, `first`, and as many whole mutators from
/// that position as fit in `capacity`. `sent` is how many were written.
///
/// @param loadout the loadout the part is drawn from
/// @param first the position of the first mutator to write
/// @param[out] out storage for the value
/// @param capacity bytes available at out
/// @param[out] written the value's length
/// @param[out] sent how many mutators were written
/// @return ok; bad_argument for a null pointer, a `first` at or past the
///         loadout's mutator count or past 31, or a mutator outside its
///         rule; buffer_too_small when not even one mutator fits
[[nodiscard]] WireError encode_presence_mutator_part(
    const PresenceMutators& loadout,
    uint8_t first,
    uint8_t* out,
    std::size_t capacity,
    std::size_t* written,
    uint8_t* sent
) noexcept;

/// The fixed part of a mutators field, without the mutators themselves.
///
/// The draw path and the start gate read this from a record without allocating.
struct PresenceMutatorsState {
    bool built{};            ///< the layer was built for this loadout without an error
    bool lacking{};          ///< a package is not installed here at its SHA-256
    bool failed{};           ///< a mutator stopped with an error
    PresenceDigest digest{}; ///< the loadout digest
    /// The mutator digest of the applied loadout and its layer; all zero unless built.
    PresenceDigest layer_digest{};
    /// While built, the first mutator dropped because its hacks were refused.
    /// While lacking or failed, the mutator that names that, or 0xff.
    uint8_t index{};
    uint8_t total{};   ///< how many mutators the loadout names
    uint8_t held{};    ///< how many leading mutators the sender holds
    uint64_t states{}; ///< two bits a position

    /// Compares two states.
    friend bool operator==(const PresenceMutatorsState&, const PresenceMutatorsState&) = default;
};

/// Reads the first mutators field's fixed part, without allocating.
///
/// The mutators after the fixed part are not read. A record with no mutators
/// field, a field shorter than the fixed part, or a fixed part outside its
/// rule answers nothing.
///
/// @param record the record, type byte first; null is nothing
/// @param size its length in bytes
/// @return the state, or nothing
[[nodiscard]] std::optional<PresenceMutatorsState>
presence_mutators_state(const uint8_t* record, std::size_t size) noexcept;

/// The host's loadout as far as its records have carried it.
///
/// A guest's `held` is `mutators.size()`.
struct PresenceLoadoutAssembly {
    PresenceDigest digest{};               ///< the loadout digest the parts belong to
    uint8_t total{};                       ///< how many mutators the loadout names
    std::vector<PresenceMutator> mutators; ///< the leading mutators received so far

    /// Compares two assemblies.
    friend bool
    operator==(const PresenceLoadoutAssembly&, const PresenceLoadoutAssembly&) = default;
};

/// Folds one host record into an assembly of its loadout.
///
/// A record with no mutators field empties the assembly. A mutators field
/// whose digest or total differs from the assembly starts it again from
/// that field's mutators. Mutators and a part of the same digest are added
/// where they continue the list: a part whose `first` is past the count
/// already held is ignored, and items already held are skipped. Anything
/// else is ignored. The assembly is never longer than `total`.
///
/// @param[in,out] assembly the loadout so far
/// @param host one host record
/// @return true when the assembly changed
[[nodiscard]] bool
presence_assembly_take(PresenceLoadoutAssembly& assembly, const PresenceRecord& host);

/// Tells whether every mutator of the loadout has arrived.
///
/// @param assembly the loadout so far
/// @return true when `total` is not 0 and every position has a mutator
[[nodiscard]] bool presence_assembly_whole(const PresenceLoadoutAssembly& assembly) noexcept;

/// How one mutator treats a unit or weapon that already has its name.
enum class PresenceClashMode : uint8_t {
    replace, ///< the mutator's record takes the place of the one already there
    add,     ///< the mutator's record is added beside the one already there
};

/// Writes the clash mode of each mutator in a host's list.
///
/// Bit n of `mask` set writes Add at position n; a clear bit writes Replace.
/// A reader that has the loadout and this mask builds each entry's mode from
/// the byte written here. A mask that sets a bit at or past `count` is
/// refused and `modes` is left unchanged.
///
/// @param mask the 0x52 value
/// @param count how many mutators the list names, 1 to 32
/// @param[out] modes one mode per position; `count` entries
/// @return ok; bad_argument for a null pointer, a count outside 1 to 32, or
///         a bit at or past `count`
[[nodiscard]] WireError
presence_clash_modes(uint32_t mask, uint8_t count, PresenceClashMode* modes) noexcept;

/// Tells whether a peer may receive a presence record.
///
/// This is the one rule: the peer's setup block carries the 'OA' engine
/// signature that 0.7 already sends (sent_by_open_annihilation). That is how
/// OA machines find each other; the setup block itself carries no presence.
///
/// @param block the 0xb9-byte setup block; null is no
/// @return true when the peer may receive a presence record
[[nodiscard]] bool presence_peer(const uint8_t* block) noexcept;

} // namespace oa::netgame
