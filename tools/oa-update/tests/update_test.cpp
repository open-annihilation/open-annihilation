// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The update helper's own checks. Keys come from fixed seeds. No test
// starts a program, and nothing is fetched.

#include "oa/app/update/plan.hpp"
#include "oa/update_helper/journal.hpp"
#include "oa/update_helper/paths.hpp"
#include "oa/update_helper/run.hpp"
#include "oa/update_helper/unpack.hpp"
#include "oa/update_helper/verify.hpp"
#include "oa/update_helper/wait.hpp"
#include "oa/update_helper/window.hpp"

#include "oa/base/sha256.hpp"
#include "oa/base/signing/ed25519.hpp"
#include "oa/data/catalogue/check.hpp"
#include "oa/data/engine_feed/feed.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/formats/url.hpp"
#include "oa/formats/zip.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/kit/components_more.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace helper = oa::update_helper;
namespace update = oa::app::update;
namespace feed = oa::data::engine_feed;
namespace signing = oa::base::signing;
namespace sha256 = oa::base::sha256;
namespace catalogue = oa::data::catalogue;
namespace oamod = oa::formats::oamod;
namespace url = oa::formats::url;
namespace zip = oa::formats::zip;

struct Scratch {
    fs::path path;

    explicit Scratch(std::string_view name) {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
               (std::string("oa-v07-") + std::string(name) + "-" + std::to_string(ticks));
        fs::create_directories(path);
    }

    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

std::string spell(const fs::path& path) {
    return helper::utf8_of(path);
}

std::vector<uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

void write_bytes(const fs::path& path, std::span<const uint8_t> bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!bytes.empty())
        out.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
    OA_CHECK(static_cast<bool>(out));
}

update::Plan sample_plan(const Scratch& scratch) {
    update::Plan plan;
    plan.plan = update::plan_format;
    plan.action = update::Action::install;
    plan.route = update::Route::check_folder;
    plan.wait_for = 0;
    plan.install = spell(scratch.path / "install");
    plan.program = spell(scratch.path / "program");
    plan.arguments = {"--skip-intro"};
    plan.from_version = "0.0.1";
    plan.feed = spell(scratch.path / "feed.json");
    plan.signature = spell(scratch.path / "feed.json.sig");
    plan.release = 2;
    plan.package = "linux-x86_64";
    plan.file = spell(scratch.path / "pkg.zip");
    plan.installer = false;
    plan.preferences = spell(scratch.path / "preferences.conf");
    plan.updates = spell(scratch.path / "updates");
    plan.title = "Installing 0.0.2";
    plan.line = "0.0.1 is kept.";
    return plan;
}

void plan_and_state() {
    const Scratch scratch("plan");
    const update::Plan plan = sample_plan(scratch);
    const std::string text = update::plan_text(plan);
    std::string error;
    const std::optional<update::Plan> loaded = update::read_plan(bytes_of(text), &error);
    OA_CHECK(loaded.has_value());
    if (loaded)
        OA_CHECK(update::plan_text(*loaded) == text);

    auto replaced = [](std::string json, std::string_view from, std::string_view to) {
        const std::size_t at = json.find(from);
        OA_CHECK(at != std::string::npos);
        if (at != std::string::npos)
            json.replace(at, from.size(), to);
        return json;
    };
    auto refuses = [](std::string_view json, std::string_view snippet) {
        std::string reason;
        OA_CHECK(!update::read_plan(bytes_of(json), &reason));
        OA_CHECK(reason.find(snippet) != std::string::npos);
    };
    const std::string good = text;
    refuses(replaced(good, "\"plan\":1", "\"plan\":2"), "not plan 1");
    refuses(replaced(good, "\"route\":\"check-folder\"", "\"route\":\"steam\""), "not known");
    refuses(
        replaced(
            good,
            "\"install\":\"" + spell(scratch.path / "install") + "\"",
            "\"install\":\"install\""
        ),
        "absolute"
    );
    refuses(
        replaced(
            good, "\"program\":\"" + spell(scratch.path / "program") + "\"", "\"program\":\"\""
        ),
        "empty"
    );
    refuses(
        replaced(
            good, "\"title\":\"Installing 0.0.2\"", "\"title\":\"" + std::string(201, 'a') + "\""
        ),
        "too long"
    );
    refuses(
        replaced(good, "\"line\":\"0.0.1 is kept.\"", "\"line\":\"" + std::string(201, 'b') + "\""),
        "too long"
    );

    update::State state;
    state.installed_version = "0.0.2";
    state.installed_release = 2;
    state.kept_version = "0.0.1";
    state.kept_path = spell(scratch.path / "previous");
    state.kept_route = update::Route::check_folder;
    state.preferences_copy = spell(scratch.path / "previous" / "preferences.conf");
    state.confirmed = false;
    state.failed_starts = 0;
    const std::string state_json = update::state_text(state);
    const std::optional<update::State> read = update::read_state(bytes_of(state_json), &error);
    OA_CHECK(read.has_value());
    if (read) {
        OA_CHECK(read->installed_version == "0.0.2");
        OA_CHECK(read->kept_version == "0.0.1");
        OA_CHECK(!read->confirmed);
        OA_CHECK(read->failed_starts == 0);
        OA_CHECK(update::state_text(*read) == state_json);
    }
    OA_CHECK(update::write_state(scratch.path / "state.json", state, &error));
}

struct SignedFeed {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> signature;
    std::string id;
    std::string text;
};

SignedFeed sign_feed(std::string_view json, std::string_view key_id, uint8_t fill) {
    std::array<uint8_t, 32> seed{};
    seed.fill(fill);
    signing::SecretKey secret{};
    signing::PublicKey public_key{};
    signing::key_pair_from_seed(seed, secret, public_key);
    seed.fill(0);
    SignedFeed signed_feed;
    signed_feed.bytes.assign(json.begin(), json.end());
    const signing::Signature signature = signing::sign(secret, signed_feed.bytes);
    secret.fill(0);
    const std::string line =
        catalogue::signature_file_text(catalogue::SignatureLine{std::string(key_id), signature});
    signed_feed.signature.assign(line.begin(), line.end());
    signed_feed.id = std::string(key_id);
    const signing::KeyText key_text = signing::public_key_text(public_key);
    signed_feed.text = std::string(key_text.view());
    return signed_feed;
}

std::string feed_json(std::string_view file_name, uint64_t size, const sha256::Digest& digest) {
    feed::Feed written;
    written.channel = "stable";
    feed::Release release;
    release.release = 2;
    release.version = *oamod::parse_engine_version("0.0.2");
    release.version_text = "0.0.2";
    release.date = "2026-10-24";
    release.from = *oamod::parse_engine_version("0.0.1");
    release.page = "https://example.net/v0.0.2";
    feed::Package package;
    package.id = "linux-x86_64";
    package.zip.file = std::string(file_name);
    package.zip.size = size;
    package.zip.sha256 = digest;
    const std::optional<url::Url> mirror =
        url::parse_http_url("http://downloads.example.net/v1/engine/0.0.2/");
    OA_CHECK(mirror.has_value());
    if (mirror)
        package.mirrors.push_back(*mirror);
    release.packages.push_back(std::move(package));
    written.releases.push_back(std::move(release));
    return feed::feed_text(written);
}

bool stage_absent(const Scratch& scratch) {
    std::error_code io;
    return !fs::exists(scratch.path / "stage", io) && !io;
}

void expect_refused(
    const helper::VerifyRequest& request, std::string_view snippet, const Scratch& scratch
) {
    helper::Verified verified;
    std::string error;
    OA_CHECK(!helper::verify_package(request, verified, &error));
    if (error.find(snippet) == std::string::npos) {
        const std::string file = request.file.generic_string();
        std::fprintf(
            stderr,
            "expected detail containing '%.*s', got '%s' (file %s)\n",
            static_cast<int>(snippet.size()),
            snippet.data(),
            error.c_str(),
            file.c_str()
        );
    }
    OA_CHECK(error.find(snippet) != std::string::npos);
    OA_CHECK(stage_absent(scratch));
}

void verification() {
    const Scratch scratch("verify");
    const std::vector<uint8_t> package = bytes_of("package-bytes");
    const fs::path package_path = scratch.path / "pkg.zip";
    write_bytes(package_path, package);
    const sha256::Digest digest = sha256::digest_of(package);
    const SignedFeed signed_feed =
        sign_feed(feed_json("pkg.zip", package.size(), digest), "v07-test", 0x21);

    const fs::path feed_path = scratch.path / "feed.json";
    const fs::path signature_path = scratch.path / "feed.json.sig";
    write_bytes(feed_path, signed_feed.bytes);
    write_bytes(signature_path, signed_feed.signature);

    helper::FeedKey key;
    key.id = signed_feed.id;
    key.text = signed_feed.text;
    std::string key_error;
    helper::FeedKey parsed;
    OA_CHECK(helper::parse_feed_key(key.id + "=" + key.text, parsed, &key_error));
    OA_CHECK(parsed.id == key.id);
    OA_CHECK(!helper::parse_feed_key("not a key", parsed, &key_error));

    helper::VerifyRequest request;
    request.feed = feed_path;
    request.signature = signature_path;
    request.file = package_path;
    request.release = 2;
    request.package = "linux-x86_64";
    request.installer = false;
    request.keys = {key};

    helper::Verified verified;
    std::string error;
    OA_CHECK(helper::verify_package(request, verified, &error));
    OA_CHECK(verified.version == "0.0.2");
    OA_CHECK(verified.release == 2);
    OA_CHECK(verified.package == "linux-x86_64");
    OA_CHECK(stage_absent(scratch));

    const std::vector<uint8_t> longer = bytes_of("package-bytes!");
    const fs::path long_path = scratch.path / "long" / "pkg.zip";
    fs::create_directories(long_path.parent_path());
    write_bytes(long_path, longer);
    helper::VerifyRequest wrong_size = request;
    wrong_size.file = long_path;
    expect_refused(wrong_size, "size", scratch);

    std::vector<uint8_t> flipped = package;
    flipped[0] = static_cast<uint8_t>(flipped[0] ^ 0x01);
    // The file name stays the one the feed records. A different name is
    // refused before the digest is compared.
    const fs::path flipped_path = scratch.path / "flipped" / "pkg.zip";
    fs::create_directories(flipped_path.parent_path());
    write_bytes(flipped_path, flipped);
    helper::VerifyRequest wrong_digest = request;
    wrong_digest.file = flipped_path;
    expect_refused(wrong_digest, "digest", scratch);

    const fs::path renamed = scratch.path / "other.zip";
    write_bytes(renamed, package);
    helper::VerifyRequest wrong_name = request;
    wrong_name.file = renamed;
    expect_refused(wrong_name, "name", scratch);

    helper::VerifyRequest missing_release = request;
    missing_release.release = 9;
    expect_refused(missing_release, "release", scratch);

    helper::VerifyRequest missing_package = request;
    missing_package.package = "windows-x64";
    expect_refused(missing_package, "package", scratch);

    helper::VerifyRequest missing_installer = request;
    missing_installer.installer = true;
    expect_refused(missing_installer, "installer", scratch);

    const SignedFeed other = sign_feed("unused", "v07-other", 0x22);
    helper::FeedKey other_key;
    other_key.id = other.id;
    other_key.text = other.text;
    helper::VerifyRequest unknown = request;
    unknown.keys = {other_key};
    expect_refused(unknown, "unknown_key", scratch);

    helper::VerifyRequest missing_signature = request;
    missing_signature.signature = scratch.path / "missing.sig";
    expect_refused(missing_signature, "no_signature", scratch);
    OA_CHECK(fs::file_size(package_path) == package.size());
}

void append_le16(std::vector<uint8_t>& out, uint16_t value) {
    out.push_back(static_cast<uint8_t>(value & 0xffU));
    out.push_back(static_cast<uint8_t>((value >> 8U) & 0xffU));
}

void append_le32(std::vector<uint8_t>& out, uint32_t value) {
    append_le16(out, static_cast<uint16_t>(value & 0xffffU));
    append_le16(out, static_cast<uint16_t>((value >> 16U) & 0xffffU));
}

/// A stored archive whose names are not passed through the safe writer.
std::vector<uint8_t> unsafe_archive(std::string_view name, std::span<const uint8_t> bytes) {
    const uint32_t crc = zip::crc32_of(bytes);
    const auto size = static_cast<uint32_t>(bytes.size());
    const auto name_size = static_cast<uint16_t>(name.size());
    std::vector<uint8_t> out;
    const uint32_t local_signature = 0x04034b50U;
    const uint32_t central_signature = 0x02014b50U;
    const uint32_t end_signature = 0x06054b50U;
    append_le32(out, local_signature);
    append_le16(out, 20);
    append_le16(out, 0);
    append_le16(out, 0);
    append_le16(out, 0);
    append_le16(out, 33);
    append_le32(out, crc);
    append_le32(out, size);
    append_le32(out, size);
    append_le16(out, name_size);
    append_le16(out, 0);
    out.insert(out.end(), name.begin(), name.end());
    out.insert(out.end(), bytes.begin(), bytes.end());
    const auto directory_offset = static_cast<uint32_t>(out.size());
    append_le32(out, central_signature);
    append_le16(out, 20);
    append_le16(out, 20);
    append_le16(out, 0);
    append_le16(out, 0);
    append_le16(out, 0);
    append_le16(out, 33);
    append_le32(out, crc);
    append_le32(out, size);
    append_le32(out, size);
    append_le16(out, name_size);
    append_le16(out, 0);
    append_le16(out, 0);
    append_le16(out, 0);
    append_le16(out, 0);
    append_le32(out, 0);
    append_le32(out, 0);
    out.insert(out.end(), name.begin(), name.end());
    const auto directory_bytes = static_cast<uint32_t>(out.size() - directory_offset);
    append_le32(out, end_signature);
    append_le16(out, 0);
    append_le16(out, 0);
    append_le16(out, 1);
    append_le16(out, 1);
    append_le32(out, directory_bytes);
    append_le32(out, directory_offset);
    append_le16(out, 0);
    return out;
}

void expect_unpack_refused(const fs::path& archive, std::string_view top, const fs::path& stage) {
    std::string error;
    OA_CHECK(!helper::unpack_package(archive, stage, top, &error));
    OA_CHECK(!error.empty());
    std::error_code io;
    OA_CHECK(!fs::exists(stage, io));
}

void unpacking() {
    const Scratch scratch("unpack");
    constexpr std::string_view top = "open-annihilation-0.0.2-linux-x86_64";
    const fs::path stage = scratch.path / "stage";
    const std::vector<uint8_t> hello = bytes_of("hello");
    const std::vector<uint8_t> program = bytes_of("program");

    std::vector<uint8_t> good;
    zip::ZipError zip_error;
    const std::string hello_name = std::string(top) + "/hello.txt";
    const std::string program_name = std::string(top) + "/open-annihilation";
    const zip::NewEntry entries[] = {
        {hello_name, hello},
        {program_name, program},
    };
    OA_CHECK(zip::write_archive(entries, good, zip_error));
    const fs::path good_path = scratch.path / "good.zip";
    write_bytes(good_path, good);
    std::string error;
    OA_CHECK(helper::unpack_package(good_path, stage, top, &error));
    const fs::path hello_path = stage / helper::path_from(std::string(top) + "/hello.txt");
    std::ifstream hello_file(hello_path, std::ios::binary);
    const std::string hello_text(
        (std::istreambuf_iterator<char>(hello_file)), std::istreambuf_iterator<char>()
    );
    OA_CHECK(hello_text == "hello");
#if !defined(_WIN32)
    const fs::path executable = stage / helper::path_from(std::string(top) + "/open-annihilation");
    const fs::perms permissions = fs::status(executable).permissions();
    OA_CHECK((permissions & fs::perms::owner_exec) != fs::perms::none);
#endif
    std::error_code removed;
    fs::remove_all(stage, removed);

    std::vector<uint8_t> outside;
    const std::string outside_name = "other/file.txt";
    const zip::NewEntry outside_entries[] = {{outside_name, hello}};
    OA_CHECK(zip::write_archive(outside_entries, outside, zip_error));
    const fs::path outside_path = scratch.path / "outside.zip";
    write_bytes(outside_path, outside);
    std::string outside_error;
    OA_CHECK(!helper::unpack_package(outside_path, stage, top, &outside_error));
    OA_CHECK(outside_error.find("top folder") != std::string::npos);
    OA_CHECK(!fs::exists(stage));

    const std::vector<std::string> slipped = {
        "/tmp/oa-v07-slip",
        "../oa-v07-slip",
        "C:/oa-v07-slip",
        std::string(top) + "\\hello.txt",
    };
    for (const std::string& name : slipped) {
        const fs::path archive = scratch.path / "slipped.zip";
        write_bytes(archive, unsafe_archive(name, hello));
        expect_unpack_refused(archive, top, stage);
    }
    const fs::path slash_archive = scratch.path / "slash.zip";
    write_bytes(slash_archive, unsafe_archive(std::string(top) + "\\hello.txt", hello));
    std::string slash_error;
    OA_CHECK(!helper::unpack_package(slash_archive, stage, top, &slash_error));
    OA_CHECK(slash_error.find("top folder") != std::string::npos);
    OA_CHECK(!fs::exists(stage));
}

struct Listed {
    std::string name;
    std::string bytes;
};

std::vector<Listed> list_files(const fs::path& root) {
    std::vector<Listed> found;
    std::error_code io;
    if (!fs::exists(root, io) || io)
        return found;
    fs::recursive_directory_iterator cursor(root, io);
    const fs::recursive_directory_iterator end;
    while (!io && cursor != end) {
        std::error_code status_io;
        if (cursor->is_regular_file(status_io) && !status_io) {
            std::ifstream in(cursor->path(), std::ios::binary);
            Listed one;
            one.name = helper::utf8_of(cursor->path().lexically_relative(root));
            one.bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            found.push_back(std::move(one));
        }
        cursor.increment(io);
    }
    std::sort(found.begin(), found.end(), [](const Listed& left, const Listed& right) {
        return left.name < right.name;
    });
    return found;
}

bool same_files(const std::vector<Listed>& left, const std::vector<Listed>& right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (left[index].name != right[index].name || left[index].bytes != right[index].bytes)
            return false;
    }
    return true;
}

std::vector<helper::Operation> five_steps(const fs::path& tree, const fs::path& source) {
    std::vector<helper::Operation> operations;
    helper::Operation first;
    first.kind = helper::Operation::Kind::write_file;
    first.path = tree / "a.txt";
    first.bytes = "alpha";
    operations.push_back(std::move(first));

    helper::Operation second;
    second.kind = helper::Operation::Kind::write_file;
    second.path = tree / "b.txt";
    second.bytes = "beta";
    operations.push_back(std::move(second));

    helper::Operation third;
    third.kind = helper::Operation::Kind::rename;
    third.from = tree / "a.txt";
    third.to = tree / "c.txt";
    operations.push_back(std::move(third));

    helper::Operation fourth;
    fourth.kind = helper::Operation::Kind::copy_tree;
    fourth.from = source;
    fourth.to = tree / "d.txt";
    operations.push_back(std::move(fourth));

    helper::Operation fifth;
    fifth.kind = helper::Operation::Kind::remove_tree;
    fifth.path = tree / "c.txt";
    operations.push_back(std::move(fifth));
    return operations;
}

struct FailAt {
    int step{0};
};

bool fail_before(void* context, int step) {
    return static_cast<FailAt*>(context)->step == step;
}

bool stop_after(void* context, int step) {
    return static_cast<FailAt*>(context)->step == step;
}

void journal() {
    const Scratch scratch("journal");
    const fs::path source = scratch.path / "source.txt";
    write_bytes(source, bytes_of("source"));

    for (int step = 1; step <= 5; ++step) {
        const fs::path updates = scratch.path / ("step-" + std::to_string(step));
        const fs::path tree = updates / "tree";
        fs::create_directories(tree);
        const std::vector<Listed> before = list_files(tree);
        FailAt fail;
        fail.step = step;
        helper::JournalHooks hooks;
        hooks.context = &fail;
        hooks.fail_before_step = fail_before;
        std::string error;
        const helper::JournalResult result =
            helper::apply_journal(updates, five_steps(tree, source), hooks, &error);
        OA_CHECK(result == helper::JournalResult::failed);
        OA_CHECK(same_files(list_files(tree), before));
        std::error_code io;
        OA_CHECK(!fs::exists(updates / "journal.json", io));
        OA_CHECK(!fs::exists(updates / "journal-held", io));
    }

    const fs::path stopped = scratch.path / "stopped";
    const fs::path stopped_tree = stopped / "tree";
    fs::create_directories(stopped_tree);
    FailAt stop;
    stop.step = 2;
    helper::JournalHooks hooks;
    hooks.context = &stop;
    hooks.stop_after_step = stop_after;
    std::string error;
    const helper::JournalResult partial =
        helper::apply_journal(stopped, five_steps(stopped_tree, source), hooks, &error);
    OA_CHECK(partial == helper::JournalResult::stopped);
    OA_CHECK(fs::exists(stopped / "journal.json"));
    OA_CHECK(helper::recover_updates(stopped) == helper::exit_swapped);
    OA_CHECK(list_files(stopped_tree).empty());
    std::error_code io;
    OA_CHECK(!fs::exists(stopped / "journal.json", io));

    const fs::path done = scratch.path / "done";
    const fs::path done_tree = done / "tree";
    fs::create_directories(done_tree);
    helper::JournalHooks plain;
    const helper::JournalResult finished =
        helper::apply_journal(done, five_steps(done_tree, source), plain, &error);
    OA_CHECK(finished == helper::JournalResult::finished);
    OA_CHECK(fs::exists(done / "journal.json"));
    const std::vector<Listed> left = list_files(done_tree);
    OA_CHECK(left.size() == 2);
    bool saw_beta = false;
    bool saw_source = false;
    for (const Listed& one : left) {
        if (one.name == "b.txt" && one.bytes == "beta")
            saw_beta = true;
        if (one.name == "d.txt" && one.bytes == "source")
            saw_source = true;
    }
    OA_CHECK(saw_beta);
    OA_CHECK(saw_source);
}

struct Clock {
    int64_t now{0};
    int alive_calls{0};
    bool living{true};
};

int64_t clock_now(void* context) {
    return static_cast<Clock*>(context)->now;
}

void clock_sleep(void* context, int64_t milliseconds) {
    static_cast<Clock*>(context)->now += milliseconds;
}

bool clock_alive(void* context, int64_t) {
    Clock* clock = static_cast<Clock*>(context);
    ++clock->alive_calls;
    return clock->living;
}

void waiting() {
    Clock clock;
    helper::WaitHooks hooks;
    hooks.context = &clock;
    hooks.now_ms = clock_now;
    hooks.sleep_ms = clock_sleep;
    hooks.alive = clock_alive;
    std::string error;
    OA_CHECK(helper::wait_for_process(0, &hooks, &error) == helper::WaitOutcome::finished);
    OA_CHECK(clock.alive_calls == 0);
    OA_CHECK(error.empty());

    clock.living = false;
    OA_CHECK(helper::wait_for_process(4, &hooks, &error) == helper::WaitOutcome::finished);
    OA_CHECK(clock.alive_calls == 1);
    OA_CHECK(clock.now == 0);

    clock.living = true;
    clock.alive_calls = 0;
    OA_CHECK(helper::wait_for_process(4, &hooks, &error) == helper::WaitOutcome::still_running);
    OA_CHECK(error == helper::still_running_text);
    OA_CHECK(clock.now >= 60000);
}

void window() {
    const helper::PlacedUpdateWindow placed =
        helper::place_update_window("Installing 0.0.2", "0.0.1 is kept.", 400);
    OA_CHECK(placed.width == oa::ui::kit::notice_width);
    OA_CHECK(placed.height > 0);
    OA_CHECK(placed.line_text == "0.0.1 is kept.");
    OA_CHECK(placed.line_width > 0);
    OA_CHECK(placed.line_height > 0);
    OA_CHECK(placed.bar_width > 0);
    OA_CHECK(placed.bar_height > 0);

    const helper::PlacedUpdateWindow spinning =
        helper::place_update_window("Installing 0.0.2", "0.0.1 is kept.", -1);
    OA_CHECK(spinning.line_text == "0.0.1 is kept.");
    OA_CHECK(spinning.bar_width == 0);
}

} // namespace

void test_linux_versions_route();
void test_macos_route();

int main() {
    plan_and_state();
    verification();
    unpacking();
    journal();
    waiting();
    window();
    test_linux_versions_route();
    test_macos_route();
    return oa::test::check_exit_status();
}
