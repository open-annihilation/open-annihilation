// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// oa-tool engine-feed, run in-process. Keygen runs once. The packages are
// stand-in files: a Windows zip and its setup, and a macOS zip and its
// package, because one package holds one installer. Nothing here is a key
// the engine pins, and nothing is fetched except from the fixture on
// 127.0.0.1.

#include "command.hpp"

#include "oa/base/sha256.hpp"
#include "oa/data/engine_feed/feed.hpp"
#include "oa/netgame/http_fixture/server.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <cstdio>
#include <span>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <sstream>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace sha256 = oa::base::sha256;
namespace feed = oa::data::engine_feed;

struct Scratch {
    fs::path path;

    Scratch() : path(oa::test::make_scratch_directory("V01-engine-feed")) {}

    ~Scratch() {
        std::error_code error;
        fs::remove_all(path, error);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

struct Captured {
    int status = 0;
    std::string out;
    std::string err;
};

Captured run(std::vector<std::string> arguments) {
    std::ostringstream out;
    std::ostringstream err;
    oa::tool::Output output{out, err};
    const int status = oa::tool::run_tool(arguments, output);
    return {status, out.str(), err.str()};
}

bool contains(std::string_view text, std::string_view part) {
    return text.find(part) != std::string_view::npos;
}

void write_text(const fs::path& path, std::string_view text) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    OA_CHECK(static_cast<bool>(out));
}

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

std::string generic_utf8(const fs::path& path) {
    const auto text = path.generic_u8string();
    return {text.begin(), text.end()};
}

std::string line_after(std::string_view text, std::string_view prefix) {
    const auto at = text.find(prefix);
    if (at == std::string_view::npos)
        return {};
    const auto start = at + prefix.size();
    const auto end = text.find('\n', start);
    if (end == std::string_view::npos)
        return std::string(text.substr(start));
    return std::string(text.substr(start, end - start));
}

void show(std::string_view label, const Captured& captured) {
    std::fprintf(
        stderr,
        "%.*s: status %d\n%.*s%.*s",
        static_cast<int>(label.size()),
        label.data(),
        captured.status,
        static_cast<int>(captured.out.size()),
        captured.out.data(),
        static_cast<int>(captured.err.size()),
        captured.err.data()
    );
}

bool exited(std::string_view label, const Captured& captured, int status) {
    if (captured.status == status)
        return true;
    show(label, captured);
    OA_CHECK(captured.status == status);
    return false;
}

std::string hex_of(std::string_view text) {
    const auto digest = sha256::digest_of(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size())
    );
    const auto digits = sha256::to_hex(digest);
    return {digits.data(), digits.size()};
}

void help_text() {
    const auto write_help = run({"engine-feed", "write", "--help"});
    exited("write help", write_help, oa::tool::exit_done);
    OA_CHECK(contains(
        write_help.out,
        "--version V [--release N] --date YYYY-MM-DD --from V --page URL --mirror URL "
        "[--mirror URL]... [--notes FILE] [--rollout N] [--online-min V] [--channel stable] "
        "[--previous FEED] --out FEED PACKAGE..."
    ));
    const auto check_help = run({"engine-feed", "check", "--help"});
    exited("check help", check_help, oa::tool::exit_done);
    OA_CHECK(contains(
        check_help.out,
        "(FEED [--sig SIG] | --url URL) [--channel stable] [--key ID=ed25519:BASE64]..."
    ));
}

void wrong_version(const fs::path& root) {
    const fs::path package = root / "open-annihilation-v0.8.6-windows-x64.zip";
    write_text(package, "old");
    const fs::path out = root / "refused.json";
    const auto refused = run(
        {"engine-feed",
         "write",
         "--version",
         "0.8.7",
         "--date",
         "2026-10-24",
         "--from",
         "0.7.0",
         "--page",
         "https://example.net/v0.8.7",
         "--mirror",
         "http://downloads.example.net/v1/engine/0.8.7/",
         "--out",
         generic_utf8(out),
         generic_utf8(package)}
    );
    exited("other version", refused, oa::tool::exit_failed);
    OA_CHECK(contains(refused.err, "another version"));
    OA_CHECK(!fs::exists(out));
}

void write_sign_check(const fs::path& root) {
    const fs::path pass = root / "pass";
    write_text(pass, "correct horse battery\n");
    const fs::path key = root / "feed-key";
    const auto made = run(
        {"catalogue",
         "keygen",
         "--id",
         "feed-test",
         "--out",
         generic_utf8(key),
         "--passphrase-file",
         generic_utf8(pass)}
    );
    if (!exited("keygen", made, oa::tool::exit_done))
        return;
    const std::string public_key = line_after(made.out, "public: ");
    OA_CHECK(public_key.starts_with("ed25519:"));
    if (!public_key.starts_with("ed25519:"))
        return;

    constexpr std::string_view windows_zip = "windows-zip-bytes";
    constexpr std::string_view windows_setup = "windows-setup-bytes";
    constexpr std::string_view macos_zip = "macos-zip-bytes";
    constexpr std::string_view macos_pkg = "macos-pkg-bytes";
    const fs::path windows_zip_path = root / "open-annihilation-0.8.7-windows-x64.zip";
    const fs::path windows_setup_path = root / "open-annihilation-0.8.7-windows-x64-setup.exe";
    const fs::path macos_zip_path = root / "open-annihilation-0.8.7-macos-universal.zip";
    const fs::path macos_pkg_path = root / "open-annihilation-0.8.7-macos-universal.pkg";
    write_text(windows_zip_path, windows_zip);
    write_text(windows_setup_path, windows_setup);
    write_text(macos_zip_path, macos_zip);
    write_text(macos_pkg_path, macos_pkg);

    const fs::path out = root / "stable.json";
    const auto wrote = run(
        {"engine-feed",
         "write",
         "--version",
         "0.8.7",
         "--date",
         "2026-10-24",
         "--from",
         "0.7.0",
         "--page",
         "https://example.net/v0.8.7",
         "--mirror",
         "http://downloads.example.net/v1/engine/0.8.7/",
         "--out",
         generic_utf8(out),
         generic_utf8(windows_zip_path),
         generic_utf8(windows_setup_path),
         generic_utf8(macos_zip_path),
         generic_utf8(macos_pkg_path)}
    );
    if (!exited("write", wrote, oa::tool::exit_done))
        return;
    OA_CHECK(contains(wrote.out, "release 807"));

    const std::vector<uint8_t> bytes = read_bytes(out);
    std::string error;
    const auto loaded = feed::read_feed(bytes, &error);
    if (!loaded) {
        std::fprintf(stderr, "read_feed: %s\n", error.c_str());
    }
    OA_CHECK(loaded.has_value());
    if (!loaded || loaded->releases.size() != 1)
        return;
    const feed::Release& release = loaded->releases[0];
    OA_CHECK(release.release == 807);
    OA_CHECK(release.version_text == "0.8.7");
    OA_CHECK(release.packages.size() == 2);
    if (release.packages.size() != 2)
        return;

    const feed::Package* windows = nullptr;
    const feed::Package* macos = nullptr;
    for (const feed::Package& package : release.packages) {
        if (package.id == "windows-x64")
            windows = &package;
        if (package.id == "macos-universal")
            macos = &package;
    }
    OA_CHECK(windows != nullptr && macos != nullptr);
    if (windows == nullptr || macos == nullptr)
        return;
    OA_CHECK(windows->zip.size == windows_zip.size());
    OA_CHECK(macos->zip.size == macos_zip.size());
    OA_CHECK(windows->installer.has_value() && macos->installer.has_value());
    if (windows->installer && macos->installer) {
        OA_CHECK(windows->installer->size == windows_setup.size());
        OA_CHECK(macos->installer->size == macos_pkg.size());
        const auto windows_zip_hash = sha256::parse_hex(hex_of(windows_zip));
        const auto windows_setup_hash = sha256::parse_hex(hex_of(windows_setup));
        const auto macos_zip_hash = sha256::parse_hex(hex_of(macos_zip));
        const auto macos_pkg_hash = sha256::parse_hex(hex_of(macos_pkg));
        OA_CHECK(windows_zip_hash && windows->zip.sha256 == *windows_zip_hash);
        OA_CHECK(windows_setup_hash && windows->installer->sha256 == *windows_setup_hash);
        OA_CHECK(macos_zip_hash && macos->zip.sha256 == *macos_zip_hash);
        OA_CHECK(macos_pkg_hash && macos->installer->sha256 == *macos_pkg_hash);
    }

    const auto signed_feed = run(
        {"engine-feed",
         "sign",
         "--key",
         generic_utf8(key),
         "--passphrase-file",
         generic_utf8(pass),
         generic_utf8(out)}
    );
    if (!exited("sign", signed_feed, oa::tool::exit_done))
        return;
    OA_CHECK(contains(signed_feed.out, "signed "));
    OA_CHECK(contains(signed_feed.out, "with feed-test"));
    const fs::path sig = root / "stable.json.sig";
    OA_CHECK(fs::exists(sig));

    const auto checked =
        run({"engine-feed", "check", "--key", "feed-test=" + public_key, generic_utf8(out)});
    if (!exited("check", checked, oa::tool::exit_done))
        return;
    OA_CHECK(contains(checked.out, "good signature by feed-test"));
    OA_CHECK(contains(checked.out, "807 0.8.7 2026-10-24 from 0.7.0"));
    OA_CHECK(contains(checked.out, "windows-x64 " + std::to_string(windows_zip.size())));
    OA_CHECK(contains(checked.out, "macos-universal " + std::to_string(macos_zip.size())));

    const auto unpinned = run({"engine-feed", "check", generic_utf8(out)});
    exited("unpinned", unpinned, oa::tool::exit_failed);
    OA_CHECK(contains(unpinned.out, "unknown_key"));

    oa::netgame::http_fixture::Server server;
    std::string server_error;
    OA_CHECK(server.start(&server_error));
    if (!server_error.empty())
        std::fprintf(stderr, "fixture: %s\n", server_error.c_str());
    const std::vector<uint8_t> signature = read_bytes(sig);
    server.serve_bytes("/v1/engine/stable.json", bytes, "application/json");
    server.serve_bytes("/v1/engine/stable.json.sig", signature, "text/plain");
    const auto fetched = run(
        {"engine-feed",
         "check",
         "--url",
         server.url("/v1/engine/stable.json"),
         "--key",
         "feed-test=" + public_key}
    );
    if (!exited("url", fetched, oa::tool::exit_done))
        return;
    OA_CHECK(contains(fetched.out, "good signature by feed-test"));
    const auto log = server.requests();
    OA_CHECK(log.size() == 2);
    if (log.size() == 2) {
        OA_CHECK(log[0].method == "GET");
        OA_CHECK(log[1].method == "GET");
        OA_CHECK(log[0].target == "/v1/engine/stable.json");
        OA_CHECK(log[1].target == "/v1/engine/stable.json.sig");
        OA_CHECK(log[0].target.find('?') == std::string::npos);
        OA_CHECK(log[1].target.find('?') == std::string::npos);
        const std::string* agent = log[0].header("user-agent");
        OA_CHECK(agent != nullptr && agent->starts_with("OpenAnnihilation/"));
    }
    server.stop();
}

} // namespace

int main() {
    help_text();
    Scratch scratch;
    wrong_version(scratch.path);
    write_sign_check(scratch.path);
    return oa::test::check_exit_status();
}
