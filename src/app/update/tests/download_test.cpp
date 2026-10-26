// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The package download against a local server: a whole file, a resume, a
// restart, a pause, a bad digest and a mirror that fails.

#include "oa/app/update/download.hpp"

#include "oa/test/check.hpp"

#if OA_SELF_UPDATE

#include "oa/base/sha256.hpp"
#include "oa/formats/url.hpp"
#include "oa/netgame/http_fixture/server.hpp"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace update = oa::app::update;
namespace feed = oa::data::engine_feed;
namespace http = oa::netgame::http_fixture;
namespace sha = oa::base::sha256;
namespace url = oa::formats::url;

/// A scratch folder removed when the case ends.
struct Scratch {
    fs::path path;

    explicit Scratch(std::string_view name) {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
               (std::string("oa-v13-") + std::string(name) + "-" + std::to_string(ticks));
        fs::create_directories(path);
    }

    ~Scratch() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

/// Waits until `pred` holds, or `ms` milliseconds have passed.
template <typename Pred>
bool until(Pred pred, int ms) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return pred();
}

url::Url must_url(std::string_view text) {
    const std::optional<url::Url> parsed = url::parse_web_url(text);
    OA_CHECK(parsed.has_value());
    return parsed.has_value() ? *parsed : url::Url{};
}

std::vector<uint8_t> patterned(std::size_t size, uint8_t salt) {
    std::vector<uint8_t> bytes(size);
    for (std::size_t index = 0; index < bytes.size(); ++index)
        bytes[index] = static_cast<uint8_t>(salt + static_cast<uint8_t>(index));
    return bytes;
}

feed::PackageFile file_for(std::string name, const std::vector<uint8_t>& bytes) {
    feed::PackageFile file;
    file.file = std::move(name);
    file.size = static_cast<uint64_t>(bytes.size());
    file.sha256 = sha::digest_of(bytes);
    return file;
}

feed::Release release_numbered(int64_t number) {
    feed::Release release;
    release.release = number;
    release.version_text = "0.0.2";
    return release;
}

feed::Package package_on(feed::PackageFile file, std::vector<url::Url> mirrors) {
    feed::Package package;
    package.id = "linux-x86_64";
    package.zip = std::move(file);
    package.mirrors = std::move(mirrors);
    return package;
}

bool ready_file(const update::DownloadSnapshot& shot, const fs::path& path) {
    return shot.state == update::DownloadState::ready && shot.path == path && shot.failure.empty();
}

void a_whole_download_and_an_older_folder() {
    Scratch scratch("whole");
    const fs::path updates = scratch.path / "updates";
    fs::create_directories(updates / "1");
    fs::create_directories(updates / "3");
    fs::create_directories(updates / "saved");
    {
        std::ofstream feed(updates / "feed.json");
        feed << "{}\n";
        std::ofstream old_file(updates / "1" / "old.bin");
        old_file << "old\n";
    }
    http::Server server;
    std::string error;
    OA_CHECK(server.start(&error));
    const std::vector<uint8_t> bytes = patterned(48, 3);
    server.serve_bytes("/pkg.zip", bytes);
    update::PackageDownload download;
    const feed::PackageFile file = file_for("pkg.zip", bytes);
    download.start(
        release_numbered(2), package_on(file, {must_url(server.url("/"))}), false, updates
    );
    const fs::path finished = updates / "2" / "pkg.zip";
    OA_CHECK(until([&] { return ready_file(download.snapshot(), finished); }, 5000));
    const update::DownloadSnapshot shot = download.snapshot();
    OA_CHECK(shot.received == bytes.size());
    OA_CHECK(shot.total == bytes.size());
    std::ifstream in(finished, std::ios::binary);
    const std::vector<uint8_t> got{std::istreambuf_iterator<char>(in), {}};
    OA_CHECK(got == bytes);
    OA_CHECK(!fs::exists(updates / "1"));
    OA_CHECK(fs::exists(updates / "3"));
    OA_CHECK(fs::exists(updates / "saved"));
    OA_CHECK(fs::exists(updates / "feed.json"));
}

void a_cut_resumes_with_a_range() {
    Scratch scratch("resume");
    const fs::path updates = scratch.path / "updates";
    http::Server server;
    std::string error;
    OA_CHECK(server.start(&error));
    server.set_ranges(true);
    const std::size_t step = update::download_step_bytes;
    const std::vector<uint8_t> bytes = patterned(step + 64, 9);
    server.serve_bytes("/pkg.zip", bytes);
    http::Reply cut;
    cut.body = bytes;
    cut.cut_after = step + 8;
    server.override_next("/pkg.zip", cut, 1);
    update::PackageDownload download;
    download.start(
        release_numbered(4),
        package_on(file_for("pkg.zip", bytes), {must_url(server.url("/"))}),
        false,
        updates
    );
    const fs::path finished = updates / "4" / "pkg.zip";
    OA_CHECK(until([&] { return ready_file(download.snapshot(), finished); }, 15000));
    const std::vector<http::LoggedRequest> log = server.requests();
    OA_CHECK(log.size() >= 2);
    bool ranged = false;
    if (log.size() >= 2) {
        const std::string* header = log[1].header("Range");
        ranged = header != nullptr && *header == "bytes=" + std::to_string(step) + "-";
    }
    OA_CHECK(ranged);
    std::ifstream in(finished, std::ios::binary);
    const std::vector<uint8_t> got{std::istreambuf_iterator<char>(in), {}};
    OA_CHECK(got == bytes);
}

void a_whole_answer_to_a_range_restarts() {
    Scratch scratch("restart");
    const fs::path updates = scratch.path / "updates";
    http::Server server;
    std::string error;
    OA_CHECK(server.start(&error));
    const std::size_t step = update::download_step_bytes;
    const std::vector<uint8_t> bytes = patterned(step + 80, 11);
    server.serve_bytes("/pkg.zip", bytes);
    http::Reply cut;
    cut.body = bytes;
    cut.cut_after = step + 8;
    server.override_next("/pkg.zip", cut, 1);
    http::Reply whole;
    whole.status = 200;
    whole.body = bytes;
    server.override_next("/pkg.zip", whole, 1);
    update::PackageDownload download;
    download.start(
        release_numbered(5),
        package_on(file_for("pkg.zip", bytes), {must_url(server.url("/"))}),
        false,
        updates
    );
    const fs::path finished = updates / "5" / "pkg.zip";
    OA_CHECK(until([&] { return ready_file(download.snapshot(), finished); }, 15000));
    const std::vector<http::LoggedRequest> log = server.requests();
    OA_CHECK(log.size() >= 2);
    bool ranged = false;
    if (log.size() >= 2) {
        const std::string* header = log[1].header("Range");
        ranged = header != nullptr && header->find("bytes=") == 0;
    }
    OA_CHECK(ranged);
    std::ifstream in(finished, std::ios::binary);
    const std::vector<uint8_t> got{std::istreambuf_iterator<char>(in), {}};
    OA_CHECK(got == bytes);
}

void a_match_pauses_at_a_step_and_then_resumes() {
    Scratch scratch("pause");
    const fs::path updates = scratch.path / "updates";
    http::Server server;
    std::string error;
    OA_CHECK(server.start(&error));
    server.set_ranges(true);
    const std::vector<uint8_t> small = patterned(32, 1);
    server.serve_bytes("/small.zip", small);
    update::PackageDownload download;
    download.set_match_running(true);
    download.start(
        release_numbered(6),
        package_on(file_for("small.zip", small), {must_url(server.url("/"))}),
        false,
        updates
    );
    OA_CHECK(until(
        [&] { return download.snapshot().state == update::DownloadState::paused_for_match; }, 5000
    ));
    OA_CHECK(server.requests().empty());
    OA_CHECK(download.snapshot().received == 0);
    download.set_match_running(false);
    const fs::path small_done = updates / "6" / "small.zip";
    OA_CHECK(until([&] { return ready_file(download.snapshot(), small_done); }, 5000));

    const std::size_t step = update::download_step_bytes;
    const std::vector<uint8_t> bytes = patterned(step + 48, 21);
    server.serve_bytes("/wide.zip", bytes);
    http::Reply slow;
    slow.body = bytes;
    slow.head_delay_ms = 1000;
    server.override_next("/wide.zip", slow, 1);
    server.clear_log();
    download.start(
        release_numbered(7),
        package_on(file_for("wide.zip", bytes), {must_url(server.url("/"))}),
        false,
        updates
    );
    OA_CHECK(until([&] { return !server.requests().empty(); }, 5000));
    download.set_match_running(true);
    OA_CHECK(until(
        [&] {
            const update::DownloadSnapshot shot = download.snapshot();
            return shot.state == update::DownloadState::paused_for_match && shot.received == step;
        },
        15000
    ));
    const update::DownloadSnapshot paused = download.snapshot();
    OA_CHECK(paused.received == step);
    OA_CHECK(paused.received < paused.total);
    download.set_match_running(false);
    const fs::path finished = updates / "7" / "wide.zip";
    OA_CHECK(until([&] { return ready_file(download.snapshot(), finished); }, 15000));
    const std::vector<http::LoggedRequest> log = server.requests();
    bool ranged = false;
    for (const http::LoggedRequest& request : log) {
        const std::string* header = request.header("Range");
        if (header != nullptr && *header == "bytes=" + std::to_string(step) + "-")
            ranged = true;
    }
    OA_CHECK(ranged);
}

void a_wrong_digest_deletes_the_part() {
    Scratch scratch("digest");
    const fs::path updates = scratch.path / "updates";
    http::Server server;
    std::string error;
    OA_CHECK(server.start(&error));
    const std::vector<uint8_t> bytes = patterned(40, 5);
    const std::vector<uint8_t> other = patterned(40, 6);
    server.serve_bytes("/pkg.zip", other);
    feed::PackageFile file = file_for("pkg.zip", bytes);
    update::PackageDownload download;
    download.start(
        release_numbered(8), package_on(file, {must_url(server.url("/"))}), false, updates
    );
    OA_CHECK(
        until([&] { return download.snapshot().state == update::DownloadState::failed; }, 5000)
    );
    const update::DownloadSnapshot shot = download.snapshot();
    OA_CHECK(shot.failure == update::wrong_file_failure);
    OA_CHECK(!fs::exists(updates / "8" / "pkg.zip"));
    OA_CHECK(!fs::exists(updates / "8" / "pkg.zip.part"));
}

void a_failed_mirror_moves_to_the_next() {
    Scratch scratch("mirrors");
    const fs::path updates = scratch.path / "updates";
    http::Server server;
    std::string error;
    OA_CHECK(server.start(&error));
    const std::vector<uint8_t> bytes = patterned(24, 7);
    server.handle("GET", "/a/pkg.zip", [](const http::LoggedRequest&) {
        http::Reply reply;
        reply.status = 500;
        reply.body = {'n', 'o'};
        return reply;
    });
    server.serve_bytes("/b/pkg.zip", bytes);
    update::PackageDownload download;
    download.start(
        release_numbered(9),
        package_on(
            file_for("pkg.zip", bytes), {must_url(server.url("/a/")), must_url(server.url("/b/"))}
        ),
        false,
        updates
    );
    const fs::path finished = updates / "9" / "pkg.zip";
    OA_CHECK(until([&] { return ready_file(download.snapshot(), finished); }, 5000));
    const std::vector<http::LoggedRequest> log = server.requests();
    bool saw_bad = false;
    bool saw_good = false;
    for (const http::LoggedRequest& request : log) {
        if (request.target.find("/a/pkg.zip") != std::string::npos)
            saw_bad = true;
        if (request.target.find("/b/pkg.zip") != std::string::npos)
            saw_good = true;
    }
    OA_CHECK(saw_bad);
    OA_CHECK(saw_good);
}

void https_is_skipped_without_tls() {
    Scratch scratch("tls");
    const fs::path updates = scratch.path / "updates";
    http::Server server;
    std::string error;
    OA_CHECK(server.start(&error));
    const std::vector<uint8_t> bytes = patterned(16, 8);
    server.serve_bytes("/b/pkg.zip", bytes);
    const std::string https = "https://127.0.0.1:" + std::to_string(server.port()) + "/a/";
    update::PackageDownload download;
    download.set_tls_available(false);
    download.start(
        release_numbered(10),
        package_on(file_for("pkg.zip", bytes), {must_url(https), must_url(server.url("/b/"))}),
        false,
        updates
    );
    const fs::path finished = updates / "10" / "pkg.zip";
    OA_CHECK(until([&] { return ready_file(download.snapshot(), finished); }, 5000));
    const std::vector<http::LoggedRequest> log = server.requests();
    OA_CHECK(log.size() == 1);
    bool only_http = !log.empty();
    for (const http::LoggedRequest& request : log) {
        if (request.target.find("/a/") != std::string::npos)
            only_http = false;
        if (request.target.find("/b/pkg.zip") == std::string::npos)
            only_http = false;
    }
    OA_CHECK(only_http);
}

void a_file_name_with_a_slash_is_refused() {
    Scratch scratch("name");
    const fs::path updates = scratch.path / "updates";
    fs::create_directories(updates);
    const std::vector<uint8_t> bytes = patterned(8, 2);
    feed::PackageFile file = file_for("pkg.zip", bytes);
    file.file = "a/pkg.zip";
    update::PackageDownload download;
    download.start(release_numbered(11), package_on(file, {url::Url{}}), false, updates);
    OA_CHECK(
        until([&] { return download.snapshot().state == update::DownloadState::failed; }, 2000)
    );
    OA_CHECK(!fs::exists(updates / "11"));
    OA_CHECK(download.snapshot().failure == "the file name is not a plain name");
}

} // namespace

#endif

int main() {
#if OA_SELF_UPDATE
    a_whole_download_and_an_older_folder();
    a_cut_resumes_with_a_range();
    a_whole_answer_to_a_range_restarts();
    a_match_pauses_at_a_step_and_then_resumes();
    a_wrong_digest_deletes_the_part();
    a_failed_mirror_moves_to_the_next();
    https_is_skipped_without_tls();
    a_file_name_with_a_slash_is_refused();
#endif
    return oa::test::check_exit_status();
}
