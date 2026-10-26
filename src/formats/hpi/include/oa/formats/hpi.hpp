// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/base/bytes.hpp"

#include <cstdint>
#include <array>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <unordered_set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa {

// Packed little-endian images of the HPI records. A uint8 follows uint32
// fields with no pad on disk, so these records stay packed.
namespace formats::hpi {
inline constexpr uint32_t HeaderMarker = 0x49504148U; // "HAPI"
inline constexpr uint32_t ChunkMarker = 0x48535153U;  // "SQSH"
inline constexpr uint32_t VersionV1 = 0x00010000U;    // Total Annihilation
inline constexpr uint32_t VersionV2 = 0x00020000U;    // TA: Kingdoms
inline constexpr uint16_t HeaderSize = 20;
inline constexpr uint16_t DirectoryEntrySize = 9;
inline constexpr uint16_t FileEntrySize = 9;
inline constexpr uint16_t ChunkHeaderSize = 9;
inline constexpr uint16_t SQSHHeaderSize = 19;
inline constexpr uint16_t CompressionNone = 0;
inline constexpr uint16_t CompressionLZ77 = 1;
inline constexpr uint16_t CompressionZLib = 2;
inline constexpr uint16_t EntryTypeFile = 0;
inline constexpr uint16_t EntryTypeDirectory = 1;

#pragma pack(push, 1)

struct Header {
    uint32_t marker{};
    uint32_t version{};
    uint32_t directory_size{};
    uint32_t decrypt_key{};
    uint32_t offset{};
};

struct ChunkHeader {
    uint32_t magic{};
    uint8_t version{};
    uint8_t compression_type{};
    uint8_t encoded{};
    uint32_t compressed_size{};
    uint32_t decompressed_size{};
    uint32_t checksum{};
};

struct DirectoryNode {
    uint32_t count{};
    uint32_t list_offset{};
};

struct DirectoryEntry {
    uint32_t name_offset{};
    uint32_t data_offset{};
    uint8_t type{};
};

struct FileEntry {
    uint32_t offset{};
    uint32_t size{};
    uint8_t compression{};
};

#pragma pack(pop)

static_assert(sizeof(Header) == HeaderSize);
static_assert(offsetof(Header, version) == 4);
static_assert(offsetof(Header, directory_size) == 8);
static_assert(offsetof(Header, decrypt_key) == 12);
static_assert(offsetof(Header, offset) == 16);
static_assert(sizeof(ChunkHeader) == SQSHHeaderSize);
static_assert(offsetof(ChunkHeader, version) == 4);
static_assert(offsetof(ChunkHeader, compression_type) == 5);
static_assert(offsetof(ChunkHeader, encoded) == 6);
static_assert(offsetof(ChunkHeader, compressed_size) == 7);
static_assert(offsetof(ChunkHeader, decompressed_size) == 11);
static_assert(offsetof(ChunkHeader, checksum) == 15);
static_assert(sizeof(DirectoryNode) == 8);
static_assert(offsetof(DirectoryNode, list_offset) == 4);
static_assert(sizeof(DirectoryEntry) == DirectoryEntrySize);
static_assert(offsetof(DirectoryEntry, data_offset) == 4);
static_assert(offsetof(DirectoryEntry, type) == 8);
static_assert(sizeof(FileEntry) == FileEntrySize);
static_assert(offsetof(FileEntry, size) == 4);
static_assert(offsetof(FileEntry, compression) == 8);
static_assert(ChunkHeaderSize != SQSHHeaderSize);
static_assert(VersionV1 != VersionV2);

// Directory-entry flag bit marking a subdirectory; other stored bits are ignored.
inline constexpr uint8_t EntryFlagDirectory = 0x01;
// The archive's 36-byte copyright trailer, the text every archive must end
// with, with the year as four zero digits and no terminating NUL; the four
// year characters are not compared.
inline constexpr char TrailerChars[] = {0x43, 0x6f, 0x70, 0x79, 0x72, 0x69, 0x67, 0x68, 0x74,
                                        0x20, 0x30, 0x30, 0x30, 0x30, 0x20, 0x43, 0x61, 0x76,
                                        0x65, 0x64, 0x6f, 0x67, 0x20, 0x45, 0x6e, 0x74, 0x65,
                                        0x72, 0x74, 0x61, 0x69, 0x6e, 0x6d, 0x65, 0x6e, 0x74};
inline constexpr std::string_view Trailer{TrailerChars, sizeof TrailerChars};
static_assert(Trailer.size() == 36);
inline constexpr std::size_t TrailerYearOffset = 10;
/// Most folder entries an AssetStore keeps listed for loose-file lookups. With
/// more, the store lists the folders on every lookup instead.
inline constexpr std::size_t LooseIndexEntryLimit = 16 * 1024;
inline constexpr std::size_t TrailerYearBytes = 4;
// Decoded bytes per chunk of a compressed entry, and the per-read block cache.
inline constexpr uint32_t BlockBytes = 0x10000;
/// Largest file, in bytes, read whole from an archive or a loose folder; a
/// larger one is refused before anything is allocated. The largest file of
/// the game and of the mods it was measured against is 238.5 MB, an effects
/// animation.
inline constexpr uint64_t EntryByteLimit = 512ULL << 20;

/// Returns whether a file of `size` bytes may be read whole.
///
/// @param size the file's size in bytes
/// @return true when it is at most EntryByteLimit
[[nodiscard]] constexpr bool entry_size_allowed(uint64_t size) noexcept {
    return size <= EntryByteLimit;
}

// Plain *.HPI archives mounted from the game directory before the scan stops.
inline constexpr int PlainArchiveMountLimit = 10;
/// Most files one pack layer lists. A longer list is refused.
inline constexpr std::size_t PackLayerFileLimit = 4096;
/// Most paths one mutator layer holds, served and hidden together.
inline constexpr std::size_t MutatorLayerEntryLimit = 65536;
/// Most mask patterns one mutator layer holds: 32 mutators of 64 masks.
inline constexpr std::size_t MutatorLayerPatternLimit = 2048;
/// Most archives one mutator layer holds open.
inline constexpr std::size_t MutatorLayerArchiveLimit = 512;
/// Most bytes of content given in memory one mutator layer holds, taken together.
/// 256 MiB.
inline constexpr uint64_t MutatorLayerContentByteLimit = 256ULL << 20;
/// Most bytes one mask pattern holds.
inline constexpr std::size_t MutatorMaskByteLimit = 256;
/// Most `*` and `?` one mask pattern holds, counted together.
inline constexpr std::size_t MutatorMaskWildcardLimit = 16;
/// FoundEntry::mount of a match from the mutator layer.
inline constexpr int MutatorLayerMount = -2;

/// Where the chunks of one compressed file node are stored, from its
/// decrypted chunk-size table.
struct ChunkTable {
    /// The file node the table belongs to.
    uint32_t node{};
    /// Archive offset of each chunk's stored bytes, then the offset just past
    /// the last chunk; the table's sizes summed at 64 bits.
    std::vector<uint64_t> offsets;
};

// Squash codec status; squash_status_name gives each value's SQUASHERR_* name.
enum class SquashStatus : uint8_t {
    ok = 0,
    bad_header = 1,
    bad_checksum = 2,
    bad_unpack_size = 3,
    bad_unpack_type = 4,
    bad_pack_type = 5,
    bad_params = 6,
};
} // namespace formats::hpi

/// Derives the per-archive XOR key from header byte 12.
///
/// @param header_key the header's key byte
/// @return 0 for 0, otherwise ~((byte >> 6) | (byte << 2)) truncated to 8
///         bits; header 0xFF also yields 0, which disables decryption
[[nodiscard]] uint8_t hpi_archive_key(uint8_t header_key) noexcept;

/// Inflates a whole zlib stream into a buffer, reporting the length only on a clean end.
///
/// @param[out] output destination buffer
/// @param[in,out] length expected length on entry, clamped to output.size();
///        updated only when the stream ends cleanly, so a corrupt or overlong
///        stream leaves the caller's value in place
/// @param input compressed bytes
/// @return Z_OK when the stream reached its end and was released cleanly;
///         otherwise the zlib error, Z_BUF_ERROR for a stream that stopped
///         before its end
int uncompress_legacy(
    std::span<uint8_t> output, uint32_t* length, std::span<const uint8_t> input
) noexcept;

/// Validates and decodes one SQSH chunk of an archive entry.
///
/// Checks the marker, a type below four and the 32-bit byte-sum checksum,
/// descrambles the payload when flagged, then decodes LZ77 or zlib. Stored
/// (0) and type-3 chunks never decode. A zlib stream must reach its end and
/// produce exactly the header's unpacked size. A stream whose packing tool
/// left off its four-byte check value still reads when its header is valid,
/// its deflate data ends at the payload's last byte and it produces exactly
/// that size; a wrong or partial check value still fails.
///
/// @param[out] output decoded bytes, at most one 64 KiB block
/// @param[in,out] block the whole chunk, header first; its payload is
///        descrambled in place
/// @return ok, or the SQUASHERR_* condition; the header's unpacked size must fit
///         output, and a packed size past the chunk is a checksum failure
[[nodiscard]] formats::hpi::SquashStatus
unsquash_archive_block(std::span<uint8_t> output, std::span<uint8_t> block) noexcept;

/// Returns the SQUASHERR_* diagnostic name of a squash status.
///
/// @param status squash status
/// @return the name, or null for a value outside the table
[[nodiscard]] const char* squash_status_name(formats::hpi::SquashStatus status) noexcept;

/// Matches a name against a '*'/'?' wildcard pattern, ignoring ASCII case.
///
/// @param name candidate name; a NUL ends it
/// @param pattern wildcard pattern
/// @return true when the whole name matches
/// @quirk At most 100 pattern positions are tracked at once; further '*'
///        branches are dropped.
[[nodiscard]] bool match_wildcard(std::string_view name, std::string_view pattern) noexcept;

struct ArchiveEntry {
    std::string path;
    uint32_t size{};
};

// One record of an archive directory, as kept after the directory block has
// been decrypted and its offsets resolved.
struct ArchiveNode {
    std::string name;
    uint8_t flags{};
    // Files: absolute data offset, decoded size, nonzero when chunked.
    uint32_t data_offset{};
    uint32_t size{};
    uint8_t compression{};
    // Directories: children occupy nodes[first_child, first_child + child_count).
    uint32_t first_child{};
    uint32_t child_count{};

    /// Returns whether the record is a subdirectory.
    [[nodiscard]] bool directory() const noexcept {
        return (flags & formats::hpi::EntryFlagDirectory) != 0;
    }
};

/// The stored bytes of an archive, read at any offset.
///
/// The code that opens an archive supplies one: a host file it opened, or
/// bytes already in memory. HpiArchive calls read_at() from one thread at a
/// time, so a source need not lock.
class ArchiveSource {
  public:

    /// Releases whatever the source holds open.
    virtual ~ArchiveSource() = default;
    /// Returns the archive's size.
    ///
    /// @return the size in bytes
    [[nodiscard]] virtual uint64_t size() const noexcept = 0;
    /// Copies stored bytes from an offset.
    ///
    /// @param offset archive offset of the first byte
    /// @param[out] output receives up to output.size() bytes
    /// @return the count copied; short past the end of the archive or when
    ///         the bytes cannot be read
    virtual std::size_t read_at(uint64_t offset, std::span<uint8_t> output) = 0;
};

/// An archive held in memory.
class ArchiveBuffer final : public ArchiveSource {
  public:

    /// Takes over an archive's bytes.
    ///
    /// @param bytes the whole archive
    explicit ArchiveBuffer(std::vector<uint8_t> bytes) noexcept : bytes_(std::move(bytes)) {}

    /// Returns the archive's size.
    ///
    /// @return the count of bytes held
    [[nodiscard]] uint64_t size() const noexcept override { return bytes_.size(); }

    /// Copies held bytes from an offset.
    ///
    /// @param offset archive offset of the first byte
    /// @param[out] output receives up to output.size() bytes
    /// @return the count copied; short past the end
    std::size_t read_at(uint64_t offset, std::span<uint8_t> output) override;

  private:

    std::vector<uint8_t> bytes_;
};

/// An open HPI archive: its resolved directory and the source its entries are read from.
///
/// Reads may run on several threads at once. Each holds the archive's source
/// only while it copies stored bytes, and decrypts and decompresses them
/// outside it.
class HpiArchive {
  public:

    /// Validates an archive and resolves its decrypted directory block.
    ///
    /// The archive must be HPI version 1 with the copyright trailer; the
    /// directory is resolved into nodes() and the source is kept for reads.
    ///
    /// @param source the archive's stored bytes; null is refused as not_found
    /// @return the archive; or, at its archive offset, bad_signature for a
    ///         missing marker or copyright trailer, unsupported_version for
    ///         a version other than 1, truncated for a directory block the
    ///         source cannot supply, limit_exceeded for a directory over 256
    ///         MiB, more than a million entries or nesting deeper than 128,
    ///         cycle for a directory reached twice, out_of_range for a node,
    ///         list, name or file record outside the directory block, or
    ///         malformed for an unterminated name
    [[nodiscard]] static base::bytes::Decoded<HpiArchive>
    open(std::unique_ptr<ArchiveSource> source);
    /// Validates an archive held in memory, as open() does for any source.
    ///
    /// @param bytes the whole archive
    /// @return the archive, or open()'s error
    [[nodiscard]] static base::bytes::Decoded<HpiArchive> open(std::vector<uint8_t> bytes);
    /// Releases the archive's source.
    ~HpiArchive();

    /// Takes over another archive's source and directory.
    ///
    /// @param other archive left empty
    HpiArchive(HpiArchive&& other) noexcept;
    /// Takes over another archive's source and directory, releasing this one's.
    ///
    /// @param other archive left empty
    /// @return this archive
    HpiArchive& operator=(HpiArchive&& other) noexcept;
    HpiArchive(const HpiArchive&) = delete;
    HpiArchive& operator=(const HpiArchive&) = delete;

    /// Lists every file record in stored depth-first order.
    ///
    /// Records a lookup can never reach (earlier duplicates, children of a
    /// shadowed directory) are included.
    ///
    /// @return '/'-joined paths and decoded sizes
    [[nodiscard]] std::vector<ArchiveEntry> entries() const;
    /// Reads the whole decoded content of a file.
    ///
    /// @param path '\\'- or '/'-separated path inside the archive
    /// @return the decoded bytes; not_found when lookup() finds no file, or
    ///         read_node's error
    [[nodiscard]] base::bytes::Decoded<std::vector<uint8_t>> read(std::string_view path) const;

    /// Returns every directory and file record.
    ///
    /// @return the records; node 0 is a synthetic root directory holding the root records
    [[nodiscard]] std::span<const ArchiveNode> nodes() const noexcept;
    /// Resolves a path through the archive tree.
    ///
    /// @param path '\\'- or '/'-separated path
    /// @return the node index, or nullopt
    /// @quirk Each segment takes the last case-insensitive match in its
    ///        directory; an intermediate match that is a file ends the lookup
    ///        unsuccessfully.
    [[nodiscard]] std::optional<uint32_t> lookup(std::string_view path) const noexcept;
    /// Resolves the directory named by all but the last segment of a path.
    ///
    /// @param path '\\'- or '/'-separated path; its last segment is not looked up
    /// @return the directory's node index, or nullopt
    [[nodiscard]] std::optional<uint32_t> lookup_directory(std::string_view path) const noexcept;
    /// Reads the whole decoded content of a file node.
    ///
    /// The size the entry claims is checked before its buffer is allocated:
    /// it is at most EntryByteLimit (entry_size_allowed), a stored entry
    /// lies wholly inside the archive, and a compressed one has room for each
    /// chunk's size-table slot and header. A compressed entry's buffer is
    /// filled one decoded chunk at a time, so a chunk that fails stops the
    /// read before the rest of the claimed size is used.
    ///
    /// @param index file node index
    /// @return the decoded bytes; or out_of_range for a directory or bad
    ///         index, limit_exceeded for an entry over EntryByteLimit or one
    ///         whose buffer cannot be allocated, truncated, at its archive
    ///         offset, for an entry the archive cannot hold or that reads
    ///         short, or read_node_range's error
    [[nodiscard]] base::bytes::Decoded<std::vector<uint8_t>> read_node(uint32_t index) const;
    /// Copies a range of a file node's decoded bytes.
    ///
    /// Compressed entries are located through the decrypted chunk-size table
    /// and decoded one 64 KiB block at a time.
    ///
    /// @param index file node index
    /// @param position first decoded byte to copy
    /// @param[out] output destination; the range is clamped to the entry
    /// @return the count copied, short when a stored entry runs past the end
    ///         of the archive; or, at its archive offset, truncated for a
    ///         chunk or size table that cannot be read, limit_exceeded for a
    ///         stored chunk over 1 MiB, or malformed for a chunk that fails
    ///         to decode, with its SQUASHERR_* name as the message and its
    ///         SquashStatus as the detail; 3.1c treats that last one as fatal.
    ///         out_of_range for a directory or bad index.
    [[nodiscard]] base::bytes::Decoded<uint32_t>
    read_node_range(uint32_t index, uint32_t position, std::span<uint8_t> output) const;
    /// Reads and decrypts a compressed file node's chunk-size table, for
    /// reads of its ranges that each would otherwise read it again.
    ///
    /// @param index file node index
    /// @return where each chunk is stored; or out_of_range for a directory,
    ///         a stored file or a bad index, or truncated, at the node's data
    ///         offset, for a table past the end of the archive
    [[nodiscard]] base::bytes::Decoded<formats::hpi::ChunkTable>
    read_chunk_table(uint32_t index) const;
    /// Copies a range of a compressed file node's decoded bytes, locating
    /// its chunks through a table read_chunk_table() gave.
    ///
    /// As read_node_range(index, position, output) with the table already read.
    ///
    /// @param index file node index
    /// @param position first decoded byte to copy
    /// @param[out] output destination; the range is clamped to the entry
    /// @param chunks read_chunk_table(index)
    /// @return read_node_range's result; or out_of_range for a table of
    ///         another node
    [[nodiscard]] base::bytes::Decoded<uint32_t> read_node_range(
        uint32_t index,
        uint32_t position,
        std::span<uint8_t> output,
        const formats::hpi::ChunkTable& chunks
    ) const;

  private:

    struct Impl;

    /// Holds a validated archive.
    ///
    /// @param impl the resolved directory and its source
    explicit HpiArchive(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

// Serialises an HPI version-1 archive: header, encrypted directory, entry data
// and the copyright trailer the reader requires.
struct HpiWriteFile {
    std::string path; // '/'-separated; directories are created as needed
    std::vector<uint8_t> bytes;
    uint8_t compression = formats::hpi::CompressionLZ77;
};

struct HpiWriteOptions {
    uint8_t header_key = 0;
    bool scramble_chunks = true;
    std::string_view trailer_year = "1997";
};

/// Serialises files as an HPI version-1 archive.
///
/// Throws std::invalid_argument when the trailer year is not four characters.
///
/// @param files entries in the order their data is written
/// @param options header key, chunk scrambling and trailer year
/// @return the archive bytes
[[nodiscard]] std::vector<uint8_t>
write_hpi(std::span<const HpiWriteFile> files, const HpiWriteOptions& options = {});

struct AssetData {
    std::vector<uint8_t> bytes;
    std::filesystem::path source;
    bool archived = false;
};

// One match of a find over the mutator layer, then loose files, then
// archives, then the pack layer.
struct FoundEntry {
    std::string name;
    bool directory = false;
    uint32_t size = 0;
    // -1 for a loose file. An archive match is its mount index. A pack-layer
    // match uses mount_paths().size(), past every archive, and pack_layer.
    // A mutator-layer match uses formats::hpi::MutatorLayerMount.
    int mount = -1;
    /// True when the match comes from the pack layer. mount is then
    /// mount_paths().size().
    bool pack_layer = false;
    /// True when the match comes from the mutator layer. mount is then
    /// formats::hpi::MutatorLayerMount.
    bool mutator_layer = false;
};

// Where a find enumeration starts: the mutator layer when the search starts
// at the top and include_mutator_layer is set, then loose files (then every
// mount when `continue_into_mounts`), or one mount onward. The pack layer
// follows the mounts when include_pack_layer is set and the search reaches
// it. FindScope{formats::hpi::MutatorLayerMount, false} lists the mutator
// layer alone.
struct FindScope {
    int first_mount = -1;
    bool continue_into_mounts = true;
    /// True to list the pack layer once the search reaches it. A search that
    /// stays inside one archive leaves the pack layer out.
    bool include_pack_layer = true;
    /// True to list the mutator layer when the search starts at the top.
    /// A file the layer serves or hides is still left out of the lower
    /// sources when this is false.
    bool include_mutator_layer = true;
};

/// Whether a pack layer reads a folder or a zip archive.
enum class PackLayerKind : uint8_t {
    folder, ///< files in a directory
    zip,    ///< files in a zip archive
};

/// One file a pack layer shows, and the file inside the pack it is read from.
struct PackLayerFile {
    /// Resource path the store shows, with '/' or '\\' between parts, matched
    /// without ASCII case.
    std::string path;
    /// Path of the file inside the pack, with '/' or '\\' between parts,
    /// matched without ASCII case. Not read when `contents` is given.
    std::string source;
    /// The file's bytes, shown in place of any file of the pack: `source` is
    /// then neither checked nor read. Null reads `source` from the pack.
    std::shared_ptr<const std::vector<uint8_t>> contents{};
};

/// A folder or a zip, the files it shows, and the name lookups report for it.
struct PackLayerSpec {
    /// Folder or zip.
    PackLayerKind kind{PackLayerKind::folder};
    /// Host path of the folder or the zip file.
    std::filesystem::path location;
    /// Files the layer shows, in the order listings report them. At most
    /// formats::hpi::PackLayerFileLimit.
    std::vector<PackLayerFile> files;
    /// Single-line name of the layer, kept in its identity and its errors.
    std::string label;
};

/// Where one path in a mutator layer is read from, or that the path is hidden.
enum class MutatorSourceKind : uint8_t {
    loose,    ///< a loose file of a mutator package
    archive,  ///< a file inside an archive the layer holds
    contents, ///< bytes given in memory
    hidden,   ///< the path is missing from lookups and listings
};

/// One path a mutator layer serves or hides.
///
/// `owner` is the mutator's id: one line, never empty. A loose entry is read
/// from `host`. An archive entry is read from `archives[archive]` at
/// `archive_path`. A contents entry is read from `contents`. A hidden entry
/// uses none of them.
struct MutatorLayerEntry {
    /// Resource path the store shows, with '/' or '\\' between parts.
    std::string path;
    /// Id of the mutator this path comes from.
    std::string owner;
    /// Which source serves the path, or hidden.
    MutatorSourceKind kind{};
    /// Host path of a loose entry.
    std::filesystem::path host;
    /// Index into MutatorLayerSpec::archives for an archive entry.
    std::size_t archive{};
    /// Path of an archive entry inside its archive, with '/' or '\\' between parts.
    std::string archive_path;
    /// Bytes of a contents entry. Null for every other kind.
    std::shared_ptr<const std::vector<uint8_t>> contents{};
};

/// One archive a mutator layer holds open.
struct MutatorLayerArchive {
    /// The archive handed to the layer. Reads use this, not a path reopened later.
    std::shared_ptr<const HpiArchive> archive;
    /// Host path lookups report for a file served from `archive`.
    std::filesystem::path location;
    /// Id of the mutator this archive comes from.
    std::string owner;
};

/// One mask pattern and the mutator it comes from.
struct MutatorMask {
    /// Path pattern, matched without ASCII case and with either slash.
    std::string pattern;
    /// Id of the mutator this mask comes from.
    std::string owner;
};

/// The paths, archives and masks of one mutator layer.
///
/// Entries are listed in the order listings report the served paths.
struct MutatorLayerSpec {
    /// Archives the layer holds, indexed by MutatorLayerEntry::archive.
    std::vector<MutatorLayerArchive> archives;
    /// Served and hidden paths, in the order given.
    std::vector<MutatorLayerEntry> entries;
    /// Masks applied to any path the table does not list. At most
    /// formats::hpi::MutatorLayerPatternLimit.
    std::vector<MutatorMask> masks;
    /// Single-line name of the layer, kept for its errors.
    std::string label;
};

/// Returns why a mask pattern is refused, or empty when it is valid.
///
/// A valid pattern is not empty, holds at most MutatorMaskByteLimit bytes,
/// is relative, holds no ':' or NUL, has no empty, "." or ".." segment, does
/// not start with the backup folder, holds at most MutatorMaskWildcardLimit
/// '*' and '?' together, and uses "**" only as a whole segment.
///
/// @param pattern the pattern
/// @return the reason, or empty when the pattern can be stored
[[nodiscard]] std::string mutator_mask_problem(std::string_view pattern);

/// Returns whether a mask pattern matches a resource path.
///
/// ASCII case is ignored and '\\' matches '/'. '*' matches any run of
/// characters inside one segment, '?' one character inside a segment, and a
/// "**" segment any number of whole segments, none included. A pattern
/// mutator_mask_problem refuses matches nothing. The work is bounded by the
/// sizes of the pattern and the path and by the wildcard limit.
///
/// @param pattern the pattern
/// @param path the resource path
/// @return true when the pattern matches the path
[[nodiscard]] bool mutator_mask_matches(std::string_view pattern, std::string_view path) noexcept;

/// Which part of the store provides a resource.
enum class ProviderKind : uint8_t {
    none,          ///< nothing provides the resource
    loose,         ///< a loose file
    archive,       ///< a mounted archive
    pack_layer,    ///< the pack layer
    mutator_layer, ///< the mutator layer
};

/// Where a resource comes from, and an identity that stays put while it does.
struct Provider {
    /// Which part provides the resource.
    ProviderKind kind{ProviderKind::none};
    /// Host path of the loose file, the archive, the pack folder or zip, a
    /// mutator's loose file, or a mutator archive's location. Empty when the
    /// mutator gave the bytes, or when nothing provides the resource.
    std::filesystem::path path;
    /// "loose:" and the file's path, "archive:" and the archive's path,
    /// "pack:", the layer's label, '#' and its serial, or "mutator:", the
    /// mutator's id, '#' and the layer's serial. Empty when nothing provides
    /// the resource.
    std::string identity;
};

/// Tells whether two archive file names name the same archive.
///
/// Discovery and the folder check compare names this way: ASCII letters
/// without case, and '\\' the same as '/'. Use it wherever an archive's name
/// is matched against one a mod profile lists.
///
/// @param left one file name
/// @param right the other file name
/// @return true when they name one archive
[[nodiscard]] bool same_archive_file_name(std::string_view left, std::string_view right);

// Mount outcome for one archive candidate during discovery.
struct DiscoveredArchive {
    std::filesystem::path path;
    bool mounted = false;
    std::string error;
    // Rejected because its full path was already in the table; such a
    // candidate never counts toward the *.HPI limit.
    bool already_mounted = false;
    // An empty file a group's pattern or the revision archive's name found:
    // it wins its path as any file does, so it hides the archive of its name
    // beneath it, and mounts nothing. It did not fail to open, so `error` is
    // empty. An installation archive the plan names is never hiding: an
    // empty one is reported as failing to open.
    bool hiding = false;
};

/// The archive groups discovery mounts, in mount order: the revision archive,
/// then the ccx and ufo groups, each in Windows NTFS name order, then the
/// installation archives in the order written, then the hpi group in NTFS
/// name order, then the disc. A group's pattern names the archives it mounts,
/// matched without case; a pattern that matches nothing mounts nothing. An
/// empty installation-archive list mounts nothing in that slot, which is the
/// order of a plan that does not name any.
struct DiscoveryPlan {
    /// File name of the revision archive, matched without case.
    std::string revision_archive{"rev31.GP3"};
    /// Pattern of the group mounted after the revision archive.
    std::string ccx_pattern{"*.CCX"};
    /// Pattern of the group mounted after the ccx group.
    std::string ufo_pattern{"*.UFO"};
    /// File names of archives from the installation, mounted after the ufo
    /// group and before the hpi group, in this order, matched as
    /// same_archive_file_name matches them. Each is sought in the folders,
    /// then on the removable roots. A listed name is left out of every group,
    /// including the revision slot when the revision archive is listed and the
    /// disc scan. Empty mounts nothing extra.
    std::vector<std::string> installation_archives{};
    /// Pattern of the main group, of which at most hpi_limit mount.
    std::string hpi_pattern{"*.HPI"};
    /// Most archives of the hpi group mounted; a candidate that fails to open
    /// does not count.
    int hpi_limit{formats::hpi::PlainArchiveMountLimit};
    /// Pattern of the archives mounted from each removable root, without limit.
    std::string disc_pattern{"*.hpi"};
    /// The store's own folders serve as the disc root after the removable
    /// roots, as for an install without discs: archives of disc_pattern there
    /// that did not mount yet mount last.
    bool folders_as_disc{};
};

/// Watches the resources and listings a store is asked for.
struct LookupObserver {
    void* context{};
    /// Receives each resource path or listing a lookup names, as the caller
    /// wrote it, from the thread that looks it up; null watches nothing.
    void (*looked_up)(void* context, std::string_view name){};
    /// Receives each path the mutator layer serves or hides, from the thread
    /// that looks it up. `hidden` is false when the layer served the path,
    /// and true when it hid the path or a listing left the path out. `owner`
    /// is the mutator of the entry or the mask. Null watches nothing.
    void (*mutator_layer_used)(
        void* context, std::string_view path, std::string_view owner, bool hidden
    ){};
};

/// The folder at the top of a layered folder that the store never reads: no
/// lookup, listing, search or archive discovery reaches it, whatever the
/// case of its letters. A mod folder keeps there the version an update
/// replaced, for one step back.
inline constexpr std::string_view backup_folder_name = ".backup";

// Open-file handle over a loose file or an archive entry. Its reads and seeks
// keep 3.1c's position rules, and it caches one decoded block.
struct ResourceFile;

// A lookup answers from the mutator layer, then loose folders, then archives
// in mount order, then the pack layer. Callers supply the archive order
// explicitly (or use discover()): platform directory enumeration order is
// not a portable replacement for the Windows order the game lists files in.
//
// Above the loose files the store holds at most one mutator layer: a table
// from resource path to a loose file, a file in an archive the layer holds,
// or bytes given in memory, or to hidden, plus mask patterns. A path the
// table lists is that entry. Any other path a mask matches is missing. A
// hidden entry or a matching mask hides the file from lookups and from
// listings. Masks and hidden entries hide files, not folders: a folder still
// lists while a lower source holds it. A table entry wins over every mask.
// The layer is not a mount: mount_paths() and mounted() never show it, and
// mounting, replacing or unmounting it touches neither the loose index nor
// an archive's marks nor the pack layer.
//
// The loose files may come from several folders layered as if each were
// copied over the next: a path, compared without case, resolves from the
// first folder that holds it; listings and discovery merge the folders by
// name, the earlier folder's file winning and the later folder's spelling
// kept, as a copy over an existing file keeps its name. A folder named
// backup_folder_name at the top of any of them is passed over.
//
// Below every archive the store holds at most one pack layer: a folder or a
// zip, restricted to a list of files, each shown at a path the caller gives.
// Lookups reach it only after the mutator layer, the loose files and every
// archive, so mounting it changes nothing a lookup above it returns. A
// second mount is refused and leaves the store as it was. Mounting touches
// neither the loose index nor the archives or their marks. Unmounting drops
// the layer and returns every lookup to exactly what it returned before the
// mount. The layer is not a mount: mount_paths() and mounted() never show it.
//
// Reads on other threads stay safe while one thread mounts, replaces or
// unmounts a layer. A lookup copies that layer under its lock and reads the
// copy, and never holds the lock across a read. One lookup takes one copy.
// A handle opened on a layer keeps that copy until the handle is closed, so
// unmounting does not free what the handle still reads.
class AssetStore {
  public:

    /// Creates a store over a game directory with no archives mounted.
    ///
    /// Loose-file lookups list each folder below the game directory once, the
    /// first time a lookup passes through it, and answer later lookups from
    /// those listings: a file added, removed or renamed afterwards is seen only
    /// after mark_loose_shadows() or discover(), which drop the listings. Once
    /// the listings would hold more than loose_index_limit entries, the store
    /// drops them and lists every folder on every lookup instead. Where a
    /// file found leads is checked as its path is at the lookup, not as it
    /// was listed: a file reached through a link, a junction or another
    /// reparse point that leads outside the game directory is not a loose
    /// file.
    ///
    /// @param loose_root game directory; loose files are resolved below it
    /// @param loose_index_limit most folder entries the kept listings may hold
    explicit AssetStore(
        std::filesystem::path loose_root,
        std::size_t loose_index_limit = formats::hpi::LooseIndexEntryLimit
    );
    /// Creates a store over folders layered in order, the first winning, with
    /// no archives mounted.
    ///
    /// With one folder it is the store over that game directory.
    ///
    /// @param loose_roots the folders, highest precedence first; at least one
    /// @param loose_index_limit most folder entries the kept listings may hold
    explicit AssetStore(
        std::vector<std::filesystem::path> loose_roots,
        std::size_t loose_index_limit = formats::hpi::LooseIndexEntryLimit
    );
    /// Unmounts every archive and drops the mutator layer and the pack layer.
    ///
    /// A handle opened while a layer was mounted keeps that layer until the
    /// handle is closed.
    ~AssetStore();
    /// Takes over another store's root and mounts.
    ///
    /// @param other store left empty
    AssetStore(AssetStore&& other) noexcept;
    /// Takes over another store's root and mounts, dropping this one's.
    ///
    /// @param other store left empty
    /// @return this store
    AssetStore& operator=(AssetStore&& other) noexcept;
    AssetStore(const AssetStore&) = delete;
    AssetStore& operator=(const AssetStore&) = delete;

    /// Mounts one archive after the existing mounts.
    ///
    /// A path already mounted (case-insensitive full path) is ignored. Throws
    /// when the archive cannot be opened or is invalid.
    ///
    /// @param archive host path of the archive
    void mount(const std::filesystem::path& archive);
    /// Mounts one archive, reporting failure instead of throwing.
    ///
    /// @param archive host path of the archive
    /// @param[out] error receives the reason when not mounted, if not null
    /// @return true when mounted; false for an invalid or already-mounted path
    bool try_mount(const std::filesystem::path& archive, std::string* error = nullptr);
    /// Scans the game directory and removable roots for archives, as the game does.
    ///
    /// Mounts that no longer open are dropped first. Then rev<version>.GP3,
    /// *.CCX, *.UFO and at most ten *.HPI in the game directory, then *.hpi on
    /// each removable root, each group in Windows NTFS name order. The mount
    /// order is the lookup precedence after loose files, so a path several
    /// archives provide resolves from the earliest mount. Candidates that fail
    /// to open are reported and do not count toward the *.HPI limit; a
    /// directory matching a pattern is such a candidate. Loose shadows are
    /// recomputed afterwards.
    ///
    /// @param version game revision in the GP3 name, e.g. "31" for rev31.gp3
    /// @param removable_roots roots of removable drives searched last
    /// @return one outcome per candidate, in scan order
    std::vector<DiscoveredArchive>
    discover(std::string_view version, std::span<const std::filesystem::path> removable_roots = {});
    /// Scans the store's folders and removable roots for archives as a plan says.
    ///
    /// As discover(version, removable_roots), with the plan's revision
    /// archive, group patterns, installation archives and hpi limit. Each
    /// group's matches in every folder of the store are merged by name, the
    /// earlier folder's file winning, and mount in NTFS name order, so
    /// layered folders mount what one folder holding the same files would.
    /// Each installation archive is sought by file name in that merged
    /// listing, then on each removable root in order, and mounted in the
    /// list's order; one that none of them holds is reported with the error
    /// "installation archive 'NAME' is missing" and is not mounted. An empty
    /// list leaves the groups in their usual order.
    ///
    /// @param plan the archive names and patterns
    /// @param removable_roots roots of removable drives searched after the folders' groups
    /// @return one outcome per candidate, in scan order
    std::vector<DiscoveredArchive> discover(
        const DiscoveryPlan& plan, std::span<const std::filesystem::path> removable_roots = {}
    );
    /// Returns the folders loose files come from.
    ///
    /// @return absolute paths, highest precedence first
    [[nodiscard]] std::span<const std::filesystem::path> loose_roots() const noexcept;
    /// Finds the folder a loose file of a path comes from, and its host path.
    ///
    /// This is the loose folders' answer. The mutator layer and the pack
    /// layer are not consulted.
    ///
    /// @param resource '\\'- or '/'-separated path, matched ignoring ASCII case
    /// @return the host path of the loose file, or nullopt when no folder holds it
    [[nodiscard]] std::optional<std::filesystem::path> loose_file(std::string_view resource) const;
    /// Sets who watches the lookups; a default observer watches nothing.
    ///
    /// @param observer the observer
    void observe_lookups(LookupObserver observer) noexcept;
    /// Recomputes which archive files, and which pack-layer files, a loose
    /// file of the same path hides from find().
    ///
    /// discover() does this after mounting. The loose index is dropped and
    /// the archives' marks are replaced. A mounted pack layer is published
    /// again with its own marks recomputed; that does not change an archive's
    /// marks beyond the replacement this already performs.
    void mark_loose_shadows();
    /// Mounts one pack layer under every archive.
    ///
    /// Refuses, leaving the store unchanged, when a layer is already mounted,
    /// the list is empty or longer than formats::hpi::PackLayerFileLimit, the
    /// label is empty or holds more than one line, a path would be refused by
    /// a loose lookup or starts with the backup folder, two paths are alike
    /// ignoring ASCII case, a file in the folder or the zip cannot be taken,
    /// or a file given as its contents is past the entry size limit. A file
    /// given as its contents reads as those bytes, as the layer's other
    /// files read: from an archive when the layer is a zip. Mounting does not
    /// reset the loose index and does not touch the archives or their marks.
    /// A file a loose file already provides is hidden from find(), as an
    /// archive file is.
    ///
    /// @param spec the folder or zip, the files it shows and its label
    /// @param[out] error receives the reason when the layer is not mounted, if not null
    /// @return true when the layer is mounted
    bool mount_pack_layer(PackLayerSpec spec, std::string* error = nullptr);
    /// Drops the pack layer.
    ///
    /// A handle opened while it was mounted keeps the layer until the handle
    /// is closed. Lookups made afterwards match the store from before the mount.
    ///
    /// @return true when a layer was mounted
    bool unmount_pack_layer() noexcept;
    /// Returns whether a pack layer is mounted.
    ///
    /// @return true when a layer is mounted
    [[nodiscard]] bool pack_layer_mounted() const;
    /// Returns the mounted layer's label.
    ///
    /// @return the label, or nullopt when no layer is mounted
    [[nodiscard]] std::optional<std::string> pack_layer_label() const;
    /// Returns the resource paths the mounted layer shows, in the order given.
    ///
    /// @return the paths, or an empty list when no layer is mounted
    [[nodiscard]] std::vector<std::string> pack_layer_paths() const;
    /// Mounts one mutator layer above the loose folders.
    ///
    /// Refuses, leaving the store unchanged, when a layer is already mounted,
    /// the entries, masks or archives pass their limits, the label is empty
    /// or holds a line break, an owner is empty or holds a line break, a path
    /// would be refused by a pack path, two paths are alike ignoring ASCII
    /// case and slash, a mask is not a valid pattern, a loose host is not a
    /// regular file or is past the entry size limit, an archive entry's index
    /// is out of range or its archive is null or does not hold the path as a
    /// file, a contents entry's bytes are null or past the entry size limit,
    /// or the given bytes together pass the content limit. The error names
    /// the entry's path and owner. Mounting touches neither the loose index,
    /// nor an archive's marks, nor the pack layer, nor mount_paths().
    ///
    /// @param spec the archives, paths, masks and label
    /// @param[out] error receives the reason when the layer is not mounted, if not null
    /// @return true when the layer is mounted
    bool mount_mutator_layer(MutatorLayerSpec spec, std::string* error = nullptr);
    /// Replaces the mutator layer, or mounts one when none is mounted.
    ///
    /// The new layer is published in one step, so no lookup sees the store
    /// without a layer between the old one and the new one. A refusal leaves
    /// the mounted layer in place and reports the same errors as
    /// mount_mutator_layer.
    ///
    /// @param spec the archives, paths, masks and label
    /// @param[out] error receives the reason when the layer is not published, if not null
    /// @return true when the new layer is published
    bool replace_mutator_layer(MutatorLayerSpec spec, std::string* error = nullptr);
    /// Drops the mutator layer.
    ///
    /// A handle opened while it was mounted keeps the layer until the handle
    /// is closed. Lookups made afterwards match the store from before the mount.
    ///
    /// @return true when a layer was mounted
    bool unmount_mutator_layer() noexcept;
    /// Returns whether a mutator layer is mounted.
    ///
    /// @return true when a layer is mounted
    [[nodiscard]] bool mutator_layer_mounted() const;
    /// Returns the mounted mutator layer's label.
    ///
    /// @return the label, or nullopt when no layer is mounted
    [[nodiscard]] std::optional<std::string> mutator_layer_label() const;
    /// Returns the served paths of the mounted mutator layer, in the order given.
    ///
    /// Hidden entries are not listed.
    ///
    /// @return the paths, or an empty list when no layer is mounted
    [[nodiscard]] std::vector<std::string> mutator_layer_paths() const;
    /// Returns what provides a resource: the mutator layer, else a loose
    /// file, else the earliest archive, else the pack layer.
    ///
    /// A path the mutator layer hides has an empty provider. Throws
    /// std::runtime_error for a path a loose lookup would refuse.
    ///
    /// @param resource '\\'- or '/'-separated path, matched ignoring ASCII case
    /// @return the provider, or an empty provider when nothing provides it
    [[nodiscard]] Provider provider(std::string_view resource) const;
    /// Returns whether the mutator layer, a loose file or an archive provides a resource.
    ///
    /// The pack layer does not count, so this is false when only the pack
    /// layer holds the path. A path the mutator layer hides is false.
    ///
    /// @param resource '\\'- or '/'-separated path, matched ignoring ASCII case
    /// @return true when the mutator layer, a loose file or an archive provides it
    [[nodiscard]] bool provided_above_pack_layer(std::string_view resource) const;
    /// Reads the pack layer's own copy of a resource, whatever is above it.
    ///
    /// This is the pack layer's own copy. The mutator layer is not consulted,
    /// so a path the mutator layer serves or hides still reads here when the
    /// pack lists it. A failing read throws std::runtime_error naming the
    /// path, the layer's label and the reason.
    ///
    /// @param resource '\\'- or '/'-separated path, matched ignoring ASCII case
    /// @return the bytes, or nullopt when no layer is mounted or it does not
    ///         list the path
    [[nodiscard]] std::optional<std::vector<uint8_t>>
    read_pack_layer(std::string_view resource) const;
    /// Returns the mounted archive paths.
    ///
    /// @return canonical paths in lookup order
    [[nodiscard]] std::span<const std::filesystem::path> mount_paths() const noexcept;
    /// Returns whether loose-file lookups answer from kept folder listings.
    ///
    /// @return false once the listings would have held more entries than the store's limit
    [[nodiscard]] bool loose_index_enabled() const noexcept;
    /// Returns one mounted archive.
    ///
    /// Throws std::out_of_range past the mounts.
    ///
    /// @param index mount index, in lookup order
    /// @return the archive
    [[nodiscard]] const HpiArchive& mounted(std::size_t index) const;

    /// Reads a whole resource: the mutator layer, else a loose file, else the
    /// first archive holding it as a file, else the pack layer.
    ///
    /// A path the mutator layer hides is missing. A file served from one of
    /// the layer's archives, or from bytes given in memory, is reported as
    /// archived; a mutator's loose file is not. Throws std::runtime_error
    /// for an invalid or traversing path, an ambiguous loose case collision,
    /// or a resource nothing provides.
    ///
    /// @param resource '\\'- or '/'-separated path, matched ignoring ASCII case
    /// @return the bytes, the providing path and whether it came from an archive
    [[nodiscard]] AssetData read(std::string_view resource) const;
    /// Reads a resource as though one archive were not mounted.
    ///
    /// The mutator layer is not an archive, so it still answers: a path it
    /// serves is read from it, and a path it hides is missing.
    ///
    /// @param resource '\\'- or '/'-separated path
    /// @param archive mount path skipped; the mutator layer, loose files and
    ///        every other mount are searched in order
    /// @return the bytes and their source
    [[nodiscard]] AssetData
    read_without(std::string_view resource, const std::filesystem::path& archive) const;
    /// Returns the archive read() would take a resource from.
    ///
    /// Answers for mounted archives only: nullopt when the mutator layer
    /// serves or hides the path, when a loose file provides it, when only
    /// the pack layer provides it, or when nothing does.
    ///
    /// @param resource '\\'- or '/'-separated path
    /// @return the mount path, or nullopt when the mutator layer serves or
    ///         hides it, a loose file provides it, only the pack layer
    ///         provides it, or nothing does
    [[nodiscard]] std::optional<std::filesystem::path>
    providing_archive(std::string_view resource) const;
    /// Loads a whole resource by reading it through a handle, so it holds what a handle read gives.
    ///
    /// The handle is the one open() returns, so the mutator layer answers first.
    ///
    /// @param resource '\\'- or '/'-separated path
    /// @return the bytes, or nullopt when the resource is absent, empty,
    ///         larger than EntryByteLimit or yields no bytes
    /// @quirk A short read keeps the full length; the unread tail is zero.
    [[nodiscard]] std::optional<std::vector<uint8_t>>
    load_file_contents(std::string_view resource) const;
    /// Loads a whole resource in ten equal reads plus the remainder, reporting progress.
    ///
    /// The handle is the one open() returns, so the mutator layer answers
    /// first. Throws std::runtime_error when the resource is absent or larger
    /// than EntryByteLimit, before anything is allocated.
    ///
    /// @param resource '\\'- or '/'-separated path
    /// @param progress called after each of the ten reads with 9, 18, ... 90; may be null
    /// @param user value passed to progress
    /// @return the bytes
    [[nodiscard]] std::vector<uint8_t> load_with_progress(
        std::string_view resource, void (*progress)(void* user, uint8_t percent), void* user
    ) const;
    /// Returns a resource's size without reading its content.
    ///
    /// The size is taken through open(), so the mutator layer answers first
    /// and a path it hides has size zero.
    ///
    /// @param resource '\\'- or '/'-separated path
    /// @return the size in bytes, or zero when absent
    [[nodiscard]] uint32_t file_size(std::string_view resource) const;
    /// Reads part of a resource.
    ///
    /// The bytes are taken through open(), so the mutator layer answers first.
    ///
    /// @param resource '\\'- or '/'-separated path
    /// @param position first byte to read
    /// @param[out] output receives up to output.size() bytes
    /// @return false when the resource is absent or nothing was read
    [[nodiscard]] bool
    read_chunk(std::string_view resource, uint32_t position, std::span<uint8_t> output) const;

    /// Enumerates a directory in the order the game lists it.
    ///
    /// The mutator layer's own files and folders come first when the scope
    /// includes it, with mutator_layer set and mount formats::hpi::MutatorLayerMount.
    /// Loose matches follow in NTFS name order, led by "." and "..", then
    /// each mount's records in stored order, then the pack layer's own files
    /// and folders when the scope includes it. A file whose full path the
    /// mutator layer serves or hides is left out of every lower source,
    /// whatever the scope. Archive records and pack-layer files shadowed by
    /// a loose file are skipped; duplicates across archives are all reported.
    /// A pack-layer file an archive also holds is reported as well. Folders
    /// are not hidden: a folder still lists while a lower source holds it.
    ///
    /// @param pattern "dir\\*.ext"-style pattern; "*.*" matches everything
    /// @param scope where the enumeration starts and whether it continues into mounts
    /// @return every match with its source
    [[nodiscard]] std::vector<FoundEntry>
    find(std::string_view pattern, FindScope scope = {}) const;
    /// Collects files under a directory that match a pattern, recursively.
    ///
    /// A folder the mutator layer reported is rescanned in that layer. A
    /// folder a lower source reported is rescanned with the mutator layer's
    /// own listing left out, so a file is listed once. A file the layer hides
    /// is absent. Each other subdirectory is rescanned only in the source
    /// that reported it.
    ///
    /// @param directory directory scanned; empty yields nothing
    /// @param pattern wildcard matched against file names
    /// @param scope where the top-level enumeration starts
    /// @return '\\'-joined paths in enumeration order
    [[nodiscard]] std::vector<std::string> scan_recursive(
        std::string_view directory, std::string_view pattern, FindScope scope = {}
    ) const;
    /// Counts the entries of a find other than "." and "..".
    ///
    /// A file the mutator layer serves or hides is counted only as the layer
    /// lists it, so a hidden file is not counted.
    ///
    /// @param pattern "dir\\*.ext"-style pattern
    /// @param directories_only true to count only directories
    /// @return the count
    [[nodiscard]] int count_entries(std::string_view pattern, bool directories_only) const;

    /// Lists the effective resource names in a directory, sorted.
    ///
    /// The names are list_effective_in_mount_order()'s, sorted. That list
    /// includes the mutator layer's served paths and leaves out every path
    /// the layer serves or hides. Names are ASCII case-folded. This is not
    /// the order the game lists a directory in.
    ///
    /// @param directory '/'-separated directory, relative
    /// @param extension required suffix, or empty for any
    /// @param include_pack_layer true to include the pack layer's files
    /// @return the names, each once
    [[nodiscard]] std::vector<std::string> list_effective(
        std::string_view directory, std::string_view extension, bool include_pack_layer = true
    ) const;
    /// Lists the effective resource names in a directory in mount order.
    ///
    /// The mutator layer's served paths come first, in the order given. Loose
    /// files follow in host directory order, then mounts in insertion order
    /// with archive entries in stored order, then the pack layer's files; the
    /// first occurrence of a path wins. A path the layer serves or hides is
    /// left out of the lower sources. Host loose-file order cannot reproduce
    /// the order Windows lists loose files in.
    ///
    /// @param directory '/'-separated directory, relative
    /// @param extension required suffix, or empty for any
    /// @param include_pack_layer true to include the pack layer's files
    /// @return the names, ASCII case-folded, each once
    [[nodiscard]] std::vector<std::string> list_effective_in_mount_order(
        std::string_view directory, std::string_view extension, bool include_pack_layer = true
    ) const;
    /// Lists the effective resource names under a directory, recursively, in mount order.
    ///
    /// Feature loading relies on this order: the first document containing a
    /// requested section wins. The mutator layer's served paths come first
    /// and the pack layer's files come last. A path the layer serves or hides
    /// is left out of the lower sources.
    ///
    /// @param directory '/'-separated directory, relative
    /// @param extension required suffix, or empty for any
    /// @param include_pack_layer true to include the pack layer's files
    /// @return the names, ASCII case-folded, each once
    [[nodiscard]] std::vector<std::string> list_effective_recursive(
        std::string_view directory, std::string_view extension, bool include_pack_layer = true
    ) const;

    /// Opens the mutator layer's copy of a resource, else a loose file, else
    /// the first archive holding that path as a file, else the pack layer.
    ///
    /// A path the mutator layer hides returns nullptr. A handle on one of the
    /// layer's archive entries keeps the layer until close(). Given bytes are
    /// read from memory. A loose host file is read as a pack folder's file is.
    ///
    /// @param resource '\\'- or '/'-separated path
    /// @return a handle to release with close(), or nullptr when the resource is absent
    [[nodiscard]] ResourceFile* open(std::string_view resource) const;
    /// Releases a handle and its cached block.
    ///
    /// @param file handle from open(), or null
    static void close(ResourceFile* file) noexcept;
    /// Returns whether a handle reads bytes that came from an archive.
    ///
    /// True for a mounted archive, a file in one of the mutator layer's
    /// archives, bytes the mutator layer was given, or a file read from a zip
    /// pack layer. False for a loose file and for a mutator's loose file.
    ///
    /// @param file open handle
    /// @return true when the bytes came from an archive or were given to the mutator layer
    [[nodiscard]] static bool archived(const ResourceFile* file) noexcept;
    /// Moves a handle's read position.
    ///
    /// Crossing into another 64 KiB block drops an archive handle's cached block.
    ///
    /// @param[in,out] file open handle
    /// @param position new position, or 0xFFFFFFFF for the end
    /// @return the new position for a loose file, 0 for an archive entry or a
    ///         zip pack file, or -1 on failure
    static int32_t seek(ResourceFile* file, uint32_t position);
    /// Returns a handle's read position.
    ///
    /// @param file open handle
    /// @return the position in bytes
    [[nodiscard]] static int32_t tell(const ResourceFile* file);
    /// Returns the length of an open resource.
    ///
    /// @param file open handle
    /// @return the length in bytes
    [[nodiscard]] static uint32_t length(const ResourceFile* file);
    /// Reads from a handle's position, decoding at most one cached block at a time.
    ///
    /// @param[in,out] file open handle; its position advances by the count read
    /// @param[out] output receives up to output.size() bytes
    /// @return the count read, 0 at the end, or -1 when a chunk cannot be read
    static int32_t read(ResourceFile* file, std::span<uint8_t> output);

  private:

    // ResourceFile names the private layer types it keeps open.
    friend struct ResourceFile;
    // PackLayerOps builds and reads the private pack-layer type.
    friend struct PackLayerOps;
    // MutatorLayerOps builds and reads the private mutator-layer type.
    friend struct MutatorLayerOps;

    /// Lists resource names under a directory with read()'s precedence.
    ///
    /// The mutator layer's served paths come first, in the order given. A
    /// path the layer serves or hides is left out of the lower sources.
    /// Throws std::runtime_error for an invalid or traversing path or a loose
    /// case collision.
    ///
    /// @param directory '/'-separated directory, relative
    /// @param extension required suffix, or empty for any
    /// @param recursive true to include subdirectories
    /// @param include_pack_layer true to include the pack layer's files
    /// @return the names, the mutator layer first, each once
    [[nodiscard]] std::vector<std::string> list_resources(
        std::string_view directory,
        std::string_view extension,
        bool recursive,
        bool include_pack_layer
    ) const;
    struct PackLayer;
    struct PackLayerSlot;
    struct MutatorLayer;
    struct MutatorLayerSlot;
    /// Copies the mounted mutator layer.
    ///
    /// The copy is taken under the layer's lock, which is not held across a
    /// read. Empty when no layer is mounted, including a store that was moved
    /// from.
    ///
    /// @return the mounted layer, or empty
    [[nodiscard]] std::shared_ptr<const MutatorLayer> mutator_layer_snapshot() const;
    /// Copies the mounted pack layer.
    ///
    /// The copy is taken under the layer's lock, which is not held across a
    /// read. Empty when no layer is mounted, including a store that was moved
    /// from.
    ///
    /// @return the mounted layer, or empty
    [[nodiscard]] std::shared_ptr<const PackLayer> pack_layer_snapshot() const;
    /// Resolves a resource to a loose file, ignoring ASCII case.
    ///
    /// Throws std::runtime_error for an absolute, drive-qualified, traversing
    /// or overlong path, or an ambiguous case collision.
    ///
    /// @param resource '\\'- or '/'-separated path
    /// @return the host path, or nullopt when no loose file exists
    [[nodiscard]] std::optional<std::filesystem::path> loose_path(std::string_view resource) const;
    /// Resolves a folded resource path to a loose file in one folder.
    ///
    /// @param root index of the folder in loose_roots_
    /// @param key resource path, '/'-separated and ASCII lower case
    /// @return the host path, or nullopt when the folder holds no such file
    [[nodiscard]] std::optional<std::filesystem::path>
    loose_path_in(std::size_t root, const std::string& key) const;
    /// Resolves a folded resource path in one folder by listing every folder on the way.
    ///
    /// Throws std::runtime_error for a traversing path or an ambiguous case collision.
    ///
    /// @param root index of the folder in loose_roots_
    /// @param key resource path, '/'-separated and ASCII lower case
    /// @return the host path, or nullopt when the folder holds no such file
    [[nodiscard]] std::optional<std::filesystem::path>
    loose_path_listed_in(std::size_t root, const std::string& key) const;
    /// Tells the observer of a lookup.
    ///
    /// @param name the resource or listing looked up
    void note_lookup(std::string_view name) const;

    struct ArchivedNode {
        std::size_t mount{};
        uint32_t node{};
    };

    /// Finds the first mount holding a resource as a file.
    ///
    /// A directory of that name sends the search on to the next mount.
    ///
    /// @param resource '\\'- or '/'-separated path
    /// @param skipped mount path to pass over, or null
    /// @return the mount and node, or nullopt
    [[nodiscard]] std::optional<ArchivedNode>
    archived_node(std::string_view resource, const std::filesystem::path* skipped) const;
    /// Reads a resource from a loose file or the mounts, optionally skipping one mount.
    ///
    /// @param resource '\\'- or '/'-separated path
    /// @param skipped mount path to pass over, or null
    /// @return the bytes and their source; throws when nothing provides it
    [[nodiscard]] AssetData
    read_skipping(std::string_view resource, const std::filesystem::path* skipped) const;
    /// Tests whether an archive path is already mounted.
    ///
    /// @param archive host path compared by case-insensitive canonical full path
    /// @return true when mounted
    [[nodiscard]] bool is_mounted(const std::filesystem::path& archive) const;
    /// Drops mounts whose archive file can no longer be opened.
    void drop_vanished_mounts();
    /// Walks the loose tree, marking each archive file a loose file hides.
    ///
    /// A folder is walked once however many links lead to it, and a folder
    /// or a file a link leads to outside the root the walk started from is
    /// passed over.
    ///
    /// @param prefix resource path of directory, with a trailing '\\'
    /// @param directory host directory walked
    void mark_loose_directory(const std::string& prefix, const std::filesystem::path& directory);
    /// Continues an enumeration through the archive records of each mount,
    /// then the pack layer when the scope reaches it.
    ///
    /// A file whose full path `mutator` serves or hides is left out. The
    /// pointer is the one copy the lookup took; null when no layer is mounted.
    /// A hidden path is reported through the observer once, recorded in
    /// `hidden_noted`.
    ///
    /// @param[in,out] found matches appended in mount and stored order
    /// @param directory directory part of the pattern, with its trailing separator
    /// @param spec wildcard for the names
    /// @param scope first mount searched and whether later mounts follow
    /// @param mutator the mutator layer copied for this lookup, or null
    /// @param[in,out] hidden_noted hidden paths already reported for this lookup
    void find_in_mounts(
        std::vector<FoundEntry>& found,
        std::string_view directory,
        std::string_view spec,
        FindScope scope,
        const MutatorLayer* mutator,
        std::unordered_set<std::string>& hidden_noted
    ) const;

    struct Mount {
        std::filesystem::path path;
        HpiArchive archive;
        // Per-node mount-time marks (loose shadowing), indexed like nodes().
        std::vector<uint8_t> marks;
    };

    // The folders loose files come from, highest precedence first.
    std::vector<std::filesystem::path> loose_roots_;
    LookupObserver observer_{};
    std::vector<Mount> mounts_;
    std::vector<std::filesystem::path> mount_paths_;
    // Folder listings kept by loose-file lookups; null in a moved-from store.
    struct LooseIndex;
    std::unique_ptr<LooseIndex> loose_index_;
    // The pack layer and the lock that guards its pointer. Null in a
    // moved-from store. The layer itself never changes once published.
    std::unique_ptr<PackLayerSlot> pack_layer_;
    // The mutator layer and the lock that guards its pointer. Null in a
    // moved-from store. The layer itself never changes once published.
    std::unique_ptr<MutatorLayerSlot> mutator_layer_;
    // The root mark_loose_shadows walks, and the folders it has walked, each
    // by its path with every link followed.
    std::filesystem::path walk_root_;
    std::set<std::filesystem::path> walked_folders_;
};

/// Opens an archive file and validates it.
///
/// The file stays open while the archive lives, for reads of its entries.
///
/// @param path host path of the archive
/// @return the archive; not_found when the file cannot be opened or its size
///         read, or HpiArchive::open()'s error
[[nodiscard]] base::bytes::Decoded<HpiArchive> open_hpi_file(const std::filesystem::path& path);

/// Writes a loose file, replacing any previous content.
///
/// @param path host path of the file
/// @param bytes content written
/// @return the count written, 0 when the write fails, or -1 when the file cannot be opened
int32_t write_loose_file(const std::filesystem::path& path, std::span<const uint8_t> bytes);
/// Creates each directory along a '\\'- or '/'-separated path, ignoring failures.
///
/// @param path host directory path; the last component is created too
void create_directory_path(const std::filesystem::path& path);

inline constexpr std::size_t palette_color_count = 256;
inline constexpr std::size_t palette_entry_bytes = 4;
using PaletteBytes = std::array<uint8_t, palette_color_count * palette_entry_bytes>;
using PaletteMap = std::array<uint8_t, palette_color_count>;

struct Image {
    uint32_t width{};
    uint32_t height{};
    std::vector<uint8_t> rgb;
    // Indexed source palette, expanded RGB+zero fourth byte for the game's GUI
    // color remapping. Absent for three-plane true-color images.
    std::optional<PaletteBytes> palette{};
    // 8-bit source indices for paletted PCX (empty for true-color images).
    std::vector<uint8_t> indices{};
};

/// Maps each source colour to the nearest destination colour.
///
/// Nearest means the smallest summed absolute channel difference; the first
/// index wins ties and byte 3 of each entry is ignored. A thread asking for
/// the pair it asked for last gets the map it was given then.
///
/// @param source palette whose entries are mapped
/// @param destination palette searched
/// @return the destination index for each source index
[[nodiscard]] PaletteMap remap_palette(const PaletteBytes& source, const PaletteBytes& destination);

/// Decodes an 8-bit indexed or 24-bit three-plane PCX image.
///
/// @param data the whole file
/// @return RGB pixels, plus indices and the palette for an indexed image; or
///         the first error at its file offset: a short header, a bad
///         manufacturer byte or version, an unsupported layout, an image
///         over the 64-megapixel limit, a missing 256-colour palette, pixel
///         data that ends early, or an invalid or scanline-crossing run;
///         bytes between the last row and the palette are ignored, as the
///         game ignores them
[[nodiscard]] base::bytes::Decoded<Image> decode_pcx(std::span<const uint8_t> data);

} // namespace oa
