// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A catalogue's entries: the sample read field by field, every skipped
// entry, and the catalogues the reader refuses whole.

#include "oa/base/sha256.hpp"
#include "oa/base/signing/ed25519.hpp"
#include "oa/data/catalogue/catalogue.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/formats/url.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace catalogue = oa::data::catalogue;
namespace oamod = oa::formats::oamod;
namespace sha = oa::base::sha256;
namespace signing = oa::base::signing;
namespace url = oa::formats::url;

std::vector<uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

std::string hex_of(std::string_view body) {
    const auto digest = sha::digest_of(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(body.data()), body.size())
    );
    const auto hex = sha::to_hex(digest);
    return {hex.data(), hex.size()};
}

std::string json_escape(std::string_view text) {
    std::string out;
    for (const unsigned char byte : text) {
        if (byte == '\\' || byte == '"') {
            out += '\\';
            out += static_cast<char>(byte);
        } else if (byte < 0x20 || byte == 0x7F) {
            constexpr char digits[] = "0123456789abcdef";
            out += "\\u00";
            out += digits[byte >> 4];
            out += digits[byte & 0x0f];
        } else {
            out += static_cast<char>(byte);
        }
    }
    return out;
}

std::string catalogue_of(std::string_view packages, std::string_view extra = {}) {
    std::string json = "{\"catalogue\":1,\"registry\":\"coreprime\",\"sequence\":7,"
                       "\"generated\":\"2026-11-02T04:10:00Z\","
                       "\"expires\":\"2026-12-02T04:10:00Z\"";
    if (!extra.empty()) {
        json += ',';
        json += extra;
    }
    json += ",\"packages\":[";
    json += packages;
    json += "]}";
    return json;
}

/// A package that passes every rule, plus any extra members.
std::string bare(
    std::string_view kind, std::string_view id, std::string_view extra = {}, bool with_maps = true
) {
    const std::string hash = hex_of(std::string(kind) + "/" + std::string(id));
    std::string json = "{\"kind\":\"";
    json += kind;
    json += "\",\"id\":\"";
    json += id;
    json += "\",\"name\":\"Name\",\"release\":1,\"size\":4,\"sha256\":\"";
    json += hash;
    json += "\",\"file\":\"/v1/p/";
    json += hash;
    json += '.';
    json += kind;
    json += '"';
    if (kind == "oamod")
        json += ",\"version\":\"1\",\"revision\":1";
    if (kind == "oamut")
        json += ",\"version\":\"1\"";
    if (kind == "oamap" && with_maps) {
        json += ",\"maps\":[{\"map\":\"stem@";
        json += id;
        json += "\",\"name\":\"Stem\",\"players\":2,\"size\":\"16x16\"}]";
    }
    if (!extra.empty()) {
        json += ',';
        json += extra;
    }
    json += '}';
    return json;
}

std::string entry_with(
    std::string_view kind, std::string_view id, std::string_view hash, std::string_view file
) {
    std::string json = "{\"kind\":\"";
    json += kind;
    json += "\",\"id\":\"";
    json += id;
    json += "\",\"name\":\"Name\",\"version\":\"1\",\"revision\":1,\"release\":1,\"size\":4,"
            "\"sha256\":\"";
    json += hash;
    json += "\",\"file\":\"";
    json += file;
    json += "\"}";
    return json;
}

catalogue::Catalogue read_ok(std::string_view json) {
    catalogue::Catalogue out;
    std::string error = "set";
    const bool ok = catalogue::read_catalogue(bytes_of(json), out, &error);
    OA_CHECK(ok);
    OA_CHECK(error == "set");
    return out;
}

void refuse(std::string_view json, std::string_view needle) {
    catalogue::Catalogue out;
    out.registry = "untouched";
    std::string error = "none";
    OA_CHECK(!catalogue::read_catalogue(bytes_of(json), out, &error));
    OA_CHECK(out.registry == "untouched");
    OA_CHECK(error.find(needle) != std::string::npos);
}

void skips_one(std::string_view bad, std::string_view needle) {
    const auto out = read_ok(catalogue_of(std::string(bad) + "," + bare("oamod", "keeper")));
    OA_CHECK(out.packages.size() == 1);
    if (!out.packages.empty())
        OA_CHECK(out.packages[0].id == "keeper");
    OA_CHECK(out.problems.size() == 1);
    if (!out.problems.empty()) {
        OA_CHECK(out.problems[0].find("packages[0]") != std::string::npos);
        OA_CHECK(out.problems[0].find(needle) != std::string::npos);
    }
}

std::string replace_token(std::string text, std::string_view token, std::string_view value) {
    for (;;) {
        const auto at = text.find(token);
        if (at == std::string::npos)
            return text;
        text.replace(at, token.size(), value);
    }
}

signing::PublicKey key_from_seed(uint8_t fill) {
    std::array<uint8_t, 32> seed{};
    seed.fill(fill);
    signing::SecretKey secret{};
    signing::PublicKey key{};
    signing::key_pair_from_seed(seed, secret, key);
    secret.fill(0);
    return key;
}

void test_sample() {
    // Made-up package id: the public tree does not name a shipped mod.
    const signing::PublicKey key = key_from_seed(0x11);
    const std::string ridge = hex_of("ridge-bytes");
    const std::string map = hex_of("archipelago-bytes");
    const std::string lang = hex_of("zh-hans-bytes");
    const std::string future = hex_of("future-bytes");
    const std::string sim = hex_of("sim-rules");
    std::string json = R"JSON(
{
  "catalogue": 1,
  "registry": "coreprime",
  "sequence": 418,
  "generated": "2026-11-02T04:10:00Z",
  "expires": "2026-12-02T04:10:00Z",
  "downloads": "http://downloads.coreprime.net/api/v1/downloads",
  "later": true,
  "keys": [ { "id": "release-2026", "public": "@PUBLIC@", "note": "ignored" } ],
  "mirrors": [ "http://mirror.example.net/v1/catalogue.json" ],
  "packages": [
    {
      "kind": "oamod", "id": "ridge", "name": "Ridge",
      "version": "4.8", "revision": 4, "release": 28,
      "publisher": "OA team", "summary": "A balance mod", "author": "OA team",
      "notes": "What this release changes", "tags": ["balance"],
      "homepage": "https://example.com/ridge",
      "requires": { "engine": ">= 0.8.0, < 0.9.0", "base": "ta-3.1c" },
      "sim_hash": "@SIM@",
      "hacks": { "sim": 8, "view": 4, "ids": ["build-speed", "unit-limit"] },
      "size": 43011223, "sha256": "@RIDGE@", "file": "/v1/p/@RIDGE@.oamod",
      "badge": "/v1/i/badge.png"
    },
    {
      "kind": "oamap", "id": "archipelago", "name": "Archipelago", "release": 3,
      "size": 1000, "sha256": "@MAP@", "file": "/v1/p/@MAP@.oamap",
      "maps": [ {
        "map": "isle_of_ashes@archipelago", "name": "Isle of Ashes",
        "players": 4, "size": "16x16", "preview": "/v1/i/preview.png",
        "compatible": { "ta-3.1c": true, "ridge@28": true, "esc@12": false }
      } ]
    },
    {
      "kind": "oalang", "id": "zh-Hans", "name": "简体中文", "release": 2,
      "size": 2000, "sha256": "@LANG@", "file": "/v1/p/@LANG@.oalang",
      "english_name": "Chinese (Simplified)", "word": "Chinese",
      "locales": ["zh-Hans", "zh-CN", "zh-SG", "zh-MY", "zh"],
      "fallbacks": ["en"], "needs": "modern-fonts", "coverage": 1.0
    },
    {
      "kind": "oamod", "id": "future-mod", "name": "Future",
      "version": "1", "revision": 1, "release": 1, "size": 10,
      "sha256": "@FUTURE@", "file": "/v1/p/@FUTURE@.oamod",
      "requires": { "engine": ">= 9.0.0" }
    }
  ]
}
)JSON";
    json = replace_token(json, "@PUBLIC@", signing::public_key_text(key).view());
    json = replace_token(json, "@SIM@", sim);
    json = replace_token(json, "@RIDGE@", ridge);
    json = replace_token(json, "@MAP@", map);
    json = replace_token(json, "@LANG@", lang);
    json = replace_token(json, "@FUTURE@", future);

    const auto out = read_ok(json);
    OA_CHECK(out.problems.empty());
    OA_CHECK(out.registry == "coreprime");
    OA_CHECK(out.sequence == 418);
    OA_CHECK(out.generated_text == "2026-11-02T04:10:00Z");
    OA_CHECK(out.expires_text == "2026-12-02T04:10:00Z");
    OA_CHECK(out.generated == catalogue::parse_utc_time(out.generated_text));
    OA_CHECK(out.expires == catalogue::parse_utc_time(out.expires_text));
    OA_CHECK(out.expires > out.generated);
    const auto downloads = url::parse_http_url("http://downloads.coreprime.net/api/v1/downloads");
    OA_CHECK(downloads.has_value());
    if (downloads)
        OA_CHECK(out.downloads == url::url_text(*downloads));
    OA_CHECK(out.keys.size() == 1);
    if (!out.keys.empty()) {
        OA_CHECK(out.keys[0].id == "release-2026");
        OA_CHECK(out.keys[0].key == key);
    }
    const auto mirror = url::parse_http_url("http://mirror.example.net/v1/catalogue.json");
    OA_CHECK(mirror.has_value() && out.mirrors.size() == 1 && out.mirrors[0] == *mirror);
    OA_CHECK(out.icon.empty());
    OA_CHECK(out.packages.size() == 4);
    if (out.packages.size() != 4)
        return;

    const catalogue::Package& mod = out.packages[0];
    OA_CHECK(mod.kind == catalogue::Kind::oamod);
    OA_CHECK(std::string(catalogue::kind_name(mod.kind)) == "oamod");
    OA_CHECK(mod.id == "ridge");
    OA_CHECK(mod.name == "Ridge");
    OA_CHECK(mod.version == "4.8");
    OA_CHECK(mod.revision == 4);
    OA_CHECK(mod.release == 28);
    OA_CHECK(mod.publisher == "OA team");
    OA_CHECK(mod.author == "OA team");
    OA_CHECK(mod.summary == "A balance mod");
    OA_CHECK(mod.notes == "What this release changes");
    OA_CHECK(mod.homepage == "https://example.com/ridge");
    OA_CHECK(oamod::homepage_valid(mod.homepage));
    OA_CHECK(mod.tags.size() == 1 && mod.tags[0] == "balance");
    OA_CHECK(mod.badge == "/v1/i/badge.png");
    OA_CHECK(mod.requires_engine_text == ">= 0.8.0, < 0.9.0");
    OA_CHECK(mod.requires_engine.has_value());
    if (mod.requires_engine) {
        OA_CHECK(oamod::engine_range_met(*mod.requires_engine, oamod::EngineVersion{0, 8, 0}));
        OA_CHECK(!oamod::engine_range_met(*mod.requires_engine, oamod::EngineVersion{0, 9, 0}));
        OA_CHECK(!oamod::engine_range_met(*mod.requires_engine, oamod::EngineVersion{0, 7, 9}));
    }
    OA_CHECK(mod.requires_base == "ta-3.1c");
    OA_CHECK(
        mod.sim_hash ==
        sha::digest_of(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("sim-rules"), 9))
    );
    OA_CHECK(mod.hacks_sim == 8);
    OA_CHECK(mod.hacks_view == 4);
    OA_CHECK(mod.hack_ids.size() == 2);
    if (mod.hack_ids.size() == 2) {
        OA_CHECK(mod.hack_ids[0] == "build-speed");
        OA_CHECK(mod.hack_ids[1] == "unit-limit");
    }
    OA_CHECK(mod.size == 43011223);
    OA_CHECK(
        mod.sha256 ==
        sha::digest_of(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("ridge-bytes"), 11)
        )
    );
    OA_CHECK(mod.file == "/v1/p/" + ridge + ".oamod");

    const catalogue::Package& maps = out.packages[1];
    OA_CHECK(maps.kind == catalogue::Kind::oamap);
    OA_CHECK(maps.id == "archipelago");
    OA_CHECK(maps.name == "Archipelago");
    OA_CHECK(maps.version.empty());
    OA_CHECK(maps.revision == 0);
    OA_CHECK(maps.release == 3);
    OA_CHECK(maps.maps.size() == 1);
    if (!maps.maps.empty()) {
        const catalogue::MapEntry& entry = maps.maps[0];
        OA_CHECK(entry.map == "isle_of_ashes@archipelago");
        OA_CHECK(entry.stem == "isle_of_ashes");
        OA_CHECK(entry.name == "Isle of Ashes");
        OA_CHECK(entry.players == 4);
        OA_CHECK(entry.width == 16);
        OA_CHECK(entry.height == 16);
        OA_CHECK(entry.preview == "/v1/i/preview.png");
        OA_CHECK(entry.compatible.size() == 3);
        if (entry.compatible.size() == 3) {
            OA_CHECK(entry.compatible[0].first == "ta-3.1c" && entry.compatible[0].second);
            OA_CHECK(entry.compatible[1].first == "ridge@28" && entry.compatible[1].second);
            OA_CHECK(entry.compatible[2].first == "esc@12" && !entry.compatible[2].second);
        }
    }

    const catalogue::Package& language = out.packages[2];
    OA_CHECK(language.kind == catalogue::Kind::oalang);
    OA_CHECK(language.id == "zh-Hans");
    OA_CHECK(language.name == "简体中文");
    OA_CHECK(language.english_name == "Chinese (Simplified)");
    OA_CHECK(language.word == "Chinese");
    OA_CHECK(language.locales.size() == 5);
    if (language.locales.size() == 5) {
        OA_CHECK(language.locales[0] == "zh-Hans");
        OA_CHECK(language.locales[1] == "zh-CN");
        OA_CHECK(language.locales[2] == "zh-SG");
        OA_CHECK(language.locales[3] == "zh-MY");
        OA_CHECK(language.locales[4] == "zh");
    }
    OA_CHECK(language.fallbacks.size() == 1 && language.fallbacks[0] == "en");
    OA_CHECK(language.needs == "modern-fonts");
    OA_CHECK(language.coverage == 1.0);
    OA_CHECK(
        language.sha256 ==
        sha::digest_of(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>("zh-hans-bytes"), 13)
        )
    );

    const catalogue::Package& unmet = out.packages[3];
    OA_CHECK(unmet.id == "future-mod");
    OA_CHECK(unmet.requires_engine_text == ">= 9.0.0");
    OA_CHECK(unmet.requires_engine.has_value());
    if (unmet.requires_engine) {
        OA_CHECK(!oamod::engine_range_met(*unmet.requires_engine, oamod::EngineVersion{0, 8, 0}));
        OA_CHECK(oamod::engine_range_met(*unmet.requires_engine, oamod::EngineVersion{9, 0, 0}));
    }
}

void test_registry_icon() {
    const auto plain = read_ok(catalogue_of(bare("oamod", "keeper")));
    OA_CHECK(plain.icon.empty());
    const std::string reference =
        "/v1/i/cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc.png";
    const auto with_icon =
        read_ok(catalogue_of(bare("oamod", "keeper"), "\"icon\":\"" + reference + "\""));
    OA_CHECK(with_icon.icon == reference);
    OA_CHECK(with_icon.packages.size() == 1);
    refuse(catalogue_of(bare("oamod", "keeper"), "\"icon\":1"), "icon is not a picture reference");
    refuse(
        catalogue_of(bare("oamod", "keeper"), "\"icon\":\"/v1/i/badge.jpg\""),
        "icon is not a picture reference"
    );
    refuse(
        catalogue_of(bare("oamod", "keeper"), "\"icon\":\"http://example.org/a.png\""),
        "icon is not a picture reference"
    );
    refuse(
        catalogue_of(bare("oamod", "keeper"), "\"icon\":\"\""), "icon is not a picture reference"
    );
}

void test_skipped_entries() {
    const std::string hash = hex_of("package");
    skips_one(bare("oamod", "pro@ta"), "pro@ta");
    skips_one(bare("oamod", "Ridge"), "not lower-case kebab-case");
    skips_one(entry_with("oamod", "ridge", "ABCD", "/v1/p/abcd.oamod"), "sha256");
    const std::string upper = [&hash] {
        std::string text = hash;
        for (char& value : text) {
            if (value >= 'a' && value <= 'f')
                value = static_cast<char>(value - 'a' + 'A');
        }
        return text;
    }();
    OA_CHECK(upper != hash);
    skips_one(entry_with("oamod", "ridge", upper, "/v1/p/" + hash + ".oamod"), "sha256");
    skips_one(
        entry_with("oamod", "ridge", hash, "/v1/p/" + hash + ".oalang"), "file not named by sha256"
    );
    skips_one(
        entry_with("oamod", "ridge", hash, "../" + hash + ".oamod"), "file is not a reference"
    );

    const std::string high = std::string("\xc2\xa0", 2);
    const std::vector<std::string> forbidden = {
        "http://host/a.oamod",
        "//host/a.oamod",
        "/v1/./a.oamod",
        "/v1/../a.oamod",
        "/v1//a.oamod",
        "/v1/a.oamod/",
        "v1\\a.oamod",
        "/v1/%2e/a.oamod",
        "/v1/a.oamod?x=1",
        "/v1/a.oamod#frag",
        "/v1/a file.oamod",
        std::string(1, '\x01'),
        std::string(1, '\x7f'),
        high,
        std::string(513, 'a'),
        "",
    };
    for (const std::string& reference : forbidden) {
        OA_CHECK(!catalogue::valid_reference(reference));
        skips_one(
            entry_with("oamod", "ridge", hash, json_escape(reference)), "file is not a reference"
        );
    }
    OA_CHECK(catalogue::valid_reference("/v1/p/abc.oamod"));
    OA_CHECK(catalogue::valid_reference("p/abc.oamod"));
    OA_CHECK(catalogue::valid_reference("abc.oamod"));
    OA_CHECK(catalogue::valid_reference(std::string(512, 'a')));
    const auto relative =
        read_ok(catalogue_of(entry_with("oamod", "ridge", hash, "p/" + hash + ".oamod")));
    OA_CHECK(relative.packages.size() == 1);

    skips_one(bare("oamap", "archipelago", "", false), "map pack names no map");
    skips_one(
        bare(
            "oamap",
            "archipelago",
            "\"maps\":[{\"map\":\"isle@other\",\"name\":\"Isle\",\"players\":2,\"size\":\"16x16\"}"
            "]",
            false
        ),
        "map names another package"
    );
    skips_one("{\"kind\":\"widget\",\"id\":\"ridge\"}", "unknown kind");
    skips_one(bare("oamod", "ridge", "\"homepage\":\"notaurl\""), "homepage");
    OA_CHECK(!oamod::homepage_valid("notaurl"));
    skips_one(bare("oamod", "ridge", "\"tags\":[\"Balance\"]"), "tag");
    OA_CHECK(!oamod::tag_valid("Balance"));
    skips_one(bare("oamod", "ridge", "\"hacks\":{\"ids\":[\"Unit\"]}"), "hack id");

    const auto duplicate = read_ok(catalogue_of(
        bare("oalang", "zh-Hans") + "," + bare("oamod", "zh-hans") + "," + bare("oamod", "keeper")
    ));
    OA_CHECK(duplicate.packages.size() == 1);
    if (!duplicate.packages.empty())
        OA_CHECK(duplicate.packages[0].id == "keeper");
    OA_CHECK(duplicate.problems.size() == 2);
    if (duplicate.problems.size() == 2) {
        OA_CHECK(duplicate.problems[0].find("same key") != std::string::npos);
        OA_CHECK(duplicate.problems[0].find("packages[0]") != std::string::npos);
        OA_CHECK(duplicate.problems[1].find("packages[1]") != std::string::npos);
        OA_CHECK(duplicate.problems[0].find("zh-hans") != std::string::npos);
    }

    std::string range_error;
    OA_CHECK(!oamod::parse_engine_range(">= 0.8", &range_error));
    const auto range = read_ok(catalogue_of(
        bare("oamod", "ridge", "\"requires\":{\"engine\":\">= 0.8\"}") + "," +
        bare("oamod", "keeper")
    ));
    OA_CHECK(range.packages.size() == 1);
    if (!range.packages.empty())
        OA_CHECK(range.packages[0].id == "keeper");
    OA_CHECK(range.problems.size() == 1);
    if (!range.problems.empty()) {
        OA_CHECK(range.problems[0].find("requires.engine") != std::string::npos);
        OA_CHECK(range.problems[0].find(range_error) != std::string::npos);
    }
}

void test_language_key_and_fields() {
    OA_CHECK(catalogue::valid_package_key(catalogue::Kind::oamod, "ridge"));
    OA_CHECK(!catalogue::valid_package_key(catalogue::Kind::oamod, "Ridge"));
    OA_CHECK(!catalogue::valid_package_key(catalogue::Kind::oamod, "pro@ta"));
    OA_CHECK(!catalogue::valid_package_key(catalogue::Kind::oamod, "-ridge"));
    OA_CHECK(!catalogue::valid_package_key(catalogue::Kind::oamod, "a--b"));
    OA_CHECK(catalogue::valid_package_key(catalogue::Kind::oalang, "zh-Hans"));
    OA_CHECK(catalogue::valid_package_key(catalogue::Kind::oalang, "zh"));
    OA_CHECK(!catalogue::valid_package_key(catalogue::Kind::oalang, "e"));
    OA_CHECK(!catalogue::valid_package_key(catalogue::Kind::oalang, "zh-"));
    OA_CHECK(!catalogue::valid_package_key(catalogue::Kind::oalang, "123"));

    const auto language = read_ok(catalogue_of(bare("oalang", "zh-Hans")));
    OA_CHECK(language.packages.size() == 1);
    if (!language.packages.empty()) {
        OA_CHECK(language.packages[0].id == "zh-Hans");
        OA_CHECK(language.packages[0].version.empty());
        OA_CHECK(language.packages[0].revision == 0);
    }

    const std::string hash = hex_of("no-version");
    const std::string missing_version =
        "{\"kind\":\"oamod\",\"id\":\"ridge\",\"name\":\"Name\",\"release\":1,"
        "\"size\":4,\"sha256\":\"" +
        hash + "\",\"file\":\"/v1/p/" + hash + ".oamod\",\"revision\":1}";
    skips_one(missing_version, "version");

    const auto ignored = read_ok(catalogue_of(bare(
        "oamod", "ridge", "\"coverage\":2,\"maps\":[],\"english_name\":\"\",\"locales\":[\"NO\"]"
    )));
    OA_CHECK(ignored.packages.size() == 1);
    if (!ignored.packages.empty()) {
        OA_CHECK(!ignored.packages[0].coverage);
        OA_CHECK(ignored.packages[0].maps.empty());
        OA_CHECK(ignored.packages[0].english_name.empty());
    }
    const auto language_maps = read_ok(catalogue_of(bare("oalang", "zh", "\"maps\":\"nope\"")));
    OA_CHECK(language_maps.packages.size() == 1);
}

void test_coverage_and_bounds() {
    const auto good = read_ok(catalogue_of(
        bare("oalang", "aa", "\"coverage\":0.5") + "," + bare("oalang", "ab", "\"coverage\":1") +
        "," + bare("oalang", "ac", "\"coverage\":1.0") + "," +
        bare("oalang", "ad", "\"coverage\":1e0") + "," +
        bare("oalang", "ae", "\"coverage\":10e-1") + "," + bare("oalang", "af", "\"coverage\":0")
    ));
    OA_CHECK(good.packages.size() == 6);
    OA_CHECK(good.problems.empty());
    if (good.packages.size() == 6) {
        OA_CHECK(good.packages[0].coverage == 0.5);
        OA_CHECK(good.packages[1].coverage == 1.0);
        OA_CHECK(good.packages[2].coverage == 1.0);
        OA_CHECK(good.packages[3].coverage == 1.0);
        OA_CHECK(good.packages[4].coverage == 1.0);
        OA_CHECK(good.packages[5].coverage == 0.0);
    }
    skips_one(bare("oalang", "ag", "\"coverage\":2"), "coverage");
    skips_one(bare("oalang", "ah", "\"coverage\":1.5"), "coverage");
    skips_one(bare("oalang", "ai", "\"coverage\":-1"), "coverage");

    auto sized = [](std::string_view id,
                    std::string_view release,
                    std::string_view size,
                    std::string_view revision) {
        const std::string hash = hex_of(id);
        return std::string("{\"kind\":\"oamod\",\"id\":\"") + std::string(id) +
               "\",\"name\":\"Name\",\"version\":\"1\",\"revision\":" + std::string(revision) +
               ",\"release\":" + std::string(release) + ",\"size\":" + std::string(size) +
               ",\"sha256\":\"" + hash + "\",\"file\":\"/v1/p/" + hash + ".oamod\"}";
    };
    const auto biggest =
        read_ok(catalogue_of(sized("biggest", "2147483647", "4294967296", "65535")));
    OA_CHECK(biggest.packages.size() == 1);
    if (!biggest.packages.empty()) {
        OA_CHECK(biggest.packages[0].release == 2147483647);
        OA_CHECK(biggest.packages[0].size == uint64_t{1} << 32);
        OA_CHECK(biggest.packages[0].revision == 65535);
    }
    skips_one(sized("over-size", "1", "4294967297", "1"), "size");
    skips_one(sized("zero-size", "1", "0", "1"), "size");
    skips_one(sized("over-release", "2147483648", "1", "1"), "release");
    skips_one(sized("zero-release", "0", "1", "1"), "release");
    skips_one(sized("over-revision", "1", "1", "65536"), "revision");

    const auto summary = read_ok(
        catalogue_of(bare("oamod", "ridge", "\"summary\":\"" + std::string(200, 's') + "\""))
    );
    OA_CHECK(summary.packages.size() == 1);
    if (!summary.packages.empty())
        OA_CHECK(summary.packages[0].summary.size() == 200);
    skips_one(bare("oamod", "ridge", "\"summary\":\"" + std::string(201, 's') + "\""), "summary");
    const auto notes = read_ok(
        catalogue_of(bare("oamod", "ridge", "\"notes\":\"" + std::string(2000, 'n') + "\""))
    );
    OA_CHECK(notes.packages.size() == 1);
    skips_one(bare("oamod", "ridge", "\"notes\":\"" + std::string(2001, 'n') + "\""), "notes");

    const auto wide = read_ok(catalogue_of(bare(
        "oamap",
        "islands",
        "\"maps\":[{\"map\":\"stem@islands\",\"name\":\"Stem\",\"players\":2,\"size\":\"256x256\"}"
        "]",
        false
    )));
    OA_CHECK(wide.packages.size() == 1);
    if (!wide.packages.empty() && !wide.packages[0].maps.empty()) {
        OA_CHECK(wide.packages[0].maps[0].width == 256);
        OA_CHECK(wide.packages[0].maps[0].height == 256);
    }
    skips_one(
        bare(
            "oamap",
            "islands",
            "\"maps\":[{\"map\":\"stem@islands\",\"name\":\"Stem\",\"players\":2,\"size\":\"01x1\"}"
            "]",
            false
        ),
        "map size"
    );
    skips_one(
        bare(
            "oamap",
            "islands",
            "\"maps\":[{\"map\":\"stem@islands\",\"name\":\"Stem\",\"players\":2,\"size\":\"0x1\"}"
            "]",
            false
        ),
        "map size"
    );
    skips_one(
        bare(
            "oamap",
            "islands",
            "\"maps\":[{\"map\":\"stem@islands\",\"name\":\"Stem\",\"players\":2,\"size\":\"1x\"}]",
            false
        ),
        "map size"
    );
    skips_one(
        bare(
            "oamap",
            "islands",
            "\"maps\":[{\"map\":\"stem@islands\",\"name\":\"Stem\",\"players\":2,\"size\":"
            "\"257x1\"}]",
            false
        ),
        "map size"
    );

    std::string many_bad;
    for (int index = 0; index < 101; ++index) {
        if (index)
            many_bad += ',';
        many_bad += "{\"kind\":\"nope\"}";
    }
    many_bad += ',';
    many_bad += bare("oamod", "keeper");
    const auto capped = read_ok(catalogue_of(many_bad));
    OA_CHECK(capped.problems.size() == catalogue::max_problems);
    OA_CHECK(capped.packages.size() == 1);
    if (!capped.packages.empty())
        OA_CHECK(capped.packages[0].id == "keeper");
}

void test_structure() {
    refuse("not json", "not JSON");
    refuse(
        "{\"catalogue\":2,\"registry\":\"coreprime\",\"sequence\":1,"
        "\"generated\":\"2026-11-02T04:10:00Z\",\"expires\":\"2026-12-02T04:10:00Z\",\"packages\":["
        "]}",
        "catalogue"
    );
    refuse(
        "{\"catalogue\":1,\"registry\":\"coreprime\","
        "\"generated\":\"2026-11-02T04:10:00Z\",\"expires\":\"2026-12-02T04:10:00Z\",\"packages\":["
        "]}",
        "sequence"
    );
    refuse(
        "{\"catalogue\":1,\"registry\":\"coreprime\",\"sequence\":1,"
        "\"generated\":\"2026-12-02T04:10:00Z\",\"expires\":\"2026-11-02T04:10:00Z\",\"packages\":["
        "]}",
        "expires"
    );
    refuse(
        "{\"catalogue\":1,\"registry\":\"coreprime\",\"sequence\":1,"
        "\"generated\":\"2026-11-02T04:10:00Z\",\"expires\":\"2026-11-02T04:10:00Z\",\"packages\":["
        "]}",
        "expires"
    );
    refuse(
        "{\"catalogue\":1,\"catalogue\":1,\"registry\":\"coreprime\",\"sequence\":1,"
        "\"generated\":\"2026-11-02T04:10:00Z\",\"expires\":\"2026-12-02T04:10:00Z\",\"packages\":["
        "]}",
        "named twice"
    );
    refuse(catalogue_of("{\"kind\":\"oamod\",\"kind\":\"oamod\"}"), "named twice");
    catalogue::Catalogue untouched;
    untouched.registry = "untouched";
    OA_CHECK(!catalogue::read_catalogue(bytes_of("not json"), untouched, nullptr));
    OA_CHECK(untouched.registry == "untouched");

    std::string too_many;
    for (int index = 0; index < 20001; ++index) {
        if (index)
            too_many += ',';
        too_many += "{}";
    }
    refuse(catalogue_of(too_many), "20000");
    std::string at_limit;
    for (int index = 0; index < 20000; ++index) {
        if (index)
            at_limit += ',';
        at_limit += "{}";
    }
    const auto full = read_ok(catalogue_of(at_limit));
    OA_CHECK(full.packages.empty());
    OA_CHECK(full.problems.size() == catalogue::max_problems);

    const auto queried =
        read_ok(catalogue_of("", "\"downloads\":\"http://downloads.example.net/api?x=1\""));
    const auto parsed = url::parse_http_url("http://downloads.example.net/api?x=1");
    OA_CHECK(parsed.has_value());
    if (parsed)
        OA_CHECK(queried.downloads == url::url_text(*parsed));
    const auto secure_downloads =
        read_ok(catalogue_of("", "\"downloads\":\"https://downloads.example.net/api\""));
    OA_CHECK(secure_downloads.downloads == "https://downloads.example.net/api");
    refuse(
        catalogue_of("", "\"mirrors\":[\"http://mirror.example.net/v1/catalogue.json?x=1\"]"),
        "mirrors"
    );

    const auto fraction = read_ok(
        "{\"catalogue\":1,\"registry\":\"coreprime\",\"sequence\":1,"
        "\"generated\":\"2000-02-29T00:00:00.5Z\","
        "\"expires\":\"2000-03-01T00:00:00Z\",\"packages\":[]}"
    );
    OA_CHECK(fraction.generated_text == "2000-02-29T00:00:00.5Z");
    OA_CHECK(fraction.generated == 951782400);

    std::string keys = "\"keys\":[";
    for (int index = 0; index < 8; ++index) {
        if (index)
            keys += ',';
        const signing::PublicKey key = key_from_seed(static_cast<uint8_t>(index + 1));
        keys += "{\"id\":\"k" + std::to_string(index) + "\",\"public\":\"" +
                std::string(signing::public_key_text(key).view()) + "\"}";
    }
    keys += "]";
    const auto eight = read_ok(catalogue_of("", keys));
    OA_CHECK(eight.keys.size() == 8);
    keys.insert(
        keys.size() - 1,
        ",{\"id\":\"k8\",\"public\":\"" +
            std::string(signing::public_key_text(key_from_seed(9)).view()) + "\"}"
    );
    refuse(catalogue_of("", keys), "more than 8");

    const signing::PublicKey first = key_from_seed(1);
    const signing::PublicKey second = key_from_seed(2);
    const std::string first_text(signing::public_key_text(first).view());
    const std::string second_text(signing::public_key_text(second).view());
    refuse(
        catalogue_of(
            "",
            "\"keys\":[{\"id\":\"same\",\"public\":\"" + first_text +
                "\"},{\"id\":\"same\",\"public\":\"" + second_text + "\"}]"
        ),
        "repeats an id"
    );
    refuse(
        catalogue_of(
            "",
            "\"keys\":[{\"id\":\"one\",\"public\":\"" + first_text +
                "\"},{\"id\":\"two\",\"public\":\"" + first_text + "\"}]"
        ),
        "repeats a public key"
    );
}

void test_utc_time() {
    // 2000-01-01T00:00:00Z is 10957 days after 1970-01-01: 30 years of 365
    // days and 7 leap days (1972, 1976, 1980, 1984, 1988, 1992, 1996).
    // 10957 * 86400 = 946684800. 2000-02-29 is 59 days later (31 in January
    // and 28 in February): 59 * 86400 = 5097600, so 951782400. A fraction
    // is the same second.
    OA_CHECK(catalogue::parse_utc_time("1970-01-01T00:00:00Z") == 0);
    OA_CHECK(catalogue::parse_utc_time("1970-01-01T23:59:59Z") == 86399);
    OA_CHECK(catalogue::parse_utc_time("2000-02-29T00:00:00Z") == 951782400);
    OA_CHECK(catalogue::parse_utc_time("2000-02-29T00:00:00.5Z") == 951782400);
    OA_CHECK(!catalogue::parse_utc_time("1900-02-29T00:00:00Z"));
    OA_CHECK(catalogue::parse_utc_time("1900-02-28T00:00:00Z").has_value());
    OA_CHECK(!catalogue::parse_utc_time("2000-02-29T00:00:00z"));
    OA_CHECK(!catalogue::parse_utc_time("2000-02-29T24:00:00Z"));
    OA_CHECK(!catalogue::parse_utc_time("2000-02-29T00:00:60Z"));
    OA_CHECK(!catalogue::parse_utc_time("2000-02-29T00:00:00.Z"));
    OA_CHECK(!catalogue::parse_utc_time(""));
}

void test_published() {
    // Every kind may name the day its release was published; it is kept as written.
    const auto dated = read_ok(catalogue_of(
        bare("oamod", "ridge", "\"published\":\"2026-10-12\"") + "," +
        bare("oamap", "isles", "\"published\":\"2024-02-29\"") + "," +
        bare("oalang", "pt-BR", "\"published\":\"1999-12-31\"") + "," + bare("oamod", "undated")
    ));
    OA_CHECK(dated.packages.size() == 4);
    OA_CHECK(dated.problems.empty());
    if (dated.packages.size() == 4) {
        OA_CHECK(dated.packages[0].published == "2026-10-12");
        OA_CHECK(dated.packages[1].published == "2024-02-29");
        OA_CHECK(dated.packages[2].published == "1999-12-31");
        OA_CHECK(dated.packages[3].published.empty());
    }
    // A malformed day leaves the entry out, as any other malformed member does.
    skips_one(bare("oamod", "late", "\"published\":\"2026-13-01\""), "published");
    skips_one(bare("oamap", "leap", "\"published\":\"2023-02-29\""), "published");
    skips_one(bare("oalang", "fr", "\"published\":\"2026-1-01\""), "published");
    skips_one(bare("oamod", "timed", "\"published\":\"2026-10-12T00:00:00Z\""), "published");
    skips_one(bare("oamod", "number", "\"published\":20261012"), "published");
    skips_one(bare("oamod", "blank", "\"published\":\"\""), "published");

    // 1970-01-01 is day 0, and 2000-02-29 is 10957 + 59 = 11016 days on.
    OA_CHECK(catalogue::parse_day("1970-01-01") == 0);
    OA_CHECK(catalogue::parse_day("2000-02-29") == 11016);
    OA_CHECK(catalogue::parse_day("1969-12-31") == -1);
    OA_CHECK(!catalogue::parse_day("1900-02-29"));
    OA_CHECK(!catalogue::parse_day("2026-10-12 "));
    OA_CHECK(!catalogue::parse_day("2026/10/12"));
    OA_CHECK(!catalogue::parse_day("2026-00-12"));
    OA_CHECK(!catalogue::parse_day("2026-10-00"));
    OA_CHECK(!catalogue::parse_day(""));
}

void test_content_needs() {
    const auto named = read_ok(catalogue_of(bare(
        "oamod",
        "ridge",
        "\"supportedGames\":[\"totala\"],\"requiredExpansions\":[\"battle-tactics\"]"
    )));
    OA_CHECK(named.packages.size() == 1);
    if (!named.packages.empty()) {
        OA_CHECK(named.packages[0].supported_games == std::vector<std::string>{"totala"});
        OA_CHECK(
            named.packages[0].required_expansions == std::vector<std::string>{"battle-tactics"}
        );
    }
    const auto omitted = read_ok(catalogue_of(bare("oamod", "ridge")));
    OA_CHECK(omitted.packages.size() == 1);
    if (!omitted.packages.empty()) {
        OA_CHECK(omitted.packages[0].supported_games.empty());
        OA_CHECK(omitted.packages[0].required_expansions.empty());
    }
    const auto none_required =
        read_ok(catalogue_of(bare("oamod", "ridge", "\"requiredExpansions\":[]")));
    OA_CHECK(none_required.packages.size() == 1);
    if (!none_required.packages.empty()) {
        OA_CHECK(none_required.packages[0].supported_games.empty());
        OA_CHECK(none_required.packages[0].required_expansions.empty());
    }
    skips_one(bare("oamod", "ridge", "\"supportedGames\":[]"), "supportedGames");
    skips_one(bare("oamod", "ridge", "\"supportedGames\":[\"ridge\"]"), "supportedGames");
    skips_one(bare("oamod", "ridge", "\"requiredExpansions\":[\"totala\"]"), "requiredExpansions");
    skips_one(
        bare(
            "oamod",
            "ridge",
            "\"supportedGames\":[\"totala-demo\"],\"requiredExpansions\":[\"battle-tactics\"]"
        ),
        "extends"
    );
}

/// An expansion entry. The file's last segment ends `.oaexp`.
///
/// @param id the package id
/// @param with_version true to name version "1"
/// @param tail extra JSON members, without a leading comma; empty to add none
/// @return one package object
std::string expansion_entry(std::string_view id, bool with_version, std::string_view tail) {
    const std::string hash = hex_of(std::string("expansion/") + std::string(id));
    std::string json = "{\"kind\":\"expansion\",\"id\":\"";
    json += id;
    json += "\",\"name\":\"Cavedog units\"";
    if (with_version)
        json += ",\"version\":\"1\"";
    json += ",\"release\":1,\"size\":4,\"sha256\":\"";
    json += hash;
    json += "\",\"file\":\"/v1/p/";
    json += hash;
    json += ".oaexp\"";
    if (!tail.empty()) {
        json += ',';
        json += tail;
    }
    json += '}';
    return json;
}

void test_expansion() {
    const std::string accepted = expansion_entry("cavedog-units", true, "\"extends\":[\"totala\"]");
    const auto out = read_ok(catalogue_of(accepted));
    OA_CHECK(out.packages.size() == 1);
    OA_CHECK(out.problems.empty());
    if (!out.packages.empty()) {
        const catalogue::Package& package = out.packages[0];
        OA_CHECK(package.kind == catalogue::Kind::expansion);
        OA_CHECK(std::string(catalogue::kind_name(package.kind)) == "expansion");
        OA_CHECK(std::string(catalogue::package_extension(package.kind)) == "oaexp");
        OA_CHECK(package.id == "cavedog-units");
        OA_CHECK(package.version == "1");
        OA_CHECK(package.extends == std::vector<std::string>{"totala"});
        OA_CHECK(
            package.file.size() >= 6 &&
            package.file.compare(package.file.size() - 6, 6, ".oaexp") == 0
        );
    }
    skips_one(expansion_entry("cavedog-units", true, "\"extends\":[\"totala-demo\"]"), "extends");
    skips_one(expansion_entry("ridge", true, "\"extends\":[\"totala\"]"), "not an expansion");
    std::string wrong_suffix = expansion_entry("cavedog-units", true, "\"extends\":[\"totala\"]");
    const auto suffix = wrong_suffix.rfind(".oaexp");
    OA_CHECK(suffix != std::string::npos);
    if (suffix != std::string::npos)
        wrong_suffix.replace(suffix, 6, ".expansion");
    skips_one(wrong_suffix, "file not named");
    skips_one(expansion_entry("cavedog-units", false, "\"extends\":[\"totala\"]"), "version");
    skips_one(expansion_entry("cavedog-units", true, {}), "extends");
}

/// One mutator entry. `version_member` is a JSON member such as `"version":"1"`, or empty to omit it.
std::string
mutator_body(std::string_view id, std::string_view version_member, std::string_view extra) {
    const std::string hash =
        hex_of(std::string("mutator/") + std::string(id) + std::string(version_member));
    std::string json = "{\"kind\":\"oamut\",\"id\":\"";
    json += id;
    json += "\",\"name\":\"Steady Hands\",\"release\":1,\"size\":4,\"sha256\":\"";
    json += hash;
    json += "\",\"file\":\"/v1/p/";
    json += hash;
    json += ".oamut\"";
    if (!version_member.empty()) {
        json += ',';
        json += version_member;
    }
    if (!extra.empty()) {
        json += ',';
        json += extra;
    }
    json += '}';
    return json;
}

void test_mutator() {
    namespace mut = oa::formats::oamut;
    OA_CHECK(std::string(catalogue::kind_name(catalogue::Kind::oamut)) == "oamut");
    OA_CHECK(std::string(catalogue::package_extension(catalogue::Kind::oamut)) == "oamut");
    OA_CHECK(catalogue::valid_package_key(catalogue::Kind::oamut, "steady-hands"));
    OA_CHECK(!catalogue::valid_package_key(catalogue::Kind::oamut, "Steady"));
    OA_CHECK(!catalogue::valid_package_key(catalogue::Kind::oamut, "steady-hands@1"));

    const std::string facts =
        "\"publisher\":\"OA team\",\"author\":\"OA team\","
        "\"summary\":\"A made-up mutator\",\"notes\":\"Smoother aim\","
        "\"homepage\":\"https://example.com/steady-hands\","
        "\"badge\":\"/v1/i/badge.png\",\"published\":\"2026-10-12\","
        "\"tags\":[\"aim\",\"units\"],"
        "\"requires\":{\"engine\":\">= 0.8.0\",\"base\":\"ta-3.1c\",\"later\":true,"
        "\"mutators\":[{\"id\":\"gatehouse\",\"version\":\">= 1.0\",\"note\":\"kept out\"},"
        "{\"id\":\"calm-seas\"}]},"
        "\"works_with\":[\"ta-3.1c\",\"ridge >= 1.0\"],"
        "\"conflicts_with\":[{\"id\":\"shaky-grip\",\"version\":\"< 2\"}],"
        "\"layers\":[\"units\",\"later-layer\",\"weapons\",\"units\",1],"
        "\"options\":["
        "{\"id\":\"steady\",\"label\":\"Steady\",\"type\":\"toggle\",\"default\":true,\"extra\":1},"
        "{\"id\":\"grip\",\"label\":\"Grip\",\"type\":\"choice\",\"values\":[\"soft\",\"firm\"],"
        "\"labels\":[\"Soft\",\"Firm\"],\"default\":\"firm\"},"
        "{\"id\":\"sway\",\"label\":\"Sway\",\"type\":\"number\",\"min\":-1,\"max\":1,"
        "\"step\":0.5,\"default\":-0.5},"
        "{\"id\":\"hands\",\"label\":\"Hands\",\"type\":\"choice\",\"values\":[1,2],"
        "\"default\":1.0},"
        "{\"id\":\"future\",\"label\":\"Future\",\"type\":\"slider\"}"
        "]";
    const auto out =
        read_ok(catalogue_of(bare("oamut", "steady-hands", facts) + "," + bare("oamod", "keeper")));
    OA_CHECK(out.problems.empty());
    OA_CHECK(out.packages.size() == 2);
    if (out.packages.size() == 2) {
        const catalogue::Package& package = out.packages[0];
        OA_CHECK(package.kind == catalogue::Kind::oamut);
        OA_CHECK(package.id == "steady-hands");
        OA_CHECK(package.version == "1");
        OA_CHECK(package.revision == 0);
        OA_CHECK(package.publisher == "OA team");
        OA_CHECK(package.author == "OA team");
        OA_CHECK(package.summary == "A made-up mutator");
        OA_CHECK(package.notes == "Smoother aim");
        OA_CHECK(package.homepage == "https://example.com/steady-hands");
        OA_CHECK(package.badge == "/v1/i/badge.png");
        OA_CHECK(package.published == "2026-10-12");
        OA_CHECK(package.tags == std::vector<std::string>({"aim", "units"}));
        OA_CHECK(package.requires_engine_text == ">= 0.8.0");
        OA_CHECK(package.requires_engine.has_value());
        OA_CHECK(package.requires_base == "ta-3.1c");
        OA_CHECK(package.requires_mutators.size() == 2);
        if (package.requires_mutators.size() == 2) {
            OA_CHECK(package.requires_mutators[0].id == "gatehouse");
            OA_CHECK(package.requires_mutators[0].range_text == ">= 1.0");
            OA_CHECK(package.requires_mutators[0].range.has_value());
            OA_CHECK(package.requires_mutators[1].id == "calm-seas");
            OA_CHECK(package.requires_mutators[1].range_text.empty());
            OA_CHECK(!package.requires_mutators[1].range.has_value());
        }
        OA_CHECK(package.works_with == std::vector<std::string>({"ta-3.1c", "ridge >= 1.0"}));
        OA_CHECK(package.conflicts_with.size() == 1);
        if (!package.conflicts_with.empty()) {
            OA_CHECK(package.conflicts_with[0].id == "shaky-grip");
            OA_CHECK(package.conflicts_with[0].range_text == "< 2");
            OA_CHECK(package.conflicts_with[0].range.has_value());
        }
        OA_CHECK(!package.clashes.has_value());
        OA_CHECK(package.layers.size() == 2);
        if (package.layers.size() == 2) {
            OA_CHECK(package.layers[0] == mut::Layer::units);
            OA_CHECK(package.layers[1] == mut::Layer::weapons);
        }
        OA_CHECK(package.options.size() == 4);
        if (package.options.size() == 4) {
            const catalogue::OptionSummary& toggle = package.options[0];
            OA_CHECK(toggle.id == "steady");
            OA_CHECK(toggle.label == "Steady");
            OA_CHECK(toggle.type == mut::OptionType::toggle);
            OA_CHECK(toggle.default_text == "true");
            OA_CHECK(toggle.values.empty());
            OA_CHECK(toggle.minimum.empty());
            const catalogue::OptionSummary& choice = package.options[1];
            OA_CHECK(choice.id == "grip");
            OA_CHECK(choice.type == mut::OptionType::choice);
            OA_CHECK(choice.values.size() == 2);
            if (choice.values.size() == 2) {
                OA_CHECK(!choice.values[0].number && choice.values[0].text == "soft");
                OA_CHECK(!choice.values[1].number && choice.values[1].text == "firm");
            }
            OA_CHECK(choice.value_labels == std::vector<std::string>({"Soft", "Firm"}));
            OA_CHECK(choice.default_text == "firm");
            const catalogue::OptionSummary& number = package.options[2];
            OA_CHECK(number.type == mut::OptionType::number);
            OA_CHECK(number.minimum == "-1");
            OA_CHECK(number.maximum == "1");
            OA_CHECK(number.step == "0.5");
            OA_CHECK(number.default_text == "-0.5");
            OA_CHECK(number.values.empty());
            const catalogue::OptionSummary& numbered = package.options[3];
            OA_CHECK(numbered.values.size() == 2);
            if (numbered.values.size() == 2) {
                OA_CHECK(numbered.values[0].number && numbered.values[0].text == "1");
                OA_CHECK(numbered.values[1].number && numbered.values[1].text == "2");
            }
            OA_CHECK(numbered.value_labels.empty());
            OA_CHECK(numbered.default_text == "1.0");
        }
        OA_CHECK(
            package.file.size() >= 6 &&
            package.file.compare(package.file.size() - 6, 6, ".oamut") == 0
        );
        const catalogue::Package& keeper = out.packages[1];
        OA_CHECK(keeper.id == "keeper");
        OA_CHECK(keeper.requires_mutators.empty());
        OA_CHECK(keeper.works_with.empty());
        OA_CHECK(keeper.conflicts_with.empty());
        OA_CHECK(keeper.layers.empty());
        OA_CHECK(!keeper.clashes.has_value());
        OA_CHECK(keeper.options.empty());
    }

    const auto any_mod =
        read_ok(catalogue_of(bare("oamut", "steady-hands", "\"works_with\":[\"*\"]")));
    OA_CHECK(any_mod.packages.size() == 1);
    if (!any_mod.packages.empty())
        OA_CHECK(any_mod.packages[0].works_with == std::vector<std::string>({"*"}));
    const auto on_minimum = read_ok(catalogue_of(bare(
        "oamut",
        "steady-hands",
        "\"options\":[{\"id\":\"sway\",\"label\":\"Sway\",\"type\":\"number\","
        "\"min\":0,\"max\":1,\"step\":0.5,\"default\":0}]"
    )));
    OA_CHECK(on_minimum.packages.size() == 1);
    if (!on_minimum.packages.empty() && !on_minimum.packages[0].options.empty())
        OA_CHECK(on_minimum.packages[0].options[0].default_text == "0");

    const auto sixteen = read_ok(catalogue_of(bare(
        "oamut",
        "steady-hands",
        "\"requires\":{\"mutators\":["
        "{\"id\":\"m0\"},{\"id\":\"m1\"},{\"id\":\"m2\"},{\"id\":\"m3\"},"
        "{\"id\":\"m4\"},{\"id\":\"m5\"},{\"id\":\"m6\"},{\"id\":\"m7\"},"
        "{\"id\":\"m8\"},{\"id\":\"m9\"},{\"id\":\"m10\"},{\"id\":\"m11\"},"
        "{\"id\":\"m12\"},{\"id\":\"m13\"},{\"id\":\"m14\"},{\"id\":\"m15\"}]}"
    )));
    OA_CHECK(sixteen.packages.size() == 1);
    if (!sixteen.packages.empty())
        OA_CHECK(sixteen.packages[0].requires_mutators.size() == 16);

    const auto ignored = read_ok(catalogue_of(bare(
        "oamod",
        "ridge",
        "\"requires\":{\"mutators\":[{\"id\":\"!!!\"},1],\"engine\":\">= 0.8.0\"},"
        "\"works_with\":[\"not a condition\"],\"conflicts_with\":[{}],"
        "\"layers\":[\"nope\"],\"options\":[{\"type\":\"slider\",\"id\":\"!!!\"}]"
    )));
    OA_CHECK(ignored.problems.empty());
    OA_CHECK(ignored.packages.size() == 1);
    if (!ignored.packages.empty()) {
        OA_CHECK(ignored.packages[0].requires_mutators.empty());
        OA_CHECK(ignored.packages[0].works_with.empty());
        OA_CHECK(ignored.packages[0].conflicts_with.empty());
        OA_CHECK(ignored.packages[0].layers.empty());
        OA_CHECK(ignored.packages[0].options.empty());
        OA_CHECK(ignored.packages[0].requires_engine_text == ">= 0.8.0");
    }

    skips_one(mutator_body("steady-hands", "", {}), "version");
    skips_one(mutator_body("steady-hands", "\"version\":\"hello\"", {}), "mutator version");
    skips_one(mutator_body("steady-hands", "\"version\":\"1.2.3.4\"", {}), "mutator version");
    skips_one(mutator_body("steady-hands", "\"version\":\"01\"", {}), "mutator version");
    skips_one(
        bare("oamut", "steady-hands", "\"requires\":{\"mutators\":\"gatehouse\"}"),
        "requires.mutators is not a list"
    );
    {
        std::string many = "\"requires\":{\"mutators\":[";
        for (int slot = 0; slot < 17; ++slot) {
            if (slot != 0)
                many += ',';
            many += "{\"id\":\"m" + std::to_string(slot) + "\"}";
        }
        many += "]}";
        skips_one(bare("oamut", "steady-hands", many), "lists more than 16");
    }
    skips_one(
        bare("oamut", "steady-hands", "\"requires\":{\"mutators\":[\"gatehouse\"]}"),
        "requires.mutators[0] is not an object"
    );
    skips_one(
        bare("oamut", "steady-hands", "\"requires\":{\"mutators\":[{\"id\":\"Gatehouse\"}]}"),
        "requires.mutators[0].id is not a mutator key"
    );
    skips_one(
        bare("oamut", "steady-hands", "\"requires\":{\"mutators\":[{\"id\":\"steady-hands\"}]}"),
        "requires.mutators[0].id is this mutator"
    );
    skips_one(
        bare(
            "oamut",
            "steady-hands",
            "\"requires\":{\"mutators\":[{\"id\":\"calm-seas\"},{\"id\":\"calm-seas\"}]}"
        ),
        "requires.mutators[1].id is repeated"
    );
    skips_one(
        bare(
            "oamut",
            "steady-hands",
            "\"requires\":{\"mutators\":[{\"id\":\"m0\"},{\"id\":\"m1\"},"
            "{\"id\":\"m2\",\"version\":\"soon\"}]}"
        ),
        "requires.mutators[2].version is not a version range"
    );
    skips_one(
        bare("oamut", "steady-hands", "\"conflicts_with\":{}"), "conflicts_with is not a list"
    );
    {
        std::string many = "\"conflicts_with\":[";
        for (int slot = 0; slot < 33; ++slot) {
            if (slot != 0)
                many += ',';
            many += "{\"id\":\"c" + std::to_string(slot) + "\"}";
        }
        many += ']';
        skips_one(bare("oamut", "steady-hands", many), "lists more than 32");
    }
    skips_one(
        bare("oamut", "steady-hands", "\"conflicts_with\":[{\"id\":\"steady-hands\"}]"),
        "conflicts_with[0].id is this mutator"
    );
    skips_one(
        bare(
            "oamut",
            "steady-hands",
            "\"requires\":{\"mutators\":[{\"id\":\"gatehouse\",\"version\":\">= 1\"}]},"
            "\"conflicts_with\":[{\"id\":\"gatehouse\",\"version\":\"< 2\"}]"
        ),
        "conflicts_with[0].id is a prerequisite"
    );
    skips_one(
        bare("oamut", "steady-hands", "\"conflicts_with\":[{\"id\":\"shaky-grip\",\"version\":1}]"),
        "conflicts_with[0].version is not a version range"
    );
    skips_one(bare("oamut", "steady-hands", "\"works_with\":\"*\""), "works_with is not a list");
    {
        std::string many = "\"works_with\":[";
        for (int slot = 0; slot < 33; ++slot) {
            if (slot != 0)
                many += ',';
            many += "\"m" + std::to_string(slot) + "\"";
        }
        many += ']';
        skips_one(bare("oamut", "steady-hands", many), "lists more than 32");
    }
    skips_one(
        bare("oamut", "steady-hands", "\"works_with\":[\"Ridge\"]"),
        "works_with[0] is not a condition"
    );
    skips_one(
        bare("oamut", "steady-hands", "\"works_with\":[\"* >= 1\"]"),
        "works_with[0] is not a condition"
    );
    skips_one(
        bare("oamut", "steady-hands", "\"works_with\":[\"*\",\"ridge\"]"),
        "works_with[0] is not alone"
    );
    {
        std::string many = "\"layers\":[";
        for (int slot = 0; slot < 17; ++slot) {
            if (slot != 0)
                many += ',';
            many += "\"units\"";
        }
        many += ']';
        skips_one(bare("oamut", "steady-hands", many), "layers lists more than 16");
    }
    skips_one(bare("oamut", "steady-hands", "\"layers\":\"units\""), "layers is not a list");
    const auto added = read_ok(catalogue_of(bare("oamut", "steady-hands", "\"clashes\":\"add\"")));
    OA_CHECK(added.packages.size() == 1);
    if (!added.packages.empty())
        OA_CHECK(added.packages[0].clashes == mut::ClashMode::add);
    const auto replaced =
        read_ok(catalogue_of(bare("oamut", "steady-hands", "\"clashes\":\"replace\"")));
    OA_CHECK(replaced.packages.size() == 1);
    if (!replaced.packages.empty())
        OA_CHECK(replaced.packages[0].clashes == mut::ClashMode::replace);
    skips_one(
        bare("oamut", "steady-hands", "\"clashes\":\"Add\""), "clashes must be add or replace"
    );
    skips_one(
        bare("oamut", "steady-hands", "\"clashes\":\"merge\""), "clashes must be add or replace"
    );
    skips_one(bare("oamut", "steady-hands", "\"clashes\":1"), "clashes must be add or replace");
    {
        std::string many = "\"options\":[";
        for (int slot = 0; slot < 17; ++slot) {
            if (slot != 0)
                many += ',';
            many += "{\"id\":\"sway\",\"label\":\"Sway\",\"type\":\"toggle\"}";
        }
        many += ']';
        skips_one(bare("oamut", "steady-hands", many), "options lists more than 16");
    }
    skips_one(bare("oamut", "steady-hands", "\"options\":[1]"), "options[0] is not an object");
    skips_one(
        bare(
            "oamut",
            "steady-hands",
            "\"options\":[{\"id\":\"1sway\",\"label\":\"Sway\",\"type\":\"toggle\"}]"
        ),
        "options[0].id is not an option id"
    );
    skips_one(
        bare(
            "oamut",
            "steady-hands",
            "\"options\":[{\"id\":\"" + std::string(33, 'a') +
                "\",\"label\":\"Sway\",\"type\":\"toggle\"}]"
        ),
        "options[0].id is not an option id"
    );
    {
        constexpr std::array<std::string_view, 22> reserved{
            "and",      "break",  "do",   "else", "elseif", "end",   "false", "for",
            "function", "goto",   "if",   "in",   "local",  "nil",   "not",   "or",
            "repeat",   "return", "then", "true", "until",  "while",
        };
        for (const std::string_view word : reserved) {
            skips_one(
                bare(
                    "oamut",
                    "steady-hands",
                    "\"options\":[{\"id\":\"" + std::string{word} +
                        "\",\"label\":\"Sway\",\"type\":\"toggle\"}]"
                ),
                "options[0].id is a word Lua reserves"
            );
        }
    }
    {
        const auto kept = read_ok(catalogue_of(
            bare(
                "oamut",
                "steady-hands",
                "\"options\":["
                "{\"id\":\"format\",\"label\":\"Format\",\"type\":\"toggle\"},"
                "{\"id\":\"ends\",\"label\":\"Ends\",\"type\":\"toggle\"},"
                "{\"id\":\"for_\",\"label\":\"For underscore\",\"type\":\"toggle\"}]"
            ) +
            "," + bare("oamod", "keeper")
        ));
        OA_CHECK(kept.packages.size() == 2);
        OA_CHECK(kept.problems.empty());
        if (kept.packages.size() == 2) {
            OA_CHECK(kept.packages[0].id == "steady-hands");
            OA_CHECK(kept.packages[0].options.size() == 3);
            if (kept.packages[0].options.size() == 3) {
                OA_CHECK(kept.packages[0].options[0].id == "format");
                OA_CHECK(kept.packages[0].options[1].id == "ends");
                OA_CHECK(kept.packages[0].options[2].id == "for_");
            }
            OA_CHECK(kept.packages[1].id == "keeper");
        }
    }
    skips_one(
        bare(
            "oamut",
            "steady-hands",
            "\"options\":[{\"id\":\"For\",\"label\":\"For\",\"type\":\"toggle\"}]"
        ),
        "options[0].id is not an option id"
    );
    skips_one(
        bare(
            "oamut",
            "steady-hands",
            "\"options\":[{\"id\":\"sway\",\"label\":\"\",\"type\":\"toggle\"}]"
        ),
        "options[0].label is not 1 to 64 bytes"
    );
    skips_one(
        bare(
            "oamut",
            "steady-hands",
            "\"options\":[{\"id\":\"sway\",\"label\":\"" + std::string(65, 'a') +
                "\",\"type\":\"toggle\"}]"
        ),
        "options[0].label is not 1 to 64 bytes"
    );
    skips_one(
        bare(
            "oamut",
            "steady-hands",
            "\"options\":[{\"id\":\"grip\",\"label\":\"Grip\",\"type\":\"choice\",\"values\":["
            "\"only\"]}]"
        ),
        "options[0].values is not 2 to 16 values"
    );
    skips_one(
        bare(
            "oamut",
            "steady-hands",
            "\"options\":[{\"id\":\"grip\",\"label\":\"Grip\",\"type\":\"choice\","
            "\"values\":[\"soft\",1]}]"
        ),
        "options[0].values[1] is not the same kind of value"
    );
    skips_one(
        bare(
            "oamut",
            "steady-hands",
            "\"options\":[{\"id\":\"grip\",\"label\":\"Grip, firm.\",\"type\":\"choice\","
            "\"values\":[\"a b\",\"firm\"]}]"
        ),
        "options[0].values[0] is not a number or 1 to 32 letters, digits, _, - or ."
    );
    skips_one(
        bare(
            "oamut",
            "steady-hands",
            "\"options\":[{\"id\":\"sway\",\"label\":\"Sway\",\"type\":\"number\","
            "\"min\":1,\"max\":1,\"step\":1}]"
        ),
        "options[0].min is not below max"
    );
    skips_one(
        bare(
            "oamut",
            "steady-hands",
            "\"options\":[{\"id\":\"sway\",\"label\":\"Sway\",\"type\":\"number\","
            "\"min\":0,\"max\":1,\"step\":0}]"
        ),
        "options[0].step is not above zero"
    );
    skips_one(
        bare(
            "oamut",
            "steady-hands",
            "\"options\":[{\"id\":\"sway\",\"label\":\"Sway\",\"type\":\"number\","
            "\"min\":0,\"max\":1,\"step\":0.5,\"default\":0.25}]"
        ),
        "options[0].default is not on a step"
    );
    skips_one(
        bare(
            "oamut",
            "steady-hands",
            "\"options\":[{\"id\":\"steady\",\"label\":\"Steady\",\"type\":\"toggle\","
            "\"default\":\"yes\"}]"
        ),
        "options[0].default is not true or false"
    );
    skips_one(
        bare(
            "oamut",
            "steady-hands",
            "\"options\":[{\"id\":\"grip\",\"label\":\"Grip\",\"type\":\"choice\","
            "\"values\":[\"soft\",\"firm\"],\"default\":\"other\"}]"
        ),
        "options[0].default is not one of the values"
    );
    skips_one(
        bare(
            "oamut",
            "steady-hands",
            "\"options\":[{\"id\":\"grip\",\"label\":\"Grip\",\"type\":\"choice\","
            "\"values\":[\"soft\",\"firm\"],\"labels\":[\"Soft\"]}]"
        ),
        "options[0].labels is not one label for each value"
    );

    std::string wrong_file = bare("oamut", "steady-hands");
    const auto suffix = wrong_file.rfind(".oamut");
    OA_CHECK(suffix != std::string::npos);
    if (suffix != std::string::npos)
        wrong_file.replace(suffix, 6, ".oamod");
    skips_one(wrong_file, "file not named");
    skips_one("{\"kind\":\"oafuture\",\"id\":\"steady-hands\"}", "unknown kind");

    const auto duplicate = read_ok(catalogue_of(
        bare("oamut", "ridge") + "," + bare("oamod", "ridge") + "," + bare("oamod", "keeper")
    ));
    OA_CHECK(duplicate.packages.size() == 1);
    if (!duplicate.packages.empty())
        OA_CHECK(duplicate.packages[0].id == "keeper");
    OA_CHECK(duplicate.problems.size() == 2);
    if (duplicate.problems.size() == 2) {
        OA_CHECK(duplicate.problems[0].find("same key") != std::string::npos);
        OA_CHECK(duplicate.problems[0].find("ridge") != std::string::npos);
    }
}

/// One brain entry. `members` is the JSON members after `file`, without a leading comma.
std::string brain_entry(std::string_view id, std::string_view members) {
    const std::string hash = hex_of(std::string("brain/") + std::string(id) + std::string(members));
    std::string json = "{\"kind\":\"oabrain\",\"id\":\"";
    json += id;
    json += "\",\"name\":\"Steady Commander\",\"release\":1,\"size\":4,\"sha256\":\"";
    json += hash;
    json += "\",\"file\":\"/v1/p/";
    json += hash;
    json += ".oabrain\"";
    if (!members.empty()) {
        json += ',';
        json += members;
    }
    json += '}';
    return json;
}

void test_brain() {
    namespace brain = oa::formats::oabrain;
    namespace mut = oa::formats::oamut;
    OA_CHECK(std::string(catalogue::kind_name(catalogue::Kind::oabrain)) == "oabrain");
    OA_CHECK(std::string(catalogue::package_extension(catalogue::Kind::oabrain)) == "oabrain");
    OA_CHECK(catalogue::valid_package_key(catalogue::Kind::oabrain, "steady-commander"));
    OA_CHECK(!catalogue::valid_package_key(catalogue::Kind::oabrain, "Steady"));
    OA_CHECK(!catalogue::valid_package_key(catalogue::Kind::oabrain, "steady-commander@1"));

    const std::string facts =
        "\"version\":\"1.2\",\"publisher\":\"OA team\",\"author\":\"OA team\","
        "\"summary\":\"A made-up brain\",\"notes\":\"Teaches the opening\","
        "\"homepage\":\"https://example.com/steady-commander\","
        "\"badge\":\"/v1/i/badge.png\",\"published\":\"2026-10-12\","
        "\"tags\":[\"economy\",\"teaching\",\"custom\"],"
        "\"requires\":{\"engine\":\">= 0.8.0\",\"base\":\"ta-3.1c\","
        "\"mutators\":\"gatehouse\",\"later\":true},"
        "\"supportedGames\":[\"totala\"],\"requiredExpansions\":[\"core-contingency\"],"
        "\"works_with\":[\"ta-3.1c\",\"ridge >= 28\"],"
        "\"conflicts_with\":1,\"layers\":\"units\",\"clashes\":\"merge\","
        "\"full_awareness\":false,\"network_bytes\":2100000,"
        "\"levels\":["
        "{\"id\":\"careful\",\"name\":\"Careful\",\"difficulty\":\"easy\",\"note\":\"kept out\"},"
        "{\"id\":\"steady\",\"name\":\"Steady\",\"difficulty\":\"medium\"}"
        "],"
        "\"options\":["
        "{\"id\":\"teach\",\"label\":\"Teaching\",\"type\":\"toggle\",\"default\":true},"
        "{\"id\":\"future\",\"label\":\"Future\",\"type\":\"slider\"}"
        "]";
    const auto out = read_ok(
        catalogue_of(brain_entry("steady-commander", facts) + "," + bare("oamod", "keeper"))
    );
    OA_CHECK(out.problems.empty());
    OA_CHECK(out.packages.size() == 2);
    if (out.packages.size() == 2) {
        const catalogue::Package& package = out.packages[0];
        OA_CHECK(package.kind == catalogue::Kind::oabrain);
        OA_CHECK(package.id == "steady-commander");
        OA_CHECK(package.name == "Steady Commander");
        OA_CHECK(package.version == "1.2");
        OA_CHECK(package.revision == 0);
        OA_CHECK(package.publisher == "OA team");
        OA_CHECK(package.author == "OA team");
        OA_CHECK(package.summary == "A made-up brain");
        OA_CHECK(package.notes == "Teaches the opening");
        OA_CHECK(package.homepage == "https://example.com/steady-commander");
        OA_CHECK(package.badge == "/v1/i/badge.png");
        OA_CHECK(package.published == "2026-10-12");
        OA_CHECK(package.tags == std::vector<std::string>({"economy", "teaching", "custom"}));
        OA_CHECK(package.requires_engine_text == ">= 0.8.0");
        OA_CHECK(package.requires_engine.has_value());
        OA_CHECK(package.requires_base == "ta-3.1c");
        OA_CHECK(package.requires_mutators.empty());
        OA_CHECK(package.supported_games == std::vector<std::string>({"totala"}));
        OA_CHECK(package.required_expansions == std::vector<std::string>({"core-contingency"}));
        OA_CHECK(package.works_with == std::vector<std::string>({"ta-3.1c", "ridge >= 28"}));
        OA_CHECK(package.conflicts_with.empty());
        OA_CHECK(package.layers.empty());
        OA_CHECK(!package.clashes.has_value());
        OA_CHECK(!package.full_awareness);
        OA_CHECK(package.network_bytes == 2100000);
        OA_CHECK(package.levels.size() == 2);
        if (package.levels.size() == 2) {
            OA_CHECK(package.levels[0].id == "careful");
            OA_CHECK(package.levels[0].name == "Careful");
            OA_CHECK(package.levels[0].difficulty == brain::Difficulty::easy);
            OA_CHECK(package.levels[1].id == "steady");
            OA_CHECK(package.levels[1].name == "Steady");
            OA_CHECK(package.levels[1].difficulty == brain::Difficulty::medium);
        }
        OA_CHECK(package.options.size() == 1);
        if (!package.options.empty()) {
            OA_CHECK(package.options[0].id == "teach");
            OA_CHECK(package.options[0].label == "Teaching");
            OA_CHECK(package.options[0].type == mut::OptionType::toggle);
            OA_CHECK(package.options[0].default_text == "true");
        }
        OA_CHECK(
            package.file.size() >= 8 &&
            package.file.compare(package.file.size() - 8, 8, ".oabrain") == 0
        );
        const catalogue::Package& keeper = out.packages[1];
        OA_CHECK(keeper.id == "keeper");
        OA_CHECK(keeper.levels.empty());
        OA_CHECK(!keeper.full_awareness);
        OA_CHECK(keeper.network_bytes == 0);
        OA_CHECK(keeper.works_with.empty());
        OA_CHECK(keeper.options.empty());
    }

    const std::string levels_only =
        "\"levels\":[{\"id\":\"careful\",\"name\":\"Careful\",\"difficulty\":\"easy\"}]";
    const std::string one_level = "\"version\":\"1\"," + levels_only;
    const auto defaults = read_ok(catalogue_of(brain_entry("steady-commander", one_level)));
    OA_CHECK(defaults.packages.size() == 1);
    if (!defaults.packages.empty()) {
        OA_CHECK(!defaults.packages[0].full_awareness);
        OA_CHECK(defaults.packages[0].network_bytes == 0);
        OA_CHECK(defaults.packages[0].levels.size() == 1);
    }

    const auto skipped = read_ok(catalogue_of(brain_entry(
        "steady-commander",
        "\"version\":\"1\",\"levels\":["
        "{\"id\":\"careful\",\"name\":\"Careful\",\"difficulty\":\"easy\"},"
        "{\"id\":\"brutal\",\"name\":\"Brutal\",\"difficulty\":\"brutal\"}"
        "]"
    )));
    OA_CHECK(skipped.problems.empty());
    OA_CHECK(skipped.packages.size() == 1);
    if (!skipped.packages.empty()) {
        OA_CHECK(skipped.packages[0].levels.size() == 1);
        if (!skipped.packages[0].levels.empty())
            OA_CHECK(skipped.packages[0].levels[0].id == "careful");
    }
    skips_one(
        brain_entry(
            "steady-commander",
            "\"version\":\"1\",\"levels\":["
            "{\"id\":\"brutal\",\"name\":\"Brutal\",\"difficulty\":\"brutal\"}]"
        ),
        "levels names no level this build knows"
    );

    skips_one(brain_entry("steady-commander", levels_only), "version is missing");
    skips_one(
        brain_entry("steady-commander", "\"version\":\"1.2.3.4\"," + levels_only),
        "version is not a mutator version"
    );
    const std::string long_version(33, '1');
    skips_one(
        brain_entry("steady-commander", "\"version\":\"" + long_version + "\"," + levels_only),
        "version is not 1 to 32 bytes"
    );
    skips_one(brain_entry("steady-commander", "\"version\":\"1\""), "levels is missing");
    skips_one(
        brain_entry("steady-commander", "\"version\":\"1\",\"levels\":\"easy\""),
        "levels is not a list"
    );
    skips_one(
        brain_entry("steady-commander", "\"version\":\"1\",\"levels\":[]"), "levels lists none"
    );
    std::string many_levels = "\"version\":\"1\",\"levels\":[";
    for (int index = 0; index < 7; ++index) {
        if (index != 0)
            many_levels += ',';
        many_levels += "{\"id\":\"level-";
        many_levels += std::to_string(index);
        many_levels += "\",\"name\":\"Level\",\"difficulty\":\"easy\"}";
    }
    many_levels += ']';
    skips_one(brain_entry("steady-commander", many_levels), "levels lists more than 6");
    skips_one(
        brain_entry("steady-commander", "\"version\":\"1\",\"levels\":[1]"),
        "levels[0] is not an object"
    );
    skips_one(
        brain_entry(
            "steady-commander",
            "\"version\":\"1\",\"levels\":[{\"id\":\"Careful\",\"name\":\"Careful\","
            "\"difficulty\":\"easy\"}]"
        ),
        "levels[0].id is not a level id"
    );
    skips_one(
        brain_entry(
            "steady-commander",
            "\"version\":\"1\",\"levels\":["
            "{\"id\":\"careful\",\"name\":\"Careful\",\"difficulty\":\"easy\"},"
            "{\"id\":\"careful\",\"name\":\"Again\",\"difficulty\":\"hard\"}]"
        ),
        "levels[1].id is repeated"
    );
    const std::string long_name(65, 'n');
    skips_one(
        brain_entry(
            "steady-commander",
            "\"version\":\"1\",\"levels\":["
            "{\"id\":\"careful\",\"name\":\"Careful\",\"difficulty\":\"easy\"},"
            "{\"id\":\"steady\",\"name\":\"" +
                long_name + "\",\"difficulty\":\"medium\"}]"
        ),
        "levels[1].name is not 1 to 64 bytes"
    );
    skips_one(
        brain_entry("steady-commander", one_level + ",\"full_awareness\":\"yes\""),
        "full_awareness is not true or false"
    );
    skips_one(
        brain_entry("steady-commander", one_level + ",\"network_bytes\":67108865"),
        "network_bytes is not a whole number from 0 to 67108864"
    );
    skips_one(
        brain_entry("steady-commander", one_level + ",\"works_with\":\"*\""),
        "works_with is not a list"
    );
    skips_one(
        brain_entry(
            "steady-commander",
            one_level + ",\"options\":[{\"id\":\"teach\",\"label\":\"Teaching\","
                        "\"type\":\"toggle\",\"default\":\"yes\"}]"
        ),
        "options[0].default is not true or false"
    );

    const auto untouched = read_ok(catalogue_of(
        bare(
            "oamod",
            "ridge",
            "\"levels\":\"nope\",\"full_awareness\":\"yes\",\"network_bytes\":-1,"
            "\"works_with\":[\"*\"],\"options\":[1]"
        ) +
        "," +
        bare(
            "oamut",
            "steady-hands",
            "\"levels\":1,\"full_awareness\":\"yes\",\"network_bytes\":\"lots\""
        )
    ));
    OA_CHECK(untouched.problems.empty());
    OA_CHECK(untouched.packages.size() == 2);
    if (untouched.packages.size() == 2) {
        OA_CHECK(untouched.packages[0].kind == catalogue::Kind::oamod);
        OA_CHECK(untouched.packages[0].levels.empty());
        OA_CHECK(!untouched.packages[0].full_awareness);
        OA_CHECK(untouched.packages[0].network_bytes == 0);
        OA_CHECK(untouched.packages[0].works_with.empty());
        OA_CHECK(untouched.packages[1].kind == catalogue::Kind::oamut);
        OA_CHECK(untouched.packages[1].levels.empty());
        OA_CHECK(!untouched.packages[1].full_awareness);
        OA_CHECK(untouched.packages[1].network_bytes == 0);
    }

    std::string wrong_file = brain_entry("steady-commander", one_level);
    const auto suffix = wrong_file.rfind(".oabrain");
    OA_CHECK(suffix != std::string::npos);
    if (suffix != std::string::npos)
        wrong_file.replace(suffix, 8, ".oamod");
    skips_one(wrong_file, "file not named");
    skips_one("{\"kind\":\"oafuture\",\"id\":\"steady-commander\"}", "unknown kind");

    const auto duplicate = read_ok(catalogue_of(
        brain_entry("ridge", one_level) + "," + bare("oamod", "ridge") + "," +
        bare("oamod", "keeper")
    ));
    OA_CHECK(duplicate.packages.size() == 1);
    if (!duplicate.packages.empty())
        OA_CHECK(duplicate.packages[0].id == "keeper");
    OA_CHECK(duplicate.problems.size() == 2);
    if (duplicate.problems.size() == 2) {
        OA_CHECK(duplicate.problems[0].find("same key") != std::string::npos);
        OA_CHECK(duplicate.problems[0].find("ridge") != std::string::npos);
        OA_CHECK(duplicate.problems[1].find("same key") != std::string::npos);
    }
}

/// One key-layout entry. `members` is the JSON members after `file`, without a leading comma.
std::string key_entry(std::string_view id, std::string_view members) {
    const std::string hash = hex_of(std::string("keys/") + std::string(id) + std::string(members));
    std::string json = "{\"kind\":\"oakeys\",\"id\":\"";
    json += id;
    json += "\",\"name\":\"Grid, left hand\",\"release\":1,\"size\":4,\"sha256\":\"";
    json += hash;
    json += "\",\"file\":\"/v1/p/";
    json += hash;
    json += ".oakeys\"";
    if (!members.empty()) {
        json += ',';
        json += members;
    }
    json += '}';
    return json;
}

void test_oakeys() {
    OA_CHECK(std::string(catalogue::kind_name(catalogue::Kind::oakeys)) == "oakeys");
    OA_CHECK(std::string(catalogue::package_extension(catalogue::Kind::oakeys)) == "oakeys");
    OA_CHECK(catalogue::valid_package_key(catalogue::Kind::oakeys, "grid-left-hand"));
    OA_CHECK(!catalogue::valid_package_key(catalogue::Kind::oakeys, "Grid"));

    const std::string facts = "\"version\":\"1.0\",\"base\":\"oa-default\","
                              "\"works_with\":[\"ta-3.1c\",\"ridge >= 28\"],"
                              "\"keyboard\":\"iso\",\"pointer\":\"left-click\","
                              "\"helpers\":[\"toggle\",\"nope\",\"sticky-modifiers\"],"
                              "\"changes\":11,"
                              "\"triggers\":[\"Q\",\"Nope\",\"@KeyW\",\"Ctrl\",\"MouseLeft\"],"
                              "\"preview\":\"/v1/i/preview.png\","
                              "\"tags\":[\"grid\",\"left-hand\"],\"badge\":\"/v1/i/badge.png\"";
    const auto out =
        read_ok(catalogue_of(key_entry("grid-left-hand", facts) + "," + bare("oamod", "keeper")));
    OA_CHECK(out.problems.empty());
    OA_CHECK(out.packages.size() == 2);
    if (out.packages.size() == 2) {
        const catalogue::Package& package = out.packages[0];
        OA_CHECK(package.kind == catalogue::Kind::oakeys);
        OA_CHECK(package.id == "grid-left-hand");
        OA_CHECK(package.version == "1.0");
        OA_CHECK(package.key_base == "oa-default");
        OA_CHECK(package.keyboard == "iso");
        OA_CHECK(package.pointer == "left-click");
        OA_CHECK(package.helpers == std::vector<std::string>({"toggle", "sticky-modifiers"}));
        OA_CHECK(package.changes == 11);
        OA_CHECK(package.triggers == std::vector<std::string>({"Q", "@KeyW", "MouseLeft"}));
        OA_CHECK(package.preview == "/v1/i/preview.png");
        OA_CHECK(package.works_with == std::vector<std::string>({"ta-3.1c", "ridge >= 28"}));
        OA_CHECK(package.tags == std::vector<std::string>({"grid", "left-hand"}));
        OA_CHECK(package.badge == "/v1/i/badge.png");
        OA_CHECK(
            package.file.size() >= 7 &&
            package.file.compare(package.file.size() - 7, 7, ".oakeys") == 0
        );
        const catalogue::Package& keeper = out.packages[1];
        OA_CHECK(keeper.id == "keeper");
        OA_CHECK(keeper.key_base.empty());
        OA_CHECK(keeper.keyboard.empty());
        OA_CHECK(keeper.changes == 0);
        OA_CHECK(keeper.helpers.empty());
        OA_CHECK(keeper.triggers.empty());
        OA_CHECK(keeper.preview.empty());
    }

    const std::string base_only = "\"version\":\"1.0\",\"base\":\"none\"";
    const auto defaults = read_ok(catalogue_of(key_entry("grid-left-hand", base_only)));
    OA_CHECK(defaults.packages.size() == 1);
    if (!defaults.packages.empty()) {
        OA_CHECK(defaults.packages[0].keyboard == "any");
        OA_CHECK(defaults.packages[0].changes == 0);
        OA_CHECK(defaults.packages[0].pointer.empty());
        OA_CHECK(defaults.packages[0].helpers.empty());
        OA_CHECK(defaults.packages[0].triggers.empty());
        OA_CHECK(defaults.packages[0].key_base == "none");
    }

    const auto skipped = read_ok(catalogue_of(
        key_entry("grid-left-hand", base_only + ",\"triggers\":[\"Ctrl\",\"Pad.A\",\"Pad.a\"]")
    ));
    OA_CHECK(skipped.problems.empty());
    OA_CHECK(skipped.packages.size() == 1);
    if (!skipped.packages.empty())
        OA_CHECK(skipped.packages[0].triggers == std::vector<std::string>({"Pad.A"}));

    skips_one(key_entry("grid-left-hand", "\"version\":\"1.0\""), "base is missing");
    skips_one(
        key_entry("grid-left-hand", "\"version\":\"1.0\",\"base\":\"qwerty\""),
        "base is not oa-default or none"
    );
    skips_one(
        key_entry("grid-left-hand", base_only + ",\"keyboard\":\"dvorak\""),
        "keyboard is not any, ansi, iso or jis"
    );
    skips_one(
        key_entry("grid-left-hand", base_only + ",\"pointer\":\"middle\""),
        "pointer is not right-click or left-click"
    );
    skips_one(
        key_entry("grid-left-hand", base_only + ",\"helpers\":\"toggle\""), "helpers is not a list"
    );
    skips_one(
        key_entry(
            "grid-left-hand",
            base_only + ",\"helpers\":[\"toggle\",\"sticky-modifiers\",\"hold-to-confirm\","
                        "\"pointer-keys\",\"radial-orders\",\"edge-scroll\",\"toggle\"]"
        ),
        "helpers lists more than 6"
    );
    skips_one(
        key_entry("grid-left-hand", base_only + ",\"helpers\":[\"toggle\",\"toggle\"]"),
        "helpers[1] is repeated"
    );
    skips_one(
        key_entry("grid-left-hand", base_only + ",\"helpers\":[1]"), "helpers[0] is not text"
    );
    skips_one(
        key_entry("grid-left-hand", base_only + ",\"triggers\":\"Q\""), "triggers is not a list"
    );
    std::string many = base_only + ",\"triggers\":[";
    for (int index = 0; index < 129; ++index) {
        if (index != 0)
            many += ',';
        many += "\"Q\"";
    }
    many += ']';
    skips_one(key_entry("grid-left-hand", many), "triggers lists more than 128");
    skips_one(
        key_entry("grid-left-hand", base_only + ",\"triggers\":[\"Q\",\"Nope\",\"Q\"]"),
        "triggers[2] is repeated"
    );
    skips_one(
        key_entry("grid-left-hand", base_only + ",\"triggers\":[\"Q\",1]"),
        "triggers[1] is not text"
    );
    skips_one(
        key_entry("grid-left-hand", base_only + ",\"preview\":\"/v1/i/preview.jpg\""),
        "preview is not a picture reference"
    );
    skips_one(
        key_entry("grid-left-hand", base_only + ",\"changes\":-1"),
        "changes is not a whole number from 0 to 2048"
    );
    skips_one(
        key_entry("grid-left-hand", base_only + ",\"changes\":2049"),
        "changes is not a whole number from 0 to 2048"
    );
    skips_one(
        key_entry("grid-left-hand", base_only + ",\"changes\":\"eleven\""),
        "changes is not a whole number from 0 to 2048"
    );
    skips_one(key_entry("grid-left-hand", "\"base\":\"oa-default\""), "version is missing");
    skips_one(
        key_entry("grid-left-hand", "\"version\":\"1.2.3.4\",\"base\":\"oa-default\""),
        "version is not a mutator version"
    );
    skips_one(
        key_entry("grid-left-hand", base_only + ",\"works_with\":\"*\""), "works_with is not a list"
    );

    const auto untouched = read_ok(catalogue_of(
        bare(
            "oamod",
            "ridge",
            "\"base\":\"oa-default\",\"keyboard\":\"iso\",\"pointer\":\"left-click\","
            "\"helpers\":[\"toggle\"],\"changes\":11,\"triggers\":[\"Q\"],"
            "\"preview\":\"/v1/i/preview.png\""
        ) +
        "," + bare("oamut", "steady-hands", "\"base\":\"none\",\"changes\":\"lots\"")
    ));
    OA_CHECK(untouched.problems.empty());
    OA_CHECK(untouched.packages.size() == 2);
    if (untouched.packages.size() == 2) {
        OA_CHECK(untouched.packages[0].kind == catalogue::Kind::oamod);
        OA_CHECK(untouched.packages[0].key_base.empty());
        OA_CHECK(untouched.packages[0].keyboard.empty());
        OA_CHECK(untouched.packages[0].changes == 0);
        OA_CHECK(untouched.packages[0].helpers.empty());
        OA_CHECK(untouched.packages[0].triggers.empty());
        OA_CHECK(untouched.packages[0].preview.empty());
        OA_CHECK(untouched.packages[1].kind == catalogue::Kind::oamut);
        OA_CHECK(untouched.packages[1].key_base.empty());
        OA_CHECK(untouched.packages[1].changes == 0);
    }

    std::string wrong_file = key_entry("grid-left-hand", base_only);
    const auto suffix = wrong_file.rfind(".oakeys");
    OA_CHECK(suffix != std::string::npos);
    if (suffix != std::string::npos)
        wrong_file.replace(suffix, 7, ".oamod");
    skips_one(wrong_file, "file not named");
    skips_one("{\"kind\":\"oafuture\",\"id\":\"grid-left-hand\"}", "unknown kind");

    const auto duplicate = read_ok(catalogue_of(
        key_entry("ridge", base_only) + "," + bare("oamod", "ridge") + "," + bare("oamod", "keeper")
    ));
    OA_CHECK(duplicate.packages.size() == 1);
    if (!duplicate.packages.empty())
        OA_CHECK(duplicate.packages[0].id == "keeper");
    OA_CHECK(duplicate.problems.size() == 2);
    if (duplicate.problems.size() == 2) {
        OA_CHECK(duplicate.problems[0].find("same key") != std::string::npos);
        OA_CHECK(duplicate.problems[0].find("ridge") != std::string::npos);
    }
}

} // namespace

int main() {
    test_sample();
    test_registry_icon();
    test_skipped_entries();
    test_language_key_and_fields();
    test_coverage_and_bounds();
    test_structure();
    test_utc_time();
    test_published();
    test_content_needs();
    test_expansion();
    test_mutator();
    test_brain();
    test_oakeys();
    return oa::test::check_exit_status();
}
