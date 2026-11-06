// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The .oa-content record: a round trip, a record this reader does not
// understand, a field it skips, whether it names every file of its bundle,
// and the package an expansion was installed from: written, read, and
// compared with the package a catalogue offers.

#include "oa/data/content_bundles/record.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using oa::data::content_bundles::Bundle;
using oa::data::content_bundles::check_installed_package;
using oa::data::content_bundles::ContentFile;
using oa::data::content_bundles::ContentRecord;
using oa::data::content_bundles::ContentRecordFile;
using oa::data::content_bundles::ContentRecordPackage;
using oa::data::content_bundles::files_not_recorded;
using oa::data::content_bundles::PackageCheck;
using oa::data::content_bundles::format_content_record;
using oa::data::content_bundles::parse_content_record;
using oa::data::content_bundles::managed_games_folder;
using oa::data::content_bundles::read_bundle_folder_record;
using oa::data::content_bundles::record_complete;

constexpr std::string_view kHash =
    "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0";

ContentRecord sample() {
    ContentRecord record;
    record.bundle_id = "totala";
    record.source = "line\nslash\\end";
    ContentRecordFile item;
    item.copy_name = "rev31.gp3";
    item.size = 42;
    item.modified = -5;
    item.digest = *oa::base::sha256::parse_hex(kHash);
    record.files.push_back(item);
    ContentRecordFile oldest;
    oldest.copy_name = "totala1.hpi";
    oldest.size = 0;
    oldest.modified = std::numeric_limits<int64_t>::min();
    oldest.digest = *oa::base::sha256::parse_hex(kHash);
    record.files.push_back(std::move(oldest));
    return record;
}

void test_round_trip() {
    const ContentRecord record = sample();
    const std::optional<std::string> text = format_content_record(record);
    OA_CHECK(text.has_value());
    const ContentRecord read = parse_content_record(*text);
    OA_CHECK(read.valid);
    OA_CHECK(!read.present);
    OA_CHECK(read.bundle_id == record.bundle_id);
    OA_CHECK(read.source == record.source);
    OA_CHECK(read.files.size() == record.files.size());
    for (std::size_t index = 0; index < record.files.size(); ++index) {
        OA_CHECK(read.files[index].copy_name == record.files[index].copy_name);
        OA_CHECK(read.files[index].size == record.files[index].size);
        OA_CHECK(read.files[index].modified == record.files[index].modified);
        OA_CHECK(read.files[index].digest == record.files[index].digest);
    }

    ContentRecord bare;
    bare.bundle_id = "totala";
    bare.source = "";
    const std::optional<std::string> empty_files = format_content_record(bare);
    OA_CHECK(empty_files.has_value());
    const ContentRecord parsed_bare = parse_content_record(*empty_files);
    OA_CHECK(parsed_bare.valid);
    OA_CHECK(parsed_bare.files.empty());
    OA_CHECK(parsed_bare.source.empty());

    ContentRecord broken_name = record;
    broken_name.files[0].copy_name = "has\nline";
    OA_CHECK(!format_content_record(broken_name).has_value());
}

void test_malformed() {
    OA_CHECK(!parse_content_record("not a record\n").valid);
    OA_CHECK(!parse_content_record("").valid);
    OA_CHECK(!parse_content_record("oa-content 1\nbundle \nsource disc\n").valid);
    OA_CHECK(!parse_content_record("oa-content 1\nsource disc\nbundle totala\n").valid);
    const std::string truncated =
        "oa-content 1\nbundle totala\nsource disc\nfile rev31.gp3\nsize 1\n";
    OA_CHECK(!parse_content_record(truncated).valid);
    const std::string bad_hash =
        "oa-content 1\nbundle totala\nsource disc\nfile rev31.gp3\nsize 1\nmodified 0\nsha256 "
        "zz\n";
    OA_CHECK(!parse_content_record(bad_hash).valid);
    const std::string bad_escape = "oa-content 1\nbundle totala\nsource bad\\q\n";
    OA_CHECK(!parse_content_record(bad_escape).valid);
    const std::string stray_size = "oa-content 1\nbundle totala\nsource disc\nsize 4\n";
    OA_CHECK(!parse_content_record(stray_size).valid);
}

void test_unknown_field() {
    const std::string text = "oa-content 1\n"
                             "bundle totala\n"
                             "origin later-reader\n"
                             "source disc\n"
                             "file rev31.gp3\n"
                             "size 42\n"
                             "modified -5\n"
                             "sha256 " +
                             std::string(kHash) +
                             "\n"
                             "kept yes\n";
    const ContentRecord read = parse_content_record(text);
    OA_CHECK(read.valid);
    OA_CHECK(read.bundle_id == "totala");
    OA_CHECK(read.source == "disc");
    OA_CHECK(read.files.size() == 1);
    OA_CHECK(read.files[0].copy_name == "rev31.gp3");
    OA_CHECK(read.files[0].size == 42);
    OA_CHECK(read.files[0].modified == -5);
    OA_CHECK(!read.files[0].not_in_source);
}

// "not-in-source" follows a file the source did not hold. A record from
// before that line reads the file as held, and a reader that skips an
// unknown line still has the size and the hash.
void test_not_in_source() {
    const std::string held = "oa-content 1\n"
                             "bundle music\n"
                             "source disc\n"
                             "file music/9.mp3\n"
                             "size 4\n"
                             "modified 3\n"
                             "sha256 " +
                             std::string(kHash) + "\n";
    const ContentRecord older = parse_content_record(held);
    OA_CHECK(older.valid);
    OA_CHECK(older.files.size() == 1);
    OA_CHECK(!older.files[0].not_in_source);
    const std::optional<std::string> held_again = format_content_record(older);
    OA_CHECK(held_again.has_value());
    OA_CHECK(held_again->find("not-in-source") == std::string::npos);

    ContentRecord record = older;
    ContentRecordFile absent;
    absent.copy_name = "music/13.mp3";
    absent.size = 0;
    absent.modified = 0;
    absent.digest = *oa::base::sha256::parse_hex(kHash);
    absent.not_in_source = true;
    record.files.push_back(absent);
    const std::optional<std::string> text = format_content_record(record);
    OA_CHECK(text.has_value());
    OA_CHECK(text->find("not-in-source\n") != std::string::npos);
    const ContentRecord read = parse_content_record(*text);
    OA_CHECK(read.valid);
    OA_CHECK(read.files.size() == 2);
    OA_CHECK(!read.files[0].not_in_source);
    OA_CHECK(read.files[1].copy_name == "music/13.mp3");
    OA_CHECK(read.files[1].not_in_source);
    OA_CHECK(read.files[1].digest == absent.digest);
    OA_CHECK(read.files[1].source_size == 0);
    OA_CHECK(text->find("source-size") == std::string::npos);

    // A file the source held at another size keeps that size after
    // "not-in-source"; a line out of place, or not a number, is refused.
    ContentRecord sized = record;
    sized.files[1].source_size = 41000000;
    const std::optional<std::string> sized_text = format_content_record(sized);
    OA_CHECK(sized_text.has_value());
    OA_CHECK(sized_text->find("not-in-source\nsource-size 41000000\n") != std::string::npos);
    const ContentRecord sized_read = parse_content_record(*sized_text);
    OA_CHECK(sized_read.valid);
    OA_CHECK(sized_read.files.size() == 2 && sized_read.files[1].source_size == 41000000);
    OA_CHECK(sized_read.files.size() == 2 && sized_read.files[0].source_size == 0);
    const std::string held_sized = held + "source-size 7\n";
    const ContentRecord stray = parse_content_record(held_sized);
    OA_CHECK(stray.valid);
    OA_CHECK(stray.files.size() == 1 && stray.files[0].source_size == 0);
    OA_CHECK(!parse_content_record(held + "not-in-source\nsource-size many\n").valid);
}

// A copy stores a record after each file. While the next file is still being
// written, the record names only the files already stored. A bundle with no
// file stored yet is the header alone; the copy does not write that file.
void test_in_progress() {
    const std::string text = "oa-content 1\n"
                             "bundle totala\n"
                             "source /games/Total Annihilation\n"
                             "file rev31.gp3\n"
                             "size 14\n"
                             "modified 1600000000\n"
                             "sha256 " +
                             std::string(kHash) + "\n";
    const ContentRecord read = parse_content_record(text);
    OA_CHECK(read.valid);
    OA_CHECK(read.bundle_id == "totala");
    OA_CHECK(read.source == "/games/Total Annihilation");
    OA_CHECK(read.files.size() == 1);
    OA_CHECK(read.files[0].copy_name == "rev31.gp3");
    OA_CHECK(read.files[0].size == 14);
    OA_CHECK(read.files[0].modified == 1600000000);
    const std::optional<std::string> again = format_content_record(read);
    OA_CHECK(again.has_value());
    OA_CHECK(*again == text);

    const ContentRecord header = parse_content_record("oa-content 1\nbundle totala\nsource disc\n");
    OA_CHECK(header.valid);
    OA_CHECK(header.files.empty());
    OA_CHECK(header.bundle_id == "totala");
    OA_CHECK(header.source == "disc");
}

// A bundle is played once its record names every file, copied or not in
// the source, in any case. A copy that stopped after its first file names
// that file alone.
void test_complete() {
    static constexpr std::string_view rev_sources[]{"rev31.gp3"};
    static constexpr std::string_view zrb_sources[]{"Data/2.zrb"};
    static constexpr std::string_view hpi_sources[]{"totala1.hpi"};
    static const ContentFile files[]{
        {"rev31.gp3", rev_sources, 14, {}},
        {"Data/2.zrb", zrb_sources, 12, {}},
        {"totala1.hpi", hpi_sources, 9, {}},
    };
    const Bundle bundle{
        "totala", "Total Annihilation", oa::data::content_bundles::Kind::base_game, {}, {}, files
    };
    ContentRecord record;
    record.valid = true;
    record.bundle_id = "totala";
    ContentRecordFile stored;
    stored.copy_name = "REV31.GP3";
    record.files.push_back(stored);
    const std::vector<std::string_view> stopped = files_not_recorded(bundle, record);
    OA_CHECK((stopped == std::vector<std::string_view>{"Data/2.zrb", "totala1.hpi"}));
    OA_CHECK(!record_complete(bundle, record));

    ContentRecordFile absent;
    absent.copy_name = "totala1.hpi";
    absent.not_in_source = true;
    record.files.push_back(absent);
    ContentRecordFile movie;
    movie.copy_name = "data/2.ZRB";
    record.files.push_back(movie);
    OA_CHECK(files_not_recorded(bundle, record).empty());
    OA_CHECK(record_complete(bundle, record));

    ContentRecord other = record;
    other.bundle_id = "campaign";
    OA_CHECK(!record_complete(bundle, other));
    ContentRecord unread = record;
    unread.valid = false;
    OA_CHECK(!record_complete(bundle, unread));
    const Bundle empty{
        "totala", "Total Annihilation", oa::data::content_bundles::Kind::base_game, {}, {}, {}
    };
    OA_CHECK(files_not_recorded(empty, record).empty());
    OA_CHECK(!record_complete(empty, record));
}

void test_folder() {
    const std::filesystem::path folder = oa::test::make_scratch_directory("G25-content-record");
    std::error_code failure;
    std::filesystem::remove_all(folder, failure);
    std::filesystem::create_directories(folder, failure);
    OA_CHECK(!failure);
    const ContentRecord absent_tree =
        read_bundle_folder_record(folder / std::string(managed_games_folder) / "totala", nullptr);
    OA_CHECK(!absent_tree.present);
    OA_CHECK(!absent_tree.valid);
    OA_CHECK(!absent_tree.io_error);
    const ContentRecord missing = read_bundle_folder_record(folder, nullptr);
    OA_CHECK(!missing.present);
    OA_CHECK(!missing.valid);

    const ContentRecord record = sample();
    const std::optional<std::string> text = format_content_record(record);
    OA_CHECK(text.has_value());
    const std::filesystem::path path =
        folder / std::string(oa::data::content_bundles::content_record_name);
    std::ofstream out(path, std::ios::binary);
    out << *text;
    out.close();
    std::string error = "left";
    const ContentRecord read = read_bundle_folder_record(folder, &error);
    OA_CHECK(error.empty());
    OA_CHECK(read.present);
    OA_CHECK(read.valid);
    OA_CHECK(!read.io_error);
    OA_CHECK(read.bundle_id == "totala");
    OA_CHECK(read.files.size() == 2);
    OA_CHECK(read.source == record.source);
    std::filesystem::remove_all(folder, failure);
}

// An expansion installed from a package records that package's SHA-256 and
// release after its source. A record of any other bundle holds no such line.
// A line this reader cannot use is skipped and the record stays valid, with
// no package. The record decides what an entry fetches: a missing or
// incomplete copy, and a copy whose record names no package or another one,
// fetch the catalogue's package; a record that names it fetches nothing.
void test_package() {
    constexpr std::string_view package_hex =
        "5a2be6e3a4a9b2d1f0e9c8b7a6958473625140fedcba98765432100123456789";
    constexpr std::string_view other_hex =
        "0000000000000000000000000000000000000000000000000000000000000001";
    const auto package_digest = *oa::base::sha256::parse_hex(package_hex);
    const auto other_digest = *oa::base::sha256::parse_hex(other_hex);

    ContentRecord record;
    record.bundle_id = "boneyards";
    record.source = "registry coreprime";
    ContentRecordPackage package;
    package.sha256 = package_digest;
    package.release = 1;
    record.package = package;
    ContentRecordFile archive;
    archive.copy_name = "expansion.ccx";
    archive.size = 1199000;
    archive.modified = 1793909917;
    archive.digest = *oa::base::sha256::parse_hex(kHash);
    record.files.push_back(archive);
    ContentRecordFile page = archive;
    page.copy_name = "Boneyards/Help/Help.def";
    page.size = 3831;
    record.files.push_back(page);
    const std::optional<std::string> text = format_content_record(record);
    OA_CHECK(text.has_value());
    if (!text)
        return;
    const std::string expected_head = "oa-content 1\n"
                                      "bundle boneyards\n"
                                      "source registry coreprime\n"
                                      "package " +
                                      std::string(package_hex) +
                                      " 1\n"
                                      "file expansion.ccx\n";
    OA_CHECK(text->rfind(expected_head, 0) == 0);
    const ContentRecord read = parse_content_record(*text);
    OA_CHECK(read.valid);
    OA_CHECK(read.package.has_value());
    if (read.package) {
        OA_CHECK(read.package->sha256 == package_digest);
        OA_CHECK(read.package->release == 1);
    }
    OA_CHECK(read.files.size() == 2);
    const std::optional<std::string> again = format_content_record(read);
    OA_CHECK(again.has_value() && *again == *text);

    // Another bundle's record is written as before, with no package line.
    const std::optional<std::string> base_text = format_content_record(sample());
    OA_CHECK(base_text.has_value() && base_text->find("package") == std::string::npos);
    OA_CHECK(!parse_content_record(*base_text).package.has_value());

    // A record from a build before the line, and lines this reader cannot use.
    const std::string head = "oa-content 1\nbundle boneyards\nsource registry coreprime\n";
    const std::string files_text =
        "file expansion.ccx\nsize 4\nmodified 0\nsha256 " + std::string(kHash) + "\n";
    const ContentRecord older = parse_content_record(head + files_text);
    OA_CHECK(older.valid && !older.package.has_value());
    const std::string hex(package_hex);
    const std::vector<std::string> unusable{
        "package " + hex + "\n",
        "package zz 1\n",
        "package " + hex + " many\n",
        "package " + hex + " -1\n",
        "package " + hex + " 2147483648\n",
    };
    for (const std::string& bad : unusable) {
        const ContentRecord skipped = parse_content_record(head + bad + files_text);
        OA_CHECK(skipped.valid);
        OA_CHECK(!skipped.package.has_value());
        OA_CHECK(skipped.files.size() == 1);
    }
    const ContentRecord first_kept = parse_content_record(
        head + "package " + std::string(package_hex) + " 3\npackage " + std::string(other_hex) +
        " 4\n" + files_text
    );
    OA_CHECK(first_kept.package.has_value() && first_kept.package->release == 3);
    const ContentRecord before_source = parse_content_record(
        "oa-content 1\nbundle boneyards\npackage " + std::string(package_hex) +
        " 1\nsource registry coreprime\n" + files_text
    );
    OA_CHECK(before_source.valid && !before_source.package.has_value());
    ContentRecord unwritable = record;
    unwritable.package->release = -1;
    OA_CHECK(!format_content_record(unwritable).has_value());

    // The decisions, over a two-file expansion shaped like the Boneyards.
    static constexpr std::string_view archive_sources[]{"expansion.ccx"};
    static constexpr std::string_view page_sources[]{"Boneyards/Help/Help.def"};
    static const ContentFile files[]{
        {"expansion.ccx", archive_sources, 1199000, {}},
        {"Boneyards/Help/Help.def", page_sources, 3831, {}},
    };
    static constexpr std::string_view extends[]{"totala"};
    const Bundle bundle{
        "boneyards", "The Boneyards", oa::data::content_bundles::Kind::expansion, extends, {}, files
    };
    OA_CHECK(check_installed_package(bundle, read, package_digest) == PackageCheck::current);
    OA_CHECK(check_installed_package(bundle, read, other_digest) == PackageCheck::changed);
    ContentRecord unrecorded = read;
    unrecorded.package.reset();
    OA_CHECK(
        check_installed_package(bundle, unrecorded, package_digest) == PackageCheck::unrecorded
    );
    OA_CHECK(
        check_installed_package(bundle, ContentRecord{}, package_digest) == PackageCheck::missing
    );
    ContentRecord stopped = read;
    stopped.files.pop_back();
    OA_CHECK(check_installed_package(bundle, stopped, package_digest) == PackageCheck::missing);
    ContentRecord other_bundle = read;
    other_bundle.bundle_id = "cavedog-units";
    OA_CHECK(
        check_installed_package(bundle, other_bundle, package_digest) == PackageCheck::missing
    );
}

} // namespace

int main() {
    test_round_trip();
    test_malformed();
    test_unknown_field();
    test_not_in_source();
    test_in_progress();
    test_complete();
    test_folder();
    test_package();
    return oa::test::check_exit_status();
}
