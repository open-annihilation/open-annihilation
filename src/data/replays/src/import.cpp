// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Copies a replay opened from outside into Recordings/Imported. Sector 6 is
// left out of the bytes that are written; every other byte of a .tad stays
// as it was. A .oademo is recognised, then read, and a refusal keeps that
// read's sentence.

#include "oa/data/replays/import.hpp"

#include "oa/data/replays/interface_words.hpp"
#include "oa/data/replays/replays.hpp"

#include "oa/base/sha256.hpp"
#include "oa/formats/json.hpp"
#include "oa/formats/oademo/interface_words.hpp"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/tad.hpp"
#include "oa/platform/files.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::data::replays {

namespace {

namespace fs = std::filesystem;
namespace oademo = oa::formats::oademo;
namespace tad = oa::formats::tad;

/// Bytes read from a file at a time.
constexpr std::size_t read_chunk_bytes = 1U << 20U;

/// How many names are tried when the one asked for is taken.
constexpr int most_names = 10000;

/// Sector 6, which a replay this game writes never holds.
constexpr uint32_t sector_left_out = static_cast<uint32_t>(tad::SectorType::player_address);

/// How reading a whole file went.
enum class Loaded : uint8_t { read, unreadable, too_large };

/// One chunk of a TA Demo recording: the length field and the bytes after it.
struct Chunk {
    std::size_t at{};                ///< where the length field starts
    uint16_t total{};                ///< the length field's value, itself included
    std::span<const uint8_t> data{}; ///< the bytes after the length field
};

/// Reads a little-endian u16.
///
/// @param bytes the bytes
/// @param at where it starts
/// @param[out] value the value
/// @return false when the bytes end first
bool read_u16(std::span<const uint8_t> bytes, std::size_t at, uint16_t& value) noexcept {
    if (at + 2 > bytes.size())
        return false;
    value = static_cast<uint16_t>(bytes[at] | (static_cast<uint16_t>(bytes[at + 1]) << 8U));
    return true;
}

/// Reads a little-endian u32.
///
/// @param bytes the bytes
/// @param at where it starts
/// @param[out] value the value
/// @return false when the bytes end first
bool read_u32(std::span<const uint8_t> bytes, std::size_t at, uint32_t& value) noexcept {
    if (at + 4 > bytes.size())
        return false;
    value = static_cast<uint32_t>(bytes[at]) | (static_cast<uint32_t>(bytes[at + 1]) << 8U) |
            (static_cast<uint32_t>(bytes[at + 2]) << 16U) |
            (static_cast<uint32_t>(bytes[at + 3]) << 24U);
    return true;
}

/// Writes a little-endian u32 over four bytes already there.
///
/// @param[in,out] bytes the bytes
/// @param at where it starts
/// @param value the value
void write_u32(std::vector<uint8_t>& bytes, std::size_t at, uint32_t value) noexcept {
    bytes[at] = static_cast<uint8_t>(value);
    bytes[at + 1] = static_cast<uint8_t>(value >> 8U);
    bytes[at + 2] = static_cast<uint8_t>(value >> 16U);
    bytes[at + 3] = static_cast<uint8_t>(value >> 24U);
}

/// Takes the next chunk.
///
/// @param bytes the recording
/// @param[in,out] cursor where the next chunk starts; moved past it
/// @return the chunk, or nothing when the recording ends inside one
std::optional<Chunk> take_chunk(std::span<const uint8_t> bytes, std::size_t& cursor) noexcept {
    uint16_t total = 0;
    if (!read_u16(bytes, cursor, total) || total < tad::layout::chunk_length_bytes ||
        cursor + total > bytes.size())
        return std::nullopt;
    Chunk chunk;
    chunk.at = cursor;
    chunk.total = total;
    chunk.data = bytes.subspan(
        cursor + tad::layout::chunk_length_bytes, total - tad::layout::chunk_length_bytes
    );
    cursor += total;
    return chunk;
}

/// Appends one chunk's original bytes.
///
/// @param[in,out] out the bytes written so far
/// @param bytes the recording
/// @param chunk the chunk
void keep_chunk(std::vector<uint8_t>& out, std::span<const uint8_t> bytes, const Chunk& chunk) {
    out.insert(
        out.end(),
        bytes.begin() + static_cast<std::ptrdiff_t>(chunk.at),
        bytes.begin() + static_cast<std::ptrdiff_t>(chunk.at + chunk.total)
    );
}

/// A recording with sector 6 removed, or the reason the walk could not.
struct Stripped {
    bool walked{false};           ///< the header, the count and every sector were walked
    bool removed{false};          ///< at least one sector 6 was left out
    std::vector<uint8_t> bytes{}; ///< the recording, when `removed`
};

/// Copies a recording with every sector 6 left out. Every other chunk is the
/// original bytes, and the sector count is the number that remain. The bytes
/// after the sectors are copied as they are.
///
/// @param bytes a recording parse has already accepted
/// @param sector_count how many sector chunks follow the count
/// @return the walk
Stripped without_sector_six(std::span<const uint8_t> bytes, uint32_t sector_count) {
    Stripped stripped;
    std::size_t cursor = 0;
    const std::optional<Chunk> header = take_chunk(bytes, cursor);
    const std::optional<Chunk> count = take_chunk(bytes, cursor);
    if (!header || !count || count->data.size() != tad::layout::sector_count_bytes)
        return stripped;
    std::vector<uint8_t> out;
    out.reserve(bytes.size());
    keep_chunk(out, bytes, *header);
    keep_chunk(out, bytes, *count);
    const std::size_t count_at = out.size() - count->data.size();
    uint32_t kept = 0;
    bool removed = false;
    for (uint32_t index = 0; index < sector_count; ++index) {
        const std::optional<Chunk> sector = take_chunk(bytes, cursor);
        uint32_t type = 0;
        if (!sector || !read_u32(sector->data, 0, type))
            return Stripped{};
        if (type == sector_left_out) {
            removed = true;
            continue;
        }
        keep_chunk(out, bytes, *sector);
        ++kept;
    }
    stripped.walked = true;
    if (!removed)
        return stripped;
    write_u32(out, count_at, kept);
    out.insert(out.end(), bytes.begin() + static_cast<std::ptrdiff_t>(cursor), bytes.end());
    stripped.removed = true;
    stripped.bytes = std::move(out);
    return stripped;
}

/// The format version a header chunk names, when its bytes are there.
///
/// @param bytes the file
/// @return the version, or nothing when the header does not hold one
std::optional<uint16_t> header_version(std::span<const uint8_t> bytes) noexcept {
    std::size_t cursor = 0;
    const std::optional<Chunk> header = take_chunk(bytes, cursor);
    if (!header || header->data.size() < tad::layout::magic_bytes + 2)
        return std::nullopt;
    uint16_t version = 0;
    if (!read_u16(header->data, tad::layout::magic_bytes, version))
        return std::nullopt;
    return version;
}

/// A refusal, with the catalogue sentence and the English a player sees.
///
/// @param sentence the catalogue sentence
/// @param version the text of `{version}`, when the sentence names it
/// @return the result
ImportResult refuse(std::string_view sentence, std::string version = {}) {
    ImportResult result;
    result.sentence = std::string(sentence);
    result.version = std::move(version);
    result.error = result.sentence;
    if (!result.version.empty()) {
        const std::string field = "{version}";
        const std::size_t at = result.error.find(field);
        if (at != std::string::npos)
            result.error.replace(at, field.size(), result.version);
    }
    return result;
}

/// Tells whether bytes 2 to 9 are the TA Demo signature.
///
/// A file shorter than those ten bytes, or whose bytes there are anything
/// else, is not a recording. The parser's own error is not what decides that:
/// a short file fails before it can name a bad signature.
///
/// @param bytes the file
/// @return true when the signature is there
bool has_tad_signature(std::span<const uint8_t> bytes) noexcept {
    constexpr std::size_t signature_at = 2;
    if (bytes.size() < signature_at + tad::layout::magic_bytes)
        return false;
    for (std::size_t index = 0; index < tad::layout::magic_bytes; ++index)
        if (bytes[signature_at + index] != static_cast<uint8_t>(tad::magic[index]))
            return false;
    return true;
}

/// What a `.tad` that does not parse says.
///
/// @param bytes the file
/// @param error the parser's error
/// @return the refusal
ImportResult refuse_tad(std::span<const uint8_t> bytes, const tad::Error& error) {
    if (!has_tad_signature(bytes))
        return refuse(tad_not_a_recording);
    if (error.code == tad::ErrorCode::unsupported_version) {
        const std::optional<uint16_t> version = header_version(bytes);
        if (version)
            return refuse(tad_unsupported_version, std::to_string(*version));
    }
    return refuse(tad_unreadable);
}

/// Reads a whole file, at most most_file_bytes.
///
/// @param file the file
/// @param[out] bytes the file's bytes
/// @return how the read went
Loaded load_file(const fs::path& file, std::vector<uint8_t>& bytes) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return Loaded::unreadable;
    bytes.clear();
    std::vector<char> chunk(read_chunk_bytes);
    for (;;) {
        in.read(chunk.data(), static_cast<std::streamsize>(chunk.size()));
        const std::streamsize got = in.gcount();
        if (got > 0) {
            if (bytes.size() + static_cast<std::size_t>(got) > most_file_bytes)
                return Loaded::too_large;
            const auto* first = reinterpret_cast<const uint8_t*>(chunk.data());
            bytes.insert(bytes.end(), first, first + static_cast<std::ptrdiff_t>(got));
        }
        if (in.eof())
            return Loaded::read;
        if (!in)
            return Loaded::unreadable;
    }
}

/// The SHA-256 of some bytes, lower-case hexadecimal.
///
/// @param bytes the bytes
/// @return the digest
std::string sha256_of(std::span<const uint8_t> bytes) {
    const auto hex = oa::base::sha256::to_hex(oa::base::sha256::digest_of(bytes));
    return {hex.begin(), hex.end()};
}

/// Tells whether two digests are the same, without ASCII case.
///
/// @param left a digest
/// @param right a digest
/// @return true when every character matches
bool same_hex(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t at = 0; at < left.size(); ++at) {
        char first = left[at];
        char second = right[at];
        if (first >= 'A' && first <= 'Z')
            first = static_cast<char>(first - 'A' + 'a');
        if (second >= 'A' && second <= 'Z')
            second = static_cast<char>(second - 'A' + 'a');
        if (first != second)
            return false;
    }
    return true;
}

/// The characters Windows-1252 gives the bytes 0x80 to 0x9F. The five bytes
/// it leaves undefined stand for the characters of the same value.
constexpr std::array<uint16_t, 32> windows_1252_high = {
    0x20AC, 0x0081, 0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021, 0x02C6, 0x2030, 0x0160,
    0x2039, 0x0152, 0x008D, 0x017D, 0x008F, 0x0090, 0x2018, 0x2019, 0x201C, 0x201D, 0x2022,
    0x2013, 0x2014, 0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0x009D, 0x017E, 0x0178,
};

/// Appends one character as UTF-8.
///
/// @param[in,out] out the text written so far
/// @param code the character, below U+10000
void append_utf8(std::string& out, uint32_t code) {
    if (code < 0x80U) {
        out.push_back(static_cast<char>(code));
    } else if (code < 0x800U) {
        out.push_back(static_cast<char>(0xC0U | (code >> 6U)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    } else {
        out.push_back(static_cast<char>(0xE0U | (code >> 12U)));
        out.push_back(static_cast<char>(0x80U | ((code >> 6U) & 0x3FU)));
        out.push_back(static_cast<char>(0x80U | (code & 0x3FU)));
    }
}

/// A recording's text as UTF-8. A text that is already UTF-8 is kept; any
/// other is read as Windows-1252, up to the first NUL.
///
/// @param bytes the text as recorded
/// @return the text
std::string text_of(std::string_view bytes) {
    const std::size_t end = bytes.find('\0');
    if (end != std::string_view::npos)
        bytes = bytes.substr(0, end);
    if (oa::formats::json::utf8_valid(bytes))
        return std::string(bytes);
    std::string text;
    text.reserve(bytes.size() * 2);
    for (const char letter : bytes) {
        const auto byte = static_cast<uint8_t>(letter);
        uint32_t code = byte;
        if (byte >= 0x80U && byte <= 0x9FU)
            code = windows_1252_high[static_cast<std::size_t>(byte - 0x80U)];
        append_utf8(text, code);
    }
    return text;
}

/// The file's name, with nothing of a folder left in it.
///
/// @param source the file that was opened
/// @param kind which reader reads it
/// @return the name under Imported
std::string file_name_of(const fs::path& source, FileKind kind) {
    std::string name = oa::platform::path_to_utf8(source.filename());
    for (char& letter : name) {
        if (letter == '/' || letter == '\\')
            letter = '_';
    }
    if (name.empty() || name == "." || name == "..")
        name = kind == FileKind::tad ? "replay.tad" : "replay.oademo";
    if (!file_kind_of(name))
        name += kind == FileKind::tad ? ".tad" : ".oademo";
    return name;
}

/// The name's stem, for a recording that names no map.
///
/// @param name the file's name
/// @return the stem; "replay" when it has none
std::string stem_of(std::string_view name) {
    const std::string stem = oa::platform::path_to_utf8(oa::platform::path_from_utf8(name).stem());
    return stem.empty() ? std::string("replay") : stem;
}

/// The nth spelling of a name: the name itself, then the stem with " (2)".
///
/// @param name the name asked for
/// @param copy 1 for the name itself
/// @return the name
std::string numbered(std::string_view name, int copy) {
    if (copy <= 1)
        return std::string(name);
    const fs::path file = oa::platform::path_from_utf8(name);
    fs::path stem = file.stem();
    stem += " (" + std::to_string(copy) + ")";
    stem += file.extension();
    return oa::platform::path_to_utf8(stem);
}

/// Writes a file whole, replacing it.
///
/// @param path the file
/// @param bytes what it holds
/// @return false when it cannot be written
bool write_file(const fs::path& path, std::span<const uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out)
        return false;
    if (!bytes.empty())
        out.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
    out.flush();
    return static_cast<bool>(out);
}

/// The identity a replay file already under Recordings keeps, when it can be read.
///
/// @param file the file
/// @param kind which reader reads it
/// @return the identity, or nothing when the file cannot be read
std::optional<std::string> identity_of(const fs::path& file, FileKind kind) {
    std::vector<uint8_t> bytes;
    if (load_file(file, bytes) != Loaded::read)
        return std::nullopt;
    if (kind == FileKind::tad)
        return sha256_of(bytes);
    const oademo::DescriptionResult described = oademo::read_description(bytes);
    if (!described.ok())
        return std::nullopt;
    return described.meta->recording.sha256;
}

/// The identity the index already keeps for a file that has not changed.
///
/// The same path, size and modification time, and a file the index could
/// read. Anything else is read again.
///
/// @param index the Recordings folder's index
/// @param listed the file, as a listing names it
/// @return the identity; nothing when the file has to be read
std::optional<std::string> indexed_identity(const Index& index, const Listed& listed) {
    for (const Replay& replay : index.replays) {
        if (replay.path != listed.path)
            continue;
        if (replay.size != listed.size || replay.modified != listed.modified)
            return std::nullopt;
        if (!replay.damage.empty() || replay.sha256.empty())
            return std::nullopt;
        return replay.sha256;
    }
    return std::nullopt;
}

/// The path of one listed replay, under Recordings.
///
/// @param recordings the Recordings folder
/// @param listed the file
/// @return the path
fs::path listed_file(const fs::path& recordings, const Listed& listed) {
    fs::path file = recordings;
    std::string_view rest = listed.path;
    while (!rest.empty()) {
        const std::size_t slash = rest.find('/');
        const std::string_view part = rest.substr(0, slash);
        file /= oa::platform::path_from_utf8(part);
        if (slash == std::string_view::npos)
            break;
        rest = rest.substr(slash + 1);
    }
    return file;
}

/// The replay under Recordings whose identity is `sha256`.
///
/// A file the index holds unchanged is not read. Only a file the index does
/// not name, or whose size or modification time has moved, is opened.
///
/// @param recordings the Recordings folder
/// @param sha256 the identity, hexadecimal
/// @return its path, relative to Recordings, or nothing
std::optional<std::string> find_identity(const fs::path& recordings, std::string_view sha256) {
    const Index index = load_index(recordings);
    for (const Listed& listed : list_folder(recordings)) {
        std::optional<std::string> found = indexed_identity(index, listed);
        if (!found)
            found = identity_of(listed_file(recordings, listed), listed.file);
        if (found && same_hex(*found, sha256))
            return listed.path;
    }
    return std::nullopt;
}

/// The bytes kept for a `.tad`, and the sentence when it is refused.
///
/// @param bytes the file that was opened
/// @param[out] kept the bytes to write, the file itself when sector 6 is absent
/// @param[out] refusal set when the file is refused
/// @return false when the file is refused
bool keep_tad(std::span<const uint8_t> bytes, std::vector<uint8_t>& kept, ImportResult& refusal) {
    const tad::ParseResult parsed = tad::parse(bytes);
    if (!parsed.ok()) {
        refusal = refuse_tad(bytes, *parsed.error);
        return false;
    }
    const Stripped stripped =
        without_sector_six(bytes, static_cast<uint32_t>(parsed.demo->sectors.size()));
    if (!stripped.walked) {
        refusal = refuse(tad_unreadable);
        return false;
    }
    if (stripped.removed) {
        const tad::ParseResult again = tad::parse(stripped.bytes);
        if (!again.ok() ||
            tad::find_sector(*again.demo, tad::SectorType::player_address) != nullptr) {
            refusal = refuse(tad_unreadable);
            return false;
        }
        kept = stripped.bytes;
        return true;
    }
    kept.assign(bytes.begin(), bytes.end());
    return true;
}

/// The bytes kept for a `.oademo`, and the sentence when it is refused.
///
/// @param bytes the file that was opened
/// @param[out] kept the bytes to write
/// @param[out] sha256 the recording's identity, as kept
/// @param[out] title the map's name
/// @param[out] refusal set when the file is refused
/// @return false when the file is refused
bool keep_oademo(
    std::span<const uint8_t> bytes,
    std::vector<uint8_t>& kept,
    std::string& sha256,
    std::string& title,
    ImportResult& refusal
) {
    const bool known = oademo::recognised(bytes);
    const oademo::ReadResult read = oademo::read(bytes);
    if (!known || !read.ok()) {
        refusal = refuse(read.error.empty() ? oademo::not_a_replay : std::string_view(read.error));
        return false;
    }
    oademo::Replay replay = *read.replay;
    if (!replay.meta.map.name.empty())
        title = replay.meta.map.name;
    const tad::ParseResult parsed = tad::parse(replay.recording);
    const bool holds =
        parsed.ok() && tad::find_sector(*parsed.demo, tad::SectorType::player_address) != nullptr;
    if (!holds) {
        sha256 = replay.meta.recording.sha256;
        kept.assign(bytes.begin(), bytes.end());
        return true;
    }
    const Stripped stripped =
        without_sector_six(replay.recording, static_cast<uint32_t>(parsed.demo->sectors.size()));
    if (!stripped.walked || !stripped.removed) {
        refusal = refuse(oademo::recording_unreadable);
        return false;
    }
    replay.recording = stripped.bytes;
    oademo::set_recording_identity(replay);
    replay.meta.recording.addresses_removed = true;
    const oademo::WriteResult written = oademo::write(replay);
    if (!written.ok()) {
        refusal = refuse(oademo::recording_unreadable);
        return false;
    }
    sha256 = replay.meta.recording.sha256;
    kept = written.bytes;
    return true;
}

} // namespace

ImportResult import_replay(const fs::path& recordings, const fs::path& source) {
    const std::optional<FileKind> kind = file_kind_of(oa::platform::path_to_utf8(source));
    if (!kind)
        return refuse(oademo::not_a_replay);
    std::vector<uint8_t> loaded;
    const Loaded loaded_as = load_file(source, loaded);
    if (loaded_as == Loaded::too_large)
        return refuse(file_too_large);
    if (loaded_as != Loaded::read)
        return refuse(file_unreadable);

    const std::string name = file_name_of(source, *kind);
    std::string title = stem_of(name);
    std::vector<uint8_t> kept;
    std::string sha256;
    if (*kind == FileKind::tad) {
        ImportResult refusal;
        if (!keep_tad(loaded, kept, refusal))
            return refusal;
        sha256 = sha256_of(kept);
        const tad::ParseResult parsed = tad::parse(kept);
        if (parsed.ok()) {
            const std::string map = text_of(parsed.demo->map_name);
            if (!map.empty())
                title = map;
        }
    } else {
        ImportResult refusal;
        if (!keep_oademo(loaded, kept, sha256, title, refusal))
            return refusal;
        if (title.empty())
            title = stem_of(name);
    }

    if (const std::optional<std::string> already = find_identity(recordings, sha256)) {
        ImportResult result;
        result.ok = true;
        result.path = *already;
        result.sha256 = sha256;
        result.title = std::move(title);
        return result;
    }

    const fs::path folder = recordings / std::string(imported_folder_name);
    std::error_code failure;
    fs::create_directories(folder, failure);
    if (failure)
        return refuse(replay_not_copied);

    fs::path destination;
    std::string relative;
    bool placed = false;
    for (int copy = 1; copy < most_names; ++copy) {
        const std::string candidate = numbered(name, copy);
        destination = folder / oa::platform::path_from_utf8(candidate);
        failure.clear();
        if (fs::exists(destination, failure) || failure)
            continue;
        // The partial name is a sibling, so a stop mid-write is not itself a
        // replay. A name taken between the check and the write is replaced
        // only when it is that partial.
        fs::path temporary = destination;
        temporary += ".importing";
        if (!write_file(temporary, kept)) {
            std::error_code removed;
            fs::remove(temporary, removed);
            return refuse(replay_not_copied);
        }
        failure.clear();
        fs::rename(temporary, destination, failure);
        if (failure) {
            std::error_code removed;
            fs::remove(temporary, removed);
            failure.clear();
            if (fs::exists(destination, failure))
                continue;
            return refuse(replay_not_copied);
        }
        relative = std::string(imported_folder_name) + "/" + candidate;
        placed = true;
        break;
    }
    if (!placed)
        return refuse(replay_not_copied);

    ImportResult result;
    result.ok = true;
    result.copied = true;
    result.path = std::move(relative);
    result.sha256 = std::move(sha256);
    result.title = std::move(title);
    return result;
}

} // namespace oa::data::replays
