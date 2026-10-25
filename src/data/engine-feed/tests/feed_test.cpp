// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The signed engine feed. Keys come from fixed seeds. No key here is one
// the engine pins, and nothing is fetched.

#include "oa/data/engine_feed/feed.hpp"

#include "oa/base/sha256.hpp"
#include "oa/base/signing/ed25519.hpp"
#include "oa/data/catalogue/check.hpp"
#include "oa/data/registry/descriptor.hpp"
#include "oa/data/registry/player_registries.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/formats/url.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace feed = oa::data::engine_feed;
namespace signing = oa::base::signing;
namespace sha256 = oa::base::sha256;
namespace catalogue = oa::data::catalogue;
namespace registry = oa::data::registry;
namespace oamod = oa::formats::oamod;
namespace url = oa::formats::url;

constexpr std::string_view kSha =
    "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr std::string_view kInstallerSha =
    "fedcba9876543210fedcba9876543210fedcba9876543210fedcba9876543210";

/// A feed and the signature a test key made over its exact bytes.
struct Signed {
    std::vector<uint8_t> bytes;
    std::vector<uint8_t> signature;
    std::string key_id;
    signing::KeyText key_text{};
};

/// Signs `json` with a key derived from a fixed seed. The seed and the secret are wiped.
Signed sign_text(std::string_view json, std::string_view key_id, uint8_t fill) {
    std::array<uint8_t, 32> seed{};
    seed.fill(fill);
    signing::SecretKey secret{};
    signing::PublicKey public_key{};
    signing::key_pair_from_seed(seed, secret, public_key);
    seed.fill(0);
    Signed signed_feed;
    signed_feed.bytes.assign(json.begin(), json.end());
    const signing::Signature signature = signing::sign(secret, signed_feed.bytes);
    secret.fill(0);
    const std::string text =
        catalogue::signature_file_text(catalogue::SignatureLine{std::string(key_id), signature});
    signed_feed.signature.assign(text.begin(), text.end());
    signed_feed.key_id = std::string(key_id);
    signed_feed.key_text = signing::public_key_text(public_key);
    return signed_feed;
}

/// Checks `signed_feed` against the one key that signed it.
feed::Checked check_signed(const Signed& signed_feed, std::string_view channel = "stable") {
    registry::PinnedKey pin;
    pin.registry = "test";
    pin.key_id = signed_feed.key_id;
    pin.public_key = signed_feed.key_text.view();
    feed::CheckRequest request;
    request.feed = signed_feed.bytes;
    request.signature = signed_feed.signature;
    request.channel = channel;
    request.keys = std::span<const registry::PinnedKey>(&pin, 1);
    return feed::check_feed(request);
}

/// The smallest feed the reader accepts.
std::string minimal() {
    std::string json =
        R"({"feed":1,"channel":"stable","releases":[{"release":807,"version":"0.8.7",)"
        R"("date":"2026-10-24","from":"0.7.0","packages":{"windows-x64":{"file":)"
        R"("open-annihilation-0.8.7-windows-x64.zip","size":1,"sha256":")";
    json += kSha;
    json += R"(","mirrors":["http://downloads.example.net/v1/engine/0.8.7/"]}}}]})";
    return json;
}

/// `minimal()` with the first `from` replaced by `to`.
std::string replaced(std::string_view from, std::string_view to) {
    std::string json = minimal();
    const std::size_t at = json.find(from);
    OA_CHECK(at != std::string::npos);
    if (at == std::string::npos)
        return json;
    json.replace(at, from.size(), to);
    return json;
}

void expect_refused(std::string_view json, feed::Verdict verdict, std::string_view detail) {
    const Signed signed_feed = sign_text(json, "test-pin", 0x21);
    const feed::Checked checked = check_signed(signed_feed);
    OA_CHECK(checked.verdict == verdict);
    if (checked.detail.find(detail) == std::string::npos) {
        std::fprintf(
            stderr,
            "expected detail containing '%.*s', got '%s'\n",
            static_cast<int>(detail.size()),
            detail.data(),
            checked.detail.c_str()
        );
    }
    OA_CHECK(checked.detail.find(detail) != std::string::npos);
    OA_CHECK(!checked.usable());
    OA_CHECK(checked.feed == nullptr);
    if (verdict == feed::Verdict::malformed || verdict == feed::Verdict::wrong_format) {
        OA_CHECK(checked.signed_by == "test-pin");
        OA_CHECK(checked.digest == sha256::digest_of(signed_feed.bytes));
    }
}

std::string package_json(std::string_view file, std::string_view size, std::string_view hash) {
    std::string json = R"({"file":")";
    json += file;
    json += R"(","size":)";
    json += size;
    json += R"(,"sha256":")";
    json += hash;
    json += R"(","mirrors":["http://downloads.example.net/v1/engine/0.8.7/"]})";
    return json;
}

std::string windows_package(std::string_view file = "a.zip") {
    return std::string("{\"windows-x64\":") + package_json(file, "1", kSha) + "}";
}

std::string release_json(std::string_view number, std::string_view packages) {
    std::string json = R"({"release":)";
    json += number;
    json += R"(,"version":"0.8.7","date":"2026-10-24","from":"0.7.0","packages":)";
    json += packages;
    json += "}";
    return json;
}

std::string feed_around(std::string_view releases) {
    std::string json = R"({"feed":1,"channel":"stable","releases":)";
    json += releases;
    json += "}";
    return json;
}

/// A feed that carries every field, plus members this build does not know.
std::string good_json() {
    std::string json = R"({
  "feed": 1,
  "remark": "kept",
  "channel": "stable",
  "online_min": "0.8.3",
  "releases": [{
    "release": 807,
    "version": "0.8.7",
    "date": "2026-10-24",
    "from": "0.7.0",
    "rollout": 25,
    "notes": {"en": ["Drag the bars", "A second line"], "de": ["Ziehen"]},
    "page": "https://example.net/v0.8.7",
    "packages": {
      "windows-x64": {
        "extra": true,
        "file": "open-annihilation-0.8.7-windows-x64.zip",
        "size": 1200,
        "sha256": ")";
    json += kSha;
    json += R"(",
        "installer": {
          "file": "open-annihilation-0.8.7-windows-x64-setup.exe",
          "size": 3400,
          "sha256": ")";
    json += kInstallerSha;
    json += R"("
        },
        "mirrors": [
          "http://downloads.example.net/v1/engine/0.8.7/",
          "https://example.net/releases/v0.8.7/"
        ]
      }
    }
  }]
})";
    return json;
}

void accepted_feed() {
    const Signed signed_feed = sign_text(good_json(), "test-pin", 0x21);
    const feed::Checked checked = check_signed(signed_feed);
    OA_CHECK(checked.verdict == feed::Verdict::accepted);
    OA_CHECK(checked.usable());
    OA_CHECK(checked.signed_by == "test-pin");
    OA_CHECK(checked.digest == sha256::digest_of(signed_feed.bytes));
    OA_CHECK(feed::verdict_text(checked.verdict) == std::string_view("accepted"));
    OA_CHECK(checked.feed != nullptr);
    if (checked.feed == nullptr)
        return;
    const feed::Feed& loaded = *checked.feed;
    OA_CHECK(loaded.channel == "stable");
    OA_CHECK(loaded.online_min.has_value());
    if (loaded.online_min)
        OA_CHECK((*loaded.online_min == oamod::EngineVersion{0, 8, 3}));
    OA_CHECK(loaded.releases.size() == 1);
    if (loaded.releases.size() != 1)
        return;
    const feed::Release& release = loaded.releases[0];
    OA_CHECK(release.release == 807);
    OA_CHECK(release.version == (oamod::EngineVersion{0, 8, 7}));
    OA_CHECK(release.version_text == "0.8.7");
    OA_CHECK(release.date == "2026-10-24");
    OA_CHECK(release.from == (oamod::EngineVersion{0, 7, 0}));
    OA_CHECK(release.rollout == 25);
    OA_CHECK(release.page == "https://example.net/v0.8.7");
    OA_CHECK(release.notes.size() == 2);
    if (release.notes.size() == 2) {
        OA_CHECK(release.notes[0].language == "en");
        OA_CHECK(release.notes[0].lines.size() == 2);
        OA_CHECK(release.notes[0].lines[0] == "Drag the bars");
        OA_CHECK(release.notes[1].language == "de");
        OA_CHECK(release.notes[1].lines.size() == 1);
        OA_CHECK(release.notes[1].lines[0] == "Ziehen");
    }
    OA_CHECK(release.packages.size() == 1);
    if (release.packages.size() != 1)
        return;
    const feed::Package& package = release.packages[0];
    OA_CHECK(package.id == "windows-x64");
    OA_CHECK(package.zip.file == "open-annihilation-0.8.7-windows-x64.zip");
    OA_CHECK(package.zip.size == 1200);
    const auto zip_hash = sha256::parse_hex(kSha);
    OA_CHECK(zip_hash.has_value());
    if (zip_hash)
        OA_CHECK(package.zip.sha256 == *zip_hash);
    OA_CHECK(package.installer.has_value());
    if (package.installer) {
        OA_CHECK(package.installer->file == "open-annihilation-0.8.7-windows-x64-setup.exe");
        OA_CHECK(package.installer->size == 3400);
        const auto installer_hash = sha256::parse_hex(kInstallerSha);
        OA_CHECK(installer_hash.has_value());
        if (installer_hash)
            OA_CHECK(package.installer->sha256 == *installer_hash);
    }
    OA_CHECK(package.mirrors.size() == 2);
    if (package.mirrors.size() == 2) {
        OA_CHECK(
            url::url_text(package.mirrors[0]) == "http://downloads.example.net/v1/engine/0.8.7/"
        );
        OA_CHECK(url::url_text(package.mirrors[1]) == "https://example.net/releases/v0.8.7/");
    }
}

void signature_cases() {
    const Signed signed_feed = sign_text(minimal(), "test-pin", 0x21);
    std::vector<uint8_t> flipped_feed = signed_feed.bytes;
    OA_CHECK(flipped_feed.size() > 8);
    if (flipped_feed.size() > 8)
        flipped_feed[8] = static_cast<uint8_t>(flipped_feed[8] ^ 0x01);
    Signed broken = signed_feed;
    broken.bytes = flipped_feed;
    const feed::Checked bad_feed = check_signed(broken);
    OA_CHECK(bad_feed.verdict == feed::Verdict::bad_signature);
    OA_CHECK(bad_feed.signed_by.empty());
    OA_CHECK(bad_feed.digest == sha256::Digest{});

    std::string signature_text(signed_feed.signature.begin(), signed_feed.signature.end());
    const std::size_t space = signature_text.rfind(' ');
    OA_CHECK(space != std::string::npos && space + 1 < signature_text.size());
    if (space != std::string::npos && space + 1 < signature_text.size()) {
        char& digit = signature_text[space + 1];
        digit = digit == 'A' ? 'B' : 'A';
    }
    Signed flipped_sig = signed_feed;
    flipped_sig.signature.assign(signature_text.begin(), signature_text.end());
    const feed::Checked bad_sig = check_signed(flipped_sig);
    OA_CHECK(bad_sig.verdict == feed::Verdict::bad_signature);

    Signed missing = signed_feed;
    missing.signature.clear();
    const feed::Checked no_sig = check_signed(missing);
    OA_CHECK(no_sig.verdict == feed::Verdict::no_signature);

    const std::string two_lines =
        "ed25519 test-pin "
        "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
        "\n"
        "ed25519 test-pin "
        "BBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBBB"
        "\n";
    OA_CHECK(two_lines.size() <= 256);
    Signed two = signed_feed;
    two.signature.assign(two_lines.begin(), two_lines.end());
    const feed::Checked bad_file = check_signed(two);
    OA_CHECK(bad_file.verdict == feed::Verdict::bad_signature_file);
}

/// An added registry's key is not a pin, and the check has nowhere to pass that registry.
void added_registry_key() {
    std::array<uint8_t, 32> seed{};
    seed.fill(0x44);
    signing::SecretKey secret{};
    signing::PublicKey public_key{};
    signing::key_pair_from_seed(seed, secret, public_key);
    seed.fill(0);
    const std::string json = minimal();
    const std::vector<uint8_t> bytes(json.begin(), json.end());
    const signing::Signature signature = signing::sign(secret, bytes);
    secret.fill(0);

    const std::optional<url::Url> catalogue_url =
        url::parse_http_url("http://127.0.0.1/catalogue.json");
    OA_CHECK(catalogue_url.has_value());
    registry::AddedRegistry added;
    added.descriptor.id = "ridge";
    added.descriptor.name = "Ridge";
    if (catalogue_url)
        added.descriptor.catalogue = *catalogue_url;
    added.descriptor.keys.push_back(registry::RegistryKey{"added-key", public_key});
    added.descriptor.mode = registry::DownloadMode::direct;
    added.added = "2026-10-01";
    added.enabled = true;

    const std::string text = catalogue::signature_file_text(
        catalogue::SignatureLine{added.descriptor.keys[0].id, signature}
    );
    Signed signed_feed;
    signed_feed.bytes = bytes;
    signed_feed.signature.assign(text.begin(), text.end());
    signed_feed.key_id = "test-pin";
    std::array<uint8_t, 32> pin_seed{};
    pin_seed.fill(0x21);
    signing::SecretKey pin_secret{};
    signing::PublicKey pin_key{};
    signing::key_pair_from_seed(pin_seed, pin_secret, pin_key);
    pin_seed.fill(0);
    pin_secret.fill(0);
    signed_feed.key_text = signing::public_key_text(pin_key);
    const feed::Checked checked = check_signed(signed_feed);
    OA_CHECK(checked.verdict == feed::Verdict::unknown_key);
    OA_CHECK(checked.feed == nullptr);
    (void)added;
}

void channel_and_size() {
    const Signed preview =
        sign_text(replaced("\"channel\":\"stable\"", "\"channel\":\"preview\""), "test-pin", 0x21);
    const feed::Checked other = check_signed(preview, "stable");
    OA_CHECK(other.verdict == feed::Verdict::wrong_channel);
    OA_CHECK(other.signed_by == "test-pin");
    OA_CHECK(other.feed == nullptr);

    std::vector<uint8_t> huge(feed::max_feed_bytes + 1, static_cast<uint8_t>('{'));
    feed::CheckRequest too_big;
    too_big.feed = huge;
    const feed::Checked large = feed::check_feed(too_big);
    OA_CHECK(large.verdict == feed::Verdict::too_large);
    OA_CHECK(large.digest == sha256::Digest{});

    std::vector<uint8_t> full(feed::max_feed_bytes, static_cast<uint8_t>('{'));
    feed::CheckRequest at_limit;
    at_limit.feed = full;
    const feed::Checked empty_sig = feed::check_feed(at_limit);
    OA_CHECK(empty_sig.verdict == feed::Verdict::no_signature);
}

void structure_refusals() {
    expect_refused("{", feed::Verdict::malformed, "the feed is not JSON");
    expect_refused("[]", feed::Verdict::malformed, "the feed is not a JSON object");
    expect_refused(
        replaced("\"channel\":\"stable\"", "\"channel\":\"stable\",\"channel\":\"stable\""),
        feed::Verdict::malformed,
        "a member is named twice"
    );
    expect_refused("{}", feed::Verdict::wrong_format, "feed is not 1");
    expect_refused(
        replaced("\"feed\":1", "\"feed\":2"), feed::Verdict::wrong_format, "feed is not 1"
    );
    expect_refused(
        replaced("\"channel\":\"stable\",", ""), feed::Verdict::malformed, "channel is missing"
    );
    expect_refused(
        replaced("\"channel\":\"stable\"", "\"channel\":\"Stable\""),
        feed::Verdict::malformed,
        "channel is not lower-case letters and digits"
    );
    expect_refused(
        replaced("\"feed\":1", "\"feed\":1.0"), feed::Verdict::wrong_format, "feed is not 1"
    );
    expect_refused(
        replaced("\"channel\":\"stable\"", "\"channel\":\"stable\",\"online_min\":\"nope\""),
        feed::Verdict::malformed,
        "online_min is not a version"
    );
    expect_refused(
        R"({"feed":1,"channel":"stable"})", feed::Verdict::malformed, "releases is not a list"
    );
    expect_refused(
        R"({"feed":1,"channel":"stable","releases":[]})",
        feed::Verdict::malformed,
        "releases is empty"
    );

    std::string many = R"({"feed":1,"channel":"stable","releases":[)";
    for (int index = 0; index < 33; ++index) {
        if (index != 0)
            many += ',';
        many += release_json(std::to_string(index + 1), windows_package());
    }
    many += "]}";
    expect_refused(many, feed::Verdict::malformed, "releases lists more than 32");

    expect_refused(feed_around("[1]"), feed::Verdict::malformed, "a release is not an object");
    expect_refused(
        feed_around(R"([{"version":"0.8.7","date":"2026-10-24","from":"0.7.0","packages":{}}])"),
        feed::Verdict::malformed,
        "release is missing"
    );
    expect_refused(
        replaced("\"release\":807", "\"release\":0"),
        feed::Verdict::malformed,
        "release is not a whole number from 1 to 2^53"
    );
    expect_refused(
        replaced("\"release\":807", "\"release\":9007199254740993"),
        feed::Verdict::malformed,
        "release is not a whole number from 1 to 2^53"
    );
    expect_refused(
        replaced("\"version\":\"0.8.7\",", ""), feed::Verdict::malformed, "version is missing"
    );
    expect_refused(
        replaced("\"version\":\"0.8.7\"", "\"version\":\"0.8\""),
        feed::Verdict::malformed,
        "version is not a version"
    );
    expect_refused(
        replaced("\"date\":\"2026-10-24\",", ""), feed::Verdict::malformed, "date is missing"
    );
    expect_refused(
        replaced("\"date\":\"2026-10-24\"", "\"date\":\"2026-13-01\""),
        feed::Verdict::malformed,
        "date is not a day"
    );
    expect_refused(
        replaced("\"from\":\"0.7.0\",", ""), feed::Verdict::malformed, "from is missing"
    );
    expect_refused(
        replaced("\"from\":\"0.7.0\"", "\"from\":\"soon\""),
        feed::Verdict::malformed,
        "from is not a version"
    );
    expect_refused(
        replaced(",\"packages\":", ",\"rollout\":101,\"packages\":"),
        feed::Verdict::malformed,
        "rollout is not a whole number from 0 to 100"
    );
    expect_refused(
        feed_around(
            "[" + release_json("1", windows_package()) + "," +
            release_json("1", windows_package()) + "]"
        ),
        feed::Verdict::malformed,
        "two releases share a release number"
    );
    expect_refused(
        replaced(",\"packages\":", ",\"page\":\"not a page\",\"packages\":"),
        feed::Verdict::malformed,
        "page is not an http or https address"
    );
    expect_refused(
        replaced(",\"packages\":", ",\"notes\":{\"de\":[\"Hallo\"]},\"packages\":"),
        feed::Verdict::malformed,
        "notes has no en"
    );
    expect_refused(
        replaced(",\"packages\":", ",\"notes\":{\"EN\":[\"Hi\"]},\"packages\":"),
        feed::Verdict::malformed,
        "a notes language is not a language tag"
    );
    expect_refused(
        replaced(",\"packages\":", ",\"notes\":{\"en\":\"Hi\"},\"packages\":"),
        feed::Verdict::malformed,
        "a notes language is not a list of lines"
    );
    expect_refused(
        replaced(",\"packages\":", ",\"notes\":{\"en\":[1]},\"packages\":"),
        feed::Verdict::malformed,
        "a notes line is not text"
    );
    expect_refused(
        replaced(",\"packages\":", ",\"notes\":\"hi\",\"packages\":"),
        feed::Verdict::malformed,
        "notes is not an object"
    );

    std::string long_line(201, 'a');
    expect_refused(
        replaced(",\"packages\":", ",\"notes\":{\"en\":[\"" + long_line + "\"]},\"packages\":"),
        feed::Verdict::malformed,
        "a notes line is longer than 200 bytes"
    );
    std::string many_lines = ",\"notes\":{\"en\":[";
    for (int index = 0; index < 13; ++index) {
        if (index != 0)
            many_lines += ',';
        many_lines += "\"line\"";
    }
    many_lines += "]},\"packages\":";
    expect_refused(
        replaced(",\"packages\":", many_lines),
        feed::Verdict::malformed,
        "a notes language has more than 12 lines"
    );
    expect_refused(
        replaced(",\"packages\":", ",\"notes\":{\"en\":[\"\\u0001\"]},\"packages\":"),
        feed::Verdict::malformed,
        "a notes line has a control character"
    );
    expect_refused(
        replaced(",\"packages\":", ",\"notes\":{\"en\":[\"\\u0080\"]},\"packages\":"),
        feed::Verdict::malformed,
        "a notes line has a control character"
    );

    std::string raw = minimal();
    const std::size_t stable = raw.find("stable");
    OA_CHECK(stable != std::string::npos);
    if (stable != std::string::npos)
        raw.insert(stable, 1, static_cast<char>(0xFF));
    expect_refused(raw, feed::Verdict::malformed, "the feed is not JSON");

    const std::string dup = std::string("{\"windows-x64\":") + package_json("a.zip", "1", kSha) +
                            ",\"windows-x64\":" + package_json("a.zip", "1", kSha) + "}";
    expect_refused(
        feed_around("[" + release_json("1", dup) + "]"),
        feed::Verdict::malformed,
        "a package is named twice"
    );
    expect_refused(
        replaced("\"windows-x64\":{", "\"windows-x64\":1,\"pad\":{"),
        feed::Verdict::malformed,
        "a package is not an object"
    );
    expect_refused(
        replaced("\"windows-x64\"", "\"Windows-x64\""),
        feed::Verdict::malformed,
        "a package id is not a package id"
    );
    expect_refused(
        replaced("\"file\":\"open-annihilation-0.8.7-windows-x64.zip\",", ""),
        feed::Verdict::malformed,
        "file is missing"
    );
    expect_refused(
        replaced("\"file\":\"open-annihilation-0.8.7-windows-x64.zip\"", "\"file\":\"a/b.zip\""),
        feed::Verdict::malformed,
        "file is not a plain name"
    );
    expect_refused(
        replaced("\"file\":\"open-annihilation-0.8.7-windows-x64.zip\"", "\"file\":\"..\""),
        feed::Verdict::malformed,
        "file is not a plain name"
    );
    expect_refused(
        replaced("\"file\":\"open-annihilation-0.8.7-windows-x64.zip\"", "\"file\":\"a\\\\b.zip\""),
        feed::Verdict::malformed,
        "file is not a plain name"
    );
    expect_refused(
        replaced("\"size\":1", "\"size\":0"),
        feed::Verdict::malformed,
        "size is not a whole number from 1 to 2^40"
    );
    expect_refused(
        replaced("\"size\":1", "\"size\":1099511627777"),
        feed::Verdict::malformed,
        "size is not a whole number from 1 to 2^40"
    );
    expect_refused(
        replaced(kSha, "ABCDEF"), feed::Verdict::malformed, "sha256 is not 64 lower-case hex digits"
    );
    std::string upper(kSha);
    upper[0] = 'A';
    expect_refused(
        replaced(kSha, upper), feed::Verdict::malformed, "sha256 is not 64 lower-case hex digits"
    );
    expect_refused(
        replaced(
            R"("mirrors":["http://downloads.example.net/v1/engine/0.8.7/"])",
            R"("mirrors":"http://downloads.example.net/v1/engine/0.8.7/")"
        ),
        feed::Verdict::malformed,
        "mirrors is not a list"
    );
    expect_refused(
        replaced(
            R"("mirrors":["http://downloads.example.net/v1/engine/0.8.7/"])", R"("mirrors":[])"
        ),
        feed::Verdict::malformed,
        "mirrors is empty"
    );
    expect_refused(
        replaced("http://downloads.example.net/v1/engine/0.8.7/", "http://example.net/v1"),
        feed::Verdict::malformed,
        "a mirror is not an http or https address ending in / with no query"
    );
    expect_refused(
        replaced("http://downloads.example.net/v1/engine/0.8.7/", "http://example.net/v1/?q=1"),
        feed::Verdict::malformed,
        "a mirror is not an http or https address ending in / with no query"
    );
    expect_refused(
        replaced(",\"mirrors\":", ",\"installer\":\"setup.exe\",\"mirrors\":"),
        feed::Verdict::malformed,
        "installer is not a file, size and sha256"
    );
    expect_refused(
        R"({"feed":1,"channel":"stable","releases":[{"release":807,"version":"0.8.7","date":"2026-10-24","from":"0.7.0"}]})",
        feed::Verdict::malformed,
        "packages is missing"
    );
    expect_refused(
        R"({"feed":1,"channel":"stable","releases":[{"release":807,"version":"0.8.7","date":"2026-10-24","from":"0.7.0","packages":[]}]})",
        feed::Verdict::malformed,
        "packages is not an object"
    );
}

void boundaries() {
    const Signed one = sign_text(replaced("\"release\":807", "\"release\":1"), "test-pin", 0x21);
    const feed::Checked numbered_one = check_signed(one);
    OA_CHECK(numbered_one.verdict == feed::Verdict::accepted);
    if (numbered_one.feed && !numbered_one.feed->releases.empty())
        OA_CHECK(numbered_one.feed->releases[0].rollout == 100);
    const Signed widest =
        sign_text(replaced("\"release\":807", "\"release\":9007199254740992"), "test-pin", 0x21);
    OA_CHECK(check_signed(widest).verdict == feed::Verdict::accepted);
    const Signed biggest =
        sign_text(replaced("\"size\":1", "\"size\":1099511627776"), "test-pin", 0x21);
    OA_CHECK(check_signed(biggest).verdict == feed::Verdict::accepted);

    std::string line(200, 'b');
    std::string notes = ",\"notes\":{\"en\":[";
    for (int index = 0; index < 12; ++index) {
        if (index != 0)
            notes += ',';
        notes += '"';
        notes += line;
        notes += '"';
    }
    notes += "]},\"packages\":";
    const Signed capped = sign_text(replaced(",\"packages\":", notes), "test-pin", 0x21);
    OA_CHECK(check_signed(capped).verdict == feed::Verdict::accepted);

    std::string thirty_two = R"({"feed":1,"channel":"stable","releases":[)";
    for (int index = 0; index < 32; ++index) {
        if (index != 0)
            thirty_two += ',';
        thirty_two += release_json(std::to_string(index + 1), windows_package());
    }
    thirty_two += "]}";
    const Signed full = sign_text(thirty_two, "test-pin", 0x21);
    const feed::Checked checked = check_signed(full);
    OA_CHECK(checked.verdict == feed::Verdict::accepted);
    OA_CHECK(checked.feed != nullptr);
    if (checked.feed) {
        OA_CHECK(checked.feed->releases.size() == 32);
        OA_CHECK(checked.feed->releases.front().release == 32);
        OA_CHECK(checked.feed->releases.back().release == 1);
    }

    const Signed unsorted = sign_text(
        feed_around(
            "[" + release_json("1", windows_package()) + "," +
            release_json("5", windows_package("b.zip")) + "]"
        ),
        "test-pin",
        0x21
    );
    const feed::Checked ordered = check_signed(unsorted);
    OA_CHECK(ordered.feed != nullptr);
    if (ordered.feed && ordered.feed->releases.size() == 2) {
        OA_CHECK(ordered.feed->releases[0].release == 5);
        OA_CHECK(ordered.feed->releases[1].release == 1);
    }

    const Signed root_mirror = sign_text(
        replaced("http://downloads.example.net/v1/engine/0.8.7/", "http://example.net"),
        "test-pin",
        0x21
    );
    const feed::Checked rooted = check_signed(root_mirror);
    OA_CHECK(rooted.verdict == feed::Verdict::accepted);
    if (rooted.feed && !rooted.feed->releases.empty() && !rooted.feed->releases[0].packages.empty())
        OA_CHECK(rooted.feed->releases[0].packages[0].mirrors[0].target == "/");

    const Signed queried = sign_text(
        replaced(",\"packages\":", ",\"page\":\"https://example.net/p?q=1\",\"packages\":"),
        "test-pin",
        0x21
    );
    const feed::Checked page = check_signed(queried);
    OA_CHECK(page.verdict == feed::Verdict::accepted);
    if (page.feed && !page.feed->releases.empty())
        OA_CHECK(page.feed->releases[0].page == "https://example.net/p?q=1");

    std::string name(128, 'c');
    const Signed long_name =
        sign_text(replaced("open-annihilation-0.8.7-windows-x64.zip", name), "test-pin", 0x21);
    OA_CHECK(check_signed(long_name).verdict == feed::Verdict::accepted);
    std::string longer(129, 'c');
    expect_refused(
        replaced("open-annihilation-0.8.7-windows-x64.zip", longer),
        feed::Verdict::malformed,
        "file is not a plain name"
    );
}

feed::Package make_package(std::string id) {
    feed::Package package;
    package.id = std::move(id);
    package.zip.file = "a.zip";
    package.zip.size = 1;
    return package;
}

feed::Release make_release(
    int64_t number,
    const char* version,
    const char* from,
    std::string package,
    int32_t rollout = 100
) {
    feed::Release release;
    release.release = number;
    release.version = *oamod::parse_engine_version(version);
    release.version_text = version;
    release.date = "2026-10-24";
    release.from = *oamod::parse_engine_version(from);
    release.rollout = rollout;
    release.packages.push_back(make_package(std::move(package)));
    return release;
}

feed::Build make_build(const char* version, int64_t number, std::string_view package) {
    feed::Build build;
    build.version = *oamod::parse_engine_version(version);
    build.release = number;
    build.package = package;
    return build;
}

void choose_cases() {
    feed::Feed none_for_build;
    none_for_build.channel = "stable";
    none_for_build.releases.push_back(make_release(900, "0.8.5", "0.8.0", "windows-x64"));
    const feed::Offer unknown =
        feed::choose(none_for_build, make_build("0.8.4", 800, "linux-x86_64"));
    OA_CHECK(unknown.way == feed::Way::none);
    OA_CHECK(unknown.release == nullptr);
    OA_CHECK(unknown.package == nullptr);

    feed::Feed old_build;
    old_build.channel = "stable";
    old_build.releases.push_back(make_release(900, "0.8.5", "0.9.0", "windows-x64"));
    const feed::Offer page = feed::choose(old_build, make_build("0.8.7", 807, "windows-x64"));
    OA_CHECK(page.way == feed::Way::page_only);
    OA_CHECK(page.release != nullptr);
    if (page.release != nullptr)
        OA_CHECK(page.release->release == 900);

    feed::Feed current;
    current.channel = "stable";
    current.releases.push_back(make_release(900, "0.8.5", "0.8.0", "windows-x64"));
    const feed::Offer in_place = feed::choose(current, make_build("0.8.0", 800, "windows-x64"));
    OA_CHECK(in_place.way == feed::Way::in_place);
    OA_CHECK(in_place.package != nullptr);
    if (in_place.package != nullptr)
        OA_CHECK(in_place.package->id == "windows-x64");

    feed::Feed numbered;
    numbered.channel = "stable";
    numbered.releases.push_back(make_release(807, "0.8.7", "0.7.0", "windows-x64"));
    numbered.releases.push_back(make_release(900, "0.8.5", "0.8.0", "windows-x64"));
    const feed::Offer newer = feed::choose(numbered, make_build("0.8.4", 800, "windows-x64"));
    OA_CHECK(newer.release != nullptr);
    if (newer.release != nullptr) {
        OA_CHECK(newer.release->release == 900);
        OA_CHECK(newer.release->version_text == "0.8.5");
    }
    OA_CHECK(newer.way == feed::Way::in_place);

    feed::Feed same;
    same.channel = "stable";
    same.releases.push_back(make_release(807, "0.8.7", "0.7.0", "windows-x64"));
    const feed::Offer equal = feed::choose(same, make_build("0.8.7", 807, "windows-x64"));
    OA_CHECK(equal.way == feed::Way::none);

    feed::Feed other_package;
    other_package.channel = "stable";
    other_package.releases.push_back(make_release(900, "0.8.5", "0.7.0", "windows-x86"));
    const feed::Offer windows_95 =
        feed::choose(other_package, make_build("0.8.4", 800, "windows-95"));
    OA_CHECK(windows_95.way == feed::Way::none);

    feed::Feed staged;
    staged.channel = "stable";
    staged.releases.push_back(make_release(901, "0.8.6", "0.8.0", "steam-deck"));
    staged.releases.push_back(make_release(900, "0.8.5", "0.8.0", "windows-x64", 0));
    const feed::Offer rolled = feed::choose(staged, make_build("0.8.4", 800, "windows-x64"));
    OA_CHECK(rolled.way == feed::Way::in_place);
    OA_CHECK(rolled.release != nullptr);
    if (rolled.release != nullptr) {
        OA_CHECK(rolled.release->release == 900);
        OA_CHECK(rolled.release->rollout == 0);
    }

    feed::Feed quiet = staged;
    quiet.online_min = oamod::EngineVersion{0, 9, 0};
    const feed::Offer with_floor = feed::choose(quiet, make_build("0.8.4", 800, "windows-x64"));
    OA_CHECK(with_floor.way == rolled.way);
    OA_CHECK(
        with_floor.release == rolled.release ||
        (with_floor.release != nullptr && rolled.release != nullptr &&
         with_floor.release->release == rolled.release->release)
    );
}

void number_and_notes() {
    const auto version_087 = oamod::parse_engine_version("0.8.7");
    const auto version_123 = oamod::parse_engine_version("1.2.3");
    const auto version_100 = oamod::parse_engine_version("0.100.0");
    OA_CHECK(version_087.has_value() && version_123.has_value() && version_100.has_value());
    if (version_087 && version_123 && version_100) {
        const auto number_087 = feed::release_number(*version_087);
        const auto number_123 = feed::release_number(*version_123);
        OA_CHECK(number_087.has_value() && *number_087 == 807);
        OA_CHECK(number_123.has_value() && *number_123 == 10203);
        OA_CHECK(!feed::release_number(*version_100));
    }

    feed::Release release;
    feed::Notes german;
    german.language = "de";
    german.lines.push_back("Hallo");
    feed::Notes english;
    english.language = "en";
    english.lines.push_back("Hello");
    release.notes.push_back(german);
    release.notes.push_back(english);
    const feed::Notes* austrian = feed::notes_for(release, "de-AT");
    OA_CHECK(austrian != nullptr && austrian->language == "de");
    const feed::Notes* french = feed::notes_for(release, "fr");
    OA_CHECK(french != nullptr && french->language == "en");
    const feed::Notes* upper = feed::notes_for(release, "DE");
    OA_CHECK(upper != nullptr && upper->language == "de");

    feed::Release german_only;
    german_only.notes.push_back(german);
    OA_CHECK(feed::notes_for(german_only, "fr") == nullptr);
    const feed::Notes* still_german = feed::notes_for(german_only, "de-AT");
    OA_CHECK(still_german != nullptr && still_german->language == "de");
}

bool same_file(const feed::PackageFile& left, const feed::PackageFile& right) {
    return left.file == right.file && left.size == right.size && left.sha256 == right.sha256;
}

bool same_feed(const feed::Feed& left, const feed::Feed& right) {
    if (left.channel != right.channel || left.online_min != right.online_min ||
        left.releases.size() != right.releases.size())
        return false;
    for (std::size_t index = 0; index < left.releases.size(); ++index) {
        const feed::Release& one = left.releases[index];
        const feed::Release& two = right.releases[index];
        if (one.release != two.release || one.version != two.version ||
            one.version_text != two.version_text || one.date != two.date || one.from != two.from ||
            one.rollout != two.rollout || one.page != two.page ||
            one.notes.size() != two.notes.size() || one.packages.size() != two.packages.size())
            return false;
        for (std::size_t note = 0; note < one.notes.size(); ++note) {
            if (one.notes[note].language != two.notes[note].language ||
                one.notes[note].lines != two.notes[note].lines)
                return false;
        }
        for (std::size_t package = 0; package < one.packages.size(); ++package) {
            const feed::Package& left_package = one.packages[package];
            const feed::Package& right_package = two.packages[package];
            if (left_package.id != right_package.id ||
                !same_file(left_package.zip, right_package.zip) ||
                left_package.installer.has_value() != right_package.installer.has_value() ||
                left_package.mirrors != right_package.mirrors)
                return false;
            if (left_package.installer &&
                !same_file(*left_package.installer, *right_package.installer))
                return false;
        }
    }
    return true;
}

void round_trip() {
    feed::Feed source;
    source.channel = "stable";
    source.online_min = oamod::EngineVersion{0, 8, 3};
    feed::Release newer = make_release(900, "0.8.5", "0.8.0", "windows-x64", 0);
    newer.page = "https://example.net/v0.8.5";
    feed::Notes english;
    english.language = "en";
    english.lines.push_back("say \"hi\"");
    english.lines.push_back("a\\b");
    feed::Notes german;
    german.language = "de";
    german.lines.push_back("Zeile");
    newer.notes.push_back(std::move(english));
    newer.notes.push_back(std::move(german));
    feed::PackageFile installer;
    installer.file = "open-annihilation-0.8.5-windows-x64-setup.exe";
    installer.size = 99;
    installer.sha256 = *sha256::parse_hex(kInstallerSha);
    newer.packages[0].zip.sha256 = *sha256::parse_hex(kSha);
    const auto mirror = url::parse_http_url("http://downloads.example.net/v1/engine/0.8.5/");
    OA_CHECK(mirror.has_value());
    if (mirror)
        newer.packages[0].mirrors.push_back(*mirror);
    newer.packages[0].installer = installer;
    feed::Release older = make_release(807, "0.8.7", "0.7.0", "macos-universal", 100);
    older.packages[0].zip.sha256 = *sha256::parse_hex(kSha);
    const auto other = url::parse_http_url("https://example.net/releases/v0.8.7/");
    OA_CHECK(other.has_value());
    if (other)
        older.packages[0].mirrors.push_back(*other);
    source.releases.push_back(std::move(newer));
    source.releases.push_back(std::move(older));

    const std::string text = feed::feed_text(source);
    OA_CHECK(!text.empty() && text.back() == '\n');
    OA_CHECK(text.find("\n  \"feed\": 1") != std::string::npos);
    OA_CHECK(text.find("\n      \"release\": ") != std::string::npos);
    std::string error;
    const std::vector<uint8_t> bytes(text.begin(), text.end());
    const std::optional<feed::Feed> loaded = feed::read_feed(bytes, &error);
    if (!loaded || !same_feed(source, *loaded)) {
        std::fprintf(
            stderr,
            "round trip failed: %s\n%.*s",
            error.c_str(),
            static_cast<int>(text.size()),
            text.data()
        );
    }
    OA_CHECK(loaded.has_value());
    if (loaded)
        OA_CHECK(same_feed(source, *loaded));
}

void verdict_names() {
    const feed::Verdict values[] = {
        feed::Verdict::accepted,
        feed::Verdict::too_large,
        feed::Verdict::no_signature,
        feed::Verdict::bad_signature_file,
        feed::Verdict::unknown_key,
        feed::Verdict::bad_signature,
        feed::Verdict::malformed,
        feed::Verdict::wrong_format,
        feed::Verdict::wrong_channel,
    };
    const char* names[] = {
        "accepted",
        "too_large",
        "no_signature",
        "bad_signature_file",
        "unknown_key",
        "bad_signature",
        "malformed",
        "wrong_format",
        "wrong_channel",
    };
    for (std::size_t index = 0; index < 9; ++index)
        OA_CHECK(std::string_view(feed::verdict_text(values[index])) == names[index]);
}

} // namespace

int main() {
    accepted_feed();
    signature_cases();
    added_registry_key();
    channel_and_size();
    structure_refusals();
    boundaries();
    choose_cases();
    number_and_notes();
    round_trip();
    verdict_names();
    return oa::test::check_exit_status();
}
