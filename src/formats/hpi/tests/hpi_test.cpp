// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/base/bytes.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/sqsh.hpp"
#include "oa/test/game_assets.hpp"

#include <zlib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>
#endif

namespace {

namespace fs = std::filesystem;
using Bytes = std::vector<uint8_t>;

int failures = 0;

void check(bool condition, const std::string& message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << '\n';
        ++failures;
    }
}

template <class Function>
bool throws(Function&& function, std::string_view containing = {}) {
    try {
        function();
    } catch (const std::exception& error) {
        return containing.empty() ||
               std::string(error.what()).find(containing) != std::string::npos;
    }
    return false;
}

/// Reports whether reading an archive entry fails with a message holding `containing`.
///
/// @param archive the archive
/// @param path the entry
/// @param containing text the error's message holds
/// @return true when the read fails so
bool read_fails(const oa::HpiArchive& archive, std::string_view path, std::string_view containing) {
    const auto read = archive.read(path);
    return !read.ok() && read.error.message != nullptr &&
           std::string_view(read.error.message).find(containing) != std::string_view::npos;
}

/// Opens an archive file the test wrote, throwing when it is refused.
///
/// @param path the archive
/// @return the archive
oa::HpiArchive opened(const fs::path& path) {
    auto archive = oa::open_hpi_file(path);
    if (!archive.ok())
        throw std::runtime_error(archive.error.message);
    return std::move(*archive.value);
}

/// Reports whether opening an archive fails with a message holding `containing`.
///
/// @param path the archive
/// @param containing text the error's message holds
/// @return true when the archive is refused so
bool opening_fails(const fs::path& path, std::string_view containing = {}) {
    const auto archive = oa::open_hpi_file(path);
    return !archive.ok() && archive.error.message != nullptr &&
           std::string_view(archive.error.message).find(containing) != std::string_view::npos;
}

Bytes text(std::string_view value) {
    return Bytes(value.begin(), value.end());
}

uint32_t get32(const Bytes& bytes, std::size_t at) {
    return oa::base::bytes::load_le32(bytes.data() + at);
}

void put32(Bytes& bytes, std::size_t at, uint32_t value) {
    oa::base::bytes::store_le32(bytes.data() + at, value);
}

class TempDir {
  public:

    TempDir() {
        static std::atomic<int> counter{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        path_ = fs::temp_directory_path() /
                ("oa-hpi-test-" + std::to_string(stamp) + "-" + std::to_string(counter++));
        fs::create_directories(path_);
    }

    ~TempDir() {
        std::error_code ignored;
        fs::remove_all(path_, ignored);
    }

    const fs::path& path() const { return path_; }

    fs::path write(const std::string& name, const Bytes& bytes) const {
        const auto target = path_ / name;
        fs::create_directories(target.parent_path());
        std::ofstream stream(target, std::ios::binary);
        stream.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
        return target;
    }

  private:

    fs::path path_;
};

Bytes archive_of(std::vector<oa::HpiWriteFile> files, oa::HpiWriteOptions options = {}) {
    return oa::write_hpi(files, options);
}

// Offset of the single root file's 9-byte data record ("a" as the only name).
constexpr std::size_t kSingleRecord = 20 + 8 + 9 + 2;

Bytes pattern_bytes(std::size_t size) {
    Bytes bytes(size);
    uint32_t state = 12345;
    for (auto& byte : bytes) {
        state = state * 1103515245U + 12345U;
        byte = static_cast<uint8_t>((state >> 16U) & 0x0F);
    }
    return bytes;
}

void hpi_writer_round_trip() {
    TempDir dir;
    const Bytes big = pattern_bytes(3 * oa::formats::hpi::BlockBytes + 777);
    std::vector<oa::HpiWriteFile> files{
        {"readme.txt", text("plain"), oa::formats::hpi::CompressionNone},
        {"units/armcom.fbi",
         text("[UNITINFO]{name=Commander;}"),
         oa::formats::hpi::CompressionLZ77},
        {"units/big.bin", big, oa::formats::hpi::CompressionZLib},
        {"Units/Sub/deep.bin", big, oa::formats::hpi::CompressionLZ77},
        {"empty.dat", {}, oa::formats::hpi::CompressionLZ77},
    };
    for (const uint8_t key : {uint8_t{0}, uint8_t{0x7D}, uint8_t{0xBF}}) {
        for (const bool scramble : {false, true}) {
            const auto path = dir.write("round.hpi", archive_of(files, {key, scramble, "1998"}));
            oa::HpiArchive archive = opened(path);
            const auto entries = archive.entries();
            check(entries.size() == files.size(), "round trip entry count");
            check(entries[1].path == "units/armcom.fbi", "directory merge keeps first spelling");
            for (const auto& file : files)
                check(
                    archive.read(file.path).value == file.bytes, "round trip content " + file.path
                );
        }
    }
}

void hpi_requires_copyright_trailer_any_year() {
    TempDir dir;
    const auto good = archive_of({{"a", text("x"), 0}}, {0, true, "2001"});
    check(!opening_fails(dir.write("year.hpi", good)), "any trailer year accepted");
    auto year = good;
    std::copy_n("ab\0d", 4, year.end() - 26);
    check(!opening_fails(dir.write("odd-year.hpi", year)), "year bytes are never compared");
    auto brand = good;
    brand.back() = 'T';
    check(opening_fails(dir.write("brand.hpi", brand), "trailer"), "altered trailer rejected");
    auto missing = good;
    missing.resize(missing.size() - 36);
    check(opening_fails(dir.write("bare.hpi", missing), "trailer"), "missing trailer rejected");
}

void hpi_version_must_be_one() {
    TempDir dir;
    auto bytes = archive_of({{"a", text("x"), 0}});
    put32(bytes, 4, 0x00020000U);
    check(opening_fails(dir.write("v2.hpi", bytes)), "version 2 rejected");
}

void hpi_header_key_ff_disables_decryption() {
    check(oa::hpi_archive_key(0xFF) == 0, "header 0xFF gives key zero");
    check(oa::hpi_archive_key(0x01) == 0xFB, "header 0x01 key");
    TempDir dir;
    auto bytes = archive_of({{"a", text("Q"), 0}});
    put32(bytes, 12, 0xFF);
    oa::HpiArchive archive = opened(dir.write("ff.hpi", bytes));
    check(archive.read("a").value == text("Q"), "header key 0xFF leaves data plain");
}

void hpi_lookup_last_duplicate_wins() {
    TempDir dir;
    oa::HpiArchive archive = opened(
        dir.write("dup.hpi", archive_of({{"a.txt", text("one"), 0}, {"A.TXT", text("two"), 0}}))
    );
    check(archive.entries().size() == 2, "duplicates are retained");
    check(archive.read("a.txt").value == text("two"), "last duplicate in a directory wins");
}

void hpi_intermediate_file_ends_lookup() {
    TempDir dir;
    const auto first = dir.write(
        "first.hpi", archive_of({{"units/x.fbi", text("hidden"), 0}, {"UNITS", text("file"), 0}})
    );
    const auto second = dir.write("second.hpi", archive_of({{"units/x.fbi", text("second"), 0}}));
    oa::HpiArchive archive = opened(first);
    check(!archive.lookup("units\\x.fbi"), "last 'units' is a file: lookup fails");
    fs::create_directory(dir.path() / "loose");
    oa::AssetStore store(dir.path() / "loose");
    store.mount(first);
    store.mount(second);
    check(store.read("units/x.fbi").bytes == text("second"), "search continues in the next mount");
}

void hpi_directory_match_falls_through() {
    TempDir dir;
    const auto first = dir.write("first.hpi", archive_of({{"gamedata/x", text("dir"), 0}}));
    const auto second = dir.write("second.hpi", archive_of({{"gamedata", text("file"), 0}}));
    fs::create_directory(dir.path() / "loose");
    oa::AssetStore store(dir.path() / "loose");
    store.mount(first);
    store.mount(second);
    check(store.read("GAMEDATA").bytes == text("file"), "a directory hit moves to the next mount");
}

void hpi_entry_flag_bit0_is_directory() {
    TempDir dir;
    auto bytes = archive_of({{"d/f", text("inner"), 0}, {"g", text("file"), 0}});
    // Root list at 28: entry 0 "d" (directory), entry 1 "g" (file).
    bytes[28 + 8] = 0x03;
    bytes[28 + 9 + 8] = 0x02;
    oa::HpiArchive archive = opened(dir.write("flags.hpi", bytes));
    check(archive.read("d/f").value == text("inner"), "flag 0x03 is a directory");
    check(archive.read("g").value == text("file"), "flag 0x02 is a file");
}

void hpi_any_nonzero_compression_is_chunked() {
    TempDir dir;
    auto bytes = archive_of({{"a", text("chunked"), oa::formats::hpi::CompressionLZ77}});
    bytes[kSingleRecord + 8] = 0x07;
    oa::HpiArchive archive = opened(dir.write("method.hpi", bytes));
    check(archive.read("a").value == text("chunked"), "compression byte 7 reads as chunked");
}

std::size_t first_chunk(const Bytes& bytes) {
    return get32(bytes, kSingleRecord) + 4;
}

void sqsh_stored_type_is_fatal() {
    TempDir dir;
    auto bytes =
        archive_of({{"a", text("stored"), oa::formats::hpi::CompressionLZ77}}, {0, false, "1997"});
    bytes[first_chunk(bytes) + 5] = 0;
    oa::HpiArchive archive = opened(dir.write("stored.hpi", bytes));
    check(read_fails(archive, "a", "SQUASHERR_BADUNPACKSIZE"), "stored chunk is fatal");
    bytes[first_chunk(bytes) + 5] = 4;
    oa::HpiArchive typed = opened(dir.write("type4.hpi", bytes));
    check(read_fails(typed, "a", "SQUASHERR_BADUNPACKTYPE"), "type 4 chunk is fatal");
}

void sqsh_version_byte_ignored() {
    TempDir dir;
    auto bytes = archive_of({{"a", text("versioned"), oa::formats::hpi::CompressionZLib}});
    bytes[first_chunk(bytes) + 4] = 0x99;
    oa::HpiArchive archive = opened(dir.write("version.hpi", bytes));
    check(archive.read("a").value == text("versioned"), "SQSH version byte is not checked");
}

// A zlib chunk reads only when its stream reaches its end, check value
// included, at exactly the chunk's unpacked size.
void sqsh_zlib_stream_must_end_cleanly() {
    TempDir dir;
    const auto pristine = archive_of(
        {{"a", text("adler tail"), oa::formats::hpi::CompressionZLib}}, {0, false, "1997"}
    );
    const std::size_t chunk = first_chunk(pristine);
    const uint32_t packed = get32(pristine, chunk + 7);
    const auto resum = [&](Bytes& bytes, uint32_t payload) {
        put32(bytes, chunk + 7, payload);
        put32(
            bytes,
            chunk + 15,
            oa::formats::sqsh::chunk_checksum(std::span(bytes).subspan(chunk + 19, payload))
        );
    };
    oa::HpiArchive whole = opened(dir.write("whole.hpi", pristine));
    check(whole.read("a").value == text("adler tail"), "a clean zlib stream reads");

    auto bytes = pristine;
    bytes[chunk + 19 + packed - 1] ^= 0x5A;
    resum(bytes, packed);
    oa::HpiArchive adler = opened(dir.write("adler.hpi", bytes));
    check(read_fails(adler, "a", "SQUASHERR_BADUNPACKSIZE"), "a bad adler32 fails the chunk");

    // The stream's four-byte check value left off, as some packing tools
    // write it: the deflate data is whole, so the chunk reads.
    bytes = pristine;
    resum(bytes, packed - 4);
    oa::HpiArchive missing = opened(dir.write("missing.hpi", bytes));
    check(
        missing.read("a").value == text("adler tail"), "a stream missing only its check value reads"
    );

    // Half a check value, and a stream cut inside its deflate data.
    for (const uint32_t cut_to : {packed - 2, packed - 5}) {
        bytes = pristine;
        resum(bytes, cut_to);
        oa::HpiArchive cut = opened(dir.write("cut.hpi", bytes));
        check(read_fails(cut, "a", "SQUASHERR_BADUNPACKSIZE"), "a stream cut short fails");
    }

    // The check value left off and the header broken: nothing vouches for
    // the stream, so it fails.
    bytes = pristine;
    bytes[chunk + 20] ^= 0x01;
    resum(bytes, packed - 4);
    oa::HpiArchive headless = opened(dir.write("headless.hpi", bytes));
    check(
        read_fails(headless, "a", "SQUASHERR_BADUNPACKSIZE"),
        "a stream without its check value and with a bad header fails"
    );

    bytes = pristine;
    bytes[chunk + 19] ^= 0xFF;
    resum(bytes, packed);
    oa::HpiArchive broken = opened(dir.write("broken.hpi", bytes));
    check(read_fails(broken, "a", "SQUASHERR_BADUNPACKSIZE"), "undecodable zlib fails");

    // A header that says one byte more than the stream holds, and one less.
    for (const uint32_t claimed :
         {get32(pristine, chunk + 11) + 1, get32(pristine, chunk + 11) - 1}) {
        bytes = pristine;
        put32(bytes, chunk + 11, claimed);
        oa::HpiArchive mismatched = opened(dir.write("mismatched.hpi", bytes));
        check(read_fails(mismatched, "a", "SQUASHERR_BADUNPACKSIZE"), "a size mismatch fails");
    }

    bytes = pristine;
    bytes[chunk + 15] ^= 1;
    oa::HpiArchive sum = opened(dir.write("sum.hpi", bytes));
    check(read_fails(sum, "a", "SQUASHERR_BADCHECKSUM"), "checksum is enforced");

    uint32_t length = 10;
    Bytes output(10);
    check(
        oa::uncompress_legacy(
            output, &length, std::span(pristine).subspan(chunk + 19, packed - 4)
        ) != Z_OK &&
            length == 10,
        "a stream without its end is not a success and keeps the expected length"
    );
}

void sqsh_chunk_table_locates_blocks() {
    TempDir dir;
    const Bytes big = pattern_bytes(oa::formats::hpi::BlockBytes + 100);
    auto bytes = archive_of({{"a", big, oa::formats::hpi::CompressionLZ77}}, {0, true, "1997"});
    const std::size_t table = get32(bytes, kSingleRecord);
    const uint32_t first = get32(bytes, table);
    const Bytes padding{0xEE, 0xEE, 0xEE, 0xEE, 0xEE};
    bytes.insert(
        bytes.begin() + static_cast<std::ptrdiff_t>(table + 8 + first),
        padding.begin(),
        padding.end()
    );
    put32(bytes, table, first + static_cast<uint32_t>(padding.size()));
    oa::HpiArchive archive = opened(dir.write("gap.hpi", bytes));
    check(archive.read("a").value == big, "blocks are located by summing the size table");
}

void hpi_negative_directory_count_is_empty() {
    TempDir dir;
    auto bytes = archive_of({{"a", text("x"), 0}});
    put32(bytes, 20, 0x80000001U);
    oa::HpiArchive archive = opened(dir.write("neg.hpi", bytes));
    check(archive.entries().empty(), "negative count reads as an empty root");
}

void hpi_rejects_cycles() {
    TempDir dir;
    auto bytes = archive_of({{"d/f", text("x"), 0}});
    // Entry "d" points its directory node back at the root node.
    put32(bytes, 28 + 4, 20);
    check(opening_fails(dir.write("cycle.hpi", bytes), "cycle"), "cycle rejected");
}

void match_wildcard_rules() {
    check(oa::match_wildcard("ARMCOM.FBI", "*.fbi"), "suffix match");
    check(oa::match_wildcard("armcom.fbi", "ARM???.FBI"), "question marks");
    check(!oa::match_wildcard("armcom.fbix", "*.fbi"), "no extra suffix");
    check(oa::match_wildcard("abc", "a*"), "trailing star");
    check(oa::match_wildcard("", "*"), "empty name with star");
    check(!oa::match_wildcard("abc", ""), "empty pattern");
    check(oa::match_wildcard("a.b.c", "*.c"), "backtracking star");
}

void asset_store_find_and_shadowing() {
    TempDir dir;
    const auto first = dir.write(
        "first.hpi",
        archive_of(
            {{"units/b.fbi", text("1b"), 0},
             {"units/a.fbi", text("1a"), 0},
             {"units/x.txt", text("1x"), 0}}
        )
    );
    const auto second = dir.write("second.hpi", archive_of({{"units/a.fbi", text("2a"), 0}}));
    const auto loose = dir.path() / "game";
    fs::create_directories(loose / "Units");
    std::ofstream(loose / "Units" / "B.FBI") << "loose";
    std::ofstream(loose / "Units" / "c.fbi") << "c";
    oa::AssetStore store(loose);
    store.mount(first);
    store.mount(second);
    store.mark_loose_shadows();
    const auto found = store.find("units\\*.fbi");
    std::vector<std::string> names;
    for (const auto& entry : found)
        names.push_back(entry.name + "@" + std::to_string(entry.mount));
    const std::vector<std::string> expected{"B.FBI@-1", "c.fbi@-1", "a.fbi@0", "a.fbi@1"};
    check(names == expected, "find: loose in NTFS order, then archives; loose-shadowed skipped");
    const auto only_second = store.find("units\\*", {1, false});
    check(
        only_second.size() == 1 && only_second[0].name == "a.fbi", "scoped find stays in one mount"
    );
    check(store.count_entries("units\\*", false) == 5, "count skips . and ..");
    const auto scanned = store.scan_recursive("units", "*.fbi");
    check(scanned.size() == 4, "recursive scan keeps cross-archive duplicates");
    check(store.read("units/b.fbi").bytes == text("loose"), "loose wins over archives");
    check(store.read("units/a.fbi").bytes == text("1a"), "first mount wins");
}

void asset_store_discover_order_and_hpi_limit() {
    TempDir dir;
    const auto root = dir.path() / "game";
    fs::create_directories(root);
    const auto small = archive_of({{"x", text("x"), 0}});
    for (int i = 0; i < 12; ++i)
        std::ofstream(root / ("pack" + std::to_string(10 + i) + ".hpi"), std::ios::binary)
            .write(
                reinterpret_cast<const char*>(small.data()),
                static_cast<std::streamsize>(small.size())
            );
    std::ofstream(root / "broken.hpi") << "not an archive";
    for (const char* name :
         {"rev31.gp3", "rev30.gp3", "b.ccx", "A.CCX", "corplas.ufo", "Cormabm.ufo", "CorNecro.ufo"})
        std::ofstream(root / name, std::ios::binary)
            .write(
                reinterpret_cast<const char*>(small.data()),
                static_cast<std::streamsize>(small.size())
            );
    oa::AssetStore store(root);
    const auto report = store.discover("31");
    std::vector<std::string> mounted;
    for (const auto& path : store.mount_paths())
        mounted.push_back(path.filename().string());
    const std::vector<std::string> expected{
        "rev31.gp3",
        "A.CCX",
        "b.ccx",
        "Cormabm.ufo",
        "CorNecro.ufo",
        "corplas.ufo",
        "pack10.hpi",
        "pack11.hpi",
        "pack12.hpi",
        "pack13.hpi",
        "pack14.hpi",
        "pack15.hpi",
        "pack16.hpi",
        "pack17.hpi",
        "pack18.hpi",
        "pack19.hpi",
    };
    check(mounted == expected, "discovery order: rev GP3, CCX, UFO, then ten HPI in NTFS order");
    check(
        !report.empty() && !report[6].mounted && report[6].path.filename() == "broken.hpi",
        "an invalid archive is skipped without counting"
    );
    // Already-mounted archives do not count toward the limit, so a rescan
    // mounts the next two plain archives as well.
    (void)store.discover("31");
    check(
        store.mount_paths().size() == expected.size() + 2 &&
            store.mount_paths().back().filename() == "pack21.hpi",
        "rescan continues past the ten-archive limit"
    );
}

// Mount order of a full install's archives, with one add-on CCX beside the
// game's own: the revision patch, the CCX and UFO groups in Windows name
// order, then the first ten of its thirteen *.HPI files. totala3.hpi,
// totala4.hpi and worlds.hpi are past the limit; the game reaches them only
// through the CD-ROM root scan.
const std::vector<std::string> kInstallMountOrder{
    "rev31.gp3",    "btdata.ccx",   "btmaps.ccx",   "ccdata.ccx",   "ccmaps.ccx",   "ccmiss.ccx",
    "extra.ccx",    "AFark.ufo",    "AFlea.ufo",    "AScarab.ufo",  "Cometctr.ufo", "Cormabm.ufo",
    "CorNecro.ufo", "corplas.ufo",  "Evadrivd.ufo", "Example.ufo",  "floggen.ufo",  "Mndsmars.ufo",
    "tactics1.hpi", "tactics2.hpi", "tactics3.hpi", "tactics4.hpi", "tactics5.hpi", "tactics6.hpi",
    "tactics7.hpi", "tactics8.hpi", "totala1.hpi",  "totala2.hpi",
};
const std::vector<std::string> kInstallDiscMountOrder{"totala3.hpi", "totala4.hpi", "worlds.hpi"};

std::vector<std::string> install_order_with_disc() {
    auto order = kInstallMountOrder;
    order.insert(order.end(), kInstallDiscMountOrder.begin(), kInstallDiscMountOrder.end());
    return order;
}

std::vector<std::string> mounted_names(const oa::AssetStore& store) {
    std::vector<std::string> names;
    for (const auto& path : store.mount_paths())
        names.push_back(path.filename().string());
    return names;
}

void asset_store_discover_pins_install_layout() {
    TempDir dir;
    const auto root = dir.path() / "game";
    fs::create_directories(root / "Data");
    fs::create_directories(root / "Sounds");
    // A directory whose name matches the pattern is a candidate that fails.
    fs::create_directories(root / "spare.hpi");
    const auto pack = [&](const std::string& name, std::vector<oa::HpiWriteFile> extra) {
        extra.push_back({"origin/" + name, text(name), 0});
        dir.write("game/" + name, archive_of(std::move(extra)));
    };
    for (const auto& name : kInstallMountOrder)
        pack(name, {});
    for (const char* name : {"totala3.hpi", "totala4.hpi", "worlds.hpi", "rev30.gp3"})
        pack(name, {});
    // The same path in several groups resolves from the earliest group even
    // when a plain name sort would put another first.
    pack("rev31.gp3", {{"gamedata/sound.tdf", text("rev31.gp3"), 0}});
    pack("ccdata.ccx", {{"gamedata/sound.tdf", text("ccdata.ccx"), 0}});
    pack(
        "totala1.hpi",
        {{"gamedata/sound.tdf", text("totala1.hpi"), 0},
         {"sounds/loose.wav", text("totala1.hpi"), 0}}
    );
    pack("extra.ccx", {{"anims/shared.gaf", text("extra.ccx"), 0}});
    pack("AFark.ufo", {{"anims/shared.gaf", text("AFark.ufo"), 0}});
    // Within a group Windows orders names case-insensitively, so Cormabm
    // precedes CorNecro although a byte comparison puts 'N' before 'm'.
    pack("CorNecro.ufo", {{"units/shared.fbi", text("CorNecro.ufo"), 0}});
    pack("Cormabm.ufo", {{"units/shared.fbi", text("Cormabm.ufo"), 0}});
    pack("btmaps.ccx", {{"maps/shared.ota", text("btmaps.ccx"), 0}});
    pack("tactics1.hpi", {{"maps/shared.ota", text("tactics1.hpi"), 0}});
    pack("totala2.hpi", {{"sounds/other.wav", text("totala2.hpi"), 0}});
    // Only an archive past the *.HPI limit provides this file.
    pack("totala3.hpi", {{"sounds/untdone.wav", text("totala3.hpi"), 0}});
    for (const char* name : {"Game.exe", "readme.txt", "Example.tdf", "Helper.dll"})
        dir.write(std::string("game/") + name, text("not an archive"));
    dir.write("game/broken.ufo", text("junk"));
    dir.write("game/Sounds/Loose.wav", text("loose"));

    oa::AssetStore store(root);
    const auto report = store.discover("31");
    check(mounted_names(store) == kInstallMountOrder, "install layout mounts in the game's order");
    int rejected = 0;
    for (const auto& outcome : report)
        if (!outcome.mounted) {
            ++rejected;
            const auto name = outcome.path.filename().string();
            check(
                name == "broken.ufo" || name == "spare.hpi",
                "only unreadable candidates are rejected"
            );
        }
    check(rejected == 2, "an invalid archive and a directory are both reported");
    check(
        store.read("gamedata/sound.tdf").bytes == text("rev31.gp3"),
        "revision patch beats CCX and HPI"
    );
    check(
        store.read("anims/shared.gaf").bytes == text("extra.ccx"),
        "CCX beats UFO regardless of name"
    );
    check(
        store.read("units/shared.fbi").bytes == text("Cormabm.ufo"),
        "case-insensitive order within a group"
    );
    check(store.read("maps/shared.ota").bytes == text("btmaps.ccx"), "CCX beats HPI");
    check(
        store.open("sounds\\untdone.wav") == nullptr,
        "an archive past the HPI limit provides nothing"
    );
    check(
        store.read("sounds/loose.wav").bytes == text("loose"), "a loose file beats every archive"
    );
    std::vector<std::string> found;
    for (const auto& entry : store.find("sounds\\*.wav"))
        found.push_back(entry.name + "@" + std::to_string(entry.mount));
    const std::vector<std::string> expected_found{"Loose.wav@-1", "other.wav@27"};
    check(found == expected_found, "find skips the archive copy a loose file hides");

    // The CD-ROM root scan takes every *.hpi with no limit but rejects paths
    // already in the table, so with the same directory standing in for the
    // disc the archives past the limit follow everything else.
    oa::AssetStore with_disc(root);
    const fs::path disc_roots[]{root};
    int rejected_duplicates = 0;
    for (const auto& outcome : with_disc.discover("31", disc_roots))
        rejected_duplicates += outcome.already_mounted ? 1 : 0;
    check(
        mounted_names(with_disc) == install_order_with_disc(),
        "disc-root scan appends the archives past the limit"
    );
    check(rejected_duplicates == 10, "the disc-root scan rejects the ten archives already mounted");
    check(
        with_disc.read("sounds/untdone.wav").bytes == text("totala3.hpi"),
        "a disc-root archive provides what nothing earlier does"
    );
    check(
        with_disc.read("gamedata/sound.tdf").bytes == text("rev31.gp3"),
        "disc-root archives never outrank earlier mounts"
    );
}

// A named installation archive that only a removable root holds is mounted
// from there, in the list's slot, and the disc scan does not mount it again.
// A name nothing holds is reported missing.
void asset_store_discover_installation_archive_on_disc_root() {
    TempDir dir;
    const auto root = dir.path() / "game";
    const auto disc = dir.path() / "disc";
    fs::create_directories(root);
    fs::create_directories(disc);
    const auto write = [](const fs::path& path, const std::vector<uint8_t>& bytes) {
        std::ofstream(path, std::ios::binary)
            .write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size())
            );
    };
    write(root / "mod.ccx", archive_of({{"maps/shared.ota", text("mod.ccx"), 0}}));
    write(root / "game.hpi", archive_of({{"maps/shared.ota", text("game.hpi"), 0}}));
    write(disc / "Worlds.HPI", archive_of({{"maps/world.ota", text("worlds"), 0}}));

    oa::DiscoveryPlan plan;
    plan.installation_archives = {"worlds.hpi"};
    oa::AssetStore store(root);
    const fs::path disc_roots[]{disc};
    const auto report = store.discover(plan, disc_roots);
    std::vector<std::string> mounted;
    for (const auto& path : store.mount_paths())
        mounted.push_back(path.filename().string());
    const std::vector<std::string> expected{"mod.ccx", "Worlds.HPI", "game.hpi"};
    check(mounted == expected, "a disc-root installation archive mounts in the list's slot, once");
    bool missing = false;
    for (const auto& outcome : report)
        missing = missing || !outcome.error.empty();
    check(!missing, "a disc-root installation archive is not reported missing");
    check(store.read("maps/world.ota").bytes == text("worlds"), "the disc-root archive is read");

    oa::DiscoveryPlan absent;
    absent.installation_archives = {"absent.hpi"};
    oa::AssetStore other(root);
    bool reported = false;
    for (const auto& outcome : other.discover(absent, disc_roots))
        reported = reported || outcome.error == "installation archive 'absent.hpi' is missing";
    check(reported, "a name no folder or root holds is reported missing");
    check(oa::same_archive_file_name("Worlds.HPI", "worlds.hpi"), "names match without case");
    check(!oa::same_archive_file_name("worlds.hpi", "worlds.ufo"), "other names do not match");
}

// The groups discovery scans the game directory in, in scan order.
enum class ArchiveGroup : uint8_t { revision, ccx, ufo, hpi, none };

// The revision patch of the 3.1c game, the one GP3 discovery mounts.
constexpr std::string_view kRevisionPatch = "rev31.gp3";

/// Upper-cases ASCII letters, as Windows compares names.
///
/// @param text a file name
/// @return the name in upper case
std::string upper_ascii(std::string_view text) {
    std::string upper(text);
    for (auto& c : upper)
        if (c >= 'a' && c <= 'z')
            c = static_cast<char>(c - 'a' + 'A');
    return upper;
}

/// Tests whether an upper-cased name ends in an extension after a stem.
///
/// @param upper_name the name, upper-cased
/// @param upper_extension the extension with its dot, upper-cased
/// @return true when the name is longer than the extension and ends in it
bool has_extension(const std::string& upper_name, std::string_view upper_extension) {
    return upper_name.size() > upper_extension.size() &&
           upper_name.compare(
               upper_name.size() - upper_extension.size(), upper_extension.size(), upper_extension
           ) == 0;
}

/// Classifies a file of the game directory by the scan that picks it up.
///
/// @param name the file's name
/// @return its group, or none for a file no scan takes
ArchiveGroup group_of(const std::string& name) {
    const auto upper = upper_ascii(name);
    if (upper == upper_ascii(kRevisionPatch))
        return ArchiveGroup::revision;
    if (has_extension(upper, ".CCX"))
        return ArchiveGroup::ccx;
    if (has_extension(upper, ".UFO"))
        return ArchiveGroup::ufo;
    if (has_extension(upper, ".HPI"))
        return ArchiveGroup::hpi;
    return ArchiveGroup::none;
}

/// Lists the archive files of an install's top directory in discovery's order.
///
/// By group, and within a group in Windows name order (ASCII upper-cased).
///
/// @param root the installation
/// @return the archives' file names
std::vector<std::string> listed_archives(const fs::path& root) {
    std::vector<std::string> names;
    for (const auto& entry : fs::directory_iterator(root))
        if (entry.is_regular_file() &&
            group_of(entry.path().filename().string()) != ArchiveGroup::none)
            names.push_back(entry.path().filename().string());
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) {
        const auto group_a = group_of(a);
        const auto group_b = group_of(b);
        return group_a != group_b ? group_a < group_b : upper_ascii(a) < upper_ascii(b);
    });
    return names;
}

/// Checks the installed game's archives mount by discovery's rules.
///
/// Discovery mounts every archive of the install's top directory by the rules
/// the synthetic layouts pin, all of them with its directory as the disc root.
/// With the 3.1c revision patch present it also shows the data gap behind the
/// silent factory-completion sound: the patch's gamedata/sound.tdf names
/// untdone for every plant, but no shipped archive holds sounds/untdone.wav,
/// so only a loose copy could ever supply it.
///
/// @param root the installation
void asset_store_discover_installed_game(const fs::path& root) {
    const auto listed = listed_archives(root);
    std::vector<std::string> expected;
    std::vector<std::string> past_limit;
    int plain = 0;
    for (const auto& name : listed) {
        if (group_of(name) == ArchiveGroup::hpi &&
            plain == oa::formats::hpi::PlainArchiveMountLimit) {
            past_limit.push_back(name);
            continue;
        }
        plain += group_of(name) == ArchiveGroup::hpi ? 1 : 0;
        expected.push_back(name);
    }
    check(!expected.empty(), "the install's directory holds archives");

    oa::AssetStore store(root);
    int unreadable = 0;
    for (const auto& outcome : store.discover(oa::test::kGameRevision))
        unreadable += outcome.mounted ? 0 : 1;
    check(unreadable == 0, "every archive the install's directory holds opens");
    check(
        mounted_names(store) == expected,
        "the revision patch, the CCX and UFO groups, then at most ten *.HPI, each group in name "
        "order"
    );

    oa::AssetStore with_disc(root);
    const fs::path disc_roots[]{root};
    int rejected_duplicates = 0;
    for (const auto& outcome : with_disc.discover(oa::test::kGameRevision, disc_roots))
        rejected_duplicates += outcome.already_mounted ? 1 : 0;
    auto expected_with_disc = expected;
    expected_with_disc.insert(expected_with_disc.end(), past_limit.begin(), past_limit.end());
    check(
        mounted_names(with_disc) == expected_with_disc,
        "the disc-root scan adds the archives past the limit last"
    );
    check(rejected_duplicates == plain, "the disc-root scan rejects the *.HPI already mounted");

    const auto is_revision_patch = [](const std::string& name) {
        return group_of(name) == ArchiveGroup::revision;
    };
    if (std::none_of(listed.begin(), listed.end(), is_revision_patch))
        return;
    const auto sound_table = with_disc.read("gamedata/sound.tdf");
    check(
        is_revision_patch(sound_table.source.filename().string()),
        "the revision patch provides sound.tdf"
    );
    const std::string table(sound_table.bytes.begin(), sound_table.bytes.end());
    check(table.find("unitcomplete=untdone;") != std::string::npos, "sound.tdf names untdone");
    oa::ResourceFile* file = with_disc.open("sounds\\untdone.wav");
    check(
        file == nullptr || !oa::AssetStore::archived(file), "no shipped archive holds untdone.wav"
    );
    if (file != nullptr)
        oa::AssetStore::close(file);
}

void resource_file_semantics() {
    TempDir dir;
    const Bytes big = pattern_bytes(2 * oa::formats::hpi::BlockBytes + 10);
    const auto path = dir.write(
        "h.hpi",
        archive_of(
            {{"big", big, oa::formats::hpi::CompressionZLib},
             {"raw", text("0123456789"), 0},
             {"zero", {}, 0}}
        )
    );
    fs::create_directory(dir.path() / "loose");
    std::ofstream(dir.path() / "loose" / "empty.txt") << "";
    oa::AssetStore store(dir.path() / "loose");
    store.mount(path);
    auto* file = store.open("big");
    check(file != nullptr && oa::AssetStore::archived(file), "open archive handle");
    check(oa::AssetStore::length(file) == big.size(), "handle length");
    check(
        oa::AssetStore::seek(file, oa::formats::hpi::BlockBytes - 3) == 0,
        "archive seek returns zero"
    );
    Bytes window(8);
    check(oa::AssetStore::read(file, window) == 8, "read across a block boundary");
    check(
        std::equal(window.begin(), window.end(), big.begin() + oa::formats::hpi::BlockBytes - 3),
        "boundary bytes"
    );
    check(
        oa::AssetStore::tell(file) == static_cast<int32_t>(oa::formats::hpi::BlockBytes + 5),
        "tell advances"
    );
    (void)oa::AssetStore::seek(file, 0xFFFFFFFFU);
    check(oa::AssetStore::read(file, window) == 0, "read at end");
    oa::AssetStore::close(file);
    Bytes part(4);
    check(store.read_chunk("raw", 3, part) && part == text("3456"), "read_chunk");
    check(!store.load_file_contents("zero").has_value(), "empty archive entry does not load");
    check(!store.load_file_contents("empty.txt").has_value(), "empty loose file does not load");
    check(store.load_file_contents("RAW") == text("0123456789"), "load_file_contents");
    check(store.file_size("big") == big.size() && store.file_size("missing") == 0, "file_size");
    check(store.open("missing") == nullptr, "open of a missing resource");
    std::vector<int> reported;
    const auto progressed = store.load_with_progress(
        "big",
        [](void* user, uint8_t percent) {
            static_cast<std::vector<int>*>(user)->push_back(percent);
        },
        &reported
    );
    check(progressed == big, "progress load content");
    check(reported == std::vector<int>{9, 18, 27, 36, 45, 54, 63, 72, 81, 90}, "progress steps");
    check(
        throws([&] { (void)store.load_with_progress("missing", nullptr, nullptr); }),
        "missing progress load"
    );
}

/// Links in a loose folder: one that stays inside the folder works, mixed
/// case and spaces included, and still wins over an archive; one that
/// leads outside is not read, so the archive's file is; a link to a folder
/// above it is walked once, and the store keeps working.
void asset_store_loose_links() {
    TempDir dir;
    dir.write("game/Units/Real File.FBI", text("loose"));
    dir.write("game/Units/inside.fbi", text("inside"));
    dir.write("outside/secret.fbi", text("secret"));
    dir.write("outside/maps/far.tnt", text("far"));
    const auto archive =
        dir.write("game.hpi", archive_of({{"units/out.fbi", text("archived"), 0}}));
    std::error_code error;
    const auto game = dir.path() / "game";
    fs::create_symlink("Real File.FBI", game / "Units" / "Alias Name.fbi", error);
    if (!error)
        fs::create_symlink(
            dir.path() / "outside" / "secret.fbi", game / "Units" / "out.fbi", error
        );
    if (!error)
        fs::create_directory_symlink(dir.path() / "outside" / "maps", game / "maps", error);
    if (!error)
        fs::create_directory_symlink(game, game / "Units" / "loop", error);
    if (!error)
        fs::create_directory_symlink(game / "Units", game / "units2", error);
    // Some systems report a link made and make none.
    std::error_code status_error;
    for (const auto& link :
         {game / "Units" / "Alias Name.fbi",
          game / "Units" / "out.fbi",
          game / "maps",
          game / "Units" / "loop",
          game / "units2"})
        if (!error && !fs::is_symlink(fs::symlink_status(link, status_error)))
            error = std::make_error_code(std::errc::no_such_file_or_directory);
    if (error) {
        std::cout << "skipped the loose link cases: " << error.message() << '\n';
        return;
    }
    oa::AssetStore store(game);
    store.mount(archive);
    store.mark_loose_shadows();
    check(store.read("units/alias name.FBI").bytes == text("loose"), "a link inside the folder");
    check(
        store.read("UNITS2/Inside.fbi").bytes == text("inside"), "a folder link inside the folder"
    );
    check(store.read("units/out.fbi").bytes == text("archived"), "a link outside gives way");
    check(!store.loose_file("units/out.fbi").has_value(), "a link outside is no loose file");
    check(
        throws([&] { (void)store.read("maps/far.tnt"); }, "asset not found"),
        "a folder link outside is not read"
    );
    check(store.list_effective("maps", ".tnt").empty(), "a folder link outside is not listed");
    const auto found = store.find("units\\*.fbi", {});
    const auto listed = [&](std::string_view name) {
        return std::any_of(found.begin(), found.end(), [&](const oa::FoundEntry& entry) {
            return entry.name == name && entry.mount < 0;
        });
    };
    check(listed("inside.fbi") && listed("Alias Name.fbi"), "links inside the folder are listed");
    check(store.read("units/loop/units/inside.fbi").bytes == text("inside"), "a loop resolves");
}

/// A loose folder or file that becomes a link leading outside after the
/// store listed it is not read, so the archive's file is, as when it was a
/// link before the listing; a plain file beside it still is.
void asset_store_loose_links_made_after_listing() {
    TempDir dir;
    dir.write("game/maps/near.tnt", text("near"));
    dir.write("game/units/a.fbi", text("a"));
    dir.write("game/units/b.fbi", text("b"));
    dir.write("outside/maps/near.tnt", text("far"));
    dir.write("outside/secret.fbi", text("secret"));
    const auto archive = dir.write("game.hpi", archive_of({{"units/a.fbi", text("archived"), 0}}));
    const auto game = dir.path() / "game";
    oa::AssetStore store(game);
    store.mount(archive);
    check(store.read("maps/near.tnt").bytes == text("near"), "a plain folder is read");
    check(store.read("units/a.fbi").bytes == text("a"), "a plain file wins over the archive");
    std::error_code error;
    fs::remove_all(game / "maps", error);
    if (!error)
        fs::create_directory_symlink(dir.path() / "outside" / "maps", game / "maps", error);
    if (!error)
        fs::remove(game / "units" / "a.fbi", error);
    if (!error)
        fs::create_symlink(dir.path() / "outside" / "secret.fbi", game / "units" / "a.fbi", error);
    // Some systems report a link made and make none.
    std::error_code status_error;
    for (const auto& link : {game / "maps", game / "units" / "a.fbi"})
        if (!error && !fs::is_symlink(fs::symlink_status(link, status_error)))
            error = std::make_error_code(std::errc::no_such_file_or_directory);
    if (error) {
        std::cout << "skipped the loose links made after listing: " << error.message() << '\n';
        return;
    }
    check(
        throws([&] { (void)store.read("maps/near.tnt"); }, "asset not found"),
        "a folder that became a link outside is not read"
    );
    check(
        store.read("units/a.fbi").bytes == text("archived"),
        "a file that became a link outside gives way"
    );
    check(!store.loose_file("units/a.fbi").has_value(), "and is no loose file");
    check(store.read("units/b.fbi").bytes == text("b"), "a plain file beside it is read");
}

/// A file over the entry limit is refused before its buffer is made, by
/// every way the store reads a whole file; one at the limit's size is not.
void asset_store_refuses_files_over_the_entry_limit() {
    TempDir dir;
    const auto over = dir.write("game/over.bin", text("x"));
    std::error_code error;
    fs::resize_file(over, oa::formats::hpi::EntryByteLimit + 1, error);
    if (error) {
        std::cout << "skipped the entry limit case: " << error.message() << '\n';
        return;
    }
    oa::AssetStore store(dir.path() / "game");
    check(!store.load_file_contents("over.bin").has_value(), "load_file_contents over the limit");
    check(
        throws([&] { (void)store.load_with_progress("over.bin", nullptr, nullptr); }, "limit"),
        "load_with_progress over the limit"
    );
    check(throws([&] { (void)store.read("over.bin"); }, "limit"), "read over the limit");
    check(store.file_size("over.bin") == oa::formats::hpi::EntryByteLimit + 1, "its size is known");
    fs::remove(over, error);
}

/// A game folder named in Chinese is mounted once however its path is
/// spelled, and a loose file named in Chinese is listed and read by its UTF-8
/// name, whatever the system's code page.
void asset_store_names_in_any_script() {
    TempDir dir;
    const auto utf8 = [](std::string_view text) {
        return fs::path(std::u8string(text.begin(), text.end()));
    };
    // U+6E38 U+620F, and U+5730 U+56FE.
    const fs::path game = dir.path() / utf8("\xe6\xb8\xb8\xe6\x88\x8f");
    const std::string map = "\xe5\x9c\xb0\xe5\x9b\xbe.ota";
    fs::create_directories(game / "maps");
    {
        std::ofstream stream(game / "maps" / utf8(map), std::ios::binary);
        stream << "m";
    }
    const auto archive = game / "names.hpi";
    {
        const Bytes bytes = archive_of({{"units/x.fbi", text("x"), 0}});
        std::ofstream stream(archive, std::ios::binary);
        stream.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
    }
    oa::AssetStore store(game);
    store.mount(archive);
    std::string refused;
    check(
        !store.try_mount(game / "." / "names.hpi", &refused) && refused == "already mounted",
        "an archive in a folder named in Chinese is mounted once"
    );
    const auto maps = store.list_effective("maps", ".ota");
    check(maps.size() == 1 && maps[0] == "maps/" + map, "a loose file named in Chinese is listed");
    check(store.read("maps/" + map).bytes == text("m"), "and read by the name listed");
}

/// Loose lookups answer from folder listings taken once, until a rescan, and
/// list on every lookup once the listings would exceed the store's limit.
void asset_store_loose_listings() {
    TempDir dir;
    dir.write("game/Units/A.FBI", text("a"));
    dir.write("game/Anims/x.gaf", text("x"));
    oa::AssetStore store(dir.path() / "game");
    check(store.loose_index_enabled(), "a new store keeps folder listings");
    check(
        store.read("units/a.fbi").bytes == text("a"), "a loose file is found through the listings"
    );
    check(store.read("UNITS\\A.fbi").bytes == text("a"), "the listings match names ignoring case");
    dir.write("game/Units/B.FBI", text("b"));
    check(
        throws([&] { (void)store.read("units/b.fbi"); }, "asset not found"),
        "a file added after its folder was listed is not seen"
    );
    store.mark_loose_shadows();
    check(store.read("units/b.fbi").bytes == text("b"), "a rescan lists the folders again");
    check(throws([&] { (void)store.read("units"); }, "asset not found"), "a folder is not a file");
    check(
        throws([&] { (void)store.read("units/a.fbi/x"); }, "asset not found"),
        "a file does not lead on as a folder"
    );
    check(
        throws([&] { (void)store.read("units/../units/a.fbi"); }, "traversal"),
        "a traversing path still fails"
    );

    // Two entries at the top, then two more in Units, exceed a limit of two.
    oa::AssetStore limited(dir.path() / "game", 2);
    check(
        limited.read("units/a.fbi").bytes == text("a"), "a store over its limit still finds files"
    );
    check(!limited.loose_index_enabled(), "listings beyond the limit are dropped");
    dir.write("game/Units/C.FBI", text("c"));
    check(
        limited.read("units/c.fbi").bytes == text("c"),
        "without listings a new file is seen at once"
    );
}

#ifdef _WIN32
/// Makes a folder junction through the file system's own calls, and checks
/// that the system reports the folder as a reparse point.
///
/// @param link the junction's path; nothing may exist there
/// @param target the folder it leads to
/// @return true when the junction was made
bool make_junction(const fs::path& link, const fs::path& target) {
    std::error_code error;
    fs::path leads_to = fs::absolute(target, error);
    if (error || ::CreateDirectoryW(link.c_str(), nullptr) == 0)
        return false;
    leads_to.make_preferred();
    // A mount point's reparse data: the tag, the data's length and a
    // reserved word, then the offsets and lengths in bytes of the target's
    // NT path and of its plain path, then both paths, each ending in a NUL.
    const std::wstring substitute = L"\\??\\" + leads_to.wstring();
    const std::wstring print = leads_to.wstring();
    constexpr std::size_t kHeaderBytes = 8;
    constexpr std::size_t kNamesHeaderBytes = 8;
    const std::size_t substitute_bytes = substitute.size() * sizeof(wchar_t);
    const std::size_t print_bytes = print.size() * sizeof(wchar_t);
    const std::size_t names_bytes = substitute_bytes + print_bytes + 2 * sizeof(wchar_t);
    std::vector<uint8_t> data(kHeaderBytes + kNamesHeaderBytes + names_bytes);
    const auto put16 = [&](std::size_t at, std::size_t value) {
        data[at] = static_cast<uint8_t>(value & 0xFFU);
        data[at + 1] = static_cast<uint8_t>((value >> 8U) & 0xFFU);
    };
    oa::base::bytes::store_le32(data.data(), IO_REPARSE_TAG_MOUNT_POINT);
    put16(4, kNamesHeaderBytes + names_bytes);
    put16(kHeaderBytes, 0);
    put16(kHeaderBytes + 2, substitute_bytes);
    put16(kHeaderBytes + 4, substitute_bytes + sizeof(wchar_t));
    put16(kHeaderBytes + 6, print_bytes);
    uint8_t* const names = data.data() + kHeaderBytes + kNamesHeaderBytes;
    std::memcpy(names, substitute.c_str(), substitute_bytes + sizeof(wchar_t));
    std::memcpy(
        names + substitute_bytes + sizeof(wchar_t), print.c_str(), print_bytes + sizeof(wchar_t)
    );
    const HANDLE handle = ::CreateFileW(
        link.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OPEN_REPARSE_POINT,
        nullptr
    );
    bool made = false;
    if (handle != INVALID_HANDLE_VALUE) {
        DWORD returned = 0;
        made = ::DeviceIoControl(
                   handle,
                   FSCTL_SET_REPARSE_POINT,
                   data.data(),
                   static_cast<DWORD>(data.size()),
                   nullptr,
                   0,
                   &returned,
                   nullptr
               ) != 0;
        ::CloseHandle(handle);
    }
    // Some systems accept the call and make no reparse point.
    const DWORD attributes = ::GetFileAttributesW(link.c_str());
    made = made && attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    if (!made)
        ::RemoveDirectoryW(link.c_str());
    return made;
}
#endif

/// A folder junction that leads outside the loose folder is not read, one
/// that stays inside is, and a folder that becomes a junction leading
/// outside after the store listed it is not read, on systems that make
/// junctions.
void asset_store_loose_junctions() {
#ifdef _WIN32
    TempDir dir;
    dir.write("game/Units/inside.fbi", text("inside"));
    dir.write("game/sounds/near.wav", text("near"));
    dir.write("outside/maps/far.tnt", text("far"));
    dir.write("outside/sounds/near.wav", text("far"));
    const auto game = dir.path() / "game";
    // Each junction is removed before the folder, so that removing the
    // folder never walks into what a junction leads to.
    std::vector<fs::path> junctions;
    const auto junction = [&](const fs::path& link, const fs::path& target) {
        if (!make_junction(link, target))
            return false;
        junctions.push_back(link);
        return true;
    };
    const auto remove_junctions = [&] {
        for (const auto& made : junctions)
            ::RemoveDirectoryW(made.c_str());
    };
    if (!junction(game / "maps", dir.path() / "outside" / "maps") ||
        !junction(game / "units2", game / "Units")) {
        std::cout << "skipped the loose junction cases: no junction made\n";
        remove_junctions();
        return;
    }
    oa::AssetStore store(game);
    check(
        throws([&] { (void)store.read("maps/far.tnt"); }, "asset not found"),
        "a junction outside is not read"
    );
    check(store.read("units2/inside.fbi").bytes == text("inside"), "a junction inside is read");
    check(store.read("units/inside.fbi").bytes == text("inside"), "a plain folder is read");
    check(
        store.read("sounds/near.wav").bytes == text("near"), "a folder is read before it changes"
    );
    std::error_code error;
    fs::remove_all(game / "sounds", error);
    if (error || !junction(game / "sounds", dir.path() / "outside" / "sounds")) {
        std::cout << "skipped the junction made after listing: no junction made\n";
        remove_junctions();
        return;
    }
    check(
        throws([&] { (void)store.read("sounds/near.wav"); }, "asset not found"),
        "a folder that became a junction outside is not read"
    );
    remove_junctions();
#endif
}

/// Folds a resource path as the store's listings do: '\\' as '/', empty
/// parts and a trailing separator dropped, ASCII letters in lower case.
///
/// @param path the path
/// @return the folded path
std::string folded_path(std::string_view path) {
    std::string result;
    bool previous_slash = true;
    for (const char raw : path) {
        const char ch = raw == '\\' ? '/' : raw;
        if (ch == '/') {
            if (!previous_slash)
                result.push_back('/');
            previous_slash = true;
            continue;
        }
        result.push_back(ch >= 'A' && ch <= 'Z' ? static_cast<char>(ch + ('a' - 'A')) : ch);
        previous_slash = false;
    }
    if (!result.empty() && result.back() == '/')
        result.pop_back();
    return result;
}

/// Lists what a store with no loose files lists from its archives, by
/// filtering every file record each archive holds.
///
/// @param store the store; its folders hold no files and it has no pack layer
/// @param directory '/'-separated directory
/// @param extension required suffix, or empty for any
/// @param recursive true to take files below subfolders too
/// @return the folded names in mount and stored order, each once
std::vector<std::string> listed_by_full_scan(
    const oa::AssetStore& store,
    std::string_view directory,
    std::string_view extension,
    bool recursive
) {
    auto prefix = folded_path(directory);
    if (!prefix.empty())
        prefix += '/';
    const auto suffix = folded_path(extension);
    std::vector<std::string> result;
    for (std::size_t mount = 0; mount < store.mount_paths().size(); ++mount)
        for (const auto& entry : store.mounted(mount).entries()) {
            const auto key = folded_path(entry.path);
            if (key.starts_with(prefix) &&
                (recursive || key.find('/', prefix.size()) == std::string::npos) &&
                (suffix.empty() || key.ends_with(suffix)) &&
                std::find(result.begin(), result.end(), key) == result.end())
                result.push_back(key);
        }
    return result;
}

/// Checks that a store lists a directory as the full scan of its archives does.
///
/// @param store the store; its folders hold no files and it has no pack layer
/// @param directory '/'-separated directory
/// @param extension required suffix, or empty for any
void check_listing_matches_full_scan(
    const oa::AssetStore& store, std::string_view directory, std::string_view extension
) {
    const std::string what = "'" + std::string(directory) + "' " + std::string(extension);
    check(
        store.list_effective_in_mount_order(directory, extension) ==
            listed_by_full_scan(store, directory, extension, false),
        "listing " + what + " matches the full scan"
    );
    check(
        store.list_effective_recursive(directory, extension) ==
            listed_by_full_scan(store, directory, extension, true),
        "recursive listing " + what + " matches the full scan"
    );
}

/// Replaces the one occurrence of a name in an archive's bytes.
///
/// @param[in,out] bytes the archive, written with no header key
/// @param from the name written
/// @param to the name it becomes, of the same length
void rename_record(Bytes& bytes, std::string_view from, std::string_view to) {
    const auto at = std::search(bytes.begin(), bytes.end(), from.begin(), from.end());
    check(at != bytes.end(), "the archive holds the name " + std::string(from));
    if (at != bytes.end())
        std::copy(to.begin(), to.end(), at);
}

/// A listing takes every record entries() lists under the directory, also
/// those of a second folder of the same name, a lookup cannot reach, and a
/// root record whose name holds a separator.
void asset_store_lists_archive_folders() {
    TempDir dir;
    auto bytes = archive_of(
        {{"guis/a.gui", text("1"), 0},
         {"guis/sub/d.gui", text("2"), 0},
         {"guis/e.txt", text("3"), 0},
         {"gui2/b.gui", text("4"), 0},
         {"gui2/deep/f.gui", text("5"), 0},
         {"other/g.gui", text("6"), 0},
         {"guisxc.gui", text("7"), 0},
         {"h.gui", text("8"), 0}}
    );
    rename_record(bytes, "gui2", "GUIS");
    rename_record(bytes, "guisxc.gui", "guis\\c.gui");
    const auto second = archive_of({{"GUIS/A.GUI", text("9"), 0}, {"guis/i.gui", text("0"), 0}});
    fs::create_directory(dir.path() / "loose");
    oa::AssetStore store(dir.path() / "loose");
    store.mount(dir.write("first.hpi", bytes));
    store.mount(dir.write("second.hpi", second));
    const auto guis = store.list_effective_in_mount_order("guis", ".gui");
    for (const auto* name : {"guis/a.gui", "guis/b.gui", "guis/c.gui", "guis/i.gui"})
        check(
            std::find(guis.begin(), guis.end(), name) != guis.end(),
            std::string("the listing of guis takes ") + name
        );
    check(guis.size() == 4, "the listing of guis takes each name once");
    for (const auto* directory : {"", "guis", "GUIS", "guis/sub", "gui2", "other", "missing"})
        for (const auto* extension : {"", ".gui", ".TXT"})
            check_listing_matches_full_scan(store, directory, extension);
}

/// Resolves a path by walking an archive's records part by part: in each
/// directory the last record whose name matches ignoring ASCII case, a file
/// before the last part ending the walk.
///
/// @param archive the archive
/// @param path '\\'- or '/'-separated path
/// @param stop_before_last true to resolve the directory of the last part
/// @return the node index, or nullopt
std::optional<uint32_t>
walked_lookup(const oa::HpiArchive& archive, std::string_view path, bool stop_before_last) {
    const auto nodes = archive.nodes();
    const auto upper = [](char c) {
        return c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A')) : c;
    };
    const auto same = [&](std::string_view a, std::string_view b) {
        return a.size() == b.size() &&
               std::equal(a.begin(), a.end(), b.begin(), [&](char x, char y) {
                   return upper(x) == upper(y);
               });
    };
    uint32_t directory = 0;
    std::size_t begin = 0;
    for (;;) {
        std::size_t end = begin;
        while (end < path.size() && path[end] != '\\' && path[end] != '/')
            ++end;
        const bool last = end == path.size();
        if (last && stop_before_last)
            return directory;
        const auto part = path.substr(begin, end - begin);
        const auto& parent = nodes[directory];
        std::optional<uint32_t> match;
        for (uint32_t i = parent.child_count; i-- > 0;)
            if (same(nodes[parent.first_child + i].name, part)) {
                match = parent.first_child + i;
                break;
            }
        if (!match || last)
            return match;
        if (!nodes[*match].directory())
            return std::nullopt;
        directory = *match;
        begin = end + 1;
    }
}

/// Checks that an archive resolves paths as a walk of its records does.
///
/// @param archive the archive
/// @param paths the paths looked up
void check_lookups_match_walk(
    const oa::HpiArchive& archive, const std::vector<std::string>& paths
) {
    for (const auto& path : paths) {
        check(archive.lookup(path) == walked_lookup(archive, path, false), "lookup of " + path);
        check(
            archive.lookup_directory(path) == walked_lookup(archive, path, true),
            "directory lookup of " + path
        );
    }
}

/// Lookups take the last record of a name, end at a file before the last
/// part, never match a name that holds a separator, and match empty parts
/// only to empty names, as a walk of the records does.
void hpi_lookups_follow_the_records() {
    TempDir dir;
    auto bytes = archive_of(
        {{"guis/a.gui", text("1"), 0},
         {"gui2/a.gui", text("2"), 0},
         {"gui2/deep/b.gui", text("3"), 0},
         {"units/x.fbi", text("4"), 0},
         {"UNITY", text("5"), 0},
         {"subxc.gui", text("6"), 0},
         {"q/zz/e", text("7"), 0},
         {"plain.txt", text("8"), 0}}
    );
    rename_record(bytes, "gui2", "GUIS");
    rename_record(bytes, "UNITY", "UNITS");
    rename_record(bytes, "subxc.gui", "sub\\c.gui");
    rename_record(bytes, "zz", std::string_view("\0z", 2));
    const auto archive = opened(dir.write("lookups.hpi", bytes));
    std::vector<std::string> paths{
        "",
        "/",
        "guis",
        "GUIS/A.GUI",
        "guis\\a.gui",
        "guis/deep",
        "guis/deep/b.gui",
        "gui2/a.gui",
        "units/x.fbi",
        "units",
        "units/",
        "sub/c.gui",
        "sub\\c.gui",
        "q",
        "q/",
        "q//e",
        "q\\\\e",
        "q/z/e",
        "plain.txt",
        "plain.txt/x",
        "missing",
        "missing/x",
        "guis/",
        "/guis",
        "PLAIN.TXT"
    };
    for (const auto& entry : archive.entries())
        paths.push_back(entry.path);
    check_lookups_match_walk(archive, paths);
    check(archive.read("guis/a.gui").value == text("2"), "the last folder of a name is read");
    check(archive.read("q//e").value == text("7"), "an empty part matches an empty name");
    check(!archive.lookup("units/x.fbi"), "a file of a folder's name ends the lookup");
    check(!archive.lookup("sub/c.gui"), "a name holding a separator is not a folder");
}

/// A directory of many records keeps the last record of each name ignoring
/// ASCII case, as a walk of the records does, and opens in far less time
/// than a cost growing with the square of its records would take.
void hpi_large_directory_keeps_the_last_of_each_name() {
    TempDir dir;
    constexpr int kNames = 40'000;
    // Every name in lower case, then in upper case, then every even one in
    // mixed case: the last spelling of each name holds its round.
    std::vector<oa::HpiWriteFile> files;
    files.reserve(3 * kNames);
    for (int round = 0; round < 3; ++round)
        for (int number = 0; number < kNames; ++number) {
            if (round == 2 && number % 2 != 0)
                continue;
            const std::string digits = std::to_string(number);
            const std::string name = round == 0   ? "unit" + digits + ".fbi"
                                     : round == 1 ? "UNIT" + digits + ".FBI"
                                                  : "Unit" + digits + ".Fbi";
            files.push_back({name, text(std::to_string(round)), 0});
        }
    const auto path = dir.write("large.hpi", archive_of(files));
    const auto start = std::chrono::steady_clock::now();
    const auto archive = opened(path);
    const auto took = std::chrono::steady_clock::now() - start;
    check(archive.entries().size() == files.size(), "a large directory keeps every record");
    check(took < std::chrono::seconds(10), "a directory of 100000 records opens quickly");
    std::vector<std::string> paths;
    for (int number = 0; number < kNames; number += 997)
        for (const int sample : {number, number + 1})
            paths.push_back("unit" + std::to_string(sample) + ".FBI");
    check_lookups_match_walk(archive, paths);
    check(archive.read("UNIT0.fbi").value == text("2"), "the mixed-case last record is read");
    check(archive.read("unit1.fbi").value == text("1"), "the upper-case last record is read");
}

/// Every directory of the installed game's archives lists as the full scan
/// of the archives does.
///
/// @param mounts the archives, in mount order
void asset_store_lists_installed_folders(std::span<const fs::path> mounts) {
    TempDir dir;
    fs::create_directory(dir.path() / "loose");
    oa::AssetStore store(dir.path() / "loose");
    for (const auto& mount : mounts)
        store.mount(mount);
    for (const auto* directory :
         {"", "guis", "units", "anims", "gamedata", "features", "features/corpses", "scripts"})
        check_listing_matches_full_scan(store, directory, "");
    check_listing_matches_full_scan(store, "guis", ".gui");
    check_listing_matches_full_scan(store, "units", ".fbi");
    for (std::size_t mount = 0; mount < store.mount_paths().size(); ++mount) {
        const auto& archive = store.mounted(mount);
        std::vector<std::string> paths;
        for (const auto& entry : archive.entries()) {
            paths.push_back(entry.path);
            std::string upper = entry.path;
            for (auto& c : upper)
                c = c == '/' ? '\\'
                             : (c >= 'a' && c <= 'z' ? static_cast<char>(c - ('a' - 'A')) : c);
            paths.push_back(upper);
            paths.push_back(entry.path + "/x");
        }
        check_lookups_match_walk(archive, paths);
    }
}

} // namespace

int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv)) {
        const auto root = oa::test::require_game_directory("the installed game's archive order");
        try {
            asset_store_discover_installed_game(root);
            oa::AssetStore installed(root);
            (void)installed.discover(oa::test::kGameRevision);
            asset_store_lists_installed_folders(installed.mount_paths());
        } catch (const std::exception& error) {
            std::cerr << "unexpected exception: " << error.what() << '\n';
            return 1;
        }
        if (failures != 0) {
            std::cerr << failures << " HPI install check(s) failed\n";
            return 1;
        }
        std::cout << "hpi install tests passed\n";
        return 0;
    }
    try {
        hpi_writer_round_trip();
        hpi_requires_copyright_trailer_any_year();
        hpi_version_must_be_one();
        hpi_header_key_ff_disables_decryption();
        hpi_lookup_last_duplicate_wins();
        hpi_intermediate_file_ends_lookup();
        hpi_directory_match_falls_through();
        hpi_entry_flag_bit0_is_directory();
        hpi_any_nonzero_compression_is_chunked();
        sqsh_stored_type_is_fatal();
        sqsh_version_byte_ignored();
        sqsh_zlib_stream_must_end_cleanly();
        sqsh_chunk_table_locates_blocks();
        hpi_negative_directory_count_is_empty();
        hpi_rejects_cycles();
        match_wildcard_rules();
        asset_store_find_and_shadowing();
        asset_store_discover_order_and_hpi_limit();
        asset_store_discover_pins_install_layout();
        asset_store_discover_installation_archive_on_disc_root();
        asset_store_loose_listings();
        asset_store_lists_archive_folders();
        hpi_lookups_follow_the_records();
        hpi_large_directory_keeps_the_last_of_each_name();
        asset_store_names_in_any_script();
        asset_store_loose_links();
        asset_store_loose_links_made_after_listing();
        asset_store_loose_junctions();
        asset_store_refuses_files_over_the_entry_limit();
        resource_file_semantics();
    } catch (const std::exception& error) {
        std::cerr << "unexpected exception: " << error.what() << '\n';
        return 1;
    }
    if (failures != 0) {
        std::cerr << failures << " HPI check(s) failed\n";
        return 1;
    }
    std::cout << "hpi tests passed\n";
    return 0;
}
