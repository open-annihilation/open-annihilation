// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Reads the OA Registry Specification examples with the engine's readers.
// The examples directory is the program's argument, so a scratch copy can be
// pointed at. The catalogue signature is the RFC 8032 section 7.1 TEST 1
// vector: signing the catalogue bytes again must match the committed file.

#include "oa/app/content/download_api.hpp"
#include "oa/base/sha256.hpp"
#include "oa/base/signing/ed25519.hpp"
#include "oa/data/catalogue/catalogue.hpp"
#include "oa/data/catalogue/check.hpp"
#include "oa/data/registry/descriptor.hpp"
#include "oa/data/registry/player_registries.hpp"
#include "oa/data/registry/trust.hpp"
#include "oa/formats/json.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace catalogue = oa::data::catalogue;
namespace content = oa::app::content;
namespace json = oa::formats::json;
namespace registry = oa::data::registry;
namespace sha = oa::base::sha256;
namespace signing = oa::base::signing;

/// The published public key of RFC 8032 section 7.1 TEST 1.
constexpr std::array<uint8_t, 32> kRfc8032Test1Public = {
    0xd7, 0x5a, 0x98, 0x01, 0x82, 0xb1, 0x0a, 0xb7, 0xd5, 0x4b, 0xfe, 0xd3, 0xc9, 0x64, 0x07, 0x3a,
    0x0e, 0xe1, 0x72, 0xf3, 0xda, 0xa6, 0x23, 0x25, 0xaf, 0x02, 0x1a, 0x68, 0xf7, 0x07, 0x51, 0x1a,
};

std::optional<std::vector<uint8_t>> read_bytes(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in)
        return std::nullopt;
    return std::vector<uint8_t>(
        std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()
    );
}

std::string as_text(const std::vector<uint8_t>& bytes) {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

bool same_json(const json::Json& left, const json::Json& right) {
    if (left.type() != right.type())
        return false;
    switch (left.type()) {
    case json::JsonType::null:
        return true;
    case json::JsonType::boolean:
        return left.boolean() == right.boolean();
    case json::JsonType::number:
        return left.number_text() && right.number_text() &&
               *left.number_text() == *right.number_text();
    case json::JsonType::string:
        return left.string() && right.string() && *left.string() == *right.string();
    case json::JsonType::array: {
        const auto left_elements = left.elements();
        const auto right_elements = right.elements();
        if (left_elements.size() != right_elements.size())
            return false;
        for (std::size_t index = 0; index < left_elements.size(); ++index) {
            if (!same_json(left_elements[index], right_elements[index]))
                return false;
        }
        return true;
    }
    case json::JsonType::object: {
        const auto left_names = left.names();
        const auto right_names = right.names();
        const auto left_values = left.values();
        const auto right_values = right.values();
        if (left_names.size() != right_names.size())
            return false;
        for (std::size_t index = 0; index < left_names.size(); ++index) {
            if (left_names[index] != right_names[index])
                return false;
            if (!same_json(left_values[index], right_values[index]))
                return false;
        }
        return true;
    }
    }
    return false;
}

std::optional<json::Json> parse_bytes(const std::vector<uint8_t>& bytes, const char* name) {
    json::JsonError error;
    auto value = json::parse_json(as_text(bytes), error);
    if (!value)
        std::fprintf(stderr, "%s: %s\n", name, error.message.c_str());
    return value;
}

void check_descriptor_file(const std::filesystem::path& path) {
    const auto bytes = read_bytes(path);
    OA_CHECK(bytes.has_value());
    if (!bytes)
        return;
    registry::Descriptor descriptor;
    std::string error;
    if (!registry::read_descriptor(*bytes, descriptor, &error))
        std::fprintf(stderr, "%s: %s\n", path.filename().string().c_str(), error.c_str());
    OA_CHECK(registry::read_descriptor(*bytes, descriptor, &error));
    OA_CHECK(descriptor.is_signed());
    OA_CHECK(descriptor.keys.size() == 1);
    if (descriptor.keys.size() == 1) {
        OA_CHECK(descriptor.keys[0].id == "rfc8032-test-1");
        OA_CHECK(descriptor.keys[0].key == kRfc8032Test1Public);
    }
    const registry::AddContext context;
    const registry::Refusal refusal = registry::check_new_registry(descriptor, context);
    if (refusal != registry::Refusal::none)
        std::fprintf(
            stderr, "%s: %s\n", path.filename().string().c_str(), registry::refusal_text(refusal)
        );
    OA_CHECK(refusal == registry::Refusal::none);
}

void check_descriptors(const std::filesystem::path& examples) {
    check_descriptor_file(examples / "registry.yaml");
    check_descriptor_file(examples / "example.oareg");
}

void check_player_registries(const std::filesystem::path& examples) {
    const auto bytes = read_bytes(examples / "Registries.yaml");
    OA_CHECK(bytes.has_value());
    if (!bytes)
        return;
    registry::PlayerRegistries registries;
    std::string error;
    if (!registry::read_player_registries(*bytes, registries, &error))
        std::fprintf(stderr, "Registries.yaml: %s\n", error.c_str());
    OA_CHECK(registry::read_player_registries(*bytes, registries, &error));
    OA_CHECK(registries.registries.size() == 1);
    OA_CHECK(registries.disabled.size() == 1);
    if (registries.registries.size() != 1 || registries.disabled.size() != 1)
        return;
    const registry::AddedRegistry& added = registries.registries[0];
    OA_CHECK(added.descriptor.id == "example-maps");
    OA_CHECK(added.enabled);
    OA_CHECK(added.descriptor.keys.size() == 1);
    if (added.descriptor.keys.size() == 1)
        OA_CHECK(added.descriptor.keys[0].key == kRfc8032Test1Public);
    OA_CHECK(registries.disabled[0] == "sample");
}

void check_catalogue(const std::filesystem::path& examples) {
    const auto bytes = read_bytes(examples / "catalogue.json");
    const auto signature_bytes = read_bytes(examples / "catalogue.json.sig");
    OA_CHECK(bytes.has_value());
    OA_CHECK(signature_bytes.has_value());
    if (!bytes || !signature_bytes)
        return;

    catalogue::Catalogue loaded;
    std::string error;
    if (!catalogue::read_catalogue(*bytes, loaded, &error))
        std::fprintf(stderr, "catalogue.json: %s\n", error.c_str());
    OA_CHECK(catalogue::read_catalogue(*bytes, loaded, &error));
    for (const std::string& problem : loaded.problems)
        std::fprintf(stderr, "%s\n", problem.c_str());
    OA_CHECK(loaded.problems.empty());
    OA_CHECK(loaded.registry == "example-maps");
    OA_CHECK(
        loaded.icon == "/v1/i/cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc.png"
    );
    OA_CHECK(loaded.packages.size() == 6);
    if (loaded.packages.size() == 6) {
        OA_CHECK(loaded.packages[0].kind == catalogue::Kind::oamod);
        OA_CHECK(loaded.packages[0].id == "example-mod");
        OA_CHECK(loaded.packages[1].kind == catalogue::Kind::oamap);
        OA_CHECK(loaded.packages[1].id == "example-maps");
        OA_CHECK(loaded.packages[2].kind == catalogue::Kind::oalang);
        OA_CHECK(loaded.packages[2].id == "xx-Test");
        const catalogue::Package& mutator = loaded.packages[3];
        OA_CHECK(mutator.kind == catalogue::Kind::oamut);
        OA_CHECK(mutator.id == "steady-hands");
        OA_CHECK(mutator.version == "1.2");
        OA_CHECK(mutator.works_with == std::vector<std::string>({"ta-3.1c", "ridge >= 1.0"}));
        OA_CHECK(mutator.requires_mutators.size() == 1);
        if (!mutator.requires_mutators.empty()) {
            OA_CHECK(mutator.requires_mutators[0].id == "calm-seas");
            OA_CHECK(mutator.requires_mutators[0].range_text == ">= 1.0");
        }
        OA_CHECK(mutator.conflicts_with.size() == 1);
        if (!mutator.conflicts_with.empty())
            OA_CHECK(mutator.conflicts_with[0].id == "shaky-grip");
        OA_CHECK(mutator.layers.size() == 2);
        if (mutator.layers.size() == 2) {
            OA_CHECK(mutator.layers[0] == oa::formats::oamut::Layer::units);
            OA_CHECK(mutator.layers[1] == oa::formats::oamut::Layer::weapons);
        }
        OA_CHECK(mutator.options.size() == 2);
        if (mutator.options.size() == 2) {
            OA_CHECK(mutator.options[0].id == "grip");
            OA_CHECK(mutator.options[0].value_labels == std::vector<std::string>({"Soft", "Firm"}));
            OA_CHECK(mutator.options[1].id == "sway");
            OA_CHECK(mutator.options[1].minimum == "0");
            OA_CHECK(mutator.options[1].maximum == "2");
            OA_CHECK(mutator.options[1].step == "0.5");
        }
        OA_CHECK(
            mutator.badge ==
            "/v1/i/ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff.png"
        );
        OA_CHECK(mutator.tags == std::vector<std::string>({"aim"}));
        const catalogue::Package& brain = loaded.packages[4];
        OA_CHECK(brain.kind == catalogue::Kind::oabrain);
        OA_CHECK(brain.id == "steady-commander");
        OA_CHECK(brain.version == "1.2");
        OA_CHECK(!brain.full_awareness);
        OA_CHECK(brain.network_bytes == 2100000);
        OA_CHECK(brain.works_with == std::vector<std::string>({"ta-3.1c", "ridge >= 28"}));
        OA_CHECK(brain.required_expansions == std::vector<std::string>({"core-contingency"}));
        OA_CHECK(brain.levels.size() == 2);
        if (brain.levels.size() == 2) {
            OA_CHECK(brain.levels[0].id == "careful");
            OA_CHECK(brain.levels[0].name == "Careful");
            OA_CHECK(brain.levels[0].difficulty == oa::formats::oabrain::Difficulty::easy);
            OA_CHECK(brain.levels[1].id == "steady");
            OA_CHECK(brain.levels[1].name == "Steady");
            OA_CHECK(brain.levels[1].difficulty == oa::formats::oabrain::Difficulty::medium);
        }
        OA_CHECK(brain.options.size() == 1);
        if (!brain.options.empty()) {
            OA_CHECK(brain.options[0].id == "teach");
            OA_CHECK(brain.options[0].label == "Teaching");
            OA_CHECK(brain.options[0].type == oa::formats::oamut::OptionType::toggle);
            OA_CHECK(brain.options[0].default_text == "true");
        }
        OA_CHECK(brain.tags == std::vector<std::string>({"economy", "teaching"}));
        OA_CHECK(
            brain.badge ==
            "/v1/i/8888888888888888888888888888888888888888888888888888888888888888.png"
        );
        const catalogue::Package& layout = loaded.packages[5];
        OA_CHECK(layout.kind == catalogue::Kind::oakeys);
        OA_CHECK(layout.id == "grid-left-hand");
        OA_CHECK(layout.version == "1.0");
        OA_CHECK(layout.key_base == "oa-default");
        OA_CHECK(layout.keyboard == "any");
        OA_CHECK(layout.pointer == "right-click");
        OA_CHECK(layout.changes == 11);
        OA_CHECK(layout.helpers == std::vector<std::string>({"toggle", "sticky-modifiers"}));
        OA_CHECK(
            layout.triggers == std::vector<std::string>({
                                   "@KeyQ",
                                   "@KeyW",
                                   "@KeyE",
                                   "@KeyR",
                                   "@KeyS",
                                   "@KeyG",
                                   "@KeyZ",
                                   "@KeyX",
                                   "@KeyC",
                                   "@KeyV",
                                   "@KeyB",
                               })
        );
        OA_CHECK(layout.works_with == std::vector<std::string>({"ta-3.1c", "ridge >= 28"}));
        OA_CHECK(layout.tags == std::vector<std::string>({"grid", "left-hand"}));
        OA_CHECK(
            layout.badge ==
            "/v1/i/1313131313131313131313131313131313131313131313131313131313131313.png"
        );
        OA_CHECK(
            layout.preview ==
            "/v1/i/1414141414141414141414141414141414141414141414141414141414141414.png"
        );
        OA_CHECK(
            layout.file.size() >= 7 &&
            layout.file.compare(layout.file.size() - 7, 7, ".oakeys") == 0
        );
    }

    // RFC 8032 section 7.1 TEST 1. The seed is the published test vector.
    std::array<uint8_t, 32> seed = {
        0x9d, 0x61, 0xb1, 0x9d, 0xef, 0xfd, 0x5a, 0x60, 0xba, 0x84, 0x4a,
        0xf4, 0x92, 0xec, 0x2c, 0xc4, 0x44, 0x49, 0xc5, 0x69, 0x7b, 0x32,
        0x69, 0x19, 0x70, 0x3b, 0xac, 0x03, 0x1c, 0xae, 0x7f, 0x60,
    };
    signing::SecretKey secret_key{};
    signing::PublicKey public_key{};
    signing::key_pair_from_seed(seed, secret_key, public_key);
    seed.fill(0);
    OA_CHECK(public_key == kRfc8032Test1Public);
    const std::string public_text(signing::public_key_text(public_key).view());
    OA_CHECK(public_text == "ed25519:11qYAYKxCrfVS/7TyWQHOg7hcvPapiMlrwIaaPcHURo=");

    const signing::Signature signature = signing::sign(secret_key, *bytes);
    secret_key.fill(0);
    OA_CHECK(signing::verify(public_key, *bytes, signature));
    const catalogue::SignatureLine line{"rfc8032-test-1", signature};
    const std::string resigned = catalogue::signature_file_text(line);
    const std::string committed = as_text(*signature_bytes);
    if (resigned != committed)
        std::fprintf(
            stderr, "resigned signature differs from catalogue.json.sig\n%s", resigned.c_str()
        );
    OA_CHECK(resigned == committed);

    std::string signature_error;
    const auto parsed = catalogue::parse_signature_file(*signature_bytes, &signature_error);
    if (!parsed)
        std::fprintf(stderr, "catalogue.json.sig: %s\n", signature_error.c_str());
    OA_CHECK(parsed.has_value());
    if (parsed) {
        OA_CHECK(parsed->key_id == "rfc8032-test-1");
        OA_CHECK(signing::verify(kRfc8032Test1Public, *bytes, parsed->signature));
    }
}

const json::Json* member(const json::Json& object, std::string_view name) {
    return object.find(name);
}

std::optional<content::DownloadReason> reason_of(std::string_view text) {
    if (text == "install")
        return content::DownloadReason::install;
    if (text == "update")
        return content::DownloadReason::update;
    if (text == "repair")
        return content::DownloadReason::repair;
    if (text == "offered-in-lobby")
        return content::DownloadReason::offered_in_lobby;
    if (text == "mirror")
        return content::DownloadReason::mirror;
    return std::nullopt;
}

std::optional<catalogue::Kind> kind_of(std::string_view text) {
    if (text == "oamod")
        return catalogue::Kind::oamod;
    if (text == "oamap")
        return catalogue::Kind::oamap;
    if (text == "oalang")
        return catalogue::Kind::oalang;
    return std::nullopt;
}

void check_key_request(const json::Json& example) {
    content::KeyRequest request;
    const json::Json* install = member(example, "install");
    const json::Json* package = member(example, "package");
    const json::Json* kind = member(example, "kind");
    const json::Json* release = member(example, "release");
    const json::Json* hash = member(example, "sha256");
    const json::Json* engine = member(example, "engine");
    const json::Json* platform = member(example, "platform");
    const json::Json* arch = member(example, "arch");
    const json::Json* language = member(example, "language");
    const json::Json* reason = member(example, "reason");
    OA_CHECK(install && install->string());
    OA_CHECK(package && package->string());
    OA_CHECK(kind && kind->string());
    OA_CHECK(release && release->integer());
    OA_CHECK(hash && hash->string());
    OA_CHECK(engine && engine->string());
    OA_CHECK(platform && platform->string());
    OA_CHECK(arch && arch->string());
    OA_CHECK(language && language->string());
    OA_CHECK(reason && reason->string());
    if (!install || !install->string() || !package || !package->string() || !kind ||
        !kind->string() || !release || !release->integer() || !hash || !hash->string() || !engine ||
        !engine->string() || !platform || !platform->string() || !arch || !arch->string() ||
        !language || !language->string() || !reason || !reason->string())
        return;
    request.install = *install->string();
    request.package = *package->string();
    const auto kind_value = kind_of(*kind->string());
    const auto digest = sha::parse_hex(*hash->string());
    const auto reason_value = reason_of(*reason->string());
    OA_CHECK(kind_value.has_value());
    OA_CHECK(digest.has_value());
    OA_CHECK(reason_value.has_value());
    if (!kind_value || !digest || !reason_value)
        return;
    request.kind = *kind_value;
    request.release = *release->integer();
    request.sha256 = *digest;
    request.engine = *engine->string();
    request.platform = *platform->string();
    request.arch = *arch->string();
    request.language = *language->string();
    request.reason = *reason_value;

    json::JsonError error;
    const auto written = json::parse_json(content::key_request_body(request), error);
    if (!written)
        std::fprintf(stderr, "key request: %s\n", error.message.c_str());
    OA_CHECK(written.has_value());
    if (written)
        OA_CHECK(same_json(example, *written));
}

void check_result(const json::Json& example) {
    const json::Json* result = member(example, "result");
    const json::Json* bytes = member(example, "bytes");
    const json::Json* seconds = member(example, "seconds");
    OA_CHECK(result && result->string() && *result->string() == "installed");
    OA_CHECK(bytes && bytes->integer());
    OA_CHECK(seconds && seconds->integer());
    if (!bytes || !bytes->integer() || !seconds || !seconds->integer())
        return;
    json::JsonError error;
    const auto written = json::parse_json(
        content::result_body(
            content::KeyResult::installed,
            static_cast<uint64_t>(*bytes->integer()),
            *seconds->integer()
        ),
        error
    );
    OA_CHECK(written.has_value());
    if (written)
        OA_CHECK(same_json(example, *written));
}

void check_api(const std::filesystem::path& examples) {
    const char* names[] = {
        "download-request.json",
        "download-response.json",
        "challenge.json",
        "challenge-state.json",
        "result.json",
        "error.json",
        "counts.json",
    };
    std::optional<json::Json> parsed[7];
    for (std::size_t index = 0; index < 7; ++index) {
        const auto bytes = read_bytes(examples / names[index]);
        OA_CHECK(bytes.has_value());
        if (!bytes)
            return;
        parsed[index] = parse_bytes(*bytes, names[index]);
        OA_CHECK(parsed[index].has_value());
        if (!parsed[index])
            return;
    }
    check_key_request(*parsed[0]);
    check_result(*parsed[4]);

    const auto response = read_bytes(examples / "download-response.json");
    const auto challenge = read_bytes(examples / "challenge.json");
    const auto state = read_bytes(examples / "challenge-state.json");
    const auto error_body = read_bytes(examples / "error.json");
    OA_CHECK(response && challenge && state && error_body);
    if (!response || !challenge || !state || !error_body)
        return;

    std::string why;
    const auto issued = content::read_issued_key(*response, &why);
    if (!issued)
        std::fprintf(stderr, "download-response.json: %s\n", why.c_str());
    OA_CHECK(issued.has_value());
    if (issued) {
        OA_CHECK(issued->download == "d_ExampleKey000000000000");
        OA_CHECK(issued->expires > 0);
        OA_CHECK(issued->url.host == "maps.example.org");
    }

    why.clear();
    const auto check = content::read_challenge(*challenge, &why);
    if (!check)
        std::fprintf(stderr, "challenge.json: %s\n", why.c_str());
    OA_CHECK(check.has_value());
    if (check) {
        OA_CHECK(check->code == "K7NP-2M4Q");
        OA_CHECK(check->poll_seconds == 3);
        OA_CHECK(check->verify.scheme == oa::formats::url::Scheme::https);
    }

    why.clear();
    const auto polled = content::read_challenge_state(*state, &why);
    if (!polled)
        std::fprintf(stderr, "challenge-state.json: %s\n", why.c_str());
    OA_CHECK(polled == content::ChallengeState::pending);

    const content::ApiError api_error = content::read_api_error(409, *error_body, nullptr);
    OA_CHECK(api_error.error == "release_mismatch");
    OA_CHECK(api_error.message == "That release is not the one this catalogue offers.");
    OA_CHECK(api_error.status == 409);
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: registry-spec-examples EXAMPLES_DIR\n");
        return 1;
    }
    const std::filesystem::path examples(argv[1]);
    check_descriptors(examples);
    check_player_registries(examples);
    check_catalogue(examples);
    check_api(examples);
    return oa::test::check_exit_status();
}
