// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The managed copy over a fixture install: a stopped copy continues, the
// source keeps its bytes and times, a Wineskin drive_c is preferred to a
// decoy of the same size, and a run nobody is watching does not copy. A
// base game whose copy stopped is not taken as copied. A movie the source
// holds at another size is left out and named; an archive of a base game,
// or of an expansion the source holds, at another size stops the copy
// before anything is copied.
#include "oa/app/content_copy.hpp"
#include "oa/base/sha256.hpp"
#include "oa/platform/file_clone.hpp"
#include "oa/platform/files.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <limits>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {
namespace fs = std::filesystem;

struct SourceStamp {
    fs::path path;
    uintmax_t size{};
    fs::file_time_type modified{};
    std::string bytes;
};

struct MidwayStop {
    std::atomic<bool>* flag{};
    uint64_t past{};
};

std::string read_bytes(const fs::path& path) {
    std::FILE* opened = oa::platform::open_file(path, "rb");
    if (opened == nullptr)
        return {};
    std::string bytes;
    char buffer[256];
    while (true) {
        const std::size_t got = std::fread(buffer, 1, sizeof buffer, opened);
        bytes.append(buffer, got);
        if (got < sizeof buffer)
            break;
    }
    std::fclose(opened);
    return bytes;
}

void place_file(const fs::path& path, std::string_view bytes) {
    const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
    std::string error;
    const bool wrote =
        oa::platform::replace_file(path, std::span<const uint8_t>(data, bytes.size()), &error);
    OA_CHECK(wrote);
}

oa::base::sha256::Digest digest_of(std::string_view bytes) {
    const auto* data = reinterpret_cast<const uint8_t*>(bytes.data());
    return oa::base::sha256::digest_of(std::span<const uint8_t>(data, bytes.size()));
}

void write_cased_install(const fs::path& install) {
    const auto files = oa::app::copy_check_files();
    place_file(install / "REV31.GP3", files[0].bytes);
    place_file(install / "data" / "2.ZRB", files[1].bytes);
    place_file(install / "totala4.hpi", files[2].bytes);
    place_file(install / "notes.txt", "leave-this\n");
}

void each_regular_file(const fs::path& root, void (*visit)(const fs::path&)) {
    std::error_code failure;
    fs::recursive_directory_iterator cursor(
        root, fs::directory_options::skip_permission_denied, failure
    );
    OA_CHECK(!failure);
    const fs::recursive_directory_iterator end;
    while (cursor != end) {
        const fs::path child = cursor->path();
        std::error_code status_error;
        const bool regular = cursor->is_regular_file(status_error);
        cursor.increment(failure);
        if (!status_error && regular)
            visit(child);
        OA_CHECK(!failure);
    }
}

void lock_one(const fs::path& path) {
    std::error_code failure;
    fs::permissions(path, fs::perms::owner_read, fs::perm_options::replace, failure);
}

void unlock_one(const fs::path& path) {
    std::error_code failure;
    fs::permissions(path, fs::perms::owner_all, fs::perm_options::add, failure);
}

std::vector<SourceStamp> stamp_files(const fs::path& root) {
    std::vector<SourceStamp> stamps;
    std::error_code failure;
    fs::recursive_directory_iterator cursor(
        root, fs::directory_options::skip_permission_denied, failure
    );
    OA_CHECK(!failure);
    const fs::recursive_directory_iterator end;
    while (cursor != end) {
        const fs::path child = cursor->path();
        std::error_code status_error;
        const bool regular = cursor->is_regular_file(status_error);
        cursor.increment(failure);
        OA_CHECK(!failure);
        if (status_error || !regular)
            continue;
        SourceStamp stamp;
        stamp.path = child;
        stamp.size = fs::file_size(child, failure);
        OA_CHECK(!failure);
        stamp.modified = fs::last_write_time(child, failure);
        OA_CHECK(!failure);
        stamp.bytes = read_bytes(child);
        stamps.push_back(std::move(stamp));
    }
    return stamps;
}

void check_unchanged(const std::vector<SourceStamp>& stamps) {
    for (const SourceStamp& stamp : stamps) {
        std::error_code failure;
        const auto size = fs::file_size(stamp.path, failure);
        OA_CHECK(!failure);
        OA_CHECK(size == stamp.size);
        const auto modified = fs::last_write_time(stamp.path, failure);
        OA_CHECK(!failure);
        OA_CHECK(modified == stamp.modified);
        OA_CHECK(read_bytes(stamp.path) == stamp.bytes);
    }
}

oa::app::CopyRequest byte_request(const fs::path& user) {
    oa::app::CopyRequest request;
    request.user_folder = user;
    request.bundles = oa::app::copy_check_bundles();
    request.share_blocks = false;
    request.chunk_bytes = 4;
    return request;
}

void stop_past_first_file(void* context, const oa::app::CopyProgress& progress) {
    auto* state = static_cast<MidwayStop*>(context);
    if (state->flag->load())
        return;
    if (!progress.file_finished && progress.bytes_done > state->past &&
        progress.bytes_done < progress.bytes_total)
        state->flag->store(true);
}

void check_copied_tree(const fs::path& user) {
    const auto files = oa::app::copy_check_files();
    const fs::path totala = user / std::string(oa::app::managed_games_folder) / "totala";
    const fs::path campaign =
        user / std::string(oa::app::managed_expansions_folder) / "totala" / "campaign";
    OA_CHECK(read_bytes(totala / "rev31.gp3") == files[0].bytes);
    OA_CHECK(read_bytes(totala / "Data" / "2.zrb") == files[1].bytes);
    OA_CHECK(read_bytes(campaign / "totala4.hpi") == files[2].bytes);
    std::error_code failure;
    OA_CHECK(!fs::exists(totala / "notes.txt", failure));
    OA_CHECK(!fs::exists(totala / "rev31.gp3.part", failure));
    OA_CHECK(!fs::exists(totala / "Data" / "2.zrb.part", failure));
    std::string error;
    const oa::app::ContentRecord base =
        oa::app::read_content_record(totala / std::string(oa::app::content_record_name), &error);
    OA_CHECK(base.valid);
    OA_CHECK(base.bundle_id == "totala");
    bool rev_recorded = false;
    for (const oa::app::ContentRecordFile& item : base.files) {
        if (item.copy_name != "rev31.gp3")
            continue;
        rev_recorded = true;
        OA_CHECK(item.size == files[0].bytes.size());
        OA_CHECK(item.digest == digest_of(files[0].bytes));
    }
    OA_CHECK(rev_recorded);
    const oa::app::ContentRecord expansion =
        oa::app::read_content_record(campaign / std::string(oa::app::content_record_name), &error);
    OA_CHECK(expansion.valid);
    OA_CHECK(expansion.bundle_id == "campaign");
}

void test_decisions() {
    using oa::app::StartCopy;
    OA_CHECK(oa::app::start_copy(true, false, false) == StartCopy::leave_alone);
    OA_CHECK(oa::app::start_copy(true, false, true) == StartCopy::leave_alone);
    OA_CHECK(oa::app::start_copy(true, true, false) == StartCopy::leave_alone);
    OA_CHECK(oa::app::start_copy(true, true, true) == StartCopy::leave_alone);
    OA_CHECK(oa::app::start_copy(false, true, false) == StartCopy::refuse);
    OA_CHECK(oa::app::start_copy(false, true, true) == StartCopy::use_copy);
    OA_CHECK(oa::app::start_copy(false, false, false) == StartCopy::copy);
    OA_CHECK(oa::app::start_copy(false, false, true) == StartCopy::copy);

    constexpr uint64_t limit = std::numeric_limits<uint64_t>::max();
    OA_CHECK(oa::app::bytes_to_make_room_for(0, true) == oa::app::content_copy_margin);
    OA_CHECK(oa::app::bytes_to_make_room_for(100, true) == oa::app::content_copy_margin);
    OA_CHECK(oa::app::bytes_to_make_room_for(100, false) == 100 + oa::app::content_copy_margin);
    OA_CHECK(oa::app::bytes_to_make_room_for(limit, false) == limit);
    OA_CHECK(oa::app::bytes_to_make_room_for(limit - 10, false) == limit);
    OA_CHECK(oa::app::room_is_enough(0, false, 100));
    OA_CHECK(oa::app::room_is_enough(100, true, 100));
    OA_CHECK(!oa::app::room_is_enough(99, true, 100));

    const auto files = oa::app::copy_check_files();
    const auto table = oa::app::copy_check_bundles();
    OA_CHECK(files.size() == 3);
    OA_CHECK(table.size() == 2);
    for (const oa::app::CopyCheckFile& file : files) {
        bool found = false;
        for (const oa::data::content_bundles::Bundle& bundle : table) {
            if (bundle.id != file.bundle_id)
                continue;
            for (const oa::data::content_bundles::ContentFile& item : bundle.files) {
                if (item.copy_name != file.copy_name)
                    continue;
                found = true;
                OA_CHECK(item.size == file.bytes.size());
                OA_CHECK(!item.sha256.empty());
                OA_CHECK(item.sha256.front() == digest_of(file.bytes));
            }
        }
        OA_CHECK(found);
    }
    const fs::path user = "player";
    const fs::path base = oa::app::bundle_folder(user, table[0]);
    OA_CHECK(base == user / std::string(oa::app::managed_games_folder) / "totala");
    const fs::path expansion = oa::app::bundle_folder(user, table[1]);
    OA_CHECK(
        expansion == user / std::string(oa::app::managed_expansions_folder) / "totala" / "campaign"
    );
}

void test_resume(const fs::path& scratch) {
    const fs::path install = scratch / "install";
    const fs::path user = scratch / "user";
    write_cased_install(install);
    each_regular_file(install, lock_one);
    const std::vector<SourceStamp> stamps = stamp_files(install);
    oa::app::FolderContentSource source(install, oa::app::copy_check_bundles());
    oa::app::SourceDescription described;
    std::string error;
    const bool described_ok =
        oa::app::describe_source(source, oa::app::copy_check_bundles(), described, &error);
    OA_CHECK(described_ok);
    OA_CHECK(described.bundles.size() == 2);
    OA_CHECK(described.matched.size() == 3);
    OA_CHECK(described.missing.empty());
    const auto files = oa::app::copy_check_files();
    const uint64_t total = static_cast<uint64_t>(files[0].bytes.size()) +
                           static_cast<uint64_t>(files[1].bytes.size()) +
                           static_cast<uint64_t>(files[2].bytes.size());
    OA_CHECK(described.bytes_to_copy == total);

    std::atomic<bool> stop{false};
    MidwayStop midway;
    midway.flag = &stop;
    midway.past = static_cast<uint64_t>(files[0].bytes.size());
    oa::app::CopyRequest request = byte_request(user);
    request.stop = &stop;
    request.on_progress = stop_past_first_file;
    request.progress_context = &midway;
    const oa::app::CopyResult first = oa::app::copy_content(source, request);
    OA_CHECK(first.status == oa::app::CopyStatus::stopped);
    OA_CHECK(first.files_copied == 1);
    // The record names rev31.gp3 alone. The base game cannot be played yet,
    // so the next start offers the copy again rather than taking it as made.
    OA_CHECK(!oa::app::managed_base_present(user, oa::app::copy_check_bundles()));
    const fs::path partial =
        user / std::string(oa::app::managed_games_folder) / "totala" / "Data" / "2.zrb.part";
    std::error_code failure;
    OA_CHECK(fs::is_regular_file(partial, failure));
    const auto partial_raw = fs::file_size(partial, failure);
    OA_CHECK(!failure);
    const uint64_t partial_bytes = static_cast<uint64_t>(partial_raw);
    const uint64_t zrb = static_cast<uint64_t>(files[1].bytes.size());
    OA_CHECK(partial_bytes > 0);
    OA_CHECK(partial_bytes < zrb);
    check_unchanged(stamps);

    request.stop = nullptr;
    request.on_progress = nullptr;
    const oa::app::CopyResult resumed = oa::app::copy_content(source, request);
    OA_CHECK(resumed.status == oa::app::CopyStatus::finished);
    OA_CHECK(resumed.files_skipped == 1);
    OA_CHECK(resumed.files_copied == 2);
    const uint64_t campaign = static_cast<uint64_t>(files[2].bytes.size());
    OA_CHECK(resumed.bytes_copied == (zrb - partial_bytes) + campaign);
    check_unchanged(stamps);
    check_copied_tree(user);

    const oa::app::CopyResult again = oa::app::copy_content(source, request);
    OA_CHECK(again.status == oa::app::CopyStatus::finished);
    OA_CHECK(again.bytes_copied == 0);
    OA_CHECK(again.files_copied == 0);
    OA_CHECK(again.files_skipped == 3);
    OA_CHECK(!fs::exists(partial, failure));
    check_unchanged(stamps);
    OA_CHECK(oa::app::managed_base_present(user, oa::app::copy_check_bundles()));
    fs::remove(
        user / std::string(oa::app::managed_games_folder) / "totala" /
            std::string(oa::app::content_record_name),
        failure
    );
    OA_CHECK(!oa::app::managed_base_present(user, oa::app::copy_check_bundles()));
    OA_CHECK(
        fs::exists(
            user / std::string(oa::app::managed_expansions_folder) / "totala" / "campaign" /
                std::string(oa::app::content_record_name),
            failure
        )
    );
}

void test_wineskin(const fs::path& scratch) {
    const fs::path wrapper = scratch / "wrapper";
    const fs::path game = wrapper / "Game.app" / "Contents" / "Resources" / "drive_c" /
                          "Program Files" / "Total Annihilation";
    const auto files = oa::app::copy_check_files();
    place_file(wrapper / "decoy" / "REV31.GP3", "DECOY-FIXTURE!");
    place_file(game / "REV31.GP3", files[0].bytes);
    place_file(game / "Data" / "2.ZRB", files[1].bytes);
    place_file(game / "totala4.hpi", files[2].bytes);
    place_file(game / "notes.txt", "leave-this\n");
    each_regular_file(wrapper, lock_one);
    const std::vector<SourceStamp> stamps = stamp_files(wrapper);
    const fs::path user = scratch / "user";
    oa::app::FolderContentSource source(wrapper, oa::app::copy_check_bundles());
    const oa::app::CopyResult result = oa::app::copy_content(source, byte_request(user));
    OA_CHECK(result.status == oa::app::CopyStatus::finished);
    OA_CHECK(result.files_copied == 3);
    check_unchanged(stamps);
    check_copied_tree(user);
    OA_CHECK(
        read_bytes(user / std::string(oa::app::managed_games_folder) / "totala" / "rev31.gp3") !=
        "DECOY-FIXTURE!"
    );
}

oa::app::FreeSpace no_free_space(void*, const fs::path&) {
    oa::app::FreeSpace report;
    report.bytes = 0;
    report.known = true;
    return report;
}

oa::app::FreeSpace unknown_free_space(void*, const fs::path&) {
    return {};
}

void test_space(const fs::path& scratch) {
    const fs::path install = scratch / "install";
    write_cased_install(install);
    const std::vector<SourceStamp> stamps = stamp_files(install);
    oa::app::FolderContentSource source(install, oa::app::copy_check_bundles());
    const fs::path blocked = scratch / "blocked";
    oa::app::CopyRequest request = byte_request(blocked);
    request.free_space = no_free_space;
    const oa::app::CopyResult denied = oa::app::copy_content(source, request);
    OA_CHECK(denied.status == oa::app::CopyStatus::no_space);
    std::error_code failure;
    OA_CHECK(!fs::exists(blocked / std::string(oa::app::managed_games_folder), failure));
    check_unchanged(stamps);

    const fs::path user = scratch / "user";
    oa::app::CopyRequest unknown = byte_request(user);
    unknown.free_space = unknown_free_space;
    const oa::app::CopyResult copied = oa::app::copy_content(source, unknown);
    OA_CHECK(copied.status == oa::app::CopyStatus::finished);
    check_unchanged(stamps);
    check_copied_tree(user);
}

void test_record(const fs::path& scratch) {
    const fs::path record_path = scratch / "bundle" / ".oa-content";
    oa::app::ContentRecord record;
    record.bundle_id = "totala";
    record.source = "line\nslash\\end";
    oa::app::ContentRecordFile item;
    item.copy_name = "rev31.gp3";
    item.size = oa::app::copy_check_files()[0].bytes.size();
    item.modified = 1600000000;
    item.digest = digest_of(oa::app::copy_check_files()[0].bytes);
    record.files.push_back(item);
    std::string error;
    OA_CHECK(oa::app::write_content_record(record_path, record, &error));
    const oa::app::ContentRecord read = oa::app::read_content_record(record_path, &error);
    OA_CHECK(read.valid);
    OA_CHECK(read.bundle_id == "totala");
    OA_CHECK(read.source == record.source);
    OA_CHECK(read.files.size() == 1);
    OA_CHECK(read.files[0].copy_name == item.copy_name);
    OA_CHECK(read.files[0].size == item.size);
    OA_CHECK(read.files[0].modified == item.modified);
    OA_CHECK(read.files[0].digest == item.digest);

    oa::app::ContentRecord empty;
    empty.bundle_id = "totala";
    const fs::path absent = scratch / "empty" / ".oa-content";
    OA_CHECK(oa::app::write_content_record(absent, empty, &error));
    std::error_code failure;
    OA_CHECK(!fs::exists(absent, failure));

    place_file(record_path, "not a record\n");
    const oa::app::ContentRecord broken = oa::app::read_content_record(record_path, &error);
    OA_CHECK(broken.present);
    OA_CHECK(!broken.valid);
    OA_CHECK(!broken.io_error);
}

void test_corrupt_copy(const fs::path& scratch) {
    const fs::path install = scratch / "install";
    const fs::path user = scratch / "user";
    write_cased_install(install);
    const std::vector<SourceStamp> stamps = stamp_files(install);
    const fs::path totala = user / std::string(oa::app::managed_games_folder) / "totala";
    place_file(totala / ".oa-content", "not a record\n");
    place_file(totala / "rev31.gp3", "WRONG-BYTES!!\n");
    place_file(totala / "Data" / "2.zrb.part", std::string(100, 'x'));
    oa::app::FolderContentSource source(install, oa::app::copy_check_bundles());
    const oa::app::CopyResult result = oa::app::copy_content(source, byte_request(user));
    OA_CHECK(result.status == oa::app::CopyStatus::finished);
    check_unchanged(stamps);
    check_copied_tree(user);
    std::error_code failure;
    OA_CHECK(!fs::exists(totala / "Data" / "2.zrb.part", failure));
}

void test_ensure(const fs::path& scratch) {
    const fs::path install = scratch / "install";
    const fs::path user = scratch / "user";
    write_cased_install(install);
    each_regular_file(install, lock_one);
    const std::vector<SourceStamp> stamps = stamp_files(install);
    oa::app::ManagedCopyRequest request;
    request.installation = install;
    request.user_folder = user;
    request.user_folder_known = true;
    request.unattended = true;
    request.bundles = oa::app::copy_check_bundles();
    bool threw = false;
    std::string message;
    try {
        oa::app::ensure_managed_copy(request);
    } catch (const std::runtime_error& error) {
        threw = true;
        message = error.what();
    }
    OA_CHECK(threw);
    OA_CHECK(
        message ==
        "Open Annihilation has no copy of the game in its own folder, and this run does not "
        "make one. Start the game so it can copy the files, or name a folder with --game-dir "
        "PATH."
    );
    check_unchanged(stamps);
    std::error_code failure;
    OA_CHECK(!fs::exists(user / std::string(oa::app::managed_games_folder), failure));

    request.unattended = false;
    std::ostringstream news;
    request.news = &news;
    oa::app::ensure_managed_copy(request);
    OA_CHECK(news.str().find("Copying the game into Open Annihilation") != std::string::npos);
    check_unchanged(stamps);
    news.str("");
    news.clear();
    oa::app::ensure_managed_copy(request);
    OA_CHECK(news.str().empty());

    each_regular_file(install, unlock_one);
    fs::remove_all(install, failure);
    request.unattended = true;
    request.installation.clear();
    oa::app::ensure_managed_copy(request);

    oa::app::ManagedCopyRequest named;
    named.installation = scratch / "unused";
    named.game_dir_named = true;
    named.unattended = true;
    named.user_folder = scratch / "named";
    named.user_folder_known = true;
    named.bundles = oa::app::copy_check_bundles();
    oa::app::ensure_managed_copy(named);
    OA_CHECK(!fs::exists(named.user_folder / std::string(oa::app::managed_games_folder), failure));

    oa::app::ManagedCopyRequest unknown_folder;
    unknown_folder.installation = scratch / "unused";
    unknown_folder.user_folder_known = false;
    unknown_folder.user_folder = scratch / "unknown";
    unknown_folder.bundles = oa::app::copy_check_bundles();
    oa::app::ensure_managed_copy(unknown_folder);
    OA_CHECK(!fs::exists(
        unknown_folder.user_folder / std::string(oa::app::managed_games_folder), failure
    ));
}

void test_inside_source(const fs::path& scratch) {
    const fs::path install = scratch / "install";
    write_cased_install(install);
    const std::vector<SourceStamp> stamps = stamp_files(install);
    const fs::path user = install / "Open Annihilation";
    std::error_code failure;
    fs::create_directories(user, failure);
    OA_CHECK(!failure);
    oa::app::ManagedCopyRequest request;
    request.installation = install;
    request.user_folder = user;
    request.user_folder_known = true;
    request.bundles = oa::app::copy_check_bundles();
    oa::app::ensure_managed_copy(request);
    OA_CHECK(!fs::exists(
        user / std::string(oa::app::managed_games_folder) / "totala" / ".oa-content", failure
    ));
    check_unchanged(stamps);
}

void test_clone(const fs::path& scratch) {
    const fs::path source = scratch / "source" / "rev31.gp3";
    const fs::path destination_directory = scratch / "dest";
    place_file(source, oa::app::copy_check_files()[0].bytes);
    std::error_code failure;
    fs::create_directories(destination_directory, failure);
    OA_CHECK(!failure);
    const auto modified = fs::last_write_time(source, failure);
    OA_CHECK(!failure);
    const std::string original = read_bytes(source);
    const fs::path destination = destination_directory / "rev31.gp3";
    std::string error;
    const bool can = oa::platform::blocks_can_be_shared(source, destination_directory);
    const bool shared = oa::platform::share_file_blocks(source, destination, &error);
    if (shared) {
        OA_CHECK(can);
        OA_CHECK(read_bytes(destination) == original);
        OA_CHECK(fs::last_write_time(source, failure) == modified);
        OA_CHECK(!failure);
        OA_CHECK(read_bytes(source) == original);
    } else {
        OA_CHECK(!fs::exists(destination, failure));
    }
}

void test_check(const fs::path& scratch) {
    const int status = oa::app::run_content_copy_check(scratch / "documents");
    OA_CHECK(status == 0);
}

const oa::app::ContentBundleCheck*
bundle_named(const oa::app::ContentCheck& check, std::string_view id) {
    for (const oa::app::ContentBundleCheck& bundle : check.bundles) {
        if (bundle.id == id)
            return &bundle;
    }
    return nullptr;
}

bool names_file(const oa::app::ContentBundleCheck& bundle, std::string_view copy_name) {
    for (const oa::app::ContentFileDifference& file : bundle.differing) {
        if (file.copy_name == copy_name)
            return true;
    }
    return false;
}

bool line_has(const oa::app::ContentNoticeCopy& copy, std::string_view text) {
    for (const std::string& line : copy.lines) {
        if (line.find(text) != std::string::npos)
            return true;
    }
    return false;
}

/// Moves a file's modification time forward by whole minutes, so each step
/// of a test gives the file a time no earlier step recorded, however quickly
/// the steps run.
///
/// @param path the file
/// @param minutes how far forward
void bump_time(const fs::path& path, int minutes) {
    std::error_code failure;
    const auto stamp = fs::last_write_time(path, failure);
    OA_CHECK(!failure);
    fs::last_write_time(path, stamp + std::chrono::minutes(minutes), failure);
    OA_CHECK(!failure);
}

void copy_fixture(const fs::path& install, const fs::path& user) {
    write_cased_install(install);
    oa::app::FolderContentSource source(install, oa::app::copy_check_bundles());
    const oa::app::CopyResult result = oa::app::copy_content(source, byte_request(user));
    OA_CHECK(result.status == oa::app::CopyStatus::finished);
}

/// A bundle this run's copy already checked is not read by the start-up
/// check. A bundle the copy did not check is. The check after that reads
/// both, as a later start does.
void test_startup_reuses_copy_check(const fs::path& scratch) {
    const fs::path install = scratch / "install";
    const fs::path user = scratch / "user";
    copy_fixture(install, user);
    const auto files = oa::app::copy_check_files();
    const auto table = oa::app::copy_check_bundles();
    const oa::app::ContentCheck copied = oa::app::check_managed_content(user, table);
    const oa::app::ContentBundleCheck* copied_totala = bundle_named(copied, "totala");
    OA_CHECK(copied_totala != nullptr);
    OA_CHECK(bundle_named(copied, "campaign") != nullptr);
    if (copied_totala == nullptr)
        return;
    oa::app::ContentCheck kept;
    kept.bundles.push_back(*copied_totala);
    oa::app::keep_this_run_copy_check(std::move(kept));

    const fs::path rev = user / std::string(oa::app::managed_games_folder) / "totala" / "rev31.gp3";
    const fs::path campaign = user / std::string(oa::app::managed_expansions_folder) / "totala" /
                              "campaign" / "totala4.hpi";
    std::string changed_rev(files[0].bytes);
    changed_rev[0] = 'X';
    place_file(rev, changed_rev);
    bump_time(rev, 4);
    std::string changed_campaign(files[2].bytes);
    changed_campaign[0] = 'Y';
    place_file(campaign, changed_campaign);
    bump_time(campaign, 4);

    const oa::app::ContentCheck start = oa::app::check_managed_content(user, table);
    OA_CHECK(start.files_hashed == 1);
    const oa::app::ContentBundleCheck* totala = bundle_named(start, "totala");
    const oa::app::ContentBundleCheck* campaign_bundle = bundle_named(start, "campaign");
    OA_CHECK(totala != nullptr);
    OA_CHECK(campaign_bundle != nullptr);
    if (totala != nullptr)
        OA_CHECK(totala->files_differ == 0);
    if (campaign_bundle != nullptr) {
        OA_CHECK(campaign_bundle->files_differ == 1);
        OA_CHECK(names_file(*campaign_bundle, "totala4.hpi"));
    }

    const oa::app::ContentCheck later = oa::app::check_managed_content(user, table);
    OA_CHECK(later.files_hashed == 1);
    totala = bundle_named(later, "totala");
    OA_CHECK(totala != nullptr);
    if (totala != nullptr) {
        OA_CHECK(totala->files_differ == 1);
        OA_CHECK(names_file(*totala, "rev31.gp3"));
    }
    oa::app::keep_this_run_copy_check({});
}

void test_content_difference(const fs::path& scratch) {
    const fs::path install = scratch / "install";
    const fs::path user = scratch / "user";
    copy_fixture(install, user);
    const auto files = oa::app::copy_check_files();
    const auto table = oa::app::copy_check_bundles();
    const fs::path rev = user / std::string(oa::app::managed_games_folder) / "totala" / "rev31.gp3";

    // A new time with the same bytes is hashed once and is not a difference.
    bump_time(rev, 2);
    const oa::app::ContentCheck touched = oa::app::check_managed_content(user, table);
    OA_CHECK(touched.files_hashed == 1);
    OA_CHECK(!oa::app::content_notice_due(touched));
    const oa::app::ContentBundleCheck* totala = bundle_named(touched, "totala");
    OA_CHECK(totala != nullptr);
    OA_CHECK(totala->files_differ == 0);
    OA_CHECK(bundle_named(touched, "campaign") != nullptr);
    const oa::app::ContentCheck settled = oa::app::check_managed_content(user, table);
    OA_CHECK(settled.files_hashed == 0);
    OA_CHECK(!oa::app::content_notice_due(settled));

    // A changed byte is a difference, and the file is named. The next check
    // does not hash it again while its size and time still match.
    std::string changed(files[0].bytes);
    changed[0] = 'X';
    place_file(rev, changed);
    bump_time(rev, 10);
    const oa::app::ContentCheck bad = oa::app::check_managed_content(user, table);
    OA_CHECK(bad.files_hashed == 1);
    OA_CHECK(oa::app::content_notice_due(bad));
    totala = bundle_named(bad, "totala");
    OA_CHECK(totala != nullptr);
    OA_CHECK(totala->files_differ == 1);
    OA_CHECK(names_file(*totala, "rev31.gp3"));
    const oa::app::ContentCheck bad_again = oa::app::check_managed_content(user, table);
    OA_CHECK(bad_again.files_hashed == 0);
    OA_CHECK(oa::app::content_notice_due(bad_again));

    // The original bytes, with a new time, match again.
    place_file(rev, files[0].bytes);
    bump_time(rev, 20);
    const oa::app::ContentCheck restored = oa::app::check_managed_content(user, table);
    OA_CHECK(!oa::app::content_notice_due(restored));

    // A recorded hash the table does not know is named, and the file is not
    // hashed while its size and time still match.
    std::string error;
    oa::app::ContentRecord record = oa::app::read_content_record(
        user / std::string(oa::app::managed_games_folder) / "totala" /
            std::string(oa::app::content_record_name),
        &error
    );
    OA_CHECK(record.valid);
    bool rewritten = false;
    for (oa::app::ContentRecordFile& item : record.files) {
        if (item.copy_name != "rev31.gp3")
            continue;
        item.digest = oa::base::sha256::Digest{};
        rewritten = true;
    }
    OA_CHECK(rewritten);
    OA_CHECK(
        oa::app::write_content_record(
            user / std::string(oa::app::managed_games_folder) / "totala" /
                std::string(oa::app::content_record_name),
            record,
            &error
        )
    );
    const oa::app::ContentCheck unknown = oa::app::check_managed_content(user, table);
    OA_CHECK(unknown.files_hashed == 0);
    OA_CHECK(oa::app::content_notice_due(unknown));
    totala = bundle_named(unknown, "totala");
    OA_CHECK(totala != nullptr);
    OA_CHECK(names_file(*totala, "rev31.gp3"));

    // The notice shows at every start until the files match. Closing it, a
    // named --game-dir, and a run nobody watches are this start only.
    OA_CHECK(oa::app::show_content_notice(false, false, false, unknown));
    OA_CHECK(!oa::app::show_content_notice(false, false, true, unknown));
    OA_CHECK(oa::app::show_content_notice(false, false, false, unknown));
    OA_CHECK(!oa::app::show_content_notice(true, false, false, unknown));
    OA_CHECK(!oa::app::show_content_notice(false, true, false, unknown));
    OA_CHECK(!oa::app::show_content_notice(false, false, false, restored));

    oa::app::ContentCheck shown;
    oa::app::ContentBundleCheck music;
    music.english_name = "Music";
    music.file_count = 18;
    music.files_differ = 1;
    oa::app::ContentFileDifference music_file;
    music_file.copy_name = "music/7.mp3";
    music.differing.push_back(std::move(music_file));
    shown.bundles.push_back(std::move(music));
    oa::app::ContentBundleCheck core;
    core.english_name = "Core Contingency";
    core.file_count = 3;
    core.files_differ = 2;
    oa::app::ContentFileDifference core_archive;
    core_archive.copy_name = "a.gp3";
    core.differing.push_back(std::move(core_archive));
    oa::app::ContentFileDifference core_pack;
    core_pack.copy_name = "b.hpi";
    core.differing.push_back(std::move(core_pack));
    shown.bundles.push_back(std::move(core));
    const oa::app::ContentNoticeCopy notice = oa::app::content_difference_notice(shown);
    OA_CHECK(notice.title == "Game content");
    OA_CHECK(notice.details == "Details…");
    OA_CHECK(notice.ok == "OK");
    OA_CHECK(
        line_has(notice, "The following content for your game does not match our expected sizes:")
    );
    OA_CHECK(line_has(notice, "Music / 1 of 18 files differs"));
    OA_CHECK(line_has(notice, "Core Contingency / 2 of 3 files differ"));
    OA_CHECK(line_has(
        notice,
        "The game still plays with these files, but network games with other players may not "
        "start, and recordings may not replay the same. Copy the content again from another "
        "source to fix it."
    ));
    const oa::app::ContentNoticeCopy listed = oa::app::content_file_notice(shown);
    OA_CHECK(line_has(listed, "Files that differ:"));
    OA_CHECK(line_has(listed, "Music: music/7.mp3"));
    OA_CHECK(line_has(listed, "Core Contingency: b.hpi"));
    OA_CHECK(oa::app::content_match_line(0) == "All files match");
    OA_CHECK(oa::app::content_match_line(1) == "1 file differs");
    OA_CHECK(oa::app::content_match_line(2) == "2 files differ");
}

// One soundtrack file is missing and matches another file's bytes. A second
// is missing and matches nothing. The first is stored under its own name.
// The second is recorded as not in the source and does not warn. A damaged
// track still does.
void test_sibling_and_absent(const fs::path& scratch) {
    const std::string shared = "track-shared";
    const std::string only = "track-only";
    static oa::base::sha256::Digest stored_shared;
    static oa::base::sha256::Digest stored_only;
    stored_shared = digest_of(shared);
    stored_only = digest_of(only);
    static const std::string_view name0[]{"music/0.mp3"};
    static const std::string_view name9[]{"music/9.mp3"};
    static const std::string_view name13[]{"music/13.mp3"};
    const uint64_t shared_size = static_cast<uint64_t>(shared.size());
    const uint64_t only_size = static_cast<uint64_t>(only.size());
    const oa::data::content_bundles::ContentFile files[] = {
        {"music/0.mp3",
         name0,
         shared_size,
         std::span<const oa::base::sha256::Digest>(&stored_shared, 1)},
        {"music/9.mp3",
         name9,
         shared_size,
         std::span<const oa::base::sha256::Digest>(&stored_shared, 1)},
        {"music/13.mp3",
         name13,
         only_size,
         std::span<const oa::base::sha256::Digest>(&stored_only, 1)},
    };
    const oa::data::content_bundles::Bundle table[] = {
        {"music",
         "Music",
         oa::data::content_bundles::Kind::expansion,
         {},
         {},
         std::span<const oa::data::content_bundles::ContentFile>(files)},
    };
    const fs::path install = scratch / "source";
    const fs::path user = scratch / "user";
    std::error_code failure;
    fs::create_directories(install / "music", failure);
    OA_CHECK(!failure);
    fs::create_directories(user, failure);
    place_file(install / "music" / "9.mp3", shared);
    oa::app::FolderContentSource source(install, table);
    oa::app::CopyRequest request;
    request.user_folder = user;
    request.bundles = table;
    request.share_blocks = false;
    const oa::app::CopyResult copied = oa::app::copy_content(source, request);
    OA_CHECK(copied.status == oa::app::CopyStatus::finished);
    const fs::path music_dir =
        user / std::string(oa::app::managed_expansions_folder) / "music" / "music";
    OA_CHECK(read_bytes(music_dir / "0.mp3") == shared);
    OA_CHECK(read_bytes(music_dir / "9.mp3") == shared);
    OA_CHECK(!fs::exists(music_dir / "13.mp3", failure));
    std::string error;
    const oa::app::ContentRecord record = oa::app::read_content_record(
        user / std::string(oa::app::managed_expansions_folder) / "music" /
            std::string(oa::app::content_record_name),
        &error
    );
    OA_CHECK(record.valid);
    bool saw_filled = false;
    bool saw_absent = false;
    for (const oa::app::ContentRecordFile& item : record.files) {
        if (item.copy_name == "music/0.mp3") {
            saw_filled = true;
            OA_CHECK(!item.not_in_source);
            OA_CHECK(item.digest == stored_shared);
        }
        if (item.copy_name == "music/13.mp3") {
            saw_absent = true;
            OA_CHECK(item.not_in_source);
        }
    }
    OA_CHECK(saw_filled);
    OA_CHECK(saw_absent);
    const oa::app::ContentCheck check = oa::app::check_managed_content(user, table);
    OA_CHECK(!oa::app::content_notice_due(check));
    const oa::app::ContentBundleCheck* music = bundle_named(check, "music");
    OA_CHECK(music != nullptr);
    if (music != nullptr) {
        OA_CHECK(music->files_differ == 0);
        OA_CHECK(music->files_not_in_source == 1);
        OA_CHECK(oa::app::content_bundle_line(*music) == "2 of 3 tracks");
    }
    OA_CHECK(oa::app::content_tracks_line(17, 18) == "17 of 18 tracks");

    place_file(music_dir / "9.mp3", "TRACK-SHARED");
    bump_time(music_dir / "9.mp3", 30);
    const oa::app::ContentCheck damaged = oa::app::check_managed_content(user, table);
    OA_CHECK(oa::app::content_notice_due(damaged));
    const oa::app::ContentBundleCheck* broken = bundle_named(damaged, "music");
    OA_CHECK(broken != nullptr);
    if (broken != nullptr) {
        OA_CHECK(broken->files_differ == 1);
        OA_CHECK(names_file(*broken, "music/9.mp3"));
        OA_CHECK(!names_file(*broken, "music/13.mp3"));
    }
}

/// A finished copy counts the files it took, so the overall line reads 100%.
void test_finished_copy_counts_taken(const fs::path& scratch) {
    constexpr oa::app::CopyFileCount full = oa::app::copy_file_count(38, 48, 10);
    OA_CHECK(full.done == 38);
    OA_CHECK(full.total == 38);
    OA_CHECK(oa::app::copy_file_percent(full) == 100);
    constexpr oa::app::CopyFileCount open = oa::app::copy_file_count(38, 48, 0);
    OA_CHECK(open.done == 38);
    OA_CHECK(open.total == 48);
    OA_CHECK(oa::app::copy_file_percent(open) == 79);
    OA_CHECK(oa::app::copy_file_percent({}) == 0);

    const auto base = oa::app::copy_check_bundles();
    static constexpr std::string_view extra_sources[]{"missing.hpi"};
    static constexpr oa::base::sha256::Digest extra_digest{};
    static constexpr oa::base::sha256::Digest extra_hashes[]{extra_digest};
    static const oa::data::content_bundles::ContentFile extra_files[]{
        {"missing.hpi", extra_sources, 3, extra_hashes},
    };
    static constexpr std::string_view extra_extends[]{"totala"};
    std::vector<oa::data::content_bundles::Bundle> table(base.begin(), base.end());
    table.push_back(
        {"absent",
         "Absent Bundle",
         oa::data::content_bundles::Kind::expansion,
         extra_extends,
         {},
         extra_files}
    );

    const fs::path install = scratch / "install";
    const fs::path user = scratch / "user";
    write_cased_install(install);
    oa::app::FolderContentSource source(install, table);
    oa::app::CopyRequest request;
    request.user_folder = user;
    request.bundles = table;
    request.share_blocks = false;
    request.chunk_bytes = 4;

    struct Tally {
        uint32_t done{};
        uint32_t total{};
        int calls{};
    } tally;

    request.progress_context = &tally;
    request.on_progress = [](void* context, const oa::app::CopyProgress& progress) {
        auto* noted = static_cast<Tally*>(context);
        noted->done = progress.files_done;
        noted->total = progress.files_total;
        noted->calls += 1;
    };
    const oa::app::CopyResult copied = oa::app::copy_content(source, request);
    OA_CHECK(copied.status == oa::app::CopyStatus::finished);
    OA_CHECK(copied.files_copied == 3);
    OA_CHECK(tally.calls > 0);
    OA_CHECK(tally.done == 3);
    OA_CHECK(tally.total == 3);
    OA_CHECK(oa::app::copy_file_percent({tally.done, tally.total}) == 100);
}

/// A source that holds a movie at another size: the copy records it as not
/// in the source, with that size, says so, and finishes, and the base game
/// is taken as copied. The check names the movie with both sizes and finds
/// no difference. A later copy from a whole source stores the movie.
void test_movie_size_differs(const fs::path& scratch) {
    const fs::path install = scratch / "install";
    const fs::path user = scratch / "user";
    write_cased_install(install);
    place_file(install / "data" / "2.ZRB", "a movie of another size\n");
    oa::app::FolderContentSource source(install, oa::app::copy_check_bundles());
    std::ostringstream news;
    oa::app::CopyRequest request = byte_request(user);
    request.news = &news;
    const oa::app::CopyResult copied = oa::app::copy_content(source, request);
    OA_CHECK(copied.status == oa::app::CopyStatus::finished);
    OA_CHECK(copied.files_copied == 2);
    OA_CHECK(copied.changed.empty());
    const auto files = oa::app::copy_check_files();
    const std::string expected = std::to_string(files[1].bytes.size());
    OA_CHECK(
        news.str().find(
            "open-annihilation: Data/2.zrb of Total Annihilation is left out: the source holds it "
            "at 24 bytes, not " +
            expected + "\n"
        ) != std::string::npos
    );
    std::string error;
    const fs::path totala = user / std::string(oa::app::managed_games_folder) / "totala";
    const oa::app::ContentRecord record =
        oa::app::read_content_record(totala / std::string(oa::app::content_record_name), &error);
    OA_CHECK(record.valid);
    bool recorded = false;
    for (const oa::app::ContentRecordFile& item : record.files) {
        if (item.copy_name != "Data/2.zrb")
            continue;
        recorded = true;
        OA_CHECK(item.not_in_source);
        OA_CHECK(item.source_size == 24);
    }
    OA_CHECK(recorded);
    std::error_code failure;
    OA_CHECK(!fs::exists(totala / "Data" / "2.zrb", failure));
    OA_CHECK(oa::app::managed_base_present(user, oa::app::copy_check_bundles()));

    const oa::app::ContentCheck check =
        oa::app::check_managed_content(user, oa::app::copy_check_bundles());
    OA_CHECK(!oa::app::content_notice_due(check));
    const oa::app::ContentBundleCheck* base = bundle_named(check, "totala");
    OA_CHECK(base != nullptr);
    if (base != nullptr) {
        OA_CHECK(base->files_differ == 0);
        OA_CHECK(base->files_not_in_source == 1);
        OA_CHECK(base->left_out.size() == 1);
        OA_CHECK(
            oa::app::content_bundle_line(*base) == "Data/2.zrb left out: 24 bytes, not " + expected
        );
    }
    oa::app::ContentBundleCheck two = base != nullptr ? *base : oa::app::ContentBundleCheck{};
    two.left_out.push_back({"Data/5.zrb", 7, 9});
    OA_CHECK(
        oa::app::content_bundle_line(two) ==
        "2 files left out for their size: Data/2.zrb, Data/5.zrb"
    );

    const fs::path whole = scratch / "whole";
    write_cased_install(whole);
    oa::app::FolderContentSource other(whole, oa::app::copy_check_bundles());
    const oa::app::CopyResult finished = oa::app::copy_content(other, byte_request(user));
    OA_CHECK(finished.status == oa::app::CopyStatus::finished);
    OA_CHECK(finished.files_copied == 1);
    OA_CHECK(read_bytes(totala / "Data" / "2.zrb") == files[1].bytes);
    const oa::app::ContentCheck rechecked =
        oa::app::check_managed_content(user, oa::app::copy_check_bundles());
    const oa::app::ContentBundleCheck* whole_base = bundle_named(rechecked, "totala");
    OA_CHECK(whole_base != nullptr && whole_base->left_out.empty());
    OA_CHECK(
        whole_base != nullptr && oa::app::content_bundle_line(*whole_base) == "All files match"
    );
}

/// A source that holds a base game's archive at another size has been
/// changed: the copy stops before anything is copied, names the archive
/// with both sizes, and leaves nothing taken as copied. A start's own copy
/// refuses it with the same reason. A later copy from a whole source
/// finishes as usual.
void test_archive_size_differs(const fs::path& scratch) {
    const fs::path install = scratch / "install";
    const fs::path user = scratch / "user";
    write_cased_install(install);
    place_file(install / "REV31.GP3", "a revision of another size\n");
    oa::app::FolderContentSource source(install, oa::app::copy_check_bundles());
    std::ostringstream news;
    oa::app::CopyRequest request = byte_request(user);
    request.news = &news;
    const oa::app::CopyResult stopped = oa::app::copy_content(source, request);
    OA_CHECK(stopped.status == oa::app::CopyStatus::changed);
    OA_CHECK(stopped.files_copied == 0);
    OA_CHECK(stopped.bytes_copied == 0);
    const auto files = oa::app::copy_check_files();
    OA_CHECK(stopped.changed.size() == 1);
    if (stopped.changed.size() == 1) {
        OA_CHECK(stopped.changed[0].bundle_id == "totala");
        OA_CHECK(stopped.changed[0].copy_name == "rev31.gp3");
        OA_CHECK(stopped.changed[0].source_size == 27);
        OA_CHECK(stopped.changed[0].size == files[0].bytes.size());
    }
    OA_CHECK(stopped.error.find("has been changed") != std::string::npos);
    OA_CHECK(
        news.str().find(
            "open-annihilation: rev31.gp3 of Total Annihilation is not copied: the source holds "
            "it at 27 bytes, not " +
            std::to_string(files[0].bytes.size()) + "\n"
        ) != std::string::npos
    );
    OA_CHECK(news.str().find("Copying the game") == std::string::npos);
    std::error_code failure;
    OA_CHECK(!fs::exists(user / std::string(oa::app::managed_games_folder), failure));
    OA_CHECK(!fs::exists(user / std::string(oa::app::managed_expansions_folder), failure));
    OA_CHECK(!oa::app::managed_base_present(user, oa::app::copy_check_bundles()));

    oa::app::ManagedCopyRequest start;
    start.installation = install;
    start.user_folder = scratch / "started";
    start.user_folder_known = true;
    start.bundles = oa::app::copy_check_bundles();
    bool refused = false;
    try {
        oa::app::ensure_managed_copy(start);
    } catch (const std::runtime_error& error) {
        refused = std::string(error.what()).find("has been changed") != std::string::npos;
    }
    OA_CHECK(refused);

    const fs::path whole = scratch / "whole";
    write_cased_install(whole);
    oa::app::FolderContentSource other(whole, oa::app::copy_check_bundles());
    const oa::app::CopyResult finished = oa::app::copy_content(other, byte_request(user));
    OA_CHECK(finished.status == oa::app::CopyStatus::finished);
    OA_CHECK(finished.files_copied == 3);
    check_copied_tree(user);
    OA_CHECK(oa::app::managed_base_present(user, oa::app::copy_check_bundles()));
}

/// An expansion the source holds, whose archive is at another size, stops
/// the copy as the base game's does: the campaign's totala4.hpi here, then
/// one of two archives of an expansion whose other archive matches, listed
/// with a base game's archive in the table's order. Nothing is recorded, a
/// start's own copy refuses it, and a clean copy then finishes. An
/// expansion the source holds none of stops nothing, and one whose archives
/// all match is copied as usual.
void test_expansion_archive_size_differs(const fs::path& scratch) {
    const auto files = oa::app::copy_check_files();
    const fs::path install = scratch / "install";
    const fs::path user = scratch / "user";
    write_cased_install(install);
    place_file(install / "totala4.hpi", "a campaign archive of another size\n");
    oa::app::FolderContentSource source(install, oa::app::copy_check_bundles());
    std::ostringstream news;
    oa::app::CopyRequest request = byte_request(user);
    request.news = &news;
    const oa::app::CopyResult stopped = oa::app::copy_content(source, request);
    OA_CHECK(stopped.status == oa::app::CopyStatus::changed);
    OA_CHECK(stopped.files_copied == 0);
    OA_CHECK(stopped.changed.size() == 1);
    if (stopped.changed.size() == 1) {
        OA_CHECK(stopped.changed[0].bundle_id == "campaign");
        OA_CHECK(stopped.changed[0].copy_name == "totala4.hpi");
        OA_CHECK(stopped.changed[0].source_size == 35);
        OA_CHECK(stopped.changed[0].size == files[2].bytes.size());
    }
    OA_CHECK(
        news.str().find("open-annihilation: totala4.hpi of Campaign is not copied") !=
        std::string::npos
    );
    std::error_code failure;
    OA_CHECK(!fs::exists(user / std::string(oa::app::managed_games_folder), failure));
    OA_CHECK(!fs::exists(user / std::string(oa::app::managed_expansions_folder), failure));
    oa::app::ManagedCopyRequest start;
    start.installation = install;
    start.user_folder = scratch / "started";
    start.user_folder_known = true;
    start.bundles = oa::app::copy_check_bundles();
    bool refused = false;
    try {
        oa::app::ensure_managed_copy(start);
    } catch (const std::runtime_error& error) {
        refused = std::string(error.what()).find("has been changed") != std::string::npos;
    }
    OA_CHECK(refused);
    OA_CHECK(
        !fs::exists(scratch / "started" / std::string(oa::app::managed_games_folder), failure)
    );
    const fs::path whole = scratch / "whole";
    write_cased_install(whole);
    oa::app::FolderContentSource clean(whole, oa::app::copy_check_bundles());
    const oa::app::CopyResult finished = oa::app::copy_content(clean, byte_request(user));
    OA_CHECK(finished.status == oa::app::CopyStatus::finished);
    OA_CHECK(finished.files_copied == 3);
    check_copied_tree(user);

    // A table with an expansion of two archives.
    static const std::string first_bytes = "tactics-one\n";
    static const std::string second_bytes = "tactics-two\n";
    static oa::base::sha256::Digest first_digest;
    static oa::base::sha256::Digest second_digest;
    first_digest = digest_of(first_bytes);
    second_digest = digest_of(second_bytes);
    static constexpr std::string_view first_sources[]{"tactics1.hpi"};
    static constexpr std::string_view second_sources[]{"tactics2.hpi"};
    static constexpr std::string_view extends[]{"totala"};
    static const oa::data::content_bundles::ContentFile tactics_files[]{
        {"tactics1.hpi",
         first_sources,
         first_bytes.size(),
         std::span<const oa::base::sha256::Digest>(&first_digest, 1)},
        {"tactics2.hpi",
         second_sources,
         second_bytes.size(),
         std::span<const oa::base::sha256::Digest>(&second_digest, 1)},
    };
    const auto base = oa::app::copy_check_bundles();
    std::vector<oa::data::content_bundles::Bundle> table(base.begin(), base.end());
    table.push_back(
        {"tactics",
         "Tactics",
         oa::data::content_bundles::Kind::expansion,
         extends,
         {},
         tactics_files}
    );
    const auto copy_with = [&](const fs::path& from, const fs::path& to) {
        oa::app::FolderContentSource held(from, table);
        oa::app::CopyRequest copy;
        copy.user_folder = to;
        copy.bundles = table;
        copy.share_blocks = false;
        return oa::app::copy_content(held, copy);
    };
    const fs::path tactics_folder = std::string(oa::app::managed_expansions_folder);

    // Present, one archive at another size, with a base game's archive too.
    const fs::path mixed = scratch / "mixed";
    write_cased_install(mixed);
    place_file(mixed / "REV31.GP3", "a revision of another size\n");
    place_file(mixed / "tactics1.hpi", first_bytes);
    place_file(mixed / "tactics2.hpi", "tactics of another size\n");
    const oa::app::CopyResult both = copy_with(mixed, scratch / "mixed-user");
    OA_CHECK(both.status == oa::app::CopyStatus::changed);
    OA_CHECK(both.changed.size() == 2);
    if (both.changed.size() == 2) {
        OA_CHECK(both.changed[0].copy_name == "rev31.gp3");
        OA_CHECK(both.changed[1].bundle_id == "tactics");
        OA_CHECK(both.changed[1].copy_name == "tactics2.hpi");
        OA_CHECK(both.changed[1].source_size == 24);
        OA_CHECK(both.changed[1].size == second_bytes.size());
    }
    OA_CHECK(!fs::exists(scratch / "mixed-user", failure));

    // Absent: the source holds none of the expansion's files.
    const fs::path without = scratch / "without";
    write_cased_install(without);
    const oa::app::CopyResult absent = copy_with(without, scratch / "without-user");
    OA_CHECK(absent.status == oa::app::CopyStatus::finished);
    OA_CHECK(absent.changed.empty());
    OA_CHECK(absent.files_copied == 3);
    OA_CHECK(
        !fs::exists(scratch / "without-user" / tactics_folder / "totala" / "tactics", failure)
    );

    // Present, and both archives the right size.
    const fs::path matching = scratch / "matching";
    write_cased_install(matching);
    place_file(matching / "tactics1.hpi", first_bytes);
    place_file(matching / "tactics2.hpi", second_bytes);
    const fs::path matching_user = scratch / "matching-user";
    const oa::app::CopyResult copied = copy_with(matching, matching_user);
    OA_CHECK(copied.status == oa::app::CopyStatus::finished);
    OA_CHECK(copied.changed.empty());
    OA_CHECK(copied.files_copied == 5);
    const fs::path tactics = matching_user / tactics_folder / "totala" / "tactics";
    OA_CHECK(read_bytes(tactics / "tactics1.hpi") == first_bytes);
    OA_CHECK(read_bytes(tactics / "tactics2.hpi") == second_bytes);
    const oa::app::ContentRecord record =
        oa::app::read_content_record(tactics / std::string(oa::app::content_record_name), nullptr);
    OA_CHECK(oa::data::content_bundles::record_complete(table.back(), record));
}

/// Archives are told from movies and tracks by their suffix, in any case./// Archives are told from movies and tracks by their suffix, in any case.
void test_archive_names() {
    OA_CHECK(oa::app::archive_copy_name("rev31.gp3"));
    OA_CHECK(oa::app::archive_copy_name("TOTALA1.HPI"));
    OA_CHECK(oa::app::archive_copy_name("ccdata.ccx"));
    OA_CHECK(oa::app::archive_copy_name("afark.ufo"));
    OA_CHECK(!oa::app::archive_copy_name("Data/2.zrb"));
    OA_CHECK(!oa::app::archive_copy_name("music/7.mp3"));
    OA_CHECK(!oa::app::archive_copy_name(".hpi"));
}

void test_published_installer_listing() {
    struct Expect {
        uint64_t size{};
        uint64_t bytes{};
        std::vector<std::string> bundles{};
        std::vector<std::string> absent{};
    };

    const Expect rows[]{
        Expect{
            1093885504,
            1049464781,
            {"totala",
             "campaign",
             "core-contingency",
             "battle-tactics",
             "music",
             "cavedog-units",
             "cavedog-tilesets"},
            {"TADemo.hpi"},
        },
        Expect{
            2559350261,
            1040584696,
            {"totala",
             "campaign",
             "core-contingency",
             "battle-tactics",
             "music",
             "cavedog-tilesets"},
            {"TADemo.hpi",
             "music/0.mp3",
             "music/13.mp3",
             "afark.ufo",
             "aflea.ufo",
             "ascarab.ufo",
             "cormabm.ufo",
             "cornecro.ufo",
             "corplas.ufo"},
        },
        Expect{
            1144736288,
            1049223122,
            {"totala",
             "campaign",
             "core-contingency",
             "battle-tactics",
             "music",
             "cavedog-tilesets"},
            {"TADemo.hpi",
             "afark.ufo",
             "aflea.ufo",
             "ascarab.ufo",
             "cormabm.ufo",
             "cornecro.ufo",
             "corplas.ufo"},
        },
    };
    for (const Expect& row : rows) {
        const auto* installer = oa::data::content_bundles::find_installer(row.size);
        OA_CHECK(installer != nullptr);
        if (installer == nullptr)
            continue;
        oa::app::SourceDescription described;
        OA_CHECK(oa::app::describe_published_holdings(*installer, described));
        OA_CHECK(described.bytes_to_copy == row.bytes);
        OA_CHECK(described.bundles == row.bundles);
        const std::span<const std::string_view> absent = oa::app::installer_not_held(*installer);
        OA_CHECK(absent.size() == row.absent.size());
        for (std::size_t index = 0; index < absent.size() && index < row.absent.size(); ++index)
            OA_CHECK(absent[index] == row.absent[index]);
        OA_CHECK(oa::app::is_published_installer_name(fs::path(installer->file_name), *installer));
        OA_CHECK(!oa::app::is_published_installer_name(fs::path("notes.txt"), *installer));
    }
}

void test_published_installer_rejects_other_bytes(const fs::path& scratch) {
    const auto* installer = oa::data::content_bundles::find_installer(1093885504);
    OA_CHECK(installer != nullptr);
    if (installer == nullptr)
        return;
    fs::create_directories(scratch);
    const fs::path file = scratch / std::string(installer->file_name);
    {
        std::ofstream created(file, std::ios::binary);
        OA_CHECK(static_cast<bool>(created));
    }
    std::error_code failure;
    fs::resize_file(file, static_cast<uintmax_t>(installer->size), failure);
    OA_CHECK(!failure);
    oa::app::SourceDescription described;
    std::string error;
    OA_CHECK(!oa::app::describe_published_installer(file, described, &error));
    OA_CHECK(described.bundles.empty());
    OA_CHECK(!error.empty());

    const fs::path small = scratch / "short" / std::string(installer->file_name);
    fs::create_directories(small.parent_path());
    place_file(small, "not-the-installer");
    OA_CHECK(!oa::app::describe_published_installer(small, described, &error));
    OA_CHECK(described.bundles.empty());
}

} // namespace

int main() {
    test_decisions();
    const fs::path scratch = oa::test::make_scratch_directory("G02-content-copy");
    test_resume(scratch / "resume");
    test_wineskin(scratch / "wineskin");
    test_space(scratch / "space");
    test_record(scratch / "record");
    test_corrupt_copy(scratch / "corrupt");
    test_ensure(scratch / "ensure");
    test_inside_source(scratch / "inside");
    test_clone(scratch / "clone");
    test_check(scratch / "check");
    test_startup_reuses_copy_check(scratch / "G54-startup");
    test_content_difference(scratch / "G03-difference");
    test_sibling_and_absent(scratch / "G26-music");
    test_finished_copy_counts_taken(scratch / "G59-count");
    test_movie_size_differs(scratch / "movie-size");
    test_archive_size_differs(scratch / "archive-size");
    test_expansion_archive_size_differs(scratch / "expansion-size");
    test_archive_names();
    test_published_installer_listing();
    test_published_installer_rejects_other_bytes(scratch / "G71-published");
    each_regular_file(scratch, unlock_one);
    std::error_code ignored;
    fs::remove_all(scratch, ignored);
    return oa::test::check_exit_status();
}
