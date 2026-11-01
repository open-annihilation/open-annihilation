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
#include "oa/formats/oamod/package_keys.hpp"
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
        "--version V [--release N] --date YYYY-MM-DD --from V [--package-from ID=V]... "
        "--page URL [--install-page URL] --mirror URL [--mirror URL]... [--notes FILE] "
        "[--rollout N] [--online-min V] [--ping-url URL --ping-registry ID] "
        "[--channel stable|testing] [--previous FEED] --out FEED PACKAGE..."
    ));
    const auto check_help = run({"engine-feed", "check", "--help"});
    exited("check help", check_help, oa::tool::exit_done);
    OA_CHECK(contains(
        check_help.out,
        "(FEED [--sig SIG] | --url URL) [--channel stable|testing] [--key ID=ed25519:BASE64]..."
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

/// engine-feed write for 0.8.12 with the extra options, from the stand-in
/// packages `write_sign_check` made beside `out`.
Captured
write_minimum(const fs::path& root, const fs::path& out, const std::vector<std::string>& extra) {
    std::vector<std::string> arguments = {
        "engine-feed",
        "write",
        "--version",
        "0.8.12",
        "--date",
        "2026-11-02",
        "--from",
        "0.8.0",
        "--page",
        "https://example.net/v0.8.12",
        "--mirror",
        "http://downloads.example.net/v1/engine/0.8.12/",
        "--out",
        generic_utf8(out),
    };
    arguments.insert(arguments.end(), extra.begin(), extra.end());
    arguments.push_back(generic_utf8(root / "open-annihilation-0.8.12-windows-x64.zip"));
    arguments.push_back(generic_utf8(root / "open-annihilation-0.8.12-macos-universal.zip"));
    return run(arguments);
}

/// A macOS minimum above the release's and an install page: the feed carries
/// both, check prints them, and the options' mistakes are refused.
void package_minimum(
    const fs::path& root, const fs::path& key, const fs::path& pass, const std::string& public_key
) {
    constexpr std::string_view windows_zip = "windows-0812-bytes";
    constexpr std::string_view macos_zip = "macos-0812-bytes!";
    write_text(root / "open-annihilation-0.8.12-windows-x64.zip", windows_zip);
    write_text(root / "open-annihilation-0.8.12-macos-universal.zip", macos_zip);
    const fs::path out = root / "minimum.json";
    const auto wrote = write_minimum(
        root,
        out,
        {"--package-from",
         "macos-universal=0.8.12",
         "--install-page",
         "https://example.net/install"}
    );
    if (!exited("write minimum", wrote, oa::tool::exit_done))
        return;
    OA_CHECK(contains(wrote.out, "release 812"));
    const std::vector<uint8_t> bytes = read_bytes(out);
    std::string error;
    const auto loaded = feed::read_feed(bytes, &error);
    if (!loaded)
        std::fprintf(stderr, "read_feed: %s\n", error.c_str());
    OA_CHECK(loaded.has_value());
    if (!loaded || loaded->releases.size() != 1)
        return;
    const feed::Release& release = loaded->releases[0];
    OA_CHECK(release.from == (oa::formats::oamod::EngineVersion{0, 8, 0}));
    OA_CHECK(release.install_page == "https://example.net/install");
    OA_CHECK(release.page == "https://example.net/v0.8.12");
    for (const feed::Package& package : release.packages) {
        if (package.id == "macos-universal") {
            OA_CHECK(package.from.has_value());
            if (package.from)
                OA_CHECK((*package.from == oa::formats::oamod::EngineVersion{0, 8, 12}));
        } else {
            OA_CHECK(package.id == "windows-x64");
            OA_CHECK(!package.from.has_value());
        }
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
    if (!exited("sign minimum", signed_feed, oa::tool::exit_done))
        return;
    const auto checked =
        run({"engine-feed", "check", "--key", "feed-test=" + public_key, generic_utf8(out)});
    if (!exited("check minimum", checked, oa::tool::exit_done))
        return;
    OA_CHECK(contains(checked.out, "812 0.8.12 2026-11-02 from 0.8.0"));
    OA_CHECK(contains(
        checked.out, "macos-universal " + std::to_string(macos_zip.size()) + " from 0.8.12"
    ));
    OA_CHECK(contains(checked.out, "windows-x64 " + std::to_string(windows_zip.size())));
    OA_CHECK(!contains(checked.out, "windows-x64 " + std::to_string(windows_zip.size()) + " from"));
    OA_CHECK(contains(checked.out, " install_page https://example.net/install\n"));

    struct Refusal {
        std::vector<std::string> extra;
        std::string_view reason;
    };

    const Refusal refusals[] = {
        {{"--package-from", "linux-x86_64=0.8.12"}, "no package given is linux-x86_64"},
        {{"--package-from", "macos-universal=0.8"},
         "the from for macos-universal is not a version"},
        {{"--package-from", "macos-universal"}, "a package's from must be a package id"},
        {{"--package-from", "macos-universal=0.8.12", "--package-from", "macos-universal=0.8.13"},
         "two froms for macos-universal"},
        {{"--install-page", "ftp://example.net/install"},
         "install_page is not an http or https address"},
    };
    for (const Refusal& refusal : refusals) {
        const fs::path refused_out = root / "refused-minimum.json";
        const auto refused = write_minimum(root, refused_out, refusal.extra);
        exited(refusal.reason, refused, oa::tool::exit_failed);
        if (!contains(refused.err, refusal.reason))
            show(refusal.reason, refused);
        OA_CHECK(contains(refused.err, refusal.reason));
        OA_CHECK(!fs::exists(refused_out));
    }
}

/// The testing channel's feed, testing-2.json, written with --channel
/// testing and a ping: check accepts it as testing, prints the ping, and
/// refuses it as stable; the stable feed is refused as testing, and an
/// earlier feed of the other channel is not copied in. The ping options are
/// given together, as an address and a registry id.
void testing_channel_and_ping(
    const fs::path& root,
    const fs::path& key,
    const fs::path& pass,
    const std::string& public_key,
    const fs::path& stable
) {
    const fs::path out = root / "testing-2.json";
    const auto wrote = write_minimum(
        root,
        out,
        {"--channel",
         "testing",
         "--ping-url",
         "https://telemetry.example.net/v1/engine/pings",
         "--ping-registry",
         "example"}
    );
    if (!exited("write testing", wrote, oa::tool::exit_done))
        return;
    const std::vector<uint8_t> bytes = read_bytes(out);
    std::string error;
    const auto loaded = feed::read_feed(bytes, &error);
    OA_CHECK(loaded.has_value());
    if (!loaded)
        return;
    OA_CHECK(loaded->channel == "testing");
    OA_CHECK(loaded->ping.has_value());
    if (loaded->ping) {
        OA_CHECK(loaded->ping->url == "https://telemetry.example.net/v1/engine/pings");
        OA_CHECK(loaded->ping->registry == "example");
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
    if (!exited("sign testing", signed_feed, oa::tool::exit_done))
        return;
    OA_CHECK(fs::exists(root / "testing-2.json.sig"));
    const std::string pin = "feed-test=" + public_key;
    const auto checked =
        run({"engine-feed", "check", "--channel", "testing", "--key", pin, generic_utf8(out)});
    if (!exited("check testing", checked, oa::tool::exit_done))
        return;
    OA_CHECK(contains(checked.out, "good signature by feed-test\n"));
    OA_CHECK(contains(
        checked.out, "ping https://telemetry.example.net/v1/engine/pings registry example\n"
    ));
    OA_CHECK(contains(checked.out, "812 0.8.12 2026-11-02 from 0.8.0"));

    const auto as_stable = run({"engine-feed", "check", "--key", pin, generic_utf8(out)});
    exited("testing as stable", as_stable, oa::tool::exit_failed);
    OA_CHECK(contains(as_stable.out, "wrong_channel"));
    const auto stable_as_testing =
        run({"engine-feed", "check", "--channel", "testing", "--key", pin, generic_utf8(stable)});
    exited("stable as testing", stable_as_testing, oa::tool::exit_failed);
    OA_CHECK(contains(stable_as_testing.out, "wrong_channel"));
    const auto stable_checked =
        run({"engine-feed", "check", "--channel", "stable", "--key", pin, generic_utf8(stable)});
    exited("stable as stable", stable_checked, oa::tool::exit_done);
    OA_CHECK(!contains(stable_checked.out, "ping "));

    struct Refusal {
        std::vector<std::string> extra;
        std::string_view reason;
    };

    const Refusal refusals[] = {
        {{"--ping-url", "https://telemetry.example.net/v1/engine/pings"},
         "--ping-url and --ping-registry are given together"},
        {{"--ping-registry", "example"}, "--ping-url and --ping-registry are given together"},
        {{"--ping-url", "ftp://example.net/pings", "--ping-registry", "example"},
         "ping's url is not an http or https address"},
        {{"--ping-url", "https://example.net/pings", "--ping-registry", "Not An Id"},
         "ping's registry is not a registry id"},
        {{"--channel", "testing", "--previous", generic_utf8(stable)},
         "the earlier feed is for another channel"},
    };
    for (const Refusal& refusal : refusals) {
        const fs::path refused_out = root / "refused-testing.json";
        const auto refused = write_minimum(root, refused_out, refusal.extra);
        exited(refusal.reason, refused, oa::tool::exit_failed);
        if (!contains(refused.err, refusal.reason))
            show(refusal.reason, refused);
        OA_CHECK(contains(refused.err, refusal.reason));
        OA_CHECK(!fs::exists(refused_out));
    }

    oa::netgame::http_fixture::Server server;
    std::string server_error;
    OA_CHECK(server.start(&server_error));
    const std::vector<uint8_t> signature = read_bytes(root / "testing-2.json.sig");
    server.serve_bytes("/v1/engine/testing-2.json", bytes, "application/json");
    server.serve_bytes("/v1/engine/testing-2.json.sig", signature, "text/plain");
    const auto fetched = run(
        {"engine-feed",
         "check",
         "--url",
         server.url("/v1/engine/testing-2.json"),
         "--channel",
         "testing",
         "--key",
         pin}
    );
    exited("url testing", fetched, oa::tool::exit_done);
    OA_CHECK(contains(fetched.out, "good signature by feed-test"));
    server.stop();
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

    package_minimum(root, key, pass, public_key);
    testing_channel_and_ping(root, key, pass, public_key, out);

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
